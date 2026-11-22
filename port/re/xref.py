#!/usr/bin/env python3
"""Cross-references for the recompiled game (a small stand-in for Ghidra).

Reads build/gen/recomp_funcs.c (every instruction is in a comment there) and
build/gen/recomp_imports.c, and builds an index (build/re/index.pickle):

  - functions: entry -> list of (addr, mnemonic, operands)
  - calls:     callee -> callers (jal), including firmware import stubs
  - refs:      data address -> [(function, instruction address, kind)],
               from lui+addiu/ori/load/store pairs and $gp-relative accesses
  - strings:   C strings in the ELF image, by address

Usage (from the repository root):
  python3 -I port/re/xref.py index                 build the index (once, ~1 min)
  python3 -I port/re/xref.py str REGEX             strings matching, with the code that loads them
  python3 -I port/re/xref.py refs ADDR [ADDR2]     code that touches ADDR (or the range ADDR..ADDR2)
  python3 -I port/re/xref.py callers FUNC|NAME     who calls a function or a firmware import by name
  python3 -I port/re/xref.py dis FUNC [N]          a function's instructions (or N instructions from FUNC)
  python3 -I port/re/xref.py args NAME [REG]       constants passed in REG (default $a1) at each call of an import
  python3 -I port/re/xref.py loops [N]            every loop bounded by N (default 4), grouped by the array it walks
  python3 -I port/re/xref.py indexed [N]          loops over i < N addressing base + (i << k): N-element arrays
  python3 -I port/re/xref.py who FUNC              class / source names printed by nearby functions
  python3 -I port/re/xref.py word VALUE            where a 32-bit value is stored in the image (pointer tables)
  python3 -I port/re/xref.py near ADDR [N]         the N words around ADDR, with strings and functions named

Nothing it writes is committed: the index is derived from the game.
"""
import os
import pickle
import re
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
GEN = os.path.join(ROOT, 'build', 'gen')
OUT = os.path.join(ROOT, 'build', 're')
IDX = os.path.join(OUT, 'index.pickle')
EBOOT = os.path.join(ROOT, 'EBOOT.BIN')
GP = 0x08ECF830

LINE = re.compile(r'^\s*/\* ([0-9A-F]{8})  (\S+)\s*(.*?)\s*\*/\s*$')
BODY = re.compile(r'^static void psp_body_([0-9A-F]{8})\(')
IMPORT = re.compile(r'^/\* (\S+) :: NID 0x([0-9A-F]{8}) \*/')
STUB = re.compile(r'^void psp_import_([0-9A-F]{8})\(')
REG = re.compile(r'\$(\w+)')
MEM = re.compile(r'(-?\d+|0x[0-9A-Fa-f]+)\(\$(\w+)\)')
LOADS = {'lw', 'lh', 'lhu', 'lb', 'lbu', 'lwc1', 'lwl', 'lwr', 'lv.s', 'lv.q', 'ulv.q'}
STORES = {'sw', 'sh', 'sb', 'swc1', 'swl', 'swr', 'sv.s', 'sv.q', 'usv.q'}


def num(s):
    return int(s, 16) if s.lower().startswith(('0x', '-0x')) else int(s)


def nid_names():
    """NID -> function name, from psprecomp's HLE registrations."""
    names = {}
    pat = re.compile(r'0x([0-9A-Fa-f]{8})u?\s*,\s*"(\w+)"\s*,\s*"(\w+)"')
    pat2 = re.compile(r'\{\s*0x([0-9A-Fa-f]{8})u?\s*,\s*"(\w+)"')
    hle = os.path.join(ROOT, 'psprecomp', 'src')
    for dirpath, _, files in os.walk(hle):
        for f in files:
            if not f.endswith('.c'):
                continue
            text = open(os.path.join(dirpath, f), encoding='utf-8', errors='replace').read()
            for m in pat.finditer(text):
                names.setdefault(int(m.group(1), 16), m.group(3))
            for m in pat2.finditer(text):
                names.setdefault(int(m.group(1), 16), m.group(2))
    return names


