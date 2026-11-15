// runtime/rtss_decide.h: the parts of RivaTuner Statistics Server's frame limiting (rtss_limit.c) that make no system
// call: the shared memory's reader against a fake RTSSSharedMemoryV2, the worker's decisions poll by poll against a
// fake profile and a fake RTSS clock, the pacer's period beside RTSS and the state file's lines. Built and run by
// tests/rtss_limit_check.py on any system; prints each failed check and exits 1 on any.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtss_decide.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// RTSS's own settings: which of them hold the game's frames, and the log's text of them.
static void settings(void) {
    RtssSettings none = {0, 1, 0, 0, 0, 0, 1, 1, 1};
    CHECK(!rtss_settings_limit(&none), "no limit, no frame time limit, no scanline sync: RTSS holds nothing");
    RtssSettings s = none;
    s.limit = 50;
    CHECK(rtss_settings_limit(&s), "a frame rate limit holds the frames");
    s = none, s.time = 16667, s.time_den = 1;
    CHECK(rtss_settings_limit(&s), "a frame time limit holds the frames");
    s = none, s.scan0 = -40;
    CHECK(rtss_settings_limit(&s), "scanline sync from the bottom holds the frames");
    s = none, s.scan1 = 1;
    CHECK(rtss_settings_limit(&s), "the second scanline sync holds the frames");
    s = none, s.limit_den = 1000, s.time_den = 7;
    CHECK(!rtss_settings_limit(&s), "denominators alone hold nothing");
    CHECK(!rtss_settings_limit(&none), "display sync, Reflex sleep and passive wait alone hold nothing");
    char text[192];
    s = (RtssSettings){59940, 1000, 16667, 1, -40, 0, 1, 2, 0};
    rtss_settings_text(text, sizeof text, &s, 0);
    CHECK(!strcmp(text, "limit 59940/1000, frame time limit 16667/1, scanline sync -40/0"), "text: %s", text);
    rtss_settings_text(text, sizeof text, &s, 1);
    CHECK(!strcmp(text, "limit 59940/1000, frame time limit 16667/1, scanline sync -40/0, display sync 1, Reflex sleep 2, "
                        "passive wait 0"),
          "text with the modes: %s", text);
    char small[16], mid[70];
    rtss_settings_text(small, sizeof small, &s, 1);
    CHECK(strlen(small) == sizeof small - 1, "a short buffer is cut, not overrun");
    rtss_settings_text(mid, sizeof mid, &s, 1);
    CHECK(strlen(mid) == sizeof mid - 1 && !strncmp(mid, text, sizeof mid - 1), "the modes cut at the buffer's end");
}

// The kept-out reasons, for the log.
static void reasons(void) {
    CHECK(!strcmp(rtss_out_text(RTSS_OUT_SETTING), "NX2NAVI_RTSS=0"), "the setting");
    CHECK(!strcmp(rtss_out_text(RTSS_OUT_FRAMEGEN), "frame generation"), "frame generation");
    CHECK(!strcmp(rtss_out_text(RTSS_OUT_LOWLAT), "the driver's low latency"), "low latency");
    CHECK(!strcmp(rtss_out_limiter(RTSS_OUT_SETTING), "the game's limiter"), "the setting leaves the game's limiter");
    CHECK(!strcmp(rtss_out_limiter(RTSS_OUT_LOWLAT), "the driver's low latency"), "low latency holds the frames");
    CHECK(RTSS_UNSET < 0 && RTSS_OUT_SETTING < 0 && RTSS_OUT_FRAMEGEN < 0 && RTSS_OUT_LOWLAT < 0,
          "every reason is below 0, where a limit never is");
    CHECK(RTSS_UNSET != RTSS_OUT_SETTING && RTSS_UNSET != RTSS_OUT_FRAMEGEN && RTSS_UNSET != RTSS_OUT_LOWLAT &&
              RTSS_OUT_SETTING != RTSS_OUT_FRAMEGEN && RTSS_OUT_FRAMEGEN != RTSS_OUT_LOWLAT &&
              RTSS_OUT_SETTING != RTSS_OUT_LOWLAT,
          "the reasons differ");
}

// ---- a fake RTSSSharedMemoryV2: the header and application entries written as RTSS lays them out
enum { PID = 4242, ESIZE = 1024, OFF = 0x100 };
static void shm_set(uint8_t *v, uint32_t sig, uint32_t ver, uint32_t esize, uint32_t off, uint32_t n) {
    uint32_t h[5] = {sig, ver, esize, off, n};
    memcpy(v, h, sizeof h);
}
static void app_put(uint8_t *v, uint32_t esize, uint32_t off, uint32_t slot, uint32_t pid, uint32_t t0, uint32_t t1,
                    uint32_t frames) {
    uint8_t *e = v + off + (size_t)slot * esize;
    uint32_t t[3] = {t0, t1, frames}, flags = RTSS_API_VULKAN | RTSS_UPDATE_ASKED;
    memcpy(e, &pid, 4);
    memcpy(e + 4 + 260, &flags, 4);   // RTSS_SHARED_MEMORY_APP_ENTRY: dwFlags after the 260-byte path
    memcpy(e + RTSS_PERIOD_AT, t, sizeof t);
}

