// The look of the game's own menus (ui/menu_skin.h). With the game's textures (ui/menu_art.cpp) each piece is those
// textures tinted and laid over each other in nine slices; until they are decoded, or without them, it is plain shapes
// whose colours and proportions are sampled from the game: the save menu's Load and Cancel buttons (brown fill, light
// edge, an inset line about a twentieth of the height in with a diamond and a curl at alternate corners), the options
// menu's cursor (a 3 px white frame with a soft white-blue glow); without the textures the divider is a plain line with
// a diamond in its middle and a dot at each end, a shape of its own, not the game's ornament.
#include <float.h>
#include <math.h>
#include <stdlib.h>

#include <initializer_list>

#include "imgui.h"
#include "imgui_internal.h"   // NavCursorVisible
#include "ui/menu_art.h"
#include "ui/menu_skin.h"

namespace {

// The mottled grain of the game's brown boxes: a tile in the font atlas (skin_atlas_add, skin_atlas_fill), white with
// the grain in alpha, drawn over a box darkened in some places and lightened in others.
const int kGrainW = 160, kGrainH = 48;
int g_grain_rect = -1;
ImVec2 g_grain_uv0, g_grain_uv1;
bool g_grain_ok;

ImU32 rgba(int r, int g, int b, float a) { return IM_COL32(r, g, b, (int)(a * 255.0f + 0.5f)); }

ImU32 mix(const int c0[3], const int c1[3], float t, float a) {
    return rgba((int)(c0[0] + (c1[0] - c0[0]) * t), (int)(c0[1] + (c1[1] - c0[1]) * t), (int)(c0[2] + (c1[2] - c0[2]) * t),
                a);
}

// A spiral of `turns` turns around c, from radius r at angle a0 (radians, clockwise on screen) in to 35 % of it.
void curl(ImDrawList *dl, ImVec2 c, float r, float a0, float turns, ImU32 col, float thick) {
    const int n = 28;
    for (int i = 0; i <= n; i++) {
        float t = (float)i / n, a = a0 + t * turns * 6.2831853f, rad = r * (1.0f - 0.65f * t);
        dl->PathLineTo(ImVec2(c.x + cosf(a) * rad, c.y + sinf(a) * rad));
    }
    dl->PathStroke(col, 0, thick);
}

void diamond(ImDrawList *dl, ImVec2 c, float d, ImU32 col) {
    dl->AddQuadFilled(ImVec2(c.x, c.y - d), ImVec2(c.x + d, c.y), ImVec2(c.x, c.y + d), ImVec2(c.x - d, c.y), col);
}

// The inset line of the button and panel frames, `inset` inside [a, b]: a diamond at the top-left and bottom-right
// corners, a curl at the top-right and bottom-left ones, the four lines running between them with a small gap.
void ornament_line(ImDrawList *dl, ImVec2 a, ImVec2 b, float inset, float s, ImU32 col) {
    const float l = a.x + inset, t = a.y + inset, r = b.x - inset, bt = b.y - inset;
    // the ornaments grow with the box as on the game's buttons (a curl about a tenth of the height across), within limits
    const float span = fminf(r - l, bt - t);
    float rad = fminf(fmaxf(span * 0.13f, 3.5f * s), 7.0f * s), d = rad * 0.6f, gap = 2.4f * s, thick = 1.4f * s;
    // top: from the diamond into the curl, whose outer turn starts at the top of its circle
    dl->AddLine(ImVec2(l + d + gap, t), ImVec2(r - rad, t), col, thick);
    curl(dl, ImVec2(r - rad, t + rad), rad, -1.5707963f, 1.3f, col, thick);
    dl->AddLine(ImVec2(r, t + 2 * rad + gap), ImVec2(r, bt - d - gap), col, thick);     // right: under the curl
    dl->AddLine(ImVec2(r - d - gap, bt), ImVec2(l + rad, bt), col, thick);              // bottom: into the other curl
    curl(dl, ImVec2(l + rad, bt - rad), rad, 1.5707963f, 1.3f, col, thick);
    dl->AddLine(ImVec2(l, bt - 2 * rad - gap), ImVec2(l, t + d + gap), col, thick);     // left: over the curl
    diamond(dl, ImVec2(l, t), d, col);
    diamond(dl, ImVec2(r, bt), d, col);
}


// The settings menu's colours: a dark umber panel, warm browns for frames and what is selected, gold for the ornaments,
// accents and the cursor, parchment text. kGreen and kOcarina keep their names for the selected and active controls.
const ImVec4 kGold(0.93f, 0.76f, 0.38f, 1), kGreen(0.36f, 0.26f, 0.13f, 1), kNavy(0.075f, 0.058f, 0.042f, 0.95f),
    kOcarina(0.62f, 0.47f, 0.22f, 1);

// Where the art's boxes lie in its textures, in texels (measured from the textures). The button's layers are 489 x 133
// with the button's box from (20, 19) to (469, 114); the cursor's are 640 x 173 and its box, the button it surrounds,
// runs from (49, 48) to (591, 125); the panel's box runs from (53, 53) to (1147, 374) in 1200 x 427.
const float kBtnW = 489, kBtnH = 133, kBtnL = 20, kBtnT = 19, kBtnBoxH = 95, kBtnCut = 60;
const float kCurW = 640, kCurH = 173, kCurX = 49, kCurY = 48, kCurCut = 70, kCurScale = 0.72f;
const float kPanelW = 1200, kPanelH = 427, kPanelM = 53;
const float kTabL = 25, kTabT = 0, kTabR = 336, kTabB = 151;   // the selected tab's plate in its 360 x 173 texture

bool have_art(std::initializer_list<MenuArt> ids) {
    ImTextureID t;
    for (MenuArt id : ids)
        if (!menu_art(id, &t, nullptr, nullptr)) return false;
    return true;
}

enum { MAX_COPIES = 15 };
struct Piece { float p0, p1, t0, t1; };

// One axis of a slice: screen [a, b] from texels [x0, x1] of a texture `size` long, the outer `cut` texels c pixels
// long. The middle is stretched, or with tile repeated at k pixels per texel: an odd number of copies, every other one
// mirrored, so each copy meets the next on the same texels and the last meets the far corner. Mottled art stretched
// across a wide box would turn into streaks.
int axis(Piece *out, float a, float b, float x0, float x1, float size, float cut, float c, float k, bool tile) {
    const float m0 = a + c, m1 = b - c, t0 = (x0 + cut) / size, t1 = (x1 - cut) / size, unit = (x1 - x0 - 2 * cut) * k;
    int copies = 1, n = 0;
    if (tile && unit > 0) copies = 2 * (int)floorf(((m1 - m0) / unit - 1) * 0.5f + 0.5f) + 1;
    copies = copies < 1 ? 1 : copies > MAX_COPIES ? MAX_COPIES : copies;
    out[n++] = {a, m0, x0 / size, t0};
    for (int i = 0; i < copies; i++) {
        float w = (m1 - m0) / copies;
        out[n++] = {m0 + i * w, m0 + (i + 1) * w, i & 1 ? t1 : t0, i & 1 ? t0 : t1};
    }
    out[n++] = {m1, b, t1, x1 / size};
    return n;
}

// The texels (x0, y0)-(x1, y1) of an art over [a, b] in nine slices: the outer `cut` texels on each side at k screen
// pixels per texel, so corners keep their shape, the rest stretched or tiled. A box smaller than its two corners shrinks
// them.
void slice(ImDrawList *dl, MenuArt id, ImVec2 a, ImVec2 b, float x0, float y0, float x1, float y1, float cut, float k, ImU32 tint,
           bool tile_x = false, bool tile_y = false) {
    ImTextureID t;
    float tw, th;
    if (!menu_art(id, &t, &tw, &th)) return;
    Piece xs[MAX_COPIES + 2], ys[MAX_COPIES + 2];
    int nx = axis(xs, a.x, b.x, x0, x1, tw, cut, fminf(cut * k, (b.x - a.x) * 0.5f), k, tile_x);
    int ny = axis(ys, a.y, b.y, y0, y1, th, cut, fminf(cut * k, (b.y - a.y) * 0.5f), k, tile_y);
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++)
            dl->AddImage(t, ImVec2(xs[i].p0, ys[j].p0), ImVec2(xs[i].p1, ys[j].p1), ImVec2(xs[i].t0, ys[j].t0), ImVec2(xs[i].t1, ys[j].t1), tint);
}