def elf_image():
    """(vaddr, bytes) of the loaded segment."""
    data = open(EBOOT, 'rb').read()
    phoff, = struct.unpack_from('<I', data, 0x1C)
    phentsize, phnum = struct.unpack_from('<HH', data, 0x2A)
    for i in range(phnum):
        p_type, p_off, p_vaddr, _, p_filesz = struct.unpack_from('<IIIII', data, phoff + i * phentsize)
        if p_type == 1:
            return p_vaddr, data[p_off:p_off + p_filesz]
    raise SystemExit('no PT_LOAD segment')


def build():
    funcs, calls, refs = {}, {}, {}
    cur = None
    insns = None
    with open(os.path.join(GEN, 'recomp_funcs.c'), encoding='utf-8', errors='replace') as f:
        for line in f:
            m = BODY.match(line)
            if m:
                cur = int(m.group(1), 16)
                insns = funcs.setdefault(cur, [])
                continue
            if cur is None:
                continue
            m = LINE.match(line)
            if m:
                insns.append((int(m.group(1), 16), m.group(2), m.group(3)))
    stubs = {}
    names = nid_names()
    with open(os.path.join(GEN, 'recomp_imports.c'), encoding='utf-8') as f:
        pending = None
        for line in f:
            m = IMPORT.match(line)
            if m:
                pending = (m.group(1), int(m.group(2), 16))
                continue
            m = STUB.match(line)
            if m and pending:
                lib, nid = pending
                stubs[int(m.group(1), 16)] = names.get(nid, '%s_%08X' % (lib, nid))
                pending = None
    # calls and data references, tracking lui constants per function
    for entry, ins in funcs.items():
        hi = {}
        for addr, mn, ops in ins:
            regs = REG.findall(ops)
            if mn == 'jal':
                t = int(ops, 16)
                calls.setdefault(t, set()).add(entry)
                continue
            if mn == 'lui' and len(regs) == 1:
                hi[regs[0]] = (num(ops.split(',')[1].strip()) & 0xFFFF) << 16
                continue
            if mn in ('addiu', 'ori') and len(regs) == 2:
                d, s = regs
                imm = num(ops.split(',')[2].strip())
                if s == 'gp':
                    a = (GP + imm) & 0xFFFFFFFF
                    refs.setdefault(a, []).append((entry, addr, 'addr'))
                elif s in hi:
                    a = (hi[s] | (imm & 0xFFFF)) if mn == 'ori' else (hi[s] + imm) & 0xFFFFFFFF
                    refs.setdefault(a, []).append((entry, addr, 'addr'))
                    hi[d] = a if d != s else a
                    continue
                hi.pop(d, None)
                continue
            mm = MEM.search(ops)
            if mm and (mn in LOADS or mn in STORES):
                off, base = num(mm.group(1)), mm.group(2)
                kind = 'load' if mn in LOADS else 'store'
                if base == 'gp':
                    refs.setdefault((GP + off) & 0xFFFFFFFF, []).append((entry, addr, kind))
                elif base in hi:
                    refs.setdefault((hi[base] + off) & 0xFFFFFFFF, []).append((entry, addr, kind))
                if mn in LOADS and regs:
                    hi.pop(regs[0], None)
                continue
            if regs:
                hi.pop(regs[0], None)                        # the destination is overwritten
    base, img = elf_image()
    strings = {}
    for m in re.finditer(rb'[\x20-\x7e\t\n]{4,}\x00', img):
        strings[base + m.start()] = m.group()[:-1].decode('ascii')
    os.makedirs(OUT, exist_ok=True)
    with open(IDX, 'wb') as f:
        pickle.dump({'funcs': funcs, 'calls': calls, 'refs': refs, 'stubs': stubs, 'strings': strings,
                     'image': (base, img)}, f, protocol=pickle.HIGHEST_PROTOCOL)
    print('index: %d functions, %d call targets, %d referenced addresses, %d imports, %d strings -> %s'
          % (len(funcs), len(calls), len(refs), len(stubs), len(strings), os.path.relpath(IDX, ROOT)))


def load():
    if not os.path.exists(IDX):
        raise SystemExit('no index: run "xref.py index" first')
    with open(IDX, 'rb') as f:
        return pickle.load(f)


def func_of(ix, addr):
    """The function containing addr."""
    best = None
    for e in ix.get('_sorted') or []:
        pass
    keys = ix.setdefault('_sorted', sorted(ix['funcs']))
    import bisect
    i = bisect.bisect_right(keys, addr) - 1
    return keys[i] if i >= 0 else None


