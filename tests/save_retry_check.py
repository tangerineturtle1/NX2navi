"""Build and run tests/save_retry_check.c: runtime/vk_save_retry.h, when the autosave thread saves the pipeline caches
and when it tries again after a save that did not happen (no copy of the data, a file that cannot be written).

Usage: save_retry_check.py
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests' / 'diff'))
try:
    from common import MSYS_BIN, compiler  # noqa: E402
except ImportError:   # a Python without the differential tests' packages: the same compiler choice without them
    MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')

    def compiler():
        if os.environ.get('CC'):
            return os.environ['CC']
        if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
            return str(MSYS_BIN / 'clang.exe')
        return shutil.which('clang') or 'clang'


def main():
    work = ROOT / 'build' / 'test' / 'save_retry'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('save_retry_check.exe' if os.name == 'nt' else 'save_retry_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-Wall', '-Werror', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'save_retry_check.c'), '-o', str(exe)], env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
