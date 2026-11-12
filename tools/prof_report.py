"""Report for a NX2NAVI_PROFILE sampling profile (runtime/prof_win.c): top functions and the guest/runtime/outside split.

usage: prof_report.py <profile> <nx2navi.map | nm -n output> [--top 40] [--chains 30]

The map is the one from the link (build/<version>/nx2navi.map from lld, whose addresses are RVAs, or a GNU ld map such
as the launcher's build-cache/ocarina.map, whose addresses include the image base); the profile header carries the
runtime image base, so ASLR does not matter. Symbols in the map are labels without sizes: a Rip belongs to the nearest
symbol at or below it. A map lists global symbols only, so a static function's time lands on the global function before
it: the output of `nm -n <exe>` in the map's place names the static functions too.

A profile taken with NX2NAVI_PROFILE_RENDER=1 or NX2NAVI_PROFILE_CHAINS=1 also has call chains: the report then adds the inclusive time per
function (a function counted once per chain), and for each module outside the exe the exe functions that call into it
and the longest common chains down to it.
"""
import argparse
import bisect
import collections
import re
import sys
from pathlib import Path

SYM = re.compile(r'^([0-9a-f]{8}) +[0-9a-f]{8} +\d+ {2,}(\S+)$')   # In column empty: object lines have a path:(section) there
SYM_LD = re.compile(r'^\s*0x([0-9a-f]{8,16})\s+(\S+)\s*$')          # a GNU ld map: the address, then the name, no size
SYM_NM = re.compile(r'^([0-9a-f]{16}) [tT] (\S+)$')                 # nm -n of the exe: code symbols, static ones too


def load_map(path):
    syms = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            m = SYM_NM.match(line) or SYM.match(line) or SYM_LD.match(line)
            if m and '(' not in m.group(2) and m.group(2) != '.text':   # nm lists each object's .text section too
                syms.setdefault(int(m.group(1), 16), m.group(2))
    addrs = sorted(syms)
    return addrs, [syms[a] for a in addrs]


def load_profile(path):
    prof = {'rips': [], 'modules': [], 'threads': [], 'stacks': [], 'samples': 0, 'base': 0}
    for line in Path(path).read_text().splitlines():
        p = line.split(None, 3)
        if not p:
            continue
        if p[0] == 'base':
            prof['base'] = int(p[1], 16)
        elif p[0] == 'samples':
            prof['samples'] = int(p[1])
        elif p[0] == 'thread':
            prof['threads'].append((int(p[1]), int(p[2]), int(p[3])))
        elif p[0] == 'module':
            prof['modules'].append((int(p[1], 16), int(p[2], 16), p[3]))
        elif p[0] == 'rip':
            prof['rips'].append((int(p[1], 16), int(p[2])))
        elif p[0] == 'stack':
            f = line.split()
            prof['stacks'].append((int(f[1]), [int(x, 16) for x in f[2:]]))
    return prof


