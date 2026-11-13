"""The build tools, installed on the player's PC: MSYS2's UCRT64 clang and the libraries tools/make_game.py links
(the packages README-BuildKit.md lists), and the zbic Python package.

No MSYS2 shell and no pacman: the packages and everything they depend on are read from the repository's database,
downloaded, checked against the sha256 the database (or the release's lock) names, and unpacked into
toolchain/msys64/ucrt64 as pacman would lay them out. A release pins the exact package files in
toolchain.lock.json (make_lock, written by tools/make_launcher.py), so every player builds with the compiler the
release was tested with; the repository keeps old package files. A checkout without a lock resolves the newest
packages instead.

Beside it goes the clang of MSYS2's CLANG64 environment (toolchain/msys64/clang64), the same LLVM release built with
profile-guided optimisation: it compiles the lifted code about a fifth faster into the same objects, reading UCRT64's
headers (tools/make_game.py fast_clang). Only clang.exe, the DLLs it loads and its own headers are unpacked.

zbic (GPL-2.0) unpacks the game's compressed program files. The release does not carry it: the launcher downloads the
wheel for its own Python from PyPI and unpacks it into pydeps/, as `pip install zbic` would.
"""
import hashlib
import importlib
import io
import json
import os
import re
import shutil
import sys
import tarfile
import time
import zipfile
from pathlib import Path

from . import net

REPO = 'https://repo.msys2.org/mingw/ucrt64/'
MIRRORS = (REPO, 'https://mirror.msys2.org/mingw/ucrt64/')
PREFIX = 'mingw-w64-ucrt-x86_64-'
ROOTS = ('clang', 'gcc', 'lld', 'sdl3', 'opus', 'libvpx', 'dav1d', 'zstd', 'vulkan-headers', 'vulkan-loader',
         'vulkan-memory-allocator')   # lld: the game links several times faster than with GNU ld
REQUIRED = ('bin/clang.exe', 'bin/clang++.exe', 'bin/SDL3.dll', 'lib/libSDL3.dll.a', 'include/vma')
SKIP_SHARE = ('ucrt64/share/doc/', 'ucrt64/share/man/', 'ucrt64/share/info/', 'ucrt64/share/locale/',
              'ucrt64/share/gtk-doc/', 'ucrt64/share/licenses/')
# CLANG64's clang: exactly the packages clang.exe loads DLLs from (not the dependency closure, which adds the linker,
# the LLVM tools and the CLANG64 C runtime the lifted code does not use).
FAST_REPO = 'https://repo.msys2.org/mingw/clang64/'
FAST_MIRRORS = (FAST_REPO, 'https://mirror.msys2.org/mingw/clang64/')
FAST_PREFIX = 'mingw-w64-clang-x86_64-'
FAST_PACKAGES = ('clang', 'clang-libs', 'llvm-libs', 'libc++', 'libffi', 'zlib', 'zstd', 'libxml2', 'libiconv')
ZBIC = ('zbic', '1.0.1.post1')
LOCK_FORMAT = 1


class ToolchainError(Exception):
    pass


# ---------------------------------------------------------------- the package database

def zstd_missing(layout):
    """Why this Python cannot read zstd files, or '' when it can. The MSYS2 packages and the game's files are zstd:
    Python 3.14 reads them with compression.zstd, an older Python needs the zstandard package. The release's Python
    has both; a checkout runs under whatever Python started it, and without either the build stopped at its first
    unpack with nothing but "No module named 'zstandard'"."""
    for name in ('compression.zstd', 'zstandard'):
        try:
            importlib.import_module(name)
            return ''
        except ImportError:
            pass
    ver = '.'.join(map(str, sys.version_info[:3]))
    if not layout.dev:
        return f'The launcher\'s Python ({ver}) is missing its zstandard package. Reinstall NX2navi.'
    req = layout.app / 'tools' / 'requirements-launcher.txt'
    return (f'This Python ({ver}) cannot unpack the build tools: before 3.14 it needs the zstandard package. '
            f'Install the launcher\'s packages with: "{sys.executable}" -m pip install -r "{req}"')


def _zstd_decompress_stream(data):
    try:
        from compression import zstd   # Python 3.14
        return zstd.decompress(data)
    except ImportError:
        import zstandard
        return zstandard.ZstdDecompressor().decompressobj().decompress(data)


