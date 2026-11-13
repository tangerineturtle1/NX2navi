// runtime/vk_tex_release.h: how long a released texture is kept, the bytes the released ones may hold, and the clock
// that turns presents into seconds. Built and run by tests/tex_release_check.py; prints each failed check and a table
// of the simulated sessions, and exits 1 on any failure.
//
// The simulation follows vk_texture.c sweep_pressure: once a second (the cache checks every 8 submissions) take-backs
// that fall due find their entry or make it again, drop_aged takes the entries past their hold, and drop_stale takes
// the oldest entries released 2 s ago or earlier while the released bytes are above trl_keep. The streams are shaped
// by the figures of two sessions' logs on an RTX 5090 (vk_tex_release.h).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk_tex_release.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MB(n) ((uint64_t)(n) << 20)

static uint64_t rng_state = 0x243f6a8885a308d3ull;
static uint32_t rnd(uint32_t n) {
    rng_state ^= rng_state << 13, rng_state ^= rng_state >> 7, rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 33) % n;
}

// ---- the clock
static TrlClock clk;

static void clock_checks(void) {
    // presents at a rate that changes every few seconds (60 to 240 a second); a tick each second, as rate_tick makes
    memset(&clk, 0, sizeof clk);
    uint64_t frame = 0, at[4000];   // at[s]: the present count at second s
    for (uint32_t s = 0; s < 4000; s++) {
        at[s] = frame;
        trl_clock_tick(&clk, frame, s);
        frame += 60 + (s / 7 % 4) * 60;
    }
    uint32_t now = 3999;
    for (uint32_t s = 0; s < 4000; s += 13) {
        uint32_t age = trl_clock_age(&clk, at[s] + 1, now), real = now - s;   // a present just after tick s
        if (now - s < TRL_CLOCK_N - 1) {
            CHECK(age <= real && age + 1 >= real, "present after second %u: %u s old, %u by the clock", s, real, age);
        } else {
            CHECK(age >= TRL_CLOCK_N - 2, "present after second %u, before the ticks kept: %u by the clock", s, age);
        }
    }
    CHECK(trl_clock_age(&clk, frame + 5, now) == 0, "a present after the last tick is 0 s old");
    CHECK(trl_clock_age(&clk, at[3999], now) == 0, "the last tick's present is 0 s old");
    // the hold in seconds does not depend on the rate: 180 s at 60 or at 240 presents a second
    uint32_t s60 = 2500, s240 = 0;   // within the ticks kept (the last TRL_CLOCK_N seconds)
    for (uint32_t s = 2500; s < 4000; s++)
        if ((s / 7 % 4) == 3) { s240 = s; break; }
    CHECK(trl_clock_age(&clk, at[s60], s60 + 180) == 180 && trl_clock_age(&clk, at[s240], s240 + 180) == 180,
          "180 s are 180 s at any rate");
    // a run shorter than the ring, and an empty clock
    TrlClock c2 = {0};
    CHECK(trl_clock_age(&c2, 100, 50) == 0, "no tick yet: 0 s");
    trl_clock_tick(&c2, 0, 0), trl_clock_tick(&c2, 100, 1), trl_clock_tick(&c2, 250, 2);
    CHECK(trl_clock_age(&c2, 50, 10) == 9 && trl_clock_age(&c2, 0, 10) == 10 && trl_clock_age(&c2, 251, 10) == 0,
          "short run: %u %u %u", trl_clock_age(&c2, 50, 10), trl_clock_age(&c2, 0, 10), trl_clock_age(&c2, 251, 10));
}

