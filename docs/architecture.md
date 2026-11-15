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
  A value in the game's RequestUpdate no longer ends the process (`audren.c` `audio_bad`); OpenAudioRenderer's
  parameters and the request's buffer descriptors still do. An update whose sections do not match the renderer is
  refused whole with 0x5299, which the SDK hands back to the game, and a record, effect or sink whose values the
  renderer cannot use is left out, passed through or skipped, with one log line per kind. `NX2NAVI_AUDIO_STRICT=1`
  ends the process there instead, for bug reports.
- Window, keyboard and mouse: Win32 (`gfx_window_win.c`, `input_win.c`). Gamepads and audio output: SDL3 (every
  connected gamepad is open; the game reads the one pressed last, `input.c`). Voice: libopus. Movies: a host VP9
  decoder over libvpx replaces the SDK's `movie::Decoder` (`runtime/ov_movie.c`).
- Settings and menu: every key with its default is in one table (`cfg.c`), read from `nx2navi.ini` and `NX2NAVI_*`
  variables and written back when the in-game menu changes it. The menu and the performance overlay are Dear ImGui,
  drawn into the swapchain image (`runtime/ui/`); their text is in `ui_text.c`.
- Log: in a game run the lines go through a queue to a thread that writes them (`runtime/logq.c`), so a stalled
  write to the log file or the console does not hold the thread that logged. The log thread gathers the lines of
  a quarter of a second, or half a queue, into one write, so the log is up to that much behind the game. A thread's
  lines reach the log in the order it logged them: a line waits for the queue's lock, and one that finds the queue
  full is written after everything queued before it. A fatal error, the crash report and the end of the process write
  the queue out first (`rt_log_sync`). `NX2NAVI_LOG_ASYNC=0` writes each line where it is logged.
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
- Several submitting threads. The game submits from several threads and presents from one of them. Their replays take
  turns on the submission lock (`vk_sync.c`). The state a present closes for the frame (the DLSS state, the motion
  tables, the frame count conditional draws go by) is reset between two replays: by the present when the lock is free,
  else first thing in the next submission or by the present thread before the frame's blit, whichever comes first
  (`vk_frame_owed.h`, `NX2NAVI_PRESENT_ORDER`). A present held for event-ordered work is put out once and in order by
  whichever thread's submission finds its work done (`vk_present_held.h`); submitting threads only try its lock, and
  the presenting thread waits for it only while another thread puts a held present out. A recording two threads
  submit at once gets a copy of its own for the second, for its deferred binds (`nvn_submit_guard.h`). Held work is
  asked about without the lock from a published copy (`vk_evo_peek.h`). The log counts the overlaps with
  `NX2NAVI_TRACE=perf` ("enters a submit ... while"), and at exit what they cost ("frame bookkeeping: ...").
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
  `NX2NAVI_PREWARM_FULL=1`, and drivers without the library, wait for every pipeline. A start that finds the
  pipeline cache written by another GPU or driver waits for the whole warm on that screen, since nothing is built
  then and all of it would compile beside the game: every recorded pipeline, the compute pipelines, and the
  prepared shader variants built into the driver's own cache when a sample says it lacks them (`vk_pipeline.c`
  `bf_decide`, `bf_wait`). `boot-warm-owed.txt` next to the caches lets a start go on with a warm that an earlier
  one began and did not end, three starts at most, and says when the warm for a GPU and driver is done
  (`runtime/boot_warm.h`); it waits `NX2NAVI_PREWARM_FULL_S` seconds at most (600), after which the rest is built
  beside the game, and `NX2NAVI_PREWARM_FULL=0` turns it off. A start that dies in the boot's builds leaves a
  marker or a journal; the crash handler's note (`vk_pipeline_crash_note`) tells the next start whether that was
  a crash, which skips the prebuild or the variants, or a start that was closed or stopped, which skips nothing.
  `NX2NAVI_PREWARM_WAIT_MS` caps the whole wait, after which the rest finishes in the background.
