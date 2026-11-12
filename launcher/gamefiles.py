"""The player's game files, as far as the launcher reads them itself: finding a .nup dump, the game's Cetra
Display font for the window (runtime/menu_assets.c reads the same font for the in-game menu's titles) and the game's
interface sounds for the window's sound effects (runtime/menu_sound.c plays the same files in the in-game menu).

They come from the built game's romfs/ (romfs.bin and index.json, tools/make_game.py) or, before the first
build, straight from the .nup. They are decoded once into data/cache/fonts and data/cache/sounds, which stay on the
player's PC like the rest of what is built from their game. Nothing of the game ships with the launcher.

- .zs files are zstd frames, most with one of the dictionaries of Pack/ZsDic.pack.zs (a SARC, compressed without a
  dictionary; a frame names its dictionary by id).
- A .bfotf font is an OpenType file behind an 8-byte header, every big-endian 32-bit word XORed with one key: the
  first word is 0x7f9a0218 under it, the second the payload size.
- A .bwav sound: "BWAV", a byte order mark, a version, a CRC32, a prefetch flag and the channel count (u16 at 0x0e),
  then 0x4c bytes per channel from 0x10: the codec (u16: 0 PCM16, 1 DSP-ADPCM), the pan (u16: 0 left, 1 right,
  2 middle), the sample rate and the sample count (u32 each), 16 DSP coefficients (s16 from +0x10) and the offset of
  the samples in the file (u32 at +0x30). DSP-ADPCM: 8-byte frames of a header byte (coefficient pair, scale
  exponent) and 14 four-bit samples.
"""
import json
import os
import struct
import sys
from pathlib import Path

TITLE_ID = 0x0400486024DEC000
FONTS = {   # role -> RomFS path; the window sets all its text in Cetra Display
    'title': 'Font2/Scalable/CetraDisplayPro-Medium.Nin_Ounce_NVN2.bfotf.zs',
}
SOUNDS = {   # the window's sound effect (launcher/ui/sound.py) -> the game's interface sound
    'select': 'Sound/Resource/Stream/SE_Sys_Do00.bwav',
    'back': 'Sound/Resource/Stream/SE_Sys_Cancel00.bwav',
}
DICTS = 'Pack/ZsDic.pack.zs'
DICT_NAMES = ('zs.zsdic', 'pack.zsdic', 'bcett.byml.zsdic', 'bkprb.zsdic')
MAX_RAW = 64 << 20


def _nup_module(layout):
    if str(layout.tools) not in sys.path:
        sys.path.insert(0, str(layout.tools))
    import nup
    return nup


# ---------------------------------------------------------------- finding a dump

def candidate_dirs(extra=()):
    home = Path.home()
    dirs = [Path(d) for d in extra if d]
    dirs += [home / 'Downloads', home / 'Desktop', home / 'Documents', home]
    if os.name == 'nt':
        import string
        dirs += [Path(f'{c}:\\') for c in string.ascii_uppercase[2:] if os.path.isdir(f'{c}:\\')]
    seen, out = set(), []
    for d in dirs:
        key = os.path.normcase(str(d))
        if key not in seen and d.is_dir():
            seen.add(key)
            out.append(d)
    return out


def is_game_nup(layout, path):
    """True when the file is a .nup holding Ocarina of Time (title 0400486024DEC000 or its update)."""
    try:
        n = _nup_module(layout).Nup(path)
    except Exception:
        return False
    return any(p['title_id'] in (TITLE_ID, TITLE_ID | 0x800) for p in n.packages)


def find_dumps(layout, extra=(), depth=1):
    """.nup files with the game in the usual places (the setup's folder first), newest first."""
    found = []
    for d in candidate_dirs(extra):
        pattern_dirs = [d]
        if depth > 0:
            try:
                pattern_dirs += [s for s in d.iterdir() if s.is_dir() and not s.name.startswith(('.', '$'))][:200]
            except OSError:
                pass
        for pd in pattern_dirs:
            try:
                for f in pd.glob('*.nup'):
                    if f.is_file() and is_game_nup(layout, f):
                        found.append(f)
            except OSError:
                continue
    uniq = {os.path.normcase(str(f)): f for f in found}
    return sorted(uniq.values(), key=lambda f: f.stat().st_mtime, reverse=True)


