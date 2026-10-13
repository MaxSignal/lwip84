/* rng.h - entropy pool and random bytes (lib/crypto/rng.c) */
#ifndef TI84P_RNG_H
#define TI84P_RNG_H
#include <stdint.h>
#include <stddef.h>
void rng_mix(const void *data, uint8_t len);
void rng_bytes(void *out, size_t len);
#endif
