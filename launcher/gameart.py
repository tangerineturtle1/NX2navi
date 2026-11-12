"""The pictures of the game's Steam shortcut (launcher/steam.py), made from the player's own game: nothing ships.

    icon        Navi as the game's HUD draws her (HUDNaviIcon_Body_00 over the glow HUDNaviIcon_Flare_00), 128 x 128
    logo        the title screen's logo with the Master Sword and the Hylian Shield (Logo_USEU_00), cut to its outline
    hero        the title screen's moonlit field, a frame of its first movie (Event/Movie/Dm_Title_0005.webm), 1920 x 620
    portrait    that frame with the logo over it, 600 x 900 (the library's cover)
    wide        the same at 920 x 430 (the library's wide tile)

The textures come from the interface archive UI/Layout2Archive/Title...bntx.zs as runtime/menu_assets.c reads them:
a BNTX's BRTI records give format, size, block height and data; the level is block linear in the Switch 2 GOB layout
(runtime/tex_deswizzle.c tex_bl_copy); BC4 masks become white with the mask as alpha; ASTC 4x4 goes through the
runtime's own decoder, runtime/tex_astc.c, built here into a library with the toolchain's clang (the launcher builds
the game with it, so it is on every PC that has a game to add). The movie is WebM with AV1 video: the frames from the
key frame before TITLE_AT up to it go into an IVF file for the toolchain's dav1d.exe, whose last picture (8-bit 4:2:0,
limited range BT.709) becomes the background. Every picture is written as PNG with art.png_bytes; nothing here needs
an imaging package.
"""
import ctypes
import hashlib
import os
import struct
import subprocess
import tempfile
from pathlib import Path

import numpy as np

from . import gamefiles

TITLE_ARCHIVE = 'UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.bntx.zs'
COMMON_ARCHIVE = 'UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.bntx.zs'
LOGO, NAVI, NAVI_GLOW = 'Logo_USEU_00', 'HUDNaviIcon_Body_00', 'HUDNaviIcon_Flare_00'
TITLE_MOVIE = 'Event/Movie/Dm_Title_0005.webm'
TITLE_AT = 12.0     # seconds into it: the field under the full moon, as the title screen shows it first
KIND_BC4, KIND_ASTC4 = 0x1d, 0x2d
NO_WINDOW = 0x08000000 if os.name == 'nt' else 0


class ArtError(Exception):
    pass


# ---------------------------------------------------------------- BNTX

def _u16(b, at): return struct.unpack_from('<H', b, at)[0]
def _u32(b, at): return struct.unpack_from('<I', b, at)[0]
def _u64(b, at): return struct.unpack_from('<Q', b, at)[0]


def bntx_textures(b):
    """Each texture of a BNTX file: name, format, size, block height, data offset and size."""
    out = []
    count, arr = _u32(b, 0x24), _u64(b, 0x28)
    for i in range(count):
        p = _u64(b, arr + 8 * i)
        if b[p:p + 4] != b'BRTI':
            continue
        no = _u64(b, p + 0x60)
        name = b[no + 2:no + 2 + _u16(b, no)].decode('utf-8', 'replace')
        out.append({'name': name, 'fmt': _u32(b, p + 0x1c), 'w': _u32(b, p + 0x24), 'h': _u32(b, p + 0x28),
                    'th': _u32(b, p + 0x34), 'size': _u32(b, p + 0x50), 'data': _u64(b, _u64(b, p + 0x70))})
    return out


