// The full performance overlay's panel (ui/perf_panel.h) in the settings menu's look: the menu's framed dark panel
// (ui/menu_skin.cpp), the numbers in tables with gold labels and parchment values, the game's divider between the
// machine, the frames, the per-frame waits and the picture, and the frame times as a gold line.
#include <float.h>
#include <math.h>
#include <stdio.h>

#include "imgui.h"
#include "imgui_internal.h"   // the window's draw cursor (the plot does not widen the panel)

#include "ui/menu_skin.h"
#include "ui/perf_panel.h"

extern "C" {
#include "ui_text.h"
}

namespace {

const ImVec4 kLabel(236 / 255.0f, 196 / 255.0f, 104 / 255.0f, 1);   // SKIN_GOLD
const ImVec4 kValue(244 / 255.0f, 236 / 255.0f, 214 / 255.0f, 1);   // SKIN_TEXT
const ImVec4 kDimText(170 / 255.0f, 152 / 255.0f, 120 / 255.0f, 1);

struct PerfCol { UiTextId fmt; float sample; };

float text_w(UiTextId fmt, float v) {
    char b[64];
    snprintf(b, sizeof b, menu_text(fmt), v);
    return ImGui::CalcTextSize(b).x;
}

// The panel keeps its width while the numbers change: every column is as wide as the widest text it can show, measured
// on samples of 8s (the menu fonts' digits share one width), and the numbers are right-aligned in it.
// A fixed column as wide as the widest of the formats (with their samples) and labels given.
void column(const char *id, const PerfCol *cols, int n, const UiTextId *labels = nullptr, int nl = 0) {
    float w = ImGui::CalcTextSize(menu_text(UI_MENU_OV_NONE)).x;
    for (int i = 0; i < n; i++) w = fmaxf(w, text_w(cols[i].fmt, cols[i].sample));
    for (int i = 0; i < nl; i++) w = fmaxf(w, ImGui::CalcTextSize(menu_text(labels[i])).x);
    ImGui::TableSetupColumn(id, ImGuiTableColumnFlags_WidthFixed, w);
}

void right(const char *s, bool dim) {
    float pad = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(s).x;
    if (pad > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
    ImGui::TextColored(dim ? kDimText : kValue, "%s", s);
}

// One cell: fmt (a catalog format, one number) with v, or "-" when the machine gives none.
void cell(UiTextId fmt, float v) {
    ImGui::TableNextColumn();
    if (v < 0) return right(menu_text(UI_MENU_OV_NONE), true);
    char b[64];
    snprintf(b, sizeof b, menu_text(fmt), v);
    right(b, false);
}

void label(UiTextId id) {
    ImGui::TableNextColumn();
    ImGui::TextColored(kLabel, "%s", menu_text(id));
}

float gb(float mb) { return mb < 0 ? -1 : mb / 1024; }

// The game's divider across the panel between two sections, smaller than the menu's.
void divider(float scale) {
    float s = scale * 0.72f, h = skin_filigree_height(s);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGuiWindow *win = ImGui::GetCurrentWindow();
    float x1 = win->WorkRect.Max.x > p.x + 40 * s ? win->WorkRect.Max.x : p.x + 200 * s;
    skin_filigree(ImGui::GetWindowDrawList(), p.x - 4 * s, x1 + 4 * s, p.y + h * 0.5f, s);
    ImGui::Dummy(ImVec2(0, h));
}

void machine_and_frames(const PerfPanel *p, float scale) {
    const PerfStats &s = p->s;
    if (!ImGui::BeginTable("##perft", 4, ImGuiTableFlags_SizingFixedFit)) return;
    static const UiTextId labels[] = {UI_MENU_OV_GPU, UI_MENU_OV_VRAM, UI_MENU_OV_CPU, UI_MENU_OV_RAM,
                                      UI_MENU_OV_FPS_LABEL, UI_MENU_OV_FRAME_LABEL};
    static const PerfCol c1[] = {{UI_MENU_OV_FMT_PCT, 100}, {UI_MENU_OV_FMT_GB, 888.8f}, {UI_MENU_OV_FMT_FPS, 888},
                                 {UI_MENU_OV_FRAME_MS, 888.8f}};
    static const PerfCol c2[] = {{UI_MENU_OV_FMT_TEMP, 188}, {UI_MENU_OV_FMT_GB_OF, 888.8f}, {UI_MENU_OV_LOW1, 888},
                                 {UI_MENU_OV_MAX_MS, 8888.8f}};
    static const PerfCol c3[] = {{UI_MENU_OV_FMT_POWER, 888}, {UI_MENU_OV_FMT_MHZ, 8888}, {UI_MENU_OV_GAME_PCT, 100},
                                 {UI_MENU_OV_GAME_GB, 888.8f}, {UI_MENU_OV_LOW01, 888}};
    column("##c0", nullptr, 0, labels, 6);
    column("##c1", c1, 4);
    column("##c2", c2, 4);
    column("##c3", c3, 5);
    label(UI_MENU_OV_GPU);
    cell(UI_MENU_OV_FMT_PCT, s.gpu_pct);
    cell(UI_MENU_OV_FMT_TEMP, s.gpu_temp_c);
    cell(UI_MENU_OV_FMT_POWER, s.gpu_power_w);
    label(UI_MENU_OV_VRAM);
    cell(UI_MENU_OV_FMT_GB, gb(s.vram_used_mb));
    cell(UI_MENU_OV_FMT_GB_OF, gb(s.vram_total_mb));
    // the card's figure counts every program on it: this game's own share beside it, the clock where that is unknown
    if (s.vram_game_mb >= 0) cell(UI_MENU_OV_GAME_GB, gb(s.vram_game_mb));
    else cell(UI_MENU_OV_FMT_MHZ, s.gpu_clock_mhz);
    label(UI_MENU_OV_CPU);
    cell(UI_MENU_OV_FMT_PCT, s.cpu_total_pct);
    cell(UI_MENU_OV_FMT_TEMP, s.cpu_temp_c);
    cell(UI_MENU_OV_GAME_PCT, s.cpu_game_pct);
    label(UI_MENU_OV_RAM);
    cell(UI_MENU_OV_FMT_GB, gb(s.ram_used_mb));
    cell(UI_MENU_OV_FMT_GB_OF, gb(s.ram_total_mb));
    cell(UI_MENU_OV_GAME_GB, gb(s.ram_game_mb));
    ImGui::EndTable();
    divider(scale);
    if (!ImGui::BeginTable("##perff", 4, ImGuiTableFlags_SizingFixedFit)) return;
    column("##f0", nullptr, 0, labels, 6);
    column("##f1", c1, 4);
    column("##f2", c2, 4);
    column("##f3", c3, 5);
    // the frames: the rate over the last second with the lows over the last ten seconds, then the frame times
    label(UI_MENU_OV_FPS_LABEL);
    cell(UI_MENU_OV_FMT_FPS, s.fps);
    cell(UI_MENU_OV_LOW1, s.low1_fps);
    cell(UI_MENU_OV_LOW01, s.low01_fps);
    if (p->fg_fps > 0) {   // frame generation: what the display gets, generated frames included
        label(UI_MENU_OV_FG_LABEL);
        cell(UI_MENU_OV_FMT_FPS, p->fg_fps);
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
    }
    label(UI_MENU_OV_FRAME_LABEL);
    cell(UI_MENU_OV_FRAME_MS, s.frame_ms_avg);
    cell(UI_MENU_OV_MAX_MS, s.frame_ms_max);
    ImGui::TableNextColumn();
    ImGui::EndTable();
}

// The frame times of the last PERF_HISTORY frames as a gold line over a dark band, as wide as the tables made the panel
// (the plot does not count toward that width, so the panel can still shrink).
void frame_plot(const PerfStats &s, float scale) {
    float top = s.frame_ms_max > 50 ? s.frame_ms_max : 50;
    ImGuiWindow *win = ImGui::GetCurrentWindow();
    float max_x = win->DC.CursorMaxPos.x;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0.32f));
    ImGui::PushStyleColor(ImGuiCol_PlotLines, kLabel);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
    ImGui::PlotLines("##ft", s.frame_ms, PERF_HISTORY, 0, nullptr, 0, top, ImVec2(-FLT_MIN, 48 * scale));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    win->DC.CursorMaxPos.x = max_x;
}

