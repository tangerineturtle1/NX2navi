// runtime/widescreen.c and the widescreen sites of recomp/patches-v111.txt, lifted by tests/widescreen_check.py into
// widescreen_sites.c, with the rest of the runtime stubbed. wide_init fills the page for a picture of each shape; the
// lifted sites then run on made-up projections, UI matrices, viewports and save targets, and each set's result is
// compared with the game's own words. Prints each failed check and exits 1 on any.
#include <stdarg.h>

#include "widescreen.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- the runtime widescreen.c reaches
const int rt_game_version = 111;
unsigned rt_trace;
static _Alignas(4096) uint8_t page[4096];
static const char *aspect = "16:9", *res = "1440";
static int win_w = 2560, win_h = 1440, fullscreen;
static GuestFn camera_host;
const char *cfg_get(const char *k) { (void)k; return NULL; }
const char *cfg_value(const char *k) {
    if (!strcmp(k, "NX2NAVI_GAME_ASPECT")) return aspect;
    if (!strcmp(k, "NX2NAVI_GAME_RES")) return res;
    return "";
}
int cfg_bool(const char *k) { (void)k; return 0; }
unsigned cfg_version(void) { return 1; }
int gfx_cfg_fullscreen(void) { return fullscreen; }
void gfx_cfg_window_size(int *w, int *h) { *w = win_w, *h = win_h; }
int gfx_window_monitor_size(int *w, int *h) { *w = win_w, *h = win_h; return 1; }
uint64_t mem_heap_extra_max_kib(void) { return 8ull << 20; }
void fs_init(void) {}
int res_heapdef_ok(void) { return 1; }
int vk_dlaa_available(void) { return 0; }
int vk_dlaa_fsr3(void) { return 0; }
int vk_dlaa_wanted(void) { return 0; }
void *rt_patch_page(void) { return page; }
static const char *const set_names[] = {"wide_out", "wide_scene", "wide_aspect", "wide_hud", "wide_movie", "wide_save",
                                        "wide_tall", "res_fixed"};
