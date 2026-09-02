/**
 * @file test_packet.c
 * @brief Wire-format tests, including the false-sync case.
 *
 * The golden vector below is checked byte for byte against docs/protocol.md.
 * If someone changes the struct layout or the field order, this test fails
 * before the host decoder silently starts reading the wrong offsets - which is
 * the sort of change that otherwise only shows up as "the data looks weird".
 */
#include "test_harness.h"
#include "packet.h"
#include "crc.h"
#include <limits.h>

/* The firmware spells this THDN_INVALID in metrics.h; repeated here so the
   packet tests do not have to pull in the DSP headers. */
#define THDN_SENTINEL INT32_MIN

static void test_roundtrip(void)
{
    daq_raw_scan_t in  = { { 1, -1, 8388607, -8388608 }, 0x12345678U, 0xBEEFU,
                           DAQ_STATUS_CAL_APPLIED };
    daq_raw_scan_t out;
    uint8_t wire[DAQ_PACKET_BYTES];

    TEST("encode/decode round-trip preserves every field");
    daq_packet_encode(&in, wire);
    CHECK(daq_packet_decode(wire, &out));

    CHECK_EQ(out.seq, in.seq);
    CHECK_EQ(out.t_us, in.t_us);
    CHECK_EQ(out.status, in.status);
    for (int c = 0; c < (int)DAQ_NUM_CHANNELS; c++) {
        CHECK_EQ(out.code[c], in.code[c]);
    }
}

static void test_wire_layout(void)
{
    daq_raw_scan_t in = { { 0x000001, 0x7FFFFF, 0x000000, -1 },
                          0x01020304U, 0x0102U, 0x00U };
    uint8_t w[DAQ_PACKET_BYTES];

    TEST("byte layout matches docs/protocol.md exactly");
    daq_packet_encode(&in, w);

    CHECK_EQ(w[0], 0xA5U);
    CHECK_EQ(w[1], 0x5AU);
    CHECK_EQ(w[2], DAQ_PROTO_VER);
    CHECK_EQ(w[3], 0x00U);
    CHECK_EQ(w[4], 0x02U);              /* seq  little-endian */
    CHECK_EQ(w[5], 0x01U);
    CHECK_EQ(w[6], 0x04U);              /* t_us little-endian */
    CHECK_EQ(w[7], 0x03U);
    CHECK_EQ(w[8], 0x02U);
    CHECK_EQ(w[9], 0x01U);
    /* ch0 = 1 */
    CHECK_EQ(w[10], 0x01U); CHECK_EQ(w[11], 0x00U); CHECK_EQ(w[12], 0x00U);
    /* ch1 = +full scale */
    CHECK_EQ(w[13], 0xFFU); CHECK_EQ(w[14], 0xFFU); CHECK_EQ(w[15], 0x7FU);
    /* ch3 = -1 truncated to 24-bit two's complement */
    CHECK_EQ(w[19], 0xFFU); CHECK_EQ(w[20], 0xFFU); CHECK_EQ(w[21], 0xFFU);

    CHECK_EQ((uint16_t)(w[22] | (w[23] << 8)), crc16_ccitt(w, 22U));
}

static void test_rejects_corruption(void)
{
    daq_raw_scan_t in = { { 5, 6, 7, 8 }, 1000U, 7U, 0U };
    daq_raw_scan_t out;
    uint8_t w[DAQ_PACKET_BYTES];

    TEST("decoder rejects bad sync, bad version and single-bit corruption");

    daq_packet_encode(&in, w);
    w[0] = 0xA4U;
    CHECK(!daq_packet_decode(w, &out));

    daq_packet_encode(&in, w);
    w[2] = 0x02U;
    CHECK(!daq_packet_decode(w, &out));

    /* Every single-bit flip in the payload must be caught by CRC-16. */
    for (int byte = 0; byte < 22; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            daq_packet_encode(&in, w);
            w[byte] ^= (uint8_t)(1U << bit);
            if ((byte < 3) || daq_packet_decode(w, &out)) {
                /* bytes 0..2 are sync/version and are rejected structurally;
                   anything else decoding successfully is a CRC escape. */
                if (byte >= 3) {
                    CHECK(0);
                }
            }
        }
    }
    CHECK(1);
}

static void test_resync(void)
{
    daq_raw_scan_t in = { { 11, 22, 33, 44 }, 55U, 66U, 0U };
    uint8_t stream[64];
    size_t off = 0U;

    TEST("resync finds the frame after arbitrary leading garbage");
    memset(stream, 0x37, sizeof(stream));
    daq_packet_encode(&in, &stream[13]);

    CHECK(daq_packet_resync(stream, sizeof(stream), &off));
    CHECK_EQ(off, 13U);
}

