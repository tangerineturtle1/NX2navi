// Interface of the graphics layer: window, shader service, back ends and the NVN front end.
#pragma once
#include <stdint.h>
#include "host.h"
#include "nvn_obj.h"

// ---- moved from ov.h
enum { OE_MSG_FOCUS_STATE_CHANGED = 15, OE_MSG_OPERATION_MODE_CHANGED = 30, OE_MSG_PERFORMANCE_MODE_CHANGED = 31 };
void oe_push_message(uint32_t message);                   // ov_system.c
void oe_set_focus(int in_focus);                          // ov_system.c: GetCurrentFocusState; NX2NAVI_ASSUME_FOCUS pins 1
typedef struct { uint64_t alloc, free, realloc, user; } OvNvAllocator;
typedef struct { OvNvAllocator graphics, devtools; uint64_t memory, memory_size; int initialized; } OvNvState;
const OvNvState *ov_nv_state(void);                        // ov_gfx.c

// ---- GPU address window for CPU-invisible pools (section 3.1)
#define RT_GPU_VA_BASE 0x8000000000ull                     // 2^39, never mapped on the host
#define RT_GPU_VA_SIZE (64ull << 30)

// ---- window, vsync, focus (gfx_present.c and the window back end, gfx_wsys.h; vk_device.c). Host threads only; never run guest code.
void gfx_window_start(void);                               // window thread, Vulkan device, swapchain; idempotent
// NX2NAVI_GPU_TIMELINE=<frame> (vk_sync.c "frame timeline"): an event of the frames it records, at CPU time t (0: now)
// with two values; gfx_tl_present counts the presented frames and writes the record to the log when it is complete.
void gfx_tl_note(int kind, uint64_t t, uint64_t a, uint64_t b);
void gfx_tl_present(void);
void gfx_vsync_bind(uint64_t system_event);                // nn::vi vsync event; signalled at 60 Hz
uint64_t gfx_vsync_ticks(void);                            // ticks since start
uint64_t gfx_vsync_time_ns(uint64_t tick);                 // kern_now_ns() time of a tick (pacing)
double gfx_window_refresh_hz(void);                        // refresh rate of the window's monitor; 0 unknown
uint64_t gfx_window_hosted_ns(void);                       // kern_now_ns() of the newest host present; 0 before one
int gfx_window_focused(void);
// gfx_present.c: who holds the present, queue and window-start locks, read without taking them (the hang watch)
typedef struct { uint32_t present, present_depth, queue, queue_depth, start; } GfxLockHolders;
void gfx_lock_holders(GfxLockHolders *h);
int gfx_window_in_front(void);   // the window has the focus and is not minimized: the game is expected to present
int vk_hdr_native(void);   // NX2NAVI_HDR = native: the game's own HDR picture (vk_hdr.c)
int gfx_picture_rect(float out[4]);   // where the last present put the picture, fractions of the swapchain (x, y, w, h); 0 before one

// ---- shader service client (gfx_shader.c)
enum { GFX_STAGE_VS, GFX_STAGE_TCS, GFX_STAGE_TES, GFX_STAGE_GS, GFX_STAGE_FS, GFX_STAGE_CS,
       GFX_STAGE_TS, GFX_STAGE_MS };   // TS / MS: NVN2 task and mesh programs (VK_EXT_mesh_shader)
enum { GFX_TEX_1D = 1, GFX_TEX_1D_ARRAY, GFX_TEX_BUFFER, GFX_TEX_2D, GFX_TEX_2D_ARRAY, GFX_TEX_2D_MS,
       GFX_TEX_2D_MS_ARRAY, GFX_TEX_3D, GFX_TEX_CUBE, GFX_TEX_CUBE_ARRAY };   // gfx_shader.c tex_names order
