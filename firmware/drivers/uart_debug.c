#include "uart_debug.h"
#include "board.h"
#include "gpio.h"

void uart_debug_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
    (void)RCC->APB1ENR;

    gpio_config(PIN_UART_TX_PORT, PIN_UART_TX, GPIO_MODE_AF,
                GPIO_PULL_UP, GPIO_SPEED_HIGH, GPIO_AF7_USART2);
    gpio_config(PIN_UART_RX_PORT, PIN_UART_RX, GPIO_MODE_AF,
                GPIO_PULL_UP, GPIO_SPEED_HIGH, GPIO_AF7_USART2);

    /* Oversampling by 16, integer+fraction in one field: BRR = fck / baud.
       42e6 / 921600 = 45.57 -> 45 + 9/16, i.e. 0x2D9.  The rounding here is
       -1.25 % of a bit period, well inside the +/-2 % a UART tolerates. */
    USART2->BRR = (APB1_HZ + (UART_DEBUG_BAUD / 2U)) / UART_DEBUG_BAUD;
    USART2->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

void uart_putc(char c)
{
    while ((USART2->SR & USART_SR_TXE) == 0U) { }
    USART2->DR = (uint32_t)(uint8_t)c;
}

void uart_puts(const char *s)
{
    while (*s != '\0') {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s);
        s++;
    }
}

void uart_put_hex32(uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    uart_puts("0x");
    for (int32_t shift = 28; shift >= 0; shift -= 4) {
        uart_putc(hex[(v >> (uint32_t)shift) & 0xFU]);
    }
}

void uart_put_u32(uint32_t v)
{
    char buf[11];
    uint32_t i = 0U;

    if (v == 0U) {
        uart_putc('0');
        return;
    }
    while (v != 0U) {
        buf[i] = (char)('0' + (char)(v % 10U));
        v /= 10U;
        i++;
    }
    while (i != 0U) {
        i--;
        uart_putc(buf[i]);
    }
}

void uart_put_i32(int32_t v)
{
    if (v < 0) {
        uart_putc('-');
        /* Negate in uint32 space: -INT32_MIN is undefined in int32. */
        uart_put_u32((uint32_t)(-(int64_t)v));
    } else {
        uart_put_u32((uint32_t)v);
    }
}