- Translator updates: every cache key names the translator, so a new one has no translations. The cache carries the
  previous translator's (`NX2NAVI_SHADER_CARRY`, `runtime/gfx_shader.c` "Carry"): `shader-cache/identities.txt` lists
  the translators it holds translations of, and a shader the new translator has not translated yet uses the previous
  one's for the whole session, so neither the start nor play waits for a translation the update caused. Except a
  previous translation that indexes the texture pool with the earlier length (32768 entries, the 0.9.2 translator's),
  which would read slot 32767 for every texture numbered from 32768 up (`runtime/gfx_shader.c` `carry_usable`): such a
  shader is translated again, in the boot prewarm when the boot names it, otherwise at first use (its draws are skipped
  meanwhile; a compute dispatch waits for the translation). A program the current translator fails keeps even such a
  translation (`carry_last`). After the boot prewarm a background pass on its own translator process at idle priority,
  which waits while the game translates, translates every stored program again for the next start, then deletes the old
  translations (a pruned copy of `shader-cache.pack` replaces the pack at the next start). A program the new translator
  refuses keeps its old translation. `--prepare-shaders` still translates everything with the current translator.
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
- Mesh fallback (`mesh_fallback`: `auto` on a device without the extension and on AMD's Windows driver for RDNA 1 and
  2, where native mesh pipelines lost the device in play, `1` on any device for testing, `0` never;
  `runtime/vk_mesh_path.h`): the task and mesh stages run as compute dispatches and the result is drawn as ordinary
  triangles.
  - The modules come from the translations the shader cache already holds, not from the translator
    (`runtime/mesh_fb.c`): a translated task or mesh module is the program as a compute function over shared memory
    plus an entry function that hands that memory to the mesh pipeline, and the fallback replaces the entry function
    by one that copies the memory into a storage buffer. The translator, its identity and every cache key stay as
    they are, so a device with mesh shaders keeps its shaders and never sees these modules, and a device without
    needs no other cache or prepared shader list.
  - A draw is: its record (the workgroup count, copied on the GPU when the game's record holds it, then cut to what
    the buffers hold by `shaders/meshfb_clamp.comp`, so a garbage count cannot launch work past them); the task
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
  - The game draws a culled list of grass or leaves twice from one indirect record, depth first and colour after, and
    the depth draw's smaller program is ready first. While the colour draw is skipped for its modules or pipeline,
    the depth draw of the same record is skipped too (`mfb_hold_role`, `mesh_fallback_hold`): alone it left the
    list's depth in the scene with nothing drawn over it, and the sky's colour showed through the ground.
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
  `vk_pipeline_hint`). Mesh pipelines are linked the same way (`gpl_mesh`, `vk_pipeline.c` `mesh_lib_init`): the task
  and mesh stages make one pre-rasterization library, no vertex input library is linked (a mesh pipeline has no such
  state), and the fragment shader and fragment output libraries are the vertex pipelines' where their keys allow (a
  mesh pipeline's fragment library differs where the mesh stage leaves an input unwritten, and on a device that
  sets a shading rate per draw). Their libraries need the mesh and task stage together, so they are made from the
  pipeline records and not from the single prepared shaders: the preparation and every boot build those of the
  listed and recorded mesh pipelines into the caches (`mesh_pre_note`). With `gpl_mesh = 0`, and wherever the start's
  log says so, a mesh pipeline has no libraries and is built whole, 0.2 to 1.1 s the first time. Compute pipelines
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
- The game's tensor-core upscaler is replaced by a decode-and-scale pass (`vk_tonemap.c`), which enlarges the scene
  with FSR 1's edge-adaptive filter (`NX2NAVI_STANDIN_FILTER`, `bilinear` for the plain one) unless DLSS or FSR 3.1
  scales it; `NX2NAVI_NATIVE_TENSOR=1` runs the game's own network programs instead (`vk_exec_draw.c` `native_tensor`,
  off by default). Without DLSS or FSR the scene is `scene_share` of the picture (`widescreen.c` `plain_share`, the
  game's own 3/4 by default); at 1, the Upscaling row's Native, it is the picture's size and the pass only decodes it,
  texel for texel. `vk_fsr.c` runs AMD FSR 1 (EASU, RCAS) on the present when the window is larger than the picture.
- Anti-aliasing (`vk_aa.c`, `NX2NAVI_AA`): the game anti-aliases with its own SMAA, three compute passes after the bloom
  composite (edges, blending weights, composition into the scene target as square-rooted RGB9E5 words). `off`, `fxaa`
  and `smaa` leave out the first two and replace the composition with `shaders/aa_post.comp` or SMAA at its Ultra preset
  (`shaders/smaa_*.comp`); the game's passes are known by a hash of their guest code (`vk_exec_draw.c` `aa_dispatch`).
  `taa` is DLSS or FSR at the scene's size with the game's SMAA. While DLSS or FSR runs (`dlaa`, any mode), the other
  values still choose the pass: in the native modes it smooths the upscaler's picture after the bloom, in the scaling
  modes it runs on the scene before the upscaler scales it.
  DLSS and FSR 3.1 read the bloom composite's output, which the game has already exposed and tonemapped;
  `NX2NAVI_DLSS_PREBLOOM=1` (a test, off by default) runs them at the bloom chain's first pass on the scene's linear
  light instead, and draws the lens flares and the bloom composite after them at the picture's size (`vk_dlaa.c`,
  "before the bloom").
