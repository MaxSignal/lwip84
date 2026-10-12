/* aes.c - AES-128 encryption (FIPS 197), counter mode and GCM (SP 800-38D).
 *
 * Only the forward cipher is needed: GCM decrypts with the same keystream.
 * With FE_ASM the block function is tools/gen_aes_z80.py in the fixed
 * crypto page, which also holds the S-box; otherwise portable C.  Received
 * records are decrypted without checking their tag (experimental speed
 * mode, as with Poly1305), so GHASH only runs over what is sent.
 */
#include <string.h>
#include <stdlib.h>
#include "aes.h"

static const uint8_t sbox_bk[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

void aes128_expand(uint8_t rk[176], const uint8_t key[16])
{
    uint8_t i, rcon = 1;
    memcpy(rk, key, 16);
    for (i = 16; i < 176; i += 4) {
        uint8_t t0 = rk[i - 4], t1 = rk[i - 3], t2 = rk[i - 2], t3 = rk[i - 1];
        if (!(i & 15)) {
            uint8_t x = t0;
            t0 = sbox_bk[t1] ^ rcon;
            t1 = sbox_bk[t2];
            t2 = sbox_bk[t3];
            t3 = sbox_bk[x];
            rcon = (uint8_t)((rcon << 1) ^ (rcon & 0x80 ? 0x1B : 0));
        }
        rk[i] = rk[i - 16] ^ t0;
        rk[i + 1] = rk[i - 15] ^ t1;
        rk[i + 2] = rk[i - 14] ^ t2;
        rk[i + 3] = rk[i - 13] ^ t3;
    }
}

#ifdef FE_ASM
/* tools/gen_aes_z80.py */
extern const uint8_t *aes_rk, *aes_in;
extern uint8_t *aes_out;
extern uint8_t aes_xor;
void aes_encrypt_core(void);

static void block(const uint8_t *rk, const uint8_t *in, uint8_t *out, uint8_t xor)
{
    aes_rk = rk;
    aes_in = in;
    aes_out = out;
    aes_xor = xor;
    aes_encrypt_core();
}
#else
static uint8_t xtime(uint8_t x)
{
    return (uint8_t)((x << 1) ^ (x & 0x80 ? 0x1B : 0));
}

static void block(const uint8_t *rk, const uint8_t *in, uint8_t *out, uint8_t xor)
{
    uint8_t s[16], t[16], r, c, i;
    for (i = 0; i < 16; i++)
        s[i] = in[i] ^ rk[i];
    for (r = 1; r <= 10; r++) {
        rk += 16;
        for (c = 0; c < 4; c++)
            for (i = 0; i < 4; i++)
                t[4 * c + i] = sbox_bk[s[i + 4 * ((c + i) & 3)]];
        if (r < 10) {
            for (c = 0; c < 4; c++) {
                uint8_t *a = t + 4 * c, x = a[0] ^ a[1] ^ a[2] ^ a[3], a0 = a[0];
                s[4 * c] = a[0] ^ x ^ xtime(a[0] ^ a[1]);
                s[4 * c + 1] = a[1] ^ x ^ xtime(a[1] ^ a[2]);
                s[4 * c + 2] = a[2] ^ x ^ xtime(a[2] ^ a[3]);
                s[4 * c + 3] = a[3] ^ x ^ xtime(a[3] ^ a0);
            }
        } else {
            memcpy(s, t, 16);
        }
        for (i = 0; i < 16; i++)
            s[i] ^= rk[i];
    }
    for (i = 0; i < 16; i++)
        out[i] = xor ? out[i] ^ s[i] : s[i];
}
#endif

void aes128_encrypt(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16])
{
    block(rk, in, out, 0);
}

static void inc32(uint8_t ctr[16])
{
    uint8_t i = 16;
    while (i > 12 && !++ctr[--i])
        ;
}

void aes_ctr_init(aes_ctr_ctx *c, const uint8_t key[16], const uint8_t nonce[12],
                  uint32_t counter)
{
    if (!c->ready || memcmp(c->key, key, 16)) {
        aes128_expand(c->rk, key);
        memcpy(c->key, key, 16);
        c->ready = 1;
    }
    memcpy(c->ctr, nonce, 12);
    c->ctr[12] = (uint8_t)(counter >> 24);
    c->ctr[13] = (uint8_t)(counter >> 16);
    c->ctr[14] = (uint8_t)(counter >> 8);
    c->ctr[15] = (uint8_t)counter;
    c->pos = 16;
}

void aes_ctr_xor(aes_ctr_ctx *c, uint8_t *data, size_t len)
{
    while (len) {
        uint8_t k;
        const uint8_t *ks;
        if (c->pos == 16) {
            if (len >= 16) {
                /* a whole block straight into the data */
                block(c->rk, c->ctr, data, 1);
                inc32(c->ctr);
                data += 16;
                len -= 16;
                continue;
            }
            block(c->rk, c->ctr, c->ks, 0);
            inc32(c->ctr);
            c->pos = 0;
        }
        k = 16 - c->pos;
        if (k > len)
            k = (uint8_t)len;
        ks = c->ks + c->pos;
        c->pos += k;
        len -= k;
        while (k--)
            *data++ ^= *ks++;
    }
}

