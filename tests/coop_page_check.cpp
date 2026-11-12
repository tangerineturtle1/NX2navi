// The menu's Co-op tab (runtime/ui/coop_page.cpp) without the game: the page's own code, the menu's rows and shapes, the
// real room-secret parser and rules (coop_proto.c, coop_state.c) and Dear ImGui, driven frame by frame with the key
// events the menu hands ImGui (menu.cpp drain_events and pad_events: a key's down and up, a held key's repeats as more
// downs, a typed letter as its key and its character), the co-op layer and the settings answered here. Checks typing a
// name, a relay and a secret with the letters that switch tabs elsewhere, Make, Join with a wrong secret and a pasted
// one, the offer accepted and declined, a held or repeated confirm never accepting, removing, resetting or leaving, the
// maker's rules and the others' read-only ones, the maker's key to compare, a player not identified (Accept greyed,
// Remove by member id only while the same session holds it), Remove by key, the pools' contributors and their reset,
// the three warnings before the first Accept, the relay's states; with the keyboard and with the pad.
// tests/coop_page_check.py builds and runs it; the exit status is the number of failed checks. COOP_PAGE_FAULT=1 puts
// the cursor on each window's acting button as the window opens (Right, as a player would), a fault the checks must
// catch.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../runtime/ui/coop_page.cpp"   // its statics (the window's step, the field) are checked below
#include "imgui_internal.h"
#include "ui/menu_art.h"

// ---- the settings: the co-op rows of cfg.c's table, in memory

static CfgMeta kMetas[] = {
    {"COOP", "0", "0|1", 1},
    {"COOP_NAME", "", NULL, 1},
    {"COOP_SERVER", "", NULL, 1},
    {"COOP_PROGRESS", "shared", "shared|own", 1},
    {"COOP_ITEMS", "shared", "shared|own", 1},
    {"COOP_SONGS", "shared", "shared|own", 1},
    {"COOP_QUEST", "shared", "shared|own", 1},
    {"COOP_KEYS", "pool", "pool|own", 1},
    {"COOP_RUPEES", "own", "own|pool", 1},
    {"COOP_AMMO", "own", "own|pool", 1},
};
enum { N_METAS = sizeof kMetas / sizeof kMetas[0] };
static char g_set[N_METAS][160];
static bool g_is_set[N_METAS];
static unsigned g_cfg_version = 1;

static int meta_index(const char *key) {
    if (!strncmp(key, "NX2NAVI_", 8)) key += 8;
    for (int i = 0; i < N_METAS; i++)
        if (!strcmp(kMetas[i].key, key)) return i;
    return -1;
}

static void settings_clear(void) {
    memset(g_is_set, 0, sizeof g_is_set);
    g_cfg_version++;
}

// ---- the co-op layer, as the test sets it

static CoopView g_fake;
static CoopOffer g_fake_offer;
static CoopContributor g_fake_contrib[8];
static int g_fake_ncontrib;
static bool g_fake_maker, g_fault;
static int n_create, n_join, n_leave, n_remove, last_remove, n_accept, n_remove_key, n_reset;
static uint32_t last_accept;
static char last_join[64], g_clipboard[256], g_copied_text[64], last_remove_key[24];
static bool g_paste_asked;

extern "C" {
const CfgMeta *cfg_meta(const char *key) {
    int i = meta_index(key);
    return i >= 0 ? &kMetas[i] : NULL;
}
const CfgMeta *cfg_meta_at(int i) { return i >= 0 && i < N_METAS ? &kMetas[i] : NULL; }
const char *cfg_get(const char *key) {
    int i = meta_index(key);
    return i >= 0 && g_is_set[i] ? g_set[i] : NULL;
}
const char *cfg_value(const char *key) {
    int i = meta_index(key);
    if (i < 0) return "";
    return g_is_set[i] && g_set[i][0] ? g_set[i] : kMetas[i].def;
}
const char *cfg_default(const char *key) {
    int i = meta_index(key);
    return i >= 0 ? kMetas[i].def : NULL;
}
int cfg_int(const char *key) { return atoi(cfg_value(key)); }
double cfg_double(const char *key) { return atof(cfg_value(key)); }
int cfg_bool(const char *key) { return strcmp(cfg_value(key), "0") != 0 && cfg_value(key)[0]; }
int cfg_from_env(const char *) { return 0; }
int cfg_pending(const char *) { return 0; }   // the rows' "after restart" note: no setting here waits for a restart
void cfg_set(const char *key, const char *value) {
    int i = meta_index(key);
    if (i < 0) return;
    snprintf(g_set[i], sizeof g_set[i], "%s", value);
    g_is_set[i] = true;
    g_cfg_version++;
}
int cfg_save(void) { return 1; }
int cfg_reset(const char *const *keys, int n) {
    for (int k = 0; k < n; k++)
        if (int i = meta_index(keys[k]); i >= 0) g_is_set[i] = false;
    g_cfg_version++;
    return n;
}
unsigned cfg_version(void) { return g_cfg_version; }
void menu_sound_play(MenuSound) {}
const char *ov_language(void) { return "en-US"; }
const char *plat_ui_language(void) { return "en"; }
uint64_t plat_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
void rt_log(const char *, ...) {}
int menu_art(MenuArt, ImTextureID *, float *, float *) { return 0; }
void plat_mutex_lock(PlatMutex *) {}
void plat_mutex_unlock(PlatMutex *) {}
int menu_is_open(void) { return 1; }

void coop_view_get(CoopView *out) { *out = g_fake; }
int coop_command(CoopCmd cmd, const char *secret) {
    if (g_fake.state == COOP_OFF) return 0;   // as the layer: nothing while co-op is off
    if (cmd == COOP_CMD_CREATE) n_create++;
    if (cmd == COOP_CMD_JOIN) n_join++, snprintf(last_join, sizeof last_join, "%s", secret ? secret : "");
    if (cmd == COOP_CMD_LEAVE) n_leave++;
    return 1;
}
int coop_remove_member(int member) { return n_remove++, last_remove = member, 1; }
int coop_game_offer(CoopOffer *out) {
    *out = g_fake_offer;
    return out->token != 0;
}
// As the layer: refused while a member in the room has not said who they are.
int coop_game_accept(uint32_t token) {
    if (!token || token != g_fake_offer.token || g_fake_offer.unidentified) return 0;
    return n_accept++, last_accept = token, 1;
}
int coop_game_remove_key(const char *fingerprint) {
    if (!g_fake_maker) return 0;
    return n_remove_key++, snprintf(last_remove_key, sizeof last_remove_key, "%s", fingerprint), 1;
}
int coop_game_reset_pools(void) { return g_fake_maker ? (n_reset++, 1) : 0; }
int coop_game_contributors(CoopContributor *out, int max) {
    int n = g_fake_ncontrib < max ? g_fake_ncontrib : max;
    memcpy(out, g_fake_contrib, sizeof *out * (size_t)n);
    return n;
}
int coop_game_is_maker(void) { return g_fake_maker; }
int plat_clipboard_set(const char *utf8) { return snprintf(g_copied_text, sizeof g_copied_text, "%s", utf8), 1; }
void gfx_window_paste(void) { g_paste_asked = true; }
int menu_paste_take(char *out, size_t cap) {
    if (!g_paste_asked) return 0;
    g_paste_asked = false;
    snprintf(out, cap, "%s", g_clipboard);
    return 1;
}
}