- HDR (`vk_hdr.c`): the game tonemaps its scene into a float target, upscales it, draws the HUD into a second float
  target and copies that into the 8-bit sRGB window texture it presents. With `NX2NAVI_HDR=1` the tonemap shader gets
  the function of `shaders/hdr_tonemap.frag` spliced in where its SDR curve clamps each channel, so the float picture
  holds the HDR picture relative to the HUD's white (the function gets the values the curve takes, and the scene
  before the game's exposure gain with that gain, which its default tone mapper anchors mid grey on); the game's FSR 1
  pass loses its output clamp; the present reads the float picture in place of the window texture and
  `shaders/hdr_encode.comp` writes PQ for an HDR10 swapchain, showing the picture as a display of gamma 2.2 would (the
  game is graded for one: decoded as its sRGB encode, the darkest tones would sit far above black). The settings are
  the peak, game and HUD whites, the tone mapper (`NX2NAVI_HDR_TONEMAP`: the observer-response mapper, the game's own
  curve or the luminance mapper), what each tone mapper takes, the grading sliders and the gamma correction
  (`NX2NAVI_HDR_GAMMA`, with every tone mapper); they travel in five vec4 the support buffer gains, and at their
  defaults the shader leaves every grading step out.
  `NX2NAVI_HDR=native` reports an HDR10 display to the game, which then renders and presents its own PQ picture.
  The menu switches between off and `1` during a run: the splice, the HDR extensions and the settings in the support
  buffer are there from the start whatever is set (`vk_hdr_armed`), the spliced tonemap returns the game's SDR result
  while the swapchain is SDR, and a change of the setting makes the swapchain again in the other format
  (`gfx_present.c` `window_settings_live`). Only `native` waits for a restart, as the game asks about the display
  once. `NX2NAVI_HDR_LIVE=0` leaves the HDR path out of a run that starts with HDR off.
- Frame pacing (`nvn_api_obj.c` `nvnQueuePresentTexture` and `pace`, `frame_rate.c` `frame_step_cap_ns`,
  `vk_backend.c` `present_main`): the frame limit (`NX2NAVI_FPS_LIMIT`, live) counts the frames the game draws.
  Ocarina steps by the clock, so the pacer holds the frames to the limit alone: at most `LIMIT_FREE_MAX_HZ` (240, also
  for "unlimited"), and while a movie plays at the movie's rate (`movie_cap_hz`, `NX2NAVI_MOVIE_FPS`). Each game
  present gets a slot one period after the one before: an early frame waits for it, a frame late by less than a period
  keeps the phase, a later one starts again from now, and while a movie plays the slots are kept through a lateness of
  up to a second (`MOVIE_CATCHUP_NS`). The game's present returns at once; the present thread blits the picture,
  waits until the slot (`wait_until`, with `pace_smooth`'s floor of seven eighths of the recent interval, off with
  frame generation) and presents under the present and queue locks (`gfx_present.c` `flip_locked`). A window image is
  free again one period after its slot, and its acquire also waits for the GPU to finish the image's previous frame
  (`NX2NAVI_ACQUIRE_WAIT`); `frame_step_present` gives `ocarina_step` the slot times, snapped to the paced rate within
  3%.
