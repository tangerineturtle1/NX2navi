// The mesh fallback's buffers and records. A task or mesh program runs as a compute dispatch that writes a storage
// buffer (mesh_fb.c), and the draw reads it:
//   task buffer   a task workgroup's payload at its index, payload-size apart
//   list          per mesh workgroup after a task stage: its task and its index among that task's (meshfb_scan.comp)
//   mesh buffer   a mesh workgroup's vertex lines and primitive area at its slot
//   records       per draw 64 bytes: the indirect arguments of its task dispatch, its mesh dispatch and its draw
// Every module and the scan reach the three buffers through a storage binding and check what they write and read
// against its length, so a binding of a part of a buffer (a window) is to them a buffer of its own:
//   A draw drawn by itself (vk_exec_draw.c mfb_draw) gets the whole buffers. Its dispatches write them and its vertex
//   stage has read them before the next draw's dispatches run, with a barrier in between.
//   The draws of one batch, whose dispatches run together before their pass (NX2NAVI_MESH_FB_BATCH), each get windows
//   of their own, handed out one after the other from the buffers' starts (vk_meshfb_window). A batch ends with its
//   pre-pass command buffer (vk_meshfb_batch_reset), which runs after the part before it, where the last batch's
//   draws read their windows, so the next batch starts at the buffers' starts again. A draw's window is what it can
//   need where the CPU knows that (the workgroup count of a draw without a task stage), else what its program asked
//   for so far with a margin (MeshFbNeed); a window that does not fit what is left ends the batch (the caller cuts
//   the pass there) rather than shrink.
// mesh_fallback_mb sets the size: an eighth for the task buffer, the rest for the mesh buffer. Workgroups past a
// buffer's or a window's end write nothing and draw nothing; the draws that had too little room are counted and the
// log names the size they needed, and a window too small makes its program's next windows larger.
// A record is used by one draw and written again only after MESHFB_RECS further draws. What its draw asked for is
// read back once the draw's submission is done (drain), so a program's need (MeshFbNeed) is known a few frames
// after its draws, not when the ring comes round again about 80 frames later.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "mesh_fb.h"
#include "vk.h"
#include "vk_mesh_fb.h"
#include "meshfb_scan_spv.h"
#include "meshfb_clamp_spv.h"

enum { MESHFB_RECS = 4096, REC_BYTES = 64 };

static struct {
    int tried, ok;
    VkBuffer task, mesh, list, rec, stat;
    VmaAllocation task_a, mesh_a, list_a, rec_a, stat_a;
    VkDeviceSize task_bytes, mesh_bytes, list_bytes;   // as one storage binding spans them
    const uint32_t *stat_map;                          // per record: mesh workgroups and task workgroups it asked for
    uint32_t cap_slots[MESHFB_RECS], slot_words[MESHFB_RECS], cap_tasks[MESHFB_RECS];   // what the record's draw had room for
    uint8_t noted[MESHFB_RECS];
    uint64_t fence[MESHFB_RECS];      // the timeline value after which the record can be read back
    MeshFbNeed *need[MESHFB_RECS];    // the draw's program's, which learns what the draw asked for
    // the perf line's report of the most one pass asks for (NX2NAVI_TRACE=perf), and the need's ratio (has_task)
    uint32_t group[MESHFB_RECS], task_words[MESHFB_RECS];   // the draw's group (vk_meshfb_record); its task payload's size
    uint64_t bound[MESHFB_RECS];                            // the most mesh workgroups the CPU could tell it needs (0: unknown)
    uint8_t has_task[MESHFB_RECS], indirect[MESHFB_RECS];
    uint32_t next, drained;   // records handed out; records read back (or passed over), in ring order
    uint64_t draws, short_draws, worst_bytes, clipped;   // clipped: of short_draws, those whose window was a part
    // the windows (WIN_*): the current one, which the bindings get (vk_meshfb_buffer), and each record's
    VkDescriptorBufferInfo cur[3];
    VkDeviceSize rwin[MESHFB_RECS][3][2];   // offset and range
    VkDeviceSize used[3], align;           // the batch's windows end at used; offsets are multiples of align
    uint64_t serial;                       // grows with every change of the current windows (vk_meshfb_window_serial)
    uint32_t batch_draws;
    // NX2NAVI_TRACE=perf, since the last perf line: batches ended, draws batched, batches ended because a window did not
    // fit, the most draws and window bytes of one batch, draws clipped to their window, and draws drawn by themselves
    // with the batches on (vk_meshfb_solo, by reason)
    uint64_t batches, batched, cuts, most_draws, most_bytes, clipped_p, solo[3];
    uint64_t cuts_total;   // batches ended because a window did not fit, in all (vk_meshfb_cuts_total)
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline scan, clamp;
} M;

