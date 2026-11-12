"""Build and run tests/mem_need_check.c: runtime/mem_need.h, whether the machine has the commit the game needs.

With the game executable's path it also runs the executable's --probe-memneed (runtime/memory.c mem_start_note: it
reads this machine's commit, prints the answer and starts nothing) three times: with mem_need_mb = 0 (not checked),
with 1 (any machine has it) and with a value no machine has, where the log's warning line must appear.

Usage: mem_need_check.py [PATH_TO_GAME_EXE]
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
    work = ROOT / 'build' / 'test' / 'mem_need'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('mem_need_check.exe' if os.name == 'nt' else 'mem_need_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-Wall', '-Werror', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'mem_need_check.c'), '-o', str(exe)], env=env)
    rc = subprocess.run([str(exe)], env=env).returncode
    if len(sys.argv) > 1:
        rc |= probe(sys.argv[1])
    return rc


def probe(game):
    """The executable's --probe-memneed under three settings; 0 when each answers as it must."""
    fails = 0
    for need, answer, warns in (('0', 'not checked', False), ('1', 'enough', False), ('1048576', 'short', True)):
        env = dict(os.environ, NX2NAVI_MEM_NEED_MB=need)
        r = subprocess.run([game, '--probe-memneed'], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, errors='replace')
        lines = r.stdout.splitlines()
        said = [ln for ln in lines if ln.startswith('probe-memneed:')]
        warned = [ln for ln in lines if 'mem: WARNING: this machine has' in ln]
        print('\n'.join(said + [ln[:160] + ' ...' for ln in warned]))
        # without a commit limit (a Linux machine with none) nothing is short: the forced case cannot be shown there
        if said and said[0].endswith('no limit known'):
            continue
        if r.returncode or len(said) != 1 or not said[0].endswith(': ' + answer) or bool(warned) != warns:
            fails += 1
            print('mem_need_check: FAIL: --probe-memneed with mem_need_mb = %s (exit code %d)' % (need, r.returncode))
    print('mem_need_check: the executable\'s answer %s' % ('FAILED' if fails else 'passed'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
