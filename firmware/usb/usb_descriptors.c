/**
 * @file usb_descriptors.c
 * @brief Hand-built USB descriptors for a single CDC-ACM function.
 *
 * The VID:PID pair below is the pid.codes test allocation (0x1209/0x0001),
 * which exists precisely so that hobby and research devices do not squat on
 * somebody else's vendor ID.  Anything shipping to more than a bench should
 * obtain its own PID from pid.codes - it is free.
 *
 * The serial-number string is derived at runtime from the STM32's 96-bit
 * unique device ID.  That matters more than it looks: with a fixed serial,
 * plugging two of these boards into the same Windows host makes the second one
 * inherit the first's COM port assignment and driver state, and the resulting
 * "the second board doesn't work" bug is deeply unfun to diagnose.
 */
#include "usb_device.h"
#include <string.h>

#define UID_BASE 0x1FFF7A10UL

#define USB_DESC_TYPE_DEVICE 1U
#define USB_DESC_TYPE_CONFIG 2U
#define USB_DESC_TYPE_STRING 3U

/* ---- Device ------------------------------------------------------------- */
static const uint8_t k_device_desc[18] = {
    18U,                      /* bLength                                    */
    USB_DESC_TYPE_DEVICE,
    0x00U, 0x02U,             /* bcdUSB 2.00                                */
    0x02U,                    /* bDeviceClass: CDC (communications)         */
    0x00U,                    /* bDeviceSubClass                            */
    0x00U,                    /* bDeviceProtocol                            */
    USB_EP0_SIZE,             /* bMaxPacketSize0                            */
    0x09U, 0x12U,             /* idVendor  0x1209 (pid.codes)               */
    0x01U, 0x00U,             /* idProduct 0x0001 (test PID)                */
    0x00U, 0x01U,             /* bcdDevice 1.00                             */
    0x01U,                    /* iManufacturer                              */
    0x02U,                    /* iProduct                                   */
    0x03U,                    /* iSerialNumber                              */
    0x01U,                    /* bNumConfigurations                         */
};

/* ---- Configuration: CDC-ACM (2 interfaces, 3 endpoints) ----------------- */
#define CONFIG_TOTAL_LEN 67U

static const uint8_t k_config_desc[CONFIG_TOTAL_LEN] = {
    /* Configuration */
    9U, USB_DESC_TYPE_CONFIG,
    (uint8_t)(CONFIG_TOTAL_LEN & 0xFFU), (uint8_t)(CONFIG_TOTAL_LEN >> 8),
    0x02U,                    /* bNumInterfaces                             */
    0x01U,                    /* bConfigurationValue                        */
    0x00U,                    /* iConfiguration                             */
    0x80U,                    /* bmAttributes: bus powered, no remote wakeup*/
    0xFAU,                    /* bMaxPower 500 mA                           */

    /* Interface 0: Communications Class, ACM subclass */
    9U, 0x04U, 0x00U, 0x00U, 0x01U, 0x02U, 0x02U, 0x01U, 0x00U,

    /* CDC Header functional descriptor */
    5U, 0x24U, 0x00U, 0x10U, 0x01U,

    /* CDC Call Management: no call management, data on interface 1 */
    5U, 0x24U, 0x01U, 0x00U, 0x01U,

    /* CDC ACM: supports Set_Line_Coding / Set_Control_Line_State */
    4U, 0x24U, 0x02U, 0x02U,

    /* CDC Union: control interface 0, subordinate interface 1 */
    5U, 0x24U, 0x06U, 0x00U, 0x01U,

    /* EP2 IN, interrupt, 8 bytes, 255 ms.  Never actually used to send a
       SERIAL_STATE notification, but omitting it makes several host stacks
       (notably older macOS) refuse the ACM function outright. */
    7U, 0x05U, 0x82U, 0x03U, 0x08U, 0x00U, 0xFFU,

    /* Interface 1: CDC Data */
    9U, 0x04U, 0x01U, 0x00U, 0x02U, 0x0AU, 0x00U, 0x00U, 0x00U,

    /* EP1 OUT, bulk, 64 */
    7U, 0x05U, 0x01U, 0x02U, 0x40U, 0x00U, 0x00U,

    /* EP1 IN, bulk, 64 */
    7U, 0x05U, 0x81U, 0x02U, 0x40U, 0x00U, 0x00U,
};

/* ---- Strings ------------------------------------------------------------ */
static const uint8_t k_lang_desc[4] = { 4U, USB_DESC_TYPE_STRING, 0x09U, 0x04U };

static uint8_t s_string_buf[64];

/** @brief Encode ASCII as a USB UTF-16LE string descriptor. */
static const uint8_t *make_string(const char *ascii, uint16_t *len)
{
    const size_t n = strlen(ascii);
    const size_t total = (n * 2U) + 2U;

    if (total > sizeof(s_string_buf)) {
        *len = 0U;
        return NULL;
    }

    s_string_buf[0] = (uint8_t)total;
    s_string_buf[1] = USB_DESC_TYPE_STRING;
    for (size_t i = 0U; i < n; i++) {
        s_string_buf[2U + (i * 2U)]      = (uint8_t)ascii[i];
        s_string_buf[2U + (i * 2U) + 1U] = 0U;
    }
    *len = (uint16_t)total;
    return s_string_buf;
}

static const uint8_t *make_serial(uint16_t *len)
{
    static const char hex[] = "0123456789ABCDEF";
    const uint32_t *uid = (const uint32_t *)UID_BASE;
    char text[25];

    for (uint32_t w = 0U; w < 3U; w++) {
        for (uint32_t n = 0U; n < 8U; n++) {
            text[(w * 8U) + n] = hex[(uid[w] >> (28U - (n * 4U))) & 0xFU];
        }
    }
    text[24] = '\0';
    return make_string(text, len);
}

const uint8_t *usb_desc_device(uint16_t *len);
const uint8_t *usb_desc_config(uint16_t *len);
const uint8_t *usb_desc_string(uint8_t index, uint16_t *len);

const uint8_t *usb_desc_device(uint16_t *len)
{
    *len = (uint16_t)sizeof(k_device_desc);
    return k_device_desc;
}

const uint8_t *usb_desc_config(uint16_t *len)
{
    *len = (uint16_t)sizeof(k_config_desc);
    return k_config_desc;
}

const uint8_t *usb_desc_string(uint8_t index, uint16_t *len)
{
    switch (index) {
    case 0U:
        *len = (uint16_t)sizeof(k_lang_desc);
        return k_lang_desc;
    case 1U:
        return make_string("IIT Madras / HTIC", len);
    case 2U:
        return make_string("ADS1256 4-Channel Precision DAQ", len);
    case 3U:
        return make_serial(len);
    default:
        *len = 0U;
        return NULL;
    }
}
