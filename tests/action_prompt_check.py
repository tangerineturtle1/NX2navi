"""Build and run tests/action_prompt_check.c: runtime/action_prompt.c, the action prompt's diamond of face buttons laid
out by label, on a layout file and archives the test makes itself. No game files.

With --romfs, also on the game's own archive: the folder holds romfs.bin and its index.json, and the program reads the
archive and Pack/ZsDic.pack.zs from romfs.bin at the offsets the index gives. Nothing is written.

The program is built with the undefined-behaviour sanitizer (traps), with locals that start as a pattern rather than
whatever the stack held, and with AddressSanitizer where the toolchain has it (not MSYS2's).

Usage: action_prompt_check.py [--romfs DIR]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
ARCHIVE = 'UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs'
DICTIONARIES = 'Pack/ZsDic.pack.zs'


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def game_args(folder):
    """romfs.bin and the offset and size of the archive and the dictionaries in it."""
    files = {f['path']: f for f in json.loads((folder / 'index.json').read_text(encoding='utf-8'))['files']}
    for rel in (ARCHIVE, DICTIONARIES):
        if rel not in files:
            raise SystemExit(f'{folder / "index.json"} has no {rel}')
    return [str(folder / 'romfs.bin')] + [str(files[rel][k]) for rel in (ARCHIVE, DICTIONARIES)
                                          for k in ('offset', 'size')]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--romfs', type=Path, help='a folder with romfs.bin and index.json: also check the game\'s archive')
    args = ap.parse_args()
    work = ROOT / 'build' / 'test' / 'action_prompt'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('action_prompt_check.exe' if os.name == 'nt' else 'action_prompt_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    sanitize = ['-fsanitize=undefined', '-fsanitize-trap=all', '-ftrivial-auto-var-init=pattern']
    if os.name != 'nt':
        sanitize.append('-fsanitize=address')
    rt = ROOT / 'runtime'
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wextra', '-Werror', *sanitize, '-I', str(rt),
                           str(ROOT / 'tests' / 'action_prompt_check.c'), str(rt / 'action_prompt.c'),
                           str(rt / 'sarc.c'), '-o', str(exe), '-lzstd', '-lm'], env=env)
    run = [str(exe)] + (game_args(args.romfs) if args.romfs else [])
    return subprocess.run(run, env=env, timeout=300).returncode


if __name__ == '__main__':
    sys.exit(main())
