"""Checks every file of a player's packed game data, and stands in for the damaged textures it can.

    python tools/romfs_check.py --game FOLDER [--romfs FOLDER] [--mods FOLDER] [--jobs N]

A dump can hold files whose first pages are zeros, with a container checksum that still passes because the damage was
packed along with them. The game then cannot decompress such a file, opens it over and over and gives up on everything
that needs it: one damaged normal map left a whole room without walls. Nothing during play looks for this, so the game
build runs this once for each copy of the game data (tools/make_game.py step_scan).

Every .zs file of <game>/romfs (index.json and romfs.bin, as tools/make_game.py writes them; --romfs names another) is
decompressed in full, with the dictionary its frame names from Pack/ZsDic.pack.zs. A file of any other kind that is
nothing but zeros is reported too. The result goes to <game>/romfs-check.json.

A damaged Tex/<name>.bntx.zs is replaced by a stand-in: an undamaged texture of the same kind, the closest name with the
same length, with the name inside it changed, so the game loads it and what uses it appears. It is another texture, not
the original, and only a clean dump restores the original. The stand-ins are an ordinary mod folder (--mods, default
<game>/mods) named STANDIN_MOD that the Mods menu can turn off; it is removed again when nothing is damaged. They are
built from the player's own files on the player's machine and never enter the repository.

Needs zstandard (tools/requirements-launcher.txt).
"""
import argparse
import concurrent.futures
import json
import os
import re
import shutil
import struct
import sys
import threading
from pathlib import Path

import zstandard

STANDIN_MOD = 'Damaged file stand-ins'
DICTIONARIES = 'Pack/ZsDic.pack.zs'
ZSTD_MAGIC = b'\x28\xb5\x2f\xfd'
# How much of a file the decompressor is given at a time. Each call returns its output as one bytes object, which
# the library grows as the output comes: with 4 MiB of input a call's output is tens of MiB, grown by copying, and
# the copying holds the interpreter's lock, so the worker threads wait on each other. Measured on 32 MiB frames
# with 12 threads: about 120 MB/s of packed data with 4 MiB pieces, about 2,500 MB/s with 64 KiB pieces; files
# smaller than a piece are the same either way.
PIECE = 1 << 16
TEXTURE = re.compile(r'Tex/([^/]+)\.bntx\.zs')


def sarc_members(blob):
    """Name -> (start, end) of the files of a SARC archive."""
    header = struct.unpack_from('<H', blob, 4)[0]
    data = struct.unpack_from('<I', blob, 12)[0]
    count = struct.unpack_from('<H', blob, header + 6)[0]
    names = header + 12 + 16 * count + 8
    out = {}
    for i in range(count):
        _, attr, start, end = struct.unpack_from('<IIII', blob, header + 12 + 16 * i)
        name = blob[names + (attr & 0xffff) * 4:].split(b'\0', 1)[0].decode('utf-8', 'replace')
        out[name] = (data + start, data + end)
    return out


class Image:
    """The packed game data; each thread reads through its own handle."""

    def __init__(self, folder):
        self.path = Path(folder) / 'romfs.bin'
        self.index = json.loads((Path(folder) / 'index.json').read_text(encoding='utf-8'))
        self.files = self.index['files']
        self.by_path = {f['path']: f for f in self.files}
        self.local = threading.local()
        self.dicts = {}
        entry = self.by_path.get(DICTIONARIES)
        if entry:
            pack = zstandard.ZstdDecompressor().decompressobj().decompress(self.read(entry))
            for start, end in sarc_members(pack).values():
                d = zstandard.ZstdCompressionDict(pack[start:end])
                self.dicts[d.dict_id()] = d

    def read(self, entry):
        f = getattr(self.local, 'f', None)
        if f is None:
            f = self.local.f = open(self.path, 'rb')
        f.seek(entry['offset'])
        return f.read(entry['size'])

    def decompressor(self, dict_id):
        """One per thread and dictionary: a decompressor is not for two threads at once."""
        cache = getattr(self.local, 'z', None)
        if cache is None:
            cache = self.local.z = {}
        if dict_id not in cache:
            d = self.dicts[dict_id] if dict_id else None
            cache[dict_id] = zstandard.ZstdDecompressor(dict_data=d) if d else zstandard.ZstdDecompressor()
        return cache[dict_id]

    def decompress(self, entry):
        data = self.read(entry)
        z = self.decompressor(zstandard.get_frame_parameters(data).dict_id)
        return z.decompressobj().decompress(data)


