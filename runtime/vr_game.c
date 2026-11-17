// VR's hooks in the game's camera update (Ocarina of Time 1.1.1), for vk_xr.cpp's head view (NX2NAVI_VR_VIEW=head).
//
// The camera update (main+0x4c48c0) chooses its position, target and up, then builds the look-at camera from them and,
// further on, the perspective projection from the camera's vertical angle. The free camera's site (debug_tools.c
// dbg_freecam_host, set freecam) sees the first in registers: for the gameplay camera the head's pose is put on them
// there (vr_camera_update). The set vrlens's site is the angle's load (vr_lens_host): the headset's lens replaces it
// for the same camera. Rendering and the game's culling both follow, as they read what the update made.
// While an event plays (a cutscene, a conversation) the events' camera follows the head instead, from where the event
// puts it. In first person, Link faces where the head looks (turn_link) and strafes when moved sideways or back (vr_pad).
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "cfg.h"
#include "debug_tools.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"
#include "input.h"
#include "vk_xr.h"
#include "vr_wheel.h"
#include "vr_game.h"

static uint64_t head_camera;   // x19 of the update the head's view was put on (0: none); the game's thread alone

static double reg_d(const Cpu *c, int r) {
    uint64_t b = (uint64_t)c->v[r];
    double v;
    memcpy(&v, &b, 8);
    return v;
}
static void set_d(Cpu *c, int r, double v) {
    uint64_t b;
    memcpy(&b, &v, 8);
    c->v[r] = b;
}
static float reg_s(const Cpu *c, int r) {
    uint32_t b = (uint32_t)c->v[r];
    float v;
    memcpy(&v, &b, 4);
    return v;
}
static void set_s(Cpu *c, int r, float v) {
    uint32_t b;
    memcpy(&b, &v, 4);
    c->v[r] = b;
}

// An event plays (a cutscene, a conversation): the event manager's singleton (main+0x4c7e090) holds the manager,
// whose +104 is the event running, 0 when none. The event camera updates every frame whether one plays or not.
static int event_playing(void) {
    uint64_t singleton = 0, events = 0, running = 0;
    memcpy(&singleton, (const void *)(uintptr_t)(MOD_MAIN_BASE + 0x4c7e090), 8);
    if (singleton) memcpy(&events, (const void *)(uintptr_t)singleton, 8);
    if (events) memcpy(&running, (const void *)(uintptr_t)(events + 104), 8);
    return running != 0;
}

// n bytes at a are the game's read-write memory (a pointer read from the game, before it is followed)
static int game_rw(uint64_t a, size_t n) {
    RtMemoryInfo mi;
    return a && !mem_query(a, &mi) && mi.state && (mi.perm & 3) == 3 && a + n <= mi.base + mi.size;
}

// First person's hiding of Link: the game fades him out when its camera comes close, through two of his components'
// alphas (the components at actor +0x270; at their +0x260 an object of vtable main+0x4b4a5a0 with its alpha at +0x118,
// at +0x278 one of vtable main+0x4b4b4a8 with its alpha at +0x130). Below zero they stay hidden through the game's
// easing back toward 1, every frame; what he holds is drawn by other actors and stays. Written while first person is on.
static void hide_link(uint64_t actor) {
    static const struct { uint32_t slot; uint64_t vtable; uint32_t field; } fades[] = {
        {0x260, MOD_MAIN_BASE + 0x4b4a5a0, 0x118}, {0x278, MOD_MAIN_BASE + 0x4b4b4a8, 0x130}};
    static const float hidden = -1.0f;
    uint64_t components = 0;
    memcpy(&components, (const void *)(uintptr_t)(actor + 0x270), 8);
    if (!game_rw(components, 0x280)) return;
    for (unsigned i = 0; i < 2; i++) {
        uint64_t object = 0, vtable = 0;
        memcpy(&object, (const void *)(uintptr_t)(components + fades[i].slot), 8);
        if (!game_rw(object, fades[i].field + 4)) continue;
        memcpy(&vtable, (const void *)(uintptr_t)object, 8);
        if (vtable == fades[i].vtable) dbg_poke(object + fades[i].field, &hidden, sizeof hidden);
    }
}