- One limiter is the goal: the driver's low latency (`vk_lowlat.c`), Reflex with DLSS Frame Generation (`vk_dlssg.c`),
  RivaTuner Statistics Server (`rtss_limit.c`) or the game's pacer holds the frame limit, and the log's "frame rate:"
  and "lowlat:" lines name the one in charge. The pacer is the fallback wherever the other limiter's hold is not
  proven: before the first host present, while a movie plays (the pacer keeps the movie's slots, which no other
  limiter knows), once no picture went out for 200 ms (a minimised window, `frame_hold`), for the rest of the run once
  the frames show the other limiter letting them through faster than the limit (for RTSS also once it was not seen
  holding them for 30 s), and after any failure. While RTSS or Reflex holds the limit the pacer keeps only the
  240-frame ceiling the game's code needs (`rtss_decide.h` `rtss_pace_period`, `frame_rate.c` `frame_step_ceiling_ns`).
  Frame generation keeps RTSS out (it would count the generated frames); with AMD's frame generation the game's pacer
  holds the drawn frames beside it. The exceptions as they stand: `pace_smooth` still waits on the present thread while
  RTSS holds the limit; at a play limit equal to a movie's rate (60, the default, for the game's 60 fps movies) RTSS's
  profile keeps that limit and RTSS holds the movie beside the pacer; once RTSS hooks the game with the limit in its
  profile it holds the frames beside the pacer until it is seen holding them, for 30 s at most, after which the
  profile goes back for the run; and a limit set in RTSS itself
  still holds the game where the game keeps RTSS out, which the log ("two limiters") and the
  frame rate row of the menu point out, and the same where RTSS is out of it for the run because its profile cannot be
  written, its hooks library does not load or it never showed its hold, which the log points out the same way (the
  menu shows the administrator note for the first, nothing for the second and that RivaTuner does not limit the game
  for the third).
