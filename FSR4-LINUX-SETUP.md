# FSR 4 on Linux: setup

This branch adds AMD FSR 4 upscaling to the Linux build of NX2navi, through an independent Vulkan implementation
([fsr4-vk](https://github.com/Astyyyyy/fsr4-vk)) since AMD ships no Linux build or source for its own upscaler. FSR 4
needs FSR 3.1's Linux library too (`runtime/vk_ffx.cpp` tries FSR 4 only after FSR 3.1's library loads), so build
both.

## 1. Build FSR 3.1's library

```sh
tools/ffx_linux/build.sh build/ffx_linux
```

Needs curl, unzip, patch, cmake, ninja, a C++17 compiler, glslangValidator and the Vulkan headers and loader
(Arch: `cmake ninja glslang vulkan-headers vulkan-icd-loader`; Ubuntu: `cmake ninja-build glslang-tools
libvulkan-dev`). Downloads AMD's FidelityFX SDK v1.1.4 (SHA-256 pinned) and builds `libamd_fidelityfx_vk.so`. See
`tools/ffx_linux/README.md`.

## 2. Build FSR 4's library

```sh
tools/ffx4_linux/fetch.sh build/ffx4_linux
```

Needs git, curl, unzip, Python 3.10+ (`python3 -m pip install zstandard` unless on Python 3.14+), cmake,
llvm-objdump and a C++20 compiler (Arch: `cmake python-zstandard llvm`; Ubuntu: `cmake python3-pip llvm`, then
`python3 -m pip install --user zstandard`). Downloads AMD's FidelityFX SDK v2.3.0 (SHA-256 pinned), clones
[fsr4-vk](https://github.com/Astyyyyy/fsr4-vk) at a pinned commit and builds `amd_fidelityfx_upscaler_vk.so` from it.
Takes a few minutes, mostly weight extraction and shader translation. See `tools/ffx4_linux/README.md`.

fsr4-vk is GPL-3.0-only and not part of this repository: the script only downloads and builds it at the output
folder you give it.

## 3. Build and run the game

```sh
python3 -I tools/make_game.py --nup "/path/to/your game.nup" --out ~/Games/Ocarina
~/Games/Ocarina/ocarina
```

`make_game.py` finds both libraries in their `build/` folders and copies them beside the game automatically. Set
`fsr4 = 1` in `nx2navi.ini`, or toggle it in the in-game menu.

## Notes

- Each build script only needs to run once. Rerun `tools/ffx_linux/build.sh` only if `tools/ffx_linux/` changes, or
  `tools/ffx4_linux/fetch.sh` only to pick up a newer fsr4-vk commit.
- Without step 1, FSR 4 does nothing even if step 2 succeeded (`runtime/vk_ffx4.cpp` needs FSR 3.1's library loaded
  first).
- Without step 2, `fsr4 = 1` does nothing on Linux, silently.
- Licensing details: `THIRD_PARTY_NOTICES.md`.