static void test_resync_ignores_false_sync(void)
{
    daq_raw_scan_t in = { { 0, 0, 0, 0 }, 0U, 0U, 0U };
    uint8_t stream[80];
    size_t off = 0U;

    TEST("a sync word occurring inside sample data is not mistaken for a frame");

    memset(stream, 0x00, sizeof(stream));
    /* Plant a bare sync word with plausible-looking bytes after it.  A framer
       that trusts sync alone locks here and never recovers; ours must reject
       it on CRC and keep scanning. */
    stream[4] = 0xA5U;
    stream[5] = 0x5AU;
    stream[6] = DAQ_PROTO_VER;
    for (int i = 7; i < 28; i++) {
        stream[i] = (uint8_t)(i * 7);
    }

    daq_packet_encode(&in, &stream[40]);

    CHECK(daq_packet_resync(stream, sizeof(stream), &off));
    CHECK_EQ(off, 40U);
}

static void test_crc_vectors(void)
{
    const uint8_t check[9] = { '1','2','3','4','5','6','7','8','9' };

    TEST("CRC implementations match their published check values");
    CHECK_EQ(crc16_ccitt(check, 9U), 0x29B1U);     /* CRC-16/CCITT-FALSE */
    CHECK_EQ(crc32_iso(check, 9U), 0xCBF43926UL);  /* CRC-32/ISO-HDLC    */
}

/* ---------------------------------------------------------------------- */
/* Metrics and PSD records                                                 */
/* ---------------------------------------------------------------------- */

static uint8_t s_rec[DAQ_MAX_RECORD_BYTES];

static void test_record_lengths(void)
{
    TEST("record length is implied by the type byte, not carried in a field");
    CHECK_EQ(daq_record_length(DAQ_SYNC1_SAMPLE), DAQ_PACKET_BYTES);
    CHECK_EQ(daq_record_length(DAQ_SYNC1_METRICS), DAQ_METRICS_BYTES);
    CHECK_EQ(daq_record_length(DAQ_SYNC1_PSD), DAQ_PSD_BYTES);
    CHECK_EQ(daq_record_length(0x99U), 0U);      /* unknown type */

    /* A corrupted stream cannot lie about a length it does not carry, and the
       sizes must stay 4-byte aligned so the encoder never stores unaligned. */
    CHECK_EQ(DAQ_PSD_BYTES, 1044U);
    CHECK_EQ(DAQ_PSD_BYTES % 4U, 0U);
    CHECK_EQ(DAQ_METRICS_BYTES % 4U, 0U);
}

static void test_metrics_roundtrip(void)
{
    daq_metrics_rec_t in = {
        { 10, -20, 30, 40 },
        { -62345, THDN_SENTINEL, -12000, THDN_SENTINEL },
        { 1000500U, 0U, 250250U, 0U },
        77U, 0xDEADBEEFU, 1234U, 1U
    };
    daq_metrics_rec_t out;

    TEST("metrics record survives a round trip, sentinel included");
    daq_metrics_encode(&in, s_rec);
    CHECK(daq_metrics_decode(s_rec, &out));

    CHECK_EQ(out.seq, 1234);
    CHECK_EQ(out.t_us, 0xDEADBEEFU);
    CHECK_EQ(out.mode, 1);
    CHECK_EQ(out.psd_serial, 77U);
    for (uint32_t c = 0U; c < DAQ_NUM_CHANNELS; c++) {
        CHECK_EQ(out.rms[c], in.rms[c]);
        CHECK_EQ(out.thdn_mdb[c], in.thdn_mdb[c]);
        CHECK_EQ(out.fund_mhz[c], in.fund_mhz[c]);
    }
    /* INT32_MIN must survive as itself - it is the "no tone" sentinel, and a
       sign-extension slip here would turn "declined to measure" into a
       plausible-looking -2147483 dB. */
    CHECK_EQ(out.thdn_mdb[1], THDN_SENTINEL);
}

static void test_psd_roundtrip(void)
{
    static daq_psd_rec_t in;
    static daq_psd_rec_t out;

    TEST("PSD record survives a round trip across all 513 bins");
    for (uint32_t k = 0U; k < DAQ_PSD_BINS; k++) {
        in.bin_dbfs_x100[k] = (int16_t)(-15000 + (int16_t)(k % 977U));
    }
    in.fs_mhz  = 7500000U;
    in.t_us    = 42U;
    in.seq     = 9U;
    in.channel = 2U;

    daq_psd_encode(&in, s_rec);
    CHECK(daq_psd_decode(s_rec, &out));

    CHECK_EQ(out.channel, 2);
    CHECK_EQ(out.fs_mhz, 7500000U);
    CHECK_EQ(out.seq, 9);
    for (uint32_t k = 0U; k < DAQ_PSD_BINS; k++) {
        CHECK_EQ(out.bin_dbfs_x100[k], in.bin_dbfs_x100[k]);
    }
}