enum { WIN_TASK, WIN_MESH, WIN_LIST };

static VkDescriptorBufferInfo whole(int k) {
    return k == WIN_TASK ? (VkDescriptorBufferInfo){M.task, 0, M.task_bytes}
         : k == WIN_LIST ? (VkDescriptorBufferInfo){M.list, 0, M.list_bytes} : (VkDescriptorBufferInfo){M.mesh, 0, M.mesh_bytes};
}

// What the checked records of one group (draws that one pass would batch) asked for.
typedef struct { uint32_t id, draws, list, unbounded; uint64_t mesh, task, bound; } PerfGroup;
// The perf line's totals since the last one (perf_account, vk_meshfb_perf_log).
static struct {
    uint64_t draws, groups, indirect, with_task, largest, largest_bound;   // largest: the most one draw asked for, in bytes
    PerfGroup cur, best;
} PF;

static int make_buffer(VkCtx *c, VkDeviceSize size, VkBufferUsageFlags usage, int host, VkBuffer *b, VmaAllocation *a, void **map) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    VmaAllocationCreateInfo ai = {.usage = host ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                  .flags = host ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT : 0};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &ai, b, a, &info) != VK_SUCCESS) return *b = VK_NULL_HANDLE, 0;
    if (map) *map = info.pMappedData;
    return !host || info.pMappedData;
}

int vk_meshfb_init(void) {
    if (M.tried) return M.ok;
    M.tried = 1;
    VkCtx *c = vk_ctx();
    if (!c) return 0;
    uint64_t mb = (uint64_t)cfg_int("NX2NAVI_MESH_FALLBACK_MB");
    if (mb < 16) mb = 16;
    if (mb > 4096) mb = 4096;
    VkDeviceSize range = c->props.limits.maxStorageBufferRange & ~(VkDeviceSize)3, task = (mb << 20) / 8, mesh = (mb << 20) - task;
    VkDeviceSize list = mesh / 128;   // an entry per slot of 512 bytes, and no mesh program's slot is smaller
    void *map = NULL;
    const VkBufferUsageFlags st = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (!make_buffer(c, task, st, 0, &M.task, &M.task_a, NULL) || !make_buffer(c, mesh, st, 0, &M.mesh, &M.mesh_a, NULL) ||
        !make_buffer(c, list, st, 0, &M.list, &M.list_a, NULL) ||
        !make_buffer(c, (VkDeviceSize)MESHFB_RECS * REC_BYTES,
                     st | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0,
                     &M.rec, &M.rec_a, NULL) ||
        !make_buffer(c, (VkDeviceSize)MESHFB_RECS * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1, &M.stat, &M.stat_a, &map)) {
        rt_log("vk: mesh fallback: its buffers (%llu MiB, mesh_fallback_mb) could not be allocated: task and mesh draws are skipped",
               (unsigned long long)mb);
        return 0;
    }
    M.stat_map = map;
    memset(map, 0, (size_t)MESHFB_RECS * 8);
    M.task_bytes = task < range ? task : range, M.mesh_bytes = mesh < range ? mesh : range, M.list_bytes = list < range ? list : range;
    M.align = c->props.limits.minStorageBufferOffsetAlignment;
    if (M.align < 4) M.align = 4;
    for (int k = 0; k < 3; k++) M.cur[k] = whole(k);

    M.push = (PFN_vkCmdPushDescriptorSetKHR)vk_dpa(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b[4];
    for (int i = 0; i < 4; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 4, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof meshfb_scan_spv,
                                   .pCode = meshfb_scan_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (!M.push || vkCreateDescriptorSetLayout(c->device, &dci, NULL, &M.dsl) != VK_SUCCESS) goto fail;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &M.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &M.layout) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS)
        goto fail;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = M.layout};
    VkResult res = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &M.scan);
    vkDestroyShaderModule(c->device, m, NULL);
    if (res != VK_SUCCESS) goto fail;
    // the clamp of a count the GPU wrote (vk_meshfb_clamp): binding 0 and three push constants of the same layout
    mi.codeSize = sizeof meshfb_clamp_spv, mi.pCode = meshfb_clamp_spv, m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) goto fail;
    ci.stage.module = m;
    res = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &M.clamp);
    vkDestroyShaderModule(c->device, m, NULL);
    if (res != VK_SUCCESS) goto fail;
    rt_log("vk: mesh fallback: %llu MiB for task payloads, %llu MiB for mesh output (mesh_fallback_mb = %llu)",
           (unsigned long long)(M.task_bytes >> 20), (unsigned long long)(M.mesh_bytes >> 20), (unsigned long long)mb);
    return M.ok = 1;
