/* main.c - lwIP-84+ OS: start-up, the application menu and the network
 * test console.
 *
 * APPS opens the menu (web browser, SSH client, console).
 * Console keys:
 *        Y= status        WINDOW ping gateway    ZOOM DNS lookup
 *        TRACE HTTP GET   GRAPH TCP echo (gateway:8084)
 *        MODE SNTP sync   STAT show time   VARS download test (gateway:8085)
 *        MATH HTTPS GET (gateway:4433), 2nd+MATH https://example.com/   PRGM X25519 self test
 *        CLEAR clear      UP/DOWN scroll back   2nd+ON power off
 */
#include <string.h>
#include "hw.h"
#include "kernel.h"
#include "usb.h"
#include "ncm.h"
#include "demo.h"
#include "apps.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
#include "net84.h"
#include "tls84.h"
#include "kx84.h"
#include "input.h"
#include "ui.h"
#include "rng.h"
#include "font.h"

uint8_t browser_enter(void);
uint8_t browser_key(uint8_t k);
void browser_poll(void);
uint8_t ssh_enter(void);
uint8_t ssh_key(uint8_t k);
void ssh_poll(void);

enum { A_CONSOLE, A_MENU, A_WEB, A_SSH };
static uint8_t app;

#define DEMO_HOST "example.com"

static uint8_t main_r;
static const char *const usb_names[] = {"off", "wait", "dflt", "addr", "cfg"};

void k_delay_ms(uint16_t ms)
{
    uint32_t end = k_now_ms() + ms;
    while ((int32_t)(k_now_ms() - end) < 0)
        __asm__("halt");
}

static uint8_t on_key_down(void)
{
    return !(port_int_id & 0x08);
}

/* 2nd+ON.  The ON key is still held when we get here and the ON interrupt
 * is level-triggered, so going to sleep right away would wake the
 * calculator again at once (and key bounce can do the same): wait for the
 * release first, and only wake up for an ON press that is still down a few
 * milliseconds later. */
void k_power_off(void)
{
    uint16_t n;
    do
        k_delay_ms(30);
    while (on_key_down());
    usb_stop();
    port_usb_line_mask = 0;
    port_lcd_cmd = 0x02;                /* display off */
    port_cpu_speed = 0;
    for (;;) {
        __asm__("di");
        k_on_pressed = 0;
        k_on_masked = 0;
        port_int_mask = INT_ON;         /* port 03h bit 3 clear: low power on halt */
        __asm__("ei");
        __asm__("halt");
        if (!k_on_pressed)
            continue;
        for (n = 0; n < 2000 && on_key_down(); n++)
            ;                           /* the timer is still off */
        if (on_key_down())
            break;
    }
    __asm__("di");
    k_on_masked = 1;                    /* the ISR re-enables ON after the release */
    port_int_mask = INT_TIMER1 | INT_POWER;
    __asm__("ei");
    port_cpu_speed = 1;
    do
        k_delay_ms(30);
    while (on_key_down());
    k_on_pressed = 0;
    lcd_init();
    con_refresh();
    con_flush();
    usb_start();
}

