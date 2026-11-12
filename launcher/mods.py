"""The game's mods folder, for the launcher's Mods screen and Play.

The game reads <game>/mods (nx2navi.ini mods_dir names another folder; a relative one is under the game folder): one
folder per mod, each part of it where tools/mod_files.py says (runtime/mods.c). The load order and the switches are
the game's own settings, mods_order and mods_disabled in nx2navi.ini, which this module writes as runtime/cfg.c does:
the top mod wins a file two mods replace, and a mod nobody switched off is on.

A mod as players download it often does not sit that way, so prepare() puts the mods folder in order before the game
starts, and add() does the same for an archive or a folder the player picks:
- an archive in the mods folder (.zip; .7z and .rar through the system's tar, which reads them on Windows 10 and 11)
  is unpacked into a folder of its own name, unless that folder is there already. The archive stays: removing the
  folder and the archive removes the mod, and switching it off keeps both;
- a mod folder with nothing of a mod at its top and one folder inside, down to where the mod's parts are, gets that
  folder's contents (an archive unpacked into a folder of its own name, holding the mod's folder);
- a romfs, romfslite, exefs or cheats folder put straight into the mods folder becomes a mod of its own (the
  Atmosphere layouts, atmosphere/, contents/ and the title's folder, already are one as they are).
Nothing is deleted: files only move inside the mods folder, and a mod that holds several folders with mod files (a
download with options to pick from) stays as it is, with a note.

Mods that change the game's code (code cheats, ExeFS patches) need the game built with them (tools/make_game.py lift).
code_waiting() names the enabled ones whose files the last build did not take in (build-cache/mod-code.json), and
Play rebuilds the game first then. The build reads them from <game>/mods only.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

from . import gameini

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import mod_files  # noqa: E402

ARCHIVES = ('.zip', '.7z', '.rar')
LOOSE_PARTS = ('romfs', 'romfslite', 'exefs', 'cheats')   # a mod's parts that are no mod when straight in mods/
# what a folder holds at its top when a mod starts there (runtime/mods.c find_root's places)
MOD_TOP = {'romfs', 'romfslite', 'exefs', 'cheats', 'atmosphere', 'contents', mod_files.TITLE.lower()}
UNWRAP_DEPTH = 4
UNNAMED = 'Unnamed mod'
STAND_INS = 'Damaged file stand-ins'   # the launcher's own (tools/romfs_check.py), shown as such
TITLE_ID = re.compile(r'^[0-9A-Fa-f]{16}$')


class ModError(Exception):
    pass


# ---------------------------------------------------------------- the folder and the settings

def mods_dir(game_dir):
    """The mods folder the game reads: nx2navi.ini mods_dir, a relative one under the game folder, else mods."""
    try:
        d = gameini.get(game_dir, 'MODS_DIR')
    except OSError:
        d = ''
    p = Path(d) if d else Path('mods')
    return p if p.is_absolute() or (len(d) > 1 and d[1] == ':') else Path(game_dir) / p


def ensure_mods_dir(game_dir):
    d = mods_dir(game_dir)
    d.mkdir(parents=True, exist_ok=True)
    return d


def _names(game_dir, key):
    try:
        v = gameini.get(game_dir, key)
    except OSError:
        v = ''
    return [n for n in v.split('|') if n] if v else []


def _quoted(v):
    """A value as runtime/cfg.c put_value writes it: quoted when the blanks at its ends or the quotes around it would
    be lost."""
    if v and (v[0] in ' \t' or v[-1] in ' \t' or (len(v) >= 2 and v[0] == v[-1] == '"')):
        return f'"{v}"'
    return v


def order(names, wanted):
    """names in the game's order (runtime/mods.c by_order): the ones wanted names first, in its order, then the rest by
    name (byte order of their UTF-8, as strcmp)."""
    pos = {n: i for i, n in reversed(list(enumerate(wanted)))}
    return sorted(names, key=lambda n: (0, pos[n], b'') if n in pos else (1, 0, n.encode('utf-8', 'surrogateescape')))


def set_enabled(game_dir, name, on):
    """Switch a mod on or off for the game's next start (nx2navi.ini mods_disabled)."""
    off = [n for n in _names(game_dir, 'MODS_DISABLED') if n != name]
    if not on:
        off.append(name)
    gameini.put(game_dir, 'mods', 'mods_disabled', _quoted('|'.join(off)))


