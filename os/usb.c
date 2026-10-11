/* usb.c - USB peripheral driver: controller power-up, endpoint 0, descriptors.
 *
 * The TI-84 Plus has a Mentor Graphics MUSBFDRC full-speed OTG controller at
 * ports 80h-AFh (register layout of the MUSB "HDRC" family; see usb.h) plus
 * TI glue logic at ports 4Ah-5Bh.  The calculator is a bus device with one
 * CDC-NCM function (ncm.c).
 *
 * The driver works by polling the CSR registers from the main loop; the
 * interrupt handler only latches the read-to-clear event registers (needed
 * to see the end of a SET_ADDRESS status stage) and wakes the CPU.
 */
#include <string.h>
#include "hw.h"
#include "kernel.h"
#include "usb.h"

uint8_t usb_state;
uint8_t usb_alt;
uint8_t usb_last_error;
uint32_t usb_last_activity;
static uint8_t usb_config;
static uint32_t usb_next_check;

/* ------------------------------------------------------------------ */
/* descriptors                                                         */

#define VID 0x1209              /* pid.codes */
#define PID 0x0001              /* pid.codes test PID - replace before release */

static const uint8_t dev_desc[18] = {
    18, 1, 0x00, 0x02,          /* USB 2.0 */
    0x02, 0x00, 0x00,           /* CDC device class */
    64,                         /* EP0 max packet size */
    VID & 0xFF, VID >> 8, PID & 0xFF, PID >> 8,
    0x00, 0x01,                 /* bcdDevice 1.00 */
    1, 2, 3,                    /* manufacturer, product, serial strings */
    1                           /* one configuration */
};

#define CFG_LEN (9 + 9 + 5 + 5 + 13 + 6 + 7 + 9 + 9 + 7 + 7)
static const uint8_t cfg_desc[CFG_LEN] = {
    9, 2, CFG_LEN & 0xFF, CFG_LEN >> 8, 2, 1, 0, 0xC0, 1,  /* self powered, 2 mA */
    /* interface 0: CDC NCM control */
    9, 4, 0, 0, 1, 0x02, 0x0D, 0x00, 0,
    5, 0x24, 0x00, 0x10, 0x01,                  /* header, CDC 1.10 */
    5, 0x24, 0x06, 0, 1,                        /* union: master 0, slave 1 */
    13, 0x24, 0x0F, 4, 0, 0, 0, 0,              /* ethernet networking, iMACAddress 4 */
        0xEA, 0x05, 0, 0, 0,                    /* max segment 1514, no filters */
    6, 0x24, 0x1A, 0x00, 0x01, 0x00,            /* NCM 1.00, no optional requests */
    7, 5, 0x80 | EP_NOTIFY, 0x03, 16, 0, 32,    /* interrupt IN, 16 bytes, 32 ms */
    /* interface 1 alt 0: data, no endpoints */
    9, 4, 1, 0, 0, 0x0A, 0x00, 0x01, 0,
    /* interface 1 alt 1: data, bulk IN + OUT */
    9, 4, 1, 1, 2, 0x0A, 0x00, 0x01, 0,
    7, 5, 0x80 | EP_BULK, 0x02, EP_BULK_SIZE, 0, 0,
    7, 5, EP_BULK, 0x02, EP_BULK_SIZE, 0, 0,
};

static const char *const strings[] = {
    "lwIP-84+",
    "TI-84 Plus Network",
    "000000000001",
};

/* host-side interface MAC (iMACAddress); the calculator itself uses
 * ncm_dev_mac, which differs in the last byte */
const uint8_t ncm_host_mac[6] = {0x02, 0x84, 0x84, 0x00, 0x00, 0x01};

/* ------------------------------------------------------------------ */
/* endpoint 0                                                          */

enum { EP0_IDLE, EP0_IN, EP0_OUT, EP0_STATUS };
static uint8_t ep0_state;
static uint8_t setup[8];
static uint8_t ep0_buf[64];
static const uint8_t *in_ptr;
static uint16_t in_len;
static uint8_t in_zlp;
static uint16_t out_len, out_pos;
static uint8_t pending_addr;     /* 0x80 | address while waiting for the status stage */
static uint32_t status_since;

#define REQ_STALL 0
#define REQ_NODATA 1
#define REQ_IN 2
#define REQ_OUT 3

