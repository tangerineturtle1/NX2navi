// The debug menu's window (ui/debug_menu.cpp), laid out by ui/menu.cpp in the settings menu's place.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
void debug_menu_layout(uint32_t w, uint32_t h, float scale);   // between ImGui::NewFrame and Render, present thread
void debug_menu_tab(int tab);                                  // the tab to show at the next layout (test hook): 0 to 4
#define DEBUG_MENU_TABS 5
#ifdef __cplusplus
}
#endif
