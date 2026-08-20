#include "shared.h"
#include "packet.h"

/*
 * Ring storage lives in CCM (core-coupled memory).  CCM is zero-wait-state
 * from the core and, crucially, sits on a bus that the DMA controllers and the
 * USB core cannot reach at all.  Putting the rings there guarantees the
 * sampler's accesses never contend with USB FIFO traffic on the SRAM1 bus
 * matrix.  It also, by construction, makes it impossible to hand a ring
 * address to a DMA engine by mistake - it would simply bus-fault immediately
 * rather than transferring garbage.
 */
#define CCM __attribute__((section(".ccmram"), aligned(4)))

static CCM daq_raw_scan_t s_raw_storage[RING_RAW_SLOTS];
static CCM uint8_t        s_tx_storage[RING_TX_SLOTS * DAQ_PACKET_BYTES];
static CCM daq_raw_scan_t s_dsp_storage[RING_DSP_SLOTS];

/*
 * The metrics ring carries variable-length records in fixed-size slots: a
 * metrics record is 64 bytes and a PSD block is 1044, but every slot is sized
 * for the larger. That wastes 980 bytes in eight slots - 7.8 KB of CCM which is
 * otherwise idle - and buys a ring whose element size is a compile-time
 * constant, so spsc_pop() stays a single memcpy with no length arithmetic on
 * the consumer side. The record's own type byte carries the real length.
 */
static CCM uint8_t s_meta_storage[RING_META_SLOTS * DAQ_MAX_RECORD_BYTES];

spsc_ring_t g_ring_raw;
spsc_ring_t g_ring_tx;
spsc_ring_t g_ring_dsp;
spsc_ring_t g_ring_meta;

TaskHandle_t g_task_sampler;
TaskHandle_t g_task_processor;
TaskHandle_t g_task_uplink;
TaskHandle_t g_task_metrics;

daq_cal_t g_calibration;
volatile bool g_calibration_from_flash;
volatile bool g_streaming = true;
volatile daq_mode_t g_mode = DAQ_MODE_MUX4;

metrics_t g_metrics;

daq_stats_t g_stats;

void daq_shared_init(void)
{
    configASSERT(spsc_init(&g_ring_raw, s_raw_storage,
                           (uint32_t)sizeof(daq_raw_scan_t), RING_RAW_SLOTS));
    configASSERT(spsc_init(&g_ring_tx, s_tx_storage,
                           DAQ_PACKET_BYTES, RING_TX_SLOTS));
    configASSERT(spsc_init(&g_ring_dsp, s_dsp_storage,
                           (uint32_t)sizeof(daq_raw_scan_t), RING_DSP_SLOTS));
    configASSERT(spsc_init(&g_ring_meta, s_meta_storage,
                           DAQ_MAX_RECORD_BYTES, RING_META_SLOTS));

    /* The muxed mode delivers 1875 scans/s per channel; the wideband mode
       delivers 7500 decimated samples/s. metrics_init() is called again by the
       housekeeping task on every mode change. */
    metrics_init(&g_metrics, (float)DAQ_SCAN_RATE_SPS);
}
