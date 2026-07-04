/**
 * @file usb_core.c
 * @brief OTG_FS device core: reset, enumeration, EP0 control, FIFO plumbing.
 */
#include "usb_device.h"
#include "board.h"
#include "gpio.h"
#include "cycle_counter.h"
#include <string.h>

extern const uint8_t *usb_desc_device(uint16_t *len);
extern const uint8_t *usb_desc_config(uint16_t *len);
extern const uint8_t *usb_desc_string(uint8_t index, uint16_t *len);

/* ---- FIFO plan (see usb_device.h) --------------------------------------- */
#define FIFO_RX_WORDS   128U
#define FIFO_TX0_WORDS   64U
#define FIFO_TX1_WORDS   96U
#define FIFO_TX2_WORDS   32U

_Static_assert(FIFO_RX_WORDS + FIFO_TX0_WORDS + FIFO_TX1_WORDS + FIFO_TX2_WORDS
               <= 320U, "OTG_FS FIFO RAM over-committed");

/* ---- Standard requests --------------------------------------------------- */
#define REQ_GET_STATUS        0x00U
#define REQ_CLEAR_FEATURE     0x01U
#define REQ_SET_FEATURE       0x03U
#define REQ_SET_ADDRESS       0x05U
#define REQ_GET_DESCRIPTOR    0x06U
#define REQ_GET_CONFIGURATION 0x08U
#define REQ_SET_CONFIGURATION 0x09U

#define DESC_DEVICE        1U
#define DESC_CONFIGURATION 2U
#define DESC_STRING        3U

static usb_state_t s_state = USB_STATE_DEFAULT;
static usb_setup_t s_setup;
static uint8_t     s_ep0_buf[USB_EP0_SIZE];
static const uint8_t *s_ep0_tx;
static uint16_t    s_ep0_tx_left;
static uint8_t     s_pending_address;
static uint8_t     s_out_buf[USB_CDC_PACKET_SIZE];

/* ------------------------------------------------------------------------ */
/* FIFO access                                                               */
/* ------------------------------------------------------------------------ */
static void fifo_write(uint8_t ep, const uint8_t *src, uint16_t len)
{
    volatile uint32_t *fifo = USB_FIFO(ep);
    const uint16_t words = (uint16_t)((len + 3U) / 4U);

    for (uint16_t i = 0U; i < words; i++) {
        uint32_t w = 0U;
        /* memcpy into a local rather than a cast-and-deref: the staging buffer
           is only guaranteed 1-byte aligned and an unaligned 32-bit load on
           the last partial word would fault with UNALIGN_TRP enabled. */
        const uint16_t remaining = (uint16_t)(len - (i * 4U));
        (void)memcpy(&w, &src[i * 4U], (remaining >= 4U) ? 4U : remaining);
        *fifo = w;
    }
}

static void fifo_read(uint8_t *dst, uint16_t len)
{
    volatile uint32_t *fifo = USB_FIFO(0U);   /* RX FIFO is always at index 0 */
    const uint16_t words = (uint16_t)((len + 3U) / 4U);

    for (uint16_t i = 0U; i < words; i++) {
        const uint32_t w = *fifo;
        const uint16_t remaining = (uint16_t)(len - (i * 4U));
        (void)memcpy(&dst[i * 4U], &w, (remaining >= 4U) ? 4U : remaining);
    }
}

static void fifo_discard(uint16_t len)
{
    volatile uint32_t *fifo = USB_FIFO(0U);
    const uint16_t words = (uint16_t)((len + 3U) / 4U);
    for (uint16_t i = 0U; i < words; i++) {
        (void)*fifo;
    }
}

static void flush_tx_fifo(uint8_t fifo_num)
{
    while ((USB_GLOBAL->GRSTCTL & GRSTCTL_AHBIDL) == 0U) { }
    USB_GLOBAL->GRSTCTL = GRSTCTL_TXFFLSH | ((uint32_t)fifo_num << 6);
    while ((USB_GLOBAL->GRSTCTL & GRSTCTL_TXFFLSH) != 0U) { }
}

static void flush_rx_fifo(void)
{
    while ((USB_GLOBAL->GRSTCTL & GRSTCTL_AHBIDL) == 0U) { }
    USB_GLOBAL->GRSTCTL = GRSTCTL_RXFFLSH;
    while ((USB_GLOBAL->GRSTCTL & GRSTCTL_RXFFLSH) != 0U) { }
}

