"""What NX2navi.exe runs: python\\pythonw.exe app\\<version>\\launcher\\start.pyw [arguments].

The embeddable Python puts only its ._pth entries on sys.path, so the release folder (the parent of launcher/) goes
first here. A failure before the window is up is written to data/logs/crash.log and ends the process with exit code
86, which tells the stub to go back to the previous version (launcher/native/nx2navi.c wWinMain). The stub waits
six seconds for that code (QUICK_MS), so the failure is shown in a message box only when the stub has no version to
go back to or no longer waits (quiet_exit): a box holds the exit code back until the player closes it, and a new
version that could not start then stayed the current one.
"""
import os
import sys
import traceback

APP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STARTUP_FAILED = 86
# The stub's wait for this process's exit code is QUICK_MS, 6 s (launcher/native/nx2navi.c start_release). An exit
# this long after the process was made still reaches it, with a second to spare for the exit itself.
STUB_WAITS = 5.0


def launcher_root():
    return os.environ.get('NX2NAVI_LAUNCHER_ROOT') or os.path.dirname(os.path.dirname(APP))


def rollback_target(root):
    """The version the stub goes back to when this one ends with STARTUP_FAILED (launcher/native/nx2navi.c
    wWinMain): the one app/previous.txt names, when it is another than this one and its folder holds a launcher.
    '' when there is none."""
    try:
        with open(os.path.join(root, 'app', 'previous.txt'), encoding='utf-8-sig') as f:
            prev = (f.read().splitlines() or [''])[0].strip()
    except (OSError, UnicodeDecodeError):
        return ''
    if not prev or prev == os.path.basename(APP):
        return ''
    return prev if os.path.isfile(os.path.join(root, 'app', prev, 'launcher', 'start.pyw')) else ''


def process_age():
    """Seconds since Windows made this process, which is when the stub began to wait for it. Infinite where that
    cannot be read, so that the failure is then shown rather than left to a stub that may have stopped waiting."""
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
        ticks = [t.dwHighDateTime << 32 | t.dwLowDateTime for t in (made, now)]   # 100 ns each
        return max(0.0, (ticks[1] - ticks[0]) / 1e7)
    except Exception:
        return float('inf')


def quiet_exit(root, argv, age):
    """True when a failed start ends without a message box: the stub goes back to the previous version on the
    exit code (rollback_target), and that version's home screen says which version did not start. Not when Steam
    started this version without the stub (--play, launcher/steam.py PLAY_TEXT), and not once the stub has stopped
    waiting (age, process_age): nobody acts on the exit code then, and without the box nothing would be seen."""
    return bool(rollback_target(root)) and '--play' not in argv and age < STUB_WAITS


def crash(text, box=True):
    """The failure into data/logs/crash.log and, with box, into a message box that waits for the player."""
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
        ctypes.windll.user32.MessageBoxW(None, 'NX2navi could not start.\n\n' + text[-1500:], 'NX2navi', 0x10)
    except Exception:
        print(text, file=sys.stderr)


def run():
    sys.path.insert(0, APP)
    try:
        from launcher.ui.app import main
    except Exception:
        crash(traceback.format_exc(), box=not quiet_exit(launcher_root(), sys.argv[1:], process_age()))
        sys.exit(STARTUP_FAILED)
    try:
        code = main()
    except SystemExit:
        raise
    except Exception:
        crash(traceback.format_exc())
        sys.exit(1)
    sys.exit(code or 0)


if __name__ == '__main__':
    run()