// rtss_parse: where the entry is, and that a bad header, a short entry or an array past the memory finds nothing.
static void parse(void) {
    static uint8_t v[65536];
    RtssApp a;
    shm_set(v, RTSS_SIGNATURE, 0x0002000E, ESIZE, OFF, 3);
    app_put(v, ESIZE, OFF, 0, 7, 0, 0, 0);
    app_put(v, ESIZE, OFF, 1, 8, 0, 0, 0);
    app_put(v, ESIZE, OFF, 2, PID, 1000, 2000, 60);
    rtss_parse(v, sizeof v, PID, &a);
    CHECK(a.running && a.listed && a.t0 == 1000 && a.t1 == 2000 && a.frames == 60, "the third entry is ours");
    CHECK(a.flags == (RTSS_API_VULKAN | RTSS_UPDATE_ASKED) && RTSS_FLAGS_AT == 264 && RTSS_PERIOD_AT == 268,
          "the flags at +264, the period at +268: %#x", a.flags);
    CHECK(rtss_fps(&a) == 60.0, "60 frames in a second: %f", rtss_fps(&a));
    rtss_parse(v, sizeof v, 99, &a);
    CHECK(a.running && !a.listed && rtss_fps(&a) == 0, "another process: running, not listed");
    shm_set(v, 0xDEAD, 0x0002000E, ESIZE, OFF, 3);
    rtss_parse(v, sizeof v, PID, &a);
    CHECK(!a.running && !a.listed, "no signature: not running");
    shm_set(v, RTSS_SIGNATURE, 0x00010003, ESIZE, OFF, 3);
    rtss_parse(v, sizeof v, PID, &a);
    CHECK(!a.running, "a version before 2.0: not running");
    shm_set(v, RTSS_SIGNATURE, 0x0002000E, RTSS_ENTRY_MIN - 1, OFF, 3);
    rtss_parse(v, sizeof v, PID, &a);
    CHECK(a.running && !a.listed, "entries shorter than the period: not read");
    rtss_parse(v, 19, PID, &a);
    CHECK(!a.running, "a memory shorter than the header");
    rtss_parse(NULL, 0, PID, &a);
    CHECK(!a.running, "no memory");
    // entries of the smallest size, ours the last that fits: found; past the memory's end: not read
    memset(v, 0, sizeof v);
    shm_set(v, RTSS_SIGNATURE, 0x0002000E, RTSS_ENTRY_MIN, OFF, 1000);
    uint32_t fit = (uint32_t)((sizeof v - OFF) / RTSS_ENTRY_MIN) - 1;
    app_put(v, RTSS_ENTRY_MIN, OFF, fit, PID, 1000, 1500, 30);
    rtss_parse(v, sizeof v, PID, &a);
    CHECK(a.listed && a.frames == 30, "the last entry inside the memory is read");
    rtss_parse(v, OFF + (size_t)fit * RTSS_ENTRY_MIN + RTSS_ENTRY_MIN - 1, PID, &a);
    CHECK(a.running && !a.listed, "an entry one byte past the memory is not read");
    // more than RTSS_APPS_MAX entries: the walk stops there
    size_t big = OFF + (size_t)5000 * RTSS_ENTRY_MIN;
    uint8_t *b = calloc(1, big);
    shm_set(b, RTSS_SIGNATURE, 0x0002000E, RTSS_ENTRY_MIN, OFF, 5000);
    app_put(b, RTSS_ENTRY_MIN, OFF, 4500, PID, 1000, 2000, 60);
    rtss_parse(b, big, PID, &a);
    CHECK(a.running && !a.listed, "an entry past %d is not looked at", RTSS_APPS_MAX);
    app_put(b, RTSS_ENTRY_MIN, OFF, 4095, PID, 1000, 2000, 60);
    rtss_parse(b, big, PID, &a);
    CHECK(a.listed, "entry %d is", RTSS_APPS_MAX - 1);
    free(b);
}

// ---- a fake profile behind RtssOps, and one poll as the worker makes it
typedef struct {
    int readonly, no_library, loaded;
    int64_t refuse_above;   // RTSS refuses a limit above this (millihertz); 0 none
    int64_t holds;          // the limit the profile holds from this run; -1 none
    int applies, firsts, clears, put_backs, last_put_back_why;
} Store;
static int f_apply(void *ctx, int64_t mhz, int first, int clear) {
    Store *st = ctx;
    st->applies++, st->firsts += first;
    if (st->readonly) return RTSS_WROTE_READONLY;
    if (st->refuse_above && mhz > st->refuse_above) {
        if (!clear) return RTSS_WROTE_REFUSED;
        st->clears++, st->holds = 0;
        return RTSS_WROTE_REFUSED_CLEARED;
    }
    st->holds = mhz;
    return RTSS_WROTE_HELD;
}
static void f_put_back(void *ctx, const RtssOut *o) {
    Store *st = ctx;
    st->put_backs++, st->holds = -1, st->last_put_back_why = o->why;
}

// RTSS as the worker sees it: polls 500 ms apart; while it runs, lists the game and counts (counting) it measures a
// period of a second every second poll, with frames frames in it (the frame rate); the game presents (presenting) and
// the newest host present is a millisecond before the poll, else it stays where it was (0: none yet).
typedef struct {
    RtssState s;
    Store st;
    RtssOps ops;
    int running, listed, counting, presenting, polls;
    uint32_t flags, t0, t1, frames;
    int64_t movie_mhz;   // a movie plays at this rate (rtss_limit_movie)
    uint64_t now_ns, hosted_ns;
} Run;
static void run_init(Run *r) {
    memset(r, 0, sizeof *r);
    r->s = (RtssState)RTSS_STATE_INIT;
    r->st.holds = -1;
    r->ops = (RtssOps){&r->st, f_apply, f_put_back};
    r->running = r->listed = r->counting = r->presenting = 1;
    r->flags = RTSS_API_VULKAN, r->t0 = 1000, r->t1 = 2000, r->frames = 60;
    r->now_ns = 10000000000ull;
}
// One poll for want, as rtss_limit.c worker makes it: the hooks library loads on the first poll that wants it and stays.
static RtssOut step_ex(Run *r, int64_t want, int current) {
    r->now_ns += 500000000ull;
    if (r->running && r->listed && r->counting && ++r->polls % 2 == 0) r->t0 = r->t1, r->t1 += 1000;
    if (r->presenting) r->hosted_ns = r->now_ns - 1000000;
    RtssIn in = {.want = want, .current = current, .movie_mhz = r->movie_mhz, .now_ns = r->now_ns,
                 .hosted_ns = r->hosted_ns};
    in.app = (RtssApp){r->running, r->running && r->listed, r->flags, r->t0, r->t1, r->frames};
    if (r->st.loaded) in.hooks = 1;
    else if (rtss_wants_hooks(&r->s, &in)) in.hooks = r->st.no_library ? 0 : (r->st.loaded = 1);
    else in.hooks = -1;
    return rtss_poll(&r->s, &in, &r->ops);
}
static RtssOut step(Run *r, int64_t want) { return step_ex(r, want, 1); }
static void why_is(const RtssOut *o, const RtssState *s, const char *want, int line) {
    char t[128];
    rtss_why_text(t, sizeof t, o, s->fail);
    if (strcmp(t, want)) fails++, printf("FAIL line %d: the reason is '%s', not '%s'\n", line, t, want);
}
#define WHY(o, r, text) why_is(&(o), &(r).s, text, __LINE__)

