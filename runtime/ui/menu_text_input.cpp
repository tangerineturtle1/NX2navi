// The window's text input follows the menu's text fields (ui/menu.h menu_text_input_follow). Its own file, so that a
// page test can run the same switch the menu does without the menu's Vulkan side.
#include "ui/menu.h"

static int g_text_on;   // what the window was last told; off at start, as the window makes it

void menu_text_input_follow(int want) {
    want = want != 0;
    if (want == g_text_on) return;
    g_text_on = want;
    gfx_window_text_input(want);
}
