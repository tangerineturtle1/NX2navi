// The smallest plugin for the game (docs/plugins.md): it draws a line of text with a frame counter over the picture,
// writes one log line from its first frame callback and reads the first bytes of the game's main module there. In a
// game with interface version 2 it also hooks one function of the game, the frame step of Ocarina of Time 1.1.1, to
// count its calls, adds a button to the game's title menu, logs the first presses of the game's buttons, and says
// goodbye when the game closes. In a game with version 3 the title menu button opens a page of two rows in the game's
// own settings screen (a switch for the text, and a button that closes the page); in an older one it shows and hides
// the text itself. In a game with version 4 it marks the point the game's camera looks at, with its distance.
// Build (the toolchain's clang; runtime/ holds plugin_api.h):
//   clang -O2 -shared -I runtime -o hello_plugin.dll tests/plugins/hello_plugin.c
// Run: set NX2NAVI_MOD_DLLS to the DLL's full path and start the game, or name the DLL in a mod's mod.json.
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>

#include "plugin_api.h"

// main + this offset is the function that advances the game by one frame in game 1.1.1 (build id e0f3ccfa...). An
// address belongs to one build of the game: check the version or the build id before using it.
#define FRAME_STEP_111 0x3b4e300ull

static const Nx2PluginApi *api;
static atomic_ullong frames;   // written by the frame callback (the game's thread), read by the overlay's (present)
static atomic_ullong steps;    // written by the hook (the thread that runs the game's frame step)
static atomic_int shown = 1;   // the overlay text shows; the title menu button "Hello Plugin" toggles it
static atomic_int pads;        // pad events logged so far
static _Thread_local char here;   // its address tells one thread from another in the log

static void on_frame(const Nx2Frame *frame, void *user) {
    (void)user;
    if (atomic_fetch_add(&frames, 1)) return;
    // the first frame: who we run in, and a harmless read of guest memory (main's first words, its module header)
    const Nx2GameInfo *g = api->game_info();
    uint32_t head[2] = {0, 0};
    int ok = api->read(g->main_base, head, sizeof head);
    api->log("hello: first frame callback at frame %llu on thread %p; game %u, runtime %s, build %.16s, main at %#llx",
             (unsigned long long)frame->index, (void *)&here, g->game_version, g->runtime_version, g->build_id,
             (unsigned long long)g->main_base);
    api->log("hello: read of main's first 8 bytes %s: %08x %08x; a read of address 0x10 gives %d", ok ? "worked" : "failed",
             head[0], head[1], api->read(0x10, head, 4));
    if (NX2_HAS(g, Nx2GameInfo, save_dir)) api->log("hello: profile folder %s, save folder %s", g->profile_dir, g->save_dir);
    if (NX2_HAS(api, Nx2PluginApi, map_range)) {
        // the same words without a call per access: the pointer is good until this callback returns
        const uint32_t *direct = api->map_range(g->main_base, sizeof head, 0);
        if (direct) api->log("hello: the same bytes read directly: %08x %08x", direct[0], direct[1]);
    }
}

// The hook: runs at every call of the game's frame step, on the thread that calls it, before the function. Returning
// lets the function run with the arguments in call->x. To run it here and go on after it: api->call_original(call).
// To leave it out, which holds the game still for a frame: call->skip = 1.
static void on_step(Nx2HookCall *call, void *user) {
    (void)user;
    unsigned long long n = atomic_fetch_add(&steps, 1);
    if (n == 0 || n == 600)
        api->log("hello: frame step call %llu on thread %p (x0 %#llx, called from %#llx); %llu frames presented so far",
                 n + 1, (void *)&here, (unsigned long long)call->x[0], (unsigned long long)call->lr,
                 (unsigned long long)atomic_load(&frames));
}

static void on_exit_game(void *user) {
    (void)user;
    api->log("hello: the game ends after %llu frames and %llu frame steps", (unsigned long long)atomic_load(&frames),
             (unsigned long long)atomic_load(&steps));
}

// What the player does on the menu page, on the game's thread: row 0 is the switch, row 1 the button.
static void on_menu(const Nx2MenuEvent *e, void *user) {
    (void)user;
    if (e->kind == NX2_MENU_CHANGE && e->row == 0) {
        atomic_store(&shown, e->value);
        api->log("hello: the menu's switch: the text is %s", e->value ? "shown" : "hidden");
    } else if (e->kind == NX2_MENU_ACTIVATE && e->row == 1) {
        api->close_menu();
    } else if (e->kind == NX2_MENU_CLOSED) {
        api->log("hello: the menu page closed");
    }
}