// RTSS's injection delay: it runs but has not hooked the game for 30 polls. The limit goes into the profile at the
// second poll (the first sees the want new), with no second write. Once RTSS lists the game the game's limiter stands
// aside only after RTSS has seen its Vulkan presents, read the profile and measured a period of its frames lately.
static void injection_delay(void) {
    Run r;
    run_init(&r);
    r.listed = 0;
    RtssOut o = step(&r, 60000);
    CHECK(o.act == RTSS_DO_NOTHING && !o.holds && o.why == RTSS_WHY_SETTLING, "a new want: not written yet");
    WHY(o, r, "a new limit: RivaTuner Statistics Server gets it once it stays");
    o = step(&r, 60000);
    CHECK(o.act == RTSS_DO_APPLY && !o.holds && o.why == RTSS_WHY_NOT_LISTED, "written, not held yet");
    CHECK(r.st.firsts == 1 && r.s.applied == 60000 && r.s.changed, "the first write of the run");
    for (int i = 0; i < 30; i++) o = step(&r, 60000);
    CHECK(r.st.applies == 1 && !o.holds && o.why == RTSS_WHY_NOT_LISTED, "no write while not listed");
    WHY(o, r, "RivaTuner Statistics Server has not hooked the game yet");
    r.listed = 1, r.flags = 0;
    o = step(&r, 60000);
    CHECK(!o.holds && o.why == RTSS_WHY_NOT_VULKAN, "listed with no API yet");
    WHY(o, r, "RivaTuner Statistics Server lists the game but has not seen its Vulkan presents yet");
    r.flags = 0x7;   // another API's value
    o = step(&r, 60000);
    CHECK(!o.holds && o.why == RTSS_WHY_NOT_VULKAN, "another API");
    r.flags = RTSS_API_VULKAN | RTSS_UPDATE_ASKED;
    o = step(&r, 60000);
    CHECK(!o.holds && o.why == RTSS_WHY_UPDATE, "the profile not read yet");
    WHY(o, r, "RivaTuner Statistics Server has not read the game's limit from its profile yet");
    r.flags = RTSS_API_VULKAN, r.counting = 0;
    for (int i = 0; i < 6; i++) o = step(&r, 60000);
    CHECK(!o.holds && o.why == RTSS_WHY_NOT_COUNTING, "no period for three seconds");
    WHY(o, r, "RivaTuner Statistics Server has measured no frames of the game lately");
    r.counting = 1;
    for (int i = 0; i < 2; i++) o = step(&r, 60000);
    CHECK(o.holds && o.why == RTSS_WHY_HOLDS && r.st.applies == 1, "held once counting, no second write");
}

// RTSS quits while it limits, and comes back: the profile stays for its return.
static void quit(void) {
    Run r;
    run_init(&r);
    step(&r, 60000);
    step(&r, 60000);
    RtssOut o = step(&r, 60000);
    CHECK(o.holds, "held");
    r.running = 0;
    o = step(&r, 60000);
    CHECK(!o.holds && o.why == RTSS_WHY_NOT_RUNNING && o.act == RTSS_DO_NOTHING, "not running: the game limits");
    CHECK(!r.st.put_backs && r.st.holds == 60000, "the profile stays");
    r.running = 1;
    for (int i = 0; i < 2; i++) o = step(&r, 60000);
    CHECK(o.holds && r.st.applies == 1, "back: held with no new write");
}

// Profiles under Program Files without administrator rights: out for the run.
static void readonly(void) {
    Run r;
    run_init(&r);
    r.st.readonly = 1;
    step(&r, 60000);
    RtssOut o = step(&r, 60000);
    CHECK(o.act == RTSS_DO_APPLY && !o.holds && o.why == RTSS_WHY_FAILED && r.s.fail == RTSS_FAIL_READONLY, "failed");
    CHECK(!r.s.changed, "nothing of the run in the profile");
    for (int i = 0; i < 20; i++) o = step(&r, i < 10 ? 60000 : 144000);
    CHECK(r.st.applies == 1 && o.why == RTSS_WHY_FAILED, "no write after it");
    WHY(o, r, "RivaTuner Statistics Server's profile cannot be changed");
}

// A limit out of RTSS's range: refused and kept by the game; the earlier limit of this run is cleared to none; a limit
// in range goes to RTSS once it stays.
static void refused(void) {
    Run r;
    run_init(&r);
    r.st.refuse_above = 200000;
    step(&r, 60000);
    step(&r, 60000);
    step(&r, 240000);
    RtssOut o = step(&r, 240000);
    CHECK(o.act == RTSS_DO_APPLY && !o.holds && o.why == RTSS_WHY_REFUSED, "refused");
    CHECK(r.s.refused == 240000 && r.s.applied == 0 && r.st.clears == 1 && r.st.holds == 0, "the 60 cleared to none");
    for (int i = 0; i < 5; i++) o = step(&r, 240000);
    CHECK(r.st.applies == 2 && o.why == RTSS_WHY_REFUSED && !o.holds, "not written again");
    o = step(&r, 144000);
    CHECK(o.act == RTSS_DO_NOTHING && o.why == RTSS_WHY_SETTLING, "144 settles first");
    o = step(&r, 144000);
    CHECK(o.act == RTSS_DO_APPLY && !o.holds && r.st.holds == 144000, "144 written");
    o = step(&r, 144000);
    CHECK(o.holds, "held from the next poll");
    CHECK(r.st.firsts == 1, "one first write in the run (%d)", r.st.firsts);
    o = step(&r, 240000);
    CHECK(o.act == RTSS_DO_NOTHING && o.why == RTSS_WHY_REFUSED, "the refused limit stays with the game at once");
    // refused as the run's first write: the profile is the run's from then on
    run_init(&r);
    r.st.refuse_above = 200000;
    step(&r, 240000);
    o = step(&r, 240000);
    CHECK(o.why == RTSS_WHY_REFUSED && r.s.changed && r.st.clears == 1, "a first refusal clears too, as it stands");
}

