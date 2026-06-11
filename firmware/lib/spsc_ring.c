#include "spsc_ring.h"
#include <string.h>

/*
 * Memory ordering.
 *
 * On Cortex-M4 (single core, no store buffer visible to another observer) a
 * plain volatile access happens to work in practice.  It is still wrong to
 * rely on: the compiler is free to hoist the data copy past the index update,
 * and -O2 will do exactly that given the chance.  __atomic_thread_fence() is
 * the portable way to say "the copy must be visible before the index moves",
 * and it costs a DMB on ARM and nothing at all on x86-64 - which is why the
 * same source file is what the native unit tests exercise.
 */
#define RELEASE_FENCE() __atomic_thread_fence(__ATOMIC_RELEASE)
#define ACQUIRE_FENCE() __atomic_thread_fence(__ATOMIC_ACQUIRE)

bool spsc_init(spsc_ring_t *r, void *storage, uint32_t elem_size, uint32_t capacity)
{
    if ((r == NULL) || (storage == NULL) || (elem_size == 0U) || (capacity == 0U)) {
        return false;
    }
    if ((capacity & (capacity - 1U)) != 0U) {
        return false;                       /* not a power of two */
    }

    r->storage   = (uint8_t *)storage;
    r->elem_size = elem_size;
    r->capacity  = capacity;
    r->mask      = capacity - 1U;
    r->head      = 0U;
    r->tail      = 0U;
    r->drops     = 0U;
    r->watermark = 0U;
    return true;
}

bool spsc_push(spsc_ring_t *r, const void *elem)
{
    const uint32_t head = r->head;
    const uint32_t tail = r->tail;          /* snapshot of the consumer index */
    const uint32_t used = head - tail;      /* unsigned wrap is intentional   */

    if (used >= r->capacity) {
        r->drops++;
        return false;
    }

    (void)memcpy(&r->storage[(size_t)(head & r->mask) * r->elem_size],
                 elem, r->elem_size);

    /* The payload must be visible before the consumer can observe the slot. */
    RELEASE_FENCE();
    r->head = head + 1U;

    if ((used + 1U) > r->watermark) {
        r->watermark = used + 1U;
    }
    return true;
}

bool spsc_pop(spsc_ring_t *r, void *elem)
{
    const uint32_t tail = r->tail;
    const uint32_t head = r->head;          /* snapshot of the producer index */

    if (head == tail) {
        return false;
    }

    /* Do not read the payload until the producer's index update is observed. */
    ACQUIRE_FENCE();

    (void)memcpy(elem, &r->storage[(size_t)(tail & r->mask) * r->elem_size],
                 r->elem_size);

    RELEASE_FENCE();                        /* copy out before freeing slot */
    r->tail = tail + 1U;
    return true;
}

uint32_t spsc_pop_bulk(spsc_ring_t *r, void *dst, uint32_t max)
{
    uint32_t tail      = r->tail;
    const uint32_t head = r->head;
    uint32_t avail     = head - tail;
    uint8_t *out       = (uint8_t *)dst;
    uint32_t n;

    if (avail == 0U) {
        return 0U;
    }
    ACQUIRE_FENCE();

    n = (avail < max) ? avail : max;

    /*
     * Copy in at most two memcpy()s: one up to the end of the storage array,
     * one for the wrapped remainder.  Byte-at-a-time would cost roughly 3x
     * here and the uplink task drains up to 42 packets per pass.
     */
    {
        const uint32_t first_idx = tail & r->mask;
        const uint32_t to_end    = r->capacity - first_idx;
        const uint32_t n1        = (n < to_end) ? n : to_end;

        (void)memcpy(out, &r->storage[(size_t)first_idx * r->elem_size],
                     (size_t)n1 * r->elem_size);

        if (n1 < n) {
            (void)memcpy(out + ((size_t)n1 * r->elem_size), r->storage,
                         (size_t)(n - n1) * r->elem_size);
        }
    }

    RELEASE_FENCE();
    tail += n;
    r->tail = tail;
    return n;
}

uint32_t spsc_count(const spsc_ring_t *r)
{
    return r->head - r->tail;
}

uint32_t spsc_space(const spsc_ring_t *r)
{
    return r->capacity - (r->head - r->tail);
}

uint32_t spsc_watermark(const spsc_ring_t *r)
{
    return r->watermark;
}

uint32_t spsc_drops(const spsc_ring_t *r)
{
    return r->drops;
}

void spsc_reset(spsc_ring_t *r)
{
    r->head      = 0U;
    r->tail      = 0U;
    r->drops     = 0U;
    r->watermark = 0U;
    RELEASE_FENCE();
}
