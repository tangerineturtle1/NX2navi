// Graphics and compute pipelines and descriptor layouts of the Vulkan replay. The op-stream replay (vk_exec.c)
// collects NVN state into a VkPipeState and asks for the pipeline of each draw; this file maps that state to Vulkan,
// numbers the descriptor bindings and keeps the caches.
//
// Binding model. The translator (tools/xlate/Program.cs, a SPIR-V back end) decorates every resource of a
// shader with DescriptorSet 0 and numbers the bindings of each shader from 1 in the order it meets them (Accessor
// Create*Binding); binding 0 is its support buffer (ResourceManager: SupportBuffer.Binding, a std140 uniform
// buffer every shader declares). The numbering is per shader, so the vertex and fragment stages of one program collide.
// The scheme here:
//   - one descriptor set, set 0, per pipeline; no push constants (the translator's SPIR-V uses none);
//   - binding 0: the support buffer, one uniform buffer shared by all stages of the pipeline;
//   - vertex stage: its own bindings 1..n unchanged; fragment stage: its bindings b >= 1 move to b + n (the SPIR-V
//     Binding decorations are rewritten when the module is created); compute: its own bindings;
//   - descriptor types from the reflection: constant buffers UNIFORM_BUFFER, storage buffers STORAGE_BUFFER, textures
//     COMBINED_IMAGE_SAMPLER (UNIFORM_TEXEL_BUFFER for buffer textures, SAMPLED_IMAGE for separate ones), images
//     STORAGE_IMAGE (STORAGE_TEXEL_BUFFER for buffer images), array lengths as descriptor counts;
//   - set 0 is a push-descriptor set (VK_KHR_push_descriptor) when its descriptors fit maxPushDescriptors (32 on the RX
//     9070; a pipeline layout may hold one push set only, which rules out a set per stage); larger ones (a vertex and a
//     fragment stage near the census maxima of 15 and 27) are ordinary sets the replay allocates (VkPipeInfo.push 0).
// A fixed layout per stage combination (a binding per constant buffer, storage buffer, texture
// and image slot per stage) would hold about 150 descriptors and could never be pushed, so layouts are made per distinct
// binding list and shared by every pipeline with that list; VkPipeInfo.bindings tells the replay which NVN slot feeds
// each binding.
//
// Fragment inputs the vertex stage does not write (stages are translated separately) get
// outputs added to the vertex module, as Vulkan rejects such pairs, initialised to (0, 0, 0, 1) as Maxwell reads an
// unwritten output (docs/maxwell.md 1.2). The unwritten components of an output the vertex stage does write
// start at (0, 0, 0, 1) in the translator (tools/xlate/patches/0006).
//
// Pipeline key. Everything Vulkan 1.3 can take as dynamic state is dynamic: viewports and scissors with count, depth
// bias, blend constants, stencil masks and references, cull mode, front face, topology (within its class), depth and
// stencil tests and ops, depth bias enable, rasterizer discard; vertex input (VK_EXT_vertex_input_dynamic_state, else
// binding strides only); and, where VK_EXT_extended_dynamic_state3 offers them, depth clamp, polygon mode, blend enables,
// equations, write masks and alpha to coverage. The key holds the rest: shaders, target formats, sample count, topology
// class, depth clip mode, logic op and what is not dynamic on the GPU. The VkPipelineCache persists to
// pipeline-cache.bin next to nx2navi.exe (NX2NAVI_PIPELINE_CACHE overrides the path).
//
// The vertex format table has a 3-component fallback. The pipeline key holds the program, target formats, blend and
// write masks, vertex layout and sample count. The binding conventions are those of the shader translator.
// Contains code Copyright (c) 2026 Peiikai, Copyright (c) 2026 jhaizhou (MIT; see THIRD_PARTY_NOTICES.md).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "mesh_emu_spirv.h"
#include "gfx.h"
#include "host.h"
#include "boot_warm.h"
#include "mem_need.h"
#include "nvn_hmap.h"
#include "platform/platform.h"
#include "prp_file.h"
#include "spirv_water.h"
#include "stutter_map.h"
#include "tex.h"
#include "vk_build_gate.h"
#include "vk_cache_file.h"
#include "vk_exec.h"
#include "vk_save_retry.h"
#include "vk_warm_hint.h"

#define LOG_ONCE(...) do { static volatile int32_t once_; if (!plat_xchg32(&once_, 1)) rt_log(__VA_ARGS__); } while (0)

enum { SPV_OP_ENTRY_POINT = 15, SPV_OP_TYPE_INT = 21, SPV_OP_TYPE_FLOAT = 22, SPV_OP_TYPE_VECTOR = 23,
       SPV_OP_TYPE_POINTER = 32, SPV_OP_CONSTANT = 43, SPV_OP_CONSTANT_COMPOSITE = 44,
       SPV_OP_CONSTANT_NULL = 46, SPV_OP_FUNCTION = 54, SPV_OP_VARIABLE = 59,
       SPV_OP_DECORATE = 71, SPV_DEC_BUILTIN = 11, SPV_DEC_LOCATION = 30, SPV_DEC_INDEX = 32, SPV_DEC_BINDING = 33, SPV_DEC_DESCRIPTOR_SET = 34,
       SPV_SC_OUTPUT = 3 };

#define MAX_BINDINGS 128
#define PIPE_BUCKETS 4096
#define LAYOUT_BUCKETS 256

// ---- device capabilities and the pipeline cache (set up on first use, under P.lock)

enum { NP_16 = 1, NP_32 = 2, NP_64 = 4 };   // float widths in P.nan_preserve and spirv_nan_preserve

static struct {
    PlatMutex lock;
    volatile int32_t ready;
    VkDevice device;
    VkPipelineCache cache;
    uint32_t max_push;
    int indep;                         // stage-independent layouts (vk_caps.gpl): vertex set 0, fragment set 1
    int push_stage;                    // GFX_STAGE_* whose set is pushed with independent layouts
    int gpl;                           // draws whose pipeline is building get a fast-linked one (vk_caps.gpl)
    int mesh_lib;                      // mesh pipelines are linked from stage libraries too (mesh_lib_init, mesh_libs)
    struct { uint64_t links, fb_links, wlinks, flinks, dlinks, pr_added, lib_slow[4], ready_n[6], ready_nofast, ready_mesh, ready_recorded, ready_fresh, ready_logged, lib_logged, hints, hint_libs, requeued, link_fail, defer, lib_n[4], lib_fail, swaps, slow, pre_used[4]; double pr_added_ms, lib_slow_ms[4], ready_ms, ready_max, link_ms, link_max, lib_ms[4], lib_max[4], dlink_ms, dlink_max;
             // mesh pipelines' share of the counts above: m_links fast links and their link time; of the ready_mesh
             // pipelines a draw missed, m_ready_fast became usable through a fast link, m_ready_ms after the miss in
             // all; m_lib_*: [0] their task and mesh libraries, [1] their fragment shader libraries (lib_build)
             uint64_t m_links, m_ready_fast, m_lib_n[2]; double m_link_ms, m_link_max, m_ready_ms, m_ready_max, m_lib_ms[2], m_lib_max[2]; } g;
    int vs_outs_all;                   // NX2NAVI_VS_OUTS: 0 union of paired fragment inputs, 1 (all) all 32 output locations
    int dyn_vi, dyn_clamp, dyn_poly, dyn_blend_en, dyn_blend_eq, dyn_wmask, dyn_a2c;
    int f_logic_op, f_depth_clamp, f_fill_non_solid, f_dual_src, f_full_subgroups;
    int wide_lines;                      // the device draws lines of other widths than 1 (vk_pipeline_line_width)
    float line_w[2], line_gran;          // its range of widths and their step
    int cache_control;                 // pipelineCreationCacheControl: a create can ask for the pipeline caches only
    int feedback;                      // NX2NAVI_PIPE_FEEDBACK: 1 count pipeline cache hits, 2 also log each compute and graphics build
    uint64_t cache_loaded;             // bytes of pipeline-cache.bin the driver took at boot
    VkPipelineCache play;              // what this run compiles (pipeline-cache-play.bin); VK_NULL_HANDLE: all goes into cache
    uint64_t play_loaded;              // bytes of pipeline-cache-play.bin the driver took at boot
    uint64_t saved[2];                 // the data size of cache [0] and play [1] that their files hold; 0: not known
    volatile uint64_t held[2];         // the data size of cache [0] and play [1] when last asked (cache_save_one,
                                       // cache_sizes_ask); 0: not asked yet (the memory report takes the loaded size)
    int fold;                          // the play file's content was merged into cache: saving cache removes the play file
    volatile uint64_t play_made;       // pipelines this run compiled into play
    int pv_last;                       // provokingVertexLast enabled: NVN's default is the last vertex
    VkShaderStageFlags sg32_stages;    // stages that accept requiredSubgroupSize = 32 (0: not available)
    int reconverge;                    // modules with subgroup ops get MaximallyReconvergesKHR (spirv_reconverge)
    uint32_t nan_preserve;             // float widths that get SignedZeroInfNanPreserve (spirv_nan_preserve): NP_16|NP_32|NP_64
    int nan_bits;                      // NaN tests as integer bit tests (spirv_nan_bits)
    volatile uint32_t nan_bits_tests, nan_bits_selects, nan_bits_modules;   // rewritten so far (the first log line)
    VkDynamicState dyn[32];
    uint32_t n_dyn;
    PFN_vkCmdPushDescriptorSetKHR push;
    int tpl;                           // descriptor update templates on (make_tpl)
    PFN_vkCreateDescriptorUpdateTemplate tpl_create;
    PFN_vkDestroyDescriptorUpdateTemplate tpl_destroy;
    PFN_vkUpdateDescriptorSetWithTemplate tpl_update;
    PFN_vkCmdPushDescriptorSetWithTemplateKHR tpl_push;
    PFN_vkCmdSetVertexInputEXT set_vi;
    PFN_vkCmdSetDepthClampEnableEXT set_clamp;
    PFN_vkCmdSetPolygonModeEXT set_poly;
    PFN_vkCmdSetColorBlendEnableEXT set_blend_en;
    PFN_vkCmdSetColorBlendEquationEXT set_blend_eq;
    PFN_vkCmdSetColorWriteMaskEXT set_wmask;
    PFN_vkCmdSetAlphaToCoverageEnableEXT set_a2c;
    char cache_path[PLAT_PATH_MAX * 2];
    char play_path[PLAT_PATH_MAX * 2 + 8];   // pipeline-cache-play.bin beside cache_path
    VkPipeStats stats;
} P = {.lock = PLAT_MUTEX_INIT};

typedef struct Layout {
    struct Layout *next;
    uint64_t hash;
    VkDescriptorSetLayout set;
    VkPipelineLayout layout;
    int push;
    uint32_t n;
    VkDescriptorSetLayout set1;        // independent layouts: the fragment set (set 1); VK_NULL_HANDLE otherwise
    int push1;                         // set 1 is a push-descriptor set
    uint32_t n0;                       // independent layouts: b[0..n0) are set 0, the rest set 1
    VkPipelineLayout half[2];          // GPL libraries: {set 0, null}, {null, set 1} (made on first use)
    VkDescriptorUpdateTemplate tpl[2]; // per set (make_tpl); VK_NULL_HANDLE: written with write structs
    uint8_t emu;                       // NX2NAVI_MESH_EMULATION: a mesh layout whose task and mesh stages run as compute
    VkDescriptorUpdateTemplate tpl_c;  // emu: set 0's template for the compute bind point
    VkDescriptorSetLayoutBinding b[];
} Layout;

typedef struct Pipe {
    struct Pipe *next;
    uint64_t hash;
    VkPipeInfo info;
    VkPipeBinding *bindings;
    int building;                      // a background job owns info.pipeline (under P.lock)
    struct Job *job;                   // that job while building
    const struct Layout *lay;          // graphics: the layout it was entered with
    VkPipeline fast;                   // GPL: the fast-linked pipeline, until the optimised one replaces it (fast_retire)
    uint8_t fast_tried;
    uint8_t link_queued;               // a waiting draw queued its fast link to the workers (LibJob p), not yet run
    uint8_t used;                      // compute: a dispatch of this run asked for it (vk_pipeline_compute)
    uint8_t pre;                       // compute: built and kept by the boot prebuild (lb_compute) or bg_main
    uint8_t optimised;                 // graphics: its pipeline is the whole build that replaced a fast link (fast_retire)
    uint32_t used_min;                 // pipe_minute when a draw or dispatch last looked it up; 0 none in this run
    uint64_t t_miss;                   // graphics: when a draw first missed it (ns), 0 once it is usable (ready_note)
    uint64_t rel_fence;                // taken out by pressure_release: destroyed once this value is done (release_drain)
    size_t key_size;
    uint8_t key[];
} Pipe;

// The memory report's clock for pipelines not used lately (vk_pipeline_mem): wall minutes, plus one so that 0 stays
// "never". The autosave thread moves it every half second; a lookup stores it into its Pipe under P.lock, which the
// lookup holds anyway, so the per-draw cost is one compare. A pipeline that only the replay's memo of the previous draw
// served (vk_exec_draw.c, PM) keeps an older minute: at most the one pipeline each replay context remembers.
static volatile uint32_t pipe_minute;
static uint32_t minute_now(void) { return (uint32_t)(plat_now_ns() / 60000000000ull) + 1; }
static inline void pipe_used(Pipe *p) {
    uint32_t m = __atomic_load_n(&pipe_minute, __ATOMIC_RELAXED);
    if (p->used_min != m) p->used_min = m;
}

static Layout *layouts[LAYOUT_BUCKETS];
static Pipe *pipes[PIPE_BUCKETS];

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}
#define FNV0 0xcbf29ce484222325ull
static __thread uint64_t mm_hash;   // FNV-1a of the module make_module last created on this thread (PIPE_FEEDBACK 2)

// Hash of a pipeline table key, 8 bytes per step: a draw hashes its whole GKey, and byte-wise FNV over it cost more
// than the lookup. Only the in-memory table uses it; the key file keeps fnv.
static uint64_t key_hash(const void *k, size_t n) { return nvn_hm_hash(k, (uint32_t)n); }

// The cache file lives next to nx2navi.exe, so every build dir (the main build and each private fork) has its own:
// forks that shared build\v121's cache broke each other's runs (2026-10-22).
static void cache_path_init(void) {
    const char *cfg_path = cfg_get("NX2NAVI_PIPELINE_CACHE");
    if (cfg_path && *cfg_path && strlen(cfg_path) < sizeof P.cache_path) {
        strcpy(P.cache_path, cfg_path);
        return;
    }
    P.cache_path[0] = 0;
    char exe[PLAT_PATH_MAX * 2];
    int n = plat_exe_path(exe, sizeof exe);
    if (!n || n >= sizeof exe) return;
    char *cut = plat_last_sep(exe);
    if (!cut) return;
    *cut = 0;
    snprintf(P.cache_path, sizeof P.cache_path, "%s/pipeline-cache.bin", exe);
}

// ---- pipeline binaries (VK_KHR_pipeline_binary; NX2NAVI_PIPE_BINARIES, default 0 while it is being tested; 1 on)
// pipeline-cache.bin does not carry compiled pipelines from one executable to another on NVIDIA 621.01: two
// byte-identical exes under different names, every compute module hash-identical, and the second compiled 317 of the
// 323 compute pipelines the first had read from the file (2026-11-09). A pipeline binary made from one exe's pipeline
// creates the same pipeline in another in 0.5 ms instead of 26 ms. So where the driver has the extension, every
// pipeline, stage library and compute pipeline is created through pipe_create: its driver pipeline key
// (vkGetPipelineKeyKHR, computed from the create info) is looked up in pipeline-binaries.bin next to nx2navi.exe and
// the pipeline is made from the stored binaries; a key not there is compiled with its data captured, and its binaries
// are appended to the file (autosave, every 2 s). pipeline-cache.bin is then neither read nor written: the driver's
// own disk cache serves an unchanged exe, and the binaries serve a renamed or updated one.
// Each binary is stored once under its own binary key: on NVIDIA 621.01 nearly every graphics pipeline has, besides
// its own binaries of 20-200 KB, one 2 MiB binary that is byte-identical across pipelines (2,506 copies in a 4.7 GB
// first-version file).
// File: header {"NX2NPBIN", version, the driver's global key size and bytes} (another global key: another driver or
// GPU, so the file starts empty); then records {u32 bytes after this field, u32 type, u32 key size, 32 key bytes,
// payload}: type PB_BLOB, a binary under its binary key, payload its data; type PB_PIPE, a pipeline under its pipeline
// key, payload {u32 count, count x {u32 key size, 32 key bytes}} naming its binaries. Only an index of the records
// stays in memory; a pipeline's records are read from the file when it is created. NX2NAVI_PIPE_BINARIES_MAX_MB
// (default 1024, 0 no limit): a larger file starts empty.
#define PB_VERSION 2
// 64-bit file offsets: long is 32 bits on Windows, and a file over 2 GB read as cut short.
#ifdef _WIN32
#define pb_seek _fseeki64
#define pb_tell _ftelli64
#else
#define pb_seek fseeko
#define pb_tell ftello
#endif
#define PB_MAX_BIN 8
#define PB_KEY VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR
enum { PB_BLOB = 1, PB_PIPE = 2 };
#define PB_HEAD (4 + 4 + PB_KEY)       // a record's bytes after its size field and before its payload
typedef struct { char magic[8]; uint32_t version, gkey_size; uint8_t gkey[PB_KEY]; } PbHeader;
typedef struct { uint64_t h, off; uint32_t size; } PbSlot;   // off: file offset of the record's size field
static struct {
    int on;
    PFN_vkCreatePipelineBinariesKHR create;
    PFN_vkDestroyPipelineBinaryKHR destroy;
    PFN_vkGetPipelineKeyKHR key;
    PFN_vkGetPipelineBinaryDataKHR data;
    PFN_vkReleaseCapturedPipelineDataKHR release;
    char path[PLAT_PATH_MAX * 2 + 32];
    PlatMutex lock;                    // the index, the pending records and rf
    FILE *rf;                          // reads records
    PbSlot *slot;
    uint32_t cap, n, pipes, blobs;
    uint8_t *pend;                     // records not written yet; they follow the file's end in this order
    size_t pend_n, pend_cap;
    uint64_t file_size;                // bytes on disk
    int no_capture[3];                 // kind (pipe_create) whose create refused the capture flag
    uint64_t from_file, load_fail, compiled, captured, capture_fail, blob_new, blob_shared;
    double from_ms, compile_ms;
} PB = {.lock = PLAT_MUTEX_INIT};
static __thread int pb_last;           // 1: the last pipe_create on this thread made its pipeline from binaries
static __thread int cc_hit;            // compute_create's last pipeline on this thread: 1 the pipeline cache held it
                                       // (P.cache: read from pipeline-cache.bin, or built into it earlier in this
                                       // run) or it was made from pipeline binaries, 0 not, -1 the driver did not say

static uint64_t pb_hash(uint32_t type, const uint8_t *key, uint32_t n) {
    uint64_t h = fnv(fnv(FNV0, &type, 4), key, n);
    return h ? h : 1;
}

static PbSlot *pb_find(uint64_t h) {   // caller holds PB.lock
    if (!PB.cap) return NULL;
    for (uint32_t j = (uint32_t)h & (PB.cap - 1);; j = (j + 1) & (PB.cap - 1)) {
        if (!PB.slot[j].h) return NULL;
        if (PB.slot[j].h == h) return &PB.slot[j];
    }
}

// Enters a record (a hash match is taken as the key: 64 bits). 0 when one with the key is there already or out of
// memory. Caller holds PB.lock.
static int pb_insert(uint64_t h, uint64_t off, uint32_t size) {
    if ((PB.n + 1) * 2 > PB.cap) {
        uint32_t cap = PB.cap ? PB.cap * 2 : 4096;
        PbSlot *t = calloc(cap, sizeof *t);
        if (!t) return 0;
        for (uint32_t i = 0; i < PB.cap; i++) {
            if (!PB.slot[i].h) continue;
            uint32_t j = (uint32_t)PB.slot[i].h & (cap - 1);
            while (t[j].h) j = (j + 1) & (cap - 1);
            t[j] = PB.slot[i];
        }
        free(PB.slot);
        PB.slot = t, PB.cap = cap;
    }
    if (pb_find(h)) return 0;
    uint32_t j = (uint32_t)h & (PB.cap - 1);
    while (PB.slot[j].h) j = (j + 1) & (PB.cap - 1);
    PB.slot[j] = (PbSlot){h, off, size};
    PB.n++;
    return 1;
}

static void pb_rewrite(const PbHeader *hd) {   // an empty file with this driver's header
    FILE *f = fopen(PB.path, "wb");
    if (f) fwrite(hd, sizeof *hd, 1, f), fclose(f);
    PB.file_size = f ? sizeof *hd : 0;
}

// Indexes the records of f (len bytes) after its header: per record its type, key and offset. Returns the offset
// after the last whole record, which is len unless the file ends inside a record (an append the process did not
// finish) or holds a malformed one.
static uint64_t pb_index(FILE *f, uint64_t len) {
    uint64_t good = sizeof(PbHeader);
    for (;;) {
        uint32_t head[3];
        uint8_t key[PB_KEY];
        if (pb_seek(f, (int64_t)good, SEEK_SET) || fread(head, sizeof head, 1, f) != 1 || head[2] > PB_KEY ||
            head[0] < PB_HEAD || (head[1] != PB_BLOB && head[1] != PB_PIPE) || fread(key, PB_KEY, 1, f) != 1 ||
            good + 4 + head[0] > len)
            break;
        if (pb_insert(pb_hash(head[1], key, head[2]), good, head[0])) {
            if (head[1] == PB_PIPE) PB.pipes++;
            else PB.blobs++;
        }
        good += 4 + (uint64_t)head[0];
    }
    return good;
}

// The length of the file at path (len bytes) to go on with after pb_index found its whole records ending at good: len
// when they fill it. When it ends inside a record (the process ended during an append, or a disk filled) or holds a
// malformed one, the records from there on are dropped and the file is cut back to good, so that appends go on after
// the whole records before it. 0 when it cannot be cut, and the file starts empty.
static uint64_t pb_keep(const char *path, uint64_t good, uint64_t len) {
    if (good == len) return len;
    if (!plat_file_truncate(path, good)) return 0;
    rt_log("vk_pipeline: pipeline binaries: %s ends inside a record or holds a malformed one at byte %llu; %llu bytes "
           "from there dropped", path, (unsigned long long)good, (unsigned long long)(len - good));
    return good;
}

// Opens pipeline-binaries.bin and indexes its records (cache_load, before P.cache).
static void pb_init(VkCtx *c) {
    if (!c->has_pipeline_binary || !P.cache_path[0]) return;
#define PBPROC(field, name) (PB.field = (void *)vk_dpa(c->device, name))
    if (!PBPROC(create, "vkCreatePipelineBinariesKHR") || !PBPROC(destroy, "vkDestroyPipelineBinaryKHR") ||
        !PBPROC(key, "vkGetPipelineKeyKHR") || !PBPROC(data, "vkGetPipelineBinaryDataKHR") ||
        !PBPROC(release, "vkReleaseCapturedPipelineDataKHR")) {
        rt_log("vk_pipeline: pipeline binaries: the driver lacks a VK_KHR_pipeline_binary function; using pipeline-cache.bin");
        return;
    }
#undef PBPROC
    PbHeader want = {{'N', 'X', '2', 'N', 'P', 'B', 'I', 'N'}, PB_VERSION, 0, {0}};
    VkPipelineBinaryKeyKHR gk = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
    if (PB.key(c->device, NULL, &gk) != VK_SUCCESS || !gk.keySize || gk.keySize > PB_KEY) {
        rt_log("vk_pipeline: pipeline binaries: no global key from the driver; using pipeline-cache.bin");
        return;
    }
    want.gkey_size = gk.keySize;
    memcpy(want.gkey, gk.key, gk.keySize);
    snprintf(PB.path, sizeof PB.path, "%s", P.cache_path);
    char *cut = plat_last_sep(PB.path);
    snprintf(cut ? cut + 1 : PB.path, sizeof PB.path - (size_t)(cut ? cut + 1 - PB.path : 0), "pipeline-binaries.bin");
    uint64_t cap = (uint64_t)strtoull(cfg_value("NX2NAVI_PIPE_BINARIES_MAX_MB"), NULL, 10) << 20;
    FILE *f = fopen(PB.path, "rb");
    PbHeader hd;
    const char *why = NULL;
    uint64_t good = 0, len = 0;
    if (f) {
        pb_seek(f, 0, SEEK_END);
        len = (uint64_t)pb_tell(f);
        pb_seek(f, 0, SEEK_SET);
        if (fread(&hd, sizeof hd, 1, f) != 1 || memcmp(&hd, &want, sizeof hd)) why = "written by another driver, GPU or version";
        else if (cap && len > cap) why = "over NX2NAVI_PIPE_BINARIES_MAX_MB";
        else good = pb_index(f, len);
        fclose(f);
    }
    // A file that ends inside a record (the process ended during an append, or a disk filled) or holds a malformed
    // record: the whole records before it stay, and the file is cut back to them so that appends go on from there.
    // Only when it cannot be cut does it start empty.
    if (f && !why) {
        uint64_t kept = pb_keep(PB.path, good, len);
        if (kept) len = kept;
        else why = "ends inside a record or holds a malformed one, and it cannot be cut back to its whole records";
    }
    if (why || !f) {   // start empty: the index is dropped and the file rewritten
        if (f) rt_log("vk_pipeline: pipeline binaries: %s %s (%.0f MB); starting empty", PB.path, why, len / 1048576.0);
        free(PB.slot), PB.slot = NULL, PB.cap = PB.n = PB.pipes = PB.blobs = 0;
        pb_rewrite(&want);
    } else {
        PB.file_size = len;
    }
    PB.rf = fopen(PB.path, "rb");
    PB.on = PB.rf != NULL && PB.file_size >= sizeof want;
    rt_log("vk_pipeline: pipeline binaries %s: %s, %u pipelines, %u binaries, %.1f MB (NX2NAVI_PIPE_BINARIES=0 goes back "
           "to pipeline-cache.bin)", PB.on ? "on" : "off (cannot open the file)", PB.path, PB.pipes, PB.blobs,
           PB.file_size / 1048576.0);
}

// The payload of the record of (type, key), malloc'd (*size bytes), or NULL. Caller holds PB.lock.
static uint8_t *pb_read(uint32_t type, const uint8_t *key, uint32_t ks, uint32_t *size) {
    PbSlot *s = pb_find(pb_hash(type, key, ks));
    if (!s || s->size < PB_HEAD) return NULL;
    uint32_t n = s->size - PB_HEAD;
    uint8_t *head = malloc(PB_HEAD + (size_t)n), *out = NULL;
    if (!head) return NULL;
    int ok;
    if (s->off >= PB.file_size) {   // still pending, at its place in pend
        size_t at = (size_t)(s->off - PB.file_size) + 4;
        ok = at + s->size <= PB.pend_n;
        if (ok) memcpy(head, PB.pend + at, s->size);
    } else {   // rf is NULL only after pb_flush could not open the file again
        ok = PB.rf && !pb_seek(PB.rf, (int64_t)(s->off + 4), SEEK_SET) && fread(head, s->size, 1, PB.rf) == 1;
    }
    uint32_t t, k;
    if (ok) memcpy(&t, head, 4), memcpy(&k, head + 4, 4);
    if (ok && t == type && k == ks && !memcmp(head + 8, key, ks)) {   // the key itself, not only its hash
        memmove(head, head + PB_HEAD, n);
        out = head, *size = n;
    } else {
        free(head);
    }
    return out;
}

// The binaries stored for pipeline key k, created in bins (*n of them). 0 when none are stored or they fail.
static int pb_load(const VkPipelineBinaryKeyKHR *k, VkPipelineBinaryKHR bins[PB_MAX_BIN], uint32_t *n) {
    VkPipelineBinaryKeyKHR keys[PB_MAX_BIN];
    VkPipelineBinaryDataKHR data[PB_MAX_BIN];
    uint8_t *blob[PB_MAX_BIN] = {0};
    uint32_t nb = 0, size = 0;
    plat_mutex_lock(&PB.lock);
    uint8_t *pipe = pb_read(PB_PIPE, k->key, k->keySize, &size);
    int ok = pipe && size >= 4;
    if (ok) memcpy(&nb, pipe, 4);
    if (!nb || nb > PB_MAX_BIN || size < 4 + nb * (4 + PB_KEY)) ok = 0;
    for (uint32_t i = 0; ok && i < nb; i++) {
        const uint8_t *e = pipe + 4 + i * (4 + PB_KEY);
        uint32_t ks, ds = 0;
        memcpy(&ks, e, 4);
        keys[i] = (VkPipelineBinaryKeyKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR, .keySize = ks};
        memcpy(keys[i].key, e + 4, PB_KEY);
        if (ks > PB_KEY || !(blob[i] = pb_read(PB_BLOB, keys[i].key, ks, &ds))) ok = 0;
        else data[i] = (VkPipelineBinaryDataKHR){.dataSize = ds, .pData = blob[i]};
    }
    plat_mutex_unlock(&PB.lock);
    if (ok) {
        VkPipelineBinaryKeysAndDataKHR kd = {.binaryCount = nb, .pPipelineBinaryKeys = keys, .pPipelineBinaryData = data};
        VkPipelineBinaryCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR, .pKeysAndDataInfo = &kd};
        VkPipelineBinaryHandlesInfoKHR hi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR,
                                             .pipelineBinaryCount = nb, .pPipelineBinaries = bins};
        ok = PB.create(P.device, &ci, NULL, &hi) == VK_SUCCESS;
        *n = nb;
    }
    for (uint32_t i = 0; i < PB_MAX_BIN; i++) free(blob[i]);
    free(pipe);
    return ok;
}

// Appends one record to pend and the index. Caller holds PB.lock.
static int pb_put(uint32_t type, const uint8_t *key, uint32_t ks, const void *a, size_t an, const void *b, size_t bn) {
    size_t size = PB_HEAD + an + bn;
    if (size >= (1u << 31)) return 0;
    if (PB.pend_n + 4 + size > PB.pend_cap) {
        size_t cap = PB.pend_cap ? PB.pend_cap * 2 : (size_t)4 << 20;
        while (cap < PB.pend_n + 4 + size) cap *= 2;
        uint8_t *t = realloc(PB.pend, cap);
        if (!t) return 0;
        PB.pend = t, PB.pend_cap = cap;
    }
    if (!pb_insert(pb_hash(type, key, ks), PB.file_size + PB.pend_n, (uint32_t)size)) return 0;
    uint8_t *o = PB.pend + PB.pend_n;
    uint32_t s32 = (uint32_t)size;
    memcpy(o, &s32, 4), memcpy(o + 4, &type, 4), memcpy(o + 8, &ks, 4);
    memset(o + 12, 0, PB_KEY), memcpy(o + 12, key, ks);
    if (an) memcpy(o + 4 + PB_HEAD, a, an);
    if (bn) memcpy(o + 4 + PB_HEAD + an, b, bn);
    PB.pend_n += 4 + size;
    if (type == PB_PIPE) PB.pipes++;
    else PB.blobs++;
    return 1;
}

static void pb_flush(void);

// Stores the binaries of pipeline p (created with its data captured) under pipeline key k, then frees the data. A
// binary whose key is stored already is not stored again.
static void pb_capture(VkPipeline p, const VkPipelineBinaryKeyKHR *k) {
    VkPipelineBinaryCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR, .pipeline = p};
    VkPipelineBinaryKHR bins[PB_MAX_BIN];
    VkPipelineBinaryHandlesInfoKHR hi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
    uint32_t nb = 0;
    int ok = PB.create(P.device, &ci, NULL, &hi) == VK_SUCCESS && hi.pipelineBinaryCount &&
             hi.pipelineBinaryCount <= PB_MAX_BIN;
    if (ok) {
        hi.pPipelineBinaries = bins;
        ok = PB.create(P.device, &ci, NULL, &hi) == VK_SUCCESS;
        nb = ok ? hi.pipelineBinaryCount : 0;
    }
    VkPipelineBinaryKeyKHR keys[PB_MAX_BIN];
    uint8_t list[4 + PB_MAX_BIN * (4 + PB_KEY)] = {0};
    memcpy(list, &nb, 4);
    for (uint32_t i = 0; ok && i < nb; i++) {   // each binary's key, and its data unless stored already
        VkPipelineBinaryDataInfoKHR di = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR, .pipelineBinary = bins[i]};
        keys[i] = (VkPipelineBinaryKeyKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
        size_t ds = 0;
        ok = PB.data(P.device, &di, &keys[i], &ds, NULL) == VK_SUCCESS && keys[i].keySize && keys[i].keySize <= PB_KEY;
        if (!ok) break;
        memcpy(list + 4 + i * (4 + PB_KEY), &keys[i].keySize, 4), memcpy(list + 8 + i * (4 + PB_KEY), keys[i].key, keys[i].keySize);
        plat_mutex_lock(&PB.lock);
        int have = pb_find(pb_hash(PB_BLOB, keys[i].key, keys[i].keySize)) != NULL;
        plat_mutex_unlock(&PB.lock);
        if (have) {
            PB.blob_shared++;
            continue;
        }
        uint8_t *d = malloc(ds ? ds : 1);
        ok = d && PB.data(P.device, &di, &keys[i], &ds, d) == VK_SUCCESS;
        if (ok) {
            plat_mutex_lock(&PB.lock);
            if (pb_put(PB_BLOB, keys[i].key, keys[i].keySize, d, ds, NULL, 0)) PB.blob_new++;
            plat_mutex_unlock(&PB.lock);   // a 0 from pb_put: another thread stored the same binary meanwhile
        }
        free(d);
    }
    for (uint32_t i = 0; i < nb; i++) PB.destroy(P.device, bins[i], NULL);
    VkReleaseCapturedPipelineDataInfoKHR ri = {.sType = VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR, .pipeline = p};
    PB.release(P.device, &ri, NULL);
    plat_mutex_lock(&PB.lock);
    if (ok && pb_put(PB_PIPE, k->key, k->keySize, list, 4 + nb * (4 + PB_KEY), NULL, 0)) PB.captured++;
    else if (!ok) PB.capture_fail++;
    int full = PB.pend_n >= ((size_t)64 << 20);
    plat_mutex_unlock(&PB.lock);
    if (full) pb_flush();   // a cold boot captures thousands before the autosave thread runs: keep the buffer small
}

// Appends the pending records (the autosave thread every 2 s, and the save at exit).
static void pb_flush(void) {
    if (!PB.on) return;
    plat_mutex_lock(&PB.lock);
    if (PB.pend_n) {
        FILE *f = fopen(PB.path, "ab");
        int ok = f && fwrite(PB.pend, PB.pend_n, 1, f) == 1;
        if (f) ok = !fclose(f) && ok;
        if (ok) PB.file_size += PB.pend_n;
        if (ok) PB.pend_n = 0;
        if (!ok) {
            // Part of the append may be in the file (a full disk): the file is cut back to file_size, where the index
            // places the pending records, or the next append would land after those bytes. The read handle is closed
            // meanwhile, since plat_file_truncate opens the file alone.
            if (PB.rf) fclose(PB.rf);
            int cut = plat_file_truncate(PB.path, PB.file_size);
            PB.rf = fopen(PB.path, "rb");
            rt_log("vk_pipeline: pipeline binaries: cannot append to %s; this run's binaries stay in memory%s", PB.path,
                   cut ? "" : " (and the file cannot be cut back to its whole records)");
        }
    }
    plat_mutex_unlock(&PB.lock);
}

static void pb_log(const char *when) {
    if (!PB.on) return;
    plat_mutex_lock(&PB.lock);
    rt_log("vk_pipeline: pipeline binaries %s: %llu made from pipeline-binaries.bin (avg %.2f ms), %llu compiled (avg %.1f "
           "ms), %llu captured, %llu could not be stored, %llu stored ones failed; binaries %llu new, %llu shared; %u pipelines and "
           "%u binaries in the file, %.1f MB", when,
           (unsigned long long)PB.from_file, PB.from_file ? PB.from_ms / PB.from_file : 0.0,
           (unsigned long long)PB.compiled, PB.compiled ? PB.compile_ms / PB.compiled : 0.0,
           (unsigned long long)PB.captured, (unsigned long long)PB.capture_fail, (unsigned long long)PB.load_fail,
           (unsigned long long)PB.blob_new, (unsigned long long)PB.blob_shared, PB.pipes, PB.blobs,
           (PB.file_size + PB.pend_n) / 1048576.0);
    plat_mutex_unlock(&PB.lock);
}

// Creates one pipeline: kind 0 a graphics pipeline, 1 a graphics pipeline library, 2 a compute pipeline; ci its
// Vk*PipelineCreateInfo. With binaries on: from the stored binaries of its pipeline key when there are any, else
// compiled with P.cache and its binaries captured. The create info is restored before returning.
typedef struct { VkStructureType sType; const void *pNext; VkPipelineCreateFlags flags; } PipeCiHead;

// Builds into P.cache against reads of its data. Every create with P.cache holds this shared (pipe_create_in); a save
// holds it exclusively for the copy of the data and only takes it when no build is running. The Vulkan rules allow
// reading a cache created without VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT while pipelines are built into
// it, but Intel's Windows driver (101.8991 on an Arc 140V) crashed inside vkGetPipelineCacheData during the copy,
// reading through a null entry, with compute and library builds running beside the autosave. So no copy is made while
// a build runs, on any driver.
static PlatMutex cache_rw = PLAT_MUTEX_INIT;
// Set while the save at exit waits for cache_rw: the builds already in the driver finish, and new ones wait in
// cache_build_enter until the copy is made. Without it the boot's prewarm workers started builds back to back, no moment
// without one came within the save's 5 s, and the pipelines built in that run were not saved (a window closed during
// the prewarm, 2026-11-11). The autosave never sets it, so it never holds a build back.
static volatile int32_t cache_saving_at_exit;

// Takes cache_rw shared for a build into P.cache; while the save at exit waits for its copy, first waits for that.
static void cache_build_enter(void) {
    for (;;) {
        while (__atomic_load_n(&cache_saving_at_exit, __ATOMIC_ACQUIRE)) plat_sleep_ms(1);
        plat_mutex_lock_shared(&cache_rw);
        if (!__atomic_load_n(&cache_saving_at_exit, __ATOMIC_ACQUIRE)) return;
        plat_mutex_unlock_shared(&cache_rw);
    }
}

// A pipeline cache of this thread's own that its creates build into in place of P.cache; VK_NULL_HANDLE (every thread
// but the background builder of large compute shaders) for P.cache itself. A create into it does not hold cache_rw, so
// a build of minutes does not keep pipeline-cache.bin from being saved; bg_merge moves its content into P.cache.
static __thread VkPipelineCache own_cache;
// 1 on a thread whose creates take no pipeline cache of ours at all (the library warm in play, lw_worker and lw_probe):
// what it builds is wanted in the driver's own disk cache only. Built into P.cache, the 37,572 stage libraries of a
// warm grew it by 1.6 GB (2026-11-11: a save of 2026.7 MB in play, +1612 MB since the boot); the file was then over
// NX2NAVI_PIPELINE_CACHE_MAX_MB, so the next start dropped it whole and compiled the pipelines it had held.
static __thread int no_cache;
// New content in P.cache that P.stats does not count, for autosave_once to save: bg_merge's merges, and the compiles
// of pipelines built for the cache only (lb_compute keep 0).
static volatile uint32_t cache_merges;

// ---- the play cache (NX2NAVI_PIPELINE_CACHE_PLAY, default 1; needs pipelineCreationCacheControl)
// pipeline-cache.bin holds what the preparation built and what earlier runs made: 300 MB and more on an install. One
// file can only be written whole, so a single new pipeline in play had the whole of it copied out of the driver and
// written again (2026-11-11: saves of 358.1 MB for 0.3 MB of new data, and 302 to 306 MB written in each of seven
// sessions that added about 0.5 MB each). So what a run compiles goes into a second cache, P.play, with a file of its
// own, pipeline-cache-play.bin beside the first: a create asks P.cache with
// VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT, which gives the pipeline when that cache or the driver's
// own has it and VK_PIPELINE_COMPILE_REQUIRED when not, and only then builds into P.play (pipe_create_in). In play
// only the play file is saved; pipeline-cache.bin is not written.
// The play file is read back at the next start, into P.play and merged into P.cache in memory (cache_load), so the
// first create finds its pipelines. Once
// it has grown to NX2NAVI_PIPELINE_CACHE_PLAY_MB (default 64) it is merged into P.cache at the start, before play: the
// autosave thread writes pipeline-cache.bin once and removes the play file (cache_save). The preparation modes
// (--prepare-shaders, --warm-shaders, --prepare-pipelines) merge it at any size and build into P.cache alone, as before.
// A pipeline the driver's own cache gives to the first create enters P.cache in memory only: it is in a file again
// when the play file is next folded in, or when the driver no longer has it and it is compiled.
// Without the device feature, with pipeline binaries on, or with the setting 0, P.play stays VK_NULL_HANDLE and
// everything goes into P.cache as before.
static int prep_mode;   // a preparation mode (set before ensure_init): no play cache

static VkResult pipe_create_one(int kind, void *ci, VkPipeline *pipe, VkPipelineCache cache) {
    return kind == 2 ? vkCreateComputePipelines(P.device, cache, 1, ci, NULL, pipe)
                     : vkCreateGraphicsPipelines(P.device, cache, 1, ci, NULL, pipe);
}

static VkResult pipe_create_in(int kind, void *ci, VkPipeline *pipe, VkPipelineCache cache) {
    if (no_cache) cache = VK_NULL_HANDLE;
    int shared = cache && !own_cache;
    if (cache && own_cache) cache = own_cache;
    uint64_t t0 = perf_mark_begin();   // NX2NAVI_TRACE=perf: the create on any thread, its wait for cache_rw included
    if (shared) cache_build_enter();
    VkResult r;
    if (shared && P.play) {
        PipeCiHead *hd = ci;
        VkPipelineCreateFlags flags = hd->flags;
        hd->flags = flags | VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
        r = pipe_create_one(kind, ci, pipe, cache);
        hd->flags = flags;
        if (r == VK_PIPELINE_COMPILE_REQUIRED) {
            // Not in P.cache nor in the driver's own cache: from P.play, or compiled into it. A caller that asked for
            // cached pipelines only (its own FAIL_ON_PIPELINE_COMPILE_REQUIRED) gets the play cache's answer.
            r = pipe_create_one(kind, ci, pipe, P.play);
            if (r == VK_SUCCESS && *pipe) __atomic_fetch_add(&P.play_made, 1, __ATOMIC_RELAXED);
        }
    } else {
        r = pipe_create_one(kind, ci, pipe, cache);
    }
    if (shared) plat_mutex_unlock_shared(&cache_rw);
    perf_mark_end(PERF_M_PIPE, t0, 0);
    return r;
}

// ---- pipelines of a library that draws on our device (FSR 3.1: vk_ffx.cpp vk_ffx_proc_addr)
// Such a library creates its pipelines itself and passes no pipeline cache, so nothing of ours kept them: every
// context it made (one per render and output size, and one for frame generation) compiled its passes again unless the
// driver's own disk cache still had them, which a renamed or updated exe on NVIDIA and a driver without a disk cache
// do not. vk_ffx.cpp hands the library these creates in the place of vkCreateComputePipelines and
// vkCreateGraphicsPipelines; a create that names no cache goes through ours as pipe_create_in does: asked of P.cache
// first without compiling, and compiled into the play cache when neither it nor the driver has the pipeline, so the
// file a run writes (pipeline-cache-play.bin, or pipeline-cache.bin without a play cache) holds it for the next start.
// Before the first pipeline of ours (P.ready 0) and with pipeline binaries on there is no cache to give: the create
// goes to the driver as the library asked. infos: n VkComputePipelineCreateInfo (compute 1) or
// VkGraphicsPipelineCreateInfo. Any thread.
static struct { volatile uint64_t made, from_cache, compiled, uncached; } FP;

void vk_pipeline_foreign_counts(VkForeignPipes *o) {
    o->made = __atomic_load_n(&FP.made, __ATOMIC_RELAXED), o->from_cache = __atomic_load_n(&FP.from_cache, __ATOMIC_RELAXED);
    o->compiled = __atomic_load_n(&FP.compiled, __ATOMIC_RELAXED), o->uncached = __atomic_load_n(&FP.uncached, __ATOMIC_RELAXED);
}

static VkResult foreign_one(VkDevice dev, int compute, VkPipelineCache cache, uint32_t n, const void *ci,
                            const VkAllocationCallbacks *alloc, VkPipeline *out) {
    return compute ? vkCreateComputePipelines(dev, cache, n, ci, alloc, out)
                   : vkCreateGraphicsPipelines(dev, cache, n, ci, alloc, out);
}

VkResult vk_pipeline_create_foreign(VkDevice dev, int compute, uint32_t n, const void *infos,
                                    const VkAllocationCallbacks *alloc, VkPipeline *out) {
    VkPipelineCache cache = __atomic_load_n(&P.ready, __ATOMIC_ACQUIRE) && dev == P.device ? P.cache : VK_NULL_HANDLE;
    if (!cache) {
        VkResult r = foreign_one(dev, compute, VK_NULL_HANDLE, n, infos, alloc, out);
        if (r == VK_SUCCESS) __atomic_fetch_add(&FP.made, n, __ATOMIC_RELAXED), __atomic_fetch_add(&FP.uncached, n, __ATOMIC_RELAXED);
        return r;
    }
    VkResult r = VK_SUCCESS;
    cache_build_enter();
    if (!P.play) {   // one cache: whether it held them is not known here; its save looks at the data's size
        r = foreign_one(dev, compute, cache, n, infos, alloc, out);
        if (r == VK_SUCCESS) __atomic_fetch_add(&FP.made, n, __ATOMIC_RELAXED), __atomic_fetch_add(&cache_merges, n, __ATOMIC_RELAXED);
    }
    for (uint32_t i = 0; P.play && i < n; i++) {
        // a copy of the create info: the flags are ours to change, the library's struct is not
        VkComputePipelineCreateInfo cc;
        VkGraphicsPipelineCreateInfo gc;
        void *ci;
        if (compute) cc = ((const VkComputePipelineCreateInfo *)infos)[i], ci = &cc;
        else gc = ((const VkGraphicsPipelineCreateInfo *)infos)[i], ci = &gc;
        PipeCiHead *hd = ci;
        // Flags given in a VkPipelineCreateFlags2CreateInfo replace the struct's own, so the question "without
        // compiling" cannot be put through hd->flags: such a create goes to the play cache at once.
        int flags2 = 0;
        for (const VkBaseInStructure *b = hd->pNext; b && !flags2; b = b->pNext)
            flags2 = b->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR;
        VkPipelineCreateFlags flags = hd->flags;
        VkResult ri = VK_PIPELINE_COMPILE_REQUIRED;
        out[i] = VK_NULL_HANDLE;
        if (!flags2) {
            hd->flags = flags | VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
            ri = foreign_one(dev, compute, cache, 1, ci, alloc, &out[i]);
            hd->flags = flags;
            if (ri == VK_SUCCESS) __atomic_fetch_add(&FP.from_cache, 1, __ATOMIC_RELAXED);
        }
        if (ri == VK_PIPELINE_COMPILE_REQUIRED) {
            ri = foreign_one(dev, compute, P.play, 1, ci, alloc, &out[i]);
            if (ri == VK_SUCCESS) {
                __atomic_fetch_add(&FP.compiled, 1, __ATOMIC_RELAXED);
                __atomic_fetch_add(&P.play_made, 1, __ATOMIC_RELAXED);
                __atomic_fetch_add(&cache_merges, 1, __ATOMIC_RELAXED);   // autosave_once: the play file is due
            }
        }
        if (ri == VK_SUCCESS) __atomic_fetch_add(&FP.made, 1, __ATOMIC_RELAXED);
        else if (r == VK_SUCCESS) r = ri;   // the first that failed, as a create of several reports one result
        if (ri != VK_SUCCESS) out[i] = VK_NULL_HANDLE;
    }
    plat_mutex_unlock_shared(&cache_rw);
    return r;
}

static VkResult pipe_create(int kind, void *ci, VkPipeline *pipe) {
    PipeCiHead *hd = ci;
    VkResult r;
    pb_last = 0;
#define PIPE_CREATE(cache) pipe_create_in(kind, ci, pipe, cache)
    if (!PB.on) return PIPE_CREATE(P.cache);
    VkPipelineCreateInfoKHR kci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR, .pNext = ci};
    VkPipelineBinaryKeyKHR key = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
    int keyed = PB.key(P.device, &kci, &key) == VK_SUCCESS && key.keySize && key.keySize <= PB_KEY;
    const void *next = hd->pNext;
    if (keyed) {
        VkPipelineBinaryKHR bins[PB_MAX_BIN];
        uint32_t nb = 0;
        uint64_t t0 = plat_now_ns();
        if (pb_load(&key, bins, &nb)) {
            VkPipelineBinaryInfoKHR bi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR, .pNext = next,
                                          .binaryCount = nb, .pPipelineBinaries = bins};
            hd->pNext = &bi;
            r = PIPE_CREATE(VK_NULL_HANDLE);   // a pipeline made from binaries takes no pipeline cache
            hd->pNext = next;
            for (uint32_t i = 0; i < nb; i++) PB.destroy(P.device, bins[i], NULL);
            plat_mutex_lock(&PB.lock);
            if (r == VK_SUCCESS) PB.from_file++, PB.from_ms += (plat_now_ns() - t0) / 1e6;
            else PB.load_fail++;
            plat_mutex_unlock(&PB.lock);
            if (r == VK_SUCCESS) return pb_last = 1, r;
        }
    }
    int capture = keyed && !PB.no_capture[kind];
    VkPipelineCreateFlags2CreateInfoKHR f2 = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR, .pNext = next,
                                              .flags = (VkPipelineCreateFlags2KHR)hd->flags | VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR};
    uint64_t t0 = plat_now_ns();
    if (capture) hd->pNext = &f2;
    r = PIPE_CREATE(P.cache);
    hd->pNext = next;
    if (capture && r < 0) {   // the driver refuses the capture for this kind: create without it from now on
        PB.no_capture[kind] = 1;
        rt_log("vk_pipeline: pipeline binaries: a %s with its data captured failed (%d); such pipelines are not stored",
               kind == 2 ? "compute pipeline" : kind ? "pipeline library" : "graphics pipeline", (int)r);
        capture = 0;
        r = PIPE_CREATE(P.cache);
    }
#undef PIPE_CREATE
    if (r == VK_SUCCESS && *pipe) {
        plat_mutex_lock(&PB.lock);
        PB.compiled++, PB.compile_ms += (plat_now_ns() - t0) / 1e6;
        plat_mutex_unlock(&PB.lock);
        if (capture) pb_capture(*pipe, &key);
    }
    return r;
}

// The data of the pipeline cache file at path for vkCreatePipelineCache (malloc'd, its size in *size), NULL when the
// run starts without it, which the log says with the file's size. Cache data whose header names another device or
// driver is dropped before Vulkan sees it.
// NX2NAVI_PIPELINE_CACHE_MAX_MB (cache_cap; 0 no limit): a larger file starts the run empty, and the run's
// saves replace it. The limit bounds each of the two files, not their sum. It has to stay above what one run
// builds: at 512, with the pipeline list of 0.9.1 (8,849 records), an RTX 2080 Ti's file held 551 MB and the play
// file the run then wrote 541 MB, so both were dropped at every start, 7,247 of 8,531 pipelines were built again in
// each (17 s of the boot) and 541 MB were written again in each session (cache_load says so when both files are
// over). The file only grows: entries of earlier translators and builds stay, and on NVIDIA
// (621.01) an entry another executable wrote is neither used nor written again, so a renamed or updated exe
// compiles while the file keeps its size (2026-11-09: 650 MB, 15-18 % of the pipelines read from it). The
// whole file sits in process memory twice at the boot (this read and the driver's copy) and is copied out
// at every save of it, which a 16 GB machine near its commit limit cannot spare. The size is checked before the
// read, so a file that is dropped never takes its size in memory. Sizes are 64-bit all the way
// (vk_cache_file.h cache_file_read): a file past 2 GB is read with the limit raised or off, and refused by
// its real size otherwise.
// The limit is kept on the write side too (cache_load's fold, cache_save_one): a run never writes a file the next start
// would refuse. Before, only the read checked it, all or nothing, and nothing kept the files under it: on an RTX 5080
// (0.9.2, 2026-11-12) pipeline-cache-play.bin reached 524.9 MB in one session of about 15 minutes, the next start
// dropped it whole ("starting empty") and the session after compiled its pipelines again (boot hits 13-18 %, about
// 5,000 pipelines left to background builds at 250-273 ms each). pipeline-cache.bin itself grows 14-28 MB at each
// fold (323 -> 448 -> 461 -> 489 MB in four starts) and so crosses 512 MB in a few sessions (0.8.0: a 766 MB file
// dropped at a start). With the limit at 2048 MB the same files were read and folded: boot hits 84 %, nothing left to
// background builds, the boot 17 s instead of 24.
// cache_cap: NX2NAVI_PIPELINE_CACHE_MAX_MB when set (0: no limit); unset, the machine's RAM / 32 between 1024 and
// 4096 MB (16 and 32 GB: 1024; 64 GB: 2048; 128 GB and more: 4096). Never below 1024: a full cache of the pipeline
// list is about 550 MB on an NVIDIA card, and a limit under what the files reach compiles everything past it again at
// every start, for good (RAM / 32 alone gave 509 MB on a 16 GB machine). The RAM and not the commit headroom at the
// boot, so the same file is not read on one start and refused on the next. Read once, at the start; every caller
// computes the same value, so the unlocked cache of it is harmless.
static uint64_t cache_cap(void) {
    static uint64_t cap = UINT64_MAX;
    if (cap != UINT64_MAX) return cap;
    const char *set = cfg_get("NX2NAVI_PIPELINE_CACHE_MAX_MB");
    float used = 0, total = 0, proc = 0;
    uint64_t mb;
    if (set && *set) mb = strtoull(set, NULL, 10);
    else {
        plat_mem_info(&used, &total, &proc);
        mb = total > 0 ? (uint64_t)total / 32 : 512;
        mb = mb < 1024 ? 1024 : mb > 4096 ? 4096 : mb;
    }
    if (set && *set)
        rt_log("vk_pipeline: pipeline cache files up to %llu MB%s (NX2NAVI_PIPELINE_CACHE_MAX_MB)",
               (unsigned long long)mb, mb ? "" : ": no limit");
    else
        rt_log("vk_pipeline: pipeline cache files up to %llu MB (from %.0f GB of RAM; NX2NAVI_PIPELINE_CACHE_MAX_MB "
               "sets it)", (unsigned long long)mb, total / 1024.0);
    return cap = mb << 20;
}

static int cache_over_cap;   // the last file cache_data_read was asked for is over the limit
static int cache_foreign;    // pipeline-cache.bin was written by another device or driver (a driver update)
static void *cache_data_read(VkCtx *c, const char *path, size_t *size) {
    *size = 0;
    cache_over_cap = 0;
    if (!path[0]) return NULL;
    uint64_t cap = cache_cap(), len = 0;
    int why = CACHE_FILE_NONE;
    void *data = cache_file_read(path, sizeof(VkPipelineCacheHeaderVersionOne), cap, &len, &why);
    if (!data) {
        if (why == CACHE_FILE_OVER_CAP)
            cache_over_cap = 1,
            rt_log("vk_pipeline: %s holds %.0f MB, over NX2NAVI_PIPELINE_CACHE_MAX_MB %llu: starting empty; it is "
                   "replaced when the play file is next folded in, at the next start that finds one", path, len / 1048576.0, (unsigned long long)(cap >> 20));
        // (A file over the limit comes from before the limit was kept on the write side, or from a lower limit.)
        else if (why != CACHE_FILE_NONE)
            rt_log("vk_pipeline: %s (%llu bytes, %.1f MB) is not read: %s; starting empty", path,
                   (unsigned long long)len, len / 1048576.0, cache_file_why(why));
        return NULL;
    }
    VkPipelineCacheHeaderVersionOne h;
    memcpy(&h, data, sizeof h);
    if (h.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || h.vendorID != c->props.vendorID ||
        h.deviceID != c->props.deviceID || memcmp(h.pipelineCacheUUID, c->props.pipelineCacheUUID, VK_UUID_SIZE)) {
        rt_log("vk_pipeline: %s (%.1f MB) was written by another device or driver; starting empty", path,
               len / 1048576.0);
        if (path == P.cache_path) cache_foreign = 1;
        free(data);
        return NULL;
    }
    *size = (size_t)len;
    return data;
}

// A pipeline cache from data (may be NULL: empty); VK_NULL_HANDLE when the driver makes none. *kept: the size of
// the data it holds afterwards. A driver may drop initial data it does not accept without failing the create, which
// leaves every build of the run compiling while the cache file looks valid.
static VkPipelineCache cache_make(VkCtx *c, const void *data, size_t size, size_t *kept) {
    VkPipelineCache cache = VK_NULL_HANDLE;
    VkPipelineCacheCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = size,
                                    .pInitialData = size ? data : NULL};
    if (vkCreatePipelineCache(c->device, &ci, NULL, &cache) != VK_SUCCESS) {
        ci.initialDataSize = 0, ci.pInitialData = NULL;
        if (vkCreatePipelineCache(c->device, &ci, NULL, &cache) != VK_SUCCESS) cache = VK_NULL_HANDLE;
    }
    *kept = 0;
    if (cache && vkGetPipelineCacheData(c->device, cache, kept, NULL) != VK_SUCCESS) *kept = 0;
    return cache;
}

static void cache_load(VkCtx *c) {
    cache_path_init();
    pb_init(c);
    if (PB.on) {   // the binaries carry compiled pipelines across runs; P.cache stays VK_NULL_HANDLE
        rt_log("vk_pipeline: %s is not read or written while pipeline binaries are on", P.cache_path);
        return;
    }
    size_t size = 0, kept = 0;
    void *data = cache_data_read(c, P.cache_path, &size);
    int base_over = cache_over_cap;   // an oversized file is never read again: the play file takes its place at once
    P.cache = cache_make(c, data, size, &kept);
    free(data);
    P.cache_loaded = kept;
    // The file holds this data when the driver took all of it: a save is due only once the cache has grown (cache_save).
    P.saved[0] = size && kept == size ? kept : 0;
    if (size)
        rt_log("vk_pipeline: %s: %.1f MB read, the driver holds %.1f MB of it", P.cache_path, size / 1048576.0,
               kept / 1048576.0);
    if (!P.cache || !P.cache_path[0]) return;

    // The play cache (see above pipe_create_in).
    snprintf(P.play_path, sizeof P.play_path, "%s", P.cache_path);
    size_t n = strlen(P.play_path);
    if (n > 4 && !strcmp(P.play_path + n - 4, ".bin")) n -= 4;
    snprintf(P.play_path + n, sizeof P.play_path - n, "-play.bin");
    int want = cfg_bool("NX2NAVI_PIPELINE_CACHE_PLAY") && !prep_mode, on = want && P.cache_control;
    size_t psize = 0, pkept = 0;
    void *pdata = cache_data_read(c, P.play_path, &psize);
    if (base_over && cache_over_cap)   // what one run builds is over the limit: nothing will ever replace either file
        rt_log("vk_pipeline: both pipeline cache files are over NX2NAVI_PIPELINE_CACHE_MAX_MB, so what one run builds does "
               "not fit it: this start builds every pipeline again, and so will each one after it until the limit is "
               "raised above the play file's size (pipeline_cache_max_mb in nx2navi.ini; 0 = no limit)");
    VkPipelineCache play = psize || on ? cache_make(c, pdata, psize, &pkept) : VK_NULL_HANDLE;
    free(pdata);
    uint64_t fold_at = (uint64_t)strtoull(cfg_value("NX2NAVI_PIPELINE_CACHE_PLAY_MB"), NULL, 10) << 20;
    int fold = play && psize && (!on || psize >= fold_at || base_over);
    if (fold && on && !base_over && cache_cap()) {
        // Only a fold whose result fits: pipeline-cache.bin past the limit would be dropped whole at the next start
        // (cache_data_read). The merge into P.cache is the one the kept play cache gets below anyway, so it is done
        // here once and its size asked; a play file mostly holds what the other already has (461.2 + 524.9 MB folded
        // to 489.0 MB, 2026-11-12), so the size of the two files says little.
        // The fold is written at the first autosave, after the boot's prewarm and background builds have added to
        // P.cache: it is taken only with room for a play file's worth of growth (fold_at) under the limit, so that
        // save is not refused (cache_save keeps the play file meanwhile).
        VkResult r = vkMergePipelineCaches(c->device, P.cache, 1, &play);
        size_t merged = 0;
        if (r == VK_SUCCESS && vkGetPipelineCacheData(c->device, P.cache, &merged, NULL) == VK_SUCCESS &&
            merged + fold_at > cache_cap()) {
            rt_log("vk_pipeline: %s: %.1f MB read, the driver holds %.1f MB of it; folding it in would make %s %.1f MB, "
                   "with less than %llu MB of room left under the limit of %llu MB: both files stay as they are, and "
                   "this run's pipelines go into the play file as long as it stays under the limit", P.play_path,
                   psize / 1048576.0, pkept / 1048576.0, P.cache_path, merged / 1048576.0,
                   (unsigned long long)(fold_at >> 20), (unsigned long long)(cache_cap() >> 20));
            fold = 0, psize = 0;   // merged already: the branch for a kept play file below is skipped
        } else if (r != VK_SUCCESS) {
            rt_log("vk_pipeline: %s did not merge into the cache of %s (%d); it is left as it is and not used in this run",
                   P.play_path, P.cache_path, (int)r);
            vkDestroyPipelineCache(c->device, play, NULL);
            play = VK_NULL_HANDLE, fold = 0, on = 0, psize = 0;
        } else {
            rt_log("vk_pipeline: folding %s in makes %s %.1f MB (the limit is %llu MB)", P.play_path, P.cache_path,
                   merged / 1048576.0, (unsigned long long)(cache_cap() >> 20));
            vkDestroyPipelineCache(c->device, play, NULL);
            play = VK_NULL_HANDLE;
            P.fold = 1, P.saved[0] = 0;
            rt_log("vk_pipeline: %s: %.1f MB read, the driver holds %.1f MB of it; merged into the cache of %s, which is "
                   "written once now (it reached the size at which it is folded in, before play)", P.play_path,
                   psize / 1048576.0, pkept / 1048576.0, P.cache_path);
            play = cache_make(c, NULL, 0, &pkept), psize = 0;
            fold = 0;   // done
        }
    }
    if (fold) {
        // Into P.cache now; the file is written by the first save, which then removes the play file. A run that ends
        // before that save finds both files as they were and merges again.
        VkResult r = vkMergePipelineCaches(c->device, P.cache, 1, &play);
        vkDestroyPipelineCache(c->device, play, NULL);
        play = VK_NULL_HANDLE;
        if (r == VK_SUCCESS) {
            P.fold = 1, P.saved[0] = 0;
            rt_log("vk_pipeline: %s: %.1f MB read, the driver holds %.1f MB of it; merged into the cache of %s, which is "
                   "written once now (%s)", P.play_path, psize / 1048576.0, pkept / 1048576.0, P.cache_path,
                   prep_mode ? "a preparation builds into that file alone"
                   : !want ? "NX2NAVI_PIPELINE_CACHE_PLAY=0"
                   : !P.cache_control ? "no pipelineCreationCacheControl on this device"
                   : "it reached the size at which it is folded in, before play");
            if (on) play = cache_make(c, NULL, 0, &pkept), psize = 0;
        } else {
            rt_log("vk_pipeline: %s did not merge into the cache of %s (%d); it is left as it is and not used in this run",
                   P.play_path, P.cache_path, (int)r);
            on = 0;
        }
    } else if (psize) {
        // Kept as the play cache, so its file goes on holding what earlier runs made, and merged into P.cache in
        // memory, where the first create of pipe_create_in finds its pipelines. Without the merge that create took
        // them from the driver's own cache when it had them, at 8 ms a pipeline against 0.4 ms from ours.
        VkResult r = vkMergePipelineCaches(c->device, P.cache, 1, &play);
        rt_log("vk_pipeline: %s: %.1f MB read, the driver holds %.1f MB of it%s", P.play_path, psize / 1048576.0,
               pkept / 1048576.0, r == VK_SUCCESS ? "" : "; it did not merge into the other cache in memory");
    }
    P.play = on ? play : VK_NULL_HANDLE;
    P.play_loaded = P.play ? pkept : 0;
    // An empty play cache is not written (its data is a header): its size counts as saved, as a whole file's does.
    P.saved[1] = P.play && (!psize || pkept == psize) ? pkept : 0;
    if (P.play)
        rt_log("vk_pipeline: what this run compiles goes into %s; %s is not written in play", P.play_path, P.cache_path);
    else if (want && !P.cache_control)
        rt_log("vk_pipeline: no play cache (no pipelineCreationCacheControl on this device): a new pipeline has %s "
               "written whole", P.cache_path);
}

// The float widths spirv_nan_preserve declares on this device (P.nan_preserve), from NX2NAVI_VK_NAN_PRESERVE: 0 off,
// 1 on every driver; unset, on every driver except NVIDIA's own, on which the game draws right without it. Only widths
// the driver can preserve (VkPhysicalDeviceFloatControlsProperties) are declared, and nothing without 32-bit floats.
static void nan_preserve_init(VkCtx *c) {
    VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceFloatControlsProperties fc = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES,
                                                  .pNext = &drv};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &fc};
    vkGetPhysicalDeviceProperties2(c->phys, &p2);
    const char *e = cfg_get("NX2NAVI_VK_NAN_PRESERVE");   // no fixed default: it depends on the driver, see above
    int set = e && *e, want = set ? strcmp(e, "0") != 0 : drv.driverID != VK_DRIVER_ID_NVIDIA_PROPRIETARY;
    uint32_t can = (fc.shaderSignedZeroInfNanPreserveFloat16 ? NP_16 : 0) |
                   (fc.shaderSignedZeroInfNanPreserveFloat32 ? NP_32 : 0) |
                   (fc.shaderSignedZeroInfNanPreserveFloat64 ? NP_64 : 0);
    P.nan_preserve = want && (can & NP_32) ? can : 0;
    // NX2NAVI_VK_NAN_BITS: 1 every driver, 0 none; unset: AMD's driver for RDNA 1 and 2 (spirv_nan_bits)
    const char *nb = cfg_get("NX2NAVI_VK_NAN_BITS");
    P.nan_bits = nb && *nb ? strcmp(nb, "0") != 0 : vk_caps.amd_sc;
    if (P.nan_bits)
        rt_log("vk_pipeline: game shaders test for NaN on the bits of the number, not as a float (%s)",
               nb && *nb ? "NX2NAVI_VK_NAN_BITS=1" : "AMD's driver for RDNA 1 and 2");
    if (P.nan_preserve)
        rt_log("vk_pipeline: game shaders keep NaN, infinity and signed zero (SignedZeroInfNanPreserve for %s32%s-bit "
               "floats%s)", can & NP_16 ? "16, " : "", can & NP_64 ? ", 64" : "", set ? ", NX2NAVI_VK_NAN_PRESERVE=1" : "");
    else
        rt_log("vk_pipeline: game shaders without SignedZeroInfNanPreserve (%s)",
               !want ? (set ? "NX2NAVI_VK_NAN_PRESERVE=0" : "NVIDIA's driver") : "the driver cannot keep them in 32-bit floats");
}

static void keys_open(void);
static void cu_open(void);
static void prebuild_start(void);

// ---- stage libraries for mesh pipelines (NX2NAVI_GPL_MESH, default 1; 0: each mesh pipeline is built whole, as before)
// A mesh pipeline (task and mesh stages: the game's foliage, grass and terrain) used to be left out of the stage
// libraries: each new one was a whole build, 0.2 to 1.1 s the first time foliage of a kind appeared, and its draws
// waited or were left out of the picture until it was done. With the setting on such a pipeline is made like a vertex
// pipeline ("graphics pipeline library" below): linked at its first use from libraries, with the whole build behind.
//  - Its pre-rasterization library holds the task and mesh stages with the viewport and rasterization state, keyed by
//    both shaders (PrKey.vs, PrKey.ts). A mesh pipeline has no vertex input state, so it is linked from three
//    libraries and none of them is a vertex input library (gpl_link).
//  - Its layout is a stage-independent pair like a vertex pipeline's (mesh_pair_layout): set 0 holds the mesh stage's
//    bindings and the task stage's after them, set 1 the fragment stage's. Without libraries all three share set 0.
//  - Its fragment shader library is the one a vertex pipeline with the same fragment shader uses, unless the key says
//    otherwise (FsKey): the inputs no mesh output feeds are filled in the module (fill, points), and on a device
//    with a per-draw shading rate the rate is dynamic state of every mesh pipeline, which the pre-rasterization and
//    the fragment shader state both hold, so both libraries list it (vrs). The fragment output library is shared.
//  - The pre-rasterization library lists the dynamic states a whole mesh pipeline lists (mesh_dyn): not the vertex
//    input, its strides, the topology or primitive restart, which a pipeline with a mesh stage may not list. So does
//    a fragment shader library that is a mesh pipeline's alone (vrs). A library shared with vertex pipelines still
//    lists those four: they belong to the vertex input state, which no library of a mesh pipeline holds, and what a
//    library lists as dynamic counts only for the state the library itself holds.
// Off, with the reason in the start's log line: without the graphics pipeline library, on a device that draws no mesh
// pipeline (no mesh shaders, or the mesh fallback), and where mesh pipelines alone are built without the driver's
// optimiser (noopt_flag_for): a fragment library shared with vertex pipelines would be an optimised one.
static VkPipelineCreateFlags noopt_flag_for(int mesh);

static void mesh_lib_init(const VkCtx *c) {
    const char *why = !c->has_mesh_shader ? "this GPU has no mesh shaders"
                    : c->mesh_fallback ? "mesh draws go through the mesh fallback (vk_device.c, mesh_fallback)"
                    : !vk_caps.gpl ? "the graphics pipeline library is off"
                    : !cfg_bool("NX2NAVI_GPL_MESH") ? "NX2NAVI_GPL_MESH=0"
                    : noopt_flag_for(1) != noopt_flag_for(0) ? "they alone are built without the driver's optimiser "
                                                               "(NX2NAVI_VK_NOOPT), which a shared library would not be"
                    : NULL;
    P.mesh_lib = !why;
    if (why) rt_log("vk_pipeline: mesh pipelines are built whole, without stage libraries: %s", why);
    else rt_log("vk_pipeline: mesh pipelines are linked from stage libraries like vertex pipelines: the task and mesh "
                "stages in one pre-rasterization library, a fast link at the first draw, the whole build behind it "
                "(NX2NAVI_GPL_MESH=0 builds each whole, as before)");
}

static int ensure_init(void) {
    if (P.ready) return 1;
    if (!vk_init()) return 0;
    plat_mutex_lock(&P.lock);
    if (P.ready) {
        plat_mutex_unlock(&P.lock);
        return 1;
    }
    VkCtx *c = vk_ctx();
    P.device = c->device;
    __atomic_store_n(&pipe_minute, minute_now(), __ATOMIC_RELAXED);
    VkPhysicalDevicePushDescriptorPropertiesKHR pd = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &pd};
    vkGetPhysicalDeviceProperties2(c->phys, &p2);
    P.max_push = pd.maxPushDescriptors;
    P.indep = vk_caps.gpl;
    P.gpl = vk_caps.gpl;
    rt_log(P.gpl ? "vk_pipeline: graphics pipeline library on: a draw whose pipeline is building gets a fast-linked one, "
                   "the optimised build replaces it when done"
                 : "vk_pipeline: graphics pipeline library off: draws wait for or skip pipelines that are building");
    const char *ps = cfg_get("NX2NAVI_PUSH_STAGE"), *vo = cfg_get("NX2NAVI_VS_OUTS");
    P.push_stage = ps && !strcmp(ps, "fs") ? GFX_STAGE_FS : GFX_STAGE_VS;
    P.vs_outs_all = vo && !strcmp(vo, "all");
    if (P.indep) rt_log("vk_pipeline: stage-independent layouts: vertex set 0, fragment set 1, %s set pushed; vertex outputs: %s",
                        P.push_stage == GFX_STAGE_VS ? "vertex" : "fragment", P.vs_outs_all ? "all 32 locations" : "union of paired fragment inputs");
    mesh_lib_init(c);
    // vk_device.c enables every feature the GPU reports in this chain, so the query tells what is enabled.
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT e3 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceProvokingVertexFeaturesEXT pvf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT,
                                                      .pNext = c->has_eds3 ? &e3 : NULL};
    VkPhysicalDeviceVulkan13Features v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .pNext = c->has_provoking_vertex ? (void *)&pvf : (c->has_eds3 ? (void *)&e3 : NULL)};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v13};
    vkGetPhysicalDeviceFeatures2(c->phys, &f2);
    P.pv_last = c->has_provoking_vertex && pvf.provokingVertexLast;
    if (!P.pv_last) rt_log("vk_pipeline: provokingVertexLast unavailable: flat outputs come from the first vertex (NVN: last)");
    // Maxwell warps have 32 lanes; the translator assumes it (maxwell.md 3.4). Pin 32 where the driver allows it.
    if (v13.subgroupSizeControl) {
        VkPhysicalDeviceVulkan13Properties p13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
        VkPhysicalDeviceProperties2 q2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p13};
        vkGetPhysicalDeviceProperties2(c->phys, &q2);
        P.sg32_stages = vk_caps.sg_pin_stages;   // vk_caps.c: 32 unless NX2NAVI_VK_SUBGROUP; 0 without the pin
        rt_log("vk_pipeline: subgroup size %u-%u, requiredSubgroupSizeStages %#x, pinning %u on %#x", p13.minSubgroupSize,
               p13.maxSubgroupSize, (unsigned)p13.requiredSubgroupSizeStages, vk_caps.sg_size, (unsigned)P.sg32_stages);
    }
    // A warp reconverges where the guest compiler put it back together; Vulkan guarantees that only with maximal
    // reconvergence (spirv_reconverge). NX2NAVI_VK_RECONVERGE=0 leaves the modules as translated.
    P.reconverge = c->has_maximal_reconvergence && rt_knob("VK_RECONVERGE", "1") == 1;
    rt_log("vk_pipeline: maximal reconvergence for shaders with subgroup ops: %s",
           P.reconverge ? "on" : c->has_maximal_reconvergence ? "off (NX2NAVI_VK_RECONVERGE=0)" : "not supported by the driver");
    nan_preserve_init(c);
    P.f_full_subgroups = v13.computeFullSubgroups;
    P.cache_control = v13.pipelineCreationCacheControl;
    // Pipeline creation feedback is core in Vulkan 1.3; the driver reports per pipeline whether P.cache held it.
    P.feedback = c->props.apiVersion >= VK_API_VERSION_1_3 ? atoi(cfg_value("NX2NAVI_PIPE_FEEDBACK")) : 0;
    P.f_logic_op = f2.features.logicOp;
    P.f_depth_clamp = f2.features.depthClamp;
    P.f_fill_non_solid = f2.features.fillModeNonSolid;
    P.f_dual_src = f2.features.dualSrcBlend;
    P.wide_lines = f2.features.wideLines && cfg_bool("NX2NAVI_LINE_WIDTH");
    P.line_w[0] = c->props.limits.lineWidthRange[0], P.line_w[1] = c->props.limits.lineWidthRange[1];
    P.line_gran = c->props.limits.lineWidthGranularity;
    if (!P.wide_lines)
        rt_log("vk_pipeline: lines are drawn 1 pixel wide whatever width the game sets (%s)",
               f2.features.wideLines ? "NX2NAVI_LINE_WIDTH=0" : "this device has no wide lines");
#define PROC(field, name) (P.field = (void *)vk_dpa(c->device, name))
    PROC(push, "vkCmdPushDescriptorSetKHR");
    // Descriptor update templates (core 1.1; the push variant comes with VK_KHR_push_descriptor on 1.1): the driver
    // walks a layout prepared when the layout is made instead of parsing write structs every draw.
    // NX2NAVI_DESC_TEMPLATE=0 keeps the write structs.
    if (!cfg_bool("NX2NAVI_DESC_TEMPLATE")) {
        rt_log("vk_pipeline: descriptor update templates off (NX2NAVI_DESC_TEMPLATE=0)");
    } else if (c->props.apiVersion >= VK_API_VERSION_1_1 && PROC(tpl_create, "vkCreateDescriptorUpdateTemplate") &&
               PROC(tpl_destroy, "vkDestroyDescriptorUpdateTemplate") && PROC(tpl_update, "vkUpdateDescriptorSetWithTemplate")) {
        P.tpl = 1;
        if (!PROC(tpl_push, "vkCmdPushDescriptorSetWithTemplateKHR"))
            rt_log("vk_pipeline: vkCmdPushDescriptorSetWithTemplateKHR missing: push sets are written with write structs");
    } else {
        rt_log("vk_pipeline: descriptor update templates unavailable: sets are written with write structs");
    }
    // NX2NAVI_VK_DYN_VERTEX=0/1 forces dynamic vertex input off or on. Default off on two drivers:
    // - Intel's frozen Windows driver for its older integrated GPUs (UHD 630, Ice Lake: Vulkan 1.3.215 and older), which
    //   draws nothing with it.
    // - AMD's driver for RDNA 3 and newer (vk_caps.amd_llpc), which compiles a pipeline with dynamic vertex input again
    //   at every boot: neither pipeline-cache.bin nor the driver's own disk cache gives it back. Measured on an
    //   RX 7900 XT (26.10.1) with NX2NAVI_PIPE_FEEDBACK=2, booting one save twice from an empty cache: the second boot
    //   compiled all 622 recorded non-mesh pipelines again at 83 ms each, with the module and state hashes of the first
    //   boot (the same with NX2NAVI_GPL=0, so it is not the pipeline libraries or their layouts), while the mesh
    //   pipelines, which have no vertex input, came from the file. With the vertex layout in the pipeline the second
    //   boot built 787 pipelines in 170 ms and the third 898 of 898 from the file in 233 ms, with no build left for the
    //   scene. A player's file of 3,979 records used the whole boot budget (PREWARM_PIPE_MS, 20 s) at every boot; with
    //   the vertex layout in the pipeline its third boot built 4,273 pipelines in 0.9 s. The price is more pipelines,
    //   one per vertex layout (787 records against 692 for that boot); records written with dynamic vertex input still
    //   prebuild, each for the layout of the draw that recorded it. The RDNA 1 and 2 branch (vk_caps.amd_sc) is not
    //   measured and keeps dynamic vertex input.
    const char *dv = cfg_get("NX2NAVI_VK_DYN_VERTEX");   // no fixed default: it depends on the GPU, see above
    int intel_legacy = c->props.vendorID == 0x8086 && c->props.apiVersion <= VK_MAKE_API_VERSION(0, 1, 3, 215);
    int dv_on = dv && *dv ? strcmp(dv, "0") != 0 : !intel_legacy && !vk_caps.amd_llpc;
    P.dyn_vi = dv_on && c->has_vertex_input_dynamic_state && PROC(set_vi, "vkCmdSetVertexInputEXT");
    if (!dv_on && c->has_vertex_input_dynamic_state)
        rt_log("vk_pipeline: dynamic vertex input off (%s)",
               dv && *dv ? "NX2NAVI_VK_DYN_VERTEX=0"
               : intel_legacy ? "Intel's older Windows driver draws nothing with it"
               : "AMD's driver compiles pipelines that have it again at every boot");
    if (c->has_eds3) {
        P.dyn_clamp = e3.extendedDynamicState3DepthClampEnable && P.f_depth_clamp && PROC(set_clamp, "vkCmdSetDepthClampEnableEXT");
        P.dyn_poly = e3.extendedDynamicState3PolygonMode && PROC(set_poly, "vkCmdSetPolygonModeEXT");
        P.dyn_blend_en = e3.extendedDynamicState3ColorBlendEnable && PROC(set_blend_en, "vkCmdSetColorBlendEnableEXT");
        P.dyn_blend_eq = e3.extendedDynamicState3ColorBlendEquation && PROC(set_blend_eq, "vkCmdSetColorBlendEquationEXT");
        P.dyn_wmask = e3.extendedDynamicState3ColorWriteMask && PROC(set_wmask, "vkCmdSetColorWriteMaskEXT");
        P.dyn_a2c = e3.extendedDynamicState3AlphaToCoverageEnable && PROC(set_a2c, "vkCmdSetAlphaToCoverageEnableEXT");
    }
    // NX2NAVI_VK_DYN_A2C=0/1 forces dynamic alpha to coverage off or on. Default off on AMD's Windows driver branch for
    // RDNA 2 and older (vk_caps.amd_sc), which applies alpha to coverage while the dynamic state says off: on an RX 590
    // (24.9.1) NX2navi, which never turns it on, drew Link and the NPCs with depth but no colour (black silhouettes) and
    // dithered other surfaces into checkerboards (issue #67, RX 470/570/580). Taken out of the dynamic state, it goes
    // into the pipeline key and the characters draw. With the texture pool marks it stays out of the dynamic state
    // whatever the setting: a marked fragment shader gets early fragment tests only in a pipeline that has alpha to
    // coverage off (spirv_pool_mark), so the pipeline has to know.
    const char *da = cfg_get("NX2NAVI_VK_DYN_A2C");   // no fixed default: it depends on the driver, see above
    int marks = vk_pool_marks_enabled();
    int da_on = !marks && (da && *da ? strcmp(da, "0") != 0 : !vk_caps.amd_sc);
    if (!da_on && P.dyn_a2c) {
        P.dyn_a2c = 0;
        rt_log("vk_pipeline: dynamic alpha to coverage off (%s)", marks ? "the texture pool marks, NX2NAVI_TEX_DISUSE"
                                                                  : da && *da ? "NX2NAVI_VK_DYN_A2C=0"
                                                                              : "AMD's driver for RDNA 2 and older");
    }
#undef PROC
    static const VkDynamicState core[] = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
        VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
    P.n_dyn = 0;
    for (size_t i = 0; i < sizeof core / sizeof *core; i++) P.dyn[P.n_dyn++] = core[i];
    P.dyn[P.n_dyn++] = P.dyn_vi ? VK_DYNAMIC_STATE_VERTEX_INPUT_EXT : VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE;
    if (P.dyn_clamp) P.dyn[P.n_dyn++] = VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT;
    if (P.dyn_poly) P.dyn[P.n_dyn++] = VK_DYNAMIC_STATE_POLYGON_MODE_EXT;
    if (P.dyn_blend_en) P.dyn[P.n_dyn++] = VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT;
    if (P.dyn_blend_eq) P.dyn[P.n_dyn++] = VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT;
    if (P.dyn_wmask) P.dyn[P.n_dyn++] = VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT;
    if (P.dyn_a2c) P.dyn[P.n_dyn++] = VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT;
    vk_mem_stage_log("device creation");
    cache_load(c);
    vk_mem_stage_log("the pipeline cache load");
    rt_log("vk_pipeline: max push descriptors %u; dynamic vertex input %d, depth clamp %d, polygon mode %d, blend enable %d, "
           "blend equation %d, write mask %d, alpha to coverage %d; cache %s",
           P.max_push, P.dyn_vi, P.dyn_clamp, P.dyn_poly, P.dyn_blend_en, P.dyn_blend_eq, P.dyn_wmask, P.dyn_a2c,
           P.cache_path[0] ? P.cache_path : "<memory only>");
    keys_open();
    cu_open();
    plat_xchg32(&P.ready, 1);
    plat_mutex_unlock(&P.lock);
    prebuild_start();
    return 1;
}

// ---- NVN -> Vulkan mappings

static VkBlendFactor blend_factor(unsigned f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: LOG_ONCE("vk_pipeline: unknown NVN blend factor %#x, using ONE", f); return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blend_op(unsigned e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

// NVN_DEPTH_FUNC_* / NVN_STENCIL_FUNC_* are NEVER = 1 .. ALWAYS = 8 in VkCompareOp order.
static VkCompareOp compare_op(unsigned f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }
// NVN_STENCIL_OP_* are KEEP = 1 .. DECR_WRAP = 8 in VkStencilOp order.
static VkStencilOp stencil_op(unsigned o) { return o >= 1 && o <= 8 ? (VkStencilOp)(o - 1) : VK_STENCIL_OP_KEEP; }
// NVN_POLYGON_MODE_POINT/LINE/FILL = 0/1/2, VkPolygonMode FILL/LINE/POINT = 0/1/2.
static VkPolygonMode polygon_mode(unsigned m) {
    if (!P.f_fill_non_solid || m > 2) return VK_POLYGON_MODE_FILL;
    return (VkPolygonMode)(2 - m);
}

VkPrimitiveTopology vk_pipeline_topology(int prim) {
    switch (prim) {
    case NVN_DRAW_PRIMITIVE_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case NVN_DRAW_PRIMITIVE_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case NVN_DRAW_PRIMITIVE_LINE_LOOP:
    case NVN_DRAW_PRIMITIVE_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP:
    case NVN_DRAW_PRIMITIVE_QUAD_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;   // same triangles, same coverage
    case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN:
    case NVN_DRAW_PRIMITIVE_POLYGON: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_PATCHES: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // TRIANGLES; QUADS after the replay's index rewrite
    }
}

// Topology class (a pipeline's static topology only fixes the class): 0 point, 1 line, 2 triangle, 3 patch.
static int topology_class(VkPrimitiveTopology t) {
    switch (t) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return 0;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY: return 1;
    case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST: return 3;
    default: return 2;
    }
}

int vk_pipeline_tex_type(int target) {
    switch (target) {
    case NVN_TEXTURE_TARGET_1D: return GFX_TEX_1D;
    case NVN_TEXTURE_TARGET_3D: return GFX_TEX_3D;
    case NVN_TEXTURE_TARGET_1D_ARRAY: return GFX_TEX_1D_ARRAY;
    case NVN_TEXTURE_TARGET_2D_ARRAY: return GFX_TEX_2D_ARRAY;
    case NVN_TEXTURE_TARGET_2D_MULTISAMPLE: return GFX_TEX_2D_MS;
    case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: return GFX_TEX_2D_MS_ARRAY;
    case NVN_TEXTURE_TARGET_CUBEMAP: return GFX_TEX_CUBE;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: return GFX_TEX_CUBE_ARRAY;
    case NVN_TEXTURE_TARGET_BUFFER: return GFX_TEX_BUFFER;
    default: return GFX_TEX_2D;   // 2D, RECTANGLE
    }
}

// Numeric type of a colour format as a shader sees it: GFX_TYPE_*.
static int format_type(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16_UINT: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_B8G8R8A8_UINT: case VK_FORMAT_S8_UINT:
        return GFX_TYPE_UINT;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8_SINT: case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16_SINT: case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_SINT: case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_A2B10G10R10_SINT_PACK32: case VK_FORMAT_B8G8R8A8_SINT:
        return GFX_TYPE_SINT;
    default: return GFX_TYPE_FLOAT;
    }
}

// Optimal-tiling (o = 1) or buffer (o = 0) features of a core format, cached.
static VkFormatFeatureFlags format_features(VkFormat f, int optimal) {
    static VkFormatFeatureFlags known[2][256];
    static uint8_t have[2][256];
    VkCtx *c = vk_ctx();
    if (!c) return 0;
    if ((unsigned)f < 256 && have[optimal][f]) return known[optimal][f];
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(c->phys, f, &p);
    VkFormatFeatureFlags r = optimal ? p.optimalTilingFeatures : p.bufferFeatures;
    if ((unsigned)f < 256) known[optimal][f] = r, have[optimal][f] = 1;
    return r;
}

// Vertex attribute formats by NVNformat.
typedef struct { VkFormat vk; uint8_t type; } VtxFmt;
#define F(v) {VK_FORMAT_##v, GFX_TYPE_FLOAT}
#define U(v) {VK_FORMAT_##v, GFX_TYPE_UINT}
#define I(v) {VK_FORMAT_##v, GFX_TYPE_SINT}
static const VtxFmt vtx_formats[0x9B] = {
    [0x01] = F(R8_UNORM), [0x02] = F(R8_SNORM), [0x03] = U(R8_UINT), [0x04] = I(R8_SINT), [0x05] = F(R16_SFLOAT),
    [0x06] = F(R16_UNORM), [0x07] = F(R16_SNORM), [0x08] = U(R16_UINT), [0x09] = I(R16_SINT), [0x0A] = F(R32_SFLOAT),
    [0x0B] = U(R32_UINT), [0x0C] = I(R32_SINT), [0x0D] = F(R8G8_UNORM), [0x0E] = F(R8G8_SNORM), [0x0F] = U(R8G8_UINT),
    [0x10] = I(R8G8_SINT), [0x11] = F(R16G16_SFLOAT), [0x12] = F(R16G16_UNORM), [0x13] = F(R16G16_SNORM),
    [0x14] = U(R16G16_UINT), [0x15] = I(R16G16_SINT), [0x16] = F(R32G32_SFLOAT), [0x17] = U(R32G32_UINT),
    [0x18] = I(R32G32_SINT), [0x19] = F(R8G8B8_UNORM), [0x1A] = F(R8G8B8_SNORM), [0x1B] = U(R8G8B8_UINT),
    [0x1C] = I(R8G8B8_SINT), [0x1D] = F(R16G16B16_SFLOAT), [0x1E] = F(R16G16B16_UNORM), [0x1F] = F(R16G16B16_SNORM),
    [0x20] = U(R16G16B16_UINT), [0x21] = I(R16G16B16_SINT), [0x22] = F(R32G32B32_SFLOAT), [0x23] = U(R32G32B32_UINT),
    [0x24] = I(R32G32B32_SINT), [0x25] = F(R8G8B8A8_UNORM), [0x26] = F(R8G8B8A8_SNORM), [0x27] = U(R8G8B8A8_UINT),
    [0x28] = I(R8G8B8A8_SINT), [0x29] = F(R16G16B16A16_SFLOAT), [0x2A] = F(R16G16B16A16_UNORM),
    [0x2B] = F(R16G16B16A16_SNORM), [0x2C] = U(R16G16B16A16_UINT), [0x2D] = I(R16G16B16A16_SINT),
    [0x2E] = F(R32G32B32A32_SFLOAT), [0x2F] = U(R32G32B32A32_UINT), [0x30] = I(R32G32B32A32_SINT),
    [0x3D] = F(A2B10G10R10_UNORM_PACK32), [0x3E] = U(A2B10G10R10_UINT_PACK32), [0x3F] = F(B10G11R11_UFLOAT_PACK32),
    // *_UI2F / *_I2F: integer data read as float; the 8- and 16-bit ones are Vulkan's SCALED formats
    [0x51] = F(R8_USCALED), [0x52] = F(R8_SSCALED), [0x53] = F(R16_USCALED), [0x54] = F(R16_SSCALED),
    [0x57] = F(R8G8_USCALED), [0x58] = F(R8G8_SSCALED), [0x59] = F(R16G16_USCALED), [0x5A] = F(R16G16_SSCALED),
    [0x5D] = F(R8G8B8_USCALED), [0x5E] = F(R8G8B8_SSCALED), [0x5F] = F(R16G16B16_USCALED), [0x60] = F(R16G16B16_SSCALED),
    [0x63] = F(R8G8B8A8_USCALED), [0x64] = F(R8G8B8A8_SSCALED), [0x65] = F(R16G16B16A16_USCALED),
    [0x66] = F(R16G16B16A16_SSCALED),
    // 32-bit ones have no SCALED format: read as integers (values are not converted; logged when used)
    [0x55] = U(R32_UINT), [0x56] = I(R32_SINT), [0x5B] = U(R32G32_UINT), [0x5C] = I(R32G32_SINT),
    [0x61] = U(R32G32B32_UINT), [0x62] = I(R32G32B32_SINT), [0x67] = U(R32G32B32A32_UINT), [0x68] = I(R32G32B32A32_SINT),
    [0x69] = F(A2B10G10R10_SNORM_PACK32), [0x6A] = I(A2B10G10R10_SINT_PACK32), [0x6B] = F(A2B10G10R10_USCALED_PACK32),
    [0x6C] = F(A2B10G10R10_SSCALED_PACK32), [0x9A] = F(B8G8R8A8_UNORM),
};
#undef F
#undef U
#undef I

// 3-component 8/16-bit formats are not vertex formats on every GPU: read 4 components (LAS vertex_format_supported).
static VkFormat widen(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R8G8B8_USCALED: return VK_FORMAT_R8G8B8A8_USCALED;
    case VK_FORMAT_R8G8B8_SSCALED: return VK_FORMAT_R8G8B8A8_SSCALED;
    case VK_FORMAT_R16G16B16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R16G16B16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    case VK_FORMAT_R16G16B16_USCALED: return VK_FORMAT_R16G16B16A16_USCALED;
    case VK_FORMAT_R16G16B16_SSCALED: return VK_FORMAT_R16G16B16A16_SSCALED;
    default: return VK_FORMAT_UNDEFINED;
    }
}

VkFormat vk_vertex_format(int nvn_format, int *type) {
    if (type) *type = GFX_TYPE_FLOAT;
    if (nvn_format <= 0 || nvn_format >= (int)(sizeof vtx_formats / sizeof *vtx_formats)) return VK_FORMAT_UNDEFINED;
    const VtxFmt *v = &vtx_formats[nvn_format];
    if (v->vk == VK_FORMAT_UNDEFINED) return VK_FORMAT_UNDEFINED;
    if (type) *type = v->type;
    if ((nvn_format >= 0x55 && nvn_format <= 0x68) && v->type != GFX_TYPE_FLOAT)
        LOG_ONCE("vk_pipeline: 32-bit *_UI2F/*_I2F vertex formats are read as integers (first: %#x)", nvn_format);
    VkFormat f = v->vk;
    if (vk_ctx() && !(format_features(f, 0) & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
        VkFormat w = widen(f);
        if (w == VK_FORMAT_UNDEFINED || !(format_features(w, 0) & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
            LOG_ONCE("vk_pipeline: vertex format %d (NVN %#x) unsupported", (int)f, nvn_format);
            return VK_FORMAT_UNDEFINED;
        }
        LOG_ONCE("vk_pipeline: vertex format %d unsupported, read as 4 components (first: NVN %#x)", (int)f, nvn_format);
        f = w;
    }
    return f;
}

// Attachment format of an NVN format (tex_vk_format: the format the images are created with).
static VkFormat target_format(int nvn_format, int *depth, int *stencil) {
    TexVkFormat t;
    if (depth) *depth = 0;
    if (stencil) *stencil = 0;
    if (!nvn_format || !tex_vk_format(nvn_format, &t)) return VK_FORMAT_UNDEFINED;
    if (depth) *depth = t.depth;
    if (stencil) *stencil = t.stencil;
    return t.vk;
}

void vk_pipe_state_defaults(VkPipeState *s) {
    memset(s, 0, sizeof *s);
    s->samples = 1;
    s->prim = NVN_DRAW_PRIMITIVE_TRIANGLES;
    for (int i = 0; i < 8; i++)
        s->blend[i] = (blend_st){(uint64_t)i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE,
                                 NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    s->color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS};
    s->chmask.mask = 0xffffffffu;
    s->ms.enable = 1;
    s->poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    s->ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS,
                    NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP,
                    NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP};
}

void vk_pipeline_answers(const VkPipeState *s, GfxShaderAnswers *vs, GfxShaderAnswers *fs) {
    if (vs)
        for (int i = 0; i < 16; i++) {
            va_st a;
            memcpy(&a, &s->vattr[i], sizeof a);
            int type = GFX_TYPE_FLOAT;
            if (a.fmt) vk_vertex_format((int)a.fmt, &type);
            vs->attr_type[i] = (uint8_t)type;
        }
    if (fs)
        for (int i = 0; i < 8; i++) {
            VkFormat f = target_format(s->color_fmt[i], NULL, NULL);
            fs->frag_out_type[i] = (uint8_t)(f == VK_FORMAT_UNDEFINED ? GFX_TYPE_FLOAT : format_type(f));
        }
}

// ---- vertex input (pipeline state, or vkCmdSetVertexInputEXT)

typedef VkVtxInput VtxInput;

// An attribute the game left without a format is a fixed one on Maxwell, a single 32-bit component of 0 (an unused
// attribute is bound as IsFixed 1x32 float): it reads (0, 0, 0, 1), as Vulkan expands a
// one-component format. NX2NAVI's foliage shaders scale wind sway by 1 - colour.w; shapes without a vertex colour
// (Brightbloom stem and leaves) must not sway, or they bend away from the parts that have one (the bulb).
static VkFormat zero_format(int type) {
    return type == GFX_TYPE_UINT ? VK_FORMAT_R32_UINT : type == GFX_TYPE_SINT ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_SFLOAT;
}

// Attributes for the Locations the vertex shader reads; one without a usable NVN attribute reads VK_PIPE_ZERO_BINDING.
static void vertex_input(const VkPipeState *s, VtxInput *v) {
    GfxShaderIo io[32];
    uint32_t used = gfx_shader_inputs(s->vs, io), streams = 0;
    int zero = 0;
    memset(v, 0, sizeof *v);
    for (int l = 0; l < 32; l++) {
        if (!(used >> l & 1)) continue;
        VkVertexInputAttributeDescription2EXT *a = &v->a[v->n_attr++];
        *a = (VkVertexInputAttributeDescription2EXT){.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
                                                     .location = (uint32_t)l};
        va_st va = {0};
        if (l < 16) memcpy(&va, &s->vattr[l], sizeof va);
        VkFormat f = va.fmt ? vk_vertex_format((int)va.fmt, NULL) : VK_FORMAT_UNDEFINED;
        if (f != VK_FORMAT_UNDEFINED && va.stream < 16) {
            a->binding = va.stream, a->format = f, a->offset = va.offset;
            streams |= 1u << va.stream;
        } else {
            a->binding = VK_PIPE_ZERO_BINDING, a->format = zero_format(io[l].type);
            zero = 1;
        }
    }
    for (uint32_t b = 0; b < 16; b++) {
        if (!(streams >> b & 1)) continue;
        uint32_t div = s->vstream[b].divisor;
        if (div > 1) LOG_ONCE("vk_pipeline: instance divisor %u treated as 1 (no VK_EXT_vertex_attribute_divisor)", div);
        v->b[v->n_bind++] = (VkVertexInputBindingDescription2EXT){
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, .binding = b, .stride = s->vstream[b].stride,
            .inputRate = div ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX, .divisor = 1};
    }
    if (zero)
        v->b[v->n_bind++] = (VkVertexInputBindingDescription2EXT){
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, .binding = VK_PIPE_ZERO_BINDING,
            .stride = 0, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX, .divisor = 1};
}

int vk_pipeline_vertex_input_dynamic(void) { return ensure_init() && P.dyn_vi; }

PFN_vkCmdPushDescriptorSetKHR vk_pipeline_push_fn(void) { return ensure_init() ? P.push : NULL; }
PFN_vkCmdPushDescriptorSetWithTemplateKHR vk_pipeline_push_tpl_fn(void) { return ensure_init() && P.tpl ? P.tpl_push : NULL; }
PFN_vkUpdateDescriptorSetWithTemplate vk_pipeline_update_tpl_fn(void) { return ensure_init() && P.tpl ? P.tpl_update : NULL; }

int vk_pipeline_set_indep(int on) {
    if (!ensure_init()) return 0;
    plat_mutex_lock(&P.lock);
    int was = P.indep;
    P.indep = on && vk_caps.gpl;
    plat_mutex_unlock(&P.lock);
    return was;
}

// ---- SPIR-V rewrites

// Binding decorations b >= 1 become b + off (the fragment stage's share of set 0).
static uint32_t *spirv_rebind(const uint32_t *w, uint64_t n, uint32_t off) {
    uint32_t *o = malloc(n * 4);
    if (!o) return NULL;
    memcpy(o, w, n * 4);
    for (uint64_t i = 5; i < n;) {
        uint32_t op = o[i] & 0xffff, len = o[i] >> 16;
        if (!len || i + len > n || op == SPV_OP_FUNCTION) break;
        if (op == SPV_OP_DECORATE && len >= 4 && o[i + 2] == SPV_DEC_BINDING && o[i + 3] >= 1) o[i + 3] += off;
        i += len;
    }
    return o;
}

// DescriptorSet decorations become set (the fragment stage's resources move to set 1 with independent layouts).
static uint32_t *spirv_set(const uint32_t *w, uint64_t n, uint32_t set) {
    uint32_t *o = malloc(n * 4);
    if (!o) return NULL;
    memcpy(o, w, n * 4);
    for (uint64_t i = 5; i < n;) {
        uint32_t op = o[i] & 0xffff, len = o[i] >> 16;
        if (!len || i + len > n || op == SPV_OP_FUNCTION) break;
        if (op == SPV_OP_DECORATE && len >= 4 && o[i + 2] == SPV_DEC_DESCRIPTOR_SET) o[i + 3] = set;
        i += len;
    }
    return o;
}

// Dual-source blending (docs/maxwell.md 5.6): SRC1 blend factors read the fragment shader's colour output 1,
// which Vulkan takes from Location 0, Index 1. The Output variable at Location 1 moves there, as the translator declares it
// with DualSourceBlendEnable (CodeGen/Spirv/Declarations.cs); the pipeline is keyed by GKey.dsb, so only pipelines
// that blend with SRC1 get this module. NULL when the module has no Output at Location 1.
static uint32_t *spirv_dual_source(const uint32_t *w, uint64_t n, uint64_t *out_n) {
    uint32_t outs[64];
    int nout = 0;
    uint64_t at = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return NULL;
        if (op == SPV_OP_VARIABLE && len >= 4 && w[i + 3] == SPV_SC_OUTPUT && nout < 64) outs[nout++] = w[i + 2];
        i += len;
    }
    for (uint64_t i = 5; i < n && !at;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == SPV_OP_DECORATE && len == 4 && w[i + 2] == SPV_DEC_LOCATION && w[i + 3] == 1)
            for (int k = 0; k < nout; k++)
                if (outs[k] == w[i + 1]) at = i;
        i += len;
    }
    if (!at) return NULL;
    uint32_t *o = malloc((n + 4) * 4);
    if (!o) return NULL;
    memcpy(o, w, (at + 4) * 4);
    o[at + 3] = 0;
    o[at + 4] = 4u << 16 | SPV_OP_DECORATE, o[at + 5] = w[at + 1], o[at + 6] = SPV_DEC_INDEX, o[at + 7] = 1;
    memcpy(o + at + 8, w + at + 4, (n - at - 4) * 4);
    *out_n = n + 4;
    return o;
}

// Adds an Output variable for each Location of `missing` (type and width from io) to a module, declared in the entry
// point interface and initialised as Maxwell reads an unwritten output: (0, 0, 0, 1), the w as the bits of 1.0f whatever
// the type (attribute memory is untyped); fewer than 4 components are all zero. Existing scalar, vector and pointer types
// are reused. NULL when the module does not have the expected layout.
static uint32_t *spirv_add_outputs(const uint32_t *w, uint64_t n, uint32_t missing, const GfxShaderIo io[32],
                                   uint64_t *out_n) {
    uint64_t ep_at = 0, ann_end = 0, fn_at = 0;
    uint32_t bound = w[3], scalar[3] = {0, 0, 0};          // GFX_TYPE_FLOAT, SINT, UINT
    struct { uint32_t id, comp, count; } vec[64];
    struct { uint32_t id, type; } ptr[64];
    int nvec = 0, nptr = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return NULL;
        const uint32_t *a = w + i + 1;
        if (op == SPV_OP_ENTRY_POINT && !ep_at) ep_at = i;
        else if (op == SPV_OP_DECORATE || op == 72 || op == 73 || op == 74 || op == 75 || op == 332 || op == 5632 || op == 5633)
            ann_end = i + len;
        else if (op == SPV_OP_TYPE_FLOAT && len >= 3 && a[1] == 32) scalar[GFX_TYPE_FLOAT] = a[0];
        else if (op == SPV_OP_TYPE_INT && len >= 4 && a[1] == 32) scalar[a[2] ? GFX_TYPE_SINT : GFX_TYPE_UINT] = a[0];
        else if (op == SPV_OP_TYPE_VECTOR && len >= 4 && nvec < 64) vec[nvec].id = a[0], vec[nvec].comp = a[1], vec[nvec++].count = a[2];
        else if (op == SPV_OP_TYPE_POINTER && len >= 4 && a[1] == SPV_SC_OUTPUT && nptr < 64) ptr[nptr].id = a[0], ptr[nptr++].type = a[2];
        else if (op == SPV_OP_FUNCTION) { fn_at = i; break; }
        i += len;
    }
    if (!ep_at || !ann_end || !fn_at || ann_end < ep_at || fn_at < ann_end) return NULL;
    uint32_t types[32 * 40], decs[32 * 4], vars[32], zero[3] = {0, 0, 0}, one[3] = {0, 0, 0};
    int nt = 0, nd = 0, nv = 0;
    for (int l = 0; l < 32; l++) {
        if (!(missing >> l & 1)) continue;
        int t = io[l].type <= GFX_TYPE_UINT ? io[l].type : GFX_TYPE_FLOAT;
        uint32_t comps = io[l].comps ? io[l].comps : 4;
        if (!scalar[t]) {
            scalar[t] = bound++;
            if (t == GFX_TYPE_FLOAT) types[nt++] = 3u << 16 | SPV_OP_TYPE_FLOAT, types[nt++] = scalar[t], types[nt++] = 32;
            else types[nt++] = 4u << 16 | SPV_OP_TYPE_INT, types[nt++] = scalar[t], types[nt++] = 32, types[nt++] = t == GFX_TYPE_SINT;
        }
        uint32_t type = scalar[t];
        if (comps > 1) {
            int k = 0;
            while (k < nvec && !(vec[k].comp == type && vec[k].count == comps)) k++;
            if (k == nvec) {
                if (nvec == 64) return NULL;
                vec[nvec].id = bound++, vec[nvec].comp = type, vec[nvec++].count = comps;
                types[nt++] = 4u << 16 | SPV_OP_TYPE_VECTOR, types[nt++] = vec[k].id, types[nt++] = type, types[nt++] = comps;
            }
            type = vec[k].id;
        }
        int k = 0;
        while (k < nptr && ptr[k].type != type) k++;
        if (k == nptr) {
            if (nptr == 64) return NULL;
            ptr[nptr].id = bound++, ptr[nptr++].type = type;
            types[nt++] = 4u << 16 | SPV_OP_TYPE_POINTER, types[nt++] = ptr[k].id, types[nt++] = SPV_SC_OUTPUT, types[nt++] = type;
        }
        uint32_t init = bound++, var = bound++;
        if (comps == 4) {
            if (!zero[t]) {
                zero[t] = bound++, one[t] = bound++;
                types[nt++] = 4u << 16 | SPV_OP_CONSTANT, types[nt++] = scalar[t], types[nt++] = zero[t], types[nt++] = 0;
                types[nt++] = 4u << 16 | SPV_OP_CONSTANT, types[nt++] = scalar[t], types[nt++] = one[t], types[nt++] = 0x3f800000;
            }
            types[nt++] = 7u << 16 | SPV_OP_CONSTANT_COMPOSITE, types[nt++] = type, types[nt++] = init;
            types[nt++] = zero[t], types[nt++] = zero[t], types[nt++] = zero[t], types[nt++] = one[t];
        } else {
            types[nt++] = 3u << 16 | SPV_OP_CONSTANT_NULL, types[nt++] = type, types[nt++] = init;
        }
        types[nt++] = 5u << 16 | SPV_OP_VARIABLE, types[nt++] = ptr[k].id, types[nt++] = var, types[nt++] = SPV_SC_OUTPUT,
        types[nt++] = init;
        decs[nd++] = 4u << 16 | SPV_OP_DECORATE, decs[nd++] = var, decs[nd++] = SPV_DEC_LOCATION, decs[nd++] = (uint32_t)l;
        vars[nv++] = var;
    }
    uint64_t total = n + (uint64_t)(nt + nd + nv);
    uint32_t *o = malloc(total * 4), *p = o;
    if (!o) return NULL;
    uint32_t ep_len = w[ep_at] >> 16;
    memcpy(p, w, 5 * 4), p[3] = bound, p += 5;
    memcpy(p, w + 5, (ep_at - 5) * 4), p += ep_at - 5;
    *p++ = (ep_len + (uint32_t)nv) << 16 | SPV_OP_ENTRY_POINT;
    memcpy(p, w + ep_at + 1, (ep_len - 1) * 4), p += ep_len - 1;
    memcpy(p, vars, (size_t)nv * 4), p += nv;
    memcpy(p, w + ep_at + ep_len, (ann_end - ep_at - ep_len) * 4), p += ann_end - ep_at - ep_len;
    memcpy(p, decs, (size_t)nd * 4), p += nd;
    memcpy(p, w + ann_end, (fn_at - ann_end) * 4), p += fn_at - ann_end;
    memcpy(p, types, (size_t)nt * 4), p += nt;
    memcpy(p, w + fn_at, (n - fn_at) * 4), p += n - fn_at;
    *out_n = (uint64_t)(p - o);
    return o;
}

// The locations a mesh module writes (its Output variables with a Location), and whether it draws points (execution
// mode OutputPoints). 0 when the module cannot be read.
static uint32_t spirv_mesh_outputs(const uint32_t *w, uint64_t n, int *points) {
    enum { OP_EXECUTION_MODE = 16, MODE_OUTPUT_POINTS = 27 };
    uint32_t bound = n >= 5 && w[3] < (1u << 22) ? w[3] : 0, mask = 0;
    uint8_t *out = bound ? calloc(bound, 1) : NULL;
    int8_t *loc = bound ? malloc(bound) : NULL;
    *points = 0;
    if (!out || !loc) return free(out), free(loc), 0;
    memset(loc, -1, bound);
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) break;
        const uint32_t *a = w + i + 1;
        if (op == OP_EXECUTION_MODE && len >= 3 && a[1] == MODE_OUTPUT_POINTS) *points = 1;
        else if (op == SPV_OP_DECORATE && len >= 4 && a[1] == SPV_DEC_LOCATION && a[0] < bound && a[2] < 32) loc[a[0]] = (int8_t)a[2];
        else if (op == SPV_OP_VARIABLE && len >= 4 && a[2] == SPV_SC_OUTPUT && a[1] < bound) out[a[1]] = 1;
        else if (op == SPV_OP_FUNCTION) break;
        i += len;
    }
    for (uint32_t id = 0; id < bound; id++)
        if (out[id] && loc[id] >= 0) mask |= 1u << loc[id];
    free(out), free(loc);
    return mask;
}

// A fragment module whose inputs at the locations of `missing` read what Maxwell gives a fragment input no earlier
// stage writes: (0, 0, 0, 1), or with `points` the point sprite's coordinate (s, t, 0, 1). Each such Input variable
// (a float or a float vector) becomes a Private one, the access chains through it Private pointers, its decorations
// go, and the entry function stores the value into it before anything else. NULL when nothing is to be done or the
// module does not have the expected layout.
static uint32_t *spirv_fs_fill_inputs(const uint32_t *w, uint64_t n, uint32_t missing, int points, uint64_t *out_n) {
    enum { SC_INPUT = 1, SC_PRIVATE = 6, OP_EXECUTION_MODE = 16, OP_TYPE_POINTER = 32, OP_LABEL = 248, OP_LOAD = 61,
           OP_STORE = 62, OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66, OP_COMPOSITE_CONSTRUCT = 80,
           OP_COMPOSITE_EXTRACT = 81, OP_DECORATE_ID = 332, BUILTIN_POINT_COORD = 16, MAX_FILL = 32, MAX_PRIV = 64 };
    if (!missing || n < 5 || w[3] >= (1u << 22)) return NULL;
    uint32_t old_bound = w[3], bound = old_bound;
    uint32_t *pt_type = calloc(old_bound, 4), *vec_comp = calloc(old_bound, 4), *vec_n = calloc(old_bound, 4);
    int8_t *loc = malloc(old_bound);
    uint8_t *fill = calloc(old_bound, 1);      // 1: a filled variable, 2: an access chain through one
    uint32_t *o = NULL;
    if (!pt_type || !vec_comp || !vec_n || !loc || !fill) goto done;
    memset(loc, -1, old_bound);
    uint32_t f32 = 0, v2 = 0, entry = 0;
    uint64_t ep_at = 0, ann_end = 0, fn_at = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) goto done;
        const uint32_t *a = w + i + 1;
        if (op == SPV_OP_ENTRY_POINT && !ep_at) ep_at = i, entry = a[1];
        else if (op == SPV_OP_DECORATE || op == 72 || op == OP_DECORATE_ID) {
            ann_end = i + len;
            if (op == SPV_OP_DECORATE && len >= 4 && a[1] == SPV_DEC_LOCATION && a[0] < old_bound && a[2] < 32)
                loc[a[0]] = (int8_t)a[2];
        } else if (op == SPV_OP_TYPE_FLOAT && len >= 3 && a[1] == 32) f32 = a[0];
        else if (op == SPV_OP_TYPE_VECTOR && len >= 4 && a[0] < old_bound) {
            vec_comp[a[0]] = a[1], vec_n[a[0]] = a[2];
            if (a[1] == f32 && a[2] == 2) v2 = a[0];
        } else if (op == OP_TYPE_POINTER && len >= 4 && a[0] < old_bound) pt_type[a[0]] = a[2];
        else if (op == SPV_OP_FUNCTION) { fn_at = i; break; }
        i += len;
    }
    if (!ep_at || !ann_end || !fn_at || !f32 || !entry || ann_end > fn_at) goto done;
    // the variables to fill, and the Private pointer types they and the chains through them need (pass 1)
    uint32_t vars[MAX_FILL], vtype[MAX_FILL], nvars = 0, priv[MAX_PRIV][2], npriv = 0;   // priv: Input pointer type -> Private
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        uint32_t ptr = 0;
        if (op == SPV_OP_VARIABLE && len >= 4 && a[2] == SC_INPUT && a[1] < old_bound && a[0] < old_bound && i < fn_at) {
            int l = loc[a[1]];
            uint32_t t = pt_type[a[0]];
            int floaty = t == f32 || (t < old_bound && vec_comp[t] == f32 && vec_n[t] >= 2 && vec_n[t] <= 4);
            if (l >= 0 && (missing >> l & 1) && floaty && nvars < MAX_FILL) {
                fill[a[1]] = 1, vars[nvars] = a[1], vtype[nvars++] = t, ptr = a[0];
            }
        } else if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && len >= 4 && a[2] < old_bound && fill[a[2]] &&
                   a[1] < old_bound && a[0] < old_bound) {
            fill[a[1]] = 2, ptr = a[0];
        }
        if (ptr) {
            uint32_t k = 0;
            while (k < npriv && priv[k][0] != ptr) k++;
            if (k == npriv) {
                if (npriv == MAX_PRIV) goto done;
                priv[npriv][0] = ptr, priv[npriv++][1] = bound++;
            }
        }
        i += len;
    }
    if (!nvars) goto done;
    // the globals added before the first function: two constants, gl_PointCoord, and the Private pointer types not yet
    // declared (a filled variable's goes right before the variable, below; an access chain's is only used in a function)
    uint32_t extra[MAX_PRIV * 4 + 32], ne = 0, c0 = bound++, c1 = bound++, pc_ptr = 0, pc_var = 0;
    uint8_t declared[MAX_PRIV] = {0};
    extra[ne++] = 4u << 16 | SPV_OP_CONSTANT, extra[ne++] = f32, extra[ne++] = c0, extra[ne++] = 0;
    extra[ne++] = 4u << 16 | SPV_OP_CONSTANT, extra[ne++] = f32, extra[ne++] = c1, extra[ne++] = 0x3f800000;
    if (points) {
        if (!v2) v2 = bound++, extra[ne++] = 4u << 16 | SPV_OP_TYPE_VECTOR, extra[ne++] = v2, extra[ne++] = f32, extra[ne++] = 2;
        pc_ptr = bound++, pc_var = bound++;
        extra[ne++] = 4u << 16 | OP_TYPE_POINTER, extra[ne++] = pc_ptr, extra[ne++] = SC_INPUT, extra[ne++] = v2;
        extra[ne++] = 4u << 16 | SPV_OP_VARIABLE, extra[ne++] = pc_ptr, extra[ne++] = pc_var, extra[ne++] = SC_INPUT;
    }
    o = malloc((n + ne + (uint64_t)npriv * 4 + 16 + (uint64_t)nvars * 12 + 32) * 4);
    if (!o) goto done;
    uint32_t *p = o;
    memcpy(p, w, 5 * 4), p += 5;
    int in_entry = 0, stored = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        if (i == fn_at) {
            for (uint32_t k = 0; k < npriv; k++)
                if (!declared[k]) *p++ = 4u << 16 | OP_TYPE_POINTER, *p++ = priv[k][1], *p++ = SC_PRIVATE, *p++ = pt_type[priv[k][0]];
            memcpy(p, extra, ne * 4), p += ne;
        }
        if (i == ep_at) {   // the interface: filled variables out before SPIR-V 1.4 (Private is not listed), gl_PointCoord in
            uint32_t *head = p, name_words = (uint32_t)(strnlen((const char *)(a + 2), (len - 3) * 4) / 4 + 1);
            for (uint32_t k = 0; k < len; k++) {
                uint32_t v = w[i + k];
                if (k >= 3 + name_words && v < old_bound && fill[v] == 1 && w[1] < 0x00010400) continue;
                *p++ = v;
            }
            if (pc_var) *p++ = pc_var;
            *head = (uint32_t)(p - head) << 16 | SPV_OP_ENTRY_POINT;
            i += len;
            continue;
        }
        if ((op == SPV_OP_DECORATE || op == OP_DECORATE_ID) && len >= 3 && a[0] < old_bound && fill[a[0]] == 1) {
            i += len;   // Location, Flat and the like belong to Input variables
            if (i == ann_end && pc_var)
                *p++ = 4u << 16 | SPV_OP_DECORATE, *p++ = pc_var, *p++ = SPV_DEC_BUILTIN, *p++ = BUILTIN_POINT_COORD;
            continue;
        }
        if (op == SPV_OP_VARIABLE && len >= 4 && a[1] < old_bound && fill[a[1]] == 1)   // its pointer type first
            for (uint32_t k = 0; k < npriv; k++)
                if (priv[k][0] == a[0] && !declared[k])
                    declared[k] = 1, *p++ = 4u << 16 | OP_TYPE_POINTER, *p++ = priv[k][1], *p++ = SC_PRIVATE, *p++ = pt_type[priv[k][0]];
        memcpy(p, w + i, len * 4);
        if ((op == SPV_OP_VARIABLE || op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && len >= 4 &&
            a[1] < old_bound && fill[a[1]]) {
            for (uint32_t k = 0; k < npriv; k++)
                if (priv[k][0] == a[0]) p[1] = priv[k][1];
            if (op == SPV_OP_VARIABLE) p[3] = SC_PRIVATE;
        }
        p += len;
        if (i + len == ann_end && pc_var)   // gl_PointCoord's BuiltIn after the last decoration
            *p++ = 4u << 16 | SPV_OP_DECORATE, *p++ = pc_var, *p++ = SPV_DEC_BUILTIN, *p++ = BUILTIN_POINT_COORD;
        if (op == SPV_OP_FUNCTION && len >= 5) in_entry = a[1] == entry;
        i += len;
        if (op == OP_LABEL && in_entry && !stored) {   // the entry's first block: its OpVariables, then the stores
            while (i < n && (w[i] & 0xffff) == SPV_OP_VARIABLE) memcpy(p, w + i, (w[i] >> 16) * 4), p += w[i] >> 16, i += w[i] >> 16;
            uint32_t sx = c0, sy = c0;
            if (pc_var) {
                uint32_t pc = bound++;
                sx = bound++, sy = bound++;
                *p++ = 4u << 16 | OP_LOAD, *p++ = v2, *p++ = pc, *p++ = pc_var;
                *p++ = 5u << 16 | OP_COMPOSITE_EXTRACT, *p++ = f32, *p++ = sx, *p++ = pc, *p++ = 0;
                *p++ = 5u << 16 | OP_COMPOSITE_EXTRACT, *p++ = f32, *p++ = sy, *p++ = pc, *p++ = 1;
            }
            for (uint32_t k = 0; k < nvars; k++) {
                uint32_t t = vtype[k], c = t == f32 ? 1 : vec_n[t], comp[4] = {sx, sy, c0, c1}, val = comp[0];
                if (c > 1) {
                    val = bound++;
                    *p++ = (3 + c) << 16 | OP_COMPOSITE_CONSTRUCT, *p++ = t, *p++ = val;
                    for (uint32_t q = 0; q < c; q++) *p++ = comp[q];   // x y z w in order: (s, t, 0, 1) or (0, 0, 0, 1)
                }
                *p++ = 3u << 16 | OP_STORE, *p++ = vars[k], *p++ = val;
            }
            stored = 1;
        }
    }
    if (!stored) { free(o), o = NULL; goto done; }
    o[3] = bound;
    *out_n = (uint64_t)(p - o);
done:
    free(pt_type), free(vec_comp), free(vec_n), free(loc), free(fill);
    return o;
}

// Two fixes for what the translator declares but a shader does not use (NULL: nothing to change):
//  - the fragment shader interlock capability and extension, which the translator adds to every fragment shader when the host
//    reports interlock support (SpirvGenerator.cs) and vk_device.c does not enable; kept when the shader interlocks;
//  - the CullDistance[1] member of gl_PerVertex next to ClipDistance[8] (Declarations.cs CreatePerVertexStructType):
//    9 distances exceed maxCombinedClipAndCullDistances (8 on the RX 9070). The translator never accesses CullDistance; the
//    member goes when it is the struct's last and no access chain selects it.
static uint32_t *spirv_fixup(const uint32_t *w, uint64_t n, uint64_t *out_n) {
    enum { OP_EXTENSION = 10, OP_CAPABILITY = 17, OP_MEMBER_NAME = 6, OP_EXECUTION_MODE = 16, OP_TYPE_ARRAY = 28,
           OP_TYPE_STRUCT = 30, OP_CONSTANT = 43, OP_MEMBER_DECORATE = 72, OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66,
           OP_BEGIN_INTERLOCK = 5364, OP_END_INTERLOCK = 5365, CAP_SAMPLE_INTERLOCK = 5363, CAP_PIXEL_INTERLOCK = 5378,
           CAP_RATE_INTERLOCK = 5372, BUILTIN_CLIP = 3, BUILTIN_CULL = 4 };
    if (n < 5) return NULL;
    uint32_t bound = w[3] < (1u << 22) ? w[3] : (1u << 22);
    uint32_t *val = calloc(bound, sizeof *val), *elem = calloc(bound, sizeof *elem), *len = calloc(bound, sizeof *len),
             *pointee = calloc(bound, sizeof *pointee), *var_type = calloc(bound, sizeof *var_type);
    uint32_t *o = NULL;
    int interlock_used = 0, interlock_declared = 0, cull_ok = 0;
    uint32_t cull_struct = 0, cull_member = 0, clip_member = ~0u, members = 0, member_types[8] = {0};
    if (!val || !elem || !len || !pointee || !var_type) goto out;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        if (!l || i + l > n) goto out;
        const uint32_t *a = w + i + 1;
        if (op == OP_BEGIN_INTERLOCK || op == OP_END_INTERLOCK) interlock_used = 1;
        else if (op == OP_EXECUTION_MODE && l >= 3 && a[1] >= 5366 && a[1] <= 5371) interlock_used = 1;
        else if (op == OP_CAPABILITY && l >= 2 && (a[0] == CAP_PIXEL_INTERLOCK || a[0] == CAP_SAMPLE_INTERLOCK || a[0] == CAP_RATE_INTERLOCK))
            interlock_declared = 1;
        else if (op == OP_CONSTANT && l == 4 && a[1] < bound) val[a[1]] = a[2];
        else if (op == OP_TYPE_ARRAY && l >= 4 && a[0] < bound) elem[a[0]] = a[1], len[a[0]] = a[2] < bound ? val[a[2]] : 0;
        else if (op == SPV_OP_TYPE_POINTER && l >= 4 && a[0] < bound) pointee[a[0]] = a[2];
        else if (op == SPV_OP_VARIABLE && l >= 4 && a[1] < bound) var_type[a[1]] = a[0] < bound ? pointee[a[0]] : 0;
        else if (op == OP_MEMBER_DECORATE && l >= 5 && a[2] == SPV_DEC_BUILTIN && a[3] == BUILTIN_CULL) cull_struct = a[0], cull_member = a[1];
        else if (op == OP_MEMBER_DECORATE && l >= 5 && a[2] == SPV_DEC_BUILTIN && a[3] == BUILTIN_CLIP) clip_member = a[1];
        else if (op == OP_TYPE_STRUCT && cull_struct && a[0] == cull_struct && l >= 2) {
            members = l - 2;
            for (uint32_t m = 0; m < members && m < 8; m++) member_types[m] = a[1 + m];
        } else if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && l >= 5 && cull_struct && a[2] < bound) {
            uint32_t t = var_type[a[2]], k = 3;
            if (t < bound && t && elem[t] == cull_struct) k = 4;   // arrayed per-vertex block
            if ((t == cull_struct || k == 4) && k + 1 < l && a[k] < bound && val[a[k]] == cull_member) cull_struct = 0;
        }
        i += l;
    }
    VkCtx *c = vk_ctx();
    uint32_t limit = c ? c->props.limits.maxCombinedClipAndCullDistances : 8;
    if (cull_struct && members && members <= 8 && cull_member == members - 1 && clip_member < members) {
        uint32_t clip_t = member_types[clip_member], cull_t = member_types[cull_member];
        uint32_t clip_n = clip_t < bound ? len[clip_t] : 0, cull_n = cull_t < bound ? len[cull_t] : 0;
        cull_ok = clip_n + cull_n > limit && clip_n <= limit;
    }
    int strip = interlock_declared && !interlock_used;
    if (!strip && !cull_ok) goto out;
    o = malloc(n * 4);
    if (!o) goto out;
    uint64_t on = 5;
    memcpy(o, w, 5 * 4);
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        int drop = 0;
        if (strip && op == OP_CAPABILITY && (a[0] == CAP_PIXEL_INTERLOCK || a[0] == CAP_SAMPLE_INTERLOCK || a[0] == CAP_RATE_INTERLOCK)) drop = 1;
        if (strip && op == OP_EXTENSION && l >= 2 && !strncmp((const char *)a, "SPV_EXT_fragment_shader_interlock", (l - 1) * 4)) drop = 1;
        if (cull_ok && (op == OP_MEMBER_DECORATE || op == OP_MEMBER_NAME) && l >= 3 && a[0] == cull_struct && a[1] == cull_member) drop = 1;
        if (!drop) {
            memcpy(o + on, w + i, l * 4);
            if (cull_ok && op == OP_TYPE_STRUCT && a[0] == cull_struct) o[on] = (l - 1) << 16 | op;   // last member gone
            on += op == OP_TYPE_STRUCT && cull_ok && a[0] == cull_struct ? l - 1 : l;
        }
        i += l;
    }
    *out_n = on;
out:
    free(val), free(elem), free(len), free(pointee), free(var_type);
    return o;
}

// True when the module declares a GroupNonUniform* or SubgroupBallot-style capability (lane masks assume 32 lanes).
static int uses_subgroup_ops(const uint32_t *w, uint64_t words) {
    for (uint64_t i = 5; i < words;) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16;
        if (!n || i + n > words) return 0;
        if (op != 17) return 0;   // OpCapability; capabilities come first, stop at the first other instruction
        uint32_t cap = w[i + 1];
        if ((cap >= 61 && cap <= 68) || cap == 4423 || cap == 4431) return 1;   // NonUniform*, SubgroupBallotKHR, SubgroupVoteKHR
        i += n;
    }
    return 0;
}

// Guest code runs warp-wide operations (ballot, shuffle, vote) where the guest compiler had the warp reconverged, for
// example right after a loop that each lane leaves at a different iteration. Vulkan only promises the same lanes at the
// same point with the MaximallyReconvergesKHR execution mode. Without it a driver may run the lanes that left the loop
// early through the operations on their own, with a ballot or scan that sees only some of the lanes: a mesh program
// that hands out output slots from one could give two primitives the same slot. This adds the extension and the mode
// for each entry point of a module that declares subgroup capabilities. NULL: nothing to change (or no memory).
static uint32_t *spirv_reconverge(const uint32_t *w, uint64_t n, uint64_t *out_n) {
    enum { OP_EXTENSION = 10, OP_EXECUTION_MODE = 16, OP_CAPABILITY = 17, MODE_MAXIMALLY_RECONVERGES = 6023 };
    static const char ext[] = "SPV_KHR_maximal_reconvergence";
    const uint32_t ext_words = (uint32_t)(sizeof ext + 3) / 4;   // the string with its terminating zero, padded
    if (n < 5 || !uses_subgroup_ops(w, n)) return NULL;
    uint64_t caps_end = 0, eps_end = 0;
    uint32_t eps[16], neps = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        if (!l || i + l > n) return NULL;
        if (op == OP_CAPABILITY) caps_end = i + l;
        else if (op == SPV_OP_ENTRY_POINT && l >= 3) {
            if (neps == 16) return NULL;
            eps[neps++] = w[i + 2], eps_end = i + l;
        } else if (op == OP_EXECUTION_MODE && l >= 3 && w[i + 2] == MODE_MAXIMALLY_RECONVERGES) return NULL;
        else if (op == SPV_OP_FUNCTION) break;
        i += l;
    }
    if (!caps_end || !neps || eps_end < caps_end) return NULL;
    uint64_t on = n + 1 + ext_words + 3ull * neps;
    uint32_t *o = malloc(on * 4);
    if (!o) return NULL;
    uint64_t p = 0;
    memcpy(o, w, caps_end * 4), p = caps_end;
    o[p++] = (1 + ext_words) << 16 | OP_EXTENSION;
    memset(o + p, 0, ext_words * 4);
    memcpy(o + p, ext, sizeof ext), p += ext_words;
    memcpy(o + p, w + caps_end, (eps_end - caps_end) * 4), p += eps_end - caps_end;
    for (uint32_t k = 0; k < neps; k++) o[p++] = 3u << 16 | OP_EXECUTION_MODE, o[p++] = eps[k], o[p++] = MODE_MAXIMALLY_RECONVERGES;
    memcpy(o + p, w + eps_end, (n - eps_end) * 4), p += n - eps_end;
    *out_n = p;
    return o;
}

// Maxwell computes in IEEE 754, and the game's shaders rely on it: its compiler writes an unordered comparison as
// `x > c || isnan(x)`, a normalisation of a zero-length vector gives an infinity that a later comparison sorts out, and
// integer masks such as 0xffffffff travel through float moves and selects. The translator's own guards are isnan
// tests as well (fragment outputs, min, max and clamp). Without the SignedZeroInfNanPreserve execution mode, SPIR-V
// lets the driver assume that no float is NaN or infinite, and AMD's compilers act on it: on an RX 9070 XT a select
// between 1 and the bits 0xffffffff, converted to float as a signed integer, gave 2^32 instead of -1 and blew the
// shadow casters up into huge triangles (issue #156); RX 6000 cards drew Navi, and Link's skin next to her, with black
// and white blocks (#41, #110); and AMD GPUs drew Navi's sparkles but not her orb, whose light sits at her own centre.
// NVIDIA's driver keeps the IEEE results without the mode. This adds the capability, the extension (the translator
// writes SPIR-V 1.3) and the mode for each entry point: 32-bit floats always, 16-bit and 64-bit ones where the module
// declares Float16 or Float64 and `widths` (NP_*) allows them. NULL: nothing to change (the module has the mode, or no
// 32-bit width is allowed, or no memory).
static uint32_t *spirv_nan_preserve(const uint32_t *w, uint64_t n, uint32_t widths, uint64_t *out_n) {
    enum { OP_EXTENSION = 10, OP_EXECUTION_MODE = 16, OP_CAPABILITY = 17, CAP_FLOAT16 = 9, CAP_FLOAT64 = 10,
           CAP_PRESERVE = 4466, MODE_PRESERVE = 4461 };
    static const char ext[] = "SPV_KHR_float_controls";
    const uint32_t ext_words = (uint32_t)(sizeof ext + 3) / 4;   // the string with its terminating zero, padded
    if (n < 5 || !(widths & NP_32)) return NULL;
    uint64_t caps_end = 0, eps_end = 0;
    uint32_t eps[16], neps = 0, use = NP_32;
    int has_cap = 0, has_ext = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        if (!l || i + l > n) return NULL;
        if (op == OP_CAPABILITY && l >= 2) {
            caps_end = i + l;
            if (w[i + 1] == CAP_FLOAT16) use |= NP_16;
            else if (w[i + 1] == CAP_FLOAT64) use |= NP_64;
            else if (w[i + 1] == CAP_PRESERVE) has_cap = 1;
        } else if (op == OP_EXTENSION && (l - 1) * 4 >= sizeof ext && !memcmp(w + i + 1, ext, sizeof ext)) {
            has_ext = 1;
        } else if (op == SPV_OP_ENTRY_POINT && l >= 3) {
            if (neps == 16) return NULL;
            eps[neps++] = w[i + 2], eps_end = i + l;
        } else if (op == OP_EXECUTION_MODE && l >= 3 && w[i + 2] == MODE_PRESERVE) {
            return NULL;
        } else if (op == SPV_OP_FUNCTION) {
            break;
        }
        i += l;
    }
    if (!caps_end || !neps || eps_end < caps_end) return NULL;
    use &= widths;
    uint32_t nw = 0, width[3];
    if (use & NP_16) width[nw++] = 16;
    width[nw++] = 32;
    if (use & NP_64) width[nw++] = 64;
    uint64_t on = n + (has_cap ? 0 : 2) + (has_ext ? 0 : 1 + ext_words) + 4ull * nw * neps;
    uint32_t *o = malloc(on * 4);
    if (!o) return NULL;
    uint64_t p = 0;
    memcpy(o, w, caps_end * 4), p = caps_end;
    if (!has_cap) o[p++] = 2u << 16 | OP_CAPABILITY, o[p++] = CAP_PRESERVE;
    if (!has_ext) {
        o[p++] = (1 + ext_words) << 16 | OP_EXTENSION;
        memset(o + p, 0, ext_words * 4);
        memcpy(o + p, ext, sizeof ext), p += ext_words;
    }
    memcpy(o + p, w + caps_end, (eps_end - caps_end) * 4), p += eps_end - caps_end;
    for (uint32_t k = 0; k < neps; k++)
        for (uint32_t j = 0; j < nw; j++)
            o[p++] = 4u << 16 | OP_EXECUTION_MODE, o[p++] = eps[k], o[p++] = MODE_PRESERVE, o[p++] = width[j];
    memcpy(o + p, w + eps_end, (n - eps_end) * 4), p += n - eps_end;
    *out_n = p;
    return o;
}

// NaN tests that no compiler can drop (P.nan_bits). The game's shaders and the translator's own guards test for NaN with
// OpIsNan: 48,717 of an install's 54,959 translated modules, 26 tests each on average (the game's unordered
// comparisons, and the guard that keeps NaN out of every float colour output, patch 0027, so that one NaN texel does not
// spread through bloom and blur into blocks of black and white). SignedZeroInfNanPreserve asks the driver to keep NaN,
// but whether a compiler that reasons about floats keeps an isnan through every optimisation is up to it. The reports
// of black and white blocks, on Navi, on Link next to her and on bright surfaces seen from inside a house, come from
// RX 5000 and 6000 cards (#41, #110, #117), whose Windows driver compiles with "AMD proprietary shader compiler"
// (vk_caps.amd_sc); RDNA 3 and 4 (LLPC) draw the same scenes clean. It is not known whether that compiler keeps the
// mode: on it each float32 test becomes integer arithmetic on the bits, which means the same under any float
// assumption:
//   isnan(x)  ->  (bitcast<uint>(x) & 0x7fffffff) > 0x7f800000
// for scalars and vectors of 32-bit floats; tests of 16- and 64-bit floats stay. A float32 select with a constant
// operand whose bits are a NaN (an integer mask such as 0xffffffff that the translator types as float) becomes a
// select of the bits (issue #156: on an RX 9070 XT without the mode such a select between 1 and -1 gave 2^32).
// NULL: nothing to change (or no memory); *tests and *selects count what changed.
static uint32_t *spirv_nan_bits(const uint32_t *w, uint64_t n, uint64_t *out_n, uint32_t *tests, uint32_t *selects) {
    enum { OP_UNDEF = 1, OP_TYPE_BOOL = 20, OP_SPEC_CONSTANT = 50, OP_SPEC_CONSTANT_COMPOSITE = 51, OP_STORE = 62,
           OP_BITCAST = 124, OP_IS_NAN = 156, OP_SELECT = 169, OP_UGREATER = 172, OP_BITWISE_AND = 199 };
    *tests = *selects = 0;
    if (n < 5 || w[3] >= (1u << 22)) return NULL;
    uint32_t bound = w[3];
    uint32_t *rtype = calloc(bound, sizeof *rtype), *vec_comp = calloc(bound, sizeof *vec_comp);
    uint8_t *f32 = calloc(bound, 1), *vec_n = calloc(bound, 1), *nan_const = calloc(bound, 1);
    uint32_t *o = NULL;
    if (!rtype || !vec_comp || !f32 || !vec_n || !nan_const) goto out;
    uint32_t tu = 0, uvec[5] = {0};   // uint32 and its vectors by component count
    uint64_t fn_at = 0;
    int vec_used[5] = {0};
    // Ids that are not results of the instruction's second word, inside functions: first assignment wins anyway, as
    // a definition comes before its uses (OpPhi operands aside, which this never reads).
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        if (!l || i + l > n) goto out;
        const uint32_t *a = w + i + 1;
        if (!fn_at) {
            if (op == SPV_OP_FUNCTION) fn_at = i;
            else if (op == SPV_OP_TYPE_FLOAT && l >= 3 && a[0] < bound && a[1] == 32) f32[a[0]] = 1;
            else if (op == SPV_OP_TYPE_INT && l >= 4 && a[1] == 32 && a[2] == 0 && !tu) tu = a[0];
            else if (op == SPV_OP_TYPE_VECTOR && l >= 4 && a[0] < bound && a[2] >= 2 && a[2] <= 4) {
                vec_comp[a[0]] = a[1], vec_n[a[0]] = (uint8_t)a[2];
                if (tu && a[1] == tu && !uvec[a[2]]) uvec[a[2]] = a[0];
            } else if ((op == SPV_OP_CONSTANT || op == SPV_OP_CONSTANT_COMPOSITE || op == SPV_OP_CONSTANT_NULL ||
                        op == OP_UNDEF || op == SPV_OP_VARIABLE || op == OP_SPEC_CONSTANT ||
                        op == OP_SPEC_CONSTANT_COMPOSITE) && l >= 3 && a[1] < bound) {
                rtype[a[1]] = a[0];
                if (op == SPV_OP_CONSTANT && l == 4 && a[0] < bound && f32[a[0]] && (a[2] & 0x7fffffffu) > 0x7f800000u)
                    nan_const[a[1]] = 1;
            }
        }
        if (fn_at) {
            int no_result = op == OP_STORE || op == 63 || op == 64 || op == 8 || op == 317 || (op >= 246 && op <= 255) ||
                            op == 99 || op == 224 || op == 225 || op == 228 || op == 56 || op == 218 || op == 219 ||
                            op == 5294 || op == 5295 || op == 4416 || op == 5380 || op == 5364 || op == 5365;
            if (!no_result && op != SPV_OP_FUNCTION && l >= 3 && a[1] < bound && !rtype[a[1]]) rtype[a[1]] = a[0];
            if (op == OP_IS_NAN && l == 4 && a[2] < bound) {
                uint32_t t = rtype[a[2]];
                if (t < bound && f32[t]) ++*tests;
                else if (t < bound && vec_n[t] && vec_comp[t] < bound && f32[vec_comp[t]]) ++*tests, vec_used[vec_n[t]] = 1;
            } else if (op == OP_SELECT && l == 6 && a[0] < bound && f32[a[0]] && a[3] < bound && a[4] < bound &&
                       (nan_const[a[3]] || nan_const[a[4]])) {
                ++*selects;
            }
        }
        i += l;
    }
    if (!fn_at || (!*tests && !*selects)) goto out;
    // the declarations go right before the first function: uint32 and its vectors where missing, then the constants
    uint32_t next = bound, decl[64], nd = 0;
    if (!tu) tu = next++, decl[nd++] = 4u << 16 | SPV_OP_TYPE_INT, decl[nd++] = tu, decl[nd++] = 32, decl[nd++] = 0;
    uint32_t c_mag = next++, c_inf = next++, cv_mag[5] = {0}, cv_inf[5] = {0};
    for (int k = 2; k <= 4; k++)
        if (vec_used[k] && !uvec[k])
            uvec[k] = next++, decl[nd++] = 4u << 16 | SPV_OP_TYPE_VECTOR, decl[nd++] = uvec[k], decl[nd++] = tu, decl[nd++] = (uint32_t)k;
    decl[nd++] = 4u << 16 | SPV_OP_CONSTANT, decl[nd++] = tu, decl[nd++] = c_mag, decl[nd++] = 0x7fffffffu;
    decl[nd++] = 4u << 16 | SPV_OP_CONSTANT, decl[nd++] = tu, decl[nd++] = c_inf, decl[nd++] = 0x7f800000u;
    for (int k = 2; k <= 4; k++) {
        if (!vec_used[k]) continue;
        cv_mag[k] = next++, cv_inf[k] = next++;
        decl[nd++] = (uint32_t)(3 + k) << 16 | SPV_OP_CONSTANT_COMPOSITE, decl[nd++] = uvec[k], decl[nd++] = cv_mag[k];
        for (int j = 0; j < k; j++) decl[nd++] = c_mag;
        decl[nd++] = (uint32_t)(3 + k) << 16 | SPV_OP_CONSTANT_COMPOSITE, decl[nd++] = uvec[k], decl[nd++] = cv_inf[k];
        for (int j = 0; j < k; j++) decl[nd++] = c_inf;
    }
    uint64_t cap = n + nd + 10ull * *tests + 12ull * *selects;
    o = malloc(cap * 4);
    if (!o) goto out;
    memcpy(o, w, fn_at * 4);
    uint64_t p = fn_at;
    memcpy(o + p, decl, nd * 4), p += nd;
    for (uint64_t i = fn_at; i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        uint32_t t = op == OP_IS_NAN && l == 4 && a[2] < bound ? rtype[a[2]] : 0;
        int k = t && t < bound ? (f32[t] ? 1 : vec_n[t] && vec_comp[t] < bound && f32[vec_comp[t]] ? vec_n[t] : 0) : 0;
        if (k) {
            uint32_t ut = k == 1 ? tu : uvec[k], mag = k == 1 ? c_mag : cv_mag[k], inf = k == 1 ? c_inf : cv_inf[k];
            uint32_t bits = next++, low = next++;
            o[p++] = 4u << 16 | OP_BITCAST, o[p++] = ut, o[p++] = bits, o[p++] = a[2];
            o[p++] = 5u << 16 | OP_BITWISE_AND, o[p++] = ut, o[p++] = low, o[p++] = bits, o[p++] = mag;
            o[p++] = 5u << 16 | OP_UGREATER, o[p++] = a[0], o[p++] = a[1], o[p++] = low, o[p++] = inf;
        } else if (op == OP_SELECT && l == 6 && a[0] < bound && f32[a[0]] && a[3] < bound && a[4] < bound &&
                   (nan_const[a[3]] || nan_const[a[4]])) {
            uint32_t x = next++, y = next++, s = next++;
            o[p++] = 4u << 16 | OP_BITCAST, o[p++] = tu, o[p++] = x, o[p++] = a[3];
            o[p++] = 4u << 16 | OP_BITCAST, o[p++] = tu, o[p++] = y, o[p++] = a[4];
            o[p++] = 6u << 16 | OP_SELECT, o[p++] = tu, o[p++] = s, o[p++] = a[2], o[p++] = x, o[p++] = y;
            o[p++] = 4u << 16 | OP_BITCAST, o[p++] = a[0], o[p++] = a[1], o[p++] = s;
        } else {
            memcpy(o + p, w + i, l * 4), p += l;
        }
        i += l;
    }
    o[3] = next;
    *out_n = p;
out:
    free(rtype), free(vec_comp), free(f32), free(vec_n), free(nan_const);
    return o;
}

static int spirv_mark_list_test(const char *list);   // texture pool marks, below

// --selftest (main.c): spirv_nan_preserve on a small fragment module, without a device. Returns the number of failures.
int vk_pipeline_spirv_selftest(void) {
    enum { CAP = 2u << 16 | 17, MODE = 4u << 16 | 16 };
    static const uint32_t head[] = {0x07230203, 0x00010300, 0, 5, 0, CAP, 1, CAP, 10};   // Shader, Float64
    static const uint32_t body[] = {
        3u << 16 | 14, 0, 1,                     // OpMemoryModel Logical GLSL450
        5u << 16 | 15, 4, 1, 0x6e69616d, 0,      // OpEntryPoint Fragment %1 "main"
        3u << 16 | 16, 1, 7,                     // OpExecutionMode %1 OriginUpperLeft
        2u << 16 | 19, 2,                        // %2 = OpTypeVoid
        3u << 16 | 33, 3, 2,                     // %3 = OpTypeFunction %2
        5u << 16 | 54, 2, 1, 0, 3,               // %1 = OpFunction %2 None %3
        2u << 16 | 248, 4, 1u << 16 | 253, 1u << 16 | 56};   // OpLabel, OpReturn, OpFunctionEnd
    enum { NH = sizeof head / 4, NB = sizeof body / 4, EP_END = 8 };   // EP_END: body words up to the entry point's end
    uint32_t in[NH + NB], want[NH + NB + 32];
    memcpy(in, head, sizeof head), memcpy(in + NH, body, sizeof body);
    int failures = 0;
    // widths: what the device allows; f64: the module declares Float64; w64: a 64-bit mode is expected
    for (int t = 0; t < 3; t++) {
        uint32_t widths = t == 0 ? NP_16 | NP_32 | NP_64 : NP_32, n = NH + NB, p = 0;
        int f64 = t != 2, w64 = t == 0;
        if (!f64) in[NH - 1] = 0;   // Matrix in place of Float64
        memcpy(want, in, NH * 4), p = NH;
        want[p++] = CAP, want[p++] = 4466;
        want[p++] = 7u << 16 | 10;
        memset(want + p, 0, 24), memcpy(want + p, "SPV_KHR_float_controls", 23), p += 6;
        memcpy(want + p, body, EP_END * 4), p += EP_END;
        want[p++] = MODE, want[p++] = 1, want[p++] = 4461, want[p++] = 32;
        if (w64) want[p++] = MODE, want[p++] = 1, want[p++] = 4461, want[p++] = 64;
        memcpy(want + p, body + EP_END, (NB - EP_END) * 4), p += NB - EP_END;
        uint64_t on = 0, again_n = 0;
        uint32_t *o = spirv_nan_preserve(in, n, widths, &on);
        uint32_t *again = o ? spirv_nan_preserve(o, on, widths, &again_n) : NULL;
        if (!o || on != p || memcmp(o, want, p * 4) || again) {
            printf("spirv: SignedZeroInfNanPreserve rewrite, case %d: wrong output\n", t);
            failures++;
        }
        free(o), free(again);
    }
    if (spirv_nan_preserve(in, NH + NB, NP_16 | NP_64, &(uint64_t){0})) {
        printf("spirv: SignedZeroInfNanPreserve rewrite: a module changed without the 32-bit width\n");
        failures++;
    }
    printf("spirv: SignedZeroInfNanPreserve rewrite: %s\n", failures ? "FAIL" : "ok");
    {   // spirv_nan_bits: a scalar and a vec2 isnan, a select on a NaN constant; no uint32 type in the module
        static const uint32_t m[] = {
            0x07230203, 0x00010300, 0, 20, 0,
            2u << 16 | 17, 1,                         // OpCapability Shader
            3u << 16 | 14, 0, 1,                      // OpMemoryModel Logical GLSL450
            5u << 16 | 15, 5, 1, 0x6e69616d, 0,       // OpEntryPoint GLCompute %1 "main"
            2u << 16 | 19, 2,                         // %2 void
            3u << 16 | 33, 3, 2,                      // %3 fn
            3u << 16 | 22, 4, 32,                     // %4 float
            2u << 16 | 20, 5,                         // %5 bool
            4u << 16 | 23, 6, 4, 2,                   // %6 vec2
            4u << 16 | 23, 7, 5, 2,                   // %7 bvec2
            4u << 16 | 43, 4, 8, 0xffffffffu,         // %8 = NaN bits
            4u << 16 | 43, 4, 9, 0x3f800000u,         // %9 = 1.0
            5u << 16 | 44, 6, 10, 8, 9,               // %10 = vec2(%8, %9)
            3u << 16 | 41, 5, 11,                     // %11 = OpConstantTrue
            5u << 16 | 54, 2, 1, 0, 3,                // %1 = OpFunction
            2u << 16 | 248, 12,                       // OpLabel
            4u << 16 | 156, 5, 13, 9,                 // %13 = OpIsNan bool %9
            4u << 16 | 156, 7, 14, 10,                // %14 = OpIsNan bvec2 %10
            6u << 16 | 169, 4, 15, 11, 9, 8,          // %15 = OpSelect float %11 %9 %8
            1u << 16 | 253, 1u << 16 | 56};           // OpReturn, OpFunctionEnd
        uint64_t on = 0;
        uint32_t tests = 0, sels = 0, isnan_left = 0, ugt = 0, int32 = 0, bitcasts = 0;
        uint32_t *o = spirv_nan_bits(m, sizeof m / 4, &on, &tests, &sels);
        for (uint64_t i = 5; o && i < on;) {
            uint32_t op = o[i] & 0xffff, l = o[i] >> 16;
            if (!l) break;
            isnan_left += op == 156, ugt += op == 172, int32 += op == 21 && o[i + 2] == 32, bitcasts += op == 124;
            i += l;
        }
        int bad = !o || tests != 2 || sels != 1 || isnan_left || ugt != 2 || int32 != 1 || bitcasts != 5 || o[3] <= 20;
        printf("spirv: integer NaN tests rewrite: %s\n", bad ? "FAIL" : "ok");
        failures += bad;
        free(o);
    }
    const char *list = cfg_get("NX2NAVI_SPV_MARK_TEST");
    if (list && *list) failures += spirv_mark_list_test(list);
    return failures;
}

// Chain {size 32} into a stage create info when the stage uses subgroup ops and the driver allows the pin.
static void pin_subgroup32(VkPipelineShaderStageCreateInfo *st, VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *rq,
                           const uint32_t *w, uint64_t words) {
    if (!(P.sg32_stages & st->stage) || !w || !uses_subgroup_ops(w, words)) return;
    *rq = (VkPipelineShaderStageRequiredSubgroupSizeCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = vk_caps.sg_size};
    st->pNext = rq;
}

// The x of a compute module's LocalSize (OpExecutionMode 17), 0 when it has none.
static uint32_t local_size_x(const uint32_t *w, uint64_t words) {
    for (uint64_t i = 5; w && i < words;) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16;
        if (!n || i + n > words) return 0;
        if (op == 16 && n >= 6 && w[i + 2] == 17) return w[i + 3];
        if (op == 54) return 0;   // OpFunction: the execution modes are all before it
        i += n;
    }
    return 0;
}

// Pool arrays (the translator's bindless texture and sampler arrays of VK_POOL_TEX / VK_POOL_SAMP elements, found by
// their types) move to set 2, at the binding of their view kind (vk.h VK_POOL_KINDS) or VK_POOL_KINDS for samplers;
// the kinds used are noted in vk_pool_kinds_used so the replay fills them. NULL: nothing to move (or no memory).
static uint32_t *spirv_pool_rebind(const uint32_t *w, uint64_t n) {
    enum { OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLER = 26, OP_TYPE_ARRAY_ = 28 };
    uint32_t bound = n >= 5 && w[3] < (1u << 22) ? w[3] : 0;
    if (!bound) return NULL;
    int32_t *kind = malloc(bound * sizeof *kind);   // per id: image kind, 100 sampler, -1 other (types, then variables)
    uint32_t *cval = calloc(bound, sizeof *cval);
    if (!kind || !cval) return free(kind), free(cval), NULL;
    memset(kind, 0xff, bound * sizeof *kind);
    // Pool variables and their binding decorations are module declarations, all before the first function.
    int any = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n || op == SPV_OP_FUNCTION) break;
        if (op == OP_TYPE_IMAGE && len >= 9 && w[i + 1] < bound && w[i + 7] == 1) {   // sampled image
            uint32_t dim = w[i + 3], depth = w[i + 4], arrayed = w[i + 5], ms = w[i + 6];
            if (dim <= 3 && !ms) kind[w[i + 1]] = (int32_t)((depth == 1 ? 8 : 0) + dim * 2 + (arrayed ? 1 : 0));
        } else if (op == OP_TYPE_SAMPLER && len >= 2 && w[i + 1] < bound) {
            kind[w[i + 1]] = 100;
        } else if (op == SPV_OP_CONSTANT && len >= 4 && w[i + 2] < bound) {
            cval[w[i + 2]] = w[i + 3];
        } else if (op == OP_TYPE_ARRAY_ && len >= 4 && w[i + 1] < bound && w[i + 2] < bound && w[i + 3] < bound) {
            int32_t k = kind[w[i + 2]];
            uint32_t want = k == 100 ? VK_POOL_SAMP : VK_POOL_TEX;
            // A translation carried over from a translator whose pool was VK_POOL_TEX_CARRIED long (gfx_shader.c,
            // carry) is a pool array too: left in its own set, its textures were never bound and the scene drew
            // with empty images until the background pass translated it again. It indexes the first
            // VK_POOL_TEX_CARRIED entries of the larger binding, which Vulkan allows.
            int pool = k >= 0 && (cval[w[i + 3]] == want || (k != 100 && cval[w[i + 3]] == VK_POOL_TEX_CARRIED));
            kind[w[i + 1]] = pool ? k + 1000 : -1;   // + 1000: a pool array
        } else if (op == SPV_OP_TYPE_POINTER && len >= 4 && w[i + 1] < bound && w[i + 3] < bound) {
            kind[w[i + 1]] = kind[w[i + 3]] >= 1000 ? kind[w[i + 3]] : -1;
        } else if (op == SPV_OP_VARIABLE && len >= 4 && w[i + 1] < bound && w[i + 2] < bound) {
            kind[w[i + 2]] = kind[w[i + 1]] >= 1000 ? kind[w[i + 1]] + 1000 : -1;   // + 2000: a pool variable
            if (kind[w[i + 2]] >= 2000) any = 1;
        }
        i += len;
    }
    uint32_t *o = NULL;
    if (any && (o = malloc(n * 4))) {
        memcpy(o, w, n * 4);
        uint32_t used = 0;
        for (uint64_t i = 5; i < n;) {
            uint32_t op = o[i] & 0xffff, len = o[i] >> 16;
            if (!len || i + len > n || op == SPV_OP_FUNCTION) break;
            if (op == SPV_OP_DECORATE && len >= 4 && o[i + 1] < bound && kind[o[i + 1]] >= 2000) {
                int32_t k = kind[o[i + 1]] - 2000;
                if (o[i + 2] == SPV_DEC_DESCRIPTOR_SET) o[i + 3] = 2;
                else if (o[i + 2] == SPV_DEC_BINDING) o[i + 3] = k == 100 ? VK_POOL_KINDS : (uint32_t)k;
                if (k != 100) used |= 1u << k;
            }
            i += len;
        }
        __atomic_fetch_or(&vk_pool_kinds_used, used, __ATOMIC_RELAXED);
    }
    free(kind), free(cval);
    return o;
}

// ---- texture pool marks (NX2NAVI_TEX_DISUSE, default on; vk_texture.c "disuse" uses them)
// Ocarina samples nearly every texture through the bindless pool: a shader reads a handle from memory and indexes the
// pool array with it, so the CPU never learns which of the registered textures a frame samples, and every registered
// texture counts as in use (vk_texture.c idle). The game keeps the textures of the areas it has loaded registered, so
// none of them could go, and their memory, which the driver charges to the system's commit, grew to 6 GiB in a session.
// This rewrite gives each module that indexes the texture pool a storage buffer at set 2, binding VK_POOL_MARK_BINDING
// (a uint per pool slot, vk_exec_draw.c "pool marks") and, after every access chain into a pool texture array, a store
// of 1 into the slot it indexes. The stores of one warp to one slot are one memory transaction. The replay copies the
// buffer out and clears it every few submissions; slots marked mean textures sampled. A module that indexes the pool
// and cannot be marked (SPIR-V before 1.3 or after 1.6, a stage whose stores the device does not allow, no memory) is
// counted in vk_pool_marks_missed, which keeps vk_texture.c from evicting by disuse: an unmarked shader could sample a
// texture the marks call unused. make_module logs the first few by stage, version, size, hash and reason.
volatile int32_t vk_pool_marks_missed;
static int marks_on = -1, marks_vs_ok, marks_fs_ok;

int vk_pool_marks_enabled(void) {
    if (marks_on < 0) {
        marks_on = rt_knob("TEX_DISUSE", "1") >= 1;   // 2: marks and stand-ins, nothing freed (vk_texture.c "disuse")
        VkCtx *c = vk_ctx();
        if (c) {
            VkPhysicalDeviceFeatures f;
            vkGetPhysicalDeviceFeatures(c->phys, &f);   // vk_device.c enables every feature the GPU reports
            marks_vs_ok = f.vertexPipelineStoresAndAtomics, marks_fs_ok = f.fragmentStoresAndAtomics;
        }
    }
    return marks_on;
}

// A fragment shader that stores to memory runs before the depth test unless it declares EarlyFragmentTests: the GPU
// must run it for fragments the test then rejects, as the stores are visible. The marks would so take the early depth
// rejection from every marked fragment shader and make overdraw cost full shading. With early_ok, a fragment shader
// whose result is the same under early tests gets EarlyFragmentTests along with its marks: one that discards, writes
// its depth, sample mask or stencil reference, or already writes memory does not, as early tests would change what it
// does. The caller passes early_ok only for a pipeline without alpha to coverage, which also drops samples after the
// shader (ensure_init keeps alpha to coverage out of the dynamic state while marks are on, so a pipeline knows).
// A fragment shader's marks are stored where it ends, not where it samples. A store between a sample's coordinates and
// the sample takes the quad's helper lanes out of the code in between on AMD's driver for Polaris: their coordinates
// stayed stale, the derivatives at a triangle's edge were garbage, and edge pixels came out dark (the lines along the
// paths' triangles since 0.7.0). Each access only notes its slot in a Private variable (a register); before each
// return of the entry function, and before a kill, terminate or demote anywhere, every variable is stored to its slot,
// a variable no access set going to the spare slot past the pool (vk_exec_draw.c pool_marks_make). An access in a loop
// or outside the entry function can run more than once with other slots: it notes them in a ring of RING Private
// variables, so a loop's last RING slots are all marked.
// *status: 1 marked, 0 no pool texture access to mark, -1 one that could not be marked (*why says what stopped it).
// NULL unless marked.
static uint32_t *spirv_pool_mark(const uint32_t *w, uint64_t n, int early_ok, uint64_t *out_n, int *status,
                                 const char **why) {
    enum { OP_TYPE_RUNTIME_ARRAY = 29, OP_TYPE_STRUCT_ = 30, OP_ACCESS_CHAIN_ = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66,
           OP_STORE_ = 62, OP_MEMBER_DECORATE_ = 72, OP_DECORATION_GROUP = 73, OP_GROUP_DECORATE = 74,
           OP_GROUP_MEMBER_DECORATE = 75, OP_DECORATE_ID = 332, OP_DECORATE_STRING = 5632, OP_MEMBER_DECORATE_STRING = 5633,
           OP_EXECUTION_MODE = 16, OP_EXECUTION_MODE_ID = 331, OP_IMAGE_WRITE = 99, OP_ATOMIC_FIRST = 227,
           OP_ATOMIC_LAST = 242, OP_KILL = 252, OP_TERMINATE_INVOCATION = 4416, OP_DEMOTE_TO_HELPER = 5380,
           EM_EARLY_FRAGMENT_TESTS = 9, BI_SAMPLE_MASK = 20, BI_FRAG_DEPTH = 22, BI_FRAG_STENCIL_REF = 5014,
           SC_STORAGE_BUFFER = 12, DEC_BLOCK = 2, DEC_ARRAY_STRIDE = 6, DEC_OFFSET_ = 35, OP_TYPE_BOOL = 20,
           OP_LOAD_ = 61, OP_BITCAST = 124, OP_SELECT_ = 169, OP_INOT_EQUAL = 171, OP_LOOP_MERGE = 246, OP_LABEL_ = 248,
           OP_RETURN = 253, OP_RETURN_VALUE = 254, SC_PRIVATE = 6, LOOP_DEPTH = 64, OP_TYPE_ARRAY_ = 28, OP_IADD = 128,
           OP_BITWISE_AND = 199, OP_ULESS_THAN = 176, OP_BRANCH = 249, OP_BRANCH_CONDITIONAL = 250, OP_PHI = 245 };
    *status = 0, *why = NULL;
    if (n < 5) return NULL;
    uint32_t bound = w[3];
    if (!bound || bound > (1u << 22)) return NULL;
    uint32_t *bind = calloc(bound, sizeof *bind);   // per id: its Binding + 1 when its DescriptorSet is 2, else 0
    uint8_t *set2 = calloc(bound, 1);
    if (!bind || !set2) return free(bind), free(set2), *status = -1, *why = "no memory", NULL;
    uint64_t ann_end = 0, fn_start = 0, em_end = 0;   // em_end: past the entry points and execution modes
    uint32_t uint_t = 0, c0 = 0, c1 = 0, ptr_u = 0, fs_entry = 0, int_t = 0, bool_t = 0;
    int vertex_stage = 0, fragment_stage = 0, entries = 0, early = early_ok;
    uint32_t *rtype = calloc(bound, sizeof *rtype);   // per id: its result type, where the defer path needs it
    uint8_t *is_type = calloc(bound, 1);
    if (!rtype || !is_type)
        return free(bind), free(set2), free(rtype), free(is_type), *status = -1, *why = "no memory", NULL;
#define POOL_MARK_FREE() (free(bind), free(set2), free(rtype), free(is_type), free(site_def))
    // per site: 0 stored in place; else deferred, bit 1 the index is signed (a bitcast), bit 2 a ring (in a loop or
    // outside the entry function)
    uint8_t *site_def = NULL;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        if (!l || i + l > n) return POOL_MARK_FREE(), *status = -1, *why = "an instruction past the module's end", NULL;
        if (op == SPV_OP_FUNCTION) { fn_start = i; break; }
        if (op == SPV_OP_ENTRY_POINT || op == OP_EXECUTION_MODE || op == OP_EXECUTION_MODE_ID) em_end = i + l;
        if (op == SPV_OP_ENTRY_POINT && l >= 3) {
            entries++;
            if (w[i + 1] <= 3) vertex_stage = 1;   // Vertex, TessellationControl, TessellationEvaluation, Geometry
            else if (w[i + 1] == 4) fragment_stage = 1, fs_entry = w[i + 2];
        } else if (op == OP_EXECUTION_MODE && l >= 3 && w[i + 2] == EM_EARLY_FRAGMENT_TESTS) {
            early = 0;   // has them already
        } else if (op == SPV_OP_DECORATE && l >= 4 && w[i + 1] < bound) {
            if (w[i + 2] == SPV_DEC_DESCRIPTOR_SET && w[i + 3] == 2) set2[w[i + 1]] = 1;
            else if (w[i + 2] == SPV_DEC_BINDING) bind[w[i + 1]] = w[i + 3] + 1;
            else if (w[i + 2] == SPV_DEC_BUILTIN &&
                     (w[i + 3] == BI_SAMPLE_MASK || w[i + 3] == BI_FRAG_DEPTH || w[i + 3] == BI_FRAG_STENCIL_REF))
                early = 0;
        } else if (op == SPV_OP_VARIABLE && l >= 4 && w[i + 3] == SC_STORAGE_BUFFER) {
            early = 0;   // the guest's own storage buffers: its stores would change under early tests
        } else if (op == SPV_OP_TYPE_INT && l == 4 && w[i + 2] == 32 && w[i + 3] == 0) {
            uint_t = w[i + 1];
        } else if (op == SPV_OP_TYPE_INT && l == 4 && w[i + 2] == 32 && w[i + 3] == 1) {
            int_t = w[i + 1];
        } else if (op == OP_TYPE_BOOL && l == 2) {
            bool_t = w[i + 1];
        } else if (op == SPV_OP_CONSTANT && l == 4 && uint_t && w[i + 1] == uint_t) {
            if (w[i + 3] == 0 && !c0) c0 = w[i + 2];
            if (w[i + 3] == 1 && !c1) c1 = w[i + 2];
        } else if (op == SPV_OP_TYPE_POINTER && l == 4 && w[i + 2] == SC_STORAGE_BUFFER && uint_t && w[i + 3] == uint_t) {
            ptr_u = w[i + 1];
        }
        if (op == SPV_OP_DECORATE || op == OP_MEMBER_DECORATE_ || op == OP_DECORATION_GROUP || op == OP_GROUP_DECORATE ||
            op == OP_GROUP_MEMBER_DECORATE || op == OP_DECORATE_ID || op == OP_DECORATE_STRING || op == OP_MEMBER_DECORATE_STRING)
            ann_end = i + l;
        if (op >= 19 && op <= 39 && l >= 2 && w[i + 1] < bound) is_type[w[i + 1]] = 1;   // OpTypeVoid .. OpTypeForwardPointer
        else if (op == SPV_OP_CONSTANT && l >= 4 && w[i + 2] < bound) rtype[w[i + 2]] = w[i + 1];
        i += l;
    }
    // pool texture variables: set 2, a binding below VK_POOL_KINDS (spirv_pool_rebind); one already at the marks
    // binding means the module was marked before
    enum { RING = 8 };
    uint64_t sites = 0, deferred = 0, rings = 0, exits = 0;
    int marked = 0;
    for (uint32_t id = 0; id < bound; id++)
        if (set2[id] && bind[id] == VK_POOL_MARK_BINDING + 1) marked = 1;
    site_def = fn_start && !marked ? calloc((n - fn_start) / 5 + 1, 1) : NULL;   // an access chain is 5 words or more
    if (fn_start && !marked && !site_def) return POOL_MARK_FREE(), *status = -1, *why = "no memory", NULL;
    uint32_t loops[LOOP_DEPTH], fn = 0;
    int depth = 0;
    for (uint64_t i = fn_start; fn_start && !marked && i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        if (!l || i + l > n) break;
        if (l >= 3 && w[i + 1] < bound && is_type[w[i + 1]] && w[i + 2] < bound) rtype[w[i + 2]] = w[i + 1];
        if (op == SPV_OP_FUNCTION && l >= 3) fn = w[i + 2], depth = 0;
        else if (op == OP_LOOP_MERGE && l >= 3) depth < LOOP_DEPTH ? (void)(loops[depth++] = w[i + 1]) : (void)depth++;
        else if (op == OP_LABEL_ && l >= 2 && depth > 0 && depth <= LOOP_DEPTH && loops[depth - 1] == w[i + 1]) depth--;
        if ((op == OP_ACCESS_CHAIN_ || op == OP_IN_BOUNDS_ACCESS_CHAIN) && l >= 5 && w[i + 3] < bound && set2[w[i + 3]] &&
            bind[w[i + 3]] && bind[w[i + 3]] - 1 < VK_POOL_KINDS) {
            uint32_t ix = w[i + 4], t = ix < bound ? rtype[ix] : 0;
            if (fragment_stage && entries == 1 && fs_entry && t && (t == uint_t || t == int_t)) {
                int ring = fn != fs_entry || depth;
                site_def[sites] = (uint8_t)(1 | (t == int_t ? 2 : 0) | (ring ? 4 : 0));
                ring ? rings++ : deferred++;
            }
            sites++;
        } else if (op == OP_KILL || op == OP_TERMINATE_INVOCATION || op == OP_DEMOTE_TO_HELPER || op == OP_IMAGE_WRITE ||
                   (op >= OP_ATOMIC_FIRST && op <= OP_ATOMIC_LAST))
            early = 0;
        if (op == OP_KILL || op == OP_TERMINATE_INVOCATION || op == OP_DEMOTE_TO_HELPER ||
            (fn == fs_entry && (op == OP_RETURN || op == OP_RETURN_VALUE)))
            exits++;
        i += l;
    }
    if (marked || !sites) return POOL_MARK_FREE(), NULL;
    uint64_t stores = deferred + rings;   // what each way out stores: a variable, or a ring's entries in a loop
    if (stores && (!exits || exits * (deferred + 3 * rings) > 8192))   // no way out to store at, or code that would grow
        memset(site_def, 0, sites), deferred = rings = stores = 0;           // too much
    *why = w[1] < 0x00010300 || w[1] > 0x00010600 ? "a SPIR-V version before 1.3 or after 1.6"
         : !ann_end || !fn_start              ? "no decorations or no function"
         : vertex_stage && !marks_vs_ok       ? "the GPU allows no stores in vertex-pipeline shaders"
         : fragment_stage && !marks_fs_ok     ? "the GPU allows no stores in fragment shaders"
                                              : NULL;
    if (*why) return POOL_MARK_FREE(), *status = -1, NULL;
    early = early && fragment_stage && entries == 1 && em_end && em_end < ann_end;
    // From SPIR-V 1.4 on an entry point lists every global variable its functions use, not only the inputs and outputs
    // (the translator's mesh shaders are 1.4): each entry point gets the marks variable, a word more.
    int iface = w[1] >= 0x00010400;
    // the mode, a word per entry point, decorations, at most 30 words of new types and the variable, 9 a site; deferred:
    // a word per entry point and variable, 80 words of types and constants, then per site its variables and the store of
    // its slot (ring: 10 + 29), and per way out 29 words a variable and 8 * 29 a ring (its loop is 83)
    uint64_t on = n + 3 + (uint64_t)entries * (1 + deferred + 2 * rings) + 20 + 30 + 9 * sites + 80 + deferred * 12 +
                  rings * 40 + (deferred * 29 + rings * RING * 29) * exits;
    uint32_t *o = malloc(on * 4);
    if (!o) return POOL_MARK_FREE(), *status = -1, *why = "no memory", NULL;
    uint32_t next = bound;
    uint32_t new_uint = uint_t ? 0 : (uint_t = next++), new_c0 = c0 ? 0 : (c0 = next++), new_c1 = c1 ? 0 : (c1 = next++);
    uint32_t rta = next++, st = next++, ptr_st = next++, var = next++, new_ptr_u = ptr_u ? 0 : (ptr_u = next++);
    // deferred: the types, constants and variables (pv0: one per site; ra0, rc0: a ring and its count per ring site)
    uint32_t new_bool = 0, ptr_pv = 0, c_none = 0, c_spare = 0, pv0 = 0, ring_t = 0, ptr_ring = 0, ring_none = 0,
             cr[RING] = {0}, c_ring = 0, ra0 = 0, rc0 = 0;
    if (stores) {
        new_bool = bool_t ? 0 : (bool_t = next++);
        ptr_pv = next++, c_none = next++, c_spare = next++, pv0 = next, next += (uint32_t)deferred;
        if (rings) {
            ring_t = next++, ptr_ring = next++, ring_none = next++, c_ring = next++;
            for (int j = 0; j < RING; j++) cr[j] = next++;
            ra0 = next, next += (uint32_t)rings, rc0 = next, next += (uint32_t)rings;
        }
    }
    uint64_t p = 5;
    memcpy(o, w, 5 * 4);
    for (uint64_t i = 5; i < ann_end;) {   // the header up to the annotations' end: entry points widened, the mode added
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        if (early && i == em_end) o[p++] = 3u << 16 | OP_EXECUTION_MODE, o[p++] = fs_entry, o[p++] = EM_EARLY_FRAGMENT_TESTS;
        memcpy(o + p, w + i, l * 4);
        if (iface && op == SPV_OP_ENTRY_POINT && l >= 3) {
            uint32_t nv = 0, *at = o + p + l + 1;   // the marks buffer, then pv0.., ra0.. and rc0..
            o[p] = (uint32_t)(l + 1 + deferred + 2 * rings) << 16 | op, o[p + l] = var, p++;
            for (uint32_t k = 0; k < deferred; k++) at[nv++] = pv0 + k;
            for (uint32_t k = 0; k < rings; k++) at[nv++] = ra0 + k;
            for (uint32_t k = 0; k < rings; k++) at[nv++] = rc0 + k;
            p += nv;
        }
        p += l;
        i += l;
    }
    o[p++] = 4u << 16 | SPV_OP_DECORATE, o[p++] = rta, o[p++] = DEC_ARRAY_STRIDE, o[p++] = 4;
    o[p++] = 5u << 16 | OP_MEMBER_DECORATE_, o[p++] = st, o[p++] = 0, o[p++] = DEC_OFFSET_, o[p++] = 0;
    o[p++] = 3u << 16 | SPV_OP_DECORATE, o[p++] = st, o[p++] = DEC_BLOCK;
    o[p++] = 4u << 16 | SPV_OP_DECORATE, o[p++] = var, o[p++] = SPV_DEC_DESCRIPTOR_SET, o[p++] = 2;
    o[p++] = 4u << 16 | SPV_OP_DECORATE, o[p++] = var, o[p++] = SPV_DEC_BINDING, o[p++] = VK_POOL_MARK_BINDING;
    memcpy(o + p, w + ann_end, (fn_start - ann_end) * 4), p += fn_start - ann_end;
    if (new_uint) o[p++] = 4u << 16 | SPV_OP_TYPE_INT, o[p++] = uint_t, o[p++] = 32, o[p++] = 0;
    if (new_c0) o[p++] = 4u << 16 | SPV_OP_CONSTANT, o[p++] = uint_t, o[p++] = c0, o[p++] = 0;
    if (new_c1) o[p++] = 4u << 16 | SPV_OP_CONSTANT, o[p++] = uint_t, o[p++] = c1, o[p++] = 1;
    o[p++] = 3u << 16 | OP_TYPE_RUNTIME_ARRAY, o[p++] = rta, o[p++] = uint_t;
    o[p++] = 3u << 16 | OP_TYPE_STRUCT_, o[p++] = st, o[p++] = rta;
    o[p++] = 4u << 16 | SPV_OP_TYPE_POINTER, o[p++] = ptr_st, o[p++] = SC_STORAGE_BUFFER, o[p++] = st;
    if (new_ptr_u) o[p++] = 4u << 16 | SPV_OP_TYPE_POINTER, o[p++] = ptr_u, o[p++] = SC_STORAGE_BUFFER, o[p++] = uint_t;
    o[p++] = 4u << 16 | SPV_OP_VARIABLE, o[p++] = ptr_st, o[p++] = var, o[p++] = SC_STORAGE_BUFFER;
    if (stores) {
        if (new_bool) o[p++] = 2u << 16 | OP_TYPE_BOOL, o[p++] = bool_t;
        o[p++] = 4u << 16 | SPV_OP_TYPE_POINTER, o[p++] = ptr_pv, o[p++] = SC_PRIVATE, o[p++] = uint_t;
        o[p++] = 4u << 16 | SPV_OP_CONSTANT, o[p++] = uint_t, o[p++] = c_none, o[p++] = 0xffffffffu;
        o[p++] = 4u << 16 | SPV_OP_CONSTANT, o[p++] = uint_t, o[p++] = c_spare, o[p++] = VK_POOL_TEX;
        if (rings) {
            for (int j = 0; j < RING; j++) o[p++] = 4u << 16 | SPV_OP_CONSTANT, o[p++] = uint_t, o[p++] = cr[j], o[p++] = (uint32_t)j;
            o[p++] = 4u << 16 | SPV_OP_CONSTANT, o[p++] = uint_t, o[p++] = c_ring, o[p++] = RING;
            o[p++] = 4u << 16 | OP_TYPE_ARRAY_, o[p++] = ring_t, o[p++] = uint_t, o[p++] = c_ring;
            o[p++] = 4u << 16 | SPV_OP_TYPE_POINTER, o[p++] = ptr_ring, o[p++] = SC_PRIVATE, o[p++] = ring_t;
            o[p++] = (3u + RING) << 16 | SPV_OP_CONSTANT_COMPOSITE, o[p++] = ring_t, o[p++] = ring_none;
            for (int j = 0; j < RING; j++) o[p++] = c_none;
        }
        for (uint32_t k = 0; k < deferred; k++)
            o[p++] = 5u << 16 | SPV_OP_VARIABLE, o[p++] = ptr_pv, o[p++] = pv0 + k, o[p++] = SC_PRIVATE, o[p++] = c_none;
        for (uint32_t k = 0; k < rings; k++)
            o[p++] = 5u << 16 | SPV_OP_VARIABLE, o[p++] = ptr_ring, o[p++] = ra0 + k, o[p++] = SC_PRIVATE, o[p++] = ring_none;
        for (uint32_t k = 0; k < rings; k++)
            o[p++] = 5u << 16 | SPV_OP_VARIABLE, o[p++] = ptr_pv, o[p++] = rc0 + k, o[p++] = SC_PRIVATE, o[p++] = c0;
    }
    uint64_t site = 0;
    uint32_t k_def = 0, k_ring = 0, cur = 0;   // cur: the label of the block being copied
    fn = 0;
    for (uint64_t i = fn_start; i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        if (op == SPV_OP_FUNCTION && l >= 3) fn = w[i + 2];
        if (op == OP_LABEL_ && l >= 2) cur = w[i + 1];
        if (stores && (op == OP_KILL || op == OP_TERMINATE_INVOCATION || op == OP_DEMOTE_TO_HELPER ||
                       (fn == fs_entry && (op == OP_RETURN || op == OP_RETURN_VALUE)))) {
            // marks[the slot, or the spare when none was noted] = 1
#define POOL_MARK_STORE(from)                                                                                         \
    do {                                                                                                              \
        uint32_t v_ = next++, ok_ = next++, ix_ = next++, at_ = next++;                                              \
        o[p++] = 4u << 16 | OP_LOAD_, o[p++] = uint_t, o[p++] = v_, o[p++] = (from);                                 \
        o[p++] = 5u << 16 | OP_INOT_EQUAL, o[p++] = bool_t, o[p++] = ok_, o[p++] = v_, o[p++] = c_none;              \
        o[p++] = 6u << 16 | OP_SELECT_, o[p++] = uint_t, o[p++] = ix_, o[p++] = ok_, o[p++] = v_, o[p++] = c_spare;  \
        o[p++] = 6u << 16 | OP_ACCESS_CHAIN_, o[p++] = ptr_u, o[p++] = at_, o[p++] = var, o[p++] = c0, o[p++] = ix_; \
        o[p++] = 3u << 16 | OP_STORE_, o[p++] = at_, o[p++] = c1;                                                    \
    } while (0)
            for (uint32_t k = 0; k < deferred; k++) POOL_MARK_STORE(pv0 + k);
            for (uint32_t r = 0; r < rings; r++) {
                if (op == OP_DEMOTE_TO_HELPER) {   // not the block's end: every entry, without a loop
                    for (int j = 0; j < RING; j++) {
                        uint32_t from = next++;
                        o[p++] = 5u << 16 | OP_ACCESS_CHAIN_, o[p++] = ptr_pv, o[p++] = from, o[p++] = ra0 + r, o[p++] = cr[j];
                        POOL_MARK_STORE(from);
                    }
                    continue;
                }
                // the block ends here (a return, kill or terminate), so it can end in a loop over the entries noted:
                // for (j = 0; j < min(count, RING); j++) mark ring[j]
                uint32_t cnt = next++, lt = next++, lim = next++, hd = next++, j = next++, mg = next++, ct = next++,
                         cond = next++, body = next++, from = next++, jn = next++;
                o[p++] = 4u << 16 | OP_LOAD_, o[p++] = uint_t, o[p++] = cnt, o[p++] = rc0 + r;
                o[p++] = 5u << 16 | OP_ULESS_THAN, o[p++] = bool_t, o[p++] = lt, o[p++] = cnt, o[p++] = c_ring;
                o[p++] = 6u << 16 | OP_SELECT_, o[p++] = uint_t, o[p++] = lim, o[p++] = lt, o[p++] = cnt, o[p++] = c_ring;
                o[p++] = 2u << 16 | OP_BRANCH, o[p++] = hd;
                o[p++] = 2u << 16 | OP_LABEL_, o[p++] = hd;
                o[p++] = 7u << 16 | OP_PHI, o[p++] = uint_t, o[p++] = j, o[p++] = c0, o[p++] = cur, o[p++] = jn, o[p++] = ct;
                o[p++] = 5u << 16 | OP_ULESS_THAN, o[p++] = bool_t, o[p++] = cond, o[p++] = j, o[p++] = lim;
                o[p++] = 4u << 16 | OP_LOOP_MERGE, o[p++] = mg, o[p++] = ct, o[p++] = 0;
                o[p++] = 4u << 16 | OP_BRANCH_CONDITIONAL, o[p++] = cond, o[p++] = body, o[p++] = mg;
                o[p++] = 2u << 16 | OP_LABEL_, o[p++] = body;
                o[p++] = 5u << 16 | OP_ACCESS_CHAIN_, o[p++] = ptr_pv, o[p++] = from, o[p++] = ra0 + r, o[p++] = j;
                POOL_MARK_STORE(from);
                o[p++] = 2u << 16 | OP_BRANCH, o[p++] = ct;
                o[p++] = 2u << 16 | OP_LABEL_, o[p++] = ct;
                o[p++] = 5u << 16 | OP_IADD, o[p++] = uint_t, o[p++] = jn, o[p++] = j, o[p++] = c1;
                o[p++] = 2u << 16 | OP_BRANCH, o[p++] = hd;
                o[p++] = 2u << 16 | OP_LABEL_, o[p++] = mg;
                cur = mg;
            }
#undef POOL_MARK_STORE
        }
        memcpy(o + p, w + i, l * 4), p += l;
        if ((op == OP_ACCESS_CHAIN_ || op == OP_IN_BOUNDS_ACCESS_CHAIN) && l >= 5 && w[i + 3] < bound && set2[w[i + 3]] &&
            bind[w[i + 3]] && bind[w[i + 3]] - 1 < VK_POOL_KINDS) {
            uint8_t d = site_def[site++];
            if (d) {   // the slot noted for the stores at the end
                uint32_t ix = w[i + 4];
                if (d & 2) o[p++] = 4u << 16 | OP_BITCAST, o[p++] = uint_t, o[p++] = next, o[p++] = ix, ix = next++;
                if (d & 4) {   // ring[count % RING] = slot, count + 1
                    uint32_t ra = ra0 + k_ring, rc = rc0 + k_ring++, cnt = next++, j = next++, at = next++, inc = next++;
                    o[p++] = 4u << 16 | OP_LOAD_, o[p++] = uint_t, o[p++] = cnt, o[p++] = rc;
                    o[p++] = 5u << 16 | OP_BITWISE_AND, o[p++] = uint_t, o[p++] = j, o[p++] = cnt, o[p++] = cr[RING - 1];
                    o[p++] = 5u << 16 | OP_ACCESS_CHAIN_, o[p++] = ptr_pv, o[p++] = at, o[p++] = ra, o[p++] = j;
                    o[p++] = 3u << 16 | OP_STORE_, o[p++] = at, o[p++] = ix;
                    o[p++] = 5u << 16 | OP_IADD, o[p++] = uint_t, o[p++] = inc, o[p++] = cnt, o[p++] = c1;
                    o[p++] = 3u << 16 | OP_STORE_, o[p++] = rc, o[p++] = inc;
                } else {
                    o[p++] = 3u << 16 | OP_STORE_, o[p++] = pv0 + k_def++, o[p++] = ix;
                }
            } else {
                uint32_t at = next++;   // marks[slot] = 1, slot being the pool index of this access
                o[p++] = 6u << 16 | OP_ACCESS_CHAIN_, o[p++] = ptr_u, o[p++] = at, o[p++] = var, o[p++] = c0, o[p++] = w[i + 4];
                o[p++] = 3u << 16 | OP_STORE_, o[p++] = at, o[p++] = c1;
            }
        }
        i += l;
    }
    o[3] = next;
    POOL_MARK_FREE();
#undef POOL_MARK_FREE
    *out_n = p;
    *status = 1;
    return o;
}

// --selftest with NX2NAVI_SPV_MARK_TEST=<list>: each line "<in.spv> <out.spv>"; the module is moved to the pools
// (spirv_pool_rebind) and marked, and written to <out> when marked, for spirv-val. Prints the counts; 0 failures unless
// a file cannot be read or written.
static int spirv_mark_list_test(const char *list) {
    marks_vs_ok = marks_fs_ok = 1;
    FILE *f = fopen(list, "rb");
    if (!f) return printf("spirv: pool marks test: %s cannot be read\n", list), 1;
    char line[2048];
    int marked = 0, none = 0, missed = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        char in[1024], out[1024];
        if (sscanf(line, "%1023s %1023s", in, out) != 2) continue;
        FILE *g = fopen(in, "rb");
        if (!g) { bad++; continue; }
        fseek(g, 0, SEEK_END);
        long sz = ftell(g);
        fseek(g, 0, SEEK_SET);
        uint32_t *w = sz > 20 ? malloc((size_t)sz) : NULL;
        if (!w || fread(w, 1, (size_t)sz, g) != (size_t)sz) { fclose(g), free(w), bad++; continue; }
        fclose(g);
        uint64_t n = (uint64_t)sz / 4, mn = 0;
        uint32_t *pooled = spirv_pool_rebind(w, n);
        int st;
        const char *why;
        uint32_t *m = spirv_pool_mark(pooled ? pooled : w, n, 1, &mn, &st, &why);
        if (st > 0) {
            marked++;
            if (!plat_file_write_atomic(out, m, (size_t)mn * 4)) {
                if (!bad) printf("spirv: pool marks test: %s cannot be written\n", out);
                bad++;
            }
        } else if (st < 0) {
            missed++;
            printf("spirv: pool marks test: %s not markable: %s\n", in, why);
        } else {
            none++;
        }
        free(m), free(pooled), free(w);
    }
    fclose(f);
    printf("spirv: pool marks test: %d marked, %d without pool texture accesses, %d not markable, %d file errors\n", marked,
           none, missed, bad);
    return bad;
}

// early_ok: a fragment shader whose pipeline has no alpha to coverage (spirv_pool_mark)
static VkShaderModule make_module(const uint32_t *w, uint64_t words, int early_ok) {
    const uint32_t *orig_w = w;   // for the log of a module the marks cannot take
    uint64_t orig_words = words, fixed_n = 0;
    uint32_t *fixed = spirv_fixup(w, words, &fixed_n);
    if (fixed) w = fixed, words = fixed_n;
    uint32_t *pooled = spirv_pool_rebind(w, words);
    if (pooled) {
        free(fixed);
        w = fixed = pooled;
        if (vk_pool_marks_enabled()) {
            uint64_t mk_n = 0;
            int st;
            const char *why;
            uint32_t *mk = spirv_pool_mark(w, words, early_ok, &mk_n, &st, &why);
            if (mk) {
                free(fixed);
                w = fixed = mk, words = mk_n;
            } else if (st < 0) {
                int nth = plat_inc32(&vk_pool_marks_missed);
                // the hash is FNV-1a of the words make_module got, as vk_dump hashes a shader's SPIR-V; it differs
                // from the translation's only where the pipeline patched the module first (added vertex outputs)
                if (nth <= 8) {
                    uint32_t model = ~0u;
                    for (uint64_t i = 5; i < words;) {
                        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
                        if (!l) break;
                        if (op == SPV_OP_ENTRY_POINT && l >= 2) { model = w[i + 1]; break; }
                        i += l;
                    }
                    const char *stage = model == 0 ? "vertex" : model <= 3 ? "tessellation or geometry" : model == 4 ? "fragment"
                                      : model == 5 ? "compute" : model == 5364 ? "task" : model == 5365 ? "mesh" : "other";
                    rt_log("vk_pipeline: a %s shader that indexes the texture pool could not be marked (SPIR-V %u.%u, %llu "
                           "words, hash %016llx): %s%s", stage, w[1] >> 16 & 0xff, w[1] >> 8 & 0xff,
                           (unsigned long long)words, (unsigned long long)fnv(FNV0, orig_w, (size_t)orig_words * 4), why,
                           nth == 1 ? "; textures are not evicted by disuse this run (NX2NAVI_TEX_DISUSE)" : "");
                }
            }
        }
    }
    uint64_t rc_n = 0;
    uint32_t *rc = P.reconverge ? spirv_reconverge(w, words, &rc_n) : NULL;
    if (rc) {
        free(fixed);
        w = fixed = rc, words = rc_n;
    }
    uint64_t np_n = 0;
    uint32_t *np = P.nan_preserve ? spirv_nan_preserve(w, words, P.nan_preserve, &np_n) : NULL;
    if (np) {
        free(fixed);
        w = fixed = np, words = np_n;
    }
    uint64_t nb_n = 0;
    uint32_t nb_tests = 0, nb_sels = 0;
    uint32_t *nb = P.nan_bits ? spirv_nan_bits(w, words, &nb_n, &nb_tests, &nb_sels) : NULL;
    if (nb) {
        free(fixed);
        w = fixed = nb, words = nb_n;
        __atomic_fetch_add(&P.nan_bits_tests, nb_tests, __ATOMIC_RELAXED);
        __atomic_fetch_add(&P.nan_bits_selects, nb_sels, __ATOMIC_RELAXED);
        if (__atomic_add_fetch(&P.nan_bits_modules, 1, __ATOMIC_RELAXED) == 1)
            rt_log("vk_pipeline: first module with NaN tests made integer: %u tests, %u selects of NaN bits", nb_tests,
                   nb_sels);
    }
    if (P.feedback >= 2) mm_hash = fnv(FNV0, w, (size_t)words * 4);
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)words * 4, .pCode = w};
    VkShaderModule m = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(P.device, &ci, NULL, &m);
    free(fixed);
    return r == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

// ---- descriptor layouts

typedef struct {
    uint32_t n;
    VkDescriptorSetLayoutBinding b[MAX_BINDINGS];
    VkPipeBinding pb[MAX_BINDINGS];
    int pool;                          // a stage indexes the texture or sampler pool (set 2, not in b / pb)
} Bindings;

static VkDescriptorType descriptor_type(int kind, const GfxShaderRes *r) {
    int buffer = (r->type & 0xff) == 2;
    switch (kind) {
    case GFX_RES_UBO: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case GFX_RES_SSBO: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case GFX_RES_TEX:
        if (buffer) return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        if (r->separate) LOG_ONCE("vk_pipeline: a separate texture handle: bound as SAMPLED_IMAGE");
        return r->separate ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    default: return buffer ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
}

static VkShaderStageFlags stage_bit(int stage) {
    return stage == GFX_STAGE_VS ? VK_SHADER_STAGE_VERTEX_BIT : stage == GFX_STAGE_FS ? VK_SHADER_STAGE_FRAGMENT_BIT
         : stage == GFX_STAGE_TS ? VK_SHADER_STAGE_TASK_BIT_EXT : stage == GFX_STAGE_MS ? VK_SHADER_STAGE_MESH_BIT_EXT
                                                                 : VK_SHADER_STAGE_COMPUTE_BIT;
}

static int is_mesh(const VkPipeState *s) { return s->vs && gfx_shader_stage(s->vs) == GFX_STAGE_MS; }

// 1 when a mesh pipeline entered now is linked from stage libraries (comment at mesh_lib_init): it then gets a
// stage-independent layout (pipe_miss), which is how the rest of the file tells (no_libs).
static int mesh_libs(void) { return P.mesh_lib && P.indep; }

int vk_pipeline_mesh_libs(void) { return ensure_init() && mesh_libs(); }

// 1 for a mesh pipeline of layout l that has no stage libraries and so no fast link: every build of it is its whole
// build. A vertex pipeline is never one (gpl_link itself refuses the shared layout the probe can give it).
static int no_libs(const VkPipeState *s, const Layout *l) { return is_mesh(s) && !(l && l->set1); }

// Where a shader's translation came from (gfx_shader_origin), for the log.
static const char *origin_name(const GfxShader *s) {
    static const char *const name[3] = {"from the shader cache", "a previous translator's", "translated in this session"};
    return s ? name[gfx_shader_origin(s)] : "none";
}

// The shaders of the pipeline this thread builds, for the crash notes (platform.h plat_crash_context): the first 8
// bytes of each cache key, as gfx_shader.c names its cache files.
static void crash_note(const char *what, const GfxShader *a, const GfxShader *b, const GfxShader *c) {
    static __thread char buf[320];
    char *o = buf;
    o += snprintf(o, 160, "building %s", what);
    const GfxShader *sh[3] = {a, b, c};
    for (int i = 0; i < 3; i++)
        if (sh[i]) {
            const uint8_t *k = gfx_shader_key(sh[i]);
            o += snprintf(o, 40, " stage %d %02x%02x%02x%02x%02x%02x%02x%02x", gfx_shader_stage(sh[i]), k[0], k[1], k[2], k[3],
                          k[4], k[5], k[6], k[7]);
        }
    plat_crash_context(buf);
}

// One past the highest binding a shader uses (1 with no resources: binding 0 is the support buffer).
static uint32_t binding_span(const GfxShader *s) {
    uint32_t span = 1;
    for (int k = 0; k < GFX_RES_KINDS; k++) {
        const GfxShaderRes *r;
        int n = gfx_shader_resources(s, k, &r);
        for (int i = 0; i < n; i++)
            if (r[i].binding + (uint32_t)(r[i].count ? r[i].count : 1) > span) span = r[i].binding + (r[i].count ? r[i].count : 1);
    }
    return span;
}

// Adds a stage's resources with bindings b >= 1 moved by off; binding 0 (support buffer) is shared.
static int add_stage(Bindings *B, const GfxShader *s, uint32_t off) {
    int stage = gfx_shader_stage(s);
    VkShaderStageFlags bit = stage_bit(stage);
    if (!B->n) {
        B->b[0] = (VkDescriptorSetLayoutBinding){0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, 0, NULL};
        B->pb[0] = (VkPipeBinding){0, (uint8_t)stage, VK_PIPE_SUPPORT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, 0, NULL};
        B->n = 1;
    }
    B->b[0].stageFlags |= bit;
    B->pb[0].stages |= bit;
    for (int k = 0; k < GFX_RES_KINDS; k++) {
        const GfxShaderRes *r;
        int n = gfx_shader_resources(s, k, &r);
        for (int i = 0; i < n; i++) {
            if (r[i].pool) {   // set 2 (spirv_pool_rebind), bound by the replay from the pools
                B->pool = 1;
                continue;
            }
            if (B->n == MAX_BINDINGS || !r[i].binding) return 0;
            VkDescriptorType t = descriptor_type(k, &r[i]);
            uint32_t binding = r[i].binding + off, count = r[i].count ? r[i].count : 1;
            B->b[B->n] = (VkDescriptorSetLayoutBinding){binding, t, count, bit, NULL};
            B->pb[B->n] = (VkPipeBinding){(uint16_t)binding, (uint8_t)stage, (uint8_t)k, t, (uint16_t)count, bit, &r[i]};
            B->n++;
        }
    }
    return 1;
}

// The first binding a module's resource variables use that the binding list b[0..n) lacks (module bindings >= 1 moved
// by off), or -1. A reflection that misses a resource would otherwise hand the driver an invalid layout. The module's
// Binding decorations come from the list the shader keeps (gfx_shader_bindings), not from its SPIR-V: this runs on the
// drawing thread at every pipeline miss (pipe_miss), where a compressed module would be decoded.
static int module_binding_missing(const GfxShader *s, const VkDescriptorSetLayoutBinding *b, uint32_t n, uint32_t off) {
    const uint32_t *bind;
    uint32_t nb = gfx_shader_bindings(s, &bind);
    const GfxShaderRes *tr;
    int nt = gfx_shader_resources(s, GFX_RES_TEX, &tr);
    for (uint32_t i = 0; i < nb; i++) {
        int pooled = 0;   // a pool array: set 2, not in this layout (spirv_pool_rebind)
        for (int t = 0; t < nt; t++)
            if (tr[t].pool && tr[t].binding == (bind[i] & 0xffff)) pooled = 1;   // GfxShaderRes.binding is 16 bits
        if (pooled) continue;
        uint32_t want = bind[i] + (bind[i] >= 1 ? off : 0), j = 0;
        while (j < n && b[j].binding != want) j++;
        if (j == n) return (int)bind[i];
    }
    return -1;
}

// Checks both stages of a graphics pipeline against its layout; 0 with why filled when one uses a missing binding.
static int layout_covers(const VkPipeState *s, const Layout *l, uint32_t fs_off, char *why, size_t cap) {
    uint32_t nv = l->set1 ? l->n0 : l->n;
    int m = module_binding_missing(s->vs, l->b, nv, 0);
    const char *stage = "vertex";
    if (m < 0 && s->fs) {
        m = l->set1 ? module_binding_missing(s->fs, l->b + l->n0, l->n - l->n0, 0) : module_binding_missing(s->fs, l->b, l->n, fs_off);
        stage = "fragment";
    }
    if (m >= 0) snprintf(why, cap, "the %s shader uses binding %d, which its reflection and layout lack", stage, m);
    return m < 0;
}

// The update template of a set whose bindings are b[0..n): one entry per binding, in order, over the packed array the
// replay fills from the set's first descriptor, a VkDescSlot per descriptor. The bind point follows the stages (compute
// layouts hold the compute stage only). VK_NULL_HANDLE when templates are off, for an empty set, for a push set
// without vkCmdPushDescriptorSetWithTemplateKHR or when creation fails: the replay then writes the set with write
// structs.
static VkDescriptorUpdateTemplate make_tpl_bp(const VkDescriptorSetLayoutBinding *b, uint32_t n, VkDescriptorSetLayout sl,
                                              int push, VkPipelineLayout pl, uint32_t set, int bp) {
    if (!P.tpl || !n || (push && !P.tpl_push)) return VK_NULL_HANDLE;
    VkDescriptorUpdateTemplateEntry e[MAX_BINDINGS];
    size_t off = 0;
    VkShaderStageFlags stages = 0;
    for (uint32_t i = 0; i < n; i++) {
        e[i] = (VkDescriptorUpdateTemplateEntry){b[i].binding, 0, b[i].descriptorCount, b[i].descriptorType, off, sizeof(VkDescSlot)};
        off += sizeof(VkDescSlot) * b[i].descriptorCount;
        stages |= b[i].stageFlags;
    }
    VkDescriptorUpdateTemplateCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO, .descriptorUpdateEntryCount = n,
        .pDescriptorUpdateEntries = e,
        .templateType = push ? VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR : VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET,
        .descriptorSetLayout = sl,
        .pipelineBindPoint = bp >= 0 ? (VkPipelineBindPoint)bp
                             : stages & VK_SHADER_STAGE_COMPUTE_BIT ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
        .pipelineLayout = pl, .set = set};
    VkDescriptorUpdateTemplate t;
    if (P.tpl_create(P.device, &ci, NULL, &t) != VK_SUCCESS) {
        LOG_ONCE("vk_pipeline: descriptor update template creation failed: that set is written with write structs");
        return VK_NULL_HANDLE;
    }
    return t;
}
static VkDescriptorUpdateTemplate make_tpl(const VkDescriptorSetLayoutBinding *b, uint32_t n, VkDescriptorSetLayout sl,
                                           int push, VkPipelineLayout pl, uint32_t set) {
    return make_tpl_bp(b, n, sl, push, pl, set, -1);
}

// Finds or creates the layout of a binding list (sorted here); NULL on failure. Caller holds P.lock.
// Sorts a binding list by binding and hashes it into *h; the descriptor total, or -1 when a binding repeats.
static int64_t bindings_sort(Bindings *B, uint64_t *h) {
    for (uint32_t i = 1; i < B->n; i++)                     // insertion sort by binding; the lists are short
        for (uint32_t j = i; j > 0 && B->b[j - 1].binding > B->b[j].binding; j--) {
            VkDescriptorSetLayoutBinding t = B->b[j]; B->b[j] = B->b[j - 1]; B->b[j - 1] = t;
            VkPipeBinding u = B->pb[j]; B->pb[j] = B->pb[j - 1]; B->pb[j - 1] = u;
        }
    int64_t total = 0;
    for (uint32_t i = 0; i < B->n; i++) {
        if (i && B->b[i].binding == B->b[i - 1].binding) return -1;
        uint32_t k[4] = {B->b[i].binding, B->b[i].descriptorType, B->b[i].descriptorCount, B->b[i].stageFlags};
        *h = fnv(*h, k, sizeof k);
        total += B->b[i].descriptorCount;
    }
    return total;
}

static Layout *layout_get(Bindings *B) {
    uint64_t h = FNV0;
    int64_t total = bindings_sort(B, &h);
    if (total < 0) return NULL;
    Layout **head = &layouts[h % LAYOUT_BUCKETS];
    for (Layout *l = *head; l; l = l->next)
        if (l->hash == h && l->n == B->n && !memcmp(l->b, B->b, B->n * sizeof *B->b)) return l;
    Layout *l = calloc(1, sizeof *l + B->n * sizeof *B->b);
    if (!l) return NULL;
    l->hash = h, l->n = B->n;
    memcpy(l->b, B->b, B->n * sizeof *B->b);
    l->push = total <= (int64_t)P.max_push;
    VkDescriptorSetLayoutCreateInfo si = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .flags = l->push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0,
                                          .bindingCount = l->n, .pBindings = l->b};
    if (vkCreateDescriptorSetLayout(P.device, &si, NULL, &l->set) != VK_SUCCESS) {
        free(l);
        return NULL;
    }
    VkDescriptorSetLayout sets3[3] = {l->set, vk_empty_set_layout(), vk_pool_set_layout()};   // set 2: the pools
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 3,
                                     .pSetLayouts = sets3};
    if (vkCreatePipelineLayout(P.device, &pi, NULL, &l->layout) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(P.device, l->set, NULL);
        free(l);
        return NULL;
    }
    l->tpl[0] = make_tpl(l->b, l->n, l->set, l->push, l->layout, 0);
    l->next = *head;
    *head = l;
    P.stats.layouts++;
    P.stats.push_layouts += (uint64_t)l->push;
    return l;
}

// NX2NAVI_MESH_EMULATION (GPUs without VK_EXT_mesh_shader): the layout of an emulated mesh pipeline. The task and
// mesh stages' bindings are visible to the compute stage (they run as compute pipelines of this layout), the
// fragment stage's stay; push constants (MESH_EMU_PC_BYTES) for the compute stages and the generated vertex shader;
// set 0 gets a template per bind point. Caller holds P.lock.
static int mesh_emu_on(void) {   // decided with the mesh fallback at device creation (vk_mesh_path.h)
    const VkCtx *c = vk_ctx();
    return c && c->mesh_emulation;
}

static Layout *layout_get_emu(Bindings *B) {
    for (uint32_t i = 0; i < B->n; i++) {
        VkShaderStageFlags f = B->b[i].stageFlags;
        if (f & (VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_TASK_BIT_EXT))
            f = (f & ~(VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_TASK_BIT_EXT)) | VK_SHADER_STAGE_COMPUTE_BIT;
        B->b[i].stageFlags = f;
        B->pb[i].stages = f;
    }
    uint64_t h = fnv(FNV0, "mesh-emu", 8);
    int64_t total = bindings_sort(B, &h);
    if (total < 0) return NULL;
    Layout **head = &layouts[h % LAYOUT_BUCKETS];
    for (Layout *l = *head; l; l = l->next)
        if (l->hash == h && l->emu && l->n == B->n && !memcmp(l->b, B->b, B->n * sizeof *B->b)) return l;
    Layout *l = calloc(1, sizeof *l + B->n * sizeof *B->b);
    if (!l) return NULL;
    l->hash = h, l->n = B->n, l->emu = 1;
    memcpy(l->b, B->b, B->n * sizeof *B->b);
    l->push = total <= (int64_t)P.max_push;
    VkDescriptorSetLayoutCreateInfo si = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .flags = l->push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0,
                                          .bindingCount = l->n, .pBindings = l->b};
    if (vkCreateDescriptorSetLayout(P.device, &si, NULL, &l->set) != VK_SUCCESS) {
        free(l);
        return NULL;
    }
    VkDescriptorSetLayout sets3[3] = {l->set, vk_empty_set_layout(), vk_pool_set_layout()};
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT, 0, MESH_EMU_PC_BYTES};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 3,
                                     .pSetLayouts = sets3, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    if (vkCreatePipelineLayout(P.device, &pi, NULL, &l->layout) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(P.device, l->set, NULL);
        free(l);
        return NULL;
    }
    l->tpl[0] = make_tpl_bp(l->b, l->n, l->set, l->push, l->layout, 0, VK_PIPELINE_BIND_POINT_GRAPHICS);
    l->tpl_c = make_tpl_bp(l->b, l->n, l->set, l->push, l->layout, 0, VK_PIPELINE_BIND_POINT_COMPUTE);
    l->next = *head;
    *head = l;
    P.stats.layouts++;
    return l;
}

// The compute pipelines of an emulated mesh pipeline, per (mesh, task, layout): made by the pipeline's build
// (create_graphics, any thread) and found by its draws. ok 0: the stages could not be converted or built.
typedef struct MeshEmuEnt {
    struct MeshEmuEnt *next;
    const GfxShader *ms, *ts;
    VkPipelineLayout layout;
    int ok;
    VkPipeline mesh, task;
    MeshEmuInfo mi, ti;
} MeshEmuEnt;
// Each draw of an emulated mesh pipeline looks its entry up (vk_pipeline_mesh_emu), so the lookup takes no lock: an
// entry is complete before it is published at its bucket's head (release) and is never removed. emu_lock only orders
// the publishing. The build (two compute pipelines, up to seconds each in the driver on a first run) is done outside
// it: held across the build, as before, it stopped every draw of the replay that looked up any entry meanwhile, 2.5 s
// of the replay thread's samples in a first 0.8.0 run in Kokiri Forest on an RX 580 (2026-11-11).
#define EMU_BUCKETS 256
static MeshEmuEnt *emu_ents[EMU_BUCKETS];
static PlatMutex emu_lock = PLAT_MUTEX_INIT;

static uint32_t emu_bucket(const GfxShader *ms, const GfxShader *ts, VkPipelineLayout layout) {
    uint64_t h = (uint64_t)(uintptr_t)ms * 0x9e3779b97f4a7c15ull ^ (uint64_t)(uintptr_t)ts * 0xc2b2ae3d27d4eb4full ^
                 (uint64_t)(uintptr_t)layout * 0x165667b19e3779f9ull;
    return (uint32_t)(h >> 40) % EMU_BUCKETS;
}

static MeshEmuEnt *emu_find(MeshEmuEnt *e, const GfxShader *ms, const GfxShader *ts, VkPipelineLayout layout) {
    while (e && !(e->ms == ms && e->ts == ts && e->layout == layout)) e = __atomic_load_n(&e->next, __ATOMIC_ACQUIRE);
    return e;
}

static VkPipeline emu_compute(const uint32_t *w, size_t n, VkPipelineLayout layout) {
    VkShaderModule m = make_module(w, n, 0);
    if (!m) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = layout};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(P.device, P.cache, 1, &ci, NULL, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(P.device, m, NULL);
    return p;
}

static const MeshEmuEnt *emu_get(const GfxShader *ms, const GfxShader *ts, VkPipelineLayout layout, uint32_t toff, int create) {
    MeshEmuEnt **head = &emu_ents[emu_bucket(ms, ts, layout)];
    MeshEmuEnt *e = emu_find(__atomic_load_n(head, __ATOMIC_ACQUIRE), ms, ts, layout);
    if (e || !create) return e;
    e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->ms = ms, e->ts = ts, e->layout = layout;
    const char *why = NULL;
    uint64_t n = 0;
    size_t cn = 0;
    const uint32_t *w = gfx_shader_spirv_pin(ms, &n);
    uint32_t *c = w ? mesh_emu_compute(w, (size_t)n, &e->mi, &cn, &why) : NULL;
    gfx_shader_spirv_unpin(ms);
    if (c && (e->mesh = emu_compute(c, cn, layout))) e->ok = 1;
    else if (!why) why = "the mesh stage's compute pipeline was not created";
    free(c);
    if (e->ok && e->mi.payload_words && !ts) e->ok = 0, why = "a mesh stage that reads a task payload without a task stage";
    if (e->ok && ts) {
        w = gfx_shader_spirv_pin(ts, &n);
        c = w ? mesh_emu_compute(w, (size_t)n, &e->ti, &cn, &why) : NULL;
        gfx_shader_spirv_unpin(ts);
        uint32_t *r = c && toff ? spirv_rebind(c, cn, toff) : NULL;   // the task stage's bindings follow the fragment stage's
        if (c && (e->task = emu_compute(r ? r : c, cn, layout))) {
            if (e->ti.payload_words != e->mi.payload_words) e->ok = 0, why = "task and mesh payload sizes differ";
        } else {
            e->ok = 0;
            if (!why) why = "the task stage's compute pipeline was not created";
        }
        free(r), free(c);
    }
    plat_mutex_lock(&emu_lock);
    MeshEmuEnt *had = emu_find(*head, ms, ts, layout);   // another build of the same stages published first: keep that
    if (!had) {
        static int logged;
        if (!e->ok && logged++ < 16) rt_log("vk_pipeline: mesh emulation: a mesh program is not emulated: %s", why ? why : "?");
        e->next = *head;
        __atomic_store_n(head, e, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&emu_lock);
    if (had) {
        if (e->mesh) vkDestroyPipeline(P.device, e->mesh, NULL);
        if (e->task) vkDestroyPipeline(P.device, e->task, NULL);
        free(e);
        return had;
    }
    return e;
}

int vk_pipeline_mesh_emu(const GfxShader *ms, const GfxShader *ts, VkPipelineLayout layout, VkPipeline *mesh,
                         VkPipeline *task, MeshEmuInfo *mi) {
    const MeshEmuEnt *e = emu_get(ms, ts, layout, 0, 0);
    if (!e || !e->ok) return 0;
    *mesh = e->mesh, *task = e->task;
    *mi = e->mi;
    if (ts) mi->max_mesh = e->ti.max_mesh;
    return 1;
}

// Independent layouts (P.indep): the vertex stage's bindings in set 0, the fragment stage's in set 1 (empty without a
// fragment shader), each with its own support buffer at binding 0, the pipeline layout created with
// VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT so stage libraries can use it. Only the P.push_stage set is a
// push-descriptor set (a layout holds one), when it fits maxPushDescriptors; the other is allocated per draw from the
// command buffer's pools. On success *B holds both lists, set 0 first, VkPipeBinding.set filled in. Caller holds P.lock.
static Layout *layout_pair(Bindings *B, Bindings *F) {
    uint64_t h = fnv(FNV0, "indep", 5);
    int64_t tv = bindings_sort(B, &h);
    h = fnv(h, "|", 1);
    int64_t tf = bindings_sort(F, &h);
    if (tv < 0 || tf < 0 || B->n + F->n > MAX_BINDINGS) return NULL;
    uint32_t n0 = B->n, n = B->n + F->n;
    memcpy(B->b + n0, F->b, F->n * sizeof *F->b);
    memcpy(B->pb + n0, F->pb, F->n * sizeof *F->pb);
    for (uint32_t i = n0; i < n; i++) B->pb[i].set = 1;
    B->n = n;
    Layout **head = &layouts[h % LAYOUT_BUCKETS];
    for (Layout *l = *head; l; l = l->next)
        if (l->hash == h && l->set1 && l->n == n && l->n0 == n0 && !memcmp(l->b, B->b, n * sizeof *B->b)) return l;
    Layout *l = calloc(1, sizeof *l + n * sizeof *B->b);
    if (!l) return NULL;
    l->hash = h, l->n = n, l->n0 = n0;
    memcpy(l->b, B->b, n * sizeof *B->b);
    l->push = P.push_stage == GFX_STAGE_VS && tv <= (int64_t)P.max_push;
    l->push1 = P.push_stage == GFX_STAGE_FS && F->n && tf <= (int64_t)P.max_push;
    VkDescriptorSetLayoutCreateInfo s0 = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .flags = l->push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0,
                                          .bindingCount = n0, .pBindings = l->b};
    VkDescriptorSetLayoutCreateInfo s1 = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .flags = l->push1 ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0,
                                          .bindingCount = n - n0, .pBindings = l->b + n0};
    if (vkCreateDescriptorSetLayout(P.device, &s0, NULL, &l->set) != VK_SUCCESS) {
        free(l);
        return NULL;
    }
    if (vkCreateDescriptorSetLayout(P.device, &s1, NULL, &l->set1) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(P.device, l->set, NULL);
        free(l);
        return NULL;
    }
    VkDescriptorSetLayout sets[3] = {l->set, l->set1, vk_pool_set_layout()};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT, .setLayoutCount = 3,
                                     .pSetLayouts = sets};
    if (vkCreatePipelineLayout(P.device, &pi, NULL, &l->layout) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(P.device, l->set, NULL);
        vkDestroyDescriptorSetLayout(P.device, l->set1, NULL);
        free(l);
        return NULL;
    }
    l->tpl[0] = make_tpl(l->b, n0, l->set, l->push, l->layout, 0);
    l->tpl[1] = make_tpl(l->b + n0, n - n0, l->set1, l->push1, l->layout, 1);
    l->next = *head;
    *head = l;
    P.stats.layouts++;
    P.stats.push_layouts += (uint64_t)(l->push | l->push1);
    return l;
}

// Vertex outputs with independent layouts: each vertex shader declares the union of the inputs of every fragment
// shader met with it (pipeline-keys.bin at the prewarm, then each pipeline built), so its module does not depend on the
// fragment shader of one pipeline. Unwritten ones read (0, 0, 0, 1) as before. NX2NAVI_VS_OUTS=all declares all 32.
#define VS_OUTS_CAP 8192
static struct {
    PlatMutex lock;
    const GfxShader *vs[VS_OUTS_CAP];
    uint32_t mask[VS_OUTS_CAP];
    GfxShaderIo io[VS_OUTS_CAP][32];
    uint32_t n, full;
} VO = {.lock = PLAT_MUTEX_INIT};

// Adds fs's inputs (packed ones excepted) to vs's union; returns the union, types in io (the first type met wins).
static uint32_t vs_outs(const GfxShader *vs, const GfxShader *fs, GfxShaderIo io[32]) {
    GfxShaderIo fin[32];
    uint32_t need = fs ? gfx_shader_inputs(fs, fin) : 0, mask;
    for (int i = 0; i < 32; i++)
        if (need >> i & 1 && fin[i].packed) need &= ~(1u << i);
    plat_mutex_lock(&VO.lock);
    uint32_t j = (uint32_t)(((uintptr_t)vs >> 4) * 0x9e3779b1u) & (VS_OUTS_CAP - 1);
    while (VO.vs[j] && VO.vs[j] != vs) j = (j + 1) & (VS_OUTS_CAP - 1);
    if (!VO.vs[j] && VO.n + 1 >= VS_OUTS_CAP * 3 / 4) {   // full: this pair alone
        VO.full++;
        plat_mutex_unlock(&VO.lock);
        LOG_ONCE("vk_pipeline: vertex output union table full: outputs from the pair");
        memset(io, 0, 32 * sizeof *io);
        for (int i = 0; i < 32; i++)
            if (need >> i & 1) io[i] = fin[i];
        return need;
    }
    if (!VO.vs[j]) VO.vs[j] = vs, VO.n++;
    for (int i = 0; i < 32; i++)
        if (!(need >> i & 1)) continue;
        else if (!(VO.mask[j] >> i & 1)) VO.io[j][i] = fin[i], VO.mask[j] |= 1u << i;
        else if (VO.io[j][i].type == fin[i].type && VO.io[j][i].comps < fin[i].comps) VO.io[j][i].comps = fin[i].comps;
    mask = VO.mask[j];
    memcpy(io, VO.io[j], 32 * sizeof *io);
    plat_mutex_unlock(&VO.lock);
    return mask;
}

// What a mesh module writes (spirv_mesh_outputs), kept per mesh shader: the library keys of a mesh pipeline need it
// at every lookup (lib_keys), and reading it walks the module's declarations with two tables of its id bound.
#define MESH_OUTS_CAP 1024
static struct {
    PlatMutex lock;
    const GfxShader *ms[MESH_OUTS_CAP];
    uint32_t mask[MESH_OUTS_CAP];
    uint8_t points[MESH_OUTS_CAP];
    uint32_t n;
} MSO = {.lock = PLAT_MUTEX_INIT};

static uint32_t mesh_outs(const GfxShader *ms, int *points) {
    uint32_t j = (uint32_t)(((uintptr_t)ms >> 4) * 0x9e3779b1u) & (MESH_OUTS_CAP - 1), mask;
    plat_mutex_lock(&MSO.lock);
    while (MSO.ms[j] && MSO.ms[j] != ms) j = (j + 1) & (MESH_OUTS_CAP - 1);
    if (MSO.ms[j]) {
        mask = MSO.mask[j], *points = MSO.points[j];
        plat_mutex_unlock(&MSO.lock);
        return mask;
    }
    plat_mutex_unlock(&MSO.lock);
    uint64_t n = 0;
    const uint32_t *w = gfx_shader_spirv_pin(ms, &n);   // once per mesh shader: decoded here if it is compressed
    *points = 0;
    mask = w ? spirv_mesh_outputs(w, n, points) : ~0u;   // as create_graphics takes a module it cannot read
    gfx_shader_spirv_unpin(ms);
    plat_mutex_lock(&MSO.lock);
    for (j = (uint32_t)(((uintptr_t)ms >> 4) * 0x9e3779b1u) & (MESH_OUTS_CAP - 1); MSO.ms[j] && MSO.ms[j] != ms;)
        j = (j + 1) & (MESH_OUTS_CAP - 1);
    if (!MSO.ms[j] && MSO.n + 1 < MESH_OUTS_CAP * 3 / 4)   // a full table: read again next time
        MSO.ms[j] = ms, MSO.mask[j] = mask, MSO.points[j] = (uint8_t)*points, MSO.n++;
    plat_mutex_unlock(&MSO.lock);
    return mask;
}

// The inputs of fragment shader fs that mesh shader ms does not write, which the fragment module of their pipeline
// gets filled in (spirv_fs_fill_inputs; create_graphics works the same mask out for a whole build), and in *points
// whether they are filled as a point sprite's. 0 and no points when nothing is filled.
static uint32_t mesh_fill(const GfxShader *ms, const GfxShader *fs, int *points) {
    GfxShaderIo fin[32];
    *points = 0;
    if (!fs) return 0;
    int pts = 0;
    uint32_t need = gfx_shader_inputs(fs, fin), have = mesh_outs(ms, &pts), missing = 0;
    for (int i = 0; i < 32; i++)
        if (need >> i & 1 && !fin[i].packed && !(have >> i & 1)) missing |= 1u << i;
    if (missing) *points = pts;
    return missing;
}

// ---- pipeline cache table

static Pipe *pipe_find(const void *key, size_t size, uint64_t h) {
    for (Pipe *p = pipes[h % PIPE_BUCKETS]; p; p = p->next)
        if (p->hash == h && p->key_size == size && !memcmp(p->key, key, size)) return p;
    return NULL;
}

// Stores a result (a failed one too, so it is not retried every draw). Caller holds P.lock.
static Pipe *pipe_add(const void *key, size_t size, uint64_t h, VkPipeline pipeline, const Layout *l, const Bindings *B,
                      VkPipelineBindPoint bp, uint32_t vs_inputs) {
    Pipe *p = calloc(1, sizeof *p + size);
    if (!p) return NULL;
    p->hash = h, p->key_size = size;
    memcpy(p->key, key, size);
    if (l && B) {
        p->bindings = malloc(B->n * sizeof *p->bindings);
        if (p->bindings) memcpy(p->bindings, B->pb, B->n * sizeof *p->bindings);
    }
    p->info = (VkPipeInfo){pipeline, l ? l->layout : VK_NULL_HANDLE, l ? l->set : VK_NULL_HANDLE, l ? l->push : 0, bp,
                           p->bindings ? B->n : 0, p->bindings, vs_inputs};
    p->info.set_layout1 = l ? l->set1 : VK_NULL_HANDLE;
    p->info.push1 = l ? l->push1 : 0;
    p->info.n_sets = l && l->set1 ? 2 : 1;
    p->info.pool = B ? B->pool : 0;
    if (l) p->info.tpl[0] = l->tpl[0], p->info.tpl[1] = l->tpl[1];
    if (l && l->emu) p->info.emu = 1, p->info.emu_tpl_c = l->tpl_c;
    p->lay = l;
    Pipe **head = &pipes[h % PIPE_BUCKETS];
    p->next = *head;
    *head = p;
    return p;
}

// ---- graphics pipelines

typedef struct {                       // normalized: zero where the state is dynamic or unused
    const GfxShader *vs, *fs, *ts;     // ts: the task stage of a mesh draw
    uint32_t color[8], depth, stencil; // VkFormat
    uint8_t n_color, samples, topo_class, minus_w;
    uint8_t clamp, poly_mode, a2c, logic_op;   // logic_op: VkLogicOp + 1 when enabled
    uint8_t blend_en, dsb, fbl, pad;  // dsb: an enabled target blends with a SRC1 factor; fbl: VkPipeState.feedback
    uint32_t wmask;
    uint8_t blend[8][6];               // src, dst, op, src alpha, dst alpha, alpha op (VkBlendFactor / VkBlendOp)
    uint32_t attr_fmt[32], attr_off[32];
    uint8_t attr_bind[32];
    uint8_t rate[17];                  // per binding: 0 unused, 1 vertex, 2 instance
    uint8_t indep;                     // P.indep when built: the probe draws through both layouts
    uint8_t pad2[2];
} GKey;

static int blendable(VkFormat f) {
    return f != VK_FORMAT_UNDEFINED && (format_features(f, 1) & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
}

static int target_count(const VkPipeState *s) {
    int n = 0;
    for (int i = 0; i < 8; i++)
        if (s->color_fmt[i]) n = i + 1;
    return n;
}

// SRC1 blend factors read the fragment shader's second output (dual-source blending); the pipeline then needs a
// fragment module whose output 1 is Location 0, Index 1 (spirv_dual_source).
static int is_src1(unsigned f) { return f >= NVN_BLEND_FUNC_SRC1_COLOR && f <= NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA; }
// Runs per draw (gkey_fill, target_wmask): only the blend factors of blend-enabled targets are read until one is SRC1,
// which NX2navi never sets (0 of 876 pipelines in the field and fight saves).
// NX2NAVI_VK_NOOPT=1: pipelines built without the driver's optimiser, for a driver whose shader compiler crashes on
// one (the test switch for such a report); slower shaders, the same picture. mesh: only pipelines with task and mesh
// stages, the terrain's and the foliage's. 0: none.
// Unset: mesh on AMD's Windows driver for RDNA 1 and 2 (vk_caps.amd_sc), 0 elsewhere. That driver loses the device
// ("No fault detected", a timeout) minutes into play on RX 6600 to RX 6950 XT cards in every version from 0.6.0 to
// 0.9.2, with the pipeline library on or off and the lazy write-back on or off. Where the reports' pass marks name a
// pass the GPU was in, twelve logs of five reporters, it is a pass whose first draw is a mesh pipeline (mesh shader
// ba4302f1 with fragment shader f64a96dd in nine, the one-draw pass of 1bbdb17e with 6cc3a476 in three). The logs of
// 0.9.1 and 0.9.2 end with the game's dispatch of cs d7566eb1 finished and nothing after it marked: the group of draws
// that followed was recorded by the replay workers, whose passes have no marks, so which draw of it hung is not known.
// No reporter ran with the optimiser off, so that it is the cause is not shown: it costs shader speed in those
// pipelines and not the picture, and NX2NAVI_VK_NOOPT=0 gives the optimised ones back. The flag is part of what the
// driver's cache keys a pipeline by, so those pipelines are built once more after the change.
// Since then that driver draws mesh programs through the mesh fallback by default (vk_mesh_path.h), and no mesh
// pipeline is built there: this applies with mesh_fallback = 0 alone, as the 0.10.x way to compare against.
static VkPipelineCreateFlags noopt_flag_for(int mesh) {
    static int on = -1;
    if (on < 0) {
        const char *v = cfg_get("NX2NAVI_VK_NOOPT");
        int by_driver = !v || !*v;
        on = by_driver ? (vk_caps.amd_sc ? 2 : 0) : !strcmp(v, "mesh") ? 2 : strcmp(v, "0") != 0;
        if (on) rt_log("vk_pipeline: %s built without the driver's optimiser (%s)",
                       on == 2 ? "pipelines with task and mesh stages" : "pipelines",
                       by_driver ? "the default on AMD's driver for RDNA 1 and 2, where the device is lost in passes "
                                   "that start with such a pipeline; NX2NAVI_VK_NOOPT=0 builds them optimised"
                                 : "NX2NAVI_VK_NOOPT");
    }
    return on == 1 || (on == 2 && mesh) ? VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT : 0;
}
static VkPipelineCreateFlags noopt_flag(void) { return noopt_flag_for(0); }

// ---- pipeline creation feedback (NX2NAVI_PIPE_FEEDBACK, default 1; 0 off)
// Each pipeline, library and compute build asks the driver whether pipeline-cache.bin (P.cache) held it
// (VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) and how long the create took. The counts appear
// with the prewarm, the stage library prebuild and every cache save, so a log tells whether a slow boot compiled or
// read its pipelines: on NVIDIA the driver's own disk cache is kept per executable, so a renamed or new exe gets nothing
// from it, and only pipeline-cache.bin can make its first boot quick. =2 also logs every compute build with a hash of
// its final module, and every graphics pipeline build with the hashes of its final modules and of its fixed state, so
// two runs can be compared for a module or a state that changes between them.
enum { FB_GRAPHICS, FB_LIBRARY, FB_COMPUTE, FB_KINDS };
static struct {
    PlatMutex lock;
    uint64_t n[FB_KINDS], hit[FB_KINDS];
    double hit_ms[FB_KINDS], miss_ms[FB_KINDS];
} FB = {.lock = PLAT_MUTEX_INIT};

// Chains fb into a create info's pNext (next: its current pNext). Returns what the create info's pNext becomes.
static const void *fb_chain(VkPipelineCreationFeedbackCreateInfo *ci, VkPipelineCreationFeedback *fb, const void *next) {
    if (!P.feedback) return next;
    memset(fb, 0, sizeof *fb);
    *ci = (VkPipelineCreationFeedbackCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
                                                 .pNext = next, .pPipelineCreationFeedback = fb};
    return ci;
}

static void fb_note(int kind, const VkPipelineCreationFeedback *fb) {
    if (!P.feedback || !P.cache || !(fb->flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT)) return;
    int hit = (fb->flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0;
    double ms = (double)fb->duration / 1e6;
    plat_mutex_lock(&FB.lock);
    FB.n[kind]++, FB.hit[kind] += (uint64_t)hit;
    if (hit) FB.hit_ms[kind] += ms;
    else FB.miss_ms[kind] += ms;
    plat_mutex_unlock(&FB.lock);
}

// One line: per kind, builds that came from pipeline-cache.bin of all builds so far, with the average time of each.
static void fb_log(const char *when) {
    pb_log(when);
    if (!P.feedback || !P.cache) return;
    static const char *const kn[FB_KINDS] = {"pipelines", "libraries", "compute"};
    char line[512];
    int o = snprintf(line, sizeof line, "vk_pipeline: pipeline-cache.bin hits %s:", when);
    plat_mutex_lock(&FB.lock);
    for (int k = 0; k < FB_KINDS && o < (int)sizeof line - 96; k++) {
        uint64_t miss = FB.n[k] - FB.hit[k];
        o += snprintf(line + o, sizeof line - o, " %s %llu of %llu (%.0f%%; avg %.2f ms from the file, %.1f ms otherwise)%s", kn[k],
                      (unsigned long long)FB.hit[k], (unsigned long long)FB.n[k], FB.n[k] ? 100.0 * FB.hit[k] / FB.n[k] : 0.0,
                      FB.hit[k] ? FB.hit_ms[k] / FB.hit[k] : 0.0, miss ? FB.miss_ms[k] / miss : 0.0, k + 1 < FB_KINDS ? ";" : "");
    }
    plat_mutex_unlock(&FB.lock);
    rt_log("%s", line);
}

static int uses_src1(const VkPipeState *s) {
    for (uint32_t m = s->color.blend_en & 0xffu; m; m &= m - 1) {
        int i = __builtin_ctz(m);
        const blend_st *b = &s->blend[i];
        if ((is_src1(b->src_rgb) || is_src1(b->dst_rgb) || is_src1(b->src_a) || is_src1(b->dst_a)) &&
            i < target_count(s) && blendable(target_format(s->color_fmt[i], NULL, NULL)))
            return 1;
    }
    return 0;
}

// Colour write mask of target i: NVN's channel mask, zero for a target outside the fragment shader's output map (SPH
// omap). Maxwell leaves such a target untouched;
// Vulkan writes undefined values, and on AMD they lit grass in front of Link as flat lime. Zeros are wrong
// too: NX2NAVI's half-res transparency pass binds its linear depth (R32F) as target 7, samples it in the same draws and
// writes only targets 0-2, 4 and 5; zeroed, the depth that later passes rebuild from it was 0 in tiles, and the fog
// over distant geometry showed rectangles of clear sky.
// The same holds per component: a shader that writes a target's RGB and not its alpha leaves the alpha alone on
// Maxwell, while Vulkan writes undefined values, and AMD's driver writes whatever its export registers held (NaN and
// saturated pixels the bloom then spreads). Components outside the shader's omap are masked out, where it is known.
static uint32_t target_wmask(const VkPipeState *s, int i) {
    uint32_t m = s->chmask.mask >> (4 * i) & 0xf;
    if (s->fs && !(gfx_shader_outputs(s->fs, NULL) >> i & 1)) m = 0;
    if (s->fs) {
        uint32_t c = gfx_shader_omap(s->fs) >> (4 * i) & 0xf;
        if (c) m &= c;
    }
    if (i == 1 && s->fs && P.f_dual_src && uses_src1(s)) m = 0;   // output 1 is the second blend source (spirv_dual_source)
    return m;
}

uint32_t vk_pipeline_target_wmask(const VkPipeState *s, int i) { return target_wmask(s, i); }

static void gkey_fill(const VkPipeState *s, GKey *k) {
    memset(k, 0, sizeof *k);
    k->vs = s->vs, k->fs = s->fs, k->ts = s->ts;
    k->indep = (uint8_t)P.indep;
    k->n_color = (uint8_t)target_count(s);
    for (int i = 0; i < k->n_color; i++) k->color[i] = target_format(s->color_fmt[i], NULL, NULL);
    int d, st;
    VkFormat df = target_format(s->depth_fmt, &d, &st);
    k->depth = d ? df : VK_FORMAT_UNDEFINED;
    k->stencil = st ? df : VK_FORMAT_UNDEFINED;
    k->samples = s->samples > 1 ? s->samples : 1;
    k->topo_class = (uint8_t)topology_class(vk_pipeline_topology(s->prim));
    k->minus_w = s->depth_minus_w != 0;
    k->dsb = s->fs && P.f_dual_src && uses_src1(s);
    k->fbl = s->fs ? s->feedback : 0;
    if (!P.dyn_clamp) k->clamp = s->depth_clamp && P.f_depth_clamp;
    if (!P.dyn_poly) k->poly_mode = (uint8_t)polygon_mode(s->poly.mode);
    if (!P.dyn_a2c) k->a2c = s->ms.a2c != 0;
    if (P.f_logic_op && s->color.logic_op != NVN_LOGIC_OP_COPY) k->logic_op = (uint8_t)(s->color.logic_op + 1);
    for (int i = 0; i < k->n_color; i++) {
        int on = (s->color.blend_en >> i & 1) && blendable(k->color[i]);
        if (!P.dyn_blend_en && on) k->blend_en |= (uint8_t)(1u << i);
        if (!P.dyn_wmask) k->wmask |= target_wmask(s, i) << (4 * i);
        if (!P.dyn_blend_eq && on) {
            const blend_st *b = &s->blend[i];
            uint8_t *o = k->blend[i];
            o[0] = (uint8_t)blend_factor(b->src_rgb), o[1] = (uint8_t)blend_factor(b->dst_rgb), o[2] = (uint8_t)blend_op(b->eq_rgb);
            o[3] = (uint8_t)blend_factor(b->src_a), o[4] = (uint8_t)blend_factor(b->dst_a), o[5] = (uint8_t)blend_op(b->eq_a);
        }
    }
    // mesh draws: no vertex input, the mesh stage declares its primitives (emulated point meshes draw point lists)
    if (is_mesh(s)) k->topo_class = s->prim == NVN_DRAW_PRIMITIVE_POINTS ? 0 : 2;
    if (!P.dyn_vi && !is_mesh(s)) {
        VtxInput v;
        vertex_input(s, &v);
        for (uint32_t i = 0; i < v.n_attr; i++) {
            uint32_t l = v.a[i].location;
            k->attr_fmt[l] = v.a[i].format, k->attr_off[l] = v.a[i].offset, k->attr_bind[l] = (uint8_t)v.a[i].binding;
        }
        for (uint32_t i = 0; i < v.n_bind; i++)
            k->rate[v.b[i].binding] = v.b[i].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE ? 2 : 1;
    }
}

// Independent layouts: the outputs the vertex shader lacks from its union with s->fs's inputs added (vs_outs), types in
// fin. The vertex module (and its pre-rasterization library) depends on the vertex shader and this mask only.
static uint32_t vs_union_missing(const VkPipeState *s, GfxShaderIo fin[32]) {
    GfxShaderIo vout[32];
    uint32_t need = vs_outs(s->vs, s->fs, fin), have = gfx_shader_outputs(s->vs, vout), missing = 0;
    if (s->fs) {   // a location another fragment shader reads as another type: this pair's type (rare)
        GfxShaderIo fi[32];
        uint32_t fneed = gfx_shader_inputs(s->fs, fi);
        for (int i = 0; i < 32; i++)
            if (fneed >> i & 1 && !fi[i].packed &&
                (!(need >> i & 1) || fin[i].type != fi[i].type || fin[i].comps < fi[i].comps)) {
                if (need >> i & 1 && fin[i].type != fi[i].type)
                    LOG_ONCE("vk_pipeline: varying %d read as type %d and %d by fragment shaders of one vertex shader", i, fin[i].type, fi[i].type);
                fin[i] = fi[i], need |= 1u << i;
            }
    }
    if (P.vs_outs_all)
        for (int i = 0; i < 32; i++)
            if (!(need >> i & 1)) fin[i] = (GfxShaderIo){GFX_TYPE_FLOAT, 4, 0}, need |= 1u << i;
    for (int i = 0; i < 32; i++)
        if (need >> i & 1 && !(have >> i & 1)) missing |= 1u << i;
    return missing;
}


// ---- water corrections (docs/water-projective.md; NX2NAVI_WATER_PROJECTIVE, default 1; 0: the translations as built)
// Two of the game's water translations are built from an edited module (spirv_water.c):
//  - the coordinate pair (mode 1). The vertex stage worked out each vertex's place on the screen (x / w and y / w,
//    scaled to 0..1) and the fragment stage read it interpolated without perspective, to sample the scene behind the
//    water. A triangle with a corner behind the camera has no such place for that corner, and the scene was sampled
//    elsewhere: a rectangle of water over solid ground at the Hyrule Castle moat and the Castle Town canal. The pair
//    now hands x, y and w down undivided and the fragment stage divides. Both stages change, or neither.
//  - the absorption material (mode 2): its distance through the water could come out negative, which turned the
//    dimming into a gain (a red beam). The distance stored is at least 0.
// The translations are known by the FNV-1a of their SPIR-V, one profile per translator output (the host options of
// AMD's and NVIDIA's drivers give different modules); every other module is built as it is. The edited modules are
// made at their first use and kept. Modes go into the stage libraries' keys (PrKey, FsKey), so a library of an edited
// module is never linked with one of the original.
typedef struct {
    uint64_t vs, fs, fs_set1;             // the coordinate pair: vertex, fragment, the fragment in its set 1 form
    uint64_t mat, mat_set1;               // the absorption material, and in its set 1 form
    uint32_t mat_pointer, mat_distance;   // the ids of the store its distance goes through
} WaterProfile;
static const WaterProfile water_profiles[] = {
    {0xf6d753852c47b0deull, 0xa39c3d09241441faull, 0x3d3a316ab4da3bbaull, 0xe73dfc0f4c556df8ull, 0xabfccea256824a78ull, 1965, 8490},
    {0x8d83e3705d790fedull, 0x3c20cb98dbaeb257ull, 0x216bd323c6b48ee2ull, 0x061318799d885bcfull, 0xb3cec6d0d78ac25aull, 1969, 8826},
};
enum { WATER_VERTEX, WATER_FRAGMENT, WATER_ABSORPTION };

// The edited modules, by the hash of the module each stands for as it is built: a vertex translation, or a fragment
// module in its set 1 form. state: 0 empty, 1 made, -1 the edit refused the module (it is built as it is).
static struct { uint64_t key; uint32_t *code; uint64_t words; int state; } water_mods[8];
static PlatMutex water_lock = PLAT_MUTEX_INIT;

// The edited module that stands for the module of hash `key`, or NULL. src: the translation it is made from at the
// first call (NULL: only look), whose SPIR-V is pinned for the edit only; a fragment translation is first put into its
// set 1 form, which must be the module `key` names, as it is the form the pipeline builds.
static const uint32_t *water_module(uint64_t key, int kind, const WaterProfile *p, const GfxShader *src, uint64_t *words) {
    const uint32_t *code = NULL;
    plat_mutex_lock(&water_lock);
    int i = 0;
    while (i < 8 && water_mods[i].state && water_mods[i].key != key) i++;
    if (i < 8 && !water_mods[i].state && src) {
        uint64_t wn = 0, n = 0;
        const uint32_t *w = gfx_shader_spirv_pin(src, &n);
        uint32_t *c = NULL, *set1 = kind == WATER_VERTEX ? NULL : spirv_set(w, n, 1);
        if (kind == WATER_VERTEX) c = spirv_water_vertex(w, n, &wn);
        else if (set1 && fnv(FNV0, set1, (size_t)n * 4) == key)
            c = kind == WATER_FRAGMENT ? spirv_water_fragment(set1, n, &wn)
                                       : spirv_water_absorption(set1, n, p->mat_pointer, p->mat_distance, &wn);
        free(set1);
        gfx_shader_spirv_unpin(src);
        water_mods[i].key = key, water_mods[i].code = c, water_mods[i].words = wn, water_mods[i].state = c ? 1 : -1;
        static const char *const names[] = {"vertex stage of the coordinate pair", "fragment stage of the coordinate pair",
                                            "absorption material"};
        if (c) rt_log("vk_pipeline: water: the %s (module %016llx) is built from its corrected form "
                      "(NX2NAVI_WATER_PROJECTIVE)", names[kind], (unsigned long long)key);
        else rt_log("vk_pipeline: water: the %s (module %016llx) is not of the shape its correction expects; built as "
                    "it is", names[kind], (unsigned long long)key);
    }
    if (i < 8 && water_mods[i].state == 1) code = water_mods[i].code, *words = water_mods[i].words;
    plat_mutex_unlock(&water_lock);
    return code;
}

// The water mode of a pipeline's shaders: 1 a coordinate pair whose two edited modules exist, 2 the absorption
// material whose edited module exists, else 0. The drawing thread asks it for every pipeline it links (lib_keys): the
// modules' hashes are the ones the shaders keep (gfx_shader_spirv_fnv), and the SPIR-V is read only to make an edited
// module, once per profile.
static int water_projective_pair(const VkPipeState *s) {
    static int on = -1;
    if (on < 0) on = cfg_bool("NX2NAVI_WATER_PROJECTIVE");
    if (!on || !P.indep || !s->fs) return 0;
    uint64_t wn, fh = gfx_shader_spirv_fnv(s->fs);
    for (size_t i = 0; i < sizeof water_profiles / sizeof *water_profiles; i++) {
        const WaterProfile *p = &water_profiles[i];
        // The absorption correction is independent of the coordinate pair.
        if (fh == p->mat) return water_module(p->mat_set1, WATER_ABSORPTION, p, s->fs, &wn) ? 2 : 0;
        if (fh != p->fs) continue;
        if (!s->vs || gfx_shader_spirv_fnv(s->vs) != p->vs) return 0;
        return water_module(p->vs, WATER_VERTEX, p, s->vs, &wn) && water_module(p->fs_set1, WATER_FRAGMENT, p, s->fs, &wn);
    }
    return 0;
}

// The fragment module of a pipeline in water mode `mode` (water_mode): the edited one for the module as it is built
// (in its set 1 form), else the module itself.
static VkShaderModule water_pair_module(const uint32_t *w, uint64_t n, int early, int mode) {
    uint64_t words = 0;
    const uint32_t *code = mode ? water_module(fnv(FNV0, w, (size_t)n * 4), 0, NULL, NULL, &words) : NULL;
    if (mode == 1 && !code) LOG_ONCE("vk_pipeline: water: a coordinate pair's fragment module is not in its known form; built as it is");
    return code ? make_module(code, words, early) : make_module(w, n, early);
}

// The vertex stage of a coordinate pair (mode 1): the edited module for the translation itself, taken before the
// outputs the vertex shader's union adds (spirv_add_outputs), which differ from one install to the next and are added
// to the edited module as to any other. NULL in every other case. The module stays the table's.
static const uint32_t *water_pair_vertex(const uint32_t *v, uint64_t n, int mode, uint64_t *words) {
    return mode == 1 && v ? water_module(fnv(FNV0, v, (size_t)n * 4), 0, NULL, NULL, words) : NULL;
}

// water_projective_pair for a pipeline of key k. A coordinate pair blended with dual-source factors keeps its original
// modules: spirv_dual_source changes the fragment module before its set 1 form, which is then none the table holds.
static int water_mode(const VkPipeState *s, const GKey *k) {
    int mode = water_projective_pair(s);
    return mode == 1 && k->dsb ? 0 : mode;
}

// ---- line width (NX2NAVI_LINE_WIDTH, default 1; 0: every line 1 pixel wide, as before)
// The game sets a width for its lines (nvnCommandBufferSetLineWidth, recorded as OP_BIND_STATE kind 7): 4.5 pixels
// for two line draws a frame into a 144x144 R8 target, which were drawn 1 pixel wide. A pipeline of the line topology
// class lists the width as dynamic state, in its whole build and in its pre-rasterization library (PrKey.lines), and
// each draw of lines records the width the game set, held to the device's range. Pipelines of the other classes are
// built as before, so their entries in the pipeline caches and the driver's stay the ones in use. A polygon mode that
// draws triangles as lines keeps 1 pixel: the mode can be dynamic state, which no pipeline key tells apart.
static int key_lines(const GKey *k) { return P.wide_lines && k->topo_class == 1; }

void vk_pipeline_line_width(VkCommandBuffer cb, const VkPipeState *s, float width) {
    if (!P.wide_lines || is_mesh(s) || topology_class(vk_pipeline_topology(s->prim)) != 1) return;
    float w = width > 0 ? width : 1.0f;
    if (P.line_gran > 0) w = P.line_w[0] + P.line_gran * (float)(int)((w - P.line_w[0]) / P.line_gran + 0.5f);
    w = w < P.line_w[0] ? P.line_w[0] : w > P.line_w[1] ? P.line_w[1] : w;
    static float logged;
    if (w != 1.0f && w != logged) {   // one line for each width in a row: the game uses one or two
        logged = w;
        rt_log("vk_pipeline: lines drawn %g pixels wide (the game set %g; the device draws %g to %g in steps of %g)",
               (double)w, (double)width, (double)P.line_w[0], (double)P.line_w[1], (double)P.line_gran);
    }
    vkCmdSetLineWidth(cb, w);
}

// What a mesh pipeline's task stage bindings are moved by in layout l: past the mesh stage's in a stage-independent
// layout, where the two share set 0 (mesh_pair_layout), and past the fragment stage's in the shared set (pipe_miss).
static uint32_t task_off(const VkPipeState *s, const Layout *l, uint32_t fs_off) {
    return l->set1 ? binding_span(s->vs) - 1 : fs_off + (s->fs ? binding_span(s->fs) - 1 : 0);
}

// The dynamic states of a mesh pipeline into out (room for P.n_dyn + 1): P.dyn without the vertex input, its strides,
// the topology and primitive restart, which a pipeline with a mesh stage must not list, and with the fragment
// shading rate where the device sets one per draw (vk_exec_draw.c mesh_shading_rate). Returns their number.
static uint32_t mesh_dyn(VkDynamicState *out) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < P.n_dyn; i++)
        if (P.dyn[i] != VK_DYNAMIC_STATE_VERTEX_INPUT_EXT && P.dyn[i] != VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE &&
            P.dyn[i] != VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY && P.dyn[i] != VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE)
            out[n++] = P.dyn[i];
    if (vk_ctx()->has_vrs) out[n++] = VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR;
    return n;
}

static VkPipeline create_graphics(const VkPipeState *s, const GKey *k, const Layout *l, uint32_t fs_off) {
    VkShaderModule vm = VK_NULL_HANDLE, fm = VK_NULL_HANDLE;
    uint64_t vw, fw, vm_hash = 0, fm_hash = 0;   // the hashes: of the final modules, for the PIPE_FEEDBACK 2 line
    // The stages' translations, pinned until the pipeline is made (gfx.h gfx_shader_spirv_pin): vcode and fcode become
    // patched copies on the way, vplain and fplain stay the translations.
    uint64_t vw0 = 0, fw0 = 0;
    const uint32_t *vplain = gfx_shader_spirv_pin(s->vs, &vw0), *fplain = s->fs ? gfx_shader_spirv_pin(s->fs, &fw0) : NULL;
    const uint32_t *vcode = vplain;
    vw = vw0;
    uint32_t *vpatched = NULL, *fpatched = NULL;
    int mesh = is_mesh(s), emu = mesh && l->emu;
    // the water pair's vertex edit is for a translated vertex stage: an emulated mesh draw's generated one has none
    int wmode = mesh || !l->set1 ? 0 : water_mode(s, k);   // the edited fragment modules are in their set 1 form
    uint64_t wn = 0;
    const uint32_t *vwater = water_pair_vertex(vcode, vw, wmode, &wn);
    if (vwater) vcode = vwater, vw = wn;
    if (emu) {   // NX2NAVI_MESH_EMULATION: the task and mesh stages as compute pipelines, a generated vertex shader here
        uint32_t toff = fs_off + (s->fs ? binding_span(s->fs) - 1 : 0);
        const MeshEmuEnt *me = emu_get(s->vs, s->ts, l->layout, toff, 1);
        if (!me || !me->ok) {
            gfx_shader_spirv_unpin(s->vs);
            if (s->fs) gfx_shader_spirv_unpin(s->fs);
            return VK_NULL_HANDLE;
        }
        GfxShaderIo fin[32];
        uint8_t ty[32], cp[32];
        uint32_t need = s->fs ? gfx_shader_inputs(s->fs, fin) : 0;
        for (int i = 0; i < 32; i++) {
            ty[i] = need >> i & 1 && !fin[i].packed && fin[i].type <= GFX_TYPE_UINT ? (uint8_t)fin[i].type : 0;
            cp[i] = need >> i & 1 && !fin[i].packed && fin[i].comps ? (uint8_t)fin[i].comps : 4;
        }
        size_t vn = 0;
        vpatched = mesh_emu_vs(&me->mi, need, ty, cp, fplain, (size_t)fw0, &vn);
        if (!vpatched) {
            gfx_shader_spirv_unpin(s->vs);
            if (s->fs) gfx_shader_spirv_unpin(s->fs);
            return VK_NULL_HANDLE;
        }
        vcode = vpatched, vw = vn;
        mesh = 0;   // a vertex pipeline from here on
    }
    VkShaderModule tm = VK_NULL_HANDLE;
    if (mesh && s->ts) {   // the task stage's bindings follow another stage's in its set (task_off)
        uint64_t tw;
        const uint32_t *tcode = gfx_shader_spirv_pin(s->ts, &tw);
        uint32_t toff = task_off(s, l, fs_off);
        uint32_t *t = tcode && toff ? spirv_rebind(tcode, tw, toff) : NULL;
        tm = make_module(t ? t : tcode, tw, 0);
        free(t);
        gfx_shader_spirv_unpin(s->ts);
    }
    if (l->set1 && !mesh) {   // independent layouts: outputs from the vertex shader's union, not from this fragment shader
        GfxShaderIo fin[32];
        uint32_t missing = vs_union_missing(s, fin);
        if (missing) {
            uint64_t n2 = 0;
            vpatched = spirv_add_outputs(vcode, vw, missing, fin, &n2);
            if (!vpatched) LOG_ONCE("vk_pipeline: cannot add missing vertex outputs (mask %#x)", missing);
            else vcode = vpatched, vw = n2;
        }
    }
    if (s->fs) {
        GfxShaderIo fin[32], vout[32];
        // a mesh stage's outputs from its module (spirv_mesh_outputs): the translation reflects none; an emulated one
        // (mesh_emu) writes what the fragment stage reads
        int points = 0;
        uint32_t need = gfx_shader_inputs(s->fs, fin), missing = 0,
                 have = mesh ? (vcode ? spirv_mesh_outputs(vcode, vw, &points) : ~0u) : emu ? ~0u : gfx_shader_outputs(s->vs, vout);
        for (int i = 0; i < 32; i++) {
            if (!(need >> i & 1) || fin[i].packed) continue;
            if (!(have >> i & 1)) missing |= 1u << i;
            else if (!mesh && !emu && vout[i].type != fin[i].type)
                LOG_ONCE("vk_pipeline: varying %d is type %d in the vertex stage, %d in the fragment stage", i, vout[i].type, fin[i].type);
        }
        if (missing && !l->set1 && !mesh) {
            uint64_t n2 = 0;
            vpatched = spirv_add_outputs(vcode, vw, missing, fin, &n2);
            if (!vpatched) LOG_ONCE("vk_pipeline: cannot add missing vertex outputs (mask %#x)", missing);
            else vcode = vpatched, vw = n2;
        }
        const uint32_t *fcode = fplain;
        fw = fw0;
        if (missing && mesh && fcode) {   // inputs no mesh output feeds read as Maxwell gives them (spirv_fs_fill_inputs)
            uint64_t n2 = 0;
            uint32_t *f = spirv_fs_fill_inputs(fcode, fw, missing, points, &n2);
            if (f) fcode = fpatched = f, fw = n2;
            else LOG_ONCE("vk_pipeline: cannot fill fragment inputs the mesh stage lacks (mask %#x)", missing);
        }
        if (k->dsb) {
            uint64_t n2 = 0;
            uint32_t *d = spirv_dual_source(fcode, fw, &n2);
            if (d) free(fpatched), fcode = fpatched = d, fw = n2;
            else LOG_ONCE("vk_pipeline: SRC1 blend factors but the fragment shader writes no colour output 1");
        }
        if (l->set1 || fs_off) {
            uint32_t *r = !fcode ? NULL : l->set1 ? spirv_set(fcode, fw, 1) : spirv_rebind(fcode, fw, fs_off);
            free(fpatched);
            fcode = fpatched = r;
        }
        // A bound colour target outside the shader's output map gets write mask 0 (target_wmask)
        if (fcode) fm = water_pair_module(fcode, fw, !k->a2c, wmode), fm_hash = mm_hash;
    }
    vm = make_module(vcode, vw, 0), vm_hash = mm_hash;
    free(vpatched), free(fpatched);
    VkPipeline pipe = VK_NULL_HANDLE;
    if (!vm || (s->fs && !fm) || (mesh && s->ts && !tm)) goto out;

    VkPipelineShaderStageCreateInfo stages[3] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = mesh ? VK_SHADER_STAGE_MESH_BIT_EXT : VK_SHADER_STAGE_VERTEX_BIT,
         .module = vm, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_TASK_BIT_EXT, .module = tm, .pName = "main"}};
    uint32_t n_stages = 1;
    if (s->fs) n_stages = 2;
    if (tm) {
        if (!s->fs) stages[1] = stages[2];
        n_stages++;
    }
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rqv, rqf;
    if (!emu) pin_subgroup32(&stages[0], &rqv, vplain, vw0);   // vcode may be a patched copy that is freed by now
    if (s->fs) pin_subgroup32(&stages[1], &rqf, fplain, fw0);
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    uint32_t na = 0, nb = 0;
    if (!P.dyn_vi) {
        VtxInput v;
        vertex_input(s, &v);
        for (uint32_t i = 0; i < v.n_attr; i++) va[na++] = (VkVertexInputAttributeDescription){v.a[i].location, v.a[i].binding, v.a[i].format, v.a[i].offset};
        for (uint32_t i = 0; i < v.n_bind; i++) vb[nb++] = (VkVertexInputBindingDescription){v.b[i].binding, v.b[i].stride, v.b[i].inputRate};
    }
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = va};
    static const VkPrimitiveTopology class_base[4] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                                      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_PATCH_LIST};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = class_base[k->topo_class]};
    VkPipelineViewportDepthClipControlCreateInfoEXT clip = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT,
                                                            .negativeOneToOne = k->minus_w};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .pNext = &clip};
    // NVN's default is the last vertex (SetProvokingVertexMode, nvn_ext.c); Vulkan's is the first.
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pvs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
        .provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .pNext = P.pv_last ? &pvs : NULL,
                                                 .depthClampEnable = k->clamp, .polygonMode = (VkPolygonMode)k->poly_mode,
                                                 .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples,
                                               .alphaToCoverageEnable = k->a2c};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .maxDepthBounds = 1.0f};
    VkPipelineColorBlendAttachmentState cba[8];
    memset(cba, 0, sizeof cba);
    for (int i = 0; i < k->n_color; i++) {
        const uint8_t *b = k->blend[i];
        cba[i].blendEnable = k->blend_en >> i & 1;
        cba[i].srcColorBlendFactor = (VkBlendFactor)b[0], cba[i].dstColorBlendFactor = (VkBlendFactor)b[1];
        cba[i].colorBlendOp = (VkBlendOp)b[2], cba[i].srcAlphaBlendFactor = (VkBlendFactor)b[3];
        cba[i].dstAlphaBlendFactor = (VkBlendFactor)b[4], cba[i].alphaBlendOp = (VkBlendOp)b[5];
        cba[i].colorWriteMask = k->wmask >> (4 * i) & 0xf;
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .logicOpEnable = k->logic_op != 0,
                                              .logicOp = k->logic_op ? (VkLogicOp)(k->logic_op - 1) : VK_LOGIC_OP_COPY,
                                              .attachmentCount = k->n_color, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = P.n_dyn, .pDynamicStates = P.dyn};
    VkDynamicState mdyn[sizeof P.dyn / sizeof P.dyn[0] + 1];
    if (!mesh && key_lines(k)) {   // lines take the game's width per draw (vk_pipeline_line_width)
        memcpy(mdyn, P.dyn, P.n_dyn * sizeof *mdyn);
        mdyn[P.n_dyn] = VK_DYNAMIC_STATE_LINE_WIDTH;
        dy.dynamicStateCount = P.n_dyn + 1, dy.pDynamicStates = mdyn;
    }
    if (mesh) dy.dynamicStateCount = mesh_dyn(mdyn), dy.pDynamicStates = mdyn;
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->n_color,
                                        .pColorAttachmentFormats = (const VkFormat *)k->color,
                                        .depthAttachmentFormat = (VkFormat)k->depth, .stencilAttachmentFormat = (VkFormat)k->stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .stageCount = n_stages, .pStages = stages, .pVertexInputState = mesh ? NULL : &vi,
                                       .pInputAssemblyState = mesh ? NULL : &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
                                       .pDynamicState = &dy, .layout = l->layout, .flags = noopt_flag_for(mesh)};
    if (k->fbl & 1) pi.flags |= VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    if (k->fbl & 2) pi.flags |= VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    VkPipelineCreationFeedbackCreateInfo fci;
    VkPipelineCreationFeedback fb;
    pi.pNext = fb_chain(&fci, &fb, pi.pNext);
    uint64_t p0 = perf_begin();
    crash_note("a graphics pipeline", s->vs, s->fs, s->ts);
    VkResult r = pipe_create(0, &pi, &pipe);
    if (r == VK_ERROR_UNKNOWN && !(pi.flags & VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT)) {
        // A compiler that gives up on a pipeline (AMD's Windows driver answers VK_ERROR_UNKNOWN) may still build it
        // without its optimiser: one more try that way, slower shaders for that pipeline instead of a draw left out.
        static int told;
        if (!told++) rt_log("vk_pipeline: the driver could not build a graphics pipeline (VK_ERROR_UNKNOWN): built again "
                            "without its optimiser (logged once)");
        pi.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
        r = pipe_create(0, &pi, &pipe);
    }
    plat_crash_context(NULL);
    perf_end(PERF_PIPE, p0);   // counts on the render thread only, so background builds stay out of it
    if (r != VK_SUCCESS) {
        rt_log("vk_pipeline: vkCreateGraphicsPipelines failed (%d)", (int)r);
        pipe = VK_NULL_HANDLE;
    } else {
        perf_count(PERF_CNT_PIPE_BUILT);
        fb_note(FB_GRAPHICS, &fb);
        if (P.feedback >= 2) {   // the state hash: what the create info takes from the key besides the modules
            uint64_t st = fnv(fnv(FNV0, cba, sizeof cba), k->color, sizeof k->color);
            if (!P.dyn_vi)   // the vertex layout is pipeline state then (zero in a mesh pipeline's key)
                st = fnv(fnv(fnv(fnv(st, k->attr_fmt, sizeof k->attr_fmt), k->attr_off, sizeof k->attr_off),
                             k->attr_bind, sizeof k->attr_bind), k->rate, sizeof k->rate);
            uint32_t bits[] = {(uint32_t)mesh, k->topo_class, k->minus_w, k->clamp, k->poly_mode, k->samples, k->a2c,
                               k->logic_op, k->n_color, k->fbl, k->dsb, l->set1 != 0, fs_off, pi.flags, k->depth, k->stencil};
            rt_log("vk_pipeline: graphics: modules %016llx %016llx, state %016llx (mesh %d, topology class %u, samples %u, "
                   "colors %u, feedback loop %u, depth format %u, stage-independent layout %d, flags 0x%x), %s, %.1f ms",
                   (unsigned long long)vm_hash, (unsigned long long)fm_hash, (unsigned long long)fnv(st, bits, sizeof bits),
                   mesh, (unsigned)k->topo_class, (unsigned)k->samples, (unsigned)k->n_color, (unsigned)k->fbl,
                   (unsigned)k->depth, l->set1 != 0, (unsigned)pi.flags,
                   pb_last ? "from pipeline-binaries.bin" : fb.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT ? "from pipeline-cache.bin" : "compiled",
                   (double)fb.duration / 1e6);
        }
    }
out:
    if (vm) vkDestroyShaderModule(P.device, vm, NULL);
    if (fm) vkDestroyShaderModule(P.device, fm, NULL);
    if (tm) vkDestroyShaderModule(P.device, tm, NULL);
    gfx_shader_spirv_unpin(s->vs);
    if (s->fs) gfx_shader_spirv_unpin(s->fs);
    return pipe;
}

// ---- graphics pipeline library (vk_caps.gpl)
// A pipeline is linked from four libraries, each cached by the part of the pipeline key it depends on: vertex input
// (topology class, vertex attributes without dynamic vertex input), pre-rasterization (vertex shader, the outputs its
// union adds, static rasterization state), fragment shader (fragment shader, dual-source rewrite, samples) and fragment
// output (targets, blend, samples). With independent layouts each stage's set depends on its shader alone, so the
// pre-rasterization library uses {set 0, null} and the fragment shader library {null, set 1}. The fast link runs on the
// thread of the draw that needs it (no link-time optimisation); the optimised pipeline is the job pipe_miss queued
// (a monolithic build with the same modules), which replaces the fast one in the Pipe when done. Libraries and fast
// pipelines live until vk_pipeline_shutdown. No library is made to be optimised at its link: the optimised pipeline is
// that whole build, so the links take what the libraries hold as it is.
// A mesh pipeline is linked from three of them, its pre-rasterization library holding the task and mesh stages
// (comment at mesh_lib_init).
// Background build signalling, both with P.lock: work_cv wakes the pipeline workers, done_cv wakes threads waiting for a
// pipeline or a stage library to be published.
static PlatCond work_cv = PLAT_COND_INIT, done_cv = PLAT_COND_INIT;
static PlatCond lib_cv = PLAT_COND_INIT;   // wakes the library workers (lib_worker): work_cv's signal must reach a pool worker
#define LIB_BUCKETS 1024
enum { LIB_VI, LIB_PR, LIB_FS, LIB_FO };
typedef struct Lib { struct Lib *next; uint64_t hash; VkPipeline pipe; uint32_t size; uint8_t pre, used, building, pad;
                     struct LibJob *job; uint8_t key[]; } Lib;
// pre: built by the boot prebuild ("stage library prebuild" below); used: a draw's link found it; building: a thread
// builds it or job holds it queued (pipe not set yet); entries live until libs_destroy
static Lib *libs[LIB_BUCKETS];

typedef struct { uint32_t kind, topo_class, attr_fmt[32], attr_off[32]; uint8_t attr_bind[32], rate[17], pad[3]; } ViKey;
typedef struct { uint32_t kind, missing; const GfxShader *vs; uint8_t clamp, poly_mode, minus_w, water_projective;
                 uint8_t lines;   // key_lines: the library lists the line width as dynamic state
                 const GfxShader *ts;   // a mesh pipeline's library (vs is its mesh stage): its task stage, or NULL
                 GfxShaderIo fin[32]; } PrKey;
// fill, points, vrs: zero for a vertex pipeline. A mesh pipeline's: the inputs filled in the module and whether as a
// point sprite's (mesh_fill), and 1 when the library lists the fragment shading rate as dynamic state (mesh_dyn).
typedef struct { uint32_t kind; uint8_t dsb, samples, a2c, water_projective; const GfxShader *fs;
                 uint32_t fill; uint8_t points, vrs, pad[2]; } FsKey;
typedef struct { uint32_t kind; uint8_t n_color, samples, a2c, logic_op, blend_en, fbl, pad[2]; uint32_t color[8], depth, stencil, wmask;
                 uint8_t blend[8][6]; } FoKey;   // fbl: GKey.fbl, the feedback loop flags of the library (gpl_feedback)

static uint64_t qpc(void) { return plat_now_ns(); }
static double ms_since(uint64_t t0) { return (double)(qpc() - t0) / 1e6; }

static VkPipelineLayout half_layout(Layout *l, int h) {
    if (l->half[h]) return l->half[h];
    VkDescriptorSetLayout sets[3] = {h ? VK_NULL_HANDLE : l->set, h ? l->set1 : VK_NULL_HANDLE, vk_pool_set_layout()};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT, .setLayoutCount = 3,
                                     .pSetLayouts = sets};
    if (vkCreatePipelineLayout(P.device, &pi, NULL, &l->half[h]) != VK_SUCCESS) l->half[h] = VK_NULL_HANDLE;
    return l->half[h];
}

// Builds one library from the pipeline's state; key is the library's key (PrKey: fin holds the added outputs).
static VkPipeline lib_create(int kind, const VkPipeState *s, const GKey *k, VkPipelineLayout lay, const void *key) {
    static const VkGraphicsPipelineLibraryFlagsEXT part[4] = {
        VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
        VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT};
    VkGraphicsPipelineLibraryCreateInfoEXT gl = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, .flags = part[kind]};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .pNext = &gl};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = P.n_dyn, .pDynamicStates = P.dyn};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | noopt_flag(), .pDynamicState = &dy};
    VkDynamicState ldyn[sizeof P.dyn / sizeof P.dyn[0] + 1];
    int mesh = is_mesh(s);   // the libraries of a mesh pipeline (comment at mesh_lib_init)
    if (kind == LIB_PR && mesh) {   // what a whole mesh pipeline lists: as create_graphics
        dy.dynamicStateCount = mesh_dyn(ldyn), dy.pDynamicStates = ldyn;
        pi.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | noopt_flag_for(1);
    } else if (kind == LIB_PR && ((const PrKey *)key)->lines) {   // the rasterization state is this library's: as create_graphics
        memcpy(ldyn, P.dyn, P.n_dyn * sizeof *ldyn);
        ldyn[P.n_dyn] = VK_DYNAMIC_STATE_LINE_WIDTH;
        dy.dynamicStateCount = P.n_dyn + 1, dy.pDynamicStates = ldyn;
    } else if (kind == LIB_FS && ((const FsKey *)key)->vrs) {
        // The shading rate is part of the pre-rasterization and of the fragment shader state: a mesh pipeline's
        // libraries of both kinds list it as dynamic state, as a state dynamic in one library has to be in every
        // other library that holds it. Such a library is a mesh pipeline's alone, so it takes that pipeline's list.
        dy.dynamicStateCount = mesh_dyn(ldyn), dy.pDynamicStates = ldyn;
    }
    VkShaderModule m = VK_NULL_HANDLE, tm = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pName = "main"};
    VkPipelineShaderStageCreateInfo mst[2];   // a mesh pipeline's pre-rasterization stages: mesh, then task
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rq;
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    static const VkPrimitiveTopology class_base[4] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                                      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_PATCH_LIST};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = class_base[k->topo_class]};
    VkPipelineViewportDepthClipControlCreateInfoEXT clip = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT,
                                                            .negativeOneToOne = k->minus_w};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .pNext = &clip};
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pvs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
        .provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .pNext = P.pv_last ? &pvs : NULL, .depthClampEnable = k->clamp,
                                                 .polygonMode = (VkPolygonMode)k->poly_mode, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples,
                                               .alphaToCoverageEnable = k->a2c};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .maxDepthBounds = 1.0f};
    VkPipelineColorBlendAttachmentState cba[8];
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    uint32_t *patched = NULL;
    VkPipeline pipe = VK_NULL_HANDLE;
    const GfxShader *pinned[2] = {NULL, NULL};   // the shaders whose SPIR-V is pinned until out (gfx_shader_spirv_pin)
    if (kind == LIB_VI) {
        if (!P.dyn_vi) {
            VtxInput v;
            vertex_input(s, &v);
            for (uint32_t i = 0; i < v.n_attr; i++)
                va[i] = (VkVertexInputAttributeDescription){v.a[i].location, v.a[i].binding, v.a[i].format, v.a[i].offset};
            for (uint32_t i = 0; i < v.n_bind; i++)
                vb[i] = (VkVertexInputBindingDescription){v.b[i].binding, v.b[i].stride, v.b[i].inputRate};
            vi.vertexAttributeDescriptionCount = v.n_attr, vi.pVertexAttributeDescriptions = va;
            vi.vertexBindingDescriptionCount = v.n_bind, vi.pVertexBindingDescriptions = vb;
        }
        pi.pVertexInputState = &vi, pi.pInputAssemblyState = &ia;
    } else if (kind == LIB_PR && mesh) {
        // The task and mesh stages as create_graphics builds them: the mesh module as translated, the task module with
        // its bindings moved past the mesh stage's (task_off in a stage-independent layout); the mesh stage alone
        // takes the subgroup pin there, so here too. No vertex outputs are added: what a fragment shader reads and
        // the mesh stage does not write is filled in the fragment module (FsKey.fill).
        uint64_t vw, tw = 0;
        const uint32_t *vcode = gfx_shader_spirv_pin(pinned[0] = s->vs, &vw);
        if (!vcode || !(m = make_module(vcode, vw, 0))) goto out;
        mst[0] = st, mst[0].stage = VK_SHADER_STAGE_MESH_BIT_EXT, mst[0].module = m;
        pin_subgroup32(&mst[0], &rq, vcode, vw);
        pi.stageCount = 1;
        if (s->ts) {
            const uint32_t *tcode = gfx_shader_spirv_pin(pinned[1] = s->ts, &tw);
            uint32_t toff = binding_span(s->vs) - 1;
            patched = tcode && toff ? spirv_rebind(tcode, tw, toff) : NULL;
            if (!tcode || !(tm = make_module(patched ? patched : tcode, tw, 0))) goto out;
            mst[1] = st, mst[1].stage = VK_SHADER_STAGE_TASK_BIT_EXT, mst[1].module = tm;
            pi.stageCount = 2;
        }
        pi.pStages = mst, pi.pViewportState = &vp, pi.pRasterizationState = &rs;
        pi.layout = lay;
    } else if (kind == LIB_PR) {
        const PrKey *pk = key;
        uint64_t vw, sw, n2 = 0;
        const uint32_t *vcode = gfx_shader_spirv_pin(pinned[0] = s->vs, &vw), *scode = vcode;
        sw = vw;
        uint64_t wn = 0;
        const uint32_t *vwater = water_pair_vertex(vcode, vw, pk->water_projective, &wn);
        if (vwater) vcode = vwater, vw = wn;
        if (pk->missing && (patched = spirv_add_outputs(vcode, vw, pk->missing, pk->fin, &n2))) vcode = patched, vw = n2;
        else if (pk->missing) LOG_ONCE("vk_pipeline: cannot add missing vertex outputs (mask %#x)", pk->missing);
        if (!(m = make_module(vcode, vw, 0))) goto out;
        st.stage = VK_SHADER_STAGE_VERTEX_BIT, st.module = m;
        pin_subgroup32(&st, &rq, scode, sw);
        pi.stageCount = 1, pi.pStages = &st, pi.pViewportState = &vp, pi.pRasterizationState = &rs;
        pi.layout = lay;
    } else if (kind == LIB_FS) {
        if (s->fs) {
            const FsKey *fk = key;
            uint64_t fw, sw, n2 = 0;
            const uint32_t *fcode = gfx_shader_spirv_pin(pinned[0] = s->fs, &fw), *scode = fcode;
            sw = fw;
            uint32_t *d = NULL, *filled = NULL;
            if (fk->fill && fcode) {   // a mesh pipeline's: before the other rewrites, as create_graphics
                if ((filled = spirv_fs_fill_inputs(fcode, fw, fk->fill, fk->points, &n2))) fcode = filled, fw = n2;
                else LOG_ONCE("vk_pipeline: cannot fill fragment inputs the mesh stage lacks (mask %#x)", fk->fill);
            }
            if (k->dsb && fcode) {
                if ((d = spirv_dual_source(fcode, fw, &n2))) fcode = d, fw = n2;
                else LOG_ONCE("vk_pipeline: SRC1 blend factors but the fragment shader writes no colour output 1");
            }
            patched = fcode ? spirv_set(fcode, fw, 1) : NULL;
            free(d), free(filled);
            if (!patched || !(m = water_pair_module(patched, fw, !k->a2c, fk->water_projective))) goto out;
            st.stage = VK_SHADER_STAGE_FRAGMENT_BIT, st.module = m;
            pin_subgroup32(&st, &rq, scode, sw);
            pi.stageCount = 1, pi.pStages = &st;
        }
        pi.pMultisampleState = &ms, pi.pDepthStencilState = &ds;
        pi.layout = lay;
    } else {
        memset(cba, 0, sizeof cba);
        for (int i = 0; i < k->n_color; i++) {
            const uint8_t *b = k->blend[i];
            cba[i].blendEnable = k->blend_en >> i & 1;
            cba[i].srcColorBlendFactor = (VkBlendFactor)b[0], cba[i].dstColorBlendFactor = (VkBlendFactor)b[1];
            cba[i].colorBlendOp = (VkBlendOp)b[2], cba[i].srcAlphaBlendFactor = (VkBlendFactor)b[3];
            cba[i].dstAlphaBlendFactor = (VkBlendFactor)b[4], cba[i].alphaBlendOp = (VkBlendOp)b[5];
            cba[i].colorWriteMask = k->wmask >> (4 * i) & 0xf;
        }
        cb.logicOpEnable = k->logic_op != 0, cb.logicOp = k->logic_op ? (VkLogicOp)(k->logic_op - 1) : VK_LOGIC_OP_COPY;
        cb.attachmentCount = k->n_color, cb.pAttachments = cba;
        ri.colorAttachmentCount = k->n_color, ri.pColorAttachmentFormats = (const VkFormat *)k->color;
        ri.depthAttachmentFormat = (VkFormat)k->depth, ri.stencilAttachmentFormat = (VkFormat)k->stencil;
        pi.pColorBlendState = &cb, pi.pMultisampleState = &ms;
        // Whether the pipeline may sample an attachment it renders to is part of the fragment output state: the flags
        // go on this library alone, and a pipeline linked from it has them (gpl_feedback).
        const FoKey *ok = key;
        if (ok->fbl & 1) pi.flags |= VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
        if (ok->fbl & 2) pi.flags |= VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    }
    VkPipelineCreationFeedbackCreateInfo fci;
    VkPipelineCreationFeedback fb;
    pi.pNext = fb_chain(&fci, &fb, pi.pNext);
    static const char *const kind_name[4] = {"a vertex input library", "a pre-rasterization library",
                                             "a fragment shader library", "a fragment output library"};
    crash_note(kind == LIB_PR && mesh ? "a pre-rasterization library of task and mesh stages" : kind_name[kind], s->vs,
               s->fs, mesh ? s->ts : NULL);
    if (pipe_create(1, &pi, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
    plat_crash_context(NULL);
    if (pipe && (kind == LIB_PR || kind == LIB_FS)) fb_note(FB_LIBRARY, &fb);   // the shader libraries: the others compile nothing
out:
    if (m) vkDestroyShaderModule(P.device, m, NULL);
    if (tm) vkDestroyShaderModule(P.device, tm, NULL);
    free(patched);
    for (int i = 0; i < 2; i++)
        if (pinned[i]) gfx_shader_spirv_unpin(pinned[i]);
    return pipe;
}

// The cached library for key. Caller holds P.lock. build 0: lookup only (VK_NULL_HANDLE and *miss when absent);
// build 1: a missing one is built with P.lock released (a failed build is cached too).
// The library entry for key, NULL when absent. Caller holds P.lock.
static Lib *lib_find(uint64_t h, const void *key, uint32_t size) {
    for (Lib *b = libs[h % LIB_BUCKETS]; b; b = b->next)
        if (b->hash == h && b->size == size && !memcmp(b->key, key, size)) return b;
    return NULL;
}

// Enters a library built with P.lock released; another thread's entry made meanwhile wins and pipe is destroyed.
// Returns the entered handle. Caller holds P.lock.
static VkPipeline lib_put(uint64_t h, const void *key, uint32_t size, VkPipeline pipe) {
    Lib *b = lib_find(h, key, size);
    if (b && b->building) {   // another thread's build publishes the entry
        if (pipe) vkDestroyPipeline(P.device, pipe, NULL);
        return VK_NULL_HANDLE;
    }
    if (b) {
        if (pipe) vkDestroyPipeline(P.device, pipe, NULL);
        return b->pipe;
    }
    if (!(b = calloc(1, sizeof *b + size))) return pipe;
    b->hash = h, b->size = size, b->pipe = pipe;
    memcpy(b->key, key, size);
    b->next = libs[h % LIB_BUCKETS];
    libs[h % LIB_BUCKETS] = b;
    return pipe;
}

// Shader libraries a waiting draw needs (lib_wait, link_queue): queued here ahead of every pipeline job, so an idle
// worker builds each at once and the vertex and fragment libraries of one draw build in parallel. A job no worker took
// is built by the first thread that needs its library (lib_get; lib_wait only without library workers). A job with p
// is a waiting draw's fast link of p (after its library jobs), run on a worker so the drawing thread only waits.
// Under P.lock.
typedef struct LibJob { struct LibJob *next; Lib *b; int kind; Layout *l; VkPipeState s; GKey k; Pipe *p; } LibJob;
static struct {
    LibJob *head, *tail;
    int idle;                          // workers asleep on work_cv
    int running;                       // jobs workers are building
    int on;                            // NX2NAVI_PIPE_WAIT: 1 workers (default), 0 the drawing thread builds them (draw);
                                       // set by vk_pipeline_async_enable, before the replay runs
    int lib_workers;                   // workers that take only these jobs (lib_worker), started with the pool
    uint64_t waits, queued, by_worker, stolen, timeouts, links;
} U;

// A building entry for key, NULL when out of memory. Caller holds P.lock and found none.
static Lib *lib_new(uint64_t h, const void *key, uint32_t size) {
    Lib *b = calloc(1, sizeof *b + size);
    if (!b) return NULL;
    b->hash = h, b->size = size, b->building = 1;
    memcpy(b->key, key, size);
    b->next = libs[h % LIB_BUCKETS];
    libs[h % LIB_BUCKETS] = b;
    return b;
}

// Builds b (building, not queued) with P.lock released and publishes it; a failed build is entered as VK_NULL_HANDLE.
static VkPipeline lib_build(Lib *b, int kind, const VkPipeState *s, const GKey *k, Layout *l) {
    VkPipelineLayout lay = kind == LIB_PR ? half_layout(l, 0) : kind == LIB_FS ? half_layout(l, 1) : VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (lay || kind == LIB_VI || kind == LIB_FO) {
        plat_mutex_unlock(&P.lock);
        uint64_t t0 = qpc();
        pipe = lib_create(kind, s, k, lay, b->key);
        double ms = ms_since(t0);
        plat_mutex_lock(&P.lock);
        P.g.lib_n[kind]++, P.g.lib_ms[kind] += ms;
        if (ms > P.g.lib_max[kind]) P.g.lib_max[kind] = ms;
        int mesh = is_mesh(s) && (kind == LIB_PR || kind == LIB_FS);   // a mesh pipeline's shader library
        if (mesh) {
            int i = kind == LIB_FS;
            P.g.m_lib_n[i]++, P.g.m_lib_ms[i] += ms;
            if (ms > P.g.m_lib_max[i]) P.g.m_lib_max[i] = ms;
        }
        int added = kind == LIB_PR && ((const PrKey *)(const void *)b->key)->missing;
        if (added) P.g.pr_added++, P.g.pr_added_ms += ms;
        if (ms >= 25) P.g.lib_slow[kind]++, P.g.lib_slow_ms[kind] += ms;
        // A shader library that took this long was not in the driver's cache; why not, for the first 32 of a run
        if (ms >= 25 && (kind == LIB_PR || kind == LIB_FS) && P.g.lib_logged < 32) {
            const GfxShader *sh = kind == LIB_PR ? s->vs : s->fs;
            const uint8_t *id = sh ? gfx_shader_input_id(sh) : NULL;
            P.g.lib_logged++;
            rt_log("vk_pipeline: gpl: a %s library took %.0f ms: shader %02x%02x%02x%02x, %s%s",
                   kind == LIB_FS ? (mesh ? "fragment shader (of a mesh pipeline)" : "fragment shader")
                                  : mesh ? "pre-rasterization (task and mesh stages)" : "pre-rasterization",
                   ms, id ? id[0] : 0, id ? id[1] : 0,
                   id ? id[2] : 0, id ? id[3] : 0, origin_name(sh),
                   added ? "; outputs were added for its fragment shader, a module the preparation did not build"
                   : sh && gfx_shader_origin(sh) == 2 ? "; nothing was prepared from a shader first translated now"
                   : "; the driver's cache did not have it, or the build waited behind others in the driver");
        }
    }
    if (!pipe && ++P.g.lib_fail <= 8) rt_log("vk_pipeline: gpl: stage library %d not built", kind);
    b->pipe = pipe, b->building = 0;
    plat_cond_broadcast(&done_cv);
    return pipe;
}

// Takes b's queued job out of the queue and builds it on this thread. Caller holds P.lock.
static void lib_steal(Lib *b) {
    LibJob *j = b->job, **pp = &U.head, *prev = NULL;
    while (*pp && *pp != j) prev = *pp, pp = &(*pp)->next;
    if (*pp) *pp = j->next;
    if (U.tail == j) U.tail = prev;
    b->job = NULL;
    lib_build(b, j->kind, &j->s, &j->k, j->l);
    free(j);
}

static VkPipeline lib_get(int kind, const VkPipeState *s, const GKey *k, Layout *l, const void *key, uint32_t size,
                          int build, int *miss) {
    uint64_t h = key_hash(key, size);
    Lib *have = lib_find(h, key, size);
    while (have && have->building) {   // being built (wait) or queued (build it here)
        if (!build) {
            *miss = 1;
            return VK_NULL_HANDLE;
        }
        if (have->job) U.stolen++, lib_steal(have);
        else plat_cond_wait(&done_cv, &P.lock);
    }
    if (have) {
        if (have->pre && !have->used) have->used = 1, P.g.pre_used[kind]++;
        return have->pipe;
    }
    VkPipelineLayout lay = kind == LIB_PR ? half_layout(l, 0) : kind == LIB_FS ? half_layout(l, 1) : VK_NULL_HANDLE;
    if (!build || ((kind == LIB_PR || kind == LIB_FS) && !lay)) {
        *miss = 1;
        return VK_NULL_HANDLE;
    }
    Lib *b = lib_new(h, key, size);   // entered first, so other threads wait for this build instead of repeating it
    if (b) return lib_build(b, kind, s, k, l);
    plat_mutex_unlock(&P.lock);
    uint64_t t0 = qpc();
    VkPipeline pipe = lib_create(kind, s, k, lay, key);
    double ms = ms_since(t0);
    plat_mutex_lock(&P.lock);
    if ((have = lib_find(h, key, size)) && !have->building) {   // another thread built it meanwhile
        if (pipe) vkDestroyPipeline(P.device, pipe, NULL);
        return have->pipe;
    }
    P.g.lib_n[kind]++, P.g.lib_ms[kind] += ms;
    if (ms > P.g.lib_max[kind]) P.g.lib_max[kind] = ms;
    if (!pipe && ++P.g.lib_fail <= 8) rt_log("vk_pipeline: gpl: stage library %d not built", kind);
    return lib_put(h, key, size, pipe);
}

static void libs_destroy(void) {
    for (int i = 0; i < LIB_BUCKETS; i++) {
        for (Lib *b = libs[i], *n; b; b = n) {
            n = b->next;
            if (b->pipe) vkDestroyPipeline(P.device, b->pipe, NULL);
            free(b);
        }
        libs[i] = NULL;
    }
}

// The four library keys of pipeline s (key k). A mesh pipeline (one with stage libraries: no_libs is 0) has no use
// for the first, and what tells its two shader libraries from a vertex pipeline's is here: the pre-rasterization
// key names the mesh stage and the task stage and adds no outputs (a mesh shader is in no vertex output union), the
// fragment key carries what is filled in the module and the dynamic shading rate (FsKey). Neither takes a water
// correction, as create_graphics gives a mesh pipeline none.
static void lib_keys(const VkPipeState *s, const GKey *k, ViKey *v, PrKey *pk, FsKey *fk, FoKey *ok) {
    int mesh = is_mesh(s);
    memset(v, 0, sizeof *v);
    v->kind = LIB_VI, v->topo_class = k->topo_class;
    memcpy(v->attr_fmt, k->attr_fmt, sizeof v->attr_fmt), memcpy(v->attr_off, k->attr_off, sizeof v->attr_off);
    memcpy(v->attr_bind, k->attr_bind, sizeof v->attr_bind), memcpy(v->rate, k->rate, sizeof v->rate);
    memset(pk, 0, sizeof *pk);
    GfxShaderIo fin[32];
    pk->kind = LIB_PR, pk->vs = s->vs, pk->clamp = k->clamp, pk->poly_mode = k->poly_mode, pk->minus_w = k->minus_w;
    pk->lines = (uint8_t)key_lines(k);
    if (mesh) pk->ts = s->ts;
    else pk->missing = vs_union_missing(s, fin);
    for (int i = 0; i < 32; i++)
        if (pk->missing >> i & 1) pk->fin[i] = fin[i];
    memset(fk, 0, sizeof *fk);
    fk->kind = LIB_FS, fk->fs = s->fs, fk->dsb = k->dsb, fk->samples = k->samples, fk->a2c = k->a2c;
    if (mesh) {
        int points = 0;
        fk->fill = mesh_fill(s->vs, s->fs, &points);
        fk->points = (uint8_t)points, fk->vrs = vk_ctx()->has_vrs != 0;
    }
    pk->water_projective = fk->water_projective = mesh ? 0 : (uint8_t)water_mode(s, k);
    memset(ok, 0, sizeof *ok);
    ok->kind = LIB_FO, ok->n_color = k->n_color, ok->samples = k->samples, ok->a2c = k->a2c, ok->logic_op = k->logic_op;
    ok->blend_en = k->blend_en, ok->fbl = k->fbl, ok->depth = k->depth, ok->stencil = k->stencil, ok->wmask = k->wmask;
    memcpy(ok->color, k->color, sizeof ok->color), memcpy(ok->blend, k->blend, sizeof ok->blend);
}

// NX2NAVI_GPL_FEEDBACK (default 1; 0 = as before): a pipeline whose fragment shader samples an attachment of its own pass
// (VkPipeState.feedback: soft particles and water reading the depth buffer, decals, full-screen passes that read the
// target they write) gets a fast link like any other. The feedback loop flags belong to the fragment output state, so
// they are part of that library's key (FoKey.fbl) and of nothing else; the shader libraries are the ones the same
// shaders use without a loop. Before, such a pipeline had no fast link at all and its draws waited for, or were
// skipped during, the whole optimised build. On a first visit to Kokiri Forest (RTX 5090, every stage library in the
// driver's cache, DLSS Frame Generation on, so no frame is held back) full-screen draws of such pipelines waited
// 267 ms at the area's first frame and 410 ms in the middle of a walk; linked, 3 ms.
static int gpl_feedback(void) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2NAVI_GPL_FEEDBACK");
        if (!on) rt_log("vk_pipeline: gpl: pipelines with a feedback loop get no fast link (NX2NAVI_GPL_FEEDBACK=0)");
    }
    return on;
}

// A fast-linked pipeline for s from its stage libraries. Caller holds P.lock.
// build 0 (drawing thread): links only when the shader libraries exist (vertex input and fragment output libraries,
// which hold no shader, are built here); *final 0 = not yet, a worker builds them. build 1 (worker): builds the
// missing libraries and links with P.lock released. VK_NULL_HANDLE with *final 1 when GPL cannot express the pipeline
// (shared layout, which is also a mesh pipeline's without stage libraries; a feedback loop with
// NX2NAVI_GPL_FEEDBACK=0) or a build failed.
// A mesh pipeline is linked from three libraries: the pre-rasterization one with its task and mesh stages, the
// fragment shader one and the fragment output one. It has no vertex input state, and a complete pipeline needs that
// state only where its pre-rasterization state holds a vertex shader. The link names no state of its own and no
// dynamic state: what is dynamic is what the libraries were made with.
static VkPipeline gpl_link(const VkPipeState *s, const GKey *k, const Layout *lc, int build, int *final) {
    Layout *l = (Layout *)lc;
    *final = 1;
    if (!l->set1 || (k->fbl && !gpl_feedback())) return VK_NULL_HANDLE;
    int mesh = is_mesh(s);
    uint64_t t0 = qpc();
    ViKey vk_;
    PrKey pk;
    FsKey fk;
    FoKey ok;
    lib_keys(s, k, &vk_, &pk, &fk, &ok);
    int miss = 0;
    VkPipeline lib[4];
    lib[1] = lib_get(LIB_PR, s, k, l, &pk, sizeof pk, build, &miss);   // shader libraries first: no work when absent
    lib[2] = miss ? VK_NULL_HANDLE : lib_get(LIB_FS, s, k, l, &fk, sizeof fk, build, &miss);
    if (miss) {
        if (!build) *final = 0, P.g.defer++;
        return VK_NULL_HANDLE;
    }
    lib[0] = mesh ? VK_NULL_HANDLE : lib_get(LIB_VI, s, k, l, &vk_, sizeof vk_, 1, &miss);
    lib[3] = lib_get(LIB_FO, s, k, l, &ok, sizeof ok, 1, &miss);
    VkPipeline pipe = VK_NULL_HANDLE;
    double link_ms = 0;
    if ((mesh || lib[0]) && lib[1] && lib[2] && lib[3]) {
        VkPipelineLibraryCreateInfoKHR li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
                                             .libraryCount = mesh ? 3 : 4, .pLibraries = mesh ? lib + 1 : lib};
        VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &li,
                                           .layout = l->layout};
        if (build) plat_mutex_unlock(&P.lock);
        uint64_t t1 = qpc();
        char what[160];   // the parts that differ between links: a link that crashes the driver names its case
        snprintf(what, sizeof what,
                 "a library link (%sfragment shader %s, %u colour targets, depth format %u, stencil format %u, %u samples, "
                 "vertex outputs added %#x)", mesh ? "task and mesh stages, " : "", s->fs ? "yes" : "none", k->n_color,
                 k->depth, k->stencil, k->samples, pk.missing);
        crash_note(what, s->vs, s->fs, mesh ? s->ts : NULL);
        if (vkCreateGraphicsPipelines(P.device, VK_NULL_HANDLE, 1, &pi, NULL, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
        plat_crash_context(NULL);
        double ms = ms_since(t1);   // the link alone: library builds are counted apart
        if (build) plat_mutex_lock(&P.lock);
        P.g.link_ms += ms, link_ms = ms;
        if (ms > P.g.link_max) P.g.link_max = ms;
    }
    if (pipe) P.g.links++, P.g.fb_links += k->fbl != 0;
    else if (!P.g.link_fail++) rt_log("vk_pipeline: gpl: fast link failed; such draws wait or skip as without GPL");
    if (pipe && mesh) {
        P.g.m_links++, P.g.m_link_ms += link_ms;
        if (link_ms > P.g.m_link_max) P.g.m_link_max = link_ms;
    }
    double total = ms_since(t0);
    if (!build && total > 5.0 && P.g.slow++ < 16)
        rt_log("vk_pipeline: gpl: fast link on the drawing thread took %.1f ms", total);
    return pipe;
}

// ---- background graphics pipeline builds (vk_pipeline_async_enable)
// A miss in vk_pipeline_graphics_async enters the Pipe as building and queues a job; the replay skips draws with it
// until a worker publishes the handle. The job holds copies of the state and the key. What they point to outlives it:
// shaders (gfx_shader_get results) live for the process (gfx_shader_drop_memory is the shader probe's, which builds no
// pipelines), layouts until vk_pipeline_shutdown, which drains the queue first. Workers call vkCreateGraphicsPipelines
// without P.lock: P.cache is created without VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT, so the driver
// synchronises builds into it; only the copy of its data in cache_save is kept apart from them (cache_rw). Compute
// pipelines stay synchronous: a skipped
// dispatch can change what the game computes.
//
// Two queues: draws' misses (A.head) and the boot prebuild (A.low, see "pipeline keys" below). Workers take the low
// queue only when the other is empty and run its jobs at PLAT_PRIO_BELOW_NORMAL, so guest threads win. A draw
// that finds a prebuild job still queued moves it to the draw queue; one that finds it running raises its worker.
typedef struct Job {
    struct Job *next, *prev;           // prev: low queue only
    Pipe *p;
    const Layout *l;
    uint32_t fs_off;
    uint8_t low;                       // in the low queue, or running at below-normal priority
    uint8_t prebuild, demanded;        // from the key file; a draw asked for it before it was ready
    int worker;                        // running on A.thread[worker]; -1 queued; JOB_ON_DRAW on the thread that waits for it
    VkPipeState s;
    GKey k;
} Job;

#define MAX_WORKERS 4
#define JOB_ON_DRAW (-2)

static struct {
    int on;                            // set once by vk_pipeline_async_enable, before the replay runs
    int threads;
    PlatThread *thread[MAX_WORKERS];
    uint8_t prio_low[MAX_WORKERS];     // worker runs below normal (under P.lock)
    Job *head, *tail;                  // draws' misses
    Job *low, *low_tail;               // prebuild
    uint32_t inflight;                 // queued or running, both queues
    int low_running;                   // pool workers running a job taken from the low queue
    int gate_shut;                     // pool workers the memory gate has refused a low job since it last admitted one
    uint64_t first, taken;             // waited-for jobs moved to the head of the draw queue; built by the waiting thread
    PlatCond idle;           // with P.lock: no job queued or running
} A = {.idle = PLAT_COND_INIT};

// ---- pipeline keys (pipeline-keys.bin next to the pipeline cache)
// Every graphics pipeline built in a game run appends a KeyRec: the VkPipeState it was built from with the shader
// pointers zeroed and the shaders named by their gfx_shader input ids instead, which do not depend on the translator,
// so the record rebuilds the pipeline without guest memory. At the next boot vk_pipeline_prewarm (rt_boot, before the
// guest runs; NX2NAVI_PIPE_PREBUILD=0 skips it) loads each record's shaders, translating the ones not cached under the
// current translator from their stored inputs (gfx_shader_prewarm; a record whose shader has neither is dropped),
// enters each Pipe as building under the key a draw would compute, queues it to the low queue and builds the queue on
// every core. Records are deduplicated by a hash of their bytes; they are collected under P.lock by the worker that
// built the pipeline and appended by the present thread (vk_pipeline_autosave) or at exit (vk_pipeline_save). A file
// with another magic, version or record size is ignored and rewritten; a version 3 file (no task stage) is converted
// and rewritten. Bump KEYS_VERSION when VkPipeState or the record changes meaning without changing size.
#define KEYS_VERSION 4                 // 3: shaders named by input id (gfx_shader.c); 4: mesh pipelines, with their task stage
typedef struct { char magic[8]; uint32_t version, rec_size; } KeysHeader;
typedef struct {
    uint8_t vs[32], fs[32];
    uint32_t has_fs, pad;
    VkPipeState s;
} KeyRecV3;
typedef struct {
    uint8_t vs[32], fs[32], ts[32];    // gfx_shader_input_id of each stage (vs: the mesh stage of a mesh pipeline); fs all
                                       // zero with has_fs 0, ts with has_ts 0
    uint32_t has_fs, has_ts;
    VkPipeState s;                     // vs, fs, ts zero
} KeyRec;
static struct {
    char path[PLAT_PATH_MAX * 2];
    int header_ok;                     // the file exists with our header: append
    uint64_t *seen;                    // open addressing set of record hashes (0 empty)
    uint32_t seen_cap, seen_n;
    KeyRec *pend;                      // records not written yet
    uint32_t pend_n, pend_cap;
    uint8_t *file;                     // the file as read at boot, until the prewarm is done with it
    size_t file_n;                     // records in it
    size_t own_n;                      // of them, the first ones: this install's own (keys_open); the shared list's follow
    PlatMutex io;                        // one writer
    // prebuild progress (under P.lock)
    int loading;                       // the prewarm still queues records
    int warm;                          // vk_pipeline_prewarm owns the file (prebuild_start leaves it)
    uint32_t left;                     // prebuild jobs queued or running
    uint32_t built, failed, promoted;
    uint32_t gate_ended;               // boot builders the memory gate ended (warm_builder)
    double t0;                         // now_ms() when the prebuild started
    int prep;                          // t0 is the preparation's start (seed_pipelines), not the ready device's
    double until;                     // now_ms() after which the boot builders stop (comment at prewarm_run); 0 none
} K = {.io = PLAT_MUTEX_INIT};

static uint64_t rec_hash(const KeyRec *r) {
    uint64_t h = fnv(FNV0, r, sizeof *r);
    return h ? h : 1;
}

// 1 if h was new. Caller holds P.lock.
static int seen_add(uint64_t h) {
    if ((K.seen_n + 1) * 2 > K.seen_cap) {
        uint32_t cap = K.seen_cap ? K.seen_cap * 2 : 4096;
        uint64_t *t = calloc(cap, sizeof *t);
        if (!t) return 1;
        for (uint32_t i = 0; i < K.seen_cap; i++) {
            if (!K.seen[i]) continue;
            uint32_t j = (uint32_t)K.seen[i] & (cap - 1);
            while (t[j]) j = (j + 1) & (cap - 1);
            t[j] = K.seen[i];
        }
        free(K.seen);
        K.seen = t, K.seen_cap = cap;
    }
    uint32_t j = (uint32_t)h & (K.seen_cap - 1);
    while (K.seen[j]) {
        if (K.seen[j] == h) return 0;
        j = (j + 1) & (K.seen_cap - 1);
    }
    K.seen[j] = h;
    K.seen_n++;
    return 1;
}

static void rec_fill(KeyRec *r, const VkPipeState *s) {
    memset(r, 0, sizeof *r);
    memcpy(r->vs, gfx_shader_input_id(s->vs), 32);
    if (s->fs) memcpy(r->fs, gfx_shader_input_id(s->fs), 32), r->has_fs = 1;
    if (s->ts) memcpy(r->ts, gfx_shader_input_id(s->ts), 32), r->has_ts = 1;
    r->s = *s;
    r->s.vs = r->s.fs = r->s.ts = NULL;
    r->s.prim_restart = 0;   // dynamic state: the same pipeline either way
}

// A pipeline was built for a draw: queue its record for the file. Caller holds P.lock.
static void keys_note(const VkPipeState *s) {
    if (!A.on || !K.path[0]) return;
    static const uint8_t none[32];
    if (s->fs && !memcmp(gfx_shader_input_id(s->fs), none, 32)) return;   // the runtime's own shader (gfx_shader_own)
    if (s->ts && !memcmp(gfx_shader_input_id(s->ts), none, 32)) return;
    if (!memcmp(gfx_shader_input_id(s->vs), none, 32)) return;   // the mesh fallback's vertex stage (vk_exec_draw.c mfb_vs_get)
    KeyRec r;
    rec_fill(&r, s);
    if (!seen_add(rec_hash(&r))) return;
    if (K.pend_n == K.pend_cap) {
        uint32_t cap = K.pend_cap ? K.pend_cap * 2 : 64;
        KeyRec *t = realloc(K.pend, cap * sizeof *t);
        if (!t) return;
        K.pend = t, K.pend_cap = cap;
    }
    K.pend[K.pend_n++] = r;
}

// Appends the pending records (present thread or exit; never the render thread).
static void keys_flush(void) {
    if (!K.path[0]) return;
    plat_mutex_lock(&K.io);
    plat_mutex_lock(&P.lock);
    KeyRec *r = K.pend;
    uint32_t n = K.pend_n;
    K.pend = NULL, K.pend_n = K.pend_cap = 0;
    plat_mutex_unlock(&P.lock);
    if (n) {
        FILE *f = fopen(K.path, K.header_ok ? "ab" : "wb");
        int ok = f != NULL;
        if (ok && !K.header_ok) {
            KeysHeader h = {{'N', 'X', '2', 'N', 'P', 'K', 'E', 'Y'}, KEYS_VERSION, sizeof(KeyRec)};
            ok = fwrite(&h, sizeof h, 1, f) == 1;
        }
        if (ok) ok = fwrite(r, sizeof *r, n, f) == n;
        if (ok) ok = plat_file_sync(f);   // appended records on the disk before the next append
        if (f) ok = !fclose(f) && ok;
        if (ok) K.header_ok = 1;
        else rt_log("vk_pipeline: cannot write %s", K.path);
    }
    free(r);
    plat_mutex_unlock(&K.io);
}

static void keys_open_seed(void);

// Reads the key file into K.file and its record hashes into the set, then adds the shared list's records
// (keys_open_seed). Caller holds P.lock (ensure_init).
static void keys_open_own(void);
static void keys_open(void) {
    keys_open_own();
    K.own_n = K.file_n;
    if (K.path[0]) keys_open_seed();
}

static int seen_has(uint64_t h) {   // caller holds P.lock
    if (!K.seen_cap) return 0;
    for (uint32_t j = (uint32_t)h & (K.seen_cap - 1); K.seen[j]; j = (j + 1) & (K.seen_cap - 1))
        if (K.seen[j] == h) return 1;
    return 0;
}

// The shared pipeline list as recorded pipelines (NX2NAVI_PIPE_SEED, default 1; 0: this install's own records only).
// seed-pipeline-keys.bin next to the game holds the list's records (tools/prepare_shaders.py writes it). Before, the boot
// built only what this install drew, so a new install met every pipeline in play: in Hyrule Field one 600-frame
// stretch of a first visit had 1,769 draws wait 461 ms in all for their pipelines, the longest frame 88 ms. The list's
// records now follow the install's own in K.file, so the prewarm counts their pairs in the vertex output unions (and the
// pipelines it builds from them are the ones draws ask for) and builds them as it builds the install's: the boot for at
// most PREWARM_PIPE_MS, the pool workers the rest. The preparation builds the same list into the caches
// (seed_pipelines, NX2NAVI_PREPARE_PIPELINES=all), so that boot finds them there. They are not added to the set or the
// file: pipeline-keys.bin keeps what this install drew, and a pipeline a draw builds that the prewarm did not still goes
// into it. A list in another record layout is left out, as the preparation leaves it.
static void keys_open_seed(void) {
    if (!cfg_bool("NX2NAVI_PIPE_SEED")) return;
    char path[sizeof K.path];
    snprintf(path, sizeof path, "%s", K.path);
    char *slash = strrchr(path, '\\'), *fslash = strrchr(path, '/');
    if (fslash > slash) slash = fslash;
    snprintf(slash ? slash + 1 : path, sizeof path - (size_t)(slash ? slash + 1 - path : 0), "seed-pipeline-keys.bin");
    FILE *f = fopen(path, "rb");
    if (!f) return;
    KeysHeader h;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t n = len > (long)sizeof h ? ((size_t)len - sizeof h) / sizeof(KeyRec) : 0;
    KeyRec *seed = NULL;
    int ok = len >= (long)sizeof h && fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, "NX2NPKEY", 8) &&
             h.version == KEYS_VERSION && h.rec_size == sizeof(KeyRec) && n && (seed = malloc(n * sizeof *seed)) &&
             fread(seed, sizeof *seed, n, f) == n;
    fclose(f);
    if (!ok) {
        if (n) rt_log("vk_pipeline: %s is a list for another runtime version (its record layout differs); left out", path);
        free(seed);
        return;
    }
    KeyRec *all = realloc(K.file, (K.file_n + n) * sizeof(KeyRec));
    if (!all) {
        free(seed);
        return;
    }
    K.file = (uint8_t *)all;
    size_t added = 0;
    for (size_t i = 0; i < n; i++)   // after the install's own: those met first in play keep coming first
        if (!seen_has(rec_hash(&seed[i]))) all[K.file_n + added++] = seed[i];
    K.file_n += added;
    free(seed);
    rt_log("vk_pipeline: %s: %zu listed pipelines, %zu of them not recorded by this install; built with its own "
           "(NX2NAVI_PIPE_SEED)", path, n, added);
}

static void keys_open_own(void) {
    if (!A.on || !P.cache_path[0]) return;
    snprintf(K.path, sizeof K.path, "%s", P.cache_path);
    char *slash = strrchr(K.path, '\\'), *fslash = strrchr(K.path, '/');
    if (fslash > slash) slash = fslash;
    snprintf(slash ? slash + 1 : K.path, sizeof K.path - (size_t)(slash ? slash + 1 - K.path : 0), "pipeline-keys.bin");
    gfx_shader_record_at(K.path);   // shader-answers.bin next to it (gfx_shader.c)
    FILE *f = fopen(K.path, "rb");
    if (!f) return;
    KeysHeader h;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    int head = len >= (long)sizeof h && fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, "NX2NPKEY", 8);
    if (head && h.version == KEYS_VERSION && h.rec_size == sizeof(KeyRec)) {
        K.header_ok = 1;
        size_t n = ((size_t)len - sizeof h) / sizeof(KeyRec);
        if (n && (K.file = malloc(n * sizeof(KeyRec))) && fread(K.file, sizeof(KeyRec), n, f) == n) K.file_n = n;
        else free(K.file), K.file = NULL;
        for (size_t i = 0; i < K.file_n; i++) seen_add(rec_hash((const KeyRec *)K.file + i));
        if (len != (long)(sizeof h + n * sizeof(KeyRec)))   // a torn append: rewrite the file from what was read
            K.header_ok = 0;
    } else if (head && h.version == 3 && h.rec_size == sizeof(KeyRecV3)) {
        // the vertex pipelines of a version 3 file keep their prewarm: each record without a task stage, and the
        // file rewritten in this version below (header_ok 0)
        size_t n = ((size_t)len - sizeof h) / sizeof(KeyRecV3);
        KeyRecV3 *old = n ? malloc(n * sizeof *old) : NULL;
        if (old && fread(old, sizeof *old, n, f) == n && (K.file = calloc(n, sizeof(KeyRec)))) {
            KeyRec *r = (KeyRec *)K.file;
            for (size_t i = 0; i < n; i++) {
                memcpy(r[i].vs, old[i].vs, 32), memcpy(r[i].fs, old[i].fs, 32);
                r[i].has_fs = old[i].has_fs, r[i].s = old[i].s;
                seen_add(rec_hash(&r[i]));
            }
            K.file_n = n;
            rt_log("vk_pipeline: %s: %zu version 3 records converted; the file is rewritten", K.path, n);
        }
        free(old);
    } else {
        rt_log("vk_pipeline: %s has another format or version; it is rewritten", K.path);
    }
    fclose(f);
    if (!K.header_ok && K.file_n) {   // keep the whole records of a torn file
        KeyRec *t = malloc(K.file_n * sizeof *t);
        if (t) memcpy(t, K.file, K.file_n * sizeof *t), K.pend = t, K.pend_n = K.pend_cap = (uint32_t)K.file_n;
    }
}

static double now_ms(void) { return (double)plat_now_ns() / 1e6; }

// Caller holds P.lock.
// p, which a draw missed, has its first usable pipeline: how long that took is how long its draws were skipped or
// waited. fast 0: it had no fast link and this is the optimised build. Caller holds P.lock.
// The summary (save_with) counts what a slow one can be: a mesh pipeline (its task and mesh library is the largest
// to compile, and without stage libraries, no_libs, this is its whole build), one the key file names whose prebuild
// had not come to it, one with a shader translated in this session (nothing of it was prepared, so its libraries
// compile cold). The first READY_LOGS that took READY_SLOW_MS or longer get a line each with those facts, so a log
// says which pipelines left their draws out of the picture and for which reason. The mesh pipelines are also counted
// by themselves: how many became usable through a fast link, and after how long.
#define READY_SLOW_MS 50.0
#define READY_LOGS 48
static void ready_note(Pipe *p, int fast, const VkPipeState *s) {
    // A pipeline goes into pipeline-keys.bin once a draw can use it, so at its fast link too: recorded only when its
    // optimised build had finished (job_build), the pipelines met in the last seconds of a session were lost. A session
    // of 2026-11-13 met 46 late pipelines in adult Kokiri Forest and ended about 2 s later; 23 of them never reached the
    // file, and with it the shared pipeline list (tools/pipeline_list.py merge). keys_note takes each record once.
    // s is the state the caller built p from: a job whose optimised build failed is gone (job_build clears p->job and
    // records nothing) before a queued fast link makes p usable, and that link is recorded from s.
    if (fast && (p->job ? !p->job->prebuild : 1)) keys_note(p->job ? &p->job->s : s);
    if (!p->t_miss) return;
    double ms = (double)(plat_now_ns() - p->t_miss) / 1e6;
    p->t_miss = 0;
    P.g.ready_n[ms < 4 ? 0 : ms < 8 ? 1 : ms < 17 ? 2 : ms < 50 ? 3 : ms < 150 ? 4 : 5]++;
    P.g.ready_ms += ms;
    if (ms > P.g.ready_max) P.g.ready_max = ms;
    P.g.ready_nofast += !fast;
    const Job *j = p->job;
    if (!j) return;
    int mesh = is_mesh(&j->s);
    int fresh = gfx_shader_origin(j->s.vs) == 2 || (j->s.fs && gfx_shader_origin(j->s.fs) == 2) ||
                (j->s.ts && gfx_shader_origin(j->s.ts) == 2);
    P.g.ready_mesh += (uint64_t)mesh, P.g.ready_recorded += j->prebuild, P.g.ready_fresh += (uint64_t)fresh;
    stutter_ready(mesh, gfx_shader_input_id(j->s.vs), j->s.fs ? gfx_shader_input_id(j->s.fs) : NULL, ms, j->prebuild,
                  fresh, fast);
    if (mesh) {
        P.g.m_ready_fast += fast != 0, P.g.m_ready_ms += ms;
        if (ms > P.g.m_ready_max) P.g.m_ready_max = ms;
    }
    if (ms < READY_SLOW_MS || P.g.ready_logged >= READY_LOGS) return;
    P.g.ready_logged++;
    const uint8_t *vi = gfx_shader_input_id(j->s.vs), *fi = j->s.fs ? gfx_shader_input_id(j->s.fs) : NULL;
    char fid[16] = "none";
    if (fi) snprintf(fid, sizeof fid, "%02x%02x%02x%02x", fi[0], fi[1], fi[2], fi[3]);
    rt_log("vk_pipeline: a pipeline a draw missed was usable after %.0f ms: a %s pipeline, %s; %s; %s shader "
           "%02x%02x%02x%02x (%s), fragment shader %s (%s)%s%s",
           ms, mesh ? "mesh" : "vertex",
           fast ? "fast-linked, so the time went to its stage libraries (the lines \"a ... library took\" name the slow ones)"
                : no_libs(&j->s, p->lay) ? "which has no stage libraries: this is its whole build"
                                         : "which got no fast link: this is its whole build",
           j->prebuild ? "pipeline-keys.bin names it and its prebuild had not come to it" : "not in pipeline-keys.bin before",
           mesh ? "mesh" : "vertex", vi[0], vi[1], vi[2], vi[3], origin_name(j->s.vs), fid, origin_name(j->s.fs),
           j->k.fbl ? "; feedback loop" : "", P.g.ready_logged == READY_LOGS ? " (the last such line of this run)" : "");
}

static void prebuild_report(void) {
    rt_log("vk_pipeline: prebuild done: %u built, %u failed, %u asked for by a draw first; %.0f ms after %s", K.built,
           K.failed, K.promoted, now_ms() - K.t0, K.prep ? "the preparation started" : "the device was ready");
}

static void job_lower(Job *j);

// Runs a waiting draw's job taken from U on a worker: a library build or, for a job with p, p's fast link (gpl_link
// build 1 builds a library still missing and waits for one another worker builds). Releases P.lock while building.
static void u_run(LibJob *lj) {
    U.by_worker++, U.running++;
    if (lj->p) {
        Pipe *p = lj->p;
        if (!p->info.pipeline) {
            int final = 0;
            VkPipeline f = gpl_link(&lj->s, &lj->k, lj->l, 1, &final);   // releases P.lock while linking
            if (f && !p->info.pipeline) {
                p->fast = p->info.pipeline = f;
                ready_note(p, 1, &lj->s);
                job_lower(p->job);   // its job now only does the optimised build
            } else if (f) {
                vkDestroyPipeline(P.device, f, NULL);   // the job published one meanwhile
            }
        }
        p->link_queued = 0;
        U.links++;
        plat_cond_broadcast(&done_cv);
    } else {
        lib_build(lj->b, lj->kind, &lj->s, &lj->k, lj->l);
    }
    U.running--;
    free(lj);
}

// NX2NAVI_PIPE_FAST_WORKERS (default 1; 0 = as before): the library workers also take the fast link of a draw-queue job
// (fast_take). A draw that is skipped rather than waiting leaves its pipeline to the pool, where a job is first
// fast-linked and then goes back in the queue for its optimised build. The pool keeps one worker out of optimised
// builds, so with the others inside one (about 100 ms each with the driver's cache warm, 350 ms cold) that single
// worker linked every new pipeline of a burst, one after another at 5 to 10 ms each. On a first visit to Kokiri
// Forest (RTX 5090, libraries in the driver's cache) 180 of the 444 pipelines draws missed took over 50 ms to become
// usable, the longest 1.3 s, and their draws were missing from every frame until then; with the library workers
// linking, 23 did, 19 of them mesh pipelines, which had no fast link then (they have one with mesh_libs).
static int fast_workers(void) {
    static int on = -1;
    if (on < 0) on = cfg_bool("NX2NAVI_PIPE_FAST_WORKERS");
    return on;
}

// The first draw-queue job whose pipeline still lacks its fast link, taken out of the queue; NULL when there is none.
// Caller holds P.lock.
static Job *fast_take(void) {
    if (!P.gpl || !fast_workers()) return NULL;
    for (Job *j = A.head, *prev = NULL; j; prev = j, j = j->next) {
        Pipe *p = j->p;
        if (p->info.pipeline || p->fast_tried || p->link_queued || !p->lay || no_libs(&j->s, p->lay)) continue;
        if (prev) prev->next = j->next;
        else A.head = j->next;
        if (A.tail == j) A.tail = prev;
        j->next = j->prev = NULL;
        return j;
    }
    return NULL;
}

// A worker for stage libraries and fast links only: a waiting draw's jobs (U) first, so a draw that waits finds one
// free even while the pool builds pipelines and the drawing thread never builds a library itself; then the fast link
// of a draw-queue job (fast_take), whose optimised build goes on to the pool.
static void lib_worker(void *arg) {
    (void)arg;
    plat_mutex_lock(&P.lock);
    for (;;) {
        Job *j = NULL;
        while (!U.head && !(j = fast_take())) plat_cond_wait(&lib_cv, &P.lock);
        if (!j) {
            LibJob *lj = U.head;
            if (!(U.head = lj->next)) U.tail = NULL;
            if (lj->b) lj->b->job = NULL;
            u_run(lj);
            continue;
        }
        Pipe *p = j->p;
        int final;
        p->fast_tried = 1;   // no other thread links it meanwhile; a draw that waits for it waits for this link
        VkPipeline f = gpl_link(&j->s, &j->k, p->lay, 1, &final);   // releases P.lock while building
        if (f && !p->info.pipeline) {
            p->fast = p->info.pipeline = f, P.g.wlinks++, P.g.flinks++;
            ready_note(p, 1, &j->s);
            plat_cond_broadcast(&done_cv);
        } else if (f) {
            vkDestroyPipeline(P.device, f, NULL);
        }
        if (p->info.pipeline) {   // the optimised build: ahead of the prebuild, behind other draws' misses
            j->low = 1, P.g.requeued++;
            j->next = A.low;
            if (A.low) A.low->prev = j;
            else A.low_tail = j;
            A.low = j;
        } else {   // no fast link for this one: its whole build, next in the draw queue
            if (!(j->next = A.head)) A.tail = j;
            A.head = j;
        }
        plat_cond_signal(&work_cv);
    }
}

// ---- the memory gate of the background builds (vk_build_gate.h)
// Every pipeline built ahead of its first use passes here before it starts: the recorded pipelines the boot builds
// (warm_builder), the pool's low queue during play (pipe_worker: the rest of the recorded pipelines and the optimised
// builds behind a fast link), the compute pipelines and stage libraries of the boot (lb_worker), the library warm
// (lw_worker) and the large compute shaders (bg_main). A build a draw or a dispatch waits for never does: it is needed
// now.
// NX2NAVI_BUILD_FREE_MB (default 4096; 0 no check): the system commit headroom that stays free of them.
// NX2NAVI_BUILD_RESERVE_MB (default 64): what one build under way is taken to cost the driver's compiler while it
// runs; a large compute shader reserves GATE_BIG_MB. A build starts only when the headroom holds the part kept free,
// the reservations of the builds under way and its own.
// Measured on the machine this was written on (RTX 5090, 73,154 MB commit limit, warm driver cache): the boot's 3,898
// recorded pipelines on 16 threads raised the process commit by 3,054 MB in 2.2 s, and the 4,066 compute pipelines and
// libraries after them, together with the game's own start, by another 9,824 MB. A device was lost on that machine
// at 56,673 of 65,531 MB system commit right after such a prebuild.
// The commit is read at most every GATE_SAMPLE_MS: sixteen builders finishing a cached pipeline every half millisecond
// would otherwise ask the system thousands of times a second. What the builds took since the last reading is covered
// by their reservations.
#define GATE_BIG_MB 4096.0
#define GATE_SAMPLE_MS 100.0
#define GATE_WAIT_MS 30000.0   // how long a boot or warm builder waits for room before it leaves the rest to their first use
static struct {
    PlatMutex lock;
    BuildGate g;
    double est, used, limit, at;
    int init;
    int off;   // a preparation run (seed_pipelines): nothing is refused, there is no game to keep room for
    uint64_t refused;
} GATE = {.lock = PLAT_MUTEX_INIT};

// 1 when a build estimated at est MB may start, and then est is reserved until gate_leave. est 0: an ordinary build
// (NX2NAVI_BUILD_RESERVE_MB), whose estimate *took returns for gate_leave.
static int gate_enter(double est, double *took) {
    plat_mutex_lock(&GATE.lock);
    if (!GATE.init) {
        GATE.init = 1;
        GATE.g.free_mb = strtod(cfg_value("NX2NAVI_BUILD_FREE_MB"), NULL);
        GATE.est = strtod(cfg_value("NX2NAVI_BUILD_RESERVE_MB"), NULL);
        if (GATE.est < 0) GATE.est = 0;
        GATE.at = -GATE_SAMPLE_MS;
    }
    if (GATE.off) GATE.g.free_mb = 0;
    double now = now_ms();
    if (now - GATE.at >= GATE_SAMPLE_MS) {
        double proc;
        plat_commit_info(&proc, &GATE.used, &GATE.limit);
        GATE.at = now;
    }
    if (est <= 0) est = GATE.est;
    int ok = build_gate_enter(&GATE.g, GATE.used, GATE.limit, est);
    if (!ok) GATE.refused++;
    plat_mutex_unlock(&GATE.lock);
    if (took) *took = est;
    return ok;
}

static void gate_leave(double est) {
    plat_mutex_lock(&GATE.lock);
    build_gate_leave(&GATE.g, est);
    plat_mutex_unlock(&GATE.lock);
}

// The gate's numbers for a log line that says why a builder stopped or waits.
static const char *gate_text(char *buf, size_t n) {
    plat_mutex_lock(&GATE.lock);
    snprintf(buf, n, "system commit %.0f of %.0f MB, %.0f MB reserved for builds under way, %.0f MB kept free "
                     "(NX2NAVI_BUILD_FREE_MB), %.0f MB a build (NX2NAVI_BUILD_RESERVE_MB)", GATE.used, GATE.limit,
             GATE.g.reserved_mb, GATE.g.free_mb, GATE.est);
    plat_mutex_unlock(&GATE.lock);
    return buf;
}

// A boot or warm builder's wait for room: 1 with est reserved (*took), 0 when *stop was set or GATE_WAIT_MS passed
// without room, which the caller takes as the end of its work. who names the builder for the log's one line a wait.
static int gate_wait(const char *who, volatile int32_t *stop, double *took) {
    double w0 = now_ms();
    int waited = 0;
    while (!gate_enter(0, took)) {
        char g[256];
        if (*stop) return 0;
        if (!waited++) rt_log("vk_pipeline: %s: waiting for commit headroom (%s)", who, gate_text(g, sizeof g));
        if (now_ms() - w0 > GATE_WAIT_MS) return 0;
        for (int i = 0; i < 20 && !*stop; i++) plat_sleep_ms(100);
    }
    return 1;
}

// --probe-buildgate (host.h): the gate with this machine's commit and the settings in effect, without a device and
// without a build. It prints what the settings and the system give, asks for sixteen ordinary builds and one large
// one, and checks the answers against the same numbers: the builds admitted are those that fit with the earlier
// ones' reservations, a refused one reserves nothing, and a builder's wait ends at once when its stop flag is set.
// tests/build_gate_check.py runs it with build_free_mb at 0 (everything admitted) and at a value no machine has free
// (everything refused).
int vk_pipeline_probe_buildgate(void) {
    double took = 0, big = 0;
    int fails = 0, in = 0, want = 0;
    int first = gate_enter(0, &took);
    double free_mb = GATE.g.free_mb, used = GATE.used, limit = GATE.limit, est = took;
    if (first) gate_leave(took);
    printf("probe-buildgate: system commit %.0f of %.0f MB, %.0f MB kept free (NX2NAVI_BUILD_FREE_MB), %.0f MB a build "
           "(NX2NAVI_BUILD_RESERVE_MB)\n", used, limit, free_mb, est);
    double room = limit - used - free_mb;
    for (int i = 0; i < 16; i++) {
        want += free_mb <= 0 || limit < 0 || room - i * est >= est;   // the commit sample stays for GATE_SAMPLE_MS
        in += gate_enter(0, &took);
    }
    double reserved = GATE.g.reserved_mb;
    int big_in = gate_enter(GATE_BIG_MB, &big);
    int big_want = free_mb <= 0 || limit < 0 || room - reserved >= GATE_BIG_MB;
    printf("probe-buildgate: %d of 16 ordinary builds admitted (%d fit), %.0f MB reserved; a large compute build beside "
           "them %s\n", in, want, reserved, big_in ? "admitted" : "refused");
    if (in != want) fails++, printf("probe-buildgate: FAIL: %d admitted, %d fit\n", in, want);
    if (reserved != in * est) fails++, printf("probe-buildgate: FAIL: %.0f MB reserved for %d builds\n", reserved, in);
    if (big_in != big_want) fails++, printf("probe-buildgate: FAIL: the large build was %s\n", big_in ? "admitted" : "refused");
    if (big_in) gate_leave(big);
    for (int i = 0; i < in; i++) gate_leave(est);
    if (GATE.g.reserved_mb != 0) fails++, printf("probe-buildgate: FAIL: %.0f MB still reserved\n", GATE.g.reserved_mb);
    volatile int32_t stop = 1;
    double t0 = now_ms();
    int got = gate_wait("probe", &stop, &took);
    if (got) gate_leave(took);
    if (got != first || now_ms() - t0 > 1000)
        fails++, printf("probe-buildgate: FAIL: a stopped builder's wait gave %d after %.0f ms\n", got, now_ms() - t0);
    printf("probe-buildgate: an ordinary build is %s now; %s\n", first ? "admitted" : "refused", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}

// ---- fast-linked pipelines the optimised build replaces
// With the graphics pipeline library a draw first gets a pipeline linked from stage libraries (p->fast), and the
// optimised build of the same state replaces it in p->info.pipeline when it is done. The fast link was kept until the
// process ended, so every pipeline a run drew held its fast link's driver memory as well: one play session of 13,800
// frames made 1,108 fast links and replaced 907 of them, while the video memory outside the allocator, where the
// driver keeps compiled pipelines, grew by 491 MiB (RTX 5090), and a 68,400-frame session grew it from 1,672 to
// 2,485 MiB. A replaced fast link is destroyed now, once the GPU is past every submission that may have recorded it.
// The draws read p->info.pipeline when they record (vk_exec_draw.c, the pipeline memo reads it again each time) and
// keep no handle past the submission they record, so after the store below a draw can only have the old handle in
// the submission being recorded, or in an earlier one: vk_sync_issued() + 1 at most. The destroy waits for one more
// submission than that, for a recording thread that runs a submission behind (vk_rec.c). It runs on the completion
// thread (vk_completion_add), which holds no lock of ours while it calls it; a lost device or the end of the process
// leaves it to the driver, as everything else.
static uint64_t fast_retired;   // P.lock: fast links destroyed so far

static void fast_destroy_cb(void *arg) {
    vkDestroyPipeline(P.device, (VkPipeline)(uintptr_t)arg, NULL);
}

// P.lock held, p->info.pipeline just replaced by the optimised pipeline: the fast link goes once the GPU is done with it.
static void fast_retire(Pipe *p) {
    VkPipeline old = p->fast;
    if (!old || old == p->info.pipeline) return;
    p->fast = VK_NULL_HANDLE;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);   // the new handle is visible before the submission count is read
    uint64_t rf = vk_exec_recording_fence(), at = vk_sync_issued() + 2;
    if (rf + 1 > at) at = rf + 1;
    vk_completion_add(at, fast_destroy_cb, (void *)(uintptr_t)old);
    fast_retired++;
}

// ---- pipelines no draw has used, given back while video memory is short
// The boot builds the recorded pipelines ahead of play (the prewarm and the shipped pipeline list), and the driver keeps
// each one's compiled code in video memory until it is destroyed. In a 4K session with DLSS and frame generation the
// device-local memory outside the allocator was 4.2 GB with 6,300 graphics pipelines alive, 3,589 of them not drawn
// once in that run. On a card short of video memory those are the cheapest thing to give back: no draw has recorded
// them. So while the device-local memory in use is over NX2NAVI_TEX_VRAM_PRESSURE percent of the budget
// (vk_mem_vram_live, which NX2NAVI_VRAM_BUDGET_MB caps for a test), the autosave thread takes up to RELEASE_BATCH
// graphics pipelines that no draw has looked up in this run out of the table every RELEASE_EVERY_MS, and destroys them
// with their entries once the GPU is past the submission being recorded and one more (as fast_retire), at most
// RELEASE_DRAIN a tick (release_drain). Not on the completion thread: a value counts as done only after its callbacks
// ran (vk_sync.c completion_main), so a batch of destroys there held every fence wait of the drawing threads. A draw that
// needs one later misses it as it would a pipeline never built (pipe_miss): its stage libraries link it, or the
// look-ahead queued them (vk_pipeline_hint), and its optimised build comes from the pipeline cache, which still holds it.
// Only entries no lookup reached go (used_min 0; pipe_minute is set before the first lookup, ensure_init), so no draw's
// memo (vk_exec_draw.c, the PM context) nor any caller holds their VkPipeInfo. Not taken: mesh pipelines (without
// stage libraries their build is long), entries building or with a link queued, failed ones, and compute pipelines.
// The next pass logs the device-local use then and now, so a log says what a batch gave back.
// The use counts the free space inside the allocator's blocks as free, as the texture evictions do (vk_texture.c
// sweep_pressure): the driver's figure (vk_mem_vram) counts it as used, so with free blocks it could stay over the line
// while pipelines went, and every pass would give back another batch until no unused pipeline was left.
// NX2NAVI_PIPE_RELEASE=0 keeps every pipeline for the run, as before 0.10.0, and so does NX2NAVI_TEX_VRAM_PRESSURE=0,
// which keeps the textures from giving way for video memory.
#define RELEASE_EVERY_MS 5000
#define RELEASE_BATCH 256
#define RELEASE_DRAIN 64   // destroyed per autosave tick (500 ms) at most: faster than RELEASE_BATCH per RELEASE_EVERY_MS
static uint64_t rel_total, rel_batches;
static uint64_t rel_used0;   // device-local use (MiB) when the last batch went; 0 none to report
// Entries out of the table, linked through next in the order they went (so by rel_fence), until release_drain
// destroys them. Only the autosave thread touches the list (pressure_release, release_drain): no lock.
static Pipe *rel_head, *rel_tail;

// The autosave thread, each tick: destroys the released entries whose fence the GPU is past. None after a device loss
// (vk_device.c vk_device_lost_check): the device is not called any more.
static void release_drain(void) {
    if (!rel_head || vk_device_is_lost()) return;
    uint64_t done = vk_sync_fence_done();
    for (int i = 0; i < RELEASE_DRAIN && rel_head && rel_head->rel_fence <= done; i++) {
        Pipe *p = rel_head;
        if (!(rel_head = p->next)) rel_tail = NULL;
        if (p->info.pipeline) vkDestroyPipeline(P.device, p->info.pipeline, NULL);
        free(p->bindings);
        free(p);
    }
}

// Caller holds P.lock.
static int releasable(const Pipe *p) {
    if (p->used_min || !p->info.pipeline || p->building || p->job || p->link_queued || p->fast || p->t_miss || !p->lay)
        return 0;
    if (p->info.bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS || p->key_size != sizeof(GKey)) return 0;
    const GKey *k = (const GKey *)p->key;
    return k->vs && gfx_shader_stage(k->vs) != GFX_STAGE_MS;
}

static void pressure_release(uint64_t now) {
    static int on = -1, pct;
    static uint64_t last;
    if (on < 0) {
        on = cfg_bool("NX2NAVI_PIPE_RELEASE");
        pct = cfg_int("NX2NAVI_TEX_VRAM_PRESSURE");
        if (pct < 0 || pct > 100) pct = 80;
        if (!on) rt_log("vk_pipeline: every pipeline stays for the run, also while video memory is short (NX2NAVI_PIPE_RELEASE=0)");
        else if (!pct) {   // 0 turns the textures' giving way off (vk_texture.c sweep_pressure): the pipelines' too
            on = 0;
            rt_log("vk_pipeline: every pipeline stays for the run, also while video memory is short "
                   "(NX2NAVI_TEX_VRAM_PRESSURE=0)");
        }
    }
    if (!on || now - last < RELEASE_EVERY_MS) return;
    last = now;
    uint64_t used = 0, budget = 0;
    if (!vk_mem_vram_live(&used, &budget) || !budget) return;
    if (rel_used0) {
        rt_log("vk_pipeline: device-local memory in use (free space inside the allocator's blocks not counted) %llu MiB "
               "when the last batch of unused pipelines went, %llu MiB now", (unsigned long long)rel_used0,
               (unsigned long long)(used >> 20));
        rel_used0 = 0;
    }
    if (used * 100 < budget * (uint64_t)pct) return;
    Pipe *gone[RELEASE_BATCH];
    int n = 0;
    plat_mutex_lock(&P.lock);
    for (int b = 0; b < PIPE_BUCKETS && n < RELEASE_BATCH; b++)
        for (Pipe **pp = &pipes[b]; *pp && n < RELEASE_BATCH;) {
            Pipe *p = *pp;
            if (releasable(p)) *pp = p->next, gone[n++] = p;
            else pp = &p->next;
        }
    uint64_t at = vk_sync_issued() + 2;
    plat_mutex_unlock(&P.lock);
    if (!n) return;
    for (int i = 0; i < n; i++) {
        gone[i]->rel_fence = at, gone[i]->next = NULL;
        if (rel_tail) rel_tail->next = gone[i];
        else rel_head = gone[i];
        rel_tail = gone[i];
    }
    rel_total += (uint64_t)n, rel_batches++;
    rel_used0 = used >> 20;
    if (rel_batches <= 16 || !(rel_batches & (rel_batches - 1)))
        rt_log("vk_pipeline: video memory in use %llu of %llu MiB (over %d%%, free space inside the allocator's blocks not "
               "counted): %d graphics pipelines no draw used in this run given back, %llu in %llu batches so far; a draw "
               "that needs one builds it again from the pipeline cache (NX2NAVI_PIPE_RELEASE)",
               (unsigned long long)(used >> 20), (unsigned long long)(budget >> 20), pct, n,
               (unsigned long long)rel_total, (unsigned long long)rel_batches);
}

// The whole build of job j, which the caller took off its queue, and the job's end: its pipeline is entered, the
// threads waiting for it are woken and j is freed. lowrun: 1 on a pool worker that took j from the low queue. Caller
// holds P.lock, which is released for the build.
static void job_build(Job *j, int lowrun) {
    plat_mutex_unlock(&P.lock);
    VkPipeline pipe = create_graphics(&j->s, &j->k, j->l, j->fs_off);
    plat_mutex_lock(&P.lock);
    if (pipe || !j->p->fast) __atomic_store_n(&j->p->info.pipeline, pipe, __ATOMIC_SEQ_CST);   // a failed optimised build keeps the fast-linked one
    ready_note(j->p, 0, &j->s);
    if (pipe && j->p->fast) P.g.swaps++, j->p->optimised = 1, fast_retire(j->p);
    j->p->building = 0;
    j->p->job = NULL;
    if (pipe) {
        P.stats.graphics++, P.stats.async_built++;
        if (!j->prebuild) keys_note(&j->s);
    } else {
        P.stats.failures++;
        rt_log("vk_pipeline: graphics pipeline not built: pipeline creation failed");
    }
    if (j->prebuild) {
        if (pipe) K.built++;
        else K.failed++;
        K.promoted += j->demanded;
        if (!--K.left && !K.loading) prebuild_report();
    }
    A.low_running -= lowrun;
    if (!--A.inflight) plat_cond_broadcast(&A.idle);
    plat_cond_broadcast(&done_cv);
    if (pipe && (!j->prebuild || j->demanded)) perf_count(PERF_CNT_PIPE_ASYNC);   // a draw waited for it
    free(j);
}

static void pipe_worker(void *arg) {
    int me = (int)(intptr_t)arg;
    plat_mutex_lock(&P.lock);
    for (;;) {
        // With 3 or more workers one never takes low-queue work, so a waiting draw's library starts at once
        // (not with NX2NAVI_PIPE_WAIT=draw).
        while (!U.head && !A.head && !(A.low && (A.threads < 3 || !U.on || A.low_running < A.threads - 1))) {
            U.idle++;
            plat_cond_wait(&work_cv, &P.lock);
            U.idle--;
        }
        if (U.head) {   // a library a draw waits for
            LibJob *lj = U.head;
            if (!(U.head = lj->next)) U.tail = NULL;
            if (lj->b) lj->b->job = NULL;
            if (A.prio_low[me]) A.prio_low[me] = 0, plat_thread_priority(PLAT_PRIO_NORMAL);
            u_run(lj);   // releases P.lock while building
            continue;
        }
        Job *j = A.head;
        int lowrun = !j;
        double held = 0;   // the low job's reservation at the memory gate
        if (!j && !gate_enter(0, &held)) {
            // No room for a background build now (gate_enter). The job stays queued: a draw that needs its pipeline
            // moves it to the draw queue (promote), which wakes this wait; otherwise the gate is asked again in 2 s.
            if (!A.gate_shut++) {
                char g[256];
                rt_log("vk_pipeline: background pipeline builds wait for commit headroom (%s); a pipeline a draw needs "
                       "is still built at once", gate_text(g, sizeof g));
            }
            plat_cond_wait_ns(&work_cv, &P.lock, 2000000000ull);
            continue;
        }
        if (!j && A.gate_shut) {
            A.gate_shut = 0;
            rt_log("vk_pipeline: background pipeline builds go on: the commit headroom is back");
        }
        if (j) {
            if (!(A.head = j->next)) A.tail = NULL;
        } else {
            j = A.low;
            if ((A.low = j->next)) A.low->prev = NULL;
            else A.low_tail = NULL;
            A.low_running++;
        }
        j->next = j->prev = NULL;
        j->worker = me;
        if (j->low != A.prio_low[me]) {   // under P.lock, so a draw raising this thread (promote) is not undone
            A.prio_low[me] = j->low;
            plat_thread_priority(j->low ? PLAT_PRIO_BELOW_NORMAL : PLAT_PRIO_NORMAL);
        }
        // Step 1 for a job a draw waits for: its stage libraries and the fast link. Step 2, the optimised build, goes to
        // the head of the low queue, so a burst of new pipelines gets every fast link before any optimised build.
        if (P.gpl && (!j->prebuild || j->demanded) && !j->p->info.pipeline && !j->p->fast_tried && j->p->lay &&
            !no_libs(&j->s, j->p->lay)) {
            int final;
            VkPipeline f = gpl_link(&j->s, &j->k, j->p->lay, 1, &final);   // releases P.lock while building
            j->p->fast_tried = 1;
            if (f && !j->p->info.pipeline) {
                j->p->fast = j->p->info.pipeline = f, P.g.wlinks++;
                ready_note(j->p, 1, &j->s);
                plat_cond_broadcast(&done_cv);
                j->low = 1, j->worker = -1, P.g.requeued++;
                j->prev = NULL, j->next = A.low;
                if (A.low) A.low->prev = j;
                else A.low_tail = j;
                A.low = j;
                A.low_running -= lowrun;
                if (lowrun) gate_leave(held);
                continue;
            } else if (f) {
                vkDestroyPipeline(P.device, f, NULL);
            }
        }
        job_build(j, lowrun);
        if (lowrun) gate_leave(held);
    }
    return;
}

static int hint_on(void);

// Starts the pool on first use: a quarter of the logical processors, 2 to 4. Caller holds P.lock.
static int workers_start(void) {
    if (A.threads) return 1;
    uint32_t cpus = plat_cpu_usable();
    int n = cpus / 4 < 2 ? 2 : cpus / 4 > MAX_WORKERS ? MAX_WORKERS : (int)(cpus / 4);
    for (int i = 0; i < n; i++) {
        PlatThread *t = plat_thread_new(pipe_worker, (void *)(intptr_t)A.threads, "pipe worker");
        if (!t) break;
        A.thread[A.threads++] = t;   // kept: promote() raises a worker running a prebuild job
    }
    // Workers for stage libraries and fast links alone: two (the vertex and fragment libraries of one waiting draw in
    // parallel), and as many as the pool has with the look-ahead, which queues the libraries of a whole submission's
    // new pipelines at once (vk_pipeline_hint), or with the fast links of skipped draws (lib_worker). They sleep
    // unless a library or a fast link is queued.
    int nlib = hint_on() || fast_workers() ? n : 2;
    for (int i = 0; A.threads && U.on && P.gpl && i < nlib; i++)
        if (plat_thread_new(lib_worker, NULL, "pipe lib worker")) U.lib_workers++;
    if (A.threads)
        rt_log("vk_pipeline: %d background graphics pipeline workers, %d for waiting draws' libraries", A.threads,
               U.lib_workers);
    else rt_log("vk_pipeline: cannot start pipeline workers; building synchronously");
    return A.threads != 0;
}

static void workers_drain(void) {
    plat_mutex_lock(&P.lock);
    K.loading = 0;
    for (Job *j = A.low, *n; j; j = n) {   // queued prebuild jobs are dropped, not built
        n = j->next;
        j->p->building = 0, j->p->job = NULL;
        A.inflight--, K.left--;
        free(j);
    }
    A.low = A.low_tail = NULL;
    for (LibJob *j = U.head, *n; j; j = n) {   // queued library jobs too: their entries read as failed builds
        n = j->next;
        if (j->b) j->b->job = NULL, j->b->building = 0;   // a draw's fast link (link_queue) names its pipeline, not a library
        if (j->p) j->p->link_queued = 0;
        free(j);
    }
    U.head = U.tail = NULL;
    while (A.inflight) plat_cond_wait(&A.idle, &P.lock);
    while (U.running) plat_cond_wait(&done_cv, &P.lock);
    plat_mutex_unlock(&P.lock);
}

void vk_pipeline_async_enable(void) {
    U.on = strcmp(cfg_value("NX2NAVI_PIPE_WAIT"), "draw") != 0;
    const char *e = cfg_get("NX2NAVI_PIPE_SYNC");
    if (e && *e && strcmp(e, "0")) return;
    A.on = 1;
}

int vk_pipeline_async_on(void) { return A.on; }

// A draw needs a pipeline a prebuild job owns: the job moves to the draw queue, or its worker to normal priority.
// Caller holds P.lock.
static void promote(Pipe *p) {
    Job *j = p->job;
    if (!j || !j->low) return;
    if (j->prebuild && !j->demanded && !p->info.pipeline) p->t_miss = plat_now_ns();
    j->low = 0, j->demanded = 1;
    if (j->worker == -1) {
        if (j->prev) j->prev->next = j->next;
        else A.low = j->next;
        if (j->next) j->next->prev = j->prev;
        else A.low_tail = j->prev;
        j->next = j->prev = NULL;
        if (A.tail) A.tail->next = j;
        else A.head = j;
        A.tail = j;
        plat_cond_signal(&work_cv);
        if (U.lib_workers) plat_cond_signal(&lib_cv);   // its fast link (lib_worker)
    } else if (j->worker >= 0 && A.prio_low[j->worker]) {
        A.prio_low[j->worker] = 0;
        plat_thread_set_priority(A.thread[j->worker], PLAT_PRIO_NORMAL);
    }
}

// A draw waits up to wait_ms for the pipeline of job j, which has no fast link (a pipeline without stage libraries,
// as a mesh pipeline is with NX2NAVI_GPL_MESH=0, or one whose link failed) and so needs its whole build. While the
// job is still queued the wait buys nothing: the draw queue is first in, first out, and the jobs before it are the
// fast links and builds of other draws' misses, so on a burst of new pipelines the one a draw stood waiting for
// started last and the draw was skipped at the end of its wait all the same.
// - The job goes to the head of the draw queue and a worker is woken, so the next free worker takes it.
// - A wait longer than TAKE_WAIT_MS is not one of a frame's budgets (vk_exec_draw.c frame_wait is at most 500 ms): the
//   caller wants this pipeline whatever it takes (a one-shot draw with NX2NAVI_ONESHOT_BUDGET=0). With no worker idle
//   the job is then built here at once (job_build), as a library no worker took is (lib_wait). A wait inside a
//   budget is never turned into a build on this thread: a whole mesh pipeline takes 0.2 to 1.1 s to compile, and the
//   budget exists so that the replay thread is not held that long.
// Caller holds P.lock; it is released while the job is built here.
#define TAKE_WAIT_MS 500u
static void job_wanted(Job *j, uint32_t wait_ms) {
    if (!j || j->worker != -1 || j->low) return;   // running already, or gone
    Job *q = NULL;
    if (A.head != j) {
        for (q = A.head; q && q->next != j; q = q->next) {}
        if (!q) return;
    }
    if (wait_ms > TAKE_WAIT_MS && !U.idle) {
        if (q) {
            if (!(q->next = j->next)) A.tail = q;
        } else if (!(A.head = j->next)) {
            A.tail = NULL;
        }
        j->next = j->prev = NULL;
        j->worker = JOB_ON_DRAW;
        A.taken++;
        job_build(j, 0);
        return;
    }
    if (q) {   // to the head
        if (!(q->next = j->next)) A.tail = q;
        j->next = A.head, A.head = j;
        A.first++;
    }
    plat_cond_signal(&work_cv);
}

// The stage-independent layout of a mesh pipeline that has stage libraries (mesh_libs): the mesh stage's bindings and
// the task stage's after them in set 0, which the pre-rasterization library of the two is made with, and the fragment
// stage's in set 1, which is the set of that fragment shader in a vertex pipeline too, so the fragment shader library
// can be the same one. NULL with *why set. On success *B holds both lists, set 0 first (layout_pair). Caller holds
// P.lock.
static Layout *mesh_pair_layout(const VkPipeState *s, Bindings *B, const char **why) {
    Bindings F = {0};
    Layout *l = NULL;
    if (!add_stage(B, s->vs, 0) || (s->ts && !add_stage(B, s->ts, binding_span(s->vs) - 1)) ||
        (s->fs && !add_stage(&F, s->fs, 0))) *why = "too many bindings";
    else if (!(l = layout_pair(B, &F))) *why = "descriptor layout creation failed";
    B->pool |= F.pool;
    return l;
}

// Enters a missing pipeline: built here (mode 0), or queued for a draw (1) or for the prebuild (2). Failures are
// entered too. *queued: 1 when a job owns it. Caller holds P.lock.
static Pipe *pipe_miss(const VkPipeState *s, const GKey *k, uint64_t h, int mode, int *queued) {
    *queued = 0;
    const char *why = NULL;
    Bindings B = {0};
    Layout *l = NULL;
    uint32_t span = binding_span(s->vs);
    int mesh = is_mesh(s);
    if (mesh) {
        // Mesh draws (VK_EXT_mesh_shader). With stage libraries (mesh_libs) the layout is a stage-independent pair
        // like a vertex pipeline's (mesh_pair_layout). Without them, and for an emulated mesh pipeline, one set holds
        // the mesh stage's bindings, the fragment stage's after them and the task stage's after those (create_graphics
        // renumbers both modules), and the pipeline has no fast link. They go to the workers like the others: built
        // here, each new one stopped the frame for its whole compile (0.26-0.51 s in Kokiri Forest), and they are
        // recorded for the boot prewarm (keys_note).
        uint32_t fspan = s->fs ? binding_span(s->fs) : 1;
        int emu = !vk_ctx()->has_mesh_shader && mesh_emu_on();
        if (!vk_ctx()->has_mesh_shader && !emu) why = "the GPU has no mesh shaders (mesh_emulation = 1 emulates them)";
        else if ((s->fs && gfx_shader_stage(s->fs) != GFX_STAGE_FS) || (s->ts && gfx_shader_stage(s->ts) != GFX_STAGE_TS)) why = "stage mismatch";
        else if (!emu && mesh_libs()) {
            l = mesh_pair_layout(s, &B, &why);
            span = 1;   // the fragment module keeps its bindings, in set 1
        } else if (!add_stage(&B, s->vs, 0) || (s->fs && !add_stage(&B, s->fs, span - 1)) ||
                 (s->ts && !add_stage(&B, s->ts, span - 1 + fspan - 1))) why = "too many bindings";
        else if (!(l = emu ? layout_get_emu(&B) : layout_get(&B))) why = "descriptor layout creation failed";
    } else if (gfx_shader_stage(s->vs) != GFX_STAGE_VS || (s->fs && gfx_shader_stage(s->fs) != GFX_STAGE_FS)) why = "stage mismatch";
    else if (k->topo_class == 3) why = "patches need tessellation stages, which are not translated";
    else if (P.indep) {
        Bindings F = {0};
        if (!add_stage(&B, s->vs, 0) || (s->fs && !add_stage(&F, s->fs, 0))) why = "too many bindings";
        else if (!(l = layout_pair(&B, &F))) why = "descriptor layout creation failed";
        B.pool |= F.pool;
        span = 1;
    } else if (!add_stage(&B, s->vs, 0) || (s->fs && !add_stage(&B, s->fs, span - 1))) why = "too many bindings";
    else if (!(l = layout_get(&B))) why = "descriptor layout creation failed";
    char lack[128];
    if (!why && !layout_covers(s, l, span - 1, lack, sizeof lack)) why = lack, l = NULL;
    VkPipeline pipe = VK_NULL_HANDLE;
    Job *j = NULL;
    if (!why && mode && workers_start() && (j = calloc(1, sizeof *j))) {
        *j = (Job){.l = l, .fs_off = span - 1, .low = mode == 2, .prebuild = mode == 2, .worker = -1, .s = *s, .k = *k};
    } else if (!why && !(pipe = create_graphics(s, k, l, span - 1))) {
        why = "pipeline creation failed";
    }
    if (why) {
        P.stats.failures++;
        rt_log("vk_pipeline: graphics pipeline not built: %s", why);
    } else if (!j) {
        P.stats.graphics++;
        if (mode != 2) keys_note(s);
    }
    Pipe *p = pipe_add(k, sizeof *k, h, pipe, l, l ? &B : NULL, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_shader_inputs(s->vs, NULL));
    if (!j || !p) {
        free(j);
        return p;
    }
    j->p = p;
    p->building = 1, p->job = j;
    if (mode != 2) p->t_miss = plat_now_ns();
    if (mode == 2) {
        j->prev = A.low_tail;
        if (A.low_tail) A.low_tail->next = j;
        else A.low = j;
        A.low_tail = j;
        K.left++;
    } else {
        if (A.tail) A.tail->next = j;
        else A.head = j;
        A.tail = j;
        if (U.lib_workers && fast_workers()) plat_cond_signal(&lib_cv);   // its fast link (lib_worker)
    }
    A.inflight++;
    plat_cond_signal(&work_cv);
    *queued = 1;
    return p;
}

// A job still in the draw queue goes to the head of the low queue: its pipeline has (or is about to have) a fast-linked
// one, so the optimised build waits behind other draws' misses. Caller holds P.lock.
static void job_lower(Job *j) {
    if (!j || j->worker != -1 || j->low) return;
    if (A.head == j) {
        if (!(A.head = j->next)) A.tail = NULL;
    } else {
        Job *q = A.head;
        while (q && q->next != j) q = q->next;
        if (!q) return;
        if (!(q->next = j->next)) A.tail = q;
    }
    j->low = 1, P.g.requeued++;
    j->prev = NULL, j->next = A.low;
    if (A.low) A.low->prev = j;
    else A.low_tail = j;
    A.low = j;
}

// A draw waits for pipeline s: its missing pre-rasterization and fragment shader libraries go to the head of the
// worker queues (U), so they build in parallel on idle workers, and this thread sleeps until both are published or
// end (now_ms) passes. A job no worker took within 1 ms (all busy) is built here. Caller holds P.lock.
static void u_push(LibJob *j) {
    if (U.tail) U.tail->next = j;
    else U.head = j;
    U.tail = j;
}

// Queues s's missing pre-rasterization and fragment shader libraries (b: both entries). 0 when one cannot be entered
// (no layout half, out of memory). Caller holds P.lock.
static int lib_queue(const VkPipeState *s, const GKey *k, Layout *l, Lib *b[2]) {
    ViKey vk_;
    PrKey pk;
    FsKey fk;
    FoKey ok;
    lib_keys(s, k, &vk_, &pk, &fk, &ok);
    U.waits++;
    const void *key[2] = {&pk, &fk};
    uint32_t size[2] = {sizeof pk, sizeof fk};
    int queued = 0;
    for (int i = 0; i < 2; i++) {
        uint64_t h = key_hash(key[i], size[i]);
        if ((b[i] = lib_find(h, key[i], size[i]))) continue;
        if (!half_layout(l, i)) return 0;   // gpl_link reports it
        LibJob *j = calloc(1, sizeof *j);
        if (!j || !(b[i] = lib_new(h, key[i], size[i]))) {
            free(j);
            return 0;
        }
        *j = (LibJob){.b = b[i], .kind = i ? LIB_FS : LIB_PR, .l = l, .s = *s, .k = *k};
        b[i]->job = j;
        u_push(j);
        U.queued++, queued++;
    }
    if (queued) plat_cond_broadcast(&work_cv), plat_cond_broadcast(&lib_cv);
    return 1;
}

// A waiting draw's whole fast link of p on the library workers: its library jobs, then the link job behind them.
// The caller then waits for p->info.pipeline. 0 when nothing was queued (the caller links as before). Under P.lock.
static int link_queue(Pipe *p, const VkPipeState *s, const GKey *k, Layout *l) {
    Lib *b[2];
    if (!U.lib_workers || !lib_queue(s, k, l, b)) return 0;
    LibJob *j = calloc(1, sizeof *j);
    if (!j) return 0;
    *j = (LibJob){.l = l, .s = *s, .k = *k, .p = p};
    p->link_queued = 1;
    u_push(j);
    plat_cond_broadcast(&work_cv), plat_cond_broadcast(&lib_cv);
    return 1;
}

static void lib_wait(const VkPipeState *s, const GKey *k, Layout *l, double end) {
    Lib *b[2];
    if (!lib_queue(s, k, l, b)) return;
    double steal_at = now_ms() + 1.0;
    for (;;) {
        if (!b[0]->building && !b[1]->building) return;
        double now = now_ms();
        if (now >= end) {
            U.timeouts++;
            return;
        }
        Lib *q = b[0]->job ? b[0] : b[1]->job ? b[1] : NULL;
        if (q && !U.lib_workers && (now >= steal_at || !U.idle)) {
            U.stolen++;
            lib_steal(q);   // releases P.lock while building
            continue;
        }
        double until = q && steal_at < end ? steal_at : end;
        plat_cond_wait_ms(&done_cv, &P.lock, (uint32_t)(until - now) + 1);
    }
}

// ---- stage libraries ahead of their draws (NX2NAVI_PIPE_AHEAD, default 1; 0 off; vk_exec.c "looking ahead")
// The replay names the state of a draw it has not reached yet. When no pipeline is known for it, its pre-rasterization
// and fragment shader libraries are queued to the library workers now, so they build while the draws before it are
// recorded, in parallel with each other and with those of the other draws named: the draw's own lookup then finds both
// and links on its thread in microseconds instead of waiting for them or being skipped while they build. Nothing else
// is entered, no pipeline, no optimised build and no key record: a state the scan guessed wrong costs two libraries
// at most. The feedback loop bits, the topology and the targets do not matter here, as no shader library depends on
// them. A mesh draw is named too where mesh pipelines have stage libraries (mesh_libs): its task and mesh library is
// the one that takes longest to make. Returns the libraries queued.
static int hint_on(void) {
    static int on = -1;
    if (on < 0) on = cfg_bool("NX2NAVI_PIPE_AHEAD");
    return on;
}

int vk_pipeline_hint(const VkPipeState *s) {
    if (!A.on || !U.on || !hint_on() || !s->vs || !ensure_init() || !P.gpl || !P.indep) return 0;
    int mesh = is_mesh(s);
    if (mesh ? !mesh_libs() || (s->ts && gfx_shader_stage(s->ts) != GFX_STAGE_TS) : gfx_shader_stage(s->vs) != GFX_STAGE_VS)
        return 0;
    if (s->fs && gfx_shader_stage(s->fs) != GFX_STAGE_FS) return 0;
    GKey k;
    gkey_fill(s, &k);
    if (k.topo_class == 3) return 0;
    uint64_t h = key_hash(&k, sizeof k);
    int queued = 0;
    plat_mutex_lock(&P.lock);
    if (!pipe_find(&k, sizeof k, h)) {
        Bindings B = {0}, F = {0};
        const char *why = NULL;
        Layout *l = mesh ? mesh_pair_layout(s, &B, &why)
                  : add_stage(&B, s->vs, 0) && (!s->fs || add_stage(&F, s->fs, 0)) ? layout_pair(&B, &F) : NULL;
        Lib *b[2];
        uint64_t q0 = U.queued, w0 = U.waits;
        if (l && l->set1 && workers_start() && lib_queue(s, &k, l, b)) queued = (int)(U.queued - q0);
        U.waits = w0, U.queued = q0;   // those count waiting draws' jobs
        P.g.hints++, P.g.hint_libs += (uint64_t)queued;
    }
    plat_mutex_unlock(&P.lock);
    return queued;
}

static VkPipeline graphics(const VkPipeState *s, const VkPipeInfo **info, int async, int *pending, uint32_t wait_ms) {
    if (info) *info = NULL;
    if (pending) *pending = 0;
    if (!s->vs || !ensure_init()) return VK_NULL_HANDLE;
    GKey k;
    gkey_fill(s, &k);
    uint64_t h = key_hash(&k, sizeof k);
    plat_mutex_lock(&P.lock);
    Pipe *p = pipe_find(&k, sizeof k, h);
    int queued;
    if (p) {
        if (!p->building) P.stats.hits++;
    } else {
        p = pipe_miss(s, &k, h, async, &queued);
    }
    if (p) pipe_used(p);
    // A draw that would wait (wait_ms) gets its missing shader libraries built at once, the vertex and fragment ones in
    // parallel on idle workers (lib_wait; about 0.12 ms per library from the driver's disk cache, 5-180 ms cold), and
    // links them here: its own job may queue behind optimised builds of earlier pipelines. The job then only does the
    // optimised build, from the low queue. Other draws leave the libraries to the job and are skipped meanwhile.
    // NX2NAVI_PIPE_WAIT=draw builds them on this thread one after the other instead, as before; NX2NAVI_GPL_DRAW_BUILD=0
    // leaves them to the job.
    static int draw_build = -1;
    if (draw_build < 0) {
        const char *e = cfg_get("NX2NAVI_GPL_DRAW_BUILD");
        draw_build = !(e && !strcmp(e, "0"));
        rt_log("vk_pipeline: a draw that waits for its pipeline %s", !draw_build ? "leaves its libraries to its job"
               : U.on ? "has its libraries built in parallel on the workers, ahead of other jobs"
                      : "builds its libraries itself (NX2NAVI_PIPE_WAIT=draw)");
    }
    double end = wait_ms ? now_ms() + wait_ms : 0;
    if (p && p->building && P.gpl && !p->info.pipeline && !p->fast_tried && !p->link_queued && p->lay &&
        !no_libs(s, p->lay) && wait_ms && draw_build && U.on && link_queue(p, s, &k, (Layout *)p->lay)) {
        // the libraries and the link on the library workers; this thread waits below without building
        p->fast_tried = 1;
        job_lower(p->job);
    } else if (p && p->building && P.gpl && !p->info.pipeline && !p->fast_tried && !p->link_queued && p->lay &&
               !no_libs(s, p->lay)) {
        int final = 0, build = wait_ms != 0 && draw_build;
        uint64_t p0 = perf_begin(), t0 = qpc();
        if (build && U.on) {
            job_lower(p->job);   // its worker would only wait for the same libraries; it links them if this draw cannot
            lib_wait(s, &k, (Layout *)p->lay, end);   // releases P.lock while waiting
            build = 0;                                // links here when both libraries exist (gpl_link build 0)
        } else if (build) {
            p->fast_tried = 1;   // the job, if a worker takes it meanwhile, goes straight to the optimised build
        }
        VkPipeline f = p->info.pipeline ? VK_NULL_HANDLE : gpl_link(s, &k, p->lay, build, &final);   // build 1 releases P.lock
        perf_end(PERF_PIPE, p0);
        if (wait_ms && draw_build) {
            double ms = ms_since(t0);
            P.g.dlinks++, P.g.dlink_ms += ms;
            if (ms > P.g.dlink_max) P.g.dlink_max = ms;
            if (ms > 5.0 && P.g.slow++ < 16)
                rt_log("vk_pipeline: gpl: a waiting draw %s its libraries in %.1f ms", U.on ? "got" : "built", ms);
        }
        if (final) p->fast_tried = 1;
        if (f && !p->info.pipeline) {
            p->fast = p->info.pipeline = f;
            ready_note(p, 1, s);
            job_lower(p->job);
            plat_cond_broadcast(&done_cv);
        } else if (f) {
            vkDestroyPipeline(P.device, f, NULL);   // a worker published one while the lock was free
        }
    }
    if (p && p->building && !p->info.pipeline) {
        promote(p);
        uint64_t p0 = wait_ms ? perf_begin() : 0;   // the wait counts as the render thread's pipeline time
        double now;
        if (wait_ms) job_wanted(p->job, wait_ms);   // releases P.lock when it builds the job here
        while (p->building && !p->info.pipeline && wait_ms && (now = now_ms()) < end)
            plat_cond_wait_ms(&done_cv, &P.lock, (uint32_t)(end - now) + 1);
        if (wait_ms) perf_end(PERF_PIPE, p0);
        if (wait_ms && !p->building) perf_count(PERF_CNT_PIPE_WAIT);
    }
    int wait = p && p->building && !p->info.pipeline;
    if (wait) P.stats.pending_skips++;
    VkPipeline pipe = p ? p->info.pipeline : VK_NULL_HANDLE;
    plat_mutex_unlock(&P.lock);
    if (wait) {
        if (pending) *pending = 1;
        return VK_NULL_HANDLE;
    }
    if (!p) return VK_NULL_HANDLE;
    if (info) *info = &p->info;
    return pipe;
}

VkPipeline vk_pipeline_graphics(const VkPipeState *s, const VkPipeInfo **info) {
    return graphics(s, info, 0, NULL, 0);
}

VkPipeline vk_pipeline_graphics_async(const VkPipeState *s, const VkPipeInfo **info, int *pending, uint32_t wait_ms) {
    return graphics(s, info, A.on, pending, wait_ms);
}

double vk_pipeline_building_ms(const VkPipeState *s) {
    if (!s->vs || !P.ready) return 0;
    GKey k;
    gkey_fill(s, &k);
    uint64_t h = key_hash(&k, sizeof k);
    plat_mutex_lock(&P.lock);
    const Pipe *p = pipe_find(&k, sizeof k, h);
    double ms = p && p->building && !p->info.pipeline && p->t_miss ? (double)(plat_now_ns() - p->t_miss) / 1e6 : 0;
    plat_mutex_unlock(&P.lock);
    return ms;
}

// ---- boot prewarm from the key file (comment at "pipeline keys")

static int id_cmp(const void *x, const void *y) { return memcmp(x, y, 32); }

// Builds queued prebuild jobs until the low queue is empty or K.until passes, beside the pipe_worker pool (same
// bookkeeping). Jobs left queued at K.until stay for the pool. So do those the memory gate refuses (gate_enter): a
// builder it refuses ends, which leaves fewer builders compiling side by side while the others' reservations stand,
// and none when the machine has no room; the pool builds what is left during play, when the gate lets it.
// arg non-NULL: one job at most. The builders start with one pipeline built alone on the starting thread:
// a driver's first graphics pipeline sets up its compiler, and NVIDIA's 621.14 was reported to fault in a sizable
// share of starts when every builder began its first pipeline at the same moment.
static void warm_builder(void *arg) {
    int one = arg != NULL;
    plat_mutex_lock(&P.lock);
    while (A.low && !(K.until && now_ms() >= K.until)) {
        double held;
        if (!gate_enter(0, &held)) {
            if (!K.gate_ended++) {
                char g[256];
                rt_log("vk_pipeline: prewarm: the boot builds fewer recorded pipelines side by side, or none: the commit "
                       "headroom is short (%s). The rest are built in the background when there is room, or at their "
                       "first draw", gate_text(g, sizeof g));
            }
            break;
        }
        Job *j = A.low;
        if ((A.low = j->next)) A.low->prev = NULL;
        else A.low_tail = NULL;
        j->next = j->prev = NULL;
        j->low = 0;                    // not on a pool thread: promote() has nothing to raise
        plat_mutex_unlock(&P.lock);
        VkPipeline pipe = create_graphics(&j->s, &j->k, j->l, j->fs_off);
        plat_mutex_lock(&P.lock);
        if (pipe || !j->p->fast) __atomic_store_n(&j->p->info.pipeline, pipe, __ATOMIC_SEQ_CST);   // a failed optimised build keeps the fast-linked one
        ready_note(j->p, 0, &j->s);
        if (pipe && j->p->fast) P.g.swaps++, j->p->optimised = 1, fast_retire(j->p);
        j->p->building = 0;
        j->p->job = NULL;
        if (pipe) {
            P.stats.graphics++, P.stats.async_built++, K.built++;
        } else {
            P.stats.failures++, K.failed++;
            rt_log("vk_pipeline: graphics pipeline not built: pipeline creation failed");
        }
        K.promoted += j->demanded;
        K.left--;
        if (!--A.inflight) plat_cond_broadcast(&A.idle);
        plat_cond_broadcast(&done_cv);
        free(j);
        gate_leave(held);
        if (one) break;
    }
    plat_mutex_unlock(&P.lock);
    return;
}

static void lb_start(const uint32_t pr_hist[16], const uint32_t fs_hist[4]);
static int pm_skipped(void);
static void pm_set(void);

// vk_pipeline_prewarm_progress's numbers: the phase, and the pipelines finished (built or failed) of those queued, copied
// while prewarm_run waits for its builders; in phases 3 and 4 (bf_wait) the compute pipelines and the prepared variants.
static int32_t pw_phase;
static int64_t pw_pipe_done, pw_pipe_total;

void vk_pipeline_prewarm_progress(VkPrewarmProgress *p) {
    p->phase = __atomic_load_n(&pw_phase, __ATOMIC_ACQUIRE);
    gfx_shader_prewarm_progress(&p->sh_done, &p->sh_total, &p->sh_translated);
    p->pipe_done = __atomic_load_n(&pw_pipe_done, __ATOMIC_RELAXED);
    p->pipe_total = __atomic_load_n(&pw_pipe_total, __ATOMIC_RELAXED);
}

static void prewarm_run(void);

// The whole warm on the boot screen after a driver change (comment at bf_decide).
static struct {
    int on;                  // this start waits for all of it
    int lw;                  // lb_start left the library warm's start to bf_wait
    volatile int32_t game;   // the wait is over: the warm ended, or the boot let the game start beside it
    double t_end;            // now_ms() at which the whole warm's time on the boot screen is up; 0 no limit
    char owed[PLAT_PATH_MAX * 2 + 48];   // boot-warm-owed.txt
} BF;

// 1 while the start waits for the builders of the whole warm: they run at normal priority on every core then, and as
// in play (idle priority, the background's thread counts) from the moment the game starts.
static int bf_waiting(void) {
    return BF.on && !__atomic_load_n(&BF.game, __ATOMIC_ACQUIRE);
}

void vk_pipeline_prewarm_detach(void) {
    __atomic_store_n(&BF.game, 1, __ATOMIC_RELEASE);
}

static void bf_decide(void);
static void bf_wait(void);

// The boot's share of the pipeline prebuild with the graphics pipeline library. The boot builds the recorded
// pipelines in record order, so those met first in play (the title screen, the first places) come first, for at most
// this long after the shaders are ready; the rest stay queued for the pool workers, which build them below normal
// priority while the game runs. A draw that meets one of them before it is built gets a pipeline linked from stage
// libraries at once (gpl_link), as for any new pipeline in play. After a driver update, when every pipeline is built
// from nothing, the boot waits for all of them and for the rest of the warm (bf_decide). Without the library a draw
// would have to wait for a whole build, so the boot waits for all; so it does with NX2NAVI_PREWARM_FULL=1.
#define PREWARM_PIPE_MS 20000.0

void vk_pipeline_prewarm(void) {
    prewarm_run();
    __atomic_store_n(&pw_phase, 5, __ATOMIC_RELEASE);
}

// ---- mesh pipelines' stage libraries ahead of their draws
// The shader preparation and the boot build the stage libraries of single shaders (lb_library): one vertex or one
// fragment shader is all such a library holds. A mesh pipeline's pre-rasterization library holds its mesh stage and
// its task stage together, and its fragment shader library depends on what that mesh stage leaves unwritten (FsKey),
// so these are taken from the pipeline records, which name all three shaders: every recorded mesh pipeline the boot
// queues (prewarm_run) and every listed one the preparation builds (seed_pipelines) notes its two shader libraries
// here, each key once. They are built once and destroyed, as the single shaders' libraries are, which leaves them in
// the pipeline caches and the driver's own: a mesh pipeline of the same stages that a draw meets later (other
// targets, another blend state or sample count, or a recorded one the boot has not come to) then finds its libraries
// there and is linked in milliseconds, where it was compiled whole before.
//  - preparation (mesh_pre_run, from seed_pipelines): on every core, before the pipeline cache is saved, so that
//    file holds them too; they are as many as the list has mesh stage pairs, not the tens of thousands of single
//    shaders' libraries that are kept out of it.
//  - boot (mesh_pre_start, from lb_start): one thread beside the game at idle priority, behind the memory gate
//    (gate_wait); NX2NAVI_LIB_PREBUILD=0 leaves it out with the rest of that prebuild.
typedef struct {
    VkPipeState s;
    GKey k;
    int kind;                          // LIB_PR or LIB_FS
    uint64_t h;                        // key_hash of the key
    uint32_t size;
    union { PrKey pr; FsKey fs; } key;
} MeshPre;
static struct {
    MeshPre *e;
    uint32_t n, cap, n_pr;             // n_pr: the pre-rasterization libraries among them
    volatile int64_t next;
    volatile int32_t stop;
    int boot;                          // the builder runs beside the game: idle priority, the memory gate
    PlatThread *th;                    // the boot's builder
    PlatMutex stat;
    uint64_t built, have, fail;        // have: a draw had made the library by the time its turn came
    double ms, max, t0;
} MP = {.stat = PLAT_MUTEX_INIT};

// Notes the two shader libraries of mesh pipeline s (key k), a pipeline with stage libraries (mesh_libs), for
// mesh_pre_worker. Caller holds P.lock; no builder runs yet.
static void mesh_pre_note(const VkPipeState *s, const GKey *k) {
    ViKey v;
    PrKey pk;
    FsKey fk;
    FoKey ok;
    lib_keys(s, k, &v, &pk, &fk, &ok);
    const void *key[2] = {&pk, &fk};
    const uint32_t size[2] = {sizeof pk, sizeof fk};
    for (int i = 0; i < 2; i++) {
        int kind = i ? LIB_FS : LIB_PR;
        uint64_t h = key_hash(key[i], size[i]);
        uint32_t j = 0;
        while (j < MP.n && !(MP.e[j].kind == kind && MP.e[j].h == h && !memcmp(&MP.e[j].key, key[i], size[i]))) j++;
        if (j < MP.n) continue;
        if (MP.n == MP.cap) {
            uint32_t cap = MP.cap ? MP.cap * 2 : 64;
            MeshPre *t = realloc(MP.e, cap * sizeof *t);
            if (!t) return;
            MP.e = t, MP.cap = cap;
        }
        MeshPre *e = &MP.e[MP.n++];
        memset(e, 0, sizeof *e);
        e->s = *s, e->k = *k, e->kind = kind, e->h = h, e->size = size[i];
        memcpy(&e->key, key[i], size[i]);
        MP.n_pr += kind == LIB_PR;
    }
}

static void mesh_pre_worker(void *arg) {
    (void)arg;
    for (;;) {
        int idle = MP.boot && !bf_waiting();   // as the boot's other builders: normal priority while the start waits
        plat_thread_priority(idle ? PLAT_PRIO_IDLE : PLAT_PRIO_NORMAL);
        int64_t i = plat_inc64(&MP.next) - 1;
        if (i >= (int64_t)MP.n || MP.stop) break;
        MeshPre *e = &MP.e[i];
        double held = 0;
        if (MP.boot && !gate_wait("mesh library prebuild", &MP.stop, &held)) {
            char g[256];
            if (!MP.stop)
                rt_log("vk_pipeline: mesh library prebuild: ends here, at %lld of %u libraries: no commit headroom for "
                       "another build (%s). The others are built when a draw first needs them", (long long)i, MP.n,
                       gate_text(g, sizeof g));
            break;
        }
        const char *why = NULL;
        Bindings B = {0};
        plat_thread_priority(PLAT_PRIO_NORMAL);   // P.lock is never held at idle priority: the replay takes it
        plat_mutex_lock(&P.lock);
        int have = lib_find(e->h, &e->key, e->size) != NULL;
        Layout *l = have ? NULL : mesh_pair_layout(&e->s, &B, &why);
        VkPipelineLayout lay = l ? half_layout(l, e->kind == LIB_FS) : VK_NULL_HANDLE;
        plat_mutex_unlock(&P.lock);
        if (idle) plat_thread_priority(PLAT_PRIO_IDLE);
        VkPipeline pipe = VK_NULL_HANDLE;
        double t0 = now_ms(), ms = 0;
        if (lay) pipe = lib_create(e->kind, &e->s, &e->k, lay, &e->key);
        if (pipe) {
            ms = now_ms() - t0;
            vkDestroyPipeline(P.device, pipe, NULL);
            __atomic_fetch_add(&cache_merges, 1, __ATOMIC_RELAXED);   // autosave_once: no pipeline count tells of it
        }
        if (MP.boot) gate_leave(held);
        plat_mutex_lock(&MP.stat);
        if (have) MP.have++;
        else if (!pipe) MP.fail++;
        else MP.built++, MP.ms += ms, MP.max = ms > MP.max ? ms : MP.max;
        plat_mutex_unlock(&MP.stat);
    }
    if (MP.boot)
        rt_log("vk_pipeline: mesh library prebuild %s: of the %u shader libraries of the recorded mesh pipelines (%u of "
               "task and mesh stages, %u fragment shader) %llu built into the caches (avg %.1f ms, longest %.0f ms; a "
               "few milliseconds each when the caches held them), %llu made by a draw first, %llu failed; %.0f ms",
               MP.stop ? "stopped" : "done", MP.n, MP.n_pr, MP.n - MP.n_pr, (unsigned long long)MP.built,
               MP.built ? MP.ms / (double)MP.built : 0.0, MP.max, (unsigned long long)MP.have,
               (unsigned long long)MP.fail, now_ms() - MP.t0);
}

// Boot: the noted libraries on one thread beside the game.
static void mesh_pre_start(void) {
    if (!MP.n || MP.th || MP.next) return;
    MP.boot = 1, MP.t0 = now_ms();
    MP.th = plat_thread_new(mesh_pre_worker, NULL, "mesh lib prebuild");
}

// Preparation: the noted libraries on up to `threads` threads; returns when all are built.
static void mesh_pre_run(int threads) {
    if (!MP.n) return;
    PlatThread *t[16];
    int n = 0;
    MP.boot = 0, MP.t0 = now_ms();
    for (int i = 0; i < threads && i < 16 && (uint32_t)i < MP.n; i++)
        if ((t[n] = plat_thread_new(mesh_pre_worker, NULL, "mesh lib prebuild"))) n++;
    if (!n) mesh_pre_worker(NULL);
    for (int i = 0; i < n; i++) plat_thread_join(t[i]);
}

// NX2NAVI_PIPE_SEED_BOOT (auto, the default; 1; 0): whether the boot builds the pipelines of the shared list that this
// install has never drawn (keys_open_seed), 1, or leaves them to their first draw, 0. Built at boot they are there
// when a draw first needs them, which is what the list is for, and every one is a pipeline the driver holds for the
// whole run: with the list of 0.9.1 the boot's pipeline stage went from about 1.6 GB of commit (0.8.0, 3,256
// pipelines on an RTX 5080) to 3.9-4.4 GB (8,531 on an RTX 2080 Ti, 6,977 on an RTX 5070 Ti) and 0.3-0.6 GiB of
// video memory, 1.5-1.8 GB of commit on AMD's driver, of which the list's share is half to nearly all. On a machine
// that has that to spare it buys the first visit of every area without a wait. On one that starts short of the
// commit the game takes in play (mem_need.h, the start's own warning) it is memory taken from the game for
// pipelines that may never be drawn, and running out of commit ends the game (a failed allocation or a lost device).
// auto leaves them out there and only there. A pipeline left out is made at its first draw as any new one is, from
// pipeline-cache.bin when the preparation built it (0.5-0.8 ms on a worker), and is the install's own from then on:
// the next start builds it at boot. Mesh pipelines are built at boot either way. Without stage libraries a draw
// waits for their whole build. With them (mesh_libs) the first draw of one would be linked from its libraries, but
// those are the largest modules the game has, and the few frames they take to come out of the caches are frames in
// which a kind of foliage is missing from a place the player has just entered: built here, it is there from the
// first frame. The list's pairs still count in the vertex output unions, so the modules stay the ones the
// preparation built.
static int seed_boot(void) {
    static int on = -1;
    if (on >= 0) return on;
    const char *e = cfg_get("NX2NAVI_PIPE_SEED_BOOT");
    if (e && (!strcmp(e, "0") || !strcmp(e, "1"))) return on = e[0] == '1';
    double proc, used, limit, need = strtod(cfg_value("NX2NAVI_MEM_NEED_MB"), NULL);
    plat_commit_info(&proc, &used, &limit);
    return on = !mem_need_short(proc, used, limit, need);
}

static void prewarm_run(void) {
    const char *gpu = rt_config()->gpu;
    uint32_t pr_hist[16] = {0}, fs_hist[4] = {0};   // library state of the recorded pipelines (lb_start)
    if (!A.on || (gpu && !strcmp(gpu, "null"))) return;
    if (!cfg_bool("NX2NAVI_PIPE_PREBUILD")) {   // prebuild_start logs the off case
        if (ensure_init()) lb_start(pr_hist, fs_hist);
        return;
    }
    double t0 = now_ms();
    K.warm = 1;
    if (!ensure_init() || !K.file_n) {
        rt_log("vk_pipeline: prewarm: no pipeline keys (%s)", K.path[0] ? K.path : "<none>");
        if (P.ready) lb_start(pr_hist, fs_hist);
        return;
    }
    const KeyRec *r = (const KeyRec *)K.file;
    size_t n = K.file_n, m = 0;
    uint8_t (*ids)[32] = malloc(3 * n * 32);
    const GfxShader **sh = calloc(3 * n, sizeof *sh);
    if (!ids || !sh) {
        free(ids), free(sh);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        memcpy(ids[m++], r[i].vs, 32);
        if (r[i].has_fs) memcpy(ids[m++], r[i].fs, 32);
        if (r[i].has_ts) memcpy(ids[m++], r[i].ts, 32);
    }
    qsort(ids, m, 32, id_cmp);
    size_t u = 0;
    for (size_t i = 0; i < m; i++)
        if (!u || memcmp(ids[u - 1], ids[i], 32)) memcpy(ids[u++], ids[i], 32);
    int cpus = (int)plat_cpu_usable();
    const char *pe = cfg_get("NX2NAVI_PREWARM_XLATE");   // no fixed default: half the CPUs, at most 16, when unset
    int procs = pe && *pe ? atoi(pe) : cpus / 2 > 16 ? 16 : cpus / 2;
    __atomic_store_n(&pw_phase, 1, __ATOMIC_RELEASE);
    gfx_shader_prewarm((const uint8_t(*)[32])ids, u, sh, cpus, procs < 1 ? 1 : procs);
    double t1 = now_ms();
    size_t carried = 0, fresh = 0;   // the recorded pipelines' shaders by where their translation came from
    for (size_t i = 0; i < u; i++)
        if (sh[i]) carried += gfx_shader_origin(sh[i]) == 1, fresh += gfx_shader_origin(sh[i]) == 2;
    vk_mem_stage_log("the shader prewarm");

    if (P.indep) {   // vertex output unions first: workers may take a job before the queue is complete
        uint64_t pair = 0, uni = 0, pairs = 0;
        for (size_t i = 0; i < n; i++) {
            const uint8_t (*v)[32] = bsearch(r[i].vs, ids, u, 32, id_cmp);
            const uint8_t (*f)[32] = r[i].has_fs ? bsearch(r[i].fs, ids, u, 32, id_cmp) : NULL;
            if (!v || !f || !sh[v - ids] || !sh[f - ids] || gfx_shader_stage(sh[v - ids]) != GFX_STAGE_VS) continue;
            GfxShaderIo io[32];
            vs_outs(sh[v - ids], sh[f - ids], io);
            pair += (uint64_t)__builtin_popcount(gfx_shader_inputs(sh[f - ids], NULL)), pairs++;
        }
        for (uint32_t j = 0; j < VS_OUTS_CAP; j++) uni += VO.vs[j] ? (uint64_t)__builtin_popcount(VO.mask[j]) : 0;
        rt_log("vk_pipeline: vertex output unions: %u vertex shaders, %.2f locations each; %llu pairs read %.2f each",
               VO.n, VO.n ? (double)uni / VO.n : 0.0, (unsigned long long)pairs, pairs ? (double)pair / pairs : 0.0);
    }
    if (pm_skipped()) {   // the boot's pipeline builds killed an earlier start on this GPU and driver
        plat_mutex_lock(&P.lock);
        free(K.file);
        K.file = NULL;
        plat_mutex_unlock(&P.lock);
        free(ids), free(sh);
        lb_start(pr_hist, fs_hist);
        return;
    }
    pm_set();
    bf_decide();
    uint32_t queued = 0, dropped = 0, known = 0, listed = 0;
    int at_boot = seed_boot();
    plat_mutex_lock(&P.lock);
    K.t0 = t1;
    K.loading = 1;
    for (size_t i = 0; i < n; i++) {
        const uint8_t (*v)[32] = bsearch(r[i].vs, ids, u, 32, id_cmp);
        const uint8_t (*f)[32] = r[i].has_fs ? bsearch(r[i].fs, ids, u, 32, id_cmp) : NULL;
        const uint8_t (*t)[32] = r[i].has_ts ? bsearch(r[i].ts, ids, u, 32, id_cmp) : NULL;
        const GfxShader *vs = v ? sh[v - ids] : NULL, *fs = f ? sh[f - ids] : NULL, *ts = t ? sh[t - ids] : NULL;
        if (!vs || (r[i].has_fs && !fs) || (r[i].has_ts && !ts)) {
            dropped++;
            continue;
        }
        VkPipeState s = r[i].s;
        s.vs = vs, s.fs = fs, s.ts = ts;
        // a mesh pipeline of the shared list (keys_open_seed) on a GPU without mesh shaders, which draws them through
        // the mesh fallback: none to build, as the preparation skips it (seed_pipelines). With the mesh emulation
        // (NX2NAVI_MESH_EMULATION) it is a vertex pipeline and its compute stages (pipe_miss: layout_get_emu), built
        // here like the others: left to the first draw, each took 113-830 ms on an RX 580 and the draws waiting for it
        // were skipped (22 of the 48 pipelines missed entering Kokiri Forest, 0.9.1), frames held back or shown without
        // them. With the mesh fallback on a device that has mesh shaders (vk_mesh_path.h: AMD's driver for RDNA 1 and
        // 2) none is built either: no mesh draw uses one, and that driver is not to compile them.
        if (is_mesh(&s) && (vk_ctx()->mesh_fallback || (!vk_ctx()->has_mesh_shader && !mesh_emu_on()))) {
            dropped++;
            continue;
        }
        GKey k;
        gkey_fill(&s, &k);
        if (!is_mesh(&s)) {   // the state of the single shaders' libraries (lb_state): vertex pipelines decide it
            pr_hist[(k.clamp & 1) | (k.poly_mode & 3) << 1 | (k.minus_w & 1) << 3]++;
            fs_hist[k.samples >= 8 ? 3 : k.samples >= 4 ? 2 : k.samples >= 2 ? 1 : 0]++;
        } else if (mesh_libs()) {   // a mesh pipeline's libraries are built with its own state (mesh_pre_start)
            mesh_pre_note(&s, &k);
        }
        if (!at_boot && i >= K.own_n && !is_mesh(&s)) {   // of the shared list alone: made at its first draw (seed_boot)
            listed++;
            continue;
        }
        uint64_t h = key_hash(&k, sizeof k);
        int q = 0;
        if (pipe_find(&k, sizeof k, h)) known++;
        else pipe_miss(&s, &k, h, 2, &q), queued += (uint32_t)q;
    }
    K.loading = 0;
    plat_mutex_unlock(&P.lock);
    if (listed) {
        const char *e = cfg_get("NX2NAVI_PIPE_SEED_BOOT");
        rt_log("vk_pipeline: prewarm: %u records of the shared pipeline list that this install has not drawn are left to "
               "their first draw, when pipeline-cache.bin gives them (NX2NAVI_PIPE_SEED_BOOT=%s%s)", listed,
               e && !strcmp(e, "0") ? "0" : "auto",
               e && !strcmp(e, "0") ? "" : ": this machine started with less commit free than the game takes in play");
    }
    free(ids), free(sh);

    __atomic_store_n(&pw_pipe_done, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&pw_pipe_total, (int64_t)queued, __ATOMIC_RELAXED);
    __atomic_store_n(&pw_phase, 2, __ATOMIC_RELEASE);
    const char *full = cfg_get("NX2NAVI_PREWARM_FULL");
    // A cache file of another device or driver (a driver update: cache_data_read) leaves every recorded pipeline to be
    // built from nothing, and the ones the boot left after PREWARM_PIPE_MS were built beside the game: on an 8-core
    // integrated GPU that start had 2,141 of 8,122 left after 20 s and took 296 s to finish them. The whole warm
    // (bf_decide) waits for all of them, with its count on the boot screen, as a start without the library does.
    int all = (full && !strcmp(full, "1")) || (BF.on && queued);
    if (P.gpl && all && BF.on && !(full && !strcmp(full, "1")))
        rt_log("vk_pipeline: prewarm: the start waits for the %u recorded pipelines as long as the whole warm after a "
               "driver change may take, and not %d s", queued, (int)(PREWARM_PIPE_MS / 1000));
    K.until = P.gpl && !all ? now_ms() + PREWARM_PIPE_MS : 0;   // before the builders start
    // the whole warm's own limit: what it leaves goes to the pool's workers, as after PREWARM_PIPE_MS
    if (P.gpl && BF.on && BF.t_end && !(full && !strcmp(full, "1"))) K.until = BF.t_end;
    PlatThread *t[64];
    int nt = 0;
    if (queued) warm_builder((void *)1);   // the first alone (warm_builder)
    for (int i = 0; i < cpus && i < 64 && queued; i++)
        if ((t[nt] = plat_thread_new(warm_builder, NULL, "pipe prewarm"))) nt++;
    if (!nt) warm_builder(NULL);
    double logged = now_ms();
    for (int i = 0; i < nt; i++)
        while (!plat_thread_join_ns(t[i], 100000000u)) {   // every 100 ms the numbers, every 2 s a log line
            uint32_t fin = __atomic_load_n(&K.built, __ATOMIC_RELAXED) + __atomic_load_n(&K.failed, __ATOMIC_RELAXED);
            __atomic_store_n(&pw_pipe_done, (int64_t)fin, __ATOMIC_RELAXED);
            if (now_ms() - logged < 2000) continue;
            logged = now_ms();
            rt_log("vk_pipeline: prewarm: %u of %u pipelines, %.0f ms", fin, queued, now_ms() - t1);
        }
    __atomic_store_n(&pw_pipe_done, (int64_t)queued, __ATOMIC_RELAXED);
    plat_mutex_lock(&P.lock);
    uint32_t later = 0;
    for (Job *j = A.low; j; j = j->next) later += j->prebuild;
    if (!later)   // all built: wait for those a pool worker took
        while (K.left) plat_cond_wait(&done_cv, &P.lock);
    else          // the boot's time is up: the pool builds the rest while the game runs
        plat_cond_broadcast(&work_cv);
    rt_log("vk_pipeline: prewarm: %zu records in %s: %u pipelines built, %u left to build in the background, %u "
           "failed, %u already built, %u dropped (shader neither cached nor stored); shaders %.0f ms, pipelines %.0f "
           "ms on %d threads, total %.0f ms", n, K.path, K.built, later, K.failed, known, dropped, t1 - t0,
           now_ms() - t1, nt, now_ms() - t0);
    free(K.file);
    K.file = NULL;
    plat_mutex_unlock(&P.lock);
    // After an update with another translator: a record names its shaders by input id, which no translator changes,
    // so every record still finds its shaders, and the pipeline cache gives the pipeline back wherever the translation
    // in use is the one it was built from. The line below this one says how many it gave back.
    if (carried || fresh)
        rt_log("vk_pipeline: prewarm: of the %zu shaders the recorded pipelines name, %zu are a previous translator's "
               "translations and %zu were translated now; a recorded pipeline is compiled again only where its shaders' "
               "translation changed, which the pipeline-cache.bin hits below show", u, carried, fresh);
    fb_log("after the boot prewarm");
    vk_mem_stage_log("the recorded pipelines' prebuild");
    vk_pipeline_prewarm_marker_clear();   // the boot's share is done; what the pool builds later runs during play
    lb_start(pr_hist, fs_hist);
    if (BF.on) bf_wait();
}

// ---- boot prebuild crash marker
// The boot builds the recorded pipelines (warm_builder) before the game starts. A driver whose compiler crashes on one
// of them would kill every start at the same point, and the preparation journal above does not cover these builds.
// So <dir>/pipeline-prewarm-running.txt exists while they run: written before the first one, deleted when the boot's
// share ends (or when the boot stops waiting and lets the game start). A start that finds it left over records this
// GPU and driver in <dir>/pipeline-prewarm-skip.txt, and starts on them build no recorded pipelines ahead of play:
// each one is built at its first draw instead, as with NX2NAVI_PIPE_PREBUILD=0. Another driver version, or deleting
// the file, brings the prebuild back. Only the boot's own share is covered: a crash during play, where the pool also
// builds what the boot left, says nothing about these builds. An orderly end of the process during the boot (the
// window closed, the guest's ExitProcess, a --seconds run) deletes the marker as well.
// A marker left over is not yet a driver crash: a start stopped from outside (the task manager, a program that ends
// the game, the power) leaves one too, and since the whole warm (bf_decide) the boot builds for minutes. So the crash
// handler says which it was (vk_pipeline_crash_note: <dir>/boot-build-crash.txt, written while the boot's builds
// run): with that file the next start skips the prebuild as before; without it the end is taken for a stopped start
// and only counted, in the marker itself. BOOT_QUIET_ENDS such ends in a row skip the prebuild after all: a driver
// that takes the process down without a fault our handler sees would otherwise end every start (boot_warm.h
// boot_end_decide).
static int cache_dir(char *dir, size_t n);
static void gpu_key(char *out, size_t n);
static char PM_run[PLAT_PATH_MAX * 2 + 48], PM_crash[PLAT_PATH_MAX * 2 + 48];
static volatile int32_t pm_on;
static volatile int32_t jr_boot_on;   // the boot's journal of library builds is open (journal_open)
static int pm_quiet;                  // starts in a row before this one that ended in the boot's builds without a crash report

static void crash_path_init(const char *dir) {
    if (!PM_crash[0]) snprintf(PM_crash, sizeof PM_crash, "%s/boot-build-crash.txt", dir);
}

// main.c crash_handler, before its report: a fault while the boot's pipeline builds or its journalled library builds
// run. The file is all the next start needs; nothing here allocates.
void vk_pipeline_crash_note(void) {
    static const char text[] = "the process faulted while the boot built pipelines or stage libraries "
                               "(runtime/vk_pipeline.c vk_pipeline_crash_note)\n";
    if (!PM_crash[0] || (!__atomic_load_n(&pm_on, __ATOMIC_ACQUIRE) && !__atomic_load_n(&jr_boot_on, __ATOMIC_ACQUIRE)))
        return;
    plat_file_write(PM_crash, text, sizeof text - 1);
}

// 1 when the last start left the crash handler's note; asked once a start, and the note goes.
static int crash_noted(void) {
    static int was = -1;
    if (was < 0) {
        was = PM_crash[0] && plat_file_exists(PM_crash);
        if (was) plat_file_delete(PM_crash);
    }
    return was;
}

static int pm_skipped(void) {
    char dir[PLAT_PATH_MAX * 2], sp[sizeof PM_run], key[32];
    if (!cache_dir(dir, sizeof dir)) return 0;
    snprintf(PM_run, sizeof PM_run, "%s/pipeline-prewarm-running.txt", dir);
    snprintf(sp, sizeof sp, "%s/pipeline-prewarm-skip.txt", dir);
    crash_path_init(dir);
    gpu_key(key, sizeof key);
    uint64_t n = 0;
    char *left = (char *)plat_file_read(PM_run, &n);
    if (left) {
        char head[64];
        snprintf(head, sizeof head, "%.*s", (int)(n < sizeof head - 1 ? n : sizeof head - 1), left);
        int crashed = crash_noted(), quiet = 0, what = boot_end_decide(head, key, crashed, &quiet);
        if (what == BOOT_END_OTHER) {   // another GPU or driver's builds
            plat_file_delete(PM_run);
        } else if (what == BOOT_END_SKIP) {
            char text[256];
            int len = snprintf(text, sizeof text,
                               "# The boot's pipeline prebuild did not finish on this GPU and driver (runtime/vk_pipeline.c "
                               "pm_skipped): starts skip it\n# while this line matches. Delete this file to build ahead of play "
                               "again.\n%s\n", key);
            if (plat_file_write_atomic(sp, text, (size_t)len)) plat_file_delete(PM_run);
            if (crashed)
                rt_log("vk_pipeline: the last start crashed while the boot built pipelines (%s was left, with the crash "
                       "handler's note); %s now skips that prebuild on this GPU and driver (%s)", PM_run, sp, key);
            else
                rt_log("vk_pipeline: %d starts in a row ended while the boot built pipelines, none with a crash report "
                       "(%s was left each time); %s now skips that prebuild on this GPU and driver (%s)", quiet, PM_run,
                       sp, key);
        } else {
            pm_quiet = quiet;
            plat_file_delete(PM_run);
            rt_log("vk_pipeline: the last start ended while the boot built pipelines and reported no crash: taken for a "
                   "start that was closed or stopped, and the pipelines are built again (%d in a row; at %d the prebuild "
                   "is skipped on this GPU and driver)", pm_quiet, BOOT_QUIET_ENDS);
        }
        free(left);
    }
    uint8_t *skip = plat_file_read(sp, &n);
    int on = 0;
    for (char *line = (char *)skip, *next; line && *line && !on; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = 0;
        on = line[0] != '#' && !strcmp(line, key);
    }
    free(skip);
    if (on)
        rt_log("vk_pipeline: prewarm: %zu recorded pipelines not built ahead of play: an earlier start died while building "
               "them on this GPU and driver (%s lists %s); each builds at its first draw", K.file_n, sp, key);
    return on;
}

static void pm_set(void) {
    if (!PM_run[0]) return;
    char key[32], text[64];
    gpu_key(key, sizeof key);
    int len = snprintf(text, sizeof text, "%s %d\n", key, pm_quiet);
    // straight to the disk: the file has to be there if the driver takes the process down a moment later
    if (plat_file_write_atomic(PM_run, text, (size_t)len)) plat_xchg32(&pm_on, 1);
}

void vk_pipeline_prewarm_marker_clear(void) {
    if (plat_xchg32(&pm_on, 0)) plat_file_delete(PM_run);
}

// ---- the whole warm on the boot screen after a driver change
// A start that finds pipeline-cache.bin written by another device or driver (cache_data_read: a driver update, another
// GPU) has nothing built: not the recorded pipelines, not the compute pipelines, and the driver's own cache of stage
// libraries is empty as well. Left to the background as on any other start, all of it was compiled beside the game:
// an 8-core integrated GPU drew fewer than 600 frames in its first 11 minutes, with the recorded pipelines done after
// 296 s and the compute pipelines after 570 s, the game's own compute dispatches compiling on the replay thread until
// then. Such a start waits for all of it on the boot screen instead, with its counts (boot.c boot_wait): the recorded
// pipelines (prewarm_run, phase 2), then the compute pipelines on up to LB_MAX_THREADS threads at normal priority
// (lb_start, phase 3), then, when a sample says the driver's cache does not hold the prepared stage libraries
// (lw_check), every prepared variant on up to LW_BOOT_THREADS threads (lw_worker, phase 4). The memory gate
// (gate_wait) holds for all of them. The large compute shaders (bg_main) are not waited for: their one thread runs
// beside the library phase.
// It takes minutes (with nothing in the driver's cache, an RTX 5090 and 16 CPU threads: 87 s for 5,775 recorded
// pipelines, 19 s for 4,075 compute pipelines, 317 s for 42,231 prepared variants), once per driver:
// <dir>/boot-warm-owed.txt names the GPU and driver and counts the starts that began the warm (boot_warm.h). A start
// that finds its own GPU and driver there with a count goes on with a warm that an earlier start did not end; what
// was built comes back from the caches in a moment. The start whose warm ends writes "done" there, which keeps the
// next starts ordinary ones: pipeline-cache.bin keeps the earlier driver's header until the play cache is folded
// into it (cache_load), some starts later. After BOOT_WARM_TRIES starts that did not end the warm the file stays as
// well, and starts on this GPU and driver are ordinary ones again. While the boot waits, the library builds write
// the preparation's journal (journal_mark), so a variant the driver's compiler dies on is skipped from the next
// start on (skip_init) instead of ending every start; a start that was only closed or stopped then blames no variant
// (the crash handler's note decides, as at pm_skipped).
// The wait has a limit, NX2NAVI_PREWARM_FULL_S seconds from here (default 600, 0 none): the 425 s above are a fast
// machine's, and a slow one would sit on the screen for half an hour. At the limit the game starts and what is left
// goes on as on any other start: the recorded pipelines on the pool's workers, the compute pipelines and the library
// warm on their idle threads (vk_pipeline_prewarm_detach). The file then says "done" as well: the boot screen had its
// turn, and the rest is the background's, as it always was. NX2NAVI_PREWARM_WAIT_MS and a minute without progress
// end the wait the same way, as for any prewarm (boot_wait).
// NX2NAVI_PREWARM_FULL=0 turns the whole warm off. Not for a --shot image, which no build timing may change and
// which nobody watches.
static void bf_decide(void) {
    const char *full = cfg_get("NX2NAVI_PREWARM_FULL");
    char dir[PLAT_PATH_MAX * 2], key[32], line[100];
    if ((full && !strcmp(full, "0")) || rt_config()->shot || !cache_dir(dir, sizeof dir)) return;
    snprintf(BF.owed, sizeof BF.owed, "%s/boot-warm-owed.txt", dir);
    gpu_key(key, sizeof key);
    uint64_t n = 0;
    char *f = (char *)plat_file_read(BF.owed, &n);
    if (f) snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line - 1 ? n : sizeof line - 1), f);
    int tries = 0, what = boot_warm_decide(f ? line : NULL, key, cache_foreign, &tries);
    if (what == BOOT_WARM_NO) {
        if (f) plat_file_delete(BF.owed);   // another GPU's or driver's
    } else if (what == BOOT_WARM_DONE) {
        if (cache_foreign)
            rt_log("vk_pipeline: the whole warm for this GPU and driver was done by an earlier start (%s); the "
                   "pipeline cache's file keeps the earlier driver's header until the play cache is folded into it",
                   BF.owed);
    } else if (what == BOOT_WARM_GAVE_UP) {
        rt_log("vk_pipeline: the whole warm after a driver change did not end in %d starts on this GPU and driver (%s): "
               "this start builds in the background like any other. Delete %s to have it tried again", BOOT_WARM_TRIES,
               key, BF.owed);
    } else {
        int len = snprintf(line, sizeof line, "%s %d\n", key, tries);
        if (!plat_file_write_atomic(BF.owed, line, (size_t)len)) rt_log("vk_pipeline: cannot write %s", BF.owed);
        BF.on = 1;
        const char *se = cfg_get("NX2NAVI_PREWARM_FULL_S");
        double secs = se && *se ? atof(se) : 600;
        BF.t_end = secs > 0 ? now_ms() + secs * 1000 : 0;
        rt_log("vk_pipeline: the whole warm on the boot screen (%s, start %d of %d at most): the start waits for the "
               "recorded pipelines, the compute pipelines and the driver's cache of stage libraries, for %.0f s at most "
               "(NX2NAVI_PREWARM_FULL_S; 0 = no limit). NX2NAVI_PREWARM_FULL=0 leaves them to the background",
               cache_foreign ? "the pipeline cache was written by another device or driver" : "an earlier start began it",
               tries, BOOT_WARM_TRIES, secs > 0 ? secs : 0.0);
    }
    free(f);
}

// ensure_init's hook: without a prewarm (NX2NAVI_PIPE_PREBUILD=0, or the device came up first elsewhere) the records
// are not built. Caller holds no lock.
static void prebuild_start(void) {
    plat_mutex_lock(&P.lock);
    if (K.file && !K.warm) {
        rt_log("vk_pipeline: prebuild off%s; %zu records not built",
               !cfg_bool("NX2NAVI_PIPE_PREBUILD") ? " (NX2NAVI_PIPE_PREBUILD=0)" : " (no boot prewarm)", K.file_n);
        free(K.file);
        K.file = NULL;
    }
    plat_mutex_unlock(&P.lock);
}

// NX2NAVI_SPV_OVERRIDE=<dir> (test): a compute shader whose SPIR-V hashes (FNV-1a over its bytes, the name vk_dump.c
// gives it) to a file <dir>/<hash>.spv runs that SPIR-V instead, an edited copy with the same interface, to see a
// shader's intermediate values in a frame dump. NULL: no override (malloc'd words otherwise).
static uint32_t *spv_override(const uint32_t *code, uint64_t words, uint64_t *out) {
    const char *dir = cfg_get("NX2NAVI_SPV_OVERRIDE");
    if (!dir || !*dir) return NULL;
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i < words * 4; i++) h = (h ^ ((const uint8_t *)code)[i]) * 0x100000001b3ull;
    char path[600];
    snprintf(path, sizeof path, "%s/%016llx.spv", dir, (unsigned long long)h);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *w = n > 20 && n % 4 == 0 ? malloc((size_t)n) : NULL;
    if (w && fread(w, 1, (size_t)n, f) != (size_t)n) free(w), w = NULL;
    fclose(f);
    if (w) *out = (uint64_t)n / 4, rt_log("vk_pipeline: compute shader %016llx replaced by %s", (unsigned long long)h, path);
    return w;
}

// A skipped froxel's exit in the fog's first compute pass: the block at word at (an OpLabel) makes five image writes, four
// of (0,0,0,1) and last the mask value (1/255,0,0,0), and returns.
static int fog_skip_block(const uint32_t *w, uint64_t n, uint64_t at, const uint64_t *def, uint32_t bound) {
    int writes = 0;
    for (uint64_t i = at + (w[at] >> 16); i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return 0;
        if (op == 253) return writes == 5;              // OpReturn
        if (op == 248 || op == 249 || op == 250) return 0;   // OpLabel, OpBranch, OpBranchConditional
        if (op == 99) {                                 // OpImageWrite
            if (len < 4 || w[i + 3] >= bound || ++writes > 5) return 0;
            uint64_t c = def[w[i + 3]];
            if (!c || (w[c] & 0xffff) != 80 || w[c] >> 16 != 7) return 0;   // OpCompositeConstruct of four
            float v[4];
            for (int k = 0; k < 4; k++) {
                uint32_t id = w[c + 3 + k];
                uint64_t f = id < bound ? def[id] : 0;
                if (!f || (w[f] & 0xffff) != 43 || w[f] >> 16 != 4) return 0;   // OpConstant of one word
                memcpy(&v[k], &w[f + 3], 4);
            }
            if (writes == 5 ? !(v[0] > 0.0039f && v[0] < 0.0040f && v[1] == 0 && v[2] == 0 && v[3] == 0)
                            : !(v[0] == 0 && v[1] == 0 && v[2] == 0 && v[3] == 1)) return 0;
        }
        i += len;
    }
    return 0;
}

// The volumetric fog's first compute pass skips a froxel behind a coarse depth table and writes it as empty, and the next
// pass's mask keeps it empty. That table comes from the frame before, so in the columns a near object has just left, every
// froxel past the object's old depth came out with no fog: a dark block of the fog grid beside the object, which the fog's
// history then smeared into a trail, and worst at the screen edges, where near objects move fastest. A froxel behind
// today's depth is never seen, so computing it only costs a little time: the comparison is made never true (x > x).
// Returns the patched copy of that shader, NULL for any other. Found by its shape rather than a hash, so it holds across
// translator updates: a branch whose other side is that exit, taken on !(distance > table) || flag.
// NX2NAVI_FOG_CULL=1 keeps the game's skip.
static uint32_t *spirv_fog_keep_froxels(const uint32_t *w, uint64_t n) {
    const char *e = cfg_get("NX2NAVI_FOG_CULL");
    if (n < 6 || (e && !strcmp(e, "1"))) return NULL;
    uint32_t bound = w[3];
    int writes = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t len = w[i] >> 16;
        if (!len || i + len > n) return NULL;
        writes += (w[i] & 0xffff) == 99;
        i += len;
    }
    if (writes < 5) return NULL;
    uint64_t *def = calloc(bound, sizeof *def);   // word of each id's definition, for the instructions looked at below
    if (!def) return NULL;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == 248 && len >= 2 && w[i + 1] < bound) def[w[i + 1]] = i;   // OpLabel
        else if ((op == 43 || op == 80 || op == 166 || op == 168 || op == 186) && len >= 3 && w[i + 2] < bound) def[w[i + 2]] = i;
        i += len;
    }
    uint64_t cmp = 0, prev = 0;
    int found = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == 250 && len >= 4 && prev && (w[prev] & 0xffff) == 247 && w[prev + 1] == w[i + 3] && w[i + 1] < bound &&
            w[i + 3] < bound && def[w[i + 3]] && fog_skip_block(w, n, def[w[i + 3]], def, bound)) {   // OpSelectionMerge just before
            uint64_t c = def[w[i + 1]];
            for (int k = 0; c && (w[c] & 0xffff) == 166 && k < 2; k++) {                              // OpLogicalOr
                uint32_t a = w[c + 3 + k];
                uint64_t no = a < bound ? def[a] : 0;
                if (!no || (w[no] & 0xffff) != 168 || w[no + 3] >= bound) continue;                  // OpLogicalNot
                uint64_t gt = def[w[no + 3]];
                if (gt && (w[gt] & 0xffff) == 186) cmp = gt, found++;                                // OpFOrdGreaterThan
            }
        }
        prev = i;
        i += len;
    }
    free(def);
    uint32_t *o = found == 1 ? malloc(n * 4) : NULL;
    if (!o) return NULL;
    memcpy(o, w, n * 4);
    o[cmp + 4] = o[cmp + 3];
    LOG_ONCE("vk_pipeline: fog: froxels behind last frame's depth are computed too");
    return o;
}

// Successful cache-only warm requests. Input ids can differ while their final SPIR-V and pipeline layouts match.
// Only preparation that destroys its pipeline may reuse these entries; a dispatch still needs its own live handle.
// The code is hashed after overrides and fog changes. Subgroup requirements follow that code and the fixed device
// capabilities. Layout handles and creation flags distinguish otherwise identical shader modules.
typedef struct {
    uint8_t code[32];
    VkPipelineLayout layout;
    uint64_t words;
    VkPipelineCreateFlags flags;
    VkPipelineShaderStageCreateFlags stage_flags;
} ComputeWarmKey;
typedef struct ComputeWarmEntry {
    ComputeWarmKey key;
    struct ComputeWarmEntry *next;
} ComputeWarmEntry;
#define COMPUTE_WARM_BUCKETS 1024
static ComputeWarmEntry *compute_warm[COMPUTE_WARM_BUCKETS];
static PlatMutex compute_warm_lock = PLAT_MUTEX_INIT;
static uint64_t compute_warm_reused;

// No in-flight entries: concurrent identical requests may both build, but a failed request never suppresses a retry.
static int compute_warm_remember(const ComputeWarmKey *key, int completed) {
    uint64_t h = fnv(FNV0, key, sizeof *key);
    plat_mutex_lock(&compute_warm_lock);
    ComputeWarmEntry **head = &compute_warm[h % COMPUTE_WARM_BUCKETS], *e = *head;
    while (e && memcmp(&e->key, key, sizeof *key)) e = e->next;
    int have = e != NULL;
    if (!have && completed && (e = malloc(sizeof *e))) e->key = *key, e->next = *head, *head = e;
    plat_mutex_unlock(&compute_warm_lock);
    return have;
}

static void compute_warm_clear(void) {
    plat_mutex_lock(&compute_warm_lock);
    for (size_t i = 0; i < COMPUTE_WARM_BUCKETS; i++) {
        for (ComputeWarmEntry *e = compute_warm[i], *n; e; e = n) n = e->next, free(e);
        compute_warm[i] = NULL;
    }
    plat_mutex_unlock(&compute_warm_lock);
}

// A compute pipeline for cs in layout l; VK_NULL_HANDLE on failure. No lock needed (P.cache is driver-synchronised).
// cached_only (needs P.cache_control): from P.cache or the driver's own disk cache, never compiled; VK_NULL_HANDLE when
// neither holds it (the driver returns VK_PIPELINE_COMPILE_REQUIRED, a success code, with no pipeline).
// already_warm is only supplied by callers that need cache coverage, not a live pipeline: 1 and VK_NULL_HANDLE mean
// an identical request already succeeded in this process. NULL preserves normal creation and has no hashing cost.
// NX2NAVI_COMPUTE_WARM_REUSE=0 disables this reuse for comparisons without changing the prepared inventory.
static VkPipeline compute_create(const GfxShader *cs, const Layout *l, int cached_only, int *already_warm) {
    if (already_warm) *already_warm = 0;
    uint64_t words;
    // pinned until the pipeline is made: a dispatch on the drawing thread that creates its pipeline here decodes a
    // compressed shader first, about 60 us for a mean module against the creation's milliseconds (gfx_shader.c
    // "SPIR-V at rest", counted in the memory report)
    const uint32_t *code = gfx_shader_spirv_pin(cs, &words);
    uint64_t ow = 0;
    uint32_t *over = spv_override(code, words, &ow);
    if (over) code = over, words = ow;
    uint32_t *fog = spirv_fog_keep_froxels(code, words);
    if (fog) free(over), over = fog, code = fog;
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rqc;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .pName = "main"},
                                      .layout = l->layout, .flags = noopt_flag()};
    if (cached_only) ci.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    pin_subgroup32(&ci.stage, &rqc, code, words);
    // Subgroup operations in a workgroup whose width is a whole number of subgroups need every subgroup full: the
    // driver may otherwise run partial ones, and a ballot over the culling's candidates then counted differently from
    // one frame to the next
    uint32_t lx = local_size_x(code, words);
    if (P.f_full_subgroups && vk_caps.sg_size && lx && lx % vk_caps.sg_size == 0 && uses_subgroup_ops(code, words))
        ci.stage.flags |= VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
    ComputeWarmKey warm_key = {0};
    const char *reuse = already_warm ? cfg_get("NX2NAVI_COMPUTE_WARM_REUSE") : NULL;
    int remember = already_warm && (!reuse || strcmp(reuse, "0"));
    if (remember) {
        rt_sha256(code, (size_t)words * 4, warm_key.code);
        warm_key.layout = l->layout, warm_key.words = words;
        warm_key.flags = ci.flags, warm_key.stage_flags = ci.stage.flags;
        if (compute_warm_remember(&warm_key, 0)) {   // before the module: a reused request makes none
            *already_warm = 1;
            __atomic_fetch_add(&compute_warm_reused, 1, __ATOMIC_RELAXED);
            free(over);
            gfx_shader_spirv_unpin(cs);
            return VK_NULL_HANDLE;
        }
    }
    VkShaderModule m = make_module(code, words, 0);
    if (!m) return free(over), gfx_shader_spirv_unpin(cs), VK_NULL_HANDLE;
    ci.stage.module = m;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkPipelineCreationFeedbackCreateInfo fci;
    VkPipelineCreationFeedback fb;
    ci.pNext = fb_chain(&fci, &fb, ci.pNext);
    crash_note("a compute pipeline", cs, NULL, NULL);
    if (pipe_create(2, &ci, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
    if (pipe && remember) compute_warm_remember(&warm_key, 1);
    plat_crash_context(NULL);
    if (pipe) fb_note(FB_COMPUTE, &fb);
    cc_hit = !pipe ? -1 : pb_last ? 1
           : !P.feedback || !P.cache || !(fb.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) ? -1
           : (fb.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0;
    if (P.feedback >= 2 && (pipe || !cached_only))
        rt_log("vk_pipeline: compute %016llx: %llu words, module %016llx, %s, %.1f ms", (unsigned long long)fnv(FNV0, code, words * 4),
               (unsigned long long)words, (unsigned long long)mm_hash,
               !pipe ? "failed" : pb_last ? "from pipeline-binaries.bin" : fb.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT ? "from pipeline-cache.bin" : "compiled",
               (double)fb.duration / 1e6);
    vkDestroyShaderModule(P.device, m, NULL);
    free(over);
    gfx_shader_spirv_unpin(cs);
    return pipe;
}

// The layout of a compute shader in *B; NULL with *why on failure. Caller holds P.lock.
static Layout *compute_layout(const GfxShader *cs, Bindings *B, const char **why) {
    Layout *l = NULL;
    if (gfx_shader_stage(cs) != GFX_STAGE_CS) *why = "not a compute shader";
    else if (!add_stage(B, cs, 0)) *why = "too many bindings";
    else if (!(l = layout_get(B))) *why = "descriptor layout creation failed";
    return l;
}

// ---- stage library prebuild
// Two places build what a draw would otherwise build while the game waits:
// - nx2navi.exe --prepare-shaders (lb_warm_prepared), after the translation, on every core: for each prepared variant
//   (shader-prepared.bin) the pre-rasterization library of a vertex shader, the fragment-shader library of a fragment
//   shader and the pipeline of a compute shader, each built from a private shader copy and destroyed at once. What
//   stays is the driver's own disk cache: on AMD a library built again in a later process takes 0.1 ms instead of
//   55 ms (vertex) / 28 ms (fragment). Our pipeline-cache.bin is not written there:
//   it would grow by about 10 KB per library (~240 MB) and be read into memory at every boot.
// - boot (lb_worker), after the pipeline-key prewarm, on two threads (lb_start) at PLAT_PRIO_IDLE (the short P.lock
//   sections and shader loads at normal priority, so a preempted thread never holds the lock the replay needs; a
//   compute pipeline's compile below normal priority, lb_compute):
//   compute pipelines of every recorded and prepared compute shader (vk_pipeline_compute builds compute
//   synchronously on the replay thread; 163 prepared): those of the shaders dispatched most recently stay, the others
//   are built into the pipeline cache and destroyed (comment at compute-used.bin). Then the libraries of the first
//   NX2NAVI_LIB_PREBUILD_KEEP variants play recorded (priority 0), kept in libs[] with their shaders. KEEP defaults to
//   0: a kept library costs 140-460 KB of process memory (10,000 kept took 9 GB in run 1) to save a rebuild the driver
//   cache makes 0.1 ms.
// Library state: the vertex output union known now (vs_union_peek; the prewarm fills it from pipeline-keys.bin, the
// preparation has none), the clamp, polygon mode and depth mode most recorded pipelines use (defaults without a key
// file: clamp 0, fill, -w..w), single-sampled, no dual source; the state dynamic on this GPU is 0 in every key.
// A draw that meets such a variant in a new pipeline then gets its libraries from libs[] or the driver cache and
// fast-links (gpl_link). NX2NAVI_LIB_PREBUILD=0 turns the boot part off, =N stops it after N variants;
// NX2NAVI_LIB_PREBUILD_THREADS sets its thread count, NX2NAVI_LIB_PREBUILD_BIG its compute size limit (LB_BIG_WORDS).
#define LB_MAX_THREADS 16
// The boot part builds a compute shader over LB_BIG_WORDS SPIR-V words only from the pipeline caches (compute_create
// cached_only); one that no cache holds is left to its first dispatch, like any shader outside the warm list. Play
// recorded four straight-line compute shaders of 0.87-4.4 million words (one function of up to 888K instructions; the
// median of 3,648 compute shaders is 7,351 words, p99 64,844). NVIDIA 621.01 on an RTX 4090 compiled them on four
// prebuild threads for minutes at 100% CPU each while the process grew from 12 GB to 28-31 GB, until RAM ran out and the
// game stuttered and went black a few minutes after boot. None finished, so no cache held one and every boot did it
// again. NX2NAVI_LIB_PREBUILD_BIG sets the limit in words; 0 builds every size.
#define LB_BIG_WORDS (512u * 1024)

static struct {
    GfxWarmEnt *e;
    size_t n;
    size_t n_keep;                                     // e[0..n_keep): compute shaders whose pipeline stays (cu_keep)
    volatile int64_t next;
    volatile int32_t stop, running;
    volatile int32_t short_mem;                        // the memory gate refused a build for GATE_WAIT_MS: the workers ended
    int threads;
    PlatThread *th[LB_MAX_THREADS];
    uint8_t clamp, poly, minus_w, samples;             // library state (GKey fields)
    double t0, ms[3];                                  // ms: build time per kind (vertex, fragment, compute)
    // warmed: built for the caches only and destroyed; nolib: vertex and fragment variants skipped with the graphics
    // pipeline library off
    // skipped: variants of the skip list (SK below)
    uint64_t built[3], have[3], fail[3], warmed[3], noshader, nolib, skipped, done, cs_draw;
    uint64_t big_words, deferred;                      // LB_BIG_WORDS; compute shaders over it left to their dispatch
    uint64_t big_unused;                               // compute shaders over it that no run dispatched: not built at all
    double warm_ms[3];
    volatile int32_t kept;                                // libraries entered in libs[] so far
    int32_t keep_n;
    int idle;                                          // build at PLAT_PRIO_IDLE (boot), normal (preparation)
    int tail_one;                                      // automatic boot policy: only worker 0 takes the warming tail
    int bg_threads;                                    // boot: the workers that go on beside the game (lb_next)
    PlatMutex stat;
} LB = {.stat = PLAT_MUTEX_INIT};
static __thread int lb_me = -1;        // on a boot prebuild thread (lb_worker): its index in LB.th

// The boot's builders run beside the game, at idle priority; not while the start waits for them (bf_waiting).
static int lb_idle(void) {
    return LB.idle && !bf_waiting();
}

// Claim a boot job without dropping one when the extra workers finish the recent-pipeline prefix. Recent pipelines
// retain parallel creation; a single worker continues the complete tail. Explicit thread counts override this
// policy, and installer preparation uses its own queue at full concurrency. The CAS makes the prefix boundary
// exact even when two workers reach it together. No pipeline creation or dispatch waits on this queue lock.
// The whole warm (bf_wait) starts more workers than go on beside a game: those end here once the game runs.
static size_t lb_next(int worker) {
    int64_t next = __atomic_load_n(&LB.next, __ATOMIC_RELAXED);
    for (;;) {
        int bg = lb_idle();
        if (LB.stop || (size_t)next >= LB.n || (bg && worker >= LB.bg_threads) ||
            (bg && LB.tail_one && worker > 0 && (size_t)next >= LB.n_keep)) return LB.n;
        if (__atomic_compare_exchange_n(&LB.next, &next, next + 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return (size_t)next;
    }
}

// ---- preparation that outlives a driver crash. A bug in the driver's shader compiler can kill the process inside
// vkCreate*Pipelines: NVIDIA 615.78.08 on an RTX 5080 crashed on the same vertex library at the same point of every
// preparation, natively and under Wine, so preparation never finished (issue #30 and a report on PR #60).
// While lb_warm_prepared builds, slot k of <dir>/shader-prepare-journal.bin holds the variant its worker k has handed to
// the driver (input id and stage; zeros between variants). The writes go straight to the OS, so they outlast the
// process, and a run that finishes deletes the file. The next preparation or boot (skip_init) moves the variants a
// leftover journal names into <dir>/shader-prepare-skip.txt with this GPU and driver version, and preparation and the
// boot prebuild (lb_worker) skip what that file lists for them. The variants the other workers were building at the
// crash are skipped along with the culprit; like it, they build at their first draw, where the driver can still crash.
// Another driver version builds them again, and so does deleting the file. tools/prepare_shaders.py runs the build
// again (--warm-shaders) after a run that left a journal.
#define JR_SLOT 64    // bytes per journal slot: input id, stage, zeros
#define JR_SLOTS 64   // lb_warm_prepared's thread cap

static struct {
    PlatFile *journal;     // while lb_warm_prepared builds, and while the start waits for the library warm (bf_wait)
    PlatMutex jlock;       // journal: bf_wait and the process's end close it while workers write
    char jpath[PLAT_PATH_MAX * 2 + 48];   // the boot's journal, "" without one
    uint8_t (*iid)[32];    // the skip list's variants for this GPU and driver
    size_t n;
    int loaded;
} SK = {.jlock = PLAT_MUTEX_INIT};

static int cache_dir(char *dir, size_t n) {   // the folder of pipeline-cache.bin and shader-prepared.bin
    snprintf(dir, n, "%s", P.cache_path);
    char *cut = plat_last_sep(dir);
    if (!cut) return 0;
    *cut = 0;
    return 1;
}

static void gpu_key(char *out, size_t n) {   // vendor:device:driver version, as the skip list names a GPU and driver
    const VkPhysicalDeviceProperties *p = &vk_ctx()->props;
    snprintf(out, n, "%04x:%04x:%08x", p->vendorID, p->deviceID, p->driverVersion);
}

static const char *stage_name(int st) {
    return st == GFX_STAGE_VS ? "vertex" : st == GFX_STAGE_FS ? "fragment" : st == GFX_STAGE_CS ? "compute" : "other";
}

static int hex_digit(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// The variants a leftover journal j (jn bytes, of file jp) names join the skip list's text *s (*sn bytes; NULL
// without a list) for this GPU and driver, and the list's file sp is written again. 1 when the journal may go:
// nothing in it, or the list holds it now.
static int journal_fold(const uint8_t *j, uint64_t jn, const char *jp, const char *sp, const char *key, char **s,
                        uint64_t *sn) {
    static const char head[] =
        "# Shader variants the graphics driver was building when the process died during preparation\n"
        "# (runtime/vk_pipeline.c skip_init): input id, stage, GPU vendor:device:driver version. Preparation skips\n"
        "# them on that GPU and driver. Delete this file to build them again.\n";
    size_t cap = (*s ? *sn : sizeof head) + (jn / JR_SLOT + 1) * 128, len = 0;
    char *out = malloc(cap);
    int added = 0;
    if (!out) return 1;
    len = *s ? *sn : sizeof head - 1;
    memcpy(out, *s ? *s : head, len);
    if (len && out[len - 1] != '\n') out[len++] = '\n';
    for (uint64_t o = 0; o + 33 <= jn; o += JR_SLOT) {
        int set = 0;
        for (int b = 0; b < 32; b++) set |= j[o + b];
        if (!set) continue;
        char hex[65];
        for (int b = 0; b < 32; b++) snprintf(hex + 2 * b, 3, "%02x", j[o + b]);
        len += (size_t)snprintf(out + len, cap - len, "%s %s %s\n", hex, stage_name(j[o + 32]), key);
        added++;
    }
    out[len] = 0;   // parsed by skip_init as the list's text
    if (added && plat_file_write_atomic(sp, out, len)) {
        rt_log("vk_pipeline: the last shader preparation, or the last start's warm on the boot screen, died while "
               "the driver built %d variants; %s now lists them and they are skipped ahead of play on this GPU and "
               "driver (%s)", added, sp, key);
        free(*s), *s = out, *sn = len;
        return 1;
    }
    if (added) rt_log("vk_pipeline: cannot write %s; %s stays for the next run", sp, jp);
    free(out);
    return !added;
}

// A journal a run that died left into the skip list, then the skip list's variants for this GPU and driver into SK.
// The preparation's journal (shader-prepare-journal.bin) always counts. The boot's (shader-warm-journal.bin,
// journal_open) counts only with the crash handler's note (crash_noted): without it the start was closed or stopped
// while it waited, and the variants it happened to be building are no one's fault.
static void skip_init(const char *dir) {
    if (SK.loaded) return;
    SK.loaded = 1;
    char jp[PLAT_PATH_MAX * 2 + 48], bj[PLAT_PATH_MAX * 2 + 48], sp[PLAT_PATH_MAX * 2 + 48], key[32];
    snprintf(jp, sizeof jp, "%s/shader-prepare-journal.bin", dir);
    snprintf(bj, sizeof bj, "%s/shader-warm-journal.bin", dir);
    snprintf(sp, sizeof sp, "%s/shader-prepare-skip.txt", dir);
    crash_path_init(dir);
    gpu_key(key, sizeof key);
    uint64_t jn = 0, bn = 0, sn = 0;
    uint8_t *j = plat_file_read(jp, &jn), *b = plat_file_read(bj, &bn);
    char *s = (char *)plat_file_read(sp, &sn);
    if (!j || journal_fold(j, jn, jp, sp, key, &s, &sn)) plat_file_delete(jp);
    if (b) {
        int marked = 0;
        for (uint64_t o = 0; o + 33 <= bn; o += JR_SLOT)
            for (int k = 0; k < 32; k++)
                if (b[o + k]) { marked++; break; }
        if (marked && !crash_noted()) {
            rt_log("vk_pipeline: the last start ended during the library warm on the boot screen, with %d variants in "
                   "the driver's hands, and reported no crash: taken for a start that was closed or stopped; no variant "
                   "is skipped for it", marked);
            plat_file_delete(bj);
        } else if (journal_fold(b, bn, bj, sp, key, &s, &sn)) {
            plat_file_delete(bj);
        }
    }
    free(j), free(b);
    for (char *line = s, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        char hex[65], st[16], k[32];
        uint8_t iid[32];
        int ok = line[0] != '#' && sscanf(line, "%64s %15s %31s", hex, st, k) == 3 && strlen(hex) == 64 &&
                 !strcmp(k, key);
        for (int b2 = 0; ok && b2 < 32; b2++) {
            int hi = hex_digit(hex[2 * b2]), lo = hex_digit(hex[2 * b2 + 1]);
            if (hi < 0 || lo < 0) ok = 0;
            else iid[b2] = (uint8_t)(hi << 4 | lo);
        }
        uint8_t (*grown)[32] = ok ? realloc(SK.iid, (SK.n + 1) * sizeof *SK.iid) : NULL;
        if (grown) memcpy(grown[SK.n++], iid, 32), SK.iid = grown;
    }
    free(s);
    if (SK.n) rt_log("vk_pipeline: %zu shader variants skipped ahead of play on this GPU and driver (%s)", SK.n, sp);
}

static int skip_listed(const uint8_t iid[32]) {
    for (size_t i = 0; i < SK.n; i++)
        if (!memcmp(SK.iid[i], iid, 32)) return 1;
    return 0;
}

static void skip_count(void) {
    plat_mutex_lock(&LB.stat);
    LB.skipped++;
    plat_mutex_unlock(&LB.stat);
}

// Journal slot `slot`: the variant e its worker hands to the driver next, or zeros (e NULL) when the build returned.
static void journal_mark(int slot, const GfxWarmEnt *e) {
    if (slot < 0 || slot >= JR_SLOTS) return;
    uint8_t rec[JR_SLOT] = {0};
    if (e) memcpy(rec, e->iid, 32), rec[32] = e->stage;
    plat_mutex_lock(&SK.jlock);
    if (SK.journal) plat_file_pwrite(SK.journal, rec, sizeof rec, (uint64_t)slot * JR_SLOT);
    plat_mutex_unlock(&SK.jlock);
}

// The boot's journal, for the library builds the start waits for (bf_wait), in a file of its own: skip_init treats
// one left over differently from the preparation's. skip_init has run (lb_start) and has dealt with a leftover one.
static void journal_open(void) {
    char dir[PLAT_PATH_MAX * 2];
    if (!cache_dir(dir, sizeof dir)) return;
    crash_path_init(dir);
    snprintf(SK.jpath, sizeof SK.jpath, "%s/shader-warm-journal.bin", dir);
    PlatFile *j = plat_fs_create(SK.jpath, NULL);
    if (!j) {
        rt_log("vk_pipeline: cannot create %s: a driver crash in this start's library warm will not be recorded", SK.jpath);
        SK.jpath[0] = 0;
        return;
    }
    plat_mutex_lock(&SK.jlock);
    SK.journal = j;
    plat_mutex_unlock(&SK.jlock);
    __atomic_store_n(&jr_boot_on, 1, __ATOMIC_RELEASE);
}

// Ends the boot's journal: when the library warm is done, when the game starts beside it (a crash in play says
// nothing about the variant a background builder has in hand) and when the process ends in order during the wait
// (vk_pipeline_save_exit), so that only a process that died while the start waited leaves one. The builds still in
// flight write nothing afterwards (journal_mark).
static void journal_close(void) {
    if (!SK.jpath[0]) return;
    __atomic_store_n(&jr_boot_on, 0, __ATOMIC_RELEASE);
    plat_mutex_lock(&SK.jlock);
    PlatFile *j = SK.journal;
    SK.journal = NULL;
    plat_mutex_unlock(&SK.jlock);
    if (!j) return;
    plat_file_close(j);
    plat_file_delete(SK.jpath);
}

// Compute shaders whose pipeline a thread is compiling now (P.lock): a cold driver takes about 2 s for one of Ocarina's
// large compute shaders. The replay used to compile under P.lock, which held every other pipeline lookup that long, and
// compiled a second copy of a pipeline the boot prebuild (lb_compute) was already compiling.
#define CS_FLIGHT 32
static const GfxShader *cs_flight[CS_FLIGHT];

static int cs_flight_has(const GfxShader *cs) {
    for (int i = 0; i < CS_FLIGHT; i++)
        if (cs_flight[i] == cs) return 1;
    return 0;
}

static volatile uint8_t cs_flight_want[CS_FLIGHT];   // a dispatch waits for the slot's pipeline (vk_pipeline_compute)
static PlatThread *cs_flight_th[CS_FLIGHT];          // the boot prebuild thread compiling it below normal priority

// th: the thread a waiting dispatch raises to normal priority, NULL when it compiles at normal priority already.
static int cs_flight_add(const GfxShader *cs, PlatThread *th) {   // 0 when the table is full (the caller compiles anyway)
    for (int i = 0; i < CS_FLIGHT; i++)
        if (!cs_flight[i]) return cs_flight[i] = cs, cs_flight_want[i] = 0, cs_flight_th[i] = th, 1;
    return 0;
}

// A dispatch waits for cs's build: a builder that would only have filled the pipeline cache keeps the pipeline. Returns
// the thread to raise, the first time.
static PlatThread *cs_flight_wanted(const GfxShader *cs) {
    PlatThread *th = NULL;
    for (int i = 0; i < CS_FLIGHT; i++)
        if (cs_flight[i] == cs && !cs_flight_want[i]) cs_flight_want[i] = 1, th = cs_flight_th[i];
    return th;
}

// Without P.lock, for the thread that holds cs's slot: whether a dispatch came to wait.
static int cs_flight_is_wanted(const GfxShader *cs) {
    for (int i = 0; i < CS_FLIGHT; i++)
        if (__atomic_load_n(&cs_flight[i], __ATOMIC_RELAXED) == cs) return cs_flight_want[i];
    return 0;
}

static int cs_flight_remove(const GfxShader *cs) {   // 1 when a dispatch waited for it
    int want = 0;
    for (int i = 0; i < CS_FLIGHT; i++)
        if (cs_flight[i] == cs) cs_flight[i] = NULL, want |= cs_flight_want[i];
    return want;
}

// ---- compute pipelines the boot keeps (compute-used.bin next to pipeline-keys.bin; NX2NAVI_COMPUTE_KEEP)
// The boot prebuild (lb_compute) used to build the pipeline of every compute shader of the warm list (the ones play
// recorded and the prepared ones, shader-prepared.bin) and keep all of them for the process: 4,066 on an installed
// copy (42,344 variants in its list, 42,142 of them prepared), of which a boot into the first village dispatched 95.
// What that cost was twenty of them. A copy's list has about twenty compute shaders of 0.9 to 2.0 million SPIR-V
// words: the game's tensor-core upscaler programs, which the game never dispatches natively (vk_exec_draw.c
// native_tensor) and which no run of the usage record below ever dispatched. A pipeline of one holds about 100 MiB
// of device memory from the moment it exists, used or not, and the driver keeps that memory when the pipeline is
// destroyed. On an RTX 5090 (NVIDIA 621.01) a boot pass that read all 4,066 pipelines from pipeline-cache.bin took the
// device memory from 386 to 2,398 MiB although it destroyed 3,949 of them at once; the same pass before the twenty
// were in the cache left it at 386, and building only the twenty took it to 1,370 (--probe-prewarm, 2026-11-10; the
// game showed 608 to 2,743 MiB over the same stage). The 4,046 others are cheap: all of them kept cost 2 MiB of device
// memory and 0.7 GB of the process's commit. So a copy with prepared shaders and a filled pipeline cache held 2 GiB
// for nothing from the title screen on, on cards that need that memory for textures.
// What the boot does now:
// - A compute shader over LB.big_words is built ahead only when a run has dispatched it (lb_compute, its keep
//   argument). One that no run dispatched is not created at all: not from the pipeline caches and not by the
//   background builder (bg_main). If the game does dispatch one for the first time, that dispatch builds it, which
//   holds the frame for the compile once (0.65 to 1.9 s measured for such a shader), and the record has it built ahead
//   from the next boot on.
// - Of the others the boot keeps the pipelines of the NX2NAVI_COMPUTE_KEEP (default 512) dispatched most recently, and
//   builds every other one into the pipeline cache and destroys it at once (lb_compute keep 0), from a copy of its
//   shader that is released again (lb_worker): the cache then holds it, so its first dispatch creates it without
//   compiling. On that machine 94 first dispatches in play took 0.24 ms on average and 1.6 ms at most, 14 of them in
//   the busiest 33 ms; reading the shader from the disk at that moment adds 0.05 to 0.15 ms (--probe-prewarm).
// The record: one entry per compute shader a run dispatched (its input id) with the number of the last run that did;
// each run that dispatches anything takes the next number. Kept at boot are the keep_n entries with the highest
// numbers, so a short run does not push out what a long one used, and nothing is forgotten until newer shaders take
// its place. Without a record (the first run that has one) nothing is kept and the run pays the first dispatches once.
// NX2NAVI_COMPUTE_KEEP=all builds and keeps every compute shader of the warm list, the large ones too, as before; 0
// keeps none.
#define CU_VERSION 1
typedef struct { char magic[8]; uint32_t version, rec_size, run, pad; } CuHeader;
typedef struct { uint8_t iid[32]; uint32_t run, pad; } CuRec;
static struct {
    PlatMutex lock;
    int opened, dirty;
    char path[PLAT_PATH_MAX * 2 + 32];
    CuRec *rec;
    uint8_t *keep;                     // per record read at boot: among the keep_n dispatched most recently
    uint32_t n, cap, n_file;           // n_file: the records read at boot (keep has one byte for each)
    uint32_t *slot;                    // open addressing by the id's first bytes: record index + 1, 0 empty
    uint32_t slot_cap;
    uint32_t run;                      // this run's number
    int quiet;                         // --probe-prewarm: its dispatches are not the game's, the file stays as it is
    int32_t keep_n;                    // NX2NAVI_COMPUTE_KEEP; -1: all
    uint32_t kept;                     // records flagged in keep
} CU = {.lock = PLAT_MUTEX_INIT};

// What the first dispatches of compute shaders cost this run (under P.lock): each creates its pipeline on the replay
// thread. burst: the creations that fell into one window of CF_WINDOW_MS, the worst such window kept, which is what a
// player would feel as one long frame when a new area starts.
#define CF_WINDOW_MS 33.0
static struct {
    uint64_t n, hit, pre_used;         // hit: the pipeline cache held it (cc_hit 1); pre_used: kept from the boot and dispatched
    double ms, max_ms, hit_ms;
    double win_t0, win_ms, worst_ms;
    uint32_t win_n, worst_n;
} CF;

static uint32_t *cu_slot(const uint8_t iid[32]) {   // the id's slot, or the empty one it would take. Caller holds CU.lock.
    uint64_t h;
    memcpy(&h, iid, 8);
    for (uint32_t j = (uint32_t)(h * 0x9e3779b97f4a7c15ull >> 32) & (CU.slot_cap - 1);; j = (j + 1) & (CU.slot_cap - 1))
        if (!CU.slot[j] || !memcmp(CU.rec[CU.slot[j] - 1].iid, iid, 32)) return &CU.slot[j];
}

// Room for one more record and a table at most half full. 0 when out of memory. Caller holds CU.lock.
static int cu_room(void) {
    if (CU.n == CU.cap) {
        uint32_t cap = CU.cap ? CU.cap * 2 : 1024;
        CuRec *t = realloc(CU.rec, cap * sizeof *t);
        if (!t) return 0;
        CU.rec = t, CU.cap = cap;
    }
    if ((CU.n + 1) * 2 > CU.slot_cap) {
        uint32_t cap = CU.slot_cap ? CU.slot_cap * 2 : 4096;
        uint32_t *t = calloc(cap, sizeof *t), *old = CU.slot;
        if (!t) return 0;
        CU.slot = t, CU.slot_cap = cap;
        for (uint32_t i = 0; i < CU.n; i++) *cu_slot(CU.rec[i].iid) = i + 1;
        free(old);
    }
    return 1;
}

static int cu_run_cmp(const void *a, const void *b) {   // {run, index} pairs, the highest run first
    const uint32_t *x = a, *y = b;
    return x[0] != y[0] ? (x[0] < y[0] ? 1 : -1) : (x[1] > y[1]) - (x[1] < y[1]);
}

// The number of records in a compute-used.bin image (f, size) with its header in *h, or -1 when it is not one.
static long cu_parse(const uint8_t *f, uint64_t size, CuHeader *h) {
    if (!f || size < sizeof *h) return -1;
    memcpy(h, f, sizeof *h);
    if (memcmp(h->magic, "NX2NCUSE", 8) || h->version != CU_VERSION || h->rec_size != sizeof(CuRec)) return -1;
    return (long)((size - sizeof *h) / sizeof(CuRec));
}

// Reads the record and marks what the boot keeps. Caller holds P.lock (ensure_init), after keys_open.
static void cu_open(void) {
    if (!A.on || !K.path[0]) return;
    const char *e = cfg_value("NX2NAVI_COMPUTE_KEEP");
    plat_mutex_lock(&CU.lock);
    int count = cfg_int("NX2NAVI_COMPUTE_KEEP");   // a value that is neither "all" nor a number reads as the default
    CU.keep_n = !strcmp(e, "all") ? -1 : count < 0 ? 0 : count;
    snprintf(CU.path, sizeof CU.path, "%s", K.path);
    char *cut = plat_last_sep(CU.path);
    snprintf(cut ? cut + 1 : CU.path, sizeof CU.path - (size_t)(cut ? cut + 1 - CU.path : 0), "compute-used.bin");
    uint64_t size = 0;
    uint8_t *f = plat_file_read(CU.path, &size);
    CuHeader h;
    long n = cu_parse(f, size, &h);
    if (n >= 0) {
        for (long i = 0; i < n && cu_room(); i++) {
            CuRec r;
            memcpy(&r, f + sizeof h + (size_t)i * sizeof r, sizeof r);
            uint32_t *s = cu_slot(r.iid);
            if (*s) continue;          // an id twice: the first entry stands
            CU.rec[CU.n] = r, *s = ++CU.n;
        }
        CU.run = h.run;
    } else if (f) {
        rt_log("vk_pipeline: %s has another format or version; it is rewritten", CU.path);
    }
    free(f);
    CU.n_file = CU.n, CU.run++;
    uint32_t (*order)[2] = CU.n ? malloc(CU.n * sizeof *order) : NULL;
    CU.keep = CU.n ? calloc(CU.n, 1) : NULL;
    if (order && CU.keep) {
        for (uint32_t i = 0; i < CU.n; i++) order[i][0] = CU.rec[i].run, order[i][1] = i;
        qsort(order, CU.n, sizeof *order, cu_run_cmp);
        for (uint32_t i = 0; i < CU.n && (CU.keep_n < 0 || i < (uint32_t)CU.keep_n); i++) CU.keep[order[i][1]] = 1, CU.kept++;
    }
    free(order);
    CU.opened = 1;
    rt_log("vk_pipeline: %s: %u compute shaders dispatched in earlier runs; the boot keeps the pipelines of %s%u "
           "dispatched most recently (NX2NAVI_COMPUTE_KEEP %s)", CU.path, CU.n,
           CU.keep_n < 0 ? "every recorded compute shader, among them the " : "the ", CU.kept, e);
    plat_mutex_unlock(&CU.lock);
}

// 1 when the boot keeps the pipeline of the compute shader with this input id.
static int cu_keep(const uint8_t iid[32]) {
    plat_mutex_lock(&CU.lock);
    int keep = CU.keep_n < 0 || !CU.opened;   // without the record (no cache folder): as before, every one
    if (!keep && CU.slot_cap) {
        uint32_t s = *cu_slot(iid);
        keep = s && s <= CU.n_file && CU.keep && CU.keep[s - 1];
    }
    plat_mutex_unlock(&CU.lock);
    return keep;
}

// The first dispatch of cs in this run: its entry takes this run's number. The runtime's own shaders (no input id)
// are built at their first use every run and are not recorded.
static void cu_note(const GfxShader *cs) {
    static const uint8_t none[32];
    const uint8_t *iid = gfx_shader_input_id(cs);
    if (!CU.opened || !memcmp(iid, none, 32)) return;
    plat_mutex_lock(&CU.lock);
    if (cu_room()) {
        uint32_t *s = cu_slot(iid);
        if (!*s) {
            memset(&CU.rec[CU.n], 0, sizeof *CU.rec);
            memcpy(CU.rec[CU.n].iid, iid, 32);
            *s = ++CU.n;
        }
        if (CU.rec[*s - 1].run != CU.run) CU.rec[*s - 1].run = CU.run, CU.dirty = 1;
    }
    plat_mutex_unlock(&CU.lock);
}

// Writes the record to path. 1 when written.
// One writer at a time, from the copy of the record to the end of the file write: the autosave thread and a save at
// exit can both be here, and the older copy must not land on the newer file.
static PlatMutex cu_io = PLAT_MUTEX_INIT;

static int cu_write(const char *path) {
    plat_mutex_lock(&cu_io);
    plat_mutex_lock(&CU.lock);
    uint32_t n = CU.n;
    plat_mutex_unlock(&CU.lock);
    // allocated outside CU.lock, which a dispatch takes (cu_note); room for the entries that come meanwhile
    size_t cap = sizeof(CuHeader) + ((size_t)n + 256) * sizeof(CuRec), size = 0;
    uint8_t *out = malloc(cap);
    plat_mutex_lock(&CU.lock);
    CuHeader h = {{'N', 'X', '2', 'N', 'C', 'U', 'S', 'E'}, CU_VERSION, sizeof(CuRec), CU.run, 0};
    if (out && sizeof h + CU.n * sizeof(CuRec) <= cap) {
        size = sizeof h + CU.n * sizeof(CuRec);
        memcpy(out, &h, sizeof h);
        if (CU.n) memcpy(out + sizeof h, CU.rec, CU.n * sizeof(CuRec));
        CU.dirty = 0;
    }
    plat_mutex_unlock(&CU.lock);
    int ok = size && plat_file_write_atomic(path, out, size);
    if (!ok) {   // still to be written: the next flush tries again
        plat_mutex_lock(&CU.lock);
        CU.dirty = 1;
        plat_mutex_unlock(&CU.lock);
    }
    free(out);
    plat_mutex_unlock(&cu_io);
    return ok;
}

// Writes the record when a dispatch changed it (the autosave thread and the save at exit).
static void cu_flush(void) {
    if (!CU.opened || CU.quiet || !*(volatile int *)&CU.dirty) return;
    if (!cu_write(CU.path)) LOG_ONCE("vk_pipeline: cannot write %s; the next saves try again", CU.path);
}

// A compute pipeline created at a dispatch took ms. Caller holds P.lock.
static void cf_note(double ms, int hit) {
    double now = now_ms();
    CF.n++, CF.ms += ms;
    if (ms > CF.max_ms) CF.max_ms = ms;
    if (hit == 1) CF.hit++, CF.hit_ms += ms;
    if (now - CF.win_t0 > CF_WINDOW_MS) CF.win_t0 = now, CF.win_n = 0, CF.win_ms = 0;
    CF.win_n++, CF.win_ms += ms;
    if (CF.win_ms > CF.worst_ms) CF.worst_ms = CF.win_ms, CF.worst_n = CF.win_n;
}

static void cf_log(void) {
    plat_mutex_lock_shared(&P.lock);
    typeof(CF) c = CF;                 // the line is written after the unlock: every dispatch needs P.lock, and this
    plat_mutex_unlock_shared(&P.lock); // runs on the autosave thread below normal priority
    if (c.n || c.pre_used)
        rt_log("vk_pipeline: compute pipelines created at their first dispatch: %llu (avg %.2f ms, longest %.2f ms; the "
               "pipeline cache held %llu of them, avg %.2f ms); the %.0f ms that held the most of it: %u pipelines, "
               "%.1f ms; of the compute pipelines kept from the boot, %llu were dispatched",
               (unsigned long long)c.n, c.n ? c.ms / c.n : 0.0, c.max_ms, (unsigned long long)c.hit,
               c.hit ? c.hit_ms / c.hit : 0.0, CF_WINDOW_MS, c.worst_n, c.worst_ms, (unsigned long long)c.pre_used);
}

// The background builder of large compute shaders (comment at bg_main).
static struct {
    const GfxShader **cs;
    uint8_t (*iid)[32];
    uint64_t *words;
    size_t n, cap;
    PlatThread *th;
    const GfxShader *volatile building;   // the shader on the driver now, NULL between builds
    volatile int32_t raised;             // a dispatch waited for it and raised the thread
    uint64_t built, failed, there;
    double ms;
} BG;

VkPipeline vk_pipeline_compute(const GfxShader *cs, const VkPipeInfo **info) {
    if (info) *info = NULL;
    if (!cs || !ensure_init()) return VK_NULL_HANDLE;
    struct { const GfxShader *cs; uint64_t compute; } k = {cs, 1};   // no padding: the key is hashed and compared as bytes
    uint64_t h = key_hash(&k, sizeof k);
    plat_mutex_lock(&P.lock);
    Pipe *p = pipe_find(&k, sizeof k, h);
    double w0 = !p && cs_flight_has(cs) ? now_ms() : 0;
    int bg = 0;
    while (!p && cs_flight_has(cs)) {   // another thread is compiling it: its result, not a second compile
        // a build that only fills the pipeline cache keeps its pipeline for this dispatch, and a boot prebuild thread
        // compiling it below normal priority goes to normal
        PlatThread *builder = cs_flight_wanted(cs);
        if (builder) plat_thread_set_priority(builder, PLAT_PRIO_NORMAL);
        if (BG.building == cs && BG.th && !plat_xchg32(&BG.raised, 1))   // the background builder: normal priority now
            plat_thread_set_priority(BG.th, PLAT_PRIO_NORMAL), bg = 1;
        plat_mutex_unlock(&P.lock);
        plat_sleep_ms(1);
        plat_mutex_lock(&P.lock);
        p = pipe_find(&k, sizeof k, h);
    }
    if (w0 && now_ms() - w0 >= 2.0) {
        static int logs;
        if (logs++ < 16)
            rt_log("vk_exec: a dispatch waited %.0f ms for its compute pipeline (%s)", now_ms() - w0,
                   bg ? "built on the background thread" : "built by another thread");
    }
    if (p) {
        P.stats.hits++;
    } else {
        const char *why = NULL;
        Bindings B = {0};
        Layout *l = compute_layout(cs, &B, &why);
        VkPipeline pipe = VK_NULL_HANDLE;
        if (!why) {
            int flight = cs_flight_add(cs, NULL);
            plat_mutex_unlock(&P.lock);   // the compile runs without the lock
            uint64_t p0 = perf_begin();
            double c0 = now_ms();
            pipe = compute_create(cs, l, 0, NULL);
            double cms = now_ms() - c0;
            int hit = cc_hit;
            perf_end(PERF_PIPE, p0);
            plat_mutex_lock(&P.lock);
            if (flight) cs_flight_remove(cs);
            if (!pipe) why = "pipeline creation failed";
            else cf_note(cms, hit);
        }
        if ((p = pipe_find(&k, sizeof k, h))) {   // another thread added it meanwhile (no flight slot was free)
            if (pipe) vkDestroyPipeline(P.device, pipe, NULL);
        } else {
            if (why) {
                P.stats.failures++;
                rt_log("vk_pipeline: compute pipeline not built: %s", why);
            } else {
                P.stats.compute++;
                LB.cs_draw++;
            }
            p = pipe_add(&k, sizeof k, h, pipe, l, l ? &B : NULL, VK_PIPELINE_BIND_POINT_COMPUTE, 0);
        }
    }
    if (p && !p->used) {   // the first dispatch of this run: the boot of the next runs keeps this shader's pipeline
        p->used = 1;
        CF.pre_used += p->pre;
        if (p->info.pipeline) cu_note(cs);
    }
    if (p) pipe_used(p);
    plat_mutex_unlock(&P.lock);
    if (!p) return VK_NULL_HANDLE;
    if (info) *info = &p->info;
    return p->info.pipeline;
}

static void lb_lock(void) {
    plat_thread_priority(PLAT_PRIO_NORMAL);
    plat_mutex_lock(&P.lock);
}

static void lb_unlock(void) {
    plat_mutex_unlock(&P.lock);
    if (lb_idle()) plat_thread_priority(PLAT_PRIO_IDLE);
}

static void lb_count(int kind, int what, double ms) {   // what: 0 built, 1 already there, 2 failed, 3 cache only
    plat_mutex_lock(&LB.stat);
    if (what == 3) LB.warmed[kind]++, LB.warm_ms[kind] += ms;
    else (what == 0 ? LB.built : what == 1 ? LB.have : LB.fail)[kind]++, LB.ms[kind] += ms;
    plat_mutex_unlock(&LB.stat);
}

// vs_union_missing for vs alone, without entering vs into the union table (it holds VS_OUTS_CAP shaders).
static uint32_t vs_union_peek(const GfxShader *vs, GfxShaderIo fin[32]) {
    GfxShaderIo vout[32];
    uint32_t need = 0, have = gfx_shader_outputs(vs, vout), missing = 0;
    memset(fin, 0, 32 * sizeof *fin);
    plat_mutex_lock_shared(&VO.lock);
    uint32_t j = (uint32_t)(((uintptr_t)vs >> 4) * 0x9e3779b1u) & (VS_OUTS_CAP - 1);
    while (VO.vs[j] && VO.vs[j] != vs) j = (j + 1) & (VS_OUTS_CAP - 1);
    if (VO.vs[j]) need = VO.mask[j], memcpy(fin, VO.io[j], 32 * sizeof *fin);
    plat_mutex_unlock_shared(&VO.lock);
    if (P.vs_outs_all)
        for (int i = 0; i < 32; i++)
            if (!(need >> i & 1)) fin[i] = (GfxShaderIo){GFX_TYPE_FLOAT, 4, 0}, need |= 1u << i;
    for (int i = 0; i < 32; i++)
        if (need >> i & 1 && !(have >> i & 1)) missing |= 1u << i;
    return missing;
}

// A compute shader over LB.big_words that no pipeline cache holds: its first dispatch builds it (vk_pipeline_compute).
static void lb_defer(const uint8_t iid[32], uint64_t words) {
    plat_mutex_lock(&LB.stat);
    LB.deferred++;
    plat_mutex_unlock(&LB.stat);
    rt_log("vk_pipeline: stage library prebuild: compute shader %02x%02x%02x%02x%02x%02x%02x%02x (%llu SPIR-V words) %s: "
           "left to its first dispatch (NX2NAVI_LIB_PREBUILD_BIG %llu)", iid[0], iid[1], iid[2], iid[3], iid[4], iid[5],
           iid[6], iid[7], (unsigned long long)words,
           P.cache_control ? "not in the pipeline caches" : "too large to build without a cache probe (no pipelineCreationCacheControl)",
           (unsigned long long)LB.big_words);
}

// ---- large compute shaders on a background thread (NX2NAVI_LIB_PREBUILD_BIG_BG, default 1; 0 leaves them to their
// first dispatch as before)
// The boot prebuild leaves a compute shader over LB.big_words that no pipeline cache holds (lb_compute). Its first
// dispatch then compiled it on the replay thread: frames of 0.65-1.9 s in Kokiri Forest on an RTX 5090 after a
// translator change. One thread builds them instead, once the boot prebuild is done, one at a time and the smallest
// first: four compiled side by side grew the process by 16-19 GB on an RTX 4090 (comment at LB_BIG_WORDS). The shader
// being built is in cs_flight, so a dispatch that needs it waits for this build (vk_pipeline_compute raises the
// thread to normal priority then) instead of compiling a second copy. What is built is entered for the process, as
// lb_compute's pipelines are, and lands in pipeline-cache.bin for the next boot.

static int bg_on(void) {
    static int on = -1;
    if (on < 0) on = cfg_bool("NX2NAVI_LIB_PREBUILD_BIG_BG");
    return on;
}

// A large compute shader no cache holds goes to the background builder (lb_worker threads; under LB.stat).
static void bg_queue(const GfxShader *cs, const uint8_t iid[32], uint64_t words) {
    plat_mutex_lock(&LB.stat);
    if (BG.n == BG.cap) {
        size_t cap = BG.cap ? BG.cap * 2 : 16;
        const GfxShader **c = realloc(BG.cs, cap * sizeof *c);
        if (c) BG.cs = c;
        uint8_t (*i)[32] = realloc(BG.iid, cap * sizeof *i);
        if (i) BG.iid = i;
        uint64_t *w = realloc(BG.words, cap * sizeof *w);
        if (w) BG.words = w;
        if (c && i && w) BG.cap = cap;
    }
    int ok = BG.n < BG.cap;
    if (ok) BG.cs[BG.n] = cs, memcpy(BG.iid[BG.n], iid, 32), BG.words[BG.n] = words, BG.n++;
    plat_mutex_unlock(&LB.stat);
    if (!ok) {
        lb_defer(iid, words);
        return;
    }
    rt_log("vk_pipeline: stage library prebuild: compute shader %02x%02x%02x%02x%02x%02x%02x%02x (%llu SPIR-V words) not in "
           "the pipeline caches: built on the background thread after the prebuild (NX2NAVI_LIB_PREBUILD_BIG_BG)",
           iid[0], iid[1], iid[2], iid[3], iid[4], iid[5], iid[6], iid[7], (unsigned long long)words);
}

// NX2NAVI_BIG_BUILD_FREE_MB (default 8192; 0 no check): the system commit headroom a large compute build waits for.
// A build of one of these shaders took the process up by 1-3 GB on an RTX 5090 and four at once by 16-19 GB on an
// RTX 4090; started on a machine near its commit limit it can end the game. While the headroom is short the shader
// waits, and if the game dispatches it first it is built there, as without the background builder.
// The builds under way elsewhere count through their reservations at the memory gate (gate_enter), where this build
// then reserves GATE_BIG_MB of its own for as long as it runs, so the other background builds leave it its room.
static int bg_room(double *used, double *limit, double *held) {
    static double need = -1;
    if (need < 0) need = strtod(cfg_value("NX2NAVI_BIG_BUILD_FREE_MB"), NULL);
    double proc;
    plat_commit_info(&proc, used, limit);
    if (!(need <= 0 || *limit < 0 || *limit - *used >= need)) return 0;
    return gate_enter(GATE_BIG_MB, held);
}

// NX2NAVI_BIG_BUILD_MAX_MB (default 4096; 0 no cap): how far the builder may raise the process commit before it stops
// and leaves the rest to their first dispatch. One of these shaders costs the driver 1-4 GB: on a 31 GB machine four of
// them took the process from 15 to 29 GB and system commit to 60 of 65 GB, which is where a player feels the game
// stutter. The builder exists to remove hitches, not to trade them for paging, so a machine that cannot hold them all
// builds only what it can and pays one hitch per shader later.
static double bg_growth_cap(void) {
    static double cap = -1;
    if (cap < 0) cap = strtod(cfg_value("NX2NAVI_BIG_BUILD_MAX_MB"), NULL);
    return cap;
}

// The builder compiles into a pipeline cache of its own (own_cache at pipe_create_in), not into P.cache. A create into
// P.cache holds cache_rw shared from start to end, and the cache is only saved at a moment when nobody holds it: on an
// RX 7900 XT (26.10.1) the largest of these shaders (4,874,502 SPIR-V words) was still compiling 590 s after the boot,
// pipeline-cache.bin was not written once in that time, and the game was closed before the build ended, so the next
// boot found neither that shader nor the pipelines built in play and started the same build again. bg_merge moves the
// result into P.cache when the build is done, at a moment with no build into P.cache running: the lock is tried as
// cache_save tries it, because a waiting exclusive request would hold back every new build.
static void bg_merge(VkPipelineCache own, int built) {
    int locked = 0;
    while (built && !LB.stop && !(locked = plat_mutex_trylock(&cache_rw))) plat_sleep_ms(2);
    if (locked) {
        // what play compiled: into the play cache when there is one, so its file gets it and not pipeline-cache.bin
        VkResult r = vkMergePipelineCaches(P.device, P.play ? P.play : P.cache, 1, &own);
        plat_mutex_unlock(&cache_rw);
        if (r == VK_SUCCESS) __atomic_fetch_add(&cache_merges, 1, __ATOMIC_RELAXED);
        else rt_log("vk_pipeline: background compute build: its cache did not merge into %s (%d)",
                    P.play ? P.play_path : P.cache_path, (int)r);
    }
    vkDestroyPipelineCache(P.device, own, NULL);
}

static void bg_main(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    // The queue can grow while this thread builds (bg_queue runs on the replay thread and reallocs these arrays), so
    // the sort and every read below happen under LB.stat; an entry is copied out before its compile so no pointer into
    // the arrays is held across it.
    plat_mutex_lock(&LB.stat);
    for (size_t i = 1; i < BG.n; i++)   // smallest first: insertion sort of a handful
        for (size_t j = i; j > 0 && BG.words[j] < BG.words[j - 1]; j--) {
            const GfxShader *c = BG.cs[j];
            BG.cs[j] = BG.cs[j - 1], BG.cs[j - 1] = c;
            uint64_t w = BG.words[j];
            BG.words[j] = BG.words[j - 1], BG.words[j - 1] = w;
            uint8_t t[32];
            memcpy(t, BG.iid[j], 32), memcpy(BG.iid[j], BG.iid[j - 1], 32), memcpy(BG.iid[j - 1], t, 32);
        }
    plat_mutex_unlock(&LB.stat);
    double t_all = now_ms();
    double c_start, c_used0, c_limit0;
    plat_commit_info(&c_start, &c_used0, &c_limit0);
    for (size_t i = 0; !LB.stop; i++) {
        uint8_t idbuf[32];
        uint64_t words = 0;
        const GfxShader *cs;
        plat_mutex_lock(&LB.stat);
        int have = i < BG.n;
        if (have) {
            cs = BG.cs[i];
            memcpy(idbuf, BG.iid[i], 32);
            words = BG.words[i];
        }
        plat_mutex_unlock(&LB.stat);
        if (!have) break;
        const uint8_t *iid = idbuf;
        struct { const GfxShader *cs; uint64_t compute; } k = {cs, 1};   // no padding: the key is hashed and compared as bytes
        uint64_t h = key_hash(&k, sizeof k);
        Bindings B = {0};
        const char *why = NULL;
        double used, limit, c_now;
        plat_commit_info(&c_now, &used, &limit);
        double cap = bg_growth_cap();
        if (cap > 0 && c_now - c_start >= cap) {
            rt_log("vk_pipeline: background compute build: stopping, the process grew %.0f MB in all (NX2NAVI_BIG_BUILD_MAX_MB "
                   "%.0f); the shaders left build at their first dispatch", c_now - c_start, cap);
            break;
        }
        int waited = 0, no_room = 0;
        double w0 = now_ms(), held = -1;   // held: this build's reservation at the memory gate, -1 none
        while (!LB.stop && !bg_room(&used, &limit, &held)) {   // not under P.lock: a dispatch builds it meanwhile
            held = -1;
            if (!waited++)
                rt_log("vk_pipeline: background compute build: waiting for commit headroom (system %.0f of %.0f MB, "
                       "NX2NAVI_BIG_BUILD_FREE_MB %s)", used, limit, cfg_value("NX2NAVI_BIG_BUILD_FREE_MB"));
            if (now_ms() - w0 > 30000.0) {   // waiting is only useful when the headroom will come back
                rt_log("vk_pipeline: background compute build: no commit headroom after %.0f s; the shaders left build at "
                       "their first dispatch", (now_ms() - w0) / 1000.0);
                no_room = 1;
                break;
            }
            plat_sleep_ms(2000);
        }
        if (LB.stop || no_room) {
            if (held >= 0 && !no_room) gate_leave(held);   // stopped right after the gate let it in
            break;
        }
        plat_mutex_lock(&P.lock);
        int there = cs_flight_has(cs) || pipe_find(&k, sizeof k, h);   // a dispatch got to it first
        Layout *l = there ? NULL : compute_layout(cs, &B, &why);
        int flight = l ? cs_flight_add(cs, NULL) : 0;   // vk_pipeline_compute raises this thread through BG.raised
        if (l) BG.building = cs;
        plat_mutex_unlock(&P.lock);
        if (!l) {
            if (held >= 0) gate_leave(held);
            if (there) BG.there++;
            else BG.failed++, rt_log("vk_pipeline: background compute build: %s", why ? why : "no layout");
            continue;
        }
        VkPipelineCache own = VK_NULL_HANDLE;
        VkPipelineCacheCreateInfo oci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        if (P.cache && vkCreatePipelineCache(P.device, &oci, NULL, &own) != VK_SUCCESS) own = VK_NULL_HANDLE;
        double t0 = now_ms();
        own_cache = own;   // without one the build goes into P.cache as before
        VkPipeline pipe = compute_create(cs, l, 0, NULL);
        own_cache = VK_NULL_HANDLE;
        double ms = now_ms() - t0;
        if (held >= 0) gate_leave(held);
        // The pipeline stays: only a shader a run dispatched is queued here (lb_compute), and it is entered before
        // the merge below, so a dispatch that comes meanwhile finds it.
        plat_mutex_lock(&P.lock);
        if (flight) cs_flight_remove(cs);
        BG.building = NULL;
        Pipe *e = NULL;
        if (pipe && !pipe_find(&k, sizeof k, h) &&
            (e = pipe_add(&k, sizeof k, h, pipe, l, &B, VK_PIPELINE_BIND_POINT_COMPUTE, 0))) {
            e->pre = 1;
            P.stats.compute++;
        } else if (pipe) {   // a dispatch built its own meanwhile (no flight slot was free), or out of memory
            vkDestroyPipeline(P.device, pipe, NULL);
        }
        plat_mutex_unlock(&P.lock);
        if (own) bg_merge(own, pipe != VK_NULL_HANDLE);   // after the entry: a dispatch waiting for it does not wait for this
        if (plat_xchg32(&BG.raised, 0)) plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
        if (pipe) BG.built++, BG.ms += ms;
        else BG.failed++;
        double c_proc, c_used, c_limit;
        plat_commit_info(&c_proc, &c_used, &c_limit);
        rt_log("vk_pipeline: background compute build: %02x%02x%02x%02x%02x%02x%02x%02x (%llu SPIR-V words) %s in %.0f ms; "
               "commit: process %.0f MB, system %.0f of %.0f MB", iid[0], iid[1], iid[2], iid[3], iid[4], iid[5], iid[6],
               iid[7], (unsigned long long)words, pipe ? "built" : "failed", ms, c_proc, c_used, c_limit);
    }
    rt_log("vk_pipeline: background compute builds %s: %llu built (%.0f ms in all), %llu failed, %llu dispatched before "
           "their turn; %.0f ms after the prebuild", LB.stop ? "stopped" : "done", (unsigned long long)BG.built, BG.ms,
           (unsigned long long)BG.failed, (unsigned long long)BG.there, now_ms() - t_all);
}

// The last boot prebuild worker starts the builder when it queued anything.
static void bg_start(void) {
    if (!BG.n || LB.stop || BG.th) return;
    if (!(BG.th = plat_thread_new(bg_main, NULL, "big compute")))
        rt_log("vk_pipeline: cannot start the background compute builder; %zu large compute shaders build at their first "
               "dispatch", BG.n);
}

// keep 0: the pipeline is built for the pipeline cache only and destroyed at once, unless a dispatch came to wait for
// it meanwhile; a shader over LB.big_words is then not built at all (comment at compute-used.bin). cs may be a copy
// the caller releases afterwards when keep is 0: nothing here holds on to it then.
static void lb_compute(const GfxShader *cs, const uint8_t iid[32], int keep, int private_shader) {
    struct { const GfxShader *cs; uint64_t compute; } k = {cs, 1};   // no padding: the key is hashed and compared as bytes
    uint64_t h = key_hash(&k, sizeof k);
    Bindings B = {0};
    const char *why = NULL;
    uint64_t words = gfx_shader_words(cs);
    int big = LB.big_words && words > LB.big_words;   // only from the caches (LB_BIG_WORDS)
    if (big && !keep) {   // no run dispatched it: a pipeline of it would hold about 100 MiB of device memory unused
        plat_mutex_lock(&LB.stat);
        LB.big_unused++;
        plat_mutex_unlock(&LB.stat);
        return;
    }
    if (big && !P.cache_control) {
        if (bg_on()) bg_queue(cs, iid, words);
        else lb_defer(iid, words);
        return;
    }
    lb_lock();
    int busy = cs_flight_has(cs);   // the replay compiles it now (vk_pipeline_compute)
    Layout *l = busy || pipe_find(&k, sizeof k, h) ? NULL : compute_layout(cs, &B, &why);
    PlatThread *me = LB.idle && lb_me >= 0 ? LB.th[lb_me] : NULL;
    int have = !l && !why, flight = l ? cs_flight_add(cs, me) : 0;
    lb_unlock();
    if (!l) {
        lb_count(2, have ? 1 : 2, 0);
        return;
    }
    double t0 = now_ms();
    // The compile runs below normal priority: after an update or a driver update every one of these pipelines is
    // compiled again (about 140 ms each on an RTX 5090's driver, minutes of two threads for a long play history), and
    // at normal priority those threads took cores from the game's own on a CPU with few of them. While the shader is in
    // flight a dispatch of it waits for this build (vk_pipeline_compute) and raises the thread to normal: a compile left
    // at idle priority under load held that frame for seconds. The dispatch may have come between the unlock and the
    // call below, which would undo its raise, hence the look at the slot after it. Without a slot or a handle nobody
    // could raise the thread, so it compiles at normal priority.
    if (lb_idle() && flight && me) {
        plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
        if (cs_flight_is_wanted(cs)) plat_thread_priority(PLAT_PRIO_NORMAL);
    } else if (lb_idle()) {
        plat_thread_priority(PLAT_PRIO_NORMAL);
    }
    int reused = 0;
    // A private shader cannot have a dispatch waiting on its cs_flight slot. Shared/kept shaders always create their
    // handle so a dispatch that arrives while the build is in flight still receives the pipeline it is waiting for.
    VkPipeline pipe = compute_create(cs, l, big, !keep && private_shader ? &reused : NULL);
    double ms = now_ms() - t0;
    int compiled = pipe && cc_hit != 1, kept = 0;
    lb_lock();
    if (flight && cs_flight_remove(cs)) keep = 1;   // a dispatch waits for it
    Pipe *e = NULL;
    if (pipe && pipe_find(&k, sizeof k, h)) {
        vkDestroyPipeline(P.device, pipe, NULL);
        have = 1;
    } else if (pipe && keep && (e = pipe_add(&k, sizeof k, h, pipe, l, &B, VK_PIPELINE_BIND_POINT_COMPUTE, 0))) {
        e->pre = 1, kept = 1;
        P.stats.compute++;
    } else if (pipe) {
        vkDestroyPipeline(P.device, pipe, NULL);
        // P.stats.compute does not count it, so the autosave would not see what its compile added to P.cache
        if (compiled) __atomic_fetch_add(&cache_merges, 1, __ATOMIC_RELAXED);
    }
    lb_unlock();
    if (!pipe && big && bg_on()) bg_queue(cs, iid, words);
    else if (!pipe && big) lb_defer(iid, words);
    else lb_count(2, reused ? 1 : !pipe ? 2 : have ? 1 : kept ? 0 : 3, ms);
}

// The pre-rasterization (vertex) or fragment-shader library of one shader with the LB state.
// keep 0: build into the driver's pipeline cache only (sh is a private copy; the library is destroyed).
static void lb_library(const GfxShader *sh, int vertex, int keep) {
    int kind = vertex ? LIB_PR : LIB_FS, stat = vertex ? 0 : 1;
    VkPipeState s;
    vk_pipe_state_defaults(&s);
    GKey k;
    memset(&k, 0, sizeof k);
    k.clamp = LB.clamp, k.poly_mode = LB.poly, k.minus_w = LB.minus_w, k.samples = LB.samples;
    PrKey pk;
    FsKey fk;
    memset(&pk, 0, sizeof pk), memset(&fk, 0, sizeof fk);
    const void *key;
    uint32_t size;
    if (vertex) {
        GfxShaderIo fin[32];
        s.vs = sh;
        pk.kind = LIB_PR, pk.vs = sh, pk.clamp = k.clamp, pk.poly_mode = k.poly_mode, pk.minus_w = k.minus_w;
        pk.missing = vs_union_peek(sh, fin);
        for (int i = 0; i < 32; i++)
            if (pk.missing >> i & 1) pk.fin[i] = fin[i];
        key = &pk, size = sizeof pk;
    } else {
        s.fs = sh;
        fk.kind = LIB_FS, fk.fs = sh, fk.samples = k.samples;
        key = &fk, size = sizeof fk;
    }
    uint64_t h = key_hash(key, size);
    Bindings B = {0}, F = {0};
    VkPipelineLayout lay = VK_NULL_HANDLE;
    lb_lock();
    int have = keep && lib_find(h, key, size) != NULL;
    if (!have && add_stage(vertex ? &B : &F, sh, 0)) {
        Layout *l = layout_pair(&B, &F);
        if (l) lay = half_layout(l, vertex ? 0 : 1);
    }
    lb_unlock();
    if (have || !lay) {
        lb_count(stat, have ? 1 : 2, 0);
        return;
    }
    double t0 = now_ms();
    VkPipeline pipe = lib_create(kind, &s, &k, lay, key);
    double ms = now_ms() - t0;
    if (!keep) {
        if (pipe) vkDestroyPipeline(P.device, pipe, NULL);
        lb_count(stat, pipe ? 3 : 2, ms);
        return;
    }
    lb_lock();
    have = lib_find(h, key, size) != NULL;
    VkPipeline got = lib_put(h, key, size, pipe);
    if (!have && pipe) {
        Lib *b = lib_find(h, key, size);
        if (b) b->pre = 1;
    }
    lb_unlock();
    lb_count(stat, !pipe ? 2 : have || got != pipe ? 1 : 0, ms);
}

// A vertex or fragment variant with the graphics pipeline library off: no stage library to build ahead.
static void lb_skip_lib(void) {
    plat_mutex_lock(&LB.stat);
    LB.nolib++;
    plat_mutex_unlock(&LB.stat);
}

static void lb_report(const char *how) {
    float mem_used, mem_total, mem_proc;
    plat_mem_info(&mem_used, &mem_total, &mem_proc);
    plat_mutex_lock_shared(&LB.stat);
    rt_log("vk_pipeline: stage library prebuild %s: %llu of %zu variants in %.0f ms; kept: vertex %llu built (avg %.1f ms), "
           "%llu there; fragment %llu built (avg %.1f ms), %llu there; compute %llu built (avg %.1f ms), %llu there; driver "
           "cache only: vertex %llu (avg %.1f ms), fragment %llu (avg %.1f ms), compute %llu (avg %.1f ms); %llu failed, %llu not in the shader cache, "
           "%llu vertex/fragment skipped (graphics pipeline library off), %llu skipped (the driver crashed on them before, "
           "shader-prepare-skip.txt), %llu large compute shaders left to their first dispatch (NX2NAVI_LIB_PREBUILD_BIG), %zu "
           "queued for the background builder (NX2NAVI_LIB_PREBUILD_BIG_BG), %llu large compute shaders not built because "
           "no run has dispatched them; %llu compute requests the same as one built before, not built again "
           "(NX2NAVI_COMPUTE_WARM_REUSE); process working set %.0f MB", how, (unsigned long long)LB.done, LB.n,
           now_ms() - LB.t0, (unsigned long long)LB.built[0], LB.built[0] ? LB.ms[0] / LB.built[0] : 0.0,
           (unsigned long long)LB.have[0], (unsigned long long)LB.built[1], LB.built[1] ? LB.ms[1] / LB.built[1] : 0.0,
           (unsigned long long)LB.have[1], (unsigned long long)LB.built[2], LB.built[2] ? LB.ms[2] / LB.built[2] : 0.0,
           (unsigned long long)LB.have[2], (unsigned long long)LB.warmed[0], LB.warmed[0] ? LB.warm_ms[0] / LB.warmed[0] : 0.0,
           (unsigned long long)LB.warmed[1], LB.warmed[1] ? LB.warm_ms[1] / LB.warmed[1] : 0.0,
           (unsigned long long)LB.warmed[2], LB.warmed[2] ? LB.warm_ms[2] / LB.warmed[2] : 0.0,
           (unsigned long long)(LB.fail[0] + LB.fail[1] + LB.fail[2]),
           (unsigned long long)LB.noshader, (unsigned long long)LB.nolib, (unsigned long long)LB.skipped,
           (unsigned long long)LB.deferred, BG.n, (unsigned long long)LB.big_unused,
           (unsigned long long)__atomic_load_n(&compute_warm_reused, __ATOMIC_RELAXED), (double)mem_proc);
    plat_mutex_unlock_shared(&LB.stat);
}

// Boot: compute pipelines (the first LB.n_keep entered for the process, the others only built into the pipeline
// cache) and kept libraries.
static void lb_worker(void *arg) {
    lb_me = (int)(intptr_t)arg;        // its handle is LB.th[lb_me]
    if (lb_idle()) plat_thread_priority(PLAT_PRIO_IDLE);
    for (;;) {
        size_t i = lb_next(lb_me);
        if (i >= LB.n) break;
        if (skip_listed(LB.e[i].iid)) {
            skip_count();
            plat_mutex_lock(&LB.stat);
            LB.done++;
            plat_mutex_unlock(&LB.stat);
            continue;
        }
        // The memory gate (gate_wait): without room for another build the worker waits, and after GATE_WAIT_MS it and
        // the other workers end. What they leave is built at its first dispatch or draw, as what a run never recorded.
        double held;
        if (LB.short_mem || !gate_wait("stage library prebuild", &LB.stop, &held)) {
            if (!LB.stop && !plat_xchg32(&LB.short_mem, 1)) {
                char g[256];
                rt_log("vk_pipeline: stage library prebuild: ends here, at %zu of %zu variants: no commit headroom for "
                       "another build (%s). The others are built when the game first uses them", i, LB.n,
                       gate_text(g, sizeof g));
            }
            break;
        }
        // A compute shader whose pipeline does not stay is read as a copy of this thread's own and released after its
        // build: kept in the shader cache's memory, the 3,900 of an installed copy's list were 240 MB of the process's
        // commit for shaders most runs never dispatch. A dispatch that does come reads its shader then (0.05 to
        // 0.15 ms from the files this pass has just read). The shaders of kept pipelines and libraries stay loaded:
        // their pipeline and library entries point at them.
        int keep = i < LB.n_keep, owned = 0;
        plat_thread_priority(PLAT_PRIO_NORMAL);   // gfx_shader's cache lock, disk reads
        const GfxShader *sh = keep || LB.e[i].stage != GFX_STAGE_CS ? gfx_shader_load_input(LB.e[i].iid, 0)
                                                                    : gfx_shader_load_private(LB.e[i].iid, &owned);
        if (lb_idle()) plat_thread_priority(PLAT_PRIO_IDLE);
        if (!sh) {
            plat_mutex_lock(&LB.stat);
            LB.noshader++;
            plat_mutex_unlock(&LB.stat);
        } else if (gfx_shader_stage(sh) == GFX_STAGE_CS) {
            lb_compute(sh, LB.e[i].iid, keep, owned);
        } else if (!owned && (gfx_shader_stage(sh) == GFX_STAGE_VS || gfx_shader_stage(sh) == GFX_STAGE_FS)) {
            if (P.gpl) lb_library(sh, gfx_shader_stage(sh) == GFX_STAGE_VS, 1);   // a kept library points at sh: never a copy
            else lb_skip_lib();
        }
        if (owned) gfx_shader_release_private(sh);
        gate_leave(held);
        plat_mutex_lock(&LB.stat);
        LB.done++;
        plat_mutex_unlock(&LB.stat);
    }
    if (!plat_dec32(&LB.running)) {
        lb_report(LB.stop ? "stopped" : LB.short_mem ? "ended for want of commit headroom" : "done");
        fb_log("after the stage library prebuild");
        vk_mem_stage_log("the compute pipeline prebuild");
        bg_start();
    }
    return;
}

// The library state from the key file's records (prewarm); defaults without one.
static void lb_state(const uint32_t pr_hist[16], const uint32_t fs_hist[4]) {
    // state index: clamp | poly (0 fill, 1 line, 2 point -> VkPolygonMode) << 1 | minus_w << 3 (pr); samples log2 (fs)
    uint32_t best = 0, bf = 0;
    for (uint32_t i = 1; i < 16; i++) best = pr_hist[i] > pr_hist[best] ? i : best;
    for (uint32_t i = 1; i < 4; i++) bf = fs_hist[i] > fs_hist[bf] ? i : bf;
    if (!pr_hist[best]) best = 8;   // no key file: NX2navi clips -w..w (all 2706 records of a recorded key file)
    LB.clamp = (uint8_t)(best & 1), LB.poly = (uint8_t)(best >> 1 & 3), LB.minus_w = (uint8_t)(best >> 3 & 1);
    LB.samples = (uint8_t)(1u << bf);
}

// ---- the driver's cache of stage libraries, warmed again during play
//
// The preparation (lb_warm_prepared) builds every prepared variant's stage library once, so the driver's disk cache
// holds it and the library a draw needs later takes a few milliseconds instead of a compile: 3 ms against 100 ms for
// a vertex and 340 ms for a fragment library on an RX 7900 XT (driver 26.10.1). The driver's cache does not outlive
// everything, though: a driver update empties it, the AMD driver keeps a limited amount and drops old entries, and a
// change to what a library is built with (the dynamic states it lists, as when dynamic vertex input was turned off for
// this driver) gives every library another entry. After any of them each pipeline a draw first needs compiles cold:
// the draw is skipped for the frames that takes, or waits for it. Measured on that card with the cache cold, a walk
// through a village not seen before skipped 11,180 draws while their pipelines built and 94 draws waited 5.5 s in
// all, one of them 1.9 s.
//
// So every start checks: a sample of the prepared fragment libraries is built (lw_probe), and when their median time
// says the cache is cold, the whole prepared list is built again into the driver's cache on a few idle-priority
// threads while the game runs (lw_worker), as the preparation does but without stopping play. shader-warm.txt beside
// the caches says where that stands: "warming" while it runs, so a start after a run that ended meanwhile goes on,
// and "done" with the driver and a count of the times the cache was cold again right after a finished warm. At two in
// a row the driver does not keep stage libraries at all and warming stops being tried (NX2NAVI_LIB_WARM=0 turns it off;
// =force warms at every start).
// The check times builds, which a busy CPU only makes longer: as the guest starts beside the boot's background builds,
// a sample built at below-normal priority read a warm cache as cold on an RTX 5080 (0.9.2, a median of 136 ms on 16
// logical processors), and the whole list of 42,231 libraries was built again in 873 s on 4 idle threads, although
// --warm-shaders gave all of them back in 10.6 s and the driver's cache file did not change. So the sample is built
// at normal priority, into the driver's cache only (no_cache: a library our own pipeline cache holds would read fast
// whatever the driver has), and a sample that says cold is built again 10 s later before the cache counts as cold,
// except while the start waits for the whole warm after a driver change, where cold is the expected answer. The cache
// counts as warm when LW_WARM_SHARE of the sample was fast: a cache the driver has partly evicted (a size limit keeps
// some of the libraries and drops others) is to be warmed, and one or two fast builds say little.
// shader-warm.txt also keeps how far a warm came ("warming <driver> <strikes> <variant>"), written every 8,000 variants:
// a run that ends before the warm does is followed by one that goes on from there, not from the first variant again,
// which on a PC whose sessions were shorter than a warm under play (15 minutes) began the list again at every start.
#define LW_SAMPLE 12
#define LW_WARM_SHARE 0.75
#define LW_COLD_MS 15.0
#define LW_MAX_THREADS 6
#define LW_BOOT_THREADS 32   // while the start waits for the warm (bf_wait): every core, as the preparation
static struct {
    GfxWarmEnt *e;
    size_t n;
    volatile int64_t next, done;
    volatile int32_t running, stop, started;
    volatile int32_t short_mem;   // the memory gate refused a build for GATE_WAIT_MS: the warm ended for this run
    volatile int32_t checked;     // lw_main has returned: the workers it started, if any, are counted in running
    size_t from;                  // the variant this run's warm began at (an earlier run's progress, lw_check)
    PlatThread *th[LW_BOOT_THREADS + 1];
    int threads, strikes;
    int bg_threads;               // the workers that go on beside the game (lw_worker)
    uint32_t vendor;   // the GPU's vendor id, for what a cold cache's log line adds (vk_warm_hint.h)
    double t0;
    char stamp[PLAT_PATH_MAX * 2 + 32], id[64];
} LW;

// at: for "warming", the variant a later run goes on from (all before it are built).
static void lw_stamp_at(const char *state, int strikes, size_t at) {
    char line[160];
    int n = snprintf(line, sizeof line, "%s %s %d %zu\n", state, LW.id, strikes, at);
    if (!plat_file_write_atomic(LW.stamp, line, (size_t)n)) rt_log("vk_pipeline: cannot write %s", LW.stamp);
}

static void lw_stamp(const char *state, int strikes) { lw_stamp_at(state, strikes, 0); }

// One prepared vertex or fragment variant's library into the driver's cache; its time in ms, negative when the
// variant is on the skip list, not in the shader cache or of another stage.
static double lw_build(const GfxWarmEnt *e) {
    if (e->stage != GFX_STAGE_VS && e->stage != GFX_STAGE_FS) return -1;
    if (skip_listed(e->iid)) return -1;
    int owned = 0;
    const GfxShader *sh = gfx_shader_load_private(e->iid, &owned);
    if (!sh) return -1;
    double t0 = now_ms();
    int st = gfx_shader_stage(sh);
    if (st == GFX_STAGE_VS || st == GFX_STAGE_FS) lb_library(sh, st == GFX_STAGE_VS, 0);
    double ms = now_ms() - t0;
    if (owned) gfx_shader_release_private(sh);
    return ms;
}

static void lw_worker(void *arg) {
    int me = (int)(intptr_t)arg, idle = 0;   // me: its journal slot while the start waits (bf_wait)
    no_cache = 1;   // into the driver's own cache only (see no_cache)
    for (;;) {
        // Beside the game: idle priority, and of the workers the whole warm started only the background's count.
        if (!bf_waiting()) {
            if (me >= LW.bg_threads) break;
            if (!idle) plat_thread_priority(PLAT_PRIO_IDLE), idle = 1;
        }
        size_t i = (size_t)plat_inc64(&LW.next) - 1;
        if (i >= LW.n || LW.stop) break;
        // The memory gate (gate_wait): without room the warm waits, and after GATE_WAIT_MS it ends for this run.
        // shader-warm.txt keeps saying "warming", so the next start goes on with it.
        double held;
        if (LW.short_mem || !gate_wait("library warm", &LW.stop, &held)) {
            if (!LW.stop && !plat_xchg32(&LW.short_mem, 1)) {
                char g[256];
                rt_log("vk_pipeline: library warm: ends for this run at %lld of %zu prepared variants: no commit headroom "
                       "for another build (%s). The next start goes on with it", (long long)LW.done, LW.n,
                       gate_text(g, sizeof g));
            }
            break;
        }
        journal_mark(me, &LW.e[i]);   // from here to its clearing, a process that dies names this variant
        lw_build(&LW.e[i]);
        journal_mark(me, NULL);
        gate_leave(held);
        int64_t d = plat_inc64(&LW.done);
        if (d % 8000 == 0) {
            // the workers take the variants in order, each holds at most one unbuilt: all below this are built
            size_t at = LW.from + (size_t)(d > LW.threads ? d - LW.threads : 0);
            lw_stamp_at("warming", LW.strikes, at);
            rt_log("vk_pipeline: library warm: %lld of %zu prepared variants in %.0f s (up to variant %zu)", (long long)d,
                   LW.n - LW.from, (now_ms() - LW.t0) / 1000, at);
        }
    }
    if (!plat_dec32(&LW.running) && !LW.stop && !LW.short_mem) {
        lw_stamp("done", LW.strikes);
        rt_log("vk_pipeline: library warm: done, %zu prepared variants in %.0f s on %d threads; new pipelines "
               "link from the driver's cache from here on", LW.n, (now_ms() - LW.t0) / 1000, LW.threads);
    }
}

// The median time of LW_SAMPLE prepared fragment libraries spread over the list, in ms, and in *fast how many were
// under LW_COLD_MS, in *got how many were built; negative without enough of them.
static double lw_time_sample(int *fast, int *got_out) {
    double ms[LW_SAMPLE];
    int got = 0;
    size_t step = LW.n / LW_SAMPLE ? LW.n / LW_SAMPLE : 1, at = (size_t)(plat_now_ns() / 1000) % step;
    for (size_t i = at; i < LW.n && got < LW_SAMPLE && !LW.stop; i += step)
        for (size_t j = i; j < i + step && j < LW.n; j++) {   // the next fragment variant from here
            if (LW.e[j].stage != GFX_STAGE_FS) continue;
            double t = lw_build(&LW.e[j]);
            if (t >= 0) ms[got++] = t;
            break;
        }
    *fast = 0, *got_out = got;
    if (got < LW_SAMPLE / 2) return -1;
    for (int i = 0; i < got; i++) *fast += ms[i] <= LW_COLD_MS;
    for (int i = 1; i < got; i++)
        for (int j = i; j > 0 && ms[j] < ms[j - 1]; j--) {
            double t = ms[j];
            ms[j] = ms[j - 1], ms[j - 1] = t;
        }
    return ms[got / 2];
}

// 1 when the sample says the driver's cache has the prepared libraries.
static int lw_warm_sample(int fast, int got) {
    return got > 0 && fast >= (int)(got * LW_WARM_SHARE + 0.999);
}

// lw_time_sample at normal priority, and once more 10 s later when it says cold, unless the start waits (bf_waiting).
// The better of the two counts. The thread goes back to below normal afterwards when the game runs beside it.
static double lw_probe(int *fast, int *got) {
    plat_thread_priority(PLAT_PRIO_NORMAL);
    double t = lw_time_sample(fast, got);
    if (t >= 0 && !lw_warm_sample(*fast, *got) && !LW.stop && !bf_waiting()) {
        for (int i = 0; i < 100 && !LW.stop; i++) plat_sleep_ms(100);
        int f2 = 0, g2 = 0;
        double t2 = lw_time_sample(&f2, &g2);
        if (t2 >= 0 && lw_warm_sample(f2, g2) >= lw_warm_sample(*fast, *got) && f2 * (*got) > (*fast) * g2)
            t = t2, *fast = f2, *got = g2;
    }
    if (!bf_waiting()) plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    return t;
}

static void lw_check(void) {
    if (!bf_waiting()) plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    no_cache = 1;   // the sample's libraries go into the driver's cache only, as the workers' do (see no_cache)
    char was[16] = "", id[64] = "";
    int strikes = 0;
    unsigned long long at = 0;
    uint64_t size = 0;
    char *f = (char *)plat_file_read(LW.stamp, &size);
    if (f && size < 200) {
        char line[200];
        memcpy(line, f, size), line[size] = 0;
        if (sscanf(line, "%15s %63s %d %llu", was, id, &strikes, &at) < 2) was[0] = 0;   // no count: from the start
    }
    free(f);
    int same = !strcmp(id, LW.id), resume = same && !strcmp(was, "warming");
    const char *mode = cfg_value("NX2NAVI_LIB_WARM");
    // A resumed warm is not sampled: the variants the earlier run built are in the driver's cache and the rest are not,
    // so a sample would read the cache as warm or cold by how far it came. It goes on from where it stopped.
    int fast = 0, got = 0;
    double median = resume ? 0 : lw_probe(&fast, &got);
    if (LW.stop) return;
    int cold = (median >= 0 && !lw_warm_sample(fast, got)) || !strcmp(mode, "force");
    LW.from = resume && at < LW.n ? (size_t)at : 0;
    LW.next = (int64_t)LW.from;
    if (!resume && !cold) {
        if (same && strikes) lw_stamp("done", 0);   // the driver kept them this time
        rt_log("vk_pipeline: library warm: the driver's cache has the prepared libraries (%d of %d sampled built in "
               "under %.0f ms, median %.1f ms)", fast, got, LW_COLD_MS, median);
        return;
    }
    if (!resume && same && strikes >= 2 && strcmp(mode, "force")) {
        rt_log("vk_pipeline: library warm: the prepared libraries are cold again (%d of %d sampled fast, median %.0f "
               "ms) after two finished warms on this driver: it does not keep them, so pipelines compile when a draw "
               "first needs them (NX2NAVI_LIB_WARM=force warms anyway).%s", fast, got, median,
               lw_cold_hint(LW.vendor, LW_COLD_GAVE_UP, LW_ON_WINDOWS));
        return;
    }
    LW.strikes = resume ? strikes : same && !strcmp(was, "done") ? strikes + 1 : 0;
    lw_stamp_at("warming", LW.strikes, LW.from);
    int cpus = (int)plat_cpu_usable(), t = cpus / 4, boot = bf_waiting();
    t = t < 2 ? 2 : t > LW_MAX_THREADS ? LW_MAX_THREADS : t;
    LW.bg_threads = t;
    if (boot) t = cpus < 1 ? 1 : cpus > LW_BOOT_THREADS ? LW_BOOT_THREADS : cpus;
    if (boot) {
        char why[64];
        if (resume) snprintf(why, sizeof why, "an earlier run began the warm, which goes on from variant %zu", LW.from);
        else snprintf(why, sizeof why, "%d of %d sampled fast, median %.0f ms", fast, got, median);
        rt_log("vk_pipeline: library warm: the driver's cache does not have the prepared libraries (%s): building %zu "
               "prepared variants into it on %d threads while the start waits (the whole warm after a driver change)",
               why, LW.n - LW.from, t);
    } else if (resume)
        rt_log("vk_pipeline: library warm: going on with the warm an earlier run began, from variant %zu of %zu, on %d "
               "idle threads", LW.from, LW.n, t);
    else   // cold right after a finished warm on this driver says more than a first cold (vk_warm_hint.h)
        rt_log("vk_pipeline: library warm: the driver's cache does not have the prepared libraries (%d of %d sampled "
               "fast, median %.0f ms; a driver update or a change of what they are built with): building %zu prepared "
               "variants into it on %d idle threads while the game runs. Until it is done, a pipeline a draw first "
               "needs may compile, which skips that draw for some frames or makes it wait.%s", fast, got, median, LW.n,
               t,
               lw_cold_hint(LW.vendor,
                            lw_warm_sample(fast, got) ? LW_WARM_FORCED   // warm, but lib_warm = force
                            : same && !strcmp(was, "done") ? LW_COLD_AFTER_WARM : LW_COLD_FIRST,
                            LW_ON_WINDOWS));
    LW.t0 = now_ms();
    LW.running = t;
    int started = 0;
    for (int i = 0; i < t; i++)
        if ((LW.th[1 + started] = plat_thread_new(lw_worker, (void *)(intptr_t)i, "lib warm"))) started++;
    LW.threads = started;
    if (plat_add32(&LW.running, started - t) == 0 && !started) rt_log("vk_pipeline: library warm: no thread started");
}

static void lw_main(void *arg) {
    (void)arg;
    lw_check();
    plat_xchg32(&LW.checked, 1);
}

// Boot, once the library state is known (lb_state): the prepared list is read and the check starts on its own thread.
static void lw_start(void) {
    const char *mode = cfg_value("NX2NAVI_LIB_WARM");
    if (!P.gpl || !strcmp(mode, "0") || plat_xchg32(&LW.started, 1)) return;
    char dir[PLAT_PATH_MAX * 2], path[PLAT_PATH_MAX * 2 + 32];
    if (!cache_dir(dir, sizeof dir)) return;
    snprintf(path, sizeof path, "%s/shader-prepared.bin", dir);
    snprintf(LW.stamp, sizeof LW.stamp, "%s/shader-warm.txt", dir);
    size_t n = 0;
    uint8_t *f = gfx_shader_prepared_read(path, &n, NULL, NULL);   // the records alone, whichever version the file is
    LW.e = n ? calloc(n, sizeof *LW.e) : NULL;
    for (size_t i = 0; LW.e && i < n; i++)
        memcpy(LW.e[i].iid, f + PRP_REC * i + 4, 32), LW.e[i].stage = f[PRP_REC * i], LW.e[i].prio = f[PRP_REC * i + 1];
    free(f);
    if (!LW.e) return;   // nothing prepared (a build without the preparation step)
    LW.n = n;
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(vk_ctx()->phys, &pp);
    snprintf(LW.id, sizeof LW.id, "%04x:%04x:%08x", pp.vendorID, pp.deviceID, pp.driverVersion);
    LW.vendor = pp.vendorID;
    LW.th[0] = plat_thread_new(lw_main, NULL, "lib warm check");
}

static void lw_stop(void) {
    plat_xchg32(&LW.stop, 1);
    for (int i = 0; i <= LW_BOOT_THREADS; i++)
        if (LW.th[i]) plat_thread_join(LW.th[i]), LW.th[i] = NULL;
}

// Boot: compute shaders of the warm list (recorded and prepared), those whose pipeline stays first, then the first
// KEEP recorded graphics variants.
static void lb_start(const uint32_t pr_hist[16], const uint32_t fs_hist[4]) {
    const char *e = cfg_value("NX2NAVI_LIB_PREBUILD"), *ke = cfg_get("NX2NAVI_LIB_PREBUILD_KEEP");
    const char *te = cfg_get("NX2NAVI_LIB_PREBUILD_THREADS");   // no fixed default: 2 when unset (1 under 4 CPUs)
    const char *be = cfg_get("NX2NAVI_LIB_PREBUILD_BIG");       // SPIR-V words; LB_BIG_WORDS when unset, 0 no limit
    if (!A.on || !cfg_bool("NX2NAVI_LIB_PREBUILD")) return;
    mesh_pre_start();             // the recorded mesh pipelines' libraries
    lb_state(pr_hist, fs_hist);   // the libraries' state, also for lw_start
    if (BF.on) BF.lw = 1;         // after the compute pipelines, with the start waiting (bf_wait)
    else lw_start();
    LB.big_words = be && *be ? strtoull(be, NULL, 10) : LB_BIG_WORDS;
    char dir[PLAT_PATH_MAX * 2];
    if (cache_dir(dir, sizeof dir)) skip_init(dir);
    GfxWarmEnt *all = NULL;
    size_t total = gfx_shader_warm_list(&all), n = 0, n_cs = 0;
    LB.keep_n = ke && *ke ? atoi(ke) : 0;
    LB.e = malloc((total + 1) * sizeof *LB.e);
    // the compute shaders whose pipeline stays come first: they are the ones the game is about to dispatch
    uint8_t *kf = total ? malloc(total) : NULL;
    for (size_t i = 0; LB.e && kf && i < total; i++)
        if (all[i].stage == GFX_STAGE_CS && (kf[i] = (uint8_t)cu_keep(all[i].iid))) LB.e[n++] = all[i];
    LB.n_keep = n;
    for (size_t i = 0; LB.e && i < total; i++)
        if (all[i].stage == GFX_STAGE_CS && !(kf && kf[i])) LB.e[n++] = all[i];
    free(kf);
    n_cs = n;
    for (size_t i = 0; LB.e && i < total && (int32_t)(n - n_cs) < LB.keep_n; i++)
        if (all[i].stage != GFX_STAGE_CS && all[i].prio == 0) LB.e[n++] = all[i];
    free(all);
    if (atoi(e) > 1 && (size_t)atoi(e) < n) n = (size_t)atoi(e);   // a count above 1 caps the list; 1 is plain on
    if (LB.n_keep > n) LB.n_keep = n;
    if (!n) {
        free(LB.e), LB.e = NULL;
        return;
    }
    LB.n = n;
    LB.idle = 1;
    int cpus = (int)plat_cpu_usable();
    // Two threads by default, not half the CPUs. On a 32-thread CPU with NVIDIA's driver, 16 builders kept the driver's
    // lock busy: 77% of the process's running time was a spin loop in nvoglv64.dll (about 8 cores), the prebuild had not
    // finished after 34,000 frames, and lib_prebuild = 0 took it to 5% and the game to its frame limit (RTX 4080 SUPER,
    // driver 617.14, 2026-11-08). With the driver's disk cache warm (--prepare-shaders, --warm-shaders) a build takes
    // well under a millisecond, so two threads still finish the list quickly.
    // The two take the shaders dispatched most recently (e[0..n_keep)); one goes on with the rest (lb_next), which is
    // only built into the caches. Where the rest still runs during play, two builders there cost frames: one against
    // two for the whole list, one 20 s route each on an RTX 5090, Castle Town 76.1 against 73.7 fps with 1 frame over
    // 25 ms against 6, Hyrule Field 85.7 against 80.7 fps. NX2NAVI_LIB_PREBUILD_THREADS keeps its count for the whole
    // list.
    int t = te && *te ? atoi(te) : (cpus < 4 ? 1 : 2);   // idle priority; a cold driver takes ~2 s per large compute shader
    LB.tail_one = !(te && *te);
    t = t < 1 ? 1 : t > LB_MAX_THREADS ? LB_MAX_THREADS : t;
    // The whole warm (bf_wait): no game runs beside these builds until they are done, so every core takes them, as
    // in the preparation; should the game start first, the count above goes on (lb_next).
    LB.bg_threads = t;
    if (bf_waiting() && LB.tail_one) t = cpus < 1 ? 1 : cpus > LB_MAX_THREADS ? LB_MAX_THREADS : cpus;
    if ((size_t)t > n) t = (int)n;
    LB.t0 = now_ms();
    LB.running = t;
    int started = 0;
    for (int i = 0; i < t; i++)
        if ((LB.th[started] = plat_thread_new(lb_worker, (void *)(intptr_t)started, "lib prebuild"))) started++;
    plat_add32(&LB.running, started - t);
    LB.threads = started;
    char big[128];
    if (!LB.big_words) snprintf(big, sizeof big, "compute shaders of every size");
    else snprintf(big, sizeof big, "compute shaders over %llu SPIR-V words %s", (unsigned long long)LB.big_words,
                  P.cache_control ? "only from the pipeline caches" : "left to their first dispatch");
    rt_log("vk_pipeline: stage library prebuild: %zu compute pipelines, of which %zu stay (the shaders dispatched most "
           "recently, NX2NAVI_COMPUTE_KEEP) and the others are built into the pipeline cache and destroyed, to be created "
           "at their first dispatch; %zu recorded variants' libraries (NX2NAVI_LIB_PREBUILD_KEEP %d); on %d %s "
           "threads; library state clamp %u, polygon mode %u, depth -w..w %u, samples %u; %s (NX2NAVI_LIB_PREBUILD_BIG)",
           n_cs < n ? n_cs : n, LB.n_keep, n > n_cs ? n - n_cs : 0, LB.keep_n, started,
           bf_waiting() ? "normal-priority (the start waits for them)" : "idle-priority", LB.clamp, LB.poly, LB.minus_w,
           LB.samples, big);
    if (LB.tail_one && started > 1 && !bf_waiting())
        rt_log("vk_pipeline: stage library prebuild: after the %zu recent compute pipelines, one worker continues the "
               "remaining list; NX2NAVI_LIB_PREBUILD_THREADS overrides this background limit", LB.n_keep);
}

static void lb_stop(void) {
    lw_stop();
    plat_xchg32(&MP.stop, 1);
    if (MP.th) plat_thread_join(MP.th), MP.th = NULL;
    plat_xchg32(&LB.stop, 1);
    for (int i = 0; i < LB.threads; i++)
        if (LB.th[i]) plat_thread_join(LB.th[i]), LB.th[i] = NULL;
    LB.threads = 0;
    if (BG.th) plat_thread_join(BG.th), BG.th = NULL;   // after its current build, if one runs
}

// 1 once the whole warm's time on the boot screen is up (BF.t_end): the builders go on as on any other start.
static int bf_time_up(void) {
    if (!BF.t_end || !bf_waiting() || now_ms() < BF.t_end) return 0;
    vk_pipeline_prewarm_detach();
    return 1;
}

// The whole warm's wait (comment at bf_decide), on the prewarm thread after the recorded pipelines: the compute
// pipelines lb_start has just started, then the library warm. The counts go to the boot screen
// (vk_pipeline_prewarm_progress). It ends with the warm, at the warm's time limit, or when the boot stops waiting
// (vk_pipeline_prewarm_detach); in the last two the builders go on beside the game.
static void bf_wait(void) {
    double t0 = now_ms();
    int up = bf_time_up();   // the recorded pipelines took all of it
    if (LB.n && LB.threads) {
        __atomic_store_n(&pw_pipe_done, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&pw_pipe_total, (int64_t)LB.n, __ATOMIC_RELAXED);
        __atomic_store_n(&pw_phase, 3, __ATOMIC_RELEASE);
        while (__atomic_load_n(&LB.running, __ATOMIC_ACQUIRE) > 0 && !LB.stop && bf_waiting()) {
            plat_sleep_ms(100);
            plat_mutex_lock_shared(&LB.stat);
            uint64_t d = LB.done;
            plat_mutex_unlock_shared(&LB.stat);
            __atomic_store_n(&pw_pipe_done, (int64_t)d, __ATOMIC_RELAXED);
            up |= bf_time_up();
        }
    }
    double t1 = now_ms();
    int libs = 0;
    if (BF.lw && !LB.stop) {
        if (bf_waiting()) journal_open();
        lw_start();   // beside the game when the wait is over: the check and the warm as on any other start
        if (LW.th[0]) {
            libs = 1;
            __atomic_store_n(&pw_pipe_done, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&pw_pipe_total, (int64_t)LW.n, __ATOMIC_RELAXED);
            __atomic_store_n(&pw_phase, 4, __ATOMIC_RELEASE);
            while ((!LW.checked || LW.running > 0) && !LW.stop && bf_waiting()) {
                plat_sleep_ms(100);
                __atomic_store_n(&pw_pipe_done, (int64_t)LW.done, __ATOMIC_RELAXED);
                __atomic_store_n(&pw_pipe_total, (int64_t)(LW.n - LW.from), __ATOMIC_RELAXED);   // a resumed warm's rest
                up |= bf_time_up();
            }
        }
        journal_close();   // the warm is over, or the game runs
    }
    int stopped = LB.stop || LW.stop;
    int over = __atomic_load_n(&LB.running, __ATOMIC_ACQUIRE) <= 0 && (!libs || (LW.checked && LW.running <= 0));
    rt_log("vk_pipeline: the whole warm %s: the compute pipelines (%zu) took %.0f ms of the wait, the driver's cache of "
           "stage libraries %.0f ms (%s; %lld of %zu prepared variants built so far)",
           stopped ? "stopped with the process" : over ? "is done"
           : up ? "goes on beside the game: its time on the boot screen is up (NX2NAVI_PREWARM_FULL_S)"
                : "goes on beside the game: the boot stopped waiting", LB.n, t1 - t0, now_ms() - t1,
           !libs ? "not warmed: no prepared list, or the graphics pipeline library or NX2NAVI_LIB_WARM is off"
                 : LW.done ? "being warmed" : "left as it is so far: the check found it warm, gave the warm up, or still runs",
           (long long)LW.done, LW.n);
    if (!stopped) {   // the boot screen's turn is over for this GPU and driver (boot_warm.h)
        char key[32], line[64];
        gpu_key(key, sizeof key);
        int len = snprintf(line, sizeof line, "%s done\n", key);
        if (!plat_file_write_atomic(BF.owed, line, (size_t)len)) rt_log("vk_pipeline: cannot write %s", BF.owed);
    }
    vk_pipeline_prewarm_detach();   // nothing waits from here on: later builders are the background's
}

// Preparation: one prepared variant at a time into the driver's disk cache (private shader copy, nothing kept). arg is
// the worker's journal slot.
static void lb_warm_worker(void *arg) {
    int slot = (int)(intptr_t)arg;
    for (;;) {
        size_t i = (size_t)plat_inc64(&LB.next) - 1;
        if (i >= LB.n) return;
        int owned = 0, skip = skip_listed(LB.e[i].iid);
        const GfxShader *sh = skip ? NULL : gfx_shader_load_private(LB.e[i].iid, &owned);
        int st = sh ? gfx_shader_stage(sh) : -1;
        if (sh) journal_mark(slot, &LB.e[i]);   // from here to its clearing below, a crash names this variant
        if (skip) {
            skip_count();
        } else if (!sh) {
            plat_mutex_lock(&LB.stat);
            LB.noshader++;
            plat_mutex_unlock(&LB.stat);
        } else if (st == GFX_STAGE_CS) {
            Bindings B = {0};
            const char *why = NULL;
            lb_lock();
            Layout *l = compute_layout(sh, &B, &why);
            lb_unlock();
            double t0 = now_ms();
            int reused = 0;
            VkPipeline pipe = l ? compute_create(sh, l, 0, &reused) : VK_NULL_HANDLE;
            if (pipe) vkDestroyPipeline(P.device, pipe, NULL);
            lb_count(2, reused ? 1 : pipe ? 3 : 2, now_ms() - t0);
        } else if (st == GFX_STAGE_VS || st == GFX_STAGE_FS) {
            if (P.gpl) lb_library(sh, st == GFX_STAGE_VS, 0);
            else lb_skip_lib();
        }
        if (sh) journal_mark(slot, NULL);
        if (owned) gfx_shader_release_private(sh);
        plat_mutex_lock(&LB.stat);
        uint64_t d = ++LB.done;
        plat_mutex_unlock(&LB.stat);
        if (d % 4000 == 0) lb_report("preparation progress");
    }
}

// --warm-shaders, before the whole list: LW_SAMPLE prepared fragment libraries spread over it, built into the driver's
// own cache alone (no_cache, as the library warm in play builds) and timed, as the game's check at every start does
// (lw_check). 1 when at least LW_WARM_SHARE of them came from the driver's cache. *fast and *got: the counts.
static int prep_sample_warm(int *fast, int *got) {
    double ms[LW_SAMPLE];
    *fast = *got = 0;
    size_t step = LB.n / LW_SAMPLE ? LB.n / LW_SAMPLE : 1;
    int was = no_cache;
    no_cache = 1;
    for (size_t i = step / 2; i < LB.n && *got < LW_SAMPLE; i += step)
        for (size_t j = i; j < i + step && j < LB.n; j++) {   // the next fragment variant from here
            if (LB.e[j].stage != GFX_STAGE_FS) continue;
            journal_mark(0, &LB.e[j]);   // a driver that dies on it names it, as the full pass would
            double t = lw_build(&LB.e[j]);
            journal_mark(0, NULL);
            if (t >= 0) ms[(*got)++] = t;
            break;
        }
    no_cache = was;
    for (int i = 0; i < *got; i++) *fast += ms[i] <= LW_COLD_MS;
    return *got >= LW_SAMPLE / 2 && lw_warm_sample(*fast, *got);
}

// Preparation: every variant of <dir>/shader-prepared.bin on every core at normal priority. sample: --warm-shaders
// after an update, which first asks the driver's cache (prep_sample_warm) and builds nothing more when it has them.
// It took 10 to 20 s on a player's PC at every update, all of it 42,000 libraries the driver gave back at once, and
// it still takes the minutes of a full build when the driver no longer has them (a driver update, or a change of what
// they are built with: 380 s on that PC once). NX2NAVI_LIB_WARM=force builds the whole list whatever the sample says.
static void lb_warm_prepared(const char *dir, int sample) {
    char path[PLAT_PATH_MAX * 2 + 32];
    snprintf(path, sizeof path, "%s/shader-prepared.bin", dir);
    size_t n = 0;
    uint8_t *f = gfx_shader_prepared_read(path, &n, NULL, NULL);   // the records alone, whichever version the file is
    LB.e = n ? calloc(n, sizeof *LB.e) : NULL;
    for (size_t i = 0; LB.e && i < n; i++)
        memcpy(LB.e[i].iid, f + PRP_REC * i + 4, 32), LB.e[i].stage = f[PRP_REC * i], LB.e[i].prio = f[PRP_REC * i + 1];
    free(f);
    if (!LB.e) return;
    skip_init(dir);
    char jp[PLAT_PATH_MAX * 2 + 48];
    snprintf(jp, sizeof jp, "%s/shader-prepare-journal.bin", dir);
    SK.journal = plat_fs_create(jp, NULL);   // skip_init removed a leftover one
    if (!SK.journal) rt_log("vk_pipeline: cannot create %s: a driver crash in this run will not be recorded", jp);
    LB.n = n, LB.idle = 0, LB.t0 = now_ms();
    static const uint32_t no_pr[16], no_fs[4];
    lb_state(no_pr, no_fs);
    int fast = 0, got = 0;
    if (sample && P.gpl && strcmp(cfg_value("NX2NAVI_LIB_WARM"), "force") && prep_sample_warm(&fast, &got)) {
        if (SK.journal) plat_file_close(SK.journal), SK.journal = NULL, plat_file_delete(jp);
        printf("prepare-shaders: progress build %zu %zu\n", n, n);
        printf("prepare-shaders: the graphics driver's cache still has the prepared shaders (%d of %d sampled came from "
               "it in under %.0f ms each): the %zu variants are not built again (%.0f ms)\n", fast, got, LW_COLD_MS, n,
               now_ms() - LB.t0);
        fflush(stdout);
        free(LB.e), LB.e = NULL, LB.n = 0, LB.next = 0, LB.done = 0, LB.skipped = 0;
        return;
    }
    if (sample && got)
        printf("prepare-shaders: the graphics driver's cache does not have the prepared shaders (%d of %d sampled came "
               "from it): all %zu variants are built into it again\n", fast, got, n);
    int cpus = (int)plat_cpu_usable(), k = 0;
    PlatThread *t[JR_SLOTS];
    for (int i = 0; i < cpus && i < JR_SLOTS; i++)
        if ((t[k] = plat_thread_new(lb_warm_worker, (void *)(intptr_t)k, "lib warm"))) k++;
    if (!k) lb_warm_worker((void *)(intptr_t)0);
    // every 2 s a count on stdout for tools/make_game.py (step_shaders), as gfx_shader.c prepare_files prints for the
    // translation: a driver building for a long time still shows the step moving
    for (int i = 0; i < k; i++)
        while (!plat_thread_join_ns(t[i], 2000000000u)) {
            plat_mutex_lock(&LB.stat);
            uint64_t d = LB.done;
            plat_mutex_unlock(&LB.stat);
            printf("prepare-shaders: progress build %llu %zu\n", (unsigned long long)d, n);
            fflush(stdout);
        }
    printf("prepare-shaders: progress build %zu %zu\n", n, n);
    fflush(stdout);
    if (SK.journal) plat_file_close(SK.journal), SK.journal = NULL, plat_file_delete(jp);   // every build returned
    lb_report("preparation done");
    // with gpl off only compute variants are built: say so, rather than count the skipped ones as built
    unsigned long long skipped = LB.skipped;
    size_t tried = n - (size_t)skipped;
    if (LB.nolib)
        printf("prepare-shaders: %zu variants on %d threads in %.0f ms, %llu vertex/fragment variants skipped (graphics "
               "pipeline library off, they build at their first draw)\n", tried, k, now_ms() - LB.t0,
               (unsigned long long)LB.nolib);
    else
        printf("prepare-shaders: %zu variants built into the driver cache on %d threads in %.0f ms\n", tried, k,
               now_ms() - LB.t0);
    if (skipped)   // installer/steps.py Prepare reads this line
        printf("prepare-shaders: %llu variants skipped: the graphics driver crashed while building them in an earlier "
               "run (shader-prepare-skip.txt); they build at their first draw\n", skipped);
    free(LB.e), LB.e = NULL, LB.n = 0, LB.next = 0, LB.done = 0, LB.skipped = 0;
}

// ---- the shared pipeline list (tools/pipeline_list.py; seed-pipeline-keys.bin next to the game)
// tools/prepare_shaders.py writes the list's key records there, in the layout of pipeline-keys.bin, after putting the
// shader variants they name into the seed. The preparation builds them once, after the seed is translated: each
// pipeline goes through the pipeline cache and so into pipeline-cache.bin and the driver's own disk cache, and is
// dropped with the process. Nothing is added to this folder's pipeline-keys.bin; every boot then counts the list as
// recorded pipelines beside the install's own (keys_open_seed) and builds them before play, from those caches.
// NX2NAVI_PREPARE_PIPELINES: all (default) builds every record; mesh, the list's mesh pipelines only (and every record
// without the graphics pipeline library); 0, none. A vertex pipeline's module depends on the vertex output union of
// the pairs known (vs_outs): here the list's, as at the first boot of a new install, which reads the list and none of
// its own yet, so a vertex pipeline built here is the one that boot asks for. An install that has drawn more pairs
// since may need some of them again with wider outputs; the boot builds those. Without the graphics pipeline library
// (vk_caps_gpl: AMD's Windows driver for RDNA 2 and older) a vertex pipeline's outputs come from its own pair (no
// independent layouts), so a listed one always is the one a draw asks for. A mesh pipeline, which has no stage
// libraries, otherwise leaves its draws out of the picture for its whole compile (0.2 to 1.1 s each on a first visit
// of a forest, 18 of them). Where mesh pipelines have stage libraries (mesh_libs) the listed ones are still built
// whole, and the libraries of their stages are built after them (mesh_pre_run), for the mesh pipelines no list names.
// A driver whose compiler crashes on one of these would crash at every preparation: seed-pipelines-running.txt marks
// the step, and a run that finds the marker of a run that died skips the step for good on this folder (the pipelines
// are then built at their first draw, as without a list) until the marker is deleted.
static int cache_save(uint32_t wait_ms, int exiting);

static int seed_pipelines(const char *dir) {
    const char *mode = cfg_value("NX2NAVI_PREPARE_PIPELINES");
    int all = !strcmp(mode, "all") || (!strcmp(mode, "mesh") && !P.gpl);
    char path[PLAT_PATH_MAX * 2 + 48], mark[PLAT_PATH_MAX * 2 + 48];
    snprintf(path, sizeof path, "%s/seed-pipeline-keys.bin", dir);
    snprintf(mark, sizeof mark, "%s/seed-pipelines-running.txt", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;   // no list: nothing to say
    if (!strcmp(mode, "0")) {
        fclose(f);
        printf("prepare-pipelines: off (NX2NAVI_PREPARE_PIPELINES=0)\n");
        return 0;
    }
    if (plat_file_exists(mark)) {
        fclose(f);
        printf("prepare-pipelines: skipped: an earlier preparation ended while it built the listed pipelines (%s); they "
               "are built at their first draw\n", mark);
        return 0;
    }
    KeysHeader h;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t n = 0;
    KeyRec *r = NULL;
    if (len >= (long)sizeof h && fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, "NX2NPKEY", 8) &&
        h.version == KEYS_VERSION && h.rec_size == sizeof(KeyRec)) {
        n = ((size_t)len - sizeof h) / sizeof(KeyRec);
        r = malloc(n * sizeof *r + 1);
        if (r && fread(r, sizeof *r, n, f) != n) n = 0;
    } else {
        printf("prepare-pipelines: %s is a list for another version of the game's runtime (its record layout differs); "
               "no pipeline is built from it\n", path);
    }
    fclose(f);
    uint8_t (*ids)[32] = n ? malloc(3 * n * 32) : NULL;
    const GfxShader **sh = n ? calloc(3 * n, sizeof *sh) : NULL;
    if (!r || !ids || !sh) {
        free(r), free(ids), free(sh);
        return 0;
    }
    double t0 = now_ms();
    size_t m = 0, u = 0;
    for (size_t i = 0; i < n; i++) {
        memcpy(ids[m++], r[i].vs, 32);
        if (r[i].has_fs) memcpy(ids[m++], r[i].fs, 32);
        if (r[i].has_ts) memcpy(ids[m++], r[i].ts, 32);
    }
    qsort(ids, m, 32, id_cmp);
    for (size_t i = 0; i < m; i++)
        if (!u || memcmp(ids[u - 1], ids[i], 32)) memcpy(ids[u++], ids[i], 32);
    int cpus = (int)plat_cpu_usable();
    const char *pe = cfg_get("NX2NAVI_PREWARM_XLATE");
    int procs = pe && *pe ? atoi(pe) : cpus / 2 > 16 ? 16 : cpus / 2;
    gfx_shader_prewarm((const uint8_t(*)[32])ids, u, sh, cpus, procs < 1 ? 1 : procs);
    static const char note[] = "the listed pipelines are being built; a run that finds this file skips them\n";
    plat_file_write_atomic(mark, note, sizeof note - 1);
    uint32_t queued = 0, dropped = 0, known = 0, other = 0, mesh = 0, no_mesh = 0;
    plat_mutex_lock(&P.lock);
    uint32_t built0 = K.built, failed0 = K.failed;
    K.t0 = t0, K.prep = 1;   // prebuild_report's start: this mode never runs prewarm_run, which sets it in play
    K.loading = 1;
    for (size_t i = 0; i < n; i++) {
        const uint8_t (*v)[32] = bsearch(r[i].vs, ids, u, 32, id_cmp);
        const uint8_t (*fi)[32] = r[i].has_fs ? bsearch(r[i].fs, ids, u, 32, id_cmp) : NULL;
        const uint8_t (*t)[32] = r[i].has_ts ? bsearch(r[i].ts, ids, u, 32, id_cmp) : NULL;
        const GfxShader *vs = v ? sh[v - ids] : NULL, *fs = fi ? sh[fi - ids] : NULL, *ts = t ? sh[t - ids] : NULL;
        if (!vs || (r[i].has_fs && !fs) || (r[i].has_ts && !ts)) {
            dropped++;
            continue;
        }
        VkPipeState s = r[i].s;
        s.vs = vs, s.fs = fs, s.ts = ts;
        if (!is_mesh(&s) && !all) {
            other++;
            continue;
        }
        if (is_mesh(&s) && (!vk_ctx()->has_mesh_shader || vk_ctx()->mesh_fallback)) {   // no mesh draw uses one
            no_mesh++;
            continue;
        }
        if (!is_mesh(&s) && P.indep && fs && gfx_shader_stage(vs) == GFX_STAGE_VS) {   // its vertex output union, as the boot
            GfxShaderIo io[32];
            vs_outs(vs, fs, io);
        }
        GKey k;
        gkey_fill(&s, &k);
        if (is_mesh(&s) && mesh_libs()) mesh_pre_note(&s, &k);
        uint64_t hk = key_hash(&k, sizeof k);
        int q = 0;
        if (pipe_find(&k, sizeof k, hk)) known++;
        else pipe_miss(&s, &k, hk, 2, &q), queued += (uint32_t)q, mesh += q && is_mesh(&s);
    }
    K.loading = 0;
    K.until = 0;   // every one of them: no boot waits for this
    GATE.off = 1;  // and no game runs beside it: the memory gate (gate_enter) refuses nothing
    plat_mutex_unlock(&P.lock);
    free(ids), free(sh), free(r);
    PlatThread *th[64];
    int nt = 0;
    if (queued) warm_builder((void *)1);   // the first alone (warm_builder)
    for (int i = 0; i < cpus && i < 64 && queued; i++)
        if ((th[nt] = plat_thread_new(warm_builder, NULL, "pipe list"))) nt++;
    if (!nt && queued) warm_builder(NULL);
    for (int i = 0; i < nt; i++)
        while (!plat_thread_join_ns(th[i], 2000000000u)) {
            printf("prepare-pipelines: progress %u %u\n",
                   __atomic_load_n(&K.built, __ATOMIC_RELAXED) + __atomic_load_n(&K.failed, __ATOMIC_RELAXED) - built0 - failed0,
                   queued);
            fflush(stdout);
        }
    plat_mutex_lock(&P.lock);
    while (K.left) plat_cond_wait(&done_cv, &P.lock);   // those a pool worker took
    uint32_t built = K.built - built0, failed = K.failed - failed0;
    plat_mutex_unlock(&P.lock);
    // The listed mesh pipelines' stage libraries, before the save and inside the marker: the file then holds them,
    // and a driver that dies on one is met by the next preparation as one that died on a listed pipeline.
    mesh_pre_run(cpus);
    if (MP.n) {
        char ml[320];
        snprintf(ml, sizeof ml, "prepare-pipelines: %u stage libraries of the listed mesh pipelines (%u of task and mesh "
                 "stages, %u fragment shader): %llu built (avg %.1f ms, longest %.0f ms), %llu failed; %.0f ms", MP.n,
                 MP.n_pr, MP.n - MP.n_pr, (unsigned long long)MP.built, MP.built ? MP.ms / (double)MP.built : 0.0, MP.max,
                 (unsigned long long)MP.fail, now_ms() - MP.t0);
        printf("%s\n", ml);
        fflush(stdout);
        rt_log("vk_pipeline: %s", ml);
    }
    int saved = cache_save(5000, 0) > 0;   // explicit preparation waits for a moment with no build running
    plat_file_delete(mark);
    char msg[640];
    snprintf(msg, sizeof msg, "prepare-pipelines: %zu listed pipelines: %u built (%u mesh), %u failed, %u the same as one "
             "built before, %u without a shader this game version or its shader cache has, %u vertex pipelines left to "
             "their first draw (NX2NAVI_PREPARE_PIPELINES=%s), %u mesh pipelines this GPU cannot draw; on %d threads in "
             "%.0f ms; pipeline cache %s", n, built, mesh, failed, known, dropped, other,
             !strcmp(mode, "all") ? "all" : all ? "mesh, every record without the graphics pipeline library" : "mesh", no_mesh,
             nt ? nt : 1, now_ms() - t0,
             saved ? "saved" : "not saved (builds kept running, or its file could not be written)");
    printf("%s\n", msg);
    fflush(stdout);
    rt_log("vk_pipeline: %s", msg);
    fb_log("after the listed pipelines");
    return 0;
}

// nx2navi.exe --prepare-pipelines: the list's pipelines alone, for a folder whose shaders are prepared already.
int vk_pipeline_prepare_pipelines(void) {
    prep_mode = 1;   // builds into pipeline-cache.bin alone (cache_load)
    if (!ensure_init()) {
        fprintf(stderr, "prepare-pipelines: no Vulkan device\n");
        return 1;
    }
    char dir[PLAT_PATH_MAX * 2];
    if (!cache_dir(dir, sizeof dir)) return 1;
    char path[PLAT_PATH_MAX * 2 + 48];
    snprintf(path, sizeof path, "%s/seed-pipeline-keys.bin", dir);
    if (!plat_file_exists(path)) {
        fprintf(stderr, "prepare-pipelines: no %s (tools/prepare_shaders.py writes it from the pipeline list)\n", path);
        return 1;
    }
    return seed_pipelines(dir);
}

// nx2navi.exe --prepare-shaders (tools/prepare_shaders.py): the device sets the translator's host answers (they are in every
// cache key), the seed next to nx2navi.exe is translated on every core, then every prepared variant's stage library or
// compute pipeline is built once into the driver's disk cache (lb_warm_prepared). No game runs.
int vk_pipeline_prepare_shaders(void) {
    prep_mode = 1;   // builds into pipeline-cache.bin alone (cache_load)
    if (!ensure_init()) {
        fprintf(stderr, "prepare-shaders: no Vulkan device\n");
        return 1;
    }
    char dir[PLAT_PATH_MAX * 2];
    if (!cache_dir(dir, sizeof dir)) return 1;
    int cpus = (int)plat_cpu_usable();
    const char *pe = cfg_get("NX2NAVI_PREWARM_XLATE");   // no fixed default: half the CPUs, at most 16, when unset
    int procs = pe && *pe ? atoi(pe) : cpus / 2 > 16 ? 16 : cpus / 2;
    int rc = gfx_shader_prepare_seed(dir, cpus, procs < 1 ? 1 : procs);
    // The listed pipelines before the stage libraries: the pipeline cache is saved with them in it and without the
    // tens of thousands of libraries the next step builds, which would take the file past its size limit.
    if (!rc) seed_pipelines(dir);
    if (!rc) lb_warm_prepared(dir, 0);
    return rc;
}

// nx2navi.exe --warm-shaders: every variant of shader-prepared.bin built into the driver's disk cache again from the shader
// cache, without translating the seed. The AMD cache is keyed to the exe path (a relink keeps it: 29,587 variants in
// 1.8 s) and holds a limited amount: preparing another build dir evicted this one's libraries, and a library a draw
// needed then took 40-220 ms in play instead of 0.2 ms. Seconds when the libraries are still there.
int vk_pipeline_warm_shaders(void) {
    prep_mode = 1;   // builds into pipeline-cache.bin alone (cache_load)
    if (!ensure_init()) {
        fprintf(stderr, "warm-shaders: no Vulkan device\n");
        return 1;
    }
    char dir[PLAT_PATH_MAX * 2];
    if (!cache_dir(dir, sizeof dir)) return 1;
    char prp[PLAT_PATH_MAX * 2 + 32];
    snprintf(prp, sizeof prp, "%s/shader-prepared.bin", dir);
    if (!plat_file_exists(prp)) {
        fprintf(stderr, "warm-shaders: no %s (tools/prepare_shaders.py writes it)\n", prp);
        return 1;
    }
    seed_pipelines(dir);   // before the libraries, as in vk_pipeline_prepare_shaders
    lb_warm_prepared(dir, 1);
    return 0;
}

// nx2navi.exe --probe-prewarm: the boot's shader and pipeline stages without the game, from the caches next to
// nx2navi.exe (pipeline-cache.bin, pipeline-keys.bin, shader-answers.bin, the shader cache): the pipeline cache load,
// the shader prewarm, the recorded pipelines and the compute pipeline prebuild, each followed by vk_mem_stage_log's
// line, so the log tells what every stage leaves in device memory and in the process's commit. Then up to
// PROBE_DISPATCHES compute shaders whose pipeline the boot did not keep are asked for as a dispatch would
// (gfx_shader_load_input for the shader, then vk_pipeline_compute), which gives what a first dispatch costs on this
// machine and how many of them the pipeline cache held. Fails when one of those pipelines is not created, when the
// prebuild kept a pipeline it should not have, when a large compute shader went to the background builder although no
// run dispatched it, or when the record of dispatched shaders, written to a file of the probe's own and read back,
// does not hold the probe's dispatches. compute-used.bin itself stays as it is; the pipeline cache is saved as after
// a boot.
#define PROBE_DISPATCHES 256
int vk_pipeline_probe_prewarm(void) {
    vk_pipeline_async_enable();
    CU.quiet = 1;
    vk_pipeline_prewarm();             // brings the device up itself: it has to own the key file before ensure_init runs
    if (!P.ready) {
        printf("probe-prewarm: SKIP (the boot prewarm brought no Vulkan device up: none on this machine, NX2NAVI_GPU=null "
               "or NX2NAVI_PIPE_SYNC=1)\n");
        return 0;
    }
    while (__atomic_load_n(&LB.running, __ATOMIC_ACQUIRE) > 0) plat_sleep_ms(50);
    if (!LB.n) {
        printf("probe-prewarm: SKIP (no recorded compute shaders next to the executable: %s)\n", P.cache_path);
        return 0;
    }
    VkPipeStats st;
    vk_pipeline_stats(&st);
    plat_mutex_lock_shared(&LB.stat);
    uint64_t kept = LB.built[2], cache_only = LB.warmed[2], failed = LB.fail[2], there = LB.have[2];
    uint64_t big_unused = LB.big_unused;
    size_t queued = BG.n;
    plat_mutex_unlock_shared(&LB.stat);
    printf("probe-prewarm: %zu compute shaders, %zu to keep: %llu kept, %llu built into the pipeline cache only, %llu "
           "there already, %llu failed, %llu large ones not built (no run dispatched them), %zu large ones for the "
           "background builder; %llu graphics pipelines\n", LB.n, LB.n_keep, (unsigned long long)kept,
           (unsigned long long)cache_only, (unsigned long long)there, (unsigned long long)failed,
           (unsigned long long)big_unused, queued, (unsigned long long)st.graphics);
    int fails = kept > LB.n_keep;      // a dispatch never waits in this mode, so nothing else may have been kept
    if (fails) printf("probe-prewarm: FAIL: %llu pipelines kept, %zu expected at most\n", (unsigned long long)kept, LB.n_keep);
    if (queued > LB.n_keep)            // only a shader a run dispatched may be built ahead when it is large
        fails++, printf("probe-prewarm: FAIL: %zu large compute shaders queued for the background builder, %zu kept "
                        "shaders at most could be among them\n", queued, LB.n_keep);
    uint32_t asked = 0, got = 0;
    double t_all = 0, t_max = 0, t_load = 0;
    for (size_t i = LB.n_keep; i < LB.n && asked < PROBE_DISPATCHES; i++) {
        if (LB.e[i].stage != GFX_STAGE_CS) continue;
        double l0 = now_ms();
        const GfxShader *sh = gfx_shader_load_input(LB.e[i].iid, 0);   // from the disk: the boot released its copy
        double load = now_ms() - l0;
        if (!sh || gfx_shader_stage(sh) != GFX_STAGE_CS) continue;
        uint64_t words = gfx_shader_words(sh);
        if (LB.big_words && words > LB.big_words) continue;   // a first dispatch of one compiles it: not what is timed here
        const VkPipeInfo *info = NULL;
        double t0 = now_ms();
        VkPipeline p = vk_pipeline_compute(sh, &info);
        double ms = now_ms() - t0;
        asked++, got += p != VK_NULL_HANDLE;
        t_all += ms, t_load += load;
        if (ms > t_max) t_max = ms;
    }
    vk_mem_stage_log("the probe's first dispatches");
    plat_mutex_lock_shared(&P.lock);
    uint64_t hit = CF.hit;
    plat_mutex_unlock_shared(&P.lock);
    printf("probe-prewarm: %u first dispatches of pipelines the boot did not keep: %u created, avg %.2f ms, longest %.2f "
           "ms, the pipeline cache held %llu of them; reading their shaders took avg %.2f ms\n", asked, got,
           asked ? t_all / asked : 0.0, t_max, (unsigned long long)hit, asked ? t_load / asked : 0.0);
    if (got != asked) fails++, printf("probe-prewarm: FAIL: %u pipelines were not created\n", asked - got);
    if (CU.opened) {
        char rp[sizeof CU.path + 8];
        snprintf(rp, sizeof rp, "%s.probe", CU.path);
        uint64_t rsize = 0;
        CuHeader rh = {0};
        uint32_t mine = 0;
        int wrote = cu_write(rp);
        uint8_t *rf = wrote ? plat_file_read(rp, &rsize) : NULL;
        long rn = cu_parse(rf, rsize, &rh);
        for (long i = 0; i < rn; i++) {
            CuRec r;
            memcpy(&r, rf + sizeof rh + (size_t)i * sizeof r, sizeof r);
            mine += r.run == CU.run;
        }
        free(rf);
        plat_file_delete(rp);
        printf("probe-prewarm: the record of dispatched compute shaders: %ld entries, %u of this run (run %u)\n", rn, mine,
               CU.run);
        if (!wrote || rn < 0 || rh.run != CU.run || mine != got)
            fails++, printf("probe-prewarm: FAIL: the record does not hold the %u shaders dispatched here\n", got);
    }
    lb_stop();
    vk_pipeline_save();
    printf("probe-prewarm: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}

// ---- dynamic state

void vk_pipeline_set_dynamic(VkCommandBuffer cb, const VkPipeState *s) {
    VkDynShadow sh;
    uint64_t skipped = 0;
    sh.have = 0;   // nothing known: every call is recorded
    vk_pipeline_set_dynamic_delta(cb, s, &sh, 0, &skipped);
}

// Every graphics pipeline the replay binds lists the same dynamic states (P.dyn; mesh pipelines leave out topology,
// primitive restart and vertex input), so a value recorded into a command buffer holds across pipeline binds until it
// is set again. Each call is recorded only when its value differs from the one *sh says the command buffer has: a new
// pipeline usually changes the shaders and a field or two, not the twenty states this used to record with every bind.
int vk_pipeline_set_dynamic_delta(VkCommandBuffer cb, const VkPipeState *s, VkDynShadow *sh, int mesh, uint64_t *skipped) {
    if (!ensure_init()) return 0;
    const poly_st *pl = &s->poly;
    const ds_st *d = &s->ds;
    int core = (sh->have & VK_DYN_CORE) != 0, vtx = !mesh && (sh->have & VK_DYN_VTX), blend = (sh->have & VK_DYN_BLEND) != 0;
    int n = 0, left = 0;
#define DYN(known, field, val, call)                                         \
    do {                                                                     \
        if ((known) && sh->field == (val)) left++;                           \
        else sh->field = (val), call, n++;                                   \
    } while (0)
    // The front face maps unchanged: NX2NAVI sets the upper-left window origin, and the y-flipped viewport (y_flip) puts
    // NDC +y at row 0 as NVN does, so the framebuffer winding Vulkan tests is the one NVN tests
    // (a lower-left origin would need it mirrored).
    int front_cw = pl->front == NVN_FRONT_FACE_CW;
    VkCullModeFlags cull = (VkCullModeFlags)(pl->cull & 3);   // NVN_FACE_* has VkCullModeFlagBits' values
    VkFrontFace front = front_cw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    DYN(core, cull, cull, vkCmdSetCullMode(cb, cull));
    DYN(core, front, front, vkCmdSetFrontFace(cb, front));
    if (!mesh) {
        VkPrimitiveTopology topo = vk_pipeline_topology(s->prim);
        DYN(vtx, topo, topo, vkCmdSetPrimitiveTopology(cb, topo));
    }
    DYN(core, dt, (VkBool32)d->dt, vkCmdSetDepthTestEnable(cb, d->dt));
    DYN(core, dw, (VkBool32)d->dw, vkCmdSetDepthWriteEnable(cb, d->dw));
    DYN(core, dop, compare_op(d->df), vkCmdSetDepthCompareOp(cb, compare_op(d->df)));
    DYN(core, st, (VkBool32)d->st, vkCmdSetStencilTestEnable(cb, d->st));
    for (int f = 0; f < 2; f++) {
        VkStencilOp o[3] = {stencil_op(f ? d->fail_b : d->fail_f), stencil_op(f ? d->zpass_b : d->zpass_f),
                            stencil_op(f ? d->zfail_b : d->zfail_f)};
        VkCompareOp c = compare_op(f ? d->sf_b : d->sf_f);
        if (core && sh->scmp[f] == c && !memcmp(sh->sop[f], o, sizeof o)) {
            left++;
            continue;
        }
        vkCmdSetStencilOp(cb, f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT, o[0], o[1], o[2], c);
        sh->scmp[f] = c, memcpy(sh->sop[f], o, sizeof o), n++;
    }
    // NVN_POLYGON_OFFSET_ENABLE_* bit of the polygon mode in use: point 1, line 2, fill 4
    unsigned mode = pl->mode <= NVN_POLYGON_MODE_FILL ? pl->mode : NVN_POLYGON_MODE_FILL;
    VkBool32 bias_en = (pl->offset_en >> mode & 1) != 0, discard = s->raster_discard != 0, restart = s->prim_restart != 0;
    DYN(core, bias_en, bias_en, vkCmdSetDepthBiasEnable(cb, bias_en));
    DYN(core, discard, discard, vkCmdSetRasterizerDiscardEnable(cb, discard));
    if (!mesh) DYN(vtx, restart, restart, vkCmdSetPrimitiveRestartEnable(cb, restart));
    if (!mesh && P.dyn_vi && s->vs) {
        VtxInput v;
        vertex_input(s, &v);
        if (vtx && sh->vi.n_attr == v.n_attr && sh->vi.n_bind == v.n_bind &&
            !memcmp(sh->vi.a, v.a, sizeof v.a[0] * v.n_attr) && !memcmp(sh->vi.b, v.b, sizeof v.b[0] * v.n_bind)) {
            left++;
        } else {
            rec_vertex_input(P.set_vi, cb, v.n_bind, v.b, v.n_attr, v.a);
            sh->vi = v, n++;
        }
    } else if (!mesh) {
        sh->vi.n_attr = UINT32_MAX;   // not recorded: unknown to the next draw
    }
    if (P.dyn_clamp) {
        VkBool32 clamp = s->depth_clamp != 0;
        DYN(core, clamp, clamp, rec_bool(P.set_clamp, cb, clamp));
    }
    if (P.dyn_poly) {
        VkPolygonMode pm = polygon_mode(pl->mode);
        DYN(core, poly, pm, rec_polygon_mode(P.set_poly, cb, pm));
    }
    if (P.dyn_a2c) {   // the same signature as the depth clamp call
        VkBool32 a2c = s->ms.a2c != 0;
        DYN(core, a2c, a2c, rec_bool((PFN_vkCmdSetDepthClampEnableEXT)P.set_a2c, cb, a2c));
    }
#undef DYN
    sh->have |= VK_DYN_CORE;
    if (mesh) sh->have &= ~(uint32_t)VK_DYN_VTX;
    else sh->have |= VK_DYN_VTX;
    int nt = target_count(s);
    if (nt) {
        VkBool32 en[8];
        VkColorBlendEquationEXT eq[8];
        VkColorComponentFlags wm[8];
        for (int i = 0; i < nt; i++) {
            const blend_st *b = &s->blend[i];
            en[i] = (s->color.blend_en >> i & 1) && blendable(target_format(s->color_fmt[i], NULL, NULL));
            eq[i] = (VkColorBlendEquationEXT){blend_factor(b->src_rgb), blend_factor(b->dst_rgb), blend_op(b->eq_rgb),
                                              blend_factor(b->src_a), blend_factor(b->dst_a), blend_op(b->eq_a)};
            wm[i] = target_wmask(s, i);
        }
        // The calls write targets [0, nt); targets past nt keep what the command buffer had.
        int known = blend && (uint32_t)nt <= sh->nt;
        if (P.dyn_blend_en) {
            if (known && !memcmp(sh->en, en, sizeof en[0] * nt)) left++;
            else rec_blend_enable(P.set_blend_en, cb, 0, (uint32_t)nt, en), n++;
        }
        if (P.dyn_blend_eq) {
            if (known && !memcmp(sh->eq, eq, sizeof eq[0] * nt)) left++;
            else rec_blend_equation(P.set_blend_eq, cb, 0, (uint32_t)nt, eq), n++;
        }
        if (P.dyn_wmask) {
            if (known && !memcmp(sh->wm, wm, sizeof wm[0] * nt)) left++;
            else rec_write_mask(P.set_wmask, cb, 0, (uint32_t)nt, wm), n++;
        }
        memcpy(sh->en, en, sizeof en[0] * nt), memcpy(sh->eq, eq, sizeof eq[0] * nt), memcpy(sh->wm, wm, sizeof wm[0] * nt);
        sh->nt = blend && sh->nt > (uint32_t)nt ? sh->nt : (uint32_t)nt;
        sh->have |= VK_DYN_BLEND;
    }
    *skipped += (uint64_t)left;
    return n;
}

// ---- statistics, persistence, teardown

void vk_pipeline_stats(VkPipeStats *s) {
    plat_mutex_lock_shared(&P.lock);
    *s = P.stats;
    plat_mutex_unlock_shared(&P.lock);
}

static PlatMutex save_lock = PLAT_MUTEX_INIT;   // one cache save at a time; shutdown takes it to destroy the cache

// Copies a pipeline cache's data into its file, with cache_rw held exclusively for the copy only (not the file write):
// P.play into pipeline-cache-play.bin when there is a play cache, else P.cache into pipeline-cache.bin; and P.cache
// once, with or without a play cache, after the play file was merged into it at the start (P.fold), which then
// removes the play file.
// Only when the data changed: the size of a cache's data is asked first, and a cache whose data has the size its file
// holds is neither copied nor written. A pipeline cache only gains entries, so the same size is the same data. Before,
// every save copied and wrote the whole cache whenever a pipeline had been created since the last one, also when the
// cache had held it: three saves of 358.1 MB in one run that added 0.3 MB (2026-11-11).
// The lock is only ever tried, never waited for: a waiting exclusive request would hold back every new build
// (a shared request queues behind it), so a pending autosave could stall the replay. The save at exit (exiting) holds
// new builds back instead (cache_saving_at_exit) and so only waits for the builds already running. wait_ms: how long to
// keep trying while builds run (0: one try). Returns 0 when no build-free moment came, -1 when the data could not be
// copied or its file not written (the log says which), 1 otherwise (saved, or nothing to save).
static int cache_save_one(int which, uint32_t wait_ms, int exiting) {
    VkPipelineCache cache = which ? P.play : P.cache;
    const char *path = which ? P.play_path : P.cache_path;
    if (!cache) return 1;
    uint64_t until = plat_now_ms() + wait_ms;
    if (exiting) __atomic_store_n(&cache_saving_at_exit, 1, __ATOMIC_RELEASE);
    while (!plat_mutex_trylock(&cache_rw)) {
        if (plat_now_ms() >= until) {
            if (exiting) __atomic_store_n(&cache_saving_at_exit, 0, __ATOMIC_RELEASE);
            return 0;
        }
        plat_sleep_ms(2);
    }
    size_t size = 0;
    void *data = NULL;
    int sized = vkGetPipelineCacheData(P.device, cache, &size, NULL) == VK_SUCCESS && size;
    if (sized) __atomic_store_n(&P.held[which], (uint64_t)size, __ATOMIC_RELAXED);
    // A cache past the limit is not written: the next start would drop the file whole (cache_data_read), so the file
    // keeps its last save under the limit and only the pipelines made since then are compiled again. A preparation
    // writes anyway: what it builds is the base every start reads, and a file it cannot write would leave none.
    static int over_said[2];
    uint64_t cap = cache_cap();
    if (sized && cap && size > cap && !prep_mode) {
        plat_mutex_unlock(&cache_rw);
        if (exiting) __atomic_store_n(&cache_saving_at_exit, 0, __ATOMIC_RELEASE);
        if (!over_said[which]++)
            rt_log("vk_pipeline: %s not written past its last save: the cache holds %.1f MB, over the limit of %llu MB; "
                   "pipelines made from here on compile again at the next start (NX2NAVI_PIPELINE_CACHE_MAX_MB raises "
                   "the limit)", path, size / 1048576.0, (unsigned long long)(cap >> 20));
        return 1;
    }
    if (sized && cap && size > cap)
        rt_log("vk_pipeline: %s holds %.1f MB, over the limit of %llu MB: written for the preparation, and dropped at "
               "the next start unless NX2NAVI_PIPELINE_CACHE_MAX_MB is raised", path, size / 1048576.0,
               (unsigned long long)(cap >> 20));
    int same = sized && size == P.saved[which];
    int got = sized && !same && (data = malloc(size)) && vkGetPipelineCacheData(P.device, cache, &size, data) == VK_SUCCESS;
    plat_mutex_unlock(&cache_rw);
    // The held-back builds go on before the log line and the file write: one of them may hold a lock those wait for.
    if (exiting) __atomic_store_n(&cache_saving_at_exit, 0, __ATOMIC_RELEASE);
    static uint64_t skipped[2];
    int failed = !sized || (!same && !got);
    if (same) skipped[which]++;
    if (failed) rt_log("vk_pipeline: %s not saved: no copy of its %.1f MB", path, size / 1048576.0);
    if (got) {
        uint64_t t0 = plat_now_ms();
        if (!plat_file_write_atomic(path, data, size)) {
            rt_log("vk_pipeline: cannot write %s (%.1f MB)", path, size / 1048576.0);
            failed = 1;
        } else {
            uint64_t loaded = which ? P.play_loaded : P.cache_loaded;
            rt_log("vk_pipeline: %s saved: %.1f MB (%+.1f MB since the boot) written in %llu ms; %llu saves before it were "
                   "skipped with the data unchanged", path, size / 1048576.0, ((double)size - (double)loaded) / 1048576.0,
                   (unsigned long long)(plat_now_ms() - t0), (unsigned long long)skipped[which]);
            P.saved[which] = size, skipped[which] = 0;
            if (!which && P.fold) {   // the play file's content is in pipeline-cache.bin now
                P.fold = 0;
                plat_file_delete(P.play_path);
                rt_log("vk_pipeline: %s removed: %s holds its pipelines", P.play_path, P.cache_path);
            }
            if (P.feedback) {
                char when[96];
                snprintf(when, sizeof when, "at a save of %.1f MB (%+.1f MB since the boot)", size / 1048576.0,
                         ((double)size - (double)loaded) / 1048576.0);
                fb_log(when);
            }
        }
    }
    free(data);
    return failed ? -1 : 1;
}

// Both caches' saves (cache_save_one): 1 when each is done, 0 when one found no build-free moment, -1 when one failed.
static int cache_save(uint32_t wait_ms, int exiting) {
    if (!P.ready || !P.cache || !P.cache_path[0]) return 1;
    // Not under P.lock: holding P.lock through the copy and the file write (about 60 ms on the 1.4.3 fight) stopped
    // every pipeline lookup of the replay, a freeze at each 20 s autosave.
    plat_mutex_lock(&save_lock);
    if (!P.cache) {   // vk_pipeline_shutdown destroyed it
        plat_mutex_unlock(&save_lock);
        return 1;
    }
    int ok = 1, r;
    if (!P.play || P.fold) ok = cache_save_one(0, wait_ms, exiting);
    // While a fold is not written yet (P.fold: refused at the limit, or no build-free moment) the play file still holds
    // what was folded in memory only: written over with this run's play cache, that would be in neither file.
    static int fold_kept_said;
    if (P.play && P.fold) {
        if (!fold_kept_said++)
            rt_log("vk_pipeline: %s is kept as it is until %s holds what was folded from it; this run's new pipelines "
                   "are saved once that write is done", P.play_path, P.cache_path);
    } else if (P.play && (r = cache_save_one(1, wait_ms, exiting)) < ok) {
        ok = r;   // the worse of the two
    }
    plat_mutex_unlock(&save_lock);
    return ok;
}

// Builds still running after this long at a save that waits (exit, the shader preparation) leave the file as it was.
#define SAVE_WAIT_MS 5000

// Returns cache_save's answer.
static int save_with(uint32_t wait_ms, int exiting) {
    if (P.ready) keys_flush(), pb_flush(), cu_flush(), cf_log();
    if (P.ready && P.gpl) {
        rt_log("vk_pipeline: gpl: %llu fast links (%llu by workers, %llu of those by the library workers for skipped draws; "
               "%llu of pipelines with a feedback loop; avg %.2f ms, "
               "max %.2f ms, %llu failed), %llu lookups by a draw found a stage library not made in this process yet and "
               "left it to a worker (whether the driver's cache has it shows in the library times below), %llu optimised "
               "pipelines swapped in (%llu queued "
               "low after their fast link; %llu replaced fast links given back), %llu drawing-thread links over 5 ms; %llu "
               "waiting draws built their libraries "
               "(avg %.2f ms, max %.2f ms)",
               (unsigned long long)P.g.links, (unsigned long long)P.g.wlinks, (unsigned long long)P.g.flinks,
               (unsigned long long)P.g.fb_links,
               P.g.links ? P.g.link_ms / P.g.links : 0.0,
               P.g.link_max, (unsigned long long)P.g.link_fail, (unsigned long long)P.g.defer,
               (unsigned long long)P.g.swaps, (unsigned long long)P.g.requeued, (unsigned long long)fast_retired,
               (unsigned long long)P.g.slow,
               (unsigned long long)P.g.dlinks, P.g.dlinks ? P.g.dlink_ms / P.g.dlinks : 0.0, P.g.dlink_max);
        uint64_t rn = P.g.ready_n[0] + P.g.ready_n[1] + P.g.ready_n[2] + P.g.ready_n[3] + P.g.ready_n[4] + P.g.ready_n[5];
        rt_log("vk_pipeline: gpl: pipelines a draw missed were usable after under 4 ms %llu, 4-8 ms %llu, 8-17 ms %llu, "
               "17-50 ms %llu, 50-150 ms %llu, longer %llu (avg %.1f ms, longest %.0f ms); %llu of them had no fast link; "
               "%llu were mesh pipelines (counted by themselves below), %llu were named in pipeline-keys.bin and not "
               "prebuilt yet (the others were new to this install), %llu had a shader translated in this session",
               (unsigned long long)P.g.ready_n[0], (unsigned long long)P.g.ready_n[1], (unsigned long long)P.g.ready_n[2],
               (unsigned long long)P.g.ready_n[3], (unsigned long long)P.g.ready_n[4], (unsigned long long)P.g.ready_n[5],
               rn ? P.g.ready_ms / (double)rn : 0.0, P.g.ready_max, (unsigned long long)P.g.ready_nofast,
               (unsigned long long)P.g.ready_mesh, (unsigned long long)P.g.ready_recorded,
               (unsigned long long)P.g.ready_fresh);
        // The mesh pipelines by themselves: with stage libraries the first two numbers follow each other and the
        // times are a link's; without (the start's line says why) none is linked and the time is a whole build's.
        if (vk_ctx()->has_mesh_shader && !vk_ctx()->mesh_fallback)
            rt_log("vk_pipeline: gpl: mesh pipelines (%s): %llu fast links (avg %.2f ms, max %.2f ms); the %llu a draw "
                   "missed were usable after avg %.1f ms, longest %.0f ms, %llu of them through a fast link (the others "
                   "after their whole build); their libraries made in this run: %llu of task and mesh stages (avg %.1f "
                   "ms, max %.0f ms), %llu fragment shader (avg %.1f ms, max %.0f ms)",
                   mesh_libs() ? "linked from stage libraries" : "built whole, without stage libraries",
                   (unsigned long long)P.g.m_links, P.g.m_links ? P.g.m_link_ms / (double)P.g.m_links : 0.0,
                   P.g.m_link_max, (unsigned long long)P.g.ready_mesh,
                   P.g.ready_mesh ? P.g.m_ready_ms / (double)P.g.ready_mesh : 0.0, P.g.m_ready_max,
                   (unsigned long long)P.g.m_ready_fast, (unsigned long long)P.g.m_lib_n[0],
                   P.g.m_lib_n[0] ? P.g.m_lib_ms[0] / (double)P.g.m_lib_n[0] : 0.0, P.g.m_lib_max[0],
                   (unsigned long long)P.g.m_lib_n[1], P.g.m_lib_n[1] ? P.g.m_lib_ms[1] / (double)P.g.m_lib_n[1] : 0.0,
                   P.g.m_lib_max[1]);
        if (P.g.hints)
            rt_log("vk_pipeline: gpl: looking ahead named %llu pipeline states not known yet and queued %llu stage libraries "
                   "for them", (unsigned long long)P.g.hints, (unsigned long long)P.g.hint_libs);
        if (A.first || A.taken)
            rt_log("vk_pipeline: whole builds a draw waited for while they were still queued: %llu moved to the head of "
                   "the queue, %llu built by the waiting thread (a wait outside the frame's budget, no worker idle)",
                   (unsigned long long)A.first, (unsigned long long)A.taken);
        if (U.waits || U.queued)
            rt_log("vk_pipeline: gpl: library jobs for waiting draws: %llu queued, %llu built by a worker, %llu by the "
                   "thread that needed them (no worker free), %llu waits ended at their limit; %llu fast links on the %d "
                   "library workers",
                   (unsigned long long)U.queued, (unsigned long long)U.by_worker, (unsigned long long)U.stolen,
                   (unsigned long long)U.timeouts, (unsigned long long)U.links, U.lib_workers);
        static const char *const kn[4] = {"vertex input", "pre-rasterization", "fragment shader", "fragment output"};
        for (int i = 0; i < 4; i++)
            rt_log("vk_pipeline: gpl: %s libraries %llu (avg %.2f ms, max %.2f ms; %llu of 25 ms or more, %.0f ms in all)",
                   kn[i], (unsigned long long)P.g.lib_n[i], P.g.lib_n[i] ? P.g.lib_ms[i] / P.g.lib_n[i] : 0.0, P.g.lib_max[i],
                   (unsigned long long)P.g.lib_slow[i], P.g.lib_slow_ms[i]);
        if (P.g.pr_added)
            rt_log("vk_pipeline: gpl: %llu of the pre-rasterization libraries had outputs added for their fragment shaders "
                   "(avg %.1f ms): another module than the one the preparation built, so the driver's cache has none of them",
                   (unsigned long long)P.g.pr_added, P.g.pr_added_ms / (double)P.g.pr_added);
        if (LB.n)
            rt_log("vk_pipeline: gpl: prebuilt libraries a draw used: pre-rasterization %llu, fragment shader %llu; "
                   "compute pipelines built on the replay thread %llu", (unsigned long long)P.g.pre_used[LIB_PR],
                   (unsigned long long)P.g.pre_used[LIB_FS], (unsigned long long)LB.cs_draw);
    }
    gfx_shader_record_flush(1);
    if (P.ready && !P.cache) pb_log("at a save");   // with P.cache the line comes with its own save below
    int saved = cache_save(wait_ms, exiting);
    if (!saved && wait_ms)
        rt_log("vk_pipeline: %s not saved: pipeline builds kept running for %u ms", P.play ? P.play_path : P.cache_path,
               (unsigned)wait_ms);
    return saved;
}

// The save after the shader preparation and at shutdown: it waits up to SAVE_WAIT_MS for a moment with no build running.
void vk_pipeline_save(void) {
    save_with(SAVE_WAIT_MS, 0);
}

// The save as the process ends (window closed, the game's exit, a timed run's end): new builds wait while it waits up to
// SAVE_WAIT_MS for the running ones to finish (cache_saving_at_exit).
void vk_pipeline_save_exit(void) {
    journal_close();   // an orderly end during the whole warm is not a driver crash
    save_with(SAVE_WAIT_MS, 1);
}

// An orderly end before the guest starts that saves no pipelines (cheats.c's rebuild exit): the boot's journal closed
// as vk_pipeline_save_exit closes it, so the next start does not take the end for a driver crash in the library warm.
void vk_pipeline_boot_abandon(void) { journal_close(); }

// The memory report's figure for the pipeline caches the driver keeps in process memory (vk_pipeline_mem): the size of
// each cache's data, asked once a minute here on the autosave thread, and by every save (cache_save_one). The size is
// asked as the save asks it, at a moment no build runs into either cache (cache_rw, tried once and never waited for),
// and under save_lock, which the shutdown takes before it destroys the caches. The play file holds what this run
// compiled, but pipeline-cache.bin is not written in play: its cache grows in memory with the compute pipelines the boot
// builds for it alone, and only this query says by how much.
#define SIZES_EVERY_MS 60000
static void cache_sizes_ask(void) {
    if (!plat_mutex_trylock(&save_lock)) return;
    if (plat_mutex_trylock(&cache_rw)) {
        for (int w = 0; w < 2; w++) {
            VkPipelineCache cache = w ? P.play : P.cache;
            size_t size = 0;
            if (cache && vkGetPipelineCacheData(P.device, cache, &size, NULL) == VK_SUCCESS && size)
                __atomic_store_n(&P.held[w], (uint64_t)size, __ATOMIC_RELAXED);
        }
        plat_mutex_unlock(&cache_rw);
    }
    plat_mutex_unlock(&save_lock);
}

// The game never shuts the device down (boot.c and the window end the process), so the cache is saved while it
// runs: at most every 20 s, and only after new pipelines were built or while a save that did not happen is still owed
// (vk_save_retry.h). New pipeline keys are appended every 2 s.
static void autosave_once(void) {
    static uint64_t last_keys, last_sizes;
    static SaveRetry sr;
    if (!P.ready) return;
    uint64_t now = plat_now_ms();
    __atomic_store_n(&pipe_minute, minute_now(), __ATOMIC_RELAXED);
    release_drain();         // the pipelines given back earlier that the GPU is done with
    pressure_release(now);   // while video memory is short: the pipelines no draw used go
    if (now - last_sizes >= SIZES_EVERY_MS && (P.cache || P.play)) last_sizes = now, cache_sizes_ask();
    if (now - last_keys >= 2000) {
        last_keys = now;
        if (*(volatile uint32_t *)&K.pend_n) keys_flush();
        pb_flush();
        cu_flush();
    }
    gfx_shader_record_flush(0);
    if (now - sr.at_ms < SAVE_EVERY_MS) return;
    VkPipeStats s;
    vk_pipeline_stats(&s);
    uint64_t n = s.graphics + s.compute + cache_merges;   // a merge comes after its pipeline was counted (bg_main)
    if (!save_due(&sr, n, now, P.fold)) return;            // P.fold: the play file merged at the start is to be written
    // Only at a moment with no build running (cache_save): while one runs the save waits for the next tick, 500 ms
    // on, before it logs or flushes anything, so a busy stretch does not repeat the statistics lines every tick.
    if (P.cache && P.cache_path[0]) {
        if (!plat_mutex_trylock(&cache_rw)) return;
        plat_mutex_unlock(&cache_rw);
    }
    if (save_tried(&sr, n, now, save_with(0, 0) > 0))
        rt_log("vk_pipeline: the pipeline cache was not saved in %d tries in a row; the next try is made when a new "
               "pipeline is built, or at exit", SAVE_RETRIES + 1);
}

// On its own thread: the save (vkGetPipelineCacheData and the file write, about 60 ms) and the key appends (a flush to
// the disk each) ran on the present thread, and the render thread waited for it to take the next frame.
static void autosave_main(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    for (;;) {
        plat_sleep_ms(500);
        autosave_once();
    }
}

void vk_pipeline_autosave(void) {
    static int32_t started;
    if (!P.ready || plat_xchg32(&started, 1)) return;
    if (!plat_thread_spawn(autosave_main, NULL, "pipe autosave", 0))
        rt_log("vk_pipeline: cannot start the autosave thread; the cache is saved at exit only");
}

void vk_pipeline_shutdown(void) {
    if (!P.ready) return;
    lb_stop();
    compute_warm_clear();
    workers_drain();   // jobs point at layouts and Pipe entries freed below
    vk_pipeline_save();
    plat_mutex_lock(&P.lock);
    vkDeviceWaitIdle(P.device);
    for (int i = 0; i < PIPE_BUCKETS; i++) {
        for (Pipe *p = pipes[i], *n; p; p = n) {
            n = p->next;
            if (p->info.pipeline) vkDestroyPipeline(P.device, p->info.pipeline, NULL);
            if (p->fast && p->fast != p->info.pipeline) vkDestroyPipeline(P.device, p->fast, NULL);
            free(p->bindings), free(p);
        }
        pipes[i] = NULL;
    }
    libs_destroy();
    for (int i = 0; i < LAYOUT_BUCKETS; i++) {
        for (Layout *l = layouts[i], *n; l; l = n) {
            n = l->next;
            for (int k = 0; k < 2; k++)
                if (l->tpl[k]) P.tpl_destroy(P.device, l->tpl[k], NULL);
            vkDestroyPipelineLayout(P.device, l->layout, NULL);
            for (int h = 0; h < 2; h++)
                if (l->half[h]) vkDestroyPipelineLayout(P.device, l->half[h], NULL);
            vkDestroyDescriptorSetLayout(P.device, l->set, NULL);
            if (l->set1) vkDestroyDescriptorSetLayout(P.device, l->set1, NULL);
            free(l);
        }
        layouts[i] = NULL;
    }
    plat_mutex_lock(&save_lock);   // not while the autosave thread copies it
    if (P.cache) vkDestroyPipelineCache(P.device, P.cache, NULL);
    if (P.play) vkDestroyPipelineCache(P.device, P.play, NULL);
    P.cache = P.play = VK_NULL_HANDLE;
    plat_mutex_unlock(&save_lock);
    memset(&P.stats, 0, sizeof P.stats);
    plat_xchg32(&P.ready, 0);
    plat_mutex_unlock(&P.lock);
}

// ---- self-test: census shaders (probe_census_dir, tools/shader_census.py) through gfx_shader_get into pipelines.
// Graphics pairs are a fragment shader and a vertex shader of the same source file, spread over the census; compute
// shaders are spread the same way. Every pipeline is built twice (the second call must hit the cache) and the
// dynamic state of the first graphics pipeline is recorded into a throwaway command buffer.

typedef struct { const char *key, *c1, *src; size_t c1_len, src_len; int stage; uint32_t local[3]; } CensusEntry;

static const char *st_field(const char *from, const char *to, const char *name, size_t *len) {
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\": \"", name);
    const char *p = strstr(from, pat);
    if (!p || p >= to) return NULL;
    p += strlen(pat);
    const char *e = memchr(p, '"', (size_t)(to - p));
    if (!e) return NULL;
    *len = (size_t)(e - p);
    return p;
}

static int census_cmp(const void *x, const void *y) {
    const CensusEntry *a = x, *b = y;
    size_t n = a->src_len < b->src_len ? a->src_len : b->src_len;
    int c = memcmp(a->src, b->src, n);
    if (c) return c;
    if (a->src_len != b->src_len) return a->src_len < b->src_len ? -1 : 1;
    return a->stage - b->stage;
}

static char *census_read(const char *root, size_t *size) {
    char path[PLAT_PATH_MAX * 2 + 64];
    snprintf(path, sizeof path, "%s/jobs.json", root);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) free(b), b = NULL;
    fclose(f);
    if (b) b[n] = 0, *size = (size_t)n;
    return b;
}

static int census_parse(const char *jobs, CensusEntry *out, int max) {
    int n = 0;
    for (const char *p = jobs; n < max && (p = strstr(p, "\"job\": {")); p += 8) {
        const char *end = strstr(p, "\"origin\": ");
        if (!end) break;
        size_t kl, sl, cl, rl;
        const char *k = st_field(p, end, "key", &kl), *s = st_field(p, end, "stage", &sl), *c = st_field(p, end, "c1", &cl),
                   *r = st_field(p, end, "source", &rl);
        if (!k || !s || !c || !r || kl != 64) continue;
        CensusEntry *e = &out[n++];
        e->key = k, e->c1 = c, e->c1_len = cl / 2, e->src = r, e->src_len = rl;
        e->stage = !strncmp(s, "vs", sl) ? GFX_STAGE_VS : !strncmp(s, "ps", sl) ? GFX_STAGE_FS : GFX_STAGE_CS;
        e->local[0] = e->local[1] = e->local[2] = 1;
        const char *l = strstr(p, "\"local\": [");
        if (l && l < end) {
            l += 10;
            for (int i = 0; i < 3; i++) {
                while (*l == ' ' || *l == '\n' || *l == '\r' || *l == ',') l++;
                e->local[i] = (uint32_t)strtoul(l, (char **)&l, 10);
            }
        }
    }
    return n;
}

static int hexv(char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }

// The program the game would hand NVN for a census entry, translated with default answers (as gfx_shader_probe.c).
static const GfxShader *census_shader(const char *root, const CensusEntry *e) {
    char path[PLAT_PATH_MAX * 2 + 128];
    static uint8_t c1[8192];
    uint8_t control[0x880] = {0};
    if (e->c1_len > sizeof c1) return NULL;
    for (size_t b = 0; b < e->c1_len; b++) c1[b] = (uint8_t)(hexv(e->c1[b * 2]) << 4 | hexv(e->c1[b * 2 + 1]));
    snprintf(path, sizeof path, "%s/code/%.64s.code", root, e->key);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long body = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t skip = e->stage == GFX_STAGE_CS ? 0 : 0x30, total = skip + (size_t)body;
    uint8_t *code = calloc(1, total);
    int ok = code && fread(code + skip, 1, (size_t)body, f) == (size_t)body;
    fclose(f);
    if (!ok) return free(code), NULL;
    uint32_t c1_off = (uint32_t)(total - e->c1_len), length = (uint32_t)total;
    memcpy(control + 0x700, &c1_off, 4), memcpy(control + 0x704, &length, 4);
    GfxShaderAnswers a = {0};
    if (e->stage == GFX_STAGE_CS) for (int k = 0; k < 3; k++) a.local[k] = (uint16_t)e->local[k];
    const GfxShader *s = gfx_shader_get(e->stage, control, code, total, c1, &a);
    free(code);
    return s;
}

// A state for a census pair: one target per fragment output (RGBA8 of the output's type), D32S8 depth with the test on,
// alpha blending on target 0, one RGBA32 attribute per vertex input, interleaved in stream 0.
static void selftest_state(VkPipeState *s, const GfxShader *vs, const GfxShader *fs) {
    vk_pipe_state_defaults(s);
    s->vs = vs, s->fs = fs;
    GfxShaderIo io[32];
    uint32_t outs = gfx_shader_outputs(fs, io);
    for (int i = 0; i < 8; i++)
        if (outs >> i & 1)
            s->color_fmt[i] = io[i].type == GFX_TYPE_UINT ? NVN_FORMAT_RGBA8UI : io[i].type == GFX_TYPE_SINT ? NVN_FORMAT_RGBA8I
                                                                                                            : NVN_FORMAT_RGBA8;
    s->depth_fmt = NVN_FORMAT_DEPTH32F_STENCIL8;
    s->ds.dt = 1;
    s->color.blend_en = 1;
    s->blend[0].src_rgb = NVN_BLEND_FUNC_SRC_ALPHA, s->blend[0].dst_rgb = NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA;
    s->poly.cull = NVN_FACE_BACK;
    uint32_t ins = gfx_shader_inputs(vs, io), off = 0;
    for (int l = 0; l < 16; l++)
        if (ins >> l & 1) {
            va_st a = {io[l].type == GFX_TYPE_UINT ? NVN_FORMAT_RGBA32UI : io[l].type == GFX_TYPE_SINT ? NVN_FORMAT_RGBA32I
                                                                                                      : NVN_FORMAT_RGBA32F,
                       off, 0};
            memcpy(&s->vattr[l], &a, sizeof a);
            off += 16;
        }
    s->vstream[0].stride = off ? off : 16;
}

// Records the dynamic state of a pipeline into a command buffer that is never submitted (validation checks the calls).
static int selftest_record(const VkPipeState *s, VkPipeline pipe) {
    VkCtx *c = vk_ctx();
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = c->gfx_family};
    VkCommandPool pool;
    if (vkCreateCommandPool(c->device, &pi, NULL, &pool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb;
    int ok = vkAllocateCommandBuffers(c->device, &ai, &cb) == VK_SUCCESS;
    if (ok) {
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cb, &bi);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vk_pipeline_set_dynamic(cb, s);
        ok = vkEndCommandBuffer(cb) == VK_SUCCESS;
    }
    vkDestroyCommandPool(c->device, pool, NULL);
    return ok;
}

int vk_pipeline_selftest(void) {
    enum { MAXN = 20000, WANT_PAIRS = 64, WANT_CS = 16 };
    char root[PLAT_PATH_MAX * 2];
    if (!vk_init() || !ensure_init()) { printf("vk_pipeline selftest: FAILED: no Vulkan device\n"); return 1; }
    if (!probe_census_dir(root, sizeof root)) {   // the census folder, build/<version>/shaders
        printf("vk_pipeline selftest: FAILED: no build/v%d/shaders/jobs.json above the exe\n", rt_game_version);
        return 1;
    }
    size_t size;
    char *jobs = census_read(root, &size);
    CensusEntry *e = malloc(sizeof *e * MAXN);
    int n = jobs && e ? census_parse(jobs, e, MAXN) : 0;
    if (!n) { printf("vk_pipeline selftest: FAILED: no census entries\n"); free(jobs), free(e); return 1; }
    qsort(e, (size_t)n, sizeof *e, census_cmp);
    long err0 = vk_validation_errors_total();
    VkPipeStats st0;
    vk_pipeline_stats(&st0);

    // candidate pairs: each fragment shader of a source file with one of the file's vertex shaders (round robin)
    int (*pairs)[2] = malloc(sizeof *pairs * (size_t)n), np = 0, *cs = malloc(sizeof *cs * (size_t)n), nc = 0;
    for (int i = 0; i < n;) {
        int j = i, v0 = -1, nv = 0, nf = 0;
        for (; j < n && e[j].src_len == e[i].src_len && !memcmp(e[j].src, e[i].src, e[i].src_len); j++) {
            if (e[j].stage == GFX_STAGE_VS && v0 < 0) v0 = j;   // sorted by stage within a file: vertex shaders first
            nv += e[j].stage == GFX_STAGE_VS;
            if (e[j].stage == GFX_STAGE_CS) cs[nc++] = j;
        }
        for (int k = i; k < j && nv; k++)
            if (e[k].stage == GFX_STAGE_FS) pairs[np][0] = v0 + nf++ % nv, pairs[np][1] = k, np++;
        i = j;
    }
    int tried = 0, built = 0, cached = 1, shader_fail = 0, ctried = 0, cbuilt = 0, recorded = 0, varying_fix = 0;
    for (int t = 0; t < WANT_PAIRS && t < np; t++) {
        const int *pr = pairs[(long long)t * np / (np < WANT_PAIRS ? np : WANT_PAIRS)];
        tried++;
        const GfxShader *vs = census_shader(root, &e[pr[0]]), *fs = census_shader(root, &e[pr[1]]);
        if (!vs || !fs) { shader_fail++; continue; }
        if (gfx_shader_inputs(fs, NULL) & ~gfx_shader_outputs(vs, NULL)) varying_fix++;
        VkPipeState s;
        selftest_state(&s, vs, fs);
        GfxShaderAnswers fa = {0};
        vk_pipeline_answers(&s, NULL, &fa);
        const VkPipeInfo *info;
        VkPipeline p = vk_pipeline_graphics(&s, &info);
        if (!p) continue;
        built++;
        const VkPipeInfo *again;
        if (vk_pipeline_graphics(&s, &again) != p || again != info) cached = 0;
        if (!recorded) recorded = selftest_record(&s, p);
    }
    for (int t = 0; t < WANT_CS && t < nc; t++) {
        const CensusEntry *c = &e[cs[(long long)t * nc / (nc < WANT_CS ? nc : WANT_CS)]];
        ctried++;
        const GfxShader *s = census_shader(root, c);
        if (!s) { shader_fail++; continue; }
        const VkPipeInfo *info;
        VkPipeline p = vk_pipeline_compute(s, &info);
        if (!p) continue;
        cbuilt++;
        const VkPipeInfo *again;
        if (vk_pipeline_compute(s, &again) != p || again != info) cached = 0;
    }
    vk_pipeline_save();
    VkPipeStats st;
    vk_pipeline_stats(&st);
    long errors = vk_validation_errors_total() - err0;
    printf("vk_pipeline selftest: graphics %d/%d census pairs (%d needed vertex outputs added), compute %d/%d, "
           "%d shaders failed to translate; layouts %llu (%llu push); cache hits %llu; dynamic state recorded %d; "
           "validation errors %ld\n",
           built, tried, varying_fix, cbuilt, ctried, shader_fail, (unsigned long long)(st.layouts - st0.layouts),
           (unsigned long long)(st.push_layouts - st0.push_layouts), (unsigned long long)(st.hits - st0.hits), recorded,
           errors);
    free(pairs), free(cs), free(e), free(jobs);
    int ok = built > 0 && cbuilt > 0 && cached && recorded && errors == 0;
    printf("vk_pipeline selftest: %s\n", ok ? "passed" : "FAILED");
    return ok ? 0 : 1;
}

// NX2NAVI_TRACE=perf "mem: sizes" (perf.c): the pipeline table's entries with their pipelines and fast links (only
// pressure_release takes entries out, while video memory is short), layouts, stage libraries, the job queues and the
// pipeline-keys.bin set. A walk of the hash chains under P.lock
// held shared: the workers' inserts and a draw's memo miss wait for it (well under a millisecond).
void vk_pipeline_sizes(PerfSizes *s) {
    uint64_t ent = 0, built = 0, fast = 0, libn = 0, libb = 0, lq = 0;
    plat_mutex_lock_shared(&P.lock);
    for (int i = 0; i < PIPE_BUCKETS; i++)
        for (const Pipe *p = pipes[i]; p; p = p->next) ent++, built += p->info.pipeline != VK_NULL_HANDLE, fast += p->fast != VK_NULL_HANDLE;
    for (int i = 0; i < LIB_BUCKETS; i++)
        for (const Lib *b = libs[i]; b; b = b->next) libn++, libb += b->pipe != VK_NULL_HANDLE;
    for (const LibJob *j = U.head; j; j = j->next) lq++;
    uint64_t lay = P.stats.layouts, fails = P.stats.failures, jobs = A.inflight, seen = K.seen_n, seen_b = (uint64_t)K.seen_cap * 8;
    plat_mutex_unlock_shared(&P.lock);
    perf_size(s, "pipeline entries", ent, 0, PERF_SIZE_NONE);
    perf_size(s, "with a pipeline", built, 0, PERF_SIZE_NONE);
    perf_size(s, "fast links", fast, 0, PERF_SIZE_NONE);
    perf_size(s, "pipeline failures", fails, 0, PERF_SIZE_NONE);
    perf_size(s, "pipeline layouts", lay, 0, PERF_SIZE_NONE);
    perf_size(s, "stage library entries", libn, 0, PERF_SIZE_NONE);
    perf_size(s, "built stage libraries", libb, 0, PERF_SIZE_NONE);
    perf_size(s, "library jobs queued", lq, 0, PERF_SIZE_NONE);
    perf_size(s, "pipeline jobs queued or running", jobs, 0, PERF_SIZE_NONE);
    perf_size(s, "pipeline keys seen", seen, 0, seen_b);
}

// The memory report's pipeline figures (vk_memory.c outside_log and system_log), once a report: what holds device
// memory outside VMA, counted because the driver gives no size for a pipeline, and the host memory of the tables. A walk
// of the hash chains under P.lock held shared, as vk_pipeline_sizes walks them (well under a millisecond: the draws'
// lookups wait for it). A graphics entry's pipeline is whole (built in one piece, no fast link before it), fast-linked
// (from its stage libraries, the optimised build not done or failed) or optimised (the whole build that replaced a fast
// link). Not used lately: no lookup since idle_min wall minutes (pipe_used), never: none in this run (the boot's
// prebuild made it from the key file or the shared list).
void vk_pipeline_mem(VkPipeMem *m, uint32_t idle_min) {
    memset(m, 0, sizeof *m);
    m->idle_min = idle_min;
    m->cache[0] = __atomic_load_n(&P.held[0], __ATOMIC_RELAXED);
    m->cache[1] = __atomic_load_n(&P.held[1], __ATOMIC_RELAXED);
    m->cache_asked = m->cache[0] || m->cache[1];
    if (!m->cache[0]) m->cache[0] = P.cache ? P.cache_loaded : 0;
    if (!m->cache[1]) m->cache[1] = P.play ? P.play_loaded : 0;
    if (!P.ready) return;
    uint32_t now = __atomic_load_n(&pipe_minute, __ATOMIC_RELAXED);
    plat_mutex_lock_shared(&P.lock);
    for (int i = 0; i < PIPE_BUCKETS; i++)
        for (const Pipe *p = pipes[i]; p; p = p->next) {
            m->ram_pipes += sizeof *p + p->key_size + (p->bindings ? p->info.n_bindings * sizeof *p->bindings : 0);
            int comp = p->info.bind_point == VK_PIPELINE_BIND_POINT_COMPUTE;
            VkPipeline pipe = p->info.pipeline;
            if (!pipe) {
                if (comp) m->comp_none++;
                else m->gfx_none++;
                continue;
            }
            int never = !p->used_min, idle = !never && now - p->used_min >= idle_min;
            if (comp) {
                m->comp++, m->comp_never += never, m->comp_idle += idle;
                continue;
            }
            m->gfx++, m->gfx_never += never, m->gfx_idle += idle;
            if (p->fast && pipe == p->fast) m->gfx_fast++;
            else if (p->optimised) m->gfx_opt++;
            else m->gfx_whole++;
        }
    for (int i = 0; i < LIB_BUCKETS; i++)
        for (const Lib *b = libs[i]; b; b = b->next) {
            m->ram_pipes += sizeof *b + b->size;
            uint32_t kind;
            memcpy(&kind, b->key, sizeof kind);   // every library key starts with its LIB_* kind
            if (!b->pipe) m->lib_none++;
            else if (kind < 4) m->lib[kind]++;
        }
    for (int i = 0; i < LAYOUT_BUCKETS; i++)
        for (const Layout *l = layouts[i]; l; l = l->next) m->layouts++, m->ram_pipes += sizeof *l + l->n * sizeof *l->b;
    m->ram_keys = (uint64_t)K.seen_cap * sizeof *K.seen + (uint64_t)K.pend_cap * sizeof(KeyRec) +
                  (K.file ? K.own_n * sizeof(KeyRec) : 0);
    m->ram_seed = K.file && K.file_n > K.own_n ? (K.file_n - K.own_n) * sizeof(KeyRec) : 0;
    plat_mutex_unlock_shared(&P.lock);
    plat_mutex_lock(&PB.lock);
    m->ram_binaries = (uint64_t)PB.cap * sizeof *PB.slot + PB.pend_cap;
    plat_mutex_unlock(&PB.lock);
}
