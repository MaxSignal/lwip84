/* sha256.c - SHA-256 (FIPS 180-4), HMAC (RFC 2104), HKDF (RFC 5869, 8446) */
#include <string.h>
#include "sha256.h"

#if !defined(FE_ASM) || defined(SHA_C)
static const uint32_t K_bk[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL, 0x3956c25bUL, 0x59f111f1UL,
    0x923f82a4UL, 0xab1c5ed5UL, 0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL, 0xe49b69c1UL, 0xefbe4786UL,
    0x0fc19dc6UL, 0x240ca1ccUL, 0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL, 0xc6e00bf3UL, 0xd5a79147UL,
    0x06ca6351UL, 0x14292967UL, 0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL, 0xa2bfe8a1UL, 0xa81a664bUL,
    0xc24b8b70UL, 0xc76c51a3UL, 0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL, 0x391c0cb3UL, 0x4ed8aa4aUL,
    0x5b9cca4fUL, 0x682e6ff3UL, 0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL};
#endif

static const uint32_t H0_bk[8] = {0x6a09e667UL, 0xbb67ae85UL, 0x3c6ef372UL, 0xa54ff53aUL,
                               0x510e527fUL, 0x9b05688cUL, 0x1f83d9abUL, 0x5be0cd19UL};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

#if defined(FE_ASM) && !defined(SHA_C)
/* Z80 assembly (tools/gen_sha256_z80.py) on the fixed crypto page */
uint32_t *sha_h;
const uint8_t *sha_p;
void sha256_block_core(void);

static void compress(uint32_t *h, const uint8_t *p)
{
    sha_h = h;
    sha_p = p;
    sha256_block_core();
}
#else
static void compress(uint32_t *h, const uint8_t *p)
{
    uint32_t w[16], a, b, c, d, e, f, g, hh, t1, t2;
    uint8_t i;
    for (i = 0; i < 16; i++, p += 4)
        w[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint16_t)p[2] << 8 | p[3];
    a = h[0]; b = h[1]; c = h[2]; d = h[3];
    e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (i = 0; i < 64; i++) {
        uint32_t wi;
        if (i < 16) {
            wi = w[i];
        } else {
            uint32_t w1 = w[(i + 1) & 15], w14 = w[(i + 14) & 15];
            uint32_t s0 = ROTR(w1, 7) ^ ROTR(w1, 18) ^ (w1 >> 3);
            uint32_t s1 = ROTR(w14, 17) ^ ROTR(w14, 19) ^ (w14 >> 10);
            wi = w[i & 15] += s0 + s1 + w[(i + 9) & 15];
        }
        t1 = hh + (ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25)) + ((e & f) ^ (~e & g)) + K_bk[i] + wi;
        t2 = (ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
#endif

void sha256_init(sha256_ctx *c)
{
    memcpy(c->h, H0_bk, sizeof(H0_bk));
    c->len_lo = c->len_hi = 0;
    c->n = 0;
}

void sha256_update(sha256_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t old = c->len_lo;
    c->len_lo += len;
    if (c->len_lo < old)
        c->len_hi++;
    while (len) {
        uint8_t k = 64 - c->n;
        if (k > len)
            k = (uint8_t)len;
        if (c->n == 0 && k == 64) {
            compress(c->h, p);
        } else {
            memcpy(c->buf + c->n, p, k);
            c->n += k;
            if (c->n == 64) {
                compress(c->h, c->buf);
                c->n = 0;
            }
        }
        p += k;
        len -= k;
    }
}

void sha256_final(sha256_ctx *c, uint8_t out[32])
{
    uint32_t hi = c->len_hi << 3 | c->len_lo >> 29, lo = c->len_lo << 3;
    uint8_t i;
    c->buf[c->n++] = 0x80;
    if (c->n > 56) {
        memset(c->buf + c->n, 0, 64 - c->n);
        compress(c->h, c->buf);
        c->n = 0;
    }
    memset(c->buf + c->n, 0, 56 - c->n);
    for (i = 0; i < 4; i++) {
        c->buf[56 + i] = (uint8_t)(hi >> (24 - 8 * i));
        c->buf[60 + i] = (uint8_t)(lo >> (24 - 8 * i));
    }
    compress(c->h, c->buf);
    for (i = 0; i < 8; i++) {
        uint32_t v = c->h[i];
        out[4 * i] = (uint8_t)(v >> 24);
        out[4 * i + 1] = (uint8_t)(v >> 16);
        out[4 * i + 2] = (uint8_t)(v >> 8);
        out[4 * i + 3] = (uint8_t)v;
    }
}

void sha256(const void *data, size_t len, uint8_t out[32])
{
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

void hmac_sha256_init(hmac_sha256_ctx *c, const uint8_t *key, size_t keylen)
{
    uint8_t k[64], i;
    memset(k, 0, 64);
    if (keylen > 64)
        sha256(key, keylen, k);
    else
        memcpy(k, key, keylen);
    for (i = 0; i < 64; i++) {
        c->okey[i] = k[i] ^ 0x5c;
        k[i] ^= 0x36;
    }
    sha256_init(&c->inner);
    sha256_update(&c->inner, k, 64);
}

void hmac_sha256_update(hmac_sha256_ctx *c, const void *data, size_t len)
{
    sha256_update(&c->inner, data, len);
}

void hmac_sha256_final(hmac_sha256_ctx *c, uint8_t out[32])
{
    uint8_t ih[32];
    sha256_final(&c->inner, ih);
    sha256_init(&c->inner);
    sha256_update(&c->inner, c->okey, 64);
    sha256_update(&c->inner, ih, 32);
    sha256_final(&c->inner, out);
}

void hmac_sha256(const uint8_t *key, size_t keylen, const void *data, size_t len,
                 uint8_t out[32])
{
    hmac_sha256_ctx c;
    hmac_sha256_init(&c, key, keylen);
    hmac_sha256_update(&c, data, len);
    hmac_sha256_final(&c, out);
}

void hkdf_extract(const uint8_t *salt, const uint8_t *ikm, uint8_t out[32])
{
    hmac_sha256(salt, 32, ikm, 32, out);
}

void hkdf_expand_label(const uint8_t secret[32], const char *label,
                       const uint8_t *context, uint8_t ctx_len,
                       uint8_t *out, uint8_t out_len)
{
    hmac_sha256_ctx c;
    uint8_t hdr[4], t[32];
    uint8_t ll = (uint8_t)strlen(label);
    hdr[0] = 0;
    hdr[1] = out_len;
    hdr[2] = 6 + ll;
    hmac_sha256_init(&c, secret, 32);
    hmac_sha256_update(&c, hdr, 3);
    hmac_sha256_update(&c, "tls13 ", 6);
    hmac_sha256_update(&c, label, ll);
    hmac_sha256_update(&c, &ctx_len, 1);
    if (ctx_len)
        hmac_sha256_update(&c, context, ctx_len);
    hdr[3] = 1;
    hmac_sha256_update(&c, &hdr[3], 1);
    hmac_sha256_final(&c, t);
    memcpy(out, t, out_len);
}