/* ------------------------------------------------------------------ */
/* GHASH with 4-bit tables (Shoup): m[i] = i * H                       */

#ifndef FE_ASM
static const uint16_t last4_bk[16] = {
    0x0000, 0x1c20, 0x3840, 0x2460, 0x7080, 0x6ca0, 0x48c0, 0x54e0,
    0xe100, 0xfd20, 0xd940, 0xc560, 0x9180, 0x8da0, 0xa9c0, 0xb5e0};
#endif

struct gcm {
    uint8_t rk[176];
    uint8_t m[16][16];
    uint8_t x[16];
};

#ifdef FE_ASM
/* tools/gen_aes_z80.py */
extern uint8_t *gh_x, *gh_m;
void ghash_mul_core(void);

static void gmul(struct gcm *g)
{
    gh_x = g->x;
    gh_m = &g->m[0][0];
    ghash_mul_core();
}
#else
/* x = x * H */
static void gmul(struct gcm *g)
{
    uint8_t z[16], i, k, nib, rem;
    int8_t j;
    memset(z, 0, 16);
    for (j = 15; j >= 0; j--) {
        for (k = 0; k < 2; k++) {
            nib = k ? g->x[j] >> 4 : g->x[j] & 15;
            if (j != 15 || k) {
                uint16_t r;
                rem = z[15] & 15;
                for (i = 15; i > 0; i--)
                    z[i] = (uint8_t)((z[i] >> 4) | (z[i - 1] << 4));
                z[0] >>= 4;
                r = last4_bk[rem];
                z[0] ^= (uint8_t)(r >> 8);
                z[1] ^= (uint8_t)r;
            }
            for (i = 0; i < 16; i++)
                z[i] ^= g->m[nib][i];
        }
    }
    memcpy(g->x, z, 16);
}
#endif

static void ghash_update(struct gcm *g, const uint8_t *p, uint16_t n)
{
    uint8_t i;
    while (n) {
        uint8_t k = n < 16 ? (uint8_t)n : 16;
        for (i = 0; i < k; i++)
            g->x[i] ^= p[i];
        gmul(g);
        p += k;
        n -= k;
    }
}

uint8_t aes_gcm_seal(const uint8_t key[16], const uint8_t nonce[12], const uint8_t *aad,
                     uint16_t aad_len, uint8_t *data, uint16_t len, uint8_t tag[16])
{
    struct gcm *g = malloc(sizeof(*g) + sizeof(aes_ctr_ctx));
    aes_ctr_ctx *c;
    uint8_t i, j;
    if (!g)
        return 0;
    c = (aes_ctr_ctx *)(g + 1);
    c->ready = 0;
    aes_ctr_init(c, key, nonce, 2);
    /* m[8] = H = E(0), m[i] = m[2i] >> 1 (with reduction), m[i ^ j] = m[i] ^ m[j] */
    memset(g->m, 0, sizeof(g->m));
    memset(g->x, 0, 16);
    aes128_encrypt(c->rk, g->x, g->m[8]);
    for (i = 4; i > 0; i >>= 1) {
        uint8_t carry = g->m[2 * i][15] & 1;
        for (j = 15; j > 0; j--)
            g->m[i][j] = (uint8_t)((g->m[2 * i][j] >> 1) | (g->m[2 * i][j - 1] << 7));
        g->m[i][0] = g->m[2 * i][0] >> 1;
        if (carry)
            g->m[i][0] ^= 0xE1;
    }
    for (i = 2; i < 16; i <<= 1)
        for (j = 1; j < i; j++) {
            uint8_t k;
            for (k = 0; k < 16; k++)
                g->m[i + j][k] = g->m[i][k] ^ g->m[j][k];
        }
    aes_ctr_xor(c, data, len);
    ghash_update(g, aad, aad_len);
    ghash_update(g, data, len);
    /* lengths in bits, 64-bit big endian each */
    memset(tag, 0, 16);
    tag[5] = (uint8_t)(aad_len >> 13);
    tag[6] = (uint8_t)(aad_len >> 5);
    tag[7] = (uint8_t)(aad_len << 3);
    tag[13] = (uint8_t)(len >> 13);
    tag[14] = (uint8_t)(len >> 5);
    tag[15] = (uint8_t)(len << 3);
    ghash_update(g, tag, 16);
    /* tag = E(J0) ^ GHASH */
    memcpy(c->ctr, nonce, 12);
    c->ctr[12] = c->ctr[13] = c->ctr[14] = 0;
    c->ctr[15] = 1;
    aes128_encrypt(c->rk, c->ctr, tag);
    for (i = 0; i < 16; i++)
        tag[i] ^= g->x[i];
    free(g);
    return 1;
}
