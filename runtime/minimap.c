// Minimap toggle for Ocarina of Time 1.1.1: a setting that shows or hides the minimap whatever the In-Game Interface
// option does with it (Normal shows it, Pro and Master do not), and a button that switches it during gameplay (the
// patch set minimap, recomp/patches-v111.txt).
//
// The minimap is the screen MapMenu_00 (class UIMapMenuScreen, vtable main+0x4be8300) in its small form: the game
// finds it through the screen manager **[main+0x4c7e098] by the MurmurHash3 of its name (main+0xb25cd0), and the
// pause menu's map is the same screen in its large form. SeqOneShotOpenMapMenu (main+0x32d8a30) writes the form at
// +0x2350 (its IsMiniMap, 1 for the minimap) and +0x2354 (its IsDirect), then asks the screen to open: vtable +0x28
// with 3 (main+0x4b6100, which only raises the request byte +0x1a5); closing is vtable +0x30 with -4. The screen's
// state is +0x1a6 (0 closed, 1 opening, 2 open, 3 closing). Its +0x1a8 is whether it is drawn: the game's own
// SeqOneShotSetScreenVisible (main+0x17c4b0) only writes that byte.
//
// hidden: while the screen is in its small form its drawn byte is 0, so it still opens and closes as the game asks
// and nothing that waits for it sees a difference; the pause menu's map (+0x2350 = 0) is never touched.
// shown: the drawn byte is 1 while the screen is the minimap, and when the game leaves the screen closed during
// gameplay (the Pro interface never opens it) this file opens it in its small form as SeqOneShotOpenMapMenu does.
// It does so only after the screen has stayed closed through REOPEN_POLLS polls in a row of gameplay without an event,
// so a screen the game closes on its way into a menu, a cutscene or a load (which stop Link's update) is not opened
// behind it. An event (a cutscene or a conversation, during which Link's update can go on) is running while the event
// manager **[main+0x4c7e090] has its +0x68, what SeqQueryIsEventRunning answers (main+0xbcc100 with 0): then the screen
// is never opened, and one this file opened is closed (vtable +0x30 with -4), as the game closes its interface for an
// event; the Pro interface, which never opened it, would leave it.
// game: the game decides; the drawn byte this file held is handed back as 1.
//
// Settings (live): minimap_button (off, or down: the D-pad's down; the Controls tab's one row) switches between shown
// and hidden, starting from what is on the screen, and saves the choice as minimap (game, shown or hidden; no row of
// its own, game until the first press). With the button off the game decides, whatever minimap holds. The press
// counts only during gameplay: while Link's movement update runs (the set's site at the top of main+0x4aaa80 calls
// minimap_link) and no event does. Then it is kept from the game until it is released, so it does not also do what
// the game gives it.
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
#define EVENT_MGR 0x4c7e090ull         // ... and to the event manager pointer
#define SCREEN_FIND 0xb25cd0ull        // (manager, hash): the screen of that name, cast to its class, or 0
#define MAP_SCREEN_HASH 0xc3332ad4u    // MurmurHash3 of "MapMenu_00"
#define MAP_SCREEN_VTABLE 0x4be8300ull
#define LINK_VTABLE 0x4b4dfc0ull       // Link's actor class
enum { SCR_REQUEST = 0x1a5, SCR_STATE = 0x1a6, SCR_VISIBLE = 0x1a8, MAP_IS_MINI = 0x2350, MAP_IS_DIRECT = 0x2354 };
enum { VT_OPEN = 0x28, VT_CLOSE = 0x30 };
enum { ST_CLOSED, ST_OPENING, ST_OPEN, ST_CLOSING };
enum { MODE_GAME, MODE_SHOWN, MODE_HIDDEN };

#define BUTTON_DOWN (1u << 15)         // HidNpadButton Down
#define GAMEPLAY_POLLS 8               // a press counts while Link's update ran within this many polls
#define REOPEN_POLLS 20                // shown: polls of gameplay with the screen closed before it is opened

static _Atomic int polls = GAMEPLAY_POLLS;   // polls since Link's last movement update (none yet: not gameplay)
static int latched;                          // the press now held began in gameplay: kept from the game
static int last_held;
static uint64_t drawn_screen;                // the screen whose drawn byte this file holds, or 0
static int closed_polls;                     // shown: gameplay polls in a row with the screen closed
static int opened;                           // shown: times this file opened the screen (log only)
static uint64_t ours;                        // shown: the screen this file opened, until it is seen closed

static uint64_t rd64(uint64_t a) { return a ? *(const uint64_t *)(uintptr_t)a : 0; }
// An event (a cutscene, a conversation) is running: SeqQueryIsEventRunning's answer
static int event_running(void) {
    uint64_t m = rd64(rd64(MOD_MAIN_BASE + EVENT_MGR));
    return m && rd64(m + 0x68);
}
static uint8_t rd8(uint64_t a) { return *(const volatile uint8_t *)(uintptr_t)a; }
static void wr8(uint64_t a, uint8_t v) { *(volatile uint8_t *)(uintptr_t)a = v; }