def move(game_dir, name, d):
    """Move a mod d places up (negative) or down in the load order (nx2navi.ini mods_order, every mod named)."""
    names = [m.name for m in scan(game_dir, details=False)]
    if name not in names:
        return
    i = names.index(name)
    j = max(0, min(len(names) - 1, i + d))
    names.insert(j, names.pop(i))
    gameini.put(game_dir, 'mods', 'mods_order', _quoted('|'.join(names)))


# ---------------------------------------------------------------- what a mod holds

class Mod:
    def __init__(self, path):
        self.path, self.name = path, path.name
        self.enabled = True
        self.romfs = mod_files.part(path, 'romfs') or mod_files.part(path, 'romfslite')
        self.exefs = mod_files.part(path, 'exefs')
        self.cheats = mod_files.part(path, 'cheats')
        self.files = []          # RomFS paths it replaces, lower case, '/' separated (details)
        self.patches = 0         # ExeFS patch files for this game's main
        self.other_version = 0   # ExeFS patch files for another version
        self.cheat_file = None   # its cheat file for this game's main
        self.code_module = False   # subsdk* or main in its exefs: code the port cannot run
        self.other_game = ''     # the title id of another game its Atmosphere folders name
        self.several = []        # folders that each hold a mod (a download with options)
        self.unsupported = ''    # why the last build could not take an ExeFS patch file of it
        self.waiting = False     # code the last build did not take in

    @property
    def usable(self):
        return not self.problem

    @property
    def problem(self):
        """Why the game leaves this mod off whatever its switch says (runtime/mods.c scan_cb), '' when it does not."""
        if '|' in self.name:
            return 'Its folder name holds "|", which the load order list uses: rename the folder.'
        if self.code_module:
            return ('It holds a code module (subsdk or main in its exefs folder), which NX2navi cannot run: the game '
                    'leaves the whole mod off.')
        if self.unsupported:
            return (f'An ExeFS patch file of it cannot be applied ({self.unsupported}): the game leaves the whole mod '
                    'off.')
        if not (self.files or self.romfs or self.patches or self.cheat_file):
            if self.other_game:
                return f'It is made for another game (title {self.other_game}), not Ocarina of Time.'
            if self.several:
                return (f'It holds {len(self.several)} folders with mod files ({", ".join(self.several[:3])}'
                        f'{", ..." if len(self.several) > 3 else ""}), a download with options to pick from: move '
                        'the one you want into the mods folder.')
            if self.other_version:
                return 'Its code patches are for another version of the game, not 1.1.1.'
            return ('It holds nothing the game reads: a mod\'s files go into a romfs, exefs or cheats folder (or '
                    'atmosphere\\contents\\' + mod_files.TITLE + '\\romfs).')
        return ''


def _main_id(game_dir):
    try:
        with open(Path(game_dir) / 'exefs' / 'main', 'rb') as f:
            head = f.read(0x60)
    except OSError:
        return ''
    return head[0x40:0x60].hex() if head[:4] == b'NSO0' and len(head) == 0x60 else ''


def _same_build(hex_id, main_id):
    """hex_id (trailing zeros may be left out) names main's build id; True when main's is unknown (runtime/mods.c)."""
    if not main_id:
        return True
    h = hex_id.lower()
    return 0 < len(h) <= 64 and all(c in '0123456789abcdef' for c in h) and main_id.startswith(h.rstrip('0')) \
        and set(main_id[len(h):]) <= {'0'}


def _pchtxt_build(data):
    """The build id a pchtxt file names (its last @nsobid- line before @stop), '' when none."""
    found = ''
    for line in data.decode('utf-8-sig', errors='replace').splitlines():
        s = line.strip()
        if s.startswith('@stop'):
            break
        if s.startswith('@nsobid-'):
            found = re.match(r'@nsobid-([0-9A-Fa-f]*)', s).group(1)
    return found


