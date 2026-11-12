"""Build and run tests/logq_check.c: the queue that hands log lines to the log thread (runtime/logq.c), with the
platform layer linked and a destination of the test's own. No game files: the module layout the runtime includes is a
stand-in written to the test's build folder.

Usage: logq_check.py
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

LAYOUT = '''// A stand-in for the generated layout.h (tests/logq_check.py).
#pragma once
#define MOD_MAIN_BASE 0x7000000000ull
#define MOD_MAIN_SIZE 0x4ea0000ull
'''


def main():
    work = ROOT / 'build' / 'test' / 'logq'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'layout.h').write_text(LAYOUT)
    win = os.name == 'nt'
    exe = work / ('logq_check.exe' if win else 'logq_check')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    plat = [rt / 'platform' / ('win32.c' if win else 'linux.c'), rt / 'platform' / 'common.c']
    libs = ['-lbcrypt', '-lws2_32', '-lpsapi', '-lwinmm', '-ldbghelp', '-lshell32', '-lole32', '-luser32', '-ladvapi32'] if win \
        else ['-lpthread']
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing',
                           '-march=x86-64-v3', '-I', str(rt), '-I', str(work),
                           str(ROOT / 'tests' / 'logq_check.c'), *map(str, plat), '-o', str(exe), '-lm', *libs],
                          env=env)
    return subprocess.run([str(exe)], env=env, timeout=300).returncode


if __name__ == '__main__':
    sys.exit(main())
