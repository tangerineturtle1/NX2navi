"""The profile-guided build of tools/make_game.py and tools/pgo.py, with a stand-in for the compiler process.

Which flags a build compiles with: a kit without an execution profile (the flags and keys it always had), a kit with one
that fits (the lifted code at -O2 and every compile with the profile), a record that names another recomp/, a record
that is missing or names none, a profile the compiler refuses in the trial compile, NX2NAVI_PGO=off, and an
instrumented build (NX2NAVI_PGO=gen). The object keys: the same profile in a kit at another place keeps them, another
profile changes them. tools/pgo.py: the record a merge writes names the recomp/ a kit compares, a game built from
another recomp/ stops the merge, and a release refuses a profile of another tree. tools/check_content.py refuses
profiles in a commit.

The last test uses the real compiler when MSYS2 UCRT64's clang is installed (and an llvm-profdata, when one is
found): a file that is no profile is refused by the trial compile, a real one is taken. No game files.

Run: python tests/pgo_build_test.py
"""
import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import check_content  # noqa: E402
import make_game as mg  # noqa: E402
import pgo  # noqa: E402

REFUSAL = ('clang: error: Error in reading profile pgo-v111.profdata: unsupported instrumentation profile format '
           'version')


class Compiler:
    """Stands in for subprocess.run in make_game.py: records every command, answers --version, writes the object
    and the dependency file of a compile, and refuses a command with -fprofile-use= when refuse is set."""

    def __init__(self, refuse=''):
        self.refuse = refuse
        self.cmds = []

    def __call__(self, cmd, **kw):
        cmd = [str(c) for c in cmd]
        self.cmds.append(cmd)
        if '--version' in cmd:
            return mock.Mock(returncode=0, stdout='clang version 23.1.5\nTarget: x86_64-w64-windows-gnu\n', stderr='')
        if self.refuse and any(c.startswith('-fprofile-use=') for c in cmd):
            return mock.Mock(returncode=1, stdout='', stderr=self.refuse + '\n')
        if '-o' in cmd:
            Path(cmd[cmd.index('-o') + 1]).write_bytes(b'object')
        if '-MF' in cmd:
            Path(cmd[cmd.index('-MF') + 1]).write_text(
                cmd[cmd.index('-o') + 1].replace('\\', '/') + ': ' + cmd[-1].replace('\\', '/') + '\n')
        return mock.Mock(returncode=0, stdout='', stderr='')

    def trials(self):
        return [c for c in self.cmds if any(a.startswith('-fprofile-use=') for a in c)]


class PgoBuildTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='nx2navi-pgo-'))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.kit = self.make_kit('kit')
        self.clang, self.clangxx = self.tmp / 'msys' / 'bin' / 'clang.exe', self.tmp / 'msys' / 'bin' / 'clang++.exe'
        self.inc = ['-I', str(self.kit / 'runtime')]
        self.cc = Compiler()
        env = {k: v for k, v in os.environ.items() if k not in ('NX2NAVI_PGO', 'NX2NAVI_GUEST_OPT')}
        for p in (mock.patch.object(mg, 'KIT', self.kit), mock.patch.object(mg, 'WIN', True),
                  mock.patch.object(mg, 'PGO_MODE', ''), mock.patch.object(mg, 'PGO_IDS', {}),
                  mock.patch.dict(os.environ, env, clear=True)):
            p.start()
            self.addCleanup(p.stop)

    def make_kit(self, name, lifter='def lift(): return 1\n'):
        kit = self.tmp / name
        (kit / 'recomp').mkdir(parents=True)
        (kit / 'recomp' / 'gen.py').write_text(lifter)
        (kit / 'recomp' / 'patches-v111.txt').write_text('# no patches\n')
        (kit / 'runtime').mkdir()
        return kit

    def add_profile(self, kit=None, folder='data', content=b'\xfflprofi\x81 counts', record='fits'):
        """A profile in the kit's data/ with a record that names the kit's recomp/ ('fits'), another one ('other'),
        none ('empty'), or without a record (None)."""
        kit = kit or self.kit
        d = kit.joinpath(*folder.split('/'))
        d.mkdir(parents=True, exist_ok=True)
        (d / 'pgo-v111.profdata').write_bytes(content)
        if record:
            digest = {'fits': mg.recomp_digest(kit), 'other': 'f' * 64}.get(record)
            (d / 'pgo-v111.json').write_text(json.dumps({'version': 'v111', **({'recomp_digest': digest} if digest else {})}))
        return d / 'pgo-v111.profdata'

    def choose(self, fast=None):
        with mock.patch.object(mg.subprocess, 'run', self.cc):
            return mg.pgo_profile(self.clang, {}, self.tmp / 'trial', fast, self.tmp / 'msys')

    def commands(self, pgo=()):
        """(the lifted code's command, a runtime file's) with the flags pgo, without the compiler and the paths."""
        cmd_for = mg.runtime_cmd(self.clang, self.clangxx, self.inc, level=3, pgo=pgo)
        guest = cmd_for(self.tmp / 'main_0001.c', self.tmp / 'main_main_0001.o')
        runtime = cmd_for(self.kit / 'runtime' / 'perf.c', self.tmp / 'rt_perf.o')
        return guest[1:-5], runtime[1:-5]   # up to -I <runtime> -o <object> <source>

    PLAIN = (['-c', '-std=gnu11', '-O1', '-march=x86-64-v3', '-fno-strict-aliasing', '-w'],
             ['-c', '-std=gnu11', '-O2', '-fno-strict-aliasing', '-w'])

    def test_a_kit_without_a_profile_builds_as_before(self):
        profile, why = self.choose()
        self.assertIsNone(profile)
        self.assertIn('no execution profile', why)
        self.assertEqual(self.cc.cmds, [])                       # nothing to try
        self.assertEqual(self.commands(), self.PLAIN)
        self.assertEqual(mg.pgo_tag(), '')                       # the compile step's key is what it was
        self.assertEqual(mg.pgo_salt('compiler', None), 'compiler')   # and so are the object keys
        with mock.patch.dict(os.environ, {'NX2NAVI_PGO': 'off'}):
            self.assertEqual(mg.pgo_tag(), '')

    def test_a_fitting_profile_goes_into_every_compile(self):
        path = self.add_profile()
        fast = self.tmp / 'msys' / 'clang64' / 'bin' / 'clang.exe'
        profile, why = self.choose(fast)
        self.assertEqual((profile, why), (path, ''))
        # one trial compile by each compiler that will read the file, the fast one with its sysroot
        trials = self.cc.trials()
        self.assertEqual([t[0] for t in trials], [str(self.clang), str(fast)])
        self.assertEqual(trials[1][1], f'--sysroot={self.tmp / "msys"}')
        self.assertFalse((self.tmp / 'trial').exists())
        use = [f'-fprofile-use={path}', '-mllvm', '-static-func-full-module-prefix=false']
        self.assertEqual(mg.pgo_use_flags(profile), use)
        guest, runtime = self.commands(mg.pgo_use_flags(profile))
        self.assertEqual(guest, ['-c', '-std=gnu11', '-O2', '-march=x86-64-v3', '-fno-strict-aliasing', '-w', *use])
        self.assertEqual(runtime, [*self.PLAIN[1], *use])
        self.assertEqual(mg.pgo_tag(), '|pgo ' + mg.pgo_id(path))
        self.assertNotEqual(mg.pgo_salt('compiler', profile), 'compiler')

    def test_a_source_checkout_takes_the_profile_from_build_pgo(self):
        path = self.add_profile(folder='build/pgo')
        self.assertEqual(self.choose(), (path, ''))
        release = self.add_profile()   # a release's data/ comes first
        self.assertEqual(self.choose(), (release, ''))

    def test_a_record_of_another_tree_leaves_the_profile_out(self):
        self.add_profile(record='other')
        profile, why = self.choose()
        self.assertIsNone(profile)
        self.assertIn('trained on another recomp/', why)
        self.assertEqual(self.cc.cmds, [])   # decided before any compiler runs
        self.assertEqual(self.commands(), self.PLAIN)
        # the same record fits once the kit's recomp/ is the one it names, and not after the lifter changed again
        self.add_profile(record='fits')
        self.assertIsNotNone(self.choose()[0])
        (self.kit / 'recomp' / 'gen.py').write_text('def lift(): return 2\n')
        self.assertIsNone(self.choose()[0])
        (self.kit / 'recomp' / 'patches-v111.txt').write_text('# a patch\n')
        self.assertIsNone(self.choose()[0])

    def test_a_missing_or_empty_record_leaves_the_profile_out(self):
        for record, said in ((None, 'is missing'), ('empty', 'does not name the recomp/')):
            shutil.rmtree(self.kit / 'data', ignore_errors=True)
            self.add_profile(record=record)
            profile, why = self.choose()
            self.assertIsNone(profile, record)
            self.assertIn(said, why)
        (self.kit / 'data' / 'pgo-v111.json').write_text('{ cut short')
        self.assertIn('does not name the recomp/', self.choose()[1])
        self.assertEqual(self.cc.cmds, [])

    def test_a_profile_the_compiler_refuses_is_left_out(self):
        self.add_profile()
        self.cc = Compiler(refuse=REFUSAL)
        profile, why = self.choose()
        self.assertIsNone(profile)
        self.assertIn('the compiler cannot read it', why)
        self.assertIn('unsupported instrumentation profile format version', why)
        self.assertEqual(len(self.cc.trials()), 1)   # one compile says so, not every compile of the build
        self.assertEqual(self.commands(), self.PLAIN)
        # the second compiler's refusal counts too
        fast = self.tmp / 'msys' / 'clang64' / 'bin' / 'clang.exe'

        class OnlyFast(Compiler):
            def __call__(self, cmd, **kw):
                self.refuse = REFUSAL if str(cmd[0]) == str(fast) else ''
                return super().__call__(cmd, **kw)
        self.cc = OnlyFast()
        self.assertIsNone(self.choose(fast)[0])
        self.assertEqual(len(self.cc.trials()), 2)

    def test_a_compiler_that_does_not_start_leaves_the_profile_out(self):
        self.add_profile()

        def gone(cmd, **kw):
            raise FileNotFoundError(2, 'No such file or directory', str(cmd[0]))
        with mock.patch.object(mg.subprocess, 'run', gone):
            profile, why = mg.pgo_profile(self.clang, {}, self.tmp / 'trial')
        self.assertIsNone(profile)
        self.assertIn('does not start', why)

    def test_off_leaves_a_fitting_profile_out(self):
        self.add_profile()
        for mode in ({'NX2NAVI_PGO': 'off'}, {'NX2NAVI_PGO': ' OFF '}):
            with mock.patch.dict(os.environ, mode):
                profile, why = self.choose()
                self.assertIsNone(profile)
                self.assertIn('NX2NAVI_PGO=off', why)
                self.assertEqual(mg.pgo_tag(), '|pgo off')
        with mock.patch.object(mg, 'PGO_MODE', 'off'):   # --pgo off
            self.assertIsNone(self.choose()[0])
        self.assertEqual(self.cc.cmds, [])
        with mock.patch.dict(os.environ, {'NX2NAVI_PGO': 'something'}):   # no mode: as without it
            self.assertIsNotNone(self.choose()[0])

    def test_only_the_windows_build_takes_the_profile(self):
        self.add_profile()
        with mock.patch.object(mg, 'WIN', False):
            profile, why = self.choose()
        self.assertIsNone(profile)
        self.assertIn('Windows', why)

    def test_an_instrumented_build_counts_into_the_game_folder(self):
        out = self.tmp / 'game out'
        with mock.patch.dict(os.environ, {'NX2NAVI_PGO': 'gen'}):
            self.assertEqual(mg.pgo_mode(), 'gen')
            self.assertEqual(mg.pgo_tag(), '|pgo gen')
        flags = mg.pgo_gen_flags(out)
        self.assertEqual(flags, [f'-fprofile-generate={(out / "pgo").as_posix()}', '-mllvm',
                                 '-static-func-full-module-prefix=false', '-DNX2NAVI_PGO_GEN'])
        guest, runtime = self.commands(flags)
        self.assertEqual(guest, ['-c', '-std=gnu11', '-O2', '-march=x86-64-v3', '-fno-strict-aliasing', '-w', *flags])
        self.assertEqual(runtime, [*self.PLAIN[1], *flags])
        # what the game was built from goes beside the counts, and tools/pgo.py reads it back
        self.make_real_recomp()
        with mock.patch.object(mg.subprocess, 'run', self.cc):
            mg.pgo_gen_record(out, self.clang, {})
        made = json.loads((out / 'pgo' / 'training.json').read_text())
        self.assertEqual(made['recomp_digest'], mg.recomp_digest())
        self.assertEqual(made['compiler'], 'clang version 23.1.5')
        self.assertEqual(pgo.training(out, 'v111'), made)

    def make_real_recomp(self):
        """The test kit's recomp/ becomes this checkout's, so that what make_game.py records in the kit is what
        tools/pgo.py, which reads the checkout, compares it with."""
        shutil.rmtree(self.kit / 'recomp')
        (self.kit / 'recomp').mkdir()
        for p in (ROOT / 'recomp').iterdir():
            if p.suffix == '.py' or p.name == 'patches-v111.txt':
                shutil.copy2(p, self.kit / 'recomp' / p.name)

    def test_build_tools_without_the_profile_runtime_stop_an_instrumented_build(self):
        def no_runtime(cmd, **kw):
            self.assertIn('-fprofile-generate', cmd)
            return mock.Mock(returncode=1, stdout='', stderr='ld: cannot find libclang_rt.profile.a: No such file\n'
                                                              'clang++: error: linker command failed\n')
        msys = self.tmp / 'msys64' / 'ucrt64'
        err = io.StringIO()
        with mock.patch.object(mg.subprocess, 'run', no_runtime), contextlib.redirect_stderr(err), \
                self.assertRaises(SystemExit):
            mg.pgo_gen_check(self.clangxx, {}, self.tmp / 'trial', msys, ['-fuse-ld=lld'])
        self.assertIn('mingw-w64-ucrt-x86_64-compiler-rt', err.getvalue())
        self.assertIn('cannot find libclang_rt.profile.a', err.getvalue())
        self.assertFalse((self.tmp / 'trial').exists())
        with mock.patch.object(mg.subprocess, 'run', self.cc):   # with the runtime: nothing to say
            mg.pgo_gen_check(self.clangxx, {}, self.tmp / 'trial', msys, [])

    def test_the_object_keys_follow_the_profile_and_not_the_kits_folder(self):
        """A launcher update puts the kit into another folder: the same profile there keeps every object. Another
        profile, or a build that leaves it out, compiles again."""
        src = self.tmp / 'lift' / 'main_0001.c'
        src.parent.mkdir()
        src.write_text('int f;\n')
        obj = self.tmp / 'obj'
        obj.mkdir()

        def key(kit, profile):
            cache = mg.ObjectCache(obj / 'objects.json', {'<kit>': kit})
            inc = ['-I', str(kit / 'runtime')]
            cmd_for = mg.runtime_cmd(self.clang, self.clangxx, inc, level=3,
                                     pgo=mg.pgo_use_flags(profile) if profile else ())
            dst = obj / 'main_main_0001.o'
            return mg.object_key(cmd_for(src, dst), src, dst, cache, mg.pgo_salt('compiler', profile))
        here = self.add_profile()
        moved = self.make_kit('kit 2')
        there = self.add_profile(moved)
        self.assertEqual(key(self.kit, here), key(moved, there))
        self.assertNotEqual(key(self.kit, here), key(self.kit, None))
        other = self.add_profile(moved, content=b'\xfflprofi\x81 other counts')
        self.assertNotEqual(key(self.kit, here), key(moved, other))
        # without a profile the key is the one a kit before profiles gave: the flags and the compiler's id alone
        cache = mg.ObjectCache(obj / 'objects.json', {'<kit>': self.kit})
        dst = obj / 'main_main_0001.o'
        plain = [str(self.clang), *self.PLAIN[0], '-I', str(self.kit / 'runtime'), '-o', str(dst), str(src)]
        self.assertEqual(key(self.kit, None), mg.object_key(plain, src, dst, cache, 'compiler'))

    def test_a_new_profile_compiles_the_objects_again(self):
        src, obj = self.tmp / 'src', self.tmp / 'obj'
        src.mkdir()
        obj.mkdir()
        srcs = []
        for i in range(4):
            (src / f'main_{i}.c').write_text(f'int f{i};\n')
            srcs.append((src / f'main_{i}.c', obj / f'main_main_{i}.o'))

        def build():
            profile, _ = mg.pgo_profile(self.clang, {}, self.tmp / 'trial')
            cache = mg.ObjectCache(obj / 'objects.json', {'<kit>': self.kit})
            cmd_for = mg.runtime_cmd(self.clang, self.clangxx, self.inc, level=3,
                                     pgo=mg.pgo_use_flags(profile) if profile else ())
            return mg.compile_all(srcs, cmd_for, cache, 2, {}, salt=mg.pgo_salt('compiler', profile))[:2]
        with mock.patch.object(mg.subprocess, 'run', self.cc):
            self.assertEqual(build(), (4, 0))        # no profile
            self.assertEqual(build(), (0, 4))
            self.add_profile()
            self.assertEqual(build(), (4, 0))        # the kit brought one
            self.assertEqual(build(), (0, 4))
            self.add_profile(content=b'\xfflprofi\x81 counts of a longer session')
            self.assertEqual(build(), (4, 0))        # and another
            self.add_profile(content=b'\xfflprofi\x81 counts of a longer session', record='other')
            self.assertEqual(build(), (4, 0))        # left out: the flags of a kit without one
            self.assertEqual(build(), (0, 4))
        compiles = [c for c in self.cc.cmds if '-MF' in c]
        self.assertTrue(all('-O1' in c for c in compiles[:4] + compiles[-4:]))
        self.assertTrue(all('-O2' in c and any(a.startswith('-fprofile-use=') for a in c) for c in compiles[4:12]))


class PgoToolTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='nx2navi-pgo-'))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.build = self.tmp / 'game'
        (self.build / 'pgo').mkdir(parents=True)
        (self.build / 'pgo' / 'default_1.profraw').write_bytes(b'raw counts')
        self.tool = self.tmp / 'llvm' / 'bin' / ('llvm-profdata' + pgo.EXE)
        self.tool.parent.mkdir(parents=True)
        self.tool.write_bytes(b'')
        self.ran = []
        p = mock.patch.object(pgo, 'recomp_tree', lambda: 'a' * 40)
        p.start()
        self.addCleanup(p.stop)

    def profdata(self, cmd, **kw):
        """Stands in for llvm-profdata."""
        cmd = [str(c) for c in cmd]
        self.ran.append(cmd)
        if 'merge' in cmd:
            Path(cmd[cmd.index('-o') + 1]).write_bytes(b'merged')
        return mock.Mock(returncode=0, stdout='LLVM (http://llvm.org/):\n  LLVM version 23.1.5\n', stderr='')

    def merge(self, version='v111'):
        out = self.tmp / 'pgo' / f'pgo-{version}.profdata'
        with mock.patch.object(pgo.subprocess, 'run', self.profdata), contextlib.redirect_stdout(io.StringIO()):
            pgo.merge(self.build, out, version, self.tool.parent.parent)
        return out

    def test_the_merge_records_the_recomp_a_kit_compares(self):
        out = self.merge()
        self.assertEqual(out.read_bytes(), b'merged')
        self.assertEqual(self.ran[0][:3], [str(self.tool), 'merge', '--failure-mode=all'])
        self.assertEqual(self.ran[0][-1], str(self.build / 'pgo' / 'default_1.profraw'))
        record = json.loads(out.with_suffix('.json').read_text())
        self.assertEqual(record['version'], 'v111')
        self.assertEqual(record['recomp_tree'], 'a' * 40)
        self.assertEqual(record['tool'], 'LLVM version 23.1.5')
        # what make_game.py works out for a kit with this checkout's recomp/: the profile fits such a kit
        with mock.patch.object(mg, 'KIT', ROOT):
            self.assertEqual(record['recomp_digest'], mg.recomp_digest())
            data = self.tmp / 'release' / 'data'
            placed = pgo.place(out, data)
            self.assertEqual(mg.pgo_record(placed), '')
        self.assertEqual(sorted(p.name for p in data.iterdir()), ['pgo-v111.json', 'pgo-v111.profdata'])

    def test_a_game_built_from_another_recomp_stops_the_merge(self):
        made = {'version': 'v111', 'recomp_digest': pgo.recomp_digest('v111'), 'compiler': 'clang version 23.1.5'}
        (self.build / 'pgo' / 'training.json').write_text(json.dumps(made))
        record = json.loads(self.merge().with_suffix('.json').read_text())
        self.assertEqual(record['compiler'], 'clang version 23.1.5')
        (self.build / 'pgo' / 'training.json').write_text(json.dumps(dict(made, recomp_digest='0' * 64)))
        with self.assertRaises(SystemExit) as e:
            self.merge()
        self.assertIn('another recomp/', str(e.exception))
        (self.build / 'pgo' / 'training.json').write_text(json.dumps(dict(made, version='v143')))
        with self.assertRaises(SystemExit):
            self.merge()

    def test_a_merge_needs_counts_and_a_clean_recomp(self):
        (self.build / 'pgo' / 'default_1.profraw').unlink()
        with self.assertRaises(SystemExit) as e:
            self.merge()
        self.assertIn('no .profraw', str(e.exception))
        (self.build / 'pgo' / 'default_1.profraw').write_bytes(b'raw counts')
        with mock.patch.object(pgo, 'recomp_tree', lambda: None), self.assertRaises(SystemExit) as e:
            self.merge()
        self.assertIn('uncommitted changes', str(e.exception))

    def test_a_release_refuses_a_profile_of_another_tree(self):
        out = self.merge()
        rec = out.with_suffix('.json')
        record = json.loads(rec.read_text())
        self.assertEqual(pgo.fitting(out), record)
        rec.write_text(json.dumps(dict(record, recomp_digest='0' * 64)))
        with self.assertRaises(SystemExit) as e:
            pgo.place(out, self.tmp / 'release' / 'data')
        self.assertIn('another recomp/', str(e.exception))
        self.assertFalse((self.tmp / 'release').exists())
        rec.write_text(json.dumps(dict(record, version='v143')))
        with self.assertRaises(SystemExit):
            pgo.fitting(out)
        rec.unlink()
        with self.assertRaises(SystemExit):
            pgo.fitting(out)

    def test_profiles_stay_out_of_a_commit(self):
        self.assertTrue(check_content.execution_profile(Path('data/pgo-v111.profdata')))
        self.assertTrue(check_content.execution_profile(Path('game/pgo/default_1.PROFRAW')))
        self.assertFalse(check_content.execution_profile(Path('data/pgo-v111.json')))
        self.assertFalse(check_content.execution_profile(Path('tools/pgo.py')))


