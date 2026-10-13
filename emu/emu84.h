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
    int ram48;                  /* 48 KiB of RAM: pages 2-7 are page 2 */
    int app;                    /* -app: a TI-OS application on a fake TI-OS */
    uint16_t sp_min;
    int app_vars;                 /* variables made by the fake TI-OS */
    uint16_t app_var_at;            /* lowest stack pointer in C000-FFFF ("stack") */
    uint8_t app_ram[2 * 0x4000]; /* TI-OS's RAM pages 0 and 1 before the app */
    uint8_t ram[8 * 0x4000];
    uint8_t bank_a, bank_b, ram_c000;
    int mem_mode, cpu_speed, flash_unlocked;
    uint8_t int_mask, int_status, timer_speed;
    int int_line, on_down, low_power;
    uint8_t key_groups, keys[8];
    struct crystal crys[3];
    struct lcd lcd;
    uint64_t now_ps, next_hwtimer, next_event;
    uint32_t rtc_base;
    uint8_t exec_lo, exec_hi;           /* ports 22h/23h */
    int debug_out, warned_flash_write, halt_warned, fault;
    int profiling;
    uint32_t *prof;                     /* samples per (bank << 16 | pc) */
    uint64_t prof_cycles, halt_cycles;
    uint32_t fc_addr;                   /* address of "jp (iy)" in farcall.s, 0 = off */
    uint32_t *fc_hist;                  /* far calls per (bank << 16 | target) */
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
void usb_vbus_glitch(double ms);
#endif
