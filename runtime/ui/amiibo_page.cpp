// Amiibo / NFC menu page (Dear ImGui). The tab: the dumps of the amiibo folder on the left (A or a click taps one), the
// reader and the figure on it on the right, and under "Advanced" the folder, the reader's options, the figure's details,
// the keys file and write-back. The picker: the same list on its own over the game, opened when the game starts waiting
// for an amiibo (ui/menu.cpp, NX2NAVI_AMIIBO_POPUP).
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <filesystem>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <string_view>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"   // SetFocusID, ScrollToItem
#include "ui/amiibo_page.h"
#include "ui/menu_skin.h"
#include "ui/option_rows.h"

extern "C" {
#include "amiibo.h"
#include "amiibo_crypto.h"
#include "cfg.h"
#include "folder_pick.h"
#include "platform/platform.h"
#include "ui_text.h"
}

namespace fs = std::filesystem;

static const ImVec4 kGold(0.93f, 0.76f, 0.33f, 1.0f);
static const ImVec4 kRed(0.9f, 0.3f, 0.3f, 1.0f);
static const ImVec4 kGreen(0.3f, 0.9f, 0.3f, 1.0f);
static const ImVec4 kCyan(0.2f, 0.9f, 1.0f, 1.0f);

// ---- the dumps of the amiibo folder and its subfolders, as one list
struct Dump {
    std::string path;    // as amiibo_load_file takes it, and as AmiiboInfo.path then holds it
    std::string label;   // the path under the amiibo folder without the extension: "Young Link", "Zelda/Young Link"
    std::string key;     // label in lower case, for sorting and the search field
};

static const char kDefaultAmiiboDir[] = "./amiibo";
static const int kMaxDepth = 3;        // subfolders listed below the amiibo folder
static const size_t kMaxDumps = 2000;
static std::string g_root = kDefaultAmiiboDir;   // the folder read last
static std::vector<Dump> g_dumps;
static bool g_scanned, g_root_missing;
static char g_filter[64];

