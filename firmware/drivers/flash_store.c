#include "flash_store.h"
#include "board.h"
#include <string.h>

/* Provided by the linker script.  Taking the *address* of these symbols is the
   value; that is why they are declared as arrays. */
extern uint32_t __calstore_start[];
extern uint32_t __calstore_size[];

#define CAL_SECTOR_NUMBER 11U
#define SLOT_BYTES        sizeof(daq_cal_t)

static const daq_cal_t *slot_ptr(uint32_t i)
{
    /* Slots are 64 bytes and the region base is sector-aligned, so every slot
       is 64-byte aligned.  The cast goes through void* because the compiler
       cannot prove that from the byte arithmetic alone. */
    const void *p = (const void *)((uintptr_t)__calstore_start +
                                   ((uintptr_t)i * SLOT_BYTES));
    return (const daq_cal_t *)p;
}

static bool slot_is_blank(const daq_cal_t *s)
{
    /* Byte-wise: a blank slot is all 0xFF because that is what an erased
       STM32F4 flash cell reads as.  This is also why DAQ_CAL_MAGIC must not
       be 0xFFFFFFFF - an erased slot would otherwise look like a record. */
    const uint8_t *bytes = (const uint8_t *)s;

    for (uint32_t i = 0U; i < SLOT_BYTES; i++) {
        if (bytes[i] != 0xFFU) {
            return false;
        }
    }
    return true;
}

static void flash_unlock(void)
{
    if ((FLASH->CR & FLASH_CR_LOCK) != 0U) {
        FLASH->KEYR = FLASH_KEY1;
        FLASH->KEYR = FLASH_KEY2;
    }
}

static void flash_lock(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
}

static fs_status_t flash_wait_idle(void)
{
    while ((FLASH->SR & FLASH_SR_BSY) != 0U) { }

    if ((FLASH->SR & FLASH_SR_ERRORS) != 0U) {
        FLASH->SR = FLASH_SR_ERRORS | FLASH_SR_EOP;   /* w1c */
        return FS_ERR_PROGRAM;
    }
    FLASH->SR = FLASH_SR_EOP;
    return FS_OK;
}

static fs_status_t flash_program_word(uint32_t addr, uint32_t value)
{
    FLASH->CR = (FLASH->CR & ~FLASH_CR_PSIZE_X32) | FLASH_CR_PSIZE_X32;
    FLASH->CR |= FLASH_CR_PG;

    *(volatile uint32_t *)addr = value;
    __DSB();

    {
        const fs_status_t st = flash_wait_idle();
        FLASH->CR &= ~FLASH_CR_PG;
        if (st != FS_OK) {
            return st;
        }
    }

    return (*(volatile uint32_t *)addr == value) ? FS_OK : FS_ERR_VERIFY;
}

static fs_status_t flash_erase_cal_sector(void)
{
    FLASH->CR &= ~(0x1FUL << FLASH_CR_SNB_Pos);
    FLASH->CR |= FLASH_CR_SER | (CAL_SECTOR_NUMBER << FLASH_CR_SNB_Pos) |
                 FLASH_CR_PSIZE_X32;
    FLASH->CR |= FLASH_CR_STRT;

    {
        const fs_status_t st = flash_wait_idle();
        FLASH->CR &= ~FLASH_CR_SER;
        if (st != FS_OK) {
            return FS_ERR_ERASE;
        }
    }

    /*
     * Flush the ART data cache.  The F407's instruction/data caches sit in
     * front of the flash array and do not observe an erase.  Without this,
     * reads of the just-erased sector can return pre-erase content for as long
     * as the line stays resident - which makes the "newest valid slot" scan
     * find a record that no longer physically exists.
     */
    FLASH->ACR &= ~FLASH_ACR_DCEN;
    FLASH->ACR |= (1UL << 12);            /* DCRST */
    FLASH->ACR &= ~(1UL << 12);
    FLASH->ACR |= FLASH_ACR_DCEN;
    return FS_OK;
}

/** @return index of the first blank slot, or FS_SLOT_COUNT if the log is full. */
static uint32_t find_first_blank(void)
{
    for (uint32_t i = 0U; i < FS_SLOT_COUNT; i++) {
        if (slot_is_blank(slot_ptr(i))) {
            return i;
        }
    }
    return FS_SLOT_COUNT;
}

uint32_t flash_store_slots_used(void)
{
    return find_first_blank();
}

fs_status_t flash_store_load(daq_cal_t *out)
{
    const uint32_t used = find_first_blank();
    uint32_t i = used;

    /* Walk backwards from the first blank slot.  The first record that passes
       CRC is the newest good one; a partially-programmed slot from an
       interrupted save fails here and is skipped, exactly as intended. */
    while (i != 0U) {
        i--;
        if (daq_cal_valid(slot_ptr(i))) {
            (void)memcpy(out, slot_ptr(i), sizeof(*out));
            return FS_OK;
        }
    }
    return FS_ERR_NOT_FOUND;
}

fs_status_t flash_store_save(daq_cal_t *cal, uint32_t uptime_s)
{
    daq_cal_t previous;
    bool have_previous = (flash_store_load(&previous) == FS_OK);
    uint32_t serial = have_previous ? (previous.serial + 1U) : 1U;
    uint32_t slot   = find_first_blank();

    /* Compile-time geometry check against the linker script. */
    _Static_assert((131072U % sizeof(daq_cal_t)) == 0U,
                   "calibration record must tile the reserved sector exactly");

    flash_unlock();
    if ((FLASH->CR & FLASH_CR_LOCK) != 0U) {
        return FS_ERR_LOCKED;
    }

    if (slot >= FS_SLOT_COUNT) {
        /* Log full: erase and start over.  The record about to be written
           supersedes everything discarded, so nothing current is lost. */
        const fs_status_t st = flash_erase_cal_sector();
        if (st != FS_OK) {
            flash_lock();
            return st;
        }
        slot = 0U;
    }

    daq_cal_finalize(cal, serial, uptime_s);

    {
        const uint32_t base = (uint32_t)(uintptr_t)__calstore_start +
                              (slot * (uint32_t)SLOT_BYTES);
        const uint32_t *src = (const uint32_t *)(const void *)cal;

        for (uint32_t w = 0U; w < (SLOT_BYTES / 4U); w++) {
            const fs_status_t st = flash_program_word(base + (w * 4U), src[w]);
            if (st != FS_OK) {
                flash_lock();
                return st;
            }
        }
    }

    flash_lock();

    /* Read back through the normal accessor, not the write pointer: this also
       proves the CRC and the "newest wins" scan agree with what we intended. */
    {
        daq_cal_t check;
        if (flash_store_load(&check) != FS_OK) {
            return FS_ERR_VERIFY;
        }
        if (memcmp(&check, cal, sizeof(check)) != 0) {
            return FS_ERR_VERIFY;
        }
    }

    (void)__calstore_size;
    return FS_OK;
}
