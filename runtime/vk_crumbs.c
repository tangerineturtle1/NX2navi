// GPU breadcrumbs for a lost device (vk.h vk_crumb). VK_AMD_buffer_marker writes a 32-bit code into a buffer from
// the command stream: at TOP_OF_PIPE once the command processor reaches the marker, at BOTTOM_OF_PIPE once every
// command before it has finished. Each crumb writes both, into two words of one host-coherent buffer, so after a
// device loss word 0 names the newest point the GPU reached and word 1 the newest point it had finished: the hang lies
// between them. A code is the crumb kind in the top 4 bits and its number in the low 28. The markers cost two small
// writes per crumb, and crumbs mark submissions, upload batches, ASTC chunks and presents, not draws.
//
// Pass marks (vk.h vk_pass_mark_begin) use words 2 and 3 of the same buffer: the replay marks the start and the end of
// every render pass and dispatch, and a ring of records says what each mark was. The design: a mark
// per pass and dispatch, numbers in a ring of 4096, text only at the device loss.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "vk.h"

// vk_pass_mark_cmd also runs on the recorder thread (vk_rec.c R_PassMark), where vk.h's macro would turn the timestamp
// into a new packet for the part being recorded: a second producer on the ring, which corrupts it ("vk_rec: unknown
// packet" with NX2NAVI_PASS_TIMES=1 on NVIDIA, where the recorder is on). The command goes straight into cb.
#undef vkCmdWriteTimestamp

static PFN_vkCmdWriteBufferMarkerAMD write_marker;
static VkBuffer buf;
static VmaAllocation alloc;
static volatile uint32_t *words;   // 0, 1: crumbs; 2, 3: pass marks; 4, 5: draw marks

// Pass marks (below)
int vk_pass_marks_on;
static int pm_amd;                          // marks into words 2 and 3
static PFN_vkCmdSetCheckpointNV set_checkpoint;
static void pass_marks_init(VkCtx *c);
static void pass_times_init(VkCtx *c);
static void pass_times_poll(void);
static VkQueryPool pt_pool;           // pass times (below): two timestamps per ring slot
static volatile uint8_t pt_timed[VK_PASS_MARKS];   // the slot's mark writes timestamps; cleared once they are read


static void buffer_init(VkCtx *c) {
    write_marker = (PFN_vkCmdWriteBufferMarkerAMD)vk_dpa(c->device, "vkCmdWriteBufferMarkerAMD");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 24,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    VmaAllocationInfo info;
    if (!write_marker || vmaCreateBuffer(c->vma, &bi, &ai, &buf, &alloc, &info) != VK_SUCCESS) {
        rt_log("vk: GPU breadcrumbs unavailable");
        write_marker = NULL;
        return;
    }
    words = info.pMappedData;
    words[0] = words[1] = words[2] = words[3] = words[4] = words[5] = 0;
    rt_log("vk: GPU breadcrumbs on (VK_AMD_buffer_marker)");
}

void vk_crumbs_init(VkCtx *c) {
    if (c->has_buffer_marker) buffer_init(c);
    pass_marks_init(c);
    pass_times_init(c);
}

void vk_crumbs_release(void) {
    VkCtx *c = vk_ctx();
    if (buf && c) vmaDestroyBuffer(c->vma, buf, alloc);
    if (pt_pool && c) vkDestroyQueryPool(c->device, pt_pool, NULL);
    pt_pool = VK_NULL_HANDLE;
    buf = VK_NULL_HANDLE, alloc = NULL, words = NULL, write_marker = NULL;
    vk_pass_marks_on = pm_amd = vk_draw_marks_on = 0, set_checkpoint = NULL;
}

void vk_crumb(VkCommandBuffer cb, int kind, uint64_t n) {
    if (!write_marker || !cb) return;
    uint32_t code = (uint32_t)kind << 28 | (uint32_t)(n & 0x0fffffff);
    write_marker(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, buf, 0, code);
    write_marker(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, buf, 4, code);
}

static void describe(uint32_t code, char *o, size_t size) {
    static const char *const kinds[] = {"nothing", "the start of a part of submission", "the end of submission",
                                        "the start of upload batch", "the end of upload batch",
                                        "ASTC decode chunk", "the start of present",
                                        "the end of present"};
    unsigned k = code >> 28;
    if (!code) snprintf(o, size, "nothing yet");
    else if (k < sizeof kinds / sizeof *kinds) snprintf(o, size, "%s %u", kinds[k], code & 0x0fffffff);
    else snprintf(o, size, "unknown code %#x", code);
}

void vk_crumbs_report(void) {
    if (!words) return;
    char reached[96], finished[96];
    describe(words[0], reached, sizeof reached);
    describe(words[1], finished, sizeof finished);
    rt_log("vk: GPU breadcrumbs: the GPU last reached %s; everything before %s had finished", reached, finished);
}

// ---- pass marks

// Cost per pass or dispatch: about twenty stores into the ring and two commands, one at the start and one at the end,
// each a marker write or a checkpoint. Per draw nothing, except that the branch of vk_exec_draw.c prepare_draw that
// records changed pipeline state also notes the shaders of a pass's first draw. A full barrier already precedes every
// pass and dispatch (vk_exec.c vk_exec_pass_begin, vk_exec_draw.c op_dispatch), so the end-of-pipe write adds no wait.
static VkPassMark ring[VK_PASS_MARKS];
static uint32_t seq;                        // marks made; only the replay thread writes it

// NX2NAVI_PASS_CRUMBS: unset, buffer marks where VK_AMD_buffer_marker exists; 1, also NVIDIA checkpoints; 0, neither.
int vk_pass_marks_want_nv(void) {
    const char *e = cfg_get("NX2NAVI_PASS_CRUMBS");
    return e && *e && strcmp(e, "0") != 0;
}

static void pass_marks_init(VkCtx *c) {
    const char *e = cfg_get("NX2NAVI_PASS_CRUMBS");
    int off = e && !strcmp(e, "0");
    set_checkpoint = c->has_nv_checkpoints
                         ? (PFN_vkCmdSetCheckpointNV)vk_dpa(c->device, "vkCmdSetCheckpointNV") : NULL;
    pm_amd = !off && write_marker && words;
    // Timestamp profiling needs pass labels, but no vendor breadcrumb extension.
    const char *timing = cfg_get("NX2NAVI_GPU_PASSES");
    int timestamps = timing && *timing && strcmp(timing, "0");
    vk_pass_marks_on = pm_amd || set_checkpoint || timestamps;
    if (timestamps && !pm_amd && !set_checkpoint)
        rt_log("vk: pass labels on for timestamp profiling; vendor breadcrumbs unavailable");
    else if (vk_pass_marks_on)
        rt_log("vk: pass marks on (%s%s%s; NX2NAVI_PASS_CRUMBS=0 turns them off%s)", pm_amd ? "VK_AMD_buffer_marker" : "",
               pm_amd && set_checkpoint ? " and " : "", set_checkpoint ? "VK_NV_device_diagnostic_checkpoints" : "",
               set_checkpoint ? "" : ", =1 adds NVIDIA checkpoints where the driver has them");
    else
        rt_log("vk: pass marks off (%s)", off ? "NX2NAVI_PASS_CRUMBS=0"
                                              : "no VK_AMD_buffer_marker; NX2NAVI_PASS_CRUMBS=1 asks for NVIDIA checkpoints");
    vk_draw_marks_on = pm_amd && cfg_bool("NX2NAVI_DRAW_CRUMBS");
    if (vk_draw_marks_on)
        rt_log("vk: draw marks on (NX2NAVI_DRAW_CRUMBS): a mark at every change of shaders inside a pass, so a lost "
               "device names the shaders of the draws the GPU had not finished");
    else if (cfg_bool("NX2NAVI_DRAW_CRUMBS"))
        rt_log("vk: draw marks off: NX2NAVI_DRAW_CRUMBS needs the pass marks of VK_AMD_buffer_marker");
}

