#!/usr/bin/env python3
"""banker.py - automatic code banking for SDCC/Z80 on the TI-84 Plus.

The TI-84 Plus maps flash page 0 at 0000h-3FFFh (always present) and one
switchable flash page ("bank A", port 06h) at 4000h-7FFFh.  This tool takes
the assembler output of SDCC and distributes the *functions* of all banked
modules over flash pages 1..N (in source order, so that related functions
stay together).  References are rewritten so that

  * calls/jumps to a function in the same page stay direct;
  * calls from another page, and every place where a function address is
    taken (function pointers, tables), go through a small thunk in page 0.

A thunk switches bank A, calls the real function and switches back.  The
return bookkeeping lives on a separate "far stack" (see os/farcall.s), so
the callee sees exactly the stack layout of a direct call and the normal
SDCC calling convention (register and stack arguments, callee cleanup)
keeps working.

Constant data must be compiled with --constseg CONST: it stays in page 0,
so pointers to constants are valid from every bank.

Usage:
  banker.py --outdir DIR --thunks DIR/thunks.s [--map map.txt] [--ldargs f]
            [--bank-size N] [--banks 1-7,33-3B]
            common:FILE.asm ... banked:FILE.asm ...
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
CODE_AREAS = ('_CODE', '_HOME')


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


class Module:
    def __init__(self, path, kind):
        self.path = path
        self.kind = kind          # 'common' or 'banked'
        self.name = re.sub(r'\W', '_', os.path.splitext(os.path.basename(path))[0])
        self.ext = os.path.splitext(path)[1]
        with open(path) as f:
            self.lines = f.read().split('\n')
        self.funcs = []           # function labels in code areas, in order
        self.statics = set()
        self.chunk_of = []        # per line: function index for code lines, else None
        self.sizes = []           # per function
        self.banks = []           # per function

    def scan(self):
        area = None
        chunk = -1
        for line in self.lines:
            code, _ = split_comment(line)
            m = AREA_RE.match(code)
            if m:
                area = m.group(1)
                self.chunk_of.append(None)
                continue
            if area in CODE_AREAS:
                lm = LABEL_RE.match(code)
                if lm and not re.match(r'^\d+\$$', lm.group(1)):
                    self.funcs.append(lm.group(1))
                    if lm.group(2) == ':':
                        self.statics.add(lm.group(1))
                    chunk = len(self.funcs) - 1
                if code.strip() and chunk < 0 and self.kind == 'banked':
                    sys.exit('banker: %s has code before the first label' % self.path)
                self.chunk_of.append(chunk if code.strip() else None)
            else:
                self.chunk_of.append(None)


def assemble_sizes(mod, sdas):
    """Size of every function: assemble with one area per function."""
    out = []
    cur = None
    for line, ch in zip(mod.lines, mod.chunk_of):
        code, _ = split_comment(line)
        if AREA_RE.match(code):
            cur = None
            out.append(line)
            continue
        if ch is not None and ch != cur:
            out.append('\t.area _F%d' % ch)
            cur = ch
        out.append(line)
    with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as td:
        src = os.path.join(td, mod.name + '.s')
        with open(src, 'w') as f:
            f.write('\n'.join(out))
        rel = os.path.join(td, mod.name + '.rel')
        r = subprocess.run([sdas, '-plosgff', rel, src], capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit('banker: cannot assemble %s:\n%s%s' % (mod.path, r.stdout, r.stderr))
        sizes = [0] * len(mod.funcs)
        with open(rel) as f:
            for l in f:
                m = re.match(r'A _F(\d+) size (\w+)', l)
                if m:
                    sizes[int(m.group(1))] = int(m.group(2), 16)
        return sizes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--outdir', required=True)
    ap.add_argument('--thunks', required=True)
    ap.add_argument('--map')
    ap.add_argument('--ldargs', help='write sdldz80 -b options for the banks to this file')
    ap.add_argument('--bank-size', type=lambda s: int(s, 0), default=0x4000)
    ap.add_argument('--banks', default='1-7',
                    help='flash pages for banked code, e.g. "1-7,33-3B" (hex)')
    ap.add_argument('--sdas', default='sdasz80')
    ap.add_argument('files', nargs='+')
    a = ap.parse_args()

    mods = []
    for spec in a.files:
        kind, _, rest = spec.partition(':')
        if kind not in ('common', 'banked'):
            sys.exit('banker: bad spec ' + spec)
        mods.append(Module(rest, kind))

    # module names must be unique: qualify clashing ones with their directory
    seen = {}
    for m in mods:
        seen.setdefault(m.name, []).append(m)
    for name, ms in seen.items():
        if len(ms) > 1:
            for m in ms:
                d = os.path.basename(os.path.dirname(m.path))
                m.name = re.sub(r'\W', '_', d) + '_' + name
    if len({m.name for m in mods}) != len(mods):
        sys.exit('banker: duplicate module names')

    for m in mods:
        m.scan()

    # Statics of banked modules become globally unique public symbols, so
    # that thunks and other banks can reference them.
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
        m.funcs = [ren.get(f, f) for f in m.funcs]
        m.statics = set()

    # Bank assignment, function by function in source order.
    pages = []
    for part in a.banks.split(','):
        lo, _, hi = part.partition('-')
        pages += range(int(lo, 16), int(hi or lo, 16) + 1)
    used = {}
    bank_idx = 0
    bank = pages[0]
    for m in mods:
        if m.kind != 'banked':
            m.banks = [0] * len(m.funcs)
            continue
        m.sizes = assemble_sizes(m, a.sdas)
        for f, sz in zip(m.funcs, m.sizes):
            if sz > a.bank_size:
                sys.exit('banker: function %s (%s) is larger than a bank' % (f, m.path))
            if used.get(bank, 0) + sz > a.bank_size:
                bank_idx += 1
                if bank_idx >= len(pages):
                    sys.exit('banker: out of banks (%s)' % a.banks)
                bank = pages[bank_idx]
            m.banks.append(bank)
            used[bank] = used.get(bank, 0) + sz

    owner = {}   # function symbol -> bank (0 = page 0)
    for m in mods:
        for f, b in zip(m.funcs, m.banks):
            owner[f] = b
    farsyms = {}

    os.makedirs(a.outdir, exist_ok=True)
    for m in mods:
        out = []
        orig_area = None
        emitted_area = None
        for line, ch in zip(m.lines, m.chunk_of):
            code, com = split_comment(line)
            am = AREA_RE.match(code)
            if am:
                orig_area = am.group(1)
                if m.kind == 'banked' and orig_area in CODE_AREAS:
                    continue            # re-emitted per function below
                out.append(code + com)
                emitted_area = orig_area
                continue
            if m.kind == 'common':
                here = 0
            elif ch is not None:
                here = m.banks[ch]
                want = '_BANK%d' % here
                if emitted_area != want:
                    out.append('\t.area %s' % want)
                    emitted_area = want
            else:
                here = None
                if code.strip() and orig_area is not None and emitted_area != orig_area:
                    out.append('\t.area %s' % orig_area)
                    emitted_area = orig_area

            lm = LABEL_RE.match(code)
            prefix = ''
            body = code
            if lm:
                prefix = lm.group(1) + lm.group(2)
                body = lm.group(3)
            st = body.strip()
            mnem = st.split(None, 1)[0].lower() if st else ''
            if mnem in ('.globl', '.module', '.optsdcc'):
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
                b = owner.get(s)
                if b is None or b == 0:
                    return s
                if s == direct_target and here == b:
                    return s
                farsyms[s] = b
                return s + '__far'
            if operands:
                body = head + ws + IDENT_RE.sub(sub, operands)
            out.append(prefix + body + com)
        with open(os.path.join(a.outdir, m.name + m.ext), 'w') as f:
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
                for m in mods:
                    n = sum(sz for sz, bb in zip(m.sizes, m.banks) if bb == b)
                    if n:
                        f.write('    %-28s %5d\n' % (m.name, n))
            f.write('thunks: %d (%d bytes)\n' % (len(farsyms), 10 * len(farsyms)))


if __name__ == '__main__':
    main()
