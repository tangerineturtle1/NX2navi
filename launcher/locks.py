"""Who may work in a launcher folder: one process at a time, and in that process one job at a time on the same thing.

Two launchers on one folder each ran their own build into the same game folder, wrote the same settings and unpacked
an update over each other. So:

- FileLock is a lock between processes: a file locked by the operating system (msvcrt.locking on Windows, flock
  elsewhere), which lets go of it when the process ends, however it ends. The window takes the one of its folder
  (paths.Layout.instance_lock) for as long as it is open, launcher/cli.py for a command, and every job that changes
  files while it runs (jobs.Job). A process that holds it already shares it with its own jobs. Who holds it is
  written beside it (<lock>.owner), so that the one refused can say so.
- claim() is the lock between the jobs of one process: a job names what it changes (jobs.Job.claims: the game folder
  and its build tools, the app folder), and a second job on the same thing is refused with the first one's name.

Both refuse with Busy, whose text names the holder; neither waits (FileLock for a few seconds when asked to, for a
launcher that is closing for its restart).
"""
import json
import os
import threading
import time

_guard = threading.Lock()
_files = {}    # lock file path -> [the open file, how many FileLocks of this process share it]
_claims = {}   # (folder, what is claimed) -> who claimed it


class Busy(Exception):
    """Another process or job holds what was asked for. Its text says who."""


def _key(path):
    return os.path.normcase(os.path.abspath(str(path)))


def _os_lock(f):
    """Lock the open file f for this process without waiting. OSError when another open file holds the lock."""
    if os.name == 'nt':
        import msvcrt
        f.seek(0)
        msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
    else:
        import fcntl
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)


def owner(path):
    """Who holds the lock file path, as its holder wrote it ("the launcher window (process 1234, since 14:02)"), or
    'another NX2navi' when that cannot be read. Only worth asking after the lock was refused: the note of a holder
    that has ended stays."""
    try:
        with open(str(path) + '.owner', encoding='utf-8') as f:
            o = json.load(f)
        return f'{o["what"]} (process {int(o["pid"])}, since {time.strftime("%H:%M", time.localtime(o["since"]))})'
    except (OSError, ValueError, KeyError, TypeError, OverflowError):
        return 'another NX2navi'


class FileLock:
    """The lock file path held for this process until release() or the process's end. what says who holds it, for
    the one that is refused. Raises Busy ("<the holder> is using <where>"; where is the lock's folder unless given)
    when another process still holds it after wait seconds. A lock file that cannot be made or opened (a folder
    that cannot be written) locks nothing and refuses nobody: the launcher must still start there."""

    def __init__(self, path, what, wait=0.0, where=None):
        self.path, self.key = path, _key(path)
        deadline = time.time() + wait
        while True:
            with _guard:
                if self._take(what):
                    return
            if time.time() >= deadline:
                raise Busy(f'{owner(path)} is using {where or os.path.dirname(os.path.abspath(str(path)))}')
            time.sleep(0.25)

    def _take(self, what):
        """True when this process holds the file now (or cannot lock at all), False when another process does."""
        entry = _files.get(self.key)
        if entry:
            entry[1] += 1
            return True
        try:
            f = open(self.path, 'a+')
        except OSError:
            self.key = None
            return True
        try:
            _os_lock(f)
        except OSError:
            f.close()
            return False
        _files[self.key] = [f, 1]
        try:
            with open(str(self.path) + '.owner', 'w', encoding='utf-8') as o:
                json.dump({'what': what, 'pid': os.getpid(), 'since': time.time()}, o)
        except OSError:
            pass
        return True

    def release(self):
        """Let go of the lock (the file is closed when the last FileLock of this process on it lets go). Calling it
        again does nothing."""
        with _guard:
            key, self.key = self.key, None
            entry = _files.get(key) if key else None
            if entry:
                entry[1] -= 1
                if entry[1] <= 0:
                    del _files[key]
                    entry[0].close()   # closing the file releases the operating system's lock

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.release()


def claim(folder, names, who):
    """Claim each of names in folder for who (a job's description), all or none. Raises Busy with the holder of the
    first one that is taken. Returns what release_claim() takes."""
    keys = [(_key(folder), n) for n in names]
    with _guard:
        for k in keys:
            if k in _claims:
                raise Busy(f'{_claims[k]} is still running: wait for it to end, or cancel it')
        for k in keys:
            _claims[k] = who
    return keys


def release_claim(keys):
    with _guard:
        for k in keys or ():
            _claims.pop(k, None)
