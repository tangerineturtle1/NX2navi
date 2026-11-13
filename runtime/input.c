// Host input for nn::hid (runtime/ov_input.c): an SDL3 gamepad (no SDL window needed), the keyboard and the mouse,
// merged into Switch npad button bits and stick axes. Button order follows HidNpadButton. The key and pad
// bindings are the tables in input_map.c (nx2navi.ini overrides them). The keyboard and the mouse come from the window
// system's back end (input_wsys.h: input_win.c with Win32, input_sdl.c with SDL), which also owns the cursor capture.
// Keys and their map: docs/controls.md.
// The gamepad also rumbles (input_rumble; a Switch controller's linear actuators can take the game's own values instead,
// "HD rumble" below) and, when it has a gyro and an accelerometer, feeds the six-axis sensor from a thread of its own
// (input_motion_start). Every SDL call on the gamepad is made under `lock`.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "input_hd_pair.h"
#include "input_map.h"
#include "input_wsys.h"
#ifdef _WIN32
#include "input_ds3.h"
#endif
#include "nvn_api.h"
#include "ui/menu.h"
#include "vk.h"
#include "platform/platform.h"

enum { STICK_MAX = 32767, STICK_DIR = 16384, TRIGGER_ON = 16384, RESCAN_MS = 1000 };

static PlatMutex lock = PLAT_MUTEX_INIT;
static int state;                    // 0 not started, 1 running, -1 disabled or SDL failed
static volatile int32_t g_block, g_menu;   // text entry / host menu: 0 off, 1 open, 2 waiting for release
// The device the player used last (input_kbm_last): 1 keyboard and mouse, 0 the gamepad. A pad counts once a button is
// down or a stick is past half way (a drifting stick never switches), the mouse once it moved MOUSE_USED raw counts
// within one poll (a slow deliberate move does; a desk bump does not), the keyboard with any key or mouse button.
enum { PAD_USED = 16384, MOUSE_USED = 6 };
static volatile int32_t g_kbm_last = 1;
int input_kbm_last(void) { return __atomic_load_n(&g_kbm_last, __ATOMIC_RELAXED); }
static SDL_Gamepad *pad;
static uint64_t next_scan;
static int pad_rumble;               // the open gamepad has rumble motors
static volatile int32_t pad_labels[4];  // ... and the labels of its south, east, west, north buttons (PAD_LABEL_*), any thread
static int pad_east_a;               // ... and labels its east face button A (Nintendo layout under NX2NAVI_PAD_LAYOUT=auto)
static int pad_layout = -1;          // PAD_LAYOUT_* in effect for the open gamepad, -1 to pick again; live (cfg_version)
static unsigned pad_layout_seen;
static volatile int32_t pad_layout_shown = -1;   // pad_layout for the menu (any thread), -1 without a gamepad
static SDL_JoystickID sensor_pad;    // the gamepad whose sensors the motion thread set up, 0 none
static int sensor_ok;                // ... and it has both sensors, enabled
static int motion_thread;            // 0 not started, 1 running, -1 off (NX2NAVI_MOTION=0, no SDL)

// SDL buttons of the gamepad inputs (input_map.h PAD_*), by position; PAD_ZL and PAD_ZR are the triggers (axes).
static const SDL_GamepadButton pad_sdl[PAD_COUNT] = {
    [PAD_SOUTH] = SDL_GAMEPAD_BUTTON_SOUTH, [PAD_EAST] = SDL_GAMEPAD_BUTTON_EAST, [PAD_WEST] = SDL_GAMEPAD_BUTTON_WEST,
    [PAD_NORTH] = SDL_GAMEPAD_BUTTON_NORTH, [PAD_LSTICK] = SDL_GAMEPAD_BUTTON_LEFT_STICK,
    [PAD_RSTICK] = SDL_GAMEPAD_BUTTON_RIGHT_STICK, [PAD_L] = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    [PAD_R] = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, [PAD_START] = SDL_GAMEPAD_BUTTON_START, [PAD_BACK] = SDL_GAMEPAD_BUTTON_BACK,
    [PAD_DPAD_LEFT] = SDL_GAMEPAD_BUTTON_DPAD_LEFT, [PAD_DPAD_UP] = SDL_GAMEPAD_BUTTON_DPAD_UP,
    [PAD_DPAD_RIGHT] = SDL_GAMEPAD_BUTTON_DPAD_RIGHT, [PAD_DPAD_DOWN] = SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    [PAD_LPADDLE1] = SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, [PAD_RPADDLE1] = SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,
    [PAD_LPADDLE2] = SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, [PAD_RPADDLE2] = SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2,
};

// NX2NAVI_INPUT_SCRIPT=<file>: lines "<frame> <button> [hold]" or "<frame> LSTICK|RSTICK <x> <y> [hold]", ORed into the real input.
// <frame> and its hold are 60 Hz ticks of game time since the first present (frame_step_ticks: presents at 60 fps), so
// the title menus get the presses at the same game time at any frame limit. A line "@<ms> ..." is timed instead: <ms>
// and its hold are milliseconds of the world clock (frame_step_world_us, game time since the loaded save's first
// frame), so it lands at the same game time at any frame rate and load length. A timed event shorter than a frame is
// applied once. In a script with timed lines (a recording) the untimed ones belong before the world clock: they stop
// when it starts, as the load ends at another tick in each run. "<frame> MARK <name>" presses nothing: with
// NX2NAVI_TRACE=perf it ends the running segment of frame times and starts one called <name> (perf.c perf_segment).
// at, hold: ticks, or microseconds of the world clock when timed.
typedef struct { uint64_t at, hold; int bit, stick, timed, mark; int32_t x, y; int logged; char text[48]; } ScriptEv;
static const struct { const char *name; int bit; } script_buttons[] = {
    {"A", B_A}, {"B", B_B}, {"X", B_X}, {"Y", B_Y}, {"LCLICK", B_STICK_L}, {"RCLICK", B_STICK_R}, {"L", B_L}, {"R", B_R},
    {"ZL", B_ZL}, {"ZR", B_ZR}, {"PLUS", B_PLUS}, {"MINUS", B_MINUS}, {"LEFT", B_LEFT}, {"UP", B_UP}, {"RIGHT", B_RIGHT}, {"DOWN", B_DOWN},
};
static ScriptEv *script_ev;
static int script_n, script_state;   // 0 not loaded, 1 loaded
static int script_mixed;             // the script has timed lines: its untimed ones end where the world clock starts
static int32_t script_mouse[2];      // one-shot raw counts, consumed by the camera hook; under lock

static void script_load(void) {
    script_state = 1;
    const char *path = cfg_get("NX2NAVI_INPUT_SCRIPT");
    if (!path || !*path) return;
    FILE *f = fopen(path, "r");
    if (!f) { rt_log("input: cannot open NX2NAVI_INPUT_SCRIPT %s", path); return; }
    char line[256];
    int cap = 0, ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char name[32];
        double at;
        int off = 0, timed = line[0] == '@';
        if (sscanf(line + timed, "%lf %31s%n", &at, name, &off) < 2 || at < 0) continue;
        off += timed;
        double unit = timed ? 1000.0 : 1.0;   // ms to us, or presents
        ScriptEv e = {.at = (uint64_t)(at * unit + 0.5), .hold = timed ? 66667 : 4, .bit = -1, .timed = timed};
        long a, b;
        double c;
        if (!strcmp(name, "MARK")) {
            const char *s = line + off;
            while (*s == ' ' || *s == '\t') s++;
            size_t len = strcspn(s, "\r\n");
            while (len && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
            e.mark = 1, e.hold = 1;
            snprintf(e.text, sizeof e.text, "%.*s", (int)len, s);
        } else if (!strcmp(name, "MOUSE") || !strcmp(name, "LSTICK") || !strcmp(name, "RSTICK")) {
            int n = sscanf(line + off, "%ld %ld %lf", &a, &b, &c);
            if (n < 2) { rt_log("input: script line %d: %s needs x y [hold]", ln, name); continue; }
            e.stick = name[0] == 'M' ? 3 : name[0] == 'L' ? 1 : 2; e.x = (int32_t)a; e.y = (int32_t)b;
            if (e.stick == 3) e.hold = 1;   // a displacement, never a held speed
            if (n >= 3 && c > 0) e.hold = (uint64_t)(c * unit + 0.5);
            snprintf(e.text, sizeof e.text, "%s %d %d", name, e.x, e.y);
        } else {
            for (size_t i = 0; i < sizeof script_buttons / sizeof *script_buttons; i++)
                if (!strcmp(name, script_buttons[i].name)) e.bit = script_buttons[i].bit;
            if (e.bit < 0) { rt_log("input: script line %d: unknown button %s", ln, name); continue; }
            if (sscanf(line + off, "%lf", &c) == 1 && c > 0) e.hold = (uint64_t)(c * unit + 0.5);
            snprintf(e.text, sizeof e.text, "%s", name);
        }
        if (script_n == cap) {
            cap = cap ? cap * 2 : 32;
            script_ev = realloc(script_ev, (size_t)cap * sizeof *script_ev);
            if (!script_ev) { script_n = 0; fclose(f); return; }
        }
        script_ev[script_n++] = e;
        script_mixed |= timed;
    }
    fclose(f);
    rt_log("input: script %s, %d events", path, script_n);
}

static void script_apply(uint64_t *b, int32_t sl[2], int32_t sr[2]) {
    if (!script_state) script_load();
    if (!script_n) return;
    uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), tick = frame_step_ticks();
    int64_t w = frame_step_world_us();
    for (int i = 0; i < script_n; i++) {
        ScriptEv *e = &script_ev[i];
        if (e->timed ? w < 0 : script_mixed && w >= 0) continue;
        uint64_t now = e->timed ? (uint64_t)w : tick;
        if (now < e->at || (now >= e->at + e->hold && (e->logged || !e->timed))) continue;
        if ((e->stick == 3 || e->mark) && e->logged) continue;
        if (!e->logged) {
            e->logged = 1;
            if (e->timed) rt_log("input: script frame %llu at %.1f ms: %s", (unsigned long long)fr, w / 1000.0, e->text);
            else rt_log("input: script frame %llu tick %llu: %s", (unsigned long long)fr, (unsigned long long)tick, e->text);
        }
        if (e->mark) perf_segment(e->text);
        else if (e->stick == 3) {
            int64_t x = (int64_t)script_mouse[0] + e->x, y = (int64_t)script_mouse[1] + e->y;
            script_mouse[0] = (int32_t)(x < INT32_MIN ? INT32_MIN : x > INT32_MAX ? INT32_MAX : x);
            script_mouse[1] = (int32_t)(y < INT32_MIN ? INT32_MIN : y > INT32_MAX ? INT32_MAX : y);
        } else if (e->stick) { int32_t *s = e->stick == 1 ? sl : sr; s[0] = e->x; s[1] = e->y; }
        else *b |= 1ull << e->bit;
    }
}