// A button layer over the box [a, b]: the whole texture, reaching past the box by its margins.
void btn_layer(ImDrawList *dl, MenuArt id, ImVec2 a, ImVec2 b, float k, ImU32 tint, bool tile_x = false) {
    const float mx = kBtnL * k, my = kBtnT * k;
    slice(dl, id, ImVec2(a.x - mx, a.y - my), ImVec2(b.x + mx, b.y + my), 0, 0, kBtnW, kBtnH, kBtnCut, k, tint, tile_x);
}

// A cursor layer around the box [a, b], at a fixed size so its frame keeps its width on any box.
void cursor_layer(ImDrawList *dl, MenuArt id, ImVec2 a, ImVec2 b, float s, ImU32 tint) {
    const float k = kCurScale * s, mx = kCurX * k, my = kCurY * k;
    slice(dl, id, ImVec2(a.x - mx, a.y - my), ImVec2(b.x + mx, b.y + my), 0, 0, kCurW, kCurH, kCurCut, k, tint);
}

}  // namespace

// The cursor line's texture is not the line: its alpha climbs from 0 at the outer edge to full about 28 texels in, and
// is full inside. The white frame is a band of that ramp, about 3.5 texels wide just outside the box it surrounds
// (alpha 102 lies about six texels out from the cursor's box), so the frame follows the texture's own corners.
void skin_art_prepare(int art, uint8_t *rgba, uint32_t w, uint32_t h) {
    if (art != MENU_ART_CURSOR_LINE) return;
    const int mid = 102, full = 16, soft = 8;   // in alpha steps; the ramp climbs about 8.7 of them per texel
    for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
        int d = abs((int)rgba[i * 4 + 3] - mid) - full;
        rgba[i * 4 + 3] = d <= 0 ? 255 : d >= soft ? 0 : (uint8_t)(255 - d * 255 / soft);
    }
}

