"""Build and run tests/handle_census_check.c: the census of the process's handles by kind (runtime/platform/win32.c
plat_handle_census) sees 20 events and 5 sections made and closed, and its total agrees with the process's handle
count. Windows only (elsewhere the program says it skipped). No game files, no GPU.

Usage: handle_census_check.py
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
    work = ROOT / 'build' / 'test' / 'handle_census'
    work.mkdir(parents=True, exist_ok=True)
    win = os.name == 'nt'
    exe = work / ('handle_census_check.exe' if win else 'handle_census_check')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    plat = [rt / 'platform' / ('win32.c' if win else 'linux.c'), rt / 'platform' / 'common.c']
    libs = ['-lbcrypt', '-lws2_32', '-lpsapi', '-lwinmm', '-ldbghelp', '-lshell32', '-lole32', '-luser32', '-ladvapi32'] if win \
        else ['-lpthread']
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing',
                           '-march=x86-64-v3', '-I', str(rt),
                           str(ROOT / 'tests' / 'handle_census_check.c'), *map(str, plat), '-o', str(exe), '-lm', *libs],
                          env=env)
    return subprocess.run([str(exe)], env=env, timeout=120).returncode


if __name__ == '__main__':
    sys.exit(main())
