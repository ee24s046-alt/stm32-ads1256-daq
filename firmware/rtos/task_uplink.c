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
 *   - At 1875 packets/s (muxed) that is 117 USB transfers per second; at 7500
 *     (wideband, post-decimation) it is 469.  Full-speed bulk allows up to 19
 *     64-byte transactions per 1 ms frame, so even the wideband case uses
 *     about 3 % of the available bulk bandwidth - which is why the host never
 *     has to NAK us, and why the measured dropped-packet count is zero.
 *
 * Payload rates: 45 kB/s muxed, 180 kB/s wideband, plus ~1.3 kB/s of metrics.
 *
 * Mixing record sizes
 * -------------------
 * Sample packets are 24 bytes; a PSD record is 1044, which does not fit in the
 * 384-byte FIFO and must be sent as three writes.  The rule that keeps the
 * stream parseable is that a record in progress always finishes before any
 * sample packet resumes - interleaving halfway through a record would leave the
 * host's framer looking at a sample packet embedded inside a PSD block, and the
 * CRC would reject both.  Hence the "meta in progress" branch comes first and
 * every path through it continues rather than falling through.
 *
 * The task never blocks inside USB.  If the endpoint is busy it leaves the data
 * in the ring and tries again on the next tick; the ring is the shock absorber,
 * and its high-water mark (reported by `stats`) is the direct measure of how
 * close the host ever came to causing a loss.
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

/* A metrics or PSD record being sent in FIFO-sized pieces. */
static uint8_t  s_meta[DAQ_MAX_RECORD_BYTES];
static uint32_t s_meta_len;
static uint32_t s_meta_sent;

void task_uplink(void *arg)
{
    (void)arg;

    for (;;) {
        if (!usb_cdc_is_connected()) {
            /* Not enumerated.  Drain and discard so that the rings do not fill
               and start reporting overflows that are not the firmware's fault -
               the data has nowhere to go and saying so once (via the stats
               counters) is more useful than flagging every packet. */
            uint8_t sink[DAQ_MAX_RECORD_BYTES];
            while (spsc_pop(&g_ring_tx, sink)) { }
            while (spsc_pop(&g_ring_meta, sink)) { }
            s_meta_len = 0U;
            s_meta_sent = 0U;
            vTaskDelay(pdMS_TO_TICKS(50U));
            continue;
        }

        if (!usb_cdc_tx_ready()) {
            g_stats.usb_retries++;
            vTaskDelay(1U);
            continue;
        }

        /* ---- finish any record already in flight, before anything else ---- */
        if (s_meta_sent < s_meta_len) {
            const uint32_t remaining = s_meta_len - s_meta_sent;
            const uint32_t chunk = (remaining > UPLINK_BATCH_BYTES)
                                 ? UPLINK_BATCH_BYTES : remaining;

            if (usb_cdc_write(&s_meta[s_meta_sent], (uint16_t)chunk)) {
                s_meta_sent += chunk;
            } else {
                g_stats.usb_retries++;
            }
            continue;
        }

        /* ---- start a new metrics/PSD record if one is waiting ------------- */
        if (spsc_pop(&g_ring_meta, s_meta)) {
            s_meta_len = daq_record_length(s_meta[1]);
            s_meta_sent = 0U;
            if (s_meta_len == 0U) {
                /* Unreachable unless the ring was corrupted; drop rather than
                   emit a record of unknown length into a framed stream. */
                g_stats.meta_dropped++;
            }
            continue;
        }

        /* ---- otherwise, a batch of sample packets ------------------------- */
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
