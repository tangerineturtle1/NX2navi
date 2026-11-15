"""Build and run tests/gen_wrap_check.c: runtime/vk_memory.c's page write generations across 2^32, 2^40 and 2^63.

From runtime/vk_memory.c it takes, verbatim, into build/test/gen_wrap/gen_section.inc:
- the declarations of PoolRec's mgen and sgen (the generations a VRAM mirror and a shadow copy hold), so the stand-in
  PoolRec has their real width;
- the section from the "page write generations" banner to the "dirty census" banner;
- the readers of the tables outside that section: cpu_wrote_since, shadow_pool_clean, lazy_clean and page_dirty.
The program starts the generations just below each seed's boundary and checks that every compare answers across it as
below it (texture checks, the snapshot path, the region check, the CPU-side tables, mirror and shadow cleanliness),
and that gen_next logs one line when the count reaches 2^32 and no other. It then takes the seeds from
NX2NAVI_GEN_SEED (gen_seed) instead: decimal and 0x hexadecimal values past 32 bits, one past the cap of 2^62, values
that are no number, 0 and none.

No GPU, no game files, any Python.

Usage: gen_wrap_check.py [--cc <clang>]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
BEGIN = '// ---------------------------------------------------------------- page write generations'
END = '// ---------------------------------------------------------------- dirty census'
READERS = ('static int cpu_wrote_since(', 'static int shadow_pool_clean(', 'static int lazy_clean(',
           'static int page_dirty(')
SEEDS = ((1 << 32) - 8, (1 << 40) - 8, (1 << 63) - 8)   # the old wrap; low 32 bits through 0 again; near 2^63
# NX2NAVI_GEN_SEED's value, the start expected of it, and a part of the line it logs ('-': none)
CFG_SEEDS = (
    ('4294967288', (1 << 32) - 8, 'page generations start at 4294967288 (NX2NAVI_GEN_SEED, a test switch)'),
    ('0xfffffffff8', (1 << 40) - 8, 'start at 1099511627768 (NX2NAVI_GEN_SEED,'),   # strtoul would stop at 2^32 - 1
    ('0x7ffffffffffffff8', 1 << 62, 'start at 4611686018427387904 (NX2NAVI_GEN_SEED capped at 2^62,'),
    ('12abc', 0, 'NX2NAVI_GEN_SEED=12abc is not a number'),
    ('-8', 0, 'NX2NAVI_GEN_SEED=-8 is not a number'),
    ('0', 0, '-'),
    ('(unset)', 0, '-'),
)


def compiler(cc):
    if cc:
        return cc
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def definition(src, start):
    """The definition that begins with start (not a prototype), up to its closing brace at column 0."""
    i = 0
    while True:
        i = src.find(start, i)
        if i < 0:
            return None
        eol = src.index('\n', i)
        if src[eol - 1] == '{':
            return src[i:src.index('\n}\n', i) + 3]
        i = eol


def extract(src):
    """gen_section.inc's text, or None and the reason."""
    a, b = src.find(BEGIN), src.find(END)
    if a < 0 or b < a:
        return None, 'the section banners are not in runtime/vk_memory.c'
    s0 = src.find('typedef struct PoolRec {')
    s1 = src.find('} PoolRec;', s0)
    if s0 < 0 or s1 < 0:
        return None, 'PoolRec is not in runtime/vk_memory.c'
    fields = []
    for name in ('mgen', 'sgen'):
        m = re.search(r'^\s*(u?int\d+_t)\s+' + name + r';', src[s0:s1], re.M)
        if not m:
            return None, f'PoolRec.{name} is not declared as an integer of fixed width'
        fields.append(f'{m.group(1)} {name};')
    out = ['// Extracted from runtime/vk_memory.c by tests/gen_wrap_check.py.\n',
           'typedef struct PoolRec { int kind; void *buf; uint64_t import_lo, import_hi; ' + ' '.join(fields) +
           ' } PoolRec;\n', src[a:b]]
    for start in READERS:
        d = definition(src, start)
        if not d:
            return None, f'{start.split()[-1]}...) is not in runtime/vk_memory.c'
        out.append(d)
    return '\n'.join(out), None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cc', help='the C compiler (default: $CC, MSYS2 UCRT64 clang, clang on PATH)')
    args = ap.parse_args()
    inc, why = extract((ROOT / 'runtime' / 'vk_memory.c').read_text(encoding='utf-8-sig'))
    if not inc:
        print(f'gen_wrap_check: FAIL: {why}')
        return 1
    work = ROOT / 'build' / 'test' / 'gen_wrap'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'gen_section.inc').write_text(inc, encoding='utf-8')
    exe = work / ('gen_wrap_check.exe' if os.name == 'nt' else 'gen_wrap_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(args.cc), '-std=gnu11', '-O1', '-Wall', '-Wno-unused-function',
                           '-Wno-unused-variable', '-I', str(work), str(ROOT / 'tests' / 'gen_wrap_check.c'), '-o',
                           str(exe)], env=env)
    runs = [[str(seed)] for seed in SEEDS] + [['--cfg', v, str(start), line] for v, start, line in CFG_SEEDS]
    bad = 0
    for run in runs:
        bad += subprocess.run([str(exe)] + run, env=env, timeout=120).returncode != 0
    if bad:
        print(f'gen_wrap_check: {bad} of {len(runs)} runs failed')
        return 1
    print('gen_wrap_check: all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