// A box filter over the 2 x 2 texels each destination texel covers (an odd last row or column repeats). Colour is
// weighted by alpha, so what is transparent does not bleed into the edges of the coloured art.
void skin_art_halve(const uint8_t *src, uint32_t w, uint32_t h, uint8_t *dst, uint32_t dw, uint32_t dh) {
    for (uint32_t y = 0; y < dh; y++)
        for (uint32_t x = 0; x < dw; x++) {
            const uint32_t xs[2] = {x * 2 < w ? x * 2 : w - 1, x * 2 + 1 < w ? x * 2 + 1 : w - 1},
                           ys[2] = {y * 2 < h ? y * 2 : h - 1, y * 2 + 1 < h ? y * 2 + 1 : h - 1};
            uint32_t rgb[3] = {0, 0, 0}, plain[3] = {0, 0, 0}, al = 0;
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    const uint8_t *p = src + ((size_t)ys[j] * w + xs[i]) * 4;
                    for (int c = 0; c < 3; c++) rgb[c] += p[c] * (uint32_t)p[3], plain[c] += p[c];
                    al += p[3];
                }
            uint8_t *d = dst + ((size_t)y * dw + x) * 4;
            for (int c = 0; c < 3; c++) d[c] = (uint8_t)(al ? (rgb[c] + al / 2) / al : (plain[c] + 2) / 4);
            d[3] = (uint8_t)((al + 2) / 4);
        }
}

extern "C" int menu_art_draw(ImDrawList *dl, MenuArt id, ImVec2 a, ImVec2 b, ImU32 tint) {
    ImTextureID t;
    if (!menu_art(id, &t, nullptr, nullptr)) return 0;
    dl->AddImage(t, a, b, ImVec2(0, 0), ImVec2(1, 1), tint);
    return 1;
}

extern "C" int menu_art_draw9(ImDrawList *dl, MenuArt id, ImVec2 a, ImVec2 b, float corner, float frac, ImU32 tint, int fill) {
    return menu_art_draw9_turned(dl, id, a, b, corner, frac, tint, fill, 0);
}

// Nine pieces: the corners at `corner` pixels on screen from the texture's outer `frac` of each side, the edges
// stretched between them, the middle left out (frames) or stretched (fill); the texture turned half a turn when turned,
// its top-left corner piece at the bottom right.
extern "C" int menu_art_draw9_turned(ImDrawList *dl, MenuArt id, ImVec2 a, ImVec2 b, float corner, float frac, ImU32 tint, int fill,
                                     int turned) {
    ImTextureID t;
    if (!menu_art(id, &t, nullptr, nullptr)) return 0;
    float c = corner, maxc = fminf((b.x - a.x) / 2, (b.y - a.y) / 2);
    if (c > maxc) c = maxc;
    const float xs[4] = {a.x, a.x + c, b.x - c, b.x}, ys[4] = {a.y, a.y + c, b.y - c, b.y};
    float us[4] = {0, frac, 1 - frac, 1}, vs[4] = {0, frac, 1 - frac, 1};
    if (turned)
        for (int k = 0; k < 4; k++) us[k] = 1 - us[k], vs[k] = 1 - vs[k];
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) {
            if (i == 1 && j == 1 && !fill) continue;
            dl->AddImage(t, ImVec2(xs[i], ys[j]), ImVec2(xs[i + 1], ys[j + 1]), ImVec2(us[i], vs[j]), ImVec2(us[i + 1], vs[j + 1]), tint);
        }
    return 1;
}

