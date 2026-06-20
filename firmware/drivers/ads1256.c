#include "ads1256.h"
#include "board.h"
#include "gpio.h"
#include "spi.h"
#include "cycle_counter.h"
#include "packet.h"

#define CS_LOW()   gpio_clear(PIN_ADC_CS_PORT, PIN_ADC_CS)
#define CS_HIGH()  gpio_set(PIN_ADC_CS_PORT, PIN_ADC_CS)
#define DRDY_LOW() (gpio_read(PIN_ADC_DRDY_PORT, PIN_ADC_DRDY) == 0U)

/*
 * Channel map: four differential pairs, AIN0-AIN1, AIN2-AIN3, AIN4-AIN5,
 * AIN6-AIN7.  The MUX register is {PSEL[3:0], NSEL[3:0]}.
 */
static const uint8_t k_mux_table[DAQ_NUM_CHANNELS] = {
    0x01U,   /* AINP = AIN0, AINN = AIN1 */
    0x23U,   /* AINP = AIN2, AINN = AIN3 */
    0x45U,   /* AINP = AIN4, AINN = AIN5 */
    0x67U,   /* AINP = AIN6, AINN = AIN7 */
};

uint8_t ads1256_mux_for_channel(uint32_t ch)
{
    return k_mux_table[ch & (DAQ_NUM_CHANNELS - 1U)];
}

static bool wait_drdy_low(uint32_t timeout_us)
{
    const uint32_t start = cyc_now();
    const uint32_t limit = timeout_us * (SYSCLK_HZ / 1000000U);

    while (!DRDY_LOW()) {
        if (cyc_since(start) > limit) {
            return false;
        }
    }
    return true;
}

void ads1256_write_reg(uint8_t reg, uint8_t value)
{
    CS_LOW();
    (void)spi1_xfer((uint8_t)(ADS_CMD_WREG | reg));
    (void)spi1_xfer(0x00U);                 /* write 1 register (n-1 = 0) */
    (void)spi1_xfer(value);
    delay_ns(ADS1256_T11_NS);
    CS_HIGH();
}

uint8_t ads1256_read_reg(uint8_t reg)
{
    uint8_t v;

    CS_LOW();
    (void)spi1_xfer((uint8_t)(ADS_CMD_RREG | reg));
    (void)spi1_xfer(0x00U);
    /*
     * t6 again: the ADS1256 needs 50 tCLKIN between the last command byte and
     * the first data byte of an RREG.  Omitting this is the single most common
     * ADS1256 bring-up bug - registers read back as 0x00 or as the previous
     * value, intermittently, and only at higher SCLK rates.
     */
    delay_ns(ADS1256_T6_NS);
    v = spi1_xfer(0xFFU);
    delay_ns(ADS1256_T11_NS);
    CS_HIGH();
    return v;
}

static void send_cmd(uint8_t cmd)
{
    CS_LOW();
    (void)spi1_xfer(cmd);
    delay_ns(ADS1256_T11_NS);
    CS_HIGH();
}

static void drdy_pin_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    (void)RCC->APB2ENR;

    gpio_config(PIN_ADC_DRDY_PORT, PIN_ADC_DRDY, GPIO_MODE_INPUT,
                GPIO_PULL_UP, GPIO_SPEED_VHIGH, 0U);
    gpio_config(PIN_ADC_SYNC_PORT, PIN_ADC_SYNC, GPIO_MODE_OUTPUT,
                GPIO_PULL_NONE, GPIO_SPEED_LOW, 0U);
    gpio_config(PIN_ADC_RESET_PORT, PIN_ADC_RESET, GPIO_MODE_OUTPUT,
                GPIO_PULL_NONE, GPIO_SPEED_LOW, 0U);

    gpio_set(PIN_ADC_SYNC_PORT, PIN_ADC_SYNC);     /* /SYNC-PDWN idle high */

    /* Route EXTI3 to port C, falling edge only. */
    SYSCFG->EXTICR[0] = (SYSCFG->EXTICR[0] & ~(0xFUL << 12)) | (2UL << 12);
    EXTI->FTSR |=  (1UL << PIN_ADC_DRDY);
    EXTI->RTSR &= ~(1UL << PIN_ADC_DRDY);
    EXTI->PR    =  (1UL << PIN_ADC_DRDY);          /* clear any stale latch */

    /*
     * Priority 5 (numerically), i.e. lower urgency than
     * configMAX_SYSCALL_INTERRUPT_PRIORITY = 5 permits... specifically it is
     * equal to it, which is the highest priority from which a FromISR() call
     * is legal.  One step more urgent and vTaskNotifyGiveFromISR() would
     * corrupt the scheduler, and configASSERT would (correctly) fire.
     */
    nvic_set_priority(EXTI3_IRQn, 5U);
}

