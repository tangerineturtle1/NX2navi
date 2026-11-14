"""List and dump the game's interface textures, for choosing the art the settings menu draws (runtime/ui/menu_art.cpp).

    python tools/ui_textures.py ROMFS [--out build/ui-textures] [--archive Common] [--match REGEX] [--sheet]

ROMFS is an extracted RomFS folder. Without --match it only lists; with it the matching textures become PNGs under
--out, and --sheet also writes contact sheets of them (numbered; sheet.txt says which number is which). Reads files the
way runtime/menu_assets.c does, and decodes ASTC with the runtime's own decoder (runtime/tex_astc.c, built into --out).
Everything written is game data and stays under build/. Needs Python 3.14 (compression.zstd) and numpy.
"""
import argparse
import ctypes
import re
import shutil
import struct
import subprocess
import sys
import zlib
from compression import zstd
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
ARCHIVES = 'UI/Layout2Archive'
# the second byte of a BNTX format: name, block size, bytes per block
KINDS = {0x0b: ('RGBA8', 1, 4), 0x1a: ('BC1', 4, 8), 0x1b: ('BC2', 4, 16), 0x1c: ('BC3', 4, 16), 0x1d: ('BC4', 4, 8),
         0x1e: ('BC5', 4, 16), 0x20: ('BC7', 4, 16), 0x2d: ('ASTC4x4', 4, 16)}


def u16(b, at): return struct.unpack_from('<H', b, at)[0]
def u32(b, at): return struct.unpack_from('<I', b, at)[0]
def u64(b, at): return struct.unpack_from('<Q', b, at)[0]


def sarc_members(p):
    """The members of a SARC archive as a name -> bytes dictionary."""
    hl, data = u16(p, 4), u32(p, 0xc)
    count, ents = u16(p, hl + 6), hl + 0xc
    names = ents + 16 * count + 8
    out = {}
    for i in range(count):
        e = ents + 16 * i
        at = names + (u32(p, e + 4) & 0xffff) * 4
        name = p[at:p.index(b'\0', at)].decode()
        out[name] = p[data + u32(p, e + 8):data + u32(p, e + 12)]
    return out


class Romfs:
    def __init__(self, root):
        self.root = Path(root)
        self.dicts = None

    def load_dicts(self):
        self.dicts = {}
        raw = zstd.decompress((self.root / 'Pack/ZsDic.pack.zs').read_bytes())
        for name, data in sarc_members(raw).items():
            d = zstd.ZstdDict(data)
            self.dicts[d.dict_id] = d

    def read(self, rel):
        b = (self.root / rel).read_bytes()
        if not rel.endswith('.zs'):
            return b
        id_ = zstd.get_frame_info(b).dictionary_id
        if not id_:
            return zstd.decompress(b)
        if self.dicts is None:
            self.load_dicts()
        return zstd.decompress(b, zstd_dict=self.dicts[id_])


def bntx_textures(b):
    """Each texture of a BNTX file: name, format, size, block height, data offset and size."""
    out = []
    count, arr = u32(b, 0x24), u64(b, 0x28)
    for i in range(count):
        p = u64(b, arr + 8 * i)
        if b[p:p + 4] != b'BRTI':
            continue
        no = u64(b, p + 0x60)
        name = b[no + 2:no + 2 + u16(b, no)].decode()
        out.append({'name': name, 'fmt': u32(b, p + 0x1c), 'w': u32(b, p + 0x24), 'h': u32(b, p + 0x28),
                    'th': u32(b, p + 0x34), 'size': u32(b, p + 0x50), 'data': u64(b, u64(b, p + 0x70))})
    return out


