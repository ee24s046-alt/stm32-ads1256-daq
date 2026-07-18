/**
 * @file test_spsc_ring.c
 * @brief Unit tests for the lock-free ring that carries every sample.
 *
 * The interesting cases are not "push then pop".  They are:
 *   - behaviour at exactly full, since the firmware relies on push() failing
 *     rather than blocking or corrupting;
 *   - the free-running index wrapping past 2^32, which on a device streaming
 *     7500 scans/s happens after 6.6 days of continuous operation.  A ring
 *     that uses `head % capacity` with signed arithmetic breaks there, and the
 *     failure is a once-per-week data corruption that is essentially
 *     impossible to catch in testing if you have not thought about it.
 *   - bulk drain across the wrap boundary, which is the two-memcpy path.
 */
#include "test_harness.h"
#include "spsc_ring.h"

static void test_init_rejects_bad_geometry(void)
{
    spsc_ring_t r;
    uint32_t storage[16];

    TEST("init rejects non-power-of-two and zero capacity");
    CHECK(!spsc_init(&r, storage, 4U, 0U));
    CHECK(!spsc_init(&r, storage, 4U, 3U));
    CHECK(!spsc_init(&r, storage, 4U, 100U));
    CHECK(spsc_init(&r, storage, 4U, 16U));
    CHECK(spsc_init(&r, storage, 4U, 1U));
}

static void test_fifo_order_and_capacity(void)
{
    spsc_ring_t r;
    uint32_t storage[8];
    uint32_t v;

    TEST("FIFO order, exact capacity, drop accounting");
    CHECK(spsc_init(&r, storage, sizeof(uint32_t), 8U));

    for (uint32_t i = 0U; i < 8U; i++) {
        CHECK(spsc_push(&r, &i));
    }
    CHECK_EQ(spsc_count(&r), 8U);
    CHECK_EQ(spsc_space(&r), 0U);

    /* Full: the 9th push must fail and must be counted, not silently lost. */
    v = 99U;
    CHECK(!spsc_push(&r, &v));
    CHECK_EQ(spsc_drops(&r), 1U);
    CHECK_EQ(spsc_watermark(&r), 8U);

    for (uint32_t i = 0U; i < 8U; i++) {
        CHECK(spsc_pop(&r, &v));
        CHECK_EQ(v, i);                 /* strict FIFO, and 99 never entered */
    }
    CHECK(!spsc_pop(&r, &v));
}

static void test_index_wraparound(void)
{
    spsc_ring_t r;
    uint8_t storage[4];
    uint8_t v;

    TEST("free-running indices wrap past 2^32 without corruption");
    CHECK(spsc_init(&r, storage, 1U, 4U));

    /* Fast-forward both indices to just below the 32-bit boundary.  This is
       legal because the invariant is (head - tail), not their absolute
       values - which is exactly the property being asserted. */
    r.head = 0xFFFFFFFEU;
    r.tail = 0xFFFFFFFEU;

    for (uint32_t i = 0U; i < 1000U; i++) {
        const uint8_t out = (uint8_t)(i & 0xFFU);
        CHECK(spsc_push(&r, &out));
        CHECK(spsc_pop(&r, &v));
        CHECK_EQ(v, out);
    }
    CHECK_EQ(spsc_drops(&r), 0U);
    /* Both indices have now wrapped through zero. */
    CHECK(r.head < 0x1000U);
}

static void test_bulk_drain_across_wrap(void)
{
    spsc_ring_t r;
    uint16_t storage[8];
    uint16_t out[8];

    TEST("pop_bulk stitches the two halves of a wrapped region");
    CHECK(spsc_init(&r, storage, sizeof(uint16_t), 8U));

    /* Advance to slot 6, then fill so the occupied region wraps 6,7,0,1,2. */
    for (uint16_t i = 0U; i < 6U; i++) {
        CHECK(spsc_push(&r, &i));
    }
    for (uint16_t i = 0U; i < 6U; i++) {
        uint16_t tmp;
        CHECK(spsc_pop(&r, &tmp));
    }
    for (uint16_t i = 100U; i < 105U; i++) {
        CHECK(spsc_push(&r, &i));
    }

    CHECK_EQ(spsc_pop_bulk(&r, out, 8U), 5U);
    for (uint16_t i = 0U; i < 5U; i++) {
        CHECK_EQ(out[i], (uint16_t)(100U + i));
    }
    CHECK_EQ(spsc_count(&r), 0U);
}

static void test_bulk_respects_max(void)
{
    spsc_ring_t r;
    uint32_t storage[16];
    uint32_t out[16];

    TEST("pop_bulk honours the caller's max and leaves the remainder");
    CHECK(spsc_init(&r, storage, sizeof(uint32_t), 16U));
    for (uint32_t i = 0U; i < 16U; i++) {
        CHECK(spsc_push(&r, &i));
    }
    CHECK_EQ(spsc_pop_bulk(&r, out, 5U), 5U);
    CHECK_EQ(spsc_count(&r), 11U);
    CHECK_EQ(out[4], 4U);
    CHECK_EQ(spsc_pop_bulk(&r, out, 100U), 11U);
    CHECK_EQ(out[0], 5U);
    CHECK_EQ(spsc_pop_bulk(&r, out, 100U), 0U);
}

int main(void)
{
    test_init_rejects_bad_geometry();
    test_fifo_order_and_capacity();
    test_index_wraparound();
    test_bulk_drain_across_wrap();
    test_bulk_respects_max();
    TEST_SUMMARY();
}
