# Building from source

Windows 10 or later, an x86-64 CPU with SSE4.2, and a GPU with Vulkan 1.3 and `VK_EXT_external_memory_host`. The
default build targets x86-64-v3 (AVX2/FMA); a CPU without them builds with `--cpu v2` instead and runs slower (see
"Build"). Allow about 60 GB for the work folder. Extraction wants about 55 GB free while it runs and keeps about 35 GB
afterwards.

Players who only want to play use the [launcher](launcher.md) instead: it installs its own build tools, builds the
game on their machine and keeps it up to date.

## Toolchain

Versions are the ones the build was verified with; newer patch releases are fine.

| Tool | Version | Install | Used for |
|---|---|---|---|
| MSYS2 UCRT64 | clang/lld 23.1.3 | msys2.org installer to `C:\msys64`, packages below | compiling the generated C and the runtime |
| Visual Studio Build Tools | 2026, C++ workload, Windows SDK 10.0.26100 | winget `Microsoft.VisualStudio.BuildTools` with `--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended` | the linker .NET NativeAOT calls |
| .NET SDK | 10.0 | winget `Microsoft.DotNet.SDK.10` | the shader translator (`nx2navi_xlate.exe`) |
| Vulkan SDK | 1.4.363 | winget `KhronosGroup.VulkanSDK` | validation layers, `vulkaninfo` |
| uv | 0.13 | standalone installer from astral.sh | the two Python environments |
| hactool | 1.4.0 @ `1d64a83` | `.venv\Scripts\python tools\build_hactool.py`, see below | NCA and update extraction |

MSYS2 packages, after `pacman -Syuu` (run it twice; the first pass updates the core and exits):

```sh
pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-{clang,lld,ninja,gcc,make,pkgconf,sdl3,opus,libvpx,dav1d,zstd,vulkan-devel,\
vulkan-memory-allocator,vulkan-validation-layers,shaderc,spirv-tools} base-devel git unzip
```

`ffmpeg` from MSYS2 is a GPLv3 build and is never linked; the movies go through libvpx.

PATH:

- Add `C:\Program Files (x86)\Microsoft Visual Studio\Installer` to the user PATH: NativeAOT needs `vswhere.exe`, or
  the publish fails at the link step (MSB3073).
- Another `cmake` or `ninja` earlier on PATH (a 32-bit winlibs MinGW, for example) is picked up by accident. The build
  scripts call Ninja from `C:\msys64\ucrt64\bin` themselves; from a shell, put `C:\msys64\ucrt64\bin` first.

### hactool

`tools/build_hactool.py` fetches hactool at `1d64a83` into `toolchain\src\hactool`, applies the three patches in
`tools/hactool/` and builds a static `toolchain\hactool\hactool.exe` with MSYS2 UCRT64 gcc (a few seconds):

- `portable.patch`: the changes a portable Windows build needs, the `MNG_TITLE_KEYS` variable and
  the two BKTR fixes in `nca.c` for the subsection table padding and the subsection layout of newer cumulative updates;
- `unicode.patch`: a manifest with `<activeCodePage>UTF-8</activeCodePage>`, so paths under a non-ASCII user profile
  reach it intact;
- `mbedtls-asm-clobbers.patch`: the `memory` and `cc` clobbers the bundled mbedtls 2.6.1's x86-64 multiply asm lacks.
  Without them a clang build (the Linux release) fails every RSA signature check and the update merge stops.

hactool reads `-k <prod.keys>`; MSYS2 shells set `HOME`, which hides its default key location, so pass `-k` there.

## Build

From the repository root, in PowerShell. Every step that depends on the game version takes it as `<ver>`: `v143` for
1.4.3, `v121` for 1.2.1 or `v110` for 1.1.0. The tools default to `v121` when `--version` is left out. Saves load on any
of them.

1. Python environments (once). `.venv` runs the lifter and the build; `.venv-py314` runs the extraction scripts, which
   need the stdlib `compression.zstd` of Python 3.14:

   ```powershell
   uv venv --python 3.12 .venv
   uv pip install --python .venv/Scripts/python.exe -r tools/requirements.txt
   uv venv --python 3.14 .venv-py314
   uv pip install --python .venv-py314/Scripts/python.exe -r tools/requirements-py314.txt
   ```

2. Copy `config.example.toml` to `config.local.toml` and set the paths to your base game, the update and your console's
   keys file. `config.local.toml` is ignored by git. The tools read the keys file themselves.

3. Extract and verify. This writes `build\extracted\v0` (1.0.0) and `build\extracted\<ver>` for each update in
   `updates`, identified by its hash; a hash mismatch stops the run. With another `[work] dir` in `config.local.toml`,
   `extracted` goes there instead, and `build_game.py` reads the ExeFS and RomFS from there:

   ```powershell
   .venv-py314\Scripts\python tools\extract.py
   ```

4. Shader translator (no game files; again after a change to its sources, and `tools\make_release.py` stops on an older
   one). This publishes `build\xlate\nx2navi_xlate.exe`:

   ```powershell
   .venv\Scripts\python tools\build_xlate.py
   ```

