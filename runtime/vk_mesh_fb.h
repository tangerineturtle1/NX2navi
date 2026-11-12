// The mesh fallback's buffers and its records (vk_mesh_fb.c); the modules are mesh_fb.c's, the draws
// vk_exec_draw.c's ("mesh fallback").
#pragma once
#include "vk.h"

// Creates the buffers and the scan pipeline on first use. 0 when that failed (logged once): the draws are skipped.
int vk_meshfb_init(void);
// The buffer a module's extra storage binding reads or writes (mesh_fb.h MESHFB_CB_*), whole or as much of it as one
// storage binding may span.
VkDescriptorBufferInfo vk_meshfb_buffer(int cb_slot);
uint64_t vk_meshfb_mesh_words(void);   // the mesh buffer's size as a binding sees it, in words

// A draw's record, 64 bytes of the record buffer (*rec) at the offset returned: the task dispatch at +0, the mesh
// dispatch at +16, the draw (VkDrawIndirectCommand) at +32. Records the fills and copies that start it, outside a
// pass: the workgroup count from 4 bytes at src_off of src, or `count` when src is VK_NULL_HANDLE. Without a task
// stage that count is the mesh dispatch and the draw's instance count; with one it is the task dispatch, and
// vk_meshfb_scan writes the rest. slot_words: of the draw's mesh program, for the report of a draw the mesh buffer
// was too small for. Returns the number of commands recorded in *cmds.
VkDeviceSize vk_meshfb_record(VkCommandBuffer cb, VkBuffer src, VkDeviceSize src_off, uint32_t count, int has_task,
                              uint32_t verts, uint32_t slot_words, VkBuffer *rec, int *cmds);
// The pass between the task and the mesh dispatch (shaders/meshfb_scan.comp): binds its own compute pipeline and set.
void vk_meshfb_scan(VkCommandBuffer cb, VkDeviceSize at, uint32_t task_words, uint32_t fan, uint32_t slot_words, uint32_t verts);
// After the record is complete (the copies or the scan, and a barrier): keeps what the draw asked for where the host
// reads it later. One copy command.
void vk_meshfb_note(VkCommandBuffer cb, VkDeviceSize at);
// Draws so far whose programs emitted more than the buffers hold, the records not looked at yet included: call it
// with the device idle (a probe, the shutdown).
uint64_t vk_meshfb_short(void);
void vk_meshfb_shutdown(void);   // the device must be idle
