#!/usr/bin/env python3
"""mkrom.py - turn the banked Intel HEX output of sdldz80 into a flash image.

Addresses 0000-3FFF are flash page 0; address N:4000-N:7FFF (N = bits 16+
of the linear address) is flash page N.  Anything else that carries data
(e.g. initialized RAM) is an error, because the OS copies initializers
from page 0 itself.

Usage: mkrom.py in.ihx out.rom [--pages 64]
"""
import sys


def main():
    args = sys.argv[1:]
    pages = 64
    if '--pages' in args:
        i = args.index('--pages')
        pages = int(args[i + 1], 0)
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
    open(dst, 'wb').write(rom)
    for p in sorted(used):
        print('page %02X: %5d bytes used' % (p, used[p]))


if __name__ == '__main__':
    main()