5. Lift the modules to C, write the RomFS overlay (the frame-rate fix), then compile and link
   `build\<ver>\nx2navi.exe`:

   ```powershell
   .venv\Scripts\python recomp\gen.py --exefs build\extracted\<ver>\exefs --out generated\<ver> --patches recomp\patches-<ver>.txt
   .venv-py314\Scripts\python tools\make_romfs_patches.py --version <ver>
   .venv\Scripts\python tools\build_game.py --version <ver>
   ```

   The menu's font comes into `runtime/ui/menu.cpp` with `#embed`; for a compiler without it (before clang 19, which
   a Linux build with the distribution's compiler can meet) the same step writes the font's bytes as
   `runtime/ui/roboto_ttf.h` (git ignores it), which `menu.cpp` then includes.
   The runtime's own shaders (upscaling, HDR, texture decode and the like) are in the repository as source only:
   GLSL and SPIR-V assembly in `runtime\shaders\`, and two small ones assembled by `tools\make_dlaa_mask.py` and
   `tools\make_mvobj_fs.py`. `build_game.py` compiles them into `runtime\<name>_spv.h` (git ignores these) before it
   compiles the runtime, with `glslc`, `spirv-as` and `spirv-val` from the `shaderc` and `spirv-tools` packages above,
   and again whenever a source changed. `python tools\build_shaders.py` does that step alone; `--check` only reports
   the headers that are missing or stale.

   `--cpu v2` targets x86-64-v2 (SSE4.2, no AVX2/FMA) for older CPUs; the default `v3` needs AVX2. The v2 build links
   `runtime/fma.c`, a correctly rounded software fma the FP lowering needs, and runs slower. Inline assembly,
   target attributes and intrinsics are not bound by `-march`, so a v2 build runs `tools/check_isa.py` over the runtime
   objects before it links: any AVX, BMI or other instruction above x86-64-v2 outside a function listed there as
   dispatched by a runtime CPU check stops the build.

   `gen.py` sizes its emit pool from the CPU count and the free memory; `--jobs N` sets it, and so does the environment variable `NX2NAVI_LIFT_JOBS=N`, which the installer's lift step honours too (it has no `--jobs`). When a worker runs out of memory, the pool restarts with half the workers (`emit: out of memory with N workers, retrying with M`), down to one; only an out-of-memory error with one worker fails the step.

   Lifting takes about 15 s, a clean compile about 200 s on 24 threads; later builds recompile only what changed.
   A compiler cache in front of clang shortens it further: with `SCCACHE` set to an sccache executable, `build_game.py`
   writes it into the ninja file ahead of `cc`, and every lift rewrites the generated chunks with the same contents, so
   a second build of the same version hits all of their objects:

   ```powershell
   $env:SCCACHE = "path\to\sccache.exe"
   .venv\Scripts\python tools\build_game.py --version <ver>
   ```
   `--patches` builds in the frame-rate, resolution and other patch sets, switchable at run time. The link copies
   `SDL3.dll`, `libiconv-2.dll` and `libopus-0.dll` next to the exe.
   For Xbox button prompts, run `make_romfs_patches.py` twice more, with `--prompts xbox` and `--prompts xbox-label`:
   they write `build\<ver>\romfs-patches-xbox` and `romfs-patches-xbox-label` ([controls.md](controls.md), "Xbox
   button prompts"), which `build_game.py` copies next to `nx2navi.exe` like `romfs-patches`. `--prompts pc` writes
   `romfs-patches-pc`, the keyboard and mouse prompts ([controls.md](controls.md), "PC button prompts").

6. Prepare the shaders (once per build folder and translator). It needs `refs\MacNativeGames` (`tools\fetch_refs.sh`)
   for the shared shader list:

   ```powershell
   .venv-py314\Scripts\python tools\prepare_shaders.py build\<ver> --version <ver>
   ```

   Without this step the first launch compiles shaders and pipelines during play and stutters until the caches fill.
   The AMD driver keeps its pipeline cache per exe path and holds a limited amount: preparing another build folder can
   evict this one's. `nx2navi.exe --warm-shaders` rebuilds it from the shader cache. A driver update empties the
   driver's cache too, and a change to what a stage library is built with gives every library another entry in it.
   So an update that carries the prepared shaders over runs `--warm-shaders` (`tools/make_game.py`
   `warm_driver_cache`: seconds while the libraries are there, minutes when they are not), and the game itself checks
   at every start: it builds a sample of the prepared fragment libraries and, when their median time says the cache
   is cold, builds the whole prepared list into the driver's cache again on idle-priority threads while it runs
   (`runtime/vk_pipeline.c` `lw_start`, `NX2NAVI_LIB_WARM`; `shader-warm.txt` next to the caches says where that
   stands). With the cache cold every pipeline a draw first needs compiles in play: on an RX 7900 XT a library took
   93 ms (vertex) and 242 ms (fragment) on average against 0.3 and 0.6 ms from the cache, and a first walk through a
   village skipped 11,180 draws and made 94 draws wait 5.5 s in all, against 1,605 skipped (1,473 of them in the
   scene's first frames) and 60 ms of waits with the cache warm.
   A frame that still lacks draws when it is presented is not shown: the previous picture stays on screen
   (`runtime/vk_backend.c` `frame_hold`, `NX2NAVI_FRAME_HOLD_MS`, an allowance of 120 ms that fills again at an eighth
   of the clock's rate). On a first walk through a village with nothing recorded, 21 of the 23 such frames were held
   back; in a player's session a hit by a boulder had shown one black frame, its lighting pass skipped after 50 ms.
   When the driver's shader compiler crashes on a variant (NVIDIA 615.78.08 on an RTX 5080 does, issue #30),
   `prepare_shaders.py` runs the driver step again with `--warm-shaders`, up to three times, without the variants the
   driver was building at the crash. `shader-prepare-skip.txt` next to the exe lists them with the GPU and driver
   version; they build at their first draw instead, another driver version builds them again, and so does deleting the
   file.

   What this step leaves for play, and the shared pipeline list. The step translates every shader's default variant
   and the shader list's variants and builds each one's stage library or compute pipeline. A whole graphics pipeline
   is built ahead at the start of a game folder that has drawn it before (`pipeline-keys.bin`) or whose release
   listed it (below); a pipeline met for the first time is linked from its stage libraries in 2 to 8 ms, of which its
   first draw waits 3 (`pipe_link_draw_ms`, from the pipeline's first miss; `runtime/vk_exec_draw.c` `link_budget`),
   except a mesh
   pipeline, which has no libraries and compiles whole (0.2 to 1.1 s, its draws left out of the picture meanwhile),
   and many of them on a first visit add up to hitches. `tools/pipeline_list.py merge` folds the `pipeline-keys.bin`
   and `shader-answers.bin` of game folders that were played, and lists made before, into a pipeline list, with the
   compute variants those folders dispatched (no pipeline record names a compute shader, and a variant the shader list
   lacks is translated on the frame that first dispatches it); `prepare_shaders.py` reads it from
   `tools\data\pipeline_list.bin` (`data\pipeline_list.bin` in a release folder, or `--pipeline-list`), adds the
   shader variants it names to the seed and writes its records next to the game as `seed-pipeline-keys.bin`, and
   `--prepare-shaders` builds every one of them into the pipeline cache and the driver's (`runtime/vk_pipeline.c`
   `seed_pipelines`; `NX2NAVI_PREPARE_PIPELINES=mesh` builds only the mesh pipelines). Every start then counts the
   list as known pipelines beside the folder's own and builds them before play, from those caches (`keys_open_seed`;
   `pipe_seed = 0` in `nx2navi.ini` leaves the list out). `--warm-shaders` builds them again, and
   `nx2navi.exe --prepare-pipelines` does that step alone. The list is
   derived from game data: it is not in the repository (`tools/check_content.py` refuses it), and a kit, a launcher
   release or an installer release takes one with `--pipeline-list` (`tools/make_kit.py`, `make_launcher.py`,
   `make_release.py`). A list belongs to the record layout of the runtime that wrote it; another runtime says so and
   builds nothing from it. `python tests\pipeline_list_test.py` checks the tool on made-up folders.

   What a game folder's shader cache holds can be checked without the game:
   `python tools\validate_shader_cache.py <game folder>\shader-cache` reads every translated shader from the pack and
   the loose files, applies the checks the game makes when it loads one (those that fail are translated again at
   their next use) and runs `spirv-val` on the rest; `--every N` samples, `--out FILE` lists the failed ones. It writes
   nothing to the cache. `python tests\validate_shader_cache_test.py` checks the tool on a made-up cache.

7. Run. The game keeps running when its window loses the focus (`assume_focus = 1` in `nx2navi.ini`, the default; the
   Game tab switches it). With 0 it pauses, as it does on the Switch when Home is pressed.

   ```powershell
   build\<ver>\nx2navi.exe
   ```

Settings live in `nx2navi.ini` next to `nx2navi.exe` ([nx2navi.ini.example](../nx2navi.ini.example) lists them). Saves go to
`build\profile` for `v121` and `build\profile-<ver>` for other versions (`build_game.py --profile` changes it).

### Portable folder

`.venv\Scripts\python tools\package_build.py --version <ver>` writes `build\<ver>-portable`, which runs on another of
your machines without MSYS2, Python or .NET. `romfs.bin` stays out unless you pass `--with-romfs`; otherwise put it in
`romfs\` next to the exe or point `NX2NAVI_ROMFS` at its folder. Run `nx2navi.exe --prepare-shaders` once in the copied
folder. The folder holds code and data derived from your game: for your own machines only.

### Profile-guided build

`build_game.py --pgo <file>.profdata` compiles the generated code at -O2 with an execution profile, and the runtime
with it too: hot paths laid out straight, hot calls inlined, code that never ran compiled for size. On 1.4.3 the fight
bench ran 12-15% faster per frame and a fight outside the training about 6%. The installer builds with
`data/pgo-<ver>.profdata` of the release when the update has one, else at -O1.

A profile only fits the generated C it was trained on: functions are matched by name and control-flow hash, so a
function whose C changes loses its counts without a warning. Retrain after a lifter or generator change, from a clean
`recomp/`. The instrumented build needs llvm-mingw, as MSYS2's clang has no profile runtime:

```powershell
$llvm = "toolchain\llvm-mingw-<date>\llvm-mingw-<date>-ucrt-x86_64"
.venv\Scripts\python tools\build_game.py --version v143 --out build\v143-pgogen --toolchain $llvm --pgo gen
```

Play `build\v143-pgogen\nx2navi.exe` through varied scenes (fights, towns, riding, building, underground, menus) for 20-30
minutes; it is slower than a normal build. It writes `build\v143-pgogen\pgo\*.profraw` every minute and at exit.
Scripted runs from saves (`tools/run_game.py`) add to the same file. Then:

```powershell
.venv\Scripts\python tools\pgo.py merge build\v143-pgogen --version v143
```

writes `build\pgo\pgo-v143.profdata` and its record (`.json`: the `recomp/` tree it fits). `make_release.py` ships every
profile of `build\pgo` in `data\` and warns when a record names another `recomp/` tree than the one released.

## Linux (in progress)

The runtime builds and runs on x86-64 Linux with a window, input and menu on SDL3 (`*_sdl.c`) in place of the Win32
files (`*_win.c`); `runtime/platform/linux.c` implements `platform.h`. Status: the selftest and the probes pass; 1.2.1
plays at 60 fps on RADV (RX 7800 XT) and NVIDIA (RTX 3060); 1.4.3 builds, probes and plays on RADV (RX 6600, and the
Vega 7 of a Ryzen 5 4600G at about 34 fps). Long play sessions and the Steam Deck itself are not verified yet.

Requirements:

- Linux 6.7 or later: the write watch uses userfaultfd write protection with `PAGEMAP_SCAN`. SteamOS 3.8 qualifies.
- A Vulkan 1.3 driver with `VK_EXT_external_memory_host` (RADV, NVIDIA).
- Packages on Arch:

  ```sh
  pacman -S --needed clang lld ninja make sdl3 opus libvpx dav1d zstd vulkan-headers vulkan-icd-loader \
      vulkan-validation-layers shaderc spirv-tools dotnet-sdk uv git
  ```

  Vulkan Memory Allocator is a single header (AUR `vulkan-memory-allocator`). Without it, `build_game.py` fetches the
  version the Windows release pins (3.4.0) into `toolchain/vma/` on first use and checks its hash.
  The Vulkan headers must be release 363 (1.4.363, the one MSYS2 supplies on Windows) or newer: the runtime names
  extensions older headers lack. Where the system's are older, `build_game.py` and `make_game.py` fetch that release
  into `toolchain/vulkan-headers/` on first use, check its hash and put it ahead of the system's (`tools/vulkan_pin.py`,
  `build_game.py` `vulkan_include`).

The steps are the Windows ones with POSIX paths: `.venv/bin/python` in place of `.venv\Scripts\python`, and `nx2navi` in
place of `nx2navi.exe`. `build_game.py` takes clang, lld and Ninja from PATH (or the ones in `<dir>/bin` with
`--toolchain <dir>`) and links against the system libraries, with `libstdc++` static and an `$ORIGIN` rpath;
`build_hactool.py` builds `hactool` with make. `tools/prepare_shaders.py` reads the shader list from
`refs/MacNativeGames` at `origin/HEAD`, which must be the pinned commit with its history (the list format changed later).

The probes open a window, so from an SSH session they need the desktop session's display, or they stop at `gpu 0
skipped: no graphics+compute+present queue family`: on Wayland `XDG_RUNTIME_DIR=/run/user/$(id -u)
WAYLAND_DISPLAY=wayland-1 SDL_VIDEODRIVER=wayland` (the socket name is in `$XDG_RUNTIME_DIR`), on X11 `DISPLAY=:0`.

NVIDIA's Linux driver compiles into a disk cache of its own. A process adds at most `__GL_SHADER_DISK_CACHE_SIZE` to it
(1 GiB by default; at a start the files after the first are deleted while the folder is over that size), and the default
folder, `~/.cache/nvidia`, is shared with every program. `--prepare-shaders` of game 1.1.1 builds 42,142 variants, about
1.5 GB: on the default limit about 30,700 of them were stored and the rest compiled again at their first draw in play.
`runtime/vk_device.c` therefore points `__GL_SHADER_DISK_CACHE_PATH` at `nvidia-cache` beside `ocarina` and the size at
8 GiB, each unless the environment sets it, as it does `MESA_SHADER_CACHE_DIR` for RADV, and only while NVIDIA's kernel
driver is loaded (`/proc/driver/nvidia/version`). The driver does not make that folder and caches nothing in a folder it
cannot write, so the game makes it, and when it cannot make or write it sets neither variable. A game run sets them in
`main` before any of its threads starts, since a thread can load the Vulkan driver before the instance is made. The
folder can be deleted; `ocarina --warm-shaders` fills it again.

Memory: on Windows the GPU imports guest memory and `GetWriteWatch` tracks the CPU's writes to it. On Linux,
re-protecting imported memory invalidates the driver's userptr buffer on every submit (about 1.3 ms per 16 MiB on
amdgpu), and importing pins the pages, which drops their write protection. Each CPU-visible pool therefore gets a
host-visible copy (shadow storage, `runtime/vk_memory.c`): the pages the CPU wrote go in before each submit, and the GPU's
writes come back at its fence. Pages both sides write keep a base copy, so each direction moves only its own bytes;
a VRAM mirror's write-back merge marks the pages it changed, and only those are compared at the fence; a lookup that
reads guest memory itself (the uniform snapshot, `vk_mem_lookup`) does not queue its pool for the copy-in. In Kokiri
Forest (1.1.1, RTX 4090) that costs about 1 ms a frame (copy-in 0.5 ms, write-back 0.6 ms), and the copies hold about
5 GB. The GPU's bytes reach guest memory in the fence's completion callback, later than an import's, so the wait
after a frame's first submission covers the write-backs there (`runtime/vk_sync.c`, "the executed timeline"): the game
rewrites what those passes wrote without a fence, and a write-back that landed after the rewrite left the GPU adding
light onto its own last output, the whole scene brighter for a frame. `NX2NAVI_SHADOW=1` turns it on under Windows too. The Steam Deck's RADV has `VK_EXT_external_memory_host` as
well: the shadow copies are chosen for the measured cost above, not for a missing extension. NVIDIA's own driver on Linux
imports instead (`NX2NAVI_SHADOW=1` brings the copies back): it imports guest memory without that cost, so there the
copies are only the slower path. In Kokiri Forest on 0.7.1 (RTX 4090, driver 615.78), imports with VRAM mirrors ran at
51.9 frames a second and the copies at 27.4; on that version the copies also flickered between two looks almost every
frame, which the wait for the write-backs under shadow storage (`vk_sync_exec_wait`) has since addressed.

The device address table (`runtime/vk_gpa.c`) names a pool's shadow copy as it names an import, so shaders follow guest
pointers into either. A shader that reads through the table resolves nothing, so the pool it reaches is not queued for
the copy-in: a guest submission whose record binds the table (`vk_exec_draw.c` `ssbo_info`) brings the shadow of
every pool in the table up to date with the pages the CPU wrote, once, before the first part recorded after the
binding is submitted, and queues the same pages for those pools' VRAM mirrors (`vk_memory.c` `vk_mem_shadow_sync`).
That copies the written pages of every CPU-visible pool once in each such guest submission, which the cost above does
not include. What a shader stores through the table lands in the shadow copy
and in the pool's VRAM mirror, and no binding names it, so nothing notes it as a GPU write: it does not come back to
guest memory, and the next whole copy-in of its page (the CPU rewrites other words of the same records) puts guest
memory's stale bytes back over it in both copies. In the runs measured so far that copy-in always came before the
frame's stores, which does no harm. `NX2NAVI_SHADOW_TABLE_STORES=1` keeps the stores instead: the copy-ins then keep a
hash of each page they write, a page whose shadow no longer matches its hash was stored into, keeps a base copy from
then on like a page with a noted GPU write, and has its stores written back to guest memory at the fence of every
submission that uses the table. It is off by default (`runtime/vk_memory.c`, "Pages a shader stores into", says why).
`NX2NAVI_SHADOW_CHECK=1` logs every 60 frames how often a copy-in overwrote such a store and for which pools, how many
write-backs landed on pages the CPU had written since their copy-in, and what the copy-ins cost and where they went.
`--probe-vkmem` checks the sync: the shadow of a pool that is never bound must hold `0x13579BDF`, keep it through a
plain CPU write and a guest submission that does not bind the table, and hold the written `0x2468ACE0` after one that
does; and with `NX2NAVI_SHADOW_TABLE_STORES=1` the store: a GPU
write into a shadow page that nothing notes must survive the CPU's writes of that page and reach guest memory. On
Linux, run it for each backing:

```sh
for shadow in 0 1; do for memory in import mirror; do
    NX2NAVI_GPA=1 NX2NAVI_SHADOW=$shadow NX2NAVI_MEMORY=$memory ./ocarina --probe-vkmem
