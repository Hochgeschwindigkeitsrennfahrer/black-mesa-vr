#!/usr/bin/env python3
"""Port src/offsets.h to a new Black Mesa build.

    python3 -I tools/offsets/port_offsets.py --old <dir with the previous build's DLLs> [--game <installed game>] [--write]

<dir> mirrors the game layout (bin/engine.dll, bin/materialsystem.dll, bin/shaderapidx9.dll,
bms/bin/client.dll, bms/bin/server.dll). Copy them BEFORE Steam applies an update, or fetch them
with DepotDownloader (Windows binaries = depot 362894).

What it does:
  1. Every RVA constant (value >= 0x10000, arrays included; module from gen_offset_checks.MODULES):
     code addresses by a masked byte window around the old address (relocated dwords and branch
     targets wildcarded), searched in the new DLL; data globals by voting over the code that
     references them. Prints old -> new and the match quality; --write rewrites offsets.h with
     `// beta <new> (<old build>: 0x...)` comments.
  2. Every signature Offset{}: number of matches in the new build and the mapped address; --write
     updates the default offset so the exact-address check hits again.
  3. Layout gates for the constants that have no runtime check (struct fields, vtable slots):
     classes whose vtable length changed (then check slots the mod calls), and datamap
     (push offset; push "m_name") fields whose offset changed.
Anything not unique is listed under PROBLEMS and is never written. Then regenerate the checks
(gen_offset_checks.py), build, and boot the game: the log line `Offset checks <dll>: N/N ok`.
"""
import argparse
import collections
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE, relocs, masked, search  # noqa: E402
import gen_offset_checks as gen  # noqa: E402

DLL = gen.DLL


def u32(pe, rva):
    return struct.unpack_from('<I', pe.d, pe.rva2off(rva))[0]


def in_text(pe, rva):
    t = pe.text()
    return t[1] <= rva < t[1] + t[2]


def map_code(o, n, old, before=0, lens=(96, 64, 48, 32, 24, 16)):
    for L in lens:
        raw, mask = masked(o, old - before, L + before)
        hits = search(n, raw, mask)
        if len(hits) == 1:
            return hits[0] + before, f'unique@{L}'
        if hits and len(hits) <= 4 and len(search(o, raw, mask)) == len(hits):
            best = min(hits, key=lambda h: abs(h + before - old))
            return best + before, f'nearest of {len(hits)}@{L} (CHECK)'
    return None, 'no match'


def map_data(o, n, old, maxsites=40):
    votes = collections.Counter()
    sites = [r for r in relocs(o) if in_text(o, r) and u32(o, r) == o.base + old]
    for s in sites[:maxsites]:
        for before in (8, 12, 16):
            raw, mask = masked(o, s - before, 40)
            hits = search(n, raw, mask)
            if len(hits) == 1:
                votes[u32(n, hits[0] + before) - n.base] += 1
                break
    return votes, len(sites)


def vtables(pe):
    out = {}
    n, va, vs, ra, rs = [x for x in pe.secs if x[0] == '.rdata'][0]
    rel = set(relocs(pe))
    for r in sorted(x for x in rel if va <= x < va + vs):
        col = u32(pe, r) - pe.base
        if not (va <= col < va + vs):
            continue
        try:
            if u32(pe, col) != 0:
                continue
            td = u32(pe, col + 12) - pe.base
            name = pe.at(td + 8, 90).split(b'\0')[0]
            if not name.startswith(b'.?AV'):
                continue
        except Exception:
            continue
        L = 0
        while (r + 4 + L * 4) in rel and in_text(pe, u32(pe, r + 4 + L * 4) - pe.base):
            L += 1
        if L:
            out.setdefault(name.decode(), []).append(L)
    return out


