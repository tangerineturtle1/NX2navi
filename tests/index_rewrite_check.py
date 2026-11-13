"""Build and run tests/index_rewrite_check.c: runtime/vk_index_rewrite.h, the index lists an indexed quad list or line
loop is drawn with (vk_exec_draw.c op_draw_idx, NX2NAVI_QUAD_INDEX): two triangles per quad from the quad's own
indices, the loop's first index again at its end.

No GPU, no game files, any Python.

Usage: index_rewrite_check.py
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
    work = ROOT / 'build' / 'test' / 'index_rewrite'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('index_rewrite_check.exe' if os.name == 'nt' else 'index_rewrite_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-Wall', '-Werror', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'index_rewrite_check.c'), '-o', str(exe)], env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
