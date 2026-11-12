// The Co-op tab (coop_page.h, docs/coop.md). The page reads the co-op layer only through the copies the layer hands out
// and drives it only through the calls the layer makes safe from any thread, all in the section "the co-op layer"
// below, so the menu's present thread never waits for the coop thread's work. What it writes (the name, the relay, the
// rules, the co-op switch) are settings, through cfg like every row, and reach nx2navi.ini as theirs do; the room's
// secret is never a setting, never logged, and shown only when asked.
//
// Every window that acts on the room (Accept, Remove, Reset the pools, Leave) opens with the cursor on its safe button
// (Not now, Cancel, Stay) whenever it opens, and its acting button has an ID of its own, so a second press of the button
// that opened it, a held button's repeats or a double tap land on the safe one.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "menu_sound.h"
#include "ui/coop_page.h"
#include "ui/menu_skin.h"
#include "ui/option_rows.h"

extern "C" {
#include "cfg.h"
#include "coop.h"
#include "coop_offer.h"
#include "platform/platform.h"
#include "ui/menu.h"
#include "ui_text.h"
}

enum Page { P_NONE, P_NAME, P_SERVER, P_JOIN, P_ACCEPT, P_REMOVE, P_RESET, P_LEAVE };
enum { FIELD_CAP = 160, PASTE_FRAMES = 120, CONTRIBUTORS = 24 };

static bool g_open, g_window_shown, g_page_new, g_by_pad;
static int g_page;
static char g_field[FIELD_CAP];          // the text of the window's field (a name, an address, a secret)
static bool g_field_bad, g_paste_empty;  // the secret typed is not one; the clipboard had no text
static int g_paste_wait;                 // frames left to wait for the clipboard's text (gfx_window_paste)
static CoopOffer g_shown;                // the offer the Accept window shows (its number: g_shown.token)
// The player the Remove window names: by the key's fingerprint when they have one (identified, or a pool contributor
// not in the room), else by their member id while the same session holds it (a player who is not identified: they
// never said who they are, or two keys present themselves for their session)
static int g_member;
static uint64_t g_member_session;
static char g_member_name[COOP_NAME_MAX + 1], g_member_key[20];
static char g_return[48];                // the row the cursor goes back to when the window closes

static CoopView g_view;                  // this frame's copies of the layer's state
static CoopOffer g_offer;
static CoopContributor g_contrib[CONTRIBUTORS];
static int g_ncontrib;
static bool g_maker;                     // this game holds the key that signs the room's rules (coop_game_is_maker)
static bool g_accepted_once;             // an Accept went through in this start: the warnings were read before it
static bool g_refused;                   // the last Accept was refused by the layer (the room changed meanwhile)
static int g_layout = -1;                // the page's last kind: 0 not in a room, 1 reaching one, 2 in one
static bool g_show_secret;
static int g_msg = -1;                   // a line under the rows (UiTextId), -1 none
static bool g_msg_warn;
static int g_copied;                     // the secret's copy: 1 done, -1 failed, 0 nothing to say
static uint64_t g_copied_at;
// A Make or Join while co-op is off: the switch is turned on and the command waits until the coop thread has come up.
static int g_pending;                    // the CoopCmd, 0 none
static char g_pending_secret[COOP_SECRET_LEN + 1];
static uint64_t g_pending_until;
static bool g_joining;                   // the last command was a join: "no such room" then means the secret is wrong

static const char *const kRuleKeys[COOP_CAT_N] = {"NX2NAVI_COOP_PROGRESS", "NX2NAVI_COOP_ITEMS", "NX2NAVI_COOP_SONGS",
                                                   "NX2NAVI_COOP_QUEST", "NX2NAVI_COOP_KEYS", "NX2NAVI_COOP_RUPEES",
                                                   "NX2NAVI_COOP_AMMO"};

static bool in_room(const CoopView &v) { return v.state == COOP_JOINED || v.state == COOP_RECONNECTING; }

static void say(int text, bool warn) { g_msg = text, g_msg_warn = warn; }

// ---- the co-op layer: every call the page makes to it. The page knows a member by the id the view gives and, once it
// said who it is, by its key's fingerprint, never by its name; an offer's number and whether it is accepted are the
// layer's, and the page passes back only the number of the offer it showed the player. A removal goes by the key; only
// a player who is not identified goes by member id, checked against the session that held the id when the
// window opened.

// This frame's copies: the view (coop.h) and, in a room, the offer (coop_offer.h: without a number while there is none
// to accept, with the members and whether their rules disagree all the same), the pools' contributors and whether this
// game is the room's maker; outside a room none of them.
static void layer_read(CoopView *v, CoopOffer *o, CoopContributor *c = nullptr, int *nc = nullptr, bool *maker = nullptr) {
    coop_view_get(v);
    coop_game_offer(o);
    const bool room = in_room(*v);
    if (!room) memset(o, 0, sizeof *o);
    if (c) *nc = room ? coop_game_contributors(c, CONTRIBUTORS) : 0;
    if (maker) *maker = room && coop_game_is_maker();
}

// Make (secret NULL: a new one), join (secret: canonical) or leave; false when the layer took nothing (co-op off).
static bool layer_command(int cmd, const char *secret) { return coop_command((CoopCmd)cmd, secret) != 0; }

// The maker's removal of a key (also of a player who left; one in the room is removed at the relay too).
static void layer_remove_key(const char *fingerprint) { coop_game_remove_key(fingerprint); }

// The maker's removal of a player who is not identified, by member id.
static void layer_remove_member(int id) { coop_remove_member(id); }

static void layer_reset_pools(void) { coop_game_reset_pools(); }

// Accepts the offer the player was shown, and no other: false when the layer refuses it (the room has changed
// meanwhile, or a player in it is not identified).
static bool layer_accept(const CoopOffer &shown) { return coop_game_accept(shown.token) != 0; }

static bool layer_same_offer(const CoopOffer &a, const CoopOffer &b) { return a.token == b.token; }

// A secret as typed or pasted (any case, '-' and spaces) into its 18 characters; false when it is not one.
static bool layer_secret(const char *typed, char canon[COOP_SECRET_LEN + 1]) { return coop_secret_normalize(typed, canon) != 0; }

