/* tidl.c - the TI-OS browser app only: programs and appvars from the web
 * into TI-OS (see tidl.h).
 *
 * A TI file (.8xp, .8xv, .8xg) is kept as it arrives in RAM page 85h.  A
 * ZIP is read as it arrives: the members that are TI files go compressed
 * to the browser's text area (page 84h), everything else is skipped.  At
 * the end, with the connection closed and its memory free again, they are
 * inflated into page 85h (lib/inflate) and checked.  Then
 * the variables in those files are listed to choose from; the chosen ones
 * go into the table platform/tios/launch.s works from after the app. */
#include <string.h>
#include "kernel.h"
#include "input.h"
#include "ui.h"
#include "html.h"
#include "inflate.h"
#include "tidl.h"

extern uint8_t __app_save[];            /* platform/tios/crt0.s */

#define COLS CON_COLS
#define TAB 0x4000UL                    /* page 85h: the table for launch.s */
#define OUT 0x4100UL                    /* the TI files */
#define OUT_END 0x7F00UL
#define NAMES 0x7F00UL                  /* member names, 32 bytes each */
#define TMP 0x0000UL                    /* page 84h: the compressed members */
#define TMP_MAX 0x2C00u
#define FILES 8
#define CANDS 16

static uint8_t zip, bad, nfiles, ncand, cur, top;
static uint16_t n_out;                  /* bytes in OUT */
static uint16_t f_at[FILES], f_len[FILES];   /* in page 85h at 4000h */
static uint8_t f_member[FILES];
static uint8_t left_out;                /* members: too big, damaged, no memory */

/* the members kept in TMP */
struct member {
    uint16_t off, csize, usize;
    uint32_t crc;
    uint8_t method;
};
static struct member members[FILES];
static uint8_t nmembers;
static uint16_t tmp_used;

struct cand {
    uint8_t type, name[8];
    uint16_t len, at;                   /* with the size word; in page 85h */
    uint8_t file, sel;
};
static struct cand cands[CANDS];

/* the ZIP reader */
static uint8_t zst, hb[30], hn, want;
static uint16_t nlen, xlen, method, flags, got;
static uint32_t csize, usize, crc, left;
static char zname[32];

