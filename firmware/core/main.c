/**
 * @file main.c
 * @brief Bring-up, static task creation, and the housekeeping task.
 *
 * Boot order is a dependency chain, not a preference:
 *
 *   cycle counter -> UART -> (report clock status) -> TIM2 -> rings ->
 *   calibration from flash -> ADS1256 -> USB -> tasks -> scheduler
 *
 * The UART comes up third because everything after it can fail, and a failure
 * with no way to say so is the worst outcome available.  The ADS1256 is
 * initialised before USB so that a converter fault is reported on a bench
 * terminal rather than being hidden behind a device that never enumerates.
 *
 * Command channel note: the USB bulk IN endpoint carries *only* 24-byte binary
 * packets, never text.  Interleaving human-readable responses into that stream
 * would break the host's framing - the resync scanner would have to distinguish
 * text from sample data, which it cannot do reliably.  Commands therefore
 * arrive on the USB OUT endpoint, and all responses go out on the debug UART.
 * One direction, one format, no ambiguity.
 */
#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "gpio.h"
#include "uart_debug.h"
#include "cycle_counter.h"
#include "ads1256.h"
#include "flash_store.h"
#include "usb_device.h"
#include "usb_cdc.h"
#include "shared.h"

#include <string.h>

extern volatile uint32_t g_clock_fallback_hsi;

/* ---- Static task storage ------------------------------------------------ */
static StaticTask_t s_tcb_sampler, s_tcb_processor, s_tcb_uplink;
static StaticTask_t s_tcb_metrics, s_tcb_house;
static StackType_t  s_stk_sampler[STACK_SAMPLER];
static StackType_t  s_stk_processor[STACK_PROCESSOR];
static StackType_t  s_stk_uplink[STACK_UPLINK];
static StackType_t  s_stk_metrics[STACK_METRICS];
static StackType_t  s_stk_house[STACK_HOUSEKEEP];

static void task_housekeeping(void *arg);

static void leds_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN;
    (void)RCC->AHB1ENR;
    gpio_config(PIN_LED_HEARTBEAT_PORT, PIN_LED_HEARTBEAT, GPIO_MODE_OUTPUT,
                GPIO_PULL_NONE, GPIO_SPEED_LOW, 0U);
    gpio_config(PIN_LED_ERROR_PORT, PIN_LED_ERROR, GPIO_MODE_OUTPUT,
                GPIO_PULL_NONE, GPIO_SPEED_LOW, 0U);
}

static void fault_enable(void)
{
    /* Enable the configurable faults so they arrive at their own handlers with
       a decodable CFSR, instead of escalating to a bare HardFault that only
       says "something escalated". */
    SCB->SHCSR |= SCB_SHCSR_USGFAULTENA | SCB_SHCSR_BUSFAULTENA |
                  SCB_SHCSR_MEMFAULTENA;
    /* Trap divide-by-zero and unaligned access rather than letting them
       produce plausible-looking garbage. */
    SCB->CCR |= SCB_CCR_DIV_0_TRP | SCB_CCR_UNALIGN_TRP;
}

static void banner(void)
{
    uart_puts("\n\n");
    uart_puts("=================================================\n");
    uart_puts(" ADS1256 4-channel precision DAQ\n");
    uart_puts(" STM32F407 @ "); uart_put_u32(SYSCLK_HZ / 1000000U);
    uart_puts(" MHz  |  FreeRTOS, fully static\n");
    uart_puts(" build: " __DATE__ " " __TIME__ "\n");
    uart_puts("=================================================\n");

    if (g_clock_fallback_hsi != 0U) {
        uart_puts("!! HSE FAILED (stage ");
        uart_put_u32(g_clock_fallback_hsi);
        uart_puts(") - running on HSI. USB will NOT enumerate.\n");
    }
}

static void calibration_boot_load(void)
{
    if (flash_store_load(&g_calibration) == FS_OK) {
        g_calibration_from_flash = true;
        uart_puts("[cal ] loaded from flash, serial=");
        uart_put_u32(g_calibration.serial);
        uart_puts(", slots used=");
        uart_put_u32(flash_store_slots_used());
        uart_puts("\n");
    } else {
        daq_cal_set_identity(&g_calibration);
        g_calibration_from_flash = false;
        uart_puts("[cal ] none stored - unity gain, zero offset\n");
    }
}