def deswizzle(t, b, row, rows):
    """The first level as tightly packed rows, in the Switch 2 GOB layout (runtime/tex_deswizzle.c tex_bl_copy)."""
    th = t['th']
    gobs, unit, block = (row + 63) // 64, 8 << th, 512 << th
    need = gobs * ((rows + unit - 1) // unit) * block
    bl = np.zeros(max(need, t['size']) // 16 + 1, dtype='V16')
    src = bytearray(bl.tobytes())
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


def astc_library(out, compiler=None, env=None):
    """The runtime's ASTC decoder as a library, built once into out."""
    dll = out / 'astc.dll'
    src = ROOT / 'runtime' / 'tex_astc.c'
    if not dll.is_file() or dll.stat().st_mtime < src.stat().st_mtime:
        cc = compiler or shutil.which('clang') or r'C:\msys64\ucrt64\bin\clang.exe'
        vma = Path(cc).resolve().parents[1] / 'include' / 'vma'   # tex.h reaches the Vulkan allocator's header
        if subprocess.run([cc, '-shared', '-O2', '-static', '-w', '-I', str(ROOT / 'runtime'), '-I', str(vma), str(src), '-o',
                           str(dll)], env=env).returncode:
            sys.exit('tex_astc.c did not compile')
    lib = ctypes.CDLL(str(dll))
    lib.astc_decode_ldr.restype = None
    lib.astc_decode_ldr.argtypes = [ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
                                    ctypes.c_bool, ctypes.c_void_p]
    return lib


def decode(t, b, lib):
    kind = t['fmt'] >> 8
    if kind not in (0x0b, 0x1d, 0x2d):
        return None
    _, bs, bpb = KINDS[kind]
    w, h = t['w'], t['h']
    wb, hb = (w + bs - 1) // bs, (h + bs - 1) // bs
    lin = deswizzle(t, b, wb * bpb, hb)
    if kind == 0x1d:
        return bc4(lin, wb, hb, w, h)
    if kind == 0x0b:
        return lin.reshape(h, w, 4).copy()
    rgba = np.zeros((h, w, 4), dtype=np.uint8)
    lib.astc_decode_ldr(lin.tobytes(), w, h, 4, 4, (t['fmt'] & 0xff) == 6, rgba.ctypes.data)
    return rgba


def png(path, a):
    h, w = a.shape[:2]
    rows = b''.join(b'\0' + a[y].tobytes() for y in range(h))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))


DIGITS = ['111101101101111', '010110010010111', '111001111100111', '111001111001111', '101101111001001',
          '111100111001111', '111100111101111', '111001010010010', '111101111101111', '111101111001111']


def label(sheet, x, y, n):
    for c in str(n):
        for j in range(5):
            for i in range(3):
                if DIGITS[int(c)][j * 3 + i] == '1':
                    sheet[y + j * 2:y + j * 2 + 2, x + i * 2:x + i * 2 + 2] = (255, 255, 0, 255)
        x += 8


def sheets(out, items, cell=220, cols=7, per=35):
    """Contact sheets over a mid brown, so white masks and dark art both show."""
    lines = []
    for s in range(0, len(items), per):
        page = items[s:s + per]
        rows = (len(page) + cols - 1) // cols
        sheet = np.full((rows * cell, cols * cell, 4), (84, 70, 58, 255), dtype=np.uint8)
        for k, (name, a) in enumerate(page):
            n = s + k
            lines.append(f'{n} {name} {a.shape[1]}x{a.shape[0]}')
            step = max(1, max(a.shape[0], a.shape[1]) // (cell - 8) + (max(a.shape[:2]) % (cell - 8) > 0))
            t = a[::step, ::step]
            cy, cx = (k // cols) * cell + 4, (k % cols) * cell + 4
            al = t[..., 3:4].astype(np.float32) / 255
            bg = sheet[cy:cy + t.shape[0], cx:cx + t.shape[1], :3].astype(np.float32)
            sheet[cy:cy + t.shape[0], cx:cx + t.shape[1], :3] = (t[..., :3] * al + bg * (1 - al)).astype(np.uint8)
            label(sheet, cx, cy, n)
        png(out / f'sheet{s // per:02}.png', sheet)
    (out / 'sheet.txt').write_text('\n'.join(lines) + '\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('romfs', type=Path)
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'ui-textures')
    ap.add_argument('--archive', default='', help='only archives whose name contains this')
    ap.add_argument('--match', help='a regular expression: the textures to decode')
    ap.add_argument('--sheet', action='store_true', help='write contact sheets of what was decoded')
    a = ap.parse_args()
    fs = Romfs(a.romfs)
    a.out.mkdir(parents=True, exist_ok=True)
    lib = astc_library(a.out) if a.match else None
    decoded = []
    for f in sorted((a.romfs / ARCHIVES).glob('*.bntx.zs')):
        if a.archive not in f.name:
            continue
        b = fs.read(f'{ARCHIVES}/{f.name}')
        ts = bntx_textures(b)
        print(f'# {f.name}: {len(ts)} textures')
        for t in sorted(ts, key=lambda t: t['name']):
            kind = KINDS.get(t['fmt'] >> 8, (f"{t['fmt']:#x}",))[0]
            if not a.match:
                print(f"{t['name']} {kind} {t['w']}x{t['h']}")
            elif re.search(a.match, t['name']):
                rgba = decode(t, b, lib)
                if rgba is None:
                    print(f"{t['name']}: format {kind} is not read by the runtime", file=sys.stderr)
                    continue
                png(a.out / f"{t['name']}.png", rgba)
                decoded.append((t['name'], rgba))
    if a.match:
        print(f'{len(decoded)} textures written to {a.out}')
    if a.sheet and decoded:
        sheets(a.out, decoded)


if __name__ == '__main__':
    main()
