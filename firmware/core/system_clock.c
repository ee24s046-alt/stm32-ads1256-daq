/**
 * @file system_clock.c
 * @brief SystemInit() - 168 MHz SYSCLK / 48 MHz USB from an 8 MHz HSE.
 *
 * Called from Reset_Handler before main().  The ordering below is not
 * cosmetic; each step is a precondition for the next:
 *
 *   1. Scale the core voltage to VOS = Scale 1.  Above 144 MHz the regulator
 *      must be at Scale 1 *before* the PLL is switched in, or the part runs
 *      out of spec and behaves like a flaky part rather than failing cleanly.
 *   2. Program flash latency to 5 wait states *before* raising the clock.
 *      Raising the clock first means fetching instructions too fast for the
 *      flash array, which manifests as a hard fault on a random instruction.
 *   3. Set the APB prescalers *before* the switch.  APB1 max is 42 MHz; at the
 *      reset value of /1 it would momentarily see 168 MHz.
 *   4. Only then switch SW to PLL and wait for SWS to confirm.
 *
 * Every wait loop is bounded.  An unbounded `while (!(RCC->CR & HSERDY))` on a
 * board with a dead crystal hangs before main() with no output at all, which
 * is the least debuggable failure mode available.  Here, a timeout falls back
 * to the 16 MHz HSI and sets a flag so main() can report it over UART.
 */
#include "board.h"
#include <stdbool.h>

/** Non-zero if the HSE failed and the part is running on HSI. */
volatile uint32_t g_clock_fallback_hsi = 0U;

#define RCC_TIMEOUT_LOOPS 0x00100000UL

void SystemInit(void);

static bool wait_flag(const volatile uint32_t *reg, uint32_t mask, bool want_set)
{
    for (uint32_t i = 0U; i < RCC_TIMEOUT_LOOPS; i++) {
        const bool is_set = ((*reg & mask) != 0U);
        if (is_set == want_set) {
            return true;
        }
    }
    return false;
}

void SystemInit(void)
{
    /* --- 1. Regulator to Scale 1 ---------------------------------------- */
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    (void)RCC->APB1ENR;
    PWR->CR |= PWR_CR_VOS;

    /* --- HSE --------------------------------------------------------------
     * The Discovery board feeds 8 MHz from the ST-LINK's MCO, which is a
     * square-wave clock rather than a crystal, so HSEBYP must be set.  On a
     * board with a real crystal, clear it.
     */
    RCC->CR |= RCC_CR_HSEBYP;
    RCC->CR |= RCC_CR_HSEON;
    if (!wait_flag(&RCC->CR, RCC_CR_HSERDY, true)) {
        g_clock_fallback_hsi = 1U;
        return;                      /* stay on 16 MHz HSI; main() reports it */
    }

    /* --- 2. Flash: 5 WS + caches ---------------------------------------- */
    FLASH->ACR = FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN |
                 FLASH_ACR_LATENCY_5WS;

    /* --- 3. Bus prescalers before the switch ----------------------------- */
    RCC->CFGR = RCC_CFGR_HPRE_DIV1 | RCC_CFGR_PPRE1_DIV4 | RCC_CFGR_PPRE2_DIV2;

    /* --- PLL: M=8 N=336 P=2 Q=7, source HSE ------------------------------ */
    RCC->CR &= ~RCC_CR_PLLON;
    (void)wait_flag(&RCC->CR, RCC_CR_PLLRDY, false);

    RCC->PLLCFGR = (PLL_M << 0) |
                   (PLL_N << 6) |
                   (((PLL_P >> 1U) - 1U) << 16) |
                   (1UL << 22) |               /* PLLSRC = HSE */
                   (PLL_Q << 24);

    RCC->CR |= RCC_CR_PLLON;
    if (!wait_flag(&RCC->CR, RCC_CR_PLLRDY, true)) {
        g_clock_fallback_hsi = 2U;
        return;
    }

    /* --- 4. Switch and confirm ------------------------------------------ */
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW_Msk) | RCC_CFGR_SW_PLL;
    for (uint32_t i = 0U; i < RCC_TIMEOUT_LOOPS; i++) {
        if ((RCC->CFGR & RCC_CFGR_SWS_Msk) == RCC_CFGR_SWS_PLL) {
            return;
        }
    }
    g_clock_fallback_hsi = 3U;
}
