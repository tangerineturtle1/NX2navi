// The part of a frame that an address-space walk of NX2NAVI_MEM_CENSUS overlapped, charged to the census (perf.c
// ft_frame). vk_memory.c census_main walks on a thread of its own, but the walk holds the process's address-space lock,
// which the replaying thread's write-watch queries (vk_memory.c vk_mem_mirror_begin) and every page fault take too. In
// a session of 2026-11-13, 63 of the 66 hitches the stutter map charged to the replay sat beside such a walk, with a
// 20-34 ms write-watch query in them; the presenting thread's 600-frame memory report (vk_memory.c vk_mem_census) ran
// at the same frames, and that log cannot tell which of the two held them up. The charge assumes the walk did, which a
// play-test with mem_census = 0 checks: the time the frame overlapped the walk is taken out of the frame's replay part
// first, then out of its unattributed part, never more than the overlap and never so much that the census part grows
// past the frame. Apart from perf.c so that tests/census_charge_check.py builds it alone.
#pragma once
#include <stdint.h>

// A walk takes 100-260 ms, so one seen running for longer than this is a torn read: perf.c ft_frame loaded the last
// walk's start, then the 0 that perf.c perf_census_walk(1) stores as the end before it stores the next start. Taken as
// running, that walk would cover the whole frame.
#define CENSUS_WALK_MAX_NS 2000000000ull

// The frame is [now - d, now]; the walk [w0, w1]: w0 0 when no walk was seen, w1 0 (or older than w0, the end of the
// walk before) while it runs, then reaching now. Returns the time moved into *census.
static inline uint64_t census_charge(uint64_t now, uint64_t d, uint64_t w0, uint64_t w1, uint64_t *replay,
                                     uint64_t *other, uint64_t *census) {
    if (!w0 || !d || d > now) return 0;
    if (w1 && w1 < w0) w1 = 0;
    if (!w1 && now - w0 > CENSUS_WALK_MAX_NS) return 0;
    uint64_t lo = now - d > w0 ? now - d : w0, hi = w1 && w1 < now ? w1 : now;
    if (hi <= lo) return 0;
    uint64_t ov = hi - lo, room = d > *census ? d - *census : 0;
    if (ov > room) ov = room;
    uint64_t r = ov < *replay ? ov : *replay;
    uint64_t o = ov - r < *other ? ov - r : *other;
    *replay -= r, *other -= o, *census += r + o;
    return r + o;
}
