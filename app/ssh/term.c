/* term.c - a small VT100/xterm subset on the 24-column text area.
 *
 * Handles printable ASCII, UTF-8, CR, LF, BS, TAB, cursor movement, erase;
 * scrolling regions are ignored; colours and modes are swallowed.  Wide
 * characters (CJK, kana, Hangul, full-width forms: two columns, as the
 * server's wcwidth counts them) are drawn with the external font in two
 * cells (font.h); with a font the rows are 8 pixels high and the terminal
 * has 7 of them, else 9 rows of 6 pixels.  Accented Latin letters lose
 * their accents, other characters become '?'.
 * Lines scrolled off the top go to a scrollback ring in extended RAM. */
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "ui.h"
#include "font.h"
#include "term.h"

#define W TERM_COLS
#define H term_rows
#define RH (font_ok ? 8 : 6)            /* pixel rows per text row */
uint8_t term_rows = 9;
#define SB_LINES 1024u                  /* 24 KiB scrollback */

static char (*scr)[W];                  /* H rows, allocated by term_open */
static uint8_t cx, cy, esc, nparam, utf8_need, utf8_big, wrap_pending;
static uint16_t utf8_cp;
static uint8_t param[4];
static uint16_t sb_head, sb_count;      /* scrollback ring */
uint8_t term_dirty;

uint8_t term_open(void)
{
    term_rows = font_ok ? 7 : 9;
    if (!scr)
        scr = malloc(TERM_ROWS_MAX * W);
    if (scr)
        term_reset();
    return scr != NULL;
}

void term_close(void)
{
    free(scr);
    scr = NULL;
}

void term_reset(void)
{
    memset(scr, ' ', H * W);
    cx = cy = esc = 0;
    wrap_pending = 0;
    sb_head = sb_count = 0;
    term_dirty = 1;
}

static void scroll_up(void)
{
    xram_write(XRAM_SSH + (uint32_t)sb_head * W, scr[0], W);
    sb_head = (sb_head + 1) % SB_LINES;
    if (sb_count < SB_LINES)
        sb_count++;
    memmove(scr[0], scr[1], (H - 1) * W);
    memset(scr[H - 1], ' ', W);
}

static void newline(void)
{
    if (cy == H - 1)
        scroll_up();
    else
        cy++;
}

/* cell x of the cursor row is about to be written: if it is half of a
 * font glyph, blank the other half (the row is read from its start, as a
 * trail byte may look like a lead byte) */
static void unpair(uint8_t x)
{
    char *r = scr[cy];
    uint8_t i = 0;
    while (i < W) {
        if (FONT_IS_LEAD(r[i]) && i + 1 < W) {
            if (i == x || i + 1 == x) {
                r[i] = ' ';
                r[i + 1] = ' ';
                return;
            }
            i += 2;
        } else {
            if (i >= x)
                return;
            i++;
        }
    }
}

static void put(char c)
{
    if (wrap_pending) {
        wrap_pending = 0;
        cx = 0;
        newline();
    }
    unpair(cx);
    scr[cy][cx] = c;
    if (cx == W - 1)
        wrap_pending = 1;
    else
        cx++;
}

/* a font glyph in two cells; at the last column it goes to the next line */
static void put_wide(uint16_t g)
{
    if (wrap_pending || cx == W - 1) {
        if (!wrap_pending)
            scr[cy][cx] = ' ';
        wrap_pending = 0;
        cx = 0;
        newline();
    }
    unpair(cx);
    unpair(cx + 1);
    scr[cy][cx] = FONT_LEAD(g);
    scr[cy][cx + 1] = FONT_TRAIL(g);
    if (cx + 2 >= W) {
        cx = W - 1;
        wrap_pending = 1;
    } else {
        cx += 2;
    }
}

/* East Asian Wide and Fullwidth (what wcwidth() counts as two columns) */
static uint8_t is_wide(uint16_t cp)
{
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) ||
           (cp >= 0x3041 && cp <= 0x33FF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
           (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6);
}

/* U+00A0..U+017F without accents (as in html.c) */
static const char latin_bk[] =
    " !cL$Y|S\"ca<-\0R-o+23'uP.,1o>///?AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy"
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

static void put_cp(uint16_t cp)
{
    if (cp >= 0xA0 && cp < 0x180) {
        char c = latin_bk[cp - 0xA0];
        if (c)
            put(c);
        return;
    }
    if (cp >= 0x0300 && cp <= 0x036F)   /* combining marks: nothing */
        return;
    if (is_wide(cp)) {
        uint16_t g = font_find(cp);
        if (g != FONT_NONE) {
            put_wide(g);
            return;
        }
        put('?');                       /* still two columns for the server */
        put('?');
        return;
    }
    put('?');
}

static uint8_t p(uint8_t i, uint8_t def)
{
    return i < nparam && param[i] ? param[i] : def;
}

