"""Make the Linux toolchain a kit carries (tools/make_game.py LINUX_TOOLCHAIN), so the game builds on any Linux without
the system's compiler, headers or libraries: SteamOS, whose read-only system has no headers, and distributions whose
clang or Vulkan headers are older than the runtime needs.

    .venv/bin/python tools/make_linux_toolchain.py --out build/linux-toolchain
    .venv/bin/python tools/make_appimage.py ... --linux-toolchain build/linux-toolchain

The folder holds:

    llvm/      LLVM's clang, clang++, lld and llvm-ar, with clang's resource headers (tools/make_release_linux.py
               build_llvm: the same pinned release, debug symbols stripped)
    sysroot/   from the Steam Runtime 3 "sniper" SDK (glibc 2.31): the C library, kernel and C++ headers and libraries,
               the GCC runtime, and SDL3, Opus, libvpx and Vulkan's loader with their headers; nothing else
    deps/      Vulkan headers 1.4.363 and VMA (the runtime needs newer than the sysroot's), zstd and dav1d built static
    libSDL3.so.0   the sysroot's SDL3, which make_game.py places beside the game (rpath $ORIGIN)
    LICENSES/  the licences of what the folder carries

The game it builds needs no newer glibc than 2.31, and from the system only Vulkan's loader (libvulkan.so.1) and the
graphics driver. Downloads are pinned by sha256 and cached in toolchain/release-dl (tools/make_release.py fetch); dav1d
builds with meson and, for its assembly, nasm when it is on PATH. Nothing of the game is read or written.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_release_linux as rl  # noqa: E402

mr = rl.mr
DAV1D = '1.5.4'
rl.mr.PINS.update({
    'dav1d-src': (f'https://downloads.videolan.org/pub/videolan/dav1d/{DAV1D}/dav1d-{DAV1D}.tar.xz',
                  '686616b7c69eb88d44459391ab25cac13b6647a3b288835c5784e71c1514a5c5'),
})

# The sysroot's header folders the runtime and the lifted code include, besides glibc's and the kernel's
# glibc's network headers (netdb.h includes netinet/in.h and rpc/netdb.h): the co-op client in runtime/platform/linux.c
HEADER_DIRS = ['SDL3', 'opus', 'vpx', 'vulkan', 'vk_video', 'c++', 'netinet', 'arpa', 'net', 'rpc']
# Libraries a link reads, by name prefix in usr/lib/x86_64-linux-gnu (and lib/x86_64-linux-gnu): glibc's and those of
# make_game.py's LINK_LIBS that come from the sysroot
LIB_NAMES = [*(f'lib{n}' for n in rl.GLIBC_LIBS), 'libSDL3.so', 'libopus.a', 'libvpx.a', 'libvulkan.so']


def copy_rel(src_root, rel, dst_root):
    """src_root/rel to dst_root/rel, a symlink as a symlink, and the file a symlink names too."""
    src, dst = src_root / rel, dst_root / rel
    dst.parent.mkdir(parents=True, exist_ok=True)
    if src.is_symlink():
        if not dst.is_symlink():
            dst.symlink_to(os.readlink(src))
        target = Path(os.path.normpath(src.parent / os.readlink(src)))
        if str(target).startswith(str(src_root) + '/') and target.exists():
            copy_rel(src_root, target.relative_to(src_root), dst_root)
    elif src.is_dir():
        shutil.copytree(src, dst, symlinks=True, dirs_exist_ok=True)
    elif src.exists() and not dst.exists():
        shutil.copy2(src, dst)


def build_sysroot(full, dst):
    inc = Path('usr/include')
    arch = inc / 'x86_64-linux-gnu'
    for p in sorted((full / inc).glob('*.h')):
        copy_rel(full, p.relative_to(full), dst)
    for d in [arch, inc / 'linux', inc / 'asm-generic', *(inc / h for h in HEADER_DIRS)]:
        if (full / d).exists():
            copy_rel(full, d, dst)
    for lib_dir in (Path('usr/lib/x86_64-linux-gnu'), Path('lib/x86_64-linux-gnu')):
        for p in sorted((full / lib_dir).iterdir()):
            n = p.name
            if any(n.startswith(name) for name in LIB_NAMES) or n.endswith('crt1.o') or n in ('crti.o', 'crtn.o') \
                    or n.startswith(('ld-linux', 'libgcc_s.so')):
                copy_rel(full, p.relative_to(full), dst)
    for p in sorted((full / 'lib64').glob('ld-linux*')):
        copy_rel(full, p.relative_to(full), dst)
    # the GCC installation clang takes its C++ library, crt objects and libgcc from (the sniper SDK's GCC 10)
    gcc = full / 'usr/lib/gcc/x86_64-linux-gnu'
    for ver in sorted(gcc.iterdir()):
        for p in sorted(ver.iterdir()):
            if p.is_file() and (p.name.startswith('crt') or p.name in rl.GCC_LIBS or p.name == 'libatomic.a'):
                copy_rel(full, p.relative_to(full), dst)
    # clang finds the C++ headers from the GCC version folder's name
    for p in sorted((full / inc / 'c++').iterdir()):
        copy_rel(full, p.relative_to(full), dst)


def build_dav1d(llvm, sysroot, d):
    """libdav1d.a against the sysroot with the toolchain's clang (meson cross file); nasm for its x86 assembly."""
    src = rl.tar_extract('dav1d-src', rl.WORK / 'dav1d', [f'dav1d-{DAV1D}'], strip=1)
    build = rl.WORK / 'dav1d-build'
    shutil.rmtree(build, ignore_errors=True)
    cross = rl.WORK / 'dav1d-cross.ini'
    flags = f"['--target=x86_64-linux-gnu', '--sysroot={sysroot}']"
    cross.write_text(f"""[binaries]
c = '{llvm / "bin" / "clang"}'
ar = '{llvm / "bin" / "llvm-ar"}'
strip = '{llvm / "bin" / "llvm-strip"}'
nasm = '{shutil.which("nasm") or "nasm"}'
[built-in options]
c_args = {flags}
c_link_args = {flags[:-1]}, '-fuse-ld=lld']
[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
""")
    asm = shutil.which('nasm') is not None
    if not asm:
        print('nasm is not on PATH: dav1d without its assembly (slower movie decoding)')
    rl.run(['meson', 'setup', build, src, '--cross-file', cross, '--buildtype=release', '--default-library=static',
            '-Denable_tools=false', '-Denable_tests=false', '-Denable_examples=false',
            f'-Denable_asm={"true" if asm else "false"}', '-Dlogging=false'], stdout=subprocess.DEVNULL)
    rl.run(['ninja', '-C', build], stdout=subprocess.DEVNULL)
    shutil.copy2(build / 'src' / 'libdav1d.a', d / 'lib' / 'libdav1d.a')
    shutil.copytree(src / 'include' / 'dav1d', d / 'include' / 'dav1d', dirs_exist_ok=True)
    for h in (build / 'include' / 'dav1d').glob('*.h'):   # version.h, generated
        shutil.copy2(h, d / 'include' / 'dav1d' / h.name)
    return src