static uint8_t string_desc(uint8_t idx)
{
    const char *s;
    char mac[13];
    uint8_t i, n;
    if (idx == 0) {
        ep0_buf[0] = 4;
        ep0_buf[1] = 3;
        ep0_buf[2] = 0x09;
        ep0_buf[3] = 0x04;
        return 4;
    }
    if (idx == 4) {
        for (i = 0; i < 6; i++) {
            mac[2 * i] = "0123456789ABCDEF"[ncm_host_mac[i] >> 4];
            mac[2 * i + 1] = "0123456789ABCDEF"[ncm_host_mac[i] & 15];
        }
        mac[12] = 0;
        s = mac;
    } else if (idx <= 3) {
        s = strings[idx - 1];
    } else {
        return 0;
    }
    n = strlen(s);
    if (n > 30)
        n = 30;
    ep0_buf[0] = 2 + 2 * n;
    ep0_buf[1] = 3;
    for (i = 0; i < n; i++) {
        ep0_buf[2 + 2 * i] = s[i];
        ep0_buf[3 + 2 * i] = 0;
    }
    return ep0_buf[0];
}

static void ep_setup_hw(void)
{
    /* notification endpoint: interrupt IN, 16 bytes */
    port_usb_index = EP_NOTIFY;
    port_usb_txmaxp = 16 / 8;
    port_usb_txcsr2 = 0x20;                 /* mode: TX */
    port_usb_csr0 = TXCSR_CLRDATATOG | TXCSR_FLUSH;
    port_usb_txtype = 0x30 | EP_NOTIFY;
    /* bulk endpoint pair */
    port_usb_index = EP_BULK;
    port_usb_txmaxp = EP_BULK_SIZE / 8;
    port_usb_txcsr2 = 0x00;
    port_usb_csr0 = TXCSR_CLRDATATOG | TXCSR_FLUSH;
    port_usb_rxmaxp = EP_BULK_SIZE / 8;
    port_usb_rxcsr = RXCSR_CLRDATATOG | RXCSR_FLUSH;
    port_usb_rxcsr2 = 0x00;
    port_usb_txtype = 0x20 | EP_BULK;
    port_usb_rxtype = 0x20 | EP_BULK;
    port_usb_index = 0;
}

static uint8_t std_request(void)
{
    uint8_t rt = setup[0], req = setup[1];
    uint16_t val = setup[2] | (setup[3] << 8);
    uint8_t idx = setup[4];

    switch (req) {
    case 0: /* GET_STATUS */
        ep0_buf[0] = (rt & 0x1F) == 0 ? 1 : 0;      /* self powered */
        ep0_buf[1] = 0;
        if ((rt & 0x1F) == 2) {
            port_usb_index = idx & 15;
            if (idx & 0x80)
                ep0_buf[0] = (port_usb_csr0 & TXCSR_SENDSTALL) ? 1 : 0;
            else
                ep0_buf[0] = (port_usb_rxcsr & RXCSR_SENDSTALL) ? 1 : 0;
            port_usb_index = 0;
        }
        in_ptr = ep0_buf;
        in_len = 2;
        return REQ_IN;
    case 1: /* CLEAR_FEATURE */
    case 3: /* SET_FEATURE */
        if ((rt & 0x1F) == 2 && val == 0 && (idx & 15) && (idx & 15) <= EP_NOTIFY) {
            port_usb_index = idx & 15;
            if (idx & 0x80)
                port_usb_csr0 = req == 3 ? TXCSR_SENDSTALL : TXCSR_CLRDATATOG;
            else
                port_usb_rxcsr = req == 3 ? RXCSR_SENDSTALL : RXCSR_CLRDATATOG;
            port_usb_index = 0;
            return REQ_NODATA;
        }
        return (rt & 0x1F) == 0 ? REQ_NODATA : REQ_STALL;
    case 5: /* SET_ADDRESS */
        pending_addr = 0x80 | (val & 0x7F);
        return REQ_NODATA;
    case 6: /* GET_DESCRIPTOR */
        switch (val >> 8) {
        case 1:
            in_ptr = dev_desc;
            in_len = sizeof(dev_desc);
            return REQ_IN;
        case 2:
            in_ptr = cfg_desc;
            in_len = sizeof(cfg_desc);
            return REQ_IN;
        case 3:
            in_len = string_desc(val & 0xFF);
            in_ptr = ep0_buf;
            return in_len ? REQ_IN : REQ_STALL;
        }
        return REQ_STALL;               /* no device qualifier: full speed only */
    case 8: /* GET_CONFIGURATION */
        ep0_buf[0] = usb_config;
        in_ptr = ep0_buf;
        in_len = 1;
        return REQ_IN;
    case 9: /* SET_CONFIGURATION */
        if (val > 1)
            return REQ_STALL;
        usb_config = val;
        usb_alt = 0;
        ncm_enable(0);
        if (val) {
            ep_setup_hw();
            usb_state = USB_CONFIGURED;
        } else {
            usb_state = USB_ADDRESS;
        }
        return REQ_NODATA;
    case 10: /* GET_INTERFACE */
        if (!usb_config || idx > 1)
            return REQ_STALL;
        ep0_buf[0] = idx == 1 ? usb_alt : 0;
        in_ptr = ep0_buf;
        in_len = 1;
        return REQ_IN;
    case 11: /* SET_INTERFACE */
        if (!usb_config)
            return REQ_STALL;
        if (idx == 0)
            return val == 0 ? REQ_NODATA : REQ_STALL;
        if (idx == 1 && val <= 1) {
            usb_alt = val;
            ep_setup_hw();
            ncm_enable(val);
            return REQ_NODATA;
        }
        return REQ_STALL;
    }
    return REQ_STALL;
}