void skin_style(ImGuiStyle &s) {
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 10; s.FrameRounding = 6; s.GrabRounding = 6; s.TabRounding = 6; s.ChildRounding = 8;
    s.WindowPadding = ImVec2(40, 30); s.FramePadding = ImVec2(14, 8); s.ItemSpacing = ImVec2(14, 12);
    s.WindowBorderSize = 2; s.ScrollbarSize = 16;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg] = kNavy;
    c[ImGuiCol_Border] = ImVec4(kGold.x, kGold.y, kGold.z, 0.55f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = kNavy;
    c[ImGuiCol_Text] = ImVec4(0.96f, 0.92f, 0.82f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.62f, 0.56f, 0.46f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.115f, 0.08f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.18f, 0.10f, 1);
    c[ImGuiCol_FrameBgActive] = kGreen;
    c[ImGuiCol_SliderGrab] = kOcarina;
    c[ImGuiCol_SliderGrabActive] = kGold;
    c[ImGuiCol_CheckMark] = kGold;
    c[ImGuiCol_Button] = ImVec4(0.19f, 0.145f, 0.09f, 1);
    c[ImGuiCol_ButtonHovered] = kGreen;
    c[ImGuiCol_ButtonActive] = kOcarina;
    c[ImGuiCol_Header] = ImVec4(0.24f, 0.18f, 0.10f, 1);
    c[ImGuiCol_HeaderHovered] = kGreen;
    c[ImGuiCol_HeaderActive] = kOcarina;
    c[ImGuiCol_Tab] = ImVec4(0.13f, 0.10f, 0.07f, 1);
    c[ImGuiCol_TabHovered] = kGreen;
    c[ImGuiCol_TabSelected] = ImVec4(0.42f, 0.31f, 0.15f, 1);
    c[ImGuiCol_TabSelectedOverline] = kGold;
    c[ImGuiCol_NavCursor] = kGold;
    c[ImGuiCol_Separator] = ImVec4(kGold.x, kGold.y, kGold.z, 0.30f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.36f, 0.28f, 0.16f, 1);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.2f);
}

// The grain over [a, b]: the tile repeated at four pixels per texel (its noise wraps, so the copies join unseen), so the
// blotches keep their size on any box; darkening where the tile is dense, lightening where its mirror image is.
static void grain(ImDrawList *dl, ImVec2 a, ImVec2 b, float s, float strength) {
    if (!g_grain_ok) return;
    ImTextureID tex = ImGui::GetIO().Fonts->TexID;
    const float pw = kGrainW * 4.0f * s, ph = kGrainH * 4.0f * s;
    const ImVec2 du(g_grain_uv1.x - g_grain_uv0.x, g_grain_uv1.y - g_grain_uv0.y);
    const ImU32 dark = rgba(20, 12, 4, 0.30f * strength), light = rgba(255, 236, 196, 0.12f * strength);
    for (float y = a.y; y < b.y; y += ph)
        for (float x = a.x; x < b.x; x += pw) {
            float x1 = fminf(x + pw, b.x), y1 = fminf(y + ph, b.y);
            ImVec2 u0 = g_grain_uv0, u1(g_grain_uv0.x + du.x * (x1 - x) / pw, g_grain_uv0.y + du.y * (y1 - y) / ph);
            dl->AddImage(tex, ImVec2(x, y), ImVec2(x1, y1), u0, u1, dark);
            // the mirror image: the tile read from its far corner, over the same piece
            ImVec2 m0(g_grain_uv1.x, g_grain_uv1.y), m1(g_grain_uv1.x - du.x * (x1 - x) / pw, g_grain_uv1.y - du.y * (y1 - y) / ph);
            dl->AddImage(tex, ImVec2(x, y), ImVec2(x1, y1), m0, m1, light);
        }
}

void skin_atlas_add(ImFontAtlas *fa) { g_grain_rect = fa->AddCustomRectRegular(kGrainW, kGrainH), g_grain_ok = false; }

void skin_atlas_fill(ImFontAtlas *fa) {
    unsigned char *px;
    int w, h;
    fa->GetTexDataAsRGBA32(&px, &w, &h);
    const ImFontAtlasCustomRect *r = g_grain_rect >= 0 ? fa->GetCustomRectByIndex(g_grain_rect) : nullptr;
    if (!px || !r || !r->IsPacked()) return;
    // value noise of three octaves on a lattice that wraps, with a little fine speckle: blotches and grain
    auto lattice = [](int x, int y, int seed) {
        unsigned v = (unsigned)x * 374761393u + (unsigned)y * 668265263u + (unsigned)seed * 2246822519u;   // unsigned: the products wrap
        v = (v ^ (v >> 13)) * 1274126177u;
        return ((v ^ (v >> 16)) & 0xffff) / 65535.0f;
    };
    auto smooth = [&](float x, float y, int cell, int seed) {
        float gx = x / cell, gy = y / cell;
        int x0 = (int)floorf(gx), y0 = (int)floorf(gy);
        float fx = gx - x0, fy = gy - y0;
        fx = fx * fx * (3 - 2 * fx), fy = fy * fy * (3 - 2 * fy);
        int wx = kGrainW / cell, wy = kGrainH / cell;
        auto at = [&](int i, int j) { return lattice(((i % wx) + wx) % wx, ((j % wy) + wy) % wy, seed); };
        float top = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * fx;
        float bot = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * fx;
        return top + (bot - top) * fy;
    };
    for (int y = 0; y < kGrainH; y++)
        for (int x = 0; x < kGrainW; x++) {
            float n = 0.55f * smooth((float)x, (float)y, 16, 1) + 0.30f * smooth((float)x, (float)y, 8, 2) +
                      0.15f * smooth((float)x, (float)y, 4, 3);
            n = n * 0.85f + 0.15f * lattice(x, y, 9);
            float v = fminf(1.0f, fmaxf(0.0f, (n - 0.35f) * 1.8f));
            unsigned char *d = px + 4 * ((size_t)(r->Y + y) * w + r->X + x);
            d[0] = d[1] = d[2] = 255, d[3] = (unsigned char)(v * 255.0f + 0.5f);
        }
    fa->CalcCustomRectUV(r, &g_grain_uv0, &g_grain_uv1);
    g_grain_ok = true;
}

