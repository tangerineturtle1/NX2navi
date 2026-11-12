// The debug menu's panel (F9 with debug_menu = 1; ui/menu.cpp lays it out in the settings menu's place while it is
// open, so its input, its sounds and the game's blocked pad are the menu's own): on the left edge, in the performance
// overlay's look, with the game in view beside it. What it does to the game is in
// debug_tools.c: the game's on/off flags by name, teleporting to the game's own start positions, starting its events,
// the game's speed, Link's position, a free camera, and a search of the game's memory with values to set and to hold. Plain Dear ImGui with English text: a tool for testing, not part of the game's
// interface. Present thread only.
#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"

#include "debug_tools.h"
extern "C" void rt_log(const char *fmt, ...);
#include "ui/debug_menu.h"
#include "ui/menu_skin.h"

namespace {

int g_tab = -1;   // a tab asked for by the test hook (debug_menu_tab), -1 none

// ---- flags
char f_filter[64];
bool f_only_on, f_only_named;
int *f_rows;          // the flags that pass the filter
int f_rows_n, f_count;
int f_age = 1000;     // frames since the values were read
bool f_dirty = true;
struct Change { uint32_t hash; int on; char name[72]; } f_log[6];
int f_log_n;

bool contains(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return true;
    }
    return !n;
}

void flags_filter(void) {
    if (!f_rows) f_rows = (int *)malloc(sizeof(int) * 200000);
    f_rows_n = 0;
    if (!f_rows) return;
    char hex[12];
    for (int i = 0; i < f_count && i < 200000; i++) {
        const char *name = dbg_flag_name(i);
        if (f_only_on && !dbg_flag_value(i)) continue;
        if (f_only_named && !name) continue;
        if (f_filter[0]) {
            snprintf(hex, sizeof hex, "%08x", dbg_flag_hash(i));
            if (!(name && contains(name, f_filter)) && !contains(hex, f_filter)) continue;
        }
        f_rows[f_rows_n++] = i;
    }
    // the named ones first, by name; the rest by hash, as the back end lists them
    std::stable_sort(f_rows, f_rows + f_rows_n, [](int a, int b) {
        const char *x = dbg_flag_name(a), *y = dbg_flag_name(b);
        if (!x || !y) return x && !y;
        return strcmp(x, y) < 0;
    });
    f_dirty = false;
    if (getenv("NX2NAVI_DBG_TEST")) rt_log("debug: filter \"%s\": %d flags shown", f_filter, f_rows_n);   // test runs
}

void tab_flags(void) {
    if (++f_age > 20) {
        int n = dbg_flags_refresh();
        if (n != f_count) f_count = n, f_dirty = true;
        f_age = 0;
        if (f_only_on) f_dirty = true;   // values moved
    }
    if (!f_count) {
        ImGui::TextWrapped("The game's flags appear once a save is loaded.");
        return;
    }
    static int names_was;
    int found = 0, names = dbg_names(&found);
    if (names != names_was) names_was = names, f_dirty = true;
    if (names == 2) ImGui::TextWrapped("%d on/off flags of the game, %d with a name found in its event files.", f_count, found);
    else ImGui::TextWrapped("%d on/off flags of the game. Reading their names from the game's event files...", f_count);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##filter", "part of a name, or of the hash", f_filter, sizeof f_filter)) f_dirty = true;
    if (ImGui::Checkbox("only on", &f_only_on)) f_dirty = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("only named", &f_only_named)) f_dirty = true;
    if (f_dirty) flags_filter();
    ImGui::SameLine();
    ImGui::TextDisabled("%d shown", f_rows_n);
    float log_h = f_log_n ? ImGui::GetTextLineHeightWithSpacing() * (float)(f_log_n + 1) + 8 : 0;
    if (ImGui::BeginChild("##flags", ImVec2(0, -log_h), ImGuiChildFlags_Borders)) {
        ImGuiListClipper clip;
        clip.Begin(f_rows_n);
        while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                int i = f_rows[r];
                const char *name = dbg_flag_name(i);
                uint32_t hash = dbg_flag_hash(i);
                bool on = dbg_flag_value(i) != 0;
                char label[128];
                if (name) snprintf(label, sizeof label, "%s##%08x", name, hash);
                else snprintf(label, sizeof label, "(no name) %08x##%08x", hash, hash);
                if (ImGui::Checkbox(label, &on)) {
                    dbg_flag_set(hash, on);
                    f_age = 15;   // read again in a few frames: the game applies the change at its next update
                    for (int k = f_log_n < 6 ? f_log_n : 5; k > 0; k--) f_log[k] = f_log[k - 1];
                    f_log[0].hash = hash, f_log[0].on = on;
                    snprintf(f_log[0].name, sizeof f_log[0].name, "%s", name ? name : "(no name)");
                    if (f_log_n < 6) f_log_n++;
                }
            }
    }
    ImGui::EndChild();
    if (f_log_n) {
        ImGui::TextDisabled("Changed in this session (click to put back):");
        for (int k = 0; k < f_log_n; k++) {
            char label[128];
            snprintf(label, sizeof label, "%s %08x -> %s##log%d", f_log[k].name, f_log[k].hash, f_log[k].on ? "on" : "off", k);
            if (ImGui::SmallButton(label)) {
                dbg_flag_set(f_log[k].hash, !f_log[k].on);
                f_log[k].on = !f_log[k].on;
                f_age = 15;
            }
        }
    }
}

