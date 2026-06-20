/**
 * @file    ads1256.h
 * @brief   TI ADS1256 24-bit delta-sigma ADC driver (SPI, RDATAC-free).
 *
 * Operating mode
 * --------------
 * This driver does *not* use continuous-read (RDATAC) mode, which is the usual
 * first instinct.  RDATAC gives you the highest throughput on a single fixed
 * channel, but it is incompatible with multiplexing: any register write forces
 * SDATAC/RDATAC bracketing, and the mux change still costs a settling
 * conversion.  Instead each DRDY is serviced as:
 *
 *      RDATA -> read 3 bytes -> WREG MUX (next channel) -> SYNC -> WAKEUP
 *
 * which is the sequence TI's datasheet gives for "cycling the input
 * multiplexer".
 *
 * The throughput this actually yields is worth stating precisely, because it
 * is the number most ADS1256 projects get wrong.  SYNC/WAKEUP restarts the
 * decimation filter, and the first fully settled result after a restart takes
 * the filter's settling time - 0.13 ms at DRATE = 30 kSPS, i.e. 4 conversion
 * periods.  So /DRDY arrives every 133 us, not every 33 us: 7500 settled
 * conversions per second in total, which across 4 channels is 1875 complete
 * scans per second.  Quoting the 30 kSPS DRATE setting as the sample rate
 * would be quoting a number the multiplexed system does not deliver.
 *
 * Self-calibration
 * ----------------
 * SELFCAL runs once at init and populates the ADC's own OFC/FSC registers.
 * That handles the converter.  It does *not* handle the input network - the
 * anti-alias filter, the reference divider, the connector - which is what the
 * host-side two-point calibration in calibration.h corrects.  The two are
 * complementary and both are needed; using only one is the common mistake.
 */
#ifndef ADS1256_H
#define ADS1256_H

#include "daq_types.h"

/* ---- Commands (datasheet Table 24) ------------------------------------- */
#define ADS_CMD_WAKEUP   0x00U
#define ADS_CMD_RDATA    0x01U
#define ADS_CMD_RDATAC   0x03U
#define ADS_CMD_SDATAC   0x0FU
#define ADS_CMD_RREG     0x10U   /* | reg,  then (n-1) */
#define ADS_CMD_WREG     0x50U   /* | reg,  then (n-1), then data */
#define ADS_CMD_SELFCAL  0xF0U
#define ADS_CMD_SELFOCAL 0xF1U
#define ADS_CMD_SELFGCAL 0xF2U
#define ADS_CMD_SYSOCAL  0xF3U
#define ADS_CMD_SYSGCAL  0xF4U
#define ADS_CMD_SYNC     0xFCU
#define ADS_CMD_STANDBY  0xFDU
#define ADS_CMD_RESET    0xFEU

/* ---- Registers ---------------------------------------------------------- */
#define ADS_REG_STATUS   0x00U
#define ADS_REG_MUX      0x01U
#define ADS_REG_ADCON    0x02U
#define ADS_REG_DRATE    0x03U
#define ADS_REG_IO       0x04U
#define ADS_REG_OFC0     0x05U
#define ADS_REG_FSC0     0x08U

/* STATUS: order MSB-first, auto-cal on, analogue input buffer on. */
#define ADS_STATUS_BUFEN  (1U << 1)
#define ADS_STATUS_ACAL   (1U << 2)
#define ADS_STATUS_ORDER  (1U << 3)

/* DRATE encodings actually used. */
#define ADS_DRATE_30000SPS 0xF0U
#define ADS_DRATE_15000SPS 0xE0U
#define ADS_DRATE_1000SPS  0xA1U
#define ADS_DRATE_100SPS   0x82U

/* ADCON: clock out off (it is a noise source), sensor detect off, PGA = 1. */
#define ADS_ADCON_PGA1     0x00U
#define ADS_ADCON_CLKOUT_OFF 0x00U

typedef enum {
    ADS_OK = 0,
    ADS_ERR_NO_DRDY,        /**< /DRDY never asserted - clock or wiring       */
    ADS_ERR_ID_MISMATCH,    /**< STATUS.ID did not read back as 3            */
    ADS_ERR_REG_READBACK,   /**< a written register did not read back        */
    ADS_ERR_CAL_TIMEOUT,    /**< SELFCAL did not complete                    */
} ads_status_t;

/**
 * @brief Hardware reset, configure, self-calibrate, and arm the first scan.
 * @return ADS_OK, or the first failure encountered.
 *
 * Every step is verified by read-back.  A converter that is simply not
 * connected otherwise looks identical to one returning legitimate zeros, and
 * "my readings are all zero" is a far more expensive bug to find at the
 * analysis stage than at init.
 */
ads_status_t ads1256_init(void);

/**
 * @brief Read the pending conversion and point the mux at the next channel.
 *
 * Called from the sampler task, once per /DRDY.  Runs the
 * RDATA / WREG MUX / SYNC / WAKEUP sequence described above.
 *
 * @param out_code receives the sign-extended 24-bit result for the channel
 *                 that was selected when this conversion started
 * @param next_ch  channel index to select for the following conversion
 */
void ads1256_read_and_advance(int32_t *out_code, uint32_t next_ch);

/** @brief Read one register (used by init read-back and the bring-up path). */
uint8_t ads1256_read_reg(uint8_t reg);

/** @brief Write one register. */
void ads1256_write_reg(uint8_t reg, uint8_t value);

/** @brief Enable/disable the /DRDY EXTI line. */
void ads1256_drdy_irq_enable(bool enable);

/** @brief MUX register value that selects differential pair @p ch. */
uint8_t ads1256_mux_for_channel(uint32_t ch);

#endif /* ADS1256_H */
