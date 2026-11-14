// runtime/vk_tex_release.h: how long a released texture is kept, the bytes the released ones may hold, and the clock
// that turns presents into seconds. Built and run by tests/tex_release_check.py; prints each failed check and a table
// of the simulated sessions, and exits 1 on any failure.
//
// The simulation follows vk_texture.c sweep_pressure: once a second (the cache checks every 8 submissions) take-backs
// that fall due find their entry or make it again, drop_aged takes the entries past their hold, and drop_stale takes
// the oldest entries released 2 s ago or earlier while the released bytes are above trl_keep. The streams are shaped
// by the figures of three sessions' logs on an RTX 5090 (vk_tex_release.h).
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
    CHECK(trl_hold(TRL_HOLD_S, TRL_STORAGE) == 60 && trl_hold(TRL_HOLD_S, TRL_GPU) == 120, "holds 60 and 120 s");
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
    // the default bound by the card's budget (MiB): a thirty-second of it, 384 MiB to 1 GiB
    CHECK(trl_cap_mb(0) == TRL_CAP_UNKNOWN_MB, "no budget known: %u", trl_cap_mb(0));
    CHECK(trl_cap_mb(31419) == 981, "RTX 5090 (budget 31419 MiB): %u", trl_cap_mb(31419));
    CHECK(trl_cap_mb(15450) == 482, "a 16 GiB card (budget 15450 MiB): %u", trl_cap_mb(15450));
    CHECK(trl_cap_mb(7680) == TRL_CAP_MIN_MB && trl_cap_mb(3900) == TRL_CAP_MIN_MB, "8 and 4 GiB cards: the least");
    CHECK(trl_cap_mb(48000) == TRL_CAP_MAX_MB && trl_cap_mb(UINT64_MAX) == TRL_CAP_MAX_MB, "a larger card: the most");
    uint32_t last = 0;
    for (uint64_t b = 1; b <= 65536; b += 97) {
        uint32_t c = trl_cap_mb(b);
        CHECK(c >= last && c >= TRL_CAP_MIN_MB && c <= TRL_CAP_MAX_MB, "budget %llu MiB: bound %u after %u",
              (unsigned long long)b, c, last);
        last = c;
    }
    // what the released textures may hold at their fullest: under 2 GiB on a 32 GiB card with room (8 GiB by the
    // rule before, a quarter of the budget and twice that), 384 MiB on an 8 GiB card in play (1.9 GiB before)
    CHECK(trl_keep(TRL_HOLD_S, MB(trl_cap_mb(31419)), 1) < MB(2048), "the most on a 32 GiB card");
    CHECK(trl_keep(TRL_HOLD_S, MB(trl_cap_mb(7680)), 0) == MB(384), "the most on an 8 GiB card in play");
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
// The run through Hyrule Field that set the bound (2026-11-14, the rule before: 180 s, 4008 MiB and twice that with
// room): MiB and textures taken back by the time released, 2-10 s, 10-60 s, later; and the textures loaded anew
// meanwhile.
static const uint64_t FIELD_BACK_MB[3] = {1494, 171, 10};
static const uint64_t FIELD_BACK_N[3] = {941, 331, 28}, FIELD_UPLOADS = 33118;

