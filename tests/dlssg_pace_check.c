// runtime/dlssg_pace.h, who holds the frame limit with DLSS Frame Generation (vk_dlssg.c vk_dlssg_pace), driven with
// made-up present times: when Reflex holds it and the pacer keeps only its ceiling, the cases the pacer keeps it
// (no host present yet or for 200 ms, a movie, the setting off, a limit Streamline has yet to take), and the checks
// over windows of presents: frames that come too soon give the limit to the pacer for the run, frames the multiplier
// periods apart switch Reflex to the frames on screen, and the same again gives it to the pacer. No GPU, no game files.
// Built and run by tests/dlssg_pace_check.py.
#include <stdio.h>
#include <string.h>

#include "dlssg_pace.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MS 1000000ull
static const uint64_t P60 = 16666667ull;   // frame_step_cap_ns at 60

// A run of n presents d ns apart starting after *t, each followed by a host present 1 ms later; the present thread takes
// every limit asked for at once (applied follows want). Returns how many were held by Reflex; *last_found the last
// non-SAME window result.
static int run(DlssgPace *p, uint64_t *t, int n, uint64_t d, uint64_t period, int mult, int allowed, uint32_t *applied,
               uint32_t *want, int *last_found) {
    int held = 0;
    for (int i = 0; i < n; i++) {
        *t += d;
        int found;
        held += dlssg_pace_step(p, *t, period, mult, *t - d + 1 * MS, allowed, *applied, want, &found);
        if (found != DLSSG_PACE_SAME && last_found) *last_found = found;
        *applied = *want;   // the present thread applies it before the next game present
    }
    return held;
}

