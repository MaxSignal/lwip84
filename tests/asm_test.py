#!/usr/bin/env python3
"""asm_test.py - check the generated Z80 crypto routines against Python
references by running them in tests/z80run (built from tests/z80run.c).

  asm_test.py sha256 [N]
"""
import os, random, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TI84P = os.path.dirname(HERE)
SDCC = os.path.abspath(os.path.join(TI84P, '../../tools/sdcc-4.4.0/bin'))
RUN = os.environ.get('Z80RUN', os.path.abspath(os.path.join(TI84P, '../../tmp/z80run')))
TMP = os.environ.get('TMPDIR', os.path.abspath(os.path.join(TI84P, '../../tmp')))


def build(gen, stub):
    d = tempfile.mkdtemp(dir=TMP)
    subprocess.check_call([sys.executable, os.path.join(TI84P, 'tools', gen), d + '/mod.s'])
    open(d + '/stub.s', 'w').write(stub)
    for m in ('stub', 'mod'):
        subprocess.check_call([SDCC + '/sdasz80', '-plosgff', d + '/%s.rel' % m, d + '/%s.s' % m])
    subprocess.check_call([SDCC + '/sdldz80', '-n', '-m', '-i', '-b', '_CODE=0x4000', '-b', '_DATA=0x8000',
                           d + '/t.ihx', d + '/stub.rel', d + '/mod.rel'], stdout=subprocess.DEVNULL)
    start = None
    for l in open(d + '/t.map'):
        p = l.split()
        if len(p) >= 2 and p[1] == 'start':
            start = int(p[0], 16)
    return d, start


def run(d, start, blobs, dump):
    args = [RUN, d + '/t.ihx', str(start)]
    for i, (addr, data) in enumerate(blobs):
        fn = d + '/in%d.bin' % i
        open(fn, 'wb').write(data)
        args.append('%s@%d' % (fn, addr))
    args += ['--', str(dump[0]), str(dump[1]), d + '/out.bin']
    t = int(subprocess.check_output(args))
    return open(d + '/out.bin', 'rb').read(), t


K = [0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
     0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
     0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
     0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
     0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
     0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
     0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
     0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2]
M = 0xFFFFFFFF


def ror(x, n):
    return ((x >> n) | (x << (32 - n))) & M


def compress(h, blk):
    w = list(struct.unpack('>16I', blk))
    for i in range(16, 64):
        s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3)
        s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10)
        w.append((w[i - 16] + s0 + w[i - 7] + s1) & M)
    a, b, c, d, e, f, g, hh = h
    for i in range(64):
        t1 = (hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i]) & M
        t2 = ((ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c))) & M
        hh, g, f, e, d, c, b, a = g, f, e, (d + t1) & M, c, b, a, (t1 + t2) & M
    return [(x + y) & M for x, y in zip(h, (a, b, c, d, e, f, g, hh))]


SHA_STUB = """\t.module stub
\t.globl _sha_h, _sha_p, _sha256_block_core
\t.area _DATA
_sha_h:: .ds 2
_sha_p:: .ds 2
\t.area _CODE
start::
\tld hl, #0xE000
\tld (_sha_h), hl
\tld hl, #0xE100
\tld (_sha_p), hl
\tcall _sha256_block_core
\thalt
"""


def test_sha256(n):
    d, start = build('gen_sha256_z80.py', SHA_STUB)
    rnd = random.Random(1)
    for k in range(n):
        h = [rnd.getrandbits(32) for _ in range(8)]
        blk = bytes(rnd.getrandbits(8) for _ in range(64))
        got, t = run(d, start, [(0xE000, struct.pack('<8I', *h)), (0xE100, blk)], (0xE000, 32))
        want = struct.pack('<8I', *compress(h, blk))
        if got != want:
            print('sha256 FAIL', k, got.hex(), want.hex())
            return 1
    print('sha256 ok, %d blocks, %d T-states per block (%.1f ms at 15 MHz, %.1f KB/s)'
          % (n, t, t / 15000.0, 64 / (t / 15e6) / 1024))
    return 0


if __name__ == '__main__':
    what = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    sys.exit({'sha256': test_sha256}[what](n))
