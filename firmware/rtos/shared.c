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

spsc_ring_t g_ring_raw;
spsc_ring_t g_ring_tx;

TaskHandle_t g_task_sampler;
TaskHandle_t g_task_processor;
TaskHandle_t g_task_uplink;

daq_cal_t g_calibration;
volatile bool g_calibration_from_flash;
volatile bool g_streaming = true;

daq_stats_t g_stats;

void daq_shared_init(void)
{
    configASSERT(spsc_init(&g_ring_raw, s_raw_storage,
                           (uint32_t)sizeof(daq_raw_scan_t), RING_RAW_SLOTS));
    configASSERT(spsc_init(&g_ring_tx, s_tx_storage,
                           DAQ_PACKET_BYTES, RING_TX_SLOTS));
}