// ---- teleport and cutscenes
char t_filter[64], e_filter[64];
int t_group;          // 0 all, 1 the warp songs' pads, 2 the two sides of the loading zones, 3 boss rooms
int *t_rows, t_rows_n, t_count = -1, t_group_was = -1;
int *e_rows, e_rows_n, e_count = -1;
char t_filter_was[64] = "\x01", e_filter_was[64] = "\x01";

bool ends_with(const char *s, const char *end) {
    size_t n = strlen(s), m = strlen(end);
    return n >= m && !strcmp(s + n - m, end);
}

// The groups go by the words the game's own names use: a warp song's pad is named after its song, a loading zone's
// far side after where it leads, and a boss's room is an area of its own whose name ends in Stage.
bool in_group(int group, const char *area, const char *pos) {
    static const char *const songs[] = {"Minuet", "Bolero", "Serenade", "Requiem", "Nocturne", "Prelude"};
    static const char *const doors[] = {"From", "Side", "Entrance", "Front"};
    if (group == 1) {
        for (const char *w : songs)
            if (ends_with(pos, w)) return true;
        return false;
    }
    if (group == 2) {
        for (const char *w : doors)
            if (strstr(pos, w)) return true;
        return false;
    }
    if (group == 3) return ends_with(area, "Stage");
    return true;
}

// The last thing the teleport or an event start did, and whether one is under way.
void status_line(void) {
    char status[160];
    dbg_status(status);
    if (status[0]) ImGui::TextWrapped("%s", status);
}

// The lists come with the flag names, from the game's files: reading them starts once the game's flags are there.
bool lists_ready(const char *what) {
    dbg_flags_refresh();
    if (dbg_names(NULL) == 2) return true;
    ImGui::TextWrapped("Reading %s from the game's files... They appear once a save is loaded.", what);
    return false;
}

void tab_teleport(void) {
    if (!lists_ready("the game's areas")) return;
    int count = dbg_places();
    if (!count) {
        ImGui::TextWrapped("The game's list of start positions was not found in its files.");
        return;
    }
    ImGui::TextWrapped("Every area of the game with its start positions, from the game's own list. Link arrives through the "
                       "game's own change of area, as after a door or a warp song.");
    static const char *const groups[] = {"All", "Warp song pads", "Loading zones", "Boss rooms"};
    for (int g = 0; g < 4; g++) {
        if (g) ImGui::SameLine();
        bool is = t_group == g;
        if (is) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(groups[g])) t_group = g;
        if (is) ImGui::PopStyleColor();
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##tfilter", "part of an area's or a position's name", t_filter, sizeof t_filter);
    if (count != t_count || t_group != t_group_was || strcmp(t_filter, t_filter_was)) {
        if (!t_rows) t_rows = (int *)malloc(sizeof(int) * 4096);
        t_rows_n = 0;
        for (int i = 0; t_rows && i < count && i < 4096; i++) {
            const char *area = dbg_place_area(i), *pos = dbg_place_pos(i);
            if (!in_group(t_group, area, pos)) continue;
            if (t_filter[0] && !contains(area, t_filter) && !contains(pos, t_filter)) continue;
            t_rows[t_rows_n++] = i;
        }
        t_count = count, t_group_was = t_group;
        snprintf(t_filter_was, sizeof t_filter_was, "%s", t_filter);
        if (getenv("NX2NAVI_DBG_TEST")) rt_log("debug: teleport list: group %d, filter \"%s\": %d of %d places", t_group, t_filter, t_rows_n, count);
    }
    status_line();
    bool busy = dbg_busy() != 0;
    const float foot = ImGui::GetTextLineHeightWithSpacing() * 3.2f;
    if (ImGui::BeginChild("##places", ImVec2(0, -foot), ImGuiChildFlags_Borders)) {
        if (busy) ImGui::BeginDisabled();
        ImGuiListClipper clip;
        clip.Begin(t_rows_n);
        while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                int i = t_rows[r];
                const char *area = dbg_place_area(i), *pos = dbg_place_pos(i);
                char label[160];
                snprintf(label, sizeof label, "%s   >   %s##%d", area, pos[0] ? pos : "(default start)", i);
                if (ImGui::Selectable(label)) dbg_warp(area, pos);
            }
        if (busy) ImGui::EndDisabled();
    }
    ImGui::EndChild();
    ImGui::TextWrapped("Areas ending in _Child or _Adult are the two times of the same place: Link arrives at the age he has. "
                       "The game may save by itself in the new area, as it does after its own doors: keep a copy of the save.");
}

