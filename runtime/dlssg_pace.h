// The frame limit Reflex holds with DLSS Frame Generation (vk_dlssg.c vk_dlssg_pace), as a function of what the
// presenting guest thread sees and nothing else, so tests/dlssg_pace_check.c drives it without a GPU.
//
// The pacer's period counts the frames the game draws (frame_rate.c frame_step_cap_ns). Reflex holds it where the frame
// starts, before the pad is read, and the pacer then keeps only its ceiling (frame_rate.c frame_step_ceiling_ns); the
// pacer keeps the whole period while a movie plays (the caller passes 0), before the first host present and while none
// went out for DLSSG_HOLD_IDLE_NS (Reflex's sleep is left out then, vk_dlssg.c vk_dlssg_frame_start), while the limit
// Reflex should hold is not the one Streamline last took, and for the rest of the run once the frames show that Reflex
// does not hold it.
//
// The frames are checked over windows of DLSSG_PACE_WINDOW presents Reflex held to one period and one multiplier.
// Over half of them sooner than three quarters of the period: nothing holds the game, and a game that runs free can
// outrun what its own code survives, so the pacer takes the limit for the run. Over half of them about the multiplier
// times the period apart (within half a period): the driver counted the frames on screen, the generated ones too, so
// Reflex is given the period over the multiplier from then on; the same again (a GPU that only managed that rate)
// gives the limit to the pacer for the run. A frame a load or a hitch held counts as neither.
#pragma once
#include <stdint.h>

#define DLSSG_HOLD_IDLE_NS 200000000ull   // no host present for this long: Reflex's sleep is left out
#define DLSSG_PACE_WINDOW 60              // presents over which Reflex's hold of the limit is checked

enum { DLSSG_PACE_SAME, DLSSG_PACE_FAST, DLSSG_PACE_SHOWN, DLSSG_PACE_SLOW };   // what the last window found

typedef struct {
    uint64_t last_ns;       // the newest present whose frame Reflex held, 0 when the pacer held it
    uint64_t last_period;   // its period
    int last_mult;          // and its multiplier
    int window, fast, slow; // presents counted in the window, those too soon, those about the multiplier periods apart
    int shown;              // Reflex counts the frames on screen: it is given the period over the multiplier
    int bad;                // Reflex did not hold the limit: the pacer holds it for the rest of the run
} DlssgPace;

// now: kern_now_ns at the game's present. period_ns: the pacer's period (0: a movie, or no limit). mult: frames on
// screen per drawn frame now (vk_dlssg_multiplier). hosted_ns: kern_now_ns after the newest host present, 0 before
// the first. allowed: Reflex may hold the limit (the setting on, a DLL with nx2sl_reflex2 that Streamline did not
// refuse). applied_us: the limit Streamline last took. *want_us: the limit Reflex should hold, 0 none. *found: what a
// window that ended here found (DLSSG_PACE_*). Returns 1 when Reflex holds this frame's limit: the pacer keeps only
// its ceiling.
static inline int dlssg_pace_step(DlssgPace *p, uint64_t now, uint64_t period_ns, int mult, uint64_t hosted_ns,
                                  int allowed, uint32_t applied_us, uint32_t *want_us, int *found) {
    *found = DLSSG_PACE_SAME;
    if (mult < 1) mult = 1;
    if (!p->bad && p->last_ns && now >= p->last_ns && p->last_period == period_ns && p->last_mult == mult) {
        uint64_t d = now - p->last_ns, m = (uint64_t)mult;
        if (d < period_ns / 4 * 3) p->fast++;
        else if (m > 1 && d * 2 > period_ns * (2 * m - 1) && d * 2 < period_ns * (2 * m + 1)) p->slow++;
        if (++p->window >= DLSSG_PACE_WINDOW) {
            if (p->fast > DLSSG_PACE_WINDOW / 2) p->bad = 1, *found = DLSSG_PACE_FAST;
            else if (p->slow > DLSSG_PACE_WINDOW / 2 && p->shown) p->bad = 1, *found = DLSSG_PACE_SLOW;
            else if (p->slow > DLSSG_PACE_WINDOW / 2) p->shown = 1, *found = DLSSG_PACE_SHOWN;
            p->window = p->fast = p->slow = 0;
        }
    } else {
        p->window = p->fast = p->slow = 0;   // a window counts only presents Reflex held to one period and multiplier
    }
    uint64_t us = 0;
    // a host present stamped after now went out while this thread read the clock: as recent as one can be
    int live = hosted_ns && (hosted_ns >= now || now - hosted_ns < DLSSG_HOLD_IDLE_NS);
    if (allowed && period_ns && !p->bad && live) {
        uint64_t per = p->shown ? period_ns / (uint64_t)mult : period_ns;
        us = (per + 500) / 1000;
        if (!us) us = 1;
        if (us > 0x7fffffffu) us = 0;   // no limit Reflex could be given: the pacer keeps it
    }
    *want_us = (uint32_t)us;
    int held = us && applied_us == (uint32_t)us;
    p->last_ns = held ? now : 0;
    p->last_period = held ? period_ns : 0;
    p->last_mult = mult;
    return held;
}
