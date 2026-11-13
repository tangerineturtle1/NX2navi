// The runtime's side of the plugin interface (plugin_api.h, plugins.c): where the game's own code hands a frame, an
// overlay, a key or a typed character to the plugins. With no plugin registered each call costs one load.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "plugin_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Cpu Cpu;

// boot.c rt_boot, once the modules are loaded and the dispatch table is built, before the guest's main thread starts:
// loads the libraries NX2NAVI_MOD_DLLS names and calls each one's nx2navi_plugin_init.
void plugins_start(void);
// nvn_api_obj.c nvnQueuePresentTexture, on the game's thread with its core held: the frame callbacks. The guest's
// registers are as before the call afterwards, whatever the callbacks' guest calls left in them.
void plugins_frame(Cpu *c);
// ui/menu.cpp overlay_frame, on the present path: 1 when a plugin draws, so the overlay is laid out with the menu
// closed and the performance overlay off too.
int plugins_overlay_wanted(void);
// ui/plugin_draw.cpp, inside the overlay's Dear ImGui frame: makes the drawing calls for a picture of w x h pixels and
// runs the overlay callbacks through plugins_overlay.
void plugins_overlay_imgui(float w, float h, float scale, int menu_open);
void plugins_overlay(Nx2Draw *draw);   // fills draw->frame and calls every overlay callback with it
// The window's thread (ui/menu_win.cpp, ui/menu_sdl.cpp). plugins_key: 1 when a callback asked to keep the key from the
// menu's own keys. plugins_text: a typed character, delivered while a plugin has the input.
int plugins_key(unsigned key, int down, int repeat);
void plugins_text(unsigned codepoint);
int plugins_capturing(void);   // 1 while a plugin has the input (capture_input)
// main.c fnprof_hit, at the entry of a lifted function whose address the hooks' filter marks (cpu.h FN_ENTER), on the
// guest thread that called it: runs the function's hooks, and the function as they ask. 1 when that happened and the
// call is over, 0 when no hook is on this function and it runs as it is.
int plugins_fn_enter(Cpu *c, uint64_t addr);
// The same for a function known to be hooked, with the lifted function given (the probe passes a stand-in).
int plugins_hook_enter(Cpu *c, uint64_t addr, void (*body)(Cpu *));
// The orderly ways out of the process, before the saves are flushed: the exit callbacks, once per process.
void plugins_exit(void);
// input.c input_poll, on the game's thread at each read of its controls. plugins_pad: what the controls hold, before
// anything blocks it, for the pad callbacks. plugins_title_poll: what the game is about to get; moves the selection
// among the plugins' title menu buttons and zeroes the input while one of them has it.
void plugins_pad(uint64_t buttons, int32_t lx, int32_t ly);
void plugins_title_poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
int plugins_title_pointer(void);   // 1 while the pointer is free for the title menu buttons (input.c input_cursor_allowed)
// ui/plugin_draw.cpp: the title menu buttons to draw now, at most max; 0 while the title menu's entries do not show.
// Per button its texts, its centre line, half its height and its text's height as fractions of the game's picture;
// *selected is the selected one or -1.
int plugins_title_buttons(int max, const char **label, const char **desc, float *cy, float *half, float *text, int *selected);
// The text of the title menu's own entry as the game's message format has it (plugins.c "title menu buttons"): the
// label, the description smaller after it, and "<" ">" around them when the entry stands for one of several buttons.
// Returns the bytes written to out, without the terminator.
int plugins_title_text(char *out, size_t cap, const char *label, const char *desc, int several);
// The probe: plugins_title_force_drawn makes the buttons the drawn ones from now on; plugins_title_probe_decide runs the
// hook of the sequence's button node on a stand-in node whose chosen control is `control`, as if our entry showed.
void plugins_title_force_drawn(void);
void plugins_title_probe_decide(Cpu *c, uint64_t node, uint64_t control);
// 1 while a title button's callback runs from the game's own entry: where open_menu works.
int plugins_title_choosing(void);
// plugin_menu.c: a plugin's page in the game's settings screen (plugin_api.h open_menu, update_menu, close_menu).
// plugin_menu_armed: open_menu was called and the page has not opened yet. plugin_menu_poll: the input the game is about
// to get, while a page that was asked to close is still open.
int plugin_menu_open(const Nx2Menu *menu);
int plugin_menu_update(const Nx2Menu *menu);
void plugin_menu_close(void);
int plugin_menu_armed(void);
int plugin_menu_reopening(void);   // a page replaces the one that just closed: the title entry is pressed by plugin_menu.c
// plugin_camera.c: the scene's camera (plugin_api.h camera, world_to_screen). plugin_camera_overlay: the overlay about
// to be drawn, whose picture rectangle world_to_screen maps into.
int plugin_camera_get(Nx2Camera *out);
int plugin_camera_to_screen(const float world[3], float out_xy[2]);
void plugin_camera_overlay(const Nx2Draw *draw);
// plugin_api.h camera_reset, and input.c's poll through plugins_title_poll: the button the game is about to read.
int plugin_camera_reset(void);
void plugin_camera_poll(uint64_t *buttons);
void plugin_menu_poll(uint64_t *buttons);

// The executable's export (plugin_api.h NX2_PLUGIN_API_EXPORT): the table for an interface version, NULL when unserved.
// On Windows the attribute puts the name into the executable's export table, where a library finds it with
// GetProcAddress on the process's own module.
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
const Nx2PluginApi *nx2navi_plugin_api(uint32_t version);

int probe_plugins(void);       // plugins_probe.c: --probe-plugins

#ifdef __cplusplus
}
#endif
