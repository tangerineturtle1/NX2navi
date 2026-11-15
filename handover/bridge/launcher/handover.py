"""NX2navi is now Triforce: the window NX2navi's launcher becomes with this release.

NX2navi and Triforce have merged and go on as Triforce. This release takes the place of NX2navi's launcher in its own
update channel: the stub (NX2navi.exe) starts launcher/start.pyw of the version app/current.txt names, as it starts
every version, and start.pyw opens this window instead of the old launcher's. The window

  - starts Triforce Setup, Triforce's launcher: the installed one when Triforce is on the PC already, else the copy
    this release carries (launcher/triforce/TriforceSetup.exe, checked against the sha256 in setup.json beside it);
  - still starts the NX2navi game that is built on the PC (ocarina.exe in the game folder), for whoever wants to go on
    with it for now;
  - shows where the player's game files and NX2navi game folder are, which Triforce Setup asks for.

It removes and changes nothing of NX2navi: the game folder, the saves in it, the settings (data/launcher.json) and the
build tools stay as they are, so Triforce Setup's Convert saves page finds the saves. Uninstalling NX2navi (Windows'
Apps list runs NX2navi.exe --uninstall) is done here as the old launcher did it.

It keeps the way back open: once an hour at most it asks the feed NX2navi's launcher asked (the GitHub releases of
feed.json's repository) for a version newer than this one and, when there is one, stages it exactly as the old
launcher did (checked download, app/<version>, previous.txt, current.txt), so the stub starts that version next time
and falls back to this one when it does not start. Whoever publishes NX2navi's releases can so replace this window.

Only the standard library and tkinter are used: the launcher's own Python (python/ beside NX2navi.exe) runs it.
"""
import ctypes
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import webbrowser
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
APP = HERE.parent
ART = HERE / 'art'
SETUP_DIR = HERE / 'triforce'

SITE = 'https://triforceproject.com'
DISCORD = 'https://discord.gg/triforce'
DEFAULT_FEED = {'type': 'github', 'repo': 'tangerineturtle1/NX2navi'}
GITHUB_API = 'https://api.github.com'
MANIFEST = 'NX2navi-update.json'
CHECK_EVERY = 3600

CREATE_NO_WINDOW = 0x08000000
DETACHED_PROCESS = 0x00000008
CREATE_NEW_PROCESS_GROUP = 0x00000200
NX_UNINSTALL_KEY = r'Software\Microsoft\Windows\CurrentVersion\Uninstall\NX2navi'
TRIFORCE_UNINSTALL_KEY = r'Software\Microsoft\Windows\CurrentVersion\Uninstall\Triforce'
# what NX2navi's launcher put in its folder (its launcher/winsys.py OWNED): an uninstall deletes these and nothing else
OWNED = ('NX2navi.exe', 'app', 'python', 'python.next', 'pydeps', 'data', 'toolchain')


# ---------------------------------------------------------------- the launcher's folder

def launcher_root():
    return Path(os.environ.get('NX2NAVI_LAUNCHER_ROOT') or APP.parents[1])


def read_text(path, default=''):
    try:
        return Path(path).read_text(encoding='utf-8-sig').strip()
    except (OSError, UnicodeDecodeError):
        return default


def write_text(path, text):
    path = Path(path)
    tmp = path.with_name(path.name + '.tmp')
    tmp.write_text(text + '\n', encoding='utf-8')
    os.replace(tmp, path)


def my_version():
    return read_text(APP / 'VERSION').split('+', 1)[0].strip() or APP.name


def log(root, line):
    """One line into data/logs/handover.log; never an error."""
    try:
        logs = Path(root) / 'data' / 'logs'
        logs.mkdir(parents=True, exist_ok=True)
        with open(logs / 'handover.log', 'a', encoding='utf-8') as f:
            f.write(time.strftime('%Y-%m-%d %H:%M:%S  ') + line + '\n')
    except OSError:
        pass


def read_settings(root):
    """NX2navi's data/launcher.json as a dict ({} when it is not there or not JSON). Only read, never written."""
    try:
        data = json.loads((Path(root) / 'data' / 'launcher.json').read_text(encoding='utf-8-sig'))
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def nx_game(settings, root):
    """The NX2navi game built on this PC, as the path of its ocarina.exe, or None: the folder the settings name, else
    the launcher's own game folder."""
    for folder in (settings.get('game_dir'), Path(root) / 'game'):
        try:
            if folder and (Path(folder) / 'ocarina.exe').is_file():
                return Path(folder) / 'ocarina.exe'
        except (OSError, TypeError, ValueError):
            pass
    return None


