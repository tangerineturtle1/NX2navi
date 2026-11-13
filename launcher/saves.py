"""Importing a save into the built game, and the saves folder of the game folder.

The game keeps its save in <game>/profile/save/<account>/ (runtime/fs_host.c storage_init). The folder is named after
the local account in <game>/profile/account.uid: 16 bytes, two little-endian 64-bit halves, the low one first; the
name is the high half then the low half, 16 hex digits each (runtime/ov_system.c load_uid). A game that never ran has
no account yet; the import makes one as the game would. Saves are committed through <game>/profile/save-tx/, where
a finished commit (ready-<n>) is newer than save/<account>: an import clears that folder, so the game loads the
imported save.

<game>/saves is the player's side. Import Saves takes a save put there first (a shared save's zip, its files, or the
folder that holds them); otherwise the player picks a zip or a folder. Before the save is replaced it is copied to
saves/backup <time>, and a save taken from saves/ moves to saves/imported <time>, so nothing is lost and nothing is
imported twice.

A shared save is one zip (export_save, import_zip; section "sharing a save" below): a player attaches it to a bug
report, and whoever fixes it imports it to reach the same place in the game. The game's settings menu exports and
imports the same zip; its import waits in <game>/profile/save-import for the game's next start.

A save folder can come from a Switch save backup (the save's files), an emulator (nested folders named by user and
title ids, gone down while each holds only one folder), another NX2navi game folder or its profile folder (the
account folder inside).

A whole save is account.sav, option.sav and the folders slot_00 to slot_05, each with progress.sav and caption.sav. A
folder with progress.sav at its top is one slot: it replaces that slot (the slot named by its folder, else slot_00) and
the rest of the current save stays, as the game reads progress.sav only inside a slot folder.
"""
import contextlib
import json
import os
import re
import shutil
import stat
import tempfile
import time
import zipfile
import zlib
from pathlib import Path


def _rmtree(path):
    """Delete a folder, read-only files in it too (shutil.rmtree stops at them on Windows); what still fails stays."""
    def writable(func, p, _exc):
        try:
            os.chmod(p, stat.S_IREAD | stat.S_IWRITE)
            func(p)
        except OSError:
            pass
    if Path(path).exists():
        shutil.rmtree(path, onexc=writable)

README = '''NX2navi saves folder

To import a save, put it here and select Saves, then Import a Save, in the NX2navi launcher:
- a save someone shared: the NX2navi-save-....zip that NX2navi exported
- the files of a Switch save backup, or the folder that holds them
- one save slot (a folder such as slot_00): only that slot is replaced
- an emulator's save folder for the game
- the profile folder (or the whole game folder) of another NX2navi install

You can also select Import a Save and choose the zip or the folder instead.

Before a save is imported, the game's current save is copied here into a folder named "backup" and the date. An
imported save moves into a folder named "imported" and the date. The game itself keeps its save in profile\\save.
'''
HEX32 = re.compile(r'^[0-9a-fA-F]{32}$')
SLOT = re.compile(r'^slot_\d\d$')
SLOT_FILES = ('progress.sav', 'caption.sav')
MAX_FILES, MAX_BYTES = 4000, 1 << 30   # a save is a few files of a few MB; more is a folder picked by mistake
OURS = re.compile(r'^(backup|imported) \d{4}-\d\d-\d\d')   # what this module writes into saves/


class SaveError(Exception):
    pass


def saves_dir(game):
    return Path(game) / 'saves'


def ensure_saves_dir(game):
    """<game>/saves with its README.txt."""
    d = saves_dir(game)
    d.mkdir(parents=True, exist_ok=True)
    readme = d / 'README.txt'
    try:
        current = readme.read_text(encoding='utf-8')
    except (OSError, ValueError):
        current = None
    if current != README:   # missing, or an older launcher's
        readme.write_text(README, encoding='utf-8', newline='\r\n')
    return d


def account_dir(game):
    """The game's save folder for its local account, making the account as the game does when it has none."""
    prof = Path(game) / 'profile'
    f = prof / 'account.uid'
    data = f.read_bytes() if f.is_file() else b''
    if not data:
        data = bytes(16)
        while data == bytes(16):
            data = os.urandom(16)
        prof.mkdir(parents=True, exist_ok=True)
        tmp = f.with_name('account.uid.tmp')
        tmp.write_bytes(data)
        os.replace(tmp, f)
    if len(data) != 16 or data == bytes(16):
        raise SaveError(f'{f} is not an account file of the game')
    lo, hi = int.from_bytes(data[:8], 'little'), int.from_bytes(data[8:], 'little')
    return prof / 'save' / f'{hi:016x}{lo:016x}'


