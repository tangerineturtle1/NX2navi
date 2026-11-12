# Architecture

How the Switch 2 build of *Ocarina of Time* (game version 1.1.1) becomes a native Windows x86-64 program. Code taken
from other projects, with its license, is listed in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

## Pipeline

### 1. Input

- The ExeFS and RomFS of the user's own dump. Everything extracted or generated stays under `build/`, `generated/` or
  the user's own folders, never in the repository (`tools/check_content.py`).

### 2. CPU: lift ARM64 to C

- Modules: `main`, `sdk` and the `subsdk*` modules. `rtld` is dropped; the host links the modules.
- A hand-written decoder (`recomp/lifter.py`) and NEON/FP lowering (`recomp/simd_c.py`, `runtime/simd.h`) emit one C
  function per guest function, direct calls for `BL` and an address-keyed table for indirect branches.
  - The generator runs in parallel processes, with bounded memory per worker.
  - Jump tables are resolved by a symbolic pass of our own (`recomp/jumptables.py`).
  - `LDXP`/`STXP` (128-bit exclusive pairs) are supported.
  - NZCV and the six most used general registers of each function live in C locals instead of the `Cpu`
    (`recomp/flags.py`, `recomp/regs.py`): registers are stored back where they changed, before calls, SVCs and
    exits, and reloaded after them. A call reloads only X0-X18 and X30, as AAPCS64 has the callee preserve X19-X29
    and SP; `gen.py --no-callee-saved-trust` reloads those too.
- Memory: guest address equals host address, all below 2^39 (IPC encodes 39-bit addresses). The heap, stacks and the
  alias region are reserved with `MEM_WRITE_WATCH`, so the graphics back end can ask which pages the CPU wrote since
  the last upload (`runtime/memory.c`).
- Indirect branches go through `rt_dispatch` (`runtime/dispatch.c`), which looks the target up in the address table
  and tail-calls it.
- Testing: a differential harness checks every lifted instruction class against Unicorn (test-only, never linked into
  the game).

### 3. Host

- The SDK boundary is at the `nn::*` function level: threads and synchronisation, IPC, memory, fs/RomFS, time,
  account, applet, hid, audio renderer and effects run as host code. Only `audren:u` and `sm:` are real IPC services.
- Window, keyboard and mouse: Win32 (`gfx_window_win.c`, `input_win.c`). Gamepads and audio output: SDL3 (every
  connected gamepad is open; the game reads the one pressed last, `input.c`). Voice: libopus. Movies: a host VP9
  decoder over libvpx replaces the SDK's `movie::Decoder` (`runtime/ov_movie.c`).
- Settings and menu: every key with its default is in one table (`cfg.c`), read from `nx2navi.ini` and `NX2NAVI_*`
  variables and written back when the in-game menu changes it. The menu and the performance overlay are Dear ImGui,
  drawn into the swapchain image (`runtime/ui/`); their text is in `ui_text.c`.
- Log: in a game run the lines go through a queue to a thread that writes them (`runtime/logq.c`), so a stalled
  write to the log file or the console does not hold the thread that logged. A thread's lines reach the log in the
  order it logged them: a line waits for the queue's lock, and one that finds the queue full is written after
  everything queued before it. A fatal error, the crash report and the end of the process write the queue out first
  (`rt_log_sync`). `NX2NAVI_LOG_ASYNC=0` writes each line where it is logged.
- Co-op (`coop.c`, `coop_proto.c`, [coop.md](coop.md)): a host thread with a UDP socket to a relay (`tools/coop_relay.py`),
  rooms of up to 8 players and a channel for messages between them; it never holds a virtual core.
- OS calls the host makes go through `runtime/platform/` (`platform.h`, `win32.c`).

#### Threads and cores

Every guest thread is a host thread. The game assumes Horizon's scheduler: three application cores, a thread runs
only on a core in its affinity mask, and a higher-priority thread takes a core from a lower-priority one. The game
keeps per-core data without locks and relies on that, so the host models it (`runtime/kernel.c`):

- Three virtual cores. A guest thread runs guest code only while it holds a core; the others wait in a
  priority-ordered list.