static void csi(char f)
{
    uint8_t i;
    switch (f) {
    case 'A': cy = cy > p(0, 1) ? cy - p(0, 1) : 0; break;
    case 'B': cy = cy + p(0, 1) < H ? cy + p(0, 1) : H - 1; break;
    case 'C': cx = cx + p(0, 1) < W ? cx + p(0, 1) : W - 1; break;
    case 'D': cx = cx > p(0, 1) ? cx - p(0, 1) : 0; break;
    case 'G': cx = p(0, 1) <= W ? p(0, 1) - 1 : W - 1; break;
    case 'd': cy = p(0, 1) <= H ? p(0, 1) - 1 : H - 1; break;
    case 'H':
    case 'f':
        cy = p(0, 1) <= H ? p(0, 1) - 1 : H - 1;
        cx = p(1, 1) <= W ? p(1, 1) - 1 : W - 1;
        break;
    case 'J':
        i = nparam ? param[0] : 0;
        if (i == 0) {
            memset(&scr[cy][cx], ' ', W - cx);
            if (cy < H - 1)
                memset(scr[cy + 1], ' ', (H - 1 - cy) * W);
        } else if (i == 1) {
            memset(scr[0], ' ', cy * W + cx + 1);
        } else {
            memset(scr, ' ', H * W);
        }
        break;
    case 'K':
        i = nparam ? param[0] : 0;
        if (i == 0)
            memset(&scr[cy][cx], ' ', W - cx);
        else if (i == 1)
            memset(scr[cy], ' ', cx + 1);
        else
            memset(scr[cy], ' ', W);
        break;
    case 'P':                           /* delete characters */
        i = p(0, 1);
        if (i > W - cx)
            i = W - cx;
        memmove(&scr[cy][cx], &scr[cy][cx + i], W - cx - i);
        memset(&scr[cy][W - i], ' ', i);
        break;
    case '@':                           /* insert blanks */
        i = p(0, 1);
        if (i > W - cx)
            i = W - cx;
        memmove(&scr[cy][cx + i], &scr[cy][cx], W - cx - i);
        memset(&scr[cy][cx], ' ', i);
        break;
    case 'X':
        i = p(0, 1);
        if (i > W - cx)
            i = W - cx;
        memset(&scr[cy][cx], ' ', i);
        break;
    }
    wrap_pending = 0;
}

void term_write(const uint8_t *s, uint16_t n)
{
    if (!scr)
        return;
    term_dirty = 1;
    while (n--) {
        uint8_t c = *s++;
        if (esc == 1) {                 /* after ESC */
            if (c == '[') {
                esc = 2;
                nparam = 0;
                param[0] = 0;
            } else if (c == ']') {
                esc = 3;                /* OSC: until BEL or ESC \ */
            } else if (c == '(' || c == ')') {
                esc = 4;                /* charset: one more byte */
            } else {
                if (c == 'M' && cy)     /* reverse index */
                    cy--;
                esc = 0;
            }
            continue;
        }
        if (esc == 2) {                 /* CSI */
            if (c >= '0' && c <= '9') {
                if (!nparam)
                    nparam = 1;
                if (nparam <= 4)
                    param[nparam - 1] = param[nparam - 1] * 10 + (c - '0');
            } else if (c == ';') {
                if (!nparam)
                    nparam = 1;
                if (nparam < 4)
                    param[nparam] = 0;
                nparam++;
            } else if (c >= 0x40 && c <= 0x7E) {
                csi((char)c);
                esc = 0;
            }
            continue;
        }
        if (esc == 3) {
            if (c == 7 || c == '\\')
                esc = 0;
            continue;
        }
        if (esc == 4) {
            esc = 0;
            continue;
        }
        if (utf8_need) {
            if ((c & 0xC0) != 0x80) {
                utf8_need = 0;          /* broken sequence: fall through */
            } else {
                utf8_cp = (uint16_t)(utf8_cp << 6 | (c & 0x3F));
                if (!--utf8_need) {
                    if (utf8_big) {     /* emoji and the like: two columns */
                        put('?');
                        put('?');
                    } else {
                        put_cp(utf8_cp);
                    }
                }
                continue;
            }
        }
        switch (c) {
        case 27:
            esc = 1;
            break;
        case '\r':
            cx = 0;
            wrap_pending = 0;
            break;
        case '\n':
            wrap_pending = 0;
            newline();
            break;
        case 8:
            if (wrap_pending)
                wrap_pending = 0;
            else if (cx)
                cx--;
            break;
        case '\t':
            do
                put(' ');
            while (cx & 7 && !wrap_pending);
            break;
        default:
            if (c >= 0xF0) {
                utf8_need = 3;          /* outside the BMP */
                utf8_big = 1;
            } else if (c >= 0xE0) {
                utf8_need = 2;
                utf8_big = 0;
                utf8_cp = c & 0x0F;
            } else if (c >= 0xC0) {
                utf8_need = 1;
                utf8_big = 0;
                utf8_cp = c & 0x1F;
            } else if (c >= 32 && c < 127) {
                put((char)c);
            }
        }
    }
}

/* back = 0: live screen; otherwise show `back` lines of scrollback */
void term_draw(uint16_t back)
{
    char row[W + 1];
    uint8_t r;
    row[W] = 0;
    if (back > sb_count)
        back = sb_count;
    for (r = 0; r < H; r++) {
        int16_t line = (int16_t)r - (int16_t)back;   /* < 0: scrollback */
        if (line < 0) {
            uint16_t idx = (sb_head + SB_LINES + line) % SB_LINES;
            xram_read(XRAM_SSH + (uint32_t)idx * W, row, W);
        } else {
            memcpy(row, scr[line], W);
        }
        ui_text_at(8 + r * RH, row, RH);
    }
    if (!back) {
        /* the cursor covers a whole glyph when it sits on one */
        uint8_t w = FONT_IS_LEAD(scr[cy][cx]) && cx + 1 < W ? 8 : 4;
        lcd_invert_rect(cx * 4, 8 + cy * RH, w, RH);
    }
    term_dirty = 0;
}

uint16_t term_scrollback(void)
{
    return sb_count;
}
