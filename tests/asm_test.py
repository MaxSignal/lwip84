#!/usr/bin/env python3
"""asm_test.py - check the generated Z80 crypto routines against Python
references by running them in tests/z80run (built from tests/z80run.c).

  asm_test.py sha256 [N]
"""
import os, random, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TI84P = os.path.dirname(HERE)
SDCC = os.environ.get('SDCC_BIN', os.path.abspath(os.path.join(TI84P, '../tools/sdcc-4.4.0/bin')))
RUN = os.environ.get('Z80RUN', os.path.abspath(os.path.join(TI84P, '../tmp/z80run')))
TMP = os.environ.get('TMPDIR', os.path.abspath(os.path.join(TI84P, '../tmp')))


def build(gen, stub, args=()):
    d = tempfile.mkdtemp(dir=TMP)
    subprocess.check_call([sys.executable, os.path.join(TI84P, 'tools', gen), d + '/mod.s'] + list(args))
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


P13_STUB = """\t.module stub
\t.globl _p13_h, _p13_r, _p13_m, _p13_hb, _poly1305_block_core
\t.area _DATA
_p13_h:: .ds 2
_p13_r:: .ds 2
_p13_m:: .ds 2
_p13_hb:: .ds 1
\t.area _CODE
start::
\tld hl, #0xE000
\tld (_p13_h), hl
\tld hl, #0xE020
\tld (_p13_r), hl
\tld hl, #0xE040
\tld (_p13_m), hl
\tld a, (0xE060)
\tld (_p13_hb), a
\tld a, (0xE061)
\tld b, a
00001$:
\tpush bc
\tcall _poly1305_block_core
\tpop bc
\tdjnz 00001$
\thalt
"""

P = (1 << 130) - 5


def test_poly1305(n):
    d, start = build('gen_poly1305_z80.py', P13_STUB)
    rnd = random.Random(2)
    for k in range(n):
        r = int.from_bytes(bytes(rnd.getrandbits(8) for _ in range(16)), 'little')
        r &= 0x0ffffffc0ffffffc0ffffffc0fffffff
        h = rnd.getrandbits(130) if k % 3 else 0
        m = bytes(rnd.getrandbits(8) for _ in range(16))
        hb = 1 if k % 4 else 0
        reps = 1 + k % 3
        want = h
        for _ in range(reps):
            want = ((want + int.from_bytes(m, 'little') + (hb << 128)) * r) % P
        blob = h.to_bytes(17, 'little') + bytes(15) + r.to_bytes(17, 'little') + bytes(15) + m + bytes(16) + bytes([hb, reps])
        got, t = run(d, start, [(0xE000, blob)], (0xE000, 17))
        gv = int.from_bytes(got, 'little')
        if gv % P != want or gv >= 1 << 131:
            print('poly1305 FAIL', k, hex(gv), hex(want))
            return 1
    print('poly1305 ok, %d cases, %d T-states for the last (%d blocks, first builds the table)' % (n, t, reps))
    return 0


AES_STUB = """\t.module stub
\t.globl _aes_rk, _aes_in, _aes_out, _aes_xor, _aes_encrypt_core
\t.area _CODE
start::
\tld hl, #0xE000
\tld (_aes_rk), hl
\tld hl, #0xE100
\tld (_aes_in), hl
\tld hl, #0xE200
\tld (_aes_out), hl
\tld a, (0xE300)
\tld (_aes_xor), a
\tcall _aes_encrypt_core
\thalt
"""


