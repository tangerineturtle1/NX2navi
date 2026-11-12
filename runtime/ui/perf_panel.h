// The full performance overlay's panel (ui/perf_panel.cpp), drawn in the settings menu's look: what menu.cpp
// perf_full_draw gathered, laid out in tables on the menu's framed panel with the game's divider between the sections.
#pragma once
#include "imgui.h"

extern "C" {
#include "perfmon.h"
}

typedef struct {
    PerfStats s;              // perfmon_snapshot
    const char *version;      // "NX2navi <version>, game <update>"
    // window, game image, scene, then the settings as the menu names them: upscaling, anti-aliasing, frame limit, VSync,
    // widescreen, resolution, HDR, frame generation. An empty string leaves its line out.
    char picture[11][64];
    bool scene_known;         // the scene line is shown (resolution.c sets the scene's size)
    float fg_fps;             // frames the display gets with frame generation, 0 without it
    bool under_menu;          // the menu is open: the panel goes under its window
} PerfPanel;

// The panel at the top right corner of a width-pixel-wide frame.
void perf_panel_draw(const PerfPanel *p, float width, float scale);