static UiTextId mode_name(int mode) {
    return mode == COOP_MODE_SHARED ? UI_MENU_COOP_SHARED : mode == COOP_MODE_POOL ? UI_MENU_COOP_POOL : UI_MENU_COOP_OWN;
}

// The setting's text as the engine reads it (coop_game.c mode_of): anything but the category's other mode is own.
static int mode_of_setting(int cat) {
    const char *v = cfg_value(kRuleKeys[cat]);
    if (coop_cat_is_pool(cat)) return !strcmp(v, "pool") ? COOP_MODE_POOL : COOP_MODE_OWN;
    return !strcmp(v, "shared") ? COOP_MODE_SHARED : COOP_MODE_OWN;
}

// Make or join (secret: canonical, or NULL for a new room). Co-op off: it is turned on first, as the switch's row would.
static void command(int cmd, const char *secret) {
    g_msg = -1;
    g_joining = cmd == COOP_CMD_JOIN;
    if (g_view.state == COOP_OFF) {
        opt_apply("NX2NAVI_COOP", "1");
        g_pending = cmd;
        snprintf(g_pending_secret, sizeof g_pending_secret, "%s", secret ? secret : "");
        g_pending_until = plat_now_ms() + 3000;
        return;
    }
    if (!layer_command(cmd, secret)) say(UI_MENU_COOP_NOT_STARTED, true);
}

static void pending_tick(void) {
    if (!g_pending) return;
    if (g_view.state != COOP_OFF) {
        if (!layer_command(g_pending, g_pending_secret[0] ? g_pending_secret : nullptr)) say(UI_MENU_COOP_NOT_STARTED, true);
        g_pending = 0;
    } else if (plat_now_ms() > g_pending_until) {
        say(UI_MENU_COOP_NOT_STARTED, true);
        g_pending = 0;
    }
    if (!g_pending) memset(g_pending_secret, 0, sizeof g_pending_secret);
}

static void open_window(int page, const char *ret) {
    g_open = true, g_page = page, g_page_new = true;
    g_by_pad = ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown);   // a pad's A opened it: no keyboard to type with
    g_field_bad = g_paste_empty = false, g_paste_wait = 0;
    snprintf(g_return, sizeof g_return, "%s", ret);
}

static void open_field(int page, const char *initial, const char *ret) {
    open_window(page, ret);
    snprintf(g_field, sizeof g_field, "%s", initial);
}

static void open_accept(void) {
    g_shown = g_offer;
    g_refused = false;
    open_window(P_ACCEPT, "##coop_answer");
}

// The offer's entry for member id (its signed name, its key, whether it is identified or removed); NULL when none.
static const CoopOfferMember *offer_member(const CoopOffer &o, int id) {
    for (int i = 0; i < o.n; i++)
        if (o.member[i].id == id) return &o.member[i];
    return nullptr;
}

// The Remove window for a player: key, their key's fingerprint ("" for a player who is not identified, who is
// then removed by member id while session still holds it); id 0 for a contributor not in the room. ret: the row the
// cursor goes back to.
static void open_remove(int id, uint64_t session, const char *name, const char *key, const char *ret) {
    g_member = id, g_member_session = session;
    snprintf(g_member_name, sizeof g_member_name, "%s", name);
    snprintf(g_member_key, sizeof g_member_key, "%s", key);
    open_window(P_REMOVE, ret);
}

// The name a member is shown by: this player's own name, else the name its game signed once it said who it is, else
// the relay's.
static const char *member_name(const CoopPeer &m, const CoopOfferMember *om) {
    if (m.id == g_view.self) return g_view.name[0] ? g_view.name : m.name;
    return om && om->heard && om->name[0] ? om->name : m.name;
}

static void open_remove_member(const CoopPeer &m) {
    const CoopOfferMember *om = offer_member(g_offer, m.id);
    char ret[32];
    snprintf(ret, sizeof ret, "##coop_member%d", m.id);
    open_remove(m.id, m.session, member_name(m, om), om && om->heard ? om->key : "", ret);
}

// "Alice (made the room), Bob (not identified), Dave (a key you accepted as Carol)" and the rules' categories by mode,
// as the offer and the Accept window say them. A key the player accepted under another name than it signs now is a new
// offer (coop_state.h: CoopOfferMember.accepted_as): the name it was accepted as is said beside the one it signs.
static void offer_lines(const CoopOffer &o, char *who, size_t who_cap, char *shared, size_t shared_cap, char *pooled,
                        size_t pooled_cap) {
    char names[COOP_MAX_MEMBERS * (2 * COOP_NAME_MAX + 200)] = "";
    size_t k = 0;
    for (int i = 0; i < o.n && k < sizeof names; i++) {
        const CoopOfferMember &m = o.member[i];
        const char *note = m.removed ? menu_text(UI_MENU_COOP_REMOVED)
                           : !m.heard ? menu_text(UI_MENU_COOP_NOT_IDENTIFIED)
                           : m.maker  ? menu_text(UI_MENU_COOP_MAKER)
                                      : "";
        char moved[COOP_NAME_MAX + 128] = "";
        if (m.heard && !m.removed && m.accepted_as[0])
            snprintf(moved, sizeof moved, menu_text(UI_MENU_COOP_ACCEPTED_AS), m.accepted_as);
        const bool paren = *note || *moved;
        k += (size_t)snprintf(names + k, sizeof names - k, "%s%s%s%s%s%s%s", i ? ", " : "", m.name, paren ? " (" : "", note,
                              *note && *moved ? "; " : "", moved, paren ? ")" : "");
    }
    snprintf(who, who_cap, menu_text(UI_MENU_COOP_OFFER_WHO), o.n ? names : menu_text(UI_MENU_COOP_OFFER_NOBODY));
    char s[256] = "", p[256] = "";
    size_t ks = 0, kp = 0;
    for (int c = 0; c < COOP_CAT_N; c++) {
        const char *label = menu_text((UiTextId)(UI_MENU_COOP_R_PROGRESS + c));
        if (o.rules.mode[c] == COOP_MODE_SHARED && ks < sizeof s)
            ks += (size_t)snprintf(s + ks, sizeof s - ks, "%s%s", ks ? ", " : "", label);
        if (o.rules.mode[c] == COOP_MODE_POOL && kp < sizeof p)
            kp += (size_t)snprintf(p + kp, sizeof p - kp, "%s%s", kp ? ", " : "", label);
    }
    shared[0] = pooled[0] = 0;
    if (s[0]) snprintf(shared, shared_cap, menu_text(UI_MENU_COOP_OFFER_SHARED), s);
    if (p[0]) snprintf(pooled, pooled_cap, menu_text(UI_MENU_COOP_OFFER_POOLED), p);
    if (!s[0] && !p[0]) snprintf(shared, shared_cap, "%s", menu_text(UI_MENU_COOP_OFFER_NOTHING));
}

