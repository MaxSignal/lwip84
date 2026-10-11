/* lcd.c - T6A04-compatible LCD driver and a small text console */
#include <string.h>
#include "hw.h"
#include "kernel.h"

#include "font3x5.c"

uint8_t lcd_buf[768];          /* 64 rows x 12 bytes, MSB = leftmost pixel */
static uint8_t con_x, con_y;
static uint8_t con_dirty;
#define CON_HIST 32             /* lines kept for scrolling back */
static char con_text[CON_HIST][CON_COLS];
static uint8_t con_view;          /* lines scrolled back from the bottom */
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
    lcd_out_cmd((uint8_t)0xF0);   /* contrast */
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
    for (r = 0; r < 6; r++) {
        uint8_t bits = r < 5 ? g[r] << 1 : 0;   /* 4-pixel cell, 1 blank column */
        uint8_t *row = &lcd_buf[(uint16_t)(y + r) * 12];
        for (i = 0; i < 4; i++) {
            uint8_t px = x + i;
            uint8_t m = 0x80 >> (px & 7);
            if (bits & (8 >> i))
                row[px >> 3] |= m;
            else
                row[px >> 3] &= ~m;
        }
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
    uint8_t r, c;
    lcd_clear();
    lcd_draw_text(1, 1, con_stat);
    lcd_invert_rect(0, 0, 96, 7);
    int8_t top = (int8_t)con_y - (CON_ROWS - 1) - con_view;
    if (top < 0)
        top = 0;
    for (r = 0; r < CON_ROWS && top + r < CON_HIST; r++)
        for (c = 0; c < CON_COLS; c++)
            if (con_text[top + r][c] != ' ')
                lcd_draw_char(c * 4, r * 6 + 9, con_text[top + r][c]);
}

void con_status(const char *s)
{
    strncpy(con_stat, s, CON_COLS);
    con_stat[CON_COLS] = 0;
    con_dirty = 1;
}

void con_clear(void)
{
    memset(con_text, ' ', sizeof(con_text));
    con_x = con_y = 0;
    con_view = 0;
    con_dirty = 1;
}

void con_goto(uint8_t col, uint8_t row)
{
    con_x = col;
    con_y = row;
}

void con_putc(char c)
{
    if (c == '\n') {
        con_x = CON_COLS;
    } else if (c == '\r') {
        con_x = 0;
        return;
    } else {
        if (con_x >= CON_COLS) {
            con_x = 0;
            con_y++;
        }
        if (con_y >= CON_HIST) {
            memmove(con_text[0], con_text[1], (CON_HIST - 1) * CON_COLS);
            memset(con_text[CON_HIST - 1], ' ', CON_COLS);
            con_y = CON_HIST - 1;
        }
        con_text[con_y][con_x++] = c;
        con_dirty = 1;
        return;
    }
    if (con_x >= CON_COLS) {
        con_x = 0;
        con_y++;
        if (con_y >= CON_HIST) {
            memmove(con_text[0], con_text[1], (CON_HIST - 1) * CON_COLS);
            memset(con_text[CON_HIST - 1], ' ', CON_COLS);
            con_y = CON_HIST - 1;
            con_dirty = 1;
        }
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
    int8_t v = (int8_t)con_view + d;
    int8_t max = (int8_t)con_y - (CON_ROWS - 1);
    if (max < 0)
        max = 0;
    if (v < 0)
        v = 0;
    if (v > max)
        v = max;
    if (v != (int8_t)con_view) {
        con_view = v;
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
