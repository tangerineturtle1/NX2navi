"""Build and run tests/dlssg_device_check.c: runtime/vk_dlssg_device.h, which card counts as usable while DLSS Frame
Generation's interposer is in use and how the device is made when the interposer cannot make it (vk_device.c
pick_device, vk_dlssg.c vk_dlssg_create_device), with stand-ins for the interposer and the loader.

No GPU, no game files, any Python.

Usage: dlssg_device_check.py
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
    work = ROOT / 'build' / 'test' / 'dlssg_device'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('dlssg_device_check.exe' if os.name == 'nt' else 'dlssg_device_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-Wall', '-Werror', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'dlssg_device_check.c'), '-o', str(exe)], env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