def report_chains(prof, name_of, in_exe, top):
    stacks = prof['stacks']
    total = sum(n for n, _ in stacks) or 1
    print(f'\ncall chains: {total} samples in {len(stacks)} chains (render thread, or every thread with NX2NAVI_PROFILE_CHAINS)')
    incl = collections.Counter()
    for n, chain in stacks:
        for f in set(name_of(r) for r in chain):
            incl[f] += n
    print(f'\ninclusive, top {top} (% of chain samples)')
    for f, n in incl.most_common(top):
        print(f'  {100 * n / total:6.2f}%  {n:8d}  {f}')
    # Where running threads are: the innermost exe frame of each chain that does not end in a wait. A chain whose leaf is
    # in ntdll, win32u or KERNELBASE is taken as blocked, which also drops the few running samples inside them (clock
    # reads); a leaf in another module (the driver, the C library) is charged to the exe function that called it.
    waits = ('ntdll.dll', 'win32u.dll', 'KERNELBASE.dll')
    self_t = collections.Counter()
    for n, chain in stacks:
        if not in_exe(chain[0]) and name_of(chain[0]).split('!')[0] in waits:
            continue
        i = next((k for k, r in enumerate(chain) if in_exe(r)), None)
        if i is None:
            self_t['(' + name_of(chain[0]).split('!')[0] + ', no exe frame)'] += n
        else:
            self_t[name_of(chain[i]) + ('' if i == 0 else '  [in ' + name_of(chain[0]).split('!')[0] + ']')] += n
    run = sum(self_t.values()) or 1
    print(f'\nself time of running chains, top {top} ({run} samples, {100 * run / total:.1f}% of the chain samples)')
    for f, n in self_t.most_common(top):
        print(f'  {100 * n / run:6.2f}%  {n:8d}  {f}')
    # Per outside module: the innermost exe frame of each chain whose leaf is in that module
    by_mod = collections.defaultdict(collections.Counter)
    paths = collections.defaultdict(collections.Counter)
    for n, chain in stacks:
        if in_exe(chain[0]):
            continue
        mod = name_of(chain[0]).split('!')[0]
        i = next((k for k, r in enumerate(chain) if in_exe(r)), None)
        if i is None:
            by_mod[mod]['(no exe frame in the chain)'] += n
            continue
        by_mod[mod][name_of(chain[i])] += n
        paths[mod][' < '.join(name_of(r) for r in chain[i:i + 4])] += n
    for mod, c in sorted(by_mod.items(), key=lambda kv: -sum(kv[1].values())):
        m = sum(c.values())
        print(f'\n{mod}: {100 * m / total:.2f}% of chain samples; exe callers')
        for f, n in c.most_common(12):
            print(f'  {100 * n / total:6.2f}%  {n:8d}  {f}')
        for pth, n in paths[mod].most_common(6):
            print(f'      {100 * n / total:6.2f}%  {pth}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('profile')
    ap.add_argument('map')
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--chains', type=int, default=30, help='inclusive functions listed from the call chains')
    a = ap.parse_args()
    prof = load_profile(a.profile)
    addrs, names = load_map(a.map)
    base = prof['base']
    # The map's addresses include the image base (0x140000000 for an x64 exe); a runtime Rip is an offset from wherever
    # the loader put the image. The lowest map address aligned down to 64 KiB recovers that base, so a Rip becomes a map
    # address by adding it back.
    img_base = addrs[0] & ~0xFFFFF if addrs else 0
    exe = next((m for m in prof['modules'] if m[0] == base), None)
    exe_end = base + exe[1] if exe else base

    def in_exe(rip):
        return base <= rip < exe_end

    def sym_at(rip):
        i = bisect.bisect_right(addrs, img_base + (rip - base)) - 1
        return names[i] if i >= 0 else '?'

    def name_of(rip):
        if in_exe(rip):
            return sym_at(rip)
        m = next((m for m in prof['modules'] if m[0] <= rip < m[0] + m[1]), None)
        return f'{Path(m[2]).name}!+{rip - m[0]:#x}' if m else 'unknown'
    total = sum(n for _, n in prof['rips']) or 1
    funcs = collections.Counter()
    cats = collections.Counter()
    for rip, n in prof['rips']:
        if base <= rip < exe_end:
            name = sym_at(rip)   # as the call chains below: right for a map with or without the image base
            funcs[name] += n
            cats['guest (f_*)' if name.startswith('f_') else 'runtime host (exe)'] += n
            continue
        mod = next((Path(m[2]).name for m in prof['modules'] if m[0] <= rip < m[0] + m[1]), None)
        cats['outside: ' + (mod or 'unknown')] += n
        funcs[(mod or 'unknown') + '!+' + hex(rip - next((m[0] for m in prof['modules'] if m[0] <= rip < m[0] + m[1]), 0))] += n
    blocked = ('outside: ntdll', 'outside: win32u', 'outside: KERNELBASE')   # waits and message loops: threads not running
    idle = sum(n for k, n in cats.items() if k.startswith(blocked))
    act = (total - idle) or 1
    print(f'samples {prof["samples"]} (Rips kept {total}), 1 ms nominal period; ntdll/win32u/KERNELBASE (blocked threads) {100 * idle / total:.1f}%')
    print('\ncategory (% of all samples, % of running samples = all minus ntdll/win32u/KERNELBASE)')
    for k, n in cats.most_common():
        print(f'  {100 * n / total:6.2f}%  {"" if k.startswith(blocked) else format(100 * n / act, "6.2f") + "%":>7}  {n:8d}  {k}')
    print(f'\ntop {a.top} functions')
    for k, n in funcs.most_common(a.top):
        print(f'  {100 * n / total:6.2f}%  {n:8d}  {k}')
    print('\nthreads (samples, % of them inside the exe)')
    for tid, n, ie in sorted(prof['threads'], key=lambda t: -t[1])[:12]:
        print(f'  tid {tid:6d} {n:8d}  {100 * ie / max(n, 1):5.1f}% in exe')
    if prof['stacks']:
        report_chains(prof, name_of, in_exe, a.chains)


if __name__ == '__main__':
    sys.exit(main())
