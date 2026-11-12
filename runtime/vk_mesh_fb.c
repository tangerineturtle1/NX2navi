// The mesh fallback's buffers and records. A task or mesh program runs as a compute dispatch that writes a storage
// buffer (mesh_fb.c), and the draw reads it:
//   task buffer   a task workgroup's payload at its index, payload-size apart
//   list          per mesh workgroup after a task stage: its task and its index among that task's (meshfb_scan.comp)
//   mesh buffer   a mesh workgroup's vertex lines and primitive area at its slot
//   records       per draw 64 bytes: the indirect arguments of its task dispatch, its mesh dispatch and its draw
// The three buffers are reused by every draw: a draw's dispatches write them and its vertex stage has read them
// before the next draw's dispatches run, with a barrier in between (vk_exec_draw.c mfb_draw), so their size is what
// the largest draw needs, not what a frame's draws need together. mesh_fallback_mb sets it: an eighth for the task
// buffer, the rest for the mesh buffer. Workgroups past either buffer's end write nothing and draw nothing (the
// modules check the buffer's length); the draws that happened to are counted and the log names the size they needed.
// A record is used by one draw and written again only after MESHFB_RECS further draws.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "mesh_fb.h"
#include "vk.h"
#include "vk_mesh_fb.h"
#include "meshfb_scan_spv.h"

enum { MESHFB_RECS = 4096, REC_BYTES = 64 };

static struct {
    int tried, ok;
    VkBuffer task, mesh, list, rec, stat;
    VmaAllocation task_a, mesh_a, list_a, rec_a, stat_a;
    VkDeviceSize task_bytes, mesh_bytes, list_bytes;   // as one storage binding spans them
    const uint32_t *stat_map;                          // per record: mesh workgroups and task workgroups it asked for
    uint32_t cap_slots[MESHFB_RECS], slot_words[MESHFB_RECS], cap_tasks[MESHFB_RECS];   // what the record's draw had room for
    uint8_t noted[MESHFB_RECS];
    uint32_t next;
    uint64_t draws, short_draws, worst_bytes;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline scan;
} M;

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
    rt_log("vk: mesh fallback: %llu MiB for task payloads, %llu MiB for mesh output (mesh_fallback_mb = %llu)",
           (unsigned long long)(M.task_bytes >> 20), (unsigned long long)(M.mesh_bytes >> 20), (unsigned long long)mb);
    return M.ok = 1;
fail:
    rt_log("vk: mesh fallback: the scan pipeline could not be created: task and mesh draws are skipped");
    return 0;
}

VkDescriptorBufferInfo vk_meshfb_buffer(int cb_slot) {
    if (cb_slot == MESHFB_CB_TASK) return (VkDescriptorBufferInfo){M.task, 0, M.task_bytes};
    if (cb_slot == MESHFB_CB_LIST) return (VkDescriptorBufferInfo){M.list, 0, M.list_bytes};
    return (VkDescriptorBufferInfo){M.mesh, 0, M.mesh_bytes};
}

uint64_t vk_meshfb_mesh_words(void) { return M.mesh_bytes / 4; }

// The record about to be written again: what its last draw asked for against what it had room for.
static void record_check(uint32_t i) {
    if (!M.noted[i]) return;
    M.noted[i] = 0;
    uint32_t want = M.stat_map[2 * i], tasks = M.stat_map[2 * i + 1];
    if (want <= M.cap_slots[i] && tasks <= M.cap_tasks[i]) return;
    uint64_t need = (uint64_t)want * M.slot_words[i] * 4;
    if (need > M.worst_bytes) M.worst_bytes = need;
    uint64_t n = ++M.short_draws;
    if (n <= 4 || !(n & (n - 1))) {
        char from[96] = "";
        if (tasks) snprintf(from, sizeof from, " from %u task workgroups (room for %u)", tasks, M.cap_tasks[i]);
        rt_log("vk: mesh fallback: a draw's programs emitted %u mesh workgroups of %u bytes%s, and the mesh buffer holds %u: "
               "what did not fit was not drawn (%llu such draws of %llu; mesh_fallback_mb of %llu or more would hold the "
               "largest so far)", want, M.slot_words[i] * 4, from, M.cap_slots[i], (unsigned long long)n,
               (unsigned long long)M.draws, (unsigned long long)((M.worst_bytes * 8 / 7 >> 20) + 1));
    }
}

