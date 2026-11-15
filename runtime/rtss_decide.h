// RivaTuner Statistics Server as the frame limiter (rtss_limit.c): the parts that make no system call, so that
// tests/rtss_limit_check.c builds them on any system. rtss_parse reads RTSS's shared memory, rtss_decide decides each
// poll of the worker, and rtss_poll carries that decision out through the calls rtss_limit.c gives it (RtssOps).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// rtss_limit_want's argument: a limit in millihertz (>0), 0 for none, or why RTSS is kept out of it (frame_rate.c
// frame_step_cap_ns). RTSS_UNSET is the want before the first one.
enum {
    RTSS_UNSET = -2,
    RTSS_OUT_SETTING = -1,    // NX2NAVI_RTSS=0, for the whole run
    RTSS_OUT_FRAMEGEN = -3,   // frame generation: RTSS would count the generated frames too
    RTSS_OUT_LOWLAT = -4,     // a driver's low latency (vk_lowlat.c) holds the frames where they start
};

// What keeps RTSS out, for the log.
static inline const char *rtss_out_text(int64_t want) {
    return want == RTSS_OUT_FRAMEGEN ? "frame generation"
           : want == RTSS_OUT_LOWLAT ? "the driver's low latency"
                                     : "NX2NAVI_RTSS=0";
}

// The limiter that holds the frames while RTSS is kept out, for the log.
static inline const char *rtss_out_limiter(int64_t want) {
    return want == RTSS_OUT_FRAMEGEN ? "frame generation and Reflex or the game's limiter"
           : want == RTSS_OUT_LOWLAT ? "the driver's low latency"
                                     : "the game's limiter";
}

// ---- RTSS's shared memory, "RTSSSharedMemoryV2". The header: signature, version, application entry size, offset of
// the application array, its length. An entry: the process id, the executable path (260 bytes), flags, then RTSS's
// last frame rate period for the process: its start and end in ms and the frames in it. The flags' low 16 bits hold
// the API RTSS saw the process present with, a value rather than bits (Vulkan is 0xA), and RTSS_UPDATE_ASKED stays set
// from UpdateProfiles until the process has read its profile again.
#define RTSS_SIGNATURE 0x52545353u   // 'RTSS' as RTSS writes it: the memory holds valid data
#define RTSS_API_MASK 0xFFFFu
#define RTSS_API_VULKAN 0xAu
#define RTSS_UPDATE_ASKED 0x10000000u
enum { RTSS_FLAGS_AT = 4 + 260, RTSS_PERIOD_AT = RTSS_FLAGS_AT + 4, RTSS_ENTRY_MIN = RTSS_PERIOD_AT + 12,
       RTSS_APPS_MAX = 4096 };

typedef struct {
    int running;                 // the signature and a version of 2.0 or later
    int listed;                  // the application list holds this process: RTSS has hooked it
    uint32_t flags;              // the entry's flags
    uint32_t t0, t1, frames;     // the period of this process's entry
} RtssApp;

// Reads the memory of size bytes at p for the process pid; never reads past size.
static inline void rtss_parse(const uint8_t *p, size_t size, uint32_t pid, RtssApp *a) {
    memset(a, 0, sizeof *a);
    if (!p || size < 20) return;
    uint32_t h[5];
    memcpy(h, p, sizeof h);
    a->running = h[0] == RTSS_SIGNATURE && h[1] >= 0x20000;
    for (uint32_t i = 0; a->running && h[2] >= RTSS_ENTRY_MIN && i < h[4] && i < RTSS_APPS_MAX; i++) {
        uint64_t at = (uint64_t)h[3] + (uint64_t)i * h[2];
        if (at + RTSS_ENTRY_MIN > size) break;
        uint32_t id, t[3];
        memcpy(&id, p + at, 4);
        if (id != pid) continue;
        a->listed = 1;
        memcpy(&a->flags, p + at + RTSS_FLAGS_AT, 4);
        memcpy(t, p + at + RTSS_PERIOD_AT, sizeof t);
        a->t0 = t[0], a->t1 = t[1], a->frames = t[2];
        break;
    }
}

