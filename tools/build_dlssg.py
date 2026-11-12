"""Build nx2navi_dlssg.dll (runtime/dlssg/nx2_sl.cpp, DLSS Frame Generation through NVIDIA Streamline) with MSVC.

    python tools/build_dlssg.py --sl <Streamline SDK folder with include/> [--out build/dlssg]

Streamline's C++ types have MSVC's ABI, so this one DLL is built with Visual Studio's compiler (the Build Tools are
enough; vswhere finds them), against the SDK's headers, which stay where they are. The Vulkan headers are MSYS2's.
A launcher release and the build kit ship the DLL ready-made beside Streamline's own runtime DLLs
(tools/make_launcher.py and tools/make_kit.py --dlssg, with the Streamline files tools/streamline_pin.py pins), and
tools/make_game.py place_dlssg puts them beside the game.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
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


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--sl', type=Path, required=True, help='the Streamline SDK folder (holds include/sl.h)')
    ap.add_argument('--vulkan', type=Path, default=Path(r'C:\msys64\ucrt64\include'), help='folder holding vulkan/vulkan.h')
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'dlssg')
    a = ap.parse_args()
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
    print(a.out / 'nx2navi_dlssg.dll')


if __name__ == '__main__':
    main()
