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

/**
 * /DRDY period in the single-channel wideband mode.
 *
 * With the multiplexer parked there is no filter restart and therefore no
 * settling penalty, so every conversion is usable and /DRDY arrives at the full
 * DRATE: 33 us.  That is a 4x tighter deadline than the muxed mode and it is
 * the reason wideband is a mode rather than the default.
 */
#define DAQ_WIDEBAND_PERIOD_US  (1000000U / DAQ_ADC_DATA_RATE_SPS)           /* 33 */

/** Decimated output rate in wideband mode, after the M=4 anti-alias filter. */
#define DAQ_WIDEBAND_OUT_SPS    (DAQ_ADC_DATA_RATE_SPS / 4U)                 /* 7500 */

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

/* -------------------------------------------------------------------------
 * Acquisition modes
 *
 * The two modes exist because the ADS1256's settling penalty applies per MUX
 * change, not per conversion.  Parking the multiplexer removes it entirely.
 * ---------------------------------------------------------------------- */
typedef enum {
    /**
     * Four differential pairs, MUX cycled every conversion.
     * 30 kSPS DRATE / 4 settling periods = 7500 settled conversions/s,
     * /4 channels = 1875 complete scans/s.  /DRDY every 133 us.
     */
    DAQ_MODE_MUX4 = 0,

    /**
     * One differential pair, multiplexer parked, RDATAC continuous read.
     * No MUX change means no settling penalty, so every conversion is usable:
     * a true 30 kSPS, /DRDY every 33.3 us.  The on-device decimator then takes
     * it to 7.5 kSPS with proper anti-aliasing (see fir_coeffs.h).
     *
     * This is the mode the DSP chain is specified for, and the only one in
     * which the numbers work: M = 4 from 30 kSPS gives a 3750 Hz output
     * Nyquist, which is what makes a 3.5 kHz anti-alias corner meaningful.
     * Decimating the muxed mode's 1875 Hz per-channel stream by 4 would put
     * the output Nyquist at 234 Hz and that corner would alias badly.
     */
    DAQ_MODE_WIDEBAND = 1,
} daq_mode_t;

/* -------------------------------------------------------------------------
 * Record types on the wire.
 *
 * All records share the 0xA5 first sync byte; the second byte identifies the
 * type and fixes the length, so the host needs no length field and the
 * existing 24-byte sample record is untouched.  See docs/protocol.md.
 * ---------------------------------------------------------------------- */
#define DAQ_SYNC1_SAMPLE   0x5AU   /**< 24 bytes   */
#define DAQ_SYNC1_METRICS  0x5BU   /**< 64 bytes   */
#define DAQ_SYNC1_PSD      0x5CU   /**< 1044 bytes */

#define DAQ_METRICS_BYTES  64U
#define DAQ_PSD_BINS       513U    /**< 1024-point FFT, DC..Nyquist inclusive */
#define DAQ_PSD_BYTES      (18U + (DAQ_PSD_BINS * 2U))   /* 1044 */
#define DAQ_MAX_RECORD_BYTES DAQ_PSD_BYTES

/** Live metrics, computed on-device.  See metrics.h for the method. */
typedef struct {
    int32_t  rms[DAQ_NUM_CHANNELS];       /**< codes, 100 ms window          */
    int32_t  thdn_mdb[DAQ_NUM_CHANNELS];  /**< milli-dB, INT32_MIN = invalid */
    uint32_t fund_mhz[DAQ_NUM_CHANNELS];  /**< milli-Hz, 0 = no tone found   */
    uint32_t psd_serial;
    uint32_t t_us;
    uint16_t seq;
    uint8_t  mode;
} daq_metrics_rec_t;

/** One averaged power spectrum for one channel. */
typedef struct {
    int16_t  bin_dbfs_x100[DAQ_PSD_BINS];
    uint32_t fs_mhz;                      /**< rate of the analysed stream   */
    uint32_t t_us;
    uint16_t seq;
    uint8_t  channel;
} daq_psd_rec_t;

#endif /* DAQ_TYPES_H */