// The frame rate RTSS measured for the process over its period; 0 unknown.
static inline double rtss_fps(const RtssApp *a) {
    return a->listed && a->t1 > a->t0 ? 1000.0 * a->frames / (a->t1 - a->t0) : 0;
}

// ---- the worker's decisions
#define RTSS_OVER_PERIODS 3            // fresh periods in a row RTSS's frame rate of the game may stay above the limit
#define RTSS_OVER_SLACK 1.05           // above limit * this, RTSS is taken not to hold the game to it
#define RTSS_FRESH_NS 2500000000ull    // a period is stale after this, or 2.5 times its own length if longer
#define RTSS_HOLD_IDLE_NS 200000000ull   // no host present for this long: RTSS holds nothing (vk_lowlat.c HOLD_IDLE_NS)
#define RTSS_SETTLE_POLLS 2              // polls in a row a want must stay before it acts on the profile
#define RTSS_UNPROVEN_POLLS 60           // polls (30 s) RTSS may hook the game with the limit and not show it holds it

enum {   // out of it for the run
    RTSS_FAIL_NONE,
    RTSS_FAIL_LIBRARY,       // its hooks library did not load
    RTSS_FAIL_READONLY,      // its profile cannot be written
    RTSS_FAIL_NOT_LIMITING,  // its frame count showed the game past the limit
    RTSS_FAIL_UNPROVEN,      // it hooked the game with the limit for RTSS_UNPROVEN_POLLS and never showed it holds it
};
enum { RTSS_DO_NOTHING, RTSS_DO_APPLY, RTSS_DO_PUT_BACK };
enum {
    RTSS_WHY_HOLDS,         // RTSS holds the game to the want
    RTSS_WHY_NOT_LISTED,    // RTSS has the limit but has not hooked the game yet
    RTSS_WHY_OUT,           // kept out (the want says why)
    RTSS_WHY_FAILED,        // RtssState.fail
    RTSS_WHY_NOT_RUNNING,
    RTSS_WHY_REFUSED,       // RTSS refused this limit
    RTSS_WHY_NEWER,         // the want changed while the poll ran: nothing decided, the next poll takes the new one
    RTSS_WHY_UNSET,         // no want yet
    RTSS_WHY_MOVIE,         // a movie plays: the game's pacer holds it at its rate and keeps its slots
    RTSS_WHY_NOT_VULKAN,    // RTSS lists the game but has not seen its Vulkan presents
    RTSS_WHY_UPDATE,        // RTSS has not read the profile the game wrote yet
    RTSS_WHY_NOT_COUNTING,  // RTSS has measured no period of the game's frames lately
    RTSS_WHY_IDLE,          // no picture went out lately (a minimised window, a frame hold): RTSS has none to hold
    RTSS_WHY_SETTLING,      // a new want: it reaches the profile once it stayed RTSS_SETTLE_POLLS polls
    RTSS_WHY_NO_PRESENT,    // no picture went out yet: nothing is written before the first
};

// What the worker keeps between polls.
typedef struct {
    int64_t applied;     // the millihertz the profile holds from this run; -1 untouched
    int64_t refused;     // a limit RTSS refused (out of its range): the game's own limiter keeps that one
    int changed;         // the profile holds a value from this run: put the old one back on exit
    int over;            // fresh periods in a row RTSS's frame rate of the game stayed above the limit
    int fail;            // RTSS_FAIL_*
    int held;            // the poll before stood aside: a fresh period now shows how RTSS holds the game
    uint32_t t1, span;   // the end and length (ms) of the newest period seen
    uint64_t fresh_ns;   // when a new period was last seen; 0 never
    int64_t seen;        // the want of the poll before
    int polls;           // polls in a row with that want
    int unproven;        // polls in a row RTSS hooked the game with a limit in the profile and did not show its hold
} RtssState;
#define RTSS_STATE_INIT {-1, -1, 0, 0, RTSS_FAIL_NONE, 0, 0, 0, 0, RTSS_UNSET, 0, 0}