def _fnv(text):
    h = 0x811C9DC5
    for b in text.encode('utf-8'):
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return f'{h:08X}'


def _status(game_dir, name):
    """{key: (kind, note)} of a build status file (build-cache/exefs.status, tools/exefs_patches.py)."""
    out = {}
    try:
        text = (Path(game_dir) / 'build-cache' / name).read_text(encoding='utf-8', errors='replace')
    except OSError:
        return out
    for line in text.splitlines():
        f = line.split('\t')
        if len(f) >= 5:
            out[f[0]] = (f[2], f[4])
    return out


def _details(m, main_id, exefs_status):
    if m.romfs:
        top = len(m.romfs.parts)
        m.files = sorted('/'.join(Path(root, f).parts[top:]).lower()
                         for root, _dirs, files in os.walk(m.romfs) for f in files)
    if m.exefs:
        for f in m.exefs.iterdir():
            n = f.name.lower()
            if not f.is_file():
                continue
            if n.startswith('subsdk') or n == 'main':
                m.code_module = True
    for mod, rel, f in mod_files.patch_files(m.path.parent):
        if mod != m.name:
            continue
        try:
            bid = f.stem if f.suffix.lower() == '.ips' else _pchtxt_build(f.read_bytes())
        except OSError:
            bid = ''
        ok = bool(bid) and _same_build(bid, main_id)
        if not ok:
            m.other_version += 1
            continue
        m.patches += 1
        kind, note = exefs_status.get(_fnv(f'{mod}/{rel}'), ('', ''))
        if kind == 'unsupported' and not m.unsupported:
            m.unsupported = f'{f.name}: {note}' if note else f.name
    contents = m.path / 'atmosphere' / 'contents'
    for where in (contents, m.path / 'contents'):
        if where.is_dir():
            others = [d.name for d in where.iterdir() if d.is_dir() and TITLE_ID.match(d.name)
                      and d.name.upper() != mod_files.TITLE]
            if others and not m.romfs and not m.exefs and not m.cheats:
                m.other_game = others[0].upper()
    if not (m.romfs or m.exefs or m.cheats or _is_mod_top(m.path)):
        m.several = [d.name for d in _mod_folders_below(m.path)]


def _mod_folders_below(folder, depth=2):
    """The folders under folder (down to depth levels) that hold a mod at their top."""
    out = []
    try:
        subs = sorted((d for d in folder.iterdir() if d.is_dir()), key=lambda d: d.name.lower())
    except OSError:
        return out
    for d in subs:
        if _is_mod_top(d):
            out.append(d)
        elif depth > 1:
            out += _mod_folders_below(d, depth - 1)
    return out


def _is_mod_top(folder):
    try:
        return any(d.is_dir() and d.name.lower() in MOD_TOP for d in folder.iterdir())
    except OSError:
        return False


def scan(game_dir, details=True):
    """The mods of the mods folder in the game's load order, with their switches; with details what each holds."""
    folder = mods_dir(game_dir)
    mods = [Mod(p) for p in mod_files.mod_folders(folder)]
    off = set(_names(game_dir, 'MODS_DISABLED'))
    ranked = order([m.name for m in mods], _names(game_dir, 'MODS_ORDER'))
    mods.sort(key=lambda m: ranked.index(m.name))
    main_id = _main_id(game_dir) if details else ''
    exefs_status = _status(game_dir, 'exefs.status') if details else {}
    waiting = set(code_waiting(game_dir)) if details else set()
    for m in mods:
        m.enabled = m.name not in off
        if details:
            _details(m, main_id, exefs_status)
            m.cheat_file = next((f for mod, f in mod_files.cheat_files(folder, main_id[:16].upper()) if mod == m.name),
                                None) if main_id else None
            m.waiting = m.name in waiting
    return mods