// NX2NAVI_INPUT_RECORD=<file>: writes the merged input as NX2NAVI_INPUT_SCRIPT lines, one per button press and per stick
// segment, so a played session replays as a script. A stick segment ends when an axis moves by more than REC_STICK_STEP.
// An event that begins on the world clock is written timed ("@<ms>", hold in ms), one before it in ticks
// (frame_step_ticks). When the world clock starts, held buttons and sticks are cut there and go on as timed events.
enum { REC_STICK_STEP = 2048 };
static FILE *rec;
static int rec_state;                // 0 not opened, 1 recording, -1 off
static uint64_t rec_down[16];        // tick + 1 at which each button went down, 0 while up
static int64_t rec_down_w[16];       // the world clock then, -1 before it started
static uint64_t rec_stick_from[2];   // tick + 1 at which the current non-zero stick segment began
static int64_t rec_stick_w[2];
static int32_t rec_stick[2][2];
static int64_t rec_last_w = -1;      // the world clock at the previous apply

// "<tick> <what> <hold>" or "@<ms> <what> <hold ms>": from tick from and world clock from_w to now.
static void rec_line(uint64_t from, int64_t from_w, uint64_t fr, int64_t w, const char *what) {
    if (from_w >= 0 && w >= from_w)
        fprintf(rec, "@%.1f %s %.1f\n", from_w / 1000.0, what, (w > from_w ? w - from_w : 1000) / 1000.0);
    else
        fprintf(rec, "%llu %s %llu\n", (unsigned long long)from, what, (unsigned long long)(fr > from ? fr - from : 1));
}

static void rec_close_stick(int s, uint64_t fr, int64_t w) {
    if (!rec_stick_from[s]) return;
    char what[48];
    snprintf(what, sizeof what, "%s %d %d", s ? "RSTICK" : "LSTICK", rec_stick[s][0], rec_stick[s][1]);
    rec_line(rec_stick_from[s] - 1, rec_stick_w[s], fr, w, what);
    rec_stick_from[s] = 0;
}

static void rec_flush_all(void) {
    if (rec_state != 1) return;
    uint64_t fr = frame_step_ticks() + 1;
    int64_t w = frame_step_world_us();
    for (int i = 0; i < 16; i++)
        if (rec_down[i]) { rec_line(rec_down[i] - 1, rec_down_w[i], fr, w, script_buttons[i].name); rec_down[i] = 0; }
    rec_close_stick(0, fr, w);
    rec_close_stick(1, fr, w);
    fflush(rec);
}

static void rec_apply(uint64_t b, const int32_t sl[2], const int32_t sr[2]) {
    if (!rec_state) {
        const char *path = cfg_get("NX2NAVI_INPUT_RECORD");   // no default: not recording when unset
        rec = path && *path ? fopen(path, "w") : NULL;
        rec_state = rec ? 1 : -1;
        if (path && *path && !rec) rt_log("input: cannot open NX2NAVI_INPUT_RECORD %s", path);
        if (rec) { fprintf(rec, "# Recorded input (NX2NAVI_INPUT_RECORD); frame = 60 Hz ticks of game time, @ms = world clock.\n"); atexit(rec_flush_all);
                   rt_log("input: recording to %s", path); }
    }
    if (rec_state != 1) return;
    uint64_t fr = frame_step_ticks();
    int64_t w = frame_step_world_us();
    int wrote = 0;
    if (w >= 0 && rec_last_w < 0) {   // the world clock starts: what is held goes on as timed events from @0
        for (int i = 0; i < 16; i++)
            if (rec_down[i]) {
                rec_line(rec_down[i] - 1, -1, fr, -1, script_buttons[i].name);
                rec_down[i] = fr + 1, rec_down_w[i] = w;
                wrote = 1;
            }
        for (int s = 0; s < 2; s++)
            if (rec_stick_from[s]) {
                rec_close_stick(s, fr, -1);
                rec_stick_from[s] = fr + 1, rec_stick_w[s] = w;
                wrote = 1;
            }
    }
    rec_last_w = w;
    for (int i = 0; i < 16; i++) {
        int bit = script_buttons[i].bit, down = (b >> bit & 1) != 0;
        if (down && !rec_down[i]) rec_down[i] = fr + 1, rec_down_w[i] = w;
        else if (!down && rec_down[i]) {
            rec_line(rec_down[i] - 1, rec_down_w[i], fr, w, script_buttons[i].name);
            rec_down[i] = 0;
            wrote = 1;
        }
    }
    const int32_t *st[2] = {sl, sr};
    for (int s = 0; s < 2; s++) {
        int moved = abs(st[s][0] - rec_stick[s][0]) > REC_STICK_STEP || abs(st[s][1] - rec_stick[s][1]) > REC_STICK_STEP ||
                    ((st[s][0] || st[s][1]) != (rec_stick[s][0] || rec_stick[s][1]));
        if (!moved) continue;
        if (rec_stick_from[s]) { rec_close_stick(s, fr, w); wrote = 1; }
        rec_stick[s][0] = st[s][0]; rec_stick[s][1] = st[s][1];
        if (st[s][0] || st[s][1]) rec_stick_from[s] = fr + 1, rec_stick_w[s] = w;
    }
    if (wrote) fflush(rec);
}

void input_direct_mouse_sample(int enabled, int32_t *dx, int32_t *dy) {
    plat_mutex_lock(&lock);
    enabled = enabled && !g_block && !g_menu;
    *dx = script_mouse[0]; *dy = script_mouse[1];
    script_mouse[0] = script_mouse[1] = 0;
    if (!enabled) { *dx = *dy = 0; }
    else if (ikb_captured() && ikb_foreground() && input_cursor_allowed()) {
        int32_t x, y;
        ikb_mouse_take(&x, &y);
        int64_t sx = (int64_t)*dx + x, sy = (int64_t)*dy + y;
        *dx = (int32_t)(sx < INT32_MIN ? INT32_MIN : sx > INT32_MAX ? INT32_MAX : sx);
        *dy = (int32_t)(sy < INT32_MIN ? INT32_MIN : sy > INT32_MAX ? INT32_MAX : sy);
    }
    if (enabled && (*dx || *dy) && rec_state == 1) {
        char what[80];
        snprintf(what, sizeof what, "MOUSE %d %d", *dx, *dy);
        int64_t w = frame_step_world_us();
        uint64_t tick = frame_step_ticks();
        rec_line(tick, w, tick + 1, w, what);
        fflush(rec);
    }
    plat_mutex_unlock(&lock);
}

static float mouse_sens = -1;              // NX2NAVI_MOUSE_SENS, 0 = mouse camera off; live (cfg_version)
static unsigned mouse_sens_seen;

static float mouse_sens_get(void) {
    unsigned ver = cfg_version();
    if (mouse_sens >= 0 && ver == mouse_sens_seen) return mouse_sens;
    mouse_sens_seen = ver;
    double v = cfg_double("NX2NAVI_MOUSE_SENS");
    mouse_sens = (float)(v < 0 ? 0 : v > 10 ? 10 : v);
    return mouse_sens;
}


// ---- walk key: the keyboard's stick samples scaled down while a key is held (NX2NAVI_WALK_KEY, NX2NAVI_WALK_SCALE)
// A keyboard key is all or nothing, so every keyboard-driven stick sample is +/-32767 and the game only ever sees a
// full tilt. Holding this key scales those two samples, which gives the half tilt a gamepad gets from its analog range,
// so the keyboard can walk as well as run.
// Only the keyboard is scaled: the gamepad's axes and the mouse camera sample their own values.
static int walk_key = KEY_NONE;        // NX2NAVI_WALK_KEY as a KEY_*, KEY_NONE = off; live (cfg_version)
static float walk_scale = 0.51f;       // NX2NAVI_WALK_SCALE, 0.2 to 0.9; live (cfg_version)
static unsigned walk_seen = ~0u;