- RivaTuner Statistics Server (`rtss_limit.c`, `NX2NAVI_RTSS`, restart; its decisions in `rtss_decide.h`): the game's
  first present starts a thread that looks RTSS up (`InstallDir` under `HKLM\SOFTWARE\WOW6432Node\Unwinder\RTSS`,
  `RTSSHooks64.dll`) and reads its shared memory (`RTSSSharedMemoryV2`) every 500 ms. While RTSS runs, the limit goes
  into RTSS's profile for the executable (`Profiles\<exe>.cfg`, through the hooks library's profile calls, read back
  from the saved file; a fractional rate over a denominator where RTSS knows one, else rounded down to whole frames)
  once the first picture went out and the limit stayed the same for two polls, so a want that flips (low latency at a
  swapchain rebuild) writes nothing. A movie leaves the play limit in the profile; only a play limit below the movie's
  rate gives the profile no limit for the movie's length, so that RTSS does not hold the movie below its rate. Above
  the movie's rate RTSS lets the frames the pacer sends at once after a late frame through faster than the movie runs;
  at the movie's own rate (a limit of 60) whether RTSS spaces them out, and the picture falls behind the sound, is not
  measured. The pacer stands aside (`rtss_limit_active`) only once RTSS is seen holding the game: it lists the
  process with its Vulkan presents, has read the profile (its update flag clear) and measured a period of the game's
  frames lately, no movie plays and a picture went out within 200 ms. The pacer keeps the limit while RTSS does not run
  or has not hooked the game, for a limit RTSS refuses, and for the rest of the run when the profile cannot be written
  (no administrator rights), the hooks library does not load, or RTSS's own frame count of the game is above 1.05
  times the limit in three new periods in a row (a period RTSS no longer moves counts neither way), or RTSS lists the
  game with the limit while pictures go out for 60 polls in a row (30 s) and never shows its hold (no Vulkan presents
  seen, the profile not read, no period measured); the menu names the first and the last two of these. Frame
  generation, the
  driver's low latency and `NX2NAVI_RTSS=0` keep RTSS out (`RTSS_OUT_*`): the profile gets back what it held before
  the run changed it, and while RTSS is kept out its thread does not load the hooks library (but for the one restore
  below at the start) and only reads RTSS's own settings for the game; a later limit (frame generation or low latency
  switched off) loads it and hands RTSS the limit as usual, and `NX2NAVI_RTSS=0` keeps the thread read-only for the
  run. While RTSS lists the game the log names those
  settings (limit, frame time limit, scanline sync, display sync, Reflex sleep, passive wait) once per change. Every
  orderly exit puts the profile back as it was before the run (`rtss_limit_restore`), and no want reaches it after
  that. What the profile held is also kept in `rtss-profile.txt` next to the executable, one line per profile, written
  before the run's first change of it, taking each later value before the profile does, and removed once the profile
  is back: a start that finds a line left by a run that did not close (a kill, a hang, the device-lost watchdog, a
  crash or a fatal error) puts that profile back first (`left_start`), unless the player changed it since; frame
  generation and low latency, which keep RTSS out, have the hooks library loaded for that one restore, and a start with
  `NX2NAVI_RTSS=0` leaves the profile and the line as they are and says so in the log. A crash and a fatal error
  (`rtss_limit_crash`) make no call into RTSS's library while the line is in the file; where the file could not be
  written they put the profile back as an orderly exit does, never from RTSS's own thread, which may hold the lock
  that takes. A file that cannot be written or read leaves the run as before, with a line in the log, and the lines of
  other executables in it as they are; profiles that versions before the file left with the game's limit cannot be
  told from the player's own.
- Measuring pacing: `NX2NAVI_TRACE=present` logs a line per picture (the host present's interval, the game's present
  interval, the wait for the slot and how far past it the present went), `NX2NAVI_TRACE=perf` each wait's share of the
  frame ("bound:"), and `tools/perf_summary.py` condenses both. Both measure before the host present, so a wait RTSS
  adds inside it does not show there: PresentMon is the reference for what reached the screen. Compare runs with the
  same executable name, cache state and save, and keep `trace` off when counting hitches; `NX2NAVI_MEM_CENSUS` and
  the memory report every 3600 frames are the runtime's own hitches.
- Low latency (`vk_lowlat.c`, `NX2NAVI_LOW_LATENCY`, off by default): the graphics driver holds each frame's start, so
  the pad is read as late as the picture's time allows and no finished picture waits. NVIDIA's mode runs through
  `VK_NV_low_latency2`: the window's swapchain is made with the latency mode, the frame sleeps in the driver before its
  first pad read or window image acquire, whichever the game makes first, and its stages are marked with the frame's
  number, on the guest thread up to the first submission, on the replaying thread at the end of its submissions and on
  the present thread around the host present. AMD's runs through `VK_AMD_anti_lag`: its input stage where the frame
  starts, its present stage before the host present. While the mode is in effect the driver's sleep keeps the frame
  limit and the pacer holds nothing (`nvn_api_obj.c` `nvnQueuePresentTexture`, `vk_lowlat_pace`); the pacer takes the
  limit back while a movie plays, while no present goes out, and for the rest of the run when the frames show that the
  driver does not hold it. DLSS Frame Generation brings its own low latency (`vk_dlssg.c`), and the direct path is off
  in such a run; NVIDIA's is also off while another frame generation presents the frames. There Reflex's sleep holds
  the frame limit the same way, through Streamline's frame limit (`nx2_sl.cpp` `nx2sl_reflex2`), with the same
  exceptions, a DLL older than that export and a limit Streamline refuses besides (`vk_dlssg.c` `vk_dlssg_pace`,
  `dlssg_pace.h`); the pacer stands aside only once Streamline took the limit and keeps the game's ceiling of 240
  (`frame_rate.c` `frame_step_ceiling_ns`). Since the SDK does not say whether the driver's interval counts the frames
  the game draws or every frame on screen, the frames are checked: about the multiplier periods apart switches Reflex
  to the period over the multiplier once, and the same again or frames too soon give the limit to the pacer for the
  run. `NX2NAVI_REFLEX_LIMIT=0` (live) leaves the limit to the pacer, as before Reflex's frame limit was used.
  The device gets the drivers' extensions only in a run that starts with the setting on (`vk_device.c`
  `create_device`): turning it on takes a restart, and with it off the device is made as it was before the mode.
