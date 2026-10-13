/* sha256.h - SHA-256, HMAC-SHA-256 and the TLS 1.3 HKDF helpers */
#ifndef TI84P_SHA256_H
#define TI84P_SHA256_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t h[8];
    uint32_t len_lo, len_hi;    /* message length in bytes */
    uint8_t buf[64];
    uint8_t n;                  /* bytes in buf */
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[32]);
void sha256(const void *data, size_t len, uint8_t out[32]);

typedef struct {
    sha256_ctx inner;
    uint32_t outer[8];          /* SHA-256 state after the opad block */
} hmac_sha256_ctx;

void hmac_sha256_init(hmac_sha256_ctx *c, const uint8_t *key, size_t keylen);
void hmac_sha256_update(hmac_sha256_ctx *c, const void *data, size_t len);
void hmac_sha256_final(hmac_sha256_ctx *c, uint8_t out[32]);
void hmac_sha256(const uint8_t *key, size_t keylen, const void *data, size_t len,
                 uint8_t out[32]);

/* HKDF-Extract (RFC 5869) and HKDF-Expand-Label (RFC 8446 7.1);
 * out_len <= 32, context may be NULL when ctx_len is 0 */
void hkdf_extract(const uint8_t *salt, const uint8_t *ikm, uint8_t out[32]);
void hkdf_expand_label(const uint8_t secret[32], const char *label,
                       const uint8_t *context, uint8_t ctx_len,
                       uint8_t *out, uint8_t out_len);

#endif
