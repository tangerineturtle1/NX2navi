"""Build and run tests/pipeline_cache_file_check.c: runtime/vk_cache_file.h, the read of pipeline-cache.bin with 64-bit
sizes. The test makes a sparse file just over 2 GB in build/test/pipeline_cache_file (it takes no disk space, and about
2 GB of memory for the seconds the read lasts) and removes it.

Usage: pipeline_cache_file_check.py
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def main():
    work = ROOT / 'build' / 'test' / 'pipeline_cache_file'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('pipeline_cache_file_check.exe' if os.name == 'nt' else 'pipeline_cache_file_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-Wall', '-Werror', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'pipeline_cache_file_check.c'), '-o', str(exe)], env=env)
    return subprocess.run([str(exe), str(work)], env=env, timeout=600).returncode


if __name__ == '__main__':
    sys.exit(main())
