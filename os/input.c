/* input.c - typing text on the calculator keypad, and a line editor.
 *
 * ALPHA cycles the mode  1 (digits) -> A (upper case) -> a (lower case),
 * then the language chosen with 2nd+ALPHA (lib/ime/ime.h; Cyrillic and
 * Greek in capitals, then small letters).  2nd gives one symbol from the
 * table below, then falls back.  The letters are the green ALPHA labels of
 * the TI-84 Plus; 0 is a space.  In the language mode . (-) + give . - ,
 * for the input method.
 *
 * Text is UTF-8; the editor shows it with the external font.
 */
#include <string.h>
#include "kernel.h"
#include "input.h"
#include "ui.h"
#include "font.h"

uint8_t in_mode;                /* IN_NUM, IN_UPPER, IN_LOWER, IN_LANG */
uint8_t in_2nd;
uint8_t in_picking;

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
        if (in_2nd) {
            in_2nd = 0;
            return IN_PICK;
        }
        if (in_mode == IN_LOWER) {
            in_mode = ime_lang ? IN_LANG : IN_NUM;
            ime_upper = ime_has_case(ime_lang);
        } else if (in_mode == IN_LANG) {
            if (ime_upper)
                ime_upper = 0;
            else
                in_mode = IN_NUM;
        } else {
            in_mode++;
        }
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
    } else if (in_mode == IN_LANG && (k == K_DOT || k == K_NEG || k == K_ADD)) {
        c = k == K_DOT ? '.' : k == K_NEG ? '-' : ',';
    } else {
        c = alpha_map_bk[k];
        if (in_mode != IN_UPPER && c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    }
    return c ? c : -1;
}

const char *in_mode_str(void)
{
    static char s[4];
    if (in_2nd || in_mode != IN_LANG) {
        s[0] = in_2nd ? '2' : in_mode == IN_NUM ? '1' : in_mode == IN_UPPER ? 'A' : 'a';
        s[1] = 0;
    } else {
        ui_from_utf8(ime_label(ime_lang), s, sizeof(s));
    }
    return s;
}

int16_t in_text(uint8_t k, char *out)
{
    int16_t c;
    out[0] = 0;
    c = in_char(k);
    if (c == IN_MODE_CHANGED || c == IN_PICK) {
        if (k == K_ALPHA)
            ime_flush(out);
        return c;
    }
    if (c < 0)
        return c;
    if (in_mode == IN_LANG && ime_lang) {
        ime_feed((char)c, out);
        return (int16_t)strlen(out);
    }
    out[0] = (char)c;
    out[1] = 0;
    return 1;
}

/* ---- the language list ---- */

static uint8_t pick_items[IME_N], pick_n, pick_sel, pick_top;
#define PICK_ROWS 7             /* lines on the screen */

void in_pick_open(void)
{
    uint8_t l;
    pick_n = 0;
    pick_sel = 0;
    for (l = 0; l < IME_N; l++) {
        if (l == IME_OFF || (ime_enabled >> l) & 1) {
            if (l == ime_lang && in_mode == IN_LANG)
                pick_sel = pick_n;
            pick_items[pick_n++] = l;
        }
    }
    pick_top = 0;
    in_picking = 1;
}

static void choose(uint8_t l)
{
    ime_reset();
    ime_lang = l;
    ime_upper = 0;
    if (l == IME_OFF) {
        if (in_mode == IN_LANG)
            in_mode = IN_LOWER;
    } else {
        in_mode = IN_LANG;
    }
}

uint8_t in_pick_key(uint8_t k)
{
    if (k == K_UP && pick_sel) {
        pick_sel--;
    } else if (k == K_DOWN && pick_sel + 1 < pick_n) {
        pick_sel++;
    } else if (k == K_ENTER) {
        choose(pick_items[pick_sel]);
        in_picking = 0;
    } else if (k == K_CLEAR || k == K_MODE || k == K_ALPHA) {
        in_picking = 0;
    } else {
        /* 0-9: that line */
        static const uint8_t digit_bk[] = { K_0, K_1, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9 };
        uint8_t i;
        for (i = 0; i < pick_n; i++) {
            if (pick_items[i] < sizeof(digit_bk) && k == digit_bk[pick_items[i]]) {
                choose(pick_items[i]);
                in_picking = 0;
            }
        }
    }
    in_2nd = 0;
    return !in_picking;
}

