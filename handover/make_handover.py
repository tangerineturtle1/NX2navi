"""Packs the handover as a release NX2navi's launcher installs by itself.

    python make_handover.py --version 0.12.0 [--setup TriforceSetup.exe --setup-version v0.3.6] [--notes notes.md]

Needs only Python 3.8 or later (no packages). It writes into release/:

    NX2navi-app-<version>.zip    the app archive: VERSION and launcher/ (the handover window, bridge/ of this folder)
    NX2navi-update.json          the manifest NX2navi's launcher reads (format 1: version, notes, and the archive's
                                 name, size and sha256)

Upload both files to a GitHub release v<version> of the repository the launchers ask (tangerineturtle1/NX2navi),
as a full release, not a pre-release: launchers on the Stable channel pass pre-releases over, and a version with a
pre-release tag (0.12.0-rc1) counts as one whatever the release says. <version> must be newer than every version
published so far, the previews included.

The manifest names no "runtime": the handover runs on whatever Python the installed launcher has, so no launcher
downloads a Python for it.
"""
import argparse
import hashlib
import json
import re
import shutil
import sys
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BRIDGE = ROOT / 'bridge'
VERSION = re.compile(r'^\d+(?:\.\d+){1,3}$')
NOTES = """NX2navi and Triforce have merged and go on as Triforce.

- This update turns NX2navi's launcher into the door to Triforce: one window that starts Triforce Setup, Triforce's launcher.
- Nothing of NX2navi is removed or changed: your built game, your saves and your settings stay where they are.
- Play NX2navi in that window still starts the game you have, for as long as you want it.
- Triforce builds the game from your own files, as NX2navi did, and its Convert saves page brings your NX2navi saves over.
"""


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--version', required=True, help='the release version, such as 0.12.0: plain numbers, newer than '
                                                     'every NX2navi version published so far')
    ap.add_argument('--setup', type=Path, help='another TriforceSetup.exe to carry than the one in bridge/')
    ap.add_argument('--setup-version', help='the Triforce release that TriforceSetup.exe is from (with --setup)')
    ap.add_argument('--notes', type=Path, help='release notes (markdown) for the manifest instead of the built-in ones')
    ap.add_argument('--out', type=Path, default=ROOT / 'release')
    a = ap.parse_args()
    if not VERSION.match(a.version):
        sys.exit(f'{a.version}: give plain numbers such as 0.12.0. A version with a tag (0.12.0-rc1) is a '
                 'pre-release to the launcher, which only the Preview channel installs.')
    ver = a.version
    setup_dir = BRIDGE / 'launcher' / 'triforce'
    info_file = setup_dir / 'setup.json'
    info = json.loads(info_file.read_text(encoding='utf-8')) if info_file.is_file() else {}
    if a.setup:
        if not a.setup_version:
            sys.exit('--setup needs --setup-version (the Triforce release the file is from)')
        shutil.copyfile(a.setup, setup_dir / 'TriforceSetup.exe')
        info['version'] = a.setup_version
    exe = setup_dir / 'TriforceSetup.exe'
    if not exe.is_file():
        sys.exit(f'{exe} is missing: give --setup')
    info = {'name': exe.name, 'version': info.get('version', ''), 'size': exe.stat().st_size,
            'sha256': sha256_file(exe), 'from': 'https://triforceproject.com'}
    info_file.write_text(json.dumps(info, indent=1) + '\n', encoding='utf-8')

    for need in ('start.pyw', 'handover.py', 'cli.py', 'selftest.py', 'art/hall_100.png'):
        if not (BRIDGE / 'launcher' / need).is_file():
            sys.exit(f'bridge/launcher/{need} is missing')
    a.out.mkdir(parents=True, exist_ok=True)
    for old in list(a.out.glob('NX2navi-app-*.zip')) + list(a.out.glob('NX2navi-update.json')):
        old.unlink()
    archive = a.out / f'NX2navi-app-{ver}.zip'
    stamp = time.localtime()[:6]
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.writestr(zipfile.ZipInfo('VERSION', stamp), ver + '\n')
        for p in sorted(BRIDGE.rglob('*')):
            if p.is_file() and '__pycache__' not in p.parts and p.name != 'VERSION':
                z.write(p, p.relative_to(BRIDGE).as_posix())
    notes = a.notes.read_text(encoding='utf-8') if a.notes else NOTES
    manifest = {'format': 1, 'version': ver, 'build': ver, 'notes': notes,
                'app': {'name': archive.name, 'size': archive.stat().st_size, 'sha256': sha256_file(archive)},
                'made': time.strftime('%Y-%m-%d %H:%M')}
    (a.out / 'NX2navi-update.json').write_text(json.dumps(manifest, indent=1), encoding='utf-8')
    with zipfile.ZipFile(archive) as z:
        names = z.namelist()
    print(f'{archive.name}: {archive.stat().st_size / 1e6:.2f} MB, {len(names)} files, sha256 {manifest["app"]["sha256"]}')
    print(f'carries {info["name"]} {info["version"]} ({info["size"]} bytes, sha256 {info["sha256"]})')
    print(f'NX2navi-update.json: version {ver}')
    print(f'upload both files from {a.out} to GitHub release v{ver} (a full release, not a pre-release)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
