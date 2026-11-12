// Camera cuts for FSR 3.1 frame generation (vk_fg.cpp). DLSS starts a new history on a cut, another scene size or
// another target, and vk_dlaa.c vk_dlaa_fg_inputs hands that out with the frame's depth and motion vectors. FSR 3.1
// takes it only in the generation callback (ffxDispatchDescFrameGeneration.reset), which its swapchain calls later,
// on its own present thread, for the frame id the configuration gave. So vk_fg_frame notes each frame's cut here
// under its id, and the callback looks it up. Without it every generated frame of a cut was blended from two unrelated
// pictures: an area load or a cutscene's cut flashed a frame of both.
//
// A ring of FGR_SLOTS frames; each slot holds the frame id and its cut as one atomic word, so a slot being rewritten
// never pairs an old id with a new frame's answer. An id the ring no longer holds (the callback fell that far behind,
// or the slot was never written) counts as a cut: a lost history costs one plain frame, a wrong one blends two.
// Own header so tests/fg_reset_check.c checks it without a GPU.
#pragma once
#include <stdint.h>

#define FGR_SLOTS 32

typedef struct {
    uint64_t slot[FGR_SLOTS];   // (frame id << 1) | cut, written by vk_fg_frame; 0 never matches (ids start at 1)
} FgrRing;

// Frame ids above this do not fit beside the cut bit; they always count as a cut.
#define FGR_MAX_ID (UINT64_MAX >> 1)

static inline void fgr_note(FgrRing *r, uint64_t id, int cut) {
    if (id && id <= FGR_MAX_ID) __atomic_store_n(&r->slot[id % FGR_SLOTS], id << 1 | (cut != 0), __ATOMIC_RELEASE);
}

static inline int fgr_cut(const FgrRing *r, uint64_t id) {
    if (!id || id > FGR_MAX_ID) return 1;
    uint64_t v = __atomic_load_n(&r->slot[id % FGR_SLOTS], __ATOMIC_ACQUIRE);
    return v >> 1 != id || (v & 1);
}