// The ini takes the key names the Controls tab writes; a hand-written line may spell them in any case.
static int name_matches(const char *name, const char *key) {
    for (; *name && *key; name++, key++) {
        unsigned char a = (unsigned char)*name, b = (unsigned char)*key;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return *name == *key;
}

static void walk_get(void) {
    unsigned ver = cfg_version();
    if (ver == walk_seen) return;
    walk_seen = ver;
    const char *name = cfg_value("NX2NAVI_WALK_KEY");   // "none" (the default) turns the key off
    int key = KEY_NONE;
    if (*name && !name_matches(name, "none")) {
        for (int k = KEY_A; k < KEY_COUNT && key == KEY_NONE; k++)
            if (input_key_name(k) && name_matches(name, input_key_name(k))) key = k;
        if (key == KEY_NONE) rt_log("input: walk_key: unknown key '%s', the keyboard sticks keep their full tilt", name);
    }
    // 0.51 rather than 0.5: a keyboard sample is 32767, and 32767 * 0.5 = 16383 falls one short of stick_bits'
    // STICK_DIR (16384), which would leave the game's stick-direction bits clear while the walk key is held. The value
    // in the settings table carries that default; this code only rejects what the table's range does not allow.
    const char *sv = cfg_value("NX2NAVI_WALK_SCALE");
    char *end;
    double d = strtod(sv, &end);
    double s = end == sv || *end || !(d >= 0.2 && d <= 0.9) ? strtod(cfg_meta("NX2NAVI_WALK_SCALE")->def, NULL) : d;
    if (key != walk_key || (float)s != walk_scale) {
        walk_key = key;
        walk_scale = (float)s;
        rt_log("input: walk key %s, keyboard sticks at %.0f%% of their tilt (mouse and gamepad unchanged)",
               key != KEY_NONE ? input_key_name(key) : "off", s * 100);
    }
}

static int mouse_selector;
int input_direct_mouse_selector_active(void) { return __atomic_load_n(&mouse_selector, __ATOMIC_RELAXED) != 0; }

// Selector 1 is a radial wheel, 2..4 are distinct horizontal quick lists; only a wheel retains a virtual cursor.
// Under `lock`.
static void mouse_camera(int32_t out[2], int selector) {
    static uint64_t frame = ~0ull, t0;
    static int32_t held[2];
    static int was_selector;
    static float cursor[2];
    static InputMouseRow row;
    uint64_t now = plat_now_ns();
    int32_t dx, dy;
    if (!input_direct_mouse_enabled() || !ikb_captured()) selector = 0;
    __atomic_store_n(&mouse_selector, selector, __ATOMIC_RELAXED);
    if (selector) {
        if (was_selector != selector) cursor[0] = cursor[1] = 0, row = (InputMouseRow){0};
        was_selector = selector;
        ikb_mouse_take(&dx, &dy);
        if (selector == 1) input_direct_mouse_radial(dx, dy, mouse_sens_get(), cursor, out);
        else input_direct_mouse_row(dx, mouse_sens_get(), now, &row, out);
        held[0] = held[1] = 0; frame = ~0ull;
        return;
    }
    if (was_selector) {
        was_selector = 0; cursor[0] = cursor[1] = 0; row = (InputMouseRow){0};
        ikb_mouse_take(&dx, &dy);   // release the wheel without sending its last movement to the camera
        held[0] = held[1] = out[0] = out[1] = 0; frame = ~0ull;
        return;
    }
    if (input_direct_mouse_active()) {
        held[0] = held[1] = out[0] = out[1] = 0;
        frame = ~0ull;
        return;   // the camera update takes the raw counts itself, once, outside the HID stick path
    }
    if (!ikb_captured()) {
        ikb_mouse_take(&dx, &dy);
        held[0] = held[1] = 0;
        frame = ~0ull;
    } else {
        uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        double age = frame == ~0ull ? 0 : (double)(now - t0) / 1e9;
        if (fr != frame) {
            double dt = age < 0.004 ? 0.004 : age > 0.1 ? 0.1 : age;
            ikb_mouse_take(&dx, &dy);
            input_mouse_stick(dx, dy, dt, mouse_sens_get(), cam_speed_applied(), held);
            frame = fr;
            t0 = now;
        } else if (age > 0.1) {
            held[0] = held[1] = 0;   // frames stopped: do not hold a deflection
        }
    }
    out[0] = held[0];
    out[1] = held[1];
}

static float stick_dead[2] = {-1, -1};     // input_pad_deadzone of the left and right stick; live (cfg_version)
static unsigned stick_dead_seen;

static float stick_dead_get(int s) {
    unsigned ver = cfg_version();
    if (stick_dead[0] < 0 || ver != stick_dead_seen) {
        stick_dead_seen = ver;
        stick_dead[0] = input_pad_deadzone(0);
        stick_dead[1] = input_pad_deadzone(1);
    }
    return stick_dead[s];
}

// One gamepad stick through the dead zone; SDL's Y grows downwards, the Switch's upwards.
// ---- SDL's gamepad update on a thread of its own
// SDL_UpdateGamepads reads the controllers, and when a device came or went it first finds every device again, 100 ms
// and more inside the system's input libraries. It ran on the game's polling thread with the input lock held: a
// controller that disconnected (a wireless one going to sleep, a cable pulled) gave a burst of 120 to 240 ms frames,
// one per device notification, with the game's other pollers queued on the lock.
// The pad thread (up_main) makes SDL's initialisation and every update. A poll asks for an update and waits for it,
// UP_WAIT_NS at most (sdl_enter): an ordinary update is done in microseconds and the poll reads fresh state, as before;
// one that runs into the device search is not, and the poll goes on with what it read last. sdl_lock is held around
// every SDL gamepad call: by the pad thread for an update, by everyone else through sdl_enter or a try
// (plat_mutex_trylock), never by waiting, so no thread of the game stands behind the device search.
#define UP_WAIT_NS 2000000ull     // how long a poll waits for its update
#define UP_FRESH_NS 500000ull     // polls this soon after an update share it (the game reads several pads in a row)
static PlatMutex sdl_lock = PLAT_MUTEX_INIT;
static PlatMutex up_lock = PLAT_MUTEX_INIT;   // guards the fields below
static PlatCond up_go = PLAT_COND_INIT, up_done = PLAT_COND_INIT;
static int up_started, up_inited, up_init_ok, up_want, up_busy;
static uint64_t up_gen, up_at, up_t0;         // updates finished; when the last one finished; when the running one began
static uint64_t up_stale;                     // polls that went on without an update
static volatile uint64_t up_test_ns;          // --probe-input: the next update takes this long, as a device search does

static void up_main(void *arg) {
    (void)arg;
    // SDL drops gamepad presses while it thinks the program is in the background, and it judges that from windows of
    // its own: the Win32 window (gfx_window_win.c) is not one, and keyboard focus is gated here (ikb_foreground)
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    int ok = SDL_Init(SDL_INIT_GAMEPAD);
    if (!ok) rt_log("input: SDL gamepad init failed (%s), keyboard only", SDL_GetError());
    plat_mutex_lock(&up_lock);
    up_inited = 1, up_init_ok = ok;
    plat_cond_broadcast(&up_done);
    while (ok) {
        while (!up_want) plat_cond_wait(&up_go, &up_lock);
        up_want = 0, up_busy = 1, up_t0 = plat_now_ns();
        plat_mutex_unlock(&up_lock);
        plat_mutex_lock(&sdl_lock);
        uint64_t slow = __atomic_exchange_n(&up_test_ns, 0, __ATOMIC_RELAXED);
        if (slow) plat_wait_ns(slow);
        SDL_UpdateGamepads();
#ifdef _WIN32
        // The Windows window is our own (gfx_window_win.c), so nothing reads SDL's event queue, yet every axis and
        // button change of a gamepad is queued there: at SDL's cap (about 65,000 events, several MB) each further one
        // fails and sets SDL's error text. The state the getters read does not come from the queue.
        SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
#endif
        plat_mutex_unlock(&sdl_lock);
        plat_mutex_lock(&up_lock);
        uint64_t took = plat_now_ns() - up_t0;
        up_busy = 0, up_gen++, up_at = plat_now_ns();
        plat_cond_broadcast(&up_done);
        static int logged;
        if (took >= 20000000ull && logged++ < 16)
            rt_log("input: the gamepad update took %.0f ms (devices found again after one came or went); %llu polls so far "
                   "went on with their last state instead of waiting", took / 1e6, (unsigned long long)up_stale);
    }
    plat_mutex_unlock(&up_lock);
}

// Under lock: starts the pad thread and waits for SDL's initialisation on it. 1 when SDL's gamepads can be used.
static int sdl_start(void) {
    plat_mutex_lock(&up_lock);
    if (!up_started) {
        up_started = 1;
        if (!plat_thread_spawn(up_main, NULL, "pads", 0)) rt_fatal("input: cannot start the gamepad thread");
    }
    while (!up_inited) plat_cond_wait(&up_done, &up_lock);
    int ok = up_init_ok;
    plat_mutex_unlock(&up_lock);
    return ok;
}

// Under lock: brings SDL's gamepad state up to date and takes sdl_lock for the caller's SDL calls. 1 with it held
// (sdl_leave gives it back); 0 when the update is not done within UP_WAIT_NS, and then nothing of SDL may be called.
static int sdl_enter(void) {
    plat_mutex_lock(&up_lock);
    uint64_t now = plat_now_ns();
    int ok = 1;
    if (up_busy && now - up_t0 >= UP_WAIT_NS) {
        ok = 0;   // the device search: the first poll waited for it, the ones after it do not
    } else if (up_busy || now - up_at >= UP_FRESH_NS) {
        uint64_t next = up_gen + 1;
        if (!up_busy) up_want = 1, plat_cond_signal(&up_go);
        while (up_gen < next) {
            uint64_t el = plat_now_ns() - now;
            if (el >= UP_WAIT_NS) break;
            plat_cond_wait_ns(&up_done, &up_lock, UP_WAIT_NS - el);
        }
        ok = up_gen >= next;
    }
    if (!ok) up_stale++;
    plat_mutex_unlock(&up_lock);
    if (ok && !plat_mutex_trylock(&sdl_lock)) ok = 0;   // the next update began already, for another poller
    return ok;
}

static void sdl_leave(void) { plat_mutex_unlock(&sdl_lock); }

// ---- HD rumble: the game's vibration values as they are, for a controller with linear actuators
// A Switch Pro Controller and a Joy-Con play a frequency and an amplitude per band, which is what the game sends
// (ov_input.c VibrationValue); SDL_RumbleGamepad drives them at two fixed frequencies. With NX2NAVI_RUMBLE_HD=1 (live,
// off by default) the primary gamepad, when it is one of these, gets the game's values in the controller's own rumble
// report (rumble_hd.c) and no dual-motor call any more (input_rumble):
// - HD_SDL, a Pro Controller or a single Joy-Con: SDL_SendGamepadEffect takes the report for these controllers and
//   writes it with SDL's own report number, over USB and over Bluetooth. With the setting on, hd_probe tries the call
//   with the neutral value when the gamepad becomes the primary, so the log says at once whether it is taken. With the
//   setting off nothing is written: the controller is known by its ids, and the first report after the setting is
//   turned on is the trial (hd_wrote; a refusal sends the gamepad back to its motors).
// - HD_PAIR, a left and a right Joy-Con that SDL presents as one gamepad: SDL refuses the call for that gamepad, so the
//   reports go to the two HID devices directly (input_hd_pair.c).
// Every other gamepad keeps the motors' path, the Switch 2 controllers too (other product ids, another protocol).
// The game's thread only posts its newest values (input_rumble_hd). A thread of its own writes the reports (hd_main):
// every HD_TICK_NS while there is something to write it asks rumble_hd_due, which spaces the reports, keeps only the
// newest value, repeats a held one, and turns values the game stopped renewing into the neutral one, so a paused or
// frozen game does not leave the controller vibrating. The thread sleeps once the neutral value is out. The neutral
// value is also written when another gamepad becomes the primary (hd_pad_leave), when the setting is turned off
// (hd_tick) and before the process ends (input_rumble_hd_stop).
enum { HD_NONE, HD_SDL, HD_PAIR };
enum { VID_NINTENDO = 0x057E, PID_JOYCON_LEFT = 0x2006, PID_JOYCON_RIGHT = 0x2007, PID_SWITCH_PRO = 0x2009,
       PID_JOYCON_GRIP = 0x200E };
#define HD_TICK_NS 5000000ull
static int pad_hd;                    // under lock: how the primary takes rumble reports (HD_*)
static volatile int32_t hd_shown;     // any thread: 0 the primary takes none, 1 it takes them, 2 for its single actuator
static int sdl_rumbling;              // under lock: the primary's motors were last set to more than 0 (input_rumble)
static PlatMutex hd_lock = PLAT_MUTEX_INIT;   // guards the fields below; taken after `lock`, never before it
static PlatCond hd_go = PLAT_COND_INIT;
static RumbleHd hd;                   // what the primary is to get and what it got
static int hd_kick;                   // work for the thread: a post, or a pair that left
static int hd_pair_left;              // the pair stopped being the primary: its two devices are to be closed
static int hd_ended;                  // input_rumble_hd_stop ran: nothing is written any more
static int hd_first_logged;           // under lock: the primary's first report was logged (hd_wrote)
static volatile int32_t hd_thread;    // 0 not started, 1 running, -1 it could not be started (set under hd_lock)

// NX2NAVI_RUMBLE_HD is on and rumble is not off altogether (NX2NAVI_RUMBLE=0); live (cfg_version). Any thread.
static int hd_setting(void) {
    static volatile int32_t on = -1;
    static volatile uint32_t seen;
    unsigned ver = cfg_version();
    if (__atomic_load_n(&on, __ATOMIC_RELAXED) < 0 || ver != __atomic_load_n(&seen, __ATOMIC_RELAXED)) {
        const char *all = cfg_get("NX2NAVI_RUMBLE");
        __atomic_store_n(&on, cfg_bool("NX2NAVI_RUMBLE_HD") && !(all && !strcmp(all, "0")), __ATOMIC_RELAXED);
        __atomic_store_n(&seen, ver, __ATOMIC_RELAXED);
    }
    return __atomic_load_n(&on, __ATOMIC_RELAXED);
}

// The time between two reports: NX2NAVI_RUMBLE_HD_MS, 5 to 50 ms, RUMBLE_HD_GAP_NS when unset; live. The HD thread only.
static uint64_t hd_gap_ns(void) {
    static uint64_t gap = RUMBLE_HD_GAP_NS;
    static unsigned seen = ~0u;
    unsigned ver = cfg_version();
    if (ver == seen) return gap;
    seen = ver;
    const char *e = cfg_get("NX2NAVI_RUMBLE_HD_MS");
    long ms = e && *e ? strtol(e, NULL, 10) : 0;
    uint64_t set = ms ? (uint64_t)(ms < 5 ? 5 : ms > 50 ? 50 : ms) * 1000000u : RUMBLE_HD_GAP_NS;
    if (set != gap) rt_log("input: HD rumble: a report at most every %llu ms", (unsigned long long)(set / 1000000u));
    return gap = set;
}

// Under lock and sdl_lock: how gamepad p takes rumble reports (HD_*), with *one set for a single actuator and the
// words for the log in how. The controllers are known by Nintendo's vendor id, their product ids and the kind SDL
// reads from the controller itself: a pad of another make that only looks like one to SDL stays on its motors.
static int hd_probe(SDL_Gamepad *p, int *one, char *how, size_t cap) {
    *one = 0;
    uint16_t vid = SDL_GetGamepadVendor(p), pid = SDL_GetGamepadProduct(p);
    SDL_GamepadType t = SDL_GetRealGamepadType(p);
    int pro = vid == VID_NINTENDO && pid == PID_SWITCH_PRO && t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO;
    int single = vid == VID_NINTENDO && (pid == PID_JOYCON_LEFT || pid == PID_JOYCON_RIGHT || pid == PID_JOYCON_GRIP) &&
                 (t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT || t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT);
    if (t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR) {
        if (hd_pair_find()) {
            snprintf(how, cap, "a Joy-Con pair, written to its two HID devices");
            return HD_PAIR;
        }
        snprintf(how, cap, "a Joy-Con pair whose two Joy-Con are not both listed as HID devices");
        return HD_NONE;
    }
    if (!pro && !single) {
        snprintf(how, cap, "%04x:%04x is not a Switch Pro Controller or Joy-Con", vid, pid);
        return HD_NONE;
    }
    const char *what = single ? "a single Joy-Con, one actuator" : "a Pro Controller";
    *one = single;
    if (!hd_setting()) {   // off: nothing is written to the controller
        snprintf(how, cap, "%s, through SDL, whose driver is asked once rumble_hd is 1", what);
        return HD_SDL;
    }
    uint8_t r[RUMBLE_HD_REPORT];
    rumble_hd_report(rumble_hd_neutral, 0, r, sizeof r);
    if (!SDL_SendGamepadEffect(p, r, sizeof r)) {
        snprintf(how, cap, "%s, but its gamepad driver takes no rumble data: %s", what, SDL_GetError());
        return HD_NONE;
    }
    snprintf(how, cap, "%s, through SDL", what);
    return HD_SDL;
}

// Under lock, and under sdl_lock when connected is set: the primary stops being the primary (another gamepad takes
// over, or it went away). A controller the HD path left vibrating gets the neutral value, and what was written to it
// does not count for the next primary.
static void hd_pad_leave(int connected) {
    if (!pad) return;
    plat_mutex_lock(&hd_lock);
    int buzzing = rumble_hd_buzzing(&hd);
    rumble_hd_forget(&hd);
    if (pad_hd == HD_PAIR) {   // the HD thread closes the two devices, after a neutral report to each
        hd_pair_left = 1, hd_kick = 1;
        plat_cond_signal(&hd_go);
    }
    plat_mutex_unlock(&hd_lock);
    if (pad_hd == HD_SDL && buzzing && connected) {
        uint8_t r[RUMBLE_HD_REPORT];
        rumble_hd_report(rumble_hd_neutral, 0, r, sizeof r);
        SDL_SendGamepadEffect(pad, r, sizeof r);
    }
    pad_hd = HD_NONE, sdl_rumbling = 0, hd_first_logged = 0;
    __atomic_store_n(&hd_shown, 0, __ATOMIC_RELAXED);
}

// Under lock: the report with data d was written at `now` (ok), or the controller or its driver did not take it: the
// gamepad then goes back to its motors until it becomes the primary again. The primary's first report is logged with
// its bytes, so a log shows that the path is in use and what the controller was sent.
static void hd_wrote(int ok, const uint8_t d[RUMBLE_HD_DATA], uint64_t now, const char *err) {
    plat_mutex_lock(&hd_lock);
    if (ok) rumble_hd_sent(&hd, d, now);
    else rumble_hd_forget(&hd);
    plat_mutex_unlock(&hd_lock);
    if (ok) {
        if (!hd_first_logged)
            rt_log("input: HD rumble: first report written (left %02x %02x %02x %02x, right %02x %02x %02x %02x)", d[0], d[1],
                   d[2], d[3], d[4], d[5], d[6], d[7]);
        hd_first_logged = 1;
        return;
    }
    rt_log("input: HD rumble: a report for the gamepad was not taken (%s); it rumbles through its motors from here on", err);
    pad_hd = HD_NONE;
    __atomic_store_n(&hd_shown, 0, __ATOMIC_RELAXED);
}

// One round of the HD rumble thread: writes the report that is due, if one is. 1 when nothing is left to write until
// the game posts again.
static int hd_tick(void) {
    uint8_t d[RUMBLE_HD_DATA];
    uint64_t now = plat_now_ns(), gap = hd_gap_ns();
    plat_mutex_lock(&lock);
    int kind = pad ? pad_hd : HD_NONE, on = kind != HD_NONE && hd_setting();
    plat_mutex_lock(&hd_lock);
    int close_pair = hd_pair_left;
    hd_pair_left = 0;
    int due = !hd_ended && kind != HD_NONE && rumble_hd_due(&hd, on, now, gap, d);
    int idle = hd_ended || kind == HD_NONE || (!due && rumble_hd_idle(&hd, on, now));
    plat_mutex_unlock(&hd_lock);
    if (due && kind == HD_SDL && plat_mutex_trylock(&sdl_lock)) {   // a round later while SDL updates
        if (SDL_GamepadConnected(pad)) {   // a gamepad that went away is let go by the next scan (scan_pads)
            uint8_t r[RUMBLE_HD_REPORT];
            rumble_hd_report(d, 0, r, sizeof r);   // SDL puts its own number in
            int ok = SDL_SendGamepadEffect(pad, r, sizeof r);
            hd_wrote(ok, d, now, ok ? "" : SDL_GetError());
        }
        sdl_leave();
    }
    plat_mutex_unlock(&lock);
    if (close_pair) hd_pair_close(1, 0);
    if (due && kind == HD_PAIR) {
        // waits for the two writes, so not under lock; after input_rumble_hd_stop it writes nothing (hd_pair_end)
        int ok = hd_pair_write(d);
        plat_mutex_lock(&lock);
        // unless the pair left or the stop came meanwhile (hd_ended is written under lock as well)
        if (pad && pad_hd == HD_PAIR && !hd_ended) hd_wrote(ok, d, now, ok ? "" : SDL_GetError());
        plat_mutex_unlock(&lock);
    }
    return idle;
}

static void hd_main(void *arg) {
    (void)arg;
    for (int idle = 1;;) {
        plat_mutex_lock(&hd_lock);
        while (idle && !hd_kick) plat_cond_wait(&hd_go, &hd_lock);
        hd_kick = 0;
        plat_mutex_unlock(&hd_lock);
        if (!idle) plat_wait_ns(HD_TICK_NS);
        idle = hd_tick();
    }
}

int input_rumble_hd_available(void) { return __atomic_load_n(&hd_shown, __ATOMIC_RELAXED) != 0; }

int input_rumble_hd_on(void) {
    return __atomic_load_n(&hd_shown, __ATOMIC_RELAXED) && __atomic_load_n(&hd_thread, __ATOMIC_RELAXED) >= 0 && hd_setting();
}

void input_rumble_hd(const RumbleHdValue v[2], const int live[2], float strength) {
    int32_t shown = __atomic_load_n(&hd_shown, __ATOMIC_RELAXED);
    if (!shown) return;
    uint8_t d[RUMBLE_HD_DATA];
    rumble_hd_data(v, live, strength, shown == 2, d);
    plat_mutex_lock(&hd_lock);
    rumble_hd_post(&hd, d, plat_now_ns());
    if (!hd_thread) {
        int ok = plat_thread_spawn(hd_main, NULL, "rumble", 0);
        __atomic_store_n(&hd_thread, ok ? 1 : -1, __ATOMIC_RELAXED);
        if (!ok) rt_log("input: HD rumble: its thread could not be started; the gamepad rumbles through its motors");
    }
    hd_kick = 1;
    plat_cond_signal(&hd_go);
    plat_mutex_unlock(&hd_lock);
}

void input_rumble_hd_stop(void) {
    int got = 0, wait = 0;
    // The locks are tried for a while and not waited for: a thread of the game may hold one for good by now.
    for (int i = 0; i < 50 && !(got = plat_mutex_trylock(&lock)); i++) plat_wait_ns(1000000);
    if (!got) return;
    plat_mutex_lock(&hd_lock);
    hd_ended = 1;
    int buzzing = rumble_hd_buzzing(&hd);
    rumble_hd_forget(&hd);
    plat_mutex_unlock(&hd_lock);
    int pair = pad && pad_hd == HD_PAIR;
    if (pad && pad_hd == HD_SDL && buzzing) {
        got = 0;
        for (int i = 0; i < 50 && !(got = plat_mutex_trylock(&sdl_lock)); i++) plat_wait_ns(1000000);
        if (got) {
            uint8_t r[RUMBLE_HD_REPORT];
            rumble_hd_report(rumble_hd_neutral, 0, r, sizeof r);
            wait = SDL_GamepadConnected(pad) && SDL_SendGamepadEffect(pad, r, sizeof r);
            sdl_leave();
        }
    }
    plat_mutex_unlock(&lock);
    if (pair) hd_pair_end(100);
    if (wait) plat_wait_ns(30000000);   // SDL writes the report on a thread of its own: time for it before the process ends
}

// Every connected gamepad is kept open; `pad` is the one the game reads (and the one that rumbles and feeds motion):
// the first one found, then whichever one the player presses (pad_follow). Taking only the first gamepad SDL lists
// left the game deaf when that one was a device nobody holds: a virtual pad of Steam Input or DS4Windows, a wheel, a
// receiver's idle second slot, listed before the controller in the player's hands.
enum { MAX_PADS = 8 };
static SDL_Gamepad *pads[MAX_PADS];
static uint8_t pad_rest[MAX_PADS];   // per open gamepad, bit a: axis a has been seen at rest (pad_axis)
static int npads;
static int pad_pressed_logged;       // the primary's first press was logged (once per primary)

// Under lock: makes p the gamepad the game reads, with its labels, rumble and layout; logs it.
static void pad_primary(SDL_Gamepad *p, const char *why) {
    hd_pad_leave(1);
    pad = p;
    sensor_pad = 0, sensor_ok = 0, pad_pressed_logged = 0;
    pad_rumble = SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
    pad_east_a = SDL_GetGamepadButtonLabel(pad, SDL_GAMEPAD_BUTTON_EAST) == SDL_GAMEPAD_BUTTON_LABEL_A;
    for (int i = 0; i < 4; i++) {
        SDL_GamepadButtonLabel lb = SDL_GetGamepadButtonLabel(pad, pad_sdl[PAD_SOUTH + i]);
        plat_xchg32(&pad_labels[i], lb == SDL_GAMEPAD_BUTTON_LABEL_A          ? PAD_LABEL_A
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_B        ? PAD_LABEL_B
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_X        ? PAD_LABEL_X
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_Y        ? PAD_LABEL_Y
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_CROSS    ? PAD_LABEL_CROSS
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_CIRCLE   ? PAD_LABEL_CIRCLE
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_SQUARE   ? PAD_LABEL_SQUARE
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE ? PAD_LABEL_TRIANGLE
                                                                                      : PAD_LABEL_NONE);
    }
    pad_layout = -1;
    int motion = SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL);
    rt_log("input: gamepad \"%s\" %s (%s, %s, east button %s)", SDL_GetGamepadName(pad), why,
           pad_rumble ? "rumble" : "no rumble", motion ? "gyro and accelerometer" : "no motion sensors",
           pad_east_a ? "labelled A" : "not labelled A");
    char how[200];
    int one;
    pad_hd = hd_probe(pad, &one, how, sizeof how);
    __atomic_store_n(&hd_shown, pad_hd == HD_NONE ? 0 : 1 + one, __ATOMIC_RELAXED);
    rt_log("input: HD rumble %s for this gamepad: %s (rumble_hd = %s)", pad_hd == HD_NONE ? "not available" : "available",
           how, cfg_value("NX2NAVI_RUMBLE_HD"));
}