// One poll's view.
typedef struct {
    int64_t want;        // the want the poll read
    int current;         // the want is still that one when the poll decides
    int hooks;           // RTSS's hooks library: 1 loaded, 0 failed to load, -1 not tried (rtss_wants_hooks)
    int64_t movie_mhz;   // the rate of a movie that plays (rtss_limit_movie), 0 none
    uint64_t now_ns;     // kern_now_ns at the poll
    uint64_t hosted_ns;  // kern_now_ns of the newest host present (gfx_present.c gfx_window_hosted_ns), 0 none yet
    RtssApp app;
} RtssIn;

typedef struct {
    int act;        // RTSS_DO_*
    int64_t mhz;    // the value decided for: the profile's (rtss_target) for a limit, the want for a kept-out one
    int holds;      // RTSS holds the game to it: the game's own limiter stands aside
    int why;        // RTSS_WHY_*
    double fps;     // RTSS's frame rate of the game, when the frame count check gave up on it
} RtssOut;

// What the profile gets for a want w >= 0 (the play limit: frame_rate.c frame_step_cap_ns wants it through a movie
// too) while a movie plays at movie_mhz: w, so that a movie no longer rewrites the profile at its start and end; but no
// limit (0) while the movie runs faster than w (a limit of 30, 40 or 45), where RTSS would hold the movie below its
// rate. The game's pacer holds every movie at its rate and keeps its slots through a late frame (nvn_api_obj.c pace),
// sending the frames that catch up at once. Above the movie's rate RTSS lets them through faster than the movie runs.
// At the movie's own rate (a limit of 60 for the game's 60 fps movies, the default) the profile keeps that rate, and
// RTSS holds the movie beside the pacer for its length: whether RTSS's limiter then spaces out the frames that catch
// up, so that the picture falls behind the sound at each late frame, is not measured. Giving the profile no limit there
// too would cost two profile writes per movie at the default limit.
static inline int64_t rtss_target(int64_t w, int64_t movie_mhz) { return w > 0 && movie_mhz > w ? 0 : w; }

// The profile's FramerateLimit and FramerateLimitDenominator for mhz (>= 0): a whole rate over 1, a fraction over 1000
// where RTSS knows the denominator (has_den), else rounded down to whole frames. Only "display" asks a fraction (59.94
// or 143.856 Hz monitors), and a limit rounded up (60, 144) let the game run above the refresh, so that every 7 to 17
// seconds a frame came that the monitor had no refresh for.
static inline void rtss_limit_value(int64_t mhz, int has_den, uint32_t *limit, uint32_t *den) {
    *den = 1;
    if (mhz % 1000 == 0) *limit = (uint32_t)(mhz / 1000);
    else if (has_den) *limit = (uint32_t)mhz, *den = 1000;
    else *limit = (uint32_t)(mhz / 1000);
}

// The hooks library is loaded only on a poll that may write the profile: a limit is wanted, RTSS is not out of it for
// the run, and it runs. A poll of a kept-out want never loads it; a later limit does.
static inline int rtss_wants_hooks(const RtssState *s, const RtssIn *in) {
    return in->want >= 0 && !s->fail && in->app.running;
}

