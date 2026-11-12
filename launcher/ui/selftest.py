"""--selftest [--shots DIR]: draw every screen with made-up states, optionally save a PNG of each, and close.
Nothing runs: the jobs are built and their steps set by hand."""
import os
import subprocess
import sys
import time
from pathlib import Path

from .. import feed, jobs, versions


def grab_gdi(x, y, w, h, path):
    """Windows: copy the screen rectangle with GDI and write it as PNG (art.png_bytes); no extra packages."""
    import ctypes
    from ctypes import wintypes
    import numpy as np
    from . import art
    # private handles with every argument typed: a handle is a pointer, and one above 2^31 passed as a plain int
    # failed with "int too long to convert", so some screens went without their picture
    user32, gdi32 = ctypes.WinDLL('user32'), ctypes.WinDLL('gdi32')
    vp = ctypes.c_void_p
    user32.GetDC.restype = gdi32.CreateCompatibleDC.restype = gdi32.CreateCompatibleBitmap.restype = vp
    gdi32.SelectObject.restype = vp
    user32.GetDC.argtypes = gdi32.CreateCompatibleDC.argtypes = gdi32.DeleteObject.argtypes = [vp]
    gdi32.DeleteDC.argtypes = [vp]
    user32.ReleaseDC.argtypes = gdi32.SelectObject.argtypes = [vp, vp]
    gdi32.CreateCompatibleBitmap.argtypes = [vp, ctypes.c_int, ctypes.c_int]
    gdi32.BitBlt.argtypes = [vp, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, vp, ctypes.c_int,
                             ctypes.c_int, wintypes.DWORD]
    gdi32.GetDIBits.argtypes = [vp, vp, ctypes.c_uint, ctypes.c_uint, vp, vp, ctypes.c_uint]
    screen = user32.GetDC(None)
    mem = gdi32.CreateCompatibleDC(screen)
    bmp = gdi32.CreateCompatibleBitmap(screen, w, h)
    old = gdi32.SelectObject(mem, bmp)
    gdi32.BitBlt(mem, 0, 0, w, h, screen, x, y, 0x00CC0020)   # SRCCOPY

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [('biSize', wintypes.DWORD), ('biWidth', wintypes.LONG), ('biHeight', wintypes.LONG),
                    ('biPlanes', wintypes.WORD), ('biBitCount', wintypes.WORD), ('biCompression', wintypes.DWORD),
                    ('biSizeImage', wintypes.DWORD), ('biXPelsPerMeter', wintypes.LONG),
                    ('biYPelsPerMeter', wintypes.LONG), ('biClrUsed', wintypes.DWORD),
                    ('biClrImportant', wintypes.DWORD)]
    bi = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = (ctypes.c_ubyte * (w * h * 4))()
    gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0)
    gdi32.SelectObject(mem, old)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(None, screen)
    a = np.frombuffer(buf, dtype=np.uint8).reshape(h, w, 4)[..., [2, 1, 0, 3]].copy()
    a[..., 3] = 255
    Path(path).write_bytes(art.png_bytes(a))
    return True


def grab(app, path):
    """A PNG of the window: GDI on Windows, else PIL's ImageGrab, else ImageMagick's import on X11."""
    app.root.update()
    x, y = app.root.winfo_rootx(), app.root.winfo_rooty()
    w, h = app.root.winfo_width(), app.root.winfo_height()
    if os.name == 'nt':
        try:
            return grab_gdi(x, y, w, h, path)
        except Exception as e:
            app.ctx.log(f'selftest: screen capture failed: {e!r}')
    try:
        from PIL import ImageGrab
        ImageGrab.grab(bbox=(x, y, x + w, y + h), xdisplay=os.environ.get('DISPLAY') if os.name != 'nt' else None
                       ).save(path)
        return True
    except Exception:
        pass
    try:
        subprocess.run(['import', '-window', str(app.root.winfo_id()), str(path)], check=True, capture_output=True)
        return True
    except (OSError, subprocess.CalledProcessError):
        return False


def ahead_of(version):
    """A version a minor step past version: the made-up release must be newer than the launcher drawing it, or the
    update screens show "Up to Date" instead (a fixed 0.3.11 did from 0.4.0 on)."""
    try:
        nums, _ = versions.parse(version)
    except ValueError:
        return '99.0.0'
    return f'{nums[0]}.{nums[1] + 1}.0'


def fake_release(version='0.3.11'):
    m = {'format': 1, 'version': version, 'app': {'name': f'NX2navi-app-{version}.zip', 'size': 1, 'sha256': '0'}}
    notes = ('- Each program module comes from the .nup package that holds it whole.\n'
             '- The PC stays awake while the game builds.\n- This launcher: one-click install, updates and play.')
    return feed.Release(m, notes)


