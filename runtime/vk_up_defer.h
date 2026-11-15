// What an upscaler lets go of while the GPU may still use it (vk_dlaa.c, vk_ffx.cpp, vk_ffx4.cpp): a DLSS feature, an
// FSR upscaling context, or an image frame generation can still read at a present. Each owner keeps a list and
// releases an entry with its own callback, on the thread that makes the upscaler's other calls (vk_dlaa.c's pass
// hooks), once the GPU timeline has reached the value of the last submission that named it (fence; 0: none did) and
// the present count has reached presents (0: no such gate). Nothing here waits: the owner checks at its own calls
// (upd_take), and only a full list or a refused creation falls back to a wait, in the owner.
// A header of its own so that tests/up_defer_check.c checks it without a GPU; vk_ffx.cpp and vk_ffx4.cpp include it as
// C++.
#pragma once
#include <stdint.h>

#define UPD_SLOTS 48

typedef struct {
    uint64_t fence;      // timeline value of the last submission that named it (vk_exec_recording_fence), 0 none
    uint64_t presents;   // released once the present count has reached this, 0 at once
    int kind;            // the owner's: what handle and aux are
    void *handle, *aux;
} UpdEntry;

typedef struct {
    UpdEntry e[UPD_SLOTS];
    int n;
    uint64_t put, released;   // so far
} UpdList;

typedef void (*UpdRelease)(const UpdEntry *e, void *arg);

// 1 when queued, or nothing to queue (a null handle: a half-made object's missing part); 0 when the list is full, and
// the caller makes room with its fallback wait first.
static inline int upd_put(UpdList *l, int kind, void *handle, void *aux, uint64_t fence, uint64_t presents) {
    if (!handle) return 1;
    if (l->n >= UPD_SLOTS) return 0;
    UpdEntry *e = &l->e[l->n++];   // field by field, which C++ takes as C does
    e->fence = fence, e->presents = presents, e->kind = kind, e->handle = handle, e->aux = aux;
    l->put++;
    return 1;
}

static inline int upd_ready(const UpdEntry *e, uint64_t done, uint64_t presents) {
    return e->fence <= done && e->presents <= presents;
}

// Releases, in the order they were put, the entries whose fence is done and whose present gate is passed; the others
// stay, in their order. The ready ones leave the list before any callback runs, so a callback may put new entries,
// which wait for the next take. Returns how many were released.
static inline int upd_take(UpdList *l, uint64_t done, uint64_t presents, UpdRelease release, void *arg) {
    UpdEntry go[UPD_SLOTS];
    int k = 0, ng = 0;
    for (int i = 0; i < l->n; i++) {
        if (upd_ready(&l->e[i], done, presents)) go[ng++] = l->e[i];
        else l->e[k++] = l->e[i];
    }
    l->n = k;
    for (int i = 0; i < ng; i++) release(&go[i], arg);
    l->released += (uint64_t)ng;
    return ng;
}

// Every entry whatever its gates: the device is idle and nothing reads them any more (teardown).
static inline int upd_take_all(UpdList *l, UpdRelease release, void *arg) {
    return upd_take(l, UINT64_MAX, UINT64_MAX, release, arg);
}

// The highest fence among the entries (0: none named by a submission), for a fallback wait.
static inline uint64_t upd_max_fence(const UpdList *l) {
    uint64_t m = 0;
    for (int i = 0; i < l->n; i++)
        if (l->e[i].fence > m) m = l->e[i].fence;
    return m;
}

// The entries of kind still waiting, or of every kind for a kind below 0.
static inline int upd_count(const UpdList *l, int kind) {
    int n = 0;
    for (int i = 0; i < l->n; i++) n += kind < 0 || l->e[i].kind == kind;
    return n;
}
