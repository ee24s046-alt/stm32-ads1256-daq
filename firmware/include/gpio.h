#ifndef DAQ_GPIO_H
#define DAQ_GPIO_H

#include "stm32f407.h"

typedef enum {
    GPIO_MODE_INPUT  = 0U,
    GPIO_MODE_OUTPUT = 1U,
    GPIO_MODE_AF     = 2U,
    GPIO_MODE_ANALOG = 3U,
} gpio_mode_t;

typedef enum {
    GPIO_PULL_NONE = 0U,
    GPIO_PULL_UP   = 1U,
    GPIO_PULL_DOWN = 2U,
} gpio_pull_t;

typedef enum {
    GPIO_SPEED_LOW    = 0U,
    GPIO_SPEED_MEDIUM = 1U,
    GPIO_SPEED_HIGH   = 2U,
    GPIO_SPEED_VHIGH  = 3U,
} gpio_speed_t;

void gpio_config(GPIO_Type *port, uint32_t pin, gpio_mode_t mode,
                 gpio_pull_t pull, gpio_speed_t speed, uint32_t af);

static inline void gpio_set(GPIO_Type *port, uint32_t pin)
{
    port->BSRR = (uint32_t)(1UL << pin);
}

static inline void gpio_clear(GPIO_Type *port, uint32_t pin)
{
    port->BSRR = (uint32_t)(1UL << (pin + 16U));
}

static inline uint32_t gpio_read(const GPIO_Type *port, uint32_t pin)
{
    return (port->IDR >> pin) & 1UL;
}

static inline void gpio_toggle(GPIO_Type *port, uint32_t pin)
{
    /* Read-modify-write on ODR, not BSRR: BSRR has no toggle semantics.  Safe
       here because only the heartbeat task touches these pins. */
    port->ODR ^= (uint32_t)(1UL << pin);
}

#endif /* DAQ_GPIO_H */
