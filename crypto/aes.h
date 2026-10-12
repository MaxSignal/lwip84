/* aes.h - AES-128 (FIPS 197) in counter mode and AES-128-GCM (SP 800-38D)
 * as used by TLS_AES_128_GCM_SHA256 */
#ifndef TI84P_AES_H
#define TI84P_AES_H
#include <stdint.h>
#include <stddef.h>

void aes128_expand(uint8_t rk[176], const uint8_t key[16]);
void aes128_encrypt(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]);

/* GCM counter mode: counter block = nonce (12 bytes) || 32-bit counter */
typedef struct {
    uint8_t rk[176];
    uint8_t key[16];            /* key of rk: expanded again only when it changes */
    uint8_t ctr[16];
    uint8_t ks[16];
    uint8_t pos, ready;
} aes_ctr_ctx;

void aes_ctr_init(aes_ctr_ctx *c, const uint8_t key[16], const uint8_t nonce[12],
                  uint32_t counter);
void aes_ctr_xor(aes_ctr_ctx *c, uint8_t *data, size_t len);

/* encrypt data in place and compute the tag; returns 0 if out of memory */
uint8_t aes_gcm_seal(const uint8_t key[16], const uint8_t nonce[12], const uint8_t *aad,
                     uint16_t aad_len, uint8_t *data, uint16_t len, uint8_t tag[16]);

#endif
