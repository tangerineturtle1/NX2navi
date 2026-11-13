// The plugins' drawing calls (plugin_api.h Nx2Draw) over Dear ImGui: text in the menu's font, rectangles and lines,
// put on the frame's background draw list, which lies under the settings menu's windows and under the performance
// overlay. ui/menu.cpp overlay_frame calls plugins_overlay_imgui between ImGui::NewFrame and ImGui::Render, on the
// present path; the calls work only during that call (g_list is NULL otherwise) and no ImGui type leaves this file.
#include <float.h>
#include <math.h>

#include "imgui.h"

#include "plugins.h"

extern "C" int gfx_picture_rect(float out[4]);   // gfx_present.c: where the game's picture sits in the window's

static ImDrawList *g_list;

static bool finite4(float a, float b, float c, float d) {
    return a - a == 0 && b - b == 0 && c - c == 0 && d - d == 0;   // false for an infinity or a NaN
}

static float text_px(float size) { return size > 0 && size - size == 0 ? (size < 512 ? size : 512) : ImGui::GetFontSize(); }

static void draw_text(float x, float y, float size, uint32_t rgba, const char *utf8) {
    if (!g_list || !utf8 || !finite4(x, y, 0, 0)) return;
    g_list->AddText(ImGui::GetFont(), text_px(size), ImVec2(x, y), rgba, utf8);
}

static void draw_text_size(float size, const char *utf8, float out_wh[2]) {
    if (!out_wh) return;
    out_wh[0] = out_wh[1] = 0;
    if (!g_list || !utf8) return;
    ImVec2 s = ImGui::GetFont()->CalcTextSizeA(text_px(size), FLT_MAX, 0, utf8);
    out_wh[0] = s.x, out_wh[1] = s.y;
}

static void draw_rect(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness) {
    if (!g_list || !finite4(x0, y0, x1, y1)) return;
    g_list->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), rgba, 0, 0, thickness > 0 ? thickness : 1);
}

static void draw_rect_filled(float x0, float y0, float x1, float y1, uint32_t rgba, float rounding) {
    if (!g_list || !finite4(x0, y0, x1, y1)) return;
    g_list->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), rgba, rounding > 0 ? rounding : 0);
}

static void draw_line(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness) {
    if (!g_list || !finite4(x0, y0, x1, y1)) return;
    g_list->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), rgba, thickness > 0 ? thickness : 1);
}