// The button from the game's layers; false while they are not there. plate: the selected tab's plate under the lines.
static bool button_art(ImDrawList *dl, ImVec2 a, ImVec2 b, float lift, bool dim, bool plate) {
    if (!have_art({MENU_ART_BTN_RIM, MENU_ART_BTN_FILL, MENU_ART_BTN_GRAIN, MENU_ART_BTN_EDGE, MENU_ART_BTN_LINE})) return false;
    static const int fill0[3] = {100, 76, 42}, fill1[3] = {128, 99, 58}, grain0[3] = {150, 118, 76}, grain1[3] = {178, 144, 96};
    static const int grey_fill[3] = {76, 67, 56}, grey_grain[3] = {128, 116, 100};
    const float t = (b.y - a.y) / kBtnBoxH;
    btn_layer(dl, MENU_ART_BTN_RIM, a, b, t, rgba(20, 14, 8, 0.85f));
    btn_layer(dl, MENU_ART_BTN_FILL, a, b, t, dim ? mix(grey_fill, grey_fill, 0, 1) : mix(fill0, fill1, lift, 1));
    btn_layer(dl, MENU_ART_BTN_GRAIN, a, b, t, dim ? mix(grey_grain, grey_grain, 0, 0.26f) : mix(grain0, grain1, lift, 0.32f), true);
    if (plate) slice(dl, MENU_ART_TAB_SELECT, a, b, kTabL, kTabT, kTabR, kTabB, 30, t, rgba(232, 186, 104, 0.55f));
    btn_layer(dl, MENU_ART_BTN_EDGE, a, b, t, dim ? rgba(98, 88, 74, 1) : rgba(140, 116, 74, 1));
    btn_layer(dl, MENU_ART_BTN_LINE, a, b, t, dim ? rgba(112, 102, 88, 1) : rgba(150 + (int)(24 * lift), 124 + (int)(20 * lift), 80 + (int)(12 * lift), 1));
    return true;
}

void skin_button(ImDrawList *dl, ImVec2 a, ImVec2 b, float s, float lift, bool dim) {
    static const int top0[3] = {98, 75, 41}, top1[3] = {122, 95, 55}, bot0[3] = {84, 63, 32}, bot1[3] = {106, 81, 44};
    static const int grey_top[3] = {66, 58, 48}, grey_bot[3] = {56, 49, 40};
    lift = lift < 0 ? 0 : lift > 1 ? 1 : lift;
    if (button_art(dl, a, b, lift, dim, false)) return;
    const float k = 1.0f * s;
    dl->AddRectFilled(a, b, rgba(28, 22, 14, 0.92f));   // the dark rim between the button and what is behind it
    ImVec2 ia(a.x + k, a.y + k), ib(b.x - k, b.y - k);
    ImU32 top = dim ? mix(grey_top, grey_top, 0, 1) : mix(top0, top1, lift, 1);
    ImU32 bot = dim ? mix(grey_bot, grey_bot, 0, 1) : mix(bot0, bot1, lift, 1);
    dl->AddRectFilledMultiColor(ia, ib, top, top, bot, bot);
    grain(dl, ia, ib, s, dim ? 0.5f : 1.0f);
    dl->AddRect(ia, ib, dim ? rgba(98, 88, 74, 1) : rgba(126, 104, 68, 1), 0, 0, 1.4f * s);   // the light edge
    float h = b.y - a.y, inset = fmaxf(4.0f * s, h * 0.1f);
    ornament_line(dl, a, b, inset, s, dim ? rgba(104, 94, 80, 1) : rgba(134 + (int)(18 * lift), 111 + (int)(14 * lift),
                                                                         70 + (int)(8 * lift), 1));
}

