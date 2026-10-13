#!/usr/bin/env python3
"""mkzips.py DIR - ZIPs for inflate_test: stored and deflated members
(fixed and dynamic Huffman codes, stored blocks, long matches, an empty
member, data that does not compress), each at most 32 KiB."""
import os
import random
import sys
import zipfile

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
rnd = random.Random(84)
text = open(__file__, 'rb').read()
members = {
    'empty.8xp': b'',
    'short.txt': b'lwIP-84+',                       # a fixed-code block
    'text.txt': text * 8,
    'runs.bin': b'\0' * 20000 + b'\xff' * 9000,     # matches of 258 at distance 1
    'random.bin': bytes(rnd.getrandbits(8) for _ in range(12000)),  # stored blocks
    'mixed.bin': b''.join(bytes(rnd.getrandbits(3) for _ in range(rnd.randrange(1, 60))) + text[:rnd.randrange(300)]
                          for _ in range(200))[:32000],
}
for level in (0, 1, 6, 9):
    with zipfile.ZipFile(os.path.join(out, 'l%d.zip' % level), 'w',
                         zipfile.ZIP_STORED if level == 0 else zipfile.ZIP_DEFLATED,
                         **({} if level == 0 else {'compresslevel': level})) as z:
        for name, data in members.items():
            z.writestr(name, data)
