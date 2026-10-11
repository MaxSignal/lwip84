/* isr.c - interrupt handler (page 0: must not call banked code) */
#include "hw.h"
#include "kernel.h"

volatile uint32_t k_ticks;
volatile uint8_t k_on_pressed;

void isr(void)
{
    uint8_t id = port_int_id;
    if (id & INT_TIMER1) {
        k_ticks++;
        port_int_mask = INT_ON | INT_POWER;              /* acknowledge */
        port_int_mask = INT_ON | INT_TIMER1 | INT_POWER;
    }
    if (id & INT_ON) {
        k_on_pressed = 1;
        port_int_mask = INT_TIMER1 | INT_POWER;
        port_int_mask = INT_ON | INT_TIMER1 | INT_POWER;
    }
}

uint32_t k_now_ms(void)
{
    uint32_t t;
    __asm__("di");
    t = k_ticks;
    __asm__("ei");
    return (t * 125u) >> 6;
}