// Joysticks SDL has no gamepad mapping for are not gamepads to it, and this file opens gamepads only. Known ones get a
// mapping built from their own GUID (so it matches whatever bus, version and name checksum SDL put in it); any other is
// logged once with its name, ids and GUID.
// - A Switch that presents its Joy-Con over USB from its boot menu's tools:
//   "Nyx Joy-Con", VID 0x11EC, PID 0xA7E1, a plain HID joystick: X, Y (left stick), Z, Rz (right stick) from 0 to 255
//   with up at 0, a hat for the D-pad, and buttons 1-12 B, A, Y, X (south, east, west, north), L, R, ZL, ZR, Minus,
//   Plus, left and right stick clicks. Capture and Home are not sent.
static const struct { uint16_t vid, pid; const char *name, *map; } known_joysticks[] = {
    {0x11EC, 0xA7E1, "Nyx Joy-Con (USB HID)",
     "a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,back:b8,start:b9,leftstick:b10,"
     "rightstick:b11,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,leftx:a0,lefty:a1,rightx:a2,righty:a3"},
};

static void map_joysticks(void) {
    static SDL_JoystickID seen[32];
    static int nseen;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&n);
    for (int i = 0; ids && i < n; i++) {
        int done = 0;
        for (int k = 0; k < nseen && !done; k++) done = seen[k] == ids[i];
        if (done || SDL_IsGamepad(ids[i])) continue;
        if (nseen < (int)(sizeof seen / sizeof *seen)) seen[nseen++] = ids[i];
        uint16_t vid = SDL_GetJoystickVendorForID(ids[i]), pid = SDL_GetJoystickProductForID(ids[i]);
        char guid[64];
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(ids[i]), guid, (int)sizeof guid);
        const char *jname = SDL_GetJoystickNameForID(ids[i]);
        int mapped = 0;
        for (size_t k = 0; k < sizeof known_joysticks / sizeof *known_joysticks; k++) {
            if (known_joysticks[k].vid != vid || known_joysticks[k].pid != pid) continue;
            char m[512];
            snprintf(m, sizeof m, "%s,%s,%s", guid, known_joysticks[k].name, known_joysticks[k].map);
            int r = SDL_AddGamepadMapping(m);
            rt_log("input: joystick \"%s\" (%04x:%04x, GUID %s) mapped as a gamepad (%s)%s%s", jname ? jname : "?", vid, pid,
                   guid, known_joysticks[k].name, r < 0 ? ": mapping refused, " : "", r < 0 ? SDL_GetError() : "");
            mapped = 1;
        }
        if (!mapped)
            rt_log("input: joystick \"%s\" (%04x:%04x, GUID %s) has no gamepad mapping in SDL: not used (an SDL mapping "
                   "in the SDL_GAMECONTROLLERCONFIG environment variable adds one)", jname ? jname : "?", vid, pid, guid);
    }
    SDL_free(ids);
}

