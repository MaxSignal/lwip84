#!/usr/bin/env python3
"""check_iy.py FILE.asm... - find an SDCC 4.4.0 code generation bug: IY is
loaded with a pointer (ld iy, ... / add iy, ...) and loaded again before
that value was used, e.g.

    ld iy, (_hs) / ld de, #0x3d2 / add iy, de    ; iy = hs->cap
    ld iy, #_cap_n / inc 0 (iy) / push iy / pop hl ; hs->cap lost

for `hs->cap[cap_n++] = c`.  Exit status 1 if any is found."""
import re
import sys

SET = re.compile(r'^\s*(ld\s+iy\s*,|add\s+iy\s*,|pop\s+iy\b)', re.I)
USE = re.compile(r'\(iy\)|\biy\b', re.I)
found = 0
for fn in sys.argv[1:]:
    pending = None
    for n, line in enumerate(open(fn), 1):
        code = line.split(';')[0]
        if not code.strip():
            continue
        if re.match(r'^\s*[\w$]+:', code) or re.match(r'^\s*(jp|jr|call|ret|djnz)\b', code):
            pending = None              # control flow: stop tracking
            continue
        m = SET.match(code)
        if m:
            reads_iy = 'add' in m.group(1).lower() or re.search(r'\(iy\)|,\s*iy\b', code[m.end():])
            if pending and not reads_iy and pending[1] == 'add':
                print('%s:%d: IY from line %d overwritten before use' % (fn, n, pending[0]))
                found += 1
            pending = (n, 'add' if m.group(1).lower().startswith('add') else 'ld')
            continue
        if USE.search(code):
            pending = None
sys.exit(1 if found else 0)
