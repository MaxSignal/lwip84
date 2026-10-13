#!/usr/bin/env python3
"""mkfont84.py LIST OUT.bin - build the external font blob from the fonts
named in LIST (fonts/fonts.txt), in priority order: the first font that has
a code point gives its glyph.  LIST lines:

  bdf     FILE [RANGES]     BDF bitmap font, cells up to 8x8
  unscii  FILE [RANGES]     unscii-style text file (U+XXXX, 8x8 picture)
  unifont FILE [RANGES]     GNU Unifont .hex, 16 pixels high, halved to 8x8

RANGES: XXXX-YYYY[:filter] ... (filter gb2312+big5 keeps only the Han
characters of GB 2312 and Big5 level 1); without ranges everything.
Paths are relative to LIST.

The blob is stored on flash data pages (os/font.c reads it):

  0   "F84" 0             magic
  4   version (2), cell height (8), cell width (8), 0
  8   N                   number of glyphs (uint16, little endian)
  10  checksum            16-bit sum of all bytes from offset 32 on
  12  offset of lo[]      uint32
  16  offset of kind[]    uint32
  20  offset of glyphs    uint32
  24  reserved
  32  hi[257]             uint16: first glyph with code point >= h * 256
      lo[N]               low byte of each code point (sorted by code point)
      kind[(N + 3) / 4]   2 bits per glyph: 0 half width, 1 full width,
                          2 mark (drawn over the glyph before it)
      glyphs[N][8]        one byte per pixel row, leftmost pixel in bit 7

At most 16575 glyphs (os/font.h numbers them with two bytes).
"""
import gzip
import os
import struct
import sys
import unicodedata

MAX_GLYPHS = 65 * 255
HALF, FULL, MARK = 0, 1, 2


def cjk_filter_sets():
    """Han characters of GB 2312 and of Big5 level 1"""
    s = set()
    for b1 in range(0xB0, 0xF8):
        for b2 in range(0xA1, 0xFF):
            try:
                s.add(ord(bytes([b1, b2]).decode('gb2312')))
            except UnicodeDecodeError:
                pass
    for b1 in range(0xA4, 0xC7):
        for b2 in list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF)):
            try:
                c = bytes([b1, b2]).decode('big5')
            except UnicodeDecodeError:
                continue
            if '一' <= c <= '鿿':
                s.add(ord(c))
    return s


def parse_ranges(specs):
    out = []
    for sp in specs:
        rng, _, flt = sp.partition(':')
        a, _, b = rng.partition('-')
        out.append((int(a, 16), int(b or a, 16), flt))
    return out


def wanted(cp, ranges, filters):
    if not ranges:
        return True
    for a, b, flt in ranges:
        if a <= cp <= b:
            return not flt or cp in filters[flt]
    return False


def kind_of(cp, width, cell_w):
    """marks: Unicode combining characters (Mn, Me) are drawn over the
    glyph before them; so are the Devanagari vowel signs that follow their
    consonant (Mc except i, which is moved in front of it): halved from
    Unifont their stroke sits at the right edge, where the consonant ends"""
    cat = unicodedata.category(chr(cp))
    if cat in ('Mn', 'Me') or (cat == 'Mc' and 0x0900 <= cp <= 0x097F and cp != 0x093F):
        return MARK
    return FULL if width * 2 > cell_w else HALF


def read_bdf(path):
    """{cp: (rows[8], width)}"""
    glyphs = {}
    asc = 7
    lines = open(path, encoding='latin-1').read().split('\n')
    i = 0
    while i < len(lines):
        p = lines[i].split()
        if p and p[0] == 'FONT_ASCENT':
            asc = int(p[1])
        if p and p[0] == 'STARTCHAR':
            enc, dw, bbx, rows = None, None, None, []
            i += 1
            while not lines[i].startswith('BITMAP'):
                q = lines[i].split()
                if q and q[0] == 'ENCODING':
                    enc = int(q[1])
                elif q and q[0] == 'DWIDTH':
                    dw = int(q[1])
                elif q and q[0] == 'BBX':
                    bbx = [int(x) for x in q[1:5]]
                i += 1
            i += 1
            while not lines[i].startswith('ENDCHAR'):
                rows.append(lines[i].strip())
                i += 1
            if enc is not None and 0 <= enc <= 0xFFFF and bbx:
                w, h, xo, yo = bbx
                cell = [0] * 8
                top = asc - (yo + h)
                for r, hexrow in enumerate(rows):
                    y = top + r
                    if not 0 <= y < 8 or not hexrow:
                        continue
                    bits = int(hexrow, 16) << max(0, 8 - 4 * len(hexrow))
                    bits >>= max(0, 4 * len(hexrow) - 8)
                    cell[y] = (bits >> xo) & 0xFF if xo >= 0 else (bits << -xo) & 0xFF
                glyphs[enc] = (cell, dw if dw is not None else w)
        i += 1
    return glyphs