float skin_cursor_reach(float s) { return 3.0f * s + 2.5f * s + 12.0f * s; }

// one slow breath every 1.6 s, as the game's cursor pulses
static float breath(void) { return 0.5f - 0.5f * cosf((float)ImGui::GetTime() * 6.2831853f / 1.6f); }

void skin_cursor_glow(ImDrawList *dl, ImVec2 a, ImVec2 b, float s, float alpha) {
    if (have_art({MENU_ART_CURSOR_GLOW})) {
        cursor_layer(dl, MENU_ART_CURSOR_GLOW, a, b, s, rgba(164, 194, 240, alpha * (0.78f + 0.22f * breath())));
        return;
    }
    // filled rounded boxes stacked from the outside in, each faint, so the glow brightens towards the frame; the box
    // drawn over them hides their middle
    const float ring = 3.0f * s + 1.25f * s, br = breath();
    const int n = 12;
    for (int i = n; i >= 1; i--) {
        float o = ring + i * 0.95f * s;
        dl->AddRectFilled(ImVec2(a.x - o, a.y - o), ImVec2(b.x + o, b.y + o), rgba(160, 190, 236, alpha * (0.055f + 0.03f * br)),
                          2.0f * s + i * 0.9f * s);
    }
}

void skin_cursor_frame(ImDrawList *dl, ImVec2 a, ImVec2 b, float s, float alpha) {
    if (have_art({MENU_ART_CURSOR_LINE})) {
        cursor_layer(dl, MENU_ART_CURSOR_LINE, a, b, s, rgba(240, 242, 245, alpha));
        return;
    }
    const float gap = 3.0f * s, thick = 2.5f * s, ring = gap + thick * 0.5f;
    // a little glow in the gap between the box and the frame, then the white frame
    dl->AddRect(ImVec2(a.x - gap * 0.5f, a.y - gap * 0.5f), ImVec2(b.x + gap * 0.5f, b.y + gap * 0.5f),
                rgba(190, 210, 240, alpha * (0.30f + 0.15f * breath())), gap * 0.4f, 0, gap);
    dl->AddRect(ImVec2(a.x - ring, a.y - ring), ImVec2(b.x + ring, b.y + ring), rgba(236, 238, 241, alpha), 2.0f * s, 0, thick);
}

float skin_filigree_height(float s) { return 26.0f * s; }

// The divider from the game's art: the ornament (its ink runs from (28, 24) to (200, 65) in 228 x 80) and on each side
// the line texture, turned round for the left one: its stroke from x = 38 to 217 stretched, then the dot up to x = 232
// at its own proportions, the whole on the row y = 23 of 240 x 43.
static bool filigree_art(ImDrawList *dl, float x0, float x1, float y, float s, float alpha) {
    ImTextureID mark, stroke;
    float mw, mh, lw, lh;
    if (!menu_art(MENU_ART_DIVIDER, &mark, &mw, &mh) || !menu_art(MENU_ART_DIVIDER_LINE, &stroke, &lw, &lh)) return false;
    const ImU32 orn = rgba(196, 164, 112, alpha), line = rgba(150, 126, 90, alpha);
    const float orn_w = fminf(84.0f * s, (x1 - x0) * 0.5f), k = orn_w / 172.0f, cx = (x0 + x1) * 0.5f;
    dl->AddImage(mark, ImVec2(cx - 114 * k, y - 44.5f * k), ImVec2(cx - 114 * k + mw * k, y - 44.5f * k + mh * k), ImVec2(0, 0), ImVec2(1, 1), orn);
    const float lk = 0.42f * s, top = y - 9 * lk, bot = y + 9 * lk, v0 = 14 / lh, v1 = 32 / lh, dot = (232 - 217) * lk;
    const float from = cx + orn_w * 0.5f + 5 * s, to = x1 - 4 * s;
    if (to - dot <= from) return true;
    const float u0 = 38 / lw, u1 = 217 / lw, u2 = 232 / lw, mirror = 2 * cx;
    dl->AddImage(stroke, ImVec2(from, top), ImVec2(to - dot, bot), ImVec2(u0, v0), ImVec2(u1, v1), line);
    dl->AddImage(stroke, ImVec2(to - dot, top), ImVec2(to, bot), ImVec2(u1, v0), ImVec2(u2, v1), line);
    dl->AddImage(stroke, ImVec2(mirror - (to - dot), top), ImVec2(mirror - from, bot), ImVec2(u1, v0), ImVec2(u0, v1), line);
    dl->AddImage(stroke, ImVec2(mirror - to, top), ImVec2(mirror - (to - dot), bot), ImVec2(u2, v0), ImVec2(u1, v1), line);
    return true;
}