static uint8_t lower(char c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* ".8xp" and the like at the end of s (n chars) */
static uint8_t ext_is(const char *s, uint8_t n, const char *ext)
{
    uint8_t i;
    if (n < 4 || s[n - 4] != '.')
        return 0;
    for (i = 0; i < 3; i++)
        if (lower(s[n - 3 + i]) != ext[i])
            return 0;
    return 1;
}

static uint8_t is_ti(const char *s, uint8_t n)
{
    return ext_is(s, n, "8xp") || ext_is(s, n, "8xv") || ext_is(s, n, "8xg");
}

uint8_t tidl_kind(const char *url)
{
    const char *e = strchr(url, '?');
    uint8_t n = (uint8_t)(e ? e - url : strlen(url));
    if (is_ti(url, n))
        return 1;
    return ext_is(url, n, "zip") ? 2 : 0;
}

void tidl_begin(uint8_t is_zip)
{
    zip = is_zip;
    bad = nfiles = ncand = cur = top = left_out = nmembers = 0;
    n_out = tmp_used = 0;
    zst = hn = 0;
    if (zip) {
        /* the page's text area holds the members: no page any more */
        html_lines = html_items = html_numbers = 0;
        html_main_line = HTML_NONE;
        html_forms = 0;
        html_title[0] = 0;
    }
}

static uint32_t u32(const uint8_t *p)
{
    return p[0] | (uint16_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t u16(const uint8_t *p)
{
    return p[0] | (uint16_t)p[1] << 8;
}

/* member k from TMP into OUT */
static void inflate_member(uint8_t k)
{
    struct member *m = &members[k];
    uint16_t n = 0;
    int8_t r = INFLATE_OK;
    uint32_t dst = OUT + n_out;
    if (m->usize > OUT_END - dst) {
        left_out++;
        return;
    }
    if (m->method == 0) {
        uint8_t b[32];
        uint16_t c;
        for (n = 0; n < m->csize; n += c) {
            c = m->csize - n > sizeof(b) ? sizeof(b) : m->csize - n;
            xram_read(TMP + m->off + n, b, c);
            xram_write(dst + n, b, c);
        }
    } else {
        r = inflate_xram(TMP + m->off, m->csize, dst, m->usize, &n);
    }
    if (r || n != m->usize || crc32_xram(dst, n) != m->crc) {
        left_out++;
        return;
    }
    f_at[nfiles] = (uint16_t)dst;
    f_len[nfiles] = n;
    f_member[nfiles] = k;
    nfiles++;
    n_out += n;
}

static void zip_feed(const uint8_t *p, uint16_t n)
{
    while (n && zst < 4) {
        if (zst == 0) {                 /* local file header */
            hb[hn] = *p;
            hn++;
            p++;
            n--;
            if (hn == 4 && u32(hb) != 0x04034B50UL) {
                zst = 4;                /* the central directory: the end */
                return;
            }
            if (hn < 30)
                continue;
            flags = u16(hb + 6);
            method = u16(hb + 8);
            crc = u32(hb + 14);
            csize = u32(hb + 18);
            usize = u32(hb + 22);
            nlen = u16(hb + 26);
            xlen = u16(hb + 28);
            hn = 0;
            got = 0;
            zst = 1;
            if (flags & 8) {            /* sizes after the data: cannot follow */
                bad = 2;
                zst = 4;
                return;
            }
            continue;
        }
        if (zst == 1) {                 /* the name */
            if (got < sizeof(zname) - 1) {
                zname[got] = (char)*p;
                zname[got + 1] = 0;
            }
            got++;
            p++;
            n--;
            if (got == nlen) {
                uint8_t l = nlen < sizeof(zname) ? (uint8_t)nlen : (uint8_t)(sizeof(zname) - 1);
                want = is_ti(zname, l) && (method == 0 || method == 8);
                if (want && (csize > TMP_MAX - tmp_used || usize > OUT_END - OUT ||
                             nmembers == FILES || usize < 57)) {
                    want = 0;
                    left_out++;
                }
                got = 0;
                zst = 2;
            }
            continue;
        }
        if (zst == 2) {                 /* extra field */
            uint16_t k = xlen - got < n ? xlen - got : n;
            got += k;
            p += k;
            n -= k;
            if (got < xlen)
                continue;
            left = csize;
            got = 0;
            zst = 3;
        }
        if (zst == 3) {                 /* data */
            uint16_t k = left < n ? (uint16_t)left : n;
            if (want)
                xram_write(TMP + tmp_used + got, p, k);
            got += k;
            left -= k;
            p += k;
            n -= k;
            if (left)
                continue;
            if (want) {
                struct member *m = &members[nmembers];
                m->off = tmp_used;
                m->csize = (uint16_t)csize;
                m->usize = (uint16_t)usize;
                m->crc = crc;
                m->method = (uint8_t)method;
                xram_write(NAMES + (uint16_t)nmembers * 32, zname, 32);
                nmembers++;
                tmp_used += (uint16_t)csize;
            }
            zst = 0;
        }
    }
}

void tidl_feed(const uint8_t *p, uint16_t n)
{
    if (zip) {
        zip_feed(p, n);
        return;
    }
    if (n_out + n > OUT_END - OUT) {
        bad = 1;
        return;
    }
    xram_write(OUT + n_out, p, n);
    n_out += n;
}

/* the programs and appvars of TI file k */
static void ti_file(uint8_t k)
{
    uint16_t base = f_at[k];
    uint8_t h[11], e[11], j;
    uint16_t off = 55, end, v;
    xram_read(base, h, 11);
    if (memcmp(h, "**TI83F*", 8))
        return;
    xram_read(base + 53, &v, 2);
    end = 55 + v;
    if (end > f_len[k])
        end = f_len[k];
    while (off + 2 + 13 <= end && ncand < CANDS) {
        uint16_t hl, len, at;
        struct cand *c = &cands[ncand];
        xram_read(base + off, e, 4);
        hl = u16(e);
        len = u16(e + 2);
        if (hl != 11 && hl != 13)
            break;
        at = off + 2 + hl + 2;
        if (at + len > end)
            break;
        xram_read(base + off + 4, e, 9);
        if (e[0] == 0x05 || e[0] == 0x06 || e[0] == 0x15) {
            c->type = e[0];
            memcpy(c->name, e + 1, 8);
            c->len = len;
            c->at = base + at;
            c->file = k;
            /* another version of a variable already chosen: not this one */
            c->sel = 1;
            for (j = 0; j < ncand; j++)
                if (cands[j].sel && cands[j].type == c->type && !memcmp(cands[j].name, c->name, 8))
                    c->sel = 0;
            ncand++;
        }
        off = at + len;
    }
}

void tidl_end(void)
{
    uint8_t k;
    if (!zip) {
        f_at[0] = (uint16_t)OUT;
        f_len[0] = n_out;
        nfiles = bad ? 0 : 1;
    }
    for (k = 0; k < nmembers; k++)
        inflate_member(k);
    for (k = 0; k < nfiles; k++)
        ti_file(k);
}

/* TI-OS's free RAM when the app started: OPS - FPS in its saved page 81h
 * (RAM page 87h) */
static uint16_t tios_free(void)
{
    uint16_t fps, ops;
    xram_read(0xC000UL + 0x1824, &fps, 2);
    xram_read(0xC000UL + 0x1828, &ops, 2);
    return ops > fps ? ops - fps : 0;
}

static uint16_t need(void)
{
    uint16_t s = 0;
    uint8_t i;
    for (i = 0; i < ncand; i++)
        if (cands[i].sel)
            s += cands[i].len + 24;
    return s;
}

void tidl_draw(const char *url)
{
    char b[COLS + 1];
    uint8_t i, k;
    const char *name = strrchr(url, '/');
    ui_clear();
    if (left_out)
        snprintf(b, sizeof(b), "%u left out: %s", (unsigned)left_out, name ? name + 1 : url);
    else
        snprintf(b, sizeof(b), "%s", name ? name + 1 : url);
    ui_status(b);
    if (!ncand) {
        ui_text(1, bad == 1 ? "too big: 15872 bytes at" :
                   bad == 2 ? "this ZIP stores sizes" : "no program or appvar in");
        ui_text(2, bad == 1 ? "most" : bad == 2 ? "after the data: cannot" : "it that fits");
        if (bad == 2)
            ui_text(3, "read it here");
        if (left_out)
            ui_text(5, "(too big or damaged)");
        ui_text(8, "CLEAR: back");
        lcd_update();
        return;
    }
    if (cur < top)
        top = cur;
    else if (cur >= top + 5)
        top = cur - 4;
    for (i = 0; i < 5 && top + i < ncand; i++) {
        struct cand *c = &cands[top + i];
        memcpy(b, c->sel ? "[x] " : "[ ] ", 4);
        memcpy(b + 4, c->type == 0x15 ? "appv " : c->type == 0x06 ? "prg* " : "prgm ", 5);
        for (k = 0; k < 8 && c->name[k]; k++)
            b[9 + k] = c->name[k] >= 'A' && c->name[k] <= 'Z' ? c->name[k] :
                       c->name[k] >= '0' && c->name[k] <= '9' ? c->name[k] :
                       c->name[k] == 0x5B ? '@' : '?';
        snprintf(b + 9 + k, sizeof(b) - 9 - k, " %u", (unsigned)(c->len - 2));
        ui_text(i, b);
        if (top + i == cur)
            lcd_invert_rect(0, 8 + i * 6, 96, 6);
    }
    if (zip) {                          /* where the one at the cursor is from */
        char z[32];
        xram_read(NAMES + (uint16_t)f_member[cands[cur].file] * 32, z, 32);
        z[COLS] = 0;
        ui_text(5, z);
    }
    snprintf(b, sizeof(b), "need %u of %u free", need(), tios_free());
    ui_text(6, b);
    ui_text(7, need() <= tios_free() ? "ENTER: into TI-OS (ends)" : "not enough RAM in TI-OS");
    ui_text(8, "0: choose  CLEAR: back");
    lcd_update();
}

int8_t tidl_key(uint8_t k)
{
    uint8_t i, n = 0;
    if (k == K_CLEAR || k == K_MODE)
        return TIDL_BACK;
    if (!ncand)
        return k == K_ENTER ? TIDL_BACK : TIDL_STAY;
    if (k == K_UP && cur)
        cur--;
    else if (k == K_DOWN && cur + 1 < ncand)
        cur++;
    else if (k == K_0)
        cands[cur].sel ^= 1;
    else if (k == K_ENTER && need() <= tios_free()) {
        /* the table for launch.s: count, then type, name, length, where */
        for (i = 0; i < ncand; i++) {
            if (!cands[i].sel)
                continue;
            xram_write(TAB + 1 + (uint16_t)n * 13, &cands[i], 13);
            n++;
        }
        if (!n)
            return TIDL_STAY;
        xram_write(TAB, &n, 1);
        __app_save[10] = n;
        return TIDL_INSTALL;
    }
    return TIDL_STAY;
}
