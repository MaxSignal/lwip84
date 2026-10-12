/* html.c - streaming HTML renderer of the web browser.
 *
 * The tokenizer is a byte-at-a-time state machine, so the page can be fed
 * in pieces of any size as it arrives.  Tag and attribute names are looked
 * up by hash; only the attributes that matter (href, form attributes,
 * class/style/hidden for invisible elements) are kept.  A small stack of
 * open elements closes elements the way browsers do (p, li, td, option...)
 * and hides whole subtrees (script, style, svg, display:none, hidden, a few
 * classes such as noprint).  There is no CSS and no JavaScript.
 *
 * Output: lines of HTML_COLS characters in extended RAM.  Links and form
 * fields are items (struct html_item) numbered in page order; the number
 * is drawn inverted (bit 7) in front of the link text or the field.
 * Text is reduced to ASCII where it can be: entities and UTF-8 are decoded,
 * accented letters lose their accents; other characters are shown with the
 * external font (os/font.c, two cells each) or become '?'.
 */
#include <string.h>
#include <stdlib.h>
#include "html.h"
#include "font.h"

#define COLS HTML_COLS
#define ABUF 640
#define STK 40
#define CAP_MAX 200

/* ------------------------------------------------------------------ */
/* tags                                                                */

enum {
    T_A = 1, T_ADDRESS, T_AREA, T_ARTICLE, T_ASIDE, T_BASE, T_BLOCKQUOTE, T_BODY, T_BR, T_BUTTON,
    T_CAPTION, T_CENTER, T_COL, T_DD, T_DETAILS, T_DIALOG, T_DIV, T_DL, T_DT, T_EMBED,
    T_FIELDSET, T_FIGCAPTION, T_FIGURE, T_FOOTER, T_FORM, T_H1, T_H2, T_H3, T_H4, T_H5,
    T_H6, T_HEAD, T_HEADER, T_HR, T_HTML, T_IFRAME, T_IMG, T_INPUT, T_LEGEND, T_LI,
    T_LINK, T_MAIN, T_MATH, T_MENU, T_META, T_NAV, T_NOFRAMES, T_OL, T_OPTGROUP, T_OPTION,
    T_P, T_PARAM, T_PRE, T_SCRIPT, T_SECTION, T_SELECT, T_SOURCE, T_STYLE, T_SUMMARY, T_SVG,
    T_TABLE, T_TBODY, T_TD, T_TEMPLATE, T_TEXTAREA, T_TFOOT, T_TH, T_THEAD, T_TITLE, T_TR,
    T_TRACK, T_UL, T_WBR, T_XMP, T_COUNT
};

#define TF_VOID 0x01
#define TF_BLOCK 0x02
#define TF_PARA 0x04            /* blank line before and after */
#define TF_RAW 0x08             /* raw text, skipped */
#define TF_HIDE 0x10            /* subtree is not shown */
#define TF_HEAD 0x20
#define TF_RC 0x40              /* raw text that is kept (title, textarea) */
#define TF_ATTR 0x80            /* keep all known attributes */

static const char tag_names_bk[T_COUNT - 1][11] = {
    "a", "address", "area", "article", "aside", "base", "blockquote", "body", "br", "button",
    "caption", "center", "col", "dd", "details", "dialog", "div", "dl", "dt", "embed",
    "fieldset", "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5",
    "h6", "head", "header", "hr", "html", "iframe", "img", "input", "legend", "li",
    "link", "main", "math", "menu", "meta", "nav", "noframes", "ol", "optgroup", "option",
    "p", "param", "pre", "script", "section", "select", "source", "style", "summary", "svg",
    "table", "tbody", "td", "template", "textarea", "tfoot", "th", "thead", "title", "tr",
    "track", "ul", "wbr", "xmp"};

#define B TF_BLOCK
#define BP (TF_BLOCK | TF_PARA)
#define V TF_VOID
static const uint8_t tag_flags_bk[T_COUNT] = {
    0,
    TF_ATTR, B, V, B, B, V, BP, 0, V, TF_ATTR,
    B, B, V, B, B, TF_HIDE, B, BP, B, V,
    B, B, BP, B, B | TF_ATTR, BP | TF_HEAD, BP | TF_HEAD, BP | TF_HEAD, BP | TF_HEAD, BP | TF_HEAD,
    BP | TF_HEAD, 0, B, V | B, 0, TF_HIDE, V | TF_ATTR, V | TF_ATTR, B, B,
    V, B, TF_HIDE, B, V, B, TF_RAW, B, TF_ATTR, TF_ATTR,
    BP, V, BP, TF_RAW, B, TF_ATTR, V, TF_RAW, B, TF_HIDE,
    BP, 0, 0, TF_HIDE, TF_RC | TF_ATTR, 0, 0, 0, TF_RC, B,
    V, B, V, TF_RAW};
#undef B
#undef BP
#undef V

/* attributes */
enum {
    A_HREF = 1, A_NAME, A_VALUE, A_TYPE, A_ACTION, A_METHOD, A_ALT, A_CHECKED, A_SELECTED,
    A_HIDDEN, A_STYLE, A_CLASS, A_ROLE, A_ARIAH, A_DISABLED, A_SIZE, A_N
};

/* classes whose elements are not shown */
static const char hide_classes_bk[] =
    "noprint\0mw-editsection\0navbox\0reference\0mw-jump-link\0sr-only\0"
    "visually-hidden\0screen-reader-text\0vector-dropdown\0vector-pinned-container\0"
    "vector-sticky-header\0";
#define N_HIDE_CLASSES 11

/* named entities: name, code point */
struct ent_def { char name[7]; uint16_t cp; };
static const struct ent_def ents_bk[] = {
    {"amp", 38}, {"lt", 60}, {"gt", 62}, {"quot", 34}, {"apos", 39}, {"nbsp", 160},
    {"copy", 169}, {"reg", 174}, {"trade", 8482}, {"mdash", 8212}, {"ndash", 8211},
    {"hellip", 8230}, {"laquo", 171}, {"raquo", 187}, {"lsaquo", 8249}, {"rsaquo", 8250},
    {"middot", 183}, {"bull", 8226}, {"rsquo", 8217}, {"lsquo", 8216}, {"ldquo", 8220},
    {"rdquo", 8221}, {"sbquo", 8218}, {"bdquo", 8222}, {"times", 215}, {"divide", 247},
    {"deg", 176}, {"plusmn", 177}, {"micro", 181}, {"para", 182}, {"sect", 167},
    {"cent", 162}, {"pound", 163}, {"yen", 165}, {"euro", 8364}, {"frac12", 189},
    {"frac14", 188}, {"frac34", 190}, {"sup1", 185}, {"sup2", 178}, {"sup3", 179},
    {"larr", 8592}, {"rarr", 8594}, {"uarr", 8593}, {"darr", 8595}, {"harr", 8596},
    {"rArr", 8658}, {"minus", 8722}, {"ensp", 8194}, {"emsp", 8195}, {"thinsp", 8201},
    {"zwnj", 8204}, {"zwj", 8205}, {"lrm", 8206}, {"rlm", 8207}, {"shy", 173},
    {"iexcl", 161}, {"iquest", 191}, {"szlig", 223}, {"prime", 8242}, {"Prime", 8243}, {"le", 8804}, {"ge", 8805}, {"ne", 8800},
    {"infin", 8734}, {"asymp", 8776}, {"dagger", 8224}, {"permil", 8240}, {"", 0}};

/* endings of the named entities of accented letters */
static const char accents_bk[] = "acute\0grave\0circ\0uml\0tilde\0ring\0cedil\0slash\0";

/* U+00A0..U+017F without accents (0 = drop) */
static const char latin_bk[] =
    " !cL$Y|S\"ca<-\0R-o+23'uP.,1o>///?AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy"
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