// The title menu button was chosen: called on the game's thread (guest_call works). The page is the game's settings
// screen with these two rows; when the game cannot show it (open_menu returns 0, or a game before version 3) the
// button does the switch's work itself. A plugin with a menu of its own would open that here instead
// (capture_input(1), on_pad and on_key for its input, on_overlay to draw it).
static void on_title_button(void *user) {
    (void)user;
    if (NX2_HAS(api, Nx2PluginApi, close_menu)) {
        const Nx2MenuRow rows[] = {
            {.size = sizeof *rows, .kind = NX2_ROW_TOGGLE, .label = "Show the plugin's text", .current = atomic_load(&shown),
             .description = "The line this plugin draws in the lower left corner of the picture."},
            {.size = sizeof *rows, .kind = NX2_ROW_ACTION, .label = "Done", .value = "Back",
             .description = "Closes this page, as the game's own back button does."},
        };
        const Nx2Menu menu = {.size = sizeof menu, .row_count = 2, .title = "Hello Plugin", .rows = rows, .on_event = on_menu};
        if (api->open_menu(&menu)) {
            api->log("hello: the title menu button was chosen on thread %p: its menu page opens", (void *)&here);
            return;
        }
    }
    int now = !atomic_load(&shown);
    atomic_store(&shown, now);
    api->log("hello: the title menu button was chosen on thread %p: the text is %s", (void *)&here, now ? "shown" : "hidden");
}

static void on_pad(const Nx2PadEvent *e, void *user) {
    (void)user;
    if (atomic_fetch_add(&pads, 1) < 4) api->log("hello: pad button %u %s", e->button, e->down ? "down" : "up");
}

static void on_overlay(const Nx2Draw *d, void *user) {
    (void)user;
    if (!atomic_load(&shown)) return;
    char text[128];
    unsigned long long n = atomic_load(&steps);
    if (n) snprintf(text, sizeof text, "hello from a plugin  -  frame %llu, step %llu", (unsigned long long)atomic_load(&frames), n);
    else snprintf(text, sizeof text, "hello from a plugin  -  frame %llu", (unsigned long long)atomic_load(&frames));
    float size = 28 * d->scale, pad = 10 * d->scale, wh[2];
    d->text_size(size, text, wh);
    float x = 24 * d->scale, y = d->height - 24 * d->scale - wh[1] - 2 * pad;
    d->rect_filled(x, y, x + wh[0] + 2 * pad, y + wh[1] + 2 * pad, NX2_RGBA(0, 0, 0, 170), 6 * d->scale);
    d->rect(x, y, x + wh[0] + 2 * pad, y + wh[1] + 2 * pad, NX2_RGBA(236, 196, 104, 255), 2 * d->scale);
    d->text(x + pad, y + pad, size, NX2_RGBA(255, 255, 255, 255), text);
    d->line(x, y - 4 * d->scale, x + wh[0] + 2 * pad, y - 4 * d->scale, NX2_RGBA(236, 196, 104, 255), 2 * d->scale);
    // Version 4: the scene's camera. world_to_screen places a point of the game's world in the overlay (inside the
    // game picture's rectangle); here the point is the one the camera looks at, which is near Link in play. Nothing
    // is drawn while there is no camera (the title screen, a loading screen).
    if (NX2_HAS(api, Nx2PluginApi, world_to_screen)) {
        Nx2Camera cam = {.size = sizeof cam};
        float xy[2];
        if (api->camera(&cam) && api->world_to_screen(cam.target, xy)) {
            float dx = cam.target[0] - cam.position[0], dy = cam.target[1] - cam.position[1], dz = cam.target[2] - cam.position[2];
            float r = 5 * d->scale;
            snprintf(text, sizeof text, "%.1f m", sqrtf(dx * dx + dy * dy + dz * dz));
            d->rect(xy[0] - r, xy[1] - r, xy[0] + r, xy[1] + r, NX2_RGBA(236, 196, 104, 255), 2 * d->scale);
            d->text(xy[0] + 2 * r, xy[1] - r, 0, NX2_RGBA(255, 255, 255, 255), text);
        }
    }
}

// Called once by the game before any of its own code runs (NX2NAVI_MOD_DLLS).
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
void nx2navi_plugin_init(const Nx2PluginApi *a) {
    if (api || !a || a->version < 1) return;
    api = a;
    api->log("hello: interface version %u, table of %u bytes", api->version, api->size);
    api->on_frame(on_frame, NULL);
    api->on_overlay(on_overlay, NULL);
    // What version 2 added is there when the table reaches it: this plugin also runs in a game with version 1.
    if (NX2_HAS(api, Nx2PluginApi, on_exit)) {
        api->on_exit(on_exit_game, NULL);
        const Nx2GameInfo *g = api->game_info();
        if (g->game_version == 111 && api->hook_function(g->main_base + FRAME_STEP_111, on_step, NULL))
            api->log("hello: hooked the frame step at main+%#llx", FRAME_STEP_111);
    }
    if (NX2_HAS(api, Nx2PluginApi, on_pad)) {
        Nx2TitleButton button = {.size = sizeof button, .label = "Hello Plugin", .description = "(its menu page)",
                                 .on_select = on_title_button};
        if (!api->add_title_button(&button)) api->log("hello: no title menu button in this game");
        api->on_pad(on_pad, NULL);
    }
}
