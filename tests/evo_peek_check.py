"""Build and run tests/evo_peek_check.c: runtime/vk_evo_peek.h, the copy of each event-ordered queue's first held wait
that vk_exec.c vk_exec_held_ready reads without the submission lock, and the decision it makes from that copy. One
writer republishes while three readers copy: no copy may mix two publishes, and no reader waits for the writer.

No GPU, no game files, any Python.

Usage: evo_peek_check.py
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
    work = ROOT / 'build' / 'test' / 'evo_peek'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('evo_peek_check.exe' if os.name == 'nt' else 'evo_peek_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    extra = [] if os.name == 'nt' else ['-pthread']
    subprocess.check_call([compiler(), '-std=gnu11', '-O2', '-Wall', '-Werror', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'evo_peek_check.c'), '-o', str(exe), *extra], env=env)
    try:
        return subprocess.run([str(exe)], env=env, timeout=120).returncode
    except subprocess.TimeoutExpired:
        print('evo_peek_check: FAIL: the check did not end within 120 s')
        return 1


if __name__ == '__main__':
    sys.exit(main())
