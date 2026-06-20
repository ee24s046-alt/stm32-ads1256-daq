/**
 * @file spi.h
 * @brief Bare-metal SPI1 master for the ADS1256.
 *
 * Blocking, byte-at-a-time, no DMA - and that is the correct choice here, for
 * a reason worth stating: the longest transaction in this system is 3 bytes
 * (18.3 us at 1.3125 MHz).  A DMA setup costs ~2 us of CPU plus an interrupt
 * on completion plus the scheduling latency of getting back into the sampler
 * task.  Against 18 us of transfer, DMA would *increase* worst-case latency
 * while adding a coherency hazard (the ADS1256 buffers live in CCM, which
 * DMA1/2 on the F407 cannot reach at all).  The sampler task runs at the top
 * priority and has 133 us of budget; spinning on TXE/RXNE for 18 us of it is
 * the simple, measurable, provably-bounded option.
 *
 * ADS1256 SPI mode: CPOL = 0, CPHA = 1 (mode 1), MSB first.
 */
#ifndef DAQ_SPI_H
#define DAQ_SPI_H

#include <stdint.h>
#include <stddef.h>

void spi1_init(void);

/** @brief Full-duplex exchange of a single byte. Blocks ~7.6 us at 1.3125 MHz. */
uint8_t spi1_xfer(uint8_t tx);

/** @brief Clock out @p len bytes, discarding the returned data. */
void spi1_write(const uint8_t *tx, size_t len);

/** @brief Clock out 0xFF @p len times, storing what comes back. */
void spi1_read(uint8_t *rx, size_t len);

#endif /* DAQ_SPI_H */