// ---- the rules
static void rule_checks(void) {
    CHECK(trl_hold(TRL_HOLD_S, TRL_STORAGE) == 180 && trl_hold(TRL_HOLD_S, TRL_GPU) == 360, "holds 180 and 360 s");
    CHECK(!trl_aged(0, TRL_STORAGE, 100000), "no hold: nothing ages out");
    CHECK(!trl_aged(180, TRL_STORAGE, 179) && trl_aged(180, TRL_STORAGE, 180), "the hold's edge");
    CHECK(!trl_aged(180, TRL_GPU, 359) && trl_aged(180, TRL_GPU, 360), "GPU content waits twice the hold");
    CHECK(2 * TRL_HOLD_MAX_S < TRL_CLOCK_N - 1, "the clock covers the longest hold of GPU content");
    uint64_t budget = MB(31 * 1024);
    // the Balanced session's line at frame 7200: 11786 MiB in use of about 31 GiB, 27.3 GB of 71 GB commit free
    CHECK(trl_room(MB(11786), budget, 80, 70981 - 43683, 7098), "RTX 5090 session: room");
    CHECK(!trl_room(MB(23000), budget, 80, 27000, 7098), "card above 70%% of its budget: no room");
    CHECK(!trl_room(MB(5000), budget, 80, 10000, 7098), "commit under twice its low mark: no room");
    CHECK(!trl_room(MB(5000), budget, 80, -1, 7098), "commit not tracked: no room");
    CHECK(!trl_room(MB(1000), 0, 80, 30000, 4096), "no budget known: no room");
    CHECK(!trl_room(MB(1), budget, 5, 30000, 4096), "pressure under the margin: no room");
    // an 8 GiB card in play: 6.3 of 7.5 GiB in use
    CHECK(!trl_room(MB(6450), MB(7680), 80, 20000, 4096), "8 GiB card in play: no room");
    CHECK(trl_keep(180, MB(4008), 1) == MB(8016) && trl_keep(180, MB(4008), 0) == MB(4008), "keep: twice with room");
    CHECK(trl_keep(0, MB(4008), 1) == MB(4008), "keep: the bound without the hold, as before");
    CHECK(trl_keep(180, UINT64_MAX, 1) == UINT64_MAX, "keep: no bound stays none");
}

// ---- the take-backs of the logs against the hold
// "vk: released textures frame N" lines of the two sessions (2026-11-12): take-backs by the time released, the buckets
// under 2 s, 2-10 s, 10-60 s, 1-5 min, longer, with each bucket's upper edge in seconds (the last open).
static const struct { const char *session; uint64_t n[5]; } TAKEN[] = {
    {"low, frame 3600", {46626, 18, 0, 0, 0}},     {"low, frame 7200", {115111, 126, 69, 0, 0}},
    {"low, frame 10800", {88575, 79, 156, 0, 0}}, {"balanced, frame 3600", {22034, 1, 0, 0, 0}},
    {"balanced, frame 7200", {120779, 91, 1, 0, 0}},
};
static const uint32_t EDGE[5] = {2, 10, 60, 300, UINT32_MAX};

static void taken_back_checks(void) {
    uint32_t longest = 0;
    for (size_t i = 0; i < sizeof TAKEN / sizeof *TAKEN; i++)
        for (int b = 0; b < 5; b++)
            if (TAKEN[i].n[b] && EDGE[b] > longest) longest = EDGE[b];
    CHECK(longest == 60, "the longest take-back of the logs lies under %u s", longest);
    // nothing the logs took back would have gone past the hold: every take-back came before 60 s, the hold is 180 s
    for (int cls = TRL_STORAGE; cls <= TRL_GPU; cls++)
        CHECK(!trl_aged(TRL_HOLD_S, cls, longest) && trl_hold(TRL_HOLD_S, cls) >= 3 * longest,
              "class %d: hold %u s against take-backs up to %u s", cls, trl_hold(TRL_HOLD_S, cls), longest);
}

// ---- simulated sessions
typedef struct {
    uint32_t rel;      // second of the release
    uint32_t back;     // second the game takes it back; 0: never
    uint32_t bytes;
    uint8_t cls, gone, taken;
} Ent;

typedef struct {
    const char *name;
    uint64_t cap;   // bytes
    uint32_t hold;  // s
} Policy;

typedef struct {
    uint64_t remade, remade_bytes, dropped_age, dropped_bound, peak, end, at_burst_end;
} Result;

#define MAXE 400000
static Ent ents[MAXE];
static uint32_t nents;
static uint32_t backs[MAXE], nbacks;   // the entries the game takes back, by the second it does

static int back_cmp(const void *a, const void *b) {
    uint32_t x = ents[*(const uint32_t *)a].back, y = ents[*(const uint32_t *)b].back;
    return x < y ? -1 : x > y;
}

