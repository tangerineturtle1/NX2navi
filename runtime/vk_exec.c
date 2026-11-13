// The op-stream replay core of the Vulkan back end: walks the command handles of
// one submission into its command buffer, keeps render targets and fixed-function state, opens and closes dynamic
// rendering passes and records clears and barriers. Draws, binds and dispatches go to vk_exec_op_draw
// (vk_exec_draw.c); copies, counters, events and conditional rendering to vk_exec_op_xfer (vk_exec_xfer.c). Images
// stay in VK_IMAGE_LAYOUT_GENERAL (vk_texture.c); full memory barriers order passes, clears and whatever those two
// record between them.
//
// The op loop handles END / BEGIN / JUMP / CALL and the pure-state ops, applies the state defaults, keeps render targets
// until they change and skips clears while rendering is disabled. A pass has GENERAL-layout attachments, LOAD/STORE and
// a render area from the smallest attachment, with a full barrier before it.
// Contains code Copyright (c) 2026 jhaizhou, Copyright (c) 2026 Peiikai (MIT; see THIRD_PARTY_NOTICES.md).
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_tex.h"
#include "platform/platform.h"
#include "prof.h"
#include "tex.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_target_frames.h"
#include "vk_dlaa.h"

#define WEAK __attribute__((weak))
#define CALL_DEPTH 8

typedef struct {
    VkImage image;
    VkFormat format;
    VkImageViewType type;
    VkImageAspectFlags aspect;
    uint32_t level, base, layers;
    VkImageView view;
} ViewEnt;

static VkExec X;
static int x_ready;
// The replay state this thread records with now (inside vk_exec_record, or a test's vk_exec_probe_inline), NULL when
// it records none: vk_exec_inline_begin hands out its command buffer.
static _Thread_local VkExec *x_cur;

// ---- parallel groups (NX2NAVI_REPLAY_WORKERS; "replay workers" below). A chunk: a part of a group's ops that one
// thread records into a command buffer of its own, from the replay state the front pass left at its first op.
#define GROUP_CHUNKS 17                 // chunks a group is cut into at most: RW_MAX workers and the submitting thread
#define LIVE_GROUPS 4                   // groups handed out whose command buffers are not in their part yet, at most
typedef struct Group Group;
typedef struct {
    VkExec ex;                          // the state at its first op, then the state it records with
    const op_hdr *h;                    // its first op
    uint32_t nops;                      // the ops it records (JUMPs not counted)
    uint32_t cond0, ncond;              // its render conditions' decisions in its group's cond
    uint64_t oq;                        // the occlusion counting at its first op (vk_exec_xfer_chunk_state)
    uint32_t slot0, nslots;             // its block of occlusion query slots (vk_exec_xfer_slots)
    uint32_t brk;                       // its ops that are not state or a draw (vk_exec_brk_now)
    int idx;                            // in its group: 0 runs the group's pass hooks
    Group *g;                           // its group
    VkCommandBuffer cb;
    int recorded;
    volatile int32_t done;
} Chunk;
struct Group {
    Chunk c[GROUP_CHUNKS];
    int n;
    uint8_t *cond;                      // the decisions of the group's render conditions, in stream order
    uint32_t ncond, ccond;
    volatile int32_t hooks;             // chunk 0 began its first pass (or ended): the others may start
    volatile int32_t ndone;             // its chunks done
    volatile uint64_t seq;              // its number: Q.out when it was handed out (vk_exec_stream_pos)
    uint32_t upd;                       // the submission's inline uniform updates before it (vk_exec_upd_top)
    int at;                             // its chunks' places in the part's list (vk_sync_append_later)
    uint64_t out_ns;                    // when it was handed out
};
// Group number s is GQ[s % LIVE_GROUPS]. Its chunks wait in Q in the order of the stream once it is handed out; the
// workers (and the submitting thread, while it waits) take them in that order.
static Group GQ[LIVE_GROUPS];
#define QN (LIVE_GROUPS * GROUP_CHUNKS)
static struct {
    Chunk *q[QN];                       // chunk number i of those handed out is q[i % QN]
    volatile uint64_t put, take;        // chunks handed out, taken
    volatile uint64_t done;             // every group up to this number has all its chunks done
    uint64_t out, drained;              // the submitting thread: groups handed out, and put into their parts
} Q;
static PlatMutex done_lock = PLAT_MUTEX_INIT;   // Q.done moves on (groups_done_advance)
static _Thread_local Chunk *rw_chunk;   // the chunk the calling thread records, NULL outside one
// The submitting thread records on past groups whose chunks may not all be done yet (NX2NAVI_REPLAY_PIPE).
static _Thread_local int rw_behind;
// Why it waited for them (RW.pw_*): room for the next group, an op chunks never record, a pass's hooks, an effect that
// waits for its turn (vk_exec_turn), a texture entry, a part split, the submission's end; and the waits that found
// every group done already.
enum { PW_NEXT, PW_OP, PW_HOOKS, PW_TURN, PW_TEX, PW_SPLIT, PW_END, PW_DONE, PW_N };
static void group_wait(int why, int help);
static void group_drain(VkExec *ex, int why);
static _Thread_local const op_hdr *rw_op;   // in a chunk: the op it records (vk_exec_mesh_run reads on from it)
static _Thread_local int rw_ordered;    // every earlier chunk of its group is done
static PlatMutex tfr_lock = PLAT_MUTEX_INIT;   // target frames (vk_target_frames.h) noted by several chunks at once

// ---- NX2NAVI_TRACE=gfx frame counters

VkExecFrame vk_exec_frame;
static struct { const uint8_t *mem; uint32_t w, h, fmt, n; } fw[64];   // textures written this frame
static int nfw;

uint64_t vk_exec_recording_fence(void) { return x_cur ? x_cur->fence : 0; }

void vk_exec_frame_written(const tex_t *t) {
    if (!(rt_trace & RT_TRACE_GFX) || !t) return;
    for (int i = 0; i < nfw; i++)
        if (fw[i].mem == t->mem) { fw[i].n++; return; }
    if (nfw == 64) return;
    fw[nfw].mem = t->mem, fw[nfw].w = t->d.width, fw[nfw].h = t->d.height, fw[nfw].fmt = (uint32_t)t->d.format;
    fw[nfw++].n = 1;
}

// ---- draw runs (NX2NAVI_REPLAY_RUNS=1, diagnostics for recording a submission on several threads)
// How the draws of a frame fall into runs that a recording thread of their own could take: a run is the draws between
// two ops that have to stay in the replay's order on one thread (dispatches, copies, texture and buffer clears, events,
// the game's barriers, a held submission going on, the end of a submission). The ops a run carries that a serial front
// pass would have to settle in order before the threads record (counter reports and resets, render conditions, inline
// uniform updates, render target changes, attachment clears) are counted apart. Every 600 presented frames one line:
// the draws per frame, the share of them in runs of at least 16, 64 and 256 draws, the runs, and the breaks by kind.
enum { RB_DISPATCH, RB_COPY, RB_CLEAR, RB_EVENT, RB_BARRIER, RB_HELD, RB_SUBMIT, RB_N };
enum { RO_COUNTER, RO_COND, RO_UB, RO_RT, RO_ATT_CLEAR, RO_N };
static const char *const rb_name[RB_N] = {"dispatches", "copies", "clears", "events", "barriers", "held", "submits"};
static const char *const ro_name[RO_N] = {"counters", "conditions", "uniform updates", "target changes",
                                          "attachment clears"};
static struct {
    int on;
    uint32_t draws;                      // of the run open now
    uint64_t total, in16, in64, in256, runs, longest, brk[RB_N], carried[RO_N], frame0;
} RR = {.on = -1};

static void runs_break(int why) {
    uint32_t d = RR.draws;
    RR.brk[why]++;
    if (!d) return;
    RR.runs++, RR.total += d, RR.draws = 0;
    if (d >= 16) RR.in16 += d;
    if (d >= 64) RR.in64 += d;
    if (d >= 256) RR.in256 += d;
    if (d > RR.longest) RR.longest = d;
}

static void runs_op(uint32_t op) {
    switch (op) {
    case OP_DRAW: case OP_DRAW_IDX: case OP_DRAW_IDX_IND: case OP_DRAW_MESH: case OP_DRAW_MESH_IND: RR.draws++; break;
    case OP_DISPATCH: case OP_DISPATCH_IND: runs_break(RB_DISPATCH); break;
    case OP_COPY_B2B: case OP_COPY_B2T: case OP_COPY_T2B: case OP_COPY_T2T: runs_break(RB_COPY); break;
    case OP_CLEAR_TEX: case OP_CLEAR_BUF: runs_break(RB_CLEAR); break;
    case OP_SIGNAL_EVENT: case OP_WAIT_EVENT: runs_break(RB_EVENT); break;
    case OP_BARRIER: runs_break(RB_BARRIER); break;
    case OP_REPORT_COUNTER: case OP_RESET_COUNTER: RR.carried[RO_COUNTER]++; break;
    case OP_RENDER_ENABLE: case OP_RENDER_COND: RR.carried[RO_COND]++; break;
    case OP_UPDATE_UB: RR.carried[RO_UB]++; break;
    case OP_SET_RT: RR.carried[RO_RT]++; break;
    case OP_CLEAR_COLOR: case OP_CLEAR_DS: RR.carried[RO_ATT_CLEAR]++; break;
    default: break;
    }
}

static int runs_on(void) {
    if (RR.on < 0) RR.on = rt_knob("REPLAY_RUNS", "0");
    return RR.on;
}

static void runs_frame(uint64_t frame) {
    if (!runs_on() || frame % 600) return;
    if (RR.frame0 && frame > RR.frame0) {
        double f = (double)(frame - RR.frame0), t = RR.total ? (double)RR.total : 1.0;
        char b[256], c[256];
        int o = 0;
        for (int k = 0; k < RB_N; k++)
            o += snprintf(b + o, sizeof b - (size_t)o, "%s %s %.1f", k ? "," : "", rb_name[k], RR.brk[k] / f);
        o = 0;
        for (int k = 0; k < RO_N; k++)
            o += snprintf(c + o, sizeof c - (size_t)o, "%s %s %.1f", k ? "," : "", ro_name[k], RR.carried[k] / f);
        rt_log("vk_exec: frame %llu: draw runs: %.0f draws a frame in %.1f runs; in runs of 16 or more %.0f%%, "
               "64 or more %.0f%%, 256 or more %.0f%%, longest %llu; breaks a frame:%s; carried a frame:%s",
               (unsigned long long)frame, RR.total / f, RR.runs / f, 100.0 * RR.in16 / t, 100.0 * RR.in64 / t,
               100.0 * RR.in256 / t, (unsigned long long)RR.longest, b, c);
    }
    uint32_t open = RR.draws;
    memset(&RR.total, 0, sizeof RR - offsetof(typeof(RR), total));
    RR.draws = open, RR.frame0 = frame;
}

uint64_t vk_exec_submit_seq;

void vk_exec_frame_log(const tex_t *p, uint64_t frame) {
    vk_exec_cond_frame_end(frame);   // a frame boundary in replay order (present_image runs on the replay thread)
    runs_frame(frame);
    if (!(rt_trace & RT_TRACE_GFX)) return;
    VkExecFrame *f = &vk_exec_frame;
    uint64_t ew, eu, el, ed;
    vk_exec_event_stats(&ew, &eu, &el, &ed);
    int hit = 0;
    char list[512] = "";
    size_t o = 0;
    for (int i = 0; i < nfw; i++) {
        if (p && fw[i].mem == p->mem) hit = 1;
        if (o < sizeof list - 48)
            o += (size_t)snprintf(list + o, sizeof list - o, " %p:%ux%u/%#x*%u", (const void *)fw[i].mem, fw[i].w,
                                  fw[i].h, fw[i].fmt, fw[i].n);
    }
    rt_log("frame %llu: submits %llu passes %llu draws %llu/%llu clears %llu copies %llu dispatches %llu; present %p "
           "%ux%u/%#x written %d; feedback draws %llu barriers %llu restarts %llu layouts %llu read-before-write %llu; "
           "uniform updates %llu (%llu ended a pass); binds %llu in %.2f ms (textures %.2f, buffers %.2f, sig %.2f, push %.2f ms; "
           "descriptors reused %llu, resolved %llu); dynamic state calls %llu (%llu unchanged, left out); "
           "event waits %llu (%llu before their signal: %llu "
           "signalled later, %.1f submits later on average); targets%s",
           (unsigned long long)frame, (unsigned long long)f->submits,
           (unsigned long long)f->passes, (unsigned long long)f->draws, (unsigned long long)f->draws_seen,
           (unsigned long long)f->clears, (unsigned long long)f->copies, (unsigned long long)f->dispatches,
           p ? (const void *)p->mem : NULL, p ? p->d.width : 0, p ? p->d.height : 0, p ? (uint32_t)p->d.format : 0,
           hit, (unsigned long long)f->fb_draws, (unsigned long long)f->fb_barriers, (unsigned long long)f->fb_restarts,
           (unsigned long long)f->fb_layouts, (unsigned long long)f->fb_war_barriers, (unsigned long long)f->ub_updates,
           (unsigned long long)f->ub_pass_breaks, (unsigned long long)f->bind_calls,
           (double)(f->bind_ns) / 1e6, (double)(f->bind_tex_ns) / 1e6, (double)(f->bind_buf_ns) / 1e6,
           (double)(f->bind_sig_ns) / 1e6, (double)(f->bind_push_ns) / 1e6, (unsigned long long)f->bind_reused,
           (unsigned long long)f->bind_resolved, (unsigned long long)f->dyn_calls, (unsigned long long)f->dyn_left,
           (unsigned long long)ew, (unsigned long long)eu, (unsigned long long)el,
           el ? (double)ed / (double)el : 0.0, list);
    memset(f, 0, sizeof *f);
    nfw = 0;
}

WEAK void vk_exec_op_draw(VkExec *ex, uint32_t op, const void *payload) {
    (void)ex, (void)payload;
    rt_fatal("vk: op %s is not replayed yet", nvn_op_name(op));
}

WEAK void vk_exec_op_xfer(VkExec *ex, uint32_t op, const void *payload) {
    (void)ex, (void)payload;
    rt_fatal("vk: op %s is not replayed yet", nvn_op_name(op));
}

enum { W_UNRENDERABLE, W_LEVEL, W_LAYER, W_VIEW_FORMAT, W_BLOCK_VIEW, W_BLOCK_LAYERS, W_CLEAR_TARGET, W_CLEAR_MASK, W_STENCIL_MASK, W_COUNT };

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

// NVN defaults of the replay state (nvn_mtl.m).
static void state_reset(VkExec *ex) {
    memset(ex, 0, sizeof *ex);
    ex->st_vmask[0] = ex->st_vmask[1] = ex->st_wmask[0] = ex->st_wmask[1] = 0xff;
    ex->depth_range[1] = 1.0f;
    vk_pipe_state_defaults(&ex->pipe);
    ex->barrier_at = -1;
}

// ---- render targets

static uint32_t level_extent(uint32_t v, uint32_t level) {
    v >>= level;
    return v ? v : 1;
}

// A view format other than the texture's is used when both map directly and have the same texel size (the image is
// created MUTABLE_FORMAT for colour formats). blocks: t is a BC texture with block_views, and the view an uncompressed
// format whose texel is one of its blocks.
static int view_format(const tex_t *t, const view_t *v, int blocks, VkFormat *out) {
    TexVkFormat a, b;
    int bw, bh, vbw, vbh;
    if (!tex_vk_format(t->d.format, &a) || a.vk != a.ideal || !tex_vk_format(v->format, &b) || b.vk != b.ideal ||
        b.depth || b.stencil)
        return 0;
    int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh), vbpb = nvn_fmt_bpb(v->format, &vbw, &vbh);
    if (!bpb || bpb != vbpb || (!blocks && (bw != 1 || bh != 1)) || vbw != 1 || vbh != 1) return 0;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(vk_ctx()->phys, b.vk, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return 0;
    *out = b.vk;
    return 1;
}

// ---- 3D BC render targets
// A 3D BC texture has no block views (vk_texture.c create_tex, NX2NAVI_BLOCK_VIEWS_3D): on RDNA2 cards such an image
// made BLOCK_TEXEL_VIEW_COMPATIBLE and 2D_ARRAY_COMPATIBLE took GPU writes past the end of its memory (issue #5). Draws
// into it through an uncompressed view of its blocks (agl's BC4 encoder fills 32x32x32 and 64x64x64 volumes and their
// levels at the logo) go to a stand-in: a 2D array of the blocks of one level in the view's format, one layer per
// depth plane. It starts with the level's content, so a pass that writes part of a plane keeps the rest, and each pass
// end copies the planes it drew into the level (a copy between a compressed and a size-compatible uncompressed format).
typedef struct { VkImage bc, img; VmaAllocation alloc; VkFormat fmt; uint32_t level, w, h, d; } BcStandIn;
#define BC_STANDINS 64   // one per level drawn: 6 volumes of up to 7 levels at the logo
static BcStandIn bcs[BC_STANDINS];
static int nbcs;
static PlatMutex bcs_lock = PLAT_MUTEX_INIT;   // the replay adds and reads; destroy_tex (any thread) removes

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, uint32_t layers) {
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .oldLayout = from, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
}

static void memory_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst,
                           VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa,
                           .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

// The stand-in of level level of a 3D BC image for draws through view format fmt, made on first use; 0 when none can
// be made (logged). Replay thread.
static int bc_standin(const VkTexImage *img, uint32_t level, VkFormat fmt, BcStandIn *out) {
    static int warned;
    plat_mutex_lock(&bcs_lock);
    int full = nbcs == BC_STANDINS;
    for (int i = 0; i < nbcs; i++)
        if (bcs[i].bc == img->image && bcs[i].level == level && bcs[i].fmt == fmt) {
            *out = bcs[i];
            plat_mutex_unlock(&bcs_lock);
            return 1;
        }
    plat_mutex_unlock(&bcs_lock);
    if (full) {
        if (!warned++) rt_log("vk: more than %d 3D BC render targets: the others are left out", BC_STANDINS);
        return 0;
    }
    uint32_t lw = level_extent(img->width, level), lh = level_extent(img->height, level);
    BcStandIn s = {img->image, VK_NULL_HANDLE, NULL, fmt, level, (lw + 3) / 4, (lh + 3) / 4,
                   level_extent(img->depth, level)};
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {s.w, s.h, 1}, .mipLevels = 1, .arrayLayers = s.d, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(vk_ctx()->vma, &ci, &ai, &s.img, &s.alloc, NULL) != VK_SUCCESS) {
        if (!warned++) rt_log("vk: stand-in for a 3D BC render target could not be created: left out");
        return 0;
    }
    // In the upload batch, which goes to the queue before this part: GENERAL, then the texture's blocks.
    VkCommandBuffer cb = vk_upload_begin();
    image_barrier(cb, s.img, VK_IMAGE_LAYOUT_UNDEFINED, s.d);
    memory_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, s.d},
                      .extent = {lw, lh, s.d}};   // texels of the compressed source
    vkCmdCopyImage(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, s.img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    memory_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                   VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    vk_upload_end();
    plat_mutex_lock(&bcs_lock);
    bcs[nbcs++] = s;
    plat_mutex_unlock(&bcs_lock);
    rt_log("vk: 3D BC render target %ux%ux%u level %u drawn through a %ux%u x %u stand-in (format %d)", img->width,
           img->height, img->depth, level, s.w, s.h, s.d, (int)fmt);
    *out = s;
    return 1;
}

// vk_exec_draw.c vk_exec_image_gone: the stand-in of a BC image that goes, freed once the GPU is done with it.
void vk_exec_bc_gone(VkImage image) {
    plat_mutex_lock(&bcs_lock);
    for (int i = 0; i < nbcs; i++)
        if (bcs[i].bc == image) {
            vk_defer_image(bcs[i].img, bcs[i].alloc);
            bcs[i--] = bcs[--nbcs];
        }
    plat_mutex_unlock(&bcs_lock);
}

// At a pass end: the planes the pass drew into a stand-in go to its texture.
static void bc_copy_back(VkExec *ex, const VkExecTarget *t) {
    plat_mutex_lock(&bcs_lock);
    for (int i = 0; i < nbcs; i++) {
        if (bcs[i].img != t->image) continue;
        memory_barrier(ex->cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t->base_layer, t->layers},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, bcs[i].level, 0, 1},
                          .dstOffset = {0, 0, (int32_t)t->base_layer},
                          .extent = {bcs[i].w, bcs[i].h, t->layers}};   // texels of the uncompressed source
        vk_gpu_gap_note(VK_GAP_BC_BACK, 0);
        vkCmdCopyImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, bcs[i].bc, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        memory_barrier(ex->cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        ex->recorded++;
        break;
    }
    plat_mutex_unlock(&bcs_lock);
}

