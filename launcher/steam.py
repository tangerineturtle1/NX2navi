"""Add to Steam (Customize Launcher): the game as a non-Steam shortcut in the player's Steam library, with Navi as its
icon and the game's title screen and logo as its pictures (launcher/gameart.py). Windows only.

Steam keeps the shortcuts of each account in <Steam>/userdata/<account id>/config/shortcuts.vdf, a binary key-value
file: a map is 0x00 name 0x00 ... 0x08, a string 0x01 name 0x00 value 0x00, a 32-bit integer 0x02 name 0x00 value
(0x07 is a 64-bit one). The file is the map "shortcuts" holding one map per shortcut, named "0", "1", ... Each holds
appid, AppName, Exe, StartDir, icon, LaunchOptions and Steam's own fields; the other shortcuts are read and written
back as they are, and a shortcut of ours already there (by its name or its id) is updated in place. Its pictures go
into config/grid named by the shortcut's id: <id>p.png the cover, <id>.png the wide tile, <id>_hero.png the
background, <id>_logo.png the logo, and the icon file the shortcut names. The id is CRC-32 of Exe and AppName as
Steam writes them, with the top bit set.

Steam reads the file when it starts and writes it back when it closes, so a running Steam would neither see the new
shortcut nor keep it: Steam is asked to close (steam.exe -shutdown), the files are written, and Steam starts again.

The shortcut starts the launcher's pythonw.exe on data/steam_play.pyw, which starts this launcher with --play: the game
runs from its own folder with its console hidden, and the Python process waits for it, so Steam shows the game as
running for as long as it is (the game's own Restart included). Starting ocarina.exe directly would open a console
window beside the game, and NX2navi.exe ends a few seconds after it started the launcher.
"""
import os
import shutil
import struct
import subprocess
import time
import zlib
from pathlib import Path

NAME = 'The Legend of Zelda Ocarina of Time'
PLAY_SCRIPT = 'steam_play.pyw'
NO_WINDOW = 0x08000000 if os.name == 'nt' else 0
STEAM64_BASE = 76561197960265728

PLAY_TEXT = '''"""Written by the NX2navi launcher (Customize Launcher > Add to Steam): Steam starts the game through this file.
It runs the launcher of app/current.txt with --play, which starts the game and waits for it."""
import os
import runpy
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
with open(os.path.join(root, 'app', 'current.txt'), encoding='utf-8') as f:
    version = f.read().strip()
os.environ['NX2NAVI_LAUNCHER_ROOT'] = root
sys.argv = [os.path.join(root, 'app', version, 'launcher', 'start.pyw'), '--play'] + sys.argv[1:]
runpy.run_path(sys.argv[0], run_name='__main__')
'''


class SteamError(Exception):
    pass


# ---------------------------------------------------------------- binary VDF

T_MAP, T_STR, T_INT, T_FLOAT, T_PTR, T_WSTR, T_COLOR, T_U64, T_END = 0, 1, 2, 3, 4, 5, 6, 7, 8


def vdf_load(b):
    """A binary VDF as a list of [type, key, value]; a map's value is such a list. Raises SteamError when malformed."""
    def cstr(at):
        end = b.index(b'\0', at)
        return b[at:end].decode('utf-8', 'surrogateescape'), end + 1

    def walk(at, top):
        items = []
        while True:
            if at >= len(b):
                if top:
                    return items, at
                raise SteamError('shortcuts.vdf ends inside a map')
            t = b[at]
            at += 1
            if t == T_END:
                return items, at
            key, at = cstr(at)
            if t == T_MAP:
                v, at = walk(at, False)
            elif t == T_STR:
                v, at = cstr(at)
            elif t in (T_INT, T_FLOAT, T_PTR, T_COLOR):
                v, at = b[at:at + 4], at + 4
            elif t == T_U64:
                v, at = b[at:at + 8], at + 8
            else:
                raise SteamError(f'shortcuts.vdf holds a value of type {t}')
            items.append([t, key, v])

    try:
        items, at = walk(0, True)
    except ValueError:
        raise SteamError('shortcuts.vdf is cut short') from None
    return items


def vdf_dump(items, top=True):
    out = bytearray()
    for t, key, v in items:
        out += bytes([t]) + key.encode('utf-8', 'surrogateescape') + b'\0'
        if t == T_MAP:
            out += vdf_dump(v, False)
        elif t == T_STR:
            out += v.encode('utf-8', 'surrogateescape') + b'\0'
        else:
            out += v
    if not top or items:
        out += bytes([T_END])
    return bytes(out)


