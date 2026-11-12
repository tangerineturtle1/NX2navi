"""The Linux AppImage's window (tools/make_appimage.py): pick the dump and the game folder, install or update the game
with progress, and play. AppRun opens it when the AppImage starts without arguments on a desktop (double-click); the
command line keeps working as before.

It runs AppRun itself with --events, which does what the command line does (the package check, zbic, make_game.py,
NVIDIA's DLSS, play.sh) and prints make_game.py's "@@" progress lines (tools/make_game.py, --events) plus its own for
the steps around it. The last dump and folder are remembered in ~/.config/nx2navi/appimage.json, the last run's output
in ~/.cache/nx2navi/build.log (Details opens it).

The window is one Tk canvas. The Tk that comes with python-build-standalone has no Xft, so its only font is X11's
bitmap "fixed", unreadable on a high-DPI screen: text is drawn with Pillow in the desktop's own font (fc-match
sans-serif) as PNG images, everything is sized by the screen's DPI (Tk's winfo_fpixels, which follows Xft.dpi), and the
file choosers are the desktop's (kdialog or zenity, else Tk's). Without fc-match or Pillow, text falls back to Tk's.

    python3 tools/appimage_gui.py --apprun <AppRun> [--icon <png>]
"""
import argparse
import base64
import io
import json
import locale
import os
import queue
import shutil
import subprocess
import threading
import tkinter as tk
from pathlib import Path

CONFIG = Path(os.environ.get('XDG_CONFIG_HOME') or Path.home() / '.config') / 'nx2navi' / 'appimage.json'
CACHE = Path(os.environ.get('XDG_CACHE_HOME') or Path.home() / '.cache') / 'nx2navi'
APPS = Path(os.environ.get('XDG_DATA_HOME') or Path.home() / '.local' / 'share') / 'applications'
DEFAULT_OUT = Path.home() / 'Games' / 'Ocarina'

# make_game.py's steps (STEPS, plus check) and AppRun's own around them, in the order they run
STEPS = ('deps', 'check', 'exefs', 'romfs', 'lift', 'compile', 'shaders', 'prompts', 'dlss')

TEXT = {
    'en': {
        'title': 'Ocarina of Time for Linux',
        'intro': 'Builds the game natively on this PC from your own dump (Switch 2, game 1.1.1). No game files are included.',
        'dump': 'Game dump', 'folder': 'Game folder', 'browse': 'Browse…', 'none': 'not chosen yet',
        'install': 'Install', 'update': 'Update', 'play': 'Play', 'cancel': 'Cancel', 'confirm': 'Stop the build?',
        'details': 'Details', 'shortcut': 'Add to the applications menu',
        'deps': 'Check the packages', 'check': 'Check the dump', 'exefs': 'Unpack the program files',
        'romfs': 'Build the RomFS image', 'lift': 'Turn the code into C', 'compile': 'Compile the game',
        'shaders': 'Prepare the shaders', 'prompts': 'Draw the button prompts', 'dlss': 'NVIDIA DLSS',
        'running': 'running', 'done': 'done', 'skip': 'kept', 'failed': 'failed', 'na': '—',
        'ready': 'Close other heavy programs while it builds: it uses a lot of memory.',
        'finished': 'Finished. Press Play.', 'stopped': 'Stopped:', 'cancelled': 'Cancelled. Run it again to go on.',
        'nodump': 'Choose your .nup dump first.', 'busy': 'A build is running: Cancel it first, or press close again.',
        'missing': 'Packages are missing. Install them (see Details), then press the button again.',
    },
    'fr': {
        'title': 'Ocarina of Time pour Linux',
        'intro': 'Construit le jeu en natif sur ce PC à partir de votre dump (Switch 2, jeu 1.1.1). Aucun fichier du jeu n\'est inclus.',
        'dump': 'Dump du jeu', 'folder': 'Dossier du jeu', 'browse': 'Parcourir…', 'none': 'pas encore choisi',
        'install': 'Installer', 'update': 'Mettre à jour', 'play': 'Jouer', 'cancel': 'Annuler',
        'confirm': 'Arrêter le build ?', 'details': 'Détails', 'shortcut': 'Ajouter au menu des applications',
        'deps': 'Vérifier les paquets', 'check': 'Vérifier le dump', 'exefs': 'Extraire les fichiers du programme',
        'romfs': 'Construire l\'image RomFS', 'lift': 'Convertir le code en C', 'compile': 'Compiler le jeu',
        'shaders': 'Préparer les shaders', 'prompts': 'Dessiner les icônes de boutons', 'dlss': 'NVIDIA DLSS',
        'running': 'en cours', 'done': 'fait', 'skip': 'conservé', 'failed': 'échec', 'na': '—',
        'ready': 'Fermez les gros programmes pendant le build : il utilise beaucoup de mémoire.',
        'finished': 'Terminé. Appuyez sur Jouer.', 'stopped': 'Arrêté :',
        'cancelled': 'Annulé. Relancez-le pour continuer.', 'nodump': 'Choisissez d\'abord votre dump .nup.',
        'busy': 'Un build est en cours : annulez-le d\'abord, ou fermez encore une fois.',
        'missing': 'Des paquets manquent. Installez-les (voir Détails), puis appuyez de nouveau sur le bouton.',
    },
}