// First person (NX2NAVI_VR_CAMERA=first): the camera at Link's eyes, from his feet, his facing and his age; the
// offsets NX2NAVI_VR_EYE_UP, _SIDE and _FORWARD (centimetres, -30..30) move it from the place found right in a Quest 3
// (child 1.27 m, adult 1.80 m over his feet, 8 cm ahead of them). 0 while there is no Link.
static int first_person_eye(double eye[3]) {
    if (strcmp(cfg_value("NX2NAVI_VR_CAMERA"), "first")) return 0;
    const uint64_t actor = dbg_player_actor();
    float feet[3], m[9];
    if (!actor || !dbg_player_pos(feet) || !dbg_player_rot(m)) return 0;
    double fx = m[2], fz = m[8], n = sqrt(fx * fx + fz * fz);   // his facing: the rotation's third column, level
    if (!(n > 1e-4)) return 0;
    fx /= n, fz /= n;
    int child = 1;
    dbg_flag_get("Sequence_IsChildhood", &child);
    const double up = (child ? 1.27 : 1.80) + cfg_int("NX2NAVI_VR_EYE_UP") / 100.0;
    const double ahead = 0.08 + cfg_int("NX2NAVI_VR_EYE_FORWARD") / 100.0, side = cfg_int("NX2NAVI_VR_EYE_SIDE") / 100.0;
    eye[0] = feet[0] + ahead * fx - side * fz;   // his right: (-fz, 0, fx)
    eye[1] = feet[1] + up;
    eye[2] = feet[2] + ahead * fz + side * fx;
    hide_link(actor);
    return 1;
}

// First person's turn of Link toward the view while he is upright (not climbing, swimming or hanging), so what he holds
// and what reacts to his facing follow the head. His facing as the game keeps it, from which his rotation, his physics
// and the rest are made each frame: the object component 0 points at (+0x60), its 3x4 matrix by rows at +0x1f8 (the
// position in each row's fourth float). Its rotation is made the turn about the vertical toward the view. Running
// forward, the game turns him toward his movement, which the head's view steers; sideways or back he strafes (vr_pad).
static _Atomic uint64_t fp_at;   // when Link was last turned to the head in first person
static _Atomic uint64_t play_at;   // when the gameplay camera last updated outside an event (plat_now_ns)
static _Atomic int hooks_on;       // vr_game_start switched the camera hooks on: play_at is kept

// Link's state's name (his component 107, the name at its +0x244: "Wait", "CrouchGuard"...), or his upper body's
// (at +0x34c, "BowChargeUpper"...; with a weapon out, +0x31c "CommonUpper"), "" when there is none
static void link_state_at(char out[48], uint32_t at) {
    out[0] = 0;
    const uint64_t actor = dbg_player_actor();
    uint64_t components = 0, c107 = 0;
    if (!actor || !game_rw(actor + 0x270, 8)) return;
    memcpy(&components, (const void *)(uintptr_t)(actor + 0x270), 8);
    if (!game_rw(components + 107 * 8, 8)) return;
    memcpy(&c107, (const void *)(uintptr_t)(components + 107 * 8), 8);
    if (!game_rw(c107 + at, 47)) return;
    memcpy(out, (const void *)(uintptr_t)(c107 + at), 47);
    out[47] = 0;
}
static void link_state(char out[48]) { link_state_at(out, 0x244); }

// Link aims a ranged item: his upper body in the bow's (the slingshot's too) or the hookshot's states
static int link_aims(void) {
    char upper[48];
    link_state_at(upper, 0x34c);
    return !strncmp(upper, "Bow", 3) || !strncmp(upper, "Hookshot", 8);
}

// The game's thread at each pad read: whether Link plays the ocarina (a DemoOcarina state, a lesson's too), and VR's
// wheels. While one is open vk_xr.cpp holds the game's own quick select (L), which pauses the game. A wheel opens in play alone: the
// gameplay camera updated a moment ago (not the title, a load, an event), Link there and not in one of his scripted
// Demo states (the ocarina among them), and the game not paused by its menus (its three pause steps' flags). An item
// wheel's slot is equipped (its era's AssignableItem enum) a few reads after the release, once the quick select closed.

