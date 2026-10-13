/* sock84.c - TCP and TLS client sockets on lwIP's altcp API (see sock84.h) */
#include <string.h>
#include <stdlib.h>
#include "lwip/opt.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tcp.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/ip4_addr.h"
#include "kernel.h"
#include "sock84.h"
#include "tls84.h"

#define HOST_MAX 64
#define TXQ_MAX 2048                    /* writes waiting for the send buffer */
#define EVQ 6
#define EV_ERROR 0x80                   /* in ev[]: the error, not a state */

struct sock {
    struct sock *next;
    struct altcp_pcb *pcb;
    sock_cb_t cb;
    uint8_t type, state;
    uint8_t ev_n, ev[EVQ];              /* events not yet delivered, in order */
    uint8_t err_comp;
    int8_t err;
    uint16_t port;
    uint32_t deadline;
    ip_addr_t addr;
    struct pbuf *rx;                    /* received, not yet read */
    uint8_t *txq;
    uint16_t txq_n;
    char host[HOST_MAX];
};

static struct sock *socks;

static uint8_t alive(const struct sock *s)
{
    const struct sock *p;
    for (p = socks; p; p = p->next)
        if (p == s)
            return 1;
    return 0;
}

static void queue_ev(struct sock *s, uint8_t ev)
{
    if (s->ev_n < EVQ)
        s->ev[s->ev_n++] = ev;
    else
        s->ev[EVQ - 1] = ev;
}

static void set_state(struct sock *s, uint8_t st)
{
    if (s->state == st)
        return;
    s->state = st;
    queue_ev(s, st);
}

static void detach(struct sock *s)
{
    if (!s->pcb)
        return;
    altcp_arg(s->pcb, NULL);
    altcp_recv(s->pcb, NULL);
    altcp_sent(s->pcb, NULL);
    altcp_err(s->pcb, NULL);
    if (altcp_close(s->pcb) != ERR_OK)
        altcp_abort(s->pcb);
    s->pcb = NULL;
}

/* the pcb is closed later, from sock_poll(): not from inside its callbacks */
static void fail(struct sock *s, uint8_t comp, err_t err)
{
    if (s->state == SOCK_ERROR)
        return;
    s->err_comp = comp;
    s->err = err;
    queue_ev(s, EV_ERROR);
    set_state(s, SOCK_ERROR);
}

/* ------------------------------------------------------------------ */
/* lwIP callbacks                                                      */

static void flush_tx(struct sock *s)
{
    uint16_t room, n;
    if (!s->pcb || !s->txq_n || s->state != SOCK_CONNECTED)
        return;
    room = altcp_sndbuf(s->pcb);
    n = s->txq_n < room ? s->txq_n : room;
    if (!n || altcp_write(s->pcb, s->txq, n, TCP_WRITE_FLAG_COPY) != ERR_OK)
        return;
    memmove(s->txq, s->txq + n, s->txq_n - n);
    s->txq_n -= n;
    altcp_output(s->pcb);
}

static err_t on_sent(void *arg, struct altcp_pcb *pcb, u16_t len)
{
    (void)pcb;
    (void)len;
    flush_tx((struct sock *)arg);
    return ERR_OK;
}

static err_t on_recv(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err)
{
    struct sock *s = arg;
    (void)pcb;
    if (!s) {
        if (p)
            pbuf_free(p);
        return ERR_OK;
    }
    if (!p || err != ERR_OK) {
        if (p)
            pbuf_free(p);
        set_state(s, SOCK_CLOSED);      /* the peer is done; closed in sock_poll() */
        return ERR_OK;
    }
    /* kept as it is; the window opens when the application reads */
    if (s->rx)
        pbuf_cat(s->rx, p);
    else
        s->rx = p;
    return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
    struct sock *s = arg;
    if (!s)
        return;
    s->pcb = NULL;                      /* lwIP has freed it */
    if (s->state == SOCK_CONNECTED && (err == ERR_CLSD || err == ERR_RST))
        set_state(s, SOCK_CLOSED);
    else
        fail(s, s->type == SOCK_TLS && s->state < SOCK_CONNECTED ? SOCK_ERR_TLS : SOCK_ERR_TCP, err);
}

static err_t on_connected(void *arg, struct altcp_pcb *pcb, err_t err)
{
    struct sock *s = arg;
    (void)pcb;
    if (!s)
        return ERR_OK;
    if (err != ERR_OK) {
        fail(s, SOCK_ERR_TCP, err);
        return ERR_OK;
    }
    set_state(s, SOCK_CONNECTED);
    flush_tx(s);
    return ERR_OK;
}

static void start_connect(struct sock *s)
{
    s->pcb = s->type == SOCK_TLS ? tls84_new(s->host) : altcp_tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!s->pcb) {
        fail(s, SOCK_ERR_MEM, ERR_MEM);
        return;
    }
    altcp_arg(s->pcb, s);
    altcp_recv(s->pcb, on_recv);
    altcp_sent(s->pcb, on_sent);
    altcp_err(s->pcb, on_err);
    set_state(s, SOCK_CONNECTING);
    if (altcp_connect(s->pcb, &s->addr, s->port, on_connected) != ERR_OK)
        fail(s, SOCK_ERR_TCP, ERR_CONN);
}

