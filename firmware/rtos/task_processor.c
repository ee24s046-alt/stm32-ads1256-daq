/**
 * @file task_processor.c
 * @brief Stage 2: apply calibration, flag over-range, encode the wire packet.
 *
 * Runs at priority 3, below the sampler.  It drains in batches rather than one
 * scan per wake: waking per scan would mean 7500 context switches per second
 * (~2.3 % of the CPU in switch overhead alone) to do 11 us of work each time.
 * Draining until empty amortises the switch across whatever has accumulated,
 * and because it is strictly lower priority than the sampler, a long drain can
 * never delay acquisition.
 *
 * Note what is *not* done here: no filtering, no decimation, no resampling.
 * The device's contract is calibrated raw samples with honest timestamps.
 * Anything that throws information away belongs on the host, where it can be
 * changed without reflashing and where the discarded data still exists.
 */
#include "shared.h"
#include "packet.h"
#include "cycle_counter.h"

void task_processor(void *arg)
{
    daq_raw_scan_t scan;
    uint8_t wire[DAQ_PACKET_BYTES];

    (void)arg;

    for (;;) {
        bool did_work = false;

        while (spsc_pop(&g_ring_raw, &scan)) {
            const uint32_t t0 = cyc_now();
            did_work = true;

            if (g_streaming) {
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
            }

            g_stats.scans_processed++;

            {
                const uint32_t elapsed = cyc_since(t0);
                if (elapsed > g_stats.processor_service_max_cyc) {
                    g_stats.processor_service_max_cyc = elapsed;
                }
            }
        }

        (void)did_work;

        /*
         * One drain pass per 1 ms tick.  At 7.5 kSPS that is ~7.5 scans per
         * pass against a 256-slot ring, i.e. 34x margin, so the batch size is
         * bounded and the ring can never approach full through scheduling
         * alone.  Polling on the tick rather than notifying per scan trades
         * up to 1 ms of added pipeline latency - irrelevant next to USB's own
         * 1 ms frame quantisation - for 7500 fewer context switches per
         * second.  The timestamp in each packet was taken in the ISR, so this
         * latency does not affect the data, only when it appears on the wire.
         */
        vTaskDelay(1U);
    }
}
