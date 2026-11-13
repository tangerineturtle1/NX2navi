// runtime/frame_rate.c: the step Ocarina is given (ocarina_step), driven through frame_step_cap_ns and
// frame_step_present with made-up display times and the rest of the runtime stubbed. Built and run by
// tests/frame_step_check.py; prints each failed check and exits 1 on any. With the argument "mean" it runs with
// NX2NAVI_STEP_STEADY=0 and checks the mean of the last five intervals.
#include "frame_rate.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- the runtime frame_rate.c reaches
static uint8_t page[4096];
static const char *limit = "60", *steady = "1";
static unsigned version = 1;
const int rt_game_version = 111;
unsigned rt_trace;
uint64_t rt_guest_start_ns;
const char *cfg_get(const char *key) {
    if (!strcmp(key, "NX2NAVI_FPS_LIMIT")) return limit;
    if (!strcmp(key, "NX2NAVI_STEP_STEADY")) return steady;
    if (!strcmp(key, "NX2NAVI_RTSS")) return "0";
    return NULL;
}
unsigned cfg_version(void) { return version; }
int rt_patch_set(const char *name) { return !strcmp(name, "ostep") ? 0 : !strcmp(name, "ofollow") ? 1 : -1; }
int rt_patch_enabled(int set) { return set == 0 || set == 1; }
void rt_patch_disable(int set) { (void)set; }
void rt_patch_switch(int set, int on) { (void)set, (void)on; }
void *rt_patch_page(void) { return page; }
void rt_log(const char *fmt, ...) { (void)fmt; }
double gfx_window_refresh_hz(void) { return 144; }
static int movie;   // a movie plays (frame_rate.c movie_cap_hz)
int ov_movie_live(void) { return movie; }
int vk_fg_generating(void) { return 0; }
float dbg_speed_factor(void) { return 1.0f; }   // the debug menu's game speed: normal
int vk_fg_active(void) { return 0; }
int vk_fg_wanted(void) { return 0; }
static int fg_mult = 1;   // frames on screen per game frame
int vk_dlssg_multiplier(void) { return fg_mult; }
int vk_dlssg_active(void) { return fg_mult > 1; }
int vk_lowlat_on(void) { return 0; }
void rtss_limit_want(int64_t millihz) { (void)millihz; }
int rtss_limit_active(void) { return 0; }
// The time manager the sound job's gate reads its step from (frame_rate.c sound_job_step), while the sound checks run:
// [main+0x4c7e890] holds the slot, the slot the manager, the manager +0x10 the step. Nothing else reads.
static int fake_manager;
static float fake_step = 1.0f;
int plat_mem_read(uint64_t a, void *out, size_t n) {
    const uint64_t slot = 0x7700000000ull, manager = 0x7700001000ull;
    if (!fake_manager) return 0;
    if (a == MOD_MAIN_BASE + 0x4c7e890 && n == 8) return memcpy(out, &slot, 8), 1;
    if (a == slot && n == 8) return memcpy(out, &manager, 8), 1;
    if (a == manager + 0x10 && n == 4) return memcpy(out, &fake_step, 4), 1;
    return 0;
}
int plat_mem_readable(uint64_t a, size_t n) { (void)a, (void)n; return 0; }
void mem_kernel_access(uint64_t a, uint64_t n) { (void)a, (void)n; }
int mem_host_readable(uint64_t a, uint64_t n) { (void)a, (void)n; return 0; }
int mem_info(uint32_t id, uint64_t *out) { (void)id, (void)out; return 0; }   // the floor probe (off here)
// the floor scan (off here)
static RtGuestLayout layout_stub;
const RtGuestLayout *mem_layout(void) { return &layout_stub; }
uint32_t mem_query(uint64_t a, RtMemoryInfo *out) { (void)a, memset(out, 0, sizeof *out); return 1; }
int mem_read_hold(uint64_t a, uint64_t n) { (void)a, (void)n; return 0; }
void mem_read_release(void) {}
int input_pad_stick_percent(double pct[2]) { pct[0] = pct[1] = 0; return 0; }
int mem_copy_readable(uint8_t *const *dst, const uint64_t (*src)[2], int n, uint8_t *ok) {
    (void)dst, (void)src, (void)n, (void)ok;
    return 0;
}
uint64_t kern_now_ns(void) { return 0; }
// the floor watch (off here)
int floor_watch_wanted(void) { return 0; }
int floor_watch_found(const uint64_t *addr, const float *val, const int *unit, int n, const float link[3]) {
    (void)addr, (void)val, (void)unit, (void)n, (void)link;
    return 0;
}
int floor_watch_present(const float link[3], int link_ok, float frames) {
    (void)link, (void)link_ok, (void)frames;
    return 0;
}
void plat_mutex_lock(PlatMutex *m) { (void)m; }     // one thread here
void plat_mutex_unlock(PlatMutex *m) { (void)m; }

