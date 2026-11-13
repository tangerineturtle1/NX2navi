// The menu's Co-op tab and co-op's drawing over the game (ui/coop_page.h).
//
// The page reads the co-op thread's view once a frame (coop_view) and hands it commands (coop_create, coop_join, ...);
// nothing here waits for Steam. What the page shows follows the view's state: off, Steam missing, in no room, on the
// way into one, in one. A press that turns co-op on and wants a room at once (Create a room while co-op is off) is
// kept for a few seconds until the thread has reached Steam.
//
// Names over the game: a player's position is projected with the frame's camera as the upscaler's pass knows it
// (vk_dlaa_fg_camera: position, right, up and forward in the world's metres, the vertical field of view and the
// aspect). Without a temporal upscaler there is no camera and no names; the player list still says where each is.
#include <math.h>
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
#include "platform/platform.h"
#include "ui/menu.h"
#include "ui_text.h"
#include "vk_dlaa.h"
}

enum Page { P_NONE, P_JOIN, P_CHAT, P_INVITE, P_REMOVE, P_LEAVE };
enum { FIELD_CAP = COOP_CHAT_CAP + 1, PASTE_FRAMES = 120, PENDING_MS = 8000, CHAT_SHOWN_MS = 8000, CHAT_FADE_MS = 1000 };

static bool g_open, g_window_shown, g_page_new, g_by_pad;
static int g_page;
static char g_field[FIELD_CAP];
static bool g_field_bad, g_paste_empty;
static int g_paste_wait;                 // frames left to wait for the clipboard's text (gfx_window_paste)
static char g_return[48];                // the row the cursor goes back to when the window closes
static CoopView g_view;                  // this frame's copy of the layer's state
static int g_layout = -1;                // the page's last kind, to move the cursor when its rows change
static uint64_t g_copied_at;             // when the room ID was last copied
static uint64_t g_remove_id;
static char g_remove_name[COOP_NAME_CAP];
static char g_invited_name[COOP_NAME_CAP];   // the friend the last invite went to
static uint64_t g_invited_at;
static SteamFriend g_friends[COOP_FRIENDS];
static int g_nfriends;
// a command that waits for co-op to come up: 1 create, 2 join g_pending_code
static int g_pending;
static char g_pending_code[STEAM_CODE_CAP];
static uint64_t g_pending_until;

static bool in_room(void) { return g_view.state == COOP_IN_ROOM; }

static UiTextId note_text(CoopNote n) {
    switch (n) {
    case COOP_NOTE_NO_LIBRARY: return UI_MENU_COOP_NO_LIBRARY;
    case COOP_NOTE_OLD_LIBRARY: return UI_MENU_COOP_OLD_LIBRARY;
    case COOP_NOTE_NO_CLIENT:
    case COOP_NOTE_OTHER_OS: return UI_MENU_COOP_NO_CLIENT;
    case COOP_NOTE_CREATE_FAILED: return UI_MENU_COOP_N_CREATE_FAILED;
    case COOP_NOTE_NO_ROOM:
    case COOP_NOTE_BAD_CODE: return UI_MENU_COOP_N_NO_ROOM;
    case COOP_NOTE_FULL: return UI_MENU_COOP_N_FULL;
    case COOP_NOTE_REFUSED: return UI_MENU_COOP_N_REFUSED;
    case COOP_NOTE_NOT_OURS: return UI_MENU_COOP_N_NOT_OURS;
    case COOP_NOTE_OTHER_VERSION: return UI_MENU_COOP_N_OTHER_VERSION;
    case COOP_NOTE_REMOVED: return UI_MENU_COOP_N_REMOVED;
    case COOP_NOTE_HOST_SILENT: return UI_MENU_COOP_N_HOST_SILENT;
    case COOP_NOTE_LOST: return UI_MENU_COOP_N_LOST;
    case COOP_NOTE_LEFT: return UI_MENU_COOP_N_LEFT;
    default: return UI_TEXT_COUNT;
    }
}

// Create or join, turning co-op on first when it is off: the command then waits for the thread to reach Steam.
static void want_room(int what, const char *code) {
    if (g_view.state == COOP_READY) {
        if (what == 1) coop_create();
        else coop_join(code);
        return;
    }
    if (g_view.state == COOP_OFF) opt_apply("NX2NAVI_COOP_STEAM", "1");
    g_pending = what;
    snprintf(g_pending_code, sizeof g_pending_code, "%s", code ? code : "");
    g_pending_until = plat_now_ms() + PENDING_MS;
}

