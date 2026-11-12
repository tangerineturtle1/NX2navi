"""Build and run tests/build_gate_check.c: runtime/vk_build_gate.h, the memory gate of the background pipeline builds:
what it admits and refuses by the system's commit, and that the builds under way count through their reservations.

With the game executable's path it also runs the executable's --probe-buildgate (runtime/vk_pipeline.c
vk_pipeline_probe_buildgate: the gate as the builders call it, with this machine's commit; no device, no window, no
build) three times: with the settings in effect, with build_free_mb = 0, where every build must be admitted, and
with build_free_mb at a value no machine has free, where every build must be refused.

Usage: build_gate_check.py [PATH_TO_GAME_EXE]
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests' / 'diff'))
try:
    from common import MSYS_BIN, compiler  # noqa: E402
except ImportError:   # a Python without the differential tests' packages: the same compiler choice without them
    MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')

    def compiler():
        if os.environ.get('CC'):
            return os.environ['CC']
        if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
            return str(MSYS_BIN / 'clang.exe')
        return shutil.which('clang') or 'clang'


def main():
    work = ROOT / 'build' / 'test' / 'build_gate'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('build_gate_check.exe' if os.name == 'nt' else 'build_gate_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-Wall', '-Werror', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'build_gate_check.c'), '-o', str(exe)], env=env)
    rc = subprocess.run([str(exe)], env=env).returncode
    if len(sys.argv) > 1:
        rc |= probe(sys.argv[1])
    return rc


def probe(game):
    """The executable's --probe-buildgate under three settings; 0 when each run passes and says what it must."""
    fails = 0
    for free_mb, must in ((None, None), ('0', 'an ordinary build is admitted now'),
                          ('1048576', 'an ordinary build is refused now')):
        env = dict(os.environ)
        env.pop('NX2NAVI_BUILD_FREE_MB', None)
        if free_mb is not None:
            env['NX2NAVI_BUILD_FREE_MB'] = free_mb
        r = subprocess.run([game, '--probe-buildgate'], env=env, capture_output=True, text=True, errors='replace')
        lines = [ln for ln in r.stdout.splitlines() if ln.startswith('probe-buildgate:')]
        print('\n'.join(lines))
        ok = r.returncode == 0 and lines and lines[-1].endswith('passed') and (must is None or must in lines[-1])
        if free_mb == '0':
            ok = ok and any('16 of 16 ordinary builds admitted' in ln and 'admitted' in ln.split(';')[-1] for ln in lines)
        if free_mb == '1048576':
            ok = ok and any('0 of 16 ordinary builds admitted' in ln and 'refused' in ln.split(';')[-1] for ln in lines)
        if not ok:
            fails += 1
            print('build_gate_check: FAIL: --probe-buildgate with build_free_mb %s (exit code %d)'
                  % (free_mb or 'as set', r.returncode))
    print('build_gate_check: the executable\'s gate %s' % ('FAILED' if fails else 'passed'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
