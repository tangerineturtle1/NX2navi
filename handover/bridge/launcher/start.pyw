"""What NX2navi.exe runs: python\\pythonw.exe app\\<version>\\launcher\\start.pyw [arguments].

From this version on that is the handover to Triforce (handover.py beside this file), not NX2navi's launcher.

The contract with the stub (NX2navi's launcher/native/nx2navi.c) is kept as the launcher kept it: a failure before
the window is up is written to data/logs/crash.log and ends the process with exit code 86 within the six seconds the
stub waits, so the stub goes back to the version before (app/previous.txt), which is still on the PC. Only when there
is no version to go back to, or the stub no longer waits, the failure is shown in a message box instead, with the
address Triforce is at.
"""
import os
import sys
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
STARTUP_FAILED = 86
STUB_WAITS = 5.0   # the stub waits 6 s for the exit code (QUICK_MS); a second is left for the exit itself


def launcher_root():
    return os.environ.get('NX2NAVI_LAUNCHER_ROOT') or os.path.dirname(os.path.dirname(APP))


def rollback_target(root):
    """The version the stub goes back to on exit code 86: the one app/previous.txt names, when it is another than
    this one and holds a launcher. '' when there is none."""
    try:
        with open(os.path.join(root, 'app', 'previous.txt'), encoding='utf-8-sig') as f:
            prev = (f.read().splitlines() or [''])[0].strip()
    except (OSError, UnicodeDecodeError):
        return ''
    if not prev or prev == os.path.basename(APP):
        return ''
    return prev if os.path.isfile(os.path.join(root, 'app', prev, 'launcher', 'start.pyw')) else ''


def process_age():
    """Seconds since Windows made this process, which is when the stub began to wait. Infinite where that cannot be
    read."""
    try:
        import ctypes
        from ctypes import wintypes
        k = ctypes.windll.kernel32
        k.GetCurrentProcess.restype = wintypes.HANDLE
        k.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
        made, ended, kernel, user, now = (wintypes.FILETIME() for _ in range(5))
        if not k.GetProcessTimes(k.GetCurrentProcess(), *(ctypes.byref(t) for t in (made, ended, kernel, user))):
            return float('inf')
        k.GetSystemTimeAsFileTime(ctypes.byref(now))
        ticks = [t.dwHighDateTime << 32 | t.dwLowDateTime for t in (made, now)]
        return max(0.0, (ticks[1] - ticks[0]) / 1e7)
    except Exception:
        return float('inf')


def crash(text, box):
    try:
        logs = os.path.join(launcher_root(), 'data', 'logs')
        os.makedirs(logs, exist_ok=True)
        with open(os.path.join(logs, 'crash.log'), 'a', encoding='utf-8') as f:
            f.write(text + '\n')
    except OSError:
        pass
    if not box:
        return
    try:
        import ctypes
        ctypes.windll.user32.MessageBoxW(
            None, 'NX2navi is now Triforce, but the window that takes you there could not open.\n\n'
                  'Get Triforce from https://triforceproject.com\n\n' + text[-900:], 'NX2navi', 0x30)
    except Exception:
        print(text, file=sys.stderr)


def run():
    sys.path.insert(0, HERE)   # the embeddable Python puts only its ._pth entries on sys.path
    argv = sys.argv[1:]
    try:
        import handover
        code = handover.main(argv)
    except SystemExit:
        raise
    except Exception:
        root = launcher_root()
        quiet = bool(rollback_target(root)) and '--play' not in argv and process_age() < STUB_WAITS
        crash(traceback.format_exc(), box=not quiet)
        sys.exit(STARTUP_FAILED if quiet else 0)
    sys.exit(code or 0)


if __name__ == '__main__':
    run()
