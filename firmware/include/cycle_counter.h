/**
 * @file    cycle_counter.h
 * @brief   Two timebases, used for two different jobs.
 *
 *  DWT->CYCCNT  168 MHz, 5.95 ns resolution, wraps every 25.6 s.
 *               Used only for *profiling*: ISR latency, sampler service time,
 *               the numbers in docs/timing-budget.md.  Never used for data
 *               timestamps, because the wrap period is shorter than a typical
 *               acquisition run.
 *
 *  TIM2->CNT    32-bit at 1 MHz, wraps every 71.6 minutes.
 *               The only source of the packet timestamp field.  Read in the
 *               DRDY ISR, so the timestamp reflects when the conversion
 *               finished, not when a task got round to packetising it.
 *
 * Using one clock for both is the tempting shortcut and it is wrong in both
 * directions: CYCCNT wraps too fast for timestamps, and TIM2's 1 us tick is
 * far too coarse to measure a 1.2 us ISR.
 */
#ifndef CYCLE_COUNTER_H
#define CYCLE_COUNTER_H

#include "stm32f407.h"
#include "board.h"

/** @brief Enable the trace unit and start CYCCNT free-running. */
void cycle_counter_init(void);

/** @brief Start TIM2 as a free-running 1 MHz / 32-bit up-counter. */
void micros_timer_init(void);

static inline uint32_t cyc_now(void)
{
    return DWT->CYCCNT;
}

/** @brief Elapsed cycles since @p start, wrap-safe for intervals < 25.6 s. */
static inline uint32_t cyc_since(uint32_t start)
{
    return DWT->CYCCNT - start;             /* unsigned wrap is correct here */
}

static inline uint32_t cyc_to_ns(uint32_t cycles)
{
    /* 168 MHz -> 1 cycle = 5.952 ns.  x1000/168 keeps this exact in integers
       for any interval up to 25 ms without overflowing 32 bits. */
    return (uint32_t)(((uint64_t)cycles * 1000ULL) / 168ULL);
}

static inline uint32_t micros(void)
{
    return TIM2->CNT;
}

/** @brief Busy-wait, for sub-microsecond device timing (ADS1256 t6/t11). */
static inline void delay_ns(uint32_t ns)
{
    const uint32_t cycles = (uint32_t)(((uint64_t)ns * 168ULL) / 1000ULL);
    const uint32_t start  = DWT->CYCCNT;
    while (cyc_since(start) < cycles) { }
}

static inline void delay_us(uint32_t us)
{
    delay_ns(us * 1000U);
}

#endif /* CYCLE_COUNTER_H */
