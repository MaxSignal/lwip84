#!/usr/bin/env python3
"""pbm2png.py IN.pbm OUT.png [SCALE] - enlarge an emu84 LCD dump (P1) to PNG."""
import sys, zlib, struct
src, dst = sys.argv[1], sys.argv[2]
scale = int(sys.argv[3]) if len(sys.argv) > 3 else 4
tok = open(src).read().split()
assert tok[0] == 'P1'
w, h = int(tok[1]), int(tok[2])
bits = ''.join(tok[3:])
raw = bytearray()
for y in range(h):
    row = bytearray()
    for x in range(w):
        v = 0x20 if bits[y * w + x] == '1' else 0xC8
        row += bytes([v, v + 8 if v < 0xC8 else 0xD8, v]) * scale
    for _ in range(scale):
        raw += b'\0' + row
def chunk(t, d):
    return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w * scale, h * scale, 8, 2, 0, 0, 0))
png += chunk(b'IDAT', zlib.compress(bytes(raw))) + chunk(b'IEND', b'')
open(dst, 'wb').write(png)
