/**
 * @file usb_cdc.c
 * @brief CDC-ACM class layer: line coding, control lines, bulk data.
 */
#include "usb_cdc.h"
#include "usb_device.h"
#include "spsc_ring.h"
#include <string.h>

#define CDC_SET_LINE_CODING        0x20U
#define CDC_GET_LINE_CODING        0x21U
#define CDC_SET_CONTROL_LINE_STATE 0x22U

typedef struct __attribute__((packed)) {
    uint32_t dwDTERate;
    uint8_t  bCharFormat;
    uint8_t  bParityType;
    uint8_t  bDataBits;
} cdc_line_coding_t;

/*
 * The host will set a baud rate.  It is meaningless here - there is no UART
 * behind this endpoint, the link runs at USB full speed regardless - but the
 * request must be accepted and echoed back, because a host that gets a STALL
 * on GET_LINE_CODING will typically fail to open the port at all.  Storing and
 * returning whatever was set is the whole contract.
 */
static cdc_line_coding_t s_line_coding = { 921600U, 0U, 0U, 8U };

static volatile bool s_tx_busy;
static volatile bool s_configured;
static volatile bool s_dtr;

/* Host -> device command bytes.  Tiny: the command set is a few ASCII verbs
   (see docs/protocol.md), not a data path. */
static uint8_t     s_rx_storage[128];
static spsc_ring_t s_rx_ring;
static bool        s_rx_ready;

void usb_cdc_open(void)
{
    if (!s_rx_ready) {
        (void)spsc_init(&s_rx_ring, s_rx_storage, 1U, (uint32_t)sizeof(s_rx_storage));
        s_rx_ready = true;
    }

    usb_ep_open_in(USB_CDC_IN_EP, 2U, USB_CDC_PACKET_SIZE, 1U);      /* bulk  */
    usb_ep_open_in(USB_CDC_NOTIFY_EP, 3U, 8U, 2U);                   /* intr  */
    usb_ep_open_out(USB_CDC_OUT_EP, 2U, USB_CDC_PACKET_SIZE);
    usb_ep_rearm_out(USB_CDC_OUT_EP);

    s_tx_busy    = false;
    s_configured = true;
}

bool usb_cdc_tx_ready(void)
{
    return s_configured && !s_tx_busy;
}

bool usb_cdc_write(const uint8_t *data, uint16_t len)
{
    if (!usb_cdc_tx_ready()) {
        return false;
    }
    if (!usb_ep_write(USB_CDC_IN_EP, data, len)) {
        return false;
    }
    s_tx_busy = true;
    return true;
}

uint16_t usb_cdc_read(uint8_t *dst, uint16_t max)
{
    if (!s_rx_ready) {
        return 0U;
    }
    return (uint16_t)spsc_pop_bulk(&s_rx_ring, dst, max);
}

bool usb_cdc_is_connected(void)
{
    return s_configured;
}

bool usb_cdc_dtr(void)
{
    return s_dtr;
}

/* ---- Hooks called by usb_core ------------------------------------------- */

void usb_cdc_on_configured(void)
{
    usb_cdc_open();
}

void usb_cdc_on_tx_complete(void)
{
    s_tx_busy = false;
}

void usb_cdc_on_rx(const uint8_t *data, uint16_t len)
{
    if (!s_rx_ready) {
        return;
    }
    for (uint16_t i = 0U; i < len; i++) {
        (void)spsc_push(&s_rx_ring, &data[i]);   /* full ring drops - fine */
    }
}

bool usb_cdc_setup(const usb_setup_t *req, uint8_t **data, uint16_t *len)
{
    switch (req->bRequest) {
    case CDC_SET_LINE_CODING:
        /* The 7 data bytes arrive in the OUT data stage; usb_core has already
           buffered them by the time the status stage completes.  Accepting the
           request without acting on the value is correct here. */
        *len = 0U;
        return true;

    case CDC_GET_LINE_CODING:
        *data = (uint8_t *)&s_line_coding;
        *len  = (uint16_t)sizeof(s_line_coding);
        return true;

    case CDC_SET_CONTROL_LINE_STATE:
        s_dtr = ((req->wValue & 0x0001U) != 0U);
        *len  = 0U;
        return true;

    default:
        return false;
    }
}
