/* isr.c - interrupt handler (page 0: must not call banked code) */
#include "hw.h"
#include "kernel.h"
#include "usb.h"

volatile uint32_t k_ticks;
static volatile uint32_t k_ms;          /* milliseconds, advanced by 125/64 per tick */
static uint8_t k_ms_frac;
volatile uint8_t k_on_pressed;
volatile uint8_t usb_ev_tx, usb_ev_rx, usb_ev_bus, usb_ev_line;

volatile uint8_t usb_irq_masked;

/* USB: latch the read-to-clear event registers for usb_poll().  If a source
 * stays asserted after that (behaviour not documented for every bit), mask
 * USB interrupts so the CPU cannot get stuck in the handler; usb_poll()
 * re-enables them. */
static void usb_irq(void)
{
    uint8_t st = port_usb_int;
    if (!(st & 0x10)) {
        usb_ev_tx |= port_usb_intrtx;
        usb_ev_rx |= port_usb_intrrx;
        usb_ev_bus |= port_usb_intrusb;
    }
    if (!(st & 0x04)) {
        uint8_t m = port_usb_line_mask;
        usb_ev_line |= port_usb_line_ev;
        port_usb_line_mask = 0;          /* acknowledge */
        port_usb_line_mask = m;
    }
    if ((port_usb_int & 0x1F) != 0x1F) {
        port_usb_proto_ie = 0;
        port_usb_line_mask = 0;
        usb_irq_masked = 1;
    }
}

void isr(void)
{
    uint8_t id = port_int_id;
    if (id & INT_TIMER1) {
        k_ticks++;
        k_ms_frac += 125;               /* 512 Hz: 1.953125 ms per tick */
        k_ms += k_ms_frac >> 6;
        k_ms_frac &= 63;
        port_int_mask = INT_ON | INT_POWER;              /* acknowledge */
        port_int_mask = INT_ON | INT_TIMER1 | INT_POWER;
    }
    if (id & INT_ON) {
        k_on_pressed = 1;
        port_int_mask = INT_TIMER1 | INT_POWER;
        port_int_mask = INT_ON | INT_TIMER1 | INT_POWER;
    }
    if ((port_usb_int & 0x14) != 0x14)
        usb_irq();
    usb_service(id & INT_TIMER1);
}

uint32_t k_now_ms(void)
{
    uint32_t t;
    __asm__("di");
    t = k_ms;
    __asm__("ei");
    return t;
}