// The rules, a row each in coop_state.h's order. editable (not in a room, or this player made it): the rows change this
// player's settings, which are the rules of a room it makes; a category the others force (items, songs and quest
// items follow Progress shared, keys follow Progress own: coop_rules_normalize) is greyed out with its value in effect.
// Otherwise the room's rules, to read.
static void rule_rows(bool editable, const CoopRules *room) {
    CoopRules r;
    if (editable) {
        coop_rules_default(&r);
        for (int c = 0; c < COOP_CAT_N; c++) r.mode[c] = (uint8_t)mode_of_setting(c);
        coop_rules_normalize(&r);
    } else {
        r = *room;
    }
    for (int c = 0; c < COOP_CAT_N; c++) {
        const bool pool = coop_cat_is_pool(c) != 0;
        const UiTextId label = (UiTextId)(UI_MENU_COOP_R_PROGRESS + c), desc = (UiTextId)(UI_MENU_COOP_R_PROGRESS_DESC + c);
        if (!editable) {
            char id[24];
            snprintf(id, sizeof id, "##coop_room_rule%d", c);
            opt_disable_next(UI_MENU_COOP_RULES_SET_BY);
            opt_text_row(id, menu_text(label), menu_text(mode_name(r.mode[c])), nullptr, menu_text(desc), false);
            continue;
        }
        // the values in the order of the setting's list (cfg.c), the default first
        const char *values[2];
        int modes[2];
        if (!pool) values[0] = "shared", values[1] = "own", modes[0] = COOP_MODE_SHARED, modes[1] = COOP_MODE_OWN;
        else if (c == COOP_CAT_KEYS) values[0] = "pool", values[1] = "own", modes[0] = COOP_MODE_POOL, modes[1] = COOP_MODE_OWN;
        else values[0] = "own", values[1] = "pool", modes[0] = COOP_MODE_OWN, modes[1] = COOP_MODE_POOL;
        const char *names[2] = {menu_text(mode_name(modes[0])), menu_text(mode_name(modes[1]))};
        const int cur = r.mode[c] == modes[0] ? 0 : 1;
        CoopRules t = r;   // the other mode, as the engine would take it: a forced category does not keep it
        t.mode[c] = (uint8_t)modes[1 - cur];
        coop_rules_normalize(&t);
        if (t.mode[c] != modes[1 - cur]) opt_disable_next(UI_MENU_COOP_RULE_FOLLOWS);
        opt_choice_list(kRuleKeys[c], label, values, names, 2, desc, cur);
    }
}

static void status_line(void) {
    char line[256];
    if (g_view.state == COOP_CONNECTING || g_pending) {
        snprintf(line, sizeof line, menu_text(UI_MENU_COOP_CONNECTING), g_view.state == COOP_CONNECTING ? g_view.status : "…");
        opt_text_str(line, false);
    } else if (g_view.state == COOP_RECONNECTING) {
        snprintf(line, sizeof line, menu_text(UI_MENU_COOP_RECONNECTING), g_view.status);
        opt_text_str(line, true);
    } else if (g_view.state == COOP_LEFT && g_view.status[0]) {
        snprintf(line, sizeof line, menu_text(UI_MENU_COOP_LEFT), g_view.status);
        opt_text_str(line, strcmp(g_view.status, "you left the room") != 0);
        if (g_joining && strstr(g_view.status, "no such room")) opt_text(UI_MENU_COOP_NO_ROOM, true);
    }
    // the relay's limit on joins from one network: the session waits a minute and tries again by itself
    if (g_view.state != COOP_LEFT && strstr(g_view.status, "trying again in a minute")) opt_text(UI_MENU_COOP_JOIN_LIMIT, true);
    // a room the relay lost (its restart): the session makes it again or waits for it, and says so in its status
    if (g_view.state != COOP_LEFT && strstr(g_view.status, "the room was lost")) opt_text(UI_MENU_COOP_LOST, true);
}

static void rows_out(void) {
    const bool reaching = g_view.state == COOP_CONNECTING || g_pending;
    const char *server = cfg_value("NX2NAVI_COOP_SERVER");
    if (reaching) {
        if (opt_button("##coop_stop", UI_MENU_COOP_STOP, UI_MENU_COOP_STOP_DESC)) {
            if (g_pending) g_pending = 0;
            else layer_command(COOP_CMD_LEAVE, nullptr);
        }
        status_line();
        return;
    }
    const char *name = cfg_value("NX2NAVI_COOP_NAME");
    if (opt_text_row("NX2NAVI_COOP_NAME", menu_text(UI_MENU_COOP_NAME), *name ? name : menu_text(UI_MENU_COOP_AUTO),
                     nullptr, menu_text(UI_MENU_COOP_NAME_DESC), false) == 2)
        open_field(P_NAME, name, "NX2NAVI_COOP_NAME");
    if (opt_text_row("NX2NAVI_COOP_SERVER", menu_text(UI_MENU_COOP_SERVER), *server ? server : menu_text(UI_MENU_COOP_NONE),
                     nullptr, menu_text(UI_MENU_COOP_SERVER_DESC), false) == 2)
        open_field(P_SERVER, server, "NX2NAVI_COOP_SERVER");
    if (!*server) opt_disable_next(UI_MENU_COOP_NEED_SERVER);
    if (opt_button("##coop_make", UI_MENU_COOP_MAKE, UI_MENU_COOP_MAKE_DESC)) command(COOP_CMD_CREATE, nullptr);
    if (!*server) opt_disable_next(UI_MENU_COOP_NEED_SERVER);
    if (opt_button("##coop_join", UI_MENU_COOP_JOIN, UI_MENU_COOP_JOIN_DESC)) open_field(P_JOIN, "", "##coop_join");
    status_line();
    opt_group(UI_MENU_COOP_RULES_MINE);
    rule_rows(true, nullptr);
    opt_group(UI_MENU_COOP_ON);
    opt_toggle("NX2NAVI_COOP", UI_MENU_COOP_ON, UI_MENU_COOP_ON_DESC);
}