// ---- draw marks (NX2NAVI_DRAW_CRUMBS=1, off by default; with VK_AMD_buffer_marker only)
// A pass mark names the pass the GPU was in and the shaders of its first and last draw. The losses reported on RDNA 2
// cards end inside one scene pass of 30 to 100 draws, and which draw the GPU never finished is not known. With this
// on, the replay writes a mark each time the shaders or the pipeline state change inside a pass (vk_exec_draw.c
// prepare_draw_ex), into words 4 and 5 of the breadcrumb buffer as a pass mark goes into words 2 and 3, and a ring
// keeps the pass, the draw's index in it and the two shaders of each. After a loss word 5 names the last mark before
// which every draw had finished: the draw that hung was recorded after it, with its shaders or those of one of the few
// marks up to the one in word 4. It costs two marker writes per change, a few thousand a frame, so it is for the
// player who can reproduce a loss, not for everyone.
// The marks are numbered as they are recorded, by the replay thread and by its workers when a pass's chunks are
// recorded side by side (vk_exec.c): the number is taken atomically and each number has its own slot of the ring, so
// no two threads write one. The two marks the GPU names are exact either way. The numbers between them are in queue
// order only where one thread recorded the pass; with workers some of them belong to chunks that ran earlier, which
// the report says.
#define VK_DRAW_MARKS 8192   // power of two
typedef struct { uint32_t id, pass, draw; const GfxShader *vs, *fs; } DrawMark;
static DrawMark dring[VK_DRAW_MARKS];
static volatile uint32_t dseq;   // draw marks made; any recording thread takes the next number
int vk_draw_marks_on;

void vk_draw_mark(VkCommandBuffer cb, const VkPassMark *m, const GfxShader *vs, const GfxShader *fs, uint32_t draw) {
    if (!vk_draw_marks_on || !cb) return;
    uint32_t id = __atomic_add_fetch(&dseq, 1, __ATOMIC_RELAXED);
    if (!id) id = __atomic_add_fetch(&dseq, 1, __ATOMIC_RELAXED);   // 0 means no mark
    dring[id & (VK_DRAW_MARKS - 1)] = (DrawMark){id, m ? m->id : 0, draw, vs, fs};
    rec_pass_mark(cb, id, 2);
}

void vk_pass_mark_cmd(VkCommandBuffer cb, uint32_t id, int end) {
    if (end == 2) {   // a draw mark (vk_draw_mark): the same two points, into its own words
        if (!pm_amd) return;
        write_marker(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, buf, 16, id);
        write_marker(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, buf, 20, id);
        return;
    }
    if (pt_pool && pt_timed[id & (VK_PASS_MARKS - 1)] == 1)
        vkCmdWriteTimestamp(cb, end ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pt_pool,
                            (id & (VK_PASS_MARKS - 1)) * 2 + (end != 0));
    if (pm_amd)
        write_marker(cb, end ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, buf,
                     end ? 12 : 8, id);
    if (set_checkpoint) set_checkpoint(cb, (const void *)(uintptr_t)((uint64_t)id << 1 | (end != 0)));
}

// ---- NX2NAVI_GPU_PASSES=1 (diagnosis, needs the pass marks): every GP_PERIOD presents the render passes and dispatches
// of GP_FRAMES frames get GPU timestamps around them. Some presents later the results are summed by what the pass is
// (targets, shaders) and the most expensive are logged in ms per frame. The timestamps run on the replay's command
// buffers, through the recorder like the marks.
#define GP_SLOTS 16384
#define GP_PERIOD 240
#define GP_FRAMES 4
#define GP_LATER 30   // presents after the capture before the results are read: the GPU is done with them by then
#define GP_TOP 30
typedef struct { uint8_t kind, used; uint32_t w, h, d, draws, layer; int c0; uint64_t vs, fs; } GpLabel;
static GpLabel gp_labels[GP_SLOTS];
// The gap ledger: what the timed frames' parts spend outside their passes and dispatches. A part (vk_sync.c: one command
// buffer of a submission) takes a slot of its own, kind GP_KIND_PART, with a timestamp at its start and its end. A gap
// runs from the part's start or the end of a mark to the start of the next mark or the part's end; vk_gpu_gap_note
// names the kinds of commands the replay records in the gap it is in (copies, clears, the upscaler, the write-back
// merge). A gap nobody noted holds barriers and state only. The report sums the gaps by the kinds noted in them. A pass
// split over two parts (vk_sync_split) keeps its own time, the wait for the second part included, and makes no gap.
#define GP_KIND_PART 3   // beside VK_MARK_PASS and VK_MARK_DISPATCH
#define GP_GAPS 16384
typedef struct { uint32_t from, to, kinds; } GpGap;   // query numbers in gp_pool; kinds: bits of VK_GAP_*
static GpGap gp_gaps[GP_GAPS];
static uint32_t gp_ngaps, gp_gap_from = UINT32_MAX, gp_gap_kinds, gp_part = UINT32_MAX;
static int gp_in_mark;                                // between a mark's start and its end
static uint64_t gp_kind_n[VK_GAP_N], gp_kind_units[VK_GAP_N];
static const char *const gp_kind_names[VK_GAP_N] = {
    "buffer copies", "buffer to texture copies", "texture to buffer copies", "texture copies", "buffer fills",
    "texture clears", "target clears", "the temporal upscaler", "write-back merges", "write-back copies",
    "depth twin copies", "texture pool marks", "mesh draw counts", "block-compressed target copies"};
static VkQueryPool gp_pool, gp_prims;   // gp_prims: mesh primitives generated per pass (with meshShaderQueries)
static VkQueryPool gp_stats;            // fragment shader invocations per pass
static int gp_state, gp_phase, gp_capture;   // state 0 not set up, 1 on, -1 off; phase 0 idle, 1 capturing, 2 waiting
static uint32_t gp_n;
static uint64_t gp_start;

static unsigned long long shader_id(const GfxShader *s);
uint32_t vk_exec_mesh_counts(const uint32_t **out);   // vk_exec_draw.c

