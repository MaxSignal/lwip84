#!/usr/bin/env python3
"""mkjis84.py OUT.bin - the Shift_JIS table for the browser (app/browser/html.c):
uint16 code points (little endian, 0 = none) of the two-byte codes of
Windows Shift_JIS (cp932), indexed by lead * 188 + trail, where lead
counts 81h-9Fh, E0h-FCh (0-59) and trail 40h-7Eh, 80h-FCh (0-187).
EUC-JP is turned into Shift_JIS first.  Characters outside the BMP are 0."""
import struct
import sys

LEADS = list(range(0x81, 0xA0)) + list(range(0xE0, 0xFD))
TRAILS = list(range(0x40, 0x7F)) + list(range(0x80, 0xFD))
out = bytearray()
n = 0
for lead in LEADS:
    for trail in TRAILS:
        try:
            s = bytes([lead, trail]).decode('cp932')
            cp = ord(s) if len(s) == 1 and ord(s) < 0x10000 else 0
        except UnicodeDecodeError:
            cp = 0
        n += cp != 0
        out += struct.pack('<H', cp)
open(sys.argv[1], 'wb').write(out)
print('%s: %d characters, %d bytes' % (sys.argv[1], n, len(out)))
