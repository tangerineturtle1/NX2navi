"""What a mod brings beyond the files the game reads: the launcher's own layer over launcher/mods.py
(docs/mods-launcher.md for mod authors, docs/launcher.md "Mods" for players).

launcher/mods.py is the mods folder as the game reads it: one folder per mod, its romfs, exefs and cheats parts, the
load order, the switches, archives unpacked, code patches built in. This module adds what an optional mod.json in a
mod's folder names:

    {"name": "Shown Name", "version": "1.0", "author": "...", "description": "...",
     "launcher": "launcher_plugin.py", "dlls": ["bin/x.dll"], "env": {"KEY": "value"}}

- launcher: a Python file the launcher imports when the mod is on; its register(api) gets a ModApi and may add rows
  to the home screen and menus of its own.
- dlls: libraries loaded into the game's process before its main thread runs (winsys.start_game).
- env: environment variables for the game's process.

Every path is relative to the mod's folder and must stay inside it. The switch is the game's own (nx2navi.ini
mods_disabled, mods.set_enabled), so one switch turns off the mod's game files and its launcher part together, and
the order is the game's load order (mods_order): the top mod wins a variable two mods set.

scan gives the mods of mods.scan with their mod.json read (Mod below); mods.scan itself returns the same objects.

A mod can come as one file, a zip named <Mod Name>.nx2mod, holding the mod's files at its top or in one top folder
(pack makes one). install_archive unpacks it into <mods>/<Mod Name>, over a mod of that name whose settings.json is
kept; add does that for a file the player picks and leaves every other archive and folder to mods.add. prepare, run
where mods.prepare was (before Play, when the launcher reads the mods), does it for the .nx2mod files lying in the
mods folder, which stay there as the other archives do, and then calls mods.prepare.
"""
import itertools
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import threading
import traceback
import types
import zipfile
from pathlib import Path, PurePosixPath, PureWindowsPath

from . import mods, winsys

MANIFEST = 'mod.json'
SETTINGS = 'settings.json'
STAMP = '.nx2mod-source'           # in a mod's folder: which .nx2mod of the mods folder prepare unpacked into it
NX2MOD = '.nx2mod'
ARCHIVE_SUFFIXES = (NX2MOD, '.zip')
LEGACY = '.installed'              # where earlier launchers moved the .nx2mod files they had installed
MAX_FILES = 20000                  # an archive with more entries is refused
MAX_BYTES = 8 << 30                # and one that unpacks to more bytes than this
KIND_TEXT = {'romfs': 'game files', 'exefs': 'game patches', 'cheats': 'cheats', 'launcher': 'a launcher add-on',
             'dlls': 'game code (DLL)'}
# the names code written against the earlier launcher.mods finds there still (mods.__getattr__)
FROM_MODS = ('MANIFEST', 'SETTINGS', 'ARCHIVE_SUFFIXES', 'MAX_FILES', 'MAX_BYTES', 'KIND_TEXT', 'DLLS_VAR', 'inside',
             'read_mod', 'ArchiveError', 'mod_name_from', 'install_archive', 'pack', 'game_env', 'game_dlls',
             'dlls_env', 'start_game', 'RowSpec', 'Host', 'ModApi', 'load_plugins', 'unload_plugins', 'in_tk_thread')


def inside(base, rel):
    """base/rel when rel is a relative path that stays inside base (after resolving .. and links), else None."""
    if not isinstance(rel, str) or not rel.strip():
        return None
    if PureWindowsPath(rel).drive or PureWindowsPath(rel).root or PurePosixPath(rel).is_absolute():
        return None
    root = Path(base).resolve()
    p = (root / rel).resolve()
    if p == root or not p.is_relative_to(root):
        return None
    return p


