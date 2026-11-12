"""The build's host probes on Linux, over fake /proc and cgroup files and a fake systemd-inhibit: tools/hostmem.py's
available_ram (MemAvailable, capped by the cgroup v2 memory limits), the lift's worker count that follows it
(recomp/gen.py pick_jobs), tools/make_game.py's cpu_tag (the compile key's processor part) and keep_awake (the sleep
lock). No game files; stdlib only (the worker count test also needs numpy, as the lifter does).

Run: python tests/host_probes_test.py
"""
import os
import signal
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'recomp'))

import hostmem  # noqa: E402
import make_game  # noqa: E402

try:
    import gen
except ImportError:
    gen = None

LINUX = unittest.skipUnless(sys.platform.startswith('linux'), 'the Linux probes')
GIB = 1 << 30


class Tmp(unittest.TestCase):
    def setUp(self):
        td = tempfile.TemporaryDirectory()
        self.addCleanup(td.cleanup)
        self.tmp = Path(td.name)


@LINUX
class AvailableRamTest(Tmp):
    """available_ram on a fake /proc and a fake cgroup v2 mount."""

    def setUp(self):
        super().setUp()
        self.proc, self.cgroup = self.tmp / 'proc', self.tmp / 'cgroup'
        (self.proc / 'self').mkdir(parents=True)
        self.cgroup.mkdir()
        for name, path in (('PROC', self.proc), ('CGROUP', self.cgroup)):
            patcher = mock.patch.object(hostmem, name, path)
            patcher.start()
            self.addCleanup(patcher.stop)

    def meminfo(self, avail_kib):
        (self.proc / 'meminfo').write_text(
            f'MemTotal:       65536000 kB\nMemFree:         1048576 kB\nMemAvailable:   {avail_kib} kB\n'
            'Buffers:          102400 kB\nCached:         20971520 kB\n')

    def member_of(self, path):
        (self.proc / 'self' / 'cgroup').write_text(f'0::{path}\n')

    def limit(self, path, maximum, current):
        """memory.max and memory.current of the cgroup at path (relative to the mount)."""
        folder = self.cgroup / path
        folder.mkdir(parents=True, exist_ok=True)
        (folder / 'memory.max').write_text(f'{maximum}\n')
        (folder / 'memory.current').write_text(f'{current}\n')

    def test_without_a_limit_it_is_mem_available(self):
        self.meminfo(10 * GIB // 1024)
        self.member_of('/user.slice/app.scope')
        self.limit('', 'max', 0)   # unlimited cgroups are not a cap
        self.limit('user.slice', 'max', 3 * GIB)
        self.limit('user.slice/app.scope', 'max', GIB)
        self.assertEqual(hostmem.available_ram(), 10 * GIB)

    def test_a_cgroup_without_memory_files_is_no_cap(self):
        self.meminfo(10 * GIB // 1024)
        self.member_of('/user.slice/app.scope')   # the root cgroup has no memory.max, and here nothing else has
        self.assertEqual(hostmem.available_ram(), 10 * GIB)

    def test_the_cgroup_limit_less_its_use_caps_it(self):
        self.meminfo(40 * GIB // 1024)
        self.member_of('/user.slice/app.scope')
        self.limit('user.slice/app.scope', 12 * GIB, 5 * GIB)
        self.assertEqual(hostmem.available_ram(), 7 * GIB)

    def test_a_limit_above_mem_available_changes_nothing(self):
        self.meminfo(6 * GIB // 1024)
        self.member_of('/app.scope')
        self.limit('app.scope', 64 * GIB, GIB)
        self.assertEqual(hostmem.available_ram(), 6 * GIB)

    def test_the_tightest_limit_up_the_tree_counts(self):
        self.meminfo(40 * GIB // 1024)
        self.member_of('/user.slice/user-1000.slice/app.scope')
        self.limit('user.slice/user-1000.slice/app.scope', 'max', GIB)
        self.limit('user.slice/user-1000.slice', 9 * GIB, 6 * GIB)   # 3 GiB left
        self.limit('user.slice', 30 * GIB, 2 * GIB)                  # 28 GiB left
        self.assertEqual(hostmem.available_ram(), 3 * GIB)

    def test_a_leaf_limit_tighter_than_its_parents(self):
        self.meminfo(40 * GIB // 1024)
        self.member_of('/a/b')
        self.limit('a/b', 4 * GIB, 3 * GIB)   # 1 GiB left
        self.limit('a', 20 * GIB, GIB)
        self.assertEqual(hostmem.available_ram(), GIB)

    def test_a_group_over_its_limit_has_nothing_left(self):
        self.meminfo(40 * GIB // 1024)
        self.member_of('/app.scope')
        self.limit('app.scope', 4 * GIB, 5 * GIB)
        self.assertEqual(hostmem.available_ram(), 0)

    def test_a_container_sees_its_own_limit_at_the_mount(self):
        # In a cgroup namespace the process is at "/" and the mount is its own cgroup.
        self.meminfo(40 * GIB // 1024)
        self.member_of('/')
        self.limit('', 8 * GIB, 2 * GIB)
        self.assertEqual(hostmem.available_ram(), 6 * GIB)

    def test_a_v1_system_has_only_mem_available(self):
        self.meminfo(10 * GIB // 1024)
        (self.proc / 'self' / 'cgroup').write_text('12:memory:/user.slice\n1:name=systemd:/user.slice\n')
        self.limit('', 2 * GIB, 0)
        self.assertEqual(hostmem.available_ram(), 10 * GIB)

    def test_unreadable_meminfo_is_none(self):
        self.member_of('/app.scope')
        self.limit('app.scope', 2 * GIB, 0)
        self.assertIsNone(hostmem.available_ram())
        (self.proc / 'meminfo').write_text('MemTotal: 1 kB\n')   # no MemAvailable (a kernel before 3.14)
        self.assertIsNone(hostmem.available_ram())

    @unittest.skipIf(gen is None, 'recomp/gen.py needs numpy')
    def test_the_lift_sizes_its_workers_by_the_limit(self):
        """A 32-processor machine under an 8 GiB cgroup gets 4 workers (8 GiB x 0.8 / 1.5 GiB), not 32."""
        self.meminfo(80 * GIB // 1024)
        self.member_of('/app.scope')
        self.limit('app.scope', 8 * GIB, 0)
        with mock.patch.dict(os.environ), mock.patch.object(gen.os, 'cpu_count', return_value=32):
            os.environ.pop('NX2NAVI_LIFT_JOBS', None)
            jobs, why = gen.pick_jobs(None)
        self.assertEqual(jobs, 4, why)


def cpuinfo(*flag_lists):
    """A /proc/cpuinfo with one processor entry per flags list."""
    return ''.join(f'processor\t: {i}\nvendor_id\t: GenuineIntel\nmodel name\t: Fake CPU\n'
                   f'flags\t\t: {flags}\nbogomips\t: 5990.40\n\n' for i, flags in enumerate(flag_lists))


V2 = 'fpu sse sse2 ssse3 sse4_1 sse4_2 popcnt'
V3 = V2 + ' avx avx2 fma bmi1 bmi2'
V4 = V3 + ' avx512f avx512bw avx512vl'


@LINUX
class CpuTagTest(Tmp):
    """cpu_tag reads the same three features as its Windows branch: SSE4.2, AVX2 and AVX-512F."""

    def tag(self, text):
        path = self.tmp / 'cpuinfo'
        path.write_text(text)
        with mock.patch.object(make_game, 'CPUINFO', path):
            return make_game.cpu_tag()

    def test_levels(self):
        self.assertEqual(self.tag(cpuinfo('fpu sse sse2')), '000')
        self.assertEqual(self.tag(cpuinfo(V2)), '100')
        self.assertEqual(self.tag(cpuinfo(V3)), '110')
        self.assertEqual(self.tag(cpuinfo(V4)), '111')

    def test_every_logical_processor_has_a_flags_line(self):
        self.assertEqual(self.tag(cpuinfo(*[V3] * 8)), '110')

    def test_a_feature_only_some_cores_have_is_not_counted(self):
        self.assertEqual(self.tag(cpuinfo(V4, V4, V3, V3)), '110')
        self.assertEqual(self.tag(cpuinfo(V3, V2)), '100')

    def test_a_longer_flag_name_is_not_a_match(self):
        self.assertEqual(self.tag(cpuinfo(V2 + ' avx2_vnni avx512f_extra')), '100')

    def test_unreadable_is_any(self):
        with mock.patch.object(make_game, 'CPUINFO', self.tmp / 'missing'):
            self.assertEqual(make_game.cpu_tag(), 'any')
        self.assertEqual(self.tag(''), 'any')
        self.assertEqual(self.tag('processor\t: 0\nFeatures\t: fp asimd evtstrm\n\n'), 'any')   # not x86

    def test_the_compile_key_follows_the_processor(self):
        """The same lift, runtime and kit compile again on a processor without AVX2."""
        def key(flags):
            path = self.tmp / 'cpuinfo'
            path.write_text(cpuinfo(flags))
            with mock.patch.object(make_game, 'CPUINFO', path):
                return make_game.step_key('compile', 'id', self.tmp)

        digests = {name: mock.Mock(return_value='x') for name in
                   ('exefs_digest', 'lift_digest', 'tree_digest', 'sha256_file', 'kit_version')}
        with mock.patch.multiple(make_game, **digests):
            self.assertEqual(key(V3), key(V3))
            self.assertEqual(len({key(V2), key(V3), key(V4)}), 3)


FAKE_INHIBIT = '''#!/bin/sh
# What systemd-inhibit does: take the lock (here: write the state), run the command, release the lock.
printf '%s\\n' "$@" > "{log}/args"
while [ "${{1#--}}" != "$1" ]; do shift; done
echo held > "{log}/state"
"$@"
echo released > "{log}/state"
'''


def wait_for(path, text, seconds=10):
    """True once the file's content is text."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            if path.read_text().strip() == text:
                return True
        except OSError:
            pass
        time.sleep(0.02)
    return False


@LINUX
class KeepAwakeTest(Tmp):
    """keep_awake with a systemd-inhibit stand-in first on PATH."""

    def setUp(self):
        super().setUp()
        self.bin, self.log = self.tmp / 'bin', self.tmp / 'log'
        self.bin.mkdir()
        self.log.mkdir()
        self.state = self.log / 'state'
        env = mock.patch.dict(os.environ, {'PATH': f'{self.bin}{os.pathsep}{os.environ["PATH"]}'})
        env.start()
        self.addCleanup(env.stop)
        awake = mock.patch.object(make_game, 'AWAKE', None)
        awake.start()
        self.addCleanup(awake.stop)
        self.addCleanup(self.release)

    def release(self):
        if make_game.AWAKE is not None:
            make_game.AWAKE.stdin.close()
            make_game.AWAKE.wait(timeout=10)

    def install(self, script=FAKE_INHIBIT):
        path = self.bin / 'systemd-inhibit'
        path.write_text(script.format(log=self.log))
        path.chmod(path.stat().st_mode | stat.S_IXUSR)

    def test_it_holds_sleep_until_released(self):
        self.install()
        make_game.keep_awake()
        self.assertTrue(wait_for(self.state, 'held'))
        args = (self.log / 'args').read_text().split()
        what = next(a for a in args if a.startswith('--what='))
        self.assertEqual(what, '--what=sleep')   # not idle: the screen may still blank and lock, as on Windows
        self.assertIn('--mode=block', args)
        time.sleep(0.3)
        self.assertIsNone(make_game.AWAKE.poll())   # still held: it does not end by itself
        self.assertEqual(self.state.read_text().strip(), 'held')
        first = make_game.AWAKE
        make_game.keep_awake()   # a second call takes no second lock
        self.assertIs(make_game.AWAKE, first)
        first.stdin.close()   # what the end of this process does
        first.wait(timeout=10)
        self.assertEqual(self.state.read_text().strip(), 'released')

    def test_a_killed_process_releases_it(self):
        self.install()
        code = ('import os, signal, sys, time\n'
                'from pathlib import Path\n'
                'sys.path.insert(0, sys.argv[1])\n'
                'import make_game\n'
                'make_game.keep_awake()\n'
                'state = Path(sys.argv[2])\n'
                'while not (state.is_file() and state.read_text().strip() == "held"):\n'
                '    time.sleep(0.02)\n'
                'os.kill(os.getpid(), signal.SIGKILL)\n')
        p = subprocess.run([sys.executable, '-c', code, str(ROOT / 'tools'), str(self.state)], timeout=30)
        self.assertEqual(p.returncode, -signal.SIGKILL)
        self.assertTrue(wait_for(self.state, 'released'))

    def test_without_systemd_inhibit_it_does_nothing(self):
        with mock.patch.dict(os.environ, {'PATH': str(self.bin)}):
            make_game.keep_awake()
        self.assertIsNone(make_game.AWAKE)

    def test_a_refused_lock_is_not_an_error(self):
        self.install('#!/bin/sh\necho "Failed to inhibit: no login manager" >&2\nexit 1\n')
        make_game.keep_awake()
        self.assertEqual(make_game.AWAKE.wait(timeout=10), 1)


if __name__ == '__main__':
    unittest.main()
