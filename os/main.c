/* main.c - lwIP-84+ OS entry */
#include "hw.h"
#include "kernel.h"

void k_delay_ms(uint16_t ms)
{
    uint32_t end = k_now_ms() + ms;
    while ((int32_t)(k_now_ms() - end) < 0)
        __asm__("halt");
}

void main(void)
{
    uint8_t k;
    port_int_id = 0x00;                 /* map mode 0, timer 1 at 512 Hz */
    port_int_mask = INT_ON | INT_TIMER1 | INT_POWER;
    __asm__("ei");
    lcd_init();
    con_clear();
    con_puts("lwIP-84+ OS\n");
    con_printf("hw %02x, %u KiB flash\n", port_hw_type,
               (port_hw_type & 1) ? 2048 : 1024);
    con_flush();
    dbg_printf("boot ok\n");
    for (;;) {
        k = kbd_get();
        if (k != K_NONE) {
            con_printf("key %u t=%lu\n", k, k_now_ms());
            dbg_printf("key %u\n", k);
        }
        con_flush();
        __asm__("halt");
    }
}
