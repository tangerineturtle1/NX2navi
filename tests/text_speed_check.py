"""Build and run tests/text_speed_check.c: the text_speed setting (runtime/text_speed.c) and the replacement words of
the patch set text_speed (recomp/patches-v111.txt), lifted by recomp/lifter.py as the generator lifts them, against a
stand-in message window and typewriter. No game files: the lifted words go to the test's build folder.

Usage: text_speed_check.py
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / 'recomp')]
from lifter import Lifter  # noqa: E402
from patches import load  # noqa: E402
from tools.check_content import GENERATED_MARKER  # noqa: E402

MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
MAIN = 0x7000000000
SKIP_SITE, SPEED_SITE = 0x774b74, 0xafe1d8


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def site_words(table, site, original):
    """The set's replacement words at a site, after checking the site has only them and the original word."""
    alternatives = table.sites['main'][site]
    assert len(alternatives) == 1, hex(site)
    key, words, have = alternatives[0]
    assert table.sets[key] == ('text_speed', 'NX2NAVI_TEXT_SPEED_PATCH', 1), table.sets[key]
    assert have == original, (hex(site), hex(have))
    return words


def lifted(name, site, words, page_reg, last_branch):
    """A C function running the words at the site: the adrp of the patch page is left to the caller, who puts the
    page's address in that register, and a last conditional branch is left to the caller too, who reads its register."""
    body = []
    for j, word in enumerate(words):
        if isinstance(word, tuple):
            assert word == ('page', page_reg) and j == (1 if site == SPEED_SITE else 0), (hex(site), word)
            continue
        insn = Lifter(0, simd='c').lift(word, MAIN + site)
        if j == len(words) - 1 and last_branch:
            assert insn.kind != 'op' and word == 0x36000069, hex(word)   # tbz w9, #0, the original's target
            continue
        assert insn.kind == 'op', (hex(site), hex(word), insn.kind)
        body.append('    ' + insn.code)
    return f'void {name}(Cpu *c) {{\n' + '\n'.join(body) + '\n}\n'


def main():
    table = load(ROOT / 'recomp/patches-v111.txt', [('main', MAIN, 0x4ea0000)])
    skip = site_words(table, SKIP_SITE, 0x36000074)     # tbz w20, #0, #0x774b80
    speed = site_words(table, SPEED_SITE, 0xbd404e61)   # ldr s1, [x19, #0x4c]
    work = ROOT / 'build' / 'test' / 'text_speed'
    work.mkdir(parents=True, exist_ok=True)
    sites = work / 'sites.c'
    sites.write_text(f'// {GENERATED_MARKER}: the patch set text_speed lifted by tests/text_speed_check.py.\n'
                     '#include "cpu.h"\n#include "simd.h"\n\n'
                     + lifted('site_skip', SKIP_SITE, skip, 9, True) + '\n'
                     + lifted('site_speed', SPEED_SITE, speed, 22, False))
    exe = work / ('text_speed_check.exe' if os.name == 'nt' else 'text_speed_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-Wall', '-Wno-unused-function', '-Wno-unused-variable',
                           '-fno-strict-aliasing', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'text_speed_check.c'), str(sites), '-o', str(exe), '-lm'], env=env)
    rc = subprocess.run([str(exe)], env=env).returncode
    return subprocess.run([str(exe), 'kept-off'], env=env).returncode or rc


if __name__ == '__main__':
    sys.exit(main())
