"""Xbox, PlayStation, Steam Deck and keyboard-and-mouse button prompts for Ocarina of Time (Switch 2), drawn from button
art on the player's machine.

    python tools/pad_prompts.py --art FOLDER --game FOLDER [--style xbox|playstation|steamdeck|pc]
                                [--layout position|label]
                                [--binds nx2navi.ini]

--art is a folder of button art with the file names of STYLES (tools/prompt_art.py) and KEY_ART below: the Source
folder of a button prompt art pack, which holds XGamepad/Default, XGamepad/Light, P5Gamepad/Default, P5Gamepad/Light,
Keyboard_Mouse/Dark and Keyboard_Mouse/White (128x128 PNGs: a dark button or key with a white label, or a light one
with a dark or coloured label), or a release's prompt-art folder, which holds the same files (tools/make_game.py runs
this tool on it). The art is read where it lies and never enters the repository.
--game is the folder of the port's exe and romfs-index.bin. The RomFS is read from its romfs folder (index.json and
romfs.bin, as the setup writes them); --romfs names another, which may also be a folder of extracted files. Each pad
style and layout becomes the overlay folder <game>/romfs-patches-<style> (layout position) or
<game>/romfs-patches-<style>-label (layout label), and the keyboard and mouse <game>/romfs-patches-pc, each with a
manifest.tsv: runtime/romfs.c overlay_init loads it in place of the game's files when nx2navi.ini has prompts = xbox,
playstation, steamdeck or pc (runtime/input_map.c input_prompts_folder). Without --style or --layout, every style or
layout. The Steam Deck style has no art of its own: its buttons are named as an Xbox pad's face buttons and a
PlayStation pad's shoulders and stick clicks, so it is drawn from those two pads' pictures (prompt_art.py STYLES).
--out writes a single style and layout elsewhere.

The two pad layouts follow pad_layout (runtime/input_map.c). position, for pad_layout = nintendo: a pad is played by
position, so Switch A is the right face button, labelled B on an Xbox pad and circle on a PlayStation pad; the prompts
for A show that button (A as B or circle, B as A or cross, X as Y or triangle, Y as X or square). label, for
pad_layout = xbox, playstation and auto: Switch A is the bottom face button, so A shows A or cross, B shows B or
circle, X shows X or square and Y shows Y or triangle.
pc shows the first key each button has (PC_KEYS, Ocarina's defaults, with the bind_ lines of --binds over them; by
default <game>/nx2navi.ini), the mouse buttons as a mouse, and for the right stick the mouse moving. A key bound
later shows after the tool runs again.

What changes, the rest of each file staying as it is:
- Font2/Scalable/External.Nin_Ounce_NVN2.bfotf.zs, the button font every prompt in text uses (dialogue, the button
  guides, the controls screen): the glyphs of the buttons (U+E040 A, B, X, Y, L, R, ZL, ZR, the d-pad, Plus, Minus,
  and the sticks and d-pad with their directions) become outlines traced from the art (trace), at the size of the
  game's own face buttons; Power, Home, the console and the arrows stay. A .bfotf is a CFF OpenType font behind an
  8-byte header and an XOR (tools/button_icons.py bfotf_decode); fontTools writes it again.
- UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.bntx.zs, the shared interface textures: KeyTexFontA/B/X/Y_00,
  the letter of the action prompt (DoCommand_00; 64x64 BC4 masks the layout tints), and OcarinaButtonA/B/X/Y/L/R_00,
  the ocarina's note buttons (PaOcarinaSingleNote_00; 93x93 ASTC 4x4 sRGB, a white button with a dark letter). They are
  painted again in place, in their own format, size and block-linear layout, so the archive keeps its size.
- the resource size table (System/Resource/ResourceSizeTable.Product.<n>...rsizetable.zs): the font grows, and the
  game sizes the buffer it loads a file into from the table, so the font's entry is raised by as much
  (button_icons.grow_restbl).

Needs fontTools, Pillow and zstandard (tools/requirements-py314.txt; the launcher's Python has them,
tools/requirements-launcher.txt).
"""
import argparse
import hashlib
import io
import json
import struct
import sys
from pathlib import Path

import zstandard
from PIL import Image, ImageDraw, ImageFont
from fontTools.pens.boundsPen import BoundsPen
from fontTools.ttLib import TTFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
import button_icons  # noqa: E402  (bfotf_decode, bfotf_encode, redraw_font, grow_restbl)
from prompt_art import BLANKS, KM, OPTIONS, SHARE, STYLES, overlay_folder  # noqa: E402  (the art, the folders)

FONT = 'Font2/Scalable/External.Nin_Ounce_NVN2.bfotf.zs'
TEXTURES = 'UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.bntx.zs'
DICTIONARIES = 'Pack/ZsDic.pack.zs'

# The Switch button each code point of the button font shows (the stick and d-pad glyphs name their directions).
# U+E06A is the d-pad with three directions lit; the art has no such picture, so it shows the whole d-pad.
GLYPHS = {
    0xE040: 'A', 0xE041: 'B', 0xE042: 'X', 0xE043: 'Y', 0xE044: 'L', 0xE045: 'R', 0xE046: 'ZL', 0xE047: 'ZR',
    0xE049: 'DPAD', 0xE04B: 'PLUS', 0xE04C: 'MINUS',
    0xE050: 'LSTICK_DOWN', 0xE051: 'RSTICK_DOWN', 0xE052: 'LSTICK_UP', 0xE053: 'RSTICK_UP',
    0xE054: 'LSTICK_LEFTRIGHT', 0xE055: 'RSTICK_LEFTRIGHT', 0xE056: 'LSTICK_PRESS', 0xE057: 'RSTICK_PRESS',
    0xE058: 'LSTICK_RIGHT', 0xE059: 'RSTICK_RIGHT', 0xE05A: 'LSTICK_LEFT', 0xE05B: 'RSTICK_LEFT',
    0xE05C: 'LSTICK_UPDOWN', 0xE05D: 'RSTICK_UPDOWN', 0xE05E: 'LSTICK', 0xE05F: 'RSTICK',
    0xE060: 'DPAD_UP', 0xE061: 'DPAD_DOWN', 0xE062: 'DPAD_LEFT', 0xE063: 'DPAD_RIGHT',
    0xE064: 'DPAD_UPDOWN', 0xE065: 'DPAD_LEFTRIGHT', 0xE068: 'RSTICK_ROTATE', 0xE06A: 'DPAD',
}
# The face button that does each Switch face button, by its place on the pad.
FACE = {'position': {'A': 'east', 'B': 'south', 'X': 'north', 'Y': 'west'},
        'label': {'A': 'south', 'B': 'east', 'X': 'west', 'Y': 'north'}}


