/**
 * @file freertos_hooks.c
 * @brief Static allocation callbacks, stack-overflow hook, assert, idle hook.
 *
 * With configSUPPORT_DYNAMIC_ALLOCATION = 0 the kernel cannot allocate its own
 * idle and timer task stacks, so the application must supply them.  This file
 * is the complete inventory of RTOS-owned RAM in the system - there is nowhere
 * else it can come from, which is the point.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "uart_debug.h"
#include "board.h"
#include "gpio.h"
#include "cycle_counter.h"

/* ---- Idle task ---------------------------------------------------------- */
static StaticTask_t s_idle_tcb;
static StackType_t  s_idle_stack[configMINIMAL_STACK_SIZE];

/* Prototypes for all of these come from the kernel headers and
   FreeRTOSConfig.h; re-declaring them here would only invite a silent
   signature mismatch. */

void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stack,
                                   configSTACK_DEPTH_TYPE *size)
{
    *tcb   = &s_idle_tcb;
    *stack = s_idle_stack;
    *size  = configMINIMAL_STACK_SIZE;
}

/* ---- Timer service task ------------------------------------------------- */
static StaticTask_t s_timer_tcb;
static StackType_t  s_timer_stack[configTIMER_TASK_STACK_DEPTH];

void vApplicationGetTimerTaskMemory(StaticTask_t **tcb, StackType_t **stack,
                                    configSTACK_DEPTH_TYPE *size)
{
    *tcb   = &s_timer_tcb;
    *stack = s_timer_stack;
    *size  = configTIMER_TASK_STACK_DEPTH;
}

/* ---- Diagnostics -------------------------------------------------------- */

/**
 * @brief Stack overflow.
 *
 * Reported over the polled UART, not over USB: by the time this fires the
 * overflowing task has already written past its stack into whatever was next
 * in RAM, and assuming the USB stack's state is intact would be optimistic.
 * Halting rather than resetting preserves the RAM contents for a debugger.
 */
/* cppcheck-suppress constParameterPointer
 * The signature is fixed by FreeRTOS; const-qualifying it would no longer
 * match the prototype the kernel calls through. */
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    __disable_irq();
    uart_puts("\n\n*** STACK OVERFLOW in task: ");
    uart_puts((name != NULL) ? name : "<unnamed>");
    uart_puts(" ***\n");
    uart_puts("Raise its depth in shared.h and re-check the high-water marks.\n");
    gpio_set(PIN_LED_ERROR_PORT, PIN_LED_ERROR);
    for (;;) { }
}

void vAssertCalled(const char *file, int line)
{
    __disable_irq();
    uart_puts("\n\n*** configASSERT failed ***\n  ");
    uart_puts(file);
    uart_puts(":");
    uart_put_u32((uint32_t)line);
    uart_puts("\n");
    gpio_set(PIN_LED_ERROR_PORT, PIN_LED_ERROR);
    for (;;) { }
}

/**
 * @brief Idle hook - WFI.
 *
 * Sleeping in idle is what makes the run-time statistics meaningful: the idle
 * task's share of the TIM2 counter is then genuine headroom rather than a
 * spin loop, and the >30 % idle figure in docs/measured-results.md is CPU that
 * was actually available, not merely unclaimed.
 */
void vApplicationIdleHook(void)
{
    __WFI();
}

/**
 * @brief Free-running counter for configGENERATE_RUN_TIME_STATS.
 *
 * TIM2 at 1 MHz.  32-bit, so it wraps every 71.6 minutes; run-time stats are
 * therefore valid for runs shorter than that, which covers every measurement
 * in the docs.  Longer runs need the host-side accounting instead.
 */
uint32_t runtime_stats_counter(void)
{
    return TIM2->CNT;
}
