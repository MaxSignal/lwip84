/* power.c - delays and turning the calculator off (2nd+ON) */
#include "hw.h"
#include "kernel.h"
#include "usb.h"

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