// VR's hands (Play style Motion): Link's own gear hidden from the picture, so only the hands' copies show. The gear's
// actors (named Weapon_..., and the item held, Item_...) sit in the actor heap near Link's; a sweep of 64 MB either side
// of his actor, kept inside the block of memory his actor is in (outside it reads fault the game's mirroring), 4 MB a
// read of the pad, finds them by an actor's shape (a vtable of the game's, its name's string at
// +0x20, an upright rotation at +0x328 and its place at +0x31c near Link's), and is begun again every few seconds, as
// gear comes and goes, and at once when an item is taken out. Each is checked again before it is touched, and once
// found for a moment (three seconds in an area just entered, while the game still makes it) hidden as first person hides Link (hide_link):
// the alphas of its components of the fade's two kinds below zero, every frame; the game eases them back when this
// stops. Only reads of the game's thread, in the heap and alias ranges alone (a pointer elsewhere is not followed).
static int in_ranges(uint64_t v, uint64_t n) {
    const RtGuestLayout *l = mem_layout();
    const RtRange r[2] = {l->heap, l->alias};
    for (int i = 0; i < 2; i++)
        if (v >= r[i].base && v - r[i].base < r[i].size && n <= r[i].size - (v - r[i].base)) return 1;
    return 0;
}
static int readable(uint64_t v, uint64_t n) {   // the 64 KB blocks' answers kept a while
    enum { SLOTS = 4096 };
    static uint64_t block[SLOTS];
    static int8_t ok[SLOTS];
    if (!in_ranges(v, n)) return 0;
    for (uint64_t b = v >> 16; b <= (v + n - 1) >> 16; b++) {
        const unsigned h = (unsigned)(b * 2654435761u) % SLOTS;
        if (block[h] != b + 1) block[h] = b + 1, ok[h] = (int8_t)mem_readable(b << 16, 1 << 16);
        if (!ok[h]) return 0;
    }
    return 1;
}
// a gear actor of Link's at v: its name (Weapon_..., or the held item's) and an actor's rotation and place near him
static int gear_actor(uint64_t v, const char *item, const float link[3]) {
    uint64_t np = 0;
    if (!readable(v, 0x360)) return 0;
    memcpy(&np, (const void *)(uintptr_t)(v + 0x20), 8);
    if (!readable(np, 40)) return 0;
    char name[40];
    memcpy(name, (const void *)(uintptr_t)np, 39), name[39] = 0;
    // the held item's actor: its model's name begins with the actor's (Item_Bottle for Item_Bottle_Fish) or the other way
    const size_t stem = strcspn(item, "."), nl = strlen(name);
    const int held = item[0] && nl >= 6 && (!strncmp(name, item, stem) || (nl <= stem && !strncmp(item, name, nl)));
    if (memcmp(name, "Weapon_", 7) && !held) return 0;
    float pos[3], m[9];
    memcpy(pos, (const void *)(uintptr_t)(v + 0x31c), 12), memcpy(m, (const void *)(uintptr_t)(v + 0x328), 36);
    for (int r = 0; r < 3; r++) {
        const float n = m[3 * r] * m[3 * r] + m[3 * r + 1] * m[3 * r + 1] + m[3 * r + 2] * m[3 * r + 2];
        if (!isfinite(n) || fabsf(n - 1.0f) > 0.05f) return 0;
    }
    for (int i = 0; i < 3; i++)
        if (!isfinite(pos[i]) || fabsf(pos[i] - link[i]) > 3.0f) return 0;
    return 1;
}
static void hide_actor(uint64_t actor) {
    static const float hidden = -1.0f;
    uint64_t comps = 0;
    memcpy(&comps, (const void *)(uintptr_t)(actor + 0x270), 8);
    for (int i = 0; i < 128 && readable(comps + i * 8, 8); i++) {   // (the fades are at 14, 76 and 79)
        uint64_t c = 0, vt = 0;
        memcpy(&c, (const void *)(uintptr_t)(comps + i * 8), 8);
        if (!readable(c, 0x140)) continue;
        memcpy(&vt, (const void *)(uintptr_t)c, 8);
        if (vt == MOD_MAIN_BASE + 0x4b4a5a0) dbg_poke(c + 0x118, &hidden, sizeof hidden);
        else if (vt == MOD_MAIN_BASE + 0x4b4b4a8) dbg_poke(c + 0x130, &hidden, sizeof hidden);
    }
}
static void hide_gear(const char *item) {
    enum { MAX = 16, WINDOW = 64 << 20, STEP = 4 << 20 };
    static uint64_t link_actor, arrived;   // Link's actor, and when it was new (an area entered)
    static char last_item[96];
    static uint64_t gear[MAX], since[MAX], sweep_at, sweep_end, next_sweep;   // since: when first found (plat_now_ns)
    static int told[MAX];
    static int n;
    float link[3];
    const uint64_t player = dbg_player_actor();
    if (!player || !dbg_player_pos(link) || strcmp(cfg_value("NX2NAVI_VR_PLAY"), "motion") ||
        strcmp(cfg_value("NX2NAVI_VR_CONTROLS"), "meta") || !vk_xr_head_active()) {
        n = 0, sweep_at = sweep_end = 0;
        return;
    }
    int kept = 0;   // the ones found still gear (an actor's slot may be another's now)
    for (int i = 0; i < n; i++)
        if (gear_actor(gear[i], item, link)) gear[kept] = gear[i], since[kept] = since[i], told[kept++] = told[i];
    n = kept;
    const uint64_t now = plat_now_ns();
    if (player != link_actor) link_actor = player, arrived = now;
    if (strcmp(item, last_item)) {   // an item taken out: its actor looked for at once
        snprintf(last_item, sizeof last_item, "%s", item);
        if (item[0]) next_sweep = 0, sweep_end = sweep_at;
    }
    if (sweep_at >= sweep_end && now >= next_sweep) {   // a sweep begun, within the block of memory Link's actor is in
        RtMemoryInfo mi;
        next_sweep = now + 3000000000ull;
        if (!mem_query(player, &mi) && mi.state && (mi.perm & 3) == 3) {
            const uint64_t lo = player - mi.base > WINDOW ? player - WINDOW : mi.base;
            const uint64_t hi = mi.base + mi.size - player > WINDOW ? player + WINDOW : mi.base + mi.size;
            sweep_at = (lo + 7) & ~7ull, sweep_end = hi - 8;
        }
    }
    if (sweep_at < sweep_end) {   // its next 2 MB
        const uint64_t to = sweep_at + STEP < sweep_end ? sweep_at + STEP : sweep_end;
        for (uint64_t q = sweep_at; q < to; q += 8) {
            uint64_t vt;
            memcpy(&vt, (const void *)(uintptr_t)q, 8);
            if (vt < MOD_MAIN_BASE || vt >= MOD_MAIN_BASE + 0x8000000ull || !gear_actor(q, item, link)) continue;
            int dup = 0;
            for (int i = 0; i < n; i++) dup |= gear[i] == q;
            if (!dup && n < MAX) gear[n] = q, since[n] = now, told[n++] = 0;
        }
        sweep_at = to;
    }
    // hidden once found for a moment, or three seconds in an area just entered (never while the game still makes it)
    const uint64_t settle = now - arrived < 5000000000ull ? 3000000000ull : 300000000ull;
    for (int i = 0; i < n; i++)
        if (now - since[i] > settle) {
            if (!told[i]) {
                uint64_t np = 0;
                char name[40] = "";
                memcpy(&np, (const void *)(uintptr_t)(gear[i] + 0x20), 8);
                if (readable(np, 40)) memcpy(name, (const void *)(uintptr_t)np, 39), name[39] = 0;
                rt_log("vr: hands: Link's %s hidden (%#llx)", name, (unsigned long long)gear[i]);
                told[i] = 1;
            }
            hide_actor(gear[i]);
        }
}

