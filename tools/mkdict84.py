#!/usr/bin/env python3
"""mkdict84.py - the conversion dictionaries of the input methods (lib/ime).

  mkdict84.py extract MOZC_DIR UNIHAN_DIR FONT.bin OUT_DIR
      picks the entries from Mozc's open dictionary (src/data/dictionary_oss)
      and Unihan whose characters the font has: OUT_DIR/ja.tsv (reading,
      word; most frequent first) and OUT_DIR/zh.tsv (pinyin without tones,
      characters; most frequent first)
  mkdict84.py build DIR OUT.bin [BUDGET]
      the blob for the flash data pages, at most BUDGET bytes (131072):
      all of zh.tsv, then the words of ja.tsv in order while they fit

The blob (lib/ime/dict.c reads it):

  0   "D84" 0, version 1, 0, 0, 0
  8   checksum            16-bit sum of all bytes from offset 32 on
  10  ja_groups           number of readings
  12  ja_index_n          entries of the index
  14  reserved
  16  ja_index            offset of the index (uint32)
  20  ja_data             offset of the readings
  24  zh                  offset of the pinyin table
  28  size                bytes
  32  ...

Japanese: readings sorted by their bytes, each
  (shared << 4 | rest)  bytes shared with the reading before, bytes that follow
  rest bytes            kana: 01h-56h hiragana 3041h-3096h, 57h ー
  n                     words, most frequent first, each
  len, bytes            kana as above; other characters two bytes
                        80h | (cp - 3000h) >> 8, (cp - 3000h) & FFh
Every 16th reading starts with shared = 0; the index lists their offsets
(3 bytes each, from ja_data).

Chinese: n (uint16), offsets (uint16 each, from zh) of n syllables sorted
by their letters, each: length, letters (v for u with umlaut), count
(uint16), code points (uint16 each).
"""
import glob
import os
import re
import struct
import sys
import unicodedata

GROUP = 16


def font_cps(path):
    b = open(path, 'rb').read()
    lo_off, = struct.unpack_from('<I', b, 12)
    hi = struct.unpack_from('<257H', b, 32)
    cps = set()
    for h in range(256):
        for k in range(hi[h], hi[h + 1]):
            cps.add(h * 256 + b[lo_off + k])
    return cps


def is_kana_reading(r):
    return r and all(0x3041 <= ord(c) <= 0x3096 or c == 'ー' for c in r)