// The hash NX2NAVI_SKIP_SHADERS takes and the frame dump prints: FNV-1a over the shader's SPIR-V.
static uint64_t gp_shader_hash(const GfxShader *s) {
    static struct { const GfxShader *s; uint64_t h; } memo[1024];
    if (!s) return 0;
    uint32_t i = (uint32_t)(((uintptr_t)s * 0x9E3779B97F4A7C15ull) >> 54);
    if (memo[i].s == s) return memo[i].h;
    uint64_t h = gfx_shader_spirv_fnv(s);   // kept by the shader: nothing is decoded here
    memo[i].s = s, memo[i].h = h;
    return h;
}

static int kind_is_pass(const VkPassMark *m) { return m->kind == VK_MARK_PASS; }

// Result points (gpu_passes): where in a submission's GPU time the results the CPU waits for are done. The replay
// notes a result (vk_exec_note_results: an event, a counter or occlusion report, a GPU write of unmirrored memory, a
// mirror write-back) as it records it; the next timestamp the capture writes after it (a pass's or dispatch's start or
// end, a part's end) is when the GPU is done with it at the latest. The report compares that, for the submissions the
// game's wait after a frame's first submission waited for (vk_sync_recorded_wait), with the submission's end: how much
// earlier the wait could end if it ended at the results instead of at the whole submission's completion.
#define GP_RES 16384
typedef struct { uint64_t fence; uint32_t q; uint8_t kind; } GpRes;   // q: query in gp_pool, UINT32_MAX until written
static GpRes gp_res[GP_RES];
static uint32_t gp_nres, gp_res_open;
static uint64_t gp_fence, gp_part_fence[GP_SLOTS];
#define GP_WAITED 256
static uint64_t gp_waited[GP_WAITED];
static uint32_t gp_nwaited;   // atomic: the game's threads add

static void gp_resolve(uint32_t q) {
    for (uint32_t i = gp_res_open; i < gp_nres; i++) gp_res[i].q = q;
    gp_res_open = gp_nres;
}

void vk_gpu_passes_fence(uint64_t fence) { gp_fence = fence; }

void vk_gpu_passes_result(int kinds) {
    if (!gp_capture || gp_part == UINT32_MAX) return;
    for (int b = 0; b < VK_RES_KINDS && gp_nres < GP_RES; b++)
        if (kinds >> b & 1) gp_res[gp_nres++] = (GpRes){gp_fence, UINT32_MAX, (uint8_t)b};
}

void vk_gpu_passes_waited(uint64_t need) {
    if (!gp_capture) return;
    uint32_t i = __atomic_fetch_add(&gp_nwaited, 1, __ATOMIC_RELAXED);
    if (i < GP_WAITED) __atomic_store_n(&gp_waited[i], need, __ATOMIC_RELEASE);
}

int vk_gpu_passes_capturing(void) { return gp_capture; }

static void gp_gap_close(uint32_t to) {
    if (gp_gap_from != UINT32_MAX && gp_ngaps < GP_GAPS) gp_gaps[gp_ngaps++] = (GpGap){gp_gap_from, to, gp_gap_kinds};
    gp_gap_from = UINT32_MAX, gp_gap_kinds = 0;
}

// The kinds of commands noted since the last mark ended (bits of VK_GAP_*), for a lost device's report: the next mark
// takes them as the commands recorded before it (VkPassMark.gap). Replay thread only, as the notes are.
static uint32_t gap_now;

void vk_gpu_gap_note(int kind, uint64_t units) {
    if (kind >= 0 && kind < VK_GAP_N) gap_now |= 1u << kind;
    if (!gp_capture || gp_part == UINT32_MAX || kind < 0 || kind >= VK_GAP_N) return;
    gp_gap_kinds |= 1u << kind;
    gp_kind_n[kind]++, gp_kind_units[kind] += units;
}

void vk_gpu_passes_part(VkCommandBuffer cb, int end) {
    if (!end) {
        gp_part = UINT32_MAX, gp_gap_from = UINT32_MAX, gp_gap_kinds = 0;
        if (!gp_capture || gp_n >= GP_SLOTS) return;
        gp_labels[gp_n] = (GpLabel){.kind = GP_KIND_PART};
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gp_pool, gp_n * 2);
        gp_part_fence[gp_n] = gp_fence;
        gp_part = gp_n++;
        if (!gp_in_mark) gp_gap_from = gp_part * 2;
        return;
    }
    if (gp_part == UINT32_MAX) return;
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gp_pool, gp_part * 2 + 1);
    gp_resolve(gp_part * 2 + 1);
    if (!gp_in_mark) gp_gap_close(gp_part * 2 + 1);
    gp_labels[gp_part].used = 1;
    gp_part = UINT32_MAX, gp_gap_from = UINT32_MAX;
}

static void gp_begin(VkCommandBuffer cb, VkPassMark *m) {
    m->gp = 0;
    if (!gp_capture || gp_n >= GP_SLOTS) return;
    if (gp_part != UINT32_MAX) gp_gap_close(gp_n * 2);   // the gap before this mark ends at its start
    gp_labels[gp_n].used = 0;
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gp_pool, gp_n * 2);
    gp_resolve(gp_n * 2);
    if (gp_prims && kind_is_pass(m)) vkCmdBeginQuery(cb, gp_prims, gp_n, 0);
    if (gp_stats && kind_is_pass(m)) vkCmdBeginQuery(cb, gp_stats, gp_n, 0);
    m->gp = ++gp_n;
}

static void gp_end(VkCommandBuffer cb, const VkPassMark *m) {
    if (!m->gp) return;
    uint32_t i = m->gp - 1;
    if (gp_prims && kind_is_pass(m)) vkCmdEndQuery(cb, gp_prims, i);
    if (gp_stats && kind_is_pass(m)) vkCmdEndQuery(cb, gp_stats, i);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gp_pool, i * 2 + 1);
    gp_resolve(i * 2 + 1);
    GpLabel *l = &gp_labels[i];
    l->kind = m->kind, l->w = m->w, l->h = m->h, l->d = m->d, l->draws = m->draws == UINT32_MAX ? 0 : m->draws;
    l->layer = m->layer;
    l->c0 = m->kind == VK_MARK_PASS && m->ncolor ? (int)m->color[0] : 0;
    l->vs = gp_shader_hash(m->first[0]), l->fs = m->kind == VK_MARK_PASS ? gp_shader_hash(m->first[1]) : 0;
    l->used = 1;
    if (gp_part != UINT32_MAX) gp_gap_from = i * 2 + 1, gp_gap_kinds = 0;   // the next gap starts at this mark's end
}