def resolve(ix, s):
    if re.fullmatch(r'(0x)?[0-9A-Fa-f]{8}', s):
        return [int(s, 16)]
    return [a for a, n in ix['stubs'].items() if n == s or n.endswith(s)]


def cmd_str(ix, pat):
    rx = re.compile(pat)
    for a, s in sorted(ix['strings'].items()):
        if rx.search(s):
            users = sorted({e for e, _, _ in ix['refs'].get(a, [])})
            print('%08X  %-60s  <- %s' % (a, repr(s)[:60], ' '.join('%08X' % u for u in users) or '(no direct reference)'))


def cmd_refs(ix, a, b=None):
    b = b if b is not None else a
    for addr in sorted(k for k in ix['refs'] if a <= k <= b):
        for e, at, kind in ix['refs'][addr]:
            print('%08X  %-5s  in %08X at %08X' % (addr, kind, e, at))


def cmd_callers(ix, s):
    for t in resolve(ix, s):
        name = ix['stubs'].get(t, '%08X' % t)
        cs = sorted(ix['calls'].get(t, []))
        print('%s (%08X): %d callers: %s' % (name, t, len(cs), ' '.join('%08X' % c for c in cs)))


def all_insns(ix):
    if '_all' not in ix:
        d = {}
        for ins in ix['funcs'].values():
            for addr, mn, ops in ins:
                d[addr] = (mn, ops)
        ix['_all'] = d
    return ix['_all']


def cmd_dis(ix, s, count=None):
    e = int(s, 16)
    if count:                                             # a raw range, whatever the bodies
        d = all_insns(ix)
        rows = [(a, *d[a]) for a in range(e, e + 4 * count, 4) if a in d]
    else:
        rows = ix['funcs'].get(e, [])
    for addr, mn, ops in rows:
        extra = ''
        if mn == 'jal':
            t = int(ops, 16)
            extra = '   ; ' + ix['stubs'].get(t, '')
        print('%08X  %-10s %s%s' % (addr, mn, ops, extra))


def cmd_args(ix, name, reg='a1'):
    """The value of REG at each call of an import (constants from li/addiu $zero/ori/lui in the 8 instructions before)."""
    reg = reg.lstrip('$')
    for t in resolve(ix, name):
        for caller in sorted(ix['calls'].get(t, [])):
            ins = ix['funcs'][caller]
            for i, (addr, mn, ops) in enumerate(ins):
                if mn == 'jal' and int(ops, 16) == t:
                    val = '?'
                    for addr2, mn2, ops2 in ins[max(0, i - 10):i + 2]:   # the delay slot counts
                        r = REG.findall(ops2)
                        if r and r[0] == reg:
                            if mn2 in ('addiu', 'ori') and len(r) == 2 and r[1] == 'zero':
                                val = str(num(ops2.split(',')[2].strip()))
                            elif mn2 == 'li':
                                val = ops2.split(',')[1].strip()
                            else:
                                val = '%s %s' % (mn2, ops2)
                    print('%s at %08X in %08X: $%s = %s' % (ix['stubs'][t], addr, caller, reg, val))


def cmd_word(ix, v):
    base, img = ix['image']
    pat = struct.pack('<I', v)
    i = img.find(pat)
    while i >= 0:
        if i % 4 == 0:
            print('%08X' % (base + i))
        i = img.find(pat, i + 1)


def cmd_near(ix, a, n=8):
    base, img = ix['image']
    for k in range(-n, n + 1):
        at = a + 4 * k
        if not base <= at < base + len(img) - 3:
            continue
        w, = struct.unpack_from('<I', img, at - base)
        note = ''
        if w in ix['strings']:
            note = repr(ix['strings'][w])[:50]
        elif w in ix['funcs']:
            note = 'func %08X' % w
        print('%s%08X  %08X  %s' % ('>' if k == 0 else ' ', at, w, note))


