#!/usr/bin/env python3
"""Build the optional helper against an installed SPIRV-Tools."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--toolchain', type=Path, help='MSYS2 UCRT64 prefix (Windows only)')
    parser.add_argument('--spirv-tools', type=Path, required=True, help='Prefix containing include/ and lib/')
    parser.add_argument('--out', type=Path, required=True, help='Folder for the helper and its license')
    args = parser.parse_args()
    source = Path(__file__).with_name('bridge.cpp')
    env = os.environ.copy()
    if os.name == 'nt':
        if not args.toolchain:
            parser.error('--toolchain is required on Windows')
        compiler = args.toolchain / 'bin' / 'clang++.exe'
        libs = [args.spirv_tools / 'lib' / f'lib{name}.a' for name in ('SPIRV-Tools-opt', 'SPIRV-Tools')]
        for lib in libs:
            if not lib.is_file():
                parser.error(f'Missing library: {lib}')
        env['PATH'] = str(compiler.parent) + os.pathsep + env.get('PATH', '')
        flags = [*(str(lib) for lib in libs), '-static-libgcc', '-static-libstdc++']
        output = 'nx2navi_spirv_opt.dll'
    else:
        compiler = os.environ.get('CXX', 'c++')
        flags = ['-fPIC', '-L' + str(args.spirv_tools / 'lib'), '-lSPIRV-Tools-opt', '-lSPIRV-Tools']
        output = 'libnx2navi_spirv_opt.so'
    args.out.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(compiler), '-O2', '-std=c++17', '-shared', str(source),
                    '-I' + str(args.spirv_tools / 'include'), *flags, '-o', str(args.out / output)],
                   env=env, check=True)
    shutil.copy2(source.parents[2] / 'runtime/licenses/spirv-tools-Apache-2.0.txt',
                 args.out / 'spirv-tools-LICENSE.txt')


if __name__ == '__main__':
    main()