// ---- frames and keys

static int failures, n_tab_keys;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            failures++;                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static ImGuiTextBuffer g_text;   // the text the last frame drew with ImGui's text calls (lines, windows)

static int g_fault_release;   // frames until the fault's Right comes up again; 0 none down
static bool g_fault_due;      // a window opened: Right goes down once no item is held active (ImGui moves no cursor then)

static void frame(void) {
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1 / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(1200, 640), ImGuiCond_Always);
    ImGui::Begin("##menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::LogToBuffer();
    opt_begin("##coop", 1);   // as menu.cpp tab_coop
    coop_rows(1);
    opt_end();
    const bool fresh = g_page_new;
    coop_after_page(1);
    g_text.clear();
    g_text.append(GImGui->LogBuffer.c_str());
    ImGui::LogFinish();
    // menu.cpp layout_menu's tab keys, which the page's window and a text field keep for themselves
    const bool keeps_keys = coop_modal() || io.WantTextInput;
    if (!keeps_keys && (ImGui::IsKeyPressed(ImGuiKey_E, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false))) n_tab_keys++;
    ImGui::End();
    ImGui::Render();
    // the deliberate fault: a window that acts on the room opens with the cursor moved onto its acting button
    if (g_fault_release && !--g_fault_release) io.AddKeyEvent(ImGuiKey_RightArrow, false);
    if (g_fault && fresh && g_open && !field_page()) g_fault_due = true;
    if (g_fault_due && !g_open) g_fault_due = false;
    // ImGui moves no cursor while the key that opened the window is held or in the window's first frames
    const bool held = ImGui::IsKeyDown(ImGuiKey_Enter) || ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown) || GImGui->ActiveId;
    if (g_fault_due && !fresh && !held && !g_fault_release)
        io.AddKeyEvent(ImGuiKey_RightArrow, true), g_fault_release = 2, g_fault_due = false;
}

static void frames(int n) {
    for (int i = 0; i < n; i++) frame();
}

static void key(ImGuiKey k, bool down) { ImGui::GetIO().AddKeyEvent(k, down); }

// A press and its release, a few frames apart as a player's are.
static void tap(ImGuiKey k) {
    key(k, true);
    frames(3);
    key(k, false);
    frames(3);
}

// Held for n frames with the system's repeats (another down every frame), then released.
static void hold(ImGuiKey k, int n) {
    for (int i = 0; i < n; i++) {
        key(k, true);
        frame();
    }
    key(k, false);
    frames(3);
}

// Typed on a keyboard: a letter's key goes down, its character arrives, the key comes up (SDL's order).
static void type(const char *s) {
    for (; *s; s++) {
        char c = *s;
        ImGuiKey k = c >= 'a' && c <= 'z' ? (ImGuiKey)(ImGuiKey_A + (c - 'a'))
                   : c >= 'A' && c <= 'Z' ? (ImGuiKey)(ImGuiKey_A + (c - 'A'))
                                          : ImGuiKey_None;
        if (k != ImGuiKey_None) key(k, true);
        ImGui::GetIO().AddInputCharacter((unsigned)c);
        frame();
        if (k != ImGuiKey_None) key(k, false);
        frame();
    }
}

struct Input {
    const char *name;
    ImGuiKey act, back, up, down, right, left;
};
static const Input kKeyboard = {"keyboard", ImGuiKey_Enter, ImGuiKey_Escape, ImGuiKey_UpArrow, ImGuiKey_DownArrow,
                                ImGuiKey_RightArrow, ImGuiKey_LeftArrow};
static const Input kPad = {"gamepad", ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight, ImGuiKey_GamepadDpadUp,
                           ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadDpadLeft};

static ImGuiID window_id(const char *label) {
    ImGuiWindow *w = ImGui::FindWindowByName("##coopwin");
    return w ? w->GetID(label) : 0;
}

