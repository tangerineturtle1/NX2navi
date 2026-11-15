"""Build and run tests/exit_tls_check.c: an ExitProcess or exit() that none of the game's own ends asked for leaves the
log queue's lines and one note in the log file, naming the calls that led to it by module and offset
(runtime/platform/win32.c exit_note_tls); TerminateProcess, plat_exit and a return from main after plat_exit_orderly
leave none. Windows only; no game files.

Usage: exit_tls_check.py
"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')

LAYOUT = '''// A stand-in for the generated layout.h (tests/exit_tls_check.py).
#pragma once
#define MOD_MAIN_BASE 0x7000000000ull
#define MOD_MAIN_SIZE 0x4ea0000ull
'''
NOTE = 'exit: the process is ending through ExitProcess'

CASES = [   # mode, the queued line in the file, a note in the file
    ('exitprocess', True, True),
    ('exit', True, True),
    ('terminate', None, False),   # the queued line may or may not have been written: the kill came while it gathered
    ('platexit', True, False),
    ('return', None, False),
]


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def main():
    if os.name != 'nt':
        print('exit_tls_check: Windows only')
        return 0
    work = ROOT / 'build' / 'test' / 'exit_tls'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'layout.h').write_text(LAYOUT)
    exe = work / 'exit_tls_check.exe'
    env = dict(os.environ)
    if MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    plat = [rt / 'platform' / 'win32.c', rt / 'platform' / 'common.c', rt / 'logq.c']
    libs = ['-lbcrypt', '-lws2_32', '-lpsapi', '-lwinmm', '-ldbghelp', '-lshell32', '-lole32', '-luser32', '-ladvapi32']
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing',
                           '-march=x86-64-v3', '-I', str(rt), '-I', str(work), str(ROOT / 'tests' / 'exit_tls_check.c'),
                           *map(str, plat), '-o', str(exe), '-lm', *libs], env=env)
    bad = 0
    for mode, queued, note in CASES:
        log = work / f'{mode}.log'
        log.unlink(missing_ok=True)
        rc = subprocess.run([str(exe), mode, str(log)], env=env, timeout=60).returncode
        text = log.read_text(errors='replace') if log.exists() else ''
        has_q, has_note = 'queued line' in text, NOTE in text
        named = bool(re.search(r'exit_tls_check\.exe\+0x[0-9a-f]+', text))
        ok = rc == 0 and (queued is None or has_q == queued) and has_note == note and (not note or named)
        print(f'{mode}: exit code {rc}, the queued line {"in" if has_q else "not in"} the file, '
              f'{"a note" + (" naming this program" if named else " NAMING NOTHING") if has_note else "no note"}'
              f'{"" if ok else ": FAIL"}', flush=True)
        if not ok:
            print(text[-2000:])
        bad += not ok
    print(f'exit_tls_check: {"FAILED" if bad else "ok"}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
