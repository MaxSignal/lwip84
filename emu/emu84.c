/*
 * emu84 - small headless TI-84 Plus / Plus SE emulator for testing the
 * lwIP-84+ operating system.  Not a general purpose TI emulator: it models
 * what the OS uses (memory mapping, interrupts, timers, LCD, keypad) plus the
 * USB controller in peripheral mode (usb.c).
 *
 * Usage: emu84 [options] rom.bin
 *   -s FILE     read commands from FILE instead of stdin
 *   -se         TI-84 Plus SE (2 MiB flash) instead of TI-84 Plus (1 MiB)
 *   -r48        48 KiB of RAM (later revisions): pages 2-7 are one page
 *   -d          print OS debug output (port FEh, DEBUG builds only)
 *   -u          attach the virtual USB host (see usb.c)
 *   -n SOCKET   with -u: bridge NCM frames to passt's UNIX socket SOCKET
 *
 * Commands (one per line):
 *   run MS                 run for MS milliseconds of emulated time
 *   waitlog TEXT MS        run until debug output contains TEXT (fail after MS)
 *   key NAME [MS]          press NAME for MS (default 100) then release, run 100 ms
 *   down NAME / up NAME    hold / release a key
 *   on                     press the ON key for 100 ms
 *   screen [FILE]          dump LCD (ASCII to stdout, or PBM to FILE)
 *   regs                   print CPU registers
 *   quit
 */
#define CHIPS_IMPL
#include "third_party/z80.h"
#include "emu84.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ti84 ti;

/* ------------------------------------------------------------------ */
/* time                                                                */

static uint64_t ps_per_tick(void)
{
    return ti.cpu_speed ? 66667 : 166667; /* 15 MHz / 6 MHz */
}

/* ------------------------------------------------------------------ */
/* memory                                                              */

static uint8_t *page_ptr(uint8_t page, int *is_flash)
{
    if (page & 0x80) {
        *is_flash = 0;
        page &= 7;
        if (ti.ram48 && page > 2)
            page = 2;
        return ti.ram + page * 0x4000;
    }
    *is_flash = 1;
    return ti.flash + (page & (ti.flash_pages - 1)) * 0x4000;
}

static uint8_t *map_addr(uint16_t addr, int *is_flash)
{
    uint8_t page;
    switch (addr >> 14) {
    case 0:
        *is_flash = 1;
        return ti.flash + addr;
    case 1:
        page = ti.mem_mode ? (ti.bank_a & ~1) : ti.bank_a;
        break;
    case 2:
        page = ti.mem_mode ? (ti.bank_a | 1) : ti.bank_b;
        break;
    default:
        page = ti.mem_mode ? ti.bank_b : (0x80 | ti.ram_c000);
        break;
    }
    return page_ptr(page, is_flash) + (addr & 0x3FFF);
}

/* Opcode fetch: model the boot code's execution protection (flash pages
 * exec_lo..exec_hi, inclusive as in TilEm) and flag any fetch from RAM,
 * which this OS never does. */
static void check_fetch(uint16_t addr)
{
    int fl;
    uint8_t page;
    if (ti.fault || addr < 0x4000)
        return;
    switch (addr >> 14) {
    case 1: page = ti.mem_mode ? (ti.bank_a & ~1) : ti.bank_a; break;
    case 2: page = ti.mem_mode ? (ti.bank_a | 1) : ti.bank_b; break;
    default: page = ti.mem_mode ? ti.bank_b : (0x80 | ti.ram_c000); break;
    }
    (void)map_addr(addr, &fl);
    if (!fl) {
        fprintf(stderr, "emu84: FAULT: opcode fetch from RAM page %02X at %04X\n", page, addr);
        ti.fault = 1;
    } else if (page >= ti.exec_lo && page <= ti.exec_hi) {
        fprintf(stderr, "emu84: FAULT: execution protection, flash page %02X at %04X "
                "(the calculator would reset)\n", page, addr);
        ti.fault = 1;
    }
}

static uint8_t mem_read(uint16_t addr)
{
    int fl;
    return *map_addr(addr, &fl);
}