static const char *nav_name(void) {
    static char out[64];
    static const char *const labels[] = {"Not now##no", "Accept##yes", "Cancel##no", "Remove##yes", "Reset the pools##yes",
                                         "Stay##no", "Leave the room##yes", "OK##yes", "Join a room##yes", "Paste",
                                         "##coop_field"};
    for (const char *l : labels)
        if (GImGui->NavId && GImGui->NavId == window_id(l)) return l;
    snprintf(out, sizeof out, "%08x", GImGui->NavId);
    return out;
}

static bool shows(UiTextId id) { return strstr(g_text.c_str(), menu_text(id)) != nullptr; }

// The cursor on a row of the page, as a move of the D-pad would put it there; then act on it.
static void row(const char *key_of_row) {
    opt_focus_key(key_of_row);
    frames(3);
}

static void press_row(const Input &in, const char *key_of_row) {
    row(key_of_row);
    tap(in.act);
}

static void close_window(void) {
    coop_cancel();
    frames(4);
}

static void counters_clear(void) {
    n_create = n_join = n_leave = n_remove = n_accept = n_remove_key = n_reset = 0;
    last_remove = 0, last_accept = 0, last_join[0] = last_remove_key[0] = 0, n_tab_keys = 0;
}

// ---- the views the layer hands out

static void member(CoopView &v, int id, const char *name, bool maker, uint32_t heard_ms) {
    CoopPeer &m = v.member[v.n++];
    memset(&m, 0, sizeof m);
    m.id = (uint8_t)id, m.maker = maker, m.heard_ms = heard_ms, m.session = 0x5e550000u + (uint64_t)id;
    snprintf(m.name, sizeof m.name, "%s", name);
}

static void out_of_room(CoopState st, const char *status) {
    memset(&g_fake, 0, sizeof g_fake);
    memset(&g_fake_offer, 0, sizeof g_fake_offer);
    g_fake_ncontrib = 0, g_fake_maker = false;
    g_fake.state = st;
    g_fake.version++;
    snprintf(g_fake.status, sizeof g_fake.status, "%s", status);
}

static const char kAliceKey[] = "a11c-e000-0000-0001", kBobKey[] = "b0b0-0000-0000-0002", kDaveKey[] = "da7e-0000-0000-0004";

// An entry of the offer: the other member id, by the name and key its game signed (heard), or not identified.
static void offer_member_add(int id, const char *name, const char *key, bool maker, bool heard) {
    CoopOfferMember &m = g_fake_offer.member[g_fake_offer.n++];
    memset(&m, 0, sizeof m);
    m.id = id, m.maker = maker, m.heard = heard;
    snprintf(m.name, sizeof m.name, "%s", name);   // the relay's name until heard (coop_game.c offer_of)
    snprintf(m.key, sizeof m.key, "%s", heard ? key : "");
}

// Bob in Alice's room (Alice made it), or Alice in her own room with Bob; the offer waiting or not.
static void in_room_as(bool maker, uint32_t token, bool accepted) {
    memset(&g_fake, 0, sizeof g_fake);
    g_fake.state = COOP_JOINED;
    snprintf(g_fake.code, sizeof g_fake.code, "K7M3PR-BCDFGH-234567");
    member(g_fake, 1, "Alice", true, maker ? 0 : 1200);
    member(g_fake, 2, "Bob", false, maker ? 2300 : 0);
    g_fake.self = maker ? 1 : 2, g_fake.maker = 1, g_fake.made = maker;
    snprintf(g_fake.name, sizeof g_fake.name, "%s", maker ? "Alice" : "Bob");
    g_fake.version++;
    g_fake_maker = maker, g_fake_ncontrib = 0;
    memset(&g_fake_offer, 0, sizeof g_fake_offer);
    g_fake_offer.token = token, g_fake_offer.accepted = accepted, g_fake_offer.copy = accepted;
    if (maker) offer_member_add(2, "Bob", kBobKey, false, true);
    else offer_member_add(1, "Alice", kAliceKey, true, true);
    snprintf(g_fake_offer.maker_key, sizeof g_fake_offer.maker_key, "%s", kAliceKey);
    coop_rules_default(&g_fake_offer.rules);
    g_fake_offer.rules.epoch = 1, g_fake_offer.rules.maker = 1;
}

// Carol in the room, who has not said who she is: her game never identified itself.
static void carol_unidentified(void) {
    member(g_fake, 3, "Carol", false, UINT32_MAX);
    g_fake.version++;
    offer_member_add(3, "Carol", "", false, false);
    g_fake_offer.unidentified = 1;
}

static bool shows_text(const char *s) { return strstr(g_text.c_str(), s) != nullptr; }

// ---- the checks