- ASTC textures are uploaded as they are where the device samples ASTC, and decoded by a compute shader everywhere
  else (`vk_astc.c`); under 20 GiB of video memory the decoder encodes them to BC3.
- Texture detail (`texture_detail`, `vk_texture.c` "texture detail"): on a card with little video memory the large
  mipmapped ASTC textures the CPU fills are made without their largest mip level (reduced) or their two largest (low),
  which is three quarters and fifteen sixteenths of their memory. The image holds the game's levels from the one it
  starts at, and uploads, views and sampler LOD clamps count from there. A texture a shader reads by texel position, or
  whose size it uses, is made whole (`gfx_shader.c` `reflect_exact`). `auto` picks by the device-local budget.
- Occlusion queries never block the CPU; a result that is not ready counts as visible.
- Holes in video memory: the allocator takes device memory in blocks and gives a block back only when it is empty,
  so a long session leaves textures scattered between holes the driver still counts. The log's `mem: holes` line
  says how much that is and how much could come back (`vk_memory.c` `holes_log`). With `NX2NAVI_TEX_COMPACT=1`
  (off by default) textures no shader sampled for ten seconds are copied on the GPU out of the emptiest blocks into
  the holes of fuller ones, a few per frame and only in frames with time to spare; which blocks is decided from a
  table of what lies in them (`vk_tex_compact.h`), and the old image of a moved texture stays until no descriptor
  set and no submission can still name it (`vk_texture.c`, "the pass").
- Memory report: beside each `mem: holes` line, `mem: by owner` splits the device-local memory the driver counts into
  VMA's allocations by the file that made them (`vk_vma_budget.c`, "owners"; the textures further by the texture
  cache's census), the free space inside VMA's blocks and what VMA never allocated (pipelines, descriptor memory, the
  swapchain, DLSS and frame generation features); `mem: host` gives the host-visible allocations, the process's
  commit and working set and the game's heap; `vk: released textures` how often released textures came back and how
  old they were, and how many were made again after a drop (`vk_memory.c` `owner_log`). A figure that only climbs over
  a session is a leak. Three lines follow at the same period (`vk_memory.c` `beyond_log`): `mem: outside VMA` gives the
  driver's usage and budget per heap against VMA's blocks in it, and counts what the runtime made outside VMA where the
  driver gives no size (graphics pipelines whole, fast-linked and optimised, those no draw used for ten minutes and
  those none did, stage libraries by kind, compute pipelines, layouts, descriptor pools) beside the swapchain's pixels
  and DLSS's and FSR's figures; `mem: system` splits the process's commit into the game's heap and physical maps, the
  device-local memory (Windows charges it to the process), VMA's host-visible blocks, the C runtime heap with the shares
  the runtime can name (shader caches, with their SPIR-V as held against uncompressed and the decompressions, pipeline
  tables, the texture cache and its decode temporaries, the log queue, the flight recorder), the data of the driver's
  pipeline caches (asked once a minute on the autosave thread) and the rest;
  `mem: growth` names what grew by more than 64 MiB since the report before and since the first report in play. Every
  two minutes of play, when the window loses the focus in play and at exit, `mem: growth ranking` ranks the categories
  by their growth since play began and marks those that only climbed (`mem_growth.h`).