fail:
    rt_log("vk: mesh fallback: the scan or clamp pipeline could not be created: task and mesh draws are skipped");
    return 0;
}

VkDescriptorBufferInfo vk_meshfb_buffer(int cb_slot) {
    return M.cur[cb_slot == MESHFB_CB_TASK ? WIN_TASK : cb_slot == MESHFB_CB_LIST ? WIN_LIST : WIN_MESH];
}

uint64_t vk_meshfb_window_serial(void) { return M.serial; }

void vk_meshfb_window_whole(void) {
    if (M.cur[WIN_MESH].range == M.mesh_bytes && !M.cur[WIN_MESH].offset && M.cur[WIN_TASK].range == M.task_bytes &&
        !M.cur[WIN_TASK].offset && M.cur[WIN_LIST].range == M.list_bytes && !M.cur[WIN_LIST].offset)
        return;
    for (int k = 0; k < 3; k++) M.cur[k] = whole(k);
    M.serial++;
}

static void drain(int reuse);
static VkDeviceSize round_up(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }
static uint64_t at_most(uint64_t v, uint64_t most) { return v < most ? v : most; }

int vk_meshfb_window(const MeshFbNeed *need, int has_task, int indirect, uint32_t count, uint32_t fan, uint32_t slot_words,
                     uint32_t task_words) {
    drain(0);   // what the program's draws in done submissions asked for
    if (!slot_words) slot_words = 1;
    if (!task_words) task_words = 1;
    if (fan > 255) fan = 255;   // as the scan reads it
    uint64_t room = M.mesh_bytes / 4 / slot_words, task_room = M.task_bytes / 4 / task_words;
    if (has_task && room > M.list_bytes / 4) room = M.list_bytes / 4;
    uint64_t slots, tasks = 0;
    if (!has_task) {
        // a direct draw's workgroup count is its need; an indirect one's is what the program asked for at most so far
        slots = !indirect ? count : need->seen ? (uint64_t)need->max_want * 3 / 2 + 64 : room / 2;
    } else if (!indirect) {
        // its task workgroups exactly; each starts fan mesh workgroups at most, and as many as the program's most so far
        tasks = count;
        uint64_t bound = (uint64_t)count * fan;
        slots = need->seen ? at_most(bound, (uint64_t)count * need->ratio16 * 3 / 32 + 64) : at_most(bound, room / 2);
    } else {
        tasks = need->seen ? (uint64_t)need->max_tasks * 3 / 2 + 16 : task_room / 2;
        slots = need->seen ? (uint64_t)need->max_want * 3 / 2 + 64 : room / 2;
    }
    slots = at_most(slots ? slots : 1, room), tasks = at_most(tasks, task_room);
    VkDeviceSize bytes[3] = {has_task ? tasks * task_words * 4 : 0, slots * slot_words * 4, has_task ? slots * 4 : 0};
    VkDeviceSize total[3] = {M.task_bytes, M.mesh_bytes, M.list_bytes}, off[3];
    for (int k = 0; k < 3; k++) {
        off[k] = round_up(M.used[k], M.align);
        if (bytes[k] && off[k] + bytes[k] > total[k]) {
            if (M.batch_draws) return M.cuts++, M.cuts_total++, 0;   // the caller cuts and asks again, the batch empty
            off[k] = 0;
        }
    }
    for (int k = 0; k < 3; k++) {
        // a stage the draw does not have binds nothing of the buffer: a range of one word keeps the binding valid
        M.cur[k] = (VkDescriptorBufferInfo){whole(k).buffer, bytes[k] ? off[k] : 0, bytes[k] ? bytes[k] : 4};
        if (bytes[k]) M.used[k] = off[k] + bytes[k];
    }
    M.serial++;
    M.batch_draws++, M.batched++;
    VkDeviceSize b = M.used[WIN_MESH] + M.used[WIN_TASK] + M.used[WIN_LIST];
    if (b > M.most_bytes) M.most_bytes = b;
    if (M.batch_draws > M.most_draws) M.most_draws = M.batch_draws;
    return 1;
}

