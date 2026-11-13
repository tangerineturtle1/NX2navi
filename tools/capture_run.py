"""Start a test run from a bug capture (runtime/bugcap.c, docs/bug-capture.md).

    python tools/capture_run.py <capture folder or number> [--game DIR] [--set KEY=VALUE ...]

The game starts on a throwaway profile holding the capture's save, with the capture's settings in place of nx2navi.ini
(NX2NAVI_CAPTURE_REPLAY), in a window of the capture's size that does not take the focus. It loads the save, goes to
the capture's area, walks Link to where he stood, puts the camera where it was and takes a capture of its own; this
script then closes the game and prints both captures side by side: the positions, the distance between them, and how
much the two pictures differ. The player's nx2navi.ini and save are not written.

One game at a time: the script refuses to start while a game is running.
"""
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import time
import zipfile
import zlib
from pathlib import Path

WIN = os.name == 'nt'
SKIP_IN_PROFILE = {'save', 'save-backup', 'save-tx', 'save-import', 'save-import.part', 'coop-backup', 'instance.lock'}


def default_game():
    if WIN:
        return Path(os.environ.get('LOCALAPPDATA', '')) / 'Programs' / 'NX2navi' / 'game'
    return Path.home() / '.local' / 'share' / 'NX2navi' / 'game'


def find_exe(game):
    for name in ('ocarina.exe', 'nx2navi.exe', 'ocarina', 'nx2navi'):
        if (game / name).is_file():
            return game / name
    return None


def read_capture(folder):
    """capture.txt as a dict of its "key = value" lines (the first of a key wins)."""
    out = {}
    for line in (folder / 'capture.txt').read_text(encoding='utf-8', errors='replace').splitlines():
        key, sep, value = line.partition(' = ')
        if sep and not key.startswith('#') and key not in out:
            out[key] = value.split('   # ')[0].strip()
    return out


def find_capture(arg, game):
    p = Path(arg)
    if p.name == 'capture.txt':
        p = p.parent
    if (p / 'capture.txt').is_file():
        return p.resolve()
    if arg.isdigit():
        hits = sorted((game / 'logs' / 'captures').glob(f'{int(arg):03d}-*'))
        hits = [h for h in hits if (h / 'capture.txt').is_file()]
        if hits:
            return hits[-1].resolve()
    return None


def game_pids(exe):
    """The ids of the processes running the game's executable, by its file name."""
    if WIN:
        out = subprocess.run(['tasklist', '/FI', f'IMAGENAME eq {exe.name}', '/FO', 'CSV', '/NH'], capture_output=True, text=True).stdout
        pids = []
        for line in out.splitlines():
            cells = [c.strip('"') for c in line.split('","')]
            if len(cells) > 1 and cells[0].lower() == exe.name.lower() and cells[1].isdigit():
                pids.append(int(cells[1]))
        return pids
    out = subprocess.run(['pgrep', '-x', exe.name], capture_output=True, text=True).stdout
    return [int(x) for x in out.split() if x.isdigit()]


def game_running(exe):
    return bool(game_pids(exe))


def make_profile(profile, game, capture):
    """A profile whose save is the capture's: the game's own profile without its saves, and the capture's save zip
    as a waiting import (runtime/fs_host.c import_pending), which the game makes the save at its start."""
    src = game / 'profile'
    profile.mkdir(parents=True)
    if src.is_dir():
        for entry in src.iterdir():
            if entry.name in SKIP_IN_PROFILE:
                continue
            if entry.is_dir():
                shutil.copytree(entry, profile / entry.name)
            else:
                shutil.copy2(entry, profile / entry.name)
    zips = sorted(capture.glob('NX2navi-save-*.zip'))
    if not zips:
        if (src / 'save').is_dir():
            shutil.copytree(src / 'save', profile / 'save')
        return 'the capture holds no save: a copy of the save the game has now'
    staged = profile / 'save-import'
    files = 0
    with zipfile.ZipFile(zips[0]) as z:
        for info in z.infolist():
            name = info.filename
            if info.is_dir() or not name.startswith('save/'):
                continue
            rel = Path(name[5:])
            if rel.is_absolute() or '..' in rel.parts or name.lower().endswith(('.keys', '.keys.txt')):
                continue
            dst = staged / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(z.read(info))
            files += 1
    return f'the capture\'s save ({files} files from {zips[0].name})'


