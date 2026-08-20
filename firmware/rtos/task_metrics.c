/**
 * @file task_metrics.c
 * @brief On-device RMS, averaged PSD and THD+N, streamed alongside the samples.
 *
 * Priority 1 - the lowest thing in the system above idle, and deliberately so.
 * A 1024-point FFT takes about 310 us; nothing with a deadline may ever wait on
 * that.  Sitting below the uplink means a PSD block can be preempted mid-
 * transform by the USB drain and nothing is lost but a few microseconds.
 *
 * This task is also the only FPU user in the firmware.  That is what makes the
 * float FFT affordable: FreeRTOS's Cortex-M4F port stacks FPU context lazily,
 * and with a single user the save is never actually triggered on a context
 * switch that matters.  Had the metrics been folded into the processor task -
 * which looks tempting, since it already has the samples - every preemption of
 * the processor by the sampler would carry 33 extra words of FPU state on a
 * path with microseconds of margin.
 *
 * What it produces
 * ----------------
 *   metrics record, 64 B, every 100 ms  - per-channel RMS, THD+N, fundamental
 *   PSD record, 1044 B, every ~478 ms   - one channel's averaged spectrum
 *
 * Together about 1.3 kB/s on a link already carrying 45-180 kB/s of samples:
 * under 3 % overhead for a live view of the noise floor that would otherwise
 * require capturing to disk and post-processing.
 */
#include "shared.h"
#include "packet.h"
#include "cycle_counter.h"
#include "ads1256.h"

/*
 * Staging for the record being built.  Static rather than automatic: a PSD
 * record is 1044 bytes and this task's stack is 512 words (2 KB). Building it
 * on the stack would leave 1 KB for everything else, including the FFT's own
 * frame - too close to the edge to be comfortable, and a stack overflow here
 * halts the device.
 */
static uint8_t s_record[DAQ_MAX_RECORD_BYTES];

static void emit_metrics_record(uint16_t seq)
{
    daq_metrics_rec_t rec;

    for (uint32_t c = 0U; c < DAQ_NUM_CHANNELS; c++) {
        rec.rms[c]      = g_metrics.rms_valid[c] ? g_metrics.rms[c] : 0;
        rec.thdn_mdb[c] = g_metrics.thdn_mdb[c];
        rec.fund_mhz[c] = g_metrics.fund_mhz[c];
    }
    rec.psd_serial = g_metrics.psd_serial;
    rec.t_us       = micros();
    rec.seq        = seq;
    rec.mode       = (uint8_t)g_mode;

    daq_metrics_encode(&rec, s_record);

    if (spsc_push(&g_ring_meta, s_record)) {
        g_stats.meta_sent++;
    } else {
        g_stats.meta_dropped++;
    }
}

static void emit_psd_record(uint16_t seq, float fs_hz)
{
    daq_psd_rec_t rec;

    rec.channel = (uint8_t)g_metrics.psd_ready_channel;
    rec.seq     = seq;
    rec.t_us    = micros();
    rec.fs_mhz  = (uint32_t)(fs_hz * 1000.0f);

    for (uint32_t k = 0U; k < DAQ_PSD_BINS; k++) {
        float db = g_metrics.psd_dbfs[k] * 100.0f;

        /* Clamp into int16 rather than letting the cast wrap: a -400 dB bin
           (an exact zero somewhere) would otherwise come out as a large
           positive value and draw a spike where there is silence. */
        if (db > 32767.0f) {
            db = 32767.0f;
        } else if (db < -32768.0f) {
            db = -32768.0f;
        } else {
            /* in range */
        }
        rec.bin_dbfs_x100[k] = (int16_t)db;
    }

    daq_psd_encode(&rec, s_record);

    if (spsc_push(&g_ring_meta, s_record)) {
        g_stats.meta_sent++;
        g_stats.psd_blocks++;
    } else {
        g_stats.meta_dropped++;
    }
}

void task_metrics(void *arg)
{
    daq_raw_scan_t scan;
    uint16_t seq = 0U;
    TickType_t last_emit = 0U;
    daq_mode_t last_mode = g_mode;

    (void)arg;

    metrics_init(&g_metrics, (float)DAQ_SCAN_RATE_SPS);

    for (;;) {
        const daq_mode_t mode = g_mode;

        if (mode != last_mode) {
            /*
             * The sample rate the analysis assumes has just changed, and every
             * frequency axis derives from it. Re-initialising rather than
             * carrying state over means the first block after a mode change is
             * delayed, which is the correct trade: a PSD labelled with the
             * wrong fs is worse than no PSD.
             */
            metrics_init(&g_metrics,
                         (mode == DAQ_MODE_WIDEBAND)
                             ? (float)DAQ_WIDEBAND_OUT_SPS
                             : (float)DAQ_SCAN_RATE_SPS);
            last_mode = mode;
        }

        while (spsc_pop(&g_ring_dsp, &scan)) {
            const uint32_t t0 = cyc_now();

            if (mode == DAQ_MODE_WIDEBAND) {
                (void)metrics_push(&g_metrics, 0U, scan.code[0]);
            } else {
                for (uint32_t c = 0U; c < DAQ_NUM_CHANNELS; c++) {
                    (void)metrics_push(&g_metrics, c, scan.code[c]);
                }
            }

            if (metrics_take_psd(&g_metrics)) {
                emit_psd_record(seq, g_metrics.fs_hz);
                seq++;

                /* Only the sample that completed a transform is timed; the
                   other 1023 are a handful of cycles each and averaging them
                   in would hide the figure that matters. */
                const uint32_t elapsed = cyc_since(t0);
                if (elapsed > g_stats.psd_service_max_cyc) {
                    g_stats.psd_service_max_cyc = elapsed;
                }
            }
        }

        /* Metrics record every 100 ms, matching the RMS integration window. */
        if ((xTaskGetTickCount() - last_emit) >= pdMS_TO_TICKS(100U)) {
            last_emit = xTaskGetTickCount();
            emit_metrics_record(seq);
            seq++;
        }

        vTaskDelay(pdMS_TO_TICKS(5U));
    }
}