class Mod(mods.Mod):
    """A mod of mods.scan with its mod.json: the manifest (empty when it has none or a bad one), the launcher add-on,
    the libraries and the variables it names, the parts found (KIND_TEXT keys) and, in problems, what is wrong with
    the manifest or the add-on as text for the player. mods.Mod's own problem (why the game leaves the mod off) stays
    apart."""

    manifest, kinds, problems, plugin, dlls, env = {}, (), (), None, (), {}

    @property
    def title(self):
        t = self.manifest.get('name')
        return t if isinstance(t, str) and t.strip() else self.name

    def field(self, key):
        v = self.manifest.get(key, '')
        return v if isinstance(v, str) else str(v)

    @property
    def extras(self):
        """Whether it brings something of this module's: a launcher add-on, game code or variables."""
        return bool(self.plugin or self.dlls or self.env)

    @property
    def problem(self):
        p = super().problem
        if p and self.extras and not ('|' in self.name or self.code_module or self.unsupported):
            return ''   # nothing the game reads, which is no fault in a mod that is an add-on or game code only
        return p

    def __repr__(self):
        return f'Mod({self.name!r}, enabled={self.enabled}, kinds={self.kinds}, problems={self.problems})'


def extend(m):
    """Make a mods.Mod a Mod of this module, in place: read its mod.json. Returns it."""
    if isinstance(m, Mod):
        return m
    m.__class__ = Mod
    path = m.path
    m.manifest, m.problems, m.plugin, m.dlls, m.env = {}, [], None, [], {}
    m.kinds = [k for k in ('romfs', 'exefs', 'cheats') if getattr(m, k)]
    mf = path / MANIFEST
    if not mf.is_file():
        return m
    try:
        data = json.loads(mf.read_text(encoding='utf-8-sig'))
        if not isinstance(data, dict):
            raise ValueError('it is not a JSON object')
    except (OSError, ValueError) as e:
        m.problems.append(f'{MANIFEST} is not valid ({e}): its launcher add-on, game code and variables are '
                          'left out.')
        return m
    m.manifest = data
    lp = data.get('launcher')
    if lp:
        p = inside(path, lp)
        if p is None:
            m.problems.append(f'launcher: {lp!r} lies outside the mod folder.')
        elif not p.is_file():
            m.problems.append(f'launcher: {lp} is missing.')
        else:
            m.plugin = p
            m.kinds.append('launcher')
    dl = data.get('dlls', [])
    if not isinstance(dl, list):
        m.problems.append('dlls: not a list of paths.')
        dl = []
    for d in dl:
        p = inside(path, d)
        if p is None:
            m.problems.append(f'dlls: {d!r} lies outside the mod folder.')
        elif not p.is_file():
            m.problems.append(f'dlls: {d} is missing.')
        else:
            m.dlls.append(p)
    if m.dlls:
        m.kinds.append('dlls')
    env = data.get('env', {})
    if isinstance(env, dict) and all(isinstance(k, str) and k and '=' not in k and isinstance(v, str)
                                     for k, v in env.items()):
        m.env = dict(env)
    else:
        m.problems.append('env: not an object of text values.')
    return m


def read_mod(path, disabled=()):
    """One mod folder as a Mod, without the mods folder's order or the game's details."""
    m = mods.Mod(Path(path))
    m.enabled = m.name not in disabled
    return extend(m)


def scan(game_dir, log=None, details=True):
    """The mods of game_dir's mods folder in the game's load order, each with its mod.json read ([] without a game
    folder). details as mods.scan's: what each mod's game files are, which the Mods screen shows and starting the
    game does not need. log is not used any more: the .nx2mod files of the mods folder are unpacked by prepare."""
    return mods.scan(game_dir, details) if game_dir else []


# ---------------------------------------------------------------- single-file mods

class ArchiveError(mods.ModError):
    pass


def mod_name_from(path):
    name = Path(path).name
    for s in ARCHIVE_SUFFIXES:
        if name.lower().endswith(s):
            return name[:-len(s)].strip()
    return Path(path).stem.strip()


def _valid_name(name):
    return bool(name) and name not in ('.', '..') and not name.startswith('.') and \
        not any(c in name for c in '|/\\:*?"<>') and name == name.strip()