static std::string lower(std::string_view s) {
    std::string out(s);
    for (char &c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

static bool is_dump(const fs::directory_entry &e) {
    std::error_code ec;
    if (!e.is_regular_file(ec) || ec) return false;
    std::string ext = lower(e.path().extension().string());
    if (ext != ".bin" && ext != ".nfc") return false;
    // amiibo_load_file takes whole 540-byte dumps only, so other files of that name (the keys file key_retail.bin among
    // them) stay out of the list
    return e.file_size(ec) == (uintmax_t)AMIIBO_DUMP_SIZE && !ec;
}

// Reads the amiibo folder (NX2NAVI_AMIIBO_DIR, else ./amiibo) again. A missing folder lists nothing and says so.
static void scan_dumps(void) {
    const char *dir = cfg_get("NX2NAVI_AMIIBO_DIR");
    g_root = dir && *dir ? dir : kDefaultAmiiboDir;
    g_dumps.clear();
    g_scanned = true;
    std::error_code ec;
    const fs::path root(g_root);
    g_root_missing = !fs::is_directory(root, ec);
    if (g_root_missing) return;
    try {   // a name the narrow encoding cannot hold throws from string(); the dumps read before it stay listed
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end && g_dumps.size() < kMaxDumps; it.increment(ec)) {
            if (it.depth() >= kMaxDepth - 1) it.disable_recursion_pending();
            if (!is_dump(*it)) continue;
            fs::path rel = it->path().lexically_relative(root);
            rel.replace_extension();
            Dump d{it->path().string(), rel.generic_string(), {}};
            d.key = lower(d.label);
            g_dumps.push_back(std::move(d));
        }
    } catch (...) {
    }
    std::sort(g_dumps.begin(), g_dumps.end(), [](const Dump &a, const Dump &b) { return a.key < b.key; });
}

// The folder a player set: saved like a row's change; "" goes back to the default.
static void set_root(const std::string &path) {
    opt_apply("NX2NAVI_AMIIBO_DIR", path.c_str());
    scan_dumps();
}

// ---- the last action: a catalog id and the figure or file it names, formatted when drawn (the language can change)
static UiTextId g_status_id = UI_MENU_AMIIBO_READY;
static std::string g_status_arg;

static void set_status(UiTextId id, const std::string &arg = std::string()) {
    g_status_id = id;
    g_status_arg = arg;
}

// Erase game data asks twice: the first press arms the button, a second one within kEraseConfirmS seconds erases.
static const double kEraseConfirmS = 3.0;
static double g_erase_armed_at = -1.0;   // ImGui::GetTime() of the first press; negative while not armed

// Loads a dump and puts it on the reader. True when it is on the reader.
static bool tap(const Dump &d) {
    g_erase_armed_at = -1.0;   // a press armed for the previous figure does not erase this one
    const bool ok = amiibo_scan_and_tap(d.path.c_str());
    set_status(ok ? UI_MENU_AMIIBO_TAPPED : UI_MENU_AMIIBO_LOAD_FAILED, d.label);
    return ok;
}

// The loaded figure's name as the list shows it: its file name without the extension.
static std::string figure_name(const AmiiboInfo *info) {
    std::string name = info->filename;
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name.resize(dot);
    return name;
}

// A catalog format and its arguments, wrapped to the pane (a path or a long translation does not fit one line).
static void text_wrapped_fmt(const char *fmt, ...) {
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ImGui::TextWrapped("%s", buf);
}

static void text_disabled_wrapped(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// The list is empty: the folder is missing or holds no dumps.
static void empty_note(void) {
    char note[768];
    snprintf(note, sizeof note, menu_text(g_root_missing ? UI_MENU_AMIIBO_NO_DIR : UI_MENU_AMIIBO_NONE_FOUND),
             g_root.c_str());
    text_disabled_wrapped(note);
}

// The dumps as one selectable line each; A, Enter or a click taps one. The loaded figure is marked: a cyan dot while it
// is on the reader, a gold one after it left. focus: the cursor goes to the loaded figure, else the first line.
// filter: only the lines holding g_filter. Returns the dump tapped this frame, or NULL.
static const Dump *dump_list(bool focus, bool filter) {
    const Dump *tapped = nullptr;
    const AmiiboInfo *info = amiibo_get_current_info();
    AmiiboFigureData fd;
    const bool loaded_any = amiibo_get_figure_data(&fd);
    const bool on_reader = amiibo_has_tag();
    const std::string needle = filter ? lower(g_filter) : std::string();

    int focus_at = -1, first = -1;
    for (size_t i = 0; i < g_dumps.size(); i++) {
        if (!needle.empty() && g_dumps[i].key.find(needle) == std::string::npos) continue;
        if (first < 0) first = (int)i;
        if (focus && loaded_any && g_dumps[i].path == info->path) focus_at = (int)i;
    }
    if (focus && focus_at < 0) focus_at = first;

    const float line_h = ImGui::GetTextLineHeight();
    const float indent = line_h * 1.1f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < g_dumps.size(); i++) {
        const Dump &d = g_dumps[i];
        if (!needle.empty() && d.key.find(needle) == std::string::npos) continue;
        const bool loaded = loaded_any && d.path == info->path;
        ImGui::PushID((int)i);
        // the label is drawn by hand: a "#" in a file name would otherwise be read as part of the item's id
        if (ImGui::Selectable("##dump", loaded, ImGuiSelectableFlags_None, ImVec2(0, line_h))) {
            if (tap(d)) tapped = &d;
        }
        const ImVec2 at = ImGui::GetItemRectMin();
        if (loaded) {
            dl->AddCircleFilled(ImVec2(at.x + indent * 0.45f, at.y + line_h * 0.5f), line_h * 0.22f,
                                ImGui::GetColorU32(on_reader ? kCyan : kGold));
        }
        dl->AddText(ImVec2(at.x + indent, at.y), ImGui::GetColorU32(loaded ? (on_reader ? kCyan : kGold)
                                                                           : ImGui::GetStyleColorVec4(ImGuiCol_Text)),
                    d.label.c_str());
        if ((int)i == focus_at) {
            ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
            ImGui::SetNavCursorVisible(true);
            ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleCenterY);
        }
        ImGui::PopID();
    }
    return tapped;
}

// ---- the folder dialog (folder_pick.h) answers on its own thread; the page takes the answer when it draws
static PlatMutex g_pick_lock = PLAT_MUTEX_INIT;
static bool g_pick_open;
static bool g_pick_ready;
static std::string g_pick_path;

static void on_folder_picked(const char *path, void *ctx) {
    (void)ctx;
    plat_mutex_lock(&g_pick_lock);
    g_pick_ready = path != NULL;
    if (path) g_pick_path = path;
    g_pick_open = false;
    plat_mutex_unlock(&g_pick_lock);
}

