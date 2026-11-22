#!/usr/bin/env python3
"""Tables for docs/player_limit_offsets.md, from the xref index
(port/re/xref.py index first). Prints Markdown.

  python3 -I port/re/player_limits.py > /tmp/tables.md

Finds, in the recompiled game:
  1. every `< 4` slot guard and loop inside the party module (the patch sites);
  2. every function elsewhere that loops i < 4 over a party accessor
     (the code that assumes at most 4 members), by subsystem;
  3. every global 4-element array the indexed-loop scan finds.
"""
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
import xref  # noqa: E402

PARTY_LO, PARTY_HI = 0x08A6D398, 0x08A72600
PARTY_PTR = 0x08ED5A28

ACCESSORS = {
    0x08A70A50: 'member(i): slot pointer if in use, else NULL',
    0x08A700CC: 'member_or_default(i): slot, or the shared empty member at 0x08ED95C4',
    0x08A714DC: 'member_data(i): slot + 8 if in use',
    0x08A701F4: 'member_object_id(i): the member\'s object id in the current area, else -1',
    0x08A70550: 'slot_of(object id): the slot holding an object id',
}

AREAS = [  # (lo, hi, subsystem), from debug strings / class names near the code
    (0x0893D000, 0x08958000, 'Quest scripts (TQuestScriptParty, QuestSequence, QuestManager)'),
    (0x0895E000, 0x08962000, 'Save data (TMemoryCardTask)'),
    (0x08A5A000, 0x08A74000, 'Online client (TPsuInfraClient)'),
    (0x08BE0000, 0x08C9C000, 'Enemy and boss AI (BossManager, SeedFals, OlgaFlow...): targeting'),
    (0x08C9C000, 0x08CB6000, 'PSN rooms (TPspNpMatching2, TPartyOrgCtrlPSPInfra)'),
    (0x08D10000, 0x08D20000, 'Room / party sync (TParty, GameRoomServer)'),
]


def area(f):
    for lo, hi, name in AREAS:
        if lo <= f < hi:
            return name
    return 'Game code (unclassified)'


def loops4(ix, d):
    """(slti address, loop top, loop end) for every loop bounded by 4."""
    out = []
    for a, (mn, ops) in d.items():
        if mn not in ('slti', 'sltiu'):
            continue
        p = [x.strip() for x in ops.split(',')]
        if len(p) != 3 or p[2] != '4':
            continue
        cond = xref.REG.findall(p[0])[0]
        for b in range(a + 4, a + 16, 4):
            if b in d and d[b][0] in ('bne', 'beq', 'bnel', 'beql') and cond in d[b][1]:
                t = int(d[b][1].split(',')[-1].strip(), 16)
                if t < a and a - t <= 800:
                    out.append((a, t, b + 8))
                break
    return out


def main():
    ix = xref.load()
    d = xref.all_insns(ix)

    # 1. guards inside the party module
    guards = sorted(a for a, (mn, ops) in d.items()
                    if PARTY_LO <= a < PARTY_HI and mn in ('slti', 'sltiu') and ops.replace(' ', '').endswith(',4'))
    print('### Party module: every `< 4` check (%d sites in %d functions)\n' % (len(guards), len({xref.func_of(ix, g) for g in guards})))
    print('| Function | `< 4` checks at |')
    print('| --- | --- |')
    byf = collections.defaultdict(list)
    for g in guards:
        byf[xref.func_of(ix, g)].append(g)
    for f in sorted(byf):
        note = ACCESSORS.get(f, '')
        print('| `%08X`%s | %s |' % (f, ' ' + note if note else '', ' '.join('`%08X`' % g for g in byf[f])))
    print()

    # 2. consumers elsewhere
    cons = collections.defaultdict(lambda: collections.defaultdict(set))
    for a, top, end in loops4(ix, d):
        for c in range(top, end, 4):
            if c in d and d[c][0] == 'jal':
                t = int(d[c][1], 16)
                f = xref.func_of(ix, a)
                if PARTY_LO <= t < PARTY_HI and not PARTY_LO <= f < PARTY_HI:
                    cons[area(f)][f].add((a, t))
    total = sum(len(v) for v in cons.values())
    print('### Code outside the party module that loops over the 4 slots (%d functions)\n' % total)
    print('| Subsystem | Function | Loop bound at | Calls |')
    print('| --- | --- | --- | --- |')
    for name in sorted(cons, key=lambda n: -len(cons[n])):
        for f in sorted(cons[name]):
            hits = sorted(cons[name][f])
            print('| %s | `%08X` | %s | %s |' % (name, f, ' '.join(sorted({'`%08X`' % a for a, _ in hits})),
                                                ' '.join(sorted({'`%08X`' % t for _, t in hits}))))
    print()

    # 3. global 4-element arrays
    rows = xref.cmd_indexed(ix, 4)
    groups = collections.defaultdict(list)
    for f, at, k, base, offs in rows:
        groups[(base, k)].append((f, at))
    print('### Global 4-element arrays found by the indexed-loop scan (%d)\n' % len(groups))
    print('| Base | Element size | Used by | Near (class / source) |')
    print('| --- | --- | --- | --- |')
    for (base, k), items in sorted(groups.items()):
        fs = sorted({f for f, _ in items})
        print('| `%s` | %d | %s | %s |' % (base, k, ' '.join('`%08X`' % f for f in fs[:6]) + (' ...' if len(fs) > 6 else ''),
                                          xref.class_hint(ix, fs[0], 0x8000) or '-'))


if __name__ == '__main__':
    main()
