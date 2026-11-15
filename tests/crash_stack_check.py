"""Build and run tests/crash_stack_check.c: a stack overflow on a thread and on a fiber made by the platform layer
(runtime/platform/win32.c thread_main, fiber_start, plat_stack_guarantee) still gets its whole crash report, a file
write and three 2 KiB log lines, and ends with the report's code 71. Without the stack kept back the report faulted
again and the process ended with an access violation and no line. No game files.

Usage: crash_stack_check.py
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')

LAYOUT = '''// A stand-in for the generated layout.h (tests/crash_stack_check.py).
#pragma once
#define MOD_MAIN_BASE 0x7000000000ull
#define MOD_MAIN_SIZE 0x4ea0000ull
'''


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def main():
    if os.name != 'nt':
        print('crash_stack_check: Windows only')
        return 0
    work = ROOT / 'build' / 'test' / 'crash_stack'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'layout.h').write_text(LAYOUT)
    exe = work / 'crash_stack_check.exe'
    env = dict(os.environ)
    if MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    plat = [rt / 'platform' / 'win32.c', rt / 'platform' / 'common.c']
    libs = ['-lbcrypt', '-lws2_32', '-lpsapi', '-lwinmm', '-ldbghelp', '-lshell32', '-lole32', '-luser32', '-ladvapi32']
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing',
                           '-march=x86-64-v3', '-I', str(rt), '-I', str(work), str(ROOT / 'tests' / 'crash_stack_check.c'),
                           *map(str, plat), '-o', str(exe), '-lm', *libs], env=env)
    bad = 0
    for mode in ('thread', 'fiber'):
        note = work / f'note_{mode}.txt'
        note.unlink(missing_ok=True)
        p = subprocess.run([str(exe), mode, str(note)], env=env, capture_output=True, text=True, errors='replace',
                           timeout=60)
        lines = [f'report line {i}' for i in (1, 2, 3)]
        missing = [x for x in lines if x not in p.stdout]
        # platform.h plat_thread_name, which the crash report reads: the spawned thread's name; the fiber runs on the
        # main thread, which the platform layer did not make
        named = ('on thread deep' if mode == 'thread' else 'on thread (unnamed)') in p.stdout
        ok = p.returncode == 71 and not missing and 'stack overflow' in p.stdout and 'the file written: 1' in p.stdout and named
        print(f'a stack overflow on a {mode}: exit code {p.returncode & 0xffffffff:#x} (want 0x47), '
              f'{3 - len(missing)} of 3 report lines: {"ok" if ok else "FAIL"}', flush=True)
        if not ok:
            print(p.stdout[-2000:], p.stderr[-2000:])
        bad += not ok
    print(f'crash_stack_check: {"FAILED" if bad else "ok"}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
