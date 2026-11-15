// The mesh fallback's depth hold (vk_exec_draw.c "a list's depth-only draw is held"): which lists' colour draws were
// skipped lately. Header only, so that tests/mesh_fb_hold_check.c checks the table without a device.
//
// A list is its indirect record's address in the game's memory (below 2^39). One word per slot: the address above
// MFB_HOLD_BITS bits of the present count. The colour draw stamps its list with the present it was skipped in and
// clears the stamp when it is drawn; the list is held while the stamp is at most MFB_HOLD_PRESENTS presents old.
// Read and written by the replay's threads without a lock: a lost update costs a list one frame of the hold, and two
// lists in one slot (MFB_HOLD_SLOTS of them, by a hash of the address) hold only the one stamped last.
#pragma once
#include <stdint.h>

#define MFB_HOLD_SLOTS 1024
#define MFB_HOLD_PRESENTS 2
#define MFB_HOLD_BITS 24
#define MFB_HOLD_MASK ((1ull << MFB_HOLD_BITS) - 1)

static inline uint32_t mfb_hold_index(uint64_t list) { return (uint32_t)((list >> 2) * 0x9e3779b1u) >> 22; }

// The list's colour draw was skipped for its modules or pipeline (waits 1) at this present, or was drawn (0).
static inline void mfb_hold_mark(volatile uint64_t *tab, uint64_t list, int waits, uint64_t present) {
    volatile uint64_t *slot = &tab[mfb_hold_index(list)];
    if (waits) __atomic_store_n(slot, list << MFB_HOLD_BITS | (present & MFB_HOLD_MASK), __ATOMIC_RELAXED);
    else if (__atomic_load_n(slot, __ATOMIC_RELAXED) >> MFB_HOLD_BITS == list) __atomic_store_n(slot, 0, __ATOMIC_RELAXED);
}

// Whether the list's depth-only draw is held at this present.
static inline int mfb_hold_test(const volatile uint64_t *tab, uint64_t list, uint64_t present) {
    uint64_t w = __atomic_load_n(&tab[mfb_hold_index(list)], __ATOMIC_RELAXED);
    if (!w || w >> MFB_HOLD_BITS != list) return 0;
    return ((present - w) & MFB_HOLD_MASK) <= MFB_HOLD_PRESENTS;
}
