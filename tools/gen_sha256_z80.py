#!/usr/bin/env python3
"""gen_sha256_z80.py OUT.s - SHA-256 compression function in Z80 assembly.

  sha256_block_core():  h[0..7] (uint32_t, little endian, at *sha_h) are
  updated with the 64-byte block at *sha_p.

Appended to the fixed crypto page (after gen_fe_z80.py), so the K table is
mapped while the code runs.  Techniques:

  * the working variables a..h live in an 8-slot block at IX; instead of
    moving them every round, the round code is unrolled 8 times with the
    slot of each variable rotated (new a goes to the old h slot, new e to
    the old d slot);
  * a 32-bit value is held in B, C, D, E and every rotation by a multiple
    of 8 bits is free: the generator only renames which register holds
    which byte;
  * XOR/add accumulation happens in the shadow registers (EXX); HL' walks
    through the K table and IY through the message schedule W[0..63].
"""
import sys

out = []
w = out.append

K = [0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
     0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
     0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
     0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
     0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
     0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
     0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
     0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
     0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
     0xc67178f2]

SV_T = 32          # temporary word in the variable block (ix+32..35)


class Val:
    """a 32-bit value in registers: r[0] holds the least significant byte"""

    def __init__(self, regs):
        self.r = list(regs)

    def rotr8(self, k):
        self.r = [self.r[(i + k) & 3] for i in range(4)]

    def rotr1(self):
        r = self.r
        w('\tld\ta, %s' % r[0])
        w('\trra')
        for i in (3, 2, 1, 0):
            w('\trr\t%s' % r[i])

    def rotl1(self):
        r = self.r
        w('\tld\ta, %s' % r[3])
        w('\trla')
        for i in (0, 1, 2, 3):
            w('\trl\t%s' % r[i])

    def rotr(self, n):
        """rotate right by n bits using byte renaming and at most 4 bit steps"""
        k, b = divmod(n, 8)
        if b > 4:
            k, b = k + 1, b - 8
        self.rotr8(k & 3)
        for _ in range(b):
            self.rotr1()
        for _ in range(-b):
            self.rotl1()

    def shr1(self):
        r = self.r
        w('\tsrl\t%s' % r[3])
        for i in (2, 1, 0):
            w('\trr\t%s' % r[i])

    def shr8(self):
        z = self.r[0]
        self.r = self.r[1:] + [z]
        w('\tld\t%s, #0' % z)

    def load_ix(self, off):
        for i in range(4):
            w('\tld\t%s, %d (ix)' % (self.r[i], off + i))

    def load_iy(self, off):
        for i in range(4):
            w('\tld\t%s, %d (iy)' % (self.r[i], off + i))


MAIN = ('e', 'd', 'c', 'b')


def to_shadow(v):
    """copy the main value into the shadow accumulator (same labels)"""
    w('\tpush\tbc')
    w('\tpush\tde')
    w('\texx')
    w('\tpop\tde')
    w('\tpop\tbc')
    w('\texx')
    return Val(v.r)


def xor_into(acc, v):
    for i in range(4):
        w('\tld\ta, %s' % v.r[i])
        w('\texx')
        w('\txor\ta, %s' % acc.r[i])
        w('\tld\t%s, a' % acc.r[i])
        w('\texx')


def add_main_into(acc, v):
    for i in range(4):
        w('\tld\ta, %s' % v.r[i])
        w('\texx')
        w('\t%s\ta, %s' % ('add' if i == 0 else 'adc', acc.r[i]))
        w('\tld\t%s, a' % acc.r[i])
        w('\texx')


def add_mem_into(acc, idx, off):
    """shadow accumulator += 32-bit word at (idx+off); call with main set active"""
    w('\texx')
    for i in range(4):
        w('\tld\ta, %s' % acc.r[i])
        w('\t%s\ta, %d (%s)' % ('add' if i == 0 else 'adc', off + i, idx))
        w('\tld\t%s, a' % acc.r[i])
    w('\texx')


def add_k_into(acc):
    """shadow accumulator += K[i] at (HL'), HL' += 4"""
    w('\texx')
    for i in range(4):
        w('\tld\ta, %s' % acc.r[i])
        w('\t%s\ta, (hl)' % ('add' if i == 0 else 'adc'))
        w('\tld\t%s, a' % acc.r[i])
        w('\tinc\thl')
    w('\texx')


def big_sigma(var_off, rots):
    """shadow acc = ROTR(x, rots[0]) ^ ROTR(x, rots[1]) ^ ROTR(x, rots[2])"""
    v = Val(MAIN)
    v.load_ix(var_off)
    v.rotr(rots[0])
    acc = to_shadow(v)
    v.rotr(rots[1] - rots[0])
    xor_into(acc, v)
    v.rotr(rots[2] - rots[1])
    xor_into(acc, v)
    return acc


