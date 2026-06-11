/**
 * @file    spsc_ring.h
 * @brief   Lock-free single-producer / single-consumer ring buffer.
 *
 * Why not a FreeRTOS queue?
 *
 * xQueueSendFromISR() takes the scheduler's critical section, which masks
 * interrupts up to configMAX_SYSCALL_INTERRUPT_PRIORITY for the duration of
 * the copy and the list walk.  With a 133 us hard deadline on the DRDY ISR,
 * every microsecond of unrelated interrupt latency is a microsecond of margin
 * spent.  A correctly written SPSC ring needs no critical section at all: the
 * producer owns @c head, the consumer owns @c tail, and correctness comes from
 * two release/acquire fences rather than from masking.
 *
 * Contract (violating any of these breaks the lock-freedom, not just the
 * performance):
 *   - Exactly one producer context calls spsc_push().
 *   - Exactly one consumer context calls spsc_pop().
 *   - @c capacity is a power of two.
 *   - Storage is provided by the caller; the ring never allocates.
 *
 * Indices are free-running and masked on use, so "full" is
 * (head - tail) == capacity and unsigned wraparound is well defined and
 * harmless.  This is the standard trick that avoids burning a slot to
 * distinguish full from empty.
 */
#ifndef SPSC_RING_H
#define SPSC_RING_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    uint8_t         *storage;     /**< caller-owned, capacity*elem_size bytes */
    uint32_t         elem_size;
    uint32_t         capacity;    /**< slots, power of two                    */
    uint32_t         mask;        /**< capacity - 1                           */
    volatile uint32_t head;       /**< producer-owned write index             */
    volatile uint32_t tail;       /**< consumer-owned read index              */
    volatile uint32_t drops;      /**< producer-owned overflow counter        */
    volatile uint32_t watermark;  /**< producer-owned peak occupancy          */
} spsc_ring_t;

/**
 * @brief Initialise a ring over caller-supplied storage.
 * @return false if capacity is zero or not a power of two.
 */
bool spsc_init(spsc_ring_t *r, void *storage, uint32_t elem_size, uint32_t capacity);

/**
 * @brief Producer: copy one element in.
 * @return false if the ring is full (the element is discarded and the drop
 *         counter is incremented - the pipeline never blocks the ISR).
 */
bool spsc_push(spsc_ring_t *r, const void *elem);

/**
 * @brief Consumer: copy one element out.
 * @return false if the ring is empty.
 */
bool spsc_pop(spsc_ring_t *r, void *elem);

/** @brief Consumer: copy up to @p max elements out. @return count copied. */
uint32_t spsc_pop_bulk(spsc_ring_t *r, void *dst, uint32_t max);

/** @brief Occupancy, safe to call from either side (may be stale by one). */
uint32_t spsc_count(const spsc_ring_t *r);

/** @brief Free slots, safe to call from either side. */
uint32_t spsc_space(const spsc_ring_t *r);

/** @brief Peak occupancy since init - the number that sizes the buffer. */
uint32_t spsc_watermark(const spsc_ring_t *r);

/** @brief Elements discarded because the ring was full. Should stay 0. */
uint32_t spsc_drops(const spsc_ring_t *r);

/** @brief Consumer-side reset. Not safe while the producer is running. */
void spsc_reset(spsc_ring_t *r);

#endif /* SPSC_RING_H */
