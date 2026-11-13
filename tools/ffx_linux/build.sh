#!/bin/bash
# Builds libamd_fidelityfx_vk.so, the Linux counterpart of AMD's amd_fidelityfx_vk.dll that runtime/vk_ffx.cpp loads from
# beside the game for FSR 3.1 upscaling. AMD ships that library for Windows only; this builds it from the FidelityFX SDK
# v1.1.4 release zip (MIT, pinned by SHA-256) with the patches in patches/: the FSR 3.1 upscaler, FSR 3.1 frame generation
# and its frame interpolation swapchain, and their Vulkan backend. See README.md.
#
#   tools/ffx_linux/build.sh <output folder> [download cache]
#
# Needs curl, unzip, patch, cmake, ninja, a C++17 compiler, glslangValidator and the Vulkan headers and loader
# (Arch: cmake ninja glslang vulkan-headers vulkan-icd-loader; Ubuntu: cmake ninja-build glslang-tools libvulkan-dev).
# NX2NAVI_LINUX_TOOLCHAIN=<folder of tools/make_linux_toolchain.py> builds with that clang and sysroot instead of the
# system's compiler and Vulkan headers.
set -euo pipefail
version=1.1.4
sha256=0216556bfb0e243cec30004a2a98d38f4e3f7406cb7938e3c1b85c758e95d952
here=$(cd "$(dirname "$0")" && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
cache=${2:-}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
[ -n "$cache" ] || cache=$work
mkdir -p "$cache"
zip="$cache/FidelityFX-SDK-v$version.zip"
if ! { [ -f "$zip" ] && echo "$sha256  $zip" | sha256sum -c --status; }; then
    curl -fsSL -o "$zip.part" "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v$version/FidelityFX-SDK-v$version.zip"
    echo "$sha256  $zip.part" | sha256sum -c --status || { rm -f "$zip.part"; echo "FidelityFX SDK zip: wrong SHA-256" >&2; exit 1; }
    mv "$zip.part" "$zip"
fi
mkdir "$work/sdk"
unzip -q "$zip" common.cmake docs/license.md 'ffx-api/*' 'sdk/*' -x 'ffx-api/bin/*' -d "$work/sdk"
# the zip's text files have CRLF line ends; the patches are LF
find "$work/sdk" -type f \( -name '*.h' -o -name '*.cpp' -o -name '*.c' -o -name '*.glsl' -o -name '*.hpp' -o -name '*.txt' \) \
    -exec sed -i 's/\r$//' {} +
for p in fidelityfx-sdk-v1.1.4-linux.patch fsr-3.1.5-backport.patch nx2navi-ffx-api-linux.patch \
         fidelityfx-sdk-v1.1.4-framegeneration-linux.patch; do
    patch -s -p1 -d "$work/sdk" < "$here/patches/$p"
done
tc=()
if [ -n "${NX2NAVI_LINUX_TOOLCHAIN:-}" ]; then
    # tools/make_linux_toolchain.py's clang and Steam Runtime sysroot: the library then needs no newer glibc than the
    # game it loads into (2.31)
    t=$(cd "$NX2NAVI_LINUX_TOOLCHAIN" && pwd)
    # the shader compiler runs here only: built with the system's compiler, whose C++ library it needs (GCC 11+)
    cmake -S "$here" -B "$work/host" -G Ninja -DCMAKE_BUILD_TYPE=Release -DFFX_ROOT="$work/sdk" >/dev/null
    cmake --build "$work/host" --target FidelityFX_SC
    tc=(-DCMAKE_C_COMPILER="$t/llvm/bin/clang" -DCMAKE_CXX_COMPILER="$t/llvm/bin/clang++" -DCMAKE_SYSROOT="$t/sysroot"
        -DCMAKE_C_COMPILER_TARGET=x86_64-linux-gnu -DCMAKE_CXX_COMPILER_TARGET=x86_64-linux-gnu
        -DCMAKE_AR="$t/llvm/bin/llvm-ar" -DCMAKE_RANLIB="$t/llvm/bin/llvm-ranlib"
        "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld" "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld"
        -DFFX_SC_EXECUTABLE="$work/host/FidelityFX_SC")
fi
cmake -S "$here" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DFFX_ROOT="$work/sdk" "${tc[@]+"${tc[@]}"}" >/dev/null
cmake --build "$work/build"
cp "$work/build/libamd_fidelityfx_vk.so" "$out/"
{ cat "$work/sdk/docs/license.md"; echo; echo 'Built with code from xplane-fsr3 (https://github.com/JonathanOrr/xplane-fsr3):'; echo
  cat "$here/LICENSE.xplane-fsr3"; } > "$out/libamd_fidelityfx_vk-LICENSE.txt"
echo "built $out/libamd_fidelityfx_vk.so (AMD FidelityFX SDK $version, FSR 3.1 upscaler and frame generation)"