# The textures drawn again: the action prompt's letter masks and the ocarina's note buttons, by Switch button.
MASKS = {'KeyTexFontA_00': 'A', 'KeyTexFontB_00': 'B', 'KeyTexFontX_00': 'X', 'KeyTexFontY_00': 'Y'}
NOTES = {'OcarinaButtonA_00': 'A', 'OcarinaButtonB_00': 'B', 'OcarinaButtonX_00': 'X', 'OcarinaButtonY_00': 'Y',
         'OcarinaButtonL_00': 'L', 'OcarinaButtonR_00': 'R'}


def pad_pictures(style, layout):
    """(glyph, note) for a pad style: Switch button -> picture, the face buttons through FACE."""
    s = STYLES[style]
    glyph = {b: s['glyph'][FACE[layout].get(b, b)] for b in set(GLYPHS.values())}
    note = {b: s['note'][FACE[layout].get(b, b)] for b in NOTES.values()}
    return glyph, note


# ---- keyboard and mouse (prompts = pc): every button shows the first key its bindings give it. The defaults are
# runtime/input_map.c DEFAULT_KEYS; --binds takes the bind_ lines of a nx2navi.ini over them. The right stick is the
# camera, which the mouse turns, so its prompts show the mouse moving (its arrow keys, a second source, do not show).
PC_KEYS = {'A': 'e', 'B': 'lctrl', 'X': 'space', 'Y': 'mouse_l', 'L': 'q', 'R': 'r', 'ZL': 'mouse_r', 'ZR': 'f',
           'PLUS': 'tab', 'MINUS': 'm', 'LEFT': '1', 'UP': '2', 'RIGHT': '3', 'DOWN': '4', 'LCLICK': 'v', 'RCLICK': 'c',
           'LSTICK_UP': 'w', 'LSTICK_LEFT': 'a', 'LSTICK_DOWN': 's', 'LSTICK_RIGHT': 'd'}
# The art's name of each key (nx2navi.ini names, runtime/input_map.c key_names); letters, digits and F keys are their
# own. The art spells Ctrl as Crtl.
KEY_ART = {'space': 'Space', 'tab': 'Tab', 'lshift': 'Shift', 'rshift': 'Shift', 'lctrl': 'Crtl', 'rctrl': 'Crtl',
           'lalt': 'Alt', 'up': 'Up', 'down': 'Down', 'left': 'Left', 'right': 'Right', 'enter': 'Enter',
           'backspace': 'BackSpace', 'capslock': 'CapsLock', 'insert': 'Ins', 'delete': 'Del', 'home': 'Home',
           'end': 'End', 'pageup': 'PageUp', 'pagedown': 'PageDown', 'minus': 'Minus', 'num_sub': 'Minus',
           'num_add': 'Plus', 'num_mul': 'Asterisk', 'num_div': 'Slash', 'lbracket': 'Brackets_L',
           'rbracket': 'Brackets_R', 'semicolon': 'Semicolon', 'apostrophe': 'Quotation', 'slash': 'Slash',
           'grave': 'Tilde', 'mouse_l': 'Mouse_Left', 'mouse_r': 'Mouse_Right', 'mouse_m': 'Mouse_Middle'}
# What a blank key shows for a key the art has no picture of (label_art); any other shows its name.
KEY_LABELS = {'comma': ',', 'period': '.', 'equals': '=', 'num_decimal': '.', 'backslash': '\\', 'mouse_x1': 'M4',
              'mouse_x2': 'M5'}


def pc_keys(ini_text=None):
    """PC_KEYS with the first key of each bind_<button> line of a nx2navi.ini text over it. A line without a key
    entry, or with key:default first, keeps the default (the prompt then shows a key the line took away)."""
    keys = dict(PC_KEYS)
    for line in (ini_text or '').splitlines():
        line = line.split('#')[0].split(';')[0].strip()
        if not line.lower().startswith('bind_') or '=' not in line:
            continue
        button, value = (s.strip() for s in line.split('=', 1))
        entries = [e.strip()[4:].strip().lower() for e in value.split(',') if e.strip().lower().startswith('key:')]
        if entries and entries[0] != 'default' and button[5:].upper() in keys:
            keys[button[5:].upper()] = entries[0]
    return keys


def key_picture(art, key, shade):
    """The art of a key or mouse button in shade (Dark: a dark key with a white label; White: the other way round), or
    a blank key with its name drawn on it when the art has none. The art names its dark 4 key T_3_Key_Dark-1."""
    stem = KEY_ART.get(key)
    if not stem:
        digit = key[3:] if key.startswith('num') and key[3:].isdigit() else key
        stem = digit.upper() if len(digit) == 1 or (digit[0] == 'f' and digit[1:].isdigit()) else None
    for name in ([f'T_{stem}_Key_{shade}.png'] + ([f'T_3_Key_{shade}-1.png'] if stem == '4' else [])) if stem else []:
        if (art / KM / shade / name).is_file():
            return f'{KM}{shade}/{name}'
    return ('label', key, shade)


