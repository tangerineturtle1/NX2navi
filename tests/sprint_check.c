// The sprint button (runtime/sprint.c) against a stand-in Link: the presses and the button the game gets from each
// controller poll (sprint_pad), and the dash input the patch set's two host functions put into Link's flags, frame by
// frame, for toggle and hold, each button, Link's states and a Link the player does not control. Built by
// tests/sprint_check.py, which writes the stand-in layout.h and patches.h.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

uint64_t test_main_base;   // layout.h's MOD_MAIN_BASE: a buffer standing in for main's image
const int rt_game_version = 111;

#include "sprint.c"

static const char *button = "off", *mode = "toggle";
static GuestFn link_fn, stance_fn;
const char *cfg_value(const char *key) {
    if (!strcmp(key, "NX2NAVI_SPRINT_BUTTON")) return button;
    if (!strcmp(key, "NX2NAVI_SPRINT_MODE")) return mode;
    return "";
}
static int set_on = 1;   // the patch set sprint, on at the start as its default
int rt_patch_set(const char *name) { return strcmp(name, "sprint") ? -1 : 3; }
int rt_patch_enabled(int set) { return set == 3 && set_on; }
void rt_patch_switch(int set, int on) { assert(set == 3); set_on = on != 0; }
void rt_register_host(uint64_t addr, GuestFn fn) {
    if (addr == RT_PATCH_PAGE + 0xf40) link_fn = fn;
    else if (addr == RT_PATCH_PAGE + 0xf30) stance_fn = fn;
    else abort();
}

static float step = 1.0f;   // frame_step_frames: the 60 Hz frames of one update of Link
float frame_step_frames(void) { return step; }
int mem_readable(uint64_t a, uint64_t n) { (void)n; return a != 0; }

// Link: vtable, +0x18 -> owner whose +0x270 -> a with +0x188 -> b with +0x1c0 -> the state, id at +0x28
static uint64_t link[8], owner[0x50], a[0x40], b[0x40], state_obj[0x20], other[8];
static uint64_t manager[0x90], controlled[8];

static void set_state(int32_t id) { memcpy((char *)state_obj + 0x28, &id, 4); }

// One frame: a controller poll with these buttons, then Link's update. Returns what the game got from the poll.
static uint64_t poll_buttons, flags_x10, flags_w8;
static uint64_t frame(uint64_t buttons, int32_t state, uint64_t *who) {
    set_state(state);
    sprint_pad(&buttons);
    poll_buttons = buttons;
    Cpu c;
    memset(&c, 0, sizeof c);
    c.x[19] = (uintptr_t)who;
    c.x[10] = 0x40;   // other flag bits stay
    if (set_on) link_fn(&c);   // the sites call in only while the set is on
    flags_x10 = c.x[10];
    c.x[8] = 0x01;
    if (set_on) stance_fn(&c);
    flags_w8 = c.x[8];
    return buttons;
}
static int dashing(void) { return (flags_x10 & 0x10) && (flags_w8 & 0x10) && (flags_x10 & 0x40) && (flags_w8 & 1); }

enum { WALK = 1, JUMP = 13, SLIDE = 35, WAIT_ID = 40, ROLL = 99, ATTACK = 112, CUTSCENE = 0 };
#define L3 (1u << 4)
#define BB (1u << 1)
#define LL (1u << 6)
#define ZL (1u << 8)

static void reset(const char *btn, const char *md) {
    button = btn, mode = md;
    for (int i = 0; i < 12; i++) frame(0, CUTSCENE, link);   // the settings change and every sprint ends
    assert(!dashing());
}

