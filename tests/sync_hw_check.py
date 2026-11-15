"""Build and run tests/sync_hw_check.c: runtime/vk_sync_hw.h, the newest value each of the replay's timelines may
reach and the test that takes a value read above it as a lost device (vk_sync.c "a lost device in the timelines'
values").

No GPU, no game files, any Python.

Usage: sync_hw_check.py
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
    work = ROOT / 'build' / 'test' / 'sync_hw'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('sync_hw_check.exe' if os.name == 'nt' else 'sync_hw_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    extra = [] if os.name == 'nt' else ['-pthread']
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-Wall', '-Werror', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'sync_hw_check.c'), '-o', str(exe), *extra], env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