def _get(items, key):
    for it in items:
        if it[1].lower() == key.lower():
            return it
    return None


def _set(items, t, key, v):
    it = _get(items, key)
    if it:
        it[0], it[2] = t, v
    else:
        items.append([t, key, v])


def appid(exe, name):
    """The id Steam gives a shortcut: CRC-32 of its Exe and AppName fields with the top bit set (unsigned)."""
    return (zlib.crc32((exe + name).encode('utf-8')) & 0xFFFFFFFF) | 0x80000000


def add_shortcut(vdf_bytes, exe, start_dir, options, icon, name=NAME):
    """shortcuts.vdf (b'' for none) with our shortcut added or updated; returns (new bytes, id)."""
    items = vdf_load(vdf_bytes) if vdf_bytes else []
    root = _get(items, 'shortcuts')
    if root is None or root[0] != T_MAP:
        items = [[T_MAP, 'shortcuts', []]]
        root = items[0]
    sid = appid(exe, name)
    entry = None
    for it in root[2]:
        if it[0] != T_MAP:
            continue
        n, a = _get(it[2], 'AppName'), _get(it[2], 'appid')
        if (n and n[0] == T_STR and n[2] == name) or (a and a[0] == T_INT and struct.unpack('<I', a[2])[0] == sid):
            entry = it[2]
            break
    if entry is None:
        used = {it[1] for it in root[2]}
        k = 0
        while str(k) in used:
            k += 1
        entry = []
        root[2].append([T_MAP, str(k), entry])
        for key, t, v in (('IsHidden', T_INT, 0), ('AllowDesktopConfig', T_INT, 1), ('AllowOverlay', T_INT, 1),
                          ('OpenVR', T_INT, 0), ('Devkit', T_INT, 0), ('DevkitGameID', T_STR, ''),
                          ('DevkitOverrideAppID', T_INT, 0), ('LastPlayTime', T_INT, 0), ('FlatpakAppID', T_STR, '')):
            entry.append([t, key, struct.pack('<I', v) if t == T_INT else v])
        entry.append([T_MAP, 'tags', []])
    _set(entry, T_INT, 'appid', struct.pack('<I', sid))
    _set(entry, T_STR, 'AppName', name)
    _set(entry, T_STR, 'Exe', exe)
    _set(entry, T_STR, 'StartDir', start_dir)
    _set(entry, T_STR, 'icon', icon)
    _set(entry, T_STR, 'LaunchOptions', options)
    # Steam's order: appid and the strings first, then the switches and tags
    order = ['appid', 'AppName', 'Exe', 'StartDir', 'icon', 'ShortcutPath', 'LaunchOptions']
    entry.sort(key=lambda it: order.index(it[1]) if it[1] in order else len(order) + (it[1] == 'tags'))
    if _get(entry, 'ShortcutPath') is None:
        entry.insert(5, [T_STR, 'ShortcutPath', ''])
    return vdf_dump(items), sid


def has_shortcut(vdf_bytes, name=NAME):
    try:
        root = _get(vdf_load(vdf_bytes), 'shortcuts') if vdf_bytes else None
    except SteamError:
        return False
    return bool(root and root[0] == T_MAP and any(
        it[0] == T_MAP and (_get(it[2], 'AppName') or [0, 0, None])[2] == name for it in root[2]))


# ---------------------------------------------------------------- Steam on this PC

def find_steam():
    """(Steam's folder, steam.exe) from the registry, else the usual folder; None when Steam is not installed."""
    if os.name != 'nt':
        return None
    folder = exe = None
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Valve\Steam') as k:
            for name in ('SteamPath', 'SteamExe'):
                try:
                    v = winreg.QueryValueEx(k, name)[0]
                except OSError:
                    continue
                if name == 'SteamPath':
                    folder = Path(v)
                else:
                    exe = Path(v)
    except OSError:
        pass
    if folder is None:
        folder = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Steam'
    exe = exe or folder / 'steam.exe'
    if not (folder / 'userdata').is_dir() and not exe.is_file():
        return None
    return folder, exe