uint64_t vk_meshfb_cuts_total(void) { return M.cuts_total; }

void vk_meshfb_batch_reset(void) {
    if (M.batch_draws) M.batches++;
    M.batch_draws = 0;
    M.used[0] = M.used[1] = M.used[2] = 0;
}

uint64_t vk_meshfb_mesh_words(void) { return M.mesh_bytes / 4; }

// Adds the record's draw to the perf line's group totals. The records come in ring order, so a group's draws are
// contiguous: a new group id closes the one before, and the group that asked for the most mesh bytes is kept.
static void perf_account(uint32_t i, uint32_t want, uint32_t tasks) {
    PerfGroup *g = &PF.cur;
    if (g->draws && g->id != M.group[i]) {
        if (g->mesh >= PF.best.mesh) PF.best = *g;
        PF.groups++;
        memset(g, 0, sizeof *g);
    }
    uint64_t sw = (uint64_t)M.slot_words[i] * 4, asked = (uint64_t)want * sw;
    g->id = M.group[i];
    g->draws++;
    g->mesh += asked;
    g->task += (uint64_t)tasks * M.task_words[i] * 4;
    if (M.has_task[i]) g->list += want;
    if (M.bound[i]) g->bound += M.bound[i] * sw;
    else g->unbounded++;
    PF.draws++;
    PF.indirect += M.indirect[i];
    PF.with_task += M.has_task[i];
    if (asked >= PF.largest) PF.largest = asked, PF.largest_bound = M.bound[i] * sw;
}

// What a draw asked for, into its program's need: the most mesh and task workgroups, and the most mesh workgroups per
// task workgroup. With fewer task workgroups run than asked for (the task buffer's room), the mesh workgroups the
// others would have asked for are counted in at that ratio.
static void need_note(MeshFbNeed *n, uint32_t i, uint32_t want, uint32_t tasks) {
    uint64_t est = want;
    if (M.has_task[i]) {
        uint32_t ran = tasks < M.cap_tasks[i] ? tasks : M.cap_tasks[i];
        if (ran) {
            uint64_t r = ((uint64_t)want * 16 + ran - 1) / ran;
            if (r > n->ratio16) n->ratio16 = r > 0xffffffffu ? 0xffffffffu : (uint32_t)r;
            if (ran < tasks) est = (uint64_t)want * tasks / ran;
        }
        if (tasks > n->max_tasks) n->max_tasks = tasks;
    }
    if (est > n->max_want) n->max_want = est > 0xffffffffu ? 0xffffffffu : (uint32_t)est;
    n->seen++;
}

