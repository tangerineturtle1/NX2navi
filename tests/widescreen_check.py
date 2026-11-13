"""Build and run tests/widescreen_check.c: Ocarina's widescreen patch sets (recomp/patches-v111.txt) and
runtime/widescreen.c, with the rest of the runtime stubbed. No game files.

The sites of the sets wide_aspect, wide_hud, wide_movie, wide_save and wide_tall are lifted as the generator lifts
them, the original's words and each set's replacement into one C function each, `@page:x<n>` as the register set to
the test's page and a last bl to the page as the call of the host registered there. The C side runs them on made-up
projections, UI matrices, viewports and save targets with the page wide_init fills for 16:10, 21:9 and 32:9 pictures.

Usage: widescreen_check.py   (CC=<clang> to pick the compiler)
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
sys.path[:0] = [str(ROOT), str(ROOT / 'recomp')]
from lifter import Lifter  # noqa: E402
from patches import load  # noqa: E402
from tools.check_content import GENERATED_MARKER  # noqa: E402

BASE = 0x7000000000
PAGE = 0x4ea0000   # the patch page of 1.1.1, main+0x4ea0000
SETS = ('wide_aspect', 'wide_hud', 'wide_movie', 'wide_save', 'wide_tall')
TALL_CAMERA = 0x4c52fc   # the camera site of wide_tall, whose bl reaches the page's +0xef0
HOSTS = {0xef0: 'wide_tall_camera_host'}

LAYOUT = '''// A stand-in for the generated layout.h (tests/widescreen_check.py).
#pragma once
#define MOD_MAIN_BASE 0x7000000000ull
#define MOD_MAIN_SIZE 0x4ea0000ull
'''
PATCHES = '''// A stand-in for the generated patches.h (tests/widescreen_check.py).
#pragma once
#define RT_PATCH_PAGE 0x7004ea0000ull
'''


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def vma_include(cc):
    """widescreen.c includes gfx_wsys.h and vk.h, which needs vk_mem_alloc.h (as tests/gpa_check.py finds it): beside
    the compiler's toolchain, MSYS2's, or the build's fetched copy."""
    dirs = [MSYS_BIN.parent / 'include' / 'vma', ROOT / 'toolchain' / 'vma', Path('/usr/include')]
    if Path(cc).is_absolute():
        dirs.insert(0, Path(cc).parent.parent / 'include' / 'vma')
    for d in dirs:
        if (d / 'vk_mem_alloc.h').is_file():
            return d
    sys.exit('widescreen_check: vk_mem_alloc.h not found (docs/building.md)')


def site_functions():
    """C: site_<offset>_<variant>(Cpu *c, uint64_t page) for the original and each set at every widescreen site, and
    a table of them for the harness."""
    table = load(ROOT / 'recomp/patches-v111.txt', [('main', BASE, PAGE)])
    names = {i: t[0] for i, t in enumerate(table.sets)}
    lift = Lifter(0, simd='c')
    out = [f'// {GENERATED_MARKER}: the widescreen sites lifted by tests/widescreen_check.py.',
           '#include "cpu.h"', '#include "simd.h"', '#include "host.h"', '']
    rows = []
    for off, alts in sorted(table.sites['main'].items()):
        if not any(names[k] in SETS for k, _, _ in alts):
            continue
        original = alts[0][2]
        variants = [('game', [original])] + [(names[k], words) for k, words, _ in alts if names[k] in SETS]
        for name, words in variants:
            body = []
            for j, w in enumerate(words):
                if isinstance(w, tuple):   # @page:x<n>
                    body.append(f'    c->x[{w[1]}] = page;')
                    continue
                insn = lift.lift(w, BASE + off)
                if insn.kind == 'bl':
                    assert j == len(words) - 1, (hex(off), name)
                    target = off + (((w & 0x3ffffff) ^ 0x2000000) - 0x2000000) * 4
                    assert target - PAGE in HOSTS, (hex(off), name, hex(target))
                    body.append(f'    c->x[30] = {BASE + off + 4:#x}ull; {HOSTS[target - PAGE]}(c);')
                    continue
                assert insn.kind == 'op', (hex(off), name, hex(w), insn.kind)
                body.append('    ' + insn.code)
            fn = f'site_{off:x}_{name}'
            out.append(f'static void {fn}(Cpu *c, uint64_t page) {{\n    (void)page;\n' + '\n'.join(body) + '\n}\n')
            rows.append(f'    {{{off:#x}, "{name}", {fn}}},')
    out.append('typedef struct { uint32_t off; const char *set; void (*fn)(Cpu *, uint64_t); } SiteFn;')
    out.append('const SiteFn site_fns[] = {\n' + '\n'.join(rows) + '\n    {0, 0, 0}};')
    return '\n'.join(out) + '\n'


def check_sites():
    """wide_tall's sites are wide_aspect's, wide_hud's, wide_movie's and wide_save's, with the camera site besides,
    and its ratio site is wide_aspect's word for word."""
    table = load(ROOT / 'recomp/patches-v111.txt', [('main', BASE, PAGE)])
    names = {i: t[0] for i, t in enumerate(table.sets)}
    sites = {}
    for off, alts in table.sites['main'].items():
        for k, words, _ in alts:
            sites.setdefault(names[k], {})[off] = words
    bad = 0
    tall = set(sites.get('wide_tall', {}))
    others = set().union(*(sites[s] for s in ('wide_aspect', 'wide_hud', 'wide_movie', 'wide_save')))
    if tall != others | {TALL_CAMERA}:
        print(f'FAIL: wide_tall patches {sorted(map(hex, tall))}, the other sets {sorted(map(hex, others))}')
        bad += 1
    if sites['wide_tall'].get(0x4c5334) != sites['wide_aspect'][0x4c5334]:
        print('FAIL: the camera ratio site of wide_tall differs from wide_aspect\'s')
        bad += 1
    order = [t[0] for t in table.sets]
    if order.index('wide_tall') < max(order.index(s) for s in ('wide_aspect', 'wide_hud', 'wide_movie', 'wide_save')):
        print('FAIL: wide_tall comes before the sets it shares sites with')
        bad += 1
    return bad


def main():
    work = ROOT / 'build' / 'test' / 'widescreen'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'layout.h').write_text(LAYOUT)
    (work / 'patches.h').write_text(PATCHES)
    sites = work / 'widescreen_sites.c'
    sites.write_text(site_functions())
    exe = work / ('widescreen_check.exe' if os.name == 'nt' else 'widescreen_check')
    env = dict(os.environ)
    cc = compiler()
    if os.name == 'nt':
        bin_dir = Path(cc).parent if Path(cc).is_absolute() else MSYS_BIN
        env['PATH'] = str(bin_dir) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([cc, '-O1', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing',
                           '-I', str(work), '-I', str(ROOT / 'runtime'), '-I', str(vma_include(cc)),
                           str(ROOT / 'tests' / 'widescreen_check.c'), str(sites), '-o', str(exe), '-lm'], env=env)
    rc = subprocess.run([str(exe)], env=env).returncode
    bad = check_sites()
    print('widescreen_check: ' + ('ok' if not rc and not bad else 'FAILED'))
    return rc or bad


if __name__ == '__main__':
    sys.exit(main())
