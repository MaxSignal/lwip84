/* ui.c - full-screen layout for applications.
 *
 * Text is a string of cells: ASCII in the 3x5 font (4 pixels wide), bit 7
 * set for inverted ASCII, and glyphs of the external font in two cells
 * (lib/font/font.h), 8 pixels wide and 7 high.  A row with font glyphs is 8
 * pixels high, other rows 6. */
#include <string.h>
#include "kernel.h"
#include "font.h"
#include "ui.h"

void ui_clear(void)
{
    lcd_clear();
}

/* OR rows of a glyph (bit 7 = leftmost pixel) into the LCD buffer */
static void draw_glyph(uint8_t x, uint8_t y, const uint8_t *rows, uint8_t n)
{
    uint8_t r, sh = x & 7;
    uint8_t *p = lcd_buf + (uint16_t)y * 12 + (x >> 3);
    for (r = 0; r < n && y + r < 64; r++, p += 12) {
        p[0] |= rows[r] >> sh;
        if (sh && (x >> 3) < 11)
            p[1] |= rows[r] << (8 - sh);
    }
}

/* cells of s from x on; ASCII at y_ascii, glyphs at y_glyph with n rows */
static void draw_cells(const char *s, uint8_t x, uint8_t y_ascii, uint8_t y_glyph,
                       uint8_t n, uint8_t inv_y, uint8_t inv_h)
{
    uint8_t rows[8], last = 0xFF;
    while (*s && x < 96) {
        if (FONT_IS_LEAD(*s) && s[1]) {
            uint8_t w = font_glyph(FONT_GLYPH(s[0], s[1]), rows);
            s += 2;
            if (!w) {
                /* a mark: over the glyph before it */
                if (last != 0xFF)
                    draw_glyph(last, y_glyph, rows, n);
                continue;
            }
            if (x + 8 > 96)
                break;
            draw_glyph(x, y_glyph, rows, n);
            last = x;
            x += 8;
            continue;
        }
        last = 0xFF;
        lcd_draw_char(x, y_ascii, *s & 0x7F);
        if ((*s & 0x80) && inv_h)
            lcd_invert_rect(x, inv_y, 4, inv_h);
        s++;
        x += 4;
    }
}

uint8_t ui_row_height(const char *s)
{
    for (; *s; s++) {
        if (FONT_IS_LEAD(*s) && s[1])
            return 8;
    }
    return 6;
}

void ui_status(const char *s)
{
    memset(lcd_buf, 0, 7 * 12);
    draw_cells(s, 1, 1, 0, 7, 0, 0);
    lcd_invert_rect(0, 0, 96, 7);
}

/* a text row at pixel row y, h = 6 or 8 (ui_row_height) */
void ui_text_at(uint8_t y, const char *s, uint8_t h)
{
    uint8_t n = 64 - y < h ? 64 - y : h;
    memset(lcd_buf + (uint16_t)y * 12, 0, n * 12);
    if (h >= 8)
        draw_cells(s, 0, y + 2, y, 7, y, h);
    else
        draw_cells(s, 0, y, y, 6, y, h);
}

void ui_text(uint8_t row, const char *s)
{
    ui_text_at(8 + row * 6, s, 6);
}

uint8_t ui_utf8_char(const char *s, uint16_t *cp)
{
    uint8_t c = (uint8_t)s[0];
    if (c < 0xC0) {
        *cp = c < 0x80 ? c : '?';
        return 1;
    }
    if (c < 0xE0) {
        if (((uint8_t)s[1] & 0xC0) != 0x80) {
            *cp = '?';
            return 1;
        }
        *cp = (uint16_t)(c & 0x1F) << 6 | (s[1] & 0x3F);
        return 2;
    }
    if (((uint8_t)s[1] & 0xC0) != 0x80 || ((uint8_t)s[2] & 0xC0) != 0x80) {
        *cp = '?';
        return 1;
    }
    if (c >= 0xF0) {                    /* outside the BMP */
        *cp = '?';
        return ((uint8_t)s[3] & 0xC0) == 0x80 ? 4 : 3;
    }
    *cp = (uint16_t)(c & 0x0F) << 12 | (uint16_t)(s[1] & 0x3F) << 6 | (s[2] & 0x3F);
    return 3;
}

void ui_from_utf8(const char *s, char *out, uint8_t max)
{
    uint16_t cp, g;
    uint8_t n = 0;
    while (*s && n + 3 <= max) {
        s += ui_utf8_char(s, &cp);
        if (cp < 0x80) {
            out[n++] = (char)cp;
            continue;
        }
        g = font_find(cp);
        if (g == FONT_NONE) {
            out[n++] = '?';
        } else {
            out[n++] = FONT_LEAD(g);
            out[n++] = FONT_TRAIL(g);
        }
    }
    out[n] = 0;
}
