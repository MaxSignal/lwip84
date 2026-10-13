/* kx84.c - the X25519 key pair used by TLS and SSH.
 *
 * A scalar multiplication takes many seconds on the Z80, so one key pair
 * is generated in the background (kx_poll() from the main loop) and then
 * reused for every connection until the calculator is reset.  This gives
 * up forward secrecy between connections in exchange for halving the time
 * of every handshake. */
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "x25519.h"
#include "rng.h"
#include "kx84.h"

static x25519_ctx *gen;
static uint8_t kx_state;               /* 0 idle, 1 generating, 2 ready */
static uint8_t priv[32], pub[32];
static uint32_t kx_t0;

void kx_start(void)
{
    static const uint8_t base[32] = {9};
    uint32_t t;
    if (kx_state)
        return;
#if KX_INSECURE_FAST
    /* Experimental speed mode (no security at all): the private key is 1,
     * so the public key is the base point 9 and the shared secret equals the
     * peer's public key (peer: s * 9 = its own public key).  Servers accept
     * this as an ordinary X25519 exchange; an eavesdropper can decrypt it. */
    memset(priv, 0, 32);
    priv[0] = 1;
    memcpy(pub, base, 32);
    kx_state = 2;
    (void)t;
    return;
#endif
    gen = malloc(sizeof(*gen));
    if (!gen)
        return;
    t = k_now_ms();
    rng_mix(&t, sizeof(t));
    rng_bytes(priv, 32);
    x25519_start(gen, priv, base);
    kx_state = 1;
    kx_t0 = k_now_ms();
}

bool kx_run(uint16_t budget_ms)
{
    uint32_t t0;
    if (kx_state == 0)
        kx_start();
    if (kx_state != 1)
        return kx_state == 2;
    t0 = k_now_ms();
    do {
        if (x25519_step(gen)) {
            x25519_result(gen, pub);
            free(gen);
            gen = NULL;
            kx_state = 2;
            dbg_printf("kx: key pair ready (%lu ms)\n", k_now_ms() - kx_t0);
            return true;
        }
    } while ((uint16_t)(k_now_ms() - t0) < budget_ms);
    return false;
}

void kx_poll(void)
{
    if (kx_state == 1)
        kx_run(8);
}

bool kx_ready(void)
{
    return kx_state == 2;
}

const uint8_t *kx_priv(void)
{
    return priv;
}

const uint8_t *kx_pub(void)
{
    return pub;
}

#if KX_INSECURE_FAST
void kx_p256_pub(uint8_t *out)
{
    static const uint8_t g_bk[65] = {
        0x04,
        0x6B, 0x17, 0xD1, 0xF2, 0xE1, 0x2C, 0x42, 0x47, 0xF8, 0xBC, 0xE6, 0xE5, 0x63, 0xA4, 0x40, 0xF2,
        0x77, 0x03, 0x7D, 0x81, 0x2D, 0xEB, 0x33, 0xA0, 0xF4, 0xA1, 0x39, 0x45, 0xD8, 0x98, 0xC2, 0x96,
        0x4F, 0xE3, 0x42, 0xE2, 0xFE, 0x1A, 0x7F, 0x9B, 0x8E, 0xE7, 0xEB, 0x4A, 0x7C, 0x0F, 0x9E, 0x16,
        0x2B, 0xCE, 0x33, 0x57, 0x6B, 0x31, 0x5E, 0xCE, 0xCB, 0xB6, 0x40, 0x68, 0x37, 0xBF, 0x51, 0xF5};
    memcpy(out, g_bk, 65);              /* g_bk is on this function's page */
}
#endif

bool kx_shared_step(x25519_ctx *c, uint16_t budget_ms)
{
    uint32_t t0 = k_now_ms();
    do {
        if (x25519_step(c))
            return true;
    } while ((uint16_t)(k_now_ms() - t0) < budget_ms);
    return false;
}

/* progress of the key pair generation in percent */
uint8_t kx_progress(void)
{
    return kx_state == 2 ? 100 : gen ? x25519_progress(gen) : 0;
}
