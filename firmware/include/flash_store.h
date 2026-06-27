/**
 * @file    flash_store.h
 * @brief   Append-only calibration log in flash sector 11.
 *
 * Why a log rather than "erase and rewrite one record"?
 *
 * An STM32F4 sector erase takes 1-2 s and leaves the sector at 0xFF the whole
 * time.  If power is lost during that window, the previous calibration is gone
 * and the unit cold-boots uncalibrated with no indication that anything was
 * lost.  Instead, sector 11 holds 2048 x 64-byte slots written in order.  A
 * save programs the next blank slot - a ~100 us operation with no erase - and
 * the newest slot whose CRC validates wins on boot.  The sector is only erased
 * when it fills, i.e. after 2048 saves, and even then the erase is followed
 * immediately by rewriting the record that was current.
 *
 * Power-fail behaviour: a save interrupted mid-program leaves a slot with a
 * bad CRC.  Boot scanning skips it and falls back to the previous valid slot.
 * The only value lost is the one that was being written, which is the correct
 * and expected outcome.
 *
 * Flash constraints honoured here:
 *   - Programming may only clear bits (1 -> 0), never set them.  A "blank"
 *     slot is therefore all-0xFF, which is why DAQ_CAL_MAGIC must not be
 *     0xFFFFFFFF.
 *   - Word (x32) programming requires 4-byte alignment; the record is 64 bytes
 *     and every slot is 64-byte aligned, so this holds by construction.
 *   - The flash controller must not be busy and the data cache must be
 *     invalidated after an erase, or subsequent reads return stale lines.
 */
#ifndef FLASH_STORE_H
#define FLASH_STORE_H

#include "calibration.h"

typedef enum {
    FS_OK = 0,
    FS_ERR_LOCKED,
    FS_ERR_PROGRAM,
    FS_ERR_ERASE,
    FS_ERR_VERIFY,
    FS_ERR_NOT_FOUND,
} fs_status_t;

/** Number of 64-byte calibration slots in the reserved sector. */
#define FS_SLOT_COUNT (131072U / 64U)   /* 2048 */

/**
 * @brief Find and return the newest valid calibration record.
 * @return FS_OK and fills @p out, or FS_ERR_NOT_FOUND on a virgin device.
 */
fs_status_t flash_store_load(daq_cal_t *out);

/**
 * @brief Append a calibration record, erasing and compacting if the log is full.
 *
 * The record is finalised (magic, serial, CRC) inside this call, and verified
 * by read-back before returning.
 */
fs_status_t flash_store_save(daq_cal_t *cal, uint32_t uptime_s);

/** @brief Slots used so far - exposed for the `stats` diagnostic. */
uint32_t flash_store_slots_used(void);

#endif /* FLASH_STORE_H */