static int hand_item(char out[96]);
void vr_tick(Cpu *c) {
    char state[48];
    link_state(state);
    vk_xr_ocarina(!strncmp(state, "DemoOcarina", 11));   // playing the ocarina (a lesson's too)
    {   // what he holds out (the sword and shield, or an item and the shield), for the draw gesture and VR's hands
        char sword[64], shield[64], item[96];
        if (!dbg_text_get("PlayerStatus.EquipWeapon", sword)) sword[0] = 0;
        if (!dbg_text_get("PlayerStatus.EquipShield", shield)) shield[0] = 0;
        vk_xr_gear(sword, shield);
        int child = 1;
        dbg_flag_get("Sequence_IsChildhood", &child);
        vk_xr_child(child);
        vk_xr_weapon_out(hand_item(item));
        vk_xr_hand_item(item);
        hide_gear(item);
    }
    int slot;
    const int open = vk_xr_wheel(&slot) != 0;
    int p = 0, pre = 0, post = 0;
    dbg_flag_get("Sequence_IsPause", &p), dbg_flag_get("Sequence_IsPreGamePause", &pre);
    dbg_flag_get("Sequence_IsPostGamePause", &post);
    const int camera = !hooks_on || plat_now_ns() - play_at < 200000000ull;   // (without the hooks: Link's state alone)
    const int play = camera && !event_playing() && state[0] && strncmp(state, "Demo", 4);
    vk_xr_wheel_allow(open || (play && !(p || pre || post)));
    vk_xr_in_play(play && !(p || pre || post));
    static int pending_slot = -1, pending_wait;
    int used_kind, used_slot;
    if (vk_xr_wheel_used(&used_kind, &used_slot) && used_kind == VR_WHEEL_ITEMS) pending_slot = used_slot, pending_wait = 12;
    if (pending_slot >= 0 && --pending_wait <= 0) {
        const char *item = vr_wheel_equip(pending_slot);
        const int child = vr_wheel_child();
        if (item) dbg_enum_set(c, child ? "PlayerStatus.AssignableItem.Child" : "PlayerStatus.AssignableItem.Adult", item);
        pending_slot = -1;
    }
}

