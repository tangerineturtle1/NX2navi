"""Build and run tests/sprint_check.c: the sprint button (runtime/sprint.c) against a stand-in Link, with the rest of
the runtime stubbed. No game files: the module layout and patch page the runtime includes are stand-ins written to the
test's build folder.

Usage: sprint_check.py
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


LAYOUT = '''// A stand-in for the generated layout.h (tests/sprint_check.py): main's image is a buffer of the test.
#pragma once
#include <stdint.h>
extern uint64_t test_main_base;
#define MOD_MAIN_BASE test_main_base
'''
PATCHES = '''// A stand-in for the generated patches.h (tests/sprint_check.py).
#pragma once
#define RT_PATCH_PAGE 0x10000ull
'''


def main():
    work = ROOT / 'build' / 'test' / 'sprint'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'layout.h').write_text(LAYOUT)
    (work / 'patches.h').write_text(PATCHES)
    exe = work / ('sprint_check.exe' if os.name == 'nt' else 'sprint_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing',
                           '-I', str(work), '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'sprint_check.c'), '-o', str(exe)], env=env)
    rc = subprocess.run([str(exe)], env=env).returncode
    return subprocess.run([str(exe), 'kept-off'], env=env).returncode or rc


if __name__ == '__main__':
    sys.exit(main())