static void check_fields_by_keyboard(void) {
    printf("keyboard text:\n");
    settings_clear();
    out_of_room(COOP_OFF, "");
    counters_clear();
    frames(4);
    // with no relay, Make and Join are greyed: a press makes nothing
    press_row(kKeyboard, "##coop_make");
    press_row(kKeyboard, "##coop_join");
    CHECK(n_create == 0 && !g_open, "Make and Join with no relay: %d rooms, window %d", n_create, g_open);

    // the name: the field takes the cursor, Q and E are letters (no tab switch), Enter keeps it
    press_row(kKeyboard, "NX2NAVI_COOP_NAME");
    CHECK(g_open && g_page == P_NAME && GImGui->ActiveId == window_id("##coop_field"), "Name: window %d step %d, field %s",
          g_open, g_page, GImGui->ActiveId == window_id("##coop_field") ? "active" : "not active");
    type("Zelda Qe");
    CHECK(n_tab_keys == 0 && coop_modal(), "typing Q and E in the name switched tabs %d times", n_tab_keys);
    tap(ImGuiKey_Enter);
    CHECK(!g_open && !strcmp(cfg_value("NX2NAVI_COOP_NAME"), "Zelda Qe"), "Name: '%s' kept, window %d",
          cfg_value("NX2NAVI_COOP_NAME"), g_open);
    printf("  name typed with Q and E: '%s', %d tab switches\n", cfg_value("NX2NAVI_COOP_NAME"), n_tab_keys);
    // the same letters with no field: the tab keys work (the gate above is not always shut)
    tap(ImGuiKey_E);
    CHECK(n_tab_keys == 1, "E on the page itself: %d tab switches", n_tab_keys);
    n_tab_keys = 0;

    // Esc in a field drops the edit and closes the window
    press_row(kKeyboard, "NX2NAVI_COOP_NAME");
    type("x");
    tap(ImGuiKey_Escape);
    CHECK(!g_open && !strcmp(cfg_value("NX2NAVI_COOP_NAME"), "Zelda Qe"), "Esc in the name: '%s', window %d",
          cfg_value("NX2NAVI_COOP_NAME"), g_open);

    // the relay
    press_row(kKeyboard, "NX2NAVI_COOP_SERVER");
    type("127.0.0.1:24680");
    tap(ImGuiKey_Enter);
    CHECK(!strcmp(cfg_value("NX2NAVI_COOP_SERVER"), "127.0.0.1:24680"), "Relay: '%s'", cfg_value("NX2NAVI_COOP_SERVER"));
    printf("  relay typed: '%s'\n", cfg_value("NX2NAVI_COOP_SERVER"));

    // Make while co-op is off: the switch goes on and the room is made once the coop thread is up
    press_row(kKeyboard, "##coop_make");
    CHECK(cfg_bool("NX2NAVI_COOP") && n_create == 0, "Make with co-op off: switch %d, %d rooms before it is up",
          cfg_bool("NX2NAVI_COOP"), n_create);
    out_of_room(COOP_LEFT, "");
    frames(2);
    CHECK(n_create == 1, "Make once co-op is up: %d rooms", n_create);
    printf("  Make with co-op off: switch on, then %d room made\n", n_create);

    // Join with a wrong secret: refused in the window, nothing sent; then one in lower case with dashes
    press_row(kKeyboard, "##coop_join");
    CHECK(g_open && g_page == P_JOIN && GImGui->ActiveId == window_id("##coop_field"), "Join: the field is not active");
    type("k7m3pq-bcdfgh-23456e");   // E and Q are not in the alphabet of secrets
    tap(ImGuiKey_Enter);
    frames(2);
    CHECK(n_join == 0 && g_open && g_field_bad && shows(UI_MENU_COOP_SECRET_BAD) && n_tab_keys == 0,
          "a wrong secret: %d joins, window %d, said %d, %d tab switches", n_join, g_open, shows(UI_MENU_COOP_SECRET_BAD),
          n_tab_keys);
    printf("  wrong secret: %d joins, the window says it is not one\n", n_join);
    ImGui::ClearActiveID();
    frames(2);
    tap(ImGuiKey_Escape);
    press_row(kKeyboard, "##coop_join");
    type("k7m3pr-bcdfgh-234567");
    tap(ImGuiKey_Enter);
    CHECK(n_join == 1 && !strcmp(last_join, "K7M3PRBCDFGH234567") && !g_open, "the secret in lower case: %d joins of '%s'",
          n_join, last_join);
    printf("  secret in lower case with dashes: %d join of %s\n", n_join, last_join);
    // the relay knows no such room: the page says the secret is wrong
    out_of_room(COOP_LEFT, "no such room");
    frames(3);
    CHECK(shows(UI_MENU_COOP_NO_ROOM), "no such room: the page does not say the secret may be wrong");
    printf("  the relay has no such room: the page says so\n");
}

static void check_join_by_pad(void) {
    printf("gamepad join:\n");
    out_of_room(COOP_LEFT, "");
    counters_clear();
    frames(3);
    // the window opens with the cursor on Cancel: A, A goes back
    press_row(kPad, "##coop_join");
    CHECK(g_open && g_page == P_JOIN && !strcmp(nav_name(), "Cancel##no"), "Join by pad: cursor on %s", nav_name());
    tap(kPad.act);
    CHECK(!g_open && n_join == 0, "Join, A, A: window %d, %d joins", g_open, n_join);
    // Paste: the clipboard's secret with a line end and spaces, then Join
    snprintf(g_clipboard, sizeof g_clipboard, " k7m3pr-bcdfgh-234567\n");
    press_row(kPad, "##coop_join");
    tap(kPad.up);
    CHECK(!strcmp(nav_name(), "Paste"), "up from Cancel: %s", nav_name());
    tap(kPad.act);
    frames(2);
    CHECK(!strcmp(g_field, "K7M3PR-BCDFGH-234567"), "pasted: '%s'", g_field);
    tap(kPad.down);
    tap(kPad.right);
    CHECK(!strcmp(nav_name(), "Join a room##yes"), "down, right: %s", nav_name());
    tap(kPad.act);
    CHECK(n_join == 1 && !strcmp(last_join, "K7M3PRBCDFGH234567"), "pasted secret: %d joins of '%s'", n_join, last_join);
    // an empty clipboard says so
    snprintf(g_clipboard, sizeof g_clipboard, "\n");
    press_row(kPad, "##coop_join");
    tap(kPad.up);
    tap(kPad.act);
    frames(2);
    CHECK(g_paste_empty && shows(UI_MENU_COOP_PASTE_EMPTY), "an empty clipboard is not said");
    close_window();
    printf("  pasted secret joined by pad: %d join; an empty clipboard is said\n", n_join);
}