COLORS = {'bg': '#1f2329', 'panel': '#2a2f37', 'text': '#e8e6e3', 'dim': '#9aa0a8', 'accent': '#5fb3f0',
          'ok': '#7cc47f', 'warn': '#f0b35f', 'bad': '#f07a6a', 'button': '#3a414c', 'button_hi': '#4a5260',
          'primary': '#2f7fc1', 'primary_hi': '#3b90d6', 'track': '#3a3f47'}


def lang():
    code = (os.environ.get('LANGUAGE') or os.environ.get('LC_ALL') or
            os.environ.get('LC_MESSAGES') or os.environ.get('LANG') or
            (locale.getlocale()[0] or ''))
    return 'fr' if code.lower().startswith('fr') else 'en'


T = TEXT[lang()]


def load_config():
    try:
        return json.loads(CONFIG.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}


def save_config(cfg):
    try:
        CONFIG.parent.mkdir(parents=True, exist_ok=True)
        CONFIG.write_text(json.dumps(cfg, indent=1), encoding='utf-8')
    except OSError:
        pass


def fc_font(pattern):
    try:
        out = subprocess.run(['fc-match', '-f', '%{file}', pattern], capture_output=True, text=True, timeout=5).stdout
        return out if out and Path(out).is_file() else None
    except (OSError, subprocess.SubprocessError):
        return None


class Text:
    """Text as PNG images in the desktop's font (Pillow), cached; None when that is not possible (Tk's text then)."""
    def __init__(self, scale):
        self.scale, self.cache = scale, {}
        try:
            from PIL import Image, ImageDraw, ImageFont
            self.Image, self.ImageDraw, self.ImageFont = Image, ImageDraw, ImageFont
            self.files = {False: fc_font('sans-serif'), True: fc_font('sans-serif:bold')}
            self.ok = bool(self.files[False])
        except ImportError:
            self.ok = False
        self.fonts = {}

    def font(self, pt, bold):
        key = (pt, bold)
        if key not in self.fonts:
            px = max(8, round(pt * self.scale * 96 / 72))
            self.fonts[key] = self.ImageFont.truetype(self.files[bold] or self.files[False], px)
        return self.fonts[key]

    def wrap(self, text, pt, bold, width):
        if not self.ok:
            return [text]
        f, lines = self.font(pt, bold), []
        for para in text.split('\n'):
            line = ''
            for word in para.split(' '):
                trial = f'{line} {word}'.strip()
                if line and f.getlength(trial) > width:
                    lines.append(line)
                    line = word
                else:
                    line = trial
            lines.append(line)
        return lines

    def image(self, text, pt, color, bold=False):
        key = (text, pt, color, bold)
        if key in self.cache:
            return self.cache[key]
        f = self.font(pt, bold)
        x0, y0, x1, y1 = f.getbbox(text or ' ')
        ascent, descent = f.getmetrics()
        img = self.Image.new('RGBA', (max(1, int(x1) + 2), ascent + descent), (0, 0, 0, 0))
        self.ImageDraw.Draw(img).text((0, 0), text, font=f, fill=color)
        buf = io.BytesIO()
        img.save(buf, 'PNG')
        photo = tk.PhotoImage(data=base64.b64encode(buf.getvalue()))
        self.cache[key] = photo
        return photo