/* a line "n title" at pixel row y, inverted when selected */
static void list_line(uint8_t y, uint8_t n, const char *mark, const char *title, uint8_t sel)
{
    char b[48];
    uint8_t i = 0;
    b[i++] = ' ';
    b[i++] = (char)('0' + n);
    b[i++] = ' ';
    while (*mark)
        b[i++] = *mark++;
    ui_from_utf8(title, b + i, (uint8_t)(sizeof(b) - i));
    ui_text_at(y, b, 8);
    if (sel)
        lcd_invert_rect(0, y, 96, 8);
}

static const char *lang_title(uint8_t l)
{
    return l == IME_OFF ? "abc (no language)" : ime_title(l);
}

void in_pick_draw(void)
{
    uint8_t r;
    if (pick_sel < pick_top)
        pick_top = pick_sel;
    else if (pick_sel >= pick_top + PICK_ROWS)
        pick_top = pick_sel - PICK_ROWS + 1;
    ui_clear();
    ui_status("input language");
    for (r = 0; r < PICK_ROWS && pick_top + r < pick_n; r++)
        list_line(8 + r * 8, pick_items[pick_top + r], "", lang_title(pick_items[pick_top + r]),
                  pick_top + r == pick_sel);
}

/* ---- the settings screen ---- */

static uint8_t setup_sel;

void in_setup_draw(void)
{
    uint8_t l;
    ui_clear();
    ui_status("input languages 2nd+ALPHA");
    for (l = 1; l < IME_N; l++)
        list_line(8 + (l - 1) * 8, l, (ime_enabled >> l) & 1 ? "[x] " : "[ ] ", ime_title(l),
                  l - 1 == setup_sel);
}

uint8_t in_setup_key(uint8_t k)
{
    static const uint8_t digit_bk[] = { K_1, K_2, K_3, K_4, K_5, K_6, K_7 };
    uint8_t i;
    if (k == K_CLEAR || k == K_MODE)
        return 0;
    if (k == K_UP && setup_sel)
        setup_sel--;
    else if (k == K_DOWN && setup_sel + 2 < IME_N)
        setup_sel++;
    else if (k == K_ENTER)
        ime_enabled ^= 1u << (setup_sel + 1);
    for (i = 0; i < sizeof(digit_bk) && i + 1 < IME_N; i++) {
        if (k == digit_bk[i]) {
            setup_sel = i;
            ime_enabled ^= 1u << (i + 1);
        }
    }
    return 1;
}

/* ---- the line editor ---- */

void le_init(struct line_edit *le, char *buf, uint8_t max, uint8_t secret)
{
    le->buf = buf;
    le->max = max;
    le->len = (uint8_t)strlen(buf);
    le->cur = le->len;
    le->secret = secret;
    ime_reset();
}

static uint8_t is_cont(char c)
{
    return ((uint8_t)c & 0xC0) == 0x80;
}

static void insert(struct line_edit *le, const char *s)
{
    uint8_t n = (uint8_t)strlen(s);
    if (!n || le->len + n > le->max)
        return;
    memmove(le->buf + le->cur + n, le->buf + le->cur, le->len - le->cur + 1);
    memcpy(le->buf + le->cur, s, n);
    le->cur += n;
    le->len += n;
}

static void commit(struct line_edit *le)
{
    char out[IME_OUT_MAX];
    ime_flush(out);
    insert(le, out);
}

int8_t le_key(struct line_edit *le, uint8_t k)
{
    char out[IME_OUT_MAX];
    int16_t c;
    uint8_t n;
    if (in_picking) {
        in_pick_key(k);
        return 0;
    }
    if (ime_pre[0]) {
        /* keys that act on the composition */
        if (k == K_DEL) {
            ime_back();
            return 0;
        }
        if (k == K_CLEAR) {
            ime_reset();
            return 0;
        }
        if (k == K_ENTER) {
            commit(le);
            return 0;
        }
    }
    switch (k) {
    case K_ENTER:
        return LE_DONE;
    case K_LEFT:
        commit(le);
        while (le->cur && is_cont(le->buf[--le->cur]))
            ;
        return 0;
    case K_RIGHT:
        commit(le);
        if (le->cur < le->len)
            while (++le->cur < le->len && is_cont(le->buf[le->cur]))
                ;
        return 0;
    case K_UP:
        commit(le);
        le->cur = 0;
        return 0;
    case K_DOWN:
        commit(le);
        le->cur = le->len;
        return 0;
    case K_DEL:
        ime_reset();
        if (le->cur == le->len) {       /* at the end: the character before */
            if (!le->cur)
                return 0;
            while (--le->cur && is_cont(le->buf[le->cur]))
                ;
        }
        n = 1;
        while (le->cur + n < le->len && is_cont(le->buf[le->cur + n]))
            n++;
        memmove(le->buf + le->cur, le->buf + le->cur + n, le->len - le->cur - n + 1);
        le->len -= n;
        return 0;
    case K_CLEAR:
        ime_reset();
        if (!le->len)
            return LE_CANCEL;
        le->len = le->cur = 0;
        le->buf[0] = 0;
        return 0;
    case K_MODE:
        ime_reset();
        return LE_CANCEL;
    }
    c = in_text(k, out);
    insert(le, out);
    if (c == IN_PICK)
        in_pick_open();
    return 0;
}

