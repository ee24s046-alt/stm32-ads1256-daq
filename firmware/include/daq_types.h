/**
 * @file    daq_types.h
 * @brief   Types shared by the firmware pipeline and the host unit tests.
 *
 * Nothing in this header depends on the MCU, the RTOS, or libc beyond
 * <stdint.h>.  That is what lets tests/ compile the real ring buffer and the
 * real packet codec natively and exercise them under CTest.
 */
#ifndef DAQ_TYPES_H
#define DAQ_TYPES_H

#include <stdint.h>
#include <stdbool.h>

/** Number of differential input pairs scanned. */
#define DAQ_NUM_CHANNELS        4U

/** ADS1256 raw conversion rate programmed into the DRATE register. */
#define DAQ_ADC_DATA_RATE_SPS   30000U

/**
 * Settling factor for a multiplexed input.
 *
 * This is the number that decides the whole real-time budget, and it is not
 * 1.  The ADS1256's decimation filter is a 5th-order sinc; after a MUX change
 * followed by SYNC/WAKEUP, the first *fully settled* result is not available
 * until the filter has flushed.  The datasheet's settling-time table gives
 * 0.13 ms at DRATE = 30 kSPS, i.e. exactly 4 conversion periods.
 *
 * So a channel-cycling design running the converter at 30 kSPS delivers one
 * settled conversion every 133 us - 7500 settled samples per second in total,
 * not 30000.  Ignoring this is the most common way an ADS1256 multiplexed
 * design ends up quoting a sample rate it does not actually achieve, with the
 * unsettled readings showing up later as inter-channel crosstalk.
 */
#define DAQ_SETTLING_PERIODS    4U

/** Settled conversions delivered per second, across all channels. */
#define DAQ_SAMPLE_DELIVERY_SPS (DAQ_ADC_DATA_RATE_SPS / DAQ_SETTLING_PERIODS) /* 7500 */

/**
 * /DRDY period in microseconds - the hard deadline for the sampler.
 * 1/7500 s = 133 us.  Every timing number in docs/timing-budget.md is measured
 * against this.
 */
#define DAQ_DRDY_PERIOD_US      (1000000U / DAQ_SAMPLE_DELIVERY_SPS)         /* 133 */

/** Complete 4-channel scans per second, and the interval between them. */
#define DAQ_SCAN_RATE_SPS       (DAQ_SAMPLE_DELIVERY_SPS / DAQ_NUM_CHANNELS) /* 1875 */
#define DAQ_SCAN_PERIOD_US      (DAQ_DRDY_PERIOD_US * DAQ_NUM_CHANNELS)      /* 532 */

/* -------------------------------------------------------------------------
 * Stage 1 -> Stage 2 element: one completed 4-channel scan, uncorrected.
 * ---------------------------------------------------------------------- */
typedef struct {
    int32_t  code[DAQ_NUM_CHANNELS];  /**< sign-extended 24-bit ADC codes     */
    uint32_t t_us;                    /**< TIM2 capture at scan start         */
    uint16_t seq;                     /**< free-running scan counter          */
    uint16_t status;                  /**< DAQ_STATUS_* bitfield              */
} daq_raw_scan_t;

#define DAQ_STATUS_OK          0x0000U
#define DAQ_STATUS_CAL_APPLIED 0x0001U  /**< offset+gain correction was live  */
#define DAQ_STATUS_OVERRANGE   0x0002U  /**< >= 1 channel clipped full scale  */
#define DAQ_STATUS_DRDY_LATE   0x0004U  /**< ISR missed its 133 us deadline   */
#define DAQ_STATUS_RAW_OVF     0x0008U  /**< stage-1 ring overflowed          */
#define DAQ_STATUS_TX_OVF      0x0010U  /**< stage-2 ring overflowed          */

/* -------------------------------------------------------------------------
 * Wire format.  See docs/protocol.md - that document and this struct must
 * agree; tests/test_packet.c checks the byte layout against a golden vector.
 * ---------------------------------------------------------------------- */
#define DAQ_SYNC0        0xA5U
#define DAQ_SYNC1        0x5AU
#define DAQ_PROTO_VER    0x01U
#define DAQ_PACKET_BYTES 24U

/** Full-scale magnitude of a 24-bit two's complement code. */
#define DAQ_CODE_FULL_SCALE  8388607L   /* 2^23 - 1 */
#define DAQ_CODE_MIN        -8388608L   /* -2^23    */

#endif /* DAQ_TYPES_H */
