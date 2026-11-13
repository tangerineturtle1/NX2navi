# FSR 4 on Linux: amd_fidelityfx_upscaler_vk.so

On Windows the game loads AMD's signed `amd_fidelityfx_upscaler_vk.dll` from beside `nx2navi.exe` for FSR 4
(`runtime/vk_ffx4.cpp`). AMD ships that library for Windows only and no source for it, unlike FSR 3.1
(`tools/ffx_linux/`), so on Linux FSR 4 was off.

`fetch.sh` builds [fsr4-vk](https://github.com/Astyyyyy/fsr4-vk), an independent Vulkan implementation of FSR 4.1.1
not affiliated with or endorsed by AMD, into the same library name and the same five ffx-api entry points. It is
GPL-3.0-only and, per `AGENTS.md`, never copied into this repository: `fetch.sh` only downloads and builds fsr4-vk's
own source at the output folder you give it.

## What `fetch.sh` does

- Downloads the AMD FidelityFX SDK v2.3.0 release zip (MIT), checks its SHA-256, and pulls the one signed DLL fsr4-vk
  needs out of it: `amd_fidelityfx_upscaler_dx12.dll`. fsr4-vk checks that DLL's own version and hash itself.
- Clones fsr4-vk at a pinned commit (it has no releases) and its submodules (its shader tools).
- Runs fsr4-vk's `tools/setup.py` to extract the FSR 4.1.1 model weights and translate its shaders to SPIR-V, then
  `tools/build/linux.sh` to build `amd_fidelityfx_upscaler_vk.so` with them embedded.
- Copies the library and a licence text (fsr4-vk's own licence and third-party notices) to the output folder.

```sh
tools/ffx4_linux/fetch.sh build/ffx4_linux
```

The next `make_game.py` run finds `build/ffx4_linux/amd_fidelityfx_upscaler_vk.so` and copies it beside the game,
with its licence text. It is built once; rerun `fetch.sh` only to pick up a newer fsr4-vk commit. Without it FSR 4
is not offered in the menu, silently (`runtime/vk_ffx4.cpp`'s `load()`); FSR 3.1 is what the menu offers instead.

See `THIRD_PARTY_NOTICES.md` for licensing details.