/* ------------------------------------------------------------------------ */
/* Endpoint control                                                          */
/* ------------------------------------------------------------------------ */
void usb_ep_open_in(uint8_t ep, uint8_t type, uint16_t mps, uint8_t fifo)
{
    USB_DEVICE->DAINTMSK |= (1UL << ep);
    if ((USB_INEP(ep)->CTL & DEPCTL_USBAEP) == 0U) {
        USB_INEP(ep)->CTL |= (uint32_t)mps
                          | ((uint32_t)type << DEPCTL_EPTYP_Pos)
                          | ((uint32_t)fifo << DEPCTL_TXFNUM_Pos)
                          | DEPCTL_SD0PID
                          | DEPCTL_USBAEP;
    }
}

void usb_ep_open_out(uint8_t ep, uint8_t type, uint16_t mps)
{
    USB_DEVICE->DAINTMSK |= (1UL << (ep + 16U));
    if ((USB_OUTEP(ep)->CTL & DEPCTL_USBAEP) == 0U) {
        USB_OUTEP(ep)->CTL |= (uint32_t)mps
                           | ((uint32_t)type << DEPCTL_EPTYP_Pos)
                           | DEPCTL_SD0PID
                           | DEPCTL_USBAEP;
    }
}

void usb_ep_stall(uint8_t ep, bool in)
{
    if (in) {
        USB_INEP(ep)->CTL |= DEPCTL_STALL;
    } else {
        USB_OUTEP(ep)->CTL |= DEPCTL_STALL;
    }
}

uint16_t usb_ep_in_space(uint8_t ep)
{
    return (uint16_t)((USB_INEP(ep)->TXFSTS & 0xFFFFU) * 4U);
}

bool usb_ep_write(uint8_t ep, const uint8_t *data, uint16_t len)
{
    if (len > usb_ep_in_space(ep)) {
        return false;
    }

    /*
     * PKTCNT must be the number of *max-packet-size* packets the transfer will
     * be split into, not 1.  Getting this wrong is subtle: a 384-byte write
     * with PKTCNT=1 transmits the first 64 bytes and then wedges the endpoint
     * with 320 bytes stranded in the FIFO, which looks like an intermittent
     * host problem rather than a firmware one.
     */
    const uint32_t mps = (ep == 0U) ? USB_EP0_SIZE : USB_CDC_PACKET_SIZE;
    const uint32_t pktcnt = (len == 0U) ? 1UL : (((uint32_t)len + mps - 1UL) / mps);

    /* Arm the transfer *before* pushing data.  The core latches the packet
       count and byte count from DIEPTSIZ when EPENA is set; writing the FIFO
       first means the first word can be consumed against a stale size. */
    USB_INEP(ep)->TSIZ = (pktcnt << 19) | (uint32_t)len;
    USB_INEP(ep)->CTL |= DEPCTL_EPENA | DEPCTL_CNAK;

    if (len > 0U) {
        fifo_write(ep, data, len);
    }
    return true;
}

void usb_ep_rearm_out(uint8_t ep)
{
    USB_OUTEP(ep)->TSIZ = (1UL << 19) | (uint32_t)USB_CDC_PACKET_SIZE;
    USB_OUTEP(ep)->CTL |= DEPCTL_EPENA | DEPCTL_CNAK;
}

/* ------------------------------------------------------------------------ */
/* EP0                                                                       */
/* ------------------------------------------------------------------------ */
static void ep0_prepare_setup(void)
{
    /* 3 back-to-back SETUP packets, 24 bytes: the value RM0090 mandates.  A
       smaller count works until the host retries a SETUP, then wedges. */
    USB_OUTEP(0U)->TSIZ = (3UL << 29) | (1UL << 19) | 24UL;
    USB_OUTEP(0U)->CTL |= DEPCTL_EPENA;
}

static void ep0_send_chunk(void)
{
    const uint16_t chunk = (s_ep0_tx_left > USB_EP0_SIZE)
                         ? (uint16_t)USB_EP0_SIZE : s_ep0_tx_left;

    (void)usb_ep_write(0U, s_ep0_tx, chunk);
    s_ep0_tx      += chunk;
    s_ep0_tx_left = (uint16_t)(s_ep0_tx_left - chunk);
}