void ads1256_drdy_irq_enable(bool enable)
{
    if (enable) {
        EXTI->PR   = (1UL << PIN_ADC_DRDY);
        EXTI->IMR |= (1UL << PIN_ADC_DRDY);
        nvic_enable_irq(EXTI3_IRQn);
    } else {
        nvic_disable_irq(EXTI3_IRQn);
        EXTI->IMR &= ~(1UL << PIN_ADC_DRDY);
    }
}

ads_status_t ads1256_init(void)
{
    spi1_init();
    drdy_pin_init();

    /* --- Hardware reset: /RESET low for >= 4 tCLKIN, then settle --------- */
    gpio_clear(PIN_ADC_RESET_PORT, PIN_ADC_RESET);
    delay_us(10U);
    gpio_set(PIN_ADC_RESET_PORT, PIN_ADC_RESET);
    delay_us(50U);

    if (!wait_drdy_low(500000U)) {
        return ADS_ERR_NO_DRDY;        /* no CLKIN, or the part is not there */
    }

    send_cmd(ADS_CMD_SDATAC);          /* leave RDATAC if a reset was missed */
    delay_ns(ADS1256_T11_NS);

    /* --- Identity check --------------------------------------------------
     * STATUS[7:4] is the factory ID and reads 0b0011 on every ADS1256.  If
     * this comes back 0x00 or 0xFF the SPI link is wrong (wiring, CPHA, CS)
     * and there is no point configuring anything.
     */
    {
        const uint8_t status = ads1256_read_reg(ADS_REG_STATUS);
        if (((status >> 4) & 0x0FU) != 0x03U) {
            return ADS_ERR_ID_MISMATCH;
        }
    }

    ads1256_write_reg(ADS_REG_STATUS, ADS_STATUS_ACAL | ADS_STATUS_BUFEN);
    ads1256_write_reg(ADS_REG_ADCON,  ADS_ADCON_CLKOUT_OFF | ADS_ADCON_PGA1);
    ads1256_write_reg(ADS_REG_DRATE,  ADS_DRATE_30000SPS);
    ads1256_write_reg(ADS_REG_IO,     0x00U);
    ads1256_write_reg(ADS_REG_MUX,    ads1256_mux_for_channel(0U));

    /* Verify what actually landed.  Writing a register and trusting it is how
       a marginal SCLK rate turns into six hours of "why is my noise floor
       20 dB high" - a corrupted DRATE write is silent. */
    if (ads1256_read_reg(ADS_REG_DRATE) != ADS_DRATE_30000SPS) {
        return ADS_ERR_REG_READBACK;
    }
    if (ads1256_read_reg(ADS_REG_ADCON) != (ADS_ADCON_CLKOUT_OFF | ADS_ADCON_PGA1)) {
        return ADS_ERR_REG_READBACK;
    }

    /* --- Self-calibration ------------------------------------------------
     * SELFCAL takes up to ~400 ms at low data rates; at 30 kSPS it is far
     * quicker, but the timeout is generous because this runs exactly once.
     */
    send_cmd(ADS_CMD_SELFCAL);
    if (!wait_drdy_low(800000U)) {
        return ADS_ERR_CAL_TIMEOUT;
    }

    send_cmd(ADS_CMD_SYNC);
    send_cmd(ADS_CMD_WAKEUP);
    return ADS_OK;
}

void ads1256_read_and_advance(int32_t *out_code, uint32_t next_ch)
{
    uint8_t rx[3];

    CS_LOW();

    /* RDATA, then t6 before the data phase. */
    (void)spi1_xfer(ADS_CMD_RDATA);
    delay_ns(ADS1256_T6_NS);
    rx[0] = spi1_xfer(0xFFU);
    rx[1] = spi1_xfer(0xFFU);
    rx[2] = spi1_xfer(0xFFU);

    /* Point the mux at the next pair while still inside the same CS window -
       one CS transition instead of three saves ~1.5 us of the 133 us budget
       and, more usefully, removes two places where a glitch can desync the
       command stream. */
    (void)spi1_xfer((uint8_t)(ADS_CMD_WREG | ADS_REG_MUX));
    (void)spi1_xfer(0x00U);
    (void)spi1_xfer(ads1256_mux_for_channel(next_ch));

    /* SYNC + WAKEUP restarts the converter so the next conversion is taken
       with the new mux setting already settled. */
    delay_ns(ADS1256_T11_NS);
    (void)spi1_xfer(ADS_CMD_SYNC);
    delay_ns(ADS1256_T11_NS);
    (void)spi1_xfer(ADS_CMD_WAKEUP);

    CS_HIGH();

    *out_code = daq_sext24(((uint32_t)rx[0] << 16) |
                           ((uint32_t)rx[1] << 8)  |
                            (uint32_t)rx[2]);
}