enum { GFX_TYPE_FLOAT, GFX_TYPE_SINT, GFX_TYPE_UINT };
typedef struct {                                           // the translator's answers (Program.cs Accessor)
    uint8_t tex_type[32], img_type[32], img_format[32];    // per c2 slot; 0 = not asked; types are GFX_TEX_*,
                                                           // img_format is the translator's TextureFormat
    uint8_t attr_type[32], frag_out_type[8];               // GFX_TYPE_*
    uint16_t local[3]; uint32_t shared_bytes;
    uint8_t depth_minus_w;
    uint8_t mvprev;                                        // VS: also write last frame's position (patch 0015,
                                                           // vk_mvprev.c); packed as bit 7 of the depth_minus_w byte
} GfxShaderAnswers;
typedef struct GfxShader GfxShader;                        // SPIR-V words + parsed reflection; owned by the cache
const GfxShader *gfx_shader_get(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                                const uint8_t *c1, const GfxShaderAnswers *a);   // blocks; NULL on failure (counted)
// As gfx_shader_get with c1 inside the code, but a shader in neither cache is queued to a background translation and
// NULL returned with *pending = 1 until it is ready. A failed translation stays failed (*pending = 0).
const GfxShader *gfx_shader_get_async(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                                      const GfxShaderAnswers *a, int *pending);
// As gfx_shader_get_async, but a shader queued for translation is moved to the front of the queue and waited for,
// wait_ms at most (UINT32_MAX: until it is done). NULL when the translation failed, or with *pending = 1 when it was
// still running at the end of the wait: it stays at the front of the queue.
const GfxShader *gfx_shader_await(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                                  const GfxShaderAnswers *a, uint32_t wait_ms, int *pending);
uint64_t gfx_shader_async_done(void);                      // translations finished by the background worker
void gfx_shader_counts(uint64_t *keys, uint64_t *translated);   // distinct keys in memory, translations done (NX2NAVI_TRACE=perf)
void gfx_shader_disk_counts(uint64_t *sync, uint64_t *sync_ns, uint64_t *bg);   // disk cache reads on the render
                                                     // thread (count, ns) and by the loaders (gfx_shader.c load_budget)
// The shader caches' host memory for the memory report (vk_memory.c system_log), at report time, in bytes: the memory
// cache of translated shaders (shaders entries), the use masks, the cache pack's index, the play records' sets and
// pending appends. The memory cache's SPIR-V (gfx_shader.c "SPIR-V at rest"): spv_held bytes held, plain copies and
// compressed frames together, against spv_full uncompressed; spv_packed shaders held compressed alone; compress 1 on,
// -1 off, 0 no shader yet. The decodes readers' pins made, and those on the drawing threads (count, time, longest).
typedef struct {
    uint64_t shaders, cache, use, pack, records;
    uint64_t spv_held, spv_full, spv_packed;
    uint64_t decodes, decode_ns, draw_decodes, draw_decode_ns, draw_decode_max_ns;
    int compress;
} GfxShaderRam;
void gfx_shader_ram(GfxShaderRam *r);
// Translator answers for this device (vk_caps.c): shaderFloat64, storage offset rounding, subgroup size per vs/fs/cs.
void gfx_shader_set_host(int float64, uint32_t sbuf_align, const uint32_t subgroup[3], uint32_t gather_bits, int zero_locals,
                         int bda);