def facts(root):
    """What the window shows and acts on, read from the PC."""
    settings = read_settings(root)
    game = nx_game(settings, root)
    dump = settings.get('dump') if isinstance(settings.get('dump'), str) else ''
    if dump and not os.path.exists(dump):
        dump = ''
    return {'triforce': triforce_installed(), 'game': game, 'dump': os.path.normpath(dump) if dump else '',
            'game_dir': str(game.parent) if game else ''}


# ---------------------------------------------------------------- Triforce Setup

def triforce_installed():
    """The installed Triforce Setup (Triforce's launcher), or None: the folder its entry in Windows' Apps list names
    (HKCU ...\\Uninstall\\Triforce, InstallLocation), else its default folder."""
    folders = []
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, TRIFORCE_UNINSTALL_KEY) as k:
            folders.append(winreg.QueryValueEx(k, 'InstallLocation')[0])
    except (OSError, ImportError):
        pass
    if os.environ.get('LOCALAPPDATA'):
        folders.append(os.path.join(os.environ['LOCALAPPDATA'], 'Programs', 'Triforce'))
    for f in folders:
        try:
            exe = Path(f) / 'TriforceSetup.exe'
            if exe.is_file():
                return exe
        except (OSError, TypeError, ValueError):
            pass
    return None


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def bundled_setup():
    """(path, info) of the Triforce Setup this release carries, after checking it against setup.json (its size and
    sha256, written when the release was made). OSError or ValueError when it is missing or is not that file."""
    info = json.loads((SETUP_DIR / 'setup.json').read_text(encoding='utf-8'))
    exe = SETUP_DIR / info['name']
    if exe.stat().st_size != info['size'] or sha256_file(exe).lower() != info['sha256'].lower():
        raise ValueError(f'{exe.name} is not the file this release was made with')
    return exe, info


def start_detached(cmd, cwd):
    flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP if os.name == 'nt' else 0
    return subprocess.Popen(cmd, cwd=str(cwd), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, close_fds=True, creationflags=flags)


def start_triforce(installed):
    """Start Triforce's launcher: the installed one, else this release's copy from a folder of its own in the temp
    folder (Setup then installs itself, as it does for anyone who downloads it). Returns what was started."""
    if installed:
        start_detached([str(installed)], Path(installed).parent)
        return str(installed)
    src, info = bundled_setup()
    folder = Path(tempfile.gettempdir()) / 'TriforceSetup'
    folder.mkdir(parents=True, exist_ok=True)
    exe = folder / info['name']
    try:
        same = exe.is_file() and exe.stat().st_size == info['size'] and sha256_file(exe).lower() == info['sha256'].lower()
    except OSError:
        same = False
    if not same:
        shutil.copyfile(src, exe)
    start_detached([str(exe)], folder)
    return str(exe)


def start_game(exe):
    """Start the NX2navi game from its own folder, as NX2navi's launcher did (its console stays hidden)."""
    exe = Path(exe)
    flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP if os.name == 'nt' else 0
    return subprocess.Popen([str(exe)], cwd=str(exe.parent), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, close_fds=True, creationflags=flags)


def copy_to_clipboard(text):
    """Windows' clipboard, set through Win32 so that it outlives this window. False when that did not work."""
    if os.name != 'nt':
        return False
    try:
        from ctypes import wintypes
        u, k = ctypes.WinDLL('user32'), ctypes.WinDLL('kernel32')
        k.GlobalAlloc.restype, k.GlobalAlloc.argtypes = wintypes.HGLOBAL, (wintypes.UINT, ctypes.c_size_t)
        k.GlobalLock.restype, k.GlobalLock.argtypes = ctypes.c_void_p, (wintypes.HGLOBAL,)
        k.GlobalUnlock.argtypes = (wintypes.HGLOBAL,)
        u.OpenClipboard.argtypes = (wintypes.HWND,)
        u.SetClipboardData.restype, u.SetClipboardData.argtypes = wintypes.HANDLE, (wintypes.UINT, wintypes.HANDLE)
        data = ctypes.create_unicode_buffer(text)
        if not u.OpenClipboard(None):
            return False
        try:
            u.EmptyClipboard()
            h = k.GlobalAlloc(0x0002, ctypes.sizeof(data))   # GMEM_MOVEABLE
            p = k.GlobalLock(h)
            ctypes.memmove(p, data, ctypes.sizeof(data))
            k.GlobalUnlock(h)
            return bool(u.SetClipboardData(13, h))            # CF_UNICODETEXT
        finally:
            u.CloseClipboard()
    except (AttributeError, OSError):
        return False


# ---------------------------------------------------------------- the way back: a newer release of the old feed

class UpdateError(Exception):
    pass


_VERSION = re.compile(r'^\s*v?(\d+(?:\.\d+)*)(?:-([0-9A-Za-z.-]+))?(?:\+([0-9A-Za-z.-]+))?\s*$')


