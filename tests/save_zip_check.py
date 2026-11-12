"""Build and run tests/save_zip_check.c: runtime/save_zip.c, the zip of a shared save as the game's menu writes and
reads it (runtime/save_share.c), against Python's zlib and zipfile, which the launcher uses for the same zip
(launcher/saves.py).

- the C program's selftest: the manifest's JSON keys, round trips in memory, damaged and cut input
- inflate: raw deflate streams zlib makes at every level and strategy (stored, fixed and dynamic blocks) inflate to
  the same bytes, and a damaged one is refused
- deflate: what the C deflate makes, zlib inflates to the same bytes
- zip: a zip the C writer makes passes zipfile's testzip with the same members; zips zipfile makes (stored, deflated,
  with data descriptors, the launcher's export) unzip in C to the same members with their CRCs; a link is marked
- the launcher imports a zip the C writer made (launcher/saves.py import_zip)

Usage: save_zip_check.py   (no game files)
"""
import io
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():
    """$CC, else MSYS2 UCRT64's clang, else clang on PATH (as tests/diff/common.py picks it, without its Unicorn)."""
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


class NoSeek(io.RawIOBase):
    """A write-only stream zipfile cannot seek in: it writes data descriptors after the members."""
    def __init__(self):
        self.buf = bytearray()

    def writable(self):
        return True

    def write(self, b):
        self.buf += b
        return len(b)


def samples():
    rnd = random.Random(7)
    log = ''.join(f'[{i:06d}] fs commit save: {rnd.randint(1, 20)} files, {rnd.randint(0, 9)} changed\n'
                  for i in range(4000)).encode()
    return {
        'empty': b'',
        'one': b'x',
        'text': log,
        'random': bytes(rnd.getrandbits(8) for _ in range(70000)),
        'runs': b'\0' * 100000 + b'\xff' * 3 + b'ab' * 40000,
        'save': bytes(rnd.choice(b'\0\0\0\1\2\x80\xff') for _ in range(150000)),
    }