// ---- one present: the pacer asks for the period, then reports the display time. Returns the step in 60 Hz frames.
static uint64_t now_ns = 1000000000ull;
uint64_t plat_now_ns(void) { return now_ns; }
static double wall_s, game_s;
static float present(double interval_s) {
    frame_step_cap_ns();
    now_ns += (uint64_t)(interval_s * 1e9 + 0.5);
    frame_step_present(now_ns, 1);
    float f;
    memcpy(&f, page + 256, 4);
    wall_s += interval_s;
    game_s += f / 60.0;
    return f;
}
static void set_limit(const char *l) {
    limit = l;
    version++;
    present(1.0);   // a gap: the filter starts again
    wall_s = game_s = 0;
}

// ---- the patch sites that cut the step to a whole number, called as the lifted code calls them: the value in s0, the
// object in its register, the whole number back in the register the cut wrote (all 64 bits of it).
static Cpu cpu;
static uint32_t water(uint64_t object, float step) {   // frame_rate_water_host: x21, w9
    uint32_t bits;
    memcpy(&bits, &step, 4);
    cpu.v[0] = bits, cpu.x[21] = object, cpu.x[9] = ~0ull;
    frame_rate_water_host(&cpu);
    CHECK(cpu.x[9] >> 32 == 0, "the water's steps leave x9's upper half %#llx", (unsigned long long)cpu.x[9]);
    return (uint32_t)cpu.x[9];
}
static uint32_t day(uint64_t object, float secs) {   // frame_rate_day_host: x19, w8
    uint32_t bits;
    memcpy(&bits, &secs, 4);
    cpu.v[0] = bits, cpu.x[19] = object, cpu.x[8] = ~0ull;
    frame_rate_day_host(&cpu);
    return (uint32_t)cpu.x[8];
}
// frame_rate_sim_host as the surfaces' sites call it (set owater): after the site's division, the update's seconds over
// 1/60 s in s0 and the object in x19; then the game's own cut of s0 (fcvtzs) gives the steps. Only s0's low lane may
// change: every general register, the other lanes and vector registers and the flags stay as they were.
static uint32_t sim(uint64_t object, float seconds) {
    Cpu c, before;
    uint8_t *p = (uint8_t *)&c;
    for (size_t i = 0; i < sizeof c; i++) p[i] = (uint8_t)(i * 37u + 11u);
    float quotient = seconds / (1.0f / 60.0f);   // the site's fdiv s0, s8, s0 (the literal is 1/60 as a float)
    uint32_t bits;
    memcpy(&bits, &quotient, 4);
    c.v[0] = bits, c.x[19] = object;   // the division wrote s0 and zeroed the lanes above it
    before = c;
    frame_rate_sim_host(&c);
    int same = c.nzcv == before.nzcv && c.sp == before.sp && !memcmp(&c.v[1], &before.v[1], sizeof c.v - sizeof c.v[0]) &&
               c.v[0] >> 32 == 0 && !memcmp(c.x, before.x, sizeof c.x);
    CHECK(same, "the surfaces' site changed something besides s0's low lane");
    memcpy(&quotient, &c.v[0], 4);
    return cut_fcvtzs(quotient);   // the game's fcvtzs after the site
}
// The physics solver step count (patches-v111.txt osolver and osolverfull, lifted by frame_step_check.py): the step
// setup's words from the literal load to the cut, run on the frames per update in s0 and the count (4) in s1 as the
// game loads them, x8 pointing at the literal 0.001 the original loads. Returns w1, the steps.
void solver_game(Cpu *c);
void solver_osolver(Cpu *c);
void solver_osolverfull(Cpu *c);
static uint32_t solver_steps(void (*fn)(Cpu *), float frames) {
    static float literal[0x410 / 4];
    literal[0x40c / 4] = 0.001f;
    Cpu c;
    memset(&c, 0, sizeof c);
    uint32_t bits;
    memcpy(&bits, &frames, 4);
    c.v[0] = bits, c.v[1] = 4, c.x[8] = (uint64_t)(uintptr_t)literal, c.x[1] = ~0ull;
    fn(&c);
    CHECK(c.x[1] >> 32 == 0, "the solver count leaves x1's upper half %#llx", (unsigned long long)c.x[1]);
    return (uint32_t)c.x[1];
}