def round_code(j):
    def slot(var):          # var 0..7 = a..h
        return 4 * ((var - j) % 8)
    A, B_, C_, D, E, F, G, H = (slot(i) for i in range(8))
    # T1 = h + S1(e) + Ch(e,f,g) + K[i] + W[i]   (shadow accumulator)
    acc = big_sigma(E, (6, 11, 25))
    add_mem_into(acc, 'ix', H)
    ch = Val(MAIN)
    for i in range(4):
        w('\tld\ta, %d (ix)' % (F + i))
        w('\txor\ta, %d (ix)' % (G + i))
        w('\tand\ta, %d (ix)' % (E + i))
        w('\txor\ta, %d (ix)' % (G + i))
        w('\tld\t%s, a' % ch.r[i])
    add_main_into(acc, ch)
    add_k_into(acc)
    add_mem_into(acc, 'iy', 4 * j)
    # d += T1 ; h = T1
    w('\texx')
    for i in range(4):
        w('\tld\ta, %d (ix)' % (D + i))
        w('\t%s\ta, %s' % ('add' if i == 0 else 'adc', acc.r[i]))
        w('\tld\t%d (ix), a' % (D + i))
    for i in range(4):
        w('\tld\t%d (ix), %s' % (H + i, acc.r[i]))
    w('\texx')
    # T2 = S0(a) + Maj(a,b,c) ; h += T2  (h slot becomes the new a)
    acc = big_sigma(A, (2, 13, 22))
    maj = Val(MAIN)
    for i in range(4):
        w('\tld\ta, %d (ix)' % (A + i))
        w('\tld\tl, a')
        w('\tand\ta, %d (ix)' % (B_ + i))
        w('\tld\th, a')
        w('\tld\ta, l')
        w('\tor\ta, %d (ix)' % (B_ + i))
        w('\tand\ta, %d (ix)' % (C_ + i))
        w('\tor\ta, h')
        w('\tld\t%s, a' % maj.r[i])
    add_main_into(acc, maj)
    add_mem_into(acc, 'ix', H)
    w('\texx')
    for i in range(4):
        w('\tld\t%d (ix), %s' % (H + i, acc.r[i]))
    w('\texx')


def small_sigma(off, r1, r2, s):
    """shadow acc = ROTR(x, r1) ^ ROTR(x, r2) ^ (x >> s), x at (iy+off)"""
    v = Val(MAIN)
    v.load_iy(off)
    v.rotr(r1)
    acc = to_shadow(v)
    v.rotr(r2 - r1)
    xor_into(acc, v)
    v = Val(MAIN)
    v.load_iy(off)
    for _ in range(s // 8):
        v.shr8()
    for _ in range(s % 8):
        v.shr1()
    xor_into(acc, v)
    return acc


w(';; generated by tools/gen_sha256_z80.py - do not edit')
w('\t.globl _sha_h, _sha_p, _sha256_block_core')
w('\t.area _DATA')
w('sha_v:\t.ds 36')            # a..h slots + T
w('sha_w:\t.ds 256')
w('sha_n:\t.ds 1')
w('\t.area _CODE')
# an equate, not a label: banker.py treats code labels as functions and
# would route "ld hl, #label" through a far-call thunk
w('sha_k = .')
for k in range(0, 64, 4):
    w('\t.db ' + ', '.join('0x%02X' % ((K[i] >> (8 * b)) & 0xFF)
                          for i in range(k, k + 4) for b in range(4)))

w('_sha256_block_core::')
w('\tpush\tix')
w('\tpush\tiy')
# h -> variable block
w('\tld\thl, (_sha_h)')
w('\tld\tde, #sha_v')
w('\tld\tbc, #32')
w('\tldir')
# message (big endian) -> W[0..15] (little endian)
w('\tld\thl, (_sha_p)')
w('\tld\tde, #sha_w + 3')
w('\tld\tb, #16')
w('00090$:')
for i in range(4):
    w('\tld\ta, (hl)')
    w('\tld\t(de), a')
    w('\tinc\thl')
    if i < 3:
        w('\tdec\tde')
w('\tinc\tde')
w('\tinc\tde')
w('\tinc\tde')
w('\tinc\tde')
w('\tinc\tde')
w('\tinc\tde')
w('\tinc\tde')
w('\tdjnz\t00090$')
# schedule: W[i] = s1(W[i-2]) + W[i-7] + s0(W[i-15]) + W[i-16], i = 16..63
w('\tld\tiy, #sha_w + 64')
w('\tld\ta, #48')
w('00091$:')
w('\tld\t(sha_n), a')
acc = small_sigma(-8, 17, 19, 10)
w('\texx')
for i in range(4):
    w('\tld\t%d (iy), %s' % (i, acc.r[i]))
w('\texx')
acc = small_sigma(-60, 7, 18, 3)
add_mem_into(acc, 'iy', 0)
add_mem_into(acc, 'iy', -28)
add_mem_into(acc, 'iy', -64)
w('\texx')
for i in range(4):
    w('\tld\t%d (iy), %s' % (i, acc.r[i]))
w('\texx')
w('\tld\tbc, #4')
w('\tadd\tiy, bc')
w('\tld\ta, (sha_n)')
w('\tdec\ta')
w('\tjp\tnz, 00091$')
# 64 rounds, 8 per loop iteration
w('\tld\tix, #sha_v')
w('\tld\tiy, #sha_w')
w('\texx')
w('\tld\thl, #sha_k')
w('\texx')
w('\tld\ta, #8')
w('00092$:')
w('\tld\t(sha_n), a')
for j in range(8):
    round_code(j)
w('\tld\tbc, #32')
w('\tadd\tiy, bc')
w('\tld\ta, (sha_n)')
w('\tdec\ta')
w('\tjp\tnz, 00092$')
# h[k] += slot k
w('\tld\thl, (_sha_h)')
w('\tld\tde, #sha_v')
w('\tld\tb, #8')
w('00093$:')
w('\tor\ta, a')
for i in range(4):
    w('\tld\ta, (de)')
    w('\tadc\ta, (hl)')
    w('\tld\t(hl), a')
    w('\tinc\thl')
    w('\tinc\tde')
w('\tdjnz\t00093$')
w('\tpop\tiy')
w('\tpop\tix')
w('\tret')

open(sys.argv[1], 'w').write('\n'.join(out) + '\n')