// A record whose draw has run: what it asked for against what it had room for, and into its program's need.
// `account`: also count it for the perf line (only the ring's own walk, not the sweep of vk_meshfb_short, which is out
// of order).
static void record_check(uint32_t i, int account) {
    if (!M.noted[i]) return;
    M.noted[i] = 0;
    uint32_t want = M.stat_map[2 * i], tasks = M.stat_map[2 * i + 1];
    if (M.need[i]) need_note(M.need[i], i, want, tasks);
    if (account && (rt_trace & RT_TRACE_PERF)) perf_account(i, want, tasks);
    if (want <= M.cap_slots[i] && tasks <= M.cap_tasks[i]) return;
    uint64_t need = (uint64_t)want * M.slot_words[i] * 4;
    if (need > M.worst_bytes) M.worst_bytes = need;
    uint64_t n = ++M.short_draws;
    // room in whole slots and payloads: a window may end short of the buffer's end by less than one
    uint64_t room = M.mesh_bytes / 4 / (M.slot_words[i] ? M.slot_words[i] : 1), task_room = 0;
    if (M.has_task[i] && room > M.list_bytes / 4) room = M.list_bytes / 4;
    if (M.has_task[i]) task_room = M.task_bytes / 4 / (M.task_words[i] ? M.task_words[i] : 1);
    if (M.cap_slots[i] < room || (M.has_task[i] && M.cap_tasks[i] < task_room)) {
        // a window, sized from what the program asked for before (vk_meshfb_window): its need now holds this draw's
        uint64_t k = ++M.clipped;
        M.clipped_p++;
        if (k <= 16 || !(k & (k - 1))) {
            char from[96] = "";
            if (tasks) snprintf(from, sizeof from, " from %u task workgroups (room for %u)", tasks, M.cap_tasks[i]);
            rt_log("vk: mesh fallback: a batched draw did not fit its window: its programs emitted %u mesh workgroups of %u "
                   "bytes%s, and the window holds %u: what did not fit was not drawn, and the program's next windows are "
                   "sized for it (%llu such draws of %llu)", want, M.slot_words[i] * 4, from, M.cap_slots[i],
                   (unsigned long long)k, (unsigned long long)M.draws);
        }
        return;
    }
    if (n <= 4 || !(n & (n - 1))) {
        char from[96] = "";
        if (tasks) snprintf(from, sizeof from, " from %u task workgroups (room for %u)", tasks, M.cap_tasks[i]);
        rt_log("vk: mesh fallback: a draw's programs emitted %u mesh workgroups of %u bytes%s, and the mesh buffer holds %u: "
               "what did not fit was not drawn (%llu such draws of %llu; mesh_fallback_mb of %llu or more would hold the "
               "largest so far)", want, M.slot_words[i] * 4, from, M.cap_slots[i], (unsigned long long)n,
               (unsigned long long)M.draws, (unsigned long long)((M.worst_bytes * 8 / 7 >> 20) + 1));
    }
}

// Reads back the records whose submissions are done, in ring order (their fences only grow), and before the ring
// hands out a record again the one it held, done or not, as before the read back by fence. The host's copy of the
// words is made current first: the stat buffer may be host memory that is cached and not coherent.
static void drain(int reuse) {
    uint64_t done = vk_sync_fence_done();
    uint32_t i = M.drained % MESHFB_RECS;
    int ready = M.drained != M.next && M.fence[i] <= done;
    if (!ready && !(reuse && M.next - M.drained >= MESHFB_RECS)) return;
    vmaInvalidateAllocation(vk_ctx()->vma, M.stat_a, 0, VK_WHOLE_SIZE);
    for (; M.drained != M.next && M.fence[M.drained % MESHFB_RECS] <= done; M.drained++) record_check(M.drained % MESHFB_RECS, 1);
    if (reuse && M.next - M.drained >= MESHFB_RECS) record_check(M.drained++ % MESHFB_RECS, 1);
}