// A stream: `burst_s` seconds of area changes at burst_mbs MiB/s of releases the game keeps released, then calm play to
// end_s at calm_mbs. Sizes 0.1 to 1.9 MiB (the Balanced session held 3946 MiB in 7754 entries, half a MiB each); a
// tenth hold content only the GPU made. A share `back_pct` per thousand comes back, after a time drawn from the
// logs' buckets past 2 s (2-10 s and 10-60 s about as often); `late` adds take-backs late_s after the release.
static void make_stream(uint32_t burst_s, double burst_mbs, uint32_t end_s, double calm_mbs, uint32_t back_pml,
                        uint32_t late_s) {
    nents = 0;
    double carry = 0;
    for (uint32_t s = 1; s < end_s; s++) {
        carry += (s <= burst_s ? burst_mbs : calm_mbs) * 1048576.0;
        while (carry > 0 && nents < MAXE) {
            Ent *e = &ents[nents++];
            memset(e, 0, sizeof *e);
            e->rel = s, e->bytes = (uint32_t)(MB(1) / 10 + rnd((uint32_t)MB(18) / 10));
            e->cls = rnd(10) == 0 ? TRL_GPU : TRL_STORAGE;
            if (rnd(1000) < back_pml) e->back = s + (rnd(2) ? 2 + rnd(8) : 10 + rnd(50));
            if (late_s && rnd(1000) < back_pml) e->back = s + late_s;
            carry -= e->bytes;
        }
    }
    nbacks = 0;
    for (uint32_t i = 0; i < nents; i++)
        if (ents[i].back) backs[nbacks++] = i;
    qsort(backs, nbacks, sizeof *backs, back_cmp);
}

static Result run(const Policy *p, int room, uint32_t burst_s, uint32_t end_s) {
    Result r = {0};
    for (uint32_t i = 0; i < nents; i++) ents[i].gone = ents[i].taken = 0;
    uint64_t held = 0;
    uint32_t next = 0, head = 0;   // next: the first entry not released yet; head: the oldest that may still be held
    uint32_t nb = 0;               // the next take-back in backs[]
    for (uint32_t s = 1; s < end_s; s++) {
        while (next < nents && ents[next].rel <= s) held += ents[next++].bytes;
        for (; nb < nbacks && ents[backs[nb]].back <= s; nb++) {   // the take-backs due now
            Ent *e = &ents[backs[nb]];
            if (e->gone) r.remade++, r.remade_bytes += e->bytes;
            else held -= e->bytes;
            e->taken = 1;
        }
        for (uint32_t i = head; p->hold && i < next; i++) {   // drop_aged
            Ent *e = &ents[i];
            if (e->gone || e->taken) continue;
            if (s - e->rel < p->hold) break;   // in release order: the rest is younger
            if (trl_aged(p->hold, e->cls, s - e->rel)) e->gone = 1, held -= e->bytes, r.dropped_age++;
        }
        uint64_t keep = trl_keep(p->hold, p->cap, room);
        for (uint32_t i = head; i < next && held > keep; i++) {   // drop_stale, oldest first, released 2 s ago or more
            Ent *e = &ents[i];
            if (e->gone || e->taken || s - e->rel < 2) continue;
            e->gone = 1, held -= e->bytes, r.dropped_bound++;
        }
        while (head < next && (ents[head].gone || ents[head].taken)) head++;
        if (held > r.peak) r.peak = held;
        if (s == burst_s) r.at_burst_end = held;
    }
    r.end = held;
    return r;
}

static void print_result(const char *scenario, const Policy *p, int room, const Result *r) {
    printf("  %-34s %-12s room %d: made again %5llu (%6.0f MiB), dropped by age %6llu, by the bound %6llu; held at "
           "the burst's end %5.0f MiB, peak %5.0f MiB, at the end %5.0f MiB\n",
           scenario, p->name, room, (unsigned long long)r->remade, r->remade_bytes / 1048576.0,
           (unsigned long long)r->dropped_age, (unsigned long long)r->dropped_bound, r->at_burst_end / 1048576.0,
           r->peak / 1048576.0, r->end / 1048576.0);
}