static void ep0_send_next(void)
{
    uint8_t n = in_len > 64 ? 64 : in_len;
    uint8_t csr = CSR0_TXPKTRDY;
    if (n) {
        memcpy(ep0_buf, in_ptr, n);      /* constants may live in any page */
        USB_WRITE(0, ep0_buf, n);
    }
    in_ptr += n;
    in_len -= n;
    if (in_len == 0) {
        if (n == 64 && in_zlp) {
            in_zlp = 0;                  /* a zero-length packet follows */
        } else {
            csr |= CSR0_DATAEND;
            ep0_state = EP0_STATUS;
            status_since = k_now_ms();
        }
    }
    port_usb_csr0 = csr;
}

static void ep0_setup(void)
{
    uint8_t r;
    uint16_t wlen = setup[6] | (setup[7] << 8);
    uint8_t *reply;
    uint16_t rlen;

    if ((setup[0] & 0x60) == 0x00) {
        r = std_request();
    } else if ((setup[0] & 0x60) == 0x20 && (setup[0] & 0x1F) == 1) {
        if (setup[0] & 0x80) {
            r = ncm_class_request(setup, &reply, &rlen) ? REQ_IN : REQ_STALL;
            in_ptr = reply;
            in_len = rlen;
        } else if (wlen == 0) {
            r = ncm_class_out(setup, NULL, 0) ? REQ_NODATA : REQ_STALL;
        } else {
            r = wlen <= sizeof(ep0_buf) ? REQ_OUT : REQ_STALL;
        }
    } else {
        r = REQ_STALL;
    }
    dbg_printf("usb: setup %02x %02x %02x%02x %02x%02x %u -> %u\n", setup[0], setup[1],
               setup[3], setup[2], setup[5], setup[4], wlen, r);

    switch (r) {
    case REQ_STALL:
        port_usb_csr0 = CSR0_SVDRXPKTRDY | CSR0_SENDSTALL;
        ep0_state = EP0_IDLE;
        break;
    case REQ_NODATA:
        port_usb_csr0 = CSR0_SVDRXPKTRDY | CSR0_DATAEND;
        ep0_state = EP0_STATUS;
        status_since = k_now_ms();
        break;
    case REQ_IN:
        if (in_len > wlen)
            in_len = wlen;
        in_zlp = in_len < wlen && (in_len & 63) == 0;
        port_usb_csr0 = CSR0_SVDRXPKTRDY;
        ep0_state = EP0_IN;
        ep0_send_next();
        break;
    case REQ_OUT:
        out_len = wlen;
        out_pos = 0;
        port_usb_csr0 = CSR0_SVDRXPKTRDY;
        ep0_state = EP0_OUT;
        break;
    }
}

static void ep0_rx(void)
{
    uint8_t n = port_usb_count;
    if (ep0_state == EP0_OUT) {
        if (out_pos + n > sizeof(ep0_buf))
            n = sizeof(ep0_buf) - out_pos;
        USB_READ(0, ep0_buf + out_pos, n);
        out_pos += n;
        if (out_pos >= out_len) {
            if (ncm_class_out(setup, ep0_buf, out_len)) {
                port_usb_csr0 = CSR0_SVDRXPKTRDY | CSR0_DATAEND;
                ep0_state = EP0_STATUS;
                status_since = k_now_ms();
            } else {
                port_usb_csr0 = CSR0_SVDRXPKTRDY | CSR0_SENDSTALL;
                ep0_state = EP0_IDLE;
            }
        } else {
            port_usb_csr0 = CSR0_SVDRXPKTRDY;
        }
        return;
    }
    if (n != 8) {
        USB_READ(0, ep0_buf, n > 64 ? 64 : n);
        port_usb_csr0 = CSR0_SVDRXPKTRDY | CSR0_SENDSTALL;
        ep0_state = EP0_IDLE;
        return;
    }
    USB_READ(0, setup, 8);
    ep0_setup();
}

/* ------------------------------------------------------------------ */
/* controller power                                                    */

static uint8_t wait_port(uint8_t port_sel, uint8_t mask, uint8_t want, uint16_t ms)
{
    uint32_t end = k_now_ms() + ms;
    for (;;) {
        uint8_t v = port_sel ? port_usb_lines : port_usb_status;
        if ((v & mask) == want)
            return 1;
        if ((int32_t)(k_now_ms() - end) >= 0)
            return 0;
    }
}