// ---- the figure on the right: its name, whether it is on the reader, its nickname and game data, Lift
static void draw_figure(void) {
    AmiiboFigureData fd;
    if (!amiibo_get_figure_data(&fd)) {
        text_disabled_wrapped(menu_text(UI_MENU_AMIIBO_NO_TAG));
        return;
    }
    const AmiiboInfo *info = amiibo_get_current_info();
    const bool on_reader = amiibo_has_tag();
    ImGui::TextColored(on_reader ? kCyan : kGold, "%s", figure_name(info).c_str());
    if (!on_reader) text_disabled_wrapped(menu_text(UI_MENU_AMIIBO_NO_TAG));
    fd.nickname[sizeof fd.nickname - 1] = 0;
    if (fd.registered) text_wrapped_fmt(menu_text(UI_MENU_AMIIBO_NICKNAME), fd.nickname);
    else ImGui::TextWrapped("%s", menu_text(UI_MENU_AMIIBO_NOT_REGISTERED));
    ImGui::TextWrapped("%s", menu_text(fd.has_app_area ? UI_MENU_AMIIBO_HAS_GAME_DATA : UI_MENU_AMIIBO_NO_GAME_DATA));
    ImGui::Spacing();
    ImGui::BeginDisabled(!on_reader);
    if (ImGui::Button(menu_text(UI_MENU_AMIIBO_REMOVE), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
        amiibo_remove_tag();
        set_status(UI_MENU_AMIIBO_REMOVED);
    }
    ImGui::EndDisabled();
}

// ---- "Advanced"
static void draw_folder(bool picking) {
    ImGui::TextUnformatted(menu_text(UI_MENU_AMIIBO_DIR));
    text_disabled_wrapped(g_root.c_str());
    ImGui::BeginDisabled(picking);
    if (ImGui::Button(menu_text(UI_MENU_AMIIBO_OPEN))) {
        plat_mutex_lock(&g_pick_lock);
        g_pick_open = true;
        plat_mutex_unlock(&g_pick_lock);
        folder_pick(g_root.c_str(), on_folder_picked, NULL);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(menu_text(UI_MENU_AMIIBO_DEFAULT_DIR))) set_root("");
    ImGui::SameLine();
    if (ImGui::Button(menu_text(UI_MENU_AMIIBO_REFRESH))) scan_dumps();
}

static void draw_reader_options(void) {
    const bool env = cfg_from_env("NX2NAVI_AMIIBO_POPUP") != 0;
    bool popup = cfg_bool("NX2NAVI_AMIIBO_POPUP") != 0;
    ImGui::BeginDisabled(env);
    if (ImGui::Checkbox(menu_text(UI_MENU_AMIIBO_POPUP), &popup)) opt_apply("NX2NAVI_AMIIBO_POPUP", popup ? "1" : "0");
    ImGui::EndDisabled();
    if (env) ImGui::TextDisabled("%s", menu_text(UI_MENU_SET_BY_ENV));

    bool auto_remove = amiibo_get_auto_remove();
    if (ImGui::Checkbox(menu_text(UI_MENU_AMIIBO_AUTO_REMOVE), &auto_remove)) amiibo_set_auto_remove(auto_remove);
    if (auto_remove) {
        float delay = amiibo_get_auto_remove_delay_s();
        ImGui::TextUnformatted(menu_text(UI_MENU_AMIIBO_HOLD));   // above the slider: beside it the text is cut off
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat("##amiibo_hold", &delay, 1.0f, 6.0f, menu_text(UI_MENU_AMIIBO_SECONDS)))
            amiibo_set_auto_remove_delay_s(delay);
    }
}