// A draw into a 3D BC image without block views through an uncompressed view of its blocks: the target is the
// image's stand-in (bc_standin). 1 when o is filled (target or left out), 0 for any other target.
static int bc_target(const tex_t *t, const view_t *v, int has_v, const VkTexImage *img, VkExecTarget *o) {
    int bw, bh;
    if (img->type != VK_IMAGE_TYPE_3D || img->block_views || !nvn_fmt_bpb(t->d.format, &bw, &bh) || bw != 4 || bh != 4)
        return 0;
    VkFormat fmt;
    BcStandIn s;
    uint32_t level = has_v && v->set_levels ? v->base_level : 0;
    if (level >= img->levels) {
        warn_once(W_LEVEL, "render target level %u of a %u-level texture: left out", level, img->levels);
        return 1;
    }
    if (!has_v || !v->set_format || !view_format(t, v, 1, &fmt) || !bc_standin(img, level, fmt, &s)) {
        warn_once(W_BLOCK_VIEW, "render target format %#x (%ux%u) through view format %#x: no block view, left out",
                  t->d.format, t->d.width, t->d.height, has_v && v->set_format ? v->format : 0);
        return 1;
    }
    uint32_t base = 0, layers = s.d;
    if (v->set_layers) {
        base = v->min_layer;
        layers = v->nlayers ? v->nlayers : (base < s.d ? s.d - base : 0);
    }
    if (base >= s.d || !layers) return 1;
    if (base + layers > s.d) layers = s.d - base;
    o->image = s.img;   // not img->image: view_of makes this command buffer's view of the stand-in
    o->img = img;
    o->format = fmt;
    o->view_type = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    o->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    o->level = 0, o->base_layer = base, o->layers = layers;
    o->width = s.w, o->height = s.h, o->samples = 1;
    o->nvn_format = v->format;
    return 1;
}

static void resolve_target(const tex_t *t, const view_t *v, int has_v, int depth_slot, VkExecTarget *o) {
    memset(o, 0, sizeof *o);
    o->tex = t;
    if (!t) return;
    const VkTexImage *img = vk_texture_get(t);
    if (img && !depth_slot && bc_target(t, v, has_v, img, o)) return;   // a 3D BC image: its stand-in
    VkImageUsageFlags need = depth_slot ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    int is3d = img && img->type == VK_IMAGE_TYPE_3D && !depth_slot;   // slices drawn through 2D views (2D_ARRAY_COMPATIBLE)
    // A replay worker's lookup that needs its turn returns no image (vk_texture_deferred), and the op is replayed again
    // in turn (resolve_rt's callers): not a target left out, so not logged as one. Logged, the first such lookup read
    // as if the game drew into depth or cube targets the replay could not render.
    if (!img && vk_texture_deferred()) return;
    if (!img || !(img->usage & need) || (img->type != VK_IMAGE_TYPE_2D && !is3d)) {
        warn_once(W_UNRENDERABLE, "render target format %#x target %d (%ux%u) is not renderable here (%s): left out",
                  t->d.format, t->d.target, t->d.width, t->d.height,
                  !img ? "no image" : !(img->usage & need) ? (depth_slot ? "no depth attachment usage" : "no colour attachment usage")
                                    : "not a 2D image");
        return;
    }
    uint32_t level = has_v && v->set_levels ? v->base_level : 0;
    if (level >= img->levels) {
        warn_once(W_LEVEL, "render target level %u of a %u-level texture: left out", level, img->levels);
        return;
    }
    // A 3D level's depth planes take the place of layers (Vulkan views them as the array layers of a 2D array view).
    uint32_t nlay = is3d ? level_extent(img->depth, level) : img->layers;
    uint32_t base = 0, layers = nlay;
    if (has_v && v->set_layers) {
        base = v->min_layer;
        layers = v->nlayers ? v->nlayers : (base < nlay ? nlay - base : 0);
    }
    if (base >= nlay || !layers) {
        warn_once(W_LAYER, "render target layer %u of a %u-layer texture: left out", base, nlay);
        return;
    }
    if (base + layers > nlay) layers = nlay - base;
    VkFormat fmt = img->format;
    int32_t nvn_fmt = t->d.format;
    int blocks = !depth_slot && img->block_views;
    if (blocks) {   // a BC texture takes draws only through an uncompressed view of its blocks
        if (!has_v || !v->set_format || !view_format(t, v, 1, &fmt)) {
            warn_once(W_BLOCK_VIEW, "render target format %#x (%ux%u) through view format %#x: no block view, left out",
                      t->d.format, t->d.width, t->d.height, has_v && v->set_format ? v->format : 0);
            return;
        }
        nvn_fmt = v->format;
        if (layers > 1) {   // an uncompressed view of a BC image has one layer (no maintenance6 here)
            warn_once(W_BLOCK_LAYERS, "render target format %#x: block view of %u layers, first layer only", t->d.format,
                      layers);
            layers = 1;
        }
    } else if (!depth_slot && has_v && v->set_format && v->format != t->d.format) {
        if (view_format(t, v, 0, &fmt)) nvn_fmt = v->format;
        else warn_once(W_VIEW_FORMAT, "render target view format %#x of a format %#x texture: texture format used",
                       v->format, t->d.format);
    }
    o->image = img->image;
    o->img = img;
    o->scale = img->scale ? img->scale : 1;
    o->format = fmt;
    o->view_type = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    o->aspect = depth_slot ? img->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                           : VK_IMAGE_ASPECT_COLOR_BIT;
    o->level = level, o->base_layer = base, o->layers = layers;
    o->is3d = (uint8_t)is3d;
    o->blocks = (uint8_t)blocks;
    o->width = level_extent(img->width, level), o->height = level_extent(img->height, level);
    if (blocks) o->width = (o->width + 3) / 4, o->height = (o->height + 3) / 4;   // the view's extent: 4x4 blocks
    o->samples = img->samples ? img->samples : 1;
    o->nvn_format = nvn_fmt;
    if (!o->aspect) o->image = VK_NULL_HANDLE;
}

/* The size of a pass without attachments: the far corner of viewport 0, else of the scissor, 1..16384.
   NX2navi draws a fragment shader that only writes a storage buffer into a 1x1
   viewport; a pass sized by the viewport's width alone puts an offset viewport outside the scissor, and the cave rock
   loses its ambient light. */
void vk_exec_noatt_dims(const VkExec *ex, uint32_t *w, uint32_t *h) {
    double x1 = 0, y1 = 0;
    if (ex->has_vp) {
        x1 = ceil(fmax(ex->vp[0][0], ex->vp[0][0] + ex->vp[0][2]));
        y1 = ceil(fmax(ex->vp[0][1], ex->vp[0][1] + ex->vp[0][3]));
    }
    if ((x1 < 1 || y1 < 1) && ex->has_scissor) x1 = (double)ex->scissor.x + ex->scissor.w, y1 = (double)ex->scissor.y + ex->scissor.h;
    *w = x1 < 1 ? 1 : x1 > 16384 ? 16384 : (uint32_t)x1;
    *h = y1 < 1 ? 1 : y1 > 16384 ? 16384 : (uint32_t)y1;
}

// Resolves ex->rt to this command buffer's images and derives the pass parameters and the pipeline's target formats.
static void resolve_rt(VkExec *ex) {
    if (ex->rt_resolved) return;
    ex->rt_resolved = 1;
    const p_set_rt *r = &ex->rt;
    int n = r->n < 0 ? 0 : r->n > 8 ? 8 : r->n;
    uint32_t w = UINT32_MAX, h = UINT32_MAX, layers = UINT32_MAX, samples = 0, scale = 1;
    for (int i = 0; i < 8; i++) {
        VkExecTarget *t = &ex->color[i];
        resolve_target(i < n ? r->color[i] : NULL, &r->cview[i], i < n && r->has_cview[i], 0, t);
        ex->pass_color[i] = t->image ? t->format : VK_FORMAT_UNDEFINED;
        ex->pipe.color_fmt[i] = t->image ? t->nvn_format : 0;
        if (!t->image) continue;
        if (t->width < w) w = t->width;
        if (t->height < h) h = t->height;
        if (t->layers < layers) layers = t->layers;
        if (!samples) samples = t->samples;
        if (t->scale > scale) scale = t->scale;
    }
    VkExecTarget *d = &ex->depth;
    resolve_target(r->depth, &r->dview, r->has_dview, 1, d);
    ex->pass_depth = d->image && (d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? d->format : VK_FORMAT_UNDEFINED;
    ex->pass_stencil = d->image && (d->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? d->format : VK_FORMAT_UNDEFINED;
    ex->pipe.depth_fmt = d->image ? d->nvn_format : 0;
    if (d->image) {
        if (d->width < w) w = d->width;
        if (d->height < h) h = d->height;
        if (d->layers < layers) layers = d->layers;
        if (!samples) samples = d->samples;
        if (d->scale > scale) scale = d->scale;
    }
    ex->pass_noatt = w == UINT32_MAX;
    if (ex->pass_noatt) {
        vk_exec_noatt_dims(ex, &w, &h);
        layers = 1;
        static int logged;
        if (logged < 8 && ex->has_vp && (ex->vp[0][0] != 0 || ex->vp[0][1] != 0 || ex->vp[0][2] != w || ex->vp[0][3] != h))
            logged++, rt_log("vk_exec: pass without attachments %ux%u for viewport %g,%g %gx%g", w, h, ex->vp[0][0],
                             ex->vp[0][1], ex->vp[0][2], ex->vp[0][3]);
    }
    ex->pass_w = w, ex->pass_h = h, ex->pass_layers = layers;
    ex->pass_scale = ex->pass_noatt ? 1 : scale;
    ex->pass_ncolor = (uint32_t)n;
    ex->pass_samples = samples ? samples : 1;
    ex->pipe.samples = (uint8_t)ex->pass_samples;
}

// The attachment view of a target. A registered texture's view lives as long as its image (vk_exec_target_view); any
// other image's view, and that of a texture destroyed since the target was resolved (gone: its record is freed only
// after this submission, vk_defer_free), is made for this command buffer and freed at its fence.
static VkImageView view_of(VkExec *ex, const VkExecTarget *t) {
    if (t->img && t->img->image == t->image && !t->img->gone)
        return vk_exec_target_view(t->img, t->format, t->view_type, t->aspect, t->level, t->base_layer, t->layers, t->blocks);
    ViewEnt *v = ex->views;
    for (int i = 0; i < ex->nviews; i++)
        if (v[i].image == t->image && v[i].format == t->format && v[i].type == t->view_type && v[i].aspect == t->aspect &&
            v[i].level == t->level && v[i].base == t->base_layer && v[i].layers == t->layers)
            return v[i].view;
    VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->image,
                                .viewType = t->view_type, .format = t->format,
                                .subresourceRange = {t->aspect, t->level, 1, t->base_layer, t->layers}};
    VkImageViewUsageCreateInfo use = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    if (t->blocks) ci.pNext = &use;   // not the BC image's sampling and transfer usages
    VkImageView view;
    if (vkCreateImageView(vk_ctx()->device, &ci, NULL, &view) != VK_SUCCESS)
        rt_fatal("vk: render target view (format %d, %ux%u) could not be created", (int)t->format, t->width, t->height);
    if (ex->nviews == ex->cap_views) {
        ex->cap_views = ex->cap_views ? ex->cap_views * 2 : 16;
        ex->views = realloc(ex->views, sizeof(ViewEnt) * (size_t)ex->cap_views);
        if (!ex->views) rt_fatal("vk: out of memory");
        v = ex->views;
    }
    v[ex->nviews++] = (ViewEnt){t->image, t->format, t->view_type, t->aspect, t->level, t->base_layer, t->layers, view};
    return view;
}

typedef struct { int n; VkImageView v[]; } ViewList;

static void free_views(void *arg) {   // completion thread, once the submission that used them is done
    ViewList *l = arg;
    VkDevice dev = vk_ctx()->device;
    for (int i = 0; i < l->n; i++) vkDestroyImageView(dev, l->v[i], NULL);
    free(l);
}

// ---- target history (vk_target_frames.h: the table, what recent and steady mean, and the pass verdict)
uint64_t vk_exec_steady_passes;   // lookups a steady target alone made per-frame (vk_exec_targets_recent)

static TfrTarget tfr_of(const VkExecTarget *t) {
    return (TfrTarget){(uint64_t)(uintptr_t)t->image, t->level, t->base_layer, t->img ? t->img->addr : 0};
}

static int steady_on(void) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2NAVI_PIPE_SKIP_STEADY");
        rt_log("vk: a pass whose targets the game has rendered in %d frames or more counts as per-frame after a "
               "pause too, so its draws are skipped, not waited for, while a pipeline builds: %s (NX2NAVI_PIPE_SKIP_STEADY)",
               TFR_STEADY, on ? "on" : "off");
    }
    return on;
}

// tfr_pass_recent for the current pass; vk_exec_steady_passes counts the lookups that only a steady target made
// per-frame.
int vk_exec_targets_recent(VkExec *ex, int any) {
    resolve_rt(ex);
    uint64_t stamp = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    TfrTarget c[8], d = tfr_of(&ex->depth);
    for (int i = 0; i < 8; i++) c[i] = tfr_of(&ex->color[i]);
    int by_steady = 0, r = tfr_pass_recent(c, 8, &d, stamp, any, steady_on(), &by_steady);
    vk_exec_steady_passes += (uint64_t)by_steady;
    return r;
}

// One line naming each target of the current pass: format, extent, layer and the two frames it was last rendered in
// (prepare_draw's pipeline wait log).
void vk_exec_targets_describe(VkExec *ex, char *buf, size_t cap) {
    resolve_rt(ex);
    size_t n = 0;
    buf[0] = 0;
    for (int i = 0; i < 9 && n < cap; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if (!t->image) continue;
        TfrTarget ft = tfr_of(t);
        const TargetFrames *e = tfr_find(&ft, 0);
        int w = snprintf(buf + n, cap - n, "%s%c%d fmt %d %ux%u L%u seen %llu/%llu in %u frames", n ? "; " : "",
                         i < 8 ? 'c' : 'd', i < 8 ? i : 0, (int)t->format, t->width, t->height, t->base_layer,
                         (unsigned long long)(e ? e->last : 0), (unsigned long long)(e ? e->prev : 0),
                         e && e->owner == ft.owner ? e->n : 0);
        if (w < 0) break;
        n += (size_t)w;
    }
    if (!n) snprintf(buf, cap, "no targets");
}

// ---- passes and barriers

static int has_image(const VkImage *list, int n, VkImage img) {
    for (int i = 0; i < n; i++)
        if (list[i] == img) return 1;
    return 0;
}

static VkImageMemoryBarrier2 layout_barrier(VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
    return (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                                   .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
}

// Full barrier. The images in the feedback layout become want[] (pass begin) or none (every other barrier): the others
// go back to GENERAL and the new ones leave it, in the same command.
// NX2NAVI_BARRIER_GAME (1; 0: every pass begin and dispatch a full barrier; ab: in the B blocks): the game's own
// barriers order its work, as on the Switch. A pass that
// samples what an earlier one drew, a dispatch that reads what another dispatch wrote, an indirect draw whose arguments
// a dispatch wrote: on the Switch each needs nvnCommandBufferBarrier (ORDER_* and INVALIDATE_* bits) in the game's
// command stream, which the replay keeps as a full barrier (OP_BARRIER). What the GPU orders by itself there is kept: a
// switch between graphics and compute work drains the one before (the 3D and compute classes share the engine). With
// every pass begin and every dispatch a full barrier, 2.3 ms of a 9.7 ms GPU frame in Kokiri Forest was barriers.
// With the switch:
//  - a pass begin after an earlier pass and no dispatch since the last full barrier gets an attachment barrier
//    (colour and depth/stencil output before the next pass's loads and writes: Vulkan orders attachment access only
//    inside one pass instance), after neither nothing;
//  - a dispatch after other dispatches only gets none;
//  - a pass after a dispatch, and a dispatch after a pass, get a full barrier, the drain of the switch;
//  - either gets a full barrier as before after one the replay itself asked for (vk_exec_barrier: its copies,
//    clears, fills, uniform updates and stand-in copies call it before their commands, which sets bar_dirty), and
//    when feedback-loop targets change layout.
// Copies, clears and the game's barriers keep their full barriers.
//
// The default is 1 on NVIDIA GPUs and 0 on every other. The game placed its barriers for an NVIDIA GPU, and what it
// left out is what such a GPU orders by itself; Vulkan asks for a barrier wherever a pass samples what an earlier pass
// drew or a dispatch reads what an earlier one wrote, whether the game recorded one or not. On an RTX 5090 the picture
// was the same with the switch on and off (34 screenshots across A/B blocks in Kokiri Forest) and the GPU time fell by
// 0.5 to 0.6 ms of 5.4 a frame; no other vendor's driver has run it, so there the setting has to be turned on by hand.
static int barrier_game(void) {
    static int mode = -1;
    if (mode < 0) {
        const VkCtx *c = vk_ctx();
        int nvidia = c && c->props.vendorID == 0x10de;
        mode = rt_knob("BARRIER_GAME", nvidia ? "1" : "0");
        if (mode)
            rt_log("vk_exec: NX2NAVI_BARRIER_GAME=%s: passes and dispatches are ordered by the game's barriers",
                   mode == 2 ? "ab" : "1");
    }
    return RT_KNOB_ON(mode);
}

// NX2NAVI_TRACE=perf: barriers per frame by kind (vk_exec_barrier_log)
enum { BS_FULL, BS_GAME, BS_PASS_FULL, BS_PASS_ATT, BS_PASS_NONE, BS_DISP_FULL, BS_DISP_NONE, BS_N };
static uint64_t bstat[BS_N];
static const char *const bs_name[BS_N] = {"replay full", "game", "pass full", "pass attachment", "pass none",
                                          "dispatch full", "dispatch none"};
enum { BR_OTHER, BR_GAME, BR_PASS, BR_DISPATCH };

static void barrier_to_why(VkExec *ex, const VkImage *want, const VkImageAspectFlags *wasp, int nwant, int why);
static void barrier_to(VkExec *ex, const VkImage *want, const VkImageAspectFlags *wasp, int nwant) {
    barrier_to_why(ex, want, wasp, nwant, BR_OTHER);
}

static void attachment_barrier(VkExec *ex) {
    const VkPipelineStageFlags2 st = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = st,
                           .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                           .dstStageMask = st,
                           .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded++;
}

static void barrier_to_why(VkExec *ex, const VkImage *want, const VkImageAspectFlags *wasp, int nwant, int why) {
    vk_exec_pass_end(ex);
    if (why == BR_OTHER) ex->bar_dirty = 1;   // also when it falls together with the last one: its commands follow
    VkImageMemoryBarrier2 ib[18];
    uint32_t n = 0;
    for (int i = 0; i < ex->nfbl; i++)
        if (!has_image(want, nwant, ex->fbl_img[i]))
            ib[n++] = layout_barrier(ex->fbl_img[i], ex->fbl_aspect[i], VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT,
                                     VK_IMAGE_LAYOUT_GENERAL);
    for (int i = 0; i < nwant; i++)
        if (!has_image(ex->fbl_img, ex->nfbl, want[i]))
            ib[n++] = layout_barrier(want[i], wasp[i], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT);
    for (int i = 0; i < nwant; i++) ex->fbl_img[i] = want[i], ex->fbl_aspect[i] = wasp[i];
    ex->nfbl = nwant;
    if (ex->barrier_at == ex->recorded && !n) return;
    if (!n && !ex->bar_dirty && ((why == BR_PASS && !ex->bar_dispatches) || (why == BR_DISPATCH && !ex->bar_passes)) &&
        barrier_game()) {
        if (why == BR_DISPATCH) {
            bstat[BS_DISP_NONE]++;
        } else if (ex->bar_passes) {
            attachment_barrier(ex);
            bstat[BS_PASS_ATT]++;
        } else {
            bstat[BS_PASS_NONE]++;
        }
        return;
    }
    bstat[why == BR_GAME ? BS_GAME : why == BR_PASS ? BS_PASS_FULL : why == BR_DISPATCH ? BS_DISP_FULL : BS_FULL]++;
    if (why != BR_OTHER) ex->bar_dirty = 0;   // after BR_OTHER the replay's own commands follow: the next pass or
    ex->bar_passes = ex->bar_dispatches = 0;    // dispatch orders after them
    vk_exec_frame.fb_layouts += n;
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb,
                           .imageMemoryBarrierCount = n, .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->barrier_at = ++ex->recorded;
}

void vk_exec_barrier(VkExec *ex) { barrier_to(ex, NULL, NULL, 0); }

void vk_exec_barrier_dispatch(VkExec *ex) {
    barrier_to_why(ex, NULL, NULL, 0, BR_DISPATCH);
    ex->bar_dispatches++;
}

void vk_exec_barrier_log(uint64_t frame) {
    static uint64_t last;
    if (!(rt_trace & RT_TRACE_PERF) || frame % 600) return;
    double f = frame > last ? (double)(frame - last) : 1.0;
    last = frame;
    char s[320];
    int o = 0;
    for (int k = 0; k < BS_N; k++) {
        o += snprintf(s + o, sizeof s - o, "%s%s %.1f", k ? ", " : "", bs_name[k], bstat[k] / f);
        bstat[k] = 0;
    }
    rt_log("vk_exec: frame %llu: barriers per frame: %s", (unsigned long long)frame, s);
}

void vk_exec_pass_begin(VkExec *ex) {
    if (ex->in_pass) return;
    resolve_rt(ex);
    if (vk_texture_deferred()) return;   // a chunk's lookup waits for its turn (chunk_record): the op is replayed again
    // A parallel group's targets do not change: its pass hooks, which follow the frame pass by pass, run in its first
    // chunk, which the others start after (chunk_record).
    if (!rw_chunk || !rw_chunk->idx) {
        // the submitting thread past a group: the hooks read what its first chunk's left, and wait for the whole
        // group where they change what its draws read (vk_exec_hook_turn)
        if (!rw_chunk) vk_exec_hooks_wait();
        vk_contact_pass_begin(ex);
        vk_dlaa_pass(ex);   // DLAA runs before the bloom chain's first pass (vk_dlaa.c)
    }
    vk_dlaa_retarget(ex);   // after DLSS ran before the bloom: the passes it moves to the picture's size (vk_dlaa.c)
    // Learned targets (VkTexImage.fb_learned) are attached in the feedback layout
    VkImage want[9];
    VkImageAspectFlags wasp[9];
    int nwant = 0;
    uint32_t fbl = 0;
    for (uint32_t i = 0; i < 9; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if ((i < 8 && i >= ex->pass_ncolor) || !t->image || !t->img || !t->img->fb_learned) continue;
        fbl |= 1u << i;
        if (has_image(want, nwant, t->image)) continue;
        want[nwant] = t->image, wasp[nwant++] = i < 8 ? VK_IMAGE_ASPECT_COLOR_BIT : t->img->aspect;
    }
    barrier_to_why(ex, want, wasp, nwant, BR_PASS);   // passes, clears, copies and dispatches see each other's results
    ex->bar_passes++;
    ex->fbl_mask = fbl;
    ex->fb_dirty = 0;
    ex->fb_read = 0;
    ex->pass_serial++;
    VkRenderingAttachmentInfo ca[8], da = {0}, sa = {0};
    for (uint32_t i = 0; i < ex->pass_ncolor; i++) {
        const VkExecTarget *t = &ex->color[i];
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (!t->image) continue;
        ca[i].imageView = view_of(ex, t);
        ca[i].imageLayout = fbl >> i & 1 ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;    }
    const VkExecTarget *d = &ex->depth;
    if (d->image) {
        VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view_of(ex, d),
                                       .imageLayout = fbl >> 8 & 1 ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT
                                                                   : VK_IMAGE_LAYOUT_GENERAL,
                                       .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        if (d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) da = a;
        if (d->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) sa = a;
    }
    // The start mark goes between the barrier and vkCmdBeginRendering, its end right after vkCmdEndRendering
    // (vk_exec_pass_end): outside the pass, and without breaking it.
    VkPassMark *m = ex->mark = rw_chunk ? NULL : vk_pass_mark_begin(ex->cb, VK_MARK_PASS, ex->fence);   // one ring
    if (m) {
        m->step = ex->mark_passes++;
        m->w = ex->pass_w, m->h = ex->pass_h, m->d = ex->pass_layers;
        m->layer = ex->depth.image ? ex->depth.base_layer : 0;
        m->vw = m->vh = 0;
        m->ncolor = (uint8_t)(ex->pass_ncolor < 8 ? ex->pass_ncolor : 8);
        for (int i = 0; i < m->ncolor; i++) m->color[i] = ex->pass_color[i];
        m->depth = ex->pass_depth ? ex->pass_depth : ex->pass_stencil;
        // The shaders bound now are the first draw's when it repeats the pipeline state; when it does not,
        // vk_exec_draw.c prepare_draw replaces them as it records the new state.
        vk_exec_draw_shaders(&m->first[0], &m->first[1]);
        ex->mark_draws0 = vk_exec_frame.draws;
    }
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {ex->pass_w, ex->pass_h}},
                          .layerCount = ex->pass_layers, .colorAttachmentCount = ex->pass_ncolor,
                          .pColorAttachments = ca,
                          .pDepthAttachment = ex->pass_depth ? &da : NULL,
                          .pStencilAttachment = ex->pass_stencil ? &sa : NULL};
    vkCmdBeginRendering(ex->cb, &ri);
    vk_exec_frame.passes++;
    ex->recorded++;
    ex->in_pass = 1;
    if (rw_chunk && !rw_chunk->idx) __atomic_store_n(&rw_chunk->g->hooks, 1, __ATOMIC_RELEASE);
}