const uint8_t *gfx_shader_key(const GfxShader *s);          // the 32-byte cache key (stable while the translator is)
const uint8_t *gfx_shader_input_id(const GfxShader *s);     // the 32-byte input id (stable across translators)
// Where the translation in use came from: 0 the shader cache, under the current translator; 1 the shader cache, a
// previous translator's (gfx_shader.c "Carry"); 2 translated in this process, so nothing was prepared from it.
int gfx_shader_origin(const GfxShader *s);
// By input id from memory or disk; xlate 1 also translates it from its stored inputs. NULL if absent.
const GfxShader *gfx_shader_load_input(const uint8_t iid[32], int xlate);
// Boot prewarm: out[i] = gfx_shader_load_input(iids[i], 1) on `threads` threads with up to `procs` translators, then
// every other stored input not cached under this translator is translated (one a previous translator's translation
// serves is left to gfx_shader_carry_start's pass). Logs progress and the total.
void gfx_shader_prewarm(const uint8_t (*iids)[32], size_t n, const GfxShader **out, int threads, int procs);
// After the boot prewarm of a game run (boot.c): when the shader cache holds translations of a previous translator
// (NX2NAVI_SHADER_CARRY), starts the background pass that translates them again under the current one for the next
// start and then drops the old ones (gfx_shader.c, "Carry").
void gfx_shader_carry_start(void);
// How far the running (or last) gfx_shader_prewarm is: shaders done of total, and how many of them it translated (the
// rest were cached). Any thread; the numbers lag the workers by at most 100 ms.
void gfx_shader_prewarm_progress(int64_t *done, int64_t *total, int64_t *translated);
// A black frame with the boot screen (ui/menu.cpp menu_boot_frame) over it: boot.c's wait for the prewarm, on the
// boot thread before the guest runs (nothing else presents then).
void gfx_present_boot(void);
// Answers met in play: from this call on, every (program, answers) a gfx_shader_get* call resolves is appended
// to shader-answers.bin, and its program to shader-programs.bin, in the directory of `keys_path` (pipeline-keys.bin).
void gfx_shader_record_at(const char *keys_path);
void gfx_shader_record_flush(int force);                   // appends pending records; without force at most every 2 s
// Translates every recorded (program, answers) on `threads` threads with up to `procs` translators (NX2NAVI_TEST_PREPARE_SHADERS).
void gfx_shader_prepare_recorded(int threads, int procs);
// nx2navi.exe --prepare-shaders: translates <dir>/seed-answers.bin + seed-programs.bin (tools/prepare_shaders.py) and
// writes <dir>/shader-prepared.bin (stage, priority, input id per variant). 0 on success.
int gfx_shader_prepare_seed(const char *dir, int threads, int procs);
// gfx_shader_guess.c: the answers of a prepared default variant, read from its first translation s: vertex inputs the
// shader reads as integers become uint, a compute shader's textures and images take the types its translation
// declares. 1 when *out differs from *a (translate again with *out).
int gfx_shader_guess_refine(const GfxShader *s, int stage, const GfxShaderAnswers *a, GfxShaderAnswers *out);
typedef struct { uint8_t iid[32]; uint8_t stage, prio; } GfxWarmEnt;   // prio 0: met in play; 1..3: prepared
size_t gfx_shader_warm_list(GfxWarmEnt **out);              // malloc'd; recorded first, then prepared; needs recording on
// Disk-cache load the caller owns (*owned 1: release it), or the memory cache's entry (*owned 0). NULL if not cached.
const GfxShader *gfx_shader_load_private(const uint8_t iid[32], int *owned);
void gfx_shader_release_private(const GfxShader *s);
// A shader's SPIR-V may be held compressed while nobody reads it (gfx_shader.c "SPIR-V at rest"). A reader pins it:
// gfx_shader_spirv_pin returns the plain SPIR-V (decoded first if need be), valid until the matching
// gfx_shader_spirv_unpin; every pin needs its unpin, and the pointer is not used after it. Any thread.
const uint32_t *gfx_shader_spirv_pin(const GfxShader *s, uint64_t *words);
void gfx_shader_spirv_unpin(const GfxShader *s);
uint64_t gfx_shader_words(const GfxShader *s);              // the SPIR-V's length in words, without reading it
// FNV-1a (64-bit, over the bytes) of the SPIR-V: worked out once per shader and kept, so it decodes nothing on a
// compressed shader (NX2NAVI_SKIP_SHADERS, vk_dump.c and vk_dlaa.c name shaders by it).
uint64_t gfx_shader_spirv_fnv(const GfxShader *s);
// The values of the module's OpDecorate Binding instructions, in module order, in *b (kept with the shader).
uint32_t gfx_shader_bindings(const GfxShader *s, const uint32_t **b);
const char *gfx_shader_res_json(const GfxShader *s);        // the translator's .res.json text
int gfx_shader_write_code(const GfxShader *s, const char *path);   // the guest program, from the stored inputs
// A shader of the runtime's own (SPIR-V and a .res.json text as the translator writes them), outside the cache and
// never freed; its key is the sha256 of both. NULL when the SPIR-V is malformed.
const GfxShader *gfx_shader_own(const uint32_t *spirv, uint64_t words, const char *res_json);
// vk_hdr.c: with NX2NAVI_HDR=1, splices the HDR tonemap into a fragment shader's SPIR-V (malloc'd, *words long) when it
// is the game's tonemap, and unclamps the game's FSR pass; gfx_shader.c calls it for every fragment shader entering the cache.
void vk_hdr_shader_swap(uint32_t **spirv, uint64_t *words);
uint64_t gfx_shader_failures(void);                         // failed translations; feeds GfxStats.shader_failures
// Reflection: parsed once per shader from the translator's .res.json and the SPIR-V before it enters the cache.
enum { GFX_RES_UBO, GFX_RES_SSBO, GFX_RES_TEX, GFX_RES_IMG, GFX_RES_KINDS };
typedef struct {
    uint16_t set, binding;      // as the SPIR-V decorates them: set 0, bindings from 1 per shader (0: support buffer)
    int16_t slot;               // UBO: c[slot]; SSBO: storage buffer index; TEX: c2 combined slot (handle at 0x20 + 8 slot)
                                // or, separate, texture slot (0x168 + 8 slot); IMG: image slot (0x120 + 8 slot); -1: other
    uint16_t type;              // TEX/IMG: the translator's SamplerType: 1 1D, 2 buffer, 3 2D, 4 3D, 5 cube | 0x100 array,
                                // 0x200 multisample, 0x400 shadow
    uint8_t format;             // IMG: declared format as a GfxShaderAnswers.img_format code, 0 unknown
    uint8_t flags;              // SSBO: 1 written; TEX/IMG: the translator's TextureUsageFlags (4 image store, 8 coherent)
    int8_t cbuf_slot;           // TEX/IMG: constant buffer of the handle (-1: bound texture buffer); SSBO: of its address
    uint16_t handle;            // TEX/IMG: raw handle (word offset; separate: texture | sampler << 14); SSBO: address offset
    uint16_t count;             // descriptors (array length)
    uint8_t separate;           // TEX: texture and sampler handles are separate
    uint8_t pool;               // TEX: 1 the texture pool, 2 the sampler pool, indexed by bindless handles (set 2)
    uint16_t ptr;               // SSBO: 0, or 1 + the byte offset of this buffer's address inside the storage buffer that
                                // cbuf_slot and handle name (a pointer table; the translator's patch 0022, res.json "ptr")
    uint8_t exact;              // TEX: the module fetches its texels by position or uses its size or level count, so it
                                // needs every level of the texture as the game made it (gfx_shader.c reflect_exact)
} GfxShaderRes;
typedef struct { uint8_t type, comps, packed; } GfxShaderIo;   // GFX_TYPE_*, components, 1 with a Component decoration
int gfx_shader_stage(const GfxShader *s);                   // GFX_STAGE_VS, GFX_STAGE_FS or GFX_STAGE_CS
int gfx_shader_resources(const GfxShader *s, int kind, const GfxShaderRes **res);   // count of GFX_RES_* kind; array in *res
uint32_t gfx_shader_exact_seen(void);   // shaders loaded so far with a GfxShaderRes.exact texture (the texture census line)
uint32_t gfx_shader_pool_short_seen(void);   // shaders in memory whose texture pool array has the earlier length, 32768
                                             // (gfx_shader.c pool_short_note; vk_texture_register's warning)
