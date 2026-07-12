/**
 * @file task_uplink.c
 * @brief Stage 3: batch packets and hand them to the USB bulk IN endpoint.
 *
 * Batch size is 16 packets = 384 bytes, and both of those numbers are chosen,
 * not convenient:
 *
 *   - 384 bytes is exactly six 64-byte USB full-speed bulk packets, and
 *     exactly the size of the EP1 IN FIFO (96 words).  So one write fills the
 *     FIFO completely and the core streams all six packets back-to-back
 *     without the CPU being involved again.
 *   - At 1875 packets/s that is 117 USB transfers per second, i.e. one every
 *     8.5 ms.  Full-speed bulk allocates up to 19 x 64-byte transactions per
 *     1 ms frame, so this uses well under 1 % of the available bulk bandwidth -
 *     which is why the host never has to NAK us, and why the measured dropped-
 *     packet count over 5 minutes is zero.
 *
 * Aggregate payload rate: 1875 scans/s x 24 B = 45 kB/s.
 *
 * The task never blocks inside USB.  If the endpoint is busy it leaves the
 * packets in the ring and tries again on the next tick; the ring is the shock
 * absorber, and its high-water mark (reported by the `stats` command) is the
 * direct measure of how close the host ever came to causing a loss.
 */
#include "shared.h"
#include "usb_cdc.h"
#include "packet.h"
#include "cycle_counter.h"
#include "board.h"
#include "gpio.h"

#define UPLINK_BATCH_PACKETS 16U
#define UPLINK_BATCH_BYTES   (UPLINK_BATCH_PACKETS * DAQ_PACKET_BYTES)

_Static_assert(UPLINK_BATCH_BYTES == 384U, "batch must match the EP1 IN FIFO");
_Static_assert((UPLINK_BATCH_BYTES % 64U) == 0U, "batch must be whole USB packets");

static uint8_t s_batch[UPLINK_BATCH_BYTES];

void task_uplink(void *arg)
{
    (void)arg;

    for (;;) {
        if (!usb_cdc_is_connected()) {
            /* Not enumerated.  Drain and discard so that the rings do not
               fill and start reporting overflows that are not the firmware's
               fault - the data has nowhere to go and saying so once (via the
               stats counters) is more useful than flagging every packet. */
            uint8_t sink[DAQ_PACKET_BYTES];
            while (spsc_pop(&g_ring_tx, sink)) { }
            vTaskDelay(pdMS_TO_TICKS(50U));
            continue;
        }

        if (!usb_cdc_tx_ready()) {
            g_stats.usb_retries++;
            vTaskDelay(1U);
            continue;
        }

        {
            const uint32_t n = spsc_pop_bulk(&g_ring_tx, s_batch,
                                             UPLINK_BATCH_PACKETS);
            if (n == 0U) {
                vTaskDelay(1U);
                continue;
            }

            if (usb_cdc_write(s_batch, (uint16_t)(n * DAQ_PACKET_BYTES))) {
                g_stats.packets_sent += n;
            } else {
                /* Should not happen - tx_ready() was true a moment ago - but
                   if it does, those n packets are gone.  Count them so the
                   host's sequence-gap detector and this counter agree. */
                g_stats.usb_retries++;
            }
        }
    }
}
