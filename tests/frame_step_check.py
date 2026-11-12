"""Build and run tests/frame_step_check.c: the step runtime/frame_rate.c gives Ocarina, with the rest of the runtime
stubbed. No game files: the module layout the runtime includes is a stand-in written to the test's build folder.

Usage: frame_step_check.py
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

LAYOUT = '''// A stand-in for the generated layout.h (tests/frame_step_check.py).
#pragma once
#define MOD_MAIN_BASE 0x7000000000ull
#define MOD_MAIN_SIZE 0x4ea0000ull
'''


def main():
    work = ROOT / 'build' / 'test' / 'frame_step'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'layout.h').write_text(LAYOUT)
    exe = work / ('frame_step_check.exe' if os.name == 'nt' else 'frame_step_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing',
                           '-I', str(ROOT / 'runtime'), '-I', str(work),
                           str(ROOT / 'tests' / 'frame_step_check.c'), '-o', str(exe), '-lm'], env=env)
    rc = subprocess.run([str(exe)], env=env).returncode
    return subprocess.run([str(exe), 'mean'], env=env).returncode or rc


if __name__ == '__main__':
    sys.exit(main())
