/* main.c - lwIP-84+ OS: start-up and the network test console.
 *
 * Keys:  Y= status   WINDOW ping gateway   ZOOM DNS lookup
 *        TRACE HTTP GET   GRAPH TCP echo (gateway:8084)
 *        CLEAR clear screen   2nd+ON power off
 */
#include <string.h>
#include "hw.h"
#include "kernel.h"
#include "usb.h"
#include "ncm.h"
#include "demo.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/timeouts.h"
#include "lwip/ip4_addr.h"
#include "netif/ethernet.h"
#include "drivers/usb_ethernet.h"

#define DEMO_HOST "example.com"

static struct netif ncm_netif;
static const char *const usb_names[] = {"off", "wait", "dflt", "addr", "cfg"};

void k_delay_ms(uint16_t ms)
{
    uint32_t end = k_now_ms() + ms;
    while ((int32_t)(k_now_ms() - end) < 0)
        __asm__("halt");
}

void k_power_off(void)
{
    usb_stop();
    port_lcd_cmd = 0x02;                /* display off */
    __asm__("di");
    k_on_pressed = 0;
    port_int_mask = INT_ON;             /* low power while halted, wake on ON */
    __asm__("ei");
    while (!k_on_pressed)
        __asm__("halt");
    port_int_mask = INT_ON | INT_TIMER1 | INT_POWER;
    k_delay_ms(50);
    lcd_init();
    con_flush();
    usb_start();
}

static void status_line(void)
{
    static char last[CON_COLS + 1];
    char buf[CON_COLS + 1];
    const ip4_addr_t *ip = netif_ip4_addr(&ncm_netif);
    snprintf(buf, sizeof(buf), "%-4s %s", usb_names[usb_state],
             ip4_addr_isany(ip) ? (ncm_link_up() ? "dhcp..." : "no link") : ip4addr_ntoa(ip));
    if (strcmp(buf, last)) {
        strcpy(last, buf);
        con_status(buf);
    }
}

static void show_info(void)
{
    con_printf("usb %s err %u 4C=%02x 4D=%02x\n", usb_names[usb_state], usb_last_error,
               port_usb_status, port_usb_lines);
    con_printf("ntb rx %u bad %u fr %u/%u\n", ncm_stats.rx_ntbs, ncm_stats.rx_bad,
               ncm_stats.rx_frames, ncm_stats.tx_frames);
    con_printf("gw %s\n", ip4addr_ntoa(netif_ip4_gw(&ncm_netif)));
    con_printf("dns %s\n", ipaddr_ntoa(dns_getserver(0)));
    con_printf("heap %u free\n", heap_free_bytes());
}

void main(void)
{
    uint8_t k, shift = 0;
    err_t e;
    port_int_id = 0x00;                 /* map mode 0, timer 1 at 512 Hz */
    port_int_mask = INT_ON | INT_TIMER1 | INT_POWER;
    __asm__("ei");
    k_detect_platform();
    heap_init();
    lcd_init();
    con_clear();
    con_puts("lwIP-84+ " __DATE__ "\n");
    con_flush();
    e = lwip_init();
    if (e != ERR_OK)
        con_printf("lwip_init: %d\n", e);
    dbg_printf("lwip_init %d heap %u%s\n", e, heap_free_bytes(), k_is_emulator ? " (emu)" : "");
    netif_add(&ncm_netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL, netif_ncm_init,
              ethernet_input);
    netif_set_default(&ncm_netif);
    netif_set_up(&ncm_netif);
    dhcp_start(&ncm_netif);
    usb_start();
    if (usb_last_error)
        con_printf("usb start error %u\n", usb_last_error);
    con_puts("connect USB to a PC\n");
    for (;;) {
        usb_poll();
        netif_ncm_poll(&ncm_netif);
        sys_check_timeouts();
        demo_poll();
        status_line();
        if (k_on_pressed) {
            k_on_pressed = 0;
            if (shift)
                k_power_off();
            shift = 0;
        }
        k = kbd_get();
        switch (k) {
        case K_2ND:
            shift ^= 1;
            break;
        case K_YEQU:
            show_info();
            break;
        case K_WINDOW:
            demo_ping(netif_ip_gw4(&ncm_netif));
            break;
        case K_ZOOM:
            demo_dns(DEMO_HOST);
            break;
        case K_TRACE:
            demo_http_get(DEMO_HOST);
            break;
        case K_GRAPH:
            demo_tcp_echo(ip4addr_ntoa(netif_ip4_gw(&ncm_netif)), 8084);
            break;
        case K_CLEAR:
            con_clear();
            break;
        }
        if (k != K_NONE && k != K_2ND)
            shift = 0;
        con_flush();
        if (!k)
            __asm__("halt");            /* wait for the next tick or USB event */
    }
}