void vk_exec_pass_end(VkExec *ex) {
    if (!ex->in_pass) return;
    vk_exec_oq_pass_end(ex);
    vkCmdEndRendering(ex->cb);
    ex->recorded++;
    ex->in_pass = 0;
    if (ex->mark) {
        // vk_exec_frame_log resets the frame counters at a present (NX2NAVI_TRACE=gfx): then only the draws since it count
        uint64_t d = vk_exec_frame.draws, d0 = ex->mark_draws0;
        ex->mark->draws = (uint32_t)(d >= d0 ? d - d0 : d);
        if (ex->mark->draws) vk_exec_draw_shaders(&ex->mark->last[0], &ex->mark->last[1]);
        vk_pass_mark_end(ex->cb, ex->mark);
        ex->mark = NULL;
    }
    for (int i = 0; i < ex->pass_ncolor && i < 8; i++)   // stand-ins of 3D BC targets go to their textures
        if (ex->color[i].image && ex->color[i].img && ex->color[i].img->image != ex->color[i].image)
            bc_copy_back(ex, &ex->color[i]);
    if (vk_dump_armed) vk_dump_pass_end(ex);
    uint64_t stamp = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    // The targets are marked written outside tfr_lock: vk_texture_gpu_written takes the texture lock, and a chunk that
    // holds the texture lock can end its pass here (a lookup's alias copy, vk_exec_inline_begin) and then wait for
    // tfr_lock. Taken the other way round, a chunk holding tfr_lock and waiting for the texture lock deadlocked with it,
    // the other chunks spinning in their turns (the loading screen after Continue, 3 replay workers). tfr_lock guards
    // only the target history and takes no other lock inside.
    for (int i = 0; i < 8; i++)
        if (ex->color[i].image) vk_texture_gpu_written(ex->color[i].tex);
    if (ex->depth.image) vk_texture_gpu_written(ex->depth.tex);
    int lk = vk_exec_shared();
    if (lk) plat_mutex_lock(&tfr_lock);
    for (int i = 0; i < 8; i++)
        if (ex->color[i].image) {
            TfrTarget ft = tfr_of(&ex->color[i]);
            tfr_note(&ft, stamp);
        }
    if (ex->depth.image) {
        TfrTarget ft = tfr_of(&ex->depth);
        tfr_note(&ft, stamp);
    }
    if (lk) plat_mutex_unlock(&tfr_lock);
    vk_dlaa_pass_end(ex);   // a jittered pass left its targets' outer rows or columns undrawn
}

VkCommandBuffer vk_exec_inline_begin(int *reopen) {
    VkExec *ex = x_cur;
    *reopen = 0;
    if (!ex || !ex->cb) return VK_NULL_HANDLE;
    *reopen = ex->in_pass;
    vk_exec_barrier(ex);   // ends the pass; feedback-layout targets go back to GENERAL
    return ex->cb;
}

void vk_exec_inline_end(int reopen) {
    VkExec *ex = x_cur;   // set: the caller's vk_exec_inline_begin handed out its command buffer
    ex->recorded++;
    if (reopen) vk_exec_pass_begin(ex);
}

void vk_exec_probe_inline(VkCommandBuffer cb, uint64_t fence) {
    X.cb = cb, X.fence = fence;
    x_cur = cb != VK_NULL_HANDLE ? &X : NULL;
}

typedef struct { VkBuffer b; VmaAllocation a; } InlineBuf;
static void free_inline_buf(void *arg) {   // completion thread
    InlineBuf *t = arg;
    vmaDestroyBuffer(vk_ctx()->vma, t->b, t->a);
    free(t);
}

VkBuffer vk_exec_inline_buffer(VkDeviceSize size) {
    InlineBuf *t = calloc(1, sizeof *t);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!t || vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &t->b, &t->a, NULL) != VK_SUCCESS) { free(t); return VK_NULL_HANDLE; }
    vk_completion_add(x_cur->fence, free_inline_buf, t);   // inside vk_exec_inline_begin and _end
    return t->b;
}

VkRect2D vk_exec_scissor(const VkExec *ex) {
    int64_t w = ex->pass_w, h = ex->pass_h;
    if (!ex->has_scissor) return (VkRect2D){{0, 0}, {(uint32_t)w, (uint32_t)h}};
    int64_t k = ex->pass_scale > 1 ? ex->pass_scale : 1;
    int64_t x0 = ex->scissor.x * k, y0 = ex->scissor.y * k;
    int64_t x1 = x0 + (int64_t)ex->scissor.w * k, y1 = y0 + (int64_t)ex->scissor.h * k;
    float sx, sy;
    if (vk_dlaa_target_scale(ex, &sx, &sy)) {   // a pass DLSS moved to the picture's size (vk_dlaa.c vk_dlaa_retarget)
        x0 = (int64_t)floor(ex->scissor.x * (double)sx), y0 = (int64_t)floor(ex->scissor.y * (double)sy);
        x1 = (int64_t)ceil(((double)ex->scissor.x + ex->scissor.w) * sx);
        y1 = (int64_t)ceil(((double)ex->scissor.y + ex->scissor.h) * sy);
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x1 <= x0 || y1 <= y0) return (VkRect2D){{0, 0}, {0, 0}};
    return (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
}

// ---- clears

// Outside a pass, a clear of a whole target (no narrower scissor, the target as large as the render area) clears the
// image directly instead of opening a pass for it.
static int whole_target(const VkExec *ex, const VkExecTarget *t) {
    // vkCmdClear*Image addresses a 3D level whole, not one depth plane: slices of a 3D target clear in a pass; it
    // cannot clear a compressed image: views of BC blocks clear in a pass too
    if (ex->in_pass || t->is3d || t->blocks || t->width != ex->pass_w || t->height != ex->pass_h) return 0;
    if (!ex->has_scissor) return 1;
    const p_scissor *s = &ex->scissor;
    return s->x <= 0 && s->y <= 0 && (int64_t)s->x + s->w >= t->width && (int64_t)s->y + s->h >= t->height;
}

static void clear_attachment(VkExec *ex, const VkClearAttachment *a) {
    vk_exec_pass_begin(ex);
    VkClearRect cr = {vk_exec_scissor(ex), 0, ex->pass_layers};
    if (!cr.rect.extent.width || !cr.rect.extent.height) return;
    uint32_t bit = a->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT ? 1u << a->colorAttachment : 0x100u;
    vk_exec_feedback_war(ex, bit);   // a draw of this pass sampled it: its reads finish before the clear writes
    vkCmdClearAttachments(ex->cb, 1, a, 1, &cr);
    ex->recorded++;
    ex->fb_dirty |= bit;
}

// NX2NAVI_LOG_RCOND=1: clears issued under a render condition (the first 400), run or skipped, with their target.
static void clear_cond_log(const VkExec *ex, const char *what, int index) {
    static int on = -1, n;
    if (on < 0) on = rt_knob("LOG_RCOND", "0");
    if (on == 2 && index < 0 && ex->has_render_cond && ex->depth.width >= 1600) {   // full-size depth clears, all of them
        char why[256];
        vk_exec_cond_why(&ex->render_cond, why, sizeof why);
        rt_log("SEQ depth clear %#llx %s; %s", ex->depth.img ? (unsigned long long)ex->depth.img->addr : 0ull,
               ex->render_disabled ? "skipped" : "RUN", why);
        return;
    }
    if (!on || !ex->has_render_cond || n >= 400) return;
    n++;
    const VkExecTarget *t = index < 0 ? &ex->depth : index < (int)ex->pass_ncolor ? &ex->color[index] : NULL;
    rt_log("vk: %s clear %d under the condition at %#llx (mode %d): %s, target %ux%u", what, index,
           (unsigned long long)ex->render_cond.addr, ex->render_cond.mode, ex->render_disabled ? "skipped" : "run",
           t ? t->width : 0, t ? t->height : 0);
}

static void clear_color(VkExec *ex, const p_clear_color *c) {
    vk_exec_frame.clears++;
    clear_cond_log(ex, "colour", c->index);
    if (ex->has_render_cond) ex->render_disabled ? vk_cond_clears_skip++ : vk_cond_clears_run++;
    if (ex->render_disabled) return;
    resolve_rt(ex);
    if (vk_texture_deferred()) return;
    int i = c->index;
    if (i < 0 || i >= (int)ex->pass_ncolor || !ex->color[i].image) {
        warn_once(W_CLEAR_TARGET, "colour clear of target %d, which is not bound or not renderable: skipped", i);
        return;
    }
    uint32_t mask = (uint32_t)c->mask & 0xf;
    if (!mask) return;
    if (mask != 0xf) {   // vkCmdClearAttachments ignores write masks; a masked clear needs a draw (vk_exec_draw.c)
        warn_once(W_CLEAR_MASK, "colour clear with channel mask %#x is not replayed: skipped", mask);
        return;
    }
    const VkExecTarget *t = &ex->color[i];
    VkClearColorValue cv;
    memcpy(cv.float32, c->rgba, sizeof cv.float32);   // integer targets: the payload's bits as the front end stored them
    if (whole_target(ex, t)) {
        vk_exec_barrier(ex);
        VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, t->level, 1, t->base_layer, t->layers};
        vk_gpu_gap_note(VK_GAP_CLEAR_TARGET, (uint64_t)t->width * t->height);
        vkCmdClearColorImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &r);
        ex->recorded++;
        vk_texture_gpu_written(t->tex);
        return;
    }
    VkClearAttachment a = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)i};
    a.clearValue.color = cv;
    clear_attachment(ex, &a);
}

