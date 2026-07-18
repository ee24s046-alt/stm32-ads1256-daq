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

int main(void)
{
    test_roundtrip();
    test_wire_layout();
    test_rejects_corruption();
    test_resync();
    test_resync_ignores_false_sync();
    test_crc_vectors();
    TEST_SUMMARY();
}
