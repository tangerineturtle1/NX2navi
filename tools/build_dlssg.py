"""Build nx2navi_dlssg.dll (runtime/dlssg/nx2_sl.cpp, DLSS Frame Generation through NVIDIA Streamline) with MSVC.

    python tools/build_dlssg.py --sl <Streamline SDK folder with include/> [--out build/dlssg]
    python tools/build_dlssg.py --check <nx2navi_dlssg.dll>

Streamline's C++ types have MSVC's ABI, so this one DLL is built with Visual Studio's compiler (the Build Tools are
enough; vswhere finds them), against the SDK's headers, which stay where they are. The Vulkan headers are MSYS2's.
A launcher release and the build kit ship the DLL ready-made beside Streamline's own runtime DLLs
(tools/make_launcher.py and tools/make_kit.py --dlssg, with the Streamline files tools/streamline_pin.py pins), and
tools/make_game.py place_dlssg puts them beside the game.

The built DLL must export every function runtime/dlssg/nx2_sl.h declares (each PFN_nx2sl_<name> is the export
nx2sl_<name>), or the build fails naming the ones it lacks: the runtime takes a missing optional export for an older
DLL and leaves its feature out without an error. --check makes the same check of a DLL built before, such as the one
a release is about to ship; tools/make_launcher.py and tools/make_kit.py make it on their --dlssg DLL first and stop
on one that fails.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'runtime' / 'dlssg' / 'nx2_sl.h'
VSWHERE = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio' / 'Installer' / 'vswhere.exe'


def vcvars():
    if not VSWHERE.is_file():
        sys.exit('Visual Studio (or its Build Tools) is not installed: vswhere.exe is missing')
    path = subprocess.run([str(VSWHERE), '-latest', '-products', '*', '-requires',
                           'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
                          capture_output=True, text=True).stdout.strip()
    bat = Path(path) / 'VC' / 'Auxiliary' / 'Build' / 'vcvars64.bat'
    if not bat.is_file():
        sys.exit(f'no vcvars64.bat under {path or "(no Visual Studio with the C++ tools)"}')
    return bat


def declared_exports(header=HEADER):
    """The exports nx2_sl.h declares: nx2sl_<name> for each function pointer type PFN_nx2sl_<name>."""
    text = Path(header).read_text(encoding='utf-8')
    return {'nx2sl_' + name for name in re.findall(r'\(\s*\*\s*PFN_nx2sl_(\w+)\s*\)', text)}


def pe_exports(path):
    """The names a PE file (PE32 or PE32+) exports: its export directory's name table. Empty without one."""
    d = Path(path).read_bytes()
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    if d[pe:pe + 4] != b'PE\0\0':
        raise ValueError(f'{path} is not a PE file')
    nsec, opt = struct.unpack_from('<H', d, pe + 6)[0], struct.unpack_from('<H', d, pe + 20)[0]
    magic = struct.unpack_from('<H', d, pe + 24)[0]
    sec = pe + 24 + opt
    # data directory 0, the exports: 112 bytes into a PE32+ optional header, 96 into a PE32 one
    rva = struct.unpack_from('<I', d, pe + 24 + (112 if magic == 0x20b else 96))[0]

    def off(r):
        for i in range(nsec):
            va, vs, raw = (struct.unpack_from('<I', d, sec + 40 * i + k)[0] for k in (12, 8, 20))
            if va <= r < va + max(vs, 1):
                return r - va + raw
        return None

    at = off(rva) if rva else None
    if at is None:
        return set()
    n, names = struct.unpack_from('<I', d, at + 24)[0], off(struct.unpack_from('<I', d, at + 32)[0])
    out = set()
    for i in range(n):
        o = off(struct.unpack_from('<I', d, names + 4 * i)[0])
        out.add(d[o:d.index(b'\0', o)].decode('ascii'))
    return out


def missing_exports(dll, header=HEADER):
    """The exports header declares that dll lacks, sorted."""
    return sorted(declared_exports(header) - pe_exports(dll))


def check(dll):
    """Exit naming the exports nx2_sl.h declares that dll lacks, or saying that dll is missing or no PE file.
    tools/make_launcher.py and tools/make_kit.py run it on their --dlssg DLL before anything is downloaded or built."""
    if not Path(dll).is_file():
        sys.exit(f'{dll} is missing')
    try:
        missing = missing_exports(dll)
    except (ValueError, TypeError, struct.error) as e:   # pe_exports: no PE signature, or a table outside the file
        sys.exit(f'{dll} is no DLL that can be read ({e})')
    if missing:
        sys.exit(f'{dll} lacks exports runtime/dlssg/nx2_sl.h declares: {", ".join(missing)} '
                 '(a DLL built from older sources: build it again)')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--sl', type=Path, help='the Streamline SDK folder (holds include/sl.h)')
    ap.add_argument('--vulkan', type=Path, default=Path(r'C:\msys64\ucrt64\include'), help='folder holding vulkan/vulkan.h')
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'dlssg')
    ap.add_argument('--check', type=Path, metavar='DLL', help='only check that DLL exports what nx2_sl.h declares')
    a = ap.parse_args()
    if a.check:
        check(a.check)
        print(f'{a.check}: the {len(declared_exports())} exports nx2_sl.h declares')
        return
    if not a.sl:
        ap.error('--sl is required to build')
    if not (a.sl / 'include' / 'sl.h').is_file():
        sys.exit(f'{a.sl / "include" / "sl.h"} is missing')
    a.out.mkdir(parents=True, exist_ok=True)
    # only the Vulkan headers: MSYS2's own C library headers must not shadow MSVC's
    inc = a.out / 'include'
    for d in ('vulkan', 'vk_video'):
        if (a.vulkan / d).is_dir():
            shutil.copytree(a.vulkan / d, inc / d, dirs_exist_ok=True)
    src = ROOT / 'runtime' / 'dlssg' / 'nx2_sl.cpp'
    cmd = (f'call "{vcvars()}" >nul && cl /nologo /LD /O2 /EHsc /std:c++17 /W3 /DNOMINMAX /DWIN32_LEAN_AND_MEAN '
           f'/I"{a.sl / "include"}" /I"{inc}" /I"{src.parent}" "{src}" /Fo"{a.out}\\\\" '
           f'/Fe"{a.out / "nx2navi_dlssg.dll"}" /link /NOLOGO')
    with tempfile.NamedTemporaryFile('w', suffix='.cmd', delete=False) as f:
        # vcvars64.bat runs vswhere itself and needs it on PATH
        f.write('@echo off\r\nset "PATH=%PATH%;' + str(VSWHERE.parent) + '"\r\n' + cmd + '\r\n')
        script = f.name
    try:
        r = subprocess.run(['cmd', '/c', script])
    finally:
        os.unlink(script)
    if r.returncode or not (a.out / 'nx2navi_dlssg.dll').is_file():
        sys.exit(f'the build failed ({r.returncode})')
    check(a.out / 'nx2navi_dlssg.dll')
    print(a.out / 'nx2navi_dlssg.dll')


if __name__ == '__main__':
    main()
