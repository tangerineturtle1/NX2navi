# NX2navi build kit

This kit builds *The Legend of Zelda: Ocarina of Time* (Nintendo Switch 2, game version 1.1.1) as a native Windows
program **on your PC, from your own copy of the game**. The kit contains no game code or data. Everything the build
writes (`ocarina.exe`, the RomFS image, the shader cache, your saves) comes from your game: keep it to yourself.

The NX2navi launcher (`NX2navi-Setup.exe`, [launcher.md](launcher.md)) does everything below for you, the
downloads included, and keeps the game up to date. This page is the same build by hand.

## What you need

- 64-bit Windows 10 or 11, 16 GB of RAM, a Vulkan 1.3 graphics card with mesh shaders and a current driver (tested on
  NVIDIA).
- Your own dump of the game as a `.nup` file (or its `exefs` and `romfs` folders), with the 1.1.1 update: program
  `0400486024DEC000`, main build `e0f3ccfa`. Other versions are refused.
- About 34 GB free on the disk you build to, besides the dump.

## One-time setup (about 15 minutes)

1. **Python 3.12 or later** from [python.org](https://www.python.org/downloads/windows/). Tick "Add python.exe to PATH"
   in the installer. Then, in a Command Prompt:

   ```bat
   py -m pip install numpy lz4 zstandard zbic fonttools pillow
   ```

   `zbic` unpacks the game's compressed program files. It is a separate GPL-2.0 package you install yourself; the kit
   only calls it. `fonttools` and `pillow` draw the Xbox, PlayStation, Steam Deck and keyboard and mouse button prompts, when the
   kit has a `prompt-art` folder.

2. **MSYS2** from [msys2.org](https://www.msys2.org/), installed to the default `C:\msys64`. Open **MSYS2 UCRT64** from
   the Start menu and paste:

   ```sh
   pacman -S --needed mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-opus mingw-w64-ucrt-x86_64-libvpx mingw-w64-ucrt-x86_64-dav1d mingw-w64-ucrt-x86_64-zstd mingw-w64-ucrt-x86_64-vulkan-headers mingw-w64-ucrt-x86_64-vulkan-loader mingw-w64-ucrt-x86_64-vulkan-memory-allocator
   ```

   Answer `Y` when it asks. If MSYS2 is somewhere other than `C:\msys64`, add `--msys <folder>\ucrt64` to the build
   command.

## Build (about 10 to 15 minutes, once)

Unpack this kit anywhere, open a Command Prompt in its folder and run:

```bat
py -I tools\make_game.py --nup "C:\path\to\your game.nup" --out "C:\Games\Ocarina"
```

With extracted folders instead, use `--exefs "C:\path\exefs" --romfs "C:\path\romfs"` in place of `--nup`. To only
check that your dump is the right version, add `--check` (it builds nothing).

The build checks the version, unpacks the program files, packs the 26 GB RomFS image (every file is checked against
the dump's own checksums) while it turns the game's code into C and compiles it (about 6 minutes on a 16-thread
Ryzen 7; longer on fewer cores), then prepares every one of the game's shaders for your graphics card (about 5 more
minutes), so the game does not stop to compile shaders while you play. When the kit has a `prompt-art` folder, it
last draws the Xbox, PlayStation, Steam Deck and keyboard and mouse button prompts from that art (about a minute) into the
`romfs-patches-*` folders beside the game. The game it builds is for this PC's processor. You can use your PC
meanwhile. If it stops, run the same command again: finished steps are skipped.

When it says `finished`, start `C:\Games\Ocarina\ocarina.exe`. You can delete `C:\Games\Ocarina\build-cache` (about
1.5 GB) afterwards.

## Linux

The same command builds a native Linux `ocarina` on an x86-64 Linux 6.7 or later (the write watch needs
`PAGEMAP_SCAN`) with a Vulkan 1.3 driver that has `VK_EXT_external_memory_host` and `VK_EXT_mesh_shader` (NVIDIA,
RADV). It takes clang, clang++ and lld from `PATH` and links against the system's libraries; `--msys` is not used.

1. Packages, on Arch and its derivatives (other distributions: the same libraries and their `-dev` headers):

   ```sh
   sudo pacman -S --needed clang lld sdl3 opus libvpx dav1d zstd vulkan-headers vulkan-icd-loader \
       python python-fonttools python-pillow python-numpy python-lz4 python-zstandard dotnet-sdk
   ```

   Python 3.10 or later (with 3.14 the RomFS step uses its own `compression.zstd`, else the `zstandard` package; the
   button prompts need `fonttools` and `pillow` when the kit carries button art), then `zbic`, which the kit only calls
   (GPL-2.0): `pip install --user zbic`, or in a virtual environment with the packages above.
   Vulkan Memory Allocator is one header: the system's (AUR `vulkan-memory-allocator`), else the build fetches the
   pinned 3.4.0 into `toolchain/vma/` and checks its hash.
   The Vulkan headers must be release 363 (1.4.363) or newer: the runtime names extensions that older headers do not
   declare (`VK_KHR_pipeline_binary` among them), and distributions with long support package older ones. The
   AppImage carries the pinned headers (`toolchain/vulkan-headers/`, `tools/vulkan_pin.py`) and builds against them
   whatever the system has; a build from a kit or a source checkout uses the system's when they are new enough and
   otherwise fetches the pinned ones there once and checks their hash. The loader's library still comes from the
   system (`vulkan-icd-loader`, `libvulkan-dev`, `vulkan-loader-devel`).
   The compiler is the system's too. The releases are built with clang 23; an older one works as far as it compiles
   the runtime (one without `#embed`, before clang 19, takes the menu's font from a header the kit carries). Before
   its long steps the build checks every runtime source with that compiler and links against the libraries, so a
   compiler, a header or a library it cannot use stops it in the first seconds, with the compiler's own words, and
   not after the RomFS image.

2. The shader translator: a kit with `bin/nx2navi_xlate` has it; from a source checkout, build it once with
   `python tools/build_xlate.py` (needs the .NET 10 SDK). The build copies it beside the game. A kit also carries the
   runtime's own shaders compiled; from a source checkout the build compiles them first, with `glslc`, `spirv-as` and
   `spirv-val` (on Arch the packages `shaderc` and `spirv-tools`).

3. Build, then play:

   ```sh
   python3 -I tools/make_game.py --nup "/path/to/your game.nup" --out ~/Games/Ocarina
   ~/Games/Ocarina/ocarina
   ```

With 16 GB of RAM, add `--jobs 4`: the compiles run beside the 26 GB RomFS image, and one clang per logical
processor can fill memory and swap until the kernel stops processes of the desktop session (about 10 minutes on a
12-thread Ryzen with 4 jobs). Do not play or prepare shaders while it builds.

The profile, logs and settings are in the `--out` folder as on Windows.

On NVIDIA the driver keeps the shaders it compiled in `nvidia-cache` beside `ocarina`, not in `~/.cache/nvidia`, where
one run adds at most 1 GiB while the preparation at the end of the build makes about 1.5 GB. The game makes and sets
the folder unless the environment sets `__GL_SHADER_DISK_CACHE_PATH`, and a limit of 8 GiB unless it sets
`__GL_SHADER_DISK_CACHE_SIZE`. When it cannot make or write the folder, it sets neither and the driver keeps its own
folder and limit; the log says which (`vk: NVIDIA's shader disk cache`). Deleting the folder costs one slow start;
`./ocarina --warm-shaders` fills it again.

The game reads the memory it may use: the limit of its cgroup v2 (`systemd-run --user --scope -p MemoryMax=16G
ocarina`, a container's) and the machine's `MemAvailable`, whichever leaves less. Linux does not fail an allocation when
memory runs short, it ends the process, so while less than the larger of 4 GB and a tenth of that limit is left the game
drops stale textures and evicts idle ones. The log names the limits at the first check (`platform: memory limits`). That
cannot give back what the game needs after its start: about 10 GB in Kokiri Forest at a 2160p game resolution.

Without NVIDIA's driver, the game hands its memory to the GPU through `/dev/udmabuf` instead of copying it there for
every submission, which is faster on AMD cards (61 frames a second instead of 40 in Kokiri Forest on an RX 6800 XT).
That needs the device readable and writable by you (recent systemd gives it to the logged-in user) and udmabuf's
`size_limit_mb` (`/sys/module/udmabuf/parameters/size_limit_mb`) at 1024 or more (`udmabuf.size_limit_mb=1024` on the
kernel command line raises it). Otherwise the game copies as before, and the log's `mem:` lines say why
(`docs/building.md`, "Memory").

AMD FSR 3.1 loads AMD's `amd_fidelityfx_vk.dll`, which only the Windows build can use: on Linux the menu offers neither
FSR 3.1 upscaling nor FSR 3.1 frame generation, and `taa = fsr3` or `framegen = fsr3` in `nx2navi.ini` leave the
game's own scaling and swapchain as they are (the log says so). AMD FSR 4 is in the Windows build alone for the same
reason, and `fsr4 = 1` does nothing on Linux.

NVIDIA DLSS (RTX cards) needs the driver's own `libnvidia-ngx.so.1` on the library path (the proprietary driver's
utilities package, `nvidia-utils` on Arch) and NVIDIA's DLSS library `libnvidia-ngx-dlss.so.<version>` beside
`ocarina`. The AppImage's kit has the library in `bin/` with `libnvidia-ngx-dlss-NOTICE.txt`, which names its licence,
and the build copies both beside `ocarina` (`tools/make_appimage.py --no-dlss` leaves it out; such an AppImage
downloads the same pinned library from NVIDIA's repository after a build on a machine with NVIDIA's driver, checks its
SHA-256 and writes the notice, unless `NX2NAVI_NO_DLSS=1` is set or a library is already there). From a source checkout,
or to use another DLSS version, put the library there yourself: NVIDIA publishes it as
`lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.<version>` in [its DLSS repository](https://github.com/NVIDIA/DLSS), tagged
`v<version>`, and `tools/dlss_pin.py` names the version the kit carries. Later builds keep a library they did not
place. Without it DLSS does nothing on Linux.

DLSS Frame Generation (RTX 40 and 50 cards) runs through NGX itself on Linux, without Streamline. It needs NVIDIA's
`libnvidia-ngx-dlssg.so.<version>` beside `ocarina` (the same repository, `lib/Linux_x86_64/rel/`; no kit carries it)
and DLSS upscaling on when the game starts. *Frame generation* in the Screen menu then offers NVIDIA DLSS, which
switches at once, with a *DLSS multiplier* of 2x to 6x (above 2x on RTX 50 cards) or Dynamic, which generates only the
frames needed to reach the display's refresh rate (`framegen = dlss` and `fg_mult` in `nx2navi.ini`). The log says
`ngxfg: DLSS Frame Generation available` when NGX found the library.

`tools/make_appimage.py` packs this kit into one `NX2navi-Linux-<version>-x86_64.AppImage` with its own Python and the
pinned Vulkan headers, for players without a source checkout: it checks for the packages above (their Vulkan headers
are not needed), installs zbic with pip on its first run, picks
`--jobs` from the memory, builds into `~/Games/Ocarina` (or the folder given after the dump) and writes `play.sh`
beside the game.

## Playing

- **F1** opens the settings menu. Settings are saved in `nx2navi.ini` beside the game; saves go to `profile\`.
- Every shader was prepared during the build. After an update of your graphics driver, the next start shows
  "Preparing shaders" once while the driver rebuilds them. With a driver that has Vulkan's graphics pipeline library
  it lasts at most about 20 seconds once the shaders are translated, and the remaining pipelines are built in the
  background while you play.
- On an NVIDIA card, set **Shader Cache Size** to **Unlimited** in the NVIDIA app (Graphics, Global Settings). The
  driver keeps the prepared shaders' libraries in that cache, and a smaller limit drops the oldest as new ones come in
  (a 2048 MB cache could not hold them): every start then builds them again in the background, and play stutters, with
  some things missing for a few frames, until that is done. The log's `library warm` lines say when this happens.
- With 16 GB of RAM, give Windows a fixed page file whose initial size is at least 32 GB (System, About, Advanced
  system settings, Performance Settings, Advanced, Virtual memory: untick the automatic management, Custom size with
  an initial size of 32768 MB or more, Set, then restart). Windows counts the game's video memory
  against RAM plus page file, and a page file Windows sizes itself grows too slowly for Hyrule Field: the game then
  lets textures go and the ground can flash white or black. The log says `system commit low` when this happens.
- **Screen** in the menu has Texture filtering (anisotropic, up to 16x, at once), Texture detail (Reduced and Low
  leave out the largest versions of the big surface textures so that the game fits the video memory of 6 and 8 GB
  cards; Automatic picks by the card), Upscaling (DLSS on NVIDIA RTX cards,
  FSR 3.1 on any card where `amd_fidelityfx_vk.dll` loads, FSR 4 on the Radeon cards AMD's FSR 4 libraries list it
  for), Frame generation (NVIDIA DLSS on RTX 40 and 50 cards, with
  a DLSS multiplier of 2x, or 3x and 4x on RTX 50 cards; FSR 3.1 on any card where it loads; it needs DLSS or FSR 3.1
  upscaling on; the frame limit counts the frames the game draws and the generated ones come on top), HDR (the game's picture tone mapped to your display, or Native, the game's own HDR, with peak, game and HUD brightness; turn HDR on in Windows first) and the
  resolution. A setting that takes effect only when the game starts again says "after restart" once you change it,
  until you set it back, and the Restart button at the bottom then counts the settings waiting for it.
- **Controls** in the menu has Button prompts: the game's own, or Xbox, PlayStation, Steam Deck or keyboard and mouse ones when
  the build drew them (a kit with `prompt-art`). It takes effect after a restart. The keyboard and mouse prompts show
  the keys `nx2navi.ini` binds when the build runs: after rebinding keys, run
  `py -I tools\make_game.py --rebuild --out "C:\Games\Ocarina"` to draw them again (it skips everything else).
- The build copies NVIDIA's DLSS library `nvngx_dlss.dll` (with `nvngx_dlss-NOTICE.txt`, which names its licence)
  and FSR 3.1's `amd_fidelityfx_vk.dll` beside `ocarina.exe` from the kit's `bin\`, and FSR 4's
  `amd_fidelityfx_loader_vk.dll` and `amd_fidelityfx_upscaler_vk.dll` when the kit has both (and
  `amd_fidelityfx_framegeneration_vk.dll` for `fsr4_fg`). To use another DLSS version, put
  your own `nvngx_dlss.dll` there: later builds keep it. On Linux the library is `libnvidia-ngx-dlss.so.<version>`
  (see "Linux").
- A kit with `bin\dlssg\` (made with `tools/make_kit.py --dlssg`) also brings DLSS Frame Generation on GeForce RTX 40
  and 50 cards: the build copies `nx2navi_dlssg.dll`, NVIDIA Streamline's runtime, `nvngx_dlssg.dll` and their licence
  texts beside `ocarina.exe`, and Frame generation in the Screen menu then offers DLSS too.
- Logs are in `logs\`. When reporting a problem, say where you were in the game and which graphics card you have, and
  attach `logs\nx2navi.log`. Never send game files.

## If something goes wrong

| Message | What to do |
|---|---|
| `not game version 1.1.1` | Install the 1.1.1 update and dump the game again |
| `the Python package ... is missing` | Run the `pip install` line above |
| `...\clang.exe is missing` | Run the `pacman` line above in MSYS2 UCRT64 |
| `clang is not on PATH` (Linux) | Install the packages under "Linux" |
| `nx2navi_xlate is missing` (Linux) | Build the translator with `python tools/build_xlate.py` |
| `needs ... GB free` | Free space on the `--out` disk, or build to another one |
| The game opens and closes | Update your graphics driver, then send `logs\nx2navi.log` |
