#!/usr/bin/env python3
"""banker.py - automatic code banking for SDCC/Z80 on the TI-84 Plus.

The TI-84 Plus maps flash page 0 at 0000h-3FFFh (always present) and one
switchable flash page ("bank A", port 06h) at 4000h-7FFFh.  This tool takes
the assembler output of SDCC, places every *banked* module into one of the
flash pages 1..N (first-fit by size) and rewrites references so that

  * calls/jumps to a function in the same page stay direct;
  * calls from another page, and every place where a function address is
    taken (function pointers, tables), go through a small thunk in page 0.

A thunk switches bank A, calls the real function and switches back.  The
return bookkeeping lives on a separate "far stack" (see os/farcall.s), so
the callee sees exactly the stack layout of a direct call and normal SDCC
calling conventions (register and stack arguments) keep working.

Constant data is expected to be compiled with --constseg CONST and is kept
in page 0, so pointers to constants stay valid from every bank.

Usage:
  banker.py --outdir DIR --thunks DIR/thunks.s [--map map.txt]
            [--bank-size N] [--first-bank N] [--max-bank N]
            common:FILE.asm ... banked:FILE.asm ... [pin:BANK:FILE.asm]
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

LABEL_RE = re.compile(r'^([A-Za-z_.$][\w.$]*)(::?)(.*)$')
AREA_RE = re.compile(r'^\s*\.area\s+(\w+)')
IDENT_RE = re.compile(r'[A-Za-z_.$][\w.$]*')
DIRECT_MNEMONICS = ('call', 'jp')


class Module:
    def __init__(self, path, kind, pin=None):
        self.path = path
        self.kind = kind          # 'common' or 'banked'
        self.pin = pin            # forced bank number, or None
        self.name = re.sub(r'\W', '_', os.path.splitext(os.path.basename(path))[0])
        with open(path) as f:
            self.lines = f.read().split('\n')
        self.bank = 0 if kind == 'common' else None
        self.size = 0
        self.funcs = set()        # function labels defined in code areas
        self.statics = set()      # of those, module-local ones

    def code_area(self, area):
        return area in ('_CODE', '_HOME')

    def scan(self):
        area = None
        for line in self.lines:
            m = AREA_RE.match(line)
            if m:
                area = m.group(1)
                continue
            m = LABEL_RE.match(line)
            if not m or not self.code_area(area):
                continue
            name, colons = m.group(1), m.group(2)
            if re.match(r'^\d+\$$', name):
                continue
            self.funcs.add(name)
            if colons == ':':
                self.statics.add(name)


def split_comment(line):
    """Split an asxxxx line into code and comment (';' outside quotes)."""
    q = None
    for i, c in enumerate(line):
        if q:
            if c == q:
                q = None
        elif c in '"\'':
            q = c
        elif c == ';':
            return line[:i], line[i:]
    return line, ''


def measure(mod, sdas):
    """Assemble a module to learn the size of its code area."""
    with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as td:
        src = os.path.join(td, mod.name + '.s')
        with open(src, 'w') as f:
            f.write('\n'.join(mod.lines))
        rel = os.path.join(td, mod.name + '.rel')
        r = subprocess.run([sdas, '-plosgff', rel, src], capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit('banker: cannot assemble %s:\n%s%s' % (mod.path, r.stdout, r.stderr))
        size = 0
        with open(rel) as f:
            for l in f:
                m = re.match(r'A (\w+) size (\w+)', l)
                if m and m.group(1) in ('_CODE', '_HOME'):
                    size += int(m.group(2), 16)
        return size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--outdir', required=True)
    ap.add_argument('--thunks', required=True)
    ap.add_argument('--map')
    ap.add_argument('--ldargs', help='write sdldz80 -b options for the banks to this file')
    ap.add_argument('--bank-size', type=lambda s: int(s, 0), default=0x4000)
    ap.add_argument('--first-bank', type=int, default=1)
    ap.add_argument('--max-bank', type=int, default=0x1B)
    ap.add_argument('--sdas', default='sdasz80')
    ap.add_argument('files', nargs='+')
    a = ap.parse_args()

    mods = []
    for spec in a.files:
        kind, _, rest = spec.partition(':')
        pin = None
        if kind == 'pin':
            b, _, rest = rest.partition(':')
            pin = int(b, 0)
            kind = 'banked'
        if kind not in ('common', 'banked'):
            sys.exit('banker: bad spec ' + spec)
        mods.append(Module(rest, kind, pin))
    names = [m.name for m in mods]
    if len(set(names)) != len(names):
        sys.exit('banker: duplicate module names')

    for m in mods:
        m.scan()

    # Make statics of banked modules globally unique and public, so that
    # thunks (in another file) can reference them.
    for m in mods:
        if m.kind != 'banked' or not m.statics:
            continue
        ren = {s: '%s__%s' % (s, m.name) for s in m.statics}
        out = []
        for line in m.lines:
            code, com = split_comment(line)
            lm = LABEL_RE.match(code)
            if lm and lm.group(1) in ren and lm.group(2) == ':':
                code = ren[lm.group(1)] + '::' + lm.group(3)
            else:
                code = IDENT_RE.sub(lambda t: ren.get(t.group(0), t.group(0)), code)
            out.append(code + com)
        m.lines = out
        m.funcs = {ren.get(f, f) for f in m.funcs}
        m.statics = set()

    # Bank assignment.
    banked = [m for m in mods if m.kind == 'banked']
    for m in banked:
        m.size = measure(m, a.sdas)
        if m.size > a.bank_size:
            sys.exit('banker: %s needs %d bytes, more than one bank' % (m.path, m.size))
    used = {}
    for m in banked:
        if m.pin is not None:
            m.bank = m.pin
            used[m.pin] = used.get(m.pin, 0) + m.size
    for m in sorted([m for m in banked if m.pin is None], key=lambda m: -m.size):
        b = a.first_bank
        while used.get(b, 0) + m.size > a.bank_size:
            b += 1
        if b > a.max_bank:
            sys.exit('banker: out of banks')
        m.bank = b
        used[b] = used.get(b, 0) + m.size
    for b, s in used.items():
        if s > a.bank_size:
            sys.exit('banker: bank %d overfull (%d bytes)' % (b, s))

    owner = {}
    for m in mods:
        for f in m.funcs:
            owner[f] = m
    farsyms = {}  # symbol -> bank

    def far(sym):
        farsyms[sym] = owner[sym].bank
        return sym + '__far'

    os.makedirs(a.outdir, exist_ok=True)
    for m in mods:
        out = []
        area = None
        for line in m.lines:
            code, com = split_comment(line)
            am = AREA_RE.match(code)
            if am:
                area = am.group(1)
                if m.kind == 'banked' and area in ('_CODE', '_HOME'):
                    code = code.replace(area, '_BANK%d' % m.bank)
                out.append(code + com)
                continue
            lm = LABEL_RE.match(code)
            prefix = ''
            body = code
            if lm:
                prefix = lm.group(1) + lm.group(2)
                body = lm.group(3)
            st = body.strip()
            mnem = st.split(None, 1)[0].lower() if st else ''
            if mnem in ('.globl', '.module', '.optsdcc', '.area'):
                out.append(code + com)
                continue
            om = re.match(r'^(\s*\S+)(\s*)(.*)$', body)
            head, ws, operands = (om.group(1), om.group(2), om.group(3)) if om else (body, '', '')
            direct_target = None
            if mnem in DIRECT_MNEMONICS:
                ops = [o.strip() for o in operands.split(',')]
                if ops and IDENT_RE.fullmatch(ops[-1]):
                    direct_target = ops[-1]

            def sub(t):
                s = t.group(0)
                o = owner.get(s)
                if o is None or o.kind == 'common':
                    return s
                if s == direct_target and o.bank == m.bank:
                    return s
                return far(s)
            if operands:
                body = head + ws + IDENT_RE.sub(sub, operands)
            out.append(prefix + body + com)
        # Thunk names must be declared for the assembler (-g makes undefined
        # symbols global anyway, but be explicit).
        refs = sorted({s + '__far' for s in farsyms if any(s + '__far' in l for l in out)})
        if refs:
            out.insert(0, '\n'.join('\t.globl %s' % r for r in refs))
        with open(os.path.join(a.outdir, os.path.basename(m.path)), 'w') as f:
            f.write('\n'.join(out))

    with open(a.thunks, 'w') as f:
        f.write(';; generated by banker.py - far call thunks (page 0)\n')
        f.write('\t.module thunks\n\t.globl ___farcall\n\t.area _CODE\n')
        for s in sorted(farsyms):
            f.write('%s__far::\n\tld\tc, a\n\tld\tb, #%d\n\tld\tiy, #%s\n\tjp\t___farcall\n'
                    % (s, farsyms[s], s))

    if a.ldargs:
        with open(a.ldargs, 'w') as f:
            f.write(' '.join('-b _BANK%d=0x%X4000' % (b, b) for b in sorted(used)) + '\n')

    if a.map:
        with open(a.map, 'w') as f:
            for b in sorted(used):
                f.write('bank %02X: %5d bytes free\n' % (b, a.bank_size - used[b]))
                for m in sorted(banked, key=lambda m: m.name):
                    if m.bank == b:
                        f.write('    %-24s %5d\n' % (m.name, m.size))
            f.write('thunks: %d (%d bytes)\n' % (len(farsyms), 10 * len(farsyms)))


if __name__ == '__main__':
    main()
