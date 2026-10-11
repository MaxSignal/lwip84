/* demo.c - small network tests on top of the raw lwIP API:
 * ping, DNS lookup, HTTP GET and a TCP echo test. */
#include <string.h>
#include "kernel.h"
#include "demo.h"
#include "lwip/raw.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ip4.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

/* ------------------------------------------------------------------ */
/* ping                                                                */

static struct raw_pcb *ping_pcb;
static uint16_t ping_seq;
static uint32_t ping_sent_at;
static uint8_t ping_count;
static uint32_t ping_next;
static ip_addr_t ping_target;

static u8_t ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr)
{
    struct icmp_echo_hdr echo;
    uint16_t hlen;
    (void)arg;
    (void)pcb;
    if (p->tot_len < 20 + sizeof(echo))
        return 0;
    hlen = (pbuf_get_at(p, 0) & 0x0F) * 4;
    pbuf_copy_partial(p, &echo, sizeof(echo), hlen);
    if (echo.type != ICMP_ER || echo.id != 0x8484 || echo.seqno != lwip_htons(ping_seq))
        return 0;
    con_printf("reply %s %lu ms\n", ipaddr_ntoa(addr), k_now_ms() - ping_sent_at);
    pbuf_free(p);
    return 1;
}

static void ping_send(void)
{
    struct pbuf *p = pbuf_alloc(PBUF_IP, sizeof(struct icmp_echo_hdr) + 32, PBUF_RAM);
    struct icmp_echo_hdr *e;
    uint8_t i;
    if (!p)
        return;
    e = (struct icmp_echo_hdr *)p->payload;
    e->type = ICMP_ECHO;
    e->code = 0;
    e->chksum = 0;
    e->id = 0x8484;
    e->seqno = lwip_htons(++ping_seq);
    for (i = 0; i < 32; i++)
        ((uint8_t *)(e + 1))[i] = 'a' + (i % 26);
    e->chksum = inet_chksum(e, p->len);
    ping_sent_at = k_now_ms();
    raw_sendto(ping_pcb, p, &ping_target);
    pbuf_free(p);
}

void demo_ping(const ip_addr_t *target)
{
    if (!ping_pcb) {
        ping_pcb = raw_new(IP_PROTO_ICMP);
        if (!ping_pcb)
            return;
        raw_recv(ping_pcb, ping_recv, NULL);
        raw_bind(ping_pcb, IP_ADDR_ANY);
    }
    ip_addr_copy(ping_target, *target);
    con_printf("ping %s\n", ipaddr_ntoa(target));
    ping_count = 4;
    ping_next = k_now_ms();
}

/* ------------------------------------------------------------------ */
/* TCP client: HTTP GET or echo                                        */

static struct tcp_pcb *cli_pcb;
static char cli_req[96];
static uint16_t cli_rx;
static char cli_host[40];
static uint16_t cli_port;
static uint8_t cli_show_lines;

static void cli_close(struct tcp_pcb *pcb)
{
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    if (tcp_close(pcb) != ERR_OK)
        tcp_abort(pcb);
    cli_pcb = NULL;
}

static err_t cli_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    char buf[48];
    uint16_t off = 0, n, i;
    (void)arg;
    (void)err;
    if (!p) {
        con_printf("closed, %u bytes\n", cli_rx);
        cli_close(pcb);
        return ERR_OK;
    }
    /* print the first lines of the reply */
    while (cli_show_lines && off < p->tot_len) {
        n = pbuf_copy_partial(p, buf, sizeof(buf) - 1, off);
        for (i = 0; i < n && buf[i] != '\n'; i++)
            ;
        if (i < n) {
            buf[i] = 0;
            if (i && buf[i - 1] == '\r')
                buf[i - 1] = 0;
            con_printf("%s\n", buf);
            cli_show_lines--;
            off += i + 1;
        } else {
            buf[n] = 0;
            con_puts(buf);
            off += n;
        }
    }
    cli_rx += p->tot_len;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t cli_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK)
        return err;
    con_printf("connected\n");
    tcp_write(pcb, cli_req, strlen(cli_req), TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    return ERR_OK;
}

static void cli_err_cb(void *arg, err_t err)
{
    (void)arg;
    con_printf("tcp error %d\n", err);
    cli_pcb = NULL;
}

static void cli_connect(const ip_addr_t *addr)
{
    cli_pcb = tcp_new();
    if (!cli_pcb) {
        con_puts("tcp_new failed\n");
        return;
    }
    cli_rx = 0;
    tcp_recv(cli_pcb, cli_recv_cb);
    tcp_err(cli_pcb, cli_err_cb);
    con_printf("connect %s:%u\n", ipaddr_ntoa(addr), cli_port);
    if (tcp_connect(cli_pcb, addr, cli_port, cli_connected_cb) != ERR_OK) {
        con_puts("connect failed\n");
        cli_close(cli_pcb);
    }
}

static void cli_dns_cb(const char *name, const ip_addr_t *addr, void *arg)
{
    (void)arg;
    if (!addr) {
        con_printf("%s: not found\n", name);
        return;
    }
    con_printf("%s = %s\n", name, ipaddr_ntoa(addr));
    if (cli_port)
        cli_connect(addr);
}

/* resolve host, then (port != 0) connect and send req */
static void cli_start(const char *host, uint16_t port, const char *req, uint8_t lines)
{
    ip_addr_t addr;
    err_t e;
    if (cli_pcb) {
        con_puts("busy\n");
        return;
    }
    strncpy(cli_host, host, sizeof(cli_host) - 1);
    cli_port = port;
    cli_show_lines = lines;
    strncpy(cli_req, req, sizeof(cli_req) - 1);
    e = dns_gethostbyname(cli_host, &addr, cli_dns_cb, NULL);
    if (e == ERR_OK)
        cli_dns_cb(cli_host, &addr, NULL);
    else if (e == ERR_INPROGRESS)
        con_printf("resolving %s\n", cli_host);
    else
        con_printf("dns error %d\n", e);
}

void demo_dns(const char *host)
{
    cli_start(host, 0, "", 0);
}

void demo_http_get(const char *host)
{
    char req[96];
    snprintf(req, sizeof(req), "GET / HTTP/1.0\r\nHost: %s\r\nUser-Agent: lwIP-84+\r\n\r\n", host);
    cli_start(host, 80, req, 3);
}

void demo_tcp_echo(const char *host, uint16_t port)
{
    cli_start(host, port, "hello from a TI-84 Plus\n", 4);
}

void demo_poll(void)
{
    if (ping_count && (int32_t)(k_now_ms() - ping_next) >= 0) {
        ping_count--;
        ping_next = k_now_ms() + 1000;
        ping_send();
    }
}
