// The plugins' drawing calls (plugin_api.h Nx2Draw) over Dear ImGui: text in the menu's font or a font of the plugin's,
// rectangles, lines and pictures, put on the frame's background draw list, which lies under the settings menu's
// windows and under the performance overlay. ui/menu.cpp overlay_frame calls plugins_overlay_imgui between
// ImGui::NewFrame and ImGui::Render, on the present path; the calls work only during that call (g_list is NULL
// otherwise) and no ImGui type leaves this file. Also here: where a plugin's pictures and fonts come from (the
// interface's texture_create, texture_load, font_load), with the PNG reader; ui/menu_art.cpp keeps the pictures and
// ui/menu.cpp the fonts.
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"

extern "C" {
#include "host.h"
#include "menu_assets.h"
#include "platform/platform.h"
#include "save_zip.h"
}
#include "plugins.h"

extern "C" int gfx_picture_rect(float out[4]);   // gfx_present.c: where the game's picture sits in the window's
// ui/menu_art.cpp: the plugins' pictures
extern "C" int plugin_tex_create(uint32_t w, uint32_t h, uint8_t *rgba, uint32_t flags);
extern "C" void plugin_tex_free(int handle);
extern "C" int plugin_tex_get(int handle, ImTextureID *tex);
extern "C" void plugin_tex_sweep(void);
// ui/menu.cpp: the plugins' fonts
int plugin_font_add(uint8_t *data, size_t n, float px, int exact, const char **why);
void plugin_font_remove(int handle);
ImFont *plugin_font_get(int handle, float scale, float *px);

static ImDrawList *g_list;
static float g_scale = 1;   // of the overlay being drawn (Nx2Draw.scale)

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

static void draw_image(int texture, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                       uint32_t rgba) {
    ImTextureID t;
    if (!g_list || !finite4(x0, y0, x1, y1) || !finite4(u0, v0, u1, v1) || !plugin_tex_get(texture, &t)) return;
    g_list->AddImage(t, ImVec2(x0, y0), ImVec2(x1, y1), ImVec2(u0, v0), ImVec2(u1, v1), rgba);
}

// The font and the size in pixels for a text_font call: the plugin's font when the atlas holds it, at the size asked
// for or, for 0, the one its letters are made at for this overlay's scale. A size within half a percent of the one its letters
// were made at is taken as that one, so that pixel_size * scale worked out by the plugin does not stretch them. Any
// other handle, and a font the atlas does not hold yet, gives the menu's font.
static ImFont *font_of(int font, float size, float *px) {
    float nominal = 0;
    ImFont *f = plugin_font_get(font, g_scale, &nominal);
    bool given = size > 0 && size - size == 0;
    *px = given ? (size < 512 ? size : 512) : nominal > 0 ? nominal : ImGui::GetFontSize();
    if (!f) return ImGui::GetFont();
    if (fabsf(*px - f->FontSize) < f->FontSize * 0.005f) *px = f->FontSize;
    return f;
}

static void draw_text_font(int font, float x, float y, float size, uint32_t rgba, uint32_t shadow_rgba, const char *utf8) {
    if (!g_list || !utf8 || !finite4(x, y, 0, 0)) return;
    float px;
    ImFont *f = font_of(font, size, &px);
    if (shadow_rgba) {
        float d = floorf(px * 0.07f + 0.5f);
        if (d < 1) d = 1;
        g_list->AddText(f, px, ImVec2(x + d, y + d), shadow_rgba, utf8);
    }
    g_list->AddText(f, px, ImVec2(x, y), rgba, utf8);
}

static void draw_text_font_size(int font, float size, const char *utf8, float out_wh[2]) {
    if (!out_wh) return;
    out_wh[0] = out_wh[1] = 0;
    if (!g_list || !utf8) return;
    float px;
    ImFont *f = font_of(font, size, &px);
    ImVec2 s = f->CalcTextSizeA(px, FLT_MAX, 0, utf8);
    out_wh[0] = s.x, out_wh[1] = s.y;
}