static void ep0_send(const uint8_t *data, uint16_t len)
{
    if (len > s_setup.wLength) {
        len = s_setup.wLength;   /* never return more than the host asked for */
    }
    s_ep0_tx      = data;
    s_ep0_tx_left = len;
    ep0_send_chunk();
}

static void ep0_send_status(void)
{
    (void)usb_ep_write(0U, NULL, 0U);
}

static void handle_get_descriptor(void)
{
    const uint8_t type  = (uint8_t)(s_setup.wValue >> 8);
    const uint8_t index = (uint8_t)(s_setup.wValue & 0xFFU);
    uint16_t len = 0U;
    const uint8_t *desc = NULL;

    switch (type) {
    case DESC_DEVICE:        desc = usb_desc_device(&len);       break;
    case DESC_CONFIGURATION: desc = usb_desc_config(&len);       break;
    case DESC_STRING:        desc = usb_desc_string(index, &len); break;
    default: break;
    }

    if (desc == NULL) {
        usb_ep_stall(0U, true);
        return;
    }
    ep0_send(desc, len);
}

static void handle_standard_request(void)
{
    switch (s_setup.bRequest) {
    case REQ_GET_DESCRIPTOR:
        handle_get_descriptor();
        break;

    case REQ_SET_ADDRESS:
        /*
         * The address must be programmed *before* the status stage, unlike
         * most USB cores where it is applied after.  The OTG core samples
         * DCFG.DAD when the status IN is transmitted, so doing it in the
         * "textbook" order leaves the device answering on address 0 and the
         * host gives up after three tries.
         */
        s_pending_address = (uint8_t)(s_setup.wValue & 0x7FU);
        USB_DEVICE->DCFG = (USB_DEVICE->DCFG & ~(0x7FUL << 4)) |
                           ((uint32_t)s_pending_address << 4);
        ep0_send_status();
        s_state = (s_pending_address != 0U) ? USB_STATE_ADDRESSED
                                            : USB_STATE_DEFAULT;
        break;

    case REQ_SET_CONFIGURATION:
        s_state = USB_STATE_CONFIGURED;
        usb_cdc_on_configured();
        ep0_send_status();
        break;

    case REQ_GET_CONFIGURATION: {
        static uint8_t cfg = 1U;
        ep0_send(&cfg, 1U);
        break;
    }

    case REQ_GET_STATUS: {
        static const uint8_t status[2] = { 0U, 0U };   /* bus powered, no RWU */
        ep0_send(status, 2U);
        break;
    }

    case REQ_CLEAR_FEATURE:
    case REQ_SET_FEATURE:
        ep0_send_status();
        break;

    default:
        usb_ep_stall(0U, true);
        break;
    }
}

static void handle_setup(void)
{
    const uint8_t type = (uint8_t)((s_setup.bmRequestType >> 5) & 0x03U);

    if (type == 0U) {                     /* standard */
        handle_standard_request();
    } else if (type == 1U) {              /* class - CDC */
        uint8_t *data = NULL;
        uint16_t len  = 0U;

        if (!usb_cdc_setup(&s_setup, &data, &len)) {
            usb_ep_stall(0U, true);
        } else if ((s_setup.bmRequestType & 0x80U) != 0U) {
            ep0_send(data, len);
        } else {
            ep0_send_status();
        }
    } else {
        usb_ep_stall(0U, true);
    }
}

/* ------------------------------------------------------------------------ */
/* Init                                                                      */
/* ------------------------------------------------------------------------ */
static void usb_pins_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    (void)RCC->AHB1ENR;

    gpio_config(PIN_USB_DM_PORT, PIN_USB_DM, GPIO_MODE_AF,
                GPIO_PULL_NONE, GPIO_SPEED_VHIGH, GPIO_AF10_OTG_FS);
    gpio_config(PIN_USB_DP_PORT, PIN_USB_DP, GPIO_MODE_AF,
                GPIO_PULL_NONE, GPIO_SPEED_VHIGH, GPIO_AF10_OTG_FS);
}

