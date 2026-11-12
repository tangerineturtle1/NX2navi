"""tools/pipeline_list.py on made-up game folders: records folded once, a key whose shader the folder's answers do not
name left out, the compute variants taken, a list made before as an input, --mesh-only, a folder of another record
layout left out, the file read back, and what
tools/prepare_shaders.py takes from a list for its seed. Also tools/check_content.py refusing such files in a commit.
No game files; stdlib only.

Run: python tests/pipeline_list_test.py
"""
import hashlib
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import check_content  # noqa: E402
import pipeline_list as pl  # noqa: E402

REC = 464          # a KeyRec of the runtime that wrote the test's files: three ids, two flags, the state
VS, FS, CS, TS, MS = 0, 4, 5, 6, 7


def h(seed):
    return hashlib.sha256(seed.encode()).digest()


def answer(seed, stage, salt=0):
    """A shader-answers.bin record: stage, code hash, input id, 147 bytes of answers, the pad byte."""
    return struct.pack('<I', stage) + h('code ' + seed) + h('id ' + seed) + bytes([salt]) * 147 + b'\0'


def key(vs, fs=None, ts=None, state=0):
    ids = h('id ' + vs) + (h('id ' + fs) if fs else bytes(32)) + (h('id ' + ts) if ts else bytes(32))
    return ids + struct.pack('<II', fs is not None, ts is not None) + bytes([state]) * (REC - 104)


def folder(root, name, keys, answers, version=4, rec=REC):
    d = root / name
    d.mkdir()
    (d / 'pipeline-keys.bin').write_bytes(b'NX2NPKEY' + struct.pack('<II', version, rec) + b''.join(keys))
    (d / 'shader-answers.bin').write_bytes(pl.ANS_MAGIC + struct.pack('<II', 1, pl.ANS_REC) + b''.join(answers))
    return d