- Preemption. When a waiting thread has a better priority than a holder on a core it may use, exactly one holder is
  flagged. The holder checks the flag at its next SVC and at every indirect call (`rt_dispatch`), checks again under
  the kernel lock that a better thread still waits, and only then hands its core over.
- Asynchronous preemption (on unless `NX2NAVI_ASYNC_PREEMPT=0`): a waiter that has spun 250 us suspends the holder
  and, if its host rip is in lifted code at host depth 0, moves it to `kern_ap_stub`, which saves the volatile
  registers and gives the core up there. A holder of a guest mutex counts at the priority of its best waiter, and one
  inside an SDK spin lock is not preempted.
- Wakes. A signalled event or a thread that ends wakes only the `WaitSynchronization` waits that hold a handle of it
  (`wake_sync_waiters`): a woken thread takes a core before it looks, so waking the others cost running threads
  their cores for nothing (`NX2NAVI_SYNC_WAKE_ALL=1` wakes them all).
- Host work gives the core back. Queue submits, presents, GPU waits, shader translation, `nn::fs` calls and NVN memory
  pool setup run without a core (`NVN_BLOCKING`, `kern_core_away`).
- Spin waits. Loops that only another thread can end send their back-edges through `rt_spin_gate`, which pauses the
  core once per pass and hands it to a waiter after 50 us of polling (`NX2NAVI_SPIN`, `NX2NAVI_SPIN_HANDOFF_US`).

### 4. Graphics

- NVN 2 front end (`nvn_api_*.c`, `nvn_obj.h`, texture layout and deswizzle, the Switch 2 GOB layout). The game
  records command buffers through it; nothing reaches the GPU until the game submits.
- Vulkan 1.3 back end. At `QueueSubmitCommands` the recorded NVN commands are replayed into Vulkan command buffers on
  the submitting thread (`vk_exec*.c`); a recorder thread makes the `vkCmd*` calls from a ring of packets
  (`vk_rec.c`).
- Memory pools are imported as host memory (`VK_EXT_external_memory_host`). On a discrete GPU, pools bound as buffers
  are mirrored in device-local memory and the pages the CPU wrote (write watch) plus the ranges the game flushes are
  uploaded before a replay (`vk_memory.c`); without the mirrors the GPU reads them over PCIe and Kokiri Forest runs at
  7 fps. GPU-rendered textures are written back when the CPU reads them.
- Uniform snapshot. The game rewrites per-object uniform blocks soon after it submits the frame that reads them; at
  the start of a submission's replay every uniform range it binds from CPU-written pools is copied, and the draws bind
  the copy (`vk_usnap.c`).
- Event order. Ocarina's queues hand work to each other with events, and its main queue waits each frame for a
  signal the game gives from the CPU once it has written the frame's data. The replay stops a submission at a wait
  whose signal has not been replayed and lets it go on after the signal (`vk_exec.c`, "event order across the game's
  queues"); a CPU signal counts for a submission only when it came before the copies above were taken, and a
  submission it comes too late for goes on at once in one of its own (`vk_exec_xfer.c`, "events the CPU signals").
  `NX2NAVI_EVO_TRACE=1` logs the order every frame was replayed in.
- Driver data. The NVN driver keeps a per-stage constant bank (c0). The back end fills it as the driver does: storage
  and uniform tables, push constants and, for NVN 2 compute, the dispatch's group counts in bytes 0-0xb.
- Storage buffers the shaders reach through pointer tables in memory are bound at the address the table holds
  (`GfxShaderRes.ptr`, `vk_exec_draw.c ptr_addr`).
- Bindless textures: the texture and sampler pools are descriptor arrays kept in step with the game's pool writes
  (`vk_exec_draw.c pool_sync`); slots that resolve alike share one lookup.
  Finalization is retained on registered slots independently of image-cache eviction: a stale CPU-backed descriptor
  cannot recreate an image from freed storage or follow newly reused CPU bytes. Registration makes it current again;
  finalized descriptors may still follow live GPU-owned content, and sample their own image while it still holds
  the texture (`vk_texture.c stale_kept`): the game finalizes ahead of the replay of its earlier frames. A finalize
  marks only the slots its own texture object registered (by initialization serial): the game initializes and
  registers the next terrain heightmap at the storage of the previous one before it finalizes that one
  (`vk_texture.c vk_texture_release`).
- Shaders: Ampere (SM86) code is lowered to Maxwell (`tools/xlate/AmpereLowering.cs`) and translated to SPIR-V by the
  pinned shader translator (`tools/vendor/ryujinx`, 1.3.2) with our patches (`tools/xlate/patches/`), built with .NET 10 NativeAOT as
  `nx2navi_xlate.exe` and run as separate processes. Translations are cached on disk; shaders and pipelines seen in
  play are prepared at the next boot. The game waits for it (`runtime/boot.c` `boot_wait`): about half a second
  when warm; after a GPU or driver change, or a translator change with nothing to carry (below), a "Preparing
  shaders" screen shows the progress (`ui/menu.cpp`
  `menu_boot_frame`). With the graphics pipeline library the boot builds recorded pipelines in record order for at
  most 20 s after the shaders are ready and leaves the rest to the pipeline workers below normal priority; a draw
  that meets one before it is built gets a fast-linked pipeline (`vk_pipeline.c` `PREWARM_PIPE_MS`).
  `NX2NAVI_PREWARM_FULL=1`, and drivers without the library, wait for every pipeline. `NX2NAVI_PREWARM_WAIT_MS` caps
  the whole wait, after which the rest finishes in the background.