def parse_version(text):
    """As NX2navi's launcher orders versions (its launcher/versions.py): a pre-release sorts before its release."""
    m = _VERSION.match(text or '')
    if not m:
        raise ValueError(f'not a version: {text!r}')
    nums = tuple(int(x) for x in m.group(1).split('.'))
    nums = nums + (0,) * (4 - len(nums)) if len(nums) < 4 else nums
    return nums, (1,) if m.group(2) is None else (0, m.group(2))


def newer(a, b):
    return parse_version(a) > parse_version(b)


def display(version):
    return (version or '').split('+', 1)[0].strip()


def _is_local(url):
    return urllib.parse.urlsplit(url).hostname in ('127.0.0.1', 'localhost', '::1')


def _allowed(url):
    """https, or plain http to this PC (tests)."""
    scheme = urllib.parse.urlsplit(url).scheme
    return scheme == 'https' or (scheme == 'http' and _is_local(url))


class _NoDowngrade(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not _allowed(newurl):
            raise UpdateError(f'redirected to {newurl}, which is not https')
        return super().redirect_request(req, fp, code, msg, headers, newurl)


_opener = None


def _open(url, headers=None, timeout=30):
    global _opener
    if not _allowed(url):
        raise UpdateError(f'not an https address: {url}')
    if _opener is None:
        try:   # Windows' own certificate check, when the launcher's Python brings the package for it
            import truststore
            truststore.inject_into_ssl()
        except Exception:
            pass
        _opener = urllib.request.build_opener(_NoDowngrade)
    req = urllib.request.Request(url, headers={'User-Agent': f'NX2navi-handover/{my_version()}', **(headers or {})})
    return _opener.open(req, timeout=timeout)


def get_json(url, headers=None):
    with _open(url, headers) as r:
        return json.loads(r.read(8 << 20).decode('utf-8-sig'))


def load_feed(root, settings):
    """Where NX2navi's launcher looked for releases: the feed.json of the version before this one (or of any other
    version in app/), with the settings' feed_repo over it, as there."""
    feed = dict(DEFAULT_FEED)
    apps = Path(root) / 'app'
    names = [read_text(apps / 'previous.txt')]
    try:
        names += sorted(d.name for d in apps.iterdir() if d.is_dir())
    except OSError:
        pass
    for name in names:
        try:
            if name and name != APP.name:
                got = json.loads((apps / name / 'feed.json').read_text(encoding='utf-8-sig'))
                if isinstance(got, dict) and got.get('type') in ('github', 'url'):
                    feed.update(got)
                    break
        except (OSError, ValueError):
            continue
    own = APP / 'feed.json'   # a release of this window may name its feed itself
    try:
        got = json.loads(own.read_text(encoding='utf-8-sig'))
        if isinstance(got, dict) and got.get('type') in ('github', 'url'):
            feed = got
    except (OSError, ValueError):
        pass
    if isinstance(settings.get('feed_repo'), str) and settings.get('feed_repo'):
        feed = {'type': 'github', 'repo': settings['feed_repo']}
    return feed


def _check_manifest(m, where):
    if not isinstance(m, dict) or m.get('format') != 1 or 'version' not in m or not isinstance(m.get('app'), dict):
        raise UpdateError(f'{where}: not an NX2navi update manifest')
    parse_version(m['version'])
    return m


def latest(feed, channel='stable'):
    """(manifest, {file name: address}) of the newest release the feed offers on the channel, or None."""
    if feed.get('type') == 'url':
        url = feed['url']
        m = _check_manifest(get_json(url), url)
        base = url.rsplit('/', 1)[0] + '/'
        return m, {e['name']: urllib.parse.urljoin(base, e['name']) for e in (m.get('app'), m.get('runtime'))
                   if isinstance(e, dict) and isinstance(e.get('name'), str)}
    api = str(feed.get('api', GITHUB_API)).rstrip('/')
    headers = {'Accept': 'application/vnd.github+json', 'X-GitHub-Api-Version': '2022-11-28'}
    for r in get_json(f'{api}/repos/{feed["repo"]}/releases?per_page=20', headers):
        if r.get('draft') or (r.get('prerelease') and channel != 'preview'):
            continue
        assets = {a['name']: a['browser_download_url'] for a in r.get('assets', [])}
        if MANIFEST not in assets:
            continue
        with _open(assets[MANIFEST]) as f:
            m = json.loads(f.read(8 << 20).decode('utf-8-sig'))
        return _check_manifest(m, f'{r.get("tag_name")}/{MANIFEST}'), assets
    return None


def _entry(manifest, key, assets):
    """(name, address, size, sha256) of the manifest's 'app' or 'runtime' file; refused without a plain file name, a
    size and a sha256, as NX2navi's launcher refuses it."""
    e = manifest.get(key) or {}
    name, size, sha = e.get('name'), e.get('size'), e.get('sha256')
    if (not isinstance(name, str) or not name or name in ('.', '..') or name != Path(name).name
            or '/' in name or '\\' in name or ':' in name):
        raise UpdateError(f'the manifest names its {key} file {name!r}, not a plain file name')
    if (not isinstance(size, int) or isinstance(size, bool) or size <= 0 or not isinstance(sha, str)
            or not re.fullmatch(r'[0-9a-fA-F]{64}', sha)):
        raise UpdateError(f'the manifest gives no size and sha256 for {name}')
    if not assets.get(name):
        raise UpdateError(f'the release lacks {name}')
    return name, assets[name], size, sha.lower()


def download(url, dest, size, sha):
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + '.handover-part')
    h, got = hashlib.sha256(), 0
    try:
        with _open(url, timeout=60) as r, open(part, 'wb') as f:
            for block in iter(lambda: r.read(1 << 18), b''):
                got += len(block)
                if got > size:
                    raise UpdateError(f'{dest.name} is larger than the manifest says')
                h.update(block)
                f.write(block)
        if got != size or h.hexdigest() != sha:
            raise UpdateError(f'{dest.name} does not match the manifest (size or sha256)')
        os.replace(part, dest)
    finally:
        try:
            part.unlink()
        except OSError:
            pass
    return dest