static void check_offer(const Input &in) {
    printf("%s offer:\n", in.name);
    in_room_as(false, 0x1234567u, false);
    counters_clear();
    g_accepted_once = false;   // as at a start of the game: the warnings come before the first Accept
    frames(4);
    CHECK(shows(UI_MENU_COOP_OFFER_TRUST) == false && strstr(g_text.c_str(), "Alice (made the room)"),
          "the page does not name the room's members");
    CHECK(shows_text(kAliceKey) && shows(UI_MENU_COOP_OFFER_COMPARE), "the page does not show the maker's key to compare");
    CHECK(!shows(UI_MENU_COOP_POOLS) && !shows(UI_MENU_COOP_RESET), "a player who is not the maker sees the pools' remedies");
    // the row's press opens the window with the cursor on Not now; the same button again goes back
    press_row(in, "##coop_answer");
    CHECK(g_open && g_page == P_ACCEPT && !strcmp(nav_name(), "Not now##no"), "Accept window: cursor on %s", nav_name());
    CHECK(shows(UI_MENU_COOP_OFFER_COPY) && shows(UI_MENU_COOP_OFFER_TRUST), "the window does not say what accepting does");
    CHECK(shows(UI_MENU_COOP_WARN_SAVE) && shows(UI_MENU_COOP_WARN_ROOM) && shows(UI_MENU_COOP_WARN_KEY) && shows_text(kAliceKey),
          "the first Accept window lacks the three warnings or the maker's key");
    tap(in.act);
    CHECK(n_accept == 0 && !g_open, "Accept or not, act, act: %d accepts, window %d", n_accept, g_open);
    // held on the row through the window's opening, then held again in it
    row("##coop_answer");
    hold(in.act, 120);
    CHECK(n_accept == 0, "act held 2 s on the row: %d accepts", n_accept);
    if (!g_open) press_row(in, "##coop_answer");
    hold(in.act, 120);
    CHECK(n_accept == 0, "act held again in the window: %d accepts", n_accept);
    // declined: Not now, and B
    close_window();
    press_row(in, "##coop_answer");
    tap(in.back);
    CHECK(n_accept == 0 && !g_open, "back: %d accepts", n_accept);
    // a new player while the cursor is on Accept: the window shows the new offer and the cursor goes back to Not now
    press_row(in, "##coop_answer");
    tap(in.right);
    CHECK(!strcmp(nav_name(), "Accept##yes"), "right from Not now: %s", nav_name());
    g_fake_offer.token = 0x7654321u, g_fake_offer.n = 2;
    snprintf(g_fake_offer.member[1].name, sizeof g_fake_offer.member[1].name, "Carol");
    frames(3);
    CHECK(!strcmp(nav_name(), "Not now##no") && strstr(g_text.c_str(), "Carol"), "a new offer under the cursor: %s",
          nav_name());
    tap(in.act);
    CHECK(n_accept == 0, "a new offer, act: %d accepts", n_accept);
    printf("  act, act / held / back / a new offer: %d accepts\n", n_accept);
    // accepted on purpose: right, act
    press_row(in, "##coop_answer");
    tap(in.right);
    tap(in.act);
    CHECK(n_accept == 1 && last_accept == 0x7654321u && !g_open, "Accept chosen: %d accepts of %08x", n_accept, last_accept);
    printf("  right, act: %d accept of the offer shown\n", n_accept);
    g_fake_offer.accepted = 1, g_fake_offer.copy = 1;
    frames(3);
    CHECK(shows(UI_MENU_COOP_ACCEPTED), "accepted: the page does not say so");
    // a later offer (another player): the warnings were read before the first Accept, the key is still there
    g_fake_offer.token = 0x7654322u, g_fake_offer.accepted = 0, g_fake_offer.copy = 0;
    press_row(in, "##coop_answer");
    CHECK(g_open && !shows(UI_MENU_COOP_WARN_SAVE) && shows_text(kAliceKey), "a later Accept window: warnings %d, key %d",
          shows(UI_MENU_COOP_WARN_SAVE), shows_text(kAliceKey));
    close_window();
    printf("  the maker's key shown to compare; the three warnings before the first Accept only\n");
    g_fake_offer.accepted = 1, g_fake_offer.copy = 1;
    frames(3);

    // the rules of someone else's room: read only
    const char *before = cfg_value("NX2NAVI_COOP_RUPEES");
    press_row(in, "##coop_room_rule5");
    row("##coop_room_rule5");
    tap(in.right);
    CHECK(!strcmp(cfg_value("NX2NAVI_COOP_RUPEES"), before), "a member changed the room's rule: rupees %s",
          cfg_value("NX2NAVI_COOP_RUPEES"));
    printf("  a member's press and right on a rule: rupees still %s\n", cfg_value("NX2NAVI_COOP_RUPEES"));
}