static void mem_write(uint16_t addr, uint8_t v)
{
    int fl;
    uint8_t *p = map_addr(addr, &fl);
    if (fl) {
        if (!ti.warned_flash_write) {
            fprintf(stderr, "emu84: write to flash at %04X (pc %04X) ignored\n",
                    addr, ti.cpu.pc);
            ti.warned_flash_write = 1;
        }
        return;
    }
    *p = v;
}

/* ------------------------------------------------------------------ */
/* interrupts and timers                                               */

static void update_int(void)
{
    /* the ON interrupt is level-triggered: it is raised again as long as the
     * key is held and the interrupt is enabled */
    if (ti.on_down && (ti.int_mask & 1))
        ti.int_status |= 1;
    int pending = (ti.int_status & 0x07) || (ti.int_status & 0xE0);
    if (usb_irq_pending())
        pending = 1;
    ti.int_line = pending;
}

static uint64_t hwtimer1_period_ps(void)
{
    /* timer 1 fires at 32768 / (64 + 80*i) Hz on the 83+SE/84+ */
    int i = (ti.timer_speed >> 1) & 3;
    return (uint64_t)(64 + 80 * i) * 1000000000000ULL / 32768;
}

static void crystal_tick_one(int n)
{
    struct crystal *t = &ti.crys[n];
    if (!t->running)
        return;
    if (t->counter == 1) {
        if (!t->acked_cycle)
            t->loop_ctrl |= 4; /* missed acknowledge */
        ti.int_status |= 0x20 << n;
        if ((t->loop_ctrl & 1) && t->acked_cycle) {
            t->counter = t->reload;
            t->acked_cycle = 0;
        } else if (!(t->loop_ctrl & 1)) {
            t->running = 0;
            t->counter = t->reload;
        } else {
            t->counter = 0; /* overflows to FF next */
        }
    } else {
        t->counter--;
    }
}

static uint64_t crystal_period_ps(uint8_t ctl)
{
    static const uint32_t div[8] = {3, 33, 328, 3277, 1, 16, 256, 4096};
    if ((ctl >> 6) == 1)
        return (uint64_t)div[ctl & 7] * 1000000000000ULL / 32768;
    if ((ctl >> 6) >= 2) /* CPU clock divided */
        return ps_per_tick() * (uint64_t)(1 << ((ctl & 0x3F) > 30 ? 30 : (ctl & 0x3F)));
    return 0;
}

static void run_events(void)
{
    if (ti.now_ps >= ti.next_hwtimer) {
        ti.next_hwtimer += hwtimer1_period_ps();
        if (ti.int_mask & 0x02)
            ti.int_status |= 0x02;
    }
    for (int n = 0; n < 3; n++) {
        struct crystal *t = &ti.crys[n];
        if (t->running && ti.now_ps >= t->next) {
            t->next += t->period;
            crystal_tick_one(n);
        }
    }
    usb_tick(ti.now_ps);
    update_int();
    uint64_t nx = ti.next_hwtimer;
    for (int n = 0; n < 3; n++)
        if (ti.crys[n].running && ti.crys[n].next < nx)
            nx = ti.crys[n].next;
    uint64_t u = usb_next_event();
    if (u < nx)
        nx = u;
    ti.next_event = nx;
}

/* ------------------------------------------------------------------ */
/* LCD (T6A04)                                                         */

static void lcd_cmd(uint8_t v)
{
    struct lcd *l = &ti.lcd;
    if (v <= 1)
        l->word8 = v;
    else if (v == 2 || v == 3)
        l->on = v & 1;
    else if (v >= 4 && v <= 7)
        l->mode = v - 4;
    else if (v >= 0x20 && v <= 0x3F)
        l->col = v - 0x20;
    else if (v >= 0x40 && v <= 0x7F)
        l->zshift = v - 0x40;
    else if (v >= 0x80 && v <= 0xBF)
        l->row = v - 0x80;
    else if (v >= 0xC0)
        l->contrast = v - 0xC0;
    l->dummy = 1;
}