uint32_t gfx_shader_slot_mask(const GfxShader *s);                    // texture/image slots whose type its translation read
uint32_t gfx_shader_inputs(const GfxShader *s, GfxShaderIo io[32]);    // Input Locations (VS attributes, FS varyings) as a mask
uint32_t gfx_shader_outputs(const GfxShader *s, GfxShaderIo io[32]);
uint32_t gfx_shader_omap(const GfxShader *s);   // FS: colour components it writes, 4 bits per target (SPH omap); 0 unknown
uint32_t gfx_shader_mvprev_banks(const GfxShader *s);   // a last-frame variant: the banks (bit n: cn) its position reads   // Output Locations (VS varyings, FS colour targets)
// VS: the matrix palette its skinning reads (gfx_shader.c reflect_palette, which states the rule). 0: none. Else the
// bytes of one bone matrix (48 or 64), *slot the uniform bank c[slot] it is read from, or with *storage 1 the storage
// buffer index, and *joints the input Locations whose integer values pick the matrices.
int gfx_shader_palette(const GfxShader *s, int *slot, int *storage, uint32_t *joints);
void gfx_shader_local_size(const GfxShader *s, uint32_t local[3]);     // CS: the LocalSize execution mode; 1, 1, 1 otherwise

// ---- back end (gfx_null.c, vk_*.c). Called by the NVN front end (nvn_api_*.c, nvn_ext.c) on guest threads.
typedef struct {
    const char *name;
    int  (*pool_init)(pool_t *p);                          // 0 fails MemoryPoolInitialize
    void (*pool_fini)(pool_t *p);
    int  (*pool_map_virtual)(pool_t *v, int n, const void *requests);   // NVNmappingRequest[n], 0x28 bytes each
    void (*texture_register)(int id, const tex_t *t, const view_t *v);
    void (*sampler_register)(int id, const samp_par *s);
    void (*texture_release)(const tex_t *t);
    void (*program_release)(const prog_t *p);
    uint64_t (*submit)(const uint64_t *handles, int n);    // replays; returns this submission's fence value
                                                           // (gfx_submit_queue names the game's queue)
    int  (*fence_wait)(uint64_t value, uint64_t timeout_ns);   // 1 reached; caller has given its core up
    // As fence_wait, but only until the GPU has executed the submission's own commands: its write-backs to guest
    // memory and its fence's side effects may follow (vk_sync.c exec_tl). The same as fence_wait where there is no split.
    int  (*exec_wait)(uint64_t value, uint64_t timeout_ns);
    // The game's own wait for a fence (nvnSyncWait): fence_wait, or where the back end knows the rest of the value's
    // GPU work reads nothing the game can change and hands it nothing, until its results are there (vk_sync.c
    // vk_sync_game_wait).
    int  (*game_wait)(uint64_t value, uint64_t timeout_ns);
    uint64_t (*fence_done)(void);                          // highest value whose side effects are written
    // Queues the present and returns at once; the host present goes out at at_ns (kern_now_ns clock), at once when that
    // has passed or at_ns is 0.
    void (*present)(const tex_t *t, const int crop[4], uint64_t at_ns);
    // The game starts a frame (nvn_api_obj.c nvnWindowAcquireTexture), after the image's own waits (the pacer's hold
    // and the GPU's): the back end may hold it longer (vk_dlssg.c vk_dlssg_frame_start, Reflex's sleep for DLSS Frame
    // Generation). NULL: nothing to do.
    void (*frame_start)(void);
    // The game is about to read its pad (ov_input.c npad_states), several times a frame: where the pad read is the
    // first thing a frame does, the back end holds the frame's start here, before the input is sampled (vk_lowlat.c
    // vk_lowlat_frame_start). NULL: nothing to do.
    void (*input_read)(void);
    // nvnQueuePresentTexture, with the pacer's period in ns (0: it holds nothing; also during a movie, whose slots only
    // the pacer keeps): the period the pacer still holds, for both the present's slot and the window image. period_ns
    // while nothing else holds the limit; less when the back end holds the next frame's start to the period itself:
    // the game's ceiling (frame_rate.c frame_step_ceiling_ns) where Reflex holds it with DLSS Frame Generation
    // (vk_dlssg.c vk_dlssg_pace), 0 where a driver's low-latency sleep does (vk_lowlat.c vk_lowlat_pace). NULL: never.
    uint64_t (*frame_paced)(uint64_t period_ns);
    void (*finish)(void);
    void (*read_texels)(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row, uint64_t img);
    void (*write_texels)(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, uint64_t row, uint64_t img);
    void (*guest_written)(uint64_t addr, uint64_t size);   // host wrote pool storage (3.2)
    void (*guest_read)(uint64_t addr, uint64_t size);      // the CPU reads pool storage: GPU-written texels go there first
    // The CPU waited for fence: small render targets written by work up to it go to their storage (vk_texture.c
    // vk_texture_write_back_small).
    void (*cpu_synced)(uint64_t fence);
} GfxBackend;
extern const GfxBackend gfx_null_backend, gfx_vulkan_backend;
const GfxBackend *gfx_backend(void);                       // NX2NAVI_GPU=null|vulkan (default vulkan)

