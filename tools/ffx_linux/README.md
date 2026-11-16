# FSR 3.1 on Linux: libamd_fidelityfx_vk.so

On Windows the game loads AMD's `amd_fidelityfx_vk.dll` from beside `nx2navi.exe` for FSR 3.1 (`runtime/vk_ffx.cpp`).
AMD ships that library for Windows only, so on Linux FSR 3.1 was off. `build.sh` builds the same library for Linux,
`libamd_fidelityfx_vk.so`, from the FidelityFX SDK v1.1.4 release (MIT): the same five entry points (`ffxCreateContext`,
`ffxDestroyContext`, `ffxConfigure`, `ffxQuery`, `ffxDispatch`) and only them, with the **FSR 3.1 upscaler**, **FSR 3.1
frame generation** (frame interpolation, optical flow and the frame interpolation swapchain `runtime/vk_fg.cpp` presents
through) and their Vulkan backend.

The Linux AppImage carries the library and copies it beside the game after each build.

## What is here

- `build.sh` downloads the AMD FidelityFX SDK v1.1.4 release (MIT), checks its SHA-256, applies the patches and builds.
- `ffx_sc/`, `gen_shaders.sh`: AMD's shader compiler, ported to Linux, and the script that runs it over the FSR 3.1
  upscaler, frame interpolation and optical flow shaders, as AMD's CMake files do for Windows.
- `ffx_linux_compat.h`: the MSVC C library names the SDK uses, force-included.
- `patches/fidelityfx-sdk-v1.1.4-linux.patch` (context size, scratch alignment, descriptor pool, unified memory) and
  `patches/fsr-3.1.5-backport.patch`: fixes to the SDK for Linux and from FSR 3.1.5.
- `patches/nx2navi-ffx-api-linux.patch`:
  - the API entry points get default visibility instead of `__declspec(dllexport)`;
  - an MSVC `ui64` literal becomes `ull`;
  - the backend offers wave32 and 32-bit shaders only: the game enables neither `subgroupSizeControl` nor
    `shaderFloat16`/`shaderInt16`, and wave64 blurs the output on RADV. Frame interpolation and optical flow get the
    same shaders.
- `patches/fidelityfx-sdk-v1.1.4-framegeneration-linux.patch`, which lets the frame generation sources build with GCC
  and clang:
  - MSVC `ui64` literals become `ull`, and `%s` of wide strings in `swprintf` becomes `%ls`;
  - a function pointer cast with `reinterpret_cast`, and two temporaries passed by non-const reference made named;
  - the swapchain's pass-through of the Windows-only full-screen-exclusive structures is left out on Linux.
- `win32/Windows.h`: the Win32 calls of the frame interpolation swapchain (critical sections, events, threads,
  `QueryPerformanceCounter`) on POSIX threads and `CLOCK_MONOTONIC`, so AMD's swapchain source stays as it is. Thread
  priorities are left to the system, since raising them needs privileges on Linux.
- `ffx_provider_linux.cpp`: the provider registry, with the FSR 3.1 upscaler, frame generation and its Vulkan
  swapchain, without AMD's D3D12 driver lookup; `exports.map`: the five entry points, nothing else.

THIRD_PARTY_NOTICES.md ("Distributed in the Linux build kit AppImage") names the code that is not ours and its licences.

libstdc++ is linked statically and not exported, so the library needs only glibc and `libvulkan.so.1`.