static void adc_boot(void)
{
    const ads_status_t st = ads1256_init();

    uart_puts("[adc ] ");
    switch (st) {
    case ADS_OK:
        uart_puts("ok, 30 kSPS raw / 7.5 kSPS settled / 1875 scans per s\n");
        break;
    case ADS_ERR_NO_DRDY:
        uart_puts("FAIL: /DRDY never asserted - check CLKIN and wiring\n");
        break;
    case ADS_ERR_ID_MISMATCH:
        uart_puts("FAIL: STATUS.ID mismatch - SPI link or CS wrong\n");
        break;
    case ADS_ERR_REG_READBACK:
        uart_puts("FAIL: register read-back mismatch - SCLK too fast?\n");
        break;
    case ADS_ERR_CAL_TIMEOUT:
        uart_puts("FAIL: SELFCAL timed out\n");
        break;
    default:
        uart_puts("FAIL: unknown\n");
        break;
    }

    if (st != ADS_OK) {
        gpio_set(PIN_LED_ERROR_PORT, PIN_LED_ERROR);
    }
}

int main(void)
{
    cycle_counter_init();
    leds_init();
    uart_debug_init();
    fault_enable();
    banner();

    micros_timer_init();
    daq_shared_init();
    calibration_boot_load();
    adc_boot();
    usb_device_init();
    uart_puts("[usb ] attached, waiting for enumeration\n");

    g_task_sampler = xTaskCreateStatic(task_sampler, "sampler", STACK_SAMPLER,
                                       NULL, PRIO_SAMPLER, s_stk_sampler,
                                       &s_tcb_sampler);
    g_task_processor = xTaskCreateStatic(task_processor, "processor",
                                         STACK_PROCESSOR, NULL, PRIO_PROCESSOR,
                                         s_stk_processor, &s_tcb_processor);
    g_task_uplink = xTaskCreateStatic(task_uplink, "uplink", STACK_UPLINK,
                                      NULL, PRIO_UPLINK, s_stk_uplink,
                                      &s_tcb_uplink);
    g_task_metrics = xTaskCreateStatic(task_metrics, "metrics", STACK_METRICS,
                                       NULL, PRIO_METRICS, s_stk_metrics,
                                       &s_tcb_metrics);
    (void)xTaskCreateStatic(task_housekeeping, "house", STACK_HOUSEKEEP,
                            NULL, PRIO_HOUSEKEEP, s_stk_house, &s_tcb_house);

    configASSERT(g_task_sampler != NULL);
    configASSERT(g_task_processor != NULL);
    configASSERT(g_task_uplink != NULL);
    configASSERT(g_task_metrics != NULL);

    uart_puts("[rtos] starting scheduler\n\n");
    vTaskStartScheduler();

    /* Unreachable: with static allocation there is no "insufficient heap"
       return path.  If control arrives here the port itself failed. */
    uart_puts("!! vTaskStartScheduler returned\n");
    for (;;) { }
}

/* ------------------------------------------------------------------------ */
/* Housekeeping: heartbeat, command parsing, statistics                      */
/* ------------------------------------------------------------------------ */

