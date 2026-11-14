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
// the longest gap seen, whether or not the bound is reached: the cache then holds what the last minute released, not
// what the bound allows, and it empties within a minute or two of the last area change. Unused counts
// from the release or from the last time a shader sampled the released image (a finalized slot may show it), whichever
// is later. The hold is the same for a texture uploaded from the game's memory and for an ASTC one the GPU decodes:
// either made again shows its stand-in for a frame, the decode only costs more of the render thread. A texture whose
// content only the GPU made (a render target) cannot be made again at all, it comes back empty: it waits twice the
// hold.
//
// The size bound (trl_cap_mb: a thirty-second of the video memory budget, TRL_CAP_MIN_MB to TRL_CAP_MAX_MB) is what
// decides how much video memory the cache costs: past it the oldest released textures go even when young, and with the
// hold on, young ones past it are kept while the card and the system have room (trl_room), up to TRL_ROOM_TIMES the
// bound. Until 0.11.0 the hold was 180 s and the bound a quarter of the budget (4 GiB on a 32 GiB card, 8 with room).
// A run through Hyrule Field showed what that bought (2026-11-14, RTX 5090, 5120x2160 with DLSS Performance, about
// three minutes of running; the game keeps 4.8 GiB of textures live there and lets go of and loads about 190 a
// second): the released textures held 5.3 GiB on average and 6.4 at most, the video memory in use went from 11.1 to
// 17.9 GiB, and of what they held the game took back 1,494 MiB within 2 to 10 s of the release, 171 MiB within 10 to
// 60 s and 10 MiB later, beside 33,000 textures it loaded anew in that time. The memory does not come back when the
// cache shrinks either: a block of the allocator goes back to the driver only when its last allocation went, so with
// a hold of 20 s in the same run the released textures fell to 0.5 GiB while the free space inside the blocks rose to
// 4.9 GiB, and the driver's figure stayed at 15.8. What the cache may hold at its fullest is therefore what the
// session costs for good. With a bound of 1 GiB (2 with room) the same run held 13.4 GiB on average and 14.8 at most,
// 3.3 GiB less, made 1,170 MiB of textures again over the run and kept its frame rate (78 against 75 fps, within what
// two runs differ by).
#pragma once
#include <stdint.h>

enum { TRL_STORAGE, TRL_GPU };   // content made again from the game's memory; content only the GPU made

#define TRL_HOLD_S 60        // the default hold, in seconds
#define TRL_HOLD_MAX_S 900   // the longest hold the setting takes: twice it fits the clock below
#define TRL_ROOM_TIMES 2     // with room, the released textures may hold this many times the bound
#define TRL_ROOM_MARGIN 10   // room: the card at least this many points of its budget under the VRAM pressure
#define TRL_CAP_SHARE 32     // the default bound: this share of the video memory budget,
#define TRL_CAP_MIN_MB 384   // at least this much (the render targets a frame lets go of and takes back the next
                             // frame come to about 70 MiB at 2560x1080; they are not dropped, but they count),
#define TRL_CAP_MAX_MB 1024  // and at most this much
#define TRL_CAP_UNKNOWN_MB 512   // without a budget (no VK_EXT_memory_budget and no heap size)

// The default bound in MiB for a video memory budget in MiB (0: not known).
static inline uint32_t trl_cap_mb(uint64_t budget_mb) {
    if (!budget_mb) return TRL_CAP_UNKNOWN_MB;
    uint64_t mb = budget_mb / TRL_CAP_SHARE;
    return (uint32_t)(mb < TRL_CAP_MIN_MB ? TRL_CAP_MIN_MB : mb > TRL_CAP_MAX_MB ? TRL_CAP_MAX_MB : mb);
}

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

// Past the hard count of released textures (vk_texture.c sweep_hard, RELEASED_HARD) entries go whatever they cost,
// cheapest first by class: one drawn over (its content is gone anyway); one that comes back intact from the game's
// memory, released longer ago than its usual gap before a take-back and not looked up lately (past_gap); another one
// that comes back intact; one whose content only the GPU made, which would come back empty. The last three only when
// nothing sampled them lately (TRL_HC_LATELY, kept).
enum { TRL_HC_OVER, TRL_HC_INTACT_OLD, TRL_HC_INTACT, TRL_HC_GPU, TRL_HC_LATELY, TRL_HC_N };

static inline int trl_hard_class(int overwritten, int sampled_lately, int intact, int past_gap) {
    if (overwritten) return TRL_HC_OVER;
    if (sampled_lately) return TRL_HC_LATELY;
    if (!intact) return TRL_HC_GPU;
    return past_gap ? TRL_HC_INTACT_OLD : TRL_HC_INTACT;
}

// Up to need entries to drop: cls[i] is the class of the i-th released entry in release order, n of them; out gets
// their indices, the cheapest class first and in release order within a class. Returns how many; 0 when every entry
// was sampled lately (the caller then takes the one sampled longest ago).
static inline int trl_hard_pick(const uint8_t *cls, int n, int need, int *out) {
    int k = 0;
    for (int c = 0; c < TRL_HC_LATELY && k < need; c++)
        for (int i = 0; i < n && k < need; i++)
            if (cls[i] == c) out[k++] = i;
    return k;
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
