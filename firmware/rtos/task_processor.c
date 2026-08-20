/**
 * @file task_processor.c
 * @brief Stage 2: decimate (wideband), calibrate, flag over-range, encode.
 *
 * Runs at priority 3, below the sampler.  It drains in batches rather than one
 * scan per wake: waking per scan would mean up to 30000 context switches per
 * second in wideband mode to do ~11 us of work each time.  Draining until empty
 * amortises the switch across whatever has accumulated, and because it is
 * strictly lower priority than the sampler, a long drain can never delay
 * acquisition.
 *
 * The decimator, and why it is load-bearing
 * -----------------------------------------
 * In DAQ_MODE_WIDEBAND the sampler produces 30000 samples/s.  Streaming those
 * raw would be 30000 x 24 B = 720 kB/s, which is most of what USB full speed
 * can actually sustain and leaves no headroom for the host to misbehave.  The
 * M=4 polyphase decimator takes it to 7500 samples/s = 180 kB/s, which is
 * comfortable.
 *
 * So the anti-alias filter is not an ornament bolted onto the data path - it is
 * what makes the wideband mode fit on the link at all.  And because it is a
 * real filter rather than "keep every 4th sample", anything between 3.75 kHz
 * and 15 kHz is attenuated by at least 55.6 dB instead of being folded into the
 * output band where nothing could distinguish it from signal.  The naive
 * version costs the same bandwidth and silently corrupts the data.
 *
 * Note what is still *not* done here: no resampling, no detrending, no
 * smoothing.  Decimation is lossless with respect to the band being kept; the
 * device's contract remains calibrated samples with honest timestamps.
 */
#include "shared.h"
#include "packet.h"
#include "cycle_counter.h"

static fir_decim_t s_decim;

void task_processor(void *arg)
{
    daq_raw_scan_t scan;
    uint8_t wire[DAQ_PACKET_BYTES];
    daq_mode_t last_mode = g_mode;

    (void)arg;
    fir_decim_reset(&s_decim);

    for (;;) {
        const daq_mode_t mode = g_mode;

        if (mode != last_mode) {
            /* The delay line holds samples taken at the other mode's rate.
               Carrying them across would emit a handful of outputs filtered by
               a filter designed for a different Nyquist. */
            fir_decim_reset(&s_decim);
            last_mode = mode;
        }

        while (spsc_pop(&g_ring_raw, &scan)) {
            const uint32_t t0 = cyc_now();
            bool emit = true;

            g_stats.scans_processed++;

            if (mode == DAQ_MODE_WIDEBAND) {
                int32_t decimated;

                /* One input sample in; an output every 4th call. */
                if (!fir_decim_push(&s_decim, scan.code[0], &decimated)) {
                    emit = false;
                } else if (!fir_decim_ready(&s_decim)) {
                    /* Still inside the filter's startup transient - these
                       outputs are contaminated by the zeroed delay line and
                       would show up as a spurious step at the start of every
                       capture. */
                    emit = false;
                } else {
                    scan.code[0] = decimated;
                    g_stats.decim_out++;
                }
            }

            if (emit && g_streaming) {
                for (uint32_t c = 0U; c < DAQ_NUM_CHANNELS; c++) {
                    if (daq_cal_is_overrange(scan.code[c])) {
                        scan.status |= DAQ_STATUS_OVERRANGE;
                    }
                    scan.code[c] = daq_cal_apply(&g_calibration, c, scan.code[c]);
                }
                if (g_calibration_from_flash) {
                    scan.status |= DAQ_STATUS_CAL_APPLIED;
                }

                daq_packet_encode(&scan, wire);

                if (!spsc_push(&g_ring_tx, wire)) {
                    /* The host is not draining.  Dropping the newest packet is
                       the right policy for a live stream: the alternative,
                       dropping the oldest, breaks the sequence-number
                       continuity check the host uses to prove nothing was
                       lost, and silently rewrites history. */
                    scan.status |= DAQ_STATUS_TX_OVF;
                }

                /* Hand the same calibrated sample to the metrics task. A full
                   DSP ring is not an error: metrics are a convenience and must
                   never cost a sample on the wire. */
                (void)spsc_push(&g_ring_dsp, &scan);
            }

            {
                const uint32_t elapsed = cyc_since(t0);
                if (elapsed > g_stats.processor_service_max_cyc) {
                    g_stats.processor_service_max_cyc = elapsed;
                }
            }
        }

        /*
         * One drain pass per 1 ms tick.  In muxed mode that is ~1.9 scans per
         * pass against a 256-slot ring; in wideband it is ~30, still 8x of
         * margin.  Polling on the tick rather than notifying per sample trades
         * up to 1 ms of added pipeline latency - irrelevant next to USB's own
         * 1 ms frame quantisation - for up to 30000 fewer context switches per
         * second.  The timestamp in each packet was taken in the ISR, so this
         * latency does not affect the data, only when it appears on the wire.
         */
        vTaskDelay(1U);
    }
}