def deswizzle(t, b, row, rows):
    """The first level as tightly packed rows of `row` bytes (runtime/tex_deswizzle.c tex_bl_copy)."""
    th = t['th']
    gobs, unit, block = (row + 63) // 64, 8 << th, 512 << th
    need = gobs * ((rows + unit - 1) // unit) * block
    src = bytearray(max(need, t['size']) + 16)
    src[:t['size']] = b[t['data']:t['data'] + t['size']]
    bl = np.frombuffer(bytes(src), dtype='V16')
    cols = (row + 15) // 16
    y = np.arange(rows)[:, None]
    x = (np.arange(cols) * 16)[None, :]
    at = (y // unit) * gobs * block + (x // 64) * block + ((y % unit) // 8) * 512 + \
        (x % 64 // 32) * 256 + (y % 8 // 4) * 128 + (x % 32 // 16) * 64 + (y % 4) * 16
    flat = np.frombuffer(bl[at // 16].tobytes(), dtype=np.uint8).reshape(rows, cols * 16)
    return flat[:, :row]


def bc4(lin, wb, hb, w, h):
    """White with the mask as alpha (runtime/menu_assets.c bc4_decode)."""
    blocks = lin.reshape(hb, wb, 8).astype(np.uint32)
    a, b = blocks[..., 0], blocks[..., 1]
    pal = np.zeros((hb, wb, 8), dtype=np.uint32)
    pal[..., 0], pal[..., 1] = a, b
    big = a > b
    for i in range(6):
        pal[..., 2 + i] = np.where(big, ((6 - i) * a + (i + 1) * b) // 7, 0)
    for i in range(4):
        pal[..., 2 + i] = np.where(big, pal[..., 2 + i], ((4 - i) * a + (i + 1) * b) // 5)
    pal[..., 6] = np.where(big, pal[..., 6], 0)
    pal[..., 7] = np.where(big, pal[..., 7], 255)
    bits = np.zeros((hb, wb), dtype=np.uint64)
    for i in range(6):
        bits |= blocks[..., 2 + i].astype(np.uint64) << np.uint64(8 * i)
    alpha = np.zeros((hb * 4, wb * 4), dtype=np.uint8)
    for i in range(16):
        sel = ((bits >> np.uint64(3 * i)) & np.uint64(7)).astype(np.intp)
        alpha[i // 4::4, i % 4::4] = np.take_along_axis(pal, sel[..., None], 2)[..., 0]
    rgba = np.full((h, w, 4), 255, dtype=np.uint8)
    rgba[..., 3] = alpha[:h, :w]
    return rgba


def astc_library(layout):
    """runtime/tex_astc.c of this release as a library in data/cache, built once per source with the toolchain."""
    src = layout.app / 'runtime' / 'tex_astc.c'
    if not src.is_file():
        raise ArtError(f'{src} is missing')
    tag = hashlib.sha256(src.read_bytes() + (layout.app / 'runtime' / 'tex.h').read_bytes()).hexdigest()[:12]
    dll = layout.cache / f'astc-{tag}.dll'
    if not dll.is_file():
        clang = layout.msys / 'bin' / 'clang.exe'
        if not clang.is_file():
            raise ArtError('the build tools are not installed (Rebuild the Game installs them)')
        env = dict(os.environ, PATH=str(layout.msys / 'bin') + os.pathsep + os.environ.get('PATH', ''))
        tmp = dll.with_name(dll.name + '.tmp')
        r = subprocess.run([str(clang), '-shared', '-O2', '-static', '-w', '-I', str(layout.app / 'runtime'),
                            '-I', str(layout.msys / 'include' / 'vma'), str(src), '-o', str(tmp)],
                           capture_output=True, text=True, env=env, creationflags=NO_WINDOW)
        if r.returncode or not tmp.is_file():
            raise ArtError(f'the texture decoder did not build: {(r.stderr or r.stdout).strip()[-300:]}')
        os.replace(tmp, dll)
    lib = ctypes.CDLL(str(dll))
    lib.astc_decode_ldr.restype = None
    lib.astc_decode_ldr.argtypes = [ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
                                    ctypes.c_uint32, ctypes.c_bool, ctypes.c_void_p]
    return lib


def texture(bntx, name, lib):
    """The named texture as an HxWx4 uint8 array (BC4 or ASTC 4x4), or None when the file has no such texture."""
    for t in bntx_textures(bntx):
        if t['name'] != name:
            continue
        kind = t['fmt'] >> 8
        w, h = t['w'], t['h']
        wb, hb = (w + 3) // 4, (h + 3) // 4
        if kind == KIND_BC4:
            return bc4(deswizzle(t, bntx, wb * 8, hb), wb, hb, w, h)
        if kind == KIND_ASTC4:
            lin = deswizzle(t, bntx, wb * 16, hb)
            rgba = np.zeros((h, w, 4), dtype=np.uint8)
            lib.astc_decode_ldr(lin.tobytes(), w, h, 4, 4, (t['fmt'] & 0xff) == 6, rgba.ctypes.data)
            return rgba
        raise ArtError(f'{name} has a texture format this launcher does not read ({t["fmt"]:#x})')
    return None


# ---------------------------------------------------------------- the title movie

_MASTERS = {0x18538067, 0x1F43B675, 0xA0, 0x1654AE6B, 0xAE, 0xE0}   # Segment, Cluster, BlockGroup, Tracks, TrackEntry, Video


def _vint(b, at, keep_marker):
    first = b[at]
    n = 1
    while n <= 8 and not first & (0x80 >> (n - 1)):
        n += 1
    if n > 8:
        raise ArtError('the movie is not WebM')
    v = first if keep_marker else first & (0xff >> n)
    for i in range(1, n):
        v = v << 8 | b[at + i]
    return v, n


def webm_frames(b):
    """(time in ms, key frame, data) of every block of a single-track WebM, and the video's width and height."""
    out, info = [], {}

    def walk(at, end, cluster):
        while at < end:
            eid, n1 = _vint(b, at, True)
            size, n2 = _vint(b, at + n1, False)
            body = at + n1 + n2
            if size == (1 << (7 * n2)) - 1:   # unknown size: to the end of the parent
                size = end - body
            if eid in _MASTERS:
                walk(body, min(body + size, end), cluster)
            elif eid == 0xE7:
                cluster[0] = int.from_bytes(b[body:body + size], 'big')
            elif eid == 0xB0:
                info['w'] = int.from_bytes(b[body:body + size], 'big')
            elif eid == 0xBA:
                info['h'] = int.from_bytes(b[body:body + size], 'big')
            elif eid in (0xA3, 0xA1):   # SimpleBlock (its flags say key frame), Block
                _track, n = _vint(b, body, False)
                rel = struct.unpack_from('>h', b, body + n)[0]
                key = eid == 0xA3 and bool(b[body + n + 2] & 0x80)
                out.append((cluster[0] + rel, key, b[body + n + 3:body + size]))
            at = body + size

    walk(0, len(b), [0])
    if not out or 'w' not in info or 'h' not in info:
        raise ArtError('the title movie has no video frames')
    return out, info['w'], info['h']


def movie_frame(layout, data, at_s, work):
    """The picture of the movie `data` at at_s seconds as an HxWx3 uint8 array, decoded by the toolchain's dav1d."""
    dav1d = layout.msys / 'bin' / 'dav1d.exe'
    if not dav1d.is_file():
        raise ArtError('the build tools are not installed (Rebuild the Game installs them)')
    frames, w, h = webm_frames(data)
    upto = [f for f in frames if f[0] <= at_s * 1000] or frames[:1]
    k0 = max((i for i, f in enumerate(upto) if f[1]), default=0)
    sel = upto[k0:]
    ivf, y4m = Path(work) / 'title.ivf', Path(work) / 'title.y4m'
    with open(ivf, 'wb') as f:
        f.write(b'DKIF' + struct.pack('<HH4sHHIIII', 0, 32, b'AV01', w, h, 1000, 1, len(sel), 0))
        for t, _key, d in sel:
            f.write(struct.pack('<IQ', len(d), max(t, 0)) + d)
    r = subprocess.run([str(dav1d), '-q', '-i', str(ivf), '-o', str(y4m)], capture_output=True, text=True,
                       cwd=str(dav1d.parent), creationflags=NO_WINDOW)
    if r.returncode or not y4m.is_file():
        raise ArtError(f'the title movie did not decode: {(r.stderr or r.stdout).strip()[-300:]}')
    b = y4m.read_bytes()
    at = b.rfind(b'FRAME\n')
    if at < 0 or len(b) < at + 6 + w * h * 3 // 2:
        raise ArtError('the title movie did not decode to a picture')
    at += 6
    y = np.frombuffer(b, np.uint8, w * h, at).reshape(h, w).astype(np.float32)
    cw, ch = (w + 1) // 2, (h + 1) // 2
    u = np.frombuffer(b, np.uint8, cw * ch, at + w * h).reshape(ch, cw).astype(np.float32)
    v = np.frombuffer(b, np.uint8, cw * ch, at + w * h + cw * ch).reshape(ch, cw).astype(np.float32)
    u = (u.repeat(2, 0).repeat(2, 1)[:h, :w] - 128) * (255 / 224)
    v = (v.repeat(2, 0).repeat(2, 1)[:h, :w] - 128) * (255 / 224)
    y = (y - 16) * (255 / 219)
    rgb = np.stack([y + 1.5748 * v, y - 0.1873 * u - 0.4681 * v, y + 1.8556 * u], -1)   # BT.709
    return np.clip(rgb + 0.5, 0, 255).astype(np.uint8)


# ---------------------------------------------------------------- pictures

def resize(img, w, h):
    """img (HxWxC uint8 or float) at w x h: whole-pixel box averaging first when it shrinks by 2 or more, then
    bilinear. Returns float32."""
    a = img.astype(np.float32)
    fy, fx = a.shape[0] // h, a.shape[1] // w
    if fy >= 2 or fx >= 2:
        fy, fx = max(fy, 1), max(fx, 1)
        H, W = a.shape[0] // fy * fy, a.shape[1] // fx * fx
        a = a[:H, :W].reshape(H // fy, fy, W // fx, fx, -1).mean(axis=(1, 3))
    ys = (np.arange(h) + 0.5) * a.shape[0] / h - 0.5
    xs = (np.arange(w) + 0.5) * a.shape[1] / w - 0.5
    y0 = np.clip(np.floor(ys).astype(int), 0, a.shape[0] - 1)
    x0 = np.clip(np.floor(xs).astype(int), 0, a.shape[1] - 1)
    y1, x1 = np.minimum(y0 + 1, a.shape[0] - 1), np.minimum(x0 + 1, a.shape[1] - 1)
    wy = np.clip(ys - y0, 0, 1)[:, None, None]
    wx = np.clip(xs - x0, 0, 1)[None, :, None]
    top = a[y0][:, x0] * (1 - wx) + a[y0][:, x1] * wx
    bottom = a[y1][:, x0] * (1 - wx) + a[y1][:, x1] * wx
    return top * (1 - wy) + bottom * wy


def premultiplied_resize(rgba, w, h):
    """Resize with the colours weighted by alpha, so transparent texels add no dark fringe; uint8 straight alpha out."""
    a = rgba.astype(np.float32)
    a[..., :3] *= a[..., 3:4] / 255
    r = resize(a, w, h)
    r[..., :3] /= np.maximum(r[..., 3:4], 1e-3) / 255
    return np.clip(r + 0.5, 0, 255).astype(np.uint8)


def trim(rgba, pad=8):
    """rgba cut to the box of its visible texels, pad texels around."""
    ys, xs = np.nonzero(rgba[..., 3] > 4)
    if not len(ys):
        return rgba
    y0, y1 = max(ys.min() - pad, 0), min(ys.max() + 1 + pad, rgba.shape[0])
    x0, x1 = max(xs.min() - pad, 0), min(xs.max() + 1 + pad, rgba.shape[1])
    return rgba[y0:y1, x0:x1]


def place(dst, src, x, y):
    """src (uint8 RGBA, straight alpha) over dst (uint8 RGB) at x, y."""
    h, w = src.shape[:2]
    s = src.astype(np.float32)
    d = dst[y:y + h, x:x + w].astype(np.float32)
    al = s[..., 3:4] / 255
    dst[y:y + h, x:x + w] = np.clip(d * (1 - al) + s[..., :3] * al + 0.5, 0, 255).astype(np.uint8)


def crop(frame, w, h, cx, cy):
    """The largest w:h part of frame centred near (cx, cy) as fractions, scaled to w x h (uint8 RGB)."""
    H, W = frame.shape[:2]
    if W * h > H * w:
        ch, cw = H, H * w // h
    else:
        cw, ch = W, W * h // w
    x0 = int(np.clip(cx * W - cw / 2, 0, W - cw))
    y0 = int(np.clip(cy * H - ch / 2, 0, H - ch))
    return np.clip(resize(frame[y0:y0 + ch, x0:x0 + cw], w, h) + 0.5, 0, 255).astype(np.uint8)


def with_logo(bg, logo, width_share, cy):
    """bg (RGB) with the logo scaled to width_share of its width, centred at height cy (a fraction)."""
    H, W = bg.shape[:2]
    lw = int(W * width_share)
    lh = int(lw * logo.shape[0] / logo.shape[1])
    if lh > H * 0.8:
        lh = int(H * 0.8)
        lw = int(lh * logo.shape[1] / logo.shape[0])
    small = premultiplied_resize(logo, lw, lh)
    out = bg.copy()
    place(out, small, (W - lw) // 2, int(np.clip(cy * H - lh / 2, 0, H - lh)))
    return out


def navi_icon(body, glow, size=128):
    """Navi's HUD icon on her glow, size x size RGBA: the glow tinted the pale blue of her light."""
    out = np.zeros((size, size, 4), dtype=np.float32)
    g = resize(glow, size, size)
    g[..., :3] = np.array([200, 228, 255], dtype=np.float32)
    g[..., 3] *= 0.85
    b = resize(body, int(size * 0.8), int(size * 0.8))
    for layer, at in ((g, 0), (b, (size - b.shape[0]) // 2)):
        h, w = layer.shape[:2]
        d = out[at:at + h, at:at + w]
        sa, da = layer[..., 3:4] / 255, d[..., 3:4] / 255
        oa = sa + da * (1 - sa)
        d[..., :3] = (layer[..., :3] * sa + d[..., :3] * da * (1 - sa)) / np.maximum(oa, 1e-6)
        d[..., 3:4] = oa * 255
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)


def rgba_of(rgb):
    return np.concatenate([rgb, np.full(rgb.shape[:2] + (1,), 255, dtype=np.uint8)], -1)


def make(layout, game_dir='', dump=''):
    """{name: HxWx4 uint8} for icon, logo, hero, portrait and wide, from the player's game. ArtError when the game's
    files or the tools to read them are not there."""
    romfs = gamefiles.open_romfs(layout, game_dir, dump)
    if romfs is None:
        raise ArtError('the game\'s files were not found (the game folder\'s romfs, or the dump)')
    zs = gamefiles.ZsReader(romfs)
    title, common = zs.read(TITLE_ARCHIVE), zs.read(COMMON_ARCHIVE)
    movie = romfs.read(TITLE_MOVIE)
    if not title or not common or not movie:
        raise ArtError('the game\'s title screen files were not found')
    lib = astc_library(layout)
    logo = texture(title, LOGO, lib)
    body, glow = texture(common, NAVI, lib), texture(common, NAVI_GLOW, lib)
    if logo is None or body is None or glow is None:
        raise ArtError('the game\'s logo or Navi\'s icon was not found')
    logo = trim(logo)
    with tempfile.TemporaryDirectory(dir=str(layout.cache)) as work:
        frame = movie_frame(layout, movie, TITLE_AT, work)
    # the moon sits right of the middle and high: every cut keeps it, and the hero keeps the horizon too
    hero = crop(frame, 1920, 620, 0.5, 0.42)
    portrait = with_logo(crop(frame, 600, 900, 0.68, 0.5), logo, 0.88, 0.70)
    wide = with_logo(crop(frame, 920, 430, 0.5, 0.45), logo, 0.52, 0.55)
    return {'icon': navi_icon(body, glow), 'logo': logo, 'hero': rgba_of(hero), 'portrait': rgba_of(portrait),
            'wide': rgba_of(wide)}
