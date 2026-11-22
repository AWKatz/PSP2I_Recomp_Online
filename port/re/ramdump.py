#!/usr/bin/env python3
"""Read RAM snapshots from the SDL host (PSP2I_RAMDUMP=1, then F7 or SIGUSR1:
logs/ramdump_<n>.bin, the game's memory from 0x08800000).

  python3 -I port/re/ramdump.py DUMP word ADDR [N]            N words from ADDR
  python3 -I port/re/ramdump.py DUMP ptr ADDR                 the pointer at ADDR
  python3 -I port/re/ramdump.py DUMP array BASE N STRIDE OFFS...  N elements: listed fields (u8/u16/u32 by suffix b/h/w, e.g. 74b 100w)
  python3 -I port/re/ramdump.py DUMP diff OTHER LO HI         words that differ between two dumps in [LO, HI)
Addresses may be "[ADDR]+K": the pointer stored at ADDR, plus K.
"""
import struct
import sys

BASE = 0x08800000


class Dump:
    def __init__(self, path):
        self.b = open(path, 'rb').read()

    def u(self, a, size=4):
        o = a - BASE
        if not 0 <= o <= len(self.b) - size:
            return None
        return int.from_bytes(self.b[o:o + size], 'little')

    def addr(self, s):
        if s.startswith('['):
            inner, _, k = s[1:].partition(']')
            p = self.u(int(inner, 16))
            return p + (int(k.lstrip('+'), 0) if k else 0)
        return int(s, 16)


def main():
    a = sys.argv[1:]
    if len(a) < 3:
        print(__doc__)
        return
    d = Dump(a[0])
    if a[1] == 'word':
        base, n = d.addr(a[2]), int(a[3]) if len(a) > 3 else 8
        for i in range(n):
            print('%08X  %08X' % (base + 4 * i, d.u(base + 4 * i)))
    elif a[1] == 'ptr':
        print('%08X' % d.u(d.addr(a[2])))
    elif a[1] == 'array':
        base, n, stride = d.addr(a[2]), int(a[3]), int(a[4])
        fields = a[5:]
        for i in range(n):
            e = base + i * stride
            vals = []
            for f in fields:
                size = {'b': 1, 'h': 2, 'w': 4}[f[-1]]
                off = int(f[:-1])
                vals.append('+%d=%X' % (off, d.u(e + off, size)))
            print('[%d] %08X  %s' % (i, e, '  '.join(vals)))
    elif a[1] == 'diff':
        o = Dump(a[2])
        lo, hi = int(a[3], 16), int(a[4], 16)
        for x in range(lo, hi, 4):
            if d.u(x) != o.u(x):
                print('%08X  %08X -> %08X' % (x, d.u(x), o.u(x)))


if __name__ == '__main__':
    main()