static void print_stats(void)
{
    uart_puts("\n--- statistics ---------------------------------\n");
    uart_puts("scans acquired   : "); uart_put_u32(g_stats.scans_acquired);  uart_puts("\n");
    uart_puts("scans processed  : "); uart_put_u32(g_stats.scans_processed); uart_puts("\n");
    uart_puts("packets sent     : "); uart_put_u32(g_stats.packets_sent);    uart_puts("\n");
    uart_puts("DRDY interrupts  : "); uart_put_u32(g_stats.drdy_irqs);       uart_puts("\n");
    uart_puts("deadline misses  : "); uart_put_u32(g_stats.deadline_misses); uart_puts("\n");
    uart_puts("usb retries      : "); uart_put_u32(g_stats.usb_retries);     uart_puts("\n");

    uart_puts("ISR latency max  : ");
    uart_put_u32(cyc_to_ns(g_stats.isr_latency_max_cyc)); uart_puts(" ns\n");
    uart_puts("sampler max      : ");
    uart_put_u32(cyc_to_ns(g_stats.sampler_service_max_cyc) / 1000U);
    uart_puts(" us  (budget "); uart_put_u32(DAQ_DRDY_PERIOD_US); uart_puts(" us)\n");
    uart_puts("processor max    : ");
    uart_put_u32(cyc_to_ns(g_stats.processor_service_max_cyc) / 1000U);
    uart_puts(" us\n");

    uart_puts("ring raw  peak   : "); uart_put_u32(spsc_watermark(&g_ring_raw));
    uart_puts(" / "); uart_put_u32(RING_RAW_SLOTS);
    uart_puts("   drops "); uart_put_u32(spsc_drops(&g_ring_raw)); uart_puts("\n");
    uart_puts("ring tx   peak   : "); uart_put_u32(spsc_watermark(&g_ring_tx));
    uart_puts(" / "); uart_put_u32(RING_TX_SLOTS);
    uart_puts("   drops "); uart_put_u32(spsc_drops(&g_ring_tx)); uart_puts("\n");

    uart_puts("--- DSP ----------------------------------------\n");
    uart_puts("mode             : ");
    uart_puts((g_mode == DAQ_MODE_WIDEBAND)
              ? "WIDEBAND  1 ch @ 30 kSPS -> M=4 -> 7.5 kSPS\n"
              : "MUX4      4 ch @ 1875 scans/s\n");
    if (g_mode == DAQ_MODE_WIDEBAND) {
        uart_puts("decimated out    : "); uart_put_u32(g_stats.decim_out);
        uart_puts("\n");
    }
    uart_puts("PSD blocks       : "); uart_put_u32(g_stats.psd_blocks);
    uart_puts("  (1024-pt, "); uart_put_u32(WELCH_SEGMENTS);
    uart_puts(" averages, 50% overlap)\n");
    uart_puts("PSD worst case   : ");
    uart_put_u32(cyc_to_ns(g_stats.psd_service_max_cyc) / 1000U);
    uart_puts(" us per block\n");
    uart_puts("meta sent/dropped: "); uart_put_u32(g_stats.meta_sent);
    uart_puts(" / "); uart_put_u32(g_stats.meta_dropped); uart_puts("\n");

    for (uint32_t c = 0U; c < DAQ_NUM_CHANNELS; c++) {
        if (!g_metrics.rms_valid[c]) {
            continue;
        }
        uart_puts("  ch"); uart_put_u32(c);
        uart_puts("  rms "); uart_put_i32(g_metrics.rms[c]);
        uart_puts(" codes  ("); 
        uart_put_u32((uint32_t)metrics_effective_bits(g_metrics.rms[c]));
        uart_puts(" effective bits)");
        if (g_metrics.thdn_mdb[c] != THDN_INVALID) {
            uart_puts("  THD+N ");
            uart_put_i32(g_metrics.thdn_mdb[c] / 1000);
            uart_puts(" dB @ ");
            uart_put_u32(g_metrics.fund_mhz[c] / 1000U);
            uart_puts(" Hz");
        }
        uart_puts("\n");
    }

    uart_puts("stack free (words, min since boot):\n");
    uart_puts("  sampler   "); uart_put_u32(uxTaskGetStackHighWaterMark(g_task_sampler));
    uart_puts(" / "); uart_put_u32(STACK_SAMPLER); uart_puts("\n");
    uart_puts("  processor "); uart_put_u32(uxTaskGetStackHighWaterMark(g_task_processor));
    uart_puts(" / "); uart_put_u32(STACK_PROCESSOR); uart_puts("\n");
    uart_puts("  uplink    "); uart_put_u32(uxTaskGetStackHighWaterMark(g_task_uplink));
    uart_puts(" / "); uart_put_u32(STACK_UPLINK); uart_puts("\n");
    uart_puts("  metrics   "); uart_put_u32(uxTaskGetStackHighWaterMark(g_task_metrics));
    uart_puts(" / "); uart_put_u32(STACK_METRICS); uart_puts("\n");
    uart_puts("  idle      "); uart_put_u32(uxTaskGetStackHighWaterMark(xTaskGetIdleTaskHandle()));
    uart_puts(" / "); uart_put_u32(configMINIMAL_STACK_SIZE); uart_puts("\n");
    uart_puts("------------------------------------------------\n");
}

/**
 * @brief Zero-calibrate every channel from the live stream.
 *
 * Averages 4096 scans (546 ms) with the inputs shorted and stores the means as
 * the per-channel offsets.  4096 is a power of two so the division is a shift,
 * and it is long enough that the residual of the averaged offset is ~64x below
 * the single-sample noise - i.e. the calibration is not itself a noise source.
 */
static void run_zero_calibration(void)
{
    int64_t accum[DAQ_NUM_CHANNELS] = { 0, 0, 0, 0 };
    uint32_t taken = 0U;
    daq_raw_scan_t scan;
    const bool was_streaming = g_streaming;

    uart_puts("[cal ] zeroing - short all inputs now, averaging 4096 scans\n");
    g_streaming = false;                 /* stop consuming into the tx ring */
    vTaskDelay(pdMS_TO_TICKS(50U));
    spsc_reset(&g_ring_raw);

    while (taken < 4096U) {
        if (spsc_pop(&g_ring_raw, &scan)) {
            for (uint32_t c = 0U; c < DAQ_NUM_CHANNELS; c++) {
                accum[c] += scan.code[c];
            }
            taken++;
        } else {
            vTaskDelay(1U);
        }
    }

    for (uint32_t c = 0U; c < DAQ_NUM_CHANNELS; c++) {
        g_calibration.offset[c] = (int32_t)(accum[c] / 4096);
        uart_puts("  ch"); uart_put_u32(c); uart_puts(" offset = ");
        uart_put_i32(g_calibration.offset[c]); uart_puts(" codes\n");
    }

    g_streaming = was_streaming;
    uart_puts("[cal ] done (not yet persisted - send 'cal save')\n");
}