MSYS = Path(r'C:\msys64\ucrt64')
TEXT_PROFILE = ':ir\nnx2navi_pgo_trial\n0\n1\n100\n\n'   # one function with one counter, as llvm-profdata reads text


@unittest.skipUnless((MSYS / 'bin' / 'clang.exe').is_file(), 'needs MSYS2 UCRT64 clang')
class RealCompilerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='nx2navi-pgo-'))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.clang = MSYS / 'bin' / 'clang.exe'
        self.env = dict(os.environ, PATH=str(MSYS / 'bin') + os.pathsep + os.environ.get('PATH', ''))

    def test_the_trial_compile_tells_a_profile_from_a_file_that_is_none(self):
        bad = self.tmp / 'pgo-v111.profdata'
        bad.write_bytes(b'not a profile')
        said = mg.pgo_trial(bad, [[self.clang]], self.env, self.tmp / 'trial')
        self.assertIn('Error in reading profile', said)
        cut = self.tmp / 'cut.profdata'
        cut.write_bytes(b'\xfflprofi\x81')   # a profile's first bytes and nothing after them
        self.assertTrue(mg.pgo_trial(cut, [[self.clang]], self.env, self.tmp / 'trial'))
        self.assertTrue(mg.pgo_trial(self.tmp / 'missing.profdata', [[self.clang]], self.env, self.tmp / 'trial'))
        tool = shutil.which('llvm-profdata') or next(
            (str(p) for p in (MSYS / 'bin' / 'llvm-profdata.exe', Path(r'C:\Program Files\LLVM\bin\llvm-profdata.exe'))
             if p.is_file()), None)
        if not tool:
            self.skipTest('no llvm-profdata to write a real profile with; the refusals above were checked')
        text, good = self.tmp / 'trial.proftext', self.tmp / 'good.profdata'
        text.write_text(TEXT_PROFILE)
        subprocess.run([tool, 'merge', '-o', str(good), str(text)], check=True, capture_output=True)
        self.assertEqual(mg.pgo_trial(good, [[self.clang]], self.env, self.tmp / 'trial'), '')


if __name__ == '__main__':
    unittest.main()