static void pending_tick(void) {
    if (!g_pending) return;
    if (g_view.state == COOP_READY) {
        if (g_pending == 1) coop_create();
        else coop_join(g_pending_code);
        g_pending = 0;
    } else if (g_view.state == COOP_NO_STEAM || plat_now_ms() > g_pending_until) {
        g_pending = 0;   // the page says why Steam is not there
    }
}

static void open_window(int page, const char *ret) {
    g_open = true, g_page = page, g_page_new = true;
    g_by_pad = ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown);   // a pad's A opened it: no keyboard to type with
    g_field_bad = g_paste_empty = false, g_paste_wait = 0;
    g_field[0] = 0;
    snprintf(g_return, sizeof g_return, "%s", ret);
}

static void settings_rows(void) {
    opt_group(UI_MENU_COOP_SETTINGS);
    opt_toggle("NX2NAVI_COOP_SHARE", UI_MENU_COOP_SHARE, UI_MENU_COOP_SHARE_DESC);
    opt_toggle("NX2NAVI_COOP_NAMES", UI_MENU_COOP_NAMES, UI_MENU_COOP_NAMES_DESC);
    opt_toggle("NX2NAVI_COOP_CHAT", UI_MENU_COOP_CHAT_SHOW, UI_MENU_COOP_CHAT_SHOW_DESC);
    opt_toggle("NX2NAVI_COOP_STEAM", UI_MENU_COOP_ON, UI_MENU_COOP_ON_DESC);
}

static void chat_text(const CoopChatLine &l, char *out, size_t cap) {
    if (l.notice) snprintf(out, cap, menu_text(l.joined ? UI_MENU_COOP_CHAT_JOINED : UI_MENU_COOP_CHAT_LEFT), l.name);
    else snprintf(out, cap, "%s: %s", l.name, l.text);
}

static void rows_out(void) {
    if (g_view.state == COOP_OFF && !g_pending) {
        opt_toggle("NX2NAVI_COOP_STEAM", UI_MENU_COOP_ON, UI_MENU_COOP_ON_DESC);
        return;
    }
    if (g_view.local) opt_text(UI_MENU_COOP_LOCAL);
    if (g_view.state == COOP_NO_STEAM) {
        const UiTextId why = note_text(g_view.note);
        if (why != UI_TEXT_COUNT) opt_text(why, true);
        if (opt_button("##coop_retry", UI_MENU_COOP_RETRY, UI_MENU_COOP_RETRY_DESC)) coop_retry();
        settings_rows();
        return;
    }
    if (g_view.state == COOP_ENTERING || g_pending) {
        if (opt_button("##coop_stop", UI_MENU_COOP_STOP, UI_MENU_COOP_STOP_DESC)) {
            g_pending = 0;
            coop_leave();
        }
        opt_text(UI_MENU_COOP_JOINING);
        return;
    }
    if (g_view.invite_room) {
        char line[COOP_NAME_CAP + 96];
        snprintf(line, sizeof line, menu_text(UI_MENU_COOP_INVITED), g_view.invite_from);
        opt_text_str(line);
        if (opt_button("##coop_invited_join", UI_MENU_COOP_INVITED_JOIN, UI_MENU_COOP_INVITED_JOIN_DESC)) coop_join_invite();
        if (opt_button("##coop_invited_no", UI_MENU_COOP_INVITED_NO, UI_MENU_COOP_INVITED_NO_DESC)) coop_decline_invite();
    }
    if (opt_button("##coop_create", UI_MENU_COOP_CREATE, UI_MENU_COOP_CREATE_DESC)) want_room(1, nullptr);
    if (opt_button("##coop_join", UI_MENU_COOP_JOIN, UI_MENU_COOP_JOIN_DESC)) open_window(P_JOIN, "##coop_join");
    const UiTextId why = note_text(g_view.note);
    if (why != UI_TEXT_COUNT) opt_text(why, g_view.note != COOP_NOTE_LEFT);
    settings_rows();
}