static void test_records_reject_corruption(void)
{
    daq_metrics_rec_t min_ = { { 1, 2, 3, 4 }, { 0, 0, 0, 0 },
                               { 0U, 0U, 0U, 0U }, 0U, 0U, 0U, 0U };
    daq_metrics_rec_t mout;
    static daq_psd_rec_t pin;
    static daq_psd_rec_t pout;

    TEST("both new record types reject bad sync, version and CRC");

    daq_metrics_encode(&min_, s_rec);
    s_rec[1] = DAQ_SYNC1_SAMPLE;                 /* right CRC, wrong type */
    CHECK(!daq_metrics_decode(s_rec, &mout));

    daq_metrics_encode(&min_, s_rec);
    s_rec[2] = 0x02U;
    CHECK(!daq_metrics_decode(s_rec, &mout));

    daq_metrics_encode(&min_, s_rec);
    s_rec[30] ^= 0x01U;
    CHECK(!daq_metrics_decode(s_rec, &mout));

    pin.fs_mhz = 1875000U;
    daq_psd_encode(&pin, s_rec);
    s_rec[600] ^= 0x80U;
    CHECK(!daq_psd_decode(s_rec, &pout));

    /* A plausible length field is not enough: nbins is checked too. */
    daq_psd_encode(&pin, s_rec);
    s_rec[10] = 0x00U;
    s_rec[11] = 0x01U;
    CHECK(!daq_psd_decode(s_rec, &pout));
}

static void test_record_resync_across_types(void)
{
    static uint8_t stream[4096];
    daq_raw_scan_t scan = { { 7, 8, 9, 10 }, 11U, 12U, 0U };
    daq_metrics_rec_t met = { { 5, 5, 5, 5 }, { 0, 0, 0, 0 },
                              { 0U, 0U, 0U, 0U }, 1U, 2U, 3U, 0U };
    static daq_psd_rec_t psd;
    size_t off = 0U;
    uint8_t type = 0U;
    uint32_t len = 0U;

    TEST("resync finds whichever record type comes first after garbage");

    /* Sample record buried in noise. */
    memset(stream, 0xA5, sizeof(stream));        /* worst case: sync everywhere */
    daq_packet_encode(&scan, &stream[100]);
    CHECK(daq_record_resync(stream, sizeof(stream), &off, &type, &len));
    CHECK_EQ(off, 100U);
    CHECK_EQ(type, DAQ_SYNC1_SAMPLE);
    CHECK_EQ(len, DAQ_PACKET_BYTES);

    /* Metrics record. */
    memset(stream, 0x5A, sizeof(stream));
    daq_metrics_encode(&met, &stream[37]);
    CHECK(daq_record_resync(stream, sizeof(stream), &off, &type, &len));
    CHECK_EQ(off, 37U);
    CHECK_EQ(type, DAQ_SYNC1_METRICS);
    CHECK_EQ(len, DAQ_METRICS_BYTES);

    /* PSD record - validated without materialising the 1 KB struct. */
    memset(stream, 0x00, sizeof(stream));
    psd.fs_mhz = 7500000U;
    psd.channel = 3U;
    daq_psd_encode(&psd, &stream[9]);
    CHECK(daq_record_resync(stream, sizeof(stream), &off, &type, &len));
    CHECK_EQ(off, 9U);
    CHECK_EQ(type, DAQ_SYNC1_PSD);
    CHECK_EQ(len, DAQ_PSD_BYTES);

    /* Nothing but noise: must report failure rather than a false lock. */
    memset(stream, 0xA5, sizeof(stream));
    CHECK(!daq_record_resync(stream, sizeof(stream), &off, &type, &len));

    /* A truncated record must not be reported as found. */
    memset(stream, 0x00, sizeof(stream));
    daq_psd_encode(&psd, &stream[0]);
    CHECK(!daq_record_resync(stream, DAQ_PSD_BYTES - 1U, &off, &type, &len));
}

int main(void)
{
    test_roundtrip();
    test_wire_layout();
    test_rejects_corruption();
    test_resync();
    test_resync_ignores_false_sync();
    test_crc_vectors();
    test_record_lengths();
    test_metrics_roundtrip();
    test_psd_roundtrip();
    test_records_reject_corruption();
    test_record_resync_across_types();
    TEST_SUMMARY();
}
