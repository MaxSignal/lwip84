#ifndef EMU84_H
#define EMU84_H
#include <stdint.h>
#include <stddef.h>
#include "third_party/z80.h"

struct crystal {
    uint8_t ctl, loop_ctrl, counter, reload;
    int running, acked_cycle;
    uint64_t period, next;
};

struct lcd {
    uint8_t ram[64][16];
    int word8, on, mode, row, col, zshift, contrast, dummy;
    uint8_t latch;
};

struct ti84 {
    z80_t cpu;
    uint64_t pins;
    uint8_t *flash;
    int flash_pages;
    uint8_t ram[8 * 0x4000];
    uint8_t bank_a, bank_b, ram_c000;
    int mem_mode, cpu_speed, flash_unlocked;
    uint8_t int_mask, int_status, timer_speed;
    int int_line, on_down;
    uint8_t key_groups, keys[8];
    struct crystal crys[3];
    struct lcd lcd;
    uint64_t now_ps, next_hwtimer, next_event;
    uint32_t rtc_base;
    uint8_t exec_lo, exec_hi;           /* ports 22h/23h */
    int debug_out, warned_flash_write, halt_warned, fault;
    char log[65536];
    size_t log_len;
};

extern struct ti84 ti;

/* usb.c */
void usb_init(int attach_host, const char *tap);
void usb_shutdown(void);
int usb_port_in(uint8_t port, uint8_t *val);
int usb_port_out(uint8_t port, uint8_t val);
void usb_tick(uint64_t now_ps);
uint64_t usb_next_event(void);
int usb_irq_pending(void);
#endif
