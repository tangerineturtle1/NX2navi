// Text speed for Ocarina of Time 1.1.1 (the patch set text_speed, recomp/patches-v111.txt). The message window shows
// its text through a typewriter whose speed and waits the message's own tags set, and a press of the decide button
// while it types shows the page at once, unless the window marked the message unskippable (a message that turns its
// page by itself on a timer, and some window types). The set's two sites read the patch page words this file writes:
//
//   +0x200  the typewriter of the message window while its message may be skipped, 0 otherwise (written by the game's
//           code at the first site, read at the second)
//   +0x208  the fast factor less one, as a float: 0 leaves the speed as it is
//   +0x20c  1 for instant: the window's skip runs on the first frame of each page, as if the button were pressed
//
// Setting text_speed (live): normal (the game's, and the set off), fast (the skippable messages type
// TEXT_SPEED_FAST_FACTOR times as fast, their scripted waits as long as before) or instant (the skippable messages
// show each page at once, as the game's skip does). Unskippable messages keep the game's speed under both.
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "text_speed.h"

enum { SPEED_NORMAL, SPEED_FAST, SPEED_INSTANT };
static const char *const speed_names[] = {"normal", "fast", "instant"};

#define PAGE_FACTOR 0x208
#define PAGE_INSTANT 0x20c

static int setting_speed(void) {
    const char *v = cfg_value("NX2NAVI_TEXT_SPEED");
    for (int i = 0; i < (int)(sizeof speed_names / sizeof *speed_names); i++)
        if (v && !strcmp(v, speed_names[i])) return i;
    return SPEED_NORMAL;
}

// Whether the set may run at all is read once, after the start: NX2NAVI_TEXT_SPEED_PATCH=0, or a mod whose code
// patches the same sites (mods.c mods_exefs_switch), leaves it off then, and switching it on later must not undo that.
static _Atomic int allowed = -1;
static int speed_set(void) { return rt_patch_set("text_speed"); }
int text_speed_available(void) {
    int a = atomic_load(&allowed);
    if (a < 0) {
        a = rt_patch_enabled(speed_set()) && rt_patch_page() != NULL;
        atomic_store(&allowed, a);
    }
    return a;
}

void text_speed_poll(void) {
    if (!text_speed_available()) return;
    int s = setting_speed(), set = speed_set();
    uint8_t *page = rt_patch_page();
    float factor = s == SPEED_FAST ? TEXT_SPEED_FAST_FACTOR - 1.0f : 0.0f;
    uint32_t bits;
    memcpy(&bits, &factor, sizeof bits);
    __atomic_store_n((uint32_t *)(page + PAGE_FACTOR), bits, __ATOMIC_RELAXED);
    __atomic_store_n(page + PAGE_INSTANT, (uint8_t)(s == SPEED_INSTANT), __ATOMIC_RELAXED);
    static int logged = -1;
    if (s != logged) {
        logged = s;
        rt_log("text speed: %s", speed_names[s]);
    }
    if (rt_patch_enabled(set) != (s != SPEED_NORMAL)) rt_patch_switch(set, s != SPEED_NORMAL);
}