static void rows_room(void) {
    const uint64_t now = plat_now_ms();
    if (g_view.local) opt_text(UI_MENU_COOP_LOCAL);
    if (g_view.code[0]) {
        const bool copied = g_copied_at && now - g_copied_at < 3000;
        if (opt_text_row("##coop_code", menu_text(UI_MENU_COOP_ROOM_ID), g_view.code, copied ? menu_text(UI_MENU_COOP_COPIED) : nullptr,
                         menu_text(UI_MENU_COOP_ROOM_ID_DESC), false) == 2 &&
            plat_clipboard_set(g_view.code))
            g_copied_at = now;
    }
    if (!g_view.local && opt_button("##coop_invite", UI_MENU_COOP_INVITE, UI_MENU_COOP_INVITE_DESC)) {
        coop_friends_refresh();
        open_window(P_INVITE, "##coop_invite");
    }
    if (g_invited_at && now - g_invited_at < 5000) {
        char line[COOP_NAME_CAP + 64];
        snprintf(line, sizeof line, menu_text(UI_MENU_COOP_INVITE_SENT), g_invited_name);
        opt_text_str(line);
    }
    opt_group(UI_MENU_COOP_PLAYERS);
    for (int i = 0; i < g_view.n; i++) {
        const CoopPlayer &p = g_view.players[i];
        char id[40], note[96], where[COOP_SCENE_CAP + 16];
        snprintf(id, sizeof id, "##coop_player%d", i);
        snprintf(note, sizeof note, "%s%s%s", p.self ? menu_text(UI_MENU_COOP_YOU) : "", p.self && p.host ? ", " : "",
                 p.host ? menu_text(UI_MENU_COOP_HOST) : "");
        const bool here = p.self ? false : p.has_pose && p.pose.scene[0];
        snprintf(where, sizeof where, "%s", p.self ? "" : here ? p.pose.scene : menu_text(UI_MENU_COOP_PLAYER_AWAY));
        const bool removable = g_view.host && !p.self;
        if (opt_text_row(id, p.name[0] ? p.name : "…", where, note[0] ? note : nullptr,
                         menu_text(removable ? UI_MENU_COOP_PLAYER_REMOVE_DESC : UI_MENU_COOP_PLAYER_DESC), false) == 2 &&
            removable) {
            g_remove_id = p.id;
            snprintf(g_remove_name, sizeof g_remove_name, "%s", p.name);
            open_window(P_REMOVE, id);
        }
    }
    opt_group(UI_MENU_COOP_CHAT);
    for (int i = 0; i < g_view.nchat; i++) {
        char line[COOP_NAME_CAP + COOP_CHAT_CAP + 8];
        chat_text(g_view.chat[i], line, sizeof line);
        opt_text_str(line);
    }
    if (opt_button("##coop_chat", UI_MENU_COOP_CHAT_WRITE, UI_MENU_COOP_CHAT_WRITE_DESC)) open_window(P_CHAT, "##coop_chat");
    if (opt_button("##coop_leave", UI_MENU_COOP_LEAVE, UI_MENU_COOP_LEAVE_DESC)) open_window(P_LEAVE, "##coop_leave");
    settings_rows();
}

void coop_rows(float scale) {
    (void)scale;
    coop_view(&g_view);
    pending_tick();
    // the page changes its rows when the player gets into a room, reaches for one or leaves it: the cursor goes to a row
    // of the new page whose press changes nothing at once
    const int layout = in_room() ? 2 : g_view.state == COOP_ENTERING || g_pending ? 1 : 0;
    if (layout != g_layout) {
        if (g_layout >= 0) opt_focus_key(layout == 2 ? "##coop_code" : layout == 1 ? "##coop_stop" : "##coop_join");
        g_layout = layout;
    }
    if (layout == 2) rows_room();
    else rows_out();
}

// ---- the window

// The cursor on the item just drawn, shown (as option_rows.cpp puts it on a row). SetItemDefaultFocus as well: on the
// window's first frame ImGui's own pick of a first item would replace the cursor set here.
static void take_cursor(void) {
    ImGui::SetItemDefaultFocus();
    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
    ImGui::SetNavCursorVisible(true);
}

static bool field_page(void) { return g_page == P_JOIN || g_page == P_CHAT; }

// Two buttons side by side, the safe one first: 1 the first, 2 the second. The cursor goes to the safe one when the
// window opens (fresh); in a field's window typed by keyboard the field has the cursor instead. ImGui activates a
// button on a press, not on a held key's repeats.
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

// A pasted room ID: the characters one is typed with, no line ends or tabs.
static void take_paste(const char *text) {
    char out[32];
    size_t k = 0;
    for (const char *p = text; *p && k + 1 < sizeof out; p++)
        if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '-')
            out[k++] = *p >= 'a' && *p <= 'z' ? (char)(*p - 'a' + 'A') : *p;
    out[k] = 0;
    g_paste_empty = !k;
    if (k) snprintf(g_field, sizeof g_field, "%s", out), g_field_bad = false;
}