enum { NSETS = sizeof set_names / sizeof *set_names };
static int on[NSETS];
int rt_patch_set(const char *name) {
    for (int i = 0; i < NSETS; i++)
        if (!strcmp(name, set_names[i])) return i;
    return -1;
}
int rt_patch_enabled(int set) { return set >= 0 && on[set]; }
void rt_patch_switch(int set, int v) { if (set >= 0) on[set] = v; }
void rt_register_host(uint64_t addr, GuestFn fn) {
    if (addr == RT_PATCH_PAGE + HOST_TALL_CAMERA) camera_host = fn;
}
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("  log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

// ---- the lifted sites (widescreen_sites.c)
typedef struct { uint32_t off; const char *set; void (*fn)(Cpu *, uint64_t); } SiteFn;
extern const SiteFn site_fns[];
static void (*site(uint32_t off, const char *set))(Cpu *, uint64_t) {
    for (const SiteFn *s = site_fns; s->fn; s++)
        if (s->off == off && !strcmp(s->set, set)) return s->fn;
    printf("FAIL: no lifted site main+%#x of %s\n", off, set);
    exit(1);
}

static float S(const Cpu *c, int r) { uint32_t b = (uint32_t)c->v[r]; float f; memcpy(&f, &b, 4); return f; }
static void setS(Cpu *c, int r, float f) { uint32_t b; memcpy(&b, &f, 4); c->v[r] = b; }
static float lane(const Cpu *c, int r, int i) { float f[4]; memcpy(f, &c->v[r], 16); return f[i]; }
static void setQ(Cpu *c, int r, const float f[4]) { memcpy(&c->v[r], f, 16); }
static int near(double a, double b) { return fabs(a - b) <= 1e-5 * (fabs(b) > 1 ? fabs(b) : 1); }
static uint64_t P(const void *p) { return (uint64_t)(uintptr_t)p; }

// Runs wide_init for a game_aspect / game_res pair and a window, and returns the picture it set up (0x0 for the game's).
static void init(const char *a, const char *r, int ww, int wh, unsigned *pw, unsigned *ph) {
    memset(on, 0, sizeof on);
    memset(page, 0, sizeof page);
    aspect = a, res = r, win_w = ww, win_h = wh;
    *pw = *ph = 0;
    picture_size(pw, ph, 1);
    wide_init();
}

// Every register a variant changes, other than the ones it may: the rest of the Cpu stays as it was.
static int same_except(const Cpu *a, const Cpu *b, uint64_t xmask, uint64_t vmask) {
    for (int i = 0; i < 32; i++) {
        if (!(xmask >> i & 1) && a->x[i] != b->x[i]) return 0;
        if (!(vmask >> i & 1) && a->v[i] != b->v[i]) return 0;
    }
    return 1;
}

// ---- the matrix-update sites: (offset, base register, tangent and ratio registers, scratch v registers)
static const struct { uint32_t off; int base, tan, ratio; uint64_t scratch; } MATRIX[] = {
    {0x3485cc0, 0, 0, 1, 1u << 2 | 1u << 3 | 1u << 4},
    {0x4f1e8, 21, 0, 3, 1u << 2 | 1u << 4 | 1u << 5},
    {0x23b9a8, 0, 0, 3, 1u << 2 | 1u << 4 | 1u << 5},
    {0x33b82c, 2, 0, 1, 1u << 2 | 1u << 3 | 1u << 4},
};

static void check_matrix(const char *set, double ratio, double tan_k, int stores) {
    for (size_t m = 0; m < sizeof MATRIX / sizeof *MATRIX; m++) {
        for (int shape = 0; shape < 2; shape++) {   // a 16:9 projection, and a square one the sets leave alone
            float tan = 0.4663f, asp = shape ? 1.0f : 1.7777778f;
            uint8_t proj[0x100] = {0}, proj2[0x100];
            memcpy(proj + 0xac, &tan, 4);
            memcpy(proj + 0xb0, &asp, 4);
            memcpy(proj2, proj, sizeof proj);
            Cpu g, t;
            memset(&g, 0x33, sizeof g);
            g.x[MATRIX[m].base] = P(proj);
            t = g;
            site(MATRIX[m].off, "game")(&g, P(page));
            t.x[MATRIX[m].base] = P(proj2);
            site(MATRIX[m].off, set)(&t, P(page));
            t.x[MATRIX[m].base] = P(proj);
            int r = MATRIX[m].ratio;
            double want_r = shape ? asp : ratio, want_t = shape ? tan : tan * tan_k;
            CHECK(near(S(&t, r), want_r) && near(S(&t, 0), want_t), "%s main+%#x %s: ratio %f tangent %f, want %f %f",
                  set, MATRIX[m].off, shape ? "square" : "16:9", S(&t, r), S(&t, 0), want_r, want_t);
            CHECK(near(S(&g, r), asp) && near(S(&g, 0), tan), "game main+%#x: %f %f", MATRIX[m].off, S(&g, r), S(&g, 0));
            CHECK(same_except(&g, &t, 1u << 9, MATRIX[m].scratch | 1u << 0 | 1ull << r),
                  "%s main+%#x changes registers outside its own", set, MATRIX[m].off);
            float stored_r, stored_t;
            memcpy(&stored_r, proj2 + 0xb0, 4);
            memcpy(&stored_t, proj2 + 0xac, 4);
            double keep_r = stores && !shape ? ratio : asp;
            CHECK(near(stored_r, keep_r) && stored_t == tan, "%s main+%#x stores ratio %f tangent %f, want %f %f", set,
                  MATRIX[m].off, stored_r, stored_t, keep_r, tan);
        }
    }
}

// The camera update: the ratio site (both sets) and wide_tall's half-angle site with its host.
static void check_camera(double ratio, double tan_k) {
    for (int shape = 0; shape < 2; shape++) {
        uint8_t cam[0x200] = {0};
        float cr = shape ? 1.0f : 1.7777778f;
        memcpy(cam + 0x140, &cr, 4);
        Cpu g, t;
        memset(&g, 0x44, sizeof g);
        g.x[19] = P(cam);
        setS(&g, 0, 0.82f);   // the vertical angle, radians
        setS(&g, 1, 0.5f);
        t = g;
        site(0x4c52fc, "game")(&g, P(page));
        site(0x4c52fc, "wide_tall")(&t, P(page));
        double half = 0.41, want = shape ? half : atan(tan(half) * tan_k);
        CHECK(near(S(&g, 8), half) && near(S(&g, 0), 0.82), "game camera: half %f angle %f", S(&g, 8), S(&g, 0));
        CHECK(near(S(&t, 8), want) && near(S(&t, 0), 2 * want), "wide_tall camera %s: half %f angle %f, want %f",
              shape ? "square" : "16:9", S(&t, 8), S(&t, 0), want);
        CHECK(near(tan(S(&t, 8)), shape ? tan(half) : tan(half) * tan_k), "wide_tall camera: tangent %f",
              tan(S(&t, 8)));
        CHECK(same_except(&g, &t, 1u << 30, 1u << 0 | 1u << 8), "wide_tall camera changes registers outside s0, s8");
        CHECK((uint32_t)(t.v[8] >> 32) == 0 && (uint32_t)(t.v[0] >> 32) == 0, "wide_tall camera: upper lanes not 0");
        // the ratio the game stores after it
        Cpu r = g, r2 = g;
        site(0x4c5334, "game")(&r, P(page));
        site(0x4c5334, "wide_tall")(&r2, P(page));
        CHECK(near(S(&r2, 0), shape ? cr : ratio), "wide_tall camera ratio %f", S(&r2, 0));
    }
}

// The UI's canvas setup: the rows of a layout's projection in q1 and q4, the canvas in x0 (its screen at x0 - 0x1b8).
static void check_hud(const char *set, double sx, double sy) {
    uint64_t keep = MOD_MAIN_BASE + 0x4be4fb0, other = MOD_MAIN_BASE + 0x1234560;
    for (int k = 0; k < 3; k++) {   // 0 the full-screen projection, 1 the same on a screen that keeps it, 2 another
        uint64_t screen[0x40] = {0};
        screen[0] = k == 1 ? keep : other;
        float row0[4] = {k == 2 ? 2.0f / 1280 : 2.0f / 3840, 0, 0, -1}, row1[4] = {0, -2.0f / 2160, 0, 1};
        float rows[8];
        memcpy(rows, row0, 16), memcpy(rows + 4, row1, 16);
        Cpu g, t;
        memset(&g, 0x55, sizeof g);
        g.x[2] = P(rows);
        g.x[0] = P(screen) + 0x1b8;
        t = g;
        site(0x774368, "game")(&g, P(page));
        site(0x774368, set)(&t, P(page));
        double fx = k ? 1 : sx, fy = k ? 1 : sy;
        for (int i = 0; i < 4; i++) {
            CHECK(near(lane(&t, 1, i), row0[i] * fx) && near(lane(&t, 4, i), row1[i] * fy),
                  "%s UI case %d lane %d: %f %f, want %f %f", set, k, i, lane(&t, 1, i), lane(&t, 4, i), row0[i] * fx,
                  row1[i] * fy);
            CHECK(lane(&g, 1, i) == row0[i] && lane(&g, 4, i) == row1[i], "game UI lane %d", i);
        }
        CHECK(same_except(&g, &t, 1u << 16 | 1u << 17, 1u << 1 | 1u << 4 | 0xfu << 16), "%s UI changes other registers",
              set);
    }
}

// The movie layer's viewport at x29 - 40 (min x +8, min y +12, max x +16, max y +20).
static void check_movie(const char *set, unsigned w, unsigned h, double mx, double my) {
    float vp[8] = {0, 0, 0, 0, (float)w, (float)h, 0, 0};   // from x29 - 40: +8 is vp[2]
    float want[4] = {(float)(w * mx), (float)(h * my), (float)(w * (1 - mx)), (float)(h * (1 - my))};
    uint64_t layer[8] = {0};
    Cpu c;
    memset(&c, 0x66, sizeof c);
    c.x[29] = P(vp) + 40;
    c.x[19] = P(layer);
    site(0x3b5c758, set)(&c, P(page));
    for (int i = 0; i < 4; i++)
        CHECK(near(vp[2 + i], want[i]), "%s movie viewport field %d: %f, want %f", set, i, vp[2 + i], want[i]);
}

// The viewport helper's call: x, y, width and height in w1..w4, SetViewport's address behind x8.
static void check_save(const char *set, int dx, int dw, int dy, int dh) {
    uint64_t proc = 0x1234;
    for (int k = 0; k < 2; k++) {   // the save's 543x306 target, and another viewport the sites leave alone
        int vp[4] = {0, 0, k ? 640 : 543, k ? 360 : 306};
        Cpu c;
        memset(&c, 0x77, sizeof c);
        c.x[8] = P(&proc);
        for (int i = 0; i < 4; i++) c.x[1 + i] = (uint32_t)vp[i];
        site(0x284824, set)(&c, P(page));
        int want[4] = {vp[0] + (k ? 0 : dx), vp[1] + (k ? 0 : dy), vp[2] + (k ? 0 : dw), vp[3] + (k ? 0 : dh)};
        for (int i = 0; i < 4; i++)
            CHECK((int32_t)c.x[1 + i] == want[i], "%s save viewport %d field %d: %d, want %d", set, k, i,
                  (int32_t)c.x[1 + i], want[i]);
        CHECK(c.x[8] == proc, "%s save: the procedure %#llx", set, (unsigned long long)c.x[8]);
    }
}

static void expect_sets(const char *what, int out, int aspect_sets, int tall) {
    int hor = on[2] && on[3] && on[4];
    CHECK(on[0] == out && on[1] == out && hor == aspect_sets && on[6] == tall && !(tall && (on[2] || on[3] || on[4] || on[5])),
          "%s: sets out %d scene %d aspect %d hud %d movie %d save %d tall %d", what, on[0], on[1], on[2], on[3], on[4],
          on[5], on[6]);
}

int main(void) {
    unsigned w, h;
    CHECK(camera_host == wide_tall_camera_host, "wide_tall_camera_host is not registered at the page's +0xef0");
    CHECK(!strcmp(cfg_value("NX2NAVI_GAME_ASPECT"), "16:9"), "stub");

    // 16:9: the game's own picture, every set off
    init("16:9", "1440", 2560, 1440, &w, &h);
    expect_sets("16:9", 0, 0, 0);

    // 16:10 at each height of the menu's Resolution row (runtime/ui/menu.cpp row_resolution)
    static const unsigned heights[] = {1080, 1440, 1800, 2160, 2880}, widths[] = {1728, 2304, 2880, 3456, 4608};
    for (int i = 0; i < 5; i++) {
        char r[8];
        snprintf(r, sizeof r, "%u", heights[i]);
        init("16:10", r, 2560, 1440, &w, &h);
        CHECK(w == widths[i] && h == heights[i], "16:10 at %u: picture %ux%u", heights[i], w, h);
        expect_sets("16:10", 1, 0, 1);
        uint64_t out;
        memcpy(&out, page + 96, 8);
        CHECK(out == ((uint64_t)heights[i] << 32 | widths[i]), "16:10 page +96 %#llx", (unsigned long long)out);
    }

    // 16:10 at 1440: the sites
    init("16:10", "1440", 2560, 1440, &w, &h);
    double r1610 = 2304.0 / 1440, k1610 = (16.0 / 9) / r1610;
    float tan_k;
    memcpy(&tan_k, page + 124, 4);
    CHECK(near(tan_k, k1610), "16:10 tangent factor %f", tan_k);
    check_matrix("wide_tall", r1610, k1610, 0);
    check_camera(r1610, k1610);
    check_hud("wide_tall", 1, r1610 / (16.0 / 9));
    check_movie("wide_tall", 2304, 1440, 0, (1 - r1610 / (16.0 / 9)) / 2);
    int dh = 2 * (int)lround(306 * (k1610 - 1) / 2);
    check_save("wide_tall", 0, 0, -dh / 2, dh);
    CHECK(dh == 34, "16:10 save: %d rows added", dh);   // 306 * 10/9 = 340

    // The wider shapes: the sets as before, wide_tall off; the page's tangent factor 1
    init("21:9", "1440", 2560, 1440, &w, &h);
    CHECK(w == 3416 && h == 1440, "21:9 picture %ux%u", w, h);   // 64:27, in whole multiples of 8 pixels
    expect_sets("21:9", 1, 1, 0);
    CHECK(on[5], "21:9: wide_save off");
    double r219 = (double)w / h;
    memcpy(&tan_k, page + 124, 4);
    CHECK(tan_k == 1.0f, "21:9 tangent factor %f", tan_k);
    check_matrix("wide_aspect", r219, 1, 1);
    check_hud("wide_hud", (16.0 / 9) / r219, 1);
    check_movie("wide_movie", w, h, (1 - (16.0 / 9) / r219) / 2, 0);
    int dw = 2 * (int)lround(543 * (r219 / (16.0 / 9) - 1) / 2);
    check_save("wide_save", -dw / 2, dw, 0, 0);
    // the 21:9 page through wide_tall's words: the same wide view (its tangent factor is 1), as a fallback would give
    check_matrix("wide_tall", r219, 1, 0);

    init("32:9", "1440", 2560, 1440, &w, &h);
    CHECK(w == 5120 && h == 1440, "32:9 picture %ux%u", w, h);
    expect_sets("32:9", 1, 1, 0);

    // The window's shape: 16:10, 5:4 (narrower: a 16:10 picture), 1366x768 (16:9 within 1%) and 21:9
    init("window", "1440", 1920, 1200, &w, &h);
    CHECK(w == 2304 && h == 1440, "window 1920x1200: picture %ux%u", w, h);
    expect_sets("window 16:10", 1, 0, 1);
    init("window", "1440", 1280, 1024, &w, &h);
    CHECK(w == 2304 && h == 1440, "window 1280x1024: picture %ux%u", w, h);
    expect_sets("window 5:4", 1, 0, 1);
    init("window", "1440", 1366, 768, &w, &h);
    expect_sets("window 1366x768", 0, 0, 0);
    init("window", "window", 2560, 1600, &w, &h);
    CHECK(w == 2560 && h == 1600, "window 2560x1600 at its own height: picture %ux%u", w, h);
    expect_sets("window 2560x1600", 1, 0, 1);
    init("window", "1440", 3440, 1440, &w, &h);
    CHECK(w == 3440 && h == 1440, "window 3440x1440: picture %ux%u", w, h);
    expect_sets("window 21:9", 1, 1, 0);

    // save_extra on its own: nothing for 16:9
    CHECK(save_extra(2560, 1440) == 0 && save_extra(1920, 1080) == 0, "save_extra of a 16:9 picture");

    printf("widescreen_check: %s\n", fails ? "FAILED" : "all checks passed");
    return fails != 0;
}
