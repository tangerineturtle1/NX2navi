"""Running tools/make_game.py for the launcher: the command line, its "@@" progress lines (make_game.py --events)
and cancelling it with every compiler it started."""
import json
import os
import signal
import stat
import subprocess
import threading

# Runs make_game.py with pydeps/ (zbic) and the script's folder first on sys.path; the embeddable Python's ._pth
# file leaves both off.
RUN = ('import os, runpy, sys; p = sys.argv[1]; s = sys.argv[2]; sys.argv = sys.argv[2:]; '
       'sys.path[:0] = [x for x in p.split(os.pathsep) if x] + [os.path.dirname(os.path.abspath(s))]; '
       'runpy.run_path(s, run_name="__main__")')
CREATE_NO_WINDOW = 0x08000000
CREATE_NEW_PROCESS_GROUP = 0x00000200
# The build runs below normal priority: a compile on every processor for ten minutes and more otherwise takes the
# PC from whatever the player does meanwhile, which the job screen invites ("Keep using your PC if you like"). On an
# idle PC it costs nothing. Windows hands a below-normal process's class on to the processes it starts, so the
# compilers, the lifter's workers and the shader preparation that make_game.py starts run that way too.
BELOW_NORMAL_PRIORITY_CLASS = 0x00004000


def command(layout, args):
    return [layout.python_for_builds(), '-I', '-c', RUN, str(layout.pydeps), str(layout.make_game), *map(str, args),
            '--events']


def has_prompt_art(layout):
    """True when this version holds button art (app/<version>/prompt-art, tools/prompt_art.py FOLDER): make_game.py
    then draws the button prompts in a step of its own, and the jobs show a row for it."""
    return (layout.app / 'prompt-art').is_dir()


def install_args(layout, dump, game_dir):
    dump = str(dump)
    if os.path.isdir(dump):
        src = ['--exefs', os.path.join(dump, 'exefs'), '--romfs', os.path.join(dump, 'romfs')]
    else:
        src = ['--nup', dump]
    return [*src, '--out', str(game_dir), '--msys', str(layout.msys)]


def rebuild_args(layout, game_dir, retune=False):
    """retune: Rebuild the Game, which compiles the game's code for this processor's level; an update keeps the level
    its objects were compiled for, so that it compiles only what changed (tools/make_game.py lifted_level)."""
    return ['--rebuild', *(['--retune'] if retune else []), '--out', str(game_dir), '--msys', str(layout.msys)]


def check_args(dump):
    dump = str(dump)
    if os.path.isdir(dump):
        return ['--exefs', os.path.join(dump, 'exefs'), '--romfs', os.path.join(dump, 'romfs'), '--check']
    return ['--nup', dump, '--check']


class BuildError(Exception):
    pass


class Run:
    """One make_game.py process. on_event(dict) for each "@@" line, on_line(str) for every other line; wait()
    returns the exit code. cancel() ends the process and its children."""

    def __init__(self, cmd, on_event, on_line, cwd=None):
        self.error = None
        kw = {}
        if os.name == 'nt':
            kw['creationflags'] = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | BELOW_NORMAL_PRIORITY_CLASS
        else:
            kw['start_new_session'] = True
        env = dict(os.environ, PYTHONUNBUFFERED='1', PYTHONIOENCODING='utf-8')
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                     cwd=cwd, env=env, text=True, encoding='utf-8', errors='replace', bufsize=1, **kw)
        self.cancelled = False
        self.dropped = 0   # lines and events whose handler raised (_read)
        self._reader = threading.Thread(target=self._read, args=(on_event, on_line), daemon=True)
        self._reader.start()

    def _read(self, on_event, on_line):
        """Reads the build's output to its end, whatever the handlers do. A handler that raises (the launcher's log
        on a full disk, a drawing error) loses its line, counted in dropped, and the reading goes on: when this
        thread ended on the exception nobody read the pipe any more, the build stopped at its next line once the
        pipe was full, and the job waited for it for good."""
        def call(handler, arg):
            try:
                handler(arg)
            except Exception:   # noqa: BLE001 - any failure of a handler must leave the pipe read
                self.dropped += 1

        for line in self.proc.stdout:
            line = line.rstrip('\r\n')
            if line.startswith('@@'):
                try:
                    ev = json.loads(line[2:])
                except ValueError:
                    call(on_line, line)
                    continue
                if isinstance(ev, dict):
                    if ev.get('ev') == 'error':
                        self.error = ev.get('message')
                    call(on_event, ev)
                else:   # "@@" before something that is no event
                    call(on_line, line)
            elif line.startswith('error: ') and not self.error:
                self.error = line[7:]
                call(on_line, line)
            else:
                call(on_line, line)

    def wait(self):
        code = self.proc.wait()
        self._reader.join(5)
        return code

    def cancel(self):
        self.cancelled = True
        if self.proc.poll() is not None:
            return
        if os.name == 'nt':
            subprocess.run(['taskkill', '/T', '/F', '/PID', str(self.proc.pid)], capture_output=True,
                           creationflags=CREATE_NO_WINDOW)
        else:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except (ProcessLookupError, PermissionError):
                pass


