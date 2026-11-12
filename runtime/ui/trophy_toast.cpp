// The trophy unlock toast (ui/trophy_toast.h). trophies.c hands live unlocks out through trophies_unlocked_next;
// this file keeps them in line and shows one at a time: a panel in the settings menu's look (ui/menu_skin.cpp) that
// slides in from the right edge at the top right corner, under the performance overlay, holds a few seconds and
// slides back out — the timing and slide of a console's own unlock notification. Silent grants (a save already past
// the event) never reach the queue, so no storm of toasts greets a loaded file.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "imgui.h"

#include "menu_sound.h"
#include "ui/menu_skin.h"
#include "ui/trophy_toast.h"

extern "C" {
#include "platform/platform.h"
#include "trophies.h"
#include "ui_text.h"
}

// The line of unlocks the overlay has not shown yet (trophies.c's own queue is small and only feeds this one).
static int g_wait[16];
static int g_wh, g_wt;

// The toast on screen: a copy of the trophy's text (the info call's strings are the defs', and stay valid) and the
// moment it started, which drives the slide and the hold.
static char g_title[96], g_desc[128], g_pts[32];
static uint64_t g_start;   // 0: nothing showing

enum { IN_MS = 280, HOLD_MS = 4200, OUT_MS = 350 };

bool trophy_toast_tick(void) {
    int i;
    while ((i = trophies_unlocked_next()) >= 0 && g_wt - g_wh < (int)(sizeof(g_wait) / sizeof(g_wait[0])))
        g_wait[g_wt++ % 16] = i;
    return g_start != 0 || g_wh != g_wt;
}

static float ease(float t) {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return 1 - (1 - t) * (1 - t) * (1 - t);   // cubic ease-out
}

void trophy_toast_draw(float w, float h, float top, float scale) {
    (void)h;
    const uint64_t now = plat_now_ms();
    if (!g_start && g_wh != g_wt) {   // promote the next in line
        const TrophyInfo *t = trophies_info(g_wait[g_wh++ % 16]);
        if (t) {
            snprintf(g_title, sizeof g_title, "%s", t->title);
            snprintf(g_desc, sizeof g_desc, "%s", t->desc);
            snprintf(g_pts, sizeof g_pts, menu_text(UI_MENU_TROPHY_PTS), t->points);
            g_start = now;
            menu_sound_play(MENU_SOUND_DECIDE);
        }
    }
    if (!g_start) return;
    const uint64_t age = now - g_start;
    if (age > IN_MS + HOLD_MS + OUT_MS) {   // done: the next one starts next frame
        g_start = 0;
        return;
    }
    const float slide = age < IN_MS ? ease((float)age / IN_MS)
                      : age > IN_MS + HOLD_MS ? 1 - ease((float)(age - IN_MS - HOLD_MS) / OUT_MS) : 1.0f;
    const float fade = age > IN_MS + HOLD_MS ? slide : 1.0f;

    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImFont *font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const ImVec2 label_sz = ImGui::CalcTextSize(menu_text(UI_MENU_TROPHY_TOAST));
    const ImVec2 title_sz = font->CalcTextSizeA(fs * 1.12f, FLT_MAX, 0, g_title);
    const ImVec2 pts_sz = ImGui::CalcTextSize(g_pts);
    const ImVec2 desc_sz = font->CalcTextSizeA(fs * 0.86f, FLT_MAX, 0, g_desc);

    const float pad = 14 * scale, icon = 34 * scale, gap = 12 * scale, margin = 12 * scale;
    const float text_w = fmaxf(fmaxf(title_sz.x, desc_sz.x), label_sz.x + pts_sz.x + 8 * scale);
    const float panel_w = pad * 2 + icon + gap + text_w;
    const float panel_h = pad * 2 + label_sz.y + 4 * scale + title_sz.y + 4 * scale + desc_sz.y;
    // in from, and out to, the right edge; on a window narrower than the panel it stops at the margin rather than
    // running off the left edge
    float x = w - margin - panel_w + (1 - slide) * (panel_w + margin);
    if (x < margin) x = margin;
    const float y = top + 8 * scale;

    skin_panel(dl, ImVec2(x, y), ImVec2(x + panel_w, y + panel_h), scale * 0.6f, 0.92f * fade);

    // the badge: a gold diamond, the trophy mark of the set (a per-trophy icon can come later)
    const ImVec2 c(x + pad + icon * 0.5f, y + pad + icon * 0.5f);
    const ImVec2 dia[4] = {ImVec2(c.x, c.y - icon * 0.5f), ImVec2(c.x + icon * 0.5f, c.y),
                           ImVec2(c.x, c.y + icon * 0.5f), ImVec2(c.x - icon * 0.5f, c.y)};
    const ImU32 gold = (SKIN_GOLD & ~IM_COL32_A_MASK) | ((unsigned)(255 * fade) << IM_COL32_A_SHIFT);
    const ImU32 text = (SKIN_TEXT & ~IM_COL32_A_MASK) | ((unsigned)(255 * fade) << IM_COL32_A_SHIFT);
    const ImU32 dim = (SKIN_TEXT_DIM & ~IM_COL32_A_MASK) | ((unsigned)(255 * fade) << IM_COL32_A_SHIFT);
    dl->AddConvexPolyFilled(dia, 4, gold);

    float tx = x + pad + icon + gap, ty = y + pad;
    dl->AddText(ImVec2(tx, ty), gold, menu_text(UI_MENU_TROPHY_TOAST));
    dl->AddText(ImVec2(tx + text_w - pts_sz.x, ty), dim, g_pts);
    ty += label_sz.y + 4 * scale;
    dl->AddText(font, fs * 1.12f, ImVec2(tx, ty), text, g_title);
    ty += title_sz.y + 4 * scale;
    dl->AddText(font, fs * 0.86f, ImVec2(tx, ty), dim, g_desc);
}