def sym_before(d, start, reg, limit=40):
    """A symbolic value for reg at address start: "G+K" (global), "[G+K]+K2" (a pointer loaded from a global), or None."""
    val = {}
    a0 = start - 4 * limit
    for a in range(a0, start, 4):
        if a not in d:
            continue
        mn, ops = d[a]
        r = REG.findall(ops)
        if not r:
            continue
        dst = r[0]
        try:
            if mn == 'lui':
                val[dst] = ('g', (num(ops.split(',')[1].strip()) & 0xFFFF) << 16)
            elif mn == 'addiu' and len(r) == 2 and r[1] in val and val[r[1]][0] in ('g', 'p'):
                k = num(ops.split(',')[2].strip())
                kind, v = val[r[1]][0], val[r[1]][1]
                val[dst] = ('g', (v + k) & 0xFFFFFFFF) if kind == 'g' else ('p', v, val[r[1]][2] + k)
            elif mn == 'addiu' and len(r) == 2 and r[1] == 'gp':
                val[dst] = ('g', (GP + num(ops.split(',')[2].strip())) & 0xFFFFFFFF)
            elif mn == 'lw':
                m = MEM.search(ops)
                b, off = m.group(2), num(m.group(1))
                if b in val and val[b][0] == 'g':
                    val[dst] = ('p', (val[b][1] + off) & 0xFFFFFFFF, 0)
                elif b == 'gp':
                    val[dst] = ('p', (GP + off) & 0xFFFFFFFF, 0)
                else:
                    val.pop(dst, None)
            elif mn == 'addu' and len(r) == 3 and r[2] == 'zero' and r[1] in val:
                val[dst] = val[r[1]]
            else:
                val.pop(dst, None)
        except (ValueError, AttributeError, IndexError):
            val.pop(dst, None)
    v = val.get(reg)
    if not v:
        return None
    return '%08X' % v[1] if v[0] == 'g' else '[%08X]+%d' % (v[1], v[2])


def class_hint(ix, f, span=0x3000):
    """Class or source names printed by functions near f (methods of a class sit together)."""
    if '_anchors' not in ix:
        anc = []
        cls = re.compile(r'(T[A-Z]\w+)::|/([A-Za-z0-9_]+)\.cpp|^([A-Z][A-Za-z0-9]+)::')
        for a, txt in ix['strings'].items():
            m = cls.search(txt)
            if not m:
                continue
            name = m.group(1) or m.group(2) or m.group(3)
            for e, _, _ in ix['refs'].get(a, []):
                anc.append((e, name))
        anc.sort()
        ix['_anchors'] = anc
    import bisect
    anc = ix['_anchors']
    i = bisect.bisect_left(anc, (f - span, ''))
    near = {}
    while i < len(anc) and anc[i][0] <= f + span:
        e, n = anc[i]
        dist = abs(e - f)
        if n not in near or dist < near[n]:
            near[n] = dist
        i += 1
    return ', '.join('%s(%+X)' % (n, d) for n, d in sorted(near.items(), key=lambda x: x[1])[:3])