- Translator updates: every cache key names the translator, so a new one has no translations. The cache carries the
  previous translator's (`NX2NAVI_SHADER_CARRY`, `runtime/gfx_shader.c` "Carry"): `shader-cache/identities.txt`
  lists the translators it holds translations of, and a shader the new translator has not translated yet uses the
  previous one's for the whole session, so neither the start nor play waits for a translation the update caused.
  After the boot prewarm a background pass on its own translator process at idle priority, which waits while the game
  translates, translates every stored program again for the next start, then deletes the old translations (a pruned
  copy of `shader-cache.pack` replaces the pack at the next start). A program the new translator refuses keeps its
  old translation. `--prepare-shaders` still translates everything with the current translator.
- Shader disk reads in play: a draw's shader that is in the disk cache but not in memory is read on the render thread
  within a budget of 1/8 of the wall time (2 ms at once), and past it by three loader threads while its draws are
  skipped as for a translation (`runtime/gfx_shader.c` `load_budget`). Opening a loose file is what costs (5.6 ms a
  pair the first time on Windows), so an update that carries the shaders over packs the cache once it holds 1,000
  loose translations (`tools/make_game.py` `PACK_LOOSE_MIN`).
- Texture pool streaming: the bindless pool's refresh before each draw creates at most `pool_create_budget` entries,
  and once a frame's refreshes have taken `pool_create_ms` (6 ms) none more until the next frame; the slots left keep
  their stand-in, and the frame's later refreshes skip them (`runtime/vk_exec_draw.c` `pool_refill`). An area load
  spreads over several frames instead of holding one for 200-400 ms.
- Mesh and task shaders run as `VK_EXT_mesh_shader`; indirect mesh draws read NVN's `{count, first}` record through a
  ring of Vulkan `{x, y, z}` records filled on the GPU. The copy cannot be recorded inside a pass, so the first such
  draw of a pass fills the records of the mesh draws that follow it too, and the pass is not cut at each of them
  (`runtime/vk_exec_draw.c` `mesh_ring_entry`).
