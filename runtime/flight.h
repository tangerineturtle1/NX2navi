// NX2NAVI_FLIGHT=1 flight recorder (flight.c): the last frames' timing and event counts plus a thumbnail each, written to
// disk on F8 or after a slow frame, so a glitch or stutter seen in play can be read after the fact.
#pragma once
#include <stdint.h>

#define FLIGHT_THUMB_W 320
#define FLIGHT_THUMB_H 180

extern int flight_on;
extern int vk_dump_white;   // vk_dump.c, NX2NAVI_DUMP_WHITE   // NX2NAVI_FLIGHT=1, set once by flight_init before the first present

void flight_init(void);   // prof_start calls it
// perf_present, once per present on the render thread: the interval since the previous present, the render thread's
// blocked time per PERF_* kind in this frame, and the running PERF_CNT_* totals.
void flight_frame(uint64_t frame, uint64_t now_ns, uint64_t interval_ns, const uint64_t blocked_ns[],
                  const volatile int64_t counts[]);

// Texture events of the recorded frames (vk_texture.c), written with each dump as events.tsv: a texture made (with how
// its content started), uploaded, uploaded again, released by the game, taken back after a release, or destroyed. Each
// carries the frame whose counts in frames.tsv it is part of. Any thread: the ring takes no lock. A no-op until
// flight_init turned the recorder on.
enum { FLIGHT_TEX_NEW, FLIGHT_TEX_UP, FLIGHT_TEX_REUP, FLIGHT_TEX_RELEASE, FLIGHT_TEX_TAKEBACK, FLIGHT_TEX_DROP,
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

// gfx_present.c: the present thread blits every presented frame into a host-visible ring of n thumbnails.
void vk_thumbs_enable(int n);            // before the first present; n slots of FLIGHT_THUMB_W x FLIGHT_THUMB_H
void vk_thumbs_frame(uint64_t frame);    // the present thread names the frame of the vk_present_image call that follows
// Copies frame's thumbnail as RGBA when the ring still holds it complete; 0 when it was never written, is still in
// flight or was overwritten. Takes the present lock for one slot copy (about 20 us); any thread.
int vk_thumbs_read(uint64_t frame, uint8_t *rgba);
