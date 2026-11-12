"""Build and run tests/prp_file_check.c: runtime/prp_file.h, the layout of shader-prepared.bin with what the list was
prepared from. The list the C side writes is then read by tools/make_game.py prepared_from, so the game's writer and
the build's reader cannot drift apart. No game files, no GPU.

Usage: prp_file_check.py
"""
import importlib.util
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
    work = ROOT / 'build' / 'test' / 'prp_file'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('prp_file_check.exe' if os.name == 'nt' else 'prp_file_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-Wall', '-Werror', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'prp_file_check.c'), '-o', str(exe)], env=env)
    rc = subprocess.run([str(exe), str(work)], env=env).returncode
    if rc:
        return rc
    sys.path.insert(0, str(ROOT / 'tools'))
    spec = importlib.util.spec_from_file_location('make_game', ROOT / 'tools' / 'make_game.py')
    mg = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mg)
    want = {'programs': bytes(range(32)).hex(), 'answers': bytes(range(0x40, 0x60)).hex(),
            'translator': bytes(range(0x80, 0xa0)).hex()}
    got = mg.prepared_from(work)
    if got != want:
        print(f'FAIL: tools/make_game.py prepared_from reads {got}, the game wrote {want}')
        return 1
    print('prepared list file: tools/make_game.py reads what the game writes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