// One timestamp of gp_pool in ticks; 0 when it is not there.
static int gp_tick(VkCtx *c, uint32_t q, uint64_t *t) {
    uint64_t v[2] = {0};
    if (vkGetQueryPoolResults(c->device, gp_pool, q, 1, sizeof v, v, sizeof v,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != VK_SUCCESS || !v[1])
        return 0;
    *t = v[0];
    return 1;
}

// The gaps of the timed frames, summed by the kinds noted in them, and the parts' own time for comparison with the
// perf line's GPU time (which counts the mirror prologues too and leaves out the time between parts).
static void gp_report_gaps(double tick_ms, double parts_ms, uint32_t parts) {
    enum { MAX_SETS = 64 };
    static struct { uint32_t kinds, n; double ms; } sets[MAX_SETS];
    VkCtx *c = vk_ctx();
    int n = 0;
    double total = 0;
    uint32_t read = 0;
    for (uint32_t i = 0; i < gp_ngaps; i++) {
        uint64_t a, b;
        if (!gp_tick(c, gp_gaps[i].from, &a) || !gp_tick(c, gp_gaps[i].to, &b) || b < a) continue;
        double ms = (double)(b - a) * tick_ms;
        int s = 0;
        while (s < n && sets[s].kinds != gp_gaps[i].kinds) s++;
        if (s == n) {
            if (n == MAX_SETS) continue;
            sets[n].kinds = gp_gaps[i].kinds, sets[n].n = 0, sets[n].ms = 0, n++;
        }
        sets[s].ms += ms, sets[s].n++, total += ms, read++;
    }
    rt_log("gpu passes:   outside the passes and dispatches: %.2f ms per frame in %u gaps; the parts themselves %.2f ms per "
           "frame in %u parts", total / GP_FRAMES, read, parts_ms / GP_FRAMES, parts);
    for (int k = 0; k < n && k < 12; k++) {   // the most expensive sets first
        int top = k;
        for (int j = k + 1; j < n; j++)
            if (sets[j].ms > sets[top].ms) top = j;
        if (top != k) {
            uint32_t tk = sets[k].kinds, tn = sets[k].n;
            double tm = sets[k].ms;
            sets[k].kinds = sets[top].kinds, sets[k].n = sets[top].n, sets[k].ms = sets[top].ms;
            sets[top].kinds = tk, sets[top].n = tn, sets[top].ms = tm;
        }
        char what[256];
        int o = 0;
        what[0] = 0;
        for (int b = 0; b < VK_GAP_N && o < (int)sizeof what - 40; b++)
            if (sets[k].kinds >> b & 1) o += snprintf(what + o, sizeof what - (size_t)o, "%s%s", o ? ", " : "", gp_kind_names[b]);
        rt_log("gpu passes:     %7.3f ms  x%-5.1f gaps with %s", sets[k].ms / GP_FRAMES, (double)sets[k].n / GP_FRAMES,
               o ? what : "barriers and state only");
    }
    for (int b = 0; b < VK_GAP_N; b++)
        if (gp_kind_n[b])
            rt_log("gpu passes:     %s: %.1f per frame, %.2f M units per frame (bytes of buffers, texels of images)",
                   gp_kind_names[b], (double)gp_kind_n[b] / GP_FRAMES, gp_kind_units[b] / 1e6 / GP_FRAMES);
}

// The result points of the timed frames (vk_gpu_passes_result), for the submissions the game's wait waited for.
static void gp_report_results(double tick_ms) {
    VkCtx *c = vk_ctx();
    static const char *const names[VK_RES_KINDS] = {"events", "counter reports", "occlusion reports",
                                                     "unmirrored writes", "mirror write-backs"};
    uint32_t nw = __atomic_load_n(&gp_nwaited, __ATOMIC_ACQUIRE);
    if (nw > GP_WAITED) nw = GP_WAITED;
    uint64_t seen[GP_WAITED];
    uint32_t ns = 0, subs = 0;
    double span = 0, last[VK_RES_KINDS] = {0}, cnt[VK_RES_KINDS] = {0}, mask_end = 0, all_end = 0;
    int mask = vk_sync_result_mask();
    for (uint32_t w = 0; w < nw; w++) {
        uint64_t f = __atomic_load_n(&gp_waited[w], __ATOMIC_ACQUIRE);
        int dup = !f;
        for (uint32_t k = 0; k < ns && !dup; k++) dup = seen[k] == f;
        if (dup) continue;
        seen[ns++] = f;
        uint64_t t0 = UINT64_MAX, t1 = 0, t;
        for (uint32_t i = 0; i < gp_n; i++) {   // the submission's parts: its first start, its last end
            if (gp_labels[i].kind != GP_KIND_PART || !gp_labels[i].used || gp_part_fence[i] != f) continue;
            if (gp_tick(c, i * 2, &t) && t < t0) t0 = t;
            if (gp_tick(c, i * 2 + 1, &t) && t > t1) t1 = t;
        }
        if (t0 == UINT64_MAX || t1 <= t0) continue;
        uint64_t lk[VK_RES_KINDS] = {0};
        int any = 0;
        for (uint32_t i = 0; i < gp_nres; i++)
            if (gp_res[i].fence == f && gp_res[i].q != UINT32_MAX && gp_tick(c, gp_res[i].q, &t) && t >= t0) {
                if (t > lk[gp_res[i].kind]) lk[gp_res[i].kind] = t;
                any = 1;
            }
        if (!any) continue;
        subs++;
        span += (double)(t1 - t0) * tick_ms;
        uint64_t me = t0, ae = t0;
        for (int b = 0; b < VK_RES_KINDS; b++) {
            if (!lk[b]) continue;
            last[b] += (double)(lk[b] - t0) * tick_ms, cnt[b]++;
            if (lk[b] > ae) ae = lk[b];
            if (mask >> b & 1 && lk[b] > me) me = lk[b];
        }
        mask_end += (double)(me - t0) * tick_ms, all_end += (double)(ae - t0) * tick_ms;
    }
    if (!subs) {
        rt_log("gpu passes:   result points: none of the %u waited-for submissions was timed with its results", ns);
        return;
    }
    char list[400];
    int o = 0;
    for (int b = 0; b < VK_RES_KINDS && o < (int)sizeof list - 80; b++)
        if (cnt[b])
            o += snprintf(list + o, sizeof list - (size_t)o, "%s%s %.2f ms (%.0f of %u)", o ? ", " : "", names[b],
                          last[b] / cnt[b], cnt[b], subs);
    list[o] = 0;
    rt_log("gpu passes:   result points: %u submissions the game's wait waited for, %.2f ms of GPU time each from their "
           "start to their end; their last results done at: %s", subs, span / subs, list);
    rt_log("gpu passes:   result points: the wait could end %.2f ms before the submission's end (the kinds it counts, "
           "%#x), %.2f ms with every kind", (span - mask_end) / subs, mask, (span - all_end) / subs);
}

typedef struct { GpLabel l; double ms; uint32_t n; uint64_t draws, prims, frags; } GpSum;

static int gp_sum_cmp(const void *a, const void *b) {
    double x = ((const GpSum *)a)->ms, y = ((const GpSum *)b)->ms;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void gp_report(uint64_t frame) {
    enum { MAX_SUMS = 2048 };
    static GpSum sums[MAX_SUMS];
    VkCtx *c = vk_ctx();
    int n = 0;
    uint32_t read = 0, unread = 0;
    double tick_ms = c->props.limits.timestampPeriod / 1e6, total = 0, parts_ms = 0;
    uint32_t parts = 0;
    for (uint32_t i = 0; i < gp_n; i++) {
        const GpLabel *l = &gp_labels[i];
        uint64_t q[4] = {0};
        if (!l->used || vkGetQueryPoolResults(c->device, gp_pool, i * 2, 2, sizeof q, q, 2 * sizeof q[0],
                                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != VK_SUCCESS ||
            !q[1] || !q[3]) {
            if (l->kind != GP_KIND_PART) unread++;
            continue;
        }
        double ms = (double)(q[2] - q[0]) * tick_ms;
        if (l->kind == GP_KIND_PART) {   // a part of a submission: for the gap ledger's line only
            parts_ms += ms, parts++;
            continue;
        }
        read++;
        total += ms;
        // a pass of many draws and its neighbours differ in the draw count: the sum is by targets and shaders alone
        int s = 0;
        while (s < n && !(sums[s].l.kind == l->kind && sums[s].l.w == l->w && sums[s].l.h == l->h && sums[s].l.c0 == l->c0 && sums[s].l.layer == l->layer &&
                          sums[s].l.vs == l->vs && sums[s].l.fs == l->fs))
            s++;
        if (s == n) {
            if (n == MAX_SUMS) continue;
            sums[n++] = (GpSum){*l, 0, 0, 0, 0, 0};
        }
        uint64_t pr[2] = {0};
        if (gp_prims && l->kind == VK_MARK_PASS &&
            vkGetQueryPoolResults(c->device, gp_prims, i, 1, sizeof pr, pr, sizeof pr,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_SUCCESS && pr[1])
            sums[s].prims += pr[0];
        uint64_t fr[2] = {0};
        if (gp_stats && l->kind == VK_MARK_PASS &&
            vkGetQueryPoolResults(c->device, gp_stats, i, 1, sizeof fr, fr, sizeof fr,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_SUCCESS && fr[1])
            sums[s].frags += fr[0];
        sums[s].ms += ms, sums[s].n++, sums[s].draws += l->draws;
    }
    qsort(sums, (size_t)n, sizeof sums[0], gp_sum_cmp);
    rt_log("gpu passes: frame %llu: %.2f ms per frame in %u passes and dispatches over %d frames (%u not read, %d kinds)",
           (unsigned long long)frame, total / GP_FRAMES, read, GP_FRAMES, unread, n);
    {   // the mesh workgroup counts of the indirect mesh draws (vk_exec_draw.c mesh_ring_entry)
        const uint32_t *mc;
        uint32_t mn = vk_exec_mesh_counts(&mc);
        uint64_t sum = 0;
        uint32_t top = 0;
        char list[200];
        int o = 0;
        for (uint32_t i = 0; i < mn; i++) {
            sum += mc[i];
            if (mc[i] > top) top = mc[i];
            if (i < 18 && o < 180) o += snprintf(list + o, sizeof list - (size_t)o, " %u", mc[i]);
        }
        list[o] = 0;
        rt_log("gpu passes:   indirect mesh draws: %.1f per frame, %.0f workgroups per frame, largest %u; first:%s", (double)mn / GP_FRAMES,
               (double)sum / GP_FRAMES, top, list);
    }
    gp_report_gaps(tick_ms, parts_ms, parts);
    gp_report_results(tick_ms);
    for (int s = 0; s < n && s < GP_TOP; s++) {
        const GpSum *e = &sums[s];
        if (e->l.kind == VK_MARK_DISPATCH)
            rt_log("gpu passes:   %7.3f ms  x%-5.1f dispatch %ux%ux%u cs %016llx", e->ms / GP_FRAMES, (double)e->n / GP_FRAMES,
                   e->l.w, e->l.h, e->l.d, (unsigned long long)e->l.vs);
        else
            rt_log("gpu passes:   %7.3f ms  x%-5.1f pass %ux%u layer %u fmt %d, %.1f draws, %.2fM fragments, %.2fM mesh prims, vs %016llx fs %016llx", e->ms / GP_FRAMES,
                   (double)e->n / GP_FRAMES, e->l.w, e->l.h, e->l.layer, e->l.c0, (double)e->draws / GP_FRAMES, e->frags / 1e6 / GP_FRAMES, e->prims / 1e6 / GP_FRAMES,
                   (unsigned long long)e->l.vs, (unsigned long long)e->l.fs);
    }
}

// NX2NAVI_SET_AT=<frame>:<key>=<value>[;...]: settings changed by a test run while the game plays, as the menu would.
static void set_at_frame(uint64_t frame) {
    static char buf[512];
    static const char *next;
    static int state;
    if (!state) {
        const char *e = cfg_get("NX2NAVI_SET_AT");
        state = e && *e ? 1 : -1;
        if (state > 0) snprintf(buf, sizeof buf, "%s", e), next = buf;
    }
    while (state > 0 && next && *next && strtoull(next, NULL, 10) <= frame) {
        char item[128], *eq;
        const char *colon = strchr(next, ':'), *end = strchr(next, ';');
        if (!colon) { state = -1; return; }
        snprintf(item, sizeof item, "%.*s", end ? (int)(end - colon - 1) : (int)strlen(colon + 1), colon + 1);
        if ((eq = strchr(item, '='))) {
            *eq = 0;
            rt_log("set-at: frame %llu: %s = %s", (unsigned long long)frame, item, eq + 1);
            cfg_set(item, eq + 1);
        }
        next = end ? end + 1 : NULL;
    }
}

void vk_gpu_passes_frame(uint64_t frame) {
    set_at_frame(frame);
    if (gp_state < 0) return;
    VkCtx *c = vk_ctx();
    if (!gp_state) {
        gp_state = -1;
        const char *e = cfg_get("NX2NAVI_GPU_PASSES");
        if (!e || !*e || !strcmp(e, "0") || !c) return;
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                    .queryCount = GP_SLOTS * 2};
        if (!vk_pass_marks_on || vkCreateQueryPool(c->device, &qi, NULL, &gp_pool) != VK_SUCCESS) {
            rt_log("gpu passes: not available (needs the pass marks and a timestamp pool)");
            return;
        }
        if (c->has_mesh_queries) {
            VkQueryPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                        .queryType = VK_QUERY_TYPE_MESH_PRIMITIVES_GENERATED_EXT, .queryCount = GP_SLOTS};
            if (vkCreateQueryPool(c->device, &pi, NULL, &gp_prims) != VK_SUCCESS) gp_prims = VK_NULL_HANDLE;
        }
        {
            VkQueryPoolCreateInfo si = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS,
                                        .queryCount = GP_SLOTS,
                                        .pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT};
            if (vkCreateQueryPool(c->device, &si, NULL, &gp_stats) != VK_SUCCESS) gp_stats = VK_NULL_HANDLE;
        }
        gp_state = 1;
        rt_log("gpu passes: timing %d frames every %d presents (NX2NAVI_GPU_PASSES)", GP_FRAMES, GP_PERIOD);
    }
    if (gp_phase == 0) {
        if (frame % GP_PERIOD) return;
        vkResetQueryPool(c->device, gp_pool, 0, GP_SLOTS * 2);
        if (gp_prims) vkResetQueryPool(c->device, gp_prims, 0, GP_SLOTS);
        if (gp_stats) vkResetQueryPool(c->device, gp_stats, 0, GP_SLOTS);
        gp_ngaps = 0, gp_gap_from = UINT32_MAX, gp_gap_kinds = 0, gp_part = UINT32_MAX;
        memset(gp_kind_n, 0, sizeof gp_kind_n), memset(gp_kind_units, 0, sizeof gp_kind_units);
        gp_nres = gp_res_open = 0;
        __atomic_store_n(&gp_nwaited, 0, __ATOMIC_RELEASE);
        gp_n = 0, gp_start = frame, gp_capture = 1, gp_phase = 1;
    } else if (gp_phase == 1) {
        if (frame < gp_start + GP_FRAMES) return;
        gp_capture = 0, gp_phase = 2;
    } else if (frame >= gp_start + GP_FRAMES + GP_LATER) {
        gp_report(frame);
        gp_phase = 0;
    }
}

VkPassMark *vk_pass_mark_begin(VkCommandBuffer cb, int kind, uint64_t fence) {
    if (!vk_pass_marks_on || !cb) return NULL;
    if (pt_pool) pass_times_poll();
    uint32_t id = ++seq;
    if (!id) id = ++seq;   // 0 means no mark
    VkPassMark *m = &ring[id & (VK_PASS_MARKS - 1)];
    m->id = id, m->kind = (uint8_t)kind, m->fence = fence, m->draws = UINT32_MAX;
    m->gap = gap_now, gap_now = 0;   // what was recorded between the mark before and this one
    // timed unless the slot's previous mark is still waiting to be read (its queries are not reset yet)
    if (pt_pool) pt_timed[id & (VK_PASS_MARKS - 1)] = !pt_timed[id & (VK_PASS_MARKS - 1)] ? 1 : 2;
    rec_pass_mark(cb, id, 0);
    gp_begin(cb, m);
    gp_in_mark = 1;
    return m;
}

void vk_pass_mark_end(VkCommandBuffer cb, const VkPassMark *m) {
    if (!m || !cb) return;
    gp_in_mark = 0;
    gp_end(cb, m);
    rec_pass_mark(cb, m->id, 1);
    gap_now = 0;   // the gap after this mark starts empty
}

int vk_pass_marks_read(uint32_t *started, uint32_t *finished) {
    if (!pm_amd) return 0;
    *started = words[2], *finished = words[3];
    return 1;
}

const VkPassMark *vk_pass_mark_record(uint32_t id) {
    const VkPassMark *m = &ring[id & (VK_PASS_MARKS - 1)];
    return id && m->id == id ? m : NULL;
}

// The first 8 bytes of the shader's input id (gfx_shader.c), the start of its stored input's name <cache>/in/<id>.in.
static unsigned long long shader_id(const GfxShader *s) {
    if (!s) return 0;
    const uint8_t *iid = gfx_shader_input_id(s);
    unsigned long long v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | iid[i];
    return v;
}

static void describe_mark(uint32_t id, char *o, size_t size) {
    const VkPassMark *m = vk_pass_mark_record(id);
    if (!id) {
        snprintf(o, size, "nothing yet");
        return;
    }
    if (!m) {
        snprintf(o, size, "no record (a later mark took its slot)");
        return;
    }
    if (m->kind == VK_MARK_DISPATCH) {
        snprintf(o, size, "dispatch %u of submission %llu: %ux%ux%u groups%s, cs %016llx", m->step,
                 (unsigned long long)m->fence, m->w, m->h, m->d, m->w ? "" : " (indirect)", shader_id(m->first[0]));
        return;
    }
    size_t n = (size_t)snprintf(o, size, "pass %u of submission %llu: %ux%u, %u layer%s, colour formats", m->step,
                                (unsigned long long)m->fence, m->w, m->h, m->d, m->d == 1 ? "" : "s");
    for (int i = 0; i < m->ncolor && i < 8 && n < size; i++)
        n += (size_t)snprintf(o + n, size - n, "%s%d", i ? "," : " ", (int)m->color[i]);
    if (!m->ncolor && n < size) n += (size_t)snprintf(o + n, size - n, " none");
    if (n < size) n += (size_t)snprintf(o + n, size - n, ", depth format %d", (int)m->depth);
    if (n >= size) return;
    if (m->draws == UINT32_MAX)
        snprintf(o + n, size - n, ", still recording; first draw vs %016llx fs %016llx", shader_id(m->first[0]),
                 shader_id(m->first[1]));
    else if (!m->draws)
        snprintf(o + n, size - n, ", no draws");
    else
        snprintf(o + n, size - n, ", %u draws; first vs %016llx fs %016llx, last vs %016llx fs %016llx", m->draws,
                 shader_id(m->first[0]), shader_id(m->first[1]), shader_id(m->last[0]), shader_id(m->last[1]));
}

void vk_pass_marks_format(const char *source, uint32_t started, uint32_t finished, char *o, size_t size) {
    char a[640], b[640];
    describe_mark(started, a, sizeof a);
    describe_mark(finished, b, sizeof b);
    snprintf(o, size,
             "vk: pass marks (%s, %u made): the GPU last started mark %u and last finished mark %u\n"
             "vk:   last started %u: %s\n"
             "vk:   last finished %u: %s",
             source, (unsigned)seq, (unsigned)started, (unsigned)finished, (unsigned)started, a, (unsigned)finished, b);
}

static void log_lines(char *t) {
    for (char *line = t, *nl; line && *line; line = nl ? nl + 1 : NULL) {
        nl = strchr(line, '\n');
        if (nl) *nl = 0;
        rt_log("%s", line);
    }
}

// vkGetQueueCheckpointData2NV, else vkGetQueueCheckpointDataNV, on the graphics queue (the replay's): one entry per
// stage with the last checkpoint that stage executed. A checkpoint is the mark's id times 2, plus 1 at its end, so the
// top-of-pipe entry names the last start or end the GPU reached, and the bottom-of-pipe entry the last one before which
// everything had finished: at a start, that is the end of the mark before it.
static void nv_report(void) {
    VkCtx *c = vk_ctx();
    if (!c || !set_checkpoint) return;
    PFN_vkGetQueueCheckpointData2NV get2 =
        (PFN_vkGetQueueCheckpointData2NV)vk_dpa(c->device, "vkGetQueueCheckpointData2NV");
    PFN_vkGetQueueCheckpointDataNV get1 =
        (PFN_vkGetQueueCheckpointDataNV)vk_dpa(c->device, "vkGetQueueCheckpointDataNV");
    enum { MAXN = 32 };
    uint64_t stage[MAXN], marker[MAXN];
    uint32_t n = 0;
    if (get2) {
        VkCheckpointData2NV d[MAXN];
        get2(c->gfx_queue, &n, NULL);
        if (n > MAXN) n = MAXN;
        for (uint32_t i = 0; i < n; i++) d[i] = (VkCheckpointData2NV){.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_2_NV};
        get2(c->gfx_queue, &n, d);
        for (uint32_t i = 0; i < n; i++) stage[i] = d[i].stage, marker[i] = (uint64_t)(uintptr_t)d[i].pCheckpointMarker;
    } else if (get1) {
        VkCheckpointDataNV d[MAXN];
        get1(c->gfx_queue, &n, NULL);
        if (n > MAXN) n = MAXN;
        for (uint32_t i = 0; i < n; i++) d[i] = (VkCheckpointDataNV){.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV};
        get1(c->gfx_queue, &n, d);
        for (uint32_t i = 0; i < n; i++) stage[i] = d[i].stage, marker[i] = (uint64_t)(uintptr_t)d[i].pCheckpointMarker;
    } else {
        rt_log("vk: pass marks: no vkGetQueueCheckpointDataNV entry point");
        return;
    }
    uint32_t started = 0, finished = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t id = (uint32_t)(marker[i] >> 1);
        int end = (int)(marker[i] & 1);
        rt_log("vk: checkpoint of stage %#llx: the %s of mark %u", (unsigned long long)stage[i], end ? "end" : "start", id);
        if (stage[i] == VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT) started = id;
        if (stage[i] == VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT) finished = end ? id : id ? id - 1 : 0;
    }
    if (!n) rt_log("vk: pass marks: the driver reported no checkpoints");
    char t[2048];
    vk_pass_marks_format("VK_NV_device_diagnostic_checkpoints", started, finished, t, sizeof t);
    log_lines(t);
}

// The kinds of bits (VK_GAP_*) as text; "barriers and state only" when none was noted.
static void gap_text(uint32_t kinds, char *o, size_t size) {
    size_t n = 0;
    o[0] = 0;
    for (int b = 0; b < VK_GAP_N && n < size; b++)
        if (kinds >> b & 1) n += (size_t)snprintf(o + n, size - n, "%s%s", n ? ", " : "", gp_kind_names[b]);
    if (!n) snprintf(o, size, "barriers and state only");
}

// A GPU that finished the last mark it started stopped between two marks, where the two lines of
// vk_pass_marks_format name nothing: in a player's log both were one compute dispatch, with 51 later marks made and
// none of them started. This adds the mark that never started and what the replay recorded between the two (the gap
// notes: copies, clears, the upscaler, write-backs), or what it recorded after the last mark of all.
static void report_between(uint32_t started, uint32_t finished) {
    if (!finished || started != finished) return;
    char what[512], next[640];
    const VkPassMark *m = vk_pass_mark_record(finished + 1);
    if (m) {
        describe_mark(finished + 1, next, sizeof next);
        gap_text(m->gap, what, sizeof what);
        rt_log("vk:   the GPU stopped after mark %u and before the next one, which it never started: %s", (unsigned)finished,
               next);
        rt_log("vk:   recorded between the two: %s", what);
    } else if (finished == seq) {
        gap_text(gap_now, what, sizeof what);
        rt_log("vk:   the GPU stopped after mark %u, the last one made; recorded since: %s", (unsigned)finished, what);
    } else {
        rt_log("vk:   the GPU stopped after mark %u; the record of the mark after it is gone (a later mark took its slot)",
               (unsigned)finished);
    }
}

static void describe_draw_mark(uint32_t id, char *o, size_t size) {
    const DrawMark *d = &dring[id & (VK_DRAW_MARKS - 1)];
    if (!id) snprintf(o, size, "nothing yet");
    else if (d->id != id) snprintf(o, size, "no record (a later mark took its slot)");
    else
        snprintf(o, size, "before draw %u of pass mark %u: vs %016llx fs %016llx", d->draw, d->pass, shader_id(d->vs),
                 shader_id(d->fs));
}

// Words 4 and 5 (vk_draw_mark): the draws the GPU had not finished were recorded after the mark in word 5, so its
// shaders and those of the marks up to the one in word 4 are the candidates; up to DRAW_MARK_LINES of them are listed.
#define DRAW_MARK_LINES 12
static void draw_marks_report(void) {
    if (!vk_draw_marks_on || !words) return;
    uint32_t reached = words[4], finished = words[5];
    char t[256];
    rt_log("vk: draw marks (NX2NAVI_DRAW_CRUMBS, %u made): the GPU last reached draw mark %u, and every draw before draw "
           "mark %u had finished; the draws it had not finished follow these marks. The marks are numbered as they were "
           "recorded: with replay workers on, the numbers between the two are not all later in the queue, and only the "
           "two marks named here are certain:", (unsigned)__atomic_load_n(&dseq, __ATOMIC_RELAXED), (unsigned)reached,
           (unsigned)finished);
    uint32_t n = reached >= finished ? reached - finished + 1 : 1;
    if (n > DRAW_MARK_LINES) n = DRAW_MARK_LINES;
    for (uint32_t i = 0; i < n; i++) {
        describe_draw_mark(finished + i, t, sizeof t);
        rt_log("vk:   draw mark %u: %s", (unsigned)(finished + i), t);
    }
    if (reached >= finished && reached - finished + 1 > n)
        rt_log("vk:   and %u more up to draw mark %u", (unsigned)(reached - finished + 1 - n), (unsigned)reached);
    if (reached < finished || reached - finished + 1 > n) {   // the mark the GPU last reached, when the list left it out
        describe_draw_mark(reached, t, sizeof t);
        rt_log("vk:   draw mark %u, the last the GPU reached: %s", (unsigned)reached, t);
    }
}

void vk_pass_marks_report(void) {
    if (!vk_pass_marks_on) return;
    uint32_t started, finished;
    char t[2048];
    if (vk_pass_marks_read(&started, &finished)) {
        vk_pass_marks_format("VK_AMD_buffer_marker", started, finished, t, sizeof t);
        log_lines(t);
        report_between(started, finished);
        draw_marks_report();
    }
    nv_report();
}

// ---- pass times (NX2NAVI_PASS_TIMES=1): GPU time per render pass and dispatch, for GPUs no vendor profiler supports
// (RGP covers RDNA only, so an RX 580 has none, and RenderDoc cannot capture this program). Every pass mark also
// writes a timestamp at its two points (TOP_OF_PIPE before the pass, BOTTOM_OF_PIPE after it), into the query slot
// pair of its ring slot. pass_times_poll, on the replay thread at each new mark, takes in the marks whose submission
// has finished, resets their slots on the host (hostQueryReset) and sums their time per kind of pass: its size,
// attachment formats and its first fragment (or compute) shader. Every 600 presented frames the log lists the passes
// with the most GPU time per frame. Passes overlap on the GPU, so the times are each pass's span, not its exclusive
// share; their order is what matters. A slot whose mark was not read yet when the ring comes round is not timed
// (pt_timed 2) for that turn.
#define PT_SUMS 1024
#define PT_REPORT_FRAMES 600
#define PT_TOP 15
typedef struct {
    uint64_t key;
    uint8_t kind, ncolor;
    uint32_t w, h, d;
    VkFormat c0, depth;
    unsigned long long sid;
    double us;
    uint64_t n, draws;
    uint32_t vw, vh;   // the largest viewport its draws set
} PtSum;
static PtSum pt_sum[PT_SUMS];
static uint32_t pt_read;                 // the newest mark id taken in
static double pt_ns_per_tick, pt_total_us;
static uint64_t pt_mask, pt_frame0, pt_marks, pt_lost;

static void pass_times_init(VkCtx *c) {
    const char *e = cfg_get("NX2NAVI_PASS_TIMES");
    if (!e || !*e || !strcmp(e, "0")) return;
    if (!vk_pass_marks_on) {
        rt_log("vk: NX2NAVI_PASS_TIMES: pass marks are off (NX2NAVI_PASS_CRUMBS=0 or no marker extension): no pass times");
        return;
    }
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(c->phys, &f2);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, qp);
    uint32_t bits = c->gfx_family < nq ? qp[c->gfx_family].timestampValidBits : 0;
    VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                .queryCount = 2 * VK_PASS_MARKS};
    if (!f12.hostQueryReset || !bits || c->props.limits.timestampPeriod <= 0 ||
        vkCreateQueryPool(c->device, &qi, NULL, &pt_pool) != VK_SUCCESS) {
        rt_log("vk: NX2NAVI_PASS_TIMES: no hostQueryReset or no timestamps on the queue: no pass times");
        pt_pool = VK_NULL_HANDLE;
        return;
    }
    vkResetQueryPool(c->device, pt_pool, 0, 2 * VK_PASS_MARKS);
    pt_ns_per_tick = c->props.limits.timestampPeriod;
    pt_mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
    pt_read = seq;
    pt_frame0 = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    rt_log("vk: pass times on (NX2NAVI_PASS_TIMES): GPU time per render pass and dispatch, the top %d every %d frames",
           PT_TOP, PT_REPORT_FRAMES);
}