int main(int argc, char **argv) {
    char *img = calloc(1, 0x4c80000);
    assert(img);
    test_main_base = (uintptr_t)img;
    assert(link_fn && stance_fn);
    if (argc > 1 && !strcmp(argv[1], "kept-off")) {
        // off at the start (NX2NAVI_SPRINT_BUTTON_PATCH=0, or a mod patching its sites): no rows, never switched on, and the
        // button always reaches the game
        set_on = 0, button = "l3";
        assert(!sprint_available());
        uint64_t bt = L3;
        for (int i = 0; i < 20; i++) bt = L3, sprint_pad(&bt);
        assert(!set_on && bt == L3);
        puts("sprint_check kept-off: ok");
        return 0;
    }
    assert(sprint_available());
    link[0] = test_main_base + 0x4b4dfc0, link[3] = (uintptr_t)owner;
    other[0] = test_main_base + 0x4b4dfc0, other[3] = (uintptr_t)a;   // Link's class, another owner
    owner[0x270 / 8] = (uintptr_t)a, a[0x188 / 8] = (uintptr_t)b, b[0x1c0 / 8] = (uintptr_t)state_obj;
    // the actor manager names the controlled Link (the one whose +0x18 is owner)
    uint64_t *slot = (uint64_t *)(img + 0x4c7e0d0);
    uint64_t mgr_ptr = (uintptr_t)manager;
    *slot = (uintptr_t)&mgr_ptr;
    manager[0] = test_main_base + 0x4bdfe68, manager[0x470 / 8] = (uintptr_t)controlled;
    controlled[0] = test_main_base + 0x4b49698, controlled[3] = (uintptr_t)owner;

    // off: nothing changes, the button reaches the game, and the set's sites stop calling in
    reset("off", "toggle");
    assert(frame(L3, WALK, link) == L3 && !dashing() && !set_on);
    button = "l3";
    frame(0, WALK, link);
    assert(set_on);   // a button chosen: the set runs again

    // toggle on L3: a press while walking starts it; the button never reaches the game during play
    reset("l3", "toggle");
    assert(frame(L3, WALK, link) == 0 && dashing());
    assert(frame(L3, WALK, link) == 0 && dashing());   // held: no second press
    assert(frame(0, WALK, link) == 0 && dashing());
    assert(frame(0, JUMP, link) == 0 && dashing());     // on through a jump
    assert(frame(0, SLIDE, link) == 0 && dashing());
    frame(L3, WALK, link);                               // the next press ends it
    assert(!dashing());
    frame(0, WALK, link);
    assert(!dashing());

    // toggle: standing still ends it after 4 frames once it has moved
    frame(L3, WALK, link), frame(0, WALK, link);
    for (int i = 0; i < 4; i++) assert((frame(0, WAIT_ID, link), dashing()));
    frame(0, WAIT_ID, link);
    assert(!dashing());
    // ... a press while standing waits up to 90 frames for Link to move
    frame(L3, WAIT_ID, link), frame(0, WAIT_ID, link);
    assert(dashing());
    for (int i = 0; i < 60; i++) frame(0, WAIT_ID, link);
    assert(dashing());
    frame(0, WALK, link);
    assert(dashing());
    frame(L3, WALK, link);
    assert(!dashing());
    frame(L3, WAIT_ID, link), frame(0, WAIT_ID, link);
    for (int i = 0; i < 90; i++) frame(0, WAIT_ID, link);
    assert(!dashing());

    // toggle: an attack, a roll, targeting or a cutscene ends it at once
    const int32_t enders[] = {ATTACK, ROLL, CUTSCENE};
    for (int k = 0; k < 3; k++) {
        frame(L3, WALK, link), frame(0, WALK, link);
        assert(dashing());
        frame(0, enders[k], link);
        assert(!dashing());
        frame(0, WALK, link);
        assert(!dashing());
    }
    frame(L3, WALK, link), frame(0, WALK, link);
    assert(frame(ZL, WALK, link) == ZL && !dashing());   // ZL reaches the game and targets

    // a press during an attack does not start it
    frame(L3, ATTACK, link);
    assert(!dashing());

    // Link's class without the player's control: no dash input, and the controlled Link keeps his sprint
    frame(0, WALK, link), frame(L3, WALK, link);
    assert(dashing());
    frame(0, WALK, other);
    assert(!(flags_x10 & 0x10) && !(flags_w8 & 0x10));
    frame(0, WALK, link);
    assert(dashing());
    frame(L3, WALK, link);

    // a menu or a load: Link's update stops running, and after 8 polls the button reaches the game again
    {
        uint64_t got = 0;
        sprint_pad(&got);   // released first, so the next poll is a press
        for (int i = 0; i < 9; i++) {
            uint64_t bt = L3;
            sprint_pad(&bt);
            got = bt;
        }
        assert(got == L3);
        frame(0, WALK, link);   // the next update takes the press made meanwhile
        assert(dashing());
        frame(L3, WALK, link);
        assert(!dashing());
    }

    // toggle on the bottom button: a press while not moving (put the weapon away) reaches the game and starts nothing
    reset("b", "toggle");
    assert(frame(BB, WAIT_ID, link) == BB && !dashing());
    frame(0, WALK, link);   // Link moves: from the next poll the button is the sprint's
    assert(frame(BB, WALK, link) == 0 && dashing());
    assert(frame(0, WALK, link) == 0 && dashing());
    frame(BB, WALK, link);
    assert(!dashing() && poll_buttons == 0);
    frame(0, ATTACK, link);
    assert(frame(BB, ATTACK, link) == BB && !dashing());   // not moving: the button keeps its own job

    // hold on L3: sprint while held; L3 never reaches the game during play, other buttons do
    reset("l3", "hold");
    assert(frame(L3, WALK, link) == 0 && dashing());
    assert(frame(L3, WAIT_ID, link) == 0 && dashing());
    assert(frame(0, WALK, link) == 0 && !dashing());
    assert(frame(LL, WALK, link) == LL && !dashing());

    // a button the setting no longer offers is off
    reset("r3", "toggle");
    assert(frame(1u << 5, WALK, link) == (1u << 5) && !dashing() && !set_on);

    // hold on the bottom button: starts only while moving, keeps going while held
    reset("b", "hold");
    frame(0, WALK, link);
    assert(frame(BB, WALK, link) == 0 && dashing());
    assert((frame(BB, JUMP, link), dashing()));
    assert(frame(0, WALK, link) == 0 && !dashing());
    frame(0, WAIT_ID, link);
    assert(frame(BB, WAIT_ID, link) == BB && !dashing());
    // a press begun while standing stays the game's for its whole length, also once Link moves: no sprint, no strip
    assert(frame(BB, WALK, link) == BB && !dashing());
    assert(frame(BB, WALK, link) == BB && !dashing());
    frame(0, WALK, link);
    // a press begun while moving stays the sprint's when Link stops: the game gets no B from it
    assert(frame(BB, WALK, link) == 0 && dashing());
    assert(frame(BB, WAIT_ID, link) == 0);
    frame(0, WAIT_ID, link);

    // a press made in a menu (no update of Link for more than 8 polls) starts nothing when play goes on
    reset("l3", "toggle");
    frame(0, WALK, link);
    {
        uint64_t bt = 0;
        for (int i = 0; i < 10; i++) bt = 0, sprint_pad(&bt);
        bt = L3, sprint_pad(&bt);
        assert(bt == L3);   // the menu's
        bt = 0, sprint_pad(&bt);
    }
    frame(0, WALK, link);
    assert(!dashing());

    // the stand-still timings count 60 Hz frames: at 240 fps (a quarter frame an update) a stop of four updates goes on
    reset("l3", "toggle");
    step = 0.25f;
    frame(L3, WALK, link), frame(0, WALK, link);
    for (int i = 0; i < 4; i++) frame(0, WAIT_ID, link);
    assert(dashing());
    for (int i = 0; i < 16; i++) frame(0, WAIT_ID, link);
    assert(!dashing());
    step = 1.0f;

    // switched off with the sprint on, and on again: it does not come back by itself
    reset("l3", "toggle");
    frame(L3, WALK, link), frame(0, WALK, link);
    assert(dashing());
    button = "off", frame(0, WALK, link);
    button = "l3", frame(0, WALK, link);
    assert(!dashing());

    puts("sprint_check: ok");
    free(img);
    return 0;
}