// The decision of one poll, in this order: no want does nothing; a want acts on the profile only once it stayed
// RTSS_SETTLE_POLLS polls in a row, so a want that flips (a driver's low latency on and off at each swapchain rebuild,
// vk_lowlat.c) does not write and remove the profile at each flip, while the game's limiter takes a new want at once
// (rtss_limit_active). A settled kept-out want puts back what this run wrote (once the hooks are loaded); a failure
// lasts the run; RTSS not running leaves the profile for its return; a library that did not load fails the run;
// nothing is written before the first picture went out; a refused limit stays with the game; a settled new limit
// (rtss_target) goes into the profile. The game's limiter stands aside only once RTSS is seen to hold the game: the
// profile held the limit at the poll before (RTSS reads it after the write), RTSS lists the game with its Vulkan
// presents, has read the profile (RTSS_UPDATE_ASKED clear) and measured a period of the game's frames lately (a fresh
// one: its end, dwTime1, moved within RTSS_FRESH_NS, or within 2.5 times the period's length if that is longer), no
// movie plays (rtss_target) and a picture went out within RTSS_HOLD_IDLE_NS. An RTSS that lists the game with a limit
// in its profile while pictures go out, but does not show its hold (another API's presents, the profile never read, no
// period measured), may hold the frames beside the game's limiter: after RTSS_UNPROVEN_POLLS such polls in a row the
// profile goes back for the run.
// Each fresh period after a poll that stood aside is checked against the limit: more than RTSS_OVER_SLACK times it
// for RTSS_OVER_PERIODS periods in a row puts the profile back for the run; a stale period counts neither way. After
// the exit put the profile back (rtss_limit_restore) no poll decides: rtss_limit.c worker skips it under the lock.
static inline RtssOut rtss_decide(RtssState *s, const RtssIn *in) {
    int64_t w = in->want;
    RtssOut o = {RTSS_DO_NOTHING, w, 0, RTSS_WHY_NOT_LISTED, 0};
    if (w == RTSS_UNSET) return o.why = RTSS_WHY_UNSET, o;
    const RtssApp *a = &in->app;
    int fresh = a->listed && a->t1 > a->t0 && a->frames && a->t1 != s->t1;   // a period RTSS measured since
    if (fresh) s->t1 = a->t1, s->span = a->t1 - a->t0, s->fresh_ns = in->now_ns;
    int held = s->held, unproven = s->unproven;
    s->held = s->unproven = 0;   // until this poll stands aside again, or counts as unproven again
    s->polls = w == s->seen ? s->polls + 1 : 1;
    s->seen = w;
    int settled = s->polls >= RTSS_SETTLE_POLLS;
    if (w < 0) {
        if (settled && in->hooks == 1 && s->changed) o.act = RTSS_DO_PUT_BACK;
        s->over = 0;
        return o.why = RTSS_WHY_OUT, o;
    }
    if (s->fail) return o.why = RTSS_WHY_FAILED, o;
    if (!a->running) return o.why = RTSS_WHY_NOT_RUNNING, o;
    if (in->hooks == 0) return s->fail = RTSS_FAIL_LIBRARY, o.why = RTSS_WHY_FAILED, o;
    if (!in->hosted_ns) return o.why = RTSS_WHY_NO_PRESENT, o;
    int64_t t = o.mhz = rtss_target(w, in->movie_mhz);
    if (t == s->refused) return o.why = RTSS_WHY_REFUSED, o;
    if (t != s->applied && !settled) return o.why = RTSS_WHY_SETTLING, o;
    if (t != s->applied) {   // RTSS reads it after the write: a later poll sees whether it holds
        o.act = RTSS_DO_APPLY;
        o.why = in->movie_mhz ? RTSS_WHY_MOVIE : a->listed ? RTSS_WHY_UPDATE : RTSS_WHY_NOT_LISTED;
        s->over = 0;
        return o;
    }
    if (!in->current) return o.why = RTSS_WHY_NEWER, o;   // these frames are not the new want's
    uint64_t idle = in->now_ns > in->hosted_ns ? in->now_ns - in->hosted_ns : 0;
    int shown = idle < RTSS_HOLD_IDLE_NS;   // a picture went out lately (one went out: RTSS_WHY_NO_PRESENT above)
    uint64_t stale = (uint64_t)s->span * 2500000ull > RTSS_FRESH_NS ? (uint64_t)s->span * 2500000ull : RTSS_FRESH_NS;
    int unseen = !a->listed                                       ? RTSS_WHY_NOT_LISTED
                 : (a->flags & RTSS_API_MASK) != RTSS_API_VULKAN   ? RTSS_WHY_NOT_VULKAN
                 : a->flags & RTSS_UPDATE_ASKED                    ? RTSS_WHY_UPDATE
                 : !s->fresh_ns || in->now_ns - s->fresh_ns > stale ? RTSS_WHY_NOT_COUNTING
                                                                   : RTSS_WHY_HOLDS;
    if (unseen != RTSS_WHY_HOLDS) {   // hooked with a limit while pictures go out, RTSS holds them beside the pacer
        s->unproven = a->listed && t > 0 && shown ? unproven + 1 : 0;
        if (s->unproven < RTSS_UNPROVEN_POLLS) return o.why = unseen, o;
        s->fail = RTSS_FAIL_UNPROVEN;
        o.act = RTSS_DO_PUT_BACK, o.why = RTSS_WHY_FAILED;
        return o;
    }
    if (fresh && held && t > 0) {
        double fps = rtss_fps(a);
        s->over = fps > t / 1000.0 * RTSS_OVER_SLACK ? s->over + 1 : 0;
        if (s->over == RTSS_OVER_PERIODS) {
            s->fail = RTSS_FAIL_NOT_LIMITING;
            o.act = RTSS_DO_PUT_BACK, o.why = RTSS_WHY_FAILED, o.fps = fps;
            return o;
        }
    }
    if (in->movie_mhz) return o.why = RTSS_WHY_MOVIE, o;
    if (!shown) return o.why = RTSS_WHY_IDLE, o;
    s->held = o.holds = 1;
    return o.why = RTSS_WHY_HOLDS, o;
}