static void session_checks(void) {
    // RTX 5090, 31 GiB budget: the size bound a quarter of it (4008 MiB, the logs' figure)
    const Policy old_low = {"old low", MB(1024), 0}, balanced = {"old balanced", MB(4008), 0},
                 low = {"new low", MB(4008), TRL_HOLD_S};
    // an 8 GiB card: bound 1.9 GiB, no room in play
    const Policy b8 = {"old balanced", MB(1920), 0}, l8 = {"new low", MB(1920), TRL_HOLD_S},
                 o8 = {"old low", MB(1024), 0};
    struct { const char *name; uint32_t burst_s; double burst_mbs; uint32_t end_s; double calm_mbs; } sc[] = {
        // the Balanced session's second line: 3.2 GiB released and kept within 35 s; then 10 minutes in one area
        {"area changes 70 s, calm 10 min", 70, 93.0, 670, 1.0},
        // a long session: an area change every few minutes, shown as a slower stream throughout
        {"steady 20 MiB/s for 20 min", 1200, 20.0, 1500, 0.5},
        // a teleport tour: 4 minutes at the burst rate, past twice the bound
        {"teleports 4 min, calm 10 min", 240, 93.0, 840, 1.0},
    };
    printf("simulated sessions (made again: taken back after its drop; MiB: own memory of released textures)\n");
    for (size_t i = 0; i < sizeof sc / sizeof *sc; i++) {
        rng_state = 0x9e3779b97f4a7c15ull + i;
        make_stream(sc[i].burst_s, sc[i].burst_mbs, sc[i].end_s, sc[i].calm_mbs, 20, 0);
        Result rl = run(&low, 1, sc[i].burst_s, sc[i].end_s), rb = run(&balanced, 1, sc[i].burst_s, sc[i].end_s);
        Result ro = run(&old_low, 1, sc[i].burst_s, sc[i].end_s);
        Result rl8 = run(&l8, 0, sc[i].burst_s, sc[i].end_s), rb8 = run(&b8, 0, sc[i].burst_s, sc[i].end_s);
        Result ro8 = run(&o8, 0, sc[i].burst_s, sc[i].end_s);
        print_result(sc[i].name, &old_low, 1, &ro), print_result("", &balanced, 1, &rb), print_result("", &low, 1, &rl);
        print_result("  same on an 8 GiB card", &o8, 0, &ro8), print_result("", &b8, 0, &rb8);
        print_result("", &l8, 0, &rl8);
        // with room the new default makes nothing again, unless a burst passes twice the bound within the hold
        uint64_t total = 0;
        for (uint32_t k = 0; k < nents && ents[k].rel <= sc[i].burst_s; k++) total += ents[k].bytes;
        if (total < 2 * low.cap - MB(256))
            CHECK(rl.remade == 0, "%s: new low made %llu again", sc[i].name, (unsigned long long)rl.remade);
        // never more made again than the old Balanced, on either card
        CHECK(rl.remade <= rb.remade && rl8.remade <= rb8.remade, "%s: new low %llu/%llu against balanced %llu/%llu",
              sc[i].name, (unsigned long long)rl.remade, (unsigned long long)rl8.remade, (unsigned long long)rb.remade,
              (unsigned long long)rb8.remade);
        // what it holds at the end: what the last hold released, far under the old Balanced's
        CHECK(rl.end <= rb.end / 2 || rb.end < MB(256), "%s: new low holds %.0f MiB at the end, balanced %.0f",
              sc[i].name, rl.end / 1048576.0, rb.end / 1048576.0);
        // the old low's 1 GiB made textures again in every burst, as the log showed
        CHECK(ro.remade > 0, "%s: old low made nothing again", sc[i].name);
        CHECK(!rl.dropped_bound || total >= 2 * low.cap - MB(256), "%s: new low dropped %llu by the bound with room",
              sc[i].name, (unsigned long long)rl.dropped_bound);
    }
    // a game that takes textures back late: at 150 s nothing goes before (hold 180), at 200 s the hold costs
    rng_state = 7;
    make_stream(70, 93.0, 670, 1.0, 20, 150);
    Result r150 = run(&low, 1, 70, 670);
    CHECK(r150.remade == 0, "take-backs at 150 s: new low made %llu again", (unsigned long long)r150.remade);
    rng_state = 7;
    make_stream(70, 93.0, 670, 1.0, 20, 200);
    Result r200 = run(&low, 1, 70, 670);
    CHECK(r200.remade > 0, "take-backs at 200 s: the hold should have dropped some first");
    printf("  late take-backs: at 150 s made again %llu, at 200 s %llu (the hold is %u s)\n",
           (unsigned long long)r150.remade, (unsigned long long)r200.remade, TRL_HOLD_S);
}

int main(void) {
    clock_checks();
    rule_checks();
    taken_back_checks();
    session_checks();
    if (fails) printf("%d checks failed\n", fails);
    else printf("tex_release_check: all checks passed\n");
    return fails ? 1 : 0;
}
