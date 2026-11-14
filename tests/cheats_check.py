"""Build the real cheat policies with synthetic guest memory; no game files required."""
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    cc = os.environ.get('CC') or shutil.which('clang') or r'C:\msys64\ucrt64\bin\clang.exe'
    work = ROOT / 'build/test/cheats'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('cheats_check.exe' if os.name == 'nt' else 'cheats_check')
    env = dict(os.environ)
    env['PATH'] = str(Path(cc).parent) + os.pathsep + env.get('PATH', '')
    subprocess.run([cc, '-O2', '-Wall', '-Wextra', '-Wno-unused-function', '-ffunction-sections', '-fdata-sections',
                    '-I', str(ROOT / 'runtime'), str(ROOT / 'tests/cheats_check.c'), str(ROOT / 'tests/cheats_stubs.c'),
                    '-Wl,--gc-sections', '-o', str(exe), '-lm'], env=env, check=True)
    subprocess.run([str(exe)], env=env, check=True)


if __name__ == '__main__':
    main()