VkDeviceSize vk_meshfb_record(VkCommandBuffer cb, VkBuffer src, VkDeviceSize src_off, uint32_t count, int has_task,
                              uint32_t verts, uint32_t slot_words, VkBuffer *rec, int *cmds) {
    uint32_t i = M.next++ % MESHFB_RECS;
    VkDeviceSize at = (VkDeviceSize)i * REC_BYTES;
    record_check(i);
    M.draws++;
    M.slot_words[i] = slot_words, M.cap_slots[i] = (uint32_t)(M.mesh_bytes / 4 / (slot_words ? slot_words : 1));
    M.cap_tasks[i] = 0xffffffffu;   // vk_meshfb_scan knows the payload size
    if (has_task && M.cap_slots[i] > M.list_bytes / 4) M.cap_slots[i] = (uint32_t)(M.list_bytes / 4);
    *rec = M.rec;
    int n = 0;
    vkCmdFillBuffer(cb, M.rec, at, REC_BYTES, 1), n++;
    // where the workgroup count goes: the task dispatch, or the mesh dispatch, the draw's instances and the report
    VkDeviceSize to[3] = {has_task ? at : at + 16, at + 36, at + 48};
    int nto = has_task ? 1 : 3;
    if (!has_task) {
        vkCmdFillBuffer(cb, M.rec, at + 32, 4, verts), n++;
        vkCmdFillBuffer(cb, M.rec, at + 40, 8, 0), n++;
        vkCmdFillBuffer(cb, M.rec, at + 52, 4, 0), n++;
    }
    if (src) {
        VkBufferCopy cp[3];
        for (int k = 0; k < nto; k++) cp[k] = (VkBufferCopy){src_off, to[k], 4};
        vkCmdCopyBuffer(cb, src, M.rec, (uint32_t)nto, cp), n++;
    } else {
        for (int k = 0; k < nto; k++) vkCmdFillBuffer(cb, M.rec, to[k], 4, count), n++;
    }
    *cmds = n;
    return at;
}

void vk_meshfb_scan(VkCommandBuffer cb, VkDeviceSize at, uint32_t task_words, uint32_t fan, uint32_t slot_words, uint32_t verts) {
    uint32_t i = (uint32_t)(at / REC_BYTES);
    M.cap_tasks[i] = (uint32_t)(M.task_bytes / 4 / (task_words ? task_words : 1));
    VkDescriptorBufferInfo bi[4] = {{M.rec, 0, VK_WHOLE_SIZE}, {M.task, 0, M.task_bytes}, {M.list, 0, M.list_bytes},
                                    {M.mesh, 0, M.mesh_bytes}};
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

void vk_meshfb_note(VkCommandBuffer cb, VkDeviceSize at) {
    uint32_t i = (uint32_t)(at / REC_BYTES);
    VkBufferCopy cp = {at + 48, (VkDeviceSize)i * 8, 8};
    vkCmdCopyBuffer(cb, M.rec, M.stat, 1, &cp);
    M.noted[i] = 1;
}

uint64_t vk_meshfb_short(void) {
    if (!M.ok) return 0;
    for (uint32_t i = 0; i < MESHFB_RECS; i++) record_check(i);
    return M.short_draws;
}

void vk_meshfb_shutdown(void) {
    VkCtx *c = vk_ctx();
    if (!c || !M.tried) return;
    if (vk_meshfb_short())
        rt_log("vk: mesh fallback: %llu of %llu draws emitted more than the buffers hold (mesh_fallback_mb of %llu or more would "
               "hold the largest)", (unsigned long long)M.short_draws, (unsigned long long)M.draws,
               (unsigned long long)((M.worst_bytes * 8 / 7 >> 20) + 1));
    if (M.scan) vkDestroyPipeline(c->device, M.scan, NULL);
    if (M.layout) vkDestroyPipelineLayout(c->device, M.layout, NULL);
    if (M.dsl) vkDestroyDescriptorSetLayout(c->device, M.dsl, NULL);
    if (M.task) vmaDestroyBuffer(c->vma, M.task, M.task_a);
    if (M.mesh) vmaDestroyBuffer(c->vma, M.mesh, M.mesh_a);
    if (M.list) vmaDestroyBuffer(c->vma, M.list, M.list_a);
    if (M.rec) vmaDestroyBuffer(c->vma, M.rec, M.rec_a);
    if (M.stat) vmaDestroyBuffer(c->vma, M.stat, M.stat_a);
    memset(&M, 0, sizeof M);
}
