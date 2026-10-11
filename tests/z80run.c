/* z80run - run a flat Z80 program for unit tests of the assembly routines.
 *   z80run IN.ihx START IN.bin@ADDR... -- dump ADDR LEN OUT.bin
 * Loads the Intel HEX image and binary blobs into a 64 KiB memory, runs from
 * START until HALT and writes the requested memory range.  Prints the number
 * of T-states. */
#define CHIPS_IMPL
#include "../emu/third_party/z80.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t mem[65536];

static void load_ihx(const char *fn)
{
    FILE *f = fopen(fn, "r");
    char line[600];
    if (!f) { perror(fn); exit(1); }
    while (fgets(line, sizeof line, f)) {
        unsigned n, a, t, i, v;
        if (line[0] != ':') continue;
        sscanf(line + 1, "%2x%4x%2x", &n, &a, &t);
        if (t != 0) continue;
        for (i = 0; i < n; i++) {
            sscanf(line + 9 + 2 * i, "%2x", &v);
            mem[(a + i) & 0xFFFF] = v;
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    int i = 3;
    unsigned start, daddr = 0, dlen = 0;
    const char *dout = NULL;
    z80_t cpu;
    uint64_t pins, ticks = 0;
    load_ihx(argv[1]);
    start = strtoul(argv[2], NULL, 0);
    for (; i < argc && strcmp(argv[i], "--"); i++) {
        char *at = strchr(argv[i], '@');
        FILE *f;
        *at = 0;
        f = fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); return 1; }
        { unsigned a = strtoul(at + 1, NULL, 0); fread(mem + a, 1, 65536 - a, f); }
        fclose(f);
    }
    if (i + 3 < argc) {
        daddr = strtoul(argv[i + 1], NULL, 0);
        dlen = strtoul(argv[i + 2], NULL, 0);
        dout = argv[i + 3];
    }
    pins = z80_init(&cpu);
    pins = z80_prefetch(&cpu, start);
    cpu.sp = 0xFFF0;
    for (;;) {
        pins = z80_tick(&cpu, pins);
        ticks++;
        if (pins & Z80_HALT) break;
        if (pins & Z80_MREQ) {
            uint16_t a = Z80_GET_ADDR(pins);
            if (pins & Z80_RD) {
                Z80_SET_DATA(pins, mem[a]);
            } else if (pins & Z80_WR) {
                mem[a] = Z80_GET_DATA(pins);
            }
        } else if ((pins & Z80_IORQ) && (pins & Z80_RD)) {
            Z80_SET_DATA(pins, 0xFF);
        }

        if (ticks > 2000000000ULL) { fprintf(stderr, "z80run: no HALT\n"); return 1; }
    }
    printf("%llu\n", (unsigned long long)ticks);
    if (dout) {
        FILE *f = fopen(dout, "wb");
        fwrite(mem + daddr, 1, dlen, f);
        fclose(f);
    }
    return 0;
}
