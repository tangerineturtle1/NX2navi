"""Keep the built-in cheat reachable through the actual settings tab loop."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CXX = os.environ.get("CXX", "clang++")


def block(text, start):
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


class BuiltinCheatMenuTest(unittest.TestCase):
    def test_root_tab_loop_exposes_and_dispatches_cheats(self):
        source = (ROOT / "runtime/ui/menu.cpp").read_text(encoding="utf-8")
        enum = re.search(r"enum\s*\{\s*TAB_SCREEN\b[^}]*\}", source).group()
        names = re.search(r"static const UiTextId kTabNames\[TAB_COUNT\]\s*=\s*\{[^}]*\}", source).group()
        self.assertIn("TAB_CHEATS", enum, "The root Settings menu does not register the Cheats tab")
        self.assertIn("UI_MENU_TAB_CHEATS", names, "The registered Cheats tab needs its localized title")
        loop = block(source, source.index("for (int k = 0; k < ntabs; k++)"))
        shown = block(source, source.index("static int tabs_shown(void)"))
        self.assertIn("tab_cheats(scale)", loop, "The root tab loop never renders the Cheats page")
        if not shutil.which(CXX):
            self.skipTest("C++ compiler required to execute the root settings tab loop")
        harness = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include "imgui.h"
#include "ui_text.h"
''' + enum + ";\n" + names + r''';
static int selected, seen, cheat_calls, sibling_calls, focused, popup_calls, beta;
static int g_tab, g_tab_request = -1, g_focus_page = -1;
static bool rebuild_requested, restart_rebuild, g_confirm_quit = true;
static ImFont *g_title_font;
const char *menu_text(UiTextId id) {
    switch (id) {
    case UI_MENU_TAB_SCREEN: return "Screen";
    case UI_MENU_TAB_GAME: return "Game";
    case UI_MENU_TAB_SOUND: return "Sound";
    case UI_MENU_TAB_CONTROLS: return "Controls";
    case UI_MENU_TAB_CHEATS: return "Cheats";
    case UI_MENU_TAB_COOP: return "Co-op";
    default: assert(false); return "";
    }
}
static bool skin_tab_item(const char *id, const char *label, ImGuiTabItemFlags, float scale, ImFont *) {
    assert(scale == 1 && *id && *label);
    int index = seen++;
    if (index == TAB_CHEATS) assert(!strcmp(label, "Cheats"));
    return index == selected;
}
static void opt_focus_first() { focused++; }
static void opt_reserve_bottom(float value) { assert(value == 1); }
static void tab_screen(float) { sibling_calls++; }
static void tab_game(float) { sibling_calls++; }
static void tab_sound(float) { sibling_calls++; }
static void tab_controls(float) { sibling_calls++; }
static void tab_coop(float) { sibling_calls++; }
static bool tab_cheats(float value) { assert(value == 1); cheat_calls++; return rebuild_requested; }
// A native built-in row must be reachable without any imported cheats.
static int cheats_count() { return 0; }
namespace ImGui {
void Spacing() {}
void EndTabItem() {}
void OpenPopup(const char *id, ImGuiPopupFlags) { assert(!strcmp(id, "##confirm")); popup_calls++; }
}
static int cfg_bool(const char *key) { assert(!strcmp(key, "NX2NAVI_COOP_BETA")); return beta; }
''' + shown + r'''
static void draw() {
    float scale = 1, bottom = 1;
    const int ntabs = tabs_shown();
    seen = 0;
''' + loop + r'''
}
int main() {
    assert(TAB_COUNT == 6 && TAB_CHEATS < TAB_COOP && TAB_COOP == TAB_COUNT - 1);
    assert(kTabNames[TAB_CHEATS] == UI_MENU_TAB_CHEATS);
    for (beta = 0; beta < 2; beta++) {
        cheat_calls = sibling_calls = 0;
        assert(tabs_shown() == (beta ? TAB_COUNT : TAB_COOP));
        for (selected = 0; selected < tabs_shown(); selected++) {
            draw(); assert(seen == tabs_shown() && g_tab == selected);
        }
        assert(cheat_calls == 1 && sibling_calls == tabs_shown() - 1 && popup_calls == 0);
    }
    beta = 0;
    selected = g_tab_request = g_focus_page = TAB_CHEATS;
    draw();
    assert(cheat_calls == 2 && focused == 1 && g_focus_page == -1 && popup_calls == 0);
    rebuild_requested = true;
    draw();
    assert(cheat_calls == 3 && popup_calls == 1 && restart_rebuild && !g_confirm_quit);
}
'''
        with tempfile.TemporaryDirectory(prefix="builtin-cheat-menu-") as directory:
            cpp, exe = Path(directory) / "menu.cpp", Path(directory) / "menu.exe"
            cpp.write_text(harness, encoding="utf-8")
            subprocess.run([CXX, "-std=gnu++17", "-I", str(ROOT / "runtime"),
                            "-I", str(ROOT / "runtime/third_party/imgui"), str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_cheats_title_included_in_title_font_glyphs(self):
        source = (ROOT / "runtime/ui/menu.cpp").read_text(encoding="utf-8")
        titles = re.search(r"static const UiTextId titles\[\]\s*=\s*\{[^}]*\}", source).group()
        self.assertIn("UI_MENU_TAB_CHEATS", titles)


if __name__ == "__main__":
    unittest.main()