// ---- NVN front end (nvn_procs.c)
void nvn_procs_register(void);                             // hostfn_register every proc; before hostfn_seal (ov_gfx.c)
uint64_t nvn_proc_address(const char *name);               // synthetic host address of a proc, 0 for an unknown name

// ---- statistics and frames (nvn_api_*.c, vk_*.c; boot.c prints them)
typedef struct { uint64_t frames, nvn_calls, nvn_distinct, unimplemented, validation_errors, shader_failures,
                 draws_skipped; } GfxStats;
GfxStats gfx_stats(void);
void rt_frame_presented(const tex_t *t, uint64_t n);       // boot.c: --frames N ends the run after the Nth present
                                                           // (saves --shot first through read_texels)

// Movie overlay (ov_movie2.c -> gfx_present.c): the latest decoded movie frame, RGBA8, shown over the game image
// while frames keep coming (the game draws movies with its own shaders, which do not translate yet).
void gfx_movie_frame(const uint8_t *rgba, int w, int h);
void gfx_movie_frame_own(uint8_t *rgba, int w, int h);   // the same, taking the malloc'd buffer (freed later)
void gfx_movie_stop(void);

// The game's queue (NVNqueue address) of the submission this thread is handing to GfxBackend.submit, 0 when none
// (probes). Set by nvnQueueSubmitCommands around the call; vk_exec.c orders queues by their events with it.
extern _Thread_local uintptr_t gfx_submit_queue;
