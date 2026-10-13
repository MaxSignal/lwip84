/* inflate_test.c - lib/inflate on the host:
 *   gcc -DINFLATE_HOST -I lib/inflate -o inflate_test lib/inflate/tests/inflate_test.c \
 *       lib/inflate/inflate.c
 *   inflate_test FILE.zip...
 * Inflates every stored or deflated member of each ZIP and checks its
 * CRC-32; members of up to 32 KiB (the extended RAM given to it here). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "inflate.h"

static uint8_t xram[0x20000];

void xram_read(uint32_t a, void *d, uint16_t n) { memcpy(d, xram + a, n); }
void xram_write(uint32_t a, const void *s, uint16_t n) { memcpy(xram + a, s, n); }

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return p[0] | p[1] << 8; }

int main(int argc, char **argv)
{
    int fails = 0, done = 0;
    for (int f = 1; f < argc; f++) {
        FILE *fp = fopen(argv[f], "rb");
        static uint8_t z[4 << 20];
        size_t n = fp ? fread(z, 1, sizeof(z), fp) : 0, o = 0;
        if (fp)
            fclose(fp);
        while (o + 30 <= n && u32(z + o) == 0x04034b50) {
            uint16_t flags = u16(z + o + 6), method = u16(z + o + 8);
            uint32_t crc = u32(z + o + 14), cs = u32(z + o + 18), us = u32(z + o + 22);
            uint16_t nl = u16(z + o + 26), xl = u16(z + o + 28), out;
            size_t d = o + 30 + nl + xl;
            if (flags & 8)
                break;
            if ((method == 8 || method == 0) && cs < 0x10000 && us <= 0x8000 && us > 0) {
                int8_t r = 0;
                memcpy(xram + 0x10000, z + d, cs);
                if (method == 8) {
                    r = inflate_xram(0x10000, (uint16_t)cs, 0, 0x8000, &out);
                } else {
                    memcpy(xram, z + d, cs);
                    out = (uint16_t)cs;
                }
                uint32_t c = crc32_xram(0, out);
                if (r || out != us || c != crc) {
                    printf("FAIL %s: %.*s r=%d out=%u/%u crc %08x/%08x\n", argv[f], nl,
                           (char *)z + o + 30, r, out, (unsigned)us, (unsigned)c, (unsigned)crc);
                    fails++;
                } else {
                    done++;
                }
            }
            o = d + cs;
        }
    }
    printf(fails ? "inflate: %d of %d failed\n" : "inflate: all %d passed\n",
           fails ? fails : done, fails + done);
    return fails != 0;
}
