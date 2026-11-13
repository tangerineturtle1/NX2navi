# NX2navi audit

This file holds the audit's state across sessions: the project context, the subsystem map, the threads and what they
share, the path of one frame, the allocators, pools and caches, the leads for Phase 3, the findings ledger and what
has been reviewed.

- Phase 1 (map, no code changes): written 2026-11-10 on branch `audit`, cut from `test-all-fixes` at `ed36773d`.
- Phase 2 (baseline and instrumentation): done 2026-11-10 on branch `claude/audit-phase2-a12eef`. It has three
  instrumentation commits (`c1ed77e4`, `d49a4dea`, `3a88b363`) and fixes nothing. See section 11.
- Phase 3 (the eight passes): started 2026-11-10 and stopped at the user's request before its end. Section 12 says what is done and what is left.
- Phase 4 (fixes): started 2026-11-10 at the user's word; 17 fix commits, section 13 says which are verified.
- Phase 5: not started.

Citations name our code by file and function; a `~line` after them is a hint taken at `ed36773d` and will drift.

Rules this file follows:

- A **finding** is labelled "confirmed" (reproduced, or shown by tool output) or "suspected" (from reading code only).
- A **lead** is something seen while mapping that Phase 3 should look at. Every lead is suspected, and none is a
  finding yet.
- Where the docs and the code disagree, the map follows the code. Section 6 lists every disagreement.

---

## 1. Project context

**What it is.** A static recompilation of *Ocarina of Time*, the Switch 2 release (ARM64, game version 1.1.1), into a
native Windows x86-64 executable with a Vulkan renderer. The launcher builds the game on the player's PC from the
player's own dump. Design: `docs/architecture.md`. Build and checks: `docs/building.md`. Launcher: `docs/launcher.md`.
Maxwell behaviour the Vulkan path must match: `docs/maxwell.md`.

The runtime also carries code for other builds of the game, gated on `rt_game_version` (`host.h`): 110, 121 and 143.
`tools/make_game.py` writes `rt_game_version = 111`. The audit targets 1.1.1. Some files (`mouse_direct.c`,
`menu_cursor.c`, parts of `ov_zstd.c`) describe themselves as 1.4.3 features; which of their paths run on 1.1.1 is
not determined yet.

### Repository layout

| Folder | What it holds |
|---|---|
| `recomp/` | The recompiler: an ARM64-to-C lifter and generator in Python. `gen.py` is the generator, `lifter.py` the decoder, `simd_c.py` the NEON/FP lowering, `flags.py` and `regs.py` the locals for flags and registers, `jumptables.py` the jump tables. `layout.py` alone decides module bases. `patches.py` and `patches-v111.txt` hold the game-code patch sets. |
| `runtime/` | The host runtime in C and C++: 125 `.c` files, 15 `.cpp`, 70 headers. |
| `launcher/`, `installer/`, `tools/` | The build kit. `tools/make_game.py` is the build a player runs. |
| `tests/` | The checks. |
| `docs/` | The design. |

The recompiled output is not in the repository: it is generated under ignored folders.

### Stack

- Our own static recompiler. It is written in Python and outputs C with GNU extensions.
- The runtime is C11 (plus C++ for the menu, DLSS frame generation and AMD FidelityFX), built with MSYS2 UCRT64 Clang.
- A Vulkan 1.3 renderer with `VK_EXT_mesh_shader`, `VK_EXT_external_memory_host` and the graphics pipeline library
  where the driver has it. Device memory goes through Vulkan Memory Allocator.
- Shaders are translated to SPIR-V by a separate translator process (`nx2navi_xlate.exe`) into an on-disk cache,
  prepared at install time.
- SDL3 for gamepads and audio output, Win32 for the window, keyboard and mouse. libopus for voice, libvpx and dav1d for
  movies, libzstd, and Dear ImGui for the menu.

### Targets

- PC only. There is no fixed resolution or frame rate.
- The frame limit (`NX2NAVI_FPS_LIMIT`, `cfg.c` default 60, live; 30 to 240, `display` or `unlimited`) counts frames
  the game draws. Frame generation multiplies on top of it.
- Reference PC: RTX 5090. The user plays at a 120 limit with DLSS frame generation x2 and HDR.
- Minimum: 16 GB RAM and a Vulkan 1.3 GPU with mesh shaders.

### Memory budget

- **Guest application pool.** The pool is `rt_app_pool_bytes`, which `tools/make_game.py` generates as 9809 MiB.
  - `memory.c` `mem_init` (~860) actually sets the limit to `NX2NAVI_APP_POOL` when that is set. Otherwise it uses
    `rt_app_pool_bytes + res_heap_extra_bytes()`, and the extra comes from the resolution settings (`resolution.c`).
    So the pool can be larger than 9809 MiB.
  - The limit is enforced in `mem_set_heap_size` (~899) and `alias_map` (~1080). The heap is also capped at
    `HEAP_CAP` (12 GiB).
- **VRAM.** VRAM follows the driver's heap budget (`VK_EXT_memory_budget`, `vk_memory.c` `vram_query`).
  `NX2NAVI_VRAM_BUDGET_MB` caps it (`vk_memory.c` `budget_cap`, a test switch).
- **CPU and GPU memory are separate.** The game's guest memory pools are imported into Vulkan as host memory. On a
  discrete GPU they are mirrored into device-local buffers (`vk_memory.c`).

### Game logic rate

- The game ships with whole-vsync 30 fps steps.
- Patch set `ostep` makes it step by the clock, up to 240 game frames a second. The step is written each present by
  `frame_rate.c` `frame_step_present`, then `ocarina_step`, then `ocarina_step_write`.
- Several known bugs come from game code that assumes the original rate.

### Commands for the later phases

Phase 1 ran nothing.

- **Dev builds** run from `C:\Users\PC\Documents\deepseek-harness\default-workspace`:
  - `C:\msys64\ucrt64\bin` first on PATH, with system Python 3.12;
  - a copy of `work\build_rt_allfix.py` pointed at this worktree.
- **Scripted game runs** use a runner script with an input script and a save folder; a Kakariko Village save exists.
  Useful settings: `NX2NAVI_TRACE=perf` and `NX2NAVI_PROFILE`.
- **Launcher tests:** `python -m unittest tests.launcher_test`.
- **Runtime self-checks:** `nx2navi.exe --selftest`, `--probe-kernel`, `--probe-vkexec`, `--probe-vkmem`,
  `--probe-cfg`, `--probe-prewarm`.
- **Unit checks that build one file with the rest stubbed:** the `tests\*_check.py` files listed in
  `docs/building.md`, "Checks". Examples: `logq_check.py`, `sync_wake_check.py`, `save_share_check.py`,
  `save_zip_check.py`, `frame_step_check.py`, `wb_lazy_check.py`, `target_frames_check.py`.

### Known problems at the start of the audit

These are what the user or players see. None is a finding until it is reproduced or traced.

**Stutter and speed**
- Stutter and frame-time spikes at uncapped frame rates in the user's own play. Not reproduced in scripted runs.
- Big towns run slower than the field in play. Kakariko Village measures 135-138 fps in scripted runs on the reference
  PC; the user reports heavy slowdowns in play.

**Rendering**
- White, untextured ground after long play under VRAM pressure. A fix is in; it is unconfirmed.
- A one-frame dark shadow at the Castle Town gate. Switch `depth_alias` addresses it and is off by default.
- Foliage flicker on Linux with an AMD card. Open.

**Crashes and device loss**
- A device loss on an RX 6600, and a crash while fighting on AMD cards. Lazy write-back is now off by default on AMD's
  Windows driver (`wb_lazy_choice.h`, `vk_memory.c` `vk_mem_lazy_decide`). Untested: there is no AMD card here.
- Crashes with third-party overlay layers injected into the process.

**Logic tied to the frame rate**
- The rupee pick-up popup stays above Link above 60 fps. Patch set `opickup` addresses it; it is proven statically,
  not seen in game. An item jingle glitches the same way.
- The ocarina song recorder rounds wrongly at high frame rates: four sites, unfixed.

**Other**
- The shader cache pack grows with every translator change.
- The replay on its own thread (`NX2NAVI_REPLAY_ASYNC=1`) has a known fault and stays off.
- `tests.installer_test` cannot be imported on this PC.

### Parts of the audit prompt that do not apply

The prompt was written for a port to the Switch 2. This project goes the other way: a Switch 2 game recompiled for
the PC.

- **"On device", "Switch 2 build", "docked", "handheld".** Do not apply. There is no device build and no docked or
  handheld mode. The only targets are Windows (and an in-progress Linux build, `docs/building.md`), at whatever
  resolution and frame limit the player picks. No substitute target is invented here.
- **"Display list buffers".** The game does not build N64-style display lists. It records NVN command buffers, which
  the runtime replays into Vulkan. The prompt's display list is read as the NVN command buffer throughout.
- **"Recompiler config".** The configuration is `recomp/patches-v111.txt` plus the generator's options. Our host code
  is `runtime/`.
- **"Emulated RAM".** Guest memory: the game's address space at fixed host addresses below 2^39 (`memory.c`).
- **"Target frame rate".** The player's frame limit, not a fixed 30 or 60.
- **"Stays inside its memory budget".** Two budgets: the guest pool above (CPU side) and the driver's VRAM budget.
  Process commit charge matters too: `vk_texture.c` evicts on commit pressure, `commit_evict`.
- **"Test route ... a boss ... save, quit, reload".** Applies as written, on PC. The Phase 2 baseline also needs the
  route at the user's own settings (a 120 limit, DLSS frame generation x2, HDR), because the stutter reports come from
  those.
- **"Original-game crashes handled more gracefully".** Applies. On the PC, a guest fault goes to the crash handler:
  `platform/win32.c` `crash_veh`, then `main.c` `crash_handler`.

### Sanitizer builds (Phase 2): what the code says

This was written before Phase 2 tried anything. The results are in section 11.4:
- UBSan (trap mode) built and ran the route, with one report (F-1-02).
- ASan built but cannot start: its shadow covers the guest's fixed windows, as feared below.
- TSan does not exist here.

**AddressSanitizer and UndefinedBehaviorSanitizer on Windows**

- *Fixed reservations.* The runtime reserves guest memory at fixed addresses below 2^39 before anything else
  (`memory.c` `mem_init`):
  - the module window;
  - the 256 MiB kernel region;
  - a 14 GiB low region (stacks and heap);
  - a 64 GiB alias region.

  It re-executes itself when a range is taken (`NX2NAVI_REEXEC`, `REEXEC_LIMIT` 20). An ASan shadow mapping placed in
  that range would make every start re-execute and then fail. Whether the Windows ASan runtime's shadow placement
  collides is not determined.
- *Faults are part of normal operation.* The runtime registers a vectored exception handler first
  (`platform/win32.c` `plat_mem_fault_hook`, `AddVectoredExceptionHandler(1)`) and takes access violations on purpose:
  - write-watch freeze and thaw (`memory.c` `thaw_fault`);
  - lazy write-back pages (`memory.c` `lazy_fault`, which runs a GPU copy inside the handler);
  - probe pages.

  ASan on Win64 commits its shadow memory on demand from an exception handler of its own. The order of the two
  handlers, and whether ours tolerates the faults ASan takes, would need a test.
- *Stack tricks.*
  - Guest threads reserve 256 MiB host stacks (`kernel.c` `HOST_STACK`).
  - Guest code runs on fibers (`kernel.c` `kern_fiber_initialize`, `plat_fiber_*`).
  - Frames are left by non-unwinding jumps (`plat_jmp_go`, used for ExitThread).
  - Asynchronous preemption suspends a thread and rewrites its instruction pointer to `kern_ap_stub`
    (`kernel.c` `ap_try`).

  ASan's fake stacks and its stack bookkeeping would need fiber annotations and probably
  `detect_stack_use_after_return=0`.
- *Generated code.* Instrumenting the generated C would put a shadow check on every guest load and store. That is
  slow, but possible in principle. Instrumenting only `runtime/` is the smaller first step.
- *Toolchain.* Whether MSYS2 UCRT64's clang ships the ASan and UBSan runtimes for the `x86_64-w64-windows-gnu`
  target was not checked.

**ThreadSanitizer**

- It does not exist for Windows targets.
- The Linux build (`runtime/platform/linux.c`) is the only place it could run. Its memory layout would have to accept
  the fixed guest reservations below 2^39 and the userfaultfd write watch; not determined.
- It would also see the guest's own lock-free per-core data, which the kernel keeps safe by its virtual-core rule
  (section 3.3), as races. Expect noise there.

---

## 2. Subsystem map

There are 22 subsystems. Files are under `runtime/` unless the path says otherwise.

| # | Subsystem | Files | What it owns |
|---|---|---|---|
| 1 | Recompiler (build time) | `recomp/*.py`, `recomp/patches-v111.txt` | Lifting ARM64 to C; module bases (`layout.py`); jump tables; patch sets (`patches.py`). Output is generated C under ignored folders (never edited by hand). |
| 2 | Process entry and boot | `main.c`, `boot.c`, `loader.c`, `hostfn.c`, `dispatch.c`, `patches.c`, `guest_setjmp.c`, `fma.c`, `cpu.h`, `simd.h`, `simd_fp.h` | Command line and modes; loading the lifted modules at their bases, linking host overrides (`HOST_OVERRIDE`/`HOST_OBSERVER` tables, `hostfn.c`); the indirect-branch table (`rt_dispatch`); runtime switching of patch sets (`rt_patch_on`, `rt_patch_switch`); guest setjmp/longjmp; the boot sequence and shader/pipeline prewarm wait (`boot.c` `rt_boot`, `boot_wait`). |
| 3 | Kernel and scheduling | `kernel.c`, `kernel.h`, `kernel_ap_stub.h`, `svc.c` | Guest threads (one host thread each), six virtual cores and their hand-off, preemption (`core_kick`, async preemption), waits and events, the handle table, fibers, system events, SVC entry. |
| 4 | Guest memory | `memory.c`, `platform/win32.c` (reserve, commit, write watch, fault hooks) | The fixed guest address space; heap, stacks, alias region and app-pool limit; the kernel-region allocator; write watch; freeze/thaw of idle regions; lazy write-back of GPU writes (armed pages fetched on fault). |
| 5 | Services (in-process Horizon) | `ipc.c`, `ov_system.c`, `net_offline.c`, `nfp.c`, `amiibo.c`, `swkbd.c`, `swkbd_win.c`, `swkbd_sdl.c`, `ov_media.c` (Opus) | `sm:` and the service table; oe/applet/am/account/time and others answered on the host; offline sockets; amiibo; the software keyboard window. |
| 6 | Files: RomFS, decompression, mods | `romfs.c`, `romfs_path.h`, `restbl.c`, `ov_zstd.c`, `mods.c`, `cheats.c` | The read-only game RomFS from the player's image with overlays (`romfs-patches*`, mods); the resource size table; host zstd decode with dictionaries; mod scan and order; cheats from the mods' cheat files. |
| 7 | Save data | `fs_host.c`, `save_share.c`, `save_zip.c`, `sha256.c` | The game's save mount on the host file system, the save transaction (work copy, commit, swap, backups), recovery at start, import and export zips. |
| 8 | Settings and logging | `cfg.c`, `cfg.h`, `logq.c`, `main.c` (log sink, crash handler), `perf.c` (switches `rt_knob`, `rt_ab`) | Every setting with its default (`nx2navi.ini`, `NX2NAVI_*`), live changes and saving; the log queue and its writer thread; log rotation; the crash report. |
| 9 | Frame pacing and game timing | `frame_rate.c`, `nvn_api_obj.c` (`present_period`, `pace`, `nvnWindowAcquireTexture`), `rtss_limit.c`, `resolution.c`, `widescreen.c`, `dynres.c` | The frame limit and present slots; the game's frame step (`ostep`); the world clock; the RivaTuner limit hand-off; render resolution, aspect and dynamic resolution. |
| 10 | NVN front end | `nvn_api_core.c`, `nvn_api_cmd.c`, `nvn_api_obj.c`, `nvn_ext.c`, `nvn_procs.c`, `nvn_procs.inc`, `nvn_tex.c`, `tex_deswizzle.c`, `ov_gfx.c`, `nvn.h`, `nvn_api.h`, `nvn_obj.h`, `nvn_hmap.h`, `nvn_pool_index.h` | The NVN API the game calls: devices, queues, memory pools, textures and samplers, command buffer recording into op records, deferred binds, recording copies and pins, submit and present entry points, texture layout and deswizzle. |
| 11 | Replay and submission | `vk_backend.c`, `vk_exec.c`, `vk_exec_draw.c`, `vk_exec_xfer.c`, `vk_exec.h`, `vk_rec.c`, `vk_rec.h`, `vk_sync.c`, `vk_usnap.c`, `vk_target_frames.h` | Replaying NVN op records into Vulkan command buffers, draws, binds, the bindless pools, copies, events and counters, event order across queues; the recorder thread's packet ring; timelines, submit batching, the completion thread; the uniform snapshot; frame hold. |
| 12 | GPU memory | `vk_memory.c`, `vk_vma.cpp`, `vk_headroom.c`, `vk_gpa.c`, `vk_gpa.h`, `vk_vaddr.c`, `wb_lazy_choice.h` | Import of guest pools, VRAM mirrors and their eviction, shadow storage, upload batches and staging, deferred destruction, write-back of GPU writes, the lazy write-back's GPU side, the device address table, the VRAM budget, the headroom spare. |
| 13 | Textures | `vk_texture.c`, `vk_astc.c`, `tex_astc.c`, `tex_astc_mt.c`, `vk_formats.c`, `tex.h` | The texture image cache and its eviction (release, staleness, idle, disuse, VRAM and commit pressure), samplers, texture detail, uploads and ASTC decode (GPU, CPU fallback). |
| 14 | Shaders | `gfx_shader.c`, `gfx_shader_guess.c`, `gfx.h`, `tools/xlate/` (translator source), `tools/vendor/` (pinned translator) | Translation through child processes, the memory cache, the disk cache and pack, the carry of the previous translator's translations, prewarm and preparation, disk-read budget. |
| 15 | Pipelines | `vk_pipeline.c`, `vk_warm_hint.h`, `vk_caps.c` | Graphics and compute pipelines, stage libraries and fast links, background workers, the boot prewarm and prebuild, driver cache warm checks, the pipeline cache file and autosave, records (`pipeline-keys.bin`, `compute-used.bin`), device capability decisions. |
| 16 | Device and present | `vk_device.c`, `gfx_present.c`, `gfx_window_win.c`, `gfx_window_sdl.c`, `gfx_wsys.h`, `vk_hdr.c`, `vk_fsr.c`, `vk_color.c`, `vk_tonemap.c`, `vk_contact.c`, `gfx_null.c` | Instance and device, the queue lock, the swapchain, the present blit and FSR 1, HDR, colour adjustment, the window thread, the vsync thread, the scene upscaler stand-in, contact shadows. |
| 17 | Upscaling and frame generation | `vk_dlaa.c`, `vk_dlaa.h`, `vk_ffx.cpp`, `vk_fg.cpp`, `vk_dlssg.c`, `dlssg/nx2_sl.cpp`, `vk_mvprev.c`, `vk_mvsurvey.c`, `vk_mvcheck.c` | DLSS/DLAA through NGX, FSR 3.1 upscaling and frame generation, DLSS frame generation through `nx2navi_dlssg.dll`, motion vectors. |
| 18 | Audio | `audren.c`, `audren.h`, `audio_mixer.c`, `audio_effects.c`, `audio_reverb.c`, `audio_capture.c`, `ov_audio_input.c`, `menu_sound.c` | The game's audio renderer (`audren:u`) on a host render thread, the mixer and its SDL output ring, effects, the microphone, menu sounds. |
| 19 | Movies | `ov_movie.c`, `ov_movie2.c`, `movie.h` | Host VP9 (libvpx) and AV1 (dav1d) decoders standing in for the SDK's, with their threads and frame queues. |
| 20 | Input | `input.c`, `input_win.c`, `input_sdl.c`, `input_map.c`, `input_ds3.c`, `input_ds3_win.c`, `ov_input.c`, `mouse_direct.c`, `motion_fusion.c`, `camera_speed.c`, `menu_cursor.c` | Gamepads (SDL3, XInput, DualShock 3), keyboard and mouse, bindings, the state the game's HID calls read, rumble, motion, mouse camera, camera speed rewrite, menu pointer. |
| 21 | In-game menu and overlay | `ui/*.cpp`, `ui_text.c`, `menu_assets.c`, `folder_pick_win.cpp`, `folder_pick_sdl.c`, `perfmon.c`, `perfmon.h`, `sysinfo.c` | The Dear ImGui menu drawn on the present path, its art and fonts from the game's files, the performance overlay and its sampler, save export/import from the menu. |
| 22 | Diagnostics | `perf.c`, `prof_win.c`, `prof.h`, `flight.c`, `vk_dump.c`, `vk_crumbs.c`, `rdoc.c`, `discord_rpc.c`, `*_probe.c` | Frame timing and counters, the sampling profiler, the flight recorder, frame dumps, GPU breadcrumbs, the external capture hook, Discord presence (not diagnostics but a side service), the runtime probes. |
| — | Platform | `platform/platform.h`, `platform/win32.c`, `platform/linux.c`, `platform/common.c`, `platform/fp_x86.h` | Threads, locks, events, atomics, fibers, virtual memory, write watch, fault and crash hooks, files (atomic writes), child processes, CPU reservation. |
| — | Build kit and launcher | `tools/make_game.py`, `tools/prepare_shaders.py`, `tools/*`, `launcher/*`, `installer/*` | The player's build, shader preparation, updates, save import/export from outside the game. Not part of the running game; mapped only by what the docs say. |

---

## 3. Threads

### 3.1 Primitives

From `platform/platform.h` and `platform/win32.c`.

- **`PlatMutex`.** A Windows SRW lock, taken exclusive or shared. Not recursive.
- **`PlatRMutex`.** A recursive mutex: an owner and a depth over a `PlatMutex` (`platform/common.c`).
- **`PlatCond`, `PlatEvent`, `PlatOnce`.** Condition variables, events, and one-time init.
- **`plat_thread_new` / `_ex`.** Creates a joinable thread: suspended `CreateThread`, named, then resumed.
  - The default stack reservation is 16 MiB (`PLAT_STACK_DEFAULT`).
  - `plat_thread_spawn` does the same and then detaches. Detaching closes the handle and leaves the small
    `PlatThread` struct (commented as intended).
- **Per-thread handles.** These `__thread` handles are never closed when a thread exits:
  - the high-resolution waitable timer behind `plat_wait_ns`;
  - the `plat_file_pread` and `plat_file_pwrite` events.

  See lead L-26.
- **`NVN_BLOCKING` and `kern_core_away`.** A guest thread that makes a long host call gives its virtual core away
  first, then takes it back after the call. This covers submit, present, fence waits, shader translation, `nn::fs`
  calls and pool setup.

### 3.2 Thread table

About 55 kinds of thread. "Detached" means started with `plat_thread_spawn` and never joined. Priorities are the
platform's seven levels; "normal" when nothing sets one.

#### Process, kernel and system

| Thread (name) | Entry | Started by, when | How many | Ends |
|---|---|---|---|---|
| Process main thread | `main.c` `main` → `boot.c` `rt_boot` | OS | 1 | Blocks in `plat_thread_join` on guest main; `rt_fatal` if guest main returns |
| "guest main" | `kernel.c` `guest_main` → `kern_main_thread` → `boot.c` `guest_main` (runs `nn::init::Start`) | `kernel.c` `kern_run_on_guest_main` (~1469) from `rt_boot` after the prewarm wait | 1, 256 MiB stack reserved | Joined by the main thread |
| "guest" | `kernel.c` `thread_main` (~1383): waits for `StartThread`, takes a core, `rt_call(entry)` | SVC `CreateThread` (0x08, `kern_svc_inner` ~1562) | One per guest thread; detached; 256 MiB reserved stack each | `ExitThread` (0x0A) → `thread_finish`; a thread created and never started waits forever (lead L-12) |
| (guest fibers) | `kernel.c` `fiber_main` | `kern_fiber_initialize` (~2206) | Up to `FIBER_MAX` 1024 | Not threads: they run on whichever guest thread switches to them |
| "prewarm" | `boot.c` `prewarm_main` (~348): `vk_pipeline_prewarm`, `gfx_shader_prepare_recorded`, `gfx_shader_carry_start` | `rt_boot` (~407) before the modules load | 1 | Joined, or detached by `boot_wait` after `NX2NAVI_PREWARM_WAIT_MS` or 60 s without progress; then runs on during play |
| "log" | `logq.c` `log_main` (~114) | `main.c` `log_open` → `logq_async` (~125), game runs, unless `NX2NAVI_LOG_ASYNC=0` | 1, detached | Never ends; `logq_sync` turns the queue off |
| "save commit" | `fs_host.c` `commit_main` → `commit_run` → `save_commit_locked` | `fs_host.c` `save_commit` (~601) on each `nn::fs::Commit` with a dirty save | One per Commit, detached; inline when the spawn fails | After the commit |
| PGO flush | `platform/win32.c` `pgo_flush_thread` | Constructor, only with `NX2NAVI_PGO_GEN` (instrumented builds) | 1 | Never |
| Open folder | `platform/win32.c` `open_folder_main` | `plat_open_folder` (menu) | Per call | After `ShellExecuteW` |
| Exception handlers | `platform/win32.c` `fault_veh` (first), `crash_veh` (last), `crash_filter` | Registered by `plat_mem_fault_hook` (from `memory.c` `mem_init`) and `plat_crash_hook` (from `main.c`) | — | Run on the faulting thread; `crash_veh` → `main.c` `crash_handler` → `plat_exit(71)` |
| "reap" (Linux only) | `platform/linux.c` `reap_main` | Child process start | Per child | When the child exits |

#### Graphics: replay, submission, present

| Thread | Entry | Started by, when | How many, priority | Ends |
|---|---|---|---|---|
| Game submit thread(s) | `nvn_api_core.c` `nvnQueueSubmitCommands` → `vk_backend.c` `submit_sync` → `vk_sync.c` `submit` → `vk_exec.c` `vk_exec_record` | The game | Guest threads | — |
| "The recording thread" | Whichever guest thread first spends 0.5 s in `vk_exec_record` | `vk_exec.c` `rec_claim` (~1643) | Raised to highest priority unless `NX2NAVI_RECORDER_PRIO=0`. Gets a reserved core (`plat_cpu_reserve`) with `NX2NAVI_RECORDER_CORE=1`, or `auto` with 12 or more usable CPUs | — |
| Game present thread | `nvn_api_obj.c` `nvnQueuePresentTexture` → `vk_backend.c` `vk_be_present` | The game | Guest thread | — |
| "vk present" | `vk_backend.c` `present_main` (~75) | `present_image` (~252) at the first present | 1, detached, normal | Never |
| "vk replay" | `vk_backend.c` `replay_main` (~406) | `replay_enqueue` (~472), only with `NX2NAVI_REPLAY_ASYNC=1` (`cfg.c` default 0) | 1, detached; becomes the recording thread | Never |
| "vk recorder" | `vk_rec.c` `rec_main` (~61) | `rec_begin` (~179) at the first submission part | 1. On unless AMD (`NX2NAVI_VK_RECORDER`, `rt_knob` default "0" on AMD, else "1"). Takes the priority of whichever thread called `rec_begin` first | Never (handle kept, never joined) |
| "vk completion" | `vk_sync.c` `completion_main` (~382) | `vk_sync_start` (~471) | 1, normal | Joined in `vk_sync_shutdown` |
| "vsync" | `gfx_present.c` `vsync_main` (~175) | `vsync_ensure` (~210) | 1, detached, highest | Never |
| "fg present" | `gfx_present.c` `ng_thread` (~1098) | `ng_ready` (~1180), only when `vk_ngxfg_wanted()`; the comment at ~925 says 0 on Windows | 1, detached | Never |
| "vk headroom" | `vk_headroom.c` `headroom_main` (~157) | `vk_headroom_start` from `vk_device.c` `vk_init`; off with `NX2NAVI_VRAM_HEADROOM=0` | 1, detached, below normal | After `vk_headroom_stop` |
| "mem census" | `vk_memory.c` `census_main` (~3138) | `heap_log`, only with `NX2NAVI_MEM_CENSUS=1` | At most one at a time | After one walk |

#### Pipelines

