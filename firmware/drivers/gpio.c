#include "gpio.h"

void gpio_config(GPIO_Type *port, uint32_t pin, gpio_mode_t mode,
                 gpio_pull_t pull, gpio_speed_t speed, uint32_t af)
{
    const uint32_t two_bit_pos = pin * 2U;

    port->MODER   = (port->MODER   & ~(3UL << two_bit_pos)) |
                    ((uint32_t)mode  << two_bit_pos);
    port->PUPDR   = (port->PUPDR   & ~(3UL << two_bit_pos)) |
                    ((uint32_t)pull  << two_bit_pos);
    port->OSPEEDR = (port->OSPEEDR & ~(3UL << two_bit_pos)) |
                    ((uint32_t)speed << two_bit_pos);

    if (mode == GPIO_MODE_AF) {
        const uint32_t idx = pin >> 3U;              /* AFR[0] = pins 0..7 */
        const uint32_t pos = (pin & 7U) * 4U;
        port->AFR[idx] = (port->AFR[idx] & ~(0xFUL << pos)) | (af << pos);
    }
}
