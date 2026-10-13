/* inflate.c - Deflate decompression from extended RAM to extended RAM (see
 * inflate.h).  The decoder follows tinf (Joergen Ibsen, zlib license):
 * canonical Huffman trees kept as code counts per length and symbols in
 * code order, decoded a bit at a time.
 *
 * Altered from tinf, Copyright (c) 2003-2019 Joergen Ibsen:
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 *   1. The origin of this software must not be misrepresented; you must
 *      not claim that you wrote the original software. If you use this
 *      software in a product, an acknowledgment in the product
 *      documentation would be appreciated but is not required.
 *
 *   2. Altered source versions must be plainly marked as such, and must
 *      not be misrepresented as being the original software.
 *
 *   3. This notice may not be removed or altered from any source
 *      distribution. */
#include <string.h>
#include <stdlib.h>
#include "inflate.h"
#ifdef INFLATE_HOST
void xram_read(uint32_t addr, void *dst, uint16_t len);
void xram_write(uint32_t addr, const void *src, uint16_t len);
#else
#include "kernel.h"
#endif

struct tree {
    uint16_t counts[16];        /* codes of each length */
    uint16_t symbols[288];      /* symbols in code order */
};

struct state {
    struct tree lt, dt;
    uint8_t lengths[288 + 32];
    /* input */
    uint32_t src;
    uint16_t left;              /* bytes not yet in ibuf */
    uint8_t ibuf[32], ipos, ilen;
    uint8_t tag, bits;          /* bits of the current byte, how many */
    uint8_t bad;
    /* output */
    uint32_t dst;
    uint16_t pos, max;          /* written (with obuf), limit */
    uint8_t obuf[32], on;
};

