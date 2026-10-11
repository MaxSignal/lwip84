/* x25519.h - X25519 (RFC 7748) that can be computed in small steps, so that
 * the network and the user interface keep running during the 10+ seconds a
 * scalar multiplication takes on a 15 MHz Z80.  Not constant-time. */
#ifndef TI84P_X25519_H
#define TI84P_X25519_H
#include <stdint.h>
#include <stdbool.h>

typedef uint8_t fe[32];        /* little endian, value < 2^256, reduced mod 2^255-19 lazily */

typedef struct {
    uint8_t k[32];
    fe v[12];                   /* ladder variables, then inversion temporaries */
    uint8_t sw;                 /* x2/z2 and x3/z3 are swapped */
    uint8_t phase;
    int16_t bit;
    uint8_t ip, n;              /* inversion program counter, squarings left */
    uint8_t lp;                 /* position in the ladder program */
#ifdef FE_ASM
    uint8_t scr[627];           /* scratch of fe_mul_core */
#endif
} x25519_ctx;

void x25519_start(x25519_ctx *c, const uint8_t scalar[32], const uint8_t point[32]);
/* do one unit of work (one field multiplication, about 7 ms with the
 * Z80 assembly); returns true when the result is ready */
bool x25519_step(x25519_ctx *c);
void x25519_result(x25519_ctx *c, uint8_t out[32]);
/* blocking convenience (host tests) */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);

/* field arithmetic (fe_mul and fe_sq may be implemented in assembly) */
void fe_mul(uint8_t *r, const uint8_t *a, const uint8_t *b);
void fe_sq(uint8_t *r, const uint8_t *a);

#endif