def licences(out, full, dav1d_src):
    lic = out / 'LICENSES'
    lic.mkdir(exist_ok=True)
    shutil.copy2(mr.fetch('llvm-license'), lic / 'LLVM.txt')
    for pkg in ('libc6', 'gcc-10-base', 'libsdl3-0', 'libopus0', 'libvpx6', 'libvulkan1'):
        cp = full / 'usr/share/doc' / pkg / 'copyright'
        if cp.is_file():
            shutil.copy2(cp, lic / f'{pkg}.txt')
    shutil.copy2(rl.WORK / 'vulkan-headers' / 'LICENSE.md', lic / 'Vulkan-Headers.md')
    shutil.copy2(rl.WORK / 'zstd' / 'LICENSE', lic / 'zstd.txt')
    shutil.copy2(dav1d_src / 'COPYING', lic / 'dav1d.txt')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--out', type=Path, required=True, help='the folder to write (replaced)')
    a = ap.parse_args()
    out = a.out.resolve()
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    llvm = rl.build_llvm(out)   # out/llvm
    full = rl.full_sysroot()
    sysroot = out / 'sysroot'
    build_sysroot(full, sysroot)
    rl.build_deps(out, llvm, sysroot)   # out/runtime/deps: Vulkan headers, VMA, zstd
    (out / 'runtime' / 'deps').rename(out / 'deps')
    (out / 'runtime').rmdir()
    dav1d_src = build_dav1d(llvm, sysroot, out / 'deps')
    shutil.copy2((full / 'usr/lib/x86_64-linux-gnu/libSDL3.so.0').resolve(), out / 'libSDL3.so.0')
    licences(out, full, dav1d_src)
    size = sum(p.stat().st_size for p in out.rglob('*') if p.is_file() and not p.is_symlink())
    print(f'{out}: {size / 1e6:.0f} MB')


if __name__ == '__main__':
    main()
