#ifndef USB_CDC_H
#define USB_CDC_H

#include <stdint.h>
#include <stdbool.h>

/** @brief Open the CDC endpoints. Called when the host sets the configuration. */
void usb_cdc_open(void);

/**
 * @brief Try to queue @p len bytes on the bulk IN endpoint.
 * @return false if a transfer is still in flight or the FIFO cannot hold it.
 *
 * Non-blocking by design: the uplink task must never sit inside a USB wait
 * while the sampler is still filling rings behind it.
 */
bool usb_cdc_write(const uint8_t *data, uint16_t len);

/** @brief true if the bulk IN endpoint can accept a new transfer now. */
bool usb_cdc_tx_ready(void);

/** @brief Pop up to @p max bytes received from the host. @return bytes copied. */
uint16_t usb_cdc_read(uint8_t *dst, uint16_t max);

/** @brief true once the host has selected a configuration and opened the port. */
bool usb_cdc_is_connected(void);

/** @brief Host-asserted DTR, i.e. a terminal actually has the port open. */
bool usb_cdc_dtr(void);

#endif /* USB_CDC_H */
