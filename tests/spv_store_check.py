"""Build and run tests/spv_store_check.c: runtime/spv_store.h, which keeps a shader's SPIR-V zstd-compressed while
nobody reads it (runtime/gfx_shader.c "SPIR-V at rest"). The check walks one store through pin, pack, decode and idle
again, and runs reader threads against a packer that compresses every store as soon as it is free: no reader may see
other words than the original, and every pin must be given back. Linked with the platform layer and libzstd.

No GPU, no game files, any Python.

Usage: spv_store_check.py
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
    work = ROOT / 'build' / 'test' / 'spv_store'
    work.mkdir(parents=True, exist_ok=True)
    win = os.name == 'nt'
    exe = work / ('spv_store_check.exe' if win else 'spv_store_check')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    plat = [rt / 'platform' / ('win32.c' if win else 'linux.c'), rt / 'platform' / 'common.c']
    libs = ['-lbcrypt', '-lws2_32', '-lpsapi', '-lwinmm', '-ldbghelp', '-lshell32', '-lole32', '-luser32', '-ladvapi32'] if win \
        else ['-lpthread']
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing',
                           '-march=x86-64-v3', '-I', str(rt),
                           str(ROOT / 'tests' / 'spv_store_check.c'), *map(str, plat), '-o', str(exe), '-lzstd', '-lm',
                           *libs], env=env)
    return subprocess.run([str(exe)], env=env, timeout=300).returncode


if __name__ == '__main__':
    sys.exit(main())
