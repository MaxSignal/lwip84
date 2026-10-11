/* x25519.c - X25519 Montgomery ladder over 8-bit limbs */
#include <string.h>
#include "x25519.h"

/* r = t[0..31] + 38 * t[32..63]  (2^256 = 38 mod p) */
static void fe_reduce(uint8_t *r, const uint8_t *t)
{
    uint16_t c = 0;
    uint8_t i;
    for (i = 0; i < 32; i++) {
        c += t[i] + 38u * t[i + 32];
        r[i] = (uint8_t)c;
        c >>= 8;
    }
    while (c) {
        c *= 38;
        for (i = 0; i < 32 && c; i++) {
            c += r[i];
            r[i] = (uint8_t)c;
            c >>= 8;
        }
    }
}

#ifdef FE_ASM
/* tools/gen_fe_z80.py: operands are passed in globals */
uint8_t *fe_r;
const uint8_t *fe_a, *fe_b;
uint8_t *fe_scr;
void fe_mul_core(void);
void fe_add_core(void);
void fe_sub_core(void);

void fe_mul(uint8_t *r, const uint8_t *a, const uint8_t *b)
{
    fe_r = r;
    fe_a = a;
    fe_b = b;
    fe_mul_core();
}

void fe_sq(uint8_t *r, const uint8_t *a)
{
    fe_mul(r, a, a);
}

static void fe_add(uint8_t *r, const uint8_t *a, const uint8_t *b)
{
    fe_r = r;
    fe_a = a;
    fe_b = b;
    fe_add_core();
}

static void fe_sub(uint8_t *r, const uint8_t *a, const uint8_t *b)
{
    fe_r = r;
    fe_a = a;
    fe_b = b;
    fe_sub_core();
}

static void fe_mul121665(uint8_t *r, const uint8_t *a)
{
    static const uint8_t k[32] = {0x41, 0xdb, 0x01};
    fe_mul(r, k, a);                    /* the nibbles of k drive the additions */
}
#else
void fe_mul(uint8_t *r, const uint8_t *a, const uint8_t *b)
{
    uint8_t t[64], k, i;
    uint32_t acc = 0;
    for (k = 0; k < 63; k++) {
        uint8_t lo = k < 32 ? 0 : k - 31, hi = k < 32 ? k : 31;
        for (i = lo; i <= hi; i++)
            acc += (uint16_t)a[i] * b[k - i];
        t[k] = (uint8_t)acc;
        acc >>= 8;
    }
    t[63] = (uint8_t)acc;
    fe_reduce(r, t);
}

void fe_sq(uint8_t *r, const uint8_t *a)
{
    fe_mul(r, a, a);
}


static void fe_add(uint8_t *r, const uint8_t *a, const uint8_t *b)
{
    uint16_t c = 0;
    uint8_t i;
    for (i = 0; i < 32; i++) {
        c += (uint16_t)a[i] + b[i];
        r[i] = (uint8_t)c;
        c >>= 8;
    }
    while (c) {
        c *= 38;
        for (i = 0; i < 32 && c; i++) {
            c += r[i];
            r[i] = (uint8_t)c;
            c >>= 8;
        }
    }
}

static void fe_sub(uint8_t *r, const uint8_t *a, const uint8_t *b)
{
    int16_t c = 0;
    uint8_t i;
    for (i = 0; i < 32; i++) {
        c += (int16_t)a[i] - b[i];
        r[i] = (uint8_t)c;
        c >>= 8;                        /* 0 or -1 */
    }
    while (c) {                         /* borrow: value + 2^256, subtract 38 */
        c = -38;
        for (i = 0; i < 32 && c; i++) {
            c += r[i];
            r[i] = (uint8_t)c;
            c >>= 8;
        }
    }
}

