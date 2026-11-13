<p align="center"><img src="docs/images/navi.png" alt="NX2navi" width="160"></p>

# NX2navi

**Join the official NX2navi update discord server: https://discord.com/invite/uf6gSrsPpq** 

**NX2navi** is a static recompilation of *The Legend of Zelda: Ocarina of Time* for Nintendo Switch 2 (game
version 1.1.1) into a native x86-64 application for Windows, with a Vulkan renderer
([docs/architecture.md](docs/architecture.md)).

> [!IMPORTANT]
> **No game files are included.** You need your own legally obtained copy of the game, dumped from your own console.
> Instructions for obtaining those files will not be provided.
>
> The game is built from your files on your machine, so the result contains the game. It is for your own use. Do not share it.

## Status

Early development, playable. Kokiri Forest, Kakariko Village, Death Mountain Trail and its crater, the Temple of
Time and the Great Fairy fountains play at 60 fps and above on current GPUs, from the title screen and from saves.
Visual bugs and occasional crashes are still expected ([Known issues](#known-issues)).

Report problems as issues, following [CONTRIBUTING.md](CONTRIBUTING.md).

## Features

- Native code, not emulation
- 60 fps by default, or a limit of 30 to 240 fps, the display's refresh rate or unlimited
- Native widescreen and ultrawide (the window's shape, 21:9 or 32:9) with the HUD, menus and movies kept 16:9
- Game resolutions from 1080p to above the game's 1440p: 1800p, 4K, 5K or the window's own size, sharp on high-DPI
  displays
- Upscaling and anti-aliasing: NVIDIA DLSS (DLAA, Quality, Balanced, Performance, Ultra Performance; RTX GPUs) or
  AMD FSR 3.1 (Native AA and the same modes; any GPU) or AMD FSR 4 (the same modes; Radeon RX 6000, RX 7000 and
  RX 9000 GPUs), which render the 3D scene at the mode's share and keep the HUD sharp; or AMD FSR 1 or bilinear scaling to the window, with a sharpness setting; black bars or stretch
- Frame generation: NVIDIA DLSS Frame Generation (RTX 40 and 50 GPUs, paced with Reflex) at 2x, or 3x and 4x on RTX 50
  GPUs (one, two or three generated frames for every frame the game draws), or AMD FSR 3.1 (any GPU, Windows), a generated
  frame between every two the game draws, with the HUD kept out of it, for about twice the frame rate on a fast
  display; in a window of another shape than the picture, frames are generated inside the picture only, so the black
  bars stay clean. The frame limit counts the frames the game draws, and frame generation adds its
  frames on top of it. The frame limit can also be
  handed to RivaTuner Statistics Server
- HDR on HDR displays, two kinds: the game's scene tone mapped to the display's peak, matched to the game's mid grey and tone, with true blacks, or the game's own native HDR; peak, game and HUD brightness in the menu
- The game's known shaders prepared when the game is built, and the pipelines met in play built again in the
  background at each start, so play rarely stops to compile them; a shader or pipeline met for the first time can
  still hold a frame, and a draw that must wait for its pipeline has it built on worker threads, never on the game's
  drawing thread
- In-game settings menu (F1, or Minus and Plus together on a gamepad) and performance overlay (F10)
- Keyboard, mouse and gamepads with remapping, Nintendo, Xbox or PlayStation button layout, rumble and gyro; the game
  follows whichever connected gamepad you press, and DualShock 3 controllers are read directly when enabled
- Xbox, PlayStation, Steam Deck and keyboard and mouse button prompts in the game's own text, drawn on your PC when the game is
  built; the keyboard prompts follow your key bindings
- Stereo or 5.1 surround sound output, on the output device of your choice; the default microphone for the game's
  microphone input
- The monitor the game opens on, straight in fullscreen when fullscreen is on
- Your Discord status shows "Playing NX2navi" with the play time (on by default; the Game tab of the menu or the
  launcher's Settings turns it off)
- Docked or handheld console mode
- The game's textures stay compressed (BC3, about half the video memory at the same speed) unless the PC has 20 GiB of
  video memory and 48 GiB of RAM, as Windows counts video memory against the system's commit limit

## Installing

Download `NX2navi-Setup-<version>.exe` from the releases and start it next to your `.nup` dump. It is the only file
you need. The launcher it
installs finds the dump, downloads the build tools, builds the game on your PC (20 to 45 minutes, once) and starts
it; afterwards it keeps the port up to date and rebuilds only what changed ([docs/launcher.md](docs/launcher.md)).
[docs/README-BuildKit.md](docs/README-BuildKit.md) describes the same build by hand, [docs/building.md](docs/building.md)
building from source. Settings are in the in-game menu and in `nx2navi.ini` ([nx2navi.ini.example](nx2navi.ini.example)
lists every key). Controls: [docs/controls.md](docs/controls.md).

Requirements: Windows 10 or later; an x86-64 CPU with SSE4.2 (AVX2/FMA machines get a faster build); 16 GB of RAM; about
34 GB free for the game and 1.5 GB for the launcher and its tools; a GPU with Vulkan 1.3, `VK_EXT_external_memory_host`
and `VK_EXT_mesh_shader`. The setup is not signed, so SmartScreen warns on its first start.

## Known issues

- The first start after an install or update, and the first visit to an area, can pause briefly while the graphics
  driver builds what it has not seen; later starts are smooth.
- Machines with 16 GB of RAM can run out of system memory in Hyrule Field.
- Integrated AMD GPUs can fail at the title screen (their host memory heap is too small for the game's memory).
- A rare crash in the game's particle code.

## License

The source is public to read. Its additions and changes are all rights reserved ([LICENSE-ADDITIONS](LICENSE-ADDITIONS)):
no part of them may be copied or published without written permission. The base import stays under its
[MIT License](LICENSE), and third-party code keeps its own license ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
No license grants anything for Nintendo's game.

NX2navi is an independent, non-commercial fan project, not affiliated with or endorsed by Nintendo. *The Legend
of Zelda*, *Ocarina of Time* and *Nintendo Switch* are trademarks of Nintendo.

## Building from source

[docs/building.md](docs/building.md) lists the toolchain and the steps. `tools/make_game.py` also builds the game
natively on Linux ([docs/README-BuildKit.md](docs/README-BuildKit.md), "Linux"); the launcher and the setup are
Windows only. Everything the build writes stays under
`build\` and `generated\`, which git ignores, as it does the runtime's shaders the build compiles
(`runtime\*_spv.h`; the repository holds their source only); run `tools/check_content.py` before a commit.