def main():
    work = ROOT / 'build' / 'test' / 'save_zip'
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    exe = work / ('save_zip_check.exe' if os.name == 'nt' else 'save_zip_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'save_zip_check.c'), str(ROOT / 'runtime' / 'save_zip.c'), '-o',
                           str(exe)], env=env)
    run = lambda *a: subprocess.run([str(exe), *map(str, a)], env=env, capture_output=True, text=True)   # noqa: E731
    bad = 0

    def check(ok, what):
        nonlocal bad
        if not ok:
            bad += 1
            print('FAIL', what)

    r = run('selftest')
    print(r.stdout.strip())
    check(r.returncode == 0, 'selftest: ' + r.stderr.strip())

    data = samples()
    for name, raw in data.items():
        src = work / f'{name}.bin'
        src.write_bytes(raw)
        for level in (0, 1, 6, 9):
            for strategy in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FIXED, zlib.Z_HUFFMAN_ONLY, zlib.Z_RLE):
                c = zlib.compressobj(level, zlib.DEFLATED, -15, 9, strategy)
                packed = work / f'{name}-{level}-{strategy}.deflate'
                packed.write_bytes(c.compress(raw) + c.flush())
                out = work / 'out.bin'
                r = run('inflate', packed, len(raw), out)
                check(r.returncode == 0 and out.read_bytes() == raw, f'inflate {name} level {level} strategy {strategy}')
        out = work / f'{name}.c-deflate'
        r = run('deflate', src, out)
        try:
            got = zlib.decompress(out.read_bytes(), -15) if r.returncode == 0 else None
        except zlib.error as e:
            got = e
        check(got == raw, f'C deflate of {name} read by zlib: {got!r:.80}')
        if raw:
            print(f'  {name}: {len(raw)} bytes, C deflate {out.stat().st_size}, zlib -9 '
                  f'{len(zlib.compress(raw, 9)) - 6}')
    # a damaged stream: a flipped bit in a dynamic block, and one cut short
    packed = zlib.compressobj(9, zlib.DEFLATED, -15)
    stream = bytearray(packed.compress(data['text']) + packed.flush())
    for mutate in ('flip', 'cut'):
        s = bytearray(stream)
        if mutate == 'flip':
            s[len(s) // 2] ^= 0x10
        else:
            del s[len(s) * 2 // 3:]
        (work / 'bad.deflate').write_bytes(s)
        r = run('inflate', work / 'bad.deflate', len(data['text']), work / 'out.bin')
        check(r.returncode != 0 or (work / 'out.bin').read_bytes() != data['text'], f'damaged stream ({mutate}) read')

    # C writes, zipfile reads
    members = {'manifest.json': b'{"format": "nx2navi-save"}', 'save/option.sav': data['save'][:3000],
               'save/slot_00/progress.sav': data['save'], 'bug-report/nx2navi.log': data['text'],
               'save/empty.sav': b''}
    args = []
    for i, (n, b) in enumerate(members.items()):
        (work / f'm{i}').write_bytes(b)
        args += [n, work / f'm{i}']
    cz = work / 'c.zip'
    check(run('zip', cz, *args).returncode == 0, 'C zip')
    with zipfile.ZipFile(cz) as z:
        check(z.testzip() is None, 'zipfile testzip of the C zip')
        check({i.filename: z.read(i) for i in z.infolist()} == members, 'C zip members')
        check(z.getinfo('save/slot_00/progress.sav').compress_type == zipfile.ZIP_DEFLATED, 'deflated member')
        check(z.getinfo('manifest.json').compress_type == zipfile.ZIP_STORED, 'small member stored')

    # zipfile writes, C reads
    def unzip(path):
        out = work / 'unz'
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir()
        r = run('unzip', path, out)
        if r.returncode:
            return r.stdout.strip()
        got = {}
        for i, line in enumerate(r.stdout.splitlines()):
            name, crc, size, is_dir, is_link = line.rsplit('|', 4)
            got[name] = ((out / str(i)).read_bytes(), int(crc, 16), int(size), is_dir == '1', is_link == '1')
        return got

    for how in ('stored', 'deflated', 'descriptors'):
        if how == 'descriptors':
            stream = NoSeek()
            with zipfile.ZipFile(stream, 'w', zipfile.ZIP_DEFLATED) as z:
                for n, b in members.items():
                    with z.open(n, 'w') as f:
                        f.write(b)
            path = work / 'py-desc.zip'
            path.write_bytes(bytes(stream.buf))
        else:
            path = work / f'py-{how}.zip'
            with zipfile.ZipFile(path, 'w', zipfile.ZIP_STORED if how == 'stored' else zipfile.ZIP_DEFLATED) as z:
                for n, b in members.items():
                    z.writestr(n, b)
                z.writestr('save/folder/', b'')
                z.comment = b'a comment the end record carries'
        got = unzip(path)
        ok = isinstance(got, dict) and all(got[n][0] == b and got[n][1] == zlib.crc32(b) for n, b in members.items())
        check(ok, f'C unzip of a zipfile zip ({how}): {str(got)[:200]}')
        if how != 'descriptors':
            check(isinstance(got, dict) and got['save/folder/'][3], f'folder entry ({how})')
    # a link member, as a Unix zip tool stores one
    path = work / 'link.zip'
    with zipfile.ZipFile(path, 'w') as z:
        info = zipfile.ZipInfo('save/link.sav')
        info.create_system = 3
        info.external_attr = (0o120777) << 16
        z.writestr(info, '../../outside')
    got = unzip(path)
    check(isinstance(got, dict) and got['save/link.sav'][4], 'link marked')
    # a damaged member: refused by CRC or inflate
    raw = bytearray((work / 'py-deflated.zip').read_bytes())
    at = raw.index(b'save/slot_00/progress.sav') + 40
    raw[at] ^= 0x21
    (work / 'bad.zip').write_bytes(raw)
    got = unzip(work / 'bad.zip')
    check(isinstance(got, str) and got.splitlines()[-1] == 'refused 2 save/slot_00/progress.sav',
          f'damaged zip read: {str(got)[:120]}')

    # the launcher imports what the game's menu writes
    from launcher import saves
    manifest = json.dumps({'format': 'nx2navi-save', 'format_version': 1, 'title_id': '0400486024dec000',
                           'nx2navi': '0.6.1', 'exported': '2026-11-09T17:30:05Z', 'exported_by': 'game'}).encode()
    (work / 'manifest').write_bytes(manifest)
    (work / 'opt').write_bytes(b'option')
    (work / 'prog').write_bytes(data['save'])
    gz = work / 'game.zip'
    check(run('zip', gz, 'manifest.json', work / 'manifest', 'save/option.sav', work / 'opt',
              'save/slot_03/progress.sav', work / 'prog').returncode == 0, 'C zip of a save')
    game = Path(tempfile.mkdtemp(prefix='nx2navi-zipcheck-'))
    try:
        r = saves.import_save(game, gz, log=lambda m: None)
        acct = saves.account_dir(game)
        check(r['manifest']['exported_by'] == 'game' and (acct / 'slot_03' / 'progress.sav').read_bytes() ==
              data['save'], 'launcher import of the C zip')
    finally:
        shutil.rmtree(game, ignore_errors=True)

    print('save_zip_check:', 'FAILED' if bad else 'ok')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