// RTSS's own frame count: each fresh period after a poll that stood aside is checked, and more than 1.05 times the
// limit for three of them in a row puts the profile back for the run. 69 fps at 60 (1.15 times) is caught, 63 is not;
// a period RTSS no longer moves counts neither way; no limit and not listed never count.
static void frame_count(void) {
    Run r;
    run_init(&r);
    r.frames = 144;
    RtssOut o = step(&r, 60000);
    int polls = 1;
    while (!r.s.fail && polls < 40) o = step(&r, 60000), polls++;
    CHECK(o.act == RTSS_DO_PUT_BACK && !o.holds && o.why == RTSS_WHY_FAILED && r.s.fail == RTSS_FAIL_NOT_LIMITING,
          "144 fps at 60 gives up");
    CHECK(polls == 8, "after three fresh periods it held through (poll %d)", polls);
    CHECK(o.fps == 144.0 && r.st.put_backs == 1 && r.st.last_put_back_why == RTSS_WHY_FAILED && !r.s.changed,
          "put back, with the measured rate");
    o = step(&r, 30000);
    CHECK(o.why == RTSS_WHY_FAILED && r.st.applies == 1, "out for the run");
    WHY(o, r, "RivaTuner Statistics Server does not hold the game to the limit");
    run_init(&r);
    r.frames = 69;
    for (int i = 0; i < 40 && !r.s.fail; i++) o = step(&r, 60000);
    CHECK(r.s.fail == RTSS_FAIL_NOT_LIMITING, "69 fps at 60 is caught");
    run_init(&r);
    r.frames = 63;   // 1.05 times the limit exactly
    for (int i = 0; i < 40; i++) o = step(&r, 60000);
    CHECK(o.holds && !r.s.fail, "63 fps at 60 is not");
    run_init(&r);
    r.frames = 700;
    for (int i = 0; i < 20; i++) o = step(&r, 0);
    CHECK(o.holds && !r.s.fail, "no limit: nothing to check");
    run_init(&r);
    r.frames = 144, r.listed = 0;
    for (int i = 0; i < 20; i++) o = step(&r, 60000);
    CHECK(!r.s.fail && !r.s.over, "not listed: no count");
    // a period RTSS no longer moves (the player lowered the limit and minimised the window): never counted
    run_init(&r);
    r.frames = 144, r.counting = 0;
    for (int i = 0; i < 20; i++) o = step(&r, 60000);
    CHECK(!r.s.fail && !r.s.over && !o.holds && o.why == RTSS_WHY_NOT_COUNTING, "a frozen period: not counting");
    // two periods over, then one under: the count starts again
    run_init(&r);
    r.frames = 144;
    for (int i = 0; i < 7; i++) step(&r, 60000);
    CHECK(r.s.over == 2, "two periods over (%d)", r.s.over);
    r.frames = 60;
    for (int i = 0; i < 2; i++) step(&r, 60000);
    CHECK(r.s.over == 0 && !r.s.fail, "one under: the count starts again");
    // a want that changed while the poll ran: nothing decided, the count waits
    run_init(&r);
    r.frames = 144;
    for (int i = 0; i < 5; i++) step(&r, 60000);
    o = step_ex(&r, 60000, 0);
    CHECK(o.why == RTSS_WHY_NEWER && !o.holds && o.act == RTSS_DO_NOTHING && r.s.over == 1, "a newer want: no count");
}

