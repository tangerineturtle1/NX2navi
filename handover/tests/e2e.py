"""The handover against NX2navi's real launcher, on a copy, without a window on the screen.

    python tests/e2e.py [--nx2navi DIR] [--work DIR]

DIR is an installed NX2navi launcher (default %LOCALAPPDATA%\\Programs\\NX2navi). It is only read: its stub, its
Python and the launcher code of each version in app/ are copied into a work folder, where the launcher's own code
then finds this handover in a feed on 127.0.0.1 and installs it, and its own stub starts it.

 A  each launcher version's command line (`cli.py check`, `cli.py update`): sees the handover, downloads, checks,
    stages and switches to it, and the handover's cli.py ends the update with exit code 0
 B  the stub (NX2navi.exe) starts the handover: it is still up when the stub stops waiting, nothing is rolled back
 C  a handover that cannot start: exit code 86 inside the stub's wait, and the stub goes back to the version before
 D  the path the launcher's window takes in the background (feed.latest, updater.stage, staged_info, install_staged)
 E  a GitHub-shaped feed, Stable channel: found as a full release, passed over as a pre-release
 F  the way back: the handover stages a newer release of the feed, as the launcher did
 G  the handover's own parts: what it finds on the PC, the Setup it carries and starts, the uninstall plan against
    the launcher's own
"""
import argparse
import functools
import http.server
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent
FAILED = []


def check(name, ok, detail=''):
    print(('  ok    ' if ok else '  FAIL  ') + name + (f': {detail}' if detail and not ok else ''), flush=True)
    if not ok:
        FAILED.append(name)


class Handler(http.server.SimpleHTTPRequestHandler):
    releases = []

    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.startswith('/api/repos/'):
            body = json.dumps(self.releases).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()


def serve(folder):
    srv = http.server.ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(Handler, directory=str(folder)))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, f'http://127.0.0.1:{srv.server_address[1]}'


def make_release(version, out):
    r = subprocess.run([sys.executable, '-B', str(PROJECT / 'make_handover.py'), '--version', version, '--out', str(out)],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stdout + r.stderr)


def clean_env(**more):
    env = {k: v for k, v in os.environ.items() if not k.startswith('NX2NAVI_')}
    env.update(more)
    return env


def make_root(work, nx, name, version, feed):
    """A launcher folder like the installed one, holding only that version of the launcher."""
    root = work / name
    shutil.rmtree(root, ignore_errors=True)
    (root / 'app' / version).mkdir(parents=True)
    shutil.copy2(nx / 'NX2navi.exe', root / 'NX2navi.exe')
    if not (work / 'python').is_dir():
        shutil.copytree(nx / 'python', work / 'python')
    # (a junction to the one copy: the stub wants python\ beside it, and 100 MB need not be copied per test)
    subprocess.run(['cmd', '/c', 'mklink', '/J', str(root / 'python'), str(work / 'python')], capture_output=True)
    src = nx / 'app' / version
    for part in ('launcher', 'tools'):
        shutil.copytree(src / part, root / 'app' / version / part, ignore=shutil.ignore_patterns('__pycache__'))
    for f in ('VERSION', 'toolchain.lock.json'):
        if (src / f).is_file():
            shutil.copy2(src / f, root / 'app' / version / f)
    (root / 'app' / version / 'feed.json').write_text(json.dumps(feed), encoding='utf-8')
    (root / 'app' / 'current.txt').write_text(version + '\n', encoding='utf-8')
    (root / 'data').mkdir()
    (root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': '', 'dump': '', 'channel': 'stable'}),
                                                 encoding='utf-8')
    return root


def text(path):
    try:
        return Path(path).read_text(encoding='utf-8-sig').strip()
    except OSError:
        return ''


def run_py(root, version, args, **kw):
    py = root / 'python' / 'python.exe'
    return subprocess.run([str(py), str(root / 'app' / version / 'launcher' / 'cli.py'), *args], capture_output=True,
                          text=True, cwd=str(root), env=clean_env(), timeout=300, **kw)


