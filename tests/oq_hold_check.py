"""Build and run tests/oq_hold_check.c: runtime/vk_oq_hold.h, the count a samples-passed report writes when the full
occlusion query ring left some of its draws uncounted (vk_exec_xfer.c oq_report_count, NX2NAVI_OQ_HOLD): the larger of
what was counted and the record's last complete count.

No GPU, no game files, any Python.

Usage: oq_hold_check.py
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
    work = ROOT / 'build' / 'test' / 'oq_hold'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('oq_hold_check.exe' if os.name == 'nt' else 'oq_hold_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-Wall', '-Werror', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'oq_hold_check.c'), '-o', str(exe)], env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
