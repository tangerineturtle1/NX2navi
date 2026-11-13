// The minimap toggle (runtime/minimap.c) against a stand-in screen: the button's presses during gameplay and outside it,
// what the game gets from each poll, and the screen's drawn byte in its small and large forms. Built by
// tests/minimap_check.py, which writes the stand-in layout.h and patches.h.
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

uint64_t test_main_base;   // layout.h's MOD_MAIN_BASE: a buffer standing in for main's image
const int rt_game_version = 111;

#include "minimap.c"

static char minimap_v[16] = "shown", button_v[16] = "down";
static int saves;
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
static int set_on = 1;
int rt_patch_set(const char *name) { return strcmp(name, "minimap") ? -1 : 5; }
int rt_patch_enabled(int set) { return set == 5 && set_on; }
void rt_patch_switch(int set, int on) { assert(set == 5); set_on = on != 0; }
static GuestFn link_fn;
void rt_register_host(uint64_t addr, GuestFn fn) {
    assert(addr == RT_PATCH_PAGE + 0xef0);
    link_fn = fn;
}

static uint8_t screen[0x2400] __attribute__((aligned(8)));
static uint64_t mgr[16], link_actor[4], finds;
static int ui_ready = 1;
uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...) {
    va_list ap;
    va_start(ap, n);
    uint64_t m = va_arg(ap, uint64_t), h = va_arg(ap, uint64_t);
    va_end(ap);
    assert(fn == test_main_base + SCREEN_FIND && n == 2 && m == (uintptr_t)mgr && h == MAP_SCREEN_HASH);
    finds++;
    return ui_ready ? (uintptr_t)screen : 0;
}

// One poll: Link's update ran just before (gameplay) or not, with these buttons. Returns what the game got.
static uint64_t poll(uint64_t buttons, int gameplay) {
    Cpu c;
    memset(&c, 0, sizeof c);
    if (gameplay && set_on) c.x[0] = (uintptr_t)link_actor, link_fn(&c);
    minimap_pad(&c, &buttons);
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
    uint64_t vt = test_main_base + MAP_SCREEN_VTABLE;
    memcpy(screen, &vt, 8);
    link_actor[0] = test_main_base + LINK_VTABLE;
    screen[SCR_VISIBLE] = 1, screen[MAP_IS_MINI] = 1;

    // shown and never hidden: no lookup at all
    for (int i = 0; i < 10; i++) assert(poll(A_BTN, 1) == A_BTN);
    assert(finds == 0 && drawn());

    // a press during gameplay hides the minimap, saves, and the game never gets it while held
    assert(poll(DOWN, 1) == 0 && !strcmp(minimap_v, "hidden") && saves == 1 && !drawn());
    assert(poll(DOWN | A_BTN, 1) == A_BTN && saves == 1);   // held: one press, still kept from the game
    assert(poll(0, 1) == 0 && !drawn());

    // the game showing its screen again (its own visible node) is undone while hidden
    screen[SCR_VISIBLE] = 1;
    poll(0, 1);
    assert(!drawn());

    // the pause menu's map (large form) is drawn, and hidden again once it is the minimap
    screen[MAP_IS_MINI] = 0;
    poll(0, 0);
    assert(drawn());
    screen[MAP_IS_MINI] = 1;
    poll(0, 0);
    assert(!drawn());

    // a press outside gameplay (a menu: Link's update stopped) reaches the game and changes nothing
    for (int i = 0; i < GAMEPLAY_POLLS; i++) poll(0, 0);
    assert(poll(DOWN, 0) == DOWN && !strcmp(minimap_v, "hidden") && saves == 1);
    assert(poll(DOWN, 1) == DOWN);   // still the same press, begun in the menu: the game's
    poll(0, 1);

    // the next press during gameplay shows it again
    assert(poll(DOWN, 1) == 0 && !strcmp(minimap_v, "shown") && saves == 2 && drawn());
    poll(0, 1);
    finds = 0;
    poll(0, 1);
    assert(finds == 0);   // shown again: nothing looked up

    // hidden from the menu's row: applied at the next poll, also before the interface exists
    snprintf(minimap_v, sizeof minimap_v, "hidden");
    ui_ready = 0;
    poll(0, 0);
    ui_ready = 1;
    poll(0, 0);
    assert(!drawn());

    // button off: the set stops, presses reach the game, the hidden setting still holds
    snprintf(button_v, sizeof button_v, "off");
    assert(poll(DOWN, 1) == DOWN && !set_on && !drawn() && !strcmp(minimap_v, "hidden"));
    snprintf(button_v, sizeof button_v, "down");
    poll(0, 1);
    assert(set_on);

    puts("minimap_check: ok");
    free(img);
    return 0;
}