static void check_maker(const Input &in) {
    printf("%s maker:\n", in.name);
    settings_clear();
    in_room_as(true, 0x2222223u, true);
    counters_clear();
    frames(4);
    // a rule the maker changes: rupees into a pool; items cannot leave a shared world (greyed)
    row("NX2NAVI_COOP_RUPEES");
    tap(in.right);
    CHECK(!strcmp(cfg_value("NX2NAVI_COOP_RUPEES"), "pool"), "the maker's rupees: %s", cfg_value("NX2NAVI_COOP_RUPEES"));
    row("NX2NAVI_COOP_ITEMS");
    tap(in.right);
    tap(in.act);
    CHECK(!strcmp(cfg_value("NX2NAVI_COOP_ITEMS"), "shared"), "items with progress shared: %s", cfg_value("NX2NAVI_COOP_ITEMS"));
    printf("  rupees -> %s; items stay %s while progress is shared\n", cfg_value("NX2NAVI_COOP_RUPEES"),
           cfg_value("NX2NAVI_COOP_ITEMS"));

    // Remove: the cursor on Cancel; act again, held, back: nothing; right, act: removed by its key
    CHECK(shows_text(kAliceKey), "the maker's page does not show its own key to read out");
    press_row(in, "##coop_member2");
    CHECK(g_open && g_page == P_REMOVE && !strcmp(nav_name(), "Cancel##no") && shows_text(kBobKey),
          "Remove window: cursor on %s, key shown %d", nav_name(), shows_text(kBobKey));
    tap(in.act);
    CHECK(n_remove + n_remove_key == 0 && !g_open, "Remove, act, act: %d removals", n_remove + n_remove_key);
    row("##coop_member2");
    hold(in.act, 120);
    if (!g_open) press_row(in, "##coop_member2");
    hold(in.act, 120);
    CHECK(n_remove + n_remove_key == 0, "act held on a player and in the window: %d removals", n_remove + n_remove_key);
    close_window();
    press_row(in, "##coop_member1");   // the maker's own row opens nothing
    CHECK(!g_open, "the maker's own row opened a window");
    press_row(in, "##coop_member2");
    tap(in.right);
    tap(in.act);
    CHECK(n_remove == 0 && n_remove_key == 1 && !strcmp(last_remove_key, kBobKey), "Remove chosen: %d by id, %d by key (%s)",
          n_remove, n_remove_key, last_remove_key);
    printf("  Remove: act, act / held: none; right, act: %d removal of key %s\n", n_remove_key, last_remove_key);

    // a player who never said who they are: by member id, and only while the same session holds it
    carol_unidentified();
    frames(3);
    CHECK(shows_text("Players in the room who are not identified"), "the maker's page does not say Carol is not identified");
    press_row(in, "##coop_member3");
    CHECK(g_open && g_page == P_REMOVE && !strcmp(nav_name(), "Cancel##no") && shows(UI_MENU_COOP_NOT_IDENTIFIED),
          "Remove Carol: cursor on %s, not identified said %d", nav_name(), shows(UI_MENU_COOP_NOT_IDENTIFIED));
    tap(in.right);
    g_fake.member[2].session++, g_fake.version++;   // Carol left and someone else got her id meanwhile
    frames(3);
    CHECK(n_remove == 0 && !g_open, "another session under the window: %d removals by id, window %d", n_remove, g_open);
    press_row(in, "##coop_member3");
    tap(in.right);
    tap(in.act);
    CHECK(n_remove == 1 && last_remove == 3 && n_remove_key == 1, "Remove of a player not identified: %d by id (%d)", n_remove,
          last_remove);
    printf("  a player not identified: removed by id only while the same session holds it (%d removal)\n", n_remove);

    // the pools: Dave, who left, took 3 keys beyond what he brought; Bob none; the maker's own row is left out
    g_fake_ncontrib = 3;
    memset(g_fake_contrib, 0, sizeof g_fake_contrib);
    snprintf(g_fake_contrib[0].key, sizeof g_fake_contrib[0].key, "%s", kAliceKey), g_fake_contrib[0].self = 1;
    g_fake_contrib[0].spent[0] = 9;
    snprintf(g_fake_contrib[1].key, sizeof g_fake_contrib[1].key, "%s", kBobKey), g_fake_contrib[1].member = 2;
    snprintf(g_fake_contrib[1].name, sizeof g_fake_contrib[1].name, "Bob");
    snprintf(g_fake_contrib[2].key, sizeof g_fake_contrib[2].key, "%s", kDaveKey), g_fake_contrib[2].spent[0] = 3;
    counters_clear();
    frames(3);
    char spent_row[48];
    snprintf(spent_row, sizeof spent_row, "##coop_spent%s", kDaveKey);
    CHECK(shows(UI_MENU_COOP_POOLS) && !shows(UI_MENU_COOP_SPENT_NONE), "the maker's pools list nobody who took more");
    press_row(in, spent_row);
    CHECK(g_open && g_page == P_REMOVE && !strcmp(nav_name(), "Cancel##no") && shows_text(kDaveKey) &&
              shows(UI_MENU_COOP_NOT_IN_ROOM),
          "Remove Dave's key: cursor on %s, key %d", nav_name(), shows_text(kDaveKey));
    tap(in.act);
    row(spent_row);
    hold(in.act, 120);
    if (!g_open) press_row(in, spent_row);
    hold(in.act, 120);
    CHECK(n_remove_key == 0, "act, act / held on Dave's key: %d removals", n_remove_key);
    close_window();
    press_row(in, spent_row);
    tap(in.right);
    tap(in.act);
    CHECK(n_remove_key == 1 && !strcmp(last_remove_key, kDaveKey), "Dave's key chosen: %d removals of %s", n_remove_key,
          last_remove_key);
    // Reset the pools: the cursor on Cancel; act again, held: nothing; right, act: reset
    press_row(in, "##coop_reset");
    CHECK(g_open && g_page == P_RESET && !strcmp(nav_name(), "Cancel##no"), "Reset window: cursor on %s", nav_name());
    tap(in.act);
    row("##coop_reset");
    hold(in.act, 120);
    if (!g_open) press_row(in, "##coop_reset");
    hold(in.act, 120);
    CHECK(n_reset == 0, "act, act / held on Reset the pools: %d resets", n_reset);
    close_window();
    press_row(in, "##coop_reset");
    tap(in.right);
    tap(in.act);
    CHECK(n_reset == 1 && !g_open, "Reset chosen: %d resets", n_reset);
    printf("  pools: Dave's key removed (%d), act, act / held: nothing; Reset: %d\n", n_remove_key, n_reset);
    g_fake_ncontrib = 0;

    // the secret: hidden until asked (its row's press shows it, a second hides it), copied on its row
    CHECK(!g_show_secret, "the secret shows unasked");
    press_row(in, "##coop_secret");
    CHECK(g_show_secret, "the secret's row did not show it");
    press_row(in, "##coop_secret");
    CHECK(!g_show_secret, "the secret's row did not hide it again");
    press_row(in, "##coop_copy");
    CHECK(!strcmp(g_copied_text, "K7M3PR-BCDFGH-234567"), "copied: '%s'", g_copied_text);

    // Leave: the cursor on Stay, the maker told what leaving costs; act again, held: stays; right, act: leaves
    press_row(in, "##coop_leave");
    CHECK(g_open && g_page == P_LEAVE && !strcmp(nav_name(), "Stay##no") && shows(UI_MENU_COOP_LEAVE_MAKER),
          "Leave window: cursor on %s", nav_name());
    tap(in.act);
    CHECK(n_leave == 0 && !g_open, "Leave, act, act: %d leaves", n_leave);
    row("##coop_leave");
    hold(in.act, 120);
    if (!g_open) press_row(in, "##coop_leave");
    hold(in.act, 120);
    CHECK(n_leave == 0, "act held on Leave and in the window: %d leaves", n_leave);
    close_window();
    press_row(in, "##coop_leave");
    tap(in.right);
    tap(in.act);
    CHECK(n_leave == 1, "Leave chosen: %d leaves", n_leave);
    printf("  Leave: act, act / held: stays; right, act: %d leave\n", n_leave);
}