// RTSS lists the game with the limit in its profile and pictures go out, but never shows its hold: it may hold the
// frames beside the game's limiter, so after RTSS_UNPROVEN_POLLS such polls in a row the profile goes back once, for
// the run. Each sign that never comes counts; a minimised window, RTSS not hooked and no limit do not count, and a poll
// that shows the hold starts the count again.
static void unproven(void) {
    static const struct { uint32_t flags; int period; const char *what, *why; } stuck[] = {
        {RTSS_API_VULKAN | RTSS_UPDATE_ASKED, 1, "the profile never read",
         "RivaTuner Statistics Server has not read the game's limit from its profile yet"},
        {0, 1, "no API seen", "RivaTuner Statistics Server lists the game but has not seen its Vulkan presents yet"},
        {0x7, 1, "another API's presents",
         "RivaTuner Statistics Server lists the game but has not seen its Vulkan presents yet"},
        {RTSS_API_VULKAN, 0, "no period measured",
         "RivaTuner Statistics Server has measured no frames of the game lately"},
    };
    Run r;
    RtssOut o;
    for (unsigned k = 0; k < sizeof stuck / sizeof *stuck; k++) {
        run_init(&r);
        r.flags = stuck[k].flags;
        if (!stuck[k].period) r.counting = 0, r.t0 = r.t1 = 0;
        step(&r, 60000);
        o = step(&r, 60000);
        CHECK(o.act == RTSS_DO_APPLY, "%s: written", stuck[k].what);
        int polls = 0;
        for (; polls < RTSS_UNPROVEN_POLLS - 1; polls++) o = step(&r, 60000);
        CHECK(!r.s.fail && !o.holds && !r.st.put_backs, "%s: %d polls, not yet", stuck[k].what, polls);
        WHY(o, r, stuck[k].why);
        o = step(&r, 60000);
        CHECK(o.act == RTSS_DO_PUT_BACK && o.why == RTSS_WHY_FAILED && r.s.fail == RTSS_FAIL_UNPROVEN &&
                  r.st.put_backs == 1 && r.st.last_put_back_why == RTSS_WHY_FAILED && !r.s.changed,
              "%s: put back at poll %d", stuck[k].what, RTSS_UNPROVEN_POLLS);
        for (int i = 0; i < 10; i++) o = step(&r, i < 5 ? 60000 : 144000);
        CHECK(r.st.put_backs == 1 && r.st.applies == 1 && o.why == RTSS_WHY_FAILED && !o.holds,
              "%s: once, out for the run", stuck[k].what);
        WHY(o, r, "RivaTuner Statistics Server never showed it holds the game");
        CHECK(rtss_out_of_limit(60000, r.s.fail), "%s: a setting of RTSS's own may still hold the game", stuck[k].what);
    }
    // a minimised window, RTSS not hooked, or no limit: nothing to count
    run_init(&r);
    r.flags = RTSS_API_VULKAN | RTSS_UPDATE_ASKED;
    step(&r, 60000);
    step(&r, 60000);
    r.presenting = 0;
    for (int i = 0; i < 3 * RTSS_UNPROVEN_POLLS; i++) o = step(&r, 60000);
    CHECK(!r.s.fail && !r.st.put_backs && r.s.unproven == 0, "minimised: not counted");
    r.presenting = 1, r.listed = 0;
    for (int i = 0; i < 3 * RTSS_UNPROVEN_POLLS; i++) o = step(&r, 60000);
    CHECK(!r.s.fail && !r.st.put_backs && o.why == RTSS_WHY_NOT_LISTED, "not hooked: not counted");
    r.listed = 1;
    for (int i = 0; i < 3 * RTSS_UNPROVEN_POLLS; i++) o = step(&r, 0);
    CHECK(!r.s.fail && !r.st.put_backs && r.st.applies == 2, "no limit: not counted");
    // one poll that shows the hold starts the count again
    run_init(&r);
    r.flags = RTSS_API_VULKAN | RTSS_UPDATE_ASKED;
    step(&r, 60000);
    step(&r, 60000);
    for (int i = 0; i < RTSS_UNPROVEN_POLLS - 1; i++) step(&r, 60000);
    r.flags = RTSS_API_VULKAN;
    o = step(&r, 60000);
    CHECK(o.holds && r.s.unproven == 0, "shown once: the count starts again");
    r.flags = RTSS_API_VULKAN | RTSS_UPDATE_ASKED;
    for (int i = 0; i < RTSS_UNPROVEN_POLLS - 1; i++) o = step(&r, 60000);
    CHECK(!r.s.fail && o.why == RTSS_WHY_UPDATE, "%d polls again: not yet", RTSS_UNPROVEN_POLLS - 1);
    o = step(&r, 60000);
    CHECK(o.act == RTSS_DO_PUT_BACK && r.s.fail == RTSS_FAIL_UNPROVEN, "the full count again: put back");
}

// No picture went out for 200 ms (a minimised window, a frame hold) or none yet: RTSS has nothing to hold, and the
// game's limiter takes the limit back until pictures go out again. Before the first picture nothing is written.
static void idle(void) {
    Run r;
    run_init(&r);
    r.presenting = 0;
    RtssOut o;
    for (int i = 0; i < 4; i++) o = step(&r, 60000);
    CHECK(!o.holds && o.why == RTSS_WHY_NO_PRESENT && !r.st.applies, "no host present yet: nothing written");
    WHY(o, r, "no picture has gone out yet");
    r.presenting = 1;
    o = step(&r, 60000);
    CHECK(o.act == RTSS_DO_APPLY && !o.holds, "written at the first picture, the want having stayed");
    o = step(&r, 60000);
    CHECK(o.holds, "held once pictures go out");
    r.presenting = 0;
    r.hosted_ns = r.now_ns + 500000000ull - 199000000ull;   // the next poll comes 199 ms after the last present
    o = step(&r, 60000);
    CHECK(o.holds, "199 ms: still held");
    o = step(&r, 60000);
    CHECK(!o.holds && o.why == RTSS_WHY_IDLE, "minimised");
    WHY(o, r, "no picture went out lately: RivaTuner has none to hold");
    r.presenting = 1;
    o = step(&r, 60000);
    CHECK(o.holds && r.st.applies == 1, "held again, no write");
}

// Kept out and let in again: the first kept-out poll puts back what the run wrote, once; a limit afterwards is
// written at once and is the run's first write again.
static void kept_out(void) {
    Run r;
    run_init(&r);
    step(&r, 60000);
    step(&r, 60000);
    RtssOut o = step(&r, RTSS_OUT_FRAMEGEN);
    CHECK(o.act == RTSS_DO_NOTHING && o.why == RTSS_WHY_OUT && !o.holds && r.st.holds == 60000,
          "the game's limiter at once, the profile once the want stays");
    o = step(&r, RTSS_OUT_FRAMEGEN);
    CHECK(o.act == RTSS_DO_PUT_BACK && o.why == RTSS_WHY_OUT && !o.holds && r.st.holds == -1, "put back");
    CHECK(r.st.last_put_back_why == RTSS_WHY_OUT, "the put back says why");
    o = step(&r, RTSS_OUT_FRAMEGEN);
    CHECK(o.act == RTSS_DO_NOTHING && r.st.put_backs == 1, "once");
    for (int i = 0; i < 3; i++) o = step(&r, RTSS_OUT_LOWLAT);
    CHECK(o.act == RTSS_DO_NOTHING && o.mhz == RTSS_OUT_LOWLAT && r.st.put_backs == 1, "a second reason: nothing more");
    WHY(o, r, "RivaTuner Statistics Server kept out: the driver's low latency");
    o = step(&r, 60000);
    CHECK(o.act == RTSS_DO_NOTHING && o.why == RTSS_WHY_SETTLING, "a limit again: settles first");
    o = step(&r, 60000);
    CHECK(o.act == RTSS_DO_APPLY && !o.holds && r.st.firsts == 2, "written, the run's first write again");
    o = step(&r, 60000);
    CHECK(o.holds, "held from the next poll");
    // kept out from the start: the hooks library is never wanted, and nothing is written or put back
    run_init(&r);
    for (int i = 0; i < 10; i++) o = step(&r, RTSS_OUT_SETTING);
    CHECK(!r.st.loaded && !r.st.applies && !r.st.put_backs && o.why == RTSS_WHY_OUT, "read only");
}