static void clear_ds(VkExec *ex, const p_clear_ds *c) {
    clear_cond_log(ex, "depth", -1);
    if (ex->has_render_cond) ex->render_disabled ? vk_cond_clears_skip++ : (vk_cond_clears_run++, vk_cond_depth_run++);
    if (ex->render_disabled) return;
    resolve_rt(ex);
    if (vk_texture_deferred()) return;
    const VkExecTarget *t = &ex->depth;
    if (!t->image) return;
    VkImageAspectFlags asp = 0;
    if (c->depth_mask && (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
    uint32_t sm = (uint32_t)c->stencil_mask & 0xff;
    if (sm && (t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
        if (sm == 0xff) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
        else warn_once(W_STENCIL_MASK, "stencil clear with mask %#x is not replayed: skipped", sm);
    }
    if (!asp) return;
    float d = c->depth < 0.0f ? 0.0f : c->depth > 1.0f ? 1.0f : c->depth;
    VkClearDepthStencilValue v = {d, (uint32_t)c->stencil & 0xff};
    if (whole_target(ex, t)) {
        vk_exec_barrier(ex);
        VkImageSubresourceRange r = {asp, t->level, 1, t->base_layer, t->layers};
        vk_gpu_gap_note(VK_GAP_CLEAR_TARGET, (uint64_t)t->width * t->height);
        vkCmdClearDepthStencilImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
        ex->recorded++;
        vk_texture_gpu_written(t->tex);
        return;
    }
    VkClearAttachment a = {.aspectMask = asp};
    a.clearValue.depthStencil = v;
    clear_attachment(ex, &a);
}

// ---- the op loop

static void set_rt(VkExec *ex, const p_set_rt *r) {
    if (!memcmp(&ex->rt, r, sizeof *r)) return;   // the same targets again: the pass goes on
    vk_exec_pass_end(ex);
    ex->rt = *r;
    ex->rt_resolved = 0;
}

// ---- command buffer parts
// NVN starts the GPU as soon as commands are submitted; one Vulkan command buffer per submission starts it only after
// the whole replay. Between passes, once NX2NAVI_SPLIT_DRAWS (64; 0 off) draws and dispatches were recorded since the
// last part, the part goes to the queue (vk_sync_split) and the replay continues in a new command buffer. Only
// command-buffer state is lost there (bindings and dynamic state: vk_exec_draw_new_cb, ex->dirty); barriers, layouts and
// queries are not: a barrier's first scope is everything earlier in submission order, image layouts belong to the image,
// and occlusion queries end with their pass. Fences, completion callbacks and reports stay per submission: only the last
// part signals the value. The texture pool set a part binds is sealed when the part goes (vk_exec_pool_split): later
// parts write and bind another, and a part is not split off while no other set is free of this submission.
// A part also spans at least NX2NAVI_SPLIT_US (1000) of replay time: a split costs about 40 us of the render thread
// (vkQueueSubmit2, a command buffer, the mirror prologue), and a fight frame of 2,000 draws made 15 parts on the 7900X;
// a millisecond of recording is a fraction of a millisecond of GPU work, so the GPU still starts early.
// With NX2NAVI_SPLIT_IDLE=1, neither limit holds the first part of a submission back while the GPU has run everything
// submitted: it goes at the first pass boundary after a draw or dispatch. Each frame the game's thread waits for the GPU
// to run the frame's first submission (nvn_api_core.c nvnQueueSubmitCommands, NX2NAVI_SUBMIT_FINISH), so the GPU is idle
// when the next one replays, and waited for the first 64 draws and 1 ms of it in Kokiri Forest (2026-11-09). Off by
// default: with it the foliage of Kokiri Forest flickered (RTX 5090, 2026-11-09), and the parts it cuts that early are
// the one thing it changes; why they matter is not known yet.
// Both limits also split inside a pass (NX2NAVI_SPLIT_IN_PASS, on): the pass ends there and the next draw begins it
// again in the new part, as after a barrier (attachments load and store; occlusion queries continue in a new segment).
// Kokiri Forest's two scene passes of ~500 draws take 2-4 ms each to replay and most of the frame's GPU time; split only
// between passes, the GPU waited for each whole pass, idle up to 3.6 ms a frame (2026-11-09).
// After the time limit held a part back, the clock is read again only 8 draws later (split_at): read after every
// command, it was 4% of the replay in Kokiri Forest.
static int split_n = -1, split_idle, split_in_pass, split_draws, split_at, part_start;
static uint64_t split_ns, part_t0;
static uint64_t n_parts, n_subs, n_idle_parts, n_pass_splits, n_cuts;

// ---- the early tail (NX2NAVI_TAIL_EARLY, default 1 since 0.10.0; 0 turns it off)
// Standing in Hyrule Field with FSR 4 (RX 7900 XT, 2026-11-13) a frame's main submission ends with one part of 4.7 ms
// of GPU time, the temporal upscaler and the interface drawn after it, and one of 1.4 ms, the write-backs of what the
// shaders wrote into VRAM mirrors. The game waits for all of it before it writes the next frame's data (the wait after
// a frame's first submission, nvn_api_core.c, and its own fence waits), about 7 ms in which the replay thread has
// nothing to record. What the game reads of the submission is done before the upscaler runs: the occlusion and
// counter reports, the event values, and the write-backs' bytes, which only came last. After it the stream holds two
// timestamp reports, and inline uniform updates for the interface's draws (64 bytes into one mirrored block, some
// thirty times), and nothing else the CPU sees.
// So from the upscaler on (vk_dlaa.c evaluate) the replay records the tail into a command buffer of its own
// (vk_sync_cut), without splits or parallel groups. An inline uniform update of a mirrored block in it takes no
// write-back: the GPU copies the bytes into the mirror in stream order as ever, and guest memory gets them from the
// CPU with the submission's results (vk_exec_draw.c op_update_ub, vk_exec_xfer.c vk_exec_tail_ub). At the submission's
// end, when the tail turned out to hand the CPU nothing but timestamps (vk_exec_tail_results), to write no other pool
// memory (vk_mem_gpu_wrote_count) and to bind no memory the GPU reads from the game's own pages (vk_mem_live_swap: a
// pool without a mirror, a device address table),
// the write-backs are recorded into a command buffer that goes to the queue before the tail's, with the executed
// timeline's signal (vk_sync_tail_hold, vk_sync_tail_release): the GPU runs the same commands, the write-backs before
// the tail. The mirror copies the tail's first uses queued go with that part too, so they read the game's pages before
// the signal. The game's waits then end at that signal and the results the game reads (vk_sync.c vk_sync_exec_wait,
// vk_sync_game_wait), and the GPU runs the tail while the game and the replay are at the next frame. A timestamp
// reported in the tail is written then with the time the tail is expected to end at (vk_exec_xfer.c counter_cb), as
// the game measures the frame's GPU time with it. A tail that fails a test goes as before, the write-backs after it.
// Measured there, 150 s runs, each pair in one batch: 63.5 against 49.4 fps with three replay workers; without them
// 50.7 against 42.5 standing and 56.0 against 45.9 along the field route, and with FSR 3.1, whose tail the GPU runs
// for 1.2 ms where FSR 4's takes 3.5, 51.8 against 47.8 and 54.9 against 52.3. All but a few of the tails passed the
// tests there; in area loads and some places a tail hands the CPU results and goes as before. The pictures matched
// those without it standing, along the route and through warps to five areas. Not measured with the lazy write-back
// (NVIDIA's and Intel's default), where a tail's uniform updates are noted as writes and no tail goes early.
// Not with shadow storage, emulated mesh shaders, a frame dump or a pass capture, nor inside a chunk or a continuation.
static struct {
    int on;                  // the submission's tail is being recorded
    int live0;               // vk_mem_live_swap's note from before it
    uint64_t wrote0;         // vk_mem_gpu_wrote_count when it began
    // since the last perf line: tails; those that went early; those refused by results, writes, late reads, other
    uint64_t n, early, why[4];
} TL;
int vk_exec_tail_results(void);   // vk_exec_xfer.c
void vk_sync_tail_stats(uint64_t *tails, uint64_t *waits, double *ms);
int vk_gpu_passes_capturing(void);
int vk_mesh_shader_native(void);   // vk_device.c
int vk_exec_tail(void) { return TL.on; }

static int tail_enabled(void) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2NAVI_TAIL_EARLY");
        if (on)
            rt_log("vk: the write-backs go before a frame's tail, and the game's waits end there (NX2NAVI_TAIL_EARLY)");
    }
    return on;
}

int vk_exec_split_now(VkExec *ex) {
    int in_pass = ex->in_pass;
    if (in_pass) vk_exec_pass_end(ex);
    VkCommandBuffer cb = vk_sync_cut();   // not handed to the queue: the texture pool set stays (vk_exec_pool_split)
    if (!cb) return 0;
    ex->cb = cb;
    ex->dirty = VK_EXEC_DIRTY_ALL;
    vk_exec_draw_new_cb();
    n_cuts++;
    return 1;
}

static int split_wanted(void) {
    if (split_draws >= split_at) return 1;
    // the first part only: later parts of the submission run on the GPU, while done and issued stay as they were
    return split_idle && split_draws && !part_start && vk_sync_fence_done() >= vk_sync_issued();
}

static void maybe_split(VkExec *ex) {
    if (split_n < 0) {
        split_n = cfg_int("NX2NAVI_SPLIT_DRAWS");
        if (split_n < 0) split_n = 0;
        int us = cfg_int("NX2NAVI_SPLIT_US");
        split_ns = us > 0 ? (uint64_t)us * 1000 : 0;
        split_idle = split_n && cfg_bool("NX2NAVI_SPLIT_IDLE");
        split_in_pass = split_n && cfg_bool("NX2NAVI_SPLIT_IN_PASS");
        rt_log("vk: replay command buffer split every %d draws %s, parts of at least %d us%s%s", split_n,
               split_in_pass ? "(inside passes too)" : "between passes", us > 0 ? us : 0, split_n ? "" : " (off)",
               split_idle ? "; a first part at once while the GPU is idle" : "");
        split_at = split_n ? split_n : INT_MAX;
    }
    if (!split_n || TL.on) return;   // the tail stays one command buffer (the early tail)
    if (Q.drained < Q.out) {   // groups the submitting thread went on past go into the part first, once done
        if (__atomic_load_n(&Q.done, __ATOMIC_ACQUIRE) < Q.out) return;   // split_draws stays: a later op asks again
        group_drain(ex, PW_SPLIT);
    }
    // the first part while the GPU is idle: at a pass boundary only, where a split costs no pass restart
    int idle = !ex->in_pass && split_idle && split_draws && !part_start && vk_sync_fence_done() >= vk_sync_issued();
    if (!idle) {
        if (ex->in_pass && !split_in_pass) return;
        if (split_draws < split_n) return;
        if (split_ns && kern_now_ns() - part_t0 < split_ns) {   // split_draws stays: a later draw asks again
            split_at = split_draws + 8;
            return;
        }
    }
    split_draws = 0;
    split_at = split_n;
    if (ex->recorded == part_start || !vk_exec_pool_split(ex->fence)) return;
    if (idle) n_idle_parts++;
    if (ex->in_pass) vk_exec_pass_end(ex), n_pass_splits++;   // the next draw begins the pass again, in the new part
    ex->cb = vk_sync_split();
    part_start = ex->recorded;
    part_t0 = kern_now_ns();
    ex->dirty = VK_EXEC_DIRTY_ALL;
    vk_exec_draw_new_cb();
    n_parts++;
}

// ---- event order across the game's queues
// Ocarina runs work on a second queue that hands results back and forth with the main one by events: each frame the
// worker submits first, its command buffer waits (WaitEvent) for a signal the main queue gives in its submission a few
// submits later, does a step, signals, waits for the next point of the main queue's frame, and so on, while the main
// queue waits for the worker's signals before it uses the results (Kokiri Forest: about 13 waits a frame, 7 of them
// reached before their signal in submission order). On one Vulkan queue in submission order the worker's steps would
// run before the work they wait for and read the previous frame's results: Link and the other characters were
// drawn much too dark, at any time of day.
// So a top-level wait whose condition does not hold in replay order (vk_exec_wait_event: memory, or the signals
// replayed so far) stops its submission there: the rest becomes a continuation of that queue (Cont), and later
// submissions of the queue line up behind it. After every replayed signal the continuations whose wait now holds go
// on, inside the command buffer being recorded: the open pass ends, a full barrier orders them after what came before
// (the event's memory dependency), the queue's own state replaces the running one while they run, and another barrier
// and the running state come back after. One that would still wait after CONT_MAX submissions goes on regardless, as
// it did before (counted, logged). Only with the replay on the submitting thread: the recording copies a continuation
// reads stay alive there until the game recycles them, which it does only after the work ran on the console.
// NX2NAVI_EVENT_ORDER=0: every submission in the order it came.
#define CONT_MAX 36
#define EVO_QUEUES 16
typedef struct Cont {
    struct Cont *next;      // the next submission of the same queue
    uint64_t *h;            // private copies of the recordings it has still to walk (cont_new), the one being walked
    int n, i;
    const op_hdr *at;       // the wait it stopped at in h[i] (NULL: not started)
    uint64_t since;         // vk_exec_submit_seq when it stopped
    int has_st;             // st holds the queue's state (else the running state carries over, as for any submission)
    uint8_t *st;
} Cont;
static struct { uintptr_t q; Cont *head, *tail; } evq[EVO_QUEUES];
static int evo_on = -1, evo_in;     // knob; inside a continuation
static uintptr_t evo_queue;         // queue of the stream being walked (0: it cannot stop at a wait)
static int evo_pending;             // continuations held
static uint64_t evo_held, evo_resumed, evo_forced, evo_switches;   // since the last perf line
int vk_exec_held(void) { return evo_pending; }
#define ST_LO offsetof(VkExec, rt)
#define ST_HI offsetof(VkExec, views)

static const op_hdr *walk(VkExec *ex, const op_hdr *h, int depth);

// ---- event order trace (NX2NAVI_EVO_TRACE=1, diagnostics)
// The order in which a frame's submissions, waits, signals and held parts were replayed, as a string of tokens:
// |<queue>.<n> a submission of n command buffers on that queue; p it lines up behind held work of its queue; m<e> a
// wait on event e that holds; u<e> one that does not; h the submission stops there; s<e>=<v> a replayed signal; r<queue>
// held work of that queue goes on; w it stops at a wait again; d it is through; F it went on without its signal;
// c<e>=<v> the CPU signals an event (nvnEventSignal, on whatever thread, where the call came between the others); L<e>
// a wait that holds only by a CPU signal that came after the submission took its copies (vk_exec_xfer.c "events the
// CPU signals"). Events and queues are numbered in the order they first appear; a value of 16 or more is written n,
// as a counter would make every frame's string its own. Every distinct string is logged once with its number, and the
// numbers of the frames are logged sixty to a line, so the frames that were replayed in another order than usual can
// be set against the pictures of the flight recorder.
#define EVT_KINDS 512
static struct {
    int on;
    PlatMutex lock;
    char buf[2048];
    int n, over;
    const void *ev[64];
    int nev;
    char *kind[EVT_KINDS];
    int nkind;
    uint64_t line_first;
    int line_n;
    char line[512];
    int line_len;
} ET = {.on = -1, .lock = PLAT_MUTEX_INIT};

static int evt_on(void) {
    if (ET.on < 0) ET.on = rt_knob("EVO_TRACE", "0");
    return ET.on;
}

void vk_exec_evt(char what, const void *event, uint32_t value) {
    if (!evt_on()) return;
    char t[40];
    int e = -1;
    plat_mutex_lock(&ET.lock);
    if (event) {
        for (e = 0; e < ET.nev && ET.ev[e] != event; e++) {}
        if (e == ET.nev && ET.nev < 64) ET.ev[ET.nev++] = event;
    }
    if (what == 's' || what == 'c') snprintf(t, sizeof t, value < 16 ? "%c%d=%u " : "%c%d=n ", what, e, value);
    else if (what == '|') snprintf(t, sizeof t, "|%d.%u ", e, value);
    else if (event) snprintf(t, sizeof t, "%c%d ", what, e);
    else snprintf(t, sizeof t, "%c ", what);
    for (const char *s = t; *s; s++) {
        if (ET.n >= (int)sizeof ET.buf - 1) { ET.over = 1; break; }
        ET.buf[ET.n++] = *s;
    }
    plat_mutex_unlock(&ET.lock);
}

void vk_exec_evt_frame(uint64_t frame) {
    if (!evt_on()) return;
    plat_mutex_lock(&ET.lock);
    ET.buf[ET.n] = 0;
    int k = 0;
    while (k < ET.nkind && strcmp(ET.kind[k], ET.buf)) k++;
    if (k == ET.nkind && ET.nkind < EVT_KINDS) {
        size_t len = strlen(ET.buf) + 1;
        if ((ET.kind[k] = malloc(len))) {
            memcpy(ET.kind[k], ET.buf, len);
            ET.nkind++;
            rt_log("evo: order %d first at frame %llu%s: %s", k, (unsigned long long)frame, ET.over ? " (cut)" : "", ET.buf);
        }
    }
    if (!ET.line_n) ET.line_first = frame, ET.line_len = 0;
    ET.line_len += snprintf(ET.line + ET.line_len, sizeof ET.line - (size_t)ET.line_len, " %d", k);
    if (++ET.line_n == 60 || ET.line_len > (int)sizeof ET.line - 16) {
        rt_log("evo: frames %llu-%llu:%s", (unsigned long long)ET.line_first, (unsigned long long)frame, ET.line);
        ET.line_n = 0;
    }
    ET.n = 0, ET.over = 0;
    plat_mutex_unlock(&ET.lock);
}

static int evo_enabled(void) {
    if (evo_on < 0) {
        const char *e = cfg_get("NX2NAVI_EVENT_ORDER");
        evo_on = !(e && !strcmp(e, "0"));
        if (evo_on && gfx_replay_async()) {
            evo_on = 0;
            rt_log("vk: event order across queues off: the replay runs on its own thread (NX2NAVI_REPLAY_ASYNC)");
        } else rt_log("vk: event order across queues %s (NX2NAVI_EVENT_ORDER)", evo_on ? "on" : "off");
    }
    return evo_on;
}

static int evq_slot(uintptr_t q) {
    int free_i = -1;
    for (int i = 0; i < EVO_QUEUES; i++) {
        if (evq[i].q == q) return i;
        if (free_i < 0 && !evq[i].head) free_i = i;
    }
    if (free_i >= 0) evq[free_i].q = q;
    return free_i;
}

// Holds h[from..n), stopped at `at` in h[from] (NULL: from its start). The recordings are copied: the game records a
// command buffer again once the fence it took after the submission is done, and that fence does not wait for a held
// part (it signals with the submission that held it).
static Cont *cont_new(const uint64_t *h, int n, int from, const op_hdr *at) {
    Cont *c = calloc(1, sizeof *c);
    if (!c || !(c->h = calloc((size_t)(n - from) + 1, sizeof *c->h))) rt_fatal("vk: out of memory (held submission)");
    for (int k = from; k < n; k++) {
        const op_hdr *x = nvn_op_first(h[k]);
        uint64_t bytes = 0;
        for (; x->op != OP_END; x = nvn_op_next(x)) bytes += x->size;
        bytes += x->size;
        uint8_t *copy = malloc(bytes);
        if (!copy) rt_fatal("vk: out of memory (held submission, %llu bytes)", (unsigned long long)bytes);
        memcpy(copy, (const void *)(uintptr_t)h[k], bytes);
        c->h[k - from] = (uintptr_t)copy;
        if (k == from && at) c->at = (const op_hdr *)(const void *)(copy + ((const uint8_t *)at - (const uint8_t *)(uintptr_t)h[k]));
    }
    c->n = n - from;
    c->since = vk_exec_submit_seq;
    return c;
}

static void cont_free(Cont *c) {
    for (int k = 0; k < c->n; k++) free((void *)(uintptr_t)c->h[k]);
    free(c->h), free(c->st), free(c);
}

static void cont_push(int slot, Cont *c) {
    if (evq[slot].tail) evq[slot].tail->next = c;
    else evq[slot].head = c;
    evq[slot].tail = c;
    evo_pending++;
}

// Swaps the queue state in ex with buf (ST_HI - ST_LO bytes); what the swapped-in state names is bound again.
static void st_swap(VkExec *ex, uint8_t *buf) {
    static uint8_t tmp[ST_HI - ST_LO];
    memcpy(tmp, (uint8_t *)ex + ST_LO, sizeof tmp);
    memcpy((uint8_t *)ex + ST_LO, buf, sizeof tmp);
    memcpy(buf, tmp, sizeof tmp);
    ex->rt_resolved = 0;
    ex->dirty = VK_EXEC_DIRTY_ALL;
    ex->tex_serial++;
    vk_exec_draw_new_cb();
}

static int cont_ready(const Cont *c) {
    return !c->at || vk_exec_event_met(nvn_op_payload(c->at)) || vk_exec_submit_seq - c->since > CONT_MAX;
}

// ---- the gate: a submission that begins with a wait for the CPU's signal (NX2NAVI_GATE_WAIT_MS, default 10; 0 off)
// Ocarina's main queue begins with a wait for an event the game signals from the CPU once it has written what the
// frame reads (vk_exec_xfer.c "events the CPU signals"). The render thread submits that queue without waiting for the
// signal, and polls the event afterwards, a millisecond at a time, before it presents. In Hyrule Field the signal
// came 1 to 2 ms after the submit call in about every other frame: the main thread first waits for the previous
// frame's fence and reads back what it needs. The submission then took its copies (the mirror sync and the uniform
// snapshot, 1.1 to 1.7 ms), met the wait unmet and was held; the render thread polled, presented, and the present
// replayed the frame (vk_backend.c held_resume) under a value the game's fence does not stand for, so the game's next
// wait for that fence returned with the GPU still at work and the frame after was replayed inside its submit call
// again: frames of 23 and 15 ms in turn where each takes 17.
// So when a submission would stop at such a wait before it has recorded anything, the submit call waits for the
// signal here, before the copies are taken, as the console's GPU waits at the wait itself: the copies are then
// taken after the game has finished writing, and the frame is replayed in the call the game's fence stands for.
// Only state may precede the wait in the command buffer, and the event must be one the CPU has signalled before;
// the wait ends after NX2NAVI_GATE_WAIT_MS at the latest, and the submission is then held as before.
static struct { uint64_t n, ns, late; } GW;   // since the last perf line: waits, their time, waits that ran out

static const void *gate_first_wait(const op_hdr *h) {
    for (int k = 0; k < 256; k++) {
        if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_) return NULL;
        const void *a = nvn_op_payload(h);
        switch (h->op) {
        case OP_WAIT_EVENT: return a;
        case OP_JUMP: h = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target; continue;
        // state the walk only stores: nothing is recorded and nothing reaches the CPU before the wait
        case OP_BEGIN: case OP_SET_RT: case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_BIND_PROGRAM:
        case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_UBO:
        case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL: case OP_POLY_OFFSET:
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: case OP_COPY_STRIDE: case OP_BLEND_COLOR: case OP_DEPTH_CLAMP:
        case OP_RASTER_DISCARD: case OP_PRIM_RESTART: case OP_PUSH_CC:
            break;
        default: return NULL;   // a draw, a copy, a signal, a call: the walk decides, as before
        }
        h = nvn_op_next(h);
    }
    return NULL;
}

void vk_exec_gate_wait(const uint64_t *handles, int n) {
    static int64_t cap_ns = -1;
    if (cap_ns < 0) {
        int ms = cfg_int("NX2NAVI_GATE_WAIT_MS");
        cap_ns = ms > 0 ? (int64_t)ms * 1000000 : 0;
    }
    if (!cap_ns || n < 1 || !handles[0] || !gfx_submit_queue || !evo_enabled()) return;
    const void *w = gate_first_wait(nvn_op_first(handles[0]));
    if (!w || vk_exec_event_holds_now(w) || !vk_exec_event_cpu_known((const void *)((const p_wait_event *)w)->val)) return;
    uint64_t t0 = kern_now_ns(), now = t0;
    for (unsigned i = 0; !vk_exec_event_holds_now(w); i++) {
        if ((i & 15) == 15 && (now = kern_now_ns()) - t0 >= (uint64_t)cap_ns) {
            GW.late++;
            break;
        }
        if (i < 20000) plat_pause();
        else plat_yield();
    }
    GW.n++, GW.ns += kern_now_ns() - t0;
    if (GW.n >= 600) {
        if (rt_trace & RT_TRACE_PERF)
            rt_log("perf: gate: %llu submissions waited for the CPU's signal before their copies, %.2f ms on average; "
                   "%llu waited %lld ms without it and were held", (unsigned long long)GW.n, (double)GW.ns / (double)GW.n / 1e6,
                   (unsigned long long)GW.late, (long long)(cap_ns / 1000000));
        GW.n = GW.ns = GW.late = 0;
    }
}

int vk_exec_held_ready(void) {
    for (int s = 0; evo_pending && s < EVO_QUEUES; s++)
        if (evq[s].head && cont_ready(evq[s].head)) return 1;
    return 0;
}

// Held work that goes on in a submission without commands of its own (the present thread's empty one after its blit,
// vk_backend.c present_main, or the one a waiting present makes, present_held) gets what any submission's recording
// gets: the VRAM mirror sync before and the write-back after (vk_mem_mirror_begin/end), and a uniform copy (vk_usnap).
// Without them its draws read mirrors that missed the CPU's writes since the last submission, and its GPU writes never
// went back to guest memory: the next vk_mem_mirror_begin dropped the write-back list. NX2NAVI_HELD_SYNC=0: as before.
static int held_sync(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("HELD_SYNC", "1");
    return on;
}

// Walks the continuation from where it stopped; 1 when it stopped at another wait (c->i, c->at updated), 0 when done.
static int cont_walk(VkExec *ex, Cont *c) {
    if (c->at && !vk_exec_event_met(nvn_op_payload(c->at))) {
        evo_forced++;
        vk_exec_evt('F', NULL, 0);
        static int logged;
        if (logged++ < 8)
            rt_log("vk: event order: a held submission goes on after %llu submissions without its signal",
                   (unsigned long long)(vk_exec_submit_seq - c->since));
    }
    for (; c->i < c->n; c->i++) {
        const op_hdr *from = c->at ? nvn_op_next(c->at) : nvn_op_first(c->h[c->i]);
        c->at = NULL;
        const op_hdr *stop = walk(ex, from, 0);
        if (stop) {
            c->at = stop;
            c->since = vk_exec_submit_seq;
            return 1;
        }
    }
    return 0;
}