static const uint8_t len_bits_bk[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t len_base_bk[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99,
    115, 131, 163, 195, 227, 258};
static const uint8_t dist_bits_bk[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12,
    13, 13};
static const uint16_t dist_base_bk[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
    1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t clc_order_bk[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

static uint8_t next_byte(struct state *s)
{
    uint8_t b;
    if (s->ipos == s->ilen) {
        uint8_t n = s->left > sizeof(s->ibuf) ? sizeof(s->ibuf) : (uint8_t)s->left;
        if (!n) {
            s->bad = 1;
            return 0;
        }
        xram_read(s->src, s->ibuf, n);
        s->src += n;
        s->left -= n;
        s->ilen = n;
        s->ipos = 0;
    }
    b = s->ibuf[s->ipos];
    s->ipos++;
    return b;
}

static uint8_t get_bit(struct state *s)
{
    uint8_t b;
    if (!s->bits) {
        s->tag = next_byte(s);
        s->bits = 8;
    }
    b = s->tag & 1;
    s->tag >>= 1;
    s->bits--;
    return b;
}

static uint16_t get_bits(struct state *s, uint8_t n)
{
    uint16_t v = 0, m = 1;
    while (n--) {
        if (get_bit(s))
            v |= m;
        m <<= 1;
    }
    return v;
}

/* returns 0 if the lengths do not make a valid code */
static uint8_t build(struct tree *t, const uint8_t *lengths, uint16_t num)
{
    uint16_t offs[16], i, codes = 0, used, max_sym = 0;
    int32_t avail = 1;
    memset(t->counts, 0, sizeof(t->counts));
    for (i = 0; i < num; i++) {
        if (lengths[i]) {
            max_sym = i;
            t->counts[lengths[i]]++;
        }
    }
    for (i = 0; i < 16; i++) {          /* counts[0] is 0: avail 2 for length 1 */
        used = t->counts[i];
        if (used > avail)
            return 0;
        avail = 2 * (avail - used);
        offs[i] = codes;
        codes += used;
    }
    if ((codes > 1 && avail > 0) || (codes == 1 && t->counts[1] != 1))
        return 0;
    for (i = 0; i < num; i++) {
        uint8_t l = lengths[i];
        if (l) {
            uint16_t o = offs[l];
            t->symbols[o] = i;
            offs[l] = o + 1;
        }
    }
    if (codes == 1) {           /* one code: also the other one of length 1 */
        t->counts[1] = 2;
        t->symbols[1] = max_sym + 1;
    }
    return 1;
}

static uint16_t decode(struct state *s, const struct tree *t)
{
    uint16_t base = 0, offs = 0;
    uint8_t len;
    for (len = 1; len < 16; len++) {
        offs = 2 * offs + get_bit(s);
        if (offs < t->counts[len])
            return t->symbols[base + offs];
        base += t->counts[len];
        offs -= t->counts[len];
    }
    s->bad = 1;
    return 0;
}

static void flush(struct state *s)
{
    if (s->on) {
        xram_write(s->dst + (s->pos - s->on), s->obuf, s->on);
        s->on = 0;
    }
}

static int8_t put(struct state *s, uint8_t b)
{
    if (s->pos >= s->max)
        return INFLATE_FULL;
    s->obuf[s->on] = b;
    s->on++;
    s->pos++;
    if (s->on == sizeof(s->obuf))
        flush(s);
    return INFLATE_OK;
}

/* len bytes from dist back: in pieces no longer than dist, so that each
 * piece reads only what is written already */
static int8_t copy(struct state *s, uint16_t len, uint16_t dist)
{
    uint8_t tmp[32], n;
    if (!dist || dist > s->pos)
        return INFLATE_BAD;
    if (len > s->max - s->pos)
        return INFLATE_FULL;
    flush(s);
    while (len) {
        n = len > sizeof(tmp) ? sizeof(tmp) : (uint8_t)len;
        if (n > dist)
            n = (uint8_t)dist;
        xram_read(s->dst + (s->pos - dist), tmp, n);
        xram_write(s->dst + s->pos, tmp, n);
        s->pos += n;
        len -= n;
    }
    return INFLATE_OK;
}

static int8_t block_data(struct state *s)
{
    for (;;) {
        uint16_t sym = decode(s, &s->lt), len, dist;
        int8_t r;
        if (s->bad)
            return INFLATE_BAD;
        if (sym < 256) {
            r = put(s, (uint8_t)sym);
            if (r)
                return r;
            continue;
        }
        if (sym == 256)
            return INFLATE_OK;
        sym -= 257;
        if (sym >= 29)
            return INFLATE_BAD;
        len = len_base_bk[sym] + get_bits(s, len_bits_bk[sym]);
        sym = decode(s, &s->dt);
        if (sym >= 30 || s->bad)
            return INFLATE_BAD;
        dist = dist_base_bk[sym] + get_bits(s, dist_bits_bk[sym]);
        r = copy(s, len, dist);
        if (r)
            return r;
    }
}

static int8_t stored(struct state *s)
{
    uint16_t len, nlen;
    int8_t r;
    s->bits = 0;                        /* to the byte boundary */
    len = next_byte(s);
    len |= (uint16_t)next_byte(s) << 8;
    nlen = next_byte(s);
    nlen |= (uint16_t)next_byte(s) << 8;
    if (len != (uint16_t)~nlen)
        return INFLATE_BAD;
    while (len--) {
        uint8_t b = next_byte(s);
        if (s->bad)
            return INFLATE_BAD;
        r = put(s, b);
        if (r)
            return r;
    }
    return INFLATE_OK;
}

static int8_t fixed(struct state *s)
{
    uint16_t i;
    for (i = 0; i < 288; i++)
        s->lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    build(&s->lt, s->lengths, 288);
    /* 32 distance codes of 5 bits (30 and 31 are never used): a complete
     * code, which 30 lengths alone would not be */
    memset(s->lengths, 5, 32);
    build(&s->dt, s->lengths, 32);
    return block_data(s);
}

static int8_t dynamic(struct state *s)
{
    uint16_t hlit = get_bits(s, 5) + 257, hdist = get_bits(s, 5) + 1, i, num;
    uint8_t hclen = (uint8_t)get_bits(s, 4) + 4, k;
    if (hlit > 286 || hdist > 30)
        return INFLATE_BAD;
    memset(s->lengths, 0, 19);
    for (k = 0; k < hclen; k++)
        s->lengths[clc_order_bk[k]] = (uint8_t)get_bits(s, 3);
    /* the code length code, in the literal tree for now */
    if (!build(&s->lt, s->lengths, 19))
        return INFLATE_BAD;
    num = hlit + hdist;
    for (i = 0; i < num; ) {
        uint16_t sym = decode(s, &s->lt), rep;
        uint8_t v = 0;
        if (s->bad)
            return INFLATE_BAD;
        if (sym < 16) {
            s->lengths[i] = (uint8_t)sym;
            i++;
            continue;
        }
        if (sym == 16) {
            if (!i)
                return INFLATE_BAD;
            v = s->lengths[i - 1];
            rep = 3 + get_bits(s, 2);
        } else if (sym == 17) {
            rep = 3 + get_bits(s, 3);
        } else {
            rep = 11 + get_bits(s, 7);
        }
        if (i + rep > num)
            return INFLATE_BAD;
        while (rep--) {
            s->lengths[i] = v;
            i++;
        }
    }
    if (!s->lengths[256])
        return INFLATE_BAD;
    if (!build(&s->lt, s->lengths, hlit) || !build(&s->dt, s->lengths + hlit, hdist))
        return INFLATE_BAD;
    return block_data(s);
}

int8_t inflate_xram(uint32_t src, uint16_t src_len, uint32_t dst, uint16_t dst_max,
                    uint16_t *out_len)
{
    struct state *s = calloc(1, sizeof(*s));
    uint8_t last;
    int8_t r = INFLATE_OK;
    *out_len = 0;
    if (!s)
        return INFLATE_NOMEM;
    s->src = src;
    s->left = src_len;
    s->dst = dst;
    s->max = dst_max;
    do {
        uint8_t type;
        last = get_bit(s);
        type = (uint8_t)get_bits(s, 2);
        if (type == 0)
            r = stored(s);
        else if (type == 1)
            r = fixed(s);
        else if (type == 2)
            r = dynamic(s);
        else
            r = INFLATE_BAD;
        if (s->bad && !r)
            r = INFLATE_BAD;
    } while (!r && !last);
    flush(s);
    *out_len = s->pos;
    free(s);
    return r;
}

/* the CRC-32 of ZIP, four bits at a time */
static const uint32_t crc_nibble_bk[16] = {
    0x00000000UL, 0x1DB71064UL, 0x3B6E20C8UL, 0x26D930ACUL, 0x76DC4190UL, 0x6B6B51F4UL,
    0x4DB26158UL, 0x5005713CUL, 0xEDB88320UL, 0xF00F9344UL, 0xD6D6A3E8UL, 0xCB61B38CUL,
    0x9B64C2B0UL, 0x86D3D2D4UL, 0xA00AE278UL, 0xBDBDF21CUL};

uint32_t crc32_xram(uint32_t a, uint16_t n)
{
    uint8_t buf[32], i, k;
    uint32_t c = 0xFFFFFFFFUL;
    while (n) {
        k = n > sizeof(buf) ? sizeof(buf) : (uint8_t)n;
        xram_read(a, buf, k);
        for (i = 0; i < k; i++) {
            c ^= buf[i];
            c = (c >> 4) ^ crc_nibble_bk[(uint8_t)c & 15];
            c = (c >> 4) ^ crc_nibble_bk[(uint8_t)c & 15];
        }
        a += k;
        n -= k;
    }
    return ~c;
}