static void taken_back_checks(void) {
    uint32_t longest = 0;
    for (size_t i = 0; i < sizeof TAKEN / sizeof *TAKEN; i++)
        for (int b = 0; b < 5; b++)
            if (TAKEN[i].n[b] && EDGE[b] > longest) longest = EDGE[b];
    CHECK(longest == 60, "the longest take-back of the logs lies under %u s", longest);
    // nothing those logs took back would have gone past the hold: every take-back came before 60 s
    for (int cls = TRL_STORAGE; cls <= TRL_GPU; cls++)
        CHECK(!trl_aged(TRL_HOLD_S, cls, longest - 1) && trl_hold(TRL_HOLD_S, cls) >= longest,
              "class %d: hold %u s against take-backs under %u s", cls, trl_hold(TRL_HOLD_S, cls), longest);
    // the field run: what came back later than the hold is under a hundredth of what came back at all, and everything
    // the cache gave back in three minutes is a twenty-fifth of what the game loaded anew in that time
    uint64_t all_mb = FIELD_BACK_MB[0] + FIELD_BACK_MB[1] + FIELD_BACK_MB[2];
    uint64_t all_n = FIELD_BACK_N[0] + FIELD_BACK_N[1] + FIELD_BACK_N[2];
    CHECK(FIELD_BACK_MB[2] * 100 < all_mb, "field run: %llu of %llu MiB came back past a minute",
          (unsigned long long)FIELD_BACK_MB[2], (unsigned long long)all_mb);
    CHECK(all_n * 20 < FIELD_UPLOADS, "field run: %llu taken back beside %llu loaded", (unsigned long long)all_n,
          (unsigned long long)FIELD_UPLOADS);
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
    uint64_t remade, remade_bytes, remade_quick, dropped_age, dropped_bound, peak, end, at_burst_end;
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
            if (e->gone) r.remade++, r.remade_bytes += e->bytes, r.remade_quick += e->back - e->rel < 10;
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
    // RTX 5090, 31 GiB budget: until 0.10.2 a quarter of it (4008 MiB, the logs' figure) and 180 s; now trl_cap_mb
    const Policy was = {"until 0.10.2", MB(4008), 180}, now = {"0.10.2", MB(trl_cap_mb(31419)), TRL_HOLD_S};
    // an 8 GiB card: no room in play
    const Policy was8 = {"until 0.10.2", MB(1920), 180}, now8 = {"0.10.2", MB(trl_cap_mb(7680)), TRL_HOLD_S};
    struct { const char *name; uint32_t burst_s; double burst_mbs; uint32_t end_s; double calm_mbs; } sc[] = {
        // the Balanced session's second line: 3.2 GiB released and kept within 35 s; then 10 minutes in one area
        {"area changes 70 s, calm 10 min", 70, 93.0, 670, 1.0},
        // a long session: an area change every few minutes, shown as a slower stream throughout
        {"steady 20 MiB/s for 20 min", 1200, 20.0, 1500, 0.5},
        // a teleport tour: 4 minutes at the burst rate
        {"teleports 4 min, calm 10 min", 240, 93.0, 840, 1.0},
        // the run through Hyrule Field: the game lets go of about 130 MiB a second for three minutes
        {"field run 3 min, calm 2 min", 170, 130.0, 290, 1.0},
    };
    printf("simulated sessions (made again: taken back after its drop; MiB: own memory of released textures)\n");
    for (size_t i = 0; i < sizeof sc / sizeof *sc; i++) {
        rng_state = 0x9e3779b97f4a7c15ull + i;
        make_stream(sc[i].burst_s, sc[i].burst_mbs, sc[i].end_s, sc[i].calm_mbs, 20, 0);
        Result rn = run(&now, 1, sc[i].burst_s, sc[i].end_s), rw = run(&was, 1, sc[i].burst_s, sc[i].end_s);
        Result rn8 = run(&now8, 0, sc[i].burst_s, sc[i].end_s), rw8 = run(&was8, 0, sc[i].burst_s, sc[i].end_s);
        print_result(sc[i].name, &was, 1, &rw), print_result("", &now, 1, &rn);
        print_result("  same on an 8 GiB card", &was8, 0, &rw8), print_result("", &now8, 0, &rn8);
        uint64_t total = 0, came_back = 0;
        for (uint32_t k = 0; k < nents; k++) total += ents[k].bytes, came_back += ents[k].back ? ents[k].bytes : 0;
        // What the cache holds at its fullest is what the session costs in video memory: never more than what it may
        // keep and what two seconds release (drop_stale leaves the entries of the last two seconds), on either card
        uint64_t slack = MB((uint64_t)(2 * sc[i].burst_mbs) + 4);
        CHECK(rn.peak <= trl_keep(now.hold, now.cap, 1) + slack, "%s: holds %.0f MiB at its fullest", sc[i].name,
              rn.peak / 1048576.0);
        CHECK(rn8.peak <= trl_keep(now8.hold, now8.cap, 0) + slack, "%s, 8 GiB card: holds %.0f MiB at its fullest",
              sc[i].name, rn8.peak / 1048576.0);
        // and never more than the rule before held; where that one filled up, less than half of it
        CHECK(rn.peak <= rw.peak && rn8.peak <= rw8.peak, "%s: peak %.0f/%.0f MiB against %.0f/%.0f before", sc[i].name,
              rn.peak / 1048576.0, rn8.peak / 1048576.0, rw.peak / 1048576.0, rw8.peak / 1048576.0);
        if (rw.peak > MB(4096))
            CHECK(rn.peak * 2 < rw.peak, "%s: peak %.0f MiB, %.0f before", sc[i].name, rn.peak / 1048576.0,
                  rw.peak / 1048576.0);
        // what it holds once play is calm is what the last hold released
        CHECK(rn.end <= rw.end && rn.end <= MB((uint64_t)(sc[i].calm_mbs * 2 * TRL_HOLD_S) + 64),
              "%s: holds %.0f MiB at the end, %.0f before", sc[i].name, rn.end / 1048576.0, rw.end / 1048576.0);
        // A take-back within ten seconds of the release (turning round) finds its texture wherever ten seconds of
        // releases fit in what the cache may keep; on the 32 GiB card that is every scenario here
        if ((uint64_t)(sc[i].burst_mbs * 12) < (trl_keep(now.hold, now.cap, 1) >> 20))
            CHECK(rn.remade_quick == 0, "%s: %llu quick take-backs made again", sc[i].name,
                  (unsigned long long)rn.remade_quick);
        if ((uint64_t)(sc[i].burst_mbs * 12) < (trl_keep(now8.hold, now8.cap, 0) >> 20))
            CHECK(rn8.remade_quick == 0, "%s, 8 GiB card: %llu quick take-backs made again", sc[i].name,
                  (unsigned long long)rn8.remade_quick);
        // and what it makes again is never more than what the game takes back at all (a fiftieth of the stream here)
        CHECK(rn.remade_bytes <= came_back && rn8.remade_bytes <= came_back && came_back * 40 < total,
              "%s: made again %.0f/%.0f MiB of %.0f MiB that came back", sc[i].name, rn.remade_bytes / 1048576.0,
              rn8.remade_bytes / 1048576.0, came_back / 1048576.0);
    }
    // a game that takes textures back late, in calm play where the bound is far: at 50 s nothing goes before (the
    // hold is 60 s), at 90 s the hold has dropped them
    rng_state = 7;
    make_stream(0, 0.0, 400, 5.0, 20, 50);
    Result r50 = run(&now, 1, 0, 400);
    CHECK(r50.remade == 0 && r50.dropped_bound == 0, "take-backs at 50 s: made %llu again, %llu dropped by the bound",
          (unsigned long long)r50.remade, (unsigned long long)r50.dropped_bound);
    rng_state = 7;
    make_stream(0, 0.0, 400, 5.0, 20, 90);
    Result r90 = run(&now, 1, 0, 400);
    CHECK(r90.remade > 0, "take-backs at 90 s: the hold should have dropped some first");
    printf("  late take-backs in calm play: at 50 s made again %llu, at 90 s %llu (the hold is %u s)\n",
           (unsigned long long)r50.remade, (unsigned long long)r90.remade, TRL_HOLD_S);
}