// What a write of the limit did (RtssOps.apply).
enum {
    RTSS_WROTE_READONLY = -1,        // the save did not reach the file
    RTSS_WROTE_REFUSED = 0,          // RTSS refused the value; the profile keeps what it held
    RTSS_WROTE_HELD = 1,             // the profile holds the limit
    RTSS_WROTE_REFUSED_CLEARED = 2,  // refused, and the earlier limit of this run was cleared to none
};

static inline void rtss_written(RtssState *s, RtssOut *o, int r) {
    if (r == RTSS_WROTE_READONLY) {
        s->fail = RTSS_FAIL_READONLY, o->holds = 0, o->why = RTSS_WHY_FAILED;
        return;
    }
    s->changed = 1;
    if (r == RTSS_WROTE_HELD) {
        s->applied = o->mhz;
        return;
    }
    if (r == RTSS_WROTE_REFUSED_CLEARED) s->applied = 0;
    s->refused = o->mhz, o->holds = 0, o->why = RTSS_WHY_REFUSED;
}

// The profile was put back: nothing of this run is in it.
static inline void rtss_put_back_done(RtssState *s) { s->changed = 0, s->applied = -1; }

// The profile calls rtss_poll makes. apply writes mhz (first: the first write of the run, which records what to put
// back; clear: on a refusal, clear this run's earlier limit to none) and returns RTSS_WROTE_*; put_back puts the profile
// back as it was before the run (o says why: kept out, or the frame count check gave up).
typedef struct {
    void *ctx;
    int (*apply)(void *ctx, int64_t mhz, int first, int clear);
    void (*put_back)(void *ctx, const RtssOut *o);
} RtssOps;

static inline RtssOut rtss_poll(RtssState *s, const RtssIn *in, const RtssOps *ops) {
    RtssOut o = rtss_decide(s, in);
    if (o.act == RTSS_DO_APPLY) {
        rtss_written(s, &o, ops->apply(ops->ctx, o.mhz, !s->changed, s->applied != 0));
    } else if (o.act == RTSS_DO_PUT_BACK) {
        ops->put_back(ops->ctx, &o);
        rtss_put_back_done(s);
    }
    return o;
}

static inline const char *rtss_fail_text(int fail) {
    return fail == RTSS_FAIL_READONLY       ? "RivaTuner Statistics Server's profile cannot be changed"
           : fail == RTSS_FAIL_NOT_LIMITING ? "RivaTuner Statistics Server does not hold the game to the limit"
           : fail == RTSS_FAIL_UNPROVEN     ? "RivaTuner Statistics Server never showed it holds the game"
                                            : "RivaTuner Statistics Server's hooks library did not load";
}

