"""Build and run tests/audio_mixer_check.c: runtime/audio_mixer.c with the rest of the runtime stubbed. No game files
and no audio device (the mixer runs as with NX2NAVI_AUDIO=none); SDL3 is only linked.

Windows: python tests\\audio_mixer_check.py [--toolchain C:/msys64/ucrt64]
Linux: python tests/audio_mixer_check.py (clang and SDL3 development files via pkg-config)
"""
import argparse
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS = Path(r'C:\msys64\ucrt64')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--toolchain', type=Path, default=MSYS if os.name == 'nt' and MSYS.exists() else None)
    a = ap.parse_args()
    work = ROOT / 'build' / 'test' / 'audio_mixer'
    work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    if a.toolchain:
        env['PATH'] = str(a.toolchain / 'bin') + os.pathsep + env.get('PATH', '')
        cc = os.environ.get('CC') or str(a.toolchain / 'bin' / 'clang.exe')
        sdl = ['-L', str(a.toolchain / 'lib'), '-lSDL3']
    else:
        cc = os.environ.get('CC') or shutil.which('clang') or 'cc'
        sdl = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl3'], text=True))
    exe = work / ('audio_mixer_check.exe' if os.name == 'nt' else 'audio_mixer_check')
    # the default build's target (x86-64-v3: AVX2 and FMA), so the mixing loops are compiled as they ship
    subprocess.check_call([cc, '-std=gnu11', '-O2', '-fno-strict-aliasing', '-march=x86-64-v3', '-Wall',
                           '-Wno-unused-function', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'audio_mixer_check.c'), '-o', str(exe), *sdl, '-lm'], env=env)
    return subprocess.run([str(exe)], env=env, timeout=60).returncode


if __name__ == '__main__':
    sys.exit(main())
