"""Build and run tests/system_event_check.c: the system-event table of runtime/kernel.c (kern_bind_system_event,
kern_signal_system_event, kern_clear_system_event, kern_unbind_system_event) against destroyed events, reused handle
numbers, a host thread racing a destroy and bind, a full table holding events in host memory, and host-memory events
unbound before their memory is freed; with the platform layer linked and the rest of the runtime stubbed. No game files: the module layout the runtime includes is a stand-in written to the test's build folder.

Usage: system_event_check.py
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

LAYOUT = '''// A stand-in for the generated layout.h (tests/system_event_check.py).
#pragma once
#define MOD_MAIN_BASE 0x7000000000ull
#define MOD_MAIN_SIZE 0x4ea0000ull
'''


def main():
    work = ROOT / 'build' / 'test' / 'system_event'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'layout.h').write_text(LAYOUT)
    win = os.name == 'nt'
    exe = work / ('system_event_check.exe' if win else 'system_event_check')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    plat = [rt / 'platform' / ('win32.c' if win else 'linux.c'), rt / 'platform' / 'common.c']
    libs = ['-lbcrypt', '-lws2_32', '-lpsapi', '-lwinmm', '-ldbghelp', '-lshell32', '-lole32', '-luser32', '-ladvapi32'] if win \
        else ['-lpthread']
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing',
                           '-march=x86-64-v3', '-I', str(rt), '-I', str(work),
                           str(ROOT / 'tests' / 'system_event_check.c'), *map(str, plat), '-o', str(exe), '-lm', *libs],
                          env=env)
    rc = subprocess.run([str(exe)], env=env, timeout=300).returncode
    # Every entry live: the next bind must end the process with "table full" (rt_fatal's stub exits 3).
    p = subprocess.run([str(exe), 'full'], env=env, timeout=300, capture_output=True, text=True)
    sys.stdout.write(p.stdout)
    if p.returncode != 3 or 'table full' not in p.stdout:
        print(f'FAIL: a bind with every entry live must end with "table full" (exit {p.returncode})')
        rc = rc or 1
    else:
        print('system_event_check full: ok (table full with every entry live)')
    return rc


if __name__ == '__main__':
    sys.exit(main())