def _members(z):
    """(archive name, relative parts) of every file, the top folder stripped when everything lies in one. Raises
    ArchiveError for a name that leaves the folder, a link, too many files or too many bytes."""
    infos = z.infolist()
    if len(infos) > MAX_FILES:
        raise ArchiveError(f'it holds more than {MAX_FILES} files')
    files, total = [], 0
    for i in infos:
        raw = i.filename.replace('\\', '/')
        if raw.startswith('/') or PureWindowsPath(raw).drive or re.match(r'^[A-Za-z]:', raw):
            raise ArchiveError(f'{i.filename!r} is an absolute path')
        parts = [p for p in raw.split('/') if p not in ('', '.')]
        if any(p == '..' for p in parts):
            raise ArchiveError(f'{i.filename!r} leaves the mod folder')
        if any(c in p for p in parts for c in ':*?"<>|') or any(p != p.rstrip(' .') for p in parts):
            raise ArchiveError(f'{i.filename!r} is not a valid file name')
        mode = (i.external_attr >> 16) & 0xFFFF
        if stat.S_ISLNK(mode):
            raise ArchiveError(f'{i.filename!r} is a link')
        if i.is_dir() or not parts:
            continue
        total += i.file_size
        if total > MAX_BYTES:
            raise ArchiveError(f'it unpacks to more than {MAX_BYTES >> 30} GB')
        files.append((i, parts))
    if not files:
        raise ArchiveError('it holds no files')
    tops = {p[0] for _, p in files}
    if len(tops) == 1 and all(len(p) > 1 for _, p in files):
        files = [(i, p[1:]) for i, p in files]
    return files


def install_archive(archive, root, name=None):
    """Unpack a single-file mod into root/<name> (name: the file's name without .nx2mod or .zip) and return that
    folder. A mod of that name is replaced; its settings.json is kept. Raises ArchiveError (the mods folder as it
    was) for an unsafe or unreadable archive."""
    name = name or mod_name_from(archive)
    if not _valid_name(name):
        raise ArchiveError(f'{name!r} cannot be a mod folder name')
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    dest, tmp, old = root / name, root / f'.{name}.unpacking', root / f'.{name}.old'
    for p in (tmp, old):
        shutil.rmtree(p, ignore_errors=True)
    try:
        with zipfile.ZipFile(archive) as z:
            files = _members(z)
            tmp.mkdir()
            written = 0
            for info, parts in files:
                out = tmp.joinpath(*parts)
                if not out.resolve().is_relative_to(tmp.resolve()):
                    raise ArchiveError(f'{info.filename!r} leaves the mod folder')
                out.parent.mkdir(parents=True, exist_ok=True)
                with z.open(info) as src, open(out, 'wb') as dst:
                    while chunk := src.read(1 << 20):
                        written += len(chunk)   # the sizes in the archive may lie
                        if written > MAX_BYTES:
                            raise ArchiveError(f'it unpacks to more than {MAX_BYTES >> 30} GB')
                        dst.write(chunk)
        if dest.exists():
            if (dest / SETTINGS).is_file():
                shutil.copy2(dest / SETTINGS, tmp / SETTINGS)
            os.replace(dest, old)
        os.replace(tmp, dest)
    except (zipfile.BadZipFile, zipfile.LargeZipFile, OSError, RuntimeError, NotImplementedError, EOFError) as e:
        if old.exists() and not dest.exists():
            os.replace(old, dest)
        raise ArchiveError(str(e) or type(e).__name__) from e
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        shutil.rmtree(old, ignore_errors=True)
    return dest


def holds_manifest(archive):
    """Whether a zip holds a mod.json where install_archive would put it at the mod's top. False for a file that is
    no zip or one install_archive would refuse."""
    try:
        with zipfile.ZipFile(archive) as z:
            return any(parts == [MANIFEST] for _, parts in _members(z))
    except (ArchiveError, zipfile.BadZipFile, zipfile.LargeZipFile, OSError, RuntimeError, NotImplementedError,
            EOFError):
        return False


def _source(archive):
    s = archive.stat()
    return {'file': archive.name, 'size': s.st_size, 'mtime_ns': s.st_mtime_ns}