// Low latency switching on and off at every poll (a swapchain rebuild, the time before the first swapchain): the want
// never stays two polls, so the profile keeps the limit written before, and the game's limiter holds the frames while
// the want is new. From the start, the same flips write nothing.
static void churn(void) {
    Run r;
    run_init(&r);
    step(&r, 60000);
    step(&r, 60000);
    RtssOut o;
    for (int i = 0; i < 20; i++) {
        o = step(&r, i & 1 ? 60000 : RTSS_OUT_LOWLAT);
        CHECK(!(i & 1) ? !o.holds : 1, "kept out at poll %d: the game's limiter", i);
    }
    CHECK(r.st.applies == 1 && !r.st.put_backs && r.st.holds == 60000, "%d writes, %d put backs", r.st.applies,
          r.st.put_backs);
    o = step(&r, 60000);
    CHECK(o.holds && r.st.applies == 1, "held once the limit stays");
    run_init(&r);
    for (int i = 0; i < 20; i++) step(&r, i & 1 ? 60000 : RTSS_OUT_LOWLAT);
    CHECK(!r.st.applies && !r.st.put_backs, "from the start: nothing written");
    // a flip every two polls is a want that stays: each one acts
    run_init(&r);
    for (int i = 0; i < 8; i++) step(&r, i & 2 ? RTSS_OUT_LOWLAT : 60000);
    CHECK(r.st.applies == 2 && r.st.put_backs == 2, "two polls each: %d writes, %d put backs", r.st.applies,
          r.st.put_backs);
}

// A movie under RTSS: the want stays the play limit and the game's pacer holds the movie. At a limit of 144 the profile
// keeps 144 through it, with no write; under the movie's rate (40) the profile has no limit for the movie's length, so
// RTSS does not hold the movie below its rate, and gets 40 back at its end; at the movie's own rate (60) it keeps 60.
static void movie(void) {
    Run r;
    run_init(&r);
    step(&r, 144000);
    step(&r, 144000);
    r.movie_mhz = 60000;
    RtssOut o = step(&r, 144000);
    CHECK(o.act == RTSS_DO_NOTHING && !o.holds && o.why == RTSS_WHY_MOVIE, "a movie at 144: no write, the pacer's");
    for (int i = 0; i < 10; i++) o = step(&r, 144000);
    CHECK(!o.holds && o.why == RTSS_WHY_MOVIE && r.st.applies == 1, "through the movie");
    WHY(o, r, "a movie plays: the game keeps its pictures on time");
    r.movie_mhz = 0;
    o = step(&r, 144000);
    CHECK(o.act == RTSS_DO_NOTHING && o.holds && o.why == RTSS_WHY_HOLDS && r.st.applies == 1, "held again after it");
    run_init(&r);
    step(&r, 40000);
    step(&r, 40000);
    r.movie_mhz = 60000;
    o = step(&r, 40000);
    CHECK(o.act == RTSS_DO_APPLY && o.mhz == 0 && !o.holds && o.why == RTSS_WHY_MOVIE && r.st.holds == 0,
          "a movie at 40: no limit in the profile");
    o = step(&r, 40000);
    CHECK(o.act == RTSS_DO_NOTHING && r.st.applies == 2, "once");
    r.movie_mhz = 0;
    o = step(&r, 40000);
    CHECK(o.act == RTSS_DO_APPLY && o.mhz == 40000 && !o.holds && r.st.holds == 40000 && r.st.applies == 3,
          "40 back at its end");
    o = step(&r, 40000);
    CHECK(o.holds, "held from the next poll");
    // at the movie's own rate (60, the default limit) the profile keeps 60 through the movie, with no write: RTSS holds
    // the movie beside the pacer, as rtss_target says
    run_init(&r);
    step(&r, 60000);
    step(&r, 60000);
    r.movie_mhz = 60000;
    for (int i = 0; i < 5; i++) o = step(&r, 60000);
    CHECK(o.act == RTSS_DO_NOTHING && !o.holds && o.why == RTSS_WHY_MOVIE && r.st.applies == 1 && r.st.holds == 60000,
          "a movie at 60: no write, the profile keeps 60");
    r.movie_mhz = 0;
    o = step(&r, 60000);
    CHECK(o.holds && r.st.applies == 1, "held again after it, no write");
    CHECK(rtss_target(60000, 60000) == 60000 && rtss_target(59940, 60000) == 0 && rtss_target(0, 60000) == 0 &&
              rtss_target(144000, 0) == 144000 && rtss_target(RTSS_OUT_LOWLAT, 60000) == RTSS_OUT_LOWLAT,
          "rtss_target");
}

// No want yet: nothing at all. The hooks library that does not load fails the run. (A run whose exit put the profile
// back makes no decision: rtss_limit.c worker skips the poll, which tests/rtss_worker_check.c's restore_* check.)
static void unset_and_library(void) {
    Run r;
    run_init(&r);
    RtssOut o = step(&r, RTSS_UNSET);
    CHECK(o.why == RTSS_WHY_UNSET && o.act == RTSS_DO_NOTHING && !r.st.loaded, "no want");
    run_init(&r);
    r.st.no_library = 1;
    o = step(&r, 60000);
    CHECK(o.why == RTSS_WHY_FAILED && r.s.fail == RTSS_FAIL_LIBRARY && !r.st.applies, "the library did not load");
    WHY(o, r, "RivaTuner Statistics Server's hooks library did not load");
    r.st.no_library = 0;
    o = step(&r, 60000);
    CHECK(o.why == RTSS_WHY_FAILED && !r.st.loaded, "not tried again");
    // not running: the library is not wanted until RTSS runs
    run_init(&r);
    r.running = 0;
    o = step(&r, 60000);
    CHECK(!r.st.loaded && o.why == RTSS_WHY_NOT_RUNNING, "not loaded while RTSS does not run");
    WHY(o, r, "RivaTuner Statistics Server is not running");
}

