#!/usr/bin/env python3
"""pagesums.py ROM [FIRST [COUNT]] - first bytes and 16-bit sum of flash pages
of a ROM image, as the debug build's status screen (Y=) shows them for the
font pages (default 08h, 11 pages)."""
import sys
rom = open(sys.argv[1], 'rb').read()
first = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x08
count = int(sys.argv[3]) if len(sys.argv) > 3 else 11
for p in range(first, first + count):
    b = rom[p * 16384:(p + 1) * 16384]
    print('pg %02x %s %04x' % (p, b[:4].hex(), sum(b) & 0xFFFF))
