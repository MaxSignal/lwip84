/* lcd.c - T6A04-compatible LCD driver and a small text console */
#include <string.h>
#include "hw.h"
#include "kernel.h"

#include "font3x5.c"

uint8_t lcd_buf[768];          /* 64 rows x 12 bytes, MSB = leftmost pixel */
/* console: the line being written is in RAM, older lines in a ring in
 * extended RAM (XRAM_CON) */
#define CON_HIST XRAM_CON_LINES /* lines kept for scrolling back */
static char con_cur[CON_COLS];
static uint8_t con_x;           /* CON_COLS: a newline is pending */
static uint16_t con_lines;      /* lines moved to the history */
static uint8_t con_dirty;
static uint8_t con_view;        /* lines scrolled back from the bottom */
static char con_stat[CON_COLS + 1];

/* The LCD driver needs about 10 us between accesses.  A fixed delay works
 * with both the old Toshiba drivers and the newer ones that must not be
 * polled through the status port. */
static void lcd_wait(void) __naked
{
    __asm
        push    bc
        ld      b, #10
    00001$:
        djnz    00001$
        pop     bc
        ret
    __endasm;
}

static void lcd_out_cmd(uint8_t c)
{
    lcd_wait();
    port_lcd_cmd = c;
}

void lcd_init(void)
{
    lcd_out_cmd(0x01);        /* 8-bit transfers */
    lcd_out_cmd(0x05);        /* auto-increment rows (X) */
    lcd_out_cmd(0x40);        /* z-address 0 */
#ifndef TI84P_APP
    lcd_out_cmd((uint8_t)0xF0);   /* contrast (in TI-OS: its own) */
#endif
    lcd_out_cmd(0x03);        /* display on */
    lcd_clear();
    lcd_update();
}

void lcd_update(void)
{
    const uint8_t *p = lcd_buf;
    uint8_t col, row;
    for (col = 0; col < 12; col++) {
        lcd_out_cmd(0x20 + col);
        lcd_out_cmd(0x80);
        for (row = 0; row < 64; row++) {
            lcd_wait();
            port_lcd_data = p[(uint16_t)row * 12];
        }
        p++;
    }
}

void lcd_clear(void)
{
    memset(lcd_buf, 0, sizeof(lcd_buf));
}

void lcd_invert_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    uint8_t x2, b0, b1, m0, m1, i;
    uint8_t *row;
    if (x >= 96 || y >= 64 || !w || !h)
        return;
    if (x + w > 96)
        w = 96 - x;
    if (y + h > 64)
        h = 64 - y;
    x2 = x + w - 1;
    b0 = x >> 3;
    b1 = x2 >> 3;
    m0 = 0xFF >> (x & 7);
    m1 = 0xFF << (7 - (x2 & 7));
    if (b0 == b1)
        m0 &= m1;
    row = &lcd_buf[(uint16_t)y * 12];
    while (h--) {
        row[b0] ^= m0;
        if (b1 != b0) {
            for (i = b0 + 1; i < b1; i++)
                row[i] ^= 0xFF;
            row[b1] ^= m1;
        }
        row += 12;
    }
}

