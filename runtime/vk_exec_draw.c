// Draws, binds and dispatches of the Vulkan op-stream replay: the strong
// vk_exec_op_draw behind vk_exec.c's weak hook. The bind ops fill the VkExec slots; a draw opens the pass, resolves the
// bound program to translated shaders (the translator's answers come from the bound state), asks vk_pipeline.c for the
// pipeline, pushes (or allocates) set 0 as VkPipeInfo.bindings says and records the dynamic state, vertex and index
// buffers and the draw. A dispatch ends the pass and does the same for the compute shader. UPDATE_UB is a buffer copy
// from transient memory between full barriers.
//
// Per command buffer the replay keeps transient memory (support buffers, copied uniform ranges, generated indices),
// descriptor pools, image views and the storage-buffer ranges the GPU may write; all of it is freed, and the ranges are
// reported with vk_gpu_written, from one completion callback once the submission's fence is done (the write watch
// does not see GPU writes). Nothing here locks: it runs on the submitting thread under vk_sync_submit's lock.
//
// Unbound vertex streams read a zeroed buffer. The support buffer layout is the translator's SupportBuffer.
// Contains code Copyright (c) 2026 jhaizhou, Copyright (c) 2026 Peiikai (MIT; see THIRD_PARTY_NOTICES.md).
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "flight.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "vk.h"

#ifndef NX2NAVI_TONEMAP_STANDIN
#define NX2NAVI_TONEMAP_STANDIN 1   // vk_tonemap.c, see untranslated_dispatch_fallback
#endif
#include "vk_exec.h"
#include "vk_gpa.h"
#include "mesh_fb.h"
#include "vk_mesh_fb.h"
#include "vk_scratch_lru.h"
#include "vk_index_rewrite.h"
#include "platform/platform.h"
#include "stutter_map.h"
#include "vk_dlaa.h"
#include "mesh_emu_spirv.h"

#define MAX_BINDINGS 128
#define MAX_DESC 1024
#define CHUNK_BYTES (4u << 20)
#define ZERO_BYTES 65536u
#define SUPPORT_BYTES 1456u   // the translator's SupportBuffer (std140): alpha_test, is_bgra[8], viewport_inverse, viewport_size, ...;
                              // 1408 pcss and 1424 shadow_spread (translator patches 0010, 0011), 1440..1455 HDR (support_fill)
#define NVN_STAGE_VS 0
#define NVN_STAGE_FS 1
#define NVN_STAGE_CS 5
#define NVN_STAGE_TASK 6   // NVN2
#define NVN_STAGE_MESH 7
// prepare_draw: how long a one-shot draw waits for its shaders and pipeline before it is skipped. Such a draw renders
// into a target that is neither recent nor steady (vk_target_frames.h: a volume, a lookup table, an icon, a target
// made new at an area change); skipped, a target the game draws once stays as it was. A draw into per-frame targets
// whose shaders write memory takes the same wait (writers_wait_on).
// NX2NAVI_ONESHOT_BUDGET (default 1): the one-shot draws of a presented frame wait from that frame's wait pool
// (frame_wait, shared with the full-screen draws), a mesh draw MESH_WAIT_MS at most. Until 0.7.1 each waited up to
// NX2NAVI_ONESHOT_WAIT_MS (30 s) on the replay thread, which is what =0 still does: after a scene load every pass was
// new or not recent, so the first frame waited for one pipeline after another, and for the whole compile of each mesh
// pipeline (0.2 to 1.3 s cold): 5-14 s on an RTX 4080 SUPER with no pipeline recorded, and frames of 0.7 to 1 s at
// area changes of a played install (vk_target_frames.h "Steady targets"), while the pipeline workers sat idle. A
// draw skipped at the end of its wait comes back the next frame its target is drawn, when the target is recent and
// the pipeline is built on a worker like any other; a target drawn only once whose pipeline took longer than the pool
// stays as it was until the game draws it again. With the stage libraries in the driver's cache a fast link takes
// 2 to 6 ms, so the pool covers the lookup tables of a load several times over.
static uint32_t oneshot_wait_ms(void) {
    static int64_t ms = -1;
    if (ms < 0) {
        ms = strtoll(cfg_value("NX2NAVI_ONESHOT_WAIT_MS"), NULL, 10);
        if (ms < 0) ms = 0;
        if (ms != 30000) rt_log("vk_exec: a one-shot draw waits at most %lld ms for its pipeline, then is skipped "
                                "(NX2NAVI_ONESHOT_WAIT_MS)", (long long)ms);
    }
    return (uint32_t)ms;
}
#define ONESHOT_WAIT_MS oneshot_wait_ms()
// What a mesh draw waits for is a few milliseconds when the caches hold it and 200 ms or more when they do not: the
// whole build of a mesh pipeline without stage libraries, which has no fast link, or with them
// (vk_pipeline_mesh_libs) the library of its task and mesh stages, which is most of that build. A longer wait only
// finds out which, and one that has been building for longer than this is a compile: the draws that meet it in the
// frames after wait for nothing (oneshot_ms). Before that rule a first visit waited 8 to 13 ms for each of 17 mesh
// draws over nine frames, up to four in one frame, and every one of them was skipped at the end.
// The wait for a fast link (link_budget) needs the rule only without stage libraries (mesh_whole): with them a mesh
// draw waits there as a vertex draw does, LINK_DRAW_MS from its pipeline's first miss and nothing after.
#define MESH_WAIT_MS 8u
static int oneshot_budget_on(void) {
    static int on = -1;
    if (on < 0) {
        on = rt_knob("ONESHOT_BUDGET", "1") == 1;
        if (!on) rt_log("vk_exec: a one-shot draw waits up to %u ms for its shaders and pipeline, outside the frame's wait "
                        "pool (NX2NAVI_ONESHOT_BUDGET=0)", ONESHOT_WAIT_MS);
    }
    return on;
}

enum { W_PROGRAM, W_SHADER, W_PIPELINE, W_NO_VS, W_INDEX, W_UB, W_BIND, W_SSBO_ALIGN, W_SSBO_SLOT, W_RESTART, W_INDIRECT, W_MESH, W_QUAD,
       W_COUNT };

static void warn_once(int key, const char *fmt, ...) {
    static volatile int32_t seen[W_COUNT];
    if (__atomic_exchange_n(&seen[key], 1, __ATOMIC_RELAXED)) return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    rt_log("vk: %s (logged once)", msg);
}

static void *grow(void *p, int *cap, size_t elem) {
    *cap = *cap ? *cap * 2 : 8;
    p = realloc(p, elem * (size_t)*cap);
    if (!p) rt_fatal("vk: out of memory");
    return p;
}

// ---- resources of one command buffer, freed by its completion callback

typedef struct { VkBuffer buf; VmaAllocation alloc; uint8_t *map; uint64_t size, used; } Chunk;
typedef struct {
    VkImage image;
    VkFormat format;
    VkImageViewType type;
    uint32_t aspect, level0, levels, layer0, layers, swz;
} ViewKey;
_Static_assert(sizeof(ViewKey) == 40, "ViewKey is hashed as bytes: no padding");
typedef struct { uint64_t addr, size; int back; } Range;   // back: written into a VRAM mirror (vk_mem_mirrored)
typedef struct {
    Chunk *chunks;
    int nchunks, cchunks;
    VkDescriptorPool *pools;
    int npools, cpools;
    Range *written;
    int nwritten, cwritten;
} CbRes;

// Descriptor pools and full-size transient chunks go back to these lists when their command buffer completes, instead
// of being destroyed and created again for the next one: each creation is a driver memory allocation (on Windows an
// allocation, a map and a paging-fence wait on the CPU), about ten of them per frame inside the replay.
#define SPARE_POOLS 64
#define SPARE_CHUNKS 16
static PlatMutex spare_lock = PLAT_MUTEX_INIT;
static VkDescriptorPool spare_pool[SPARE_POOLS];
static Chunk spare_chunk[SPARE_CHUNKS];
static int nspare_pool, nspare_chunk;
static volatile int64_t npools_live;   // the replay's descriptor pools in existence, spare ones included (vk_exec_desc_mem)
#define REPLAY_POOL_SETS 256           // sets in each of them (alloc_set)

// State of the command buffer being recorded that the Vulkan command buffer itself holds (DrawCtx.B).
typedef struct {
    VkPipeline pipe[2];                  // bound per bind point: 0 graphics, 1 compute
    VkPipeState st;                      // the state vk_pipeline_set_dynamic last recorded
    int st_valid;
    VkDynShadow dyn;                     // the dynamic state values the command buffer holds (prepare_draw_ex)
    VkBuffer vb[17];                     // vertex buffers last bound
    VkDeviceSize vo[17], vsz[17], vstr[17];
    int vcls[17];                        // their pool class and pool record, for the census
    void *vpool[17];
    NVNbufferRange vin[16];              // the ranges they were resolved from, under vk_mem_gen vgen
    uint64_t vgen;
    int vb_valid;
    VkBuffer ib;                         // index buffer last bound, with its offset and index type (bind_index_buffer)
    VkDeviceSize ib_off;
    VkIndexType ib_type;
    int ib_valid;
    uint8_t support[SUPPORT_BYTES];      // last support buffer and where it lives
    float sup_in[3];                     // the support_fill arguments it was filled from
    VkBuffer sup_buf;
    VkDeviceSize sup_off;
    int sup_valid;
    uint8_t c0[5][VK_C0_BYTES];          // last c0 bank per stage (vertex, fragment, compute, task, mesh) and where it lives
    VkBuffer c0_buf[5];
    VkDeviceSize c0_off[5];
    int c0_valid[5];
    struct { p_bind_buf ssbo[16], ubo[16]; uint32_t dim[3], binst; uint8_t push[1024]; } c0_in[5];   // what vk_c0_fill read for c0[k]
    VkPipelineLayout ds_layout;          // graphics layout the sets below were last pushed or bound with
    VkDescriptorSet ds_bound[2];         // allocated set bound per set number (0: none or pushed since)
    VkDescriptorSet ds_last[2];          // last allocated and written set per set number, contents in DS
    VkDescriptorSetLayout ds_last_layout[2];
    int ds_pushed[2];                    // set number holds the push of DS.psig under ds_layout
    int res_valid;                       // the graphics sets bound hold what the inputs in RS give (bind_resources)
} CbState;

// Contents of B.ds_last (DrawCtx.DS): the set's packed descriptors (VkDescSlot), which with its layout determine every
// write. A draw whose allocated set would hold the same reuses the set instead of allocating and writing one (a per-draw
// cost). psig: the same for the last push per set number; a push equal to it under the same layout is not recorded
// again (the descriptors stay bound across pipeline binds of that layout).
#define DS_SIG_BYTES (MAX_DESC * sizeof(VkDescSlot))
typedef struct { uint32_t n[2], pn[2]; uint8_t sig[2][DS_SIG_BYTES], psig[2][DS_SIG_BYTES]; } DsSig;

// NX2NAVI_BIND_STATS counters (bind_stats).
static struct {
    int on;
    uint64_t draws, bind[2], desc[2], push, push_skip, set_new, set_reuse, ac_miss, same_prog, same_all, bind_skip;
} BS = {-1};

// The inputs of the last graphics bind_resources (bind_sig, DrawCtx.RS) and what its build counted for the traffic
// census, which a skipped build counts again. RS_BYTES bounds the bound-slot bytes a pipeline reads (RsList): per
// binding at most one c0 storage table, otherwise one bound range or handle per descriptor.
#define RS_BYTES (MAX_BINDINGS * sizeof(((VkExec *)0)->ssbo[0]) + MAX_DESC * sizeof(p_bind_buf))
typedef struct { int cls, use; uint64_t bytes; } RsCount;
typedef struct {
    uint32_t hn;                         // words of h the last signature filled; 0: none
    uint64_t h[24];                      // generations, feedback images, support inputs, base instance
    const VkPipeInfo *info;              // the pipeline whose RsList ranges body holds, as of bind op generation gen
    uint64_t gen;
    uint8_t body[RS_BYTES];
    int rec;                             // a graphics build is running: count_use records, scratch copies mark volatile
    int volatile_;                       // the build copied guest bytes into transient memory: a rebuild differs
    RsCount cnt[2 * VK_MCLS_N], pcnt[2 * VK_MCLS_N];
    int ncnt, pncnt, cnt_over;
    int cmds;                            // check mode: pushes, binds and set writes the build recorded
    uint32_t ckn;                        // check mode: the last build's packed descriptors (bytes in ck)
    uint8_t ck[DS_SIG_BYTES];
} RsState;
static int rs_on = -1;                   // NX2NAVI_BIND_SKIP: 1 skip (default), 2 check, 0 off

// The bound-slot bytes of VkExec a pipeline's descriptors read, as ranges merged where they touch, built once per
// pipeline info: c0's storage table, the uniform and storage ranges of the bound slots and the texture and image
// handles. sup and c0: the support inputs and the base instance count too. p: storage buffers whose address the shader
// loads from a user uniform block; the address read from the block counts too. ok 0: never skipped (too many
// descriptors or such buffers).
#define RS_PTRS 32
typedef struct { uint32_t off, len; } RsRange;
typedef struct { int st; const GfxShaderRes *res; } RsPtr;
typedef struct { uint32_t n, bytes; uint8_t sup, c0, ok, np; RsPtr p[RS_PTRS]; RsRange r[]; } RsList;
static nvn_hmap rs_lists;                // VkPipeInfo pointer -> RsList, until shutdown

// Types of the context's caches and memos; each is described where it is used.
#define STAGEM_N 512                     // find_stage
#define SLOTM_N 4096                     // slot_mask_of
#define PB_N 8                           // per-binding descriptor reuse (pb_take)
#define PB_DESC 192
#define PB_G 17
typedef struct {
    uint64_t tex[32], img[32], ttok[32], itok[32];
    uint32_t tgen[32], igen[32];
    uint8_t tt[32], it[32], ifmt[32];
} AcSlots;
typedef struct { uint64_t serial; int8_t idx[6]; uint32_t len[6]; } StageMemo;
typedef struct { uint64_t serial; int st; uint32_t m; } SlotMask;
typedef struct { int cls, use, has; uint64_t n; } PbCount;
typedef struct {
    const VkPipeInfo *info;
    uint64_t g[PB_G];
    uint8_t ok[PB_DESC];        // descriptor j holds a result that may be reused
    uint64_t in[PB_DESC][4];    // the values it was resolved from (pb_key)
    VkDescSlot d[PB_DESC];
    PbCount cnt[PB_DESC];       // what its resolution counted for the census
} PbEntry;
typedef struct {
    const prog_t *prog;
    uint64_t serial;
    uint32_t gen, tex_serial;
    uint64_t tex[2][32], img[2][32];
    VkPipeState pipe;
} MemoKey;

// ---- the recording context
// bind_resources: the sampled-texture descriptors this context resolved, each with the generations and slot token it
// was resolved under: while its slot, the submission and the samplers are as they were (slot_still, as the texture
// descriptor memo TD checks, without the lock) it holds what TD would give, and a hit takes no texture lock. The
// replay workers' chunks waited for that lock at nearly every draw: 6.1 s of waits a minute in bind_resources alone
// (Hyrule Field, 3 workers, NX2NAVI_LOCK_TIME, 2026-11-12).
#define TM_N 512
typedef struct {
    uint64_t h, tok;
    uint32_t gen, ep, sg;
    uint16_t type;
    uint8_t separate, valid;
    VkImage image;
    VkDescriptorImageInfo out;
} TexMemo;

// What the draws, binds and dispatches keep from one call to the next while they record a command buffer: the state
// the command buffer holds (B), the signatures that let a draw skip its descriptor build or push (DS, RS, PB), the
// previous draw's shaders and pipeline (SM, PM), the shader lookup's caches (AC, stagem, slotm) and the submission's
// transient resources (res). A thread reaches the one it records with through dc; every thread starts with dc_main,
// the replay's, which is the only one while the replay records on a single thread. Shared tables (image views,
// shaders, descriptor lists, the texture pool sets, the stand-ins) stay file statics.
typedef struct DrawCtx {
    CbRes *res;                          // transient resources of the submission being recorded (cb_res)
    uint64_t res_fence;                  // the submission they belong to
    CbState B;
    DsSig DS;
    RsState RS;
    const VkPipeInfo *rs_last;           // rs_list's last lookup
    const RsList *rs_last_l;
    uint32_t c0_dim[3], c0_binst;        // the dispatch size and base instance of the draw or dispatch being bound
    uint64_t bind_gen;                   // bumped by every bind op that writes ex->prog, ubo, ssbo, tex or img
    AcSlots AC[8];                       // answers_for: texture and image types per stage
    StageMemo stagem[STAGEM_N];          // find_stage: the shader serving each NVN stage, per program serial
    SlotMask slotm[SLOTM_N];             // slot_mask_of: the slots a (program, stage) translation reads
    int sf_mvprev;                       // shader_for asks for the vertex shader's previous-frame variant (motion)
    int mfb_stage;                       // the mesh fallback binds this NVN stage's resources (nvn_stage_of)
    const GfxShader *mfb_cur_vs;         // the mesh fallback's vertex stage (prepare_draw_ex)
    PbCount *pb_cap;                     // per-binding reuse: where the resolving descriptor's census count goes
    int pb_spoiled;                      // ... and it may not be kept
    PbEntry PB[PB_N];
    PbEntry *pb_e;                       // the build running, NULL when it keeps nothing
    uint64_t pb_key_cur[4];              // pb_take to pb_keep: the descriptor's values
    int pb_key_ok;                       // ... and whether it may be kept at all
    PbCount pb_cnt_cur;
    int mo_ovr_on;                       // per-object motion: mo_ovr replaces uniform bindings (ubo_info)
    VkDescriptorBufferInfo mo_ovr[2][32];   // [vertex, fragment][slot]; buffer 0: not replaced
    struct { int valid, has_fs; MemoKey k; const GfxShader *vs, *fs; } SM;   // the shader memo (shader_memo_on)
    struct { int valid; VkPipeState s; const VkPipeInfo *info; } PM;          // the pipeline memo (pipe_memo_on)
    void *fm;                            // feedback_scan's memo per texture handle (FmEnt[FM_N]), made at first use
    void *mo_vm;                         // mvobj_pick's memo of the motion variant per vertex shader, made at first use
    struct { const void *d; uint64_t ind; VkDeviceSize at; } ma[64];   // mesh_ring_entry: entries filled ahead
    int ma_n, ma_i;                      // ... how many, and the next one a draw takes
    uint32_t ma_brk;                     // ... vk_exec_brk_now when they were filled
    TexMemo TM[TM_N];                    // bind_resources: sampled-texture descriptors (tm_take)
    VkWriteDescriptorSet bind_w[MAX_BINDINGS];   // bind_resources: write structs, packed descriptors, texel views
    VkDescSlot bind_d[MAX_DESC];
    VkBufferView bind_bv[MAX_DESC];
} DrawCtx;
static DrawCtx dc_main = {.bind_gen = 1};
static _Thread_local DrawCtx *dc = &dc_main;

void *vk_exec_draw_ctx_new(void) {
    DrawCtx *c = calloc(1, sizeof *c);
    if (!c) rt_fatal("vk: out of memory (draw context)");
    c->bind_gen = 1;
    return c;
}

void vk_exec_draw_ctx_use(void *c) { dc = c ? c : &dc_main; }

// Tables the chunks of a parallel group (vk_exec.c "replay workers") share, locked only while a chunk records: outside
// groups the submitting thread alone replays, and these lookups run per draw.
static PlatMutex shader_lock = PLAT_MUTEX_INIT;   // shader_tab
static PlatMutex rs_lock = PLAT_MUTEX_INIT;       // rs_lists
static PlatMutex pool_mt = PLAT_MUTEX_INIT;       // pool_sync from a chunk

static void cb_done(void *arg) {   // completion thread
    CbRes *r = arg;
    VkCtx *c = vk_ctx();
    for (int i = 0; i < r->nwritten; i++)
        if (r->written[i].back) vk_gpu_written_back(r->written[i].addr, r->written[i].size);
        else vk_gpu_written(r->written[i].addr, r->written[i].size);
    for (int i = 0; i < r->npools; i++) {
        vkResetDescriptorPool(c->device, r->pools[i], 0);
        plat_mutex_lock(&spare_lock);
        int kept = nspare_pool < SPARE_POOLS;
        if (kept) spare_pool[nspare_pool++] = r->pools[i];
        plat_mutex_unlock(&spare_lock);
        if (!kept) vkDestroyDescriptorPool(c->device, r->pools[i], NULL), __atomic_sub_fetch(&npools_live, 1, __ATOMIC_RELAXED);
    }
    for (int i = 0; i < r->nchunks; i++) {
        plat_mutex_lock(&spare_lock);
        int kept = r->chunks[i].size == CHUNK_BYTES && nspare_chunk < SPARE_CHUNKS;
        if (kept) spare_chunk[nspare_chunk++] = r->chunks[i];
        plat_mutex_unlock(&spare_lock);
        if (!kept) vmaDestroyBuffer(c->vma, r->chunks[i].buf, r->chunks[i].alloc);
    }
    free(r->written), free(r->pools), free(r->chunks), free(r);
}

void vk_exec_draw_new_cb(void) { memset(&dc->B, 0, sizeof dc->B); }

void vk_exec_draw_shaders(const GfxShader **vs, const GfxShader **fs) {
    DrawCtx *const cx = dc;
    *vs = cx->B.st_valid ? cx->B.st.vs : NULL;
    *fs = cx->B.st_valid ? cx->B.st.fs : NULL;
}

// Marks a dispatch about to be recorded (vk.h vk_pass_mark_begin); the caller ends it after the dispatch.
static VkPassMark *dispatch_mark(VkExec *ex, const GfxShader *cs, uint32_t x, uint32_t y, uint32_t z) {
    VkPassMark *m = vk_pass_mark_begin(ex->cb, VK_MARK_DISPATCH, ex->fence);
    if (!m) return NULL;
    m->step = ex->mark_dispatches++;
    m->w = x, m->h = y, m->d = z, m->draws = 0, m->ncolor = 0;
    m->first[0] = cs, m->first[1] = m->last[0] = m->last[1] = NULL;
    return m;
}

static CbRes *cb_res(VkExec *ex) {
    DrawCtx *d = ex->dc;
    if (d->res && d->res_fence == ex->fence) return d->res;
    CbRes *r = calloc(1, sizeof *r);
    if (!r) rt_fatal("vk: out of memory");
    d->res = r, d->res_fence = ex->fence;
    memset(&d->B, 0, sizeof d->B);
    vk_completion_add(ex->fence, cb_done, r);
    return r;
}

static uint8_t *scratch(VkExec *ex, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    CbRes *r = cb_res(ex);
    size = (size + 255) & ~255ull;
    Chunk *c = r->nchunks ? &r->chunks[r->nchunks - 1] : NULL;
    if (!c || c->used + size > c->size) {
        if (r->nchunks == r->cchunks) r->chunks = grow(r->chunks, &r->cchunks, sizeof *r->chunks);
        c = &r->chunks[r->nchunks];
        uint64_t n = size > CHUNK_BYTES ? size : CHUNK_BYTES;
        int reused = 0;
        if (n == CHUNK_BYTES) {
            plat_mutex_lock(&spare_lock);
            if (nspare_chunk) *c = spare_chunk[--nspare_chunk], reused = 1;
            plat_mutex_unlock(&spare_lock);
        }
        if (reused) {
            c->used = 0;
            r->nchunks++;
            *buf = c->buf;
            *off = 0;
            c->used = size;
            return c->map;
        }
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n,
                                 .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        // HOST_COHERENT: the CPU fills these chunks and never flushes them. Intel's integrated GPUs (ANV, Meteor Lake)
        // offer host-cached memory only without coherence, which VMA picks for random host access: the GPU then read
        // what an earlier submission left in the chunk (an UpdateUniformBuffer payload of another draw: a lighting
        // shader's step count of 0x3f800000 instead of 128, and a loop of a billion steps).
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                      .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
        VmaAllocationInfo info;
        VkResult res = vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &c->buf, &c->alloc, &info);
        if (res == VK_ERROR_OUT_OF_DEVICE_MEMORY || res == VK_ERROR_OUT_OF_HOST_MEMORY) {   // once more after a give-back
            uint64_t freed = vk_mem_give_back();
            VkResult again = vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &c->buf, &c->alloc, &info);
            static volatile int32_t told;
            if (plat_inc32(&told) <= 16)
                rt_log("vk: transient buffer of %llu bytes refused (%d): %.1f MiB given back, %s", (unsigned long long)n,
                       (int)res, freed / 1048576.0, again == VK_SUCCESS ? "made at the second try" : "refused again");
            res = again;
        }
        if (res != VK_SUCCESS) rt_fatal("vk: transient buffer of %llu bytes failed (%d)", (unsigned long long)n, (int)res);
        c->map = info.pMappedData, c->size = n, c->used = 0;
        r->nchunks++;
    }
    *buf = c->buf;
    *off = c->used;
    uint8_t *p = c->map + c->used;
    c->used += size;
    return p;
}

static void note_written(VkExec *ex, uint64_t addr, uint64_t size) {
    CbRes *r = cb_res(ex);
    for (int i = 0; i < r->nwritten; i++)
        if (r->written[i].addr == addr && r->written[i].size == size) return;
    if (r->nwritten == r->cwritten) r->written = grow(r->written, &r->cwritten, sizeof *r->written);
    r->written[r->nwritten++] = (Range){addr, size, vk_mem_mirrored(addr, size)};
    vk_mem_gpu_pending(addr, size);
}

// ---- stand-ins for what a binding names but the game did not bind (zeros, a 1x1 image, the default sampler)

enum { DV_1D, DV_1D_ARRAY, DV_2D, DV_2D_ARRAY, DV_3D, DV_CUBE, DV_CUBE_ARRAY, DV_2D_MS, DV_2D_MS_ARRAY, DV_COUNT };

static struct {
    int ready;
    VkBuffer zbuf;
    VmaAllocation zalloc;
    VkBufferView zview;
    VkBuffer sink;                       // ssbo_info: an unresolved storage buffer a shader writes (zbuf stays zero)
    VmaAllocation sink_alloc;
    VkSampler samp, samp_shadow;
    struct { VkImage image; VmaAllocation alloc; VkImageView view, sview; } img[DV_COUNT];
} Z;

static const struct { VkImageType t; VkImageViewType vt; uint32_t layers; VkSampleCountFlagBits s; VkImageCreateFlags f; } dinfo[DV_COUNT] = {
    {VK_IMAGE_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D_ARRAY, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, 6, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 6, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1, VK_SAMPLE_COUNT_4_BIT, 0},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1, VK_SAMPLE_COUNT_4_BIT, 0}};

// A 1x1 stand-in image of view type dinfo[i] in `fmt`, moved to GENERAL and cleared to `clear` by the upload batch (which
// reaches the queue before the submission that uses it). *sview is the view a storage descriptor takes (a cube becomes a
// 2D array). Left uncleared its content was undefined, and a material that read its roughness map through one (a slot
// with no texture yet) could see roughness near 0: on Kakariko's distant cliffs at night the specular term reached
// 3392 in single pixels, which bloom and the temporal upscaler spread into bright white spots.
static void make_dummy(int i, VkFormat fmt, const VkClearColorValue *clear, VkImage *image, VmaAllocation *alloc,
                       VkImageView *view, VkImageView *sview) {
    VkCtx *c = vk_ctx();
    int ms = dinfo[i].s != VK_SAMPLE_COUNT_1_BIT;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = dinfo[i].f, .imageType = dinfo[i].t,
                            .format = fmt, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = dinfo[i].layers,
                            .samples = dinfo[i].s, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     (ms ? 0 : VK_IMAGE_USAGE_STORAGE_BIT),
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ia = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ia, image, alloc, NULL) != VK_SUCCESS) rt_fatal("vk: stand-in image %d failed", i);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *image,
                                .viewType = dinfo[i].vt, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, dinfo[i].layers}};
    if (vkCreateImageView(c->device, &vi, NULL, view) != VK_SUCCESS) rt_fatal("vk: stand-in view failed");
    *sview = *view;
    if (!ms && (dinfo[i].vt == VK_IMAGE_VIEW_TYPE_CUBE || dinfo[i].vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)) {
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        if (vkCreateImageView(c->device, &vi, NULL, sview) != VK_SUCCESS) rt_fatal("vk: stand-in view failed");
    }
    VkImageMemoryBarrier2 bar = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                 .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                 .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                 .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .image = *image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, dinfo[i].layers}};
    VkCommandBuffer cb = vk_upload_begin();
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &bar};
    bar.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT, bar.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier2(cb, &di);
    vkCmdClearColorImage(cb, *image, VK_IMAGE_LAYOUT_GENERAL, clear, 1, &bar.subresourceRange);
    bar.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT, bar.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    bar.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    bar.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier2(cb, &di);
    vk_upload_end();
}

// Sampled stand-ins read as mid grey, opaque: a missing roughness or metalness map gives a middling value, a missing
// two-channel normal map a flat normal, a missing colour map grey. Storage stand-ins (any format, written by shaders that
// have no image) start at zero.
static const VkClearColorValue DUMMY_SAMPLED = {.float32 = {0.5f, 0.5f, 0.5f, 1.0f}};
static const VkClearColorValue DUMMY_STORAGE = {.uint32 = {0, 0, 0, 0}};

// Storage image stand-ins in the format a shader declares (GfxShaderRes.format: the translator's TextureFormat order); Vulkan wants
// the view format to match the declaration. Created on first use.
static const VkFormat tex_format[40] = {
    VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT,
    VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT,
    VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM,
    VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM,
    VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT,
    VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT,
    VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT,
    VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT,
    VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_B10G11R11_UFLOAT_PACK32};
static struct { int used, di; VkFormat fmt; VkImage image; VmaAllocation alloc; VkImageView view, sview; } SD[48];

static VkImageView storage_dummy(int di, int code) {
    VkFormat fmt = code > 0 && code < 40 ? tex_format[code] : VK_FORMAT_R8G8B8A8_UNORM;
    if (fmt == VK_FORMAT_R8G8B8A8_UNORM || dinfo[di].s != VK_SAMPLE_COUNT_1_BIT) return Z.img[di].sview;
    for (int i = 0; i < 48; i++) {
        if (!SD[i].used) {
            SD[i].used = 1, SD[i].di = di, SD[i].fmt = fmt;
            make_dummy(di, fmt, &DUMMY_STORAGE, &SD[i].image, &SD[i].alloc, &SD[i].view, &SD[i].sview);
            return SD[i].sview;
        }
        if (SD[i].di == di && SD[i].fmt == fmt) return SD[i].sview;
    }
    return Z.img[di].sview;
}

static void dummies_init(void) {
    if (Z.ready) return;
    VkCtx *c = vk_ctx();
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = ZERO_BYTES,
                             .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};   // the memset below is not flushed
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &ai, &Z.zbuf, &Z.zalloc, &info) != VK_SUCCESS) rt_fatal("vk: zero buffer failed");
    memset(info.pMappedData, 0, ZERO_BYTES);
    VkBufferViewCreateInfo bv = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = Z.zbuf,
                                 .format = VK_FORMAT_R32G32B32A32_SFLOAT, .range = ZERO_BYTES};
    if (vkCreateBufferView(c->device, &bv, NULL, &Z.zview) != VK_SUCCESS) rt_fatal("vk: zero buffer view failed");
    VkBufferCreateInfo sbi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = ZERO_BYTES,
                              .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo sai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateBuffer(c->vma, &sbi, &sai, &Z.sink, &Z.sink_alloc, NULL) != VK_SUCCESS) rt_fatal("vk: sink buffer failed");
    samp_par sp;
    memset(&sp, 0, sizeof sp);
    sp.lod_max = 1000.0f;
    Z.samp = vk_sampler_for(&sp);
    sp.cmp_mode = 1, sp.cmp_func = NVN_DEPTH_FUNC_ALWAYS;
    Z.samp_shadow = vk_sampler_for(&sp);

    for (int i = 0; i < DV_COUNT; i++)
        make_dummy(i, VK_FORMAT_R8G8B8A8_UNORM, &DUMMY_SAMPLED, &Z.img[i].image, &Z.img[i].alloc, &Z.img[i].view, &Z.img[i].sview);
    Z.ready = 1;
}

static void twins_release(VkCtx *c);

// Releases the stand-ins and the depth twins. The device must be idle; the game path leaves them to process exit like
// the pipelines.
static void mesh_ring_release(VkCtx *c);
static void pool_marks_release(VkCtx *c);

static void pose_shutdown(void);
static void skin_shutdown(void);
void vk_exec_draw_shutdown(void) {
    pose_shutdown();
    skin_shutdown();
    vk_meshfb_shutdown();
    if (vk_ctx()) mesh_ring_release(vk_ctx()), pool_marks_release(vk_ctx());   // made without the stand-ins below too
    if (!Z.ready) return;
    VkCtx *c = vk_ctx();
    twins_release(c);
    for (int i = 0; i < DV_COUNT; i++) {
        if (Z.img[i].sview != Z.img[i].view) vkDestroyImageView(c->device, Z.img[i].sview, NULL);
        vkDestroyImageView(c->device, Z.img[i].view, NULL);
        vmaDestroyImage(c->vma, Z.img[i].image, Z.img[i].alloc);
    }
    for (int i = 0; i < 48 && SD[i].used; i++) {
        if (SD[i].sview != SD[i].view) vkDestroyImageView(c->device, SD[i].sview, NULL);
        vkDestroyImageView(c->device, SD[i].view, NULL);
        vmaDestroyImage(c->vma, SD[i].image, SD[i].alloc);
    }
    memset(SD, 0, sizeof SD);
    nvn_hm_clear(&rs_lists, free);
    dc_main.rs_last = NULL, dc_main.rs_last_l = NULL;   // the replay's context: no other records now
    for (int i = 0; i < nspare_pool; i++) vkDestroyDescriptorPool(c->device, spare_pool[i], NULL);
    __atomic_sub_fetch(&npools_live, nspare_pool, __ATOMIC_RELAXED);
    for (int i = 0; i < nspare_chunk; i++) vmaDestroyBuffer(c->vma, spare_chunk[i].buf, spare_chunk[i].alloc);
    nspare_pool = nspare_chunk = 0;
    vk_usnap_shutdown();
    vkDestroyBufferView(c->device, Z.zview, NULL);
    vmaDestroyBuffer(c->vma, Z.zbuf, Z.zalloc);
    if (Z.sink) vmaDestroyBuffer(c->vma, Z.sink, Z.sink_alloc);
    memset(&Z, 0, sizeof Z);
}

// ---- resource types

static VkImageViewType res_view_type(uint16_t t) {
    int arr = t & 0x100;
    switch (t & 0xff) {
    case 1: return arr ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
    case 4: return VK_IMAGE_VIEW_TYPE_3D;
    case 5: return arr ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
    default: return arr ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}

static int dummy_index(VkImageViewType vt, int ms) {
    switch (vt) {
    case VK_IMAGE_VIEW_TYPE_1D: return DV_1D;
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY: return DV_1D_ARRAY;
    case VK_IMAGE_VIEW_TYPE_3D: return DV_3D;
    case VK_IMAGE_VIEW_TYPE_CUBE: return DV_CUBE;
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return DV_CUBE_ARRAY;
    case VK_IMAGE_VIEW_TYPE_2D_ARRAY: return ms ? DV_2D_MS_ARRAY : DV_2D_ARRAY;
    default: return ms ? DV_2D_MS : DV_2D;
    }
}

// GFX_TEX_* of a bound texture slot (the translator's tex_type / img_type answers).
static int gfx_type_of(const VkTexImage *img, const view_t *v, int hv) {
    if (hv && v->set_target) return vk_pipeline_tex_type(v->target);
    int ms = img->samples > 1;
    switch (img->view_type) {
    case VK_IMAGE_VIEW_TYPE_1D: return GFX_TEX_1D;
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY: return GFX_TEX_1D_ARRAY;
    case VK_IMAGE_VIEW_TYPE_2D_ARRAY: return ms ? GFX_TEX_2D_MS_ARRAY : GFX_TEX_2D_ARRAY;
    case VK_IMAGE_VIEW_TYPE_3D: return GFX_TEX_3D;
    case VK_IMAGE_VIEW_TYPE_CUBE: return GFX_TEX_CUBE;
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return GFX_TEX_CUBE_ARRAY;
    default: return ms ? GFX_TEX_2D_MS : GFX_TEX_2D;
    }
}

static VkComponentSwizzle swz_of(uint8_t s) {
    switch (s) {
    case NVN_TEXTURE_SWIZZLE_ZERO: return VK_COMPONENT_SWIZZLE_ZERO;
    case NVN_TEXTURE_SWIZZLE_ONE: return VK_COMPONENT_SWIZZLE_ONE;
    case NVN_TEXTURE_SWIZZLE_R: return VK_COMPONENT_SWIZZLE_R;
    case NVN_TEXTURE_SWIZZLE_G: return VK_COMPONENT_SWIZZLE_G;
    case NVN_TEXTURE_SWIZZLE_B: return VK_COMPONENT_SWIZZLE_B;
    default: return VK_COMPONENT_SWIZZLE_A;
    }
}

// A view in STENCIL mode samples the stencil plane (TextureViewSetDepthStencilMode); otherwise depth wins. The
// translator declares every sampled image float and the shader takes the raw bits, as for integer formats.
VkImageAspectFlags vk_exec_view_aspect(const VkTexImage *img, const view_t *v, int hv) {
    if (hv && v->set_dsmode && v->dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL && (img->aspect & VK_IMAGE_ASPECT_STENCIL_BIT))
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    return img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT     ? VK_IMAGE_ASPECT_DEPTH_BIT
           : img->aspect & VK_IMAGE_ASPECT_STENCIL_BIT ? VK_IMAGE_ASPECT_STENCIL_BIT
                                                       : VK_IMAGE_ASPECT_COLOR_BIT;
}

// Image views of registered textures, kept for their image's lifetime: made per command buffer they were created again
// every frame (vkCreateImageView took about 8% of the fight replay on the 7900X). Used under the texture lock, as
// vk_exec_image_gone is (destroy_tex), which hands the views of a destroyed image to the deferred release with it.
typedef struct ViewNode { struct ViewNode *next; ViewKey k; VkImageView view; } ViewNode;
static nvn_hmap view_of;                 // ViewKey -> VkImageView
static nvn_hmap views_by_image;          // VkImage -> its first ViewNode

// Integer formats a texture view may give an image (nvnTextureViewSetFormat): the Vulkan format and the translator's
// TextureFormat code (gfx_shader.c fmt_names) the translator declares a storage image with. A shader that packs texels
// itself writes raw words through such a view (Ocarina's last post-process pass stores R11G11B10 bits through an R32UI
// view of an R11G11B10F target); through a view of the texture's own format the store converted them (a black picture
// with red blobs). 0: not an integer format.
static int int_view_format(uint32_t nvn, VkFormat *vk) {
    static const struct { uint16_t nvn; uint8_t code; VkFormat vk; } T[] = {
        {NVN_FORMAT_R8UI, 3, VK_FORMAT_R8_UINT}, {NVN_FORMAT_R8I, 4, VK_FORMAT_R8_SINT},
        {NVN_FORMAT_R16UI, 8, VK_FORMAT_R16_UINT}, {NVN_FORMAT_R16I, 9, VK_FORMAT_R16_SINT},
        {NVN_FORMAT_R32UI, 11, VK_FORMAT_R32_UINT}, {NVN_FORMAT_R32I, 12, VK_FORMAT_R32_SINT},
        {NVN_FORMAT_RG8UI, 15, VK_FORMAT_R8G8_UINT}, {NVN_FORMAT_RG8I, 16, VK_FORMAT_R8G8_SINT},
        {NVN_FORMAT_RG16UI, 20, VK_FORMAT_R16G16_UINT}, {NVN_FORMAT_RG16I, 21, VK_FORMAT_R16G16_SINT},
        {NVN_FORMAT_RG32UI, 23, VK_FORMAT_R32G32_UINT}, {NVN_FORMAT_RG32I, 24, VK_FORMAT_R32G32_SINT},
        {NVN_FORMAT_RGBA8UI, 27, VK_FORMAT_R8G8B8A8_UINT}, {NVN_FORMAT_RGBA8I, 28, VK_FORMAT_R8G8B8A8_SINT},
        {NVN_FORMAT_RGBA16UI, 32, VK_FORMAT_R16G16B16A16_UINT}, {NVN_FORMAT_RGBA16I, 33, VK_FORMAT_R16G16B16A16_SINT},
        {NVN_FORMAT_RGBA32UI, 35, VK_FORMAT_R32G32B32A32_UINT}, {NVN_FORMAT_RGBA32I, 36, VK_FORMAT_R32G32B32A32_SINT},
        {NVN_FORMAT_RGB10A2UI, 38, VK_FORMAT_A2B10G10R10_UINT_PACK32}};
    for (size_t i = 0; i < sizeof T / sizeof *T; i++)
        if (T[i].nvn == nvn) {
            if (vk) *vk = T[i].vk;
            return T[i].code;
        }
    return 0;
}

// Bytes per texel of the formats a storage view may reinterpret (int_view_format's and the 32/64/128-bit float and
// unorm ones they alias); 0 unknown.
static int texel_bytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8_UNORM: return 1;
    case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8_UNORM: return 2;
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
        return 4;
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: return 8;
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 0;
    }
}

// Images written this frame through an integer storage view (storage_view_format), in the order of their writes (seq):
// Ocarina's last post-process pass stores shared-exponent RGB9E5 texels that way into its R11G11B10F scene target, raw
// words the upscaler that reads them (tensor-core, untranslatable) decodes itself, and the fallback blit must too
// (raw_9e5). Now and then (1 of 529 frames sampled in a field) the same pass writes them into the RGB9E5 image at that
// address instead, as native texels (raw_native_9e5); the R11G11B10F image there then holds an older picture.
static struct { VkImage img; uint64_t frame, seq; } RAW[8];
static uint64_t raw_seq;

static void raw_note(VkImage img) {
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    int k = 0;
    raw_seq++;
    for (int i = 0; i < 8; i++) {
        if (RAW[i].img == img) { RAW[i].frame = f, RAW[i].seq = raw_seq; return; }
        if (RAW[i].frame < RAW[k].frame) k = i;
    }
    RAW[k].img = img, RAW[k].frame = f, RAW[k].seq = raw_seq;
}

// The order of img's last write through an integer view, 0 when it had none this frame or the one before.
static uint64_t raw_written(VkImage img) {
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    for (int i = 0; i < 8; i++)
        if (RAW[i].img == img && now - RAW[i].frame <= 1) return RAW[i].seq;
    return 0;
}

static int raw_9e5(VkImage img, VkFormat f) { return f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && raw_written(img); }
static int raw_native_9e5(VkImage img, VkFormat f) { return f == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 && raw_written(img); }

// The image that holds this frame's post-process picture at t's place in memory, NULL when none was written: the
// R11G11B10F image written as raw RGB9E5 words, or t itself when t is the RGB9E5 image and was written as native texels,
// whichever was written last. The picture resolves as an RGB9E5 texture there, and that image is otherwise one nothing
// renders into, stale or never initialised. Looked up live (texture lock held), as an image noted in RAW may be gone.
static const VkTexImage *raw_9e5_twin(const VkTexImage *t) {
    const VkTexImage *o = vk_texture_at_locked(t, VK_FORMAT_B10G11R11_UFLOAT_PACK32);
    uint64_t ro = o && raw_9e5(o->image, o->format) ? raw_written(o->image) : 0;
    uint64_t rt = raw_native_9e5(t->image, t->format) ? raw_written(t->image) : 0;
    return rt > ro ? t : ro ? o : NULL;
}

// vk_dump.c: 1 when img was written as raw RGB9E5 words this frame (dump it decoded as such).
int vk_exec_raw_9e5(VkImage img, VkFormat f) { return raw_9e5(img, f); }

// The format a storage view of img takes: the view's integer format when it sets one of the image's texel size (the
// image is MUTABLE_FORMAT, vk_texture.c), else the image's own.
static VkFormat storage_view_format(const VkTexImage *img, const view_t *v, int hv) {
    VkFormat f;
    if (hv && v->set_format && int_view_format(v->format, &f) && texel_bytes(f) && texel_bytes(f) == texel_bytes(img->format)) {
        if (img->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || img->format == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32)
            raw_note(img->image);
        return f;
    }
    return img->format;
}

// An image view of a registered texture for a shader resource of type `rtype` (GfxShaderRes.type); VK_NULL_HANDLE when
// the image cannot serve it (the caller binds a stand-in). Texture lock held.
static VkImageView get_view(VkExec *ex, const VkTexImage *img, const view_t *v, int hv, uint16_t rtype, int storage) {
    VkImageViewType vt = res_view_type(rtype);
    int ms = (rtype & 0x200) != 0;
    if ((img->samples > 1) != ms || (storage && ms)) return VK_NULL_HANDLE;
    if (storage && !(img->usage & VK_IMAGE_USAGE_STORAGE_BIT)) return VK_NULL_HANDLE;
    int cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    int ok = 0;
    switch (vt) {
    case VK_IMAGE_VIEW_TYPE_1D:
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY: ok = img->type == VK_IMAGE_TYPE_1D; break;
    case VK_IMAGE_VIEW_TYPE_3D:
        ok = img->type == VK_IMAGE_TYPE_3D;
        if (!ok && !storage && !ms && img->type == VK_IMAGE_TYPE_2D) {   // a flat texture under a 3D sample: its 3D copy
            const VkTexImage *f = vk_texture_flat3d_locked(img, hv && v->set_layers ? v->min_layer : 0);
            if (f) img = f, ok = 1;
        }
        break;
    case VK_IMAGE_VIEW_TYPE_CUBE:
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
        ok = img->type == VK_IMAGE_TYPE_2D && (img->view_type == VK_IMAGE_VIEW_TYPE_CUBE || img->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY);
        break;
    default: ok = img->type == VK_IMAGE_TYPE_2D; break;
    }
    if (!ok) return VK_NULL_HANDLE;
    uint32_t l0 = hv && v->set_levels ? v->base_level : 0;
    uint32_t nl;
    if (img->skip) {
        // Texture detail (vk_texture.c): the image was made without the game's top img->skip levels, so the view's
        // levels [l0, end) of the game are the image's [l0 - skip, end - skip). A view that names only levels the image
        // lacks shows the first one it has: the nearest content, where a stand-in would show none.
        uint32_t sk = img->skip, all = img->levels + sk;
        if (l0 >= all) return VK_NULL_HANDLE;
        uint32_t end = hv && v->set_levels && v->nlevels ? l0 + v->nlevels : all;
        if (end > all) end = all;
        l0 = l0 > sk ? l0 - sk : 0;
        end = end > sk ? end - sk : 1;
        nl = end > l0 ? end - l0 : 1;
    } else {
        if (l0 >= img->levels) return VK_NULL_HANDLE;
        nl = hv && v->set_levels && v->nlevels ? v->nlevels : img->levels - l0;
        if (l0 + nl > img->levels) nl = img->levels - l0;
    }
    if (storage) nl = 1;
    uint32_t y0 = 0, ny = 1;
    if (vt != VK_IMAGE_VIEW_TYPE_3D) {
        y0 = hv && v->set_layers ? v->min_layer : 0;
        if (y0 >= img->layers) return VK_NULL_HANDLE;
        uint32_t rest = hv && v->set_layers && v->nlayers ? v->nlayers : img->layers - y0;
        if (y0 + rest > img->layers) rest = img->layers - y0;
        if (vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_2D) ny = 1;
        else if (vt == VK_IMAGE_VIEW_TYPE_CUBE) ny = 6;
        else if (vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) ny = rest / 6 * 6;
        else ny = rest;
        if (!ny || y0 + ny > img->layers) return VK_NULL_HANDLE;
    }
    if (storage && cube) vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    uint32_t aspect = vk_exec_view_aspect(img, v, hv);
    uint8_t sw[4] = {NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_G, NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_A};
    if (!storage) {   // view selectors replace the texture's, then the format remap
        memcpy(sw, hv && v->set_swizzle ? v->swz : img->swz, 4);
        if (img->alpha_one || img->chan[0])
            for (int i = 0; i < 4; i++) {
                if (img->alpha_one && sw[i] == NVN_TEXTURE_SWIZZLE_A) sw[i] = NVN_TEXTURE_SWIZZLE_ONE;
                else if (img->chan[0] && sw[i] >= NVN_TEXTURE_SWIZZLE_R && sw[i] <= NVN_TEXTURE_SWIZZLE_A)
                    sw[i] = img->chan[sw[i] - NVN_TEXTURE_SWIZZLE_R];
            }
    }
    uint32_t swz = (uint32_t)sw[0] | (uint32_t)sw[1] << 8 | (uint32_t)sw[2] << 16 | (uint32_t)sw[3] << 24;
    VkFormat vf = storage ? storage_view_format(img, v, hv) : img->format;
    ViewKey k = {img->image, vf, vt, aspect, l0, nl, y0, ny, swz};
    if (!view_of.klen) nvn_hm_init(&view_of, sizeof(ViewKey)), nvn_hm_init(&views_by_image, sizeof(VkImage));
    VkImageView found = (VkImageView)nvn_hm_get(&view_of, &k);
    if (found) return found;
    VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img->image, .viewType = vt,
                                .format = vf,
                                .components = {swz_of(sw[0]), swz_of(sw[1]), swz_of(sw[2]), swz_of(sw[3])},
                                .subresourceRange = {aspect, l0, nl, y0, ny}};
    VkImageViewUsageCreateInfo use = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                      .usage = VK_IMAGE_USAGE_SAMPLED_BIT};
    if (img->block_views) ci.pNext = &use;   // the image's COLOR_ATTACHMENT usage is its block views' (vk_texture.c)
    VkImageView view;
    if (vkCreateImageView(vk_ctx()->device, &ci, NULL, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    ViewNode *n = malloc(sizeof *n);
    if (!n) rt_fatal("vk: out of memory");
    *n = (ViewNode){nvn_hm_get(&views_by_image, &img->image), k, view};
    nvn_hm_put(&views_by_image, &img->image, n);
    nvn_hm_put(&view_of, &k, (void *)view);
    return view;
}

// An attachment view of a registered texture, kept with its sampled views for the image's lifetime: made per command
// buffer, every render target's views were created again each frame (vkCreateImageView under vk_exec_pass_begin, 2%
// of the fight replay on the 7900X). Attachment keys carry a swizzle word no sampled key has (those hold four NVN
// selectors, each below 8). The texture lock guards the tables, as for get_view; vk_exec_pass_begin may already hold it.
VkImageView vk_exec_target_view(const VkTexImage *img, VkFormat format, VkImageViewType type, uint32_t aspect,
                                uint32_t level, uint32_t base, uint32_t layers, int blocks) {
    vk_texture_lock();   // destroy_tex sets gone and purges the image's views with it held
    ViewKey k = {img->image, format, type, aspect, level, 1, base, layers, blocks ? 0xfffffffeu : 0xffffffffu};
    if (!view_of.klen) nvn_hm_init(&view_of, sizeof(ViewKey)), nvn_hm_init(&views_by_image, sizeof(VkImage));
    // A texture destroyed while a command buffer or DLSS still held it (img->gone): its image lives on until the GPU is
    // past the submission being recorded (vk_defer_image), so a view for this use is made, and goes the same way. Cached,
    // it outlived the image: Vulkan hands the handle to a later image, which then got the view of the dead image's memory
    // and the GPU faulted reading it at the start of a pass ("read invalid", a 128x128 scene capture, #177).
    VkImageView view = img->gone ? VK_NULL_HANDLE : (VkImageView)nvn_hm_get(&view_of, &k);
    if (!view) {
        VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img->image,
                                    .viewType = type, .format = format, .subresourceRange = {aspect, level, 1, base, layers}};
        VkImageViewUsageCreateInfo use = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                          .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
        if (blocks) ci.pNext = &use;   // not the BC image's sampling and transfer usages
        if (vkCreateImageView(vk_ctx()->device, &ci, NULL, &view) != VK_SUCCESS)
            rt_fatal("vk: render target view (format %d) could not be created", (int)format);
        if (img->gone) {
            vk_defer_view(view);
            vk_texture_unlock();
            return view;
        }
        ViewNode *n = malloc(sizeof *n);
        if (!n) rt_fatal("vk: out of memory");
        *n = (ViewNode){nvn_hm_get(&views_by_image, &img->image), k, view};
        nvn_hm_put(&views_by_image, &img->image, n);
        nvn_hm_put(&view_of, &k, (void *)view);
    }
    vk_texture_unlock();
    return view;
}

int vk_exec_image_gone(VkImage image) {
    vk_exec_bc_gone(image);
    vk_tonemap_forget(image);   // the stand-in's views of a game image it read as it is
    vk_hdr_image_gone(image);
    if (!views_by_image.klen) return 0;
    int purged = 0;
    for (ViewNode *n = nvn_hm_del(&views_by_image, &image), *next; n; n = next, purged++) {
        next = n->next;
        nvn_hm_del(&view_of, &n->k);
        vk_defer_view(n->view);
        free(n);
    }
    return purged;
}

// ---- program to shaders

// (program, stage, answers) -> shader, 4-way set-associative, most recently used first. A miss costs a SHA-256 of the
// whole shader code (gfx_shader.c req_init), slow on CPUs without SHA instructions: a field frame looks up ~3000 and
// the 2048-slot direct-mapped table this replaced missed 3% of them on conflicts.
#define SHADER_SETS 2048
#define SHADER_WAYS 4
typedef struct { uint64_t serial; int stage; int used; GfxShaderAnswers a; const GfxShader *s; } ShaderEnt;
static ShaderEnt shader_tab[SHADER_SETS][SHADER_WAYS];

// Stage and code length of a control section. NVN1 (0x880 bytes): stage at +0x714, length at +0x704, c1 offset at
// +0x700, workgroup size at +0x748. NVN2 (Switch 2, 0xe0 bytes, magic 0x98761234 at +0): stage in bits 16+ of +0x54,
// length at +0x38, workgroup size at +0x58, no c1 part (gfx_shader.c, "NVN2").
static int ctl_nvn2(const uint8_t *c) {
    uint32_t magic;
    memcpy(&magic, c, 4);
    return magic == 0x98761234u;
}
static void ctl_stage_len(const uint8_t *c, uint32_t *st, uint32_t *len) {
    if (ctl_nvn2(c)) {
        uint32_t c1s, c1e;
        memcpy(st, c + 0x54, 4), *st >>= 16;
        memcpy(len, c + 0x38, 4);
        // NVN2: the c1 constants (BRX jump tables) follow the code from +0x40 to +0x44, in the same storage; the
        // length covers them so gfx_shader.c req_init sends them (with the code length alone it never could)
        memcpy(&c1s, c + 0x40, 4), memcpy(&c1e, c + 0x44, 4);
        if (c1s >= *len && c1e > c1s && c1e - *len < (1u << 20)) *len = c1e;
    } else {
        memcpy(st, c + 0x714, 4);
        memcpy(len, c + 0x704, 4);
    }
}
static uint32_t ctl_c1(const uint8_t *c, uint32_t len) {   // offset of the c1 constants (len: none)
    uint32_t c1 = len;
    if (!ctl_nvn2(c)) memcpy(&c1, c + 0x700, 4);
    else memcpy(&c1, c + 0x40, 4);   // NVN2: after the code and its padding, up to +0x44
    return c1;
}
static const uint8_t *ctl_local(const uint8_t *c) { return c + (ctl_nvn2(c) ? 0x58 : 0x748); }

static int find_stage_scan(const prog_t *p, int nvn_stage, const uint8_t **ctl, const uint8_t **code, uint32_t *len) {
    for (uint32_t i = 0; i < p->nstage && i < 6; i++) {
        const uint8_t *c = p->sd[i].control;
        if (!c || !p->sd[i].data) continue;
        uint32_t st;
        ctl_stage_len(c, &st, len);
        if ((int)st != nvn_stage) continue;
        *ctl = c, *code = (const uint8_t *)(uintptr_t)p->sd[i].data;
        return 1;
    }
    return 0;
}

// Which shader of a program serves each NVN stage, and its code length, per program serial (new at every
// nvnProgramSetShaders): the scan reads two words of every stage's control block, cold guest cache lines, and runs
// several times a draw. Direct-mapped (DrawCtx.stagem); a collision only scans again.
static int find_stage(DrawCtx *cx, const prog_t *p, int nvn_stage, const uint8_t **ctl, const uint8_t **code, uint32_t *len) {
    if (!p->serial || (unsigned)nvn_stage >= 6) return find_stage_scan(p, nvn_stage, ctl, code, len);
    StageMemo *m = &cx->stagem[p->serial * 0x9E3779B97F4A7C15ull >> 55];
    if (m->serial != p->serial) {
        m->serial = p->serial;
        memset(m->idx, -1, sizeof m->idx);
        for (uint32_t i = 0; i < p->nstage && i < 6; i++) {
            const uint8_t *c = p->sd[i].control;
            if (!c || !p->sd[i].data) continue;
            uint32_t st, len;
            ctl_stage_len(c, &st, &len);
            if (st < 6 && m->idx[st] < 0) m->idx[st] = (int8_t)i, m->len[st] = len;
        }
    }
    int i = m->idx[nvn_stage];
    if (i < 0) return 0;
    *ctl = p->sd[i].control, *code = (const uint8_t *)(uintptr_t)p->sd[i].data, *len = m->len[nvn_stage];
    return 1;
}

// Texture and image types of a stage's first 32 slots, each kept with the handle and the generation and slot token it
// was looked up under: a slot is looked up again, a hash probe, only when the handle changed or its slot may resolve
// otherwise (slot_still). DrawCtx.AC, per stage.

// vk_texture_slot_token of the slot a texture or image handle names; an empty handle always resolves to nothing.
static uint64_t token_of(uint64_t h) { return h ? vk_texture_slot_token((int)(h & 0xfffff)) : 1; }

// Whether what handle h resolved to under the global generation *egen and its slot token etok (both read before that
// lookup) still holds at global generation gen: nothing at all changed since, or not its slot (the token, read only when
// the global generation moved, about every 16 draws). A token hit moves *egen to gen.
static int slot_still(uint32_t *egen, uint64_t etok, uint64_t h, uint32_t gen) {
    if (*egen == gen) return 1;
    uint64_t t = token_of(h);
    if (!t || t != etok) return 0;
    *egen = gen;
    return 1;
}
static int shader_memo_on(void);

// The slots of (program, stage) whose types its translation reads (gfx_shader_slot_mask), learned from its first
// shader; ~0u while unknown. The types of other slots select the same shader, so answers_for leaves them zero instead
// of looking up all 64 handles. Direct-mapped by serial and stage (DrawCtx.slotm); a collision only relearns.
static uint32_t slotm_idx(uint64_t serial, int st) { return (uint32_t)((serial * 8 + (uint64_t)st) * 0x9E3779B97F4A7C15ull >> 52); }

static uint32_t slot_mask_of(DrawCtx *cx, uint64_t serial, int st) {
    const SlotMask *e = &cx->slotm[slotm_idx(serial, st)];
    return serial && e->serial == serial && e->st == st ? e->m : ~0u;
}

static void slot_mask_learn(DrawCtx *cx, uint64_t serial, int st, const GfxShader *s) {
    if (!serial || slot_mask_of(cx, serial, st) != ~0u) return;
    cx->slotm[slotm_idx(serial, st)] = (SlotMask){serial, st, gfx_shader_slot_mask(s)};
}

static uint8_t type_of_handle(uint64_t h) {   // texture lock held
    view_t v;
    int hv;
    const VkTexImage *img = h ? vk_texture_by_id_locked((int)(h & 0xfffff), &v, &hv) : NULL;
    return img ? (uint8_t)gfx_type_of(img, &v, hv) : 0;
}

// The img_format answer of an image handle: the integer format its view gives it (int_view_format, as get_view binds
// it), else 0 (the translator's default, a float format). Texture lock held.
static uint8_t format_of_handle(uint64_t h) {
    view_t v;
    int hv;
    const VkTexImage *img = h ? vk_texture_by_id_locked((int)(h & 0xfffff), &v, &hv) : NULL;
    VkFormat f = VK_FORMAT_UNDEFINED;
    static int logged;
    if (img && hv && v.set_format && int_view_format(v.format, NULL) && logged < 8)
        logged++, rt_log("vk: image view format: handle %#llx view nvn format %#x on vk format %d -> %d",
                         (unsigned long long)h, v.format, img->format, int_view_format(v.format, NULL));
    if (!img || !hv || !v.set_format || !int_view_format(v.format, &f) || texel_bytes(f) != texel_bytes(img->format) || !texel_bytes(f))
        return 0;
    return (uint8_t)int_view_format(v.format, NULL);
}

// The answers of stage st for the slots in mask; the types of other slots stay zero.
static void answers_for(VkExec *ex, int st, const uint8_t *ctl, uint32_t mask, GfxShaderAnswers *a) {
    memset(a, 0, sizeof *a);
    if (st == NVN_STAGE_VS) vk_pipeline_answers(&ex->pipe, a, NULL);
    else if (st == NVN_STAGE_FS) vk_pipeline_answers(&ex->pipe, NULL, a);
    else
        for (int k = 0; k < 3; k++) {
            uint32_t l;
            memcpy(&l, ctl_local(ctl) + 4 * k, 4);
            a->local[k] = (uint16_t)(l ? l : 1);
        }
    AcSlots *c = &ex->dc->AC[st];
    // The generation and tokens are read before the lookups (one that creates an entry changes them) and unlocked: a
    // registration racing with them is ordered after this draw, and a slot stored under older ones is only looked up
    // again.
    uint32_t gen = vk_texture_slots_gen();
    int locked = 0, check = shader_memo_on() == 2;
    for (uint32_t m = mask; m; m &= m - 1) {
        int slot = __builtin_ctz(m);
        uint64_t h = ex->tex[st][slot], hi = ex->img[st][slot];
        int tmiss = c->tex[slot] != h || !c->ttok[slot] || !slot_still(&c->tgen[slot], c->ttok[slot], h, gen);
        int imiss = c->img[slot] != hi || !c->itok[slot] || !slot_still(&c->igen[slot], c->itok[slot], hi, gen);
        uint64_t tt = tmiss ? token_of(h) : 0, it = imiss ? token_of(hi) : 0;
        if ((tmiss || imiss || check) && !locked) vk_texture_lock(), locked = 1, BS.ac_miss++;
        if (tmiss) c->tt[slot] = type_of_handle(h), c->tex[slot] = h, c->ttok[slot] = tt, c->tgen[slot] = gen;
        if (imiss)
            c->it[slot] = type_of_handle(hi), c->ifmt[slot] = format_of_handle(hi), c->img[slot] = hi, c->itok[slot] = it,
            c->igen[slot] = gen;
        if (check && (type_of_handle(h) != c->tt[slot] || type_of_handle(hi) != c->it[slot]))   // NX2NAVI_SHADER_MEMO=check
            rt_fatal("vk_exec: kept texture types of stage %d slot %d differ from a lookup", st, slot);
        a->tex_type[slot] = c->tt[slot], a->img_type[slot] = c->it[slot], a->img_format[slot] = c->ifmt[slot];
    }
    if (locked) vk_texture_unlock();
}

// *present: the program has this stage. Returns NULL when it has not or the translation failed. pending (graphics
// stages, vk_pipeline_async_on): a shader not translated yet is queued and NULL returned with *pending = 1. await_ms:
// a shader queued earlier is waited for instead, that long at most (gfx_shader_await; UINT32_MAX: until it is done),
// and *pending = 1 says the wait ended first.
// DrawCtx.sf_mvprev: shader_for asks for the vertex shader's last-frame variant (per-object motion).

// Probes only (mesh_fb_probe.c): the shader of the program stage whose control section is ctl, in place of a
// translation. The probes build their programs from modules made here, which no translator has an input for.
static struct { const uint8_t *ctl; const GfxShader *s; } probe_sh[16];
static int n_probe_sh;
void vk_exec_probe_shader(const uint8_t *ctl, const GfxShader *s) {
    if (n_probe_sh < 16) probe_sh[n_probe_sh].ctl = ctl, probe_sh[n_probe_sh].s = s, n_probe_sh++;
}
static const GfxShader *shader_for(VkExec *ex, int st, int *present, int *pending, uint32_t await_ms) {
    const uint8_t *ctl, *code;
    uint32_t len;
    *present = 0;
    if (pending) *pending = 0;
    if (!ex->prog || !find_stage(ex->dc, ex->prog, st, &ctl, &code, &len)) return NULL;
    *present = 1;
    for (int i = 0; i < n_probe_sh; i++)
        if (probe_sh[i].ctl == ctl) return probe_sh[i].s;
    GfxShaderAnswers a;
    uint64_t serial = ex->prog->serial;
    answers_for(ex, st, ctl, slot_mask_of(ex->dc, serial, st), &a);
    a.mvprev = (uint8_t)(ex->dc->sf_mvprev && st == NVN_STAGE_VS);
    ShaderEnt *set = NULL;
    if (serial) {
        uint64_t h = (serial * 0x9E3779B97F4A7C15ull ^ (uint64_t)st) ^ nvn_hm_hash(&a, sizeof a);   // a is zero-padded
        set = shader_tab[(h ^ h >> 29) & (SHADER_SETS - 1)];
        int lk = vk_exec_shared();   // only while a parallel group records do several threads look up at once
        if (lk) plat_mutex_lock(&shader_lock);
        for (int w = 0; w < SHADER_WAYS && set[w].used; w++) {
            ShaderEnt *e = &set[w];
            if (e->serial != serial || e->stage != st || memcmp(&e->a, &a, sizeof a)) continue;
            if (w) {   // to the front
                ShaderEnt hit = *e;
                memmove(&set[1], &set[0], w * sizeof *set);
                set[0] = hit;
            }
            const GfxShader *found = set[0].s;
            if (lk) plat_mutex_unlock(&shader_lock);
            return found;
        }
        if (lk) plat_mutex_unlock(&shader_lock);
    }
    int gs = st == NVN_STAGE_VS ? GFX_STAGE_VS : st == NVN_STAGE_FS ? GFX_STAGE_FS : st == NVN_STAGE_TASK ? GFX_STAGE_TS
           : st == NVN_STAGE_MESH ? GFX_STAGE_MS : GFX_STAGE_CS;
    int late = 0;
    if (await_ms && !pending) pending = &late;
    const GfxShader *s = await_ms ? gfx_shader_await(gs, ctl, code, len, &a, await_ms, pending)
                       : pending && (vk_pipeline_async_on() || a.mvprev) ? gfx_shader_get_async(gs, ctl, code, len, &a, pending)
                                                           : gfx_shader_get(gs, ctl, code, len, NULL, &a);
    if (pending && *pending) return NULL;
    if (s) slot_mask_learn(ex->dc, serial, st, s);
    if (!s) warn_once(W_SHADER, "a stage %d shader of program %llu failed to translate: draws with it are skipped", st,
                      (unsigned long long)serial);
    else if (set) {   // in front, the least recently used way drops out
        int lk = vk_exec_shared();
        if (lk) plat_mutex_lock(&shader_lock);
        memmove(&set[1], &set[0], (SHADER_WAYS - 1) * sizeof *set);
        set[0] = (ShaderEnt){.serial = serial, .stage = st, .used = 1, .a = a, .s = s};
        if (lk) plat_mutex_unlock(&shader_lock);
    }
    return s;
}

// ---- descriptors

// The mesh fallback (below, "mesh fallback") runs a program's task and mesh stages as compute modules: while it binds
// one's resources, a compute module's bindings are those of this NVN stage (6 task, 7 mesh), not of the compute stage
// (DrawCtx.mfb_stage).

static int nvn_stage_of(int gfx_stage) {
    if (gfx_stage == GFX_STAGE_CS && dc->mfb_stage) return dc->mfb_stage;
    return gfx_stage == GFX_STAGE_VS ? NVN_STAGE_VS : gfx_stage == GFX_STAGE_FS ? NVN_STAGE_FS
         : gfx_stage == GFX_STAGE_TS ? NVN_STAGE_TASK : gfx_stage == GFX_STAGE_MS ? NVN_STAGE_MESH : NVN_STAGE_CS;
}

static VkDescriptorBufferInfo zero_info(uint64_t max) {
    return (VkDescriptorBufferInfo){Z.zbuf, 0, max < ZERO_BYTES ? max : ZERO_BYTES};
}

// Per-binding descriptor reuse (bind_resources, PB): while a descriptor that may be kept resolves, pb_cap is where its
// census count goes, so a later reuse counts the same bytes again; pb_spoiled is set when the resolution copied guest
// bytes into transient memory or counted more than once, and such a descriptor is not kept (both in DrawCtx).

// vk_mem_count, also kept per (class, use) while a graphics build runs (RS).
static void count_use(DrawCtx *cx, int cls, int use, uint64_t n) {
    vk_mem_count(cls, use, n);
    if (cx->pb_cap) {
        if (cx->pb_cap->has) cx->pb_spoiled = 1;
        else *cx->pb_cap = (PbCount){cls, use, 1, n};
    }
    if (!cx->RS.rec) return;
    for (int i = 0; i < cx->RS.ncnt; i++)
        if (cx->RS.cnt[i].cls == cls && cx->RS.cnt[i].use == use) {
            cx->RS.cnt[i].bytes += n;
            return;
        }
    if (cx->RS.ncnt < (int)(sizeof cx->RS.cnt / sizeof cx->RS.cnt[0])) cx->RS.cnt[cx->RS.ncnt++] = (RsCount){cls, use, n};
    else cx->RS.cnt_over = 1;
}

// A buffer descriptor for [addr, addr+size) of guest memory: its backing (a pool's VRAM mirror when it has one) when
// aligned, a copy in transient memory when the backing is host memory with an unaligned offset. 0 when neither works.
// written: the shader stores into it (a mirror's range goes back to guest memory at the end of the submission).
static int bind_range(VkExec *ex, uint64_t addr, uint64_t size, uint64_t align, uint64_t max, int use, int written,
                      VkDescriptorBufferInfo *out) {
    VkMemSpan sp;
    if (!addr || !vk_mem_resolve(addr, 1, &sp)) return 0;
    uint64_t n = size && size < sp.avail ? size : sp.avail;
    if (n > max) n = max;
    if (!n) return 0;
    if (sp.offset % align == 0) {
        VK_MEM_USE(sp);
        *out = (VkDescriptorBufferInfo){sp.buffer, sp.offset, n};
        if (written) vk_mem_gpu_wrote_shader(&sp, n), vk_usnap_drop(addr, n);
        count_use(ex->dc, sp.cls, use, n);
        VK_MEM_BOUND(sp);
        return 1;
    }
    if (sp.alloc) return 0;
    VkBuffer b;
    VkDeviceSize o;
    ex->dc->RS.volatile_ = 1;
    ex->dc->pb_spoiled = 1;
    memcpy(scratch(ex, n, &b, &o), (const void *)(uintptr_t)addr, n);
    vk_mem_count(sp.cls, VK_MUSE_CPU_READ, n);
    vk_mem_count(VK_MCLS_SCRATCH, use, n);
    *out = (VkDescriptorBufferInfo){b, o, n};
    return 1;
}

static void c0_table(uint8_t *at, const p_bind_buf b[16], int rows) {
    for (int r = 0; r < rows; r++) {
        uint64_t a = b[r].addr;
        uint32_t n = (uint32_t)b[r].size;
        memcpy(at + 16 * r, &a, 8);
        memcpy(at + 16 * r + 8, &n, 4);
    }
}

// The driver constant bank c0 as GLSLC-compiled shaders read it. Storage-buffer tables {u64 address, u32 size} every
// 16 bytes at 0x110 (vertex), 0x510 (fragment), 0x310 (compute): the translator's GlobalToStorage fallback loads each
// candidate base from here and compares the global pointer against it. Compute: dispatch size at 0x200, the uniform
// buffer table at 0x210 + 16 r. Graphics: the largest point size (Maxwell's limit, GLSLC clamps gl_PointSize to it) at
// 0x610, the base instance at 0x644. NVN2 task and mesh stages: storage tables at 0x670 and 0x770 (the game binds
// slots 0-2 there, and their programs read 0x670.. and 0x770..). The mesh table starts 0x90 bytes before the bank's
// end: it holds the 9 slots that fit, and sixteen would write 0x6C bytes past the bank. Other stages: zeros. Unbound
// slots read zero, since the replay keeps them zero.
// Byte offset of a stage's storage buffer table in c0, -1 for none.
static int c0_ssbo_base(int stage) {
    return stage == NVN_STAGE_VS ? 0x110 : stage == NVN_STAGE_FS ? 0x510 : stage == NVN_STAGE_CS ? 0x310
         : stage == NVN_STAGE_TASK ? 0x670 : stage == NVN_STAGE_MESH ? 0x770 : -1;
}

void vk_c0_fill(uint8_t bank[VK_C0_BYTES], int stage, const p_bind_buf ssbo[16], const p_bind_buf ubo[16],
                const uint32_t dispatch[3], uint32_t base_instance) {
    memset(bank, 0, VK_C0_BYTES);
    int base = c0_ssbo_base(stage);
    if (base < 0) return;
    int rows = (int)(VK_C0_BYTES - (uint32_t)base) / 16;
    c0_table(bank + base, ssbo, rows < 16 ? rows : 16);
    if (stage == NVN_STAGE_TASK || stage == NVN_STAGE_MESH) return;
    if (stage == NVN_STAGE_CS) {
        memcpy(bank + 0x200, dispatch, 12);
        c0_table(bank + 0x210, ubo, 16);
        return;
    }
    const float max_point = 2047.0f;
    memcpy(bank + 0x610, &max_point, 4);
    memcpy(bank + 0x644, &base_instance, 4);
}

static int c0_on(void) {   // NX2NAVI_FIX_C0=0: c0 reads zeros instead of the driver constants
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2NAVI_FIX_C0");
    }
    return on;
}

// c0 from replay scratch; a bank equal to the stage's previous one reuses its range, so it costs no copy.
static void nvn2_cs_tables(const uint8_t *ctl, uint32_t *ubos, uint32_t *ssbos);
static uint32_t nvn2_cs_tables_at(const uint8_t *ctl);

static VkDescriptorBufferInfo c0_info(VkExec *ex, int st) {
    DrawCtx *const cx = ex->dc;
    // One bank per stage: the inputs below do not say which stage they are for, and the stages lay the storage table out
    // at different offsets (c0_ssbo_base). Task and mesh shared the compute entry: the foliage's task and mesh stages
    // bind the same storage buffers, so the task stage was handed the mesh stage's bank (its table at 0x770), read
    // zero addresses at 0x670, culled every instance and launched no mesh workgroups.
    int k = st == NVN_STAGE_VS ? 0 : st == NVN_STAGE_FS ? 1 : st == NVN_STAGE_TASK ? 3 : st == NVN_STAGE_MESH ? 4 : 2;
    // vk_c0_fill reads the storage slots, and the uniform slots and dispatch size for compute or the base instance for
    // graphics: equal inputs give the same bank, so neither the fill nor the 2 KiB compare is needed.
    // A vertex or fragment bank depends on the storage slots and the base instance alone (the rest of its inputs record
    // is zero): those two are compared without building and comparing the whole 1.8 KiB record on every draw.
    if (k < 2 && cx->B.c0_valid[k] && cx->B.c0_in[k].binst == cx->c0_binst &&
        !memcmp(cx->B.c0_in[k].ssbo, ex->ssbo[st], sizeof cx->B.c0_in[k].ssbo))
        return (VkDescriptorBufferInfo){cx->B.c0_buf[k], cx->B.c0_off[k], VK_C0_BYTES};
    typeof(cx->B.c0_in[0]) in;
    memset(&in, 0, sizeof in);
    memcpy(in.ssbo, ex->ssbo[st], sizeof in.ssbo);
    if (st == NVN_STAGE_CS) memcpy(in.ubo, ex->ubo[st], sizeof in.ubo), memcpy(in.dim, cx->c0_dim, sizeof in.dim);
    else in.binst = cx->c0_binst;
    // NVN2 compute: the bank holds the push constants from byte 0x10 (then the program's handle table, which the
    // host does not read: textures and images are bound by slot).
    const uint8_t *pctl, *pcode;
    uint32_t plen;
    int nvn2_push = st == NVN_STAGE_CS && ex->prog && find_stage(ex->dc, ex->prog, st, &pctl, &pcode, &plen) && ctl_nvn2(pctl);
    if (nvn2_push) memcpy(in.push, ex->cs_push, sizeof in.push);
    if (cx->B.c0_valid[k] && !memcmp(&in, &cx->B.c0_in[k], sizeof in))
        return (VkDescriptorBufferInfo){cx->B.c0_buf[k], cx->B.c0_off[k], VK_C0_BYTES};
    cx->B.c0_in[k] = in;
    uint8_t tmp[VK_C0_BYTES];
    vk_c0_fill(tmp, st, ex->ssbo[st], ex->ubo[st], cx->c0_dim, cx->c0_binst);
    if (nvn2_push) {
        // the pushed constants from 0x10, the driver's tables after them (push_ptr, nvn2_cs_tables_at)
        uint32_t nu, ns, ta = nvn2_cs_tables_at(pctl);
        nvn2_cs_tables(pctl, &nu, &ns);
        uint32_t pn = ta - 0x10 < sizeof ex->cs_push ? ta - 0x10 : sizeof ex->cs_push;
        memcpy(tmp + 0x10, ex->cs_push, pn < VK_C0_BYTES - 0x10 ? pn : VK_C0_BYTES - 0x10);
        // Bytes 0-0xb of an NVN2 compute bank are the dispatch's group counts (x, y, z). The fog pass after the
        // integration (passes 0115/0116 of a Kokiri frame) maps each thread to tid / (groups * 16 - 1): with zeros
        // there every thread read the same point. An indirect dispatch leaves them 0 (op_dispatch_indirect).
        memcpy(tmp, cx->c0_dim, 12);
        for (uint32_t k = 0; k < nu + ns && ta + 16 * k + 12 <= VK_C0_BYTES; k++) {
            const p_bind_buf *sb = k < nu ? (k < 18 ? &ex->ubo[st][k] : NULL) : (k - nu < 16 ? &ex->ssbo[st][k - nu] : NULL);
            if (!sb) continue;
            uint32_t sz = (uint32_t)sb->size;
            memcpy(tmp + ta + 16 * k, &sb->addr, 8), memcpy(tmp + ta + 16 * k + 8, &sz, 4);
        }
    }
    if (!cx->B.c0_valid[k] || memcmp(tmp, cx->B.c0[k], VK_C0_BYTES)) {
        memcpy(scratch(ex, VK_C0_BYTES, &cx->B.c0_buf[k], &cx->B.c0_off[k]), tmp, VK_C0_BYTES);
        memcpy(cx->B.c0[k], tmp, VK_C0_BYTES);
        cx->B.c0_valid[k] = 1;
    }
    return (VkDescriptorBufferInfo){cx->B.c0_buf[k], cx->B.c0_off[k], VK_C0_BYTES};
}

// The buffer range limits, read once: the device's properties are a cold cache line on every binding of every draw.
static struct { uint64_t ubo_max, ubo_align, sbo_max, sbo_align; } LIM;

static void limits_init(void) {
    const VkPhysicalDeviceLimits *l = &vk_ctx()->props.limits;
    LIM.ubo_align = l->minUniformBufferOffsetAlignment, LIM.sbo_max = l->maxStorageBufferRange;
    LIM.sbo_align = l->minStorageBufferOffsetAlignment;
    __atomic_store_n(&LIM.ubo_max, l->maxUniformBufferRange, __ATOMIC_RELEASE);
}

// Per-object motion (section at the end): while it records a draw again, these replace the uniform bindings of the
// vertex stage's slots 18-20 (last frame's camera, bones and place) and the fragment stage's slot 3 (its parameters)
// (DrawCtx.mo_ovr).

static VkDescriptorBufferInfo ubo_info(VkExec *ex, int st, int slot) {
    DrawCtx *const cx = ex->dc;
    if (cx->mo_ovr_on && (st == NVN_STAGE_VS || st == NVN_STAGE_FS) && slot >= 0 && slot < 32 &&
        cx->mo_ovr[st][slot].buffer)
        return cx->mo_ovr[st][slot];
    if (!__atomic_load_n(&LIM.ubo_max, __ATOMIC_ACQUIRE)) limits_init();
    uint64_t max = LIM.ubo_max, align = LIM.ubo_align;
    VkDescriptorBufferInfo bi;
    if (slot == 0 && c0_on()) return c0_info(ex, st);
    if (slot == 1 && ex->prog) {   // compiler constants stored after the shader code
        const uint8_t *ctl, *code;
        uint32_t len, c1;
        if (find_stage(ex->dc, ex->prog, st, &ctl, &code, &len)) {
            c1 = ctl_c1(ctl, len);
            if (ctl_nvn2(ctl)) memcpy(&len, ctl + 0x44, 4);   // NVN2: c1 is [+0x40, +0x44) after the code
            if (c1 < len && bind_range(ex, (uint64_t)(uintptr_t)code + c1, len - c1, align, max, VK_MUSE_UBO, 0, &bi))
                return bi;
        }
    } else if (slot >= 3 && slot - 3 < 18) {
        const p_bind_buf *b = &ex->ubo[st][slot - 3];
        if (b->size && vk_usnap_find(b->addr, b->size < max ? b->size : max, align, &bi)) {   // as of the submit call
            count_use(ex->dc, VK_MCLS_SCRATCH, VK_MUSE_UBO, bi.range);
            return bi;
        }
        if (bind_range(ex, b->addr, b->size, align, max, VK_MUSE_UBO, 0, &bi)) return bi;
    }
    return zero_info(max);
}

// The NVN slot of a storage binding. The translator numbers a shader's storage buffers in the order it meets them
// (GfxShaderRes.slot); the NVN slot is the entry of the driver constant buffer c0 that holds the buffer's address
// (cbuf_slot 0, handle = word offset): 16-byte entries at byte 0x110 (vertex), 0x510 (fragment) and 0x310 (compute).
// A compute address at c0 0x210..0x30f is uniform block (offset - 0x210) / 16 read as storage: *ubo is set. -1: the
// address is not in c0 (the shader loads it from a user uniform block: ubo_ptr) or the stage has no known table.
// Contains code Copyright (c) 2026 jhaizhou (MIT; see THIRD_PARTY_NOTICES.md).
static int ssbo_slot(const GfxShaderRes *r, int st, int *ubo) {
    *ubo = 0;
    if (r->cbuf_slot != 0) return -1;
    int off = r->handle * 4, base = c0_ssbo_base(st);
    if (st == NVN_STAGE_CS && off >= 0x210 && off < 0x310 && (off - 0x210) % 16 == 0) {
        *ubo = 1;
        return (off - 0x210) / 16;
    }
    return base >= 0 && off >= base && off < base + 0x100 && (off - base) % 16 == 0 ? (off - base) / 16 : -1;
}

// 1 when the shader loads the storage buffer's address from a user uniform block (cbuf_slot 3.., NVN uniform slot
// cbuf_slot - 3, byte offset handle * 4). Some terrain reads its vertices through one, about 500 draws a frame;
// with a zero buffer they collapse and the ground is missing.
static int ubo_ptr_res(const GfxShaderRes *r) {
    return r->cbuf_slot >= 3 && r->cbuf_slot - 3 < 18;
}

// The inline uniform updates of the submission being replayed, newest last (a ring of UPD_N). op_update_ub writes them
// into the buffer's GPU copy; guest memory gets them only from the write-back when the submission ends. The terrain
// rewrites its pointer block this way before every draw, all at one address, so ubo_ptr reads the latest update that
// covers the pointer, else memory. Payloads stay in the recorded command buffer for the whole replay.
#define UPD_N 4096
static struct {
    uint64_t fence;                      // the submission the entries belong to
    uint32_t n;                          // updates noted in it
    struct { uint64_t lo, hi; const uint8_t *data; } u[UPD_N];
} UPD;

static void upd_note(const VkExec *ex, const p_update_ub *u) {
    if (UPD.fence != ex->fence) UPD.fence = ex->fence, UPD.n = 0;
    UPD.u[UPD.n++ % UPD_N] = (typeof(UPD.u[0])){u->dst, u->dst + u->size, u->data};
}

// 1 when an update of this submission supplied [a, a+n).
static int upd_read(const VkExec *ex, uint64_t a, void *out, size_t n) {
    if (UPD.fence != ex->fence) return 0;
    uint32_t top = vk_exec_upd_top(UPD.n);   // a replay worker's chunk: the updates before its group
    for (uint32_t i = top, end = top > UPD_N ? top - UPD_N : 0; i-- > end;) {
        const typeof(UPD.u[0]) *e = &UPD.u[i % UPD_N];
        if (a >= e->hi || a + n <= e->lo) continue;
        if (a < e->lo || a + n > e->hi) return 0;   // partly covered: memory
        memcpy(out, e->data + (a - e->lo), n);
        return 1;
    }
    return 0;
}

// That address as the bound block holds it at this point of the replay; 0 when the block is unbound or too short. The
// block has no size field.
static uint64_t ubo_ptr(const VkExec *ex, int st, const GfxShaderRes *r) {
    const p_bind_buf *u = &ex->ubo[st][r->cbuf_slot - 3];
    uint64_t off = (uint64_t)r->handle * 4, a = 0;
    VkMemSpan sp;
    if (!u->addr || u->size < off + 8 || upd_read(ex, u->addr + off, &a, 8)) return a;
    if (vk_mem_lookup(u->addr + off, 8, &sp)) memcpy(&a, (const void *)(uintptr_t)(u->addr + off), 8);
    return a;
}

// NVN2 compute c0 (fitted to the storage offsets of 2549 compute programs against their control sections, round 24):
// from byte 0x10 the constants the game pushed (PushComputeConstants), then a table of uniform buffers and one of
// storage buffers, {u64 address, u32 size} every 16 bytes (counts: bytes 1 and 2 of control +0xb0), then 8-byte entries
// (byte 3 of +0xb0, then byte 2 of +0xb4); +0xb8 (high half) is the end of it all, which gives the pushed size. Entry k
// of the storage part is storage slot k (BindStorageBuffer), entry k of the uniform part uniform slot k. Progs 98 and
// 99 (the compute after the composite: auto exposure) push 0x74 bytes, rounded to 0x80, and read storage entries 9 and 8
// at c0+0x120 / +0x1c0; most programs push nothing and have their tables at 0x10.
static void nvn2_cs_tables(const uint8_t *ctl, uint32_t *ubos, uint32_t *ssbos) {
    uint32_t w;
    memcpy(&w, ctl + 0xb0, 4);
    *ubos = w >> 8 & 0xff, *ssbos = w >> 16 & 0xff;
}

// Where the tables start: after the pushed constants.
static uint32_t nvn2_cs_tables_at(const uint8_t *ctl) {
    uint32_t b0, b4, b8;
    memcpy(&b0, ctl + 0xb0, 4), memcpy(&b4, ctl + 0xb4, 4), memcpy(&b8, ctl + 0xb8, 4);
    uint32_t rest = 16 * ((b0 >> 8 & 0xff) + (b0 >> 16 & 0xff)) + 8 * ((b0 >> 24) + (b4 >> 16 & 0xff)), end = b8 >> 16;
    if (!(b0 >> 8 & 0xffff)) return 0x10 + 1024;   // no tables: all of c0 from 0x10 is the pushed constants
    return end >= 0x10 + rest ? end - rest : 0x10;
}

// An NVN2 compute program that takes a storage buffer's address from c0 (c0_info): an entry of the driver's tables
// (the bound uniform or storage slot), or the {u64 address, u32 size} at that place of the constants the game pushed.
// 1 with *out filled; 0 when the program is not NVN2 or the offset is in neither.
static int push_ptr(const VkExec *ex, int st, const GfxShaderRes *r, p_bind_buf *out) {
    const uint8_t *pctl, *pcode;
    uint32_t plen;
    uint32_t o = (uint32_t)r->handle * 4;   // c0 byte offset
    if (st != NVN_STAGE_CS || r->cbuf_slot != 0 || o < 0x10 || !ex->prog || !find_stage(ex->dc, ex->prog, st, &pctl, &pcode, &plen) ||
        !ctl_nvn2(pctl))
        return 0;
    uint32_t nu, ns, ta = nvn2_cs_tables_at(pctl);
    nvn2_cs_tables(pctl, &nu, &ns);
    if (o < ta) {   // in the pushed constants: a pointer the game passes itself
        uint64_t a;
        uint32_t n = 0;
        if (o - 0x10 + 8 > sizeof ex->cs_push) return 0;
        memcpy(&a, ex->cs_push + (o - 0x10), 8);
        if (o - 0x10 + 12 <= sizeof ex->cs_push) memcpy(&n, ex->cs_push + (o - 0x10) + 8, 4);
        *out = (p_bind_buf){0, 0, a, n < (1u << 30) ? n : 0};   // size 0: up to the end of the backing
        return 1;
    }
    uint32_t t = (o - ta) / 16;
    if ((o - ta) % 16 == 0 && t < nu + ns) {
        *out = t < nu ? (t < 18 ? ex->ubo[st][t] : (p_bind_buf){0}) : (t - nu < 16 ? ex->ssbo[st][t - nu] : (p_bind_buf){0});
        return 1;
    }
    static int logged;   // diagnosis: offsets past the tables
    if (logged++ < 16) {
        uint32_t b0, b4, b8;
        memcpy(&b0, pctl + 0xb0, 4), memcpy(&b4, pctl + 0xb4, 4), memcpy(&b8, pctl + 0xb8, 4);
        rt_log("vk: NVN2 compute c0+%#x is past the tables (prog %llu, +0xb0 %08x +0xb4 %08x +0xb8 %08x, tables at %#x)", o,
               (unsigned long long)ex->prog->serial, b0, b4, b8, ta);
    }
    return 0;
}

// The buffer a storage resource names by where its address lives (GfxShaderRes.ptr aside): an NVN slot of the stage, a
// pointer in a user uniform block (size 0: up to the end of the backing), or an NVN2 compute push pointer. *tmp holds
// the last two. NULL when it names none.
static const p_bind_buf *ssbo_direct(const VkExec *ex, int st, const GfxShaderRes *r, int *ubo, p_bind_buf *tmp) {
    int slot = ssbo_slot(r, st, ubo);
    *tmp = (p_bind_buf){0};
    if (slot >= 0 && slot < 16) return *ubo ? &ex->ubo[st][slot] : &ex->ssbo[st][slot];
    if (ubo_ptr_res(r)) return tmp->addr = ubo_ptr(ex, st, r), tmp;
    return push_ptr(ex, st, r, tmp) ? tmp : NULL;
}

// The address of a storage resource read through a pointer table (GfxShaderRes.ptr): the u64 at offset ptr - 1 of the
// buffer it names, as that buffer holds it at this point of the replay (like ubo_ptr); 0 when the buffer is unbound,
// too short or not backed. Ocarina binds such a table as NVN storage slot 1 of most stages (five pointers at 0-0x20)
// and its vertex, pixel and mesh shaders read their instance and light data through them (PORT-HANDOFF.md 7.17).
static uint64_t ptr_addr(const VkExec *ex, int st, const GfxShaderRes *r) {
    p_bind_buf tmp;
    int ubo;
    const p_bind_buf *b = ssbo_direct(ex, st, r, &ubo, &tmp);
    uint64_t off = r->ptr - 1u, a = 0;
    VkMemSpan sp;
    if (!b || !b->addr || (b->size && b->size < off + 8) || upd_read(ex, b->addr + off, &a, 8)) return a;
    if (vk_mem_lookup(b->addr + off, 8, &sp)) memcpy(&a, (const void *)(uintptr_t)(b->addr + off), 8);
    return a;
}

// vk_dump.c: the guest address a storage resource of NVN stage st resolves to, 0 when none.
uint64_t vk_exec_storage_addr(const VkExec *ex, int st, const GfxShaderRes *r) {
    p_bind_buf tmp;
    int ubo;
    if (r->ptr) return ptr_addr(ex, st, r);
    const p_bind_buf *b = ssbo_direct(ex, st, r, &ubo, &tmp);
    return b ? b->addr : 0;
}

uint64_t vk_exec_cs_storage_addr(const VkExec *ex, const GfxShaderRes *r) { return vk_exec_storage_addr(ex, NVN_STAGE_CS, r); }

static VkDescriptorBufferInfo ssbo_info(VkExec *ex, int st, const GfxShaderRes *r, int written) {
    // The device address table (vk_gpa.c, translator patch 0030). Mirrored pools are not synced for its readers: a sync
    // of every mirror at a table reader's draw copied stale guest bytes over what the GPU had written into the mirrors
    // earlier in the frame, and Kokiri Forest's lit areas went black. A pool read only through the table is rarely a
    // mirrored one (mirrors are made for pools bound as buffers, which sync then). With shadow storage every pool the
    // table names is a copy of guest memory, and nothing resolves the pools such a reader reaches: the binding is
    // noted here, and the copies are brought up to date before the part is submitted (vk_memory.c
    // vk_mem_shadow_table_read). A draw that reuses the descriptors of the one before it (bind_resources) comes after
    // a build in the same command buffer, and the packed descriptors of mo_desc are built in the same guest
    // submission as their draws: either way this ran for the guest submission.
    if (r->cbuf_slot == VK_GPA_CB_SLOT) {
        vk_mem_shadow_table_read();
        // What the table names is mostly guest memory itself (pools without a mirror), which the GPU reads as it runs:
        // the submission's submit wait is the GPU's (vk_mem_take_live). Without this the staged mirror uploads
        // (NX2NAVI_MIRROR_STAGE) ended the wait once the replay had submitted, and the game wrote the next frame's
        // instances while the GPU still drew from them: the small grass and moss, which reach their instances through
        // the table, flickered and slid over the ground as the camera turned (RX 580, Kokiri Forest, 2026-11-11).
        // Not every bind comes here (the bind skip, the kept descriptors): gpa_reader notes it at each bind.
        vk_mem_note_live();
        VkDescriptorBufferInfo t = vk_gpa_info();
        return t.buffer ? t : (VkDescriptorBufferInfo){Z.zbuf, 0, VK_WHOLE_SIZE};
    }
    // The mesh fallback's own buffers (mesh_fb.c): no guest memory behind them
    if (r->cbuf_slot >= MESHFB_CB_TASK && r->cbuf_slot <= MESHFB_CB_LIST) return vk_meshfb_buffer(r->cbuf_slot);
    if (!__atomic_load_n(&LIM.ubo_max, __ATOMIC_ACQUIRE)) limits_init();
    uint64_t max = LIM.sbo_max, align = LIM.sbo_align;
    VkDescriptorBufferInfo bi;
    int ubo = 0;
    p_bind_buf up = {0};
    const p_bind_buf *b;
    if (r->ptr) up.addr = ptr_addr(ex, st, r), b = &up;   // size 0: up to the end of the backing
    else b = ssbo_direct(ex, st, r, &ubo, &up);
    if (b) {
        // vk_caps.c: shaders translated for sbuf_align address the buffer from its address rounded down to it.
        uint64_t lead = vk_caps.sbuf_align ? b->addr & (vk_caps.sbuf_align - 1) : 0;
        if (bind_range(ex, b->addr - lead, b->size ? b->size + lead : 0, align, max, ubo ? VK_MUSE_UBO : VK_MUSE_SSBO,
                       written, &bi)) {
            if (written && bi.buffer != VK_NULL_HANDLE) note_written(ex, b->addr - lead, bi.range);
            return bi;
        }
        if (b->addr) warn_once(W_SSBO_ALIGN, "storage buffer %#llx is not backed or not aligned: a zero buffer is bound",
                               (unsigned long long)b->addr);
    } else {
        warn_once(W_SSBO_SLOT, "a storage buffer whose address is at c%d+%#x (stage %d) is not replayed: a zero buffer is bound",
                  r->cbuf_slot, r->handle * 4, st);
        static int logged;   // diagnosis: what c1 of an NVN2 compute program is (its control section's c1 range, the
        const uint8_t *ctl, *code;   // uniform slots the game bound)
        uint32_t len;
        if (logged < 4 && r->cbuf_slot == 1 && ex->prog && find_stage(ex->dc, ex->prog, st, &ctl, &code, &len)) {
            uint32_t c1s, c1e, size;
            memcpy(&size, ctl + 0x38, 4), memcpy(&c1s, ctl + 0x40, 4), memcpy(&c1e, ctl + 0x44, 4);
            char line[512];
            int o = snprintf(line, sizeof line, "vk: c1 storage address, prog %llu: code %#x, c1 [%#x, %#x), len %#x; c1 words:",
                             (unsigned long long)ex->prog->serial, size, c1s, c1e, len);
            for (uint32_t k = 0; k < 4 && c1s + 4 * k + 4 <= len; k++) {
                uint32_t wv;
                memcpy(&wv, code + c1s + 4 * k, 4);
                o += snprintf(line + o, sizeof line - o, " %08x", wv);
            }
            for (int k = 0; k < 6 && o < (int)sizeof line - 40; k++)
                if (ex->ubo[st][k].addr)
                    o += snprintf(line + o, sizeof line - o, " ubo%d=%#llx+%#x", k + 3, (unsigned long long)ex->ubo[st][k].addr,
                                  (unsigned)ex->ubo[st][k].size);
            rt_log("%s", line);
            logged++;
        }
    }
    // A shader that writes it gets the sink: the zero buffer also stands in for missing vertex buffers and read-only
    // storage, and words written into it came back as garbage loop counts (a GPU hang at the first gameplay frame).
    if (written && Z.sink) return (VkDescriptorBufferInfo){Z.sink, 0, max < ZERO_BYTES ? max : ZERO_BYTES};
    return zero_info(max);
}

// ---- feedback loops (maxwell.md 1.9, vk.h VkCtx.feedback)
// NX2navi's G-buffer and terrain shaders sample targets of the pass that renders them, at their own pixel, after earlier
// draws of the pass wrote them (albedo, normals, R32F linear depth). Maxwell reads see those writes; in Vulkan the read
// is undefined without a barrier, and on the RX 9070 it showed as dark screen-aligned blocks on the island ground
// and hid Tulin's sage spirit. Before a draw whose fragment shader samples an attachment of
// the open pass that a draw or clear of the pass wrote since the pass began or the last barrier:
// - VK_FEEDBACK_LAYOUT: the image is learned (VkTexImage.fb_learned; it has the feedback usage since its creation), the
//   pass restarts once with it in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT, and from then on its passes attach it in that
//   layout, its descriptors carry that layout, the draws that sample it use a pipeline with the feedback flag
//   (VkPipeState.feedback) and a by-region barrier precedes such a draw.
// - VK_FEEDBACK_RESTART: the pass ends and restarts behind the full barrier (vk_exec_pass_begin).
static int in_fbl(const VkExec *ex, VkImage img) {
    for (int i = 0; i < ex->nfbl; i++)
        if (ex->fbl_img[i] == img) return 1;
    return 0;
}

// The image and the levels [l0, l1) and layers [a0, a1) a texture's view covers.
typedef struct { VkImage image; uint32_t l0, l1, a0, a1; } SubRange;

static SubRange sub_of(const VkTexImage *img, const view_t *v, int hv) {
    SubRange r = {img->image, 0, img->levels, 0, img->layers};
    if (hv && v->set_levels) r.l0 = v->base_level, r.l1 = v->base_level + (v->nlevels ? v->nlevels : img->levels);
    if (hv && v->set_layers) r.a0 = v->min_layer, r.a1 = v->min_layer + (v->nlayers ? v->nlayers : img->layers);
    return r;
}

// Attachment slots of the open pass (bit 8 depth) whose subresource r covers.
static uint32_t attached_as(const VkExec *ex, const SubRange *r) {
    uint32_t m = 0;
    if (!r->image) return 0;
    for (uint32_t i = 0; i < 9; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if ((i < 8 && i >= ex->pass_ncolor) || t->image != r->image) continue;
        if (t->level < r->l0 || t->level >= r->l1) continue;
        if (!t->is3d && (t->base_layer + t->layers <= r->a0 || t->base_layer >= r->a1)) continue;
        m |= 1u << i;
    }
    return m;
}

// Knobs the draw and dispatch paths test on every call, read once: each cfg lookup walks the settings table
// (cfg.c raw_get), about 0.2 ms a frame of replay in Kokiri Forest when they ran per draw.
static int knob_log_mesh(void) {   // NX2NAVI_LOG_MESH=1: the first mesh draws' state
    static int on = -1;
    if (on < 0) on = rt_knob("LOG_MESH", "0");
    return on;
}
static int knob_tm_dispatch(void) {   // NX2NAVI_TM_DISPATCH: the scene upscaler stand-in's mode
    static int on = -1;
    if (on < 0) on = rt_knob("TM_DISPATCH", "1");
    return on;
}
static int knob_log_cs_binds(void) {   // NX2NAVI_LOG_CS_BINDS set: the compute bindings of each program's first dispatch
    static int on = -1;
    if (on < 0) on = cfg_get("NX2NAVI_LOG_CS_BINDS") != NULL;
    return on;
}

// NX2NAVI_TEX_MEMO (tex_info, feedback_scan): 0 off, 1 on, 2 (=check) computes every hit again and stops on a difference.
static int td_mode(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_value("NX2NAVI_TEX_MEMO");
        on = !strcmp(e, "check") ? 2 : strcmp(e, "0") != 0;
    }
    return on;
}

// The subresource a texture handle samples, per handle, kept while its slot resolves the same (slot_still): a draw's
// texture bindings change the whole-scan memo below, but the handles they bind were mostly looked up before, in this
// pass or an earlier one. Direct-mapped.
#define FM_N 8192
typedef struct { uint64_t h, tok; uint32_t gen; SubRange r; } FmEnt;   // DrawCtx.fm: one table per recording thread

static SubRange sampled_sub(uint64_t h) {   // texture lock held
    view_t v;
    int hv;
    const VkTexImage *img = vk_texture_by_id_locked((int)(h & 0xfffff), &v, &hv);
    return img ? sub_of(img, &v, hv) : (SubRange){0};
}

// Attachments of the open pass the fragment shader fs samples; kept while the shader, the texture bindings and the
// pass stay the same.
static uint32_t feedback_scan(VkExec *ex, const GfxShader *fs) {
    if (ex->fb_fs == fs && ex->fb_serial == ex->tex_serial && ex->fb_pass == ex->pass_serial) return ex->fb_sampled;
    const GfxShaderRes *res;
    int n = gfx_shader_resources(fs, GFX_RES_TEX, &res);
    uint32_t m = 0, gen = vk_texture_slots_gen();   // before any lookup
    int memo = td_mode(), locked = 0;
    FmEnt *fm = ex->dc->fm;
    if (!fm && !(fm = ex->dc->fm = calloc(FM_N, sizeof *fm))) rt_fatal("vk: out of memory (feedback memo)");
    for (int r = 0; r < n; r++) {
        // Only bound slots (0..127) are looked at. The texture and sampler pool arrays (res.pool: VK_POOL_TEX and 4096
        // descriptors, bindless handles) have slot -1: walking them ran up to about 100,000 empty iterations per scan,
        // about 3.6 ms of replay per frame in Kokiri Forest, and read tex[FS][0..127] for them by mistake.
        if (res[r].pool || res[r].slot < 0) continue;
        int count = res[r].count ? res[r].count : 1;
        if (count > 128 - res[r].slot) count = 128 - res[r].slot;
        for (int k = 0; k < count; k++) {
            int slot = res[r].slot + k;
            uint64_t h = ex->tex[NVN_STAGE_FS][slot];
            if (!h) continue;
            FmEnt *f = &fm[(h * 0x9E3779B97F4A7C15ull) >> 51];
            if (memo && f->h == h && slot_still(&f->gen, f->tok, h, gen)) {
                if (memo == 2) {
                    if (!locked) vk_texture_lock(), locked = 1;
                    SubRange c = sampled_sub(h);
                    if (memcmp(&c, &f->r, sizeof c))
                        rt_fatal("vk_exec: feedback memo of handle %#llx differs from a lookup", (unsigned long long)h);
                }
                m |= attached_as(ex, &f->r);
                continue;
            }
            uint64_t tok = token_of(h);   // before the lookup
            if (!locked) vk_texture_lock(), locked = 1;
            SubRange sr = sampled_sub(h);
            m |= attached_as(ex, &sr);
            if (tok) *f = (typeof(*f)){.h = h, .tok = tok, .gen = gen, .r = sr};
            else f->h = 0;
        }
    }
    if (locked) vk_texture_unlock();
    ex->fb_fs = fs, ex->fb_serial = ex->tex_serial, ex->fb_pass = ex->pass_serial, ex->fb_sampled = m;
    return m;
}

// Before the pipeline lookup of a draw with fragment shader fs: learns and restarts as the mode says. Returns the
// sampled attachments in the feedback layout (the pipeline's feedback flag and feedback_barrier).
static uint32_t feedback_prepare(VkExec *ex, const GfxShader *fs) {
    int mode = vk_ctx()->feedback;
    if (mode == VK_FEEDBACK_OFF || !fs || !ex->in_pass) return 0;
    uint32_t m = feedback_scan(ex, fs);
    uint32_t d = m & ex->fb_dirty;
    if (d) vk_exec_frame.fb_draws++;
    if (d & ~(mode == VK_FEEDBACK_LAYOUT ? ex->fbl_mask : 0)) {
        if (mode == VK_FEEDBACK_LAYOUT)
            for (uint32_t i = 0; i < 9; i++) {
                const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
                if ((d >> i & 1) && t->img && t->img->fb_usage) ((VkTexImage *)t->img)->fb_learned = 1;
            }
        vk_exec_pass_end(ex);
        vk_exec_pass_begin(ex);   // learned targets now in the feedback layout; the full barrier cleared fb_dirty
        vk_exec_frame.fb_restarts++;
    }
    return mode == VK_FEEDBACK_LAYOUT ? m & ex->fbl_mask : 0;
}

// By-region barrier inside the pass: earlier attachment writes become visible to this draw's fragment shader reads.
static void feedback_barrier(VkExec *ex) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                           VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                           .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
                           .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded++;
    ex->fb_dirty = 0;
    vk_exec_frame.fb_barriers++;
}

// Write after read inside the open pass: the GPU orders attachment writes after earlier attachment writes, not after
// earlier draws' texture reads of the same image (feedback layout), so a draw or clear writing a target an earlier
// draw of the pass sampled could land before those reads finish and they read the new value (a band of the picture
// a frame out: the fog's flat colour, a dark shadow band). A by-region barrier from fragment shader reads to
// attachment writes orders them; written: the attachments about to be written (bit i colour i, bit 8 depth).
void vk_exec_feedback_war(VkExec *ex, uint32_t written) {
    if (!(written & ex->fb_read)) return;
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                           VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                           .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
                           .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded++;
    ex->fb_read = 0;
    vk_exec_frame.fb_war_barriers++;
}

// The attachments the draw with state s writes: colour targets with a nonzero effective write mask, depth with depth
// writes or the stencil test on.
static uint32_t feedback_writes(const VkExec *ex, const VkPipeState *s) {
    uint32_t w = ex->depth.image && ((s->ds.dt && s->ds.dw) || s->ds.st) ? 0x100u : 0;
    for (uint32_t i = 0; i < ex->pass_ncolor; i++)
        if (ex->color[i].image && vk_pipeline_target_wmask(s, (int)i)) w |= 1u << i;
    return w;
}

// Marks the attachments the draw with state s writes, after ordering them behind earlier draws' reads of them
// (vk_exec_feedback_war); sampled: what this draw itself samples in the feedback layout, which a later write must wait
// for in turn (a draw reading and writing one target is the game's own feedback loop, ordered by region as it is).
static void feedback_written(VkExec *ex, const VkPipeState *s, uint32_t sampled) {
    if (vk_ctx()->feedback == VK_FEEDBACK_OFF) return;
    uint32_t w = feedback_writes(ex, s);
    vk_exec_feedback_war(ex, w);
    ex->fb_dirty |= w;
    ex->fb_read |= sampled;
}

static int tex_info_full(VkExec *ex, const GfxShaderRes *r, uint64_t h, int storage, int separate,
                         VkDescriptorImageInfo *out, VkImage *image);

// Sampled-texture descriptors per (handle, resource type, separate), kept while nothing they derive from changed: the
// slot's resolution and texture entry (slot_still) and the samplers (vk_sampler_gen); the layout is set again on every
// hit from the images in the feedback layout (in_fbl of the image kept with it). The first binding of a slot in a submission (vk_texture_epoch) still makes the slot lookup
// for its storage check; a re-upload writes the same image, so the descriptor stays. Draws bind the same textures
// thousands of times a frame (15,000 lookups a frame in the monsters fight); a hit skips the slot lookup, the view key
// with its hash probe and the sampler lookup. Storage images, which note their binding per submission, are
// not kept. Direct-mapped; texture lock held. NX2NAVI_TEX_MEMO=0 turns it off; =check computes every hit again and stops
// on a difference.
#define TD_N 8192
static struct {
    uint64_t h, tok;
    uint32_t gen, epoch, sgen;
    uint16_t type;
    uint8_t separate, valid;
    uint8_t reduced;   // the image was made without its top levels and the resource did not mind (GfxShaderRes.exact)
    VkImage image;
    VkDescriptorImageInfo out;
} TD[TD_N];

static void tex_info_lookup(VkExec *ex, const GfxShaderRes *r, uint64_t h, int storage, int separate,
                            VkDescriptorImageInfo *out, VkImage *img_out) {
    int td_on = td_mode();
    VkImage image = VK_NULL_HANDLE;
    if (!h || storage || !td_on) {
        tex_info_full(ex, r, h, storage, separate, out, &image);
        if (img_out) *img_out = image;
        return;
    }
    uint32_t gen = vk_texture_slots_gen(), ep = vk_texture_epoch(), sg = vk_sampler_gen();   // before any lookup
    typeof(TD[0]) *m = &TD[(h * 0x9E3779B97F4A7C15ull ^ (uint64_t)r->type << 7 ^ (uint64_t)separate) >> 51];
    if (m->valid && m->h == h && m->type == r->type && m->separate == separate && m->sgen == sg &&
        !(m->reduced && r->exact) && slot_still(&m->gen, m->tok, h, gen)) {
        if (m->epoch != ep) {   // the slot's first binding in this submission: only its storage check is due
            vk_texture_by_id_locked((int)(h & 0xfffff), NULL, NULL);
            m->epoch = ep;
        }
        *out = m->out;
        out->imageLayout = m->image && ex->nfbl && in_fbl(ex, m->image) ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT
                                                                          : VK_IMAGE_LAYOUT_GENERAL;
        if (img_out) *img_out = m->image;
        if (td_on == 2) {
            VkDescriptorImageInfo full;
            tex_info_full(ex, r, h, storage, separate, &full, &image);
            if (full.sampler != out->sampler || full.imageView != out->imageView || full.imageLayout != out->imageLayout)
                rt_fatal("vk_exec: texture descriptor memo of handle %#llx type %#x differs from a lookup",
                         (unsigned long long)h, r->type);
        }
        return;
    }
    uint64_t tok = token_of(h);   // before the lookup
    int reduced = tex_info_full(ex, r, h, storage, separate, out, &image);
    // a lookup that made the texture whole again changed the slot's resolution after the stamps above were read: the
    // entry never matches, and the next binding fills it
    *m = (typeof(*m)){.h = h, .tok = tok, .gen = gen, .epoch = ep, .sgen = sg, .type = r->type,
                      .separate = (uint8_t)separate, .valid = 1, .reduced = (uint8_t)reduced, .image = image, .out = *out};
    if (img_out) *img_out = image;
}

// Returns whether the image bound was made without its top mip levels (texture detail, VkTexImage.skip).
// For the flight recorder: a draw's own binding of slot id resolved to no view, so the draw samples the stand-in. The
// pool's entries are noted where they change (pool_note_blank); a binding written into a draw's own set is not, and it
// keeps what it got. Noted once per slot and frame, for the first 256 slots of a frame. Texture lock held.
static void direct_blank(uint32_t id) {
    static uint32_t seen[256];
    static uint64_t frame = ~0ull;
    static int n;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (f != frame) frame = f, n = 0;
    for (int i = 0; i < n; i++)
        if (seen[i] == id) return;
    if (n == (int)(sizeof seen / sizeof *seen)) return;
    seen[n++] = id;
    uint64_t addr;
    uint32_t fmt, w, h, why = vk_texture_slot_why_locked((int)id, &addr, &fmt, &w, &h);
    flight_tex(FLIGHT_TEX_DIRECT, addr, fmt, w, h, 0, 0, why, id);
}

static int tex_info_full(VkExec *ex, const GfxShaderRes *r, uint64_t h, int storage, int separate,
                         VkDescriptorImageInfo *out, VkImage *image) {
    int ms = (r->type & 0x200) != 0, reduced = 0;
    *image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler samp = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    if (h) {
        view_t v;
        int hv;
        const VkTexImage *img = vk_texture_by_id_locked((int)(h & 0xfffff), &v, &hv);
        // A shader that reads this binding's texels by position, or asks for its size, needs every level of the
        // texture where the game put it (gfx_shader.c reflect_exact): one made without its top levels is made again.
        if (img && img->skip && r->exact) img = vk_texture_exact_by_id_locked((int)(h & 0xfffff), &v, &hv);
        reduced = img && img->skip;
        if (img) view = get_view(ex, img, &v, hv, r->type, storage), *image = img->image;
        if (img && storage) vk_texture_storage_bound_locked(img);
        if (img && !storage && ex->nfbl && in_fbl(ex, img->image)) layout = VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
        if (!storage && !separate) {
            // A depth texture sampled without a compare reads one texel on the Switch, whatever the sampler's filter:
            // blending neighbouring depths puts reconstructed positions in mid-air at object edges (shadow masks).
            int sid = (int)(h >> 20 & 0xfff);
            int point = img && (img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && !(r->type & 0x400) &&
                        vk_exec_view_aspect(img, &v, hv) != VK_IMAGE_ASPECT_STENCIL_BIT;
            samp = point ? vk_sampler_point_by_id_locked(sid)
                 : r->type & 0x400 ? vk_sampler_compare_by_id_locked(sid)
                 : reduced ? vk_sampler_detail_by_id_locked(sid, img->skip) : vk_sampler_by_id_locked(sid);
        }
    }
    if (!view) {
        if (h && !(h >> 63) && !storage && flight_on) direct_blank((uint32_t)(h & 0xfffff));
        int di = dummy_index(res_view_type(r->type), ms);
        view = storage ? storage_dummy(di, r->format) : Z.img[di].view;
    }
    if (!samp) samp = r->type & 0x400 ? Z.samp_shadow : Z.samp;
    *out = (VkDescriptorImageInfo){separate || storage ? VK_NULL_HANDLE : samp, view, layout};
    return reduced;
}

// ---- texture and sampler pools (bindless)
// Shaders that take texture handles from memory (TEX.B and kin) index the whole texture pool: the translator declares
// arrays of VK_POOL_TEX images and VK_POOL_SAMP samplers (index =
// handle & 0xfffff and handle >> 20), which vk_pipeline.c spirv_pool_rebind moves to set 2, one image binding per view
// kind (vk.h). One set serves every such pipeline: entry id of binding k holds texture slot id viewed as kind k (a
// stand-in when the slot is empty or cannot be viewed so), entry id of binding VK_POOL_KINDS sampler slot id. pool_sync
// rewrites the entries whose slot changed (vk_texture_slot_token) and fills the kinds a new pipeline asked for; with
// update-after-bind (descriptorIndexing) a write never invalidates a command buffer being recorded. Each submit's first
// sync also looks every registered slot up once, the freshness check a bound texture gets (vk_texture_by_id_locked).
// The set is one of a ring of POOL_SETS, one per submission: on the Switch the GPU reads the pool as it goes, but it
// runs a submission right after nvnQueueSubmitCommands, so what it reads is the pool as of about the submit. Here the
// GPU runs a submission several milliseconds later, and Ocarina registers slots for its next submission right after
// submitting: with one set, an in-flight submission sampled a texture registered for the next one, the lit materials
// of distant characters read the wrong texture as their light or probe data, wrote NaN and bloom spread it (white
// dialogue frames, a fifth of them; none once each submission waited for its GPU work, NX2NAVI_SUBMIT_FINISH). A
// submission's first pool_sync takes the next set (pool_take_set), waits for the GPU only if that set's last
// submission is still running, and brings it up to date from the change log (every entry written since the set was
// last current; the whole set when the log has wrapped). Writes within a submission go to its own set at once, until a
// part of the submission goes to the queue early (vk_exec.c maybe_split): that part may read any entry of the set, so
// the set is sealed (vk_exec_pool_split) and the next sync takes the following set as if a new submission began.
// Writing into it instead updated descriptors a pending command buffer reads, which Vulkan does not allow: the part
// sampled textures registered after it went, and on RADV a texture whose memory was mapped after the part's submit is
// absent from that submit's page tables, a GPU page fault on the 128x128 probe pass of Kokiri Forest's first frame
// (RADV's radv_wait_for_vm_map_updates hid it, as did NX2NAVI_SPLIT_DRAWS=0).
volatile uint32_t vk_pool_kinds_used;
// Per texture slot, what pool_fill keeps between refills: the descriptor hash of the registration it was read under,
// and the lookup memo stamp its entries were last written from (0: none) with the kinds that write covered.
typedef struct { uint64_t kh; uint32_t kreg, stamp, fmask; } PoolSlot;
#define POOL_SETS VK_POOL_SETS
#define POOL_LOG (1u << 20)   // entries of the change log: binding * VK_POOL_TEX + id, one per write
static struct {
    VkDescriptorSetLayout layout, empty;
    VkDescriptorPool dpool;
    VkDescriptorSet set;               // the current submission's (sets[cur])
    VkDescriptorSet sets[POOL_SETS];
    uint64_t set_fence[POOL_SETS];     // the timeline value of the last submission that bound it
    uint64_t set_synced[POOL_SETS];    // the change-log position up to which it holds every write
    int nsets, cur, all;               // all: the first fill, written to every set
    uint64_t cur_fence;                // the submission sets[cur] serves
    uint32_t *log;                     // POOL_LOG entries
    uint64_t logn;                     // writes so far
    uint64_t *last;                    // per entry: the log position of its newest write (catch-up writes it once, there)
    uint64_t switches, waits, catchup, written, rewrites, catch_ns, sync_ns, n_slots, n_refill, n_fresh;
    uint64_t n_walk[2], walk_visit[2], walk_refill[2], walk_ns[2];   // dirty walks: at a submission's start, within one
    uint64_t n_visit;                  // slots the dirty walks looked at
    uint64_t n_gwalk, n_gskip, n_gskip_slots, n_grep;   // POOL_GROUP: groups walked, left alone and their slots, first
                                       // members looked up for the memo
    uint64_t n_gchk_writes, n_gchk_groups;   // POOL_GROUP_CHECK: writes the groups left alone got all the same, and in how many
    uint64_t n_gchk_pending, n_gchk_bad;     // of members registered since their last fill (not counted above); this group's
    uint64_t n_gnouni, n_gchanged;     // groups walked member by member: not known to hold one resolution, first member changed
    int gmode;                         // pool_group() as of the last sync
    uint64_t n_by_reg, n_by_addr;      // refills whose token changed by a registration, by an address generation only
    uint64_t full_ns, n_full;          // the walks over every slot (a submission's first sync) and their time
    uint64_t ep_ns, n_ep, n_ep_ent;    // POOL_FAST: a submission's freshness check of the noted entries instead
    uint64_t unres_ns, n_unres_walks;  // of it: the walks over the slots without an entry
    uint32_t unres_mg;                 // mem_map_gen of the last such walk
    int unres_more;                    // it left a lookup for the creation budget: the next submission walks again
    PoolSlot *ps;                      // [id]
    uint8_t *unres_on;                 // [id]: its last fill resolved to no entry (POOL_FAST: retried each submission)
    uint8_t *unres_in;                 // [id]: the slot is in unres (once: pool_unres never lists it a second time)
    int *unres, nunres, cunres;        // those slots (some may have left: unres_on clear, dropped by the next walk)
    uint32_t *ent;                     // [id]: the entry its last fill resolved to, held there (pool_hold); 0: none
    uint64_t dirty_pos;                // vk_texture_dirty_slots_locked position (0: the next sync walks every slot)
    int ready, uab, sealed;            // sealed: a part of the current submission already reads sets[cur]
    uint32_t kinds, gen, epoch, sgen;
    int retry;                         // the last sync left slots to the creation budget
    uint64_t frame, frame_ns;          // the frame of the last sync, and the time its syncs spent making textures (pool_create_ns)
    uint64_t n_frame_stops, n_defer_skips;   // syncs that created nothing for the frame's time, lookups they skipped
    uint64_t *tok;                     // per texture slot: vk_texture_slot_token it was filled under (0: stand-ins)
    VkImageView *view;                 // [kind * VK_POOL_TEX + id]: what the entry holds
    VkSampler *samp;                   // [id]
    uint64_t writes;
    // pool marks (vk_exec_pool_marks): the device buffer the marked shaders write, read-back copies in a ring
    VkBuffer marks;
    VmaAllocation marks_alloc;
    struct { VkBuffer buf; VmaAllocation alloc; const uint32_t *p; uint64_t fence; } rb[4];
    int rb_head, rb_n, marks_ready;
    uint32_t marks_since;              // submissions that recorded work since the last copy
} PL;

static void pool_marks_make(void);

// The device's limits for the sampled images of the pool's set: in one set, and in one shader stage of a pipeline
// layout (the pool's bindings are visible to every stage, and a layout adds the bound textures of set 0 to them). The
// update-after-bind limits apply when PL.uab, the plain ones otherwise. *set and *stage are 0 when unknown.
static void pool_limits(VkCtx *c, uint64_t *set, uint64_t *stage) {
    VkPhysicalDeviceVulkan12Properties p12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12};
    vkGetPhysicalDeviceProperties2(c->phys, &p2);
    if (PL.uab) {
        *set = p12.maxDescriptorSetUpdateAfterBindSampledImages;
        *stage = p12.maxPerStageDescriptorUpdateAfterBindSampledImages;
    } else {
        *set = p2.properties.limits.maxDescriptorSetSampledImages;
        *stage = p2.properties.limits.maxPerStageDescriptorSampledImages;
    }
}

static VkDescriptorSetLayout pool_layout_make(void) {
    VkCtx *c = vk_ctx();
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(c->phys, &f2);   // create_device enables every supported feature
    PL.uab = f12.descriptorBindingSampledImageUpdateAfterBind && f12.descriptorBindingPartiallyBound &&
             f12.descriptorBindingUpdateUnusedWhilePending;
    // The pool needs VK_POOL_KINDS x VK_POOL_TEX sampled images in its set, whatever the game uses: a translated shader
    // indexes the whole array. A device whose limits are below that is told in the log before the layout is asked for;
    // a driver that accepts the layout anyway is let go on, one that refuses it ends the start with the limit named.
    uint64_t need = (uint64_t)VK_POOL_KINDS * VK_POOL_TEX, lim_set = 0, lim_stage = 0;
    pool_limits(c, &lim_set, &lim_stage);
    int over = (lim_set && need > lim_set) || (lim_stage && need > lim_stage);
    rt_log("vk: bindless texture pools: %d kinds x %d textures = %llu sampled images; the device allows %llu in a set "
           "and %llu in a stage (%s limits)%s", VK_POOL_KINDS, VK_POOL_TEX, (unsigned long long)need,
           (unsigned long long)lim_set, (unsigned long long)lim_stage, PL.uab ? "update-after-bind" : "plain",
           over ? ": WARNING, over the device's limit" : "");
    int marks = vk_pool_marks_enabled();
    uint32_t nb = VK_POOL_KINDS + 1 + (marks ? 1u : 0u);   // the marks buffer at VK_POOL_MARK_BINDING, last
    VkDescriptorSetLayoutBinding b[VK_POOL_KINDS + 2];
    VkDescriptorBindingFlags fl[VK_POOL_KINDS + 2];
    VkDescriptorBindingFlags f = (f12.descriptorBindingPartiallyBound ? VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT : 0) |
        (PL.uab ? VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT : 0);
    for (uint32_t k = 0; k <= VK_POOL_KINDS; k++) {
        b[k] = (VkDescriptorSetLayoutBinding){k, k < VK_POOL_KINDS ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER,
                                              k < VK_POOL_KINDS ? VK_POOL_TEX : VK_POOL_SAMP, VK_SHADER_STAGE_ALL, NULL};
        fl[k] = f;
    }
    if (marks) {   // written once, before any set is bound: no update-after-bind
        b[VK_POOL_MARK_BINDING] = (VkDescriptorSetLayoutBinding){VK_POOL_MARK_BINDING, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                                                 VK_SHADER_STAGE_ALL, NULL};
        fl[VK_POOL_MARK_BINDING] = 0;
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                                                      .bindingCount = nb, .pBindingFlags = fl};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = &bf,
                                          .flags = PL.uab ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0,
                                          .bindingCount = nb, .pBindings = b};
    VkDescriptorSetLayout l;
    if (vkCreateDescriptorSetLayout(c->device, &li, NULL, &l) != VK_SUCCESS)
        rt_fatal("vk: the texture pool layout failed: it holds %llu sampled images, and this device allows %llu in a "
                 "set and %llu in a stage (%s limits)", (unsigned long long)need, (unsigned long long)lim_set,
                 (unsigned long long)lim_stage, PL.uab ? "update-after-bind" : "plain");
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_POOL_KINDS * VK_POOL_TEX},
                                  {VK_DESCRIPTOR_TYPE_SAMPLER, VK_POOL_SAMP}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    uint32_t nps = marks ? 3 : 2;
    // POOL_SETS sets, fewer where the device has no room for them (one at the least, as before)
    for (int n = POOL_SETS; n >= 1 && !PL.nsets; n--) {
        VkDescriptorPoolSize psn[3];
        for (uint32_t i = 0; i < nps; i++) psn[i] = (VkDescriptorPoolSize){ps[i].type, ps[i].descriptorCount * (uint32_t)n};
        VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                         .flags = PL.uab ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0,
                                         .maxSets = (uint32_t)n, .poolSizeCount = nps, .pPoolSizes = psn};
        if (vkCreateDescriptorPool(c->device, &pi, NULL, &PL.dpool) != VK_SUCCESS) continue;
        VkDescriptorSetLayout ls[POOL_SETS];
        for (int i = 0; i < n; i++) ls[i] = l;
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = PL.dpool,
                                          .descriptorSetCount = (uint32_t)n, .pSetLayouts = ls};
        if (vkAllocateDescriptorSets(c->device, &ai, PL.sets) == VK_SUCCESS) PL.nsets = n;
        else vkDestroyDescriptorPool(c->device, PL.dpool, NULL), PL.dpool = VK_NULL_HANDLE;
    }
    if (!PL.nsets) rt_fatal("vk: the texture pool set failed");
    if (marks) pool_marks_make();
    PL.set = PL.sets[0];
    PL.log = malloc(sizeof *PL.log * POOL_LOG);
    PL.last = calloc((size_t)VK_POOL_KINDS * VK_POOL_TEX + VK_POOL_SAMP, sizeof *PL.last);
    if (!PL.log || !PL.last) rt_fatal("vk: out of memory");
    PL.tok = calloc(VK_POOL_TEX, sizeof *PL.tok);
    PL.view = calloc((size_t)VK_POOL_KINDS * VK_POOL_TEX, sizeof *PL.view);
    PL.samp = calloc(VK_POOL_SAMP, sizeof *PL.samp);
    PL.ps = calloc(VK_POOL_TEX, sizeof *PL.ps);
    PL.unres_on = calloc(VK_POOL_TEX, 1);
    PL.unres_in = calloc(VK_POOL_TEX, 1);
    PL.ent = calloc(VK_POOL_TEX, sizeof *PL.ent);
    if (!PL.tok || !PL.view || !PL.samp || !PL.ps || !PL.unres_on || !PL.unres_in || !PL.ent) rt_fatal("vk: out of memory");
    rt_log("vk: bindless texture pools: %d kinds x %d textures, %d samplers, %s, %d sets (one per submission in turn)%s",
           VK_POOL_KINDS, VK_POOL_TEX, VK_POOL_SAMP,
           PL.uab ? "update after bind" : "no update-after-bind on this GPU (entries written while bound are undefined)", PL.nsets,
           marks ? "; shaders mark the slots they sample (NX2NAVI_TEX_DISUSE)" : "");
    return l;
}

// ---- pool marks (NX2NAVI_TEX_DISUSE; vk_pipeline.c spirv_pool_mark writes them, vk_texture.c "disuse" reads them)
// A device buffer of a uint per texture slot at VK_POOL_MARK_BINDING of every pool set: a marked shader stores 1 into
// the slot of every pool texture it samples. Every MARKS_EVERY submissions that recorded work, the end of the
// submission copies the buffer into a host-visible read-back buffer and clears it; a copy is in the submission that
// signals the timeline value, so once vk_sync_fence_done reaches that value the copy is complete, and a later
// submission hands it to vk_pool_marks_read. Up to MARKS_RB copies may be in flight; with none free, the marks stay
// and the next submission tries. The buffer starts with whatever the memory held: a stray mark only keeps a texture
// one read longer, and vk_texture.c waits for two reads before it evicts by disuse.
#define MARKS_EVERY 4
#define MARKS_RB 4

static void pool_marks_make(void) {
    VkCtx *c = vk_ctx();
    // a uint past the slots: the spare that a fragment shader's marks of accesses it did not reach go to
    // (vk_pipeline.c spirv_pool_mark); it is never read back
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)(VK_POOL_TEX + 1) * 4,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateBuffer(c->vma, &bi, &ai, &PL.marks, &PL.marks_alloc, NULL) != VK_SUCCESS)
        rt_fatal("vk: the texture pool marks buffer failed");
    bi.size = (VkDeviceSize)VK_POOL_TEX * 4, bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo hi = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    for (int i = 0; i < MARKS_RB; i++) {
        VmaAllocationInfo info;
        if (vmaCreateBuffer(c->vma, &bi, &hi, &PL.rb[i].buf, &PL.rb[i].alloc, &info) != VK_SUCCESS || !info.pMappedData)
            rt_fatal("vk: the texture pool marks read-back buffer failed");
        PL.rb[i].p = info.pMappedData;
    }
    VkDescriptorBufferInfo di = {PL.marks, 0, VK_WHOLE_SIZE};
    for (int i = 0; i < PL.nsets; i++) {
        VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = PL.sets[i],
                                  .dstBinding = VK_POOL_MARK_BINDING, .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &di};
        vkUpdateDescriptorSets(c->device, 1, &w, 0, NULL);
    }
    __atomic_store_n(&PL.marks_ready, 1, __ATOMIC_RELEASE);   // the recording thread reads the rest after this
}

static void pool_marks_release(VkCtx *c) {   // vk_exec_draw_shutdown: a probe ends with the device idle
    if (!PL.marks) return;
    __atomic_store_n(&PL.marks_ready, 0, __ATOMIC_RELEASE);
    vmaDestroyBuffer(c->vma, PL.marks, PL.marks_alloc);
    for (int i = 0; i < MARKS_RB; i++)
        if (PL.rb[i].buf) vmaDestroyBuffer(c->vma, PL.rb[i].buf, PL.rb[i].alloc), PL.rb[i].buf = VK_NULL_HANDLE;
    PL.marks = VK_NULL_HANDLE;
}

void vk_exec_pool_marks(VkExec *ex) {
    if (!__atomic_load_n(&PL.marks_ready, __ATOMIC_ACQUIRE)) return;
    VkCtx *c = vk_ctx();
    uint64_t done = vk_sync_fence_done();
    while (PL.rb_n) {   // the copies the GPU completed, in submission order
        int i = (PL.rb_head - PL.rb_n + MARKS_RB) % MARKS_RB;
        if (PL.rb[i].fence > done) break;
        vmaInvalidateAllocation(c->vma, PL.rb[i].alloc, 0, VK_WHOLE_SIZE);
        vk_pool_marks_read(PL.rb[i].p, VK_POOL_TEX);
        PL.rb_n--;
    }
    if (++PL.marks_since < MARKS_EVERY || PL.rb_n == MARKS_RB) return;
    PL.marks_since = 0;
    int i = PL.rb_head;
    PL.rb_head = (PL.rb_head + 1) % MARKS_RB, PL.rb_n++;
    PL.rb[i].fence = ex->fence;
    VkCommandBuffer cb = ex->cb;
    // the shaders' stores before the copy; the copy's read before the clear
    VkMemoryBarrier2 mb[2] = {
        {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
         .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT}};
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = mb};
    vkCmdPipelineBarrier2(cb, &dep);
    VkBufferCopy region = {0, 0, (VkDeviceSize)VK_POOL_TEX * 4};
    vk_gpu_gap_note(VK_GAP_POOL_MARKS, region.size);
    vkCmdCopyBuffer(cb, PL.marks, PL.rb[i].buf, 1, &region);
    mb[0] = (VkMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT};
    vkCmdPipelineBarrier2(cb, &dep);
    vkCmdFillBuffer(cb, PL.marks, 0, VK_WHOLE_SIZE, 0);
    // the clear before later shaders' stores, and the copy visible to the host
    mb[0] = (VkMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    mb[1] = (VkMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                               .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    dep.memoryBarrierCount = 2;
    vkCmdPipelineBarrier2(cb, &dep);
}

static PlatMutex pool_lock_ = PLAT_MUTEX_INIT;

VkDescriptorSetLayout vk_pool_set_layout(void) {
    plat_mutex_lock(&pool_lock_);
    if (!PL.layout) PL.layout = pool_layout_make();
    plat_mutex_unlock(&pool_lock_);
    return PL.layout;
}

VkDescriptorSetLayout vk_empty_set_layout(void) {
    plat_mutex_lock(&pool_lock_);
    if (!PL.empty) {
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        if (vkCreateDescriptorSetLayout(vk_ctx()->device, &li, NULL, &PL.empty) != VK_SUCCESS) rt_fatal("vk: empty set layout failed");
    }
    plat_mutex_unlock(&pool_lock_);
    return PL.empty;
}

// The translator's sampler type (GfxShaderRes.type) of pool kind k.
static uint16_t pool_kind_type(int k) {
    static const uint16_t dim[4] = {1, 3, 4, 5};   // 1D, 2D, 3D, CUBE
    return (uint16_t)(dim[k >> 1 & 3] | (k & 1 ? 0x100 : 0) | (k & 8 ? 0x400 : 0));
}

#define POOL_WBATCH 512
// Texture entries a pool sync may create (images and uploads), the rest at the next sync (NX2NAVI_POOL_CREATE_BUDGET,
// default 16). The sync holds the texture lock throughout, and with 64 streaming in new textures made frames of 20-40 ms
// with the replay and other threads waiting on that lock (24 ms of holds per slow frame in a dense area); 16 brought
// that to 5 ms with no pop-in a player could see. The slots it leaves keep their stand-in until the next sync.
// An area load is not streaming. It leaves thousands of slots at once (4,925 in the load measured below), and at the
// streaming budget they came in over a hundred frames and more: the first seconds out of a load screen showed the
// ground, the walls and the buildings as their black stand-ins. So while a sync leaves NX2NAVI_POOL_LOAD_BACKLOG slots
// or more (default 512; 0: never) the pool is loading: the count budget is off, a frame's refreshes may take
// NX2NAVI_POOL_LOAD_MS (default 50), and the presents of those frames are left out so the picture before the load
// stays up (vk_backend.c frame_hold, vk_exec_pool_loading) until fewer than POOL_LOAD_END slots are left. The load
// screen lasts those few frames longer and the area appears whole.
#define POOL_LOAD_END 64
static volatile int32_t pool_loading;   // replay: a sync left an area load's worth of slots, and they are not made yet
static uint64_t pool_load_made, pool_load_frame0;
int vk_exec_pool_loading(void) { return __atomic_load_n(&pool_loading, __ATOMIC_RELAXED); }
static int pool_load_backlog(void) {
    static int n = -1;
    if (n < 0) n = cfg_int("NX2NAVI_POOL_LOAD_BACKLOG");
    return n;
}

static int pool_create_budget(void) {
    static int n = -1;
    if (n < 0) {
        n = cfg_int("NX2NAVI_POOL_CREATE_BUDGET");
        if (n < 1) n = 1;
    }
    return pool_loading ? 1 << 20 : n;
}

// The refresh runs before every draw that binds the pools, not once per submission, so the count above held for each
// draw: an area load made 4,925 entries (1.7 GB of uploads) in one 430 ms frame, and while slots were left over every
// draw walked the whole table and looked each of them up again (key and hash), about 100 ms of that frame on its own.
// Once a frame's refreshes have spent NX2NAVI_POOL_CREATE_MS (default 6; 0: no limit) making textures, its later ones
// create nothing, and skip the slots left earlier in the frame (pool_defer) instead of looking them up again; the
// first refresh of each frame keeps its count budget. The slots left keep their stand-in, a few frames longer in a
// large load.
// The time counted is what the lookups spend making entries and uploading them the first time
// (vk_texture_pool_make_ns_take). It was the whole refresh, with its check of every entry the pools hold: in Kakariko
// that check alone takes 0.8 ms a submission, six submissions a frame, so most of the 6 ms was gone before a texture
// was made, and the frame's later refreshes made none. The game moves about thirty of its textures to new storage
// every four or five frames there; the ones a refresh left were on screen as their grey stand-ins for a frame (three
// lamp posts in one recorded frame), 87 refreshes in 600 frames with 1,090 slots left between them.
static uint64_t pool_create_ns(void) {
    static int64_t ns = -1, load_ns;
    if (ns < 0) {
        int ms = cfg_int("NX2NAVI_POOL_CREATE_MS"), lms = cfg_int("NX2NAVI_POOL_LOAD_MS");
        load_ns = (int64_t)(lms < 1 ? 1 : lms) * 1000000;
        ns = (int64_t)(ms < 0 ? 0 : ms) * 1000000;
    }
    return (uint64_t)(pool_loading && ns ? load_ns : ns);
}
static uint32_t pool_defer[VK_POOL_TEX];   // per slot: 1 + the low bits of the frame a refresh left it in
// The slots a refresh left (pool_refill failed: the creation budget or the frame's time was spent), so that the next
// refresh of the same submission looks up these and the changed slots (vk_texture_dirty_slots_locked) instead of
// walking the whole table again. While an area loads, slots are left at every refresh, and each refresh walked the
// table: about 67,000 slot reads and 1.65 ms of the replay per submission in Kokiri Forest on an RX 580 (2026-11-11),
// with six submissions a frame. pool_left_all: the list overflowed (or nothing tracks it yet): walk the table.
static uint32_t pool_left[VK_POOL_TEX], pool_nleft;
static uint8_t pool_is_left[VK_POOL_TEX];
static int pool_left_all = 1;
static void pool_left_add(uint32_t id) {
    if (id >= VK_POOL_TEX || pool_is_left[id]) return;
    pool_is_left[id] = 1, pool_left[pool_nleft++] = id;
}
static void pool_left_clear(void) {
    for (uint32_t i = 0; i < pool_nleft; i++) pool_is_left[pool_left[i]] = 0;
    pool_nleft = 0;
}
static VkWriteDescriptorSet pool_w[POOL_WBATCH];
static VkDescriptorImageInfo pool_ii[POOL_WBATCH];
static uint32_t pool_nw;

static void pool_flush(void) {
    if (pool_nw) vkUpdateDescriptorSets(vk_ctx()->device, pool_nw, pool_w, 0, NULL);
    PL.writes += pool_nw;
    pool_nw = 0;
}

static void pool_write_to(VkDescriptorSet set, uint32_t binding, uint32_t id, VkImageView view, VkSampler samp) {
    if (pool_nw == POOL_WBATCH) pool_flush();
    pool_ii[pool_nw] = (VkDescriptorImageInfo){samp, view, VK_IMAGE_LAYOUT_GENERAL};
    pool_w[pool_nw] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
                                             .dstBinding = binding, .dstArrayElement = id, .descriptorCount = 1,
                                             .descriptorType = view ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER,
                                             .pImageInfo = &pool_ii[pool_nw]};
    pool_nw++;
}

// An entry whose wanted value PL.view / PL.samp already hold: into the current set, and into the log for the others
// (the first fill goes into every set).
static void pool_write(uint32_t binding, uint32_t id, VkImageView view, VkSampler samp) {
    if (PL.all) {
        for (int i = 0; i < PL.nsets; i++) pool_write_to(PL.sets[i], binding, id, view, samp);
        return;
    }
    pool_write_to(PL.set, binding, id, view, samp);
    uint32_t e = binding * VK_POOL_TEX + id;
    PL.last[e] = PL.logn;
    PL.log[PL.logn++ % POOL_LOG] = e;
}

// Writes entry e (binding * VK_POOL_TEX + id) of set as PL.view / PL.samp hold it now.
static void pool_entry_to(VkDescriptorSet set, uint32_t e) {
    uint32_t binding = e / VK_POOL_TEX, id = e % VK_POOL_TEX;
    if (binding < VK_POOL_KINDS) {
        VkImageView v = PL.view[(size_t)binding * VK_POOL_TEX + id];
        if (v) pool_write_to(set, binding, id, v, VK_NULL_HANDLE);
    } else if (id < VK_POOL_SAMP && PL.samp[id]) {
        pool_write_to(set, binding, id, VK_NULL_HANDLE, PL.samp[id]);
    }
}

// A submission's first pool use: the next set of the ring, once the GPU is done with it. 1 when it switched; *from is
// the log position the set holds writes up to (pool_catch_up brings it level once pool_sync has refreshed the entries:
// a view written into a set must be alive, and the refresh is what replaces the views of destroyed textures).
static int pool_current(void);
static volatile uint64_t pool_takes;   // sets taken (vk_pool_takes): counted once the take's pool_sync is done

static int pool_take_set(VkExec *ex, uint64_t *from) {
    // a sealed set stays bound while it is current: reading it is what the pending part does too
    if (PL.cur_fence == ex->fence && (!PL.sealed || pool_current())) return 0;
    int i = (PL.cur + 1) % PL.nsets;
    if (PL.nsets > 1) {
        const GfxBackend *b = gfx_backend();
        if (b->fence_done() < PL.set_fence[i]) PL.waits++, b->fence_wait(PL.set_fence[i], UINT64_MAX);
    }
    *from = PL.set_synced[i];
    PL.cur = i, PL.set = PL.sets[i], PL.cur_fence = ex->fence, PL.set_fence[i] = ex->fence, PL.sealed = 0;
    return PL.nsets > 1;
}

int vk_exec_pool_split(uint64_t fence) {
    if (PL.cur_fence != fence || PL.sealed) return 1;   // no part of this submission has the current set unsealed
    // pool_take_set moves to the next set of the ring: it must not be bound by this submission's earlier parts too
    if (PL.nsets < 2 || PL.set_fence[(PL.cur + 1) % PL.nsets] == fence) return 0;
    PL.sealed = 1;
    return 1;
}

uint64_t vk_pool_takes(void) { return __atomic_load_n(&pool_takes, __ATOMIC_ACQUIRE); }

// The current set gets the entries written into other sets in [from, to) of the log, as PL.view / PL.samp hold them now.
static uint64_t pool_memo_hits;   // pool_fill: slots that shared another slot's lookup (defined with pool_memo below)
static uint64_t pool_memo_same;   // of those, slots whose entries held the shared views already

static void pool_catch_up(uint64_t from, uint64_t to) {
    VkDescriptorSet set = PL.set;
    if (to - from > POOL_LOG) {   // the log has wrapped since: every entry
        PL.rewrites++;
        for (uint32_t e = 0; e < VK_POOL_KINDS * VK_POOL_TEX + VK_POOL_SAMP; e++) pool_entry_to(set, e);
    } else {
        // each entry once, at its newest write: an entry written again later (also by this submission's own sync,
        // which wrote it into this set already) is skipped at the older positions
        PL.catchup += to - from;
        for (uint64_t q = from; q < to; q++) {
            uint32_t e = PL.log[q % POOL_LOG];
            if (PL.last[e] == q) pool_entry_to(set, e), PL.written++;
        }
    }
    pool_flush();
    PL.switches++;
    if (PL.switches % 3600 == 0) {   // diagnosis: what invalidates the slots (vk_texture.c vk_texture_bucket_stats)
        char bs[1024];
        vk_texture_bucket_stats(bs, sizeof bs);
        rt_log("vk: texture pool buckets over 3600 switches: %s", bs);
    }
    if (PL.switches % 3600 == 0)
        rt_log("vk: texture pool sets: %llu switches, %llu waited for the GPU, %.1f log entries and %.1f writes per "
               "catch-up, %llu whole rewrites; %.3f ms catch-up and %.3f ms entry refresh per switch (%.3f ms of it in "
               "%.2f walks over every slot, %.3f ms in %.2f checks of %.0f listed entries and %d slots without one "
               "(%.3f ms in %.2f walks over those); "
               "%.0f slots, %.0f refilled: %.0f registered, %.0f address changes, %.0f "
               "shared a lookup, %.0f of them holding its views already; %.0f checked); %llu identical registrations kept; "
               "%llu refreshes created nothing for their frame's time (pool_create_ms), %llu lookups of slots left in the frame skipped",
               (unsigned long long)PL.switches, (unsigned long long)PL.waits, (double)PL.catchup / (double)PL.switches,
               (double)PL.written / (double)PL.switches, (unsigned long long)PL.rewrites,
               PL.catch_ns / 1e6 / (double)PL.switches, PL.sync_ns / 1e6 / (double)PL.switches,
               PL.full_ns / 1e6 / (double)PL.switches, (double)PL.n_full / (double)PL.switches,
               PL.ep_ns / 1e6 / (double)PL.switches, (double)PL.n_ep / (double)PL.switches,
               PL.n_ep ? (double)PL.n_ep_ent / (double)PL.n_ep : 0.0, PL.nunres,
               PL.unres_ns / 1e6 / (double)PL.switches, (double)PL.n_unres_walks / (double)PL.switches,
               (double)PL.n_slots / (double)PL.switches, (double)PL.n_refill / (double)PL.switches,
               (double)PL.n_by_reg / (double)PL.switches, (double)PL.n_by_addr / (double)PL.switches,
               (double)pool_memo_hits / (double)PL.switches, (double)pool_memo_same / (double)PL.switches,
               (double)PL.n_fresh / (double)PL.switches, (unsigned long long)vk_texture_same_regs(),
               (unsigned long long)PL.n_frame_stops, (unsigned long long)PL.n_defer_skips);
    if (PL.switches % 3600 == 0)
        rt_log("vk: texture pool dirty walks per switch: %.2f at a submission's start (%.0f slots looked at, %.0f "
               "refilled, %.3f ms), %.2f within one (%.0f looked at, %.0f refilled, %.3f ms)",
               (double)PL.n_walk[0] / (double)PL.switches, (double)PL.walk_visit[0] / (double)PL.switches,
               (double)PL.walk_refill[0] / (double)PL.switches, PL.walk_ns[0] / 1e6 / (double)PL.switches,
               (double)PL.n_walk[1] / (double)PL.switches, (double)PL.walk_visit[1] / (double)PL.switches,
               (double)PL.walk_refill[1] / (double)PL.switches, PL.walk_ns[1] / 1e6 / (double)PL.switches);
    if (PL.switches % 3600 == 0)
        rt_log("vk: texture pool slot groups per switch (NX2NAVI_POOL_GROUP): %.0f walked, %.0f left alone with %.0f "
               "slots, %.0f first members refilled (%.0f changed), %.0f not known to hold one resolution; check "
               "(NX2NAVI_POOL_GROUP_CHECK): %llu writes in %llu groups left alone so far, %llu more to members registered "
               "since their last fill",
               (double)PL.n_gwalk / (double)PL.switches, (double)PL.n_gskip / (double)PL.switches,
               (double)PL.n_gskip_slots / (double)PL.switches, (double)PL.n_grep / (double)PL.switches,
               (double)PL.n_gchanged / (double)PL.switches, (double)PL.n_gnouni / (double)PL.switches,
               (unsigned long long)PL.n_gchk_writes, (unsigned long long)PL.n_gchk_groups,
               (unsigned long long)PL.n_gchk_pending);
}

// Entry id of every kind in mask: the slot's view of that kind or the kind's stand-in. Texture lock held. 0: the
// slot's texture was left to a later sync by the creation budget (pool_sync).
// NX2NAVI_SKIP_SHADERS=<hash>,...: draws and dispatches whose shader's SPIR-V has one of these FNV-1a hashes (the "vs"/"fs"/
// "cs" hashes of the frame dump's passes.txt) are skipped (diagnosis: which pass makes an effect).
static int shader_skipped(const GfxShader *sh) {
    static uint64_t list[16];
    static int n = -1;
    static struct { const GfxShader *s; uint8_t skip; } memo[256];
    if (n < 0) {
        n = 0;
        const char *e = cfg_get("NX2NAVI_SKIP_SHADERS");
        for (const char *p = e; p && *p && n < 16;) {
            char *end;
            uint64_t v = strtoull(p, &end, 16);
            if (end == p) break;
            list[n++] = v;
            p = *end ? end + 1 : end;
        }
        if (n) rt_log("vk: NX2NAVI_SKIP_SHADERS: %d shader hashes skipped", n);
    }
    if (!n || !sh) return 0;
    uint32_t i = (uint32_t)(((uintptr_t)sh * 0x9E3779B97F4A7C15ull) >> 56);
    if (memo[i].s == sh) return memo[i].skip;
    uint64_t h = gfx_shader_spirv_fnv(sh);   // as vk_dump.c shader_hash
    uint8_t skip = 0;
    for (int k = 0; k < n; k++) skip |= list[k] == h;
    memo[i].s = sh, memo[i].skip = skip;
    return skip;
}

// NX2NAVI_POOL=0: draws and dispatches of pipelines that index the pools are skipped (A/B against bindless shaders).
static int pool_on(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("POOL", "1");
    return on;
}

// Slots that resolve alike share one lookup (vk_texture_slot_khash_locked): Ocarina keeps ~400 descriptors of each
// transient render target registered over its storage, and every finalize and initialize of that target changes their
// tokens, so a refill looked up the same texture ~400 times, ~15,000 lookups a frame in Kokiri Forest. An entry holds the
// views one slot got, for the slots with its hash, address generation and entry generation.
// Its stamp changes only when one of its views does (NX2NAVI_POOL_FAST, default on): a slot whose entries were written
// from the entry at the stamp it has now (PoolSlot) holds those views already, so a refill that finds the entry so
// compares and writes nothing. That is most refills: a transient render target released and taken back moves the
// address generation of its storage, ~1,300 slots in Kokiri Forest, and every slot resolves as before (18,500 of 18,800
// refills a submission shared a lookup, each comparing up to 16 views a quarter megabyte apart: 1.9 ms).
#define POOL_MEMO 4096
static struct { uint64_t kh; uint32_t agen, egen, mask, stamp, got, ent; VkImageView view[VK_POOL_KINDS]; } pool_memo[POOL_MEMO];
static uint32_t pool_stamp;   // the last stamp given out
static uint64_t pool_memo_hits, pool_memo_same;

static int pool_fast(void) {   // ab: in the B blocks
    static int on = -1;
    if (on < 0) on = rt_knob("POOL_FAST", "1");
    return RT_KNOB_ON(on);
}

// Slot id's last fill resolved to the entry ent (vk_texture_pool_note_locked; 0: none). vk_texture.c counts the slots
// that hold each entry: the entries held are the ones the walk over every slot reached with a lookup in each
// submission, and the check by list leaves their use stamps from that count (vk_texture_pool_hold_locked).
static void pool_hold(uint32_t id, uint32_t ent) {
    if (PL.ent[id] == ent) return;
    vk_texture_pool_hold_locked(PL.ent[id], ent);
    PL.ent[id] = ent;
}

// Slot id's last fill resolved to no entry (got 0) or to one: the slots without one are looked up again each
// submission (pool_sync_entries), as the walk over every slot did: an entry their storage could not have then may be
// made now, and its address generation then refills them. A slot that resolved keeps its place in the list until the
// next walk drops it; when it stops resolving before that walk, that place serves again. Listing it a second time
// made the list grow by one entry per such change (160,000 entries for the 32,768 slots after a minute in the Market),
// and every 8th submission's walk over it then held the texture lock for 15 to 30 ms.
static void pool_unres(uint32_t id, int got) {
    if (got || PL.unres_on[id]) {
        if (got) PL.unres_on[id] = 0;
        return;
    }
    PL.unres_on[id] = 1;
    if (PL.unres_in[id]) return;
    PL.unres_in[id] = 1;
    if (PL.nunres == PL.cunres) {
        int n = PL.cunres ? PL.cunres * 2 : 1024;
        int *p = realloc(PL.unres, sizeof *p * (size_t)n);
        if (!p) rt_fatal("vk: out of memory");
        PL.unres = p, PL.cunres = n;
    }
    PL.unres[PL.nunres++] = (int)id;
}

// For the flight recorder: v is one of the stand-in views.
static int pool_view_blank(VkImageView v) {
    for (int i = 0; i < DV_COUNT; i++)
        if (v == Z.img[i].view || v == Z.img[i].sview) return 1;
    return 0;
}

// For the flight recorder's blank-slot events: 16384 when no pipeline made so far reads the pool as a volume (kind =
// depth * 8 + dim * 2 + arrayed, vk.h), so a slot that holds a volume has a stand-in in every kind that is read.
static uint32_t pool_no_3d(void) { return __atomic_load_n(&vk_pool_kinds_used, __ATOMIC_RELAXED) & 0x3030u ? 0 : 16384u; }

// Slot id's entry of a kind went from the view was to now: noted when a texture's view gives way to the stand-in, which
// every draw of the submission that samples the slot then reads (the set is written with update-after-bind).
static uint32_t pool_blanked[512];   // the slots that lost their picture since the last pool_blank_check
static int pool_nblanked;
static void pool_note_blank(uint32_t id, VkImageView was, VkImageView now, int *noted) {
    if (!flight_on || *noted || !was || pool_view_blank(was) || !pool_view_blank(now)) return;
    uint64_t addr;
    uint32_t fmt, w, h, why = vk_texture_slot_why_locked((int)id, &addr, &fmt, &w, &h) | pool_no_3d();
    flight_tex(FLIGHT_TEX_TOBLANK, addr, fmt, w, h, 0, 0, why, id);
    if (pool_nblanked < (int)(sizeof pool_blanked / sizeof *pool_blanked)) pool_blanked[pool_nblanked++] = id;
    *noted = 1;
}

// A submission's first sync, before it changes anything: the slots that lost their picture during the submission
// before and have none in any kind now went to the queue with their stand-ins. Texture lock taken here.
static void pool_blank_check(void) {
    if (!pool_nblanked) return;
    vk_texture_lock();
    for (int i = 0; i < pool_nblanked; i++) {
        uint32_t id = pool_blanked[i];
        int real = 0;
        for (int k = 0; k < VK_POOL_KINDS && !real; k++) {
            VkImageView v = PL.view[(size_t)k * VK_POOL_TEX + id];
            real = v && !pool_view_blank(v);
        }
        if (real) continue;
        uint64_t addr;
        uint32_t fmt, w, h, why = vk_texture_slot_why_locked((int)id, &addr, &fmt, &w, &h) | pool_no_3d();
        flight_tex(FLIGHT_TEX_BLANKSUB, addr, fmt, w, h, 0, 0, why, id);
    }
    vk_texture_unlock();
    pool_nblanked = 0;
}

// Whether slot id's entry of a kind in mask is a texture's view now (vk_texture_pool_shown_locked).
static int pool_slot_shown(uint32_t id, uint32_t mask) {
    for (int k = 0; k < VK_POOL_KINDS; k++) {
        if (!(mask >> k & 1)) continue;
        VkImageView v = PL.view[(size_t)k * VK_POOL_TEX + id];
        if (v && !pool_view_blank(v)) return 1;
    }
    return 0;
}

static int pool_fill(VkExec *ex, uint32_t id, uint32_t mask, uint64_t tok) {
    int noted = 0;
    vk_texture_create_deferred_locked();
    PoolSlot *ps = &PL.ps[id];
    uint64_t kh = 0;
    if (tok) {   // the descriptor hash changes only with a registration (the token's upper half)
        uint32_t reg = (uint32_t)(tok >> 32);
        if (ps->kreg != reg || !ps->kh) ps->kh = vk_texture_slot_khash_locked((int)id), ps->kreg = reg;
        kh = ps->kh;
    }
    uint32_t ag = (uint32_t)tok;
    typeof(pool_memo[0]) *m = kh ? &pool_memo[(kh >> 20) % POOL_MEMO] : NULL;
    if (m && m->kh == kh && m->agen == ag && m->egen == vk_texture_entry_gen() && (m->mask & mask) == mask) {
        pool_memo_hits++;
        pool_unres(id, (int)m->got);
        pool_hold(id, m->ent);
        if (ps->stamp && ps->stamp == m->stamp && (ps->fmask & mask) == mask && pool_fast()) {
            pool_memo_same++;
            return 1;
        }
        for (int k = 0; k < VK_POOL_KINDS; k++) {
            if (!(mask >> k & 1)) continue;
            VkImageView *e = &PL.view[(size_t)k * VK_POOL_TEX + id];
            if (*e != m->view[k]) {
                pool_note_blank(id, *e, m->view[k], &noted);
                *e = m->view[k], pool_write((uint32_t)k, id, m->view[k], VK_NULL_HANDLE);
            }
        }
        ps->fmask = ps->stamp == m->stamp ? ps->fmask | mask : mask;
        ps->stamp = m->stamp;
        return 1;
    }
    vk_texture_pool_shown_locked(tok && pool_slot_shown(id, mask));
    for (int k = 0; k < VK_POOL_KINDS; k++) {
        if (!(mask >> k & 1)) continue;
        GfxShaderRes r = {0};
        r.type = pool_kind_type(k);
        VkDescriptorImageInfo ii;
        VkImage img;
        tex_info_full(ex, &r, tok ? id | 1ull << 63 : 0, 0, 1, &ii, &img);
        VkImageView *e = &PL.view[(size_t)k * VK_POOL_TEX + id];
        if (*e != ii.imageView) {
            pool_note_blank(id, *e, ii.imageView, &noted);
            *e = ii.imageView, pool_write((uint32_t)k, id, ii.imageView, VK_NULL_HANDLE);
        }
    }
    vk_texture_pool_shown_locked(0);
    int ok = !vk_texture_create_deferred_locked();
    uint32_t ent = 0;
    int got = vk_texture_pool_note_locked((int)id, &ent);
    if (tok) pool_unres(id, got);
    pool_hold(id, tok ? ent : 0);
    ps->stamp = 0;   // its entries match no memo entry, unless the one below
    if (m && ok) {   // the generation after the lookups: an entry they created is part of what they resolved
        uint32_t eg = vk_texture_entry_gen();
        // a view that changes gives the entry a new stamp; one kept outside the mask keeps its value, so a kind that
        // comes back with another view is a change too
        int changed = !m->stamp;
        for (int k = 0; k < VK_POOL_KINDS; k++)
            if (mask >> k & 1) {
                VkImageView v = PL.view[(size_t)k * VK_POOL_TEX + id];
                if (m->view[k] != v) m->view[k] = v, changed = 1;
            }
        if (m->kh != kh || m->agen != ag || m->egen != eg) m->kh = kh, m->agen = ag, m->egen = eg, m->mask = 0;
        m->mask |= mask;
        m->got = (uint32_t)got, m->ent = ent;
        if (changed) {
            if (!++pool_stamp) pool_stamp = 1;
            m->stamp = pool_stamp;
        }
        ps->stamp = m->stamp, ps->fmask = mask;
    }
    return ok;
}

static void pool_sync_entries(VkExec *ex);

static void pool_sync(VkExec *ex);

// A chunk of a parallel group (vk_exec.c "replay workers") binds the set as vk_exec_draw_group_begin left it, up to date
// when the group began; the chunk that takes this submission's first set takes it in turn, under pool_mt. Elsewhere the
// replay syncs the pools at every draw that binds them, as before, under pool_mt past a group whose chunks still record
// (NX2NAVI_REPLAY_PIPE): the set is the submission's, read by the GPU when the part runs.
static void pool_sync_draw(VkExec *ex) {
    if (!vk_exec_shared()) return pool_sync(ex);
    if (!vk_exec_in_chunk()) {
        plat_mutex_lock(&pool_mt);
        pool_sync(ex);
        plat_mutex_unlock(&pool_mt);
        return;
    }
    if (__atomic_load_n(&PL.cur_fence, __ATOMIC_ACQUIRE) == ex->fence) return;
    vk_exec_turn();
    plat_mutex_lock(&pool_mt);
    pool_sync(ex);
    plat_mutex_unlock(&pool_mt);
}

static int mesh_ring_make(void);

uint32_t vk_exec_draw_group_begin(VkExec *ex) {
    dummies_init();
    if (vk_ctx()->has_mesh_shader && !vk_ctx()->mesh_fallback) mesh_ring_make();   // the chunks' indirect mesh draws
    if (!__atomic_load_n(&LIM.ubo_max, __ATOMIC_ACQUIRE)) limits_init();
    if (pool_on() && PL.cur_fence == ex->fence && !PL.sealed) {   // the set this submission took
        int lk = vk_exec_shared();   // earlier groups may still record (NX2NAVI_REPLAY_PIPE)
        if (lk) plat_mutex_lock(&pool_mt);
        pool_sync(ex);
        if (lk) plat_mutex_unlock(&pool_mt);
    }
    return UPD.fence == ex->fence ? UPD.n : 0;
}

static void pool_sync(VkExec *ex) {
    if (!PL.layout) vk_pool_set_layout();
    uint64_t from = 0, pre = PL.logn;
    int took = PL.cur_fence != ex->fence;
    if (took && flight_on) pool_blank_check();   // what the submission before went to the queue with
    int switched = pool_take_set(ex, &from);   // every submission its own set, before the shortcut
    if (PL.sealed) return;   // current when checked: a change since waits for the next sync, which takes a new set
    uint64_t t0 = kern_now_ns(), f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (f != PL.frame) PL.frame = f, PL.frame_ns = 0;
    vk_texture_pool_make_ns_take();   // what other lookups left: only this sync's making counts for the frame
    pool_sync_entries(ex);
    uint64_t t1 = kern_now_ns();
    PL.sync_ns += t1 - t0;
    PL.frame_ns += vk_texture_pool_make_ns_take();
    if (switched) {
        vk_texture_lock();
        pool_catch_up(from, pre);   // what the other sets got since; this sync's own writes went into it already
        vk_texture_unlock();
        PL.catch_ns += kern_now_ns() - t1;
    }
    PL.set_synced[PL.cur] = PL.logn;
    // A set taken and up to date (with one set, the refresh above wrote into it directly), counted only when this sync
    // left no slot behind: once the frame's creation time is spent, pool_refill skips a slot it left earlier in the frame
    // and pool_sync_entries may return before the walk, and such a slot keeps its old view in this set with nothing in
    // the log, so the takes after it would not carry the change into the other sets. After a counted take, every slot
    // change made before it is in this set and in the log; vk_texture.c retire_sweep frees a retired image only after
    // more than VK_POOL_SETS of them, so no set of the ring still holds its view.
    if (took && !PL.retry) __atomic_fetch_add(&pool_takes, 1, __ATOMIC_RELEASE);
}

typedef struct { VkExec *ex; uint32_t used; int n, retry, spent; } PoolDirty;

// A slot whose token changed: filled, or noted as left (pool_defer). With the frame's creation time spent (spent), a
// slot left earlier in the frame is not looked up again. A left slot holds the stand-ins (its lookup resolved to
// nothing), and a fill clears the note: kept, a slot left and then filled in the same frame was skipped later in that
// frame with the filled texture's view still in the set, also after that texture was destroyed (a released one dropped,
// one evicted), and a shader sampling the slot read freed memory.
static int pool_refill(VkExec *ex, uint32_t id, uint32_t used, uint64_t t, int spent) {
    uint32_t mark = (uint32_t)PL.frame + 1;
    if (spent && pool_defer[id] == mark) return PL.n_defer_skips++, 0;
    if (pool_fill(ex, id, used, t)) return pool_defer[id] = 0, 1;
    pool_defer[id] = mark;
    return 0;
}

static void pool_dirty_slot(int id, void *arg) {   // vk_texture_dirty_slots_locked: a slot whose token may have changed
    PoolDirty *d = arg;
    PL.n_visit++;
    if (id >= d->n) return;
    uint64_t t = vk_texture_slot_token(id);
    if (t == PL.tok[id]) return;
    PL.n_refill++;
    if (t >> 32 != PL.tok[id] >> 32) PL.n_by_reg++;
    else PL.n_by_addr++;
    if (pool_refill(d->ex, (uint32_t)id, d->used, t, d->spent)) PL.tok[id] = t;
    else d->retry++, pool_left_add((uint32_t)id);
}

// Every entry as the slots, kinds and samplers are now (pool_sync_entries has nothing to write).
static int pool_current(void) {
    return PL.ready && !PL.retry && __atomic_load_n(&vk_pool_kinds_used, __ATOMIC_RELAXED) == PL.kinds &&
           vk_texture_slots_gen() == PL.gen && vk_texture_epoch() == PL.epoch && vk_sampler_gen() == PL.sgen;
}

// NX2NAVI_POOL_GROUP=1 (default), 0, ab: a dirty bucket's slot groups (vk_texture.c) are walked whole. The members of a
// group resolve alike, so once a walk of the group brought every member to the current resolution (every fill done, an
// entry for each) they all hold the same views, and later the group is left alone when its first member still holds the
// current resolution: its token did not change (its views are what that token resolves to), or its refill wrote nothing.
// A slot that joined since is at the tail and refilled on its own (its registration's entry on the dirty ring).
// The members keep their older tokens: their views are what those tokens resolved to, and what the current ones do.
// (Comparing the memo's stamps instead left half the groups to their members: the memo's entries are shared between
// hashes, so the same views came back under a new stamp.)
static int pool_group(void) {
    static int mode = -1;
    if (mode < 0) mode = rt_knob("POOL_GROUP", "1");
    return RT_KNOB_ON(mode);
}

// NX2NAVI_POOL_GROUP_CHECK=1: a group left alone is walked member by member all the same, and the writes that walk
// makes are counted (none when the rule holds; the pool log's slot groups line), but for the members registered since
// their last fill: their registration's entry on the dirty ring refills them in the same walk (counted apart).
static int pool_group_check(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("POOL_GROUP_CHECK", "0") == 1;   // a diagnosis: no settings row
    return on;
}

static void pool_dirty_group(int g, void *arg) {
    PoolDirty *d = arg;
    uint64_t chk = 0;
    VkSlotGroup *G = vk_texture_group_locked(g);
    int rep = G->head;
    PL.n_gwalk++;
    if (!G->stamp) PL.n_gnouni++;
    if (G->stamp && (G->fmask & d->used) == d->used && rep >= 0 && rep < d->n && !PL.unres_on[rep]) {
        uint64_t t = vk_texture_slot_token(rep);
        int same = t == PL.tok[rep];
        if (!same) {   // the first member's refill: nothing written, an entry found, its token taken
            uint64_t l0 = PL.logn;
            PL.n_grep++;
            pool_dirty_slot(rep, arg);
            same = PL.tok[rep] == t && PL.logn == l0 && !PL.unres_on[rep];
            if (!same) PL.n_gchanged++;
        }
        if (same) {
            PL.n_gskip++, PL.n_gskip_slots += (uint64_t)G->n;
            if (!pool_group_check()) return;
            chk = PL.logn + 1;
        }
    }
    int r0 = d->retry, uni = 1;   // member by member: then all hold the current resolution, unless a fill was left
    d->retry = 0;
    for (int id = G->head; id >= 0; id = vk_texture_group_next_locked(id)) {
        if (id >= d->n) continue;   // outside the pool
        if (chk) {   // the check: a member registered since its last fill is refilled by its own entry anyway
            uint64_t l0 = PL.logn;
            int pending = vk_texture_slot_token(id) >> 32 != PL.tok[id] >> 32;
            pool_dirty_slot(id, arg);
            if (PL.logn != l0) {
                if (pending) PL.n_gchk_pending += PL.logn - l0;
                else PL.n_gchk_writes += PL.logn - l0, PL.n_gchk_bad++;
            }
        } else {
            pool_dirty_slot(id, arg);
        }
        if (PL.unres_on[id]) uni = 0;
    }
    if (d->retry) uni = 0;
    d->retry |= r0;
    G = vk_texture_group_locked(g);
    G->stamp = (uint32_t)uni, G->fmask = uni ? d->used : 0;
    if (chk && PL.n_gchk_bad) PL.n_gchk_groups++, PL.n_gchk_bad = 0;
}

static void pool_sync_entries(VkExec *ex) {
    uint32_t used = __atomic_load_n(&vk_pool_kinds_used, __ATOMIC_RELAXED);
    uint32_t gen = vk_texture_slots_gen(), ep = vk_texture_epoch(), sg = vk_sampler_gen();
    if (PL.ready && !PL.retry && used == PL.kinds && gen == PL.gen && ep == PL.epoch && sg == PL.sgen) return;
    int spent = pool_create_ns() && PL.frame_ns >= pool_create_ns();   // this frame creates nothing more
    if (spent && PL.ready && used == PL.kinds && gen == PL.gen && ep == PL.epoch && sg == PL.sgen) return;   // left slots only
    vk_texture_lock();
    vk_texture_pool_ctx(1);   // these lookups are the pool's own: a texture evicted by disuse keeps its stand-in
    uint32_t fresh = used & ~PL.kinds;
    int n = vk_texture_slot_count_locked();
    if (n > VK_POOL_TEX) n = VK_POOL_TEX;
    if (!PL.ready) {   // every entry a stand-in first: partially bound arrays are still read out of range by guests
        PL.all = 1;
        for (uint32_t id = 0; id < VK_POOL_TEX; id++) pool_fill(ex, id, 0xffffu, 0);
        for (uint32_t id = 0; id < VK_POOL_SAMP; id++) PL.samp[id] = Z.samp, pool_write(VK_POOL_KINDS, id, VK_NULL_HANDLE, Z.samp);
        pool_flush();
        PL.all = 0;
        PL.ready = 1, PL.sgen = 0;
    }
    int retry = 0;   // slots whose texture the creation budget left: filled again by the next sync
    if (fresh || gen != PL.gen || ep != PL.epoch || PL.retry) {
        vk_texture_create_budget_locked(spent ? 0 : pool_create_budget());
        PL.n_frame_stops += (uint64_t)spent;
        // Only a slot change since the last sync (no new kinds, no new submission): the slots that
        // changed (vk_texture_dirty_slots_locked), not the whole table. With NX2NAVI_POOL_FAST (default on) a new
        // submission too: its freshness check goes through the entries the slots resolved to and the slots that
        // resolved to none (vk_texture_pool_fresh_locked, pool_unres), not every slot.
        PoolDirty pd = {ex, used, n, 0, spent};
        int newep = ep != PL.epoch;
        uint64_t v0 = PL.n_visit, f0 = PL.n_refill, w0 = kern_now_ns();
        int grp = pool_group();
        if (grp && !PL.gmode) vk_texture_groups_unstamp_locked();   // members were refilled one by one meanwhile
        PL.gmode = grp;
        // With slots left by an earlier refresh (the creation budget or the frame's time spent): those (pool_left)
        // first, then the changed slots, instead of the walk over every slot. While an area loads, slots are left at
        // every refresh, and each one walked the table: about 67,000 slot reads and 1.65 ms of the replay per
        // submission in Kokiri Forest on an RX 580 (0.7.1, 2026-11-11), six submissions a frame. pool_left_all: no
        // list kept yet (until the first walk): walk.
        int quick = !fresh && (!newep || pool_fast()) && (!PL.retry || !pool_left_all) && PL.dirty_pos;
        if (quick && PL.retry) {
            uint32_t k = 0;
            for (uint32_t i = 0; i < pool_nleft; i++) {
                uint32_t id = pool_left[i];
                uint64_t t = (int)id < n ? vk_texture_slot_token((int)id) : 0;
                int left = 0;
                if (t != PL.tok[id]) {
                    PL.n_refill++;
                    if (t >> 32 != PL.tok[id] >> 32) PL.n_by_reg++;
                    else PL.n_by_addr++;
                    if (pool_refill(ex, id, used, t, spent)) PL.tok[id] = t;
                    else left = 1;
                }
                if (left) pool_left[k++] = id;
                else pool_is_left[id] = 0;
            }
            pool_nleft = k;
            PL.n_slots += (uint64_t)k;
        }
        int walked = quick && vk_texture_dirty_slots_locked(&PL.dirty_pos, pool_dirty_slot, grp ? pool_dirty_group : NULL, &pd);
        if (walked) {
            int at = newep ? 0 : 1;
            PL.n_walk[at]++, PL.walk_visit[at] += PL.n_visit - v0, PL.walk_refill[at] += PL.n_refill - f0;
            PL.walk_ns[at] += kern_now_ns() - w0;
        }
        retry = pd.retry;
        if (walked && newep) {
            uint64_t te = kern_now_ns();
            PL.n_ep_ent += (uint64_t)vk_texture_pool_fresh_locked();
            // The slots without an entry are over storage the guest has not mapped (stale descriptors, 1,160 in
            // Kokiri Forest): looked up again when the mapping changed, while a lookup is left for the creation budget,
            // and every 8th submission for the rest (an entry whose creation failed, an image at their address that
            // took GPU content since: slot_resolve's alias rule)
            uint32_t mg = mem_map_gen();
            uint64_t tu = kern_now_ns();
            int walk_unres = mg != PL.unres_mg || PL.unres_more || !(PL.n_ep & 7);
            if (walk_unres) PL.unres_mg = mg, PL.n_unres_walks++, vk_texture_create_deferred_locked();
            for (int i = 0; walk_unres && i < PL.nunres;) {
                int id = PL.unres[i];
                uint64_t t = PL.unres_on[id] ? vk_texture_slot_token(id) : 0;
                int r = t && t == PL.tok[id] ? vk_texture_slot_retry_locked(id) : -1;
                if (r) {   // resolves now (an entry it made changes its token: refilled), was refilled since, or
                    PL.unres_on[id] = 0;   // resolves to none until its token changes (the refill lists it again)
                    PL.unres_in[id] = 0;
                    PL.unres[i] = PL.unres[--PL.nunres];
                    continue;
                }
                i++;
            }
            if (walk_unres) PL.unres_more = vk_texture_create_deferred_locked();
            PL.unres_ns += kern_now_ns() - tu;
            PL.n_ep++, PL.ep_ns += kern_now_ns() - te;
        }
        if (!walked) PL.dirty_pos = vk_texture_dirty_pos_locked(), pool_left_clear(), pool_left_all = 0;
        uint64_t tw = walked ? 0 : kern_now_ns();
        for (int id = 0; id < n && !walked; id++) {   // slots past n were never registered: stand-ins since the first sync
            uint64_t t = vk_texture_slot_token(id);
            if (t != PL.tok[id]) {
                PL.n_refill++;
                if (t >> 32 != PL.tok[id] >> 32) PL.n_by_reg++;
                else PL.n_by_addr++;
                if (pool_refill(ex, (uint32_t)id, used, t, spent)) PL.tok[id] = t;
                else retry++, pool_left_add((uint32_t)id);
            } else if (t && fresh) {
                pool_fill(ex, (uint32_t)id, fresh, t);
            } else if (t && ep != PL.epoch) {
                PL.n_fresh++;
                vk_texture_by_id_locked(id, NULL, NULL);   // the submit's freshness check
            }
        }
        if (!walked) {
            PL.n_slots += (uint64_t)n, PL.n_full++, PL.full_ns += kern_now_ns() - tw;
            vk_texture_groups_unstamp_locked();   // the walk over every slot refilled members one by one
        }
        retry = (int)pool_nleft;   // every slot left, by this refresh or an earlier one not filled since
        vk_texture_create_budget_locked(-1);
    }
    PL.retry = retry != 0;
    // retry counts every slot left (pool_left: this sync's and the earlier ones' not filled since)
    int backlog = pool_load_backlog();
    if (!pool_loading && backlog > 0 && retry >= backlog) {
        pool_load_made = (uint64_t)retry, pool_load_frame0 = PL.frame;
        __atomic_store_n(&pool_loading, 1, __ATOMIC_RELEASE);
    } else if (pool_loading && retry < POOL_LOAD_END) {
        __atomic_store_n(&pool_loading, 0, __ATOMIC_RELEASE);
        rt_log("vk: texture pool: an area load of about %llu textures was made in %llu frames (NX2NAVI_POOL_LOAD_BACKLOG)",
               (unsigned long long)pool_load_made, (unsigned long long)(PL.frame - pool_load_frame0 + 1));
    }
    if (sg != PL.sgen) {
        int ns = vk_sampler_slot_count_locked();
        for (int id = 0; id < VK_POOL_SAMP; id++) {
            VkSampler s = id < ns ? vk_sampler_by_id_locked(id) : VK_NULL_HANDLE;
            if (!s) s = Z.samp;
            if (s != PL.samp[id]) PL.samp[id] = s, pool_write(VK_POOL_KINDS, (uint32_t)id, VK_NULL_HANDLE, s);
        }
    }
    pool_flush();
    vk_texture_pool_ctx(0);
    vk_texture_unlock();
    PL.kinds = used, PL.gen = gen, PL.epoch = ep, PL.sgen = sg;
}

// ---- depth twins: colour images under a compare sampler in a compute shader
// NX2navi's compute passes sample an R32_SFLOAT texture through a shadow sampler. Maxwell compares any single-channel
// float or unorm texel; Vulkan only formats with SAMPLED_IMAGE_DEPTH_COMPARISON (VUID-vkCmdDispatch-None-06479), which
// AMD's drivers do not give R32_SFLOAT, so the result there is undefined. Such an image gets a depth twin of the same
// size and bits (R32_SFLOAT -> D32_SFLOAT, R16_UNORM -> D16_UNORM), copied through a buffer right before each dispatch
// that samples it (a dispatch is outside any render pass), and the dispatch samples the twin. Draws keep the colour
// image: a copy cannot go inside their pass.
// NX2NAVI_DEPTH_TWIN=0: off.
#define TWINS 8
typedef struct { VkImage src, img; VmaAllocation mem; VkImageView view[2]; uint32_t w, h, layers; VkFormat fmt; } Twin;
static Twin twins[TWINS];
static unsigned twin_next;
// op_dispatch*: the resources being bound are a dispatch's, on this thread (the replay workers record draws meanwhile);
// 2 when the dispatch is a network program's (uses_tensor_ops; net_binding)
static _Thread_local int twin_dispatch;
static VkBuffer twin_buf;
static VmaAllocation twin_buf_mem;
static VkDeviceSize twin_buf_cap;

static void twin_free(void *arg) {   // completion thread, after the last submission that may sample it
    Twin *t = arg;
    VkCtx *c = vk_ctx();
    for (int k = 0; k < 2; k++)
        if (t->view[k]) vkDestroyImageView(c->device, t->view[k], NULL);
    vmaDestroyImage(c->vma, t->img, t->mem);
    free(t);
}

typedef struct { VkBuffer buf; VmaAllocation mem; } TwinBuf;
static void twin_buf_free(void *arg) {   // completion thread, after the last submission that may copy through it
    TwinBuf *b = arg;
    vmaDestroyBuffer(vk_ctx()->vma, b->buf, b->mem);
    free(b);
}

static VkFormat twin_format(const VkTexImage *img) {
    static int on = -1;
    if (on < 0) on = rt_knob("DEPTH_TWIN", "1");
    if (!on || (img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || img->samples > 1 || img->depth > 1) return VK_FORMAT_UNDEFINED;
    VkFormat df = img->format == VK_FORMAT_R32_SFLOAT ? VK_FORMAT_D32_SFLOAT
                : img->format == VK_FORMAT_R16_UNORM  ? VK_FORMAT_D16_UNORM : VK_FORMAT_UNDEFINED;
    if (!df) return df;
    static int compares[2] = {-1, -1};   // per source format: the driver compares it as it is
    int i = df == VK_FORMAT_D16_UNORM;
    if (compares[i] < 0) {
        VkFormatProperties3 p3 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &p3};
        vkGetPhysicalDeviceFormatProperties2(vk_ctx()->phys, img->format, &p2);
        compares[i] = (p3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT) != 0;
        rt_log("vk_exec: %s under a compare sampler: %s", i ? "R16_UNORM" : "R32_SFLOAT",
               compares[i] ? "compared by the driver" : "sampled through a depth twin in compute passes");
    }
    return compares[i] ? VK_FORMAT_UNDEFINED : df;
}

static void twins_release(VkCtx *c) {
    for (int i = 0; i < TWINS; i++) {
        for (int k = 0; k < 2; k++)
            if (twins[i].view[k]) vkDestroyImageView(c->device, twins[i].view[k], NULL);
        if (twins[i].img) vmaDestroyImage(c->vma, twins[i].img, twins[i].mem);
    }
    memset(twins, 0, sizeof twins);
    if (twin_buf) vmaDestroyBuffer(c->vma, twin_buf, twin_buf_mem);
    twin_buf = VK_NULL_HANDLE, twin_buf_mem = NULL, twin_buf_cap = 0;
}

// The twin of img holding img's current texels, the copies recorded into ex->cb; NULL when img needs none or it cannot
// be made.
static Twin *twin_update(VkExec *ex, const VkTexImage *img) {
    VkFormat df = twin_format(img);
    if (!df) return NULL;
    VkCtx *c = vk_ctx();
    uint32_t bpp = df == VK_FORMAT_D32_SFLOAT ? 4 : 2, layers = img->layers ? img->layers : 1;
    VkDeviceSize bytes = (VkDeviceSize)img->width * img->height * layers * bpp;
    if (bytes > (64u << 20)) return NULL;
    if (bytes > twin_buf_cap) {   // one copy buffer, grown; the old one goes after the submission being recorded
        VkDeviceSize want = twin_buf_cap ? twin_buf_cap * 2 : 1u << 16;
        while (want < bytes) want *= 2;
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = want,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkBuffer nb;
        VmaAllocation nm;
        if (vmaCreateBuffer(c->vma, &bi, &ai, &nb, &nm, NULL) != VK_SUCCESS) return NULL;
        if (twin_buf) {
            TwinBuf *old = malloc(sizeof *old);
            if (!old) rt_fatal("vk: out of memory (depth twin)");
            *old = (TwinBuf){twin_buf, twin_buf_mem};
            vk_completion_add(vk_sync_issued() + 1, twin_buf_free, old);
        }
        twin_buf = nb, twin_buf_mem = nm, twin_buf_cap = want;
    }
    Twin *t = NULL;
    for (int i = 0; i < TWINS && !t; i++)
        if (twins[i].img && twins[i].src == img->image && twins[i].w == img->width && twins[i].h == img->height &&
            twins[i].layers == layers && twins[i].fmt == df)
            t = &twins[i];
    if (!t) {
        t = &twins[twin_next++ % TWINS];
        if (t->img) {   // the submission being recorded may still sample the replaced twin
            Twin *old = malloc(sizeof *old);
            if (!old) rt_fatal("vk: out of memory (depth twin)");
            *old = *t;
            vk_completion_add(vk_sync_issued() + 1, twin_free, old);
        }
        *t = (Twin){.src = img->image, .w = img->width, .h = img->height, .layers = layers, .fmt = df};
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = df,
                                .extent = {img->width, img->height, 1}, .mipLevels = 1, .arrayLayers = layers,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ci, &ai, &t->img, &t->mem, NULL) != VK_SUCCESS) {
            *t = (Twin){0};
            return NULL;
        }
        for (int k = 0; k < 2; k++) {
            VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->img,
                                        .viewType = k ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, .format = df,
                                        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, k ? layers : 1}};
            if (vkCreateImageView(c->device, &vi, NULL, &t->view[k]) != VK_SUCCESS) t->view[k] = VK_NULL_HANDLE;
        }
        static int nlog;
        if (nlog++ < 8)
            rt_log("vk_exec: depth twin %ux%ux%u %s for a compute pass's compare sampler", img->width, img->height,
                   layers, df == VK_FORMAT_D32_SFLOAT ? "D32_SFLOAT" : "D16_UNORM");
    }
    // The source's earlier writes and the twin's earlier readers before the copies; the twin's old texels are dropped.
    VkMemoryBarrier2 m0 = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = t->img, .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers}};
    VkDependencyInfo d0 = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m0,
                           .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(ex->cb, &d0);
    VkBufferImageCopy src = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers},
                             .imageExtent = {img->width, img->height, 1}};
    vk_gpu_gap_note(VK_GAP_TWIN, (uint64_t)img->width * img->height);
    vkCmdCopyImageToBuffer(ex->cb, img->image, VK_IMAGE_LAYOUT_GENERAL, twin_buf, 1, &src);
    VkMemoryBarrier2 m1 = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
    VkDependencyInfo d1 = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m1};
    vkCmdPipelineBarrier2(ex->cb, &d1);
    VkBufferImageCopy dst = {.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, layers},
                             .imageExtent = {img->width, img->height, 1}};
    vkCmdCopyBufferToImage(ex->cb, twin_buf, t->img, VK_IMAGE_LAYOUT_GENERAL, 1, &dst);
    // The twin's texels before the dispatch; the source's next writers and the buffer's next copy after these copies.
    VkMemoryBarrier2 m2 = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d2 = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m2};
    vkCmdPipelineBarrier2(ex->cb, &d2);
    ex->recorded += 5;
    return t;
}

// What a network program run natively (NX2NAVI_NATIVE_TENSOR=1, native_tensor) binds at slot h, before its dispatch.
// Its input picture: the game's scene upscaler samples the post pass's picture through an RGB9E5 texture that lies at
// the scene target's address. On the console the two are one memory, so the words the post pass stores through the
// target's integer view are that texture's texels. Here they are two images (raw_9e5_twin), and the RGB9E5 one holds
// what it was given when it was made: an older picture, or the target's R11G11B10F words read as shared-exponent
// texels, which is noise up to 65408 that the half-float network turns into infinities. The stand-in reads the
// target's image for that reason (untranslated_dispatch_fallback); the network's own code samples the texture, so the
// words are copied into it here (the same texel size: a copy keeps the bits), each written picture once.
// Its output: an image made without storage use cannot be written, and the dispatch then writes a dummy; logged, as
// the picture is then lost without another sign. Texture lock held.
static void net_binding(VkExec *ex, uint64_t h, int storage) {
    const VkTexImage *t = vk_texture_by_id_locked((int)(h & 0xfffff), NULL, NULL);
    if (!t) return;
    if (storage) {
        static int told;
        if (!(t->usage & VK_IMAGE_USAGE_STORAGE_BIT) && told++ < 4)
            rt_log("vk_exec: a network program's output image %ux%u fmt %d at %#llx has no storage use: nothing is written to it",
                   t->width, t->height, (int)t->format, (unsigned long long)t->addr);
        return;
    }
    if (t->format != VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 || t->type != VK_IMAGE_TYPE_2D || t->samples != 1) return;
    const VkTexImage *o = raw_9e5_twin(t);
    if (!o) {
        static int told;
        if (!told++)
            rt_log("vk_exec: a network program samples the RGB9E5 picture %ux%u at %#llx, which nothing wrote this frame or the last",
                   t->width, t->height, (unsigned long long)t->addr);
        return;
    }
    if (o == t) return;   // written as native texels, later than the target was
    static struct { VkImage dst; uint64_t seq; } done[4];
    static unsigned next;
    uint64_t seq = raw_written(o->image);
    for (int i = 0; i < 4; i++)
        if (done[i].dst == t->image && done[i].seq == seq) return;
    done[next++ % 4] = (typeof(done[0])){t->image, seq};
    // The target's earlier writes and the texture's earlier readers before the copy, the copy before the dispatch.
    VkMemoryBarrier2 m0 = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkDependencyInfo d0 = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m0};
    vkCmdPipelineBarrier2(ex->cb, &d0);
    VkImageCopy cp = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .extent = {t->width, t->height, 1}};
    vkCmdCopyImage(ex->cb, o->image, VK_IMAGE_LAYOUT_GENERAL, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    VkMemoryBarrier2 m1 = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d1 = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m1};
    vkCmdPipelineBarrier2(ex->cb, &d1);
    ex->recorded += 3;
    static int told;
    if (!told++)
        rt_log("vk_exec: network input: the post pass's RGB9E5 words (%ux%u at %#llx) are copied into the texture the network samples",
               t->width, t->height, (unsigned long long)t->addr);
}

// The image descriptor of slot h; in a dispatch, a compare-sampled colour image the driver cannot compare is swapped
// for its depth twin; in a pass larger than it, the scene's light target, once DLSS scaled it this frame, for DLSS's
// output (vk_dlaa.c).
static void tex_info(VkExec *ex, const GfxShaderRes *r, uint64_t h, int storage, int separate, VkDescriptorImageInfo *out,
                     VkImage *img_out) {
    tex_info_lookup(ex, r, h, storage, separate, out, img_out);
    if (h && !storage && vk_dlaa_swap_img && ex->in_pass && ex->pass_w > vk_dlaa_swap_img->width + 1 &&
        vk_texture_by_id_locked((int)(h & 0xfffff), NULL, NULL) == vk_dlaa_swap_img)
        out->imageView = vk_dlaa_swap_view, out->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    if (twin_dispatch == 2 && h) net_binding(ex, h, storage);
    if (!twin_dispatch || storage || separate || !h || !(r->type & 0x400)) return;
    VkImageViewType vt = res_view_type(r->type);
    if (vt != VK_IMAGE_VIEW_TYPE_2D && vt != VK_IMAGE_VIEW_TYPE_2D_ARRAY) return;
    const VkTexImage *img = vk_texture_by_id_locked((int)(h & 0xfffff), NULL, NULL);
    Twin *t = img ? twin_update(ex, img) : NULL;
    VkImageView v = t ? t->view[vt == VK_IMAGE_VIEW_TYPE_2D_ARRAY] : VK_NULL_HANDLE;
    if (v) out->imageView = v, out->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
}

static VkDescriptorSet alloc_set(VkExec *ex, VkDescriptorSetLayout layout) {
    CbRes *r = cb_res(ex);
    VkCtx *c = vk_ctx();
    for (int attempt = 0; attempt < 2; attempt++) {
        if (r->npools) {
            VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                              .descriptorPool = r->pools[r->npools - 1], .descriptorSetCount = 1,
                                              .pSetLayouts = &layout};
            VkDescriptorSet set;
            VkResult res = vkAllocateDescriptorSets(c->device, &ai, &set);
            if (res == VK_SUCCESS) return set;
            if (res != VK_ERROR_OUT_OF_POOL_MEMORY && res != VK_ERROR_FRAGMENTED_POOL) rt_fatal("vk: descriptor set allocation failed (%d)", (int)res);
        }
        static const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 256},
            {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 256}};
        VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = REPLAY_POOL_SETS,
                                         .poolSizeCount = sizeof sizes / sizeof sizes[0], .pPoolSizes = sizes};
        if (r->npools == r->cpools) r->pools = grow(r->pools, &r->cpools, sizeof *r->pools);
        VkDescriptorPool pool = VK_NULL_HANDLE;
        plat_mutex_lock(&spare_lock);
        if (nspare_pool) pool = spare_pool[--nspare_pool];
        plat_mutex_unlock(&spare_lock);
        if (!pool) {
            VkResult res = vkCreateDescriptorPool(c->device, &pi, NULL, &pool);
            if (res == VK_ERROR_OUT_OF_DEVICE_MEMORY || res == VK_ERROR_OUT_OF_HOST_MEMORY) {   // once more, as scratch
                uint64_t freed = vk_mem_give_back();
                res = vkCreateDescriptorPool(c->device, &pi, NULL, &pool);
                rt_log("vk: descriptor pool refused: %.1f MiB given back, %s", freed / 1048576.0,
                       res == VK_SUCCESS ? "made at the second try" : "refused again");
            }
            if (res != VK_SUCCESS) rt_fatal("vk: descriptor pool failed (%d)", (int)res);
            __atomic_add_fetch(&npools_live, 1, __ATOMIC_RELAXED);
        }
        r->pools[r->npools++] = pool;
    }
    rt_fatal("vk: descriptor set allocation failed twice");
    return VK_NULL_HANDLE;
}

static void support_in(const VkExec *ex, float in[3]) {   // what support_fill reads: viewport size, point size
    float k = ex->pass_scale > 1 ? (float)ex->pass_scale : 1.0f;
    in[0] = ex->in_pass && ex->has_vp ? ex->vp[0][2] * k : (float)ex->pass_w;
    in[1] = ex->in_pass && ex->has_vp ? ex->vp[0][3] * k : (float)ex->pass_h;
    in[2] = ex->point_size > 0 ? ex->point_size : 1.0f;
}

// NX2NAVI_PCSS (percent, 0 = off): the pcss field of the support buffer, which translator patch 0010 reads in every
// explicit-LOD compare on a 2D array texture (the sun cascades). x: penumbra width in UV per unit of light-space depth
// between receiver and blocker (0 keeps the game's single compare), y and z: the narrowest and widest disc radius,
// w: the blocker search radius, all in UV of the cascade map. Radii are in UV, so they hold at any NX2NAVI_SHADOW_SCALE.
// NX2NAVI_PCSS_K overrides the depth-to-penumbra factor at 100 percent, for tuning.
static const float *pcss_params(void) {
    static float p[4] = {-1, 0, 0, 0};
    if (p[0] < 0) {
        int pct = cfg_int("NX2NAVI_PCSS");
        pct = pct < 0 ? 0 : pct > 200 ? 200 : pct;
        const char *k = cfg_get("NX2NAVI_PCSS_K");
        float k100 = k && *k ? (float)atof(k) : 0.5f;
        p[0] = k100 * (float)pct / 100.0f;
        p[1] = 0.35f / 960.0f, p[2] = 5.0f / 960.0f, p[3] = 4.0f / 960.0f;
        if (pct) rt_log("vk: soft shadows (PCSS) at %d%% (depth factor %g)", pct, p[0]);
    }
    return p;
}

// The shadow_spread field of the support buffer: the factor translator patch 0011 applies to the lighting shaders' sun
// shadow filter offset (element 56 .xy of their lighting block), 1 / NX2NAVI_SHADOW_SCALE. The game spaces its four
// compares for its own shadow map texels; with maps NX2NAVI_SHADOW_SCALE times larger, the same offset in UV spans that
// many more texels and the near cascade shows four separate copies of each edge. Dividing by the scale keeps the game's
// spacing in texels. The offset is in UV of the shadow map, so the output resolution plays no part. The scale is read
// once, as vk_texture.c shadow_scale reads it: the maps keep the size they were made with.
static const float *shadow_spread_params(void) {
    static float p[4] = {0, 0, 0, 0};
    if (p[0] == 0) {
        int scale = cfg_int("NX2NAVI_SHADOW_SCALE");
        scale = scale < 1 ? 1 : scale > 4 ? 4 : scale;
        p[0] = 1.0f / (float)scale;
    }
    return p;
}

static void support_fill(uint8_t *b, float w, float h, float point_size) {
    memset(b, 0, SUPPORT_BYTES);
    float inv[4] = {w > 0 ? 2.0f / w : 0, h > 0 ? 2.0f / h : 0, 0, 0}, size[4] = {w, h, 0, 0};
    memcpy(b + 144, inv, 16);
    memcpy(b + 160, size, 16);
    const float one = 1.0f;
    memcpy(b + 1392, &point_size, 4);   // point_size (translator patch 0008): vertex shaders that write none output it
    memcpy(b + 1408, pcss_params(), 16);   // pcss (translator patch 0010): soft sun shadows in the cascade compares
    memcpy(b + 1424, shadow_spread_params(), 16);    // shadow_spread (translator patch 0011): sun shadow filter width
    for (int i = 0; i < 73; i++) memcpy(b + 192 + 16 * i, &one, 4);   // render scale 1: the host renders at guest size
    if (vk_hdr_armed()) {   // read by the game's tonemap only, once vk_hdr.c tonemap_splice has grown its block past the translator's
        float hdr[4];
        vk_hdr_support(hdr);
        memcpy(b + 1440, hdr, sizeof hdr);
    }
}

// NX2NAVI_BIND_STATS=1: bindings and descriptors per draw of each stage (Task 2.2's choice of the pushed stage), and how
// often a draw repeats the previous one, logged every 20000 draws: pushes recorded and skipped, allocated sets written
// and reused, stage texture-type recomputations (answers_for), draws with the previous draw's program, bound state
// and textures (shaders and pipeline would be the same) and those that also have its buffers and viewport (all
// descriptors would be the same). BS is declared beside DS.
static int bind_stats_on(void) {
    if (BS.on < 0) {
        const char *e = cfg_get("NX2NAVI_BIND_STATS");
        BS.on = e && *e && strcmp(e, "0");
    }
    return BS.on;
}

static void bind_stats(const VkPipeInfo *info) {
    if (!bind_stats_on() || info->bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS) return;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *b = &info->bindings[i];
        int fs = info->n_sets > 1 ? b->set : !(b->stages & VK_SHADER_STAGE_VERTEX_BIT);
        BS.bind[fs]++, BS.desc[fs] += b->count ? b->count : 1;
    }
    if (++BS.draws % 20000 == 0) {
        double d = (double)BS.draws;
        rt_log("vk_exec: per draw over %llu draws: vertex %.2f bindings %.2f descriptors, fragment %.2f bindings %.2f descriptors",
               (unsigned long long)BS.draws, BS.bind[0] / d, BS.desc[0] / d, BS.bind[1] / d, BS.desc[1] / d);
        rt_log("vk_exec: per draw: pushes %.3f skipped %.3f, sets written %.3f reused %.3f, texture types %.3f; "
               "same program and state as the draw before %.3f, also same buffers %.3f; descriptor builds skipped %.3f",
               BS.push / d, BS.push_skip / d, BS.set_new / d, BS.set_reuse / d, BS.ac_miss / d, BS.same_prog / d,
               BS.same_all / d, BS.bind_skip / d);
    }
}

// The bound state a draw reads, compared with the previous draw's (NX2NAVI_BIND_STATS only).
static void repeat_stats(const VkExec *ex, const VkPipeInfo *info) {
    static struct {
        const prog_t *prog;
        VkPipeState pipe;
        uint64_t tex[2][128], img[2][32];
        const VkPipeInfo *info;
        p_bind_buf ubo[2][18], ssbo[2][16];
        float vp[6], point_size;
    } last, cur;
    if (!bind_stats_on()) return;
    memset(&cur, 0, sizeof cur);
    cur.prog = ex->prog, cur.pipe = ex->pipe, cur.info = info;
    const int st[2] = {NVN_STAGE_VS, NVN_STAGE_FS};
    for (int k = 0; k < 2; k++) {
        memcpy(cur.tex[k], ex->tex[st[k]], sizeof cur.tex[k]), memcpy(cur.img[k], ex->img[st[k]], sizeof cur.img[k]);
        memcpy(cur.ubo[k], ex->ubo[st[k]], sizeof cur.ubo[k]), memcpy(cur.ssbo[k], ex->ssbo[st[k]], sizeof cur.ssbo[k]);
    }
    memcpy(cur.vp, ex->vp[0], sizeof cur.vp), cur.point_size = ex->point_size;
    size_t prog_part = offsetof(typeof(cur), info);
    if (!memcmp(&cur, &last, prog_part)) {
        BS.same_prog++;
        if (!memcmp((const char *)&cur + prog_part, (const char *)&last + prog_part, sizeof cur - prog_part)) BS.same_all++;
    }
    last = cur;
}

static int push_skip_on(void) {   // NX2NAVI_PUSH_SKIP=0: every graphics draw pushes its set again
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2NAVI_PUSH_SKIP");
    }
    return on;
}

// A graphics draw whose descriptor inputs equal the previous graphics draw's in this command buffer skips building its
// sets: every write would be the same, the push would be skipped and the allocated set reused without a bind (about a
// quarter of field and monsters2 draws, NX2NAVI_BIND_STATS). The inputs, compared and stored in place, are what
// bind_resources and its helpers read beyond the pipeline info's fixed bindings and Z's stand-ins:
// - the info (bindings, layouts), the program and its serial (the compiler-constant range of UBO slot 1);
// - vk_texture_slots_gen, vk_texture_epoch (a slot's image, view and once-per-submit storage check), vk_sampler_gen;
// - vk_mem_gen (buffer and offset of a span, mirror creation and per-submission sync);
// - the images in the feedback layout (the sampled layout);
// - per binding: the support inputs, c0's storage table and base instance, the bound uniform and storage ranges, the
//   texture and image handles (RsList; compared only when a bind op ran since the last draw, bind_gen);
// - storage buffer addresses read from user uniform blocks (RsList.p, ubo_ptr): compared on every draw, since the
//   block's contents change without a bind op.
// The side effects of a build repeat as no-ops in the same command buffer and submission: c0 and support buffers kept
// in B, note_written's ranges, vk_mem_gpu_wrote's write-backs, the per-submission mirror sync and dirty census note,
// the views cached per submission. The census counts are added again. A build that copied guest bytes into transient
// memory (an unaligned host range) is never skipped after: the copy may differ. B.res_valid says the sets bound hold
// the last build; vk_exec_draw_new_cb's memset clears it, and every build clears it until it ends.
// NX2NAVI_BIND_SKIP=0 builds every draw; =check builds every draw and stops when a draw with equal inputs gets other
// writes, census counts or descriptor commands.
static int bind_skip_on(void) {
    if (rs_on < 0) {
        const char *e = cfg_value("NX2NAVI_BIND_SKIP");
        rs_on = !strcmp(e, "check") ? 2 : strcmp(e, "0") != 0;
        if (rs_on != 1) rt_log("vk_exec: descriptor build skip %s", rs_on ? "check" : "off");
    }
    return rs_on;
}

static void rs_add(RsRange *r, uint32_t *n, const VkExec *ex, const void *at, size_t len) {
    uint32_t off = (uint32_t)((const uint8_t *)at - (const uint8_t *)ex);
    if (*n && r[*n - 1].off + r[*n - 1].len == off) r[*n - 1].len += (uint32_t)len;
    else r[(*n)++] = (RsRange){off, (uint32_t)len};
}

static const RsList *rs_list(const VkExec *ex, const VkPipeInfo *info) {
    DrawCtx *const cx = ex->dc;
    if (info == cx->rs_last) return cx->rs_last_l;
    int lk = vk_exec_shared();
    if (lk) plat_mutex_lock(&rs_lock);
    if (!rs_lists.klen) nvn_hm_init(&rs_lists, sizeof info);
    RsList *l = nvn_hm_get(&rs_lists, &info);
    if (!l) {
        static RsRange tmp[MAX_DESC];
        RsPtr ptr[RS_PTRS];
        uint32_t n = 0, nd = 0, np = 0;
        int sup = 0, c0 = 0, ok = 1;
        for (uint32_t i = 0; i < info->n_bindings && ok; i++) {
            const VkPipeBinding *b = &info->bindings[i];
            const GfxShaderRes *r = b->res;
            uint32_t cnt = b->count ? b->count : 1;
            if ((nd += cnt) > MAX_DESC) { ok = 0; break; }   // the build stops on it
            int st = nvn_stage_of(b->stage);
            if (b->kind == VK_PIPE_SUPPORT) {
                sup = 1;
            } else if (b->kind == GFX_RES_UBO) {
                for (uint32_t k = 0; k < cnt; k++) {
                    int slot = r->slot + (int)k;
                    if (slot == 0 && c0_on()) c0 = 1, rs_add(tmp, &n, ex, ex->ssbo[st], sizeof ex->ssbo[st]);
                    else if (slot >= 3 && slot - 3 < 18) rs_add(tmp, &n, ex, &ex->ubo[st][slot - 3], sizeof(p_bind_buf));
                }   // slot 1: the program (bind_sig); others: the zero buffer
            } else if (b->kind == GFX_RES_SSBO) {   // every descriptor of the binding is the same range
                int ubo, slot = ssbo_slot(r, st, &ubo), direct = slot >= 0 && slot < 16;
                if (direct) rs_add(tmp, &n, ex, ubo ? &ex->ubo[st][slot] : &ex->ssbo[st][slot], sizeof(p_bind_buf));
                else if (ubo_ptr_res(r)) rs_add(tmp, &n, ex, &ex->ubo[st][r->cbuf_slot - 3], sizeof(p_bind_buf));
                if (r->ptr || (!direct && ubo_ptr_res(r))) {   // an address read from memory: bind_sig reads it again
                    if (np == RS_PTRS) { ok = 0; break; }
                    ptr[np++] = (RsPtr){st, r};
                }
            } else if ((b->kind == GFX_RES_TEX || b->kind == GFX_RES_IMG) && b->type != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER &&
                       b->type != VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                int img = b->kind == GFX_RES_IMG, cap = img ? 32 : 128;
                for (uint32_t k = 0; k < cnt; k++) {
                    int slot = r->slot + (int)k;
                    if (slot >= 0 && slot < cap) rs_add(tmp, &n, ex, img ? &ex->img[st][slot] : &ex->tex[st][slot], 8);
                }
            }
        }
        uint64_t bytes = 0;
        for (uint32_t i = 0; i < n; i++) bytes += tmp[i].len;
        if (bytes > RS_BYTES) ok = 0;
        l = malloc(sizeof *l + sizeof *tmp * n);
        if (!l) rt_fatal("vk: out of memory");
        l->n = n, l->bytes = (uint32_t)bytes, l->sup = (uint8_t)sup, l->c0 = (uint8_t)c0, l->ok = (uint8_t)ok;
        l->np = (uint8_t)np;
        memcpy(l->p, ptr, sizeof *ptr * np);
        memcpy(l->r, tmp, sizeof *tmp * n);
        nvn_hm_put(&rs_lists, &info, l);
    }
    if (lk) plat_mutex_unlock(&rs_lock);
    cx->rs_last = info, cx->rs_last_l = l;
    return l;
}

// The signature of a graphics draw's descriptor inputs into RS; 1 when it equals the stored one. The bound-slot bytes
// are compared only when a bind op ran since they were stored or the pipeline changed: otherwise they are the same.
// Check mode compares them anyway and stops when the bind op generation said they could not differ but they do.
static int bind_sig(const VkExec *ex, const VkPipeInfo *info) {
    DrawCtx *const cx = ex->dc;
    const RsList *l = rs_list(ex, info);
    if (!l->ok) return cx->RS.hn = 0, cx->RS.info = NULL, 0;
    uint64_t *h = cx->RS.h, diff = 0;
    uint32_t n = 0;
#define PUT(v) do { uint64_t v_ = (uint64_t)(v); diff |= h[n] ^ v_; h[n++] = v_; } while (0)
    PUT((uintptr_t)info);
    PUT((uintptr_t)ex->prog);
    PUT(ex->prog->serial);
    PUT((uint64_t)vk_texture_slots_gen() << 32 | vk_texture_epoch());
    PUT((uint64_t)vk_sampler_gen() << 32 | (uint32_t)ex->nfbl);
    PUT(vk_mem_gen() + vk_usnap_gen());   // both only grow
    for (int i = 0; i < ex->nfbl && i < 9; i++) PUT((uintptr_t)ex->fbl_img[i]);
    if (l->sup) {
        float in[3];
        support_in(ex, in);
        uint64_t a;
        uint32_t ps;
        memcpy(&a, in, 8), memcpy(&ps, &in[2], 4);
        PUT(a);
        PUT(ps);
    }
    if (l->c0) PUT(cx->c0_binst);
    for (int i = 0; i < l->np; i++)
        PUT(l->p[i].res->ptr ? ptr_addr(ex, l->p[i].st, l->p[i].res) : ubo_ptr(ex, l->p[i].st, l->p[i].res));
#undef PUT
    diff |= n != cx->RS.hn;
    cx->RS.hn = n;
    int fast = cx->RS.info == info && cx->RS.gen == cx->bind_gen;
    if (fast && rs_on != 2) return !diff;
    const uint8_t *e = (const uint8_t *)ex;
    uint8_t *d = cx->RS.body;
    int bd = 0;
    for (uint32_t i = 0; i < l->n; i++) {
        const RsRange *r = &l->r[i];
        if (!bd && memcmp(d, e + r->off, r->len)) bd = 1;
        if (bd) memcpy(d, e + r->off, r->len);
        d += r->len;
    }
    if (fast && bd) rt_fatal("vk_exec: descriptor build skip check: bound slots changed without a bind op");
    cx->RS.info = info, cx->RS.gen = cx->bind_gen;
    return !diff && !bd;
}

// The binding profile in the NX2NAVI_TRACE=gfx frame line reads the clock around every descriptor; without the trace
// those reads would cost more than some of what they time, so they read 0.
static inline uint64_t bt_now(void) { return rt_trace & RT_TRACE_GFX ? plat_now_ns() : 0; }

// ---- per-binding descriptor reuse
// A graphics draw whose inputs differ from the previous draw's in one binding (the whole-draw skip, bind_sig, misses it)
// used to resolve every one of its descriptors again: each uniform and storage range through vk_mem_resolve and its
// mirror, each texture through the descriptor memo and the slot table, hundreds of thousands a frame in Kokiri Forest.
// PB keeps, per pipeline info (the PB_N most recent, direct-mapped), each descriptor's last result together with the
// bound-slot values it came from, and a descriptor whose values are unchanged takes that result instead of resolving.
// Results stay valid while nothing else a resolution reads changes (pb_globals): the command buffer, the program, the
// texture slot, epoch and sampler generations, the memory generation, the feedback-layout images and DLSS's stand-in
// image. vk_mem_gen moves with every submission, so a result never outlives the submission whose mirror sync and
// write-back notes it relied on; within one, repeating vk_mem_use for the same pool is a no-op, which is why it can be
// left out. Only descriptors that read nothing but their bound slot are kept (pb_key): uniform slots 1 and 3-20,
// storage buffers the shader only reads that a stage slot names, and sampled textures; written storage, c0, pointer
// tables, storage images and the support buffer resolve every time. A reuse counts its census bytes again, and a
// resolution that copied guest bytes into transient memory is never kept. Off while per-object motion replaces uniform
// bindings (mo_ovr_on). NX2NAVI_BIND_REUSE=0 resolves every descriptor; =check resolves every descriptor and stops on
// one that resolves differently from its kept result with the same inputs.
// The entries (PbEntry, DrawCtx.PB) and the build running (DrawCtx.pb_e) belong to the recording context.
static int pb_mode = -1;

static int pb_on(void) {
    if (pb_mode < 0) {
        const char *e = cfg_value("NX2NAVI_BIND_REUSE");
        pb_mode = !strcmp(e, "check") ? 2 : strcmp(e, "0") != 0;
        if (pb_mode != 1) rt_log("vk_exec: per-binding descriptor reuse %s", pb_mode ? "check" : "off");
    }
    return pb_mode;
}

// Everything besides its own bound slot that a kept descriptor's resolution read.
static void pb_globals(const VkExec *ex, uint64_t g[PB_G]) {
    int swap = vk_dlaa_swap_img && ex->in_pass && ex->pass_w > vk_dlaa_swap_img->width + 1;
    memset(g, 0, sizeof(uint64_t) * PB_G);
    g[0] = (uintptr_t)ex->cb;
    g[1] = (uintptr_t)ex->prog;
    g[2] = ex->prog ? ex->prog->serial : 0;
    g[3] = (uint64_t)vk_texture_slots_gen() << 32 | vk_texture_epoch();
    g[4] = (uint64_t)vk_sampler_gen() << 32 | (uint32_t)ex->nfbl;
    g[5] = vk_mem_gen() + vk_usnap_gen();
    g[6] = swap ? (uintptr_t)vk_dlaa_swap_img : 0;
    g[7] = swap ? (uint64_t)(uintptr_t)vk_dlaa_swap_view : 0;
    for (int i = 0; i < ex->nfbl && i < 9; i++) g[8 + i] = (uint64_t)(uintptr_t)ex->fbl_img[i];
}

// info's entry, emptied when it held another pipeline's results or the globals moved.
static PbEntry *pb_entry(DrawCtx *cx, const VkPipeInfo *info, const uint64_t g[PB_G]) {
    uintptr_t p = (uintptr_t)info;
    PbEntry *e = &cx->PB[(p >> 6 ^ p >> 13) % PB_N];
    if (e->info != info || memcmp(e->g, g, sizeof e->g)) {
        e->info = info;
        memcpy(e->g, g, sizeof e->g);
        memset(e->ok, 0, sizeof e->ok);
    }
    return e;
}

// The bound-slot values descriptor k of binding b resolves from, when they are all it reads besides pb_globals.
static int pb_key(const VkExec *ex, const VkPipeBinding *b, uint32_t k, uint64_t key[4]) {
    const GfxShaderRes *r = b->res;
    int st = nvn_stage_of(b->stage);
    key[0] = key[1] = 0, key[2] = (uintptr_t)r, key[3] = (uint64_t)(uint32_t)b->kind << 32 | k;
    if (b->kind == GFX_RES_UBO) {
        int slot = r->slot + (int)k;
        if (slot == 0 && c0_on()) return 0;
        if (slot >= 3 && slot - 3 < 18) key[0] = ex->ubo[st][slot - 3].addr, key[1] = ex->ubo[st][slot - 3].size;
        return 1;   // slot 1: the program's constants (pb_globals); others: the zero buffer
    }
    if (b->kind == GFX_RES_SSBO) {
        if ((r->flags & 1) || r->ptr || r->cbuf_slot >= VK_GPA_CB_SLOT) return 0;   // the table, the mesh fallback's buffers
        int ubo, slot = ssbo_slot(r, st, &ubo);
        if (slot < 0 || slot >= 16) return 0;
        const p_bind_buf *p = ubo ? &ex->ubo[st][slot] : &ex->ssbo[st][slot];
        key[0] = p->addr, key[1] = p->size;
        return 1;
    }
    if (b->kind == GFX_RES_TEX && b->type != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER &&
        b->type != VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
        int slot = r->slot + (int)k;
        key[0] = slot >= 0 && slot < 128 ? ex->tex[st][slot] : 0;
        key[1] = (uint64_t)r->separate;
        return 1;
    }
    return 0;
}

// Before descriptor j (descriptor k of binding b) resolves: 1 when its kept result serves, *o filled and its census
// counted again. 0: resolve it, then pb_keep.
static int pb_take(const VkExec *ex, const VkPipeBinding *b, uint32_t j, uint32_t k, VkDescSlot *o) {
    DrawCtx *const cx = ex->dc;
    cx->pb_key_ok = 0;
    if (!cx->pb_e || j >= PB_DESC || !pb_key(ex, b, k, cx->pb_key_cur)) return 0;
    cx->pb_key_ok = 1;
    if (pb_mode != 2 && cx->pb_e->ok[j] && !memcmp(cx->pb_e->in[j], cx->pb_key_cur, sizeof cx->pb_key_cur)) {
        const PbCount *c = &cx->pb_e->cnt[j];
        *o = cx->pb_e->d[j];
        if (c->has) count_use(cx, c->cls, c->use, c->n);
        vk_exec_frame.bind_reused++;
        return 1;
    }
    cx->pb_cnt_cur = (PbCount){0};
    cx->pb_cap = &cx->pb_cnt_cur, cx->pb_spoiled = 0;
    return 0;
}

// After descriptor j resolved to *o.
static void pb_keep(DrawCtx *cx, uint32_t j, const VkDescSlot *o) {
    cx->pb_cap = NULL;
    if (!cx->pb_e || j >= PB_DESC) return;
    vk_exec_frame.bind_resolved++;
    if (!cx->pb_key_ok) {
        cx->pb_e->ok[j] = 0;
        return;
    }
    if (pb_mode == 2 && cx->pb_e->ok[j] && !cx->pb_spoiled &&
        !memcmp(cx->pb_e->in[j], cx->pb_key_cur, sizeof cx->pb_key_cur) &&
        memcmp(&cx->pb_e->d[j], o, sizeof *o))
        rt_fatal("vk_exec: per-binding descriptor reuse check: descriptor %u resolves differently from its kept result "
                 "with the same inputs", j);
    cx->pb_e->ok[j] = (uint8_t)!cx->pb_spoiled;
    memcpy(cx->pb_e->in[j], cx->pb_key_cur, sizeof cx->pb_key_cur);
    cx->pb_e->d[j] = *o;
    cx->pb_e->cnt[j] = cx->pb_cnt_cur;
}

// Writes allocated set s from its packed descriptors d: through the layout's update template, or with the write
// structs w[i0..i1) when it has none.
static void write_set(const VkPipeInfo *info, uint32_t s, VkDescriptorSet set, const VkDescSlot *d, VkWriteDescriptorSet *w,
                      uint32_t i0, uint32_t i1) {
    if (i1 == i0) return;
    if (info->tpl[s]) {
        vk_pipeline_update_tpl_fn()(vk_ctx()->device, set, info->tpl[s], d);
        return;
    }
    for (uint32_t k = i0; k < i1; k++) w[k].dstSet = set;
    vkUpdateDescriptorSets(vk_ctx()->device, i1 - i0, w + i0, 0, NULL);
}

// Writes the pipeline's sets from the bound state: set 0, and set 1 with independent layouts. Each is pushed into the
// command buffer, or allocated, written and bound; a graphics set whose contents equal the last one allocated for that
// set number in this command buffer is reused, and bound again only when another layout or a push came between. A
// graphics push equal to the last one at its set number, with no other layout or bind between, is not recorded.
// The descriptors go into one packed array d, a VkDescSlot per descriptor in binding order, which a set's update
// template reads directly (VkPipeInfo.tpl); write structs are filled only when a set has no template.
static _Thread_local const VkDescSlot *emu_d;   // set 0's descriptors of the last bind_resources build (mesh emulation)
static uint32_t emu_d_bytes;
// A pipeline that binds the device address table reads what it names, mostly guest memory itself (pools without a
// mirror), as the GPU runs: the submission's submit wait is the GPU's (vk_mem_take_live). Without this the staged mirror
// uploads (NX2NAVI_MIRROR_STAGE) ended the wait once the replay had submitted, and the game wrote the next frame's
// instances while the GPU still drew from them: the small grass and moss, which reach their instances through the
// table, flickered and slid over the ground as the camera turned (RX 580, Kokiri Forest, 2026-11-11). Noted at every
// bind, before the bind skip and the kept descriptors (bind_sig, pb_take), which leave ssbo_info out: noted only there,
// it was missed once those had warmed up, and the flicker came back after a minute of play.
// the pipeline's binding list holds a table binding
static void gpa_reader(const VkPipeInfo *info) {
    int8_t g = __atomic_load_n(&info->gpa, __ATOMIC_RELAXED);   // the replay workers bind too
    if (!g) {
        g = 1;
        for (uint32_t i = 0; i < info->n_bindings && g == 1; i++) {
            const VkPipeBinding *b = &info->bindings[i];
            if (b->kind == GFX_RES_SSBO && b->res && b->res->cbuf_slot == VK_GPA_CB_SLOT) g = 2;
        }
        __atomic_store_n(&((VkPipeInfo *)info)->gpa, g, __ATOMIC_RELAXED);
    }
    if (g == 2) vk_mem_note_live();
}

static void bind_resources(VkExec *ex, const VkPipeInfo *info) {
    gpa_reader(info);
    DrawCtx *const cx = ex->dc;
    VkWriteDescriptorSet *w = cx->bind_w;
    VkDescSlot *d = cx->bind_d;
    emu_d = d;
    VkBufferView *bv = cx->bind_bv;
    if (info->n_bindings > MAX_BINDINGS) rt_fatal("vk: %u descriptor bindings exceed the replay's %d", info->n_bindings, MAX_BINDINGS);
    bind_stats(info);
    int gfx = info->bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS;
    int skip = gfx ? bind_skip_on() : 0, same = 0, was_valid = cx->B.res_valid;
    if (skip) {
        uint64_t sig_t0 = bt_now();
        same = bind_sig(ex, info);
        vk_exec_frame.bind_sig_ns += (uint64_t)(bt_now() - sig_t0);
        if (same && was_valid && skip == 1) {
            for (int i = 0; i < cx->RS.ncnt; i++) vk_mem_count(cx->RS.cnt[i].cls, cx->RS.cnt[i].use, cx->RS.cnt[i].bytes);
            BS.bind_skip++;
            ex->recorded++;
            return;
        }
        cx->B.res_valid = 0;   // until the build below ends
        if (skip == 2) memcpy(cx->RS.pcnt, cx->RS.cnt, sizeof cx->RS.cnt), cx->RS.pncnt = cx->RS.ncnt;
        cx->RS.rec = 1, cx->RS.ncnt = 0, cx->RS.cnt_over = 0, cx->RS.volatile_ = 0, cx->RS.cmds = 0;
    }
    cx->pb_e = NULL;
    if (gfx && !cx->mo_ovr_on && pb_on()) {
        uint64_t g[PB_G];
        pb_globals(ex, g);
        cx->pb_e = pb_entry(cx, info, g);
    }
    uint32_t n_sets = info->n_sets ? info->n_sets : 1;
    uint32_t lo[2] = {0, 0}, hi[2] = {0, 0}, d0[2] = {0, 0};   // bindings [lo, hi) and first descriptor per set
    int need_w = 0;                                             // a set without a template: fill the write structs
    for (uint32_t s = 0, i0 = 0; s < n_sets; s++) {
        uint32_t i1 = i0;
        while (i1 < info->n_bindings && (n_sets == 1 || info->bindings[i1].set == s)) i1++;
        lo[s] = i0, hi[s] = i1, i0 = i1;
        if (i1 > lo[s] && !info->tpl[s]) need_w = 1;
    }
    uint32_t nd = 0;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *b = &info->bindings[i];
        const GfxShaderRes *r = b->res;
        uint32_t cnt = b->count ? b->count : 1;
        if (nd + cnt > MAX_DESC) rt_fatal("vk: %u descriptors exceed the replay's %d", nd + cnt, MAX_DESC);
        if (n_sets == 2 && i == lo[1]) d0[1] = nd;
        VkDescSlot *o = &d[nd];
        int st = nvn_stage_of(b->stage), kind = 0;   // kind: 0 buffer, 1 image, 2 texel buffer view
        if (b->kind == VK_PIPE_SUPPORT) {
            float in[3];
            support_in(ex, in);
            if (!cx->B.sup_valid || memcmp(in, cx->B.sup_in, sizeof in)) {   // support_fill reads only these
                uint8_t tmp[SUPPORT_BYTES];
                support_fill(tmp, in[0], in[1], in[2]);
                memcpy(cx->B.sup_in, in, sizeof in);
                // a new buffer only when the contents changed
                if (!cx->B.sup_valid || memcmp(tmp, cx->B.support, SUPPORT_BYTES)) {
                    VkBuffer sb;
                    VkDeviceSize so;
                    memcpy(scratch(ex, SUPPORT_BYTES, &sb, &so), tmp, SUPPORT_BYTES);
                    memcpy(cx->B.support, tmp, SUPPORT_BYTES);
                    cx->B.sup_buf = sb, cx->B.sup_off = so, cx->B.sup_valid = 1;
                }
            }
            o->buffer = (VkDescriptorBufferInfo){cx->B.sup_buf, cx->B.sup_off, SUPPORT_BYTES};
        } else if (b->kind == GFX_RES_UBO) {
            uint64_t bt = bt_now();
            for (uint32_t k = 0; k < cnt; k++) {
                if (pb_take(ex, b, nd + k, k, &o[k])) continue;
                o[k].buffer = ubo_info(ex, st, r->slot + (int)k);
                pb_keep(cx, nd + k, &o[k]);
            }
            vk_exec_frame.bind_buf_ns += (uint64_t)(bt_now() - bt);
        } else if (b->kind == GFX_RES_SSBO) {
            uint64_t bt = bt_now();
            for (uint32_t k = 0; k < cnt; k++) {
                if (pb_take(ex, b, nd + k, k, &o[k])) continue;
                o[k].buffer = ssbo_info(ex, st, r, r->flags & 1);
                pb_keep(cx, nd + k, &o[k]);
            }
            vk_exec_frame.bind_buf_ns += (uint64_t)(bt_now() - bt);
        } else if (b->kind == GFX_RES_TEX || b->kind == GFX_RES_IMG) {
            int img = b->kind == GFX_RES_IMG;
            if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || b->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                kind = 2;
                for (uint32_t k = 0; k < cnt; k++) {   // buffer textures are not replayed yet
                    o[k].w[0] = 0, o[k].w[1] = 0, o[k].w[2] = 0;
                    o[k].view = Z.zview;
                    if (need_w) bv[nd + k] = Z.zview;
                }
            } else {
                kind = 1;
                int locked = 0;   // tex_info looks up with the texture lock held; kept descriptors (pb_take) need none
                // the context's memo (TexMemo) for sampled textures while other threads record (the lock is free
                // otherwise), outside a dispatch's depth twins and DLSS's stand-in image, with the generations read
                // before any lookup below
                int tm = !img && !r->exact && td_mode() == 1 && !vk_dlaa_swap_img && !twin_dispatch && vk_exec_shared();
                uint32_t tg = tm ? vk_texture_slots_gen() : 0, te = tm ? vk_texture_epoch() : 0;
                uint32_t ts = tm ? vk_sampler_gen() : 0;
                for (uint32_t k = 0; k < cnt; k++) {
                    if (pb_take(ex, b, nd + k, k, &o[k])) continue;
                    int slot = r->slot + (int)k;
                    uint64_t h = 0;
                    if (img) h = slot >= 0 && slot < 32 ? ex->img[st][slot] : 0;
                    else h = slot >= 0 && slot < 128 ? ex->tex[st][slot] : 0;
                    uint64_t tt = bt_now();
                    TexMemo *m = tm && h ? &cx->TM[(h * 0x9E3779B97F4A7C15ull ^ (uint64_t)r->type << 9 ^
                                                    (uint64_t)r->separate) >> 55 & (TM_N - 1)] : NULL;
                    if (m && m->valid && m->h == h && m->type == r->type && m->separate == (uint8_t)r->separate &&
                        m->ep == te && m->sg == ts && slot_still(&m->gen, m->tok, h, tg)) {
                        o[k].image = m->out;
                        o[k].image.imageLayout = m->image && ex->nfbl && in_fbl(ex, m->image)
                                                     ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT
                                                     : VK_IMAGE_LAYOUT_GENERAL;
                    } else {
                        if (!locked) vk_texture_lock(), locked = 1;
                        VkImage ti = VK_NULL_HANDLE;
                        uint64_t tok = m ? token_of(h) : 0;   // before the lookup
                        tex_info(ex, r, h, img, r->separate, &o[k].image, &ti);
                        if (m) *m = (TexMemo){.h = h, .tok = tok, .gen = tg, .ep = te, .sg = ts, .type = r->type,
                                              .separate = (uint8_t)r->separate, .valid = 1, .image = ti,
                                              .out = o[k].image};
                    }
                    vk_exec_frame.bind_tex_ns += (uint64_t)(bt_now() - tt);
                    o[k].w[2] = (uint32_t)o[k].image.imageLayout;   // zero the tail padding
                    pb_keep(cx, nd + k, &o[k]);
                }
                if (locked) vk_texture_unlock();
            }
        } else {
            rt_fatal("vk: binding %u has an unknown kind %d", (unsigned)b->binding, (int)b->kind);
        }
        if (need_w) {
            w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                          .dstBinding = b->binding, .descriptorCount = cnt, .descriptorType = b->type};
            if (kind == 0) w[i].pBufferInfo = &o->buffer;   // a VkDescSlot is as large as either info struct
            else if (kind == 1) w[i].pImageInfo = &o->image;
            else w[i].pTexelBufferView = &bv[nd];
        }
        nd += cnt;
    }
    cx->pb_e = NULL;
    uint64_t push_t0 = bt_now();
    // An empty set 1 (a fragment stage without resources: depth and shadow draws) starts past the last binding, which
    // the loop never reaches: d0[1] stayed 0 and set 0 was pushed with 0 bytes, so the driver read stale descriptors
    if (n_sets == 2 && lo[1] == hi[1]) d0[1] = nd;
    emu_d_bytes = (uint32_t)((n_sets > 1 ? d0[1] : nd) * sizeof *d);
    // Under the validation layer, a new graphics layout binds set 1 before set 0 is pushed or bound. The layer (SDK
    // 1.4.363, PipelineLayoutCompatDef::operator==) takes any two INDEPENDENT_SETS layouts as compatible, so
    // vkCmdPushDescriptorSetKHR keeps the push set it made for an earlier layout and drops writes to bindings that
    // layout lacks (false VUID-vkCmdDrawIndexed-None-08114, then an access violation inside the layer). A bind at set 1
    // disturbs set 0 by its canonical id, which drops the layer's push set: the push after it gets a new one.
    int set1_first = 0;
    if (gfx && info->layout != cx->B.ds_layout) {   // another layout: sets bound before may be disturbed
        cx->B.ds_layout = info->layout;
        cx->B.ds_bound[0] = cx->B.ds_bound[1] = VK_NULL_HANDLE;
        cx->B.ds_pushed[0] = cx->B.ds_pushed[1] = 0;
        set1_first = n_sets == 2 && vk_ctx()->validation;
    }
    for (uint32_t k = 0; k < n_sets; k++) {
        uint32_t s = set1_first ? 1 - k : k, i0 = lo[s], i1 = hi[s];
        const VkDescSlot *ds = &d[d0[s]];
        uint32_t dbytes = (uint32_t)(((s + 1 < n_sets ? d0[s + 1] : nd) - d0[s]) * sizeof *d);
        if (s && i1 == i0) {               // no fragment stage: set 1 is empty and unused
            if (set1_first) {              // bound only to drop the layer's push set (comment above)
                VkDescriptorSet set = alloc_set(ex, info->set_layout1);
                vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, 1, 1, &set, 0, NULL);
                cx->B.ds_bound[1] = set, cx->B.ds_pushed[1] = 0;
                cx->RS.cmds++;
            }
            continue;
        }
        if (s ? info->push1 : info->push) {
            uint32_t n = gfx && push_skip_on() ? dbytes : 0;
            if (n && cx->B.ds_pushed[s] && cx->DS.pn[s] == n && !memcmp(cx->DS.psig[s], ds, n)) {
                BS.push_skip++;
                continue;
            }
            if (info->tpl[s]) rec_push_tpl(vk_pipeline_push_tpl_fn(), ex->cb, info->tpl[s], info->layout, s, ds, dbytes);
            else rec_push(vk_pipeline_push_fn(), ex->cb, info->bind_point, info->layout, s, i1 - i0, w + i0);
            BS.push++;
            cx->RS.cmds++;
            if (gfx) {
                cx->B.ds_bound[s] = VK_NULL_HANDLE, cx->B.ds_pushed[s] = n != 0;
                if (n) cx->DS.pn[s] = n, memcpy(cx->DS.psig[s], ds, n);
            }
        } else if (!gfx || s > 1) {
            VkDescriptorSet set = alloc_set(ex, s ? info->set_layout1 : info->set_layout);
            write_set(info, s, set, ds, w, i0, i1);
            vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, s, 1, &set, 0, NULL);
        } else {
            VkDescriptorSetLayout sl = s ? info->set_layout1 : info->set_layout;
            VkDescriptorSet set = cx->B.ds_last[s];
            if (!set || cx->B.ds_last_layout[s] != sl || cx->DS.n[s] != dbytes || memcmp(cx->DS.sig[s], ds, dbytes)) {
                set = alloc_set(ex, sl);
                write_set(info, s, set, ds, w, i0, i1);
                cx->B.ds_last[s] = set, cx->B.ds_last_layout[s] = sl, cx->DS.n[s] = dbytes;
                memcpy(cx->DS.sig[s], ds, dbytes);
                BS.set_new++;
                cx->RS.cmds++;
            } else {
                BS.set_reuse++;
            }
            if (cx->B.ds_bound[s] != set) {
                vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, s, 1, &set, 0, NULL);
                cx->B.ds_bound[s] = set, cx->B.ds_pushed[s] = 0;
                cx->RS.cmds++;
            }
        }
    }
    if (info->pool) {   // set 2: the texture and sampler pools
        pool_sync_draw(ex);
        vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, 2, 1, &PL.set, 0, NULL);
        cx->RS.volatile_ = 1;   // the pools may change under an equal draw: build the next one again
    }
    if (skip) {
        cx->RS.rec = 0;
        // An equal next draw records nothing when every push set holds its push (NX2NAVI_PUSH_SKIP on) and nothing made
        // the build unrepeatable; allocated sets are always bound to the one written last.
        int valid = !cx->RS.volatile_ && !cx->RS.cnt_over && cx->RS.hn;
        for (uint32_t s = 0; s < n_sets; s++)
            if (hi[s] > lo[s] && (s ? info->push1 : info->push) && !cx->B.ds_pushed[s]) valid = 0;
        if (skip == 2) {   // with the pipeline info equal (bind_sig), the packed descriptors determine every write
            uint32_t n = (uint32_t)(nd * sizeof *d);
            int wr = n != cx->RS.ckn || memcmp(cx->RS.ck, d, n);
            int cn = cx->RS.ncnt != cx->RS.pncnt ||
                     memcmp(cx->RS.cnt, cx->RS.pcnt, sizeof cx->RS.cnt[0] * (size_t)cx->RS.ncnt);
            if (same && was_valid && (wr || cn || cx->RS.cmds))
                rt_fatal("vk_exec: descriptor build skip check: a draw of program %llu with the inputs of the previous one "
                         "gets %s", (unsigned long long)ex->prog->serial,
                         cx->RS.cmds ? "descriptor commands" : wr ? "other writes" : "other census counts");
            if (same && was_valid) BS.bind_skip++;   // the draws a skip would have spared, compared instead
            cx->RS.ckn = n;
            memcpy(cx->RS.ck, d, n);
        }
        cx->B.res_valid = valid;
    }
    vk_exec_frame.bind_push_ns += (uint64_t)(bt_now() - push_t0);
    ex->recorded++;
}

// ---- draws

static void bind_pipeline(VkExec *ex, VkPipelineBindPoint bp, VkPipeline p) {
    DrawCtx *const cx = ex->dc;
    int k = bp == VK_PIPELINE_BIND_POINT_COMPUTE;
    if (cx->B.pipe[k] == p) return;
    vkCmdBindPipeline(ex->cb, bp, p);
    ex->recorded++;
    cx->B.pipe[k] = p;
    if (!k) cx->B.st_valid = 0;
}

static VkViewport base_viewport(VkExec *ex) {
    float x = 0, y = 0, w = (float)ex->pass_w, h = (float)ex->pass_h;
    if (ex->has_vp) {
        float k = ex->pass_scale > 1 ? (float)ex->pass_scale : 1.0f;
        x = ex->vp[0][0] * k, y = ex->vp[0][1] * k, w = ex->vp[0][2] * k, h = ex->vp[0][3] * k;
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    VkViewport v = {x, y + h, w, -h, ex->depth_range[0], ex->depth_range[1]};   // the y flip: negative height
    return v;
}

static VkViewport draw_viewport(VkExec *ex) {
    VkViewport v = base_viewport(ex);
    if (ex->mark) {   // NX2NAVI_PASS_TIMES: how much of the pass the draws cover
        if ((uint32_t)v.width > ex->mark->vw) ex->mark->vw = (uint32_t)v.width;
        if ((uint32_t)-v.height > ex->mark->vh) ex->mark->vh = (uint32_t)-v.height;
    }
    vk_dlaa_viewport(ex, &v);   // the scene's draws move by the DLAA jitter
    return v;
}

// The slope-scaled depth bias of a draw. The term is the bias per unit of depth slope across one pixel of the target,
// and a shadow map made k times larger on each side (NX2NAVI_SHADOW_SCALE, vk_texture.c shadow_scale_of, pass_scale)
// has 1/k of the slope across each of its texels: the game's factor alone gives k times less bias than it asked for
// on sloped surfaces, which is what shows as acne (stripes of self-shadow over slanted ground and walls). The
// constant term counts the depth format's smallest steps, which do not change with the size, and keeps its value.
static float slope_bias(const VkExec *ex) {
    return ex->pass_scale > 1 ? ex->poly_offset.factor * (float)ex->pass_scale : ex->poly_offset.factor;
}

static void record_fixed(VkExec *ex) {
    VkCommandBuffer cb = ex->cb;
    if (ex->dirty & VK_EXEC_DIRTY_VIEWPORT) {
        VkViewport v = draw_viewport(ex);
        vkCmdSetViewportWithCount(cb, 1, &v);
        ex->recorded++;
    }
    if (ex->dirty & VK_EXEC_DIRTY_SCISSOR) {
        VkRect2D r = vk_exec_scissor(ex);
        vkCmdSetScissorWithCount(cb, 1, &r);
        ex->recorded++;
    }
    if (ex->dirty & VK_EXEC_DIRTY_DEPTH_BIAS) {
        // NX2NAVI_DEPTH_BIAS_X scales the constant term (diagnostic): NVN's units are tuned for the Switch's
        // fixed-point depth buffer, and this back end draws into a 32-bit float one, where the same constant means a
        // far smaller offset. If decals z-fight here, this multiplier says whether the offset is the reason.
        static float bx = -1.0f;
        if (bx < 0.0f) {
            const char *e = getenv("NX2NAVI_DEPTH_BIAS_X");
            bx = e && *e ? (float)atof(e) : 1.0f;
            if (bx != 1.0f) rt_log("vk: NX2NAVI_DEPTH_BIAS_X: constant depth bias multiplied by %.1f", bx);
        }
        vkCmdSetDepthBias(cb, ex->poly_offset.units * bx, ex->poly_offset.clamp, slope_bias(ex));
        ex->recorded++;
    }
    if (ex->dirty & VK_EXEC_DIRTY_BLEND_CONST) {
        vkCmdSetBlendConstants(cb, ex->blend_color);
        ex->recorded++;
    }
    if (ex->dirty & VK_EXEC_DIRTY_STENCIL) {
        for (int f = 0; f < 2; f++) {
            VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
            vkCmdSetStencilCompareMask(cb, face, ex->st_vmask[f]);
            vkCmdSetStencilWriteMask(cb, face, ex->st_wmask[f]);
            vkCmdSetStencilReference(cb, face, ex->st_ref[f]);
            ex->recorded += 3;
        }
    }
    ex->dirty = 0;
}

static void bind_vertex_buffers(VkExec *ex) {
    CbState *held = &ex->dc->B;
    VkBuffer vb[17];
    VkDeviceSize vo[17], vsz[17], vstr[17];
    int vcls[17] = {0};
    void *vpool[17] = {0};
    int dyn = vk_pipeline_vertex_input_dynamic();
    // A stream whose range is the one its bound buffer was resolved from, with no pool or mirror change since
    // (vk_mem_gen, read before any resolve), resolves to that buffer again: most draws change one stream or none.
    uint64_t gen = vk_mem_gen();
    int same_gen = held->vb_valid && held->vgen == gen;
    // Most draws change no stream at all: with every range as it was bound and no pool or mirror change, the loop
    // below would take each buffer from B again and return at the compare after it.
    if (same_gen && !memcmp(held->vin, ex->vbuf, sizeof held->vin)) {
        int same = 1;
        for (int b = 0; !dyn && b < 16 && same; b++) same = held->vstr[b] == ex->pipe.vstream[b].stride;
        if (same) return;
    }
    for (int b = 0; b < 17; b++) {
        vb[b] = Z.zbuf, vo[b] = 0, vsz[b] = ZERO_BYTES, vstr[b] = b < 16 ? ex->pipe.vstream[b].stride : 0;
        if (b == VK_PIPE_ZERO_BINDING) continue;
        const NVNbufferRange *r = &ex->vbuf[b];
        if (same_gen && held->vin[b].address == r->address && held->vin[b].size == r->size) {
            vb[b] = held->vb[b], vo[b] = held->vo[b], vsz[b] = held->vsz[b];
            vcls[b] = held->vcls[b], vpool[b] = held->vpool[b];
            continue;
        }
        VkMemSpan sp;
        if (!r->address || !vk_mem_resolve(r->address, 1, &sp)) continue;
        VK_MEM_USE(sp);
        uint64_t n = r->size && r->size < sp.avail ? r->size : sp.avail;
        vb[b] = sp.buffer, vo[b] = sp.offset, vsz[b] = n, vcls[b] = sp.cls, vpool[b] = sp.pool;
    }
    memcpy(held->vcls, vcls, sizeof vcls), memcpy(held->vpool, vpool, sizeof vpool);
    memcpy(held->vin, ex->vbuf, sizeof held->vin);
    held->vgen = gen;
    if (held->vb_valid && !memcmp(held->vb, vb, sizeof vb) && !memcmp(held->vo, vo, sizeof vo) &&
        !memcmp(held->vsz, vsz, sizeof vsz) &&
        (dyn || !memcmp(held->vstr, vstr, sizeof vstr)))
        return;
    vkCmdBindVertexBuffers2(ex->cb, 0, 17, vb, vo, vsz, dyn ? NULL : vstr);
    ex->recorded++;
    for (int b = 0; b < 17; b++)
        if (vcls[b]) vk_mem_count(vcls[b], VK_MUSE_VB, vsz[b]);
    if (rt_trace & RT_TRACE_PERF)
        for (int b = 0; b < 17; b++)
            if (vpool[b]) vk_mem_bound_note(vpool[b]);
    memcpy(held->vb, vb, sizeof vb), memcpy(held->vo, vo, sizeof vo), memcpy(held->vsz, vsz, sizeof vsz);
    memcpy(held->vstr, vstr, sizeof vstr);
    held->vb_valid = 1;
}

// NX2NAVI_FOLIAGE_SHADOW_CULL=<layer>[f] (0 to 3, default 1; 3 turns it off): the foliage's mesh draws into the sun's shadow cascades
// from this layer on drop one side of their cards ("f": the front faces, else the back faces). The game draws them
// double-sided, and stacks of alpha-tested cards seen from the sun reached about 1,000 million fragments in one draw of
// the 2.5 million texel far cascade, where half the cards still cover the same texels. Cull mode is dynamic state, so
// this needs no other pipeline.
static void foliage_shadow_cull(const VkExec *ex, VkPipeState *s) {
    static int from = -1, face;
    if (from < 0) {
        const char *e = cfg_get("NX2NAVI_FOLIAGE_SHADOW_CULL");
        from = e && *e ? atoi(e) : 1;
        face = e && strchr(e, 'f') ? NVN_FACE_FRONT : NVN_FACE_BACK;
    }
    if (!ex->color[0].image && ex->depth.image && (int)ex->depth.base_layer >= from && s->poly.cull == NVN_FACE_NONE)
        s->poly.cull = (unsigned)face;
}

// NX2NAVI_PIPE_SKIP: which draws are skipped, rather than waited for, while their shaders or pipeline build.
// strict (0): only draws whose pass targets were all rendered in the previous presented frame; a pass with a target
//   that was not counts as one-shot and waits.
// any (1): also draws in a pass with at least one target rendered in the previous frame (a per-frame depth
//   buffer or g-buffer with a colour target the frame before did not use: half-resolution effect buffers in fights).
//   Passes with none (volumes, LUTs, shadow layers seen for the first time) still wait, and so do passes whose only
//   recent target is the depth buffer while a colour target is rendered for the first time (item icon captures).
//   Full-screen draws into such passes wait from their budget of 50 ms a presented frame. The default: a skipped
//   full-screen draw is a lighting, fog or tone mapping pass that leaves its target as it was, which shows as a black
//   or unlit frame with the interface intact, and with the stage libraries in the driver's cache the wait is a
//   millisecond (vk_pipeline.c lw_start keeps them there). One that is skipped once its budget is spent leaves a frame
//   that is not shown (vk_backend.c frame_hold); where that cannot hold the picture any longer, the draw waits on
//   (FULLSCREEN_LAST_MS).
// fullscreen (2): as any, and full-screen draws into such passes are skipped instead of waiting from their
//   budget. A full-screen pass whose stage libraries are cold in the driver cache took 86 ms on average (up to 194 ms)
//   in a fight on an RX 9070: three frames in a row each spent the 50 ms budget, and some of the draws were skipped
//   after all. Skipping costs the pass for the frames until the pipeline is ready. It was the default until the
//   libraries were kept warm.
// Any other draw whose pipeline is missing waits for its fast link from a small budget per presented frame
// (link_budget) before it is skipped.
static int pipe_skip_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = cfg_value("NX2NAVI_PIPE_SKIP");
        mode = !strcmp(e, "strict") || !strcmp(e, "0") ? 0 : !strcmp(e, "fullscreen") || !strcmp(e, "2") ? 2 : 1;
        static const char *const what[3] = {"only draws into per-frame passes (NX2NAVI_PIPE_SKIP=strict)",
                                            "draws into passes with a target the previous frame rendered; full-screen "
                                            "draws wait (NX2NAVI_PIPE_SKIP=any)",
                                            "those and full-screen draws (NX2NAVI_PIPE_SKIP=fullscreen)"};
        rt_log("vk_exec: draws skipped while their pipeline builds: %s", what[mode]);
    }
    return mode;
}

// NX2NAVI_DRAW_WRITERS_WAIT (default 1; 0 = off): a draw whose shaders write memory waits for its shaders and
// pipeline as a one-shot draw does, whatever its targets. A draw into per-frame targets may be left out while its
// pipeline builds because the game renders those targets again in the next frame (pipe_skip_mode). A storage buffer
// its shaders write, or an image they store into, is no such target: the write is a side effect of this draw that
// later draws and frames read, and with the draw left out that memory keeps what it held. So the draw takes the wait
// of a draw into targets that are not per-frame (prepare_draw_ex: from the frame's wait pool, or ONESHOT_WAIT_MS with
// NX2NAVI_ONESHOT_BUDGET=0), and is skipped only at the end of it. draw_stats_tick's line counts the draws that waited
// by this rule alone.
static int writers_wait_on(void) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2NAVI_DRAW_WRITERS_WAIT");
        if (!on) rt_log("vk_exec: a draw whose shaders write memory is skipped like any other while its pipeline builds "
                        "(NX2NAVI_DRAW_WRITERS_WAIT=0)");
    }
    return on;
}

// 1 when the stage writes a storage buffer or stores into an image (GfxShaderRes.flags, gfx.h).
static int stage_writes(const GfxShader *s) {
    const GfxShaderRes *r;
    if (!s) return 0;
    for (int i = 0, n = gfx_shader_resources(s, GFX_RES_SSBO, &r); i < n; i++)
        if (r[i].flags & 1) return 1;
    for (int i = 0, n = gfx_shader_resources(s, GFX_RES_IMG, &r); i < n; i++)
        if (r[i].flags & 4) return 1;
    return 0;
}

// 1 when a draw with these stages waits because it writes memory (writers_wait_on). A stage that is NULL writes
// nothing: the program has none, or it is still being translated and has no reflection to ask yet.
static int draw_writes(const GfxShader *vs, const GfxShader *ts, const GfxShader *fs) {
    return writers_wait_on() && (stage_writes(vs) || stage_writes(ts) || stage_writes(fs));
}

// NX2NAVI_PIPE_LINK_MS (default 6; 0 = off): how long the draws of one presented frame may wait, in all, for the fast
// link of a pipeline that is missing, LINK_DRAW_MS at most each. A draw into a per-frame pass used to be skipped at
// once and for as long as its pipeline built, also when its stage libraries were in the driver's cache and the link
// took under a millisecond: geometry seen for the first time (ground, a wall, a character) was missing for a frame or
// more and then appeared. With the libraries warm the wait ends within the budget and nothing is skipped; with them
// cold it runs out after a few draws and the rest are skipped as before, so a burst of cold pipelines costs a frame
// this much and no more. Measured on an RX 7900 XT, first walk through a village with no pipeline recorded: 2,349
// draws skipped without the budget, with the libraries warm in both runs.
// NX2NAVI_PIPE_LINK_DRAW_MS (default 3): the wait for one pipeline, counted from its first miss and shared by all its
// draws (prepare_draw_ex), so a pipeline whose libraries the driver has to compile costs its first frame this much and
// the frames after it nothing; it was 3 ms for each draw, in every frame. A frame that has left a draw out already
// waits for no fast link either (pending_draws): it is held back or shown without that draw whatever its other
// pipelines do.
// The 3 ms are just under what a pipeline takes with its two shader libraries in the driver's cache: on a new
// install's first visits (RTX 5090, the libraries prepared, 67 pipelines the install's list did not hold) 44 were
// usable under 4 ms after their miss, 17 after 4 to 8 ms and 6 later, and of 87 draws that waited 66 were skipped at
// the end of the wait, their frame then held back. 8 ms a pipeline and 16 ms a frame were tried for that. A scripted
// walk through a village with no pipeline recorded (610 missed) had the picture stand still 15 times against 50 and
// 15 frames over 25 ms against 17, but in play it felt worse to the player than the 3 ms it replaced, in two sessions
// on the same install: a frame that takes 10 to 14 ms longer showed more than one that is left out. The waits stay
// short; the settings take longer ones.
// NX2NAVI_PIPE_FRAME_WAIT_MS (default 50; 0 to 500): the wait pool of one presented frame. Its full-screen draws and
// its one-shot draws (NX2NAVI_ONESHOT_BUDGET) wait for their shaders and pipelines from it, each for what the ones
// before left, and are skipped once it is spent, so the pipelines of one frame hold the replay thread this long in
// all and no longer (beside the fast links' own small budget, link_budget, and FULLSCREEN_LAST_MS).
static uint64_t *frame_wait(void) {
    static uint64_t frame = ~0ull, left, per_frame = ~0ull;
    if (per_frame == ~0ull) {
        long long ms = cfg_int("NX2NAVI_PIPE_FRAME_WAIT_MS");
        per_frame = (uint64_t)(ms < 0 ? 0 : ms > 500 ? 500 : ms) * 1000000ull;
        rt_log("vk_exec: the full-screen and one-shot draws of a frame wait up to %llu ms in all for their shaders and "
               "pipelines (NX2NAVI_PIPE_FRAME_WAIT_MS), then are skipped while a worker builds them",
               (unsigned long long)(per_frame / 1000000));
    }
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (f != frame) frame = f, left = per_frame;
    return &left;
}

// The milliseconds of pool not used up since t0, when the caller began to draw on it; UINT32_MAX without a pool.
static uint32_t pool_ms(const uint64_t *pool, uint64_t t0) {
    if (!pool) return UINT32_MAX;
    uint64_t spent = kern_now_ns() - t0;
    return spent < *pool ? (uint32_t)((*pool - spent) / 1000000) : 0;
}

static void pool_charge(uint64_t *pool, uint64_t t0) {
    if (!pool) return;
    uint64_t spent = kern_now_ns() - t0;
    *pool = spent < *pool ? *pool - spent : 0;
}

// 1 for a draw whose pipeline is a mesh pipeline without stage libraries, which gets no fast link (MESH_WAIT_MS).
// mesh: the draw binds a mesh pipeline.
static int mesh_whole(int mesh) { return mesh && !vk_pipeline_mesh_libs(); }

// What a one-shot draw with the state s may wait for its pipeline from pool.
static uint32_t oneshot_ms(const uint64_t *pool, int mesh, const VkPipeState *s) {
    uint32_t ms = (uint32_t)(*pool / 1000000);
    if (!mesh || !ms) return ms;
    if (vk_pipeline_building_ms(s) > MESH_WAIT_MS) return 0;   // a compile (MESH_WAIT_MS)
    return ms > MESH_WAIT_MS ? MESH_WAIT_MS : ms;
}

// ---- wait accounting: what the replay thread spent on draws that found their pipeline or a shader missing
// Counted from the miss to the draw's bind or skip, the look-ahead scan included, by the kind of wait the draw was
// allowed; left: the draws skipped at the end of it. draw_stats_tick logs the sums with the skipped draws, so a
// session's log says how much time the pipelines cost on this thread and which rule spent it. writers: the draws into
// per-frame targets that took a one-shot wait because their shaders write memory (draw_writes); they are among the
// one-shot kinds' counts.
enum { WK_LINK, WK_FULL, WK_LAST, WK_ONESHOT, WK_ONESHOT_MESH, WK_SHADER, WK_N };
static const char *const wk_names[WK_N] = {"a fast link", "full-screen", "full-screen with no picture to hold",
                                           "one-shot", "one-shot mesh", "one-shot, for a shader"};
static struct { uint64_t n[WK_N], ns[WK_N], max_ns[WK_N], left[WK_N], frame_ns, frame_max_ns, frame, writers; } WS;

static void wait_note(int kind, uint64_t t0, int left) {
    uint64_t ns = kern_now_ns() - t0, f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    WS.n[kind]++, WS.ns[kind] += ns, WS.left[kind] += left != 0;
    if (ns > WS.max_ns[kind]) WS.max_ns[kind] = ns;
    if (f != WS.frame) WS.frame = f, WS.frame_ns = 0;
    WS.frame_ns += ns;
    if (WS.frame_ns > WS.frame_max_ns) WS.frame_max_ns = WS.frame_ns;
}

static uint32_t link_draw_ms(void) {
    static int ms = -1;
    if (ms < 0) {
        long long v = cfg_int("NX2NAVI_PIPE_LINK_DRAW_MS");
        ms = v < 1 ? 1 : v > 50 ? 50 : (int)v;
    }
    return (uint32_t)ms;
}
#define LINK_DRAW_MS link_draw_ms()
// How long the full-screen draws of a frame wait on for their pipelines, in all, once the frame's wait pool is spent
// and the previous picture cannot be held any longer (prepare_draw_ex, last_wait). Every such pipeline has a fast link
// (vk_pipeline.c gpl_feedback), so the wait is for stage libraries: a few milliseconds from the driver's cache, and
// in the one case a player's session shows the draw had its pipeline 18 ms into this wait. It was 500 ms for each
// draw while a pipeline with a feedback loop waited for its whole optimised build; with the pool before it, a frame
// now waits under 100 ms for pipelines in all.
#define FULLSCREEN_LAST_MS 40u
static uint64_t *last_wait(void) {
    static uint64_t frame = ~0ull, left;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (f != frame) frame = f, left = FULLSCREEN_LAST_MS * 1000000ull;
    return &left;
}
static uint64_t *link_budget(void) {
    static uint64_t frame = ~0ull, left, per_frame = ~0ull;
    if (per_frame == ~0ull) {
        long long ms = cfg_int("NX2NAVI_PIPE_LINK_MS");
        per_frame = (uint64_t)(ms < 0 ? 0 : ms > 100 ? 100 : ms) * 1000000ull;
        rt_log("vk_exec: draws wait up to %llu ms a frame in all for the fast link of a missing pipeline "
               "(NX2NAVI_PIPE_LINK_MS), %u ms at most each (NX2NAVI_PIPE_LINK_DRAW_MS)",
               (unsigned long long)(per_frame / 1000000), LINK_DRAW_MS);
    }
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (f != frame) frame = f, left = per_frame;
    return &left;
}

// The previous draw's shaders, kept while its program, bound pipeline state, the vertex and fragment stages' first 32
// texture and image handles and every slot's resolution (vk_texture_slots_gen, read unlocked: a registration racing
// with it is ordered after this draw, as answers_for's own check would order it) stay the same: answers_for reads
// nothing else, so shader_for would return the same shaders. About 65% of field draws repeat them
// (NX2NAVI_BIND_STATS). NX2NAVI_SHADER_MEMO=0 turns it off; =check looks the shaders up on every hit too, and every slot's
// texture type in answers_for, and stops on a difference.
// tex_serial (bumped by every texture and image bind) equal to the key's means the handles were not rebound since
// they were compared, so the 1 KiB compare is skipped; a rebind to the same handles still matches by the compare.
// The memo and its key (MemoKey) are DrawCtx.SM.
static int sm_on = -1;

static void memo_key(const VkExec *ex, uint32_t gen, MemoKey *k) {
    k->prog = ex->prog, k->serial = ex->prog->serial, k->gen = gen, k->tex_serial = ex->tex_serial;
    memcpy(k->tex[0], ex->tex[NVN_STAGE_VS], sizeof k->tex[0]), memcpy(k->tex[1], ex->tex[NVN_STAGE_FS], sizeof k->tex[1]);
    memcpy(k->img[0], ex->img[NVN_STAGE_VS], sizeof k->img[0]), memcpy(k->img[1], ex->img[NVN_STAGE_FS], sizeof k->img[1]);
    memcpy(&k->pipe, &ex->pipe, sizeof k->pipe);
}

static int memo_match(const VkExec *ex, uint32_t gen, MemoKey *k) {
    if (k->prog != ex->prog || k->serial != ex->prog->serial || k->gen != gen || memcmp(&k->pipe, &ex->pipe, sizeof k->pipe))
        return 0;
    if (k->tex_serial == ex->tex_serial) return 1;
    if (memcmp(k->tex[0], ex->tex[NVN_STAGE_VS], sizeof k->tex[0]) || memcmp(k->tex[1], ex->tex[NVN_STAGE_FS], sizeof k->tex[1]) ||
        memcmp(k->img[0], ex->img[NVN_STAGE_VS], sizeof k->img[0]) || memcmp(k->img[1], ex->img[NVN_STAGE_FS], sizeof k->img[1]))
        return 0;
    k->tex_serial = ex->tex_serial;
    return 1;
}

static int shader_memo_on(void) {
    if (sm_on < 0) {
        const char *e = cfg_value("NX2NAVI_SHADER_MEMO");
        sm_on = !strcmp(e, "check") ? 2 : strcmp(e, "0") != 0;
    }
    return sm_on;
}

static int pm_on = -1;   // the memo is DrawCtx.PM

static int pipe_memo_on(void) {
    if (pm_on < 0) pm_on = cfg_bool("NX2NAVI_PIPE_MEMO");
    return pm_on;
}

// Pass, shaders, pipeline, fixed state and resources for a draw. 0: the draw is skipped.
// ---- draw accounting: why draws were not drawn
// Every draw prepare_draw_ex turns away is counted by reason, vertex and mesh draws apart. The first draw of each kind
// skipped for each reason is logged with its program, and every DRAW_STATS_FRAMES presented frames one line sums up
// the draws issued and skipped since the last line (only when something was skipped). A black screen with draws
// skipped for "shader failed" or "pipeline failed" points at the translator or the pipeline; draws issued and still
// nothing on screen point at the shaders' results (NX2NAVI_DUMP).
const char *const vk_draw_skip_names[SK_COUNT] = {
    "rendering disabled", "empty scissor", "no program bound",
    "no vertex (or mesh) stage in the program", "shader still translating",
    "shader failed to translate (see gfx_shader: translation failed)", "replaced by the upscaler",
    "pipeline still building", "pipeline could not be created", "uses the texture pools (NX2NAVI_POOL=0)",
    "excluded by NX2NAVI_SKIP_PROG", "excluded by NX2NAVI_SKIP_SHADERS"};
#define DRAW_STATS_FRAMES 600
static struct { uint64_t issued[2], skipped[2][SK_COUNT], frame; uint32_t first[2]; } DRS;

static void draw_stats_tick(void) {
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (!DRS.frame) DRS.frame = f;
    if (f - DRS.frame < DRAW_STATS_FRAMES) return;
    uint64_t waits = 0, wait_ns = 0;
    for (int k = 0; k < WK_N; k++) waits += WS.n[k], wait_ns += WS.ns[k];
    static uint64_t steady_logged;
    if (waits || vk_exec_steady_passes != steady_logged) {
        char line[1200];
        int o = snprintf(line, sizeof line, "vk_exec: over %llu frames %llu draws found a shader or pipeline missing and "
                         "held the replay thread %.1f ms in all, %.1f ms in the worst frame; by the wait they were allowed:",
                         (unsigned long long)(f - DRS.frame), (unsigned long long)waits, wait_ns / 1e6, WS.frame_max_ns / 1e6);
        for (int k = 0; k < WK_N && o < (int)sizeof line - 320; k++)   // room for the two counts after the kinds
            if (WS.n[k])
                o += snprintf(line + o, sizeof line - o, " %s %llu (%.1f ms, longest %.1f ms, %llu skipped at its end);",
                              wk_names[k], (unsigned long long)WS.n[k], WS.ns[k] / 1e6, WS.max_ns[k] / 1e6,
                              (unsigned long long)WS.left[k]);
        snprintf(line + o, sizeof line - o, " %llu lookups found only steady targets and skipped a draw that would have "
                 "waited as one-shot; %llu draws into per-frame targets waited as one-shot because their shaders write "
                 "memory (NX2NAVI_DRAW_WRITERS_WAIT)", (unsigned long long)(vk_exec_steady_passes - steady_logged),
                 (unsigned long long)WS.writers);
        rt_log("%s", line);
        steady_logged = vk_exec_steady_passes;
        memset(&WS, 0, sizeof WS);
    }
    for (int m = 0; m < 2; m++) {
        uint64_t any = 0;
        for (int k = 0; k < SK_COUNT; k++) any += DRS.skipped[m][k];
        if (!any) continue;
        char line[512];
        int o = snprintf(line, sizeof line, "vk: %s draws over %llu frames: %llu issued, skipped:", m ? "mesh" : "vertex",
                         (unsigned long long)(f - DRS.frame), (unsigned long long)DRS.issued[m]);
        for (int k = 0; k < SK_COUNT && o < (int)sizeof line - 60; k++)
            if (DRS.skipped[m][k]) o += snprintf(line + o, sizeof line - o, " %s %llu;", vk_draw_skip_names[k], (unsigned long long)DRS.skipped[m][k]);
        rt_log("%s", line);
    }
    memset(&DRS, 0, sizeof DRS.issued + sizeof DRS.skipped);
    DRS.frame = f;
}

// The draws skipped since the last present because their shader or pipeline was still building: the picture of that
// frame lacks them (a light, a wall, a whole lighting pass). vk_backend.c frame_hold reads it at the present and keeps
// the previous picture on screen instead. Written and read on the replay thread.
static uint32_t pending_draws;
uint32_t vk_exec_pending_draws_take(void) {
    uint32_t n = pending_draws;
    pending_draws = 0;
    return n;
}

static int draw_skipped(const VkExec *ex, int mesh, int why) {
    DRS.skipped[mesh][why]++;
    if (vk_dump_armed) vk_dump_draw_skipped(ex, mesh, why);
    if (why == SK_SHADER_PENDING || why == SK_PIPE_PENDING) pending_draws++;
    if (why == SK_PIPE_PENDING) perf_count(PERF_CNT_PIPE_SKIP);   // skipped draws only: a lookup that then waited is none
    if (!(DRS.first[mesh] >> why & 1)) {
        DRS.first[mesh] |= 1u << why;
        rt_log("vk: first %s draw skipped because: %s (program %llu)", mesh ? "mesh" : "vertex", vk_draw_skip_names[why],
               ex->prog ? (unsigned long long)ex->prog->serial : 0ull);
    }
    draw_stats_tick();
    return 0;
}

// NX2NAVI_SKIP_PROG=<serial>[,<serial>...]: diagnostics. Drops every draw whose program serial is listed, so something
// on screen can be attributed to one program by elimination: pass the programs of the pass it appears in, then half of
// them, and so on. Environment only, and nothing is skipped without it. Read once: a getenv on every draw scanned the
// whole environment block and was a sixth of the replay thread's samples in a play profile.
static int skip_prog_list[32], n_skip_prog = -1;
static int skip_prog_wanted(uint64_t serial) {
    if (n_skip_prog < 0) {
        const char *e = getenv("NX2NAVI_SKIP_PROG");
        n_skip_prog = 0;
        while (e && *e && n_skip_prog < 32) {
            char *end;
            long v = strtol(e, &end, 10);
            if (end == e) break;
            skip_prog_list[n_skip_prog++] = (int)v;
            e = *end == ',' ? end + 1 : end;
        }
        if (n_skip_prog) rt_log("vk: NX2NAVI_SKIP_PROG: dropping draws of %d program(s)", n_skip_prog);
    }
    for (int i = 0; i < n_skip_prog; i++)
        if ((uint64_t)skip_prog_list[i] == serial) return 1;
    return 0;
}

// NX2NAVI_PROG_LEAD=1 (diagnostics): how long before its first draw the game made each program
// (nvnProgramSetShaders, nvn_api_obj.c nvn_prog_made_ns): the head start a build begun at that moment would have over
// one begun at the draw, which is where shaders are looked up and pipelines built today. A line for vertex, mesh and
// compute programs (their first dispatch) each, after every 200 programs first used.
#define PROG_LEAD_N 16384
static struct { int on; uint64_t seen[PROG_LEAD_N]; uint64_t n[3][6], logged; } PLD = {-1};

static int prog_lead_on(void) {
    if (PLD.on < 0) PLD.on = cfg_get("NX2NAVI_PROG_LEAD") != NULL && strcmp(cfg_get("NX2NAVI_PROG_LEAD"), "0") != 0;
    return PLD.on;
}

// mesh: 0 a vertex program's draw, 1 a mesh program's, 2 a compute program's dispatch.
static void prog_lead(const prog_t *p, int mesh) {
    uint32_t i = (uint32_t)p->serial & (PROG_LEAD_N - 1);
    if (PLD.seen[i] == p->serial) return;
    PLD.seen[i] = p->serial;
    uint64_t made = nvn_prog_made_ns(p->serial);
    if (!made) return;
    double ms = (double)(plat_now_ns() - made) / 1e6;
    PLD.n[mesh][ms < 17 ? 0 : ms < 100 ? 1 : ms < 1000 ? 2 : ms < 5000 ? 3 : ms < 30000 ? 4 : 5]++;
    uint64_t tot = 0;
    for (int m = 0; m < 3; m++)
        for (int k = 0; k < 6; k++) tot += PLD.n[m][k];
    if (tot - PLD.logged < 200) return;
    PLD.logged = tot;
    static const char *const kind[3] = {"vertex", "mesh", "compute"};
    for (int m = 0; m < 3; m++)
        rt_log("vk: %s programs were made before their first use by: under 17 ms %llu, 17-100 ms %llu, 0.1-1 s %llu, 1-5 s "
               "%llu, 5-30 s %llu, longer %llu (NX2NAVI_PROG_LEAD)", kind[m], (unsigned long long)PLD.n[m][0],
               (unsigned long long)PLD.n[m][1], (unsigned long long)PLD.n[m][2], (unsigned long long)PLD.n[m][3],
               (unsigned long long)PLD.n[m][4], (unsigned long long)PLD.n[m][5]);
}

static const VkPipeInfo *PD_info;   // the pipeline of the last draw prepare_draw_ex prepared
// mesh: a mesh draw (op_draw_mesh): the program's mesh and task stages stand in for the vertex stage, and nothing
// vertex-related (vertex buffers, motion vector bookkeeping of vertex shaders) is bound or noted. 2: a mesh draw of
// the mesh fallback (mfb_draw): the vertex stage is DrawCtx.mfb_cur_vs, in an ordinary pipeline without vertex input.

static int prepare_draw_ex(VkExec *ex, int prim, int32_t count, int restart, int mesh) {
    DrawCtx *const cx = ex->dc;
    int fb = mesh == 2;   // from here on mesh says what the draw is, and !fb that its pipeline is a mesh pipeline
    mesh = mesh != 0;
    if (ex->render_disabled) return vk_cond_draws_skip += ex->has_render_cond, draw_skipped(ex, mesh, SK_DISABLED);
    cb_res(ex);
    dummies_init();
    if (ex->pass_noatt) {   // no attachment: the pass covers this draw's viewport or scissor, reopened when it needs more
        uint32_t w, h;
        vk_exec_noatt_dims(ex, &w, &h);
        if (ex->in_pass && (w > ex->pass_w || h > ex->pass_h)) vk_exec_pass_end(ex);
        if (!ex->in_pass) ex->rt_resolved = 0;
    }
    // a new pass may have another scale (pass_scale): the viewport, the scissor and the slope bias (slope_bias) follow it
    if (!ex->in_pass) ex->dirty |= VK_EXEC_DIRTY_VIEWPORT | VK_EXEC_DIRTY_SCISSOR | VK_EXEC_DIRTY_DEPTH_BIAS;
    vk_exec_pass_begin(ex);
    VkRect2D sc = vk_exec_scissor(ex);
    if (!sc.extent.width || !sc.extent.height) return draw_skipped(ex, mesh, SK_SCISSOR);
    if (!ex->prog) return draw_skipped(ex, mesh, SK_NO_PROGRAM);
    if (prog_lead_on()) prog_lead(ex->prog, mesh);
    if (n_skip_prog && skip_prog_wanted(ex->prog->serial)) return draw_skipped(ex, mesh, SK_PROG_FILTER);   // -1 until read
    int has_vs, has_fs, vs_wait = 0, fs_wait = 0, memo = mesh ? 0 : shader_memo_on();
    int has_ts = 0, ts_wait = 0, vst = mesh ? NVN_STAGE_MESH : NVN_STAGE_VS;
    const GfxShader *vs, *fs, *ts = NULL;
    uint32_t gen = memo ? vk_texture_slots_gen() : 0;   // before shader_for, whose lookups may bump it
    if (memo && cx->SM.valid && memo_match(ex, gen, &cx->SM.k)) {
        vs = cx->SM.vs, fs = cx->SM.fs, has_vs = 1, has_fs = cx->SM.has_fs;
        if (memo == 2) {
            int hv, hf, wv, wf;
            const GfxShader *v2 = shader_for(ex, NVN_STAGE_VS, &hv, &wv, 0), *f2 = shader_for(ex, NVN_STAGE_FS, &hf, &wf, 0);
            if (v2 != vs || f2 != fs || hf != has_fs || wv || wf)
                rt_fatal("vk_exec: shader memo of program %llu differs from the lookup", (unsigned long long)ex->prog->serial);
        }
    } else {
        cx->SM.valid = 0;
        if (fb) vs = cx->mfb_cur_vs, has_vs = 1;
        else vs = shader_for(ex, vst, &has_vs, &vs_wait, 0);
        if (mesh && !fb) ts = shader_for(ex, NVN_STAGE_TASK, &has_ts, &ts_wait, 0);
        fs = shader_for(ex, NVN_STAGE_FS, &has_fs, &fs_wait, 0);
        if (memo && vs && !vs_wait && !fs_wait && (!has_fs || fs))
            memo_key(ex, gen, &cx->SM.k), cx->SM.vs = vs, cx->SM.fs = fs, cx->SM.has_fs = has_fs, cx->SM.valid = 1;
    }
    if (!has_vs) return draw_skipped(ex, mesh, SK_NO_VERTEX_STAGE);
    // A draw into a target the previous frame did not render (or into none) waits for a pending shader or pipeline:
    // one-shot draws (volumes and LUTs generated once, later only sampled) would leave their target empty for the
    // session. A draw into per-frame targets is skipped until they are ready and comes back next frame, unless its
    // shaders write memory: then it waits like a one-shot draw (draw_writes).
    int oneshot = -1, skip = pipe_skip_mode();   // -1: not looked up
    int writer = 0;                              // the draw waits as one-shot by draw_writes alone
    if (vs_wait || fs_wait || ts_wait) {
        vk_exec_ahead(ex);   // the later draws' shaders and stage libraries are queued while this one waits or is skipped
        oneshot = !vk_exec_targets_recent(ex, skip >= 1);
        // only the stages that are here can say that they write: one still being translated has no reflection yet
        if (!oneshot && draw_writes(vs, ts, fs)) oneshot = writer = 1, WS.writers++;
        if (!oneshot)   // skipped until ready
            return perf_count(PERF_CNT_SHADER_SKIP), draw_skipped(ex, mesh, SK_SHADER_PENDING);
        perf_count(PERF_CNT_ONESHOT_WAIT);
        // from the frame's wait pool (NX2NAVI_ONESHOT_BUDGET=0: until each is translated); a shader the pool does not
        // reach stays at the front of the translators' queue, and the draw is skipped like a per-frame one
        uint64_t *pool = oneshot_budget_on() ? frame_wait() : NULL, x0 = kern_now_ns();
        uint32_t ms;
        if (vs_wait && (ms = pool_ms(pool, x0))) vs = shader_for(ex, vst, &has_vs, &vs_wait, ms);
        if (ts_wait && (ms = pool_ms(pool, x0))) ts = shader_for(ex, NVN_STAGE_TASK, &has_ts, &ts_wait, ms);
        if (fs_wait && (ms = pool_ms(pool, x0))) fs = shader_for(ex, NVN_STAGE_FS, &has_fs, &fs_wait, ms);
        pool_charge(pool, x0);
        wait_note(WK_SHADER, x0, vs_wait || ts_wait || fs_wait);
        if (stutter_map_on)   // the shaders translated by now name it; one still translating is not known yet
            stutter_miss(1, mesh && !fb, vs && !vs_wait ? gfx_shader_input_id(vs) : NULL,
                         fs && !fs_wait ? gfx_shader_input_id(fs) : NULL, kern_now_ns() - x0, vs_wait || ts_wait || fs_wait);
        if (vs_wait || ts_wait || fs_wait)
            return perf_count(PERF_CNT_SHADER_SKIP), draw_skipped(ex, mesh, SK_SHADER_PENDING);
    }
    if (!vs || (has_fs && !fs) || (has_ts && !ts)) return draw_skipped(ex, mesh, SK_SHADER_FAILED);
    if (shader_skipped(vs) || shader_skipped(fs)) return draw_skipped(ex, mesh, SK_SHADER_FILTER);
    if (vk_dlaa_skip_draw(ex, fs)) return draw_skipped(ex, mesh, SK_DLAA);   // the game's FXAA off, or its scaling replaced by DLSS (vk_dlaa.c)
    uint32_t fbl = feedback_prepare(ex, fs);
    VkPipeState s = ex->pipe;
    s.vs = vs, s.fs = fs, s.ts = ts;
    if (mesh) foliage_shadow_cull(ex, &s);
    if (fb) memset(s.vattr, 0, sizeof s.vattr), memset(s.vstream, 0, sizeof s.vstream);   // whatever the last vertex draw left
    s.feedback = (uint8_t)((fbl & 0xff ? 1 : 0) | (fbl & 0x100 ? 2 : 0));
    s.prim = (uint8_t)prim;
    s.depth_minus_w = (uint8_t)(nvn_depth_minus_w() != 0);
    s.y_flip = 1;
    s.prim_restart = (uint8_t)(restart != 0);
    const VkPipeInfo *info = NULL;
    int pending;
    // A draw of at most 6 vertices over half the render area or more (a full-screen triangle or quad, a tile) waits for
    // its pipeline: skipping it can leave a black tile. Those waits and the one-shot draws' come from the frame's wait
    // pool (frame_wait; NX2NAVI_PIPE_SKIP=fullscreen skips the full-screen draws instead, and with
    // NX2NAVI_ONESHOT_BUDGET=0 a one-shot draw waits up to ONESHOT_WAIT_MS outside the pool). Everything else waits
    // for its fast link from the small budget of link_budget and is skipped while its pipeline is built.
    uint32_t wait_ms = 0;
    uint64_t t0 = 0;
    uint64_t *budget = NULL;   // the per-frame budget a timed wait is charged to
    int linking = 0;           // the wait is the short one for a fast link (link_budget)
    int fullscreen = 0;        // the wait is a full-screen draw's
    if (oneshot > 0 && oneshot_budget_on()) {
        budget = frame_wait();
        wait_ms = oneshot_ms(budget, mesh && !fb, &s);
        if (wait_ms) t0 = kern_now_ns();
    } else if (oneshot > 0) {
        wait_ms = ONESHOT_WAIT_MS;
    } else if (skip < 2 && count <= 6 && (uint64_t)sc.extent.width * sc.extent.height * 2 >= (uint64_t)ex->pass_w * ex->pass_h) {
        budget = frame_wait(), fullscreen = 1;
        wait_ms = (uint32_t)(*budget / 1000000);
        if (wait_ms) t0 = kern_now_ns();
    } else {
        budget = link_budget();
        // A frame that has left a draw out already is not shown, or is shown without that draw (pending_draws,
        // vk_backend.c frame_hold): a wait for another pipeline would only make it later.
        wait_ms = pending_draws ? 0 : (uint32_t)(*budget / 1000000);
        if (wait_ms > LINK_DRAW_MS) wait_ms = LINK_DRAW_MS;
        linking = wait_ms != 0;
        if (!wait_ms) budget = NULL;
    }
    // The previous draw's pipeline while its state repeats (PM, NX2NAVI_PIPE_MEMO=0 turns it off): the cache key is made
    // from the state alone, and a pipeline once found only changes from its fast link to its optimised build, read
    // from the info the cache entry keeps.
    VkPipeline p;
    const char *why = NULL;
    uint64_t w0 = 0, m0 = 0;   // m0: when the lookup missed (wait_note)
    int kind = WK_LINK;
    if (pipe_memo_on() && cx->PM.valid && !memcmp(&cx->PM.s, &s, sizeof s)) {
        info = cx->PM.info, pending = 0;
        p = __atomic_load_n(&info->pipeline, __ATOMIC_ACQUIRE);
    } else {
        // Nearly every draw finds its pipeline, so the first lookup waits for nothing and the clock is read only for a
        // draw that missed. A miss first has the rest of the submission read ahead (vk_exec_ahead: the stage libraries
        // of the later draws' pipelines build on the workers from here on), inside this draw's wait budget.
        p = vk_pipeline_graphics_async(&s, &info, &pending, 0);
        if (pending) {
            m0 = kern_now_ns();
            kind = oneshot > 0 ? (mesh ? WK_ONESHOT_MESH : WK_ONESHOT) : fullscreen ? WK_FULL : WK_LINK;
            if (linking) t0 = kern_now_ns();
            else w0 = t0 ? t0 : wait_ms ? kern_now_ns() : 0;
            vk_exec_ahead(ex);
            // The fast link's wait is one for each pipeline, counted from its first miss (link_budget): what that left
            // unbuilt is a compile, and its later draws, in this frame and the next ones, wait for nothing.
            double building = wait_ms && linking ? vk_pipeline_building_ms(&s) : 0;
            if (wait_ms && linking) {
                uint32_t left = building >= LINK_DRAW_MS ? 0 : LINK_DRAW_MS - (uint32_t)building;
                if (wait_ms > left) wait_ms = left;
            }
            // A mesh pipeline without stage libraries that is known to be compiling gets no fast link, so the few
            // milliseconds a draw waits for one are lost in every frame that meets it until the compile ends, 0.2 to
            // 1.1 s later (MESH_WAIT_MS).
            if (wait_ms && linking && building > MESH_WAIT_MS && mesh_whole(mesh && !fb)) wait_ms = 0;
            if (wait_ms) p = vk_pipeline_graphics_async(&s, &info, &pending, wait_ms);
        }
        if (t0 && budget) {
            uint64_t spent = kern_now_ns() - t0;
            *budget = spent < *budget ? *budget - spent : 0;
        }
        why = writer ? "writes memory, shader" : oneshot > 0 ? "one-shot, shader" : linking ? NULL
            : wait_ms ? "full-screen" : NULL;
        // Still pending after the wait any draw gets: a draw into targets that are not per-frame waits as one-shot,
        // and so does a draw into per-frame targets whose shaders write memory (draw_writes).
        if (pending && oneshot < 0 && (!vk_exec_targets_recent(ex, skip >= 1) || (writer = draw_writes(vs, ts, fs)))) {
            perf_count(PERF_CNT_ONESHOT_WAIT);
            WS.writers += (uint64_t)writer;
            why = writer ? "writes memory" : "one-shot";
            kind = mesh ? WK_ONESHOT_MESH : WK_ONESHOT;
            if (!w0) w0 = kern_now_ns();
            if (oneshot_budget_on()) {
                uint64_t *pool = frame_wait(), b0 = kern_now_ns();
                p = vk_pipeline_graphics_async(&s, &info, &pending, oneshot_ms(pool, mesh && !fb, &s));
                pool_charge(pool, b0);
            } else {
                p = vk_pipeline_graphics_async(&s, &info, &pending, ONESHOT_WAIT_MS);
            }
        }
        // A full-screen draw still pending after its budget: skipped, it leaves the frame black or unlit. While the
        // previous picture can stay on screen in its place (vk_frame_hold_left) that is what happens; once that time
        // is used up the draw waits on, from the FULLSCREEN_LAST_MS of its frame, before such a frame is shown.
        uint64_t *last = pending && fullscreen && !vk_frame_hold_left() ? last_wait() : NULL;
        if (last && *last >= 1000000) {
            why = "full-screen, nothing left to hold the picture";
            kind = WK_LAST;
            if (!w0) w0 = kern_now_ns();
            uint64_t b0 = kern_now_ns();
            p = vk_pipeline_graphics_async(&s, &info, &pending, (uint32_t)(*last / 1000000));
            pool_charge(last, b0);
        }
        if (m0) wait_note(kind, m0, pending), draw_stats_tick();
        if (m0 && stutter_map_on)
            stutter_miss(0, mesh && !fb, vs ? gfx_shader_input_id(vs) : NULL, fs ? gfx_shader_input_id(fs) : NULL,
                         kern_now_ns() - m0, pending);
        cx->PM.valid = !pending && p && info;
        if (cx->PM.valid) cx->PM.s = s, cx->PM.info = info;
    }
    uint64_t waited = w0 ? kern_now_ns() - w0 : 0;
    static int wait_logs;
    // also a one-shot draw skipped at the end of its wait, however short: its target may not be drawn again
    if (why && (waited >= 2000000 || (pending && kind == WK_ONESHOT)) && wait_logs < 200) {
        char tg[512];
        wait_logs++;
        vk_exec_targets_describe(ex, tg, sizeof tg);
        rt_log("vk_exec: frame %llu: a draw waited %.1f ms for its pipeline (%s%s%s): %u vertices, pass %ux%u, %s",
               (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1, waited / 1e6, why,
               s.feedback ? ", feedback loop" : "", pending ? ", skipped after all" : "", count, ex->pass_w, ex->pass_h,
               tg);
    }
    if (pending) return draw_skipped(ex, mesh, SK_PIPE_PENDING);   // a background worker is building it: skipped until it is ready
    if (!p || !info) return draw_skipped(ex, mesh, SK_PIPE_FAILED);
    if (info->pool && !pool_on()) return draw_skipped(ex, mesh, SK_POOL_OFF);
    bind_pipeline(ex, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    if (!cx->B.st_valid || memcmp(&cx->B.st, &s, sizeof s)) {
        // Only the dynamic states whose values changed (vk_pipeline_set_dynamic_delta); NX2NAVI_DYN_DELTA=0 records
        // them all after every pipeline bind, as before.
        static int delta = -1;
        if (delta < 0) {
            delta = cfg_bool("NX2NAVI_DYN_DELTA");
            if (!delta) rt_log("vk_exec: every dynamic state recorded after each pipeline bind (NX2NAVI_DYN_DELTA=0)");
        }
        // an emulated mesh draw (NX2NAVI_MESH_EMULATION) binds a vertex pipeline: its topology, primitive restart and
        // vertex input are dynamic and are set as for any vertex draw
        if (delta) vk_exec_frame.dyn_calls += (uint64_t)vk_pipeline_set_dynamic_delta(ex->cb, &s, &cx->B.dyn, mesh && !fb && !info->emu,
                                                                                       &vk_exec_frame.dyn_left);
        else vk_pipeline_set_dynamic(ex->cb, &s);
        ex->recorded++;
        cx->B.st = s, cx->B.st_valid = 1;
        if (ex->mark && vk_exec_frame.draws == ex->mark_draws0) ex->mark->first[0] = vs, ex->mark->first[1] = fs;
        if (vk_draw_marks_on)   // NX2NAVI_DRAW_CRUMBS: a lost device then names these shaders (vk_crumbs.c vk_draw_mark)
            vk_draw_mark(ex->cb, ex->mark, vs, fs, (uint32_t)(vk_exec_frame.draws - ex->mark_draws0));
    }
    vk_pipeline_line_width(ex->cb, &s, ex->line_width);   // a draw of lines: the game's width; else nothing
    if (!mesh) {
        vk_dlaa_draw(ex);
        vk_mvsurvey_draw(ex);
        vk_mvprev_draw(ex);
    }
    vk_dlaa_jitter_check(ex);   // a draw with another jitter than the viewport recorded last records it again
    record_fixed(ex);
    repeat_stats(ex, info);
    {   // the replay's binding path is the CPU cost the frame profile needs to separate from recording
        uint64_t bt = bt_now();
        bind_resources(ex, info);
        vk_exec_frame.bind_ns += (uint64_t)(bt_now() - bt);
        vk_exec_frame.bind_calls++;
    }
    if (vk_texture_deferred()) return 0;   // a chunk's lookup waits for its turn: the op is replayed again
    if (fbl & ex->fb_dirty) feedback_barrier(ex);
    feedback_written(ex, &s, fbl);
    if (!mesh) bind_vertex_buffers(ex);
    if (vk_dump_armed) vk_dump_draw(ex, &s, p, info, mesh && !fb);
    if (vk_hdr_track) vk_hdr_draw(ex, info);
    if (vk_texture_deferred()) return 0;
    vk_exec_oq_draw(ex);
    DRS.issued[mesh]++;
    draw_stats_tick();
    PD_info = info;
    return 1;
}

static int prepare_draw(VkExec *ex, int prim, int32_t count, int restart) {
    return prepare_draw_ex(ex, prim, count, restart, 0);
}

// ---- looking ahead (vk_exec.c vk_exec_ahead): the binds and draws of ops the replay has not reached, applied to sx, a
// copy of the replay state. A draw looks its shaders up as prepare_draw_ex will (one not in memory is queued for its
// disk read or translation by that lookup) and names its state to vk_pipeline_hint. The last draw looked up is kept:
// one with the same program, texture binds and state repeats it, as most draws of a pass do.
// A mesh draw is looked up with its mesh and task stages where its pipeline will be a mesh pipeline with stage
// libraries (vk_pipeline_mesh_libs; never with the mesh fallback, which draws through vertex pipelines of its own).
uint64_t vk_exec_ahead_draws, vk_exec_ahead_lookups;
static struct { int valid; const prog_t *prog; uint64_t serial; uint32_t tex_serial; VkPipeState pipe; } AHL;

void vk_exec_ahead_begin(void) { AHL.valid = 0; }

static void ahead_draw(VkExec *sx, int mesh) {
    vk_exec_ahead_draws++;
    if (!sx->prog || (mesh && (!vk_pipeline_mesh_libs() || vk_ctx()->mesh_fallback))) return;
    if (AHL.valid && AHL.prog == sx->prog && AHL.serial == sx->prog->serial && AHL.tex_serial == sx->tex_serial &&
        !memcmp(&AHL.pipe, &sx->pipe, sizeof AHL.pipe))
        return;
    AHL.valid = 1, AHL.prog = sx->prog, AHL.serial = sx->prog->serial, AHL.tex_serial = sx->tex_serial, AHL.pipe = sx->pipe;
    vk_exec_ahead_lookups++;
    int has_vs, has_fs, has_ts = 0, vs_wait = 0, fs_wait = 0, ts_wait = 0;
    const GfxShader *vs = shader_for(sx, mesh ? NVN_STAGE_MESH : NVN_STAGE_VS, &has_vs, &vs_wait, 0), *ts = NULL;
    if (!has_vs) return;   // a program without the stage this draw starts from: no libraries
    if (mesh) ts = shader_for(sx, NVN_STAGE_TASK, &has_ts, &ts_wait, 0);
    const GfxShader *fs = shader_for(sx, NVN_STAGE_FS, &has_fs, &fs_wait, 0);
    if (!vs || vs_wait || fs_wait || ts_wait || (has_fs && !fs) || (has_ts && !ts) || shader_skipped(vs) ||
        shader_skipped(fs))
        return;
    VkPipeState s = sx->pipe;
    s.vs = vs, s.fs = fs, s.ts = ts;
    s.feedback = 0;
    s.prim = NVN_DRAW_PRIMITIVE_TRIANGLES;
    s.depth_minus_w = (uint8_t)(nvn_depth_minus_w() != 0);
    s.y_flip = 1;
    s.prim_restart = 0;
    vk_pipeline_hint(&s);
}

void vk_exec_ahead_op(VkExec *sx, uint32_t op, const void *payload) {
    if (!sx->dc) sx->dc = dc;
    switch (op) {
    case OP_BIND_PROGRAM: {
        const p_bind_program *b = payload;
        sx->prog = b->prog, sx->prog_stages = b->stages;
        break;
    }
    case OP_BIND_STATE: {
        const p_bind_state *b = payload;
        blend_st bs;
        switch (b->kind) {
        case 0:
            memcpy(&bs, b->bytes, sizeof bs);
            if (bs.target < 8) sx->pipe.blend[bs.target] = bs;
            break;
        case 1: memcpy(&sx->pipe.color, b->bytes, sizeof sx->pipe.color); break;
        case 2: memcpy(&sx->pipe.chmask, b->bytes, sizeof sx->pipe.chmask); break;
        case 3: memcpy(&sx->pipe.ms, b->bytes, sizeof sx->pipe.ms); break;
        case 4: memcpy(&sx->pipe.poly, b->bytes, sizeof sx->pipe.poly); break;
        case 5: memcpy(&sx->pipe.ds, b->bytes, sizeof sx->pipe.ds); break;
        }
        break;
    }
    case OP_BIND_VATTR: {
        const p_bind_vattr *b = payload;
        if (b->first < 0 || b->n < 0 || b->first + b->n > 16) break;
        memset(sx->pipe.vattr, 0, sizeof sx->pipe.vattr);
        memcpy(&sx->pipe.vattr[b->first], b->st, sizeof(uint32_t) * (size_t)b->n);
        break;
    }
    case OP_BIND_VSTREAM: {
        const p_bind_vstream *b = payload;
        if (b->first < 0 || b->n < 0 || b->first + b->n > 16) break;
        memset(sx->pipe.vstream, 0, sizeof sx->pipe.vstream);
        memcpy(&sx->pipe.vstream[b->first], b->st, sizeof(vs_st) * (size_t)b->n);
        break;
    }
    case OP_BIND_TEX: case OP_BIND_IMG: {
        const p_bind_handles *b = payload;
        int cap = op == OP_BIND_TEX ? 128 : 32;
        if (b->stage < 0 || b->stage >= 8 || b->first < 0 || b->n < 0 || b->n > 32 || b->first + b->n > cap) break;
        memcpy(op == OP_BIND_TEX ? &sx->tex[b->stage][b->first] : &sx->img[b->stage][b->first], b->h,
               sizeof(uint64_t) * (size_t)b->n);
        sx->tex_serial++;
        break;
    }
    case OP_DRAW: case OP_DRAW_IDX: case OP_DRAW_IDX_IND: ahead_draw(sx, 0); break;
    case OP_DRAW_MESH: case OP_DRAW_MESH_IND: ahead_draw(sx, 1); break;
    default: break;
    }
}

// ---- per-object motion
// The camera pass (vk_dlaa.c mv_pass) gives everything the camera's motion, so a character that moves with the camera,
// or on its own, gets the wrong one and DLSS throws its history away there. Each solid scene draw whose position reads
// an object's own uniform blocks (bones c5, place c7, c6; mvobj_draw_ says which are left out and why) is therefore
// drawn a second time (listed during the replay, MQ, and recorded by mv_pass) with its vertex shader's last-frame
// variant (translator patch 0015: the position again from last frame's blocks, bound at slot bank + 15, and last
// frame's camera, written to a spare output) and the fragment shader of mvobj_fs_spv.h, which writes last frame's window
// position minus this frame's. The draw keeps everything else of the original: vertex and index buffers, the other
// uniform blocks, the jittered viewport and the scene depth, tested with the game's own test and its equal case
// (LEQUAL, GEQUAL) without writes, so only the visible surface writes its vector. Last frame's blocks:
// vk_mvprev_find and mo_prev_block; the camera: vk_dlaa_prev_camera. A draw whose variant or pipeline is still
// building, or whose last-frame blocks are not in the snapshot, keeps the camera's motion. NX2NAVI_DLAA_OBJMV=0 turns it
// off (vk_mvprev.c).
#include "mvobj_fs_spv.h"
#include "link_motion_spv.h"
static const char MO_FS_RES[] = "{\"key\":\"mvobj\",\"stage\":\"ps\",\"notes\":[],\"cbufs\":[{\"binding\":1,\"slot\":3}],"
                                "\"sbufs\":[],\"textures\":[],\"images\":[]}";
static const char LINK_FS_RES[] = "{\"key\":\"link-motion\",\"stage\":\"ps\",\"cbufs\":[{\"binding\":1,\"slot\":3}],"
    "\"textures\":[{\"binding\":2,\"type\":3}],\"sbufs\":[],\"images\":[]}";
static struct {
    const GfxShader *fs[16], *link_fs[16];     // per input Location
    int failed;
    uint64_t cand, still, draws, nodata, building, ran, dropped;
    uint64_t ns_check, ns_rec, n_rec, ns_emit, ns_still;
    uint64_t ns_part[3], n_part[3], misses, cyc[3], ncyc, big;   // checks: filter, data and stillness (memo misses), variant and pipeline   // time spent: checks and lookups of every candidate; recording the draws again   // cand: draws whose vertex shader reads c5 or c7
    uint64_t why[5];             // nodata by cause: camera, bones, place, bones copy, place copy
    uint64_t no_out, storage, no_fs;   // variant without the extra output, with storage resources; no fs
    uint64_t other_cam;          // listed draws left out: their camera block is not the scene camera
    uint64_t lk[14][5];          // last-frame lookups per binding index: none, self, side, nearer, refused (another object)
    uint64_t e_draws, e_pipe, e_dyn, e_push, e_pushw, e_sets, e_vb, e_setw, e_setreuse;   // emitted commands (summary)
} MO;

// The motion draws of the frame, kept as a list while the scene replays and recorded into the replay's own command
// buffer by vk_mvobj_execute, through the recorder thread (vk_rec.h) like every other draw: recording them straight
// into a secondary command buffer made the driver's work for each one on the replay thread. What a draw shares with
// the one before it (pipeline, dynamic state, viewport, buffers, descriptors) is recorded once; a push descriptor set
// is pushed again only where a binding changed (pushes update the set in place while its layout stays bound).
typedef struct {
    VkPipeline pipe;
    const VkPipeInfo *info;
    uint32_t st, desc, vb;       // indices into MQ.st, MQ.desc (the draw's packed descriptors) and MQ.vb
    VkDescriptorSet set[2];      // allocated sets; VK_NULL_HANDLE for a pushed one
    VkViewport v;
    VkRect2D sc;
    float bias[3];
    VkBuffer ib;
    VkDeviceSize ib_off;
    VkIndexType it;
    uint32_t count, instances, base_instance;
    int32_t base_vertex;
    uint64_t cam;                // the draw's camera block, this frame's
    int pose_index, pose_view;
    int skin;                    // vk_skin_motion.inc: 0 not a skinned character's draw; 1 one, 2 one checked against
                                 // last frame's depth; -1 one skin_resolve left to the camera
} MoDraw;
typedef struct { VkBuffer b[17]; VkDeviceSize o[17], sz[17], str[17]; } MoVb;
static struct {
    uint64_t frame, fence;       // the frame and submission the list belongs to
    VkFormat depth_fmt;
    MoDraw *d;
    VkPipeState *st;
    VkDescSlot *desc;
    MoVb *vb;
    int n, cap, nst, cst, nvb, cvb;
    uint32_t ndesc, cdesc;
    NVNbufferRange vin[17];      // the vertex ranges MQ.vb[nvb - 1] was resolved from, under vk_mem_gen vgen
    uint64_t vgen;
    float sup_in[3];             // the support buffer last filled for the list and its inputs
    VkDescriptorBufferInfo sup;
    float par_in[8];             // the fragment shader's parameters last written and their inputs
    VkDescriptorBufferInfo par;
    VkDescriptorSetLayout aset_layout[2];   // the last allocated set per set number, with its descriptors
    VkDescriptorSet aset[2];
    uint32_t aset_n[2];
    VkDescSlot aset_d[2][MAX_DESC];
} MQ;

static void *mo_grow(void *p, int *cap, int need, size_t elem) {
    if (need <= *cap) return p;
    int c = *cap ? *cap : 256;
    while (c < need) c *= 2;
    if (!(p = realloc(p, elem * (size_t)c))) rt_fatal("vk_exec: out of memory");
    *cap = c;
    return p;
}

// The position bank masks of the draws considered (gfx_shader_mvprev_banks), with counts, for the summary.
static struct { uint32_t mask; uint64_t n; int ubo, ssbo, tex; } MM[32];   // ubo, ssbo, tex: the first vertex shader's
static void mo_mask_note(uint32_t banks, const GfxShader *vs) {
    for (int i = 0; i < 32; i++)
        if (MM[i].mask == banks || !MM[i].n) {
            if (!MM[i].n) {
                const GfxShaderRes *r;
                MM[i].ubo = gfx_shader_resources(vs, GFX_RES_UBO, &r), MM[i].ssbo = gfx_shader_resources(vs, GFX_RES_SSBO, &r);
                MM[i].tex = gfx_shader_resources(vs, GFX_RES_TEX, &r);
            }
            MM[i].mask = banks, MM[i].n++;
            return;
        }
}

static void mo_drop(void) {
    if (MQ.n) MO.dropped++;
    MQ.n = MQ.nst = MQ.nvb = 0, MQ.ndesc = 0, MQ.frame = 0;
}

static uint64_t mo_gen = 1;   // the list's snapshot (mo_prev_cached), bumped by mo_begin
static struct { uint32_t mask, chg, n; } MF[16];   // this list's draws per position banks and changed banks
static char fm_last[400];                          // the last executed list's, formatted (vk_mvobj_frame_masks)

static void skin_begin(uint64_t frame);
// Starts this frame's list (the old one, never executed, is dropped).
static void mo_begin(VkExec *ex, uint64_t frame) {
    mo_drop();
    skin_begin(frame);
    mo_gen++;
    memset(MF, 0, sizeof MF);
    MQ.frame = frame, MQ.fence = ex->fence, MQ.depth_fmt = ex->depth.format;
    memset(MQ.vin, 0, sizeof MQ.vin);
    MQ.vgen = 0, MQ.sup.buffer = MQ.par.buffer = VK_NULL_HANDLE;
    memset(MQ.aset, 0, sizeof MQ.aset);
}

static const GfxShader *mo_fs(int loc) {
    if (loc < 0 || loc >= 16) return NULL;
    if (!MO.fs[loc]) {
        uint32_t w[sizeof mvobj_fs_spv / 4];
        memcpy(w, mvobj_fs_spv, sizeof w);
        w[MVOBJ_FS_LOCATION_WORD] = (uint32_t)loc;
        MO.fs[loc] = gfx_shader_own(w, sizeof w / 4, MO_FS_RES);
        if (!MO.fs[loc]) MO.failed = 1, rt_log("vk_exec: the per-object motion fragment shader is malformed: off");
    }
    return MO.fs[loc];
}

static const GfxShader *link_motion_fs(int loc) {
    if (loc < 0 || loc >= 16) return NULL;
    if (MO.link_fs[loc]) return MO.link_fs[loc];
    uint32_t words[sizeof link_motion_spv / sizeof(uint32_t)];
    memcpy(words, link_motion_spv, sizeof words);
    for (size_t i = 5; i < sizeof words / sizeof *words; i += words[i] >> 16) {
        // OpDecorate Location 15 uniquely identifies the previous clip position in link_motion.frag.
        if (words[i] == ((4u << 16) | 71u) && words[i + 2] == 30 && words[i + 3] == 15) {
            words[i + 3] = (uint32_t)loc;
            MO.link_fs[loc] = gfx_shader_own(words, sizeof words / sizeof *words, LINK_FS_RES);
            break;
        }
    }
    return MO.link_fs[loc];
}

// The write of binding b from its packed descriptors.
static VkWriteDescriptorSet mo_write(const VkPipeBinding *b, const VkDescSlot *d) {
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b->binding,
                              .descriptorCount = b->count ? b->count : 1, .descriptorType = b->type};
    if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || b->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
        b->type == VK_DESCRIPTOR_TYPE_SAMPLER)
        w.pImageInfo = &d->image;
    else
        w.pBufferInfo = &d->buffer;   // VkDescSlot is 24 bytes, as VkDescriptorBufferInfo: arrays line up
    return w;
}

// The descriptors of a motion draw, packed into MQ.desc at q->desc without the binding caches of the command buffer
// being replayed (B, DS, RS), which then stay true for its next draw: uniform buffers (through ubo_info, so with
// mo_ovr), the support buffer and sampled textures. An allocated set is written here (or the last one reused when it
// would hold the same); a pushed set is pushed by mo_emit. 0 for a pipeline with storage buffers, images or texel
// buffers (not drawn again).
static int mo_desc(VkExec *ex, const VkPipeInfo *info, MoDraw *q, const VkDescriptorImageInfo *pose_depth) {
    gpa_reader(info);
    if (info->n_bindings > MAX_BINDINGS || info->pool) return 0;
    uint32_t total = 0;
    for (uint32_t i = 0; i < info->n_bindings; i++) total += info->bindings[i].count ? info->bindings[i].count : 1;
    if (total > MAX_DESC) return 0;
    int cdesc = (int)MQ.cdesc;
    MQ.desc = mo_grow(MQ.desc, &cdesc, (int)(MQ.ndesc + total), sizeof *MQ.desc);
    MQ.cdesc = (uint32_t)cdesc;
    VkDescSlot *d = MQ.desc + MQ.ndesc;
    memset(d, 0, total * sizeof *d);   // unused bytes zero: equal descriptors compare equal
    for (uint32_t i = 0, nd = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *b = &info->bindings[i];
        const GfxShaderRes *r = b->res;
        uint32_t cnt = b->count ? b->count : 1;
        VkDescSlot *o = &d[nd];
        int st = nvn_stage_of(b->stage);
        if (b->kind == VK_PIPE_SUPPORT) {
            float in[3];
            support_in(ex, in);
            if (!MQ.sup.buffer || memcmp(in, MQ.sup_in, sizeof in)) {
                VkBuffer sb;
                VkDeviceSize so;
                support_fill(scratch(ex, SUPPORT_BYTES, &sb, &so), in[0], in[1], in[2]);
                MQ.sup = (VkDescriptorBufferInfo){sb, so, SUPPORT_BYTES};
                memcpy(MQ.sup_in, in, sizeof in);
            }
            o->buffer = MQ.sup;
        } else if (b->kind == GFX_RES_UBO) {
            for (uint32_t k = 0; k < cnt; k++) o[k].buffer = ubo_info(ex, st, r->slot + (int)k);
        } else if (b->kind == GFX_RES_SSBO && !(r->flags & 1)) {   // read only: instance data of foliage and such
            for (uint32_t k = 0; k < cnt; k++) o[k].buffer = ssbo_info(ex, st, r, 0);
        } else if (pose_depth && st == NVN_STAGE_FS && b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            if (cnt != 1) return 0;
            o->image = *pose_depth;
        } else if (b->kind == GFX_RES_TEX && b->type != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
            vk_texture_lock();
            for (uint32_t k = 0; k < cnt; k++) {
                int slot = r->slot + (int)k;
                tex_info(ex, r, slot >= 0 && slot < 128 ? ex->tex[st][slot] : 0, 0, r->separate, &o[k].image, NULL);
            }
            vk_texture_unlock();
        } else {
            return 0;
        }
        nd += cnt;
    }
    uint32_t n_sets = info->n_sets ? info->n_sets : 1;
    q->set[0] = q->set[1] = VK_NULL_HANDLE;
    for (uint32_t s = 0, i0 = 0, k0 = 0; s < n_sets && s < 2; s++) {
        uint32_t i1 = i0, k1 = k0;
        while (i1 < info->n_bindings && (n_sets == 1 || info->bindings[i1].set == s))
            k1 += info->bindings[i1].count ? info->bindings[i1].count : 1, i1++;
        if (i1 > i0 && !(s ? info->push1 : info->push)) {
            VkDescriptorSetLayout sl = s ? info->set_layout1 : info->set_layout;
            if (MQ.aset[s] && MQ.aset_layout[s] == sl && MQ.aset_n[s] == k1 - k0 &&
                !memcmp(MQ.aset_d[s], d + k0, (k1 - k0) * sizeof *d)) {
                q->set[s] = MQ.aset[s];
                MO.e_setreuse++;
            } else {
                static VkWriteDescriptorSet w[MAX_BINDINGS];
                VkDescriptorSet set = alloc_set(ex, sl);
                for (uint32_t i = i0, k = k0; i < i1; k += info->bindings[i].count ? info->bindings[i].count : 1, i++)
                    w[i - i0] = mo_write(&info->bindings[i], d + k), w[i - i0].dstSet = set;
                vkUpdateDescriptorSets(vk_ctx()->device, i1 - i0, w, 0, NULL);
                q->set[s] = MQ.aset[s] = set, MQ.aset_layout[s] = sl, MQ.aset_n[s] = k1 - k0;
                memcpy(MQ.aset_d[s], d + k0, (k1 - k0) * sizeof *d);
                MO.e_setw++;
            }
        }
        i0 = i1, k0 = k1;
    }
    q->desc = MQ.ndesc;
    MQ.ndesc += total;
    return 1;
}

// The draw's vertex buffers as bind_vertex_buffers resolves them, into MQ.vb; the previous entry again while the
// ranges and vk_mem_gen are the same (most motion draws in a row are one model's parts).
static uint32_t mo_vertex_buffers(VkExec *ex) {
    uint64_t gen = vk_mem_gen();
    if (MQ.nvb && MQ.vgen == gen && !memcmp(MQ.vin, ex->vbuf, sizeof MQ.vin)) {
        const MoVb *l = &MQ.vb[MQ.nvb - 1];
        int same = 1;
        if (!vk_pipeline_vertex_input_dynamic())
            for (int b = 0; b < 16 && same; b++) same = l->str[b] == ex->pipe.vstream[b].stride;
        if (same) return (uint32_t)MQ.nvb - 1;
    }
    MQ.vb = mo_grow(MQ.vb, &MQ.cvb, MQ.nvb + 1, sizeof *MQ.vb);
    MoVb *v = &MQ.vb[MQ.nvb];
    for (int b = 0; b < 17; b++) {
        v->b[b] = Z.zbuf, v->o[b] = 0, v->sz[b] = ZERO_BYTES, v->str[b] = b < 16 ? ex->pipe.vstream[b].stride : 0;
        if (b == VK_PIPE_ZERO_BINDING) continue;
        const NVNbufferRange *r = &ex->vbuf[b];
        VkMemSpan sp;
        if (!r->address || !vk_mem_resolve(r->address, 1, &sp)) continue;
        VK_MEM_USE(sp);
        v->b[b] = sp.buffer, v->o[b] = sp.offset, v->sz[b] = r->size && r->size < sp.avail ? r->size : sp.avail;
    }
    memcpy(MQ.vin, ex->vbuf, sizeof MQ.vin);
    MQ.vgen = gen;
    return (uint32_t)MQ.nvb++;
}

// Records the list into cb (the replay's command buffer, inside the motion pass's rendering), skipping what the draw
// before already set.
static void mo_emit(VkCommandBuffer cb) {
    int dyn_vi = vk_pipeline_vertex_input_dynamic();
    VkPipeline pipe = VK_NULL_HANDLE;
    uint32_t st = UINT32_MAX, vb = UINT32_MAX;
    VkPipelineLayout lay[2] = {0};
    const VkPipeInfo *pinfo[2] = {0};
    int pdesc[2] = {-1, -1};      // the pushed set's descriptors (MQ.desc index) under lay/pinfo, -1 none
    VkDescriptorSet bound[2] = {0};
    const MoDraw *p = NULL;
    for (int n = 0; n < MQ.n; p = &MQ.d[n++]) {
        const MoDraw *q = &MQ.d[n];
        const VkPipeInfo *info = q->info;
        if (q->pipe != pipe) vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, q->pipe), pipe = q->pipe, MO.e_pipe++;
        if (q->st != st) vk_pipeline_set_dynamic(cb, &MQ.st[q->st]), st = q->st, MO.e_dyn++;
        if (!p || memcmp(&q->v, &p->v, sizeof q->v)) vkCmdSetViewportWithCount(cb, 1, &q->v);
        if (!p || memcmp(&q->sc, &p->sc, sizeof q->sc)) vkCmdSetScissorWithCount(cb, 1, &q->sc);
        if (!p || memcmp(q->bias, p->bias, sizeof q->bias)) vkCmdSetDepthBias(cb, q->bias[0], q->bias[1], q->bias[2]);
        uint32_t n_sets = info->n_sets ? info->n_sets : 1;
        const VkDescSlot *d = MQ.desc + q->desc;
        for (uint32_t s = 0, i0 = 0, k0 = 0; s < n_sets && s < 2; s++) {
            uint32_t i1 = i0, k1 = k0;
            while (i1 < info->n_bindings && (n_sets == 1 || info->bindings[i1].set == s))
                k1 += info->bindings[i1].count ? info->bindings[i1].count : 1, i1++;
            if (i1 > i0) {
                if (s ? info->push1 : info->push) {
                    // the bindings that differ from the last push of this layout and pipeline (all after another)
                    int same = lay[s] == info->layout && pinfo[s] == info && pdesc[s] >= 0;
                    const VkDescSlot *pd = same ? MQ.desc + pdesc[s] : NULL;
                    VkWriteDescriptorSet w[MAX_BINDINGS];
                    uint32_t nw = 0;
                    for (uint32_t i = i0, k = k0; i < i1; i++) {
                        uint32_t cnt = info->bindings[i].count ? info->bindings[i].count : 1;
                        if (!same || memcmp(d + k, pd + k, cnt * sizeof *d)) w[nw++] = mo_write(&info->bindings[i], d + k);
                        k += cnt;
                    }
                    if (nw) rec_push(vk_pipeline_push_fn(), cb, VK_PIPELINE_BIND_POINT_GRAPHICS, info->layout, s, nw, w);
                    MO.e_push += nw != 0, MO.e_pushw += nw;
                    lay[s] = info->layout, pinfo[s] = info, pdesc[s] = (int)q->desc, bound[s] = VK_NULL_HANDLE;
                } else if (q->set[s] != bound[s] || lay[s] != info->layout) {
                    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, info->layout, s, 1, &q->set[s], 0, NULL);
                    bound[s] = q->set[s], lay[s] = info->layout, pinfo[s] = info, pdesc[s] = -1, MO.e_sets++;
                }
            }
            i0 = i1, k0 = k1;
        }
        if (q->vb != vb) {
            const MoVb *v = &MQ.vb[q->vb];
            vkCmdBindVertexBuffers2(cb, 0, 17, v->b, v->o, v->sz, dyn_vi ? NULL : v->str);
            vb = q->vb, MO.e_vb++;
        }
        if (!p || q->ib != p->ib || q->ib_off != p->ib_off || q->it != p->it) vkCmdBindIndexBuffer(cb, q->ib, q->ib_off, q->it);
        vkCmdDrawIndexed(cb, q->count, q->instances, 0, q->base_vertex, q->base_instance);
        MO.e_draws++;
    }
}

#include "vk_fg_pose.inc"
#include "vk_skin_motion.inc"

static int mvobj_pick(VkExec *ex, const p_draw_idx *d, const GfxShader **vs_out, uint32_t *banks_out, MotionOwner *owner_out,
                      SkinPick *skin_out, uint64_t t0);
static void mvobj_draw_(VkExec *ex, const p_draw_idx *d, VkBuffer ib, VkDeviceSize ib_off, uint64_t t0,
                        const GfxShader *vs, uint32_t banks, const MotionOwner *owner, const SkinPick *skin);
// The chunks of a parallel group (vk_exec.c) add to the frame's list at once: whether a draw is listed at all is decided
// by its own draw context (mvobj_pick, its memo per context), and only the frame's list, its memos and statistics go
// under mo_lock there: under one lock for the whole check, the chunks' scene draws waited for each other. The order of
// the list does not change the picture: its draws write motion without blending, tested against the scene depth they
// do not write.
static PlatMutex mo_lock = PLAT_MUTEX_INIT;

static void mvobj_draw(VkExec *ex, const p_draw_idx *d, VkBuffer ib, VkDeviceSize ib_off) {
    if (MO.failed || !ex->dc->B.st_valid) return;
    if (RT_V111) {
        // the upscaler, or frame generation without one (vk_dlaa_fg_only), takes the motion
        if (!(vk_dlaa_available() || vk_dlaa_fg_only()) || (!pose_enabled() && !skin_enabled())) return;
    } else if (!vk_mvprev_on()) return;
    static int timed = -1;
    if (timed < 0) timed = rt_knob("MVOBJ_TIMES", "0");   // NX2NAVI_MVOBJ_TIMES=1: the summary's times (the clock reads cost)
    uint64_t t0 = timed ? kern_now_ns() : 0;
    const GfxShader *vs;
    uint32_t banks;
    MotionOwner owner = {0};
    SkinPick skin = {0};
    int listed = mvobj_pick(ex, d, &vs, &banks, &owner, &skin, t0);
    int lk = listed && vk_exec_shared();
    if (lk) plat_mutex_lock(&mo_lock);
    if (listed) mvobj_draw_(ex, d, ib, ib_off, t0, vs, banks, &owner, skin.stride ? &skin : NULL);
    if (t0) __atomic_add_fetch(&MO.ns_check, kern_now_ns() - t0, __ATOMIC_RELAXED);
    if (lk) plat_mutex_unlock(&mo_lock);
}

// NX2NAVI_MVOBJ_BANKS=1 (test): for scene draws, the words of each other uniform bank (c3, c6, c8-c16) that changed
// since the same shader's draw last frame, logged at frame 3000: where the time that moves wind and water lives
static void mvobj_banks(VkExec *ex) {
    DrawCtx *const cx = ex->dc;
    static int on = -1;
    if (on < 0) on = rt_knob("MVOBJ_BANKS", "0");
    if (!on || !cx->B.st_valid) return;
    uint64_t frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (frame < 2999 || frame > 3000) return;
    static struct { const GfxShader *vs; int bank; uint64_t frame; float w[64]; int n; } tab[4096];
    static int logged;
    const GfxShaderRes *ur;
    int nu = gfx_shader_resources(cx->B.st.vs, GFX_RES_UBO, &ur);
    for (int i = 0; i < nu; i++) {
        int bank = ur[i].slot;
        if (bank < 3 || bank == 4 || bank == 5 || bank == 7 || bank > 16) continue;
        const p_bind_buf *u = &ex->ubo[NVN_STAGE_VS][bank - 3];
        uint32_t n = u->size < 256 ? (uint32_t)u->size / 4 : 64;
        const float *now = n ? (const float *)vk_usnap_bytes(u->addr, n * 4) : NULL;
        if (!now) continue;
        uint32_t k =
            (uint32_t)(((uintptr_t)cx->B.st.vs ^ (uint64_t)bank * 0x9E3779B97F4A7C15ull) * 0xff51afd7ed558ccdull >> 52);
        if (frame == 2999) {
            tab[k].vs = cx->B.st.vs, tab[k].bank = bank, tab[k].frame = frame, tab[k].n = (int)n;
            memcpy(tab[k].w, now, n * 4);
        } else if (tab[k].vs == cx->B.st.vs && tab[k].bank == bank && tab[k].frame == 2999 && logged < 80) {
            char line[700];
            int o = 0, nd = 0;
            for (int j = 0; j < tab[k].n && j < (int)n && o < 600; j++)
                if (memcmp(&tab[k].w[j], &now[j], 4)) nd++, o += snprintf(line + o, sizeof line - o, " [%d] %g>%g", j, tab[k].w[j], now[j]);
            if (nd) logged++, rt_log("mvbanks: vs %p c%d size %#llx: %d words%s", (const void *)cx->B.st.vs, bank,
                                     (unsigned long long)u->size, nd, line);
            tab[k].frame = 0;
        }
    }
}

// 1: old is cur a little changed or the same (every float within 10 %, at least 1.0 and at most 4.0); *changed: not
// the same. The cap: NX2navi's world positions are in the thousands, and 10 % of them let another object's block a few
// units away pass as this one moved; nothing moves 4 units in a frame.
static int mo_similar(const uint8_t *cur, const uint8_t *old, uint64_t n, int *changed) {
    *changed = 0;
    if (!memcmp(cur, old, n)) return 1;
    // word by word as integers; float arithmetic only on two normal floats (the blocks hold integers too, and
    // denormal arithmetic took this loop 1.9 million cycles a block)
    const uint32_t *x = (const uint32_t *)cur, *y = (const uint32_t *)old;
    for (uint64_t w = 0; w < n / 4; w++) {
        if (x[w] == y[w]) continue;
        uint32_t ex = x[w] >> 23 & 0xff, ey = y[w] >> 23 & 0xff;
        if (!ex || ex == 0xff || !ey || ey == 0xff) continue;   // not both normal floats: an integer or a tiny value
        float fx, fy;
        memcpy(&fx, &x[w], 4), memcpy(&fy, &y[w], 4);
        float dd = fabsf(fx - fy), tol = 0.1f * fabsf(fy);
        if (dd > (tol < 1.0f ? 1.0f : tol > 4.0f ? 4.0f : tol)) return 0;
    }
    *changed = 1;
    return 1;
}

// Last frame's copy of a block, accepted when it is this frame's block a little changed (mo_similar,
// which see): a block of another object the address rule found would make motion up. 1 and *out: found (the block itself
// when it is unchanged, *changed 0); 0: none, the draw keeps this frame's block there.
static int mo_prev_block(int index, const p_bind_buf *u, VkDescriptorBufferInfo *out, int *changed) {
    uint64_t max = LIM.ubo_max, n = u->size < max ? u->size : max, pa;
    int how;
    *changed = 0;
    // c9 and c11 keep this frame's block. With the copies the address rule finds for c11 and last frame's c9, the
    // c4 c7 c9 c11 draws (walls, cliffs, arches) got several pixels of motion under a still camera (NX2NAVI_MV_CHECK,
    // those draws alone: 34 % of the picture moving; with c11 kept, 0.8 %). With c11 kept, their c9 still changes every
    // frame while nothing moves (the check's per-draw line: about 120 such draws a frame changed in c9 alone), the
    // object scale the picture shows is 0.6-0.8 of theirs, and with Link stopped against a cliff 40-77 % of the picture
    // got 1-3 px of motion that flipped direction, three times the warp error of the camera's vectors. With c9 kept too,
    // the turning run is as good (object warp error 0.46 of the camera's). Why their copies are wrong is not known.
    // NX2NAVI_MVPREV_SKIP=<hex bank mask> (test) replaces the default a00 (bit n: cn).
    static long skip = -2;
    if (skip == -2) {
        const char *e = cfg_get("NX2NAVI_MVPREV_SKIP");
        skip = e && *e ? strtol(e, NULL, 16) : 1L << 9 | 1L << 11;
    }
    if (skip >> (index + 3) & 1) return 0;
    uint64_t c0 = __builtin_ia32_rdtsc();
    int found = vk_mvprev_find(index, u->addr, u->size, &pa, &how);
    uint64_t c1 = __builtin_ia32_rdtsc();
    MO.cyc[0] += c1 - c0, MO.ncyc++, MO.big += n;
    if (index >= 0 && index < 14) MO.lk[index][found ? how : MVPREV_NONE]++;
    if (!found || !vk_usnap_find(pa, n, LIM.ubo_align, out)) return 0;
    if (how == MVPREV_SELF) return 1;   // bound at the same address last frame: not written again
    const uint8_t *map = vk_usnap_map(), *cur = vk_usnap_bytes(u->addr, n);
    uint64_t c2 = __builtin_ia32_rdtsc();
    MO.cyc[1] += c2 - c1;
    if (!cur) return 0;
    const uint8_t *old = map + out->offset;
    static long trust = -2;   // NX2NAVI_MVPREV_TRUST=<hex bank mask> (test): the copy found is taken without the check
    if (trust == -2) {
        const char *e = cfg_get("NX2NAVI_MVPREV_TRUST");
        trust = e && *e ? strtol(e, NULL, 16) : 0;
    }
    if (trust >> (index + 3) & 1) {
        *changed = memcmp(cur, old, n) != 0;
        return 1;
    }
    int r = mo_similar(cur, old, n, changed);
    MO.cyc[2] += __builtin_ia32_rdtsc() - c2;
    if (!r && index >= 0 && index < 14) MO.lk[index][4]++;
    return r;
}

// mo_prev_block once per block and list: many draws share their blocks (the scene's c14, a model's c7 and c9 across
// its parts), and the lookup with its comparison was most of the checks' time.
static int mo_prev_cached(int index, const p_bind_buf *u, VkDescriptorBufferInfo *out, int *changed) {
    static struct { uint64_t gen, addr, size; int index, r, changed; VkDescriptorBufferInfo out; } bc[4096];
    uint32_t i = (uint32_t)((u->addr ^ (uint64_t)index << 56 ^ u->size << 40) * 0x9E3779B97F4A7C15ull >> 52);
    if (bc[i].gen == mo_gen && bc[i].addr == u->addr && bc[i].size == u->size && bc[i].index == index) {
        *out = bc[i].out, *changed = bc[i].changed;
        return bc[i].r;
    }
    int r = mo_prev_block(index, u, out, changed);
    bc[i].gen = mo_gen, bc[i].addr = u->addr, bc[i].size = u->size, bc[i].index = index;
    bc[i].r = r, bc[i].changed = *changed, bc[i].out = *out;
    return r;
}

static int mo_legacy_previous(VkExec *ex, uint64_t frame, uint32_t banks, uint64_t t0, uint64_t tp,
                              VkDescriptorBufferInfo prevb[17], uint32_t *have_out, uint32_t *changed_mask) {
    static struct { uint64_t frame, key; int moving; VkDescriptorBufferInfo b[17]; uint32_t have; } memo[8192];
    uint64_t key = banks;
    for (int b = 3; b <= 16; b++)
        if (banks >> b & 1 && b != 4) key = (key ^ ex->ubo[NVN_STAGE_VS][b - 3].addr) * 0x100000001b3ull;
    uint32_t mi = (uint32_t)(key * 0xff51afd7ed558ccdull >> 51);
    uint32_t have = 0;
    if (memo[mi].frame == frame && memo[mi].key == key) {
        if (!memo[mi].moving) {
            if (t0) MO.ns_still += kern_now_ns() - tp;
            MO.still++;
            return 0;
        }
        memcpy(prevb, memo[mi].b, sizeof memo[mi].b), have = memo[mi].have;
    } else {
        MO.misses++;
        int moving = 0;
        for (int b = 3; b <= 16; b++) {
            if (!(banks >> b & 1) || b == 4) continue;
            const p_bind_buf *u = &ex->ubo[NVN_STAGE_VS][b - 3];
            int changed;
            if (u->addr && u->size && mo_prev_cached(b - 3, u, &prevb[b], &changed))
                have |= 1u << b, moving |= changed << b;   // the banks whose block changed
        }
        memo[mi].frame = frame, memo[mi].key = key, memo[mi].moving = moving;
        memcpy(memo[mi].b, prevb, sizeof memo[mi].b), memo[mi].have = have;
        if (!moving) {
            if (t0) MO.ns_still += kern_now_ns() - tp;
            MO.still++;
            return 0;
        }
    }
    *have_out = have;
    *changed_mask = (uint32_t)memo[mi].moving;
    return 1;
}

// Whether the draw goes on to the frame's list (mvobj_draw_), with its motion variant and the banks its position reads;
// this draw context alone decides (no lock in a chunk): the draw's state, and the variant memo per context.
static int mvobj_pick(VkExec *ex, const p_draw_idx *d, const GfxShader **vs_out, uint32_t *banks_out, MotionOwner *owner_out,
                      SkinPick *skin_out, uint64_t t0) {
    DrawCtx *const cx = ex->dc;
    if (!vk_exec_in_chunk()) mvobj_banks(ex);   // a test's log of every scene draw (its tables are not shared)
    const VkExecTarget *z = &ex->depth;
    if (!z->image || z->width < DLAA_SCENE_MIN_W || z->format != VK_FORMAT_D32_SFLOAT || cx->B.st.samples > 1 ||
        cx->B.st.raster_discard)
        return 0;
    // solid geometry only, which tests and writes the scene depth: effects, particles and screen quads write none, and
    // drawn again they would put their motion on the surfaces behind them (DLSS takes those from the surface)
    if (!cx->B.st.ds.dt) return 0;
    if (!cx->B.st.ds.dw && !(RT_V111 && cx->B.st.ds.df == NVN_DEPTH_FUNC_EQUAL && ex->color[0].image &&
                         (vk_pipeline_target_wmask(&cx->B.st, 0) & 7))) return 0;
    // An opaque colour pass can use EQUAL against a depth prepass. Its geometry is already in the scene depth.
    if (RT_V111) {
        VkViewport v = base_viewport(ex);
        if (ex->ubo[0][0].size != 0x3500 || d->instances > 1 || d->base_instance || cx->B.st.color.blend_en || cx->B.st.ds.st ||
            ex->pass_layers != 1 || z->level || z->base_layer || z->layers != 1 ||
            v.x != 0.0f || v.y != (float)z->height ||
            v.width != (float)z->width || v.height != -(float)z->height) return 0;
        // the player's own draw (ownership records), else any other skinned character's (vk_skin_motion.inc)
        if (!pose_owner(ex, owner_out)) {
            __atomic_add_fetch(&pose_unowned, 1, __ATOMIC_RELAXED);
            if (!skin_pick(ex, skin_out)) return 0;
        }
    }
    // a vertex shader that reads a user bank besides the camera's (c4) may move on its own
    const GfxShaderRes *ur;
    int nu = gfx_shader_resources(cx->B.st.vs, GFX_RES_UBO, &ur), reads = 0;
    for (int i = 0; i < nu; i++) reads |= ur[i].slot >= 3 && ur[i].slot <= 16 && ur[i].slot != 4;
    if (!reads) return skin_pick_left(skin_out, SKIN_BANKS_OTHER);
    __atomic_add_fetch(&MO.cand, 1, __ATOMIC_RELAXED);
    if (t0) {
        __atomic_add_fetch(&MO.ns_part[0], kern_now_ns() - t0, __ATOMIC_RELAXED);
        __atomic_add_fetch(&MO.n_part[0], 1, __ATOMIC_RELAXED);
    }
    if (!__atomic_load_n(&LIM.ubo_max, __ATOMIC_ACQUIRE)) limits_init();
    // the variant: the translation of the same code and answers, kept per shader once found; it names the banks the
    // position depends on (0: none, the shader is skipped from then on)
    typedef struct { const GfxShader *of, *vs; uint32_t banks; } VMemo;
    VMemo *vmemo = cx->mo_vm;
    if (!vmemo && !(vmemo = cx->mo_vm = calloc(1024, sizeof *vmemo))) rt_fatal("vk: out of memory (motion memo)");
    uint32_t vi = (uint32_t)((uintptr_t)cx->B.st.vs * 0x9E3779B97F4A7C15ull >> 54);
    if (vmemo[vi].of != cx->B.st.vs) {
        int has, pending = 0;
        cx->sf_mvprev = 1;
        const GfxShader *v = shader_for(ex, NVN_STAGE_VS, &has, &pending, 0);
        cx->sf_mvprev = 0;
        if (!v || pending) {
            __atomic_add_fetch(&MO.building, 1, __ATOMIC_RELAXED);
            return skin_pick_left(skin_out, SKIN_NOT_READY);
        }
        GfxShaderIo io[32];
        uint32_t extra = gfx_shader_outputs(v, io) & ~gfx_shader_outputs(cx->B.st.vs, io);
        uint32_t banks = extra ? gfx_shader_mvprev_banks(v) & 0x1fff8u : 0;
        vmemo[vi].of = cx->B.st.vs, vmemo[vi].vs = v, vmemo[vi].banks = banks;
        if (!RT_V111) vk_mvprev_want((banks & ~(1u << 4)) >> 3);   // binding index = bank - 3
    }
    const GfxShader *vs = vmemo[vi].vs;
    uint32_t banks = vmemo[vi].banks;
    if (!banks || !(banks & ~(1u << 4))) {
        __atomic_add_fetch(&MO.no_out, 1, __ATOMIC_RELAXED);
        return skin_pick_left(skin_out, SKIN_BANKS_OTHER);
    }
    // a position without the scene camera (c4) has a view of its own (c8 c9 c10 [c13]: its c8 is at a new address every
    // frame, last frame's is never found): drawn again it would get this frame's view and no camera motion
    // (NX2NAVI_MV_CHECK while turning: 6.2 px end-point error against the camera vectors' 1.7)
    static int noc4 = -1;
    if (noc4 < 0) noc4 = rt_knob("MVOBJ_NOC4", "0");   // NX2NAVI_MVOBJ_NOC4=1 (test): drawn again all the same
    if (!(banks >> 4 & 1) && !noc4) {
        __atomic_add_fetch(&MO.no_out, 1, __ATOMIC_RELAXED);
        return skin_pick_left(skin_out, SKIN_BANKS_OTHER);
    }
    // only positions that read an object's own blocks: bones (c5), place (c7) or c6. The rest (terrain and grass: c14
    // c15 c16, with or without c9) measured no better than the camera's motion (NX2NAVI_MV_CHECK, turning, those draws
    // alone: object warp error 0.112 against the camera's 0.113, the object part 0.4 of what the picture shows) and
    // were half the draws checked and listed. NX2NAVI_MVOBJ_ALL=1 (test) takes them again.
    static int all = -1;
    if (all < 0) all = rt_knob("MVOBJ_ALL", "0");
    if (!all && !(banks & (1u << 5 | 1u << 6 | 1u << 7))) {
        __atomic_add_fetch(&MO.no_out, 1, __ATOMIC_RELAXED);
        return skin_pick_left(skin_out, SKIN_BANKS_OTHER);
    }
    // NX2NAVI_MVOBJ_ONLY=<hex bank mask> (test): only draws whose position reads no bank outside the mask (bit n: cn)
    static long only = -2;
    if (only == -2) {
        const char *e = cfg_get("NX2NAVI_MVOBJ_ONLY");
        only = e && *e ? strtol(e, NULL, 16) : -1;
    }
    if (only >= 0 && banks & ~(uint32_t)only & ~(1u << 4)) return skin_pick_left(skin_out, SKIN_BANKS_OTHER);
    static long exact = -2;   // NX2NAVI_MVOBJ_EXACT=<hex bank mask> (test): only draws with exactly these position banks
    if (exact == -2) {
        const char *e = cfg_get("NX2NAVI_MVOBJ_EXACT");
        exact = e && *e ? strtol(e, NULL, 16) : -1;
    }
    if (exact >= 0 && banks != (uint32_t)exact) return skin_pick_left(skin_out, SKIN_BANKS_OTHER);
    *vs_out = vs, *banks_out = banks;
    return 1;
}

// A draw mvobj_pick took, onto the frame's list (in a chunk under mo_lock): the list begins at the first such draw of
// the frame's submission (mo_emit drops a list of another frame, submission or depth format).
static void mvobj_draw_(VkExec *ex, const p_draw_idx *d, VkBuffer ib, VkDeviceSize ib_off, uint64_t t0,
                        const GfxShader *vs, uint32_t banks, const MotionOwner *owner, const SkinPick *skin) {
    DrawCtx *const cx = ex->dc;
    const VkExecTarget *z = &ex->depth;
    uint64_t tp = t0 ? kern_now_ns() : 0;
    uint64_t frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (MQ.frame != frame || MQ.fence != ex->fence || MQ.depth_fmt != z->format) mo_begin(ex, frame);
    mo_mask_note(banks, cx->B.st.vs);
    // last frame's camera and blocks; held still when none of the position's blocks changed (most of the scenery)
    PoseRange *pose = RT_V111 && !skin ? pose_match(ex, d, owner) : NULL;
    if (RT_V111 && !pose && !skin) return;
    SkinDraw skin_draw;
    SkinEntry skin_entry;
    uint64_t skin_listed = 0;
    VkDescriptorBufferInfo cam = {0}, prevb[17] = {0};
    uint32_t have = 0, changed = 0;
    const ActorPose *previous_pose = NULL;
    ActorPose *pose_view = NULL;
    VkDescriptorImageInfo pose_depth = {0};
    int pose_visibility_guard = 0;
    float history[4] = {0};
    float jit[2];
    if (pose) {
        const uint32_t required = (1u << 3) | (1u << 4) | (1u << 6);
        if ((banks & required) != required || (banks & ~0xf8u)) return;
        const GfxShaderRes *resources;
        if (gfx_shader_resources(vs, GFX_RES_SSBO, &resources) || gfx_shader_resources(vs, GFX_RES_TEX, &resources) ||
            gfx_shader_resources(vs, GFX_RES_IMG, &resources)) return;
        previous_pose = pose_previous(pose, ex, d, frame, banks, &pose_view);
        if (!previous_pose) return;
        pose_visibility_guard = pose_guard_needed(pose, pose_view, previous_pose);
        if (pose_visibility_guard && !vk_dlaa_pose_depth(frame, z->width, z->height, &pose_depth, history)) {
            // Missing visibility evidence is not evidence that this valid pose vector is wrong.
            // Keep the raw vector, with the normal current-depth and history-validity checks.
            pose_view->guard_missing_depth = 1;
            pose_visibility_guard = 0;
        }
        pose_view->visibility_guard = pose_visibility_guard;
        VkViewport v = draw_viewport(ex);
        jit[0] = v.x;
        jit[1] = v.y - (float)z->height;
    } else if (skin) {
        // another skinned character's draw: its blocks, and whether a bone turned sharply (vk_skin_motion.inc); which
        // draw of last frame it continues is decided when the vectors are made (skin_resolve)
        if (!skin_try(ex, d, frame, vs, banks, skin, &skin_draw, &skin_entry, &skin_listed, &pose_visibility_guard)) return;
        if (pose_visibility_guard && !vk_dlaa_pose_depth(frame, z->width, z->height, &pose_depth, history))
            pose_visibility_guard = 0;   // as for the player: no depth of last frame to check against, the plain vector
        VkViewport v = draw_viewport(ex);
        jit[0] = v.x;
        jit[1] = v.y - (float)z->height;
    } else {
        if (!vk_dlaa_prev_camera(&cam, jit)) {
            MO.nodata++, MO.why[0]++;
            return;
        }
        if (!mo_legacy_previous(ex, frame, banks, t0, tp, prevb, &have, &changed)) return;
    }
    if (t0) MO.ns_part[1] += kern_now_ns() - tp, MO.n_part[1]++, tp = kern_now_ns();
    GfxShaderIo io[32];
    uint32_t extra = gfx_shader_outputs(vs, io) & ~gfx_shader_outputs(cx->B.st.vs, io);
    const GfxShaderRes *r;
    if (gfx_shader_resources(vs, GFX_RES_IMG, &r)) {
        MO.storage++;
        return;
    }
    int loc = 31 - __builtin_clz(extra);
    const GfxShader *fs = pose_visibility_guard ? link_motion_fs(loc) : mo_fs(loc);
    if (!fs) {
        MO.no_fs++;
        if (skin) skin_left(SKIN_NOT_READY);
        return;
    }
    VkPipeState ms = cx->B.st;
    ms.vs = vs, ms.fs = fs, ms.feedback = 0;
    memset(ms.color_fmt, 0, sizeof ms.color_fmt);
    ms.color_fmt[0] = NVN_FORMAT_RG16F;
    // blending off (blend_en below); the factors stay the game's, valid NVN values
    ms.color.blend_en = 0, ms.color.logic_op = 0;
    ms.chmask.mask = 0x3;   // r and g of target 0
    ms.ms.a2c = 0;
    // the game's own test with its equal case (LESS -> LEQUAL, GREATER -> GEQUAL): the surface the draw left in the
    // scene depth passes, anything nearer hides it (NX2NAVI_MVOBJ_EQUAL=1: EQUAL, for comparison)
    static int eq = -1;
    if (eq < 0) eq = rt_knob("MVOBJ_EQUAL", "0");
    int df = cx->B.st.ds.df;
    df = (eq || pose || skin) ? NVN_DEPTH_FUNC_EQUAL : df == NVN_DEPTH_FUNC_LESS ? NVN_DEPTH_FUNC_LEQUAL
       : df == NVN_DEPTH_FUNC_GREATER ? NVN_DEPTH_FUNC_GEQUAL : df == NVN_DEPTH_FUNC_LEQUAL || df == NVN_DEPTH_FUNC_GEQUAL ? df
       : NVN_DEPTH_FUNC_EQUAL;
    ms.ds.dt = 1, ms.ds.dw = 0, ms.ds.df = (uint64_t)df, ms.ds.st = 0;
    // the pipeline per state, kept: the lookup hashes and locks the whole state
    static struct { VkPipeState st; VkPipeline p; const VkPipeInfo *info; int used; } pmemo[64];
    static int plast = -1;   // the entry of the last draw: a model's parts in a row share the state, no hash then
    uint32_t pi;
    if (plast >= 0 && pmemo[plast].used && !memcmp(&pmemo[plast].st, &ms, sizeof ms)) {
        pi = (uint32_t)plast;
    } else {
        uint64_t ph = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < sizeof ms; i += 8) {
            uint64_t w8;
            memcpy(&w8, (const uint8_t *)&ms + i, sizeof ms - i < 8 ? sizeof ms - i : 8);
            ph = (ph ^ w8) * 0x100000001b3ull;
        }
        pi = (uint32_t)(ph >> 58);
    }
    const VkPipeInfo *info = NULL;
    VkPipeline pipe;
    int pending = 0;
    if (pmemo[pi].used && !memcmp(&pmemo[pi].st, &ms, sizeof ms)) {
        info = pmemo[pi].info;
        pipe = __atomic_load_n(&info->pipeline, __ATOMIC_ACQUIRE);   // its optimised build once swapped in
        if (!pipe) {
            if (skin) skin_left(SKIN_NOT_READY);
            return;
        }
    } else {
        pipe = vk_pipeline_graphics_async(&ms, &info, &pending, 0);
        if (pending || !pipe || !info) {
            MO.building += pending != 0;
            if (skin) skin_left(SKIN_NOT_READY);
            return;
        }
        pmemo[pi].st = ms, pmemo[pi].p = pipe, pmemo[pi].info = info, pmemo[pi].used = 1;
    }
    plast = (int)pi;
    uint64_t t1 = kern_now_ns();
    if (t0) MO.ns_part[2] += t1 - tp, MO.n_part[2]++;
    if (t0) MO.ns_check -= t1 - t0;   // counted as recording below
    // into the frame's list, past the primary's binding caches (mo_desc); mo_emit records it
    MQ.d = mo_grow(MQ.d, &MQ.cap, MQ.n + 1, sizeof *MQ.d);
    MoDraw *q = &MQ.d[MQ.n];
    float par_in[8] = {(float)z->width, (float)z->height, jit[0], jit[1], history[0], history[1], history[2], history[3]};
    if (!MQ.par.buffer || memcmp(par_in, MQ.par_in, sizeof par_in)) {
        VkBuffer pbuf;
        VkDeviceSize poff;
        memcpy(scratch(ex, sizeof par_in, &pbuf, &poff), par_in, sizeof par_in);
        MQ.par = (VkDescriptorBufferInfo){pbuf, poff, sizeof par_in};
        memcpy(MQ.par_in, par_in, sizeof par_in);
    }
    memset(cx->mo_ovr, 0, sizeof cx->mo_ovr);
    if (previous_pose) {
        const uint8_t *data = previous_pose->bytes;
        for (int i = 0; i < POSE_BANKS; ++i) {
            if (!previous_pose->size[i]) continue;
            VkDescriptorBufferInfo *b = &cx->mo_ovr[NVN_STAGE_VS][18 + i];
            b->range = previous_pose->size[i];
            memcpy(scratch(ex, b->range, &b->buffer, &b->offset), data, b->range);
            data += b->range;
        }
    } else if (skin) {
        // where skin_resolve puts last frame's bytes of each block, once it knows whose they are
        for (int i = 0; i < SKIN_BANKS; i++)
            if (skin_draw.block[i] >= 0) cx->mo_ovr[NVN_STAGE_VS][18 + i] = SK.f[SK.cur].b[skin_draw.block[i]].was;
    } else {
        for (int bk = 3; bk <= 16; bk++)
            cx->mo_ovr[NVN_STAGE_VS][bk + 15] = bk == 4 ? cam : (have >> bk & 1) ? prevb[bk] : ubo_info(ex, NVN_STAGE_VS, bk);
    }
    cx->mo_ovr[NVN_STAGE_FS][3] = MQ.par;
    cx->mo_ovr_on = 1;
    int bound = mo_desc(ex, info, q, pose_visibility_guard ? &pose_depth : NULL);
    cx->mo_ovr_on = 0;
    if (!bound) {
        MO.storage++;
        if (skin) skin_left(SKIN_STORAGE);
        return;
    }
    q->pipe = pipe, q->info = info;
    if (!MQ.nst || memcmp(&MQ.st[MQ.nst - 1], &ms, sizeof ms)) {
        MQ.st = mo_grow(MQ.st, &MQ.cst, MQ.nst + 1, sizeof *MQ.st);
        MQ.st[MQ.nst++] = ms;
    }
    q->st = (uint32_t)MQ.nst - 1;
    q->v = draw_viewport(ex), q->sc = vk_exec_scissor(ex);
    q->bias[0] = ex->poly_offset.units, q->bias[1] = ex->poly_offset.clamp, q->bias[2] = slope_bias(ex);
    q->vb = mo_vertex_buffers(ex);
    q->ib = ib, q->ib_off = ib_off;
    q->it = d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    q->count = (uint32_t)d->count, q->instances = d->instances > 0 ? (uint32_t)d->instances : 1;
    q->base_vertex = d->base_vertex, q->base_instance = (uint32_t)d->base_instance;
    q->cam = ex->ubo[NVN_STAGE_VS][pose || skin ? 0 : 1].addr;
    q->pose_index = pose ? (int)(pose - pose_ranges) + 1 : 0;
    q->pose_view = pose ? (int)(pose_view - pose->history[frame & 1].view) : 0;
    q->skin = skin ? 1 + pose_visibility_guard : 0;
    if (skin) skin_commit(&skin_draw, &skin_entry, skin_listed, MQ.n);
    MQ.n++;
    MO.draws++;
    for (int i = 0; i < 16; i++)   // the frame's draws per position banks and changed banks (vk_mvobj_frame_masks)
        if (!MF[i].n || (MF[i].mask == banks && MF[i].chg == changed)) {
            MF[i].mask = banks, MF[i].chg = changed, MF[i].n++;
            break;
        }
    if (t0) MO.ns_rec += kern_now_ns() - t1, MO.n_rec++;
}

int vk_fg_pose_prepare(VkExec *ex, uint64_t cam, VkImage depth, uint32_t w, uint32_t h) {
    if (!RT_V111 || (!pose_enabled() && !skin_enabled())) return 0;
    uint64_t frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    pose_select(frame, cam, depth);
    skin_resolve(ex, frame, cam, depth, w, h);
    pose_status(frame);
    return MQ.n != 0;
}

int vk_mvobj_execute(VkExec *ex, uint64_t cam, VkImageView mv, VkImageView depth, VkFormat depth_fmt, uint32_t w,
                     uint32_t h) {
    static uint64_t frames;
    if (++frames % 60 == 0)
        rt_log("vk_exec: per-object motion: %.2f ms a frame checking, %.2f ms listing (%.1f us a draw), %.2f ms recording",
               MO.ns_check / 1e6 / (frames ? frames : 1), MO.ns_rec / 1e6 / (frames ? frames : 1),
               MO.n_rec ? MO.ns_rec / 1e3 / MO.n_rec : 0.0, MO.ns_emit / 1e6 / (frames ? frames : 1));
    if (frames % 600 == 0) {
        char l[1400];
        int o = 0;
        for (int i = 0; i < 32 && MM[i].n && o < 1300; i++)
            o += snprintf(l + o, sizeof l - o, " %#x:%llu (u%d s%d t%d)", MM[i].mask, (unsigned long long)MM[i].n, MM[i].ubo,
                          MM[i].ssbo, MM[i].tex);
        if (o) rt_log("vk_exec: per-object motion position banks (bit n: cn) and draws:%s", l);
        o = 0;
        for (int i = 0; i < 14 && o < 1300; i++)
            if (MO.lk[i][0] + MO.lk[i][1] + MO.lk[i][2] + MO.lk[i][3])
                o += snprintf(l + o, sizeof l - o, " c%d %llu/%llu/%llu/%llu/%llu", i + 3, (unsigned long long)MO.lk[i][0],
                              (unsigned long long)MO.lk[i][1], (unsigned long long)MO.lk[i][2],
                              (unsigned long long)MO.lk[i][3], (unsigned long long)MO.lk[i][4]);
        if (o) rt_log("vk_exec: per-object motion last-frame lookups (none/self/side/nearer/refused):%s", l);
    }
    if (frames % 60 == 0 && MO.ran)
        rt_log("vk_exec: per-object motion recorded a frame: %.0f draws, %.0f pipeline binds, %.0f dynamic states, %.0f "
               "pushes of %.1f bindings, %.0f set binds, %.0f vertex buffer binds; sets written %.0f, reused %.0f",
               (double)MO.e_draws / MO.ran, (double)MO.e_pipe / MO.ran, (double)MO.e_dyn / MO.ran, (double)MO.e_push / MO.ran,
               MO.e_push ? (double)MO.e_pushw / MO.e_push : 0.0, (double)MO.e_sets / MO.ran, (double)MO.e_vb / MO.ran,
               (double)MO.e_setw / MO.ran, (double)MO.e_setreuse / MO.ran);
    if (frames % 60 == 0)
        rt_log("vk_exec: per-object motion checks: filter %.2f ms (%.0f a frame), data %.2f ms (%.0f), variant and "
               "pipeline %.2f ms (%.0f); %.0f objects looked up; still draws %.2f ms", MO.ns_part[0] / 1e6 / frames,
               (double)MO.n_part[0] / frames,
               MO.ns_part[1] / 1e6 / frames, (double)MO.n_part[1] / frames, MO.ns_part[2] / 1e6 / frames,
               (double)MO.n_part[2] / frames, (double)MO.misses / frames, MO.ns_still / 1e6 / frames);
    if (frames % 60 == 0 && MO.ncyc)
        rt_log("vk_exec: per-object motion lookups: %llu cycles finding last frame's, %llu the copies, %llu comparing, "
               "a lookup; %llu bytes a lookup", (unsigned long long)(MO.cyc[0] / MO.ncyc),
               (unsigned long long)(MO.cyc[1] / MO.ncyc), (unsigned long long)(MO.cyc[2] / MO.ncyc),
               (unsigned long long)(MO.big / MO.ncyc));
    if (frames % 60 == 0)
        rt_log("vk_exec: per-object motion: %llu draws read place or bones, %llu of them still, %llu drawn again, %llu without last frame's data, %llu "
               "waiting for a shader or pipeline; %llu frames ran, %llu dropped (no data: camera %llu, bones %llu, place %llu, "
               "bones copy %llu, place copy %llu; uniform alignment %llu; variant without its output %llu, with storage %llu, "
               "no fragment shader %llu; another camera %llu)",
               (unsigned long long)MO.cand, (unsigned long long)MO.still, (unsigned long long)MO.draws, (unsigned long long)MO.nodata,
               (unsigned long long)MO.building,
               (unsigned long long)MO.ran, (unsigned long long)MO.dropped, (unsigned long long)MO.why[0],
               (unsigned long long)MO.why[1], (unsigned long long)MO.why[2], (unsigned long long)MO.why[3],
               (unsigned long long)MO.why[4], (unsigned long long)LIM.ubo_align, (unsigned long long)MO.no_out,
               (unsigned long long)MO.storage, (unsigned long long)MO.no_fs, (unsigned long long)MO.other_cam);
    if (!MQ.n) return 0;
    static int exec = -1;
    if (exec < 0) exec = rt_knob("MVOBJ_EXEC", "1");   // NX2NAVI_MVOBJ_EXEC=0: listed, never recorded (test)
    if (!exec) {
        mo_drop();
        return 0;
    }
    uint64_t frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (MQ.frame != frame || MQ.fence != ex->fence || MQ.depth_fmt != depth_fmt) {
        mo_drop();
        return 0;
    }
    int fo = 0;
    for (int i = 0; i < 16 && MF[i].n && fo < (int)sizeof fm_last - 40; i++)
        fo += snprintf(fm_last + fo, sizeof fm_last - fo, " %#x/%#x:%u", MF[i].mask, MF[i].chg, MF[i].n);
    if (!fo) fm_last[0] = 0;
    // only draws with the scene camera block (vk_dlaa.c mv_pass's): last frame's camera they get is that block's, and
    // a draw of another view would move by the difference between the two views
    int kept = 0;
    for (int i = 0; i < MQ.n; i++)
        if (MQ.d[i].cam == cam && (!MQ.d[i].pose_index ||
            pose_ranges[MQ.d[i].pose_index - 1].history[frame & 1].view[MQ.d[i].pose_view].selected) &&
            (!MQ.d[i].skin || (MQ.d[i].skin > 0 && SK.resolved))) {   // a skinned draw: one skin_resolve paired
            if (MQ.d[i].pose_index) ++pose_written[pose_ranges[MQ.d[i].pose_index - 1].actor];
            if (MQ.d[i].skin) skin_recorded(MQ.d[i].skin == 2);
            MQ.d[kept++] = MQ.d[i];
        }
        else if (!MQ.d[i].skin) MO.other_cam++;   // a skinned draw left out has its own count (skin_status)
    MQ.n = kept;
    if (!MQ.n) {
        mo_drop();
        return 0;
    }
    uint64_t t0 = kern_now_ns();
    VkCommandBuffer cb = ex->cb;
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = mv,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingAttachmentInfo da = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = depth,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca, .pDepthAttachment = &da};
    vkCmdBeginRendering(cb, &ri);   // through the recorder (vk_rec.h), in order with the draws and the end
    mo_emit(cb);
    vkCmdEndRendering(cb);
    // the draws left their pipeline, descriptors, buffers and dynamic state bound: the next game draw sets everything
    vk_exec_draw_new_cb();
    ex->dirty = VK_EXEC_DIRTY_ALL;
    ex->recorded += MQ.n;
    MQ.n = MQ.nst = MQ.nvb = 0, MQ.ndesc = 0, MQ.frame = 0;
    MO.ran++;
    MO.ns_emit += kern_now_ns() - t0;
    return 1;
}

// NVN2 mesh draws (DrawMeshTasks): count task workgroups, or mesh workgroups for a program without a task stage, through
// VK_EXT_mesh_shader. The stages are the translator's TaskEXT / MeshEXT modules (tools/xlate/MeshSpirv.cs).
// The lazy lookups below store the pointer before the flag that says it was looked up: the chunks of a parallel
// group draw meshes at once, and one that saw the flag first went on with no pointer and skipped its draws.
static PFN_vkCmdDrawMeshTasksEXT draw_mesh_fn(void) {
    static PFN_vkCmdDrawMeshTasksEXT fn;
    static int looked;
    if (!__atomic_load_n(&looked, __ATOMIC_ACQUIRE)) {
        if (vk_ctx()->has_mesh_shader) fn = (PFN_vkCmdDrawMeshTasksEXT)vk_dpa(vk_ctx()->device, "vkCmdDrawMeshTasksEXT");
        __atomic_store_n(&looked, 1, __ATOMIC_RELEASE);
    }
    return fn;
}

static PFN_vkCmdDrawMeshTasksIndirectEXT draw_mesh_ind_fn(void) {
    static PFN_vkCmdDrawMeshTasksIndirectEXT fn;
    static int looked;
    if (!__atomic_load_n(&looked, __ATOMIC_ACQUIRE)) {
        if (vk_ctx()->has_mesh_shader)
            fn = (PFN_vkCmdDrawMeshTasksIndirectEXT)vk_dpa(vk_ctx()->device, "vkCmdDrawMeshTasksIndirectEXT");
        __atomic_store_n(&looked, 1, __ATOMIC_RELEASE);
    }
    return fn;
}

// NX2NAVI_FOLIAGE_SHADOW_VRS (0 to 3, default 2): the foliage's mesh draws in the shadow cascades from this one on shade one
// fragment per 2x2 pixels; 3 turns it off. A leaf's alpha test then runs a quarter as often, and the far cascade's texels
// are metres across the forest floor, where the coarser cut-out does not show. A mesh pipeline has the rate as dynamic
// state, so every mesh draw sets it.
static void mesh_shading_rate(VkExec *ex) {
    static PFN_vkCmdSetFragmentShadingRateKHR fn;
    static int looked, from;
    if (!__atomic_load_n(&looked, __ATOMIC_ACQUIRE)) {   // as draw_mesh_fn: the pointer before the flag
        const char *e = cfg_get("NX2NAVI_FOLIAGE_SHADOW_VRS");
        from = e && *e ? atoi(e) : 2;
        if (vk_ctx()->has_vrs)
            fn = (PFN_vkCmdSetFragmentShadingRateKHR)vkGetDeviceProcAddr(vk_ctx()->device, "vkCmdSetFragmentShadingRateKHR");
        __atomic_store_n(&looked, 1, __ATOMIC_RELEASE);
    }
    if (!fn) return;
    rec_shading_rate(fn, ex->cb, !ex->color[0].image && ex->depth.image && (int)ex->depth.base_layer >= from ? 2 : 1);
    ex->recorded++;
}

// ---- mesh fallback: a mesh draw on a device without VK_EXT_mesh_shader (mesh_fallback, vk_device.c), or on any
// device with mesh_fallback = 1. The program's task and mesh stages run as compute dispatches that write the
// fallback's buffers, and an ordinary draw reads them (mesh_fb.c makes the modules, vk_mesh_fb.c holds the buffers):
//   the draw's record        its workgroup count, copied on the GPU when the game's own record holds it
//   [task dispatch           the task module, with the task stage's bindings
//    scan                    lists the mesh workgroups the tasks started and counts them into the record]
//   mesh dispatch            the mesh module, with the mesh stage's bindings
//   draw                     one instance per mesh workgroup, with the generated vertex stage and the program's
//                            fragment stage, the instance count from the record
// Every step reads what the one before wrote, so a full barrier stands between them, and the dispatches cannot be
// recorded inside a pass: each such draw ends the open pass, and the next draw begins it again. The buffers are
// reused by every draw (vk_mesh_fb.c), which the barrier before a draw's first dispatch makes safe: the previous
// draw's vertex stage has read them by then.
//
// The modules of a program are made once, on a thread of their own, with their compute pipelines (a mesh program of
// the game is up to 900,000 SPIR-V words, and its pipeline compiles for as long as the mesh pipeline it replaces):
// until they are ready the program's draws are skipped, like draws whose pipeline is being built. Without background
// pipeline builds (the probes) they are made at the draw.
typedef struct MfbCs {
    const GfxShader *ms, *ts;              // the key: the translated mesh and task shaders (ts NULL: none)
    MeshFbInfo mi, ti;
    volatile int32_t state;                // 0 queued or being made, 1 ready, -1 failed
    const GfxShader *cs_mesh, *cs_task;    // the compute modules (gfx_shader_own)
    struct MfbCs *next;                    // the worker's queue
} MfbCs;
typedef struct { const GfxShader *ms; uint32_t mask; uint8_t type[32]; const GfxShader *vs; } MfbVs;
#define MFB_TAB 4096
static MfbCs *mfb_cs_tab[MFB_TAB];
static MfbVs mfb_vs_tab[MFB_TAB];
static struct {
    PlatMutex lock;
    PlatCond wake;
    int threads, busy;                     // workers started, workers making a program now
    MfbCs *head, *tail;
    uint64_t made, failed, pending_skips;
    uint64_t first_ms, drained;            // the first program's queueing; the queue has emptied once (logged)
} MFB = {.lock = PLAT_MUTEX_INIT};

static uint32_t mfb_binding_base(const GfxShader *s) {   // one past the highest binding the module declares
    const uint32_t *b;
    uint32_t n = gfx_shader_bindings(s, &b), top = 0;
    for (uint32_t i = 0; i < n; i++)
        if (b[i] > top) top = b[i];
    return top + 1;
}

// The compute module of stage (6 task, 7 mesh) of e, as a shader of the runtime's own with the stage's resources and
// the fallback's buffers.
static const GfxShader *mfb_module(const MfbCs *e, int stage, char *err, size_t cap) {
    const GfxShader *src = stage == 6 ? e->ts : e->ms;
    uint64_t n, on = 0;
    const uint32_t *w = gfx_shader_spirv_pin(src, &n);
    uint32_t base = mfb_binding_base(src);
    uint32_t *out = stage == 6 ? meshfb_task_cs(w, n, &e->ti, base, &on, err, cap)
                               : meshfb_mesh_cs(w, n, &e->mi, base, e->ti.payload_bytes / 4, &on, err, cap);
    gfx_shader_spirv_unpin(src);
    if (!out) return NULL;
    static const int task_slots[1] = {MESHFB_CB_TASK}, mesh_slots[3] = {MESHFB_CB_TASK, MESHFB_CB_MESH, MESHFB_CB_LIST};
    char *res = stage == 6 ? meshfb_res_json(gfx_shader_res_json(src), base, task_slots, 1)
              : e->mi.has_task ? meshfb_res_json(gfx_shader_res_json(src), base, mesh_slots, 3)
                               : meshfb_res_json(gfx_shader_res_json(src), base + 1, mesh_slots + 1, 1);
    const GfxShader *s = res ? gfx_shader_own(out, on, res) : NULL;
    if (!s) snprintf(err, cap, "the converted module or its reflection is malformed");
    free(out), free(res);
    return s;
}

static void mfb_make(MfbCs *e) {
    char err[200] = "";
    const VkPipeInfo *info;
    int ok = (e->cs_mesh = mfb_module(e, 7, err, sizeof err)) != NULL;
    if (ok && e->ts) ok = (e->cs_task = mfb_module(e, 6, err, sizeof err)) != NULL;
    if (ok && !vk_pipeline_compute(e->cs_mesh, &info)) ok = 0, snprintf(err, sizeof err, "the mesh stage's compute pipeline failed");
    if (ok && e->ts && !vk_pipeline_compute(e->cs_task, &info)) ok = 0, snprintf(err, sizeof err, "the task stage's compute pipeline failed");
    plat_mutex_lock(&MFB.lock);
    uint64_t nth = ok ? ++MFB.made : ++MFB.failed;
    plat_mutex_unlock(&MFB.lock);
    if (!ok && (nth <= 8 || !(nth & (nth - 1))))
        rt_log("vk: mesh fallback: a program's modules could not be made (%s): its draws are skipped (%llu such programs)", err,
               (unsigned long long)nth);
    __atomic_store_n(&e->state, ok ? 1 : -1, __ATOMIC_RELEASE);
}

// The programs queued at their first draws are made by NX2NAVI_MESH_FALLBACK_WORKERS threads (auto: one for each 4
// logical processors, 1 to 4). A place's mesh programs all arrive in its first frames and each one's draws are left
// out until its compute pipelines are built (up to seconds each on some drivers), so with one thread the foliage of a
// first visit filled in one program at a time. The first time the queue empties the log says how long it took.
static int mfb_worker_count(void) {
    const char *v = cfg_value("NX2NAVI_MESH_FALLBACK_WORKERS");
    int n = strcmp(v, "auto") ? atoi(v) : (int)plat_cpu_usable() / 4;
    return n < 1 ? 1 : n > 4 ? 4 : n;
}

static void mfb_worker(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);   // as the pipeline workers' optimised builds: the game's threads go first
    for (;;) {
        plat_mutex_lock(&MFB.lock);
        while (!MFB.head) plat_cond_wait(&MFB.wake, &MFB.lock);
        MfbCs *e = MFB.head;
        if (!(MFB.head = e->next)) MFB.tail = NULL;
        MFB.busy++;
        plat_mutex_unlock(&MFB.lock);
        mfb_make(e);
        plat_mutex_lock(&MFB.lock);
        if (!--MFB.busy && !MFB.head && !MFB.drained++)
            rt_log("vk: mesh fallback: %llu programs made (%llu refused) on %d threads, the queue empty %llu ms after the "
                   "first program was queued (NX2NAVI_MESH_FALLBACK_WORKERS)", (unsigned long long)MFB.made,
                   (unsigned long long)MFB.failed, MFB.threads, (unsigned long long)(plat_now_ms() - MFB.first_ms));
        plat_mutex_unlock(&MFB.lock);
    }
}

// The entry of (ms, ts), made and queued at its first draw; NULL (logged) when the program's layout is not carried.
static MfbCs *mfb_cs_get(VkExec *ex, const GfxShader *ms, const GfxShader *ts) {
    uint32_t i = (uint32_t)((((uintptr_t)ms >> 4) * 0x9e3779b1u) ^ (((uintptr_t)ts >> 4) * 0x85ebca6bu)) & (MFB_TAB - 1);
    for (uint32_t k = 0; k < MFB_TAB; k++, i = (i + 1) & (MFB_TAB - 1)) {
        MfbCs *e = mfb_cs_tab[i];
        if (e && (e->ms != ms || e->ts != ts)) continue;
        if (e) return e;
        const uint8_t *ctl, *code;
        uint32_t len;
        char err[160] = "";
        e = calloc(1, sizeof *e);
        if (!e) rt_fatal("vk: out of memory");
        e->ms = ms, e->ts = ts, e->state = -1;
        mfb_cs_tab[i] = e;   // a program that is refused is not looked at again
        int ok = find_stage_scan(ex->prog, NVN_STAGE_MESH, &ctl, &code, &len) && len >= 0x80 &&
                 meshfb_info(ctl, code, 7, &e->mi, err, sizeof err);
        if (ok && ts) ok = find_stage_scan(ex->prog, NVN_STAGE_TASK, &ctl, &code, &len) && meshfb_info(ctl, code, 6, &e->ti, err, sizeof err);
        if (ok && e->mi.has_task != (ts != NULL)) ok = 0, snprintf(err, sizeof err, "the mesh stage %s a task stage and the program has %s",
                                                                   e->mi.has_task ? "expects" : "does not expect", ts ? "one" : "none");
        if (!ok) {
            warn_once(W_MESH, "mesh fallback: a program is not carried (%s): its draws are skipped", *err ? err : "no stage data");
            return e;
        }
        e->state = 0;
        if (!vk_pipeline_async_on()) {
            mfb_make(e);
            return e;
        }
        plat_mutex_lock(&MFB.lock);
        if (!MFB.threads) {
            plat_cond_init(&MFB.wake);
            MFB.first_ms = plat_now_ms();
            for (int w = mfb_worker_count(); w > 0; w--) {
                PlatThread *t = plat_thread_new(mfb_worker, NULL, "mesh fallback");
                if (t) plat_thread_detach(t), MFB.threads++;
            }
            if (!MFB.threads) rt_fatal("vk: mesh fallback: cannot start a worker thread");
        }
        if (MFB.tail) MFB.tail->next = e;
        else MFB.head = e;
        MFB.tail = e;
        plat_cond_signal(&MFB.wake);
        plat_mutex_unlock(&MFB.lock);
        return e;
    }
    rt_fatal("vk: mesh fallback: more than %d task and mesh program pairs", MFB_TAB);
}

// The vertex stage that draws e's mesh buffer slots for fragment shader fs: an output for every input fs reads, in
// its type.
static const GfxShader *mfb_vs_get(const MfbCs *e, const GfxShader *fs) {
    GfxShaderIo fin[32];
    uint8_t type[32] = {0};
    uint32_t mask = fs ? gfx_shader_inputs(fs, fin) : 0;
    for (int g = 0; g < 32; g++)
        if (mask >> g & 1) type[g] = fin[g].type;
    uint32_t i = (uint32_t)((((uintptr_t)e->ms >> 4) * 0x9e3779b1u) ^ (mask * 0x85ebca6bu)) & (MFB_TAB - 1);
    for (uint32_t k = 0; k < MFB_TAB; k++, i = (i + 1) & (MFB_TAB - 1)) {
        MfbVs *v = &mfb_vs_tab[i];
        if (v->ms && (v->ms != e->ms || v->mask != mask || memcmp(v->type, type, 32))) continue;
        if (v->ms) return v->vs;
        static const char res[] = "{\"key\":\"meshfb\",\"stage\":\"vs\",\"notes\":[],\"cbufs\":[],\"sbufs\":[{\"binding\": 1, "
                                  "\"slot\": -1, \"cb_slot\": 66, \"cb_offset\": 0, \"flags\": 0}],\"textures\":[],\"images\":[]}";
        _Static_assert(MESHFB_CB_MESH == 66, "the vertex stage's reflection names the mesh buffer");
        uint64_t n = 0;
        uint32_t *w = meshfb_vs(&e->mi, mask, type, &n);
        v->ms = e->ms, v->mask = mask, memcpy(v->type, type, 32);
        v->vs = w ? gfx_shader_own(w, n, res) : NULL;
        free(w);
        if (!v->vs) warn_once(W_MESH, "mesh fallback: a vertex stage could not be made: its draws are skipped");
        return v->vs;
    }
    rt_fatal("vk: mesh fallback: more than %d vertex stages", MFB_TAB);
}

// One of the draw's compute dispatches: module cs with the bindings of NVN stage `stage`, its group counts read from
// the record at `at`. 0 when its pipeline is not there.
static int mfb_dispatch(VkExec *ex, const GfxShader *cs, int stage, VkBuffer rec, VkDeviceSize at) {
    const VkPipeInfo *info = NULL;
    VkPipeline p = vk_pipeline_compute(cs, &info);
    if (!p || !info || (info->pool && !pool_on())) return 0;
    bind_pipeline(ex, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    ex->dc->mfb_stage = stage;
    bind_resources(ex, info);
    ex->dc->mfb_stage = 0;
    VkPassMark *m = dispatch_mark(ex, cs, 0, 0, 0);
    vkCmdDispatchIndirect(ex->cb, rec, at);
    vk_pass_mark_end(ex->cb, m);
    ex->recorded++;
    return 1;
}

// A mesh draw of `count` workgroups, or of the count in the game's record at ind (4 bytes, GPU-written).
static void mfb_draw(VkExec *ex, uint32_t count, const VkMemSpan *ind) {
    DrawCtx *const cx = ex->dc;
    if (ex->render_disabled) {
        vk_cond_draws_skip += ex->has_render_cond;
        draw_skipped(ex, 1, SK_DISABLED);
        return;
    }
    if (!ex->prog) { draw_skipped(ex, 1, SK_NO_PROGRAM); return; }
    cb_res(ex);
    dummies_init();
    int has_ms, has_ts, has_fs, w_ms = 0, w_ts = 0, w_fs = 0;
    const GfxShader *ms = shader_for(ex, NVN_STAGE_MESH, &has_ms, &w_ms, 0);
    const GfxShader *ts = shader_for(ex, NVN_STAGE_TASK, &has_ts, &w_ts, 0);
    const GfxShader *fs = shader_for(ex, NVN_STAGE_FS, &has_fs, &w_fs, 0);
    if (!has_ms) { draw_skipped(ex, 1, SK_NO_VERTEX_STAGE); return; }
    if (w_ms || w_ts || w_fs) {
        vk_exec_ahead(ex);
        perf_count(PERF_CNT_SHADER_SKIP);
        draw_skipped(ex, 1, SK_SHADER_PENDING);
        return;
    }
    if (!ms || (has_ts && !ts) || (has_fs && !fs)) { draw_skipped(ex, 1, SK_SHADER_FAILED); return; }
    MfbCs *c = mfb_cs_get(ex, ms, ts);
    int state = __atomic_load_n(&c->state, __ATOMIC_ACQUIRE);
    if (!state && !vk_exec_targets_recent(ex, pipe_skip_mode() >= 1)) {
        // a target the game does not render every frame: the draw may not come again, so it waits for its modules
        // from the frame's wait pool, as such a draw waits for a pipeline (prepare_draw_ex)
        uint64_t *pool = frame_wait(), t0 = kern_now_ns();
        while (!state && pool_ms(pool, t0)) plat_sleep_ms(1), state = __atomic_load_n(&c->state, __ATOMIC_ACQUIRE);
        pool_charge(pool, t0);
    }
    if (!state) { MFB.pending_skips++; draw_skipped(ex, 1, SK_PIPE_PENDING); return; }
    const GfxShader *vs = state > 0 ? mfb_vs_get(c, fs) : NULL;
    if (!vs || !vk_meshfb_init()) { draw_skipped(ex, 1, SK_PIPE_FAILED); return; }
    uint32_t verts = c->mi.maxp * c->mi.ipp, slot_words = meshfb_slot_words(&c->mi);
    uint32_t most = vk_ctx()->props.limits.maxComputeWorkGroupCount[0];
    if (count > most) count = most;   // a count the GPU wrote is not looked at here: the dispatch reads it as it is
    VkBuffer rec;
    int cmds;
    vk_exec_barrier(ex);   // ends the pass; the culling's record and the last draw's reads come before what follows
    VkDeviceSize at = vk_meshfb_record(ex->cb, ind ? ind->buffer : VK_NULL_HANDLE, ind ? ind->offset : 0, count, ts != NULL,
                                       verts, slot_words, &rec, &cmds);
    ex->recorded += (uint32_t)cmds;
    vk_exec_barrier(ex);
    if (ts) {
        if (!mfb_dispatch(ex, c->cs_task, NVN_STAGE_TASK, rec, at)) { draw_skipped(ex, 1, SK_PIPE_FAILED); return; }
        vk_exec_barrier(ex);
        vk_meshfb_scan(ex->cb, at, c->ti.payload_bytes / 4, c->ti.threads, slot_words, verts);
        cx->B.pipe[1] = VK_NULL_HANDLE;   // the scan bound its own
        ex->recorded += 4;
        vk_exec_barrier(ex);
    }
    if (!mfb_dispatch(ex, c->cs_mesh, NVN_STAGE_MESH, rec, at + 16)) { draw_skipped(ex, 1, SK_PIPE_FAILED); return; }
    vk_exec_barrier(ex);
    vk_meshfb_note(ex->cb, at);
    ex->recorded++;
    vk_exec_barrier(ex);
    cx->c0_binst = 0;
    cx->mfb_cur_vs = vs;
    int prim = c->mi.ipp == 1 ? NVN_DRAW_PRIMITIVE_POINTS : c->mi.ipp == 2 ? NVN_DRAW_PRIMITIVE_LINES : NVN_DRAW_PRIMITIVE_TRIANGLES;
    if (!prepare_draw_ex(ex, prim, 0x7fffffff, 0, 2)) return;
    vk_exec_frame.draws++;
    vkCmdDrawIndirect(ex->cb, rec, at + 32, 1, sizeof(VkDrawIndirectCommand));
    ex->recorded++;
}

static int mesh_emu_draw(VkExec *ex, uint32_t count, const VkMemSpan *ind, const VkMemSpan *args);
// The primitive an emulated draw's mesh stage outputs: point meshes draw point lists, the rest triangles.
static int mesh_emu_draw_prim(VkExec *ex) {
    static struct { const GfxShader *s; int prim; } memo[256];
    int present = 0, pending = 0;
    const GfxShader *ms = ex->prog ? shader_for(ex, NVN_STAGE_MESH, &present, &pending, 0) : NULL;
    if (!ms) return NVN_DRAW_PRIMITIVE_TRIANGLES;
    unsigned h = (unsigned)((uintptr_t)ms >> 4) & 255;
    if (memo[h].s != ms) {
        uint64_t n = 0;
        const uint32_t *w = gfx_shader_spirv_pin(ms, &n);   // once per mesh shader the memo meets
        memo[h].s = ms, memo[h].prim = w && mesh_emu_prim(w, (size_t)n) == 1 ? NVN_DRAW_PRIMITIVE_POINTS : NVN_DRAW_PRIMITIVE_TRIANGLES;
        gfx_shader_spirv_unpin(ms);
    }
    return memo[h].prim;
}
static int mesh_emu_on(void) {
    static int on = -1;
    if (on < 0) {
        on = !vk_ctx()->has_mesh_shader && rt_knob("MESH_EMULATION", "0");
        if (on) rt_log("vk: mesh shaders emulated with compute (NX2NAVI_MESH_EMULATION)");
    }
    return on;
}

static void op_draw_mesh(VkExec *ex, const p_draw_mesh *d) {
    PFN_vkCmdDrawMeshTasksEXT fn = draw_mesh_fn();
    if (!fn && mesh_emu_on()) {
        if (d->first) warn_once(W_MESH, "an emulated mesh draw from workgroup %u: drawn from workgroup 0", d->first);
        if (d->count && rt_knob("MESH", "1")) mesh_emu_draw(ex, d->count, NULL, NULL);
        return;
    }
    static int on = -1;
    if (on < 0) on = rt_knob("MESH", "1");   // NX2NAVI_MESH=0: mesh draws are skipped (diagnosis)
    if (!d->count || !on) return;
    if (vk_ctx()->mesh_fallback) {
        if (d->first) warn_once(W_MESH, "a mesh draw from workgroup %u: drawn from workgroup 0", d->first);
        mfb_draw(ex, d->count, NULL);
        return;
    }
    if (!fn) return warn_once(W_MESH, "mesh draws need VK_EXT_mesh_shader (mesh_emulation = 1 emulates them): skipped");
    ex->dc->c0_binst = 0;
    if (!prepare_draw_ex(ex, NVN_DRAW_PRIMITIVE_TRIANGLES, 0x7fffffff, 0, 1)) return;
    if (d->first) warn_once(W_MESH, "a mesh draw from workgroup %u: drawn from workgroup 0", d->first);
    vk_exec_frame.draws++;
    static uint32_t n_logged;
    if (n_logged < 8 && knob_log_mesh()) {
        n_logged++;
        rt_log("vk: mesh draw: %u workgroups, program %llu", d->count, (unsigned long long)ex->prog->serial);
    }
    mesh_shading_rate(ex);
    int q = vk_dump_armed ? vk_dump_mesh_begin(ex) : -1;
    rec_draw_mesh(fn, ex->cb, d->count, 1, 1);
    ex->recorded++;
    vk_dump_mesh_end(ex, q);
}

// The record NVN's DrawMeshTasksIndirect reads is {count, first} (as NV_mesh_shader's), and Vulkan's {x, y, z}: read as
// it is, y = first (0) drew nothing. Each draw gets an entry of this ring, {count, 1, 1}: y and z filled with 1 and x
// copied from the game's record on the GPU, between the pass's end and the draw (the record is written by the culling
// compute earlier in the frame and never comes back to guest memory). `first` is not applied: the game leaves it 0.
// MESH_RING entries outlast the frames in flight: an entry is rewritten only after a barrier behind its last draw.
enum { MESH_RING = 4096 };
static VkBuffer mesh_ring;
static VmaAllocation mesh_ring_alloc;
static uint32_t mesh_ring_next;

// NX2NAVI_GPU_PASSES: the workgroup counts of the indirect mesh draws of the timed frames, copied to host memory.
int vk_gpu_passes_capturing(void);
enum { MESH_COUNTS = 4096 };
static VkBuffer mesh_count_buf;
static VmaAllocation mesh_count_alloc;
static uint32_t *mesh_count_map, mesh_count_n, mesh_count_was;

static void mesh_ring_release(VkCtx *c) {   // vk_exec_draw_shutdown: a probe ends with the device idle
    if (mesh_ring) vmaDestroyBuffer(c->vma, mesh_ring, mesh_ring_alloc);
    if (mesh_count_buf) vmaDestroyBuffer(c->vma, mesh_count_buf, mesh_count_alloc);
    mesh_ring = mesh_count_buf = VK_NULL_HANDLE, mesh_count_map = NULL;
}
uint32_t vk_exec_mesh_counts(const uint32_t **out) {
    *out = mesh_count_map;
    return mesh_count_map ? mesh_count_n : 0;
}
static void mesh_count_note(VkExec *ex, VkCommandBuffer cb, const VkMemSpan *src) {
    uint32_t cap = (uint32_t)vk_gpu_passes_capturing();
    if (!cap) { mesh_count_was = 0; return; }
    if (!mesh_count_was) mesh_count_was = 1, mesh_count_n = 0;
    if (!mesh_count_buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4ull * MESH_COUNTS,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &mesh_count_buf, &mesh_count_alloc, &info) != VK_SUCCESS) return;
        mesh_count_map = info.pMappedData;
    }
    if (mesh_count_n >= MESH_COUNTS) return;
    VkBufferCopy cp = {src->offset, 4ull * mesh_count_n++, 4};
    vkCmdCopyBuffer(cb, src->buffer, mesh_count_buf, 1, &cp);
    if (cb == ex->cb) ex->recorded++;
}

// NX2NAVI_MESH_AHEAD (default 1; 0 = each draw by itself, "ab" switches with the A/B blocks): the copy of a record
// cannot be recorded inside a pass, so every indirect mesh draw ended its pass, and a pass that draws its meshes one
// after another was cut into as many pieces (a village: 45 of them a frame, each with a gap of its own on the GPU).
// The first such draw of a run now fills the entries of the ones that follow it as well, with nothing but state,
// draws and counter reports between them (vk_exec.c op_linear, vk_exec_mesh_run): those draws then find their entry
// and stay in the pass. The culling compute that writes the records cannot run inside that run, and an entry is taken
// only by the draw it was filled for, at the same record address, while no other op was replayed since
// (vk_exec_brk). In the village 42 of the 45 draws a frame are served that way: 3 gaps with these copies a frame
// where there were 41 (0.04 ms of GPU time for 0.22), and about 40 passes fewer to begin and end.
// The entries filled ahead are the draw context's (DrawCtx.ma): a chunk of a parallel group (vk_exec.c) fills them for
// the draws that follow its own (vk_exec_mesh_run reads on from the op it records), and an entry it filled for a draw
// another chunk records is left unused.
enum { MESH_AHEAD = 64 };
_Static_assert(MESH_AHEAD == sizeof dc_main.ma / sizeof dc_main.ma[0], "DrawCtx.ma holds MESH_AHEAD entries");
static uint64_t mesh_ahead_taken, mesh_ahead_own;   // since the start: draws served by an entry filled ahead; the others

// The ring, made before the chunks of a group record (vk_exec_draw_group_begin) or at the first indirect mesh draw.
static int mesh_ring_make(void) {
    if (mesh_ring) return 1;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 12ull * MESH_RING,
                             .usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkBuffer b;
    if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &b, &mesh_ring_alloc, NULL) != VK_SUCCESS) return 0;
    __atomic_store_n(&mesh_ring, b, __ATOMIC_RELEASE);
    return 1;
}

static int mesh_ahead_on(void) {
    static int mode = -1;
    if (mode < 0) mode = rt_knob("MESH_AHEAD", "1");
    return RT_KNOB_ON(mode);
}

// A ring entry {count from src, 1, 1}, filled in cb; ex->cb: after a full barrier, which ends the pass (the next draw
// opens it again) and orders the culling's writes before the copy. Another cb (the pre-pass one of the mesh
// emulation) begins with one. d: the draw it is for, which may take an entry filled ahead (MESH_AHEAD, ex->cb only);
// NULL: none (the mesh emulation's).
static int mesh_ring_entry(VkExec *ex, VkCommandBuffer cb, const p_draw_mesh_ind *d, const VkMemSpan *src, VkDeviceSize *at) {
    DrawCtx *const cx = ex->dc;
    if ((rt_trace & RT_TRACE_PERF) && (mesh_ahead_taken + mesh_ahead_own) % 20000 == 19999) {
        char stops[160];
        vk_exec_mesh_run_stops(stops, sizeof stops);
        rt_log("perf: indirect mesh draws: %llu so far, %llu of them with their record filled ahead by an earlier draw of "
               "their pass; the runs ended: %s", (unsigned long long)(mesh_ahead_taken + mesh_ahead_own + 1),
               (unsigned long long)mesh_ahead_taken, stops);
    }
    if (d && cb == ex->cb && cx->ma_i < cx->ma_n) {
        if (cx->ma[cx->ma_i].d == d && cx->ma[cx->ma_i].ind == d->ind && cx->ma_brk == vk_exec_brk_now()) {
            *at = cx->ma[cx->ma_i++].at;
            __atomic_add_fetch(&mesh_ahead_taken, 1, __ATOMIC_RELAXED);
            return 1;
        }
        cx->ma_n = cx->ma_i = 0;   // the walk went another way: the entries left are never drawn from
    }
    __atomic_add_fetch(&mesh_ahead_own, 1, __ATOMIC_RELAXED);
    if (!__atomic_load_n(&mesh_ring, __ATOMIC_ACQUIRE) && (vk_exec_in_chunk() || !mesh_ring_make())) return 0;
    *at = 12ull * (__atomic_fetch_add(&mesh_ring_next, 1, __ATOMIC_RELAXED) % MESH_RING);
    int own = cb == ex->cb;
    if (own) {
        vk_exec_barrier(ex);
        cb = ex->cb;
        vk_gpu_gap_note(VK_GAP_MESH_RING, 12);
    }
    vkCmdFillBuffer(cb, mesh_ring, *at + 4, 8, 1);
    VkBufferCopy cp = {src->offset, *at, 4};
    vkCmdCopyBuffer(cb, src->buffer, mesh_ring, 1, &cp);
    mesh_count_note(ex, cb, src);
    cx->ma_n = cx->ma_i = 0;
    if (d && own && mesh_ahead_on()) {
        const void *run[MESH_AHEAD];
        int n = vk_exec_mesh_run(d, run, MESH_AHEAD);
        for (int i = 0; i < n; i++) {
            const p_draw_mesh_ind *f = run[i];
            VkMemSpan ip;
            if (!vk_mem_resolve(f->ind, 12, &ip) || ip.offset % 4) break;   // that draw reports it when the walk gets there
            VK_MEM_USE(ip);
            VkDeviceSize fa = 12ull * (__atomic_fetch_add(&mesh_ring_next, 1, __ATOMIC_RELAXED) % MESH_RING);
            vk_gpu_gap_note(VK_GAP_MESH_RING, 12);
            vkCmdFillBuffer(ex->cb, mesh_ring, fa + 4, 8, 1);
            VkBufferCopy fc = {ip.offset, fa, 4};
            vkCmdCopyBuffer(ex->cb, ip.buffer, mesh_ring, 1, &fc);
            mesh_count_note(ex, ex->cb, &ip);
            ex->recorded += 2;
            cx->ma[cx->ma_n].d = f, cx->ma[cx->ma_n].ind = f->ind, cx->ma[cx->ma_n].at = fa;
            cx->ma_n++;
        }
        cx->ma_brk = vk_exec_brk_now();
    }
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                           .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
    if (own) ex->recorded += 3;
    return 1;
}

// NX2NAVI_FOLIAGE (full, reduced, off; default full; the low-end GPU rows of the menu's Game tab): Ocarina draws its foliage with
// DrawMeshTasksIndirect, the instances its compute passes culled on the GPU. With a task stage: the small grass and moss
// that carpet the ground; without one: ferns, leaves, clover and the other plants. Its other mesh draws (DrawMeshTasks
// with a count: terrain, rocks, buildings, doors) are not foliage. reduced skips the indirect draws with a task stage,
// off every indirect one, in every pass (their shadows too). In a frame dump of Kokiri Forest on an RX 580
// (f8118, 2026-11-10) the grass and moss were one draw with task stage 2ddf7b0bf74e5a05; the plants nine without.
static int foliage_skip(VkExec *ex) {
    static int mode = -1;
    if (mode < 0) {
        const char *v = cfg_get("NX2NAVI_FOLIAGE");
        mode = v && !strcmp(v, "off") ? 2 : v && !strcmp(v, "reduced") ? 1 : 0;
        rt_log("vk: foliage %s (NX2NAVI_FOLIAGE)", mode == 2 ? "off" : mode ? "reduced: no small grass or moss" : "full");
    }
    if (!mode) return 0;
    if (mode == 2) return 1;
    const uint8_t *ctl, *code;
    uint32_t len;
    return ex->prog && find_stage(ex->dc, ex->prog, NVN_STAGE_TASK, &ctl, &code, &len);
}

// DrawMeshTasksIndirect: one draw from its ring entry (mesh_ring_entry); x2-x4 of the call vary like leftovers, so they
// are not read as a count or stride.
static void op_draw_mesh_ind(VkExec *ex, const p_draw_mesh_ind *d) {
    PFN_vkCmdDrawMeshTasksIndirectEXT fn = draw_mesh_ind_fn();
    int emu = !fn && mesh_emu_on(), fallback = !emu && vk_ctx()->mesh_fallback;
    if (!fn && !emu && !fallback)
        return warn_once(W_MESH, "mesh draws need VK_EXT_mesh_shader (mesh_emulation = 1 emulates them): skipped");
    static int on = -1;
    if (on < 0) on = rt_knob("MESH", "1");
    if (!on || foliage_skip(ex)) return;
    VkMemSpan ip;
    if (!vk_mem_resolve(d->ind, 12, &ip) || ip.offset % 4)
        return warn_once(W_INDIRECT, "indirect mesh draw arguments at %#llx are not backed or not aligned: skipped",
                         (unsigned long long)d->ind);
    ex->dc->c0_binst = 0;
    VK_MEM_USE(ip);
    if (emu) {   // emulated: a ring entry {count, 1, 1}, made where the stages run, is the first stage's dispatch
        mesh_emu_draw(ex, 0, NULL, &ip);
        vk_mem_count(ip.cls, VK_MUSE_IND, 4);
        VK_MEM_BOUND(ip);
        return;
    }
    if (fallback) {
        mfb_draw(ex, 0, &ip);
        vk_mem_count(ip.cls, VK_MUSE_IND, 4);
        VK_MEM_BOUND(ip);
        return;
    }
    VkDeviceSize at;
    if (!mesh_ring_entry(ex, ex->cb, d, &ip, &at)) return warn_once(W_MESH, "the indirect mesh ring could not be created: skipped");
    if (!prepare_draw_ex(ex, NVN_DRAW_PRIMITIVE_TRIANGLES, 0x7fffffff, 0, 1)) return;
    vk_exec_frame.draws++;
    static uint32_t n_logged;
    if (n_logged < 8 && knob_log_mesh()) {
        n_logged++;
        rt_log("vk: indirect mesh draw: arguments at %#llx, program %llu", (unsigned long long)d->ind,
               (unsigned long long)ex->prog->serial);
    }
    mesh_shading_rate(ex);
    int q = vk_dump_armed ? vk_dump_mesh_begin(ex) : -1;
    rec_draw_mesh_ind(fn, ex->cb, mesh_ring, at, 1, 12);
    ex->recorded++;
    vk_dump_mesh_end(ex, q);
    vk_mem_count(ip.cls, VK_MUSE_IND, 4);
    VK_MEM_BOUND(ip);
}

// ---- NX2NAVI_MESH_EMULATION: mesh draws on GPUs without VK_EXT_mesh_shader
// Each mesh draw (prepared like one, prepare_draw_ex, for its layout, descriptors and graphics state) runs its task
// and mesh stages as compute pipelines (vk_pipeline_mesh_emu) outside the pass: they write vertices and triangles into
// the arena (mesh_emu_spirv.h) and the draw record's DrawIndexedIndirect, which the generated vertex shader and the
// game's fragment shader then draw. The arena's counters are zeroed at the first emulated draw of each submission;
// the draw records go round a ring of MESH_EMU_DRECS. NX2NAVI_MESH_EMU_MB sizes the arena (default 256 MiB),
// NX2NAVI_MESH_EMU_TASKS its task records per submission (default 16384, 3 KiB each). Before the counters start over
// they are copied to a readback ring; the perf log reports the most a submission asked for against the capacities.
#define ME_RB 64
static struct {
    VkBuffer buf, rb;
    VmaAllocation alloc, rb_alloc;
    uint32_t *rb_map;
    uint64_t addr, resets;
    uint32_t iw, icap, vw, vcap, trecs, epoch, rec;
    int failed;
    uint64_t draws, skipped;
    uint32_t max[4];
    VkPipelineLayout alayout;   // the allocation shader's (push constants only)
    VkPipeline apipe;
} ME;

// The allocation shader's pipeline (mesh_emu_spirv.h mesh_emu_alloc_spirv); 0 when it cannot be made.
static int me_alloc_pipe(void) {
    VkDevice dev = vk_ctx()->device;
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &pcr};
    if (vkCreatePipelineLayout(dev, &li, NULL, &ME.alayout) != VK_SUCCESS) return ME.alayout = VK_NULL_HANDLE, 0;
    size_t n = 0;
    const uint32_t *w = mesh_emu_alloc_spirv(&n);
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n * 4, .pCode = w};
    VkShaderModule m;
    if (vkCreateShaderModule(dev, &mi, NULL, &m) != VK_SUCCESS) return 0;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = ME.alayout};
    VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &ME.apipe);
    vkDestroyShaderModule(dev, m, NULL);
    if (r != VK_SUCCESS) return ME.apipe = VK_NULL_HANDLE, 0;
    return 1;
}

static int me_arena(void) {
    if (ME.buf) return 1;
    if (ME.failed) return 0;
    const char *v = cfg_get("NX2NAVI_MESH_EMU_MB"), *t = cfg_get("NX2NAVI_MESH_EMU_TASKS");
    int mb = v && atoi(v) >= 32 ? atoi(v) : 256;
    if (mb > 2048) mb = 2048;
    ME.trecs = t && atoi(t) >= 256 ? (uint32_t)atoi(t) : 16384;
    if (ME.trecs > 262144) ME.trecs = 262144;
    uint64_t tbytes = (uint64_t)MESH_EMU_TOFF + (uint64_t)ME.trecs * MESH_EMU_TWORDS * 4;
    if (tbytes + (32ull << 20) > ((uint64_t)mb << 20)) mb = (int)((tbytes >> 20) + 64);   // room for vertices and indices
    VkDeviceSize size = (VkDeviceSize)mb << 20;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!me_alloc_pipe()) {
        ME.failed = 1;
        rt_log("vk: mesh emulation: the allocation shader's pipeline could not be created: mesh draws skipped");
        return 0;
    }
    if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &ME.buf, &ME.alloc, NULL) != VK_SUCCESS) {
        ME.buf = VK_NULL_HANDLE, ME.failed = 1;
        rt_log("vk: mesh emulation: the %d MiB arena could not be created: mesh draws skipped", mb);
        return 0;
    }
    VkBufferDeviceAddressInfo dai = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = ME.buf};
    ME.addr = vkGetBufferDeviceAddress(vk_ctx()->device, &dai);
    uint64_t ioff = (tbytes + 4095) & ~4095ull, ibytes = ((size - ioff) / 4) & ~4095ull;
    uint64_t voff = ioff + ibytes;
    ME.iw = (uint32_t)(ioff / 4), ME.icap = (uint32_t)(ibytes / 4), ME.vw = (uint32_t)(voff / 16), ME.vcap = (uint32_t)((size - voff) / 16);
    rt_log("vk: mesh emulation arena: %d MiB, %u task records, %u indices, %u vertex slots (16 bytes)", mb, ME.trecs,
           ME.icap, ME.vcap);
    VkBufferCreateInfo rbi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = ME_RB * 16,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo rai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                   .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo rinfo;
    if (vmaCreateBuffer(vk_ctx()->vma, &rbi, &rai, &ME.rb, &ME.rb_alloc, &rinfo) == VK_SUCCESS) ME.rb_map = rinfo.pMappedData;
    else ME.rb = VK_NULL_HANDLE;
    return 1;
}

// The access masks also carry the generic MEMORY_WRITE / MEMORY_READ bits. SHADER_STORAGE_READ/WRITE exist only in
// synchronization2 (bits above 31), and AMD's driver for Polaris (23.10.01, RX 580) did not act on them: the mesh
// dispatch that reads the {groups, task workgroups, 1} the task stage had just written read zeros and launched no
// workgroup (mesh.txt: "mesh groups started 0" with the record holding 32 x 12 x 1), so every draw with a task stage
// (Kokiri's moss foliage, Link's door) drew nothing. Mesh draws without a task stage drew, because their record is
// read by the draw after a command buffer boundary. The general replay barrier (vk_exec.c barrier_to) uses those bits.
static void me_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = ss,
                           .srcAccessMask = sa | VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = ds,
                           .dstAccessMask = da | VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

// The stages of the emulated draws whose records were filled, waiting to be recorded (me_flush): the draws of a pass
// with NX2NAVI_MESH_EMU_BATCH, which run in the part's pre-pass command buffer when it ends (vk_exec_mesh_flush, from
// vk_sync.c), or the one draw that runs in the part. Recorded by stage, with one barrier between stages for all the
// draws: the fills, the allocation (task records; index ranges of draws without a task stage), every task stage, the
// allocation of the other index ranges, every mesh stage. Each draw's indices are a range of its own and its task
// records too (the allocation shader), so the draws' stages need nothing of each other's. One by one, as before
// 2026-11-11, each draw took three barriers, and its task base and first index were copies of the arena's counters
// after the draw before it: an RX 580 spent 5.5 ms of a frame's GPU time on 69 draws' stages (Kokiri Forest),
// most of it waiting at the barriers with the GPU nearly idle.
typedef struct {
    VkPipeline tp, mp;
    VkPipelineLayout layout;
    VkDescriptorUpdateTemplate tpl_c;
    VkDescriptorSet set0, pset;
    int push;
    uint32_t count, rec, pc[8];
    VkBuffer ib;   // the first stage's indirect dispatch (VK_NULL_HANDLE: count)
    VkDeviceSize io;
    size_t doff;
    uint32_t dbytes;
} MeQ;
#define MEQ_MAX 256
static struct {
    MeQ q[MEQ_MAX];
    int n, tasks;
    uint8_t *d;   // the push descriptors' data of the queued draws
    size_t dn, dcap;
} MEQ;

static void me_bind(VkCommandBuffer cb, const MeQ *q) {
    if (q->push) rec_push_tpl(vk_pipeline_push_tpl_fn(), cb, q->tpl_c, q->layout, 0, MEQ.d + q->doff, q->dbytes);
    else vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, q->layout, 0, 1, &q->set0, 0, NULL);
    if (q->pset) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, q->layout, 2, 1, &q->pset, 0, NULL);
    vkCmdPushConstants(cb, q->layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT, 0, MESH_EMU_PC_BYTES, q->pc);
}

static void me_first(VkCommandBuffer cb, const MeQ *q) {
    if (q->ib) vkCmdDispatchIndirect(cb, q->ib, q->io);
    else vkCmdDispatch(cb, q->count, 1, 1);
}

static void me_alloc(VkCommandBuffer cb, uint32_t mode) {
    uint32_t pc[8] = {(uint32_t)ME.addr, (uint32_t)(ME.addr >> 32), MEQ.q[0].rec, (uint32_t)MEQ.n, mode, ME.icap, ME.trecs, 0};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ME.apipe);
    vkCmdPushConstants(cb, ME.alayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, 1, 1, 1);
}

// The queued draws' stages into cb (the pre-pass command buffer, or the part for a draw that runs there); returns the
// commands recorded.
static int me_flush(VkCommandBuffer cb) {
    if (!MEQ.n || !cb) return 0;
    const VkAccessFlags2 rw = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    const VkPipelineStageFlags2 cs = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    int k = 0;
    me_barrier(cb, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, cs | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
               rw | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    me_alloc(cb, 0), k += 4;
    me_barrier(cb, cs, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, cs | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
               rw | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    if (MEQ.tasks) {   // the task stages wrote the mesh dispatch {max mesh groups, task workgroups, 1} into the draw record
        for (int i = 0; i < MEQ.n; i++) {
            const MeQ *q = &MEQ.q[i];
            if (!q->tp) continue;
            me_bind(cb, q);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, q->tp);
            me_first(cb, q), k += 4;
        }
        me_barrier(cb, cs, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, cs | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                   rw | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
        me_alloc(cb, 1), k += 4;
        me_barrier(cb, cs, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, cs | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                   rw | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    }
    for (int i = 0; i < MEQ.n; i++) {
        const MeQ *q = &MEQ.q[i];
        me_bind(cb, q);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, q->mp);
        if (q->tp) vkCmdDispatchIndirect(cb, ME.buf, MESH_EMU_DOFF + (VkDeviceSize)q->rec * MESH_EMU_DWORDS * 4 + 20);
        else me_first(cb, q);
        k += 4;
    }
    me_barrier(cb, cs, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                   cs | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | rw | VK_ACCESS_2_TRANSFER_READ_BIT);
    MEQ.n = MEQ.tasks = 0, MEQ.dn = 0;
    return k + 4;
}

// vk.h: the pre-pass command buffer ends (vk_sync.c pre_end): the stages of the draws queued for it go in first.
void vk_exec_mesh_flush(VkCommandBuffer cb) { me_flush(cb); }

// count: the first stage's workgroups, or ind: a {count, 1, 1} record to dispatch from
// args: an indirect draw's arguments in game memory (count, 1, 1 copied into a ring entry, which ind then is)
static int mesh_emu_draw(VkExec *ex, uint32_t count, const VkMemSpan *ind, const VkMemSpan *args) {
    if (!me_arena()) return 0;
    ex->dc->c0_binst = 0;
    int prim = mesh_emu_draw_prim(ex);
    if (!prepare_draw_ex(ex, prim, 0x7fffffff, 0, 1)) return 0;
    const VkPipeInfo *info = PD_info;
    const GfxShader *ms = ex->dc->B.st.vs, *ts = ex->dc->B.st.ts;
    VkPipeline mp = VK_NULL_HANDLE, tp = VK_NULL_HANDLE;
    MeshEmuInfo mi;
    if (!info || !info->emu || !ex->dc->B.st_valid || !vk_pipeline_mesh_emu(ms, ts, info->layout, &mp, &tp, &mi) || !!ts != !!tp ||
        (info->push && !info->emu_tpl_c) || (!info->push && !ex->dc->B.ds_bound[0])) {
        static int logged;
        if (logged++ < 8)
            rt_log("vk: mesh emulation: a draw of program %llu skipped: %s", ex->prog ? (unsigned long long)ex->prog->serial : 0ull,
                   !info || !info->emu ? "its pipeline is not an emulated one"
                   : !mp ? "its stages are not emulated (see vk_pipeline: mesh emulation)"
                   : "its descriptors cannot be bound for compute");
        ME.skipped++;
        return 0;
    }
    VkPipelineLayout layout = info->layout;
    int push = info->push, pool = info->pool;
    VkDescriptorUpdateTemplate tpl_c = info->emu_tpl_c;
    VkDescriptorSet set0 = ex->dc->B.ds_bound[0];
    // NX2NAVI_MESH_EMU_BATCH (default on): the stages run in the part's pre-pass command buffer (vk_sync_pre), which
    // the queue runs before the part, and the pass is not split for them. At the first emulated draw of a pass the part
    // is cut there (vk_sync_cut: ended, but handed to the queue only with the next part, so the texture pool set stays;
    // a split there took a new set, and each took 2.3 ms of the replay to refresh), so that the pass begins the next
    // part and only what came before it runs before the stages. Split, a
    // pass of 20 emulated draws was 20 passes, each emulated draw a pass end and begin with a full barrier, three more
    // barriers and the targets stored and loaded again: on an RX 580 6 to 7 ms of a frame's GPU time went to the gaps
    // between passes (gpu_passes, Kokiri Forest, 2026-11-10).
    static int batch = -1;
    if (batch < 0) {
        batch = rt_knob("MESH_EMU_BATCH", "1");
        rt_log("vk: mesh emulation: the stages of a pass's emulated draws %s (NX2NAVI_MESH_EMU_BATCH=%d)",
               batch ? "run before the pass, in a command buffer of their own" : "split the pass", batch);
    }
    VkCommandBuffer pre = VK_NULL_HANDLE;
    if (batch && ex->in_pass) {
        if (ex->pre_pass != ex->pass_serial && vk_exec_split_now(ex)) {
            if (!prepare_draw_ex(ex, prim, 0x7fffffff, 0, 1) || PD_info != info) {   // the pass again, in the new part
                ME.skipped++;
                return 0;
            }
            ex->pre_pass = ex->pass_serial;
            set0 = ex->dc->B.ds_bound[0];
        }
        if (ex->pre_pass == ex->pass_serial) pre = vk_sync_pre();
    }
    if (!pre) vk_exec_barrier(ex);   // ends the pass; what the game wrote before (its culling) is visible to the stages
    if (!pre && MEQ.n) me_flush(vk_sync_pre());   // the draws queued for the pre-pass command buffer go before this one
    VkCommandBuffer cb = pre ? pre : ex->cb;
    VkMemSpan ring = {0};
    if (args) {
        if (!mesh_ring_entry(ex, cb, NULL, args, &ring.offset)) {
            warn_once(W_MESH, "the indirect mesh ring could not be created: skipped");
            ME.skipped++;
            return 0;
        }
        ring.buffer = mesh_ring, ind = &ring;
        cb = pre ? pre : ex->cb;
    }
    uint32_t ep = vk_texture_epoch();
    if (ep != ME.epoch) {   // the first emulated draw of this submission: the counters start over
        ME.epoch = ep;
        if (ME.rb && ME.rb_map) {   // the last submission's counters to the readback ring; one 32 resets back is read
            if (ME.resets >= 32) {
                vmaInvalidateAllocation(vk_ctx()->vma, ME.rb_alloc, 0, VK_WHOLE_SIZE);
                const uint32_t *h = ME.rb_map + 4 * ((ME.resets - 32) % ME_RB);
                for (int k = 0; k < 4; k++)
                    if (h[k] > ME.max[k]) ME.max[k] = h[k];
            }
            VkBufferCopy rc = {0, 16 * (ME.resets % ME_RB), 16};
            vkCmdCopyBuffer(cb, ME.buf, ME.rb, 1, &rc);
            ME.resets++;
            me_barrier(cb, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT);
        }
        vkCmdFillBuffer(cb, ME.buf, 0, 32, 0);   // the counters, and the diagnosis words 4-7 (mesh_emu_spirv.c)
        me_barrier(cb, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    uint32_t rec = ME.rec++ % MESH_EMU_DRECS;
    VkDeviceSize ro = MESH_EMU_DOFF + (VkDeviceSize)rec * MESH_EMU_DWORDS * 4;
    if (MEQ.n && (MEQ.n == MEQ_MAX || (MEQ.q[0].rec + (uint32_t)MEQ.n) % MESH_EMU_DRECS != rec))
        me_flush(vk_sync_pre());   // full, or not the next record (the allocation shader takes a run of them)
    // the record: DrawIndexedIndirect {0, 1, 0, 0, 0}, mesh dispatch, task base, then the words the allocation shader
    // reads (mesh_emu_spirv.h MESH_EMU_R_*); an indirect first stage's count is copied from its arguments
    vkCmdFillBuffer(cb, ME.buf, ro, 4, 0);
    vkCmdFillBuffer(cb, ME.buf, ro + 4, 4, 1);
    vkCmdFillBuffer(cb, ME.buf, ro + 8, (MESH_EMU_R_FIRST - 2) * 4, 0);
    if (ind) {
        const VkMemSpan *src = args ? args : ind;
        VkBufferCopy cp = {src->offset, ro + MESH_EMU_R_FIRST * 4, 4};
        vkCmdCopyBuffer(cb, src->buffer, ME.buf, 1, &cp);
    } else vkCmdFillBuffer(cb, ME.buf, ro + MESH_EMU_R_FIRST * 4, 4, count);
    vkCmdFillBuffer(cb, ME.buf, ro + MESH_EMU_R_ICUR * 4, 8, 0);
    vkCmdFillBuffer(cb, ME.buf, ro + MESH_EMU_R_PER * 4, 4, mi.max_p * (mi.prim ? mi.prim : 3));
    vkCmdFillBuffer(cb, ME.buf, ro + MESH_EMU_R_TASK * 4, 8, tp ? 1 : 0);   // and the unused last word
    uint32_t pc[8] = {(uint32_t)ME.addr, (uint32_t)(ME.addr >> 32), (uint32_t)(ro / 4), ME.iw, ME.icap, ME.vw, ME.vcap, ME.trecs};
    MeQ *q = &MEQ.q[MEQ.n];
    *q = (MeQ){.tp = tp, .mp = mp, .layout = layout, .tpl_c = tpl_c, .set0 = set0, .pset = pool ? PL.set : VK_NULL_HANDLE,
               .push = push, .count = count, .rec = rec, .ib = ind ? ind->buffer : VK_NULL_HANDLE, .io = ind ? ind->offset : 0};
    memcpy(q->pc, pc, sizeof pc);
    if (push) {
        if (MEQ.dn + emu_d_bytes > MEQ.dcap) {
            size_t c = MEQ.dcap ? MEQ.dcap * 2 : 65536;
            while (c < MEQ.dn + emu_d_bytes) c *= 2;
            uint8_t *d = realloc(MEQ.d, c);
            if (!d) {
                ME.skipped++;
                return 0;
            }
            MEQ.d = d, MEQ.dcap = c;
        }
        memcpy(MEQ.d + MEQ.dn, emu_d, emu_d_bytes);
        q->doff = MEQ.dn, q->dbytes = emu_d_bytes, MEQ.dn += emu_d_bytes;
    }
    MEQ.n++, MEQ.tasks += !!tp;
    if (!pre) ex->recorded += 3 + me_flush(cb);   // the one draw's stages, here
    if (!pre) {
        // the draw: prepared again (the pass reopens, every binding is recorded again)
        vk_exec_draw_new_cb();
        ex->dirty = VK_EXEC_DIRTY_ALL;
        if (!prepare_draw_ex(ex, prim, 0x7fffffff, 0, 1) || PD_info != info) {
            ME.skipped++;
            return 0;
        }
    }
    cb = ex->cb;
    vkCmdBindIndexBuffer(cb, ME.buf, (VkDeviceSize)ME.iw * 4, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT, 0, MESH_EMU_PC_BYTES, pc);
    vkCmdDrawIndexedIndirect(cb, ME.buf, ro, 1, 20);
    ex->recorded += 3;
    vk_exec_frame.draws++;
    // the index buffer and push constants are the emulation's: the next draw binds everything again
    vk_exec_draw_new_cb();
    ex->dirty = VK_EXEC_DIRTY_ALL;
    ME.draws++;
    static uint64_t log_frame;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if ((rt_trace & RT_TRACE_PERF) && f / 600 != log_frame / 600) {
        log_frame = f;
        rt_log("perf: frame %llu: mesh emulation: %llu draws, %llu skipped (not emulated or not ready); most in one submission: "
               "%u of %u vertex slots, %u of %u indices, %u of %u task records, %u mesh workgroups that did not fit",
               (unsigned long long)f, (unsigned long long)ME.draws, (unsigned long long)ME.skipped, ME.max[0], ME.vcap,
               ME.max[1], ME.icap, ME.max[2], ME.trecs, ME.max[3]);
        ME.draws = ME.skipped = 0;
        memset(ME.max, 0, sizeof ME.max);
    }
    return 1;
}

// Binds a draw's index buffer unless the command buffer holds that bind already: the same buffer, offset and index
// type as the indexed draw before, as when a mesh is drawn again with other uniforms or textures. B.ib is cleared with
// the rest of B wherever the replay goes on in another command buffer or something else has bound into this one
// (vk_exec_draw_new_cb, cb_res: a new submission or part, a chunk's own command buffer, the draws of mo_emit and
// mesh_emu_draw, which bind index buffers of their own), so a bind from before those is never taken for held.
static void bind_index_buffer(VkExec *ex, VkBuffer b, VkDeviceSize o, VkIndexType t) {
    CbState *held = &ex->dc->B;
    if (held->ib_valid && held->ib == b && held->ib_off == o && held->ib_type == t) return;
    vkCmdBindIndexBuffer(ex->cb, b, o, t);
    ex->recorded++;
    held->ib = b, held->ib_off = o, held->ib_type = t, held->ib_valid = 1;
}

// Indices made here (a quad list or line loop as triangles or a strip, byte indices widened), from a place of their own
// in the submission's transient memory.
static void bind_index(VkExec *ex, const void *data, uint64_t bytes, VkIndexType type) {
    VkBuffer b;
    VkDeviceSize o;
    memcpy(scratch(ex, bytes, &b, &o), data, bytes);
    bind_index_buffer(ex, b, o, type);
}

static void op_draw(VkExec *ex, const p_draw *d) {
    ex->dc->c0_binst = (uint32_t)d->base_instance;
    if (d->count <= 0 || !prepare_draw(ex, d->prim, d->count, 0)) return;
    vk_exec_frame.draws++;
    uint32_t inst = d->instances > 0 ? (uint32_t)d->instances : 1, first = (uint32_t)d->first, n = (uint32_t)d->count;
    if (d->prim == NVN_DRAW_PRIMITIVE_QUADS && n >= 4) {   // quads become two triangles each
        uint32_t q = n / 4;
        uint32_t *ix = malloc(sizeof *ix * 6 * q);
        if (!ix) rt_fatal("vk: out of memory");
        for (uint32_t i = 0; i < q; i++) {
            uint32_t v = first + 4 * i, k[6] = {0, 1, 2, 0, 2, 3};
            for (int j = 0; j < 6; j++) ix[6 * i + j] = v + k[j];
        }
        bind_index(ex, ix, sizeof *ix * 6 * q, VK_INDEX_TYPE_UINT32);
        free(ix);
        vkCmdDrawIndexed(ex->cb, 6 * q, inst, 0, 0, (uint32_t)d->base_instance);
    } else if (d->prim == NVN_DRAW_PRIMITIVE_LINE_LOOP && n >= 2) {   // a strip that returns to its first vertex
        uint32_t *ix = malloc(sizeof *ix * (n + 1));
        if (!ix) rt_fatal("vk: out of memory");
        for (uint32_t i = 0; i < n; i++) ix[i] = first + i;
        ix[n] = first;
        bind_index(ex, ix, sizeof *ix * (n + 1), VK_INDEX_TYPE_UINT32);
        free(ix);
        vkCmdDrawIndexed(ex->cb, n + 1, inst, 0, 0, (uint32_t)d->base_instance);
    } else {
        vkCmdDraw(ex->cb, n, inst, first, (uint32_t)d->base_instance);
    }
    ex->recorded++;
}

// Primitive restart of an indexed draw. NVN restarts strips wherever an index equals the SetPrimitiveRestart value;
// Vulkan restarts only at the all-ones value of the bound index type, and only strips and fans without
// primitiveTopologyListRestart. Byte indices are widened to 16 bits, so their restart value becomes 0xffff
// (op_draw_idx). Without restart the 0xffff entries of NX2NAVI's terrain and cliff strips fetch vertex 65535 + base and
// the triangles around them drop out.
static int restart_for(const VkExec *ex, int prim, int itype) {
    if (!ex->prim_restart.enable) return 0;
    switch (vk_pipeline_topology(prim)) {
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY: case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY: break;
    default: return 0;   // lists: a restart index only drops the unfinished primitive
    }
    static const uint32_t all_ones[3] = {0xffu, 0xffffu, 0xffffffffu};
    if (ex->prim_restart.index == all_ones[itype]) return 1;
    warn_once(W_RESTART, "primitive restart index %#x with %d-byte indices is not replayed: strips drawn without restart",
              ex->prim_restart.index, 1 << itype);
    return 0;
}

// NX2NAVI_QUAD_INDEX (default 0): an indexed quad list is drawn as two triangles per quad and an indexed line loop as a
// strip that comes back to its first vertex, from the game's indices (vk_index_rewrite.h), as the non-indexed ones are
// (op_draw). Off, they go to the GPU with the game's indices under the triangle list and line strip topologies, and the
// first such draw is logged either way. Read once.
static int quad_index_on(void) {
    static int on = -1;
    if (on < 0) on = cfg_bool("NX2NAVI_QUAD_INDEX");
    return on;
}

// An indexed quad list or line loop, drawn with indices made from the game's (NX2NAVI_QUAD_INDEX). 0: not one of those,
// or left as it was (setting off, indices the CPU cannot read, a loop with primitive restart); the caller draws it.
static int draw_idx_rewritten(VkExec *ex, const p_draw_idx *d, const VkMemSpan *sp, int restart) {
    static const uint32_t isize[3] = {1, 2, 4};
    int quads = d->prim == NVN_DRAW_PRIMITIVE_QUADS, loop = d->prim == NVN_DRAW_PRIMITIVE_LINE_LOOP;
    if (!quads && !loop) return 0;
    if (!quad_index_on()) {
        warn_once(W_QUAD, "an indexed %s (%d indices): drawn with its indices as a %s (quad_index = 1 rewrites them)",
                  quads ? "quad list" : "line loop", d->count, quads ? "triangle list" : "line strip");
        return 0;
    }
    if (sp->alloc || (loop && restart)) {
        warn_once(W_QUAD, "an indexed %s (%d indices, %s): drawn with its indices as they are", quads ? "quad list" : "line loop",
                  d->count, sp->alloc ? "indices in device-local memory" : "primitive restart");
        return 0;
    }
    uint32_t n = (uint32_t)d->count, unit = isize[d->itype];
    if (quads && n < 4) return 1;   // no whole quad: nothing to draw
    if (loop && n < 2) return 0;
    uint32_t cap = quads ? 6 * (n / 4) : n + 1;
    uint32_t *ix = malloc(sizeof *ix * (size_t)cap);
    if (!ix) rt_fatal("vk: out of memory");
    const uint8_t *src = (const uint8_t *)(uintptr_t)d->ibuf;
    uint32_t m = quads ? idx_quads(src, unit, n, ix) : idx_loop(src, unit, n, ix);
    vk_mem_count(sp->cls, VK_MUSE_CPU_READ, (uint64_t)n * unit);
    vk_mem_count(VK_MCLS_SCRATCH, VK_MUSE_IB, sizeof *ix * (uint64_t)m);
    bind_index(ex, ix, sizeof *ix * (uint64_t)m, VK_INDEX_TYPE_UINT32);
    free(ix);
    static int told;
    if (!told++)
        rt_log("vk: an indexed %s of %u indices drawn as %u indices of a %s (NX2NAVI_QUAD_INDEX)", quads ? "quad list" : "line loop",
               n, m, quads ? "triangle list" : "line strip");
    vkCmdDrawIndexed(ex->cb, m, d->instances > 0 ? (uint32_t)d->instances : 1, 0, d->base_vertex, (uint32_t)d->base_instance);
    ex->recorded++;
    return 1;
}

static void op_draw_idx(VkExec *ex, const p_draw_idx *d) {
    ex->dc->c0_binst = (uint32_t)d->base_instance;
    static const uint32_t isize[3] = {1, 2, 4};
    if (d->itype < 0 || d->itype > 2) return warn_once(W_INDEX, "index type %d is unknown: draw skipped", d->itype);
    int restart = restart_for(ex, d->prim, d->itype);
    if (d->count <= 0 || !prepare_draw(ex, d->prim, d->count, restart)) return;
    vk_exec_frame.draws++;
    uint64_t bytes = (uint64_t)d->count * isize[d->itype];
    VkMemSpan sp;
    if (!vk_mem_resolve(d->ibuf, bytes, &sp)) return warn_once(W_INDEX, "index buffer %#llx is not backed: draws with it are skipped", (unsigned long long)d->ibuf);
    if (draw_idx_rewritten(ex, d, &sp, restart)) return;
    if (d->itype == NVN_INDEX_TYPE_UNSIGNED_BYTE) {   // no uint8 index type here: widen (host-readable backing only)
        if (sp.alloc) return warn_once(W_INDEX, "byte indices in device-local memory: draw skipped");
        vk_mem_count(sp.cls, VK_MUSE_CPU_READ, bytes);
        vk_mem_count(VK_MCLS_SCRATCH, VK_MUSE_IB, 2 * bytes);
        uint16_t *ix = malloc(sizeof *ix * (size_t)d->count);
        if (!ix) rt_fatal("vk: out of memory");
        for (int i = 0; i < d->count; i++) {
            uint8_t v = ((const uint8_t *)(uintptr_t)d->ibuf)[i];
            ix[i] = restart && v == 0xff ? 0xffff : v;
        }
        bind_index(ex, ix, sizeof *ix * (size_t)d->count, VK_INDEX_TYPE_UINT16);
        free(ix);
    } else if (sp.offset % isize[d->itype]) {
        return warn_once(W_INDEX, "index buffer %#llx is not aligned to its index size: draw skipped", (unsigned long long)d->ibuf);
    } else {
        VK_MEM_USE(sp);
        bind_index_buffer(ex, sp.buffer, sp.offset, d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vk_mem_count(sp.cls, VK_MUSE_IB, bytes);
        VK_MEM_BOUND(sp);
    }
    vkCmdDrawIndexed(ex->cb, (uint32_t)d->count, d->instances > 0 ? (uint32_t)d->instances : 1, 0, d->base_vertex,
                     (uint32_t)d->base_instance);
    ex->recorded++;
    if (d->itype != NVN_INDEX_TYPE_UNSIGNED_BYTE) mvobj_draw(ex, d, sp.buffer, sp.offset);
}

// DrawElementsIndirect: the arguments are read by the GPU from the pool buffer (they are often written by an earlier
// dispatch). The base instance stays out of c0 (0x644): c0 cannot be patched by a transfer
// inside a pass; gl_BaseInstance of the Vulkan draw is the argument's. Byte indices need the count to be widened: skipped.
static void op_draw_idx_ind(VkExec *ex, const p_draw_idx_ind *d) {
    ex->dc->c0_binst = 0;
    if (d->itype != NVN_INDEX_TYPE_UNSIGNED_SHORT && d->itype != NVN_INDEX_TYPE_UNSIGNED_INT)
        return warn_once(W_INDEX, "indirect draw with index type %d: skipped", d->itype);
    VkMemSpan ib, ip;
    if (!vk_mem_resolve(d->ind, sizeof(VkDrawIndexedIndirectCommand), &ip) || ip.offset % 4)
        return warn_once(W_INDIRECT, "indirect draw arguments at %#llx are not backed or not aligned: draws skipped", (unsigned long long)d->ind);
    if (!vk_mem_resolve(d->ibuf, 1, &ib) || ib.offset % (d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? 2 : 4))
        return warn_once(W_INDEX, "index buffer %#llx is not backed or not aligned: indirect draws skipped", (unsigned long long)d->ibuf);
    if (!prepare_draw(ex, d->prim, 0x7fffffff, restart_for(ex, d->prim, d->itype))) return;
    vk_exec_frame.draws++;
    VK_MEM_USE(ip);
    VK_MEM_USE(ib);
    bind_index_buffer(ex, ib.buffer, ib.offset, d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexedIndirect(ex->cb, ip.buffer, ip.offset, 1, sizeof(VkDrawIndexedIndirectCommand));
    ex->recorded++;
    vk_mem_count(ip.cls, VK_MUSE_IND, sizeof(VkDrawIndexedIndirectCommand));
    VK_MEM_BOUND(ip);
    VK_MEM_BOUND(ib);
}

// ---- dispatch and uniform buffer updates

// A compute dispatch whose shader cannot be translated: Switch 2 titles run tensor-core networks (HMMA/LDSM, an
// AI upscaler: Ocarina's title movie goes 1920x1080 -> 2560x1440 through ten such passes) that the Maxwell translator
// has no model for. Instead of leaving the outputs untouched (black), the largest bound storage image receives a
// linear blit of the largest bound sampled RGBA8 texture that is a different image and no larger (the networks'
// 10-bit intermediates are only partly meaningful as pictures, so they are not a source): what an upscaler
// approximates. In-place passes (the output also sampled) are left alone. NX2NAVI_CS_FALLBACK=0 turns it off.
// sRGB-ness of the RGBA8 formats the fallback blits: a blit converts between encodings, the game's network writes the
// bits through a UNORM view, so a UNORM source and an sRGB target go through a scratch image in the source's format
// (blit, scaling only) and then a raw copy.
static int fmt_srgb(VkFormat f) { return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32; }
static int fmt_rgba8(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_UNORM ||
           f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_UNORM_PACK32 || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

// Scratch images for those blits, one per (size, format), at most SCRATCH_N; a new size takes the place of the one used
// longest ago (vk_scratch_lru.h), whose image goes through the deferred destruction.
#define SCRATCH_N 8
static VkImage fallback_scratch(VkExec *ex, uint32_t w, uint32_t h, VkFormat f) {
    static ScrEntry ent[SCRATCH_N];
    static struct { VkImage img; VmaAllocation a; } cache[SCRATCH_N];
    static int evictions;
    int hit, evict, i = scr_pick(ent, SCRATCH_N, w, h, (uint32_t)f,
                                 __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), &hit, &evict);
    if (hit) return cache[i].img;
    if (evict) {
        if (evictions++ < 8)
            rt_log("vk: a stand-in scratch image of %ux%u (format %d) makes room for one of %ux%u (format %d)",
                   ent[i].w, ent[i].h, (int)ent[i].f, w, h, (int)f);
        vk_tonemap_forget(cache[i].img);   // the stand-in's views of it, before a new image can take its handle
        vk_defer_image(cache[i].img, cache[i].a);
        ent[i].live = 0, cache[i].img = VK_NULL_HANDLE, cache[i].a = NULL;
    }
    VkCtx *c = vk_ctx();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     (f == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 && knob_tm_dispatch() != 2 ? VK_IMAGE_USAGE_SAMPLED_BIT : 0),
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ia = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage img; VmaAllocation a;
    if (vmaCreateImage(c->vma, &ii, &ia, &img, &a, NULL) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkImageMemoryBarrier2 bar = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                 .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                 .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                 .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &bar};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ent[i] = (ScrEntry){w, h, (uint32_t)f, __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), 1};
    cache[i].img = img, cache[i].a = a;
    return img;
}

// 1 when the bound compute program uses tensor-core instructions (HMMA 0x23c, IMMA 0x237, LDSM 0x83b): the game's
// upscalers (the movie's and the 3D scene's), which the fallback blit stands in for unless native_tensor runs them.
// Other untranslated programs (bindless texturing, indirect branches) bind the output image too, and a blit for them
// pasted G-buffer normals and intermediates over the picture. Kept per program serial.
static int uses_tensor_ops(const VkExec *ex) {
    static struct { uint64_t serial; int8_t v; } memo[256];
    const uint8_t *ctl, *code;
    uint32_t len;
    if (!ex->prog || !find_stage(ex->dc, ex->prog, NVN_STAGE_CS, &ctl, &code, &len)) return 0;
    uint64_t sr = ex->prog->serial;
    typeof(memo[0]) *m = &memo[(sr * 0x9E3779B97F4A7C15ull) >> 56];
    if (sr && m->serial == sr) return m->v;
    int v = 0;
    if (ctl_nvn2(ctl))
        for (uint32_t o = 0; o + 16 <= len && !v; o += 16) {
            uint32_t op = (uint32_t)(code[o] | code[o + 1] << 8) & 0xfff;
            v = op == 0x23c || op == 0x237 || op == 0x83b;
        }
    m->serial = sr, m->v = (int8_t)v;
    return v;
}

// NX2NAVI_NATIVE_TENSOR=1: tensor-core programs run their translation (HMMA, LDSM and MOVM lowered to warp shuffles in
// tools/xlate/AmpereLowering.cs Hmma, Ldsm, Movm). The lowering gives the hardware's results: its SPIR-V, run by the
// driver, matches what the same SM86 kernels compute natively, bit for bit, on single instructions and on a network
// pass over four warps (tests/tensor_gpu_check.py). The infinities and noise the scene upscaler wrote came from its
// input picture, not from the arithmetic (net_binding). Still off by default: no game run has confirmed the picture
// since, and each network pipeline takes the driver tens of seconds and about 100 MiB of device memory to build
// (vk_pipeline.c, compute pipeline prebuild). While off, the programs take the fallback below as untranslated ones do.
static int native_tensor(const VkExec *ex) {
    static int on = -1;
    if (on < 0) on = rt_knob("NATIVE_TENSOR", "0");
    return on || !uses_tensor_ops(ex);
}

// Everything an untranslated program binds (NX2NAVI_LOG_CS_BINDS): what a stand-in could use instead. Texture lock held.
static void log_cs_bindings_locked(const VkExec *ex) {
    char line[1600];
    int o = 0;
    line[0] = 0;
    for (int i = 0; i < 160 && o < (int)sizeof line - 80; i++) {
        int im = i >= 128;
        uint64_t h = im ? ex->img[NVN_STAGE_CS][i - 128 < 32 ? i - 128 : 0] : ex->tex[NVN_STAGE_CS][i];
        if (im && i - 128 >= 32) break;
        view_t v; int hv = 0;
        const VkTexImage *t = h ? vk_texture_by_id_locked(NVN_HANDLE_TEX(h), &v, &hv) : NULL;
        if (t && (i == 0 || h != ex->tex[NVN_STAGE_CS][0] || im))
            o += snprintf(line + o, sizeof line - o, " %s%d=%ux%u/f%d@%#llx", im ? "img" : "tex", im ? i - 128 : i,
                          t->width, t->height, t->format, (unsigned long long)t->addr);
    }
    for (int i = 0; i < 18 && o < (int)sizeof line - 60; i++)
        if (ex->ubo[NVN_STAGE_CS][i].size)
            o += snprintf(line + o, sizeof line - o, " ubo%d=%#llx+%#x", i + 3,
                          (unsigned long long)ex->ubo[NVN_STAGE_CS][i].addr, (unsigned)ex->ubo[NVN_STAGE_CS][i].size);
    const uint8_t *ctl, *code;
    uint32_t len;
    int ntex = -1, nimg = -1;
    if (ex->prog && find_stage(ex->dc, ex->prog, NVN_STAGE_CS, &ctl, &code, &len) && ctl_nvn2(ctl)) ntex = ctl[0xb3], nimg = ctl[0xb6];
    rt_log("vk_exec:   its bindings (control: %d texture, %d image entries):%s", ntex, nimg, line);
}

// NX2NAVI_LOG_CS_BINDS: an untranslated program the fallback leaves alone, so its output keeps what it held (black when
// nothing else wrote it): why, and what it binds. in_place -1: not a tensor program (its translation failed). Each
// (program, output) once, 256 at most. Texture lock held.
static void log_fallback_skip_locked(const VkExec *ex, const VkTexImage *out, int in_place) {
    static struct { uint64_t prog, d; } seen[256];
    static int n;
    uint64_t pr = ex->prog ? ex->prog->serial : 0, d = out ? out->addr : 0;
    for (int k = 0; k < n; k++)
        if (seen[k].prog == pr && seen[k].d == d) return;
    if (n == 256) return;
    seen[n].prog = pr, seen[n].d = d, n++;
    if (in_place < 0)
        rt_log("vk_exec: fallback leaves prog %llu alone: not translated and no tensor-core ops, so nothing stands in",
               (unsigned long long)pr);
    else if (!out)
        rt_log("vk_exec: fallback leaves prog %llu alone: it binds no single-sample 2D colour image to write",
               (unsigned long long)pr);
    else
        rt_log("vk_exec: fallback leaves prog %llu alone: %s; its output %ux%u fmt %d at %#llx", (unsigned long long)pr,
               in_place ? "it also samples its output (in place)"
                        : "no RGBA8 input and no input in the output's format, no larger than the output",
               out->width, out->height, out->format, (unsigned long long)out->addr);
    log_cs_bindings_locked(ex);
}

// The scene upscaler's input as an RGB9E5 image, for an output in r11g11b10f: the post pass's picture (raw_9e5's words).
static int fallback_9e5_scene(VkFormat src, VkFormat dst) {
    return src == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 && dst == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
}

// FNV-1a of a program's compute code as the game hands it over, kept per program: what vk_aa.c knows the game's SMAA
// passes by, the same with every translator and setting. 0 without compute code.
static uint64_t cs_guest_hash(const prog_t *p) {
    static struct { uint64_t serial, hash; } memo[1024];
    typeof(memo[0]) *m = p->serial ? &memo[p->serial * 0x9E3779B97F4A7C15ull >> 54] : NULL;
    if (m && m->serial == p->serial) return m->hash;
    const uint8_t *ctl, *code;
    uint32_t len;
    uint64_t h = 0;
    if (find_stage_scan(p, NVN_STAGE_CS, &ctl, &code, &len) && code && len) {
        h = 0xcbf29ce484222325ull;
        for (uint32_t i = 0; i < len; i++) h = (h ^ code[i]) * 0x100000001b3ull;
    }
    if (m) m->serial = p->serial, m->hash = h;
    return h;
}

// The anti-aliasing setting (vk_aa.c): while it asks for something other than the game's SMAA, the game's edge and
// weight passes are left out and its composition is replaced by vk_aa_record, from the same composite (the sampled
// R11G11B10F image of the output's size) into the same scene target, through the integer storage view the game's own
// pass writes it with. 1: the dispatch is handled (left out or replaced). NX2NAVI_AA_LOG=1 logs each compute program's
// guest code hash once, with its dispatch size and output, to find the passes in another game version.
static int aa_dispatch(VkExec *ex, int x, int y, int z) {
    int mode = vk_aa_mode();
    if (mode == VK_AA_GAME || !ex->prog) return 0;
    uint64_t gh = cs_guest_hash(ex->prog);
    static int log_on = -1;
    if (log_on < 0) log_on = rt_knob("AA_LOG", "0");
    if (log_on) {
        static uint64_t seen[512];
        static int nseen;
        int known = 0;
        for (int i = 0; i < nseen; i++) known |= seen[i] == gh;
        if (!known && nseen < 512) {
            seen[nseen++] = gh;
            const VkTexImage *o = NULL;
            vk_texture_lock();
            for (int i = 0; i < 32 && !o; i++) {
                uint64_t h = ex->img[NVN_STAGE_CS][i];
                view_t v; int hv = 0;
                o = h ? vk_texture_by_id_locked(NVN_HANDLE_TEX(h), &v, &hv) : NULL;
            }
            rt_log("aa: compute prog %llu guest %016llx dispatch %dx%dx%d output %ux%u fmt %d",
                   (unsigned long long)ex->prog->serial, (unsigned long long)gh, x, y, z, o ? o->width : 0,
                   o ? o->height : 0, o ? (int)o->format : 0);
            vk_texture_unlock();
        }
    }
    int role = vk_aa_role(gh);
    if (role == VK_AA_ROLE_NONE) return 0;
    if (role != VK_AA_ROLE_COMPOSE) return vk_aa_skip_ok();
    const VkTexImage *dst = NULL, *src = NULL;
    VkImageView dv = VK_NULL_HANDLE, sv = VK_NULL_HANDLE;
    vk_texture_lock();
    // the scene target, written as raw words: the R11G11B10F image, or at times the RGB9E5 one the same memory also
    // resolves as (raw_9e5_twin), which takes the same words
    for (int i = 0; i < 32 && !dst; i++) {
        uint64_t h = ex->img[NVN_STAGE_CS][i];
        view_t v; int hv = 0;
        const VkTexImage *t = h ? vk_texture_by_id_locked(NVN_HANDLE_TEX(h), &v, &hv) : NULL;
        if (!t || (t->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 && t->format != VK_FORMAT_E5B9G9R9_UFLOAT_PACK32) ||
            t->type != VK_IMAGE_TYPE_2D || t->samples != 1)
            continue;
        dst = t;
        if (storage_view_format(t, &v, hv) == VK_FORMAT_R32_UINT) dv = get_view(ex, t, &v, hv, 3, 1);   // 3: Texture2D
    }
    for (int i = 0; dst && i < 128 && !src; i++) {   // the composite: R11G11B10F of the same size, another image
        uint64_t h = ex->tex[NVN_STAGE_CS][i];
        view_t v; int hv = 0;
        const VkTexImage *t = h ? vk_texture_by_id_locked(NVN_HANDLE_TEX(h), &v, &hv) : NULL;
        if (!t || t->image == dst->image || t->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || t->width != dst->width ||
            t->height != dst->height || t->type != VK_IMAGE_TYPE_2D || t->samples != 1)
            continue;
        src = t;
        view_t nv;
        memset(&nv, 0, sizeof nv);
        sv = get_view(ex, t, &nv, 0, 3, 0);
    }
    uint32_t w = dst ? dst->width : 0, hh = dst ? dst->height : 0;
    if (dv && sv) vk_texture_storage_bound_locked(dst);   // the GPU writes it below, as the game's pass would have
    // Left to the game, once logged. Expected while an area loads: the target is then the RGB9E5 image at the same
    // memory, made without storage use, which the game's own pass cannot write either.
    static int told;
    if ((!dv || !sv) && !told++)
        rt_log("aa: a composition of the game's SMAA left to the game: target %s (%ux%u fmt %d, storage %d), composite %s",
               dst ? (dv ? "ok" : "without an integer storage view") : "not found", w, hh, dst ? (int)dst->format : 0,
               dst ? !!(dst->usage & VK_IMAGE_USAGE_STORAGE_BIT) : 0, src ? (sv ? "ok" : "without a view") : "not found");
    vk_texture_unlock();
    // recorded without the texture lock: SMAA's first frame waits for its lookup textures' upload. The views stay valid
    // meanwhile: images and views go only through the deferred release.
    int ok = 0;
    if (dv && sv) {
        vk_exec_barrier_dispatch(ex);   // ends the pass; the composite's writes are visible
        ok = vk_aa_record(ex->cb, mode, sv, dv, w, hh);
        if (ok) {
            ex->dc->B.pipe[1] = VK_NULL_HANDLE;   // the replay must bind its compute pipeline again
            ex->recorded++;
            vk_exec_barrier(ex);
        }
    }
    vk_aa_composed(ok);
    return ok;
}

static void untranslated_dispatch_fallback(VkExec *ex) {
    static int on = -1;
    if (on < 0) on = rt_knob("CS_FALLBACK", "1");
    if (!on) return;
    if (!uses_tensor_ops(ex)) {   // its translation failed, and no stand-in exists for programs without tensor ops
        if (knob_log_cs_binds()) {
            vk_texture_lock();
            log_fallback_skip_locked(ex, NULL, -1);
            vk_texture_unlock();
        }
        return;
    }
    const VkTexImage *dst = NULL, *src = NULL, *out;
    int in_place = 0;
    // The output is looked for among the image slots the program uses: the count of image entries of its NVN2 control
    // section (byte +0xb6), at least slot 0 (the networks' in-place passes count none and write slot 0). An image slot
    // keeps its binding from earlier dispatches, so past that count lie other programs' images: with a 3840x2160
    // picture the scene upscaler leaves its 3840x2160 RGB9E5 target in slot 2, the largest image bound to the movie
    // network's last pass, and that pass's real output (2560x1440, slot 1, which the frame's last passes enlarge) kept
    // stale bits for the whole movie: the Triforce movie after Gohma showed as frozen coloured stripes.
    int nimg = 32;
    const uint8_t *ctl, *code;
    uint32_t len;
    if (ex->prog && find_stage(ex->dc, ex->prog, NVN_STAGE_CS, &ctl, &code, &len) && ctl_nvn2(ctl)) nimg = ctl[0xb6] ? ctl[0xb6] : 1;
    if (nimg > 32) nimg = 32;
    vk_texture_lock();
    for (int i = 0; i < nimg; i++) {
        uint64_t h = ex->img[NVN_STAGE_CS][i];
        view_t v; int hv = 0;
        const VkTexImage *t = h ? vk_texture_by_id_locked(NVN_HANDLE_TEX(h), &v, &hv) : NULL;
        if (t && t->aspect == VK_IMAGE_ASPECT_COLOR_BIT && t->type == VK_IMAGE_TYPE_2D && t->samples == 1 &&
            (!dst || (uint64_t)t->width * t->height > (uint64_t)dst->width * dst->height))
            dst = t;
    }
    out = dst;
    for (int i = 0; dst && i < 128; i++) {   // an in-place pass (its output also sampled) is left alone
        uint64_t h = ex->tex[NVN_STAGE_CS][i];
        view_t v; int hv = 0;
        const VkTexImage *t = h ? vk_texture_by_id_locked(NVN_HANDLE_TEX(h), &v, &hv) : NULL;
        if (t && t->image == dst->image) { dst = NULL; in_place = 1; break; }
    }
    for (int i = 0; dst && i < 128; i++) {
        uint64_t h = ex->tex[NVN_STAGE_CS][i];
        view_t v; int hv = 0;
        const VkTexImage *t = h ? vk_texture_by_id_locked(NVN_HANDLE_TEX(h), &v, &hv) : NULL;
        // RGBA8 sources (the movie's frames; its 10-bit intermediates gave black bands), or one in the output's own
        // format: the 3D scene (r11g11b10f) the game's upscaler enlarges to the output resolution
        int rgba8 = t && (t->format == VK_FORMAT_R8G8B8A8_UNORM || t->format == VK_FORMAT_R8G8B8A8_SRGB);
        // The picture also resolves as RGB9E5 at the same address; what the post pass wrote there stands in for it below.
        // Refused for its format, the next input of its size (the older picture the upscaler blends with) was shown, and
        // what moves trailed.
        int same = t && (t->format == dst->format || (fallback_9e5_scene(t->format, dst->format) && raw_9e5_twin(t)));
        if (t && (rgba8 || same) && t->image != dst->image && t->aspect == VK_IMAGE_ASPECT_COLOR_BIT && t->type == VK_IMAGE_TYPE_2D &&
            t->samples == 1 && t->width <= dst->width && t->height <= dst->height && t->width >= 16 && t->height >= 16 &&
            (!src || (uint64_t)t->width * t->height > (uint64_t)src->width * src->height))
            src = t;
    }
    if (dst && src) vk_texture_storage_bound_locked(dst);
    else if (knob_log_cs_binds()) log_fallback_skip_locked(ex, out, in_place);
    if (dst && src && knob_log_cs_binds()) {   // each distinct (program, source, target) once, 256 at most
        static struct { uint64_t prog, s, d; } seen[256];
        static int n;
        uint64_t pr = ex->prog ? ex->prog->serial : 0;
        int k = 0;
        while (k < n && (seen[k].prog != pr || seen[k].s != src->addr || seen[k].d != dst->addr)) k++;
        if (k == n && n < 256) seen[n++] = (typeof(seen[0])){pr, src->addr, dst->addr}, k = -1;
        if (k < 0) {
            rt_log("vk_exec: fallback blit prog %llu: %ux%u fmt %d at %#llx -> %ux%u fmt %d at %#llx",
                   (unsigned long long)(ex->prog ? ex->prog->serial : 0), src->width, src->height, src->format,
                   (unsigned long long)src->addr, dst->width, dst->height, dst->format, (unsigned long long)dst->addr);
            log_cs_bindings_locked(ex);
        }
    }
    VkImage si = src ? src->image : VK_NULL_HANDLE, di = dst ? dst->image : VK_NULL_HANDLE;
    VkFormat sf = src ? src->format : VK_FORMAT_UNDEFINED;
    if (src && dst && fallback_9e5_scene(sf, dst->format)) {   // the image the post pass wrote this frame (raw_9e5_twin)
        const VkTexImage *twin = raw_9e5_twin(src);
        si = twin ? twin->image : VK_NULL_HANDLE, sf = twin ? twin->format : VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    }
    int native = raw_native_9e5(si, sf);   // RGB9E5 texels in an RGB9E5 image: decoded as they are, no copy
    VkImageView dst_view = VK_NULL_HANDLE;   // for the tonemap pass, while the lock keeps the image
    if (src && dst && (dst->usage & VK_IMAGE_USAGE_STORAGE_BIT) && (raw_9e5(si, sf) || native) &&
        dst->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && knob_tm_dispatch() != 2) {
        view_t nv;
        memset(&nv, 0, sizeof nv);
        dst_view = get_view(ex, dst, &nv, 0, 3, 1);   // 3: the translator's Texture2D
    }
    uint32_t sw = src ? src->width : 0, sh = src ? src->height : 0, dw = dst ? dst->width : 0, dh = dst ? dst->height : 0;
    VkFormat df = dst ? dst->format : VK_FORMAT_UNDEFINED;
    vk_texture_unlock();
    if (!si || !di) return;
    static int logged;
    if (!logged++) rt_log("vk_exec: untranslated compute dispatch: its output image (%ux%u) gets a linear blit of its "
                          "largest input (%ux%u) instead (NX2NAVI_CS_FALLBACK=0 turns this off)", dw, dh, sw, sh);
    vk_exec_pass_end(ex);
    vk_exec_barrier(ex);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}};
    bl.srcOffsets[1] = (VkOffset3D){(int32_t)sw, (int32_t)sh, 1};
    bl.dstOffsets[1] = (VkOffset3D){(int32_t)dw, (int32_t)dh, 1};
    VkImage scratch = fmt_rgba8(df) && fmt_srgb(sf) != fmt_srgb(df) ? fallback_scratch(ex, dw, dh, sf) : VK_NULL_HANDLE;
    static int e5_blit = -1;   // E5B9G9R9 can be a blit source (with linear filtering)
    if (e5_blit < 0) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(vk_ctx()->phys, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        e5_blit = (fp.optimalTilingFeatures & need) == need ? 1 + !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) : 0;
        rt_log("vk_exec: RGB9E5 fallback decode %s", e5_blit ? "on" : "unavailable (E5B9G9R9 is no blit source here)");
    }
    VkImage e5 = scratch || !e5_blit ? VK_NULL_HANDLE : native ? si
               : raw_9e5(si, sf) ? fallback_scratch(ex, sw, sh, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32) : VK_NULL_HANDLE;
    if (e5 && !native) {   // the raw words into a shared-exponent image (same texel size: a copy keeps the bits), then the blit
        VkImageCopy cp = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .extent = {sw, sh, 1}};
        vkCmdCopyImage(ex->cb, si, VK_IMAGE_LAYOUT_GENERAL, e5, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
        VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(ex->cb, &dep);
        si = e5;
    }
#if NX2NAVI_TONEMAP_STANDIN
    // The square decode of the stored picture (vk_tonemap.c). Builds of round 24 lost the device with it: its descriptor
    // push did not go through the recorder (vk_rec.h rec_push; PORT-HANDOFF 7.12 item 11), which it now does.
    // Its edge-adaptive filter is for the picture the screen shows: while DLSS or FSR 3.1 runs (vk_dlaa_active) that
    // upscaler scales the scene itself and vk_dlaa_upscaler below puts its picture over this one, so the pass keeps
    // its linear filter there, also in the few frames that upscaler misses.
    VkImage tm = e5 && dst_view ? vk_tonemap_record(ex->cb, e5, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, sw, sh, dw, dh,
                                                    !vk_dlaa_active())
                                : VK_NULL_HANDLE;
    if (tm) {   // the tonemapped picture, already at the target's size, then the same blit as below
        ex->dc->B.pipe[1] = VK_NULL_HANDLE;   // the replay must bind its compute pipeline again
        si = tm, sw = dw, sh = dh;
        bl.srcOffsets[1] = (VkOffset3D){(int32_t)sw, (int32_t)sh, 1};
    }
#else
    VkImage tm = VK_NULL_HANDLE;
    (void)dst_view;
#endif
    // The scene upscaler's input is the post pass's picture stored as the square root of each linear value (prog 102
    // packs sqrt(composite) into RGB9E5), and its output is linear: the HUD is drawn over it and the frame's last pass
    // encodes to sRGB. A plain blit carried the stored values into the linear target, so the picture was washed out.
    // The pass above squares them. Without it, this approximation decodes with the sRGB curve instead: scale into an
    // RGBA8 UNORM scratch (the stored values), copy those bits into an RGBA8 sRGB scratch, and blit that into the
    // target, which reads the sRGB image as linear (mid-tones a little dark, the darkest values a little lifted).
    int filter_nearest = tm || (e5 && e5_blit == 1);
    VkImage g8 = VK_NULL_HANDLE, g8s = VK_NULL_HANDLE;
    if (e5 && !tm && !scratch && !fmt_rgba8(df)) {
        g8 = fallback_scratch(ex, dw, dh, VK_FORMAT_R8G8B8A8_UNORM);
        g8s = g8 ? fallback_scratch(ex, dw, dh, VK_FORMAT_R8G8B8A8_SRGB) : VK_NULL_HANDLE;
    }
    if (g8 && g8s) {
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
        VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdBlitImage(ex->cb, si, VK_IMAGE_LAYOUT_GENERAL, g8, VK_IMAGE_LAYOUT_GENERAL, 1, &bl,
                       filter_nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        vkCmdPipelineBarrier2(ex->cb, &dep);
        VkImageCopy cp = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .extent = {dw, dh, 1}};
        vkCmdCopyImage(ex->cb, g8, VK_IMAGE_LAYOUT_GENERAL, g8s, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
        vkCmdPipelineBarrier2(ex->cb, &dep);
        si = g8s, sw = dw, sh = dh, filter_nearest = 1;
        bl.srcOffsets[1] = (VkOffset3D){(int32_t)sw, (int32_t)sh, 1};
    }
    vkCmdBlitImage(ex->cb, si, VK_IMAGE_LAYOUT_GENERAL, scratch ? scratch : di, VK_IMAGE_LAYOUT_GENERAL, 1, &bl,
                   filter_nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    if (scratch) {   // the bits as they are: a UNORM picture stays the same picture in an sRGB image's memory
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
        VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(ex->cb, &dep);
        VkImageCopy cp = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .extent = {dw, dh, 1}};
        vkCmdCopyImage(ex->cb, scratch, VK_IMAGE_LAYOUT_GENERAL, di, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    }
    ex->recorded++;
    vk_exec_barrier(ex);
    vk_dlaa_upscaler(ex, di, df, dw, dh);   // the scene upscaler's output: DLSS's or FSR's picture goes there (vk_dlaa.c)
}

static void op_dispatch(VkExec *ex, const p_dispatch *d) {
    DrawCtx *const cx = ex->dc;
    if (d->x <= 0 || d->y <= 0 || d->z <= 0) return;
    if (!ex->prog) return warn_once(W_PROGRAM, "a dispatch without a bound program is skipped");
    if (prog_lead_on()) prog_lead(ex->prog, 2);
    vk_contact_flush(ex);
    cb_res(ex);
    dummies_init();
    if (aa_dispatch(ex, d->x, d->y, d->z)) return;   // the game's SMAA left out or replaced (vk_aa.c)
    // A tensor-core program runs the stand-in while NX2NAVI_NATIVE_TENSOR is off, so ask that before translating it.
    // Translating it anyway recorded four AI-upscaler compute shaders of 0.87-4.4M SPIR-V words that this game never
    // dispatches natively, and building them cost gigabytes of memory at boot (NX2NAVI_LIB_PREBUILD_BIG, bg_main).
    if (!native_tensor(ex)) { untranslated_dispatch_fallback(ex); return; }
    int present;
    const GfxShader *cs = shader_for(ex, NVN_STAGE_CS, &present, NULL, 0);   // synchronous: a skipped dispatch changes results
    if (knob_log_cs_binds()) {   // diagnosis: the compute bindings of the first dispatch of each program
        static uint64_t seen[64]; static int nseen;
        int known = 0;
        for (int i = 0; i < nseen; i++) known |= seen[i] == ex->prog->serial;
        if (!known && nseen < 64) {
            seen[nseen++] = ex->prog->serial;
            char line[2048]; int o = snprintf(line, sizeof line, "cs binds prog %llu %dx%dx%d:", (unsigned long long)ex->prog->serial, d->x, d->y, d->z);
            for (int i = 0; i < 128 && o < 1900; i++) if (ex->tex[NVN_STAGE_CS][i]) o += snprintf(line + o, sizeof line - o, " tex%d=%llx", i, (unsigned long long)ex->tex[NVN_STAGE_CS][i]);
            for (int i = 0; i < 32 && o < 1900; i++) if (ex->img[NVN_STAGE_CS][i]) o += snprintf(line + o, sizeof line - o, " img%d=%llx", i, (unsigned long long)ex->img[NVN_STAGE_CS][i]);
            const GfxShaderRes *rs;
            int nt = cs ? gfx_shader_resources(cs, GFX_RES_TEX, &rs) : 0;
            for (int i = 0; i < nt && o < 1900; i++) o += snprintf(line + o, sizeof line - o, " want-tex(h%u,slot%d)", rs[i].handle, rs[i].slot);
            int ni = cs ? gfx_shader_resources(cs, GFX_RES_IMG, &rs) : 0;
            for (int i = 0; i < ni && o < 1900; i++) o += snprintf(line + o, sizeof line - o, " want-img(h%u,slot%d)", rs[i].handle, rs[i].slot);
            rt_log("%s%s", line, cs ? "" : " (not translated)");
        }
    }
    if (!cs || !native_tensor(ex)) { untranslated_dispatch_fallback(ex); return; }
    const VkPipeInfo *info = NULL;
    VkPipeline p = vk_pipeline_compute(cs, &info);
    if (!p || !info) return warn_once(W_PIPELINE, "a compute pipeline could not be created: dispatches with it are skipped");
    if ((info->pool && !pool_on()) || shader_skipped(cs)) return;   // NX2NAVI_POOL=0, NX2NAVI_SKIP_SHADERS
    vk_exec_barrier_dispatch(ex);   // ends the pass; earlier writes are visible to the shader
    bind_pipeline(ex, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    cx->c0_dim[0] = (uint32_t)d->x, cx->c0_dim[1] = (uint32_t)d->y, cx->c0_dim[2] = (uint32_t)d->z;
    twin_dispatch = uses_tensor_ops(ex) ? 2 : 1;
    {   // the replay's binding path is the CPU cost the frame profile needs to separate from recording
        uint64_t bt = bt_now();
        bind_resources(ex, info);
        vk_exec_frame.bind_ns += (uint64_t)(bt_now() - bt);
        vk_exec_frame.bind_calls++;
    }
    twin_dispatch = 0;
    if (vk_texture_deferred()) return;   // past a parallel group: the op goes again once it is done (vk_exec.c op_again)
    VkPassMark *m = dispatch_mark(ex, cs, (uint32_t)d->x, (uint32_t)d->y, (uint32_t)d->z);
    vkCmdDispatch(ex->cb, (uint32_t)d->x, (uint32_t)d->y, (uint32_t)d->z);
    vk_pass_mark_end(ex->cb, m);
    ex->recorded++;
    if (vk_dump_armed) vk_dump_dispatch(ex, cs, p, info, d);
}

// DispatchComputeIndirect: vkCmdDispatchIndirect from the pool buffer. The shader reads the group counts from c0 0x200,
// so they are copied there on the GPU, into a c0 range no other dispatch uses, before the dispatch.
static void op_dispatch_ind(VkExec *ex, const p_dispatch_ind *d) {
    DrawCtx *const cx = ex->dc;
    if (!ex->prog) return warn_once(W_PROGRAM, "a dispatch without a bound program is skipped");
    VkMemSpan ip;
    if (!vk_mem_resolve(d->ind, sizeof(VkDispatchIndirectCommand), &ip) || ip.offset % 4)
        return warn_once(W_INDIRECT, "indirect dispatch arguments at %#llx are not backed or not aligned: skipped", (unsigned long long)d->ind);
    cb_res(ex);
    dummies_init();
    if (aa_dispatch(ex, 0, 0, 0)) return;   // as in op_dispatch
    if (!native_tensor(ex)) { untranslated_dispatch_fallback(ex); return; }   // as in op_dispatch: never translate it
    int present;
    const GfxShader *cs = shader_for(ex, NVN_STAGE_CS, &present, NULL, 0);
    if (!cs || !native_tensor(ex)) { untranslated_dispatch_fallback(ex); return; }
    const VkPipeInfo *info = NULL;
    VkPipeline p = vk_pipeline_compute(cs, &info);
    if (!p || !info) return warn_once(W_PIPELINE, "a compute pipeline could not be created: dispatches with it are skipped");
    if ((info->pool && !pool_on()) || shader_skipped(cs)) return;   // NX2NAVI_POOL=0, NX2NAVI_SKIP_SHADERS
    vk_exec_barrier_dispatch(ex);
    VK_MEM_USE(ip);
    bind_pipeline(ex, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    cx->c0_dim[0] = cx->c0_dim[1] = cx->c0_dim[2] = 0;
    cx->B.c0_valid[2] = 0;   // a fresh c0 range: the copy below must not change one an earlier dispatch reads
    twin_dispatch = uses_tensor_ops(ex) ? 2 : 1;
    {   // the replay's binding path is the CPU cost the frame profile needs to separate from recording
        uint64_t bt = bt_now();
        bind_resources(ex, info);
        vk_exec_frame.bind_ns += (uint64_t)(bt_now() - bt);
        vk_exec_frame.bind_calls++;
    }
    twin_dispatch = 0;
    if (vk_texture_deferred()) return;   // as in op_dispatch
    if (cx->B.c0_valid[2]) {   // the shader binds c0
        // NVN2 programs read the group counts from c0 byte 0 (c0_info); at 0x200 their bank holds pushed constants
        // or the driver's tables, which the copy would overwrite
        const uint8_t *pctl, *pcode;
        uint32_t plen;
        int nvn2 = find_stage(ex->dc, ex->prog, NVN_STAGE_CS, &pctl, &pcode, &plen) && ctl_nvn2(pctl);
        VkBufferCopy cp = {ip.offset, cx->B.c0_off[2] + (nvn2 ? 0 : 0x200), sizeof(VkDispatchIndirectCommand)};
        vkCmdCopyBuffer(ex->cb, ip.buffer, cx->B.c0_buf[2], 1, &cp);
        ex->recorded++;
        vk_exec_barrier(ex);
        cx->B.c0_valid[2] = 0;   // the range no longer holds B.c0[2]
    }
    VkPassMark *m = dispatch_mark(ex, cs, 0, 0, 0);
    vkCmdDispatchIndirect(ex->cb, ip.buffer, ip.offset);
    vk_pass_mark_end(ex->cb, m);
    ex->recorded++;
    vk_mem_count(ip.cls, VK_MUSE_IND, sizeof(VkDispatchIndirectCommand));
    VK_MEM_BOUND(ip);
    if (vk_dump_armed) vk_dump_dispatch(ex, cs, p, info, &(p_dispatch){0, 0, 0, d->caller});
}

static void op_update_ub(VkExec *ex, const p_update_ub *u) {
    if (!u->size) return;
    VkMemSpan sp;
    if (!vk_mem_resolve(u->dst, u->size, &sp)) return warn_once(W_UB, "UpdateUniformBuffer to %#llx is not backed: dropped", (unsigned long long)u->dst);
    cb_res(ex);
    vk_exec_frame.ub_updates++;
    if (ex->in_pass) vk_exec_frame.ub_pass_breaks++;
    vk_exec_barrier(ex);   // earlier draws finished with the old contents
    VkBuffer b;
    VkDeviceSize o;
    memcpy(scratch(ex, u->size, &b, &o), u->data, u->size);
    VK_MEM_PICK(sp);
    VkBufferCopy cp = {o, sp.offset, u->size};
    vkCmdCopyBuffer(ex->cb, b, sp.buffer, 1, &cp);
    // In a submission's tail (vk_exec.c "the early tail") an update of a mirrored block reaches guest memory with the
    // results the game reads, from the bytes here, and takes no write-back: one after the tail would land when the
    // game is already writing the next frame's data. Not with the lazy write-back, where the block's pages may be
    // armed and a store into them would wait for the GPU (memory.c lazy_fault): the update is noted as ever there,
    // and the tail goes after the write-backs as before.
    if (vk_exec_tail() && !vk_mem_wb_lazy() && vk_mem_mirrored(u->dst, u->size))
        vk_exec_tail_ub(ex->fence, u->dst, u->data, u->size);
    else vk_mem_gpu_wrote(&sp, u->size);
    vk_usnap_drop(u->dst, u->size);
    ex->recorded++;
    vk_mem_count(sp.cls, VK_MUSE_GPU_WRITE, u->size);
    note_written(ex, u->dst, u->size);
    upd_note(ex, u);
}

// ---- bind ops

static void bind_handles(uint64_t *dst, int cap, const p_bind_handles *b) {
    if (b->stage < 0 || b->stage >= 8 || b->first < 0 || b->n < 0 || b->n > 32 || b->first + b->n > cap) {
        warn_once(W_BIND, "a bind of %d handles at %d of stage %d is out of range: ignored", b->n, b->first, b->stage);
        return;
    }
    memcpy(&dst[b->first], b->h, sizeof(uint64_t) * (size_t)b->n);
}

static void op_draw_impl(VkExec *ex, uint32_t op, const void *payload) {
    DrawCtx *const cx = ex->dc;
    switch (op) {
    case OP_BIND_PROGRAM: {
        const p_bind_program *b = payload;
        ex->prog = b->prog, ex->prog_stages = b->stages;
        cx->bind_gen++;
        break;
    }
    case OP_BIND_STATE: {
        const p_bind_state *b = payload;
        blend_st bs;
        switch (b->kind) {
        case 0:
            memcpy(&bs, b->bytes, sizeof bs);
            if (bs.target < 8) ex->pipe.blend[bs.target] = bs;
            break;
        case 1: memcpy(&ex->pipe.color, b->bytes, sizeof ex->pipe.color); break;
        case 2: memcpy(&ex->pipe.chmask, b->bytes, sizeof ex->pipe.chmask); break;
        case 3: memcpy(&ex->pipe.ms, b->bytes, sizeof ex->pipe.ms); break;
        case 4: memcpy(&ex->pipe.poly, b->bytes, sizeof ex->pipe.poly); break;
        case 5: memcpy(&ex->pipe.ds, b->bytes, sizeof ex->pipe.ds); break;
        case 6: memcpy(&ex->point_size, b->bytes, sizeof ex->point_size); break;
        case 7: memcpy(&ex->line_width, b->bytes, sizeof ex->line_width); break;
        }
        break;
    }
    case OP_BIND_VATTR: {
        const p_bind_vattr *b = payload;
        if (b->first < 0 || b->n < 0 || b->first + b->n > 16) break;
        memset(ex->pipe.vattr, 0, sizeof ex->pipe.vattr);   // the new list replaces the old one
        memcpy(&ex->pipe.vattr[b->first], b->st, sizeof(uint32_t) * (size_t)b->n);
        break;
    }
    case OP_BIND_VSTREAM: {
        const p_bind_vstream *b = payload;
        if (b->first < 0 || b->n < 0 || b->first + b->n > 16) break;
        memset(ex->pipe.vstream, 0, sizeof ex->pipe.vstream);
        memcpy(&ex->pipe.vstream[b->first], b->st, sizeof(vs_st) * (size_t)b->n);
        break;
    }
    case OP_BIND_VBUF: {
        const p_bind_vbuf *b = payload;
        if (b->first < 0 || b->n < 0 || b->first + b->n > 16) break;
        memcpy(&ex->vbuf[b->first], b->r, sizeof(NVNbufferRange) * (size_t)b->n);
        break;
    }
    case OP_BIND_UBO: {
        const p_bind_buf *b = payload;
        if (b->stage >= 0 && b->stage < 8 && b->index >= 0 && b->index < 18) ex->ubo[b->stage][b->index] = *b;
        else if (b->index >= 18 && b->index < 64) {   // NVN2 has more banks than Maxwell's 18: say which, once each
            static uint64_t seen;
            if (!(__atomic_fetch_or(&seen, 1ull << b->index, __ATOMIC_RELAXED) >> b->index & 1))
                rt_log("vk: uniform buffer bind to index %d (bank %d), stage %d, %llu bytes: beyond Maxwell's banks, dropped",
                       b->index, b->index + 3, b->stage, (unsigned long long)b->size);
        }
        cx->bind_gen++;
        break;
    }
    case OP_BIND_SSBO: {
        const p_bind_buf *b = payload;
        if (b->stage >= 0 && b->stage < 8 && b->index >= 0 && b->index < 16) ex->ssbo[b->stage][b->index] = *b;
        if (b->stage >= NVN_STAGE_TASK && b->stage <= NVN_STAGE_MESH && b->index >= 0 && b->index < 16) {   // once per slot
            static uint32_t seen;
            uint32_t bit = 1u << (16 * (b->stage - NVN_STAGE_TASK) + b->index);
            if (!(__atomic_fetch_or(&seen, bit, __ATOMIC_RELAXED) & bit))
                rt_log("vk: storage buffer bind, stage %d slot %d: %#llx, %llu bytes", b->stage, b->index,
                       (unsigned long long)b->addr, (unsigned long long)b->size);
        }
        cx->bind_gen++;
        break;
    }
    case OP_BIND_TEX: {
        const p_bind_handles *b = payload;
        bind_handles(b->stage >= 0 && b->stage < 8 ? ex->tex[b->stage] : NULL, 128, b);
        ex->tex_serial++;
        cx->bind_gen++;
        break;
    }
    case OP_BIND_SAMP: {
        const p_bind_handles *b = payload;
        bind_handles(b->stage >= 0 && b->stage < 8 ? ex->samp[b->stage] : NULL, 128, b);
        break;
    }
    case OP_BIND_IMG: {
        const p_bind_handles *b = payload;
        bind_handles(b->stage >= 0 && b->stage < 8 ? ex->img[b->stage] : NULL, 32, b);
        ex->tex_serial++;   // the shader memo's handle check (feedback_scan reads only textures: a rescan at most)
        cx->bind_gen++;
        break;
    }
    case OP_UPDATE_UB: op_update_ub(ex, payload); break;
    case OP_PUSH_CC: {
        const p_push_cc *p = payload;
        if (p->offset + (uint64_t)p->size <= sizeof ex->cs_push) memcpy(ex->cs_push + p->offset, p->data, p->size);
        cx->bind_gen++;   // storage buffers may come from these words (push_ptr)
        break;
    }
    case OP_DRAW: op_draw(ex, payload); break;
    case OP_DRAW_MESH: vk_exec_frame.draws_seen++; op_draw_mesh(ex, payload); break;
    case OP_DRAW_MESH_IND: vk_exec_frame.draws_seen++; op_draw_mesh_ind(ex, payload); break;
    case OP_DRAW_IDX: op_draw_idx(ex, payload); break;
    case OP_DISPATCH: op_dispatch(ex, payload); break;
    case OP_DRAW_IDX_IND: op_draw_idx_ind(ex, payload); break;
    case OP_DISPATCH_IND: op_dispatch_ind(ex, payload); break;
    default: rt_fatal("vk: op %s is not a draw op", nvn_op_name(op));
    }
}

// The last executed list's draws as " <position banks>/<changed banks>:<draws>" (bit n: cn), for NX2NAVI_MV_CHECK's lines.
const char *vk_mvobj_frame_masks(void) { return fm_last; }

// NX2NAVI_REPLAY_TIME (default 0, off): times each op so the frame-time summary can say how the replay splits between
// draws, binds and transfers. The clock reads cost a fraction of a millisecond a frame, which is why it is off by
// default and why the wrapper is the only thing between the walk and the switch.
static int op_is_draw(uint32_t op) {
    switch (op) {
    case OP_DRAW: case OP_DRAW_IDX: case OP_DRAW_IDX_IND:
    case OP_DISPATCH: case OP_DISPATCH_IND:
    case OP_DRAW_MESH: case OP_DRAW_MESH_IND:
    case OP_UPDATE_UB:
        return 1;
    default:
        return 0;
    }
}

void vk_exec_op_draw(VkExec *ex, uint32_t op, const void *payload) {
    if (!ex->dc) ex->dc = dc;
    static int on = -1;
    if (on < 0) on = cfg_bool("NX2NAVI_REPLAY_TIME");
    if (!on) { op_draw_impl(ex, op, payload); return; }
    uint64_t t0 = plat_now_ns();
    op_draw_impl(ex, op, payload);
    perf_replay_op_at(op_is_draw(op) ? PERF_RP_DRAW : PERF_RP_BIND, op, plat_now_ns() - t0);
}

// NX2NAVI_TRACE=perf "mem: sizes" (perf.c): the image view cache (under the texture lock), the spare descriptor pools
// and transient chunks, and the continuations held for event order.
void vk_exec_sizes(PerfSizes *s) {
    vk_texture_lock();
    uint64_t views = view_of.n, imgs = views_by_image.n;
    vk_texture_unlock();
    plat_mutex_lock(&spare_lock);
    uint64_t sp = (uint64_t)nspare_pool, sc = (uint64_t)nspare_chunk;
    plat_mutex_unlock(&spare_lock);
    perf_size(s, "image views", views, 0, PERF_SIZE_NONE);
    perf_size(s, "images with views", imgs, 0, PERF_SIZE_NONE);
    perf_size(s, "spare descriptor pools", sp, SPARE_POOLS, PERF_SIZE_NONE);
    perf_size(s, "spare transient chunks", sc, SPARE_CHUNKS, sc * CHUNK_BYTES);
    perf_size(s, "held continuations", (uint64_t)vk_exec_held(), 36, PERF_SIZE_NONE);
}

// The memory report's descriptor pools (vk_memory.c outside_log): the replay's (alloc_set, REPLAY_POOL_SETS sets each),
// the spare ones among them, and the texture pool's sets. Their memory is the driver's, outside VMA, and has no size.
void vk_exec_desc_mem(VkDescMem *d) {
    plat_mutex_lock(&spare_lock);
    d->spare = (uint64_t)nspare_pool;
    plat_mutex_unlock(&spare_lock);
    int64_t live = __atomic_load_n(&npools_live, __ATOMIC_RELAXED);
    d->pools = live > 0 ? (uint64_t)live : 0;
    d->sets_each = REPLAY_POOL_SETS;
    d->tex_sets = PL.dpool ? (uint64_t)PL.nsets : 0;
}