static void lcd_advance(void)
{
    struct lcd *l = &ti.lcd;
    int maxcol = l->word8 ? 15 : 20;
    switch (l->mode) {
    case 0: l->row = (l->row + 63) & 63; break;
    case 1: l->row = (l->row + 1) & 63; break;
    case 2: l->col = (l->col + maxcol - 1) % maxcol; break;
    case 3: l->col = (l->col + 1) % maxcol; break;
    }
}

static uint8_t lcd_get(void)
{
    struct lcd *l = &ti.lcd;
    uint8_t v = 0;
    if (l->word8) {
        v = l->ram[l->row][l->col % 15];
    } else {
        int bit = l->col * 6;
        for (int i = 0; i < 6; i++) {
            int x = bit + i;
            int b = x < 120 ? (l->ram[l->row][x >> 3] >> (7 - (x & 7))) & 1 : 0;
            v = (v << 1) | b;
        }
    }
    return v;
}

static void lcd_data_write(uint8_t v)
{
    struct lcd *l = &ti.lcd;
    if (l->word8) {
        l->ram[l->row][l->col % 15] = v;
    } else {
        int bit = l->col * 6;
        for (int i = 0; i < 6; i++) {
            int x = bit + i;
            if (x >= 120)
                break;
            uint8_t m = 0x80 >> (x & 7);
            if (v & (0x20 >> i))
                l->ram[l->row][x >> 3] |= m;
            else
                l->ram[l->row][x >> 3] &= ~m;
        }
    }
    lcd_advance();
}

static uint8_t lcd_data_read(void)
{
    struct lcd *l = &ti.lcd;
    uint8_t v = l->latch;
    l->latch = lcd_get();
    if (l->dummy)
        l->dummy = 0;
    lcd_advance();
    return v;
}

static int lcd_pixel(int x, int y)
{
    struct lcd *l = &ti.lcd;
    int r = (y + l->zshift) & 63;
    return (l->ram[r][x >> 3] >> (7 - (x & 7))) & 1;
}

static void screen_ascii(FILE *f)
{
    fprintf(f, "+------------------------------------------------+\n");
    for (int y = 0; y < 64; y += 2) {
        fputc('|', f);
        for (int x = 0; x < 96; x += 2) {
            int n = lcd_pixel(x, y) + lcd_pixel(x + 1, y) + lcd_pixel(x, y + 1) +
                    lcd_pixel(x + 1, y + 1);
            fputc(" .:#"[n > 2 ? 3 : n], f);
        }
        fputs("|\n", f);
    }
    fprintf(f, "+------------------------------------------------+\n");
}

static void screen_pbm(const char *fn)
{
    FILE *f = fopen(fn, "w");
    if (!f) {
        perror(fn);
        return;
    }
    fprintf(f, "P1\n96 64\n");
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 96; x++)
            fputc(ti.lcd.on && lcd_pixel(x, y) ? '1' : '0', f);
        fputc('\n', f);
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* keypad                                                              */

static const struct { const char *name; int group, bit; } keymap[] = {
    {"down", 0, 0}, {"left", 0, 1}, {"right", 0, 2}, {"up", 0, 3},
    {"enter", 1, 0}, {"+", 1, 1}, {"-", 1, 2}, {"*", 1, 3}, {"/", 1, 4},
    {"^", 1, 5}, {"clear", 1, 6},
    {"neg", 2, 0}, {"3", 2, 1}, {"6", 2, 2}, {"9", 2, 3}, {")", 2, 4},
    {"tan", 2, 5}, {"vars", 2, 6},
    {".", 3, 0}, {"2", 3, 1}, {"5", 3, 2}, {"8", 3, 3}, {"(", 3, 4},
    {"cos", 3, 5}, {"prgm", 3, 6}, {"stat", 3, 7},
    {"0", 4, 0}, {"1", 4, 1}, {"4", 4, 2}, {"7", 4, 3}, {",", 4, 4},
    {"sin", 4, 5}, {"apps", 4, 6}, {"xt", 4, 7},
    {"sto", 5, 1}, {"ln", 5, 2}, {"log", 5, 3}, {"x2", 5, 4}, {"x-1", 5, 5},
    {"math", 5, 6}, {"alpha", 5, 7},
    {"graph", 6, 0}, {"trace", 6, 1}, {"zoom", 6, 2}, {"window", 6, 3},
    {"y=", 6, 4}, {"2nd", 6, 5}, {"mode", 6, 6}, {"del", 6, 7},
    {NULL, 0, 0}};