VkDeviceSize vk_meshfb_record(VkCommandBuffer cb, VkBuffer src, VkDeviceSize src_off, uint32_t count, int has_task,
                              uint32_t verts, uint32_t slot_words, uint32_t group, uint64_t bound_slots,
                              MeshFbNeed *need, uint64_t fence, VkBuffer *rec, int *cmds) {
    drain(1);
    uint32_t i = M.next++ % MESHFB_RECS;
    VkDeviceSize at = (VkDeviceSize)i * REC_BYTES;
    M.draws++;
    M.fence[i] = fence, M.need[i] = need;
    M.group[i] = group, M.bound[i] = bound_slots, M.task_words[i] = 0;
    M.has_task[i] = (uint8_t)(has_task != 0), M.indirect[i] = src != VK_NULL_HANDLE;
    for (int k = 0; k < 3; k++) M.rwin[i][k][0] = M.cur[k].offset, M.rwin[i][k][1] = M.cur[k].range;
    M.slot_words[i] = slot_words, M.cap_slots[i] = (uint32_t)(M.cur[WIN_MESH].range / 4 / (slot_words ? slot_words : 1));
    M.cap_tasks[i] = 0xffffffffu;   // vk_meshfb_scan knows the payload size
    if (has_task && M.cap_slots[i] > M.cur[WIN_LIST].range / 4) M.cap_slots[i] = (uint32_t)(M.cur[WIN_LIST].range / 4);
    *rec = M.rec;
    int n = 0;
    // Every word of the record is written by exactly one command. A fill of the whole record followed by fills and
    // copies into parts of it were transfer writes to the same bytes without a barrier between them, which the GPU may
    // run in any order: on an RX 580 the vertex count, the first vertex or instance or the task count was the fill's 1
    // in about half the draws, and the foliage of the mesh programs flashed (missing, or white from the wrong slots).
    // Fills and copies only: they go through the recorder thread (vk_rec.h) like the replay's other commands.
    uint32_t r[REC_BYTES / 4], copied = 0;   // copied: the words the GPU-written count goes to
    for (uint32_t k = 0; k < REC_BYTES / 4; k++) r[k] = 1;
    // where the workgroup count goes: the task dispatch, or the mesh dispatch, the draw's instances and the report
    VkDeviceSize to[3] = {has_task ? 0 : 16, 36, 48};
    int nto = has_task ? 1 : 3;
    if (!has_task) r[32 / 4] = verts, r[40 / 4] = 0, r[44 / 4] = 0, r[52 / 4] = 0;
    for (int k = 0; k < nto; k++) {
        if (src) copied |= 1u << (to[k] / 4);
        else r[to[k] / 4] = count;
    }
    for (uint32_t k = 0; k < REC_BYTES / 4;) {
        if (copied >> k & 1) { k++; continue; }
        uint32_t e = k + 1;
        while (e < REC_BYTES / 4 && !(copied >> e & 1) && r[e] == r[k]) e++;
        vkCmdFillBuffer(cb, M.rec, at + 4 * (VkDeviceSize)k, 4 * (VkDeviceSize)(e - k), r[k]), n++;
        k = e;
    }
    if (src) {
        VkBufferCopy cp[3];
        for (int k = 0; k < nto; k++) cp[k] = (VkBufferCopy){src_off, at + to[k], 4};
        vkCmdCopyBuffer(cb, src, M.rec, (uint32_t)nto, cp), n++;
    }
    *cmds = n;
    return at;
}

