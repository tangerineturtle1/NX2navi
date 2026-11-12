"""Build and run tests/trophies_check.c: runtime/trophies.c against a made-up game — a fake guest arena holding the
game-data manager's flag table and the actor manager's player the way the 1.1.1 build lays them out. The check drives
no save, a load's silent grants, a live unlock's toast queue, the flag table growing mid-play and the trophies.dat
round trip.

Usage: trophies_check.py
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests' / 'diff'))
from common import MSYS_BIN, compiler  # noqa: E402


def main():
    work = ROOT / 'build' / 'test' / 'trophies'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('trophies_check.exe' if os.name == 'nt' else 'trophies_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-Wall', '-Werror', '-I', str(ROOT / 'runtime'), '-I', str(ROOT),
                           str(ROOT / 'tests' / 'trophies_check.c'), '-o', str(exe)], env=env)
    return subprocess.run([str(exe), str(work)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
