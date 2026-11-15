"""Build and run tests/present_held_check.c: runtime/vk_present_held.h, the present that waits for held work
(vk_backend.c "a present that waits for held work"). One presenting thread and three submitting threads over 200,000
frames: every frame reaches the present exactly once and in order, never two at once, and each reason a held present
goes out occurs. Step by step: a held present stays while work is held, goes out after the work, at the next present
or before a later present, and only once. With the lock held elsewhere a submitting thread gives up at once, and the
presenting thread waits and counts its wait. Then the unlocked 0.10.1 shape runs through the same harness and only
prints what it saw, to show that the harness catches the race.

No GPU, no game files, any Python.

Usage: present_held_check.py
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():
    """$CC, else MSYS2 UCRT64's clang, else clang on PATH."""
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def main():
    work = ROOT / 'build' / 'test' / 'present_held'
    work.mkdir(parents=True, exist_ok=True)
    win = os.name == 'nt'
    exe = work / ('present_held_check.exe' if win else 'present_held_check')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    plat = [rt / 'platform' / ('win32.c' if win else 'linux.c'), rt / 'platform' / 'common.c']
    libs = ['-lbcrypt', '-lws2_32', '-lpsapi', '-lwinmm', '-ldbghelp', '-lshell32', '-lole32', '-luser32', '-ladvapi32'] if win \
        else ['-lpthread']
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-g', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing',
                           '-march=x86-64-v3', '-I', str(rt), str(ROOT / 'tests' / 'present_held_check.c'),
                           *map(str, plat), '-o', str(exe), '-lm', *libs], env=env)
    try:
        rc = subprocess.run([str(exe)], env=env, timeout=300).returncode
        if rc == 0:
            subprocess.run([str(exe), 'old'], env=env, timeout=300)
        return rc
    except subprocess.TimeoutExpired:
        print('present_held_check: FAIL: the check did not end within 300 s')
        return 1


if __name__ == '__main__':
    sys.exit(main())
