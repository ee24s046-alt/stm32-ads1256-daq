/**
 * @file    usb_device.h
 * @brief   USB 2.0 full-speed device stack for OTG_FS, written to the registers.
 *
 * Scope is deliberately narrow: one configuration, one CDC-ACM function, no
 * DMA, no OTG/host role, no remote wakeup.  That is about 600 lines instead of
 * the ~14k lines of ST's middleware, and every one of them is traceable to
 * RM0090 chapter 35 or to the CDC 1.2 specification.
 *
 * FIFO budget - the OTG_FS core has 1.25 KB (320 x 32-bit words) of dedicated
 * RAM and the device will not enumerate reliably if this is over-committed:
 *
 *      RX  (shared, all OUT endpoints)   128 words   512 B
 *      TX0 (EP0 IN, control)              64 words   256 B
 *      TX1 (EP1 IN, bulk data)            96 words   384 B
 *      TX2 (EP2 IN, CDC notifications)    32 words   128 B
 *      -----------------------------------------------
 *                                        320 words  1280 B  (exactly full)
 *
 * The 96 words on TX1 is the number that matters for throughput: it holds six
 * 64-byte packets, so the uplink task can stage a full 6-packet burst and the
 * core streams them back-to-back across consecutive frames without the task
 * having to wake once per packet.
 */
#ifndef USB_DEVICE_H
#define USB_DEVICE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define USB_EP0_SIZE        64U
#define USB_CDC_IN_EP       1U
#define USB_CDC_OUT_EP      1U
#define USB_CDC_NOTIFY_EP   2U
#define USB_CDC_PACKET_SIZE 64U

/* Standard request recipients / types, from USB 2.0 Table 9-2. */
typedef struct __attribute__((packed)) {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_t;

typedef enum {
    USB_STATE_DEFAULT = 0,
    USB_STATE_ADDRESSED,
    USB_STATE_CONFIGURED,
    USB_STATE_SUSPENDED,
} usb_state_t;

/** @brief Bring up the OTG_FS core in device mode and attach to the bus. */
void usb_device_init(void);

/** @brief OTG_FS interrupt entry point. Called from OTG_FS_IRQHandler. */
void usb_device_irq(void);

/** @brief Current enumeration state. */
usb_state_t usb_device_state(void);

/* ---- Endpoint primitives used by the CDC layer -------------------------- */
void usb_ep_open_in(uint8_t ep, uint8_t type, uint16_t mps, uint8_t fifo);
void usb_ep_open_out(uint8_t ep, uint8_t type, uint16_t mps);
void usb_ep_stall(uint8_t ep, bool in);

/** @brief Queue @p len bytes on IN endpoint @p ep. @return false if no space. */
bool usb_ep_write(uint8_t ep, const uint8_t *data, uint16_t len);

/** @brief Free bytes in the IN FIFO for @p ep. */
uint16_t usb_ep_in_space(uint8_t ep);

/** @brief Re-arm OUT endpoint @p ep for one packet. */
void usb_ep_rearm_out(uint8_t ep);

/* ---- Hooks implemented by the CDC layer --------------------------------- */
bool usb_cdc_setup(const usb_setup_t *req, uint8_t **data, uint16_t *len);
void usb_cdc_on_rx(const uint8_t *data, uint16_t len);
void usb_cdc_on_tx_complete(void);
void usb_cdc_on_configured(void);

#endif /* USB_DEVICE_H */
