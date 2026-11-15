"""Build and run tests/submit_clone_check.c: runtime/nvn_submit_guard.h, a synchronous submission's deferred binds when
another guest thread may be replaying the same recording copy (nvn_api_cmd.c nvn_submit_prepare, nvn_submit_done).
Four threads submit two hand-built copies 200,000 times each, a quarter of the calls giving their copy twice, while a
"game" thread rewrites the arrays their deferred binds read: no replay sees its copy's handles change between its
resolve and its end, the in-flight counts end at 0, and the in-place, the given-again and the clone path run.
nvn_copy_again step by step: a copy given twice in one call stays in place both times, and is cloned both times when
another submission has it in flight. nvn_resolve_binds step by step: a changed array is copied in and counted,
an unchanged one is not counted, binds without src, with n 0 or over 32 stay untouched, OP_CALL is followed 16 deep and
refused at 17, and a corrupt record is reported where it is. Then the unguarded 0.10.1 shape runs through the same
harness and only prints what it saw, to show that the harness catches the race.

No GPU, no game files, any Python.

Usage: submit_clone_check.py
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():
    """$CC, else MSYS2 UCRT64's clang, else clang on PATH."""
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def main():
    work = ROOT / 'build' / 'test' / 'submit_clone'
    work.mkdir(parents=True, exist_ok=True)
    win = os.name == 'nt'
    exe = work / ('submit_clone_check.exe' if win else 'submit_clone_check')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    plat = [rt / 'platform' / ('win32.c' if win else 'linux.c'), rt / 'platform' / 'common.c']
    libs = ['-lbcrypt', '-lws2_32', '-lpsapi', '-lwinmm', '-ldbghelp', '-lshell32', '-lole32', '-luser32', '-ladvapi32'] if win \
        else ['-lpthread']
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-g', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing',
                           '-march=x86-64-v3', '-I', str(rt), str(ROOT / 'tests' / 'submit_clone_check.c'),
                           *map(str, plat), '-o', str(exe), '-lm', *libs], env=env)
    try:
        rc = subprocess.run([str(exe)], env=env, timeout=300).returncode
        if rc == 0:
            subprocess.run([str(exe), 'old'], env=env, timeout=300)
        return rc
    except subprocess.TimeoutExpired:
        print('submit_clone_check: FAIL: the check did not end within 300 s')
        return 1


if __name__ == '__main__':
    sys.exit(main())