static int setting_button(void) {
    const char *v = cfg_value("NX2NAVI_MINIMAP_BUTTON");
    return v && !strcmp(v, "down");
}

static int setting_mode(void) {
    const char *v = cfg_value("NX2NAVI_MINIMAP");
    return !v ? MODE_GAME : !strcmp(v, "shown") ? MODE_SHOWN : !strcmp(v, "hidden") ? MODE_HIDDEN : MODE_GAME;
}

// Whether the set may run, read once after the start (NX2NAVI_MINIMAP_BUTTON_PATCH=0, or a mod whose code patches
// the same site, leaves it off; switching it on later must not undo that).
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

// Open or opening without a close asked for, or a request to open pending.
static int screen_up(uint64_t s) {
    int st = rd8(s + SCR_STATE), req = (int8_t)rd8(s + SCR_REQUEST);
    return req > 0 || ((st == ST_OPENING || st == ST_OPEN) && req >= 0);
}

// The minimap is on the screen: up, in its small form and drawn.
static int minimap_seen(uint64_t s) { return s && screen_up(s) && rd8(s + MAP_IS_MINI) && rd8(s + SCR_VISIBLE); }

// The drawn byte while this file holds it. Handed back (the pause menu's map, or the game deciding again) it is 1, the
// screen drawn: not the value found when this file first wrote it, which can be a 0 of a screen the game had not yet
// shown or that the Pro interface left closed, and would leave the pause menu's map undrawn.
static void set_drawn(uint64_t s, uint8_t v) {
    drawn_screen = s;
    if (rd8(s + SCR_VISIBLE) != v) wr8(s + SCR_VISIBLE, v);
}
static void restore_drawn(uint64_t s) {
    if (drawn_screen && drawn_screen == s && !rd8(s + SCR_VISIBLE)) wr8(s + SCR_VISIBLE, 1);
    drawn_screen = 0;
}

void minimap_pad(Cpu *c, uint64_t *buttons) {
    if (!RT_V111) return;
    int avail = minimap_available(), button = avail && setting_button();
    if (avail && rt_patch_enabled(minimap_set()) != button) rt_patch_switch(minimap_set(), button);

    int n = atomic_load(&polls), event = event_running(), gameplay = n < GAMEPLAY_POLLS && !event;
    if (n <= GAMEPLAY_POLLS) atomic_store(&polls, n + 1);
    int mode = button ? setting_mode() : MODE_GAME;   // the button off turns the whole of it off
    uint64_t s = 0;
    int looked = 0;

    int held = button && buttons && (*buttons & BUTTON_DOWN);
    if (held && !last_held && gameplay) {
        latched = 1;
        if (mode == MODE_GAME) s = map_screen(c), looked = 1;
        mode = mode == MODE_SHOWN || (mode == MODE_GAME && minimap_seen(s)) ? MODE_HIDDEN : MODE_SHOWN;
        cfg_set("NX2NAVI_MINIMAP", mode == MODE_SHOWN ? "shown" : "hidden");
        cfg_save();
        closed_polls = REOPEN_POLLS;   // a press shows it at once when the screen is closed
    }
    if (!held) latched = 0;
    last_held = held;
    if (latched && buttons) *buttons &= ~(uint64_t)BUTTON_DOWN;

    // Nothing is looked up while the game decides and nothing of this file is left on the screen.
    if (mode == MODE_GAME && !drawn_screen) return;
    if (!looked) s = map_screen(c);
    if (drawn_screen && s && drawn_screen != s) drawn_screen = 0;   // another screen: nothing of ours is on it

    if (!s) return;
    int mini = rd8(s + MAP_IS_MINI) != 0;
    if (mode == MODE_GAME || !mini) {   // the game's, or the pause menu's map: as the game left it
        restore_drawn(s);
        closed_polls = 0;
        return;
    }
    if (mode == MODE_HIDDEN) {
        set_drawn(s, 0);
        closed_polls = 0;
        return;
    }
    // shown
    set_drawn(s, 1);
    if (screen_up(s)) {
        closed_polls = 0;
        if (event && ours == s) {   // an event the Pro interface would leave it on for
            rt_call(c, rd64(rd64(s) + VT_CLOSE), 2, s, (uint64_t)(int64_t)-4);
            ours = 0;
        }
        return;
    }
    ours = 0;
    if (!gameplay) {
        closed_polls = 0;
        return;
    }
    // Link's update runs once between two pad reads during gameplay, so a read after none (the first of a menu, or a
    // second read in one frame) does not count, and neither breaks the run of polls yet.
    if (n > 0) return;
    if (++closed_polls < REOPEN_POLLS) return;
    closed_polls = 0;
    wr8(s + MAP_IS_MINI, 1);
    wr8(s + MAP_IS_DIRECT, 0);
    rt_call(c, rd64(rd64(s) + VT_OPEN), 2, s, (uint64_t)3);
    ours = s;
    if (opened++ < 20) rt_log("minimap: opened the minimap the game left closed (state %d)", rd8(s + SCR_STATE));
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
