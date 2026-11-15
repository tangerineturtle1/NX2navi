"""Build and run the checks of runtime/rtss_limit.c, RivaTuner Statistics Server as the frame limiter.

tests/rtss_limit_check.c builds runtime/rtss_decide.h alone, the parts that make no system call. tests/rtss_worker_check.c
(Windows only, skipped elsewhere) compiles the real rtss_limit.c against a fake RTSS and runs each of its scenarios in a
process of its own. Checks statically that a crash and a fatal error (runtime/main.c crash_handler, runtime/dispatch.c
fatal_end) leave RTSS's profile to the next start through rtss_limit_crash, which calls into RTSS's library only
through the orderly restore and only when the state file holds no line of the run.

No GPU, no game files, no RTSS, any Python.

Usage: rtss_limit_check.py
"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():
    """$CC, else MSYS2 UCRT64's clang, else clang on PATH (as tests/diff/common.py picks it, without its Unicorn)."""
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def build(src, exe, env):
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-Wall', '-Werror', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / src), '-o', str(exe)], env=env)


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return re.sub(r'//[^\n]*', '', source[start:end])


def static_checks():
    rt = ROOT / 'runtime'
    bad = 0
    for file, sig in (('main.c', 'static void crash_handler(const PlatCrash *x)'),
                      ('dispatch.c', 'static void fatal_end(int code, const char *fmt, va_list ap) {')):
        body = function((rt / file).read_text(encoding='utf-8'), sig)
        ok = 'rtss_limit_crash(' in body and 'rtss_limit_restore(' not in body
        print(f'static: {file} {sig.split("(")[0].split()[-1]} leaves RTSS\'s profile to the next start '
              f'(rtss_limit_crash, no rtss_limit_restore): {"ok" if ok else "FAIL"}')
        bad += not ok
    crash = function((rt / 'rtss_limit.c').read_text(encoding='utf-8'), 'void rtss_limit_crash(void) {')
    calls = ('load_profile', 'save_profile', 'delete_profile', 'set_property', 'get_property', 'update_profiles',
             'put_back', 'write_limit', 'plat_mutex_lock', 'plat_mutex_trylock')
    gate, restore = crash.find('recorded'), crash.find('rtss_limit_restore(')
    ok = (crash.find('&closed, 1)') >= 0 and not any(c + '(' in crash for c in calls)
          and 0 <= gate < restore and 'rtss_tid' in crash[:restore])
    print(f'static: rtss_limit.c rtss_limit_crash closes the run and calls into RTSS only through the restore, when the '
          f'state file holds no line and not on the worker\'s thread: {"ok" if ok else "FAIL"}')
    return bad + (not ok)


def main():
    failed = ['static checks'] if static_checks() else []
    work = ROOT / 'build' / 'test' / 'rtss_limit'
    work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    pure = work / ('rtss_limit_check.exe' if os.name == 'nt' else 'rtss_limit_check')
    build('rtss_limit_check.c', pure, env)
    if subprocess.run([str(pure)], env=env).returncode != 0:
        failed.append('rtss_limit_check')
    if os.name != 'nt':
        print('rtss_worker_check: skipped (rtss_limit.c limits only on Windows)')
        return 1 if failed else 0
    exe = work / 'rtss_worker_check.exe'
    build('rtss_worker_check.c', exe, env)
    names = subprocess.run([str(exe), '--list'], env=env, capture_output=True, text=True, check=True).stdout.split()
    for name in names:
        try:
            p = subprocess.run([str(exe), name], env=env, capture_output=True, text=True, timeout=120)
            ok, out = p.returncode == 0, p.stdout + p.stderr
        except subprocess.TimeoutExpired as e:
            out = e.stdout if isinstance(e.stdout, str) else (e.stdout or b'').decode(errors='replace')
            ok, out = False, out + '\nTIMEOUT'
        print(f'rtss_worker_check {name}: {"ok" if ok else "FAIL"}')
        if not ok:
            print(out)
            failed.append(name)
    print(f'{len(names) + 2 - len(failed)} of {len(names) + 2} checks ok' + (f'; failed: {", ".join(failed)}' if failed else ''))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