// ---- where pictures and fonts come from (any thread)

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// A PNG file as RGBA8, the top row first: every colour type at 1 to 16 bits a sample (16 bits keep their high byte),
// a palette's and a colour key's transparency (tRNS); no Adam7 interlace. The checksums are not checked: the inflate
// fails on damaged data, and a wrong picture from a damaged file harms nothing.
extern "C" uint8_t *plugin_png_decode(const uint8_t *d, size_t n, uint32_t *ow, uint32_t *oh, const char **why) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    *why = "it is not a PNG file";
    if (!d || n < 8 + 25 || memcmp(d, sig, 8)) return nullptr;
    uint32_t w = 0, h = 0, depth = 0, ctype = 0, npal = 0, ntrns = 0;
    uint8_t pal[768] = {}, trns[256] = {};
    uint8_t *z = (uint8_t *)malloc(n), *raw = nullptr, *out = nullptr;
    size_t zn = 0;
    auto fail = [&](const char *m) -> uint8_t * {
        free(z), free(raw), free(out);
        *why = m;
        return nullptr;
    };
    if (!z) return fail("out of memory");
    for (size_t at = 8;;) {
        if (at + 12 > n) return fail("the PNG file is cut off");
        uint32_t len = be32(d + at);
        const uint8_t *type = d + at + 4, *body = d + at + 8;
        if (len > n - at - 12) return fail("the PNG file is cut off");
        if (!memcmp(type, "IHDR", 4)) {
            if (len < 13 || w) return fail("the PNG file is damaged");
            w = be32(body), h = be32(body + 4), depth = body[8], ctype = body[9];
            if (body[10] || body[11]) return fail("the PNG file uses a compression this reader does not know");
            if (body[12]) return fail("the PNG file is interlaced (Adam7): save it without interlacing");
        } else if (!w) {
            return fail("the PNG file is damaged");
        } else if (!memcmp(type, "PLTE", 4)) {
            npal = len / 3 > 256 ? 256 : len / 3;
            memcpy(pal, body, npal * 3);
        } else if (!memcmp(type, "tRNS", 4)) {
            ntrns = len > 256 ? 256 : len;
            memcpy(trns, body, ntrns);
        } else if (!memcmp(type, "IDAT", 4)) {
            memcpy(z + zn, body, len);
            zn += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        at += 12 + (size_t)len;
    }
    uint32_t ch = ctype == 0 || ctype == 3 ? 1 : ctype == 2 ? 3 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    bool depth_ok = depth == 8 || (depth == 16 && ctype != 3) || ((depth == 1 || depth == 2 || depth == 4) && ch == 1);
    if (!ch || !depth_ok) return fail("the PNG file has a colour type or bit depth that does not exist");
    if (!w || !h || w > NX2_TEXTURE_SIDE_MAX || h > NX2_TEXTURE_SIDE_MAX) return fail("the picture is empty or has a side above 4096 pixels");
    size_t bpp = (ch * depth + 7) / 8, rb = ((size_t)w * ch * depth + 7) / 8, raw_n = (rb + 1) * h;
    raw = (uint8_t *)malloc(raw_n);
    out = (uint8_t *)malloc((size_t)w * h * 4);
    if (!raw || !out) return fail("out of memory");
    // the data is a zlib stream: two bytes of header, the deflate data, four bytes of checksum
    if (zn < 6 || (z[0] & 15) != 8 || (z[1] & 32) || sz_inflate(z + 2, zn - 6, raw, raw_n)) return fail("the PNG file's pixel data is damaged");
    for (uint32_t y = 0; y < h; y++) {   // each row back from its filter: what was stored is the difference to its neighbours
        uint8_t *cur = raw + (size_t)y * (rb + 1) + 1, *prev = y ? cur - (rb + 1) : nullptr;
        uint8_t ft = cur[-1];
        if (ft > 4) return fail("the PNG file's pixel data is damaged");
        for (size_t i = 0; ft && i < rb; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0, b = prev ? prev[i] : 0, c = prev && i >= bpp ? prev[i - bpp] : 0, add;
            if (ft == 1) add = a;
            else if (ft == 2) add = b;
            else if (ft == 3) add = (a + b) / 2;
            else {
                int pa = abs(b - c), pb = abs(a - c), pc = abs(a + b - 2 * c);
                add = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
            }
            cur[i] = (uint8_t)(cur[i] + add);
        }
    }
    uint32_t maxv = (1u << depth) - 1;
    auto sample = [&](const uint8_t *r, uint32_t x, uint32_t c) -> uint32_t {
        if (depth == 8) return r[(size_t)x * ch + c];
        if (depth == 16) {
            const uint8_t *q = r + ((size_t)x * ch + c) * 2;
            return (uint32_t)q[0] << 8 | q[1];
        }
        uint32_t bit = x * depth;
        return (uint32_t)(r[bit >> 3] >> (8 - depth - (bit & 7))) & maxv;
    };
    auto to8 = [&](uint32_t v) -> uint8_t { return (uint8_t)(depth == 16 ? v >> 8 : depth == 8 ? v : v * 255 / maxv); };
    auto key = [&](uint32_t i) -> uint32_t { return (uint32_t)trns[i * 2] << 8 | trns[i * 2 + 1]; };
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *r = raw + (size_t)y * (rb + 1) + 1;
        uint8_t *o = out + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; x++, o += 4) {
            if (ctype == 3) {
                uint32_t i = sample(r, x, 0);
                const uint8_t *e = pal + (i < npal ? i : 0) * 3;
                o[0] = e[0], o[1] = e[1], o[2] = e[2], o[3] = i < ntrns ? trns[i] : 255;
            } else if (ch <= 2) {
                uint32_t g = sample(r, x, 0);
                o[0] = o[1] = o[2] = to8(g);
                o[3] = ch == 2 ? to8(sample(r, x, 1)) : ntrns >= 2 && g == key(0) ? 0 : 255;
            } else {
                uint32_t cr = sample(r, x, 0), cg = sample(r, x, 1), cb = sample(r, x, 2);
                o[0] = to8(cr), o[1] = to8(cg), o[2] = to8(cb);
                o[3] = ch == 4 ? to8(sample(r, x, 3)) : ntrns >= 6 && cr == key(0) && cg == key(1) && cb == key(2) ? 0 : 255;
            }
        }
    }
    free(z), free(raw);
    *ow = w, *oh = h;
    return out;
}

