// Dynamic resolution from the GPU's own timestamps: the scene's resolution follows how long the GPU takes per frame,
// so a heavy view renders fewer pixels and keeps the frame time, and a light one goes back to the player's choice. The
// scene size is the one the game composes in (resolution.c), and it moves in the steps DLSS upscales from: quality,
// balanced, performance and ultra performance, 2/3, 0.58, 1/2 and 1/3 of the output on each axis.
//
// NX2NAVI_DYN_RES=1 turns it on (off by default: it changes the picture's sharpness), and it acts only while
// NX2NAVI_DLSS is one of those upscaling modes, which is then the sharpest step it uses; the lowest is performance,
// or ultra performance when that is the player's own mode. While it runs it asks for GPU timestamps
// (perf_gpu_timing_want; vk_sync.c times every submission part) and reads their running total (vk_gpu_busy_all) once
// per presented frame. It averages the busy time and moves one step at a time: down when the average is over the
// target by 8 %, up when it is under 75 % of it, and holds each step for NX2NAVI_DYN_RES_STEP 60 Hz frames of the clock
// (default 45, 750 ms), because a size that changes every few frames is worse to look at than a steady one. The target
// is NX2NAVI_DYN_RES_TARGET milliseconds when that is set (1 to 100), else the frame time of the limit the game draws
// at (frame_rate.c frame_step_game_hz: 1000 / the limit, the monitor's rate for display, the drawn frames with frame
// generation), and 16.7 ms, a 60 Hz frame, while that rate is unknown or unlimited. A fixed 16.7 ms target let the GPU
// take up to 18 ms a frame at full size under a 120 or 144 fps limit, and a hold of 45 presented frames lasted 0.19 s
// at 240 fps. The step in use goes to vk_dlaa.c dlss_mode as an override (dynres_override), which applies it as it
// applies the menu's choice; the player's setting itself never changes.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "platform/platform.h"

#define DYN_LEVELS 4
#define DYN_TARGET_MS 16.7   // the target while the game's rate is unknown or unlimited: a 60 Hz frame
static const char *const DYN_MODE[DYN_LEVELS] = {"quality", "balanced", "performance", "ultra_performance"};

static struct {
    int on;               // dynamic resolution is acting now
    int base, low;        // the player's step (the sharpest used) and the lowest one used
    int level;            // the step in use
    uint64_t held_from;   // plat_now_ns when the step in use was taken
    uint64_t hold_ns;     // how long to hold a step
    double avg_ns;        // moving average of the GPU's busy time per frame
    double target_ms;     // NX2NAVI_DYN_RES_TARGET; under 1: the frame time of the game's rate
    double target_ns;     // the target in use
    double logged_ns;     // the target the log last named (the start line's, or dynres_frame's last line)
    uint64_t last_busy;   // vk_gpu_busy_all at the previous presented frame
    unsigned cfg_seen;    // cfg_version the settings were read at
    char base_mode[32];
} D = {.cfg_seen = ~0u};

static int mode_index(const char *m) {
    for (int i = 0; i < DYN_LEVELS; i++)
        if (m && !strcmp(m, DYN_MODE[i])) return i;
    return -1;
}

// The target in ns: the setting's, else the frame time of the rate the game draws at under the limit.
static double target_now_ns(void) {
    if (D.target_ms >= 1) return D.target_ms * 1e6;
    double hz = frame_step_game_hz();
    return (hz > 0 ? 1000.0 / hz : DYN_TARGET_MS) * 1e6;
}

// The settings, again whenever one changed (the menu switches them live).
static void dynres_read(void) {
    D.cfg_seen = cfg_version();
    const char *m = cfg_value("NX2NAVI_DLSS");
    int want = cfg_bool("NX2NAVI_DYN_RES"), base = mode_index(m);
    D.target_ms = cfg_double("NX2NAVI_DYN_RES_TARGET");
    D.target_ns = target_now_ns();
    int step = cfg_int("NX2NAVI_DYN_RES_STEP");
    if (step < 8) step = 8;
    D.hold_ns = (uint64_t)step * 1000000000ull / 60;
    int on = want && base >= 0;
    if (on && (!D.on || strcmp(m, D.base_mode))) {   // (re)start at the player's step
        D.base = D.level = base;
        D.low = base > 2 ? base : 2;
        D.held_from = plat_now_ns(), D.avg_ns = 0, D.last_busy = vk_gpu_busy_all();
        D.logged_ns = D.target_ns;
        rt_log("resolution: dynamic resolution on: target %.1f ms (%s), each step held %.2f s, DLSS %s down to %s",
               D.target_ns / 1e6, D.target_ms >= 1 ? "as set" : "the frame limit's", D.hold_ns / 1e9,
               DYN_MODE[D.base], DYN_MODE[D.low]);
    } else if (!on && D.on) {
        rt_log("resolution: dynamic resolution off: DLSS %s as set", m);
    } else if (want && base < 0) {
        static int said;
        if (!said++) rt_log("resolution: dynamic resolution needs a DLSS upscaling mode (quality, balanced, performance or "
                            "ultra performance); DLSS is %s", m);
    }
    snprintf(D.base_mode, sizeof D.base_mode, "%s", m);
    D.on = on;
    perf_gpu_timing_want(on);
}

const char *dynres_override(void) { return D.on && D.level != D.base ? DYN_MODE[D.level] : NULL; }

// Once per presented frame, before vk_dlaa.c reads the mode.
void dynres_frame(void) {
    if (cfg_version() != D.cfg_seen) dynres_read();
    if (!D.on) return;
    // The limit, the monitor's rate and frame generation move the target without a dyn_res setting changing, so it is
    // read every frame. A move of more than 1 % from the target the log last named is logged, whether it came from the
    // frame, from the menu (dynres_read takes a changed limit or dyn_res_target already) or since the start line: a
    // measured refresh rate wanders a little. At most 20 lines a run.
    D.target_ns = target_now_ns();
    if (fabs(D.target_ns - D.logged_ns) > D.logged_ns * 0.01) {
        static int said;
        if (said < 20) {
            said++, D.logged_ns = D.target_ns;
            rt_log("resolution: dynamic resolution: target %.2f ms (%s)", D.target_ns / 1e6,
                   D.target_ms >= 1 ? "as set" : "the frame limit's");
        }
    }
    uint64_t now = vk_gpu_busy_all(), d = now - D.last_busy;
    D.last_busy = now;
    if (!d || d > 1000000000ull) return;   // no timing this frame yet, or a stall that says nothing about the scene
    D.avg_ns = D.avg_ns > 0 ? D.avg_ns * 0.9 + (double)d * 0.1 : (double)d;
    static int frames, said;
    if (++frames % 600 == 0 && said < 12) {
        said++;
        rt_log("resolution: dynamic resolution: GPU %.2f ms a frame, DLSS %s, target %.2f ms", D.avg_ns / 1e6,
               DYN_MODE[D.level], D.target_ns / 1e6);
    }
    if (plat_now_ns() - D.held_from < D.hold_ns) return;
    int to = D.level;
    if (D.avg_ns > D.target_ns * 1.08 && D.level < D.low) to = D.level + 1;
    else if (D.avg_ns < D.target_ns * 0.75 && D.level > D.base) to = D.level - 1;
    if (to == D.level) return;
    rt_log("resolution: dynamic resolution: GPU %.2f ms a frame against %.2f ms: DLSS %s -> %s", D.avg_ns / 1e6,
           D.target_ns / 1e6, DYN_MODE[D.level], DYN_MODE[to]);
    D.level = to, D.held_from = plat_now_ns();
}