done; done
```

The Deck's GPU (Van Gogh) has no native ASTC, so its textures go through the compute decoder (`runtime/vk_astc.c`,
`NX2NAVI_ASTC`) as on desktop AMD GPUs without it; frame times measured there include it.

## Checks

Before a commit:

- `python tools/check_content.py --staged`: nothing game-derived and no compiled shader is staged. CI runs the same check;
  `git config core.hooksPath .githooks` runs it before each local commit.
- `build\<ver>\nx2navi.exe --selftest`: layout, dispatch table and host-function registry, without game files.

A change to the in-game menu's look (`runtime/ui/menu_skin.cpp`, `option_rows.cpp`, `perf_panel.cpp`) can be looked at
without game files: `python tools/menu_preview.py` builds the menu's own drawing code with the host's clang++ and
writes the Screen tab, the full performance overlay and the boot screen to `build/menu-preview/` (`menu.png`,
`overlay.png`, `boot.png`; a stand-in font, and the window's plain border in place of the game's ornamented frame).
With `--romfs <extracted RomFS folder>` it draws the game's interface textures as the game does, and
`tools/ui_textures.py <folder>` lists the textures there and dumps those matching `--match` to `build/ui-textures/`;
both decode the game's files, so they run with `.venv-py314\Scripts\python` (`compression.zstd` and numpy). What they
write is decoded game data: keep it under `build/`.

A change to the shared save (`runtime/save_zip.c`, `runtime/save_share.c`, the save import in `runtime/fs_host.c`)
passes `python tests\save_zip_check.py` and `python tests\save_share_check.py`. The first builds `save_zip.c` alone
and checks its deflate, inflate, zip writer and reader against Python's zlib and zipfile both ways, and that the
launcher imports a zip it wrote. The second builds `fs_host.c` and `save_share.c` with the rest of the runtime stubbed
and runs each step as a start of the game on a profile it makes: the export, an import from a zip the launcher
exported, its backup and swap at the next start, the zips it refuses and Cancel. No game files.

A change to the step Ocarina is given each frame (`runtime/frame_rate.c` `ocarina_step`) passes
`python tests\frame_step_check.py`, which builds `frame_rate.c` with the rest of the runtime stubbed and feeds it
display times: a held 60 fps limit gives exactly one frame every frame, a 240 fps limit never less than a quarter
frame, uneven frame times one steady step, and game time follows the clock. No game files.

A change to the mouse camera (`runtime/mouse_direct.c`, the mouse path of `runtime/input.c`, the set `mouse_direct` of
`recomp/patches-v111.txt` or `patches-v143.txt`) passes `python tests\mouse_direct_check.py --cc <clang> --deps
<ucrt64>\include`, which builds `mouse_direct.c`, and `input.c` with `--deps`, with the rest of the runtime stubbed and
calls the camera hooks as their patch sites do, for each game version: the registers a site hands over and gets back,
the same turn for the same mouse distance at every frame rate and stick camera speed, a single count, the idle bit, the
pause after a camera reset, the capture and the mode, and scripted mouse counts. No game files.

A change to which waits a signalled event or an ended thread wakes (`runtime/kernel.c` `wake_sync_waiters`, the
`WaitSynchronization` service) passes `python tests\sync_wake_check.py`, which builds `kernel.c` with the platform
layer and the rest of the runtime stubbed and runs guest threads whose entries are host functions: a signal wakes the
waits on its event and leaves the others asleep, a wait on several handles reports the right one, a thread's end ends
the wait on it, timed waits keep their time, a cancel ends any wait, and 4,000 signals to eight looping waiters are
each seen. It runs a second time with `NX2NAVI_SYNC_WAKE_ALL=1`. No game files.

A change to the log queue (`runtime/logq.c`) passes `python tests\logq_check.py`, which builds it with the platform
layer and a destination of its own: 160,000 lines from eight threads arrive whole and in each thread's order, a line
logged while the queue's lock is taken waits and does not pass the lines queued before it, a destination that takes
30 ms a write does not hold the threads that log, the line about slow writes comes once in ten seconds, a destination
that stops while four times the queue's size is logged loses nothing, and `logq_sync` returns with everything written.
The test passes on a busy machine too: it waits for the queue to empty instead of for a time, and counts slow lines
instead of timing one. No game files.

A change to the button prompts (`tools/make_romfs_patches.py --prompts`, `tools/button_icons.py`) passes
`.venv-py314\Scripts\python tests\romfs_prompts_test.py`, which runs the tool on a RomFS it makes itself (no game
files); a change to the settings passes `nx2navi.exe --probe-cfg`; a change to the co-op layer (`runtime/coop*.c`,
`tools/coop_relay.py`) passes `python tests\coop_relay_test.py` and `nx2navi.exe --probe-coop` ([coop.md](coop.md)).

A change to the action prompt's diamond of face buttons (`runtime/action_prompt.c`) or to the SARC and zstd dictionary
helpers (`runtime/sarc.c`) passes `python tests\action_prompt_check.py`. It builds them alone, with the
undefined-behaviour sanitizer, on a layout file, archives and dictionaries it makes from the format `action_prompt.c`'s
header comment describes, and checks that:

- by label every place, cross, flip and bubble moves and no other byte, in every copy of the layout in the archive;
- a second run changes nothing, and by label then by position gives the game's file back;
- a file that is not the expected one is refused and left as it was: one value off, half laid out, one copy of four,
  damaged offsets, a pane record laid over the records that lead to another field, more than 16 copies, copies that
  share some of their bytes;
- the packed file names the dictionary the original named, found by its id in the pack of dictionaries.

No game files. `--romfs <folder with romfs.bin and index.json>` also runs it on the game's own archive: four copies laid
out, 208 fields, the same dictionary and unpacked size, and no byte changed outside the layout's copies.

A change to what the library warm logs when the driver's cache of stage libraries is cold (`runtime/vk_warm_hint.h`,
`runtime/vk_pipeline.c` `lw_main`) passes `python tests\warm_hint_check.py`, which builds the header alone and checks
the added text by GPU vendor and by why the cache was cold. No game files.

A change to how a run ends when the GPU device is lost (`runtime/vk_device.c` `vk_device_lost_check`, the checks of
`vk_lost` in the replay, upload and present paths, `runtime/vk_sync.c` `completion_main`) passes
`python tests\device_lost_check.py <game exe>`, which runs the executable's `--probe-devlost`: with the device marked
lost no completion callback runs and the submit, upload and wait paths return at once, and four threads that report a
loss at the same moment end the process with exit code 72, one device report and one FATAL line. It needs a Vulkan
device and opens no window; the device is not really lost, so what a driver answers after a loss is not covered.
`device_lost_at = <n>` in `nx2navi.ini` ends a real run the same way at its n-th hand-over to the GPU queue.

A change to the memory gate of the background pipeline builds (`runtime/vk_build_gate.h`, `runtime/vk_pipeline.c`
`gate_enter`) passes `python tests\build_gate_check.py [<game exe>]`, which builds the header alone and checks what it
admits and refuses and that builds under way count through their reservations. With the executable it also runs
`--probe-buildgate` (no device, no window, no build) with the settings in effect, with `build_free_mb = 0` and with a
value no machine has free. What the builders do when they are refused (`warm_builder`, `pipe_worker`, `lb_worker`,
`lw_worker`) shows in a run's log: start the game with `build_free_mb` set above the machine's free commit and read the
`vk_pipeline:` lines that name the commit headroom.

A change to when the pipeline caches are saved in play (`runtime/vk_save_retry.h`, `runtime/vk_pipeline.c`
`autosave_once`) passes `python tests\save_retry_check.py`, which builds the header alone: a save for new content, none
without, and after a save that did not happen a bounded row of tries at the save interval.

A change to how FSR 3.1's pipelines reach the pipeline caches (`runtime/vk_ffx.cpp` `vk_ffx_proc_addr`,
`runtime/vk_pipeline.c` `vk_pipeline_create_foreign`) passes `python tests\ffx_cache_check.py <game exe>`. It runs
`--probe-ffx-cache` (one upscaling context on the device; no window, no game) twice on pipeline cache files of its own
and once with `ffx_pipe_cache = 0`: the library's pipelines must come through the runtime's creates, the second run
must compile none of them, and with the setting off none may come through. The executable needs
`amd_fidelityfx_vk.dll` beside it; without it the probe and the test skip. The graphics driver's own cache can serve
the first run too, so to see the files alone do it, run the probe from a copy of the executable under a name the
driver has not seen: its first run compiles, its second does not.

A change to the start's memory warning (`runtime/mem_need.h`, `runtime/memory.c` `mem_start_note`) passes
`python tests\mem_need_check.py [<game exe>]`, which builds the header alone and checks when the game's need counts
as short. With the executable it also runs `--probe-memneed` (it prints the machine's commit and the answer and
starts nothing) with `mem_need_mb` at 0, at 1 and at a value no machine has, and reads the warning line of the last.
A change to how `pipeline-cache.bin` is read (`runtime/vk_cache_file.h`, `runtime/vk_pipeline.c` `cache_load`) passes
`python tests\pipeline_cache_file_check.py`, which builds the header alone and reads a sparse file just over 2 GB
through it: measured and read whole without a size limit, refused by its real size under one. No game files, no GPU;
about 2 GB of memory for a few seconds.

A change to the layout of `shader-prepared.bin` (`runtime/prp_file.h`, `runtime/gfx_shader.c`
`gfx_shader_prepare_seed`, `tools/make_game.py` `prepared_from`) passes `python tests\prp_file_check.py`, which builds
the header alone, checks the parse of both versions and has `make_game.py` read the list the C side wrote. No game
files, no GPU.

A change to where the lazy write-back is on by default (`runtime/wb_lazy_choice.h`, `runtime/vk_memory.c`
`vk_mem_lazy_decide`) passes `python tests\wb_lazy_check.py`, which builds the header alone and checks the choice and
its reason by platform, by GPU driver and by what the player set. No game files, no GPU.

A change to what writes `nx2navi.ini` outside the game (`launcher/gameini.py`, `installer/ini.py`, `tools/make_game.py`,
`launcher/updater.py`) passes the settings-file tests of `tests\launcher_test.py` (`PlayerIniTest`, and the tests named
`..._settings_file` of `BackgroundUpdateTest` and `MakeGameTest`): a player's file with lines that are no settings of
the game comes through an update and a rebuild byte for byte. The game's own writer, the settings menu, is checked by
`--probe-cfg` with the same lines.

A change to the mouse camera's stick deflection (`runtime/input_map.c` `input_mouse_stick`) passes
`python tests\mouse_stick_check.py`, which builds that file with the rest of the runtime stubbed and checks the
deflection at each camera speed: a slow movement is divided by the factor, a fast flick reaches full tilt. No game
files.

A change to how a gamepad's axes are read (`runtime/input.c` `pad_axis`: an axis counts once it has been seen at rest)
passes `python tests\pad_rest_check.py`, which builds that function with stand-ins for SDL. No game files.

A change to how a mesh pipeline's fragment module gets the inputs no mesh output feeds (`runtime/vk_pipeline.c`
`spirv_mesh_outputs`, `spirv_fs_fill_inputs`) passes `python tests\fs_fill_inputs_check.py`, which builds both
functions and runs them on mesh and fragment shaders it compiles with glslang, then validates the result. No game
files.

A change to which render passes count as drawn frame after frame (`runtime/vk_target_frames.h`: a draw into one is
skipped while its pipeline builds, a draw into any other waits) passes `python tests\target_frames_check.py`, which
builds the header alone and replays an area change after a loading screen from a player's log: the scene, probe and
half-resolution passes are per-frame again at once, a lookup table, a volume's slices and an icon capture still wait,
and an image taken over by another texture starts without the history of the one before. No GPU, no game files.

The lifter (`recomp/`) and the CPU headers (`runtime/cpu.h`, `runtime/simd.h`) are checked against Unicorn. Every lifter
change passes these, all run with `.venv`:

| Test | Checks | Game files |
|---|---|---|
| `tests\diff\lift.py` | instructions sampled from the extracted modules; `--coverage` lists every word the lifter rejects or traps | yes |
| `tests\diff\simd.py` | the whole FP/SIMD encoding space | no |
| `tests\diff\atomic.py` | exclusive load/store sequences and ARMv8.1 atomics | no |
| `tests\diff\func.py` | whole functions as the generator emits them, loops, jump tables and calls included (calls go to a stub that checks the registers at the call) | yes |
| `tests\flags_check.py` | the function-local flag helpers of `runtime\cpu.h` against its packed ones | no |
| `tests\ds3_check.py` | the DualShock 3 report decoding and output report of `runtime\input_ds3.c` | no |
| `tests\gpa_check.py` | the device address table of `runtime\vk_gpa.c` as a shader reads it: mapping, mirrors and twins, overlapping imports, and blocks given back and taken again while pool addresses climb (no GPU; any Python) | no |
| `tests\excl_mt.py` | the exclusive monitor helpers under contention and ABA, on several threads | no |
| `tests\jumptables_check.py` | `recomp/jumptables.py` against a Unicorn oracle; must report 0 disagreements, on 1.4.3 too | yes |

A `--cpu v2` build passes the same differential tests with `--cflags=-march=x86-64-v2`; they compile `runtime/fma.c`
too, so they check the software fma the v2 build links rather than the C runtime's.

On NVIDIA Linux, `NX2NAVI_DLAA=1 ./ocarina --probe-vkdevice` logs NGX initialization and shutdown when available.

Runtime probes, each a mode of `nx2navi.exe` (the list is at the top of `runtime/main.c`): `--probe-kernel`,
`--probe-vkexec`, `--probe-nvn-tex` and `--probe-shader` cover most runtime changes. `--probe-shader` and the draw
part of `--probe-vkexec` translate shaders of the census below: they look for `build/<ver>/shaders/jobs.json` of the
exe's own version first, then of the other one.
`--probe-vkmem` also checks CPU-backed texture uploads and rewrites, GPU-written texture aliases, and memory write watches.
`--probe-meshfb` (no game files) draws mesh programs of the runtime's own, with and without a task stage and with
direct and GPU-written workgroup counts, through `VK_EXT_mesh_shader` and through the mesh fallback, and compares the
pictures; on a device without mesh shaders only the fallback draws. A change to `runtime/mesh_fb.c`,
`runtime/vk_mesh_fb.c` or the fallback's part of `runtime/vk_exec_draw.c` passes it. With
`NX2NAVI_PROBE_MESHFB_DIR=<folder>` it converts the game's own task and mesh translations instead, for `spirv-val`
(the top of `runtime/mesh_fb_probe.c` says what the folder holds).
A change to the texture detail (`runtime/vk_texture.c` "texture detail": `detail_skip`, `upload`, `astc_bl_regions`)
passes it three times, with `NX2NAVI_TEXTURE_DETAIL` set to `full`, `reduced` and `low`: each time ASTC textures are
uploaded and rewritten, and every level and layer of their images must hold the game's level it stands for.
`--probe-prewarm` runs the boot's pipeline stages without the game, from the caches next to the exe (it skips when
there are none): the pipeline cache load, the shader prewarm, the recorded pipelines and the compute pipeline prebuild,
with a `mem: boot stage` log line after each that gives the device memory and the process commit so far. It then asks
for compute pipelines the boot did not keep, as their first dispatch would, and prints what one costs. The device
memory after the compute stage should be what it was before the stage, apart from the kept pipelines. A change to what
the boot builds or keeps (`runtime/vk_pipeline.c`: `prewarm_run`, `lb_compute`, the `compute-used.bin` record) passes it.

Shaders: `.venv-py314\Scripts\python tools\shader_census.py --version <ver> --translate` extracts every Maxwell shader
to `build\<ver>\shaders\` and runs it through the translator and `spirv-val`; failure groups go to `failures.txt`.
`tools\xlate_smoke.py` checks the translator alone, without game files.

A change to the SM86 tensor lowering (`tools/xlate/AmpereLowering.cs` Hmma, Ldsm, Movm, Half) passes
`python tests\tensor_lowering_check.py`: it lowers GPU-captured vectors with `nx2navi_xlate --lower` and runs them on
`tests\maxwell_warp_emu.py`, a model of what the shader translator makes of Maxwell code (the docstring names the
vector file and what is checked). `tests\maxwell_warp_emu_selftest.py` checks the emulator itself. No game files.

## Reverse engineering (optional)

- Ghidra 12.1.2 with the Ghidra-Switch-Loader extension in `toolchain\ghidra\` (JDK 21) opens the game modules (NSO).