int main(void) {
    uint32_t want = 0, applied = 0;
    int found = 0;
    uint64_t t = 1000 * MS;

    // before the first host present: the pacer
    DlssgPace p;
    memset(&p, 0, sizeof p);
    CHECK(!dlssg_pace_step(&p, t, P60, 2, 0, 1, 0, &want, &found) && want == 0, "no host present yet: pacer, no limit");

    // a host present 5 ms ago: Reflex is asked for the drawn period, the pacer holds until Streamline took it
    CHECK(!dlssg_pace_step(&p, t, P60, 2, t - 5 * MS, 1, 0, &want, &found) && want == 16667,
          "first frame: limit asked (%u), not yet applied", want);
    CHECK(dlssg_pace_step(&p, t + P60, P60, 2, t, 1, 16667, &want, &found) && want == 16667, "applied: Reflex holds");
    CHECK(!dlssg_pace_step(&p, t + 2 * P60, P60, 2, t, 1, 20000, &want, &found), "another limit applied: the pacer holds");

    // a movie (period 0): no limit for Reflex, the pacer holds
    CHECK(!dlssg_pace_step(&p, t, 0, 2, t - MS, 1, 16667, &want, &found) && want == 0, "movie: pacer");
    // the setting off, or an older DLL
    CHECK(!dlssg_pace_step(&p, t, P60, 2, t - MS, 0, 16667, &want, &found) && want == 0, "not allowed: pacer");
    // no host present for 200 ms (minimised): the pacer; 199 ms still Reflex
    CHECK(!dlssg_pace_step(&p, t, P60, 2, t - 200 * MS, 1, 16667, &want, &found) && want == 0, "idle 200 ms: pacer");
    CHECK(dlssg_pace_step(&p, t, P60, 2, t - 199 * MS, 1, 16667, &want, &found), "idle 199 ms: Reflex");

    // Reflex holds the limit: presents a period apart, no verdict over many windows
    memset(&p, 0, sizeof p), applied = 0, found = 0;
    int held = run(&p, &t, 600, P60, P60, 2, 1, &applied, &want, &found);
    CHECK(held == 599 && !p.bad && !p.shown && found == 0 && want == 16667, "held: %d of 600, bad %d shown %d", held, p.bad, p.shown);

    // a GPU slower than the limit at 1x: frames 2 periods apart, never taken for the shown count
    memset(&p, 0, sizeof p), applied = 0, found = 0;
    run(&p, &t, 300, 2 * P60, P60, 1, 1, &applied, &want, &found);
    CHECK(!p.bad && !p.shown && found == 0, "1x, slow GPU: no verdict");

    // nothing holds the game: 31 of 60 too soon -> the pacer for the run; 30 of 60 is not enough
    memset(&p, 0, sizeof p), applied = 0, found = 0;
    run(&p, &t, 2, P60, P60, 2, 1, &applied, &want, NULL);   // asked, then held: the next present counts
    run(&p, &t, 30, 5 * MS, P60, 2, 1, &applied, &want, &found);
    run(&p, &t, 30, P60, P60, 2, 1, &applied, &want, &found);
    CHECK(!p.bad && found == 0, "30 of 60 too soon: Reflex keeps it");
    run(&p, &t, 31, 5 * MS, P60, 2, 1, &applied, &want, &found);
    run(&p, &t, 29, P60, P60, 2, 1, &applied, &want, &found);
    CHECK(p.bad && found == DLSSG_PACE_FAST, "31 of 60 too soon: the pacer for the run");
    held = run(&p, &t, 120, P60, P60, 2, 1, &applied, &want, NULL);
    CHECK(held == 0 && want == 0, "after that, the pacer, whatever the frames do (%d held)", held);

    // the driver counts the frames on screen: at 2x the drawn frames come two periods apart -> shown units
    memset(&p, 0, sizeof p), applied = 0, found = 0;
    run(&p, &t, 2, P60, P60, 2, 1, &applied, &want, NULL);
    run(&p, &t, 60, 2 * P60, P60, 2, 1, &applied, &want, &found);
    CHECK(p.shown && !p.bad && found == DLSSG_PACE_SHOWN && want == 8333, "2x, two periods apart: shown units (%u)", want);
    // it does now: a period apart, Reflex keeps it
    found = 0;
    held = run(&p, &t, 300, P60, P60, 2, 1, &applied, &want, &found);
    CHECK(!p.bad && found == 0 && held == 300, "shown units hold the drawn period");
    // 4x: the period over 4
    run(&p, &t, 2, P60, P60, 4, 1, &applied, &want, NULL);
    CHECK(want == 4167, "4x shown units: %u", want);
    // the generation paused (the menu, 1x): the period itself
    run(&p, &t, 2, P60, P60, 1, 1, &applied, &want, NULL);
    CHECK(want == 16667, "1x shown units: %u", want);

    // shown units guessed wrong (the driver counts drawn frames): the frames come too soon -> the pacer
    memset(&p, 0, sizeof p), applied = 0, found = 0;
    p.shown = 1;
    run(&p, &t, 2, P60, P60, 2, 1, &applied, &want, NULL);
    run(&p, &t, 60, P60 / 2, P60, 2, 1, &applied, &want, &found);
    CHECK(p.bad && found == DLSSG_PACE_FAST, "shown units, frames at twice the limit: the pacer");

    // the same slow band twice (a GPU that only manages the limit over the multiplier) -> the pacer
    memset(&p, 0, sizeof p), applied = 0, found = 0;
    run(&p, &t, 2, P60, P60, 3, 1, &applied, &want, NULL);
    run(&p, &t, 60, 3 * P60, P60, 3, 1, &applied, &want, &found);
    CHECK(p.shown && found == DLSSG_PACE_SHOWN, "3x, three periods apart: shown units");
    run(&p, &t, 61, 3 * P60, P60, 3, 1, &applied, &want, &found);
    CHECK(p.bad && found == DLSSG_PACE_SLOW, "again: the pacer for the run");

    // hitches (a load: 10 periods) are neither
    memset(&p, 0, sizeof p), applied = 0, found = 0;
    run(&p, &t, 2, P60, P60, 2, 1, &applied, &want, NULL);
    run(&p, &t, 120, 10 * P60, P60, 2, 1, &applied, &want, &found);
    CHECK(!p.bad && !p.shown && found == 0, "hitches: no verdict");

    // a change of the period or the multiplier starts the window again: 40 too soon at one limit, 40 at the next
    memset(&p, 0, sizeof p), applied = 0, found = 0;
    run(&p, &t, 2, P60, P60, 2, 1, &applied, &want, NULL);
    run(&p, &t, 40, 5 * MS, P60, 2, 1, &applied, &want, &found);
    run(&p, &t, 40, 5 * MS, P60 / 2, 2, 1, &applied, &want, &found);
    CHECK(!p.bad, "window restarted at the period's change");
    run(&p, &t, 40, 2 * MS, P60 / 2, 3, 1, &applied, &want, &found);
    CHECK(!p.bad, "window restarted at the multiplier's change");

    // presents the pacer held are not counted: the limit never applied
    memset(&p, 0, sizeof p), found = 0;
    for (int i = 0; i < 200; i++) {
        t += 5 * MS;
        dlssg_pace_step(&p, t, P60, 2, t - MS, 1, 0, &want, &found);
    }
    CHECK(!p.bad && found == 0, "unapplied: no window");

    // the longest limit Reflex takes: 30 fps is 33333 us; a period too long for it is the pacer's
    memset(&p, 0, sizeof p);
    dlssg_pace_step(&p, t, 1000000000ull / 30, 1, t - MS, 1, 0, &want, &found);
    CHECK(want == 33333, "30 fps: %u", want);
    dlssg_pace_step(&p, t, 3000000000000000ull, 1, t - MS, 1, 0, &want, &found);
    CHECK(want == 0, "a period no limit can hold: %u", want);
    // the clock going back (a host present stamped after the game's present read the clock) holds nothing up
    CHECK(dlssg_pace_step(&p, t, P60, 2, t + MS, 1, 16667, &want, &found) && want == 16667, "a host present stamped after the game present read the clock: Reflex");

    printf(fails ? "%d checks failed\n" : "all checks passed\n", fails);
    return fails != 0;
}
