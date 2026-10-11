/* term.c - a small VT100/xterm subset on the 24x9 text area.
 *
 * Handles printable ASCII, CR, LF, BS, TAB, cursor movement, erase and
 * scrolling regions are ignored; colours and modes are swallowed.
 * Lines scrolled off the top go to a scrollback ring in extended RAM. */
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "ui.h"
#include "term.h"

#define W TERM_COLS
#define H TERM_ROWS
#define SB_LINES 1024u                  /* 24 KiB scrollback */

static char (*scr)[W];                  /* H rows, allocated by term_open */
static uint8_t cx, cy, esc, nparam, utf8_skip, wrap_pending;
static uint8_t param[4];
static uint16_t sb_head, sb_count;      /* scrollback ring */
uint8_t term_dirty;

uint8_t term_open(void)
{
    if (!scr)
        scr = malloc(H * W);
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

static void put(char c)
{
    if (wrap_pending) {
        wrap_pending = 0;
        cx = 0;
        newline();
    }
    scr[cy][cx] = c;
    if (cx == W - 1)
        wrap_pending = 1;
    else
        cx++;
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
        if (utf8_skip) {
            utf8_skip--;
            continue;
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
            if (c >= 0xC0) {
                utf8_skip = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
                put('?');
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
        ui_text(r, row);
    }
    if (!back)
        lcd_invert_rect(cx * 4, 8 + cy * 6, 4, 6);
    term_dirty = 0;
}

uint16_t term_scrollback(void)
{
    return sb_count;
}