/* other code points: ranges and their ASCII replacement */
struct cp_map { uint16_t lo, hi; char s[4]; };
static const struct cp_map cpmap_bk[] = {
    {0x02C6, 0x02C6, "^"}, {0x02DC, 0x02DC, "~"}, {0x0300, 0x036F, ""},
    {0x2002, 0x200A, " "}, {0x200B, 0x200F, ""}, {0x2010, 0x2015, "-"},
    {0x2018, 0x201B, "'"}, {0x201C, 0x201F, "\""}, {0x2020, 0x2021, "+"},
    {0x2022, 0x2022, "*"}, {0x2026, 0x2026, "..."}, {0x2028, 0x202F, " "},
    {0x2030, 0x2030, "%"}, {0x2032, 0x2032, "'"}, {0x2033, 0x2033, "\""},
    {0x2039, 0x2039, "<"}, {0x203A, 0x203A, ">"}, {0x2044, 0x2044, "/"},
    {0x2060, 0x2064, ""}, {0x20AC, 0x20AC, "EUR"}, {0x2122, 0x2122, "TM"},
    {0x2190, 0x2190, "<-"}, {0x2191, 0x2191, "^"}, {0x2192, 0x2192, "->"},
    {0x2193, 0x2193, "v"}, {0x2194, 0x2194, "<->"}, {0x21D2, 0x21D2, "=>"},
    {0x2212, 0x2212, "-"}, {0x2215, 0x2215, "/"}, {0x2217, 0x2217, "*"},
    {0x2219, 0x2219, "."}, {0x221E, 0x221E, "inf"}, {0x2248, 0x2248, "~"},
    {0x2260, 0x2260, "!="}, {0x2264, 0x2264, "<="}, {0x2265, 0x2265, ">="},
    {0x2500, 0x2501, "-"}, {0x2502, 0x2503, "|"}, {0x25B2, 0x25B2, "^"},
    {0x25BC, 0x25BC, "v"}, {0x3000, 0x3000, " "}, {0xFEFF, 0xFEFF, ""},
    {0xFFFF, 0xFFFF, ""}};

/* ------------------------------------------------------------------ */
/* state                                                               */

enum {
    S_TEXT, S_LT, S_LTSLASH, S_TNAME, S_ABEFORE, S_ANAME, S_AAFTER, S_VBEFORE, S_VQ, S_VU,
    S_ENT, S_BANG, S_BANGD, S_COMMENT, S_SKIPTAG, S_RAW, S_RAWLT, S_RAWEND, S_RAWENDC, S_PLAIN
};
enum { SK_TEXT, SK_TITLE, SK_CAP, SK_NONE };

/* buffers, allocated while a page is loading */
struct hstate {
    char line[COLS], word[COLS];
    char abuf[ABUF];
    uint16_t aoff[A_N];
    char href[HTML_STR_MAX + 1];
    char cap[CAP_MAX + 1];
    char oval[64];
    char sel_label[COLS + 1];
    char ent[12];
    char tname[11], aname[12], rname[10];
    uint8_t stk[STK];
    uint8_t open_cnt[T_COUNT];
    struct html_item sel_item, ta_item;
    struct html_form fm;
};
static struct hstate *hs;

uint16_t html_lines, html_items, html_numbers, html_main_line;
uint8_t html_forms, html_full;
char html_title[COLS + 1];

static uint16_t heap_used;
static uint8_t tag_hash[128];
static uint16_t hide_hash[N_HIDE_CLASSES];
static uint8_t tables_ready;

/* tokenizer */
static uint8_t st, st_ret, closing, selfclose, tid, tname_n, tname_long, th;
static uint8_t aname_n, aid, quote, cap_open, cap_left, class_on, tag_hide, tag_main;
static uint16_t abuf_n, cw_h;
static uint8_t ent_n, dash, raw_id, raw_n, raw_len, raw_cap;
static uint8_t u_need, u_big, unk_run;
static uint16_t u_cp;

/* layout */
static uint8_t col, lmargin, word_n, pending_space, blank, indent, in_pre, upper;
static uint8_t sink, hide, sd, sover, link_pending, cell_sep, row_cells, last_th;
static uint8_t ld, bq, ddn, ol_mask, in_button, title_cap, cap_n, title_sp, cap_sp;
static uint8_t olnum[8];
static char bullet[5];

/* forms */
static uint8_t cur_form, in_select, opt_open, opt_sel, opt_has_val, sel_max;

#define line (hs->line)
#define word (hs->word)

/* macros: a call to another flash page costs more than the test */
#define lower(c) ((c) >= 'A' && (c) <= 'Z' ? (c) + 32 : (c))
#define is_space(c) ((c) == ' ' || (c) == '\n' || (c) == '\t' || (c) == '\r' || (c) == '\f')

/* ------------------------------------------------------------------ */
/* page start and end (cold code first: the hot code below should share
 * one flash page) */

static void update_sink(void);
static void raw_end(void);
static void select_end(void);
static void form_end(void);
static void flush_word(void);
static void emit_line(void);

static void init_tables(void)
{
    uint8_t id, h, i;
    const char *s;
    for (id = 1; id < T_COUNT; id++) {
        const char *n = tag_names_bk[id - 1];
        h = 0;
        for (i = 0; i < 10 && n[i]; i++)
            h = (uint8_t)(h * 5 + (uint8_t)n[i]);
        i = h & 127;
        while (tag_hash[i])
            i = (i + 1) & 127;
        tag_hash[i] = id;
    }
    s = hide_classes_bk;
    for (i = 0; i < N_HIDE_CLASSES; i++) {
        uint16_t v = 0;
        for (; *s; s++)
            v = (uint16_t)((v << 5) + v + (uint8_t)*s);
        hide_hash[i] = v;
        s++;
    }
    tables_ready = 1;
}

void html_begin(uint8_t is_html)
{
    if (!tables_ready)
        init_tables();
    if (!hs)
        hs = calloc(1, sizeof(*hs));
    else
        memset(hs, 0, sizeof(*hs));
    html_lines = html_items = html_numbers = 0;
    html_main_line = HTML_NONE;
    html_forms = html_full = 0;
    html_title[0] = 0;
    heap_used = 0;
    st = is_html ? S_TEXT : S_PLAIN;
    in_pre = !is_html;
    col = lmargin = word_n = pending_space = indent = upper = 0;
    blank = 1;
    hide = sd = sover = link_pending = cell_sep = row_cells = last_th = 0;
    ld = bq = ddn = ol_mask = in_button = title_cap = cap_n = 0;
    raw_id = 0;
    u_need = unk_run = 0;
    bullet[0] = 0;
    cur_form = HTML_NOFORM;
    in_select = opt_open = 0;
    update_sink();
}

void html_end(void)
{
    if (!hs)
        return;
    if (raw_id == T_TEXTAREA)
        raw_end();
    raw_id = 0;
    select_end();
    form_end();
    flush_word();
    if (col)
        emit_line();
    free(hs);
    hs = NULL;
}

/* ------------------------------------------------------------------ */
/* storage                                                             */

void html_get_line(uint16_t n, char *buf)
{
    xram_read(HX_TEXT + (uint32_t)n * COLS, buf, COLS);
}

void html_get_item(uint16_t idx, struct html_item *it)
{
    xram_read(HX_ITEMS + (uint32_t)idx * sizeof(*it), it, sizeof(*it));
}

void html_put_item(uint16_t idx, const struct html_item *it)
{
    xram_write(HX_ITEMS + (uint32_t)idx * sizeof(*it), it, sizeof(*it));
}

void html_get_form(uint8_t f, struct html_form *fm)
{
    xram_read(HX_FORMS + (uint16_t)f * sizeof(*fm), fm, sizeof(*fm));
}

