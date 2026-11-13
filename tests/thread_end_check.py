"""Build and run tests/thread_end_check.c: what a thread keeps for itself is given back when it ends
(runtime/platform/win32.c thread_main, platform.h plat_thread_end_hook). 416 threads sleep, read a file and end, half
joined and half detached; the end hook must run once on each, and on Windows the process's handle count must be what
it was. No game files, no GPU.

Usage: thread_end_check.py
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


def main():
    work = ROOT / 'build' / 'test' / 'thread_end'
    work.mkdir(parents=True, exist_ok=True)
    win = os.name == 'nt'
    exe = work / ('thread_end_check.exe' if win else 'thread_end_check')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    plat = [rt / 'platform' / ('win32.c' if win else 'linux.c'), rt / 'platform' / 'common.c']
    libs = ['-lbcrypt', '-lws2_32', '-lpsapi', '-lwinmm', '-ldbghelp', '-lshell32', '-lole32', '-luser32', '-ladvapi32'] if win \
        else ['-lpthread']
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing',
                           '-march=x86-64-v3', '-I', str(rt),
                           str(ROOT / 'tests' / 'thread_end_check.c'), *map(str, plat), '-o', str(exe), '-lm', *libs],
                          env=env)
    data = work / 'read.bin'
    data.write_bytes(bytes(range(64)))
    return subprocess.run([str(exe), str(data)], env=env, timeout=300).returncode


if __name__ == '__main__':
    sys.exit(main())