static void status_line(void)
{
    static char last[CON_COLS + 1];
    char buf[CON_COLS + 1], ip[16];
    if (netif_default && !ip4_addr_isany_val(*netif_ip4_addr(netif_default))) {
        strcpy(ip, ip4addr_ntoa(netif_ip4_addr(netif_default)));
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
    con_printf("lwIP %s\n", LWIP_VERSION_STRING);
    con_printf("usb %s err %u 4C=%02x 4D=%02x\n", usb_names[usb_state], usb_last_error,
               port_usb_status, port_usb_lines);
    con_printf("ntb rx %u bad %u fr %u/%u\n", ncm_stats.rx_ntbs, ncm_stats.rx_bad,
               ncm_stats.rx_frames, ncm_stats.tx_frames);
    if (net84_netif)
        con_printf("gw %s\n", ip4addr_ntoa(netif_ip4_gw(net84_netif)));
    con_printf("dns %s\n", ipaddr_ntoa(dns_getserver(0)));
    con_printf("heap %u free, font %s\n", heap_free_bytes(), font_ok ? "ok" : "none");
#ifdef TI84P_DEBUG
    /* the font pages as they are on flash: first bytes and 16-bit sum of
       each page, to compare with the ROM image (tools/pagesums.py) */
    {
        uint8_t pg, b[64], i;
        uint16_t a, sum;
        for (pg = FONT_PAGE; pg < FONT_PAGE + 11; pg++) {
            sum = 0;
            for (a = 0; a < 0x4000; a += sizeof(b)) {
                far_read(pg, 0x4000 + a, b, sizeof(b));
                for (i = 0; i < sizeof(b); i++)
                    sum += b[i];
            }
            far_read(pg, 0x4000, b, 4);
            con_printf("pg %02x %02x%02x%02x%02x %04x\n", pg, b[0], b[1], b[2], b[3], sum);
        }
        /* test marks ("PG", page; tools/pagetest.sh) on pages 08h-3Fh:
           where=page it was sent as */
        for (pg = 0x08; pg < 0x40; pg++) {
            far_read(pg, 0x4000, b, 3);
            if (b[0] == 'P' && b[1] == 'G')
                con_printf("%02x=%02x ", pg, b[2]);
        }
        con_printf("marks\n");
    }
#endif
    if (usb_recoveries)
        con_printf("usb reconnected %u times\n", usb_recoveries);
}

static const char *gateway_str(void)
{
    static char buf[16];
    if (!netif_default || ip4_addr_isany_val(*netif_ip4_gw(netif_default)))
        return "192.168.84.1";
    strcpy(buf, ip4addr_ntoa(netif_ip4_gw(netif_default)));
    return buf;
}

static void menu_draw(void)
{
    ui_clear();
    ui_status("lwIP-84+");
    ui_text(1, " 1  Web browser");
    ui_text(2, " 2  SSH client");
    ui_text(3, " 3  Network console");
    ui_text(5, " 2nd+ON  power off");
    {
        char b[CON_COLS + 1];
        snprintf(b, sizeof(b), kx_ready() ? " key pair ready" : " key pair: %u%%", kx_progress());
        ui_text(7, b);
    }
    lcd_update();
}

static void app_switch(uint8_t a)
{
    app = a;
    if (a == A_MENU)
        kx_start();                     /* the browser or SSH will want the key pair */
    if ((a == A_WEB && !browser_enter()) || (a == A_SSH && !ssh_enter())) {
        app = A_MENU;
        menu_draw();
        ui_text(8, " not enough memory");
        lcd_update();
        return;
    }
    if (a == A_MENU)
        menu_draw();
    else if (a == A_CONSOLE)
        con_refresh();
}

/* a key for the menu or one of the applications; returns 1 if consumed */
static uint8_t app_key(uint8_t k)
{
    switch (app) {
    case A_MENU:
        if (k != K_2ND)
            in_2nd = 0;
        if (k == K_1)
            app_switch(A_WEB);
        else if (k == K_2)
            app_switch(A_SSH);
        else if (k == K_3 || k == K_CLEAR || k == K_MODE)
            app_switch(A_CONSOLE);
        else if (k == K_2ND)
            in_char(k);
        return 1;
    case A_WEB:
        if (!browser_key(k))
            app_switch(A_MENU);
        break;
    case A_SSH:
        if (!ssh_key(k))
            app_switch(A_MENU);
        break;
    default:
        if (k == K_APPS) {
            app_switch(A_MENU);
            return 1;
        }
        return 0;
    }
    if (k != K_2ND)
        in_2nd = 0;
    return 1;
}
void main(void)
{
    uint8_t k, ui_div = 0;
    uint32_t now, next_ui = 0, next_poll = 0;
    port_int_id = 0x00;                 /* map mode 0, timer 1 at 512 Hz */
    port_int_mask = INT_ON | INT_TIMER1 | INT_POWER;
    __asm__("ei");
    k_detect_platform();
    heap_init();
    lcd_init();
    con_clear();
    con_puts("lwIP-84+ " __DATE__ "\n");
    con_flush();
    font_init();
    if (!font_ok)
        con_puts("no font: non-ASCII text as ?\n");
    net84_init();
    dbg_printf("lwip up, heap %u%s\n", heap_free_bytes(), k_is_emulator ? " (emu)" : "");
    if (usb_last_error)
        con_printf("usb start error %u\n", usb_last_error);
    con_puts("connect USB to a PC\nAPPS: web browser, SSH\n");
    for (;;) {
        net84_poll();
        now = k_now_ms();
        /* the rest every 10 ms: each of these is a far call, and the loop
         * spins a few hundred times a second while packets flow */
        if ((int32_t)(now - next_poll) >= 0) {
            next_poll = now + 10;
            usb_log_print();
            demo_poll();
            app_poll();
            tls84_poll();
            kx_poll();
            browser_poll();
            ssh_poll();
        }
        if ((int32_t)(now - next_ui) >= 0) {
            next_ui = now + 20;
            k = kbd_get();
            if (k) {
                uint8_t r;
                __asm__("ld a, r");     /* key timing and the refresh register */
                __asm__("ld (_main_r), a");
                r = main_r ^ (uint8_t)now;
                rng_mix(&r, 1);
            }
            if (++ui_div >= 12 && app == A_CONSOLE) {   /* status line every ~250 ms */
                ui_div = 0;
                status_line();
            }
        } else {
            k = K_NONE;
        }
        if (k_on_pressed) {
            k_on_pressed = 0;
            if (in_2nd) {
                in_2nd = 0;
                k_power_off();
                if (app != A_CONSOLE)
                    app_switch(app);
            }
        }
        if (k && app_key(k))
            k = K_NONE;
        switch (k) {
        case K_2ND:
            in_char(k);
            break;
        case K_YEQU:
            show_info();
            break;
        case K_WINDOW:
            if (net84_netif)
                demo_ping(netif_ip_gw4(net84_netif));
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
        case K_VARS:
            app_download(gateway_str(), 8085);
            break;
        case K_PRGM:
            app_selftest();
            break;
        case K_MATH:
            /* 2nd, MATH: a real site over the Internet */
            if (in_2nd)
                app_https_get("example.com", 443);
            else
                app_https_get(gateway_str(), 4433);
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
        case K_UP:
            con_scroll(1);
            break;
        case K_DOWN:
            con_scroll(-1);
            break;
        }
        if (k != K_NONE && k != K_2ND)
            in_2nd = 0;
        if (app == A_CONSOLE)
            con_flush();
        /* sleep until the next tick or USB interrupt, but keep polling while
         * USB traffic is flowing (the FIFOs are single-buffered) */
        if (!k && (uint32_t)(k_now_ms() - usb_last_activity) > 50)
            __asm__("halt");
    }
}