static void fe_mul121665(uint8_t *r, const uint8_t *a)
{
    uint8_t t[64], i;
    uint32_t c = 0;
    for (i = 0; i < 32; i++) {
        c += (uint32_t)a[i] * 121665u;
        t[i] = (uint8_t)c;
        c >>= 8;
    }
    memset(t + 32, 0, 32);
    t[32] = (uint8_t)c;
    t[33] = (uint8_t)(c >> 8);
    t[34] = (uint8_t)(c >> 16);
    fe_reduce(r, t);
}

#endif

/* fully reduce mod p = 2^255 - 19 */
static void fe_pack(uint8_t *out, const uint8_t *a)
{
    uint8_t t[32], i, n;
    int16_t c;
    memcpy(out, a, 32);
    for (n = 0; n < 2; n++) {
        /* t = out - p */
        c = (int16_t)out[0] - 0xED;
        t[0] = (uint8_t)c;
        c >>= 8;
        for (i = 1; i < 31; i++) {
            c += (int16_t)out[i] - 0xFF;
            t[i] = (uint8_t)c;
            c >>= 8;
        }
        c += (int16_t)out[31] - 0x7F;
        t[31] = (uint8_t)c;
        c >>= 8;
        if (c == 0)
            memcpy(out, t, 32);
    }
}

enum { X1, X2, Z2, X3, Z3, T0, T1, T2, T3 };
/* inversion slots (after the ladder): Z = z2 */
enum { IZ = Z2, IX = X2, I0 = X1, I9 = X3, I11 = Z3, I5 = T0, I10 = T1, I20 = T2, I50 = T3,
       I100 = 9, IT = 10, IOUT = 11 };

/* z^(p-2) = z^(2^255-21), the usual chain of 254 squarings and 11 multiplications */
#define SQ(d, s, n) {0, d, s, n}
#define MUL(d, a, b) {1, d, a, b}
static const uint8_t inv_prog_bk[][4] = {
    SQ(I0, IZ, 1), SQ(IT, I0, 2), MUL(I9, IT, IZ), MUL(I11, I9, I0), SQ(IT, I11, 1),
    MUL(I5, IT, I9), SQ(IT, I5, 5), MUL(I10, IT, I5), SQ(IT, I10, 10), MUL(I20, IT, I10),
    SQ(IT, I20, 20), MUL(IT, IT, I20), SQ(IT, IT, 10), MUL(I50, IT, I10), SQ(IT, I50, 50),
    MUL(I100, IT, I50), SQ(IT, I100, 100), MUL(IT, IT, I100), SQ(IT, IT, 50),
    MUL(IT, IT, I50), SQ(IT, IT, 5), MUL(IT, IT, I11), MUL(IOUT, IX, IT)};
#define INV_LEN (sizeof(inv_prog_bk) / sizeof(inv_prog_bk[0]))

void x25519_start(x25519_ctx *c, const uint8_t scalar[32], const uint8_t point[32])
{
    memcpy(c->k, scalar, 32);
    c->k[0] &= 248;
    c->k[31] = (c->k[31] & 127) | 64;
    memset(c->v, 0, sizeof(c->v));
    memcpy(c->v[X1], point, 32);
    c->v[X1][31] &= 127;
    c->v[X2][0] = 1;
    memcpy(c->v[X3], c->v[X1], 32);
    c->v[Z3][0] = 1;
    c->sw = 0;
    c->bit = 254;
    c->phase = 0;
    c->ip = 0;
    c->n = 0;
    c->lp = 0;
}

static void swap_xz(x25519_ctx *c)
{
    uint8_t t[32];
    memcpy(t, c->v[X2], 32); memcpy(c->v[X2], c->v[X3], 32); memcpy(c->v[X3], t, 32);
    memcpy(t, c->v[Z2], 32); memcpy(c->v[Z2], c->v[Z3], 32); memcpy(c->v[Z3], t, 32);
}

/* one ladder bit (RFC 7748) as a program of field operations, run one
 * multiplication at a time so that a step is only a few milliseconds */