def pc_pictures(art, keys):
    """(glyph, note) for the keyboard and mouse: Switch button -> picture, by the key each button has. A d-pad or a
    stick with several directions shows their keys in a grid, as they sit on the keyboard."""
    def k(button, shade='Dark'):
        return key_picture(art, keys[button], shade)

    def grid(*cells):
        return ('grid', tuple(cells))

    def mouse(axes):
        return f'{KM}Dark/T_Mouse_{axes}_Key_Dark.png'
    glyph = {b: k(b) for b in ('A', 'B', 'X', 'Y', 'L', 'R', 'ZL', 'ZR', 'PLUS', 'MINUS')}
    glyph.update({
        'LSTICK_PRESS': k('LCLICK'), 'RSTICK_PRESS': k('RCLICK'),
        'DPAD': grid((k('UP'), 1, 0), (k('LEFT'), 0, 1), (k('DOWN'), 1, 1), (k('RIGHT'), 2, 1)),
        'DPAD_UP': k('UP'), 'DPAD_DOWN': k('DOWN'), 'DPAD_LEFT': k('LEFT'), 'DPAD_RIGHT': k('RIGHT'),
        'DPAD_UPDOWN': grid((k('UP'), 0, 0), (k('DOWN'), 0, 1)), 'DPAD_LEFTRIGHT': grid((k('LEFT'), 0, 0), (k('RIGHT'), 1, 0)),
        'LSTICK': grid((k('LSTICK_UP'), 1, 0), (k('LSTICK_LEFT'), 0, 1), (k('LSTICK_DOWN'), 1, 1),
                       (k('LSTICK_RIGHT'), 2, 1)),
        'LSTICK_UP': k('LSTICK_UP'), 'LSTICK_DOWN': k('LSTICK_DOWN'), 'LSTICK_LEFT': k('LSTICK_LEFT'),
        'LSTICK_RIGHT': k('LSTICK_RIGHT'), 'LSTICK_UPDOWN': grid((k('LSTICK_UP'), 0, 0), (k('LSTICK_DOWN'), 0, 1)),
        'LSTICK_LEFTRIGHT': grid((k('LSTICK_LEFT'), 0, 0), (k('LSTICK_RIGHT'), 1, 0)),
        'RSTICK': mouse('XY'), 'RSTICK_ROTATE': mouse('XY'), 'RSTICK_UP': mouse('Y'), 'RSTICK_DOWN': mouse('Y'),
        'RSTICK_UPDOWN': mouse('Y'), 'RSTICK_LEFT': mouse('X'), 'RSTICK_RIGHT': mouse('X'), 'RSTICK_LEFTRIGHT': mouse('X'),
    })
    note = {b: k(b, 'White') for b in NOTES.values()}
    return glyph, note


# ---- the RomFS: index.json and romfs.bin (each file's offset and size), or a folder of extracted files
class Romfs:
    def __init__(self, root):
        self.root = Path(root)
        index = self.root / 'index.json'
        self.files = None
        if index.is_file():
            data = json.loads(index.read_text())
            assert data.get('status') == 'pass', f'{index} is not a verified RomFS index'
            self.files = {f['path']: f for f in data['files']}
        self.dicts = None

    def raw(self, path):
        if self.files is None:
            return (self.root / path).read_bytes()
        f = self.files[path]
        with (self.root / 'romfs.bin').open('rb') as image:
            image.seek(f['offset'])
            data = image.read(f['size'])
        assert len(data) == f['size'], f'{path}: short read'
        return data

    def paths(self):
        if self.files is not None:
            return list(self.files)
        return [p.relative_to(self.root).as_posix() for p in self.root.rglob('*') if p.is_file()]

    def dictionary(self, dict_id):
        if self.dicts is None:
            self.dicts = {}
            pack = unpack(self.raw(DICTIONARIES))
            for lo, hi in sarc_members(pack).values():
                d = zstandard.ZstdCompressionDict(pack[lo:hi])
                self.dicts[d.dict_id()] = d
        return self.dicts[dict_id]

    def load(self, path):
        """(contents, dictionary or None) of a RomFS file, a .zs file decompressed with the dictionary its frame names."""
        raw = self.raw(path)
        if not path.endswith('.zs'):
            return raw, None
        dict_id = zstandard.get_frame_parameters(raw).dict_id
        d = self.dictionary(dict_id) if dict_id else None
        return unpack(raw, d), d


# How much of a packed file unpack gives the decompressor at a time.
PIECE = 1 << 16


def unpack(raw, dictionary=None):
    """The contents of a zstd frame, with the dictionary it was packed with; bytes after the frame are left out.

    The file goes to the decompressor in pieces of PIECE bytes and the pieces' outputs are joined once. Given the
    whole file in one call, the library returns one bytes object that it grows by copying as the output comes: made-up
    data of the size of the interface texture archive, 69 MB packed and 244 MB unpacked, took 21 s that way on a
    fast PC against 0.3 s in pieces. Players' builds waited 44 to 52 s before this tool's first folder, most of
    its minute."""
    out, parts = zstandard.ZstdDecompressor(dict_data=dictionary).decompressobj(), []
    for at in range(0, len(raw), PIECE):
        parts.append(out.decompress(raw[at:at + PIECE]))
        if out.eof:
            break
    return b''.join(parts)


def sarc_members(data):
    """name -> (absolute start, end) of every member of a SARC archive."""
    assert data[:4] == b'SARC'
    header, start = struct.unpack_from('<H', data, 4)[0], struct.unpack_from('<I', data, 12)[0]
    count = struct.unpack_from('<H', data, header + 6)[0]
    names, out = header + 12 + count * 16 + 8, {}
    for i in range(count):
        _, attribute, lo, hi = struct.unpack_from('<4I', data, header + 12 + 16 * i)
        at = names + (attribute & 0xffffff) * 4
        out[data[at:data.index(0, at)].decode()] = (start + lo, start + hi)
    return out


def compress(contents, dictionary, level):
    """A zstd frame of contents with the dictionary the original used, so the game reads it the same way."""
    return zstandard.ZstdCompressor(level=level, dict_data=dictionary, threads=-1).compress(contents)


# ---- art: a button as coverage, traced into outlines
def load_art(art, rel):
    """A picture as RGBA: a file under art, or one drawn from several (OPTIONS, ('grid', cells), ('label', key, shade))."""
    if rel == OPTIONS:
        return options_art(art)
    if isinstance(rel, tuple) and rel[0] == 'grid':
        return grid_art(art, rel[1])
    if isinstance(rel, tuple) and rel[0] == 'label':
        return label_art(art, *rel[1:])
    path = art / rel
    if not path.is_file():
        sys.exit(f'{path} missing: --art is the art pack\'s Source folder')
    return Image.open(path).convert('RGBA')


