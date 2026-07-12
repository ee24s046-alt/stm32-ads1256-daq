/**
 * @file fault_handler.c
 * @brief Fault handlers that print a diagnosis instead of silently resetting.
 *
 * The default behaviour of most projects - `while(1);` in HardFault_Handler,
 * or worse, a watchdog reset - throws away the only evidence that exists at
 * the moment the bug is reproducible.  What this does instead:
 *
 *   1. Recovers the stack frame the core pushed (MSP or PSP, selected by bit 2
 *      of EXC_RETURN in LR) so the faulting PC and LR are recoverable.
 *   2. Decodes CFSR / HFSR into English, and prints MMFAR / BFAR *only when
 *      the corresponding VALID bit is set* - those registers hold stale
 *      garbage otherwise, and reporting them unconditionally has sent more
 *      than one person chasing an address that was never accessed.
 *   3. Names the most likely cause for the common patterns (IMPRECISERR,
 *      UNSTKERR, INVSTATE, NOCP) rather than only dumping hex.
 *   4. Emits everything over the polled UART, which needs no interrupts, no
 *      RTOS and no heap - all three of which may be exactly what is broken.
 *
 * The handler is naked and tail-calls a C function so the compiler cannot
 * insert a prologue that would clobber the stack frame being inspected.
 */
#include "stm32f407.h"
#include "uart_debug.h"

typedef struct {
    uint32_t r0, r1, r2, r3, r12, lr, pc, psr;
} exception_frame_t;

void HardFault_Handler(void)   __attribute__((naked));
void MemManage_Handler(void)   __attribute__((naked));
void BusFault_Handler(void)    __attribute__((naked));
void UsageFault_Handler(void)  __attribute__((naked));
void fault_report(const exception_frame_t *frame, uint32_t exc_return,
                  uint32_t exc_number);

static void report_bit(uint32_t cfsr, uint32_t mask, const char *text)
{
    if ((cfsr & mask) != 0U) {
        uart_puts("    - ");
        uart_puts(text);
        uart_puts("\n");
    }
}