def is_game_folder(path):
    p = Path(path)
    return (p / 'exefs' / 'main').is_file() and (p / 'romfs').is_dir()


# ---------------------------------------------------------------- reading RomFS files

class RomfsImage:
    """The built game's romfs/romfs.bin through its index.json."""

    def __init__(self, folder):
        folder = Path(folder)
        index = json.loads((folder / 'index.json').read_text(encoding='utf-8'))
        self.files = {e['path']: (e['offset'], e['size']) for e in index['files']}
        self.bin = folder / 'romfs.bin'

    def read(self, rel):
        if rel not in self.files:
            return None
        off, size = self.files[rel]
        with open(self.bin, 'rb') as f:
            f.seek(off)
            return f.read(size)


class NupRomfs:
    """The RomFS of the game's update (or base) package inside a .nup."""

    def __init__(self, layout, path):
        nup = _nup_module(layout)
        self.n = nup.Nup(path)
        pkgs = [p for p in self.n.packages if p['title_id'] in (TITLE_ID, TITLE_ID | 0x800)]
        pkgs.sort(key=lambda p: (p['kind'] != 1, -p['version']))
        self.trees = []
        for p in pkgs:
            if p['romfs'] < self.n.ndir:
                self.trees.append(dict(self.n.tree(p['romfs'])))

    def read(self, rel):
        for t in self.trees:
            if rel in t:
                return self.n.read(t[rel])
        return None


def open_romfs(layout, game_dir='', dump=''):
    """A RomFS reader for the built game, else the dump, else None."""
    if game_dir and (Path(game_dir) / 'romfs' / 'index.json').is_file():
        try:
            return RomfsImage(Path(game_dir) / 'romfs')
        except (OSError, ValueError, KeyError):
            pass
    if dump and Path(dump).is_file():
        try:
            return NupRomfs(layout, dump)
        except Exception:
            pass
    if dump and is_game_folder(dump):
        return FolderRomfs(Path(dump) / 'romfs')
    return None


class FolderRomfs:
    def __init__(self, folder):
        self.folder = Path(folder)

    def read(self, rel):
        p = self.folder / rel
        return p.read_bytes() if p.is_file() else None


# ---------------------------------------------------------------- zstd with the game's dictionaries

def _zstd():
    try:
        from compression import zstd   # Python 3.14
        return 'stdlib', zstd
    except ImportError:
        import zstandard
        return 'zstandard', zstandard


def _frame_dict_id(data):
    kind, z = _zstd()
    if kind == 'stdlib':
        return z.get_frame_info(data).dictionary_id
    return z.get_frame_parameters(data).dict_id


def _decompress(data, dict_bytes=None):
    kind, z = _zstd()
    if kind == 'stdlib':
        d = z.ZstdDict(dict_bytes) if dict_bytes else None
        return z.decompress(data, zstd_dict=d)
    dctx = z.ZstdDecompressor(dict_data=z.ZstdCompressionDict(dict_bytes) if dict_bytes else None)
    return dctx.decompressobj().decompress(data)


def _dict_id(dict_bytes):
    """The id a zstd dictionary carries (magic 0xEC30A437, then the id)."""
    if len(dict_bytes) >= 8 and struct.unpack_from('<I', dict_bytes, 0)[0] == 0xEC30A437:
        return struct.unpack_from('<I', dict_bytes, 4)[0]
    return 0


def sarc_members(data):
    """{name: bytes} of a SARC archive."""
    if len(data) < 0x14 or data[:4] != b'SARC':
        return {}
    hl = struct.unpack_from('<H', data, 4)[0]
    base = struct.unpack_from('<I', data, 0xC)[0]
    if data[hl:hl + 4] != b'SFAT':
        return {}
    count = struct.unpack_from('<H', data, hl + 6)[0]
    ents = hl + 0xC
    names = ents + 16 * count + 8
    if data[ents + 16 * count:ents + 16 * count + 4] != b'SFNT':
        return {}
    out = {}
    for i in range(count):
        _h, attr, start, end = struct.unpack_from('<IIII', data, ents + 16 * i)
        no = names + (attr & 0xFFFF) * 4
        name = data[no:data.index(b'\0', no)].decode('utf-8', 'replace')
        if start <= end and base + end <= len(data):
            out[name] = data[base + start:base + end]
    return out


