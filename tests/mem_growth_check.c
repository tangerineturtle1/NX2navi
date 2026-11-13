// runtime/mem_growth.h: the memory report's growth bookkeeping. Built and run by tests/mem_growth_check.py; prints each
// failed check and exits 1 on any failure.
//
// The checks feed the book reports as vk_memory.c growth_log does, once a period: a boot and a title screen before the
// world clock runs, then play. A session shaped like the 15-minute log of 2026-11-13 (logs-b10-play: the textures and
// the device-local memory jump by gigabytes at the first area, then go up and down with the areas; the process's commit
// follows them) is checked for what the growth line and the ranking say.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mem_growth.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MB(n) ((uint64_t)(n) << 20)

enum { DEV, TEX, COMMIT, SHADERS, FLAT, LEAK, N };

static MgBook book(void) {
    MgBook b;
    memset(&b, 0, sizeof b);
    static const char *const names[N] = {"device-local in use", "textures", "process commit", "shader caches", "log queue",
                                         "other"};
    for (int i = 0; i < N; i++) CHECK(mg_cat(&b, names[i]) == i, "category %d", i);
    return b;
}

static void take(MgBook *b, uint64_t dev, uint64_t tex, uint64_t commit, uint64_t sh, uint64_t flat, uint64_t leak,
                 int play) {
    uint64_t v[N] = {MB(dev), MB(tex), MB(commit), MB(sh), MB(flat), MB(leak)};
    mg_take(b, v, play);
}

static void basics(void) {
    MgBook b = book();
    char line[1024];
    // the first report: nothing to compare with
    take(&b, 2444, 84, 15943, 741, 2, 4000, 0);
    CHECK(mg_grew(&b, MB(64), line, sizeof line) == 0 && !line[0], "first report named '%s'", line);
    CHECK(mg_rank(&b, line, sizeof line) == 0 && !line[0], "rank before play wrote '%s'", line);
    // before play: only the previous report counts, and more than the step
    take(&b, 2476, 148, 16007, 741, 2, 4000, 0);   // +32 +64 +64: none past "more than 64"
    CHECK(mg_grew(&b, MB(64), line, sizeof line) == 0, "+64 MiB named: '%s'", line);
    take(&b, 2476, 213, 16007, 741, 2, 4000, 0);   // textures +65
    CHECK(mg_grew(&b, MB(64), line, sizeof line) == 1, "one category: '%s'", line);
    CHECK(!strcmp(line, "since the previous report: textures +65 MiB"), "line '%s'", line);
    CHECK(!b.play && b.play_lines == 0, "no base yet");
    // the base: first figures, no growth since play yet
    take(&b, 1866, 213, 15371, 741, 2, 4000, 1);
    CHECK(b.play && b.play_lines == 1, "base taken");
    CHECK(mg_grew(&b, MB(64), line, sizeof line) == 0, "fall named: '%s'", line);
    CHECK(mg_rank(&b, line, sizeof line) == 0 && !strcmp(line, "nothing grew; within 1 MiB of the base: 6 more"),
          "rank at the base '%s'", line);
    // play: both lists
    take(&b, 5199, 3500, 19921, 760, 2, 4100, 1);
    int n = mg_grew(&b, MB(64), line, sizeof line);
    CHECK(n == 8, "named %d: '%s'", n, line);
    CHECK(!strcmp(line, "since the previous report: device-local in use +3333 MiB, textures +3287 MiB, process commit "
                        "+4550 MiB, other +100 MiB; since play began: device-local in use +3333 MiB (only climbs), "
                        "textures +3287 MiB (only climbs), process commit +4550 MiB (only climbs), other +100 MiB "
                        "(only climbs)"),
          "line '%s'", line);
    // a later report with no change: the second list stays, the first is empty
    take(&b, 5199, 3500, 19921, 760, 2, 4100, 1);
    n = mg_grew(&b, MB(64), line, sizeof line);
    static const char flat[] = "since play began: device-local in use +3333 MiB (only climbs), textures";
    CHECK(n == 4 && !strncmp(line, flat, sizeof flat - 1), "flat '%s'", line);
    // the world clock stops (title screen): still in play
    take(&b, 4000, 2300, 18700, 760, 2, 4200, 0);
    CHECK(b.play_lines == 4, "play lines %u", b.play_lines);
    CHECK(!mg_only_climbs(&b, &b.c[DEV]) && !mg_only_climbs(&b, &b.c[TEX]), "a fall ends only climbs");
    CHECK(mg_only_climbs(&b, &b.c[LEAK]), "the leak only climbs");
    CHECK(!mg_only_climbs(&b, &b.c[FLAT]) && mg_only_climbs(&b, &b.c[SHADERS]), "the log queue stays, the shader caches rose once");
    mg_rank(&b, line, sizeof line);
    CHECK(!strcmp(line, "process commit +3329 MiB, device-local in use +2134 MiB, textures +2087 MiB, other +200 MiB "
                        "(only climbs), shader caches +19 MiB (only climbs); within 1 MiB of the base: 1 more"),
          "rank '%s'", line);
}

