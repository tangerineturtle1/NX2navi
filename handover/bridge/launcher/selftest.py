"""Pictures of the handover window without showing it and without doing anything: for whoever wants to look at it
before it is released, and for the tests.

    NX2navi.exe --selftest --shots DIR          through the stub of an installed launcher
    pythonw start.pyw --selftest --shots DIR    directly
    NX2NAVI_HANDOVER_TEST=DIR                   the same from the environment (a test that starts the stub)

The window opens far off the screen, each state is drawn and saved as handover-<state>.png (the window as Windows
draws it, title bar included), state.json says what was found on this PC, and the process ends with exit code 0.
Started through the environment it first stays until it is eight seconds old, longer than the stub waits for a
failed start. No button is pressed, nothing is started, downloaded or written outside DIR.
"""
import ctypes
import json
import os
import struct
import time
import zlib
from pathlib import Path

import handover

BORN = time.time()


def write_png(path, w, h, rgb):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))


def snap(win, path):
    """The window as Windows draws it (PrintWindow: it need not be on the screen). False where that is not possible."""
    if os.name != 'nt':
        return False
    from ctypes import wintypes
    user32, gdi32 = ctypes.WinDLL('user32'), ctypes.WinDLL('gdi32')
    P = ctypes.c_void_p
    user32.GetParent.restype, user32.GetParent.argtypes = P, [P]
    user32.GetWindowRect.argtypes = [P, ctypes.POINTER(wintypes.RECT)]
    user32.GetWindowDC.restype, user32.GetWindowDC.argtypes = P, [P]
    user32.ReleaseDC.argtypes = [P, P]
    user32.PrintWindow.argtypes = [P, P, ctypes.c_uint]
    gdi32.CreateCompatibleDC.restype, gdi32.CreateCompatibleDC.argtypes = P, [P]
    gdi32.CreateCompatibleBitmap.restype, gdi32.CreateCompatibleBitmap.argtypes = P, [P, ctypes.c_int, ctypes.c_int]
    gdi32.SelectObject.restype, gdi32.SelectObject.argtypes = P, [P, P]
    gdi32.GetDIBits.argtypes = [P, P, ctypes.c_uint, ctypes.c_uint, P, P, ctypes.c_uint]
    gdi32.DeleteObject.argtypes = [P]
    gdi32.DeleteDC.argtypes = [P]
    hwnd = user32.GetParent(win.winfo_id()) or win.winfo_id()
    r = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    if w <= 0 or h <= 0:
        return False
    dc = user32.GetWindowDC(hwnd)
    mem = gdi32.CreateCompatibleDC(dc)
    bmp = gdi32.CreateCompatibleBitmap(dc, w, h)
    old = gdi32.SelectObject(mem, bmp)
    try:
        user32.PrintWindow(hwnd, mem, 2)   # PW_RENDERFULLCONTENT
        header = struct.pack('<IiiHHIIiiII', 40, w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
        info = ctypes.create_string_buffer(header, 40 + 12)
        buf = ctypes.create_string_buffer(w * h * 4)
        if not gdi32.GetDIBits(mem, bmp, 0, h, buf, info, 0):
            return False
        raw = buf.raw
        rgb = bytearray(w * h * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = raw[2::4], raw[1::4], raw[0::4]
        write_png(path, w, h, bytes(rgb))
        return True
    finally:
        gdi32.SelectObject(mem, old)
        gdi32.DeleteObject(bmp)
        gdi32.DeleteDC(mem)
        user32.ReleaseDC(hwnd, dc)


SAMPLE = {'dump': r'D:\Games\dumps\My game dump [0400486024dec000][v131072].nup', 'game_dir': r'D:\Games\NX2navi',
          'game': Path(r'D:\Games\NX2navi\ocarina.exe')}
STATES = [
    ('new', dict(SAMPLE, triforce=None), '', False),
    ('triforce-installed', dict(SAMPLE, triforce=Path(r'C:\Triforce\TriforceSetup.exe')), '', False),
    ('nothing-built', {'triforce': None, 'game': None, 'dump': '', 'game_dir': ''}, '', False),
    ('copied', dict(SAMPLE, triforce=None), 'Copied. Paste it where Triforce Setup asks for it.', False),
    ('problem', dict(SAMPLE, triforce=None),
     'Triforce Setup could not be started from here. Get it from triforceproject.com, which opens now.', True),
]


def run(app, out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    real = dict(app.found)
    shots = {}
    for _ in range(5):
        app.win.update()
        time.sleep(0.1)
    try:   # the first picture Windows gives of a window is an empty frame: take it and throw it away
        snap(app.win, out / 'first-frame.png')
        (out / 'first-frame.png').unlink()
    except Exception:
        pass
    for name, found, status, bad in STATES + [('this-pc', real, '', False)]:
        app.found, app.status, app.status_color = dict(found), status, handover.BAD if bad else handover.GOLD_300
        app.draw()
        app.win.update()
        time.sleep(0.15)
        app.win.update()
        try:
            shots[name] = bool(snap(app.win, out / f'handover-{name}.png'))
        except Exception as e:
            shots[name] = f'{type(e).__name__}: {e}'
    state = {'version': handover.my_version(), 'arguments': app.argv, 'launcher_root': str(app.root),
             'scale': app.s, 'tk': str(app.win.tk.call('info', 'patchlevel')),
             'found': {k: (str(v) if v else '') for k, v in real.items()}, 'shots': shots}
    try:
        _, info = handover.bundled_setup()
        state['setup'] = {'ok': True, **info}
    except Exception as e:
        state['setup'] = {'ok': False, 'error': f'{type(e).__name__}: {e}'}
    (out / 'state.json').write_text(json.dumps(state, indent=1), encoding='utf-8')
    wait = max(0.0, 8.0 - (time.time() - BORN)) if os.environ.get('NX2NAVI_HANDOVER_TEST') else 0.0
    app.win.after(int(wait * 1000) + 50, app.win.destroy)