// The floor-carry gate (set ofollow): 1 when its b.gt is taken, which keeps a tenth of the floor's velocity. s0 is the
// physics' inverse step; the set's load reads the page through x8.
void follow_game(Cpu *c);
void follow_ofollow(Cpu *c);
static int follow_cut(void (*fn)(Cpu *), float inverse) {
    Cpu c;
    memset(&c, 0, sizeof c);
    uint32_t bits;
    memcpy(&bits, &inverse, 4);
    c.v[0] = bits, c.x[8] = (uint64_t)(uintptr_t)page;
    fn(&c);
    return !FLAG_Z(&c) && FLAG_N(&c) == FLAG_V(&c);   // gt
}

int main(int argc, char **argv) {
    int mean = argc > 1 && !strcmp(argv[1], "mean");
    if (mean) steady = "0";
    frame_step_init();
    float one, cut0;
    memcpy(&one, page + 256, 4);
    memcpy(&cut0, page + 260, 4);
    CHECK(one == 1.0f, "the page holds one frame before the first present, not %g", one);
    CHECK(cut0 == 100.0f, "the floor carry's cut is the game's 100 before the first present, not %g", cut0);
    const double p60 = 16666667e-9, p240 = 4166667e-9;

    if (mean) {   // the comparison setting: 5 and 30 ms in turn give 15 and 20 ms in turn
        set_limit("unlimited");
        float lo = 9, hi = 0;
        for (int i = 0; i < 200; i++) {
            float f = present(i & 1 ? 0.030 : 0.005);
            if (i >= 20) lo = f < lo ? f : lo, hi = f > hi ? f : hi;
        }
        CHECK(fabsf(lo - 0.9f) < 1e-3f && fabsf(hi - 1.2f) < 1e-3f, "mean of five: %g..%g frames, not 0.9..1.2", lo, hi);
        printf("frame_step_check (mean): %s\n", fails ? "FAILED" : "ok");
        return fails != 0;
    }

    // A 60 fps limit with a late frame and the short one after it every 37 frames: one frame exactly, every frame.
    set_limit("60");
    for (int i = 0; i < 600; i++) {
        float f = present(i % 37 == 5 ? p60 + 0.004 : i % 37 == 6 ? p60 - 0.004 : p60);
        CHECK(f == 1.0f, "60 fps limit, frame %d: %.9g frames", i, f);
    }
    CHECK(fabs(game_s - wall_s) < 0.001, "60 fps limit: %.4f s stepped in %.4f s", game_s, wall_s);

    // The 240 cap with late frames: never under a quarter frame (the game's own solver step count is 0 there).
    set_limit("240");
    for (int i = 0; i < 1200; i++) {
        double late = 0.0005 + 0.0025 * ((i * 7919) % 100) / 100.0;
        float f = present(i % 29 == 3 ? p240 + late : i % 29 == 4 ? p240 - 0.0005 - 0.0025 * (((i - 1) * 7919) % 100) / 100.0 : p240);
        CHECK(f >= 0.25f && f < 0.2525f, "240 cap, frame %d: %.6f frames", i, f);
    }
    CHECK(fabs(game_s - wall_s) < 0.003, "240 cap: %.4f s stepped in %.4f s", game_s, wall_s);

    // Unlimited, 5 and 30 ms in turn: one step, and the clock kept.
    set_limit("unlimited");
    float prev = 0;
    for (int i = 0; i < 600; i++) {
        float f = present(i & 1 ? 0.030 : 0.005);
        if (i >= 24) CHECK(fabsf(f / prev - 1) < 0.02f, "5/30 ms, frame %d: %.4f after %.4f frames", i, f, prev);
        prev = f;
    }
    CHECK(fabs(game_s / wall_s - 1) < 0.005, "5/30 ms: %.4f s stepped in %.4f s", game_s, wall_s);

    // 200 fps and one 60 ms frame: the step rises by a tenth at most and the 55 ms are paid back.
    set_limit("unlimited");
    for (int i = 0; i < 1000; i++) {
        float f = present(i == 300 ? 0.060 : 0.005);
        if (i >= 24) CHECK(f >= 0.2999f && f <= 0.3301f, "hitch, frame %d: %.4f frames", i, f);
        if (i == 302) CHECK(f > 0.32f, "hitch: the step does not repay (%.4f frames two frames later)", f);
    }
    CHECK(fabs(game_s - wall_s) < 0.004, "hitch: %.4f s stepped in %.4f s", game_s, wall_s);

    // A real change of rate, 240 to 120 fps, arrives within 12 frames.
    set_limit("unlimited");
    for (int i = 0; i < 300; i++) present(p240);
    float f = 0;
    for (int i = 0; i < 12; i++) f = present(2 * p240);
    CHECK(f > 0.49f, "240 -> 120 fps: %.4f frames after 12", f);

    // A movie started at 240 fps: exactly one frame from its first update, late and short frames included, as the game
    // shows one picture per update; at its end the step follows the frame limit again within 12 frames.
    set_limit("unlimited");
    for (int i = 0; i < 300; i++) present(p240);
    movie = 1;
    for (int i = 0; i < 600; i++) {
        float g = present(i % 120 == 7 ? 0.070 : i % 120 == 8 ? 0.004 : p60);
        CHECK(g == 1.0f, "movie, frame %d: %.6f frames", i, g);
    }
    CHECK(frame_step_movie_hz() == 60, "the movie's rate: %d", frame_step_movie_hz());
    movie = 0;
    float after = 0;
    for (int i = 0; i < 12; i++) after = present(p240);
    CHECK(fabsf(after - 0.25f) < 0.002f, "after the movie: %.4f frames", after);

    // A frame longer than 1/15 s slows the game down; the step never leaves 1/480..1/15 s.
    set_limit("unlimited");
    for (int i = 0; i < 100; i++) {
        float g = present(0.2);
        CHECK(g <= 4.0001f, "long frames: %.4f frames", g);
    }
    set_limit("unlimited");
    for (int i = 0; i < 100; i++) {
        float g = present(0.0011);   // 900 fps, as a quick menu without the pacer would run
        CHECK(g >= 0.125f, "short frames: %.4f frames", g);
    }
    // The limit counts the frames the game draws: frame generation's multiplier leaves the pace of a numeric limit and
    // of unlimited alone, and divides only the monitor's rate (144 Hz here), which is a rate on the screen.
    static const struct { const char *l; int gen; double hz, game_hz; } caps[] = {
        {"60", 1, 60, 60},        {"60", 4, 60, 60},      {"120", 2, 120, 120},     {"240", 4, 240, 240},
        {"display", 1, 144, 144}, {"display", 4, 36, 36}, {"unlimited", 1, 240, 0}, {"unlimited", 4, 240, 0}};
    for (unsigned i = 0; i < sizeof caps / sizeof *caps; i++) {
        set_limit(caps[i].l);
        fg_mult = caps[i].gen;
        double hz = 1e9 / (double)frame_step_cap_ns(), game = frame_step_game_hz();
        CHECK(fabs(hz - caps[i].hz) < 0.01, "limit %s at %dx paces the game at %.3f fps, not %.0f", caps[i].l, caps[i].gen,
              hz, caps[i].hz);
        CHECK(fabs(game - caps[i].game_hz) < 0.01, "limit %s at %dx reports %.3f game fps, not %.0f", caps[i].l,
              caps[i].gen, game, caps[i].game_hz);
    }
    fg_mult = 1;

    // The water's ripples at 144 fps, a step of 60/144 frame: 64 water objects asked once a frame each run 600 steps
    // in 1440 frames (the cut gave none), and the day's seconds asked between them keep their own fraction.
    {
        const float step = 60.0f / 144.0f;
        const uint64_t first = 0x7100000000ull, clock = 0x7200000000ull;
        uint32_t steps[64] = {0}, secs = 0;
        for (int f = 0; f < 1440; f++) {
            for (int k = 0; k < 64; k++) steps[k] += water(first + 0x1000 * (uint64_t)k, step);
            secs += day(clock, 2 * step);
        }
        for (int k = 0; k < 64; k++)
            CHECK(steps[k] >= 599 && steps[k] <= 600, "water object %d of 64 at 144 fps: %u steps in 1440 frames", k, steps[k]);
        CHECK(secs >= 1199 && secs <= 1200, "the day beside 64 water objects: %u seconds in 1440 frames", secs);
    }
    // A step of one frame, and of two, is what the game computes, also after a fraction was left; a quarter frame
    // gives one step every fourth frame, and what fcvtzs cuts to nothing or saturates leaves the remainder alone.
    {
        const uint64_t pond = 0x7300000000ull;
        for (int i = 0; i < 3; i++) CHECK(water(pond, 0.25f) == 0, "a quarter frame, call %d", i);
        CHECK(water(pond, 0.0f) == 0 && water(pond, -3.5f) == 0xfffffffdu && water(pond, NAN) == 0 &&
                  water(pond, 3e9f) == 0x7fffffffu, "zero, a negative step, NaN and a step too large are cut as fcvtzs cuts");
        CHECK(water(pond, 0.25f) == 1, "the fourth quarter frame");
        CHECK(water(pond, 0.5f) == 0, "half a frame");
        for (int i = 0; i < 60; i++) CHECK(water(pond, 1.0f) == 1, "one frame, call %d", i);
        for (int i = 0; i < 30; i++) CHECK(water(pond, 2.0f) == 2, "two frames, call %d", i);
        CHECK(water(pond, 0.5f) == 1, "the half frame kept through the whole ones");
    }
    // The water managers and the floors (set owater): at 120, 144 and 240 fps each runs 60 fixed steps a second (the cut
    // gave none above 60), beside 64 water objects; at 60 and 30 fps one and two steps an update, as the game counts them.
    {
        const uint64_t interactive = 0x7400000000ull, responsive = 0x7400001000ull, floor = 0x7400002000ull,
                       first = 0x7500000000ull;
        const float rates[] = {144.0f, 240.0f, 120.0f};
        for (int r = 0; r < 3; r++) {
            uint32_t a = 0, b = 0, c = 0, w = 0;
            int frames = (int)(rates[r] * 10);
            for (int f = 0; f < frames; f++) {
                a += sim(interactive, 1.0f / rates[r]);
                for (int k = 0; k < 64; k++) w += water(first + 0x1000 * (uint64_t)k, 60.0f / rates[r]);
                b += sim(responsive, 1.0f / rates[r]);
                c += sim(floor, 1.0f / rates[r]);
            }
            CHECK(a >= 599 && a <= 600 && b >= 599 && b <= 600 && c >= 599 && c <= 600,
                  "%.0f fps: the managers and the floor ran %u, %u and %u steps in 10 s",
                  rates[r], a, b, c);
            CHECK(w >= 64 * 599, "%.0f fps: the water objects beside the managers ran %u steps", rates[r], w);
        }
        for (int i = 0; i < 120; i++) CHECK(sim(interactive, 1.0f / 60.0f) == 1, "60 fps, update %d", i);
        for (int i = 0; i < 60; i++) CHECK(sim(responsive, 1.0f / 30.0f) == 2, "30 fps, update %d", i);
        CHECK(sim(interactive, 0.0f) == 0 && sim(interactive, -1.0f / 60.0f) == 0xffffffffu &&
                  sim(interactive, NAN) == 0, "no time, negative time and NaN are cut as fcvtzs cuts");
        CHECK(sim(interactive, 1.0f / 15.0f) == 4, "four steps for a 15 fps update (the game's clamp to 3 follows)");
    }
    // A place with many interactive floors: 2 water managers and 30 floors, each asking once an update at 144 fps, all
    // keep their own remainder and run 60 fixed steps a second (with 8 slots they took each other's and ran none).
    {
        const uint64_t first = 0x7600000000ull;
        uint32_t steps[32] = {0};
        for (int f = 0; f < 1440; f++)
            for (int k = 0; k < 32; k++) steps[k] += sim(first + 0x1000 * (uint64_t)k, 1.0f / 144.0f);
        for (int k = 0; k < 32; k++)
            CHECK(steps[k] >= 599 && steps[k] <= 600, "surface %d of 32 at 144 fps: %u steps in 10 s", k, steps[k]);
    }
    // The sound job (set osound). At 60 and 30 fps every phase of every frame runs with the game's step; at 144 and 240
    // fps it runs 60 times a second with a step of one, phases 1 and 2 with phase 0; a skipped phase gets 3 in w2.
    {
        fake_manager = 1;
        const float steps[] = {1.0f, 2.0f, 60.0f / 144.0f, 0.25f};
        const int frames[] = {600, 300, 1440, 2400};   // 10 s each
        for (int r = 0; r < 4; r++) {
            fake_step = steps[r];
            int ran[3] = {0}, odd = 0;
            float fed = 0;
            for (int f = 0; f < frames[r]; f++) {
                for (uint32_t phase = 0; phase < 3; phase++) {
                    cpu.x[2] = phase;
                    frame_rate_sound_gate_host(&cpu);
                    if (cpu.x[2] == phase) ran[phase]++;
                    else odd += cpu.x[2] != 3;
                    if (phase == 0 && cpu.x[2] == 0) {   // phase 0 runs on to its load of the step
                        uint32_t bits;
                        memcpy(&bits, &fake_step, 4);
                        cpu.v[0] = bits;
                        frame_rate_sound_step_host(&cpu);
                        float s;
                        memcpy(&s, &cpu.v[0], 4);
                        fed += s;
                        CHECK(cpu.v[0] >> 32 == 0, "the step's upper lanes stay zero");
                    }
                }
            }
            const int want = steps[r] >= 1.0f ? frames[r] : 600;
            CHECK(ran[0] >= want - 1 && ran[0] <= want && ran[1] == ran[0] && ran[2] == ran[0] && !odd,
                  "step %.3f: phases ran %d, %d, %d of %d frames (want %d)", steps[r], ran[0], ran[1], ran[2], frames[r],
                  want);
            CHECK(fed >= 599.0f && fed <= 600.5f, "step %.3f: the sound system was fed %.2f frames in 10 s", steps[r], fed);
        }
        // Owed fractions survive a whole step, and a step that is not positive runs and clears them.
        SoundGate g = {0.0f, 1};
        CHECK(!sound_gate_take(&g, 0, 0.5f) && !sound_gate_take(&g, 1, 1.0f), "half a frame: the job and its phase 1 wait");
        CHECK(sound_gate_take(&g, 0, 1.0f) && sound_gate_take(&g, 2, 1.0f) && g.owed == 0.5f, "a whole frame runs, the half kept");
        CHECK(sound_gate_take(&g, 0, 0.5f) && g.owed == 0.0f, "the kept half and a new one run");
        CHECK(!sound_gate_take(&g, 0, 0.75f) && sound_gate_take(&g, 0, NAN) && g.owed == 0.0f &&
                  sound_gate_take(&g, 0, 0.0f), "NaN and 0 run as the game has them and clear what is owed");
        fake_manager = 0;
    }
    // The solver step count: at 60 and 30 fps all three give the game's 4 and 8 (also with the steady step a little
    // off them); above 60 fps the game's count falls to 1 and to 0, osolver's rounds up (2 at 144 fps, 1 at 240), and
    // osolverfull's keeps every update at the 60 Hz frame's 4.
    {
        const struct { float frames; uint32_t game, up, full; } want[] = {
            {1.0f, 4, 4, 4}, {2.0f, 8, 8, 8}, {0.98f, 3, 4, 4}, {1.02f, 4, 4, 4}, {60.0f / 144.0f, 1, 2, 4},
            {60.0f / 160.0f, 1, 2, 4}, {0.25f, 1, 1, 4}, {60.0f / 300.0f, 0, 1, 4}, {1.5f, 6, 6, 6}};
        for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
            uint32_t g = solver_steps(solver_game, want[i].frames), u = solver_steps(solver_osolver, want[i].frames),
                     f = solver_steps(solver_osolverfull, want[i].frames);
            CHECK(g == want[i].game && u == want[i].up && f == want[i].full,
                  "%.4f frames an update: solver steps %u / %u / %u, not %u / %u / %u", want[i].frames, g, u, f,
                  want[i].game, want[i].up, want[i].full);
        }
        // steps a second at 144 and 240 fps: osolver 288 and 240, osolverfull 576 and 960
        printf("frame_step_check: solver steps a second at 144 / 240 fps: physics_steps 1 %u / %u, 2 %u / %u\n",
               144 * solver_steps(solver_osolver, 60.0f / 144.0f), 240 * solver_steps(solver_osolver, 0.25f),
               144 * solver_steps(solver_osolverfull, 60.0f / 144.0f), 240 * solver_steps(solver_osolverfull, 0.25f));
    }
    // The floor carry (set ofollow). The physics' inverse step is 60 / (frames per update * time scale); the game cuts
    // a floor's velocity to a tenth above 100, which at 60 fps is a time scale under 0.6 and above 100 fps every
    // update. With the page's 100 / frames per update the set cuts exactly where the game cuts at 60 fps at any rate
    // above 60; at 60 and 30 fps the page holds 100 and the set cuts where the game's own words do.
    {
        const char *limits[] = {"60", "30", "90", "120", "144", "240"};
        const int hz[] = {60, 30, 90, 120, 144, 240};
        const float scales[] = {1.0f, 0.8f, 0.62f, 0.58f, 0.5f, 0.25f};
        for (int r = 0; r < 6; r++) {
            set_limit(limits[r]);
            float frames = 0, cut;
            for (int i = 0; i < 60; i++) frames = present(1.0 / hz[r]);
            memcpy(&cut, page + 260, 4);
            CHECK(cut == (frames < 1 ? 100.0f / frames : 100.0f), "%d fps (%g frames): the floor carry's cut is %g",
                  hz[r], frames, cut);
            for (int s = 0; s < 6; s++) {
                float inverse = 60.0f / (frames * scales[s]);
                int set = follow_cut(follow_ofollow, inverse),
                    want = follow_cut(follow_game, hz[r] > 60 ? 60.0f / scales[s] : inverse);
                CHECK(set == want, "%d fps, time scale %.2f: the set %s, the game %s %s", hz[r], scales[s],
                      set ? "cuts" : "carries", hz[r] > 60 ? "at 60 fps" : "here", want ? "cuts" : "carries");
            }
            CHECK(follow_cut(follow_game, 60.0f / frames) == (hz[r] > 100), "%d fps: the game's own words %s", hz[r],
                  hz[r] > 100 ? "do not cut" : "cut");
        }
    }
    printf("frame_step_check: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
