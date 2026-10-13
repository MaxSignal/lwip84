/* usb_ctl.c - USB controller power-up/shutdown and the debug log printer
 * (banked; the interrupt-driven core is usb.c in page 0). */
#include <string.h>
#include "hw.h"
#include "kernel.h"
#include "usb.h"

static uint8_t wait_port(uint8_t port_sel, uint8_t mask, uint8_t want, uint16_t ms)
{
    uint32_t end = k_now_ms() + ms;
    for (;;) {
        uint8_t v = port_sel ? port_usb_lines : port_usb_status;
        if ((v & mask) == want)
            return 1;
        if ((int32_t)(k_now_ms() - end) >= 0)
            return 0;
    }
}

void usb_stop(void)
{
    __asm__("di");
    port_usb_proto_ie = 0;
    port_usb_status = 0x00;
    port_usb_ctrl = 0x02;               /* controller and 48 MHz crystal off */
    usb_state = USB_OFF;
    __asm__("ei");
    ncm_enable(0);
}

void usb_start(void)
{
    usb_last_error = 0;
    usb_stop();
    port_usb_line_mask = 0;
    port_usb_pull = 0x20;               /* default pull-down setting */
    port_usb_vpwr = 0x00;               /* we never drive VBus */
    usb_ev_tx = usb_ev_rx = usb_ev_bus = 0;
    port_usb_ctrl = 0x00;
    k_delay_ms(5);
    if (port_gpio & 0x08)               /* older hardware revision */
        port_usb_ctrl = 0x44;
    port_usb_ctrl = 0xC4;
    if (!wait_port(0, 0xBF, 0x12, 200)) {
        usb_last_error = 1;             /* controller did not come up */
        dbg_printf("usb: 4C=%02x after power-up\n", port_usb_status);
    }
    port_usb_status = 0x08;
    if (!wait_port(0, 0x08, 0x08, 200))
        usb_last_error = 2;
    port_usb_faddr = 0;
    port_usb_intrtxe = 0x01 | (1 << EP_BULK) | (1 << EP_NOTIFY);
    port_usb_intrrxe = 1 << EP_BULK;
    port_usb_intrusbe = 0x05;           /* suspend + reset, as TI-OS in peripheral mode */
    __asm__("di");
    usb_reset_state();
    usb_state = USB_WAIT_HOST;
    port_usb_proto_ie = 1;
    __asm__("ei");
    ncm_reset();
    dbg_printf("usb: start 4C=%02x 4D=%02x 54=%02x 8F=%02x e%u\n", port_usb_status,
               port_usb_lines, port_usb_ctrl, port_usb_devctl, usb_last_error);
}

/* print what the interrupt-driven core logged (debug builds) */
void usb_log_print(void)
{
    while (usb_log_tail != usb_log_head) {
        struct usb_log_entry e;
        __asm__("di");
        e = usb_log_ring[usb_log_tail % USB_LOG_SIZE];
        usb_log_tail++;
        __asm__("ei");
        switch (e.kind) {
        case USB_LOG_SETUP:
            dbg_printf("%02x %02x %02x%02x %02x%02x %u>%u\n", e.data[0], e.data[1], e.data[3],
                       e.data[2], e.data[5], e.data[4], e.data[6] | (e.data[7] << 8),
                       e.result);
            break;
        case USB_LOG_RESET:
            dbg_printf("usb: reset\n");
            break;
        case USB_LOG_ADDRESS:
            dbg_printf("usb: addr %u (%u)\n", e.data[0] & 0x7F, e.result);
            break;
        case USB_LOG_VBUS_LOST:
            dbg_printf("usb: VBus lost\n");
            break;
        }
    }
}

/* Main loop: if the function was configured once and the cable is still in
 * (VBus present) but the host has not configured it again for a while, the
 * two sides have lost each other (for example after a reset the host never
 * followed up).  Disconnect and connect again, as replugging would, so
 * that the host enumerates the calculator anew.  The wait doubles after
 * each try, up to 30 s. */
uint8_t usb_recoveries;

void usb_watchdog(void)
{
    static uint8_t was_configured;
    static uint32_t lost_at;
    static uint16_t wait_ms = 3000;
    uint32_t now;
    if (usb_state == USB_CONFIGURED) {
        was_configured = 1;
        lost_at = 0;
        wait_ms = 3000;
        return;
    }
    if (!was_configured || usb_state == USB_OFF || !(port_usb_lines & 0x40)) {
        lost_at = 0;
        return;
    }
    now = k_now_ms();
    if (!lost_at) {
        lost_at = now | 1;
        return;
    }
    if ((uint32_t)(now - lost_at) < wait_ms)
        return;
    dbg_printf("usb: not configured for %u ms, reconnecting\n", wait_ms);
    usb_recoveries++;
    usb_stop();
    k_delay_ms(250);                    /* the host sees the disconnect */
    usb_start();
    lost_at = 0;
    if (wait_ms < 30000)
        wait_ms *= 2;
}
