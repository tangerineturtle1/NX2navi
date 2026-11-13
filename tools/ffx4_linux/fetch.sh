#!/bin/bash
# Builds amd_fidelityfx_upscaler_vk.so, the library runtime/vk_ffx4.cpp loads from beside the game for FSR 4 on
# Linux. AMD ships no Linux build and no source for FSR 4's upscaler (unlike FSR 3.1's library, tools/ffx_linux/):
# this runs fsr4-vk's own setup and build, pinned by commit, against the AMD FidelityFX SDK v2.3.0 release zip
# (MIT, pinned by SHA-256) for the signed amd_fidelityfx_upscaler_dx12.dll it extracts the FSR 4.1.1 model weights
# and shaders from; fsr4-vk checks that DLL's own version and hash itself. See THIRD_PARTY_NOTICES.md: fsr4-vk is
# GPL-3.0-only and not part of this repository; this script only downloads and builds it at its output folder.
#
#   tools/ffx4_linux/fetch.sh <output folder> [download cache]
#
# Needs git, curl, unzip, python 3.10+ (the zstandard package, unless running 3.14+: python3 -m pip install
# zstandard), cmake, llvm-objdump and a C++20 compiler (Arch: cmake python-zstandard llvm; Ubuntu: cmake python3-pip
# llvm, then python3 -m pip install --user zstandard).
set -euo pipefail
commit=b0125ea9798633750f83ebabbc00279bcb49d8dd
sdk_version=2.3.0
sdk_zip=FidelityFX-Samples-v$sdk_version-prebuilt.zip
sdk_sha256=f90890b9323bb2f4f2404ac4cdc9395e8495ecdac6f7aa0bcdf1ad1848422273
dll_in_zip=Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release/amd_fidelityfx_upscaler_dx12.dll
out=$(mkdir -p "$1" && cd "$1" && pwd)
cache=${2:-}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
[ -n "$cache" ] || cache=$work
mkdir -p "$cache"

zip="$cache/$sdk_zip"
if ! { [ -f "$zip" ] && echo "$sdk_sha256  $zip" | sha256sum -c --status; }; then
    curl -fsSL -o "$zip.part" "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v$sdk_version/$sdk_zip"
    echo "$sdk_sha256  $zip.part" | sha256sum -c --status || { rm -f "$zip.part"; echo "FidelityFX SDK zip: wrong SHA-256" >&2; exit 1; }
    mv "$zip.part" "$zip"
fi
unzip -q -o "$zip" "$dll_in_zip" -d "$work/sdk"

git clone -q https://github.com/Astyyyyy/fsr4-vk.git "$work/fsr4-vk"
git -C "$work/fsr4-vk" checkout -q "$commit"
git -C "$work/fsr4-vk" submodule update --init --recursive

python3 "$work/fsr4-vk/tools/setup.py" --dll "$work/sdk/$dll_in_zip"
(cd "$work/fsr4-vk" && bash tools/build/linux.sh)

cp "$work/fsr4-vk/build/linux/amd_fidelityfx_upscaler_vk.so" "$out/"
{ echo "fsr4-vk @ $commit (https://github.com/Astyyyyy/fsr4-vk), not affiliated with or endorsed by AMD:"; echo
  cat "$work/fsr4-vk/LICENSE"; echo
  echo "Third-party notices (AMD's model weights and third-party code keep their own licence):"
  cat "$work/fsr4-vk/THIRD_PARTY_NOTICES.md"; } > "$out/amd_fidelityfx_upscaler_vk-LICENSE.txt"
echo "built $out/amd_fidelityfx_upscaler_vk.so (fsr4-vk @ $commit, FSR 4.1.1 upscaler, GPL-3.0-only)"