// The loaded figure's identity and stored data, and the button that erases its game data.
static void draw_figure_details(void) {
    AmiiboFigureData fd;
    if (!amiibo_get_figure_data(&fd)) {
        g_erase_armed_at = -1.0;
        return;
    }
    const AmiiboInfo *info = amiibo_get_current_info();
    char character[8], uid[24];
    snprintf(character, sizeof character, "%02x%02x%02x", info->character_id[0], info->character_id[1],
             info->character_id[2]);
    snprintf(uid, sizeof uid, "%02x:%02x:%02x:%02x:%02x:%02x:%02x", info->uuid[0], info->uuid[1], info->uuid[2],
             info->uuid[3], info->uuid[4], info->uuid[5], info->uuid[6]);
    text_wrapped_fmt(menu_text(UI_MENU_AMIIBO_TAG_FILE), info->filename);
    text_wrapped_fmt(menu_text(UI_MENU_AMIIBO_TAG_ID), character, info->series_id, info->numbering_id, info->nfp_type);
    text_wrapped_fmt(menu_text(UI_MENU_AMIIBO_TAG_UID), uid);
    UiTextId source = fd.source == AMIIBO_SOURCE_SAVED  ? UI_MENU_AMIIBO_SOURCE_SAVED
                      : fd.source == AMIIBO_SOURCE_DUMP ? UI_MENU_AMIIBO_SOURCE_DUMP
                                                        : UI_MENU_AMIIBO_SOURCE_NEW;
    text_wrapped_fmt(menu_text(UI_MENU_AMIIBO_SOURCE), menu_text(source));
    ImGui::TextWrapped("%s", menu_text(fd.decrypted ? UI_MENU_AMIIBO_DECRYPTED : UI_MENU_AMIIBO_NOT_DECRYPTED));
    text_wrapped_fmt(menu_text(UI_MENU_AMIIBO_WRITES), (unsigned)fd.write_counter, (unsigned)fd.write_year,
                     (unsigned)fd.write_month, (unsigned)fd.write_day);
    if (fd.has_app_area)
        text_wrapped_fmt(menu_text(UI_MENU_AMIIBO_GAME_DATA), (unsigned)fd.app_area_size, (unsigned)fd.access_id);
    else
        ImGui::TextWrapped("%s", menu_text(UI_MENU_AMIIBO_NO_GAME_DATA));

    // The game data goes for good (the port's copy in the profile too), so the button asks for a second press. "###"
    // keeps one id under both labels: the controller's cursor stays on the button when its label changes.
    const double now = ImGui::GetTime();
    if (!fd.has_app_area || now - g_erase_armed_at >= kEraseConfirmS) g_erase_armed_at = -1.0;
    const bool armed = g_erase_armed_at >= 0.0;
    const std::string label =
        std::string(menu_text(armed ? UI_MENU_AMIIBO_ERASE_CONFIRM : UI_MENU_AMIIBO_ERASE)) + "###amiibo_erase";
    ImGui::BeginDisabled(!fd.has_app_area);
    if (armed) ImGui::PushStyleColor(ImGuiCol_Text, kRed);
    const bool pressed = ImGui::Button(label.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0));
    if (armed) ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (pressed && !armed) {
        g_erase_armed_at = now;
    } else if (pressed) {
        g_erase_armed_at = -1.0;
        set_status(amiibo_erase_app_area() ? UI_MENU_AMIIBO_ERASED : UI_MENU_AMIIBO_ERASE_FAILED);
    }
}

// The keys file amiibo_reload_keys reads: NX2NAVI_AMIIBO_KEYS, else key_retail.bin in the amiibo folder
// (NX2NAVI_AMIIBO_DIR, else ./amiibo).
static std::string keys_path(void) {
    const char *keys = cfg_get("NX2NAVI_AMIIBO_KEYS");
    if (keys && *keys) return keys;
    const char *dir = cfg_get("NX2NAVI_AMIIBO_DIR");
    std::string path = dir && *dir ? dir : kDefaultAmiiboDir;
    if (path.back() != '/' && path.back() != '\\') path += '/';
    return path + "key_retail.bin";
}

// The keys: loaded or not, the file in effect, Reload keys; and the write-back setting (NX2NAVI_AMIIBO_WRITEBACK, a live
// setting), saved like a row's change (option_rows.h opt_apply).
static void draw_keys(void) {
    const bool loaded = amiibo_keys_loaded();
    ImGui::TextUnformatted(menu_text(UI_MENU_AMIIBO_KEYS));
    ImGui::SameLine();
    ImGui::TextColored(loaded ? kGreen : kRed, "%s",
                       menu_text(loaded ? UI_MENU_AMIIBO_KEYS_LOADED : UI_MENU_AMIIBO_KEYS_MISSING));
    const std::string path = keys_path();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    text_wrapped_fmt(menu_text(UI_MENU_AMIIBO_KEYS_FILE), path.c_str());
    ImGui::PopStyleColor();
    if (ImGui::Button(menu_text(UI_MENU_AMIIBO_RELOAD_KEYS), ImVec2(ImGui::GetContentRegionAvail().x, 0)))
        set_status(amiibo_reload_keys() ? UI_MENU_AMIIBO_KEYS_RELOADED : UI_MENU_AMIIBO_KEYS_FAILED, path);

    ImGui::Spacing();
    const bool env = cfg_from_env("NX2NAVI_AMIIBO_WRITEBACK") != 0;
    bool writeback = cfg_bool("NX2NAVI_AMIIBO_WRITEBACK") != 0;
    ImGui::BeginDisabled(env);
    if (ImGui::Checkbox(menu_text(UI_MENU_AMIIBO_WRITEBACK), &writeback))
        opt_apply("NX2NAVI_AMIIBO_WRITEBACK", writeback ? "1" : "0");
    ImGui::EndDisabled();
    if (env)
        ImGui::TextDisabled("%s", menu_text(UI_MENU_SET_BY_ENV));
    else if (!loaded)
        ImGui::TextDisabled("%s", menu_text(UI_MENU_AMIIBO_WRITEBACK_NEEDS_KEYS));
}