def overlaps(mods):
    """{mod name: [(other mod name, files both replace, whether this one wins)]} among the enabled mods with RomFS
    files: the mod higher in the load order wins a file two replace."""
    on = [m for m in mods if m.enabled and m.files]
    out = {m.name: [] for m in on}
    sets = {m.name: set(m.files) for m in on}
    for i, a in enumerate(on):
        for j, b in enumerate(on):
            if a is not b:
                n = len(sets[a.name] & sets[b.name])
                if n:
                    out[a.name].append((b.name, n, i < j))
    return out


# ---------------------------------------------------------------- the mods' code and the build

def built_code(game_dir):
    """{"<mod>/<path>": sha256} of the mods' code the last build took in (tools/make_game.py MOD_RECORD)."""
    try:
        return json.loads((Path(game_dir) / 'build-cache' / 'mod-code.json').read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}


def code_waiting(game_dir):
    """The enabled mods whose code cheats or ExeFS patches the game was not built with (or with other ones). Only the
    game folder's own mods folder is built from."""
    folder = Path(game_dir) / 'mods'
    if mods_dir(game_dir) != folder or not folder.is_dir():
        return []
    bid = mod_files.main_build_id(Path(game_dir) / 'exefs' / 'main')
    if not bid:
        return []
    off = set(_names(game_dir, 'MODS_DISABLED'))
    built = built_code(game_dir)
    out = []
    for key, sha in mod_files.code_record(folder, bid).items():
        mod = key.split('/', 1)[0]
        if mod not in off and built.get(key) != sha and mod not in out:
            out.append(mod)
    return out


# ---------------------------------------------------------------- putting downloads in order

def _unique(folder, name, file=False):
    """folder / name, or the first free "name 2", "name 3" ... (for a file "stem (2).txt" ...)."""
    p, k = folder / name, 2
    stem, suffix = (Path(name).stem, Path(name).suffix) if file else (name, '')
    while p.exists():
        p = folder / (f'{stem} ({k}){suffix}' if file else f'{name} {k}')
        k += 1
    return p


def _merge_into(src, dst):
    """Move src's entries into dst (made when missing) and remove src: a folder dst holds already is merged into, and
    a file of a name dst holds already moves in under a free name (a readme beside the wrapped folder)."""
    dst.mkdir(parents=True, exist_ok=True)
    for e in list(src.iterdir()):
        t = dst / e.name
        if t.is_dir() and e.is_dir():
            _merge_into(e, t)
        else:
            shutil.move(str(e), str(_unique(dst, e.name, file=True) if t.exists() else t))
    src.rmdir()


def unwrap(folder):
    """A mod folder with nothing of a mod at its top and one folder inside (files beside it, a readme, do not count),
    down to UNWRAP_DEPTH levels until the mod starts: that folder's contents move up. True when it did."""
    inner = folder
    for _ in range(UNWRAP_DEPTH):
        if _is_mod_top(inner):
            break
        subs = [d for d in inner.iterdir() if d.is_dir()]
        if len(subs) != 1:
            return False
        inner = subs[0]
    else:
        if not _is_mod_top(inner):
            return False
    if inner == folder:
        return False
    # the chain's top folder may share its name with a folder of the mod (MyMod/romfs/romfs): moved aside first. The
    # mod's folders go up first, then whatever lay beside them on the way down (a readme), so nothing stays behind
    top = next(p for p in [inner, *inner.parents] if p.parent == folder)
    aside = _unique(folder, '.unwrapping')
    top.rename(aside)
    inner = aside / inner.relative_to(top)
    _merge_into(inner, folder)
    if aside.exists():
        _merge_into(aside, folder)
    return True


def _safe_members(names):
    for n in names:
        p = n.replace('\\', '/')
        if p.startswith('/') or re.match(r'^[A-Za-z]:', p) or '..' in p.split('/'):
            raise ModError(f'it holds a file outside its own folder ({n})')


def _tar():
    if os.name == 'nt':
        t = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32' / 'tar.exe'
        return str(t) if t.is_file() else None
    return shutil.which('bsdtar')


