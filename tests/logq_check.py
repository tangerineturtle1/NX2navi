"""Build and run tests/logq_check.c: the queue that hands log lines to the log thread (runtime/logq.c), with the
platform layer linked and a destination of the test's own. No game files: the module layout the runtime includes is a
stand-in written to the test's build folder.

After its checks the program is run twice more as a process that logs to a file and ends at once, by an exit and as
a fatal error does, while the log thread is still gathering its lines (ends): each file has to hold every line.

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
    failed = subprocess.run([str(exe)], env=env, timeout=300).returncode
    return ends(exe, work, env) or failed


END_LINES = 200   # tests/logq_check.c END_LINES


def ends(exe, work, env):
    """The end of a process whose lines the log thread is still gathering (tests/logq_check.c end_run): the exit code
    is the one the process ended with, and its file holds every line it logged, the fatal line last."""
    bad = 0
    for how, code in (('exit', 0), ('fatal', 70)):
        path = work / f'end_{how}.log'
        path.unlink(missing_ok=True)
        rc = subprocess.run([str(exe), how, path.name], cwd=work, env=env, timeout=60).returncode
        want = ''.join(f't 0 {i} \n' for i in range(END_LINES)) + ('FATAL: the end\n' if how == 'fatal' else '')
        got = path.read_bytes() if path.exists() else b''
        if rc != code or got != want.encode():
            print(f'FAIL: a process ended by "{how}": exit code {rc}, want {code}; its log holds {len(got)} bytes, '
                  f'{"the" if got == want.encode() else "not the"} {len(want)} it logged')
            bad = 1
    print(f'logq_check: the end of a process: {"FAILED" if bad else "ok"}')
    return bad


if __name__ == '__main__':
    sys.exit(main())
