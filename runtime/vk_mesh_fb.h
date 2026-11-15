// The mesh fallback's buffers and its records (vk_mesh_fb.c); the modules are mesh_fb.c's, the draws
// vk_exec_draw.c's ("mesh fallback").
#pragma once
#include "vk.h"

// Creates the buffers and the scan pipeline on first use. 0 when that failed (logged once): the draws are skipped.
int vk_meshfb_init(void);
// The buffer a module's extra storage binding reads or writes (mesh_fb.h MESHFB_CB_*): the current window of it, which
// is the whole buffer (as much of it as one storage binding may span) outside a batched draw.
VkDescriptorBufferInfo vk_meshfb_buffer(int cb_slot);
uint64_t vk_meshfb_mesh_words(void);   // the mesh buffer's size as a binding sees it, in words

// What the draws of one task and mesh program pair asked for so far, as the host read it back from their records
// (vk_mesh_fb.c record_check, once the submission of a draw is done). One per pair (vk_exec_draw.c MfbCs).
typedef struct MeshFbNeed {
    uint32_t seen;        // draws read back
    uint32_t max_want;    // the most mesh workgroups one draw asked for
    uint32_t max_tasks;   // the most task workgroups one draw asked for
    uint32_t ratio16;     // the most mesh workgroups per task workgroup one draw asked for, in sixteenths
} MeshFbNeed;

// The windows of a batched draw (vk_exec_draw.c, NX2NAVI_MESH_FB_BATCH): parts of the task, mesh and list buffers
// after the windows of the batch's draws before it, which become the current ones (vk_meshfb_buffer) and the next
// record's (vk_meshfb_record). Sized for the draw: a direct draw without a task stage `count` mesh workgroups; a direct
// draw with one `count` task workgroups and at most `fan` mesh workgroups each, fewer by what its program asked for so
// far; an indirect draw what its program asked for at most so far; each estimate doubled as margin and at least a
// 32nd of the mesh buffer and a 16th of the task buffer, and with nothing asked yet half the buffer. Never more than the whole buffer. 0 when a window does not fit what the batch left
// and the batch holds draws: nothing changes, and the caller ends the batch (vk_meshfb_batch_reset) and asks again.
int vk_meshfb_window(const MeshFbNeed *need, int has_task, int indirect, uint32_t count, uint32_t fan, uint32_t slot_words,
                     uint32_t task_words);
// The whole buffers become the current windows again.
void vk_meshfb_window_whole(void);
// A number that grows whenever the current windows change: the replay's skip of an equal descriptor build counts it in
// (vk_exec_draw.c bind_sig), as the windows are not in the bound state it compares.
uint64_t vk_meshfb_window_serial(void);
// The batch's command buffer has ended: the next batch's windows start at the buffers' starts.
void vk_meshfb_batch_reset(void);
// Batches ended so far because a window did not fit what they left (the probe).
uint64_t vk_meshfb_cuts_total(void);

// A draw's record, 64 bytes of the record buffer (*rec) at the offset returned: the task dispatch at +0, the mesh
// dispatch at +16, the draw (VkDrawIndirectCommand) at +32. Records the fills and copies that start it, outside a
// pass: the workgroup count from 4 bytes at src_off of src, or `count` when src is VK_NULL_HANDLE. Without a task
// stage that count is the mesh dispatch, the draw's instance count and the report's mesh word; with one it is the
// task dispatch and the report's task word, and vk_meshfb_scan writes the rest. slot_words: of the draw's mesh program, for the report of a draw the mesh buffer
// was too small for. group: the draws of one pass share a number (NX2NAVI_TRACE=perf adds up the draws of a group);
// bound_slots: the most mesh workgroups the CPU can tell the draw needs, 0 when it cannot (indirect). need: the
// program's, which gets what the draw asked for once it is read back; fence: the timeline value of the submission
// that runs the draw, after which it can be. The record keeps the current windows: the draw's room, and the scan's.
// Returns the number of commands recorded in *cmds.
VkDeviceSize vk_meshfb_record(VkCommandBuffer cb, VkBuffer src, VkDeviceSize src_off, uint32_t count, int has_task,
                              uint32_t verts, uint32_t slot_words, uint32_t group, uint64_t bound_slots,
                              MeshFbNeed *need, uint64_t fence, VkBuffer *rec, int *cmds);
// For a record whose count came from src (a count the GPU wrote): cuts it to what the record's windows hold (the whole
// buffers outside a batch), in the task dispatch (task_words, the task stage's payload words) or without a task stage
// (0) in the mesh dispatch and the instances (shaders/meshfb_clamp.comp). The report words keep the count asked for.
// Binds its own compute pipeline and set; four commands, after a barrier that orders the record's fills and copies
// and before one that orders the cut count for the dispatches.
void vk_meshfb_clamp(VkCommandBuffer cb, VkDeviceSize at, uint32_t task_words);
// The pass between the task and the mesh dispatch (shaders/meshfb_scan.comp), over the windows of the record at `at`:
// binds its own compute pipeline and set.
void vk_meshfb_scan(VkCommandBuffer cb, VkDeviceSize at, uint32_t task_words, uint32_t fan, uint32_t slot_words, uint32_t verts);
// After the record is complete (the copies or the scan, and a barrier): keeps what the draw asked for where the host
// reads it later. One copy command; vk_meshfb_note_many: one for the n records at at[] (a batch's).
void vk_meshfb_note(VkCommandBuffer cb, VkDeviceSize at);
void vk_meshfb_note_many(VkCommandBuffer cb, const VkDeviceSize *at, int n);
// A draw drawn by itself while the batches are on, for the perf line: why (0 a compute layout that is not pushed, 1 no
// room for another cut, 2 no pre-pass command buffer).
void vk_meshfb_solo(int why);
// NX2NAVI_TRACE=perf, every 600 frames: the line with the most mesh, task and list bytes one pass group asked for
// (draws are looked at once their submission is done, a few frames later), and with batches the line of their counts.
// Nothing when off.
void vk_meshfb_perf_log(uint64_t frame);
// Draws so far whose programs emitted more than the buffers or their windows hold, the records not looked at yet
// included: call it with the device idle (a probe, the shutdown). vk_meshfb_clipped: of those, the draws whose window
// was smaller than the buffers (call it after vk_meshfb_short).
uint64_t vk_meshfb_short(void);
uint64_t vk_meshfb_clipped(void);
void vk_meshfb_shutdown(void);   // the device must be idle
