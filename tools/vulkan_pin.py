"""The Vulkan headers the runtime is compiled against where the system's are older.

The runtime names structures and constants of Vulkan extensions as they come out (runtime/vk_device.c asks for
VK_KHR_pipeline_binary, VK_KHR_dynamic_rendering_local_read and VK_KHR_shader_maximal_reconvergence when the driver
has them), so it needs headers at least as new as the ones it was written against. On Windows MSYS2 supplies them and
the Linux release (tools/make_release_linux.py) carries them in runtime/deps. A build with the system's own compiler
and headers (tools/make_appimage.py's kit, a source checkout) met distributions whose packaged headers are years
older: every file that names a newer extension then fails to compile. Such a build takes the headers from here
instead: the release the others use, from Khronos's Vulkan-Headers repository, checked against its SHA-256.

Only the C headers are kept (include/vulkan/*.h and include/vk_video/*.h, which vulkan_core.h includes) with the
repository's licence note. The headers are Apache-2.0 OR MIT (THIRD_PARTY_NOTICES.md). Standard library only: the
build kit's Python runs this.

    python tools/vulkan_pin.py [--dest toolchain/vulkan-headers]     fetch and unpack the pinned headers
"""
import argparse
import hashlib
import io
import re
import shutil
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION = '1.4.363'
HEADER_VERSION = 363   # VK_HEADER_VERSION of that release; the numbers only grow, across 1.3 and 1.4 alike
# (address, SHA-256): the archive GitHub makes of the release's tag
SOURCE = (f'https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v{VERSION}.tar.gz',
          'cbaf687d3c59b9666fe080e7f8396b6b4f4d344a768ee6b337c59852f4526b68')
TOP = f'Vulkan-Headers-{VERSION}/'                # the archive's top folder
FOLDER = Path('toolchain') / 'vulkan-headers'     # below a checkout's or a kit's root: include/ and LICENSE.md
STAMP = 'pinned.txt'                              # in the folder: the release it holds, written last
KEEP = re.compile(r'include/(vulkan|vk_video)/[A-Za-z0-9_]+\.h|LICENSE\.md')


def have(folder):
    """True when folder holds the pinned headers, complete (the stamp is written after the last file)."""
    try:
        return (folder / STAMP).read_text(encoding='ascii').strip() == VERSION and \
            (folder / 'include' / 'vulkan' / 'vulkan_core.h').is_file()
    except OSError:
        return False


def unpack(archive, folder):
    """The C headers and the licence note of the release's archive (bytes) into folder, replacing what is there."""
    shutil.rmtree(folder, ignore_errors=True)
    n = 0
    with tarfile.open(fileobj=io.BytesIO(archive), mode='r:gz') as tar:
        for m in tar:
            rel = m.name[len(TOP):] if m.name.startswith(TOP) else ''
            if not m.isfile() or not KEEP.fullmatch(rel):   # a name the pattern takes has no '..' and no other folder
                continue
            dst = folder / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(tar.extractfile(m).read())
            n += 1
    if not (folder / 'include' / 'vulkan' / 'vulkan_core.h').is_file() or not (folder / 'LICENSE.md').is_file():
        shutil.rmtree(folder, ignore_errors=True)
        sys.exit(f'vulkan_pin: the archive of Vulkan-Headers {VERSION} lacks vulkan_core.h or LICENSE.md')
    (folder / STAMP).write_text(VERSION + '\n', encoding='ascii')
    return n


def resolve(folder=None, archive=None):
    """The folder holding the pinned headers (default <root>/toolchain/vulkan-headers), fetched and unpacked there
    when it does not hold them yet. archive: the release's archive on disk instead of the download; it must be the
    pinned file either way. Returns the folder; its include/ goes on the include path."""
    folder = Path(folder) if folder else ROOT / FOLDER
    if have(folder):
        return folder
    url, want = SOURCE
    if archive:
        data = Path(archive).read_bytes()
    else:
        print(f'vulkan_pin: fetching the Vulkan headers {VERSION} into {folder}', flush=True)
        try:
            data = urllib.request.urlopen(url, timeout=120).read()
        except OSError as e:
            sys.exit(f'vulkan_pin: fetching {url} failed ({e}). Install Vulkan headers {VERSION} or newer, or put the '
                     f'archive on disk and run tools/vulkan_pin.py --archive <file>')
    if hashlib.sha256(data).hexdigest() != want:
        sys.exit(f'vulkan_pin: {archive or url} is not the pinned archive of Vulkan-Headers {VERSION} (SHA-256 differs)')
    unpack(data, folder)
    return folder


def header_version(text):
    """VK_HEADER_VERSION as vulkan_core.h defines it in text, 0 when it does not."""
    m = re.search(r'^[ \t]*#[ \t]*define[ \t]+VK_HEADER_VERSION[ \t]+(\d+)', text, re.M)
    return int(m.group(1)) if m else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--dest', type=Path, default=ROOT / FOLDER, help='where the headers go (default toolchain/vulkan-headers)')
    ap.add_argument('--archive', type=Path, help=f'the archive of Vulkan-Headers {VERSION} on disk instead of the download')
    a = ap.parse_args()
    folder = resolve(a.dest, a.archive)
    n = sum(1 for _ in (folder / 'include').rglob('*.h'))
    print(f'{folder}: Vulkan headers {VERSION}, {n} files')
    return 0


if __name__ == '__main__':
    sys.exit(main())