def opaque(img):
    return img.crop(img.getchannel('A').point(lambda a: 255 if a > 8 else 0).getbbox())


def grid_art(art, cells):
    """Keys side by side: cells are (picture, column, row), row 0 on top, each centred in a cell as large as the
    largest key, with a small gap between cells."""
    pics = [(opaque(load_art(art, rel)), c, r) for rel, c, r in cells]
    cw, ch, gap, pad = max(p.width for p, _, _ in pics), max(p.height for p, _, _ in pics), 6, 4
    cols, rows = max(c for _, c, _ in pics) + 1, max(r for _, _, r in pics) + 1
    canvas = Image.new('RGBA', (cols * cw + (cols - 1) * gap + 2 * pad, rows * ch + (rows - 1) * gap + 2 * pad))
    for p, c, r in pics:
        canvas.alpha_composite(p, (pad + c * (cw + gap) + (cw - p.width) // 2, pad + r * (ch + gap) + (ch - p.height) // 2))
    return canvas


def label_art(art, key, shade):
    """A key the art has no picture of: its blank key with the key's name written on it."""
    blank = load_art(art, BLANKS[shade])
    name = KEY_LABELS.get(key, key.upper())
    d = ImageDraw.Draw(blank)
    x0, y0, x1, y1 = blank.getchannel('A').point(lambda a: 255 if a > 127 else 0).getbbox()
    size = 48
    while size > 10:
        font = ImageFont.load_default(size)
        l, t, r, b = d.textbbox((0, 0), name, font=font)
        if r - l <= (x1 - x0) * 0.7 and b - t <= (y1 - y0) * 0.5:
            break
        size -= 2
    d.text(((x0 + x1 - (r - l)) / 2 - l, (y0 + y1 - (b - t)) / 2 - t), name, font=font,
           fill=(255, 255, 255, 255) if shade == 'Dark' else (40, 40, 40, 255))
    return blank


def options_art(art):
    """PlayStation's Options button as the art draws its Create button's neighbour on the pad: the dark pill of the
    share picture with three white bars, drawn four times larger and scaled down for smooth edges."""
    share = load_art(art, SHARE)
    x0, y0, x1, y1 = share.getchannel('A').point(lambda a: 255 if a > 127 else 0).getbbox()
    k = 4
    big = Image.new('RGBA', (share.width * k, share.height * k), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    d.rounded_rectangle((x0 * k, y0 * k, x1 * k - 1, y1 * k - 1), radius=(y1 - y0) * k // 2, fill=(42, 42, 42, 255))
    cx, cy, half, bar = (x0 + x1) * k / 2, (y0 + y1) * k / 2, (y1 - y0) * k * 0.32, (y1 - y0) * k * 0.05
    for dy in (-0.22, 0, 0.22):
        y = cy + dy * (y1 - y0) * k
        d.rounded_rectangle((cx - half, y - bar, cx + half, y + bar), radius=bar, fill=(255, 255, 255, 255))
    return big.convert('RGBa').resize(share.size, Image.LANCZOS).convert('RGBA')


LIT_AREA, LIT_RING = 200, 6   # pixels of the art


def ink(img, lit=False):
    """Coverage 0..1 per pixel (rows of floats) of a button drawn as a dark body with white symbols, for one-colour
    glyphs: the body counts, and so does white outside it (a stick's direction arrows, which touch the transparent
    background); white inside it (letters, symbols) is a hole.
    lit: a d-pad with its pressed directions in white. The art draws them as a white arm in a hairline outline (Xbox)
    or as a white arm the others' colour reversed (PlayStation); at text size the one vanishes and the other looks like
    the rest, so each white part of LIT_AREA pixels or more becomes a hollow arm, a LIT_RING-pixel band along its
    edge."""
    a, lum, near, parts = _parts(img)
    w, h = img.size
    field = [[a[y][x] * (1 - lum[y][x]) + (a[y][x] * lum[y][x] if near[y][x] else 0) for x in range(w)] for y in range(h)]
    for part in parts if lit else ():
        if len(part) < LIT_AREA:
            continue
        inside = set(part)
        dist = {p: 1 for p in part if any((p[0] + dx, p[1] + dy) not in inside
                                          for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))}
        front = list(dist)
        while front:
            nxt = []
            for x, y in front:
                for q in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                    if q in inside and q not in dist:
                        dist[q] = dist[x, y] + 1
                        nxt.append(q)
            front = nxt
        for x, y in part:
            field[y][x] = a[y][x] if dist[x, y] <= LIT_RING else 0.0
    return field


def symbol(img):
    """Coverage of the white letter or symbol inside a dark button (the action prompt's letter), as an L image: the
    white inside the body, the body's own grey left out."""
    a, lum, near, _ = _parts(img)
    out = Image.new('L', img.size)
    for y in range(img.height):
        for x in range(img.width):
            white = min(1.0, max(0.0, (lum[y][x] - 0.25) / 0.6))
            out.putpixel((x, y), 0 if near[y][x] else round(255 * a[y][x] * white))
    return out


def _parts(img):
    """Alpha and luminance (0..1) per pixel, whether each pixel is white outside the body or its soft edge, and the
    white parts (lists of (x, y))."""
    w, h = img.size
    px = img.load()
    a = [[px[x, y][3] / 255 for x in range(w)] for y in range(h)]
    lum = [[(0.299 * px[x, y][0] + 0.587 * px[x, y][1] + 0.114 * px[x, y][2]) / 255 for x in range(w)] for y in range(h)]
    white = [[a[y][x] > 0.5 and lum[y][x] > 0.5 for x in range(w)] for y in range(h)]
    # the background: transparent pixels reachable from the edge
    bg = [[False] * w for _ in range(h)]
    stack = [(x, y) for x in range(w) for y in (0, h - 1)] + [(x, y) for y in range(h) for x in (0, w - 1)]
    while stack:
        x, y = stack.pop()
        if 0 <= x < w and 0 <= y < h and not bg[y][x] and a[y][x] <= 0.5:
            bg[y][x] = True
            stack += [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]
    # each white part: outside the body when a quarter of its edge touches the background. A stick's arrows touch it
    # all round; a lit d-pad direction inside a thin dark outline touches it only where the outline thins at a rounded
    # corner, and stays a hole.
    outside = [[False] * w for _ in range(h)]
    seen = [[False] * w for _ in range(h)]
    parts = []
    for sy in range(h):
        for sx in range(w):
            if not white[sy][sx] or seen[sy][sx]:
                continue
            part, stack, edge, touching = [], [(sx, sy)], 0, 0
            seen[sy][sx] = True
            while stack:
                x, y = stack.pop()
                part.append((x, y))
                if not all(0 <= nx < w and 0 <= ny < h and white[ny][nx]
                           for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1))):
                    edge += 1
                    touching += any(bg[yy][xx] for yy in range(max(0, y - 1), min(h, y + 2))
                                    for xx in range(max(0, x - 1), min(w, x + 2)))
                for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                    if 0 <= nx < w and 0 <= ny < h and white[ny][nx] and not seen[ny][nx]:
                        seen[ny][nx] = True
                        stack.append((nx, ny))
            parts.append(part)
            if touching * 4 >= edge:
                for x, y in part:
                    outside[y][x] = True
    # the soft edge of an outside part (antialiased pixels next to it) counts with it
    near = [[outside[y][x] or any(outside[yy][xx] for yy in range(max(0, y - 1), min(h, y + 2))
                                  for xx in range(max(0, x - 1), min(w, x + 2))) for x in range(w)] for y in range(h)]
    return a, lum, near, parts


def trace(field, level=0.5):
    """Closed outlines (lists of (x, y) in pixels, y down) where field crosses level, by marching squares with the
    crossings placed linearly along the cell edges. Each outline has the covered side on its left once y points up
    (as in a font): outer outlines run counter-clockwise and holes clockwise, as a CFF font wants them."""
    h, w = len(field), len(field[0])

    def v(x, y):   # one pixel of zero around the field, so every outline closes
        return field[y - 1][x - 1] if 1 <= x <= w and 1 <= y <= h else 0.0

    def cross(p, q):
        (x0, y0), (x1, y1) = p, q
        a, b = v(x0, y0), v(x1, y1)
        t = (level - a) / (b - a)
        return (x0 + (x1 - x0) * t - 1, y0 + (y1 - y0) * t - 1)   # back to field coordinates

    starts = {}   # edge -> (start point, end edge, end point) of the segment leaving the cell through it
    for y in range(h + 1):
        for x in range(w + 1):
            corners = [(x, y), (x + 1, y), (x + 1, y + 1), (x, y + 1)]   # clockwise with y down
            inside = [v(*c) >= level for c in corners]
            if all(inside) or not any(inside):
                continue
            # the crossings met going round the cell clockwise: (edge key, point, entering the covered part)
            hits = []
            for i in range(4):
                p, q = corners[i], corners[(i + 1) % 4]
                if inside[i] != inside[(i + 1) % 4]:
                    hits.append((tuple(sorted((p, q))), cross(p, q), inside[(i + 1) % 4]))
            # each segment joins the crossing entering a covered part to the one leaving it, so that going round the
            # cell clockwise (y down) the covered part lies between them
            if len(hits) == 2:
                enter, leave = (hits[0], hits[1]) if hits[0][2] else (hits[1], hits[0])
                pairs = [(enter, leave)]
            else:   # a saddle: the middle decides whether the covered corners join
                order = hits if hits[0][2] else hits[1:] + hits[:1]   # enter, leave, enter, leave
                e1, l1, e2, l2 = order
                joined = (v(x, y) + v(x + 1, y) + v(x + 1, y + 1) + v(x, y + 1)) / 4 >= level
                pairs = [(e2, l1), (e1, l2)] if joined else [(e1, l1), (e2, l2)]
            for (sk, sp, _), (ek, ep, _) in pairs:
                starts[sk] = (sp, ek, ep)
    outlines = []
    while starts:
        key, (point, end, _) = next(iter(starts.items()))
        loop = []
        while key in starts:
            point, end, _ = starts.pop(key)
            loop.append(point)
            key = end
        if len(loop) >= 3:
            outlines.append(loop)
    return outlines


def simplify(loop, tolerance):
    """A closed outline with the points that stray less than tolerance from a straight line dropped (Douglas-Peucker on
    the two halves between the first point and the one farthest from it)."""
    def dp(points):
        if len(points) < 3:
            return points
        (ax, ay), (bx, by) = points[0], points[-1]
        dx, dy = bx - ax, by - ay
        norm = (dx * dx + dy * dy) ** 0.5
        far, at = -1.0, 0
        for i in range(1, len(points) - 1):
            px, py = points[i]
            d = abs(dy * (px - ax) - dx * (py - ay)) / norm if norm else ((px - ax) ** 2 + (py - ay) ** 2) ** 0.5
            if d > far:
                far, at = d, i
        if far <= tolerance:
            return [points[0], points[-1]]
        return dp(points[:at + 1])[:-1] + dp(points[at:])
    x0, y0 = loop[0]
    k = max(range(len(loop)), key=lambda i: (loop[i][0] - x0) ** 2 + (loop[i][1] - y0) ** 2)
    out = dp(loop[:k + 1])[:-1] + dp(loop[k:] + loop[:1])[:-1]
    return out if len(out) >= 3 else loop


def area(loop):
    return sum(x0 * y1 - x1 * y0 for (x0, y0), (x1, y1) in zip(loop, loop[1:] + loop[:1])) / 2


def outlines_of(img, lit=False):
    """The traced, simplified outlines of a dark-style button picture, specks dropped (lit: as ink)."""
    return [s for s in (simplify(o, 0.3) for o in trace(ink(img, lit))) if abs(area(s)) >= 1.5]


def bounds(outlines):
    xs = [x for o in outlines for x, _ in o]
    ys = [y for o in outlines for _, y in o]
    return min(xs), min(ys), max(xs), max(ys)


# ---- the button font
def draw_font(otf, glyph, traced, lit_dpad):
    """The button font with the glyphs of GLYPHS drawn from glyph (Switch button -> picture). Every picture is scaled
    as A's picture is to the game's A (both fill their square the same way), centred in its advance and placed at the
    height of the art's own square, so the arrows of a stick reach out as they do in the art. A grid of keys is made
    no taller than the game's A and centred on it. A wider picture widens its glyph. lit_dpad: the d-pad directions
    are lit pad pictures (ink lit)."""
    font = TTFont(io.BytesIO(otf))
    glyphs, cmap, hmtx = font.getGlyphSet(), font.getBestCmap(), font['hmtx']
    box = BoundsPen(glyphs)
    glyphs[cmap[0xE040]].draw(box)
    gx0, gy0, gx1, gy1 = box.bounds   # the game's A: a round button
    rx0, ry0, rx1, ry1 = bounds(traced(glyph['A']))
    scale = (gy1 - gy0) / (ry1 - ry0)
    mid_y_px, mid_y = (ry0 + ry1) / 2, (gy0 + gy1) / 2
    side = gx0   # the game's A has this much room on either side
    drawn = {}
    for cp, button in GLYPHS.items():
        pic = glyph[button]
        outlines = traced(pic, lit_dpad and button.startswith('DPAD_'))
        x0, y0, x1, y1 = bounds(outlines)
        s, cy = scale, mid_y_px
        if isinstance(pic, tuple) and pic[0] == 'grid':
            s, cy = min(scale, (gy1 - gy0) / (y1 - y0)), (y0 + y1) / 2
        advance = max(hmtx[cmap[cp]][0], round((x1 - x0) * s + 2 * side))
        ox = advance / 2 - (x0 + x1) / 2 * s

        def draw(pen, outlines=outlines, ox=ox, s=s, cy=cy):
            for o in outlines:
                pen.moveTo((ox + o[0][0] * s, mid_y - (o[0][1] - cy) * s))
                for x, y in o[1:]:
                    pen.lineTo((ox + x * s, mid_y - (y - cy) * s))
                pen.closePath()
        drawn[cp] = (advance, draw)
    return button_icons.redraw_font(otf, drawn)


# ---- BNTX textures: block-linear in the Switch 2 GOB layout (runtime/tex_deswizzle.c, nvn_bl_offset)
def bntx_textures(data):
    """name -> (format, width, height, block height (log2 GOBs), level offset, level size) of every one-level 2D
    texture of a BNTX."""
    assert data[:4] == b'BNTX'
    count, table = struct.unpack_from('<IQ', data, 0x24)
    out = {}
    for k in range(count):
        info = struct.unpack_from('<Q', data, table + 8 * k)[0]
        assert data[info:info + 4] == b'BRTI'
        at = struct.unpack_from('<Q', data, info + 0x60)[0]
        name = data[at + 2:at + 2 + struct.unpack_from('<H', data, at)[0]].decode()
        mips = struct.unpack_from('<H', data, info + 0x16)[0]
        fmt, (w, h), th = struct.unpack_from('<I', data, info + 0x1c)[0], struct.unpack_from('<II', data, info + 0x24), \
            struct.unpack_from('<I', data, info + 0x34)[0]
        start = struct.unpack_from('<Q', data, struct.unpack_from('<Q', data, info + 0x70)[0])[0]
        out[name] = dict(fmt=fmt, w=w, h=h, th=th, mips=mips, start=start, size=struct.unpack_from('<I', data, info + 0x50)[0])
    return out


def level_tiles(th, rows):
    """The block height of a level rows block-rows high (runtime/nvn_tex.c adj_tile): halved while it is too tall."""
    x = 8 << (th - 1) if th else 0
    while th and x >= rows:
        th -= 1
        x >>= 1
    return th


def block_linear(row_bytes, rows, th):
    """Offset of each 16-byte piece (row, piece) of a level, in the Switch 2 GOB order."""
    th = level_tiles(th, rows)
    unit, block, gobs = 8 << th, 512 << th, (row_bytes + 63) // 64
    return lambda x, y: ((y // unit) * gobs * block + (x // 64) * block + ((y % unit) // 8) * 512 + (x % 64 // 32) * 256
                         + (y % 8 // 4) * 128 + (x % 32 // 16) * 64 + (y % 4) * 16)


def read_level(data, t, row_bytes, rows):
    at = block_linear(row_bytes, rows, t['th'])
    out = bytearray(row_bytes * rows)
    for y in range(rows):
        for x in range(0, row_bytes, 16):
            o = t['start'] + at(x, y)
            out[y * row_bytes + x:y * row_bytes + x + 16] = data[o:o + 16]
    return out


def write_level(data, t, lin, row_bytes, rows):
    at = block_linear(row_bytes, rows, t['th'])
    for y in range(rows):
        for x in range(0, row_bytes, 16):
            o = at(x, y)
            assert o + 16 <= t['size'], 'past the end of the texture'
            data[t['start'] + o:t['start'] + o + 16] = lin[y * row_bytes + x:y * row_bytes + x + 16]


BC4_UNORM, ASTC_4X4_SRGB = 0x1d01, 0x2d06


def bc4_decode(lin, w, h):
    out = Image.new('L', (w, h))
    wb = (w + 3) // 4
    for by in range((h + 3) // 4):
        for bx in range(wb):
            b = lin[(by * wb + bx) * 8:(by * wb + bx) * 8 + 8]
            pal = bc4_palette(b[0], b[1])
            bits = int.from_bytes(b[2:8], 'little')
            for i in range(16):
                x, y = bx * 4 + i % 4, by * 4 + i // 4
                if x < w and y < h:
                    out.putpixel((x, y), pal[bits >> (3 * i) & 7])
    return out


def bc4_palette(a, b):
    if a > b:
        return [a, b] + [((6 - i) * a + (i + 1) * b) // 7 for i in range(6)]
    return [a, b] + [((4 - i) * a + (i + 1) * b) // 5 for i in range(4)] + [0, 255]


def bc4_encode(img):
    """BC4 blocks of an L image: each block's lowest and highest value as the ends, every pixel the nearest of the
    eight levels between them."""
    w, h = img.size
    px = img.load()
    out = bytearray()
    for by in range((h + 3) // 4):
        for bx in range((w + 3) // 4):
            vals = [px[min(w - 1, bx * 4 + i % 4), min(h - 1, by * 4 + i // 4)] for i in range(16)]
            hi, lo = max(vals), min(vals)
            if hi == lo:
                out += bytes([hi, lo]) + bytes(6)
                continue
            pal = bc4_palette(hi, lo)
            bits = 0
            for i, v in enumerate(vals):
                bits |= min(range(8), key=lambda k: abs(pal[k] - v)) << (3 * i)
            out += bytes([hi, lo]) + bits.to_bytes(6, 'little')
    return bytes(out)


# ASTC 4x4, one partition, endpoint mode 12 (RGBA, direct), 8-bit endpoints and a 4x4 grid of 2-bit weights: 11 bits
# of block mode (0x042: weights 0..3, grid 4x4), 2 of partition count, 4 of endpoint mode, then the eight endpoint
# bytes from bit 17 (r0 r1 g0 g1 b0 b1 a0 a1) and the 32 weight bits from bit 127 down. The 79 bits left after the
# mode and the weights hold eight values of 0..255 exactly, so the decoder reads the endpoints as plain bytes.
ASTC_WEIGHTS = (0, 21, 43, 64)


def astc_block(texels):
    """One 128-bit block of 16 RGBA texels: the two ends of the colours' main direction, every texel its nearest of
    the four steps between them."""
    n = len(texels)
    mean = [sum(t[c] for t in texels) / n for c in range(4)]
    cov = [[sum((t[i] - mean[i]) * (t[j] - mean[j]) for t in texels) for j in range(4)] for i in range(4)]
    axis = [1.0, 1.0, 1.0, 1.0]
    for _ in range(12):
        nxt = [sum(cov[i][j] * axis[j] for j in range(4)) for i in range(4)]
        norm = sum(v * v for v in nxt) ** 0.5
        if norm < 1e-9:
            break
        axis = [v / norm for v in nxt]
    proj = [sum((t[c] - mean[c]) * axis[c] for c in range(4)) for t in texels]
    lo, hi = min(proj), max(proj)
    e0 = [max(0, min(255, round(mean[c] + lo * axis[c]))) for c in range(4)]
    e1 = [max(0, min(255, round(mean[c] + hi * axis[c]))) for c in range(4)]
    if sum(e1[:3]) < sum(e0[:3]):   # the decoder would read the ends the other way (blue contraction)
        e0, e1 = e1, e0
    span = [e1[c] - e0[c] for c in range(4)]
    length = sum(v * v for v in span)
    weights = []
    for t in texels:
        f = sum((t[c] - e0[c]) * span[c] for c in range(4)) / length if length else 0.0
        weights.append(min(range(4), key=lambda k: abs(ASTC_WEIGHTS[k] / 64 - f)))
    bits = 0x042 | (12 << 13)
    for i, v in enumerate((e0[0], e1[0], e0[1], e1[1], e0[2], e1[2], e0[3], e1[3])):
        bits |= v << (17 + 8 * i)
    for i, wt in enumerate(weights):
        for b in range(2):
            if wt >> b & 1:
                bits |= 1 << (127 - (2 * i + b))
    return bits.to_bytes(16, 'little')


def astc_encode(img):
    w, h = img.size
    px = img.load()
    out = bytearray()
    for by in range((h + 3) // 4):
        for bx in range((w + 3) // 4):
            out += astc_block([px[min(w - 1, bx * 4 + i % 4), min(h - 1, by * 4 + i // 4)] for i in range(16)])
    return bytes(out)


def fit(img, box, size):
    """img's opaque part scaled to fit box (x0, y0, x1, y1) and centred in it, on a transparent canvas of size;
    resampled with premultiplied alpha, so the edges keep their colour."""
    crop = img.crop(img.getchannel('A').point(lambda a: 255 if a > 8 else 0).getbbox())
    bw, bh = box[2] - box[0], box[3] - box[1]
    k = min(bw / crop.width, bh / crop.height)
    scaled = crop.convert('RGBa').resize((max(1, round(crop.width * k)), max(1, round(crop.height * k))), Image.LANCZOS)
    canvas = Image.new('RGBa', size, (0, 0, 0, 0))
    canvas.paste(scaled, (round(box[0] + (bw - scaled.width) / 2), round(box[1] + (bh - scaled.height) / 2)))
    return canvas.convert('RGBA')


def bleed(img):
    """The colour of the nearest covered pixel spread under transparent ones, so a compressed block that holds both
    ends in the button's own colour rather than in black."""
    w, h = img.size
    px = img.load()
    known = {(x, y) for y in range(h) for x in range(w) if px[x, y][3] > 8}
    for _ in range(6):
        grown = {}
        for y in range(h):
            for x in range(w):
                if (x, y) in known:
                    continue
                near = [px[x + dx, y + dy] for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))
                        if 0 <= x + dx < w and 0 <= y + dy < h and (x + dx, y + dy) in known]
                if near:
                    grown[x, y] = tuple(sum(p[c] for p in near) // len(near) for c in range(3)) + (px[x, y][3],)
        for xy, p in grown.items():
            px[xy] = p
        known |= set(grown)
    return img


def mask_of(img, rel):
    """The action prompt's mask of a dark picture: the white letter or symbol inside it, or for a mouse, whose white is
    the pressed button rather than a label, the mouse itself with that button cut out."""
    if '/T_Mouse_' not in str(rel):
        return symbol(img)
    field = ink(img)
    out = Image.new('L', img.size)
    out.putdata([round(255 * min(1.0, v)) for row in field for v in row])
    return out


def note_texture(art, rel, box):
    pic = fit(load_art(art, rel), box, (93, 93))
    return astc_encode(bleed(pic))


def draw_textures(data, glyph, note, art, cache):
    """Paints the action prompt's letters (from glyph) and the ocarina's note buttons (from note; both Switch button ->
    picture) into the BNTX data (a bytearray) in place. A new letter takes the height and centre of the game's own
    letter in that mask (a wider one may use a little more width); a note button fills NOTE_BOXES."""
    textures = bntx_textures(data)
    for name, button in {**MASKS, **NOTES}.items():
        assert name in textures, f'no {name} in {TEXTURES}'
        t = textures[name]
        assert t['mips'] == 1, f'{name}: {t["mips"]} levels'
        wb, hb = (t['w'] + 3) // 4, (t['h'] + 3) // 4
        if name in MASKS:
            assert t['fmt'] == BC4_UNORM, f'{name}: format {t["fmt"]:#x}'
            old = bc4_decode(read_level(data, t, wb * 8, hb), t['w'], t['h'])
            box = old.point(lambda v: 255 if v > 64 else 0).getbbox()
            # the old letter's height; a wider new symbol may use a little more width
            cx, cy, half = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2, (box[3] - box[1]) / 2
            rel = glyph[button]
            sym = cache(('mask', rel), lambda: mask_of(load_art(art, rel), rel))
            rgba = Image.merge('RGBA', (sym.point(lambda v: 255),) * 3 + (sym,))
            mask = fit(rgba, (cx - half * 1.15, cy - half, cx + half * 1.15, cy + half), (t['w'], t['h']))
            write_level(data, t, bc4_encode(mask.getchannel('A')), wb * 8, hb)
        else:
            assert t['fmt'] == ASTC_4X4_SRGB and (t['w'], t['h']) == (93, 93), f'{name}: format {t["fmt"]:#x}, ' \
                f'{t["w"]}x{t["h"]}'
            box = NOTE_BOXES['shoulder' if button in 'LR' else 'disc']
            rel = note[button]
            encoded = cache(('note', rel, box), lambda: note_texture(art, rel, box))
            write_level(data, t, encoded, wb * 16, hb)


# Where the game's own note buttons are opaque in their 93x93 textures (1.1.1), which the new ones fill: the face
# buttons a disc, L and R a wider shape. Measured once with the runtime's ASTC decoder (tools/ui_textures.py), so this
# tool needs none.
NOTE_BOXES = {'disc': (13, 13, 80, 80), 'shoulder': (8, 15, 84, 76)}


# ---- the overlay folder
def write_overlay(out, outputs, originals):
    """outputs: RomFS path -> new file bytes; originals: RomFS path -> the original's bytes. Writes each file under out
    and the manifest runtime/romfs.c overlay_load checks: path, size, SHA-256, the original's SHA-256."""
    lines = []
    for path, data in outputs.items():
        target = out / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        lines.append('\t'.join([path, str(len(data)), hashlib.sha256(data).hexdigest(),
                                hashlib.sha256(originals[path]).hexdigest()]))
    (out / 'manifest.tsv').write_text('# path\tsize\tsha256\toriginal_sha256 (tools/pad_prompts.py)\n'
                                      + ''.join(line + '\n' for line in lines))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--art', type=Path, required=True,
                    help='the art pack\'s Source folder (XGamepad, P5Gamepad, Keyboard_Mouse)')
    ap.add_argument('--game', type=Path, help='the folder of the exe and romfs-index.bin; the overlays go there')
    ap.add_argument('--romfs', type=Path, help='default <game>/romfs')
    ap.add_argument('--style', choices=sorted(STYLES) + ['pc'], help='default: every one')
    ap.add_argument('--layout', choices=sorted(FACE), help='default: both (pc has one)')
    ap.add_argument('--binds', type=Path, help='pc: a nx2navi.ini whose bind_ lines the keys follow '
                                               '(default <game>/nx2navi.ini when there is one)')
    ap.add_argument('--out', type=Path, help='the overlay folder, for one --style and --layout')
    ap.add_argument('--level', type=int, default=9, help='zstd level of the written files')
    args = ap.parse_args()
    if not args.game and not (args.out and args.romfs):
        ap.error('--game, or --out with --romfs')
    if args.out and not (args.style and (args.layout or args.style == 'pc')):
        ap.error('--out writes one folder: give --style and --layout')
    binds = args.binds or (args.game / 'nx2navi.ini' if args.game and (args.game / 'nx2navi.ini').is_file() else None)
    keys = pc_keys(binds.read_text(encoding='utf-8', errors='replace') if binds else None)
    romfs = Romfs(args.romfs or args.game / 'romfs')
    restbl = max((p for p in romfs.paths() if p.startswith('System/Resource/ResourceSizeTable.') and p.endswith('.zs')),
                 key=lambda p: int(p.split('.')[2]) if p.split('.')[2].isdigit() else 0, default=None)
    assert restbl, 'no resource size table in the RomFS'
    originals = {p: romfs.raw(p) for p in (FONT, TEXTURES, restbl)}
    font_blob, font_dict = romfs.load(FONT)
    otf, key = button_icons.bfotf_decode(font_blob)
    table, table_dict = romfs.load(restbl)
    textures, textures_dict = romfs.load(TEXTURES)
    memo = {}

    def cache(k, make):
        if k not in memo:
            memo[k] = make()
        return memo[k]

    def traced(rel, lit=False):
        return cache(('outlines', rel, lit), lambda: outlines_of(load_art(args.art, rel), lit))

    report = []
    for style in [args.style] if args.style else sorted(STYLES) + ['pc']:
        # the keyboard follows no pad layout: one folder, romfs-patches-pc
        for layout in [None] if style == 'pc' else [args.layout] if args.layout else sorted(FACE):
            out = args.out or args.game / overlay_folder(style, layout)
            glyph, note = pc_pictures(args.art, keys) if style == 'pc' else pad_pictures(style, layout)
            new_otf = draw_font(otf, glyph, traced, style != 'pc')
            blob = button_icons.bfotf_encode(new_otf, key, font_blob)
            data = bytearray(textures)
            draw_textures(data, glyph, note, args.art, cache)
            assert len(data) == len(textures)
            outputs = {FONT: compress(blob, font_dict, 19), TEXTURES: compress(bytes(data), textures_dict, args.level)}
            if len(blob) > len(font_blob):
                outputs[restbl] = compress(button_icons.grow_restbl(table, {FONT: len(blob) - len(font_blob)}),
                                           table_dict, 19)
            write_overlay(out, outputs, originals)
            report.append(dict(folder=str(out), style=style, layout=layout, font_bytes=len(blob),
                               font_grew=len(blob) - len(font_blob), files=sorted(outputs)))
            print(f'{out}: {len(outputs)} files, the font grew by {len(blob) - len(font_blob)} bytes', flush=True)
    print(json.dumps(dict(status='pass', folders=report)))


if __name__ == '__main__':
    main()
