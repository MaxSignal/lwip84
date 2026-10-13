/* dict.c - the conversion dictionaries (see dict.h and tools/mkdict84.py) */
#include <string.h>
#include "dict.h"
#ifndef DICT_HOST
#include "kernel.h"
#endif

#define GROUP 16                /* readings per index entry */

uint8_t dict_ok;
static uint16_t ja_index_n;
static uint32_t ja_index, ja_data, zh;

/* bytes at offset off of the dictionaries, across flash pages */
static void dict_read(uint32_t off, void *dst, uint16_t len)
{
    uint8_t *d = dst;
    while (len) {
        uint16_t a = (uint16_t)off & 0x3FFF, n = 0x4000 - a;
        uint8_t page = DICT_PAGE + ((uint8_t)((uint16_t)off >> 8) >> 6) +
                       (uint8_t)((uint8_t)(off >> 16) << 2);
        if (n > len)
            n = len;
        far_read(page, 0x4000 + a, d, n);
        off += n;
        d += n;
        len -= n;
    }
}

static uint8_t dict_byte(uint32_t off)
{
    uint8_t b;
    dict_read(off, &b, 1);
    return b;
}

void dict_init(void)
{
    uint8_t h[32];
    dict_read(0, h, 32);
    dict_ok = !memcmp(h, "D84", 4) && h[4] == 1;
    if (!dict_ok)
        return;
    ja_index_n = h[12] | (uint16_t)h[13] << 8;
    memcpy(&ja_index, h + 16, 4);
    memcpy(&ja_data, h + 20, 4);
    memcpy(&zh, h + 24, 4);
}

/* compare a reading with the key: <0, 0, >0 */
static int8_t cmp(const uint8_t *r, uint8_t rn, const uint8_t *key, uint8_t n)
{
    uint8_t i, m = rn < n ? rn : n;
    for (i = 0; i < m; i++) {
        if (r[i] != key[i])
            return r[i] < key[i] ? -1 : 1;
    }
    return rn < n ? -1 : rn > n ? 1 : 0;
}

/* the reading at offset off (from ja_data) after prev[]; returns the
 * offset of its word count */
static uint32_t reading_at(uint32_t off, uint8_t *r, uint8_t *rn)
{
    uint8_t h = dict_byte(ja_data + off), rest = h & 15;
    *rn = (h >> 4) + rest;
    dict_read(ja_data + off + 1, r + (h >> 4), rest);
    return off + 1 + rest;
}

/* past the words of a reading whose count is at off */
static uint32_t skip_words(uint32_t off)
{
    uint8_t n = dict_byte(ja_data + off);
    off++;
    while (n--)
        off += 1 + dict_byte(ja_data + off);
    return off;
}

static uint32_t index_entry(uint16_t i)
{
    uint8_t b[3];
    dict_read(ja_index + (uint32_t)i * 3, b, 3);
    return b[0] | (uint16_t)b[1] << 8 | (uint32_t)b[2] << 16;
}

uint8_t dict_ja_find(const uint8_t *key, uint8_t n, uint32_t *at)
{
    uint8_t r[16], rn, g;
    uint16_t lo = 0, hi, mid;
    uint32_t off;
    int8_t c;
    if (!dict_ok || !n || n > 15 || !ja_index_n)
        return 0;
    /* the last index entry whose reading is not after the key */
    hi = ja_index_n;
    while (hi - lo > 1) {
        mid = (lo + hi) >> 1;
        reading_at(index_entry(mid), r, &rn);
        if (cmp(r, rn, key, n) <= 0)
            lo = mid;
        else
            hi = mid;
    }
    off = index_entry(lo);
    for (g = 0; g < GROUP; g++) {
        if (ja_data + off >= zh)
            return 0;
        off = reading_at(off, r, &rn);
        c = cmp(r, rn, key, n);
        if (!c) {
            *at = ja_data + off;
            return dict_byte(*at);
        }
        if (c > 0)
            return 0;
        off = skip_words(off);
    }
    return 0;
}

static char *put_cp(char *o, uint16_t cp)
{
    if (cp < 0x800) {
        *o++ = (char)(0xC0 | (cp >> 6));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *o++ = (char)(0xE0 | (cp >> 12));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    }
    return o;
}

void dict_ja_word(uint32_t at, uint8_t i, char *out)
{
    uint8_t b[32], n, k;
    at++;
    while (i--)
        at += 1 + dict_byte(at);
    n = dict_byte(at);
    if (n > sizeof(b))
        n = sizeof(b);
    dict_read(at + 1, b, n);
    for (k = 0; k < n; k++) {
        if (b[k] < 0x80) {
            out = put_cp(out, b[k] == 0x57 ? 0x30FC : 0x3040 + b[k]);
        } else if (k + 1 < n) {
            out = put_cp(out, 0x3000 + ((uint16_t)(b[k] & 0x7F) << 8 | b[k + 1]));
            k++;
        }
    }
    *out = 0;
}

uint16_t dict_zh_find(const char *syl, uint8_t n, uint32_t *at)
{
    uint16_t cnt, lo = 0, hi, mid, o;
    uint8_t s[8], sn;
    int8_t c;
    if (!dict_ok || !n || n > 6)
        return 0;
    dict_read(zh, &cnt, 2);
    hi = cnt;
    while (lo < hi) {
        mid = (lo + hi) >> 1;
        dict_read(zh + 2 + 2 * (uint32_t)mid, &o, 2);
        sn = dict_byte(zh + o);
        if (sn > sizeof(s))
            sn = sizeof(s);
        dict_read(zh + o + 1, s, sn);
        c = cmp(s, sn, (const uint8_t *)syl, n);
        if (!c) {
            *at = zh + o + 1 + sn + 2;
            dict_read(zh + o + 1 + sn, &cnt, 2);
            return cnt;
        }
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

uint16_t dict_zh_char(uint32_t at, uint16_t i)
{
    uint16_t cp;
    dict_read(at + 2 * (uint32_t)i, &cp, 2);
    return cp;
}