// Under lock: closes the gamepads that went away and opens the ones that came; the primary falls back to the first
// open one when it left. Logs each change.
// The game is always told one controller is connected, so a disconnect read as nothing pressed and play went on
// uncontrolled (the console would stop and ask for a controller). When the primary goes and no other gamepad takes
// its place, the menu opens: the player sees it, and it takes the keyboard and mouse until it is closed.
static void scan_pads(void) {
    int had = pad != NULL;
    map_joysticks();
    for (int i = 0; i < npads;) {
        if (SDL_GamepadConnected(pads[i])) { i++; continue; }
        rt_log("input: gamepad \"%s\" disconnected", SDL_GetGamepadName(pads[i]) ? SDL_GetGamepadName(pads[i]) : "?");
        if (pads[i] == pad) {
            hd_pad_leave(0);
            pad = NULL, sensor_pad = 0, sensor_ok = 0;
            plat_xchg32(&pad_layout_shown, -1);
            for (int k = 0; k < 4; k++) plat_xchg32(&pad_labels[k], PAD_LABEL_NONE);
        }
        SDL_CloseGamepad(pads[i]);
        --npads;
        pads[i] = pads[npads], pad_rest[i] = pad_rest[npads];
    }
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n && npads < MAX_PADS; i++) {
        int open = 0;
        for (int k = 0; k < npads && !open; k++) open = SDL_GetGamepadID(pads[k]) == ids[i];
        if (open) continue;
        SDL_Gamepad *p = SDL_OpenGamepad(ids[i]);
        if (!p) { rt_log("input: gamepad %u could not be opened (%s)", (unsigned)ids[i], SDL_GetError()); continue; }
        pad_rest[npads] = 0;
        pads[npads++] = p;
        if (pad) rt_log("input: gamepad \"%s\" connected (the game switches to it at its first press)", SDL_GetGamepadName(p));
    }
    SDL_free(ids);
    if (!pad && npads) pad_primary(pads[0], "in use");
    if (had && !pad && !menu_is_open()) {
        rt_log("input: the gamepad in use went away and no other is connected: the menu opens");
        menu_toggle();
    }
}

// Under lock: axis a of open gamepad p, or 0 until the axis has been seen at rest (within a quarter of its range of the
// middle; a trigger's rest is 0). A device that reports an axis held from the start (a virtual pad nobody holds, a
// trigger or pedal mapped onto a stick, a stick that rests far off its middle) would otherwise turn the camera on its
// own, and, counted as pressed, take the gamepad role from the controller in the player's hands for good (pad_follow).
// A stick the player holds at the start counts once it has been let go.
enum { AXIS_REST = 8192 };
static int16_t pad_axis(SDL_Gamepad *p, SDL_GamepadAxis a) {
    int16_t v = SDL_GetGamepadAxis(p, a);
    for (int i = 0; i < npads; i++) {
        if (pads[i] != p) continue;
        if (pad_rest[i] >> a & 1) return v;
        if (abs(v) >= AXIS_REST) return 0;
        pad_rest[i] |= (uint8_t)(1u << a);
        return v;
    }
    return v;
}

static void stick(SDL_GamepadAxis ax, SDL_GamepadAxis ay, float dead, int32_t out[2]) {
    input_pad_stick(pad_axis(pad, ax), -(int32_t)pad_axis(pad, ay), dead, out);
}

// Under lock: 1 when p has a button down, a trigger pulled or a stick past half way (a drifting stick does not count,
// nor an axis not yet seen at rest).
static int pad_active(SDL_Gamepad *p) {
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
        if (SDL_GetGamepadButton(p, (SDL_GamepadButton)b)) return 1;
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++)
        if (abs(pad_axis(p, (SDL_GamepadAxis)a)) > PAD_USED) return 1;
    return 0;
}

// Under lock: when the primary is idle and another open gamepad is pressed, that one becomes the primary.
static void pad_follow(void) {
    if (npads < 2 || (pad && pad_active(pad))) return;
    for (int i = 0; i < npads; i++)
        if (pads[i] != pad && pad_active(pads[i])) { pad_primary(pads[i], "in use (pressed)"); return; }
}

// Under lock: the layout of the open gamepad, picked again when a setting or the gamepad changed; logs each change.
static int pad_layout_get(void) {
    unsigned ver = cfg_version();
    if (pad_layout >= 0 && ver == pad_layout_seen) return pad_layout;
    pad_layout_seen = ver;
    int was = pad_layout;
    const char *set = cfg_value("NX2NAVI_PAD_LAYOUT");
    pad_layout = input_pad_layout_pick(set, pad_east_a);
    if (pad_layout != was)
        rt_log("input: gamepad layout %s (pad_layout = %s)", pad_layout == PAD_LAYOUT_XBOX ? "xbox" : "nintendo", set);
    plat_xchg32(&pad_layout_shown, pad_layout);
    return pad_layout;
}

// The NX2NAVI_BIND_* values last applied, one per line; the tables are built again when one changes (the menu's remapping rows write
// them while the game runs). Under lock.
static unsigned binds_seen = ~0u;
static char binds_applied[4096];

static void binds_refresh(void) {
    unsigned v = cfg_version();
    if (v == binds_seen) return;
    binds_seen = v;
    char now[sizeof binds_applied];
    size_t len = 0;
    for (int a = 0; a < ACTION_COUNT; a++) {
        char var[32];
        snprintf(var, sizeof var, "NX2NAVI_BIND_%s", input_action_name(a));
        const char *s = cfg_get(var);
        len += (size_t)snprintf(now + len, len < sizeof now ? sizeof now - len : 0, "%s\n", s ? s : "");
    }
    if (!strcmp(now, binds_applied)) return;
    memcpy(binds_applied, now, sizeof now);
    input_map_configure();
}

// NX2NAVI_TEST_PAD=<lx>,<ly>,<rx>,<ry> (test runs): an SDL virtual gamepad whose sticks rest at these raw SDL axis values
// (-32768..32767, down is +y), attached before the first scan so it is the gamepad the game reads: drift and full
// deflection go through the same mapping and dead zone as a real pad. NX2NAVI_TEST_PAD_AT=<ms>: attached that long after
// input starts instead, like a controller switched on in play, and its south button pressed for half a second one
// second later (the log then shows the hot-plug and the press arriving: scan_pads, pad_physical).
static SDL_Joystick *test_joy;     // kept open: a virtual joystick's axes and buttons are set through an open handle
static uint64_t test_attach_ns, test_press_ns;   // NX2NAVI_TEST_PAD_AT: when to attach, when to press (0: done)

