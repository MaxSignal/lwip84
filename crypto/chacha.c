/* chacha.c - ChaCha20 and Poly1305 (RFC 8439) */
#include <string.h>
#include "chacha.h"

static uint32_t ld32(const uint8_t *p)
{
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint16_t)p[1] << 8 | p[0];
}

#ifndef CHACHA_ASM
#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define QR(a, b, c, d) \
    x[a] += x[b]; x[d] ^= x[a]; x[d] = ROTL(x[d], 16); \
    x[c] += x[d]; x[b] ^= x[c]; x[b] = ROTL(x[b], 12); \
    x[a] += x[b]; x[d] ^= x[a]; x[d] = ROTL(x[d], 8);  \
    x[c] += x[d]; x[b] ^= x[c]; x[b] = ROTL(x[b], 7)

void chacha_block(const uint32_t in[16], uint8_t out[64])
{
    uint32_t x[16];
    uint8_t i;
    memcpy(x, in, 64);
    for (i = 0; i < 10; i++) {
        QR(0, 4, 8, 12); QR(1, 5, 9, 13); QR(2, 6, 10, 14); QR(3, 7, 11, 15);
        QR(0, 5, 10, 15); QR(1, 6, 11, 12); QR(2, 7, 8, 13); QR(3, 4, 9, 14);
    }
    for (i = 0; i < 16; i++) {
        uint32_t v = x[i] + in[i];
        out[4 * i] = (uint8_t)v;
        out[4 * i + 1] = (uint8_t)(v >> 8);
        out[4 * i + 2] = (uint8_t)(v >> 16);
        out[4 * i + 3] = (uint8_t)(v >> 24);
    }
}
#endif

void chacha_init(chacha_ctx *c, const uint8_t key[32], const uint8_t *nonce,
                 uint8_t nonce_len, uint32_t counter)
{
    uint8_t i;
    c->s[0] = 0x61707865UL;
    c->s[1] = 0x3320646eUL;
    c->s[2] = 0x79622d32UL;
    c->s[3] = 0x6b206574UL;
    for (i = 0; i < 8; i++)
        c->s[4 + i] = ld32(key + 4 * i);
    c->s[12] = counter;
    if (nonce_len == 8) {
        c->s[13] = 0;
        c->s[14] = ld32(nonce);
        c->s[15] = ld32(nonce + 4);
    } else {
        c->s[13] = ld32(nonce);
        c->s[14] = ld32(nonce + 4);
        c->s[15] = ld32(nonce + 8);
    }
    c->pos = 64;
}

void chacha_xor(chacha_ctx *c, uint8_t *data, size_t len)
{
    while (len) {
        uint8_t k;
        const uint8_t *ks;
        if (c->pos == 64) {
            chacha_block(c->s, c->ks);
            c->s[12]++;
            c->pos = 0;
        }
        k = 64 - c->pos;
        if (k > len)
            k = (uint8_t)len;
        ks = c->ks + c->pos;
        c->pos += k;
        len -= k;
        while (k--)
            *data++ ^= *ks++;
    }
}

/* Poly1305 with 17 byte limbs (as in TweetNaCl), products are 8x8 bits */
static void add1305(uint8_t *h, const uint8_t *c)
{
    uint16_t u = 0;
    uint8_t j;
    for (j = 0; j < 17; j++) {
        u += (uint16_t)h[j] + c[j];
        h[j] = (uint8_t)u;
        u >>= 8;
    }
}

static void block1305(poly1305_ctx *c, const uint8_t *m, uint8_t hibit)
{
    uint8_t blk[17], i, j;
    uint32_t x[17], u;
    memcpy(blk, m, 16);
    blk[16] = hibit;
    add1305(c->h, blk);
    for (i = 0; i < 17; i++) {
        uint32_t lo = 0, hi = 0;
        for (j = 0; j <= i; j++)
            lo += (uint16_t)c->h[j] * c->r[i - j];
        for (j = i + 1; j < 17; j++)
            hi += (uint16_t)c->h[j] * c->r[i + 17 - j];
        x[i] = lo + 320 * hi;           /* 2^136 = 2^130 * 64 = 5 * 64 mod p */
    }
    u = 0;
    for (j = 0; j < 16; j++) {
        u += x[j];
        c->h[j] = (uint8_t)u;
        u >>= 8;
    }
    u += x[16];
    c->h[16] = (uint8_t)(u & 3);
    u = 5 * (u >> 2);
    for (j = 0; j < 16; j++) {
        u += c->h[j];
        c->h[j] = (uint8_t)u;
        u >>= 8;
    }
    c->h[16] += (uint8_t)u;
}

void poly1305_init(poly1305_ctx *c, const uint8_t key[32])
{
    memcpy(c->r, key, 16);
    c->r[3] &= 15; c->r[7] &= 15; c->r[11] &= 15; c->r[15] &= 15;
    c->r[4] &= 252; c->r[8] &= 252; c->r[12] &= 252;
    c->r[16] = 0;
    memset(c->h, 0, 17);
    memcpy(c->pad, key + 16, 16);
    c->n = 0;
}

void poly1305_update(poly1305_ctx *c, const uint8_t *data, size_t len)
{
    while (len) {
        uint8_t k = 16 - c->n;
        if (k > len)
            k = (uint8_t)len;
        memcpy(c->buf + c->n, data, k);
        c->n += k;
        data += k;
        len -= k;
        if (c->n == 16) {
            block1305(c, c->buf, 1);
            c->n = 0;
        }
    }
}

void poly1305_final(poly1305_ctx *c, uint8_t tag[16])
{
    static const uint8_t minusp[17] = {5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 252};
    uint8_t g[17], j;
    if (c->n) {
        c->buf[c->n] = 1;
        memset(c->buf + c->n + 1, 0, 15 - c->n);
        block1305(c, c->buf, 0);
    }
    memcpy(g, c->h, 17);
    add1305(g, minusp);                  /* h - p */
    if (g[16] & 0x80)
        memcpy(g, c->h, 17);             /* h < p */
    {
        uint16_t u = 0;
        for (j = 0; j < 16; j++) {
            u += (uint16_t)g[j] + c->pad[j];
            tag[j] = (uint8_t)u;
            u >>= 8;
        }
    }
}
