// The mesh shader fallback's modules (mesh_fb.c): what a task or mesh program computes, as compute modules that write
// it into storage buffers, and the vertex stage that draws from those buffers. No Vulkan calls here.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define MESHFB_MAX_LINES 136   // 8 system value components and 32 generic vectors of 4

// A task or mesh program's stage buffer, as tools/xlate/AmpereLowering.cs MeshInfo lays it out in the shared memory
// of the translated module.
typedef struct {
    int stage;                             // 6 task, 7 mesh
    uint32_t threads;                      // workgroup size
    uint32_t maxv, maxp;                   // mesh: vertex and primitive limits
    uint32_t ipp;                          // mesh: indices per primitive (1 points, 2 lines, 3 triangles)
    uint32_t payload_bytes;                // the task payload (task, or a mesh program a task feeds), with its header
    int has_task;                          // mesh: a task stage feeds it
    uint32_t nlines;                       // mesh: output components per vertex
    uint16_t lines[MESHFB_MAX_LINES];      // their attribute addresses: 0x6c point size, 0x70.. position, 0x80 + 16 g + 4 c
    uint32_t attr, map, pay, shared_bytes; // byte offsets in shared memory, and its size
} MeshFbInfo;

// Where a module's extra storage buffers are bound: res.json "cb_slot" of their entries (vk_exec_draw.c ssbo_info).
enum { MESHFB_CB_TASK = 0x41, MESHFB_CB_MESH = 0x42, MESHFB_CB_LIST = 0x43 };
// A list entry: the task workgroup in bits 0-23, the mesh workgroup's index among that task's in bits 24-31.
#define MESHFB_LIST_TASK_BITS 24

// From the program's control section and the first 0x80 bytes of its code (the header with the output map). 0 with
// err filled when the program uses what the fallback does not carry.
int meshfb_info(const uint8_t *control, const uint8_t *header, int stage, MeshFbInfo *out, char *err, size_t cap);
// Words one mesh workgroup's output takes in the mesh buffer: its vertex lines and its primitive area.
static inline uint32_t meshfb_slot_words(const MeshFbInfo *m) { return m->pay / 4; }

// The compute module of a TaskEXT module (spirv, as the translator wrote it): the same program, then the payload of
// workgroup t into words [t * payload_bytes / 4, ..) of the task buffer, a storage buffer at `binding`. malloc'd;
// NULL with err filled.
uint32_t *meshfb_task_cs(const uint32_t *spirv, uint64_t words, const MeshFbInfo *m, uint32_t binding, uint64_t *out_words,
                         char *err, size_t cap);
// The compute module of a MeshEXT module. Workgroup g writes its stage buffer (meshfb_slot_words) into slot g of the
// mesh buffer at binding + 1. After a task stage (m->has_task) entry g of the list at binding + 2 names its task
// workgroup and its index among that task's mesh workgroups, and the payload comes from the task buffer at `binding`,
// task_payload_words apart.
uint32_t *meshfb_mesh_cs(const uint32_t *spirv, uint64_t words, const MeshFbInfo *m, uint32_t binding,
                         uint32_t task_payload_words, uint64_t *out_words, char *err, size_t cap);
// The vertex module that draws the mesh buffer (binding 1): instance = slot, maxp * ipp vertices an instance; vertex
// index / ipp is the primitive, and a primitive past the slot's count is degenerate. Outputs: position, point size for
// points, and a 4-component vector at every Location of out_mask, of out_type[location] (GFX_TYPE_*).
uint32_t *meshfb_vs(const MeshFbInfo *m, uint32_t out_mask, const uint8_t out_type[32], uint64_t *out_words);
// A copy of the translator's res.json text res for a compute module made above: its stage is "cs" and its storage
// buffers gain the entries of the extra buffers (n of them from `binding` on, cb_slots[i] each). malloc'd.
char *meshfb_res_json(const char *res, uint32_t binding, const int *cb_slots, int n);
