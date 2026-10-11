/* rng.c - random numbers for keys and nonces.
 *
 * Entropy (timer values, the Z80 refresh register and USB/keypad timing)
 * is folded cheaply into a small pool by rng_mix(); rng_bytes() hashes the
 * pool with a counter.  Good enough for ephemeral keys on a calculator, not
 * a certified DRBG. */
#include <string.h>
#include "sha256.h"
#include "rng.h"

static uint8_t pool[32];
static uint8_t pool_pos;
static uint32_t counter;

void rng_mix(const void *data, uint8_t len)
{
    const uint8_t *p = data;
    while (len--) {
        uint8_t i = pool_pos++ & 31;
        pool[i] = (uint8_t)((pool[i] << 1 | pool[i] >> 7) ^ *p++ ^ pool_pos);
    }
}

void rng_bytes(void *out, size_t len)
{
    uint8_t *o = out, h[32];
    sha256_ctx c;
    while (len) {
        uint8_t k = len > 32 ? 32 : (uint8_t)len;
        counter++;
        sha256_init(&c);
        sha256_update(&c, pool, 32);
        sha256_update(&c, &counter, sizeof(counter));
        sha256_final(&c, h);
        memcpy(o, h, k);
        /* forward secrecy of the pool: replace it with another hash */
        sha256_init(&c);
        sha256_update(&c, h, 32);
        sha256_update(&c, pool, 32);
        sha256_final(&c, pool);
        o += k;
        len -= k;
    }
}
