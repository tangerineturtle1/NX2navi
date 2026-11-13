// NX2NAVI_FLIGHT=1 flight recorder (flight.c): the last frames' timing and event counts plus a thumbnail each, written to
// disk with a bug capture (F8, bugcap.c), after a slow frame or after a one-frame flash, so a glitch or stutter seen
// in play can be read after the fact.
#pragma once
#include <stdint.h>

#define FLIGHT_THUMB_W 640
#define FLIGHT_THUMB_H 360

extern int flight_on;
extern int vk_dump_white;   // vk_dump.c, NX2NAVI_DUMP_WHITE   // NX2NAVI_FLIGHT=1, set once by flight_init before the first present

void flight_init(void);   // prof_start calls it
// Its memory in bytes, for the memory report (vk_memory.c system_log): the records and texture events in process memory,
// and the thumbnail ring in host-visible memory. 0 while it is off.
void flight_mem(uint64_t *records, uint64_t *thumbs);
// perf_present, once per present on the render thread: the interval since the previous present, the render thread's
// blocked time per PERF_* kind in this frame, and the running PERF_CNT_* totals.
void flight_frame(uint64_t frame, uint64_t now_ns, uint64_t interval_ns, const uint64_t blocked_ns[],
                  const volatile int64_t counts[]);
// A bug capture at this frame (the render thread, before or after flight_frame): the recorder dumps the frames around
// it into dir. 0 when the recorder is off or busy with another dump.
int flight_capture(uint64_t frame, uint64_t interval_ns, const char *dir);

// Texture events of the recorded frames (vk_texture.c), written with each dump as events.tsv: a texture made (with how
// its content started), uploaded, uploaded again, released by the game, taken back after a release, or destroyed. Each
// carries the frame whose counts in frames.tsv it is part of. Any thread: the ring takes no lock. A no-op until
// flight_init turned the recorder on.
enum { FLIGHT_TEX_NEW, FLIGHT_TEX_UP, FLIGHT_TEX_REUP, FLIGHT_TEX_RELEASE, FLIGHT_TEX_TAKEBACK, FLIGHT_TEX_DROP,
       FLIGHT_TEX_BYTES,   // after UP and REUP: what the upload read (detail: a hash of 64 probes of its storage; us:
                           // how many of the probes were all zero), so a second upload tells new bytes from the same
       FLIGHT_TEX_REG,     // a slot registered with a CPU-written texture (us: the slot; detail: what it named before:
                           // 0 nothing, 1 another descriptor at this storage, 2 other storage, 3 a finalized texture)
       FLIGHT_TEX_BLANK,   // a shader sampled a slot that has no image of its own and follows none: its stand-in was
                           // on screen (us: the slot; detail: bit 0 the slot is finalized, bit 1 it is held out)
       FLIGHT_TEX_TOBLANK, // the pool's entry of a slot went from a texture's view to the stand-in (us: the slot;
                           // detail: vk_texture_slot_why_locked's bits, what the slot's own texture was then)
       FLIGHT_TEX_BLANKSUB,// a slot that lost its picture during a submission still had none when the next one began:
                           // the submission's draws read its stand-in (us and detail as TOBLANK)
       FLIGHT_TEX_DIRECT,  // a draw's own texture binding (not the pool's) named a slot and got the stand-in: that draw
                           // samples it whatever the slot resolves to later (us and detail as TOBLANK; once a frame)
       FLIGHT_TEX_UNREG,   // a shader sampled a pool slot that is not registered when its marks are read (us: the slot)
       FLIGHT_TEX_N };
// The texture's flags: a depth or stencil image, mirrored (the CPU writes its storage), bound into pool memory, an ASTC
// texture, written by the GPU (VkTexImage.gpu_owned).
enum { FLIGHT_TF_DEPTH = 1, FLIGHT_TF_STENCIL = 2, FLIGHT_TF_MIRRORED = 4, FLIGHT_TF_BOUND = 8, FLIGHT_TF_ASTC = 16,
       FLIGHT_TF_GPU = 32 };
// detail: for NEW how the content started (vk_texture_start_name), for UP and REUP the WriteTexels calls since the last
// upload, for DROP why (vk_texture_drop_name). us: an upload's time, else 0.
void flight_tex(int kind, uint64_t addr, uint32_t format, uint32_t w, uint32_t h, uint32_t layers, uint32_t flags,
                uint32_t detail, uint32_t us);
const char *vk_texture_start_name(int start);   // vk_texture.c: the names of Tex.og_start and of drop_tex's reasons
const char *vk_texture_drop_name(int why);

// A w x h RGBA picture as a compressed RGB PNG (Paeth rows, fixed-Huffman deflate); any thread, recorder on or off.
int flight_write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);

// gfx_present.c: the present thread blits every presented frame into a host-visible ring of n thumbnails.
void vk_thumbs_enable(int n);            // before the first present; n slots of FLIGHT_THUMB_W x FLIGHT_THUMB_H
void vk_thumbs_frame(uint64_t frame);    // the present thread names the frame of the vk_present_image call that follows
// Copies frame's thumbnail as RGBA when the ring still holds it complete; 0 when it was never written, is still in
// flight or was overwritten. Takes the present lock for one slot copy (about 20 us); any thread.
int vk_thumbs_read(uint64_t frame, uint8_t *rgba);

// The flash trigger (flight_flash.h): while flight_flash_on (set once by flight_init), gfx_present.c hands in the cell
// means (flash_cells, FLASH_CELLS bytes) of each thumbnail when it completes, on the present thread, in frame order.
// flight_cells judges the frame before and leaves a flash for flight_frame to dump.
extern int flight_flash_on;
void flight_cells(uint64_t frame, const uint8_t *cells);