// A player in the room who is not identified: their game never said who they are, or two keys present themselves for
// their session (the engine disputes it: heard 0, counted in unidentified, the same to the page). The page names them
// and says why, the Accept window says it too with Accept greyed (right does not reach it, act does nothing); once they
// are identified Accept works.
static void check_unidentified(const Input &in) {
    printf("%s not identified:\n", in.name);
    in_room_as(false, 0x3333333u, false);
    carol_unidentified();
    counters_clear();
    frames(4);
    CHECK(shows(UI_MENU_COOP_NOT_IDENTIFIED) && shows_text("Carol (") && shows_text("Players in the room who are not identified"),
          "the page does not say Carol is not identified and why the room cannot be accepted");
    press_row(in, "##coop_answer");
    CHECK(g_open && !strcmp(nav_name(), "Not now##no") && shows_text("Players in the room who are not identified"),
          "Accept window with Carol not identified: cursor on %s", nav_name());
    tap(in.right);
    tap(in.act);
    CHECK(n_accept == 0, "right, act with a player not identified: %d accepts", n_accept);
    close_window();
    g_fake_offer.unidentified = 0, g_fake_offer.member[1].heard = 1;
    snprintf(g_fake_offer.member[1].name, sizeof g_fake_offer.member[1].name, "Carol");
    snprintf(g_fake_offer.member[1].key, sizeof g_fake_offer.member[1].key, "c4r0-0000-0000-0003");
    g_fake_offer.token = 0x3333334u;
    press_row(in, "##coop_answer");
    tap(in.right);
    tap(in.act);
    CHECK(n_accept == 1 && last_accept == 0x3333334u, "Carol identified, right, act: %d accepts", n_accept);
    printf("  Accept greyed while Carol is not identified; 1 accept once she is\n");
}

// What of a row's note the Co-op tab shows. option_rows.cpp draws a row's note at 0.72 of the font size, at most 60 % of
// (row width - 2 pad) wide, and clips the rest. At the harness's 1903x1030 window Review T measured, from a picture of
// the tab, a row 689 px wide with a 23 px pad (a note shows at most 386 px) and the note "press to show" 113 px wide;
// the menu's font is Roboto Medium (runtime/ui/fonts, the menu's text below the CJK glyphs it takes from the game), here
// scaled to that size. The first part of the note up to its first "  ·  " must show whole. zh-Hans is not measured: its
// glyphs come from the game's own files.
static ImFont *g_roboto;
static void note_fits(const char *what, const char *note) {
    CHECK(g_roboto, "the menu's font (COOP_PAGE_FONT) did not load: the note's width is not measured");
    if (!g_roboto) return;
    const float row = 689, pad = 23, ref_px = 113, cap = (row - 2 * pad) * 0.6f;
    const float size = 26.0f * 0.72f * ref_px / g_roboto->CalcTextSizeA(26.0f * 0.72f, 1e9f, 0, "press to show").x;
    const char *end = strstr(note, "  ·  ");
    const float width = g_roboto->CalcTextSizeA(size, 1e9f, 0, note, end ? end : note + strlen(note)).x;
    CHECK(width <= cap, "%s: the note's first part is %.0f px, of which the row shows %.0f: '%s'", what, width, cap, note);
    printf("  %s: the note's first part %.0f px of the %.0f px a row shows\n", what, width, cap);
}

