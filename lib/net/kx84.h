/* kx84.h - shared X25519 key pair (lib/net/kx84.c) */
#ifndef TI84P_KX84_H
#define TI84P_KX84_H
#include <stdint.h>
#include <stdbool.h>
#include "x25519.h"

/* experimental speed mode (README, Security): private key 1 */
#ifndef KX_INSECURE_FAST
#define KX_INSECURE_FAST 1
#endif

void kx_start(void);                    /* begin generating in the background */
bool kx_run(uint16_t budget_ms);        /* work on it now; true when ready */
void kx_poll(void);                     /* main loop: a little background work */
bool kx_ready(void);
uint8_t kx_progress(void);
const uint8_t *kx_priv(void);
const uint8_t *kx_pub(void);
#if KX_INSECURE_FAST
/* the P-256 (secp256r1) public key with private key 1 into out: the base
 * point G, uncompressed (65 bytes); the shared secret is the x coordinate
 * of the peer's public key */
void kx_p256_pub(uint8_t *out);
#endif
/* advance a shared-secret computation for up to budget_ms */
bool kx_shared_step(x25519_ctx *c, uint16_t budget_ms);

#endif