def _fresh(folder, name):
    """folder/name, or folder/"name (2)" and so on when that is taken (two imports in one second)."""
    p, n = folder / name, 2
    while p.exists():
        p, n = folder / f'{name} ({n})', n + 1
    return p


def _entries(folder, skip=()):
    return [e for e in Path(folder).iterdir() if e.name not in skip and not e.name.startswith('.')]


def find_save(folder):
    """(source folder, top-level names to leave out) of the save in folder, or (None, ()) when it holds none."""
    p = Path(folder)
    if not p.is_dir():
        return None, ()
    for base in (p / 'profile' / 'save', p / 'save'):   # another game folder, or its profile folder
        if base.is_dir():
            ready = _ready(base.parent / 'save-tx')   # a finished commit there is newer than its account folder
            if ready is not None and _has_file(ready):
                return ready, ()
            accounts = [d for d in base.iterdir() if d.is_dir() and HEX32.match(d.name)]
            if accounts:
                return max(accounts, key=lambda d: d.stat().st_mtime), ()
    for _ in range(8):   # nested id folders: go down while the folder holds a single folder and no file
        entries = _entries(p)
        dirs = [e for e in entries if e.is_dir()]
        if len(entries) != 1 or len(dirs) != 1:
            break
        p = dirs[0]
    # a backup this module wrote keeps the replaced save's journal beside it, which is not part of the save
    return (p, ('save-tx',) if (p / 'save-tx').is_dir() else ()) if _has_file(p) else (None, ())


def _one_slot(src):
    """The slot folder the files of src belong in when src is a single slot, None when it is a whole save."""
    p = Path(src)
    if not (p / 'progress.sav').is_file() or any(e.is_dir() and SLOT.match(e.name) for e in p.iterdir()):
        return None
    return p.name if SLOT.match(p.name) else 'slot_00'


def waiting(game):
    """(source, names to leave out) of a save put into <game>/saves, or (None, ()). A zip there (export_save's) comes
    first, the newest one; its source is the zip file."""
    d = saves_dir(game)
    if not d.is_dir():
        return None, ()
    skip = {'README.txt'} | {e.name for e in d.iterdir() if e.is_dir() and OURS.match(e.name)}
    entries = _entries(d, skip)
    if not entries:
        return None, ()
    zips = [e for e in entries if e.is_file() and e.suffix.lower() == '.zip']
    if zips:
        return max(zips, key=lambda e: e.stat().st_mtime), ()
    if len(entries) == 1 and entries[0].is_dir():
        return find_save(entries[0])
    return (d, tuple(sorted(skip))) if any(e.is_file() or _has_file(e) for e in entries) else (None, ())


def _has_file(folder):
    return any(files for _, _, files in os.walk(folder))


def _files(src, skip):
    """Relative paths of the files to import; SaveError when there are too many or too large to be a save."""
    out, size = [], 0
    for root, dirs, files in os.walk(src):
        if Path(root) == Path(src):
            dirs[:] = [d for d in dirs if d not in skip and not d.startswith('.')]
            files = [f for f in files if f not in skip and not f.startswith('.')]
        for f in files:
            p = Path(root) / f
            out.append(p.relative_to(src))
            size += p.stat().st_size
            if len(out) > MAX_FILES or size > MAX_BYTES:
                raise SaveError(f'{src} holds more than a save ({len(out)} files, {size / 1e6:.0f} MB so far); '
                                'choose the folder with the save\'s files')
    if not out:
        raise SaveError(f'{src} holds no files')
    return out


def _ready(tx):
    """The newest finished commit (ready-<n>) in a save-tx folder, or None."""
    ready = []
    if Path(tx).is_dir():
        for d in Path(tx).iterdir():
            m = re.fullmatch(r'ready-(\d+)', d.name)
            if m and d.is_dir():
                ready.append((int(m.group(1)), d))
    return max(ready)[1] if ready else None


