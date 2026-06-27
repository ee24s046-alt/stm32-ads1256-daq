/**
 * @file    calibration.h
 * @brief   Two-point (offset + gain) correction and its persistent record.
 *
 * Correction model, per channel:
 *
 *     corrected = ((raw - offset) * gain_q16) >> 16
 *
 * Fixed point, not float, on purpose.  The F407 has a single-precision FPU,
 * but touching it in the processor task forces FreeRTOS to lazily stack the
 * 33-word FPU context on every preemption of that task, which adds jitter to
 * a path that is supposed to be boring.  Q16.16 covers a gain range of
 * 0.0000153 .. 32767 with a resolution of 15 ppm - roughly two orders of
 * magnitude finer than the ADS1256's own gain drift over temperature, so the
 * fixed-point representation is nowhere near the dominant error term.
 *
 * The record is 64 bytes and is stored as one entry in an append-only log in
 * flash sector 11 (see flash_store.h).  Keeping it a fixed 64 bytes with its
 * own CRC is what makes the log scannable: "newest entry whose CRC checks out
 * wins", with no separate index to keep consistent.
 */
#ifndef DAQ_CALIBRATION_H
#define DAQ_CALIBRATION_H

#include "daq_types.h"

#define DAQ_CAL_MAGIC    0x31434441UL   /* "ADC1" little-endian */
#define DAQ_CAL_VERSION  0x0001U
#define DAQ_CAL_UNITY_Q16 65536L

typedef struct {
    uint32_t magic;                        /* 0x00 */
    uint16_t version;                      /* 0x04 */
    uint16_t num_channels;                 /* 0x06 */
    uint32_t serial;                       /* 0x08 monotonic write counter   */
    uint32_t uptime_s;                     /* 0x0C uptime when calibrated    */
    int32_t  offset[DAQ_NUM_CHANNELS];     /* 0x10 codes, shorted input      */
    int32_t  gain_q16[DAQ_NUM_CHANNELS];   /* 0x20 Q16.16, 65536 == unity    */
    uint32_t reserved[3];                  /* 0x30 pads the record to 64 B   */
    uint32_t crc32;                        /* 0x3C - covers bytes 0x00..0x3B */
} daq_cal_t;

/* If this fires, the flash log geometry in flash_store.c is wrong. */
_Static_assert(sizeof(daq_cal_t) == 64, "calibration record must stay 64 bytes");

/** @brief Populate a record with unity gain / zero offset. */
void daq_cal_set_identity(daq_cal_t *cal);

/** @brief Stamp magic/version/length and compute the CRC. Call before writing. */
void daq_cal_finalize(daq_cal_t *cal, uint32_t serial, uint32_t uptime_s);

/** @brief true if magic, version, channel count and CRC all check out. */
bool daq_cal_valid(const daq_cal_t *cal);

/**
 * @brief Apply the correction to one raw code, saturating at 24-bit full scale.
 *
 * Saturation rather than wrap: a clipped reading is recoverable information
 * ("this channel railed"), a wrapped one is a sign flip that looks like real
 * signal and will quietly corrupt any downstream analysis.
 */
int32_t daq_cal_apply(const daq_cal_t *cal, uint32_t channel, int32_t raw);

/** @brief true if @p raw is at or beyond 24-bit full scale. */
bool daq_cal_is_overrange(int32_t raw);

/**
 * @brief Derive offset and gain for one channel from two measured points.
 *
 * @param cal        record to update
 * @param channel    channel index
 * @param zero_code  mean code with the inputs shorted
 * @param ref_code   mean code with the reference applied
 * @param ref_ideal  the code that reference *should* produce
 * @return false if the two points are degenerate (would divide by zero) or if
 *         the implied gain falls outside 0.5x .. 2.0x, which in practice means
 *         the reference was not actually connected.
 */
bool daq_cal_from_two_point(daq_cal_t *cal, uint32_t channel,
                            int32_t zero_code, int32_t ref_code,
                            int32_t ref_ideal);

#endif /* DAQ_CALIBRATION_H */