def _stamp(folder):
    try:
        return json.loads((folder / STAMP).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None


def unpack_single(game_dir, log=None):
    """Unpack the single-file mods lying in the mods folder. Returns what was done, one sentence each, as mods.prepare
    does.

    A .nx2mod file is unpacked into the folder of its name (install_archive) when there is no such folder, and again
    when the file changed since this function unpacked it (the player put a newer one in its place: the folder notes
    in STAMP which file it came from). A folder that came another way is never replaced. A .zip that holds a mod.json
    is unpacked the same way when its folder is missing, so that the mod.json ends at the folder's top; every other
    archive is mods.prepare's. The files stay where they are, as mods.prepare leaves archives: removing the folder and
    the file removes the mod.

    The .nx2mod files an earlier launcher moved to <mods>/.installed come back beside their folders (which stay as
    they are), and that folder goes."""
    folder = mods.mods_dir(game_dir)
    done = []
    if not folder.is_dir():
        return done

    def note(text):
        done.append(text)
        if log:
            log(f'mods: {text}')

    legacy = folder / LEGACY
    if legacy.is_dir():
        try:
            for p in list(legacy.iterdir()):
                if p.is_file() and not (folder / p.name).exists():
                    os.replace(p, folder / p.name)
            legacy.rmdir()
        except OSError:
            pass   # something else lies in it, or a file is held: it stays, and mods.scan leaves it out
    for a in sorted(p for p in folder.iterdir() if p.is_file() and p.name.lower().endswith(ARCHIVE_SUFFIXES)):
        single = a.name.lower().endswith(NX2MOD)
        name = mod_name_from(a)
        target = folder / name
        try:
            source = _source(a)
            was = target.exists()
            if was:
                if not single or _stamp(target) in (None, source):
                    continue
            elif not single and not (_valid_name(name) and holds_manifest(a)):
                continue
            install_archive(a, folder, name)
            (target / STAMP).write_text(json.dumps(source), encoding='utf-8')
        except (ArchiveError, OSError) as e:
            note(f'{a.name} was not unpacked: {e}')
            continue
        note(f'{a.name} unpacked into {name}' + (', over the mod that was there; its settings stay.' if was else '.'))
    return done


def prepare(game_dir, log=None):
    """Put the mods folder in order for the game: the single-file mods lying in it (unpack_single), then mods.prepare.
    Returns what was done, one sentence each."""
    return unpack_single(game_dir, log) + mods.prepare(game_dir, log)


def _single(src):
    src = Path(src)
    return src.is_file() and (src.name.lower().endswith(NX2MOD)
                              or (src.suffix.lower() == '.zip' and holds_manifest(src)))


def replaces(game_dir, src):
    """Whether add would unpack src over a mod that is there."""
    return _single(src) and (mods.mods_dir(game_dir) / mod_name_from(src)).exists()


def add(game_dir, src, log=None):
    """Add what the player picked as a mod and return its folder. A .nx2mod file, or a .zip that holds a mod.json, is
    a single-file mod: unpacked into the folder of the file's name, over a mod of that name whose settings.json is
    kept (install_archive). Anything else is mods.add's: another archive or a folder, under a free name."""
    if _single(src):
        target = install_archive(src, mods.ensure_mods_dir(game_dir))
        if log:
            log(f'mods: installed {src} into {target.name}')
        return target
    return mods.add(game_dir, src, log)


def pack(mod_dir, out_file):
    """Pack a mod's folder into a single-file mod (a zip with the files at its top). The mod's settings.json, Python
    caches and hidden files stay out. Returns out_file as a Path."""
    mod_dir, out_file = Path(mod_dir), Path(out_file)
    out_file.parent.mkdir(parents=True, exist_ok=True)
    tmp = out_file.with_name(out_file.name + '.tmp')
    try:
        with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as z:
            for p in sorted(mod_dir.rglob('*')):
                rel = p.relative_to(mod_dir)
                if any(part.startswith('.') or part == '__pycache__' for part in rel.parts):
                    continue
                if rel.as_posix() == SETTINGS or p.is_symlink() or not p.is_file():
                    continue
                if p.resolve() == out_file.resolve() or p == tmp:
                    continue
                z.write(p, rel.as_posix())
        os.replace(tmp, out_file)
    finally:
        tmp.unlink(missing_ok=True)
    return out_file


# ---------------------------------------------------------------- starting the game

def game_env(mod_list, extra=None):
    """The variables the enabled mods add (the top mod wins a name two mods set), then extra; plus
    NX2NAVI_MODS_LOADED, the enabled mods' names joined by '|'."""
    on = [m for m in mod_list if m.enabled]
    env = {}
    for m in reversed(on):
        env.update(m.env)
    env.update(extra or {})
    env['NX2NAVI_MODS_LOADED'] = '|'.join(m.name for m in on)
    return env


def game_dlls(mod_list, extra=None):
    return [str(p) for m in mod_list if m.enabled for p in m.dlls] + [str(p) for p in (extra or [])]


DLLS_VAR = 'NX2NAVI_MOD_DLLS'


def dlls_env(libs):
    """The value of NX2NAVI_MOD_DLLS for the libraries libs: their full paths joined by '|'. The game loads the
    libraries this variable names itself and calls each one's nx2navi_plugin_init (runtime/plugins.c plugins_start,
    docs/plugins.md). For a game the launcher starts, they are the libraries winsys.start_game has already loaded,
    and loading one again gives the same library; a process the game starts itself (its Restart) inherits the
    variable and so loads them too."""
    return '|'.join(os.path.abspath(d) for d in libs)


def start_game(game_dir, exe, env_extra=None, dlls=None, log=print):
    """Start the game with what the enabled mods add (game_env, game_dlls), as Play and the Steam shortcut do. The
    returned process carries dll_errors, the libraries that did not load (the game runs without them)."""
    ms = scan(game_dir, details=False)
    env, libs = game_env(ms, env_extra), game_dlls(ms, dlls)
    if os.name == 'nt':   # where winsys.start_game loads libraries; set when empty too, over a value inherited
        env[DLLS_VAR] = dlls_env(libs)
    loaded = env['NX2NAVI_MODS_LOADED']
    if loaded or libs:
        log(f'mods: starting the game with {loaded.replace("|", ", ") or "no mods"}'
            + (f'; game code: {", ".join(libs)}' if libs else ''))
    return winsys.start_game(exe, env, libs, log)


# ---------------------------------------------------------------- launcher add-ons

class RowSpec:
    """A row a plugin asks for (ModApi.action and the others); the window makes its own row of it."""

    def __init__(self, kind, label='', value='', desc='', on=None, get=None, set=None, secret=False, prompt=''):
        self.kind, self.label, self.value, self.desc, self.on = kind, label, value, desc, on
        self.get, self.set, self.secret, self.prompt = get, set, secret, prompt

    def __repr__(self):
        return f'RowSpec({self.kind!r}, {self.label!r})'


class Host:
    """What a ModApi asks of the launcher. This one has no window (tests, the Steam shortcut); the window's App
    implements the same methods."""

    def __init__(self, game_dir='', log=print):
        self._game_dir, self._log = game_dir, log
        self.messages, self.menus = [], []

    def mod_game_dir(self):
        return self._game_dir

    def mod_log(self, text):
        self._log(text)

    def mod_refresh(self):
        pass

    def mod_show_menu(self, api, title, rows):
        self.menus.append((api, title, rows))

    def mod_back(self):
        if self.menus:
            self.menus.pop()

    def mod_home(self):
        self.menus.clear()

    def mod_message(self, text):
        self.messages.append(text)

    def mod_ask_text(self, title, prompt, initial='', secret=False):
        return None

    def mod_play(self, env, dlls):
        return False

    def mod_game_running(self):
        return False

    def mod_open(self, path):
        winsys.open_path(path)


class ModApi:
    """What a launcher add-on's register(api) gets: one per mod (docs/mods-launcher.md lists every method)."""

    def __init__(self, mod, host):
        self._mod, self._host = mod, host
        self.mod_dir = Path(mod.path)
        self.game_dir = Path(host.mod_game_dir() or '.')
        self.name = mod.name
        self.home_rows = []
        self.exit_callbacks = []
        self.module = None

    def log(self, text):
        self._host.mod_log(f'mod {self.name}: {text}')

    # rows
    def add_home_row(self, label, value='', desc='', on=None):
        self.home_rows.append(RowSpec('value', label, value=value, desc=desc, on=on))
        self._host.mod_refresh()

    def action(self, label, on, desc=''):
        return RowSpec('action', label, desc=desc, on=on)

    def value(self, label, value, on=None, desc=''):
        return RowSpec('value', label, value=value, desc=desc, on=on)

    def toggle(self, label, get, set, desc=''):
        return RowSpec('toggle', label, desc=desc, get=get, set=set)

    def text(self, label, get, set, desc='', secret=False, prompt=''):
        return RowSpec('text', label, desc=desc, get=get, set=set, secret=secret, prompt=prompt)

    def divider(self):
        return RowSpec('divider')

    # screens
    def show_menu(self, title, rows):
        self._host.mod_show_menu(self, title, rows)

    def refresh(self):
        self._host.mod_refresh()

    def back(self):
        self._host.mod_back()

    def home(self):
        self._host.mod_home()

    def message(self, text):
        self._host.mod_message(text)

    def ask_text(self, title, prompt, initial='', secret=False):
        return self._host.mod_ask_text(title, prompt, initial, secret)

    # the game
    def _here(self, p):
        p = Path(p)
        return p if p.is_absolute() else self.mod_dir / p

    def play(self, env=None, dlls=None):
        return bool(self._host.mod_play(dict(env or {}), [str(self._here(d)) for d in (dlls or [])]))

    def game_running(self):
        return bool(self._host.mod_game_running())

    def on_game_exit(self, callback):
        self.exit_callbacks.append(callback)

    def start_process(self, cmd, cwd=None, console=True, env=None):
        """Start a program; console: in a console window of its own. cwd defaults to the mod's folder."""
        full = dict(os.environ)
        full.update(env or {})
        if os.name == 'nt':
            flags = {'creationflags': 0x00000010 if console else winsys.CREATE_NO_WINDOW}   # CREATE_NEW_CONSOLE
        else:
            flags = {'start_new_session': True}
        return subprocess.Popen(cmd, cwd=str(cwd or self.mod_dir), env=full, **flags)

    # files
    def load_settings(self):
        try:
            d = json.loads((self.mod_dir / SETTINGS).read_text(encoding='utf-8'))
            return d if isinstance(d, dict) else {}
        except (OSError, ValueError):
            return {}

    def save_settings(self, d):
        path = self.mod_dir / SETTINGS
        tmp = path.with_name(path.name + '.tmp')
        try:
            tmp.write_text(json.dumps(d, indent=2, ensure_ascii=False), encoding='utf-8')
            os.replace(tmp, path)
        finally:
            tmp.unlink(missing_ok=True)

    def open(self, path):
        self._host.mod_open(str(self._here(path)))


_serial = itertools.count(1)


def _import(path, name):
    """The plugin file as a module of its own, without writing a bytecode cache into the mod's folder. It is
    registered in sys.modules (dataclasses and pickle look there) under a name no other module has."""
    modname = f'nx2navi_mod_{re.sub(r"[^0-9A-Za-z_]", "_", name)}_{next(_serial)}'
    module = types.ModuleType(modname)
    module.__file__ = str(path)
    sys.modules[modname] = module
    try:
        exec(compile(Path(path).read_bytes(), str(path), 'exec'), module.__dict__)
    except BaseException:
        sys.modules.pop(modname, None)
        raise
    return module


def load_plugins(mod_list, host):
    """Import the launcher add-on of every enabled mod and call its register(api); the ModApis of those that
    registered. An add-on that raises is logged and its problem added to the mod; it never stops the launcher."""
    apis = []
    for m in mod_list:
        if not (m.enabled and m.plugin):
            continue
        api = ModApi(m, host)
        try:
            api.module = _import(m.plugin, m.name)
            reg = getattr(api.module, 'register', None)
            if not callable(reg):
                raise AttributeError(f'{m.plugin.name} has no register(api)')
            reg(api)
        except (Exception, SystemExit) as e:   # SystemExit: an add-on that calls sys.exit does not close the launcher
            host.mod_log(f'mod {m.name}: its launcher add-on failed:\n{traceback.format_exc()}')
            m.problems.append(f'Its launcher add-on failed: {type(e).__name__}: {e}')
            if api.module:
                sys.modules.pop(api.module.__name__, None)
            continue
        host.mod_log(f'mod {m.name}: launcher add-on loaded')
        apis.append(api)
    return apis


def unload_plugins(apis, host):
    """Call each add-on's optional unregister(api) and forget its module."""
    for api in apis:
        fn = getattr(api.module, 'unregister', None)
        if callable(fn):
            try:
                fn(api)
            except Exception:
                host.mod_log(f'mod {api.name}: unregister failed:\n{traceback.format_exc()}')
        if api.module:
            sys.modules.pop(api.module.__name__, None)


def in_tk_thread():
    return threading.current_thread() is threading.main_thread()