void fault_report(const exception_frame_t *frame, uint32_t exc_return,
                  uint32_t exc_number)
{
    const uint32_t cfsr = SCB->CFSR;
    const uint32_t hfsr = SCB->HFSR;

    uart_puts("\n\n================ FAULT ================\n");

    uart_puts("exception : ");
    switch (exc_number) {
    case 3U:  uart_puts("HardFault");   break;
    case 4U:  uart_puts("MemManage");   break;
    case 5U:  uart_puts("BusFault");    break;
    case 6U:  uart_puts("UsageFault");  break;
    default:  uart_puts("unknown");     break;
    }
    uart_puts("\n");

    uart_puts("mode      : ");
    uart_puts(((exc_return & 0x4UL) != 0U) ? "thread (PSP) - faulted in a task"
                                           : "handler (MSP) - faulted in an ISR");
    uart_puts("\n");

    uart_puts("PC        : "); uart_put_hex32(frame->pc);  uart_puts("\n");
    uart_puts("LR        : "); uart_put_hex32(frame->lr);  uart_puts("\n");
    uart_puts("xPSR      : "); uart_put_hex32(frame->psr); uart_puts("\n");
    uart_puts("R0-R3,R12 : ");
    uart_put_hex32(frame->r0);  uart_putc(' ');
    uart_put_hex32(frame->r1);  uart_putc(' ');
    uart_put_hex32(frame->r2);  uart_putc(' ');
    uart_put_hex32(frame->r3);  uart_putc(' ');
    uart_put_hex32(frame->r12); uart_puts("\n");

    uart_puts("CFSR      : "); uart_put_hex32(cfsr); uart_puts("\n");
    uart_puts("HFSR      : "); uart_put_hex32(hfsr); uart_puts("\n");

    if ((hfsr & HFSR_VECTTBL) != 0U) {
        uart_puts("    - vector table read error (VTOR wrong?)\n");
    }
    if ((hfsr & HFSR_FORCED) != 0U) {
        uart_puts("    - escalated from a disabled configurable fault\n");
    }

    /* --- MemManage ----------------------------------------------------- */
    report_bit(cfsr, CFSR_IACCVIOL,  "MMU: instruction fetch from a no-execute region");
    report_bit(cfsr, CFSR_DACCVIOL,  "MMU: data access violation");
    report_bit(cfsr, CFSR_MUNSTKERR, "MMU: fault while unstacking on exception return");
    report_bit(cfsr, CFSR_MSTKERR,   "MMU: fault while stacking on exception entry");
    report_bit(cfsr, CFSR_MLSPERR,   "MMU: fault during lazy FP state preservation");

    /* --- BusFault ------------------------------------------------------- */
    report_bit(cfsr, CFSR_IBUSERR,     "BUS: instruction prefetch error");
    report_bit(cfsr, CFSR_PRECISERR,   "BUS: precise data bus error (BFAR is the address)");
    report_bit(cfsr, CFSR_IMPRECISERR, "BUS: IMPRECISE data bus error - a buffered write "
                                       "failed; the PC above is NOT the culprit");
    report_bit(cfsr, CFSR_UNSTKERR,    "BUS: fault unstacking - classic stack overflow signature");
    report_bit(cfsr, CFSR_STKERR,      "BUS: fault stacking - classic stack overflow signature");
    report_bit(cfsr, CFSR_LSPERR,      "BUS: fault during lazy FP state preservation");

    /* --- UsageFault ----------------------------------------------------- */
    report_bit(cfsr, CFSR_UNDEFINSTR, "USAGE: undefined instruction");
    report_bit(cfsr, CFSR_INVSTATE,   "USAGE: invalid EPSR state - branched to an address "
                                      "with bit 0 clear (Thumb bit lost)");
    report_bit(cfsr, CFSR_INVPC,      "USAGE: invalid PC load on exception return");
    report_bit(cfsr, CFSR_NOCP,       "USAGE: coprocessor absent - FPU used before CPACR enabled");
    report_bit(cfsr, CFSR_UNALIGNED,  "USAGE: unaligned access with UNALIGN_TRP set");
    report_bit(cfsr, CFSR_DIVBYZERO,  "USAGE: divide by zero with DIV_0_TRP set");

    if ((cfsr & CFSR_MMARVALID) != 0U) {
        uart_puts("MMFAR     : "); uart_put_hex32(SCB->MMFAR);
        uart_puts("   (valid)\n");
    }
    if ((cfsr & CFSR_BFARVALID) != 0U) {
        uart_puts("BFAR      : "); uart_put_hex32(SCB->BFAR);
        uart_puts("   (valid)\n");
    }
    if ((cfsr & (CFSR_MMARVALID | CFSR_BFARVALID)) == 0U) {
        uart_puts("MMFAR/BFAR: not valid - contents are stale, ignore them\n");
    }

    uart_puts("=======================================\n");
    uart_puts("halted. attach a debugger, or power cycle.\n");

    /* Deliberately do NOT reset.  A reset here would destroy the RAM state a
       debugger could still inspect, and an auto-resetting device that faults
       once a minute looks healthy from the outside. */
    for (;;) { }
}

/*
 * Naked trampolines.  r0 = frame pointer, r1 = EXC_RETURN, r2 = exception
 * number from IPSR.  "b" rather than "bl": there is nothing to return to.
 */
#define FAULT_TRAMPOLINE()                       \
    __asm volatile (                             \
        "tst  lr, #4            \n"              \
        "ite  eq                \n"              \
        "mrseq r0, msp          \n"              \
        "mrsne r0, psp          \n"              \
        "mov  r1, lr            \n"              \
        "mrs  r2, ipsr          \n"              \
        "b    fault_report      \n")

void HardFault_Handler(void)  { FAULT_TRAMPOLINE(); }
void MemManage_Handler(void)  { FAULT_TRAMPOLINE(); }
void BusFault_Handler(void)   { FAULT_TRAMPOLINE(); }
void UsageFault_Handler(void) { FAULT_TRAMPOLINE(); }
