"""Build and run tests/fg_reset_check.c: runtime/vk_fg_reset.h, which frames FSR 3.1 frame generation's callback is
told are camera cuts.

No GPU, no game files, any Python.

Usage: fg_reset_check.py
"""
import os
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
    work = ROOT / 'build' / 'test' / 'fg_reset'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('fg_reset_check.exe' if os.name == 'nt' else 'fg_reset_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-Wall', '-Werror', '-pthread', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'fg_reset_check.c'), '-o', str(exe)], env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