static void on_dns(const char *name, const ip_addr_t *addr, void *arg)
{
    struct sock *s = arg;
    (void)name;
    if (!alive(s) || s->state != SOCK_RESOLVING)
        return;
    if (!addr) {
        fail(s, SOCK_ERR_DNS, ERR_VAL);
        return;
    }
    s->addr = *addr;
    start_connect(s);
}

static uint8_t net_ready(void)
{
    return netif_default && netif_is_up(netif_default) && netif_is_link_up(netif_default) &&
           !ip4_addr_isany_val(*netif_ip4_addr(netif_default));
}

static void resolve(struct sock *s)
{
    err_t e;
    if (ip4addr_aton(s->host, ip_2_ip4(&s->addr))) {
        IP_SET_TYPE_VAL(s->addr, IPADDR_TYPE_V4);
        start_connect(s);
        return;
    }
    set_state(s, SOCK_RESOLVING);
    e = dns_gethostbyname(s->host, &s->addr, on_dns, s);
    if (e == ERR_OK)
        start_connect(s);
    else if (e != ERR_INPROGRESS)
        fail(s, SOCK_ERR_DNS, e);
}

/* ------------------------------------------------------------------ */
/* API                                                                 */

struct sock *sock_new(uint8_t type, uint32_t timeout_ms, sock_cb_t cb)
{
    struct sock *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->type = type;
    s->cb = cb;
    s->deadline = timeout_ms;
    s->next = socks;
    socks = s;
    return s;
}

int8_t sock_connect(struct sock *s, const char *host, uint16_t port)
{
    if (!s || s->state != SOCK_INIT || strlen(host) >= HOST_MAX)
        return -1;
    strcpy(s->host, host);
    s->port = port;
    s->deadline = k_now_ms() + s->deadline;
    if (net_ready()) {
        resolve(s);
    } else {
        set_state(s, SOCK_WAITING_NET);
    }
    return s->state == SOCK_ERROR ? -1 : 0;
}

int8_t sock_write(struct sock *s, const void *data, uint16_t len)
{
    if (!s || s->state > SOCK_CONNECTED)
        return -1;
    if (!s->txq_n && s->state == SOCK_CONNECTED && s->pcb && altcp_sndbuf(s->pcb) >= len) {
        if (altcp_write(s->pcb, data, len, TCP_WRITE_FLAG_COPY) != ERR_OK)
            return -1;
        altcp_output(s->pcb);
        return 0;
    }
    /* before the connection is up, or the send buffer is full */
    if (!s->txq)
        s->txq = malloc(TXQ_MAX);
    if (!s->txq || s->txq_n + len > TXQ_MAX)
        return -1;
    memcpy(s->txq + s->txq_n, data, len);
    s->txq_n += len;
    flush_tx(s);
    return 0;
}

uint16_t sock_available(const struct sock *s)
{
    return s && s->rx ? s->rx->tot_len : 0;
}

uint16_t sock_read(struct sock *s, void *buf, uint16_t len)
{
    uint16_t n;
    if (!s || !s->rx)
        return 0;
    n = pbuf_copy_partial(s->rx, buf, len, 0);
    s->rx = pbuf_free_header(s->rx, n);
    if (s->pcb)
        altcp_recved(s->pcb, n);
    return n;
}

uint8_t sock_state(const struct sock *s)
{
    return s ? s->state : SOCK_ERROR;
}

uint8_t sock_is_active(const struct sock *s)
{
    return s && (s->state <= SOCK_CONNECTED || s->rx);
}

void sock_free(struct sock *s)
{
    struct sock **pp;
    if (!s)
        return;
    for (pp = &socks; *pp; pp = &(*pp)->next)
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    detach(s);
    if (s->rx)
        pbuf_free(s->rx);
    free(s->txq);
    free(s);
}

void sock_poll(void)
{
    struct sock *s, *next;
    uint32_t now = k_now_ms();
    for (s = socks; s; s = next) {
        next = s->next;
        if (s->state == SOCK_WAITING_NET && net_ready())
            resolve(s);
        if (s->state > SOCK_INIT && s->state < SOCK_CONNECTED &&
            (int32_t)(now - s->deadline) >= 0)
            fail(s, SOCK_ERR_TIMEOUT, ERR_TIMEOUT);
        if (s->state >= SOCK_CLOSED)
            detach(s);
        else
            flush_tx(s);
    }
    /* deliver events; a callback may free its socket (or another one) */
again:
    for (s = socks; s; s = s->next) {
        if (s->ev_n && s->cb) {
            uint8_t ev = s->ev[0];
            s->ev_n--;
            memmove(s->ev, s->ev + 1, s->ev_n);
            if (ev == EV_ERROR)
                s->cb(s, SOCK_EV_ERROR, s->err_comp, s->err);
            else
                s->cb(s, SOCK_EV_STATE, ev, 0);
            goto again;
        }
    }
}
