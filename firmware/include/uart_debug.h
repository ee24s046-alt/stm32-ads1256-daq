/**
 * @file uart_debug.h
 * @brief Polled USART2 diagnostics channel, independent of USB and the RTOS.
 *
 * This is deliberately the dumbest possible driver: no interrupts, no DMA, no
 * queue, no RTOS involvement.  Its job is to still work when everything else
 * has failed - inside the hard fault handler, before the scheduler starts, and
 * after a stack overflow has corrupted the task that owns the USB link.  A
 * diagnostics path that depends on the subsystem it is diagnosing is not a
 * diagnostics path.
 *
 * Also note there is no printf().  newlib's printf pulls in ~10 KB and, more
 * importantly, up to 1 KB of stack for a single %f - inside a fault handler
 * running on an already-suspect stack.  The handful of formatters here use a
 * fixed 12-byte scratch buffer and no recursion.
 */
#ifndef UART_DEBUG_H
#define UART_DEBUG_H

#include <stdint.h>

void uart_debug_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_put_hex32(uint32_t v);
void uart_put_u32(uint32_t v);
void uart_put_i32(int32_t v);

#endif /* UART_DEBUG_H */