// ---- past the hard count (trl_hard_class, trl_hard_pick)
static void hard_checks(void) {
    CHECK(trl_hard_class(1, 1, 0, 0) == TRL_HC_OVER, "an overwritten entry goes first, sampled or not");
    CHECK(trl_hard_class(0, 1, 1, 1) == TRL_HC_LATELY, "a sampled entry is kept");
    CHECK(trl_hard_class(0, 0, 0, 0) == TRL_HC_GPU, "GPU-made content");
    CHECK(trl_hard_class(0, 0, 1, 1) == TRL_HC_INTACT_OLD && trl_hard_class(0, 0, 1, 0) == TRL_HC_INTACT, "intact");
    // the session's end: release order mixes render targets (GPU), intact entries and a few overwritten ones; the
    // render targets are the oldest. The rule before took them first.
    enum { N = 64 };
    uint8_t cls[N];
    int out[N];
    for (int i = 0; i < N; i++)
        cls[i] = i < 16 ? TRL_HC_GPU : i % 9 == 0 ? TRL_HC_OVER : i % 3 == 0 ? TRL_HC_LATELY : i < 40 ? TRL_HC_INTACT_OLD
               : TRL_HC_INTACT;
    int k = trl_hard_pick(cls, N, 20, out);
    CHECK(k == 20, "picked %d of 20", k);
    int last = -1, gpu = 0, prev_i = -1;
    for (int j = 0; j < k; j++) {
        int c = cls[out[j]];
        CHECK(c != TRL_HC_LATELY, "a lately sampled entry was picked (%d)", out[j]);
        CHECK(c >= last, "class order: %d after %d", c, last);
        if (c == last) CHECK(out[j] > prev_i, "release order within a class");
        gpu += c == TRL_HC_GPU;
        last = c, prev_i = out[j];
    }
    CHECK(gpu == 0, "%d render targets went while intact entries were left", gpu);
    // only render targets and sampled ones: the render targets go, in release order; none unsampled: nothing
    for (int i = 0; i < N; i++) cls[i] = i % 2 ? TRL_HC_GPU : TRL_HC_LATELY;
    k = trl_hard_pick(cls, N, 5, out);
    CHECK(k == 5 && out[0] == 1 && out[4] == 9, "render targets in release order: %d, %d..%d", k, out[0], out[4]);
    for (int i = 0; i < N; i++) cls[i] = TRL_HC_LATELY;
    CHECK(trl_hard_pick(cls, N, 5, out) == 0, "all sampled lately: none picked");
}

int main(void) {
    clock_checks();
    rule_checks();
    taken_back_checks();
    session_checks();
    hard_checks();
    if (fails) printf("%d checks failed\n", fails);
    else printf("tex_release_check: all checks passed\n");
    return fails ? 1 : 0;
}