// Runs the held continuations whose wait holds now, in the command buffer being recorded, until none can go on.
static void evo_resume(VkExec *ex) {
    if (evo_in || !evo_pending) return;
    vk_exec_signalled = 0;
    uintptr_t run_q = evo_queue;
    for (int progress = 1; progress;) {
        progress = 0;
        for (int s = 0; s < EVO_QUEUES; s++) {
            Cont *c = evq[s].head;
            if (!c || !cont_ready(c)) continue;
            progress = 1;
            evo_switches++;
            vk_exec_brk++;
            if (RR.on > 0) runs_break(RB_HELD);
            vk_exec_pass_end(ex);
            vk_exec_barrier(ex);
            uint8_t *mine = malloc(ST_HI - ST_LO);
            if (!mine) rt_fatal("vk: out of memory (queue state)");
            memcpy(mine, (uint8_t *)ex + ST_LO, ST_HI - ST_LO);
            if (c->has_st) st_swap(ex, c->st);
            evo_in = 1, evo_queue = evq[s].q;
            for (;;) {
                evo_resumed++;
                vk_exec_evt('r', (const void *)evq[s].q, 0);
                if (cont_walk(ex, c)) {   // waits again: the queue's state stays with it
                    vk_exec_evt('w', NULL, 0);
                    vk_exec_pass_end(ex);
                    if (!c->st && !(c->st = malloc(ST_HI - ST_LO))) rt_fatal("vk: out of memory (queue state)");
                    memcpy(c->st, (uint8_t *)ex + ST_LO, ST_HI - ST_LO);
                    c->has_st = 1;
                    break;
                }
                evq[s].head = c->next;   // done: the queue's next submission starts with the state this one left
                if (!evq[s].head) evq[s].tail = NULL;
                evo_pending--;
                vk_exec_evt('d', NULL, 0);
                cont_free(c);
                c = evq[s].head;
                if (!c) break;
                c->has_st = 0;
            }
            evo_in = 0, evo_queue = run_q;
            vk_exec_pass_end(ex);
            vk_exec_barrier(ex);
            memcpy((uint8_t *)ex + ST_LO, mine, ST_HI - ST_LO);   // the running stream's state back
            ex->rt_resolved = 0;
            ex->dirty = VK_EXEC_DIRTY_ALL;
            ex->tex_serial++;
            vk_exec_draw_new_cb();
            free(mine);
        }
    }
}

// ---- looking ahead for pipelines (NX2NAVI_PIPE_AHEAD, default 1; 0 off)
// A draw whose pipeline is not there yet waits for its fast link or is skipped until a worker has built it, and the
// next draw with a new pipeline is only found when the replay reaches it: an area's first frame, or a turn that brings
// a dozen new materials into view, paid for them one after another (each wait a few milliseconds with the stage
// libraries in the driver's cache, 100 ms and more without), or showed frames with those draws missing while the
// workers that could have built them all at once sat idle.
// The submission is recorded before it is replayed, so what comes after the draw is known. The first draw of a
// submission that finds a shader or pipeline missing has the rest read here (vk_exec_ahead), from the op after it to
// the end of the submission's last command buffer: the scan applies the binds that decide a draw's shaders and its
// shader libraries (program, state objects, vertex attributes, textures and images, render targets, depth clamp) to a
// copy of the replay state and, at each draw, looks its shaders up as the draw will and names the state to
// vk_pipeline_hint, which queues the libraries not built yet. Nothing is recorded and no Vulkan object is touched;
// a shader not in memory is queued for its disk read or translation by the lookup itself. The draws then find their
// libraries built, or building on several workers at once.
// What the scan does not follow: the op stream past a wait for another queue's signal (what follows may be held), a
// change of the texture pool, mesh draws where their pipelines have no stage libraries (vk_pipeline_mesh_libs), and
// whatever would take it past AHEAD_NS. Render targets are read from their descriptors (format, view format, sample
// count), which is what the replay resolves them to unless a target is left out; a guess that misses costs two
// libraries at most.
#define AHEAD_NS 3000000ull
#define AHEAD_OPS 400000u
static struct {
    int on, live;                      // on: -1 until read; live: vk_exec_record walks handles[i] now
    const uint64_t *handles;
    int n, i;
    const op_hdr *at[CALL_DEPTH + 1];  // the op being replayed at each call depth
    int depth;
    uint64_t seq;                      // the submission scanned last
    uint64_t scans, ops, cut, ns, max_ns, logged_scans;
} AH = {.on = -1};

static void ahead_set_rt(VkExec *sx, const p_set_rt *r) {
    sx->rt = *r;
    int n = r->n < 0 ? 0 : r->n > 8 ? 8 : r->n, samples = 0;
    for (int i = 0; i < 8; i++) {
        const tex_t *t = i < n ? r->color[i] : NULL;
        sx->pipe.color_fmt[i] = !t ? 0 : r->has_cview[i] && r->cview[i].set_format ? (int32_t)r->cview[i].format : t->d.format;
        if (t && !samples) samples = t->d.samples > 1 ? t->d.samples : 1;
    }
    sx->pipe.depth_fmt = r->depth ? r->depth->d.format : 0;
    if (r->depth && !samples) samples = r->depth->d.samples > 1 ? r->depth->d.samples : 1;
    sx->pipe.samples = (uint8_t)(samples ? samples : 1);
}

void vk_exec_ahead(VkExec *ex) {
    if (AH.on < 0) {
        AH.on = cfg_bool("NX2NAVI_PIPE_AHEAD");
        rt_log("vk: a draw that finds its pipeline missing has the rest of its submission read ahead for the stage "
               "libraries the later draws need: %s (NX2NAVI_PIPE_AHEAD)", AH.on ? "on" : "off");
    }
    if (!AH.on || !AH.live || evo_in || rw_chunk || AH.seq == vk_exec_submit_seq) return;   // not in a chunk
    AH.seq = vk_exec_submit_seq;
    uint64_t t0 = kern_now_ns();
    uint32_t ops = 0;
    static VkExec sx;   // a copy of the replay state: too large for the stack
    sx = *ex;
    vk_exec_ahead_begin();
    const op_hdr *ret[CALL_DEPTH + 1];
    int depth = AH.depth, i = AH.i;
    for (int d = 0; d < depth; d++) ret[d] = AH.at[d];
    const op_hdr *h = nvn_op_next(AH.at[depth]);
    for (;;) {
        if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_) break;   // the replay reports it when it gets there
        if (++ops >= AHEAD_OPS || (!(ops & 63) && kern_now_ns() - t0 > AHEAD_NS)) {
            AH.cut++;
            break;
        }
        const void *a = nvn_op_payload(h);
        int stop = 0;
        switch (h->op) {
        case OP_END:
            if (depth > 0) h = ret[--depth];   // back in the caller: on from the op after its call, below
            else if (++i < AH.n) {
                h = nvn_op_first(AH.handles[i]);
                continue;
            } else stop = 1;
            break;
        case OP_JUMP: h = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target; continue;
        case OP_CALL:
            if (depth >= CALL_DEPTH) {
                stop = 1;
                break;
            }
            ret[depth++] = h;
            h = nvn_op_first(((const p_jump *)a)->target);
            continue;
        case OP_WAIT_EVENT: stop = 1; break;
        case OP_SET_TEX_POOL: stop = ((const p_set_pool *)a)->pool != sx.tex_pool; break;
        case OP_SET_RT:
            if (memcmp(&sx.rt, a, sizeof sx.rt)) ahead_set_rt(&sx, a);
            break;
        case OP_DEPTH_CLAMP: sx.pipe.depth_clamp = ((const p_flag *)a)->value != 0; break;
        case OP_RASTER_DISCARD: sx.pipe.raster_discard = ((const p_flag *)a)->value != 0; break;
        case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_TEX: case OP_BIND_IMG:
        case OP_DRAW: case OP_DRAW_IDX: case OP_DRAW_IDX_IND: case OP_DRAW_MESH: case OP_DRAW_MESH_IND:
            vk_exec_ahead_op(&sx, h->op, a);
            break;
        default: break;
        }
        if (stop) break;
        h = nvn_op_next(h);
    }
    uint64_t ns = kern_now_ns() - t0;
    AH.scans++, AH.ops += ops, AH.ns += ns;
    if (ns > AH.max_ns) AH.max_ns = ns;
    if ((rt_trace & RT_TRACE_PERF) && AH.scans - AH.logged_scans >= 100) {
        rt_log("perf: looking ahead: %llu scans so far, %.0f ops and %.2f ms each (longest %.2f ms, %llu ended at their "
               "limit); %llu draws seen, %llu looked up", (unsigned long long)AH.scans, (double)AH.ops / (double)AH.scans,
               (double)AH.ns / 1e6 / (double)AH.scans, (double)AH.max_ns / 1e6, (unsigned long long)AH.cut,
               (unsigned long long)vk_exec_ahead_draws, (unsigned long long)vk_exec_ahead_lookups);
        AH.logged_scans = AH.scans;
    }
}

// Ops that only store state or draw: nothing they record runs outside a pass, and the walk goes on with the op after
// them. Counter reports and resets are among them: they close and open an occlusion query inside the pass, and the
// value reaches guest memory from the completion thread once the submission's fence has passed (vk_exec_xfer.c
// oq_cb, counter_cb), never from the GPU inside the run; so are the render conditions, which the replay decides on
// the CPU. Ocarina reports a counter after most of its indirect mesh draws. Any other op the walk replays bumps
// vk_exec_brk, and so does a held submission that goes on in between (evo_resume): what was prepared for the draws
// of a run of such ops (vk_exec_draw.c, the indirect mesh draws' records) holds only while it stays the same.
uint32_t vk_exec_brk;

static int op_linear(uint32_t op) {
    switch (op) {
    case OP_BEGIN: case OP_JUMP: case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_STENCIL:
    case OP_POLY_OFFSET: case OP_BLEND_COLOR: case OP_DEPTH_CLAMP: case OP_RASTER_DISCARD: case OP_PRIM_RESTART:
    case OP_COPY_STRIDE: case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: case OP_DRAW: case OP_DRAW_IDX: case OP_DRAW_IDX_IND:
    case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_UBO:
    case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_PUSH_CC: case OP_DRAW_MESH:
    case OP_DRAW_MESH_IND: case OP_REPORT_COUNTER: case OP_RESET_COUNTER: case OP_RENDER_ENABLE: case OP_RENDER_COND:
        return 1;
    default: return 0;
    }
}

#define MESH_RUN_OPS 2048
static uint32_t mesh_run_stop[OP_COUNT_];   // the op each run of vk_exec_mesh_run ended at, since the start

// For the perf log: the ops the runs ended at most often.
void vk_exec_mesh_run_stops(char *out, size_t n) {
    uint32_t c[OP_COUNT_];
    memcpy(c, mesh_run_stop, sizeof c);
    size_t at = 0;
    *out = 0;
    for (int k = 0; k < 4; k++) {
        int best = 0;
        for (int i = 1; i < OP_COUNT_; i++)
            if (c[i] > c[best]) best = i;
        if (!c[best]) break;
        int w = snprintf(out + at, n - at, "%s%u at %s", k ? ", " : "", c[best], nvn_op_name((uint32_t)best));
        if (w < 0 || (size_t)w >= n - at) break;
        at += (size_t)w, c[best] = 0;
    }
}

// The indirect mesh draws that follow the op being replayed (cur: its payload) with only ops of op_linear between
// them, MESH_RUN_OPS at most: their payloads into out, at most max. 0 when the walk is not at cur.
uint32_t vk_exec_brk_now(void) { return rw_chunk ? rw_chunk->brk : vk_exec_brk; }

int vk_exec_mesh_run(const void *cur, const void **out, int max) {
    const op_hdr *h = rw_chunk ? rw_op : AH.at[AH.depth];   // a chunk reads on from its own op (into the next ones)
    if (!h || nvn_op_payload(h) != cur) return 0;
    int n = 0;
    h = nvn_op_next(h);
    for (int ops = 0; ops < MESH_RUN_OPS && n < max; ops++) {
        if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_) break;
        if (!op_linear(h->op)) {
            mesh_run_stop[h->op]++;
            break;
        }
        if (h->op == OP_JUMP) {
            h = (const op_hdr *)(uintptr_t)((const p_jump *)nvn_op_payload(h))->target;
            continue;
        }
        if (h->op == OP_DRAW_MESH_IND) out[n++] = nvn_op_payload(h);
        h = nvn_op_next(h);
    }
    return n;
}

// One op of the stream other than those that steer the walk (END, JUMP, CALL and WAIT_EVENT: walk).
static void op_one(VkExec *ex, const op_hdr *h) {
    const void *a = nvn_op_payload(h);
    switch (h->op) {
    case OP_BEGIN: break;
    case OP_SET_RT: set_rt(ex, a); break;
    case OP_CLEAR_COLOR: clear_color(ex, a); break;
    case OP_CLEAR_DS: clear_ds(ex, a); break;
    case OP_VIEWPORTS: {
        const p_viewports *v = a;
        for (int i = 0; i < v->n && v->first + i < 16; i++)
            if (v->first + i >= 0) memcpy(ex->vp[v->first + i], v->v[i], sizeof ex->vp[0]);
        ex->has_vp = 1;
        ex->dirty |= VK_EXEC_DIRTY_VIEWPORT;
        break;
    }
    case OP_SCISSOR:
        ex->scissor = *(const p_scissor *)a;
        ex->has_scissor = 1;
        ex->dirty |= VK_EXEC_DIRTY_SCISSOR;
        break;
    case OP_DEPTH_RANGE: {
        const p_depth_range *r = a;
        ex->depth_range[0] = r->n, ex->depth_range[1] = r->f;
        ex->dirty |= VK_EXEC_DIRTY_VIEWPORT;
        break;
    }
    case OP_STENCIL: {
        const p_stencil *st = a;
        for (int f = 0; f < 2; f++) {
            if (!(st->face & (1 << f))) continue;
            if (st->which == 0) ex->st_vmask[f] = (uint32_t)st->value_mask;
            else if (st->which == 1) ex->st_wmask[f] = (uint32_t)st->mask;
            else ex->st_ref[f] = (uint32_t)st->ref;
        }
        ex->dirty |= VK_EXEC_DIRTY_STENCIL;
        break;
    }
    case OP_POLY_OFFSET:
        ex->poly_offset = *(const p_poly_offset *)a;
        ex->dirty |= VK_EXEC_DIRTY_DEPTH_BIAS;
        break;
    case OP_BLEND_COLOR:
        memcpy(ex->blend_color, ((const p_blend_color *)a)->rgba, sizeof ex->blend_color);
        ex->dirty |= VK_EXEC_DIRTY_BLEND_CONST;
        break;
    case OP_DEPTH_CLAMP: ex->pipe.depth_clamp = ((const p_flag *)a)->value != 0; break;
    case OP_RASTER_DISCARD: ex->pipe.raster_discard = ((const p_flag *)a)->value != 0; break;
    case OP_PRIM_RESTART: ex->prim_restart = *(const p_prim_restart *)a; break;
    case OP_COPY_STRIDE: ex->copy_stride = ((const p_copy_stride *)a)->stride; break;
    case OP_SET_TEX_POOL: ex->tex_pool = ((const p_set_pool *)a)->pool, ex->tex_serial++; break;
    case OP_SET_SAMP_POOL: ex->samp_pool = ((const p_set_pool *)a)->pool; break;
    case OP_BARRIER: barrier_to_why(ex, NULL, NULL, 0, BR_GAME); break;
    case OP_DRAW: case OP_DRAW_IDX: case OP_DISPATCH: case OP_UPDATE_UB: case OP_DRAW_IDX_IND: case OP_DISPATCH_IND:
        if (h->op == OP_DRAW || h->op == OP_DRAW_IDX || h->op == OP_DRAW_IDX_IND) vk_exec_frame.draws_seen++, split_draws++;
        if (h->op == OP_DISPATCH || h->op == OP_DISPATCH_IND) vk_exec_frame.dispatches++, split_draws++;
        // fall through
    case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
    case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_PUSH_CC:
    case OP_DRAW_MESH: case OP_DRAW_MESH_IND:
        vk_exec_op_draw(ex, h->op, a);
        break;
    case OP_COPY_B2B: case OP_COPY_B2T: case OP_COPY_T2B: case OP_COPY_T2T: case OP_CLEAR_TEX:
        if (h->op != OP_CLEAR_TEX) vk_exec_frame.copies++;
        if (vk_dump_armed) vk_dump_copy(ex, h->op, a);
        // fall through
    case OP_SIGNAL_EVENT: case OP_REPORT_COUNTER: case OP_RESET_COUNTER: case OP_RENDER_ENABLE: case OP_RENDER_COND:
    case OP_CLEAR_BUF:
        vk_exec_op_xfer(ex, h->op, a);
        break;
    default: rt_fatal("vk: op %s has no replay handler", nvn_op_name(h->op));
    }
}

// ---- replay workers (NX2NAVI_REPLAY_WORKERS, default auto: as many as the CPU's cores make worth it,
// vk_exec_replay_workers_auto; 0: the replay records every op on the submitting thread; the Game tab's CPU helper
// threads row sets it)
// 3 workers: on an RX 7900 XT with a Ryzen 7 5800X3D, Hyrule Field standing went from 41.2 to 50.4 fps (2026-11-12, one
// batch), with the same pictures there and in Kokiri Forest, Kakariko, Goron City, a graveyard house and Gerudo
// Valley; 5 or 7 workers measured the same as 3. Default 0 again after players' games crashed with 3 on by default
// (0.9.1, 0.9.2): an RX 6700 XT lost its device after about 18,000 frames, its last mark a compute dispatch of the
// replay's own, with an occlusion query slot found unavailable after its fence just before; the cause is not known.
// A submission's draws come in runs between ops that have to stay in the replay's order on its own thread (dispatches,
// copies, texture and buffer clears, events, the game's barriers, calls). Hyrule Field, standing (NX2NAVI_REPLAY_RUNS,
// 2026-11-11): about 3,500 draws a frame in 82 runs, 96% of them in runs of 16 draws or more and 67% in runs of 256 or
// more, with a timestamp report beside nearly every draw and a render condition beside every second one.
// A run is cut into groups: a group ends where the render targets change after its first draw or clear, and before an
// inline uniform update (and a mesh draw with the mesh fallback or while NX2NAVI_GPU_PASSES copies their counts), which
// the submitting thread replays itself in the order of the stream like everything outside groups. A group of
// 2 * CHUNK_DRAWS draws or more is recorded by several threads at once:
// - the front pass, on the submitting thread, walks the group in the order of the stream before anything of it is
//   recorded: it applies the state and binds, settles the CPU side of counter reports and decides the render conditions
//   (vk_exec_xfer_front), and copies the replay state at the first op of each chunk;
// - the chunks are taken in order by the workers and the submitting thread, each recording into a command buffer of its
//   own pool with a draw context of its own, from its copy of the state, behind the full barrier that ended the
//   replay's command buffer before the group, and ending behind a full barrier of its own. vk_sync_append puts them
//   after the replay's command buffer in the same part, in the order of the stream, and the replay goes on in a new one.
// The frame's pass hooks (vk_contact_pass_begin, vk_dlaa_pass) follow the frame pass by pass: they run in chunk 0,
// which reaches the group's pass first, and the others start after that pass began; groups follow one another.
// What still depends on the order between chunks waits for its turn (vk_exec_turn: every earlier chunk of the group
// done): a texture lookup that would make or take back an entry (vk_texture.c entry_of) returns nothing and the chunk
// replays the op again once in turn, and so do the contact shadows' pass and the HDR pass's source. Per-object motion
// lists its draws under a lock (vk_exec_draw.c mvobj_draw), and the occlusion queries' slots and segments follow the
// stream (vk_exec_xfer.c vk_exec_xfer_slots, vk_exec_xfer_group_merge).
// NX2NAVI_REPLAY_PIPE=N (1-4, default 0): the submitting thread hands the group to the workers, takes chunks of it as
// they do until none is left to take, and goes on with the ops after it, in a new command buffer after the chunks'
// places in the part (vk_sync_append_later), while the last chunks are recorded; also into the front pass of the next
// group: up to N (at most LIVE_GROUPS) groups are handed out and not in their part yet, their chunks taken from one
// queue in the order of the stream (Q). Hyrule Field, 3 workers (2026-11-12): the frame's groups took 6.8 ms of the
// submitting thread and the ops between them 4.6. A group's chunk 0 runs its pass hooks after the group before began
// its pass, and waits for every earlier group where a hook acts; a turn (vk_exec_turn) waits for every earlier chunk,
// of earlier groups too.
// The submitting thread waits for the groups (group_wait), and puts their command buffers and query slots in place
// (group_drain, oldest first: groups_put_done), only where the order needs it:
// - for room when N groups are out, before a part is split off or submitted (a part due before a group goes to the
//   GPU first: its work would otherwise wait for the group's last chunk too), and before an op that chunks never
//   record (pipe_op: copies, clears of textures and buffers);
// - where a pass's hook acts: DLAA's run (vk_dlaa.c: the jitter and its end, and the per-object motion lists it takes)
//   and the contact shadows' targets (vk_contact.c) change what earlier draws read (vk_exec_hook_turn);
// - when a lookup would make a texture entry or take back a retired one: the op returns early and goes again once the
//   groups are done (op_again), as a chunk's does in turn (vk_texture.c lookup_quiet: what changes nothing, or takes
//   back a released render target that kept its image, needs no turn);
// - uniform snapshot drops count from where they are in the stream (vk_exec_epoch: a group's chunks do not see the
//   drops of ops after the group), the texture pool set syncs under the lock the chunks take, a chunk reads only the
//   inline uniform updates before its group (vk_exec_upd_top), the HDR present's source goes by stream position
//   (vk_exec_stream_pos), and the queries after a group's last report go into the next segment made after its front
//   pass (vk_exec_xfer.c GS).
// Locks a chunk takes, the submitting thread takes too while it is behind (vk_exec_shared). The chunks it records,
// at the hand-out or while it waits between two ops, use a draw context of its own (RW.help_dc).
// Off by default: Hyrule Field, 3 workers, standing (2026-11-12), it shortens the replay (10.9-11.5 against 11.7-12.1
// ms a frame) but not the frame: with 1, 2 or 4 groups out the fps stayed within one of the 50 the stage 2 mode makes,
// over three batches. A frame there is the replay (about 11.5 ms) and then the GPU's work after it (about 6.6 ms: the
// temporal upscaler about 4, the write-back copies 1.5), which the next frame waits for (nvn_api_core.c
// nvnQueueSubmitCommands, NX2NAVI_SUBMIT_FINISH; without that wait the game waits as long itself, 52.4 fps); during the
// replay the GPU keeps up with what is submitted. Sending the part's command buffers up to the first chunk place still
// being recorded while later groups record (tried) did so for at most 0.2 parts per submission and gained nothing.
#define RW_MAX 16
#define CHUNK_DRAWS 32                  // draws a chunk takes at least
#define GROUP_PEEK 65536u               // ops read ahead for a group, at most