// Two limiters: RTSS out of the game's limit (kept out, or a profile that cannot be written or a hooks library that did
// not load), listed and a limit of RTSS's own; never for a limit the game hands RTSS.
static void two(void) {
    CHECK(rtss_two(RTSS_OUT_SETTING, 0, 1, 1) && rtss_two(RTSS_OUT_FRAMEGEN, 0, 1, 1) &&
              rtss_two(RTSS_OUT_LOWLAT, 0, 1, 1),
          "every reason");
    CHECK(!rtss_two(RTSS_OUT_SETTING, 0, 0, 1), "not listed: RTSS holds nothing");
    CHECK(!rtss_two(RTSS_OUT_SETTING, 0, 1, 0), "no limit of its own");
    CHECK(!rtss_two(60000, 0, 1, 1) && !rtss_two(0, 0, 1, 1), "the game's limit in RTSS: one limiter");
    CHECK(!rtss_two(RTSS_UNSET, 0, 1, 1) && !rtss_two(RTSS_UNSET, RTSS_FAIL_READONLY, 1, 1), "no want yet");
    CHECK(rtss_two(60000, RTSS_FAIL_READONLY, 1, 1) && rtss_two(0, RTSS_FAIL_LIBRARY, 1, 1),
          "out for the run: the profile cannot be written, or the hooks library did not load");
    CHECK(!rtss_two(60000, RTSS_FAIL_READONLY, 0, 1) && !rtss_two(60000, RTSS_FAIL_LIBRARY, 1, 0),
          "out for the run, but not listed or no limit of its own");
    CHECK(!rtss_two(60000, RTSS_FAIL_NOT_LIMITING, 1, 1), "RTSS seen not to hold the game: not counted");
}

// The period the game's pacer keeps beside RTSS: the ceiling while RTSS holds and host presents go out, the limit's own
// otherwise; never 0.
static void pace_period(void) {
    const uint64_t p60 = 16666667, ceiling = 4166667, now = 50000000000ull;
    CHECK(rtss_pace_period(1, 0, now, now - 10000000, p60, ceiling) == ceiling, "holds, a host present 10 ms ago");
    CHECK(rtss_pace_period(1, 0, now, now - RTSS_HOLD_IDLE_NS + 1, p60, ceiling) == ceiling, "just under 200 ms");
    CHECK(rtss_pace_period(1, 0, now, now - RTSS_HOLD_IDLE_NS, p60, ceiling) == p60, "200 ms with no host present");
    CHECK(rtss_pace_period(1, 0, now, 0, p60, ceiling) == p60, "no host present yet");
    CHECK(rtss_pace_period(0, 0, now, now - 10000000, p60, ceiling) == p60, "RTSS does not hold");
    CHECK(rtss_pace_period(1, 0, now, now + 1000, p60, ceiling) == ceiling, "a host present stored after now was read");
    CHECK(rtss_pace_period(1, 0, now, now - 10000000, ceiling, ceiling) == ceiling, "a limit at the ceiling");
    CHECK(rtss_pace_period(1, 60, now, now - 10000000, p60, ceiling) == p60, "a movie: its period and slots");
}