def _open_tar(path_or_bytes):
    """A tarfile over a .tar.zst (or plain/gzip tar) file or bytes."""
    data = path_or_bytes if isinstance(path_or_bytes, (bytes, bytearray)) else Path(path_or_bytes).read_bytes()
    if data[:4] == b'\x28\xb5\x2f\xfd':
        data = _zstd_decompress_stream(data)
    return tarfile.open(fileobj=io.BytesIO(data), mode='r:*')


def parse_db(data):
    """{name: desc} for a pacman sync database (ucrt64.db): each desc is {FIELD: [lines]}."""
    out = {}
    with _open_tar(data) as t:
        for m in t.getmembers():
            if not m.name.endswith('/desc'):
                continue
            desc, field = {}, None
            for line in t.extractfile(m).read().decode('utf-8').splitlines():
                if line.startswith('%') and line.endswith('%') and len(line) > 2:
                    field = line[1:-1]
                    desc[field] = []
                elif line and field:
                    desc[field].append(line)
            if desc.get('NAME'):
                out[desc['NAME'][0]] = desc
    return out


def _dep_name(dep):
    return re.split(r'[<>=:]', dep, maxsplit=1)[0].strip()


def resolve(db, roots=ROOTS):
    """Every package the roots need, roots first; names a package provides resolve to the package."""
    provides = {}
    for name, d in db.items():
        for p in d.get('PROVIDES', []):
            provides.setdefault(_dep_name(p), name)
    order, seen = [], set()
    todo = [r if r.startswith(PREFIX) else PREFIX + r for r in roots]
    while todo:
        n = _dep_name(todo.pop(0))
        n = n if n in db else provides.get(n, n)
        if n in seen:
            continue
        if n not in db:
            raise ToolchainError(f'the package database has no {n}')
        seen.add(n)
        order.append(n)
        todo += db[n].get('DEPENDS', [])
    return [_pkg(db, n) for n in order]


def make_lock(repo=REPO, roots=ROOTS, version='dev', fast_repo=None):
    """The lock a release ships: the resolved package files of the repository's current database. With fast_repo
    (FAST_REPO), also CLANG64's clang, when that repository has the same clang version as UCRT64's."""
    db = parse_db(net.get_bytes(repo + 'ucrt64.db', version=version))
    pkgs = resolve(db, roots)
    lock = {'format': LOCK_FORMAT, 'repo': repo, 'roots': list(roots), 'packages': pkgs,
            'zbic': {'name': ZBIC[0], 'version': ZBIC[1]}, 'made': time.strftime('%Y-%m-%d')}
    if fast_repo:
        fdb = parse_db(net.get_bytes(fast_repo + 'clang64.db', version=version))
        fast = [_pkg(fdb, FAST_PREFIX + n, env='clang64') for n in FAST_PACKAGES]
        mine = next(p['version'] for p in pkgs if p['name'] == PREFIX + 'clang')
        if fast[0]['version'] == mine:
            lock['packages'] = pkgs + fast
            lock['fast_repo'] = fast_repo
        else:
            lock['fast_skipped'] = f'CLANG64 has clang {fast[0]["version"]}, UCRT64 {mine}'
    return lock


def _pkg(db, name, env=None):
    if name not in db:
        raise ToolchainError(f'the package database has no {name}')
    d = db[name]
    p = {'name': name, 'version': d['VERSION'][0], 'filename': d['FILENAME'][0], 'sha256': d['SHA256SUM'][0],
         'csize': int(d['CSIZE'][0]), 'isize': int(d.get('ISIZE', ['0'])[0])}
    if env:
        p['env'] = env
    return p


def lock_digest(lock):
    files = sorted(p['filename'] + ':' + p['sha256'] for p in lock['packages'])
    return hashlib.sha256('\n'.join(files).encode()).hexdigest()


def load_lock(layout, version='dev'):
    """The release's toolchain.lock.json, or a lock resolved from the live database in a checkout."""
    if layout.lock_file.is_file():
        lock = json.loads(layout.lock_file.read_text(encoding='utf-8'))
        if lock.get('format') != LOCK_FORMAT:
            raise ToolchainError(f'{layout.lock_file}: unknown format {lock.get("format")}')
        return lock
    return make_lock(version=version)


# ---------------------------------------------------------------- install