void skin_filigree(ImDrawList *dl, float x0, float x1, float y, float s, float alpha) {
    if (filigree_art(dl, x0, x1, y, s, alpha)) return;
    const ImU32 line = rgba(96, 80, 56, alpha), dot = rgba(110, 92, 64, alpha), orn = rgba(138, 114, 76, alpha);
    float orn_w = fminf(78.0f * s, (x1 - x0) * 0.5f), k = orn_w / 108.0f, cx = (x0 + x1) * 0.5f;
    float dot_r = 1.5f * s, gap = 5.0f * s;
    dl->AddCircleFilled(ImVec2(x0 + 4 * s + dot_r, y), dot_r, dot);
    dl->AddCircleFilled(ImVec2(x1 - 4 * s - dot_r, y), dot_r, dot);
    dl->AddLine(ImVec2(x0 + 4 * s + 2 * dot_r + gap, y), ImVec2(cx - orn_w * 0.5f - gap, y), line, 1.0f * s);
    dl->AddLine(ImVec2(cx + orn_w * 0.5f + gap, y), ImVec2(x1 - 4 * s - 2 * dot_r - gap, y), line, 1.0f * s);
    // the middle: a diamond outline with a small filled one inside, and a short stroke on each side of it
    float d = 9.0f * k * 1.08f, inner = 3.2f * k * 1.08f, t = 1.6f * k;
    dl->AddQuad(ImVec2(cx, y - d), ImVec2(cx + d, y), ImVec2(cx, y + d), ImVec2(cx - d, y), orn, t);
    dl->AddQuadFilled(ImVec2(cx, y - inner), ImVec2(cx + inner, y), ImVec2(cx, y + inner), ImVec2(cx - inner, y), orn);
    dl->AddLine(ImVec2(cx - orn_w * 0.5f, y), ImVec2(cx - d - 3 * k, y), orn, t);
    dl->AddLine(ImVec2(cx + d + 3 * k, y), ImVec2(cx + orn_w * 0.5f, y), orn, t);
}

void skin_panel(ImDrawList *dl, ImVec2 a, ImVec2 b, float s, float alpha) {
    if (have_art({MENU_ART_PANEL_FILL, MENU_ART_BTN_EDGE, MENU_ART_BTN_LINE})) {
        // the panel's own texture inside its box, with the button's edge and inset line at a size that suits a large box
        const float k = 0.62f * s;
        slice(dl, MENU_ART_PANEL_FILL, a, b, kPanelM, kPanelM, kPanelW - kPanelM, kPanelH - kPanelM, 40, k, rgba(255, 255, 255, alpha), true, true);
        btn_layer(dl, MENU_ART_BTN_EDGE, a, b, k, rgba(126, 104, 68, 1));
        btn_layer(dl, MENU_ART_BTN_LINE, a, b, k, rgba(150, 124, 80, 1));
        return;
    }
    dl->AddRectFilled(a, b, rgba(20, 15, 10, 0.85f * alpha));
    ImVec2 ia(a.x + 1.0f * s, a.y + 1.0f * s), ib(b.x - 1.0f * s, b.y - 1.0f * s);
    dl->AddRectFilledMultiColor(ia, ib, rgba(44, 35, 25, alpha), rgba(44, 35, 25, alpha), rgba(33, 26, 19, alpha),
                                rgba(33, 26, 19, alpha));
    grain(dl, ia, ib, s, 0.6f * alpha);
    dl->AddRect(ia, ib, rgba(110, 90, 58, 1), 0, 0, 1.4f * s);
    ornament_line(dl, a, b, 7.0f * s, s, rgba(122, 101, 66, 1));
}

bool skin_widget_button(const char *label, ImVec2 size, float s) {
    ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
    const ImGuiStyle &st = ImGui::GetStyle();
    ImVec2 sz(size.x > 0 ? size.x : ts.x + st.FramePadding.x * 2, size.y > 0 ? size.y : ts.y + st.FramePadding.y * 2);
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + sz.x, a.y + sz.y);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));   // the cursor below shows the selection
    bool pressed = ImGui::InvisibleButton(label, sz, ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
    bool held = ImGui::IsItemActive();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (foc || hov) skin_cursor_glow(dl, a, b, s, foc ? 1.0f : 0.55f);   // under the button
    skin_button(dl, a, b, s, held ? 0.4f : foc ? 1.0f : hov ? 0.7f : 0.15f, false);
    const char *end = ImGui::FindRenderedTextEnd(label);
    dl->AddText(ImVec2(floorf((a.x + b.x - ts.x) * 0.5f), floorf((a.y + b.y - ts.y) * 0.5f)), SKIN_TEXT, label, end);
    if (foc || hov) skin_cursor_frame(dl, a, b, s, foc ? 1.0f : 0.55f);
    return pressed;
}