// The room's offer while the player has not accepted it: who, what is shared, the key that signs the rules (to compare
// with the maker's), why it cannot be accepted yet, and the row that opens Accept or not. Members whose rules disagree
// are said also while there is no offer.
static void offer_rows(const CoopOffer &o) {
    char line[640];
    if (o.token && !o.accepted) {
        opt_group(UI_MENU_COOP_OFFER);
        char who[2400], shared[320], pooled[320];
        offer_lines(o, who, sizeof who, shared, sizeof shared, pooled, sizeof pooled);
        opt_text_str(who, false);
        if (shared[0]) opt_text_str(shared, false);
        if (pooled[0]) opt_text_str(pooled, false);
        if (o.new_save) opt_text(UI_MENU_COOP_NEW_SAVE);
        if (o.maker_key[0] && !g_maker) {
            snprintf(line, sizeof line, menu_text(UI_MENU_COOP_OFFER_KEY), o.maker_key);
            opt_text_str(line, false);
            opt_text(UI_MENU_COOP_OFFER_COMPARE);
        }
        if (o.disagree) opt_text(UI_MENU_COOP_DISAGREE, true);
        if (o.unidentified) {
            snprintf(line, sizeof line, menu_text(UI_MENU_COOP_UNIDENTIFIED), o.unidentified);
            opt_text_str(line, true);
        }
        if (opt_button("##coop_answer", UI_MENU_COOP_ANSWER, UI_MENU_COOP_ANSWER_DESC)) open_accept();
    } else if (!o.token && o.disagree) {
        opt_group(UI_MENU_COOP_OFFER);
        opt_text(UI_MENU_COOP_DISAGREE, true);
    }
}

// The note under a player's row: the name the player accepted that key as when it now signs another (a new offer),
// first, since a row shows only the start of a long note (option_rows.cpp: at most 60 % of the row); then made the room,
// when they were last heard, their key (or not identified, or removed). om: NULL for this player.
static void member_note(const CoopPeer &m, const CoopOfferMember *om, bool self, char *note, size_t cap) {
    char heard[64] = "", key[64] = "", moved[COOP_NAME_MAX + 128] = "";
    if (!self) {
        if (m.away) snprintf(heard, sizeof heard, "%s", menu_text(UI_MENU_COOP_AWAY));
        else if (m.heard_ms == UINT32_MAX) snprintf(heard, sizeof heard, "%s", menu_text(UI_MENU_COOP_NOT_HEARD));
        else snprintf(heard, sizeof heard, menu_text(UI_MENU_COOP_HEARD), (unsigned)(m.heard_ms / 1000));
    }
    if (om && om->removed) snprintf(key, sizeof key, "%s", menu_text(UI_MENU_COOP_REMOVED));
    else if (om && om->heard) snprintf(key, sizeof key, menu_text(UI_MENU_COOP_KEY), om->key);
    else if (om) snprintf(key, sizeof key, "%s", menu_text(UI_MENU_COOP_NOT_IDENTIFIED));
    if (om && om->heard && !om->removed && om->accepted_as[0])
        snprintf(moved, sizeof moved, menu_text(UI_MENU_COOP_ACCEPTED_AS), om->accepted_as);
    const char *const parts[] = {moved, m.maker ? menu_text(UI_MENU_COOP_MAKER) : "", heard, key};
    size_t k = 0;
    note[0] = 0;
    for (const char *part : parts)
        if (part[0] && k < cap) k += (size_t)snprintf(note + k, cap - k, "%s%s", k ? "  ·  " : "", part);
}

// The players in the room, by the name their game signed once it said who it is; each with its key or "not
// identified". The maker sees its own key to read out to the others, and a Remove on every other row.
static void player_rows(const CoopOffer &o) {
    opt_group(UI_MENU_COOP_PLAYERS);
    if (g_maker && o.maker_key[0]) {
        char line[640];
        snprintf(line, sizeof line, menu_text(UI_MENU_COOP_MY_KEY), o.maker_key);
        opt_text_str(line, false);
    }
    if (o.unidentified && (!o.token || o.accepted)) {   // else the offer above says it
        char line[640];
        snprintf(line, sizeof line, menu_text(UI_MENU_COOP_UNIDENTIFIED), o.unidentified);
        opt_text_str(line, true);
    }
    for (int i = 0; i < g_view.n; i++) {
        const CoopPeer &m = g_view.member[i];
        const bool self = m.id == g_view.self, removable = g_maker && !self;
        const CoopOfferMember *om = self ? nullptr : offer_member(o, m.id);
        const char *name = member_name(m, om);
        char id[32], note[400], desc[UI_DESC_BYTES - 256];
        snprintf(id, sizeof id, "##coop_member%d", m.id);
        member_note(m, om, self, note, sizeof note);
        snprintf(desc, sizeof desc, "%s%s%s", menu_text(UI_MENU_COOP_MEMBER_DESC), removable ? "\n\n" : "",
                 removable ? menu_text(UI_MENU_COOP_REMOVE_DESC) : "");
        const char *value = self ? menu_text(UI_MENU_COOP_YOU) : removable ? menu_text(UI_MENU_COOP_REMOVE) : "";
        if (opt_text_row(id, name, value, note[0] ? note : nullptr, desc, false) == 2 && removable) open_remove_member(m);
    }
}