def _newest(game, account):
    """The folder the game would load now, in the order of runtime/fs_host.c save_recover: a finished commit still
    waiting in save-tx, else the account folder, else an interrupted commit's new save, else the save it replaced."""
    tx = Path(game) / 'profile' / 'save-tx'
    for d in (_ready(tx), account, tx / 'new', tx / 'old'):
        if d is not None and d.is_dir():
            return d
    return None


def game_running(game):
    """Windows: ocarina.exe cannot be opened for writing while it runs (launcher/build.py game_running)."""
    from .build import game_running as running
    return running(game)


RUNNING = 'the game is running: quit it first, it keeps its save open while it runs'


@contextlib.contextmanager
def profile_lock(game):
    """The game's own lock on its profile, held while the save is replaced. The game takes
    <game>/profile/instance.lock when it starts and keeps it until it ends (runtime/fs_host.c fs_host_lock_profile):
    on Windows by opening the file without sharing, elsewhere with flock. Taken here the same way, it says for
    certain whether the game runs (SaveError when it does), and a game started while it is held ends at once with
    its "another copy is running" message instead of mounting a save that is half replaced. game_running() alone,
    asked before the player picked a file, left the whole time of that dialog and of the copy for the game to
    start in. A lock file that cannot be opened for another reason locks nothing: the import goes on as before."""
    prof = Path(game) / 'profile'
    path = prof / 'instance.lock'
    try:
        prof.mkdir(parents=True, exist_ok=True)
    except OSError:
        pass
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes
        k = ctypes.WinDLL('kernel32', use_last_error=True)
        k.CreateFileW.restype = wintypes.HANDLE
        k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                  wintypes.DWORD, wintypes.HANDLE]
        k.CloseHandle.argtypes = [wintypes.HANDLE]
        # read and write access, no sharing, made when missing: platform/win32.c plat_lock_file's own call
        h = k.CreateFileW(str(path), 0xC0000000, 0, None, 4, 0x80, None)
        if h in (None, wintypes.HANDLE(-1).value):
            h = None
            if ctypes.get_last_error() == 32:   # ERROR_SHARING_VIOLATION: the game has it open
                raise SaveError(RUNNING)
        try:
            yield
        finally:
            if h is not None:
                k.CloseHandle(h)
        return
    f = None
    try:
        f = open(path, 'a+b')
    except OSError:
        pass
    try:
        if f is not None:
            import fcntl
            try:
                fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)   # platform/linux.c plat_lock_file's own call
            except BlockingIOError:
                raise SaveError(RUNNING) from None
            except OSError:   # a file system without locks
                pass
        yield
    finally:
        if f is not None:
            f.close()


def pending_import(game):
    """The save the game's menu took from a shared zip and left in <game>/profile/save-import for its next start
    (runtime/fs_host.c import_pending), or None."""
    p = Path(game) / 'profile' / PENDING
    return p if p.is_dir() else None


def import_save(game, folder=None, log=print):
    """Make the save in folder (None: the one waiting in <game>/saves) the game's save. folder may also be a zip of
    export_save (import_zip). Returns {'files', 'source', 'backup', 'slot'}; backup is the folder the replaced save
    went to, or None when there was none."""
    game = Path(game)
    if folder is not None and Path(folder).resolve() == saves_dir(game).resolve():
        folder = None   # the saves folder itself, picked in the dialog: what waits in it, as the button takes it
    if folder is not None and Path(folder).is_file():
        return import_zip(game, folder, log=log)
    from_saves = folder is None
    src, skip = waiting(game) if from_saves else find_save(folder)
    if src is None:
        raise SaveError('the saves folder holds no save' if from_saves else f'{folder} holds no save files')
    stamp = time.strftime('%Y-%m-%d %H.%M.%S')
    if Path(src).is_file():   # a shared save's zip put into saves/: it moves out once imported, as a folder does
        r = import_zip(game, src, log=log, stamp=stamp)
        _put_away(game, [Path(src).name], stamp)
        return r
    prof = (game / 'profile').resolve()
    if Path(src).resolve() == prof or prof in Path(src).resolve().parents:
        raise SaveError('that is the game\'s own save')
    files = _files(src, set(skip))
    slot = _one_slot(src)
    account, backup = _replace(game, src, files, slot, stamp, log)
    if from_saves:   # what was put into saves/ moves out whole, so the next import does not take any of it again
        saves = saves_dir(game)
        if Path(src) == saves:
            top = sorted({rel.parts[0] for rel in files})
        else:   # the save sits in a folder that was put there (a game folder, an emulator's id folders)
            top = [Path(src).relative_to(saves).parts[0]]
        _put_away(game, top, stamp)
    log(f'saves: imported {len(files)} files from {src} into {account / slot if slot else account}' +
        (f'; the old save is in {backup}' if backup else ''))
    return {'files': len(files), 'source': Path(src), 'backup': backup, 'slot': slot}


