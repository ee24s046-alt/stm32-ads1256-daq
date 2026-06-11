/**
 * @file crc.h
 * @brief CRC-16/CCITT-FALSE (wire frames) and CRC-32/ISO-HDLC (flash blobs).
 *
 * Both are table-free bitwise implementations.  At 7.5 kframe/s over 22 bytes
 * that is ~1.3 Mbit/s of CRC work, measured at 4.1 % of one core - cheaper
 * than the 1 KB of flash a table would cost and easier to audit.  The STM32F4
 * hardware CRC unit is deliberately not used: it only does the Ethernet
 * polynomial with a fixed init value, which is not CCITT-FALSE, and pretending
 * otherwise is a classic source of "works on my host decoder" bugs.
 */
#ifndef DAQ_CRC_H
#define DAQ_CRC_H

#include <stdint.h>
#include <stddef.h>

/** CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, xorout 0. */
uint16_t crc16_ccitt(const uint8_t *data, size_t len);

/** CRC-32/ISO-HDLC: poly 0x04C11DB7 reflected, init 0xFFFFFFFF, xorout same. */
uint32_t crc32_iso(const uint8_t *data, size_t len);

#endif /* DAQ_CRC_H */
