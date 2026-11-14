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
- Stability pass for 0.10.2: 2026-11-13 on branch `stability-pass` (worktree `E:\nx2pass`, cut from `next-0.10.0` at
  `f1a2a64e`), 225 commits to `87777fea`. Section 14 says what it did; its findings are F-1-13 to F-8-15 in section 8.

Citations name our code by file and function; a `~line` after them is a hint taken at `ed36773d` and will drift.
Hashes in the stability pass's text are `stability-pass` commits; the older rows' hashes are the audit branch's
(section 8 gives their `next-0.10.0` equivalents).

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

The stability pass (section 14) refreshed the table at `87777fea`: rows marked *new* come from its commits, rows marked
*missing before* existed at `f1a2a64e` but were not listed, and changed rows say what changed.

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
| "wait watch" (*missing before*) | `kernel.c` `wait_watch` | `kern_run_on_guest_main`, once | 1, detached | At once unless `NX2NAVI_WAIT_WATCH_S` > 0; then after its line limit |
| "hang watch" (*new*, F-1-36) | `hang_watch.c` `hang_watch_main`: every second it reads the frames shown, the GPU's progress, the focus and the window state | `main.c` `hang_watch_start` before `rt_boot`, game mode only; `NX2NAVI_HANG_WATCH_S` (default 20, 0 off) | 1, detached | After 3 reports (`HW_REPORTS`) |
| "rumble stop" (*new*, F-1-31) | `dispatch.c` `rumble_stop_main` → `input.c` `input_rumble_hd_stop` | `rt_rumble_stop_bounded` from `fatal_end` and `crash_handler` | 1, detached; the ending thread waits for it 300 ms at most | After the stop |
| "vk lost exit" (*missing before*) | `vk_device.c` `lost_wait_exit` | `vk_device_lost_check` at the first lost-device result | 1, detached | Ends the process with 72 after `LOST_EXIT_MS` (10 s) when the FATAL has not; since `8f952d28` through TerminateProcess (F-1-27) |
| "mem budget" (*missing before*) | `mem_budget.c` `budget_main` | `mem_budget_vram(1)` from `vk_device.c` once the device is ready, once | 1, detached | Never |

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
| "mem census" (changed) | `vk_memory.c` `census_main` | `heap_log`, only with `NX2NAVI_MEM_CENSUS=1`; since `f7487cee` not when the frame's present walked already (`perf.c` `census_check`: every 3600 frames and when the process's commit grew) | At most one at a time | After one walk; the frames it overlapped are charged to `census` (F-4-33) |
| "vk replay worker N" (*missing before*) | `vk_exec.c` `rw_main` | `rw_init` at the first replay, with `NX2NAVI_REPLAY_WORKERS` (default 0; `auto` picks by physical cores) | Up to 16, highest | Never |
| "par jobs" (*missing before*) | `par_jobs.c` `worker` | `pool_init` through `plat_once` at the first `par_run` (large texture copies); `NX2NAVI_PAR_JOBS` (default 1) | Usable CPUs less 3, at most 7 (`PAR_MAX_THREADS` 8 with the thread that asks) | Never |

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
| "pipe autosave" (changed) | `vk_pipeline.c` `autosave_main` (~6390) | `vk_pipeline_autosave` (~6402) from the present thread, once | 1, detached, below normal, 500 ms loop | Never; not stopped by `vk_pipeline_shutdown` (lead L-15). Since `48846dc9` (F-4-25) it also destroys the pipelines given back under VRAM pressure, 64 a tick once their fence passed (`release_drain`) |

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
| "shader compress" (*missing before*) | `gfx_shader.c` `zpass_main`: keeps idle shaders' SPIR-V compressed (F-3-12) | `zpass_kick` through `plat_once` when the first shader enters the memory cache; `NX2NAVI_SHADER_COMPRESS` | 1, detached, a 2 s loop | Never |

#### Audio and movies

| Thread | Entry | Started by, when | How many, priority | Ends |
|---|---|---|---|---|
| "audren" | `audren.c` `render_thread` (~517) | `audren_manager_handler` cmd 0 (~682) on the game's IPC thread | One per open renderer, highest | Joined in `renderer_close` |
| SDL audio output | `audio_mixer.c` `sdl_pull` (~320) | SDL's own device thread, `SDL_OpenAudioDeviceStream` in `stream_open` (~344) | 1 per stream | When the stream is destroyed |
| SDL audio capture | `audio_capture.c` `received` (~26) | SDL's own thread, `mic_open` (~98) | 1 per mic | `mic_close` |
| "movie" | `ov_movie.c` `worker` (~238) | `ov_ctor` (~454) on a game thread | One per VP9 decoder (`MAX_MOVIES` 16), plus libvpx's own threads (`VPX_THREADS` 4) | Joined in `destroy` |
| "movie decoder" | `ov_movie2.c` `decoder_thread` (~483) | `ov_initialize` (~601) | One per decoder, plus dav1d's own threads (8) | Joined in `stop_thread` |
| "movie convert" | `ov_movie2.c` `convert_thread` (~443) | `ov_initialize` (~603) | One per decoder | Joined in `stop_thread` |
| "movie convert" (stripes, changed) | `ov_movie2.c` `stripe_worker` → `stripe_run` | `run_stripes` at the first converted picture | 5 helpers kept for the process since `8106fdaa` (they were created and joined each frame, lead L-34) | Never |

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
| "camera speed" | `camera_speed.c` `search_main` (~262) | `cam_speed_tick` (~304) on the present path | One at a time (`searching`) | After one search; armed and in-flight ranges are skipped (F-4-26) |
| "exit" (*new*, F-1-38) | `gfx_window_win.c` `exit_main` → `gfx_present.c` `gfx_window_closed` | `wnd_proc` (WM_CLOSE, WM_ENDSESSION) through `exit_start`, once; the window is hidden and keeps answering | 1 | Ends the process |
| "rumble" (*missing before*) | `input.c` `hd_main` | `input_rumble_hd` at the first HD rumble | 1, detached | Never |

#### Monitoring, profiling and other

| Thread | Entry | Started by, when | Ends |
|---|---|---|---|
| "discord" | `discord_rpc.c` `discord_main` (~193) | `discord_init` from `main.c` | Never ("there is no shutdown path") |
| "rtss" (changed) | `rtss_limit.c` `worker`, every 500 ms (`POLL_MS`) | `rtss_limit_want` at the first want of any kind, kept-out ones and `NX2NAVI_RTSS=0` included; since `6be25cc9` the worker looks RTSS up itself (`find_rtss`), so the present waits on no registry key or file. It loads RTSS's library, writes the profile and `rtss-profile.txt`, restores a profile a killed run left (`left_start`), and reads RTSS's settings read-only while RTSS is kept out | Returns at once when RTSS is not installed; otherwise never |
| "handle census" (*new*, F-3-15) | `perf.c` `hc_main` | `hc_start` at start, only with `NX2NAVI_HANDLE_CENSUS=1` and `trace = perf`; woken by the 600-frame report | Never |
| "nvn copy sweep" (*new*, F-3-13) | `nvn_api_cmd.c` `retire_main`: fingerprints the recording copies' control memory every 5 s, at normal priority | `copy_add` at the first copy with `NX2NAVI_COPY_RETIRE=1` (default 0) | Never |
| "perfmon" | `perfmon.c` `sampler` (~360) | `perfmon_mode(2)` (~404) from the menu's overlay | When the full overlay is turned off |
| Profiler sampler | `prof_win.c` `sampler_main` (~242), time-critical, suspends every other thread each 1 ms | `prof_sampler_start` (~386), only with `NX2NAVI_PROFILE` | When `prof_sampler_write` stops it (waits 2 s) |
| "flight dump" | `flight.c` `dump_main` (~231), below normal | `start_dump` (~311), with `NX2NAVI_FLIGHT` | After one dump |

Also missing before and not changed by the pass: "debug names" (`debug_tools.c` `names_thread`), "bug capture"
(`bugcap.c` `writer`), "mesh fallback" (`vk_exec_draw.c` `mfb_worker`) and "mesh lib prebuild" (`vk_pipeline.c`
`mesh_pre_worker`). The pass adds no thread to the drawing path: the held-present lock and the frame's bookkeeping
(F-2-09) and the lazy fetch's flights (F-4-22) run on the threads that were there.

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

**Added by the stability pass** (from its fix reports and reviews; no inversion found in them):

- `frz_lock` → `lz_lock`; a holder of `frz_lock` may wait on `lz_fly_cv` for a range in flight, whose fetch takes only
  `g_ql` and then waits for a fence (`memory.c` "Ranges in flight", F-4-22). No `frz_lock` is taken inside `lz_lock`,
  and no `g_ql` holder takes `frz_lock` or `pool_lock`. A thaw that changes a mapping still fetches under `frz_lock`,
  with `frz_lock` → `vm_lock` for its page calls. `frz_lock` is not a leaf lock (F-2-15); `heap_lazy = 1` once made a
  copy fault under it into itself (F-1-16).
- `pool_lock` (exclusive) → `frz_lock` → `lz_lock` in the second release of `vk_memory.c` `mirror_evict` and
  `hot_check`, which may wait for a flight of the victim's buffer. With `wb_lazy_unlocked = 0` they hold `pool_lock` →
  `frz_lock` → `g_ql` and a GPU wait across the release, as in 0.10.1. `use_lock` → `mem_lazy_release`, whose fetch runs
  without `frz_lock`. `lazy_fault`'s miss branch makes one `VirtualQuery` under `frz_lock` alone.
- `cmd_lock` → astc `g_lock` (`vk_upload_begin` → `vk_astc.c` `scratch_get`); astc `g_lock` → `cmd_lock` by trylock only
  (`vk_astc_scratch_trim` → `vk_memory.c` `vk_defer_buffer_try`, F-1-20).
- `gen_lock` → the log queue (`vk_memory.c` `gen_mark`); `g_ql` → the log queue (the queue lock's holder lines, F-4-34).
  The log queue takes neither; inside it, `Q.io` → `Q.q`.
- Kernel `G` → `g_se_lock` (`kernel.c` `system_event_set`). `kern_bind_system_event` and `kern_unbind_system_event`
  never hold both (the unbind runs `DestroySystemEvent` before it takes `g_se_lock`); the full table's scan takes
  `g_se_lock` → `memory.c` `lock`. `G` is released before the hang watch's report logs the guest threads' waits, and the
  report only tries `G`.
- `gfx_present.c` `g_vs_lock` → `memory.c` `lock` (`gfx_vsync_bind` → `kern_system_event_live`).
- `nvn_api_cmd.c` `retire_lock` is never held with `copy_lock`; `nvn_submit_prepare` holds `copy_lock` shared across a
  synchronous clone's copy. `ipc.c` `g_domains` is taken alone: never with `G`, never across a handler or a close.
- The held-present lock (`vk_backend.c`, `vk_present_held.h`) → `sub_lock` (tried only), `pl`, `tex_lock`, `cmd_lock`.
  Submitting threads only try it, and no path takes it holding `sub_lock`, `pl`, `tex_lock` or `g_ql`. The frame's
  bookkeeping (`frame_end`) runs under `sub_lock` and takes what `vk_dlaa_frame` takes (`tex_lock` through
  `vk_sampler_dlss_bias`, NGX releases, VMA); the present thread takes `sub_lock` holding nothing else for a frame still
  owed, and nothing that holds `sub_lock` waits for the present thread (F-2-09).
- `g_pl` → Streamline's Reflex swapchain mutex (`vk_dlssg.c` `vk_dlssg_frame` → `reflex_apply` on the present thread;
  and `g_pl` → `g_ql` → that mutex in `gfx_present.c` `create_swapchain` → `vk_dlssg_create_swapchain`). The acquiring
  guest thread holds it only around `vkLatencySleepNV`, with no runtime lock (F-4-17).
- An upscaler creation refused while released ones are in flight: `sub_lock` → `g_ql` (an existing order) to flush the
  held submissions, then a timeline wait under `sub_lock` alone, 2 s at most; `cmd_lock` is never held when the deferral
  takes `g_ql` (F-4-20).
- `rtss_limit.c` `lock` → the log queue, RTSS's library and the state file; taken only by the worker and by the exit's
  restore through a trylock, never with a drawing thread's lock.
- `fs_host.c` `tx_lock` → `lock` is unchanged; a save file's last user takes `lock` at the end of a call only when a
  close is pending (F-1-28).

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
   5. `frame_start`, and the back end's frame start (`vk_backend.c` `vk_be_frame_start`): Reflex's sleep with DLSS Frame
      Generation, counted as pacing while Reflex holds a limit, or a driver's low-latency sleep.
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
    4. `present_period` → `frame_rate.c` `frame_step_cap_ns` gives the limit's period. While RivaTuner Statistics
       Server is seen holding the limit (`rtss_limit_active`) it returns the 240 ceiling's period
       (`frame_step_ceiling_ns`, through `rtss_decide.h` `rtss_pace_period`), not 0 as before `27a034e9`; at a limit of
       240 or unlimited that is RTSS's own period. It returns the limit's own period while a movie plays, before the
       first host present, after 200 ms with no host present (a minimised window, a frame hold), and whenever RTSS is
       not proven to hold the game: not hooked, its profile not read back, no fresh period measured, or given up for
       the run (1.05 times the limit over three fresh periods, or 30 s listed with the limit and no proof of its
       hold). A movie keeps the play limit in RTSS's profile (F-4-12).
    5. The back end's `frame_paced` may take the limit, except during a movie: Reflex with DLSS Frame Generation holds
       it where the frame starts and the pacer keeps only the ceiling (`vk_dlssg.c` `vk_dlssg_pace`, F-4-17); a
       driver's low latency holds it and the pacer keeps nothing (`vk_lowlat.c` `vk_lowlat_pace`). Then `pace()` keeps
       the phase: early waits for the slot; late by less than a period presents now; later restarts, except that
       during a movie the slots are kept through a lateness of up to `MOVIE_CATCHUP_NS` (1 s).
    6. `gfx_backend()->present` → `vk_backend.c` `vk_be_present(t, crop, slot)`.
    7. `o->avail[idx] = slot + period`; the image's fence.
    8. `frame_step_present(slot, interval)`: `ocarina_step` writes the game's next step (patch set `ostep`) to the
       patch page or the game's time manager, and the world clock advances.
    9. `rt_frame_presented`.
15. **`vk_be_present`** (sync).
    1. Frame counter; `vk_dlssg_submitted`; `perf_present`.
    2. With `NX2NAVI_PRESENT_HELD` (default on in sync mode), a present that finds held work waits for it, up to
       `PH_MAX` (8) submissions. Since `a789bd8f` a held present goes out under a lock of its own
       (`vk_present_held.h`) that submitting threads only try, once, whole and before the next present.
    3. Otherwise `present_image` (~215):
       1. Frame-level work: `vk_exec_frame_log`, `dynres_frame`, `vk_dlaa_frame`, `vk_sampler_aniso_live`,
          `vk_mem_census`. Since `51c9261d` the replay's part of it (`vk_backend.c` `frame_end`: the frame log,
          `dynres_frame`, `vk_dlaa_frame`, the motion surveys, the GPU passes) runs under `sub_lock`: here when the lock
          is free, else first thing in the next submission, or on the present thread before this frame's blit
          (`vk_frame_owed.h`, `618cfc01`).
       2. `vk_texture_get(t)` gets the presented texture's image, possibly recording an upload.
       3. `frame_hold` checks for skipped draws, a resize or an area load. If it holds the frame, the previous
          picture stays and this present is dropped (`NX2NAVI_FRAME_HOLD_MS`, 120 ms allowance).
       4. `vk_hdr_present_source`.
       5. `vk_upload_flush(0)`.
       6. The `PresentJob` goes to the "vk present" thread. At most one job waits, so the caller blocks while one is
          pending; that wait is timed as the hand-off (`PERF_HANDOFF`, `04727340`).
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
| 65 | Page-generation tables (`gen_chunk`, `cgen_chunk`): 256 MiB chunks of uint64 (512 KiB each) on first stamp since `6b95c867` (F-8-08); `reg_max`/`creg_max` 2 MiB each | `gen_lock` | `RT_ADDRESS_LIMIT` | Shutdown |
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

- *Release overflow.* `sweep_released`: `RELEASED_MAX` 2048 entries, and `NX2NAVI_TEX_RELEASED_MB` (default
  budget/32 clamped 384-1024 MiB since 0.10.2, `vk_tex_release.h` `trl_cap_mb`; before it RAM/32 clamped 512-2048
  MiB, raised by budget/4 up to 8192).
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

- **L-01** *(Phase 3: F-2-04, suspected.)* *(Stability pass: fixed (`5ddff078` on `next-0.10.0`).)* `fs_host.c` `save_commit`, `commit_run`. A second Commit while the first runs: the first thread clears
  `commit_pending` while the second waits for `tx_lock`, so `save_wait_commit` may return early and a write may land
  during the second copy.
- **L-02** *(Phase 3: F-1-09, suspected.)* *(Stability pass: F-1-09's listings fixed (`8ce64407`), its file handles by `790d201a` (F-1-28); `romfs.c` `handle_source` is unchanged, a fault of the game's own.)* `fs_host.c` `file_of`, `read_directory`; `romfs.c` `handle_source`. A handle or listing is used after the
  lock is dropped while another guest thread may close it.
- **L-03** *(Phase 3: F-8-02, suspected; the transaction itself confirmed atomic.)* *(Stability pass: fixed (`7ccb958c`).)* `fs_host.c`. Crash and `rt_fatal` paths do not call `fs_save_flush`. Recovery is left to `save_recover` at
  the next start; pass 8 should interrupt each commit step and check it.