def cmd_indexed(ix, bound=4):
    """Loops over an index i < bound that address base + (i << k): arrays of `bound` elements."""
    d = all_insns(ix)
    rows = []
    for a, (mn, ops) in d.items():
        if mn not in ('slti', 'sltiu'):
            continue
        parts = [p.strip() for p in ops.split(',')]
        if len(parts) != 3 or parts[2] != str(bound):
            continue
        r = REG.findall(ops)
        cond, idx = r[0], r[1]
        top = None
        for b in range(a + 4, a + 16, 4):
            if b in d and d[b][0] in ('bne', 'beq', 'bnel', 'beql') and cond in d[b][1]:
                t = int(d[b][1].split(',')[-1].strip(), 16)
                if t < a:
                    top, end = t, b + 8
                break
        if top is None or a - top > 4 * 200:
            continue
        shifted = {}
        for c in range(top, end, 4):
            if c not in d:
                continue
            m2, o2 = d[c]
            rr = REG.findall(o2)
            if m2 == 'sll' and len(rr) == 2 and rr[1] == idx:
                shifted[rr[0]] = int(o2.split(',')[2].strip())
            elif m2 == 'addu' and len(rr) == 3 and (rr[1] in shifted or rr[2] in shifted):
                sh = rr[1] if rr[1] in shifted else rr[2]
                other = rr[2] if sh == rr[1] else rr[1]
                base = sym_before(d, c, other, limit=(c - top) // 4 + 30)
                if base:
                    uses = set()
                    for e2 in range(c, end, 4):
                        if e2 in d:
                            mm = MEM.search(d[e2][1])
                            if mm and mm.group(2) == rr[0] and (d[e2][0] in LOADS or d[e2][0] in STORES):
                                uses.add(num(mm.group(1)))
                    rows.append((func_of(ix, a), a, 1 << shifted[sh], base, sorted(uses)))
    return rows


def cmd_loops(ix, bound=4):
    """Every loop bounded by `bound`: the arrays it walks."""
    d = all_insns(ix)
    rows = []
    for a, (mn, ops) in d.items():
        if mn not in ('slti', 'sltiu'):
            continue
        parts = [p.strip() for p in ops.split(',')]
        if len(parts) != 3 or parts[2] != str(bound):
            continue
        # the backward branch on the result within 3 instructions
        cond = REG.findall(parts[0])[0]
        top = None
        for b in range(a + 4, a + 16, 4):
            if b in d and d[b][0] in ('bne', 'beq', 'bnel', 'beql') and cond in d[b][1]:
                t = int(d[b][1].split(',')[-1].strip(), 16)
                if t < a:
                    top = t
                    end = b + 4
                break
        if top is None:
            continue
        strides, uses = {}, {}
        for c in range(top, end + 4, 4):
            if c not in d:
                continue
            m2, o2 = d[c]
            r = REG.findall(o2)
            if m2 == 'addiu' and len(r) == 2 and r[0] == r[1]:
                k = num(o2.split(',')[2].strip())
                if k not in (1, -1) and 2 <= abs(k) <= 4096:
                    strides[r[0]] = k
            mm = MEM.search(o2)
            if mm and (m2 in LOADS or m2 in STORES):
                uses.setdefault(mm.group(2), set()).add(num(mm.group(1)))
        for reg, k in strides.items():
            if reg not in uses:
                continue
            base = sym_before(d, top, reg)
            rows.append((func_of(ix, a), a, k, base or '?', sorted(uses[reg])))
    return rows


def main():
    a = sys.argv[1:]
    if not a or a[0] in ('-h', '--help'):
        print(__doc__)
        return
    if a[0] == 'index':
        build()
        return
    ix = load()
    if a[0] == 'str':
        cmd_str(ix, a[1])
    elif a[0] == 'refs':
        cmd_refs(ix, int(a[1], 16), int(a[2], 16) if len(a) > 2 else None)
    elif a[0] == 'callers':
        cmd_callers(ix, a[1])
    elif a[0] == 'dis':
        cmd_dis(ix, a[1], int(a[2]) if len(a) > 2 else None)
    elif a[0] == 'args':
        cmd_args(ix, a[1], a[2] if len(a) > 2 else 'a1')
    elif a[0] == 'loops':
        rows = cmd_loops(ix, int(a[1]) if len(a) > 1 else 4)
        groups = {}
        for f, at, k, base, offs in rows:
            groups.setdefault((base, k), []).append((f, at, offs))
        for (base, k), items in sorted(groups.items(), key=lambda g: -len(g[1])):
            offs = sorted({o for _, _, os_ in items for o in os_})
            print('base %-18s stride %4d  loops %3d  fields %s  funcs %s' % (base, k, len(items), offs[:12],
                  ' '.join(sorted({'%08X' % f for f, _, _ in items}))[:120]))
    elif a[0] == 'indexed':
        rows = cmd_indexed(ix, int(a[1]) if len(a) > 1 else 4)
        groups = {}
        for f, at, k, base, offs in rows:
            groups.setdefault((base, k), []).append((f, at, offs))
        for (base, k), items in sorted(groups.items(), key=lambda g: (-len(g[1]), g[0])):
            offs = sorted({o for _, _, os_ in items for o in os_})
            fs = sorted({f for f, _, _ in items})
            print('base %-18s elem %4d  loops %3d  fields %-28s near %-40s funcs %s' % (base, k, len(items), str(offs[:8]),
                  class_hint(ix, fs[0])[:40], ' '.join('%08X' % f for f in fs)[:100]))
    elif a[0] == 'who':
        print(class_hint(ix, int(a[1], 16), int(a[2], 16) if len(a) > 2 else 0x3000))
    elif a[0] == 'word':
        cmd_word(ix, int(a[1], 16))
    elif a[0] == 'near':
        cmd_near(ix, int(a[1], 16), int(a[2]) if len(a) > 2 else 8)
    else:
        print(__doc__)


if __name__ == '__main__':
    main()
