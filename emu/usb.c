/*
 * usb.c - TI-84 Plus USB controller (Mentor MUSBFDRC, peripheral side) and a
 * virtual USB host that enumerates the calculator as a CDC-NCM function and
 * bridges its Ethernet frames to passt (qemu "stream" socket protocol:
 * every frame is preceded by a 32-bit big-endian length).
 *
 * The register model follows the MUSB semantics documented on WikiTI
 * (ports 4Ah-5Bh, 80h-AFh).  Where real hardware behaviour is unknown the
 * model is permissive and logs what the OS did, so differences can be
 * found on a real calculator.
 */
#include "emu84.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define NEP 4
#define FIFO 64
#define SLOT_PS 45000000ULL /* one transaction attempt per endpoint every 45 us */

/* CSR0 (peripheral) */
#define CSR0_RXPKTRDY 0x01
#define CSR0_TXPKTRDY 0x02
#define CSR0_SENTSTALL 0x04
#define CSR0_DATAEND 0x08
#define CSR0_SETUPEND 0x10
#define CSR0_SENDSTALL 0x20
#define CSR0_SVDRXPKTRDY 0x40
#define CSR0_SVDSETUPEND 0x80
/* TXCSR low */
#define TX_TXPKTRDY 0x01
#define TX_FIFONOTEMPTY 0x02
#define TX_UNDERRUN 0x04
#define TX_FLUSH 0x08
#define TX_SENDSTALL 0x10
#define TX_SENTSTALL 0x20
#define TX_CLRDATATOG 0x40
/* RXCSR low */
#define RX_RXPKTRDY 0x01
#define RX_FLUSH 0x10
#define RX_SENDSTALL 0x20
#define RX_SENTSTALL 0x40
#define RX_CLRDATATOG 0x80

struct ep {
    uint8_t txmaxp, rxmaxp, txcsr, txcsr2, rxcsr, rxcsr2, txtype, rxtype, txint, rxint;
    uint8_t txbuf[FIFO];
    int txlen;
    uint8_t rxbuf[FIFO];
    int rxlen, rxpos;
};

static struct {
    int host_attached, verbose;
    /* power / line control */
    uint8_t r4a, r4b, r4c, r51, r52, r54, r56, r57, r5b;
    /* MUSB registers */
    uint8_t faddr, power, index, devctl;
    uint16_t intrtx, intrrx, intrtxe, intrrxe;
    uint8_t intrusb, intrusbe;
    struct ep ep[NEP];
    uint8_t csr0;
    uint16_t frame;
    /* virtual host */
    int connected;        /* controller active and seen by host */
    uint64_t t_active, next_slot, next_frame;
    int state;
    uint64_t state_t;
    uint8_t addr_dev;     /* address the host uses */
    /* control transfer */
    int cx_active, cx_phase, cx_in, cx_status_ok, cx_stall;
    uint8_t cx_setup[8];
    uint8_t cx_buf[512];
    int cx_len, cx_pos, cx_want;
    uint64_t cx_deadline, cx_addr_deadline;
    int ep0_maxp;          /* host's idea of bMaxPacketSize0 */
    /* bulk IN reassembly (device -> host) */
    uint8_t in_xfer[4096];
    int in_len;
    uint8_t ntf[64];
    int ntf_len;
    /* bulk OUT (host -> device) */
    uint8_t out_xfer[4096];
    int out_len, out_pos, out_zlp;
    /* NCM state */
    int ncm_data_if, ncm_ctrl_if, ncm_up;
    uint8_t mac[6];
    uint16_t seq;
    uint32_t ntb_in_max, ntb_out_max;
    uint16_t out_div, out_rem, out_align;
    /* network */
    int net_fd;
    uint8_t net_rx[65536 + 4];
    int net_rx_len;
    unsigned long frames_to_dev, frames_from_dev, ntb_bad;
} u;

static uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