uint16_t html_add_str(const char *s)
{
    uint16_t n = (uint16_t)strlen(s) + 1, off = heap_used;
    if (n > HTML_STR_MAX + 1 || heap_used + n > HTML_HEAP_SIZE)
        return HTML_NONE;
    xram_write(HX_HEAP + off, s, n);
    heap_used += n;
    return off;
}

void html_get_str(uint16_t off, char *buf, uint16_t max)
{
    buf[0] = 0;
    if (off == HTML_NONE)
        return;
    xram_read(HX_HEAP + off, buf, max);
    buf[max - 1] = 0;
}

static uint16_t str_skip(uint16_t off)
{
    char buf[16];
    uint8_t i;
    for (;;) {
        xram_read(HX_HEAP + off, buf, sizeof(buf));
        for (i = 0; i < sizeof(buf); i++, off++)
            if (!buf[i])
                return off + 1;
    }
}

void html_get_option(const struct html_item *it, uint8_t k, char *value, char *label)
{
    uint16_t off = it->value;
    uint8_t i;
    for (i = 0; i < k; i++)
        off = str_skip(str_skip(off));
    if (value)
        html_get_str(off, value, HTML_STR_MAX + 1);
    if (label)
        html_get_str(str_skip(off), label, HTML_COLS + 1);
}

uint16_t html_find_number(uint16_t n, struct html_item *it)
{
    uint16_t i, k = 0;
    for (i = 0; i < html_items; i++) {
        html_get_item(i, it);
        if ((it->flags & ITF_NUMBERED) && ++k == n)
            return i;
    }
    return HTML_NONE;
}

/* ------------------------------------------------------------------ */
/* lines                                                               */

static void emit_line(void)
{
    if (html_lines < HTML_MAX_LINES) {
        memset(line + col, ' ', COLS - col);
        xram_write(HX_TEXT + (uint32_t)html_lines * COLS, line, COLS);
        html_lines++;
    } else {
        html_full = 1;
    }
    col = 0;
}

/* first content of a line: indentation and a pending list bullet */
static void line_start(void)
{
    uint8_t b = indent >= 2 ? indent - 2 : 0, n;
    memset(line, ' ', 12);
    col = lmargin = indent;
    if (bullet[0]) {
        n = (uint8_t)strlen(bullet);
        memcpy(line + b, bullet, n);
        if (b + n + 1 > col)
            col = b + n + 1;
        if (col > COLS - 4)
            col = COLS - 4;
        bullet[0] = 0;
    }
}

static void flush_word(void)
{
    uint8_t i = 0;
    if (!word_n)
        return;
    if (col > lmargin && col + pending_space + word_n > COLS)
        emit_line();
    if (!col)
        line_start();
    else if (pending_space && col > lmargin)
        line[col++] = ' ';
    pending_space = 0;
    blank = 0;
    while (i < word_n) {
        /* a font glyph's two cells stay on one line */
        uint8_t pair = FONT_IS_LEAD(word[i]) && i + 1 < word_n;
        if (col == COLS || (pair && col == COLS - 1)) {
            emit_line();
            line_start();
        }
        line[col++] = word[i++];
        if (pair)
            line[col++] = word[i++];
    }
    word_n = 0;
}

static void space(void)
{
    flush_word();
    if (col)
        pending_space = 1;
}

/* end the current line; para: also leave one empty line */
static void out_break(uint8_t para)
{
    flush_word();
    pending_space = 0;
    if (col) {
        emit_line();
        blank = 0;
    }
    if (para && !blank && html_lines) {
        emit_line();
        blank = 1;
    }
}

static void set_indent(void)
{
    uint8_t i = (uint8_t)(2 * (ld + bq + ddn));
    indent = i > 8 ? 8 : i;
}

/* ------------------------------------------------------------------ */
/* items                                                               */

static uint16_t new_item(struct html_item *it, uint8_t numbered)
{
    if (html_items >= HTML_MAX_ITEMS)
        return HTML_NONE;
    it->flags |= numbered ? ITF_NUMBERED : 0;
    html_put_item(html_items, it);
    if (numbered)
        html_numbers++;
    return html_items++;
}

