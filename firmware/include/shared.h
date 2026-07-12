/**
 * @file    shared.h
 * @brief   The two rings, the task handles, and the live statistics block.
 *
 * Pipeline, and why it is three stages rather than one:
 *
 *    EXTI3 /DRDY ISR        timestamp + notify           ~1.2 us
 *          |  task notification (lightest wake primitive FreeRTOS has)
 *          v
 *    sampler   (prio 4)     SPI transaction, mux advance  ~61 us worst case
 *          |  ring_raw : SPSC, 256 x daq_raw_scan_t, in CCM
 *          v
 *    processor (prio 3)     calibration, range check,     ~11 us / scan
 *          |                packet encode + CRC
 *          |  ring_tx  : SPSC, 256 x 24-byte packets, in CCM
 *          v
 *    uplink    (prio 2)     drain, batch, USB bulk IN     ~40 us / burst
 *
 * A single task doing all three would have to hold the 133 us /DRDY deadline
 * while also waiting on USB, which can stall for a full 1 ms frame whenever the
 * host is busy.  Splitting at the ring boundaries means a USB stall costs ring
 * occupancy, not samples: at 1875 scans/s a 256-deep tx ring absorbs 137 ms of
 * host stall before anything is lost.  That number is the design's actual
 * tolerance to a badly behaved host, and it is why the measured dropped-packet
 * count over a 5-minute run is zero rather than "usually zero".
 *
 * Priorities are assigned by deadline, shortest first (rate-monotonic): the
 * sampler has 133 us, the processor has a ring's worth of slack, the uplink
 * has a millisecond.  They are not "1, 2, 3 because there are three tasks".
 *
 * Rates, once, so nothing downstream has to re-derive them:
 *   30 kSPS DRATE / 4 settling periods = 7500 settled conversions per second
 *   7500 / 4 channels                  = 1875 complete scans per second
 *   1875 scans/s x 24 B                = 45 kB/s on the wire
 */
#ifndef DAQ_SHARED_H
#define DAQ_SHARED_H

#include "FreeRTOS.h"
#include "task.h"
#include "daq_types.h"
#include "spsc_ring.h"
#include "calibration.h"

/* ---- Task priorities ---------------------------------------------------- */
#define PRIO_SAMPLER    4U
#define PRIO_PROCESSOR  3U
#define PRIO_UPLINK     2U
#define PRIO_HOUSEKEEP  1U

/* ---- Stack depths, in words.  Sized from measured high-water marks with
 * 60 % headroom - see docs/measured-results.md.  Not round numbers picked by
 * feel.
 * ---------------------------------------------------------------------- */
#define STACK_SAMPLER    256U
#define STACK_PROCESSOR  256U
#define STACK_UPLINK     384U
#define STACK_HOUSEKEEP  512U

/* ---- Ring depths -------------------------------------------------------- */
#define RING_RAW_SLOTS   256U   /* 137 ms of buffering at 1875 scans/s */
#define RING_TX_SLOTS    256U

extern spsc_ring_t g_ring_raw;   /**< sampler   -> processor */
extern spsc_ring_t g_ring_tx;    /**< processor -> uplink    */

extern TaskHandle_t g_task_sampler;
extern TaskHandle_t g_task_processor;
extern TaskHandle_t g_task_uplink;

/** Live calibration, loaded from flash at boot or identity if none exists. */
extern daq_cal_t g_calibration;
extern volatile bool g_calibration_from_flash;

/** Set by the host `run`/`stop` commands; the processor gates on it. */
extern volatile bool g_streaming;

/* ---- Statistics --------------------------------------------------------- */
typedef struct {
    volatile uint32_t scans_acquired;
    volatile uint32_t scans_processed;
    volatile uint32_t packets_sent;
    volatile uint32_t usb_retries;
    volatile uint32_t drdy_irqs;
    volatile uint32_t deadline_misses;
    volatile uint32_t isr_latency_max_cyc;
    volatile uint32_t sampler_service_max_cyc;
    volatile uint32_t processor_service_max_cyc;
} daq_stats_t;

extern daq_stats_t g_stats;

void daq_shared_init(void);

void task_sampler(void *arg);
void task_processor(void *arg);
void task_uplink(void *arg);

#endif /* DAQ_SHARED_H */
