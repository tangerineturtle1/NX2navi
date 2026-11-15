"""Check the shader translator guard of the packaging scripts (tools/build_xlate.py source_identity and check_built,
used by tools/make_launcher.py check_xlate, tools/make_release.py check_translator, tools/make_kit.py and
tools/make_appimage.py):

- a folder whose nx2navi_xlate.id names this checkout's sources passes; a missing exe or .id is refused;
- a folder built before a patch changed is refused, although identity() over its own prepared tree (what
  make_release.py compared before) equals its .id;
- build_xlate.py has no option that writes an .id beside an exe it did not build (--id-only is gone);
- make_launcher.check_xlate, make_release.check_translator and make_kit stop on a foreign translator, make_kit before
  it downloads or deletes anything; make_launcher and make_appimage run the check before their slow steps;
- tools/xlate_smoke.py pool_check, which make_launcher.check_xlate runs on the exe on Windows: spirv_clamps reads the
  array lengths and the UMin clamps of a module made here, pool_check refuses an exe that does not run, check_xlate
  refuses such an exe beside a matching .id, and a translator built here (build/xlate) passes when there is one.

Needs git on PATH (prepare_sources applies the patches with it) and tools/vendor. No game files; any Python 3.10+.

Usage: xlate_package_check.py
"""
import contextlib
import inspect
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tools'))
import build_xlate  # noqa: E402
import xlate_smoke  # noqa: E402

EXE = 'nx2navi_xlate.exe'


def folder(base, name, ident, exe=True):
    d = base / name
    d.mkdir(parents=True)
    if exe:
        (d / EXE).write_bytes(b'MZ stand-in, not a translator')
    if ident is not None:
        (d / 'nx2navi_xlate.id').write_text(ident + '\n', encoding='ascii')
    return d


def spirv(*instructions):
    """A SPIR-V module of these instructions, each (opcode, operand words...), after a five-word header."""
    words = [0x07230203, 0x00010300, 0, 100, 0]
    for op, *ops in instructions:
        words += [(len(ops) + 1) << 16 | op, *ops]
    return struct.pack(f'<{len(words)}I', *words)


def pool_checks(base):
    name = struct.unpack('<4I', b'GLSL.std.450\0\0\0\0')
    module = spirv((11, 1, *name), (43, 2, 3, 40960), (43, 2, 4, 40959), (43, 2, 5, 7), (28, 6, 2, 3),
                   (12, 2, 7, 1, 38, 5, 4), (12, 2, 8, 1, 37, 5, 3), (12, 2, 9, 99, 38, 5, 5))
    assert xlate_smoke.spirv_clamps(module) == ([40960], [7, 40959]), xlate_smoke.spirv_clamps(module)
    print('spirv_clamps: array lengths and the GLSL.std.450 UMin constants only')
    if os.name != 'nt':
        return
    stand_in = folder(base, 'stand_in', build_xlate.source_identity())
    why = xlate_smoke.pool_check(stand_in / EXE)
    assert why and 'does not run' in why, why
    why = refused(make_launcher_check(), stand_in)
    assert why and 'does not run' in why, why
    built = ROOT / 'build' / 'xlate' / EXE
    if built.is_file() and not build_xlate.check_built(built.parent, EXE):
        assert xlate_smoke.pool_check(built) is None, xlate_smoke.pool_check(built)
        print('pool_check: the translator built here passes')
    print('pool_check: an exe that does not run is refused, also by make_launcher.check_xlate beside a matching .id')


def make_launcher_check():
    import make_launcher
    return make_launcher.check_xlate


def refused(fn, *args):
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            fn(*args)
    except SystemExit as e:
        return str(e.code)
    return None