// Why the game's own limiter paces, for the log line "the game's own limiter (<text>)".
static inline void rtss_why_text(char *out, size_t n, const RtssOut *o, int fail) {
    if (o->why == RTSS_WHY_OUT) snprintf(out, n, "RivaTuner Statistics Server kept out: %s", rtss_out_text(o->mhz));
    else if (o->why == RTSS_WHY_FAILED) snprintf(out, n, "%s", rtss_fail_text(fail));
    else if (o->why == RTSS_WHY_NOT_RUNNING) snprintf(out, n, "RivaTuner Statistics Server is not running");
    else if (o->why == RTSS_WHY_REFUSED) snprintf(out, n, "RivaTuner Statistics Server refused this limit");
    else if (o->why == RTSS_WHY_MOVIE) snprintf(out, n, "a movie plays: the game keeps its pictures on time");
    else if (o->why == RTSS_WHY_NOT_VULKAN)
        snprintf(out, n, "RivaTuner Statistics Server lists the game but has not seen its Vulkan presents yet");
    else if (o->why == RTSS_WHY_UPDATE)
        snprintf(out, n, "RivaTuner Statistics Server has not read the game's limit from its profile yet");
    else if (o->why == RTSS_WHY_NOT_COUNTING)
        snprintf(out, n, "RivaTuner Statistics Server has measured no frames of the game lately");
    else if (o->why == RTSS_WHY_IDLE) snprintf(out, n, "no picture went out lately: RivaTuner has none to hold");
    else if (o->why == RTSS_WHY_SETTLING) snprintf(out, n, "a new limit: RivaTuner Statistics Server gets it once it stays");
    else if (o->why == RTSS_WHY_NO_PRESENT) snprintf(out, n, "no picture has gone out yet");
    else snprintf(out, n, "RivaTuner Statistics Server has not hooked the game yet");
}

// ---- the game's pacer beside RTSS (frame_rate.c frame_step_cap_ns)
// The period the pacer keeps (now_ns and hosted_ns are kern_now_ns times; hosted_ns of the newest host present,
// gfx_present.c gfx_window_hosted_ns, 0 before the first). The limit's own period while RTSS does not hold the game,
// while a movie plays (from its first frame: the pacer keeps the movie's slots, which RTSS does not know; rtss_target
// says what RTSS's profile holds meanwhile), before the first host present, and
// once no host present went out for RTSS_HOLD_IDLE_NS: a minimised window or a frame hold (vk_backend.c frame_hold)
// gives RTSS nothing to hold, and a pacer that held nothing then let the quick menu run at the 640-700 fps that read
// address 0 (frame_rate.c LIMIT_FREE_MAX_HZ). While RTSS holds, the ceiling's period and never 0: the frames RTSS lets
// through come further apart than that, so pace() waits for none of them, and it still holds the game's frames to the
// ceiling the game's code needs. The exception is a limit of 240, or unlimited, which RTSS gets as 240: there the
// ceiling is RTSS's own period, and both may hold a frame.
static inline uint64_t rtss_pace_period(int holds, int movie, uint64_t now_ns, uint64_t hosted_ns, uint64_t period,
                                        uint64_t ceiling) {
    uint64_t idle = now_ns > hosted_ns ? now_ns - hosted_ns : 0;
    if (!holds || movie || !hosted_ns || idle >= RTSS_HOLD_IDLE_NS) return period;
    return ceiling < period ? ceiling : period;
}

