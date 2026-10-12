#!/usr/bin/env python3
"""mkfont84.py FONT.bdf OUT.bin - a BDF bitmap font as an lwIP-84+ font blob.

The blob is stored on flash data pages (os/font.c reads it):

  0   "F84" 0             magic
  4   version (1), cell height (8), cell width (<= 8), 0
  8   N                   number of glyphs (uint16, little endian)
  10  checksum            16-bit sum of all bytes from offset 32 on
  12  offset of lo[]      uint32
  16  offset of wide[]    uint32
  20  offset of glyphs    uint32
  24  reserved
  32  hi[257]             uint16: first glyph with code point >= h * 256
      lo[N]               low byte of each code point (sorted by code point)
      wide[(N + 7) / 8]   bit i: glyph i is full width (else half width)
      glyphs[N][8]        one byte per pixel row, leftmost pixel in bit 7

Only code points up to U+FFFF are kept; glyphs wider than 8 pixels or taller
than the cell are cut.
"""
import struct
import sys


def read_bdf(path):
    glyphs = {}
    asc, desc = 7, 1
    cell_w, cell_h = 8, 8
    lines = open(path, encoding='latin-1').read().split('\n')
    i = 0
    while i < len(lines):
        p = lines[i].split()
        if not p:
            i += 1
            continue
        if p[0] == 'FONTBOUNDINGBOX':
            cell_w, cell_h = int(p[1]), int(p[2])
        elif p[0] == 'FONT_ASCENT':
            asc = int(p[1])
        elif p[0] == 'FONT_DESCENT':
            desc = int(p[1])
        elif p[0] == 'STARTCHAR':
            enc, dw, bbx, rows = None, None, None, []
            i += 1
            while not lines[i].startswith('BITMAP'):
                q = lines[i].split()
                if q[0] == 'ENCODING':
                    enc = int(q[1])
                elif q[0] == 'DWIDTH':
                    dw = int(q[1])
                elif q[0] == 'BBX':
                    bbx = [int(x) for x in q[1:5]]
                i += 1
            i += 1
            while not lines[i].startswith('ENDCHAR'):
                rows.append(lines[i].strip())
                i += 1
            if enc is not None and 0 <= enc <= 0xFFFF and bbx:
                glyphs[enc] = (dw, bbx, rows)
        i += 1
    return glyphs, asc, desc, cell_w, cell_h


def main():
    src, dst = sys.argv[1], sys.argv[2]
    glyphs, asc, desc, cell_w, cell_h = read_bdf(src)
    if cell_h > 8 or cell_w > 8:
        sys.exit('mkfont84: cells larger than 8x8 are not supported')
    cps = sorted(glyphs)
    n = len(cps)
    hi = [0] * 257
    for h in range(257):
        hi[h] = next((k for k, cp in enumerate(cps) if cp >= h * 256), n)
    lo = bytes(cp & 0xFF for cp in cps)
    wide = bytearray((n + 7) // 8)
    rows_out = bytearray()
    for k, cp in enumerate(cps):
        dw, (w, h, xo, yo), rows = glyphs[cp]
        if dw * 2 > cell_w:
            wide[k >> 3] |= 1 << (k & 7)
        cell = [0] * 8
        top = asc - (yo + h)                # first cell row of the bitmap
        for r, hexrow in enumerate(rows):
            y = top + r
            if not 0 <= y < 8:
                continue
            bits = int(hexrow, 16) << (8 - 4 * len(hexrow)) if hexrow else 0
            cell[y] = (bits >> xo) & 0xFF if xo >= 0 else (bits << -xo) & 0xFF
        rows_out += bytes(cell)
    lo_off = 32 + 2 * 257
    wide_off = lo_off + n
    glyph_off = wide_off + len(wide)
    body = struct.pack('<257H', *hi) + lo + bytes(wide) + rows_out
    csum = sum(body) & 0xFFFF
    head = b'F84\0' + bytes([1, 8, cell_w, 0]) + struct.pack('<HHIII', n, csum, lo_off, wide_off,
                                                              glyph_off) + bytes(8)
    assert len(head) == 32
    open(dst, 'wb').write(head + body)
    print('%s: %d glyphs, %d bytes' % (dst, n, len(head) + len(body)))


if __name__ == '__main__':
    main()