static void test_pad_make(void) {
    const char *e = cfg_get("NX2NAVI_TEST_PAD");
    int v[4];
    if (!e || !*e) return;
    if (sscanf(e, "%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3]) != 4) {
        rt_log("input: NX2NAVI_TEST_PAD=%s is not <lx>,<ly>,<rx>,<ry>; no test gamepad", e);
        return;
    }
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.name = "NX2navi test gamepad";
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
    test_joy = id ? SDL_OpenJoystick(id) : NULL;
    if (!test_joy) {
        rt_log("input: test gamepad not attached (%s)", SDL_GetError());
        return;
    }
    static const SDL_GamepadAxis axes[4] = {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX,
                                            SDL_GAMEPAD_AXIS_RIGHTY};
    for (int i = 0; i < 4; i++)
        SDL_SetJoystickVirtualAxis(test_joy, axes[i], (Sint16)(v[i] < -32768 ? -32768 : v[i] > 32767 ? 32767 : v[i]));
    rt_log("input: test gamepad attached, sticks at %d,%d %d,%d", v[0], v[1], v[2], v[3]);
}

static void test_pad_attach(void) {
    const char *at = cfg_get("NX2NAVI_TEST_PAD_AT");
    if (at && *at && cfg_get("NX2NAVI_TEST_PAD")) {
        test_attach_ns = plat_now_ns() + strtoull(at, NULL, 10) * 1000000ull;
        return;
    }
    test_pad_make();
}

// Under lock, every poll: the delayed attach and the press of NX2NAVI_TEST_PAD_AT.
static void test_pad_tick(void) {
    uint64_t now = plat_now_ns();
    if (test_attach_ns && now >= test_attach_ns) {
        test_attach_ns = 0;
        test_pad_make();
        if (test_joy) test_press_ns = now + 1000000000ull;
    }
    if (test_press_ns && test_joy && now >= test_press_ns) {
        int down = now < test_press_ns + 500000000ull;
        SDL_SetJoystickVirtualButton(test_joy, SDL_GAMEPAD_BUTTON_SOUTH, down);
        if (!down) test_press_ns = 0;
    }
}

static void start(void) {
    binds_refresh();
    const char *e = cfg_get("NX2NAVI_INPUT");
    if (e && !strcmp(e, "none")) { state = -1; rt_log("input: disabled (NX2NAVI_INPUT=none)"); return; }
    state = sdl_start() ? 1 : -1;   // SDL's initialisation, on the pad thread (up_main)
    if (state == 1) {
        plat_mutex_lock(&sdl_lock);   // the pad thread updates only when asked, and nobody has asked yet
        test_pad_attach();
        scan_pads();
        plat_mutex_unlock(&sdl_lock);
        if (!pad) rt_log("input: no gamepad (keyboard only until one is connected)");
    }
    if (state == -1) state = 2;   // keyboard only
}

static void stick_bits(uint64_t *b, int32_t x, int32_t y, int first) {
    if (x < -STICK_DIR) *b |= 1ull << (first + 0);
    if (y > STICK_DIR) *b |= 1ull << (first + 1);
    if (x > STICK_DIR) *b |= 1ull << (first + 2);
    if (y < -STICK_DIR) *b |= 1ull << (first + 3);
}

static void pick(int32_t *x, int32_t *y, int32_t kx, int32_t ky) {
    int64_t a = (int64_t)*x * *x + (int64_t)*y * *y, b = (int64_t)kx * kx + (int64_t)ky * ky;
    if (b > a) { *x = kx; *y = ky; }
}

// The shake key (B_SHAKE): the game detaches a part fused to the held object when the right stick swings left and
// right a few times. A press swings it for SHAKE_NS, holding keeps it swinging; each side lasts SHAKE_SIDE_NS, two
// frames at the game's 30 fps, so every side is read even at 20 fps. Returns the stick x, 0 while not shaking.
// Under lock.
#define SHAKE_NS 800000000ull
#define SHAKE_SIDE_NS 66666667ull
static int32_t shake_stick(int held) {
    static int was;
    static uint64_t from, until;
    uint64_t now = plat_now_ns();
    if (held && !was && now >= until) from = now;
    if (held && !was) until = now + SHAKE_NS;
    if (held && until < now) until = now;
    was = held;
    if (now >= until && !held) return 0;
    return (now - from) / SHAKE_SIDE_NS % 2 ? STICK_MAX : -STICK_MAX;
}

// Under lock and inside sdl_enter (the gamepads' state is up to date), state 1: the open gamepad's inputs by position
// (pd[PAD_*] != 0 held); 0 without a connected gamepad.
static int pad_physical(uint8_t pd[PAD_COUNT]) {
    test_pad_tick();
    // every RESCAN_MS, not only without a gamepad: a second controller, or one that wakes up after the start, is found
    if (plat_now_ns() / 1000000u >= next_scan) { next_scan = plat_now_ns() / 1000000u + RESCAN_MS; scan_pads(); }
    pad_follow();
    if (!pad || !SDL_GamepadConnected(pad)) return 0;
    for (int p = PAD_SOUTH; p < PAD_ZL; p++) pd[p] = SDL_GetGamepadButton(pad, pad_sdl[p]);
    pd[PAD_ZL] = pad_axis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > TRIGGER_ON;
    pd[PAD_ZR] = pad_axis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > TRIGGER_ON;
    for (int p = PAD_ZR + 1; p < PAD_COUNT; p++) pd[p] = SDL_GetGamepadButton(pad, pad_sdl[p]);   // false without paddles
    if (!pad_pressed_logged) {   // once per primary: the log then shows that its presses arrive
        for (int p = PAD_SOUTH; p < PAD_COUNT && !pad_pressed_logged; p++)
            if (pd[p]) pad_pressed_logged = 1, rt_log("input: gamepad \"%s\": first press (%s)", SDL_GetGamepadName(pad), input_pad_name(p));
    }
    return 1;
}

// Under lock, state 1: the open gamepad's buttons (Switch bits after NX2NAVI_PAD_LAYOUT and the NX2NAVI_BIND_* lines) and
// sticks; 0 without a connected gamepad.
#ifdef _WIN32
// Without a gamepad SDL can open, Xbox controllers are read through XInput directly (xinput1_4, else xinput9_1_0):
// SDL may miss a pad another program or driver presents only as XInput. Slots 0-3, the first connected one; a slot
// found empty is asked again once a second, as XInputGetState on an empty slot takes milliseconds.
typedef struct { uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; } XiPad;
typedef struct { uint32_t packet; XiPad pad; } XiState;
typedef uint32_t(__stdcall *XiGetState)(uint32_t, XiState *);
__declspec(dllimport) void *__stdcall LoadLibraryA(const char *);
__declspec(dllimport) void *__stdcall GetProcAddress(void *, const char *);

// Under lock: the first connected XInput pad's inputs by position (input_map.h PAD_*) and sticks (y up); 0 without one.
static int xinput_read(uint8_t pd[PAD_COUNT], int32_t *raw) {
    static XiGetState get;
    static int tried, slot = -1;
    static uint64_t next_try;
    if (!tried) {
        tried = 1;
        void *m = LoadLibraryA("xinput1_4.dll");
        if (!m) m = LoadLibraryA("xinput9_1_0.dll");
        if (m) get = (XiGetState)GetProcAddress(m, "XInputGetState");
    }
    if (!get) return 0;
    XiState st;
    if (slot >= 0 && get((uint32_t)slot, &st) != 0) {
        rt_log("input: XInput controller %d disconnected", slot);
        slot = -1;
    }
    if (slot < 0) {
        uint64_t now = plat_now_ns();
        if (now < next_try) return 0;
        next_try = now + 1000000000ull;
        for (int i = 0; i < 4 && slot < 0; i++)
            if (get((uint32_t)i, &st) == 0) slot = i, rt_log("input: XInput controller %d (SDL has no gamepad)", i);
        if (slot < 0) return 0;
    }
    uint16_t w = st.pad.buttons;
    static const struct { uint16_t bit; uint8_t pad; } map[] = {
        {0x1000, PAD_SOUTH}, {0x2000, PAD_EAST}, {0x4000, PAD_WEST}, {0x8000, PAD_NORTH}, {0x0040, PAD_LSTICK},
        {0x0080, PAD_RSTICK}, {0x0100, PAD_L}, {0x0200, PAD_R}, {0x0010, PAD_START}, {0x0020, PAD_BACK},
        {0x0004, PAD_DPAD_LEFT}, {0x0001, PAD_DPAD_UP}, {0x0008, PAD_DPAD_RIGHT}, {0x0002, PAD_DPAD_DOWN}};
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) pd[map[i].pad] = (w & map[i].bit) != 0;
    pd[PAD_ZL] = st.pad.lt > 128, pd[PAD_ZR] = st.pad.rt > 128;
    raw[0] = st.pad.lx, raw[1] = st.pad.ly, raw[2] = st.pad.rx, raw[3] = st.pad.ry;
    return 1;
}
#endif

#ifdef _WIN32
// 1 when the DualShock 3 reader (input_ds3_win.c) has input beyond rest: a button down or a stick past half way.
static int ds3_pressed(const uint8_t pd[PAD_COUNT], const int32_t raw[4]) {
    for (int p = PAD_SOUTH; p < PAD_COUNT; p++)
        if (pd[p]) return 1;
    for (int i = 0; i < 4; i++)
        if (abs(raw[i]) > PAD_USED) return 1;
    return 0;
}

// Under lock: the DualShock 3 reader's inputs when it is the controller the game follows, else 0. Next to an SDL
// gamepad the one pressed last is followed, as between two SDL gamepads (pad_follow); the same DualShock 3 that a driver
// also presents to SDL is two sources of one device, and either one gives the same presses.
static int ds3_primary(uint8_t pd[PAD_COUNT], int32_t raw[4], int sdl_pad) {
    if (!ds3_read(pd, raw)) return 0;
    if (!sdl_pad) return 1;
    static int follow = 1;
    if (ds3_pressed(pd, raw)) follow = 1;
    else if (pad_active(pad)) follow = 0;
    return follow;
}
#endif

static int pad_read_now(uint64_t *b, int32_t sl[2], int32_t sr[2]);

// Under lock, state 1: the gamepad's buttons (ORed into *b) and sticks; 0 without one. While SDL is finding devices
// again (sdl_enter) the answer is the last one read: what was held stays held for those moments.
static int pad_read(uint64_t *b, int32_t sl[2], int32_t sr[2]) {
    static uint64_t last_b;
    static int32_t last_sl[2], last_sr[2];
    static int last_ok;
    if (sdl_enter()) {
        uint64_t pb = 0;
        int32_t l[2] = {0, 0}, r[2] = {0, 0};
        last_ok = pad_read_now(&pb, l, r);
        sdl_leave();
        last_b = pb, last_sl[0] = l[0], last_sl[1] = l[1], last_sr[0] = r[0], last_sr[1] = r[1];
    }
    if (last_ok) *b |= last_b, sl[0] = last_sl[0], sl[1] = last_sl[1], sr[0] = last_sr[0], sr[1] = last_sr[1];
    return last_ok;
}

static int pad_read_now(uint64_t *b, int32_t sl[2], int32_t sr[2]) {
    uint8_t pd[PAD_COUNT] = {0};
#ifdef _WIN32
    int sdl = pad_physical(pd);
    uint8_t dd[PAD_COUNT] = {0};
    int32_t raw[4];
    if (ds3_primary(dd, raw, sdl)) {
        *b |= input_map_pad_buttons(dd, pad_layout_get());
        input_pad_stick(raw[0], raw[1], stick_dead_get(0), sl);
        input_pad_stick(raw[2], raw[3], stick_dead_get(1), sr);
        return 1;
    }
    if (!sdl) {
        if (!xinput_read(pd, raw)) return 0;
        *b |= input_map_pad_buttons(pd, pad_layout_get());
        input_pad_stick(raw[0], raw[1], stick_dead_get(0), sl);
        input_pad_stick(raw[2], raw[3], stick_dead_get(1), sr);
        return 1;
    }
#else
    if (!pad_physical(pd)) return 0;
#endif
    *b |= input_map_pad_buttons(pd, pad_layout_get());
    stick(SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, stick_dead_get(0), sl);
    stick(SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY, stick_dead_get(1), sr);
    return 1;
}