static void pt_add(const VkPassMark *m, double us) {
    unsigned long long sid = shader_id(m->kind == VK_MARK_DISPATCH ? m->first[0] : m->first[1]);
    uint64_t key = sid * 0x9E3779B97F4A7C15ull ^ (uint64_t)m->kind << 56 ^ (uint64_t)m->w << 32 ^ (uint64_t)m->h << 16 ^
                   (uint64_t)m->ncolor << 8 ^ (uint64_t)m->color[0] * 0x100000001b3ull ^ (uint64_t)m->depth << 40;
    if (!key) key = 1;
    for (uint32_t i = (uint32_t)(key % PT_SUMS), k = 0; k < PT_SUMS; k++, i = (i + 1) % PT_SUMS) {
        PtSum *e = &pt_sum[i];
        if (e->key && e->key != key) continue;
        if (!e->key)
            *e = (PtSum){key, m->kind, m->ncolor, m->w, m->h, m->d, m->ncolor ? m->color[0] : VK_FORMAT_UNDEFINED,
                         m->depth, sid, 0, 0, 0, 0, 0};
        if (m->vw > e->vw) e->vw = m->vw;
        if (m->vh > e->vh) e->vh = m->vh;
        e->us += us, e->n++, e->draws += m->draws == UINT32_MAX ? 0 : m->draws;
        return;
    }
    pt_lost++;   // table full: counted in the total only
}