// The ops a run may hold; JUMP is followed.
static int run_op(uint32_t op) {
    switch (op) {
    case OP_BEGIN: case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_STENCIL: case OP_POLY_OFFSET:
    case OP_BLEND_COLOR: case OP_DEPTH_CLAMP: case OP_RASTER_DISCARD: case OP_PRIM_RESTART: case OP_COPY_STRIDE:
    case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR:
    case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP:
    case OP_BIND_IMG: case OP_PUSH_CC: case OP_DRAW: case OP_DRAW_IDX: case OP_DRAW_IDX_IND: case OP_DRAW_MESH:
    case OP_DRAW_MESH_IND: case OP_REPORT_COUNTER: case OP_RESET_COUNTER: case OP_RENDER_ENABLE: case OP_RENDER_COND:
    case OP_UPDATE_UB: case OP_SET_RT: case OP_CLEAR_COLOR: case OP_CLEAR_DS:
        return 1;
    default: return 0;
    }
}

typedef struct {
    PlatThread *t;
    int idx;                            // also its command pool (vk_sync_worker_begin); the submitting thread's is RW.n
    void *dc;                           // its draw context (vk_exec_draw_ctx_new)
    volatile int32_t sleeping;          // waits for wake rather than spinning
    PlatEvent wake;
} RwWorker;

static const char *const pw_name[PW_N] = {"next group", "op", "pass hooks", "turn", "texture entry", "split",
                                          "submission end", "done before"};

static struct {
    int n;                              // workers; 0 off, -1 not read yet
    int pipe;                           // NX2NAVI_REPLAY_PIPE: the groups out at once at most, 0 none
    int small;                          // NX2NAVI_REPLAY_SMALL: the fewest draws of a group one worker takes whole
    uint64_t small_groups, small_draws; // such groups and their draws since the last perf line
    void *help_dc;                      // the submitting thread's draw context for chunks it takes while it waits
    RwWorker *w[RW_MAX];
    const op_hdr *skip_to[CALL_DEPTH + 1];   // per call depth: the walk read ahead to this op, a group too small
    uint64_t groups, group_draws, chunks, serial_draws, wait_ns;   // since the last perf line
    uint64_t end_draws[5];              // draws of groups by what ended them (group_peek's GE_*), since the last perf line
    uint64_t small_end[5];              // ... of those too small to split
    uint64_t run_end[OP_COUNT_];        // draws of groups that an op outside groups ended, by that op
    // the submitting thread, since the last perf line: ops outside groups by kind (SER_*), and in groups the front
    // pass, its own chunks and the wait for the others'
    uint64_t ser_n[4], ser_ns[4], front_ns, own_ns, spin_ns;
    // NX2NAVI_REPLAY_PIPE, since the last perf line: waits by why (PW_*), the time from a group's hand-out to the end
    // of the wait for it, and the ops that waited (PW_OP)
    uint64_t pw_n[PW_N], pw_ns[PW_N], behind_ns, pd_op[OP_COUNT_];
    volatile uint64_t turns, turn_ns, redos, chunk_ns;   // chunk_ns: the chunks' recording, every thread's
} RW = {.n = -1};

static void group_take(int pool);

enum { SER_DRAW, SER_MESH, SER_DISPATCH, SER_OTHER };
static int ser_kind(uint32_t op) {
    switch (op) {
    case OP_DRAW: case OP_DRAW_IDX: case OP_DRAW_IDX_IND: return SER_DRAW;
    case OP_DRAW_MESH: case OP_DRAW_MESH_IND: return SER_MESH;
    case OP_DISPATCH: case OP_DISPATCH_IND: return SER_DISPATCH;
    default: return SER_OTHER;
    }
}

static Chunk *chunk_take(void);
static void chunk_record(Chunk *c, int pool);

// ---- side work (vk_exec_par): the replay thread hands the workers a loop over n items while they have no chunk to
// record, at a submission's start (vk_usnap.c copy_regions). One job at a time, in a static slot: seq is odd while it
// runs. A worker counts itself in (PJ.in) and checks seq again before it reads the job, and the caller, once every
// slice is done, makes seq even and waits until no worker is in, so none reads a job that has ended.
static struct {
    void (*fn)(void *, int, int);
    void *arg;
    int n, grain;
    volatile int32_t next, left, in;
    volatile uint32_t seq;
} PJ;
static PlatMutex pj_lock = PLAT_MUTEX_INIT;

static void par_slices(void) {
    for (;;) {
        int lo = __atomic_fetch_add(&PJ.next, PJ.grain, __ATOMIC_ACQ_REL);
        if (lo >= PJ.n) return;
        int hi = lo + PJ.grain < PJ.n ? lo + PJ.grain : PJ.n;
        PJ.fn(PJ.arg, lo, hi);
        __atomic_sub_fetch(&PJ.left, hi - lo, __ATOMIC_ACQ_REL);
    }
}

static int par_pending(void) { return __atomic_load_n(&PJ.seq, __ATOMIC_ACQUIRE) & 1; }

static void par_help(void) {
    uint32_t s = __atomic_load_n(&PJ.seq, __ATOMIC_ACQUIRE);
    if (!(s & 1)) return;
    __atomic_add_fetch(&PJ.in, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&PJ.seq, __ATOMIC_SEQ_CST) == s) par_slices();
    __atomic_sub_fetch(&PJ.in, 1, __ATOMIC_SEQ_CST);
}

void vk_exec_par(void (*fn)(void *, int, int), void *arg, int n, int grain) {
    if (grain < 1) grain = 1;
    if (RW.n <= 0 || n <= grain || Q.drained != Q.out) {
        if (n > 0) fn(arg, 0, n);
        return;
    }
    plat_mutex_lock(&pj_lock);
    PJ.fn = fn, PJ.arg = arg, PJ.n = n, PJ.grain = grain;
    __atomic_store_n(&PJ.next, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&PJ.left, n, __ATOMIC_RELAXED);
    __atomic_add_fetch(&PJ.seq, 1, __ATOMIC_SEQ_CST);   // odd: running
    for (int k = 0; k < RW.n; k++)
        if (__atomic_load_n(&RW.w[k]->sleeping, __ATOMIC_SEQ_CST)) plat_event_set(&RW.w[k]->wake);
    par_slices();
    while (__atomic_load_n(&PJ.left, __ATOMIC_ACQUIRE) > 0) plat_pause();
    __atomic_add_fetch(&PJ.seq, 1, __ATOMIC_SEQ_CST);   // even: ended
    while (__atomic_load_n(&PJ.in, __ATOMIC_SEQ_CST)) plat_pause();
    plat_mutex_unlock(&pj_lock);
}

static void rw_main(void *arg) {
    RwWorker *w = arg;
    plat_thread_priority(PLAT_PRIO_HIGHEST);   // as the recording thread (rec_claim)
    perf_lock_timing_here();   // NX2NAVI_LOCK_TIME, as on the recording thread
    vk_exec_draw_ctx_use(w->dc);
    for (;;) {
        par_help();
        Chunk *c = chunk_take();
        if (c) {
            chunk_record(c, w->idx);
            continue;
        }
        // spins about 100 us after a chunk (the next group of the frame is usually closer), then sleeps
        uint64_t t0 = kern_now_ns();
        for (unsigned i = 0; __atomic_load_n(&Q.take, __ATOMIC_SEQ_CST) >= __atomic_load_n(&Q.put, __ATOMIC_SEQ_CST);
             i++) {
            if (par_pending()) break;
            if ((i & 63) || kern_now_ns() - t0 < 100000) {
                plat_pause();
                continue;
            }
            __atomic_store_n(&w->sleeping, 1, __ATOMIC_SEQ_CST);
            if (__atomic_load_n(&Q.take, __ATOMIC_SEQ_CST) >= __atomic_load_n(&Q.put, __ATOMIC_SEQ_CST) &&
                !par_pending())
                plat_event_wait_ns(&w->wake, 2000000);
            __atomic_store_n(&w->sleeping, 0, __ATOMIC_RELAXED);
            t0 = kern_now_ns();
        }
    }
}

// NX2NAVI_REPLAY_WORKERS = auto: the workers this PC gets. The game's own threads, the submitting thread, the present
// thread and the pipeline builders already keep about half of the cores busy, and on the 8-core CPU above 3 workers
// gave all there was (5 and 7 measured the same), so the count is half the physical cores less one: 2 on 6 cores, 3 on
// 8, 5 on 12, at most 6. Four cores or fewer get none: a worker would take a core from the game's threads. AMD's
// Windows driver for RX 6000 and older (vk_caps.amd_sc) gets none either, as that is where a player's device was lost
// with workers on and the cause is not known; a number set in nx2navi.ini or the menu still applies there, with a
// warning in the log (rw_init). Physical cores of the process's affinity (plat_cpu_cores), or half the usable logical
// processors where that is unknown.
#define RW_AUTO_MAX 6
int vk_exec_replay_workers_auto(const char **why) {
    uint64_t all, first;
    int cores = plat_cpu_cores(&all, &first);
    if (cores <= 0) cores = (int)plat_cpu_usable() / 2;
    int n = cores <= 4 ? 0 : cores / 2 - 1;
    if (n > RW_AUTO_MAX) n = RW_AUTO_MAX;
    const char *w = cores <= 4 ? "four cores or fewer" : "half the cores less one";
    if (vk_caps.amd_sc) n = 0, w = "AMD's driver for RX 6000 and older, where a game with workers lost its device";
    if (why) *why = w;
    return n;
}

static void rw_init(void) {
    const char *set = cfg_value("NX2NAVI_REPLAY_WORKERS"), *why = NULL;
    int autoset = !strcmp(set, "auto");
    int n = autoset ? vk_exec_replay_workers_auto(&why) : atoi(set);
    RW.n = n < 0 ? 0 : n > RW_MAX ? RW_MAX : n;
    if (autoset) {
        uint64_t all, first;
        rt_log("vk: replay workers automatic: %d for %d physical cores (%s; NX2NAVI_REPLAY_WORKERS = a number sets "
               "them)", RW.n, plat_cpu_cores(&all, &first), why);
    } else if (RW.n && vk_caps.amd_sc) {   // the player's number stays (vk_exec_replay_workers_auto)
        rt_log("vk: WARNING: %d replay workers set on AMD's driver for RX 6000 and older, where a game with workers lost "
               "its device; NX2NAVI_REPLAY_WORKERS = auto or 0 avoids it", RW.n);
    }
    for (int k = 0; k < RW.n; k++) {
        RwWorker *w = calloc(1, sizeof *w);
        if (!w) rt_fatal("vk: out of memory (replay worker)");
        w->idx = k, w->dc = vk_exec_draw_ctx_new();
        plat_event_init(&w->wake, 0, 0);
        char name[32];
        snprintf(name, sizeof name, "vk replay worker %d", k);
        if (!(w->t = plat_thread_new(rw_main, w, name))) rt_fatal("vk: cannot start a replay worker");
        RW.w[k] = w;
    }
    int pipe = cfg_int("NX2NAVI_REPLAY_PIPE");   // the groups out at once
    RW.pipe = RW.n <= 0 || pipe <= 0 ? 0 : pipe > LIVE_GROUPS ? LIVE_GROUPS : pipe;
    if (RW.pipe) RW.help_dc = vk_exec_draw_ctx_new();
    RW.small = RW.pipe ? cfg_int("NX2NAVI_REPLAY_SMALL") : 0;
    if (RW.small >= 2 * CHUNK_DRAWS) {   // a group that large is cut into chunks (group_start): no group would match
        rt_log("vk: NX2NAVI_REPLAY_SMALL = %d: groups of %d draws or more are shared out anyway, so %d is used (0..%d)",
               RW.small, 2 * CHUNK_DRAWS, 2 * CHUNK_DRAWS - 1, 2 * CHUNK_DRAWS - 1);
        RW.small = 2 * CHUNK_DRAWS - 1;
    }
    if (RW.small > 0)
        rt_log("vk: a group of %d draws or more goes whole to one replay worker (NX2NAVI_REPLAY_SMALL)", RW.small);
    else RW.small = 0;
    if (RW.pipe)
        rt_log("vk: groups of %d draws or more are recorded by %d replay worker%s while the submitting thread goes on "
               "past them, %d at once at most (NX2NAVI_REPLAY_WORKERS, NX2NAVI_REPLAY_PIPE)", 2 * CHUNK_DRAWS, RW.n,
               RW.n > 1 ? "s" : "", RW.pipe);
    else if (RW.n)
        rt_log("vk: groups of %d draws or more are recorded by %d replay worker%s and the submitting thread "
               "(NX2NAVI_REPLAY_WORKERS)", 2 * CHUNK_DRAWS, RW.n, RW.n > 1 ? "s" : "");
}

// Every group before g has all its chunks done.
static int groups_before_done(const Group *g) { return __atomic_load_n(&Q.done, __ATOMIC_ACQUIRE) + 1 >= g->seq; }

// Every chunk before c in the stream is done: those of earlier groups, and its group's earlier ones.
static int chunk_in_turn(const Chunk *c) {
    if (!groups_before_done(c->g)) return 0;
    for (int i = 0; i < c->idx; i++)
        if (!__atomic_load_n(&c->g->c[i].done, __ATOMIC_ACQUIRE)) return 0;
    return 1;
}

int vk_exec_turn_due(void) {
    if (!rw_chunk) {
        if (rw_behind && __atomic_load_n(&Q.done, __ATOMIC_ACQUIRE) >= Q.out) rw_behind = 0;
        return rw_behind;
    }
    if (!rw_ordered && chunk_in_turn(rw_chunk)) rw_ordered = 1;
    return !rw_ordered;
}
int vk_exec_in_chunk(void) { return rw_chunk != NULL; }
int vk_exec_shared(void) { return rw_chunk || rw_behind; }
int vk_exec_behind(void) { return !rw_chunk && rw_behind; }

// A chunk of a group comes after the ops before the group and after its group's earlier chunks; the submitting
// thread after every chunk of the groups it handed out.
uint64_t vk_exec_stream_pos(void) { return rw_chunk ? rw_chunk->g->seq << 5 | (uint64_t)rw_chunk->idx : Q.out << 5 | 31; }

// Uniform snapshot drops (vk_usnap.c): a chunk's are its group's; the submitting thread's come after every group it
// handed out. A chunk sees the drops of epochs up to its group's, the submitting thread every one.
uint64_t vk_exec_epoch(int write) { return rw_chunk ? rw_chunk->g->seq : write ? Q.out + 1 : UINT64_MAX; }

// The inline uniform updates of the submission a draw may read (vk_exec_draw.c upd_read): a chunk only those before
// its group.
uint32_t vk_exec_upd_top(uint32_t all) { return rw_chunk ? rw_chunk->g->upd : all; }

void vk_exec_hooks_wait(void) {
    if (!vk_exec_behind()) return;
    const Group *g = &GQ[Q.out % LIVE_GROUPS];   // the last handed out, whose first chunk ran after the others'
    while (!__atomic_load_n(&g->hooks, __ATOMIC_ACQUIRE)) plat_pause();
}

void vk_exec_hook_turn(void) {
    if (!rw_chunk) {
        group_wait(PW_HOOKS, 0);
        return;
    }
    // a group's hooks run in its first chunk, after its group's earlier chunks; the earlier groups' draws read what
    // the hook changes
    while (!groups_before_done(rw_chunk->g)) plat_pause();
}

void vk_exec_turn(void) {
    Chunk *c = rw_chunk;
    if (!c) {   // the submitting thread: after every chunk of the groups it went on past
        group_wait(PW_TURN, 0);
        return;
    }
    if (rw_ordered) return;
    uint64_t t0 = kern_now_ns();
    while (!chunk_in_turn(c)) plat_pause();
    rw_ordered = 1;
    __atomic_add_fetch(&RW.turns, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&RW.turn_ns, kern_now_ns() - t0, __ATOMIC_RELAXED);
}

// The image views a command buffer made for its targets go when its submission is done.
static void views_release(VkExec *ex, uint64_t fence) {
    if (!ex->nviews) return;
    ViewList *l = malloc(sizeof *l + sizeof(VkImageView) * (size_t)ex->nviews);
    if (!l) rt_fatal("vk: out of memory");
    l->n = ex->nviews;
    for (int i = 0; i < ex->nviews; i++) l->v[i] = ((ViewEnt *)ex->views)[i].view;
    vk_completion_add(fence, free_views, l);
    ex->nviews = 0;
}

// What ended a group (RW.end_draws): the end of the run, a render target change, an inline uniform update, a mesh
// draw, the read-ahead limit.
enum { GE_RUN, GE_TARGET, GE_UB, GE_MESH, GE_LIMIT, GE_N };
static const char *const ge_name[GE_N] = {"run end", "target change", "uniform update", "mesh draw", "read limit"};

int vk_gpu_passes_capturing(void);   // vk_crumbs.c: NX2NAVI_GPU_PASSES is copying this frame's counts