def main():
    if not shutil.which('git'):
        print('xlate package check: git is not on PATH; skipped')
        return 0
    with contextlib.redirect_stdout(io.StringIO()):
        want = build_xlate.source_identity()
    assert len(want) == 64 and build_xlate.BUILD == ROOT / 'build' / 'xlate', 'source_identity restores BUILD'

    with tempfile.TemporaryDirectory(prefix='xlate-pkg-') as t:
        base = Path(t)
        good = folder(base, 'good', want)
        no_exe = folder(base, 'no_exe', want, exe=False)
        no_id = folder(base, 'no_id', None)
        other = folder(base, 'other', '4d051299931d390e' + '0' * 48)

        # A folder built before a patch changed: its prepared tree differs from a fresh one by one file, and its .id
        # is identity() over that tree, as build_xlate.main wrote it then.
        stale = base / 'stale'
        old = build_xlate.BUILD
        build_xlate.BUILD = stale
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                tree = build_xlate.prepare_sources()
            victim = sorted(p for p in (tree / 'src').rglob('*.cs'))[0]
            victim.unlink()
            stale_id = build_xlate.identity()
        finally:
            build_xlate.BUILD = old
        (stale / EXE).write_bytes(b'MZ stand-in, not a translator')
        (stale / 'nx2navi_xlate.id').write_text(stale_id + '\n', encoding='ascii')
        assert stale_id != want

        def check(d):
            with contextlib.redirect_stdout(io.StringIO()):
                return build_xlate.check_built(d, EXE)

        assert check(good) is None
        assert 'missing' in check(no_exe)
        assert 'missing' in check(no_id)
        assert 'other sources' in check(other)
        assert 'other sources' in check(stale)
        print('check_built: this checkout\'s translator passes; missing files, another identity and a stale build '
              'are refused')

        # --id-only is gone: the option is refused and writes nothing.
        bare = folder(base, 'bare', None)
        r = subprocess.run([sys.executable, str(ROOT / 'tools' / 'build_xlate.py'), '--id-only', '--out', str(bare)],
                           capture_output=True, text=True)
        assert r.returncode != 0 and not (bare / 'nx2navi_xlate.id').exists(), r.stdout + r.stderr
        print('build_xlate.py: --id-only refused, no .id written')

        # The packaging scripts, with the fresh identity computed once.
        build_xlate.source_identity = lambda: want
        pool_checks(base)
        import make_launcher
        import make_release
        import make_kit
        xlate_smoke.pool_check = lambda exe: None   # the stand-in exes do not run: pool_checks covers the real one
        assert refused(make_launcher.check_xlate, good) is None
        for bad in (other, stale, no_exe):
            assert refused(make_launcher.check_xlate, bad)
        main_root = make_release.MAIN_ROOT
        try:
            for d, ok in ((good, True), (stale, False), (other, False)):
                rel = base / f'rel_{d.name}'
                shutil.copytree(d, rel / 'build' / 'xlate')
                make_release.MAIN_ROOT = rel
                assert (refused(make_release.check_translator) is None) == ok, d.name
        finally:
            make_release.MAIN_ROOT = main_root
        out = base / 'kit_out'
        argv = sys.argv
        try:
            sys.argv = ['make_kit.py', '--xlate', str(stale), '--out', str(out)]
            why = refused(make_kit.main)
        finally:
            sys.argv = argv
        assert why and 'other sources' in why and not out.exists()
        print('make_launcher, make_release and make_kit stop on a foreign translator (make_kit before any download)')

    # The check comes before the slow steps.
    src = inspect.getsource(make_launcher.main)
    assert src.index('check_xlate(a.xlate)') < src.index('shutil.rmtree(out') < src.index('toolchain.make_lock')
    import make_appimage
    src = inspect.getsource(make_appimage.main)
    assert src.index('build_xlate.check_built(') < src.index('dlss_pin.resolve(') < src.index('shutil.rmtree(app')
    src = inspect.getsource(make_release.main)
    assert src.index('check_translator()') < src.index('shutil.rmtree(out)')
    print('xlate package check: passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