void vk_meshfb_scan(VkCommandBuffer cb, VkDeviceSize at, uint32_t task_words, uint32_t fan, uint32_t slot_words, uint32_t verts) {
    uint32_t i = (uint32_t)(at / REC_BYTES);
    M.cap_tasks[i] = (uint32_t)(M.rwin[i][WIN_TASK][1] / 4 / (task_words ? task_words : 1));
    M.task_words[i] = task_words;
    // the record's windows: the scan counts the tasks and the room in them, as the modules do
    VkDescriptorBufferInfo bi[4] = {{M.rec, 0, VK_WHOLE_SIZE}, {M.task, M.rwin[i][WIN_TASK][0], M.rwin[i][WIN_TASK][1]},
                                    {M.list, M.rwin[i][WIN_LIST][0], M.rwin[i][WIN_LIST][1]},
                                    {M.mesh, M.rwin[i][WIN_MESH][0], M.rwin[i][WIN_MESH][1]}};
    VkWriteDescriptorSet w[4];
    for (int k = 0; k < 4; k++)
        w[k] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)k,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .pBufferInfo = &bi[k]};
    uint32_t pc[6] = {(uint32_t)(at / 4), task_words, fan, slot_words, verts, vk_ctx()->props.limits.maxComputeWorkGroupCount[0]};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, M.scan);
    rec_push(M.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, M.layout, 0, 4, w);
    vkCmdPushConstants(cb, M.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, 1, 1, 1);
}

// A count the GPU wrote, copied into record `at` (vk_meshfb_record with a source), cut to what the record's windows hold
// before a dispatch reads it (meshfb_clamp.comp says why). The room is the record's own: the task window's payloads,
// or without a task stage the mesh window's slots (cap_slots, as record_check measures the draw against). A batched
// draw's windows are its own parts of the buffers, so a garbage count launches no more workgroups and instances than
// its window holds, the same that its modules and vertex stage would let write and draw, and nothing of another draw's
// window is touched. task_words: the task stage's payload words, 0 without a task stage. One single-thread dispatch;
// the caller orders it after the record's copies and before the first dispatch.
void vk_meshfb_clamp(VkCommandBuffer cb, VkDeviceSize at, uint32_t task_words) {
    uint32_t i = (uint32_t)(at / REC_BYTES), most = vk_ctx()->props.limits.maxComputeWorkGroupCount[0];
    uint32_t room = task_words ? (uint32_t)(M.rwin[i][WIN_TASK][1] / 4 / task_words) : M.cap_slots[i];
    if (room > most) room = most;
    VkDescriptorBufferInfo bi = {M.rec, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi};
    uint32_t pc[3] = {(uint32_t)(at / 4), task_words != 0, room};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, M.clamp);
    rec_push(M.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, M.layout, 0, 1, &w);
    vkCmdPushConstants(cb, M.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, 1, 1, 1);
}

void vk_meshfb_note(VkCommandBuffer cb, VkDeviceSize at) { vk_meshfb_note_many(cb, &at, 1); }

void vk_meshfb_note_many(VkCommandBuffer cb, const VkDeviceSize *at, int n) {
    VkBufferCopy cp[256];
    for (int k = 0; k < n;) {
        int m = 0;
        for (; k < n && m < 256; k++, m++) {
            uint32_t i = (uint32_t)(at[k] / REC_BYTES);
            cp[m] = (VkBufferCopy){at[k] + 48, (VkDeviceSize)i * 8, 8};
            M.noted[i] = 1;
        }
        vkCmdCopyBuffer(cb, M.rec, M.stat, (uint32_t)m, cp);
    }
}

void vk_meshfb_solo(int why) {
    if (why >= 0 && why < 3) M.solo[why]++;
}

uint64_t vk_meshfb_short(void) {
    if (!M.ok) return 0;
    vmaInvalidateAllocation(vk_ctx()->vma, M.stat_a, 0, VK_WHOLE_SIZE);
    for (uint32_t i = 0; i < MESHFB_RECS; i++) record_check(i, 0);
    M.drained = M.next;
    return M.short_draws;
}

uint64_t vk_meshfb_clipped(void) { return M.clipped; }