// The draws of the group that starts at h (it ends at *end, for the reason *why), reading at most GROUP_PEEK ops.
static uint32_t group_peek(const VkExec *ex, const op_hdr *h, const op_hdr **end, int *why, uint32_t *end_op) {
    uint32_t draws = 0;
    *end_op = 0;
    int begun = 0;
    const p_set_rt *rt = &ex->rt;
    *why = GE_LIMIT;
    // mesh draws stay on the submitting thread with the mesh fallback (its buffers are shared), with the mesh emulation
    // (no VK_EXT_mesh_shader: its arena, queue and the part's pre-pass command buffer are the submitting thread's) and
    // while NX2NAVI_GPU_PASSES copies their workgroup counts (one list)
    int mesh_serial = vk_ctx()->mesh_fallback || !vk_ctx()->has_mesh_shader || vk_gpu_passes_capturing();
    for (uint32_t ops = 0; ops < GROUP_PEEK; ops++) {
        if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_) { *why = GE_RUN; break; }   // the walk reports it
        const void *a = nvn_op_payload(h);
        if (h->op == OP_JUMP) {
            h = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target;
            continue;
        }
        if (!run_op(h->op)) { *why = GE_RUN, *end_op = h->op; break; }
        if (h->op == OP_UPDATE_UB) { *why = GE_UB; break; }
        int mesh = h->op == OP_DRAW_MESH || h->op == OP_DRAW_MESH_IND;
        if (mesh && mesh_serial) { *why = GE_MESH; break; }
        if (h->op == OP_SET_RT && memcmp(rt, a, sizeof *rt)) {
            if (begun) { *why = GE_TARGET; break; }   // another pass: the next group
            rt = a;
        }
        if (h->op == OP_DRAW || h->op == OP_DRAW_IDX || h->op == OP_DRAW_IDX_IND || mesh) draws++, begun = 1;
        if (h->op == OP_CLEAR_COLOR || h->op == OP_CLEAR_DS) begun = 1;
        h = nvn_op_next(h);
    }
    *end = h;
    return draws;
}

static Chunk *chunk_open(Group *g, const VkExec *ex, const op_hdr *h, int idx) {
    Chunk *c = &g->c[idx];
    memcpy(&c->ex, ex, sizeof *ex);
    c->h = h, c->nops = 0, c->cond0 = g->ncond, c->ncond = 0, c->idx = idx, c->g = g, c->oq = vk_exec_xfer_chunk_state();
    c->nslots = 0, c->brk = 0;
    return c;
}