void tab_events(void) {
    if (!lists_ready("the game's events")) return;
    int count = dbg_events();
    ImGui::TextWrapped("The game's event scripts by name: cutscenes (most start with Dm_), conversations and scripted moments. "
                       "One starts as if its trigger had fired, in the area Link is in.");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##efilter", "part of a name", e_filter, sizeof e_filter);
    if (count != e_count || strcmp(e_filter, e_filter_was)) {
        if (!e_rows) e_rows = (int *)malloc(sizeof(int) * 4096);
        e_rows_n = 0;
        for (int i = 0; e_rows && i < count && i < 4096; i++)
            if (!e_filter[0] || contains(dbg_event_name(i), e_filter)) e_rows[e_rows_n++] = i;
        e_count = count;
        snprintf(e_filter_was, sizeof e_filter_was, "%s", e_filter);
        if (getenv("NX2NAVI_DBG_TEST")) rt_log("debug: event list: filter \"%s\": %d of %d events", e_filter, e_rows_n, count);
    }
    status_line();
    bool busy = dbg_busy() != 0;
    const float foot = ImGui::GetTextLineHeightWithSpacing() * 4.2f;
    if (ImGui::BeginChild("##events", ImVec2(0, -foot), ImGuiChildFlags_Borders)) {
        if (busy) ImGui::BeginDisabled();
        ImGuiListClipper clip;
        clip.Begin(e_rows_n);
        while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                char label[112];
                snprintf(label, sizeof label, "%s##%d", dbg_event_name(e_rows[r]), e_rows[r]);
                if (ImGui::Selectable(label)) dbg_event_start(dbg_event_name(e_rows[r]));
            }
        if (busy) ImGui::EndDisabled();
    }
    ImGui::EndChild();
    ImGui::TextWrapped("An event made for another area, or for actors that are not here, may show nothing, end at once or leave "
                       "the game waiting: teleport to its area first. An event can set the story's flags and can move Link to "
                       "another area, as it does in the game: keep a copy of the save.");
}