void usb_device_init(void)
{
    usb_pins_init();

    RCC->AHB2ENR |= RCC_AHB2ENR_OTGFSEN;
    (void)RCC->AHB2ENR;

    /* Power up the embedded FS PHY and ignore VBUS sensing - the Discovery
       board does not wire VBUS to PA9, and with sensing left enabled the core
       never sees a valid session and simply never enumerates. */
    USB_GLOBAL->GCCFG = GCCFG_PWRDWN | GCCFG_NOVBUSSENS;
    USB_GLOBAL->GUSBCFG = GUSBCFG_PHYSEL | GUSBCFG_FDMOD | (6UL << 10); /* TRDT */

    /* Core soft reset */
    while ((USB_GLOBAL->GRSTCTL & GRSTCTL_AHBIDL) == 0U) { }
    USB_GLOBAL->GRSTCTL |= GRSTCTL_CSRST;
    while ((USB_GLOBAL->GRSTCTL & GRSTCTL_CSRST) != 0U) { }
    delay_us(50U);

    USB_PCGCCTL = 0U;                            /* clocks on */
    USB_DEVICE->DCFG = DCFG_DSPD_FS | (0UL << 4);

    /* FIFO plan.  Sizes are in 32-bit words and the TX start addresses are
       cumulative - an overlap here is silent and shows up as random data
       corruption on whichever endpoint got clipped. */
    USB_GLOBAL->GRXFSIZ   = FIFO_RX_WORDS;
    USB_GLOBAL->DIEPTXF0  = (FIFO_TX0_WORDS << 16) | FIFO_RX_WORDS;
    USB_GLOBAL->DIEPTXF[0] = (FIFO_TX1_WORDS << 16) |
                             (FIFO_RX_WORDS + FIFO_TX0_WORDS);
    USB_GLOBAL->DIEPTXF[1] = (FIFO_TX2_WORDS << 16) |
                             (FIFO_RX_WORDS + FIFO_TX0_WORDS + FIFO_TX1_WORDS);

    flush_tx_fifo(0x10U);   /* 0x10 = all TX FIFOs */
    flush_rx_fifo();

    USB_DEVICE->DIEPMSK  = DIEPINT_XFRC;
    USB_DEVICE->DOEPMSK  = DOEPINT_XFRC | DOEPINT_STUP;
    USB_DEVICE->DAINTMSK = 0x00010001UL;   /* EP0 IN + EP0 OUT */

    USB_GLOBAL->GINTMSK = GINTSTS_USBRST | GINTSTS_ENUMDNE | GINTSTS_RXFLVL |
                          GINTSTS_IEPINT | GINTSTS_OEPINT | GINTSTS_USBSUSP |
                          GINTSTS_WKUPINT;
    USB_GLOBAL->GINTSTS = 0xFFFFFFFFUL;
    USB_GLOBAL->GAHBCFG |= GAHBCFG_GINTMSK;

    USB_DEVICE->DCTL &= ~DCTL_SDIS;        /* attach: pull D+ up */

    /* Priority 6: less urgent than the DRDY line (5).  A USB frame is 1 ms
       and the core FIFOs absorb latency; a missed /DRDY is a lost sample. */
    nvic_set_priority(OTG_FS_IRQn, 6U);
    nvic_enable_irq(OTG_FS_IRQn);
}

usb_state_t usb_device_state(void)
{
    return s_state;
}

/* ------------------------------------------------------------------------ */
/* Interrupt                                                                 */
/* ------------------------------------------------------------------------ */
static void handle_rxflvl(void)
{
    /* Popping GRXSTSP is what advances the RX FIFO; it must happen exactly
       once per packet status entry, and the payload must be drained before
       the next pop or the FIFO desynchronises for good. */
    const uint32_t sts  = USB_GLOBAL->GRXSTSP;
    const uint8_t  ep   = (uint8_t)(sts & 0x0FU);
    const uint16_t cnt  = (uint16_t)((sts >> 4) & 0x7FFU);
    const uint8_t  pkt  = (uint8_t)((sts >> 17) & 0x0FU);

    switch (pkt) {
    case 0x06U:                                   /* SETUP data packet */
        fifo_read((uint8_t *)&s_setup, (cnt > 8U) ? 8U : cnt);
        break;

    case 0x02U:                                   /* OUT data packet */
        if (ep == 0U) {
            fifo_read(s_ep0_buf, (cnt > USB_EP0_SIZE) ? USB_EP0_SIZE : cnt);
        } else if (cnt > 0U) {
            fifo_read(s_out_buf, (cnt > USB_CDC_PACKET_SIZE)
                                 ? USB_CDC_PACKET_SIZE : cnt);
            usb_cdc_on_rx(s_out_buf, cnt);
        } else {
            /* zero-length OUT: nothing to drain */
        }
        break;

    default:
        if (cnt > 0U) {
            fifo_discard(cnt);
        }
        break;
    }
}

