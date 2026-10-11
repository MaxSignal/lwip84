/* input.c - typing text on the calculator keypad, and a line editor.
 *
 * ALPHA cycles the mode  1 (digits) -> A (upper case) -> a (lower case).
 * 2nd gives one symbol from the table below, then falls back.
 * The letters are the green ALPHA labels of the TI-84 Plus; 0 is a space.
 */
#include <string.h>
#include "kernel.h"
#include "input.h"
#include "ui.h"

uint8_t in_mode;                /* IN_NUM, IN_UPPER, IN_LOWER */
uint8_t in_2nd;

/* indexed by key code (kernel.h), 0 = no character */
static const char num_map_bk[K_ON + 1] = {
    0, 0, 0, 0, 0, 0, '+', '-', '*',                    /* NONE DOWN LEFT RIGHT UP ENTER + - * */
    '/', '^', 0, '-', '3', '6', '9', ')', 0, 0,         /* / ^ CLEAR (-) 3 6 9 ) TAN VARS */
    '.', '2', '5', '8', '(', 0, 0, 0, '0', '1', '4',    /* . 2 5 8 ( COS PRGM STAT 0 1 4 */
    '7', ',', 0, 0, 0, '=', 0, 0, 0, 0,                 /* 7 , SIN APPS XT STO LN LOG x2 x-1 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const char alpha_map_bk[K_ON + 1] = {
    0, 0, 0, 0, 0, 0, '"', 'W', 'R',
    'M', 'H', 0, '?', '_', 'V', 'Q', 'L', 'G', 0,
    ':', 'Z', 'U', 'P', 'K', 'F', 'C', 0, ' ', 'Y', 'T',
    'O', 'J', 'E', 'B', 0, 'X', 'S', 'N', 'I', 'D',
    'A', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const char second_map_bk[K_ON + 1] = {
    0, 0, 0, 0, 0, 0, '\'', ']', '[',
    '|', '~', 0, '?', 0, 0, 0, '}', '*', 0,
    '_', 0, 0, 0, '{', '&', '#', 0, '`', 0, 0,
    0, ';', '%', '@', 0, '=', '>', '<', '\\', '$',
    '!', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

int16_t in_char(uint8_t k)
{
    char c;
    if (k == K_ALPHA) {
        in_mode = in_mode == IN_LOWER ? IN_NUM : in_mode + 1;
        in_2nd = 0;
        return IN_MODE_CHANGED;
    }
    if (k == K_2ND) {
        in_2nd ^= 1;
        return IN_MODE_CHANGED;
    }
    if (k > K_ON)
        return -1;
    if (in_2nd) {
        in_2nd = 0;
        c = second_map_bk[k];
    } else if (in_mode == IN_NUM) {
        c = num_map_bk[k];
    } else {
        c = alpha_map_bk[k];
        if (in_mode == IN_LOWER && c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    }
    return c ? c : -1;
}

char in_mode_char(void)
{
    if (in_2nd)
        return '2';
    return in_mode == IN_NUM ? '1' : in_mode == IN_UPPER ? 'A' : 'a';
}

void le_init(struct line_edit *le, char *buf, uint8_t max, uint8_t secret)
{
    le->buf = buf;
    le->max = max;
    le->len = (uint8_t)strlen(buf);
    le->cur = le->len;
    le->secret = secret;
}

int8_t le_key(struct line_edit *le, uint8_t k)
{
    int16_t c;
    switch (k) {
    case K_ENTER:
        return LE_DONE;
    case K_LEFT:
        if (le->cur)
            le->cur--;
        return 0;
    case K_RIGHT:
        if (le->cur < le->len)
            le->cur++;
        return 0;
    case K_UP:
        le->cur = 0;
        return 0;
    case K_DOWN:
        le->cur = le->len;
        return 0;
    case K_DEL:
        if (le->cur < le->len) {
            memmove(le->buf + le->cur, le->buf + le->cur + 1, le->len - le->cur);
            le->len--;
        } else if (le->cur) {
            le->cur--;
            le->len--;
            le->buf[le->len] = 0;
        }
        return 0;
    case K_CLEAR:
        if (!le->len)
            return LE_CANCEL;
        le->len = le->cur = 0;
        le->buf[0] = 0;
        return 0;
    case K_MODE:
        return LE_CANCEL;
    }
    c = in_char(k);
    if (c < 0 || le->len >= le->max)
        return 0;
    memmove(le->buf + le->cur + 1, le->buf + le->cur, le->len - le->cur + 1);
    le->buf[le->cur++] = (char)c;
    le->len++;
    return 0;
}

/* draw the line on text row `row` (0..9, 6 pixels each) with a cursor */
void le_draw(struct line_edit *le, uint8_t row, const char *prompt)
{
    uint8_t y = row * 6 + 8, pl = (uint8_t)strlen(prompt), w = CON_COLS - pl, first, i;
    first = le->cur >= w ? le->cur - w + 1 : 0;
    ui_text(row, prompt);
    for (i = 0; i < w && first + i < le->len; i++)
        lcd_draw_char((pl + i) * 4, y, le->secret ? '*' : le->buf[first + i]);
    lcd_invert_rect((pl + le->cur - first) * 4, y, 4, 6);
}