int input_pad_stick_percent(double pct[2]) {
    int ok = 0;
    pct[0] = pct[1] = 0;
    plat_mutex_lock(&lock);
    int in = state == 1 && plat_mutex_trylock(&sdl_lock);   // not while SDL updates: this reading is skipped then
    int sdl_pad = in && pad && SDL_GamepadConnected(pad);
#ifdef _WIN32
    uint8_t dd[PAD_COUNT];
    int32_t raw[4];
    if (state == 1 && ds3_primary(dd, raw, sdl_pad)) {
        pct[0] = input_stick_percent(raw[0], raw[1]);
        pct[1] = input_stick_percent(raw[2], raw[3]);
        ok = 1;
    } else
#endif
    if (sdl_pad) {   // the game's poll keeps the state fresh (sdl_enter)
        pct[0] = input_stick_percent(pad_axis(pad, SDL_GAMEPAD_AXIS_LEFTX), pad_axis(pad, SDL_GAMEPAD_AXIS_LEFTY));
        pct[1] = input_stick_percent(pad_axis(pad, SDL_GAMEPAD_AXIS_RIGHTX), pad_axis(pad, SDL_GAMEPAD_AXIS_RIGHTY));
        ok = 1;
    }
    if (in) sdl_leave();
    plat_mutex_unlock(&lock);
    return ok;
}

static void poll_merged(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    uint64_t b = 0;
    int32_t sl[2] = {0, 0}, sr[2] = {0, 0};
    plat_mutex_lock(&lock);
    if (!state) start();
    if (state > 0) {
        binds_refresh();
        if (state == 1) pad_read(&b, sl, sr);
        int pad_used = b || abs(sl[0]) > PAD_USED || abs(sl[1]) > PAD_USED || abs(sr[0]) > PAD_USED ||
                       abs(sr[1]) > PAD_USED, kbm_used = 0;
        if (ikb_foreground()) {
            uint8_t down[KEY_COUNT] = {0};
            // Mouse buttons count only while the cursor is captured, and a button already held when the capture began
            // (the click that refocused the window) is ignored until it is released.
            static int was_captured;
            static uint8_t mouse_ignore[KEY_COUNT];
            int cap = ikb_captured();
            // A scan is one key state system call per key (about 95). The game reads several pads in a row: reads
            // within a millisecond of a scan get its answers again.
            static uint8_t raw[KEY_COUNT];
            static uint64_t raw_at;
            uint64_t now = plat_now_ns();
            if (!raw_at || now - raw_at >= 1000000) {
                for (int k = 1; k < KEY_COUNT; k++) raw[k] = (uint8_t)(ikb_down(k) != 0);
                raw_at = now;
            }
            for (int k = 1; k < KEY_COUNT; k++) {
                down[k] = raw[k];
                kbm_used |= down[k];
                if (k < KEY_MOUSE_L || k > KEY_MOUSE_X2) continue;
                if (cap && !was_captured) mouse_ignore[k] = down[k];
                if (!down[k]) mouse_ignore[k] = 0;
                if (!cap || mouse_ignore[k]) down[k] = 0;
            }
            was_captured = cap;
            b |= input_map_buttons(down);
            int32_t k[2];
            walk_get();
            int walk = walk_key != KEY_NONE && down[walk_key];
            input_map_stick(down, 0, k);
            if (walk) k[0] = (int32_t)(k[0] * walk_scale), k[1] = (int32_t)(k[1] * walk_scale);
            pick(&sl[0], &sl[1], k[0], k[1]);
            input_map_stick(down, 1, k);
            if (walk) k[0] = (int32_t)(k[0] * walk_scale), k[1] = (int32_t)(k[1] * walk_scale);
            pick(&sr[0], &sr[1], k[0], k[1]);
            // The buttons whose selector the right stick picks from. None in Ocarina (1.1.1): its D-pad acts at once
            // (ocarina, accessory, Navi, boots), and with L or R held the camera input update goes on taking the
            // mouse; where an update stops taking it, the mouse is the right stick again (mouse_camera).
            int selector = rt_game_version == 111 ? 0
                               : b & (1ull << B_L) ? 1 : b & (1ull << B_UP) ? 2 :
                                 b & (1ull << B_LEFT) ? 3 : b & (1ull << B_RIGHT) ? 4 : 0;
            mouse_camera(k, selector);
            pick(&sr[0], &sr[1], k[0], k[1]);
            int32_t shake = shake_stick(input_map_held(down, B_SHAKE));
            if (shake) sr[0] = shake, sr[1] = 0;
            static uint32_t travel_seen;
            uint32_t travel = ikb_mouse_travel();
            if (travel - travel_seen >= MOUSE_USED) kbm_used = 1;
            travel_seen = travel;
        }
        // A press or a full-way stick on the pad wins over keys held in the same poll: the player picked it up
        if (pad_used) __atomic_store_n(&g_kbm_last, 0, __ATOMIC_RELAXED);
        else if (kbm_used) __atomic_store_n(&g_kbm_last, 1, __ATOMIC_RELAXED);
    }
    script_apply(&b, sl, sr);
    rec_apply(b, sl, sr);
    // Minus and Plus held together (gamepad or keys) open or close the menu and never reach the game
    static int combo_held;
    int32_t st[4] = {sl[0], sl[1], sr[0], sr[1]};
    int toggle = input_menu_gate(&combo_held, menu_is_open(), &b, st);
    static int overlay_held;   // L3+R3 step the performance overlay (F10)
    int overlay = input_overlay_gate(&overlay_held, &b);
    sl[0] = st[0], sl[1] = st[1], sr[0] = st[2], sr[1] = st[3];
    plat_mutex_unlock(&lock);
    if (toggle) menu_toggle();
    if (overlay) menu_perf_cycle();
    stick_bits(&b, sl[0], sl[1], B_SL_LEFT);
    stick_bits(&b, sr[0], sr[1], B_SR_LEFT);
    *buttons = b; *lx = sl[0]; *ly = sl[1]; *rx = sr[0]; *ry = sr[1];
}

// Text entry (swkbd_*.c): while it is open the game reads no input, as behind the keyboard applet; after it closes
// the game reads none until every button and stick direction is released, so the press that closed it stays there.
// g_block: 0 off, 1 text entry open, 2 closed, waiting for the release. The cursor is released while it is open.
// When g_block and g_menu last went to 2 (plat_now_ns). The wait for the release ends after RELEASE_WAIT_NS even with
// something held: an input that never reads as released (a button another program holds down, a key whose release
// went to another window, a stick that rests outside its dead zone) would otherwise keep the game deaf until restart
// (#462, #494). A press that closed the menu is let go well within the wait; one still held after it is meant.
static volatile uint64_t g_block_closed, g_menu_closed;
#define RELEASE_WAIT_NS 1000000000ull

int input_cursor_allowed(void) {
    const char *in = cfg_get("NX2NAVI_INPUT");
    return g_block != 1 && g_menu != 1 && mouse_sens_get() > 0 && !(in && !strcmp(in, "none")) && !menu_cursor_pointer();
}

void input_block(int on) {
    if (!on) __atomic_store_n(&g_block_closed, plat_now_ns(), __ATOMIC_RELEASE);
    plat_xchg32(&g_block, on ? 1 : 2);
    ikb_cursor_update();
}

// The in-game menu (ui/menu.cpp) blocks the game's input the same way: g_menu 0 closed, 1 open, 2 closed, waiting for
// the release. The cursor is released while it is open, for the menu's mouse.
void input_menu(int open) {
    if (!open) __atomic_store_n(&g_menu_closed, plat_now_ns(), __ATOMIC_RELEASE);
    plat_xchg32(&g_menu, open ? 1 : 2);
    if (!open) ikb_menu_closed();   // the capture before Esc comes back
    ikb_cursor_update();
}

// 1 while flag (g_block, g_menu) blocks the game's input; state 2 ends at the first poll with nothing held, or with
// something still held RELEASE_WAIT_NS after closed. A stick axis counts as held as well: with the walk key the
// keyboard's sticks move part way without setting a direction bit, so a gate that watched the buttons alone would let
// that movement through the moment the menu or the text entry closes.
static int blocked(volatile int32_t *flag, volatile uint64_t *closed, const char *what, uint64_t buttons,
                   const int32_t *axes) {
    int32_t k = *flag;
    if (k != 2) return k != 0;
    int held = buttons != 0;
    for (int i = 0; i < 4; i++) held |= axes[i] != 0;
    if (held && plat_now_ns() - __atomic_load_n(closed, __ATOMIC_ACQUIRE) < RELEASE_WAIT_NS) return 1;
    if (plat_cas32(flag, 0, 2) != 2) return *flag != 0;   // reopened meanwhile
    if (held)
        rt_log("input: the %s closed 1 s ago and input is still held (buttons %#llx, sticks %d %d %d %d); the game "
               "gets it from now on", what, (unsigned long long)buttons, axes[0], axes[1], axes[2], axes[3]);
    return 0;
}

void input_poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    poll_merged(buttons, lx, ly, rx, ry);
    if (!g_block && !g_menu) menu_cursor_poll(buttons);
    const int32_t axes[4] = {*lx, *ly, *rx, *ry};
    int text = blocked(&g_block, &g_block_closed, "text entry", *buttons, axes),
        menu = blocked(&g_menu, &g_menu_closed, "menu", *buttons, axes);
    if (!text && !menu) return;
    *buttons = 0; *lx = *ly = *rx = *ry = 0;
}

void input_poll_ui(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    poll_merged(buttons, lx, ly, rx, ry);
}

int input_pad_read(uint64_t *buttons, int32_t *lx, int32_t *ly) {
    uint64_t b = 0;
    int32_t sl[2] = {0, 0}, sr[2] = {0, 0};
    plat_mutex_lock(&lock);
    if (!state) start();
    if (state > 0) binds_refresh();
    int ok = state == 1 && pad_read(&b, sl, sr);
    plat_mutex_unlock(&lock);
    *buttons = b;
    *lx = sl[0];
    *ly = sl[1];
    return ok;
}

uint16_t rumble_motor(float a, float curve, float strength) {
    if (!(a > 0) || !(strength > 0)) return 0;
    float v = powf(a > 1 ? 1 : a, curve) * strength * 65535.0f + 0.5f;
    return v >= 65535.0f ? 65535 : (uint16_t)v;
}

int input_pad_layout(void) {
    return (int)plat_cas32(&pad_layout_shown, 0, 0);
}

int input_pad_label(int pad_id) {
    if (pad_id < PAD_SOUTH || pad_id > PAD_NORTH) return PAD_LABEL_NONE;
    return (int)plat_cas32(&pad_labels[pad_id - PAD_SOUTH], 0, 0);
}

void input_map_snapshot(InputMap *m) {
    plat_mutex_lock(&lock);
    if (!state) start();
    if (state > 0) binds_refresh();
    *m = input_map;
    plat_mutex_unlock(&lock);
}

