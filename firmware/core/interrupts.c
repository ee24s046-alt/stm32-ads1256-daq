/**
 * @file interrupts.c
 * @brief The two ISRs this firmware owns.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "board.h"
#include "shared.h"
#include "cycle_counter.h"
#include "usb_device.h"

extern volatile uint32_t g_drdy_timestamp_us;
extern volatile uint32_t g_drdy_entry_cycles;

void EXTI3_IRQHandler(void);
void OTG_FS_IRQHandler(void);

/**
 * @brief ADS1256 /DRDY falling edge.
 *
 * Measured at 1.21 us worst case over a 5-minute run (docs/timing-budget.md).
 * Everything in here is O(1) and branch-free apart from the yield decision.
 *
 * Order matters: the EXTI pending bit is cleared *first*.  Clearing it last
 * leaves a window in which a second edge arriving during the handler is
 * cleared along with the first, and the sample is silently lost - a class of
 * bug that only appears when the system is already under load.
 */
void EXTI3_IRQHandler(void)
{
    BaseType_t higher_priority_woken = pdFALSE;

    EXTI->PR = (1UL << PIN_ADC_DRDY);

    g_drdy_entry_cycles = cyc_now();
    g_drdy_timestamp_us = TIM2->CNT;
    g_stats.drdy_irqs++;

    vTaskNotifyGiveFromISR(g_task_sampler, &higher_priority_woken);

    {
        const uint32_t latency = cyc_since(g_drdy_entry_cycles);
        if (latency > g_stats.isr_latency_max_cyc) {
            g_stats.isr_latency_max_cyc = latency;
        }
    }

    /* Request the context switch on exit rather than letting the sampler wait
       for the next tick - up to 1 ms of the 133 us budget, which would be an
       instant deadline miss. */
    portYIELD_FROM_ISR(higher_priority_woken);
}

void OTG_FS_IRQHandler(void)
{
    usb_device_irq();
}
