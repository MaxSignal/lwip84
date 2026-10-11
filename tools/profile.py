#!/usr/bin/env python3
"""profile.py MAPFILE PROFILE - attribute emu84 PC samples to functions."""
import re, sys, collections
syms = []
for l in open(sys.argv[1]):
    for m in re.finditer(r'\b([0-9A-F]{8})\s+(_\S+|___\S+)', l):
        v = int(m.group(1), 16)
        if (v & 0xFFFF) < 0x8000:
            syms.append((v, m.group(2)))
syms.sort()
import bisect
keys = [s[0] for s in syms]
tot = collections.Counter()
n = 0
fc = collections.Counter()
for l in open(sys.argv[2]):
    if l.startswith('F '):
        _, a, c = l.split()
        bank, pc = a.split(':')
        addr = int(bank, 16) << 16 | int(pc, 16)
        i = bisect.bisect_right(keys, addr) - 1
        fc[syms[i][1] if i >= 0 else '?'] += int(c)
        continue
    if l.startswith('#'):
        print(l.strip())
        continue
    a, c = l.split()
    bank, pc = a.split(':')
    bank, pc, c = int(bank, 16), int(pc, 16), int(c)
    addr = (bank << 16 | pc) if pc >= 0x4000 else pc
    i = bisect.bisect_right(keys, addr) - 1
    name = syms[i][1] if i >= 0 else '?'
    if pc >= 0x4000 and (syms[i][0] >> 16) != bank:
        name = '?bank%02X' % bank
    tot[name] += c
    n += c
for name, c in tot.most_common(30):
    print('%6.2f%%  %s' % (100.0 * c / n, name))
if fc:
    print('far calls: %d' % sum(fc.values()))
    for name, c in fc.most_common(25):
        print('%8d  %s' % (c, name))
