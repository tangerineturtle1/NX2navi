// The minimap toggle (runtime/minimap.c) against a stand-in screen: the button's presses during gameplay and outside
// it, what the game gets from each poll, the screen's drawn byte in its small and large forms, and the screen opened
// when the game leaves it closed (the Pro interface). Built by tests/minimap_check.py, which writes the stand-in
// layout.h and patches.h.
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

uint64_t test_main_base;   // layout.h's MOD_MAIN_BASE: a buffer standing in for main's image
const int rt_game_version = 111;

#include "minimap.c"

static char minimap_v[16] = "game", button_v[16] = "down";
static int saves, opens, closes;
const char *cfg_value(const char *key) {
    if (!strcmp(key, "NX2NAVI_MINIMAP")) return minimap_v;
    if (!strcmp(key, "NX2NAVI_MINIMAP_BUTTON")) return button_v;
    return "";
}
void cfg_set(const char *key, const char *v) {
    assert(!strcmp(key, "NX2NAVI_MINIMAP"));
    snprintf(minimap_v, sizeof minimap_v, "%s", v);
}
int cfg_save(void) { return ++saves; }
void rt_log(const char *fmt, ...) { (void)fmt; }
static int set_on = 1;
int rt_patch_set(const char *name) { return strcmp(name, "minimap") ? -1 : 5; }
int rt_patch_enabled(int set) { return set == 5 && set_on; }
void rt_patch_switch(int set, int on) { assert(set == 5); set_on = on != 0; }
static GuestFn link_fn;
void rt_register_host(uint64_t addr, GuestFn fn) {
    assert(addr == RT_PATCH_PAGE + 0xef0);
    link_fn = fn;
}

#define OPEN_FN 0x7777000ull
#define CLOSE_FN 0x7778000ull
static uint8_t screen[0x2400] __attribute__((aligned(8)));
static uint64_t mgr[16], link_actor[4], finds, event_mgr[16];
static void set_event(int on) { event_mgr[0x68 / 8] = on ? 1 : 0; }
static int ui_ready = 1;
uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...) {
    va_list ap;
    va_start(ap, n);
    uint64_t a = va_arg(ap, uint64_t), b = va_arg(ap, uint64_t);
    va_end(ap);
    if (fn == OPEN_FN) {   // the screen's open: raises the request; the stand-in screen is open at the next poll
        assert(a == (uintptr_t)screen && b == 3 && screen[MAP_IS_MINI] == 1);
        opens++, screen[SCR_REQUEST] = 3;
        return 0;
    }
    if (fn == CLOSE_FN) {   // the screen's close: the stand-in screen is closed at the next poll
        assert(a == (uintptr_t)screen && (int64_t)b == -4);
        closes++, screen[SCR_REQUEST] = (uint8_t)(int8_t)-4;
        return 0;
    }
    assert(fn == test_main_base + SCREEN_FIND && n == 2 && a == (uintptr_t)mgr && b == MAP_SCREEN_HASH);
    finds++;
    return ui_ready ? (uintptr_t)screen : 0;
}

static void set_state(int st, int req) { screen[SCR_STATE] = (uint8_t)st, screen[SCR_REQUEST] = (uint8_t)(int8_t)req; }
// One poll: Link's update ran just before (gameplay) or not, with these buttons. Returns what the game got.
static uint64_t poll(uint64_t buttons, int gameplay) {
    Cpu c;
    memset(&c, 0, sizeof c);
    if (gameplay && set_on) c.x[0] = (uintptr_t)link_actor, link_fn(&c);
    minimap_pad(&c, &buttons);
    if (screen[SCR_REQUEST] == 3) set_state(ST_OPEN, 0);   // the manager takes the request
    if ((int8_t)screen[SCR_REQUEST] == -4) set_state(ST_CLOSED, 0);
    return buttons;
}
static int drawn(void) { return screen[SCR_VISIBLE]; }
#define DOWN BUTTON_DOWN
#define A_BTN 1u