/* width in pixels of the character at s (length *n) */
static uint8_t char_px(const char *s, uint8_t *n)
{
    uint16_t cp, g;
    *n = ui_utf8_char(s, &cp);
    if (cp < 0x80)
        return 4;
    g = font_find(cp);
    if (g == FONT_NONE)
        return 4;
    return font_kind(g) == FONT_MARK ? 0 : 8;
}

/* width of the editor's character at i; a password shows stars */
static uint8_t px_of(struct line_edit *le, uint8_t i, uint8_t *n)
{
    if (le->secret) {
        *n = 1;
        return 4;
    }
    return char_px(le->buf + i, n);
}

/* cells of one character to b (2 bytes at most) */
static uint8_t char_cells(const char *s, char *b, uint8_t secret)
{
    uint16_t cp, g;
    ui_utf8_char(s, &cp);
    if (secret) {
        b[0] = '*';
        return 1;
    }
    if (cp < 0x80) {
        b[0] = (char)cp;
        return 1;
    }
    g = font_find(cp);
    if (g == FONT_NONE) {
        b[0] = '?';
        return 1;
    }
    b[0] = FONT_LEAD(g);
    b[1] = FONT_TRAIL(g);
    return 2;
}

/* draw the line on text row `row` (0..9, 6 pixels each; 8 pixels high when
 * it shows font glyphs) with a cursor and the composition underlined */
void le_draw(struct line_edit *le, uint8_t row, const char *prompt)
{
    char b[64];
    uint8_t y = row * 6 + 8, h = 6, i, n, w, bn, x, cur_x = 0, pre_x = 0, pre_w = 0;
    uint8_t avail, start, px_cur = 0, px_start = 0, pre_px = 0;
    const char *p;
    if (in_picking) {
        in_pick_draw();
        return;
    }
    /* glyphs need 8-pixel rows */
    for (p = le->buf; *p; p++)
        if ((uint8_t)*p >= 0x80)
            h = 8;
    if (ime_pre[0])
        h = 8;
    if (le->secret || y + h > 64)
        h = 6;
    bn = (uint8_t)strlen(prompt);
    memcpy(b, prompt, bn);
    x = bn * 4;
    avail = 96 - x - 4;
    /* where the cursor is, then the first character to show */
    for (i = 0; i < le->cur; i += n)
        px_cur += px_of(le, i, &n);
    for (p = ime_pre; *p; p += n)
        pre_px += char_px(p, &n);
    start = 0;
    while (start < le->cur && px_cur + pre_px - px_start > avail) {
        px_start += px_of(le, start, &n);
        start += n;
    }
    /* the cells */
    for (i = start; ; i += n) {
        if (i == le->cur) {
            pre_x = x;
            for (p = ime_pre; *p && bn + 2 < sizeof(b); p += n) {
                w = char_px(p, &n);
                if (x + w > 96)
                    break;
                bn += char_cells(p, b + bn, 0);
                x += w;
            }
            pre_w = x - pre_x;
            cur_x = x;
        }
        if (i >= le->len || bn + 2 >= sizeof(b))
            break;
        w = px_of(le, i, &n);
        if (x + w > 96)
            break;
        bn += char_cells(le->buf + i, b + bn, le->secret);
        x += w;
    }
    b[bn] = 0;
    ui_text_at(y, b, h);
    if (pre_w)
        lcd_invert_rect(pre_x, y + h - 1, pre_w, 1);
    if (cur_x < 96)
        lcd_invert_rect(cur_x, y, le->cur < le->len && !le->secret &&
                        char_px(le->buf + le->cur, &n) == 8 && cur_x <= 88 ? 8 : 4, h);
}