// A key the player accepted that now signs another name (the seventh review: it took another member's place in the
// room): Dave holds member 3 with the key the player accepted as Carol. The engine makes it a new offer
// (CoopOfferMember.accepted_as); the page says the name it was accepted as in the offer, in Dave's row and in the Accept
// window, and Accept takes the new offer only.
static void check_moved_key(const Input &in) {
    printf("%s a key accepted under another name:\n", in.name);
    static const char kCarolKey[] = "c4r0-0000-0000-0003";
    in_room_as(false, 0x4444444u, false);
    member(g_fake, 3, "Dave", false, 800);
    g_fake.version++;
    offer_member_add(3, "Dave", kCarolKey, false, true);
    snprintf(g_fake_offer.member[1].accepted_as, sizeof g_fake_offer.member[1].accepted_as, "Carol");
    counters_clear();
    frames(4);
    char moved[96], in_offer[128], note[400];
    snprintf(moved, sizeof moved, menu_text(UI_MENU_COOP_ACCEPTED_AS), "Carol");
    snprintf(in_offer, sizeof in_offer, "Dave (%s)", moved);
    // the row's note is drawn into the draw list, not logged as text: it is checked as the page builds it, and a row
    // shows only the start of a long note, so the name the key was accepted as comes first
    member_note(g_fake.member[2], offer_member(g_fake_offer, 3), false, note, sizeof note);
    CHECK(shows_text(in_offer), "the offer does not say Dave's key was accepted as Carol");
    CHECK(!strncmp(note, moved, strlen(moved)), "Dave's row does not begin with his key's accepted name: '%s'", note);
    note_fits("Dave's row", note);
    static const char *const others[] = {"ru", "pt-BR"};   // the same part in the other languages; zh-Hans: see note_fits
    for (const char *l : others) {
        char other[160];
        snprintf(other, sizeof other, ui_text_in(l, UI_MENU_COOP_ACCEPTED_AS), "Carol");
        note_fits(l, other);
    }
    press_row(in, "##coop_answer");
    CHECK(g_open && g_page == P_ACCEPT && !strcmp(nav_name(), "Not now##no") && shows_text(in_offer),
          "Accept window with Carol's key under Dave: cursor on %s", nav_name());
    tap(in.right);
    tap(in.act);
    CHECK(n_accept == 1 && last_accept == 0x4444444u, "Carol's key under Dave, right, act: %d accepts of %08x", n_accept,
          last_accept);
    printf("  the offer, Dave's row and the Accept window say 'a key you accepted as Carol'; %d accept of the new offer\n",
           n_accept);
}

// The relay's states the page says in words: its limit on joins from one network, and a room it lost.
static void check_states(void) {
    printf("relay states:\n");
    out_of_room(COOP_CONNECTING, "too many joins from this network; trying again in a minute");
    frames(3);
    CHECK(shows(UI_MENU_COOP_JOIN_LIMIT), "the relay's join limit is not said");
    out_of_room(COOP_CONNECTING, "the room was lost; waiting for it");
    frames(3);
    CHECK(shows(UI_MENU_COOP_LOST), "a room the relay lost is not said");
    printf("  the join limit and a lost room are said\n");
}

// The conversions of a printf format in order ("%s%u" for "heard %u s ago, %s"), "%%" left out.
static void conversions(const char *f, char *out, size_t cap) {
    size_t k = 0;
    for (; *f && k + 3 < cap; f++) {
        if (*f != '%') continue;
        if (f[1] == '%') {
            f++;
            continue;
        }
        while (f[1] && !strchr("sdiuxXcfgp", f[1])) f++;
        out[k++] = '%', out[k++] = f[1];
    }
    out[k] = 0;
}

// The checks --probe-services makes of the catalog (ov_probe.c probe_menu_text), for the Co-op tab's texts: English,
// Russian, Brazilian Portuguese and Simplified Chinese, none empty, each short enough for the description pane, and
// the same printf conversions in every language as in English. The other menu texts that lack a language are counted
// and named, not failed: they are not this page's.
static void check_catalog(void) {
    static const char *const langs[] = {"en", "ru", "pt-BR", "zh-Hans"};
    for (int id = UI_MENU_TAB_COOP; id <= UI_MENU_COOP_ACCEPTED_AS; id++) {
        char en[32], other[32];
        conversions(ui_text_in("en", (UiTextId)id), en, sizeof en);
        for (const char *l : langs) {
            CHECK(ui_text_defined(l, (UiTextId)id), "co-op text %d ('%s') lacks %s", id, ui_text_in("en", (UiTextId)id), l);
            CHECK(strlen(ui_text_in(l, (UiTextId)id)) + 256 < UI_DESC_BYTES, "co-op text %d in %s is too long", id, l);
            conversions(ui_text_in(l, (UiTextId)id), other, sizeof other);
            CHECK(!strcmp(en, other), "co-op text %d: %s has '%s', English '%s'", id, l, other, en);
        }
    }
    printf("catalog: the %d co-op texts in en, ru, pt-BR and zh-Hans, with English's conversions\n",
           UI_MENU_COOP_ACCEPTED_AS - UI_MENU_TAB_COOP + 1);
    int gaps = 0;
    for (int id = UI_MENU_FIRST; id < UI_MENU_TAB_COOP; id++)
        for (const char *l : langs)
            if (!ui_text_defined(l, (UiTextId)id)) {
                if (!gaps++) printf("  not this page's, lacking a language:");
                printf(" %d (%s, '%s')", id, l, ui_text_in("en", (UiTextId)id));
            }
    if (gaps) printf("\n");
}

int main(void) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;   // as menu.cpp
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.Fonts->AddFontDefault();   // the font the page is drawn with here; Roboto only measures notes (note_fits)
    static const ImWchar ranges[] = {0x20, 0x24f, 0x400, 0x4ff, 0x2000, 0x206f, 0};
    if (const char *f = getenv("COOP_PAGE_FONT")) g_roboto = io.Fonts->AddFontFromFileTTF(f, 26.0f, nullptr, ranges);
    unsigned char *px;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    frames(2);
    g_fault = getenv("COOP_PAGE_FAULT") && atoi(getenv("COOP_PAGE_FAULT"));
    if (g_fault) printf("deliberate fault: each window that acts on the room opens with Right pressed\n");

    check_fields_by_keyboard();
    check_join_by_pad();
    check_offer(kKeyboard);
    check_offer(kPad);
    check_maker(kKeyboard);
    check_maker(kPad);
    check_unidentified(kKeyboard);
    check_unidentified(kPad);
    check_moved_key(kKeyboard);
    check_moved_key(kPad);
    check_states();
    check_catalog();

    ImGui::DestroyContext();
    printf("coop page: %s\n", failures ? "see FAIL lines" : "ok");
    return failures;
}