enum { O_ADD, O_SUB, O_MUL, O_SQ, O_M121665 };
static const uint8_t ladder_prog_bk[][4] = {
    {O_ADD, T0, X2, Z2}, {O_SUB, T1, X2, Z2}, {O_ADD, T2, X3, Z3}, {O_SUB, T3, X3, Z3},
    {O_MUL, T3, T3, T0},                /* DA */
    {O_MUL, T2, T2, T1},                /* CB */
    {O_SQ, T0, T0, 0},                  /* AA */
    {O_SQ, T1, T1, 0},                  /* BB */
    {O_ADD, X3, T3, T2}, {O_SQ, X3, X3, 0},            /* x3 = (DA + CB)^2 */
    {O_SUB, Z3, T3, T2}, {O_SQ, Z3, Z3, 0},
    {O_MUL, Z3, Z3, X1},                /* z3 = x1 (DA - CB)^2 */
    {O_MUL, X2, T0, T1},                /* x2 = AA BB */
    {O_SUB, T1, T0, T1},                /* E = AA - BB */
    {O_M121665, T2, T1, 0}, {O_ADD, T2, T2, T0},
    {O_MUL, Z2, T1, T2}};               /* z2 = E (AA + a24 E) */
#define LADDER_LEN (sizeof(ladder_prog_bk) / sizeof(ladder_prog_bk[0]))

static void ladder_part(x25519_ctx *c)
{
    fe *v = c->v;
    if (c->lp == 0) {
        uint8_t kt = (c->k[c->bit >> 3] >> (c->bit & 7)) & 1;
        if (kt != c->sw) {
            swap_xz(c);
            c->sw = kt;
        }
    }
    for (;;) {
        const uint8_t *op = ladder_prog_bk[c->lp];
        uint8_t heavy = op[0] >= O_MUL;
        switch (op[0]) {
        case O_ADD: fe_add(v[op[1]], v[op[2]], v[op[3]]); break;
        case O_SUB: fe_sub(v[op[1]], v[op[2]], v[op[3]]); break;
        case O_MUL: fe_mul(v[op[1]], v[op[2]], v[op[3]]); break;
        case O_SQ: fe_sq(v[op[1]], v[op[2]]); break;
        default: fe_mul121665(v[op[1]], v[op[2]]); break;
        }
        if (++c->lp == LADDER_LEN) {
            c->lp = 0;
            c->bit--;
            return;
        }
        if (heavy)
            return;
    }
}

bool x25519_step(x25519_ctx *c)
{
    const uint8_t *op;
#ifdef FE_ASM
    fe_scr = c->scr;
#endif
    if (c->phase == 0) {
        ladder_part(c);
        if (c->bit < 0) {
            if (c->sw)
                swap_xz(c);
            c->phase = 1;
        }
        return false;
    }
    if (c->ip >= INV_LEN)
        return true;
    op = inv_prog_bk[c->ip];
    if (op[0]) {
        fe_mul(c->v[op[1]], c->v[op[2]], c->v[op[3]]);
        c->ip++;
    } else {
        if (c->n == 0) {
            fe_sq(c->v[op[1]], c->v[op[2]]);
            c->n = op[3];
        } else {
            fe_sq(c->v[op[1]], c->v[op[1]]);
        }
        if (--c->n == 0)
            c->ip++;
    }
    return c->ip >= INV_LEN;
}

void x25519_result(x25519_ctx *c, uint8_t out[32])
{
    fe_pack(out, c->v[IOUT]);
}

#ifndef FE_ASM
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    static x25519_ctx c;
    x25519_start(&c, scalar, point);
    while (!x25519_step(&c))
        ;
    x25519_result(&c, out);
}
#endif

/* rough progress in percent (the ladder is about 90% of the work) */
uint8_t x25519_progress(const x25519_ctx *c)
{
    if (c->phase == 0)
        return (uint8_t)((uint16_t)(254 - c->bit) * 90u / 255u);
    return (uint8_t)(90u + c->ip * 10u / INV_LEN);
}
