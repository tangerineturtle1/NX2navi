// When the freezing of idle guest regions waits, the part without the OS: vk_memory.c freeze_pass makes the freezes
// (memory.c mem_freeze) and asks these functions whether a pass may run; tests/freeze_hold_check.py runs this file alone.
//
// A freeze is a VirtualQuery and a VirtualProtect of 2 MiB, some microseconds as a rule, and it only saves later
// write-watch queries (about a microsecond per MiB per submission), so it can wait. Around an area load it is slow: in
// the second after it, while the loaders write and the driver imports and locks new memory, freezes took 4 to 32 ms.
// Two rules hold the passes back:
// - A freeze of FRZ_SLOW_NS or more holds them for FRZ_BACKOFF_NS, doubled for each further slow one up to
//   FRZ_BACKOFF_MAX_NS. The doubling starts over once quiet_ns pass without a slow freeze (quiet_ns 0: at the first
//   fast freeze, the rule of 0.10.1, under which a slow freeze came every 250 ms for seconds after a load, fast ones
//   between them).
// - While the texture pool makes an area's textures, and for load_hold_ns after that or after an area's load, no pass
//   runs (load_hold_ns 0: no such hold).
#pragma once
#include <stdint.h>

#define FRZ_SLOW_NS 2000000ull
#define FRZ_BACKOFF_NS 250000000ull
#define FRZ_BACKOFF_MAX_NS 2000000000ull

typedef struct {
    uint64_t hold_until;   // the backoff holds the passes until then
    uint64_t backoff;      // the last hold's length; 0: none since the doubling started over
    uint64_t last_slow;    // when the last slow freeze ended; 0: none yet
    uint64_t load_until;   // an area load holds the passes until then
} FrzHold;

// A freeze ended at t1 after taking took ns.
static inline void frz_freeze_done(FrzHold *h, uint64_t t1, uint64_t took, uint64_t quiet_ns) {
    if (took < FRZ_SLOW_NS) {
        if (!quiet_ns) h->backoff = 0;
        return;
    }
    if (quiet_ns && h->last_slow && t1 - h->last_slow >= quiet_ns) h->backoff = 0;
    h->backoff = h->backoff ? h->backoff * 2 : FRZ_BACKOFF_NS;
    if (h->backoff > FRZ_BACKOFF_MAX_NS) h->backoff = FRZ_BACKOFF_MAX_NS;
    h->hold_until = t1 + h->backoff;
    h->last_slow = t1;
}

// Whether an area load holds the passes at now: loading, the texture pool is making an area's textures; area_at, when
// the last area load began (0: none).
static inline int frz_load_hold(FrzHold *h, uint64_t now, uint64_t load_hold_ns, int loading, uint64_t area_at) {
    if (!load_hold_ns) return 0;
    if (loading) {
        h->load_until = now + load_hold_ns;
        return 1;
    }
    if (area_at && area_at <= now && area_at + load_hold_ns > h->load_until) h->load_until = area_at + load_hold_ns;
    return now < h->load_until;
}

// Whether a pass may run at now (the backoff, then the load).
static inline int frz_may_run(FrzHold *h, uint64_t now, uint64_t load_hold_ns, int loading, uint64_t area_at) {
    int load = frz_load_hold(h, now, load_hold_ns, loading, area_at);
    return now >= h->hold_until && !load;
}
