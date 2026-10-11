/* heap.c - malloc/free/realloc for the OS and lwIP.
 *
 * First-fit allocator with immediate coalescing over the RAM between the
 * end of the static data and the stack.  Every block has a 2-byte header:
 * the block size (including the header) with bit 0 set while in use.
 */
#include <string.h>
#include "kernel.h"

#define STACK_RESERVE 2048u
#define HDR 2u
#define USED 1u

extern uint8_t heap_base[];     /* end of the static data (crt0.s) */

static uint8_t *heap_start, *heap_end;

static uint16_t *hdr(uint8_t *p) { return (uint16_t *)p; }

void heap_init(void)
{
    /* block sizes are even, so both ends must be even too */
    heap_start = (uint8_t *)(((uint16_t)heap_base + 1u) & ~1u);
    heap_end = (uint8_t *)(0x10000u - STACK_RESERVE);
    *hdr(heap_start) = (uint16_t)(heap_end - heap_start) & ~1u;
}

static void coalesce_from(uint8_t *p)
{
    uint16_t sz = *hdr(p);
    for (;;) {
        uint8_t *n = p + sz;
        if (n >= heap_end || (*hdr(n) & USED))
            break;
        sz += *hdr(n);
    }
    *hdr(p) = sz;
}

void *malloc(size_t size)
{
    uint8_t *p;
    uint16_t need;
    if (!size || size > 0x7FF0u)
        return NULL;
    need = (size + HDR + 1u) & ~1u;
    if (need < 4u)
        need = 4u;
    for (p = heap_start; p < heap_end; p += *hdr(p) & ~USED) {
        uint16_t h = *hdr(p);
        if (h & USED)
            continue;
        coalesce_from(p);
        h = *hdr(p);
        if (h >= need) {
            if (h - need >= 6u) {
                *hdr(p + need) = h - need;
                h = need;
            }
            *hdr(p) = h | USED;
            return p + HDR;
        }
    }
    return NULL;
}

void free(void *ptr)
{
    uint8_t *p;
    if (!ptr)
        return;
    p = (uint8_t *)ptr - HDR;
    *hdr(p) &= ~USED;
    coalesce_from(p);
}

void *calloc(size_t n, size_t size)
{
    size_t total = n * size;
    void *p;
    if (size && total / size != n)
        return NULL;
    p = malloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size)
{
    uint8_t *p;
    uint16_t have, need;
    void *n;
    if (!ptr)
        return malloc(size);
    if (!size) {
        free(ptr);
        return NULL;
    }
    p = (uint8_t *)ptr - HDR;
    have = *hdr(p) & ~USED;
    need = (size + HDR + 1u) & ~1u;
    if (need < 4u)
        need = 4u;
    /* grow in place if the following blocks are free */
    *hdr(p) = have;
    coalesce_from(p);
    have = *hdr(p);
    if (have >= need) {
        if (have - need >= 6u) {
            *hdr(p + need) = have - need;
            have = need;
        }
        *hdr(p) = have | USED;
        return ptr;
    }
    *hdr(p) = have | USED;
    n = malloc(size);
    if (!n)
        return NULL;
    memcpy(n, ptr, have - HDR);
    free(ptr);
    return n;
}

size_t heap_free_bytes(void)
{
    uint8_t *p;
    size_t total = 0;
    for (p = heap_start; p < heap_end; p += *hdr(p) & ~USED)
        if (!(*hdr(p) & USED))
            total += *hdr(p) - HDR;
    return total;
}

size_t heap_largest_block(void)
{
    uint8_t *p;
    size_t best = 0;
    for (p = heap_start; p < heap_end; p += *hdr(p) & ~USED) {
        if (!(*hdr(p) & USED)) {
            coalesce_from(p);
            if (*hdr(p) - HDR > best)
                best = *hdr(p) - HDR;
        }
    }
    return best;
}