def read_unscii(path):
    """unscii text: "U+XXXX" (or "U+XXXX = YYYY" for the same picture), then
    "8x8" and eight rows of '.' and '#'"""
    glyphs, alias = {}, {}
    lines = open(path, encoding='utf-8').read().split('\n')
    cur = None
    i = 0
    while i < len(lines):
        ln = lines[i].strip()
        if ln.startswith('U+'):
            parts = ln.split('=')
            try:
                cur = int(parts[0].strip()[2:], 16)
            except ValueError:          # "U+IGNORE" and other notes
                cur = None
                i += 1
                continue
            if len(parts) > 1:
                alias[cur] = int(parts[1].strip().split()[0], 16)
                cur = None
        elif ln == '8x8' and cur is not None:
            rows = []
            for r in range(8):
                pic = lines[i + 1 + r][:8]
                rows.append(sum(1 << (7 - x) for x, ch in enumerate(pic) if ch == '#'))
            glyphs[cur] = (rows, 8)
            i += 8
        i += 1
    for _ in range(4):                  # aliases of aliases
        for cp, src in alias.items():
            if cp not in glyphs and src in glyphs:
                glyphs[cp] = glyphs[src]
    return glyphs


# Arabic contextual forms (Unicode presentation forms B): isolated, final,
# initial, medial; letters that join only to the right have two
ARABIC_FORMS = {
    0x0621: (0xFE80,), 0x0622: (0xFE81, 0xFE82), 0x0623: (0xFE83, 0xFE84),
    0x0624: (0xFE85, 0xFE86), 0x0625: (0xFE87, 0xFE88), 0x0626: (0xFE89, 0xFE8A, 0xFE8B, 0xFE8C),
    0x0627: (0xFE8D, 0xFE8E), 0x0628: (0xFE8F, 0xFE90, 0xFE91, 0xFE92),
    0x0629: (0xFE93, 0xFE94), 0x062A: (0xFE95, 0xFE96, 0xFE97, 0xFE98),
    0x062B: (0xFE99, 0xFE9A, 0xFE9B, 0xFE9C), 0x062C: (0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0),
    0x062D: (0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4), 0x062E: (0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8),
    0x062F: (0xFEA9, 0xFEAA), 0x0630: (0xFEAB, 0xFEAC), 0x0631: (0xFEAD, 0xFEAE),
    0x0632: (0xFEAF, 0xFEB0), 0x0633: (0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4),
    0x0634: (0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8), 0x0635: (0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC),
    0x0636: (0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0), 0x0637: (0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4),
    0x0638: (0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8), 0x0639: (0xFEC9, 0xFECA, 0xFECB, 0xFECC),
    0x063A: (0xFECD, 0xFECE, 0xFECF, 0xFED0), 0x0641: (0xFED1, 0xFED2, 0xFED3, 0xFED4),
    0x0642: (0xFED5, 0xFED6, 0xFED7, 0xFED8), 0x0643: (0xFED9, 0xFEDA, 0xFEDB, 0xFEDC),
    0x0644: (0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0), 0x0645: (0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4),
    0x0646: (0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8), 0x0647: (0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC),
    0x0648: (0xFEED, 0xFEEE), 0x0649: (0xFEEF, 0xFEF0), 0x064A: (0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4),
}

DOT_ABOVE = [0x18, 0, 0, 0, 0, 0, 0, 0]
HAMZA_ABOVE = [0x30, 0x40, 0x70, 0, 0, 0, 0, 0]
HAMZA_BELOW = [0, 0, 0, 0, 0, 0, 0x30, 0x40]
MADDA = [0x78, 0, 0, 0, 0, 0, 0, 0]
HAMZA_ALONE = [0, 0, 0x30, 0x40, 0x70, 0x40, 0, 0]