def load_handover(app_dir):
    sys.path.insert(0, str(app_dir / 'launcher'))
    spec = importlib.util.spec_from_file_location('handover', app_dir / 'launcher' / 'handover.py')
    mod = importlib.util.module_from_spec(spec)
    sys.modules['handover'] = mod
    spec.loader.exec_module(mod)
    return mod


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nx2navi', type=Path, default=Path(os.environ.get('LOCALAPPDATA', '')) / 'Programs' / 'NX2navi')
    ap.add_argument('--work', type=Path, default=Path(tempfile.gettempdir()) / 'nx2navi-handover-e2e')
    a = ap.parse_args()
    nx, work = a.nx2navi, a.work
    if not (nx / 'NX2navi.exe').is_file():
        sys.exit(f'{nx} is not an installed NX2navi launcher')
    work.mkdir(parents=True, exist_ok=True)
    versions = sorted(d.name for d in (nx / 'app').iterdir() if (d / 'launcher' / 'updater.py').is_file())
    print(f'NX2navi launcher versions on this PC: {", ".join(versions)}')
    NEW, NEWER = '0.12.0', '0.13.0'
    feed_dir, feed2_dir = work / 'feed', work / 'feed2'
    for d in (feed_dir, feed2_dir):
        shutil.rmtree(d, ignore_errors=True)
    make_release(NEW, feed_dir)
    make_release(NEWER, feed2_dir)
    srv, base = serve(feed_dir)
    srv2, base2 = serve(feed2_dir)
    url_feed = {'type': 'url', 'url': f'{base}/NX2navi-update.json'}

    for v in versions:
        print(f'\nA  the command line of launcher {v}')
        root = make_root(work, nx, f'root-a-{v}', v, url_feed)
        r = run_py(root, v, ['check'])
        check('check sees the handover', f'update available: {NEW}' in r.stdout, r.stdout + r.stderr)
        r = run_py(root, v, ['update'])
        check('update ends with exit code 0', r.returncode == 0, r.stdout[-600:] + r.stderr[-600:])
        check('the handover\'s cli.py finished it', 'NX2navi is now Triforce' in r.stdout, r.stdout[-400:])
        check('current.txt names the handover', text(root / 'app' / 'current.txt') == NEW)
        check('previous.txt names the old version', text(root / 'app' / 'previous.txt') == v)
        check('the old version is still on the disk', (root / 'app' / v / 'launcher' / 'start.pyw').is_file())
        check('the archive unpacked whole', all((root / 'app' / NEW / p).is_file() for p in (
            'VERSION', 'launcher/start.pyw', 'launcher/handover.py', 'launcher/triforce/TriforceSetup.exe',
            'launcher/art/hall_100.png')))

        print(f'B  the stub starts the handover (after launcher {v})')
        shots = work / f'shots-{v}'
        shutil.rmtree(shots, ignore_errors=True)
        t0 = time.time()
        r = subprocess.run([str(root / 'NX2navi.exe'), '--continue-update', v], cwd=str(root),
                           env=clean_env(NX2NAVI_HANDOVER_TEST=str(shots)), timeout=60)
        waited = time.time() - t0
        check('the stub ends with 0 after its six seconds', r.returncode == 0 and 5.5 < waited < 9, f'{r.returncode} after {waited:.1f} s')
        for _ in range(40):
            if (shots / 'state.json').is_file():
                break
            time.sleep(0.25)
        state = json.loads(text(shots / 'state.json') or '{}')
        check('the window came up with the stub\'s arguments', state.get('arguments') == ['--continue-update', v], str(state.get('arguments')))
        check('it knows its launcher folder', os.path.normcase(state.get('launcher_root', '')) == os.path.normcase(str(root)))
        check('its pictures were taken', bool(state.get('shots')) and all(v2 is True for v2 in state['shots'].values()), str(state.get('shots')))
        check('the Setup it carries is intact', state.get('setup', {}).get('ok') is True, str(state.get('setup')))
        time.sleep(3)
        check('nothing was rolled back', text(root / 'app' / 'current.txt') == NEW and text(root / 'app' / 'previous.txt') == v)
        check('no crash was logged', not (root / 'data' / 'logs' / 'crash.log').exists(), text(root / 'data' / 'logs' / 'crash.log')[-500:])

        print(f'C  a handover that cannot start (after launcher {v})')
        (root / 'app' / NEW / 'launcher' / 'handover.py').rename(root / 'app' / NEW / 'launcher' / 'handover.py.off')
        dummy = root / 'app' / '0.0.9' / 'launcher'
        dummy.mkdir(parents=True)
        (dummy / 'start.pyw').write_text(
            'import os, sys\nopen(os.path.join(os.path.dirname(__file__), "started.txt"), "w").write(" ".join(sys.argv[1:]))\n',
            encoding='utf-8')
        (root / 'app' / 'previous.txt').write_text('0.0.9\n', encoding='utf-8')
        t0 = time.time()
        r = subprocess.run([str(root / 'NX2navi.exe')], cwd=str(root), env=clean_env(), timeout=60)
        waited = time.time() - t0
        for _ in range(40):
            if (dummy / 'started.txt').is_file():
                break
            time.sleep(0.25)
        check('the stub went back to the version before', text(root / 'app' / 'current.txt') == '0.0.9', text(root / 'app' / 'current.txt'))
        check('and started it with --rolled-back', text(dummy / 'started.txt') == f'--rolled-back {NEW}', text(dummy / 'started.txt'))
        check('no message box held the exit code back', waited < 14, f'{waited:.1f} s')
        check('the failure is in crash.log', 'handover' in text(root / 'data' / 'logs' / 'crash.log'))

        print(f'D  the background path of launcher {v} (stage, then install when the game is not running)')
        root = make_root(work, nx, f'root-d-{v}', v, url_feed)
        script = work / 'stage_test.py'
        script.write_text(
            'import json, sys\n'
            'sys.path.insert(0, sys.argv[1])\n'
            'from launcher import feed, updater\n'
            'from launcher.paths import Layout\n'
            'from launcher.settings import Settings\n'
            'lay = Layout().ensure()\n'
            's = Settings(lay.settings)\n'
            'rel = feed.latest(feed.load(lay, s), s.get("channel", "stable"))\n'
            'updater.stage(lay, rel)\n'
            'ver = rel.version\n'
            'out = {"version": ver, "prerelease": updater.prerelease(rel), "staged": bool(updater.staged_info(lay, ver)),\n'
            '       "found": (updater.staged(lay, "stable") or {}).get("version"), "runtime_needed": updater.runtime_needed(lay, rel)}\n'
            'updater.install_staged(lay, ver)\n'
            'out["current"], out["previous"] = updater.current(lay), updater.previous(lay)\n'
            'print(json.dumps(out))\n', encoding='utf-8')
        r = subprocess.run([str(root / 'python' / 'python.exe'), str(script), str(root / 'app' / v)], capture_output=True,
                           text=True, cwd=str(root), env=clean_env(NX2NAVI_LAUNCHER_ROOT=str(root)), timeout=300)
        try:
            out = json.loads(r.stdout.strip().splitlines()[-1])
        except (ValueError, IndexError):
            out = {}
        check('staged complete and found again', out.get('staged') is True and out.get('found') == NEW, r.stdout[-300:] + r.stderr[-900:])
        check('a full release to the Stable channel, no Python to fetch', out.get('prerelease') is False and out.get('runtime_needed') is False, str(out))
        check('installed: current and previous switched', out.get('current') == NEW and out.get('previous') == v, str(out))

    v = versions[-1]
    print(f'\nE  a GitHub-shaped feed (launcher {v})')
    man = json.loads(text(feed_dir / 'NX2navi-update.json'))
    assets = [{'name': n, 'url': f'{base}/{n}', 'browser_download_url': f'{base}/{n}'}
              for n in ('NX2navi-update.json', man['app']['name'])]
    older = {'tag_name': 'v0.11.0-rc4', 'draft': False, 'prerelease': True, 'body': '', 'html_url': '', 'assets': []}
    gh_feed = {'type': 'github', 'repo': 'someone/NX2navi', 'api': f'{base}/api'}
    root = make_root(work, nx, 'root-e', v, gh_feed)
    Handler.releases = [{'tag_name': f'v{NEW}', 'draft': False, 'prerelease': False, 'body': 'notes', 'html_url': '', 'assets': assets}, older]
    r = run_py(root, v, ['check'])
    check('a full release is offered on Stable', f'update available: {NEW}' in r.stdout, r.stdout + r.stderr)
    Handler.releases = [{'tag_name': f'v{NEW}', 'draft': False, 'prerelease': True, 'body': 'notes', 'html_url': '', 'assets': assets}, older]
    r = run_py(root, v, ['check'])
    check('a pre-release is passed over on Stable', 'update available' not in r.stdout, r.stdout + r.stderr)

    print('\nF  the way back: the handover stages a newer release of the launcher\'s feed')
    root = make_root(work, nx, 'root-f', v, {'type': 'url', 'url': f'{base2}/NX2navi-update.json'})
    r = run_py(root, v, ['update'])
    # (that installed 0.13.0 from the second feed; set the folder up as a PC on 0.12.0 whose old version asks that feed)
    shutil.rmtree(root / 'app' / NEWER, ignore_errors=True)
    shutil.unpack_archive(str(feed_dir / f'NX2navi-app-{NEW}.zip'), str(root / 'app' / NEW), 'zip')
    (root / 'app' / 'current.txt').write_text(NEW + '\n', encoding='utf-8')
    (root / 'app' / 'previous.txt').write_text(v + '\n', encoding='utf-8')
    os.environ['NX2NAVI_LAUNCHER_ROOT'] = str(root)
    h = load_handover(root / 'app' / NEW)
    check('it reads the old version\'s feed', h.load_feed(root, {}).get('url', '').startswith(base2), str(h.load_feed(root, {})))
    found = h.latest(h.load_feed(root, {}), 'stable')
    check('it finds the newer release', bool(found) and found[0]['version'] == NEWER)
    try:
        staged = h.check_for_newer(root, force=True)
    except Exception as e:
        staged = f'{type(e).__name__}: {e}'
    check('it staged it', staged == NEWER, str(staged))
    check('current.txt names the newer release, previous.txt the handover',
          text(root / 'app' / 'current.txt') == NEWER and text(root / 'app' / 'previous.txt') == NEW)
    check('the newer release is whole', (root / 'app' / NEWER / 'launcher' / 'start.pyw').is_file() and text(root / 'app' / NEWER / 'VERSION') == NEWER)
    check('nothing newer than itself: nothing staged', h.check_for_newer(root, {'feed_repo': ''}, force=True) in ('', NEWER) and
          not h.newer(NEW, NEWER) and h.newer('0.12.0', '0.11.0-rc4') and h.newer('0.11.0', '0.11.0-rc4') and not h.newer('0.12.0-rc1', '0.12.0'))
    bad = dict(found[0], app=dict(found[0]['app'], sha256='0' * 64))
    (root / 'app' / 'current.txt').write_text(NEW + '\n', encoding='utf-8')
    shutil.rmtree(root / 'app' / NEWER, ignore_errors=True)
    try:
        h.stage(root, bad, found[1])
        refused = False
    except h.UpdateError:
        refused = True
    check('a file that does not match its sha256 is refused and nothing is switched',
          refused and text(root / 'app' / 'current.txt') == NEW and not (root / 'app' / NEWER).exists())

    print('\nG  the handover\'s own parts')
    game = work / 'fake game'
    shutil.rmtree(game, ignore_errors=True)
    game.mkdir()
    (game / 'ocarina.exe').write_bytes(b'MZ')
    dump = work / 'fake dump.nup'
    dump.write_bytes(b'x')
    (root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game), 'dump': str(dump).replace('\\', '/')}), encoding='utf-8')
    f = h.facts(root)
    check('it finds the NX2navi game and the dump from the launcher\'s settings',
          f['game'] == game / 'ocarina.exe' and f['game_dir'] == str(game) and f['dump'] == str(dump), str(f))
    started = []
    h.start_detached = lambda cmd, cwd: started.append((cmd, str(cwd)))
    temp = work / 'temp'
    shutil.rmtree(temp, ignore_errors=True)
    temp.mkdir()
    h.tempfile.gettempdir = lambda: str(temp)
    exe = h.start_triforce(None)
    info = json.loads(text(root / 'app' / NEW / 'launcher' / 'triforce' / 'setup.json'))
    check('Get Triforce starts the carried Setup from its own temp folder, checked',
          started and started[-1][0] == [exe] and Path(exe).parent == temp / 'TriforceSetup' and h.sha256_file(exe) == info['sha256'], str(started))
    h.start_triforce(Path(r'C:\somewhere\TriforceSetup.exe'))
    check('Open Triforce starts the installed one', started[-1][0] == [r'C:\somewhere\TriforceSetup.exe'])
    carried = root / 'app' / NEW / 'launcher' / 'triforce' / 'TriforceSetup.exe'
    carried.write_bytes(carried.read_bytes() + b'x')
    try:
        h.start_triforce(None)
        refused = False
    except (ValueError, OSError):
        refused = True
    check('a changed Setup is not started', refused and len(started) == 2)
    # the uninstall plan against the launcher's own, on the same folder
    script = work / 'plan_test.py'
    script.write_text(
        'import json, sys\n'
        'sys.path.insert(0, sys.argv[1])\n'
        'from launcher import winsys\n'
        'from launcher.paths import Layout\n'
        'lay = Layout()\n'
        'out = []\n'
        'for game in ("", sys.argv[2]):\n'
        '    gone, empty = winsys.uninstall_plan(lay, game)\n'
        '    out.append(winsys.uninstall_script(gone, empty))\n'
        'print(json.dumps(out))\n', encoding='utf-8')
    r = subprocess.run([str(root / 'python' / 'python.exe'), str(script), str(root / 'app' / v), str(game)], capture_output=True,
                       text=True, cwd=str(root), env=clean_env(NX2NAVI_LAUNCHER_ROOT=str(root)), timeout=120)
    try:
        theirs = json.loads(r.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        theirs = [r.stderr[-600:]]
    mine = [h.uninstall_script(*h.uninstall_plan(root, g)) for g in ('', str(game))]
    check('its uninstall removes exactly what the launcher\'s own would', mine == theirs, f'\n mine   {mine}\n theirs {theirs}')

    srv.shutdown()
    srv2.shutdown()
    print(f'\n{"ALL PASSED" if not FAILED else str(len(FAILED)) + " FAILED: " + "; ".join(FAILED)}')
    return 1 if FAILED else 0


if __name__ == '__main__':
    sys.exit(main())
