/* bytes.c - C versions of lwIP-CE's eZ80 helpers in tls/core/bytes.s */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../../src/tls/includes/bytes.h"

bool tls_bytes_compare(const void *buf1, const void *buf2, size_t len)
{
    const volatile uint8_t *a = buf1, *b = buf2;
    uint8_t diff = 0;
    while (len--)
        diff |= *a++ ^ *b++;
    return diff == 0;
}

void tls_secure_memzero(void *ptr, size_t len)
{
    volatile uint8_t *p = ptr;
    while (len--)
        *p++ = 0;
}