def arabic_complete(g):
    """letters and forms that unscii lacks: from similar ones plus a dot or
    hamza; a missing form falls back to the letter itself"""
    def over(src, mark):
        if src not in g:
            return None
        rows, w = g[src]
        return ([r | m for r, m in zip(rows, mark)], w)

    derive = [
        (0x062E, 0x062D, DOT_ABOVE), (0xFEA5, 0xFEA1, DOT_ABOVE), (0xFEA6, 0xFEA2, DOT_ABOVE),
        (0xFEA7, 0xFEA3, DOT_ABOVE), (0xFEA8, 0xFEA4, DOT_ABOVE),
        (0x0636, 0x0635, DOT_ABOVE), (0xFEBD, 0xFEB9, DOT_ABOVE), (0xFEBE, 0xFEBA, DOT_ABOVE),
        (0xFEBF, 0xFEBB, DOT_ABOVE), (0xFEC0, 0xFEBC, DOT_ABOVE),
        (0x0622, 0x0627, MADDA), (0x0625, 0x0627, HAMZA_BELOW), (0x0624, 0x0648, HAMZA_ABOVE),
        (0x0626, 0x0649, HAMZA_ABOVE), (0xFE8B, 0xFEF3, HAMZA_ABOVE), (0xFE8C, 0xFEF4, HAMZA_ABOVE),
        (0x0621, None, HAMZA_ALONE),
    ]
    for dst, src, mark in derive:
        if dst in g:
            continue
        if src is None:
            g[dst] = (list(mark), 8)
        else:
            r = over(src, mark)
            if r:
                g[dst] = r
    for base, forms in ARABIC_FORMS.items():
        for k, f in enumerate(forms):
            if f in g or base not in g:
                continue
            # the final/medial of a letter: the isolated/initial one if known
            alt = forms[k - 1] if k in (1, 3) and forms[k - 1] in g else base
            g[f] = g[alt]
    return g


def read_unifont(path):
    """16 pixels high: every two rows and two columns become one pixel when
    any of the four is set; glyphs are moved up so that the Devanagari
    head line lands on row 1"""
    glyphs = {}
    for ln in open(path, encoding='ascii'):
        if ln.startswith('#') or ':' not in ln:
            continue
        cp, h = ln.strip().split(':')
        cp = int(cp, 16)
        bpr = len(h) // 16              # hex digits per row
        w = bpr * 4
        rows16 = [int(h[r * bpr:(r + 1) * bpr], 16) for r in range(16)]
        rows8 = []
        for y in range(8):
            v = 0
            for x in range(w // 2):
                for yy in (2 * y, 2 * y + 1):
                    for xx in (2 * x, 2 * x + 1):
                        if (rows16[yy] >> (w - 1 - xx)) & 1:
                            v |= 1 << (7 - x) if x < 8 else 0
            rows8.append(v & 0xFF)
        glyphs[cp] = (rows8, w // 2)
    return glyphs


def main():
    lst, dst = sys.argv[1], sys.argv[2]
    base = os.path.dirname(lst)
    filters = {}
    chosen = {}                         # cp -> (rows, kind)
    for ln in open(lst, encoding='utf-8'):
        p = ln.split('#')[0].split()
        if not p:
            continue
        typ, fn, ranges = p[0], os.path.join(base, p[1]), parse_ranges(p[2:])
        for _, _, flt in ranges:
            if flt and flt not in filters:
                if flt != 'gb2312+big5':
                    sys.exit('mkfont84: unknown filter ' + flt)
                filters[flt] = cjk_filter_sets()
        g = {'bdf': read_bdf, 'unscii': read_unscii, 'unifont': read_unifont}[typ](fn)
        if any(a <= 0x0627 <= b for a, b, _ in ranges) or (not ranges and 0x0627 in g):
            g = arabic_complete(g)
        cell_w = 8 if typ != 'bdf' else max(w for _, w in g.values() if w <= 8) or 8
        n_new = 0
        for cp, (rows, w) in sorted(g.items()):
            if cp in chosen or not wanted(cp, ranges, filters):
                continue
            chosen[cp] = (rows, kind_of(cp, w, 8 if typ != 'bdf' else cell_w))
            n_new += 1
        print('%s: %d glyphs' % (p[1], n_new))
    cps = sorted(chosen)
    n = len(cps)
    if n > MAX_GLYPHS:
        sys.exit('mkfont84: %d glyphs, at most %d' % (n, MAX_GLYPHS))
    hi = []
    k = 0
    for h in range(257):
        while k < n and cps[k] < h * 256:
            k += 1
        hi.append(k)
    lo = bytes(cp & 0xFF for cp in cps)
    kinds = bytearray((n + 3) // 4)
    rows_out = bytearray()
    for k, cp in enumerate(cps):
        rows, kind = chosen[cp]
        kinds[k >> 2] |= kind << (2 * (k & 3))
        rows_out += bytes(rows)
    lo_off = 32 + 2 * 257
    kind_off = lo_off + n
    glyph_off = kind_off + len(kinds)
    body = struct.pack('<257H', *hi) + lo + bytes(kinds) + rows_out
    csum = sum(body) & 0xFFFF
    head = b'F84\0' + bytes([2, 8, 8, 0]) + struct.pack('<HHIII', n, csum, lo_off, kind_off,
                                                        glyph_off) + bytes(8)
    open(dst, 'wb').write(head + body)
    print('%s: %d glyphs, %d bytes (%d flash pages)' % (dst, n, len(head) + len(body),
                                                      (len(head) + len(body) + 0x3FFF) // 0x4000))


if __name__ == '__main__':
    main()