static void handle_reset(void)
{
    USB_DEVICE->DCTL &= ~(1UL << 0);             /* clear remote wakeup sig  */
    for (uint8_t i = 0U; i < 4U; i++) {
        USB_INEP(i)->CTL  |= DEPCTL_SNAK;
        USB_OUTEP(i)->CTL |= DEPCTL_SNAK;
    }
    flush_tx_fifo(0x10U);
    flush_rx_fifo();

    USB_DEVICE->DAINTMSK = 0x00010001UL;
    USB_DEVICE->DCFG &= ~(0x7FUL << 4);          /* back to address 0 */
    s_state = USB_STATE_DEFAULT;
}

static void handle_enumdone(void)
{
    /* Full speed confirmed; open EP0 at 64 bytes (MPSIZ encoding 0). */
    USB_INEP(0U)->CTL &= ~0x03UL;
    USB_GLOBAL->GUSBCFG = (USB_GLOBAL->GUSBCFG & ~(0xFUL << 10)) | (6UL << 10);
    ep0_prepare_setup();
}

static void handle_iepint(void)
{
    uint32_t pending = (USB_DEVICE->DAINT & USB_DEVICE->DAINTMSK) & 0xFFFFU;

    for (uint8_t ep = 0U; pending != 0U; ep++, pending >>= 1) {
        if ((pending & 1U) == 0U) {
            continue;
        }
        if ((USB_INEP(ep)->INT & DIEPINT_XFRC) != 0U) {
            USB_INEP(ep)->INT = DIEPINT_XFRC;

            if (ep == 0U) {
                if (s_ep0_tx_left > 0U) {
                    ep0_send_chunk();
                } else {
                    ep0_prepare_setup();
                }
            } else {
                usb_cdc_on_tx_complete();
            }
        }
    }
}

static void handle_oepint(void)
{
    uint32_t pending = ((USB_DEVICE->DAINT & USB_DEVICE->DAINTMSK) >> 16) & 0xFFFFU;

    for (uint8_t ep = 0U; pending != 0U; ep++, pending >>= 1) {
        if ((pending & 1U) == 0U) {
            continue;
        }
        const uint32_t ints = USB_OUTEP(ep)->INT;

        if ((ints & DOEPINT_STUP) != 0U) {
            USB_OUTEP(ep)->INT = DOEPINT_STUP;
            handle_setup();
        }
        if ((ints & DOEPINT_XFRC) != 0U) {
            USB_OUTEP(ep)->INT = DOEPINT_XFRC;
            if (ep == 0U) {
                ep0_prepare_setup();
            } else {
                usb_ep_rearm_out(ep);
            }
        }
    }
}

void usb_device_irq(void)
{
    const uint32_t sts = USB_GLOBAL->GINTSTS & USB_GLOBAL->GINTMSK;

    if ((sts & GINTSTS_RXFLVL) != 0U) {
        /* Mask RXFLVL while draining: it is level-triggered and would
           re-enter continuously until the FIFO is empty. */
        USB_GLOBAL->GINTMSK &= ~GINTSTS_RXFLVL;
        handle_rxflvl();
        USB_GLOBAL->GINTMSK |= GINTSTS_RXFLVL;
    }
    if ((sts & GINTSTS_USBRST) != 0U) {
        USB_GLOBAL->GINTSTS = GINTSTS_USBRST;
        handle_reset();
    }
    if ((sts & GINTSTS_ENUMDNE) != 0U) {
        USB_GLOBAL->GINTSTS = GINTSTS_ENUMDNE;
        handle_enumdone();
    }
    if ((sts & GINTSTS_IEPINT) != 0U) {
        handle_iepint();
    }
    if ((sts & GINTSTS_OEPINT) != 0U) {
        handle_oepint();
    }
    if ((sts & GINTSTS_USBSUSP) != 0U) {
        USB_GLOBAL->GINTSTS = GINTSTS_USBSUSP;
        s_state = USB_STATE_SUSPENDED;
    }
    if ((sts & GINTSTS_WKUPINT) != 0U) {
        USB_GLOBAL->GINTSTS = GINTSTS_WKUPINT;
        s_state = USB_STATE_CONFIGURED;
    }
}