- Mesh fallback (`mesh_fallback`: `auto` on a device without the extension, `1` on any device for testing): the task
  and mesh stages run as compute dispatches and the result is drawn as ordinary triangles.
  - The modules come from the translations the shader cache already holds, not from the translator
    (`runtime/mesh_fb.c`): a translated task or mesh module is the program as a compute function over shared memory
    plus an entry function that hands that memory to the mesh pipeline, and the fallback replaces the entry function
    by one that copies the memory into a storage buffer. The translator, its identity and every cache key stay as
    they are, so a device with mesh shaders keeps its shaders and never sees these modules, and a device without
    needs no other cache or prepared shader list.
  - A draw is: its record (the workgroup count, copied on the GPU when the game's record holds it); the task
    dispatch, which writes each task workgroup's payload into the task buffer; a scan (`shaders/meshfb_scan.comp`)
    that lists the mesh workgroups the tasks started, in task order, and writes their number into the record; the
    mesh dispatch, which writes each workgroup's vertex lines and primitive bytes into its slot of the mesh buffer;
    and an instanced indirect draw, one instance per slot, whose generated vertex stage looks the vertex of each
    primitive corner up in the slot (`runtime/vk_exec_draw.c` `mfb_draw`, `runtime/vk_mesh_fb.c`).
  - Each step reads what the one before wrote, so full barriers stand between them and every such draw ends its pass:
    a village frame's 45 mesh draws are 45 passes cut, 90 to 135 dispatches and about 270 barriers more than with mesh
    shaders. The buffers (`mesh_fallback_mb`, 128 MiB) are reused by every draw, so they hold the largest draw, not
    a frame; a draw that emits more loses what does not fit, and the log says how much it needed.
  - A program's modules and compute pipelines are made by worker threads at its first draw, which is skipped until
    they are ready (`mesh_fallback_workers`, auto: one for each 4 logical processors, 1 to 4, so a place's programs,
    which all arrive in its first frames, are not made one after the other); they are not recorded for the next start,
    where the pipeline cache makes them quick.
    Not carried: the far shadow cascades' coarser shading rate, a draw's first workgroup other than 0, and more
    workgroups in one draw than one compute dispatch may have.
- Pipelines: `VK_EXT_graphics_pipeline_library` where the driver has it: stage libraries compiled ahead, a fast link
  at a new state, an optimised pipeline replacing it from a background thread. A draw whose pipeline is not ready is
  skipped when its pass is one the game renders frame after frame: one of its targets was rendered in the last few
  frames, or in eight frames or more at any time, which keeps the first frame after a loading screen from reading as
  all new (`runtime/vk_target_frames.h`). A draw into a target rendered for the first time, and a full-screen draw,
  wait instead, from one pool of 50 ms a presented frame (`pipe_frame_wait_ms`; `runtime/vk_exec_draw.c`
  `prepare_draw_ex`, `frame_wait`); what is not ready when the pool is spent is skipped and built on a worker, so no
  frame waits for pipelines much longer than that. Three things keep
  that short on a first visit (`runtime/vk_pipeline.c`): a pipeline whose fragment shader reads a target of its own
  pass is fast-linked like any other, the feedback loop flags being part of the fragment output library alone
  (`gpl_feedback`); the library workers also link the pipelines of skipped draws, so a burst of new materials is
  linked on several threads while the pool is inside optimised builds (`lib_worker`, `fast_take`); and the first
  draw of a submission that misses has the rest of the submission read ahead, which queues the stage libraries of
  every pipeline the later draws need before the replay reaches them (`runtime/vk_exec.c` `vk_exec_ahead`,
  `vk_pipeline_hint`). Mesh pipelines have no libraries and are built whole. Compute pipelines
  are compiled outside the pipeline lock, and one compile serves every thread that needs it. The boot builds the
  pipeline of every compute shader play recorded or the shader preparation listed, but keeps only those of the shaders
  dispatched most recently (`compute-used.bin`, `compute_keep`): the others go into the pipeline cache and are
  destroyed, and their first dispatch creates them from it. A compute shader of more than half a million SPIR-V words
  is built ahead only when a run has dispatched it: a pipeline of one holds about 100 MiB of device memory whether it
  is used or not, also after it is destroyed, and the list has about twenty of them that the game never dispatches
  (`runtime/vk_pipeline.c`, "compute pipelines the boot keeps").
  The pipeline cache is two files beside the game. `pipeline-cache.bin` holds what the preparation built and what
  earlier runs made; what a run compiles goes into `pipeline-cache-play.bin`, so one new pipeline in play writes a
  small file and not hundreds of megabytes. A cache is written only when its data grew. The small file is merged
  into the large one at a start once it reached `pipeline_cache_play_mb`, and by every preparation; the stage
  libraries the library warm builds in play go to the driver's own cache alone (`runtime/vk_pipeline.c`, "the play
  cache", `cache_save`).
- The game's tensor-core upscaler is replaced by a decode-and-scale pass (`vk_tonemap.c`); `vk_fsr.c` runs AMD FSR 1
  (EASU, RCAS) on the present when the window is larger than the picture.
- Anti-aliasing (`vk_aa.c`, `NX2NAVI_AA`): the game anti-aliases with its own SMAA, three compute passes after the bloom
  composite (edges, blending weights, composition into the scene target as square-rooted RGB9E5 words). `off`, `fxaa`
  and `smaa` leave out the first two and replace the composition with `shaders/aa_post.comp` or SMAA at its Ultra preset
  (`shaders/smaa_*.comp`); the game's passes are known by a hash of their guest code (`vk_exec_draw.c` `aa_dispatch`).
  `taa` is DLSS or FSR at the scene's size with the game's SMAA. While DLSS or FSR runs (`dlaa`, any mode), the other
  values still choose the pass: in the native modes it smooths the upscaler's picture after the bloom, in the scaling
  modes it runs on the scene before the upscaler scales it.
- HDR (`vk_hdr.c`): the game tonemaps its scene into a float target, upscales it, draws the HUD into a second float
  target and copies that into the 8-bit sRGB window texture it presents. With `NX2NAVI_HDR=1` the tonemap shader gets
  the function of `shaders/hdr_tonemap.frag` spliced in where its SDR curve clamps each channel, so the float picture
  holds the HDR picture relative to the HUD's white (the function gets the values the curve takes, and the scene
  before the game's exposure gain with that gain, which it anchors mid grey on); the game's FSR 1 pass loses its output
  clamp; the present reads the float picture in place of the window texture and `shaders/hdr_encode.comp` writes PQ
  for an HDR10 swapchain, showing the picture as a display of gamma 2.2 would (the game is graded for one: decoded as
  its sRGB encode, the darkest tones would sit far above black). The look is fixed; the settings are the peak, game and
  HUD whites.
  `NX2NAVI_HDR=native` reports an HDR10 display to the game, which then renders and presents its own PQ picture.
  The menu switches between off and `1` during a run: the splice, the HDR extensions and the settings in the support
  buffer are there from the start whatever is set (`vk_hdr_armed`), the spliced tonemap returns the game's SDR result
  while the swapchain is SDR, and a change of the setting makes the swapchain again in the other format
  (`gfx_present.c` `window_settings_live`). Only `native` waits for a restart, as the game asks about the display
  once. `NX2NAVI_HDR_LIVE=0` leaves the HDR path out of a run that starts with HDR off.
- ASTC textures are uploaded as they are where the device samples ASTC, and decoded by a compute shader everywhere
  else (`vk_astc.c`); under 20 GiB of video memory the decoder encodes them to BC3.
- Texture detail (`texture_detail`, `vk_texture.c` "texture detail"): on a card with little video memory the large
  mipmapped ASTC textures the CPU fills are made without their largest mip level (reduced) or their two largest (low),
  which is three quarters and fifteen sixteenths of their memory. The image holds the game's levels from the one it
  starts at, and uploads, views and sampler LOD clamps count from there. A texture a shader reads by texel position, or
  whose size it uses, is made whole (`gfx_shader.c` `reflect_exact`). `auto` picks by the device-local budget.
- Occlusion queries never block the CPU; a result that is not ready counts as visible.
- Allocation: Vulkan Memory Allocator. Debugging: Vulkan validation layers, per-pass frame dumps (`NX2NAVI_DUMP`,
  `vk_dump.c`), a sampling profiler (`NX2NAVI_PROFILE`, `tools/prof_report.py`) and a flight recorder
  (`NX2NAVI_FLIGHT`). RenderDoc cannot capture, because it hides `VK_EXT_external_memory_host` from the application.

Maxwell behaviour the back end has to match: [maxwell.md](maxwell.md).

### 5. Toolchain

- Generated C: MSYS2 UCRT64 Clang + LLD. The generated code uses GNU extensions (`__int128`, `musttail`, vector
  types) that `cl.exe` cannot compile.
- Host code: the same Clang, C11.
- Builds write a Ninja file and run MSYS2 Ninja. Every object is hashed before linking, because a faulty RAM module
  once flipped bits in stored objects.