// ---- the state file (rtss_limit.c, rtss-profile.txt next to the executable): each RTSS profile a run changed, as it
// was before, so that a start after a run that did not close (a kill, a hang, a crash) puts it back. One line per
// profile, so executables that share the folder keep their own:
//   rtss1 <existed 0|1> <old limit> <old denominator> <written limit> <written denominator> <previous limit>
//         <previous denominator> <profile name>
// on one line. The name comes last and may hold spaces; lines that start with # are comments. The line takes a new
// value before the profile does, and the value written before it stays as the previous one until the profile holds the
// new one: a run that ends between the two leaves either value in the profile, and both are the game's.
#define RTSS_SAVED_HEADER                                                                                              \
    "# RivaTuner Statistics Server's profiles as they were before the game changed them (rtss_limit.c): existed, old\n" \
    "# limit and denominator, the game's limit and denominator, the game's previous ones, profile. The game puts a\n"   \
    "# profile back and removes its line when it closes; a start that finds a line puts that profile back first.\n"
enum { RTSS_NAME_MAX = 260 };

typedef struct {
    int existed;                   // the profile existed before the run changed it
    uint32_t old_limit, old_den;   // what it held then
    uint32_t limit, den;           // what the run wrote last
    uint32_t prev_limit, prev_den;   // what it wrote before, while the profile may still hold it; else limit and den
    char name[RTSS_NAME_MAX];      // the profile's name, the executable's (ocarina.exe)
} RtssSaved;

// The line of r, with its newline: its length, 0 when it does not fit n.
static inline size_t rtss_saved_format(char *out, size_t n, const RtssSaved *r) {
    int k = snprintf(out, n, "rtss1 %d %u %u %u %u %u %u %s\n", r->existed ? 1 : 0, r->old_limit, r->old_den,
                     r->limit, r->den, r->prev_limit, r->prev_den, r->name);
    return k > 0 && (size_t)k < n ? (size_t)k : 0;
}

// One line (len bytes, no newline) into r: 1 when it is a well-formed line of this format, 0 otherwise.
static inline int rtss_saved_parse(const char *line, size_t len, RtssSaved *r) {
    char buf[RTSS_NAME_MAX + 96];
    while (len && (line[len - 1] == '\r' || line[len - 1] == ' ')) len--;
    if (len >= sizeof buf) return 0;
    memcpy(buf, line, len);
    buf[len] = 0;
    int existed = -1, at = 0;
    unsigned v[6];
    if (sscanf(buf, "rtss1 %d %u %u %u %u %u %u %n", &existed, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &at) != 7 ||
        !at)
        return 0;
    size_t name = len - (size_t)at;
    if ((existed != 0 && existed != 1) || !v[1] || !v[3] || !v[5] || !name || name >= RTSS_NAME_MAX) return 0;
    r->existed = existed, r->old_limit = v[0], r->old_den = v[1], r->limit = v[2], r->den = v[3];
    r->prev_limit = v[4], r->prev_den = v[5];
    memcpy(r->name, buf + at, name + 1);
    return 1;
}

// Profile names compare as Windows compares file names: ASCII letters in either case.
static inline int rtss_name_eq(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

// The line of profile name in the file's text (NUL-terminated): 1 and r, or 0. A line that is not well formed is
// passed over, and the lines after it are still read.
static inline int rtss_saved_find(const char *text, const char *name, RtssSaved *r) {
    for (const char *p = text; p && *p;) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (*p != '#' && rtss_saved_parse(p, len, r) && rtss_name_eq(r->name, name)) return 1;
        p = end ? end + 1 : NULL;
    }
    return 0;
}

// The file's text with profile name's line replaced by r's (r NULL: removed): the header, every other profile's
// well-formed line as it was, then r's. Comments, blank and malformed lines are dropped. Writes the text into out and
// returns its length (0 when it does not fit cap); *records is the number of profile lines in it, 0 when the file is
// to go.
static inline size_t rtss_saved_update(const char *text, const char *name, const RtssSaved *r, char *out, size_t cap,
                                       int *records) {
    size_t at = (size_t)snprintf(out, cap, "%s", RTSS_SAVED_HEADER);
    *records = 0;
    if (at >= cap) return 0;
    for (const char *p = text; p && *p;) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        RtssSaved other;
        if (*p != '#' && rtss_saved_parse(p, len, &other) && !rtss_name_eq(other.name, name)) {
            size_t k = rtss_saved_format(out + at, cap - at, &other);
            if (!k) return 0;
            at += k, ++*records;
        }
        p = end ? end + 1 : NULL;
    }
    if (r) {
        size_t k = rtss_saved_format(out + at, cap - at, r);
        if (!k) return 0;
        at += k, ++*records;
    }
    return at;
}