// For the maker: the keys that took more from the pools than they brought (in the room or not; a player who used keys
// others found is among them), each with a Remove of its key, and the pool reset.
static void pool_rows(void) {
    if (!g_maker) return;
    opt_group(UI_MENU_COOP_POOLS);
    int listed = 0;
    for (int i = 0; i < g_ncontrib; i++) {
        const CoopContributor &c = g_contrib[i];
        if (c.self || c.removed || !(c.spent[0] > 0 || c.spent[1] > 0 || c.spent[2] > 0)) continue;
        char id[48], label[COOP_NAME_MAX + 64], key[64], spent[200], note[280];
        snprintf(id, sizeof id, "##coop_spent%s", c.key);
        snprintf(label, sizeof label, "%s", c.member && c.name[0] ? c.name : menu_text(UI_MENU_COOP_NOT_IN_ROOM));
        snprintf(key, sizeof key, menu_text(UI_MENU_COOP_KEY), c.key);
        snprintf(spent, sizeof spent, menu_text(UI_MENU_COOP_SPENT), (long long)c.spent[0], (long long)c.spent[1],
                 (long long)c.spent[2]);
        snprintf(note, sizeof note, "%s  ·  %s", key, spent);
        if (opt_text_row(id, label, menu_text(UI_MENU_COOP_REMOVE), note, menu_text(UI_MENU_COOP_SPENT_DESC), false) == 2) {
            uint64_t session = 0;
            for (int j = 0; j < g_view.n; j++)
                if (c.member && g_view.member[j].id == c.member) session = g_view.member[j].session;
            open_remove(c.member, session, label, c.key, id);
        }
        listed++;
    }
    if (!listed) opt_text(UI_MENU_COOP_SPENT_NONE);
    if (opt_button("##coop_reset", UI_MENU_COOP_RESET, UI_MENU_COOP_RESET_DESC)) open_window(P_RESET, "##coop_reset");
}

static void rows_room(void) {
    status_line();
    const CoopOffer &o = g_offer;
    offer_rows(o);
    player_rows(o);
    pool_rows();
    opt_group(UI_MENU_COOP_SECRET);
    char shown[COOP_SECRET_TEXT + 1];
    snprintf(shown, sizeof shown, "%s", g_show_secret ? g_view.code : "******-******-******");
    if (opt_text_row("##coop_secret", menu_text(UI_MENU_COOP_SECRET), shown,
                     menu_text(g_show_secret ? UI_MENU_COOP_HIDE : UI_MENU_COOP_SHOW), menu_text(UI_MENU_COOP_SECRET_DESC),
                     false) == 2)
        g_show_secret = !g_show_secret;
    if (g_copied && plat_now_ms() - g_copied_at > 3000) g_copied = 0;
    const char *copied = g_copied > 0 ? menu_text(UI_MENU_COPY_INFO_DONE) : g_copied < 0 ? menu_text(UI_MENU_ACTION_FAILED) : "";
    if (opt_button("##coop_copy", UI_MENU_COOP_COPY, UI_MENU_COOP_COPY_DESC, copied)) {
        g_copied = g_view.code[0] && plat_clipboard_set(g_view.code) ? 1 : -1;
        g_copied_at = plat_now_ms();
    }
    if (o.token && o.accepted && o.copy) opt_text(o.copy > 0 ? UI_MENU_COOP_ACCEPTED : UI_MENU_COOP_COPY_FAILED, o.copy < 0);
    opt_group(UI_MENU_COOP_RULES_ROOM);
    if (g_view.made) {
        rule_rows(true, nullptr);
        opt_text(UI_MENU_COOP_RULES_CHANGE);
    } else if (o.token) {
        rule_rows(false, &o.rules);
    } else {
        opt_text(UI_MENU_COOP_RULES_UNKNOWN);
    }
    opt_group(UI_MENU_COOP_LEAVE);
    if (opt_button("##coop_leave", UI_MENU_COOP_LEAVE, UI_MENU_COOP_LEAVE_DESC)) open_window(P_LEAVE, "##coop_leave");
}

void coop_rows(float scale) {
    (void)scale;
    layer_read(&g_view, &g_offer, g_contrib, &g_ncontrib, &g_maker);
    pending_tick();
    // the page changes its rows when the player gets into a room, reaches for one or leaves it: the cursor goes to a row
    // of the new page whose press changes nothing at once (never Leave, Make or the secret)
    const int layout = in_room(g_view) ? 2 : g_view.state == COOP_CONNECTING || g_pending ? 1 : 0;
    if (layout != g_layout) {
        if (g_layout >= 0) {
            char key[32];
            if (layout == 2) snprintf(key, sizeof key, "##coop_member%d", g_view.self);
            else snprintf(key, sizeof key, "%s", layout == 1 ? "##coop_stop" : "NX2NAVI_COOP_NAME");
            opt_focus_key(key);
        }
        if (layout != 2) g_show_secret = false;
        g_layout = layout;
    }
    if (layout == 2) rows_room();
    else rows_out();
    if (g_msg >= 0) opt_text((UiTextId)g_msg, g_msg_warn);
}

// ---- the window

// The cursor on the item just drawn, shown (as option_rows.cpp puts it on a row). SetItemDefaultFocus as well: on the
// window's first frame ImGui's own pick of a first item would replace the cursor set here.
static void take_cursor(void) {
    ImGui::SetItemDefaultFocus();
    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
    ImGui::SetNavCursorVisible(true);
}

static bool field_page(void) { return g_page == P_NAME || g_page == P_SERVER || g_page == P_JOIN; }

// Two buttons side by side, the safe one first: 1 the first, 2 the second. The cursor goes to the safe one whenever a
// step opens (fresh), not only when the window appears, and the second has an ID of its own ("##yes"): ImGui keeps the
// cursor on an ID across steps, and SetItemDefaultFocus works only on a window's first frame. In a field's window typed
// by keyboard the field has the cursor instead. ImGui activates a button on a press, not on a held key's repeats.
static int two_buttons(const char *no, const char *yes, bool yes_enabled, bool fresh, float scale) {
    const ImGuiStyle &st = ImGui::GetStyle();
    float bw = (ImGui::CalcTextSize(no).x > ImGui::CalcTextSize(yes).x ? ImGui::CalcTextSize(no).x : ImGui::CalcTextSize(yes).x) +
               st.FramePadding.x * 4;
    char id[160];
    snprintf(id, sizeof id, "%s##no", no);
    int r = skin_widget_button(id, ImVec2(bw, 0), scale) ? 1 : 0;
    if (fresh && !(field_page() && !g_by_pad)) take_cursor();
    ImGui::SameLine(0, st.ItemSpacing.x + skin_cursor_reach(scale));
    ImGui::BeginDisabled(!yes_enabled);
    snprintf(id, sizeof id, "%s##yes", yes);
    if (skin_widget_button(id, ImVec2(bw, 0), scale)) r = 2;
    ImGui::EndDisabled();
    return r;
}

