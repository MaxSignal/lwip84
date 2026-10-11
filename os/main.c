/* main.c - lwIP-84+ OS: start-up and the network test console.
 *
 * Keys:  Y= status        WINDOW ping gateway    ZOOM DNS lookup
 *        TRACE HTTP GET   GRAPH TCP echo (gateway:8084)
 *        MODE SNTP sync   STAT show time
 *        CLEAR clear      2nd+ON power off
 */
#include <string.h>
#include "hw.h"
#include "kernel.h"
#include "usb.h"
#include "ncm.h"
#include "demo.h"
#include "apps.h"
#include "lwIP.h"
#include "lwip/netif.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
#include "ti84p_usb_ethernet.h"

#define DEMO_HOST "example.com"

bool lwip_start(void);

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

static void ip_str(char *buf, const uint8_t *a)
{
    snprintf(buf, 16, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
}

static void status_line(void)
{
    static char last[CON_COLS + 1];
    char buf[CON_COLS + 1], ip[16];
    lwip_netif_info_t info;
    if (lwip_default_netif_info(&info) && info.has_ipv4) {
        ip_str(ip, info.ipv4_addr);
    } else {
        strcpy(ip, ncm_link_up() ? "dhcp..." : "no link");
    }
    snprintf(buf, sizeof(buf), "%-4s %s", usb_names[usb_state], ip);
    if (strcmp(buf, last)) {
        strcpy(last, buf);
        con_status(buf);
    }
}

static void show_info(void)
{
    con_printf("lwIP-CE %s\n", lwip_version_string());
    con_printf("usb %s err %u 4C=%02x 4D=%02x\n", usb_names[usb_state], usb_last_error,
               port_usb_status, port_usb_lines);
    con_printf("ntb rx %u bad %u fr %u/%u\n", ncm_stats.rx_ntbs, ncm_stats.rx_bad,
               ncm_stats.rx_frames, ncm_stats.tx_frames);
    if (eth_ncm_netif)
        con_printf("gw %s\n", ip4addr_ntoa(netif_ip4_gw(eth_ncm_netif)));
    con_printf("dns %s\n", ipaddr_ntoa(dns_getserver(0)));
    con_printf("heap %u free\n", heap_free_bytes());
}

static const char *gateway_str(void)
{
    static char buf[16];
    lwip_netif_info_t info;
    if (!lwip_default_netif_info(&info) || !info.has_ipv4_gateway)
        return "192.168.84.1";
    ip_str(buf, info.ipv4_gateway);
    return buf;
}

void main(void)
{
    uint8_t k, shift = 0;
    port_int_id = 0x00;                 /* map mode 0, timer 1 at 512 Hz */
    port_int_mask = INT_ON | INT_TIMER1 | INT_POWER;
    __asm__("ei");
    k_detect_platform();
    heap_init();
    lcd_init();
    con_clear();
    con_puts("lwIP-84+ " __DATE__ "\n");
    con_flush();
    if (!lwip_start())
        con_printf("lwip_start failed: %u\n", lwip_start_last_error());
    else if (!lwip_network_up())
        con_printf("network_up failed: %u\n", lwip_start_last_error());
    dbg_printf("lwip up, heap %u%s\n", heap_free_bytes(), k_is_emulator ? " (emu)" : "");
    if (usb_last_error)
        con_printf("usb start error %u\n", usb_last_error);
    con_puts("connect USB to a PC\n");
    for (;;) {
        lwip_service_events();
        demo_poll();
        app_poll();
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
            if (eth_ncm_netif)
                demo_ping(netif_ip_gw4(eth_ncm_netif));
            break;
        case K_ZOOM:
            demo_dns(DEMO_HOST);
            break;
        case K_TRACE:
            app_http_get(DEMO_HOST);
            break;
        case K_GRAPH:
            app_tcp_echo(gateway_str(), 8084);
            break;
        case K_MODE:
            app_sntp();
            break;
        case K_STAT:
            app_show_time();
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