class ZsReader:
    def __init__(self, romfs):
        self.romfs = romfs
        self._dicts = None

    def dicts(self):
        if self._dicts is None:
            self._dicts = {}
            raw = self.romfs.read(DICTS)
            if raw:
                members = sarc_members(_decompress(raw))
                for name in DICT_NAMES:
                    if name in members:
                        self._dicts[_dict_id(members[name])] = members[name]
        return self._dicts

    def read(self, rel):
        data = self.romfs.read(rel)
        if data is None or not rel.endswith('.zs'):
            return data
        did = _frame_dict_id(data)
        if did:
            d = self.dicts().get(did)
            if d is None:
                return None
            return _decompress(data, d)
        return _decompress(data)


# ---------------------------------------------------------------- fonts

def bfotf_to_otf(b):
    """The OpenType (or TrueType) bytes inside a .bfotf/.bfttf, or None."""
    if not b or len(b) < 12:
        return None
    key = struct.unpack_from('>I', b, 0)[0] ^ 0x7F9A0218
    size = struct.unpack_from('>I', b, 4)[0] ^ key
    if size < 12 or size > len(b) - 8 or size > MAX_RAW:
        return None
    body = b[8:8 + size] + b'\0' * (-size % 4)
    words = struct.unpack(f'>{len(body) // 4}I', body)
    out = struct.pack(f'>{len(words)}I', *(w ^ key for w in words))[:size]
    if out[:4] != b'OTTO' and out[:4] != b'\x00\x01\x00\x00':
        return None
    return out


def font_family(otf):
    """The family name in an OpenType 'name' table (name id 16, else 1), or None."""
    try:
        num = struct.unpack_from('>H', otf, 4)[0]
        for i in range(num):
            tag, _cs, off, _len = struct.unpack_from('>4sIII', otf, 12 + 16 * i)
            if tag != b'name':
                continue
            _fmt, count, strings = struct.unpack_from('>HHH', otf, off)
            found = {}
            for k in range(count):
                pid, eid, lid, nid, ln, so = struct.unpack_from('>HHHHHH', otf, off + 6 + 12 * k)
                if nid not in (1, 16):
                    continue
                raw = otf[off + strings + so:off + strings + so + ln]
                if pid == 3 or pid == 0:
                    name = raw.decode('utf-16-be', 'replace')
                elif pid == 1:
                    name = raw.decode('mac_roman', 'replace')
                else:
                    continue
                rank = (nid == 16, pid == 3 and lid == 0x409)
                if nid not in found or rank > found[nid][0]:
                    found[nid] = (rank, name)
            for nid in (16, 1):
                if nid in found:
                    return found[nid][1]
    except (struct.error, IndexError):
        return None
    return None


def cached_fonts(layout):
    """{role: (path, family)} of the fonts decoded earlier."""
    d = layout.cache / 'fonts'
    out = {}
    for role in FONTS:
        p = d / f'{role}.otf'
        meta = d / f'{role}.family'
        if p.is_file() and meta.is_file():
            out[role] = (p, meta.read_text(encoding='utf-8').strip())
    return out


def extract_fonts(layout, game_dir='', dump=''):
    """Decode the game's Cetra Display into data/cache/fonts; {role: (path, family)} of the fonts found."""
    romfs = open_romfs(layout, game_dir, dump)
    if romfs is None:
        return {}
    zs = ZsReader(romfs)
    d = layout.cache / 'fonts'
    d.mkdir(parents=True, exist_ok=True)
    out = {}
    for role, rel in FONTS.items():
        try:
            otf = bfotf_to_otf(zs.read(rel))
        except Exception:
            otf = None
        fam = font_family(otf) if otf else None
        if not otf or not fam:
            continue
        p = d / f'{role}.otf'
        if not (p.is_file() and p.read_bytes() == otf):   # a file this process registered as a font stays as it is
            p.write_bytes(otf)
        (d / f'{role}.family').write_text(fam, encoding='utf-8')
        out[role] = (d / f'{role}.otf', fam)
    return out