static int g_aiming;   // hand_item: Link aims or holds a ranged item ready now (the bow, slingshot, hookshot, boomerang)
static uint64_t zr_at, y_at;   // the item button's (ZR) and the sword's (Y) last presses (plat_now_ns), vr_pad

// Whether Link holds something out, and the item in his hand as VR's hands draw it (a model of the game's files), ""
// for the sword. His state's names (all of component 107's, one after another) tell it: a weapon or item out
// (CommonUpper, WeaponEquipOnUpper), aiming or throwing (the bow's and slingshot's Bow...Upper, the hookshot's and
// boomerang's HoldItem, Hookshot..., FaceBattleBoomerang), a bottle's use, a swing; out until half a second without, or
// at once when he puts it away (WeaponEquipOff). The item button with a hand item on it puts that item in his hand, the sword's
// button followed by a sword's attack the sword again; aiming puts the item aimed. A bottle's model shows what it holds
// now (its BottleItem enum).
static int hand_item(char out[96]) {
    out[0] = 0;
    char names[512] = " ";
    const uint64_t actor = dbg_player_actor();
    uint64_t components = 0, c107 = 0;
    if (actor && game_rw(actor + 0x270, 8)) memcpy(&components, (const void *)(uintptr_t)(actor + 0x270), 8);
    if (components && game_rw(components + 107 * 8, 8)) memcpy(&c107, (const void *)(uintptr_t)(components + 107 * 8), 8);
    if (c107 && game_rw(c107 + 0x200, 0x300)) {   // all of component 107's names, one after another
        const char *b = (const char *)(uintptr_t)(c107 + 0x200);
        size_t len = 1;
        for (int o = 0; o < 0x300; o++) {
            if (b[o] < 'A' || b[o] > 'Z' || (o && b[o - 1])) continue;
            int n = 0;
            while (o + n < 0x300 && b[o + n] > 32 && b[o + n] < 127) n++;
            if (n >= 4 && o + n < 0x300 && !b[o + n] && len + n + 2 < sizeof names)
                memcpy(names + len, b + o, n), len += n, names[len++] = ' ', names[len] = 0;
        }
    }
    int child = 1;
    uint32_t item = 0;
    dbg_flag_get("Sequence_IsChildhood", &child);
    dbg_enum_get(child ? "PlayerStatus.AssignableItem.Child" : "PlayerStatus.AssignableItem.Adult", &item);
    // the hand items by their AssignableItem value; a bottle 'A'..'E' by its slot
    static const struct { const char *value, *model; } items[] = {
        {"SlingShot", "Weapon_Range_SlingShot"}, {"Bow", "Weapon_Range_Bow"}, {"Hookshot", "Item_Hookshot.Item_Hookshot_Body"},
        {"Longshot", "Item_Hookshot.Item_Hookshot_Body"}, {"Boomerang", "Item_Boomerang"}, {"Hammer", "Item_Hammer"},
        {"DekuStick", "Item_DekuStick"}, {"BottleA", "A"}, {"BottleB", "B"}, {"BottleC", "C"}, {"BottleD", "D"},
        {"BottleE", "E"}};
    const char *now_item = NULL;
    for (size_t i = 0; i < sizeof items / sizeof *items && !now_item; i++)
        if (item == dbg_name_hash(items[i].value)) now_item = items[i].model;
    static const char *held;          // what the last button put in his hand (NULL the sword)
    static uint64_t seen_zr, seen_y;
    if (zr_at != seen_zr) {
        seen_zr = zr_at;
        if (now_item) held = now_item;
    }
    // the sword's button takes the sword back only when a sword's attack follows (the game ignores it with a bottle out,
    // and the sword hand's swing taps it); the hammer's own swing puts the hammer back
    const uint64_t t0 = plat_now_ns();
    if (y_at != seen_y && t0 - y_at < 400000000ull && (strstr(names, " Cut") || strstr(names, " Charge")) &&
        !strstr(names, "Hammer") && t0 - zr_at > 400000000ull)
        seen_y = y_at, held = NULL;
    if (strstr(names, "Hammer") && now_item && !strcmp(now_item, "Item_Hammer")) held = now_item;
    const int aiming = (strstr(names, " BowReLoadUpper ") || strstr(names, " BowChargeUpper ") ||
                        strstr(names, " BowShootUpper ") || strstr(names, " HoldItem") || strstr(names, " Hookshot") ||
                        strstr(names, " FaceBattleBoomerang ")) && now_item;
    if (aiming) held = now_item;
    g_aiming = aiming;
    static uint64_t out_at;
    const uint64_t t = plat_now_ns();
    if (aiming || strstr(names, " CommonUpper ") || strstr(names, " WeaponEquipOnUpper ") || strstr(names, "Bottle") ||
        strstr(names, "Drink") || strstr(names, " Cut") || strstr(names, " DemoShield") || strstr(names, "Hammer"))
        out_at = t;
    // put away: at once when he puts it away, else after two seconds with nothing out (what the button took stays
    // meanwhile: the item comes out a moment after the press, and a swing's end shows none of those names)
    const int away = strstr(names, " WeaponEquipOff") != NULL;
    if (away || t - out_at > 2000000000ull) held = NULL;
    if (away || t - out_at > 500000000ull) return 0;
    if (!held) return 1;
    if (held[1]) {
        snprintf(out, 96, "%s", held);
        return 1;
    }
    static const struct { const char *value, *model; } bottles[] = {
        {"RedPotion", "Item_Bottle_RedPotion"}, {"Fish", "Item_Bottle_Fish"}, {"Letter", "Item_Bottle_Letter"},
        {"BigPoe", "Item_Bottle_Poe"}, {"Poe", "Item_Bottle_Poe"}, {"Bug", "Item_Bottle_Bug"},
        {"Fairy", "Item_Bottle_Fairy"}, {"MaxMilk", "Item_Bottle_Milk"}, {"HalfMilk", "Item_Bottle_HalfMilk"}};
    char key[96];
    uint32_t what = 0;
    snprintf(key, sizeof key, "PlayerStatus.AssignableItem.BottleInfo.Bottle%c.BottleItem", held[0]);
    dbg_enum_get(key, &what);
    snprintf(out, 96, "Item_Bottle");   // empty, or what has no model of its own (the potions' other colours, fire)
    for (size_t i = 0; i < sizeof bottles / sizeof *bottles; i++)
        if (what == dbg_name_hash(bottles[i].value)) snprintf(out, 96, "%s", bottles[i].model);
    return 1;
}