def problem(image, entry):
    """Why the file is damaged, or None. A frame naming a dictionary the game does not have cannot be judged: None."""
    data = image.read(entry)
    if len(data) != entry['size']:
        return 'shorter than the index says'
    if not entry['path'].endswith('.zs'):
        return 'nothing but zeros' if len(data) >= 16 and not any(data) else None
    if data[:4] != ZSTD_MAGIC:
        return 'no compressed frame at its start'
    try:
        params = zstandard.get_frame_parameters(data)
        if params.dict_id and params.dict_id not in image.dicts:
            return None
        out = image.decompressor(params.dict_id).decompressobj()
        size = 0
        for at in range(0, len(data), PIECE):
            size += len(out.decompress(data[at:at + PIECE]))
            if out.eof:   # the frame is whole: bytes after it are not the frame's, whichever piece they fall in
                break
    except zstandard.ZstdError as e:
        return f'does not decompress ({e})'
    if params.content_size and size != params.content_size:
        return f'decompresses to {size} bytes, not {params.content_size}'
    return None


def donors(image, damaged, name):
    """Undamaged textures whose names have this one's length, most alike first; the same last word is worth most."""
    words = name.split('_')
    found = []
    for path in image.by_path:
        m = TEXTURE.fullmatch(path)
        if not m or path in damaged or len(m[1]) != len(name):
            continue
        other = m[1].split('_')
        found.append((other[-1] != words[-1], -len(set(other) & set(words)), path))
    return [path for _, _, path in sorted(found)]


def stand_in(image, damaged, path):
    """A compressed stand-in for the damaged texture at path, or None when no undamaged texture can be renamed to it."""
    name = TEXTURE.fullmatch(path)[1]
    for donor in donors(image, damaged, name):
        try:
            blob = image.decompress(image.by_path[donor])
        except zstandard.ZstdError:
            continue
        old = TEXTURE.fullmatch(donor)[1].encode()
        if blob[:4] == b'BNTX' and blob.count(old) == 1:
            return zstandard.ZstdCompressor(level=19).compress(blob.replace(old, name.encode())), donor
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--game', type=Path, required=True, help='the game folder')
    ap.add_argument('--romfs', type=Path, help='the folder of index.json and romfs.bin (default <game>/romfs)')
    ap.add_argument('--mods', type=Path, help='the mods folder (default <game>/mods)')
    ap.add_argument('--jobs', type=int, default=min(os.cpu_count() or 4, 12))
    a = ap.parse_args()
    image = Image(a.romfs or a.game / 'romfs')
    total = len(image.files)
    damaged = {}
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        for n, (entry, why) in enumerate(zip(image.files, pool.map(lambda e: problem(image, e), image.files)), 1):
            if why:
                damaged[entry['path']] = why
                print(f'damaged: {entry["path"]}: {why}', flush=True)
            if n % 5000 == 0 or n == total:
                print(f'checked {n} of {total}', flush=True)
    mod = (a.mods or a.game / 'mods') / STANDIN_MOD
    shutil.rmtree(mod, ignore_errors=True)
    repaired, left = {}, []
    for path in sorted(damaged):
        made = stand_in(image, damaged, path) if TEXTURE.fullmatch(path) else None
        if not made:
            left.append(path)
            continue
        target = mod / 'romfs' / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(made[0])
        repaired[path] = made[1]
        print(f'stand-in: {path} from {made[1]}', flush=True)
    report = {'status': 'pass', 'files': total, 'damaged': damaged, 'repaired': repaired, 'not_repaired': left,
              'romfs_sha256': image.index.get('romfs_sha256')}
    (a.game / 'romfs-check.json').write_text(json.dumps(report, indent=1) + '\n', encoding='utf-8')
    print(f'{total} files checked, {len(damaged)} damaged, {len(repaired)} given a stand-in, {len(left)} left as they are',
          flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