def _put_away(game, names, stamp):
    """Move names (in <game>/saves) into saves/imported <stamp>."""
    saves = saves_dir(game)
    done = _fresh(saves, f'imported {stamp}')
    done.mkdir()
    for name in names:
        shutil.move(str(saves / name), str(done / name))


def _replace(game, src, files, slot, stamp, log):
    """Copy the files of src (relative paths) into the game's save: the whole save, or with slot only that slot folder
    and the rest of the current save kept. The save the game would load now goes to saves/backup <stamp> first.
    Returns (account folder, backup folder or None). The game's profile lock is held meanwhile (profile_lock):
    SaveError when the game runs."""
    with profile_lock(game):
        return _replace_held(Path(game), src, files, slot, stamp, log)


def _replace_held(game, src, files, slot, stamp, log):
    account = account_dir(game)
    saves = ensure_saves_dir(game)
    backup = None
    current = _newest(game, account)
    if current is not None and _has_file(current):
        backup = _fresh(saves, f'backup {stamp}')
        shutil.copytree(current, backup)
    tx = game / 'profile' / 'save-tx'
    tx.mkdir(parents=True, exist_ok=True)
    stage = tx / 'import'
    shutil.rmtree(stage, ignore_errors=True)
    if slot and current is not None:   # the rest of the save stays; a slot's files left at the top by mistake do not
        shutil.copytree(current, stage, ignore=lambda d, _names: SLOT_FILES if Path(d) == Path(current) else ())
        _rmtree(stage / slot)
    target = stage / slot if slot else stage
    for rel in files:
        (target / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(Path(src) / rel, target / rel)
        # a save copied from read-only media keeps the attribute through copy2; the game rewrites its files
        os.chmod(target / rel, stat.S_IREAD | stat.S_IWRITE)
    # The journal of the replaced save goes out of save-tx (the game would load a finished commit first, or complete an
    # interrupted one): what holds files moves into the backup, beside the save it was copied from, so a copy the
    # backup's own choice missed (an interrupted commit's other half) is kept too.
    moved = []
    for d in list(tx.iterdir()):
        if d == stage:
            continue
        if _has_file(d) if d.is_dir() else d.stat().st_size:
            if backup is None:
                backup = _fresh(saves, f'backup {stamp}')
                backup.mkdir()
            (backup / 'save-tx').mkdir(exist_ok=True)
            shutil.move(str(d), str(backup / 'save-tx' / d.name))
            moved.append(d)
        else:
            _rmtree(d) if d.is_dir() else d.unlink(missing_ok=True)
    account.parent.mkdir(parents=True, exist_ok=True)
    # The imported save goes into place before the old one is deleted; a failed rename puts the old one back, and the
    # journal too: a finished commit waiting in save-tx is newer than the account folder, and without it the game
    # would load the older save although nothing was imported.
    gone = None
    try:
        if account.exists():
            gone = _fresh(tx, 'replaced')
            os.replace(account, gone)
        try:
            os.replace(stage, account)
        except OSError:
            if gone is not None:
                os.replace(gone, account)
            raise
    except OSError:
        for d in moved:
            try:
                shutil.move(str(backup / 'save-tx' / d.name), str(d))
            except OSError as e:
                log(f'saves: could not put {d.name} back into {tx}: {e}; it is in {backup / "save-tx"}')
        _rmtree(stage)
        raise
    if gone is not None:
        _rmtree(gone)
    # A save the game's menu took for its next start would replace this one then: this import is the newer choice.
    for name in (PENDING, PENDING + '.part'):
        if (game / 'profile' / name).exists():
            log(f'saves: the save import the game\'s menu left for its next start ({name}) is dropped; '
                'this import replaces it')
            _rmtree(game / 'profile' / name)
    return account, backup


# ---- sharing a save: one zip with the save and a manifest (export_save, import_zip)
#
# NX2navi-save-<date>_<time>[-bug-report].zip:
#   manifest.json         FORMAT, FORMAT_VERSION, the game's title id and version, the NX2navi version, the export time
#   save/...              the save's files as the game keeps them (account.sav, option.sav, slot_00/progress.sav, ...)
#   bug-report/...        with bug_report: the game's latest log and nx2navi.ini; an import leaves them out
# The game's own menu writes and reads the same zip (runtime/save_share.c), with the same manifest keys.

FORMAT, FORMAT_VERSION = 'nx2navi-save', 1
MANIFEST, SAVE_DIR, REPORT_DIR = 'manifest.json', 'save/', 'bug-report/'
GAME, GAME_VERSION = 'The Legend of Zelda: Ocarina of Time', '1.1.1'
PENDING = 'save-import'   # profile/save-import: what the game's menu took from a zip, imported at the game's next start
ZIP_MAX_FILES, ZIP_MAX_BYTES = 4000, 256 << 20
SAVE_FILE = re.compile(r'^(account\.sav|option\.sav|slot_\d\d/progress\.sav)$')   # what makes a folder this game's save


def _title_hex():
    from .gamefiles import TITLE_ID
    return f'{TITLE_ID:016x}'


def _key_file(rel):
    """Key files never go into a zip, wherever a stray one was copied (a save folder taken from another tool)."""
    n = Path(rel).name.lower()
    return n.endswith('.keys') or n.endswith('.keys.txt')


def default_export_dir():
    """The player's Downloads folder (where Windows keeps it), else the Desktop, else the home folder."""
    from .winsys import known_folder
    for p in (known_folder('downloads'), Path.home() / 'Downloads', known_folder('desktop'), Path.home() / 'Desktop'):
        if p and Path(p).is_dir():
            return Path(p)
    return Path.home()


def _fresh_file(folder, stem, suffix):
    """folder/<stem><suffix>, or "<stem> (2)<suffix>" and so on when that is taken."""
    p, n = folder / f'{stem}{suffix}', 2
    while p.exists():
        p, n = folder / f'{stem} ({n}){suffix}', n + 1
    return p


NO_SAVE = 'the game has no save yet: play until it saves, then export'


# Settings whose values stay on the player's PC (runtime/cfg_private.h has the same list): the menu's test hook, and
# what the co-op of versions up to 0.9.2 kept in nx2navi.ini, which a player's file may still hold.
PRIVATE_SETTINGS = ('coop_room', 'coop_server', 'coop_name', 'coop_accept', 'coop_remove', 'menu_test_set')
PRIVATE_SHOWN = b'(set, not shown)'


def scrub_settings(data):
    """The bytes of an nx2navi.ini for a bug report: every private setting that has a value reads
    "key = (set, not shown)"; everything else, comments and line ends included, byte for byte (cfg_ini_scrub in
    runtime/cfg_private.h does the same for the game's own export)."""
    out = []
    for n, line in enumerate(data.split(b'\n')):
        text = line[:-1] if line.endswith(b'\r') else line
        start = 3 if n == 0 and text.startswith(b'\xef\xbb\xbf') else 0
        body = text[start:].lstrip(b' \t')
        key, eq, value = body.partition(b'=')
        if (eq and value.strip(b' \t') and body[:1] not in (b'#', b';', b'[')
                and key.strip(b' \t').lower() in tuple(k.encode() for k in PRIVATE_SETTINGS)):
            line = text[:text.index(b'=') + 1] + b' ' + PRIVATE_SHOWN + line[len(text):]
        out.append(line)
    return b'\n'.join(out)


def _add_report_file(z, f):
    """One file of a bug report into the zip: nx2navi.ini without its private values, anything else as it is."""
    if f.name == 'nx2navi.ini':
        z.writestr(REPORT_DIR + f.name, scrub_settings(f.read_bytes()))
    else:
        z.write(f, REPORT_DIR + f.name)


def _export_log(game, dest, version, log):
    """The bug report of a game without a save (it stopped before its first one): NX2navi-log-<date>_<time>-bug-report.zip
    with the game's latest log, nx2navi.ini and a manifest that counts no save files; import_zip refuses it as a zip
    without a save. SaveError when the game has written no log either: then there is nothing to report with."""
    extras = [f for f in (game / 'logs' / 'nx2navi.log', game / 'nx2navi.ini') if f.is_file()]
    if not extras or extras[0].name != 'nx2navi.log':
        raise SaveError(NO_SAVE)
    dest.mkdir(parents=True, exist_ok=True)
    out = _fresh_file(dest, time.strftime('NX2navi-log-%Y-%m-%d_%H-%M-%S') + '-bug-report', '.zip')
    tmp = out.with_name(out.name + '.part')
    manifest = {'format': FORMAT, 'format_version': FORMAT_VERSION, 'game': GAME, 'title_id': _title_hex(),
                'game_version': GAME_VERSION, 'nx2navi': version,
                'exported': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'exported_by': 'launcher',
                'files': 0, 'bug_report': True}
    try:
        with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as z:
            z.writestr(MANIFEST, json.dumps(manifest, indent=1) + '\n')
            for f in extras:
                _add_report_file(z, f)
        os.replace(tmp, out)
    except BaseException:
        tmp.unlink(missing_ok=True)
        raise
    log(f'saves: the game has no save yet; exported {", ".join(f.name for f in extras)} to {out}')
    return {'path': out, 'files': 0, 'extras': [f.name for f in extras]}


def export_save(game, dest=None, bug_report=False, version='', log=print):
    """Write the save the game would load now into one zip in dest (default_export_dir when None), for a bug report
    or another player. bug_report adds the game's latest log and nx2navi.ini, and still writes those two when the game
    has no save yet (_export_log): a game that stops before its first save is the one its player wants to report.
    Never a key file. Returns {'path', 'files', 'extras'} (files 0: no save; extras: the names of the bug report files
    that were there)."""
    game = Path(game)
    dest = Path(dest) if dest else default_export_dir()
    if not (game / 'profile' / 'account.uid').is_file():   # account_dir would make the account: not for an export
        if bug_report:
            return _export_log(game, dest, version, log)
        raise SaveError(NO_SAVE)
    account = account_dir(game)
    dest.mkdir(parents=True, exist_ok=True)
    stem = time.strftime('NX2navi-save-%Y-%m-%d_%H-%M-%S') + ('-bug-report' if bug_report else '')
    extras = [f for f in (game / 'logs' / 'nx2navi.log', game / 'nx2navi.ini') if bug_report and f.is_file()]
    for attempt in range(3):
        # a running game may swap a commit in while its files are read (runtime/fs_host.c swap_in): read again
        current = _newest(game, account)
        if current is None or not _has_file(current):
            if bug_report:
                return _export_log(game, dest, version, log)
            raise SaveError(NO_SAVE)
        files = [rel for rel in _files(current, set()) if not _key_file(rel)]
        out = _fresh_file(dest, stem, '.zip')
        tmp = out.with_name(out.name + '.part')
        manifest = {'format': FORMAT, 'format_version': FORMAT_VERSION, 'game': GAME, 'title_id': _title_hex(),
                    'game_version': GAME_VERSION, 'nx2navi': version,
                    'exported': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'exported_by': 'launcher',
                    'files': len(files), 'bug_report': bool(bug_report)}
        try:
            with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as z:
                z.writestr(MANIFEST, json.dumps(manifest, indent=1) + '\n')
                for rel in files:
                    z.write(current / rel, SAVE_DIR + rel.as_posix())
                for f in extras:
                    _add_report_file(z, f)
            os.replace(tmp, out)
        except FileNotFoundError:
            tmp.unlink(missing_ok=True)
            if attempt == 2:
                raise
            time.sleep(0.2)
            continue
        except BaseException:
            tmp.unlink(missing_ok=True)
            raise
        log(f'saves: exported {len(files)} files from {current} to {out}' +
            (f' with {", ".join(f.name for f in extras)}' if extras else ''))
        return {'path': out, 'files': len(files), 'extras': [f.name for f in extras]}


def _member_path(name):
    """The relative path of a zip member below save/, or SaveError when it could land outside the save."""
    parts = name.split('/')
    if '\\' in name or name.startswith('/') or any(p in ('', '.', '..') for p in parts) or ':' in name:
        raise SaveError(f'it holds a file outside the save ({SAVE_DIR}{name})')
    return Path(*parts)


def check_manifest(m):
    """SaveError unless m is the manifest of an NX2navi save of this game that this version can read."""
    if not isinstance(m, dict) or m.get('format') != FORMAT:
        raise SaveError(f'its {MANIFEST} is not an NX2navi save manifest')
    v = m.get('format_version')
    if type(v) is not int or v < 1:
        raise SaveError(f'its {MANIFEST} has no format version')
    if v > FORMAT_VERSION:
        raise SaveError(f'it was made by a newer NX2navi ({m.get("nx2navi") or "format " + str(v)}): update NX2navi '
                        'first')
    if str(m.get('title_id', '')).lower() != _title_hex():
        raise SaveError(f'it is a save of another game ({m.get("game") or m.get("title_id") or "not named"})')


def read_zip(path):
    """The manifest of a zip of export_save and its save files as [(ZipInfo, relative Path)], checked: the manifest
    (check_manifest), every path inside the save, no links, at most ZIP_MAX_FILES files and ZIP_MAX_BYTES, and among
    them the files of this game's save (SAVE_FILE). SaveError otherwise."""
    name = Path(path).name
    try:
        z = zipfile.ZipFile(path)
    except (zipfile.BadZipFile, OSError) as e:
        raise SaveError(f'{name} cannot be read as a zip file ({e})') from None
    with z:
        try:
            info = z.getinfo(MANIFEST)
        except KeyError:
            raise SaveError(f'{name} is not a save exported by NX2navi (it has no {MANIFEST}); to import a save '
                            'folder, unpack the zip and choose the folder') from None
        try:
            if info.file_size > 1 << 16:
                raise ValueError('too large')
            m = json.loads(z.read(info).decode('utf-8'))
        except (ValueError, zipfile.BadZipFile, OSError, EOFError, zlib.error) as e:
            raise SaveError(f'the {MANIFEST} of {name} cannot be read ({e})') from None
        check_manifest(m)
        members, total = [], 0
        for info in z.infolist():
            if not info.filename.startswith(SAVE_DIR) or info.is_dir():
                continue   # the manifest, bug report files
            rel = _member_path(info.filename[len(SAVE_DIR):])
            if (info.external_attr >> 16) & 0o170000 == 0o120000:
                raise SaveError(f'it holds a link ({info.filename})')
            if _key_file(rel):
                continue
            total += info.file_size
            members.append((info, rel))
            if len(members) > ZIP_MAX_FILES or total > ZIP_MAX_BYTES:
                raise SaveError(f'it holds more than a save ({len(members)} files, {total / 1e6:.0f} MB so far)')
        if not any(SAVE_FILE.match(rel.as_posix()) for _, rel in members):
            raise SaveError(f'it holds no save of {GAME}' if members else 'it holds no save files')
    return m, members


def import_zip(game, path, log=print, stamp=None):
    """Make the save in a zip of export_save the game's save (a whole save: every slot), backed up first as
    import_save does. Returns import_save's dict with the zip's manifest under 'manifest'."""
    game = Path(game)
    m, members = read_zip(path)
    stage = Path(tempfile.mkdtemp(prefix='nx2navi-save-'))
    try:
        try:
            with zipfile.ZipFile(path) as z:
                for info, rel in members:
                    out = stage / rel
                    out.parent.mkdir(parents=True, exist_ok=True)
                    with z.open(info) as f, open(out, 'wb') as g:   # the CRC is checked as the member is read
                        shutil.copyfileobj(f, g)
        except (zipfile.BadZipFile, EOFError, zlib.error) as e:
            raise SaveError(f'{Path(path).name} is damaged ({e})') from None
        files = [rel for _, rel in members]
        account, backup = _replace(game, stage, files, None, stamp or time.strftime('%Y-%m-%d %H.%M.%S'), log)
    finally:
        _rmtree(stage)
    log(f'saves: imported {len(files)} files from {path} (exported {m.get("exported", "?")} by NX2navi '
        f'{m.get("nx2navi") or "?"}, game {m.get("game_version", "?")}) into {account}' +
        (f'; the old save is in {backup}' if backup else ''))
    return {'files': len(files), 'source': Path(path), 'backup': backup, 'slot': None, 'manifest': m}