// ov_input.c npad_states: in first person play, while the left stick is pushed sideways or back the game gets ZL held,
// so Link strafes as Z-targeting does, facing the way he faced (the head's, from running forward or standing). Pushed
// forward (within 45 degrees) he runs where the head looks, unless the head is within NX2NAVI_VR_RUN_ZONE degrees of
// its rest (the game's own camera's way, which the head turns from): then straight that way, so a head held about
// forward never steers him. The stick is turned so the game, which moves him relative to the head's view, runs him
// so, and turn_link faces him that way. The player's own ZL is kept.
static double head_s, head_c = 1;   // the head's view's level direction (sin, cos of its yaw), from the last camera update
static double base_s, base_c = 1;   // the game's own camera's, before the head turned it
static double run_s, run_c = 1;     // the way a run straight ahead takes
static uint64_t run_at;             // when the stick last ran him straight ahead (plat_now_ns)

void vr_pad(uint64_t *buttons, int32_t *lx, int32_t *ly) {
    {   // the item button's and the sword's presses, for the item in hand (hand_item)
        static int zr_was, y_was;
        const int zr = (int)(*buttons >> 9 & 1), y = (int)(*buttons >> 3 & 1);
        if (zr && !zr_was) zr_at = plat_now_ns();
        if (y && !y_was) y_at = plat_now_ns();
        zr_was = zr, y_was = y;
    }
    if (plat_now_ns() - fp_at > 200000000ull) return;
    const int64_t m2 = (int64_t)*lx * *lx + (int64_t)*ly * *ly;
    if (m2 < (int64_t)8000 * 8000) return;
    if (!(*ly > 0 && 2 * (int64_t)*ly * *ly >= m2)) {
        *buttons |= 1u << 8;   // ZL
        return;
    }
    const double zone = cfg_int("NX2NAVI_VR_RUN_ZONE") * M_PI / 180.0;
    const double off = atan2(head_s * base_c - head_c * base_s, head_c * base_c + head_s * base_s);
    if (!(fabs(off) < zone)) return;
    // the stick's way from the rest's forward (s, c) and right (-c, s), then given relative to the head's
    const double wx = base_s * *ly - base_c * *lx, wz = base_c * *ly + base_s * *lx;
    const double f = wx * head_s + wz * head_c, r = -wx * head_c + wz * head_s;
    *ly = (int32_t)fmax(-32767, fmin(32767, f));
    *lx = (int32_t)fmax(-32767, fmin(32767, r));
    const double n = sqrt(wx * wx + wz * wz);
    run_s = wx / n, run_c = wz / n, run_at = plat_now_ns();
}