static void gold(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(SKIN_GOLD));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

static void warn(const char *text) { ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%s", text); }

// A pasted secret: the characters a secret is typed with, no line ends or tabs, at most the field's room.
static void take_paste(const char *text) {
    char out[FIELD_CAP];
    size_t k = 0;
    for (const char *p = text; *p && k + 1 < sizeof out; p++)
        if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '-' || *p == ' ')
            out[k++] = *p >= 'a' && *p <= 'z' ? (char)(*p - 'a' + 'A') : *p;
    out[k] = 0;
    while (k && out[k - 1] == ' ') out[--k] = 0;
    const char *s = out;
    while (*s == ' ') s++;
    g_paste_empty = !*s;
    if (*s) snprintf(g_field, sizeof g_field, "%s", s), g_field_bad = false;
}

// The text of the field without the spaces around it.
static void trimmed(char *out, size_t cap) {
    const char *s = g_field;
    while (*s == ' ') s++;
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') n--;
    snprintf(out, cap, "%.*s", (int)n, s);
}

// The field's window: Cancel and OK (Join): the name, the relay's address or the room's secret.
static void field_window(bool fresh, float scale, float wrap, bool *close, bool *back) {
    const bool join = g_page == P_JOIN;
    gold(menu_text(join ? UI_MENU_COOP_SECRET : g_page == P_NAME ? UI_MENU_COOP_NAME : UI_MENU_COOP_SERVER));
    ImGui::TextDisabled("%s", menu_text(join ? UI_MENU_COOP_SECRET_HINT : g_page == P_NAME ? UI_MENU_COOP_NAME_DESC
                                                                                           : UI_MENU_COOP_SERVER_DESC));
    if (fresh && !g_by_pad) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(wrap);
    // the name: at most COOP_NAME_MAX bytes, as the relay takes it
    const size_t cap = g_page == P_NAME ? COOP_NAME_MAX + 1 : join ? 64 : sizeof g_field;
    const bool enter = ImGui::InputText("##coop_field", g_field, cap,
                                        ImGuiInputTextFlags_EnterReturnsTrue | (join ? ImGuiInputTextFlags_CharsUppercase : 0));
    if (ImGui::IsItemEdited()) g_field_bad = g_paste_empty = false;
    if (join) {
        char text[256];
        if (g_paste_wait > 0 && menu_paste_take(text, sizeof text)) take_paste(text), g_paste_wait = 0;
        else if (g_paste_wait > 0) g_paste_wait--;
        const float bw = ImGui::CalcTextSize(menu_text(UI_MENU_COOP_PASTE)).x + ImGui::GetStyle().FramePadding.x * 4;
        if (skin_widget_button(menu_text(UI_MENU_COOP_PASTE), ImVec2(bw, 0), scale)) {
            menu_paste_take(text, sizeof text);   // an answer of an earlier Paste is not this one's
            gfx_window_paste();
            g_paste_wait = PASTE_FRAMES;
        }
        if (g_field_bad) warn(menu_text(UI_MENU_COOP_SECRET_BAD));
        if (g_paste_empty) warn(menu_text(UI_MENU_COOP_PASTE_EMPTY));
    }
    ImGui::Spacing();
    char text[FIELD_CAP];
    trimmed(text, sizeof text);
    const int r = two_buttons(menu_text(UI_MENU_RESET_NO), menu_text(join ? UI_MENU_COOP_JOIN : UI_MENU_COOP_OK),
                              !join || text[0], fresh, scale);
    if (r == 1) *back = true;
    if (r != 2 && !enter) return;
    if (g_page == P_NAME || g_page == P_SERVER) {
        opt_apply(g_page == P_NAME ? "NX2NAVI_COOP_NAME" : "NX2NAVI_COOP_SERVER", text);   // "": automatic, the default
        *close = true;
        return;
    }
    char canon[COOP_SECRET_LEN + 1];
    if (!text[0]) return;
    if (!layer_secret(text, canon)) {
        g_field_bad = true;
        return;
    }
    command(COOP_CMD_JOIN, canon);
    memset(canon, 0, sizeof canon);
    memset(g_field, 0, sizeof g_field);
    *close = true;
}