static int pt_cmp(const void *a, const void *b) {
    double x = ((const PtSum *)a)->us, y = ((const PtSum *)b)->us;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void pt_report(uint64_t frames) {
    static PtSum top[PT_SUMS];
    int n = 0;
    for (int i = 0; i < PT_SUMS; i++)
        if (pt_sum[i].key) top[n++] = pt_sum[i];
    qsort(top, (size_t)n, sizeof *top, pt_cmp);
    double f = frames ? (double)frames : 1;
    rt_log("vk: pass times over %llu frames: %.2f ms of pass spans per frame in %.0f passes and dispatches (%d kinds%s)",
           (unsigned long long)frames, pt_total_us / f / 1e3, pt_marks / f, n, pt_lost ? ", table full" : "");
    for (int i = 0; i < n && i < PT_TOP; i++) {
        const PtSum *e = &top[i];
        if (e->kind == VK_MARK_DISPATCH)
            rt_log("vk:   %5.2f ms/frame %5.1f%% dispatch %ux%ux%u groups, cs %016llx: %.1f per frame, %.3f ms each",
                   e->us / f / 1e3, pt_total_us > 0 ? e->us * 100 / pt_total_us : 0, e->w, e->h, e->d, e->sid, e->n / f,
                   e->us / (double)e->n / 1e3);
        else
            rt_log("vk:   %5.2f ms/frame %5.1f%% pass %ux%u x%u, %u colour (first fmt %d), depth fmt %d, fs %016llx: %.1f per "
                   "frame, %.3f ms and %.0f draws each, viewport up to %ux%u", e->us / f / 1e3,
                   pt_total_us > 0 ? e->us * 100 / pt_total_us : 0, e->w, e->h, e->d, e->ncolor, (int)e->c0, (int)e->depth,
                   e->sid, e->n / f, e->us / (double)e->n / 1e3, (double)e->draws / (double)e->n, e->vw, e->vh);
    }
    memset(pt_sum, 0, sizeof pt_sum);
    pt_total_us = 0, pt_marks = 0, pt_lost = 0;
}

// Replay thread (vk_pass_mark_begin).
static void pass_times_poll(void) {
    VkCtx *c = vk_ctx();
    uint64_t done = vk_sync_fence_done();
    while (pt_read != seq) {
        uint32_t id = pt_read + 1;
        if (!id) { pt_read = id; continue; }
        uint32_t slot = id & (VK_PASS_MARKS - 1);
        const VkPassMark *m = &ring[slot];
        if (m->id == id) {
            if (!m->fence || m->fence > done) break;   // its submission is still being recorded or running
            // the submission finished: both timestamps are in, or one was never written (a pass whose end was not
            // recorded), which leaves that mark out
            if (pt_timed[slot] == 1 && m->draws != UINT32_MAX) {
                uint64_t r[4] = {0};
                VkResult q = vkGetQueryPoolResults(c->device, pt_pool, slot * 2, 2, sizeof r, r, 16,
                                                   VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
                if ((q == VK_SUCCESS || q == VK_NOT_READY) && r[1] && r[3]) {
                    double us = (double)((r[2] - r[0]) & pt_mask) * pt_ns_per_tick / 1e3;
                    if (us < 1e6) pt_add(m, us), pt_total_us += us, pt_marks++;
                }
            }
            if (pt_timed[slot]) {
                vkResetQueryPool(c->device, pt_pool, slot * 2, 2);
                pt_timed[slot] = 0;
            }
        }
        pt_read = id;
    }
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (now - pt_frame0 >= PT_REPORT_FRAMES) {
        pt_report(now - pt_frame0);
        pt_frame0 = now;
    }
}
