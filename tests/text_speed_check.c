// The text_speed setting (runtime/text_speed.c) and the patch set text_speed's lifted replacement words against a
// stand-in message window and typewriter: what each setting puts on the patch page and how it switches the set; the
// skip site's typewriter word and decide bit, for a message the window lets the player skip and one it does not; the
// speed site's scaled speed for the window's typewriter and for any other; and that both sites change nothing else.
// Built by tests/text_speed_check.py, which lifts the words into sites.c. With the argument kept-off the set starts
// off (NX2NAVI_TEXT_SPEED_PATCH=0 or a mod on its sites), and no setting may switch it on or touch the page.
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "text_speed.c"

void site_skip(Cpu *c);    // main+0x774b74 without its adrp (x9 = the page) and its last tbz w9, #0
void site_speed(Cpu *c);   // main+0xafe1d8 without its adrp (x22 = the page)

static const char *speed = "normal";
const char *cfg_value(const char *key) { return strcmp(key, "NX2NAVI_TEXT_SPEED") ? "" : speed; }
static int set_on = 1;   // the patch set text_speed, on at the start as its default
int rt_patch_set(const char *name) { return strcmp(name, "text_speed") ? -1 : 7; }
int rt_patch_enabled(int set) { return set == 7 && set_on; }
void rt_patch_switch(int set, int on) { assert(set == 7); set_on = on != 0; }
static _Alignas(4096) uint8_t page[4096];
void *rt_patch_page(void) { return page; }
void rt_log(const char *fmt, ...) { (void)fmt; }

static uint8_t window[0x400];             // the message window: +0x34a 1 when its message may not be skipped
static _Alignas(16) uint8_t typewriter[0x80], other[0x80];   // typewriters: +0x4c the speed

static float page_float(int off) { float f; memcpy(&f, page + off, 4); return f; }
static uint64_t page_word(int off) { uint64_t v; memcpy(&v, page + off, 8); return v; }
static float lane(const Cpu *c, int v) { float f; memcpy(&f, &c->v[v], 4); return f; }

static void fill(Cpu *c, unsigned seed) {
    uint8_t *p = (uint8_t *)c;
    for (size_t i = 0; i < sizeof *c; i++) p[i] = (uint8_t)(seed * 131u + i * 7u);
    c->nzcv = (uint64_t)(seed & 15) << 28;
}

// The skip site for the window and its typewriter: returns the decide bit the site's tbz tests, and checks that only
// x9, x10 and the page's typewriter word changed.
static int run_skip(int unskippable, int pressed, unsigned seed) {
    Cpu c, before;
    fill(&c, seed);
    window[0x34a] = (uint8_t)unskippable;
    c.x[19] = (uintptr_t)window;
    c.x[8] = (uintptr_t)typewriter;
    c.x[20] = (c.x[20] & ~1ull) | (uint64_t)pressed;
    c.x[9] = (uintptr_t)page;   // the site's adrp
    before = c;
    site_skip(&c);
    assert(page_word(0x200) == (unskippable ? 0 : (uint64_t)(uintptr_t)typewriter));
    for (int r = 0; r < 31; r++)
        if (r != 9 && r != 10) assert(c.x[r] == before.x[r]);
    assert(c.nzcv == before.nzcv && c.sp == before.sp);
    assert(!memcmp(c.v, before.v, sizeof c.v));
    return (int)(c.x[9] & 1);
}

// The speed site for a typewriter: returns the speed it loads into s1, and checks that only x27, s1, s2 and s8 changed.
static float run_speed(uint8_t *tw, float base, unsigned seed) {
    Cpu c, before;
    fill(&c, seed);
    memcpy(tw + 0x4c, &base, 4);
    c.x[19] = (uintptr_t)tw;
    c.x[22] = (uintptr_t)page;   // the site's adrp
    before = c;
    site_speed(&c);
    for (int r = 0; r < 31; r++)
        if (r != 27) assert(c.x[r] == before.x[r]);
    assert(c.nzcv == before.nzcv && c.sp == before.sp);
    for (int v = 0; v < 32; v++)
        if (v != 1 && v != 2 && v != 8) assert(!memcmp(&c.v[v], &before.v[v], sizeof c.v[v]));
    return lane(&c, 1);
}

static void kept_off(void) {
    set_on = 0;
    memset(page, 0xa5, sizeof page);
    const char *all[] = {"normal", "fast", "instant", "normal"};
    for (int i = 0; i < 4; i++) {
        speed = all[i];
        text_speed_poll();
        assert(!set_on && !text_speed_available());
        for (size_t k = 0; k < sizeof page; k++) assert(page[k] == 0xa5);
    }
    printf("text_speed_check: a set kept off stays off and the page untouched\n");
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "kept-off")) {
        kept_off();
        return 0;
    }
    // normal: the set goes off, the page holds no factor and no skip
    speed = "normal";
    text_speed_poll();
    assert(text_speed_available() && !set_on && page_float(0x208) == 0.0f && page[0x20c] == 0);
    // An unknown value reads as normal.
    speed = "turbo";
    text_speed_poll();
    assert(!set_on && page_float(0x208) == 0.0f && page[0x20c] == 0);

    // fast: the set on, the factor less one on the page, no skip
    speed = "fast";
    text_speed_poll();
    assert(set_on && page_float(0x208) == TEXT_SPEED_FAST_FACTOR - 1.0f && page[0x20c] == 0);
    for (unsigned seed = 1; seed < 40; seed++) {
        int pressed = seed & 1;
        // a message the player may skip: the window's typewriter goes on the page and types faster; the decide bit
        // is the player's own
        assert(run_skip(0, pressed, seed) == pressed);
        float base = 0.25f * (float)(seed % 9 + 1);
        assert(fabsf(run_speed(typewriter, base, seed) - base * TEXT_SPEED_FAST_FACTOR) < 1e-5f);
        // any other typewriter keeps its speed
        assert(run_speed(other, base, seed) == base);
        // a message the window does not let the player skip: no typewriter on the page, the game's speed
        assert(run_skip(1, pressed, seed) == pressed);
        assert(run_speed(typewriter, base, seed) == base);
    }

    // instant: the set on, no factor, the decide bit set for the skip test (whose +0x34a test is the game's own)
    speed = "instant";
    text_speed_poll();
    assert(set_on && page_float(0x208) == 0.0f && page[0x20c] == 1);
    for (unsigned seed = 1; seed < 40; seed++) {
        assert(run_skip(0, seed & 1, seed) == 1);
        assert(run_speed(typewriter, 1.5f, seed) == 1.5f);
        assert(run_skip(1, seed & 1, seed) == 1);
        assert(run_speed(typewriter, 1.5f, seed) == 1.5f);
    }

    // back to normal: the set off again (the sites run the original instructions), the page cleared
    speed = "normal";
    text_speed_poll();
    assert(!set_on && page_float(0x208) == 0.0f && page[0x20c] == 0);
    printf("text_speed_check: settings, page words and both sites as expected\n");
    return 0;
}
