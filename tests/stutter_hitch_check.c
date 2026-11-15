// tests/stutter_hitch_check.py builds this: runtime/stutter_map.c's hitch rule on made-up frame times, with the few
// runtime functions it calls stubbed. A steady change of pace (the game drawing at 60 fps after 200) counts no hitch;
// isolated long frames each count one; a frame over SM_HITCH_SURE_NS counts whatever follows it.
#include <stdarg.h>
#include <stdio.h>

#include "../runtime/stutter_map.c"

static uint64_t clock_ns;
int cfg_bool(const char *name) { return !strcmp(name, "NX2NAVI_STUTTER_MAP"); }
uint64_t kern_now_ns(void) { return clock_ns; }
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
int plat_mutex_trylock(PlatMutex *m) { (void)m; return 1; }
static int logged;
static char head[1400];   // the last report's first line
void rt_log(const char *fmt, ...) {
    logged++;
    char line[1400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (!strncmp(line, "stutter map (", 13)) memcpy(head, line, sizeof head);
}

// perf.c's causes, in its order (FT_C_*); the frames below are charged to the last, unattributed
static const char *const names[] = {"pipeline", "translation", "texture", "fence", "pacing", "hand-off", "replay",
                                    "census", "report", "unattributed"};
#define OTHER (SM_CAUSES - 1)
_Static_assert(sizeof names / sizeof *names == SM_CAUSES, "names follows perf.c's FT_C_* list");
static uint64_t frame_no;

static void frames(int n, double ms) {
    uint64_t part[SM_CAUSES] = {0};
    for (int i = 0; i < n; i++) {
        uint64_t d = (uint64_t)(ms * 1e6);
        clock_ns += d;
        part[OTHER] = d;
        stutter_frame(++frame_no, clock_ns, d, part, names, SM_CAUSES, OTHER);
    }
}

static int fails;
static void expect(const char *what, uint64_t got, uint64_t want) {
    if (got != want) printf("FAIL %s: %llu, expected %llu\n", what, (unsigned long long)got, (unsigned long long)want), fails++;
}

// The hitches in play the exit's report prints, and the ones the areas hold (what a report counts so far).
static uint64_t exit_hitches(void) {
    head[0] = 0;
    stutter_report("exit");
    const char *p = strstr(head, " areas; ");
    return p ? strtoull(p + 8, NULL, 10) : UINT64_MAX;
}
static uint64_t counted(void) {
    uint64_t n = 0;
    for (int i = 0; i < n_areas; i++) n += areas[i].hitch;
    return n;
}

int main(void) {
    stutter_map_init();
    clock_ns = 10000000000ull;   // well past any area load
    frames(200, 5.0);
    expect("steady 200 fps", areas[0].hitch, 0);
    frames(300, 16.6);   // the game's menus at 60 fps
    expect("a change to 60 fps", areas[0].hitch, 0);
    if (!paced_out) printf("FAIL the change of pace was not counted as one\n"), fails++;
    frames(200, 7.0);
    for (int k = 0; k < 5; k++) frames(1, 30.0), frames(60, 7.0);   // five isolated spikes
    expect("five isolated spikes", areas[0].hitch, 5);
    frames(1, 150.0), frames(1, 150.0), frames(60, 7.0);   // two long frames in a row: one episode of 300 ms
    expect("two frames over the sure line", areas[0].hitch, 6);
    expect("their episode's time", areas[0].worst_ns, 300000000ull);
    frames(1, 40.0), frames(1, 40.0), frames(60, 7.0);   // a double spike: one episode
    expect("a double spike", areas[0].hitch, 7);
    // three long frames, each followed by one as long, as after an area load in a session of 2026-11-13 (87, 91 and
    // 96 ms): one episode, not three changes of pace
    uint64_t paced0 = paced_out, charged0 = areas[0].cause_ns[OTHER];
    frames(1, 87.0), frames(1, 91.0), frames(1, 96.0), frames(60, 7.0);
    expect("three slow frames in a row", areas[0].hitch, 8);
    expect("not a change of pace", paced_out, paced0);
    expect("the episode's time", areas[0].cause_ns[OTHER] - charged0, 274000000ull);
    // a change to 30 fps from 140: a run of long frames longer than SM_EPISODE_MAX, a change of pace
    frames(200, 7.0);
    frames(300, 33.3);
    expect("a change to 30 fps", areas[0].hitch, 8);
    frames(200, 7.0);
    stutter_area("Kakariko");   // a load: what follows within SM_LOAD_NS is the load's
    frames(1, 60.0), frames(30, 7.0);
    expect("a spike right after a load (play)", areas[1].hitch, 0);
    expect("a spike right after a load (load)", areas[1].load_hitch, 1);
    clock_ns += 10000000000ull;
    frames(60, 7.0);
    stutter_load();   // the texture pool's load of an area, without the game's
    frames(1, 60.0), frames(30, 7.0);
    expect("a spike right after the pool's load (play)", areas[1].hitch, 0);
    expect("a spike right after the pool's load (load)", areas[1].load_hitch, 2);
    // At 30 fps every frame is over SM_HITCH_ANY_NS and so a candidate: a hitch followed by the steady frames ends at
    // itself, not at the end of the slow run (an episode of 301 frames, 10 s long, before)
    stutter_area("Lake Hylia");
    clock_ns += 10000000000ull;
    frames(200, 7.0);
    frames(1, 150.0), frames(300, 33.3);
    expect("150 ms then 300 frames at 30 fps", areas[2].hitch, 1);
    expect("150 ms then 300 frames at 30 fps (worst)", areas[2].worst_ns, 150000000ull);
    frames(200, 7.0);   // the end of the slow run stands out from the frames after it: one more
    stutter_area("Gerudo Valley");
    clock_ns += 10000000000ull;
    frames(200, 33.3);   // a steady 30 fps with a 120 ms hitch every 60 frames: each counts
    for (int k = 0; k < 5; k++) frames(1, 120.0), frames(59, 33.3);
    expect("five hitches at 30 fps", areas[3].hitch, 5);
    expect("five hitches at 30 fps (worst)", areas[3].worst_ns, 120000000ull);
    expect("five hitches at 30 fps (their time)", areas[3].cause_ns[OTHER], 600000000ull);
    // The exit's report counts a hitch in the last frames, which only the next frames would have decided: a frame over
    // the sure line followed by 0, 1 or 2 frames (a waiting candidate, then an open episode), two such frames in a row
    // (one episode) and three long frames ending the run (one episode); each time what the run counts once it goes on
    stutter_area("Death Mountain");
    clock_ns += 10000000000ull;
    static const double tail[][3] = {{150.0, 0, 0}, {150.0, 7.0, 0}, {150.0, 7.0, 7.0}, {150.0, 150.0, 0},
                                     {150.0, 150.0, 7.0}, {87.0, 91.0, 96.0}};
    for (size_t t = 0; t < sizeof tail / sizeof *tail; t++) {
        frames(100, 7.0);
        uint64_t before = counted();
        for (int k = 0; k < 3 && tail[t][k] > 0; k++) frames(1, tail[t][k]);
        char what[64];
        snprintf(what, sizeof what, "the exit right after case %zu", t);
        expect(what, exit_hitches(), before + 1);
        frames(60, 7.0);
        snprintf(what, sizeof what, "case %zu once the run went on", t);
        expect(what, counted(), before + 1);
    }
    frames(100, 7.0);
    expect("the exit with nothing waiting", exit_hitches(), counted());
    if (!logged) printf("FAIL nothing was logged\n"), fails++;
    if (fails) return 1;
    printf("stutter_hitch_check: all checks passed\n");
    return 0;
}
