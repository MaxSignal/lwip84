/* html_test.c - run the browser's HTML renderer (os/html.c) on the host.
 *
 *   gcc -O2 -DHTML_HOST -I os -o html_test tests/html_test.c os/html.c os/font.c
 *   html_test [-c CHUNK] [-i] [-a] [-f FONT.bin] FILE.html
 *
 * Prints the title and the page as it appears on the calculator (item
 * numbers inverted on a terminal), -i adds the list of links and fields,
 * -c feeds the page in pieces of CHUNK bytes (0: random sizes) to check
 * that the result does not depend on how the page arrives.  -a prints the
 * page without terminal escapes.  -f uses a font made by tools/mkfont84.py
 * (font glyphs are printed as UTF-8).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "html.h"
#include "font.h"

static uint8_t xram[0x10000];
static uint8_t *font_blob;
static long font_len;
static uint16_t glyph_cp[16575];

void far_read(uint8_t page, uint16_t addr, void *dst, uint16_t len)
{
    long off = (long)(page - FONT_PAGE) * 0x4000 + (addr - 0x4000);
    memset(dst, 0xFF, len);
    if (font_blob && off >= 0 && off + len <= font_len)
        memcpy(dst, font_blob + off, len);
}

static void font_load(const char *fn)
{
    FILE *f = fopen(fn, "rb");
    uint32_t lo;
    int h, k;
    if (!f) {
        perror(fn);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    font_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    font_blob = malloc(font_len);
    if (fread(font_blob, 1, font_len, f) != (size_t)font_len)
        exit(2);
    fclose(f);
    font_init();
    if (!font_ok) {
        fprintf(stderr, "%s: bad font\n", fn);
        exit(2);
    }
    /* glyph -> code point, for printing */
    memcpy(&lo, font_blob + 12, 4);
    for (h = 0; h < 256; h++) {
        int a = font_blob[32 + 2 * h] | font_blob[33 + 2 * h] << 8;
        int b = font_blob[34 + 2 * h] | font_blob[35 + 2 * h] << 8;
        for (k = a; k < b && k < 16575; k++)
            glyph_cp[k] = (uint16_t)(h << 8 | font_blob[lo + k]);
    }
}

static void put_utf8(unsigned cp)
{
    if (cp < 0x80)
        putchar(cp);
    else if (cp < 0x800)
        printf("%c%c", 0xC0 | cp >> 6, 0x80 | (cp & 0x3F));
    else
        printf("%c%c%c", 0xE0 | cp >> 12, 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F));
}

void xram_read(uint32_t addr, void *dst, uint16_t len)
{
    if (addr + len > sizeof(xram)) {
        fprintf(stderr, "xram_read out of range %x+%u\n", addr, len);
        exit(2);
    }
    memcpy(dst, xram + addr, len);
}

void xram_write(uint32_t addr, const void *src, uint16_t len)
{
    if (addr + len > 0xF600) {
        fprintf(stderr, "xram_write out of range %x+%u\n", addr, len);
        exit(2);
    }
    memcpy(xram + addr, src, len);
}

static const char *kinds[] = {"?", "link", "text", "password", "textarea", "checkbox",
                              "radio", "select", "submit", "hidden"};

int main(int argc, char **argv)
{
    int chunk = 32768, items = 0, ascii = 0, i;
    const char *file = NULL;
    FILE *f;
    uint8_t *data;
    long n, pos = 0;
    char buf[HTML_COLS + 1];
    clock_t t0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc)
            chunk = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-i"))
            items = 1;
        else if (!strcmp(argv[i], "-a"))
            ascii = 1;
        else if (!strcmp(argv[i], "-f") && i + 1 < argc)
            font_load(argv[++i]);
        else
            file = argv[i];
    }
    if (!file || !(f = fopen(file, "rb"))) {
        fprintf(stderr, "usage: html_test [-c CHUNK] [-i] [-a] FILE\n");
        return 2;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(n);
    if (fread(data, 1, n, f) != (size_t)n)
        return 2;
    fclose(f);
    srand(1);
    t0 = clock();
    html_begin(1);
    while (pos < n) {
        long k = chunk ? chunk : 1 + rand() % 97;
        if (k > n - pos)
            k = n - pos;
        html_feed(data + pos, (uint16_t)k);
        pos += k;
        if (html_full)
            break;
    }
    html_end();
    fprintf(stderr, "%ld bytes, %u lines, %u items (%u numbered), %u forms%s, %.1f ms\n",
            n, html_lines, html_items, html_numbers, html_forms, html_full ? ", full" : "",
            (clock() - t0) * 1000.0 / CLOCKS_PER_SEC);
    printf("title: ");
    for (i = 0; html_title[i]; i++) {
        if (FONT_IS_LEAD(html_title[i]) && html_title[i + 1]) {
            put_utf8(glyph_cp[FONT_GLYPH(html_title[i], html_title[i + 1])]);
            i++;
        } else {
            putchar(html_title[i]);
        }
    }
    printf("\nmain: %d\n", html_main_line == HTML_NONE ? -1 : html_main_line);
    for (i = 0; i < html_lines; i++) {
        int c, inv = 0;
        html_get_line(i, buf);
        for (c = 0; c < HTML_COLS; c++) {
            int ch = (uint8_t)buf[c];
            if (FONT_IS_LEAD(ch) && c + 1 < HTML_COLS) {
                put_utf8(glyph_cp[FONT_GLYPH(ch, buf[c + 1])]);
                c++;
                continue;
            }
            if (!ascii && (ch & 0x80) != inv) {
                fputs(ch & 0x80 ? "\033[7m" : "\033[0m", stdout);
                inv = ch & 0x80;
            }
            putchar(ascii && (ch & 0x80) ? (ch & 0x7F) : ch & 0x7F);
        }
        if (inv)
            fputs("\033[0m", stdout);
        printf("|\n");
    }
    if (items) {
        struct html_item it;
        struct html_form fm;
        char s1[HTML_STR_MAX + 1], s2[HTML_STR_MAX + 1];
        int num = 0;
        for (i = 0; i < html_forms; i++) {
            html_get_form(i, &fm);
            html_get_str(fm.action, s1, sizeof(s1));
            printf("form %d: %s %s, %u text fields\n", i, fm.post ? "POST" : "GET", s1, fm.n_text);
        }
        for (i = 0; i < html_items; i++) {
            html_get_item(i, &it);
            html_get_str(it.name, s1, sizeof(s1));
            html_get_str(it.value, s2, sizeof(s2));
            if (it.flags & ITF_NUMBERED)
                printf("%4d ", ++num);
            else
                printf("     ");
            printf("%-8s", kinds[it.kind]);
            if (it.form != HTML_NOFORM)
                printf(" form%u", it.form);
            if (it.kind == IT_SELECT) {
                int k;
                printf(" %s sel=%u:", s1, it.sel);
                for (k = 0; k < it.nopt && k < 4; k++) {
                    html_get_option(&it, k, s1, s2);
                    printf(" [%s|%s]", s1, s2);
                }
                printf("%s\n", it.nopt > 4 ? " ..." : "");
            } else {
                printf(" %s%s%s%s\n", s1, it.kind == IT_LINK ? "" : " = ", s2,
                       it.flags & ITF_CHECKED ? " (checked)" : "");
            }
        }
    }
    return 0;
}
