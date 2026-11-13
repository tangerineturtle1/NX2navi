// runtime/vk_scratch_lru.h: which entry of the stand-in's scratch image cache serves a request (vk_exec_draw.c
// fallback_scratch). Built and run by tests/scratch_lru_check.py; prints each failed check and exits 1 on any.
// The old cache kept its first eight sizes and refused the ninth: the sizes below are those a run walks through when
// the window and the DLSS mode change a few times.
#include <stdio.h>
#include <string.h>

#include "vk_scratch_lru.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define N 8
static ScrEntry e[N];

// A request as fallback_scratch makes it: a hit returns its entry, a miss fills the entry it was given (after freeing
// the evicted image). Returns the entry; *was_hit, *was_evict as scr_pick said.
static int request(uint32_t w, uint32_t h, uint32_t f, uint64_t frame, int *was_hit, int *was_evict) {
    int hit, evict, i = scr_pick(e, N, w, h, f, frame, &hit, &evict);
    if (!hit) e[i] = (ScrEntry){w, h, f, frame, 1};
    if (was_hit) *was_hit = hit;
    if (was_evict) *was_evict = evict;
    return i;
}

int main(void) {
    int hit, evict;
    // ---- empty: free entries in order, nothing evicted
    for (int k = 0; k < N; k++) {
        int i = request(1920 + (uint32_t)k, 1080, 37, (uint64_t)k + 1, &hit, &evict);
        CHECK(i == k && !hit && !evict, "size %d took entry %d (hit %d, evict %d)", k, i, hit, evict);
    }
    // ---- a match is a hit and refreshes its frame
    int i = request(1923, 1080, 37, 100, &hit, &evict);
    CHECK(i == 3 && hit && !evict && e[3].used == 100, "the match was entry %d (hit %d, evict %d, used %llu)", i, hit,
          evict, (unsigned long long)e[3].used);
    // same size, another format: not a match
    i = request(1923, 1080, 43, 101, &hit, &evict);
    CHECK(!hit && evict, "another format matched (hit %d, evict %d)", hit, evict);
    // ---- the ninth size: the one used longest ago goes (entry 0, frame 1), not the full cache refusing it
    CHECK(i == 0, "the other format evicted entry %d, not the oldest (0)", i);
    i = request(5120, 2142, 37, 102, &hit, &evict);
    CHECK(i == 1 && evict, "the next new size evicted entry %d (evict %d), not entry 1", i, evict);
    // the refreshed entry 3 (frame 100) outlives the untouched ones
    for (int k = 0; k < 4; k++) request(800 + (uint32_t)k, 600, 37, 103 + (uint64_t)k, &hit, &evict);
    CHECK(e[3].live && e[3].w == 1923 && e[3].f == 37, "entry 3, used at frame 100, was evicted before older ones");
    // ---- two images in one frame never evict each other while older entries remain (g8 then g8s of one dispatch)
    int a = request(2560, 1440, 37, 200, &hit, &evict);
    int b = request(2560, 1440, 43, 200, &hit, &evict);
    CHECK(a != b && e[a].live && e[a].w == 2560 && e[a].f == 37, "the second image of a frame took the first's entry");
    CHECK(request(2560, 1440, 37, 201, &hit, NULL) == a && hit, "the first image of the pair was not kept");
    // ---- a freed entry (an image that failed to be made stays not live) is taken before any eviction
    e[5].live = 0;
    i = request(640, 360, 37, 300, &hit, &evict);
    CHECK(i == 5 && !hit && !evict, "a free entry was passed over (entry %d, evict %d)", i, evict);
    if (fails) return printf("scratch_lru_check: %d checks failed\n", fails), 1;
    printf("scratch_lru_check: all checks passed\n");
    return 0;
}
