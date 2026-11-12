"""Build and run tests/warm_hint_check.c: runtime/vk_warm_hint.h, what the library warm's log adds when the driver's
cache of stage libraries is cold, by GPU vendor and by why it is cold.

Usage: warm_hint_check.py
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests' / 'diff'))
from common import MSYS_BIN, compiler  # noqa: E402


def main():
    work = ROOT / 'build' / 'test' / 'warm_hint'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('warm_hint_check.exe' if os.name == 'nt' else 'warm_hint_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    # both platforms' text is checked through lw_cold_hint's windows argument; LW_ON_WINDOWS on the one built for
    subprocess.check_call([compiler(), '-O1', '-Wall', '-Werror', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'warm_hint_check.c'), '-o', str(exe)], env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
