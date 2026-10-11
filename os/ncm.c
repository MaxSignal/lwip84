/* ncm.c - USB CDC-NCM function (NTB16 only).
 *
 * Receive: bulk OUT packets are collected into one NTB buffer; while the
 * datagrams of that NTB are being consumed, further packets stay in the
 * controller FIFO and the host is NAKed (natural flow control).
 * Transmit: one datagram per NTB (NTH16 + NDP16 + frame).
 */
#include <string.h>
#include "hw.h"
#include "kernel.h"
#include "usb.h"
#include "ncm.h"

#define NTB_OUT_MAX 2048        /* host -> calculator */
#define NTB_IN_MAX  2048        /* calculator -> host */
#define TX_DGRAM_OFS 28         /* NTH16 (12) + NDP16 with one entry and terminator (16) */

const uint8_t ncm_dev_mac[6] = {0x02, 0x84, 0x84, 0x00, 0x00, 0x02};

static uint8_t rx_ntb[NTB_OUT_MAX];
static uint8_t tx_ntb[NTB_IN_MAX];
static uint16_t rx_len;
static uint8_t rx_ready;        /* complete NTB waiting to be consumed */
static uint8_t rx_drop;         /* current transfer overflowed: discard until its end */
static uint16_t rx_ndp, rx_ndp_len, rx_entry;
static uint16_t tx_len, tx_pos;
static uint8_t tx_busy, tx_zlp;
static uint16_t tx_seq;
static uint16_t ntb_in_size = NTB_IN_MAX;
static uint8_t enabled;
static uint8_t notify_state;    /* 0 idle, 1 send connection, 2 send speed, 3 done */
static uint8_t notify_buf[16];

struct ncm_stats ncm_stats;

static const uint8_t ntb_params[28] = {
    28, 0,                      /* wLength */
    0x01, 0x00,                 /* NTB16 only */
    NTB_IN_MAX & 0xFF, NTB_IN_MAX >> 8, 0, 0,
    4, 0,                       /* wNdpInDivisor */
    0, 0,                       /* wNdpInPayloadRemainder */
    4, 0,                       /* wNdpInAlignment */
    0, 0,
    NTB_OUT_MAX & 0xFF, NTB_OUT_MAX >> 8, 0, 0,
    4, 0,                       /* wNdpOutDivisor */
    0, 0,                       /* wNdpOutPayloadRemainder */
    4, 0,                       /* wNdpOutAlignment */
    8, 0,                       /* wNtbOutMaxDatagrams */
};

static uint8_t creq[8];

static uint16_t rd16(const uint8_t *p)
{
    return p[0] | ((uint16_t)p[1] << 8);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = v;
    p[1] = v >> 8;
}

void ncm_reset(void)
{
    enabled = 0;
    rx_len = 0;
    rx_ready = 0;
    rx_drop = 0;
    tx_busy = 0;
    notify_state = 0;
    ntb_in_size = NTB_IN_MAX;
}

void ncm_enable(uint8_t on)
{
    if (on == enabled)
        return;
    enabled = on;
    rx_len = 0;
    rx_ready = 0;
    rx_drop = 0;
    tx_busy = 0;
    notify_state = on ? 1 : 0;
    dbg_printf("ncm: %s\n", on ? "data interface on" : "off");
}

uint8_t ncm_link_up(void)
{
    return enabled && usb_state == USB_CONFIGURED;
}

uint8_t ncm_class_request(const uint8_t *setup, uint8_t **reply, uint16_t *len)
{
    switch (setup[1]) {
    case 0x80: /* GET_NTB_PARAMETERS */
        *reply = (uint8_t *)ntb_params;
        *len = sizeof(ntb_params);
        return 1;
    case 0x83: /* GET_NTB_FORMAT */
        wr16(creq, 0);
        *reply = creq;
        *len = 2;
        return 1;
    case 0x85: /* GET_NTB_INPUT_SIZE */
        wr16(creq, ntb_in_size);
        wr16(creq + 2, 0);
        *reply = creq;
        *len = 4;
        return 1;
    case 0x87: /* GET_MAX_DATAGRAM_SIZE */
        wr16(creq, 1514);
        *reply = creq;
        *len = 2;
        return 1;
    case 0x89: /* GET_CRC_MODE */
        wr16(creq, 0);
        *reply = creq;
        *len = 2;
        return 1;
    }
    return 0;
}