def aes_ref(key, blk):
    """AES-128 encryption of one block (FIPS 197), plain Python"""
    sys.path.insert(0, os.path.join(TI84P, 'tools'))
    from gen_aes_z80 import sbox
    sb = sbox()
    xt = lambda x: ((x << 1) ^ 0x11B) if x & 0x80 else x << 1
    rk = list(key)
    rcon = 1
    for i in range(16, 176, 4):
        t = rk[i - 4:i]
        if i % 16 == 0:
            t = [sb[t[1]] ^ rcon, sb[t[2]], sb[t[3]], sb[t[0]]]
            rcon = xt(rcon)
        rk += [rk[i - 16 + j] ^ t[j] for j in range(4)]
    s = [b ^ k for b, k in zip(blk, rk[:16])]
    for r in range(1, 11):
        t = [sb[s[i % 4 + 4 * ((i // 4 + i % 4) % 4)]] for i in range(16)]
        if r < 10:
            s = []
            for c in range(4):
                a = t[4 * c:4 * c + 4]
                x = a[0] ^ a[1] ^ a[2] ^ a[3]
                s += [a[i] ^ x ^ xt(a[i] ^ a[(i + 1) % 4]) for i in range(4)]
        else:
            s = t
        s = [b ^ k for b, k in zip(s, rk[16 * r:16 * r + 16])]
    return bytes(s), bytes(rk)


def test_aes(n):
    sys.path.insert(0, os.path.join(TI84P, 'tools'))
    from gen_aes_z80 import sbox
    d, start = build('gen_aes_z80.py', AES_STUB, ['0xD000'])
    rnd = random.Random(3)
    # FIPS 197 appendix C.1
    vectors = [(bytes(range(16)), bytes.fromhex('00112233445566778899aabbccddeeff'))]
    vectors += [(bytes(rnd.getrandbits(8) for _ in range(16)), bytes(rnd.getrandbits(8) for _ in range(16)))
                for _ in range(n)]
    for k, (key, blk) in enumerate(vectors):
        want, rk = aes_ref(key, blk)
        if k == 0 and want.hex() != '69c4e0d86a7b0430d8cdb78070b4c55a':
            print('aes reference FAIL')
            return 1
        xor = k % 2
        old = bytes(rnd.getrandbits(8) for _ in range(16))
        got, t = run(d, start, [(0xD000, bytes(sbox())), (0xE000, rk), (0xE100, blk),
                                (0xE200, old), (0xE300, bytes([xor]))], (0xE200, 16))
        exp = bytes(a ^ b for a, b in zip(want, old)) if xor else want
        if got != exp:
            print('aes FAIL', k, got.hex(), exp.hex())
            return 1
    print('aes ok, %d blocks, %d T-states per block (%.1f KB/s at 15 MHz)'
          % (len(vectors), t, 16 / (t / 15e6) / 1024))
    return 0


GH_STUB = """\t.module stub
\t.globl _gh_x, _gh_m, _ghash_mul_core
\t.area _CODE
start::
\tld hl, #0xE000
\tld (_gh_x), hl
\tld hl, #0xE100
\tld (_gh_m), hl
\tcall _ghash_mul_core
\thalt
"""


def gf_mul(x, y):
    """GF(2^128) product of SP 800-38D (bit 0 is the most significant)"""
    R = 0xE1 << 120
    z, v = 0, y
    for i in range(127, -1, -1):
        if (x >> i) & 1:
            z ^= v
        v = (v >> 1) ^ R if v & 1 else v >> 1
    return z


def test_ghash(n):
    d, start = build('gen_aes_z80.py', GH_STUB, ['0xD000'])
    rnd = random.Random(4)
    for k in range(n):
        h = rnd.getrandbits(128)
        x = rnd.getrandbits(128) if k else 0
        # m[i] = i * H with the nibble bits as x^0..x^3 of the 4-bit polynomial
        m = b''.join(gf_mul(i << 124, h).to_bytes(16, 'big') for i in range(16))
        got, t = run(d, start, [(0xE000, x.to_bytes(16, 'big')), (0xE100, m)], (0xE000, 16))
        want = gf_mul(x, h).to_bytes(16, 'big')
        if got != want:
            print('ghash FAIL', k, got.hex(), want.hex())
            return 1
    print('ghash ok, %d products, %d T-states per block (%.1f KB/s at 15 MHz)'
          % (n, t, 16 / (t / 15e6) / 1024))
    return 0


if __name__ == '__main__':
    what = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    sys.exit({'sha256': test_sha256, 'poly1305': test_poly1305, 'aes': test_aes, 'ghash': test_ghash}[what](n))