def pick_file(title, start):
    """The desktop's file chooser (kdialog, zenity), else Tk's."""
    if shutil.which('kdialog'):
        r = subprocess.run(['kdialog', '--title', title, '--getopenfilename', str(start), '*.nup|Switch 2 (*.nup)'],
                           capture_output=True, text=True)
        return r.stdout.strip() or None
    if shutil.which('zenity'):
        r = subprocess.run(['zenity', '--file-selection', '--title', title, '--filename', f'{start}/',
                            '--file-filter=*.nup', '--file-filter=*'], capture_output=True, text=True)
        return r.stdout.strip() or None
    from tkinter import filedialog
    return filedialog.askopenfilename(title=title, initialdir=str(start)) or None


def pick_dir(title, start):
    if shutil.which('kdialog'):
        r = subprocess.run(['kdialog', '--title', title, '--getexistingdirectory', str(start)], capture_output=True,
                           text=True)
        return r.stdout.strip() or None
    if shutil.which('zenity'):
        r = subprocess.run(['zenity', '--file-selection', '--directory', '--title', title, '--filename', f'{start}/'],
                           capture_output=True, text=True)
        return r.stdout.strip() or None
    from tkinter import filedialog
    return filedialog.askdirectory(title=title, initialdir=str(start)) or None


def exec_quote(path):
    """A path as one quoted argument of a desktop entry's Exec line: inside the quotes a backslash goes before a
    quote, a backtick, a dollar sign and a backslash, the line itself doubles each backslash again, and a percent sign
    is doubled so that it is no field code."""
    out = str(path)
    for ch in '\\"`$':
        out = out.replace(ch, '\\' + ch)
    return '"' + out.replace('\\', '\\\\').replace('%', '%%') + '"'


