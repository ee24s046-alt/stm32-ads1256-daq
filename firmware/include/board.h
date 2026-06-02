/**
 * @file    board.h
 * @brief   Pin map and clock constants for the STM32F4-Discovery + ADS1256 rig.
 *
 * Every pin here is justified in docs/hardware.md.  The two that are not free
 * choices:
 *
 *   /DRDY on PC3  - must be on an EXTI line that is not shared.  EXTI3 is
 *                   dedicated (EXTI5..15 are muxed onto one vector, which
 *                   would add a source-decode branch to the most timing-
 *                   critical ISR in the system).
 *   PA11/PA12     - fixed by silicon: OTG_FS DM/DP.
 */
#ifndef BOARD_H
#define BOARD_H

#include "stm32f407.h"

/* ---- Clock tree ---------------------------------------------------------
 * HSE 8 MHz (Discovery MCO from the ST-LINK) -> PLL -> 168 MHz SYSCLK.
 *   VCO in  = HSE / M   = 8 / 8   = 1 MHz
 *   VCO out = VCO in* N = 1 * 336 = 336 MHz
 *   SYSCLK  = VCO out/P = 336 / 2 = 168 MHz
 *   USB     = VCO out/Q = 336 / 7 = 48 MHz  <- must be exactly 48 MHz
 *
 * The /7 on Q is the whole reason N is 336 and not, say, 320.  Getting 48.000
 * MHz out of the same VCO that produces 168 MHz constrains the solution to
 * essentially this one point.  A "close enough" USB clock enumerates fine on
 * some hosts and fails CRC on others, which is a miserable bug to chase.
 * ---------------------------------------------------------------------- */
#define HSE_HZ         8000000UL
#define SYSCLK_HZ      168000000UL
#define AHB_HZ         SYSCLK_HZ
#define APB1_HZ        (SYSCLK_HZ / 4U)   /* 42 MHz, max 42 */
#define APB2_HZ        (SYSCLK_HZ / 2U)   /* 84 MHz, max 84 */
#define APB1_TIMER_HZ  (APB1_HZ * 2U)     /* 84 MHz - APB prescaler != 1 */

#define PLL_M 8U
#define PLL_N 336U
#define PLL_P 2U      /* encoded as (P/2)-1 in PLLCFGR */
#define PLL_Q 7U

/* ---- ADS1256 --------------------------------------------------------------
 * CLKIN is a 7.68 MHz can oscillator on the analogue board.  The ADS1256
 * requires SCLK <= CLKIN/4 = 1.92 MHz.  APB2 is 84 MHz, so the usable SPI
 * prescalers are /64 (1.3125 MHz, used) and /128 (656 kHz).  /32 gives
 * 2.625 MHz, which violates the spec - and, being a violation of a *setup*
 * time rather than a hard limit, it mostly works, which is worse.
 * ------------------------------------------------------------------------ */
#define ADS1256_CLKIN_HZ   7680000UL
#define ADS1256_SCLK_HZ    (APB2_HZ / 64U)     /* 1.3125 MHz */
#define SPI1_BR_DIV64      5U                  /* CR1.BR encoding for /64    */

/** t6: DRDY falling edge to first SCLK, 50 * tCLKIN = 6.51 us. */
#define ADS1256_T6_NS      6510U
/** t11: final SCLK of a command to the next command, 4 * tCLKIN = 521 ns. */
#define ADS1256_T11_NS     521U

/* ---- Pin map ----------------------------------------------------------- */
#define PIN_SPI1_SCK_PORT   GPIOA
#define PIN_SPI1_SCK        5U
#define PIN_SPI1_MISO_PORT  GPIOA
#define PIN_SPI1_MISO       6U      /* ADS1256 DOUT */
#define PIN_SPI1_MOSI_PORT  GPIOA
#define PIN_SPI1_MOSI       7U      /* ADS1256 DIN  */
#define GPIO_AF5_SPI1       5U

#define PIN_ADC_CS_PORT     GPIOA
#define PIN_ADC_CS          4U      /* software chip select, active low */
#define PIN_ADC_DRDY_PORT   GPIOC
#define PIN_ADC_DRDY        3U      /* EXTI3, falling edge               */
#define PIN_ADC_SYNC_PORT   GPIOC
#define PIN_ADC_SYNC        2U      /* /SYNC-PDWN                        */
#define PIN_ADC_RESET_PORT  GPIOC
#define PIN_ADC_RESET       1U      /* /RESET                            */

#define PIN_UART_TX_PORT    GPIOA
#define PIN_UART_TX         2U      /* USART2_TX, AF7 - ST-LINK VCP      */
#define PIN_UART_RX_PORT    GPIOA
#define PIN_UART_RX         3U
#define GPIO_AF7_USART2     7U
#define UART_DEBUG_BAUD     921600UL

#define PIN_USB_DM_PORT     GPIOA
#define PIN_USB_DM          11U
#define PIN_USB_DP_PORT     GPIOA
#define PIN_USB_DP          12U
#define GPIO_AF10_OTG_FS    10U

#define PIN_LED_HEARTBEAT_PORT GPIOD
#define PIN_LED_HEARTBEAT      13U  /* orange */
#define PIN_LED_ERROR_PORT     GPIOD
#define PIN_LED_ERROR          14U  /* red    */

#endif /* BOARD_H */
