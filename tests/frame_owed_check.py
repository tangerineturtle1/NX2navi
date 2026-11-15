"""Build and run tests/frame_owed_check.c: runtime/vk_frame_owed.h, a frame's bookkeeping at its present in replay order
(vk_backend.c "a frame's bookkeeping in replay order", vk_sync.c submit). One presenting thread, three replaying
threads that hold the lock for their replays, a few of them for 40 ms, and a present thread that takes each frame
from the presenting thread, blits it and submits, over 20,000 frames: every frame's bookkeeping runs once and in frame
order, never during a replay, before the body of any replay that took the lock after the frame was presented and
before the present thread's blit of the frame (vk_backend.c present_frame_closed), and no present takes 20 ms, so the
presenting thread never waits for a replay. Step by step: a present with the lock free runs its own frame's, presents
past a held lock leave theirs to the next holder, which runs them in order, an older frame number runs nothing again,
and the present thread takes the lock only for a frame still owed. The exe's "old" argument leaves the present
thread's catch-up out and fails on the blits that come before their frame's bookkeeping.

No GPU, no game files, any Python.

Usage: frame_owed_check.py
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
    work = ROOT / 'build' / 'test' / 'frame_owed'
    work.mkdir(parents=True, exist_ok=True)
    win = os.name == 'nt'
    exe = work / ('frame_owed_check.exe' if win else 'frame_owed_check')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    plat = [rt / 'platform' / ('win32.c' if win else 'linux.c'), rt / 'platform' / 'common.c']
    libs = ['-lbcrypt', '-lws2_32', '-lpsapi', '-lwinmm', '-ldbghelp', '-lshell32', '-lole32', '-luser32', '-ladvapi32'] if win \
        else ['-lpthread']
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-g', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing',
                           '-march=x86-64-v3', '-I', str(rt), str(ROOT / 'tests' / 'frame_owed_check.c'),
                           *map(str, plat), '-o', str(exe), '-lm', *libs], env=env)
    try:
        return subprocess.run([str(exe)], env=env, timeout=300).returncode
    except subprocess.TimeoutExpired:
        print('frame_owed_check: FAIL: the check did not end within 300 s')
        return 1


if __name__ == '__main__':
    sys.exit(main())
