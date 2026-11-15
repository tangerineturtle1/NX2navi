"""Build and run tests/dlssg_pace_check.c: runtime/dlssg_pace.h, whether Reflex or the game's own pacer holds the frame
limit with DLSS Frame Generation (vk_dlssg.c vk_dlssg_pace), and its checks of the frames Reflex held.

Needs a C compiler only: no Vulkan headers, no GPU and no game files.

Usage: dlssg_pace_check.py
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():
    """$CC, else MSYS2 UCRT64's clang, else clang on PATH (as tests/wb_lazy_check.py picks it)."""
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def main():
    work = ROOT / 'build' / 'test' / 'dlssg_pace'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('dlssg_pace_check.exe' if os.name == 'nt' else 'dlssg_pace_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    # undefined behaviour and unsigned wrap-around trap: every period and time in dlssg_pace.h is a uint64_t, which
    # -fsanitize=undefined alone lets wrap, so an overflow in that arithmetic fails the check instead of passing by
    # chance
    ub = 'undefined,unsigned-integer-overflow'
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-Wall', '-Werror', f'-fsanitize={ub}',
                           f'-fsanitize-trap={ub}', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'dlssg_pace_check.c'), '-o', str(exe)], env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