static void ulog(const char *fmt, ...)
{
    va_list ap;
    if (!u.verbose)
        return;
    va_start(ap, fmt);
    fprintf(stderr, "usb: [%llu] ", (unsigned long long)(ti.now_ps / 1000000000ULL));
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void uerr(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "usb: ERROR: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static int ctrl_active(void)
{
    /* WikiTI: bit 2 of port 54h together with bit 3 of port 4Ch turn the
     * 80h-AFh ports on; bits 6/7 of port 54h must be set for USB use. */
    return (u.r54 & 0x04) && (u.r4c & 0x08) && (u.r54 & 0xC0) == 0xC0;
}

static void musb_bus_reset(void)
{
    u.faddr = 0;
    u.index = 0;
    u.csr0 = 0;
    for (int i = 0; i < NEP; i++) {
        struct ep *e = &u.ep[i];
        e->txcsr = e->rxcsr = 0;
        e->txlen = e->rxlen = e->rxpos = 0;
    }
    u.intrusb |= 0x04;
}

/* ------------------------------------------------------------------ */
/* register file                                                       */

static uint8_t r4c_read(void)
{
    uint8_t v = u.r4c & 0x08;
    v |= 0x02;                          /* no bus reset signal right now */
    if (u.r54 & 0x04) v |= 0x10;
    if (!(u.r54 & 0x40)) v |= 0x20;
    if (u.r54 & 0x80) v |= 0x40;
    return v;
}

/* "vbusglitch MS": the calculator reads VBus low for a while, the host
 * notices nothing (a false reading of the line) */
static uint64_t vbus_glitch_until;

void usb_vbus_glitch(double ms)
{
    vbus_glitch_until = ti.now_ps + (uint64_t)(ms * 1e9);
    ulog("VBus reads low for %.0f ms", ms);
}

static uint8_t r4d_read(void)
{
    uint8_t v = 0x20;                   /* ID high: B cable or none */
    if (u.host_attached) {
        if (ti.now_ps >= vbus_glitch_until)
            v |= 0x40;                  /* VBus high */
        if (u.connected) v |= 0x02;     /* D+ high (pull-up) */
        else v |= 0x01;
        v |= 0x04;                      /* D- low */
    } else {
        v |= 0x80 | 0x01 | 0x04;
    }
    return v;
}

static int usb_irq_proto(void)
{
    if (!(u.r5b & 1))
        return 0;
    return (u.intrtx & u.intrtxe) || (u.intrrx & u.intrrxe) || (u.intrusb & u.intrusbe);
}

int usb_irq_pending(void)
{
    return usb_irq_proto() || (u.r56 & u.r57);
}

int usb_port_in(uint8_t p, uint8_t *v)
{
    struct ep *e = &u.ep[u.index & 3];
    switch (p) {
    case 0x4A: *v = u.r4a | 0x02; return 1;
    case 0x4B: *v = u.r4b | (u.host_attached ? 0x01 : 0x04); return 1;
    case 0x4C: *v = r4c_read(); return 1;
    case 0x4D: *v = r4d_read(); return 1;
    case 0x51: *v = u.r51; return 1;
    case 0x52: *v = u.r52; return 1;
    case 0x54: *v = u.r54; return 1;
    case 0x55: *v = 0x1F & ~(usb_irq_proto() ? 0x10 : 0) & ~((u.r56 & u.r57) ? 0x04 : 0); return 1;
    case 0x56: *v = u.r56; return 1;
    case 0x57: *v = u.r57; return 1;
    case 0x5B: *v = u.r5b; return 1;
    }
    if (p < 0x80)
        return 0;
    if (!ctrl_active()) {
        *v = 0x00;
        return 1;
    }
    switch (p) {
    case 0x80: *v = u.faddr; return 1;
    case 0x81: *v = u.power; return 1;
    case 0x82: *v = u.intrtx & 0xFF; u.intrtx &= 0xFF00; return 1;
    case 0x83: *v = u.intrtx >> 8; u.intrtx &= 0x00FF; return 1;
    case 0x84: *v = u.intrrx & 0xFF; u.intrrx &= 0xFF00; return 1;
    case 0x85: *v = u.intrrx >> 8; u.intrrx &= 0x00FF; return 1;
    case 0x86: *v = u.intrusb; if (u.intrusb) ulog("INTRUSB read %02x", u.intrusb); u.intrusb = 0; return 1;
    case 0x87: *v = u.intrtxe & 0xFF; return 1;
    case 0x88: *v = u.intrtxe >> 8; return 1;
    case 0x89: *v = u.intrrxe & 0xFF; return 1;
    case 0x8A: *v = u.intrrxe >> 8; return 1;
    case 0x8B: *v = u.intrusbe; return 1;
    case 0x8C: *v = u.frame & 0xFF; return 1;
    case 0x8D: *v = (u.frame >> 8) & 0x07; return 1;
    case 0x8E: *v = u.index; return 1;
    case 0x8F: *v = 0x80 | 0x10 | (u.host_attached ? 0x01 : 0) | (u.connected ? 0x40 : 0); return 1;
    case 0x90: *v = e->txmaxp; return 1;
    case 0x91:
        if ((u.index & 15) == 0) *v = u.csr0;
        else *v = e->txcsr | (e->txlen ? TX_FIFONOTEMPTY : 0);
        return 1;
    case 0x92: *v = e->txcsr2; return 1;
    case 0x93: *v = e->rxmaxp; return 1;
    case 0x94: *v = e->rxcsr; return 1;
    case 0x95: *v = e->rxcsr2; return 1;
    case 0x96: *v = (e->rxlen - e->rxpos) & 0xFF; return 1;
    case 0x97: *v = (e->rxlen - e->rxpos) >> 8; return 1;
    case 0x98: *v = e->txtype; return 1;
    case 0x99: *v = e->txint; return 1;
    case 0x9A: *v = e->rxtype; return 1;
    case 0x9B: *v = e->rxint; return 1;
    }
    if (p >= 0xA0 && p < 0xA0 + NEP) {
        struct ep *f = &u.ep[p - 0xA0];
        *v = f->rxpos < f->rxlen ? f->rxbuf[f->rxpos++] : 0;
        return 1;
    }
    *v = 0;
    return 1;
}

static void csr0_write(uint8_t v)
{
    struct ep *e = &u.ep[0];
    if (v & CSR0_SVDRXPKTRDY) {
        u.csr0 &= ~CSR0_RXPKTRDY;
        e->rxlen = e->rxpos = 0;
    }
    if (v & CSR0_SVDSETUPEND)
        u.csr0 &= ~CSR0_SETUPEND;
    if (v & CSR0_SENDSTALL)
        u.csr0 |= CSR0_SENDSTALL;
    if (v & CSR0_DATAEND)
        u.csr0 |= CSR0_DATAEND;
    if (v & CSR0_TXPKTRDY)
        u.csr0 |= CSR0_TXPKTRDY;
    if (!(v & CSR0_SENTSTALL))
        u.csr0 &= ~CSR0_SENTSTALL;
}

int usb_port_out(uint8_t p, uint8_t v)
{
    struct ep *e = &u.ep[u.index & 3];
    switch (p) {
    case 0x4A: u.r4a = v & 0x38; return 1;
    case 0x4B: u.r4b = v & 0x20; return 1;
    case 0x4C: u.r4c = v & 0x08; return 1;
    case 0x51: u.r51 = v; return 1;
    case 0x52: u.r52 = v; return 1;
    case 0x54: {
        int was = ctrl_active();
        u.r54 = v & ~0x28;
        if (!was && ctrl_active())
            ulog("controller on (54=%02X)", v);
        return 1;
    }
    case 0x57: u.r57 = v; u.r56 &= v; return 1;
    case 0x5B: u.r5b = v & 5; return 1;
    }
    if (p < 0x80)
        return 0;
    if (!ctrl_active()) {
        ulog("write %02X=%02X while controller off", p, v);
        return 1;
    }
    switch (p) {
    case 0x80: u.faddr = v & 0x7F; ulog("FADDR=%d", u.faddr); return 1;
    case 0x81: u.power = v; return 1;
    case 0x87: u.intrtxe = (u.intrtxe & 0xFF00) | v; return 1;
    case 0x88: u.intrtxe = (u.intrtxe & 0x00FF) | (v << 8); return 1;
    case 0x89: u.intrrxe = (u.intrrxe & 0xFF00) | v; return 1;
    case 0x8A: u.intrrxe = (u.intrrxe & 0x00FF) | (v << 8); return 1;
    case 0x8B: u.intrusbe = v; return 1;
    case 0x8E: u.index = v & 15; return 1;
    case 0x8F: u.devctl = v; return 1;
    case 0x90: e->txmaxp = v; return 1;
    case 0x91:
        if ((u.index & 15) == 0) {
            csr0_write(v);
        } else {
            if (v & TX_FLUSH) e->txlen = 0;
            if (v & TX_CLRDATATOG) { /* toggle state not modelled */ }
            e->txcsr = (e->txcsr & TX_TXPKTRDY) | (v & (TX_SENDSTALL | TX_UNDERRUN | TX_SENTSTALL));
            if (!(v & TX_SENTSTALL)) e->txcsr &= ~TX_SENTSTALL;
            if (!(v & TX_UNDERRUN)) e->txcsr &= ~TX_UNDERRUN;
            if (v & TX_TXPKTRDY) e->txcsr |= TX_TXPKTRDY;
        }
        return 1;
    case 0x92: e->txcsr2 = v; return 1;
    case 0x93: e->rxmaxp = v; return 1;
    case 0x94:
        if (v & RX_FLUSH) { e->rxlen = e->rxpos = 0; e->rxcsr &= ~RX_RXPKTRDY; }
        if (!(v & RX_RXPKTRDY) && (e->rxcsr & RX_RXPKTRDY)) {
            e->rxcsr &= ~RX_RXPKTRDY;   /* packet unloaded */
            e->rxlen = e->rxpos = 0;
        }
        e->rxcsr = (e->rxcsr & RX_RXPKTRDY) | (v & RX_SENDSTALL) | (e->rxcsr & RX_SENTSTALL & v);
        return 1;
    case 0x95: e->rxcsr2 = v; return 1;
    case 0x98: e->txtype = v; return 1;
    case 0x99: e->txint = v; return 1;
    case 0x9A: e->rxtype = v; return 1;
    case 0x9B: e->rxint = v; return 1;
    }
    if (p >= 0xA0 && p < 0xA0 + NEP) {
        struct ep *f = &u.ep[p - 0xA0];
        if (f->txlen < FIFO)
            f->txbuf[f->txlen++] = v;
        else
            uerr("FIFO %d overflow", p - 0xA0);
        return 1;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* network backend (passt)                                             */

static void net_open(const char *path)
{
    struct sockaddr_un sa;
    u.net_fd = -1;
    if (!path)
        return;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "usb: cannot connect to %s: %s\n", path, strerror(errno));
        close(fd);
        return;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    u.net_fd = fd;
    fprintf(stderr, "usb: network backend %s\n", path);
}

static void net_send(const uint8_t *frame, int len)
{
    uint8_t hdr[4] = {len >> 24, len >> 16, len >> 8, len};
    if (u.net_fd < 0)
        return;
    int fl = fcntl(u.net_fd, F_GETFL);
    fcntl(u.net_fd, F_SETFL, fl & ~O_NONBLOCK);
    if (write(u.net_fd, hdr, 4) != 4 || write(u.net_fd, frame, len) != len)
        uerr("network write failed");
    fcntl(u.net_fd, F_SETFL, fl);
}

/* returns frame length or 0 */
static int net_recv(uint8_t *frame, int max)
{
    if (u.net_fd < 0)
        return 0;
    for (;;) {
        if (u.net_rx_len >= 4) {
            int len = (u.net_rx[0] << 24) | (u.net_rx[1] << 16) | (u.net_rx[2] << 8) | u.net_rx[3];
            if (u.net_rx_len >= 4 + len) {
                int n = len < max ? len : max;
                memcpy(frame, u.net_rx + 4, n);
                memmove(u.net_rx, u.net_rx + 4 + len, u.net_rx_len - 4 - len);
                u.net_rx_len -= 4 + len;
                return n;
            }
        }
        ssize_t r = read(u.net_fd, u.net_rx + u.net_rx_len, sizeof(u.net_rx) - u.net_rx_len);
        if (r <= 0)
            return 0;
        u.net_rx_len += r;
    }
}

/* ------------------------------------------------------------------ */
/* virtual host: control transfers                                     */

enum { CX_SETUP, CX_SETUP_ACK, CX_DATA_IN, CX_DATA_OUT, CX_DATA_OUT_ACK, CX_STATUS, CX_DONE };

static void ep0_irq(void) { u.intrtx |= 1; }

static void cx_start(uint8_t bmrt, uint8_t breq, uint16_t wval, uint16_t widx, uint16_t wlen,
                     const void *out)
{
    uint8_t *s = u.cx_setup;
    s[0] = bmrt; s[1] = breq; s[2] = wval; s[3] = wval >> 8;
    s[4] = widx; s[5] = widx >> 8; s[6] = wlen; s[7] = wlen >> 8;
    u.cx_in = bmrt & 0x80;
    u.cx_want = wlen;
    u.cx_len = u.cx_in ? 0 : wlen;
    if (!u.cx_in && wlen && out)
        memcpy(u.cx_buf, out, wlen);
    u.cx_pos = 0;
    u.cx_active = 1;
    u.cx_stall = 0;
    u.cx_status_ok = 0;
    u.cx_phase = CX_SETUP;
    u.cx_deadline = ti.now_ps + 500000000000ULL;
    /* like a real host: after SET_ADDRESS the device has ~10 ms to switch */
    u.cx_addr_deadline = ti.now_ps + 10000000000ULL;
    ulog("SETUP %02x %02x %04x %04x %d", bmrt, breq, wval, widx, wlen);
}

static void cx_finish(int ok)
{
    u.cx_active = 0;
    u.cx_phase = CX_DONE;
    u.cx_status_ok = ok;
}

static void cx_step(void)
{
    struct ep *e = &u.ep[0];
    if (!u.cx_active)
        return;
    if (ti.now_ps > u.cx_deadline) {
        uerr("control transfer %02x %02x timed out in phase %d (csr0=%02x)",
             u.cx_setup[0], u.cx_setup[1], u.cx_phase, u.csr0);
        cx_finish(0);
        return;
    }
    if (u.csr0 & CSR0_SENDSTALL) {
        /* the device stalls the next stage */
        u.csr0 &= ~CSR0_SENDSTALL;
        u.csr0 |= CSR0_SENTSTALL;
        ep0_irq();
        u.cx_stall = 1;
        ulog("  -> STALL");
        cx_finish(0);
        return;
    }
    switch (u.cx_phase) {
    case CX_SETUP:
        if (u.faddr != u.addr_dev) {
            /* device does not answer at this address (yet) */
            if (ti.now_ps > u.cx_addr_deadline) {
                uerr("device not responding at address %d (FADDR=%d): -71", u.addr_dev, u.faddr);
                cx_finish(0);
            }
            break;
        }
        if (u.csr0 & (CSR0_RXPKTRDY | CSR0_TXPKTRDY)) {
            /* a new SETUP aborts an unfinished transfer */
            u.csr0 |= CSR0_SETUPEND;
            u.csr0 &= ~CSR0_TXPKTRDY;
        }
        /* and the controller flushes EP0's FIFO: a packet queued for a data
         * stage the host cut short does not stay in front of the answer */
        e->txlen = 0;
        memcpy(e->rxbuf, u.cx_setup, 8);
        e->rxlen = 8;
        e->rxpos = 0;
        u.csr0 = (u.csr0 & CSR0_SETUPEND) | CSR0_RXPKTRDY;
        ep0_irq();
        u.cx_phase = CX_SETUP_ACK;
        break;
    case CX_SETUP_ACK:
        if (u.csr0 & CSR0_RXPKTRDY)
            break;
        if (u.cx_want == 0) {
            u.cx_phase = CX_STATUS;
        } else if (u.cx_in) {
            u.cx_phase = CX_DATA_IN;
        } else {
            u.cx_phase = CX_DATA_OUT;
        }
        cx_step();
        break;
    case CX_DATA_IN:
        if (!(u.csr0 & CSR0_TXPKTRDY))
            break;
        {
            int n = e->txlen;
            if (u.cx_len + n > (int)sizeof(u.cx_buf))
                n = sizeof(u.cx_buf) - u.cx_len;
            memcpy(u.cx_buf + u.cx_len, e->txbuf, n);
            u.cx_len += n;
            e->txlen = 0;
            u.csr0 &= ~CSR0_TXPKTRDY;
            int last = (u.csr0 & CSR0_DATAEND) || n < u.ep0_maxp || u.cx_len >= u.cx_want;
            if (n > u.ep0_maxp)
                uerr("EP0 packet of %d bytes, max packet size %d", n, u.ep0_maxp);
            ep0_irq();
            if (last && !(u.csr0 & CSR0_DATAEND)) {
                /* host ended the data stage early (short packet): the status
                 * stage comes before DataEnd, the controller flags SetupEnd */
                ulog("  (data stage ended early: SetupEnd)");
                u.csr0 |= CSR0_SETUPEND;
                cx_finish(1);
                break;
            }
            if (last)
                u.cx_phase = CX_STATUS;
        }
        break;
    case CX_DATA_OUT:
        if (u.csr0 & CSR0_RXPKTRDY)
            break;
        {
            int n = u.cx_len - u.cx_pos;
            if (n > FIFO)
                n = FIFO;
            memcpy(e->rxbuf, u.cx_buf + u.cx_pos, n);
            e->rxlen = n;
            e->rxpos = 0;
            u.cx_pos += n;
            u.csr0 |= CSR0_RXPKTRDY;
            ep0_irq();
            if (u.cx_pos >= u.cx_len)
                u.cx_phase = CX_DATA_OUT_ACK;
        }
        break;
    case CX_DATA_OUT_ACK:
        if (u.csr0 & CSR0_RXPKTRDY)
            break;
        u.cx_phase = CX_STATUS;
        cx_step();
        break;
    case CX_STATUS:
        if (!(u.csr0 & CSR0_DATAEND))
            break;
        u.csr0 &= ~CSR0_DATAEND;
        ep0_irq();
        ulog("  -> ok, %d bytes", u.cx_in ? u.cx_len : u.cx_want);
        cx_finish(1);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* virtual host: enumeration and NCM                                   */

enum {
    H_IDLE, H_RESET, H_DEVDESC8, H_RESET2, H_SETADDR, H_DEVDESC, H_CFG9, H_CFG, H_MACSTR, H_SETCFG,
    H_NTBPARAM, H_ALT0, H_ALT1, H_RUN, H_FAILED
};

static int cfg_iMAC;

static void parse_config(const uint8_t *d, int len)
{
    int i = 0, cur_if = -1, cur_class = -1, cur_sub = -1;
    u.ncm_ctrl_if = u.ncm_data_if = -1;
    cfg_iMAC = 0;
    while (i + 2 <= len && d[i] >= 2) {
        int l = d[i], t = d[i + 1];
        if (t == 4) {
            cur_if = d[i + 2];
            cur_class = d[i + 5];
            cur_sub = d[i + 6];
            if (cur_class == 2 && cur_sub == 0x0D)
                u.ncm_ctrl_if = cur_if;
            if (cur_class == 0x0A)
                u.ncm_data_if = cur_if;
        } else if (t == 0x24 && l >= 13 && d[i + 2] == 0x0F) {
            cfg_iMAC = d[i + 3];
        } else if (t == 5) {
            ulog("  endpoint %02x attr %02x maxp %d (if %d)", d[i + 2], d[i + 3],
                 d[i + 4] | (d[i + 5] << 8), cur_if);
        }
        i += l;
    }
    ulog("  NCM control if %d, data if %d, iMACAddress %d", u.ncm_ctrl_if, u.ncm_data_if, cfg_iMAC);
}

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 :
           c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static void host_next(int st)
{
    u.state = st;
    u.state_t = ti.now_ps;
    switch (st) {
    case H_DEVDESC8: cx_start(0x80, 6, 0x0100, 0, 64, NULL); break;
    case H_SETADDR: cx_start(0x00, 5, 7, 0, 0, NULL); break;
    case H_DEVDESC: cx_start(0x80, 6, 0x0100, 0, 18, NULL); break;
    case H_CFG9: cx_start(0x80, 6, 0x0200, 0, 9, NULL); break;
    case H_MACSTR: cx_start(0x80, 6, 0x0300 | cfg_iMAC, 0x0409, 255, NULL); break;
    case H_SETCFG: cx_start(0x00, 9, 1, 0, 0, NULL); break;
    case H_NTBPARAM: cx_start(0xA1, 0x80, 0, u.ncm_ctrl_if, 28, NULL); break;
    case H_ALT0: cx_start(0x01, 11, 0, u.ncm_data_if, 0, NULL); break;
    case H_ALT1: cx_start(0x01, 11, 1, u.ncm_data_if, 0, NULL); break;
    default: break;
    }
}

static void host_fail(const char *why)
{
    uerr("enumeration failed: %s", why);
    u.state = H_FAILED;
}

static void host_step(void)
{
    if (u.cx_active)
        return;
    int ok = u.cx_status_ok;
    switch (u.state) {
    case H_IDLE:
        if (u.connected && ti.now_ps - u.t_active > 100000000000ULL) {
            ulog("bus reset");
            musb_bus_reset();
            u.addr_dev = 0;
            u.ep0_maxp = 64;
            u.state = H_RESET;
            u.state_t = ti.now_ps;
        }
        break;
    case H_RESET:
        if (ti.now_ps - u.state_t > 20000000000ULL)
            host_next(H_DEVDESC8);
        break;
    case H_DEVDESC8:
        if (!ok || u.cx_len < 8)
            return host_fail("GET_DESCRIPTOR(device)");
        u.ep0_maxp = u.cx_buf[7];
        ulog("  bMaxPacketSize0 %d", u.ep0_maxp);
        musb_bus_reset();               /* Linux resets again before SET_ADDRESS */
        u.state = H_RESET2;
        u.state_t = ti.now_ps;
        return;
    case H_RESET2:
        if (ti.now_ps - u.state_t < 20000000000ULL)
            return;
        host_next(H_SETADDR);
        break;
    case H_SETADDR:
        if (!ok)
            return host_fail("SET_ADDRESS");
        u.addr_dev = 7;
        host_next(H_DEVDESC);
        break;
    case H_DEVDESC:
        if (u.faddr != u.addr_dev)
            return host_fail("device did not apply the new address");
        if (!ok || u.cx_len != 18)
            return host_fail("GET_DESCRIPTOR(device, 18)");
        ulog("  VID %04x PID %04x class %02x maxp0 %d configs %d",
             u.cx_buf[8] | (u.cx_buf[9] << 8), u.cx_buf[10] | (u.cx_buf[11] << 8),
             u.cx_buf[4], u.cx_buf[7], u.cx_buf[17]);
        host_next(H_CFG9);
        break;
    case H_CFG9:
        if (!ok || u.cx_len != 9)
            return host_fail("GET_DESCRIPTOR(config, 9)");
        u.state = H_CFG;
        cx_start(0x80, 6, 0x0200, 0, u.cx_buf[2] | (u.cx_buf[3] << 8), NULL);
        break;
    case H_CFG:
        if (!ok)
            return host_fail("GET_DESCRIPTOR(config)");
        parse_config(u.cx_buf, u.cx_len);
        if (u.ncm_ctrl_if < 0 || u.ncm_data_if < 0 || !cfg_iMAC)
            return host_fail("no NCM function in the configuration");
        host_next(H_MACSTR);
        break;
    case H_MACSTR:
        if (!ok || u.cx_len < 26)
            return host_fail("iMACAddress string");
        for (int i = 0; i < 6; i++)
            u.mac[i] = (hexval(u.cx_buf[2 + 4 * i]) << 4) | hexval(u.cx_buf[4 + 4 * i]);
        ulog("  device MAC %02x:%02x:%02x:%02x:%02x:%02x", u.mac[0], u.mac[1], u.mac[2],
             u.mac[3], u.mac[4], u.mac[5]);
        host_next(H_SETCFG);
        break;
    case H_SETCFG:
        if (!ok)
            return host_fail("SET_CONFIGURATION");
        host_next(H_NTBPARAM);
        break;
    case H_NTBPARAM:
        if (!ok || u.cx_len != 28)
            return host_fail("GET_NTB_PARAMETERS");
        {
            const uint8_t *b = u.cx_buf;
            u.ntb_in_max = b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24);
            u.ntb_out_max = b[16] | (b[17] << 8) | (b[18] << 16) | ((uint32_t)b[19] << 24);
            u.out_div = b[20] | (b[21] << 8);
            u.out_rem = b[22] | (b[23] << 8);
            u.out_align = b[24] | (b[25] << 8);
            ulog("  NTB formats %04x, in max %u, out max %u, out div %u rem %u align %u",
                 b[2] | (b[3] << 8), u.ntb_in_max, u.ntb_out_max, u.out_div, u.out_rem, u.out_align);
            if (u.ntb_out_max > sizeof(u.out_xfer))
                u.ntb_out_max = sizeof(u.out_xfer);
        }
        host_next(H_ALT0);
        break;
    case H_ALT0:
        if (!ok)
            return host_fail("SET_INTERFACE(alt 0)");
        host_next(H_ALT1);
        break;
    case H_ALT1:
        if (!ok)
            return host_fail("SET_INTERFACE(alt 1)");
        fprintf(stderr, "usb: NCM device configured\n");
        u.state = H_RUN;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* NTB16 handling                                                      */


static void ntb_from_device(const uint8_t *b, int len)
{
    if (len < 12 || memcmp(b, "NCMH", 4)) {
        u.ntb_bad++;
        uerr("bad NTB from device (len %d, sig %.4s)", len, b);
        return;
    }
    int hl = le16(b + 4), blk = le16(b + 8), ndp = le16(b + 10);
    if (hl != 12 || blk > len) {
        u.ntb_bad++;
        uerr("bad NTH16 (hl %d, block %d, xfer %d)", hl, blk, len);
        return;
    }
    while (ndp) {
        if (ndp + 16 > len || memcmp(b + ndp, "NCM0", 4)) {
            u.ntb_bad++;
            uerr("bad NDP16 at %d", ndp);
            return;
        }
        int nl = le16(b + ndp + 4), next = le16(b + ndp + 6);
        for (int i = 8; i + 4 <= nl; i += 4) {
            int idx = le16(b + ndp + i), dl = le16(b + ndp + i + 2);
            if (!idx || !dl)
                break;
            if (idx + dl > len) {
                u.ntb_bad++;
                uerr("datagram out of range");
                break;
            }
            u.frames_from_dev++;
            ulog("frame from device: %d bytes", dl);
            net_send(b + idx, dl);
        }
        ndp = next;
    }
}

/* build an NTB from queued frames into out_xfer */
static void ntb_build(void)
{
    uint8_t frame[2048];
    uint8_t *b = u.out_xfer;
    int pos = 12, nfr = 0;
    int idx[16], dl[16];
    int max = u.ntb_out_max ? (int)u.ntb_out_max : 2048;
    int div = u.out_div ? u.out_div : 4;
    for (;;) {
        if (nfr == 16)
            break;
        /* peek: only take a frame if it fits */
        if (u.net_rx_len >= 4) {
            int fl = (u.net_rx[0] << 24) | (u.net_rx[1] << 16) | (u.net_rx[2] << 8) | u.net_rx[3];
            int start = (pos + div - 1) / div * div;
            if (start + fl + 12 + 4 * (nfr + 2) > max) {
                if (!nfr)
                    net_recv(frame, sizeof(frame)); /* drop frame that never fits */
                break;
            }
        }
        int n = net_recv(frame, sizeof(frame));
        if (!n)
            break;
        pos = (pos + div - 1) / div * div;
        memcpy(b + pos, frame, n);
        idx[nfr] = pos;
        dl[nfr] = n;
        nfr++;
        pos += n;
    }
    if (!nfr)
        return;
    pos = (pos + 3) & ~3;
    int ndp = pos;
    memcpy(b + ndp, "NCM0", 4);
    put16(b + ndp + 4, 8 + 4 * (nfr + 1));
    put16(b + ndp + 6, 0);
    for (int i = 0; i < nfr; i++) {
        put16(b + ndp + 8 + 4 * i, idx[i]);
        put16(b + ndp + 10 + 4 * i, dl[i]);
    }
    put16(b + ndp + 8 + 4 * nfr, 0);
    put16(b + ndp + 10 + 4 * nfr, 0);
    pos = ndp + 8 + 4 * (nfr + 1);
    memcpy(b, "NCMH", 4);
    put16(b + 4, 12);
    put16(b + 6, u.seq++);
    put16(b + 8, pos);
    put16(b + 10, ndp);
    u.out_len = pos;
    u.out_pos = 0;
    u.out_zlp = (pos % FIFO) == 0;
    u.frames_to_dev += nfr;
    ulog("NTB to device: %d frames, %d bytes", nfr, pos);
}

static void bulk_step(void)
{
    struct ep *tx1 = &u.ep[1], *ntf = &u.ep[2];
    if (u.state != H_RUN)
        return;
    /* bulk IN on EP1 (device -> host) */
    if (tx1->txcsr & TX_SENDSTALL) {
        tx1->txcsr |= TX_SENTSTALL;
        u.intrtx |= 2;
    } else if (tx1->txcsr & TX_TXPKTRDY) {
        int n = tx1->txlen;
        if (u.in_len + n <= (int)sizeof(u.in_xfer)) {
            memcpy(u.in_xfer + u.in_len, tx1->txbuf, n);
            u.in_len += n;
        } else {
            uerr("bulk IN transfer too long");
        }
        tx1->txlen = 0;
        tx1->txcsr &= ~TX_TXPKTRDY;
        u.intrtx |= 2;
        if (n < FIFO || u.in_len >= (int)u.ntb_in_max) {
            ntb_from_device(u.in_xfer, u.in_len);
            u.in_len = 0;
        }
    }
    /* interrupt IN on EP2 (notifications) */
    if (ntf->txcsr & TX_TXPKTRDY) {
        int n = ntf->txlen;
        memcpy(u.ntf + u.ntf_len, ntf->txbuf, n < 64 - u.ntf_len ? n : 64 - u.ntf_len);
        u.ntf_len += n;
        ntf->txlen = 0;
        ntf->txcsr &= ~TX_TXPKTRDY;
        u.intrtx |= 4;
        if (u.ntf_len >= 8 && u.ntf_len >= 8 + le16(u.ntf + 6)) {
            if (u.ntf[0] == 0xA1) {
                if (u.ntf[1] == 0x00) {
                    u.ncm_up = le16(u.ntf + 2);
                    fprintf(stderr, "usb: NCM link %s\n", u.ncm_up ? "up" : "down");
                } else if (u.ntf[1] == 0x2A) {
                    ulog("speed change notification");
                }
            }
            u.ntf_len = 0;
        }
    }
    /* bulk OUT on EP1 (host -> device) */
    struct ep *rx1 = &u.ep[1];
    if (!(rx1->rxcsr & RX_RXPKTRDY)) {
        if (u.out_pos >= u.out_len && !u.out_zlp) {
            u.out_len = u.out_pos = 0;
            ntb_build();
        }
        if (u.out_len > 0 && (u.out_pos < u.out_len || u.out_zlp)) {
            int n = u.out_len - u.out_pos;
            if (n > FIFO)
                n = FIFO;
            if (n == 0)
                u.out_zlp = 0;
            else
                memcpy(rx1->rxbuf, u.out_xfer + u.out_pos, n);
            rx1->rxlen = n;
            rx1->rxpos = 0;
            u.out_pos += n;
            rx1->rxcsr |= RX_RXPKTRDY;
            u.intrrx |= 2;
        }
    }
}

/* ------------------------------------------------------------------ */

void usb_init(int attach_host, const char *net)
{
    memset(&u, 0, sizeof(u));
    u.host_attached = attach_host;
    u.verbose = getenv("EMU84_USB_VERBOSE") != NULL;
    u.r4c = 0;
    u.r54 = 0x02;
    u.r4a = 0x20;
    u.intrtxe = 0xFFFF;
    u.intrrxe = 0xFFFE;
    u.intrusbe = 0x06;
    u.next_slot = SLOT_PS;
    u.next_frame = 1000000000ULL;
    u.ncm_ctrl_if = u.ncm_data_if = -1;
    net_open(attach_host ? net : NULL);
}

void usb_shutdown(void)
{
    if (u.host_attached)
        fprintf(stderr, "usb: frames to device %lu, from device %lu, bad NTBs %lu\n",
                u.frames_to_dev, u.frames_from_dev, u.ntb_bad);
    if (u.net_fd >= 0)
        close(u.net_fd);
}

void usb_tick(uint64_t now)
{
    while (now >= u.next_frame) {
        u.next_frame += 1000000000ULL;
        if (ctrl_active() && u.host_attached)
            u.frame = (u.frame + 1) & 0x7FF;
    }
    if (now < u.next_slot)
        return;
    u.next_slot = now + SLOT_PS;
    if (!u.host_attached)
        return;
    int act = ctrl_active();
    if (act && !u.connected) {
        u.connected = 1;
        u.t_active = now;
        u.state = H_IDLE;
        ulog("device connected");
    } else if (!act && u.connected) {
        u.connected = 0;
        u.state = H_IDLE;
        u.cx_active = 0;
        ulog("device disconnected");
    }
    if (!u.connected)
        return;
    cx_step();
    host_step();
    bulk_step();
}

uint64_t usb_next_event(void)
{
    return u.next_slot < u.next_frame ? u.next_slot : u.next_frame;
}