static void turn_link(const double ahead[3]) {
    const uint64_t actor = dbg_player_actor();
    if (!actor || strcmp(cfg_value("NX2NAVI_VR_CAMERA"), "first")) return;
    const double n = sqrt(ahead[0] * ahead[0] + ahead[2] * ahead[2]);
    if (!(n > 1e-4)) return;
    head_s = ahead[0] / n, head_c = ahead[2] / n;
    const int straight = plat_now_ns() - run_at < 100000000ull;   // running straight ahead: that way
    const float s = (float)(straight ? run_s : head_s), c = (float)(straight ? run_c : head_c);
    uint64_t components = 0, c0 = 0, facing = 0;
    memcpy(&components, (const void *)(uintptr_t)(actor + 0x270), 8);
    if (game_rw(components, 8)) memcpy(&c0, (const void *)(uintptr_t)components, 8);
    if (game_rw(c0, 0x68)) memcpy(&facing, (const void *)(uintptr_t)(c0 + 0x60), 8);
    float t[12];
    if (!game_rw(facing, 0x1f8 + sizeof t)) return;
    memcpy(t, (const void *)(uintptr_t)(facing + 0x1f8), sizeof t);
    if (fabsf(t[5] - 1.0f) > 0.01f) return;   // not upright now
    const float r[12] = {c, 0, s, t[3], 0, 1, 0, t[7], -s, 0, c, t[11]};
    dbg_poke(facing + 0x1f8, r, sizeof r);
    fp_at = plat_now_ns();
}

// Play style Motion: while Link aims (g_aiming), the game's aim (its camera's ray, d) steered onto where the sword
// hand's controller points, through the game's own motion aiming: its six-axis sensor is fed turn rates (radians a
// second, the Switch controller's frame) proportional to the yaw and pitch between the two, every camera update, so
// the aim follows the controller wherever it points and does not drift; the right stick still adds to it. The view's
// base yaw is held meanwhile (vk_xr_aim_freeze), else it would turn with the aim it chases. Which of the sensor's
// axes turn the aim's yaw and pitch: pitch on x, yaw on z (NX2NAVI_VR_GYRO, "x+z+", may change which and which way).
static double wrap_pi(double a) {
    while (a > M_PI) a -= 2 * M_PI;
    while (a < -M_PI) a += 2 * M_PI;
    return a;
}
static void steer_aim(const double d[3]) {
    const int pointed = d && g_aiming && !strcmp(cfg_value("NX2NAVI_VR_PLAY"), "motion") &&
                        !strcmp(cfg_value("NX2NAVI_VR_CONTROLS"), "meta") && vk_xr_head_active();
    vk_xr_aim_freeze(pointed);
    double t[3];
    if (!pointed || !vk_xr_pointer(t)) return;
    const double yaw_err = wrap_pi(atan2(t[0], t[2]) - atan2(d[0], d[2]));
    const double pitch_err = asin(fmax(-1, fmin(1, t[1]))) - asin(fmax(-1, fmin(1, d[1])));
    char spec[8] = "x+z+";
    plat_getenv("NX2NAVI_VR_GYRO", spec, sizeof spec);
    double gyro[3] = {0, 0, 0};
    const double k = 10.0, most = 8.0;
    const double rate[2] = {fmax(-most, fmin(most, k * pitch_err)), fmax(-most, fmin(most, k * yaw_err))};
    for (int i = 0; i < 2; i++) {
        const int axis = spec[2 * i] == 'x' ? 0 : spec[2 * i] == 'y' ? 1 : 2;
        gyro[axis] += spec[2 * i + 1] == '-' ? -rate[i] : rate[i];
    }
    static const double acc[3] = {0, 0, -1};   // lying face up, at rest
    motion_feed(motion_global(), acc, gyro, kern_now_ns());
}

