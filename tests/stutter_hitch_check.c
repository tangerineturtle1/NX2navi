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
static int logged;
void rt_log(const char *fmt, ...) {
    (void)fmt;
    logged++;
}

static const char *const names[7] = {"pipeline", "translation", "texture", "fence", "pacing", "replay", "unattributed"};
static uint64_t frame_no;

static void frames(int n, double ms) {
    uint64_t part[7] = {0};
    for (int i = 0; i < n; i++) {
        uint64_t d = (uint64_t)(ms * 1e6);
        clock_ns += d;
        part[6] = d;
        stutter_frame(++frame_no, clock_ns, d, part, names, 7, 6);
    }
}

static int fails;
static void expect(const char *what, uint64_t got, uint64_t want) {
    if (got != want) printf("FAIL %s: %llu, expected %llu\n", what, (unsigned long long)got, (unsigned long long)want), fails++;
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
    frames(1, 150.0), frames(1, 150.0), frames(60, 7.0);   // two long frames in a row: the second stands out
    expect("two frames over the sure line", areas[0].hitch, 7);
    frames(1, 40.0), frames(1, 40.0), frames(60, 7.0);   // a double spike: the second one counts
    expect("a double spike", areas[0].hitch, 8);
    stutter_area("Kakariko");   // a load: what follows within SM_LOAD_NS is the load's
    frames(1, 60.0), frames(30, 7.0);
    expect("a spike right after a load (play)", areas[1].hitch, 0);
    expect("a spike right after a load (load)", areas[1].load_hitch, 1);
    if (!logged) printf("FAIL nothing was logged\n"), fails++;
    if (fails) return 1;
    printf("stutter_hitch_check: all checks passed\n");
    return 0;
}
