#include "cycle_counter.h"

void cycle_counter_init(void)
{
    CoreDebug->DEMCR |= DEMCR_TRCENA;

    /* Some debug probes leave CYCCNT enabled from a previous session and some
       leave the whole DWT powered down.  Toggle explicitly rather than
       assuming either state. */
    DWT->CTRL &= ~DWT_CTRL_CYCCNTENA;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA;
}

void micros_timer_init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    (void)RCC->APB1ENR;

    /*
     * APB1 runs at 42 MHz but its timer clock is doubled to 84 MHz because the
     * APB1 prescaler is not 1 (RM0090 "Clock tree" note).  Forgetting that
     * doubling is the single most common reason an STM32 "1 MHz" timer ticks
     * at 500 kHz - and it would show up here as every timestamp being exactly
     * 2x wrong, which looks like a sample-rate bug rather than a clock bug.
     */
    TIM2->PSC = (uint16_t)((APB1_TIMER_HZ / 1000000U) - 1U);   /* 83 -> 1 MHz */
    TIM2->ARR = 0xFFFFFFFFUL;                                  /* full 32-bit */
    TIM2->CR1 = 0U;
    TIM2->EGR = TIM_EGR_UG;      /* latch PSC immediately */
    TIM2->SR  = 0U;
    TIM2->CNT = 0U;
    TIM2->CR1 = TIM_CR1_CEN;
}