ERROR_ACCESS_DENIED, ERROR_SHARING_VIOLATION = 5, 32


def _write_open_error(path):
    """0 when path opens for writing (it is closed again at once), else Windows' error code for the refusal. Every
    share mode is granted, so a sharing violation comes only from a running image."""
    import ctypes
    from ctypes import wintypes
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.CreateFileW.restype = wintypes.HANDLE
    k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                              wintypes.DWORD, wintypes.HANDLE]
    k.CloseHandle.argtypes = [wintypes.HANDLE]
    # GENERIC_WRITE; FILE_SHARE_READ, FILE_SHARE_WRITE and FILE_SHARE_DELETE; OPEN_EXISTING; FILE_ATTRIBUTE_NORMAL
    h = k.CreateFileW(str(path), 0x40000000, 7, None, 3, 0x80, None)
    if h in (None, wintypes.HANDLE(-1).value):
        return ctypes.get_last_error() or -1
    k.CloseHandle(h)
    return 0


def game_running(game_dir, exe='ocarina.exe', log=None):
    """True when the built game runs from game_dir: Windows refuses to open a running exe for writing, with a sharing
    violation. A read-only exe (a copy restored from a backup or read-only media) is refused too, with access denied,
    though nothing runs it: its read-only attribute is cleared and the open tried again, as the next build replaces
    the file anyway. Access denied for another reason (an access list that denies the player writes, Controlled
    Folder Access) is no running game either: taken for one, it kept Play saying "already running" and a rebuild
    failing with "the game is running" for an exe that nothing ran. log, when given, gets a line for that case, which
    a build's failure to replace the exe (tools/make_game.py) then has beside it."""
    path = os.path.join(str(game_dir), exe)
    if os.name != 'nt' or not os.path.isfile(path):
        return False
    for attempt in range(2):
        err = _write_open_error(path)
        if err == ERROR_SHARING_VIOLATION:
            return True
        if err != ERROR_ACCESS_DENIED:   # opened, or gone meanwhile
            return False
        if attempt or not os.stat(path).st_file_attributes & stat.FILE_ATTRIBUTE_READONLY:
            break
        try:
            os.chmod(path, stat.S_IREAD | stat.S_IWRITE)
        except OSError:
            break
    if log:
        log(f'{path} cannot be opened for writing (access denied), though the game does not run it: a build cannot '
            'replace it until it can')
    return False


def build_state(game_dir):
    """The game folder's build-cache/state.json (make_game.py), or {}."""
    try:
        with open(os.path.join(str(game_dir), 'build-cache', 'state.json'), encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def built_version(game_dir):
    """The kit version that last built the game folder ('' when none finished)."""
    st = build_state(game_dir)
    if 'compile' in st.get('steps', {}) and os.path.isfile(os.path.join(str(game_dir), 'ocarina.exe')):
        return st.get('kit', '')
    return ''


def scan_report(game_dir):
    """What the check of the game files found (tools/romfs_check.py writes <game>/romfs-check.json): (the paths of
    the damaged files, how many of them got a stand-in), or None when the folder has no readable report."""
    try:
        with open(os.path.join(str(game_dir), 'romfs-check.json'), encoding='utf-8') as f:
            report = json.load(f)
        return sorted(report['damaged']), len(report.get('repaired') or ())
    except (OSError, ValueError, KeyError, TypeError):
        return None


def can_rebuild(game_dir):
    st = build_state(game_dir)
    return st.get('version') == 2 and all(s in st.get('steps', {}) for s in ('exefs', 'romfs'))


def unpacked(game_dir):
    """True when the game files a rebuild reads are in the game folder (make_game.py BuiltSource names the same
    ones): the unpacked ExeFS, the RomFS image and its index."""
    gd = str(game_dir)
    return all(os.path.isfile(os.path.join(gd, *parts)) for parts in
               (('exefs', 'main'), ('romfs', 'romfs.bin'), ('romfs', 'index.json')))


def installed(game_dir):
    """True when the game folder holds a game the player can start: a finished build, or the exe of the last
    finished build beside a build that stopped later. make_game.py drops a step's record when a step before it
    runs again, so an update that lifted again and then stopped in the compile (an error, Cancel, a power cut)
    leaves state.json without `compile` and built_version says '': the old exe is still there and playable, and
    a rebuild continues from the unpacked game files without the dump (can_rebuild)."""
    if not game_dir:
        return False
    if built_version(game_dir):
        return True
    return os.path.isfile(os.path.join(str(game_dir), 'ocarina.exe')) and can_rebuild(game_dir)