def accounts(folder):
    """The config folders of the accounts that signed in to Steam on this PC (config/loginusers.vdf), else of every
    account folder under userdata."""
    ud = Path(folder) / 'userdata'
    if not ud.is_dir():
        return []
    dirs = {p.name: p / 'config' for p in ud.iterdir() if p.is_dir() and p.name.isdigit() and p.name != '0'}
    signed = []
    try:
        text = (Path(folder) / 'config' / 'loginusers.vdf').read_text(encoding='utf-8', errors='replace')
        import re
        signed = [str(int(i) - STEAM64_BASE) for i in re.findall(r'"(7656119\d{10})"', text)]
    except (OSError, ValueError):
        pass
    picked = [dirs[a] for a in signed if a in dirs] or list(dirs.values())
    return sorted(set(picked))


def added(folder=None):
    """True when an account of this PC has our shortcut."""
    found = (folder, None) if folder else find_steam()
    if not found:
        return False
    for cfg in accounts(found[0]):
        try:
            if has_shortcut((cfg / 'shortcuts.vdf').read_bytes()):
                return True
        except OSError:
            pass
    return False


def steam_running():
    if os.name != 'nt':
        return False
    try:
        r = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq steam.exe', '/NH', '/FO', 'CSV'], capture_output=True,
                           text=True, creationflags=NO_WINDOW, timeout=15)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return '"steam.exe"' in r.stdout.lower()


def close_steam(exe, timeout=45.0):
    """Ask Steam to close and wait for it; False when it is still running after timeout seconds."""
    subprocess.Popen([str(exe), '-shutdown'], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, creationflags=NO_WINDOW, close_fds=True)
    end = time.time() + timeout
    while time.time() < end:
        time.sleep(1)
        if not steam_running():
            time.sleep(2)   # it writes its files on the way out
            return True
    return False


def write_atomic(path, data):
    tmp = path.with_name(path.name + '.nx2navi-tmp')
    tmp.write_bytes(data)
    os.replace(tmp, path)


def add(layout, game_dir, dump='', log=print, restart=True):
    """Add the game to Steam (or update the shortcut): the pictures from the player's game, the shortcut in every
    account's shortcuts.vdf, Steam closed first and started again. Returns the sentence the launcher shows."""
    from . import gameart
    from .ui import art
    found = find_steam()
    if not found:
        raise SteamError('Steam is not installed on this PC.')
    folder, exe_steam = found
    cfgs = accounts(folder)
    if not cfgs:
        raise SteamError('Steam has no account on this PC yet: sign in to Steam once, then try again.')
    game = Path(game_dir) / 'ocarina.exe'
    if not game.is_file():
        raise SteamError('The game is not built yet.')
    try:
        pics = gameart.make(layout, game_dir, dump)
    except gameart.ArtError as e:
        raise SteamError(f'The pictures could not be made: {e}.') from None
    script = layout.data / PLAY_SCRIPT
    write_atomic(script, PLAY_TEXT.encode('utf-8'))
    python = Path(layout.python) / 'pythonw.exe'
    if not python.is_file():   # a checkout runs under its own interpreter
        import sys
        python = Path(sys.executable).with_name('pythonw.exe')
    exe, start = f'"{python}"', f'"{layout.root}"'
    options = f'"{script}"'
    was_running = steam_running()
    if was_running:
        log('steam: asking Steam to close')
        if not close_steam(exe_steam):
            raise SteamError('Steam did not close. Close Steam yourself, then select Add to Steam again.')
    sid = appid(exe, NAME)
    try:
        for cfg in cfgs:
            grid = cfg / 'grid'
            grid.mkdir(parents=True, exist_ok=True)
            icon = grid / f'{sid}_icon.png'
            for name, file in (('icon', icon.name), ('portrait', f'{sid}p.png'), ('wide', f'{sid}.png'),
                               ('hero', f'{sid}_hero.png'), ('logo', f'{sid}_logo.png')):
                write_atomic(grid / file, art.png_bytes(pics[name]))
            vdf = cfg / 'shortcuts.vdf'
            try:
                old = vdf.read_bytes()
            except FileNotFoundError:
                old = b''
            new, got = add_shortcut(old, exe, start, options, str(icon))
            if old and not (cfg / 'shortcuts.vdf.nx2navi-backup').exists():
                shutil.copyfile(vdf, cfg / 'shortcuts.vdf.nx2navi-backup')
            write_atomic(vdf, new)
            log(f'steam: shortcut {got} in {vdf}')
    finally:   # Steam comes back also when a file could not be written
        if was_running and restart:
            subprocess.Popen([str(exe_steam)], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, creationflags=0x00000008, close_fds=True)   # detached
    if was_running and restart:
        return 'Added to Steam.'
    return 'Added. It shows up the next time you open Steam.'