static int key_set(const char *name, int down)
{
    if (!strcmp(name, "on")) {
        ti.on_down = down;
        if (down && (ti.int_mask & 1))
            ti.int_status |= 1;
        return 0;
    }
    for (int i = 0; keymap[i].name; i++) {
        if (!strcasecmp(keymap[i].name, name)) {
            if (down)
                ti.keys[keymap[i].group] |= 1 << keymap[i].bit;
            else
                ti.keys[keymap[i].group] &= ~(1 << keymap[i].bit);
            return 0;
        }
    }
    fprintf(stderr, "emu84: unknown key '%s'\n", name);
    return -1;
}

static uint8_t key_read(void)
{
    uint8_t v = 0xFF;
    for (int g = 0; g < 7; g++)
        if (!(ti.key_groups & (1 << g)))
            v &= ~ti.keys[g];
    return v;
}

/* ------------------------------------------------------------------ */
/* I/O ports                                                           */

static uint8_t port_in(uint8_t p)
{
    if ((p >= 0x4A && p <= 0x5B) || (p >= 0x80 && p <= 0xAF)) {
        uint8_t v;
        if (usb_port_in(p, &v))
            return v;
    }
    switch (p) {
    case 0x00: return 0x03;
    case 0x01: return key_read();
    case 0x02: return 0xE3 | (ti.flash_unlocked ? 4 : 0);
    case 0x03: return ti.int_mask;
    case 0x04: {
        uint8_t v = ti.int_status & 0xF7;
        if (!ti.on_down)
            v |= 0x08;
        return v;
    }
    case 0x05: return ti.ram_c000;
    case 0x06: return ti.bank_a;
    case 0x07: return ti.bank_b;
    case 0x10: case 0x12:
        return (ti.lcd.word8 ? 0x40 : 0) | (ti.lcd.on ? 0x20 : 0) |
               ((ti.lcd.mode & 2) ? 2 : 0) | (ti.lcd.mode & 1);
    case 0x11: case 0x13: return lcd_data_read();
    case 0x20: return ti.cpu_speed;
    case 0x22: return ti.exec_lo;
    case 0x23: return ti.exec_hi;
    case 0x21: return ti.flash_pages == 128 ? 0x21 : 0x20;
    case 0x30: case 0x33: case 0x36: return ti.crys[(p - 0x30) / 3].ctl;
    case 0x31: case 0x34: case 0x37: return ti.crys[(p - 0x31) / 3].loop_ctrl;
    case 0x32: case 0x35: case 0x38: return ti.crys[(p - 0x32) / 3].counter;
    case 0x45: case 0x46: case 0x47: case 0x48: {
        uint32_t s = ti.rtc_base + (uint32_t)(ti.now_ps / 1000000000000ULL);
        return s >> ((p - 0x45) * 8);
    }
    default:
        return 0xFF;
    }
}

