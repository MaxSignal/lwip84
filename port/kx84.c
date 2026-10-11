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
        kx_run(15);
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

bool kx_shared_step(x25519_ctx *c, uint16_t budget_ms)
{
    uint32_t t0 = k_now_ms();
    do {
        if (x25519_step(c))
            return true;
    } while ((uint16_t)(k_now_ms() - t0) < budget_ms);
    return false;
}