static void draw_reader_state(void) {
    UiTextId state_id = UI_MENU_AMIIBO_STATE_IDLE;
    ImVec4 state_col = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
    switch (amiibo_get_reader_state()) {
    case AMIIBO_DEV_STATE_SEARCHING: state_id = UI_MENU_AMIIBO_STATE_SEARCHING; state_col = ImVec4(1.0f, 0.8f, 0.2f, 1.0f); break;
    case AMIIBO_DEV_STATE_TAG_FOUND: state_id = UI_MENU_AMIIBO_STATE_FOUND; state_col = kGreen; break;
    case AMIIBO_DEV_STATE_TAG_MOUNTED: state_id = UI_MENU_AMIIBO_STATE_MOUNTED; state_col = kCyan; break;
    case AMIIBO_DEV_STATE_UNAVAILABLE: state_id = UI_MENU_AMIIBO_STATE_OFF; state_col = kRed; break;
    default: break;
    }
    ImGui::TextUnformatted(menu_text(UI_MENU_AMIIBO_STATE));
    ImGui::SameLine();
    ImGui::TextColored(state_col, "%s", menu_text(state_id));
}

// The last action, under the reader's state; nothing until there is one.
static void draw_status(void) {
    if (g_status_id == UI_MENU_AMIIBO_READY) return;
    char status[768];
    snprintf(status, sizeof status, menu_text(g_status_id), g_status_arg.c_str());
    const bool bad = g_status_id == UI_MENU_AMIIBO_LOAD_FAILED || g_status_id == UI_MENU_AMIIBO_KEYS_FAILED ||
                     g_status_id == UI_MENU_AMIIBO_ERASE_FAILED;
    if (bad) ImGui::PushStyleColor(ImGuiCol_Text, kRed);
    else ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", status);
    ImGui::PopStyleColor();
}

void tab_amiibo(float scale) {
    (void)scale;   // the widgets take ImGui's own sizes, already scaled with the font
    // amiibo.c reads the keys with the first dump; the tab's first draw reads them too, so its keys line is right before
    // any tap
    static bool keys_read;
    if (!keys_read) {
        keys_read = true;
        if (!amiibo_keys_loaded()) amiibo_reload_keys();
    }

    plat_mutex_lock(&g_pick_lock);
    const bool picking = g_pick_open;
    std::string picked;
    if (g_pick_ready) picked.swap(g_pick_path);
    g_pick_ready = false;
    plat_mutex_unlock(&g_pick_lock);
    if (!picked.empty()) set_root(picked);

    // the menu opened or the tab was switched to: the folder is read again (dumps added since show) and the cursor goes
    // to the list
    const bool focus = opt_focus_take();
    if (focus || !g_scanned) scan_dumps();

    const ImGuiStyle &st = ImGui::GetStyle();
    const float content_h = ImGui::GetContentRegionAvail().y - opt_bottom();   // Continue and Quit stay visible
    const float left_w = ImGui::GetContentRegionAvail().x * 0.5f;
    const float right_w = ImGui::GetContentRegionAvail().x - left_w - st.ItemSpacing.x;

    // ---- left: the dumps
    ImGui::BeginChild("##amiibo_dumps_pane", ImVec2(left_w, content_h), ImGuiChildFlags_Borders);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##amiibo_filter", menu_text(UI_MENU_AMIIBO_FILTER), g_filter, sizeof g_filter);
    const char *hint = menu_text(UI_MENU_AMIIBO_LIST_HINT);
    const float hint_h = ImGui::CalcTextSize(hint, nullptr, false, ImGui::GetContentRegionAvail().x).y + st.ItemSpacing.y;
    ImGui::BeginChild("##amiibo_dumps", ImVec2(0, -hint_h), ImGuiChildFlags_Borders);
    if (g_dumps.empty()) empty_note();
    dump_list(focus, true);
    ImGui::EndChild();
    text_disabled_wrapped(hint);
    ImGui::EndChild();

    ImGui::SameLine();

    // ---- right: the reader, the figure, Advanced
    ImGui::BeginChild("##amiibo_side", ImVec2(right_w, content_h), ImGuiChildFlags_Borders);
    ImGui::TextColored(kGold, "%s", menu_text(UI_MENU_AMIIBO_READER));
    ImGui::Separator();
    draw_reader_state();
    draw_status();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    draw_figure();
    ImGui::Spacing();
    if (ImGui::CollapsingHeader(menu_text(UI_MENU_AMIIBO_ADVANCED))) {
        draw_folder(picking);
        ImGui::Spacing();
        ImGui::Separator();
        draw_reader_options();
        AmiiboFigureData fd;
        if (amiibo_get_figure_data(&fd)) {
            ImGui::Spacing();
            ImGui::Separator();
            draw_figure_details();
        } else {
            g_erase_armed_at = -1.0;
        }
        ImGui::Spacing();
        ImGui::Separator();
        draw_keys();
    } else {
        g_erase_armed_at = -1.0;
    }
    ImGui::EndChild();
}