static const char *short_path(const char *path) {   // the end of a long path, for a log line
    size_t n = strlen(path);
    return n > 160 ? path + n - 160 : path;
}

// The pixels become a picture's (ui/menu_art.cpp takes them); the log says when no slot is free.
static int tex_take(uint32_t w, uint32_t h, uint8_t *rgba, uint32_t flags) {
    int t = plugin_tex_create(w, h, rgba, flags);
    if (!t) {
        free(rgba);
        rt_log("plugins: no picture made: all %d are in use (texture_free gives one back)", NX2_TEXTURES_MAX);
    }
    return t;
}

extern "C" int plugin_texture_create(uint32_t width, uint32_t height, const void *rgba, uint32_t flags) {
    if (!rgba || !width || !height || width > NX2_TEXTURE_SIDE_MAX || height > NX2_TEXTURE_SIDE_MAX) {
        rt_log("plugins: texture_create refused: %ux%u pixels at %p (a side is 1 to %d pixels)", width, height, rgba,
               NX2_TEXTURE_SIDE_MAX);
        return 0;
    }
    size_t n = (size_t)width * height * 4;
    uint8_t *copy = (uint8_t *)malloc(n);
    if (!copy) {
        rt_log("plugins: texture_create refused: no memory for %ux%u pixels", width, height);
        return 0;
    }
    memcpy(copy, rgba, n);
    return tex_take(width, height, copy, flags);
}

extern "C" int plugin_texture_load(const char *path, uint32_t flags, uint32_t out_wh[2]) {
    if (out_wh) out_wh[0] = out_wh[1] = 0;
    if (!path || !path[0]) return 0;
    uint8_t *rgba = nullptr;
    uint32_t w = 0, h = 0;
    const char *why = "the file is missing or cannot be read";
    if (!strncmp(path, "romfs:", 6)) {   // "romfs:<archive>#<texture>": one of the game's own
        const char *hash = strrchr(path, '#');
        char rel[512];
        size_t n = 0;
        why = "the name is not romfs:<archive>#<texture>";
        if (hash && hash[1] && (size_t)(hash - path - 6) < sizeof rel && hash > path + 6) {
            memcpy(rel, path + 6, (size_t)(hash - path - 6));
            rel[hash - path - 6] = 0;
            uint8_t *bntx = menu_asset_load(rel, &n);
            why = "the game's files hold no such archive";
            if (bntx) {
                why = "the archive holds no texture of that name, or one in a format the runtime does not read";
                if (!menu_asset_texture(bntx, n, hash + 1, &rgba, &w, &h)) rgba = nullptr;
                else if (!w || !h || w > NX2_TEXTURE_SIDE_MAX || h > NX2_TEXTURE_SIDE_MAX) free(rgba), rgba = nullptr, why = "the texture has a side above 4096 pixels";
                free(bntx);
            }
        }
    } else {
        uint64_t n = 0;
        uint8_t *file = plat_file_read(path, &n);
        if (file) rgba = plugin_png_decode(file, (size_t)n, &w, &h, &why);
        free(file);
    }
    if (!rgba) {
        rt_log("plugins: texture_load of %s refused: %s", short_path(path), why);
        return 0;
    }
    int t = tex_take(w, h, rgba, flags);
    if (t && out_wh) out_wh[0] = w, out_wh[1] = h;
    return t;
}

extern "C" void plugin_texture_free(int texture) { plugin_tex_free(texture); }

extern "C" int plugin_font_load(const char *path, float pixel_size, uint32_t flags) {
    if (!path || !path[0]) return 0;
    if (!(pixel_size >= 4 && pixel_size <= 400)) {
        rt_log("plugins: font_load of %s refused: a size of %g (4 to 400 pixels)", short_path(path), (double)pixel_size);
        return 0;
    }
    uint8_t *data;
    size_t n = 0;
    if (!strncmp(path, "romfs:", 6)) {
        data = menu_asset_font(path + 6, &n);
    } else {
        uint64_t n64 = 0;
        data = plat_file_read(path, &n64);
        n = (size_t)n64;
    }
    const char *why = "the file is missing or cannot be read";
    int f = data ? plugin_font_add(data, n, pixel_size, (flags & NX2_FONT_EXACT) != 0, &why) : 0;
    if (!f) rt_log("plugins: font_load of %s refused: %s", short_path(path), why);
    return f;
}

extern "C" void plugin_font_free(int font) { plugin_font_remove(font); }

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
    plugin_tex_sweep();   // pictures given up since the last overlay: nothing of this frame draws with them yet
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
    d.image = draw_image, d.text_font = draw_text_font, d.text_font_size = draw_text_font_size;
    g_scale = scale;
    g_list = ImGui::GetBackgroundDrawList();
    plugins_overlay(&d);
    g_list = nullptr;
}
