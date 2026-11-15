"""Build and run tests/audio_params_check.c: runtime/audren.c and runtime/audio_mixer.c with the rest of the runtime
stubbed and the real effect DSPs (runtime/audio_effects.c, runtime/audio_reverb.c) linked. It sends RequestUpdates the
renderer cannot use: refused ones must answer 0x5299 and change nothing, records, effects and sinks are left out of
updates that are otherwise applied, each kind is logged once, and with NX2NAVI_AUDIO_STRICT=1 such a value ends the
process (two runs of their own, which must exit 2 with "FATAL: audio:"). No game files and no audio device.

Windows: python tests\\audio_params_check.py [--toolchain C:/msys64/ucrt64] [-v]
Linux: python tests/audio_params_check.py (clang and SDL3 development files via pkg-config)
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
    ap.add_argument('-v', action='store_true', help='print the log lines of the checks')
    a = ap.parse_args()
    work = ROOT / 'build' / 'test' / 'audio_params'
    work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    if a.toolchain:
        env['PATH'] = str(a.toolchain / 'bin') + os.pathsep + env.get('PATH', '')
        cc = os.environ.get('CC') or str(a.toolchain / 'bin' / 'clang.exe')
        sdl = ['-L', str(a.toolchain / 'lib'), '-lSDL3']
    else:
        cc = os.environ.get('CC') or shutil.which('clang') or 'cc'
        sdl = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl3'], text=True))
    exe = work / ('audio_params_check.exe' if os.name == 'nt' else 'audio_params_check')
    # three translation units: both effect files have a static gain10; the default build's target (x86-64-v3)
    subprocess.check_call([cc, '-std=gnu11', '-O2', '-fno-strict-aliasing', '-march=x86-64-v3', '-Wall',
                           '-Wno-unused-function', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'audio_params_check.c'), str(ROOT / 'runtime' / 'audio_effects.c'),
                           str(ROOT / 'runtime' / 'audio_reverb.c'), '-o', str(exe), *sdl, '-lm'], env=env)
    fails = 0
    p = subprocess.run([str(exe)] + (['-v'] if a.v else []), env=env, timeout=120, capture_output=True, text=True)
    sys.stdout.write(p.stdout + p.stderr)
    if p.returncode != 0:
        fails += 1
    for case in ('strict-effect', 'strict-update'):
        p = subprocess.run([str(exe), case], env=env, timeout=60, capture_output=True, text=True)
        text = p.stdout + p.stderr
        ok = p.returncode == 2 and 'FATAL: audio:' in text and 'NX2NAVI_AUDIO_STRICT=1' in text
        print(f'audio_params_check {case}: {"ok" if ok else "FAIL"} (exit {p.returncode}: {text.strip()[-200:]})')
        fails += not ok
    print('audio_params_check.py:', 'FAIL' if fails else 'ok')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