bool skin_tab_item(const char *id, const char *text, int flags, float s, ImFont *font) {
    // Dear ImGui keeps the bar's behaviour (selecting, sizing, the D-pad) and lays out the tab with everything it would
    // draw made invisible; the button and the text go over its rectangle here.
    static const ImGuiCol hide[] = {ImGuiCol_Tab, ImGuiCol_TabHovered, ImGuiCol_TabSelected, ImGuiCol_TabSelectedOverline, ImGuiCol_TabDimmed,
                                    ImGuiCol_TabDimmedSelected, ImGuiCol_TabDimmedSelectedOverline, ImGuiCol_Text, ImGuiCol_NavCursor};
    for (ImGuiCol c : hide) ImGui::PushStyleColor(c, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(12.0f * s, ImGui::GetStyle().ItemInnerSpacing.y));
    if (font) ImGui::PushFont(font);
    const bool shown = ImGui::BeginTabItem(id, nullptr, flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor((int)(sizeof hide / sizeof *hide));
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    if (b.x > a.x && b.y > a.y) {
        b.y -= 4.0f * s;   // clear of the line under the tabs
        const bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        if (foc) skin_cursor_glow(dl, a, b, s, 1.0f);
        const float lift = shown ? 1.0f : hov ? 0.6f : 0.0f;
        if (!button_art(dl, a, b, lift, false, shown)) skin_button(dl, a, b, s, lift, false);
        const ImVec2 ts = ImGui::CalcTextSize(text, nullptr, true);
        dl->AddText(ImVec2(floorf((a.x + b.x - ts.x) * 0.5f), floorf((a.y + b.y - ts.y) * 0.5f)), shown ? SKIN_GOLD : SKIN_TEXT, text);
        if (foc) skin_cursor_frame(dl, a, b, s, 1.0f);
    }
    if (font) ImGui::PopFont();
    return shown;
}

void skin_boot_panel(ImDrawList *dl, ImFont *head, float head_px, ImFont *body, float body_px, const char *title,
                     const char *step, float frac, float sweep, float w, float h, float s) {
    const float pad = 38 * s, gap = 16 * s, bar_h = 12 * s, fh = skin_filigree_height(s);
    ImVec2 ts = head->CalcTextSizeA(head_px, FLT_MAX, 0, title), ss = body->CalcTextSizeA(body_px, FLT_MAX, 0, step);
    float pw = fminf(fmaxf(fmaxf(ts.x, ss.x), 560 * s) + pad * 2, w - 24 * s);
    float ph = pad + ts.y + fh + gap * 0.4f + ss.y + gap * 0.7f + bar_h + pad;
    ImVec2 a(floorf((w - pw) / 2), floorf((h - ph) / 2)), b(a.x + pw, a.y + ph);
    skin_panel(dl, a, b, s);
    float y = a.y + pad;
    dl->AddText(head, head_px, ImVec2(floorf((w - ts.x) / 2), y), SKIN_GOLD, title);
    y += ts.y;
    skin_filigree(dl, a.x + pad, b.x - pad, y + fh * 0.5f, s);
    y += fh + gap * 0.4f;
    dl->AddText(body, body_px, ImVec2(a.x + pad, y), SKIN_TEXT, step);
    y += ss.y + gap * 0.7f;
    // the bar: a sunken track with the frames' brown edge, filled in gold from the left
    ImVec2 ba(a.x + pad, y), bb(b.x - pad, y + bar_h);
    dl->AddRectFilled(ba, bb, rgba(12, 9, 6, 0.85f), 2 * s);
    float x0 = ba.x + 2 * s, x1 = bb.x - 2 * s, top = ba.y + 2 * s, bot = bb.y - 2 * s;
    auto fill = [&](float from, float to) {
        if (to - from < 1) return;
        dl->AddRectFilledMultiColor(ImVec2(from, top), ImVec2(to, bot), rgba(246, 214, 128, 1), rgba(246, 214, 128, 1),
                                    rgba(196, 148, 66, 1), rgba(196, 148, 66, 1));
        dl->AddLine(ImVec2(from, top + 0.5f * s), ImVec2(to, top + 0.5f * s), rgba(255, 240, 196, 0.6f), 1.0f * s);
    };
    if (frac >= 0) {
        fill(x0, x0 + (x1 - x0) * fminf(frac, 1.0f));
    } else {
        float span = (x1 - x0) * 0.2f, run = (x1 - x0) - span, at = sweep < 0 ? 0 : sweep > 1 ? 1 : sweep;
        fill(x0 + run * at, x0 + run * at + span);
    }
    dl->AddRect(ba, bb, rgba(110, 90, 58, 1), 2 * s, 0, 1.3f * s);
}