void vk_meshfb_perf_log(uint64_t frame) {
    if (!M.ok || !(rt_trace & RT_TRACE_PERF) || frame % 600 || !PF.draws) return;
    const double mib = 1048576.0;
    const PerfGroup *b = PF.cur.mesh > PF.best.mesh ? &PF.cur : &PF.best;   // the group still open counts too
    rt_log("perf: frame %llu: mesh fallback: %llu draws in %llu pass groups (%llu indirect, %llu with a task stage); most in "
           "one group: %u draws, %.1f MiB mesh asked, %.2f MiB task payloads, %u list entries, bounds %.1f MiB + %u unbounded "
           "draws; largest draw %.1f MiB asked (bound %.1f MiB); mesh buffer %llu MiB", (unsigned long long)frame,
           (unsigned long long)PF.draws, (unsigned long long)(PF.groups + (PF.cur.draws != 0)),
           (unsigned long long)PF.indirect, (unsigned long long)PF.with_task, b->draws, (double)b->mesh / mib,
           (double)b->task / mib, b->list, (double)b->bound / mib, b->unbounded, (double)PF.largest / mib,
           (double)PF.largest_bound / mib, (unsigned long long)(M.mesh_bytes >> 20));
    uint64_t solo = M.solo[0] + M.solo[1] + M.solo[2];
    if (M.batched || solo)
        rt_log("perf: frame %llu: mesh fallback batches: %llu draws batched in %llu batches (at most %llu draws and %.1f MiB "
               "of windows in one), %llu batches ended because a window did not fit, %llu draws did not fit their window; "
               "%llu draws by themselves (%llu with a compute layout that is not pushed, %llu with no room for a cut, %llu "
               "without a pre-pass command buffer)", (unsigned long long)frame, (unsigned long long)M.batched,
               (unsigned long long)M.batches, (unsigned long long)M.most_draws, (double)M.most_bytes / mib,
               (unsigned long long)M.cuts, (unsigned long long)M.clipped_p, (unsigned long long)solo,
               (unsigned long long)M.solo[0], (unsigned long long)M.solo[1], (unsigned long long)M.solo[2]);
    M.batches = M.batched = M.cuts = M.most_draws = M.most_bytes = M.clipped_p = 0;
    memset(M.solo, 0, sizeof M.solo);
    PerfGroup open = PF.cur;   // the draws of the group still open belong to the next period too: its id stays
    memset(&PF, 0, sizeof PF);
    PF.cur.id = open.id;
}

void vk_meshfb_shutdown(void) {
    VkCtx *c = vk_ctx();
    if (!c || !M.tried) return;
    if (vk_meshfb_short())
        rt_log("vk: mesh fallback: %llu of %llu draws emitted more than the buffers hold, %llu of them more than their window "
               "of a batch (mesh_fallback_mb of %llu or more would hold the largest)", (unsigned long long)M.short_draws,
               (unsigned long long)M.draws, (unsigned long long)M.clipped, (unsigned long long)((M.worst_bytes * 8 / 7 >> 20) + 1));
    if (M.scan) vkDestroyPipeline(c->device, M.scan, NULL);
    if (M.clamp) vkDestroyPipeline(c->device, M.clamp, NULL);
    if (M.layout) vkDestroyPipelineLayout(c->device, M.layout, NULL);
    if (M.dsl) vkDestroyDescriptorSetLayout(c->device, M.dsl, NULL);
    if (M.task) vmaDestroyBuffer(c->vma, M.task, M.task_a);
    if (M.mesh) vmaDestroyBuffer(c->vma, M.mesh, M.mesh_a);
    if (M.list) vmaDestroyBuffer(c->vma, M.list, M.list_a);
    if (M.rec) vmaDestroyBuffer(c->vma, M.rec, M.rec_a);
    if (M.stat) vmaDestroyBuffer(c->vma, M.stat, M.stat_a);
    memset(&M, 0, sizeof M);
}