uint8_t ncm_class_out(const uint8_t *setup, const uint8_t *data, uint16_t len)
{
    uint16_t val = setup[2] | (setup[3] << 8);
    switch (setup[1]) {
    case 0x43: /* SET_ETHERNET_PACKET_FILTER: we deliver everything anyway */
        return 1;
    case 0x84: /* SET_NTB_FORMAT */
        return val == 0;
    case 0x86: /* SET_NTB_INPUT_SIZE */
        if (len < 4 || !data)
            return 0;
        {
            uint32_t sz = rd16(data) | ((uint32_t)rd16(data + 2) << 16);
            if (sz < 2048 && sz < NTB_IN_MAX)
                return 0;           /* NCM requires at least 2048 */
            ntb_in_size = sz > NTB_IN_MAX ? NTB_IN_MAX : (uint16_t)sz;
        }
        return 1;
    case 0x88: /* SET_MAX_DATAGRAM_SIZE */
        return 1;
    case 0x8A: /* SET_CRC_MODE */
        return val == 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* receive                                                             */

static void rx_parse(void)
{
    if (rx_len < 12 || memcmp(rx_ntb, "NCMH", 4) || rd16(rx_ntb + 4) != 12) {
        ncm_stats.rx_bad++;
        rx_len = 0;
        return;
    }
    rx_ndp = rd16(rx_ntb + 10);
    rx_entry = 0;
    rx_ndp_len = 0;
    rx_ready = 1;
    ncm_stats.rx_ntbs++;
}

static void rx_poll(void)
{
    uint8_t csr, n;
    if (rx_ready)
        return;                     /* leave the packet in the FIFO */
    port_usb_index = EP_BULK;
    csr = port_usb_rxcsr;
    if (csr & RXCSR_SENTSTALL)
        port_usb_rxcsr = 0;
    if (!(csr & RXCSR_RXPKTRDY))
        return;
    n = port_usb_count;
    if (n > EP_BULK_SIZE)
        n = EP_BULK_SIZE;
    if (rx_drop || rx_len + n > NTB_OUT_MAX) {
        uint8_t junk[EP_BULK_SIZE];
        USB_READ(EP_BULK, junk, n);
        rx_drop = 1;
    } else {
        USB_READ(EP_BULK, rx_ntb + rx_len, n);
        rx_len += n;
    }
    port_usb_rxcsr = 0;             /* packet unloaded */
    if (n < EP_BULK_SIZE || rx_len >= NTB_OUT_MAX) {
        if (rx_drop) {
            ncm_stats.rx_bad++;
            rx_drop = 0;
            rx_len = 0;
        } else if (rx_len) {
            rx_parse();
        }
    }
}

uint8_t ncm_rx_next(uint8_t **frame, uint16_t *len)
{
    for (;;) {
        if (!rx_ready)
            return 0;
        if (!rx_ndp_len) {
            if (!rx_ndp || rx_ndp + 16 > rx_len || memcmp(rx_ntb + rx_ndp, "NCM0", 4)) {
                if (rx_ndp)
                    ncm_stats.rx_bad++;
                rx_ready = 0;
                rx_len = 0;
                continue;
            }
            rx_ndp_len = rd16(rx_ntb + rx_ndp + 4);
            rx_entry = 8;
        }
        if (rx_entry + 4 <= rx_ndp_len) {
            uint16_t idx = rd16(rx_ntb + rx_ndp + rx_entry);
            uint16_t dl = rd16(rx_ntb + rx_ndp + rx_entry + 2);
            rx_entry += 4;
            if (idx && dl) {
                if (idx + dl > rx_len) {
                    ncm_stats.rx_bad++;
                    continue;
                }
                *frame = rx_ntb + idx;
                *len = dl;
                ncm_stats.rx_frames++;
                return 1;
            }
        }
        /* end of this NDP: follow the chain */
        rx_ndp = rd16(rx_ntb + rx_ndp + 6);
        rx_ndp_len = 0;
    }
}

/* ------------------------------------------------------------------ */
/* transmit                                                            */

uint8_t *ncm_tx_buffer(void)
{
    if (!enabled || tx_busy)
        return NULL;
    return tx_ntb + TX_DGRAM_OFS;
}

void ncm_tx_send(uint16_t len)
{
    uint8_t *b = tx_ntb;
    uint16_t total = TX_DGRAM_OFS + len;
    if (!enabled || tx_busy || total > ntb_in_size)
        return;
    memcpy(b, "NCMH", 4);
    wr16(b + 4, 12);
    wr16(b + 6, tx_seq++);
    wr16(b + 8, total);
    wr16(b + 10, 12);
    memcpy(b + 12, "NCM0", 4);
    wr16(b + 16, 16);
    wr16(b + 18, 0);
    wr16(b + 20, TX_DGRAM_OFS);
    wr16(b + 22, len);
    wr16(b + 24, 0);
    wr16(b + 26, 0);
    tx_len = total;
    tx_pos = 0;
    tx_zlp = (total & (EP_BULK_SIZE - 1)) == 0 && total < ntb_in_size;
    tx_busy = 1;
    ncm_stats.tx_frames++;
}

static void tx_poll(void)
{
    uint8_t csr, n;
    if (!tx_busy)
        return;
    port_usb_index = EP_BULK;
    csr = port_usb_csr0;            /* TXCSR */
    if (csr & TXCSR_SENTSTALL)
        port_usb_csr0 = 0;
    if (csr & TXCSR_TXPKTRDY)
        return;
    n = tx_len - tx_pos > EP_BULK_SIZE ? EP_BULK_SIZE : tx_len - tx_pos;
    if (n == 0 && !tx_zlp) {
        tx_busy = 0;
        return;
    }
    if (n == 0)
        tx_zlp = 0;
    else
        USB_WRITE(EP_BULK, tx_ntb + tx_pos, n);
    tx_pos += n;
    port_usb_csr0 = TXCSR_TXPKTRDY;
}

uint8_t ncm_tx_idle(void)
{
    return !tx_busy;
}

/* ------------------------------------------------------------------ */

static void notify_poll(void)
{
    uint8_t n;
    if (notify_state == 0 || notify_state == 3)
        return;
    port_usb_index = EP_NOTIFY;
    if (port_usb_csr0 & TXCSR_TXPKTRDY)
        return;
    notify_buf[0] = 0xA1;
    notify_buf[4] = 0;              /* wIndex: control interface 0 */
    notify_buf[5] = 0;
    if (notify_state == 1) {
        notify_buf[1] = 0x00;       /* NETWORK_CONNECTION */
        wr16(notify_buf + 2, 1);    /* connected */
        wr16(notify_buf + 6, 0);
        n = 8;
    } else {
        notify_buf[1] = 0x2A;       /* CONNECTION_SPEED_CHANGE */
        wr16(notify_buf + 2, 0);
        wr16(notify_buf + 6, 8);
        wr16(notify_buf + 8, 0x1B00);   /* 12 Mbit/s down and up */
        wr16(notify_buf + 10, 0x00B7);
        wr16(notify_buf + 12, 0x1B00);
        wr16(notify_buf + 14, 0x00B7);
        n = 16;
    }
    USB_WRITE(EP_NOTIFY, notify_buf, n);
    port_usb_csr0 = TXCSR_TXPKTRDY;
    notify_state++;
}

void ncm_poll(void)
{
    if (!enabled)
        return;
    notify_poll();
    rx_poll();
    tx_poll();
    port_usb_index = 0;
}
