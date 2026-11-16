"""Build and run tests/rumble_haptics_check.c: the DualSense haptics synthesis (runtime/rumble_haptics.c) with the
platform layer stubbed and SDL linked but its audio device never opened. No game files and no controller.

Usage: rumble_haptics_check.py
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
    work = ROOT / 'build' / 'test' / 'rumble_haptics'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('rumble_haptics_check.exe' if os.name == 'nt' else 'rumble_haptics_check')
    env = dict(os.environ)
    bin_dir = Path(compiler()).parent
    if os.name == 'nt':
        env['PATH'] = str(bin_dir) + os.pathsep + str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-Wall', '-Wno-unused-function', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'rumble_haptics_check.c'), '-o', str(exe), '-lSDL3'], env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