static void trimmed(char *out, size_t cap) {
    const char *s = g_field;
    while (*s == ' ') s++;
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') n--;
    snprintf(out, cap, "%.*s", (int)n, s);
}

// The field's window: a room ID (Cancel, Join; Paste) or a chat line (Cancel, Send).
static void field_window(bool fresh, float scale, float wrap, bool *close, bool *back) {
    const bool join = g_page == P_JOIN;
    gold(menu_text(join ? UI_MENU_COOP_ROOM_ID : UI_MENU_COOP_CHAT_WRITE));
    if (join) ImGui::TextDisabled("%s", menu_text(UI_MENU_COOP_ROOM_ID_HINT));
    if (fresh && !g_by_pad) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(wrap);
    const bool enter = ImGui::InputText("##coop_field", g_field, join ? 24 : sizeof g_field,
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
        if (g_field_bad) warn(menu_text(UI_MENU_COOP_ROOM_ID_BAD));
        if (g_paste_empty) warn(menu_text(UI_MENU_COOP_PASTE_EMPTY));
    }
    ImGui::Spacing();
    char text[FIELD_CAP];
    trimmed(text, sizeof text);
    const int r = two_buttons(menu_text(UI_MENU_RESET_NO), menu_text(join ? UI_MENU_COOP_JOIN : UI_MENU_COOP_SEND), text[0] != 0, fresh, scale);
    if (r == 1) *back = true;
    if ((r != 2 && !enter) || !text[0]) return;
    if (join) {
        if (!steam_code_parse(text)) {
            g_field_bad = true;
            return;
        }
        want_room(2, text);
    } else {
        coop_chat(text);
    }
    *close = true;
}

// The player's friends, each a button that sends the invite; Steam's own window when the game was started from Steam.
static void invite_window(bool fresh, float scale, bool *close, bool *back) {
    static uint32_t serial;
    if (fresh || serial != g_view.friends_serial) serial = g_view.friends_serial, g_nfriends = coop_friends(g_friends, COOP_FRIENDS);
    gold(menu_text(UI_MENU_COOP_INVITE_TITLE));
    const float bw = ImGui::GetFontSize() * 22;
    if (skin_widget_button(menu_text(UI_MENU_COOP_INVITE_CLOSE), ImVec2(bw, 0), scale)) *back = true;
    if (fresh) take_cursor();
    if (g_view.overlay && skin_widget_button(menu_text(UI_MENU_COOP_INVITE_STEAM), ImVec2(bw, 0), scale)) {
        coop_invite_window();
        *close = true;
    }
    if (!g_nfriends) ImGui::TextDisabled("%s", menu_text(UI_MENU_COOP_INVITE_NONE));
    ImGui::BeginChild("##coop_friends", ImVec2(bw + ImGui::GetStyle().ScrollbarSize + skin_cursor_reach(scale) * 2, ImGui::GetFontSize() * 14),
                      ImGuiChildFlags_NavFlattened);
    for (int i = 0; i < g_nfriends; i++) {
        const SteamFriend &f = g_friends[i];
        char label[STEAM_NAME_CAP + 64];
        snprintf(label, sizeof label, "%s  (%s)##f%d", f.name, menu_text(f.in_app ? UI_MENU_COOP_INVITE_IN_GAME : f.online ? UI_MENU_COOP_INVITE_ONLINE
                                                                                                                             : UI_MENU_COOP_INVITE_OFFLINE), i);
        if (skin_widget_button(label, ImVec2(bw, 0), scale)) {
            coop_invite(f.id);
            snprintf(g_invited_name, sizeof g_invited_name, "%s", f.name);
            g_invited_at = plat_now_ms();
            *close = true;
        }
    }
    ImGui::EndChild();
}