void vr_camera_update(Cpu *c, int scene) {
    static const int up_reg[3] = {7, 16, 6};
    if (scene != 1 && scene != 2) return;
    const int event = event_playing();
    const int k = scene - 1;
    if (scene == 1) head_camera = 0;
    if (scene == 2 ? !event : event) return;   // the camera that shows: Link's in play, the events' in an event
    if (scene == 1) play_at = plat_now_ns();
    double pos[3], at[3];
    float up[3];
    for (int i = 0; i < 3; i++) pos[i] = reg_d(c, i), at[i] = reg_d(c, 3 + i), up[i] = reg_s(c, up_reg[i]);
    // the view put on last time handed back as it was (the game did not choose its camera again): the game's own from
    // before it is used, so the head is never put on twice
    static double in_pos[2][3], in_at[2][3], out_pos[2][3], out_at[2][3];
    static float in_up[2][3];
    static int have_out[2];
    static unsigned fed_back;
    if (have_out[k] && !memcmp(pos, out_pos[k], sizeof pos) && !memcmp(at, out_at[k], sizeof at)) {
        memcpy(pos, in_pos[k], sizeof pos), memcpy(at, in_at[k], sizeof at), memcpy(up, in_up[k], sizeof up);
        if (!fed_back++) rt_log("vr: the game's camera handed back the head's view: its own view from before used");
    }
    memcpy(in_pos[k], pos, sizeof pos), memcpy(in_at[k], at, sizeof at), memcpy(in_up[k], up, sizeof up);
    have_out[k] = 0;
    if (scene == 1) {   // the game's own camera's level way: the head's rest, for a run straight ahead
        const double fx = at[0] - pos[0], fz = at[2] - pos[2], n = sqrt(fx * fx + fz * fz);
        if (n > 1e-6) base_s = fx / n, base_c = fz / n;
    }
    if (scene == 1) {   // aiming: the game's own camera's ray, where its shots go, for the head's view's reticle
        const int aims = link_aims();
        double d[3] = {at[0] - pos[0], at[1] - pos[1], at[2] - pos[2]};
        const double l = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l > 1e-6) d[0] /= l, d[1] /= l, d[2] /= l;
        vk_xr_aim(aims && l > 1e-6, pos, d);
        steer_aim(l > 1e-6 ? d : NULL);
    }
    double eye[3];
    if (scene == 1 && vk_xr_head_active() && first_person_eye(eye))   // the game's camera's direction kept, from his eyes
        for (int i = 0; i < 3; i++) at[i] += eye[i] - pos[i], pos[i] = eye[i];
    if (scene == 2) {   // an event's camera raised by Link's height for his age, and NX2NAVI_VR_EVENT_UP cm more
        int child = 1;
        dbg_flag_get("Sequence_IsChildhood", &child);
        const double lift = (child ? 1.27 : 1.80) + cfg_int("NX2NAVI_VR_EVENT_UP") / 100.0;
        pos[1] += lift, at[1] += lift;
    }
    vk_xr_update_step(frame_step_update_s());
    if (!vk_xr_camera(pos, at, up)) return;
    for (int i = 0; i < 3; i++) set_d(c, i, pos[i]), set_d(c, 3 + i, at[i]), set_s(c, up_reg[i], up[i]);
    memcpy(out_pos[k], pos, sizeof pos), memcpy(out_at[k], at, sizeof at), have_out[k] = 1;
    head_camera = c->x[19];
    if (scene != 1) return;   // Link turns in play alone
    const double ahead[3] = {at[0] - pos[0], at[1] - pos[1], at[2] - pos[2]};
    turn_link(ahead);
}

void vr_lens_host(Cpu *c) {
    float angle;
    memcpy(&angle, (const void *)(uintptr_t)(c->x[19] + 0x13c), sizeof angle);   // the instruction: ldr s0, [x19, #0x13c]
    float headset;
    if (head_camera && c->x[19] == head_camera && vk_xr_lens(&headset) && isfinite(headset) && headset > 0.05f &&
        headset < 3.0f)
        angle = headset;
    set_s(c, 0, angle);
}

void vr_game_start(void) {
    const int lens = rt_patch_set("vrlens");
    if (lens < 0 || rt_patch_set("freecam") < 0) {
        rt_log("vr: this game version has no camera sites for the head's view: the picture stays on the screen");
        return;
    }
    rt_patch_switch(lens, 1);
    dbg_camera_watch(1);   // the free camera's site, where vr_camera_update runs
    hooks_on = 1;
    rt_log("vr: the game's camera follows the headset (NX2NAVI_VR_VIEW=head)");
}
