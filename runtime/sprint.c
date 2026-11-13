// Sprint button for Ocarina of Time 1.1.1 (the patch set sprint, recomp/patches-v111.txt). Link sprints while his
// input flags hold the dash input (bit 4 of the word at *(Link +0x50)). The set's two sites call the host functions
// below right after the game loads those flags, and they OR the bit in while the sprint button's sprint is on, so the
// game's own sprint runs: its speed, animation, camera and every rule that ends it.
//
// Settings (live): sprint_button picks the button (off, b the game's B button, the bottom face button of the Nintendo
// layout, or l3 the left stick's click, which has no other use); sprint_mode is toggle (a press starts the sprint, the
// next press or Link's state ends it) or hold (sprint while the button is held). During gameplay the button is kept
// from the game (sprint_pad, called by ov_input.c npad_states), so it does not also fire its own action. B is kept only
// for a press that begins while Link moves (Walk, Jump, Land, Fall, Clamber), and then until it is released, so it
// puts the weapon away as usual otherwise and the game never sees half a press. Presses made in a menu, a cutscene or
// a load (no update of Link within GAMEPLAY_POLLS polls) start nothing: B closes the pause menu, and that press is the
// menu's. Under the game's Swap Jump Controls B is the jump, which a B sprint button keeps from the game while Link
// moves: L3 is the one to use then.
// Patch page slots: the set's sites call +0xf40 and +0xf30; +0xf20 and +0xf50 to +0xff0 belong to other sets
// (runtime/patches.c, mouse_direct.c, ov_zstd.c). A slot registered twice ends the start (dispatch.c rt_register_host).
//
// Link's state: the id at [[[[Link +0x18] +0x270] +0x188] +0x1c0] +0x28 (+0x4c when that reads 0x11a, a state that
// wraps another), the same field main+0x4aed50 compares with 1 (Walk) before the game starts a sprint.
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "layout.h"
#include "sprint.h"

#if __has_include("patches.h")
#include "patches.h"
#endif

enum { BTN_OFF, BTN_B, BTN_L3 };
static const char *const button_names[] = {"off", "b", "l3"};
static const uint64_t button_bits[] = {0, 1u << 1, 1u << 4};   // HidNpadButton bits
#define BUTTON_ZL (1u << 8)

#define DASH_INPUT 0x10u
#define LINK_VTABLE 0x4b4dfc0ull   // main offset: Link's actor class
#define GAMEPLAY_POLLS 8           // the button is kept from the game while Link's update ran within this many polls
// Standing still this long ends a toggled sprint that has moved, or this long after a press while standing, which has
// not moved yet. In 60 Hz frames: above 60 fps Link updates once a rendered frame, each step frame_step_frames() of
// them, so counting updates would end the sprint on any short stop at 240 fps (four updates are 17 ms there).
#define WAIT_STOP_FRAMES 4.0
#define WAIT_START_FRAMES 90.0

// What Link's state does to a sprint: MOVE keeps it (and is when the bottom button sprints), KEEP keeps it,
// WAIT ends it after a few frames, STOP ends it at once. A state not listed stops it too.
enum { MOVE, KEEP, WAIT, STOP };
static const struct { int32_t id; uint8_t kind; } states[] = {
    {1, MOVE},     // Walk (also running)
    {13, MOVE},    // Jump
    {32, MOVE},    // Land
    {33, MOVE},    // Fall
    {68, MOVE},    // Clamber
    {253, KEEP},   // StepOn
    {35, KEEP},    // Slide
    {36, KEEP},    // SlideLand
    {40, WAIT},    // Wait
};

static _Atomic uint64_t pressed;   // the button's presses since Link's last update (sprint_pad -> sprint_link)
static _Atomic int held;           // the button held at the last poll
static _Atomic int zl_held;        // ZL (targeting) held at the last poll
static _Atomic int polls = GAMEPLAY_POLLS;   // polls since Link's last update (none yet: not gameplay)
static _Atomic int moving;         // Link's state was a MOVE one at his last update
static _Atomic int active;         // the sprint is on: the dash input goes in
static _Atomic int latched;        // the press now held began in gameplay (and, for B, while Link moved): kept from the game
static int moved;                  // sprint_link only: the sprint has moved since it started
static double wait_frames;         // sprint_link only: 60 Hz frames stood still
static int last_button = -1;       // sprint_link only: the button setting at the last update

static int setting_button(void) {
    const char *v = cfg_value("NX2NAVI_SPRINT_BUTTON");
    for (int i = 0; i < (int)(sizeof button_names / sizeof *button_names); i++)
        if (v && !strcmp(v, button_names[i])) return i;
    return BTN_OFF;
}

static int setting_hold(void) {
    const char *v = cfg_value("NX2NAVI_SPRINT_MODE");
    return v && !strcmp(v, "hold");
}

// Guest reads through Link's pointers: a stale pointer in the chain reads 0 instead of faulting.
static uint64_t ld64(uint64_t a) { return a && mem_readable(a, 8) ? *(const uint64_t *)(uintptr_t)a : 0; }
static int32_t ld32(uint64_t a) { return a && mem_readable(a, 4) ? *(const int32_t *)(uintptr_t)a : 0; }