static void handle_command(const char *cmd)
{
    if (strcmp(cmd, "run") == 0) {
        g_streaming = true;
        uart_puts("[cmd ] streaming ON\n");
    } else if (strcmp(cmd, "stop") == 0) {
        g_streaming = false;
        uart_puts("[cmd ] streaming OFF\n");
    } else if (strcmp(cmd, "stats") == 0) {
        print_stats();
    } else if (strcmp(cmd, "cal zero") == 0) {
        run_zero_calibration();
    } else if (strcmp(cmd, "cal save") == 0) {
        const fs_status_t st = flash_store_save(&g_calibration,
                                                (uint32_t)(xTaskGetTickCount() / 1000U));
        if (st == FS_OK) {
            g_calibration_from_flash = true;
            uart_puts("[cal ] persisted, serial=");
            uart_put_u32(g_calibration.serial);
            uart_puts(", slots used=");
            uart_put_u32(flash_store_slots_used());
            uart_puts("\n");
        } else {
            uart_puts("[cal ] SAVE FAILED, code ");
            uart_put_u32((uint32_t)st);
            uart_puts("\n");
        }
    } else if (strcmp(cmd, "cal reset") == 0) {
        daq_cal_set_identity(&g_calibration);
        g_calibration_from_flash = false;
        uart_puts("[cal ] reset to identity (in RAM only)\n");
    } else if ((strcmp(cmd, "mode mux") == 0) || (strcmp(cmd, "mode wide") == 0)) {
        const daq_mode_t want = (strcmp(cmd, "mode wide") == 0)
                              ? DAQ_MODE_WIDEBAND : DAQ_MODE_MUX4;

        if (want == g_mode) {
            uart_puts("[mode] already in that mode\n");
        } else {
            /* Stop streaming across the switch. The converter is reconfigured
               and the decimator's delay line flushed; packets emitted during
               that window would carry a mode label that no longer matches the
               data behind them. */
            const bool was = g_streaming;
            g_streaming = false;
            vTaskDelay(pdMS_TO_TICKS(20U));

            if (ads1256_set_mode(want) == ADS_OK) {
                g_mode = want;
                vTaskDelay(pdMS_TO_TICKS(20U));
                uart_puts((want == DAQ_MODE_WIDEBAND)
                    ? "[mode] WIDEBAND: 1 channel, 30 kSPS, M=4 decimation "
                      "-> 7.5 kSPS, 3.5 kHz anti-alias corner\n"
                    : "[mode] MUX4: 4 channels, 1875 scans/s\n");
            } else {
                uart_puts("[mode] FAILED - converter did not acknowledge\n");
            }
            g_streaming = was;
        }
    } else if (strcmp(cmd, "help") == 0) {
        uart_puts("commands: run | stop | stats | mode mux | mode wide | "
                  "cal zero | cal save | cal reset | help\n");
    } else if (cmd[0] != '\0') {
        uart_puts("[cmd ] unknown: '");
        uart_puts(cmd);
        uart_puts("' (try 'help')\n");
    }
}

static void task_housekeeping(void *arg)
{
    char line[32];
    uint32_t fill = 0U;
    uint8_t rx[32];
    TickType_t last_stats = 0U;

    (void)arg;

    for (;;) {
        /* Heartbeat: 1 Hz when idle, 5 Hz while streaming.  A glanceable
           indication that the scheduler is alive without attaching anything. */
        gpio_toggle(PIN_LED_HEARTBEAT_PORT, PIN_LED_HEARTBEAT);

        {
            const uint16_t n = usb_cdc_read(rx, (uint16_t)sizeof(rx));
            for (uint16_t i = 0U; i < n; i++) {
                const char c = (char)rx[i];
                if ((c == '\n') || (c == '\r')) {
                    line[fill] = '\0';
                    handle_command(line);
                    fill = 0U;
                } else if (fill < (sizeof(line) - 1U)) {
                    line[fill] = c;
                    fill++;
                } else {
                    fill = 0U;          /* overlong line: discard, resync */
                }
            }
        }

        if ((xTaskGetTickCount() - last_stats) >= pdMS_TO_TICKS(30000U)) {
            last_stats = xTaskGetTickCount();
            print_stats();
        }

        vTaskDelay(pdMS_TO_TICKS(g_streaming ? 100U : 500U));
    }
}
