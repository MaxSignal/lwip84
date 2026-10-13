/* chacha.h - ChaCha20 (RFC 8439) and Poly1305 */
#ifndef TI84P_CHACHA_H
#define TI84P_CHACHA_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t s[16];             /* input block: constants, key, counter, nonce */
    uint8_t ks[64];             /* current keystream block */
    uint8_t pos;                /* bytes of ks used (64: need a new block) */
} chacha_ctx;

/* nonce_len 12 (RFC 8439: 32-bit counter) or 8 (original ChaCha20 as used
 * by chacha20-poly1305@openssh.com: 64-bit counter) */
void chacha_init(chacha_ctx *c, const uint8_t key[32], const uint8_t *nonce,
                 uint8_t nonce_len, uint32_t counter);
void chacha_xor(chacha_ctx *c, uint8_t *data, size_t len);
void chacha_block(const uint32_t in[16], uint8_t out[64]);

typedef struct {
    uint8_t r[17], h[17], pad[16];
    uint8_t buf[16];
    uint8_t n;
} poly1305_ctx;

void poly1305_init(poly1305_ctx *c, const uint8_t key[32]);
void poly1305_update(poly1305_ctx *c, const uint8_t *data, size_t len);
void poly1305_final(poly1305_ctx *c, uint8_t tag[16]);

#endif