// The offer: who, what is shared, the key that signs the rules, where the save is copied, before the first Accept of
// this start the three warnings of docs/coop.md ("Before you join a room with strangers"), and Not now (the cursor) /
// Accept. A new offer while the window is open replaces the one shown and puts the cursor back on Not now: Accept takes
// only the offer the player read. While a player in the room is not identified, Accept is greyed and the window
// says why (the layer would refuse it).
static void accept_window(bool fresh, float scale, float wrap, bool *close, bool *back) {
    (void)wrap;
    if (!g_offer.token || g_offer.accepted) {   // gone, or accepted meanwhile (coop_accept)
        *close = true;
        return;
    }
    if (!layer_same_offer(g_offer, g_shown)) g_shown = g_offer, fresh = true, g_refused = false;
    gold(menu_text(UI_MENU_COOP_ACCEPT_CONFIRM));
    char who[2400], shared[320], pooled[320], line[640];
    offer_lines(g_shown, who, sizeof who, shared, sizeof shared, pooled, sizeof pooled);
    ImGui::TextUnformatted(who);
    if (shared[0]) ImGui::TextUnformatted(shared);
    if (pooled[0]) ImGui::TextUnformatted(pooled);
    if (g_shown.new_save) ImGui::TextUnformatted(menu_text(UI_MENU_COOP_NEW_SAVE));
    if (g_shown.maker_key[0] && !g_maker) {
        snprintf(line, sizeof line, menu_text(UI_MENU_COOP_OFFER_KEY), g_shown.maker_key);
        ImGui::TextUnformatted(line);
        ImGui::TextUnformatted(menu_text(UI_MENU_COOP_OFFER_COMPARE));
    }
    if (g_offer.disagree) warn(menu_text(UI_MENU_COOP_DISAGREE));
    ImGui::Spacing();
    ImGui::TextUnformatted(menu_text(UI_MENU_COOP_OFFER_COPY));
    warn(menu_text(UI_MENU_COOP_OFFER_TRUST));
    if (!g_accepted_once) {
        ImGui::Spacing();
        gold(menu_text(UI_MENU_COOP_WARNINGS));
        ImGui::TextUnformatted(menu_text(UI_MENU_COOP_WARN_SAVE));
        ImGui::TextUnformatted(menu_text(UI_MENU_COOP_WARN_ROOM));
        ImGui::TextUnformatted(menu_text(UI_MENU_COOP_WARN_KEY));
    }
    const bool can = g_offer.unidentified == 0;
    if (!can) {
        snprintf(line, sizeof line, menu_text(UI_MENU_COOP_UNIDENTIFIED), g_offer.unidentified);
        warn(line);
    }
    if (g_refused) warn(menu_text(UI_MENU_COOP_NOT_ACCEPTED));
    ImGui::Spacing();
    const int r = two_buttons(menu_text(UI_MENU_COOP_NOT_NOW), menu_text(UI_MENU_COOP_ACCEPT), can, fresh, scale);
    if (r == 1) *back = true;
    if (r == 2 && layer_accept(g_shown)) {
        g_accepted_once = true;
        snprintf(g_return, sizeof g_return, "##coop_member%d", g_view.self);   // the Accept row goes with the offer
        *close = true;
    } else if (r == 2) {
        g_refused = true;
    }
}

// Remove: by the key's fingerprint when the player has one, which every game drops (also once the player left);
// otherwise by member id, and only while the session that held the id when the window opened still holds it.
static void remove_window(bool fresh, float scale, bool *close, bool *back) {
    bool there = false;
    for (int i = 0; i < g_view.n; i++)
        there |= g_view.member[i].id == g_member && g_view.member[i].session == g_member_session;
    if (!g_maker || (!g_member_key[0] && !there)) {
        *close = true;
        return;
    }
    char text[256];
    if (g_member_key[0]) snprintf(text, sizeof text, menu_text(UI_MENU_COOP_REMOVE_KEY_CONFIRM), g_member_name, g_member_key);
    else snprintf(text, sizeof text, menu_text(UI_MENU_COOP_REMOVE_CONFIRM), g_member_name);
    gold(text);
    if (!g_member_key[0]) ImGui::TextUnformatted(menu_text(UI_MENU_COOP_NOT_IDENTIFIED));
    ImGui::TextUnformatted(menu_text(UI_MENU_COOP_REMOVE_DESC));
    ImGui::Spacing();
    const int r = two_buttons(menu_text(UI_MENU_RESET_NO), menu_text(UI_MENU_COOP_REMOVE), true, fresh, scale);
    if (r == 1) *back = true;
    if (r == 2) {
        if (g_member_key[0]) layer_remove_key(g_member_key);
        else layer_remove_member(g_member);
        snprintf(g_return, sizeof g_return, "##coop_member%d", g_view.self);
        *close = true;
    }
}

static void reset_window(bool fresh, float scale, bool *close, bool *back) {
    if (!g_maker) {
        *close = true;
        return;
    }
    gold(menu_text(UI_MENU_COOP_RESET_CONFIRM));
    ImGui::TextUnformatted(menu_text(UI_MENU_COOP_RESET_DESC));
    ImGui::Spacing();
    const int r = two_buttons(menu_text(UI_MENU_RESET_NO), menu_text(UI_MENU_COOP_RESET), true, fresh, scale);
    if (r == 1) *back = true;
    if (r == 2) {
        layer_reset_pools();
        *close = true;
    }
}

static void leave_window(bool fresh, float scale, bool *close, bool *back) {
    gold(menu_text(UI_MENU_COOP_LEAVE_CONFIRM));
    if (g_maker) warn(menu_text(UI_MENU_COOP_LEAVE_MAKER));
    ImGui::Spacing();
    const int r = two_buttons(menu_text(UI_MENU_COOP_STAY), menu_text(UI_MENU_COOP_LEAVE), true, fresh, scale);
    if (r == 1) *back = true;
    if (r == 2) {
        layer_command(COOP_CMD_LEAVE, nullptr);
        g_return[0] = 0;   // the page's rows change; coop_rows puts the cursor on the new page
        *close = true;
    }
}

void coop_after_page(float scale) {
    g_window_shown = false;
    if (g_open && !ImGui::IsPopupOpen("##coopwin")) ImGui::OpenPopup("##coopwin");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0);
    const bool shown = ImGui::BeginPopupModal("##coopwin", nullptr,
                                              ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar();
    if (!shown) {
        g_open = false;
        return;
    }
    g_window_shown = true;
    // a window opened since the last frame: latched and cleared before it is drawn (as the Saves tab's file window)
    const bool fresh = g_page_new;
    g_page_new = false;
    const ImVec2 box = ImGui::GetWindowPos(), box_size = ImGui::GetWindowSize();
    skin_panel(ImGui::GetWindowDrawList(), box, ImVec2(box.x + box_size.x, box.y + box_size.y), scale);
    const float wrap = ImGui::GetFontSize() * 30;
    bool close = false, back = false;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    const bool leaves_room = g_page == P_ACCEPT || g_page == P_REMOVE || g_page == P_RESET || g_page == P_LEAVE;
    if (leaves_room && !in_room(g_view)) close = true;   // the room ended under the window
    else if (field_page()) field_window(fresh, scale, wrap, &close, &back);
    else if (g_page == P_ACCEPT) accept_window(fresh, scale, wrap, &close, &back);
    else if (g_page == P_REMOVE) remove_window(fresh, scale, &close, &back);
    else if (g_page == P_RESET) reset_window(fresh, scale, &close, &back);
    else if (g_page == P_LEAVE) leave_window(fresh, scale, &close, &back);
    else close = true;
    ImGui::PopTextWrapPos();
    if (!fresh && !ImGui::IsAnyItemActive() &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) {
        back = true;
        menu_sound_play(MENU_SOUND_CANCEL);
    }
    if (back || close) {
        g_open = false;
        g_page = P_NONE;
        memset(g_field, 0, sizeof g_field);
        ImGui::CloseCurrentPopup();
        if (g_return[0]) opt_focus_key(g_return);   // back on the row that opened it
    }
    ImGui::EndPopup();
}