// The groups whose every chunk is done, in order (Q.done), once a group's last chunk is.
static void groups_done_advance(void) {
    plat_mutex_lock(&done_lock);
    for (;;) {
        uint64_t d = Q.done;
        const Group *g = &GQ[(d + 1) % LIVE_GROUPS];
        if (__atomic_load_n(&g->seq, __ATOMIC_ACQUIRE) != d + 1 || __atomic_load_n(&g->ndone, __ATOMIC_ACQUIRE) != g->n)
            break;
        __atomic_store_n(&Q.done, d + 1, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&done_lock);
}

static Chunk *chunk_take(void) {
    uint64_t t = __atomic_load_n(&Q.take, __ATOMIC_ACQUIRE);
    while (t < __atomic_load_n(&Q.put, __ATOMIC_ACQUIRE))
        if (__atomic_compare_exchange_n(&Q.take, &t, t + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return Q.q[t % QN];
    return NULL;
}

// Records chunk c with the command pool `pool` (a worker's index; the submitting thread's is RW.n).
static void chunk_record(Chunk *c, int pool) {
    Group *g = c->g;
    if (c->idx)
        while (!__atomic_load_n(&g->hooks, __ATOMIC_ACQUIRE)) plat_pause();
    else if (!groups_before_done(g)) {   // the pass hooks follow the frame pass by pass: after the group before's
        const Group *p = &GQ[(g->seq - 1) % LIVE_GROUPS];   // not done, so not put in its part yet: still its slot
        while (!__atomic_load_n(&p->hooks, __ATOMIC_ACQUIRE)) plat_pause();
    }
    uint64_t t0 = kern_now_ns();
    gfx_tl_note('c', t0, (uint64_t)c->idx, (uint64_t)c->nops);
    VkExec *ex = &c->ex, *was = x_cur;
    // the submitting thread records with its own (the state's), or one of its own past the group (RW.pipe)
    int help = pool == RW.n && RW.pipe;
    if (pool < RW.n) ex->dc = RW.w[pool]->dc;
    else if (help) ex->dc = RW.help_dc, vk_exec_draw_ctx_use(RW.help_dc);
    x_cur = ex, rw_chunk = c, rw_ordered = !c->idx && groups_before_done(g);
    vk_exec_xfer_worker(1, g->cond + c->cond0, c->ncond, c->oq, c->slot0, c->nslots, c->idx, (int)(g - GQ));
    ex->cb = vk_sync_worker_begin(pool);
    // the replay's command buffer before the group ended behind a full barrier, with every image in GENERAL
    ex->recorded = 0, ex->barrier_at = 0, ex->in_pass = 0, ex->nfbl = 0, ex->mark = NULL;
    ex->dirty = VK_EXEC_DIRTY_ALL, ex->rt_resolved = 0, ex->fb_fs = NULL;
    ex->views = NULL, ex->nviews = ex->cap_views = 0;
    vk_exec_draw_new_cb();
    const op_hdr *o = c->h;
    for (uint32_t k = 0; k < c->nops;) {
        if (o->op == OP_JUMP) {
            o = (const op_hdr *)(uintptr_t)((const p_jump *)nvn_op_payload(o))->target;
            continue;
        }
        if (!op_linear(o->op)) c->brk++;
        rw_op = o;
        op_one(ex, o);
        if (vk_texture_deferred_take()) {   // a lookup would have made or taken back a texture entry: in turn, again
            vk_exec_pass_end(ex);
            ex->rt_resolved = 0, ex->dirty = VK_EXEC_DIRTY_ALL;
            vk_exec_draw_new_cb();
            vk_exec_turn();
            __atomic_add_fetch(&RW.redos, 1, __ATOMIC_RELAXED);
            op_one(ex, o);
        }
        k++, o = nvn_op_next(o);
    }
    if (!c->idx) __atomic_store_n(&g->hooks, 1, __ATOMIC_RELEASE);   // a chunk 0 that began no pass
    vk_exec_barrier(ex);   // the next command buffer starts after it, every image back in GENERAL
    vk_sync_worker_end(ex->cb);
    views_release(ex, ex->fence);
    free(ex->views);
    ex->views = NULL;
    c->cb = ex->cb, c->recorded = ex->recorded;
    vk_exec_xfer_worker(0, NULL, 0, 0, 0, 0, 0, 0);
    x_cur = was, rw_chunk = NULL;
    if (help) vk_exec_draw_ctx_use(NULL);
    __atomic_add_fetch(&RW.chunk_ns, kern_now_ns() - t0, __ATOMIC_RELAXED);
    gfx_tl_note('d', 0, (uint64_t)c->idx, 0);
    __atomic_store_n(&c->done, 1, __ATOMIC_RELEASE);
    if (__atomic_add_fetch(&g->ndone, 1, __ATOMIC_ACQ_REL) == g->n) groups_done_advance();
}

static void group_take(int pool) {
    for (Chunk *c; (c = chunk_take());) chunk_record(c, pool);
}

// The submitting thread past groups it handed out (RW.pipe): until every chunk of them is done. With `help` (only
// between two ops) it records the chunks no worker took yet itself. Then nothing of them runs beside it any more.
static void group_wait(int why, int help) {
    if (!rw_behind) return;
    uint64_t t0 = kern_now_ns();
    int done = __atomic_load_n(&Q.done, __ATOMIC_ACQUIRE) >= Q.out;
    if (!done) {
        gfx_tl_note('q', t0, (uint64_t)(uintptr_t)pw_name[why], 0);
        if (help) group_take(RW.n);
        while (__atomic_load_n(&Q.done, __ATOMIC_ACQUIRE) < Q.out) plat_pause();
        gfx_tl_note('u', 0, 0, 0);
    }
    rw_behind = 0;
    uint64_t t1 = kern_now_ns();
    if (done) RW.pw_n[PW_DONE]++;
    else RW.pw_n[why]++, RW.pw_ns[why] += t1 - t0;
}

// A done group's command buffers into their places in the part, its closed query slots into their segments.
static void group_put(VkExec *ex, Group *g) {
    vk_exec_xfer_group_merge((int)(g - GQ), g->n);
    VkCommandBuffer cbs[GROUP_CHUNKS];
    int rec = 0;
    for (int i = 0; i < g->n; i++) cbs[i] = g->c[i].cb, rec += g->c[i].recorded;
    vk_sync_append_fill(g->at, cbs, g->n);
    if (ex->barrier_at == ex->recorded) ex->barrier_at += rec;   // nothing since the chunks' closing barrier
    ex->recorded += rec;
    RW.behind_ns += kern_now_ns() - g->out_ns;
    gfx_tl_note('h', 0, 0, 0);
}

// The done groups not in their parts yet, oldest first; when every one handed out is, the submitting thread is no
// longer behind.
static void groups_put_done(VkExec *ex) {
    uint64_t d = __atomic_load_n(&Q.done, __ATOMIC_ACQUIRE);
    while (Q.drained < d) Q.drained++, group_put(ex, &GQ[Q.drained % LIVE_GROUPS]);
    if (Q.drained == Q.out) rw_behind = 0;
}

static void group_drain(VkExec *ex, int why) {
    if (Q.drained == Q.out) return;
    group_wait(why, 1);
    groups_put_done(ex);
}

// Before the front pass of a new group: its slot holds a group put in its part (the oldest goes in first).
static void group_room(VkExec *ex) {
    if (Q.out - Q.drained < (uint64_t)RW.pipe) return;
    uint64_t want = Q.drained + 1;
    if (__atomic_load_n(&Q.done, __ATOMIC_ACQUIRE) < want) {
        uint64_t t0 = kern_now_ns();
        gfx_tl_note('q', t0, (uint64_t)(uintptr_t)pw_name[PW_NEXT], 0);
        group_take(RW.n);
        while (__atomic_load_n(&Q.done, __ATOMIC_ACQUIRE) < want) plat_pause();
        gfx_tl_note('u', 0, 0, 0);
        RW.pw_n[PW_NEXT]++, RW.pw_ns[PW_NEXT] += kern_now_ns() - t0;
    }
    groups_put_done(ex);
}

// The ops the submitting thread replays past a group it handed out; before any other it waits for the group. The
// ops a chunk records, the game's barriers, dispatches, and events (a wait reads memory, a signal's value goes there
// from the completion thread): their lookups go again once the group is done (op_again), and they leave what the
// chunks read as it was (vk_exec_shared, vk_exec_epoch, vk_exec_upd_top).
static int pipe_op(uint32_t op) {
    return run_op(op) || op == OP_BARRIER || op == OP_DISPATCH || op == OP_DISPATCH_IND || op == OP_WAIT_EVENT ||
           op == OP_SIGNAL_EVENT;
}

// A lookup of op h would have made or taken back a texture entry before the group it went on past was done (the
// submitting thread's vk_texture_deferred): once it is, h goes again, as a chunk's op does in turn.
static void op_again(VkExec *ex, const op_hdr *h) {
    vk_exec_pass_end(ex);
    ex->rt_resolved = 0, ex->dirty = VK_EXEC_DIRTY_ALL;
    vk_exec_draw_new_cb();
    group_wait(PW_TEX, 1);
    __atomic_add_fetch(&RW.redos, 1, __ATOMIC_RELAXED);
    op_one(ex, h);
}

// The submitting thread, at op h of the walk: when a group large enough to split starts here, it is recorded by the
// workers and this thread, and the walk goes on with the op that ended it, which is returned; otherwise h (the walk
// replays the group itself, without reading ahead again before its end).
static const op_hdr *group_start(VkExec *ex, const op_hdr *h, int depth) {
    const op_hdr *end;
    int why;
    uint32_t end_op;
    uint32_t draws = group_peek(ex, h, &end, &why, &end_op);
    if (why == GE_RUN && end_op < OP_COUNT_) RW.run_end[end_op] += draws;
    if (end == h) return h;   // an op the submitting thread replays itself
    RW.end_draws[why] += draws;
    int n = (int)(draws / CHUNK_DRAWS);
    if (n > RW.n + 1) n = RW.n + 1;
    if (n > GROUP_CHUNKS) n = GROUP_CHUNKS;
    // NX2NAVI_REPLAY_SMALL=N (default 0, with NX2NAVI_REPLAY_PIPE): a group of N draws or more that is too small to cut
    // goes whole to one worker, and the submitting thread goes on with what follows it without taking the chunk
    // itself. Kokiri Forest standing, 5 workers, 4 groups out, the early tail on (2026-11-13): about 700 draws a frame
    // were in such groups, 3.6 ms of the submitting thread; with 16 the replay took 10.2 instead of 11.0 ms a frame
    // (63.2 against 62.2 fps: the game then waits longer for the GPU, which is busy 83% of the frame there).
    int small = RW.pipe && RW.small && n < 2 && draws >= (uint32_t)RW.small;
    if (small) n = 1;
    if (n < 1 + !small || vk_dump_armed || TL.on || !vk_sync_append_room(n)) {
        RW.skip_to[depth] = end, RW.serial_draws += draws, RW.small_end[why] += draws;
        return h;
    }
    if (RW.pipe) {
        // a part due goes to the GPU before the group, with the groups still recording in it: its GPU work would
        // otherwise wait for this group's chunks too
        if (split_draws && split_wanted()) group_drain(ex, PW_SPLIT), maybe_split(ex);
        group_room(ex);
    }
    Group *g = &GQ[(Q.out + 1) % LIVE_GROUPS];
    uint64_t t0 = kern_now_ns();
    gfx_tl_note('g', 0, draws, (uint64_t)n);
    vk_exec_barrier(ex);         // the command buffer so far ends behind a full barrier, every image back in GENERAL
    g->upd = vk_exec_draw_group_begin(ex);
    // the front pass
    uint32_t per = (draws + (uint32_t)n - 1) / (uint32_t)n, cd = 0;
    int nc = 1, cut = 0;
    g->ncond = 0;
    vk_exec_xfer_group((int)(g - GQ));
    Chunk *c = chunk_open(g, ex, h, 0);
    for (const op_hdr *o = h; o != end;) {
        const void *a = nvn_op_payload(o);
        if (o->op == OP_JUMP) {
            o = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target;
            continue;
        }
        if (cut) c = chunk_open(g, ex, o, nc++), cd = 0, cut = 0;
        switch (o->op) {
        case OP_DRAW: case OP_DRAW_IDX: case OP_DRAW_IDX_IND: case OP_DRAW_MESH: case OP_DRAW_MESH_IND:
            if (vk_exec_xfer_counting() && !ex->render_disabled) c->nslots++;   // at most one query each
            if (++cd == per && nc < n) cut = 1;
            break;
        case OP_CLEAR_COLOR: case OP_CLEAR_DS: break;   // recorded by the chunk
        case OP_REPORT_COUNTER: case OP_RESET_COUNTER: case OP_RENDER_ENABLE: case OP_RENDER_COND: {
            int on = vk_exec_xfer_front(ex, o->op, a);
            if (o->op == OP_RENDER_COND) {
                if (g->ncond == g->ccond) {
                    g->ccond = g->ccond ? g->ccond * 2 : 1024;
                    if (!(g->cond = realloc(g->cond, g->ccond))) rt_fatal("vk: out of memory (render conditions)");
                }
                g->cond[g->ncond++] = (uint8_t)on;
                c->ncond++;
            }
            break;
        }
        default: op_one(ex, o); break;   // state, binds, render targets
        }
        c->nops++;
        o = nvn_op_next(o);
    }
    vk_exec_xfer_group(-1);   // the segments stay listed for the workers until the group is put in its part
    for (int i = 0; i < nc; i++) g->c[i].slot0 = vk_exec_xfer_slots(g->c[i].nslots);   // in the order of the stream
    // handed out: the workers (and without RW.pipe this thread) take the chunks in order
    for (int i = 0; i < nc; i++) g->c[i].done = 0;
    g->hooks = 0, g->ndone = 0, g->n = nc;
    __atomic_store_n(&g->seq, ++Q.out, __ATOMIC_RELEASE);
    // the replay's command buffer so far goes into the part, then the chunks' places, and the replay goes on in a new
    // one at once
    ex->cb = vk_sync_append_later(nc, &g->at);
    for (int i = 0; i < nc; i++) Q.q[(Q.put + (uint64_t)i) % QN] = &g->c[i];
    uint64_t tf = kern_now_ns();
    g->out_ns = tf;
    __atomic_store_n(&Q.put, Q.put + (uint64_t)nc, __ATOMIC_SEQ_CST);
    for (int k = 0; k < RW.n; k++)
        if (__atomic_load_n(&RW.w[k]->sleeping, __ATOMIC_SEQ_CST)) plat_event_set(&RW.w[k]->wake);
    ex->barrier_at = ex->recorded;   // the chunks end with a full barrier
    ex->in_pass = 0, ex->nfbl = 0, ex->mark = NULL, ex->dirty = VK_EXEC_DIRTY_ALL, ex->rt_resolved = 0, ex->fb_fs = NULL;
    vk_exec_draw_new_cb();
    RW.groups++, RW.group_draws += draws, RW.chunks += (uint64_t)nc;
    if (RW.pipe) {
        // the submitting thread takes chunks too, as the workers wake up, and goes on once none is left to take: the
        // GPU waits for the group's part until its last chunk is done
        rw_behind = 1;
        if (small) RW.small_groups++, RW.small_draws += draws;   // a worker takes it: this thread goes on at once
        else group_take(RW.n);
        uint64_t to = kern_now_ns();
        RW.front_ns += tf - t0, RW.own_ns += to - tf, RW.wait_ns += to - t0;
        return end;
    }
    group_take(RW.n);
    uint64_t to = kern_now_ns();
    while (__atomic_load_n(&Q.done, __ATOMIC_ACQUIRE) < g->seq) plat_pause();
    uint64_t tw = kern_now_ns();
    RW.front_ns += tf - t0, RW.own_ns += to - tf, RW.spin_ns += tw - to;
    groups_put_done(ex);
    vk_exec_draw_new_cb();   // the submitting thread's own chunks used its draw context
    RW.wait_ns += kern_now_ns() - t0;
    return end;
}

static const op_hdr *walk(VkExec *ex, const op_hdr *h, int depth) {
    RW.skip_to[depth] = NULL;
    for (;;) {
        if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_)
            rt_fatal("vk: corrupt command record at %p: op %u size %u", (const void *)h, h->op, h->size);
        if (RW.skip_to[depth] == h) RW.skip_to[depth] = NULL;   // the small group read ahead is behind
        if (Q.drained < Q.out && Q.drained < __atomic_load_n(&Q.done, __ATOMIC_ACQUIRE)) groups_put_done(ex);
        if (RW.n > 0 && !RW.skip_to[depth] && !evo_in && h->op != OP_JUMP && run_op(h->op)) {
            const op_hdr *stop = group_start(ex, h, depth);
            if (stop != h) {
                if (split_draws && split_wanted()) maybe_split(ex);
                h = stop;
                continue;
            }
        }
        const void *a = nvn_op_payload(h);
        AH.at[depth] = h, AH.depth = depth;   // where vk_exec_ahead reads on from
        if (RR.on > 0) runs_op(h->op);
        if (!op_linear(h->op)) vk_exec_brk++;
        switch (h->op) {
        case OP_END: return NULL;
        case OP_JUMP: h = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target; continue;
        case OP_CALL:
            if (depth >= CALL_DEPTH) rt_fatal("vk: command buffer calls nested deeper than %d", CALL_DEPTH);
            walk(ex, nvn_op_first(((const p_jump *)a)->target), depth + 1);
            break;
        case OP_WAIT_EVENT: {
            int met = vk_exec_wait_event(a);
            vk_exec_evt(met ? 'm' : 'u', (const void *)((const p_wait_event *)a)->val, 0);
            if (!met && depth == 0 && evo_queue) {
                vk_exec_pass_end(ex);
                return h;
            }
            break;
        }
        default:
            if (RW.n > 0) {   // the perf line's split of the replay outside groups
                if (Q.drained < Q.out && !pipe_op(h->op)) RW.pd_op[h->op]++, group_drain(ex, PW_OP);
                int k = ser_kind(h->op);
                uint64_t t = kern_now_ns();
                op_one(ex, h);
                if (vk_texture_deferred_take()) op_again(ex, h);
                RW.ser_n[k]++, RW.ser_ns[k] += kern_now_ns() - t;
            } else op_one(ex, h);
            break;
        }
        if (vk_exec_signalled && depth == 0 && !evo_in) evo_resume(ex);   // their ops go through this walk's checks
        if (split_draws && split_wanted()) maybe_split(ex);
        h = nvn_op_next(h);
    }
}

// The replay records on the thread the game submitted from, and the kernel schedules that like any other guest
// thread: a preemption in the middle of a part leaves the submission late for the frame it belongs to (Kokiri
// Forest: 274 of 2,737 frames had a late submit). The recording thread therefore keeps the highest priority so the
// scheduler prefers it over the guest's other cores (NX2NAVI_RECORDER_PRIO=0 leaves the guest's priorities alone) and
// gets a core of its own (plat_cpu_reserve): every other thread of the process, the guest's cores, the audio, the
// shader and pipeline workers, is kept off it, so nothing of this process preempts the recording. That core is then
// lost to the rest, so NX2NAVI_RECORDER_CORE=auto reserves it only with 12 or more logical processors, and not while
// the process runs on the die with the largest L3 alone (perf.c cpu_placement): on a two-die X3D part the game's
// threads already have that die to themselves, and a core taken from them cost frames; 1 always, 0 never. Linux
// reserves none with fewer than four fast cores.
// 0 is the default: on an i9-14900KF (8 performance and 16 efficiency cores, 32 logical processors) with the replay on
// its own thread, the reserved core (logical processors 14 and 15) left the woken replay thread unscheduled for about
// 200 ms roughly once a second while the game stood in Kakariko: its queue held 40 to 50 items, its lock was free, the
// wakes had been sent and it used no CPU time, while the game waited on a fence of the work in that queue (27 frames a
// minute with a 150 ms or longer frame). With no core reserved the same minute had none.
// The recording thread is not the first to call vk_exec_record: the game's main thread submits first and goes on
// submitting a little at a time, while another guest thread does the work (Kokiri Forest: that thread spent 21.9 of
// the 22.5 s the replay took in a minute and a half, the first one 0.5 s). The first thread to have spent
// REC_CLAIM_NS in vk_exec_record is the one that keeps both.
#define REC_CLAIM_NS 500000000ull
static _Thread_local uint64_t rec_spent_ns;   // time this thread has spent in vk_exec_record

static void rec_claim(void) {
    static int done;
    if (done) return;
    done = 1;
    rt_log("vk: thread %u is the recording thread (the first to spend %.1f s in it)", plat_thread_id(),
           REC_CLAIM_NS / 1e9);
    const char *e = cfg_get("NX2NAVI_RECORDER_PRIO");
    if (!(e && !strcmp(e, "0"))) {
        plat_thread_priority(PLAT_PRIO_HIGHEST);
        rt_log("vk: the recording thread keeps the highest scheduling priority (NX2NAVI_RECORDER_PRIO=0 turns it off)");
    }
    const char *rc = cfg_value("NX2NAVI_RECORDER_CORE");
    unsigned ncpu = plat_cpu_usable();
    int cache_die = perf_cpu_on_cache_die();
    if (!strcmp(rc, "auto") && cache_die) rt_log("vk: no core is reserved for the recording thread: the process runs on "
                                                 "the die with the largest L3 cache (NX2NAVI_RECORDER_CORE=1 reserves one)");
    if (!strcmp(rc, "1") || (!strcmp(rc, "auto") && ncpu >= 12 && !cache_die)) {
        char which[96];
        int n = plat_cpu_reserve(which, sizeof which);
        if (n) rt_log("vk: the recording thread has a core of its own: logical processor%s %s (%d of %u; "
                      "NX2NAVI_RECORDER_CORE=0 turns it off)", n > 1 ? "s" : "", which, n, ncpu);
        else rt_log("vk: no core was reserved for the recording thread (too few fast cores, or the system refused it; "
                    "NX2NAVI_RECORDER_CORE=0 stops the attempt)");
    } else {
        rt_log("vk: the recording thread runs on any core (NX2NAVI_RECORDER_CORE=auto or 1 reserves one)");
    }
}

// The tail begins here (the early tail): what was recorded so far waits in the chain, the tail gets a command buffer of
// its own. Not in a chunk or a continuation, nor with groups still out: the cut needs every piece of the part.
void vk_exec_tail_begin(VkExec *ex) {
    if (!tail_enabled() || TL.on || ex != x_cur || rw_chunk || evo_in) return;
    if (Q.drained != Q.out) group_drain(ex, PW_OP);   // the cut needs every piece of the part
    if (!vk_sync_wb_split() || !vk_mesh_shader_native() || vk_dump_armed || vk_gpu_passes_capturing()) return;
    if (!vk_sync_tail_ok()) return;
    if (ex->in_pass) vk_exec_pass_end(ex);
    vk_exec_barrier(ex);   // the command buffer so far ends behind a full barrier, every image back in GENERAL
    VkCommandBuffer cb = vk_sync_cut();
    if (!cb) return;
    ex->cb = cb;
    ex->dirty = VK_EXEC_DIRTY_ALL;
    vk_exec_draw_new_cb();
    n_cuts++;
    TL.on = 1, TL.live0 = vk_mem_live_swap(0), TL.wrote0 = vk_mem_gpu_wrote_count();
    vk_exec_tail_results();
}

int vk_exec_record(VkCommandBuffer cb, const uint64_t *handles, int n, uint64_t fence) {
    if (vk_device_is_lost()) return 0;   // the run is ending (vk_device.c vk_device_lost_check): nothing is recorded
    uint64_t in_ns = kern_now_ns();
    gfx_tl_note('R', 0, (uint64_t)n, fence);
    perf_lock_timing_here();   // NX2NAVI_LOCK_TIME
    if (prof_replay_tid != plat_thread_id()) perf_replay_thread();   // the profiler follows the thread replaying now
    perf_rec_waits = rec_wait_totals;
    VkExec *ex = &X;
    if (!x_ready) state_reset(ex), vk_dump_init(), x_ready = 1;
    if (RW.n < 0) rw_init();
    if (Q.drained != Q.out) rt_fatal("vk: a parallel group outlived its submission");
    const tex_t *keep[9];   // the render targets carry over to this submission (set_rt compares them by pointer)
    memcpy(keep, ex->rt.color, sizeof ex->rt.color);
    keep[8] = ex->rt.depth;
    nvn_pins_sweep(nvn_submit_count(), keep, 9);
    ex->cb = cb;
    ex->fence = fence;
    TL.on = 0;
    ex->recorded = 0;
    ex->in_pass = 0;
    ex->mark = NULL;
    ex->mark_passes = ex->mark_dispatches = 0;
    ex->dirty = VK_EXEC_DIRTY_ALL;
    ex->rt_resolved = 0;   // images may have changed since the last command buffer
    ex->nviews = 0;
    ex->barrier_at = -1;
    ex->nfbl = 0;   // the previous command buffer ended with a full barrier: every image is in GENERAL
    vk_exec_frame.submits++;
    vk_exec_submit_seq++;
    split_draws = part_start = 0;
    split_at = split_n < 0 ? 0 : split_n ? split_n : INT_MAX;   // 0 until maybe_split reads the settings
    part_t0 = kern_now_ns();
    x_cur = ex;
    int evo = evo_enabled() && gfx_submit_queue;
    int slot = evo ? evq_slot(gfx_submit_queue) : -1;
    if (evo && slot < 0) evo = 0;
    vk_exec_evt('|', (const void *)gfx_submit_queue, (uint32_t)n);
    vk_exec_event_sync(1);   // before the mirror sync and the uniform copy: a CPU signal after this point came after them
    int synced = n || (evo_pending && held_sync() && vk_exec_held_ready());   // held_sync
    if (synced) {
        vk_mem_mirror_begin();
        if (evo_pending) {   // the uniform snapshot covers what held submissions bind too: they may go on here
            int tot = n;
            for (int s = 0; s < EVO_QUEUES; s++)
                for (Cont *c = evq[s].head; c; c = c->next) tot += c->n;
            uint64_t *all = malloc(sizeof *all * (size_t)tot);
            if (!all) rt_fatal("vk: out of memory (snapshot handles)");
            if (n) memcpy(all, handles, sizeof *all * (size_t)n);
            int k = n;
            for (int s = 0; s < EVO_QUEUES; s++)
                for (Cont *c = evq[s].head; c; c = c->next) memcpy(all + k, c->h, sizeof *all * (size_t)c->n), k += c->n;
            vk_usnap_begin(all, tot, fence);
            free(all);
        } else vk_usnap_begin(handles, n, fence);
    }
    for (int i = 0; i < n; i++)
        if (!handles[i]) rt_fatal("vk: command handle %d of a submission is null", i);
    if (evo_pending) evo_resume(ex);   // signals the CPU gave since, and holds past CONT_MAX
    if (evo && evq[slot].head) {   // the queue waits: this submission lines up behind it
        if (n) cont_push(slot, cont_new(handles, n, 0, NULL)), evo_held++, vk_exec_evt('p', NULL, 0);
    } else {
        evo_queue = evo ? gfx_submit_queue : 0;
        AH.handles = handles, AH.n = n, AH.live = 1;
        for (int i = 0; i < n; i++) {
            AH.i = i;
            const op_hdr *stop = walk(ex, nvn_op_first(handles[i]), 0);
            if (!stop) continue;
            cont_push(slot, cont_new(handles, n, i, stop));   // the rest of this submission waits for its signal
            evo_held++;
            vk_exec_evt('h', NULL, 0);
            break;
        }
        AH.live = 0;
        evo_queue = 0;
    }
    group_drain(ex, PW_END);
    vk_exec_pass_end(ex);
    if (RR.on > 0) runs_break(RB_SUBMIT);
    x_cur = NULL;
    vk_exec_event_sync(0);
    if (ex->recorded) vk_exec_barrier(ex), vk_exec_pool_marks(ex);   // later submissions see this one's writes
    // NX2NAVI_WB_SPLIT: the write-backs in a part of their own, after one that signals the executed timeline
    // (vk_sync.c exec_tl): the wait after a frame's first submission then ends when the submission's passes are
    // through, not its write-backs. Off by default: vk_sync.c says what else that wait then leaves out.
    int tail = 0;   // the early tail: the write-backs go before the tail, with the executed timeline's signal
    if (TL.on) {
        int live = vk_mem_live_swap(0), res = vk_exec_tail_results();
        vk_mem_live_swap(live | TL.live0);   // the submission's own note, as it would have been
        int wrote = vk_mem_gpu_wrote_count() != TL.wrote0;
        int other = !synced || !ex->recorded || vk_dump_armed || vk_gpu_passes_capturing() || !vk_sync_tail_ok();
        TL.on = 0, TL.n++;
        if (res) TL.why[0]++;
        else if (wrote) TL.why[1]++;
        else if (live) TL.why[2]++;
        else if (other || !vk_exec_pool_split(ex->fence)) TL.why[3]++;
        else tail = 1, TL.early++;
        if (tail) {
            ex->cb = vk_sync_tail_hold();
            ex->dirty = VK_EXEC_DIRTY_ALL;
            vk_exec_draw_new_cb();
        }
    }
    if (!tail && synced && ex->recorded && vk_sync_wb_split() && vk_mem_mirror_wb_pending() &&
        vk_exec_pool_split(ex->fence)) {
        ex->cb = vk_sync_split_exec();
        ex->dirty = VK_EXEC_DIRTY_ALL;
        vk_exec_draw_new_cb();
    }
    if (synced) ex->recorded += vk_mem_mirror_end(ex->cb, fence), vk_usnap_end(fence);   // VRAM mirror bytes the GPU wrote go back to guest memory
    if (tail) {
        ex->cb = vk_sync_tail_release();
        part_start = ex->recorded;   // the last command buffer holds nothing: the tail is in the chain before it
    }
    if ((rt_trace & RT_TRACE_PERF) && TL.n >= 600) {
        uint64_t tails, waits;
        double ms;
        vk_sync_tail_stats(&tails, &waits, &ms);
        rt_log("perf: early tail: %llu tails, %llu with the write-backs before them (refused: %llu handed the CPU "
               "results, %llu wrote pool memory, %llu read guest memory late, %llu other); %llu waits of the game "
               "ended before their tail, which the GPU runs for about %.2f ms",
               (unsigned long long)TL.n, (unsigned long long)TL.early,
               (unsigned long long)TL.why[0], (unsigned long long)TL.why[1], (unsigned long long)TL.why[2],
               (unsigned long long)TL.why[3], (unsigned long long)waits, ms);
        TL.n = TL.early = 0, memset(TL.why, 0, sizeof TL.why);
    }
    views_release(ex, fence);
    ex->cb = VK_NULL_HANDLE;
    if (n) n_subs++;
    if ((rt_trace & RT_TRACE_PERF) && n_subs >= 240) {
        rt_log("perf: replay command buffer parts per submission %.2f (%.2f first parts sent while the GPU was idle, %.2f "
               "splits inside a pass, %.2f cuts for emulated mesh stages); event order: held %llu, resumed %llu (%llu without their signal), switches %llu, "
               "held now %d; waits a CPU signal met only after the submission's copies were taken: %llu",
               (double)(n_subs + n_parts) / (double)n_subs, (double)n_idle_parts / (double)n_subs,
               (double)n_pass_splits / (double)n_subs, (double)n_cuts / (double)n_subs, (unsigned long long)evo_held, (unsigned long long)evo_resumed,
               (unsigned long long)evo_forced, (unsigned long long)evo_switches, evo_pending,
               (unsigned long long)vk_exec_event_late_take());
        uint64_t xw, xe, xl, xm, le, lc, lo;
        vk_sync_exec_stats(&xw, &xe, &xl, &xm);
        vk_exec_late_stats(&le, &lc, &lo);
        if (xw)   // NX2NAVI_WB_SPLIT: what the wait on the executed timeline left behind
            rt_log("perf: waits after a submit on the executed timeline: %llu, %llu of them back before the completion "
                   "thread had run the side effects of the submissions up to theirs (through %.3f ms later on average, "
                   "%.3f at most); written into guest memory in that time: %llu event values, %llu counter reports, "
                   "%llu occlusion counts",
                   (unsigned long long)xw, (unsigned long long)xe, xe ? xl / 1e6 / (double)xe : 0.0, xm / 1e6,
                   (unsigned long long)le, (unsigned long long)lc, (unsigned long long)lo);
        char cbs[1600];
        vk_sync_cb_stats(cbs, sizeof cbs);
        rt_log("perf: completion thread since the last line, by callback (distance from vk_completion_add):%s", cbs);
        if (RW.n > 0) {
            uint64_t turns = __atomic_exchange_n(&RW.turns, 0, __ATOMIC_RELAXED);
            uint64_t turn_ns = __atomic_exchange_n(&RW.turn_ns, 0, __ATOMIC_RELAXED);
            uint64_t redos = __atomic_exchange_n(&RW.redos, 0, __ATOMIC_RELAXED);
            rt_log("perf: replay workers: per submission %.1f groups of %.0f draws in %.1f chunks, %.2f ms each; %.0f draws "
                   "in smaller groups; waits for an earlier chunk %.1f (%.3f ms), ops replayed again in turn %.2f",
                   (double)RW.groups / (double)n_subs, RW.groups ? (double)RW.group_draws / (double)RW.groups : 0.0,
                   RW.groups ? (double)RW.chunks / (double)RW.groups : 0.0,
                   RW.groups ? (double)RW.wait_ns / 1e6 / (double)RW.groups : 0.0, (double)RW.serial_draws / (double)n_subs,
                   (double)turns / (double)n_subs, (double)turn_ns / 1e6 / (double)n_subs, (double)redos / (double)n_subs);
            char e[512];
            int o = 0;
            for (int k = 0; k < GE_N; k++)
                o += snprintf(e + o, sizeof e - (size_t)o, "%s %s %.0f", k ? "," : "", ge_name[k],
                              (double)RW.end_draws[k] / (double)n_subs);
            o += snprintf(e + o, sizeof e - (size_t)o, "; of them in groups too small to split:");
            for (int k = 0; k < GE_N; k++)
                o += snprintf(e + o, sizeof e - (size_t)o, "%s %s %.0f", k ? "," : "", ge_name[k],
                              (double)RW.small_end[k] / (double)n_subs);
            rt_log("perf: replay workers: draws per submission by what ended their group:%s", e);
            o = 0;
            for (int k = 0; k < 6; k++) {   // the ops outside groups that ended the most, by their groups' draws
                int best = 0;
                for (int op = 1; op < OP_COUNT_; op++)
                    if (RW.run_end[op] > RW.run_end[best]) best = op;
                if (!RW.run_end[best]) break;
                o += snprintf(e + o, sizeof e - (size_t)o, "%s %s %.0f", k ? "," : "", nvn_op_name((uint32_t)best),
                              (double)RW.run_end[best] / (double)n_subs);
                RW.run_end[best] = 0;
            }
            memset(RW.run_end, 0, sizeof RW.run_end);
            if (o) rt_log("perf: replay workers: draws per submission of groups a run end ended, by the op:%s", e);
            memset(RW.end_draws, 0, sizeof RW.end_draws), memset(RW.small_end, 0, sizeof RW.small_end);
            double ns = 1e6 * (double)n_subs;
            rt_log("perf: replay workers: the submitting thread per submission: outside groups draws %.0f (%.3f ms), mesh "
                   "draws %.0f (%.3f ms), dispatches %.0f (%.3f ms), other ops %.0f (%.3f ms); in groups the front pass "
                   "%.3f ms, its own chunks %.3f ms, waiting for the others %.3f ms",
                   (double)RW.ser_n[SER_DRAW] / (double)n_subs, (double)RW.ser_ns[SER_DRAW] / ns,
                   (double)RW.ser_n[SER_MESH] / (double)n_subs, (double)RW.ser_ns[SER_MESH] / ns,
                   (double)RW.ser_n[SER_DISPATCH] / (double)n_subs, (double)RW.ser_ns[SER_DISPATCH] / ns,
                   (double)RW.ser_n[SER_OTHER] / (double)n_subs, (double)RW.ser_ns[SER_OTHER] / ns,
                   (double)RW.front_ns / ns, (double)RW.own_ns / ns, (double)RW.spin_ns / ns);
            memset(RW.ser_n, 0, sizeof RW.ser_n), memset(RW.ser_ns, 0, sizeof RW.ser_ns);
            RW.front_ns = RW.own_ns = RW.spin_ns = 0;
            rt_log("perf: replay workers: the chunks took %.3f ms per submission, every thread's",
                   (double)__atomic_exchange_n(&RW.chunk_ns, 0, __ATOMIC_RELAXED) / ns);
            if (RW.pipe) {
                o = 0;
                for (int k = 0; k < PW_N; k++)
                    o += snprintf(e + o, sizeof e - (size_t)o, "%s %s %.2f (%.3f ms)", k ? "," : "", pw_name[k],
                                  (double)RW.pw_n[k] / (double)n_subs, (double)RW.pw_ns[k] / ns);
                rt_log("perf: replay workers: past a group the submitting thread went on %.3f ms per submission until "
                       "it was done; waits for it per submission:%s", (double)RW.behind_ns / ns, e);
                o = 0;
                for (int k = 0; k < 5; k++) {   // the ops that waited the most often
                    int best = 0;
                    for (int op = 1; op < OP_COUNT_; op++)
                        if (RW.pd_op[op] > RW.pd_op[best]) best = op;
                    if (!RW.pd_op[best]) break;
                    o += snprintf(e + o, sizeof e - (size_t)o, "%s %s %.2f", k ? "," : "", nvn_op_name((uint32_t)best),
                                  (double)RW.pd_op[best] / (double)n_subs);
                    RW.pd_op[best] = 0;
                }
                if (o) rt_log("perf: replay workers: ops that waited for a group, per submission:%s", e);
                uint64_t td[3];
                vk_texture_deferred_stats(td);
                rt_log("perf: replay workers: texture lookups put off until their turn, per submission: no entry "
                       "%.2f, a retired one %.2f, a released one %.2f", (double)td[0] / (double)n_subs,
                       (double)td[1] / (double)n_subs, (double)td[2] / (double)n_subs);
                memset(RW.pw_n, 0, sizeof RW.pw_n), memset(RW.pw_ns, 0, sizeof RW.pw_ns);
                memset(RW.pd_op, 0, sizeof RW.pd_op);
                RW.behind_ns = 0;
                if (RW.small)
                    rt_log("perf: replay workers: groups one worker took whole (NX2NAVI_REPLAY_SMALL): %.2f per "
                           "submission, %.1f draws each", (double)RW.small_groups / (double)n_subs,
                           RW.small_groups ? (double)RW.small_draws / (double)RW.small_groups : 0.0);
            }
        }
        RW.small_groups = RW.small_draws = 0;
        RW.groups = RW.group_draws = RW.chunks = RW.serial_draws = RW.wait_ns = 0;
        evo_held = evo_resumed = evo_forced = evo_switches = 0;
        n_subs = n_parts = n_idle_parts = n_pass_splits = n_cuts = 0;
    }
    rec_spent_ns += kern_now_ns() - in_ns;
    if (rec_spent_ns >= REC_CLAIM_NS) rec_claim();
    gfx_tl_note('r', 0, (uint64_t)(ex->recorded - part_start), 0);
    return ex->recorded - part_start;   // what the last part holds
}