static uint8_t marker(uint16_t n, char *out)
{
    char tmp[6];
    uint8_t k = 0, i = 0;
    do {
        tmp[k++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    while (k)
        out[i++] = (char)(tmp[--k] | 0x80);
    return i;
}

static void cell_separator(void)
{
    uint8_t s = cell_sep;
    cell_sep = 0;
    if (col || word_n) {
        if (s == 2) {
            if (word_n == COLS)
                flush_word();
            word[word_n++] = ':';
        }
        space();
    }
}

static void start_link(void)
{
    struct html_item it;
    char m[6];
    uint8_t n, i;
    link_pending = 0;
    memset(&it, 0, sizeof(it));
    it.kind = IT_LINK;
    it.form = HTML_NOFORM;
    it.value = HTML_NONE;
    it.name = html_add_str(hs->href);
    if (it.name == HTML_NONE || new_item(&it, 1) == HTML_NONE)
        return;
    n = marker(html_numbers, m);
    if (word_n + n >= COLS)
        flush_word();
    for (i = 0; i < n; i++)
        word[word_n++] = m[i];
}

static void put_char(char c)
{
    if (cell_sep)
        cell_separator();
    if (link_pending)
        start_link();
    if (upper && c >= 'a' && c <= 'z')
        c -= 32;
    if (word_n == COLS)
        flush_word();
    word[word_n++] = c;
}

/* ASCII character of the page text */
static void emit_ascii(char c)
{
    uint8_t n;
    switch (sink) {
    case SK_TEXT:
        if ((uint8_t)c <= ' ') {
            if (!in_pre)
                space();
            else if (c == '\n') {
                flush_word();
                pending_space = 0;
                emit_line();
            } else if (c != '\r') {
                put_char(' ');
            }
        } else {
            put_char(c);
        }
        break;
    case SK_TITLE:
        n = (uint8_t)strlen(html_title);
        if (is_space(c))
            c = ' ';
        if (n < COLS && !(c == ' ' && (!n || title_sp))) {
            html_title[n] = c;
            html_title[n + 1] = 0;
            title_sp = c == ' ';
        }
        break;
    case SK_CAP:
        if (is_space(c))
            c = ' ';
        if (cap_n < CAP_MAX && !(c == ' ' && (!cap_n || cap_sp))) {
            hs->cap[cap_n++] = c;
            cap_sp = c == ' ';
        }
        break;
    }
}

/* a glyph of the external font: two cells, a line may break before and
 * after it (no spaces between CJK characters) */
static void emit_wide(uint16_t g)
{
    uint8_t n;
    switch (sink) {
    case SK_TEXT:
        if (cell_sep)
            cell_separator();
        if (link_pending)
            start_link();
        if (word_n && !(word[word_n - 1] & 0x80))
            flush_word();               /* keep only a link number in front */
        if (word_n + 2 > COLS)
            flush_word();
        word[word_n++] = FONT_LEAD(g);
        word[word_n++] = FONT_TRAIL(g);
        flush_word();
        break;
    case SK_TITLE:
        n = (uint8_t)strlen(html_title);
        if (n + 2 <= COLS) {
            html_title[n] = FONT_LEAD(g);
            html_title[n + 1] = FONT_TRAIL(g);
            html_title[n + 2] = 0;
            title_sp = 0;
        }
        break;
    case SK_CAP:
        if (cap_n + 2 <= CAP_MAX) {
            hs->cap[cap_n++] = FONT_LEAD(g);
            hs->cap[cap_n++] = FONT_TRAIL(g);
            cap_sp = 0;
        }
        break;
    }
}

static void emit_cp(uint16_t cp)
{
    const struct cp_map *m;
    const char *s;
    if (cp < 0x80) {
        unk_run = 0;
        emit_ascii((char)cp);
        return;
    }
    if (cp < 0xA0)
        return;
    if (cp < 0x180) {
        char c = latin_bk[cp - 0xA0];
        if (c) {
            unk_run = 0;
            emit_ascii(c);
        }
        return;
    }
    if (cp > 0x3000 && cp < 0xFEFF)          /* CJK and the like: the font */
        goto font;
    if (cp >= 0xFF01 && cp <= 0xFF5E) {     /* full-width ASCII */
        unk_run = 0;
        emit_ascii((char)(cp - 0xFEE0));
        return;
    }
    for (m = cpmap_bk; m->lo != 0xFFFF; m++) {
        if (cp >= m->lo && cp <= m->hi) {
            char rep[4];
            memcpy(rep, m->s, 4);
            for (s = rep; *s; s++)
                emit_ascii(*s);
            unk_run = 0;
            return;
        }
    }
font:
    {
        uint16_t g = font_find(cp);
        if (g != FONT_NONE) {
            unk_run = 0;
            emit_wide(g);
            return;
        }
    }
    if (!unk_run)
        emit_ascii('?');
    unk_run = 1;
}

/* a byte of text (UTF-8) */
static void text_byte(uint8_t c)
{
    if (c < 0x80) {
        u_need = 0;
        unk_run = 0;
        emit_ascii((char)c);
    } else if ((c & 0xC0) == 0x80) {
        if (u_need) {
            u_cp = (u_cp << 6) | (c & 0x3F);
            if (!--u_need)
                emit_cp(u_big ? 0xFFFD : u_cp);
        }
    } else if (c >= 0xF0) {
        u_need = 3;
        u_big = 1;              /* outside the BMP: unknown */
    } else if (c >= 0xE0) {
        u_need = 2;
        u_big = 0;
        u_cp = c & 0x0F;
    } else {
        u_need = 1;
        u_big = 0;
        u_cp = c & 0x1F;
    }
}

/* page text from an attribute value (UTF-8) */
static void text_str(const char *s)
{
    while (*s)
        text_byte((uint8_t)*s++);
    u_need = 0;
}

static void update_sink(void)
{
    if (raw_id == T_TITLE)
        sink = title_cap ? SK_TITLE : SK_NONE;
    else if (raw_id == T_TEXTAREA)
        sink = SK_CAP;
    else if (hide)
        sink = SK_NONE;
    else if (in_select)
        sink = opt_open ? SK_CAP : SK_NONE;
    else
        sink = SK_TEXT;
}

/* ------------------------------------------------------------------ */
/* form fields                                                         */

/* length of s (n cells) without trailing spaces; a font glyph's trail byte
 * may look like a space */
static uint8_t cells_trim(const char *s, uint8_t n)
{
    uint8_t i = 0, end = 0;
    while (i < n) {
        if (FONT_IS_LEAD(s[i]) && i + 1 < n) {
            i += 2;
            end = i;
        } else if (s[i++] != ' ') {
            end = i;
        }
    }
    return end;
}

/* cells for a raw UTF-8 string (field values, button labels), at most w */
static uint8_t utf8_cells(const char *v, char *out, uint8_t w)
{
    uint8_t i = 0;
    while (*v && i < w) {
        uint8_t c = (uint8_t)*v++, k;
        uint16_t cp, g;
        if (c < 0x80) {
            out[i++] = (char)c;
            continue;
        }
        if ((c & 0xC0) == 0x80)
            continue;
        k = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
        cp = c & (k == 2 ? 0x0F : 0x1F);
        while (k-- && ((uint8_t)*v & 0xC0) == 0x80)
            cp = (uint16_t)(cp << 6 | (*v++ & 0x3F));
        if (c >= 0xF0)
            cp = 0xFFFD;
        if (cp >= 0xA0 && cp < 0x180 && latin_bk[cp - 0xA0]) {
            out[i++] = latin_bk[cp - 0xA0];
            continue;
        }
        g = font_find(cp);
        if (g == FONT_NONE) {
            out[i++] = '?';
        } else if (i + 2 <= w) {
            out[i++] = FONT_LEAD(g);
            out[i++] = FONT_TRAIL(g);
        } else {
            break;
        }
    }
    return i;
}

/* picture of a field's value, width cells */
static void value_area(const struct html_item *it, const char *v, char *out)
{
    uint8_t i = 0, w = it->width;
    switch (it->kind) {
    case IT_CHECK:
        out[0] = it->flags & ITF_CHECKED ? 'x' : ' ';
        return;
    case IT_RADIO:
        out[0] = it->flags & ITF_CHECKED ? '*' : ' ';
        return;
    case IT_PASSWORD:
        while (*v && i < w) {
            out[i++] = '*';
            v++;
        }
        break;
    case IT_SELECT:
        /* the label is page text already */
        while (*v && i < w) {
            if (FONT_IS_LEAD(*v)) {
                if (!v[1] || i + 2 > w)
                    break;
                out[i++] = *v++;
            }
            out[i++] = *v++;
        }
        break;
    default:
        i = utf8_cells(v, out, w);
    }
    while (i < w)
        out[i++] = it->kind == IT_SELECT || it->kind == IT_SUBMIT ? ' ' : '_';
}

/* put an item's field into the page: number, brackets, value */
static void place_field(struct html_item *it, const char *v, uint16_t idx)
{
    char m[6], buf[COLS];
    uint8_t mn, total, avail;
    char open = '[', close = ']';
    (void)idx;
    if (cell_sep)
        cell_separator();
    flush_word();
    mn = marker(html_numbers, m);
    avail = COLS - (indent < 8 ? indent : 8) - mn - 2;
    if (it->width > avail)
        it->width = avail;
    total = mn + it->width + 2;
    if (col > lmargin && col + pending_space + total > COLS)
        emit_line();
    if (!col)
        line_start();
    else if (pending_space && col > lmargin)
        line[col++] = ' ';
    pending_space = 0;
    blank = 0;
    if (col + total > COLS) {
        emit_line();
        line_start();
    }
    if (it->kind == IT_RADIO) {
        open = '(';
        close = ')';
    } else if (it->kind == IT_SELECT) {
        open = '<';
        close = '>';
    }
    memcpy(line + col, m, mn);
    col += mn;
    line[col++] = open;
    it->pos = (uint16_t)(html_lines * COLS + col);
    value_area(it, v, buf);
    memcpy(line + col, buf, it->width);
    col += it->width;
    line[col++] = close;
}

void html_show_field(const struct html_item *it)
{
    char v[COLS + 1], area[COLS];
    uint16_t ln = it->pos / COLS;
    if (it->kind == IT_SELECT) {
        html_get_option(it, it->sel, NULL, v);
    } else {
        html_get_str(it->value, v, sizeof(v));
    }
    value_area(it, v, area);
    if (hs && ln == html_lines)
        memcpy(line + it->pos % COLS, area, it->width);
    else if (ln < html_lines)
        xram_write(HX_TEXT + it->pos, area, it->width);
}

static const char *attr(uint8_t a)
{
    return hs->aoff[a] == HTML_NONE ? NULL : hs->abuf + hs->aoff[a];
}

static uint8_t attr_is(uint8_t a, const char *s)
{
    const char *v = attr(a);
    if (!v)
        return 0;
    for (; *s; v++, s++)
        if (lower(*v) != *s)
            return 0;
    return *v == 0;
}

static void form_save(void)
{
    if (cur_form != HTML_NOFORM)
        xram_write(HX_FORMS + (uint16_t)cur_form * sizeof(hs->fm), &hs->fm, sizeof(hs->fm));
}

static void form_end(void)
{
    form_save();
    cur_form = HTML_NOFORM;
}

static void form_start(void)
{
    const char *a;
    form_end();
    if (html_forms >= HTML_MAX_FORMS)
        return;
    a = attr(A_ACTION);
    hs->fm.action = a && *a ? html_add_str(a) : HTML_NONE;
    hs->fm.post = attr_is(A_METHOD, "post");
    hs->fm.n_text = 0;
    cur_form = html_forms++;
    form_save();
}


static void field_init(struct html_item *it, uint8_t kind)
{
    const char *a = attr(A_NAME);
    memset(it, 0, sizeof(*it));
    it->kind = kind;
    it->form = cur_form;
    it->name = a && *a ? html_add_str(a) : HTML_NONE;
    it->value = HTML_NONE;
}

static void text_field(struct html_item *it, uint8_t width)
{
    char v[COLS + 1];
    uint16_t idx;
    html_get_str(it->value, v, sizeof(v));
    it->width = width;
    idx = new_item(it, 1);
    if (idx == HTML_NONE)
        return;
    place_field(it, v, idx);
    html_put_item(idx, it);
    if (cur_form != HTML_NOFORM)
        hs->fm.n_text++;
}

static void input_tag(void)
{
    struct html_item it;
    const char *v = attr(A_VALUE);
    const char *t = attr(A_TYPE);
    uint8_t kind = IT_TEXT, w = 12, invisible = hide || tag_hide;
    uint16_t idx;
    char tl[10];
    uint8_t i = 0;
    if (attr(A_DISABLED) || cur_form == HTML_NOFORM)
        return;
    if (t) {
        while (t[i] && i < sizeof(tl) - 1) {
            tl[i] = lower(t[i]);
            i++;
        }
    }
    tl[i] = 0;
    if (!strcmp(tl, "hidden"))
        kind = IT_HIDDEN;
    else if (!strcmp(tl, "checkbox"))
        kind = IT_CHECK;
    else if (!strcmp(tl, "radio"))
        kind = IT_RADIO;
    else if (!strcmp(tl, "submit") || !strcmp(tl, "image"))
        kind = IT_SUBMIT;
    else if (!strcmp(tl, "password"))
        kind = IT_PASSWORD;
    else if (!strcmp(tl, "button") || !strcmp(tl, "reset") || !strcmp(tl, "file"))
        return;
    field_init(&it, kind);
    if (kind == IT_CHECK || kind == IT_RADIO) {
        it.value = html_add_str(v ? v : "on");
        if (attr(A_CHECKED))
            it.flags |= ITF_CHECKED;
        it.width = 1;
    } else if (v) {
        it.value = html_add_str(v);
    }
    if (kind == IT_HIDDEN || invisible) {
        if (it.name == HTML_NONE || kind == IT_SUBMIT ||
            ((kind == IT_CHECK || kind == IT_RADIO) && !(it.flags & ITF_CHECKED)))
            return;
        it.kind = kind == IT_CHECK || kind == IT_RADIO ? kind : IT_HIDDEN;
        new_item(&it, 0);
        return;
    }
    if (kind == IT_SUBMIT) {
        const char *label = v ? v : attr(A_ALT);
        char lb[COLS + 1];
        uint8_t n;
        if (!label || !*label)
            label = "Submit";
        strncpy(lb, label, COLS);
        lb[COLS] = 0;
        n = (uint8_t)strlen(lb);
        it.width = n > 16 ? 16 : n;
        idx = new_item(&it, 1);
        if (idx != HTML_NONE) {
            place_field(&it, lb, idx);
            html_put_item(idx, &it);
        }
        return;
    }
    if (kind == IT_CHECK || kind == IT_RADIO) {
        idx = new_item(&it, 1);
        if (idx != HTML_NONE) {
            place_field(&it, "", idx);
            html_put_item(idx, &it);
        }
        return;
    }
    if ((t = attr(A_SIZE)) != NULL) {
        w = (uint8_t)atoi(t);
        w = w < 4 ? 4 : w > 16 ? 16 : w;
    }
    text_field(&it, w);
}

static void button_start(void)
{
    struct html_item it;
    char m[6];
    uint8_t n, i;
    const char *v = attr(A_VALUE);
    in_button = 0;
    if (sink != SK_TEXT || cur_form == HTML_NOFORM || attr(A_DISABLED) || (attr(A_TYPE) && !attr_is(A_TYPE, "submit")))
        return;
    field_init(&it, IT_SUBMIT);
    it.value = v ? html_add_str(v) : HTML_NONE;
    if (new_item(&it, 1) == HTML_NONE)
        return;
    n = marker(html_numbers, m);
    if (cell_sep)
        cell_separator();
    if (word_n + n + 1 >= COLS)
        flush_word();
    for (i = 0; i < n; i++)
        word[word_n++] = m[i];
    word[word_n++] = '[';
    in_button = 1;
}

static void option_end(void)
{
    uint16_t off;
    uint8_t n;
    if (!opt_open)
        return;
    opt_open = 0;
    cap_n = cells_trim(hs->cap, cap_n);
    hs->cap[cap_n] = 0;
    off = html_add_str(opt_has_val ? hs->oval : hs->cap);
    if (off == HTML_NONE || html_add_str(hs->cap) == HTML_NONE || hs->sel_item.nopt == 255) {
        update_sink();
        return;
    }
    if (!hs->sel_item.nopt)
        hs->sel_item.value = off;
    if (opt_sel || !hs->sel_item.nopt) {
        hs->sel_item.sel = hs->sel_item.nopt;
        strncpy(hs->sel_label, hs->cap, COLS);
        hs->sel_label[COLS] = 0;
    }
    hs->sel_item.nopt++;
    n = cap_n > 16 ? 16 : cap_n;
    if (n > sel_max)
        sel_max = n;
    update_sink();
}

static void option_start(void)
{
    const char *v = attr(A_VALUE);
    option_end();
    if (!in_select)
        return;
    opt_open = 1;
    opt_sel = attr(A_SELECTED) != NULL;
    opt_has_val = v != NULL;
    if (v) {
        strncpy(hs->oval, v, sizeof(hs->oval) - 1);
        hs->oval[sizeof(hs->oval) - 1] = 0;
    }
    cap_n = 0;
    update_sink();
}

static void select_start(void)
{
    in_select = 1;
    field_init(&hs->sel_item, IT_SELECT);
    hs->sel_label[0] = 0;
    sel_max = 1;
    update_sink();
}

static void select_end(void)
{
    uint16_t idx;
    option_end();
    if (!in_select)
        return;
    in_select = 0;
    update_sink();
    if (!hs->sel_item.nopt)
        return;
    hs->sel_item.width = sel_max;
    idx = new_item(&hs->sel_item, 1);
    if (idx == HTML_NONE)
        return;
    place_field(&hs->sel_item, hs->sel_label, idx);
    html_put_item(idx, &hs->sel_item);
}

/* ------------------------------------------------------------------ */
/* elements                                                            */

static uint8_t flags_of(uint8_t id)
{
    return id < T_COUNT ? tag_flags_bk[id] : 0;
}

static void element_end(uint8_t id)
{
    uint8_t f = flags_of(id);
    switch (id) {
    case T_A:
        link_pending = 0;
        break;
    case T_H1: case T_H2: case T_H3:
        upper = 0;
        break;
    case T_UL: case T_OL: case T_MENU:
        if (ld)
            ld--;
        set_indent();
        break;
    case T_DD:
        if (ddn)
            ddn--;
        set_indent();
        break;
    case T_BLOCKQUOTE:
        if (bq)
            bq--;
        set_indent();
        break;
    case T_PRE:
        if (in_pre)
            in_pre--;
        break;
    case T_TR:
        out_break(0);
        break;
    case T_TD:
    case T_TH:
        last_th = id == T_TH;
        break;
    case T_SELECT:
        select_end();
        break;
    case T_OPTION:
        option_end();
        break;
    case T_BUTTON:
        if (in_button) {
            put_char(']');
            in_button = 0;
        }
        break;
    case T_LI:
        out_break(0);
        bullet[0] = 0;
        break;
    }
    if (f & TF_BLOCK)
        out_break(f & TF_PARA);
}

static void pop_to(uint8_t i)
{
    while (sd > i) {
        uint8_t id = hs->stk[--sd];
        if (id < T_COUNT)
            hs->open_cnt[id]--;
        if (hide) {
            if (sd < hide) {
                hide = 0;
                update_sink();
            }
            continue;
        }
        element_end(id);
    }
}

/* index of the topmost open element in targets[], stopping at stops[] */
static int8_t find_open(const uint8_t *targets, const uint8_t *stops)
{
    uint8_t i, id;
    const uint8_t *p;
    for (i = sd; i > 0; i--) {
        id = hs->stk[i - 1];
        for (p = targets; *p; p++)
            if (*p == id)
                return (int8_t)(i - 1);
        for (p = stops; *p; p++)
            if (*p == id)
                return -1;
    }
    return -1;
}

static void close_open(uint8_t t1, uint8_t t2, uint8_t s1, uint8_t s2)
{
    uint8_t targets[3], stops[4];
    int8_t i;
    if (!hs->open_cnt[t1] && (!t2 || !hs->open_cnt[t2]))
        return;
    targets[0] = t1;
    targets[1] = t2;
    targets[2] = 0;
    stops[0] = s1;
    stops[1] = s2;
    stops[2] = T_TABLE;
    stops[3] = 0;
    i = find_open(targets, stops);
    if (i >= 0)
        pop_to((uint8_t)i);
}

static void implied_close(uint8_t id, uint8_t f)
{
    if (f & TF_BLOCK)
        close_open(T_P, 0, T_BUTTON, T_TD);
    switch (id) {
    case T_LI:
        close_open(T_LI, 0, T_UL, T_OL);
        break;
    case T_DT:
    case T_DD:
        close_open(T_DT, T_DD, T_DL, 0);
        break;
    case T_TR:
        close_open(T_TR, 0, 0, 0);
        break;
    case T_TD:
    case T_TH:
        close_open(T_TD, T_TH, T_TR, 0);
        break;
    case T_TBODY:
    case T_THEAD:
    case T_TFOOT:
        close_open(T_TBODY, T_THEAD, 0, 0);
        close_open(T_TFOOT, 0, 0, 0);
        break;
    case T_OPTION:
    case T_OPTGROUP:
        close_open(T_OPTION, 0, T_SELECT, 0);
        break;
    case T_A:
        close_open(T_A, 0, 0, 0);
        break;
    case T_SELECT:
        close_open(T_SELECT, 0, 0, 0);
        break;
    case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6:
        if (sd && hs->stk[sd - 1] >= T_H1 && hs->stk[sd - 1] <= T_H6)
            pop_to(sd - 1);
        break;
    }
}

static uint8_t hexval(char c)
{
    c = lower(c);
    return (uint8_t)(c <= '9' ? c - '0' : c - 'a' + 10);
}

static void set_link(const char *h)
{
    char *o = hs->href;
    const char *u;
    uint8_t n = 0;
    link_pending = 0;
    if (!h || !*h || *h == '#' || !memcmp(h, "javascript:", 11) || !memcmp(h, "mailto:", 7) ||
        !memcmp(h, "tel:", 4) || !memcmp(h, "data:", 5))
        return;
    /* DuckDuckGo result links: the target is in the uddg parameter */
    if (strstr(h, "duckduckgo.com/l/") && (u = strstr(h, "uddg=")) != NULL) {
        for (u += 5; *u && *u != '&' && n < HTML_STR_MAX; u++) {
            char c = *u;
            if (c == '%' && u[1] && u[2]) {
                c = (char)(hexval(u[1]) << 4 | hexval(u[2]));
                u += 2;
            } else if (c == '+') {
                c = ' ';
            }
            o[n++] = c;
        }
        o[n] = 0;
    } else {
        strncpy(o, h, HTML_STR_MAX);
        o[HTML_STR_MAX] = 0;
    }
    link_pending = 1;
}

static void main_mark(void)
{
    if (html_main_line == HTML_NONE) {
        out_break(0);
        html_main_line = html_lines;
    }
}

static void element_start(uint8_t id, uint8_t f)
{
    if (f & TF_BLOCK)
        out_break(f & TF_PARA);
    if (tag_main)
        main_mark();
    switch (id) {
    case T_A:
        set_link(attr(A_HREF));
        break;
    case T_H1: case T_H2: case T_H3:
        upper = 1;
        break;
    case T_UL: case T_MENU:
        if (ld < 8)
            ol_mask &= (uint8_t)~(1 << ld);
        ld++;
        set_indent();
        break;
    case T_OL:
        if (ld < 8) {
            ol_mask |= (uint8_t)(1 << ld);
            olnum[ld] = 0;
        }
        ld++;
        set_indent();
        break;
    case T_LI: {
        uint8_t d = ld ? ld - 1 : 0;
        if (d < 8 && (ol_mask & (1 << d))) {
            uint8_t n = ++olnum[d];
            if (n >= 100)
                n = 99;
            bullet[0] = n >= 10 ? (char)('0' + n / 10) : (char)('0' + n);
            bullet[1] = n >= 10 ? (char)('0' + n % 10) : '.';
            bullet[2] = n >= 10 ? '.' : 0;
            bullet[3] = 0;
        } else {
            bullet[0] = '*';
            bullet[1] = 0;
        }
        if (!ld)
            indent = indent < 2 ? 2 : indent;
        break;
    }
    case T_DD:
        ddn++;
        set_indent();
        break;
    case T_BLOCKQUOTE:
        bq++;
        set_indent();
        break;
    case T_PRE:
        in_pre++;
        break;
    case T_TR:
        row_cells = 0;
        last_th = 0;
        break;
    case T_TD:
    case T_TH:
        if (row_cells++)
            cell_sep = last_th && id == T_TD ? 2 : 1;
        break;
    case T_MAIN:
        main_mark();
        break;
    case T_FORM:
        form_start();
        break;
    case T_SELECT:
        select_start();
        break;
    case T_OPTION:
        option_start();
        break;
    case T_BUTTON:
        button_start();
        break;
    }
}

static void void_tag(uint8_t id)
{
    const char *a;
    switch (id) {
    case T_BR:
        flush_word();
        pending_space = 0;
        if (col)
            emit_line();
        else if (!blank && html_lines) {
            emit_line();
            blank = 1;
        }
        break;
    case T_HR:
        out_break(0);
        memset(line, '-', COLS);
        col = COLS;
        emit_line();
        break;
    case T_IMG:
        a = attr(A_ALT);
        if (a && *a && !tag_hide) {
            /* TeX of Wikipedia formulas */
            if (!memcmp(a, "{\\displaystyle", 14)) {
                char *e;
                a += 14;
                e = (char *)a + strlen(a);
                if (e > a && e[-1] == '}')
                    e[-1] = 0;
            }
            emit_ascii('[');
            text_str(a);
            emit_ascii(']');
        }
        break;
    case T_INPUT:
        input_tag();
        break;
    }
}

static void enter_raw(uint8_t id)
{
    raw_id = id;
    raw_n = 0;
    memcpy(hs->rname, tag_names_bk[id - 1], 10);
    for (raw_len = 0; raw_len < 10 && hs->rname[raw_len]; raw_len++)
        ;
    raw_cap = (flags_of(id) & TF_RC) != 0;
    if (id == T_TITLE)
        title_cap = !html_title[0] && !hide;
    if (id == T_TEXTAREA) {
        field_init(&hs->ta_item, IT_TEXTAREA);
        cap_n = 0;
    }
    update_sink();
}

static void raw_end(void)
{
    uint8_t id = raw_id;
    raw_id = 0;
    update_sink();
    if (id == T_TITLE) {
        html_title[cells_trim(html_title, (uint8_t)strlen(html_title))] = 0;
        title_cap = 0;
    } else if (id == T_TEXTAREA) {
        struct html_item *it = &hs->ta_item;
        hs->cap[cap_n] = 0;
        it->value = html_add_str(hs->cap);
        if (hide || tag_hide) {
            if (it->name != HTML_NONE) {
                it->kind = IT_HIDDEN;
                new_item(it, 0);
            }
        } else {
            text_field(it, 16);
        }
    }
}

static void start_tag(void)
{
    uint8_t f = flags_of(tid);
    const char *a;
    /* invisible: hidden, aria-hidden, style display:none, some classes */
    if (attr(A_HIDDEN) || attr_is(A_ARIAH, "true"))
        tag_hide = 1;
    if ((a = attr(A_STYLE)) != NULL) {
        const char *d = strstr(a, "display");
        if (d && strstr(d, "none"))
            tag_hide = 1;
    }
    if (attr_is(A_ROLE, "main"))
        tag_main = 1;
    /* buttons that are not submit buttons of a form need JavaScript */
    if (tid == T_BUTTON && (cur_form == HTML_NOFORM || (attr(A_TYPE) && !attr_is(A_TYPE, "submit"))))
        tag_hide = 1;
    if (f & TF_RAW) {
        if (!selfclose)
            enter_raw(tid);
        return;
    }
    if (tid == T_TITLE) {
        if (!hide)
            enter_raw(tid);
        return;
    }
    if (tid == T_TEXTAREA) {
        enter_raw(tid);
        return;
    }
    if (!hide)
        implied_close(tid, f);
    if (f & TF_VOID) {
        if (!hide || tid == T_INPUT)
            void_tag(tid);
        return;
    }
    if (sd < STK) {
        hs->stk[sd++] = tid;
        if (tid < T_COUNT)
            hs->open_cnt[tid]++;
    } else {
        sover++;
        return;
    }
    if (!hide && (tag_hide || (f & TF_HIDE))) {
        hide = sd;
        update_sink();
        if (tid == T_FORM)
            form_start();      /* fields of a hidden form are still sent */
        return;
    }
    if (!hide)
        element_start(tid, f);
}

static void end_tag(void)
{
    uint8_t i;
    if (tid == T_FORM)
        form_end();             /* fields belong to the form until </form> */
    if (tid < T_COUNT && !hs->open_cnt[tid]) {
        if (hide)
            return;
        if (tid == T_P)
            out_break(1);
        else if (tid == T_BR)
            void_tag(T_BR);
        return;
    }
    if (sover) {
        sover--;
        return;
    }
    /* unknown elements are only closed near the top of the stack */
    for (i = sd; i > 0 && (tid < T_COUNT || sd - i < 12); i--) {
        if (hs->stk[i - 1] == tid) {
            pop_to(i - 1);
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* tokenizer                                                           */

static uint8_t tag_lookup(void)
{
    uint8_t i = th & 127, id;
    if (!tname_long) {
        while ((id = tag_hash[i]) != 0) {
            const char *n = tag_names_bk[id - 1];
            if (!memcmp(n, hs->tname, tname_n) && (tname_n == 10 || !n[tname_n]))
                return id;
            i = (i + 1) & 127;
        }
    }
    return (uint8_t)(0x80 | (th & 0x7F));
}

static uint8_t attr_lookup(void)
{
    const char *n = hs->aname;
    switch (aname_n) {
    case 3:
        return !memcmp(n, "alt", 3) ? A_ALT : 0;
    case 4:
        if (!memcmp(n, "href", 4)) return A_HREF;
        if (!memcmp(n, "name", 4)) return A_NAME;
        if (!memcmp(n, "type", 4)) return A_TYPE;
        if (!memcmp(n, "role", 4)) return A_ROLE;
        if (!memcmp(n, "size", 4)) return A_SIZE;
        return 0;
    case 5:
        if (!memcmp(n, "class", 5)) return A_CLASS;
        if (!memcmp(n, "value", 5)) return A_VALUE;
        if (!memcmp(n, "style", 5)) return A_STYLE;
        return 0;
    case 6:
        if (!memcmp(n, "action", 6)) return A_ACTION;
        if (!memcmp(n, "method", 6)) return A_METHOD;
        if (!memcmp(n, "hidden", 6)) return A_HIDDEN;
        return 0;
    case 7:
        return !memcmp(n, "checked", 7) ? A_CHECKED : 0;
    case 8:
        if (!memcmp(n, "selected", 8)) return A_SELECTED;
        if (!memcmp(n, "disabled", 8)) return A_DISABLED;
        return 0;
    case 11:
        return !memcmp(n, "aria-hidden", 11) ? A_ARIAH : 0;
    }
    return 0;
}

static void tag_begin(void)
{
    uint8_t i;
    tname_n = 0;
    tname_long = 0;
    th = 0;
    selfclose = 0;
    tag_hide = 0;
    tag_main = 0;
    abuf_n = 0;
    cap_open = 0;
    class_on = 0;
    for (i = 0; i < A_N; i++)
        hs->aoff[i] = HTML_NONE;
}

static void close_cap(void)
{
    if (cap_open) {
        hs->abuf[abuf_n++] = 0;
        cap_open = 0;
    }
}

static void class_word_end(void)
{
    uint8_t i;
    if (cw_h) {
        for (i = 0; i < N_HIDE_CLASSES; i++)
            if (hide_hash[i] == cw_h)
                tag_hide = 1;
    }
    cw_h = 0;
}

static void attr_name_end(void)
{
    uint8_t a;
    close_cap();
    class_on = 0;
    if (closing)
        return;
    a = attr_lookup();
    aid = a;
    if (!a)
        return;
    if (a == A_CLASS) {
        class_on = 1;
        cw_h = 0;
        return;
    }
    if (!(flags_of(tid) & TF_ATTR) && a != A_STYLE && a != A_HIDDEN && a != A_ARIAH &&
        a != A_ROLE)
        return;
    if (abuf_n + 2 > ABUF)
        return;
    hs->aoff[a] = abuf_n;
    cap_open = 1;
    cap_left = a == A_STYLE ? 48 : a == A_ROLE || a == A_ARIAH || a == A_TYPE ||
               a == A_METHOD || a == A_SIZE ? 12 : HTML_STR_MAX;
    hs->abuf[abuf_n] = 0;
}

static void value_char(uint8_t c)
{
    if (class_on) {
        if (is_space((char)c))
            class_word_end();
        else
            cw_h = (uint16_t)((cw_h << 5) + cw_h + c);
    } else if (cap_open && cap_left && abuf_n + 2 < ABUF) {
        hs->abuf[abuf_n++] = (char)c;
        cap_left--;
    }
}

static void value_end(void)
{
    if (class_on)
        class_word_end();
    class_on = 0;
    close_cap();
}

static void tag_end(void)
{
    close_cap();
    if (class_on)
        class_word_end();
    class_on = 0;
    st = S_TEXT;
    if (closing)
        end_tag();
    else
        start_tag();
    if (raw_id)
        st = S_RAW;
}

static uint16_t entity_cp(uint8_t need_semi, uint8_t semi)
{
    const struct ent_def *e;
    char *n = hs->ent;
    hs->ent[ent_n] = 0;
    if (n[0] == '#') {
        uint16_t v = 0;
        uint8_t hex = n[1] == 'x' || n[1] == 'X';
        n += hex ? 2 : 1;
        if (!*n)
            return 0;
        for (; *n; n++) {
            char c = lower(*n);
            if (c >= '0' && c <= '9')
                v = v * (hex ? 16 : 10) + c - '0';
            else if (hex && c >= 'a' && c <= 'f')
                v = v * 16 + c - 'a' + 10;
            else
                return 0;
        }
        return v ? v : 0;
    }
    if (need_semi && !semi)
        return 0;
    for (e = ents_bk; e->name[0]; e++)
        if (!strcmp(e->name, n))
            return e->cp;
    /* accented letters: eacute, Ouml, ntilde, aring, ... */
    if (ent_n >= 4 && ((n[0] | 32) >= 'a' && (n[0] | 32) <= 'z')) {
        const char *a;
        for (a = accents_bk; *a; a += strlen(a) + 1)
            if (!strcmp(n + 1, a))
                return (uint8_t)n[0];
    }
    return 0;
}

/* entity ended by c (';' is consumed) */
static void entity_end(uint8_t semi)
{
    uint8_t in_attr = st_ret == S_VQ || st_ret == S_VU;
    uint16_t cp = entity_cp(in_attr, semi);
    uint8_t i;
    st = st_ret;
    if (in_attr) {
        if (!cp) {
            value_char('&');
            for (i = 0; i < ent_n; i++)
                value_char((uint8_t)hs->ent[i]);
            if (semi)
                value_char(';');
        } else if (cp < 0x80) {
            value_char((uint8_t)cp);
        } else if (cp < 0x800) {
            value_char((uint8_t)(0xC0 | (cp >> 6)));
            value_char((uint8_t)(0x80 | (cp & 0x3F)));
        } else {
            value_char((uint8_t)(0xE0 | (cp >> 12)));
            value_char((uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
            value_char((uint8_t)(0x80 | (cp & 0x3F)));
        }
        return;
    }
    if (!cp) {
        text_byte('&');
        for (i = 0; i < ent_n; i++)
            text_byte((uint8_t)hs->ent[i]);
        if (semi)
            text_byte(';');
    } else {
        unk_run = 0;
        emit_cp(cp);
    }
}

void html_feed(const uint8_t *p, uint16_t n)
{
    const uint8_t *end = p + n;
    uint8_t c;
    if (!hs)
        return;
    while (p < end) {
        c = *p;
        switch (st) {
        case S_TEXT:
            /* fast path: plain ASCII words */
            if (sink == SK_TEXT && !link_pending && !cell_sep && !upper && !in_pre) {
                if (c > ' ' && c < 0x7F)
                    unk_run = 0;
                while (c > ' ' && c < 0x7F && c != '<' && c != '&' && word_n < COLS) {
                    word[word_n++] = (char)c;
                    if (++p == end)
                        return;
                    c = *p;
                }
            }
            if (c == '<') {
                st = S_LT;
            } else if (c == '&') {
                st = S_ENT;
                st_ret = S_TEXT;
                ent_n = 0;
            } else {
                text_byte(c);
            }
            break;
        case S_LT:
            if ((c | 32) >= 'a' && (c | 32) <= 'z') {
                closing = 0;
                tag_begin();
                st = S_TNAME;
                continue;
            }
            if (c == '/') {
                st = S_LTSLASH;
            } else if (c == '!') {
                st = S_BANG;
            } else if (c == '?') {
                st = S_SKIPTAG;
            } else {
                st = S_TEXT;
                text_byte('<');
                continue;
            }
            break;
        case S_LTSLASH:
            if ((c | 32) >= 'a' && (c | 32) <= 'z') {
                closing = 1;
                tag_begin();
                st = S_TNAME;
                continue;
            }
            st = S_SKIPTAG;
            continue;
        case S_TNAME:
            if (is_space((char)c) || c == '/' || c == '>') {
                tid = tag_lookup();
                st = S_ABEFORE;
                continue;
            }
            c = (uint8_t)lower((char)c);
            if (tname_n < 10)
                hs->tname[tname_n++] = (char)c;
            else
                tname_long = 1;
            th = (uint8_t)(th * 5 + c);
            break;
        case S_ABEFORE:
            if (c == '>') {
                tag_end();
            } else if (c == '/') {
                selfclose = 1;
            } else if (!is_space((char)c)) {
                selfclose = 0;
                aname_n = 0;
                st = S_ANAME;
                continue;
            }
            break;
        case S_ANAME:
            if (c == '=' || c == '>' || c == '/' || is_space((char)c)) {
                attr_name_end();
                st = c == '=' ? S_VBEFORE : is_space((char)c) ? S_AAFTER : S_ABEFORE;
                if (c == '>' || c == '/')
                    continue;
            } else {
                if (aname_n < sizeof(hs->aname))
                    hs->aname[aname_n++] = lower((char)c);
                else
                    aname_n = 0xFF;     /* too long: unknown */
            }
            break;
        case S_AAFTER:
            if (c == '=') {
                st = S_VBEFORE;
            } else if (!is_space((char)c)) {
                close_cap();
                st = S_ABEFORE;
                continue;
            }
            break;
        case S_VBEFORE:
            if (c == '"' || c == '\'') {
                quote = c;
                st = S_VQ;
            } else if (c == '>') {
                value_end();
                tag_end();
            } else if (!is_space((char)c)) {
                st = S_VU;
                continue;
            }
            break;
        case S_VQ:
            if (!cap_open && !class_on) {
                /* fast path: skip the value */
                while (c != quote) {
                    if (++p == end)
                        return;
                    c = *p;
                }
            }
            if (c == quote) {
                value_end();
                st = S_ABEFORE;
            } else if (c == '&' && cap_open) {
                st = S_ENT;
                st_ret = S_VQ;
                ent_n = 0;
            } else {
                value_char(c);
            }
            break;
        case S_VU:
            if (is_space((char)c) || c == '>') {
                value_end();
                st = S_ABEFORE;
                continue;
            }
            if (c == '&' && cap_open) {
                st = S_ENT;
                st_ret = S_VU;
                ent_n = 0;
            } else {
                value_char(c);
            }
            break;
        case S_ENT:
            if (c == ';') {
                entity_end(1);
                break;
            }
            if (ent_n < 10 && (((c | 32) >= 'a' && (c | 32) <= 'z') || (c >= '0' && c <= '9') ||
                               (c == '#' && !ent_n))) {
                hs->ent[ent_n++] = (char)c;
                break;
            }
            entity_end(0);
            continue;
        case S_BANG:
            if (c == '-') {
                st = S_BANGD;
                break;
            }
            st = S_SKIPTAG;
            continue;
        case S_BANGD:
            if (c == '-') {
                st = S_COMMENT;
                dash = 0;
                break;
            }
            st = S_SKIPTAG;
            continue;
        case S_COMMENT:
            while (c != '-' && c != '>') {
                dash = 0;
                if (++p == end)
                    return;
                c = *p;
            }
            if (c == '-') {
                if (dash < 2)
                    dash++;
            } else if (dash >= 2) {
                st = S_TEXT;
            } else {
                dash = 0;
            }
            break;
        case S_SKIPTAG:
            while (c != '>') {
                if (++p == end)
                    return;
                c = *p;
            }
            st = S_TEXT;
            break;
        case S_RAW:
            if (!raw_cap) {
                while (c != '<') {
                    if (++p == end)
                        return;
                    c = *p;
                }
            }
            if (c == '<') {
                st = S_RAWLT;
            } else if (c == '&') {
                st = S_ENT;
                st_ret = S_RAW;
                ent_n = 0;
            } else {
                text_byte(c);
            }
            break;
        case S_RAWLT:
            if (c == '/') {
                st = S_RAWEND;
                raw_n = 0;
                break;
            }
            if (raw_cap)
                text_byte('<');
            st = S_RAW;
            continue;
        case S_RAWEND:
            if (lower((char)c) == hs->rname[raw_n]) {
                if (++raw_n == raw_len)
                    st = S_RAWENDC;
                break;
            }
            st = S_RAW;
            continue;
        case S_RAWENDC:
            if (c == '>' || c == '/' || is_space((char)c)) {
                raw_end();
                st = c == '>' ? S_TEXT : S_SKIPTAG;
                break;
            }
            st = S_RAW;
            continue;
        case S_PLAIN:
            text_byte(c);
            break;
        }
        p++;
    }
}

