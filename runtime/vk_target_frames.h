// Target history: the presented frame (gfx_counters.frames + 1; 0: never) each render target subresource was last
// rendered into, the one before, and in how many frames. vk_exec_draw.c prepare_draw_ex lets a draw into a target the
// game does not render frame after frame (a one-shot draw: a volume or lookup table generated once) wait for its
// shaders and pipeline instead of skipping it. Own header so tests/target_frames_check.c checks the rules without
// Vulkan; vk_exec.c is the one runtime file that includes it (the table is static).
// Per subresource, not per image: a volume may be generated a few slices per frame.
//
// Recent means within TFR_RECENT frames, not the frame before alone: the game's output images are a ring of three, its
// probe and shadow layers are refreshed in turn every second to fourth frame, and the mips of its small effect buffers
// alternate between images. Sets of TFR_WAYS, the oldest entry replaced: hundreds of subresources are rendered a
// frame, and in a direct-mapped table of 1024 a fifth of the scene passes and most of the probe layers read as never
// rendered because another subresource had taken their slot. Each misread made a draw whose pipeline was still
// building wait for it, so a new area was entered one pipeline at a time.
//
// Steady targets (NX2NAVI_PIPE_SKIP_STEADY, default 1; 0 = as before). Recent alone called every pass one-shot in the
// first frame after the game had drawn something else for a while (a loading screen between two areas, a full-screen
// menu): the scene's own targets, the probe layers and the half-resolution buffers had last been rendered hundreds of
// frames before, so each draw with a new pipeline waited on the replay thread, and a mesh draw, whose pipeline has no
// fast link, for its whole compile. One player's session shows it three times at area changes: frames of 954, 1015
// and 684 ms, six mesh draws waiting 52 to 660 ms each in passes whose targets had been rendered in thousands of
// frames. An entry therefore also counts the frames its subresource was rendered in, for as long as the image is the
// texture at the same storage (its owner, the storage GPU address the texture cache keys the image by: an image
// handed on to a texture elsewhere starts again at none). The guest's texture object does not serve as the owner:
// the game initializes its render target objects anew every frame at the same storage, so a count kept per object
// never passed one. From TFR_STEADY frames on the target is one the game draws again and again, and its draws are
// skipped while a pipeline builds, as in any other frame, however long ago the last one was. A target generated once
// never gets there; one generated again at every area load would after TFR_STEADY loads, by when its pipelines are
// recorded and prebuilt at the start.
#pragma once

#include <stdint.h>

#define TFR_SETS 1024
#define TFR_WAYS 8
#define TFR_RECENT 4
#define TFR_STEADY 8

// One render target of a pass: the image handle as a number, the subresource, the image's owner (0 unknown).
typedef struct { uint64_t image; uint32_t level, layer; uint64_t owner; } TfrTarget;
typedef struct { uint64_t image; uint32_t level, layer, n; uint64_t last, prev, owner; } TargetFrames;
static TargetFrames tfr[TFR_SETS * TFR_WAYS];

// The target's entry, or with make the entry it takes (the set's oldest, an empty one first); NULL without make
// when it has none.
static inline TargetFrames *tfr_find(const TfrTarget *t, int make) {
    uint64_t h = (t->image ^ (uint64_t)t->level << 56 ^ (uint64_t)t->layer << 40) * 0x9E3779B97F4A7C15ull;
    TargetFrames *set = &tfr[(h >> 54) * TFR_WAYS], *oldest = set;
    for (int i = 0; i < TFR_WAYS; i++) {
        if (set[i].image == t->image && set[i].level == t->level && set[i].layer == t->layer) return &set[i];
        if (set[i].last < oldest->last) oldest = &set[i];
    }
    if (!make) return NULL;
    *oldest = (TargetFrames){.image = t->image, .level = t->level, .layer = t->layer};
    return oldest;
}

// The target is rendered in frame stamp.
static inline void tfr_note(const TfrTarget *t, uint64_t stamp) {
    TargetFrames *e = tfr_find(t, 1);
    if (e->owner != t->owner) e->owner = t->owner, e->n = 0;   // another texture has the image now: its own count
    if (e->last != stamp) e->prev = e->last, e->last = stamp, e->n += e->n < UINT32_MAX;
}

// The frame before stamp that last rendered the target, 0 for none.
static inline uint64_t tfr_before(const TfrTarget *t, uint64_t stamp) {
    const TargetFrames *e = tfr_find(t, 0);
    return !e ? 0 : e->last == stamp ? e->prev : e->last;
}

static inline int tfr_recent(const TfrTarget *t, uint64_t stamp) {
    uint64_t before = tfr_before(t, stamp);
    return before && stamp - before <= TFR_RECENT;
}

// No frame before stamp rendered the target.
static inline int tfr_first(const TfrTarget *t, uint64_t stamp) { return !tfr_before(t, stamp); }

// TFR_STEADY frames or more before stamp rendered the target under the owner it has now.
static inline int tfr_steady(const TfrTarget *t, uint64_t stamp) {
    const TargetFrames *e = tfr_find(t, 0);
    return e && e->owner == t->owner && e->n - (e->last == stamp) >= TFR_STEADY;
}

// Whether the pass with the n_color colour targets c and the depth target d (NULL: none) is one the game renders
// frame after frame, so that a draw into it may be skipped in frame stamp and comes back in the next.
// any 0: every target is recent (and the pass has one). any 1: a colour target is, or the depth target is while no
// colour target is rendered for the first time: the game's item icon capture draws into a new colour target with the
// per-frame depth buffer, once, and its draws skipped there stay missing from the icon until the item is captured
// again. steady 1: a steady target counts as a recent one; *by_steady is set when only that made the answer 1.
static inline int tfr_pass_recent(const TfrTarget *c, int n_color, const TfrTarget *d, uint64_t stamp, int any,
                                  int steady, int *by_steady) {
    int n = 0, color[2] = {0, 0}, depth[2] = {0, 0}, color_first = 0, all[2] = {1, 1};
    for (int i = 0; i <= n_color; i++) {
        const TfrTarget *t = i < n_color ? &c[i] : d;
        if (!t || !t->image) continue;
        n++;
        int recent = tfr_recent(t, stamp), kept = recent || (steady && tfr_steady(t, stamp));
        if (recent) *(i < n_color ? &color[0] : &depth[0]) = 1;
        else all[0] = 0;
        if (kept) *(i < n_color ? &color[1] : &depth[1]) = 1;
        else all[1] = 0;
        if (i < n_color && tfr_first(t, stamp)) color_first = 1;   // one never rendered is neither recent nor steady
    }
    int r[2];   // without the steady rule, and with it
    for (int k = 0; k < 2; k++) r[k] = any ? color[k] || (depth[k] && !color_first) : n > 0 && all[k];
    if (by_steady) *by_steady = r[1] && !r[0];
    return r[1];
}