void lcd_draw_char(uint8_t x, uint8_t y, char c)
{
    const uint8_t *g;
    uint8_t r, i;
    if (c < 32 || c > 126)
        c = '?';
    g = font3x5_bk[c - 32];
    if (!(x & 3) && x < 96 && y <= 58) {
        /* text grid: the 4-pixel cell is one nibble */
        uint8_t *p = &lcd_buf[(uint16_t)y * 12 + (x >> 3)];
        if (x & 4) {
            for (r = 0; r < 5; r++, p += 12)
                *p = (*p & 0xF0) | (g[r] << 1);
            *p &= 0xF0;
        } else {
            for (r = 0; r < 5; r++, p += 12)
                *p = (*p & 0x0F) | (g[r] << 5);
            *p &= 0x0F;
        }
        return;
    }
    /* anywhere else: the cell spans two bytes, one shift per row */
    if (x >= 96)
        return;
    i = x & 7;
    for (r = 0; r < 6 && y + r < 64; r++) {
        uint8_t bits = r < 5 ? g[r] << 1 : 0;   /* 4-pixel cell, 1 blank column */
        uint8_t *row = &lcd_buf[(uint16_t)(y + r) * 12 + (x >> 3)];
        uint16_t v = (uint16_t)bits << 12, m = 0xF000;
        uint8_t k;
        for (k = i; k; k--) {
            v >>= 1;
            m >>= 1;
        }
        row[0] = (row[0] & ~(uint8_t)(m >> 8)) | (uint8_t)(v >> 8);
        if ((x >> 3) < 11)
            row[1] = (row[1] & ~(uint8_t)m) | (uint8_t)v;
    }
}

void lcd_draw_text(uint8_t x, uint8_t y, const char *s)
{
    while (*s && x <= 92) {
        lcd_draw_char(x, y, *s++);
        x += 4;
    }
}

static void con_redraw(void)
{
    char line[CON_COLS + 1];
    uint8_t r, c;
    int16_t first;                      /* history line shown on row 0; con_lines = con_cur */
    lcd_clear();
    lcd_draw_text(1, 1, con_stat);
    lcd_invert_rect(0, 0, 96, 7);
    first = (int16_t)con_lines - (CON_ROWS - 1) - con_view;
    if (first < 0)
        first = 0;
    for (r = 0; r < CON_ROWS; r++) {
        uint16_t n = (uint16_t)first + r;
        if (n > con_lines)
            break;
        if (n == con_lines)
            memcpy(line, con_cur, CON_COLS);
        else
            xram_read(XRAM_CON + (uint32_t)(n % CON_HIST) * CON_COLS, line, CON_COLS);
        for (c = 0; c < CON_COLS; c++)
            if (line[c] != ' ')
                lcd_draw_char(c * 4, r * 6 + 9, line[c]);
    }
}

static void con_newline(void)
{
    xram_write(XRAM_CON + (uint32_t)(con_lines % CON_HIST) * CON_COLS, con_cur, CON_COLS);
    con_lines++;
    memset(con_cur, ' ', CON_COLS);
    con_x = 0;
    con_dirty = 1;
}

void con_status(const char *s)
{
    strncpy(con_stat, s, CON_COLS);
    con_stat[CON_COLS] = 0;
    con_dirty = 1;
}

void con_clear(void)
{
    memset(con_cur, ' ', CON_COLS);
    con_x = 0;
    con_lines = 0;
    con_view = 0;
    con_dirty = 1;
}

void con_putc(char c)
{
    if (c == '\n') {
        if (con_x == CON_COLS)
            con_newline();
        con_x = CON_COLS;
    } else if (c == '\r') {
        con_x = 0;
    } else {
        if (con_x >= CON_COLS)
            con_newline();
        con_cur[con_x++] = c;
        con_dirty = 1;
    }
}

void con_puts(const char *s)
{
    while (*s)
        con_putc(*s++);
}

void con_printf(const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    con_puts(buf);
}

/* UP/DOWN: look at older lines */
void con_scroll(int8_t d)
{
    int16_t v = (int16_t)con_view + d;
    int16_t max = (int16_t)(con_lines < CON_HIST ? con_lines : CON_HIST) - (CON_ROWS - 1);
    if (max < 0)
        max = 0;
    if (v < 0)
        v = 0;
    if (v > max)
        v = max;
    if (v != (int16_t)con_view) {
        con_view = (uint8_t)v;
        con_dirty = 1;
    }
}

void con_refresh(void)
{
    con_dirty = 1;
}

void con_flush(void)
{
    if (!con_dirty)
        return;
    con_dirty = 0;
    con_redraw();
    lcd_update();
}
