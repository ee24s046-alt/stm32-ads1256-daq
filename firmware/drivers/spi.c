#include "spi.h"
#include "board.h"
#include "gpio.h"

void spi1_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
    (void)RCC->APB2ENR;                     /* RCC enable is posted; read back */

    gpio_config(PIN_SPI1_SCK_PORT,  PIN_SPI1_SCK,  GPIO_MODE_AF,
                GPIO_PULL_NONE, GPIO_SPEED_HIGH, GPIO_AF5_SPI1);
    gpio_config(PIN_SPI1_MISO_PORT, PIN_SPI1_MISO, GPIO_MODE_AF,
                GPIO_PULL_NONE, GPIO_SPEED_HIGH, GPIO_AF5_SPI1);
    gpio_config(PIN_SPI1_MOSI_PORT, PIN_SPI1_MOSI, GPIO_MODE_AF,
                GPIO_PULL_NONE, GPIO_SPEED_HIGH, GPIO_AF5_SPI1);

    /* Chip select is a plain GPIO, driven by the ADS1256 driver.  Hardware NSS
       would deassert between bytes, and the ADS1256 requires CS to stay low
       across a whole command-plus-data sequence. */
    gpio_set(PIN_ADC_CS_PORT, PIN_ADC_CS);
    gpio_config(PIN_ADC_CS_PORT, PIN_ADC_CS, GPIO_MODE_OUTPUT,
                GPIO_PULL_NONE, GPIO_SPEED_HIGH, 0U);

    SPI1->CR1 = 0U;
    SPI1->CR1 = SPI_CR1_MSTR
              | (SPI1_BR_DIV64 << SPI_CR1_BR_Pos)
              | SPI_CR1_CPHA          /* CPHA = 1, CPOL = 0 -> SPI mode 1 */
              | SPI_CR1_SSM           /* software slave management        */
              | SPI_CR1_SSI;          /* ...and hold internal NSS high,   */
                                      /* or the peripheral drops out of   */
                                      /* master mode and sets MODF.       */
    SPI1->CR2 = 0U;
    SPI1->CR1 |= SPI_CR1_SPE;
}

uint8_t spi1_xfer(uint8_t tx)
{
    while ((SPI1->SR & SPI_SR_TXE) == 0U) { }
    *(volatile uint8_t *)&SPI1->DR = tx;      /* 8-bit access: writing the   */
                                              /* full word queues two frames */
    while ((SPI1->SR & SPI_SR_RXNE) == 0U) { }
    return *(volatile uint8_t *)&SPI1->DR;
}

void spi1_write(const uint8_t *tx, size_t len)
{
    for (size_t i = 0U; i < len; i++) {
        (void)spi1_xfer(tx[i]);
    }
}

void spi1_read(uint8_t *rx, size_t len)
{
    for (size_t i = 0U; i < len; i++) {
        rx[i] = spi1_xfer(0xFFU);
    }
}