bool coop_modal(void) { return g_open || g_window_shown; }

void coop_cancel(void) {
    g_open = false;
    g_page = P_NONE;
    g_show_secret = false;
    memset(g_field, 0, sizeof g_field);
}

int coop_test_set(const char *key, const char *value) {
    if (!strcmp(key, "NX2NAVI_COOP_PAGE")) {
        layer_read(&g_view, &g_offer, g_contrib, &g_ncontrib, &g_maker);
        if (!strcmp(value, "name")) open_field(P_NAME, cfg_value("NX2NAVI_COOP_NAME"), "NX2NAVI_COOP_NAME");
        else if (!strcmp(value, "server")) open_field(P_SERVER, cfg_value("NX2NAVI_COOP_SERVER"), "NX2NAVI_COOP_SERVER");
        else if (!strcmp(value, "join")) open_field(P_JOIN, "", "##coop_join");
        else if (!strcmp(value, "accept")) open_accept();
        else if (!strcmp(value, "leave")) open_window(P_LEAVE, "##coop_leave");
        else if (!strcmp(value, "reset")) open_window(P_RESET, "##coop_reset");
        else if (!strncmp(value, "remove:", 7)) {
            const int id = atoi(value + 7);
            int i = 0;
            while (i < g_view.n && g_view.member[i].id != id) i++;
            if (i == g_view.n) return 0;
            open_remove_member(g_view.member[i]);
        } else {
            return 0;
        }
        g_by_pad = true;   // the cursor as a pad's A would leave it: on the safe button
    } else if (!strcmp(key, "NX2NAVI_COOP_JOIN")) {
        char canon[COOP_SECRET_LEN + 1];
        layer_read(&g_view, &g_offer);
        if (layer_secret(value, canon)) command(COOP_CMD_JOIN, canon);
        else g_field_bad = true;
    } else if (!strcmp(key, "NX2NAVI_COOP_ACCEPT_OFFER")) {
        CoopView v;
        CoopOffer o;
        layer_read(&v, &o);
        if (o.token && !o.accepted) layer_accept(o);
    } else {
        return 0;
    }
    return 1;
}

// ---- toasts

enum { TOASTS = 3 };
static struct {
    char text[COOP_NAME_MAX + 128];
    uint64_t until;
} g_toast[TOASTS];
static unsigned g_toast_version;
static CoopPeer g_toast_room[COOP_MAX_MEMBERS];   // the members as last seen, to tell who came and went
static int g_toast_n = -1;                         // -1: not in a room when last seen
static bool g_toast_waiting;                       // the room waited for an answer when last seen

static void toast(UiTextId fmt, const char *arg) {
    int slot = 0;
    for (int i = 1; i < TOASTS; i++)
        if (g_toast[i].until < g_toast[slot].until) slot = i;
    snprintf(g_toast[slot].text, sizeof g_toast[slot].text, menu_text(fmt), arg);
    g_toast[slot].until = plat_now_ms() + 5000;
}

static bool has(const CoopPeer *list, int n, int id) {
    for (int i = 0; i < n; i++)
        if (list[i].id == id) return true;
    return false;
}

bool coop_toast_tick(void) {
    static unsigned cfg_seen = ~0u;
    static bool on;
    if (unsigned v = cfg_version(); v != cfg_seen) cfg_seen = v, on = cfg_bool("NX2NAVI_COOP") != 0;
    const uint64_t now = plat_now_ms();
    if (on) {
        CoopView v;
        CoopOffer o;
        layer_read(&v, &o);
        if (v.version != g_toast_version) {
            g_toast_version = v.version;
            const int n = in_room(v) ? v.n : -1;
            if (n >= 0 && g_toast_n >= 0 && !menu_is_open()) {   // only changes within a room, not its first roster
                for (int i = 0; i < v.n; i++)
                    if (v.member[i].id != v.self && !has(g_toast_room, g_toast_n, v.member[i].id))
                        toast(UI_MENU_COOP_TOAST_JOINED, v.member[i].name);
                for (int i = 0; i < g_toast_n; i++)
                    if (g_toast_room[i].id != v.self && !has(v.member, v.n, g_toast_room[i].id))
                        toast(UI_MENU_COOP_TOAST_LEFT, g_toast_room[i].name);
            }
            g_toast_n = n;
            if (n > 0) memcpy(g_toast_room, v.member, sizeof(CoopPeer) * (size_t)n);
        }
        // a room that waits for an answer again (a new player, other rules): once per wait, not per offer
        const bool waiting = o.token && !o.accepted;
        if (waiting && !g_toast_waiting && !menu_is_open()) toast(UI_MENU_COOP_TOAST_OFFER, "");
        g_toast_waiting = waiting;
    }
    bool any = false;
    for (int i = 0; i < TOASTS; i++) any |= g_toast[i].until > now;
    return any;
}

void coop_toast_draw(float w, float h, float scale) {
    (void)h;
    const uint64_t now = plat_now_ms();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const float padx = 14 * scale, pady = 9 * scale;
    float y = 12 * scale;
    for (int i = 0; i < TOASTS; i++) {
        if (g_toast[i].until <= now) continue;
        // the top middle: the game's hearts and rupees are at the left, the performance overlay at the right; the same
        // small panel as that overlay
        const ImVec2 sz = ImGui::CalcTextSize(g_toast[i].text);
        const ImVec2 p(w * 0.5f - sz.x * 0.5f - padx, y);
        skin_panel(dl, p, ImVec2(p.x + sz.x + padx * 2, p.y + sz.y + pady * 2), scale * 0.6f, 0.9f);
        dl->AddText(ImVec2(p.x + padx, p.y + pady), SKIN_TEXT, g_toast[i].text);
        y += sz.y + pady * 2 + 6 * scale;
    }
}
