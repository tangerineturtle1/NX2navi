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
int rt_patch_set(const char *name) { return !strcmp(name, "ostep") ? 0 : -1; }
int rt_patch_enabled(int set) { return set == 0; }
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
void rtss_limit_want(int64_t millihz) { (void)millihz; }
int rtss_limit_active(void) { return 0; }
int plat_mem_read(uint64_t a, void *out, size_t n) { (void)a, (void)out, (void)n; return 0; }
int plat_mem_readable(uint64_t a, size_t n) { (void)a, (void)n; return 0; }
void mem_kernel_access(uint64_t a, uint64_t n) { (void)a, (void)n; }
int mem_host_readable(uint64_t a, uint64_t n) { (void)a, (void)n; return 0; }
uint64_t kern_now_ns(void) { return 0; }

// ---- one present: the pacer asks for the period, then reports the display time. Returns the step in 60 Hz frames.
static uint64_t now_ns = 1000000000ull;
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

int main(int argc, char **argv) {
    int mean = argc > 1 && !strcmp(argv[1], "mean");
    if (mean) steady = "0";
    frame_step_init();
    float one;
    memcpy(&one, page + 256, 4);
    CHECK(one == 1.0f, "the page holds one frame before the first present, not %g", one);
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
    printf("frame_step_check: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
