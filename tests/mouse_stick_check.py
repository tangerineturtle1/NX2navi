"""Build and run tests/mouse_stick_check.c with runtime/input_map.c: the mouse camera's stick deflection at each camera
speed. No game files.

Usage: python tests/mouse_stick_check.py [--cc <clang>]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc', help='the C compiler (default: MSYS2 UCRT64 clang on Windows, clang elsewhere)')
    args = ap.parse_args()
    win = os.name == 'nt'
    cc = args.cc or (str(MSYS_BIN / 'clang.exe') if win and MSYS_BIN.exists() else 'clang')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    with tempfile.TemporaryDirectory(prefix='nx2navi-mouse-stick-') as tmp:
        exe = Path(tmp) / ('check.exe' if win else 'check')
        subprocess.check_call([cc, '-O2', '-Wall', '-fno-strict-aliasing', '-I', str(ROOT / 'runtime'),
                               str(ROOT / 'tests' / 'mouse_stick_check.c'), str(ROOT / 'runtime' / 'input_map.c'),
                               *([] if win else ['-lm']), '-o', str(exe)], env=env)
        return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