// The plugins' buttons on the game's title menu (plugins.c "title menu buttons"), drawn to sit with the game's own
// entries under them: centred white text of their size with a soft shadow, the description a little smaller after the
// label as the game writes its own remarks. The selected one gets what the game gives its selected entry: a dark band
// across the line that fades out at both ends, a small ornament at each end of it, and a blue glow around the text;
// it comes and goes over an eighth of a second, as the game's does. Positions are fractions of the game's picture,
// which may sit inside the window's with bars (gfx_picture_rect).
static void title_buttons(ImDrawList *list, float w, float h) {
    static float lit[NX2_TITLE_BUTTONS_MAX];   // how far each button's highlight has come, 0 to 1
    const char *label[NX2_TITLE_BUTTONS_MAX], *desc[NX2_TITLE_BUTTONS_MAX];
    float cy[NX2_TITLE_BUTTONS_MAX], half[NX2_TITLE_BUTTONS_MAX], text[NX2_TITLE_BUTTONS_MAX], r[4] = {0, 0, 1, 1};
    int sel = -1, n = plugins_title_buttons(NX2_TITLE_BUTTONS_MAX, label, desc, cy, half, text, &sel);
    if (!n) {
        for (float &v : lit) v = 0;
        return;
    }
    if (!gfx_picture_rect(r) || r[2] <= 0 || r[3] <= 0) r[0] = r[1] = 0, r[2] = r[3] = 1;
    float px = r[0] * w, py = r[1] * h, pw = r[2] * w, ph = r[3] * h, step = ImGui::GetIO().DeltaTime / 0.125f;
    ImFont *font = ImGui::GetFont();
    for (int i = 0; i < n; i++) {
        float t = lit[i] + (i == sel ? step : -step);
        t = lit[i] = t < 0 ? 0 : t > 1 ? 1 : t;
        float size = text[i] * ph, small = size * 0.8f, gap = desc[i][0] ? size * 0.3f : 0;
        ImVec2 a = font->CalcTextSizeA(size, FLT_MAX, 0, label[i]);
        ImVec2 b = desc[i][0] ? font->CalcTextSizeA(small, FLT_MAX, 0, desc[i]) : ImVec2(0, 0);
        float total = a.x + gap + b.x, mid = px + pw * 0.5f, x = mid - total * 0.5f, y = py + cy[i] * ph;
        ImVec2 at(x, y - a.y * 0.5f), dat(x + a.x + gap, y - a.y * 0.5f + (a.y - b.y) * 0.72f);
        if (t > 0) {
            // the band: even over the text and a little past it, then clear over three text heights
            float bh = half[i] * ph * 0.94f, core = total * 0.5f + size * 0.9f, end = core + size * 3.2f;
            ImU32 dark = IM_COL32(0, 0, 0, (int)(118 * t)), clear = IM_COL32(0, 0, 0, 0);
            list->AddRectFilled(ImVec2(mid - core, y - bh), ImVec2(mid + core, y + bh), dark);
            list->AddRectFilledMultiColor(ImVec2(mid - end, y - bh), ImVec2(mid - core, y + bh), clear, dark, dark, clear);
            list->AddRectFilledMultiColor(ImVec2(mid + core, y - bh), ImVec2(mid + end, y + bh), dark, clear, clear, dark);
            // the ornaments: a small diamond with a stroke towards the text, in the dull gold of the game's
            ImU32 gold = IM_COL32(176, 156, 96, (int)(215 * t));
            float o = size * 0.2f, line = size * 0.055f < 1 ? 1 : size * 0.055f;
            for (int side = -1; side <= 1; side += 2) {
                float ox = mid + (float)side * (core + size * 0.25f);
                list->AddQuad(ImVec2(ox - o, y), ImVec2(ox, y - o), ImVec2(ox + o, y), ImVec2(ox, y + o), gold, line);
                list->AddLine(ImVec2(ox - (float)side * o * 1.5f, y), ImVec2(ox - (float)side * o * 3.2f, y), gold, line);
                list->AddCircleFilled(ImVec2(ox + (float)side * o * 1.9f, y), line * 1.1f, gold);
            }
            // the glow: the text again around itself in blue, near and far
            for (int ring = 1; ring <= 2; ring++) {
                float rad = size * 0.075f * (float)ring;
                ImU32 blue = IM_COL32(84, 104, 255, (int)((ring == 1 ? 52 : 24) * t));
                for (int k = 0; k < 12; k++) {
                    float cx = rad * cosf((float)k * 0.5236f), sy = rad * sinf((float)k * 0.5236f);
                    list->AddText(font, size, ImVec2(at.x + cx, at.y + sy), blue, label[i]);
                    if (desc[i][0]) list->AddText(font, small, ImVec2(dat.x + cx, dat.y + sy), blue, desc[i]);
                }
            }
        }
        ImU32 shadow = IM_COL32(0, 0, 0, (int)(150 * (1 - t))), white = IM_COL32(255 - (int)(18 * t), 255 - (int)(14 * t), 255, 250);
        float d = size * 0.07f < 1 ? 1 : size * 0.07f;
        list->AddText(font, size, ImVec2(at.x + d, at.y + d), shadow, label[i]);
        list->AddText(font, size, at, white, label[i]);
        if (desc[i][0]) {
            list->AddText(font, small, ImVec2(dat.x + d, dat.y + d), shadow, desc[i]);
            list->AddText(font, small, dat, white, desc[i]);
        }
    }
}

extern "C" void plugins_overlay_imgui(float w, float h, float scale, int menu_open) {
    if (!plugins_overlay_wanted()) return;
    title_buttons(ImGui::GetBackgroundDrawList(), w, h);
    Nx2Draw d = {};
    d.size = sizeof d;
    d.menu_open = menu_open != 0;
    d.width = w, d.height = h, d.scale = scale;
    float pr[4] = {0, 0, 1, 1};
    if (!gfx_picture_rect(pr) || pr[2] <= 0 || pr[3] <= 0) pr[0] = pr[1] = 0, pr[2] = pr[3] = 1;
    d.picture_x = pr[0] * w, d.picture_y = pr[1] * h, d.picture_width = pr[2] * w, d.picture_height = pr[3] * h;
    d.text_height = ImGui::GetFontSize();
    d.text = draw_text, d.text_size = draw_text_size;
    d.rect = draw_rect, d.rect_filled = draw_rect_filled, d.line = draw_line;
    g_list = ImGui::GetBackgroundDrawList();
    plugins_overlay(&d);
    g_list = nullptr;
}