class App:
    W, H = 760, 640   # logical pixels (96 dpi); everything is drawn at scale

    def __init__(self, root, apprun, icon):
        self.root, self.apprun, self.icon = root, apprun, icon
        self.s = max(1.0, root.winfo_fpixels('1i') / 96)
        self.text = Text(self.s)
        cfg = load_config()
        self.nup = cfg.get('nup', '')
        self.out = cfg.get('out', str(DEFAULT_OUT))
        self.shortcut = cfg.get('shortcut', True)
        self.proc, self.events, self.log = None, queue.Queue(), []
        self.rows = {s: [0.0, ''] for s in STEPS}   # fraction, state
        self.status, self.status_color = T['ready'], 'dim'
        self.confirm = self.close_armed = False
        self.dirty = True
        root.title(f"NX2navi – {T['title']}")
        if icon and icon.is_file():
            try:
                self.icon_img = tk.PhotoImage(file=str(icon))
                root.iconphoto(True, self.icon_img)
            except tk.TclError:
                pass
        w, h = self.px(self.W), self.px(self.H)
        root.geometry(f'{w}x{h}')
        root.minsize(w, h)
        root.configure(bg=COLORS['bg'])
        self.c = tk.Canvas(root, width=w, height=h, bg=COLORS['bg'], highlightthickness=0)
        self.c.pack(fill='both', expand=True)
        self.c.bind('<Configure>', lambda e: self.mark())
        self.buttons = {}
        self.c.bind('<Button-1>', self.click)
        self.c.bind('<Motion>', self.hover)
        self.hot = None
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(50, self.tick)

    def px(self, v):
        return int(round(v * self.s))

    def mark(self):
        self.dirty = True

    # ---- drawing
    def label(self, x, y, text, pt=11, color='text', bold=False, anchor='nw'):
        if self.text.ok:
            return self.c.create_image(x, y, image=self.text.image(text, pt, COLORS[color], bold), anchor=anchor)
        return self.c.create_text(x, y, text=text, fill=COLORS[color], anchor=anchor,
                                  font=('TkDefaultFont', int(pt), 'bold' if bold else 'normal'))

    def rect(self, x0, y0, x1, y1, r, fill, outline=''):
        r = min(r, (x1 - x0) / 2, (y1 - y0) / 2)
        pts = [x0 + r, y0, x1 - r, y0, x1, y0, x1, y0 + r, x1, y1 - r, x1, y1, x1 - r, y1, x0 + r, y1, x0, y1,
               x0, y1 - r, x0, y0 + r, x0, y0]
        return self.c.create_polygon(pts, smooth=True, fill=fill, outline=outline)

    def button(self, key, x1, y, text, primary=False, enabled=True):
        """A button whose right edge is at x1; returns its left edge."""
        pt = 11
        width = (self.text.font(pt, True).getlength(text) if self.text.ok else len(text) * self.px(8)) + self.px(36)
        h = self.px(40)
        x0 = x1 - width
        hot = self.hot == key and enabled
        fill = (COLORS['primary_hi' if hot else 'primary'] if primary else COLORS['button_hi' if hot else 'button'])
        if not enabled:
            fill = COLORS['panel']
        self.rect(x0, y, x1, y + h, self.px(8), fill)
        self.label((x0 + x1) / 2, y + h / 2, text, pt, 'text' if enabled else 'dim', bold=True, anchor='center')
        self.buttons[key] = (x0, y, x1, y + h, enabled)
        return x0

    def draw(self):
        c, s = self.c, self.s
        c.delete('all')
        self.buttons = {}
        w = c.winfo_width() or self.px(self.W)
        m = self.px(24)
        y = self.px(20)
        self.label(m, y, T['title'], 18, bold=True)
        y += self.px(40)
        for line in self.text.wrap(T['intro'], 10.5, False, w - 2 * m):
            self.label(m, y, line, 10.5, 'dim')
            y += self.px(20)
        y += self.px(12)

        # the dump and the folder
        for key, title, value in (('dump', T['dump'], self.nup), ('out', T['folder'], self.out)):
            self.rect(m, y, w - m, y + self.px(56), self.px(10), COLORS['panel'])
            self.label(m + self.px(14), y + self.px(8), title, 10, 'dim', bold=True)
            shown = value or T['none']
            if self.text.ok:
                f, room = self.text.font(10.5, False), w - 2 * m - self.px(170)
                while f.getlength(shown) > room and len(shown) > 8:
                    shown = '…' + shown[len(shown) // 8 + 2:]
            self.label(m + self.px(14), y + self.px(28), shown, 10.5, 'text' if value else 'dim')
            self.button('pick_' + key, w - m - self.px(10), y + self.px(8), T['browse'], enabled=not self.proc)
            y += self.px(68)

        # the steps
        y += self.px(4)
        label_w = self.px(270)
        state_w = self.px(90)
        for step in STEPS:
            frac, state = self.rows[step]
            self.label(m, y, T[step], 10.5)
            bx0, bx1 = m + label_w, w - m - state_w
            by = y + self.px(7)
            self.rect(bx0, by, bx1, by + self.px(10), self.px(5), COLORS['track'])
            if frac > 0:
                color = COLORS['bad'] if state == 'failed' else COLORS['ok'] if state in ('done', 'skip') else COLORS['accent']
                self.rect(bx0, by, bx0 + max(self.px(10), (bx1 - bx0) * frac), by + self.px(10), self.px(5), color)
            if state:
                text = T['na'] if state == 'na' else T[state]
                color = {'failed': 'bad', 'done': 'ok', 'skip': 'ok', 'running': 'accent'}.get(state, 'dim')
                self.label(w - m, y, text, 10, color, anchor='ne')
            y += self.px(30)

        # status, the shortcut box, the buttons
        y += self.px(8)
        for line in self.text.wrap(self.status, 10.5, False, w - 2 * m)[:4]:
            self.label(m, y, line, 10.5, self.status_color)
            y += self.px(20)
        bottom = c.winfo_height() or self.px(self.H)
        by = bottom - m - self.px(40)
        box = self.px(20)
        bx, bxy = m, by + self.px(10)
        self.rect(bx, bxy, bx + box, bxy + box, self.px(4), COLORS['primary'] if self.shortcut else COLORS['track'])
        if self.shortcut:
            c.create_line(bx + box * .22, bxy + box * .52, bx + box * .43, bxy + box * .74, bx + box * .8, bxy + box * .28,
                          fill=COLORS['text'], width=max(2, self.px(2.5)))
        self.label(bx + box + self.px(10), bxy + box / 2, T['shortcut'], 10, anchor='w')
        self.buttons['shortcut'] = (bx, bxy, bx + box + self.px(260), bxy + box, True)

        built = (Path(self.out) / 'ocarina').is_file()
        playable = (Path(self.out) / 'play.sh').is_file()
        x = w - m
        x = self.button('play', x, by, T['play'], primary=playable and not self.proc, enabled=playable and not self.proc)
        main = T['confirm'] if self.confirm else T['cancel'] if self.proc else T['update'] if built else T['install']
        x = self.button('main', x - self.px(10), by, main, primary=not playable and not self.proc)
        self.button('details', x - self.px(10), by, T['details'], enabled=CACHE.joinpath('build.log').is_file() or bool(self.log))
        self.dirty = False

    # ---- input
    def hit(self, e):
        for key, (x0, y0, x1, y1, enabled) in self.buttons.items():
            if enabled and x0 <= e.x <= x1 and y0 <= e.y <= y1:
                return key
        return None

    def hover(self, e):
        key = self.hit(e)
        if key != self.hot:
            self.hot = key
            self.c.config(cursor='hand2' if key else '')
            self.mark()

    def click(self, e):
        key = self.hit(e)
        if key == 'pick_dump':
            f = pick_file(T['dump'], Path(self.nup).parent if self.nup else Path.home())
            if f:
                self.nup = f
        elif key == 'pick_out':
            d = pick_dir(T['folder'], Path(self.out).parent if self.out else Path.home())
            if d:
                self.out = d
        elif key == 'shortcut':
            self.shortcut = not self.shortcut
        elif key == 'details':
            self.details()
        elif key == 'play':
            self.play()
        elif key == 'main':
            self.main_button()
        if key != 'main':
            self.confirm = False
        if key:
            save_config({'nup': self.nup, 'out': self.out, 'shortcut': self.shortcut})
        self.mark()

    # ---- the build
    def main_button(self):
        if self.proc:
            if not self.confirm:
                self.confirm = True
                return
            self.confirm, self.cancelled = False, True
            self.kill()
            return
        if not self.nup or not Path(self.nup).is_file():
            self.status, self.status_color = T['nodump'], 'warn'
            return
        self.rows = {s: [0.0, ''] for s in STEPS}
        self.log, self.cancelled, self.failed, self.missing = [], False, None, False
        self.status, self.status_color = T['ready'], 'dim'
        self.proc = subprocess.Popen([str(self.apprun), '--events', self.nup, self.out], stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, errors='replace', bufsize=1,
                                     start_new_session=True)
        threading.Thread(target=self.read, args=(self.proc,), daemon=True).start()

    def kill(self):
        try:
            os.killpg(self.proc.pid, 15)
        except (OSError, AttributeError):
            pass

    def read(self, proc):
        for line in proc.stdout:
            self.events.put(line.rstrip('\n'))
        self.events.put(('exit', proc.wait()))

    def tick(self):
        try:
            while True:
                item = self.events.get_nowait()
                if isinstance(item, tuple):
                    self.finished(item[1])
                else:
                    self.line(item)
        except queue.Empty:
            pass
        if self.dirty:
            self.draw()
        self.root.after(100, self.tick)

    def line(self, text):
        if not text.startswith('@@'):
            self.log.append(text)
            del self.log[:-5000]
            if text.startswith('Missing:'):
                self.missing = True
            return
        try:
            ev = json.loads(text[2:])
        except ValueError:
            return
        kind, step = ev.get('ev'), ev.get('step')
        row = self.rows.get(step)
        if kind == 'step' and row is not None:
            state = ev.get('state')
            if state == 'start':
                row[1] = 'running'
            elif state == 'skip':
                row[0], row[1] = 1.0, 'na' if step == 'dlss' else 'skip'
            else:
                row[0], row[1] = 1.0, 'done'
        elif kind == 'progress' and row is not None:
            frac = float(ev['fraction']) if 'fraction' in ev else ev.get('done', 0) / max(ev.get('total', 1), 1)
            row[0] = max(0.0, min(1.0, frac))
            if ev.get('detail'):
                self.status, self.status_color = ev['detail'], 'dim'
        elif kind == 'error':
            self.failed = ev.get('message') or ''
            if row is not None:
                row[1] = 'failed'
        else:
            return
        self.mark()

    def finished(self, code):
        self.proc = None
        try:
            CACHE.mkdir(parents=True, exist_ok=True)
            CACHE.joinpath('build.log').write_text('\n'.join(self.log) + '\n', encoding='utf-8')
        except OSError:
            pass
        if self.cancelled:
            self.status, self.status_color = T['cancelled'], 'warn'
        elif code == 0:
            self.status, self.status_color = T['finished'], 'ok'
            if self.shortcut:
                self.add_shortcut()
        elif self.missing:
            self.status, self.status_color = T['missing'], 'bad'
        else:
            last = self.failed or next((l for l in reversed(self.log) if l.strip()), str(code))
            self.status, self.status_color = f"{T['stopped']} {last}", 'bad'
        self.mark()

    def details(self):
        log = CACHE / 'build.log'
        if self.log:   # the run in progress, as far as it went
            try:
                CACHE.mkdir(parents=True, exist_ok=True)
                log.write_text('\n'.join(self.log) + '\n', encoding='utf-8')
            except OSError:
                pass
        if log.is_file():
            subprocess.Popen(['xdg-open', str(log)], start_new_session=True, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)

    # ---- play and the menu entry
    def play(self):
        play = Path(self.out) / 'play.sh'
        if play.is_file():
            subprocess.Popen([str(play)], cwd=str(play.parent), start_new_session=True,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def add_shortcut(self):
        out = Path(self.out)
        try:
            icon = out / 'nx2navi.png'
            if self.icon and self.icon.is_file() and not icon.is_file():
                icon.write_bytes(self.icon.read_bytes())
            APPS.mkdir(parents=True, exist_ok=True)
            (APPS / 'nx2navi-ocarina.desktop').write_text(
                '[Desktop Entry]\nType=Application\nName=Ocarina of Time (NX2navi)\n'
                f'Exec={exec_quote(out / "play.sh")}\nPath={out}\nIcon={icon}\nTerminal=false\nCategories=Game;\n',
                encoding='utf-8')
        except OSError:
            pass

    def close(self):
        if self.proc and not self.close_armed:
            self.close_armed = True
            self.status, self.status_color = T['busy'], 'warn'
            self.mark()
            return
        if self.proc:
            self.kill()
        self.root.destroy()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--apprun', type=Path, required=True)
    ap.add_argument('--icon', type=Path)
    a = ap.parse_args()
    root = tk.Tk(className='nx2navi')
    App(root, a.apprun, a.icon)
    root.mainloop()


if __name__ == '__main__':
    main()