# ---------------------------------------------------------------- interface sounds

def bwav_to_pcm(b):
    """A .bwav as (sample rate, frames): frames a list of (left, right) 16-bit samples, or None when it is no BWAV of
    one or two channels this reads. A middle-panned channel goes to both sides at 0.707 when it is the only one."""
    if not b or len(b) < 0x10 or b[:4] != b'BWAV':
        return None
    channels = struct.unpack_from('<H', b, 0x0e)[0]
    if channels not in (1, 2) or 0x10 + channels * 0x4c > len(b):
        return None
    rate, frames = struct.unpack_from('<II', b, 0x10 + 4)
    if not 8000 <= rate <= 96000 or not 0 < frames <= rate * 10:
        return None
    left, right = [0.0] * frames, [0.0] * frames
    for c in range(channels):
        info = 0x10 + c * 0x4c
        codec, pan = struct.unpack_from('<HH', b, info)
        count = min(struct.unpack_from('<I', b, info + 8)[0], frames)
        coef = struct.unpack_from('<16h', b, info + 0x10)
        off = struct.unpack_from('<I', b, info + 0x30)[0]
        if codec == 0:
            if off + count * 2 > len(b):
                return None
            samples = struct.unpack_from(f'<{count}h', b, off)
        elif codec == 1:
            if off + (count + 13) // 14 * 8 > len(b):
                return None
            samples, h1, h2 = [], 0, 0
            for i in range(count):
                fr = off + i // 14 * 8
                k = i % 14
                c1, c2 = coef[2 * (b[fr] >> 4 & 7)], coef[2 * (b[fr] >> 4 & 7) + 1]
                nib = b[fr + 1 + k // 2] >> (0 if k & 1 else 4) & 15
                if nib >= 8:
                    nib -= 16
                v = (((nib << (b[fr] & 15)) << 11) + c1 * h1 + c2 * h2 + 1024) >> 11
                v = -32768 if v < -32768 else 32767 if v > 32767 else v
                h2, h1 = h1, v
                samples.append(v)
        else:
            return None
        gain = 0.7071 if channels == 1 else 1.0
        for i, v in enumerate(samples):
            if pan != 1:
                left[i] += v * gain
            if pan != 0:
                right[i] += v * gain
    clip = lambda v: -32768 if v < -32768 else 32767 if v > 32767 else int(round(v))   # noqa: E731
    return rate, [(clip(a), clip(c)) for a, c in zip(left, right)]


def pcm_to_wav(rate, frames):
    """A 16-bit stereo WAV of frames, a list of (left, right) samples."""
    pcm = struct.pack(f'<{len(frames) * 2}h', *[v for fr in frames for v in fr])
    return (b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVEfmt '
            + struct.pack('<IHHIIHH', 16, 1, 2, rate, rate * 4, 4, 16) + b'data' + struct.pack('<I', len(pcm)) + pcm)


def cached_sounds(layout):
    """{effect: path} of the game's interface sounds decoded earlier."""
    d = layout.cache / 'sounds'
    return {name: d / f'{name}.wav' for name in SOUNDS if (d / f'{name}.wav').is_file()}


def extract_sounds(layout, game_dir='', dump=''):
    """Decode the game's select and back sounds into data/cache/sounds as WAV files; {effect: path} of the ones
    found."""
    romfs = open_romfs(layout, game_dir, dump)
    if romfs is None:
        return {}
    d = layout.cache / 'sounds'
    d.mkdir(parents=True, exist_ok=True)
    out = {}
    for name, rel in SOUNDS.items():
        try:
            got = bwav_to_pcm(romfs.read(rel))
        except Exception:
            got = None
        if not got:
            continue
        tmp = d / f'{name}.wav.tmp'
        tmp.write_bytes(pcm_to_wav(*got))
        os.replace(tmp, d / f'{name}.wav')
        out[name] = d / f'{name}.wav'
    return out