def safe_extract(zip_path, dest):
    """Unpack a zip into dest, refusing absolute names and names that climb out of it."""
    dest = Path(dest)
    root = dest.resolve()
    with zipfile.ZipFile(zip_path) as z:
        for info in z.infolist():
            name = info.filename.replace('\\', '/')
            if name.startswith('/') or ':' in name.split('/')[0] or '..' in name.split('/'):
                raise UpdateError(f'{Path(zip_path).name}: unsafe path {info.filename}')
            target = (dest / name).resolve()
            if target != root and root not in target.parents:
                raise UpdateError(f'{Path(zip_path).name}: unsafe path {info.filename}')
        z.extractall(dest)


def stage(root, manifest, assets):
    """Make the release the version the stub starts next, as NX2navi's launcher did (its launcher/updater.py): the app
    archive, checked, unpacks into app/<version>; a Python the release needs unpacks into python.next, which the stub
    swaps in at its next start; then previous.txt names this version and current.txt the new one. Returns the
    version. Nothing is switched when any step fails."""
    root = Path(root)
    apps, downloads = root / 'app', root / 'data' / 'downloads'
    ver = display(manifest['version'])
    name, url, size, sha = _entry(manifest, 'app', assets)
    archive = download(url, downloads / name, size, sha)
    part, final = apps / f'{ver}.part', apps / ver
    shutil.rmtree(part, ignore_errors=True)
    part.mkdir(parents=True)
    try:
        safe_extract(archive, part)
        if display(read_text(part / 'VERSION')) != ver or not (part / 'launcher' / 'start.pyw').is_file():
            raise UpdateError(f'{name} does not hold version {ver} with a launcher')
        if final.exists():
            if final.resolve() == APP.resolve():
                raise UpdateError(f'{ver} is the version that runs now')
            shutil.rmtree(final)
        os.replace(part, final)
    except BaseException:
        shutil.rmtree(part, ignore_errors=True)
        raise
    finally:
        try:
            archive.unlink()
        except OSError:
            pass
    rt = manifest.get('runtime')
    if isinstance(rt, dict) and rt.get('id') and rt.get('id') != read_text(root / 'python' / 'runtime-id.txt'):
        name, url, size, sha = _entry(manifest, 'runtime', assets)
        archive = download(url, downloads / name, size, sha)
        nxt = root / 'python.next'
        part = root / 'python.next.part'
        shutil.rmtree(nxt, ignore_errors=True)
        shutil.rmtree(part, ignore_errors=True)
        part.mkdir()
        try:
            safe_extract(archive, part)
            if not (part / 'python.exe').is_file() or read_text(part / 'runtime-id.txt') != rt['id']:
                raise UpdateError(f'{name} is not the runtime {rt["id"]}')
            os.replace(part, nxt)
        except BaseException:
            shutil.rmtree(part, ignore_errors=True)
            shutil.rmtree(final, ignore_errors=True)
            raise
        finally:
            try:
                archive.unlink()
            except OSError:
                pass
    write_text(apps / 'previous.txt', APP.name)
    write_text(apps / 'current.txt', ver)
    return ver


