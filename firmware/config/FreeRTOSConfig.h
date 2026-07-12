/**
 * @file FreeRTOSConfig.h
 * @brief Kernel configuration.  Every non-default choice here is deliberate.
 *
 * The two that matter most:
 *
 * configSUPPORT_DYNAMIC_ALLOCATION = 0
 *     No heap_*.c is compiled into the image at all.  Every task, queue, timer
 *     and buffer is statically allocated.  This is not stylistic: it makes
 *     out-of-memory a *link-time* error instead of a runtime one, removes
 *     allocation-order-dependent fragmentation entirely, and means the worst-
 *     case RAM footprint is the linker map, not an empirical observation.
 *
 * configMAX_SYSCALL_INTERRUPT_PRIORITY = 5 << 4
 *     The STM32F4 implements the upper 4 bits of the 8-bit NVIC priority
 *     field, so a "priority 5" interrupt is register value 0x50.  Any ISR that
 *     calls a FromISR() API must be at numeric priority >= 5 (i.e. equal or
 *     less urgent).  The /DRDY handler sits exactly at 5 - the most urgent
 *     level from which notifying a task is legal.  Interrupts more urgent than
 *     this are never masked by the kernel and may not call kernel APIs; none
 *     are used here, but the headroom exists deliberately.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

/* ---- Scheduler ---------------------------------------------------------- */
#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1   /* CLZ-based, O(1)       */
#define configUSE_TICKLESS_IDLE                 0
#define configCPU_CLOCK_HZ                      (168000000UL)
#define configTICK_RATE_HZ                      (1000U)
#define configMAX_PRIORITIES                    (6)
#define configMINIMAL_STACK_SIZE                (128U)   /* words */
#define configMAX_TASK_NAME_LEN                 (12)
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1

/* ---- Memory: static only ------------------------------------------------ */
#define configSUPPORT_STATIC_ALLOCATION         1
#define configSUPPORT_DYNAMIC_ALLOCATION        0
#define configAPPLICATION_ALLOCATED_HEAP        0

/* ---- Features actually used -------------------------------------------- */
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_TASK_NOTIFICATIONS            1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES   1
#define configUSE_QUEUE_SETS                    0
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               (1)
#define configTIMER_QUEUE_LENGTH                4
#define configTIMER_TASK_STACK_DEPTH            (192U)
#define configQUEUE_REGISTRY_SIZE               4

/* ---- Diagnostics --------------------------------------------------------
 * Stack overflow checking mode 2 writes a known pattern over the whole stack
 * at creation and verifies the last 20 bytes on every context switch.  It
 * costs ~25 cycles per switch, which at 1 kHz plus preemptions is under 0.1 %
 * of the CPU - a price worth paying permanently, not just in debug builds.
 * The measured stack high-water marks in docs/measured-results.md come from
 * uxTaskGetStackHighWaterMark(), which is only meaningful because of this.
 */
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_MALLOC_FAILED_HOOK            0   /* no heap to fail       */
#define configUSE_IDLE_HOOK                     1
#define configUSE_TICK_HOOK                     0
#define configUSE_TRACE_FACILITY                1
#define configUSE_STATS_FORMATTING_FUNCTIONS    0   /* saves ~1.2 KB flash   */
#define configRECORD_STACK_HIGH_ADDRESS         1

/* ---- Run-time statistics ------------------------------------------------
 * Driven by TIM2 (free-running, 1 MHz, 32-bit).  Not SysTick: at a 1 ms tick
 * the resolution would be far too coarse to see a task that runs for 58 us
 * every 133 us, which is precisely the task whose load needs measuring.
 */
#define configGENERATE_RUN_TIME_STATS           1
extern uint32_t runtime_stats_counter(void);
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS()  /* TIM2 started in main() */
#define portGET_RUN_TIME_COUNTER_VALUE()        runtime_stats_counter()

/* ---- Cortex-M4F interrupt priorities ------------------------------------ */
#define configPRIO_BITS                         4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY         15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY    5
#define configKERNEL_INTERRUPT_PRIORITY \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* ---- API surface: trim what is not used to keep the image small --------- */
#define INCLUDE_vTaskPrioritySet                0
#define INCLUDE_uxTaskPriorityGet               0
#define INCLUDE_vTaskDelete                     0
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetIdleTaskHandle          1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xTaskGetCurrentTaskHandle       1

/* ---- Asserts ------------------------------------------------------------
 * configASSERT is live in release builds too.  A silently-wrong scheduler is
 * worse than a halted one, and the kernel's asserts catch exactly the class of
 * bug (illegal ISR priority, FromISR from the wrong context) that is otherwise
 * intermittent and untraceable.
 */
void vAssertCalled(const char *file, int line);
#define configASSERT(x) do { if ((x) == 0) { vAssertCalled(__FILE__, __LINE__); } } while (0)

/* Map kernel handlers straight onto the vector table names. */
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

#endif /* FREERTOS_CONFIG_H */
