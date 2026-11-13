// Minimap toggle for Ocarina of Time 1.1.1: a setting that hides the minimap (and nothing else of the interface), and
// a button that switches it during gameplay (the patch set minimap, recomp/patches-v111.txt).
//
// The minimap is the screen MapMenu_00 (class UIMapMenuScreen, vtable main+0x4be8300) in its small form: the game
// finds it through the screen manager **[main+0x4c7e098] by the MurmurHash3 of its name (main+0xb25cd0), and the
// pause menu's map is the same screen in its large form. SeqOneShotOpenMapMenu (main+0x32d8a30) writes which form it
// opens at +0x2350 (its IsMiniMap, 1 for the minimap) before it asks the screen to open. A screen's +0x1a8 is whether it
// is drawn: the game's own SeqOneShotSetScreenVisible (main+0x17c4b0) only writes that byte, and leaves the screen's
// state (+0x1a6: closed, opening, open, closing) and requests (+0x1a5) alone. So hiding the minimap writes 0 there
// while the screen is in its small form, and the screen keeps opening and closing as the game asks: nothing that
// waits for the screen to open, or reads its state, sees a difference. The pause menu's map (+0x2350 = 0) is never
// touched, and the byte goes back to 1 when the minimap is shown again or the screen is the map, only where this file
// wrote the 0.
//
// Settings (live): minimap (shown or hidden); minimap_button (off, or down: the D-pad's down) switches minimap and
// saves it. The press counts only during gameplay, while Link's movement update runs (the set's site at the top of
// main+0x4aaa80 calls minimap_link): a menu, a cutscene or a load stops that update, so the button keeps its own role
// there. During gameplay the press is kept from the game until it is released, so it does not also do what the game
// gives it.
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "layout.h"
#include "minimap.h"

#if __has_include("patches.h")
#include "patches.h"
#endif

#define SCREEN_MGR 0x4c7e098ull        // main offset: the pointer to eui's screen manager pointer
#define SCREEN_FIND 0xb25cd0ull        // (manager, hash): the screen of that name, cast to its class, or 0
#define MAP_SCREEN_HASH 0xc3332ad4u    // MurmurHash3 of "MapMenu_00"
#define MAP_SCREEN_VTABLE 0x4be8300ull
#define LINK_VTABLE 0x4b4dfc0ull       // Link's actor class
enum { SCR_VISIBLE = 0x1a8, MAP_IS_MINI = 0x2350 };

#define BUTTON_DOWN (1u << 15)         // HidNpadButton Down
#define GAMEPLAY_POLLS 8               // a press counts while Link's update ran within this many polls

static _Atomic int polls = GAMEPLAY_POLLS;   // polls since Link's last movement update (none yet: not gameplay)
static int latched;                          // the press now held began in gameplay: kept from the game
static int last_held;
static uint64_t hid_screen;                  // the screen whose +0x1a8 this file set to 0, or 0

static uint64_t rd64(uint64_t a) { return a ? *(const uint64_t *)(uintptr_t)a : 0; }

static int setting_button(void) {
    const char *v = cfg_value("NX2NAVI_MINIMAP_BUTTON");
    return v && !strcmp(v, "down");
}

static int setting_hidden(void) {
    const char *v = cfg_value("NX2NAVI_MINIMAP");
    return v && !strcmp(v, "hidden");
}

// Whether the set may run, read once after the start (NX2NAVI_MINIMAP_PATCH=0, or a mod whose code patches the same
// site, leaves it off; switching it on later must not undo that).
static _Atomic int allowed = -1;
static int minimap_set(void) { return rt_patch_set("minimap"); }
int minimap_available(void) {
    if (!RT_V111) return 0;
    int a = atomic_load(&allowed);
    if (a < 0) {
        a = rt_patch_enabled(minimap_set());
        atomic_store(&allowed, a);
    }
    return a;
}

// The minimap's screen, through the game's own lookup; 0 before the interface exists.
static uint64_t map_screen(Cpu *c) {
    uint64_t mgr = rd64(rd64(MOD_MAIN_BASE + SCREEN_MGR));
    if (!mgr || !rd64(mgr + 0x48)) return 0;
    uint64_t s = rt_call(c, MOD_MAIN_BASE + SCREEN_FIND, 2, mgr, (uint64_t)MAP_SCREEN_HASH);
    return s && rd64(s) == MOD_MAIN_BASE + MAP_SCREEN_VTABLE ? s : 0;
}

void minimap_pad(Cpu *c, uint64_t *buttons) {
    if (!RT_V111) return;
    int avail = minimap_available(), button = avail && setting_button();
    if (avail && rt_patch_enabled(minimap_set()) != button) rt_patch_switch(minimap_set(), button);

    int n = atomic_load(&polls), gameplay = n < GAMEPLAY_POLLS;
    if (n <= GAMEPLAY_POLLS) atomic_store(&polls, n + 1);
    int held = button && buttons && (*buttons & BUTTON_DOWN);
    if (held && !last_held && gameplay) {
        latched = 1;
        cfg_set("NX2NAVI_MINIMAP", setting_hidden() ? "shown" : "hidden");
        cfg_save();
    }
    if (!held) latched = 0;
    last_held = held;
    if (latched && buttons) *buttons &= ~(uint64_t)BUTTON_DOWN;

    // The drawn byte: 0 while hidden and the screen is the minimap, back to 1 where this file wrote the 0. Nothing is
    // looked up while the minimap is shown and was never hidden here.
    int hidden = setting_hidden();
    if (!hidden && !hid_screen) return;
    uint64_t s = map_screen(c);
    volatile uint8_t *vis = s ? (volatile uint8_t *)(uintptr_t)(s + SCR_VISIBLE) : 0;
    if (hid_screen && hid_screen != s) hid_screen = 0;   // the screen is gone or another: nothing of ours is left
    if (!s) return;
    int mini = *(const volatile uint8_t *)(uintptr_t)(s + MAP_IS_MINI) != 0;
    if (hidden && mini) {
        if (*vis) *vis = 0;
        hid_screen = s;
    } else if (hid_screen) {
        *vis = 1;
        hid_screen = 0;
    }
}

// main+0x4aaabc, after ldr x21, [x0, #0x18] at the top of Link's movement update: x0 is the actor.
static void minimap_link(Cpu *c) {
    if (rd64(c->x[0]) == MOD_MAIN_BASE + LINK_VTABLE) atomic_store(&polls, 0);
}

#ifdef RT_PATCH_PAGE
__attribute__((constructor)) static void minimap_register(void) {
    if (!RT_V111) return;   // the set's site is in 1.1.1's main only
    rt_register_host(RT_PATCH_PAGE + 0xef0, minimap_link);
}
#endif