def states(app):
    ctx = app.ctx
    s = app.settings
    s['dump'] = s.get('dump') or str(Path.home() / 'Downloads' / 'Ocarina of Time.nup')
    s['game_dir'] = s.get('game_dir') or ('D:\\Games\\NX2navi' if os.name == 'nt' else '/games/NX2navi')
    app.dumps = [Path(s['dump'])]

    def first_run():
        app.show('first_run', len(app.rows_first_run()) - 1)

    def installing():
        j = jobs.InstallJob(ctx, s['dump'], s['game_dir'])
        for sid, val in (('tools', 'Done · 0:48'), ('check', 'Version 1.1.1'), ('exefs', 'Done · 0:04'),
                         ('romfs', 'Done · 0:31')):
            j.step(sid).state, j.step(sid).value, j.step(sid).fraction = jobs.DONE, val, 1.0
        j.begin('lift')
        j.progress('lift', 0.34, '412 of 1,204 chunks')
        j.started = time.time() - 192
        for ln in ('check: Ocarina of Time 1.1.1 (build e0f3ccfa)', 'romfs: 26.0 of 26.0 GB, 31 s', 'lift:',
                   '  emitting with 16 workers', '  emitted 412/1204 chunks, 18s'):
            j.lines.append(ln)
        app.job, app.job_return, app.job_back = j, 'home', 'first_run'
        app.show('job')

    def home():
        app.release, app.checked_at, app.check, app.check_error, app.message = None, time.time() - 5, None, '', ''
        app.show('home', 0)

    def update_ready():
        app.release = fake_release(ahead_of(ctx.version))
        app.show('home', 1)

    def updating():
        j = jobs.UpdateDownloadJob(ctx, fake_release(ahead_of(ctx.version)))
        for sid, val in (('download', 'Done · 0:06'), ('verify', 'Checksum OK')):
            j.step(sid).state, j.step(sid).value, j.step(sid).fraction = jobs.DONE, val, 1.0
        j.skip('tools_update', 'Unchanged')
        j.step('lift').state, j.step('lift').value, j.step('lift').fraction = jobs.DONE, 'Done · 0:41', 1.0
        j.begin('compile_changed')
        j.progress('compile_changed', 0.61, '238 of 391 changed files')
        j.started = time.time() - 104
        app.job, app.job_return = j, 'home'
        app.show('job')

    def settings():
        app.show('settings', 0)

    def failed():
        j = jobs.InstallJob(ctx, s['dump'], s['game_dir'])
        for sid, val in (('tools', 'Done · 0:48'), ('check', 'Version 1.1.1'), ('exefs', 'Done · 0:04'),
                         ('romfs', 'Done · 0:31'), ('lift', 'Done · 0:48')):
            j.step(sid).state, j.step(sid).value, j.step(sid).fraction = jobs.DONE, val, 1.0
        j.step('compile').state, j.step('compile').value = jobs.FAILED, 'Stopped'
        j.result, j.error = 'failed', '3 files did not compile'
        j.started, j.finished = time.time() - 362, time.time()
        for ln in ('compile:', '  compiled 1200 of 1212, 236 s', 'error: 3 files did not compile'):
            j.lines.append(ln)
        app.job, app.job_return, app.job_back = j, 'home', 'first_run'
        app.show('job')

    def uninstall():
        app.show('uninstall', 0)

    def shaders():   # the counts and the wording tools/make_game.py shader_progress sends
        j = jobs.InstallJob(ctx, s['dump'], s['game_dir'])
        for sid, val in (('tools', 'Done · 0:48'), ('check', 'Version 1.1.1'), ('exefs', 'Done · 0:04'),
                         ('romfs', 'Done · 0:31'), ('lift', 'Done · 0:48'), ('compile', 'Done · 4:12')):
            j.step(sid).state, j.step(sid).value, j.step(sid).fraction = jobs.DONE, val, 1.0
        j.begin('shaders')
        j.progress('shaders', 0.5 + 0.5 * 6214 / 20480, 'Built 6,214 of 20,480 shaders for your graphics card')
        j.started = time.time() - 611
        for ln in ('  shaders: translated 20480 of 20480, 141 s', '  shaders: built 6214 of 20480, 402 s'):
            j.lines.append(ln)
        app.job, app.job_return, app.job_back = j, 'home', 'first_run'
        app.show('job')

    def import_saves():
        app.release, app.message, app.export_message = None, '', ''
        app.save_message = ('Save imported from NX2navi-save-2026-11-09_17-30-05.zip (exported 2026-11-09 with NX2navi '
                            '0.6.1). Your old save is in saves' + chr(92) + 'backup 2026-11-09 18.02.44.')
        app.show('saves', next(i for i, r in enumerate(app.rows_saves()) if r.label == 'Import a Save'))

    def export_save():
        app.save_message = ''
        app.export_message = ('Exported 14 files with the game' + chr(39) + 's log and settings to '
                              + str(Path.home() / 'Downloads' / 'NX2navi-save-2026-11-09_17-30-05-bug-report.zip')
                              + '.')
        app.show('saves', 0)

    def update_staged():   # a release downloaded in the background, waiting for the game to be closed
        rel = fake_release(ahead_of(ctx.version))
        staged = {'version': rel.version, 'runtime': '', 'prerelease': False, 'notes': rel.notes}
        app.bg.ready = lambda release=None: staged
        app.release, app.message = rel, ''
        app.show('home', 1)

    def customize():
        app.bg.ready = lambda release=None: None
        app.release, app.message, app.look_message = None, '', ''
        app.show('customize', 0)

    def color_scheme():   # another colour scheme on the home screen, then the first one again
        from . import art
        was = s.get('theme')
        s['theme'] = list(art.THEMES)[2]
        app.apply_look()
        app.show('home', 0)
        s['theme'] = was

    def custom_picker():   # the Custom scheme with two picked colours, and its picker on the button colour
        from . import art
        was = {k: s.get(k) for k in ('theme', 'custom_background', 'custom_panel', 'custom_buttons')}
        s.update(theme='custom', custom_background='#3a1d5c', custom_panel='#24324a', custom_buttons='#2f6fb0')
        app.apply_look()
        app.open_picker('buttons')
        s.update(was)
        art.set_theme(was['theme'], app.custom_colors())   # the next screen's colours; this one is drawn already

    def restore_scheme():
        app.apply_look()
        app.show('home', 0)

    was_game = s.get('game_dir')

    def mods_screen():   # a game folder of made-up mods, removed when the selftest closes
        import atexit
        import shutil
        import tempfile
        game = Path(tempfile.mkdtemp(prefix='nx2navi-selftest-'))
        atexit.register(shutil.rmtree, game, True)
        for name, files in (('HD Textures', 40), ('Better Menus', 12), ('Classic Music', 3)):
            for k in range(files):
                f = game / 'mods' / name / 'romfs' / ('UI' if k < 6 else name.split()[0]) / f'file{k}.bin'
                f.parent.mkdir(parents=True, exist_ok=True)
                f.write_bytes(b'0')
        (game / 'mods' / 'Old Plugin' / 'exefs').mkdir(parents=True)
        (game / 'mods' / 'Old Plugin' / 'exefs' / 'subsdk9').write_bytes(b'0')
        (game / 'nx2navi.ini').write_text('[mods]\nmods_disabled = Classic Music\n', encoding='utf-8')
        s['game_dir'] = str(game)
        app.mods_page, app.mods_message = 0, ''
        app.show('mods', next(i for i, r in enumerate(app.rows_mods()) if r.label == 'HD Textures'))

    def mod_order():
        app.show('mod_order', 1)

    def restore_game():
        s['game_dir'] = was_game
        app.show('home', 0)

    return [('1-first-run', first_run), ('2-installing', installing), ('3-home', home),
            ('4-update-ready', update_ready), ('5-updating', updating), ('6-settings', settings),
            ('7-build-failed', failed), ('8-uninstall', uninstall), ('9-import-saves', import_saves),
            ('10-shaders', shaders), ('11-update-staged', update_staged), ('12-export-save', export_save),
            ('13-customize', customize), ('14-color-scheme', color_scheme), ('15-custom-picker', custom_picker),
            ('16-first-scheme', restore_scheme), ('17-mods', mods_screen), ('18-mod-order', mod_order),
            ('19-home-again', restore_game)]


def run(app, shots):
    out = Path(shots) if shots else None
    if out:
        out.mkdir(parents=True, exist_ok=True)
    seq = states(app)
    ok = True

    def step(i=0):
        nonlocal ok
        if i >= len(seq):
            app.ctx.log(f'selftest: {len(seq)} screens drawn')
            print(f'selftest: {len(seq)} screens drawn' + (f', shots in {out}' if out else ''), flush=True)
            app.root.after(100, app.root.destroy)
            return
        name, fn = seq[i]
        try:
            fn()
            app.root.update()
        except Exception as e:
            ok = False
            print(f'selftest: {name}: {e!r}', file=sys.stderr, flush=True)
            raise
        if out:
            app.root.after(350, lambda: (grab(app, out / f'{name}.png'), step(i + 1)))
        else:
            app.root.after(50, lambda: step(i + 1))
    step()
