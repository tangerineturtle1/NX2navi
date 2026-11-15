"""Build and run tests/up_defer_check.c: runtime/vk_up_defer.h, the list of what the upscalers let go of while the GPU
may still use it (vk_dlaa.c, vk_ffx.cpp, vk_ffx4.cpp). Built as C, and as C++ since the two FSR files include the
header from C++.

No GPU, no game files, any Python.

Usage: up_defer_check.py
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
    work = ROOT / 'build' / 'test' / 'up_defer'
    work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    src = str(ROOT / 'tests' / 'up_defer_check.c')
    rc = 0
    for lang, flags in (('c', ['-std=gnu11']), ('c++', ['-x', 'c++', '-std=gnu++17', '-pedantic'])):
        exe = work / (f'up_defer_check_{lang.replace("+", "x")}' + ('.exe' if os.name == 'nt' else ''))
        subprocess.check_call([compiler()] + flags + ['-O1', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'runtime'),
                               src, '-o', str(exe)], env=env)
        print(f'up_defer_check: as {lang}:', flush=True)
        rc |= subprocess.run([str(exe)], env=env).returncode
    return rc


if __name__ == '__main__':
    sys.exit(main())
