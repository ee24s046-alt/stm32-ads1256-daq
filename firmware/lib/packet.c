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

/* ======================================================================== */
/* Metrics and PSD records                                                  */
/* ======================================================================== */

uint32_t daq_record_length(uint8_t sync1)
{
    switch (sync1) {
    case DAQ_SYNC1_SAMPLE:  return DAQ_PACKET_BYTES;
    case DAQ_SYNC1_METRICS: return DAQ_METRICS_BYTES;
    case DAQ_SYNC1_PSD:     return DAQ_PSD_BYTES;
    default:                return 0U;
    }
}

/* ---- metrics: 64 bytes ------------------------------------------------- */
#define MET_OFF_MODE   3U
#define MET_OFF_SEQ    4U
#define MET_OFF_TIME   6U
#define MET_OFF_RMS   10U
#define MET_OFF_THDN  26U
#define MET_OFF_FUND  42U
#define MET_OFF_PSDSN 58U
#define MET_OFF_CRC   62U

void daq_metrics_encode(const daq_metrics_rec_t *rec, uint8_t *out)
{
    out[OFF_SYNC0]     = DAQ_SYNC0;
    out[OFF_SYNC1]     = DAQ_SYNC1_METRICS;
    out[OFF_VER]       = DAQ_PROTO_VER;
    out[MET_OFF_MODE]  = rec->mode;

    put_u16(&out[MET_OFF_SEQ], rec->seq);
    put_u32(&out[MET_OFF_TIME], rec->t_us);

    for (uint32_t c = 0U; c < DAQ_NUM_CHANNELS; c++) {
        put_u32(&out[MET_OFF_RMS  + (c * 4U)], (uint32_t)rec->rms[c]);
        put_u32(&out[MET_OFF_THDN + (c * 4U)], (uint32_t)rec->thdn_mdb[c]);
        put_u32(&out[MET_OFF_FUND + (c * 4U)], rec->fund_mhz[c]);
    }
    put_u32(&out[MET_OFF_PSDSN], rec->psd_serial);
    put_u16(&out[MET_OFF_CRC], crc16_ccitt(out, MET_OFF_CRC));
}

bool daq_metrics_decode(const uint8_t *in, daq_metrics_rec_t *rec)
{
    if ((in[OFF_SYNC0] != DAQ_SYNC0) || (in[OFF_SYNC1] != DAQ_SYNC1_METRICS)) {
        return false;
    }
    if (in[OFF_VER] != DAQ_PROTO_VER) {
        return false;
    }
    if (get_u16(&in[MET_OFF_CRC]) != crc16_ccitt(in, MET_OFF_CRC)) {
        return false;
    }

    rec->mode = in[MET_OFF_MODE];
    rec->seq  = get_u16(&in[MET_OFF_SEQ]);
    rec->t_us = get_u32(&in[MET_OFF_TIME]);

    for (uint32_t c = 0U; c < DAQ_NUM_CHANNELS; c++) {
        rec->rms[c]      = (int32_t)get_u32(&in[MET_OFF_RMS  + (c * 4U)]);
        rec->thdn_mdb[c] = (int32_t)get_u32(&in[MET_OFF_THDN + (c * 4U)]);
        rec->fund_mhz[c] = get_u32(&in[MET_OFF_FUND + (c * 4U)]);
    }
    rec->psd_serial = get_u32(&in[MET_OFF_PSDSN]);
    return true;
}

/* ---- PSD: 1044 bytes --------------------------------------------------- */
#define PSD_OFF_CHAN   3U
#define PSD_OFF_SEQ    4U
#define PSD_OFF_TIME   6U
#define PSD_OFF_NBINS 10U
#define PSD_OFF_FS    12U
#define PSD_OFF_BINS  16U
#define PSD_OFF_CRC   (PSD_OFF_BINS + (DAQ_PSD_BINS * 2U))

void daq_psd_encode(const daq_psd_rec_t *rec, uint8_t *out)
{
    out[OFF_SYNC0]    = DAQ_SYNC0;
    out[OFF_SYNC1]    = DAQ_SYNC1_PSD;
    out[OFF_VER]      = DAQ_PROTO_VER;
    out[PSD_OFF_CHAN] = rec->channel;

    put_u16(&out[PSD_OFF_SEQ], rec->seq);
    put_u32(&out[PSD_OFF_TIME], rec->t_us);
    put_u16(&out[PSD_OFF_NBINS], (uint16_t)DAQ_PSD_BINS);
    put_u32(&out[PSD_OFF_FS], rec->fs_mhz);

    for (uint32_t k = 0U; k < DAQ_PSD_BINS; k++) {
        put_u16(&out[PSD_OFF_BINS + (k * 2U)], (uint16_t)rec->bin_dbfs_x100[k]);
    }
    put_u16(&out[PSD_OFF_CRC], crc16_ccitt(out, PSD_OFF_CRC));
}

bool daq_psd_decode(const uint8_t *in, daq_psd_rec_t *rec)
{
    if ((in[OFF_SYNC0] != DAQ_SYNC0) || (in[OFF_SYNC1] != DAQ_SYNC1_PSD)) {
        return false;
    }
    if (in[OFF_VER] != DAQ_PROTO_VER) {
        return false;
    }
    if (get_u16(&in[PSD_OFF_NBINS]) != (uint16_t)DAQ_PSD_BINS) {
        return false;
    }
    if (get_u16(&in[PSD_OFF_CRC]) != crc16_ccitt(in, PSD_OFF_CRC)) {
        return false;
    }

    rec->channel = in[PSD_OFF_CHAN];
    rec->seq     = get_u16(&in[PSD_OFF_SEQ]);
    rec->t_us    = get_u32(&in[PSD_OFF_TIME]);
    rec->fs_mhz  = get_u32(&in[PSD_OFF_FS]);

    for (uint32_t k = 0U; k < DAQ_PSD_BINS; k++) {
        rec->bin_dbfs_x100[k] =
            (int16_t)get_u16(&in[PSD_OFF_BINS + (k * 2U)]);
    }
    return true;
}

bool daq_record_resync(const uint8_t *buf, size_t len, size_t *out_off,
                       uint8_t *out_type, uint32_t *out_len)
{
    for (size_t off = 0U; (off + DAQ_PACKET_BYTES) <= len; off++) {
        if (buf[off] != DAQ_SYNC0) {
            continue;
        }

        {
            const uint8_t type = buf[off + 1U];
            const uint32_t rlen = daq_record_length(type);
            bool ok = false;

            if ((rlen == 0U) || ((off + rlen) > len)) {
                continue;
            }

            switch (type) {
            case DAQ_SYNC1_SAMPLE: {
                daq_raw_scan_t scratch;
                ok = daq_packet_decode(&buf[off], &scratch);
                break;
            }
            case DAQ_SYNC1_METRICS: {
                daq_metrics_rec_t scratch;
                ok = daq_metrics_decode(&buf[off], &scratch);
                break;
            }
            case DAQ_SYNC1_PSD:
                /* Validate without materialising a 1 KB struct on the stack -
                   this runs on a task with a 384-word stack. */
                ok = (buf[off + OFF_VER] == DAQ_PROTO_VER) &&
                     (get_u16(&buf[off + PSD_OFF_CRC]) ==
                      crc16_ccitt(&buf[off], PSD_OFF_CRC));
                break;
            default:
                break;
            }

            if (ok) {
                *out_off  = off;
                *out_type = type;
                *out_len  = rlen;
                return true;
            }
        }
    }
    return false;
}
