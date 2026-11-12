// The settings menu's art from the game's own interface textures (ui/menu_art.cpp).
#pragma once
#include "imgui.h"

typedef enum {
    MENU_ART_FRAME_DECO,    // a frame with ornamented corners
    MENU_ART_GUIDE_BAR,     // the control guide's bar
    MENU_ART_WINDOW_BASE,   // a rounded window base
    // the layers of the save menu's Load and Cancel button, in drawing order; white masks except for the grain
    MENU_ART_BTN_RIM,       // a soft line along the button's edge, reaching outside it
    MENU_ART_BTN_FILL,      // the button's shape
    MENU_ART_BTN_GRAIN,     // the mottling: light at the edge, dark and speckled in the middle
    MENU_ART_BTN_EDGE,      // the light edge
    MENU_ART_BTN_LINE,      // the inset line, a diamond and a curl at alternate corners
    MENU_ART_CURSOR_GLOW,   // the options cursor's soft glow
    MENU_ART_CURSOR_LINE,   // the options cursor's white frame (skin_art_prepare cuts it out of the texture)
    MENU_ART_PANEL_FILL,    // a dark brown panel
    MENU_ART_DIVIDER,       // the options menu's divider ornament
    MENU_ART_DIVIDER_LINE,  // the line beside the ornament, ending in a dot
    MENU_ART_TAB_SELECT,    // the plate of the selected category in the game's menu header
    MENU_ART_COUNT
} MenuArt;

extern "C" void menu_art_start(void);    // decodes the textures on a worker, once
// The texture for the menu's draw lists and its size in pixels; 0 while it is not there (still decoding, or absent).
// Window thread: the first call for an art uploads it.
extern "C" int menu_art(MenuArt id, ImTextureID *tex, float *w, float *h);
extern "C" void menu_art_release(void);  // before ImGui's Vulkan back end shuts down; the font atlas goes too
// The font atlas's pixels (w x h, RGBA) as a texture for the draw lists in *tex; 0 when it could not be made, the previous
// atlas then kept. The previous one is freed once the queue has finished with it.
extern "C" int menu_font_upload(const unsigned char *rgba, int w, int h, ImTextureID *tex);
// Results other than VK_SUCCESS that Dear ImGui's Vulkan back end reported so far (ui/menu.cpp menu_vk_check).
extern "C" int menu_vk_errors(void);
// Draws an art stretched over [a, b] with a tint; 0 (nothing drawn) while it is not there.
extern "C" int menu_art_draw(ImDrawList *dl, MenuArt id, ImVec2 a, ImVec2 b, ImU32 tint);
// Draws an art as nine pieces: corners `corner` pixels on screen from the outer `frac` of the texture, edges stretched,
// the middle too when fill.
extern "C" int menu_art_draw9(ImDrawList *dl, MenuArt id, ImVec2 a, ImVec2 b, float corner, float frac, ImU32 tint, int fill);
// The same with the texture turned half a turn (turned 1): a top-left corner piece drawn at the bottom right.
extern "C" int menu_art_draw9_turned(ImDrawList *dl, MenuArt id, ImVec2 a, ImVec2 b, float corner, float frac, ImU32 tint, int fill,
                                     int turned);