def extract(mozc, unihan, font, out):
    cps = font_cps(font)
    pos = {}
    for ln in open(os.path.join(mozc, 'id.def'), encoding='utf-8'):
        i, p = ln.split(' ', 1)
        pos[i] = p.strip().split(',')
    # verbs and adjectives: the forms that other words follow (the rest of
    # a word stays kana); common nouns before the many inflected forms
    keep_forms = {'基本形', '連用形', '連用タ接続', '未然形', '*'}
    # words, not phrases: (o|go) kanji with up to 3 kana, at most twice
    word_re = re.compile('^[おご]?([\u4e00-\u9fff々]+[\u3041-\u3096]{0,3}){1,2}$')
    best = {}
    for f in sorted(glob.glob(os.path.join(mozc, 'dictionary0*.txt'))):
        for ln in open(f, encoding='utf-8'):
            r, lid, _, cost, w = ln.rstrip('\n').split('\t')[:5]
            p = pos[lid]
            if not is_kana_reading(r) or len(r) > 12 or '人名' in p[1] or '人名' in p[2]:
                continue
            if p[0] in ('動詞', '形容詞') and p[5] not in keep_forms:
                continue
            if len(w) > 6 or not word_re.match(w):
                continue
            if not all(0x3000 <= ord(c) <= 0xAFFF and ord(c) in cps for c in w):
                continue
            cost = int(cost)
            if p[0] == '名詞' and p[1] not in ('固有名詞', '接尾', '数'):
                cost -= 1000
            if best.get((r, w), 1 << 30) > cost:
                best[(r, w)] = cost
    items = sorted(best.items(), key=lambda kv: (kv[1], kv[0]))[:36000]
    with open(os.path.join(out, 'ja.tsv'), 'w', encoding='utf-8') as f:
        f.write('# from Mozc dictionary_oss (IPAdic, see COPYING): reading, word\n')
        for (r, w), _ in items:
            f.write('%s\t%s\n' % (r, w))
    print('ja.tsv: %d words' % len(items))

    # Chinese: simplified characters (GB 2312) the font has, by frequency
    def toneless(p):
        p = unicodedata.normalize('NFD', p.lower())
        p = p.replace('ü', 'v')
        return ''.join(c for c in p if 'a' <= c <= 'z')

    gb = {}                             # code point -> GB 2312 level (1 common, 2)
    for hi_ in range(0xB0, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                gb[ord(bytes([hi_, lo]).decode('gb2312'))] = 1 if hi_ < 0xD8 else 2
            except UnicodeDecodeError:
                pass
    freq, mandarin = {}, {}
    for ln in open(os.path.join(unihan, 'Unihan_Readings.txt'), encoding='utf-8'):
        if ln.startswith('#') or not ln.strip():
            continue
        u, field, val = ln.rstrip('\n').split('\t')
        cp = int(u[2:], 16)
        if cp not in gb or cp not in cps:
            continue
        if field == 'kHanyuPinlu':
            for v in val.split():
                p, n = v.rstrip(')').split('(')
                freq[(toneless(p), cp)] = freq.get((toneless(p), cp), 0) + int(n)
        elif field == 'kMandarin':
            mandarin[cp] = [toneless(p) for p in val.split()]
    syl = {}
    for (p, cp), n in freq.items():
        syl.setdefault(p, {})[cp] = n
    for cp, ps in mandarin.items():
        for p in ps:
            s = syl.setdefault(p, {})
            if cp not in s:
                s[cp] = 0
    with open(os.path.join(out, 'zh.tsv'), 'w', encoding='utf-8') as f:
        f.write('# from Unihan (kHanyuPinlu, kMandarin; see COPYING): pinyin, characters\n')
        for p in sorted(syl):
            chars = sorted(syl[p], key=lambda cp: (-syl[p][cp], gb[cp], cp))
            f.write('%s\t%s\n' % (p, ''.join(chr(c) for c in chars)))
    print('zh.tsv: %d syllables, %d characters' % (len(syl), sum(len(v) for v in syl.values())))


def kana_code(c):
    o = ord(c)
    if 0x3041 <= o <= 0x3096:
        return bytes([o - 0x3040])
    if c == 'ー':
        return b'\x57'
    return None


def enc_word(w):
    out = b''
    for c in w:
        k = kana_code(c)
        if k:
            out += k
        else:
            v = ord(c) - 0x3000
            out += bytes([0x80 | v >> 8, v & 0xFF])
    return out


def build(src, dst, budget):
    zh = {}
    for ln in open(os.path.join(src, 'zh.tsv'), encoding='utf-8'):
        if ln.startswith('#'):
            continue
        p, chars = ln.rstrip('\n').split('\t')
        zh[p] = chars
    zb = bytearray(struct.pack('<H', len(zh)))
    recs = bytearray()
    offs = []
    base = 2 + 2 * len(zh)
    for p in sorted(zh):
        offs.append(base + len(recs))
        recs += bytes([len(p)]) + p.encode() + struct.pack('<H', len(zh[p]))
        recs += b''.join(struct.pack('<H', ord(c)) for c in zh[p])
    zb += b''.join(struct.pack('<H', o) for o in offs) + recs
    assert len(zb) < 65536

    words = []
    for ln in open(os.path.join(src, 'ja.tsv'), encoding='utf-8'):
        if ln.startswith('#'):
            continue
        r, w = ln.rstrip('\n').split('\t')
        words.append((r, w))

    def ja_blob(n):
        groups = {}
        for r, w in words[:n]:
            groups.setdefault(b''.join(kana_code(c) for c in r), []).append(enc_word(w))
        data = bytearray()
        index = []
        prev = b''
        for i, r in enumerate(sorted(groups)):
            if i % GROUP == 0:
                index.append(len(data))
                shared = 0
            else:
                shared = 0
                while shared < min(len(r), len(prev), 15) and r[shared] == prev[shared]:
                    shared += 1
            rest = r[shared:]
            assert len(rest) < 16
            data += bytes([shared << 4 | len(rest)]) + rest + bytes([min(len(groups[r]), 255)])
            for w in groups[r][:255]:
                data += bytes([len(w)]) + w
            prev = r
        ib = b''.join(struct.pack('<I', o)[:3] for o in index)
        return len(groups), len(index), ib, data

    lo, hi = 0, len(words)
    while lo < hi:                      # the most words that fit
        mid = (lo + hi + 1) // 2
        _, _, ib, data = ja_blob(mid)
        if 32 + len(ib) + len(data) + len(zb) <= budget:
            lo = mid
        else:
            hi = mid - 1
    ng, ni, ib, data = ja_blob(lo)
    body = ib + data + zb
    size = 32 + len(body)
    head = b'D84\0' + bytes([1, 0, 0, 0]) + struct.pack('<HHHH', sum(body) & 0xFFFF, ng, ni, 0)
    head += struct.pack('<IIII', 32, 32 + len(ib), 32 + len(ib) + len(data), size)
    open(dst, 'wb').write(head + body)
    print('%s: %d Japanese words (%d readings), %d syllables, %d bytes' %
          (dst, lo, ng, len(zh), size))


def main():
    a = sys.argv[1:]
    if a[:1] == ['extract'] and len(a) == 5:
        extract(*a[1:])
    elif a[:1] == ['build'] and len(a) in (3, 4):
        build(a[1], a[2], int(a[3]) if len(a) > 3 else 131072)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
