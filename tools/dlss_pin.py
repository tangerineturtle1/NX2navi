"""NVIDIA's DLSS library for the releases: nvngx_dlss.dll for Windows, libnvidia-ngx-dlss.so.<version> for Linux.
tools/make_launcher.py and tools/make_kit.py put the DLL in the release's bin/ beside AMD's FSR 3.1 library,
tools/make_appimage.py puts the Linux library in the AppImage's kit bin/, and tools/make_game.py copies either beside
the game, where NGX loads it (runtime/vk_dlaa.c). Without it DLSS runs only where the NVIDIA driver supplies the model
itself (Windows), and on Linux not at all (runtime/resolution.c dlss_dll).

The file is NVIDIA's own build from the DLSS SDK repository at a pinned tag, unmodified and checked against its size
and SHA-256. NVIDIA's RTX SDKs License lets an application ship it in object form; the notice written beside it names
the version and the licence that covers it. Both platforms' files are published under the one licence file of that tag.
"""
from pathlib import Path
from typing import NamedTuple

NAME = 'nvngx_dlss.dll'
NOTICE = 'nvngx_dlss-NOTICE.txt'   # beside the DLL; make_game.py reads its sha256 line
VERSION = '310.9.1'
URL = f'https://raw.githubusercontent.com/NVIDIA/DLSS/v{VERSION}/lib/Windows_x86_64/rel/{NAME}'
SIZE = 58956912
SHA256 = '3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983'
# The Linux library of the same release. Its name ends in the DLSS version; NGX finds it by that name beside the
# executable (runtime/resolution.c dlss_dll looks for the prefix).
LINUX_NAME = f'libnvidia-ngx-dlss.so.{VERSION}'
LINUX_NOTICE = 'libnvidia-ngx-dlss-NOTICE.txt'   # beside the library; make_game.py reads its sha256 line
LINUX_URL = f'https://raw.githubusercontent.com/NVIDIA/DLSS/v{VERSION}/lib/Linux_x86_64/rel/{LINUX_NAME}'
LINUX_SIZE = 59481944
LINUX_SHA256 = '7561f16cab74e2b7ccf791bf44bb9cc43abb44bcf78f33c5663520ad4e857e02'
LICENSE_URL = f'https://github.com/NVIDIA/DLSS/blob/v{VERSION}/LICENSE.txt'


class Pin(NamedTuple):
    name: str
    notice: str
    url: str
    size: int
    sha256: str


def pin(linux=False):
    """The pinned file of a platform."""
    if linux:
        return Pin(LINUX_NAME, LINUX_NOTICE, LINUX_URL, LINUX_SIZE, LINUX_SHA256)
    return Pin(NAME, NOTICE, URL, SIZE, SHA256)


def notice_text(linux=False):
    p = pin(linux)
    return (f'{p.name} is NVIDIA DLSS {VERSION}, unmodified, as published in NVIDIA\'s DLSS SDK\n'
            f'(https://github.com/NVIDIA/DLSS, tag v{VERSION}). Copyright NVIDIA Corporation.\n'
            '\n'
            'It is not part of NX2navi and NX2navi\'s licence does not cover it. It is distributed under the\n'
            f'NVIDIA RTX SDKs License, and its use is subject to that licence: {LICENSE_URL}\n'
            'NVIDIA licenses DLSS only for use on systems with NVIDIA GPUs.\n'
            '\n'
            f'sha256 {p.sha256}\n')


def sha256_file(path):
    import hashlib
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while b := f.read(1 << 20):
            h.update(b)
    return h.hexdigest()


def resolve(local, cache_dir, linux=False):
    """The pinned library: local when given (it must be the pinned file), else the copy in cache_dir, downloaded when
    missing (launcher/net.py: resumed, checked against the size and SHA-256). linux: the Linux library, else the DLL."""
    p = pin(linux)
    if local:
        local = Path(local)
        if not local.is_file():
            raise SystemExit(f'{local} does not exist')
        if local.stat().st_size != p.size or sha256_file(local) != p.sha256:
            raise SystemExit(f'{local} is not NVIDIA DLSS {VERSION} (size or SHA-256 differ); a newer DLSS needs '
                             'VERSION, SIZE and SHA256 in tools/dlss_pin.py changed first')
        return local
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from launcher import net
    dest = Path(cache_dir) / (p.name if linux else f'nvngx_dlss-{VERSION}.dll')
    if not (dest.is_file() and sha256_file(dest) == p.sha256):
        print(f'download {p.url}', flush=True)
    net.download(p.url, dest, sha256=p.sha256, size=p.size)
    return dest


def place(src, bin_dir, linux=False):
    """The library and its notice into a release's or the kit's bin/."""
    import shutil
    p, bin_dir = pin(linux), Path(bin_dir)
    shutil.copy2(src, bin_dir / p.name)
    (bin_dir / p.notice).write_text(notice_text(linux), encoding='utf-8')