// Changes under MG_NOISE are neither rises nor falls; a fall of MG_NOISE ends "only climbs".
static void noise(void) {
    MgBook b = book();
    char line[512];
    take(&b, 0, 0, 0, 0, 0, 100, 1);
    uint64_t v[N] = {0};
    v[LEAK] = MB(100) + MB(1) - 1;   // just under a MiB up
    mg_take(&b, v, 1);
    CHECK(b.c[LEAK].ups == 0 && !mg_only_climbs(&b, &b.c[LEAK]), "under a MiB is no rise");
    v[LEAK] = MB(101) + MB(1) - 1;   // a MiB up from there
    mg_take(&b, v, 1);
    CHECK(b.c[LEAK].ups == 1 && mg_only_climbs(&b, &b.c[LEAK]), "a MiB is a rise");
    v[LEAK] = MB(100) + MB(1) - 1;   // a MiB down
    mg_take(&b, v, 1);
    CHECK(b.c[LEAK].downs == 1 && !mg_only_climbs(&b, &b.c[LEAK]), "a MiB down ends only climbs");
    mg_rank(&b, line, sizeof line);
    CHECK(!strcmp(line, "nothing grew; within 1 MiB of the base: 6 more"), "rank '%s'", line);
}

// Falls are ranked largest first after the growth; a cut buffer stays terminated and the count is still right.
static void falls_and_cut(void) {
    MgBook b = book();
    char line[2048];
    take(&b, 1000, 1000, 1000, 1000, 1000, 1000, 1);
    take(&b, 900, 1500, 400, 1000, 1000, 1200, 1);
    int g = mg_rank(&b, line, sizeof line);
    CHECK(g == 2, "grew %d", g);
    CHECK(!strcmp(line, "textures +500 MiB (only climbs), other +200 MiB (only climbs); fell: process commit -600 MiB, "
                        "device-local in use -100 MiB; within 1 MiB of the base: 2 more"),
          "rank '%s'", line);
    char small[24];
    memset(small, 'x', sizeof small);
    g = mg_rank(&b, small, sizeof small);
    CHECK(g == 2 && strlen(small) == sizeof small - 1 && !strncmp(small, line, sizeof small - 1), "cut '%s'", small);
    memset(small, 'x', sizeof small);
    int n = mg_grew(&b, MB(64), small, sizeof small);
    CHECK(n == 4 && strlen(small) == sizeof small - 1, "grew cut %d '%s'", n, small);   // textures and other, twice
    CHECK(mg_grew(&b, MB(64), NULL, 0) == 4 && mg_rank(&b, NULL, 0) == 2, "no buffer");
}

// The 15-minute session of 2026-11-13 at 1440p, one report a minute: boot and title screen (the outside-VMA memory falls
// by 610 MiB when the boot's compute pipelines go), then play, where the textures and the device-local memory move with
// the areas and the commit with them. The figures are the log's mem: by owner and mem: host lines.
static void session(void) {
    static const struct { uint64_t dev, tex, commit; int play; } s[] = {
        {2444, 84, 15943, 0},   {2476, 92, 15984, 0},   {1866, 92, 15371, 0},   {1866, 92, 15363, 0},
        {5199, 3400, 19921, 1}, {6039, 4100, 20975, 1}, {6822, 4800, 21967, 1}, {7042, 5000, 22190, 1},
        {6906, 4850, 22044, 1}, {6671, 4600, 21817, 1}, {7558, 5500, 22952, 1}, {6629, 4500, 21829, 1},
        {6786, 4700, 22031, 1}, {6408, 4300, 21581, 1}, {6320, 4200, 21481, 1}, {6334, 4250, 21500, 1},
    };
    MgBook b = book();
    char line[1024];
    int lines_named = 0;
    for (size_t i = 0; i < sizeof s / sizeof *s; i++) {
        take(&b, s[i].dev, s[i].tex, s[i].commit, 741 + (s[i].play ? 19 : 0), 2, 3000 + 40 * (uint64_t)i, s[i].play);
        lines_named += mg_grew(&b, MB(64), line, sizeof line) > 0;
    }
    // the base is the first report in play, after the jump: the textures grew by 850 MiB since, and came down twice
    CHECK(b.c[TEX].first == MB(3400) && b.c[TEX].last == MB(4250), "textures %llu %llu",
          (unsigned long long)b.c[TEX].first >> 20, (unsigned long long)b.c[TEX].last >> 20);
    CHECK(!mg_only_climbs(&b, &b.c[TEX]) && !mg_only_climbs(&b, &b.c[COMMIT]), "areas come and go");
    CHECK(mg_only_climbs(&b, &b.c[LEAK]), "the steady climb is a leak candidate");
    mg_rank(&b, line, sizeof line);
    CHECK(!strcmp(line, "process commit +1579 MiB, device-local in use +1135 MiB, textures +850 MiB, other +440 MiB "
                        "(only climbs); within 1 MiB of the base: 2 more"),
          "rank '%s'", line);
    CHECK(lines_named == 12, "%d reports named a growth", lines_named);   // the base and every report after it
}

int main(void) {
    basics();
    noise();
    falls_and_cut();
    session();
    if (fails) printf("%d checks failed\n", fails);
    else printf("mem_growth: all checks passed\n");
    return fails != 0;
}