def installed(layout):
    p = layout.msys_root / 'installed.json'
    try:
        return json.loads(p.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None


def missing_files(msys):
    return [r for r in REQUIRED if not (Path(msys) / r).exists()]


def is_current(layout, lock):
    inst = installed(layout)
    return bool(inst) and inst.get('digest') == lock_digest(lock) and not missing_files(layout.msys)


def _wanted(name, env):
    """Whether a package member is unpacked: UCRT64's files without the documentation; of CLANG64's, clang.exe, the
    DLLs and clang's own headers."""
    if env == 'clang64':
        return (name == 'clang64/bin/clang.exe' or (name.startswith('clang64/bin/') and name.endswith('.dll'))
                or name.startswith('clang64/lib/clang/'))
    return name.startswith('ucrt64/') and not name.startswith(SKIP_SHARE)


def _members(path):
    """The members of a .pkg.tar.zst (or plain tar), read as a stream, member by member, so a 350 MB package never
    sits in memory whole. Yields (tarfile, member)."""
    with open(path, 'rb') as raw:
        zst = raw.read(4) == b'\x28\xb5\x2f\xfd'
        raw.seek(0)
        if zst:
            try:
                from compression import zstd   # Python 3.14
                fileobj = zstd.ZstdFile(raw)
            except ImportError:
                import zstandard
                fileobj = zstandard.ZstdDecompressor().stream_reader(raw)
        else:
            fileobj = raw
        with tarfile.open(fileobj=fileobj, mode='r|*') as t:
            for m in t:
                yield t, m


def _extract_package(path, dest, env=None):
    """Unpack one package into dest (the msys64 folder): the members _wanted keeps. A wanted hard link needs its
    target on disk, and a stream cannot go back for it: a selective unpack (env) first reads the headers for the
    targets of the wanted links (CLANG64's clang.exe is one) and unpacks them too."""
    want = set()
    if env:
        for _, m in _members(path):
            if m.islnk() and _wanted(m.name.lstrip('./'), env):
                want.add(m.linkname.lstrip('./'))
    for t, m in _members(path):
        name = m.name.lstrip('./')
        if name in want or _wanted(name, env):
            t.extract(m, dest, filter='data')


def install(layout, lock, progress=None, cancel=None, log=print, version='dev'):
    """Download and unpack the lock's packages into toolchain/msys64 (through msys64.new, swapped in at the end).
    progress(phase, done, total) with phase 'download' (bytes) or 'unpack' (packages)."""
    pkgs = lock['packages']
    layout.pkgs.mkdir(parents=True, exist_ok=True)
    need = [p for p in pkgs if not ((layout.pkgs / p['filename']).is_file()
                                    and (layout.pkgs / p['filename']).stat().st_size == p['csize'])]
    total = sum(p['csize'] for p in need)
    log(f'toolchain: {len(pkgs)} packages, {len(need)} to download ({total / 1e6:.0f} MB)')
    # Room for the downloads and for the unpacked tools beside the ones in use (msys64.new), asked before the first
    # download: a disk that fills while a package unpacks otherwise ends the step with an error that names no cause.
    # A package's installed size is in the lock; of a CLANG64 package only a few files are unpacked (_wanted), for
    # which its download size stands in.
    unpacked = sum(p['csize'] if p.get('env') else p.get('isize', 0) for p in pkgs)
    full = net.space_problem(layout.toolchain, total + unpacked, 'the build tools')
    if full:
        raise ToolchainError(full)
    done_before = 0
    for p in need:
        dest = layout.pkgs / p['filename']
        last_error = None
        first, others = ((lock.get('fast_repo', FAST_REPO), FAST_MIRRORS) if p.get('env') == 'clang64'
                         else (lock.get('repo', REPO), MIRRORS))
        for mirror in [first] + [m for m in others if m != first]:
            try:
                net.download(mirror + p['filename'], dest, sha256=p['sha256'], size=p['csize'], cancel=cancel,
                             progress=(lambda d, t, base=done_before: progress and progress('download', base + d, total)),
                             version=version, log=lambda line: log(f'toolchain: {line}'))
                last_error = None
                break
            except net.NoSpace as e:   # another server has the same file for the same full disk
                raise ToolchainError(str(e)) from e
            except net.DownloadError as e:
                last_error = e
                log(f'toolchain: {e}')
        if last_error:
            raise ToolchainError(f'{p["filename"]} could not be downloaded: {last_error}')
        done_before += p['csize']
    for p in pkgs:   # files kept from an earlier download are checked too
        if net.sha256_file(layout.pkgs / p['filename']) != p['sha256']:
            (layout.pkgs / p['filename']).unlink()
            raise ToolchainError(f'{p["filename"]} was damaged on disk; run the setup again to download it anew')
    staging = layout.toolchain / 'msys64.new'
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    for i, p in enumerate(pkgs):
        if cancel and cancel():
            raise net.Cancelled()
        if progress:
            progress('unpack', i, len(pkgs))
        _extract_package(layout.pkgs / p['filename'], staging, p.get('env'))
    if progress:
        progress('unpack', len(pkgs), len(pkgs))
    missing = missing_files(staging / 'ucrt64')
    if missing:
        raise ToolchainError(f'the packages lack {", ".join(missing)}')
    (staging / 'installed.json').write_text(json.dumps({
        'digest': lock_digest(lock), 'packages': [f'{p["name"]} {p["version"]}' for p in pkgs],
        'installed': time.strftime('%Y-%m-%d %H:%M')}, indent=1), encoding='utf-8')
    old = layout.toolchain / f'msys64.old-{int(time.time())}'
    if layout.msys_root.exists():
        os.replace(layout.msys_root, old)
    os.replace(staging, layout.msys_root)
    shutil.rmtree(old, ignore_errors=True)
    keep = {p['filename'] for p in pkgs}
    for f in layout.pkgs.iterdir():
        if f.name not in keep:
            f.unlink(missing_ok=True)
    log(f'toolchain: installed in {layout.msys}')


# ---------------------------------------------------------------- zbic

def wheel_tag():
    """The wheel platform tags this interpreter loads, best first."""
    v = f'cp{sys.version_info.major}{sys.version_info.minor}'
    if os.name == 'nt':
        return [f'{v}-{v}-win_amd64']
    return [f'{v}-{v}-manylinux', f'{v}-{v}-musllinux']


def zbic_ok(layout):
    """zbic loads from pydeps/ in the interpreter the build runs with (a fresh process: this one may have cached
    that pydeps/ did not exist)."""
    import subprocess
    if not layout.pydeps.is_dir():
        return False
    code = 'import sys; sys.path.insert(0, sys.argv[1]); import zbic; assert callable(zbic.decompress)'
    flags = {'creationflags': 0x08000000} if os.name == 'nt' else {}   # CREATE_NO_WINDOW
    r = subprocess.run([layout.python_for_builds(), '-I', '-c', code, str(layout.pydeps)], capture_output=True,
                       **flags)
    return r.returncode == 0


def ensure_zbic(layout, progress=None, cancel=None, log=print, version='dev'):
    if zbic_ok(layout):
        return
    name, ver = ZBIC
    info = net.get_json(f'https://pypi.org/pypi/{name}/{ver}/json', version=version)
    files = [u for u in info.get('urls', []) if u['filename'].endswith('.whl')]
    pick = None
    for tag in wheel_tag():
        for u in files:
            if tag in u['filename'] and ('x86_64' in u['filename'] or 'amd64' in u['filename']):
                pick = u
                break
        if pick:
            break
    if not pick:
        raise ToolchainError(f'PyPI has no {name} {ver} wheel for Python {sys.version_info.major}.'
                             f'{sys.version_info.minor} on this system')
    dest = layout.downloads / pick['filename']
    net.download(pick['url'], dest, sha256=pick['digests']['sha256'], size=pick.get('size'), cancel=cancel,
                 progress=(lambda d, t: progress and progress('download', d, t)), version=version)
    shutil.rmtree(layout.pydeps, ignore_errors=True)
    layout.pydeps.mkdir(parents=True)
    with zipfile.ZipFile(dest) as z:
        for m in z.infolist():
            target = (layout.pydeps / m.filename).resolve()
            if not target.is_relative_to(layout.pydeps.resolve()):   # not a sibling such as pydeps2 either
                raise ToolchainError(f'{pick["filename"]}: unsafe path {m.filename}')
        z.extractall(layout.pydeps)
    dest.unlink(missing_ok=True)
    if not zbic_ok(layout):
        raise ToolchainError(f'{name} {ver} was unpacked into {layout.pydeps} but does not load')
    log(f'zbic: {pick["filename"]}')
