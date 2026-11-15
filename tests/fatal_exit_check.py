"""Build and run tests/fatal_exit_check.c: the end of a fatal error (runtime/dispatch.c fatal_end) with the platform
layer linked. The process ends with its own code and its FATAL line, and without the libraries' detach calls that
ExitProcess makes (a driver's detach that faulted there turned the end into a crash report with another code). Also
checks statically that no runtime file outside the platform layer ends the process with _Exit, and that the fatal
error's end and the lost device's (runtime/vk_device.c lost_wait_exit) end with plat_terminate. No game files, no GPU.

A fatal error stops HD rumble on a thread of its own and waits for it 300 ms at most (runtime/dispatch.c
rt_rumble_stop_bounded), so a stop that hangs on SDL's lock does not hold the end. RivaTuner's profile is left to the
next start (runtime/rtss_limit.c rtss_limit_crash), which on RTSS's own thread ("rtss"), whose lock that thread may
hold, makes no call into RTSS's library and takes no lock.

Two failures at once end once: two threads that fault, a fault while a fatal error is reported and a fatal error
while a crash is: the first report is written whole, the process ends with its code, and the second adds no line
(runtime/dispatch.c rt_ending_enter, which runtime/main.c crash_handler calls first).

Usage: fatal_exit_check.py
"""
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / 'runtime'
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')

