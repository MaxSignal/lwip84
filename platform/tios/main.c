/* main.c - the TI-OS applications: one of the web browser (TIOS_BROWSER)
 * and the SSH client (TIOS_SSH) on the kernel and the network library,
 * without the menu and the console of the standalone OS.  Leaving the
 * application (MODE) goes back to TI-OS (platform/tios/launch.s). */
#include <string.h>
#include "hw.h"
#include "kernel.h"
#include "usb.h"
#include "net84.h"
#include "tls84.h"
#include "kx84.h"
#include "input.h"
#include "ui.h"
#include "rng.h"
#include "font.h"
#include "dict.h"

#ifdef TIOS_BROWSER
uint8_t browser_enter(void);
uint8_t browser_key(uint8_t k);
void browser_poll(void);
#define app_enter browser_enter
#define app_key browser_key
#define app_poll browser_poll
#else
uint8_t ssh_enter(void);
uint8_t ssh_key(uint8_t k);
void ssh_poll(void);
#define app_enter ssh_enter
#define app_key ssh_key
#define app_poll ssh_poll
#endif

void __app_exit(void);                  /* platform/tiplatform/os/crt0.s */

static uint8_t main_r;

extern uint8_t __app_save[];            /* platform/tios/crt0.s */

/* TI-OS's settings of the USB controller's outer ports, taken before the
 * app sets it up: given back so that TI-OS sees the cable again */
static uint8_t usb_gpio, usb_pull, usb_vpwr, usb_ctrl, usb_line_mask, usb_proto_ie;

static void quit(void)
{
    __asm__("di");
    usb_stop();
    port_usb_proto_ie = 0;
    port_usb_line_mask = 0;
    port_usb_ctrl = usb_ctrl;
    port_usb_vpwr = usb_vpwr;
    port_usb_pull = usb_pull;
    port_gpio = usb_gpio;
    port_usb_line_mask = usb_line_mask;
    port_usb_proto_ie = usb_proto_ie;
    __app_save[9] = lcd_contrast;       /* and the contrast as it is now */
    __app_exit();
}

void main(void)
{
    uint8_t k;
    uint32_t now, next_ui = 0, next_poll = 0;
    port_int_id = 0x00;                 /* timer 1 at 512 Hz */
    port_int_mask = INT_ON | INT_TIMER1 | INT_POWER;
    __asm__("ei");
    k_detect_platform();
    usb_gpio = port_gpio;
    usb_pull = port_usb_pull;
    usb_vpwr = port_usb_vpwr;
    usb_ctrl = port_usb_ctrl;
    usb_line_mask = port_usb_line_mask;
    usb_proto_ie = port_usb_proto_ie;
    heap_init();
    lcd_contrast = __app_save[9] <= 39 ? __app_save[9] : 24;   /* TI-OS's */
    lcd_init();
    con_clear();
    font_init();
    dict_init();
    net84_init();
    dbg_printf("lwip up, heap %u%s\n", heap_free_bytes(), k_is_emulator ? " (emu)" : "");
    if (!app_enter()) {
        ui_clear();
        ui_text(3, " not enough memory");
        lcd_update();
        k_delay_ms(2000);
        quit();
    }
    for (;;) {
        net84_poll();
        now = k_now_ms();
        if ((int32_t)(now - next_poll) >= 0) {
            next_poll = now + 10;
            tls84_poll();
            kx_poll();
            app_poll();
        }
        k = K_NONE;
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
        }
        if (k_on_pressed) {
            k_on_pressed = 0;
            if (in_2nd) {
                in_2nd = 0;
                k_power_off();
                app_enter();            /* draw it again */
            }
        }
        if (k && in_contrast_key(k))
            k = K_NONE;
        if (k) {
            if (!app_key(k))
                quit();
            if (k != K_2ND)
                in_2nd = 0;
        }
    }
}