- **L-04** *(Phase 3: F-4-05, suspected; the AMD-crash part struck.)* *(Stability pass: fixed with the user's approval (`2e077edc`, F-4-22).)* `memory.c` `lazy_drop_locked`, `vk_memory.c` `lazy_fetch`. A GPU copy and a wait (10 s, then fatal) inside
  the exception handler under `frz_lock` serialise every other fault, arm and thaw. It is a candidate for the AMD
  crash and device-loss reports, and for hitches where lazy write-back is on.
- **L-05** *(Phase 3: struck: nothing under `frz_lock` touches guest pages, and the handlers never take `lock`.)* *(Stability pass: the strike does not hold with the lazy heap: a copy faulted into itself under `frz_lock` with `heap_lazy = 1` (F-1-16, fixed by `b146ee71`), and `frz_lock` is not a leaf lock (F-2-15).)* `platform/win32.c` `fault_veh` → `memory.c` handlers. A fault on a thread that already holds `frz_lock`
  (or `lock`, in `stack_move`'s copy) would re-take a non-recursive lock.
- **L-06** *(Phase 3: struck: the completion thread runs a submission's callbacks before the fence the game waits for, and mirrored records are freed on that same thread.)* *(Stability pass: struck; now also guarded in code (`vk_gpu_written_back` looks the pool up again under `pool_lock`).)* `vk_memory.c` `vk_gpu_written_back`. A `PoolRec` from `vk_mem_lookup` is used after `pool_lock` is dropped,
  on the completion thread, while `vk_pool_fini` may destroy it.
- **L-07** *(Phase 3: struck: reachable only through F-2-05.)* *(Stability pass: the strike was wrong: the race was real without F-2-05 and `4882623c` fixed it; F-1-15 closed a second window in `mirror_create` (`350c8b21`).)* `vk_memory.c` `mirror_create`. `mstate = MS_ON` is set after `pool_lock` is released; a concurrent finalize
  may skip `mirror_forget` and leave the record in `mlist`.
- **L-08** *(Phase 3: struck: only a method racing its own destructor, a fault of the game's.)* *(Stability pass: struck; F-2-17 fixed `ov_decode`'s unlocked read (`6029321e`); `ov_movie.c` `find` is not used by 1.1.1 (F-2-19).)* `ov_movie.c` `find`. The movie is used after `g_lock` is released while `destroy` may free it.
  `ov_movie2.c` `ov_decode` reads `v->dav1d` unlocked against `finalize`.
- **L-09** *(Phase 3: F-1-03, suspected.)* *(Stability pass: fixed (`63228e7c`, F-2-06).)* `ipc.c` `ipc_svc` 0x21/0x22. The session is used without a reference while `CloseHandle` may free it.
  Domain fields are unlocked.
- **L-10** *(Phase 3: F-1-07 and F-1-08, growth confirmed; `files[1024]` struck (slots are freed at close).)* *(Stability pass: fixed (`4a69a38b`, `9be72ae9`); F-2-07 and F-1-17 fixed two more tables.)* Fixed tables that are never emptied, each fatal when full, for multi-hour sessions:
  - `kernel.c` `g_se[512]`: system events, bound by `ov_system.c` `ov_report_event` and `net_offline.c`;
  - `guest_setjmp.c` `jt[1024]`;
  - `fs_host.c` `files[1024]`.
- **L-11** *(Phase 3: F-1-04, suspected; the fiber and bind parts struck.)* *(Stability pass: fixed (`61e1ca8b`).)* Concurrency in the kernel and zstd:
  - `kernel.c` `kern_fiber_finalize`, `kern_fiber_initialize`: fiber fields outside `g_fiber_lock`;
  - `kernel.c` `kern_bind_system_event`: check-then-create;
  - `ov_zstd.c` `stream_take`: a spent slot reused while still read.
- **L-12** *(Phase 3: struck: such a thread holds no core, and the thread count stays 84-85.)* *(Stability pass: struck.)* `kernel.c` `thread_main`. A thread created but never started (or closed first) waits forever, keeping its
  stack reservation.
- **L-13** *(Phase 3: F-4-09, suspected; the crash and race parts struck.)* *(Stability pass: fixed (`af061602`, F-4-26); F-3-19 is left.)* `camera_speed.c` `search_main`. It rewrites every byte-pattern match of the camera speed array across the
  guest heap and alias region, while the game runs.
- **L-14** *(Phase 3: F-6-01, suspected; not reproduced in 54 logs.)* *(Stability pass: fixed (`79661811`).)* `menu_assets.c` `dicts_load`. Lazy dictionary load without a lock, from the "menu-art" thread and the
  present thread.
- **L-15** *(Phase 3: struck: the game never shuts the device down; only probes do.)* *(Stability pass: struck; F-1-35 fixed a NULL read in `workers_drain` at a probe's shutdown (`ea5087ba`).)* `vk_pipeline.c` `autosave_main`. It is never stopped and outlives `vk_pipeline_shutdown`; `vkDeviceWaitIdle`
  at shutdown runs without `g_ql` (D-11).
- **L-16** *(Phase 3: F-1-06, F-2-01, F-2-02, F-2-03; overlapping submits confirmed.)* *(Stability pass: fixed: F-2-02 (`d15b4c95`), and F-1-06, F-2-01, F-2-03 and `q->submits` in this pass (F-1-14, F-2-09).)* Sync-mode races:
  - `vk_backend.c` `PH`, `FH`, `vk_exec_held` and `nvn_submits_in_frame`;
  - `nvn_api_core.c` `q->submits`;
  - `nvn_api_cmd.c` `resolve_deferred` in place.

  All of these apply if the game submits and presents from different threads.
- **L-17** *(Phase 3: struck: this game has no such patch sets, so the function returns before the switch.)* *(Stability pass: struck.)* `frame_rate.c` `frame_limit_live`. It is reached from three threads without a lock, and can call
  `rt_patch_switch` concurrently.
- **L-18** *(Phase 3: struck: the environment is written only at start and just before exit.)* *(Stability pass: struck: `cfg_setenv` is called only by probes.)* `cfg.c` `raw_get`. `getenv` races with `cfg_setenv` → `_putenv_s` while the game runs
  (`gfx_present.c` ~90).
- **L-19** *(Phase 3: struck as a crash lead: these fire only when memory is gone; the measured 175-178 samplers are far from the driver's limit. The texture part is in F-3-01 and F-3-02.)* *(Stability pass: changed: refused allocations give back the kept buffers and try again (`74b74495`), a refused upload submit is retried (`e1b1691e`); a failed `vkCreateSampler` is still fatal.)* Fatal on allocation failure: staging, transient chunks, samplers (`vk_texture.c` `sampler_for`), the AV1
  picture temporaries. A texture create failure triggers no eviction, only a 2 s hold.
- **L-20** *(Phase 3: F-1-10, suspected; not determined (no dungeon or boss).)* *(Stability pass: fixed with the user's approval (`7e7fed63`, F-1-19).)* `audio_mixer.c` and `audren.c`. `rt_fatal` on game-supplied parameters during RequestUpdate, with the
  renderer lock held.

### Memory and VRAM (pass 3)

- **L-21** *(Phase 3: F-3-01, suspected; not reproduced.)* *(Stability pass: fixed (`29eb7cf0`, `ccc31ad5`).)* `vk_texture.c` `upload` (~2411). A failed GPU ASTC decode leaves the texture marked uploaded and
  uninitialised until its storage changes. This is one candidate for the white ground after long play.
- **L-22** *(Phase 3: F-3-02, suspected; not reproduced.)* *(Stability pass: changed (`84b0672e`, `0d564a21`).)* `vk_texture.c` `evict_disused`, `retire_sweep`, `create_tex`. Retired or failed textures show stand-ins;
  re-creation is limited per frame (`WANT_PER_FRAME` 32). Another candidate for white or untextured ground.
- **L-23** *(Phase 3: F-3-03, suspected; not reproduced.)* *(Stability pass: fixed (`3e67a74c`).)* `vk_memory.c` `mirror_create`. A pool refused for `MR_ALLOC` is never retried and is read over PCIe for
  life. A candidate for towns running slower in long play than in fresh scripted runs.
- **L-24** *(Phase 3: struck as unbounded growth (each item is bounded by the game's content or the player's changes; flat across the reload); F-3-12 for the memory the shader cache keeps.)* *(Stability pass: struck; F-3-12 changed (SPIR-V compressed at rest); keys are recorded at the fast link (`7aa29ff1`, F-4-39).)* Growth without a limit for the life of the process:
  - pipelines, libraries, layouts and the shader memory cache (`vk_program_release` is a no-op);
  - the sampler cache;
  - shadow-storage base copies (`vk_memory.c` `sbase_make`, one 4 KiB copy per GPU-written page, kept until the pool
    is finalized; shadow mode only);
  - `pipeline-keys.bin`;
  - `cfg_set`'s old strings.
- **L-25** *(Phase 3: F-3-04 and F-3-05, suspected; the offline socket part struck (spare events are reused).)* *(Stability pass: fixed (F-3-16, F-3-17).)* Leaked kernel objects per call:
  - `audio_mixer.c` change events (beyond 16);
  - `ipc.c` domain sub-objects;
  - `net_offline.c` spare events.
- **L-26** *(Phase 3: F-3-06, suspected.)* *(Stability pass: fixed (`222a025c`).)* `platform/win32.c`. Per-thread timer and file-event handles are never closed. One "save commit" thread per
  Commit and one "camera speed" thread per search leak a few handles each.
- **L-27** *(Phase 3: F-3-07, suspected.)* *(Stability pass: open (F-3-07).)* `vk_headroom.c` `room`. It ignores `NX2NAVI_VRAM_BUDGET_MB`, so a capped test run is not the small card it
  claims to be.
- **L-28** *(Phase 3: struck: bounded by the armed pages; 84-144 ranges measured.)* *(Stability pass: struck; the list has its own `lz_lock` (`1dda8bc2`).)* `memory.c` `lzs`. Lazy write-back segments grow without a cap, and every fault and arm scans them linearly.
- **L-29** *(Phase 3: F-3-08, suspected for the non-default mode.)* *(Stability pass: fixed (`a96328e9`); the arena free list is still never trimmed (measured flat).)* `vk_usnap.c`. `U.wr` grows without pruning when `NX2NAVI_UBO_SNAPSHOT=0`. The arena free list is never
  trimmed.
- **L-30** *(Phase 3: F-3-09, suspected.)* *(Stability pass: fixed (`a8c6e992`).)* `vk_exec_draw.c` `mesh_ring_entry` (~5048). The mesh ring's allocation handle is discarded, so it is never
  destroyed (shutdown only).

### Stutter (pass 4) and performance (pass 7)

- **L-31** *(Phase 3: F-7-01, suspected; the A/B is not valid yet.)* *(Stability pass: open, not measured.)* `nvn_api_core.c` `nvnQueueSubmitCommands` and `nvn_api_obj.c` `nvnWindowAcquireTexture`. Every frame waits
  for the GPU twice (after its first submission, and at acquire for the image's last present). At 120 fps and above,
  a slow GPU frame becomes a CPU stall. This is the first suspect for the uncapped-rate spikes.
- **L-32** *(Phase 3: F-4-07, suspected; the lock part struck.)* *(Stability pass: open; the hand-off is timed (`04727340`).)* `vk_sync.c` `submit`. `sub_lock` is held over a whole replay; the present thread's empty submission waits
  behind it, and held work may then replay on the normal-priority present thread.
- **L-33** *(Phase 3: F-4-06, suspected.)* *(Stability pass: narrowed (F-2-11, F-4-34); the present under `g_ql` is F-4-13, measure first.)* `gfx_present.c` `flip_locked`, `create_swapchain`, `movie_overlay`, `hudless_record`.
  `vkQueuePresentKHR` and `vkDeviceWaitIdle` run under `g_ql`, which `queue_submit` waits for. `vk_memory.c`
  `vk_upload_flush(1)` waits for fences under `cmd_lock`.
- **L-34** *(Phase 3: F-4-10, suspected; the motion thread and the recorder spin struck.)* *(Stability pass: changed: `run_stripes` fixed (`8106fdaa`); the compute poll is open.)* Thread churn and polling:
  - `ov_movie2.c` `run_stripes` creates and joins 5 threads per frame;
  - `input.c` `motion_main` takes `lock` 250 times a second forever;
  - `vk_rec.c` `wait_space` spins without yielding;
  - `vk_pipeline.c` `vk_pipeline_compute` polls every 1 ms without a limit.
- **L-35** *(Phase 3: F-7-03, suspected.)* *(Stability pass: fixed (`c0f8a322`, F-7-05).)* `vk_texture.c` `sweep_pressure`, `evict_idle`, `evict_disused`. Each scans the whole cache under `tex_lock`
  on the submitting thread every 8 submits.
- **L-36** *(Phase 3: F-4-04, the time confirmed.)* *(Stability pass: changed (`83faa1da`, `e70cbb6a`).)* `vk_memory.c` `vk_pool_fini`. `vk_upload_flush(1)` plus `vkQueueWaitIdle` on a guest thread per finalized
  pool: a hitch at area changes.
- **L-37** *(Phase 3: struck for the default settings: the boot waits without a cap.)* *(Stability pass: struck for the defaults; F-7-11 (measure first).)* `boot.c` `boot_wait` → detach. A prewarm still running in play keeps up to 16 translator processes and
  cpus-many worker threads busy.
- **L-38** *(Phase 3: F-4-08, suspected; the audio part struck.)* *(Stability pass: open.)* The present path does slow work under the present lock: `ui/menu.cpp` `menu_overlay_frame` (`cfg_save`,
  `vkQueueWaitIdle` for the font, art uploads); `audio_mixer.c` reverb allocation and device enumeration on the
  highest-priority render thread.
- **L-39** *(Phase 3: F-7-02, suspected.)* *(Stability pass: open.)* `vk_rec.c` `rec_begin`. The recorder thread keeps the first caller's priority, not the recording thread's
  highest, and may sit outside the reserved core.
- **L-40** *(Phase 3: struck: fences are added in order.)* *(Stability pass: struck.)* `vk_sync.c` completion queue. It is FIFO, not ordered by fence, so one late callback holds back the frees
  behind it.
- **L-41** *(Phase 3: kept as a rule of method, not a finding.)* *(Stability pass: a rule of method.)* `prof_win.c` sampler. Time-critical, suspending every thread each 1 ms. Phase 2 must not compare profiled
  runs with unprofiled ones.
- **L-43** *(Phase 3: struck: the core is already given away before the wait.)* *(Stability pass: struck (asynchronous replay is still off by default).)* `vk_backend.c` `vk_be_present` (asynchronous replay only). The "frame bound" wait (~561-567) blocks the
  game's present thread without giving its virtual core away, unlike `replay_drain` and `replay_enqueue`.

**Leads added by Phase 2's measurements** (section 11.3). They are measured, but their causes are not traced.

- **L-44** *(Phase 3: F-4-01, confirmed.)* *(Stability pass: fixed (`b933e3d9`; the 0.10.0 log stays at 3,662 at most).)* A periodic stall in the Market. In `bl_market_1`, after the script ended (Link standing, segment
  `market-end`), every 8th frame took 25-30 ms from frame 13042 to 13786 (90 frames). The same pattern appears 5 times
  in `bl_market_2`. Nothing is named:
  - no render-thread wait, no work mark;
  - GPU 3.15 ms per frame, "bound: cpu";
  - 6 submits per frame, so it is not `vk_texture.c` `sweep_pressure`'s every-8-submits rhythm.

  A profiled run (`NX2NAVI_PROFILE_SLOW`) would name it.
- **L-45** *(Phase 3: not a growth of the host's replay (its time stays 5.5 ms); the cause is not determined.)* *(Stability pass: open, not measured.)* The CPU frame grows while standing in Hyrule Field toward dusk (`bl_fieldlong`).
  - Over 9 minutes the frame went from about 7.1 to 8.4 ms (140 → 118 fps).
  - The GPU's time stayed at 4.37 ms, and every window was "bound: cpu".
  - The size lines are flat, except released textures (165 → 542).
- **L-46** *(Phase 3: struck: 32512 is the game's own fixed pool, full from the start.)* *(Stability pass: the strike was wrong: the game makes 33,068 descriptors and handles past 32767 were clamped (the see-through ground); `2388f1f5` sets `VK_POOL_TEX` to 40960, and F-6-04 and F-5-04 keep the earlier translator's clamp out of releases and updates.)* `texture pool slots 32512/32768` in every area and run: the registered texture slots the bindless pool
  covers (`vk_texture.c` `pool_slots`, `VK_POOL_TEX` 32768). What happens to a slot past `VK_POOL_TEX` was not checked.
- **L-47** *(Phase 3: F-4-02 and F-4-03, confirmed.)* *(Stability pass: F-4-02 changed (F-4-20), F-4-03's part named (F-4-19).)* The first one or two world frames after every save load take 48-68 ms and 26-36 ms, with nothing named.

### Logic that assumes the original rate (pass 8)

- **L-42** *(Phase 3: F-8-05, F-8-06, F-8-07, suspected; `res_present` and the mouse camera struck.)* *(Stability pass: F-8-05 and F-8-06 fixed with the user's approval (`1c26907a`, `5c9e480f`); F-8-07 open; the ocarina recorder's sites located and patched (F-8-13); the item jingle not located (F-8-12).)* Assumptions in the host code:
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

The stability pass for 0.10.2 (section 14) added F-1-13 to F-8-15: one row per finding its verifiers did not refute,
joined findings in one row, numbered within each pass by severity. Their Lead column says "stability pass" with the
pass's own ids (A-4, Fv-1, …; lane letter, then number; a `v` marks one the verifiers found), or names the second wave's
design or review that found it. A "re-check of" an older row means the pass found that row still open; both rows then
name the fix. The pass also checked every older row and lead again (a map refresh at `2f640fb8`) and added a **Stability
pass** note to the last column of each older row it found changed or fixed.

The Phase 4 commits in the last column are the audit branch's. On `next-0.10.0`, and so on `stability-pass`, they are:
F-1-01 `fb448090`, F-1-02 `abd2e2c3`, F-1-04 `61e1ca8b`, F-1-05 `4ffaf6ea`, F-1-07 `4a69a38b`, F-1-08 `9be72ae9`, F-1-09
`8ce64407`, F-2-02 `d15b4c95`, F-2-04 `5ddff078`, F-3-03 and F-3-11 `3e67a74c`, F-3-08 `a96328e9`, F-3-10 and F-3-01's
part `ccc31ad5`, F-4-01 `b933e3d9`, F-4-11 `e887a1d1`, F-6-01 `79661811`, F-6-03 `493c877d`, F-8-02 `7ccb958c`.

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
| F-1-03 | S1 | Services (5) | `ipc.c` `ipc_svc` 0x21/0x22 (~516), `kernel.c` `kern_handle_get` (~2008), `ipc_handle_closed` (~237) | `kern_handle_get` returns the session without a reference and `dispatch` uses it unlocked. A CloseHandle on another guest thread frees it mid-request. Domain tables (`objects`, `nobjects`) are also written unlocked while `dispatch` reads them | A guest thread closes a session while another is inside SendSyncRequest on it (the console keeps the session alive). The sessions here are made at boot, plus the audio renderer and amiibo | suspected (code read, `kern_handle_get` re-read by the auditor); not seen in any run | Take a reference under the kernel lock for the length of the request, released through the path `ipc_handle_closed` uses | Low; services only | L-09 | **Stability pass:** fixed by `63228e7c` (a reference held for each request; transfer memory copied under the kernel lock) and `e9becc12` (domain objects under a lock of their own): F-2-06, F-3-17. Not reproduced before. |
| F-1-04 | S1 | Files (6) | `ov_zstd.c` `stream_drop` (~340), `stream_take` (~326) | `stream_drop` clears the slot by pointer, not by owner: thread A looks up spent slot k for its stream and unlocks; thread B takes slot k (all 16 slots taken); A's `stream_drop` zeroes it; thread C takes it as free and resets the decoder context B is decoding with. The asset B decodes is corrupted | More than 16 stream states alive, so that slots are reused, with loader threads decoding at once. Phase 2 saw 0 of 16 slots taken at every snapshot (snapshots fall outside loads) | suspected (code re-read by the auditor) | `stream_drop(st, s)` clears only when `st->s == s`, under `stream_lock` | None | L-11 | `32f2a41a`. Not reproduced before, so not shown fixed. Regression: the route with the fixed exe: `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6) |
| F-1-05 | S1 | Textures (13) | `vk_texture.c` `same_content` (~2489-2493), `upload` (~2264-2272) | `same_content` hashes a texture's guest storage with no readability check and no read hold; `upload` hashes it after `mem_readable` but outside `mem_read_hold`. If the game unmaps the storage between the check and the hash, the render thread reads decommitted memory: an access violation in host code | A texture of at most `CHASH_MAX` bytes whose storage is freed while a page change sends it through `make_fresh` / `make_fresh_snap` | suspected (code re-read by the auditor). Whether a change can be reported for a range just unmapped was not traced | Hash under `mem_read_hold`; in `same_content` treat a failed hold as "changed" | Low, textures only | — | `a9005e5c`. Not reproduced before, so not shown fixed. Regression: the route with the fixed exe: `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6) |
| F-1-06 | S1 | Replay (11) | `vk_exec.c` `vk_exec_held_ready` (~1261), `cont_walk` → `cont_free` (~1332) | `vk_exec_held_ready` reads the head continuation of each event queue (`evq[s].head->at`, `->since`) without `sub_lock`. A second submitting thread inside `vk_sync_submit` can free that continuation meanwhile: a read of freed memory, then `vk_exec_event_met` through a dangling pointer | Held (event-order) work at the moment two guest threads are in submit or present. The precondition is confirmed: submits overlap from the pause menu's opening (F-2-01) | suspected (code re-read by the auditor) | The front-end lock of F-2-01, or read under `sub_lock` | Low to medium; see F-2-01 | L-16 | **Stability pass:** fixed without the front-end lock by `688b2506` and `7a1c220e` (F-1-14). Not reproduced before. |
| F-1-07 | S1 | Process entry (2) | `guest_setjmp.c` `slot_of` (~101), `ov_setjmp` (~127) | The `jmp_buf` table `jt[1024]` keys on the buffer's guest address and entries are never removed; when it is full `ov_setjmp` ends the process with "setjmp: more than 1024 buffers" | A long session: every new `jmp_buf` address takes an entry for good | confirmed growth, the fatal itself not reached: `mem: sizes … setjmp buffers` went 10 → 22 and 8 → 23 across one save, title and reload (section 11.3). At +12 to +15 per reload the table lasts about 70 reloads if the addresses keep changing; whether they level off was not measured | When full, replace the entry set longest ago and log once; a `longjmp` to a replaced buffer stays fatal as now | Low: only the overflow path changes | L-10 | `7dfa809c`. The full table was not reached in any run, so the new path has not run. Not reproduced before, so not shown fixed. Regression: the route with the fixed exe: `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6) |
| F-1-08 | S1 | Kernel (3) | `kernel.c` `kern_bind_system_event`, `g_se[512]` (~2060-2095) | The system-event table keys on the event's guest address and entries are never removed; full is fatal ("table full") | A long session with many reloads | confirmed growth, the fatal not reached: `system events` went 8 → 10 across one reload in both route runs (about 250 reloads to the limit) | Reuse an entry whose event is no longer live (`se_live` 0) | Low | L-10 | `1a57cc21`. The full table was not reached in any run, so the new path has not run. `tests\sync_wake_check.py` (builds `kernel.c`) passes. Not reproduced before, so not shown fixed. Regression: the route with the fixed exe: `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6). **Stability pass:** a full table no longer evicts live events in host memory, and a signal racing a destroy no longer ends the process (F-2-07: `465ffe67`, `d8c046ac`, `d9940113`). |
| F-1-09 | S1 | Save data (7), Files (6) | `fs_host.c` `read_directory` (~1307, no lock at all), `file_of` (~1068), `close_directory` (~1340); `romfs.c` `handle_source` | A directory listing or file handle is used after the lock is dropped (or with none taken): a CloseDirectory on another guest thread frees the listing `read_directory` is copying from; a CloseFile closes the host file a read is using | The game closes a handle on one thread while another uses it. That is a fault of the game's own; the console answers it with an error result, not a crash | suspected (code re-read by the auditor); not seen | Hold `lock` shared for the length of the call, or count users per slot | Low | L-02 | `d61cd9b9`. Directory listings only; file handles keep their window. Not reproduced before. `--probe-fs` and `tests\save_share_check.py` pass with the change. **Stability pass:** the file-handle half fixed by `790d201a`, `b8b57cfd` (F-1-28). |
| F-1-10 | S1 | Audio (18) | `audren.c` `need` (~66-70), `audio_mixer.c` parameter checks (~641-1045) | Parameters the game supplies in RequestUpdate (limiter, reverb, sink sizes) are checked with `rt_fatal`; the console's renderer returns an error result instead | An effect configuration outside the checked ranges. The route (five areas, the pause menu) tripped none; dungeons and bosses, whose effects differ, could not be reached | suspected; not determined by a run | Return the error result and log once instead of ending the process | Changes what the game is told in that case: ask the user first | L-20 | **Stability pass:** fixed with the user's approval by `7e7fed63`, `9770d409`, `a40afdd8` (F-1-19). |
| F-1-11 | S1 | Replay (11), GPU memory (12); cause not determined | The GPU fault is reported by `vk_crumbs.c` and the device-fault lines; the process ends in `vk_memory.c` ("upload submit failed (-4)") or `vk_sync.c` ("the wait for a submission's commands failed (-4; device lost?)") | The GPU reports a page fault and the device is lost while a save loads: `vk: device fault address 1: read invalid at 0x31f000000` (also `0x30b000000`, `0x311000000`), right after `vk: texture pool: an area load of about 4629 textures was made in 1 frames`; the GPU's last mark is `dispatch 3 of submission N: 293x1x1 groups, cs 3524d41df45a41e0`, started and finished | Loading the Hyrule Field day save (`saves\one_u3`) in `E:\nx2devrun` while the boot's pipeline prebuild is skipped there (F-4-11), so pipelines are built at their first draw | confirmed 3 times: runs `p3_l31_nosubmit` (`SUBMIT_FINISH=0`), `p3_l31_noacquire` (`ACQUIRE_WAIT=0`) and `p3_l31_default2` (default settings), three in a row. In the same state `p3_l31_default`, `p3_l31_nosubmit2` and `p3_market_prof` loaded without it; none of the earlier runs of Phases 2 and 3 (prebuild not skipped) had it. **Later data from the session diagnosing it** (its runs, `E:\nx2devrun\out\crash_matrix.txt`): the loss follows the lazy write-back's piece release, not the skipped prebuild: 0 lost in 15 loads of the night field save (`saves\one_u0`) with `NX2NAVI_WB_LAZY_CHUNK=0`, 0 in 10 with `NX2NAVI_WB_LAZY=0`, about 7 in 22 with it on, and one more in the day field (`out\bn_fday_new_2`). The read fault is at the first page of a 64 MiB device memory that holds a 26.6 MiB buffer and 125 images bound 2 s earlier. The mechanism is not known | `test-all-fixes` switches the piece release off by default (`6e3f29d4`, not on this branch: here `WB_LAZY_CHUNK` still defaults to 1). The cause itself is open | — | — | Not the audit's commit; the audit's own runs after it (`p4_*`, 7 save loads with the default) lost no device. **Stability pass:** changed. The map refresh found the piece release off by default (`47f6aef2`, `WB_LAZY_CHUNK` 0) and no device fault in the 0.10.0 logs; the mechanism is not known. This pass fetches a piece without `lz_lock` (`7df96ef0`, F-4-37) and every range without `frz_lock` (`2e077edc`, F-4-22), and fixed a read during an arm that went to the crash handler (`ef0673fb`, F-1-21). Play-test: an AMD RX 5000/6000 session. |
| F-1-12 | S3 | Replay (11) | `vk_sync.c` `completion_main` (~395-431) | After the device is lost the completion thread can still take the timeline as advanced: it ends the process only when `vkWaitSemaphores` or `vkGetSemaphoreCounterValue` return an error, and a lost device may answer both with success and a signalled value. It then runs the callbacks of submissions that never finished (frees, record reuse, counter reports) before the next checked call reports the loss, so the first symptom in the log is a wrong one | Any device loss (F-1-11) | symptom confirmed, mechanism suspected: in `p3_l31_noacquire` the line `vk: occlusion query slot 96 not available after its fence, counted as visible` comes six lines before `vk: device fault`. Reported by the session diagnosing F-1-11; the code was re-read by the auditor (the two result checks are there, a check of the device itself is not) | Ask the device whether it is lost before running callbacks (a cheap checked call, or the fault query), and end with the fault report first | Low: only the path after a loss changes | — | **Stability pass:** fixed in part on `next-0.10.0` by `a6a3944b` and `8df43117` (the timeline read is checked and the device polled before a value counts; it stops before the callbacks once the device is lost); a lost device that returns a plausible value and passes the poll still runs them. `68bc1973` routes the present thread's fence waits through the lost-device check (F-1-33). Not confirmed on a real loss. |
| F-1-13 | S1 | Frame pacing (9), Device and present (16) | `frame_rate.c` `frame_step_cap_ns`; `gfx_present.c` `present_image` (minimised); `vk_backend.c` `frame_hold` | While RTSS held the limit `frame_step_cap_ns` returned 0, so the pacer held nothing. A minimised window or a frame hold sends RTSS no present to hold, and the game then ran unlimited; at the quick menu that nears the 640-700 fps crash (`9bba578e`) | RTSS holding, then the window minimised, or presents dropped by `frame_hold` (pipeline hold, resize, area load) | suspected (code read: period 0 makes the acquire wait for nothing; the crash itself not reproduced) | Keep the 240 ceiling while RTSS holds, and take the limit back after 200 ms with no host present (`gfx_window_hosted_ns`, `rtss_decide.h` `rtss_pace_period`) | Pacing under RTSS (approved). At 240 or unlimited the ceiling equals RTSS's period, so both can hold a frame | stability pass A-1; P-3 | Fixed with the user's approval: `27a034e9`, comments `af2ecc3c`, docs `80b96a74`; `3def3687` takes the ceiling from `frame_step_ceiling_ns`. Not reproduced before; `frame_step_check` (new no-host-present cases) and `rtss_limit_check` pass. Play-test: the quick menu minimised |
| F-1-14 | S1 | Replay (11) | `vk_exec.c` `vk_exec_held_ready`, `cont_ready`, `evo_resume`; `vk_evo_peek.h`; `vk_backend.c` `held_resume` | `vk_exec_held_ready` followed each event queue's head continuation without `sub_lock` while another submitting thread could free it: a read of freed memory (F-1-06) | Two guest threads in submit or present while work is held for event order (the overlap is confirmed in play: 18 'enters a submit … while 1 other' lines in one 0.10.0 log) | suspected (code re-read at `2f640fb8`; the overlap confirmed) | Each queue's head wait is published as a copy under a sequence word; the unlocked reader takes the copy, tests the event's value now and reads the counts as atomics | Low: inside the replay; no wait added | stability pass B-1; re-check of F-1-06 | Fixed: `688b2506`, `7a1c220e`. Not reproduced before; `evo_peek_check` (one million publishes, 0 mixed reads) passes |
| F-1-15 | S1 | GPU memory (12) | `vk_memory.c` `mirror_create`, `pool_fini`, `destroy_rec` | `mirror_create` put the pool on the mirrored list under `pool_lock` but set `MS_ON` after the lock dropped; a finalize in that gap skipped `mirror_forget` and left a freed record on `mlist` | A guest thread finalizing a pool while the replay makes its mirror | suspected (code read; the window is real) | Set the state under the hold that puts the record on the list | Low | stability pass C-3; L-07 | Fixed: `350c8b21`. Not reproduced before; `gpa_check`, `mem_growth_check`, `wb_lazy_check` pass |
| F-1-16 | S1 | Guest memory (4) | `memory.c` `mem_copy_readable`, `heap_lazy_fault` | `mem_copy_readable`'s quick path could fault into `heap_lazy_fault` while holding `frz_lock`: a self-deadlock (L-05's strike does not hold with the lazy heap) | `NX2NAVI_HEAP_LAZY=1` (off by default) and a copy from a heap page never committed | confirmed: reproduced against the real `memory.c` and the Windows fault handler (the copy hung) | Copy untouched lazy-heap pages as zeros instead of faulting on them | None at the default | stability pass E-2; L-05 | Fixed: `b146ee71`. `heap_lazy_copy_check` hung on the old code (exit 4) and passes |
| F-1-17 | S1 | Device and present (16), Kernel (3) | `gfx_present.c` `gfx_vsync_bind`; `kernel.c` `kern_system_event_live` | The vsync event list held 16 entries, never dropped one, and ended the process at the 17th | The game asking for the vsync event at new addresses more than 16 times in a run | suspected (code read; not reached in the logs) | A destroyed event gives its slot when the list is full | None | stability pass E-4; L-10 | Fixed: `0672655c`. Not reproduced; `vsync_bind_check` passes |
| F-1-18 | S1 | Movies (19) | `ov_movie2.c` `make_picture`, `planes_for` | Movie conversion ended the game on a failed plane allocation in its usual debanded 8-bit path, and allocated 6 MB of planes per picture | Commit exhausted while a 1080p movie plays | confirmed (code: the fatal sits on the default path) | Planes kept per decoder; a failed allocation drops the frame | Low: 6 MB per live decoder | stability pass E-5; with F-4-10 | Fixed: `0b217575`. `movie_planes_check` passes |
| F-1-19 | S1 | Audio (18) | `audio_mixer.c` routing, effect and render checks; `audio_effects.c`, `audio_reverb.c` validators; `audren.c` `need` | Values the game supplies in RequestUpdate were checked with `rt_fatal` at some 80 sites; dungeon and boss effects can reach them (F-1-10) | An effect configuration outside the checked ranges | suspected for the game; crafted updates reproduced the fatals | A framing fault refuses the update with 0x5299, as the console's renderer does; bad records and effects are left out or passed through; one log line per kind; `audio_strict = 1` ends the process for most of them, as before | Changes what the game is told (approved). The 0x5299 pass-through rests on the design's disassembly; `--probe-ipc` would confirm it | stability pass E-6; re-check of F-1-10 (L-20) | Fixed with approval: `7e7fed63`, `9770d409`, `a40afdd8`. `audio_params_check`, `audio_mixer_check` pass. Play-test: dungeons and a boss. The 'audren: renderer closed' line never reaches a normal session's log |
| F-1-20 | S1 | Textures (13), GPU memory (12) | `vk_astc.c` `vk_astc_scratch_trim`; `vk_memory.c` `vk_defer_buffer_try` | The ASTC scratch trim (`1fa8b61b`) took the decoder's lock and then waited for the upload lock: a deadlock with any thread uploading, else a wait on the presenting thread | A present after the scratch grew past 32 MiB and sat idle 5 s, while another thread uploads (likelier with `replay_async = 1`) | confirmed: `astc_trim_check` reported the deadlock on the old code | The trim only tries the upload lock and tries again at the next present | Low | stability pass L-1; review of `1fa8b61b` | Fixed: `b755cdc5`. `astc_trim_check` passes |
| F-1-21 | S1 | Guest memory (4) | `memory.c` `mem_lazy_arm`, `lazy_protect_locked`, `lazy_fault` | An arm made a range's pages inaccessible before it marked the region, and `lazy_fault` returns at once for an unmarked region: a CPU read in that window went to the crash handler as an access violation | A read of GPU-written memory while the completion thread arms it, which is when the game's fence wait returns and it reads the results; the window lasts as long as the protect and the clean test | confirmed by test: `lazy_fetch_check` arm_race (5 of 6 runs) and arm_race_slow (6 of 6) with the real fault handler | Mark the region before the protect and put the mark back if the arm is refused; a fault that finds its range gone asks the page state under `frz_lock` | Low | stability pass, second wave, lazy fetch design D2; with F-1-11 | Fixed: `ef0673fb`, `a8d18ef4`. Not reproduced in the game; `lazy_fetch_check` passes in every run since |
| F-1-22 | S1 | Audio (18) | `audio_mixer.c` `sample_at`, `adpcm_next`, the aux, capture and sink rings, `audio_mixer_open`; `audren.c` `renderer_new` | Guest addresses the mixer reads directly (wave buffers, ADPCM coefficients and context, rings inside the 39-bit limit but not mapped) are not checked: a bad one is an access violation, not a refused update. Smaller: two lines share one log-once bit, limiter and DSP gains are not checked for NaN, a sample count over 1024 ends the process at open | A game value pointing outside mapped guest memory | suspected (code read) | Check the mapping before the first read and leave the voice or effect out | Audio only | stability pass, second wave, the audio design's follow-ups; with F-1-10 | Open |
| F-1-23 | S2 | Shaders (14), Platform | `gfx_shader.c` `svc_request`, `child_exchange_raw`; `platform/win32.c` `plat_child_read` | A translator child stuck on one program held the replaying thread for 240 s: a timeout read like a broken pipe, so the request was sent again to a fresh child | A translator hang on a program (the comment above `plat_child_read` records one) | confirmed (code; `child_timeout_check` shows the two timeouts) | `plat_child_read` tells a timeout from a gone child; a silent child costs one timeout and the shader is not asked again | Low: only the hung path | stability pass D-2 | Fixed: `72047d12`. Not reproduced in the game (no hang in the logs); `child_timeout_check` passes. `linux.c` was not compiled here |
| F-1-24 | S3 | Device and present (16), Upscaling and frame generation (17) | `gfx_present.c` `create_swapchain`; `vk_fg.cpp` `vk_fg_create_swapchain`; `vk_dlssg.c` | Overlays re-hook at every swapchain rebuild (a recorder's layer crashed at that moment once, `main.c` `foreign_hook`'s comment), and RTSS's OSD on the HDR10 swapchain is unchecked | RTSS's OSD on while `vk_present`, `hdr` or `framegen` change | suspected (cannot be shown without a run) | Play-test first; any fix needs approval | — | stability pass A-19; P-17 | Open: play-test |
| F-1-25 | S3 | Process entry and boot (2), Frame pacing (9), Save data (7) | `gfx_present.c` `gfx_window_closed`, `rt_exit_begin`, `rt_exit_steps`; `svc.c` ExitProcess (0x07); `boot.c` `--seconds` | The orderly exits ran the pipeline save (up to 5 s) before RTSS's restore, the rumble stop, the frame-time summary and the save flush, so a kill or a session end in that time skipped them; ExitProcess also lacked `cfg_save` and the rumble stop | Closing while pipelines build; WM_ENDSESSION; a launcher stop during the save | confirmed for the order (code); the kills it counts on are the system's behaviour | One exit sequence for every orderly exit (`rt_exit_steps`): a wait for a Commit under way (`fs_save_wait`), RTSS's restore, the rumble stop, `cfg_save`, the summaries and a log flush before the pipeline save; `fs_save_flush` stays last | Low; the log order changes ('frametime: exit' comes before the save's lines). Relies on A-4's latch | stability pass F-1, A-6 | Fixed: `e39f71df`, `cd3f0157`; the latch's check `419e173e`. Not reproduced before; `exit_order_check` passes |
| F-1-26 | S3 | Settings and logging (8), Process entry and boot (2) | `main.c` `crash_handler`; `dispatch.c` `fatal_end`, `rt_ending_enter` | A second crash on another thread ended the process at once and cut the first report, its save-commit wait and RTSS's restore; a FATAL and a crash beside it cut each other and replaced each other's line and exit code | Two threads failing from one bad state within the first report's seconds | confirmed: `fatal_exit_check`'s new cases failed on the old guards | One guard for both ends: the first report ends the run whole, the others wait (30 s at most) and the first code stands | A cascade ends as much later as the first report takes | stability pass F-2, Fv-2 | Fixed: `1352b14f`, `66c60741`. Reproduced before by `fatal_exit_check` (two faults at once, a fault during `rt_fatal`, `rt_fatal` during a report); it passes |
| F-1-27 | S3 | Process entry and boot (2), Device and present (16), Platform | `dispatch.c` `fatal_end`; `vk_device.c` `lost_wait_exit`; `platform/win32.c` `plat_terminate`, `plat_exit` | Fatal errors and the device-lost end used `_Exit`, which in the UCRT is ExitProcess and runs every library's detach, the path `plat_exit` avoids for NGX's detach crash: a detach that faults turns 70 or 72 into a crash report, one that hangs defeats the 10 s watchdog | Any FATAL or device loss with DLSS (NGX) or Streamline loaded | confirmed for the mechanism (exit probes: detach runs after `_Exit`, not after TerminateProcess); the NGX fault itself not seen | End with TerminateProcess (`plat_terminate`); the watchdog logs one line first | Exit codes unchanged | stability pass F-3, C-4 | Fixed in the first wave (C-4's verifier marked it for approval, as it crosses `dispatch.c`, `vk_device.c` and the platform layer; F-3's let it land): `8f952d28`. Not reproduced before; `fatal_exit_check` passes. `device_lost_check` and `exit_fault_check` need approval |
| F-1-28 | S3 | Save data (7) | `fs_host.c` `file_of`, `read_file`, `write_file`, `close_file`, `file_release` | A save file's handle was used after its lock dropped: a CloseFile on another thread closed it and Windows could give the same handle to the next open, so a read returned another file's bytes (F-1-09's file half) | The game closing a save file on one thread while another uses it (a fault of the game's own) | confirmed by test: the old functions read another file's bytes | A file stays open until the last call through its handle is out; the lock is taken at the end only when a close is pending | Low | stability pass F-4; F-1-09, L-02 | Fixed: `790d201a`, `b8b57cfd`. `fs_file_use_check` failed before and passes; `save_share_check`, `save_retry_check`, `save_zip_check` pass. `--probe-fs` needs approval |
| F-1-29 | S3 | Settings and logging (8) | `main.c` `log_sink`, `rt_log_sync`, `rt_log_ending` | Past 64 MiB the log file got nothing more, the exit and FATAL lines included; `trace = perf` reaches it in about 94 minutes | A long diagnostic session | suspected (the rate from the 0.10.0 log) | Once the process is ending, lines go to the file up to a 4 MiB reserve counted on its own | The file can end 4 MiB larger | stability pass F-6, E-10 | Fixed: `bbea3ce2`, `313ef090`. `log_cap_check`, `logq_check`, `exit_order_check` pass |
| F-1-30 | S3 | Platform, Settings and logging (8) | `platform/win32.c` the thread start, `fiber_start`, `crash_veh` | A stack overflow's crash report ran on the overflowed stack with no stack guarantee, faulted again and ended with no report | Deep recursion in lifted or host code | confirmed by test: a thread and a fiber ended with 0xC0000005 and no line | A 32 KiB stack guarantee on every thread and fiber | +32 KiB committed per thread, about 5 MiB | stability pass F-11 | Fixed: `722a5dd1`. `crash_stack_check` failed before and passes |
| F-1-31 | S3 | Input (20) | `dispatch.c` `rt_rumble_stop_bounded`; `main.c` `crash_handler`; `input.c` `input_rumble_hd_stop` | Fatal errors and crashes did not stop HD rumble, and the stop took `hd_lock` blocking | A FATAL or crash while a controller vibrates | suspected (code read) | Stop it on a thread of its own that the end waits for 300 ms at most; `hd_lock` is tried | At most 300 ms more at a failed end | stability pass F-12 | Fixed: `87fd385b`. Not reproduced; `fatal_exit_check`, `rumble_hd_check` pass |
| F-1-32 | S4 | Settings and logging (8), Frame pacing (9) | `main.c` `crash_handler`, `foreign_hook`; `dispatch.c` `fatal_end`; `rtss_limit.c` `rtss_limit_crash` | The crash handler called into `RTSSHooks64.dll`, and a crash in that library on the game's own RTSS thread was blamed on another program | A crash on the RTSS thread inside RivaTuner's library (as `EnumProfiles` did, `91acff67`) | confirmed: `foreign_hook` run verbatim matches the library through 'hook' | On the 'rtss' thread the report says it is the game's own call and skips the restore; crash and fatal paths leave the profile to the next start through the state file (A-5) | Crash text only | stability pass A-14; P-13 | Fixed: `dd78132e`, `e6a6d6a8`, with A-5's `04f4d447` and its check `c19f75e5`. Not reproduced before; `foreign_hook_check`, `fatal_exit_check`, `crash_stack_check` pass. Whether loading the library before RTSS's own injection delay matters is a play-test question |
| F-1-33 | S4 | Device and present (16) | `gfx_present.c` `present_image` (the slot's fence wait, the capture wait, the acquire's results) | A lost device at the slot's fence wait was logged as 'present fence wait timed out', without a cap or the lost-device check, and the capture path read its buffer after a failed wait | `VK_ERROR_DEVICE_LOST`, or a hung GPU, at the present thread | suspected (code read) | Route the wait results through `vk_device_lost_check`, cap the lines, skip a capture whose wait failed, log a lost surface once | None outside error paths. Making the surface again needs approval | stability pass B-10; with F-1-12 | Fixed: `68bc1973`. Not reproduced before; `device_lost_check` needs approval |
| F-1-34 | S4 | Device and present (16), Upscaling and frame generation (17) | `gfx_present.c` `create_swapchain`, `movie_overlay`, `hudless_record`; `vk_fg.cpp` `vk_fg_frame`; `vk_hdr.c` | `vkDeviceWaitIdle` runs holding only the game queue's lock while AMD's frame generation may be submitting on its own queues; Vulkan wants every queue externally synchronised | `framegen = fsr3` or `fsr4` with a resize, F11, an HDR or monitor change | suspected (code read) | Measure first; any change is device and queue synchronisation (approval) | A wrong wait can free an image frame generation still reads | stability pass B-13; with C-5 | Open: measure first. The upscalers' share of these waits is gone (C-6) |
| F-1-35 | S4 | Pipelines (15) | `vk_pipeline.c` `workers_drain` | `workers_drain` dereferenced the NULL library of a queued fast-link job | `vk_pipeline_shutdown`, in probe modes only | suspected (code read) | Check the library and clear `link_queued` | None | stability pass D-12; L-15 | Fixed: `ea5087ba`. Not reproduced; compile check |
| F-1-36 | S4 | Diagnostics (22) | `hang_watch.c`; `kernel.c` `kern_wait_report`; `vk_sync.c` `vk_sync_progress` | No host-side hang report: a hang left a log that just stops | A present, submit or completion that stops | suspected (a missing diagnostic) | A 'hang watch' thread logs a report (frames, GPU progress, lock holders, guest waits) when no frame is shown with the window in front, or the GPU does not finish, for 20 s (`hang_watch_s`) | A report in a legitimately long stall only logs | stability pass F-5; 'Exits without an exit summary' (brief section 5) | Fixed in part: `b4556957`, `e4f10a5a`. `hang_watch_check` passes. The design's second part, the owner of every lock and each thread's wait site, did not land. Play-test: no report in a normal session |
| F-1-37 | S4 | Settings and logging (8) | `main.c` the exe's TLS callback; `logq.c` `logq_flush`; `gfx_present.c` `gfx_window_focus_changed` | An end with no line could not be told apart (a library's ExitProcess against an outside kill), and lines gathered before a kill were lost | The two unexplained exits (code 0, no WER event, no exit lines) | confirmed for the mechanism (a TLS callback probe) | A TLS callback leaves a line when a library ends the process with ExitProcess; the log is written at a focus loss; the module list is noted in 0.3 ms instead of 25 | None | stability pass F-13; 'Exits without an exit summary' (brief section 5) | Fixed: `b23acff3`, `359591c2`. `exit_tls_check` passes |
| F-1-38 | S4 | Device and present (16) | `gfx_window_win.c` `wnd_proc`, `exit_start`, `exit_main` | The window thread ran the whole close, so the window stopped answering for up to about 5 s and Windows showed it as not responding | Closing while pipelines build | suspected (code read) | The close runs on an 'exit' thread and the window is hidden; the hide's own focus loss is ignored | Low | stability pass F-15 | Fixed: `dc947710`, `a135c194`. Not reproduced; `exit_order_check` passes |
| F-1-39 | S4 | Platform, Save data (7) | `platform/win32.c` `con_ctrl` | A second Ctrl+C or Ctrl+Break during the close fell to the default handler's ExitProcess and cut a save commit in flight | A game started from a console; Ctrl+C twice | suspected (code read) | Wait for a commit in flight (2 s at most), then end with STATUS_CONTROL_C_EXIT | The force-quit can take 2 s longer | stability pass F-16 | Fixed: `0efa9a4c`. Not reproduced; `fatal_exit_check`, `logq_check`, `save_share_check` pass |
| F-1-40 | S4 | Files (6), Pipelines (15) | `cheats.c` `rebuild_if_needed`; `vk_pipeline.c` `vk_pipeline_boot_abandon` | The cheats rebuild's exit ran while the prewarm built and cleared neither the prewarm's marker nor the boot journal | A cheat mod whose code cheats need a rebuild | suspected (code read) | Put both away before the exit | None | stability pass F-17; F-4-11 | Fixed: `f9033dce`. Not reproduced; `exit_order_check` passes |
| F-1-41 | S4 | Settings and logging (8) | `main.c` `foreign_hook` | `foreign_hook` did not recognise Vulkan layers named `VkLayer_*`, a recorder's Vulkan hook or a driver's capture tool | A crash inside such a layer | confirmed (probe of the names) | Match them, but not the validation layer the game loads itself | Crash text only | stability pass F-19 | Fixed: `4a36e964`. `foreign_hook_check` passes |
| F-1-42 | S4 | Diagnostics (22) | `dispatch.c` `fatal_end`; `perf.c` `perf_summary_flush_try` | A fatal error wrote no frame-time summary or stutter map | Any FATAL | suspected (code read) | Write them after RTSS's restore and the rumble stop, the map only when its lock is free | About 1 ms on the failing thread | stability pass F-22 | Fixed: `c040ca6c`, `78e9d232`. `fatal_exit_check` passes |
| F-2-01 | S2 | NVN front end (10), Replay (11) | `vk_backend.c` `submit_sync` (~385-400), `ph_flush` (~366), `vk_be_present` sync branch (~546-557), `present_image` (~220-232) | The synchronous replay's front end takes no lock of its own. `ph_flush` is check-then-clear on `PH.on`, so two submitting threads can both present one held frame, and `present_image` (frame log, pending draws, `vk_dlaa_frame`, `frame_hold`, all unlocked globals) can run while another thread's replay writes the same state | Two guest threads inside QueueSubmitCommands or QueuePresentTexture at once, with a present held for event order | precondition confirmed, harm suspected. Run `p3_route`: `nvn: thread 36088 enters a submit on queue 0x134133020 while 1 other submit or present call run (overlap 1, frame 8086)`, then 31 more by frame 12847; the same in both Market runs from the pause menu's first frame. Four threads submit: one on 3 queues, the presenting thread on 4, two more on the present queue. No doubled present was seen (no "went out … after the game's present call" line in these runs) | One mutex around the synchronous `submit_sync` and the synchronous branch of `vk_be_present`; replays already serialise on `sub_lock` | Low to medium: a present may wait behind another thread's replay. No save or gameplay change, but it crosses subsystems 10 and 11: ask the user first | L-16 | **Stability pass:** fixed with the user's approval, without one front-end mutex: a lock of its own for held presents, the frame's bookkeeping under `sub_lock`, the shared counts as atomics (F-2-09: `a789bd8f`, `51c9261d` and twelve more). Not reproduced before. |
| F-2-02 | S3 | NVN front end (10) | `nvn_api_core.c` `nvnQueueSubmitCommands` (`nvn_submits_in_frame++`, ~676), reset in `nvn_api_obj.c` `nvnQueuePresentTexture` (~868) | A plain `int` is incremented by every submitting thread and reset by the presenter. A lost update gives a frame's first submission no GPU wait (the white-frame case the wait exists for) or one wait too many | The overlapping submits of F-2-01 | suspected; the overlap is confirmed | Make the counter atomic (fetch-add, exchange at the present) | None | L-16 | `ca06d5c8`. Not reproduced before, so not shown fixed. Regression: the route with the fixed exe: `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6) |
| F-2-03 | S3 | NVN front end (10) | `nvn_api_cmd.c` `resolve_deferred` (~1218-1252) | Deferred binds are resolved by copying new handles into the recording in place, without a lock. A continuation of an earlier submission of the same command handle, still held, is replayed later with the new handles | The same command handle submitted again while its earlier continuation is held | suspected | Copy the handles when a continuation of the handle is held, as the asynchronous path does | Low | L-16 | **Stability pass:** fixed by `58fad912`, `b0baa981` (F-2-09). |
| F-2-04 | S1 | Save data (7) | `fs_host.c` `save_commit` (~601-622), `commit_run` (~583), `save_touched` (~99) | Two Commits entered at once both pass `save_wait_commit`, both set `commit_pending`, and each starts a thread. The first thread clears `commit_pending` while the second still waits for `tx_lock`; a save write then passes `save_touched` and lands in the working copy while the second thread copies it. The committed save can mix files from before and after the write | Two guest threads calling Commit at the same moment, then a save write. One Commit after another (the only pattern seen) is safe: the second waits for the first | suspected (code re-read by the auditor); not seen. The transaction itself is confirmed atomic against a crash (12.3) | Count pending commits instead of a flag, or take `pend_lock` across the check and the start | Low; the save format is untouched | L-01 | `b2f9ea4c`. Not reproduced before (it needs two Commits at once). With the change `--probe-fs` still reads `a crash at each of 6 commit steps recovers to the old save 3 times, the new one 3 times, mixed 0 times; 3 backups` and `tests\save_share_check.py` passes |
| F-2-05 | S2 | Replay (11) | `vk_backend.c` `submit_sync` (~392-396) | A submission stopped at an event wait returns its fence value while its remainder is still held for a later replay. The game's wait on that value can return before the held part is even recorded; if the game then reuses what that part reads, the held replay reads newer memory | Event-order holds (dialogue scenes are the known case) | suspected; needs a counter of fence waits that return with continuations held | Signal the value only once the held remainder has been replayed, or count such returns first | Changes when the game's fence waits return (timing): ask the user first | — | **Stability pass:** changed: the counter `0f1885f9` (`next-0.10.0`) counts these returns, and `f687ede7` leaves the waits for 0 out (F-4-36); 10 lines in one 0.10.0 log, so under 600 in 20,832 waits. The fix still waits for the user. |
| F-2-06 | S1 | Services (5), Kernel (3), Audio (18) | `ipc.c` `ipc_svc` 0x21/0x22, `dispatch`, `ipc_handle_closed`; `kernel.c` `kern_handle_hold`, `kern_handle_copy`; `audren.c` OpenAudioRenderer | IPC sessions and transfer memory were used without a reference while a `CloseHandle` on another thread could free them (F-1-03) | A guest thread closing a session or the transfer-memory handle during a request on it | suspected (code re-read at `2f640fb8`) | A reference held under the kernel lock for the request and dropped through the close path; transfer memory copied under the kernel lock | Low | stability pass E-1; re-check of F-1-03 (L-09) | Fixed: `63228e7c`, `66d38dc0`. Not reproduced in a game run; `ipc_session_check` passes. `--probe-ipc` needs approval |
| F-2-07 | S1 | Kernel (3), Movies (19), Services (5) | `kernel.c` `kern_signal_system_event`, `kern_bind_system_event`, `system_event_set`, `kern_unbind_system_event`; `ov_movie2.c` `ov_dtor`; `net_offline.c` | A host thread's signal of a bound system event raced `DestroySystemEvent` and ended the process ('not a writable event handle'), or signalled an unrelated event that took the handle number; and a full table (512) evicted live events in host memory (movie decoders, network tasks), whose next signal then ended the process | A host thread (vsync, a movie decoder, a network task) signalling while the game destroys the event; a long session filling the table (2 more per reload) | confirmed: both reproduced with the real `kernel.c` (a race mode and a full-table probe) | The kept handle is checked under the kernel lock before signalling; host-memory events stay in a full table; a movie decoder and a network task give their events back when they go | None in the normal flow | stability pass E-3, Ev-1; L-10, F-1-08 | Fixed: `465ffe67`, `d8c046ac`, `d9940113`. `system_event_check` passes (the old `kernel.c` ends with 'kern_event_signal: 0x1001 is not a writable event handle'), as does `sync_wake_check` |
| F-2-08 | S1 | GPU memory (12) | `vk_memory.c` `hot_check` | `hot_check` read its records' fields for its log line, and cleared `r->hot_bytes`, after `pool_lock` was released, when a finalize could free them | `low_end_gpu = 1` (hot pools copied through their mirror) with a pool finalized meanwhile under `pool_fini_idle = 1`, whose finalize frees the record once the queue is idle; by default the record waits for the submission being recorded (`pool_wait_add`), the one `hot_check` runs in | suspected (code read). S1 for the write to a freed record, although only under a switch that is off by default (as F-1-16) | Copy the log line's fields under the lock | None | stability pass, second wave, lazy fetch review | Fixed in `2e077edc`. Compile check only (no device) |
| F-2-09 | S2 | NVN front end (10), Replay (11) | `vk_backend.c` `submit_sync`, `ph_flush`, `vk_be_present`, `present_image`, `frame_end`; `vk_present_held.h`; `vk_frame_owed.h`; `nvn_api_cmd.c` `nvn_submit_prepare` | The synchronous front end took no lock of its own: a held present could go out twice or after the next one, the frame's bookkeeping (`vk_dlaa_frame`, the frame hold, skipped draws) ran beside another thread's replay, and deferred binds were rewritten under a replay (F-2-01, F-2-03) | Two guest threads in submit or present at once, which happens in normal play (Kokiri Forest, Hyrule Field) | suspected (code read; the overlap confirmed in the 0.10.0 logs) | No front-end mutex: a held present goes out under a lock of its own that submitting threads only try; the frame's bookkeeping runs under `sub_lock`, at the present when free, else first in the next submission or on the present thread before the blit; the shared counts are atomics; a copy another thread still replays gets a clone for its binds | One new wait: the presenting thread waits on the held-present lock while another thread puts a held present out (counted on the exit line). `present_order = 0` and `present_held = 0` go back | stability pass B-2; re-check of F-2-01, F-2-03 | Fixed with approval: `a789bd8f`, `51c9261d`, `c56a8787`, `ddc0247b`, `01cb13dc`, `8b5ddd08`, `58fad912`, `0e918d48`, `518185b8`, `effdbd8d`; corrections `618cfc01`, `b0baa981`, `b18fff64`, `93e94348`, `5b5fa6e2`. Not reproduced before; `present_held_check`, `frame_owed_check`, `submit_clone_check` pass |
| F-2-10 | S3 | Frame pacing (9) | `rtss_limit.c` `rtss_limit_restore`, `rtss_limit_want`, `worker` | The next paced frame undid `rtss_limit_restore`: `rtss_limit_want` replaced the restore's -2 and the worker wrote the game's limit back into the profile before the process ended; the next run then took that limit for the player's own | Any orderly exit with frames still coming after the restore (the rumble stop, the save flush; near-certain once the restore runs before the pipeline save) | confirmed by probe: a fake RTSS around the unchanged `rtss_limit.c` left 'the profile with the game's limit after the exit', in both lanes' probes | A `closed` latch set first in `rtss_limit_restore`: `rtss_limit_want` returns on it and the worker skips under its lock | None in play: only exit paths call the restore | stability pass A-4, Fv-1 | Fixed: `a1153aa0`; the exit order's check `419e173e`. Reproduced before: five `rtss_worker_check` scenarios (restore_race, restore_first and others) failed on the old code; they pass |
| F-2-11 | S3 | GPU memory (12), Replay (11) | `vk_memory.c` `vk_mem_collect_try`, `vk_upload_flush`, `stg_create`; `vk_sync.c` `completion_main` | The completion thread's collect waited for `cmd_lock`, which upload flushes hold while they wait for the queue lock behind a present or for batch fences | An upload flush during a blocking present (FIFO, RTSS's limiter, frame generation) | suspected (code read; long `g_ql` waits measured with `lock_time = 1`) | The completion thread skips a busy `cmd_lock` up to 8 times, then waits as before | Low: deferred objects may go one completion later | stability pass B-5; with F-4-06 | Fixed: `4840f574`. Not reproduced before; `gpa_check`, `sync_hw_check`, `tex_release_check` pass |
| F-2-12 | S3 | Device and present (16), Upscaling and frame generation (17) | `vk_fg.cpp` `vk_fg_create_swapchain`, `vk_fg_frame`; `vk_fg4.cpp`; the `vkDeviceWaitIdle` calls of `gfx_present.c`, `vk_hdr.c`, `vk_fsr.c`, `vk_color.c` | Every `vkDeviceWaitIdle` holds only `g_ql` while AMD's frame generation uses its own present and acquire queues from its own threads | `framegen = fsr3` live and a device-idle wait (a size change, an HDR toggle, a rebuild) | suspected (code read) | The upscalers' share is gone (C-6). For the rest: measure, then wait on the game queue for what only it uses | Medium: frame generation's queue rules | stability pass C-5; with B-13 | Narrowed by `22c86f08` and `9065d148`: no upscaler waits for the device on the default path, unless the library's own destroy waits (the 'upscaler releases' mark shows it in play). The rest is open: measure first |
| F-2-13 | S3 | Upscaling and frame generation (17), Replay (11) | `gfx_present.c` `hudless_record`; `vk_fg.cpp` `vk_fg_frame`; `vk_dlssg.c` `vk_dlssg_frame`; `vk_dlaa.c` `D.hl`, `D.fg_*` | The present thread's frame generation reads race the hook thread's writes of `D.hl` and `D.fg_*` when the game presents from another thread than it submits | Frame generation with the game submitting from more than one thread | suspected (code read) | Belongs with the front end's order (B-2), which does not cover these fields; the upscaler change made the images' lifetime safe, not the values | — | stability pass, second wave, upscaler side finding 3; with F-2-01 | Open |
| F-2-14 | S4 | Frame pacing (9) | `rtss_limit.c` `worker`, `set_active`, `rtss_limit_active` | The worker could mark RTSS active for a want that changed after it checked: a store of `active = 0` from a new want was overwritten with 1, and the pacer stood aside for a want RTSS did not hold | A limit change, a movie, or frame generation or low latency turning on between the worker's check and its store | confirmed by test: 128,440 of 400,000 switches read active wrongly (`rtss_worker_check` active_race) | Keep the want RTSS holds (`active_for`); `rtss_limit_active` compares it with the current want | None | stability pass A-11 | Fixed: `07aca017`, `55e03cf7`. `rtss_worker_check` active_race failed before and passes |
| F-2-15 | S4 | Guest memory (4) | `memory.c` the comment above `FRZ_SHIFT`; `lazy_gone_locked` → `vk_memory.c` `lazy_fetch` | `memory.c` called `frz_lock` a leaf lock, but `lz_lock` nests inside it and a fetch took the queue lock and waited for the GPU under it | Any reader of the lock rules | confirmed (code) | The comment says what nests and what waits (again after the fetch left `frz_lock`); section 3.4 records it | None: a comment | stability pass B-12; L-05 | Fixed: `dcccf59a`; reworded with C-12 in `2e077edc` and `b5e4243c` |
| F-2-16 | S4 | Frame pacing (9) | `vk_lowlat.c` `amd_stage`, `refresh`, `vk_lowlat_present_begin` | AMD Anti-Lag's input-stage sleep holds `L.m` shared; a switch-off waits for it exclusively, and the present's stage call under the queue lock could queue behind that | AMD low latency switched off while a frame-start thread sleeps | suspected; the window is far narrower than first read (the present tests `L.on` first) | None: it cannot be provoked, and a try-acquire would change low latency's pacing | — | stability pass B-14 | Not fixed, by decision |
| F-2-17 | S4 | Movies (19) | `ov_movie2.c` `ov_decode`, `finalize`, `set_live` | `ov_decode` read `v->dav1d` without a lock and could mark a finalized decoder live again, holding the 60 Hz movie cap | DecodeAsync racing Finalize on two guest threads | suspected (code read) | Test the decoder under its lock | None | stability pass E-14; L-08, F-8-07 | Fixed: `6029321e`. Not reproduced; `movie_planes_check` passes. Play-test: 'frame rate: the movie ended' after each movie |
| F-2-18 | S4 | Audio (18) | `audren.h` the mixer interface's comment | `audren.h` said every mixer call but open, close and set_running holds `r->lock`; pace, owed and out_stats run without it, correctly | Reading the header | confirmed (code) | Comment | None | stability pass E-15 | Fixed: `88a64573` |
| F-2-19 | S4 | Movies (19) | `ov_movie.c` `find`, `destroy` | `find` returns a Movie without `g_lock` while `destroy` may free it; the VP9 path is not used by 1.1.1 | A VP9 movie method racing its destructor | suspected; no 'VP9 decoder' line in any log | None for 1.1.1 | — | stability pass E-23; L-08 | Not fixed, by decision |
| F-2-20 | S4 | Process entry and boot (2), Diagnostics (22) | `svc.c` ExitProcess (0x07); `perf.c` `perf_summary_flush`, `ft_frame`, `ft_stop` | ExitProcess and the window's close shared no once-guard, and the exit's summary raced the present thread's frame-time state | The guest's ExitProcess beside a window close; any exit while frames come | suspected (code read: torn or doubled exit lines) | The shared exit guard (F-1), and an in-flight count `ft_stop` waits for | None | stability pass F-18 | Fixed: `e39f71df`. `exit_order_check` passes |
| F-2-21 | S4 | Recompiler (1) (tests) | `tests/excl_mt.py` | `excl_mt` asserted the console's exclusive-monitor ABA behaviour, which `cpu.h`'s compare-and-swap monitor never had | Every run | confirmed: it failed at `f1a2a64e` | Check the monitor as it is: one change fails the store, a change and its restore succeed | None: a test | stability pass G-9 | Fixed: `acfc0ac2`. It passes |
| F-2-22 | S4 | Recompiler (1) | `cpu.h` `EXCL` (ldxN/stxN, ldxp128/stxp128); `recomp/lifter.py` | Latent: the lifted LDXR/STXR emulation cannot see ABA, so a guest lock-free list built on LL/SC could be corrupted | A guest pop, pop, push of one pointer between a load and its store | suspected; no fault traced to it | None without a traced corruption: a strict monitor costs every guest atomic | — | stability pass G-10 | Not fixed, by decision |
| F-2-23 | S4 | Upscaling and frame generation (17) | `vk_dlaa.c` `vk_dlaa_frame`, `presents_now` | `D.presents` was incremented plainly while the hook thread reads it atomically without a lock | Any upscaler session | suspected (a formal data race, harmless on x86-64) | An atomic store | None | stability pass, second wave, upscaler review | Fixed: `5b5fa6e2`. Compile check |
| F-3-01 | S2 | Textures (13) | `vk_texture.c` `upload` (~2404-2413, ~2451); `vk_astc.c` `vk_astc_record` (~566-572) | When the GPU ASTC decode cannot be recorded (its scratch buffer of 32 MiB or more fails to allocate, or no pipeline), `upload` logs "not decoded on the GPU: left as it was" and still sets `uploaded = 1`. The image keeps what its memory held before. It is not retried: the content hash was taken from the storage, so equal bytes written again are skipped as unchanged | An allocation failure, which needs the commit or the device heap exhausted (the budget cap never fails an allocation), at an area load. This is one candidate for the white ground after long play | suspected (code re-read by the auditor). Not reproduced: the line "not decoded on the GPU" is in none of the logs here, the user's recent ones included. A capped run cannot fail an allocation, so it would not reproduce this | On a failed record leave `uploaded` and `chash_ok` clear so the next lookup retries, with a limit, or fall back to the CPU decode | Low, textures only | L-21 | `054bd556`. Part only: the content hash is forgotten after a failed decode, so the next write uploads. A texture whose decode failed is still not uploaded again until its storage is written: that part is open. Not reproduced before. Not reproduced before, so not shown fixed. Regression: the route with the fixed exe: `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6). **Stability pass:** the map refresh found the retry on `next-0.10.0`: `29eb7cf0` (merged by `2ccd69ef`) leaves a texture whose GPU decode had no memory not uploaded and tries it again every 30 frames, with `ccc31ad5`. A WriteTexels decode without memory is still only logged. `1fa8b61b`'s scratch trim makes that path run at area loads under pressure; the trim's deadlock is fixed (`b755cdc5`, F-1-20). |
| F-3-02 | S2 | Textures (13), Replay (11) | `vk_texture.c` `held_out` (~1853), `slot_resolve` (~3109-3124), `evict_disused` (~1863); `vk_exec_draw.c` `make_dummy` (~354-382) | A texture evicted for disuse leaves a hold that ends only at a shader mark or a lookup outside the pool refresh. If a pool-indexing shader that cannot be marked is built after evictions have started, further evictions stop but the keys already held stay held, and what only that shader samples shows its stand-in for good. The stand-ins are 1x1 images moved from UNDEFINED to GENERAL with no clear, so their colour is undefined | VRAM over 80 % of the budget or low commit, textures unsampled for 120 s, then an unmarkable pool shader. A second candidate for the white ground | suspected (code re-read by the auditor for `make_dummy`). The run planned for it (the route under a 6 GiB cap) was not done before Phase 3 was stopped | Clear the holds (and cancel retiring textures) when the first unmarkable shader appears; clear the stand-ins to a defined colour | Low; gives back VRAM that was freed | L-22 | **Stability pass:** changed: the disuse eviction is off by default (`84b0672e`, `TEX_DISUSE_EVICT` 0) and the stand-ins are cleared (`0d564a21`). The holds remain with `tex_disuse_evict = 1`. |
| F-3-03 | S2 | GPU memory (12) | `vk_memory.c` `refuse` (~1745), `vk_mem_use` (~2140-2146) | `refuse` sets a retry time only for a budget refusal (`r->retry_sub = why == MR_BUDGET ? … : 0`). A pool whose mirror allocation failed (`MR_ALLOC`) stays refused until it is finalized and is read over the bus for the rest of the session | A device-local allocation failure (heap or commit exhausted) at the pool's first use | suspected (line re-read by the auditor). Not reproduced: `failed 0` in every `mem: mirror` line of every run here; the budget cap gives budget refusals, never this one | Give `MR_ALLOC` the same retry, and keep the refusal reason in the record so the counters stay right | Low | L-23 | `94ee6e78`. Not reproduced before, so not shown fixed. Regression: the route with the fixed exe: `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6) |
| F-3-04 | S3 | Audio (18), Kernel (3) | `audio_mixer.c` change events (~1213-1218, ~1296, ~1365) | Each call of the audio device's change-event commands makes a new kernel event, which takes a slot of the handle table (`MAX_H` 4096, fatal when full) and is never closed; events past the 16th are never signalled | Each call of those commands. Handles grew 112 → 116 and 112 → 117 across one reload (section 11.3); which calls took them was not traced | suspected; the handle growth is measured, its source is not | One shared event per kind, as the other audio commands already use | Low | L-25 | **Stability pass:** fixed by `d3a5c3c3`, `ef2e5e24` (F-3-16). |
| F-3-05 | S3 | Services (5) | `ipc.c` domain objects (`objects[32]`, `ipc.h` ~54; CloseVirtualHandle ~428) | A domain's object table is never compacted and closing a virtual handle frees nothing; the 33rd object is answered with an unknown-command error | A domain session that opens and closes sub-objects repeatedly (the applet proxy), for instance at each return to the title | suspected; needs a run with `NX2NAVI_TRACE=ipc` counting the commands per title return | Free the slot on close and reuse it | Low | L-25 | **Stability pass:** fixed by `e9becc12`, `2c59addd` (F-3-17). |
| F-3-06 | S4 | Platform | `platform/win32.c` `sleep_timer` (~478), `plat_file_pread` / `plat_file_pwrite` events (~1267, ~1526), `plat_thread_detach` (~73) | Per-thread timer and event handles are never closed when a thread exits, and a detached thread's small record stays. One "save commit" thread per changed Commit and one "camera speed" thread per search leak about two handles each | Every save and every sensitivity change | suspected; tens per hour against a limit of millions | Close them when the thread ends | None | L-26 | **Stability pass:** fixed on `next-0.10.0` by `222a025c`; `tests/thread_end_check.py` covers it. |
| F-3-07 | S4 | GPU memory (12) | `vk_headroom.c` `room` (~112-120) | The headroom thread measures against the driver's budget, not the capped one, so under `NX2NAVI_VRAM_BUDGET_MB` it still keeps its spare blocks. A capped test run understates what a small card does | Any capped run | suspected (code read). The cap is a test switch | Compare against the smaller of the driver's budget and the cap | None outside capped runs | L-27 | **Stability pass:** changed after the 0.10.2 candidate: with a cap set, `room` also compares the device-local use with 85 % of the capped budget (`vk_mem_vram`), so a capped run keeps no spare block where the card it stands for keeps none. Without a cap nothing changes. Not run: a capped session is the user's to play. |
| F-3-08 | S3 | Replay (11) | `vk_usnap.c` `vk_usnap_drop` (~433), `vk_usnap_begin` (~358), `arena_get` (~90) | With uniform snapshots switched off (`NX2NAVI_UBO_SNAPSHOT=0`, not the default) the GPU-write list is appended on every write and never pruned, and its linear scan slows without limit. In the default mode the list is pruned (199-250 entries at every snapshot) but the free arenas are never trimmed (3-6 arenas, 12-24 MiB measured) | Only with the switch off | suspected for the off mode; the default mode's sizes are measured and flat | Return early in `vk_usnap_drop` when snapshots are off; trim arenas idle for some submissions | None | L-29 | `3966d878`. Non-default mode only; not run in that mode. Not reproduced before, so not shown fixed. Regression: the route with the fixed exe: `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6) |
| F-3-09 | S4 | Replay (11) | `vk_exec_draw.c` `mesh_ring_entry` (~5044-5053), `mesh_count_buf` (~4990) | The allocation handles of the mesh ring (48 KiB) and the count buffer (16 KiB) are dropped, so neither is ever freed | Once per process | suspected; a fixed 64 KiB | Keep the handles and destroy them at shutdown | None | L-30 | **Stability pass:** fixed on `next-0.10.0` by `a8c6e992` (`mesh_ring_release`). |
| F-3-10 | S3 | Textures (13) | `vk_texture.c` `upload` (~2336-2342 with ~2272) | When a texture's storage is released during its upload it is uploaded as zeros, but the content hash already holds the bytes from before. If the game maps the same bytes back, `same_content` matches and the upload is skipped: the texture stays zero | Storage unmapped in the middle of an upload, then mapped again with equal content | suspected (code read) | Clear `chash_ok` whenever the bytes that reached the image are not the storage's (the same change as F-3-01) | Low | — | `054bd556`. Not reproduced before, so not shown fixed. Regression: the route with the fixed exe: `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6) |
| F-3-11 | S4 | GPU memory (12) | `vk_memory.c` `refuse` (~1747) | The log quota for mirror refusals is shared by all reasons (12 lines), so allocation-failure refusals can go unlogged behind budget refusals; only the `failed` count in the `mem: mirror` line shows them | Many budget refusals before the first allocation failure | suspected (code read) | A quota per reason | None | — | `94ee6e78`. With F-3-03. Not reproduced before, so not shown fixed. Regression: the route with the fixed exe: `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6) |
| F-3-12 | S4 | Shaders (14) | `gfx_shader.c` memory cache (`buckets`, `mem_insert`) | The shader memory cache keeps every shader's SPIR-V and reflection for the life of the process, after its pipelines are built | Always | confirmed size: `shader memory cache 3564, 386.2 MiB` to `3676, 395.4 MiB` in every area, flat across the reload (section 11.3). It is bounded by the game's shaders, not growing | Drop the SPIR-V of shaders whose pipelines are all built, keeping what later variants need | Medium: a later pipeline variant would have to read the shader again from disk | L-24 | **Stability pass:** changed: the "shader compress" thread keeps idle SPIR-V compressed (`3573a96f`, `d499a828`): 714.2 MiB down to 187.4 MiB in the 0.10.0 log. The cache is still kept for the whole process. |
| F-3-13 | S3 | NVN front end (10) | `nvn_api_cmd.c` `copy_add`, `retire_main`, `retire_pass`, `ctl_fingerprint` | NVN recording copies grow for good (from 220 copies, 0.1 MiB, to 223,753, 58.3 MiB, in the 0.10.0 log), and nothing counted the ones whose control memory the game reused | Every area load | confirmed (growth in the fresh log) | Stage 1, count only: `copy_retire = 1` starts a sweep thread that fingerprints the copies and counts changed and gone control memory | None at the default (0) | stability pass E-7 | Fixed (stage 1): `811024e6`, `168a80f2`, `b072b8a2`, `601a9546`. `copy_sweep_check` passes. Stage 2 is E-8 |
| F-3-14 | S3 | NVN front end (10) | `nvn_api_cmd.c` the sweep, `copy_touch`, `nvn_submit_prepare` | Retiring the copies whose control memory changed waits for stage 1's numbers | Long sessions: about 38,000 copies (10 MB) more per area return | suspected | A mode 2 that retires with tombstones at every lookup | A wrong retirement drops that recording's draws | stability pass E-8 | Open: measure first (`copy_retire = 1` in a long session) |
| F-3-15 | S3 | Kernel (3), Platform, Diagnostics (22) | `kernel.c` `kern_sizes`; `perf.c` `hc_main`; `platform/win32.c` `plat_handle_count`, `plat_handle_census` | OS handles grow at area loads (683 to 1,095 over 11 minutes) while guest handles stay flat; the sizes line lacked the count and nothing told the kind | Every area load | confirmed (`memory-sampler.csv` of the 0.10.0 session) | 'process handles' on the sizes line; `handle_census = 1` with `trace = perf` logs handles by kind | None at the default | stability pass E-9 | Fixed (the diagnostic): `26b7cfd6`. `handle_census_check` passes. The growth is open until a play-test names the kind |
| F-3-16 | S3 | Audio (18), Kernel (3) | `audio_mixer.c` `change_event_new`, `audio_device_handler` 4/11/12/15/17, auddmg 500 | Audio device notification commands made a new kernel event per call, never closed; past 16 they were never signalled (F-3-04) | Each such call | suspected (code read; the handle growth is measured, its source not) | One event per kind; the loser of a race closes its own; a channel change skips a slot not stored yet | Low | stability pass E-11; re-check of F-3-04 (L-25) | Fixed: `d3a5c3c3`, `ef2e5e24`. Not reproduced; `audio_mixer_check` passes |
| F-3-17 | S3 | Services (5) | `ipc.c` `dispatch` (CloseVirtualHandle), `ipc_domain_add`, `applet_proxy_handler`, `applet_oe_handler` | Domain objects were never freed nor their slots reused, and were written without a lock; the 33rd object failed (F-3-05) | A domain session opening and closing sub-objects again and again | suspected (code read; not counted: no `trace = ipc` log) | Free closed objects and reuse their ids under a domain lock; keep a domain alive while its objects are | Low | stability pass E-12; re-check of F-3-05 (L-25) | Fixed: `e9becc12`, `2c59addd`. Not reproduced; `ipc_session_check` passes |
| F-3-18 | S4 | Pipelines (15) | `vk_pipeline.c` `pb_init`, `pb_index`, `pb_flush` | With `pipe_binaries = 1`, a torn append made the next start drop the whole `pipeline-binaries.bin` | A kill or crash during the autosave's append, or a short write | suspected (code read; an opt-in path) | Keep the whole records and cut the file back (`plat_file_truncate`); cut a failed append back out | Low | stability pass D-13 | Fixed: `79c41ca7`, `75b1e0b5`. `pb_torn_check` passes |
| F-3-19 | S4 | Input (20), Guest memory (4) | `camera_speed.c` `rewrite` | The camera-speed search skips untouched lazy-heap pages only where a run of them starts a piece; a 16 MiB piece that begins in touched memory reads, and so commits, the untouched pages after it | `NX2NAVI_HEAP_LAZY=1` (off by default) and a sensitivity change | suspected (code read; it predates E-13) | Skip untouched runs anywhere in a piece | Low | stability pass, guest branch review, with E-13 | Open |
| F-4-01 | S2 | Replay (11), Textures (13) | `vk_exec_draw.c` `pool_unres` (~2231-2244), `pool_sync_entries` (~2496-2510) | The list of texture pool slots that resolve to no entry grows duplicates: a slot that resolves between two walks has its flag cleared but stays listed, and when it stops resolving `pool_unres` appends it again. Every 8th pool check walks the whole list, calling the slot retry for each copy, under the texture lock. In the Market the list reaches several times the number of slots that exist, and the walk becomes a stall | Standing in the Market (adult) for about a minute. It then repeats every 8th frame; game threads registering textures wait for the lock too | confirmed (12.3): `slots without one` 134371 and 160385 in `p3_market_t1`, 115652 in `bl_market_1`, 88824 in `bl_market_2`, against 32768 slots; the walk's own timer gives about 15 ms a walk in the last window; `texture lock held over 1 ms: 7792 ms (581 holds) … max 33.61 ms` from `bind_resources`; `slow replay frames 225 … its slowest op 20.0 ms (DRAW_MESH 225)`; 375 of 380 gaps between slow frames are 8 frames. In `p3_market_t2` the list fell back to 1273 and no stall came: it depends on timing | Never list a slot twice (drop the stale entry when its flag clears, or check membership), and bound the retries per walk | Low: the walk's result is unchanged, only its length. Textures and replay only | L-44 | `015a89dc`. Before: 134371 and 160385 list entries, walks of about 15 ms, 225 frames over 25 ms standing in the Market (`p3_market_t1`). After: three runs of three (`p4_market_1` to `_3`): the list stays at 524-3192 entries and the walk at 0.008-0.026 ms per switch; standing after the pause tour no frame reaches 20 ms (worst 14.2, 17.3 and 18.3 ms at 118.5, 145.7 and 139.6 fps); the texture lock is held over 1 ms for at most 149 ms in a period (longest hold 4.34 ms), against 7792 ms (33.61 ms) before. **Stability pass:** the 0.10.0 field log stays at 3,662 'slots without one' at most. |
| F-4-02 | S3 | Upscaling (17) | `vk_dlaa.c` `evaluate` (~1645-1675), `release_kept` | A DLSS feature is created on the replaying thread the first time a render size is used, after a device idle wait when kept features are released first | The first frame of the scene after a load, the first frame of the pause menu (its own size), and each reload | confirmed with the mark added in `c85892ab`: `DLSS feature creates 23.5 ms` in the load frame, `16.4 ms` of a 22.4 ms frame at the pause menu's first frame (`p3_market_t1` frame 5663), `43.8 ms` and `44.9 ms` at the reload (`p3_route` frame 20156, `p3_route_cold` frame 18952) | Create the feature for the known sizes ahead of use (at boot or during the load), or on a worker with the previous output shown meanwhile | Medium: one more thread on the device; no gameplay change | L-47 | **Stability pass:** changed: `release_kept`'s and the other upscaler device-idle waits are gone (F-4-20: `22c86f08`, `9065d148`); the feature is still created on the replaying thread. `dlss_range` (`8273d881`) would share one feature across a mode's sizes but is off by default. |
| F-4-03 | S3 | Replay (11) | `vk_exec.c` / `vk_exec_draw.c` replay of the first frames after a load | One or two frames some 10 frames after a load's big frame take 48-95 ms. With the columns added in `df6766aa` they read as replay time and nothing else: `slow frame 2122: 95.0 ms; blocked pipeline 0.0 xlate 0.0 texture 0.0 astc 0.0 fence 0.0 pacing 0.0 drain 0.0; replay 93.6` | Every save load | confirmed that the time is the replay's; which part of it is not determined (the timed Market run's nearest frame shows 2867 draws against the usual 1250-1900, slowest single draw 8.7 ms) | Not proposed until the part is named (a timed run with `NX2NAVI_REPLAY_TIME=1` on the route) | — | L-47 | **Stability pass:** the part is named: the texture pool's area load inside the replay (F-4-19). Open. |
| F-4-04 | S2 | GPU memory (12) | `vk_memory.c` `pool_fini` (~382-387), `pool_init` | A pool's finalize flushes the uploads and calls `vkQueueWaitIdle` under the queue lock, on a guest thread, once per used pool; the lock blocks the render thread's submits and the present for that time. Pool initialize imports and allocates on the calling thread | Area changes and boot, when the game makes and frees many pools | confirmed for the time, suspected for the idle wait as its cause: `slow frame 459: 45.8 ms; … on any thread: pool init/finalize 45.8 ms (2, 288.0 MiB, longest 43.1 ms)` and `slow frame 522: 58.5 ms; … pool init/finalize 56.7 ms (71, 94.4 MiB, longest 3.0 ms)` (`bl_route_1`). The mark does not tell initialize from finalize | Wait for the pool's last fence instead of the whole queue, without holding the queue lock | Medium: the wait must still cover uploads and a present not yet drained | L-36 | **Stability pass:** changed: `83faa1da` (`next-0.10.0`) waits for the submissions issued so far instead of the idle queue, not under the queue lock; `e70cbb6a` destroys finalized pools outside every lock (F-4-18). Still a GPU wait per used pool on the guest thread. |
| F-4-05 | S3 | Guest memory (4), GPU memory (12) | `memory.c` `lazy_drop_locked` / `lazy_piece_locked` (~474-507), `vk_memory.c` `lazy_fetch` (~2600), `mirror_evict` (~1829) | A lazily written range is fetched with a GPU copy and a wait inside the fault handler while `frz_lock` is held, which stalls every other fault, thaw and the replay's skip check for that time. `mirror_evict` does the same while holding the pool lock exclusively | A CPU read of an armed range, a file read near GPU pools, or a mirror eviction, with GPU work queued | suspected (code read); no deadlock path was found. The AMD-crash part of the lead is struck: lazy write-back is off by default on that driver | Mark the range as being fetched and drop `frz_lock` for the copy and wait | Medium: touches the piece protocol; no gameplay or save change | L-04 | **Stability pass:** fixed with the user's approval by `2e077edc` and its corrections (F-4-22); narrowed before by `1dda8bc2` and `ebb37689`. |
| F-4-06 | S3 | Device and present (16), GPU memory (12) | `gfx_present.c` `flip_locked` (~306-316); `vk_memory.c` `vk_upload_flush(1)` (~3780-3791) | The present call runs under the queue lock that `queue_submit` waits for, and a blocking upload flush waits for its fences while holding `cmd_lock`, which every upload, deferred free and the completion thread need | A present that blocks (vsync modes, frame generation); texel reads, pool finalizes and menu art uploads | suspected (code read) | Note the fence, unlock, wait, lock again; a present queue of its own where the device has one | Low for the flush; medium for the queue | L-33 | **Stability pass:** narrowed: the completion thread's collect gives way to an upload holding `cmd_lock` (`4840f574`, F-2-11), and the queue lock's holders are named (`0ffac802`, F-4-34). The present still runs under `g_ql` (F-4-13, measure first), and `vk_upload_flush(1)` still waits under `cmd_lock`. |
| F-4-07 | S3 | Replay (11), Device and present (16) | `vk_backend.c` `submit_sync` drain (~389), `gfx_present.c` present thread (~253, ~1339-1349) | Before each submit the game's thread waits for the present thread's whole job (slot wait, acquire, menu, its empty submission). Held work can then be replayed on the present thread, which runs at normal priority off the reserved core | A blocking acquire, or held work resumed by the present thread's empty submission | suspected (code read). The wait now shows as `drain` on the slow-frame line; it was 0.0 in every slow frame of the Phase 3 runs at these settings | Replay held work only on the submitting thread, or raise the present thread's priority | Low | L-32 | **Stability pass:** still open; the hand-off is timed since `04727340`. |
| F-4-08 | S3 | In-game menu (21) | `ui/menu.cpp` `menu_overlay_frame` (~1729-1740), `vk_up` (~297); `ui/menu_art.cpp` `menu_font_upload` (~198), `upload_rgba` (~160) | With the overlay menu open, the settings file is written, and fonts and art are uploaded with queue idle waits, on the present thread under the present lock, which the game's next submit waits for | Opening the overlay menu, changing a setting or the scale | suspected (code read); the audio part of the lead is struck (it runs on the audio thread, on a change only) | Save settings on a worker; batch the art uploads; retire the old font with a deferred free | None to gameplay | L-38 | **Stability pass:** still open. |
| F-4-09 | S3 | Input (20), Guest memory (4) | `camera_speed.c` `search_main` (~218-247) | The search for the camera speed array reads the whole guest heap, including every lazily written page, so each such piece faults and is fetched from the GPU (F-4-05) while the game runs | Changing the stick sensitivity mid-game | suspected (code read); the crash and race parts of the lead are struck (each piece is read under a hold, writes thaw first) | Skip ranges that are lazily armed | Low | L-13 | **Stability pass:** fixed by `af061602` (F-4-26); `420aef04` had already skipped the lazy heap's never-committed pages. |
| F-4-10 | S4 | Pipelines (15), Movies (19) | `vk_pipeline.c` `vk_pipeline_compute` (~4949-4960); `ov_movie2.c` `run_stripes` (~287-301) | A dispatch whose compute pipeline another thread is building polls with 1 ms sleeps and the wait is not timed as a pipeline wait; each movie picture starts and joins four threads and allocates its planes anew | A compute pipeline not built yet; movies | suspected (code read) | A condition variable and the pipeline timer; a small worker pool and reused buffers | None | L-34 | **Stability pass:** the movie half fixed on `next-0.10.0` (`8106fdaa`), and F-1-18 keeps the planes per decoder (`0b217575`). The compute poll is open. |
| F-4-11 | S2 | Pipelines (15) | `vk_pipeline.c` `pm_set` (~4276), `vk_pipeline_prewarm_marker_clear` (~4284; called only from `boot.c` ~379 and the prebuild's end ~4223), `pm_skipped` (~4241) | `pipeline-prewarm-running.txt` exists while the boot builds the recorded pipelines, so that a driver crash there is noticed. No orderly exit clears it. A game closed during that boot build leaves it, the next start reads that as "the last start died", writes `pipeline-prewarm-skip.txt`, and from then on every start on that GPU and driver skips the prebuild: each recorded pipeline is built at its first draw, in play | Close the game window (or end the process) while the boot still builds pipelines, then start again | confirmed by an incident in the shared run folder: at 22:50 a scripted run was closed by its runner during boot (most likely this session's first `p3_market_prof` attempt, which gave way to another game then; its log was replaced by the repeat, so this is not proven), and the next start, another session's `out\gate_off`, logged `vk_pipeline: the last start died while the boot built pipelines (E:\nx2devrun/pipeline-prewarm-running.txt was left); E:\nx2devrun/pipeline-prewarm-skip.txt now skips that prebuild on this GPU and driver (10de:2b85:9b404000)`. Every later start there logs `prewarm: 4063 recorded pipelines not built ahead of play`. No driver crash had happened. The file is still there: it was not deleted, as other sessions run in that folder | Clear the marker on every orderly exit (the window close, the guest's exit, the runner's close), so only a real crash leaves it; give the skip an expiry or a retry | Low; pipelines only | — | `6c0b0b10`. Reproduced and verified with `route\prewarm_close.py` (the window closed one second after the marker appears, each exe on its own pipeline cache folder). Before the fix (`ocarina_audit_p3b.exe`, `p4_prewarm_old`): `marker seen 2.8 s after the start … after the exit the marker is LEFT`, and the next start `took the first for dead; skip file WRITTEN`. After (`ocarina_audit_fix.exe`, `p4_prewarm_fix`): `after the exit the marker is gone`, next start `no such line; skip file absent`. A process that is killed still leaves the marker, as meant |
| F-4-12 | S2 | Frame pacing (9), Movies (19) | `frame_rate.c` `frame_step_cap_ns`; `nvn_api_obj.c` `pace`; `rtss_limit.c` `worker`; `rtss_decide.h` `rtss_target` | Under RTSS the pacer kept no movie slots (period 0) while `ocarina_step` gave one movie frame per update, so each late frame put the picture behind the sound for good; every movie start and end also rewrote RTSS's profile | RTSS holding during any movie; at limits of 30-45 RTSS also held the movie under its own 60 | suspected (code read) | The pacer holds the movie's rate and keeps its slots whatever RTSS does (`rtss_limit_movie`); the profile keeps the play limit through a movie: no write at a limit of 60 or more, no limit while a movie outruns a lower one | Pacing of movies under RTSS (approved). At exactly 60 RTSS keeps holding 60 beside the pacer through a movie; whether it lets the catch-up frames through is not measured | stability pass A-2; P-2 | Fixed with approval: `208a1071`, `ad84e0a1`, docs `80b96a74`. Not reproduced before; `rtss_limit_check`, `rtss_worker_check`, `frame_step_check` pass. The reviewer's option (a), no limit in the profile during every movie, waits for the user's word. Play-test: a movie with `TEST_HITCH=120:70` |
| F-4-13 | S3 | Device and present (16), Replay (11) | `gfx_present.c` `flip_locked`; `vk_sync.c` `queue_submit` | RTSS's limiter sleep and OSD run inside the hooked `vkQueuePresentKHR`, which `flip_locked` calls holding `g_ql` and `g_pl`, so the sleep blocks every submission, upload and the completion thread's flush | Any frame while RTSS holds; also a blocking FIFO present, AMD's frame generation, Streamline | suspected (code read; not measured) | Measure first with the present-call and queue-lock fields. A present on a queue of its own needs approval and keeps F-4-06's constraints (overlays, the reserved frame generation queue, single-queue families) | Device and queue creation and the present path | stability pass A-3; P-1; with F-4-06 | Open: measure first. The instruments landed: `04727340` times the present call and the hand-off, `0ffac802` names the queue lock's holders |
| F-4-14 | S3 | Frame pacing (9), Settings and logging (8) | `rtss_limit.c` `apply`, `put_back`, `left_start`, `restore_left`, `rtss_limit_crash`; `rtss_decide.h` `RtssSaved` | After a kill, hang or crash the game's limit stayed in `Profiles\<exe>.cfg`; the next start took it for the player's and put it back at every later exit. The frozen `ocarina.exe.cfg` of 2026-11-08 is such a copy | Any exit that skips the restore: Task Manager, a hang, the device-lost watchdog, a crash | confirmed by probe (kill1 and kill2 against a fake RTSS) | `rtss-profile.txt` next to the exe keeps each profile as it was, written before the run's first change and removed once the profile is back; the next start puts it back first, unless the player changed it since; crash and fatal paths leave it to that start, with no call into RTSS's library | A new file in the user folder (approved). A state file that exists but cannot be read counts as empty at that start, so its restore waits for a later one | stability pass A-5; P-4 | Fixed with approval: `04f4d447`, corrections `a7b4f0c7`, `da822abe`, `9f45d744`, `af2ecc3c`, docs `405a75f3`. Not reproduced in a game; `rtss_limit_check` (state-file cases) and `rtss_worker_check` (kill and restore scenarios) pass. Play-test: a Task Manager kill |
| F-4-15 | S3 | Frame pacing (9) | `rtss_limit.c` `worker`, `rtss_running`; `rtss_decide.h` `rtss_decide` | The game's limiter stood aside once RTSS listed the pid and the profile read back, before RTSS was seen holding the game; a stale period failed RTSS for the run, and 15 % over the limit was never caught | RTSS's injection delay (15 s in both profiles here), a limit lowered and the window minimised at once, an RTSS that holds loosely | confirmed by probe on the real worker (listed_no_api, stale_fps, over115) | Stand aside only when RTSS lists the game's Vulkan presents, has read the profile, measured a fresh period and pictures go out; give up at 1.05 times the limit over three fresh periods, and after 30 s listed with the limit without proof of its hold | Changes when the pacer stands aside (approved). The 30 s give-up goes beyond the approved rules and needs the user's word: reverting `1744681f` alone drops it | stability pass A-7; P-6 | Fixed with approval: `f7899e54`, `1744681f`, comments `af2ecc3c`. Not reproduced in a game; `rtss_limit_check` and `rtss_worker_check` pass. Play-test: the first 30 s after 'has hooked the game' |
| F-4-16 | S3 | Frame pacing (9), In-game menu (21) | `rtss_limit.c` `watch`; `rtss_decide.h` `rtss_out_of_limit`, `rtss_two`; `frame_rate.c` `frame_step_cap_ns`; `ui/menu.cpp` | With `rtss = 0`, frame generation or low latency, RTSS's own Global or per-exe limit stacked on the game's limiter unreported, and the kept-out line left low latency out | A limit, frame time limit or scanline sync set in RTSS while the game keeps RTSS out | suspected (code read) | Kept-out reasons per cause (`RTSS_OUT_*`); the worker reads RTSS's settings for the game read-only and logs two limiters, with the menu note `UI_MENU_FPS_RTSS_TWO`; also for a read-only profile, a library that did not load, and with Reflex named | None to pacing: log and menu note | stability pass A-8; P-5 | Fixed: `b0488796`, `4c919882`, `2150458a`, `fa229f94`, `87777fea`. Not reproduced before; `rtss_limit_check`, `rtss_worker_check`, `frame_step_check` pass. Play-test: a Global limit with `rtss = 0` |
| F-4-17 | S3 | Upscaling and frame generation (17), Frame pacing (9) | `dlssg/nx2_sl.cpp` `nx2sl_reflex`, `nx2sl_reflex2`; `vk_dlssg.c` `vk_dlssg_pace`; `dlssg_pace.h`; `frame_rate.c` `frame_step_ceiling_ns` | With DLSS Frame Generation Reflex slept at the frame start with no frame limit while the game's pacer still held the present: a second wait, and the latency low latency removes elsewhere | `framegen = dlss` on RTX 40 or 50 | suspected (code read; no RTX 40 or 50 here) | A new export hands Reflex the limit (`frameLimitUs`); Reflex holds it and the pacer keeps only its ceiling, except during movies, after 200 ms without a host present and when Reflex is seen not holding; `reflex_limit = 0` gives it back to the pacer | Pacing with DLSS-G (approved). Needs the rebuilt `nx2navi_dlssg.dll`: a release and a kit now refuse the 0.8.0 one | stability pass A-16; P-16 | Fixed with approval: `660c42f6`, `2d6f9f07`, `2bf32649`, `dc3f424f`, `e0843439`, `9f4f51f0`; corrections `0131b1b2`, `46bf4823`, `358713fb`, `bb0317ac`, `879c76f9`. Not reproduced; `dlssg_pace_check`, `dlssg_exports_check`, `frame_step_check` pass. Play-test on RTX 40 or 50 |
| F-4-18 | S3 | GPU memory (12) | `vk_memory.c` `pool_retire`, `destroy_rec`, `pool_wait_collect`, `pool_dead_drain`, `vk_mem_collect`; `vk_sync.c` `completion_main` | Finalized pools were destroyed inside the completion thread's callbacks before 'done' was published, so every fence waiter waited for those frees, and under `cmd_lock` by whichever thread collected first, the replaying thread's upload flush and the presenting thread among them | Area loads and returns, when the game finalizes hundreds of pools | suspected (code read; the 0.10.0 log's per-callback lines name `pool_retire`) | `pool_retire` only moves the record to a dead list; `vk_mem_collect` destroys outside every lock; the destroys are timed per thread on the `mem: sizes` line | Low: one subsystem | stability pass B-4, Bv-1; with F-4-04 | Fixed: `e70cbb6a`, timing `c4140a3f`. Not reproduced before; `gpa_check`, `mem_growth_check`, `sync_hw_check`, `tex_release_check` pass. Whether `pool_fini`'s own drain should stop waits for the play-test's numbers |
| F-4-19 | S3 | Replay (11), Textures (13) | `vk_exec_draw.c` the texture pool's area load; `vk_backend.c` `frame_hold` | F-4-03's part is named: the long replay frames after a load are the texture pool's area load (1,500-2,400 textures made in one or two frames inside the replay, with hundreds of MiB of device allocations) | Entering an area whose textures the pool has not made | confirmed: 'an area load of about 2442 textures was made in 2 frames', then 'slow frame 23089: 121.2 ms … replay 113.8' (0.10.0 log) | None in the replay: the cost belongs with the first visit's work (C-10 times the allocations). Measure again with `replay_time = 1` | — | stability pass B-6; F-4-03, L-47 | Open: measure first |
| F-4-20 | S3 | Upscaling and frame generation (17) | `vk_dlaa.c` `free_size`, `free_snap`, `stale_sizes`, `release_kept`, `hudless_from`, `pb_images`, `retire_idle`; `vk_ffx.cpp` and `vk_ffx4.cpp` `context_for`; `vk_up_defer.h` | Twelve device-idle waits on the replaying thread with `g_ql` held (seven in `vk_dlaa.c`, two in `vk_ffx.cpp`, three in `vk_ffx4.cpp`), each a stall at an upscaler size change; F-4-02 measured 16-45 ms with the create | A DLSS or FSR size or mode change: the pause menu, a reload, dynamic resolution, an idle feature | confirmed (code; the cost measured for DLSS in F-4-02) | What an upscaler lets go of goes on a list freed once the GPU is past it; a refused creation waits on the timeline for those submissions only and tries again; perf marks name the waits and the releases | The libraries' rules for releasing off the evaluating thread (approved). `upscaler_idle_wait = 1` brings the device waits back and keeps the deferral | stability pass C-6, Cv-1; with F-4-02 | Fixed with approval: `c6b29834`, `86e6d62d`, `22c86f08`, `9065d148`, `ed87d306`; corrections `61f08a17`, `45069883`, `74bedbee`, `75b06b0a`, `cde143c5`. Not reproduced before (no DLSS or FSR session log here); `up_defer_check`, `upscaler_idle_check` pass. `ffx_cache_check` needs approval |
| F-4-21 | S3 | Textures (13) | `vk_texture.c` `sweep_hard`, `drop_tex`, `write_back_n`; `vk_tex_release.h` `trl_hard_class` | The hard-count sweep (`2969005e`) drops a batch in one walk, and each GPU-drawn entry among them waited for its own write-back (`vk_upload_flush(1)`), possibly on the replaying thread | More than 16,384 released entries with GPU-drawn mirrored ones in the batch | suspected (code read) | Write the batch's GPU-drawn picks back in one upload batch with one wait before the drops; the same entries go in the same order | None to play | stability pass C-7; with L-2 | Fixed: `28a69d23`. Not reproduced before; `tex_release_check`, `tex_compact_check` pass |
| F-4-22 | S3 | Guest memory (4), GPU memory (12) | `memory.c` `lazy_fault`, `lazy_fly_run`, `mem_thaw`, `mem_lazy_touches`; `vk_memory.c` `lazy_fetch`, `mirror_evict`, `hot_check` | A lazily written range was fetched (a GPU copy and a wait of up to 10 s) inside the fault handler under `frz_lock`, which stalled every other fault, thaw, freeze, arm and the replay's copies; `mirror_evict` and `hot_check` fetched under the exclusive `pool_lock` (F-4-05) | A CPU touch of an armed range (lazy write-back is on by default on Windows outside AMD's driver), a file read near GPU pools, an eviction | confirmed for the work under the lock: 848 fetches, 415.8 ms in the 0.10.0 log; the design's probe had unrelated copies, arms and freezes wait 299 ms behind a 300 ms fetch | Ranges go into a table of flights and are fetched without `frz_lock`, so a touch waits for its own range only; thaws release before the lock; evictions fetch before taking `pool_lock`; four fetch slots; `wb_lazy_unlocked = 0` goes back | The piece protocol and the eviction's order against `vk_pool_fini` (approved). The fetch slots and the eviction order have compile checks only | stability pass C-12; re-check of F-4-05 (L-04) | Fixed with approval: `2e077edc`, the diagnostic `f586c291`; corrections `92121a11`, `0eee8499`, `da60e0d3`, `a8d18ef4`, `b5e4243c`, `47078fa0`, `61b8a6d6`. Not reproduced in the game; `lazy_fetch_check` (25 scenarios, each checked against a mutant) passes |
| F-4-23 | S3 | Pipelines (15) | `vk_pipeline.c` `pressure_release` | The pipeline release under VRAM pressure measured with the driver's figure, which counts free space inside VMA's blocks, so it could give back every prebuilt pipeline | Device-local use over `tex_vram_pressure` while VMA's blocks hold free space | suspected (code read) | Measure with `vk_mem_vram_live`, as the textures and mirrors do | Low | stability pass D-5 | Fixed: `2f93d274`. Not reproduced before; compile check |
| F-4-24 | S3 | Pipelines (15) | `vk_pipeline.c` `releasable`, `pressure_release` | Without the graphics pipeline library (AMD's driver for RDNA 2 and older) a released pipeline costs a whole build at its next draw | VRAM pressure on such a card (often 8 GB) | suspected (code read) | Decide by measurement: release only where a fast link exists, or accept the build | Keeping pipelines keeps VRAM on exactly the cards short of it | stability pass D-6 | Open: measure first (an 8 GB RDNA 2 card) |
| F-4-25 | S3 | Pipelines (15), Replay (11) | `vk_pipeline.c` `pressure_release`, `release_drain`, `autosave_once`; `vk_sync.c` `completion_main` | Released pipelines were destroyed in batches of up to 256 on the completion thread, before the timeline value counted as done | VRAM pressure: a batch every 5 s | suspected (code read) | The autosave thread destroys them, 64 a tick, once their fence passed | Low: destruction up to 500 ms later | stability pass D-7 | Fixed: `48846dc9`. Not reproduced before; compile check |
| F-4-26 | S3 | Input (20), Guest memory (4) | `camera_speed.c` `rewrite`, `search_main` | The camera-speed heap search read lazily armed ranges, so each armed piece faulted and was fetched from the GPU while the game ran (F-4-09) | Changing the stick sensitivity with lazy write-back on | suspected (code read) | Skip armed ranges (`mem_lazy_touches`), logged once; since C-12 ranges in flight too | Low | stability pass E-13; re-check of F-4-09 (L-13) | Fixed: `af061602`. Not reproduced; `camera_search_check` passes |
| F-4-27 | S3 | Diagnostics (22) | `stutter_map.c` `cand_decide`, `episode_close` | Stutter-map episodes never ended while frames stayed over 25 ms: at 40 fps or lower every hitch after the first merged into one seconds long | `stutter_map = 1` at 30 or 40 fps | confirmed by probe against `stutter_map.c` | An episode ends at its last frame that stood out once the frames after it settle | None | stability pass L-3; review of `2f640fb8` | Fixed: `baf83fda`. Two `stutter_hitch_check` cases failed before and pass |
| F-4-28 | S4 | Frame pacing (9) | `rtss_limit.c` `worker`; `vk_lowlat.c` `vk_lowlat_swapchain`; `gfx_present.c` `create_swapchain` | A want flipping between a limit and kept-out (low latency turning on after the first present, each swapchain rebuild) created and removed RTSS's profile at every poll | NVIDIA with low latency on and RTSS running | suspected for the game; the real worker made 6 profiles and removed 5 in 10 flipping polls (probe) | A want reaches the profile only once it stayed two polls, and nothing is written before the first host present; the created and removed lines are numbered | A limit change reaches RTSS up to 1 s later (approved) | stability pass A-10; P-12 | Fixed with approval: `4d4b97b0`; numbering `9c32d3bc`. Not reproduced in a game; `rtss_limit_check` (churn) passes. Play-test: live toggles |
| F-4-29 | S4 | Frame pacing (9) | `rtss_limit.c` `apply` | Without a denominator field, 'display' at 59.94 Hz was written as 60 (143.856 as 144), above the refresh. VRR and `VK_KHR_present_wait` are not handled | `fps_limit = display` on a fractional monitor with an RTSS that has no `FramerateLimitDenominator` | confirmed on the real `apply` (probe) | Round down when the denominator field is missing | 1 fps lower on such monitors (approved) | stability pass A-12; P-10 | Rounding fixed with approval: `60694532`. Not reproduced in a game; `rtss_limit_check`, `rtss_worker_check` pass. VRR and present_wait: open, measure first with PresentMon |
| F-4-30 | S4 | Frame pacing (9) | `vk_backend.c` `present_main`, `present_image`; `gfx_present.c` `flip_locked` | Latency under RTSS is not measured; RTSS's sleep under the queue lock (A-3) also bounds how far the game runs ahead | RTSS holding | suspected (reasoning only) | Measure 'game to shown' with RTSS, `rtss = 0` and `low_latency = 1`; one frame in flight needs approval | — | stability pass A-17; P-8 | Open: measure first |
| F-4-31 | S4 | Frame pacing (9), Platform | `nvn_api_obj.c` `sleep_until`; `vk_backend.c` `wait_until`; `gfx_present.c` `ng_sleep_until` | Three deadline waits with different margins, and the present thread at normal priority without MMCSS | An unfocused window (EcoQoS), `timer_1ms = 0`, `power_full = 0` | suspected (code read) | Measure 'ms past target' focused and unfocused first; a shared helper or a priority change needs approval | — | stability pass A-18; P-15 | Open: measure first |
| F-4-32 | S4 | Frame pacing (9) | `rtss_limit.c` `rtss_limit_want`, `find_rtss` | RTSS's start (registry, file and module-name reads, thread creation) ran on the presenting guest thread | The first non-negative want: the first present, or the first after frame generation or low latency is switched off | suspected (code read) | `rtss_limit_want` only starts the worker, which looks RTSS up itself | None; the thread now starts in kept-out runs too | stability pass Av-2 | Fixed: `6be25cc9`. Not reproduced before; `rtss_worker_check` passes |
| F-4-33 | S4 | Diagnostics (22), GPU memory (12) | `vk_memory.c` `census_main`, `heap_log`; `perf.c` `ft_frame`, `census_check`; `perf_census.h` | The 'replay' hitches of the known issue were the memory census's address-space walk, which the stutter map charged to replay; frame 3600N walked twice | `mem_census = 1` with `trace = perf` (both fresh 0.10.0 sessions) | confirmed: of 66 replay hitches in the 0.10.0 log, 49 sit at frames N×600+1..3 and 8 beside census walks (the lane's script, rerun by the verifier) | Frames a walk overlapped are charged to 'census', the report's own time to 'report'; one walk at frame 3600N; a walk seen over 2 s counts as a torn read | None: diagnostics, `mem_census` is off by default | stability pass B-3; 'Replay hitches' (brief section 5) | Fixed: `af8e2a32`, `b7c67854`, `28f420bf`, `ec967d4d`, `a473e763`. `census_charge_check`, `stutter_hitch_check`, `stutter_map_check` pass. Play-test: the hitch recount with `mem_census = 0` |
| F-4-34 | S4 | Device and present (16), Diagnostics (22) | `gfx_present.c` `vk_queue_lock`, `vk_queue_lock_raw`, `create_swapchain`; `perf.c` `perf_qlock_hold`, `lt_report` | Nothing named who held the queue lock when the present waited 48 ms for it, and a swapchain rebuild's hold was untimed | Any long `g_ql` hold | confirmed (the `lock_time` table lists wait sites only) | With `lock_time = 1` the queue lock records its holder and logs holds of 5 ms or more; each rebuild's hold is timed | None: behind an off switch | stability pass B-7; with F-4-06 | Fixed: `0ffac802`. `sync_hw_check`, `stutter_hitch_check`, `pgo_build_test` pass |
| F-4-35 | S4 | Diagnostics (22) | `vk_sync.c` `ts_read_cb`, `vk_gtl_note_ns`, `vk_gpu_parts_log` | The GPU-parts line for long fence waits (`7b3edac1`) never saw upload batches or presents, though the frames it was made for follow area loads | `trace = perf` and a frame whose render thread waited 20 ms or more for fences | suspected (code: one writer of the ring) | The ring takes every timed part where the timeline notes it | None: trace only | stability pass B-8 | Fixed: `f26d0910`. `sync_hw_check`, `stutter_hitch_check` pass |
| F-4-36 | S4 | Diagnostics (22), Replay (11) | `vk_backend.c` `synced_held_count`, `vk_be_cpu_synced`; `nvn_api_core.c` `nvnSyncWait` | The F-2-05 counter (`0f1885f9`) counted waits for value 0, the acquire's sync, which no held submission signals: half of its logged lines | Any wait on the window acquire's sync while work is held | confirmed: 6 of the 12 lines of the two fresh sessions are 'wait for 0' | Leave waits for 0 out. Counting only waits at or after the oldest held submission needs `vk_exec.c` (not done) | None: a counter | stability pass B-9; F-2-05 | Fixed in part: `f687ede7`. The refinement is open |
| F-4-37 | S4 | Guest memory (4) | `memory.c` `lazy_fault`, `lazy_piece_locked` | With `wb_lazy_chunk = 1` the piece path held `lz_lock` across the GPU fetch and the page protection, against `memory.c`'s own rule | `wb_lazy_chunk = 1` (off by default) and a CPU touch of an armed range | suspected (code read) | Take the piece out under `lz_lock` and fetch without it; since C-12 a piece goes through the flight table without `frz_lock` as well | Low: chunk mode only | stability pass C-8; with F-1-11 | Fixed: `7df96ef0`, then `2e077edc`. Not reproduced before; `wb_lazy_check`, `heap_lazy_copy_check`, `lazy_fetch_check` (chunk scenario) pass |
| F-4-38 | S4 | Diagnostics (22), GPU memory (12) | `perf.c` `mark_text`; `vk_headroom.c` `on_allocate`; `vk_device.c` `timed_alloc`, `create_device` | About 180 ms of the first visit's 421 ms frame were attributed to nothing: its 78 device memory allocations (4.4 GB) carried no time in any mark | An area's first load | suspected (log lines checked; their share not measured) | VMA's `vkAllocateMemory` goes through `timed_alloc`, so the work mark carries the time (failed attempts included) | None | stability pass C-10; 'First visit to an area' (brief section 5) | Fixed: `74b9687a`, comment `93be3e06`. `mem_growth_check`, `stutter_hitch_check` pass. The first visit's cost itself is open |
| F-4-39 | S4 | Pipelines (15) | `vk_pipeline.c` `ready_note`, `u_run`, `job_build` | A pipeline whose optimised build failed but whose fast link succeeded was never written to `pipeline-keys.bin` | A failed optimised build with a queued fast link | suspected (code read; rare) | `ready_note` takes the pipeline state from every caller | None | stability pass D-14; L-24 | Fixed: `6af16f7c`. Not reproduced; compile check |
| F-4-40 | S4 | Pipelines (15) | `vk_pipeline.c` `pressure_release`; `nx2navi.ini.example` | `tex_vram_pressure = 0` meant 80 % to the pipeline release, while the textures read 0 as off | A player setting `tex_vram_pressure = 0` | suspected (code read) | 0 keeps every pipeline | None at the default | stability pass Dv-1 | Fixed: `ae90aff5`. Not reproduced; compile check |
| F-4-41 | S4 | Textures (13) | `vk_texture.c` `sweep_hard`; `vk_tex_release.h` `trl_hard_pick` | Past the hard count of released textures one call can walk 16k entries and drop up to 1,024 textures on a drawing thread | More than 16,384 released entries (the 0.10.0 session reached 16,328) | suspected (code read) | Time the batches first; a cap on the GPU-drawn picks per batch needs the numbers and approval (C-7 removed the per-entry waits) | — | stability pass L-2; review of `2969005e`; with C-7 | The diagnostic landed: `8fc22289`. The behaviour is open: measure first |
| F-4-42 | S4 | Textures (13), Diagnostics (22) | `vk_texture.c` `sweep_pressure`, `rel_census` | The census of released textures over their bound walked every entry once a minute for every player, and said 'kept while the card has room' without room | Released textures between their bound and 1.25 times what they may keep | confirmed (code) | Write it only while they may keep more than their bound | None | stability pass L-4; review of `00081be5` | Fixed: `a8cc1e69`. `tex_release_check` passes |
| F-4-43 | S4 | Diagnostics (22) | `stutter_map.c` `stutter_report` | The report held `sm_lock` through a sort and some 85 log lines on the presenting thread, now every 5 minutes and at focus loss, while the replay and pipeline workers take that lock | `stutter_map = 1` | suspected (code read) | Copy under the lock, sort and log the copy without it (the same text) | None | stability pass L-6; review of `7b0dbeb1` | Fixed: `110c1b24`. `stutter_hitch_check`, `stutter_map_check` pass; the report is byte-identical in a scratch harness |
| F-4-44 | S4 | Diagnostics (22) | `nx2navi.ini.example` (stutter_map); `docs/building.md`; `vk_texture.c` `released_cap_bytes` | Text drift: the ini still said every 10 minutes and one line per hitch; `freeze_hold_check` had no paragraph; the released bound's line named installed memory when its size is unknown | Reading the docs | confirmed | Texts updated | None | stability pass L-7; review of `7b0dbeb1`, `2f640fb8`, `0e0cfe08` | Fixed: `48ea6243`, `1f547853` |
| F-5-01 | S3 | Textures (13) | `vk_texture.c` `alias_compatible` (~2567), `start_at` → `init_new` (~2755) | A new depth image made over a live depth target's memory starts cleared unless `depth_alias` is on and both are depth-only with one format; a shadow map that starts cleared shadows everything for one frame | The known one-frame dark shadow at the Castle Town gate | suspected cause of a known problem; `NX2NAVI_DEPTH_ALIAS_LOG=1` would line the dark frame up with an alias logged as cleared. Not run (the gate is not on the route) | Carry depth content across the alias for depth-stencil pairs and partial covers too | Rendering behaviour: needs the user's eye on the result | — | **Stability pass:** changed by `8bc23ea0` (a new depth image cleared at the replay's position; `depth_alias` is gone); unconfirmed here (F-5-03: the play-test at the gate). |
| F-5-02 | S2 | GPU memory (12) | `vk_memory.c` shadow storage (`shadow_sync_pool` ~1248, default on for the Linux build ~1676) | On the Linux build CPU-written pools reach the GPU through a per-submission snapshot; a write made after the snapshot goes with the next submission, so per-frame instance data can be one frame behind the buffers around it | The known foliage flicker on Linux with an AMD card | suspected cause of a known problem; cannot be run here (no Linux machine, no AMD card). `NX2NAVI_SHADOW=0` on that machine would tell this from the shader-side suspects in `docs/maxwell.md` | Depends on that test | — | — | **Stability pass:** changed: on Linux guest memory is imported through udmabuf (`eaf10677`), with shadow storage as the fallback. Untested on AMD Linux. |
| F-5-03 | S3 | Textures (13) | `vk_texture.c` `alias_compatible`, `init_new`, `depth_init_inline`, `init_image` | The gate's case changed since F-5-01: `8bc23ea0` clears a new depth image at the replay's position and `depth_alias` is gone; a new depth image over a live one still starts at depth 0 with no content | The Castle Town gate (a new depth descriptor over a live one about 40 times a second) | suspected (code read; a contributor counted 0 dark frames in 5,423 with the change against 12 in 2,728 without; not confirmed here) | Measure at the gate with `NX2NAVI_DEPTH_ALIAS_LOG=1`, `oq_hold` 0 and 1; carrying content or a 1.0 clear changes rendering | Rendering: needs the user's eye | stability pass C-11; re-check of F-5-01 | Open: play-test at the gate |
| F-5-04 | S3 | Shaders (14) | `gfx_shader.c` `carry_load`, `carry_find`, `carry_usable`, `carry_last`, `get_one`; `tools/make_game.py` `step_shaders` | After an update, translations carried from the 0.9.2 translator (identity 4d051299…) still clamp texture pool handles to slot 32767 for the session: the see-through ground with the fixed translator | An update whose shader step carries the old translations | suspected (the clamp shown by a probe through the old translator) | A carried translation whose pool array has the earlier length (32768) is not served: it is translated again, and served only where the current translator fails the shader | The first session after such an update translates those shaders once; a refused compute shader is translated while its dispatch waits | stability pass D-3; issues #569, #824 (brief section 5) | Fixed in the first wave under the user's request to fix issues #569 and #824 in 0.10.2 (the verifier had marked it for approval; it stays inside `gfx_shader.c`): `46c764a4`, `6c6e7b39`, `a47c6d69`, `d2e23ce9`. Not reproduced before; `carry_pool_check` passes. Play-test: the update from 0.10.1 |
| F-5-05 | S3 | Upscaling and frame generation (17) | `vk_dlaa.c` `pb_images`, `pb_destroy`, `idle_release` | A picture size change cleared `PB.used` with the old prebloom images, so `idle_release` freed the new ones at the next present without a wait, while the frame that drew into them could still be in flight | `dlss_prebloom = 1` (off by default), each picture size change after the first 600 presents | suspected (code read); 'prebloom: the picture-size images went unused for 600 presents: freed' right after a resize would show it | Set `PB.used` again once the new images are made | None | stability pass, second wave, upscaler side finding 1 | Fixed: `c1498815`, `bede842d`. Not reproduced; compile check |
| F-5-06 | S3 | Upscaling and frame generation (17) | `vk_dlaa.c` `vk_ngxfg_evaluate` | Linux NGX frame generation releases `FG.feat` on the present thread at a backbuffer size or format change, with no regard to the previous present's command buffer, which may still be evaluating it | Linux with NGX frame generation and a resize or format change | suspected (code read) | A fence check before the release | Linux only | stability pass, second wave, upscaler side finding 2 | Open: outside the approved item |
| F-5-07 | S4 | Shaders (14), Textures (13) | `gfx_shader.c` `pool_short_note`, `mem_insert`; `vk_texture.c` `vk_texture_register` | No log line said a pool shader of the earlier translator's length was in use, or that registered slots 32768 and up are out of its reach | Carried old translations with the game's pool of 33,068 descriptors | suspected (code read) | Count such shaders and log once; log a registered slot of 32768-40959 while one is in use | None | stability pass D-9 | Fixed: `b04e7c85`. `carry_pool_check` passes |
| F-6-01 | S3 | In-game menu (21) | `menu_assets.c` `dicts_load` (~56-73, callers ~92, ~125) | The dictionaries are loaded lazily with no lock, and `ndicts` is set to 0 before the load: a second caller meanwhile sees "loaded, none" and gets nothing. The menu's fonts then fall back for the whole run, as the font read is tried once | The "menu-art" thread and the present path both asking during the first frames | suspected (code read). Not reproduced: 54 of 54 logs here show `menu: the game's interface fonts: text ClarimoUDPE, titles CetraDisplayPro` | Load once behind a `plat_once` | None | L-14 | `b4bfb7b6`. Not reproduced before (54 of 54 logs had the game's fonts). The fixed exe's runs show the game's fonts too. Not reproduced before, so not shown fixed. Regression: the route with the fixed exe: `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6) |
| F-6-02 | S3 | Launcher | `launcher/ui/app.py` `play_only` (~2175-2196) against `watch_game` (~1387-1399) | Started from the launcher, a fatal exit (codes 70, 71) shows the game's FATAL line; started from the Steam shortcut the launcher only logs the exit code, and the runtime raises no message itself: the window disappears | Any fatal exit of a game started from the shortcut, F-1-01 included | suspected (code re-read by the auditor) | Show the last FATAL line on codes 70 and 71 in `play_only` as well | Launcher only | — | **Stability pass:** fixed by `8f42883a` (F-6-08). |
| F-6-03 | S4 | Settings (8) | `nvn_api_obj.c` `nvnWindowAcquireTexture` (~833), `perf.c` `rt_knob` | `NX2NAVI_ACQUIRE_WAIT` is read with `rt_knob`, which maps every value but "0" and "ab" to 1 and "ab" to 2: the documented mode 2 cannot be chosen, and "ab" selects it instead of alternating | Setting the key to 2 or ab | suspected (code read); a diagnostics switch | Read it as a number | None | — | `4b47a10a`. The value 2 was not run. Not reproduced before, so not shown fixed. Regression: the route with the fixed exe: `p4_route` went through every segment with no FATAL and committed the save five times. Its frame rates cannot be compared with the baseline: the exe name is new and it has a pipeline cache folder of its own, so pipelines were still compiling on other threads (Kakariko 118.1 fps against 140.7; 17 frames over 25 ms in the 12 s after the reload, against 0 and 6). **Stability pass:** `7684734b` logs values other than 0, 1 and ab (F-6-15). |
| F-6-04 | S3 | Build kit and launcher, Shaders (14) | `tools/build_xlate.py` `main`, `check_built`, `source_identity`; `tools/make_launcher.py` `check_xlate`; `tools/make_release.py` `check_translator`; `tools/make_appimage.py`; `tools/make_kit.py` | The packaging guard against the 0.9.2 translator (the see-through ground) could be passed: `--id-only` wrote a matching id beside any exe, `make_release` hashed whatever prepared tree was in `build/xlate`, `make_appimage` and `make_kit` checked nothing, and `make_launcher` checked after the downloads and the deletion of `--out` | A release from a folder holding an old translator, or after a patch change without a rebuild | confirmed by probe: a 20-byte stand-in passed `check_xlate` after `--id-only` | `--id-only` removed; every packaging script calls `build_xlate.check_built` before any download or delete; `make_launcher` also asks the shipped translator for its texture pool length | Release tooling only. The exe build record (a sha256 beside the id) waits until after 0.10.2 | stability pass D-1, D-4, G-11, G-12; issues #569, #824 | Fixed in part (the exe build record of D-1 and G-12 waits until after 0.10.2): `f3d8f086`, `78bfb83b`, `ad0e934f`. `xlate_package_check` passes: it refuses the 0.9.2 translator beside a current id and passes `work\xlate_6866a13e` |
| F-6-05 | S3 | Settings and logging (8) | `cfg.c` `cfg_load`, `add`, `cfg_bool`, `cfg_int` | `nx2navi.ini` values were taken unchecked: a trailing comment stayed in the value, `cfg_bool` read anything but 0 as on, out-of-range values passed, and nothing was logged | A hand-edited file: 'pace_smooth = 0 ; off', 'fullscreen = false', 'sharpness = 250' | confirmed by probe (`cfg.c` with stand-ins) | Log values outside their settings' sets, with the line they came from | None: log lines | stability pass F-7 | Fixed: `8c1ec424`. `cfg_parse_check` passes |
| F-6-06 | S3 | Settings and logging (8), Build kit and launcher | `cfg.c` `cut_comment`, `switch_of`, `cfg_bool`, `lowend_of`; `perf.c` `rt_knob`; `tools/make_game.py` `ini_choice`; `launcher/gameini.py` | Players' trailing comments and the words false, off and no turned settings on or broke them (as F-7) | As F-7 | confirmed (probe) | Cut a comment after a value that has a list of choices or a range; settings whose values are 0 and 1 take off, false, no and on, true, yes; `make_game.py`'s readers agree | Changes what existing files do, pacing and presentation keys included (approved) | stability pass F-8 | Fixed with approval: `b6709da8`, `8c9011d5`, `4fe13391`. `cfg_parse_check`, `rt_knob_check`, `launcher_test` pass. Play-test: an ini with comments |
| F-6-07 | S3 | Build kit and launcher | `launcher/ui/app.py` `EXIT_CODES`, `game_end`, `watch_game` | The launcher explained exit code 72 (a lost device) with neither its meaning nor the game's FATAL line | Any device-lost end (the RX 6600 logs) | confirmed (the field logs end with the bare code 72) | Codes 70, 71 and 72 show the FATAL line; 72 has a text that fits without one too | Launcher text only | stability pass G-1 | Fixed: `8de99df6`. Not reproduced in the launcher; `launcher_test` passes |
| F-6-08 | S3 | Build kit and launcher | `launcher/ui/app.py` `play_only`, `game_end` | A game started from the Steam shortcut that ended fatally showed nothing: no FATAL line, no message, no note in the game's log (F-6-02, wider since `7b0dbeb1`) | Any nonzero end of a game started with `--play` | confirmed (probe of `play_only`) | `play_only` shares `watch_game`'s end handling, with a message box for a fatal end | Steam shows the game running until the box is closed | stability pass G-2; re-check of F-6-02 | Fixed: `8f42883a`. `launcher_test` passes |
| F-6-09 | S3 | Build kit and launcher | `launcher/ui/app.py` `watch_game`, `GameRun`, `log_has_own_exit`; `launcher/winsys.py` `game_copy` | After the game's own Restart the launcher reported the first process's code 0 for the restarted copy and never showed its FATAL line; a log cut at 64 MiB got a false 'no line of its own end' note | A setting that restarts the game, then a fatal end or a kill of the copy | confirmed (probe of `watch_game` on a scratch copy) | Follow the Restart's copy by its image path and read its own exit code; an unknown code is said so; `log_has_own_exit` answers 'unknown' at the cut | Launcher only | stability pass G-3, F-14, L-5 | Fixed: `8de99df6`. `launcher_test` passes |
| F-6-10 | S4 | Pipelines (15) | `vk_pipeline.c` `seed_pipelines`, `prebuild_report` | `--prepare-pipelines` logged a nonsense 'prebuild done … ms after the device was ready': the start time was never set there | The preparation modes | suspected (code read) | Set the start time and say it counts from the preparation's start | None | stability pass D-10 | Fixed: `339f391c`, `1e1e3b75`. Not reproduced (`prepared_request_reuse_check` needs approval) |
| F-6-11 | S4 | Pipelines (15) | `tests/cache_exit_snapshot_check.py`, `tests/compute_prebuild_queue_check.py` (their stand-ins) | Two pipeline checks did not compile (test rot): their stand-ins lacked `P.held` and `LB.bg_threads`/`lb_idle`, so the exit save's snapshot rules and the prebuild queue went unchecked | Every run since `d9429b0c` and `cd0b1a38` | confirmed: both failed at `f1a2a64e` | Bring the stand-ins up to the code | None: tests | stability pass D-11, G-6, G-7, F-23 | Fixed: `077c363f`, `fd91f46b`. Both pass |
| F-6-12 | S4 | Settings and logging (8) | `cfg.c` `add`, `cfg_set`, `cfg_reset`, `migrate` | `CFG_MAX_KEYS` (512) dropped settings silently at load, and a menu change silently | More than 512 keys (each cheat ever toggled keeps a line) | confirmed by probe; practically out of reach | Log it; a larger table needs approval | None for the logging | stability pass F-9 | Fixed (the logging): `5b9ed9ed`. `cfg_parse_check` passes. A larger table waits for the user's word |
| F-6-13 | S4 | Settings and logging (8) | `cfg.c` `known[]`; `tests/cfg_rows_check.py`; the content guard | 15 names the runtime reads were missing from `known[]`, so setting them logged 'unknown key', and no static check guarded the tables (or a `//` hiding rows) | Setting any of them | confirmed (the prototype check) | Add the names; `tests/cfg_rows_check.py` checks `cfg.c`'s tables and runs with the content guard | None | stability pass F-10 | Fixed: `0fbd2b8f`. The check reported the 15 names before and 0 now; its self-test catches 15 injected faults |
| F-6-14 | S4 | Settings and logging (8), Device and present (16) | `cfg.c` `default_number`; `gfx_present.c` `gfx_cfg_window_size` | Both read `cfg_meta()` without a NULL check; the window-size fallback ignores the device profile | Unreachable today for `default_number`; an unparsable window size on a Steam Deck | confirmed by test (`cfg_int` of an unset row's key crashed the check program) | NULL checks; the profile's fallback needs approval | None for the checks | stability pass F-20 | Fixed (the NULL checks): `153295b6`. `cfg_parse_check` failed before and passes. The Deck fallback waits for the user's word |
| F-6-15 | S4 | Settings and logging (8) | `perf.c` `rt_knob` | `rt_knob` read every value but 0 and ab as 1, silently | 'cache_die = off' | confirmed (code) | Log once per switch; F-8 reads the words | None | stability pass F-21; F-6-03 | Fixed: `7684734b` (the words: `b6709da8`). `rt_knob_check` passes |
| F-6-16 | S4 | Build kit and launcher | `launcher/build.py` `game_running` | `game_running` took an exe the user cannot write for a running game, so the launcher refused Play, Steam play and rebuilds for good | An ACL or Controlled Folder Access denying writes to `ocarina.exe` | confirmed (probe with a scratch exe) | Only a sharing violation means running; access denied is logged | Launcher only | stability pass G-4 | Fixed: `01057202`. `launcher_test` passes |
| F-6-17 | S4 | Build kit and launcher | `rel_next.sh` (the coordinator's scratch, outside the repository) | `rel_next.sh` would package `f1a2a64e` from `E:\nx2pl5` as 0.10.1 with 0.10.1's notes, not 0.10.2 | Running it to make 0.10.2 | confirmed (script read) | A corrected script with a preflight; it must also pass the rebuilt `nx2navi_dlssg.dll` (A-16) | Packaging only; publishing needs the user's word | stability pass G-5 | Not in the repository: waits for the user's word on the release |
| F-6-18 | S4 | Recompiler (1) (tests) | `tests/movie_timeline_check.py` `main` | The check asserted that the movie timestamp sites belong to set `ostep`; they moved to `omovie` (`70c99357`) | Every run | confirmed: it failed at `f1a2a64e` | Point it at `omovie` | None: a test | stability pass G-8, E-16 | Fixed: `e5632aea`. It passes |
| F-6-19 | S4 | Build kit and launcher | `tools/check_platform.py` `scan` | `check_platform.py` failed on 12 runtime files whose Win32 use sits in Windows-only branches, so it guarded nothing | Every run | confirmed (it failed at `f1a2a64e`) | Skip the Windows-only branches | None | stability pass G-13 | Fixed: `56db0d48`. `check_platform_test` passes |
| F-6-20 | S4 | Build kit and launcher | `tools/check_isa.py` `DISPATCHED` | `kern_ap_stub_ymm` was not listed as dispatched, so the `--cpu v2` check failed on every build | `tools/build_game.py --cpu v2` | confirmed (objdump of 0.10.0's runtime) | List it | None | stability pass G-14 | Fixed: `6b051eea`. `check_isa_test` passes |
| F-6-21 | S4 | Build kit and launcher, Recompiler (1) | `tools/make_game.py` `modules_c`; `recomp/layout.py` `write` | The generated `modules.c` and `layout.h` carried no `GENERATED_MARKER`, so `check_content` would not refuse them in a commit | Copying a gen folder into the repository | confirmed (code) | Mark `modules.c`; `layout.h` waits for a release that changes `recomp/` anyway (a full re-lift for every player) | Every install recompiles `modules.c` once | stability pass G-15 | Fixed in part: `5f90486c`. The `layout.h` half is open |
| F-6-22 | S4 | Build kit and launcher | `launcher/cli.py` `run` (update) | `launcher.cli update` switched to the new version without checking whether the game runs | The command-line update while the game runs | suspected (code read) | Refuse it while the game runs, as the window does | None | stability pass G-16 | Fixed: `73d6c8ff`. `launcher_test` passes |
| F-6-23 | S4 | Build kit and launcher | `launcher/native/nx2navi.c` `swap_python`; `launcher/jobs.py` `BackgroundUpdate.install` | The stub's Python swap can fail while a Steam `--play` process still runs from `python/`, so a release starts on the wrong Python | A release that brings a new Python, installed right after a Steam-started game closes | suspected; 0.10.2 keeps the same Python | None for 0.10.2; retry the swap when a release next changes the Python | — | stability pass G-17 | Not fixed, by decision |
| F-6-24 | S4 | Build kit and launcher | `tools/cheats.py` `BASE` | `cheats.py` hard-coded main's base instead of taking it from `recomp/layout.py` | A change of the layout | suspected (code read) | Take `FIRST_BASE` | Installs with code cheats lift once more | stability pass G-18 | Fixed: `a0fd6edc`. `launcher_test` passes |
| F-6-25 | S4 | Build kit and launcher | `launcher/ui/app.py` `play_only`; `launcher/jobs.py` `running_job` | Play from the Steam shortcut started the game while the launcher window ran a job in the game folder (a rebuild, an update, a mods build) | The Steam shortcut during such a job | suspected (code read) | Try the launcher folder's lock; a running job gets a message box | Launcher only | stability pass Gv-1 | Fixed: `814c10b3`. `launcher_test` passes |
| F-7-01 | S3 | NVN front end (10), Replay (11) | `nvn_api_core.c` `nvnQueueSubmitCommands` (~670-677, `NX2NAVI_SUBMIT_FINISH`, default mask 1), `nvn_api_obj.c` `nvnWindowAcquireTexture` (~832-843, `NX2NAVI_ACQUIRE_WAIT`) | Every frame the game's thread waits for the GPU to run the frame's first submission, and again at the acquire for the image's last present. While the GPU is ahead the waits are short; when the GPU is the slower side they make the CPU frame wait for it | Every frame; the cost should grow at high frame rates or with a loaded GPU | suspected; not determined. The A/B was started and is not valid: the run folder's pipeline state changed under it (F-4-11) and three of its runs lost the device (F-1-11). The one finished pair reads 99.9 fps with the waits and 121.1 without the submit wait in the day field, but the first run also carried more background pipeline builds. The comments say the submit wait prevents white frames in dialogue (3 % of frames without it) and the acquire wait is mostly covered by it | Depends on a valid A/B: drop the acquire wait when the submit wait is on, or wait on the passes' timeline | Changes when frames are drawn against the game's data rewrites (the white-frame case): ask the user first | L-31 | **Stability pass:** still open and not measured; `7b3edac1` times the GPU's parts of frames that waited 20 ms or more. |
| F-7-02 | S3 | Replay (11) | `vk_rec.c` `rec_begin` (~175), `rec_main` (~63); `vk_exec.c` `rec_claim` (~1650) | The recorder thread takes the priority of the thread that made the first submission and keeps it; later only the recording thread is raised and given the reserved core. The recorder then competes with some 85 guest threads, and the recording thread waits for it at the end of every part | Any load on the other cores | suspected (code read). The drains are measured: `recorder ring full 0 ms, drains 443 ms` over 49 s in `p3_market_t1`, about 0.07 ms a frame, so the cost was small on this PC | Raise the recorder's priority in `rec_claim` | Low | L-39 | **Stability pass:** still open; the replay workers take the highest priority, the recorder does not. |
| F-7-03 | S3 | Textures (13) | `vk_texture.c` `sweep_pressure` (~2021), `evict_idle` (~1716), `evict_disused` (~1871) | Under VRAM or commit pressure, every 8th submission the eviction scans walk the whole texture table under the texture lock; a scan that finds nothing does not back off, so it repeats for as long as the pressure lasts | VRAM over 80 % of the budget, or the process's commit over its bound (installed RAM, at least 24 GiB; the baseline's private bytes reach 25.8 GiB) | suspected for the cost; the capped run planned for it was not done | Back off after an empty scan, or keep an order of use | Low | L-35 | **Stability pass:** fixed by `c0f8a322` (F-7-05). |
| F-7-04 | S4 | Diagnostics (22) | `nvn_api_obj.c` `nvnWindowAcquireTexture` (~839-841), `vk_sync.c` `vk_sync_fence_wait` | The acquire's GPU wait is counted as a fence wait and again as pacing, so the frame-time summary counts it twice and a frame bound by the GPU at the acquire reads as "pace" | Any frame that waits at the acquire | suspected (code read); diagnostics only | Count it once | None | — | **Stability pass:** fixed by `04727340` (the acquire's GPU wait is counted once) and `52d996d4` (F-7-06). |
| F-7-05 | S3 | Textures (13) | `vk_texture.c` `sweep_pressure`, `evict_idle`, `evict_disused` | Under VRAM pressure `evict_idle` and `evict_disused` walked the whole texture table every 8th submission, and the pressure loop the released list; a walk that found nothing set no backoff (F-7-03) | VRAM over `tex_vram_pressure` or a standing mirror room request with nothing evictable | suspected (code read) | An empty walk waits 16 submissions (`idle_empty_wait`), and the released-list walk resumes where it stopped | Low: a room request can wait up to 16 more submissions | stability pass C-9; re-check of F-7-03 (L-35) | Fixed: `c0f8a322`. Not reproduced before; `tex_release_check`, `tex_compact_check` pass |
| F-7-06 | S4 | Diagnostics (22), Frame pacing (9) | `vk_backend.c` `present_image` (the hand-off), `present_main`; `nvn_api_obj.c` `nvnWindowAcquireTexture`; `perf.c` `perf_end`, `perf_end_any`; `vk_lowlat.c` `frame_begin` | The perf lines could not see pacing: the hand-off to the present thread was untimed, the acquire's GPU wait counted as both pace and fence (F-7-04), and waits on an acquiring thread other than the presenting one were dropped; all 666 windows of the 0.10.0 session read 'bound: cpu' | Every session with `trace = perf` | confirmed: 666 of 666 windows read pacing 0 % and 'bound: cpu', and 70 of 70 slow frames 'pacing 0.0 drain 0.0' (0.10.0 log) | Time the hand-off (`PERF_HANDOFF`) and the present call; count the acquire's GPU wait once; count other threads' acquire waits as pacing, the low-latency sleep too; the present thread's own line; `handoff` in the perf, slow-frame, flight and stutter-map lines | None: diagnostics. The pacing share now includes the acquiring thread's wait, so shares can add up past 100 % | stability pass A-9; P-7; F-7-04 | Fixed: `04727340`, `9d8f698b`, `a2255cfc`, `2d02ea66`, `a473e763`, `52d996d4`. Not reproduced before; `perf_summary_check`, `stutter_hitch_check`, `stutter_map_check`, `flight_flash_check`, `frame_step_check` pass |
| F-7-07 | S4 | Frame pacing (9) | `rtss_limit.c` `worker` (the settings reader) | RTSS's own modes (frame time limit, scanline sync, SyncDisplay, Reflex) were never read, so the log could not tell what RTSS holds | A frame time limit or scanline sync in RTSS's Global profile | confirmed (the keys are in `Profiles\Global` here and nothing read them) | Read them read-only from the .cfg on the worker and log them once per change | None | stability pass A-13; P-11 | Fixed: `ecbd4944`. Not reproduced before; `rtss_limit_check`, `rtss_worker_check` pass |
| F-7-08 | S4 | Upscaling and frame generation (17), Frame pacing (9) | `vk_dlssg.c`, `vk_fg.cpp`, `rtss_limit.c` header comments | Stale comments: the limit divided by the multiplier, FRAMEGEN needing a restart, 'both reasons need a restart' | Reading the code | confirmed (each comment against the code) | Comments rewritten | None | stability pass A-15; P-14 | Fixed: `7613b7ad`, `fb648ace` |
| F-7-09 | S4 | Diagnostics (22) | `stutter_map.c` `stutter_report` | The stutter map's exit report dropped a hitch still open, or still a candidate, at the exit | A hitch in the last two frames before the exit or a focus-loss report | confirmed with the real `stutter_map.c` | The exit report counts the open tail without closing the episode from the exit's thread | None | stability pass A-20 | Fixed: `395f57a2`. Six tail cases in `stutter_hitch_check` printed one hitch too few before; they pass |
| F-7-10 | S4 | Frame pacing (9) | `rtss_limit.c` `apply` | The 'limits to' line reported the requested rate, not the limit written into the profile after rounding | 'display' on a fractional monitor with an RTSS that has no denominator field | confirmed by probe (logged 59.940 while the profile held 60/1) | Log what the profile holds, with the asked rate beside it | None | stability pass Av-1 | Fixed: `b2b37a3e`. `rtss_worker_check` round_logged_whole failed before and passes |
| F-7-11 | S4 | Shaders (14), Process entry and boot (2) | `gfx_shader.c` `gfx_shader_prewarm`, `warm_worker`; `boot.c` `boot_wait`; `vk_pipeline.c` `vk_pipeline_prewarm_detach` | After `boot_wait` detaches the prewarm, its raised translator limit (up to 16 children) and normal-priority workers stay during play | A start that translates many shaders and stands still 60 s | suspected; narrower than first read | Measure first; lowering the detached prewarm crosses three files (approval) | — | stability pass D-8; L-37 | Open: measure first |
| F-8-01 | S2 | GPU memory (12), Textures (13) | `vk_memory.c` `gen_now` (~748), handed out in `vk_mem_changed` (~836) and nine other places; compared in `vk_mem_changed` (~848), `vk_mem_changed_snap` (~923); `DIRTY_UNSYNCED` = `UINT32_MAX` (~991) | The page write generation is a 32-bit counter with no handling of its wrap. After it wraps, new writes compare as older than everything, so CPU-written textures and mirrored pools stop being uploaded again (stale or wrong textures), and pages stamped before the wrap compare as always changed. 0 ("never stamped") and `UINT32_MAX` ("not scanned") are also handed out as real values at the wrap | One long session. The rate decides how long: an estimate from the code is 200-1000 generations a frame, which is 5 to 50 hours | suspected (the counter and the sentinel re-read by the auditor). The rate is not measured: `587e8494` adds it to the `mem: sizes` lines and `ocarina_audit_p3b.exe` is built with it, but no run was made before Phase 3 was stopped | Make the generations 64-bit | Crosses subsystems 12 and 13 (the tables and every compare): ask the user first. No gameplay or save change | — | **Stability pass:** fixed with the user's approval by `6b95c867`, `028a6ce1`, `5ee960f8` (F-8-08). The rate in play: 134-336 generations a frame. |
| F-8-02 | S1 | Save data (7), Settings and logging (8) | `main.c` `crash_handler` (~271-320), `dispatch.c` `rt_fatal` (~60-82), `fs_host.c` `save_commit` (~601-621) | The game's Commit returns once the commit is queued; a thread then copies and swaps (95-135 ms). The crash and fatal paths end the process without waiting for it. A crash inside that window leaves an unfinished copy, which the next start rolls back: the player loses a save the game reported as done | A crash or fatal within about a tenth of a second after a save | suspected (code re-read: `fs_save_flush` is called from the three normal exits only). The roll-back itself is confirmed safe (12.3) | Wait a bounded time for the pending commit in both paths, unless the crashing thread is the commit thread | Save path only; the format is unchanged | L-03 | `cd170dad`. Not reproduced before (a crash within a tenth of a second of a save). The new wait did not run in any run: no crash fell into a commit. `--probe-fs` and `tests\save_share_check.py` pass with the change |
| F-8-03 | S3 | Input (20) | `input.c` `pad_read` (~817-832), `sdl_enter` (~481) | While SDL searches for devices (a pad pulled, a wireless pad asleep) every poll returns the last state read before, so buttons and stick held at that moment stay held for the search's length (120-240 ms per device notification, longer with a slow driver) | Unplugging the controller while moving | suspected (code read); cannot be run here (no hand on the controller) | Stop replaying the last state after about 150 ms without a fresh read | Input only | — | **Stability pass:** still open (F-8-14, measure first). |
| F-8-04 | S3 | Input (20), In-game menu (21) | `ov_input.c` `npad_states` (~82-98) | The game is always told one controller is connected. A disconnect becomes "nothing pressed" and play goes on uncontrolled; the console would ask for a controller | A controller disconnect in play | suspected (code read) | Open the in-game menu when the primary pad goes away | Changes what the player sees on a disconnect (behaviour): ask the user first | — | **Stability pass:** still waits for the user. |
| F-8-05 | S3 | Frame pacing (9) | `dynres.c` `dynres_read` (~51-54) | Dynamic resolution's default target is 16.7 ms whatever the frame limit, and a step is held for 45 frames, not for a time: at a 120 limit it reacts only once the rate has fallen to about 55, and at 240 fps the resolution can change five times a second | `NX2NAVI_DYN_RES=1` (off by default, not in the menu) above a 60 limit | suspected (code read) | Derive the default target from the limit; hold steps for a time | None to gameplay | L-42 | **Stability pass:** fixed with the user's approval by `1c26907a`, `1ae00ed8` (F-8-11). |
| F-8-06 | S3 | Input (20) | `input.c` `shake_stick` (~706-722) | The shake key swings the stick on a wall-clock period (two frames at 30 fps), so at 120 or 240 fps each side lasts 8 or 16 game frames; a game check that counts swings in frames could miss them | The shake key at a high frame limit | suspected; not determined (needs a fused item and the key at 30, 120 and 240) | Count game steps instead of time | Input timing: ask the user first | L-42 | **Stability pass:** fixed with the user's approval by `5c9e480f` (F-8-10). |
| F-8-07 | S3 | Frame pacing (9), Movies (19) | `frame_rate.c` `movie_cap_hz` (~194-209), `ov_movie2.c` `set_live` (~590, ~740) | The limit is held at 60 while a movie decoder is live, by design. A movie skipped without its end-of-stream picture or a Finalize would leave the decoder live and the game capped at 60 in play | Skipping a movie at a limit above 60 | suspected; not determined (no movie on the route) | End the cap when the decoder has had no sample for a time | Low | L-42 | **Stability pass:** still open (F-8-09, measure first); `6029321e` (F-2-17) removed one way the cap stayed. |
| F-8-08 | S2 | GPU memory (12), Textures (13) | `vk_memory.c` `gen_now`, `gen_next`, `stamp_page_as`, `vk_mem_changed`, every compare; `vk_texture.c` `make_fresh`, `storage_rewritten` | The 32-bit page write generations wrap after 15 to 66 hours of play; after the wrap each consumer misses one write, and every later texture, mirror and shadow check reads old pages as written (F-8-01) | One session reaching 2^32 generations: 180-336 a frame were measured in play, so about 66 h at 60 fps, 27.6 h at 144, 16.6 h at 240 | confirmed by probe: `vk_memory.c`'s generation code compiled across the wrap answers wrong; not reproduced in play | 64-bit generations, stored atomically for the lock-free readers; the tables' size on the trace line; `NX2NAVI_GEN_SEED` starts them near the wrap for a test | Crosses GPU memory and textures (approved). 8 bytes a page: up to about 13.6 MiB more of chunks, plus 2 MiB. Review nits open: a seeded run's rate includes the seed; the size entry counts `reg_max`; `verify_cb` reads its stamp outside the lock; `gen_width_check` uses `-std=gnu11` | stability pass C-1; re-check of F-8-01 | Fixed with approval: `6b95c867`, `028a6ce1`, `5ee960f8`. `gen_wrap_check` (19 checks fail at 2^32 on the old code) and `gen_width_check` pass. Play-test: a seeded run |
| F-8-09 | S3 | Frame pacing (9), Movies (19) | `frame_rate.c` `movie_cap_hz`; `ov_movie2.c` `set_live`, `ov_acquire`, `ov_finalize`, `ov_dtor` | The 60 Hz movie cap ends only at the end-of-stream picture, Finalize or the destructor (F-8-07) | A movie skipped some other way at a limit above 60 | suspected; every movie in the logs here ended with 'the movie ended: the frame limit again' | If a play-test shows a skip that keeps the cap: end it after 1 s without a picture (pacing: approval) | Pacing | stability pass E-17; re-check of F-8-07 (L-42) | Open: measure first. E-14 (`6029321e`) removed one way the cap stayed |
| F-8-10 | S3 | Input (20) | `input.c` `shake_stick`; `frame_rate.c` `frame_step_presents` | The shake key swung on a wall-clock period of two 30 fps frames, so at 120 or 240 fps each side lasted 8 or 16 game frames (F-8-06) | The shake key at a high frame limit | suspected (code read) | Swing by the game's updates, two a side at any rate; a press still lasts 800 ms | Input timing (approved) | stability pass E-18; re-check of F-8-06 (L-42) | Fixed with approval: `5c9e480f`. `shake_key_check` failed 118 times on the old code and passes |
| F-8-11 | S3 | Frame pacing (9) | `dynres.c` `dynres_read`, `dynres_frame` | Dynamic resolution's default target was 16.7 ms and its step hold 45 frames whatever the limit (F-8-05) | `dyn_res = 1` (off by default) above a 60 limit | suspected (code read) | Aim at the limit's frame time by default and hold each step 0.75 s (`DYN_RES_STEP` in 60ths of a second); log target changes | Presentation for players who set it (approved) | stability pass E-19; re-check of F-8-05 (L-42) | Fixed with approval: `1c26907a`, `1ae00ed8`. `dynres_check` (24 failures on the old code) passes |
| F-8-12 | S3 | Recompiler (1) | `recomp/patches-v111.txt` (no site yet) | The item jingle glitches above 60 fps (known); its site is not located in the repository | Picking up an item above 60 | suspected; nothing in the repository names the site | Locate its counter in the lifted main with game files, then a patch set (approval) | Game logic | stability pass E-20; L-42 | Open: locate first (needs game files) |
| F-8-13 | S3 | Recompiler (1), Frame pacing (9) | `recomp/patches-v111.txt` set `orecord` (four sites); `frame_rate.c` | The ocarina song recorder's clock lost the fraction of a frame at four rounding sites above 60 fps (known), so recorded pauses and note lengths came out wrong | Recording or playing songs above 60 (the scarecrow's song) | suspected (located from the code; the 64 original words match the 1.1.1 main) | A new patch set `orecord` keeps the fraction; `ocarina_timing = 0` turns it off | Game logic (approved). Unchanged at a held 30 or 60, except right after a hitch at 60 | stability pass E-21; L-42 | Fixed with approval: `5f9374e1`. `ocarina_timing_check` (50,640 lifted calls, 10,042 C/Unicorn comparisons) passes. The play-test needs a dev exe built from regenerated game C whose 'patches:' line shows `orecord on` |
| F-8-14 | S3 | Input (20) | `input.c` `pad_read`, `sdl_enter` | A pad that disappears keeps its last state for the length of SDL's device search (F-8-03) | Unplugging the controller while moving | suspected (code read) | Report nothing pressed after 150 ms without a fresh read; changes `--probe-input`'s expectation | Input behaviour | stability pass E-22; re-check of F-8-03 | Open: measure first |
| F-8-15 | S4 | GPU memory (12), Diagnostics (22) | `vk_memory.c` `gen_next`, `gen_mark`, `vk_mem_sizes` | Nothing in a normal log showed the page generations nearing their wrap; only `trace = perf` counted them | Any session of 15 h or more at a high frame rate | confirmed (code) | Log where the session stands near the wrap; after C-1 one line at 2^32 | None | stability pass C-2 | Fixed: `5dea57c5`, reworked by `6b95c867`. `gen_wrap_check` failed before (no lines) and passes |

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
- The ledger (section 8) holds 56 findings: 11 confirmed by a run or by tool output, 45 suspected from reading code. (F-1-12 was added during Phase 4; section 14 counts the stability pass's rows.)
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

The stability pass (section 14) brought this list up to date; the rows it names are in section 8.

**Fixed in the stability pass with the user's approval** (its second wave):

- F-2-01 with F-1-06 and F-2-03: the submitting threads take turns without one front-end mutex (F-2-09, F-1-14).
- F-8-01: 64-bit page generations (F-8-08).
- F-1-10: audio values the renderer cannot use are refused or left out instead of ending the process (F-1-19).
- F-8-06: the shake key steps by the game's updates (F-8-10); F-8-05: dynamic resolution's target and step hold
  (F-8-11).
- F-4-05: the lazy fetch outside the freeze lock (F-4-22).
- RivaTuner Statistics Server: the 240 ceiling and the limit taken back with no host present (F-1-13), movies (F-4-12),
  the state file `rtss-profile.txt` (F-4-14), standing aside only once RTSS is seen holding the game (F-4-15), the
  settle rule (F-4-28), the rounding without a denominator (F-4-29); Reflex's frame limit with DLSS Frame Generation
  (F-4-17).
- The upscalers' device-idle waits (F-4-20, which narrows F-2-12 and F-4-02); the ocarina recorder above 60 fps
  (F-8-13); comments and the switch words in `nx2navi.ini` (F-6-06).

**Waiting for the user:**

- F-2-05: the fence value returned before held work is replayed (changes when the game's waits return).
- F-8-04: opening the menu when the controller disconnects.
- F-7-01: the two GPU waits per frame (not measured yet either).
- F-4-15's 30 s give-up (`1744681f`) goes beyond the approved RTSS rules: keep it, or revert that commit alone.
- F-1-27 (C-4) landed in the first wave on the coordinator's call although its verifier marked it for approval: it
  crosses `dispatch.c`, `vk_device.c` and the platform layer, and the exit codes stay 70 and 72. Keep it, or take it
  back (`8f952d28`; not a plain revert: later exit fixes call its `plat_terminate`). F-5-04 landed under the user's
  request to fix issues #569 and #824 in 0.10.2, which it completes for installs updated from 0.10.0 or 0.10.1.
- F-4-12's option (a): no limit in RTSS's profile during every movie, so that RTSS cannot hold the catch-up frames at
  the default limit of 60 (a one-character change in `rtss_decide.h` `rtss_target`, with two test cases).
- F-6-12: a settings table larger than 512 keys; F-6-14: the Steam Deck's window-size fallback; F-4-41: a cap on the
  GPU-drawn textures per hard-count batch, once its timing is known; F-1-33: making the surface again after it is lost.
- F-6-17: the 0.10.2 release itself, with the corrected release script and an `nx2navi_dlssg.dll` rebuilt from this tree
  (a release now refuses the 0.8.0 one, F-4-17).

**Measure first** (a probe run or the play-test, section 14): F-4-13 (RTSS's sleep under the queue lock), F-4-30
(latency under RTSS), F-4-31 (deadline waits and priorities), F-1-24 (overlays across rebuilds and HDR), F-4-29's VRR
part, F-4-19 with F-4-03 (the frames after a load), F-1-34 and F-2-12 (device-idle waits beside AMD's frame generation),
F-5-03 with F-5-01 (the gate), F-4-24 (released pipelines without the pipeline library), F-7-11 (the detached prewarm),
F-3-14 (retiring recording copies), F-3-15's growth (which handles), F-8-09 with F-8-07 (the movie cap), F-8-14 with
F-8-03 (a pad that disappears), F-8-12 (the item jingle: locate it first), F-4-41 (the hard-count batches).

**Not attempted, or left open:**

- F-1-11, the device loss at a save load: its observed trigger is off by default and its mechanism unknown; an AMD RX
  5000/6000 play-test decides.
- F-1-12's residual: a lost device that answers like a live one still runs the callbacks.
- F-4-02 (the DLSS feature created on the replaying thread), F-4-04 (a GPU wait per finalized pool on the guest thread),
  F-4-06 (the present under `g_ql`; the upload flush's wait under `cmd_lock`), F-4-07, F-4-08, F-4-10's compute poll:
  each needs a design on the device's thread rules.
- F-3-01's WriteTexels decode without memory (still only logged), F-3-02's holds with `tex_disuse_evict = 1`, F-3-07,
  F-3-12 (the shader cache kept for the process), F-5-02 (Linux, AMD), F-7-02.
- The pass's own open rows: F-1-22 (guest addresses the audio mixer reads without a mapping check), F-2-13 (frame
  generation's values against the hook thread), F-5-06 (Linux NGX frame generation's release), F-3-19 (the camera search
  and the lazy heap), F-4-36's refinement, F-6-21's `layout.h` half (for a release that changes `recomp/` anyway),
  F-1-36's second part (the owner of every lock and each thread's wait site in the hang report), and F-6-04's exe
  build record (after 0.10.2).
- Not fixed, by decision: F-2-16 (AMD Anti-Lag's switch-off), F-2-19 (`ov_movie.c` `find`, unused by 1.1.1), F-2-22
  (LL/SC and ABA), F-6-23 (the launcher stub's Python swap).

---

## 14. The stability pass for 0.10.2

Done on 2026-11-13 on branch `stability-pass` (worktree `E:\nx2pass`), cut from `next-0.10.0` at `f1a2a64e`: release
0.10.1, the AMD device-loss fixes and a packaging guard for the translator. 225 commits to `87777fea`; nothing pushed.
Its brief: one debugging, stability and frame-pacing pass over the whole project, RivaTuner Statistics Server first.

**What it covered.**

- A map refresh at `2f640fb8` of every open ledger row and lead (the **Stability pass** notes in sections 7 and 8), and
  of sections 3.2 and 3.4.
- Eight read-only audit lanes: A frame pacing and RTSS (the brief's P-1 to P-17); B the front end, locks, replay
  workers, recorder, completion, present and swapchain; C GPU memory, textures, device loss, out of memory, upscalers
  and frame generation; D pipelines, shaders, caches and the translator; E guest memory, kernel, services, audio,
  movies, input and long sessions; F save data, exits, the log, settings and crash reporting; G the launcher, build and
  translation; L a review of the 13 leak and stutter commits at the branch's start (`f7487cee` to `2f640fb8`, from an
  earlier analysis of a fresh 0.10.0 session). Each lane listed its coverage, its probe runs and its play-test items.

**How it was verified.**

- An adversarial verifier that had not written a finding tried to refute it from the code, and where it could ran a
  probe that builds our real source against stand-ins: a fake RTSS (registry key, hooks library, profiles, shared
  memory), the real Windows fault handler, `kernel.c` with host threads, the exit paths in small exes. "Confirmed" needs
  such output or a log line; otherwise the row says suspected and why. One finding was refuted (a menu screenshot's PNG
  encode on the present thread: no player action reaches it). The verifiers found 9 more (the `v` ids).
- First wave: seven fix branches (RTSS, perf instrumentation, GPU memory, pipelines, guest memory and kernel, exits and
  settings, launcher), one finding per commit, most severe first, each branch reviewed by an agent that had not written
  it. The reviews' corrections landed as commits of their own; nothing was rewritten. The merge added four follow-ups
  (`52d996d4`, `d9940113`, `419e173e`, `1f547853`).
- Lane A's own deliverables are commits too: `8e8d6a2f` moves RTSS's decisions into `rtss_decide.h` and pins them
  with checks that need no RTSS (`130fa905` drops an input the worker never set), `3b250adf` describes the pacer,
  the one-limiter rule and RTSS in `docs/architecture.md`, `c9c61e72` rewraps the lines the review left long, and
  `301445b3` names the four new checks in `docs/building.md`.
- Second wave: the items the user approved (13.3), each designed first against the merged code, then a fix branch, a
  review and corrections: RTSS's fallbacks and state file, Reflex's frame limit, 64-bit page generations, the lazy
  fetch, the upscalers' waits, the front end's turns, audio values, and the ocarina recorder, the shake key, dynamic
  resolution and the settings words. Three merge follow-ups: `3def3687`, `5b5fa6e2`, `87777fea`. The second wave and the
  reviews found 8 findings of their own (rows F-1-21, F-1-22, F-2-08, F-2-13, F-2-23, F-3-19, F-5-05, F-5-06).
- A fix for a finding never reproduced says "not reproduced before" in its commit and names the checks that passed;
  where a new check fails on the old code, the row says so.
- No game exe, probe mode or `--selftest` was started: those runs are listed below for the user's approval, and the rest
  waits for the play-test.

**Counts.**

- Findings: 140 (131 from the lanes, 9 from the verifiers); 1 refuted; 139 kept, 63 confirmed and 76 suspected; 8 more
  from the second wave and the reviews. Section 8 holds them in 130 rows, F-1-13 to F-8-15.
- Landed: of the 139, 109 fixed, 12 fixed in part, 18 open; of the 8, 4 fixed and 4 open. By row: 97 fixed, 11 in part,
  22 open; 16 rows were fixed with the user's approval (F-4-29 in part).
- By severity (rows): S1 13 (12 fixed, 1 open), S2 4 (fixed), S3 49 (34 fixed, 4 in part, 11 open), S4 64 (47 fixed, 7
  in part, 10 open).
- Older rows this pass fixed: F-1-03, F-1-06, F-1-10, F-2-01, F-2-03, F-3-04, F-3-05, F-4-05, F-4-09, F-6-02, F-7-03,
  F-7-04, F-8-01, F-8-05, F-8-06 and F-1-09's file half. Narrowed: F-1-12, F-2-05, F-4-02, F-4-03 (its part named),
  F-4-04, F-4-06, F-8-07.

**Test matrix** (no game exe).

- At `f1a2a64e`: 41 checks of the brief's matrix, 34 pass. Four failed from test rot (`cache_exit_snapshot_check`,
  `compute_prebuild_queue_check`, `movie_timeline_check`, `excl_mt`: F-6-11, F-6-18, F-2-21), three from the
  environment: `installer_test` (no interpreter here has both `compression.zstd` and `cryptography`), `worktree_test`
  (an environment hook's `CLAUDE.local.md` shows in its temporary worktree), `water_projective_check` (no `spirv-as` on
  PATH in that run).
- At `87777fea`: 89 checks (the matrix, 45 new checks and three older ones the matrix lacked), 87 pass; `installer_test`
  and `worktree_test` fail as before, for the environment only. The reviews compiled every runtime file on their
  branches (`pass_cc_check`), and a clean dev build at `87777fea` compiled all 169 runtime files and linked the exe.
  `tools/check_content.py` passes on every tracked
  file. `tests/cfg_rows_check.py` now runs with the content guard.
- The lifter's differential tests did not apply: `recomp/` changed only in `patches-v111.txt` (the set `orecord`), which
  `ocarina_timing_check` compares against Unicorn.

**What only a play-test can show.** Pacing under a real RTSS (the hand-off, the 30 s give-up, the 240 ceiling, movies,
the state file after a kill); RTSS's OSD across rebuilds and in HDR; Reflex's frame limit (RTX 40 or 50); the upscalers'
deferred releases and refused creations; the lazy fetch's slots and eviction order (compile checks only); the front
end's held-present lock under real overlaps; audio values in dungeons and bosses; the ocarina recorder at 144 and 240
(it needs a dev exe built from regenerated game C); the update from 0.10.1 (shaders prepared again, the ground no longer
see-through); the Castle Town gate; long sessions (handles, recording copies, page generations, the log's end); the hang
watch staying quiet; an AMD RX 5000/6000 session (F-1-11, F-1-12). The checklist went to the user with this pass.

**Probe runs waiting for the user's approval** (none was run; each with an exe not named `ocarina*`, one game at a
time):

- RTSS: the hook time and the flip, queue-lock and hand-off times with RTSS running and not (`trace = present,perf`); a
  movie with `TEST_HITCH=120:70` at `rtss` 1 and 0; low-latency toggles counting the profile's created and removed
  lines; the quick menu minimised while RTSS holds; `SPEED_PROBE=1` at 60, 144 and display; ten closes, a kill and a
  normal start with the profile compared each time.
- The brief's exe tests: `device_lost_check`, `exit_fault_check`, `ffx_cache_check`, `prepared_request_reuse_check`
  (optional: `build_gate_check`, `mem_need_check`).
- Probe modes: `--probe-fs` (F-1-28), `--probe-cfg` (the new rows, among them `HANG_WATCH_S`, `HANDLE_CENSUS`,
  `COPY_RETIRE`, `REFLEX_LIMIT`, `GEN_SEED`, `WB_LAZY_UNLOCKED`, `UPSCALER_IDLE_WAIT`, `AUDIO_STRICT`, `PRESENT_ORDER`,
  `OCARINA_TIMING`), `--probe-ipc` (F-2-06, F-3-17, and the audio result 0x5299), `--probe-kernel` (F-2-07, F-1-17),
  `--probe-crash-exit` (F-1-26, F-1-27), `--probe-window` and `--probe-vkdevice` (F-4-34's rebuild timing, the teardown
  order), `--probe-nvn` with a case for F-3-13, the texture probe after F-8-08, the game's `--selftest`, the launcher's
  `--selftest`.
- Measurements: the hitch count with `mem_census = 0` (F-4-33); a timed route with `replay_time = 1` (F-4-19); a valid
  A/B of the two GPU waits (F-7-01); a VRAM-pressure route with `vram_budget_mb = 6144` (F-7-05, F-4-23, F-4-25,
  F-3-02); a first visit with `texup_log_ms = 16` (F-4-38); a carry probe with a 0.10.1 shader cache (F-5-04); a
  detached prewarm (F-7-11); a seeded generation wrap (F-8-08); `stutter_map = 1` at a capped 30 fps (F-4-27);
  `replay_async = 1` across area loads (F-1-20); the 0.9.2 to 0.10.2 update in a test install.