LAYOUT = '''// A stand-in for the generated layout.h (tests/fatal_exit_check.py).
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
    """_Exit is ExitProcess in the C runtime: the platform layer alone ends the process (plat_exit, plat_terminate)."""
    bad = 0
    for path in sorted(RT.rglob('*')):
        rel = path.relative_to(RT).as_posix()
        if path.suffix not in ('.c', '.cpp', '.h', '.inc') or rel.startswith(('platform/', 'third_party/')):
            continue
        code = re.sub(r'//[^\n]*', '', path.read_text(encoding='utf-8', errors='replace'))
        for m in re.finditer(r'\b_Exit\s*\(', code):
            print(f'FAIL: runtime/{rel}:{code.count(chr(10), 0, m.start()) + 1} ends the process with _Exit '
                  '(ExitProcess: every library\'s detach runs); use plat_terminate or plat_exit')
            bad += 1
    for file, sig in (('dispatch.c', 'static void fatal_end(int code, const char *fmt, va_list ap) {'),
                      ('vk_device.c', 'static void lost_wait_exit(void *arg)')):
        body = re.sub(r'//[^\n]*', '', function((RT / file).read_text(encoding='utf-8'), sig))
        if 'plat_terminate(' not in body:
            print(f'FAIL: runtime/{file} {sig.split("(")[0].split()[-1]} does not end with plat_terminate')
            bad += 1
    print(f'static: no _Exit outside the platform layer, fatal_end and lost_wait_exit end with plat_terminate: '
          f'{"FAILED" if bad else "ok"}', flush=True)
    # the crash handler's guard is the one the stand-in in tests/fatal_exit_check.c runs
    main_c = (RT / 'main.c').read_text(encoding='utf-8')
    body = re.sub(r'//[^\n]*', '', function(main_c, 'static void crash_handler(const PlatCrash *x)'))
    first = body[body.index('{') + 1:].strip().split('\n')[0].strip()
    ok = first == 'if (!rt_ending_enter(71)) plat_exit(71);'
    print(f'static: main.c crash_handler starts with the shared guard: {"ok" if ok else "FAIL (" + first + ")"}',
          flush=True)
    bad += not ok
    # the fatal error's summary leaves out what the failure may have left half made, and only tries the map's lock
    flush = re.sub(r'//[^\n]*', '', function((RT / 'perf.c').read_text(encoding='utf-8'), 'void perf_summary_flush_try(void)'))
    ok = ('stutter_report_as(' in flush and 'stutter_report(' not in flush and 'gfx_exit_log' not in flush
          and 'seg_close' not in flush)
    print(f'static: perf.c perf_summary_flush_try tries the stutter map\'s lock and leaves the back end out: '
          f'{"ok" if ok else "FAIL"}', flush=True)
    bad += not ok
    # both failed ends stop HD rumble through the bounded stop, after RivaTuner's profile is left to the next start
    fatal = re.sub(r'//[^\n]*', '', function((RT / 'dispatch.c').read_text(encoding='utf-8'),
                                              'static void fatal_end(int code, const char *fmt, va_list ap) {'))
    for name, code in (('dispatch.c fatal_end', fatal), ('main.c crash_handler', body)):
        r, s = code.find('rtss_limit_crash('), code.find('rt_rumble_stop_bounded(')
        ok = 0 <= r < s and 'input_rumble_hd_stop(' not in code
        print(f'static: {name} stops HD rumble through rt_rumble_stop_bounded after rtss_limit_crash: '
              f'{"ok" if ok else "FAIL"}', flush=True)
        bad += not ok
    # a first report that hangs: the thread that waits for it ends the process after 30 s with that report's code, and
    # fatal_end hands its own code to the guard (a hung FATAL must not read as a crash to the launcher)
    dispatch = re.sub(r'//[^\n]*', '', (RT / 'dispatch.c').read_text(encoding='utf-8'))
    enter = function(dispatch, 'int rt_ending_enter(int code)')
    ok = ('__atomic_store_n(&ending_code, code' in enter
          and 'plat_terminate(__atomic_load_n(&ending_code' in enter and 'rt_ending_enter(code)' in fatal)
    print(f'static: dispatch.c rt_ending_enter ends a hung first report with that report\'s code: '
          f'{"ok" if ok else "FAIL"}', flush=True)
    bad += not ok
    # the fatal error's frame-time summary comes after RTSS's end and the rumble stop, as in the orderly end
    s, p = fatal.find('rt_rumble_stop_bounded('), fatal.find('perf_summary_flush_try(')
    ok = 0 <= s < p
    print(f'static: dispatch.c fatal_end writes the frame-time summary after RTSS\'s end and the rumble stop: '
          f'{"ok" if ok else "FAIL"}', flush=True)
    bad += not ok
    return bad


CASES = [
    # mode, exit code, lines that must be there, lines that must not
    ('fatal', 70, ['FATAL: the test\'s fatal error 1', 'frametime: at a fatal error', 'rumble: stopped',
                   'rtss: left to the next start'], ['detach:']),
    # rt_fatal on RTSS's own thread: rtss_limit_crash runs from there too, and takes no lock there
    ('fatal_rtss', 70, ['FATAL: the test\'s fatal error 3', 'rumble: stopped', 'rtss: left to the next start'],
     ['detach:']),
    # the rumble stop hangs (SDL's lock held by a thread that crashed): the end goes on after 300 ms
    ('fatal_rumble', 70, ['FATAL: the test\'s fatal error 1'], ['rumble: stopped', 'detach:']),
    # two faults at once: the first report whole, the second thread waits (dispatch.c rt_ending_enter)
    ('crash_crash', 71, ['report line 1', 'report line 2', 'report line 3'], ['detach:', 'FATAL:']),
    # a fault while rt_fatal reports: its FATAL line and notes whole, no crash report, rt_fatal's code
    ('fatal_crash', 70, ['FATAL: the test\'s fatal error 1', 'fatal note: end'], ['report line', 'detach:']),
    # rt_fatal on another thread while a crash is reported: the report whole, the crash report's code
    ('crash_fatal', 71, ['report line 1', 'report line 2', 'report line 3'], ['FATAL:', 'detach:']),
]


def main():
    bad = static_checks()
    if os.name != 'nt':
        print('fatal_exit_check: the program part runs on Windows only')
        return 1 if bad else 0
    work = ROOT / 'build' / 'test' / 'fatal_exit'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'layout.h').write_text(LAYOUT)
    exe = work / 'fatal_exit_check.exe'
    env = dict(os.environ)
    if MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    plat = [RT / 'platform' / 'win32.c', RT / 'platform' / 'common.c']
    libs = ['-lbcrypt', '-lws2_32', '-lpsapi', '-lwinmm', '-ldbghelp', '-lshell32', '-lole32', '-luser32', '-ladvapi32']
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing',
                           '-march=x86-64-v3', '-I', str(RT), '-I', str(work), str(ROOT / 'tests' / 'fatal_exit_check.c'),
                           str(RT / 'dispatch.c'), *map(str, plat), '-o', str(exe), '-lm', *libs], env=env)
    for mode, code, want, unwanted in CASES:
        t0 = time.monotonic()
        p = subprocess.run([str(exe), mode], env=env, capture_output=True, text=True, errors='replace', timeout=120)
        took = time.monotonic() - t0
        out = p.stdout + p.stderr
        missing = [w for w in want if w not in out]
        extra = [u for u in unwanted if u in out]
        fatal_lines = out.count('FATAL:')
        ok = p.returncode == code and not missing and not extra and took < 20
        print(f'{mode}: exit code {p.returncode} (want {code}) after {took:.1f} s, {fatal_lines} FATAL line(s)'
              + (f', missing {missing}' if missing else '') + (f', unexpected {extra}' if extra else '')
              + ('' if ok else ': FAIL'), flush=True)
        if not ok:
            print(out[-3000:])
        bad += not ok
    print(f'fatal_exit_check: {"FAILED" if bad else "ok"}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
