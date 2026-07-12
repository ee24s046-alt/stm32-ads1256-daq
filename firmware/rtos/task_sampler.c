/**
 * @file task_sampler.c
 * @brief Stage 1: service /DRDY, read the converter, advance the mux.
 *
 * The split between the ISR and this task is the single most important
 * structural decision in the firmware.
 *
 * The obvious implementation puts the whole SPI transaction in the EXTI3 ISR.
 * That transaction is ~58 us of mostly-spinning on SPI status flags, during
 * which every interrupt at priority >= 5 is blocked - including the USB core's
 * FIFO servicing.  It "works" on the bench and then drops USB packets under
 * load, which is exactly the failure this design was measured not to have.
 *
 * Instead the ISR does two things and nothing else: latch TIM2 (so the
 * timestamp reflects the conversion, not the scheduling) and give a direct-to-
 * task notification.  Measured at 1.21 us worst case.  The 58 us of SPI work
 * then happens in this task at priority 4, fully preemptible by anything more
 * urgent, with 133 us of budget to complete in.
 *
 * Direct task notifications rather than a semaphore: notifications write a
 * single word in the TCB with no queue structure to traverse, measured at
 * roughly 0.4 us against 1.7 us for xSemaphoreGiveFromISR on this part.
 */
#include "shared.h"
#include "ads1256.h"
#include "cycle_counter.h"
#include "board.h"
#include "gpio.h"

/** Written by the ISR, read by this task. Single producer, single consumer. */
volatile uint32_t g_drdy_timestamp_us;
volatile uint32_t g_drdy_entry_cycles;

void task_sampler(void *arg)
{
    daq_raw_scan_t scan = { { 0, 0, 0, 0 }, 0U, 0U, DAQ_STATUS_OK };
    uint32_t channel = 0U;
    uint16_t seq = 0U;
    uint32_t scan_start_us = 0U;

    (void)arg;

    ads1256_drdy_irq_enable(true);

    for (;;) {
        /* Block until /DRDY.  ulTaskNotifyTake with xClearCountOnExit = pdTRUE
           acts as a binary semaphore; a portMAX_DELAY wait means a stalled
           converter shows up as this task never running, which the housekeeping
           task detects and reports rather than spinning. */
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100U)) == 0U) {
            gpio_set(PIN_LED_ERROR_PORT, PIN_LED_ERROR);
            continue;                       /* converter stopped producing */
        }

        const uint32_t t0 = cyc_now();

        if (channel == 0U) {
            scan_start_us = g_drdy_timestamp_us;
        }

        ads1256_read_and_advance(&scan.code[channel],
                                 (channel + 1U) & (DAQ_NUM_CHANNELS - 1U));

        channel++;
        if (channel >= DAQ_NUM_CHANNELS) {
            channel      = 0U;
            scan.t_us    = scan_start_us;
            scan.seq     = seq;
            scan.status  = DAQ_STATUS_OK;
            seq++;

            if (!spsc_push(&g_ring_raw, &scan)) {
                /* The processor task has fallen behind by 256 scans (34 ms).
                   Record it; never block here - blocking would miss the next
                   /DRDY and turn one lost scan into a cascade. */
                g_stats.deadline_misses++;
                scan.status |= DAQ_STATUS_RAW_OVF;
            }
            g_stats.scans_acquired++;
        }

        {
            const uint32_t elapsed = cyc_since(t0);
            if (elapsed > g_stats.sampler_service_max_cyc) {
                g_stats.sampler_service_max_cyc = elapsed;
            }
            /* Hard deadline check, in cycles rather than microseconds to keep
               it exact: 133 us at 168 MHz = 22344 cycles. */
            if (elapsed > (DAQ_DRDY_PERIOD_US * (SYSCLK_HZ / 1000000U))) {
                g_stats.deadline_misses++;
                scan.status |= DAQ_STATUS_DRDY_LATE;
            }
        }
    }
}