static void port_out(uint8_t p, uint8_t v)
{
    if ((p >= 0x4A && p <= 0x5B) || (p >= 0x80 && p <= 0xAF)) {
        if (usb_port_out(p, v))
            return;
    }
    switch (p) {
    case 0x01:
        ti.key_groups = v;
        break;
    case 0x02:
        /* writing 0 bits acknowledges ON/timers/link on 83+SE/84+ */
        ti.int_status &= (v & 0x07) | 0xF8;
        break;
    case 0x03:
        ti.int_mask = v;
        ti.int_status &= (v & 0x17) | 0xE0;
        update_int();
        break;
    case 0x04:
        ti.mem_mode = v & 1;
        if (((v >> 1) & 3) != ti.timer_speed >> 1) {
            ti.timer_speed = v & 6;
            ti.next_hwtimer = ti.now_ps + hwtimer1_period_ps();
        }
        break;
    case 0x05: ti.ram_c000 = v & 7; break;
    case 0x06: ti.bank_a = v & 0x80 ? (0x80 | (v & 7)) : (v & (ti.flash_pages - 1)); break;
    case 0x07: ti.bank_b = v & 0x80 ? (0x80 | (v & 7)) : (v & (ti.flash_pages - 1)); break;
    case 0x10: case 0x12: lcd_cmd(v); break;
    case 0x11: case 0x13: lcd_data_write(v); break;
    case 0x14: ti.flash_unlocked = v & 1; break;
    case 0x22: if (ti.flash_unlocked) ti.exec_lo = v; break;
    case 0x23: if (ti.flash_unlocked) ti.exec_hi = v; break;
    case 0x20: ti.cpu_speed = v & 3 ? 1 : 0; break;
    case 0x30: case 0x33: case 0x36: {
        struct crystal *t = &ti.crys[(p - 0x30) / 3];
        t->ctl = v;
        t->period = crystal_period_ps(v);
        if (!t->period)
            t->running = 0;
        break;
    }
    case 0x31: case 0x34: case 0x37: {
        int n = (p - 0x31) / 3;
        struct crystal *t = &ti.crys[n];
        t->loop_ctrl = v & 3;
        t->acked_cycle = 1;
        ti.int_status &= ~(0x20 << n);
        break;
    }
    case 0x32: case 0x35: case 0x38: {
        struct crystal *t = &ti.crys[(p - 0x32) / 3];
        t->reload = t->counter = v;
        if (t->period) {
            t->running = 1;
            t->next = ti.now_ps + t->period;
        }
        break;
    }
    case 0xFE:
        if (ti.debug_out) {
            fputc(v, stdout);
            fflush(stdout);
        }
        if (ti.log_len < sizeof(ti.log) - 1) {
            ti.log[ti.log_len++] = v;
            ti.log[ti.log_len] = 0;
        } else {
            memmove(ti.log, ti.log + sizeof(ti.log) / 2, sizeof(ti.log) / 2);
            ti.log_len = sizeof(ti.log) / 2 - 1;
            ti.log[ti.log_len++] = v;
            ti.log[ti.log_len] = 0;
        }
        break;
    default:
        break;
    }
    update_int();
}

/* ------------------------------------------------------------------ */
/* main loop                                                           */

static void run_ps(uint64_t dur)
{
    uint64_t end = ti.now_ps + dur;
    uint64_t pins = ti.pins;
    while (ti.now_ps < end && !ti.fault) {
        if (ti.int_line && ti.cpu.iff1)
            pins |= Z80_INT;
        else
            pins &= ~Z80_INT;
        pins = z80_tick(&ti.cpu, pins);
        if (pins & Z80_MREQ) {
            uint16_t a = Z80_GET_ADDR(pins);
            if (pins & Z80_RD) {
                if (pins & Z80_M1) {
                    check_fetch(a);
                    if (ti.fc_addr && a == ti.fc_addr && ti.fc_hist)
                        ti.fc_hist[((uint32_t)ti.bank_a << 16) | ti.cpu.iy]++;
                }
                Z80_SET_DATA(pins, mem_read(a));
            } else if (pins & Z80_WR) {
                mem_write(a, Z80_GET_DATA(pins));
            }
        } else if (pins & Z80_IORQ) {
            if (pins & Z80_M1) {
                Z80_SET_DATA(pins, 0xFF); /* IM 1/IM 2 data bus */
            } else if (pins & Z80_RD) {
                Z80_SET_DATA(pins, port_in(Z80_GET_ADDR(pins) & 0xFF));
            } else if (pins & Z80_WR) {
                port_out(Z80_GET_ADDR(pins) & 0xFF, Z80_GET_DATA(pins));
            }
        }
        if (ti.profiling) {
            if (pins & Z80_HALT)
                ti.halt_cycles++;
            if (++ti.prof_cycles % 97 == 0) {
                uint16_t pc = ti.cpu.pc;
                uint32_t bank = pc >= 0x4000 && pc < 0x8000 ? ti.bank_a : 0;
                ti.prof[(bank << 16) | pc]++;
            }
        }
        ti.now_ps += ps_per_tick();
        if (ti.now_ps >= ti.next_event)
            run_events();
        /* halt with port 03h bit 3 clear: low-power mode (calculator off) */
        if ((pins & Z80_HALT) && !(ti.int_mask & 0x08)) {
            if (!ti.low_power) {
                ti.low_power = 1;
                fprintf(stderr, "emu84: low-power halt (lcd %s)\n", ti.lcd.on ? "on" : "off");
            }
        } else if (ti.low_power && !(pins & Z80_HALT)) {
            ti.low_power = 0;
            fprintf(stderr, "emu84: woke from low power\n");
        }
        if ((pins & Z80_HALT) && !ti.cpu.iff1 && !ti.int_line && !ti.halt_warned) {
            /* di; halt: nothing will ever wake the CPU (except ON) */
            ti.halt_warned = 1;
            fprintf(stderr, "emu84: CPU halted with interrupts disabled (pc %04X)\n",
                    ti.cpu.pc);
        }
    }
    ti.pins = pins;
}