// 1 when x19 is the Link the player controls. Link's class has one more instance the player does not control: the
// game's actor manager ([[main+0x4c7e0d0]], class main+0x4bdfe68) names the controlled one at +0x470 (class
// main+0x4b49698), whose +0x18 must be Link's own +0x18.
static int is_link(uint64_t link) {
    if (!link || ld64(link) != MOD_MAIN_BASE + LINK_VTABLE) return 0;
    uint64_t slot = ld64(MOD_MAIN_BASE + 0x4c7e0d0), mgr = slot ? ld64(slot) : 0;
    if (!mgr || ld64(mgr) != MOD_MAIN_BASE + 0x4bdfe68) return 1;
    uint64_t ctl = ld64(mgr + 0x470);
    if (!ctl || ld64(ctl) != MOD_MAIN_BASE + 0x4b49698) return 1;
    return ld64(ctl + 0x18) == ld64(link + 0x18);
}

static int state_kind(uint64_t link) {
    uint64_t p = ld64(link + 0x18);
    if (p) p = ld64(p + 0x270);
    if (p) p = ld64(p + 0x188);
    if (p) p = ld64(p + 0x1c0);
    if (!p) return STOP;
    int32_t id = ld32(p + 0x28);
    if (id == 0x11a) id = ld32(p + 0x4c);
    for (size_t i = 0; i < sizeof states / sizeof *states; i++)
        if (states[i].id == id) return states[i].kind;
    return STOP;
}

// The set runs only while a button is chosen, so with the sprint button off its sites cost nothing. Whether it may run
// at all is read once, after the start: NX2NAVI_SPRINT_BUTTON_PATCH=0 or a mod whose code patches the same sites (mods.c
// mods_exefs_switch) leaves it off then, and switching it on later must not undo that.
static _Atomic int allowed = -1;
static int sprint_set(void) { return rt_patch_set("sprint"); }
int sprint_available(void) {
    int a = atomic_load(&allowed);
    if (a < 0) {
        a = rt_patch_enabled(sprint_set());
        atomic_store(&allowed, a);
    }
    return a;
}

void sprint_pad(uint64_t *buttons) {
    if (!sprint_available()) return;
    int b = setting_button(), set = sprint_set();
    if (rt_patch_enabled(set) != (b != BTN_OFF)) rt_patch_switch(set, b != BTN_OFF);
    if (b == BTN_OFF) {   // the sites stop calling in: nothing may stay on for when a button is chosen again
        atomic_store(&active, 0), atomic_store(&pressed, 0), atomic_store(&latched, 0);
        return;
    }
    static uint64_t last;
    uint64_t bit = button_bits[b], now = *buttons;
    int n = atomic_load(&polls), gameplay = n < GAMEPLAY_POLLS;
    if (n <= GAMEPLAY_POLLS) atomic_store(&polls, n + 1);
    // A press is decided at its start, for its whole length: kept from the game, and the sprint's, when it begins in
    // gameplay (B: while Link moves); else the game's alone, and no sprint's.
    if (now & ~last & bit) {
        int mine = gameplay && (b != BTN_B || atomic_load(&moving));
        atomic_store(&latched, mine);
        if (mine) atomic_fetch_or(&pressed, bit);
    } else if (!(now & bit)) {
        atomic_store(&latched, 0);
    }
    last = now;
    atomic_store(&held, (now & bit) != 0);
    atomic_store(&zl_held, (now & BUTTON_ZL) != 0);
    if (atomic_load(&latched) && gameplay) *buttons &= ~bit;
}

// The sprint for this update of Link, from his state and the button's presses: 1 when it is on.
int sprint_step(int kind, int button, int hold) {
    if (button != last_button) last_button = button, atomic_store(&active, 0), atomic_store(&pressed, 0);
    int press = (atomic_exchange(&pressed, 0) & button_bits[button]) != 0, on = atomic_load(&active);
    if (atomic_load(&zl_held)) kind = STOP;
    atomic_store(&moving, kind == MOVE);
    if (button == BTN_OFF) on = 0;
    else if (hold) on = atomic_load(&held) && atomic_load(&latched) && kind != STOP;
    else {
        if (press) {
            if (on) on = 0;
            else if (kind != STOP && (button != BTN_B || kind == MOVE)) on = 1, moved = kind == MOVE, wait_frames = 0;
        } else if (on) {
            if (kind == STOP) on = 0;
            else if (kind == WAIT) {
                wait_frames += frame_step_frames();
                if (wait_frames > (moved ? WAIT_STOP_FRAMES : WAIT_START_FRAMES)) on = 0;
            } else {
                wait_frames = 0;
                moved |= kind == MOVE;
            }
        }
    }
    atomic_store(&active, on);
    return on;
}

// main+0x4ae798, after ldr x10, [x10]: x10 holds Link's input flags for the sprint's start and keep tests.
static void sprint_link(Cpu *c) {
    if (!is_link(c->x[19])) return;
    atomic_store(&polls, 0);
    if (sprint_step(state_kind(c->x[19]), setting_button(), setting_hold())) c->x[10] |= DASH_INPUT;
}

// main+0xc07f3c, after ldrb w8, [x8]: the same flags' low byte for the stance and camera update.
static void sprint_stance(Cpu *c) {
    if (atomic_load(&active) && is_link(c->x[19])) c->x[8] |= DASH_INPUT;
}

#ifdef RT_PATCH_PAGE
__attribute__((constructor)) static void sprint_register(void) {
    if (!RT_V111) return;   // the set's sites are in 1.1.1's main only
    rt_register_host(RT_PATCH_PAGE + 0xf40, sprint_link);
    rt_register_host(RT_PATCH_PAGE + 0xf30, sprint_stance);
}
#endif
