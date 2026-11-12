"""tools/romfs_check.py on a small made-up game: damaged files are found, a damaged texture gets a stand-in with its own
name inside, and a clean copy leaves nothing behind. No game data."""
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import zstandard

TOOL = Path(__file__).resolve().parent.parent / 'tools' / 'romfs_check.py'


def sarc(files):
    """A SARC archive: little endian, the file table, then the names, then the data."""
    names, data, nodes = b'', b'', b''
    for name, body in files.items():
        nodes += struct.pack('<IIII', 0, 0x01000000 | len(names) // 4, len(data), len(data) + len(body))
        names += (name.encode() + b'\0').ljust((len(name) + 4) & ~3, b'\0')
        data += body
    sfat = b'SFAT' + struct.pack('<HHI', 12, len(files), 101) + nodes
    sfnt = b'SFNT' + struct.pack('<HH', 8, 0) + names
    offset = 0x14 + len(sfat) + len(sfnt)
    head = b'SARC' + struct.pack('<HHIIHH', 0x14, 0xFEFF, offset + len(data), offset, 0x100, 0)
    return head + sfat + sfnt + data


def texture(name):
    return b'BNTX\0\0\0\0' + name.encode() + b'\0' + bytes(range(256)) * 8


class RomfsCheck(unittest.TestCase):
    def build(self, game, files):
        z = zstandard.ZstdCompressor(level=3)
        rom = game / 'romfs'
        rom.mkdir(parents=True)
        entries, image = [], b''
        for path, body in files.items():
            entries.append({'path': path, 'offset': len(image), 'size': len(body)})
            image += body
        (rom / 'romfs.bin').write_bytes(image)
        (rom / 'index.json').write_text(json.dumps({'status': 'pass', 'romfs_size': len(image), 'files': entries}))
        return z

    def run_tool(self, game):
        r = subprocess.run([sys.executable, str(TOOL), '--game', str(game), '--jobs', '2'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return json.loads((game / 'romfs-check.json').read_text())

    def test_damaged_files_and_stand_in(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp)
            z = zstandard.ZstdCompressor(level=3)
            good = z.compress(texture('Cave_Floor_B_Nrm'))
            bad = bytes(16) + z.compress(texture('Cave_Floor_A_Nrm'))
            self.build(game, {'Tex/Cave_Floor_B_Nrm.bntx.zs': good, 'Tex/Cave_Floor_A_Nrm.bntx.zs': bad,
                              'Model/Cave.bfres.zs': bytes(64), 'Param/Zero.bgyml': bytes(32),
                              'Param/Fine.bgyml': b'YB' + bytes(30)})
            report = self.run_tool(game)
            self.assertEqual(sorted(report['damaged']),
                             ['Model/Cave.bfres.zs', 'Param/Zero.bgyml', 'Tex/Cave_Floor_A_Nrm.bntx.zs'])
            self.assertEqual(report['repaired'], {'Tex/Cave_Floor_A_Nrm.bntx.zs': 'Tex/Cave_Floor_B_Nrm.bntx.zs'})
            self.assertEqual(sorted(report['not_repaired']), ['Model/Cave.bfres.zs', 'Param/Zero.bgyml'])
            made = game / 'mods' / 'Damaged file stand-ins' / 'romfs' / 'Tex' / 'Cave_Floor_A_Nrm.bntx.zs'
            blob = zstandard.ZstdDecompressor().decompressobj().decompress(made.read_bytes())
            self.assertIn(b'Cave_Floor_A_Nrm\0', blob)
            self.assertNotIn(b'Cave_Floor_B_Nrm', blob)

    def test_clean_game_leaves_no_mod(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp)
            z = zstandard.ZstdCompressor(level=3)
            self.build(game, {'Tex/A.bntx.zs': z.compress(texture('A'))})
            stale = game / 'mods' / 'Damaged file stand-ins' / 'romfs'
            stale.mkdir(parents=True)
            self.assertEqual(self.run_tool(game)['damaged'], {})
            self.assertFalse(stale.parent.exists())


if __name__ == '__main__':
    unittest.main()
