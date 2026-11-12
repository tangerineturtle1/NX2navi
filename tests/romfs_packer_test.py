"""RomFS read-ahead and packing with synthetic files. Run: python tests/romfs_packer_test.py"""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import make_game


class RomfsPackerTest(unittest.TestCase):
    def test_read_ahead_limits_total_bytes_and_file_count(self):
        limit = make_game.ROMFS_READ_AHEAD
        sizes = [limit - 1, 1, limit + 1, 1, limit] + [0] * 512
        files = [(str(i), i, size) for i, size in enumerate(sizes)]
        groups = list(make_game.romfs_read_groups(files))
        self.assertEqual([entry for group in groups for entry in group], files)
        for group in groups:
            self.assertLessEqual(sum(size for _, _, size in group if size <= limit), limit)
            self.assertLessEqual(len(group), 256)

    def test_changed_file_cannot_replace_the_image(self):
        source = mock.Mock(romfs=[('changed.bin', 0)])
        source.romfs_size.return_value = 4
        with tempfile.TemporaryDirectory() as tmp:
            dst = Path(tmp) / 'romfs'
            dst.mkdir()
            (dst / 'romfs.bin').write_bytes(b'previous image')
            (dst / 'index.json').write_bytes(b'previous index')
            for limit, chunks in [(8, [b'123']), (2, [b'123', b'456'])]:
                with self.subTest(limit=limit), mock.patch.object(make_game, 'ROMFS_READ_AHEAD', limit):
                    source.romfs_chunks.return_value = chunks
                    with self.assertRaises(SystemExit):
                        make_game.step_romfs(source, dst.parent, 1)
                    self.assertEqual((dst / 'romfs.bin').read_bytes(), b'previous image')
                    self.assertEqual((dst / 'index.json').read_bytes(), b'previous index')

    def test_streamed_and_buffered_files_keep_the_same_layout(self):
        # Lower the read-ahead limit so these tiny files span several worker groups.
        large_file_readers = []

        class Source:
            romfs = [('e', b'123456789'), ('a', b'12345'), ('b', b'678'), ('c', b''), ('d', b'!')]

            def romfs_size(self, ref):
                return len(ref)

            def romfs_chunks(self, ref):
                if len(ref) > 8:
                    large_file_readers.append(threading.current_thread())
                yield ref[:3]
                yield ref[3:]

        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(make_game, 'ROMFS_READ_AHEAD', 8):
            out = Path(tmp)
            make_game.step_romfs(Source(), out, 2)
            image = (out / 'romfs' / 'romfs.bin').read_bytes()
            index = json.loads((out / 'romfs' / 'index.json').read_text())
        self.assertEqual(large_file_readers, [threading.main_thread()])
        self.assertEqual(image, b'12345' + bytes(11) + b'678' + bytes(13) + b'!' + bytes(15) + b'123456789')
        self.assertEqual(index['romfs_sha256'], hashlib.sha256(image).hexdigest())
        self.assertEqual(index['romfs_size'], len(image))
        self.assertEqual([(e['path'], e['offset'], e['size']) for e in index['files']],
                         [('a', 0, 5), ('b', 16, 3), ('c', 32, 0), ('d', 32, 1), ('e', 48, 9)])


if __name__ == '__main__':
    unittest.main()
