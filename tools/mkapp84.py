#!/usr/bin/env python3
"""mkapp84.py [--name NAME] IN.ihx IN.map OUT.bin - a TI-OS application
(make PLATFORM=tios) as the binary rabbitsign signs: page n of the app at
offset 4000h * n.  NAME (up to 8 characters) goes into the header.

  page 0     4000-43FF: header and launcher (platform/tios/launch.s)
             4400-    : the RAM image, linked at 8184h: _CODE .. _GSFINAL
                        (platform/tiplatform/os/crt0.s copies it there); image_len at 4083h
  page 1..N  the banked code, linked at N:4000h (logical page N)
"""
import re
import struct
import sys

IMAGE_AT = 0x8184
IMAGE_OFF = 0x400


def read_ihx(path):
    mem = {}
    base = 0
    for ln in open(path):
        ln = ln.strip()
        if not ln.startswith(':'):
            continue
        b = bytes.fromhex(ln[1:])
        n, addr, typ = b[0], b[1] << 8 | b[2], b[3]
        if typ == 0:
            for i in range(n):
                mem[base + addr + i] = b[4 + i]
        elif typ == 4:
            base = (b[4] << 8 | b[5]) << 16
    return mem


def sym(mapfile, name):
    m = re.search(r'\b([0-9A-F]{8})\s+%s\b' % re.escape(name), open(mapfile).read())
    if not m:
        sys.exit('mkapp84: %s not in the map' % name)
    return int(m.group(1), 16)


def main():
    args = sys.argv[1:]
    name = None
    if args[:1] == ['--name']:
        name, args = args[1], args[2:]
    ihx, mapfile, out = args
    mem = read_ihx(ihx)
    end = sym(mapfile, 's__GSFINAL') + sym(mapfile, 'l__GSFINAL')
    data = sym(mapfile, 's__DATA')
    if data < end:
        sys.exit('mkapp84: _DATA (%04X) overlaps the image (ends %04X)' % (data, end))
    for a in mem:
        if 0x8000 <= a < IMAGE_AT or (end <= a <= 0xFFFF and a < data):
            sys.exit('mkapp84: data at %04X outside the image' % a)
    size = end - IMAGE_AT
    if IMAGE_OFF + size > 0x4000:
        sys.exit('mkapp84: the RAM image (%d bytes) does not fit page 0' % size)
    pages = max([a >> 16 for a in mem if a >= 0x10000] + [0])
    app = bytearray(b'\0' * (0x4000 * (pages + 1)))
    for a, v in mem.items():
        if 0x4000 <= a < 0x4000 + IMAGE_OFF:
            app[a - 0x4000] = v
        elif IMAGE_AT <= a < end:
            app[IMAGE_OFF + a - IMAGE_AT] = v
        elif a >= 0x10000:
            p, off = a >> 16, a & 0xFFFF
            if not 0x4000 <= off < 0x8000:
                sys.exit('mkapp84: %06X is not in bank A' % a)
            app[p * 0x4000 + off - 0x4000] = v
        elif a >= data:
            sys.exit('mkapp84: initialized data at %04X (use the initializer)' % a)
    struct.pack_into('<H', app, 0x83, size)
    # the page count field of the header (80h 81h n)
    i = app.find(b'\x80\x81', 0, 0x80)
    if i < 0:
        sys.exit('mkapp84: no page count in the header')
    app[i + 2] = pages + 1
    if name:
        i = app.find(b'\x80\x48', 0, 0x80)
        if i < 0 or len(name) > 8:
            sys.exit('mkapp84: no 8-character name field, or the name is too long')
        app[i + 2:i + 10] = name.encode().ljust(8)
    # the last page only as far as it is used (the signature goes after it)
    last = max([a for a in mem if a >> 16 == pages] or [0x3FFF])
    used = pages * 0x4000 + (last & 0xFFFF) - 0x4000 + 1 if pages else IMAGE_OFF + size
    del app[used:]
    open(out, 'wb').write(app)
    print('%s: %d pages, RAM image %d bytes (8184-%04X), data from %04X' %
          (out, pages + 1, size, end - 1, data))


if __name__ == '__main__':
    main()