def close_game(proc, exe, grace=30):
    """Ask the game's windows to close, as the window's own button does (it then writes its exit lines); by force when
    that does not end it. The game may be a child of the process this script started: at its start it runs itself
    again when Windows has put something where its memory must go (runtime/platform/win32.c plat_reexec), and the
    first process only waits for the second. No other game ran when this one started (main checks), so every process
    of the executable is this run's."""
    if proc.poll() is not None and not game_pids(exe):
        return 'had ended'
    if WIN:
        import ctypes
        from ctypes import wintypes
        user32 = ctypes.windll.user32
        proto = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        pids = set(game_pids(exe)) | {proc.pid}

        def each(hwnd, _):
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value in pids and user32.IsWindowVisible(hwnd):
                user32.PostMessageW(hwnd, 0x10, 0, 0)   # WM_CLOSE
            return True
        user32.EnumWindows(proto(each), 0)
    else:
        proc.terminate()
    end = time.time() + grace
    while time.time() < end and (proc.poll() is None or game_pids(exe)):
        time.sleep(0.5)
    if proc.poll() is not None and not game_pids(exe):
        return 'closed'
    if WIN:
        for pid in game_pids(exe):
            subprocess.run(['taskkill', '/PID', str(pid), '/T', '/F'], capture_output=True)
    if proc.poll() is None:
        proc.kill()
    proc.wait()
    time.sleep(1)
    return 'STILL RUNNING: close it by hand' if game_pids(exe) else 'closed by force'