void usb_stop(void)
{
    port_usb_proto_ie = 0;
    port_usb_status = 0x00;
    port_usb_ctrl = 0x02;               /* controller and 48 MHz crystal off */
    usb_state = USB_OFF;
    ncm_enable(0);
}

void usb_start(void)
{
    usb_last_error = 0;
    usb_stop();
    port_usb_line_mask = 0;
    port_usb_pull = 0x20;               /* default pull-down setting */
    port_usb_vpwr = 0x00;               /* we never drive VBus */
    usb_ev_tx = usb_ev_rx = usb_ev_bus = 0;
    port_usb_ctrl = 0x00;
    k_delay_ms(5);
    if (port_gpio & 0x08)               /* older hardware revision */
        port_usb_ctrl = 0x44;
    port_usb_ctrl = 0xC4;
    if (!wait_port(0, 0xBF, 0x12, 200)) {
        usb_last_error = 1;             /* controller did not come up */
        dbg_printf("usb: 4C=%02x after power-up\n", port_usb_status);
    }
    port_usb_status = 0x08;
    if (!wait_port(0, 0x08, 0x08, 200))
        usb_last_error = 2;
    port_usb_faddr = 0;
    port_usb_intrtxe = 0x01 | (1 << EP_BULK) | (1 << EP_NOTIFY);
    port_usb_intrrxe = 1 << EP_BULK;
    port_usb_intrusbe = 0x05;           /* suspend + reset, as TI-OS in peripheral mode */
    ep0_state = EP0_IDLE;
    pending_addr = 0;
    usb_config = 0;
    usb_alt = 0;
    ncm_reset();
    usb_state = USB_WAIT_HOST;
    port_usb_proto_ie = 1;
    dbg_printf("usb: started, 4C=%02x 4D=%02x 54=%02x 8F=%02x err=%u\n", port_usb_status,
               port_usb_lines, port_usb_ctrl, port_usb_devctl, usb_last_error);
}

void usb_poll(void)
{
    uint8_t tx, bus, csr;
    if (usb_state == USB_OFF)
        return;
    __asm__("di");
    tx = usb_ev_tx;
    bus = usb_ev_bus;
    usb_ev_tx = 0;
    usb_ev_bus = 0;
    __asm__("ei");

    /* every 100 ms: cable/VBus check and re-arming of masked interrupts */
    if ((int32_t)(k_now_ms() - usb_next_check) >= 0) {
        usb_next_check = k_now_ms() + 100;
        if (usb_irq_masked) {
            usb_irq_masked = 0;
            port_usb_proto_ie = 1;
        }
        if (!(port_usb_lines & 0x40) && usb_state > USB_WAIT_HOST) {
            dbg_printf("usb: VBus lost\n");
            ep0_state = EP0_IDLE;
            pending_addr = 0;
            usb_config = 0;
            usb_alt = 0;
            ncm_enable(0);
            usb_state = USB_WAIT_HOST;
        }
    }
    if (bus & 0x04) {                   /* bus reset */
        dbg_printf("usb: bus reset\n");
        port_usb_faddr = 0;
        pending_addr = 0;
        ep0_state = EP0_IDLE;
        usb_config = 0;
        usb_alt = 0;
        ncm_enable(0);
        usb_state = USB_DEFAULT;
    }

    port_usb_index = 0;
    csr = port_usb_csr0;
    if (csr & CSR0_SENTSTALL) {
        port_usb_csr0 = 0;
        ep0_state = EP0_IDLE;
    }
    if (csr & CSR0_SETUPEND) {
        port_usb_csr0 = CSR0_SVDSETUPEND;
        ep0_state = EP0_IDLE;
    }
    if (ep0_state == EP0_STATUS && !(csr & CSR0_DATAEND)) {
        /* status stage finished: an EP0 interrupt arrived, or (fallback)
         * the host has had a few milliseconds to complete it */
        if ((tx & 1) || (csr & CSR0_RXPKTRDY) || (uint32_t)(k_now_ms() - status_since) > 50) {
            ep0_state = EP0_IDLE;
            if (pending_addr) {
                port_usb_faddr = pending_addr & 0x7F;
                usb_state = (pending_addr & 0x7F) ? USB_ADDRESS : USB_DEFAULT;
                dbg_printf("usb: address %u\n", pending_addr & 0x7F);
                pending_addr = 0;
            }
        }
    }
    if (csr & CSR0_RXPKTRDY) {
        usb_last_activity = k_now_ms();
        if (usb_state == USB_WAIT_HOST)
            usb_state = USB_DEFAULT;
        ep0_rx();
    } else if (ep0_state == EP0_IN && !(csr & CSR0_TXPKTRDY)) {
        ep0_send_next();
    }
    if (usb_state == USB_CONFIGURED)
        ncm_poll();
}