// What a start does with a line a run that did not close left (r), given the profile as it is now: whether it exists
// and the limit and denominator it holds. Either value of the line is the game's (RtssSaved.prev_limit).
enum {
    RTSS_PLAN_NONE,     // no profile, and there was none before that run: as it was
    RTSS_PLAN_LEAVE,    // the player changed or removed the profile since: theirs as it is
    RTSS_PLAN_WRITE,    // it still holds what the run wrote: the old limit and denominator back
    RTSS_PLAN_DELETE,   // the same, for a profile that run made: removed
};
static inline int rtss_restore_plan(const RtssSaved *r, int exists, uint32_t limit, uint32_t den) {
    if (!exists) return r->existed ? RTSS_PLAN_LEAVE : RTSS_PLAN_NONE;
    den = den ? den : 1;
    if ((limit != r->limit || den != r->den) && (limit != r->prev_limit || den != r->prev_den)) return RTSS_PLAN_LEAVE;
    return r->existed ? RTSS_PLAN_WRITE : RTSS_PLAN_DELETE;
}

// ---- RTSS's own settings
// RTSS's own [Framerate] settings for the game, as its profile file holds them (rtss_limit.c read_settings): a frame
// rate limit (Limit over LimitDenominator), a frame time limit (LimitTime over LimitTimeDenominator) and scanline sync
// (SyncScanline0 and SyncScanline1), 0 off for each; and the modes beside them, for the log: SyncDisplay, ReflexSleep
// and PassiveWait.
typedef struct {
    int limit, limit_den, time, time_den, scan0, scan1;
    int sync_display, reflex_sleep, passive_wait;
} RtssSettings;

// RTSS holds the game's frames by a setting of its own.
static inline int rtss_settings_limit(const RtssSettings *s) { return s->limit || s->time || s->scan0 || s->scan1; }

// RTSS has no part in the game's limit although it may list the game: the want keeps it out, or it is out for the run
// because its profile cannot be written, its hooks library did not load or it never showed it holds the game (its
// profile put back, a setting of its own may still hold the game). A run whose frame count check gave up on RTSS does
// not count: RTSS was seen not to hold the game.
static inline int rtss_out_of_limit(int64_t want, int fail) {
    return want != RTSS_UNSET &&
           (want < 0 || fail == RTSS_FAIL_READONLY || fail == RTSS_FAIL_LIBRARY || fail == RTSS_FAIL_UNPROVEN);
}

// Two limiters: RTSS is out of the game's limit (rtss_out_of_limit), and while it lists the game a setting of its own
// still holds the frames (rtss_settings_limit), beside the game's limiter, the driver's or frame generation's.
static inline int rtss_two(int64_t want, int fail, int listed, int own_limit) {
    return rtss_out_of_limit(want, fail) && listed && own_limit;
}

// "limit L/D, frame time limit T/TD, scanline sync S0/S1", and with modes ", display sync X, Reflex sleep R, passive
// wait P": the values as the file holds them.
static inline void rtss_settings_text(char *out, size_t n, const RtssSettings *s, int modes) {
    int k = snprintf(out, n, "limit %d/%d, frame time limit %d/%d, scanline sync %d/%d", s->limit, s->limit_den, s->time,
                     s->time_den, s->scan0, s->scan1);
    if (modes && k >= 0 && (size_t)k < n)
        snprintf(out + k, n - (size_t)k, ", display sync %d, Reflex sleep %d, passive wait %d", s->sync_display,
                 s->reflex_sleep, s->passive_wait);
}