def unpack(archive, dest):
    """Unpack an archive into dest (a new folder). ModError when it cannot be."""
    dest.mkdir(parents=True)
    try:
        if archive.suffix.lower() == '.zip':
            try:
                with zipfile.ZipFile(archive) as z:
                    _safe_members(z.namelist())
                    z.extractall(dest)
            except (zipfile.BadZipFile, NotImplementedError, RuntimeError) as e:
                raise ModError(f'it is not a zip file the launcher can read ({e})')
        else:
            tar = _tar()
            if not tar:
                raise ModError(f'this PC has no tool for {archive.suffix} files: unpack it yourself into a folder of '
                               'the mods folder')
            flags = {'creationflags': 0x08000000} if os.name == 'nt' else {}
            r = subprocess.run([tar, '-tf', str(archive)], capture_output=True, text=True, errors='replace', **flags)
            if r.returncode:
                raise ModError(f'it could not be read ({r.stderr.strip()[:200] or "tar " + str(r.returncode)}): '
                               'unpack it yourself into a folder of the mods folder')
            _safe_members(r.stdout.splitlines())
            r = subprocess.run([tar, '-xf', str(archive), '-C', str(dest)], capture_output=True, text=True,
                               errors='replace', **flags)
            if r.returncode:
                raise ModError(f'it could not be unpacked ({r.stderr.strip()[:200] or "tar " + str(r.returncode)}): '
                               'unpack it yourself into a folder of the mods folder')
    except BaseException:
        shutil.rmtree(dest, ignore_errors=True)
        raise


def prepare(game_dir, log=None):
    """Put the mods folder in order for the game (see the top of this file). Returns what it did, one sentence each."""
    folder = mods_dir(game_dir)
    done = []
    if not folder.is_dir():
        return done

    def note(text):
        done.append(text)
        if log:
            log(f'mods: {text}')

    for a in sorted(p for p in folder.iterdir() if p.is_file() and p.suffix.lower() in ARCHIVES):
        name = a.stem.strip().rstrip('.')
        target = folder / name
        if not name or target.exists():
            continue
        try:
            unpack(a, target)
        except (ModError, OSError) as e:
            note(f'{a.name} was not unpacked: {e}')
            continue
        unwrap(target)
        note(f'{a.name} unpacked into {target.name}.')
    loose = [p for p in folder.iterdir() if p.is_dir() and p.name.lower() in LOOSE_PARTS]
    if loose:
        target = _unique(folder, UNNAMED)
        target.mkdir()
        for p in loose:
            shutil.move(str(p), str(target / p.name.lower()))
        note(f'{", ".join(p.name for p in loose)} moved into a mod folder of its own, {target.name}: rename it as you '
             'like.')
    for d in mod_files.mod_folders(folder):
        # an Atmosphere layout straight in the mods folder is a mod as it is; a link is the player's own arrangement
        if d.name.lower() in MOD_TOP or d.is_symlink() or getattr(d, 'is_junction', lambda: False)():
            continue
        try:
            if unwrap(d):
                note(f'{d.name}: its files moved up to where the game looks for them.')
        except OSError as e:
            note(f'{d.name}: its files could not be moved up ({e}).')
    return done


def add(game_dir, src, log=None):
    """Add an archive or a folder the player picked as a mod: unpacked or copied into the mods folder under its name
    (a free one), in order for the game. Returns the new mod's folder."""
    src = Path(src)
    folder = ensure_mods_dir(game_dir)
    if src.is_dir():
        try:
            src.resolve().relative_to(folder.resolve())
            raise ModError('it is in the mods folder already')
        except ValueError:
            pass
        name = src.name if src.name.lower() not in MOD_TOP else UNNAMED
        target = _unique(folder, name)
        if src.name.lower() in MOD_TOP:   # the player picked the mod's romfs (or atmosphere) folder itself
            target.mkdir()
            shutil.copytree(src, target / src.name.lower() if src.name.lower() in LOOSE_PARTS else target / src.name)
        else:
            shutil.copytree(src, target)
    elif src.suffix.lower() in ARCHIVES:
        target = _unique(folder, src.stem.strip().rstrip('.') or UNNAMED)
        unpack(src, target)
    else:
        raise ModError('it is no archive (.zip, .7z or .rar) and no folder')
    unwrap(target)
    if log:
        log(f'mods: added {src} as {target.name}')
    return target

