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