| Thread | Entry | Started by, when | How many, priority | Ends |
|---|---|---|---|---|
| "pipe worker" | `vk_pipeline.c` `pipe_worker` (~3533) | `workers_start` (~3623), first use | `plat_cpu_usable()/4` clamped 2..`MAX_WORKERS` (4). Below normal for prebuild jobs, normal otherwise | Never |
| "pipe lib worker" | `vk_pipeline.c` `lib_worker` (~3500) | `workers_start` (~3638), with GPL and `NX2NAVI_PIPE_WAIT` not `draw` | Pipe-worker count or 2 | Never |
| "pipe prewarm" | `vk_pipeline.c` `warm_builder` (~4015) | `prewarm_run` (~4186) on the prewarm thread | min(cpus, 64) | Joined; ends at `PREWARM_PIPE_MS` (20 s) unless `NX2NAVI_PREWARM_FULL=1` or the whole warm after a driver change (`bf_decide`) |
| "pipe list" | `warm_builder` (~5942) | `seed_pipelines`, preparation modes only | min(cpus, 64) | Joined |
| "big compute" | `vk_pipeline.c` `bg_main` (~5140) | `bg_start` (~5252) by the last `lb_worker`; `NX2NAVI_LIB_PREBUILD_BIG_BG` (default 1) | 1, below normal; raised to normal when a dispatch waits for it | Joined in `lb_stop` |
| "lib prebuild" | `vk_pipeline.c` `lb_worker` (~5418) | `lb_start` (~5707) from the boot prewarm; `NX2NAVI_LIB_PREBUILD` | `NX2NAVI_LIB_PREBUILD_THREADS` or 2 (max 16). Idle priority, normal around `P.lock` and shader loads. In the whole warm after a driver change (`bf_wait`): one per core (max 16) at normal priority while the start waits | When the list is done |
| "lib warm check" | `vk_pipeline.c` `lw_main` (~5572) | `lw_start` (~5649); `NX2NAVI_LIB_WARM` (default 1) | 1, below normal | Joined in `lw_stop` |
| "lib warm" | `vk_pipeline.c` `lw_worker` (~5532) | `lw_main` when the driver cache is cold | cpus/4 clamped 2..6, idle. In the whole warm after a driver change (`bf_wait`): one per core (max 32) at normal priority while the start waits | Joined in `lw_stop` |
| "lib warm" | `vk_pipeline.c` `lb_warm_worker` (~5733) | `lb_warm_prepared` (~5794), preparation modes only | min(cpus, 64) | Joined |
| "pipe autosave" | `vk_pipeline.c` `autosave_main` (~6390) | `vk_pipeline_autosave` (~6402) from the present thread, once | 1, detached, below normal, 500 ms loop | Never; not stopped by `vk_pipeline_shutdown` (lead L-15) |

#### Shaders

| Thread | Entry | Started by, when | How many, priority | Ends |
|---|---|---|---|---|
| "xlate" | `gfx_shader.c` `xlate_worker` (~2070) | `start_workers` (~2127), first async job | `NX2NAVI_XLATE_PROCS` (default 2, 1..32) | Never |
| "shload" | `gfx_shader.c` `load_worker` (~2093) | Same | `LOADERS` = 3 | Never |
| "use preload" | `gfx_shader.c` `use_preload` (~1304) | `use_preload_start` (~1335) | 1, below normal | After the scan |
| "shader ids" | `gfx_shader.c` `ids_write_main` (~506) | `ids_note` (~515), once | 1 | After one write |
| "shader prewarm" | `gfx_shader.c` `warm_worker` (~2314) | `gfx_shader_prewarm` (~2399) from `prewarm_run` | cpus, 1..64. Translator children raised to `NX2NAVI_PREWARM_XLATE` or min(cpus/2, 16) meanwhile | Joined |
| "shader prepare" | `gfx_shader.c` `prep_worker` (~2732) | `prepare_files` (~2787), preparation modes | 1..64 | Joined |
| "shader carry" | `gfx_shader.c` `carry_main` (~3194) | `gfx_shader_carry_start` (~3305) from `prewarm_main`; `NX2NAVI_SHADER_CARRY` (default 1) | 1, lowest; own translator child at idle priority | After one pass |
| Translator processes | `nx2navi_xlate.exe --serve` | `gfx_shader.c` `child_start` (~533) via `plat_child_start` | Pool `S.s[MAX_SVC=32]`, limit `NX2NAVI_XLATE_PROCS`, raised during prewarm | Kept for the life of the process |
| "astc" | `tex_astc_mt.c` `worker` (~58) | `pool_init` via `plat_once` at the first large CPU ASTC decode | Up to 7 | Never |

#### Audio and movies

| Thread | Entry | Started by, when | How many, priority | Ends |
|---|---|---|---|---|
| "audren" | `audren.c` `render_thread` (~517) | `audren_manager_handler` cmd 0 (~682) on the game's IPC thread | One per open renderer, highest | Joined in `renderer_close` |
| SDL audio output | `audio_mixer.c` `sdl_pull` (~320) | SDL's own device thread, `SDL_OpenAudioDeviceStream` in `stream_open` (~344) | 1 per stream | When the stream is destroyed |
| SDL audio capture | `audio_capture.c` `received` (~26) | SDL's own thread, `mic_open` (~98) | 1 per mic | `mic_close` |
| "movie" | `ov_movie.c` `worker` (~238) | `ov_ctor` (~454) on a game thread | One per VP9 decoder (`MAX_MOVIES` 16), plus libvpx's own threads (`VPX_THREADS` 4) | Joined in `destroy` |
| "movie decoder" | `ov_movie2.c` `decoder_thread` (~483) | `ov_initialize` (~601) | One per decoder, plus dav1d's own threads (8) | Joined in `stop_thread` |
| "movie convert" | `ov_movie2.c` `convert_thread` (~443) | `ov_initialize` (~603) | One per decoder | Joined in `stop_thread` |
| "movie convert" (stripes) | `ov_movie2.c` `stripe_run` (~269) | `run_stripes` (~287), per converted picture | 5 per call, created and joined each frame (lead L-34) | Joined |

#### Input, window and menu

| Thread | Entry | Started by, when | How many | Ends |
|---|---|---|---|---|
| "pads" | `input.c` `up_main` (~416) | `sdl_start` (~458) at the first poll | 1, detached | Never (unless SDL init fails) |
| "motion" | `input.c` `motion_main` (~1159) | `input_motion_start` (~1186) from `ov_input.c` `ov_six_start` | 1, detached, 250 Hz loop | Never, also after the sensor stops |
| "ds3-scan" | `input_ds3_win.c` `scan_thread` (~122) | `ds3_read` (~276) with `NX2NAVI_DS3` | 1, detached, 256 KiB stack | Never |
| "window" | `gfx_window_win.c` `window_main` (~643): the Win32 message loop | `wsys_open` (~729) | 1, detached | When `GetMessage` returns <= 0 |
| "folder dialog" | `folder_pick_win.cpp` `pick_main` (~51) | `pick` (~90) from the menu | Per pick | After the dialog |
| "save export" | `ui/menu.cpp` `share_export_main` (~1116) | `rows_saves` (~1176) | One at a time (`g_share_busy`) | After the export |
| "menu-art" | `ui/menu_art.cpp` `decode_all` (~48) | `menu_art_start` (~66), once | 1 | After decoding |
| "camera speed" | `camera_speed.c` `search_main` (~262) | `cam_speed_tick` (~304) on the present path | One at a time (`searching`) | After one search |

#### Monitoring, profiling and other

| Thread | Entry | Started by, when | Ends |
|---|---|---|---|
| "discord" | `discord_rpc.c` `discord_main` (~193) | `discord_init` from `main.c` | Never ("there is no shutdown path") |
| "rtss" | `rtss_limit.c` `worker` (~245) | `start` (~319) at the first `rtss_limit_want` | Never |
| "perfmon" | `perfmon.c` `sampler` (~360) | `perfmon_mode(2)` (~404) from the menu's overlay | When the full overlay is turned off |
| Profiler sampler | `prof_win.c` `sampler_main` (~242), time-critical, suspends every other thread each 1 ms | `prof_sampler_start` (~386), only with `NX2NAVI_PROFILE` | When `prof_sampler_write` stops it (waits 2 s) |
| "flight dump" | `flight.c` `dump_main` (~231), below normal | `start_dump` (~311), with `NX2NAVI_FLIGHT` | After one dump |

#### Threads inside libraries

Not enumerated:

- the GPU driver;
- NVIDIA Streamline and NGX;
- AMD FidelityFX (its frame-generation presenter; `vk_device.c` makes two more graphics-family queues for it,
  `vk_fg_queues`);
- SDL's internal threads;
- libvpx and dav1d workers.

### 3.3 Shared state and what protects it

Each line names a piece of shared state, the threads that touch it, and the rule. "Protection not found" marks state
touched from more than one thread where no lock or atomic rule was found. That is a lead for Phase 3 pass 2, not a
finding.

#### Kernel (`kernel.c`)

- **`G` (`PlatMutex`, ~114).** Header comment: "every wait hangs on one global lock G with one condition variable per
  thread". It guards:
  - the handle table `g_h[MAX_H]`, `g_all`, `g_wait`, `g_core_owner[]`, `g_core_wait`;
  - every `RtThread` wait and core field, event `signaled` and `refs`;
  - `g_next_id`.

  Threads: every guest thread at SVCs and waits, plus the waiter that suspends a holder (`ap_try`, `dg_sample`).
- **Lock-free reads, documented.**
  - `g_core_wait` in `rt_spin_gate` and the SleepThread yield ("a racy look").
  - `t->preempt` at checkpoints.
  - `host_depth`.
  - TLR+0x100 and TLR+0x102 ("any thread may read them").
- **Guest per-core data.** Kept without locks by the game itself, made safe by the virtual-core rule. A guest thread
  runs guest code only while it holds a core, and gives it up only where Horizon would switch.
  `NX2NAVI_FREE_CORES=1` turns the gate off.
- **`g_se_lock`** (rwlock, ~2062): the `g_se[512]` system-event table.
- **`g_fiber_lock`** (rwlock, ~2133): `g_fibers[]`. The `Fiber` fields written by `kern_fiber_initialize` outside the
  lock: protection not found (guest threads re-initialising and switching to a fiber).
- **Lazy one-time init outside `G`:** protection not found (guest main and the first guest threads).
  - `cores_enabled` (~341), with `diag_init` and `ap_init`.
  - `g_spin_on`, and `lim` in `kern_svc`.
  - `mode` in `wake_sync_waiters`.

#### Guest memory (`memory.c`)

- **`lock`** (rwlock, ~50): heap size, alias bookkeeping, `stack_maps`, `module_perm`, `map_gen`.
  - `mem_read_hold` keeps it shared over long reads.
  - `stack_move` (~997) reads `heap_size` before taking `lock`: protection not found (guest threads mapping against
    `SetHeapSize`).
- **`frz_lock`** (~75). It guards:
  - the freeze/thaw times;
  - the lazy write-back segments `lzs`;
  - the hot list and probe hits;
  - every `lz_*` counter.

  Taken by any faulting thread inside the exception handler (`thaw_fault`, `lazy_fault`, `probe_fault`). The comment
  calls it a leaf lock, but the back end's `lz_fetch` (`vk_memory.c` `lazy_fetch`: a GPU copy and a wait of up to
  10 s) and `lz_clean` run under it (section 6, D-05).
- **Atomic flags read without the lock:** `frz[]`, `prb[]`, `lzr[]`, `nfrz`, `nprb`, `frz_count`, and `lz_n` in
  `mem_lazy_note` (documented).
- **`klock`** (~1284): the kernel-region allocator's free lists and bump pointer.
- **Write watch.** `plat_mem_written` (`GetWriteWatch` with reset) is called from `vk_memory.c` (~748, ~2606, ~2882)
  under `vk_memory.c`'s `gen_lock` on the replay side. `memory.c` itself takes no lock for it.
- **Lock orders:** `lock` → `frz_lock` (commit, decommit, protect); `klock` → `frz_lock`.

#### Save data (`fs_host.c`)

- **`tx_lock`** (~220): "taken before lock, never inside it". It guards:
  - the save transaction (`ready_root`, `ready_seq`, the working copy, commit and swap steps);
  - the backup rotation;
  - `fs_save_export`.

  The commit thread takes `tx_lock` alone.
- **`lock`** (rwlock, ~48): slot assignment and release in `mounts[]`, `files[]`, `dirs[]`.
- **`pend_lock` and `pend_cv`** (~562): `commit_pending`, `exiting`. `save_writers`, `save_dirty`, `save_changed_ns`
  and `storage_ready` are atomics.
- **Protection not found:**
  - the `files[].h` handle used after `file_of` (~1068) drops the shared lock while `close_file` may close it (guest
    threads);
  - `dirs[]` contents read in `read_directory` (~1307) while `close_directory` frees them;
  - `remove_failed` and `remove_error` (commit thread and guest threads).

#### Logging and settings

- **`logq.c`.** `Q.q` guards the buffer; `Q.io` guards writes and the output, and is taken first.
  - `logq_sync` try-locks each for up to 500 ms, then writes without them (documented).
  - The log file is appended with whole writes (`plat_append_write`).
- **`cfg.c`.** `set_lock` (~28) covers `cfg_set`, `cfg_reset` and `cfg_save_to`. Readers take no lock: "an entry is
  complete before n_entries counts it, and value pointers are swapped atomically".
  - `cfg_memo` is per thread.
  - `getenv`-based reads against `cfg_setenv` → `_putenv_s` while the game runs (`gfx_present.c` ~90): protection
    not found.

#### Files, services and the rest of the CPU side

**RomFS, decompression, IPC**
- `romfs.c`:
  - `lock` (rwlock, ~83) guards the handle tables.
  - Protection not found: `mount_names` read by `romfs_owns` while `romfs_mount` writes it; a handle used after
    `handle_source` unlocks while `romfs_close` closes it.
- `ov_zstd.c`:
  - `dict_lock` guards the dictionaries ("never freed: another thread may be decoding with one").
  - `stream_lock` covers slot claim and drop only; a slot's stream fields belong to the thread that holds it.
  - A "spent" slot can be handed out while the old thread still reads it: protection not found (lead L-11).
- `ipc.c`:
  - `g_applet_lock` guards the applet message ring.
  - Protection not found: `IpcSession` domain fields changed during dispatch by any thread using the session; the
    session pointer is used without a reference while `CloseHandle` may free it.

**System services**
- `ov_system.c`:
  - `lock` and `message_cv` guard the message queue.
  - `account_lock` guards account state.
  - `fonts.lock` guards the shared fonts.
- `net_offline.c`: `lock`, then `G` through `kern_event_clear`.
- `amiibo.c`: `g_amiibo_lock`.
- `swkbd.c`: `one` serialises keyboards.
- `loader.c`: `plt_lock`.
- `guest_setjmp.c`: `jt_lock`.

**Written once at start, then read-only**
- `dispatch.c`: the dispatch table is sealed read-only.
- `hostfn.c`: the registry is fixed at seal ("guest threads start after that").

**Patches, mods, cheats**
- `patches.c`: `rt_patch_on[]` bytes are written atomically by `rt_patch_switch`.
- `mods.c`: `mods[]` is changed by `mods_move` and `mods_set_enabled` (menu) while romfs, cheats and boot read it.
  Protection not found.

#### NVN front end

- **`nvn_api_cmd.c`**
  - `copy_lock` (~417): "guards the two maps, the counters and every prefix", and `zombies[]`. Order: `copy_lock`,
    then `pin_lock`.
  - `pin_lock` (~257): pins and the pin queue.
  - `pool_lock` (~86): the recording-buffer pool.
  - `cb_lock` (~41).
  - `nvn_submit_prepare_async` writes `prefix->deferred` after unlocking (~1294): protection not found.
- **`nvn_api_core.c`**
  - `pool_lock` (~75): "guards gva and index".
  - `submit_lock` (~638): only with the asynchronous replay.
  - With the default synchronous replay there is no front-end submit lock. `q->submits++` and
    `nvn_submits_in_frame++` (~634, ~676) are plain increments, and `nvnQueuePresentTexture` resets
    `nvn_submits_in_frame` (`nvn_api_obj.c` ~868). Protection not found if the game submits and presents from
    different threads.
- **`nvn_api_cmd.c` `nvn_submit_prepare` → `resolve_deferred`** rewrites op records in place, unlocked. Another
  thread may be replaying the same handle under `sub_lock`. Protection not found (sync mode).
- **`nvn_api_obj.c`**
  - `live_lock` guards the live-texture copies.
  - `img_fence[]` and `last_present_fence` are written at present and read at acquire. Protection not found if those
    come from different threads.

#### Replay, submission and the queue

- **`vk_sync.c` `sub_lock`** (~31): "Submissions: one at a time, so timeline values are signalled in the order they
  are handed out".
  - It is held across all of `submit()`: the whole `vk_exec_record` replay, `vk_upload_flush`, the prologue and
    `queue_submit`.
  - Through that it guards vk_exec's `X` state, the recorder's producer side (`vk_rec.h`: every producer holds it),
    and vk_memory's per-submission lists ("submit lock").
  - Taken by: game submit threads (sync), the replay thread (async), the present thread (its empty submission) and
    memory probes.
- **`st_lock`, `cv_work`, `cv_done`** (~39): `issued`, `done`, `reserved`, `stop` and the completion queue. Shared
  with the completion thread, which runs callbacks and `vk_mem_collect` outside the lock.
- **`start_lock`** (~26): device start-up and the completion thread handle.
- **The queue lock `g_ql`** (`gfx_present.c` ~42, recursive).
  - `vk.h` ~213: "Every submission to ctx->gfx_queue (and xfer_queue when shared) takes this lock".
  - `vk_queue_lock` also flushes held submissions; the `_raw` forms do not.
  - Submit batching `H` is guarded by it (`vk_sync.c` ~58).
  - Callers of `vkQueueSubmit2`:
    - `vk_sync.c` `hold_flush_locked`: from `queue_submit`, `hold_release` (fence waiters; the completion thread only
      try-locks), and `vk_queue_lock`;
    - `gfx_present.c` `present_image` (~1542);
    - `vk_memory.c` `vk_upload_flush` (~3750) and `lazy_fetch` (~2582);
    - `vk_fg.cpp` `submit_game`.
  - Callers of `vkQueuePresentKHR`: `gfx_present.c` `flip_locked` (~313) and `ng_present` (~991), through Streamline
    in `vk_dlssg_present`.
  - `vkDeviceWaitIdle` without `g_ql`: `vk_pipeline.c` `vk_pipeline_shutdown` (~6412) and `vk_device.c` `destroy_all`
    (~548); see D-11.
- **`vk_backend.c`**
  - `pl` and `pcv` (~41): the present job (`job`, `pending`, `busy`). Taken by the present thread and every
    `vk_present_drain` caller.
  - `rl` and `rcv` (~279): the asynchronous replay ring (`RQ_CAP` 1024).
  - `PH` (present held, ~338) and `FH` (frame hold, ~145): protection not found. Touched by the game present thread
    and the submit thread(s) in sync mode, and `FH` by the recording thread (`vk_frame_hold_left`).
- **`vk_exec.c`**
  - `bcs_lock` (~172): the BC stand-ins ("the replay adds and reads; destroy_tex (any thread) removes").
  - `ET.lock` (~1061): the event-order trace.
  - Called outside `sub_lock` from `vk_backend.c` (~220-232, ~360, ~549): `vk_exec_held()`, `vk_exec_held_ready()`,
    `vk_exec_frame_log`, `vk_exec_pending_draws_take`, `vk_exec_gate_wait`. Protection not found when the presenting
    and the recording thread differ.
- **`vk_exec_draw.c`**
  - `spare_lock` (~134): spare descriptor pools and chunks; the replay takes them, the completion thread returns them.
  - `pool_lock_` (~1951): the bindless pool layout's lazy creation.
  - The texture lock is held throughout the bindless pool sync.
- **`vk_exec_xfer.c`** `cpu_ev_lock` (~458): the CPU event signals. Guest threads (`nvnEventSignal`) against the
  replay.
- **`vk_usnap.c`**
  - `free_lock`: the free arenas.
  - `gather_lock`: the collect buffers, shared by the submitting thread and the replay thread.
  - `tq_lock`: the taken-snapshot queue.
  - The lazy `U.mode` init: protection not found (benign).
- **`vk_rec.c`.** A lock-free single-producer ring.
  - `head` and `tail` are atomics; `wake_ev` is an auto-reset event.
  - The producer is any thread holding `sub_lock`.
  - `OWNED(cb)` reads `rec_live` and `rec_cb` without atomics from other threads that record into other command
    buffers: protection not found (benign by value, per the mapping pass).

#### GPU memory (`vk_memory.c`)

**Documented order:** `pool_lock` → `shadow_lock` → `gen_lock`.

| Lock | Guards | Taken by, and order |
|---|---|---|
| `pool_lock` (~105) | Pool index, physical list, page tables, `mlist` (mirrored pools) | Shared on lookups, exclusive on init, finalize, map and mirror changes |
| `gen_lock` (~729) | Page-generation tables, `snap_*`, `mgen`/`sgen` | The replay, guest threads (`vk_guest_written`), the completion thread (`vk_gpu_written`), textures (`vk_mem_gen_lock`) |
| `shadow_lock` (~1035) | Shadow storage | After `pool_lock`, before `gen_lock` |
| `flush_lock` (~1609) | Flush ranges per pool | Order `pool_lock` → `flush_lock` |
| `dirty_lock` (~983) | Perf census | — |
| `mirror_stat_lock` (~1622) | Mirror summary | — |
| `retire_lock` (~1982) | `retiring[]` | Finalizing guest thread vs the completion thread |
| `snap_pool_lock` (~2221) | Write-back snapshot chunks | — |
| `lz_init_lock` (~2553) | One-time lazy write-back setup | — |
| `cmd_lock` (~3460, recursive) | The upload batch, deferred destruction `dead[]`, the staging cache and arena | Held from `vk_upload_begin` to `vk_upload_end`; order `cmd_lock` → `g_ql` |

Protection not found:
- `PoolRec.mstate`: set to `MS_ON` after `pool_lock` is released in `mirror_create` (~1949), read by `vk_pool_fini`
  (guest) and `vk_gpu_written_back` (completion thread). See L-07.
- `PoolRec.st_*`: "the log reads and clears them without a lock" (acknowledged).

#### Textures (`vk_texture.c`)

- **`tex_lock`** (~119, recursive). It guards:
  - the texture cache (`by_key`, `by_addr`);
  - the texture and sampler slots, the sampler cache;
  - the released lists;
  - the eviction state.

  Taken by the replay, guest threads (register, release, ReadTexels/WriteTexels, `guest_read`, `range_gone` from pool
  finalize) and the present path (`vk_texture_get`).
  - Orders: `tex_lock` → `cmd_lock` (`init_image(e, vk_upload_begin())`); `tex_lock` → `gen_lock`;
    `tex_lock` → `pool_lock` (shared).
- **`owned_lock`** (~165): GPU-written mirrored textures. "Changed under tex_lock and owned_lock; read under either".

#### Pipelines and shaders

**`vk_pipeline.c`**

- **`P.lock`** (~71). It guards:
  - the pipeline, layout and library hash tables;
  - the job queues;
  - the prebuild bookkeeping and statistics.

  `work_cv`, `done_cv`, `lib_cv` and `A.idle` go with it. Builds run with `P.lock` released.
- **`cache_rw`** (~520). Creates against `P.cache` hold it shared; `cache_save` takes it exclusive, by trylock only.
- **Other locks:**

  | Lock | Guards |
  |---|---|
  | `save_lock` | One cache save at a time; order `P.lock` → `save_lock` at shutdown |
  | `cu_io` | `compute-used.bin` |
  | `PB.lock` | Pipeline binaries |
  | `K.io` | `pipeline-keys.bin` |
  | `VO.lock`, `FB.lock`, `CU.lock`, `LB.stat` | Small tables and statistics |
- **Not protected:** `BG.building` is written by "big compute" without `P.lock` and read under it (benign per the
  mapping pass).

**`gfx_shader.c`**

| Lock | Guards |
|---|---|
| `cache_lock` (~135) | The memory cache |
| `S.lock` | Translator children |
| `F.lock` | "one translation per id at a time" |
| `ids_lock` | `identities.txt` |
| `failed_lock` | Failed keys |
| `X.lock` with `work`/`done`/`lwork` | Job queues |
| `load_lock` | The disk-read budget |
| `RC.lock`, `rec_io` | Record files; order `rec_io` → `RC.lock` |

- `use_files` is filled by "use preload" and published by an atomic flag.
- `gfx_shader_drop_memory` (probe only) frees it while it may still be filling: protection not found.
- The code drops threads to idle or lowest priority, and raises them to normal around `cache_lock`. The comment says
  why: "use_store takes the cache lock the render thread reads under".

#### Present, window and menu

- **`gfx_present.c` `g_pl`** (~125, recursive): the swapchain and present state. Order `g_pl` → `g_ql`.
  - Held across the 5 s fence wait, the 1 s acquire and `create_swapchain`.
  - Taken by the present thread, the boot thread (`gfx_present_boot`) and the window thread (`gfx_window_resize`).
- **`g_start_lock`** (recursive): window and vsync start. Order `g_start_lock` → `g_pl`.
- **`g_vs_lock`**: vsync events; exclusive in `gfx_vsync_bind`, shared in the vsync thread.
- **`g_mv_lock`**: the movie frame (decoder threads write, the present thread reads).
- **`ng.m` and `ng.cv`**: the NGX presenter hand-off. `ng.interval`, `ng.tried` and `ng.n`: protection not found
  (non-Windows only).
- **Menu (`ui/menu.cpp`).**
  - "ImGui runs on the present path only, under gfx_present.c's present lock; the event functions only queue."
  - `open_lock` → `ev_lock`, and `open_lock` → `cur_lock` (`input_win.c`) through `input_menu`.
  - `g_share_lock`; `g_pick_lock` (`amiibo_page.cpp`).
- **`menu_assets.c`** `dicts` and `ndicts`: lazily loaded without a lock from the "menu-art" thread and the present
  thread. Protection not found.
- **`gfx_window_win.c`.** The window thread owns its state. Settings arrive by `PostMessage`; `wsys_resize` and
  `wsys_probe_*` call into it synchronously.

#### Audio, movies and input

**Audio**

- **`audren.h` `AudioRenderer.lock`.** It guards voices, mixes, effects, sinks, state and mixer render state, between
  the game's IPC thread (`request_update`) and the render thread.
  - Unlocked: `GetState`, `time_limit`, and `g_game_prio`. Protection not found (benign ints).
- **`audio_mixer.c`.** The output ring is single-producer single-consumer and lock-free ("the SDL audio thread drains
  that ring without taking any lock"): `wr`/`rd` atomics.
  - The stream fields are changed only after the stream is destroyed.
  - `change_events[16]`: the slot index is reserved before the slot is stored. Protection not found for the slot
    contents.
  - `host_volume` and `sink_dump` statics: protection not found if two renderers are open.
- **`menu_sound.c`.** Per-voice atomics. `started` is plain while `menu_sound_play` is called from the window thread,
  the polling thread and the present path: protection not found.
- **`audio_capture.c`.** Relies on the SDL stream lock.

**Movies**

- **`ov_movie.c`.** `g_lock` → `Movie.mu`.
  - `find()` releases `g_lock` before the movie is used while `destroy` may free it: protection not found (L-08).
- **`ov_movie2.c`.** `lock` ("queues, frames, flags") and `codec` (dav1d use).
  - `ov_decode` reads `v->dav1d` without either while `finalize` closes it: protection not found.
  - Static timing totals: protection not found (benign).

**Input**

- **`input.c`.**
  - `lock` guards the pads, state, script, binds and DualShock 3 state.
  - `up_lock` with `up_go` and `up_done` guards the update hand-off to the pad thread.
  - `sdl_lock` is held around SDL gamepad calls; others only try-lock it.
  - Order: `lock` → `up_lock` → `sdl_lock` (trylock).
  - `mouse_sens_get` statics: protection not found.
- **`ov_input.c`.** `lock`; `vib_lock` → `input.c` `lock`. The `rumble_cfg` statics written outside `vib_lock`:
  protection not found.
- **`input_win.c`.** Mouse deltas are interlocked; `cur_lock` serialises cursor updates.

**Frame rate and resolution**

- **`frame_rate.c`.**
  - The step words on the patch page are atomics: "one 64-bit and one 32-bit store per present; the guest reads the
    floats on its own threads".
  - `limit_hz`, `limit_seen`, `limit_read` and `fps60_set` in `frame_limit_live` are reached from three threads: the
    presenting guest thread, the menu's present path (`frame_step_limit_hz`), and the mouse camera thread
    (`frame_step_game_hz`). Protection not found.
- **`resolution.c`.** `renderer` (written by `res_fn_hit` on any guest thread) and `R.sw`/`R.sh` (DLAA vs the menu):
  protection not found.
- **`camera_speed.c`.** `applied`, written by the search thread and read by the poll thread: protection not found.

### 3.4 Lock order (observed)

These orders were observed in the code; only a few are documented in comments.

- `sub_lock` → `g_ql`, `cmd_lock`, `st_lock`, `tex_lock`, `pool_lock`, `gen_lock`.
- `tex_lock` → `cmd_lock`, `gen_lock`, `pool_lock`.
- `cmd_lock` → `g_ql`.
- `g_start_lock` → `g_pl` → `g_ql`.
- `rl` → `st_lock`.
- Async `submit_lock` → `copy_lock`, `gather_lock`, `tq_lock`, `rl`.
- `start_lock` → `st_lock`, `sub_lock`, astc `g_lock`.
- `P.lock` → `save_lock`; `rec_io` → `RC.lock`.
- `memory.c` `lock` → `frz_lock`; `klock` → `frz_lock`.
- `fs_host.c` `tx_lock` → `lock`.
- `ov_movie.c` `g_lock` → `Movie.mu`; `ov_media.c` registry → decoder.
- `input.c` `lock` → `up_lock` → `sdl_lock` (trylock).
- `ov_input.c` `vib_lock` → `input.c` `lock`.
- `menu.cpp` `open_lock` → `ev_lock`, `cur_lock`.
- Kernel `G` is innermost against the other modules.

No inversion was found in what was read. Not checked:
- `frz_lock` against the GPU-side locks `lazy_fetch` takes (`g_ql`);
- any lock held by a thread that faults into `fault_veh` (L-05).

---

## 4. The path of one frame

This is the default configuration: replay on the submitting thread, recorder thread on (not on AMD), a discrete GPU
with VRAM mirrors, no frame generation. Steps are in order, function by function.

### 4.1 The game's CPU frame (guest threads)

1. **Acquire.** `nvn_api_obj.c` `nvnWindowAcquireTexture` (~813).
   1. Gives the core away (`NVN_BLOCKING`).
   2. Picks the next image index.
   3. `sleep_until(o->avail[idx])`: the image's previous present slot plus one period. This is back-edge pacing at
      the frame limit, using `plat_wait_ns` and then `plat_yield`.
   4. Waits for the GPU to finish that image's newest present (`fence_wait`, `NX2NAVI_ACQUIRE_WAIT` default 1; 2
      waits for the newest present of any image).
   5. `frame_start`.
