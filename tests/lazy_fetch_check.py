"""Build and run tests/lazy_fetch_check.c: runtime/memory.c's lazy write-back (memory.c "Lazy write-back", "Ranges in
flight") with the Windows platform layer and its real fault handler, the rest of the runtime stubbed and the back end's
fetch a stand-in that only waits. It checks that a fetch holds up nothing but the touches of its range, that those
wait for it, and that arms, thaws, mapping changes, freezes and releases see it; also with NX2NAVI_WB_LAZY_CHUNK=1 and
with NX2NAVI_WB_LAZY_UNLOCKED=0. Each scenario runs as its own process, so an access violation passed to the crash
handler shows as its exit code; any exit code other than 0, or a scenario that does not end within 120 s, fails.

Windows only (the fault handler is): no game files, no GPU. The module layout the runtime includes is a stand-in
written to the test's build folder.

Usage: lazy_fetch_check.py [scenario ...]
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
# name: (scenario argument, extra environment)
CHUNK = {'CHECK_CHUNK': '1'}     # NX2NAVI_WB_LAZY_CHUNK=1
LOCKED = {'CHECK_LOCKED': '1'}   # NX2NAVI_WB_LAZY_UNLOCKED=0
SCENARIOS = {
    'arm_race': ('arm_race', {}),
    'arm_race_slow': ('arm_race_slow', {}),
    'no_stall': ('no_stall', {}),
    'join': ('join', {}),
    'loser': ('loser', {}),
    'late_write': ('late_write', {}),
    'concurrent': ('concurrent', {}),
    'thaw': ('thaw', {}),
    'thaw_unlocked': ('thaw_unlocked', {}),
    'locked_thaw': ('thaw_unlocked', LOCKED),
    'mapping': ('mapping', {}),
    'release': ('release', {}),
    'release_join': ('release_join', {}),
    'arm_busy': ('arm_busy', {}),
    'arm_flight': ('arm_flight', {}),
    'foreign': ('foreign', {}),
    'chunk': ('chunk', CHUNK),
    'locked': ('locked', LOCKED),
    'locked_join': ('join', LOCKED),
    'locked_loser': ('loser', LOCKED),
    'locked_arm_race': ('arm_race_slow', LOCKED),
    'locked_release': ('release', LOCKED),
    'report': ('report', {}),
    'flood': ('flood', {}),
    'freeze': ('freeze', {}),
}


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


LAYOUT = '''// A stand-in for the generated layout.h (tests/lazy_fetch_check.py).
#pragma once
#define MOD_MAIN_BASE 0x7000000000ull
#define MOD_MAIN_SIZE 0x4ea0000ull
#define MOD_COUNT 3
'''


def main():
    if os.name != 'nt':
        print('lazy_fetch_check: the fault handler it runs is Windows only; skipped')
        return 0
    names = sys.argv[1:] or list(SCENARIOS)
    unknown = [n for n in names if n not in SCENARIOS]
    if unknown:
        print('lazy_fetch_check: unknown scenario %s; known: %s' % (', '.join(unknown), ', '.join(SCENARIOS)))
        return 2
    work = ROOT / 'build' / 'test' / 'lazy_fetch'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'layout.h').write_text(LAYOUT)
    exe = work / 'lazy_fetch_check.exe'
    env = dict(os.environ)
    if MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    plat = [rt / 'platform' / 'win32.c', rt / 'platform' / 'common.c']
    libs = ['-lbcrypt', '-lws2_32', '-lpsapi', '-lwinmm', '-ldbghelp', '-lshell32', '-lole32', '-luser32', '-ladvapi32']
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing',
                           '-march=x86-64-v3', '-I', str(rt), '-I', str(work),
                           str(ROOT / 'tests' / 'lazy_fetch_check.c'), *map(str, plat), '-o', str(exe), '-lm',
                           *libs], env=env)
    bad = 0
    for name in names:
        arg, extra = SCENARIOS[name]
        try:
            p = subprocess.run([str(exe), arg], env={**env, **extra}, capture_output=True, text=True, timeout=120)
        except subprocess.TimeoutExpired:
            bad += 1
            print('lazy_fetch_check: %s: FAIL: did not end within 120 s' % name)
            continue
        code = p.returncode & 0xFFFFFFFF
        if code:
            bad += 1
            print('lazy_fetch_check: %s: FAIL: exit code %#x%s' % (
                name, code, ' (an access violation)' if code == 0xC0000005 else ''))
            sys.stdout.write(p.stdout)
        else:
            print('lazy_fetch_check: %s: passed' % name)
    if bad:
        print('lazy_fetch_check: %d of %d scenarios failed' % (bad, len(names)))
        return 1
    print('lazy_fetch_check: all %d scenarios passed' % len(names))
    return 0


if __name__ == '__main__':
    sys.exit(main())
