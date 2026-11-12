"""NVIDIA Streamline's runtime for DLSS Frame Generation (runtime/vk_dlssg.c): tools/make_launcher.py and
tools/make_kit.py put it in the release's or the kit's bin/dlssg/ beside nx2navi_dlssg.dll (tools/build_dlssg.py), and
tools/make_game.py place_dlssg copies the whole set beside the game.

The files are NVIDIA's own builds from the Streamline SDK release at a pinned version, unmodified: the SDK archive is
downloaded once, checked against its size and SHA-256, and only the files the game loads are kept, each checked
against its own SHA-256. The sl.*.dll files are under Streamline's MIT licence; nvngx_dlssg.dll is under the NVIDIA RTX
SDKs License, which licenses it only for use on systems with NVIDIA GPUs. The notice written beside them names the
version and each file's sha256.
"""
import shutil
import sys
import zipfile
from pathlib import Path

VERSION = '2.14.16'
URL = f'https://github.com/NVIDIA-RTX/Streamline/releases/download/v{VERSION}/streamline-sdk-v{VERSION}.zip'
SIZE = 276016170
SHA256 = '32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b'

DLL = 'nx2navi_dlssg.dll'   # this repository's runtime/dlssg/, built by tools/build_dlssg.py
# The set tools/make_game.py DLSSG_FILES names: without all of it the game leaves DLSS Frame Generation off.
FILES = (DLL, 'sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll', 'sl.pcl.dll', 'sl.reflex.dll', 'nvngx_dlssg.dll')
NOTICE = 'dlssg-NOTICE.txt'
# The SDK's files the set takes, by their path in the SDK, with their sha256.
SDK_FILES = {
    'bin/x64/sl.interposer.dll': 'b1d7395aebd7b9722841d64dcf7433680020b6a8e421f851c16d5ba0c2182edf',
    'bin/x64/sl.common.dll': '38eea3b4209afc455636ea372b5933d65fcc13becec6c84d101dcfc4d8648f75',
    'bin/x64/sl.dlss_g.dll': '3ed9538df724993bea8a050195a6a78b9c8c8a566d5b05ade477aab3c390e90f',
    'bin/x64/sl.pcl.dll': '5698c38e722318681f215c9fe68e45b9bedd51378acc699d8eee26e961e57770',
    'bin/x64/sl.reflex.dll': '443577e869352436ff7ec8f4d9c90418d3e95368436ce219261e0f4f07a38122',
    'bin/x64/nvngx_dlssg.dll': '8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7',
    'license.txt': '7b6f23e7d6f3ad6292f9308d2b42cdc3d82ae4e9b2abb55f230279d83bedd43d',
    'bin/x64/nvngx_dlss.license.txt': '3027f23ca5a46dd9cb8183fbd522983a86f64d7daac5982912bf9f214671f294',
}
# The licence texts beside the DLLs, by the SDK file each is copied from.
TEXTS = {'streamline-LICENSE.txt': 'license.txt', 'nvidia-rtx-sdks-LICENSE.txt': 'bin/x64/nvngx_dlss.license.txt'}


def sha256_file(path):
    import hashlib
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while b := f.read(1 << 20):
            h.update(b)
    return h.hexdigest()


def wrong_files(sdk):
    """The pinned SDK files that are missing from sdk or differ from the pin."""
    sdk = Path(sdk)
    return [n for n, h in SDK_FILES.items() if not (sdk / n).is_file() or sha256_file(sdk / n) != h]


def resolve(sdk, cache_dir):
    """A folder laid out as the SDK is that holds the pinned files: sdk when given (its files must be the pinned
    ones), else cache_dir/streamline-<version>, filled from the SDK archive when missing (downloaded with
    launcher/net.py: resumed, checked against the size and SHA-256, and deleted once its files are out)."""
    if sdk:
        sdk = Path(sdk)
        if not sdk.is_dir():
            raise SystemExit(f'{sdk} does not exist')
        bad = wrong_files(sdk)
        if bad:
            raise SystemExit(f'{sdk} is not the Streamline {VERSION} SDK ({", ".join(bad)} missing or different); '
                             'another Streamline needs VERSION, SIZE, SHA256 and SDK_FILES in tools/streamline_pin.py '
                             'changed first')
        return sdk
    dest = Path(cache_dir) / f'streamline-{VERSION}'
    if not wrong_files(dest):
        return dest
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from launcher import net
    archive = Path(cache_dir) / f'streamline-sdk-v{VERSION}.zip'
    if not (archive.is_file() and archive.stat().st_size == SIZE):
        print(f'download {URL} ({SIZE / 1e6:.0f} MB)', flush=True)
    net.download(URL, archive, sha256=SHA256, size=SIZE)
    extract(archive, dest)
    archive.unlink()
    return dest


def extract(archive, dest):
    """The pinned files out of the SDK archive into dest, each checked against its sha256."""
    import hashlib
    with zipfile.ZipFile(archive) as z:
        names = {n.replace('\\', '/'): n for n in z.namelist()}
        for n, h in SDK_FILES.items():
            if n not in names:
                raise SystemExit(f'{archive.name} has no {n}')
            data = z.read(names[n])
            if hashlib.sha256(data).hexdigest() != h:
                raise SystemExit(f'{n} in {archive.name} differs from the pinned file')
            (dest / n).parent.mkdir(parents=True, exist_ok=True)
            (dest / n).write_bytes(data)


def place(dll, sdk, dest):
    """DLSS Frame Generation into dest (a release's or the kit's bin/dlssg/): nx2navi_dlssg.dll, Streamline's
    interposer and the plugins it loads, and NGX's nvngx_dlssg.dll from the SDK folder resolve returned, the two
    licence texts, and a notice naming each file's sha256."""
    dll, sdk, dest = Path(dll), Path(sdk), Path(dest)
    if not dll.is_file():
        raise SystemExit(f'{dll} does not exist')
    srcs = {n: (dll if n == DLL else sdk / 'bin' / 'x64' / n) for n in FILES}
    dest.mkdir(parents=True, exist_ok=True)
    lines = ['DLSS Frame Generation for NX2navi. nx2navi_dlssg.dll is NX2navi\'s own code (runtime/dlssg/), built',
             'with the Streamline SDK\'s headers. The other files are NVIDIA\'s own builds, unmodified, from the',
             f'Streamline {VERSION} SDK (https://github.com/NVIDIA-RTX/Streamline): the sl.*.dll files under its',
             'MIT licence (streamline-LICENSE.txt), nvngx_dlssg.dll under the NVIDIA RTX SDKs License',
             '(nvidia-rtx-sdks-LICENSE.txt), which licenses it only for use on systems with NVIDIA GPUs. They are not',
             'part of NX2navi and NX2navi\'s licence does not cover them.', '']
    for n, p in srcs.items():
        shutil.copy2(p, dest / n)
        lines.append(f'{n} sha256 {sha256_file(p)}')
    for n, src in TEXTS.items():
        shutil.copy2(sdk / src, dest / n)
    (dest / NOTICE).write_text('\n'.join(lines) + '\n', encoding='utf-8')
