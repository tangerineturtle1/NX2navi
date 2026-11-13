// Released textures by age, the part without a GPU: how long a released texture is kept, how many bytes the released
// ones may hold before the size bound drops young ones, and the wall clock that turns a present count into seconds.
// vk_texture.c ("released textures by age", drop_aged and sweep_pressure) does what these say;
// tests/tex_release_check.py runs this file alone.
//
// The game releases a texture when what uses it leaves the view and takes it back when that returns. The cache keeps a
// released texture's image so that the take-back finds it; a texture taken back after its image was dropped is made
// again (created and uploaded, or decoded on the GPU), and the frame that asks for it samples its stand-in. How long
// after the release the take-backs come is what decides how long an image is worth keeping. Two sessions on an RTX
// 5090 (2026-11-12, one at the old Balanced, one at the old Low; vk_texture.c vk_texture_reuse_log) took back, per
// line of 3600 presents: under 2 s 22,000 to 121,000; 2 to 10 s 1 to 126; 10 to 60 s 0 to 156; 1 to 5 min and longer
// none. Balanced kept every release up to its bound of 4 GiB, so its zero past a minute is not a dropped texture's
// absence: nothing released for longer was asked for again. A released texture not taken back within a minute was
// never taken back.
//
// So a released texture goes once it has gone unused for the hold (TRL_HOLD_S by default, NX2NAVI_TEX_RELEASED_HOLD),
// three times the longest gap seen, whether or not the bound is reached: the cache then holds what the last few
// minutes released, not what the bound allows, and it empties within minutes of the last area change. Unused counts
// from the release or from the last time a shader sampled the released image (a finalized slot may show it), whichever
// is later. The hold is the same for a texture uploaded from the game's memory and for an ASTC one the GPU decodes:
// either made again shows its stand-in for a frame, the decode only costs more of the render thread. A texture whose
// content only the GPU made (a render target) cannot be made again at all, it comes back empty: it waits twice the
// hold.
//
// The size bound (released_cap_bytes, a quarter of the video memory budget by default) stays as the safety net it was:
// past it the oldest released textures go even when young. With the hold on, young ones past it are kept while the card
// and the system have room (trl_room), up to TRL_ROOM_TIMES the bound: the old Balanced dropped 31 young ones in a
// burst of area changes at its bound of 4 GiB on a card with 20 GiB free, and each was made again within a minute. The
// hold empties that excess within minutes, so the room it takes is temporary; without the hold (tex_released_hold = 0) the
// bound acts as before.
#pragma once
#include <stdint.h>

enum { TRL_STORAGE, TRL_GPU };   // content made again from the game's memory; content only the GPU made

#define TRL_HOLD_S 180       // the default hold, in seconds
#define TRL_HOLD_MAX_S 900   // the longest hold the setting takes: twice it fits the clock below
#define TRL_ROOM_TIMES 2     // with room, the released textures may hold this many times the bound
#define TRL_ROOM_MARGIN 10   // room: the card at least this many points of its budget under the VRAM pressure

// The hold of a class, in seconds; 0: no hold.
static inline uint32_t trl_hold(uint32_t hold_s, int cls) { return cls == TRL_GPU ? hold_s * 2 : hold_s; }

// Whether a released texture unused for unused_s seconds has passed its hold.
static inline int trl_aged(uint32_t hold_s, int cls, uint32_t unused_s) {
    return hold_s && unused_s >= trl_hold(hold_s, cls);
}

// Whether the card and the system have room for young released textures past the bound: the device-local memory in
// use (the driver's figure, holes counted) is TRL_ROOM_MARGIN points or more under the VRAM pressure's share of the
// budget, and the system's commit has at least twice the free space under which it counts as low (commit_free_mb
// below 0: the commit is not tracked here, which counts as no room).
static inline int trl_room(uint64_t used, uint64_t budget, int pressure_pct, double commit_free_mb,
                           double commit_low_mb) {
    if (!budget || pressure_pct <= TRL_ROOM_MARGIN || commit_free_mb < 0) return 0;
    return used <= budget / 100 * (uint64_t)(pressure_pct - TRL_ROOM_MARGIN) && commit_free_mb >= 2 * commit_low_mb;
}

// The bytes the released textures may hold before the bound drops young ones (drop_stale's keep): the bound, or
// TRL_ROOM_TIMES it while the hold is on and there is room. cap UINT64_MAX: no bound.
static inline uint64_t trl_keep(uint32_t hold_s, uint64_t cap, int room) {
    if (cap == UINT64_MAX || !hold_s || !room) return cap;
    return cap > UINT64_MAX / TRL_ROOM_TIMES ? UINT64_MAX : cap * TRL_ROOM_TIMES;
}

// The wall clock of the presents. The cache stamps its textures with present counts (gfx_counters.frames), and the
// presents per second change with the scene and the menus: a hold counted in presents would be shorter in wall time
// after a fast stretch. The clock notes the present count once a second or so (tick) and answers how many seconds ago
// a present was, rounded down to the tick after it: a texture is taken as at most that old, never older.
#define TRL_CLOCK_N 2048     // ticks kept: at least 2048 s, more than twice TRL_HOLD_MAX_S
typedef struct {
    uint64_t frame[TRL_CLOCK_N];   // the present count at each tick
    uint32_t sec[TRL_CLOCK_N];     // the tick's second of the run
    uint32_t n;                    // ticks so far
} TrlClock;

static inline void trl_clock_tick(TrlClock *c, uint64_t frame, uint32_t sec) {
    c->frame[c->n % TRL_CLOCK_N] = frame, c->sec[c->n % TRL_CLOCK_N] = sec;
    c->n++;
}

// Seconds from present `frame` to now_sec: from the first tick at or after it. A present after the last tick is 0 s
// old; one before the oldest tick kept is as old as that tick, at least TRL_CLOCK_N - 1 seconds.
static inline uint32_t trl_clock_age(const TrlClock *c, uint64_t frame, uint32_t now_sec) {
    uint32_t kept = c->n < TRL_CLOCK_N ? c->n : TRL_CLOCK_N, first = c->n - kept;
    if (!kept || c->frame[(c->n - 1) % TRL_CLOCK_N] < frame) return 0;
    uint32_t lo = first, hi = c->n - 1;   // the first tick with frame >= frame lies in [lo, hi]
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (c->frame[mid % TRL_CLOCK_N] >= frame) hi = mid;
        else lo = mid + 1;
    }
    uint32_t s = c->sec[lo % TRL_CLOCK_N];
    return now_sec > s ? now_sec - s : 0;
}
