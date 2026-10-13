/* hw.h - TI-84 Plus I/O ports used by the OS */
#ifndef TI84P_HW_H
#define TI84P_HW_H
#include <stdint.h>

__sfr __at(0x00) port_link;
__sfr __at(0x01) port_keypad;
__sfr __at(0x02) port_status;
__sfr __at(0x03) port_int_mask;
__sfr __at(0x04) port_int_id;     /* read: interrupt source, write: map mode/timer speed */
__sfr __at(0x05) port_ram_c000;
__sfr __at(0x06) port_bank_a;
__sfr __at(0x07) port_bank_b;
__sfr __at(0x10) port_lcd_cmd;
__sfr __at(0x11) port_lcd_data;
__sfr __at(0x20) port_cpu_speed;
__sfr __at(0x21) port_hw_type;
#ifdef TI84P_DEBUG
__sfr __at(0xFE) port_emu_debug;  /* emu84 debug console (DEBUG builds only) */
#endif

/* port 03h / 04h interrupt bits */
#define INT_ON      0x01
#define INT_TIMER1  0x02
#define INT_TIMER2  0x04
#define INT_POWER   0x08            /* port 03h: keep calculator powered */
#define INT_LINK    0x10

#endif