static void run_ms(double ms)
{
    run_ps((uint64_t)(ms * 1e9));
}

static void print_regs(void)
{
    z80_t *c = &ti.cpu;
    printf("PC=%04X SP=%04X AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X IY=%04X "
           "IFF1=%d IM=%d bankA=%02X bankB=%02X C000=%02X\n",
           c->pc, c->sp, c->af, c->bc, c->de, c->hl, c->ix, c->iy, c->iff1, c->im,
           ti.bank_a, ti.bank_b, ti.ram_c000);
}

static int do_command(char *line)
{
    char *argv[8];
    int argc = 0;
    char *s = line;
    while (*s && argc < 8) {
        while (isspace((unsigned char)*s))
            s++;
        if (!*s || *s == '#')
            break;
        if (*s == '"') {
            argv[argc++] = ++s;
            while (*s && *s != '"')
                s++;
        } else {
            argv[argc++] = s;
            while (*s && !isspace((unsigned char)*s))
                s++;
        }
        if (*s)
            *s++ = 0;
    }
    if (!argc)
        return 0;
    if (!strcmp(argv[0], "run") && argc > 1) {
        run_ms(atof(argv[1]));
    } else if (!strcmp(argv[0], "waitlog") && argc > 2) {
        double lim = atof(argv[2]), t = 0;
        while (!strstr(ti.log, argv[1])) {
            if (t >= lim) {
                fprintf(stderr, "emu84: waitlog '%s' timed out\n", argv[1]);
                return 1;
            }
            run_ms(5);
            t += 5;
        }
        /* consume the log up to the match so the next waitlog needs new output */
        char *m = strstr(ti.log, argv[1]) + strlen(argv[1]);
        ti.log_len = strlen(m);
        memmove(ti.log, m, ti.log_len + 1);
    } else if (!strcmp(argv[0], "key") && argc > 1) {
        if (key_set(argv[1], 1))
            return 1;
        run_ms(argc > 2 ? atof(argv[2]) : 100);
        key_set(argv[1], 0);
        run_ms(100);
    } else if (!strcmp(argv[0], "down") && argc > 1) {
        return key_set(argv[1], 1) ? 1 : 0;
    } else if (!strcmp(argv[0], "up") && argc > 1) {
        return key_set(argv[1], 0) ? 1 : 0;
    } else if (!strcmp(argv[0], "vbusglitch") && argc > 1) {
        usb_vbus_glitch(atof(argv[1]));
    } else if (!strcmp(argv[0], "on")) {
        key_set("on", 1);
        run_ms(100);
        key_set("on", 0);
    } else if (!strcmp(argv[0], "screen")) {
        if (argc > 1)
            screen_pbm(argv[1]);
        else
            screen_ascii(stdout);
    } else if (!strcmp(argv[0], "dumpram") && argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        if (f) {
            fwrite(ti.ram, 1, sizeof(ti.ram), f);
            fclose(f);
        }
    } else if (!strcmp(argv[0], "profile") && argc > 1) {
        /* profile on | profile FILE: write "bank:pc count" lines */
        if (!strcmp(argv[1], "farcall") && argc > 2) {
            /* count far calls: ARG is the address of "jp (iy)" in farcall.s */
            ti.fc_addr = strtoul(argv[2], NULL, 0);
            if (!ti.fc_hist)
                ti.fc_hist = calloc(256u << 16, sizeof(uint32_t));
        } else if (!strcmp(argv[1], "on")) {
            if (!ti.prof)
                ti.prof = calloc(256u << 16, sizeof(uint32_t));
            ti.profiling = 1;
        } else if (ti.prof) {
            FILE *f = fopen(argv[1], "w");
            ti.profiling = 0;
            if (f) {
                fprintf(f, "# cycles %llu halted %llu\n", (unsigned long long)ti.prof_cycles,
                        (unsigned long long)ti.halt_cycles);
                for (uint32_t i = 0; i < (256u << 16); i++)
                    if (ti.prof[i])
                        fprintf(f, "%02X:%04X %u\n", i >> 16, i & 0xFFFF, ti.prof[i]);
                if (ti.fc_hist) {
                    for (uint32_t i = 0; i < (256u << 16); i++)
                        if (ti.fc_hist[i])
                            fprintf(f, "F %02X:%04X %u\n", i >> 16, i & 0xFFFF, ti.fc_hist[i]);
                    memset(ti.fc_hist, 0, (256u << 16) * sizeof(uint32_t));
                }
                fclose(f);
            }
        }
    } else if (!strcmp(argv[0], "regs")) {
        print_regs();
    } else if (!strcmp(argv[0], "quit")) {
        return 2;
    } else {
        fprintf(stderr, "emu84: bad command '%s'\n", argv[0]);
        return 1;
    }
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    const char *script = NULL, *rom = NULL, *tap = NULL;
    int se = 0, use_usb = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc)
            script = argv[++i];
        else if (!strcmp(argv[i], "-se"))
            se = 1;
        else if (!strcmp(argv[i], "-r48"))
            ti.ram48 = 1;
        else if (!strcmp(argv[i], "-d"))
            ti.debug_out = 1;
        else if (!strcmp(argv[i], "-u"))
            use_usb = 1;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc)
            tap = argv[++i];
        else
            rom = argv[i];
    }
    if (!rom) {
        fprintf(stderr, "usage: emu84 [-s script] [-se] [-r48] [-d] [-u [-n tap]] rom.bin\n");
        return 2;
    }
    ti.flash_pages = se ? 128 : 64;
    ti.flash = malloc(ti.flash_pages * 0x4000);
    memset(ti.flash, 0xFF, ti.flash_pages * 0x4000);
    FILE *f = fopen(rom, "rb");
    if (!f) {
        perror(rom);
        return 2;
    }
    size_t n = fread(ti.flash, 1, ti.flash_pages * 0x4000, f);
    fclose(f);
    if (n < 0x4000) {
        fprintf(stderr, "emu84: %s too small\n", rom);
        return 2;
    }
    /* emulator signature on the boot page (real calculators carry TI's boot
     * code there); the OS uses it to send debug output to port FEh */
    {
        uint8_t *boot = ti.flash + (ti.flash_pages - 1) * 0x4000;
        if (boot[0x10] == 0xFF)
            memcpy(boot + 0x10, "emu84", 6);
    }
    ti.bank_b = 0x81;
    ti.exec_lo = 0x08;                  /* values left by the retail boot code */
    ti.exec_hi = se ? 0x69 : 0x29;
    ti.mem_mode = 0;
    ti.timer_speed = 6;
    ti.key_groups = 0xFF;
    ti.lcd.word8 = 1;
    ti.lcd.mode = 1;
    ti.rtc_base = 0;
    ti.pins = z80_init(&ti.cpu);
    ti.next_hwtimer = hwtimer1_period_ps();
    ti.next_event = ti.next_hwtimer;
    usb_init(use_usb, tap);

    FILE *in = script ? fopen(script, "r") : stdin;
    if (!in) {
        perror(script);
        return 2;
    }
    char line[512];
    int rc = 0;
    while (fgets(line, sizeof(line), in)) {
        line[strcspn(line, "\r\n")] = 0;
        int r = do_command(line);
        if (r == 2)
            break;
        if (r)
            rc = 1;
    }
    usb_shutdown();
    if (ti.fault) {
        print_regs();
        return 3;
    }
    return rc;
}