2. **Game logic.** The game's update runs on its own threads, stepped by the value `frame_rate.c` wrote at the last
   present (section 4.4). Audio, movie and input services answer on the threads that call them.
3. **Recording.** The game records command buffers through `nvn_api_cmd.c`: the `nvnCommandBuffer*` entry points.
   - Each call appends an op record (`op_hdr` plus payload) to recording memory.
   - `CallCommands` is inlined at record time (`inline_recording`).
   - Deferred binds are noted, to be resolved at submit.
   - Nothing reaches Vulkan yet.
4. **Submit.** `nvn_api_core.c` `nvnQueueSubmitCommands` (~627).
   1. `NVN_BLOCKING`; `q->submits++`.
   2. `nvn_submit_prepare(h, n)` resolves deferred binds in place.
   3. `gfx_backend()->submit` → `vk_backend.c` `vk_be_submit` → `submit_sync` (~383).

### 4.2 Replay into Vulkan (on the submitting thread)

5. **`submit_sync`.**
   1. If a present is waiting for held work, the held work goes first (`held_resume`, `ph_flush`).
   2. `vk_present_drain` waits until the present thread has taken and submitted its job.
   3. `vk_exec_gate_wait` handles a submission that begins with a wait for a CPU signal.
   4. `vk_texture_new_submit` bumps the submit epoch. Every 8th epoch it also runs the texture VRAM pressure check
      `sweep_pressure` and the evictions.
   5. `vk_sync_submit` → `vk_sync.c` `submit` (~573).
6. **`submit`.**
   1. Takes `sub_lock`.
   2. `fence = issued + 1`.
   3. `take_slot` → `claim_slot` reuses or allocates a command buffer.
   4. `vkBeginCommandBuffer`, `ts_begin` (GPU timestamps with `NX2NAVI_TRACE=perf`).
   5. `rec_begin` hands the part to the recorder ring.
   6. `vk_exec_record`.
7. **`vk_exec_record`** (`vk_exec.c` ~1666).
   1. `nvn_pins_sweep`.
   2. `vk_exec_event_sync(1)`: a CPU signal after this point counts as late.
   3. `vk_mem_mirror_begin`: the pages the CPU wrote (write watch) and the ranges the game flushed are queued as
      copies into the VRAM mirrors.
   4. `vk_usnap_begin` copies every uniform range the submission binds from CPU-written pools.
   5. `evo_resume` lets held submissions whose signal came go on.
   6. For each command handle, `walk()`.
8. **`walk`** (~1532) dispatches each op:

   | Ops | Handler |
   |---|---|
   | `OP_SET_RT` | `set_rt` |
   | Clears | `clear_color`, `clear_ds` |
   | Viewport, scissor, stencil, bias, blend state | Stored in `ex`, marked dirty |
   | Draws, dispatches, binds | `vk_exec_draw.c` `vk_exec_op_draw` |
   | Copies, event signals, counters, conditional rendering | `vk_exec_xfer.c` `vk_exec_op_xfer` |
   | `OP_WAIT_EVENT` | `vk_exec_wait_event`. An unmet wait ends the pass, and the rest of the submission is held (`cont_push`) until the signal is replayed |
   | `OP_BARRIER` | `barrier_to_why` |
   | (long submissions) | `maybe_split` sends a part early: `vk_sync_split` → `split_part` → `queue_submit` |
9. **A draw** (`vk_exec_draw.c` `op_draw`, `op_draw_idx`, mesh and dispatch variants) → `prepare_draw_ex` (~3856).
   1. **Per-command-buffer resources and the pass.**
      1. `cb_res` provides transient chunks and descriptor pools for this command buffer.
      2. `vk_exec_pass_begin` begins dynamic rendering. Its targets are resolved through `resolve_rt` and the texture
         cache.
   2. **Shaders.**
      1. `shader_for` → `gfx_shader.c` `gfx_shader_get_async` searches, in order:
         1. the memory cache;
         2. the disk cache, read within `load_budget`: 1/8 of wall time, 2 ms at once;
         3. the "shload" or "xlate" workers.
      2. A draw whose shader is not ready:
         - reads the rest of the submission ahead (`vk_exec_ahead`), so later pipelines' stage libraries are queued;
         - is skipped when its targets are per-frame (`vk_exec_targets_recent`);
         - otherwise waits from the frame's wait pool (`frame_wait`, `pipe_frame_wait_ms`).
   3. **Pipeline.**
      1. `vk_pipeline.c` `vk_pipeline_graphics_async` looks up the pipeline hash. A miss is fast-linked from stage
         libraries and queued for an optimised build on the pipe workers.
      2. Waits come from `link_budget`, `frame_wait` (full-screen draws), or `last_wait` when the frame hold has no
         allowance left.
      3. A pending pipeline skips the draw (`draw_skipped`); the count feeds the frame hold.
   4. **Bind and record.**
      1. `bind_pipeline`.
      2. Dynamic state: `vk_pipeline_set_dynamic_delta`.
      3. `record_fixed`: viewport, scissor, depth bias, stencil.
      4. `bind_resources` (~3189), under the texture lock:
         - `pool_sync` → `pool_refill`: the bindless texture and sampler descriptor sets, at most `pool_create_budget`
           new entries per refresh and `pool_create_ms` per frame;
         - uniform and storage buffers resolved through `vk_mem_lookup` and the uniform snapshot;
         - push descriptors.
      5. `feedback_barrier`, when the draw reads a target of its own pass.
      6. `bind_vertex_buffers`.
      7. `vk_exec_oq_draw` (occlusion queries).
      8. `vkCmdDraw*`.

   Every `vkCmd*` goes through the `rc_*` wrappers (`vk_rec.h`), which copy the arguments into the ring. The
   "vk recorder" thread makes the real calls in order.
10. **End of the recording** (`vk_exec_record` ~1736).
    1. `vk_exec_pass_end`.
    2. `vk_exec_barrier`, `vk_exec_pool_marks`.
    3. If there are write-backs (`NX2NAVI_WB_SPLIT`), `vk_sync_split_exec` ends the part that signals the executed
       timeline `exec_tl`.
    4. `vk_mem_mirror_end` records the copies of what the GPU wrote into mirrors back to guest memory. With lazy
       write-back, it arms those pages instead.
    5. `vk_usnap_end`.
    6. Per-command-buffer views are handed to the completion queue.
11. **Back in `submit`.**
    1. `rec_end_cb`: the recorder ends the command buffer.
    2. `vk_upload_flush(0)`: the texture and staging uploads this submission asked for are submitted first.
    3. `prologue`:
       - `vk_mem_shadow_sync` (shadow mode);
       - `vk_mem_gpa_sync`;
       - `vk_mem_lazy_part_end`;
       - the mirror copies (`vk_mem_mirror_prologue`) in a command buffer of their own.
    4. `queue_submit` (~115) takes `g_ql` raw and holds the submission. Up to `NX2NAVI_SUBMIT_BATCH` (default 4) are
       held, for at most `HOLD_NS` (0.5 ms), then `hold_flush_locked` → `vkQueueSubmit2`.
    5. `vk_mem_note_submit`.
    6. `issued = fence`, broadcast to the completion thread.
    7. Releases `sub_lock`.
12. **Wait after the frame's first submission.** Back in `nvnQueueSubmitCommands`, the first submission of each frame
    waits until the GPU has executed that submission's own passes. The wait is `exec_wait` on `exec_tl`, governed by
    `NX2NAVI_SUBMIT_FINISH`, default mask 1. The comment explains why: the game rewrites data those passes read
    without a fence.

### 4.3 GPU completion ("vk completion")

13. **`completion_main`** (`vk_sync.c` ~382).
    1. Waits on the timeline semaphore. It uses 100 ms steps, or `HOLD_NS` while submissions are held, flushing aged
       held submissions with a trylock.
    2. Runs the `vk_completion_add` callbacks in FIFO order up to the reached value:
       - `cb_done`: chunks and descriptor pools go back to the spares;
       - GPU-write stamps (`note_gpu_write`);
       - `arena_done` for the uniform snapshot arenas;
       - write-back snapshot chunks;
       - counter reports (`write_counter`);
       - `mirror_retire`, `gpa_retire`.
    3. Sets `done` and wakes fence waiters.
    4. Calls `vk_mem_collect`: deferred destruction and retired upload batches.

### 4.4 Present

14. **The game presents.** `nvn_api_obj.c` `nvnQueuePresentTexture` (~858).
    1. `movie_tick` hands over movie frames that are due.
    2. `res_present`, `wide_present`.
    3. `NVN_BLOCKING`; `nvn_submits_in_frame = 0`.
    4. `present_period` → `frame_rate.c` `frame_step_cap_ns` gives the limit's period. While RivaTuner holds the
       limit it returns 0.
    5. `pace()` keeps the phase: early waits for the slot; late by less than a period presents now; later restarts.
    6. `gfx_backend()->present` → `vk_backend.c` `vk_be_present(t, crop, slot)`.
    7. `o->avail[idx] = slot + period`; the image's fence.
    8. `frame_step_present(slot, interval)`: `ocarina_step` writes the game's next step (patch set `ostep`) to the
       patch page or the game's time manager, and the world clock advances.
    9. `rt_frame_presented`.
15. **`vk_be_present`** (sync).
    1. Frame counter; `vk_dlssg_submitted`; `perf_present`.
    2. With `NX2NAVI_PRESENT_HELD` (default on in sync mode), a present that finds held work waits for it, up to
       `PH_MAX` (8) submissions.
    3. Otherwise `present_image` (~215):
       1. Frame-level work: `vk_exec_frame_log`, `dynres_frame`, `vk_dlaa_frame`, `vk_sampler_aniso_live`,
          `vk_mem_census`.
       2. `vk_texture_get(t)` gets the presented texture's image, possibly recording an upload.
       3. `frame_hold` checks for skipped draws, a resize or an area load. If it holds the frame, the previous
          picture stays and this present is dropped (`NX2NAVI_FRAME_HOLD_MS`, 120 ms allowance).
       4. `vk_hdr_present_source`.
       5. `vk_upload_flush(0)`.
       6. The `PresentJob` goes to the "vk present" thread. At most one job waits, so the caller blocks while one is
          pending.
16. **"vk present" `present_main`** (~75).
    1. `vk_thumbs_frame` (flight recorder), `vk_dlssg_present_frame`.
    2. `vk_present_blit` → `gfx_present.c` `present_image(defer=1)` (~1323), under `g_pl`.
    3. `vk_sync_submit(NULL, 0)`: an empty submission so deferred destruction waits for the blit. It may also replay
       held work.
    4. `wait_until(pace_smooth(...))`: back-edge pacing to the slot. `NX2NAVI_PACE_SMOOTH` keeps a floor of 7/8 of
       the recent interval.
    5. `vk_present_flip` → `flip_locked` → `vkQueuePresentKHR` under `g_ql` (or `vk_dlssg_present` /
       `vk_fg_present`).
    6. `perfmon_shown`, `vk_pipeline_autosave`.
17. **What `present_image` (gfx_present.c) does,** under `g_pl`:
    1. `flip_locked` for the previous blit; the swapchain is remade if dirty.
    2. `vkWaitForFences` on the slot (5 s); `vkAcquireNextImageKHR` (1 s), or the frame-generation acquire.
    3. `window_settings_live`, `cam_speed_tick`.
    4. `menu_overlay_frame`: the ImGui layout, the font upload on the first frame.
    5. Record:
       1. bars;
       2. `put_picture`: blit, or FSR 1 (EASU, RCAS), colour, HDR encode;
       3. `hudless_record`;
       4. `movie_overlay`;
       5. `menu_overlay_draw`;
       6. capture and thumbnail;
       7. `vk_fg_frame`, `vk_dlssg_frame`, `ng_record`.
    6. `vkQueueSubmit2` under `vk_queue_lock`: waits on acquire, signals render-done, fences the slot.
    7. The flip is deferred to `vk_present_flip`.

### 4.5 With `NX2NAVI_REPLAY_ASYNC=1` (off by default)

- **Submit.** `nvnQueueSubmitCommands` takes the front-end `submit_lock` and runs `nvn_submit_prepare_async`. That
  clones recordings still read by queued submissions.
- **Queueing.** `vk_be_submit` copies the handles, takes the uniform snapshot now (`vk_usnap_take`), and
  `replay_enqueue`s. That reserves the timeline value under `rl`, and waits while the 1024-item ring is full.
- **Replay thread.** "vk replay" runs steps 5 to 11 for each item, and `present_image` for each present item.
- **Present.** The game's present waits until the previous present was handled ("frame bound"), without giving up
  its core (L-43).
- **What changes:**
  - event order across queues is off;
  - the present-held logic is off;
  - `exec_wait` becomes a full fence wait.

### 4.6 Where a frame can stall

Collected from the path; these are leads, not findings.

**Waits in the path**
- The acquire's pacing sleep and GPU fence wait (step 1).
- `vk_present_drain` at every submit (step 5).
- The wait after the first submission of every frame (step 12).
- Shader and pipeline waits from the frame's pools (step 9).
- The texture pressure sweep every 8th submit, under `tex_lock` (step 5).
- The present thread's 5 s fence and 1 s acquire waits under `g_pl` (step 17).

**Contention**
- `g_ql` held across `vkQueuePresentKHR` (step 16), which `queue_submit` waits behind.
- `sub_lock` held across a whole replay, which the present thread's empty submission waits behind.

**Other sources**
- Lazy write-back faults on any guest thread, which wait for GPU work under `frz_lock`.
- Pool finalize, which waits for the GPU (`vkQueueWaitIdle`) on a guest thread.

---

## 5. Allocators, pools and caches

"Limit" names the constant, setting or variable that bounds it, or "none found". "Frees" says when its memory goes
back.

### 5.1 Guest memory and kernel (CPU side)

| # | What it holds | Owner | Limit | Frees |
|---|---|---|---|---|
| 1 | Guest address space reservations (`memory.c` `mem_init` ~818): module window, kernel region (`RT_KERNEL_SIZE` 256 MiB), 14 GiB low region (stacks 256 MiB, heap), 64 GiB alias region, host-function window | `memory.c` | Fixed sizes | Never released; pages decommitted on heap shrink, `UnmapPhysicalMemory`, stack unmap |
| 2 | Guest heap and alias mappings (the application pool) | `memory.c` `mem_set_heap_size`, `alias_map` | `NX2NAVI_APP_POOL`, else `rt_app_pool_bytes` (9809 MiB) + `res_heap_extra_bytes()`; heap `HEAP_CAP` 12 GiB | Committed on demand, decommitted when shrunk or unmapped |
| 3 | Kernel-region allocator (`mem_kernel_alloc`, power-of-two classes, `KCLASSES` 29, `KCOMMIT` 64 KiB steps): thread TLR and TLS, shared fonts (up to 64 MiB each), the exception page | `memory.c`, `klock` | The kernel region; "kernel region exhausted" is fatal | Free lists; never decommitted |
| 4 | Moved stacks `stack_maps[4096]`, `alias_pages` (16 MiB calloc), `module_perm[]` | `memory.c` | Fixed | Commit/decommit on map/unmap; tables never freed |
| 5 | Lazy write-back segments `lzs` | `memory.c`, `frz_lock` | None found (realloc doubling from 256) | When touched, thawed or released before a mirror eviction; the array never shrinks |
| 6 | Lazy write-back hot list `lz_hots` | `memory.c` | `LZ_HOTS` 1024 | Replacement |
| 7 | Guest thread host stacks | `kernel.c` | 256 MiB reserved each, committed on demand | At thread exit |
| 8 | `RtThread` + `Cpu` | `kernel.c` | — | When the thread has exited and its handle is closed |
| 9 | Handle table `g_h` | `kernel.c` | `MAX_H` 4096; fatal when full | Slots freed at `CloseHandle` |
| 10 | Fibers | `kernel.c` | `FIBER_MAX` 1024; 256 MiB reserve each | At `FinalizeFiber` |
| 11 | System-event table `g_se` | `kernel.c` | 512; fatal "table full" | Entries never removed (L-10) |
| 12 | Events | `kernel.c` | — | When `refs` reach 0 |
| 13 | Critical-section trace buffer | `kernel.c` | `CRIT_N` × 32 B, only with `NX2NAVI_CRIT` | Never |
| 14 | Dispatch table | `dispatch.c` | `TABLE_SPAN_LIMIT` 1 GiB reserved; host hash `HOST_CAP` 8192 | Never (sealed) |
| 15 | Host-function registry | `hostfn.c` | `MAX_HOSTFN` 4096 | Never |
| 16 | Guest `jmp_buf` table `jt` | `guest_setjmp.c` | `JMP_N` 1024; fatal beyond | Never removed (L-10) |
| 17 | Loader tables (hash, functions, PLT slots) | `loader.c` | — | Never |

### 5.2 Files, services, settings, logs (CPU side)

| # | What it holds | Owner | Limit | Frees |
|---|---|---|---|---|
| 18 | Log queue `Q.buf`, `Q.out` | `logq.c` | `LOGQ_CAP` 1 MiB each; a larger line is written by its own thread | Never |
| 19 | Log file | `main.c` | `LOG_CAP` 64 MiB, `LOG_KEEP` 5 rotations | Rotated at start |
| 20 | Early log buffer | `main.c` | 32 KiB static | — |
| 21 | Settings entries and strings | `cfg.c` | `CFG_MAX_KEYS` 512 | Old strings kept on every change "for readers" (L-24) |
| 22 | Per-thread settings memo `cfg_memo` | `cfg.c` | 256 per thread | Thread exit |
| 23 | RomFS index, overlays, open counters, `handles[65536]` | `romfs.c` | Fixed | Never |
| 24 | RomFS whole-image mapping | `romfs.c` | Only with `NX2NAVI_ROMFS_MMAP` | Never |
| 25 | zstd dictionaries | `ov_zstd.c` | `NDICT` 32, each ≤ `MAX_DICT` 64 MiB; beyond, the guest decoder runs | Never ("another thread may be decoding with one") |
| 26 | zstd stream contexts | `ov_zstd.c` | `NSTREAM` 16 | Kept with the slot |
| 27 | Per-thread `ZSTD_DCtx` in `host_decode` | `ov_zstd.c` | One per thread | Never |
| 28 | Save file tables `mounts`, `files`, `dirs` | `fs_host.c` | 16, 1024, 256; fatal when full | Slots at close |
| 29 | Directory listings | `fs_host.c` | — | At `CloseDirectory` |
| 30 | Copy buffers (commit, import) | `fs_host.c` | `COPY_CHUNK` 1 MiB, 2 MiB | Per call |
| 31 | Save backups on disk | `fs_host.c` `rotate_backup` | `SAVE_BACKUPS` 3 | Oldest removed |
| 32 | Save zip read and write buffers | `save_share.c`, `save_zip.c` | `MAX_ZIP` 512 MiB, members 256 MiB, 4000 files | After staging |
| 33 | Mods table | `mods.c` | `MAX_MODS` 512 | Never |
| 34 | Cheats | `cheats.c` | None found (realloc) | Never |
| 35 | IPC sessions and domain objects | `ipc.c` | — | Sessions at refs 0; domain sub-objects not freed (L-25) |
| 36 | Offline socket events | `net_offline.c` | `NEFD` 64; spare events 64 | Spare events never closed |
| 37 | System message queue | `ov_system.c` | 32; dropped silently when full | — |
| 38 | Applet message queue | `ipc.c` | 32; dropped with a log line | — |
| 39 | Detached `PlatThread` structs, per-thread timer and file events | `platform/win32.c` | One set per thread | Never (L-26) |

### 5.3 Audio, movies, input, menu, diagnostics

| # | What it holds | Owner | Limit | Frees |
|---|---|---|---|---|
| 40 | Audio renderer tables (voices, pools, mixes, sinks, effects) | `audren.c` `renderer_new` | Voices ≤ 1024, mixes ≤ 256, sinks ≤ 256, effects ≤ 4096; game's work buffer 0x10000 | At renderer close |
| 41 | Mixer output ring | `audio_mixer.c` | `RING_FRAMES` 16384 × 6 channels (about 340 ms); prefill `NX2NAVI_AUDIO_PREFILL_MS` 0..500, default 20 | At mixer close |
| 42 | Mixer work buffers, splitter ids (reallocated every update), limiter delay | `audio_mixer.c` | Limiter ≤ 6 × 192000 floats | At close |
| 43 | Effect delay lines, 3D reverb (made lazily on the render thread) | `audio_effects.c`, `audio_reverb.c` | Delay ≤ 20000 ms at ≤ 192 kHz | At close |
| 44 | Guest AUX/capture rings, circular sinks | `audio_mixer.c` | ≤ 16M samples (fatal above); sink ≤ 256 MiB | At close |
| 45 | Audio change events | `audio_mixer.c` | 16 slots; events past 16 never signalled or closed (L-25) | Never |
| 46 | Menu sounds | `menu_sound.c` | 7 sounds ≤ 10 s each; 8 voices | Never |
| 47 | Microphone ring | `audio_capture.c` | 2 s | `mic_close` |
| 48 | VP9 decoder buffers: `IN_BUFS` 4 × `IN_CAP` 2 MiB (guest-visible), `OUT_BUFS` 8 NV12 frames, libvpx context | `ov_movie.c` | `MAX_MOVIES` 16 decoders | `release_codec`, `destroy` |
| 49 | AV1 decoder queues: `QMAX` 8 pictures, `INMAX` 8 samples, `CQMAX` 2, 16 registered frames; per-picture Y16/U16/V16 temporaries | `ov_movie2.c` | Fixed counts | `clear_queues`, `finalize`; `Video` struct at `ov_dtor` |
| 50 | Opus decoders | `ov_media.c` | None found (one per guest object) | At Finalize |
| 51 | Input script events | `input.c` | Script file size | Never |
| 52 | Motion ring | `motion_fusion.c` | `MOTION_RING` 64 | — |
| 53 | Menu art pixels (14 arts) | `ui/menu_art.cpp` | A few MB | Never ("the pixels stay"); GPU images by `menu_art_release` |
| 54 | Menu asset dictionaries | `menu_assets.c` | 8; per asset ≤ `MAX_RAW` 512 MiB | Never |
| 55 | Menu event queue | `ui/menu.cpp` | `EV_MAX` 256 | — |
| 56 | Perf overlay history and histograms | `perfmon.c`, `perf.c` | `PERF_HISTORY` 120; fixed bins | Static |
| 57 | Profiler tables | `prof_win.c` | About 15 MB static, `SLOW_DUMPS` 64 | Static |
| 58 | Flight recorder ring and thumbnails | `flight.c`, `gfx_present.c` | `NX2NAVI_FLIGHT_FRAMES` (600, 64..7200) | Never; dump jobs freed by the dump thread |
| 59 | Camera speed copies and temporaries | `camera_speed.c` | `copies[16]`; temporaries ≤ 64 MiB | Per search |

### 5.4 GPU memory (`vk_memory.c`, `vk_headroom.c`, `vk_gpa.c`)

**How the VRAM budget works.**

