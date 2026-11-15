"""Build and run tests/heap_lazy_copy_check.c: runtime/memory.c mem_copy_readable with the lazy heap
(NX2NAVI_HEAP_LAZY=1), where a heap page nothing touched yet is still reserved. The copy holds frz_lock, which the
fault on such a page waits for; the check needs every copy to return, untouched pages to read zero and stay reserved,
and pages past the heap's end to be left out.

Windows only (the lazy heap is): memory.c is built with the Windows platform layer and its fault handler, the rest of
the runtime stubbed. No game files: the module layout the runtime includes is a stand-in written to the test's build
folder.

Usage: heap_lazy_copy_check.py
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


LAYOUT = '''// A stand-in for the generated layout.h (tests/heap_lazy_copy_check.py).
#pragma once
#define MOD_MAIN_BASE 0x7000000000ull
#define MOD_MAIN_SIZE 0x4ea0000ull
#define MOD_COUNT 3
'''


def main():
    if os.name != 'nt':
        print('heap_lazy_copy_check: the lazy heap is Windows only; skipped')
        return 0
    work = ROOT / 'build' / 'test' / 'heap_lazy_copy'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'layout.h').write_text(LAYOUT)
    exe = work / 'heap_lazy_copy_check.exe'
    env = dict(os.environ)
    if MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    plat = [rt / 'platform' / 'win32.c', rt / 'platform' / 'common.c']
    libs = ['-lbcrypt', '-lws2_32', '-lpsapi', '-lwinmm', '-ldbghelp', '-lshell32', '-lole32', '-luser32', '-ladvapi32']
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing',
                           '-march=x86-64-v3', '-I', str(rt), '-I', str(work),
                           str(ROOT / 'tests' / 'heap_lazy_copy_check.c'), *map(str, plat), '-o', str(exe), '-lm',
                           *libs], env=env)
    try:
        return subprocess.run([str(exe)], env=env, timeout=120).returncode
    except subprocess.TimeoutExpired:
        print('heap_lazy_copy_check: FAIL: the check did not end within 120 s')
        return 1


if __name__ == '__main__':
    sys.exit(main())
