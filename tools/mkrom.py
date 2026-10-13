#!/usr/bin/env python3
"""mkrom.py - turn the banked Intel HEX output of sdldz80 into a flash image.

Addresses 0000-3FFF are flash page 0; address N:4000-N:7FFF (N = bits 16+
of the linear address) is flash page N.  Anything else that carries data
(e.g. initialized RAM) is an error, because the OS copies initializers
from page 0 itself.

Usage: mkrom.py in.ihx out.rom [--pages 64] [--data PAGE:FILE]... [--pagelist OUT]

--data puts a file (a font) on flash pages from PAGE on; --pagelist writes
the pages that hold anything, for mktiupgrade.
"""
import sys


def main():
    args = sys.argv[1:]
    pages = 64
    extra, pagelist = [], None
    if '--pages' in args:
        i = args.index('--pages')
        pages = int(args[i + 1], 0)
        del args[i:i + 2]
    while '--data' in args:
        i = args.index('--data')
        page, fn = args[i + 1].split(':', 1)
        extra.append((int(page, 16), fn))
        del args[i:i + 2]
    if '--pagelist' in args:
        i = args.index('--pagelist')
        pagelist = args[i + 1]
        del args[i:i + 2]
    src, dst = args
    rom = bytearray(b'\xff' * (pages * 0x4000))
    used = {}
    base = 0
    for ln, line in enumerate(open(src), 1):
        line = line.strip()
        if not line.startswith(':'):
            continue
        b = bytes.fromhex(line[1:])
        n, addr, typ, data = b[0], (b[1] << 8) | b[2], b[3], b[4:4 + b[0]]
        if sum(b) & 0xFF:
            sys.exit('%s:%d: bad checksum' % (src, ln))
        if typ == 1:
            break
        if typ == 4:
            base = ((data[0] << 8) | data[1]) << 16
            continue
        if typ != 0:
            continue
        for i, v in enumerate(data):
            a = base + addr + i
            page, off = a >> 16, a & 0xFFFF
            if page == 0 and off < 0x4000:
                fa = off
            elif 0x4000 <= off < 0x8000 and page > 0:
                fa = page * 0x4000 + (off - 0x4000)
            else:
                sys.exit('mkrom: data at %06X is outside flash' % a)
            if fa >= len(rom):
                sys.exit('mkrom: page %02X beyond flash size' % (fa >> 14))
            rom[fa] = v
            used[fa >> 14] = used.get(fa >> 14, 0) + 1
    for page, fn in extra:
        blob = open(fn, 'rb').read()
        fa = page * 0x4000
        if fa + len(blob) > len(rom):
            sys.exit('mkrom: %s does not fit from page %02X' % (fn, page))
        for i in range(0, len(blob), 0x4000):
            p = (fa + i) >> 14
            if p in used:
                sys.exit('mkrom: %s overlaps page %02X' % (fn, p))
        for i in range((len(blob) + 0x3FFF) >> 14):
            used[page + i] = min(0x4000, len(blob) - i * 0x4000)
        rom[fa:fa + len(blob)] = blob
    # the TI-84 Plus boot code (1.02) writes a page sent as 10h-1Fh to
    # 30h-3Fh, and did not write 2Ch-32h (tools/pagetest.sh)
    for p in sorted(used):
        if 0x10 <= p <= 0x1F or (pages == 64 and 0x2C <= p <= 0x32):
            sys.exit('mkrom: page %02X cannot be sent to the calculator' % p)
    open(dst, 'wb').write(rom)
    for p in sorted(used):
        print('page %02X: %5d bytes used' % (p, used[p]))
    if pagelist:
        open(pagelist, 'w').write(' '.join('%02X' % p for p in sorted(used)) + '\n')


if __name__ == '__main__':
    main()
