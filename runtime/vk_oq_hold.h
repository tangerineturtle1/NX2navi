// The count a samples-passed report writes when the replay could not count all of its draws (vk_exec_xfer.c oq_cb,
// NX2NAVI_OQ_HOLD). Header only, so that tests/oq_hold_check.c checks it without a device.
//
// A draw that finds every slot of the occlusion query ring still waiting for the completion thread is left uncounted,
// and the report after it says only what the other draws counted, at least 1. The game reads these counts as how much
// of an object shows, and a count of 1 where an object's proxy covers thousands of samples tells it the object is all
// but hidden for that frame: whatever it scales by that count (a light's glare, an object's fade) drops for one frame
// and comes back, a flicker. The ring fills while the GPU or the completion thread falls behind the replay, in a hitch
// at a few hundred frames a second. With the hold such a report writes the larger of what was counted and what the
// last complete report into the same record said: the previous frame's answer for that object. A record with no
// complete report remembered keeps the old rule.
#pragma once
#include <stdint.h>

#define OQH_N 1024   // records remembered, direct-mapped by address; a collision forgets the older one

typedef struct {
    uint64_t addr[OQH_N];
    uint64_t count[OQH_N];
} OqHold;

static inline uint32_t oqh_slot(uint64_t addr) {
    uint64_t h = (addr >> 4) * 0x9E3779B97F4A7C15ull;   // records are 16 bytes apart at least
    return (uint32_t)(h >> 54) & (OQH_N - 1);
}

// A complete report of count into the record at addr.
static inline void oqh_note(OqHold *t, uint64_t addr, uint64_t count) {
    if (!addr) return;
    uint32_t i = oqh_slot(addr);
    t->addr[i] = addr, t->count[i] = count;
}

// The count to report into addr for a report that missed draws, of which the others counted `counted`.
static inline uint64_t oqh_lost(const OqHold *t, uint64_t addr, uint64_t counted) {
    uint64_t c = counted ? counted : 1;
    if (!addr) return c;
    uint32_t i = oqh_slot(addr);
    if (t->addr[i] == addr && t->count[i] > c) c = t->count[i];
    return c;
}