// Per frame, averaged over the last half second; the labels' column stretches, so the numbers end at the panel's right
// edge, and the table still asks for no more than its widest label and number.
void waits(const PerfStats &s) {
    if (!ImGui::BeginTable("##perfw", 2, ImGuiTableFlags_SizingFixedFit)) return;
    static const PerfCol c1[] = {{UI_MENU_OV_FMT_MS, 888.88f}, {UI_MENU_OV_PER_FRAME, 888.8f}};
    ImGui::TableSetupColumn("##w0", ImGuiTableColumnFlags_WidthStretch);
    column("##w1", c1, 2);
    label(UI_MENU_OV_REPLAY);
    cell(UI_MENU_OV_FMT_MS, s.replay_ms);
    label(UI_MENU_OV_GPU_TIME);
    cell(UI_MENU_OV_FMT_MS, s.gpu_ms);
    label(UI_MENU_OV_PIPE_WAIT);
    cell(UI_MENU_OV_FMT_MS, s.pipe_wait_ms);
    label(UI_MENU_OV_SHADER_WAIT);
    cell(UI_MENU_OV_FMT_MS, s.xlate_ms);
    label(UI_MENU_OV_TEX_UPLOADS);
    cell(UI_MENU_OV_PER_FRAME, s.tex_uploads);
    ImGui::EndTable();
}

