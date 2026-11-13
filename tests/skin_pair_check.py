"""Build and run tests/skin_pair_check.c: runtime/skin_pair.h, which decides the draw of last frame a skinned
character's draw continues (the same key twice in a frame, a character that appears, one that leaves, two that change
places, a palette that changes size).

No GPU, no game files, any Python.

Usage: skin_pair_check.py
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():
    """$CC, else MSYS2 UCRT64's clang, else clang on PATH (as tests/fg_reset_check.py picks it)."""
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def main():
    work = ROOT / 'build' / 'test' / 'skin_pair'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('skin_pair_check.exe' if os.name == 'nt' else 'skin_pair_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-Wall', '-Werror', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'skin_pair_check.c'), '-o', str(exe), '-lm'], env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