class PipelineList(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.log = []

    def tearDown(self):
        self.tmp.cleanup()

    def merge(self, dirs, **kw):
        return pl.merge(dirs, log=self.log.append, **kw)

    def test_merge(self):
        a = folder(self.root, 'a', [key('v1', 'f1'), key('m1', 'f2', 't1'), key('v1', 'f1', state=1)],
                   [answer('v1', VS), answer('f1', FS), answer('m1', MS), answer('f2', FS), answer('t1', TS)])
        b = folder(self.root, 'b', [key('v1', 'f1'), key('m2', 'f1'), key('v9', 'f1')],
                   [answer('v1', VS), answer('f1', FS), answer('m2', MS)])   # v9's answers are not recorded there
        header, keys, answers = self.merge([a, b])
        self.assertEqual(header, (b'NX2NPKEY', 4, REC))
        self.assertEqual(keys, [key('v1', 'f1'), key('m1', 'f2', 't1'), key('v1', 'f1', state=1), key('m2', 'f1')])
        self.assertEqual({x[36:68] for x in answers}, {h('id ' + s) for s in ('v1', 'f1', 'm1', 'f2', 't1', 'm2')})
        self.assertIn('1 without their shaders\' answers', self.log[1])

    def test_mesh_only(self):
        a = folder(self.root, 'a', [key('v1', 'f1'), key('m1', 'f2', 't1'), key('m1', None)],
                   [answer('v1', VS), answer('f1', FS), answer('m1', MS), answer('f2', FS), answer('t1', TS)])
        _, keys, answers = self.merge([a], mesh_only=True)
        self.assertEqual(keys, [key('m1', 'f2', 't1'), key('m1', None)])
        self.assertEqual({x[36:68] for x in answers}, {h('id ' + s) for s in ('m1', 'f2', 't1')})

    def test_compute_variants_and_a_list_as_input(self):
        a = folder(self.root, 'a', [key('v1', 'f1')], [answer('v1', VS), answer('f1', FS), answer('c1', CS), answer('f9', FS)])
        header, keys, answers = self.merge([a])
        self.assertEqual({x[36:68] for x in answers}, {h('id ' + s) for s in ('v1', 'f1', 'c1')})   # f9: no key names it
        self.assertIn('1 compute variants new', self.log[0])
        lst = self.root / 'first.bin'
        lst.write_bytes(pl.pack(header, keys, answers))
        b = folder(self.root, 'b', [key('v1', 'f1'), key('v2', 'f1')],
                   [answer('v2', VS), answer('f1', FS), answer('c1', CS), answer('c2', CS)])   # v1's answers: the list's
        header, keys, answers = self.merge([lst, b])
        self.assertEqual(keys, [key('v1', 'f1'), key('v2', 'f1')])
        self.assertEqual({x[36:68] for x in answers}, {h('id ' + s) for s in ('v1', 'f1', 'c1', 'v2', 'c2')})
        self.assertIn('1 key records, 1 new to the list', self.log[1])
        self.assertIn('2 key records, 1 new to the list, 0 without', self.log[2])
        self.assertIn('1 compute variants new', self.log[2])
        _, _, answers = self.merge([a], mesh_only=True)
        self.assertEqual(answers, [])

    def test_other_layout_left_out(self):
        a = folder(self.root, 'a', [key('v1', 'f1')], [answer('v1', VS), answer('f1', FS)])
        b = folder(self.root, 'b', [key('v2', 'f1')], [answer('v2', VS), answer('f1', FS)], version=5)
        _, keys, _ = self.merge([a, b])
        self.assertEqual(keys, [key('v1', 'f1')])
        self.assertIn('another layout', self.log[1])
        self.assertIsNone(self.merge([self.root / 'none']))

    def test_file(self):
        a = folder(self.root, 'a', [key('v1', 'f1'), key('m1', 'f1')],
                   [answer('v1', VS), answer('f1', FS), answer('m1', MS)])
        header, keys, answers = self.merge([a])
        data = pl.pack(header, keys, answers)
        got_answers, image = pl.unpack(data)
        self.assertEqual(got_answers, answers)
        self.assertEqual(image, (a / 'pipeline-keys.bin').read_bytes())   # the layout the runtime reads as it is
        self.assertEqual(pl.read_keys(image)[3], keys)
        self.assertIsNone(pl.unpack(data[:-1]))
        self.assertIsNone(pl.unpack(b'NX2NPKEY' + data[8:]))
        self.assertIsNone(pl.unpack(b''))

    def test_seed_variants(self):
        """prepare_shaders.list_variants: variants of shaders the game version has, once, with the id left to the
        runtime."""
        try:
            import prepare_shaders as ps
        except ImportError as e:   # its shader inventory needs Python 3.14's compression.zstd
            self.skipTest(f'tools/prepare_shaders.py does not import here: {e}')
        recs = [answer('v1', VS), answer('f1', FS), answer('gone', FS), answer('v1', VS, salt=1)]
        have = {h('code v1'), h('code f1')}
        seen = {(h('code f1'), bytes(147))}   # the seed has f1's variant already (its default one, say)
        out, absent = ps.list_variants(recs, have, seen)
        self.assertEqual(absent, 1)
        self.assertEqual(len(out), 2)
        for r in out:
            self.assertEqual(len(r), pl.ANS_REC)
            self.assertEqual(r[4:36], h('code v1'))
            self.assertEqual(r[36:68], bytes(32))
            self.assertEqual(r[-1], ps.LIST_PRIORITY)
        self.assertEqual([r[68] for r in out], [0, 1])
        self.assertEqual(ps.list_variants(recs, have, seen), ([], 1))   # all in the seed now

    def test_refused_in_a_commit(self):
        a = folder(self.root, 'a', [key('v1', 'f1')], [answer('v1', VS), answer('f1', FS)])
        lst = self.root / 'pipeline_list.bin'
        lst.write_bytes(pl.pack(*self.merge([a])))
        for p in (lst, a / 'pipeline-keys.bin', a / 'shader-answers.bin'):
            self.assertTrue(check_content.recorded_in_play(p, None), p.name)
            self.assertTrue(check_content.recorded_in_play(p, p.read_bytes()), p.name)
        other = self.root / 'notes.txt'
        other.write_text('NX2N is the prefix of the recorded files\n')
        self.assertFalse(check_content.recorded_in_play(other, None))
        self.assertFalse(check_content.recorded_in_play(self.root / 'missing.bin', None))


if __name__ == '__main__':
    unittest.main()
