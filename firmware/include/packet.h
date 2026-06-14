/**
 * @file    packet.h
 * @brief   Binary wire codec for the USB CDC stream.  See docs/protocol.md.
 *
 * One packet == one complete 4-channel scan == 24 bytes, little-endian:
 *
 *   off  size  field
 *    0    2    sync   0xA5 0x5A
 *    2    1    ver    protocol version (0x01)
 *    3    1    flags  low byte of daq_raw_scan_t.status
 *    4    2    seq    u16, free-running scan counter, wraps at 65535
 *    6    4    t_us   u32, TIM2 microseconds at scan start
 *   10   12    ch[4]  4 x int24 little-endian, sign-extended by the decoder
 *   22    2    crc16  CCITT-FALSE over bytes 0..21
 *
 * Design notes that matter and are easy to get wrong:
 *
 *  - The sync word is *not* enough to frame on.  0xA5 0x5A occurs naturally in
 *    24-bit sample data roughly once every 65 k bytes, so a receiver that
 *    trusts sync alone will lock onto garbage within a minute or two at
 *    45 kB/s.  Framing is sync AND version AND CRC, and the decoder here
 *    reflects that.
 *  - The timestamp is taken at scan *start*, in the ISR, not at packetisation
 *    time in a task.  Otherwise jitter in the processor task would show up as
 *    jitter in the data, which for a PWV-style measurement is fatal.
 *  - 24 bytes is 4-byte aligned, so the encoder never does an unaligned store
 *    and the host can memoryview/numpy the stream without a copy.
 */
#ifndef DAQ_PACKET_H
#define DAQ_PACKET_H

#include <stddef.h>
#include "daq_types.h"

/**
 * @brief Serialise one scan into @p out (must hold DAQ_PACKET_BYTES bytes).
 */
void daq_packet_encode(const daq_raw_scan_t *scan, uint8_t *out);

/**
 * @brief Validate and deserialise one packet.
 * @return true if sync, version and CRC all check out.
 */
bool daq_packet_decode(const uint8_t *in, daq_raw_scan_t *scan);

/**
 * @brief Scan a byte stream for the first offset that decodes cleanly.
 *
 * This is the resynchronisation path used after a USB stall, a host-side
 * buffer overrun, or an attach part-way through a frame.
 *
 * @param buf     stream bytes
 * @param len     bytes available
 * @param out_off receives the offset of a valid frame start
 * @return true if a fully validated frame was found within @p buf
 */
bool daq_packet_resync(const uint8_t *buf, size_t len, size_t *out_off);

/** @brief Sign-extend a 24-bit two's complement value to int32_t. */
static inline int32_t daq_sext24(uint32_t raw24)
{
    return (int32_t)((raw24 & 0x800000UL) != 0U ? (raw24 | 0xFF000000UL) : raw24);
}

#endif /* DAQ_PACKET_H */
