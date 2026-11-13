"""Build and run tests/tex_copy_mt_check.c with runtime/tex_deswizzle.c, runtime/par_jobs.c and the platform layer: the
split texture copy (tex_bl_copy_mt) and par_memcpy write the same bytes as the copies they stand for.

No GPU, no game files, any Python.

Usage: tex_copy_mt_check.py
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


def vma_include():
    """tex.h includes vk.h, which needs vk_mem_alloc.h (as tests/gpa_check.py finds it)."""
    for d in (MSYS_BIN.parent / 'include' / 'vma', ROOT / 'toolchain' / 'vma', Path('/usr/include')):
        if (d / 'vk_mem_alloc.h').is_file():
            return d
    sys.exit('tex_copy_mt_check: vk_mem_alloc.h not found (docs/building.md)')


def main():
    work = ROOT / 'build' / 'test' / 'tex_copy_mt'
    work.mkdir(parents=True, exist_ok=True)
    win = os.name == 'nt'
    exe = work / ('tex_copy_mt_check.exe' if win else 'tex_copy_mt_check')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    plat = [rt / 'platform' / ('win32.c' if win else 'linux.c'), rt / 'platform' / 'common.c']
    libs = ['-lbcrypt', '-lws2_32', '-lpsapi', '-lwinmm', '-ldbghelp', '-lshell32', '-lole32', '-luser32', '-ladvapi32'] if win \
        else ['-lpthread']
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing',
                           '-march=x86-64-v3', '-I', str(rt), '-I', str(vma_include()),
                           str(ROOT / 'tests' / 'tex_copy_mt_check.c'), str(rt / 'tex_deswizzle.c'), str(rt / 'par_jobs.c'), *map(str, plat), '-o', str(exe), '-lzstd', '-lm',
                           *libs], env=env)
    return subprocess.run([str(exe)], env=env, timeout=300).returncode


if __name__ == '__main__':
    sys.exit(main())