- *Computation.* `vram_query` sums `heapUsage` and `heapBudget` over the device-local heaps (`VK_EXT_memory_budget`;
  the driver's figure counts every process). `NX2NAVI_VRAM_BUDGET_MB` caps it (`budget_cap`).
  `vk_mem_vram_live` counts free space inside VMA's blocks as room.
- *Mirrors.* `mirror_create` evicts idle mirrors (`mirror_evict`) only when that frees enough. It may then exceed the
  budget while mirrors total no more than half of it (`MIRROR_SHARE` 2), asking textures for the room
  (`vk_texture_want_room`). Otherwise it refuses the pool, which stays imported (read over PCIe).
- *Textures.* `vk_texture.c` `sweep_pressure` runs every 8 submits. Its limit is `NX2NAVI_TEX_VRAM_PRESSURE` percent
  of the budget, default 80.
- *Headroom.* The headroom thread keeps one spare block per memory type while usage plus two blocks stays at or under
  85 % of VMA's budget. It does not apply the cap (D-04).

| # | What it holds | Owner | Limit | Frees |
|---|---|---|---|---|
| 60 | Pool records: imported host pools (`import_pool`), physical pools (dedicated device-local, `alloc_physical`, no budget check), virtual pools (page tables) | Guest threads via the back end | None found | Pool finalize; mirrored ones by `mirror_retire` on the completion thread |
| 61 | VRAM mirrors of imported pools | Replay | The budget rules above; `NX2NAVI_MEMORY` auto/import/mirror/verify; `NX2NAVI_MIRROR_EVICT` (default 1) | Pool finalize, or `mirror_evict`: idle ≥ `MIRROR_IDLE` 120 submissions, ≤ `MAXV` 64 candidates, oldest first. Deferred to fence +2 |
| 62 | Mirror retry state | Replay | `RETRY_SOON` 16, `RETRY_SUBS` 600 submissions; a pool refused for `MR_ALLOC` is never retried (L-23) | — |
| 63 | Per-submission copy lists (`pend`, `wbl`, `mregions`) | Replay | Grow only | Reset each submission |
| 64 | Flush ranges per pool | `flush_lock` | `FLUSH_RANGES` 32 | — |
| 65 | Page-generation tables (`gen_chunk`, `cgen_chunk`, 256 MiB chunks of uint32 on first stamp) | `gen_lock` | `RT_ADDRESS_LIMIT` | Shutdown |
| 66 | Shadow storage per imported pool, and base copies (one 4 KiB malloc per GPU-written page) | Shadow mode (`NX2NAVI_SHADOW`; Linux default) | None found | Pool finalize (L-24) |
| 67 | Write-back merge snapshot chunks | Replay / completion | ≥ `SNAP_CHUNK` 1 MiB; pool ≤ `SNAP_POOL_N` 128 and budget/64 clamped 64-256 MiB; `NX2NAVI_WB_MERGE_MAX` 32 MiB | At fence; the excess destroyed |
| 68 | Lazy write-back command buffer and fence | `lz_init_lock` | One | Never freed explicitly |
| 69 | Upload batches (command buffers and fences: inflight and spare) | `cmd_lock` | Grow only | Reused; shutdown |
| 70 | Staging cache | `cmd_lock` | `STG_CACHE` 24 buffers, `STG_CACHE_BYTES` 192 MiB | At batch retire |
| 71 | Staging VMA pool | — | `STG_BLOCK` 64 MiB blocks; `STG_MIN_BLOCKS` 4 kept on a discrete GPU (256 MiB of host memory), `STG_MIN_BLOCKS_UMA` 2 on an integrated one; requests ≤ 32 MiB; an open batch is submitted at `STG_FLUSH` 64 MiB of staging | — |
| 72 | Staging arena | `cmd_lock` | `ARENA_CHUNK` 8 MiB, ≤ 2 MiB per request, `ARENA_KEEP` 8; `NX2NAVI_STG_ARENA` | At fence; shutdown |
| 73 | Deferred destruction list `dead[]` (images, views, buffers, host frees) | `cmd_lock` | None found | `vk_mem_collect` on the completion thread once the stamped value is reached |
| 74 | Headroom spare (half a VMA block per device-local type; block `NX2NAVI_VMA_BLOCK_MB` 64, 16..256) | "vk headroom" | 85 % of VMA's budget; `NX2NAVI_VRAM_HEADROOM=0` off | Replaced, dropped when there is no room or commit is low, at stop |
| 75 | Device address table (L1 65536 entries, `BLOCKS` 1024 × 16 MiB L2, `RECS` 65536, about 17 MiB) | `gpa_lock` | Fixed; when exhausted, logs and shaders read zeros | Free lists at fence +1 |
| 76 | VMA allocator (`preferredLargeHeapBlockSize` = `vk_vma_block_size`) | `vk_device.c` | — | Shutdown |

### 5.5 Textures (`vk_texture.c`, `vk_astc.c`)

**Texture cache eviction.**

- *Release overflow.* `sweep_released`: `RELEASED_MAX` 2048 entries, and `NX2NAVI_TEX_RELEASED_MB` (default RAM/32
  clamped 512-2048 MiB, raised by budget/4 up to 8192).
- *Stale.* `drop_stale`: `STALE_FRAMES` 120 presents, `GPU_STALE_FRAMES` 1800.
- *Overwritten.* `drop_overwritten`: `OVERWRITTEN_KEEP` 600.
- *VRAM pressure.* `PRESSURE_STEP` 32 per check.
- *Idle.* `evict_idle`: idle for `IDLE_ROUNDS` 240 pool walks and `STALE_FRAMES`; `EVICT_STEP` 64 per check.
- *Disuse.* `evict_disused`: unsampled for `DISUSE_SECONDS` 120; retired, then destroyed after the pool sets turn over.
- *Commit pressure.* `commit_evict`: `COMMIT_EVICT_STEP` 256 MiB, `COMMIT_LOW_KEEP` 256 MiB; `NX2NAVI_TEX_PROC_COMMIT_MB`
  (minimum 24576).
- *Destruction.* Deferred (`vk_defer_image`, `vk_defer_free`).

| # | What it holds | Owner | Limit | Frees |
|---|---|---|---|---|
| 77 | Texture image cache (`by_key`, `by_addr`). Memory comes from: pool memory; a VMA sub-allocation ≤ `SUBALLOC_MAX` 32 MiB; a reused image (`NX2NAVI_TEX_REUSE`); its own allocation (dedicated ≥ `DEDICATED_MIN` 32 MiB) | `tex_lock` | No count cap; the evictions above | The evictions above |
| 78 | Disuse holds | `tex_lock` | `HOLDS_MAX` 65536; cleared when full | — |
| 79 | Image memory requirements cache `img_reqs` | `tex_lock` | None found | Shutdown |
| 80 | Reusable released images | `tex_lock` | ≤ 128 searched per create | With the release sweep |
| 81 | Texture and sampler slots | `tex_lock` | Texture slots ≤ 1<<20 (fatal beyond); sampler slots ≤ 4096 | Shutdown |
| 82 | Sampler cache | `tex_lock` | None found; keyed by parameters, DLSS LOD bias and anisotropy; `vkCreateSampler` failure is fatal (L-19) | Shutdown |
| 83 | Failed-create hold ring | `tex_lock` | 32 entries, 2 s | — |
| 84 | ASTC decode scratch | `vk_astc.c` `g_lock` | ≥ `SCRATCH_MIN` 32 MiB, `CHUNK_OUT` 16 MiB pieces; new per upload batch unless `NX2NAVI_ASTC_SCRATCH_KEEP=1` | Deferred |
| 85 | CPU ASTC worker pool | `tex_astc_mt.c` | 7 threads | Never |

### 5.6 Replay, submission, present (`vk_exec*.c`, `vk_usnap.c`, `vk_rec.c`, `vk_sync.c`, `gfx_present.c`)

| # | What it holds | Owner | Limit | Frees |
|---|---|---|---|---|
| 86 | Per-command-buffer resources (transient host-coherent chunks, descriptor pools, written ranges) | Replay; `spare_lock` | `CHUNK_BYTES` 4 MiB; spares `SPARE_POOLS` 64, `SPARE_CHUNKS` 16; allocation failure is fatal | `cb_done` on the completion thread |
| 87 | Image view cache (`view_of`, `views_by_image`) | Replay | No count limit | With the image (`vk_exec_image_gone`) |
| 88 | Bindless texture and sampler pool sets | Replay | `VK_POOL_SETS` 4 (fewer when the device has no room), `VK_POOL_TEX` 32768 × 16 kinds, `VK_POOL_SAMP` 4096; change log 1<<20 | Shutdown |
| 89 | Mesh indirect ring and count buffer | Replay | `MESH_RING` 4096 × 12 B, `MESH_COUNTS` 4096; wraps | Never destroyed (allocation handle discarded, L-30) |
| 90 | Fixed draw tables (stand-ins, shader table 2048×4, and others) | Replay | Fixed | Shutdown |
| 91 | BC stand-ins | `bcs_lock` | `BC_STANDINS` 64 | — |
| 92 | Held submissions (deep copies of recordings for event order) | Replay | `CONT_MAX` 36, `EVO_QUEUES` 16 | When replayed |
| 93 | Completion callbacks per GPU write, report, event | Replay | — | When run |
| 94 | Pending counter reports | Replay | `PENDING` 1<<16 | — |
| 95 | Occlusion query ring | Replay | `OQ_N` 4096; when full, draws go uncounted | — |
| 96 | Uniform snapshot arenas (with a host copy under DLSS) | `free_lock` | ≥ `ARENA_MIN` 4 MiB each; `MAX_RANGE` 64 KiB per binding; free list never trimmed | `arena_done` at fence; destroyed only at shutdown |
| 97 | Uniform snapshot GPU-write list `U.wr` | Replay | Pruned only while snapshots are on (L-29) | `wr_prune` |
| 98 | Recorder packet ring | `vk_rec.c` | `RING_WORDS` 1<<21 (16 MiB static); a packet > 1/4 ring is fatal; a full ring spins | Static |
| 99 | Command buffer slots | `sub_lock` | Grow only | Reused after their fence |
| 100 | Completion queue | `st_lock` | Grows ×2 | Entries run and drop |
| 101 | GPU timestamp pairs | `vk_sync.c` | `TS_PAIRS` 1024 | — |
| 102 | Async replay ring | `rl` | `RQ_CAP` 1024 with back-pressure | — |
| 103 | Recording-buffer pool | `nvn_api_cmd.c` | `POOL_SLOTS` 16, `POOL_MAX_BYTES` 4 MiB | Reused |
| 104 | Texture pins | `nvn_api_cmd.c` | `NVN_PIN_AGE` submissions without references | Swept |
| 105 | Recording copies and zombies | `copy_lock` | None found | Zombies once their submissions are replayed |
| 106 | Live-texture copy table | `live_lock` | One per storage address | — |
| 107 | GVA window | `nvn_api_core.c` | `GVA_MAX` 1024 ranges; full → `gva_alloc` returns 0 | — |
| 108 | Swapchain images and views, present command buffers and fences, capture buffer, thumbnails | `g_pl` | `MAX_IMAGES` 16 | On swapchain remake, release |
| 109 | HDR, FSR 1, tonemap targets and source views | `vk_hdr.c`, `vk_fsr.c`, `vk_tonemap.c` | `SLOTS` 8 source views | Remade on a size change after `vkDeviceWaitIdle` |
| 110 | DLSS features and work images, motion-vector blocks | `vk_dlaa.c` | `DLSS_SIZES` 5; a size unused for 3600 evaluations is freed; `MVM_SLOTS` 16 | — |
| 111 | Frame dump read-back buffers | `vk_dump.c` | `PNG_CAP` 2 GiB, `INFLIGHT_CAP` 3 GiB; debug only | After writing |
| 112 | GPU breadcrumbs | `vk_crumbs.c` | `GP_SLOTS` 16384 | — |
| 113 | Address bind reports | `vk_vaddr.c` | None found while `NX2NAVI_VA_REPORT` is on | Never |

### 5.7 Pipelines and shaders (memory and disk)

| # | What it holds | Owner | Limit | Frees |
|---|---|---|---|---|
| 114 | Pipelines and their fast-link variants, hash `pipes[4096]` chained | `P.lock` | None found; never evicted | Only at `vk_pipeline_shutdown`: `vk_program_release` is a weak no-op (D-03, L-24) |
| 115 | Pipeline layouts, set layouts, update templates | `P.lock` | None found | Shutdown |
| 116 | Stage libraries `libs[1024]` chained; failed builds cached as null | `P.lock` | None found | Shutdown (`libs_destroy`) |
| 117 | Library and pipeline job queues | `P.lock` | None found | When built; dropped at shutdown |
| 118 | `VkPipelineCache` in memory and `pipeline-cache.bin` | `cache_rw`, `save_lock` | Each file > `NX2NAVI_PIPELINE_CACHE_MAX_MB` (RAM / 32, 1024 to 4096 MB) starts empty, and no cache past it is written; in memory: the driver's, none found | Saved by "pipe autosave" at most every 20 s and at exit (waits ≤ 5 s) |
| 119 | Pipeline binaries `pipeline-binaries.bin` | `PB.lock` | `NX2NAVI_PIPE_BINARIES_MAX_MB` 1024; `NX2NAVI_PIPE_BINARIES` default 0 | — |
| 120 | `pipeline-keys.bin` and its `seen` set | `K.io` | None found (append-only) | Never |
| 121 | `compute-used.bin` | `cu_io` | `NX2NAVI_COMPUTE_KEEP` 512 | — |
| 122 | Large-compute builder's own pipeline cache | "big compute" | `NX2NAVI_BIG_BUILD_FREE_MB` 8192, `NX2NAVI_BIG_BUILD_MAX_MB` 4096 | Merged, then destroyed |
| 123 | Shader memory cache `buckets[4096]` (SPIR-V and reflection) | `cache_lock` | None found; "for the life of the process" | Only by the probe's `gfx_shader_drop_memory` |
| 124 | Use records `use_buckets`, `use_files` | `cache_lock` | `USE_FILE_BUCKETS` 65536 buckets; entries none found | Never |
| 125 | Failed shader keys | `failed_lock` | `FAILED_N` 4096; when full, failures are no longer remembered | — |
| 126 | Shader disk cache: `shader-cache/` loose files, `shader-cache.pack`, `in/*.in`, records | `gfx_shader.c`, `tools/make_game.py` | None found on size; translators listed ≤ `IDS_MAX` 8; packed once 1,000 loose translations exist (`PACK_LOOSE_MIN`) | The carry pass prunes old translators' entries; the pack is replaced by a pruned copy at the next start |
| 127 | Translation job and load queues `X.head`, `X.lhead` | `X.lock` | None found | When done |
| 128 | Translator child processes | `S.lock` | `NX2NAVI_XLATE_PROCS` (2), `MAX_SVC` 32; raised during prewarm | Kept for the process |
| 129 | GPU driver caches (per exe; NVIDIA's per-process disk cache on Linux) | The driver | Driver-defined | Driver-defined |

That is 129 allocators, pools and caches.

---

## 6. Docs and comments against the code

The code is what the map follows. "Verified" means the item was checked directly at `ed36773d`. The others come from
the mapping passes' reading, cited, and not re-checked line by line.

| # | Where | What it says | What the code does | Verified |
|---|---|---|---|---|
| D-01 | `docs/architecture.md` "Threads and cores"; `kernel.c` comment ~322 | Three application cores, three virtual cores | `kernel.c` `NCORE` = 6. Comment ~30: Switch 2 titles get cores 0-5. `NX2NAVI_CORES` can lower it | yes |
| D-02 | `docs/architecture.md` "Graphics" | A recorder thread makes the `vkCmd*` calls | Only where `NX2NAVI_VK_RECORDER` is on: the default is off on AMD (`vk_rec.c` ~170, `rt_knob("VK_RECORDER", amd ? "0" : "1")`) | yes |
| D-03 | `vk.h` ~687, `vk_backend.c` ~581 | `vk_pipeline.c` implements `vk_program_release` and drops a released program's pipelines | No strong definition exists; the weak no-op in `vk_backend.c` always runs | yes |
| D-04 | `vk_memory.c` ~656 | `NX2NAVI_VRAM_BUDGET_MB` caps "every decision here and in vk_texture.c" | `vk_headroom.c` `room` reads `vmaGetHeapBudgets` itself, without the cap | yes |
| D-05 | `memory.c` ~68 | "frz_lock is a leaf lock: nothing writes guest memory under it" | `lazy_drop_locked` and `lazy_piece_locked` call the back end's `lz_fetch` under it (a GPU copy into guest memory and a wait of up to 10 s); `mem_lazy_arm` calls `lz_clean` under it | yes |
| D-06 | `vk_memory.c` ~1744 vs the log line in `evict_enabled` ~1766 | The comment: "Mirrors never go to make room for textures". The log line: idle mirrors make room "for textures under VRAM pressure" | `mirror_evict` is called only from `mirror_create`. The comment is right; the log line is wrong | partly (comment read) |
| D-07 | `svc.c` ~45 `unimplemented[]` | 0x36 `SynchronizePreemptionState` is listed as never reached | `kernel.c` implements 0x36. The table is reached only after the kernel declines, so this has no effect | yes (listing) |
| D-08 | `audren.h` ~176 | "Every call but open/close/set_running runs with r->lock held" | `audio_mixer_pace_ns` (which can destroy and reopen SDL streams) and `audio_mixer_out_stats` run without the lock | no |
| D-09 | `ov_movie2.c` header ~16; `make_picture` ~311, ~333 | One host thread per decoder; make_picture runs on the decoder thread; "a frame the PC has no memory for is dropped" | A decoder thread, a convert thread and 5 stripe threads per picture; make_picture runs on the convert thread; the 8-bit fast path calls `rt_fatal` when its temporaries fail to allocate | no |
| D-10 | `input.c` ~7 | Every SDL gamepad call is made under `lock` | The pad thread makes them under `sdl_lock` only (the comment at ~401 describes the newer scheme) | no |
| D-11 | `vk_sync.c` ~54, `vk_fsr.c` ~105 | Every idle wait takes the queue lock | `vk_pipeline.c` `vk_pipeline_shutdown` (~6412) and `vk_device.c` `destroy_all` (~548) call `vkDeviceWaitIdle` without `g_ql` | no |
| D-12 | `vk_pipeline.c` `bg_main` comment ~5143 | `bg_queue` runs on the replay thread | Its callers are in `lb_compute`, which runs on `lb_worker` threads | no |
| D-13 | `vk_rec.c` ~32 | `rec_prio` is the submitting thread's priority | It is captured once at the first `rec_begin`, from whichever thread holds `sub_lock` then, and never follows `rec_claim`'s raise | no |
| D-14 | `nvn_api_cmd.c` ~417 | `copy_lock` guards every prefix | `nvn_submit_prepare_async` writes `prefix->deferred` after unlocking (~1294) | no |
| D-15 | `gfx_shader.c` ~3 | More translator processes for the boot prewarm only | If `boot_wait` detaches the prewarm, the raised limit (up to 16) stays during play until the prewarm returns | no |
| D-16 | `ui/menu_art.cpp` ~4; `gfx_window_sdl.c` ~6; `menu_sound.c` ~10 | The window thread uploads the art; the window thread makes every SDL video call; the menu thread is the only producer of sounds | The present path uploads the art; several SDL video calls run on the caller's thread; sounds are started from the window, polling and present threads | no |
| D-17 | `ov_input.c` ~2, `flight.c` ~10, `audio_mixer.c` ~786 | `input_poll` in `input_win.c`; `perf_present` in `prof_win.c`; `audio_renderer.c` | They are in `input.c`, `perf.c` and `audren.c` | no |
| D-18 | `vk_fg.cpp` ~51 | The game's queue is shared with the recorder | The recorder never submits (`vk_rec.h`: "submitting and the command pool stay on the submitting thread") | no |
| D-19 | `platform/win32.c` ~473 | The timer handle is "one handle per thread, kept until exit" | Kept until process exit: never closed when a thread exits | no |
| D-20 | Task context and docs: app pool 9809 MiB | — | `memory.c` `mem_init` adds `res_heap_extra_bytes()` (resolution settings), and `NX2NAVI_APP_POOL` overrides it | yes |

---

## 7. Leads for Phase 3

Phase 3 went through every lead; the note in italics after each number says what became of it (a ledger ID of section 8, or struck with the reason).

Every lead is suspected, from reading code only, and none is a finding. Each line gives the place and the
suspicion.

### Crashes, hangs, data loss (pass 1, 2, 8)

- **L-01** *(Phase 3: F-2-04, suspected.)* `fs_host.c` `save_commit`, `commit_run`. A second Commit while the first runs: the first thread clears
  `commit_pending` while the second waits for `tx_lock`, so `save_wait_commit` may return early and a write may land
  during the second copy.
- **L-02** *(Phase 3: F-1-09, suspected.)* `fs_host.c` `file_of`, `read_directory`; `romfs.c` `handle_source`. A handle or listing is used after the
  lock is dropped while another guest thread may close it.
- **L-03** *(Phase 3: F-8-02, suspected; the transaction itself confirmed atomic.)* `fs_host.c`. Crash and `rt_fatal` paths do not call `fs_save_flush`. Recovery is left to `save_recover` at
  the next start; pass 8 should interrupt each commit step and check it.
- **L-04** *(Phase 3: F-4-05, suspected; the AMD-crash part struck.)* `memory.c` `lazy_drop_locked`, `vk_memory.c` `lazy_fetch`. A GPU copy and a wait (10 s, then fatal) inside
  the exception handler under `frz_lock` serialise every other fault, arm and thaw. It is a candidate for the AMD
  crash and device-loss reports, and for hitches where lazy write-back is on.
- **L-05** *(Phase 3: struck: nothing under `frz_lock` touches guest pages, and the handlers never take `lock`.)* `platform/win32.c` `fault_veh` → `memory.c` handlers. A fault on a thread that already holds `frz_lock`
  (or `lock`, in `stack_move`'s copy) would re-take a non-recursive lock.
- **L-06** *(Phase 3: struck: the completion thread runs a submission's callbacks before the fence the game waits for, and mirrored records are freed on that same thread.)* `vk_memory.c` `vk_gpu_written_back`. A `PoolRec` from `vk_mem_lookup` is used after `pool_lock` is dropped,
  on the completion thread, while `vk_pool_fini` may destroy it.
- **L-07** *(Phase 3: struck: reachable only through F-2-05.)* `vk_memory.c` `mirror_create`. `mstate = MS_ON` is set after `pool_lock` is released; a concurrent finalize
  may skip `mirror_forget` and leave the record in `mlist`.
- **L-08** *(Phase 3: struck: only a method racing its own destructor, a fault of the game's.)* `ov_movie.c` `find`. The movie is used after `g_lock` is released while `destroy` may free it.
  `ov_movie2.c` `ov_decode` reads `v->dav1d` unlocked against `finalize`.
- **L-09** *(Phase 3: F-1-03, suspected.)* `ipc.c` `ipc_svc` 0x21/0x22. The session is used without a reference while `CloseHandle` may free it.
  Domain fields are unlocked.
- **L-10** *(Phase 3: F-1-07 and F-1-08, growth confirmed; `files[1024]` struck (slots are freed at close).)* Fixed tables that are never emptied, each fatal when full, for multi-hour sessions:
  - `kernel.c` `g_se[512]`: system events, bound by `ov_system.c` `ov_report_event` and `net_offline.c`;
  - `guest_setjmp.c` `jt[1024]`;
  - `fs_host.c` `files[1024]`.
- **L-11** *(Phase 3: F-1-04, suspected; the fiber and bind parts struck.)* Concurrency in the kernel and zstd:
  - `kernel.c` `kern_fiber_finalize`, `kern_fiber_initialize`: fiber fields outside `g_fiber_lock`;
  - `kernel.c` `kern_bind_system_event`: check-then-create;
  - `ov_zstd.c` `stream_take`: a spent slot reused while still read.
- **L-12** *(Phase 3: struck: such a thread holds no core, and the thread count stays 84-85.)* `kernel.c` `thread_main`. A thread created but never started (or closed first) waits forever, keeping its
  stack reservation.
- **L-13** *(Phase 3: F-4-09, suspected; the crash and race parts struck.)* `camera_speed.c` `search_main`. It rewrites every byte-pattern match of the camera speed array across the
  guest heap and alias region, while the game runs.
- **L-14** *(Phase 3: F-6-01, suspected; not reproduced in 54 logs.)* `menu_assets.c` `dicts_load`. Lazy dictionary load without a lock, from the "menu-art" thread and the
  present thread.
- **L-15** *(Phase 3: struck: the game never shuts the device down; only probes do.)* `vk_pipeline.c` `autosave_main`. It is never stopped and outlives `vk_pipeline_shutdown`; `vkDeviceWaitIdle`
  at shutdown runs without `g_ql` (D-11).
- **L-16** *(Phase 3: F-1-06, F-2-01, F-2-02, F-2-03; overlapping submits confirmed.)* Sync-mode races:
  - `vk_backend.c` `PH`, `FH`, `vk_exec_held` and `nvn_submits_in_frame`;
  - `nvn_api_core.c` `q->submits`;
  - `nvn_api_cmd.c` `resolve_deferred` in place.

  All of these apply if the game submits and presents from different threads.
- **L-17** *(Phase 3: struck: this game has no such patch sets, so the function returns before the switch.)* `frame_rate.c` `frame_limit_live`. It is reached from three threads without a lock, and can call
  `rt_patch_switch` concurrently.
- **L-18** *(Phase 3: struck: the environment is written only at start and just before exit.)* `cfg.c` `raw_get`. `getenv` races with `cfg_setenv` → `_putenv_s` while the game runs
  (`gfx_present.c` ~90).
- **L-19** *(Phase 3: struck as a crash lead: these fire only when memory is gone; the measured 175-178 samplers are far from the driver's limit. The texture part is in F-3-01 and F-3-02.)* Fatal on allocation failure: staging, transient chunks, samplers (`vk_texture.c` `sampler_for`), the AV1
  picture temporaries. A texture create failure triggers no eviction, only a 2 s hold.
- **L-20** *(Phase 3: F-1-10, suspected; not determined (no dungeon or boss).)* `audio_mixer.c` and `audren.c`. `rt_fatal` on game-supplied parameters during RequestUpdate, with the
  renderer lock held.

### Memory and VRAM (pass 3)

- **L-21** *(Phase 3: F-3-01, suspected; not reproduced.)* `vk_texture.c` `upload` (~2411). A failed GPU ASTC decode leaves the texture marked uploaded and
  uninitialised until its storage changes. This is one candidate for the white ground after long play.
- **L-22** *(Phase 3: F-3-02, suspected; not reproduced.)* `vk_texture.c` `evict_disused`, `retire_sweep`, `create_tex`. Retired or failed textures show stand-ins;
  re-creation is limited per frame (`WANT_PER_FRAME` 32). Another candidate for white or untextured ground.
- **L-23** *(Phase 3: F-3-03, suspected; not reproduced.)* `vk_memory.c` `mirror_create`. A pool refused for `MR_ALLOC` is never retried and is read over PCIe for
  life. A candidate for towns running slower in long play than in fresh scripted runs.
- **L-24** *(Phase 3: struck as unbounded growth (each item is bounded by the game's content or the player's changes; flat across the reload); F-3-12 for the memory the shader cache keeps.)* Growth without a limit for the life of the process:
  - pipelines, libraries, layouts and the shader memory cache (`vk_program_release` is a no-op);
  - the sampler cache;
  - shadow-storage base copies (`vk_memory.c` `sbase_make`, one 4 KiB copy per GPU-written page, kept until the pool
    is finalized; shadow mode only);
  - `pipeline-keys.bin`;
  - `cfg_set`'s old strings.
- **L-25** *(Phase 3: F-3-04 and F-3-05, suspected; the offline socket part struck (spare events are reused).)* Leaked kernel objects per call:
  - `audio_mixer.c` change events (beyond 16);
  - `ipc.c` domain sub-objects;
  - `net_offline.c` spare events.
- **L-26** *(Phase 3: F-3-06, suspected.)* `platform/win32.c`. Per-thread timer and file-event handles are never closed. One "save commit" thread per
  Commit and one "camera speed" thread per search leak a few handles each.
- **L-27** *(Phase 3: F-3-07, suspected.)* `vk_headroom.c` `room`. It ignores `NX2NAVI_VRAM_BUDGET_MB`, so a capped test run is not the small card it
  claims to be.
- **L-28** *(Phase 3: struck: bounded by the armed pages; 84-144 ranges measured.)* `memory.c` `lzs`. Lazy write-back segments grow without a cap, and every fault and arm scans them linearly.
- **L-29** *(Phase 3: F-3-08, suspected for the non-default mode.)* `vk_usnap.c`. `U.wr` grows without pruning when `NX2NAVI_UBO_SNAPSHOT=0`. The arena free list is never
  trimmed.
- **L-30** *(Phase 3: F-3-09, suspected.)* `vk_exec_draw.c` `mesh_ring_entry` (~5048). The mesh ring's allocation handle is discarded, so it is never
  destroyed (shutdown only).

### Stutter (pass 4) and performance (pass 7)

- **L-31** *(Phase 3: F-7-01, suspected; the A/B is not valid yet.)* `nvn_api_core.c` `nvnQueueSubmitCommands` and `nvn_api_obj.c` `nvnWindowAcquireTexture`. Every frame waits
  for the GPU twice (after its first submission, and at acquire for the image's last present). At 120 fps and above,
  a slow GPU frame becomes a CPU stall. This is the first suspect for the uncapped-rate spikes.
- **L-32** *(Phase 3: F-4-07, suspected; the lock part struck.)* `vk_sync.c` `submit`. `sub_lock` is held over a whole replay; the present thread's empty submission waits
  behind it, and held work may then replay on the normal-priority present thread.
- **L-33** *(Phase 3: F-4-06, suspected.)* `gfx_present.c` `flip_locked`, `create_swapchain`, `movie_overlay`, `hudless_record`.
  `vkQueuePresentKHR` and `vkDeviceWaitIdle` run under `g_ql`, which `queue_submit` waits for. `vk_memory.c`
  `vk_upload_flush(1)` waits for fences under `cmd_lock`.
- **L-34** *(Phase 3: F-4-10, suspected; the motion thread and the recorder spin struck.)* Thread churn and polling:
  - `ov_movie2.c` `run_stripes` creates and joins 5 threads per frame;
  - `input.c` `motion_main` takes `lock` 250 times a second forever;
  - `vk_rec.c` `wait_space` spins without yielding;
  - `vk_pipeline.c` `vk_pipeline_compute` polls every 1 ms without a limit.
- **L-35** *(Phase 3: F-7-03, suspected.)* `vk_texture.c` `sweep_pressure`, `evict_idle`, `evict_disused`. Each scans the whole cache under `tex_lock`
  on the submitting thread every 8 submits.
- **L-36** *(Phase 3: F-4-04, the time confirmed.)* `vk_memory.c` `vk_pool_fini`. `vk_upload_flush(1)` plus `vkQueueWaitIdle` on a guest thread per finalized
  pool: a hitch at area changes.
- **L-37** *(Phase 3: struck for the default settings: the boot waits without a cap.)* `boot.c` `boot_wait` → detach. A prewarm still running in play keeps up to 16 translator processes and
  cpus-many worker threads busy.
- **L-38** *(Phase 3: F-4-08, suspected; the audio part struck.)* The present path does slow work under the present lock: `ui/menu.cpp` `menu_overlay_frame` (`cfg_save`,
  `vkQueueWaitIdle` for the font, art uploads); `audio_mixer.c` reverb allocation and device enumeration on the
  highest-priority render thread.
- **L-39** *(Phase 3: F-7-02, suspected.)* `vk_rec.c` `rec_begin`. The recorder thread keeps the first caller's priority, not the recording thread's
  highest, and may sit outside the reserved core.
- **L-40** *(Phase 3: struck: fences are added in order.)* `vk_sync.c` completion queue. It is FIFO, not ordered by fence, so one late callback holds back the frees
  behind it.
- **L-41** *(Phase 3: kept as a rule of method, not a finding.)* `prof_win.c` sampler. Time-critical, suspending every thread each 1 ms. Phase 2 must not compare profiled
  runs with unprofiled ones.
- **L-43** *(Phase 3: struck: the core is already given away before the wait.)* `vk_backend.c` `vk_be_present` (asynchronous replay only). The "frame bound" wait (~561-567) blocks the
  game's present thread without giving its virtual core away, unlike `replay_drain` and `replay_enqueue`.

**Leads added by Phase 2's measurements** (section 11.3). They are measured, but their causes are not traced.

- **L-44** *(Phase 3: F-4-01, confirmed.)* A periodic stall in the Market. In `bl_market_1`, after the script ended (Link standing, segment
  `market-end`), every 8th frame took 25-30 ms from frame 13042 to 13786 (90 frames). The same pattern appears 5 times
  in `bl_market_2`. Nothing is named:
  - no render-thread wait, no work mark;
  - GPU 3.15 ms per frame, "bound: cpu";
  - 6 submits per frame, so it is not `vk_texture.c` `sweep_pressure`'s every-8-submits rhythm.

  A profiled run (`NX2NAVI_PROFILE_SLOW`) would name it.
- **L-45** *(Phase 3: not a growth of the host's replay (its time stays 5.5 ms); the cause is not determined.)* The CPU frame grows while standing in Hyrule Field toward dusk (`bl_fieldlong`).
  - Over 9 minutes the frame went from about 7.1 to 8.4 ms (140 → 118 fps).
  - The GPU's time stayed at 4.37 ms, and every window was "bound: cpu".
  - The size lines are flat, except released textures (165 → 542).
- **L-46** *(Phase 3: struck: 32512 is the game's own fixed pool, full from the start.)* `texture pool slots 32512/32768` in every area and run: the registered texture slots the bindless pool
  covers (`vk_texture.c` `pool_slots`, `VK_POOL_TEX` 32768). What happens to a slot past `VK_POOL_TEX` was not checked.
- **L-47** *(Phase 3: F-4-02 and F-4-03, confirmed.)* The first one or two world frames after every save load take 48-68 ms and 26-36 ms, with nothing named.

### Logic that assumes the original rate (pass 8)

- **L-42** *(Phase 3: F-8-05, F-8-06, F-8-07, suspected; `res_present` and the mouse camera struck.)* Assumptions in the host code:
  - `input.c` `shake_stick` uses `SHAKE_SIDE_NS`, two frames at 30 fps;
  - `dynres.c` defaults its target to 16.7 ms whatever the limit;
  - `resolution.c` `res_present` gives up after 1800 presents ("half a minute at 60 fps");
  - `frame_rate.c` `movie_cap_hz` forces 60 during movies;
  - `mouse_direct.c` `camera_input` scales by the 30 Hz ratio.

  Already known in game code: the pick-up popup (`opickup`), the item jingle, and the ocarina recorder's four
  rounding sites.

---

## 8. Findings ledger

Phase 2 entered F-1-01 and F-1-02. Phase 3 (section 12, stopped before its end) added the rest. "Suspected" rows come from reading code and are not bugs until reproduced; "confirmed" rows name the run or tool output that shows them.

| Field | Meaning |
|---|---|
| ID | `F-<pass>-<n>`, e.g. `F-1-03` |
| Severity | S1 crash, data loss or out-of-memory; S2 hitch, unbounded memory growth or major visual bug; S3 minor visual or UI bug; S4 optimization |
| Subsystem | A row of section 2 |
| Location | `file` `function` (~line) |
| What happens | — |
| How to trigger | — |
| Status | confirmed (reproduced, or shown by tool output, with the evidence) or suspected (code reading, with the reasoning) |
| Proposed fix | — |
| Risk of the fix | Including whether it changes gameplay timing or behaviour, the save format, or crosses subsystems (those need the user's approval first) |
| Lead | The L-number it came from, if any |
| Fix commit / verification | Phase 4: the commit, the repro, and what showed the fix works |

| ID | Sev | Subsystem | Location | What happens | Trigger | Status | Fix | Risk | Lead | Commit / verified |
|---|---|---|---|---|---|---|---|---|---|---|
| F-1-01 | S1 | Save data (7) / the game's own loader | Lifted `f_700112b3b0` = `main+0x112b3b0`, called from `main+0x74d7b0`/`0x74d660` | The process dies with `FATAL: access violation (0xc0000005) at host … (nx2navi+0x42419ae), reading address 0x150` in guest code while a save loads. **Cause traced (Phase 3, 12.3):** the save names the area `HyruleCastle_Child` (twice in `progress.sav`); the 1.1.1 RomFS has only `Banc/HyruleCastle/HyruleCastle_Static` and `_Dynamic`, with no `_Child` area. The game opens `Banc/HyruleCastle/HyruleCastle_Child_Static.bcett.byml.zs` (twice), then `.byml`, then the `_Dynamic` pair, gets "not found" for each, reads `Banc/GameBancParam/HyruleCastle_Child.game__banc__GameBancParam.bgyml`, and then reads through a null area object at +0x150. The host answered correctly: the files do not exist. The save's header bytes match the other 11 saves. | Load the test install's slot 1 (`E:\NX2navi-060b\game\profile\save\<user>\slot_01`: Kokiri Forest, child, 3 hearts, autosave dated 11/5/2026 12:07 a.m., blank thumbnail, `caption.sav` 11638 bytes against about 40 KB for the others) as the only slot (`E:\nx2devrun\out\audit\saves\one_d1`), then Load. Runs `probe_d1`, `probe_d1b`, `probe_d1_all`, `f101_fs` | confirmed: 4 of 4, including `ocarina_all.exe` (no audit code) and a file trace. Where the save came from (another game version, or an edited save) is not determined. It is the only Kokiri Forest save available | The host cannot repair the save. It can make the failure readable: when an area's `Banc/<area>/<name>_Static.bcett.byml` is asked for and absent, log one line that names the area and say in the crash report that the loaded save names an area this game version does not have (and see F-6-02 for showing it) | None for a log line. Refusing the load would change game behaviour: ask the user first | — | `58a8d21d`. The crash stays (the save cannot load). Verified that it now explains itself, run `p4_d1`: `RomFS: the game asked for the area HyruleCastle_Child (Banc/HyruleCastle/HyruleCastle_Child_Static.bcett.byml.zs), which this game version does not have; the save being loaded names it and cannot be loaded here`, and after the FATAL `fatal: the game asked for the area HyruleCastle_Child, which this game version does not have; a save that names it cannot be loaded` |
| F-1-02 | S4 | In-game menu (21) | `ui/menu_skin.cpp` `skin_atlas_fill`, lambda `lattice` (~265) | `x * 374761393` and `y * 668265263` are signed `int` multiplications that overflow for x ≥ 6 (undefined behaviour; on this compiler they wrap, so the grain looks as meant) | Any start: the menu's font atlas is filled at start-up | confirmed by UBSan: a trap `ud1 0xc` (multiply overflow) at `nx2navi+0x11782fd2`, `addr2line` → `menu_skin.cpp:265` (run `san_ubsan`, section 11.4) | Do the hash in `unsigned` (`(unsigned)x * 374761393u + …`) | None: the same bits, no gameplay or format change | — | `cebdd4a9`. UBSan build without the ignore list (`ocarina_ubsan_fix.exe`), the route: `p4_ubsan` went through the whole route (261 s of world time) with no trap: the start no longer stops at `menu_skin.cpp:265`, and none of the changed code tripped a check |
| F-1-03 | S1 | Services (5) | `ipc.c` `ipc_svc` 0x21/0x22 (~516), `kernel.c` `kern_handle_get` (~2008), `ipc_handle_closed` (~237) | `kern_handle_get` returns the session without a reference and `dispatch` uses it unlocked. A CloseHandle on another guest thread frees it mid-request. Domain tables (`objects`, `nobjects`) are also written unlocked while `dispatch` reads them | A guest thread closes a session while another is inside SendSyncRequest on it (the console keeps the session alive). The sessions here are made at boot, plus the audio renderer and amiibo | suspected (code read, `kern_handle_get` re-read by the auditor); not seen in any run | Take a reference under the kernel lock for the length of the request, released through the path `ipc_handle_closed` uses | Low; services only | L-09 | — |
| F-1-04 | S1 | Files (6) | `ov_zstd.c` `stream_drop` (~340), `stream_take` (~326) | `stream_drop` clears the slot by pointer, not by owner: thread A looks up spent slot k for its stream and unlocks; thread B takes slot k (all 16 slots taken); A's `stream_drop` zeroes it; thread C takes it as free and resets the decoder context B is decoding with. The asset B decodes is corrupted | More than 16 stream states alive, so that slots are reused, with loader threads decoding at once. Phase 2 saw 0 of 16 slots taken at every snapshot (snapshots fall outside loads) | suspected (code re-read by the auditor) | `stream_drop(st, s)` clears only when `st->s == s`, under `stream_lock` | None | L-11 | `32f2a41a`. Not reproduced before, so not shown fixed. Regression: the route with the fixed exe: `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6) |
| F-1-05 | S1 | Textures (13) | `vk_texture.c` `same_content` (~2489-2493), `upload` (~2264-2272) | `same_content` hashes a texture's guest storage with no readability check and no read hold; `upload` hashes it after `mem_readable` but outside `mem_read_hold`. If the game unmaps the storage between the check and the hash, the render thread reads decommitted memory: an access violation in host code | A texture of at most `CHASH_MAX` bytes whose storage is freed while a page change sends it through `make_fresh` / `make_fresh_snap` | suspected (code re-read by the auditor). Whether a change can be reported for a range just unmapped was not traced | Hash under `mem_read_hold`; in `same_content` treat a failed hold as "changed" | Low, textures only | — | `a9005e5c`. Not reproduced before, so not shown fixed. Regression: the route with the fixed exe: `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6) |
| F-1-06 | S1 | Replay (11) | `vk_exec.c` `vk_exec_held_ready` (~1261), `cont_walk` → `cont_free` (~1332) | `vk_exec_held_ready` reads the head continuation of each event queue (`evq[s].head->at`, `->since`) without `sub_lock`. A second submitting thread inside `vk_sync_submit` can free that continuation meanwhile: a read of freed memory, then `vk_exec_event_met` through a dangling pointer | Held (event-order) work at the moment two guest threads are in submit or present. The precondition is confirmed: submits overlap from the pause menu's opening (F-2-01) | suspected (code re-read by the auditor) | The front-end lock of F-2-01, or read under `sub_lock` | Low to medium; see F-2-01 | L-16 | — |
| F-1-07 | S1 | Process entry (2) | `guest_setjmp.c` `slot_of` (~101), `ov_setjmp` (~127) | The `jmp_buf` table `jt[1024]` keys on the buffer's guest address and entries are never removed; when it is full `ov_setjmp` ends the process with "setjmp: more than 1024 buffers" | A long session: every new `jmp_buf` address takes an entry for good | confirmed growth, the fatal itself not reached: `mem: sizes … setjmp buffers` went 10 → 22 and 8 → 23 across one save, title and reload (section 11.3). At +12 to +15 per reload the table lasts about 70 reloads if the addresses keep changing; whether they level off was not measured | When full, replace the entry set longest ago and log once; a `longjmp` to a replaced buffer stays fatal as now | Low: only the overflow path changes | L-10 | `7dfa809c`. The full table was not reached in any run, so the new path has not run. Not reproduced before, so not shown fixed. Regression: the route with the fixed exe: `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6) |
| F-1-08 | S1 | Kernel (3) | `kernel.c` `kern_bind_system_event`, `g_se[512]` (~2060-2095) | The system-event table keys on the event's guest address and entries are never removed; full is fatal ("table full") | A long session with many reloads | confirmed growth, the fatal not reached: `system events` went 8 → 10 across one reload in both route runs (about 250 reloads to the limit) | Reuse an entry whose event is no longer live (`se_live` 0) | Low | L-10 | `1a57cc21`. The full table was not reached in any run, so the new path has not run. `tests\sync_wake_check.py` (builds `kernel.c`) passes. Not reproduced before, so not shown fixed. Regression: the route with the fixed exe: `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6) |
| F-1-09 | S1 | Save data (7), Files (6) | `fs_host.c` `read_directory` (~1307, no lock at all), `file_of` (~1068), `close_directory` (~1340); `romfs.c` `handle_source` | A directory listing or file handle is used after the lock is dropped (or with none taken): a CloseDirectory on another guest thread frees the listing `read_directory` is copying from; a CloseFile closes the host file a read is using | The game closes a handle on one thread while another uses it. That is a fault of the game's own; the console answers it with an error result, not a crash | suspected (code re-read by the auditor); not seen | Hold `lock` shared for the length of the call, or count users per slot | Low | L-02 | `d61cd9b9`. Directory listings only; file handles keep their window. Not reproduced before. `--probe-fs` and `tests\save_share_check.py` pass with the change |
| F-1-10 | S1 | Audio (18) | `audren.c` `need` (~66-70), `audio_mixer.c` parameter checks (~641-1045) | Parameters the game supplies in RequestUpdate (limiter, reverb, sink sizes) are checked with `rt_fatal`; the console's renderer returns an error result instead | An effect configuration outside the checked ranges. The route (five areas, the pause menu) tripped none; dungeons and bosses, whose effects differ, could not be reached | suspected; not determined by a run | Return the error result and log once instead of ending the process | Changes what the game is told in that case: ask the user first | L-20 | — |
| F-1-11 | S1 | Replay (11), GPU memory (12); cause not determined | The GPU fault is reported by `vk_crumbs.c` and the device-fault lines; the process ends in `vk_memory.c` ("upload submit failed (-4)") or `vk_sync.c` ("the wait for a submission's commands failed (-4; device lost?)") | The GPU reports a page fault and the device is lost while a save loads: `vk: device fault address 1: read invalid at 0x31f000000` (also `0x30b000000`, `0x311000000`), right after `vk: texture pool: an area load of about 4629 textures was made in 1 frames`; the GPU's last mark is `dispatch 3 of submission N: 293x1x1 groups, cs 3524d41df45a41e0`, started and finished | Loading the Hyrule Field day save (`saves\one_u3`) in `E:\nx2devrun` while the boot's pipeline prebuild is skipped there (F-4-11), so pipelines are built at their first draw | confirmed 3 times: runs `p3_l31_nosubmit` (`SUBMIT_FINISH=0`), `p3_l31_noacquire` (`ACQUIRE_WAIT=0`) and `p3_l31_default2` (default settings), three in a row. In the same state `p3_l31_default`, `p3_l31_nosubmit2` and `p3_market_prof` loaded without it; none of the earlier runs of Phases 2 and 3 (prebuild not skipped) had it. **Later data from the session diagnosing it** (its runs, `E:\nx2devrun\out\crash_matrix.txt`): the loss follows the lazy write-back's piece release, not the skipped prebuild: 0 lost in 15 loads of the night field save (`saves\one_u0`) with `NX2NAVI_WB_LAZY_CHUNK=0`, 0 in 10 with `NX2NAVI_WB_LAZY=0`, about 7 in 22 with it on, and one more in the day field (`out\bn_fday_new_2`). The read fault is at the first page of a 64 MiB device memory that holds a 26.6 MiB buffer and 125 images bound 2 s earlier. The mechanism is not known | `test-all-fixes` switches the piece release off by default (`6e3f29d4`, not on this branch: here `WB_LAZY_CHUNK` still defaults to 1). The cause itself is open | — | — | Not the audit's commit; the audit's own runs after it (`p4_*`, 7 save loads with the default) lost no device |
| F-1-12 | S3 | Replay (11) | `vk_sync.c` `completion_main` (~395-431) | After the device is lost the completion thread can still take the timeline as advanced: it ends the process only when `vkWaitSemaphores` or `vkGetSemaphoreCounterValue` return an error, and a lost device may answer both with success and a signalled value. It then runs the callbacks of submissions that never finished (frees, record reuse, counter reports) before the next checked call reports the loss, so the first symptom in the log is a wrong one | Any device loss (F-1-11) | symptom confirmed, mechanism suspected: in `p3_l31_noacquire` the line `vk: occlusion query slot 96 not available after its fence, counted as visible` comes six lines before `vk: device fault`. Reported by the session diagnosing F-1-11; the code was re-read by the auditor (the two result checks are there, a check of the device itself is not) | Ask the device whether it is lost before running callbacks (a cheap checked call, or the fault query), and end with the fault report first | Low: only the path after a loss changes | — | — |
| F-2-01 | S2 | NVN front end (10), Replay (11) | `vk_backend.c` `submit_sync` (~385-400), `ph_flush` (~366), `vk_be_present` sync branch (~546-557), `present_image` (~220-232) | The synchronous replay's front end takes no lock of its own. `ph_flush` is check-then-clear on `PH.on`, so two submitting threads can both present one held frame, and `present_image` (frame log, pending draws, `vk_dlaa_frame`, `frame_hold`, all unlocked globals) can run while another thread's replay writes the same state | Two guest threads inside QueueSubmitCommands or QueuePresentTexture at once, with a present held for event order | precondition confirmed, harm suspected. Run `p3_route`: `nvn: thread 36088 enters a submit on queue 0x134133020 while 1 other submit or present call run (overlap 1, frame 8086)`, then 31 more by frame 12847; the same in both Market runs from the pause menu's first frame. Four threads submit: one on 3 queues, the presenting thread on 4, two more on the present queue. No doubled present was seen (no "went out … after the game's present call" line in these runs) | One mutex around the synchronous `submit_sync` and the synchronous branch of `vk_be_present`; replays already serialise on `sub_lock` | Low to medium: a present may wait behind another thread's replay. No save or gameplay change, but it crosses subsystems 10 and 11: ask the user first | L-16 | — |
| F-2-02 | S3 | NVN front end (10) | `nvn_api_core.c` `nvnQueueSubmitCommands` (`nvn_submits_in_frame++`, ~676), reset in `nvn_api_obj.c` `nvnQueuePresentTexture` (~868) | A plain `int` is incremented by every submitting thread and reset by the presenter. A lost update gives a frame's first submission no GPU wait (the white-frame case the wait exists for) or one wait too many | The overlapping submits of F-2-01 | suspected; the overlap is confirmed | Make the counter atomic (fetch-add, exchange at the present) | None | L-16 | `ca06d5c8`. Not reproduced before, so not shown fixed. Regression: the route with the fixed exe: `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6) |
| F-2-03 | S3 | NVN front end (10) | `nvn_api_cmd.c` `resolve_deferred` (~1218-1252) | Deferred binds are resolved by copying new handles into the recording in place, without a lock. A continuation of an earlier submission of the same command handle, still held, is replayed later with the new handles | The same command handle submitted again while its earlier continuation is held | suspected | Copy the handles when a continuation of the handle is held, as the asynchronous path does | Low | L-16 | — |
| F-2-04 | S1 | Save data (7) | `fs_host.c` `save_commit` (~601-622), `commit_run` (~583), `save_touched` (~99) | Two Commits entered at once both pass `save_wait_commit`, both set `commit_pending`, and each starts a thread. The first thread clears `commit_pending` while the second still waits for `tx_lock`; a save write then passes `save_touched` and lands in the working copy while the second thread copies it. The committed save can mix files from before and after the write | Two guest threads calling Commit at the same moment, then a save write. One Commit after another (the only pattern seen) is safe: the second waits for the first | suspected (code re-read by the auditor); not seen. The transaction itself is confirmed atomic against a crash (12.3) | Count pending commits instead of a flag, or take `pend_lock` across the check and the start | Low; the save format is untouched | L-01 | `b2f9ea4c`. Not reproduced before (it needs two Commits at once). With the change `--probe-fs` still reads `a crash at each of 6 commit steps recovers to the old save 3 times, the new one 3 times, mixed 0 times; 3 backups` and `tests\save_share_check.py` passes |
| F-2-05 | S2 | Replay (11) | `vk_backend.c` `submit_sync` (~392-396) | A submission stopped at an event wait returns its fence value while its remainder is still held for a later replay. The game's wait on that value can return before the held part is even recorded; if the game then reuses what that part reads, the held replay reads newer memory | Event-order holds (dialogue scenes are the known case) | suspected; needs a counter of fence waits that return with continuations held | Signal the value only once the held remainder has been replayed, or count such returns first | Changes when the game's fence waits return (timing): ask the user first | — | — |
| F-3-01 | S2 | Textures (13) | `vk_texture.c` `upload` (~2404-2413, ~2451); `vk_astc.c` `vk_astc_record` (~566-572) | When the GPU ASTC decode cannot be recorded (its scratch buffer of 32 MiB or more fails to allocate, or no pipeline), `upload` logs "not decoded on the GPU: left as it was" and still sets `uploaded = 1`. The image keeps what its memory held before. It is not retried: the content hash was taken from the storage, so equal bytes written again are skipped as unchanged | An allocation failure, which needs the commit or the device heap exhausted (the budget cap never fails an allocation), at an area load. This is one candidate for the white ground after long play | suspected (code re-read by the auditor). Not reproduced: the line "not decoded on the GPU" is in none of the logs here, the user's recent ones included. A capped run cannot fail an allocation, so it would not reproduce this | On a failed record leave `uploaded` and `chash_ok` clear so the next lookup retries, with a limit, or fall back to the CPU decode | Low, textures only | L-21 | `054bd556`. Part only: the content hash is forgotten after a failed decode, so the next write uploads. A texture whose decode failed is still not uploaded again until its storage is written: that part is open. Not reproduced before. Not reproduced before, so not shown fixed. Regression: the route with the fixed exe: `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6) |
| F-3-02 | S2 | Textures (13), Replay (11) | `vk_texture.c` `held_out` (~1853), `slot_resolve` (~3109-3124), `evict_disused` (~1863); `vk_exec_draw.c` `make_dummy` (~354-382) | A texture evicted for disuse leaves a hold that ends only at a shader mark or a lookup outside the pool refresh. If a pool-indexing shader that cannot be marked is built after evictions have started, further evictions stop but the keys already held stay held, and what only that shader samples shows its stand-in for good. The stand-ins are 1x1 images moved from UNDEFINED to GENERAL with no clear, so their colour is undefined | VRAM over 80 % of the budget or low commit, textures unsampled for 120 s, then an unmarkable pool shader. A second candidate for the white ground | suspected (code re-read by the auditor for `make_dummy`). The run planned for it (the route under a 6 GiB cap) was not done before Phase 3 was stopped | Clear the holds (and cancel retiring textures) when the first unmarkable shader appears; clear the stand-ins to a defined colour | Low; gives back VRAM that was freed | L-22 | — |
| F-3-03 | S2 | GPU memory (12) | `vk_memory.c` `refuse` (~1745), `vk_mem_use` (~2140-2146) | `refuse` sets a retry time only for a budget refusal (`r->retry_sub = why == MR_BUDGET ? … : 0`). A pool whose mirror allocation failed (`MR_ALLOC`) stays refused until it is finalized and is read over the bus for the rest of the session | A device-local allocation failure (heap or commit exhausted) at the pool's first use | suspected (line re-read by the auditor). Not reproduced: `failed 0` in every `mem: mirror` line of every run here; the budget cap gives budget refusals, never this one | Give `MR_ALLOC` the same retry, and keep the refusal reason in the record so the counters stay right | Low | L-23 | `94ee6e78`. Not reproduced before, so not shown fixed. Regression: the route with the fixed exe: `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6) |
| F-3-04 | S3 | Audio (18), Kernel (3) | `audio_mixer.c` change events (~1213-1218, ~1296, ~1365) | Each call of the audio device's change-event commands makes a new kernel event, which takes a slot of the handle table (`MAX_H` 4096, fatal when full) and is never closed; events past the 16th are never signalled | Each call of those commands. Handles grew 112 → 116 and 112 → 117 across one reload (section 11.3); which calls took them was not traced | suspected; the handle growth is measured, its source is not | One shared event per kind, as the other audio commands already use | Low | L-25 | — |
| F-3-05 | S3 | Services (5) | `ipc.c` domain objects (`objects[32]`, `ipc.h` ~54; CloseVirtualHandle ~428) | A domain's object table is never compacted and closing a virtual handle frees nothing; the 33rd object is answered with an unknown-command error | A domain session that opens and closes sub-objects repeatedly (the applet proxy), for instance at each return to the title | suspected; needs a run with `NX2NAVI_TRACE=ipc` counting the commands per title return | Free the slot on close and reuse it | Low | L-25 | — |
| F-3-06 | S4 | Platform | `platform/win32.c` `sleep_timer` (~478), `plat_file_pread` / `plat_file_pwrite` events (~1267, ~1526), `plat_thread_detach` (~73) | Per-thread timer and event handles are never closed when a thread exits, and a detached thread's small record stays. One "save commit" thread per changed Commit and one "camera speed" thread per search leak about two handles each | Every save and every sensitivity change | suspected; tens per hour against a limit of millions | Close them when the thread ends | None | L-26 | — |
| F-3-07 | S4 | GPU memory (12) | `vk_headroom.c` `room` (~112-120) | The headroom thread measures against the driver's budget, not the capped one, so under `NX2NAVI_VRAM_BUDGET_MB` it still keeps its spare blocks. A capped test run understates what a small card does | Any capped run | suspected (code read). The cap is a test switch | Compare against the smaller of the driver's budget and the cap | None outside capped runs | L-27 | — |
| F-3-08 | S3 | Replay (11) | `vk_usnap.c` `vk_usnap_drop` (~433), `vk_usnap_begin` (~358), `arena_get` (~90) | With uniform snapshots switched off (`NX2NAVI_UBO_SNAPSHOT=0`, not the default) the GPU-write list is appended on every write and never pruned, and its linear scan slows without limit. In the default mode the list is pruned (199-250 entries at every snapshot) but the free arenas are never trimmed (3-6 arenas, 12-24 MiB measured) | Only with the switch off | suspected for the off mode; the default mode's sizes are measured and flat | Return early in `vk_usnap_drop` when snapshots are off; trim arenas idle for some submissions | None | L-29 | `3966d878`. Non-default mode only; not run in that mode. Not reproduced before, so not shown fixed. Regression: the route with the fixed exe: `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6) |
| F-3-09 | S4 | Replay (11) | `vk_exec_draw.c` `mesh_ring_entry` (~5044-5053), `mesh_count_buf` (~4990) | The allocation handles of the mesh ring (48 KiB) and the count buffer (16 KiB) are dropped, so neither is ever freed | Once per process | suspected; a fixed 64 KiB | Keep the handles and destroy them at shutdown | None | L-30 | — |
| F-3-10 | S3 | Textures (13) | `vk_texture.c` `upload` (~2336-2342 with ~2272) | When a texture's storage is released during its upload it is uploaded as zeros, but the content hash already holds the bytes from before. If the game maps the same bytes back, `same_content` matches and the upload is skipped: the texture stays zero | Storage unmapped in the middle of an upload, then mapped again with equal content | suspected (code read) | Clear `chash_ok` whenever the bytes that reached the image are not the storage's (the same change as F-3-01) | Low | — | `054bd556`. Not reproduced before, so not shown fixed. Regression: the route with the fixed exe: `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6) |
| F-3-11 | S4 | GPU memory (12) | `vk_memory.c` `refuse` (~1747) | The log quota for mirror refusals is shared by all reasons (12 lines), so allocation-failure refusals can go unlogged behind budget refusals; only the `failed` count in the `mem: mirror` line shows them | Many budget refusals before the first allocation failure | suspected (code read) | A quota per reason | None | — | `94ee6e78`. With F-3-03. Not reproduced before, so not shown fixed. Regression: the route with the fixed exe: `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6) |
| F-3-12 | S4 | Shaders (14) | `gfx_shader.c` memory cache (`buckets`, `mem_insert`) | The shader memory cache keeps every shader's SPIR-V and reflection for the life of the process, after its pipelines are built | Always | confirmed size: `shader memory cache 3564, 386.2 MiB` to `3676, 395.4 MiB` in every area, flat across the reload (section 11.3). It is bounded by the game's shaders, not growing | Drop the SPIR-V of shaders whose pipelines are all built, keeping what later variants need | Medium: a later pipeline variant would have to read the shader again from disk | L-24 | — |
| F-4-01 | S2 | Replay (11), Textures (13) | `vk_exec_draw.c` `pool_unres` (~2231-2244), `pool_sync_entries` (~2496-2510) | The list of texture pool slots that resolve to no entry grows duplicates: a slot that resolves between two walks has its flag cleared but stays listed, and when it stops resolving `pool_unres` appends it again. Every 8th pool check walks the whole list, calling the slot retry for each copy, under the texture lock. In the Market the list reaches several times the number of slots that exist, and the walk becomes a stall | Standing in the Market (adult) for about a minute. It then repeats every 8th frame; game threads registering textures wait for the lock too | confirmed (12.3): `slots without one` 134371 and 160385 in `p3_market_t1`, 115652 in `bl_market_1`, 88824 in `bl_market_2`, against 32768 slots; the walk's own timer gives about 15 ms a walk in the last window; `texture lock held over 1 ms: 7792 ms (581 holds) … max 33.61 ms` from `bind_resources`; `slow replay frames 225 … its slowest op 20.0 ms (DRAW_MESH 225)`; 375 of 380 gaps between slow frames are 8 frames. In `p3_market_t2` the list fell back to 1273 and no stall came: it depends on timing | Never list a slot twice (drop the stale entry when its flag clears, or check membership), and bound the retries per walk | Low: the walk's result is unchanged, only its length. Textures and replay only | L-44 | `015a89dc`. Before: 134371 and 160385 list entries, walks of about 15 ms, 225 frames over 25 ms standing in the Market (`p3_market_t1`). After: three runs of three (`p4_market_1` to `_3`): the list stays at 524-3192 entries and the walk at 0.008-0.026 ms per switch; standing after the pause tour no frame reaches 20 ms (worst 14.2, 17.3 and 18.3 ms at 118.5, 145.7 and 139.6 fps); the texture lock is held over 1 ms for at most 149 ms in a period (longest hold 4.34 ms), against 7792 ms (33.61 ms) before |
| F-4-02 | S3 | Upscaling (17) | `vk_dlaa.c` `evaluate` (~1645-1675), `release_kept` | A DLSS feature is created on the replaying thread the first time a render size is used, after a device idle wait when kept features are released first | The first frame of the scene after a load, the first frame of the pause menu (its own size), and each reload | confirmed with the mark added in `c85892ab`: `DLSS feature creates 23.5 ms` in the load frame, `16.4 ms` of a 22.4 ms frame at the pause menu's first frame (`p3_market_t1` frame 5663), `43.8 ms` and `44.9 ms` at the reload (`p3_route` frame 20156, `p3_route_cold` frame 18952) | Create the feature for the known sizes ahead of use (at boot or during the load), or on a worker with the previous output shown meanwhile | Medium: one more thread on the device; no gameplay change | L-47 | — |
| F-4-03 | S3 | Replay (11) | `vk_exec.c` / `vk_exec_draw.c` replay of the first frames after a load | One or two frames some 10 frames after a load's big frame take 48-95 ms. With the columns added in `df6766aa` they read as replay time and nothing else: `slow frame 2122: 95.0 ms; blocked pipeline 0.0 xlate 0.0 texture 0.0 astc 0.0 fence 0.0 pacing 0.0 drain 0.0; replay 93.6` | Every save load | confirmed that the time is the replay's; which part of it is not determined (the timed Market run's nearest frame shows 2867 draws against the usual 1250-1900, slowest single draw 8.7 ms) | Not proposed until the part is named (a timed run with `NX2NAVI_REPLAY_TIME=1` on the route) | — | L-47 | — |
| F-4-04 | S2 | GPU memory (12) | `vk_memory.c` `pool_fini` (~382-387), `pool_init` | A pool's finalize flushes the uploads and calls `vkQueueWaitIdle` under the queue lock, on a guest thread, once per used pool; the lock blocks the render thread's submits and the present for that time. Pool initialize imports and allocates on the calling thread | Area changes and boot, when the game makes and frees many pools | confirmed for the time, suspected for the idle wait as its cause: `slow frame 459: 45.8 ms; … on any thread: pool init/finalize 45.8 ms (2, 288.0 MiB, longest 43.1 ms)` and `slow frame 522: 58.5 ms; … pool init/finalize 56.7 ms (71, 94.4 MiB, longest 3.0 ms)` (`bl_route_1`). The mark does not tell initialize from finalize | Wait for the pool's last fence instead of the whole queue, without holding the queue lock | Medium: the wait must still cover uploads and a present not yet drained | L-36 | — |
| F-4-05 | S3 | Guest memory (4), GPU memory (12) | `memory.c` `lazy_drop_locked` / `lazy_piece_locked` (~474-507), `vk_memory.c` `lazy_fetch` (~2600), `mirror_evict` (~1829) | A lazily written range is fetched with a GPU copy and a wait inside the fault handler while `frz_lock` is held, which stalls every other fault, thaw and the replay's skip check for that time. `mirror_evict` does the same while holding the pool lock exclusively | A CPU read of an armed range, a file read near GPU pools, or a mirror eviction, with GPU work queued | suspected (code read); no deadlock path was found. The AMD-crash part of the lead is struck: lazy write-back is off by default on that driver | Mark the range as being fetched and drop `frz_lock` for the copy and wait | Medium: touches the piece protocol; no gameplay or save change | L-04 | — |
| F-4-06 | S3 | Device and present (16), GPU memory (12) | `gfx_present.c` `flip_locked` (~306-316); `vk_memory.c` `vk_upload_flush(1)` (~3780-3791) | The present call runs under the queue lock that `queue_submit` waits for, and a blocking upload flush waits for its fences while holding `cmd_lock`, which every upload, deferred free and the completion thread need | A present that blocks (vsync modes, frame generation); texel reads, pool finalizes and menu art uploads | suspected (code read) | Note the fence, unlock, wait, lock again; a present queue of its own where the device has one | Low for the flush; medium for the queue | L-33 | — |
| F-4-07 | S3 | Replay (11), Device and present (16) | `vk_backend.c` `submit_sync` drain (~389), `gfx_present.c` present thread (~253, ~1339-1349) | Before each submit the game's thread waits for the present thread's whole job (slot wait, acquire, menu, its empty submission). Held work can then be replayed on the present thread, which runs at normal priority off the reserved core | A blocking acquire, or held work resumed by the present thread's empty submission | suspected (code read). The wait now shows as `drain` on the slow-frame line; it was 0.0 in every slow frame of the Phase 3 runs at these settings | Replay held work only on the submitting thread, or raise the present thread's priority | Low | L-32 | — |
| F-4-08 | S3 | In-game menu (21) | `ui/menu.cpp` `menu_overlay_frame` (~1729-1740), `vk_up` (~297); `ui/menu_art.cpp` `menu_font_upload` (~198), `upload_rgba` (~160) | With the overlay menu open, the settings file is written, and fonts and art are uploaded with queue idle waits, on the present thread under the present lock, which the game's next submit waits for | Opening the overlay menu, changing a setting or the scale | suspected (code read); the audio part of the lead is struck (it runs on the audio thread, on a change only) | Save settings on a worker; batch the art uploads; retire the old font with a deferred free | None to gameplay | L-38 | — |
| F-4-09 | S3 | Input (20), Guest memory (4) | `camera_speed.c` `search_main` (~218-247) | The search for the camera speed array reads the whole guest heap, including every lazily written page, so each such piece faults and is fetched from the GPU (F-4-05) while the game runs | Changing the stick sensitivity mid-game | suspected (code read); the crash and race parts of the lead are struck (each piece is read under a hold, writes thaw first) | Skip ranges that are lazily armed | Low | L-13 | — |
| F-4-10 | S4 | Pipelines (15), Movies (19) | `vk_pipeline.c` `vk_pipeline_compute` (~4949-4960); `ov_movie2.c` `run_stripes` (~287-301) | A dispatch whose compute pipeline another thread is building polls with 1 ms sleeps and the wait is not timed as a pipeline wait; each movie picture starts and joins four threads and allocates its planes anew | A compute pipeline not built yet; movies | suspected (code read) | A condition variable and the pipeline timer; a small worker pool and reused buffers | None | L-34 | — |
| F-4-11 | S2 | Pipelines (15) | `vk_pipeline.c` `pm_set` (~4276), `vk_pipeline_prewarm_marker_clear` (~4284; called only from `boot.c` ~379 and the prebuild's end ~4223), `pm_skipped` (~4241) | `pipeline-prewarm-running.txt` exists while the boot builds the recorded pipelines, so that a driver crash there is noticed. No orderly exit clears it. A game closed during that boot build leaves it, the next start reads that as "the last start died", writes `pipeline-prewarm-skip.txt`, and from then on every start on that GPU and driver skips the prebuild: each recorded pipeline is built at its first draw, in play | Close the game window (or end the process) while the boot still builds pipelines, then start again | confirmed by an incident in the shared run folder: at 22:50 a scripted run was closed by its runner during boot (most likely this session's first `p3_market_prof` attempt, which gave way to another game then; its log was replaced by the repeat, so this is not proven), and the next start, another session's `out\gate_off`, logged `vk_pipeline: the last start died while the boot built pipelines (E:\nx2devrun/pipeline-prewarm-running.txt was left); E:\nx2devrun/pipeline-prewarm-skip.txt now skips that prebuild on this GPU and driver (10de:2b85:9b404000)`. Every later start there logs `prewarm: 4063 recorded pipelines not built ahead of play`. No driver crash had happened. The file is still there: it was not deleted, as other sessions run in that folder | Clear the marker on every orderly exit (the window close, the guest's exit, the runner's close), so only a real crash leaves it; give the skip an expiry or a retry | Low; pipelines only | — | `6c0b0b10`. Reproduced and verified with `route\prewarm_close.py` (the window closed one second after the marker appears, each exe on its own pipeline cache folder). Before the fix (`ocarina_audit_p3b.exe`, `p4_prewarm_old`): `marker seen 2.8 s after the start … after the exit the marker is LEFT`, and the next start `took the first for dead; skip file WRITTEN`. After (`ocarina_audit_fix.exe`, `p4_prewarm_fix`): `after the exit the marker is gone`, next start `no such line; skip file absent`. A process that is killed still leaves the marker, as meant |
| F-5-01 | S3 | Textures (13) | `vk_texture.c` `alias_compatible` (~2567), `start_at` → `init_new` (~2755) | A new depth image made over a live depth target's memory starts cleared unless `depth_alias` is on and both are depth-only with one format; a shadow map that starts cleared shadows everything for one frame | The known one-frame dark shadow at the Castle Town gate | suspected cause of a known problem; `NX2NAVI_DEPTH_ALIAS_LOG=1` would line the dark frame up with an alias logged as cleared. Not run (the gate is not on the route) | Carry depth content across the alias for depth-stencil pairs and partial covers too | Rendering behaviour: needs the user's eye on the result | — | — |
| F-5-02 | S2 | GPU memory (12) | `vk_memory.c` shadow storage (`shadow_sync_pool` ~1248, default on for the Linux build ~1676) | On the Linux build CPU-written pools reach the GPU through a per-submission snapshot; a write made after the snapshot goes with the next submission, so per-frame instance data can be one frame behind the buffers around it | The known foliage flicker on Linux with an AMD card | suspected cause of a known problem; cannot be run here (no Linux machine, no AMD card). `NX2NAVI_SHADOW=0` on that machine would tell this from the shader-side suspects in `docs/maxwell.md` | Depends on that test | — | — | — |
| F-6-01 | S3 | In-game menu (21) | `menu_assets.c` `dicts_load` (~56-73, callers ~92, ~125) | The dictionaries are loaded lazily with no lock, and `ndicts` is set to 0 before the load: a second caller meanwhile sees "loaded, none" and gets nothing. The menu's fonts then fall back for the whole run, as the font read is tried once | The "menu-art" thread and the present path both asking during the first frames | suspected (code read). Not reproduced: 54 of 54 logs here show `menu: the game's interface fonts: text ClarimoUDPE, titles CetraDisplayPro` | Load once behind a `plat_once` | None | L-14 | `b4bfb7b6`. Not reproduced before (54 of 54 logs had the game's fonts). The fixed exe's runs show the game's fonts too. Not reproduced before, so not shown fixed. Regression: the route with the fixed exe: `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6) |
| F-6-02 | S3 | Launcher | `launcher/ui/app.py` `play_only` (~2175-2196) against `watch_game` (~1387-1399) | Started from the launcher, a fatal exit (codes 70, 71) shows the game's FATAL line; started from the Steam shortcut the launcher only logs the exit code, and the runtime raises no message itself: the window disappears | Any fatal exit of a game started from the shortcut, F-1-01 included | suspected (code re-read by the auditor) | Show the last FATAL line on codes 70 and 71 in `play_only` as well | Launcher only | — | — |
| F-6-03 | S4 | Settings (8) | `nvn_api_obj.c` `nvnWindowAcquireTexture` (~833), `perf.c` `rt_knob` | `NX2NAVI_ACQUIRE_WAIT` is read with `rt_knob`, which maps every value but "0" and "ab" to 1 and "ab" to 2: the documented mode 2 cannot be chosen, and "ab" selects it instead of alternating | Setting the key to 2 or ab | suspected (code read); a diagnostics switch | Read it as a number | None | — | `4b47a10a`. The value 2 was not run. Not reproduced before, so not shown fixed. Regression: the route with the fixed exe: `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6) |
| F-7-01 | S3 | NVN front end (10), Replay (11) | `nvn_api_core.c` `nvnQueueSubmitCommands` (~670-677, `NX2NAVI_SUBMIT_FINISH`, default mask 1), `nvn_api_obj.c` `nvnWindowAcquireTexture` (~832-843, `NX2NAVI_ACQUIRE_WAIT`) | Every frame the game's thread waits for the GPU to run the frame's first submission, and again at the acquire for the image's last present. While the GPU is ahead the waits are short; when the GPU is the slower side they make the CPU frame wait for it | Every frame; the cost should grow at high frame rates or with a loaded GPU | suspected; not determined. The A/B was started and is not valid: the run folder's pipeline state changed under it (F-4-11) and three of its runs lost the device (F-1-11). The one finished pair reads 99.9 fps with the waits and 121.1 without the submit wait in the day field, but the first run also carried more background pipeline builds. The comments say the submit wait prevents white frames in dialogue (3 % of frames without it) and the acquire wait is mostly covered by it | Depends on a valid A/B: drop the acquire wait when the submit wait is on, or wait on the passes' timeline | Changes when frames are drawn against the game's data rewrites (the white-frame case): ask the user first | L-31 | — |
| F-7-02 | S3 | Replay (11) | `vk_rec.c` `rec_begin` (~175), `rec_main` (~63); `vk_exec.c` `rec_claim` (~1650) | The recorder thread takes the priority of the thread that made the first submission and keeps it; later only the recording thread is raised and given the reserved core. The recorder then competes with some 85 guest threads, and the recording thread waits for it at the end of every part | Any load on the other cores | suspected (code read). The drains are measured: `recorder ring full 0 ms, drains 443 ms` over 49 s in `p3_market_t1`, about 0.07 ms a frame, so the cost was small on this PC | Raise the recorder's priority in `rec_claim` | Low | L-39 | — |
| F-7-03 | S3 | Textures (13) | `vk_texture.c` `sweep_pressure` (~2021), `evict_idle` (~1716), `evict_disused` (~1871) | Under VRAM or commit pressure, every 8th submission the eviction scans walk the whole texture table under the texture lock; a scan that finds nothing does not back off, so it repeats for as long as the pressure lasts | VRAM over 80 % of the budget, or the process's commit over its bound (installed RAM, at least 24 GiB; the baseline's private bytes reach 25.8 GiB) | suspected for the cost; the capped run planned for it was not done | Back off after an empty scan, or keep an order of use | Low | L-35 | — |
| F-7-04 | S4 | Diagnostics (22) | `nvn_api_obj.c` `nvnWindowAcquireTexture` (~839-841), `vk_sync.c` `vk_sync_fence_wait` | The acquire's GPU wait is counted as a fence wait and again as pacing, so the frame-time summary counts it twice and a frame bound by the GPU at the acquire reads as "pace" | Any frame that waits at the acquire | suspected (code read); diagnostics only | Count it once | None | — | — |
| F-8-01 | S2 | GPU memory (12), Textures (13) | `vk_memory.c` `gen_now` (~748), handed out in `vk_mem_changed` (~836) and nine other places; compared in `vk_mem_changed` (~848), `vk_mem_changed_snap` (~923); `DIRTY_UNSYNCED` = `UINT32_MAX` (~991) | The page write generation is a 32-bit counter with no handling of its wrap. After it wraps, new writes compare as older than everything, so CPU-written textures and mirrored pools stop being uploaded again (stale or wrong textures), and pages stamped before the wrap compare as always changed. 0 ("never stamped") and `UINT32_MAX` ("not scanned") are also handed out as real values at the wrap | One long session. The rate decides how long: an estimate from the code is 200-1000 generations a frame, which is 5 to 50 hours | suspected (the counter and the sentinel re-read by the auditor). The rate is not measured: `587e8494` adds it to the `mem: sizes` lines and `ocarina_audit_p3b.exe` is built with it, but no run was made before Phase 3 was stopped | Make the generations 64-bit | Crosses subsystems 12 and 13 (the tables and every compare): ask the user first. No gameplay or save change | — | — |
| F-8-02 | S1 | Save data (7), Settings and logging (8) | `main.c` `crash_handler` (~271-320), `dispatch.c` `rt_fatal` (~60-82), `fs_host.c` `save_commit` (~601-621) | The game's Commit returns once the commit is queued; a thread then copies and swaps (95-135 ms). The crash and fatal paths end the process without waiting for it. A crash inside that window leaves an unfinished copy, which the next start rolls back: the player loses a save the game reported as done | A crash or fatal within about a tenth of a second after a save | suspected (code re-read: `fs_save_flush` is called from the three normal exits only). The roll-back itself is confirmed safe (12.3) | Wait a bounded time for the pending commit in both paths, unless the crashing thread is the commit thread | Save path only; the format is unchanged | L-03 | `cd170dad`. Not reproduced before (a crash within a tenth of a second of a save). The new wait did not run in any run: no crash fell into a commit. `--probe-fs` and `tests\save_share_check.py` pass with the change |
| F-8-03 | S3 | Input (20) | `input.c` `pad_read` (~817-832), `sdl_enter` (~481) | While SDL searches for devices (a pad pulled, a wireless pad asleep) every poll returns the last state read before, so buttons and stick held at that moment stay held for the search's length (120-240 ms per device notification, longer with a slow driver) | Unplugging the controller while moving | suspected (code read); cannot be run here (no hand on the controller) | Stop replaying the last state after about 150 ms without a fresh read | Input only | — | — |
| F-8-04 | S3 | Input (20), In-game menu (21) | `ov_input.c` `npad_states` (~82-98) | The game is always told one controller is connected. A disconnect becomes "nothing pressed" and play goes on uncontrolled; the console would ask for a controller | A controller disconnect in play | suspected (code read) | Open the in-game menu when the primary pad goes away | Changes what the player sees on a disconnect (behaviour): ask the user first | — | — |
| F-8-05 | S3 | Frame pacing (9) | `dynres.c` `dynres_read` (~51-54) | Dynamic resolution's default target is 16.7 ms whatever the frame limit, and a step is held for 45 frames, not for a time: at a 120 limit it reacts only once the rate has fallen to about 55, and at 240 fps the resolution can change five times a second | `NX2NAVI_DYN_RES=1` (off by default, not in the menu) above a 60 limit | suspected (code read) | Derive the default target from the limit; hold steps for a time | None to gameplay | L-42 | — |
| F-8-06 | S3 | Input (20) | `input.c` `shake_stick` (~706-722) | The shake key swings the stick on a wall-clock period (two frames at 30 fps), so at 120 or 240 fps each side lasts 8 or 16 game frames; a game check that counts swings in frames could miss them | The shake key at a high frame limit | suspected; not determined (needs a fused item and the key at 30, 120 and 240) | Count game steps instead of time | Input timing: ask the user first | L-42 | — |
| F-8-07 | S3 | Frame pacing (9), Movies (19) | `frame_rate.c` `movie_cap_hz` (~194-209), `ov_movie2.c` `set_live` (~590, ~740) | The limit is held at 60 while a movie decoder is live, by design. A movie skipped without its end-of-stream picture or a Finalize would leave the decoder live and the game capped at 60 in play | Skipping a movie at a limit above 60 | suspected; not determined (no movie on the route) | End the cap when the decoder has had no sample for a time | Low | L-42 | — |

---

## 9. What was reviewed

Phase 1 read code to map it. It reviewed nothing for bugs.

The map was made in this session by one auditor with four read-only mapping passes run in parallel:

1. kernel, memory, save, settings and system services;
2. audio, input, movies, menu and diagnostics;
3. graphics threads and locks;
4. graphics allocators and caches.

The auditor read the frame path directly and checked the items marked "verified" in section 6. The rest of the
mapping passes' statements are cited to file and function, not re-checked line by line. Phase 3 should treat them as
leads.

### Read fully

**By the auditor**
- `docs/architecture.md`, `docs/building.md`, `docs/maxwell.md`, `docs/launcher.md`, `AGENTS.md`.
- `platform/platform.h`.
- Read fully in the path's functions:
  - `vk_backend.c` 40-450 and 537-636;
  - `vk_sync.c` 50-190 and 370-649;
  - `vk_exec.c` `walk`, `rec_claim`, `vk_exec_record` (1532-1771);
  - `vk_exec_draw.c` `prepare_draw_ex` (3856-4073);
  - `gfx_present.c` 300-354 and 1300-1610;
  - `nvn_api_core.c` 600-697;
  - `nvn_api_obj.c` 755-880.

**By the mapping passes**
- `kernel.c`, `kernel.h`, `svc.c`, `dispatch.c`, `memory.c`, `fs_host.c`, `save_share.c`, `logq.c`, `boot.c`,
  `main.c`, `hostfn.c`, `guest_setjmp.c`, `patches.c`, `platform/common.c`, `platform/win32.c` 1-1717.
- `audren.c`, `audren.h`, `audio_mixer.c`, `audio_capture.c`, `ov_audio_input.c`, `ov_movie.c`, `ov_movie2.c`,
  `ov_media.c`, `movie.h`, `menu_sound.c`, `frame_rate.c`.
- `input.c`, `ov_input.c`, `input_win.c`, `input_sdl.c`, `input_ds3.c`, `input_ds3_win.c`, `motion_fusion.c`,
  `mouse_direct.c`, `camera_speed.c`.
- `discord_rpc.c`, `rtss_limit.c`, `perfmon.c`, `perf.c`, `prof_win.c`, `flight.c`, `tex_astc_mt.c`, `dynres.c`,
  `menu_cursor.c`.
- `gfx_window_win.c`, `gfx_window_sdl.c`, `folder_pick_win.cpp`, `ui/menu_art.cpp`, `ui/menu_win.cpp`.
- `vk_backend.c`, `vk_rec.c`, `vk_rec.h`, `vk_sync.c`, `vk_headroom.c`, `vk_vma.cpp`.
- `vk_usnap.c` (all but ~141-224).

### Read in sections, with searches for threads, locks, allocations and caches

**Large graphics files**
- `vk_memory.c`, `vk_texture.c`, `vk_pipeline.c`, `gfx_shader.c`.
- `vk_exec.c`, `vk_exec_draw.c`, `vk_exec_xfer.c`.
- `gfx_present.c` (1-490, 660-1676).

**Other graphics and front-end files**
- `vk_device.c`, `vk_astc.c`, `vk_gpa.c`, `vk_vaddr.c`, `vk_dump.c`, `vk_dlaa.c`, `vk_fsr.c`, `vk_dlssg.c`,
  `vk_fg.cpp`, `dlssg/nx2_sl.cpp`.
- `nvn_api_cmd.c`, `nvn_api_obj.c`, `nvn_api_core.c`, `nvn_procs.c`, `ov_gfx.c`.

**CPU-side files**
- `ipc.c`, `romfs.c`, `cfg.c`, `ov_zstd.c` (not `decode()`), `ov_system.c`, `net_offline.c`, `amiibo.c`, `nfp.c`,
  `swkbd.c`, `swkbd_win.c`, `cheats.c`, `mods.c`, `loader.c`.
- `ui/menu.cpp`, `menu_assets.c`, `resolution.c`, `ui_text.c`, `audio_effects.c`, `ui/amiibo_page.cpp`.

### Searched only

- `restbl.c`, `save_zip.c`, `input_map.c`, `audio_reverb.c`, `sysinfo.c`.
- `ui/perf_panel.cpp`, `ui/cheats_tab.cpp`.
- `vk_ffx.cpp`, `vk_hdr.c`, `vk_tonemap.c`, `vk_crumbs.c`.
- `nvn_tex.c`, `tex_astc.c`, `tex_deswizzle.c`.
- `platform/win32.c` 1717-2166 (load counters, child processes, IPC, restart).
- The header comment only, for the subsystem map: `vk_caps.c`, `vk_color.c`, `vk_contact.c`, `vk_mvcheck.c`,
  `vk_mvprev.c`, `vk_mvsurvey.c`, `rdoc.c`, `nvn_ext.c`, `gfx_shader_guess.c`, `gfx_null.c`, `widescreen.c`.

### Not opened

**Runtime**
- `platform/linux.c` beyond the thread-creation search, and `platform/fp_x86.h`.
- `vk_formats.c` beyond its header, `fma.c`, `sha256.c`, `folder_pick_sdl.c`, `swkbd_sdl.c`, `input_sdl.h` and the
  other headers not named above.
- `ui/binds_page.cpp`, `ui/option_rows.cpp`, `ui/menu_skin.cpp`, `ui/menu_sdl.cpp`.
- Every `*_probe.c` (probe modes only).
- `runtime/third_party/`, `runtime/shaders/`.

**Outside the runtime**
- `recomp/` (the generator itself).
- The generated C (by the audit's rule).
- `launcher/`, `installer/`, `tools/`, beyond `tools/make_game.py` (searched for the app pool) and `tools/build_game.py`
  (searched).
- `tests/`.
- `tools/xlate/` (the translator lowering and its patches).

### Phase 3 coverage

| Pass | Status |
|---|---|
| 1 Crashes and memory safety | leads done; F-1-01 traced; F-1-11 found and open |
| 2 Threading and races | leads done; overlapping submits confirmed by a run |
| 3 Memory and VRAM budget | leads done by reading; the capped run not done |
| 4 Shader stutter and hitching | leads done; three confirmed by runs |
| 5 Rendering correctness | the three known problems' suspected causes only; nothing run |
| 6 UI | the menu read for clear bugs only; nothing run |
| 7 Performance | leads done by reading; the L-31 A/B not valid yet |
| 8 Edge cases | save atomicity confirmed by the probe; the rest by reading; nothing else run |

**What Phase 3 reviewed for bugs.** It read the code each lead cites, in these files, and no further:

- Passes 1 and 2: `memory.c`, `vk_memory.c`, `vk_sync.c`, `vk_backend.c`, `vk_exec.c`, `nvn_api_core.c`, `nvn_api_obj.c`,
  `nvn_api_cmd.c`, `guest_setjmp.c`, `kernel.c`, `ov_zstd.c`, `ipc.c`, `menu_assets.c`, `camera_speed.c`, `ov_movie.c`,
  `ov_movie2.c`, `frame_rate.c`, `cfg.c`, `vk_pipeline.c`, `vk_texture.c`, `audren.c`, `audio_mixer.c`,
  `platform/win32.c`, `fs_host.c` (the save transaction, read fully by the auditor).
- Pass 3: `vk_texture.c`, `vk_astc.c`, `vk_memory.c`, `vk_headroom.c`, `vk_usnap.c`, `vk_exec_draw.c` (the pool refresh
  and stand-ins), `audio_mixer.c`, `ipc.c`, `net_offline.c`, `platform/win32.c`.
- Passes 4 and 7: `nvn_api_core.c`, `nvn_api_obj.c`, `vk_sync.c`, `vk_backend.c`, `gfx_present.c`, `vk_memory.c`,
  `vk_texture.c`, `vk_exec_draw.c`, `vk_rec.c`, `vk_pipeline.c`, `vk_dlaa.c`, `boot.c`, `ui/menu.cpp`,
  `ui/menu_art.cpp`, `ov_movie2.c`, `input.c`, `prof_win.c`, `perf.c`.
- Passes 5, 6 and 8: `input.c`, `ov_input.c`, `dynres.c`, `resolution.c`, `frame_rate.c`, `mouse_direct.c`, `main.c`,
  `dispatch.c`, `ui/*.cpp`, `menu_assets.c`, `launcher/ui/app.py` (the two exit paths only), and for the known
  rendering problems `vk_texture.c` (aliasing) and `vk_memory.c` (shadow storage).

Still not reviewed for bugs: everything section 9 lists as "not opened", `recomp/`, the generated C, the translator,
and in the files above everything outside the functions the leads cite.

---

## 10. Not determined in Phase 1

**Threads and synchronisation**
- Whether the game submits and presents from the same guest thread. Several sync-mode leads (L-16) depend on it. The
  log line "vk: thread N is the recording thread" and a thread trace in Phase 2 would answer it.
- The threads inside the GPU driver, Streamline/NGX, FidelityFX, SDL, libvpx and dav1d, and what they touch of ours.
- What `plat_cpu_reserve` leaves for the recorder and pipeline threads on the reference PC.
- Which caller thread runs `tex_astc_mt.c`'s first large decode, `dynres_override`, `cheats_frame`, and
  `rt_fatal_note_add`.

**Version and platform**
- Which of the 1.4.3-specific files (`mouse_direct.c`, `menu_cursor.c`, `ov_zstd.c` patch set `zstd`) are active on
  1.1.1.
- Whether the Windows ASan runtime's shadow memory fits beside the fixed guest reservations, and whether TSan on Linux
  accepts them (section 1).

**Memory**
- Real sizes in play: how large the shader memory cache, the pipeline tables, the sampler cache, the lazy write-back
  segment list and the deferred-destruction list get in a multi-hour session.
  - Phase 2 now logs them (`mem: sizes`) and measured them over runs of up to 10 minutes (section 11.3): shader memory
    cache about 3600 entries, 390 MiB; 3260-3360 pipelines; 175-178 samplers; 84-144 lazy ranges; 0-4 deferred
    destructions at the snapshots.
  - Multi-hour growth is still not measured.
- What the front end does when `vk_pool_init` fails for a physical pool.

---

## 11. Phase 2: baseline and instrumentation

Phase 2 was done on branch `claude/audit-phase2-a12eef`, cut from `audit` at `cc179d1d`. It added instrumentation in three
commits and fixed nothing. All the runs were on the reference PC (RTX 5090, driver 621.01), with the runtime of this
branch over the lifted game in `work\lift-pickup`, run from `E:\nx2devrun`.

**The three instrumentation commits.** Each one built and each was shown working by a run. Each costs nothing while
`NX2NAVI_TRACE=perf` is off: a call and a branch per marked site; the size, scene and segment code runs only with the
trace.

| Commit | What it adds | Shown working by |
|---|---|---|
| `c1ed77e4` (c1) | Work marks on any thread (`host.h` `PERF_M_*`, `perf.c` `perf_mark_add`): RomFS reads, zstd decodes, pipeline creates, guest memory maps, pool initialize/finalize, device memory allocations; named on the slow-frame line and per 60-frame window | `E:\nx2devrun\out\audit\c1_check` (exe `ocarina_audit_c1.exe`), Kakariko route |
| `d49a4dea` (c2) | Route segments (input-script `MARK`, `perf.c` `perf_segment`) with mean, 1% low and worst frame, RAM and VRAM; scene lines from area loads (`romfs.c` `scene_note`, `perf.c` `perf_scene`) with a memory snapshot once settled | `c2_check` (`ocarina_audit_c2.exe`), `mark_check.txt` |
| `3a88b363` (c3) | `mem: sizes` lines: entries and bytes of the caches, pools and tables (sixteen `*_sizes` functions, `perf.c` `sizes_log`), every 600 presents and with every snapshot | `probe_u0` (`ocarina_audit_c3.exe`) |

Log lines from those runs:

```
c1: perf: slow frame 2186: 430.8 ms; blocked pipeline 0.0 xlate 0.0 texture 154.6 astc 26.4 fence 0.0 pacing 0.0; on any
    thread: file reads 96.0 ms (597, 97.1 MiB, longest 0.7 ms), zstd decodes 404.2 ms (600, 336.3 MiB, longest 5.9 ms),
    pipeline creates 3070.9 ms (25, 0.0 MiB, longest 463.9 ms), guest memory maps 0.1 ms (1, 5.0 MiB, longest 0.1 ms),
    device memory allocations 99 (5464.9 MiB)
c1: perf: frame 120: work in the window; on any thread: file reads 0.0 ms (1, 0.0 MiB, longest 0.0 ms), pipeline creates
    2118.5 ms (17, 0.0 MiB, longest 342.6 ms), guest memory maps 0.0 ms (1, 0.0 MiB, longest 0.0 ms), pool init/finalize
    179.7 ms (33, 85.0 MiB, longest 16.1 ms), device memory allocations 1 (64.0 MiB)
c2: segment: 'kakariko-walk' frames 2264-4634, world 1.0-25.0 s: 2370 frames in 24.0 s, mean 10.11 ms (98.9 fps), 1% low
    47.2 fps (21.20 ms), worst 43.3 ms at frame 3491; over 25 ms 10, 50 ms 0, 100 ms 0; ram working set 11612 MiB, private
    23683 MiB; vram 8999 of 31419 MiB; on any thread: file reads 304.5 ms (981, 145.4 MiB, longest 10.0 ms), …
c2: scene: KakarikoVillage_Adult, HyruleField_Adult settled (no area loaded for 10 s) at frame 2812, world 7.6 s; ram
    working set 11120 MiB, private 20721 MiB; vram 6650 of 31419 MiB
c3: mem: sizes frame 4200, pipelines and shaders: pipeline entries 3229; with a pipeline 2791; fast links 0; pipeline
    failures 0; pipeline layouts 1249; … pipeline jobs queued or running 438; pipeline keys seen 3259, 0.1 MiB; shader
    memory cache 3531, 384.4 MiB; of it reflection 7.4 MiB; failed shader keys 0/4096; …
c3: mem: sizes frame 4200, GPU memory and textures: pool records 4428; of them physical 1; VRAM mirrors 546, 744.6 MiB; …
    textures 6334; released textures 58, 6.6 MiB; … texture pool slots 32512/32768; sampler slots 242/4096;
```

The c2 run printed the world clock before it starts as `-0.0`; c3 prints `-`.

The c1 and c2 runs are each the first run of a new exe name, so they ran with a cold driver cache. Their large
pipeline-create sums come from that. In the warm baseline runs, `pipeline jobs queued or running` is 0 by the end of
the first area.

**Running the instrumentation.**

- `NX2NAVI_TRACE=perf`, plus `NX2NAVI_PERF_SLOW_MS=25` for a line per frame of 25 ms or more.
- A script line `@<ms> MARK <name>` names the route's segments.
- `NX2NAVI_TEXUP_LOG_MS=16` adds the texture work per slow frame.
- The dev folder's `nx2navi.ini` sets `perf_overlay = full`. Every run here (the earlier ones as well) took the
  overlay's counts and GPU timestamps.

### 11.1 Instrumentation map

Every line below is written only with `NX2NAVI_TRACE=perf` unless the row says "always". "Added" names the Phase 2
commit; the rest existed at `ed36773d`.

**Frame-time markers (what a hitch waited for)**

| Marker the prompt asks for | Log line | Written by | State |
|---|---|---|---|
| Frame time per frame window | `perf: frame N: interval avg … max … (… fps)`, every 60 presents | `perf.c` `perf_present` | existed |
| Frame time over a period | `frametime: 60 s|exit: … mean … fps, 1% low … fps, over 25/50/100 ms`, worst frames with their waits (always) | `perf.c` `ft_report` | existed |
| Frame time per route segment (mean, 1% low, worst) | `segment: '<name>' frames a-b, world …: … mean … ms (… fps), 1% low … fps (… ms), worst … ms at frame …; …; ram …; vram …` | `perf.c` `seg_close` | added (c2) |
| A hitch with its causes | `perf: slow frame N: … ms; blocked pipeline … xlate … texture … astc … fence … pacing …` (frames ≥ `NX2NAVI_PERF_SLOW_MS`, default 50) | `perf.c` `perf_present` | existed (render-thread waits) |
| Pipeline creation on the render thread | `blocked pipeline` in the two lines above | `vk_pipeline.c` → `perf_end(PERF_PIPE)` | existed |
| Pipeline creation on any thread | `; on any thread: … pipeline creates X ms (n, …, longest Y ms)` on the slow-frame line, and `perf: frame N: work in the window; on any thread: …` every 60 presents | `vk_pipeline.c` `pipe_create_in` → `perf.c` `perf_mark_add` | added (c1) |
| Shader translation | `xlate` / `shader xlate` in the perf lines; `shader disk reads` | `gfx_shader.c` → `perf_end(PERF_XLATE)` | existed |
| Texture decode and upload | `texture` / `astc` in the perf lines; `texup: frame N: …` per frame over `NX2NAVI_TEXUP_LOG_MS` (creation, uploads, staging, storage copy, ASTC record, flush, new device memory) | `vk_texture.c` `vk_texstat_frame` | existed |
| File IO | `file reads X ms (n, MiB, longest)` in the any-thread part | `romfs.c` `romfs_read` | added (c1) |
| File IO, which files | `fs <t> rom <path>` per RomFS open (`NX2NAVI_TRACE=fs`) | `romfs.c` `fs_trace` | existed (opens only, no time) |
| Decompression | `zstd decodes X ms (n, MiB out, longest)` in the any-thread part; `zstd: N calls on the host …` running totals (always, every 16384 calls) | `ov_zstd.c` `tracked_decode`, `stream_run`; `count` | added (c1) / existed |
| Large allocations, guest side | `guest memory maps X ms (n, MiB)`: SetHeapSize, Map/UnmapPhysicalMemory | `memory.c` `mem_set_heap_size`, `alias_map` | added (c1) |
| Large allocations, GPU pools | `pool init/finalize X ms (n, MiB)`: NVN memory pool initialize and finalize with their imports, device memory, mirror retire and GPU waits | `vk_memory.c` `vk_pool_init`, `vk_pool_fini` | added (c1) |
| Large allocations, device memory | `device memory allocations n (MiB)`: every block and dedicated allocation VMA makes (no time; only while the headroom callback is installed, `NX2NAVI_VRAM_HEADROOM` default on) | `vk_headroom.c` `on_allocate` | added (c1) |
| Large allocations, host heap | — | — | **missing**: there is no single funnel for the host's `malloc`; the big host allocations are named in section 5 instead |
| GPU time and who bounds the frame | `perf: frame N: gpu avg … ; fence wait …; vram …; ram working set …, private …; bound: cpu|gpu|pace` | `perf.c` `perf_gpu_line` | existed |
| Per-thread samples of a slow frame | `prof: slow frame N (… ms) -> <path>` (`NX2NAVI_PROFILE`, `NX2NAVI_PROFILE_SLOW`) | `prof_win.c` | existed (changes timing: not for the baseline, L-41) |
| Per-frame record of any run part | `NX2NAVI_FLIGHT=1` dumps (`frames.tsv`) | `flight.c` | existed |
| Scene transitions | `scene: <area> loads (frame, world)`; `scene: <areas> settled (no area loaded for 10 s) …; ram …; vram …` followed by the memory snapshot | `romfs.c` `scene_note` → `perf.c` `perf_scene`, `seg_frame` | added (c2) |
| Route segments | input script line `<tick> MARK <name>` or `@<ms> MARK <name>`; `segment: '<name>' starts at frame …` | `input.c` `script_apply` → `perf.c` `perf_segment` | added (c2) |

**Memory figures**

| Figure the prompt asks for | Log line | Written by | State |
|---|---|---|---|
| Process RAM | `ram working set … MiB, private … MiB` in `perf: frame N: gpu …` (every 60 presents) and in every `segment:` and `scene: … settled` line | `perf.c` `perf_gpu_line`, `mem_text` | existed / added (c2) |
| GPU memory | `vram U of B MiB` (driver budget figure, all processes) in the same lines; `mem: vram frame N: …`, `mem: vram by use frame N: …` (textures, released textures, mirrors, physical pools, ASTC scratch, headroom spare, other, free inside VMA blocks, outside VMA incl. swapchain/DLSS/pipelines/driver), heap lines, `mem: pools frame N` | `vk_memory.c` `vram_log`, `vram_use_log`, `heap_log`, `pool_census_log` | existed (every 600 presents) |
| Sampled per scene and after every scene transition | `scene: <areas> settled (no area loaded for 10 s) at frame …; ram …; vram …` followed by the `mem: vram…` lines and the four `mem: sizes` lines; and the same snapshot at every segment's end | `perf.c` `seg_frame`, `seg_close` → `mem_snapshot` → `vk_memory.c` `vk_mem_snapshot`, `perf.c` `sizes_log` | added (c2, c3) |
| Size of each cache and pool | `mem: sizes frame N, <group>: name count[/limit][, MiB]; …` in four groups, every 600 presents and with each snapshot | `perf.c` `sizes_log`, `perf_size`; one `*_sizes` function per subsystem (`memory.c`, `kernel.c`, `guest_setjmp.c`, `fs_host.c`, `romfs.c`, `ov_zstd.c`, `cfg.c`, `vk_memory.c`, `vk_gpa.c`, `vk_texture.c`, `vk_exec_draw.c`, `vk_sync.c`, `vk_usnap.c`, `nvn_api_cmd.c`, `vk_pipeline.c`, `gfx_shader.c`) | added (c3) |
| Commit census (where committed memory lies) | `mem: commit census at frame …` | `prof_win.c` (`NX2NAVI_MEM_CENSUS=1`; it stalls frames by 65-240 ms, so not in the baseline) | existed |

**Section 5's pools and caches against the figures.** By the numbers of section 5:

- *A figure existed before Phase 2:* 60 (census by kind), 61 mirrors, 66 shadow storage (`shadow_log`), 73 deferred
  destructions (count, "releases pending"), 74 headroom spare, 76 VMA blocks and heaps, 77 texture cache bytes (live and
  released), 84 ASTC scratch, 110 DLSS memory (as DLSS reports it), 123 shader memory cache (count only, "shaders: N keys"
  in the perf line), 129 the driver's share (only as "outside VMA").
- *Added in Phase 2 (c3):* 2 guest heap and physical maps, 3 kernel region used and committed, 4 moved stacks, 5 lazy
  write-back ranges (count and the array's room), 7/8 guest threads, 9 handles, 10 fibers, 11 system events, 16 setjmp
  buffers, 21 settings and the replaced values kept, 23 RomFS handles, 25 zstd dictionaries, 26 zstd stream slots, 28/29
  save files and listings open, 60 pool records, 67 write-back snapshot pool, 69 upload batches, 70 staging cache, 72 free
  staging arena chunks, 75 device address records and blocks, 77 texture count, 78 disuse holds, 79 image requirements,
  80 reuse keys, 81 texture and sampler slots, 82 samplers, 87 image views, 86 spare descriptor pools and transient chunks,
  92 held continuations, 93/100 completion queue, 96 free uniform snapshot arenas, 97 uniform snapshot GPU writes, 99
  command buffer slots, 103 recording-buffer pool, 104 texture pins and the sweep queue, 105 recording copies and zombies,
  114 pipeline entries, built pipelines and fast links, 115 layouts, 116 stage libraries, 117 library and pipeline job
  queues, 120 pipeline keys seen, 123 shader memory cache bytes (SPIR-V and reflection), 125 failed shader keys, 127
  translation and load queues, 128 translator processes.
- *Still no size figure:* fixed-size or static tables that cannot grow (1, 6, 13-15, 17, 20, 52, 55-57, 64, 68, 85, 88-90,
  98, 101, 112); transient per-call buffers (30, 32, 63); off by default or debug only (24, 58, 102, 111, 113, 119); on disk
  (19, 31, 118, 121, 126); and these that can grow and have no figure: 12 events, 22 per-thread settings memo, 27
  per-thread zstd contexts, 33 mods, 34 cheats, 35 IPC sessions and domain objects (L-25), 36 offline socket events
  (L-25), 37/38 message queues, 39 per-thread handles (L-26), 40-47 audio tables, rings and change events (L-25), 48-50
  movie and Opus decoders, 51 input script, 53/54 menu art and dictionaries, 59 camera-speed copies, 62 mirror retry
  state, 65 page-generation tables, 71 the staging VMA pool's blocks, 73 the bytes of the deferred destructions, 83
  failed-create ring, 86 live per-command-buffer chunks and pools, 91 BC stand-ins, 94 pending reports, 95 occlusion
  queries, 96 uniform snapshot arenas in use, 106 live-texture copies, 107 GVA ranges, 108/109 swapchain, HDR, FSR and
  tonemap targets (counted only inside "outside VMA"), 122 the large-compute builder's cache, 124 shader use records.

### 11.2 Test route and coverage

Everything the route uses lives outside the repository, under `E:\nx2devrun\out\audit`:

- `route\make_scripts.py` writes the input scripts: `route\route_full.txt` and `route\scene_<name>.txt`.
- `route\baseline.sh` is the batch. It drives `route\arun.sh`, a wrapper around the stripes runner that waits while any other
  game runs.
- `route\baseline_table.py` reads the logs into the tables below.
- `saves\` holds the save folders, copied (read only) from the user's save (`E:\nx2fix\profile-user\save\<user>`) and
  from the test install's profile (`E:\NX2navi-060b\game\profile\save\<user>`):
  - `u0`-`u5` and `d0`-`d5` are the whole folder with slot *k* copied into slot 0.
  - `one_u<k>` and `one_d<k>` hold slot *k* alone.
- `probes\` holds the probes that found where each save loads, and the menu explorations.
- Each run's log and screenshots are in `<tag>\`.

**How the game is driven.**

- The Load screen lists the saves newest first. The title's A presses take the top one.
- DPAD presses in the list are lost while it fades in. So each area is reached with a folder that holds one slot.
- The pause menu:
  - opens on Items;
  - L and R move across Collection, Equipment, Items and System; MINUS opens the map.
  - System lists Save, Load, Options, Special Actions and Title Screen. Its column moves with the left stick, not the
    DPAD.
  - Save opens a dialog with Cancel focused. A "Saved." notice takes one A press of its own.
- The world clock (`frame_rate.c` `world_clock`) starts at the first load and never resets. So one timed script runs
  through a return to the title and a reload.
- An area is seen through RomFS opens. A reload that finds the area still in the game's own cache opens nothing, and
  logs no `scene:` line.
- The title screen's backdrop loads `Market_Adult` (and `HyruleField_Adult`) by itself. Every "scene" line before the
  first save loads is the title's, not the save's.

**Where each save loads** (probe runs `probe_<slot>`, scene lines and screenshots):

| Save | Area (scene lines, then the screenshot) |
|---|---|
| user slot 0 | Hyrule Field at night, before the Market's gate (`HyruleField_Adult`, `Market_Adult`) |
| user slot 1 | Market, adult (ruined, fog) |
| user slot 2 | Kakariko Village by the graveyard path (`KakarikoVillage_Adult`, `Graveyard_Adult`) |
| user slot 3 | Hyrule Field by day (`HyruleField_Adult`, `KakarikoVillage_Adult`) |
| user slot 4 | Market, adult, at night |
| user slot 5 (`townsave`) | Kakariko Village, at the gate |
| test slot 0 | Death Mountain Trail (`DeathMountainTrail_Adult`) |
| test slot 1 | Kokiri Forest, child, 3 hearts. **Loading it crashes** (F-1-01) |
| test slots 2, 4 | Hyrule Field at night, by the Market |
| test slot 3 | Market, adult |
| test slot 5 | Kakariko Village, at the gate |

**Coverage of the prompt's route**

| Route step | Covered | How |
|---|---|---|
| Boot | yes | Every run: segment `boot` (first present to the script's first MARK) |
| File select | yes | Segment `title` (title, Continue, Load list, Load) |
| Kokiri Forest | **no** | The only Kokiri Forest save crashes when it loads (F-1-01). No other save is a child save, and scripted travel there is not possible. |
| Hyrule Field day/night transition | partly | Day (user slot 3) and night (user slot 0) are measured separately. The change itself: `bl_fieldlong`, a 10-minute stand in the day field (see the baseline). |
| Market | yes | User slot 1, adult Market (ruined, fog, ReDeads) |
| A dungeon with heavy effects | **no** | No save stands in a dungeon. Death Mountain Trail (test slot 0) is the heaviest outdoor area reachable, and it is measured as a stand-in, not as a dungeon. |
| A boss | **no** | No save is at a boss, and scripts cannot fight one |
| Pause menu and every subscreen | yes | Every run: Items, Equipment, Collection, Items, System, the map |
| Save, quit, reload | yes | `route_full.txt`: System → Save → Title Screen → Continue → the new save → Load, in one process |
| Return to the first scene | yes | `route_full.txt` walks Kakariko Village again after the reload (segment `kakariko-return`, from the save point at the gate, so the path differs from the first walk) |
| Added: Kakariko Village | yes | The town where players report slowdowns (section 1); the route's first scene |
| Added: Graveyard edge | yes | User slot 2 |

### 11.3 Baseline

**Settings and method.**

- Runs: `route\baseline.sh`, exe `ocarina_audit_c3.exe` (commit `3a88b363`).
- Every run starts a fresh process on a fresh copy of its save.
- Settings A, the earlier town measurements' settings: `FPS_LIMIT=unlimited FRAMEGEN=0 HDR=0 GAME_ASPECT=16:9`, DLSS
  performance from the ini, window 2560x1440.
- Settings U, the user's own: `FPS_LIMIT=120 FRAMEGEN=dlss FRAMEGEN_MULTIPLIER=2 HDR=1`.
- Two runs of each set. Each run is r1 or r2.
- All runs are warm. The exe had run more than 20 times before, so the driver cache was warm; the shared
  `pipeline-cache.bin` and shader cache were warm too.
- r1 is compared with r2, never with the cold c1/c2 runs.

**Columns.**

- Frames over 25 ms are counted.
- RAM is the process's private bytes. The working set runs 10.1-11.2 GiB in the areas.
- VRAM is the driver's figure for the device-local heaps (`vk_mem_vram`) at the segment's end.
- "1% low" is the frame time at the slowest 1%, given as fps (`perf.c` `seg_close`, 50 us bins).

**Segments.**

- *Route* (`route_full.txt`, save `u5`):
  - `title`: from the title to the world clock's start, so it includes the first save load;
  - `kakariko`: 46 s walking the village;
  - `pause-menu`: every subscreen;
  - `save`;
  - `title-return`;
  - `reload`: the title, the Load list and the load;
  - `kakariko-return`: 45 s walking from the save point.
- *Scene runs* (`scene_<name>.txt`, one save each):
  - `-settle`: 10 s standing right after the load;
  - `-camera`: 13 s of camera turns;
  - `-walk`: 9 s out and back;
  - `-pause`: the subscreen tour.
- *Boot* (first present to the title's first MARK, about 2 s). Settings A: 227.3 / 210.9 fps, 1% low 90.9 / 77.8,
  worst 45.8 / 46.7 ms (one frame over 25 ms: the boot's pool initialize, 43 ms in `vk_pool_init`), private 11.4 GiB,
  VRAM 964 MiB. Settings U: 118.9 / 119.2 fps, worst 53.4 / 50.7 ms.

| Run set | Segment | Frames r1 / r2 | Mean fps r1 / r2 | 1% low fps r1 / r2 | Worst ms r1 / r2 | Frames >25 ms r1 / r2 | RAM private MiB r1 / r2 | VRAM MiB r1 / r2 |
|---|---|---|---|---|---|---|---|---|
| route | title | 1414 / 1480 | 108.1 / 110.8 | 51.8 / 50.4 | 294.6 / 274.2 | 7 / 7 | 19901 / 20057 | 6546 / 6670 |
| route | kakariko | 6541 / 6561 | 140.7 / 141.1 | 90.9 / 92.2 | 15.0 / 14.1 | 0 / 0 | 24160 / 23891 | 10005 / 9634 |
| route | pause-menu | 4911 / 4914 | 223.2 / 223.4 | 152.7 / 149.3 | 20.7 / 20.6 | 0 / 0 | 21863 / 21796 | 8020 / 7906 |
| route | save | 3356 / 3357 | 240.0 / 240.0 | 215.1 / 217.4 | 10.5 / 9.5 | 0 / 0 | 21689 / 21638 | 7862 / 7762 |
| route | title-return | 2254 / 2258 | 205.1 / 205.3 | 59.5 / 59.7 | 20.2 / 17.9 | 0 / 0 | 21666 / 21752 | 7726 / 7754 |
| route | reload | 2830 / 2790 | 113.0 / 111.4 | 56.5 / 54.5 | 92.5 / 97.6 | 6 / 10 | 21778 / 21725 | 7916 / 7814 |
| route | kakariko-return | 7290 / 6186 | 160.2 / 136.0 | 98.5 / 76.6 | 12.3 / 64.8 | 0 / 6 | 25770 / 22599 | 11471 / 8596 |
| route_user | title | 1014 / 1028 | 76.8 / 78.2 | 46.5 / 46.0 | 290.6 / 279.9 | 8 / 8 | 20525 / 20603 | 7175 / 7239 |
| route_user | kakariko | 5494 / 5397 | 118.2 / 116.1 | 92.2 / 85.8 | 13.9 / 15.0 | 0 / 0 | 24070 / 23620 | 9942 / 9499 |
| route_user | pause-menu | 2640 / 2640 | 120.0 / 120.0 | 107.0 / 106.4 | 20.4 / 20.8 | 0 / 0 | 22122 / 22405 | 8349 / 8577 |
| route_user | save | 1680 / 1680 | 120.0 / 120.0 | 114.3 / 114.3 | 9.5 / 9.4 | 0 / 0 | 22048 / 22134 | 8279 / 8320 |
| route_user | title-return | 1207 / 1207 | 109.8 / 109.8 | 59.5 / 59.5 | 22.1 / 18.0 | 0 / 0 | 22139 / 22222 | 8262 / 8309 |
| route_user | reload | 2131 / 2106 | 85.2 / 84.2 | 57.5 / 41.4 | 52.1 / 54.1 | 3 / 19 | 22523 / 22576 | 8730 / 8725 |
| route_user | kakariko-return | 5083 / 5394 | 111.7 / 118.6 | 75.8 / 92.2 | 81.6 / 17.4 | 4 / 0 | 23321 / 25010 | 9360 / 10945 |
| market | market-settle | 1510 / 1573 | 151.2 / 157.5 | 76.6 / 77.2 | 15.6 / 18.7 | 0 / 0 | 19441 / 19349 | 6414 / 6305 |
| market | market-camera | 2255 / 2264 | 173.4 / 174.1 | 114.3 / 117.0 | 16.4 / 12.3 | 0 / 0 | 19744 / 19549 | 6436 / 6370 |
| market | market-walk | 1325 / 1314 | 147.1 / 146.0 | 78.7 / 79.1 | 17.7 / 17.6 | 0 / 0 | 19760 / 19586 | 6436 / 6372 |
| market | market-pause | 4713 / 4769 | 219.2 / 221.8 | 108.1 / 130.7 | 22.3 / 19.8 | 0 / 0 | 20325 / 20247 | 7019 / 7017 |
| fieldday | field-day-settle | 1282 / 1265 | 128.3 / 126.5 | 101.0 / 101.5 | 10.8 / 10.8 | 0 / 0 | 20487 / 20373 | 7015 / 6951 |
| fieldday | field-day-camera | 1825 / 1799 | 140.4 / 138.4 | 107.5 / 104.7 | 19.6 / 10.5 | 0 / 0 | 20677 / 20656 | 7045 / 7045 |
| fieldday | field-day-walk | 1228 / 1223 | 136.5 / 135.8 | 77.2 / 84.7 | 18.9 / 18.4 | 0 / 0 | 21535 / 21250 | 7815 / 7581 |
| fieldday | field-day-pause | 4704 / 4702 | 218.8 / 218.7 | 125.0 / 124.2 | 20.3 / 20.5 | 0 / 0 | 21909 / 21749 | 8244 / 8108 |
| fieldnight | field-night-settle | 1139 / 1111 | 113.9 / 111.1 | 97.6 / 94.8 | 11.7 / 12.0 | 0 / 0 | 20187 / 20283 | 6697 / 6695 |
| fieldnight | field-night-camera | 1748 / 1745 | 134.5 / 134.2 | 97.6 / 96.6 | 11.4 / 12.6 | 0 / 0 | 24265 / 24308 | 10283 / 10298 |
| fieldnight | field-night-walk | 1033 / 1049 | 114.8 / 116.6 | 83.3 / 96.2 | 34.4 / 12.2 | 1 / 0 | 23133 / 22958 | 8744 / 8719 |
| fieldnight | field-night-pause | 3643 / 3762 | 169.5 / 175.0 | 75.5 / 97.6 | 34.4 / 33.4 | 2 / 2 | 21686 / 21581 | 7783 / 7775 |
| trail | trail-settle | 1666 / 1662 | 166.6 / 166.1 | 133.3 / 131.6 | 10.1 / 8.6 | 0 / 0 | 19155 / 19151 | 5933 / 5933 |
| trail | trail-camera | 2057 / 2131 | 158.2 / 164.0 | 79.4 / 132.5 | 38.2 / 9.3 | 1 / 0 | 19638 / 19471 | 6202 / 6203 |
| trail | trail-walk | 1464 / 1459 | 162.6 / 162.1 | 131.6 / 128.2 | 8.8 / 8.7 | 0 / 0 | 19966 / 19793 | 6503 / 6495 |
| trail | trail-pause | 4795 / 4803 | 223.1 / 223.4 | 146.0 / 149.3 | 20.9 / 21.8 | 0 / 0 | 20290 / 20147 | 6902 / 6842 |
| graveyard | graveyard-settle | 1100 / 1091 | 110.0 / 109.1 | 89.3 / 86.2 | 13.7 / 12.9 | 0 / 0 | 19659 / 19659 | 5983 / 5983 |
| graveyard | graveyard-camera | 1776 / 1774 | 136.6 / 136.5 | 96.2 / 96.2 | 11.3 / 11.7 | 0 / 0 | 19977 / 19932 | 6232 / 6179 |
| graveyard | graveyard-walk | 1014 / 1018 | 112.8 / 113.1 | 61.9 / 66.0 | 21.5 / 17.9 | 0 / 0 | 20481 / 20591 | 6703 / 6790 |
| graveyard | graveyard-pause | 4633 / 4623 | 215.5 / 215.0 | 105.3 / 106.4 | 21.6 / 21.3 | 0 / 0 | 21065 / 21130 | 7273 / 7321 |


`route_user` is settings U, capped at 120. With frame generation the window shows twice the frames; the table counts
the frames the game draws.

**The day/night change.** `bl_fieldlong`: user slot 3, 9.7 minutes of world time standing in Hyrule Field by day.

- By the end the light reached dusk, not night. The full change is not covered.
- Over the stand the mean fell from 140 fps (`field-day-camera`) to 118 fps (`field-day-end`, 519 s, 61244 frames, 1%
  low 96.2 fps, worst 16.2 ms, none over 25 ms).
- The GPU's time stayed at 4.36-4.37 ms per frame, and every window was "bound: cpu".
- The CPU frame grew by about 1.3 ms. The size lines at frames 13200 and 73200 are flat apart from released textures
  (165 → 542, 36 → 478 MiB). Lead L-45.

**Memory in the areas** (segment ends, r1; private RAM / VRAM in MiB):

| Area | Private RAM | VRAM |
|---|---|---|
| Market | 19.4-20.3 GiB | 6.3-7.0 GiB |
| Death Mountain Trail | 19.2-20.3 GiB | 5.9-6.9 GiB |
| Graveyard edge | 19.7-21.1 GiB | 6.0-7.3 GiB |
| Hyrule Field by day | 20.4-21.9 GiB | 7.0-8.2 GiB |
| Hyrule Field by night | 20.2-24.3 GiB | 6.7-10.3 GiB |
| Kakariko | 23.9-24.2 GiB | 9.6-10.0 GiB |

The `scene: … settled` snapshots are in each log.

**Sizes at the end of the first scene and after returning to it** (route, settings A, `route\summary.md` has the full
lines):

| Figure | r1 Kakariko | r1 after the reload | r2 Kakariko | r2 after the reload |
|---|---|---|---|---|
| VRAM in use (driver) | 10005 MiB | 11471 MiB | 9634 MiB | 8596 MiB |
| of it textures / released textures | 4731 / 2580 | 4908 / 3732 | 4673 / 2262 | 4996 / 892 |
| VRAM mirrors | 783 (619 pools) | 802 (577) | 783 (619) | 1206 (948) |
| outside VMA (swapchain, DLSS, pipelines, driver) | 1218 | 1134 | 1146 | 704 |
| Private RAM | 24160 | 25770 | 23891 | 22599 |
| Pool records / device address records | 4516 / 4515 of 65536 | 4600 / 4599 | 4516 / 4515 | 5145 / 5144 |
| Textures / image views | 8292 / 19194 | 9056 / 20833 | 8374 / 19305 | 8554 / 18666 |
| Texture pool slots | 32512 of 32768 | 32512 | 32512 | 32512 |
| Recording copies | 51656, 12.6 MiB | 77724, 20.3 MiB | 51721, 12.6 MiB | 82842, 22.6 MiB |
| Texture pins / pin sweep queue | 9076 / 9056 | 12321 / 12303 | 9126 / 9106 | 8460 / 8432 |
| Completion queue | 618 | 457 | 678 | 579 |
| Pipeline entries (all built) / layouts | 3258 / 886 | 3258 / 886 | 3354 / 894 | 3354 / 894 |
| Shader memory cache | 3564, 386.2 MiB | 3567, 386.2 MiB | 3657, 394.4 MiB | 3676, 395.4 MiB |
| Guest heap / physical maps | 6910 / 2674.5 MiB | 6910 / 2674.8 | 6910 / 2674.1 | 6910 / 2668.1 |
| Handles / system events / setjmp buffers | 112 / 8 / 10 | 116 / 10 / 22 | 112 / 8 / 8 | 117 / 10 / 23 |
| Lazy write-back ranges | 111 | 84 | 126 | 102 |

Unchanged everywhere:

- 84-85 guest threads, 3 fibers, 0 deferred destructions at the snapshot;
- staging cache 24 of 24, 8 free arena chunks, 17 of 128 write-back snapshot chunks;
- 0 shader failures, 0 translator processes, 0 pipeline jobs pending, 0 fast links;
- 4 zstd dictionaries, 21 settings, no replaced setting values.

Growth across the return, in both runs: setjmp buffers (+12/+15, never removed, L-10), system events (+2), recording
copies (+26k/+31k). The VRAM figures differ between r1 and r2 because releases and mirror evictions fall at different
times.

**Hitches and their causes.** `route\slow_causes.py` over all 15 baseline logs:

- 287 frames of 25 ms or more.
- 132 have a render-thread wait or a work mark of at least a quarter of the frame. Mostly texture upload with zstd
  decodes and file reads at an area load, fence waits right after it, and pool initialize/finalize.
- 30 have something named, but less.
- 125 have nothing named:
  - 95 are the Market's periodic stall: 90 in r1, 5 in r2 (L-44);
  - 20 are the one or two frames right after each save load, in every run: 48-68 ms, then 26-36 ms;
  - 10 are single frames of 25-34 ms: 9 during the route's reloads, 1 in the night field.

The unnamed time is outside the recorded waits and marks: the game's own threads or the driver. `NX2NAVI_PROFILE` with
`NX2NAVI_PROFILE_SLOW` can name them. It suspends every thread every millisecond (L-41), so it stayed out of the
baseline.

**Other baseline observations** (recorded, not investigated):

- The menu segments run at exactly 240.0 fps with `FPS_LIMIT=unlimited`: the game step's limit (`ostep`, up to 240
  game frames a second).
- In r2 the reload's area streaming ran into the `kakariko-return` segment. That gave 6 frames over 25 ms (worst
  64.8 ms: texture 26.2 ms, 1563 MiB of new device memory), against 0 in r1. The segment's mean depends on when the load
  ends.

### 11.4 Sanitizers

| Build | Built | Ran | Reported |
|---|---|---|---|
| UBSan (`-fsanitize=undefined -fsanitize-trap=undefined -fno-sanitize=vptr -g1`, runtime only) | yes: `work\build_rt_audit.py` with `AUDIT_CFLAGS`, `work\rt_obj_ubsan`, `ocarina_ubsan.exe` | yes, the full route | one report, then a clean route (below) |
| ASan + UBSan (`-fsanitize=address` added, linked to `clang_rt.asan_dynamic-x86_64.lib`) | yes: `work\rt_obj_asan`, `ocarina_asan.exe` | no: it cannot start | — |
| TSan | no | no | — |

**What was built and how.**

- The MSYS2 UCRT64 clang (23.1.4) ships no sanitizer runtimes. `lib\clang\23` holds only headers, and the
  compiler-rt package would have to be downloaded, which was not done.
- UBSan therefore runs in trap mode: every check is an `ud1` instruction whose operand names the check. The crash
  handler logs the address, and `addr2line` on the `-g1` exe gives the line (`route\ubsan_once.sh`).
- The standalone LLVM 23.1.5 for the MSVC target (`C:\Program Files\LLVM`) carries the ASan runtime DLL. A MinGW UCRT
  program linked against its import library works: a test program (`E:\nx2devrun\out\audit\san\t.c`) had its heap overflow
  reported in full.
- Only `runtime/` was instrumented. The guest objects in `work\obj_pickup` are the uninstrumented ones (the generated
  C is not built again).

**UBSan results.**

1. Run `san_ubsan`: a trap at start-up, in the menu thread's font atlas.
   - `FATAL: illegal instruction (0xc000001d) at host … (nx2navi+0x11782fd2)`.
   - The instruction is `ud1 0xc(%eax),%eax`: check 12, multiply overflow.
   - `addr2line`: `ui/menu_skin.cpp:265`, the `lattice` lambda in `skin_atlas_fill`.
   - Ledger F-1-02.
2. With that one check skipped for that file (`work\asan\ubsan_ignore.txt`), run `san_ubsan2` went through the whole
   route (boot, title, Kakariko, every pause subscreen, save, title, reload, Kakariko, 282 s of world time) with no
   further trap.
   - UBSan stops at its first report. So this says: no other undefined behaviour of the instrumented kinds was reached
     on this route in `runtime/`.
   - Its cost: Kakariko 119.5 fps against 140.7.

**ASan result.** It cannot run with the guest's fixed layout.

- Started directly (`ocarina_asan.exe --version` with the DLL on `PATH`), the runtime wrote "memory: a fixed region is
  taken; re-running the process" 20 times, then `FATAL: memory: the module window is not free after 20 attempts`
  (exit 70). This is `memory.c` `mem_init` → `reexec_or_fail`.
- A probe under the same ASan DLL (`E:\nx2devrun\out\audit\san\vq.c`) shows why:
  - the shadow starts at `0x80000000`;
  - one reservation from allocation base `0x90000000` covers `0x100000000` to past `0x7010000000` (2046 GiB from
    `0x100000000`, 1602 GiB from `0x7000000000`);
  - so the module window (`MOD_MAIN_BASE`, `0x7000000000`) and the kernel region (`0x7010000000`) lie inside ASan's
    shadow.
- The Windows ASan runtime chooses the shadow's place itself, and no option moves it. ASan needs the guest windows
  moved above the shadow, which `recomp/layout.py` decides (a generator change across subsystems). Not attempted;
  waiting for the user if wanted.
- The first `ASAN_OPTIONS` tried had a log path with a drive letter, whose colon the option parser takes as a
  separator ("expected '='"). A path relative to the game folder works.

**TSan.**

- ThreadSanitizer has no Windows target.
- No Linux machine is here, and WSL is not installed ("The Windows Subsystem for Linux is not installed").
- Not built, not run.

### 11.5 What Phase 2 could not do

- **Route steps:**
  - Kokiri Forest is not covered: its only save crashes on load (F-1-01).
  - Dungeon and boss: no save is in one, and scripted input cannot travel or fight there.
  - The full day/night change: only dusk was reached in 9.7 minutes.
- **Saves:** no save of the user's loads as a child. The probes used copies of the user's slots (read only) and of the
  test install's.
- **Sanitizers:** ASan cannot start (shadow over the fixed guest layout); TSan does not exist here. UBSan stops at its
  first report, so each further report needs a rebuild with an ignore list.
- **Instrumentation not added:**
  - a host `malloc` funnel for "large allocations" (there is none; the large host allocations are named by subsystem
    instead);
  - sizes for the items listed under "still no size figure" in 11.1;
  - the game's own threads: their CPU time in a frame stays unnamed, as above.
- **Not measured:**
  - the commit census (`NX2NAVI_MEM_CENSUS=1`), which stalls frames by 65-240 ms;
  - long sessions: the longest run is the 10-minute field stand; the multi-hour growth that section 10 asks about is
    not measured;
  - an AMD or Linux machine (not available).

---

## 12. Phase 3: the eight passes (stopped before its end)

Phase 3 was started on 2026-11-10 on branch `claude/audit-phase2-a12eef` and stopped at the user's request before its
end. It changes no runtime behaviour: the four commits it adds only log under `NX2NAVI_TRACE=perf`. Nothing is fixed.

**Where it stands.**

- Every lead L-01 to L-47 has an outcome (section 7, in italics after each number).
- The ledger (section 8) holds 56 findings: 11 confirmed by a run or by tool output, 45 suspected from reading code. (F-1-12 was added during Phase 4.)
- Not done: the runs listed in 12.4. Until they are done, the findings they would settle stay suspected.

| Severity | Confirmed | Suspected |
|---|---|---|
| S1 crash, data loss | 4: F-1-01, F-1-11, and the measured growth of F-1-07 and F-1-08 (their fatal was not reached) | 8: F-1-03, F-1-04, F-1-05, F-1-06, F-1-09, F-1-10, F-2-04, F-8-02 |
| S2 hitch, growth, major visual | 3: F-4-01, F-4-04 (the time), F-4-11 | 7: F-2-01, F-2-05, F-3-01, F-3-02, F-3-03, F-5-02, F-8-01 |
| S3 minor | 2: F-4-02, F-4-03 | 22 |
| S4 optimization | 2: F-1-02, F-3-12 | 7 |

Of the eight suspected S1 rows, five need the game itself to misuse a handle or two guest threads to meet in a narrow
window (F-1-03, F-1-04, F-1-06, F-1-09, F-2-04), and none of them was seen in any run.

### 12.1 How it was done

- **Reading.** Four read-only passes went through the leads, one group of passes each: passes 1 and 2; pass 3; passes 4
  and 7; passes 5, 6 and 8. Each lead ended as a finding, a strike with its reason, or "needs a run".
- **Checked again by the auditor.** These claims were re-read in the code before they were entered: F-1-03
  (`kern_handle_get`), F-1-04 (`stream_drop`), F-1-05 (`same_content`), F-1-06 (`vk_exec_held_ready`, `ph_flush`),
  F-1-09 (`read_directory`), F-2-04 (`save_commit`, `commit_run`), F-3-01 (`upload`), F-3-02 (`make_dummy`), F-3-03
  (`refuse`), F-4-04 (`pool_fini`), F-4-11 (the marker's callers), F-6-02 (`play_only`), F-8-01 (`gen_now`), F-8-02
  (the callers of `fs_save_flush`). The other suspected rows and the strikes rest on the reading passes alone.
- **Runs.** Exe `ocarina_audit_p3.exe` (commit `a1eebd03`), batch `E:\nx2devrun\out\audit\route\p3_runs.sh`, runs
  `p3_*` under `E:\nx2devrun\out\audit`. The profiler was used once to name a cause, never to measure.
- **Logs read for evidence.** The 54 logs under `E:\nx2devrun\out\audit`, the five logs of the user's install
  (`D:\test\logs`, read only) and five earlier copies of the user's logs. None holds the lines that F-3-01, F-3-02 or
  F-3-03 would write.

### 12.2 Instrumentation added in Phase 3

All four write only with `NX2NAVI_TRACE=perf`.

| Commit | What it adds | Shown working by |
|---|---|---|
| `df6766aa` | `drain` and `replay` on the `perf: slow frame` line (`perf.c` `perf_present`) | `p3_route`: `perf: slow frame 2122: 95.0 ms; blocked pipeline 0.0 xlate 0.0 texture 0.0 astc 0.0 fence 0.0 pacing 0.0 drain 0.0; replay 93.6; on any thread: pipeline creates 561.6 ms (8, 0.0 MiB, longest 223.0 ms)` |
| `c85892ab` | Work mark `DLSS feature creates` (`host.h` `PERF_M_NGX`, `vk_dlaa.c` `evaluate` and the frame generation create) | `p3_route`: `perf: slow frame 20156: 108.7 ms; … replay 104.7; on any thread: pipeline creates 562.2 ms (4, 0.0 MiB, longest 294.7 ms), device memory allocations 16 (797.8 MiB), DLSS feature creates 43.8 ms (1, 0.0 MiB, longest 43.8 ms)` |
| `a1eebd03` | Which threads submit and present on which queue, and a line when such a call starts while another runs (`nvn_api_core.c` `nvn_call_note`) | `p3_route`: `nvn: thread 22980 presents on queue 0x134133020 (first at frame 0)`; `nvn: thread 36088 enters a submit on queue 0x134133020 while 1 other submit or present call run (overlap 1, frame 8086)` |
| `587e8494` | `page generations handed out` in the `mem: sizes` lines (`vk_memory.c` `vk_mem_sizes`) | Built (`ocarina_audit_p3b.exe` holds the text); **not yet shown by a run** |

### 12.3 Evidence

**F-1-01, the save that crashes on load** (run `f101_fs`, `NX2NAVI_TRACE=perf,fs`).

- The save's `progress.sav` names `HyruleCastle_Child` twice. The other 11 saves name `HyruleCastle`.
- The 1.1.1 RomFS index has `Banc/HyruleCastle/HyruleCastle_Static` and `_Dynamic`, and `GameBancParam/HyruleCastle`. It
  has no `HyruleCastle_Child` file. (Sixteen other areas do have `_Child` files.)
- The file trace, in order:
  - `fs    8.028 rom content:/Banc/HyruleCastle/HyruleCastle_Child_Static.bcett.byml.zs` (twice), then the same name
    without `.zs`, then the `_Dynamic` pair the same way;
  - `fs   15.134 rom content:/Banc/GameBancParam/HyruleCastle_Child.game__banc__GameBancParam.bgyml`;
  - then `FATAL: access violation … reading address 0x150` in `main+0x112b3b0`.
- None of those opens logged a `scene:` line, which `romfs.c` `romfs_open` writes only after the file is found: the host
  answered "not found", which is right.
- So the host left nothing null. The save names an area this game version does not have, and the game does not check.
  The first 12 bytes of the save match every other save here; where the save came from is not determined.

**Interrupted save writes** (pass 8). `ocarina_audit_c3.exe --probe-fs`, in a temporary profile:

```
save tx: uncommitted writes visible to the game and dropped by a restart and by Unmount, commits kept; a crash at each
of 6 commit steps recovers to the old save 3 times, the new one 3 times, mixed 0 times; 3 backups
probe-fs: passed (186587 files, 64 sampled)
```

A commit interrupted at any of its six steps leaves the whole old save or the whole new one. What is left open is the
window before the commit thread has finished (F-8-02) and two Commits at once (F-2-04).

**F-4-01, the Market stall** (lead L-44; runs `p3_market_t1`, `bl_market_1`, `bl_market_2`).

- With the replay column, every slow frame is the replay: `slow frame 11303: 20.3 ms; … drain 0.0; replay 18.0`.
  375 of 380 gaps between slow frames are 8 frames.
- `NX2NAVI_REPLAY_TIME=1`: `slow replay frames 225, per frame: draws 23.2 ms (1253) … its slowest op 20.0 ms (DRAW_MESH
  225); texture lock held over 1 ms 19.9 ms`.
- `NX2NAVI_LOCK_TIME=1`: `texture lock held over 1 ms: 7792 ms (581 holds): 00007FF67A6C43C0 7791.1 ms (580, max 33.61
  ms)`. Against the link map that address is `bind_resources+0x1980`. A game thread waits behind it:
  `vk_texture_register+0x2c 768.9 ms (1963, max 26.81 ms)`.
- The list walked there, from the `vk: texture pool sets:` line every 3600 pool switches (`slots without one`, and the
  walk's own time per switch):

  | Run | at 10800 | 14400 | 18000 | 21600 | 25200 | 28800 |
  |---|---|---|---|---|---|---|
  | `p3_market_t1` | 22149, 0.070 ms | 5063, 0.082 | 134371, 0.148 | 111986, 0.249 | 160385, 0.324 | — |
  | `bl_market_1` | 24323, 0.041 | 13506, 0.076 | 67223, 0.141 | 115652, 0.226 | 72314, 0.281 | 87258, 0.326 |
  | `bl_market_2` | 49739, 0.034 | 6067, 0.073 | 70646, 0.119 | 62904, 0.188 | 77066, 0.245 | 88824, 0.301 |
  | `p3_market_t2` (no stall) | 31397, 0.073 | 2792, 0.077 | 1297, 0.065 | 1300, 0.056 | 1273, 0.050 | 1327, 0.045 |

  Only 32768 slots exist, so a list of 160385 holds copies. In `p3_market_t1` the walk time grew from 0.249 to 0.324 ms
  per switch over 3600 switches with 0.05 walks per switch: about 15 ms a walk.
- Elsewhere the list stays small: 203-4167 in Kakariko (`bl_route_1`), 1058-2126 in the field (`bl_fieldlong`).
- The profiled run (`p3_market_prof`) caught the stall at its end (frames 14239, 14247, 14255) but its 64 slow-frame
  dumps were used up during boot, and the whole-run profile is 82 % blocked threads: it adds nothing to the above.

**F-4-02 and F-4-03, the frames after a load** (lead L-47).

- DLSS feature creates, from the new mark: 23.5 ms and 26.7 ms in the load's big frame (`p3_route` 2111,
  `p3_route_cold` 2178); 16.4 and 15.6 ms at the pause menu's first frame (`p3_market_t1` 5663, `p3_market_t2` 5742:
  the menu draws at its own size, `DLSS feature created at 2560x1440 -> 2560x1440`); 43.8 and 44.9 ms at the reload,
  where `1 DLSS feature(s) released before the one for 1280x720 is made` comes first.
- The unnamed 48-68 ms frame of the baseline is a different frame, about 10 after the load's big one. It is the replay's
  own time (the `df6766aa` line above), and its part is not named yet.

**F-2-01, which threads submit** (this also answers section 10's first question).

- `p3_route`: thread 28188 submits on three queues; thread 22980 submits on four and is the only one that presents;
  thread 17368 submits on the present queue from frame 1079; thread 36088 from frame 8086.
- Frame 8086 is the pause menu's first frame (the segment starts at 8080). From there submits overlap: 32 or more
  overlaps by frame 12847, and the same from the pause menu's first frame in both Market runs.
- So the game submits and presents from the same thread, and other threads submit at the same time.

**L-45, the frame that grows toward dusk.** In `bl_fieldlong` the replay stays at 5.46-5.59 ms while the interval goes
from 7.96 to 8.5 ms, and the pool's freshness check stays at 0.034 ms with 6019 to 6825 listed entries. The growth is
outside the replay: the game's own frame, or host calls it makes. The night field's baseline is slower than the day
field's in the same way (111-114 fps standing against 126-128). The cause is not determined.

**F-4-11 and F-1-11, the run folder's pipeline state and three device losses.**

- At 22:50 `E:\nx2devrun\pipeline-prewarm-skip.txt` appeared (the ledger row quotes the line that wrote it). From then
  on every start in that folder, of every session, skips the boot's pipeline prebuild.
- The audit did not delete the file: other sessions run there, and removing it was the user's call. Its own text says
  "Delete this file to build ahead of play again." On 2026-11-11 the user said to delete it; by then it was already
  gone from `E:\nx2devrun` (who removed it, and when, is not known), so starts there build ahead of play again. The
  copy in `out\audit\pcache_old` is the F-4-11 reproduction's own product and stays as its evidence; only starts that
  name that folder as their pipeline cache read it.
- Runs before 22:50 (`p3_route_cold`, `p3_route`, `p3_market_t1`, `p3_market_t2`) are in the earlier state. Runs after
  it (`p3_market_prof`, `p3_l31_*`) build pipelines in play and are slower: 99.9 fps standing in the day field against
  126-128 in the baseline. They cannot be compared with the baseline.
- In that state three runs in a row lost the device while the save loaded (F-1-11). All three show the same last GPU
  mark, `dispatch 3 of submission N: 293x1x1 groups, cs 3524d41df45a41e0`, and a read fault at `0x30b000000`,
  `0x311000000` or `0x31f000000`.

### 12.4 What is left of Phase 3

- **F-7-01 (L-31), the two GPU waits.** A valid A/B: default, `NX2NAVI_SUBMIT_FINISH=0` and `NX2NAVI_ACQUIRE_WAIT=0`,
  two runs each, in one pipeline state. `route\l31_table.py` reads them.
- **F-8-01, the generation counter's rate.** One route run with `ocarina_audit_p3b.exe`, reading `page generations
  handed out` at two 600-frame lines.
- **F-3-01, F-3-02, F-3-03, F-7-03 (L-21, L-22, L-23, L-35).** The route under `NX2NAVI_VRAM_BUDGET_MB=6144` with a long
  stand, for the disuse eviction and the scans. A budget cap cannot fail an allocation, so F-3-01 and F-3-03 need real
  memory exhaustion to reproduce.
- **F-1-11.** Name the faulting read, and say whether the skipped prebuild causes it.
- **F-4-03.** A timed route run (`NX2NAVI_REPLAY_TIME=1`) to name the replay's part in the frame after a load.
- **Not reachable here, so not determined:** F-1-10 (dungeon and boss audio), F-5-01 (the gate), F-5-02 (Linux, AMD),
  F-8-03 and F-8-04 (a hand on the controller), F-8-06 (a fused item), F-8-07 (a movie).
- **Not looked at:** pass 5 beyond the three known problems, and pass 6 beyond a read of the menu for clear bugs. The
  game's own pause menu ignores the DPAD in its System column under scripted input; whether that is the game's own
  behaviour was not checked.

---

## 13. Phase 4: fixes

Started 2026-11-10 at the user's word ("fix what was found"), on `claude/audit-phase2-a12eef`. One commit per fix. Nothing
is pushed.

**Rules kept.**

- A fix that would change gameplay timing or behaviour, the save format, or cross subsystems was not made; those wait
  for the user (13.3).
- A suspected finding that was never reproduced cannot be shown fixed. Its row says "not reproduced before" and names
  the regression checks that passed with the change.
- Exe: `ocarina_audit_fix.exe`, built from `d61cd9b9`. Its runs use a pipeline cache folder of their own
  (`NX2NAVI_PIPELINE_CACHE`, `E:\nx2devrun\out\audit\pcache`), so they neither depend on the shared folder's skip file
  nor change it. Batch: `E:\nx2devrun\out\audit\route\p4_verify.sh`.

### 13.1 What was changed

The ledger's last column (section 8) has each commit with its evidence. In short:

| Finding | Commit | Change | Verified |
|---|---|---|---|
| F-4-11 prebuild skipped for good | `6c0b0b10` | Every orderly exit deletes the boot prebuild's running marker | yes: reproduced with the old exe, gone with the fixed one |
| F-4-01 Market stall | `015a89dc` | A pool slot without an entry is listed once | yes: three runs of three: the list at 3192 entries at most, no frame over 20 ms standing |
| F-1-01 save naming a missing area | `58a8d21d` | The log and the crash report name the area | yes, for the report; the save still cannot load |
| F-1-02 signed overflow | `cebdd4a9` | The hash is computed unsigned | yes: the UBSan route ran with no trap |
| F-1-07, F-1-08 tables fatal when full | `7dfa809c`, `1a57cc21` | Replace the oldest buffer; reuse a destroyed event's slot | no: the tables were never full in a run |
| F-2-04 two Commits at once | `b2f9ea4c` | Pending commits are counted | not reproduced before; the save probe and check pass |
| F-8-02 crash during a commit | `cd170dad` | The crash and fatal paths wait up to 2 s for a commit under way | not reproduced before; the save probe and check pass |
| F-1-09 listing freed under a reader | `d61cd9b9` | Directory reads hold the lock | not reproduced before; the save probe and check pass |
| F-1-04 zstd slot | `32f2a41a` | The slot is dropped by its stream state under one lock hold | not reproduced before |
| F-1-05 hash of unmapped storage | `a9005e5c` | Both hashes run with the storage held mapped | not reproduced before |
| F-3-10, part of F-3-01 | `054bd556` | The content hash is forgotten when the image does not hold the storage's bytes | not reproduced before |
| F-3-03, F-3-11 mirror never asked again | `94ee6e78` | A failed allocation is asked again like a budget refusal; four log lines per reason | not reproduced before |
| F-2-02 submit count | `ca06d5c8` | The count is atomic | not reproduced before |
| F-6-01 menu dictionaries | `b4bfb7b6` | Loaded once behind a once-guard | not reproduced before |
| F-3-08 uniform write list | `3966d878` | Pruned with snapshots off too | not run in that mode |
| F-6-03 `NX2NAVI_ACQUIRE_WAIT` | `4b47a10a` | Read as a number | the value 2 was not run |

**Regression runs with the fixed exe.**

- The route (`p4_route`): `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6)
- The Market (`p4_market_1` to `_3`, `NX2NAVI_REPLAY_TIME=1 NX2NAVI_LOCK_TIME=1`): three runs of three (`p4_market_1` to `_3`): the list stays at 524-3192 entries and the walk at 0.008-0.026 ms per switch; standing after the pause tour no frame reaches 20 ms (worst 14.2, 17.3 and 18.3 ms at 118.5, 145.7 and 139.6 fps); the texture lock is held over 1 ms for at most 149 ms in a period (longest hold 4.34 ms), against 7792 ms (33.61 ms) before
- The save transaction (`--probe-fs`): `a crash at each of 6 commit steps recovers to the old save 3 times, the new one 3
  times, mixed 0 times; 3 backups`, `probe-fs: passed (186587 files, 64 sampled)`.
- UBSan, no ignore list (`p4_ubsan`): `p4_ubsan` went through the whole route (261 s of world time) with no trap: the start no longer stops at `menu_skin.cpp:265`, and none of the changed code tripped a check

### 13.2 A regression of the audit's own, repaired

Phase 2's size lines (`3a88b363`) made `kernel.c`, `fs_host.c` and `vk_gpa.c` call `perf_size`. Three unit checks build
one of those files without `perf.c` and stopped linking: `tests\sync_wake_check.py`, `tests\save_share_check.py` and
`tests\gpa_check.py`. Commit `78033dc4` gives them a stub (and gives `save_share_check` the `mem_probing` stub it lacked
since an earlier commit that is not the audit's).

Checks run after it, with the system Python 3.12:

- pass: `logq_check`, `save_zip_check`, `frame_step_check`, `wb_lazy_check`, `target_frames_check`, `gpa_check`,
  `mouse_stick_check`, `sync_wake_check`, `save_share_check`;
- `audio_input_check` does not link (`rt_knob` undefined); none of the audit's commits touches what it builds;
- not run: `warm_hint_check`, `flags_check`, `ds3_check` (they need the `unicorn` module, which this Python lacks),
  `jumptables_check`, `mods_romfs_check`, `mouse_direct_check`, `tensor_lowering_check` (they need arguments or game
  files).

### 13.3 Not fixed

**Waiting for the user** (the fix changes gameplay timing or behaviour, or crosses subsystems):

- F-2-01 with F-1-06: one lock around the synchronous submit and present front end (subsystems 10 and 11).
- F-2-05: the fence value returned before held work is replayed (changes when the game's waits return).
- F-8-01: 64-bit page generations (subsystems 12 and 13).
- F-1-10: audio parameter checks answered with an error result instead of ending the process.
- F-8-04: opening the menu when the controller disconnects.
- F-8-06: the shake key's timing.
- F-7-01: the two GPU waits per frame (not measured yet either).
- F-4-05: the fetch inside the fault handler (touches the piece protocol of lazy write-back).

**Not attempted.**

- F-1-11, the device loss at a save load: another session is diagnosing it and tied it to the lazy write-back's piece release (the ledger row has its counts). `test-all-fixes` has that release off by default since `6e3f29d4`; this branch does not hold that commit.
- F-1-12, callbacks run after a lost device: entered from that session's report and one of the audit's own logs; not changed.
- F-4-02 (create the DLSS feature ahead of use), F-4-04 (the pool finalize's wait): each needs a design and a careful
  run plan on the device's thread rules.
- F-4-03: its cause inside the replay is not named yet.
- F-3-02: the stand-ins' colour and the holds are rendering behaviour; they need the user's eye on the result.
- F-3-01's retry of a failed decode, F-3-12, and the remaining S3 and S4 rows (F-2-03, F-3-04 to F-3-07, F-3-09, F-4-06
  to F-4-10, F-5-01, F-5-02, F-6-02, F-7-02 to F-7-04, F-8-03, F-8-05, F-8-07).