static void confirm_window(bool fresh, float scale, bool *close, bool *back) {
    const bool remove = g_page == P_REMOVE;
    char line[COOP_NAME_CAP + 160];
    if (remove) snprintf(line, sizeof line, menu_text(UI_MENU_COOP_REMOVE_CONFIRM), g_remove_name);
    else snprintf(line, sizeof line, "%s", menu_text(UI_MENU_COOP_LEAVE_CONFIRM));
    gold(line);
    if (!remove && g_view.host && g_view.n > 1) ImGui::TextUnformatted(menu_text(UI_MENU_COOP_LEAVE_HOST));
    ImGui::Spacing();
    const int r = two_buttons(menu_text(remove ? UI_MENU_RESET_NO : UI_MENU_COOP_STAY), menu_text(remove ? UI_MENU_COOP_REMOVE : UI_MENU_COOP_LEAVE),
                              true, fresh, scale);
    if (r == 1) *back = true;
    if (r == 2) {
        if (remove) coop_remove(g_remove_id);
        else coop_leave(), g_return[0] = 0;   // the page's rows change; coop_rows puts the cursor on the new page
        *close = true;
    }
}

void coop_after_page(float scale) {
    g_window_shown = false;
    if (g_open && !ImGui::IsPopupOpen("##coopwin")) ImGui::OpenPopup("##coopwin");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0);
    const bool shown = ImGui::BeginPopupModal("##coopwin", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize);
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
    const bool needs_room = g_page == P_CHAT || g_page == P_INVITE || g_page == P_REMOVE || g_page == P_LEAVE;
    if (needs_room && !in_room()) close = true;   // the room ended under the window
    else if (field_page()) field_window(fresh, scale, wrap, &close, &back);
    else if (g_page == P_INVITE) invite_window(fresh, scale, &close, &back);
    else if (g_page == P_REMOVE || g_page == P_LEAVE) confirm_window(fresh, scale, &close, &back);
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
        g_field[0] = 0;
        ImGui::CloseCurrentPopup();
        if (g_return[0]) opt_focus_key(g_return);   // back on the row that opened it
    }
    ImGui::EndPopup();
}

bool coop_modal(void) { return g_open || g_window_shown; }

void coop_cancel(void) {
    g_open = false;
    g_page = P_NONE;
    g_field[0] = 0;
}

int coop_test_set(const char *key, const char *value) {
    if (strcmp(key, "NX2NAVI_COOP_DO")) return 0;
    coop_view(&g_view);
    if (!strcmp(value, "create")) want_room(1, nullptr);
    else if (!strcmp(value, "leave")) coop_leave();
    else if (!strncmp(value, "join:", 5)) want_room(2, value + 5);
    else if (!strncmp(value, "chat:", 5)) coop_chat(value + 5);
    else if (!strcmp(value, "page:join")) open_window(P_JOIN, "##coop_join");
    else if (!strcmp(value, "page:invite")) coop_friends_refresh(), open_window(P_INVITE, "##coop_invite");
    else if (!strcmp(value, "page:chat")) open_window(P_CHAT, "##coop_chat");
    else if (!strcmp(value, "page:leave")) open_window(P_LEAVE, "##coop_leave");
    g_by_pad = true;   // the cursor as a pad's A would leave it: on the safe button
    return 1;
}

// ---- over the game

static CoopView g_over;
static bool g_over_on;

bool coop_overlay_tick(void) {
    static unsigned cfg_seen = ~0u;
    static uint32_t serial;
    if (unsigned v = cfg_version(); v != cfg_seen) cfg_seen = v, g_over_on = cfg_bool("NX2NAVI_COOP_STEAM") != 0;
    if (!g_over_on) return false;
    if (uint32_t s = coop_serial(); s != serial) serial = s, coop_view(&g_over);
    if (g_over.invite_room) return true;
    if (g_over.state != COOP_IN_ROOM) return false;
    if (g_over.n > 1) return true;
    const uint64_t now = plat_now_ms();
    return g_over.nchat && now - g_over.chat[g_over.nchat - 1].at_ms < CHAT_SHOWN_MS;
}

// The label of a small panel centred on x, its top at y.
static float label(ImDrawList *dl, float x, float y, const char *text, float scale, float alpha) {
    const float padx = 10 * scale, pady = 5 * scale;
    const ImVec2 sz = ImGui::CalcTextSize(text);
    const ImVec2 p(x - sz.x * 0.5f - padx, y);
    skin_panel(dl, p, ImVec2(p.x + sz.x + padx * 2, p.y + sz.y + pady * 2), scale * 0.5f, 0.85f * alpha);
    dl->AddText(ImVec2(p.x + padx, p.y + pady), IM_COL32(244, 236, 214, (int)(255 * alpha)), text);
    return sz.y + pady * 2;
}