def read_png(path):
    """(width, height, bytes per pixel, rows) of an 8-bit RGB or RGBA PNG without interlacing, as the game writes."""
    data = path.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    at, idat, head = 8, [], None
    while at + 8 <= len(data):
        n, kind = struct.unpack('>I4s', data[at:at + 8])
        body = data[at + 8:at + 8 + n]
        if kind == b'IHDR':
            head = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat.append(body)
        at += 12 + n
    w, h, depth, colour, _, _, interlace = head
    if depth != 8 or colour not in (2, 6) or interlace:
        raise ValueError('a PNG kind this does not read')
    bpp = 3 if colour == 2 else 4
    raw = zlib.decompress(b''.join(idat))
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                line[i] = (line[i] + ((line[i - bpp] if i >= bpp else 0) + prev[i]) // 2) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, h, bpp, rows


def picture_difference(a, b, step=4):
    """The mean difference of the colour channels (0 to 255) over every step-th pixel of two pictures of one size."""
    wa, ha, bpa, ra = read_png(a)
    wb, hb, bpb, rb = read_png(b)
    if (wa, ha) != (wb, hb):
        return None, f'{wa}x{ha} against {wb}x{hb}'
    total = count = 0
    for y in range(0, ha, step):
        la, lb = ra[y], rb[y]
        for x in range(0, wa, step):
            for c in range(3):
                total += abs(la[x * bpa + c] - lb[x * bpb + c])
            count += 3
    return total / count, f'{wa}x{ha}'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else ''


def main():
    ap = argparse.ArgumentParser(description='Start a test run at a bug capture\'s place, camera and settings.')
    ap.add_argument('capture', help='a capture\'s folder, or its number in the game\'s logs/captures')
    ap.add_argument('--game', type=Path, default=default_game(), help='the game folder (default: the installed game)')
    ap.add_argument('--exe', type=Path, help='the game\'s executable (default: the one in the game folder)')
    ap.add_argument('--out', type=Path, help='where the run\'s log and its own capture go (default: <capture>/replays)')
    ap.add_argument('--seconds', type=int, default=300, help='give up after this long (default 300)')
    ap.add_argument('--hold', type=int, default=0, help='seconds to keep the game open after its capture, to watch or test')
    ap.add_argument('--set', action='append', default=[], metavar='KEY=VALUE',
                    help='a setting for this run on top of the capture\'s (NX2NAVI_ is added), e.g. --set FSR4=0')
    ap.add_argument('--no-camera', action='store_true', help='leave the camera to the game instead of placing it')
    ap.add_argument('--fullscreen', action='store_true', help='keep the capture\'s fullscreen and window settings')
    ap.add_argument('--foreground', action='store_true', help='a normal window that takes the focus and your input')
    args = ap.parse_args()

    game = args.game.resolve()
    exe = (args.exe or find_exe(game) or game / 'ocarina.exe').resolve()
    if not exe.is_file():
        sys.exit(f'no game executable in {game}')
    capture = find_capture(args.capture, game)
    if not capture:
        sys.exit(f'no capture.txt found for "{args.capture}" (a folder, or a number under {game / "logs" / "captures"})')
    if game_running(exe):
        sys.exit(f'{exe.name} is running: one game at a time. Close it first.')
    cap = read_capture(capture)
    out = (args.out or capture / 'replays').resolve()
    out.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime('%Y%m%d-%H%M%S')
    profile = out / f'profile-{stamp}'
    log = out / f'run-{stamp}.log'
    print(f'capture {cap.get("number", "?")} ({capture.name}): {cap.get("area", "no area")}, Link at {cap.get("player_pos", "?")}, '
          f'camera at {cap.get("camera_pos", "not captured")}; NX2navi {cap.get("nx2navi", "?")}')
    print('profile:', make_profile(profile, game, capture))

    env = {k: v for k, v in os.environ.items() if not k.upper().startswith('NX2NAVI_')}
    pins = {'CAPTURE_REPLAY': str(capture), 'CAPTURE_DIR': str(out), 'COOP': '0', 'COOP_STEAM': '0', 'DISCORD_RPC': '0'}
    if not args.fullscreen:
        pins['FULLSCREEN'] = '0'
        if 'x' in cap.get('window', ''):
            pins['WINDOW_SIZE'] = cap['window']
    if not args.foreground:
        pins.update(BACKGROUND_WINDOW='1', ASSUME_FOCUS='1', INPUT='none')
    if args.no_camera:
        pins['CAPTURE_REPLAY_CAMERA'] = '0'
    for pair in args.set:
        key, sep, value = pair.partition('=')
        if not sep:
            sys.exit(f'--set takes KEY=VALUE, not "{pair}"')
        key = key.strip().upper()
        pins[key[8:] if key.startswith('NX2NAVI_') else key] = value
    for key, value in pins.items():
        env['NX2NAVI_' + key] = value
    print('this run sets:', ', '.join(f'{k}={v}' for k, v in sorted(pins.items()) if k not in ('CAPTURE_REPLAY', 'CAPTURE_DIR')))

    ini = game / 'nx2navi.ini'
    ini_before = sha(ini)
    before = {p.name for p in out.iterdir() if p.is_dir()}
    result, state = None, 'timed out'
    with open(log, 'wb') as err:
        proc = subprocess.Popen([str(exe), '--profile', str(profile)], cwd=str(game), env=env, stderr=err,
                                stdout=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
        t0 = time.time()
        ready = False
        try:
            while time.time() - t0 < args.seconds and proc.poll() is None:
                time.sleep(1)
                text = log.read_text(encoding='utf-8', errors='replace')
                if 'capture: replay of' in text and ' stopped: ' in text:
                    state = 'the replay stopped'
                    break
                if not ready and ' ready: ' in text and 'capture: replay of' in text:
                    ready = True
                if ready and any(' saved to ' in line and '(replay' in line for line in text.splitlines()):
                    state = 'done'
                    break
            if proc.poll() is not None:
                state = f'the game ended by itself (exit {proc.returncode})'
            elif state == 'done' and args.hold:
                print(f'the game stays open for {args.hold} s')
                time.sleep(args.hold)
        finally:
            print('game:', close_game(proc, exe), f'after {time.time() - t0:.0f} s; {state}')
    shutil.rmtree(profile, ignore_errors=True)
    print('nx2navi.ini', 'unchanged' if sha(ini) == ini_before else 'CHANGED by the run: put it back from your copy')

    text = log.read_text(encoding='utf-8', errors='replace')
    for line in text.splitlines():
        if 'capture: ' in line or 'debug: route' in line or ('debug: ' in line and ('eleport' in line or 'Arrived' in line)):
            print('  ' + line[:300])
    new = sorted(p for p in out.iterdir() if p.is_dir() and p.name not in before and (p / 'capture.txt').is_file())
    if new:
        result = new[-1]
        rep = read_capture(result)
        print(f'\nthe run\'s own capture: {result}')
        for key in ('area', 'player_pos', 'camera_pos', 'camera_yaw', 'camera_pitch', 'camera_fov_y_degrees', 'window', 'game_image'):
            a, b = cap.get(key, '-'), rep.get(key, '-')
            print(f'  {key:22} {a:38} {b:38} {"same" if a == b else ""}')
        print(f'  Link ended {rep.get("replay_player_off_metres", "?")} m from the captured place; the walk: {rep.get("replay_walk", "?")}; '
              f'camera: {rep.get("replay_camera", "?")}')
        if (capture / 'frame.png').is_file() and (result / 'frame.png').is_file():
            try:
                diff, size = picture_difference(capture / 'frame.png', result / 'frame.png')
                print(f'  pictures ({size}):', 'of different sizes' if diff is None else
                      f'mean difference {diff:.1f} of 255 per channel (two frames of one standing view differ by 1 to 4)')
            except (ValueError, zlib.error, struct.error) as e:
                print('  pictures: not compared:', e)
    else:
        print('\nno capture came out of the run; the log:', log)
    return 0 if result else 1


if __name__ == '__main__':
    sys.exit(main())
