#!/usr/bin/env python3
"""typekeys.py [--mode 1|A|a] TEXT... - emu84 "key" commands that type TEXT
with os/input.c's keypad layout (ALPHA cycles 1 -> A -> a, 2nd = symbol).
Prints the commands; the final mode is printed as a comment."""
import sys

NUM = {'+': '+', '-': '-', '*': '*', '/': '/', '^': '^', ')': ')', '.': '.', '(': '(',
       ',': ',', '=': 'sto', '0': '0', '1': '1', '2': '2', '3': '3', '4': '4', '5': '5',
       '6': '6', '7': '7', '8': '8', '9': '9'}
ALPHA = {'"': '+', 'W': '-', 'R': '*', 'M': '/', 'H': '^', '?': 'neg', '_': '3', 'V': '6',
         'Q': '9', 'L': ')', 'G': 'tan', ':': '.', 'Z': '2', 'U': '5', 'P': '8', 'K': '(',
         'F': 'cos', 'C': 'prgm', ' ': '0', 'Y': '1', 'T': '4', 'O': '7', 'J': ',', 'E': 'sin',
         'B': 'apps', 'X': 'sto', 'S': 'ln', 'N': 'log', 'I': 'x2', 'D': 'x-1', 'A': 'math'}
SECOND = {"'": '+', ']': '-', '[': '*', '|': '/', '~': '^', '}': ')', '_': '.', '{': '(',
          '&': 'cos', '#': 'prgm', '`': '0', ';': ',', '%': 'sin', '@': 'apps', '=': 'sto',
          '>': 'ln', '<': 'log', '\\': 'x2', '$': 'x-1', '!': 'math', '?': 'neg'}
ORDER = ['1', 'A', 'a']

args = sys.argv[1:]
mode = '1'
if args and args[0] == '--mode':
    mode = args[1]
    args = args[2:]
text = ' '.join(args)
out = []


def go(m):
    global mode
    while mode != m:
        out.append('key alpha')
        mode = ORDER[(ORDER.index(mode) + 1) % 3]


for c in text:
    if c in NUM and (mode == '1' or (c not in ALPHA and c.upper() not in ALPHA)):
        go('1')
        out.append('key ' + NUM[c])
    elif c.isalpha() or c in ALPHA:
        if c.isalpha():
            go('A' if c.isupper() else 'a')
            out.append('key ' + ALPHA[c.upper()])
        else:
            if mode == '1':
                go('A')
            out.append('key ' + ALPHA[c])
    elif c in SECOND:
        out.append('key 2nd')
        out.append('key ' + SECOND[c])
    else:
        sys.exit('cannot type %r' % c)
print('\n'.join(out))
print('# mode ' + mode)