void coop_overlay_draw(float w, float h, float scale, bool menu_open) {
    if (!g_over_on) return;
    const uint64_t now = plat_now_ms();
    ImDrawList *dl = menu_open ? ImGui::GetBackgroundDrawList() : ImGui::GetForegroundDrawList();
    // the other players' names where their Link stands, in the area this game's Link is in
    VkDlaaCamera cam;
    const CoopPlayer *me = nullptr;
    for (int i = 0; i < g_over.n; i++)
        if (g_over.players[i].self) me = &g_over.players[i];
    if (!menu_open && g_over.state == COOP_IN_ROOM && cfg_bool("NX2NAVI_COOP_NAMES") && vk_dlaa_fg_camera(&cam)) {
        const float ty = tanf(cam.fov_y * 0.5f), tx = ty * cam.aspect;
        // the game's picture in the frame: centred, with bars above and below or at the sides when its shape differs
        const float pw = cam.aspect > w / h ? w : h * cam.aspect, ph = cam.aspect > w / h ? w / cam.aspect : h;
        for (int i = 0; i < g_over.n; i++) {
            const CoopPlayer &p = g_over.players[i];
            if (p.self || !p.has_pose || !p.pose.scene[0]) continue;
            // only a player in the area this game's Link is in
            if (me && me->has_pose && me->pose.scene[0] && strcmp(me->pose.scene, p.pose.scene)) continue;
            // a moment ahead along the velocity: the pose is up to a twentieth of a second old
            float age = (float)p.pose_ms / 1000.0f;
            if (age > 0.25f) age = 0.25f;
            float d[3];
            for (int k = 0; k < 3; k++) d[k] = p.pose.pos[k] + p.pose.vel[k] * age - cam.pos[k];
            d[1] += 1.9f;   // above the head of an adult Link; a child's name floats a little higher
            const float z = d[0] * cam.fwd[0] + d[1] * cam.fwd[1] + d[2] * cam.fwd[2];
            float x = d[0] * cam.right[0] + d[1] * cam.right[1] + d[2] * cam.right[2];
            float y = d[0] * cam.up[0] + d[1] * cam.up[1] + d[2] * cam.up[2];
            const float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            char text[COOP_NAME_CAP + 24];
            snprintf(text, sizeof text, "%s  %.0f m", p.name[0] ? p.name : "?", dist);
            float nx, ny;
            bool on_screen = z > 0.1f;
            if (on_screen) {
                nx = x / (z * tx), ny = y / (z * ty);
                on_screen = fabsf(nx) < 0.96f && fabsf(ny) < 0.92f;
            }
            if (!on_screen) {
                // beside or behind the picture: at its edge, in the direction to turn to
                float len = sqrtf(x * x + y * y);
                if (len < 1e-4f) x = 1, y = 0, len = 1;
                nx = x / len, ny = y / len;
                const float m = fabsf(nx) / 0.96f > fabsf(ny) / 0.88f ? fabsf(nx) / 0.96f : fabsf(ny) / 0.88f;
                nx /= m, ny /= m;
            }
            label(dl, w * 0.5f + nx * 0.5f * pw, h * 0.5f - ny * 0.5f * ph, text, scale, on_screen ? 1.0f : 0.75f);
        }
    }
    if (menu_open) return;
    float y = 12 * scale;
    if (g_over.invite_room) {
        char text[COOP_NAME_CAP + 160];
        snprintf(text, sizeof text, menu_text(UI_MENU_COOP_TOAST_INVITE), g_over.invite_from);
        y += label(dl, w * 0.5f, y, text, scale * 1.2f, 1) + 6 * scale;
    }
    if (g_over.state == COOP_IN_ROOM && cfg_bool("NX2NAVI_COOP_CHAT")) {
        // the top middle: the game's hearts and rupees are at the left, the performance overlay at the right
        for (int i = 0; i < g_over.nchat; i++) {
            const uint64_t age = now - g_over.chat[i].at_ms;
            if (age >= CHAT_SHOWN_MS) continue;
            const float alpha = age > CHAT_SHOWN_MS - CHAT_FADE_MS ? (float)(CHAT_SHOWN_MS - age) / CHAT_FADE_MS : 1.0f;
            char text[COOP_NAME_CAP + COOP_CHAT_CAP + 8];
            chat_text(g_over.chat[i], text, sizeof text);
            y += label(dl, w * 0.5f, y, text, scale * 1.2f, alpha) + 6 * scale;
        }
    }
}
