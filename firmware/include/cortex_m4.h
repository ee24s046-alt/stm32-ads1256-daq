/**
 * @file    cortex_m4.h
 * @brief   Minimal hand-written Cortex-M4 core definitions.
 *
 * This project uses no vendor HAL and no CMSIS pack.  Everything the firmware
 * touches in the core peripheral block is declared here, from the ARMv7-M
 * Architecture Reference Manual and the Cortex-M4 TRM.  The file is small
 * on purpose: only what is actually used.
 *
 * Rationale: the point of the project is to demonstrate register-level
 * competence.  Pulling in 40k lines of generated headers to reach four
 * registers would defeat that, and it makes the build self-contained.
 */
#ifndef CORTEX_M4_H
#define CORTEX_M4_H

#include <stdint.h>

#define __I  volatile const
#define __O  volatile
#define __IO volatile

/* ------------------------------------------------------------------------ */
/* System Control Block                                                     */
/* ------------------------------------------------------------------------ */
typedef struct {
    __I  uint32_t CPUID;
    __IO uint32_t ICSR;
    __IO uint32_t VTOR;
    __IO uint32_t AIRCR;
    __IO uint32_t SCR;
    __IO uint32_t CCR;
    __IO uint8_t  SHPR[12];   /* SHPR1..3 as a byte array (priority per exc) */
    __IO uint32_t SHCSR;
    __IO uint32_t CFSR;       /* MMFSR:8 | BFSR:8 | UFSR:16                  */
    __IO uint32_t HFSR;
    __IO uint32_t DFSR;
    __IO uint32_t MMFAR;
    __IO uint32_t BFAR;
    __IO uint32_t AFSR;
} SCB_Type;

#define SCS_BASE   (0xE000E000UL)
#define SCB        ((SCB_Type *)(SCS_BASE + 0x0D00UL))

#define SCB_SHCSR_USGFAULTENA (1UL << 18)
#define SCB_SHCSR_BUSFAULTENA (1UL << 17)
#define SCB_SHCSR_MEMFAULTENA (1UL << 16)
#define SCB_CCR_DIV_0_TRP     (1UL << 4)
#define SCB_CCR_UNALIGN_TRP   (1UL << 3)

/* CFSR decode bits - used by the fault handler to print a diagnosis. */
#define CFSR_IACCVIOL   (1UL << 0)
#define CFSR_DACCVIOL   (1UL << 1)
#define CFSR_MUNSTKERR  (1UL << 3)
#define CFSR_MSTKERR    (1UL << 4)
#define CFSR_MLSPERR    (1UL << 5)
#define CFSR_MMARVALID  (1UL << 7)
#define CFSR_IBUSERR    (1UL << 8)
#define CFSR_PRECISERR  (1UL << 9)
#define CFSR_IMPRECISERR (1UL << 10)
#define CFSR_UNSTKERR   (1UL << 11)
#define CFSR_STKERR     (1UL << 12)
#define CFSR_LSPERR     (1UL << 13)
#define CFSR_BFARVALID  (1UL << 15)
#define CFSR_UNDEFINSTR (1UL << 16)
#define CFSR_INVSTATE   (1UL << 17)
#define CFSR_INVPC      (1UL << 18)
#define CFSR_NOCP       (1UL << 19)
#define CFSR_UNALIGNED  (1UL << 24)
#define CFSR_DIVBYZERO  (1UL << 25)

#define HFSR_VECTTBL    (1UL << 1)
#define HFSR_FORCED     (1UL << 30)
#define HFSR_DEBUGEVT   (1UL << 31)

/* ------------------------------------------------------------------------ */
/* NVIC                                                                      */
/* ------------------------------------------------------------------------ */
typedef struct {
    __IO uint32_t ISER[8];
    uint32_t      RES0[24];
    __IO uint32_t ICER[8];
    uint32_t      RES1[24];
    __IO uint32_t ISPR[8];
    uint32_t      RES2[24];
    __IO uint32_t ICPR[8];
    uint32_t      RES3[24];
    __IO uint32_t IABR[8];
    uint32_t      RES4[56];
    __IO uint8_t  IPR[240];
} NVIC_Type;

#define NVIC ((NVIC_Type *)(SCS_BASE + 0x0100UL))

/* ------------------------------------------------------------------------ */
/* Data Watchpoint & Trace - free-running 168 MHz cycle counter.            */
/* This is the timebase for every timing number in docs/timing-budget.md.   */
/* ------------------------------------------------------------------------ */
typedef struct {
    __IO uint32_t CTRL;
    __IO uint32_t CYCCNT;
    __IO uint32_t CPICNT;
    __IO uint32_t EXCCNT;
    __IO uint32_t SLEEPCNT;
    __IO uint32_t LSUCNT;
    __IO uint32_t FOLDCNT;
} DWT_Type;

typedef struct {
    uint32_t      RES0[1];
    __IO uint32_t DEMCR;
} CoreDebug_Type;

#define DWT        ((DWT_Type *)0xE0001000UL)
#define CoreDebug  ((CoreDebug_Type *)0xE000EDF0UL)
#define DWT_CTRL_CYCCNTENA  (1UL << 0)
#define DEMCR_TRCENA        (1UL << 24)

/* ------------------------------------------------------------------------ */
/* Intrinsics                                                                */
/* ------------------------------------------------------------------------ */
#define __DSB() __asm volatile ("dsb 0xF" ::: "memory")
#define __DMB() __asm volatile ("dmb 0xF" ::: "memory")
#define __ISB() __asm volatile ("isb 0xF" ::: "memory")
#define __WFI() __asm volatile ("wfi")
#define __NOP() __asm volatile ("nop")

static inline void __disable_irq(void) { __asm volatile ("cpsid i" ::: "memory"); }
static inline void __enable_irq(void)  { __asm volatile ("cpsie i" ::: "memory"); }

static inline uint32_t __get_IPSR(void)
{
    uint32_t r;
    __asm volatile ("mrs %0, ipsr" : "=r"(r));
    return r;
}

static inline void nvic_enable_irq(int32_t irqn)
{
    NVIC->ISER[((uint32_t)irqn) >> 5U] = (uint32_t)(1UL << (((uint32_t)irqn) & 0x1FU));
}

static inline void nvic_disable_irq(int32_t irqn)
{
    NVIC->ICER[((uint32_t)irqn) >> 5U] = (uint32_t)(1UL << (((uint32_t)irqn) & 0x1FU));
    __DSB();
    __ISB();
}

/**
 * @brief Set an NVIC priority, in the 4-bit-implemented form the STM32F4 uses.
 *
 * The F4 implements the top 4 bits of the 8-bit priority field, so the value
 * must be shifted left by 4.  Getting this wrong is the classic reason an
 * "impossible" configASSERT fires inside the FreeRTOS port: a nominally low
 * priority ends up numerically higher than configMAX_SYSCALL_INTERRUPT_PRIORITY.
 */
static inline void nvic_set_priority(int32_t irqn, uint32_t prio)
{
    if (irqn >= 0) {
        NVIC->IPR[irqn] = (uint8_t)((prio << 4U) & 0xFFU);
    } else {
        SCB->SHPR[((uint32_t)(irqn) & 0xFU) - 4U] = (uint8_t)((prio << 4U) & 0xFFU);
    }
}

static inline void system_reset(void)
{
    __DSB();
    SCB->AIRCR = (0x5FAUL << 16) | (1UL << 2);   /* VECTKEY | SYSRESETREQ */
    __DSB();
    for (;;) { }
}

#endif /* CORTEX_M4_H */