def check_for_newer(root, settings=None, force=False):
    """Ask the feed for a version newer than this one and stage it. Returns the staged version, or '' when there is
    none, the player turned update checks off, or the last check was less than an hour ago. Raises on a failure."""
    root = Path(root)
    settings = read_settings(root) if settings is None else settings
    if not force:
        if settings.get('auto_check') is False:
            return ''
        state = root / 'data' / 'handover.json'
        try:
            last = float(json.loads(state.read_text(encoding='utf-8')).get('last_check', 0))
        except (OSError, ValueError, TypeError, AttributeError):
            last = 0.0
        if 0 <= time.time() - last < CHECK_EVERY:
            return ''
        try:
            state.parent.mkdir(parents=True, exist_ok=True)
            write_text(state, json.dumps({'last_check': time.time()}))
        except OSError:
            pass
    channel = settings.get('channel') if settings.get('channel') in ('stable', 'preview') else 'stable'
    found = latest(load_feed(root, settings), channel)
    if not found or not newer(found[0]['version'], my_version()):
        return ''
    return stage(root, *found)


def check_in_background(root):
    def work():
        try:
            ver = check_for_newer(root)
            if ver:
                log(root, f'a newer release, {ver}, is staged: NX2navi.exe starts it next time')
        except Exception as e:   # no network, a refused request, a damaged file: tried again at a later start
            log(root, f'update check: {type(e).__name__}: {e}')
    t = threading.Thread(target=work, name='handover-update', daemon=True)
    t.start()
    return t


# ---------------------------------------------------------------- uninstalling NX2navi

def message_box(text, title='NX2navi', flags=0x40):
    try:
        return ctypes.windll.user32.MessageBoxW(None, text, title, flags)
    except (AttributeError, OSError):
        print(text, file=sys.stderr)
        return 0


def ansi_safe(path):
    try:
        str(path).encode('mbcs', errors='strict')
        return True
    except (UnicodeEncodeError, LookupError):
        return False


def uninstall_plan(root, remove_game=''):
    """(paths to delete with all they hold, folders to delete only when that left them empty), as NX2navi's launcher
    planned it (its launcher/winsys.py uninstall_plan)."""
    root = Path(root)
    gone = [Path(remove_game)] if remove_game else []
    gone += [root / n for n in OWNED]
    try:
        gone += sorted(root.glob('python.old-*'))
    except OSError:
        pass
    empty = [root]
    if not ansi_safe(root):   # the build tools then went to <system drive>\NX2navi\toolchain
        tools = Path(os.environ.get('SystemDrive', 'C:') + '\\') / 'NX2navi' / 'toolchain'
        gone.append(tools)
        empty.append(tools.parent)
    return gone, empty


def uninstall_script(gone, empty):
    """The cmd.exe line that deletes them once this process has exited. A path holding % is left out: cmd.exe expands
    %NAME% even inside quotes."""
    steps = ['ping -n 3 127.0.0.1 >nul']
    for p in gone:
        if '%' not in str(p):
            steps.append(f'(if exist "{p}\\*" (rmdir /s /q "{p}") else if exist "{p}" (del /f /q "{p}"))')
    steps += [f'rmdir "{p}" 2>nul' for p in empty if '%' not in str(p)]
    return ' & '.join(steps)


def uninstall(root):
    """Windows' Apps list > NX2navi > Uninstall (NX2navi.exe --uninstall): asks, then removes the shortcuts, the Apps
    entry and what the launcher put in its folder. The built game goes only when the player says so in a second
    question; the game dump is never touched. Triforce is not touched either."""
    root = Path(root)
    if message_box('Remove NX2navi from this PC?\n\nIts launcher, its build tools and its shortcuts are removed. '
                   'Your game dump is never touched, and neither is Triforce.', 'Uninstall NX2navi', 0x24) != 6:
        return 0
    game = nx_game(read_settings(root), root)
    remove_game = ''
    if game and len(game.parent.parts) > 1:
        if message_box(f'Also remove the NX2navi game in\n{game.parent}?\n\nYour NX2navi saves are in that folder. '
                       'Choose No to keep it, for instance until Triforce has taken your saves over.',
                       'Uninstall NX2navi', 0x124) == 6:   # Yes / No, No preselected
            remove_game = str(game.parent)
    log(root, f'uninstall (game folder too: {bool(remove_game)})')
    stub = root / 'NX2navi.exe'
    if stub.is_file():
        try:
            subprocess.run([str(stub), '--shortcuts', 'off'], timeout=30, capture_output=True,
                           creationflags=CREATE_NO_WINDOW)
        except (OSError, subprocess.TimeoutExpired):
            pass
    try:
        import winreg
        winreg.DeleteKey(winreg.HKEY_CURRENT_USER, NX_UNINSTALL_KEY)
    except (OSError, ImportError):
        pass
    gone, empty = uninstall_plan(root, remove_game)
    subprocess.Popen(f'cmd.exe /c "{uninstall_script(gone, empty)}"', cwd=os.environ.get('TEMP', 'C:\\'),
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True,
                     creationflags=DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP)
    return 0


