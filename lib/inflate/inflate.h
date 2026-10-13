/* inflate.h - Deflate (RFC 1951) decompression from extended RAM to
 * extended RAM (lib/inflate/inflate.c), after tinf by Joergen Ibsen.
 *
 * The output is its own window: back references are read from what was
 * written, so the whole output must stay in extended RAM.  Only small
 * buffers and the Huffman tables (about 1 KiB, from the heap while it
 * runs) are in RAM. */
#ifndef INFLATE_H
#define INFLATE_H
#include <stdint.h>

#define INFLATE_OK 0
#define INFLATE_BAD (-1)        /* not valid Deflate data */
#define INFLATE_FULL (-2)       /* more than dst_max bytes */
#define INFLATE_NOMEM (-3)

/* inflate src_len bytes at extended RAM address src into dst (at most
 * dst_max bytes); *out_len is set to the bytes written */
int8_t inflate_xram(uint32_t src, uint16_t src_len, uint32_t dst, uint16_t dst_max,
                    uint16_t *out_len);

/* CRC-32 (as in ZIP) of n bytes at extended RAM address a */
uint32_t crc32_xram(uint32_t a, uint16_t n);

#endif
