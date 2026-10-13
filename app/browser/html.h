/* html.h - streaming HTML renderer and form store of the web browser
 * (app/browser/html.c).  The page is laid out into lines of HTML_COLS characters in
 * extended RAM while it arrives; links and form fields become numbered
 * items.  Characters with bit 7 set are drawn inverted (item numbers). */
#ifndef TI84P_HTML_H
#define TI84P_HTML_H
#include <stdint.h>

#ifdef HTML_HOST
#define HTML_COLS 24
void xram_read(uint32_t addr, void *dst, uint16_t len);
void xram_write(uint32_t addr, const void *src, uint16_t len);
#define XRAM_BROWSER 0UL
#else
#include "kernel.h"
#define HTML_COLS CON_COLS
#endif

/* extended RAM of the browser (64 KiB) */
#define HX_TEXT     (XRAM_BROWSER)              /* lines of HTML_COLS bytes */
#define HTML_MAX_LINES 1600u                    /* 37.5 KiB */
#define HX_ITEMS    (XRAM_BROWSER + 0x9600UL)   /* struct html_item[] */
#define HTML_MAX_ITEMS 768u                     /* 9 KiB */
#define HX_FORMS    (XRAM_BROWSER + 0xBA00UL)   /* struct html_form[] */
#define HTML_MAX_FORMS 64u
#define HX_HEAP     (XRAM_BROWSER + 0xBB00UL)   /* NUL-terminated strings */
#define HTML_HEAP_SIZE 0x3B00u                  /* 14.75 KiB, up to F600 */
/* F600-FFFF: history and saved state of browser.c */

#define HTML_NONE 0xFFFFu
#define HTML_NOFORM 0xFFu
#define HTML_STR_MAX 255

enum {
    IT_LINK = 1, IT_TEXT, IT_PASSWORD, IT_TEXTAREA, IT_CHECK, IT_RADIO,
    IT_SELECT, IT_SUBMIT, IT_HIDDEN
};
#define ITF_CHECKED 1
#define ITF_NUMBERED 2

struct html_item {
    uint8_t kind, form, width, flags;
    uint8_t sel, nopt;          /* select: chosen option, number of options */
    uint16_t name;              /* heap offset: href of a link, field name */
    uint16_t value;             /* heap offset: value; select: first option */
    uint16_t pos;               /* text offset of the field's value */
};

struct html_form {
    uint16_t action;            /* heap offset, HTML_NONE for the page itself */
    uint8_t post, n_text;       /* method, number of text fields */
};

extern uint16_t html_lines, html_items, html_numbers, html_main_line;
extern uint8_t html_forms, html_full;
extern char html_title[HTML_COLS + 1];

void html_begin(uint8_t is_html);
void html_feed(const uint8_t *p, uint16_t n);
void html_end(void);

void html_get_line(uint16_t n, char *buf);          /* HTML_COLS bytes */
/* item with display number n (1..html_numbers); returns its index or
 * HTML_NONE */
uint16_t html_find_number(uint16_t n, struct html_item *it);
void html_get_item(uint16_t idx, struct html_item *it);
void html_put_item(uint16_t idx, const struct html_item *it);
void html_get_form(uint8_t f, struct html_form *fm);
void html_get_str(uint16_t off, char *buf, uint16_t max);  /* "" for NONE */
uint16_t html_add_str(const char *s);               /* HTML_NONE if full */
/* select option k: value (HTML_STR_MAX + 1 bytes) and label (HTML_COLS + 1
 * bytes, truncated); either may be NULL */
void html_get_option(const struct html_item *it, uint8_t k, char *value, char *label);
/* redraw a field's value in the page text after a change */
void html_show_field(const struct html_item *it);

#endif
