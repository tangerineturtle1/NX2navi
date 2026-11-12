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
#include "vk.h"

static PFN_vkCmdWriteBufferMarkerAMD write_marker;
static VkBuffer buf;
static VmaAllocation alloc;
static volatile uint32_t *words;   // 0, 1: crumbs; 2, 3: pass marks

// Pass marks (below)
int vk_pass_marks_on;
static int pm_amd;                          // marks into words 2 and 3
static PFN_vkCmdSetCheckpointNV set_checkpoint;
static void pass_marks_init(VkCtx *c);

static void buffer_init(VkCtx *c) {
    write_marker = (PFN_vkCmdWriteBufferMarkerAMD)vk_dpa(c->device, "vkCmdWriteBufferMarkerAMD");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 16,
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
    words[0] = words[1] = words[2] = words[3] = 0;
    rt_log("vk: GPU breadcrumbs on (VK_AMD_buffer_marker)");
}

void vk_crumbs_init(VkCtx *c) {
    if (c->has_buffer_marker) buffer_init(c);
    pass_marks_init(c);
}

void vk_crumbs_release(void) {
    VkCtx *c = vk_ctx();
    if (buf && c) vmaDestroyBuffer(c->vma, buf, alloc);
    buf = VK_NULL_HANDLE, alloc = NULL, words = NULL, write_marker = NULL;
    vk_pass_marks_on = pm_amd = 0, set_checkpoint = NULL;
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
}

void vk_pass_mark_cmd(VkCommandBuffer cb, uint32_t id, int end) {
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
    uint64_t words = 0, h = 0xcbf29ce484222325ull;
    const uint8_t *b = (const uint8_t *)gfx_shader_spirv(s, &words);
    for (uint64_t k = 0; b && k < words * 4; k++) h = (h ^ b[k]) * 0x100000001b3ull;
    memo[i].s = s, memo[i].h = h;
    return h;
}

static int kind_is_pass(const VkPassMark *m) { return m->kind == VK_MARK_PASS; }

int vk_gpu_passes_capturing(void) { return gp_capture; }

static void gp_gap_close(uint32_t to) {
    if (gp_gap_from != UINT32_MAX && gp_ngaps < GP_GAPS) gp_gaps[gp_ngaps++] = (GpGap){gp_gap_from, to, gp_gap_kinds};
    gp_gap_from = UINT32_MAX, gp_gap_kinds = 0;
}

void vk_gpu_gap_note(int kind, uint64_t units) {
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
        gp_part = gp_n++;
        if (!gp_in_mark) gp_gap_from = gp_part * 2;
        return;
    }
    if (gp_part == UINT32_MAX) return;
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gp_pool, gp_part * 2 + 1);
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
    uint32_t id = ++seq;
    if (!id) id = ++seq;   // 0 means no mark
    VkPassMark *m = &ring[id & (VK_PASS_MARKS - 1)];
    m->id = id, m->kind = (uint8_t)kind, m->fence = fence, m->draws = UINT32_MAX;
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

void vk_pass_marks_report(void) {
    if (!vk_pass_marks_on) return;
    uint32_t started, finished;
    char t[2048];
    if (vk_pass_marks_read(&started, &finished)) {
        vk_pass_marks_format("VK_AMD_buffer_marker", started, finished, t, sizeof t);
        log_lines(t);
    }
    nv_report();
}