// The state file (rtss-profile.txt): its lines, the file's text as the worker rewrites it, and what a start does with a
// line that a run which did not close left behind.
static void saved(void) {
    RtssSaved a = {0, 0, 1, 60, 1, 60, 1, "ocarina.exe"}, r;
    char line[400], out[2048];
    size_t n = rtss_saved_format(line, sizeof line, &a);
    CHECK(n == strlen(line) && !strcmp(line, "rtss1 0 0 1 60 1 60 1 ocarina.exe\n"), "the line: %s", line);
    CHECK(rtss_saved_parse(line, n - 1, &r) && !r.existed && !r.old_limit && r.old_den == 1 && r.limit == 60 &&
              r.den == 1 && r.prev_limit == 60 && r.prev_den == 1 && !strcmp(r.name, "ocarina.exe"),
          "read back");
    const char *spaced = "rtss1 1 59940 1000 144 1 60 1 My Game.exe\r";
    CHECK(rtss_saved_parse(spaced, strlen(spaced), &r) && r.existed && r.old_limit == 59940 && r.old_den == 1000 &&
              r.limit == 144 && r.prev_limit == 60 && !strcmp(r.name, "My Game.exe"),
          "a name with a space, a line ending in CR");
    static const char *bad[] = {"rtss1 2 0 1 60 1 60 1 x.exe", "rtss1 0 0 0 60 1 60 1 x.exe",
                                "rtss1 0 0 1 60 0 60 1 x.exe", "rtss1 0 0 1 60 1 60 0 x.exe",
                                "rtss1 0 0 1 60 1 60 1",       "rtss1 0 0 1 60 1 60 1 ",
                                "rtss2 0 0 1 60 1 60 1 x.exe", "rtss1 0 0 1 x 1 60 1 x.exe",
                                "rtss1 0 0 1 60 1 x.exe",      "",
                                "# rtss1 0 0 1 60 1 60 1 x.exe"};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++)
        CHECK(!rtss_saved_parse(bad[i], strlen(bad[i]), &r), "not a line: '%s'", bad[i]);
    CHECK(!rtss_saved_format(line, 20, &a), "a line that does not fit");

    // found past a comment, a malformed line and another profile's line, whatever the case of its letters
    const char *text = RTSS_SAVED_HEADER
        "rtss1 9 garbage\nrtss1 1 30 1 144 1 144 1 My Game.exe\nrtss1 0 0 1 60 1 60 1 OCARINA.EXE";
    CHECK(rtss_saved_find(text, "ocarina.exe", &r) && r.limit == 60 && !r.existed, "ours, the last line, no newline");
    CHECK(rtss_saved_find(text, "my game.exe", &r) && r.limit == 144 && r.old_limit == 30, "the other one");
    CHECK(!rtss_saved_find(text, "ocarina_dev.exe", &r) && !rtss_saved_find("", "ocarina.exe", &r), "none");

    // rewritten: ours replaced, the other kept, the malformed line and comments dropped, the header first
    int records = -1;
    RtssSaved a2 = a;
    a2.limit = 144;
    n = rtss_saved_update(text, "ocarina.exe", &a2, out, sizeof out, &records);
    CHECK(n == strlen(out) && records == 2 &&
              !strcmp(out, RTSS_SAVED_HEADER
                      "rtss1 1 30 1 144 1 144 1 My Game.exe\nrtss1 0 0 1 144 1 60 1 ocarina.exe\n"),
          "replaced:\n%s", out);
    char again[2048];
    n = rtss_saved_update(out, "OCARINA.exe", NULL, again, sizeof again, &records);
    CHECK(records == 1 && !strcmp(again, RTSS_SAVED_HEADER "rtss1 1 30 1 144 1 144 1 My Game.exe\n"), "removed:\n%s",
          again);
    n = rtss_saved_update(again, "My Game.exe", NULL, out, sizeof out, &records);
    CHECK(n && !records, "the last line removed: no record left, the file goes");
    n = rtss_saved_update("", "ocarina.exe", &a, out, sizeof out, &records);
    CHECK(records == 1 && !strcmp(out, RTSS_SAVED_HEADER "rtss1 0 0 1 60 1 60 1 ocarina.exe\n"), "a new file:\n%s",
          out);
    CHECK(!rtss_saved_update(text, "ocarina.exe", &a, out, sizeof RTSS_SAVED_HEADER + 10, &records),
          "a text that does not fit");

    // What a start does. Run A made the profile and wrote 60/1, and was killed: B finds 60/1 and removes the profile.
    RtssSaved made = {0, 0, 1, 60, 1, 60, 1, "ocarina.exe"}, had = {1, 30, 1, 60, 1, 60, 1, "ocarina.exe"};
    CHECK(rtss_restore_plan(&made, 1, 60, 1) == RTSS_PLAN_DELETE, "the run's own profile: removed");
    CHECK(rtss_restore_plan(&made, 1, 75, 1) == RTSS_PLAN_LEAVE, "the player set 75 since: theirs");
    CHECK(rtss_restore_plan(&made, 0, 0, 1) == RTSS_PLAN_NONE, "the player removed it, or the run was killed before "
                                                               "its first write: nothing to do");
    CHECK(rtss_restore_plan(&had, 1, 60, 1) == RTSS_PLAN_WRITE, "the player's profile with 30/1 before: 30/1 back");
    CHECK(rtss_restore_plan(&had, 1, 30, 1) == RTSS_PLAN_LEAVE, "killed before the write landed: the player's 30/1");
    CHECK(rtss_restore_plan(&had, 0, 0, 1) == RTSS_PLAN_LEAVE, "the player removed their profile since: left so");
    CHECK(rtss_restore_plan(&had, 1, 60, 0) == RTSS_PLAN_WRITE, "an RTSS without the denominator reads none: 1");
    RtssSaved frac = {1, 0, 1, 59940, 1000, 59940, 1000, "ocarina.exe"};
    CHECK(rtss_restore_plan(&frac, 1, 59940, 1) == RTSS_PLAN_LEAVE && rtss_restore_plan(&frac, 1, 59940, 1000) ==
                                                                          RTSS_PLAN_WRITE,
          "the denominator is compared too");
    // A run that wrote 60/1 and then 144/1 ended between the line and the profile's change, or after it: the profile
    // holds 60/1 or 144/1, both the game's; anything else is the player's.
    RtssSaved moving = {1, 30, 1, 144, 1, 60, 1, "ocarina.exe"}, moving_made = {0, 0, 1, 144, 1, 60, 1, "ocarina.exe"};
    CHECK(rtss_restore_plan(&moving, 1, 60, 1) == RTSS_PLAN_WRITE && rtss_restore_plan(&moving, 1, 144, 1) ==
                                                                          RTSS_PLAN_WRITE,
          "either value of the game's: 30/1 back");
    CHECK(rtss_restore_plan(&moving_made, 1, 60, 1) == RTSS_PLAN_DELETE &&
              rtss_restore_plan(&moving_made, 1, 144, 1) == RTSS_PLAN_DELETE,
          "either value of the game's, in a profile the run made: removed");
    CHECK(rtss_restore_plan(&moving, 1, 75, 1) == RTSS_PLAN_LEAVE && rtss_restore_plan(&moving, 1, 60, 1000) ==
                                                                         RTSS_PLAN_LEAVE,
          "another value: the player's");
}

// The profile's values for a limit (T15): whole rates as they are, a fraction over 1000 where RTSS knows the
// denominator, else rounded down, so "display" at 59.94 Hz never holds the game above the refresh.
static void limit_value(void) {
    static const struct { int64_t mhz; int has_den; uint32_t limit, den; } cases[] = {
        {59940, 0, 59, 1},   {59940, 1, 59940, 1000}, {60000, 0, 60, 1},  {60000, 1, 60, 1},
        {143856, 0, 143, 1}, {143856, 1, 143856, 1000}, {0, 0, 0, 1},     {240000, 1, 240, 1}};
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        uint32_t l = 7, d = 7;
        rtss_limit_value(cases[i].mhz, cases[i].has_den, &l, &d);
        CHECK(l == cases[i].limit && d == cases[i].den, "%lld mHz, denominator %d: %u/%u, not %u/%u",
              (long long)cases[i].mhz, cases[i].has_den, l, d, cases[i].limit, cases[i].den);
    }
}

int main(void) {
    settings();
    reasons();
    parse();
    injection_delay();
    quit();
    readonly();
    refused();
    frame_count();
    unproven();
    idle();
    kept_out();
    churn();
    movie();
    unset_and_library();
    two();
    pace_period();
    saved();
    limit_value();
    printf("rtss_limit_check: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