// ---- the picker

static bool g_was_waiting;
static bool g_picker_focus;

bool amiibo_picker_wanted(void) {
    // amiibo_get_reader_state holds "searching" across the game's many restarts of detection while it waits
    const bool waiting = amiibo_get_reader_state() == AMIIBO_DEV_STATE_SEARCHING;
    const bool started = waiting && !g_was_waiting;
    g_was_waiting = waiting;
    return started && cfg_bool("NX2NAVI_AMIIBO_POPUP") && !amiibo_has_tag();
}

void amiibo_picker_begin(void) {
    scan_dumps();
    g_picker_focus = true;
    set_status(UI_MENU_AMIIBO_READY);
}

// "Tapped: <figure>" after a tap from the picker, until g_note_until (plat_now_ns)
static uint64_t g_note_until;
static std::string g_note_label;
static const uint64_t kNoteNs = 2500000000ull;

bool amiibo_note_shows(void) { return g_note_until && plat_now_ns() < g_note_until; }

void amiibo_note_draw(uint32_t w, float scale) {
    if (!amiibo_note_shows()) return;
    char line[256];
    snprintf(line, sizeof line, menu_text(UI_MENU_AMIIBO_TAPPED), g_note_label.c_str());
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const ImVec2 s = ImGui::CalcTextSize(line);
    const float padx = 16 * scale, pady = 10 * scale;
    const float bw = s.x + padx * 2, bh = s.y + pady * 2;
    const ImVec2 p(((float)w - bw) * 0.5f, 12 * scale);
    skin_panel(dl, p, ImVec2(p.x + bw, p.y + bh), scale * 0.6f, 0.92f);
    dl->AddText(ImVec2(p.x + padx, p.y + pady), SKIN_TEXT, line);
}

bool amiibo_picker_layout(uint32_t w, uint32_t h, float scale) {
    // the game gave up waiting (its own time-out, a cut scene): nothing left to tap for
    if (amiibo_get_reader_state() != AMIIBO_DEV_STATE_SEARCHING) return false;

    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2((float)w, (float)h), IM_COL32(10, 6, 2, 110));
    const float ww = fminf(w * 0.9f, 560 * scale), wh = fminf(h * 0.8f, 620 * scale);
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ww, wh), ImGuiCond_Always);
    ImGui::Begin("##amiibo_picker", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored(kGold, "%s", menu_text(UI_MENU_AMIIBO_POPUP_TITLE));
    ImGui::Separator();
    const ImGuiStyle &st = ImGui::GetStyle();
    // under the list: the hint, and a failed load's line
    const bool failed = g_status_id == UI_MENU_AMIIBO_LOAD_FAILED;
    const float below = ImGui::GetTextLineHeightWithSpacing() * (failed ? 3 : 1) + st.ItemSpacing.y;
    ImGui::BeginChild("##amiibo_picker_list", ImVec2(0, -below), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
    if (g_dumps.empty()) empty_note();
    const Dump *tapped = dump_list(g_picker_focus, false);
    g_picker_focus = false;
    ImGui::EndChild();
    if (failed) draw_status();
    ImGui::TextDisabled("%s", menu_text(UI_MENU_AMIIBO_POPUP_HINT));
    ImGui::End();

    if (tapped) {
        g_note_label = tapped->label;
        g_note_until = plat_now_ns() + kNoteNs;
        return false;
    }
    return !ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
}
