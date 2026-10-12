/* font.c - the external font (see font.h and tools/mkfont84.py) */
#include <string.h>
#include "font.h"
#ifndef HTML_HOST
#include "kernel.h"
#endif

uint8_t font_ok;
static uint16_t n_glyphs;
/* recently found code points (Japanese text repeats many characters) */
#define CACHE 64
static uint16_t cache_cp[CACHE], cache_g[CACHE];
static uint32_t lo_off, wide_off, glyph_off;
static uint8_t cell_w;

/* bytes at offset off of the font, across flash pages */
static void font_read(uint32_t off, void *dst, uint16_t len)
{
    uint8_t *d = dst;
    while (len) {
        uint16_t a = (uint16_t)off & 0x3FFF, n = 0x4000 - a;
        /* page: bits 14-21 of off, with byte operations (no 32-bit shift) */
        uint8_t page = FONT_PAGE + ((uint8_t)((uint16_t)off >> 8) >> 6) +
                       (uint8_t)((uint8_t)(off >> 16) << 2);
        if (n > len)
            n = len;
        far_read(page, 0x4000 + a, d, n);
        off += n;
        d += n;
        len -= n;
    }
}

void font_init(void)
{
    uint8_t h[32], buf[64];
    uint32_t off, end;
    uint16_t sum = 0, want;
    uint8_t i;
    font_ok = 0;
    font_read(0, h, 32);
    if (memcmp(h, "F84", 4) || h[4] != 1 || h[5] != 8)
        return;
    n_glyphs = h[8] | (uint16_t)h[9] << 8;
    want = h[10] | (uint16_t)h[11] << 8;
    memcpy(&lo_off, h + 12, 4);
    memcpy(&wide_off, h + 16, 4);
    memcpy(&glyph_off, h + 20, 4);
    cell_w = h[6];
    /* the pages must have arrived intact */
    end = glyph_off + (uint32_t)n_glyphs * 8;
    for (off = 32; off < end; off += sizeof(buf)) {
        uint16_t n = end - off < sizeof(buf) ? (uint16_t)(end - off) : sizeof(buf);
        font_read(off, buf, n);
        for (i = 0; i < n; i++)
            sum += buf[i];
    }
    font_ok = sum == want;
    memset(cache_cp, 0, sizeof(cache_cp));
}

uint16_t font_find(uint16_t cp)
{
    uint16_t r[2], lo, hi;
    uint8_t want = (uint8_t)cp, b, slot = (uint8_t)(cp ^ (cp >> 6)) & (CACHE - 1);
    if (!font_ok)
        return FONT_NONE;
    if (cache_cp[slot] == cp)
        return cache_g[slot];
    cache_cp[slot] = cp;
    cache_g[slot] = FONT_NONE;
    font_read(32 + 2 * (uint16_t)(cp >> 8), r, 4);
    lo = r[0];
    hi = r[1];
    while (lo < hi) {
        uint16_t mid = (lo + hi) >> 1;
        font_read(lo_off + mid, &b, 1);
        if (b == want)
            return cache_g[slot] = mid;
        if (b < want)
            lo = mid + 1;
        else
            hi = mid;
    }
    return FONT_NONE;
}

uint8_t font_glyph(uint16_t g, uint8_t rows[8])
{
    uint8_t w;
    font_read(glyph_off + (uint32_t)g * 8, rows, 8);
    font_read(wide_off + (g >> 3), &w, 1);
    return w & (1 << (g & 7)) ? cell_w : cell_w / 2;
}
