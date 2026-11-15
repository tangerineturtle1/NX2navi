"""Build and run tests/hang_watch_check.c: runtime/hang_watch.c's decisions on made-up readings. No report while the
frames and the GPU move, while the window is not in front or minimized, or while the process ends; one report for a
stall of the frames with the window in front, another after progress and a new stall, one for GPU submissions not
done whatever the focus, none for an idle GPU, at most three a run. A report writes the log out with logq_flush and
never with logq_sync, which would turn the log thread off. Also checks statically that runtime/kernel.c logs the guest
threads' waits (kern_wait_report, wait_watch) only after it released the kernel lock: a log call can wait for room in
the log queue, and a stalled log thread would then hold every guest thread. No game files, no GPU.

Usage: hang_watch_check.py
"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')

LAYOUT = '''// A stand-in for the generated layout.h (tests/hang_watch_check.py).
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


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def static_checks():
    """kernel.c: no log call while G is held in the wait report's walk and its two callers."""
    kernel = re.sub(r'//[^\n]*', '', (ROOT / 'runtime' / 'kernel.c').read_text(encoding='utf-8'))
    bad = 0
    walk = function(kernel, 'static int wait_pass(')
    ok = 'rt_log' not in walk
    print(f'static: kernel.c wait_pass formats its lines and logs none: {"ok" if ok else "FAIL"}', flush=True)
    bad += not ok
    for sig in ('void kern_wait_report(void)', 'static void wait_watch(void *arg)'):
        body = function(kernel, sig)
        held = body[body.index('wait_pass('):body.index('unlock()')]
        ok = 'rt_log' not in held and 'wait_lines_log(' in body[body.index('unlock()'):]
        print(f'static: kernel.c {sig.split("(")[0].split()[-1]} logs the waits after unlock: {"ok" if ok else "FAIL"}',
              flush=True)
        bad += not ok
    return bad


def main():
    bad = static_checks()
    work = ROOT / 'build' / 'test' / 'hang_watch'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'layout.h').write_text(LAYOUT)
    win = os.name == 'nt'
    exe = work / ('hang_watch_check.exe' if win else 'hang_watch_check')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    plat = [rt / 'platform' / ('win32.c' if win else 'linux.c'), rt / 'platform' / 'common.c']
    libs = ['-lbcrypt', '-lws2_32', '-lpsapi', '-lwinmm', '-ldbghelp', '-lshell32', '-lole32', '-luser32', '-ladvapi32'] if win \
        else ['-lpthread']
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing',
                           '-march=x86-64-v3', '-I', str(rt), '-I', str(work), str(ROOT / 'tests' / 'hang_watch_check.c'),
                           *map(str, plat), '-o', str(exe), '-lm', *libs], env=env)
    rc = subprocess.run([str(exe)], env=env, timeout=120).returncode
    if bad:
        print('hang_watch_check: the static checks FAILED')
    return 1 if bad or rc else 0


if __name__ == '__main__':
    sys.exit(main())
