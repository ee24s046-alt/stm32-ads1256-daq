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
#include "fir_decimate.h"
#include "metrics.h"

/* ---- Task priorities ---------------------------------------------------- */
#define PRIO_SAMPLER    4U
#define PRIO_PROCESSOR  3U
#define PRIO_UPLINK     2U
#define PRIO_METRICS    1U
#define PRIO_HOUSEKEEP  1U

/* ---- Stack depths, in words.  Sized from measured high-water marks with
 * 60 % headroom - see docs/measured-results.md.  Not round numbers picked by
 * feel.
 * ---------------------------------------------------------------------- */
#define STACK_SAMPLER    256U
#define STACK_PROCESSOR  384U
#define STACK_UPLINK     384U
#define STACK_METRICS    512U
#define STACK_HOUSEKEEP  512U

/* ---- Ring depths --------------------------------------------------------
 * RING_RAW is sized by the harder of the two modes. At 1875 scans/s it holds
 * 137 ms; in wideband mode the sampler runs at 30 kSPS, so the same 256 slots
 * are only 8.5 ms - still 8x the processor's 1 ms polling period.
 * ---------------------------------------------------------------------- */
#define RING_RAW_SLOTS   256U
#define RING_TX_SLOTS    256U
#define RING_DSP_SLOTS   256U   /* processor -> metrics            */
#define RING_META_SLOTS    8U   /* metrics   -> uplink, 1044 B each */

extern spsc_ring_t g_ring_raw;   /**< sampler   -> processor */
extern spsc_ring_t g_ring_tx;    /**< processor -> uplink    */
extern spsc_ring_t g_ring_dsp;   /**< processor -> metrics   */
extern spsc_ring_t g_ring_meta;  /**< metrics   -> uplink    */

extern TaskHandle_t g_task_sampler;
extern TaskHandle_t g_task_processor;
extern TaskHandle_t g_task_uplink;
extern TaskHandle_t g_task_metrics;

/**
 * Acquisition mode. Written only by the housekeeping task in response to a
 * host command, read by the sampler and processor. A plain volatile is
 * sufficient: it is a single aligned word, and a mode change taking effect one
 * sample early or late is harmless - the packets carry the mode in their flags,
 * so the host is never confused about which regime a sample belongs to.
 */
extern volatile daq_mode_t g_mode;

/** Live on-device metrics. Owned by the metrics task. */
extern metrics_t g_metrics;

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
    volatile uint32_t decim_out;          /**< samples emitted by the decimator */
    volatile uint32_t decim_service_max_cyc;
    volatile uint32_t psd_blocks;         /**< completed Welch averages         */
    volatile uint32_t psd_service_max_cyc;/**< worst-case 1024-point FFT + PSD  */
    volatile uint32_t meta_sent;
    volatile uint32_t meta_dropped;
} daq_stats_t;

extern daq_stats_t g_stats;

void daq_shared_init(void);

void task_sampler(void *arg);
void task_processor(void *arg);
void task_uplink(void *arg);
void task_metrics(void *arg);

#endif /* DAQ_SHARED_H */
