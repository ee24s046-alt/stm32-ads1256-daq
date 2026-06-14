#include "packet.h"
#include "crc.h"
#include <string.h>

#define OFF_SYNC0  0U
#define OFF_SYNC1  1U
#define OFF_VER    2U
#define OFF_FLAGS  3U
#define OFF_SEQ    4U
#define OFF_TIME   6U
#define OFF_DATA  10U
#define OFF_CRC   22U

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void daq_packet_encode(const daq_raw_scan_t *scan, uint8_t *out)
{
    out[OFF_SYNC0] = DAQ_SYNC0;
    out[OFF_SYNC1] = DAQ_SYNC1;
    out[OFF_VER]   = DAQ_PROTO_VER;
    out[OFF_FLAGS] = (uint8_t)(scan->status & 0xFFU);

    put_u16(&out[OFF_SEQ], scan->seq);
    put_u32(&out[OFF_TIME], scan->t_us);

    for (uint32_t c = 0U; c < DAQ_NUM_CHANNELS; c++) {
        const uint32_t v = (uint32_t)scan->code[c] & 0x00FFFFFFUL;
        uint8_t *p = &out[OFF_DATA + (c * 3U)];
        p[0] = (uint8_t)(v & 0xFFU);
        p[1] = (uint8_t)((v >> 8) & 0xFFU);
        p[2] = (uint8_t)((v >> 16) & 0xFFU);
    }

    put_u16(&out[OFF_CRC], crc16_ccitt(out, OFF_CRC));
}

bool daq_packet_decode(const uint8_t *in, daq_raw_scan_t *scan)
{
    if ((in[OFF_SYNC0] != DAQ_SYNC0) || (in[OFF_SYNC1] != DAQ_SYNC1)) {
        return false;
    }
    if (in[OFF_VER] != DAQ_PROTO_VER) {
        return false;
    }
    if (get_u16(&in[OFF_CRC]) != crc16_ccitt(in, OFF_CRC)) {
        return false;
    }

    scan->status = in[OFF_FLAGS];
    scan->seq    = get_u16(&in[OFF_SEQ]);
    scan->t_us   = get_u32(&in[OFF_TIME]);

    for (uint32_t c = 0U; c < DAQ_NUM_CHANNELS; c++) {
        const uint8_t *p = &in[OFF_DATA + (c * 3U)];
        const uint32_t raw = (uint32_t)p[0] |
                             ((uint32_t)p[1] << 8) |
                             ((uint32_t)p[2] << 16);
        scan->code[c] = daq_sext24(raw);
    }
    return true;
}

bool daq_packet_resync(const uint8_t *buf, size_t len, size_t *out_off)
{
    daq_raw_scan_t scratch;

    if (len < DAQ_PACKET_BYTES) {
        return false;
    }

    for (size_t off = 0U; off + DAQ_PACKET_BYTES <= len; off++) {
        /* Cheap reject first: two byte compares eliminate ~99.998 % of
           candidate offsets before the CRC is ever computed. */
        if ((buf[off] != DAQ_SYNC0) || (buf[off + 1U] != DAQ_SYNC1)) {
            continue;
        }
        if (daq_packet_decode(&buf[off], &scratch)) {
            *out_off = off;
            return true;
        }
    }
    return false;
}