void picture(const PerfPanel *p) {
    static const UiTextId labels[11] = {UI_MENU_OV_WINDOW, UI_MENU_OV_OUTPUT, UI_MENU_OV_SCENE, UI_MENU_UPSCALING, UI_MENU_AA,
                                        UI_MENU_OV_LIMIT, UI_MENU_PRESENT_VSYNC, UI_MENU_GAME_ASPECT, UI_MENU_GAME_RES, UI_MENU_HDR,
                                        UI_MENU_FRAMEGEN};
    if (!ImGui::BeginTable("##perfp", 2, ImGuiTableFlags_SizingFixedFit)) return;
    float vw = 0;
    for (const auto &v : p->picture) vw = fmaxf(vw, ImGui::CalcTextSize(v).x);
    ImGui::TableSetupColumn("##p0", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##p1", ImGuiTableColumnFlags_WidthFixed, vw);
    for (int i = 0; i < 11; i++) {
        if ((i == 2 && !p->scene_known) || !p->picture[i][0]) continue;
        label(labels[i]);
        ImGui::TableNextColumn();
        right(p->picture[i], false);
    }
    ImGui::EndTable();
}

}  // namespace

void perf_panel_draw(const PerfPanel *p, float width, float scale) {
    const float m = 12 * scale;
    ImGui::SetNextWindowPos(ImVec2(width - m, m), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));   // the panel below is the background
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * scale, 20 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(7 * scale, 1.5f * scale));
    ImGui::Begin("##perf", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
    // under the menu while it is open (the settings stay readable), else over everything
    if (p->under_menu) ImGui::BringWindowToDisplayBack(ImGui::GetCurrentWindow());
    else ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);   // the panel goes under the text, drawn once the window's size is known
    dl->ChannelsSetCurrent(1);
    ImGui::SetWindowFontScale(0.72f);
    // the port version and the game update first, so a screenshot in a bug report carries both
    ImGui::TextColored(kDimText, "%s", p->version);
    divider(scale);
    machine_and_frames(p, scale);
    ImGui::Dummy(ImVec2(0, 4 * scale));
    frame_plot(p->s, scale);
    ImGui::Dummy(ImVec2(0, 2 * scale));
    waits(p->s);
    divider(scale);
    picture(p);
    if (p->s.gpu_name[0] || p->s.cpu_name[0]) divider(scale);
    if (p->s.gpu_name[0]) ImGui::TextColored(kDimText, "%s", p->s.gpu_name);
    if (p->s.cpu_name[0]) ImGui::TextColored(kDimText, "%s", p->s.cpu_name);
    dl->ChannelsSetCurrent(0);
    ImVec2 a = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    skin_panel(dl, a, ImVec2(a.x + sz.x, a.y + sz.y), scale * 0.8f, 0.94f);
    dl->ChannelsMerge();
    ImGui::End();
}