// ---- world
void tab_world(void) {
    float p[3];
    if (dbg_player_pos(p)) ImGui::Text("Link is at  x %.2f   y %.2f   z %.2f  (metres; y is up)", p[0], p[1], p[2]);
    else ImGui::TextDisabled("Link's position appears once a save is loaded.");
    ImGui::Separator();
    float speed = dbg_speed_factor();
    ImGui::Text("Game speed");
    static const float steps[] = {0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
    for (float s : steps) {
        char label[16];
        snprintf(label, sizeof label, s < 1 ? "%.2gx" : "%.0fx", s);
        if (s != steps[0]) ImGui::SameLine();
        bool is = speed == s;
        if (is) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(label)) dbg_speed_set(s);
        if (is) ImGui::PopStyleColor();
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##speed", &speed, 0.05f, 8.0f, "%.2fx", ImGuiSliderFlags_Logarithmic)) dbg_speed_set(speed);
    ImGui::TextWrapped("The whole game runs at this speed, its interface and its music's timing included. It goes back to 1x when "
                       "the game is closed.");
    ImGui::Separator();
    ImGui::TextColored(ImVec4(236 / 255.0f, 196 / 255.0f, 104 / 255.0f, 1), "Free camera");
    if (!dbg_freecam_available()) {
        ImGui::TextWrapped("The free camera needs the game's code built by this version: the launcher's update or its rebuild "
                           "makes it.");
        return;
    }
    int sticks = 1;
    float cam_speed = 8;
    double at[3] = {0, 0, 0};
    bool on = dbg_freecam(&sticks, &cam_speed, at) != 0;
    bool changed = ImGui::Checkbox("The game draws from a camera of your own", &on);
    if (on) {
        if (ImGui::RadioButton("The sticks fly the camera, Link stands", sticks != 0)) sticks = 1, changed = true;
        if (ImGui::RadioButton("The camera stays where it is, the pad plays the game", sticks == 0)) sticks = 0, changed = true;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##camspeed", &cam_speed, 1.0f, 100.0f, "%.0f metres a second", ImGuiSliderFlags_Logarithmic)) changed = true;
        if (ImGui::Button("Back to the game's view")) dbg_freecam_reset();
        ImGui::SameLine();
        ImGui::TextDisabled("x %.1f   y %.1f   z %.1f", at[0], at[1], at[2]);
    }
    if (changed) dbg_freecam_set(on, sticks, cam_speed);
    ImGui::TextWrapped("Close this menu to fly: the left stick moves level, ahead and sideways, the right stick turns, ZR and ZL "
                       "go up and down, R is four times as fast and L four times as slow. The height changes only with ZR "
                       "and ZL, and with DLSS, DLAA or FSR 3.1 or 4 on the camera rises where the ground ahead or below comes "
                       "within a few metres. The game goes on meanwhile and draws what the camera sees; the game speed above "
                       "slows it down. Without those upscalers it does not stop at hills: below the ground the picture shows "
                       "the void under the map, not a missing floor.");
}

// ---- memory
int m_type;           // 0: 4 bytes, 1: 2 bytes, 2: 1 byte, 3: float
char m_value[32] = "0";
char m_set[32];
char m_note[160];
const int m_sizes[] = {4, 2, 1, 4};

bool parse_value(const char *s, uint32_t *out) {
    char *end = NULL;
    if (m_type == 3) {
        float f = strtof(s, &end);
        memcpy(out, &f, 4);
    } else {
        unsigned long long v = strtoull(s, &end, 0);
        if (v >> (8 * m_sizes[m_type])) return false;
        *out = (uint32_t)v;
    }
    return end && end != s;
}

void show_value(char *out, size_t n, uint32_t v) {
    if (m_type == 3) {
        float f;
        memcpy(&f, &v, 4);
        snprintf(out, n, "%g", (double)f);
    } else {
        snprintf(out, n, "%u", v);
    }
}

void tab_memory(void) {
    ImGui::TextWrapped("Finds a value in the game's memory: search for what a number is now, change it in the game, then narrow "
                       "the places down until few are left. Setting or holding a wrong place can break the running game; a save "
                       "made before is the way back.");
    const float field = ImGui::GetFontSize() * 12;
    ImGui::SetNextItemWidth(field);
    int over = 0, count = dbg_search_count(&over);
    if (count) ImGui::BeginDisabled();
    ImGui::Combo("##type", &m_type, "4 bytes (whole number)\0002 bytes\0001 byte\000float (4 bytes)\000");
    if (count) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(field * 0.6f);
    ImGui::InputText("value", m_value, sizeof m_value, ImGuiInputTextFlags_CharsScientific);
    uint32_t v = 0;
    bool ok = parse_value(m_value, &v);
    if (!count) {
        if (!ok) ImGui::BeginDisabled();
        if (ImGui::Button("Search all memory")) {
            int n = dbg_search_new(v, m_sizes[m_type]);
            dbg_search_count(&over);
            snprintf(m_note, sizeof m_note, over ? "More than %d places hold it: only these are kept. Search for a rarer value, or narrow down."
                                                 : "%d places hold it.", n);
        }
        if (!ok) ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("(takes a few seconds; the game waits)");
    } else {
        static const char *const modes[] = {"equals the value", "changed", "unchanged", "went up", "went down"};
        for (int m = 0; m < 5; m++) {
            if (m && m != 3) ImGui::SameLine();   // two rows: the panel is narrow
            if (m == 0 && !ok) ImGui::BeginDisabled();
            if (ImGui::Button(modes[m])) snprintf(m_note, sizeof m_note, "%d places left.", dbg_search_next(m, v));
            if (m == 0 && !ok) ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ImGui::Button("New search")) dbg_search_clear(), m_note[0] = 0;
    }
    if (m_note[0]) ImGui::TextUnformatted(m_note);
    int frozen = dbg_frozen_count();
    if (frozen) {
        char label[48];
        snprintf(label, sizeof label, "Stop holding %d value%s", frozen, frozen == 1 ? "" : "s");
        if (ImGui::Button(label)) dbg_unfreeze_all();
    }
    count = dbg_search_count(&over);
    if (!count) return;
    ImGui::SetNextItemWidth(field * 0.6f);
    ImGui::InputText("value to set", m_set, sizeof m_set, ImGuiInputTextFlags_CharsScientific);
    uint32_t setv = 0;
    bool set_ok = parse_value(m_set, &setv);
    int size = dbg_search_size();
    if (ImGui::BeginChild("##results", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        int shown = count < 300 ? count : 300;
        if (count > shown) ImGui::TextDisabled("The first %d of %d places:", shown, count);
        for (int i = 0; i < shown; i++) {
            uint64_t a = dbg_search_addr(i);
            uint32_t now = 0;
            char text[32] = "?";
            if (dbg_mem_read(a, size, &now)) show_value(text, sizeof text, now);
            ImGui::PushID(i);
            ImGui::Text("%012llx   %-14s", (unsigned long long)a, text);
            ImGui::SameLine();
            if (!set_ok) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Set")) dbg_mem_write(a, setv, size);
            if (!set_ok) ImGui::EndDisabled();
            ImGui::SameLine();
            bool hold = dbg_frozen(a) != 0;
            if (ImGui::Checkbox("hold", &hold)) {
                if (!dbg_freeze(a, set_ok ? setv : now, size, hold)) snprintf(m_note, sizeof m_note, "No more than 64 values can be held.");
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

}  // namespace

extern "C" void debug_menu_tab(int tab) { g_tab = tab; }

// A panel on the left edge in the performance overlay's dress (the game's panel art, gold labels, the smaller text),
// as tall as the window: the game stays in view beside it while a flag or a value is changed.
extern "C" void debug_menu_layout(uint32_t w, uint32_t h, float scale) {
    const float m = 12 * scale;
    const ImVec4 gold(236 / 255.0f, 196 / 255.0f, 104 / 255.0f, 1), dim(0.72f, 0.69f, 0.62f, 1);
    float width = (float)w * 0.27f, min_width = 560 * scale;
    if (width < min_width) width = min_width < (float)w - 2 * m ? min_width : (float)w - 2 * m;
    ImGui::SetNextWindowPos(ImVec2(m, m), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, (float)h - 2 * m), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));   // the panel below is the background
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * scale, 20 * scale));
    bool shown = ImGui::Begin("##debug", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    if (shown) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 a = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
        skin_panel(dl, a, ImVec2(a.x + sz.x, a.y + sz.y), scale * 0.8f, 0.92f);
        ImGui::SetWindowFontScale(0.72f);
        ImGui::TextColored(gold, "Debug menu");
        ImGui::SameLine();
        ImGui::TextColored(dim, "  F9 or Esc closes");
        ImGui::Dummy(ImVec2(0, 2 * scale));
        if (!dbg_supported()) {
            ImGui::TextWrapped("The debug menu knows the structures of Ocarina of Time 1.1.1 only.");
        } else if (ImGui::BeginTabBar("##tabs")) {
            static const char *const names[DEBUG_MENU_TABS] = {"Flags", "Teleport", "Cutscenes", "World", "Memory"};
            for (int t = 0; t < DEBUG_MENU_TABS; t++) {
                if (ImGui::BeginTabItem(names[t], NULL, g_tab == t ? ImGuiTabItemFlags_SetSelected : 0)) {
                    if (t == 0) tab_flags();
                    else if (t == 1) tab_teleport();
                    else if (t == 2) tab_events();
                    else if (t == 3) tab_world();
                    else tab_memory();
                    ImGui::EndTabItem();
                }
            }
            g_tab = -1;
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}