int main(void) {
    char *img = calloc(1, 0x4d00000);
    assert(img && link_fn);
    test_main_base = (uintptr_t)img;
    uint64_t mgr_ptr = (uintptr_t)mgr;
    memcpy(img + SCREEN_MGR, &(uint64_t){(uintptr_t)&mgr_ptr}, 8);
    mgr[0x48 / 8] = 1;
    uint64_t vt = test_main_base + MAP_SCREEN_VTABLE, open_fn = OPEN_FN;
    memcpy(screen, &vt, 8);
    memcpy(img + MAP_SCREEN_VTABLE + VT_OPEN, &open_fn, 8);
    uint64_t close_fn = CLOSE_FN, event_ptr = (uintptr_t)event_mgr;
    memcpy(img + MAP_SCREEN_VTABLE + VT_CLOSE, &close_fn, 8);
    memcpy(img + EVENT_MGR, &(uint64_t){(uintptr_t)&event_ptr}, 8);
    link_actor[0] = test_main_base + LINK_VTABLE;

    // Normal: the game shows the minimap; with minimap = game nothing is looked up
    screen[SCR_VISIBLE] = 1, screen[MAP_IS_MINI] = 1;
    set_state(ST_OPEN, 0);
    for (int i = 0; i < 10; i++) assert(poll(A_BTN, 1) == A_BTN);
    assert(finds == 0 && drawn());

    // a press during gameplay starts from what is on the screen: shown, so it hides; saved; kept from the game
    assert(poll(DOWN, 1) == 0 && !strcmp(minimap_v, "hidden") && saves == 1 && !drawn());
    assert(poll(DOWN | A_BTN, 1) == A_BTN && saves == 1);   // held: one press
    poll(0, 1);
    screen[SCR_VISIBLE] = 1;   // the game's own visible node: undone while hidden
    poll(0, 1);
    assert(!drawn());

    // the pause menu's map (large form) is as the game left it, and hidden again once it is the minimap
    screen[MAP_IS_MINI] = 0;
    poll(0, 0);
    assert(drawn());
    screen[MAP_IS_MINI] = 1;
    poll(0, 0);
    assert(!drawn());

    // a press outside gameplay reaches the game and changes nothing
    for (int i = 0; i < GAMEPLAY_POLLS; i++) poll(0, 0);
    assert(poll(DOWN, 0) == DOWN && !strcmp(minimap_v, "hidden") && saves == 1);
    assert(poll(DOWN, 1) == DOWN);   // the same press, begun in the menu: the game's
    poll(0, 1);

    // back to game from the menu's row: the drawn byte as the game had it
    snprintf(minimap_v, sizeof minimap_v, "game");
    poll(0, 1);
    assert(drawn());

    // Pro: the game leaves the screen closed. A press: nothing on the screen, so it shows, and opens it at once
    set_state(ST_CLOSED, 0);
    opens = 0;
    for (int i = 0; i < 30; i++) poll(0, 1);
    assert(opens == 0);   // game: never opened here
    assert(poll(DOWN, 1) == 0 && !strcmp(minimap_v, "shown") && opens == 1 && drawn());
    poll(0, 1);
    assert(screen[SCR_STATE] == ST_OPEN && opens == 1);

    // the game closes it on its way into a menu: Link's update stops, and it is not opened behind the menu
    set_state(ST_CLOSING, -4);
    for (int i = 0; i < 200; i++) poll(0, i < 2);
    set_state(ST_CLOSED, 0);
    for (int i = 0; i < 200; i++) poll(0, 0);
    assert(opens == 1);
    // ... and back in gameplay it is opened once the screen has stayed closed for REOPEN_POLLS polls
    for (int i = 0; i < REOPEN_POLLS - 1; i++) poll(0, 1);
    assert(opens == 1);
    poll(0, 1);
    assert(opens == 2 && screen[SCR_STATE] == ST_OPEN);
    // a gap in gameplay (a short cutscene) starts the count again
    set_state(ST_CLOSED, 0);
    for (int i = 0; i < REOPEN_POLLS - 1; i++) poll(0, 1);
    for (int i = 0; i < GAMEPLAY_POLLS + 1; i++) poll(0, 0);
    for (int i = 0; i < REOPEN_POLLS - 1; i++) poll(0, 1);
    assert(opens == 2);
    poll(0, 1);
    assert(opens == 3);

    // the pause menu's map open in its large form is never opened over or hidden
    screen[MAP_IS_MINI] = 0, screen[SCR_VISIBLE] = 0;
    set_state(ST_CLOSED, 0);
    for (int i = 0; i < 100; i++) poll(0, 1);
    assert(opens == 3 && drawn());   // the large form is drawn, never hidden
    screen[MAP_IS_MINI] = 1;

    // the next press hides it (it is on the screen)
    for (int i = 0; i < REOPEN_POLLS; i++) poll(0, 1);
    assert(drawn() && screen[SCR_STATE] == ST_OPEN);
    assert(poll(DOWN, 1) == 0 && !strcmp(minimap_v, "hidden") && !drawn());
    poll(0, 1);

    // set in nx2navi.ini before the interface exists: applied once it does
    snprintf(minimap_v, sizeof minimap_v, "shown");
    ui_ready = 0;
    poll(0, 1);
    ui_ready = 1;
    poll(0, 1);
    assert(drawn());

    // button off: the whole of it is off. The set stops, presses reach the game, the minimap is the game's again
    // (drawn as the game had it) and nothing is opened, while the saved choice stays for when the button is back
    snprintf(minimap_v, sizeof minimap_v, "hidden");
    poll(0, 1);
    assert(!drawn());
    snprintf(button_v, sizeof button_v, "off");
    assert(poll(DOWN, 1) == DOWN && !set_on && drawn() && !strcmp(minimap_v, "hidden"));
    set_state(ST_CLOSED, 0);
    opens = finds = 0;
    for (int i = 0; i < 100; i++) poll(0, 1);
    assert(opens == 0 && finds == 0);
    snprintf(button_v, sizeof button_v, "down");
    poll(0, 1);
    assert(set_on && !drawn());

    // the pause menu's map after the minimap was hidden on Pro: the screen was undrawn and closed when the first press
    // showed it, and that 0 is not what the large form gets back
    snprintf(minimap_v, sizeof minimap_v, "game");
    poll(0, 1);
    screen[MAP_IS_MINI] = 1, screen[SCR_VISIBLE] = 0;
    set_state(ST_CLOSED, 0);
    poll(0, 1);
    assert(poll(DOWN, 1) == 0 && !strcmp(minimap_v, "shown") && drawn());
    poll(0, 1);
    assert(poll(DOWN, 1) == 0 && !strcmp(minimap_v, "hidden") && !drawn());
    poll(0, 1);
    for (int i = 0; i < GAMEPLAY_POLLS + 1; i++) poll(0, 0);   // the pause menu
    screen[MAP_IS_MINI] = 0;                                    // its map: the same screen, large
    poll(0, 0);
    assert(drawn());
    screen[MAP_IS_MINI] = 1;                                    // back to the minimap: hidden again
    poll(0, 1);
    assert(!drawn());

    // hidden from the start (saved), with the screen already undrawn: the large form is drawn all the same
    screen[SCR_VISIBLE] = 0;
    drawn_screen = 0;
    poll(0, 1);
    screen[MAP_IS_MINI] = 0;
    poll(0, 0);
    assert(drawn());

    // an event (a conversation: Link's update goes on) with the minimap this file opened on Pro: closed at once, never
    // opened during it, a press is the game's; after it the minimap comes back once REOPEN_POLLS polls have passed
    snprintf(minimap_v, sizeof minimap_v, "shown");
    screen[MAP_IS_MINI] = 1;
    set_state(ST_CLOSED, 0);
    opens = closes = 0;
    for (int i = 0; i < REOPEN_POLLS; i++) poll(0, 1);
    assert(opens == 1 && screen[SCR_STATE] == ST_OPEN);
    set_event(1);
    poll(0, 1);
    assert(closes == 1 && screen[SCR_STATE] == ST_CLOSED);
    for (int i = 0; i < 300; i++) poll(0, 1);
    assert(opens == 1 && closes == 1);
    assert(poll(DOWN, 1) == DOWN && !strcmp(minimap_v, "shown"));   // the conversation's: no switch
    poll(0, 1);
    set_event(0);
    for (int i = 0; i < REOPEN_POLLS - 1; i++) poll(0, 1);
    assert(opens == 1);
    poll(0, 1);
    assert(opens == 2 && screen[SCR_STATE] == ST_OPEN);
    // the game's own minimap (Normal) in an event is left to the game: it closes it itself
    set_state(ST_CLOSED, 0);
    poll(0, 1);   // seen closed: no longer this file's
    set_state(ST_OPEN, 0);
    poll(0, 1);   // opened by the game (Normal) before this file would
    assert(opens == 2);
    set_event(1);
    for (int i = 0; i < 50; i++) poll(0, 1);
    set_event(0);
    assert(closes == 1);

    puts("minimap_check: ok");
    free(img);
    return 0;
}