int input_capture(int reset, int *id) {
    static uint8_t prev_k[KEY_COUNT], prev_p[PAD_COUNT];
    static int prev_esc;
    uint8_t k[KEY_COUNT] = {0}, p[PAD_COUNT] = {0};
    int fg = ikb_foreground();
    for (int i = 1; i < KEY_COUNT; i++) k[i] = fg && ikb_down(i);
    int esc = fg && ikb_escape();
    plat_mutex_lock(&lock);
    if (!state) start();
    if (state == 1 && sdl_enter()) pad_physical(p), sdl_leave();
#ifdef _WIN32
    if (state == 1) {   // a DualShock 3 the reader has: its presses can be bound too
        uint8_t dd[PAD_COUNT];
        int32_t raw[4];
        if (ds3_read(dd, raw))
            for (int i = 1; i < PAD_COUNT; i++) p[i] |= dd[i];
    }
#endif
    plat_mutex_unlock(&lock);
    int got = INPUT_CAPTURE_NONE;
    if (!reset) {
        if (esc && !prev_esc) got = INPUT_CAPTURE_CANCEL;
        for (int i = 1; i < KEY_COUNT && !got; i++)
            if (k[i] && !prev_k[i]) got = INPUT_CAPTURE_KEY, *id = i;
        for (int i = 1; i < PAD_COUNT && !got; i++)
            if (p[i] && !prev_p[i]) got = INPUT_CAPTURE_PAD, *id = i;
    }
    memcpy(prev_k, k, sizeof k);
    memcpy(prev_p, p, sizeof p);
    prev_esc = esc;
    return got;
}

int input_rumble_triggers_enabled(void) {
    return cfg_bool("NX2NAVI_RUMBLE_TRIGGERS");
}

int input_rumble(uint16_t low, uint16_t high, uint32_t ms) {
    int ok = 0;
    static int triggers = -1;
    static unsigned seen;
    if (triggers < 0 || seen != cfg_version()) seen = cfg_version(), triggers = input_rumble_triggers_enabled();
    plat_mutex_lock(&lock);
    if (state == 1 && pad && pad_rumble && plat_mutex_trylock(&sdl_lock)) {   // one skipped while SDL updates
        if (pad_hd != HD_NONE && input_rumble_hd_on()) {
            // HD rumble has this gamepad (hd_tick): its motors are stopped once and get nothing more, so the reports
            // with the game's values are the only rumble it is sent
            if (sdl_rumbling) SDL_RumbleGamepad(pad, 0, 0, 0);
            sdl_rumbling = 0;
            ok = 1;
        } else {
            ok = SDL_RumbleGamepad(pad, low, high, ms);
            sdl_rumbling = low || high;
            if (triggers) SDL_RumbleGamepadTriggers(pad, high, high, ms);   // unsupported pads return false: ignored
        }
        sdl_leave();
    }
#ifdef _WIN32
    if (state == 1 && ds3_rumble(low, high, ms)) ok = 1;   // no-op unless the DualShock 3 reader has a controller
#endif
    plat_mutex_unlock(&lock);
    return ok;
}

// SDL gamepad sensor axes (SDL_sensor.h, a gamepad held in front of you): x to the right, y up out of the face (lying
// flat face up, +y points at the ceiling), z toward the player (out of the bottom edge). The accelerometer reads the
// push against gravity in m/s^2 (lying flat: y = +9.81); the gyro reads rad/s, counter-clockwise positive (right hand).
// Switch six-axis axes (nn::hid): x to the
// right, y toward the top edge (away from the player), z out of the face; acceleration as gravity reads (lying flat:
// z = -1 G). So Switch x = SDL x, Switch y = -SDL z, Switch z = SDL y: a rotation (right-handed to right-handed), so the
// gyro keeps its sense, while the acceleration is also negated (push -> gravity) and scaled to G.
void input_motion_axes(const float a[3], const float g[3], double acc[3], double gyro[3]) {
    acc[0] = -a[0] / SDL_STANDARD_GRAVITY;
    acc[1] = a[2] / SDL_STANDARD_GRAVITY;
    acc[2] = -a[1] / SDL_STANDARD_GRAVITY;
    gyro[0] = g[0];
    gyro[1] = -g[2];
    gyro[2] = g[1];
}

// Under lock: (re)enable the sensors when the open gamepad changed; 1 if it has both and they are on.
static int sensors_ready(void) {
    SDL_JoystickID id = SDL_GetGamepadID(pad);
    if (id == sensor_pad) return sensor_ok;
    sensor_pad = id;
    // Nothing drains the SDL event queue: no event per sample (the data still updates, SDL_SendJoystickSensor)
    SDL_SetEventEnabled(SDL_EVENT_GAMEPAD_SENSOR_UPDATE, false);
    sensor_ok =SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL) &&
                SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true) &&
                SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
    if (sensor_ok) rt_log("input: motion sensors on (gyro %.0f Hz, accelerometer %.0f Hz)",
                          (double)SDL_GetGamepadSensorDataRate(pad, SDL_SENSOR_GYRO),
                          (double)SDL_GetGamepadSensorDataRate(pad, SDL_SENSOR_ACCEL));
    else rt_log("input: \"%s\" has no gyro and accelerometer: the six-axis sensor reads a controller at rest",
                SDL_GetGamepadName(pad));
    return sensor_ok;
}

int input_motion_enabled(void) {
    static int on = -1;
    static unsigned seen;
    unsigned v = cfg_version();
    if (on < 0 || v != seen) {
        seen = v;
        on = cfg_bool("NX2NAVI_MOTION");
    }
    return on;
}

// 250 Hz: the gamepads update (reports read, rumble expiry) and the newest gyro and accelerometer sample goes into the
// fusion. Without a gamepad with sensors, or with motion off (NX2NAVI_MOTION=0, live), nothing is fed and the six-axis
// sensor reads at rest.
static void motion_main(void *arg) {
    (void)arg;
    for (;;) {
        plat_wait_ns(4000000);
        float a[3], g[3];
        int have = 0;
        plat_mutex_lock(&lock);
        if (pad && sdl_enter()) {   // no sample while SDL finds devices again
            have = SDL_GamepadConnected(pad) && sensors_ready() &&
                   SDL_GetGamepadSensorData(pad, SDL_SENSOR_ACCEL, a, 3) && SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, g, 3);
            sdl_leave();
        }
        plat_mutex_unlock(&lock);
        if (!have || !input_motion_enabled()) continue;
        double acc[3], gyro[3];
        input_motion_axes(a, g, acc, gyro);
        motion_feed(motion_global(), acc, gyro, kern_now_ns());
    }
}

void input_motion_start(void) {
    plat_mutex_lock(&lock);
    if (!state) start();
    if (!motion_thread) {
        if (!input_motion_enabled()) rt_log("input: motion off (NX2NAVI_MOTION=0); it can be turned on while the game runs");
        if (state != 1) motion_thread = -1;
        else {
            if (!plat_thread_spawn(motion_main, NULL, "motion", 0)) rt_fatal("input: cannot start the motion thread");
            motion_thread = 1;
        }
    }
    plat_mutex_unlock(&lock);
}

// --probe-input: the gamepad path on SDL's virtual test gamepad (NX2NAVI_TEST_PAD). The pad thread comes up, a poll
// reads the gamepad, polls are quick, and while an update takes as long as a device search (up_test_ns) the polls go on
// with their last state instead of waiting for it. Needs no game files and no controller.
int probe_gamepad(void) {
    int fails = 0;
    cfg_setenv("NX2NAVI_TEST_PAD", "20000,-12000,0,0");
    uint64_t b = 0;
    int32_t lx = 0, ly = 0;
    uint64_t t0 = plat_now_ns();
    int ok = input_pad_read(&b, &lx, &ly);
    printf("input: first poll %.1f ms (SDL comes up on the pad thread), gamepad %s\n", (plat_now_ns() - t0) / 1e6,
           ok ? "read" : "not read");
    if (!ok) fails++, printf("FAIL: no gamepad was read, with the test gamepad attached\n");
    plat_mutex_lock(&lock);
    const char *nm = pad && plat_mutex_trylock(&sdl_lock) ? SDL_GetGamepadName(pad) : NULL;
    int test = nm && strstr(nm, "test gamepad");
    if (nm) sdl_leave();
    plat_mutex_unlock(&lock);
    if (test && (lx < 8000 || ly == 0)) fails++, printf("FAIL: the test gamepad's left stick read %d,%d\n", lx, ly);
    if (!test) printf("input: another gamepad is the primary one; the stick values are not checked\n");
    // ordinary polls: each asks the pad thread for an update unless one is fresh
    uint64_t worst = 0, sum = 0;
    enum { N = 400 };
    for (int i = 0; i < N; i++) {
        plat_wait_ns(700000);   // past UP_FRESH_NS: every poll asks
        uint64_t p0 = plat_now_ns();
        int32_t x, y;
        ok = input_pad_read(&b, &x, &y);
        uint64_t d = plat_now_ns() - p0;
        sum += d;
        if (d > worst) worst = d;
        if (!ok || (test && x != lx)) { fails++, printf("FAIL: poll %d read %d (stick %d, was %d)\n", i, ok, x, lx); break; }
    }
    printf("input: %d polls, each with its own update: %.0f us on average, %.0f us the longest\n", N, sum / 1e3 / N, worst / 1e3);
    if (worst > UP_WAIT_NS + 3000000) fails++, printf("FAIL: a poll took %.1f ms\n", worst / 1e6);
    uint64_t stale0 = up_stale;
    // an update that takes 300 ms: the first poll waits UP_WAIT_NS for it, the others not at all, and all get the stick
    __atomic_store_n(&up_test_ns, 300000000ull, __ATOMIC_RELAXED);
    plat_wait_ns(700000);
    uint64_t s0 = plat_now_ns(), first = 0, rest = 0;
    int n = 0;
    while (plat_now_ns() - s0 < 250000000ull) {
        uint64_t p0 = plat_now_ns();
        int32_t x, y;
        ok = input_pad_read(&b, &x, &y);
        uint64_t d = plat_now_ns() - p0;
        if (!n) first = d;
        else if (d > rest) rest = d;
        n++;
        if (!ok || (test && x != lx)) { fails++, printf("FAIL: a poll during the long update read %d (stick %d, was %d)\n", ok, x, lx); break; }
        plat_wait_ns(1000000);
    }
    printf("input: during a 300 ms update: %d polls, the first %.1f ms, the longest after it %.0f us, %llu went on with "
           "the last state\n", n, first / 1e6, rest / 1e3, (unsigned long long)(up_stale - stale0));
    if (first > UP_WAIT_NS + 3000000 || rest > 1500000 || up_stale - stale0 < (uint64_t)n - 1)
        fails++, printf("FAIL: polls waited for the long update\n");
    plat_wait_ns(120000000);   // the long update ends: polls are fresh again
    uint64_t g0 = up_gen;
    ok = input_pad_read(&b, &lx, &ly);
    if (!ok || up_gen == g0) fails++, printf("FAIL: no update after the long one\n");
    double pct[2];
    input_pad_stick_percent(pct);
    input_rumble(1000, 1000, 10);
    printf("probe-input: %s\n", fails ? "FAILED" : "passed");
    return fails != 0;
}