def datamap_fields(pe):
    n, va, vs, ra, rs = [x for x in pe.secs if x[0] == '.rdata'][0]
    names = {}
    for m in re.finditer(rb'(m_[A-Za-z0-9_\[\]\.]{1,60})\0', pe.d[ra:ra + rs]):
        names[pe.base + va + m.start()] = m.group(1).decode()
    t = pe.text()
    out = collections.defaultdict(set)
    for m in re.finditer(rb'\x68(....)\x68(....)', pe.d[t[3]:t[3] + t[4]], re.S):
        off, nm = struct.unpack('<I', m.group(1))[0], struct.unpack('<I', m.group(2))[0]
        if nm in names and off < 0x10000:
            out[names[nm]].add(off)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--old', required=True)
    ap.add_argument('--game', default=gen.DEFAULT_GAME)
    ap.add_argument('--write', action='store_true')
    args = ap.parse_args()
    old = {m: PE(os.path.join(args.old, p)) for m, p in DLL.items()}
    new = {m: PE(os.path.join(args.game, p)) for m, p in DLL.items()}
    newbuild = gen.build_id(args.game)
    path = os.path.join(gen.REPO, 'src', 'offsets.h')
    text = open(path).read()
    problems, changes = [], {}

    print('== RVA constants')
    for name, idx, val, _ in gen.constants(text):
        if val < 0x10000:
            continue
        label = name + (f'[{idx}]' if idx is not None else '')
        mod = gen.MODULE_OF.get(name)
        if not mod:
            problems.append(f'{label}: no module in gen_offset_checks.MODULES')
            continue
        o, n = old[mod], new[mod]
        if in_text(o, val):
            mid = bool(gen.MID.search(name)) or idx is not None
            r, why = map_code(o, n, val, before=24 if mid else 0)
            if r is None and not mid:
                r, why = map_code(o, n, val, before=24)
        else:
            votes, nsites = map_data(o, n, val)
            if votes:
                (r, c), *rest = votes.most_common(2)
                why = f'data {c}/{nsites} sites' + (f', runner-up {rest[0][1]}' if rest else '')
                if rest and rest[0][1] * 3 > c:
                    why += ' (CHECK)'
            else:
                r, why = None, f'no mappable reference ({nsites} sites)'
        print(f'  {label:44s} {mod:8s} {val:#08x} -> {r:#08x}  {why}' if r is not None else f'  {label:44s} {mod:8s} {val:#08x} -> ?  {why}')
        if r is None or 'CHECK' in why:
            problems.append(f'{label}: {why}')
            continue
        changes[(name, idx)] = (val, r)

    print('== Signature Offset{} entries')
    sig_changes = {}
    for m in re.finditer(r'Offset\s+(\w+)\{\s*"([\w.]+)",\s*(0x[0-9A-Fa-f]+),\s*((?:"[^"]*"\s*)+)(?:,\s*(-?\w+))?', text):
        name, dll, off, sig, so = m.groups()
        mod = dll.lower().replace('.dll', '')
        off = int(off, 16)
        so = int(so, 0) if so and so not in ('true', 'false') else 0
        toks = ' '.join(re.findall(r'"([^"]*)"', sig)).split()
        rx = b''.join(b'.' if t in ('?', '??') else re.escape(bytes([int(t, 16)])) for t in toks)
        t = new[mod].text()
        hits = [x.start() + t[1] + so for x in re.finditer(rx, new[mod].d[t[3]:t[3] + t[4]], re.S)]
        r, why = map_code(old[mod], new[mod], off, before=24)
        ok = r in hits if r is not None else len(hits) == 1
        target = r if r is not None else (hits[0] if len(hits) == 1 else None)
        print(f'  {name:34s} {mod:8s} {off:#x} -> {target if target is None else hex(target)}  sig matches {len(hits)}, map {why}')
        if not ok or target is None:
            problems.append(f'Offset {name}: signature matches {len(hits)}, map {why}')
            continue
        if target != off:
            sig_changes[name] = (off, target)

    print('== Layout gates')
    for mod in DLL:
        a, b = vtables(old[mod]), vtables(new[mod])
        ch = [(k, a[k], b.get(k)) for k in a if b.get(k) != a[k]]
        print(f'  {mod}: {len(ch)} classes with a different vtable length')
        for k, x, y in ch:
            print(f'      {k} {x} -> {y}')
    for mod in ('client', 'server'):
        a, b = datamap_fields(old[mod]), datamap_fields(new[mod])
        ch = {k: (sorted(a[k]), sorted(b.get(k, ()))) for k in a if a[k] != b.get(k, set())}
        print(f'  {mod}: {len(a)} datamap fields, {len(ch)} changed')
        for k, (x, y) in sorted(ch.items()):
            print(f'      {k} {[hex(v) for v in x]} -> {[hex(v) for v in y]}')
    print('  -> for every changed class/field above, check the constants in offsets.h that use it')

    if problems:
        print('== PROBLEMS (not written; port these by hand):', *problems, sep='\n  ')
    if not args.write:
        print('(dry run; --write updates src/offsets.h)')
        return
    for (name, idx), (ov, nv) in changes.items():
        if ov == nv:
            continue
        if idx is None:
            pat = re.compile(rf'({name}(?:\[\d*\])? = )0x{ov:X}\b;[^\n]*', re.I)
            text, k = pat.subn(lambda x: f'{x.group(1)}0x{nv:X}; // beta {newbuild} (was 0x{ov:X})', text)
        else:
            pat = re.compile(rf'({name}\[\d*\] = \{{[^}}]*?)\b0x{ov:X}\b', re.I | re.S)
            text, k = pat.subn(lambda x: f'{x.group(1)}0x{nv:X}', text)
        if k != 1:
            print(f'  could not rewrite {name}[{idx}] ({k} matches)')
    for name, (ov, nv) in sig_changes.items():
        text, k = re.subn(rf'(Offset {name}\{{ "[\w.]+", )0x{ov:X},', lambda x: f'{x.group(1)}0x{nv:X},', text, flags=re.I)
        if k != 1:
            print(f'  could not rewrite Offset {name}')
    open(path, 'w', newline='\n').write(text)
    print(f'wrote {path}: {len(changes)} constants mapped, {len(sig_changes)} signature defaults moved')


if __name__ == '__main__':
    main()
