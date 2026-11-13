"""Check the runtime's surface-format enumeration with mocked Vulkan queries, without game files or a GPU.

Usage: python tests/surface_formats_check.py [--sanitize]
CC selects a C compiler; otherwise use clang, or MSYS2 UCRT64 clang on Windows.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    work = ROOT / 'build' / 'test' / 'surface_formats'
    work.mkdir(parents=True, exist_ok=True)
    # Compile the actual helper, not a duplicate of its implementation. The rest of the presenter needs a window,
    # a Vulkan device and generated game headers, none of which this API-enumeration regression needs.
    source = (ROOT / 'runtime' / 'gfx_present.c').read_text()
    start = source.index('static VkSurfaceFormatKHR *surface_formats(')
    end = source.index('\n}\n', start) + 3
    (work / 'surface_formats.inc').write_text(source[start:end])
    cc = os.environ.get('CC') or (str(MSYS_BIN / 'clang.exe') if os.name == 'nt' and
                                 (MSYS_BIN / 'clang.exe').is_file() else shutil.which('clang') or 'cc')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    exe = work / ('surface_formats_check.exe' if os.name == 'nt' else 'surface_formats_check')
    flags = ['-fsanitize=address,undefined'] if args.sanitize else []
    subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', *flags, '-I', str(work),
                    str(ROOT / 'tests' / 'surface_formats_check.c'), '-o', str(exe)], env=env, check=True)
    subprocess.run([str(exe)], env=env, check=True)


if __name__ == '__main__':
    main()