- Released textures by age: the game releases a texture when what uses it leaves the view and takes it back when that
  returns; the cache keeps the released image so that the take-back finds it. In play the take-backs came within a
  minute of the release or never, so a released texture goes once it has gone unused for a hold of one minute, two
  for content only the GPU made (`vk_texture.c` `drop_aged`, `vk_tex_release.h`, `tex_released_hold`). The size bound
  (a thirty-second of the video memory budget, 384 MiB to 1 GiB) is what the cache costs in video memory: the
  allocator's blocks go back to the driver only when empty, so what the released textures hold at their fullest
  stays in use for the session. Young textures may pass it, up to twice it, while the card and the system's commit
  have room. Times are wall seconds from a clock of
  the presents, not present counts. What a switched-off upscaler or SMAA held is freed after it has gone unused for a
  while (`vk_dlaa.c` `idle_release`, `vk_fsr.c` `idle_note`, `vk_aa.c` `work_idle`). What DLSS or FSR lets go of at
  a change of size or mode (a network, an FSR context, a work image) goes once the GPU is past the last submission that
  used it, and an image frame generation reads at a present some presents later, without a wait for the device on the
  replaying thread (`vk_dlaa.c` "what the upscalers let go of", `vk_up_defer.h`; `upscaler_idle_wait = 1` waits for
  the device at each such change as before, and the freeing keeps these gates).
- Translated shaders at rest: the shader memory cache keeps every shader for the whole session, but a shader's SPIR-V
  is read only while modules are made of it. A background thread compresses (zstd, checked by decoding it) the SPIR-V
  of each shader no reader used for `shader_compress_idle_s` seconds (30) and frees the plain copy; a reader pins it
  and gets it decoded back, about 60 us for a mean module (`runtime/gfx_shader.c` "SPIR-V at rest",
  `runtime/spv_store.h`). The SPIR-V keeps about a quarter of its size. What the drawing thread compares at pipeline
  misses and draws (the module's hash and its Binding decorations) is kept apart and needs no decode; a compute
  pipeline created at a dispatch and a mesh shader's first pipeline key decode on that thread, which the memory report
  counts. `shader_compress = 0` keeps it plain.
- Room levels: a thread samples every two seconds how much room the card and the system have and turns each into a
  level, roomy (over 40% free), normal (over 20%), tight (over 8%) or critical, with five points of hysteresis upward
  (`runtime/mem_budget.h`, `mem_budget.c`): the card's from the driver's budget against this process's use, the
  system's from the lower of the commit's headroom and the physical memory free. A change is logged, and the memory
  report's `mem: system` line ends with both levels. The SPIR-V at rest is compressed after 5 s unused while the system
  is tight and after 1 s while it is critical. `mem_budget_test_vram_mb` and `mem_budget_test_ram_mb` pretend a side is
  that small, to test what gives memory back on a large machine; they leave the texture cache's own rules alone.
- Allocation: Vulkan Memory Allocator. Debugging: Vulkan validation layers, per-pass frame dumps (`NX2NAVI_DUMP`,
  `vk_dump.c`), a sampling profiler (`NX2NAVI_PROFILE`, `tools/prof_report.py`), a flight recorder
  (`NX2NAVI_FLIGHT`) and bug captures, which a player's F8 writes and a test run starts from (`bugcap.c`,
  [bug-capture.md](bug-capture.md)). RenderDoc cannot capture, because it hides `VK_EXT_external_memory_host` from the application.

Maxwell behaviour the back end has to match: [maxwell.md](maxwell.md).

### 5. Toolchain

- Generated C: MSYS2 UCRT64 Clang + LLD. The generated code uses GNU extensions (`__int128`, `musttail`, vector
  types) that `cl.exe` cannot compile.
- Host code: the same Clang, C11.
- Builds write a Ninja file and run MSYS2 Ninja. Every object is hashed before linking, because a faulty RAM module
  once flipped bits in stored objects.