# ---------------------------------------------------------------- the window

W, H = 820, 480            # the composition, in units; tools/make_art.py draws the hall for it
SCALES = (1.0, 1.25, 1.5, 2.0)
TEXT_HI, TEXT, TEXT_DIM, TEXT_FAINT = '#F3EAD5', '#CDC2A8', '#948A70', '#5F5744'
GOLD, GOLD_300, LINE, LINE_GOLD = '#E0B23E', '#F7DC8C', '#2C2518', '#4A3C1B'
BAD = '#FF6B76'
HAIR = '\u200a'


def dpi_aware():
    if os.name != 'nt':
        return
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except (AttributeError, OSError):
            pass


def caps(text, gaps=1):
    """Letter-spaced capitals: hair spaces between the letters, as the Triforce client sets its labels."""
    return (HAIR * gaps).join(text.upper())


class App:
    def __init__(self, tk_root, root, found, argv=(), offscreen=False):
        import tkinter as tk
        import tkinter.font as tkfont
        self.tk, self.tkfont = tk, tkfont
        self.win, self.root, self.found, self.argv = tk_root, Path(root), dict(found), list(argv)
        self.status, self.status_color = '', GOLD_300
        self.game_proc = None
        self.closing = False
        dpi = self.win.winfo_fpixels('1i') / 96.0
        self.s = max([s for s in SCALES if s <= dpi + 0.01] or [1.0])
        tag = str(round(self.s * 100))
        self.img = {n: tk.PhotoImage(master=self.win, file=str(ART / f'{n}_{tag}.png')) for n in ('hall', 'btn')}
        self.img['btn_hi'] = tk.PhotoImage(master=self.win, file=str(ART / f'btn_{tag}_hi.png'))
        self.icons = [tk.PhotoImage(master=self.win, file=str(ART / f'icon_{n}.png')) for n in (64, 32, 16)]
        families = set(tkfont.families(self.win))
        serif = next((f for f in ('Palatino Linotype', 'Book Antiqua', 'Georgia') if f in families), 'Times New Roman')
        sans = next((f for f in ('Segoe UI', 'Tahoma') if f in families), 'Arial')
        semi = 'Segoe UI Semibold' if 'Segoe UI Semibold' in families else sans

        def font(family, px, weight='normal'):
            return tkfont.Font(root=self.win, family=family, size=-max(8, round(px * self.s)), weight=weight)
        self.f_title = font(serif, 31)
        self.f_body = font(sans, 14)
        self.f_small = font(sans, 12.5)
        self.f_label = font(semi, 10.5, 'normal' if semi != sans else 'bold')
        self.f_button = font(serif, 13, 'bold')
        self.f_second = font(sans, 13)

        w, h = round(W * self.s), round(H * self.s)
        self.win.title('Triforce')
        self.win.resizable(False, False)
        self.win.configure(bg='#0A0908')
        try:
            self.win.iconphoto(True, *self.icons)
        except tk.TclError:
            pass
        x = -32000 if offscreen else max(0, (self.win.winfo_screenwidth() - w) // 2)
        y = -32000 if offscreen else max(0, (self.win.winfo_screenheight() - h) // 2 - round(20 * self.s))
        self.win.geometry(f'{w}x{h}+{x}+{y}')
        self.c = tk.Canvas(self.win, width=w, height=h, highlightthickness=0, bd=0, bg='#0A0908')
        self.c.pack()
        self.win.protocol('WM_DELETE_WINDOW', self.close)
        self.win.bind('<Return>', lambda e: self.primary())
        self.win.bind('<Escape>', lambda e: self.close())
        self.draw()
        self.win.after(60, self.dress)

    # ---- drawing

    def u(self, v):
        return round(v * self.s)

    def dress(self):
        """The title bar in the hall's colours (Windows 11; Windows 10 takes the dark frame)."""
        if os.name != 'nt':
            return
        try:
            user32, dwm = ctypes.windll.user32, ctypes.windll.dwmapi
            user32.GetParent.restype, user32.GetParent.argtypes = ctypes.c_void_p, [ctypes.c_void_p]
            hwnd = ctypes.c_void_p(user32.GetParent(self.win.winfo_id()) or self.win.winfo_id())
            for attr, value in ((20, 1), (19, 1), (35, 0x00090A0B), (36, 0x003EB2E0), (34, 0x001E546E)):
                v = ctypes.c_int(value)
                dwm.DwmSetWindowAttribute(hwnd, attr, ctypes.byref(v), ctypes.sizeof(v))
        except (AttributeError, OSError):
            pass

    def wrap(self, text, font, width):
        lines, line = [], ''
        for word in text.split():
            trial = (line + ' ' + word).strip()
            if line and font.measure(trial) > width:
                lines.append(line)
                line = word
            else:
                line = trial
        return lines + [line] if line else lines

    def fit(self, text, font, width):
        """A path shortened in its middle until it fits."""
        if font.measure(text) <= width:
            return text
        head, tail = text[:len(text) // 2], text[len(text) // 2:]
        while head and tail and font.measure(head + ' … ' + tail) > width:
            if len(head) >= len(tail):
                head = head[:-1]
            else:
                tail = tail[1:]
        return head + ' … ' + tail

    def link(self, tag, on, rest, hot):
        self.c.tag_bind(tag, '<Enter>', lambda e: (self.c.itemconfigure(tag, fill=hot), self.c.configure(cursor='hand2')))
        self.c.tag_bind(tag, '<Leave>', lambda e: (self.c.itemconfigure(tag, fill=rest), self.c.configure(cursor='')))
        self.c.tag_bind(tag, '<Button-1>', lambda e: on())

    def draw(self):
        c, u = self.c, self.u
        c.delete('all')
        c.configure(cursor='')
        c.create_image(0, 0, anchor='nw', image=self.img['hall'])
        left, col = u(52), u(348)
        y = u(54)
        # the eyebrow: a short rule, then the label
        c.create_line(left, y + u(6), left + u(20), y + u(6), fill=LINE_GOLD)
        c.create_text(left + u(30), y, anchor='nw', text=caps('NX2navi  ×  Triforce'), font=self.f_label, fill=GOLD)
        y += u(28)
        c.create_text(left - u(1), y, anchor='nw', text='NX2navi is now Triforce', font=self.f_title, fill=TEXT_HI)
        y += u(52)
        body = ('The two projects have merged and go on under one name. Triforce is your launcher from here: it '
                'builds the game from your own files, as NX2navi did, and its Convert saves page brings your '
                'NX2navi saves over.')
        for line in self.wrap(body, self.f_body, col):
            c.create_text(left, y, anchor='nw', text=line, font=self.f_body, fill=TEXT)
            y += u(22)
        y += u(14)
        rows = []
        if self.found.get('dump'):
            rows.append(('Your game files', self.found['dump'], 'copy_dump'))
        if self.found.get('game_dir'):
            rows.append(('Your NX2navi game and saves', self.found['game_dir'], 'copy_game'))
        for label, path, tag in rows:
            c.create_text(left, y, anchor='nw', text=caps(label), font=self.f_label, fill=TEXT_DIM)
            copy_w = self.f_small.measure('Copy path')
            c.create_text(left, y + u(17), anchor='nw', text=self.fit(path, self.f_small, col - copy_w - u(18)),
                          font=self.f_small, fill=TEXT)
            c.create_text(left + col, y + u(17), anchor='ne', text='Copy path', font=self.f_small, fill=TEXT_DIM,
                          tags=(tag,))
            self.link(tag, lambda p=path: self.copy(p), TEXT_DIM, GOLD_300)
            y += u(44)
        # the actions, on a fixed line near the foot
        by = u(370)
        if self.status:   # (two lines at most, between the rows above, which end at 316, and the buttons)
            for i, line in enumerate(self.wrap(self.status, self.f_small, col)[:2]):
                c.create_text(left, by - u(44) + i * u(18), anchor='nw', text=line, font=self.f_small,
                              fill=self.status_color)
        bw, bh = u(196), u(44)
        c.create_image(left, by, anchor='nw', image=self.img['btn'], tags=('primary', 'primary_bar'))
        label = 'Open Triforce' if self.found.get('triforce') else 'Get Triforce'
        c.create_text(left + bw // 2, by + bh // 2 + u(1), text=caps(label, 2), font=self.f_button, fill='#F9E7A8',
                      tags=('primary',))
        c.create_text(left + bw // 2, by + bh // 2, text=caps(label, 2), font=self.f_button, fill='#2A1C05',
                      tags=('primary',))
        c.tag_bind('primary', '<Enter>', lambda e: (c.itemconfigure('primary_bar', image=self.img['btn_hi']),
                                                    c.configure(cursor='hand2')))
        c.tag_bind('primary', '<Leave>', lambda e: (c.itemconfigure('primary_bar', image=self.img['btn']),
                                                    c.configure(cursor='')))
        c.tag_bind('primary', '<Button-1>', lambda e: self.primary())
        if self.found.get('game'):
            sx, sw, sh = left + bw + u(14), u(138), u(36)
            sy = by + (bh - sh) // 2
            c.create_rectangle(sx, sy, sx + sw, sy + sh, outline=LINE, fill='#0C0B09', tags=('second', 'second_box'))
            c.create_text(sx + sw // 2, sy + sh // 2, text='Play NX2navi', font=self.f_second, fill=TEXT,
                          tags=('second', 'second_text'))
            c.tag_bind('second', '<Enter>', lambda e: (c.itemconfigure('second_box', outline='#8C7230'),
                                                       c.itemconfigure('second_text', fill=TEXT_HI),
                                                       c.configure(cursor='hand2')))
            c.tag_bind('second', '<Leave>', lambda e: (c.itemconfigure('second_box', outline=LINE),
                                                       c.itemconfigure('second_text', fill=TEXT),
                                                       c.configure(cursor='')))
            c.tag_bind('second', '<Button-1>', lambda e: self.play())
        # the foot: what stays, and where Triforce lives
        fy = u(H - 36)
        c.create_line(left, fy - u(12), left + col, fy - u(12), fill=LINE)
        c.create_text(left, fy, anchor='nw', font=self.f_small, fill=TEXT_DIM,
                      text='Nothing of NX2navi is removed or changed.')
        # (under the crest, centred on its column: tools/make_art.py CX)
        site_w, dot_w, disc_w = (self.f_small.measure(t) for t in ('triforceproject.com', '·', 'Discord'))
        gap = u(12)
        sx = u(632) - (site_w + dot_w + disc_w + 2 * gap) // 2
        c.create_text(sx, fy, anchor='nw', text='triforceproject.com', font=self.f_small, fill=TEXT_DIM, tags=('site',))
        self.link('site', lambda: self.browse(SITE), TEXT_DIM, GOLD_300)
        c.create_text(sx + site_w + gap, fy, anchor='nw', text='·', font=self.f_small, fill=TEXT_FAINT)
        c.create_text(sx + site_w + dot_w + 2 * gap, fy, anchor='nw', text='Discord', font=self.f_small,
                      fill=TEXT_DIM, tags=('discord',))
        self.link('discord', lambda: self.browse(DISCORD), TEXT_DIM, GOLD_300)

    # ---- actions

    def say(self, text, bad=False):
        self.status, self.status_color = text, BAD if bad else GOLD_300
        self.draw()

    def copy(self, path):
        self.say('Copied. Paste it where Triforce Setup asks for it.' if copy_to_clipboard(path)
                 else 'The path could not be copied.', bad=False)

    def browse(self, url):
        try:
            webbrowser.open(url)
        except Exception:
            self.say(url)

    def primary(self):
        if self.closing:
            return
        try:
            started = start_triforce(self.found.get('triforce'))
        except Exception as e:
            log(self.root, f'Triforce Setup could not be started: {type(e).__name__}: {e}')
            self.say('Triforce Setup could not be started from here. Get it from triforceproject.com, which opens '
                     'now.', bad=True)
            self.browse(SITE)
            return
        log(self.root, f'started {started}')
        self.closing = True
        self.say('Triforce is opening.')
        self.win.after(700, self.win.destroy)

    def play(self):
        if self.closing or not self.found.get('game'):
            return
        try:
            self.game_proc = start_game(self.found['game'])
        except OSError as e:
            log(self.root, f'{self.found["game"]}: {e}')
            self.say(f'The NX2navi game could not be started: {e}', bad=True)
            return
        log(self.root, f'started {self.found["game"]}')
        self.closing = True
        self.win.withdraw()
        self.wait_game()

    def wait_game(self):
        """Stay (hidden) while the game runs, as the old launcher did: whoever started this window (Steam, with
        --play) sees the game as running for that time."""
        if self.game_proc is not None and self.game_proc.poll() is None:
            self.win.after(1000, self.wait_game)
        else:
            self.win.destroy()

    def close(self):
        self.win.destroy()


def main(argv=None):
    """The window. Returns the process's exit code; raises when the window cannot be made (start.pyw then lets the
    stub go back to the version before)."""
    argv = list(sys.argv[1:] if argv is None else argv)
    root = launcher_root()
    if '--uninstall' in argv:
        return uninstall(root)
    selftest = os.environ.get('NX2NAVI_HANDOVER_TEST') or ('--selftest' in argv and '.') or ''
    if '--shots' in argv and argv.index('--shots') + 1 < len(argv):
        selftest = argv[argv.index('--shots') + 1]
    dpi_aware()
    try:
        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID('Triforce.NX2naviHandover')
    except (AttributeError, OSError):
        pass
    import tkinter as tk
    win = tk.Tk(className='Triforce')
    app = App(win, root, facts(root), argv, offscreen=bool(selftest))
    log(root, f'window up: version {my_version()}, arguments {argv}, Triforce installed: '
              f'{bool(app.found["triforce"])}, NX2navi game: {bool(app.found["game"])}')
    if selftest:
        import selftest as st
        win.after(300, lambda: st.run(app, selftest))
    else:
        check_in_background(root)
    win.mainloop()
    return 0
