"""Build and run tests/flight_flash_check.c: runtime/flight_flash.h, which frames the flight recorder takes for a
one-frame flash (a flash, a camera cut, steady motion, frames whose neighbours have no thumbnail).

No GPU, no game files, any Python. Built as the runtime is (-O2, x86-64-v3 on x86-64), so the "time" run measures what
the present thread pays.

Usage: flight_flash_check.py [time]
"""
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():
    """$CC, else MSYS2 UCRT64's clang, else clang on PATH (as tests/diff/common.py picks it, without its Unicorn)."""
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def main():
    work = ROOT / 'build' / 'test' / 'flight_flash'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('flight_flash_check.exe' if os.name == 'nt' else 'flight_flash_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    arch = ['-march=x86-64-v3'] if platform.machine().lower() in ('amd64', 'x86_64') else []
    subprocess.check_call([compiler(), '-std=gnu11', '-O2', *arch, '-Wall', '-Wextra', '-Werror',
                           '-I', str(ROOT / 'runtime'), str(ROOT / 'tests' / 'flight_flash_check.c'), '-o', str(exe)],
                          env=env)
    return subprocess.run([str(exe), *sys.argv[1:2]], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
