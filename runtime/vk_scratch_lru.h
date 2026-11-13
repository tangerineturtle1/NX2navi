// Which entry of a small cache of scratch images serves a request for an image of one size and format
// (vk_exec_draw.c fallback_scratch). Header only, so that tests/scratch_lru_check.c checks it without a device.
//
// The cache held its first SCRATCH_N images for the whole run and refused every other size: each scene size the
// stand-in of the game's upscaler blits through takes one, and after a run of window, resolution or DLSS mode
// changes the blit of a new size found no image and was left out. Now a request that matches no live entry takes a
// free one, else the one used longest ago, which the caller frees (deferred: a submission may still name it) before
// it makes the new image there. The images in use stay; one used in no frame for a while is what goes.
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t w, h, f;   // the image's size and format (VkFormat)
    uint64_t used;      // the frame of its last request
    int live;           // holds an image
} ScrEntry;

// The entry for (w, h, f) among e[0..n-1]: a live match (*hit 1), else a free entry, else the live one used longest ago
// (*evict 1: its image is to be freed before the slot is reused). A hit's used becomes now; the caller fills a new
// entry once its image is made.
static inline int scr_pick(ScrEntry *e, int n, uint32_t w, uint32_t h, uint32_t f, uint64_t now, int *hit, int *evict) {
    int free_at = -1, lru = -1;
    *hit = *evict = 0;
    for (int i = 0; i < n; i++) {
        if (!e[i].live) {
            if (free_at < 0) free_at = i;
            continue;
        }
        if (e[i].w == w && e[i].h == h && e[i].f == f) {
            e[i].used = now, *hit = 1;
            return i;
        }
        if (lru < 0 || e[i].used < e[lru].used) lru = i;
    }
    if (free_at >= 0) return free_at;
    *evict = 1;
    return lru;
}
