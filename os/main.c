/* main.c - lwIP-84+ OS entry */
#include "hw.h"
#include "kernel.h"
#include "usb.h"
#include "ncm.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/timeouts.h"
#include "lwip/ip4_addr.h"
#include "netif/ethernet.h"
#include "drivers/usb_ethernet.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"

void k_delay_ms(uint16_t ms)
{
    uint32_t end = k_now_ms() + ms;
    while ((int32_t)(k_now_ms() - end) < 0)
        __asm__("halt");
}

static struct netif ncm_netif;

static void show_status(void)
{
    static uint32_t last_ip = 1;
    uint32_t ip = ip4_addr_get_u32(netif_ip4_addr(&ncm_netif));
    if (ip != last_ip) {
        last_ip = ip;
        con_printf("ip %s\n", ip4addr_ntoa(netif_ip4_addr(&ncm_netif)));
        dbg_printf("ip %s\n", ip4addr_ntoa(netif_ip4_addr(&ncm_netif)));
    }
}

/* --- quick TCP test: connect to the host (192.168.84.1:8084), send a line,
 * print what comes back --- */
static struct tcp_pcb *test_pcb;

static err_t test_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    char buf[64];
    uint16_t n;
    (void)arg;
    (void)err;
    if (!p) {
        con_puts("tcp: closed\n");
        dbg_printf("tcp closed\n");
        tcp_close(pcb);
        test_pcb = NULL;
        return ERR_OK;
    }
    n = pbuf_copy_partial(p, buf, sizeof(buf) - 1, 0);
    buf[n] = 0;
    con_printf("rx: %s", buf);
    dbg_printf("tcp rx %u: %s", p->tot_len, buf);
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t test_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    static const char msg[] = "hello from a TI-84 Plus\n";
    (void)arg;
    con_printf("tcp: connected %d\n", err);
    dbg_printf("tcp connected %d\n", err);
    tcp_write(pcb, msg, sizeof(msg) - 1, 0);
    tcp_output(pcb);
    return ERR_OK;
}

static void test_err(void *arg, err_t err)
{
    (void)arg;
    con_printf("tcp: error %d\n", err);
    dbg_printf("tcp error %d\n", err);
    test_pcb = NULL;
}

static void tcp_test(void)
{
    ip_addr_t host;
    if (test_pcb)
        return;
    IP4_ADDR(&host, 192, 168, 84, 1);
    test_pcb = tcp_new();
    tcp_recv(test_pcb, test_recv);
    tcp_err(test_pcb, test_err);
    tcp_connect(test_pcb, &host, 8084, test_connected);
    con_puts("tcp: connecting\n");
    dbg_printf("tcp connecting\n");
}

void main(void)
{
    uint8_t k, last_state = 0xFF;
    err_t e;
    port_int_id = 0x00;                 /* map mode 0, timer 1 at 512 Hz */
    port_int_mask = INT_ON | INT_TIMER1 | INT_POWER;
    __asm__("ei");
    heap_init();
    lcd_init();
    con_clear();
    con_puts("lwIP-84+ OS\n");
    con_flush();
    e = lwip_init();
    con_printf("lwip_init %d heap %u\n", e, heap_free_bytes());
    dbg_printf("lwip_init %d heap %u\n", e, heap_free_bytes());
    netif_add(&ncm_netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL, netif_ncm_init,
              ethernet_input);
    netif_set_default(&ncm_netif);
    netif_set_up(&ncm_netif);
    dhcp_start(&ncm_netif);
    usb_start();
    con_flush();
    for (;;) {
        usb_poll();
        netif_ncm_poll(&ncm_netif);
        sys_check_timeouts();
        if (usb_state != last_state) {
            last_state = usb_state;
            dbg_printf("usb state %u\n", usb_state);
        }
        show_status();
        k = kbd_get();
        if (k == K_ENTER)
            tcp_test();
        if (k == K_CLEAR) {
            usb_stop();
            con_puts("usb stopped\n");
        }
        con_flush();
    }
}
