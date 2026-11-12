// Contains code Copyright (c) 2026 MacNativeGames contributors (MIT; see THIRD_PARTY_NOTICES.md).
// Frame rate: the frame limit and the real-time frame step, the host half of the patch sets fps60, realtime and cloth
// (build tables patches-v121.txt, patches-v143.txt).
//
// NX2navi's adaptive present-interval controller (main+0x8f26c8) turns the main thread's measured work into a number of
// 60 Hz ticks clamped to [minimum, maximum]; shipping data sets the minimum to 2 (30 fps), the set fps60 lowers it to
// 1. The controller still raises the interval when a frame does not fit. Shipping code then steps the game by whole
// vsyncs (VFR step main+0x7ede6c: ratio = interval * 0.5, seconds = interval / 60; global ratio at main+0x8f2394), so
// game speed is right only when frames take exactly their interval. With the set realtime both read the patch page
// instead: +0 ratio = seconds * 30, +4 seconds (1.4.3: +8 interval = seconds * 60, the producer's nominal interval
// before its half-step coefficient), the moving average of the last 5 display intervals, at most 1/15 s
// (slower frames slow the game down, as on the console) and at least 1/240 s, so a higher frame-rate cap only changes
// the pacer (frame_step_cap_ns, NX2NAVI_FPS_LIMIT). The set cloth gives hair and cloth the same clock and needs realtime.
//
// The floor: with a shorter step (ratio below 0.125) 1.4.3 aborts within seconds of some save loads (issue #241, a
// 280 Hz monitor): Havok's hkDefaultMemoryImpl reports "Out of memory" for the buffer the physics pre-solve (profiler
// scope "TtPreSolve") resizes in main+0x1261630. Forced steps on one save: 1/240 s plays, 1/244 s and shorter
// abort; the field, sky island and fight saves play at 1/280 s, the town save aborts after about 800 frames. The
// same short steps also crash a physics worker reading that buffer's contacts: an access violation in main+0x1292560
// or its caller main+0x1286870 (narrowphase contact setup, reached from the grid task main+0x12f1ea0), reading a
// Havok body's shape at body+0x78 as null or garbage (issues #29 and #92, 240 Hz monitors with fps_limit = display).
// Steps drawn from 1/252..1/230 s crash that save's load in seconds; 1/240..1/230 s plays. The frame limit
// stops at 240 too (LIMIT_MAX_HZ), so the floor only catches the average dipping under 1/240 s at that limit: a
// present late by less than a period keeps the pacer's phase (nvn_api_obj.c, pace), so the next display interval is
// shorter than the period. No limit makes the game run slower than the clock.
//
// The sample is the display time the pacer gives each present (nvnQueuePresentTexture), so the step follows the
// frames as shown, pacing waits included. With the step on, the pacer ignores the game's interval and holds a frame
// only to the frame limit (nvn_api_obj.c present_period): the controller's 2 or 3 vsyncs would only add latency. The
// vsync event stays at 60 Hz whatever the limit: the game does not wait for it each frame (1.4.3 shows its title at 144
// fps with the event at 60 Hz).
// One 64-bit and one 32-bit store per present; the guest reads the floats on its own threads, and each stays finite
// and in range whichever store it sees.
//
// The world clock (frame_step_world_us) sums the steps from the first present that finds the player actor away from
// the origin, where the title screen keeps it (1.1.1 has no player actor until the save has loaded): the end of the
// first save load. Recorded and scripted input (input.c) runs on it, so a replay presses at the same game time at any
// frame rate and after a first load of any length; a later load (fast travel) still shifts what follows by its length.
//
// NX2NAVI_TRACE=perf adds a line per 120 presents: fps, the game seconds stepped against wall seconds, and with
// NX2NAVI_SPEED_PROBE=1 the player's position and ground speed (an independent check: Link runs at the same speed at
// any frame rate when game time follows wall time).
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug_tools.h"
#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"
#include "vk_dlssg.h"
#include "vk_fg.h"

#define STEP_SAMPLES 5
#define STEP_MAX_S (1.0 / 15.0)    // longest step; a longer frame slows the game down
#define STEP_RESET_S 0.25          // a gap this long (loading, a debugger) restarts the average
#define STEP_FIRST_S (1.0 / 60.0)
#define STEP_MIN_S (1.0 / 240.0)   // shortest step; a shorter one makes the game's physics run out of memory

static _Atomic uint64_t *step_word;   // patch page +0: ratio (low float), seconds (high float)
static double step_now = STEP_FIRST_S;   // the step the patch page holds: what the game steps each frame
static _Atomic uint32_t *step_interval;   // patch page +8: seconds * 60 (patches-v143.txt realtime)
static _Atomic uint32_t *ostep_page;   // patch page +256 with the set ostep: frames per update (patches-v111.txt)
static uint64_t last_ns;
static double samples[STEP_SAMPLES];
static unsigned sample_count, sample_next;

static void step_store(float seconds) {
    step_now = seconds;
    float ratio = seconds * 30.0f, interval = seconds * 60.0f;
    uint32_t lo, hi, iv;
    memcpy(&lo, &ratio, 4);
    memcpy(&hi, &seconds, 4);
    memcpy(&iv, &interval, 4);
    atomic_store_explicit(step_word, (uint64_t)lo | (uint64_t)hi << 32, memory_order_release);
    atomic_store_explicit(step_interval, iv, memory_order_release);
}

static void frame_limit_live(void);

void frame_step_init(void) {
    int fps60 = rt_patch_set("fps60"), realtime = rt_patch_set("realtime"), cloth = rt_patch_set("cloth");
    int ostep = rt_patch_set("ostep");
    if (ostep >= 0 && rt_patch_enabled(ostep)) {   // Ocarina: frames per update from the page, one 60 Hz frame until the first present
        ostep_page = (_Atomic uint32_t *)((uint8_t *)rt_patch_page() + 256);
        float one = 1.0f;
        uint32_t bits;
        memcpy(&bits, &one, 4);
        atomic_store_explicit(ostep_page, bits, memory_order_release);
        rt_log("frame rate: the game's frames per update follow the display interval (set ostep)");
    }
    if (realtime < 0) {
        rt_log("frame rate: no patch table, shipping 30 fps whole-vsync steps");
        return;
    }
    if (!rt_patch_enabled(realtime) && rt_patch_enabled(cloth)) {
        rt_patch_disable(cloth);
        rt_log("frame rate: cloth clock off, it needs the real-time step (NX2NAVI_REALTIME=0)");
    }
    if (rt_patch_enabled(realtime)) {
        step_word = (_Atomic uint64_t *)rt_patch_page();
        step_interval = (_Atomic uint32_t *)((uint8_t *)rt_patch_page() + 8);
        step_store((float)STEP_FIRST_S);
    }
    frame_limit_live();   // NX2NAVI_FPS_LIMIT decides the set fps60 from the start
    rt_log("frame rate: present interval %s, %s", rt_patch_enabled(fps60) ? "1..3 (above 30 fps)" : "2..3 (shipping 30 fps)",
           step_word ? "real-time step (5-frame average of display intervals, at most 1/15 s)" : "whole-vsync steps");
}

int frame_step_realtime(void) { return step_word != NULL; }

// The frame limit (menu, NX2NAVI_FPS_LIMIT, live): 30, 60, 90, 120, 144, any whole rate from 30 to 240, or display (the
// refresh rate of the window's monitor, at most 240). A higher number gives 240, the rate of the shortest step the
// game survives (STEP_MIN_S). Unset, the older NX2NAVI_FPS60=0 still means 30. A limit of 30 switches the set
// fps60 off (the shipping minimum interval 2), every other limit switches it on; the controller reads its minimum
// interval from the patched sites each frame. With the real-time step the pacer holds frames to the limit; above 60
// only the pacer changes, as the step's floor is the limit's ceiling. Without it the game steps whole 60 Hz vsyncs and the pacer
// follows the game's interval, so the limit stops at 60.
#define LIMIT_MIN_HZ 30
#define LIMIT_MAX_HZ 240   // 1 / STEP_MIN_S
static int fps60_set = -2;
static int game_interval = 1;   // the present interval of the newest frame (frame_step_present)

// The rate "display" paces at: 0 is a monitor rate not known yet (no window), taken as 60; a real one below the
// lowest limit (a 24 or 25 Hz TV) is held at that limit rather than read as unknown and doubled to 60.
static double shown_hz(double hz) { return hz <= 0 ? 60 : hz < LIMIT_MIN_HZ ? LIMIT_MIN_HZ : hz; }

// A build without the fps60 and realtime patch sets (Ocarina) steps its game by the clock itself: walked by the same
// frame-numbered input script, Link covered half the distance at 120 fps that he covers at 60. There the pacer holds
// frames to the limit alone (nvn_api_obj.c present_period), the game's own 60 Hz interval aside, and the limit may be
// any rate up to LIMIT_FREE_MAX_HZ, or "unlimited", which paces the game at that rate too. Ocarina's code does fail
// past some rate: on the quick menu, where a frame costs the GPU almost nothing, it ran at 640 to 700 fps and after
// about two seconds read address 0 in its own draw code (main+0x175b2a0, the same call stack twice), while 92 to 255
// fps ran on. 240 keeps every common monitor's rate; with frame generation it counts the game's own frames, so the
// screen still shows the generated ones above it.
#define LIMIT_FREE_MAX_HZ 240
int frame_free_running(void) { return rt_patch_set("fps60") < 0 && rt_patch_set("realtime") < 0; }
int frame_limit_adjustable(void) { return 1; }
static unsigned limit_seen;
static int limit_read;
static int limit_hz = -1;   // 0: display; -2: unlimited (free-running builds); -1 before the first read

static int limit_parse(void) {
    const char *e = cfg_get("NX2NAVI_FPS_LIMIT"), *old = cfg_get("NX2NAVI_FPS60");
    if (!e || !*e) return old && !strcmp(old, "0") ? 30 : 60;
    if (!strcmp(e, "display")) return 0;
    int free_run = frame_free_running();
    if (!strcmp(e, "unlimited")) {
        if (free_run) return -2;
        rt_log("frame rate: NX2NAVI_FPS_LIMIT=unlimited needs a game that steps by the clock; %d", LIMIT_MAX_HZ);
        return LIMIT_MAX_HZ;
    }
    char *end;
    long n = strtol(e, &end, 10);
    if (!*end && free_run && n >= LIMIT_MIN_HZ) return n > LIMIT_FREE_MAX_HZ ? LIMIT_FREE_MAX_HZ : (int)n;
    if (!*end && n > LIMIT_MAX_HZ) {
        rt_log("frame rate: NX2NAVI_FPS_LIMIT=%s is above %d, where the game's physics runs out of memory; %d", e,
               LIMIT_MAX_HZ, LIMIT_MAX_HZ);
        return LIMIT_MAX_HZ;
    }
    if (!*end && n >= LIMIT_MIN_HZ) return (int)n;
    rt_log("frame rate: NX2NAVI_FPS_LIMIT=%s is not %d..%d, display or unlimited; 60", e, LIMIT_MIN_HZ, LIMIT_MAX_HZ);
    return 60;
}

static void frame_limit_live(void) {
    unsigned v = cfg_version();
    if (limit_read && v == limit_seen) return;
    limit_read = 1;
    limit_seen = v;
    int hz = limit_parse();
    if (hz == limit_hz) return;
    limit_hz = hz;
    if (frame_free_running()) {
        if (hz == -2) rt_log("frame rate: unlimited (the game steps by the clock), at most %d game frames a second",
                             LIMIT_FREE_MAX_HZ);
        else if (hz) rt_log("frame rate: limit %d fps (the game steps by the clock)", hz);
        else rt_log("frame rate: limit at the monitor's refresh rate (the game steps by the clock)");
        return;
    }
    if (fps60_set == -2) fps60_set = rt_patch_set("fps60");
    if (fps60_set >= 0) rt_patch_switch(fps60_set, hz != 30);
    if ((hz > 60 || hz == 0) && !step_word)
        rt_log("frame rate: limits above 60 need the real-time step (NX2NAVI_REALTIME=1); 60 fps");
    else if (hz) rt_log("frame rate: limit %d fps", hz);
    else if (gfx_window_refresh_hz() > 0) rt_log("frame rate: limit at the monitor's refresh rate (%.3f Hz now)", gfx_window_refresh_hz());
    else rt_log("frame rate: limit at the monitor's refresh rate (60 until the window measures it)");
}

// NX2NAVI_MOVIE_FPS (default 60; 0 = off): the game's frame rate while a movie plays (ov_movie2.c ov_movie_live). A
// movie's event counts the game's updates as 60 Hz frames whatever the step: at 144 fps the intro's narration lines
// started 2.4 times too early, each over the one before, and its closing fade to black came at 65 s of the movie's
// 154, hiding the whole dream. The movies are 60 fps, so the cap shows the same pictures. It is the rate in both
// directions: under a lower limit (40 fps, or 60 shown with frame generation, which is 30 of the game's) the movie ran
// at that fraction of its speed and ended many seconds behind its sound, so the game is paced at the movie's rate
// while one plays, above the limit too.
static int movie_cap_hz(void) {
    static int hz = -1;
    if (hz < 0) {
        const char *e = cfg_get("NX2NAVI_MOVIE_FPS");
        hz = e && *e ? atoi(e) : 60;
        if (hz && hz < LIMIT_MIN_HZ) hz = 60;
    }
    return hz && ov_movie_live() ? hz : 0;
}

// Frames on screen per game frame while frame generation runs: 2 with AMD's (vk_fg.cpp, one generated frame), the
// multiplier with NVIDIA's (vk_dlssg.c, one to three); 1 while neither generates (off, or paused for the menu).
static int generated_factor(void) { return vk_fg_generating() ? 2 : vk_dlssg_multiplier(); }

// The limit counts the frames the game draws, and frame generation adds its frames on top: a limit of 60 at 4x is 60
// drawn and 240 shown. Counted on the screen instead, the game was paced at the limit over the factor, so each step up
// of the multiplier took drawn frames away (60 at 4x drew 15) and the generated ones had less to work from and more
// delay. Only "display" is a rate on the screen, the monitor's: there the game draws that rate over the factor, since a
// frame past the monitor's refresh is never seen. The result is game frames a second, before the ceiling of the step.
static double limit_game_hz(int limit, int gen) {
    if (limit) return shown_hz(limit);
    double hz = shown_hz(gfx_window_refresh_hz());
    return gen > 1 ? hz / gen : hz;
}

// The rate the game renders at under the limit (mouse_direct.c, whose camera input follows the game's own frame time);
// 0 unknown or unlimited.
double frame_step_game_hz(void) {
    if (frame_step_limit_hz() <= 0) return 0;
    double hz = limit_game_hz(limit_hz, generated_factor());
    return !frame_free_running() && !step_word && hz > 60 ? 60 : hz > LIMIT_MAX_HZ ? LIMIT_MAX_HZ : hz;
}

// RivaTuner Statistics Server (rtss_limit.c) gets the same limit; while it limits this process the pacer holds no
// frames, so the two never stack. NX2NAVI_RTSS=0 and frame generation (RTSS would count the generated frames) keep it out.
static double paced_hz;   // the game frames a second the newest frame_step_cap_ns paces at (ocarina_step); 0: none
static int movie_paced;   // the movie rate the newest frame_step_cap_ns paces at (ocarina_step, frame_step_movie_hz); 0: none
int frame_step_movie_hz(void) { return movie_paced; }
uint64_t frame_step_cap_ns(void) {
    frame_limit_live();
    static int rtss_allowed = -1;
    if (rtss_allowed < 0) {
        const char *e = cfg_get("NX2NAVI_RTSS");
        rtss_allowed = !(e && !strcmp(e, "0"));
    }
    int rtss = rtss_allowed && !vk_fg_wanted() && !vk_dlssg_active();
    int gen = generated_factor();
    int movie_hz = movie_cap_hz();
    static int movie_clock = -1;   // NX2NAVI_MOVIE_CLOCK=0: the steady step and the usual pacing through a movie too
    if (movie_clock < 0) {
        const char *e = cfg_get("NX2NAVI_MOVIE_CLOCK");
        movie_clock = !(e && !strcmp(e, "0"));
    }
    movie_paced = movie_clock ? movie_hz : 0;
    static int movie_was, movie_logs;
    if (!!movie_hz != movie_was) {
        movie_was = !!movie_hz;
        if (movie_logs++ < 40) {   // the first 20 movies
            if (movie_hz) rt_log("frame rate: a movie plays: %d fps until it ends (NX2NAVI_MOVIE_FPS)", movie_hz);
            else rt_log("frame rate: the movie ended: the frame limit again");
        }
    }
    if (limit_hz == -2 && !movie_hz && !frame_free_running()) {   // unlimited: the pacer does not hold frames, and
        rtss_limit_want(rtss ? 0 : -1);                              // RTSS's limit is cleared
        paced_hz = 0;
        return 0;
    }
    // unlimited in a build that steps by the clock: LIMIT_FREE_MAX_HZ game frames; during a movie, the movie's rate
    double hz = limit_hz == -2 ? LIMIT_FREE_MAX_HZ : limit_game_hz(limit_hz, gen);
    double max = frame_free_running() ? LIMIT_FREE_MAX_HZ : LIMIT_MAX_HZ;
    if (hz > max) hz = max;
    static double logged_hz;
    static int logged_gen, gen_logs;
    if (gen > 1 && (hz != logged_hz || gen != logged_gen) && gen_logs < 40) {
        gen_logs++, logged_hz = hz, logged_gen = gen;
        rt_log("frame rate: the game draws up to %.0f frames a second and frame generation shows %d for each (%.0f on "
               "screen)", hz, gen, hz * gen);
    }
    if (movie_hz) hz = movie_hz;
    paced_hz = hz;
    rtss_limit_want(rtss ? (int64_t)(hz * 1000 + 0.5) : -1);
    if (rtss_limit_active()) return 0;
    return (uint64_t)(1e9 / hz + 0.5);
}

// The limit as it is set, for the performance overlay: the setting, the monitor's rate for "display", 60 at
// most without the real-time step; the game's own rate (60 Hz over its present interval) in a build without the patch
// sets; 0 before the first frame read the setting.
double frame_step_limit_hz(void) {
    if (frame_free_running()) {   // 0 for unlimited, as before the first read
        frame_limit_live();
        int limit = limit_hz;
        if (limit == -2 || limit == -1) return 0;
        return shown_hz(limit ? limit : gfx_window_refresh_hz());
    }
    int hz = limit_hz;
    if (hz < 0) return 0;
    double r = shown_hz(hz ? hz : gfx_window_refresh_hz());
    return !step_word && r > 60 ? 60 : r > LIMIT_MAX_HZ ? LIMIT_MAX_HZ : r;
}

// ---- NX2NAVI_TRACE=perf speed lines
static uint64_t win_t0, win_frames, win_hist[4];   // win_hist: frames by present interval 1, 2, 3+ ([0] unused)
static double win_game;
static int probe = -1;
static float last_pos[3];
static uint64_t last_pos_ns;

// plat_mem_read copies through the kernel, which fails on a heap page NX2NAVI_HEAP_LAZY has not committed yet
static int read_guest(uint64_t a, void *out, size_t n) {
    if (!a) return 0;
    mem_kernel_access(a, n);
    return plat_mem_read(a, out, n);
}
static uint64_t read_ptr(uint64_t a) {
    uint64_t p = 0;
    return read_guest(a, &p, 8) ? p : 0;
}
// 1.1.1: [[main+0x4e371d0] + 0x480] is the Player actor, its name pointer at +0x20 reads "Player" and its position, in
// metres, is the float triplet at +0x31c. The slot is empty until the first save has loaded and then holds the actor.
// The other builds: [main+0x4728538] (1.1.0: [main+0x4630a68], 1.4.3: [main+0x3add238]) +0x1e8 scene, its component 13
// lists the resident actors; the one named "Player" has its position at +0x2b4.
// The 1.1.0 slot was measured from its own dereference chain: 181 scene accesses in that build
// read it, and one of them reproduces this walk (the 14-component check, component 13, the 0x70 stride and the name
// compare) instruction for instruction.
static int player_name(uint64_t name) {
    char text[7];
    return name && read_guest(name, text, 7) && !memcmp(text, "Player", 7);
}

static int player_pos(float xyz[3]) {
    if (RT_V111) {
        uint64_t manager = read_ptr(MOD_MAIN_BASE + 0x4e371d0), actor = manager ? read_ptr(manager + 0x480) : 0;
        return actor && player_name(read_ptr(actor + 0x20)) && read_guest(actor + 0x31c, xyz, 12) && isfinite(xyz[0]) &&
               isfinite(xyz[2]);
    }
    uint64_t module = read_ptr(MOD_MAIN_BASE + (RT_V143 ? 0x3add238 : RT_V110 ? 0x4630a68 : 0x4728538)),
             scene = module ? read_ptr(module + 0x1e8) : 0;
    int32_t count = 0;
    if (!scene || !read_guest(scene + 0x50, &count, 4) || count < 14 || count > 256) return 0;
    uint64_t comps = read_ptr(scene + 0x58), resident = comps ? read_ptr(comps + 13 * 8) : 0;
    if (!resident || !read_guest(resident + 0x20, &count, 4) || count < 1 || count > 64) return 0;
    uint64_t links = read_ptr(resident + 0x28);
    for (int i = 0; links && i < count; i++) {
        uint64_t data = read_ptr(links + (uint64_t)i * 0x70 + 8), actor = data ? read_ptr(data + 0x40) : 0;
        if (actor && player_name(read_ptr(actor + 0x218)))
            return read_guest(actor + 0x2b4, xyz, 12) && isfinite(xyz[0]) && isfinite(xyz[2]);
    }
    return 0;
}

// Ocarina 1.1.1's time manager, **[main+0x4c7e890]: +0x1c0 the frame's base step in 60 Hz frames, +0x10 the game
// speed (main+0x2c84c makes each time category's {frames, 1/frames, seconds, 1/seconds} from them and the category's
// factor). Read by the speed lines of a free-running build: the game seconds a frame stepped, or -1.
static double ocarina_step_s(void) {
    uint64_t slot = read_ptr(MOD_MAIN_BASE + 0x4c7e890), mgr = slot ? read_ptr(slot) : 0;
    float base = 0, speed = 0;
    if (!mgr || !read_guest(mgr + 0x1c0, &base, 4) || !read_guest(mgr + 0x10, &speed, 4) || !isfinite(base) || !isfinite(speed))
        return -1;
    return base * speed / 60.0;
}

static _Atomic uint64_t guest_presents;   // frame_step_present calls so far (frame_step_flight)

// The flight recorder's view of the step at a host present (flight.c, render thread): the newest step the patch page
// holds in 60 Hz frames per update (set ostep; -1 without), the game's own step from its time manager in 60 Hz frames
// (-1 unknown or not free-running), and the game's presents so far, which name the update those two belong to.
void frame_step_flight(float *page, float *game, uint64_t *presents) {
    *page = ostep_page ? frame_step_frames() : -1.0f;
    double s = frame_free_running() ? ocarina_step_s() : -1;
    *game = s > 0 ? (float)(s * 60.0) : -1.0f;
    *presents = atomic_load_explicit(&guest_presents, memory_order_relaxed);
}

// Free-running builds (Ocarina): the game's step follows the display interval (ocarina_step, in 60 Hz frames), written
// at each present: frames per update on the patch page with the set ostep, else the time manager's base step. The
// game left it at one 60 Hz frame whatever the frame rate, so at 144 fps it ran 2.4 times too fast.
// NX2NAVI_OCARINA_STEP=0 leaves it. Counts how often the game itself had changed the value since the last write
// (diagnosis).
static uint64_t ostep_writes, ostep_changed;
static float ostep_last = -1;

// The step Ocarina is given for a display interval dt, in seconds. The game ships with a step of one 60 Hz frame (two
// at 30 fps) that never changes in play, so nothing in it was written for a step that differs from frame to frame: its
// physics takes the step as the length of the next solve while the bodies' velocities come from the step before
// (the physics system keeps both, main+0x38af1f0), and it derives whole numbers from it (the solver step count,
// patches-v111.txt osolver). The mean of the last 5 intervals, which stood here, moved with every one of them: display
// intervals of 5 and 30 ms in turn gave steps of 15 and 20 ms in turn, and one 60 ms frame at 200 fps took the step
// from 5 to 16 ms for five frames and back. So the step is kept steady and the clock is kept by a slow correction:
//   - the base is the mean of the last 12 intervals without their longest and shortest: one late frame and the short
//     one after it (nvn_api_obj.c pace keeps the phase) leave it alone, intervals alternating in a period of 2, 3 or 4
//     frames give one value, and a real change of frame rate arrives within 12 frames;
//   - a base within 3% of the pacer's period is that period: at a frame limit the game holds, the step is the same
//     number every frame, at 60 fps exactly the one frame the game ships with;
//   - what the display showed and the game was not given is owed (the late frame cut from the base, the 3% of the
//     snap). Past half a step of it the step repays a 64th a frame, 10% of the step at most, so the game still follows
//     the clock and a hitch is paid back over the next second rather than in five frames. More than STEP_RESET_S owed
//     is dropped: the game runs slower than the clock, as it does for a frame longer than STEP_MAX_S.
// The step is at least OSTEP_MIN_S: the solver step count of the set osolver reaches 0 under a 16th of a frame.
// NX2NAVI_STEP_STEADY=0 gives the mean of the last 5 intervals again (comparison runs).
#define OSTEP_WINDOW 12
#define OSTEP_SNAP 0.03
#define OSTEP_REPAY_MAX 0.10
#define OSTEP_MIN_S (1.0 / 480.0)
static double os_ring[OSTEP_WINDOW], os_owed;
static unsigned os_count, os_next;
static double os_step_lo, os_step_hi;   // the steps given since the last speed line (speed_report)

static void ocarina_step_reset(void) {
    os_count = os_next = 0;
    os_owed = 0;
}

// While a movie plays (frame_step_cap_ns paces the game at the movie's rate) the step is exactly one movie frame, whatever
// the display intervals; NX2NAVI_MOVIE_CLOCK=0 leaves movies to the steady step (comparison runs). The game's movie
// player (main+0x3b5ca60) adds the time manager's seconds times 60 to an accumulator at each update and advances the
// movie by its whole pictures, at most the player's limit per update (+0x20, set at main+0x3b5d094: 1, or 2 in one of
// its modes), keeping the rest. The Triforce movie showed one picture per update (600 pictures in 600 updates at 59.6
// updates a second), so the time a late frame left owed, which the steady step pays back as steps over one frame, never
// became pictures: the picture fell behind its sound, which plays by the clock, by each late frame for good. The
// movie's event counts updates as 60 Hz frames too (frame_step_cap_ns). So the updates themselves keep the clock
// (nvn_api_obj.c pace keeps the movie's slots through a late frame) and each counts as one movie frame, for the
// picture, the event and the game's clock alike. The filter starts again at a movie's start and end, so neither rate's
// intervals reach the other.
static double ocarina_step(double dt) {
    static int steady = -1, movie_was;
    if (steady < 0) {
        const char *e = cfg_get("NX2NAVI_STEP_STEADY");
        steady = !(e && !strcmp(e, "0"));
    }
    if (movie_paced != movie_was) {
        movie_was = movie_paced;
        ocarina_step_reset();
    }
    if (movie_paced > 0) {
        double step = 1.0 / movie_paced;
        if (os_step_lo <= 0 || step < os_step_lo) os_step_lo = step;
        if (step > os_step_hi) os_step_hi = step;
        return step;
    }
    if (dt > STEP_MAX_S) dt = STEP_MAX_S;
    os_ring[os_next++ % OSTEP_WINDOW] = dt;
    if (os_count < OSTEP_WINDOW) os_count++;
    double step;
    if (!steady) {
        unsigned n = os_count < STEP_SAMPLES ? os_count : STEP_SAMPLES;
        double sum = 0;
        for (unsigned i = 1; i <= n; i++) sum += os_ring[(os_next - i) % OSTEP_WINDOW];
        step = sum / n;
        if (step < 1.0 / 1000) step = 1.0 / 1000;
    } else {
        double sum = 0, lo = os_ring[0], hi = os_ring[0];
        for (unsigned i = 0; i < os_count; i++) {
            sum += os_ring[i];
            if (os_ring[i] < lo) lo = os_ring[i];
            if (os_ring[i] > hi) hi = os_ring[i];
        }
        double base = os_count >= 4 ? (sum - lo - hi) / (os_count - 2) : sum / os_count;
        double period = paced_hz > 0 ? 1.0 / paced_hz : 0;
        if (period > 0 && fabs(base - period) <= OSTEP_SNAP * period) base = period;
        double over = fabs(os_owed) - base * 0.5, repay = 0;
        if (over > 0) {
            repay = over / 64 < OSTEP_REPAY_MAX * base ? over / 64 : OSTEP_REPAY_MAX * base;
            if (os_owed < 0) repay = -repay;
        }
        step = base + repay;
        if (step < OSTEP_MIN_S) step = OSTEP_MIN_S;
        if (step > STEP_MAX_S) step = STEP_MAX_S;
        os_owed += dt - step;
        if (os_owed > STEP_RESET_S) os_owed = STEP_RESET_S;
        if (os_owed < -STEP_RESET_S) os_owed = -STEP_RESET_S;
    }
    if (os_step_lo <= 0 || step < os_step_lo) os_step_lo = step;
    if (step > os_step_hi) os_step_hi = step;
    return step;
}

// ---- whole frames out of the real-time step (the set ostep's sites that cut it to a whole number, patches-v111.txt)
// The game adds the step, or the step times a whole number, to a few counters and cuts the sum to a whole number: at the
// console's 60 fps the step is 1 and they count frames. With the step the display interval in 60 Hz frames, under one
// above 60 fps, the cut loses the fraction every frame. whole_frames keeps it per counter (by its guest address) and
// hands out the whole part, so each counter moves at the game's pace at any frame rate; at a step of exactly one it
// gives what the game computes. A value that is not positive, or too large for a count, is cut as fcvtzs cuts it.
#define WHOLE_SLOTS 8
static struct { uint64_t at; float rem; uint32_t used; } whole_slot[WHOLE_SLOTS];
static uint32_t whole_calls;
static PlatMutex whole_lock = PLAT_MUTEX_INIT;

static uint32_t cut_fcvtzs(float f) {   // fcvtzs w, s: toward zero, saturating, NaN 0
    if (f != f) return 0;
    if (f >= 2147483648.0f) return 0x7fffffffu;
    if (f <= -2147483648.0f) return 0x80000000u;
    return (uint32_t)(int32_t)f;
}

static uint32_t whole_frames(uint64_t at, float amount) {
    if (!(amount > 0.0f) || amount >= 2147483648.0f) return cut_fcvtzs(amount);
    plat_mutex_lock(&whole_lock);
    int k = -1, oldest = 0;
    for (int i = 0; i < WHOLE_SLOTS; i++) {
        if (whole_slot[i].at == at) { k = i; break; }
        if (whole_slot[i].used < whole_slot[oldest].used) oldest = i;
    }
    if (k < 0) k = oldest, whole_slot[k].at = at, whole_slot[k].rem = 0.0f;
    whole_slot[k].used = ++whole_calls;
    float sum = whole_slot[k].rem + amount;
    uint32_t whole = cut_fcvtzs(sum);
    whole_slot[k].rem = sum - (float)whole;
    plat_mutex_unlock(&whole_lock);
    return whole;
}

// The time of day (main+0x3cdf70, patches-v111.txt): s0 holds twice the step, the game seconds of this frame; w8 gets
// the whole ones, the counter being the seconds of the day at +0x3c of the object in x19.
// NX2NAVI_TRACE=perf: every 10 s, the game seconds the day was given beside those the cut alone would have given.
void frame_rate_day_host(Cpu *c) {
    uint32_t bits = (uint32_t)c->v[0];
    float secs;
    memcpy(&secs, &bits, 4);
    uint32_t whole = whole_frames(c->x[19] + 0x3c, secs);
    c->x[8] = whole;
    if (rt_trace & RT_TRACE_PERF) {
        static uint64_t t0, given, cut;
        static double exact;
        uint64_t now = plat_now_ns();
        given += whole, cut += cut_fcvtzs(secs), exact += secs;
        if (!t0) t0 = now;
        if (now - t0 >= 10000000000ull) {
            rt_log("frame rate: the time of day got %llu game seconds in %.1f s (the frames' step makes %.1f; cut "
                   "to whole seconds each frame, as the game cuts them, %llu)", (unsigned long long)given,
                   (now - t0) / 1e9, exact, (unsigned long long)cut);
            t0 = now, given = cut = 0, exact = 0;
        }
    }
}

// The 60 Hz frames the game steps at each update where the set ostep gives them (Ocarina); 1 elsewhere, the game's own
// nominal frame. The mouse camera divides by it (mouse_direct.c): the game multiplies camera input by this step.
float frame_step_frames(void) {
    if (!ostep_page) return 1.0f;
    uint32_t bits = atomic_load_explicit(ostep_page, memory_order_acquire);
    float f;
    memcpy(&f, &bits, 4);
    return f > 0 ? f : 1.0f;
}

static void ocarina_step_write(double dt) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("NX2NAVI_OCARINA_STEP");
        on = !(e && !strcmp(e, "0"));
    }
    if (!on || dt <= 0) return;
    if (ostep_page) {   // the set ostep: the game makes its whole nominal frame from this, the interface's included
        if (dt < 1.0 / 1000) dt = 1.0 / 1000;
        if (dt > STEP_MAX_S) dt = STEP_MAX_S;
        float f = (float)(dt * 60.0);
        uint32_t bits;
        memcpy(&bits, &f, 4);
        atomic_store_explicit(ostep_page, bits, memory_order_release);
        ostep_last = f;
        return;
    }
    uint64_t slot = read_ptr(MOD_MAIN_BASE + 0x4c7e890), mgr = slot ? read_ptr(slot) : 0;
    if (!mgr || !mem_host_readable(mgr + 0x1c0, 4)) return;
    _Atomic uint32_t *word = (_Atomic uint32_t *)(uintptr_t)(mgr + 0x1c0);
    uint32_t now_bits = atomic_load_explicit(word, memory_order_relaxed);
    float now_v;
    memcpy(&now_v, &now_bits, 4);
    if (ostep_last >= 0 && now_v != ostep_last) ostep_changed++;
    if (dt < 1.0 / 1000) dt = 1.0 / 1000;
    if (dt > STEP_MAX_S) dt = STEP_MAX_S;
    float v = (float)(dt * 60.0);
    uint32_t bits;
    memcpy(&bits, &v, 4);
    atomic_store_explicit(word, bits, memory_order_relaxed);
    ostep_last = v;

    if (++ostep_writes % 1200 == 0)
        rt_log("frame step: Ocarina base step %.3f frames (%.2f ms); the game changed it %llu times in the last 1200 presents",
               v, dt * 1000, (unsigned long long)ostep_changed), ostep_changed = 0;
}

// NX2NAVI_TIME_DUMP=1: the time manager's first 0x280 bytes once, 30 s into the run (diagnosis)
static void time_dump(void) {
    static int done;
    const char *e = cfg_get("NX2NAVI_TIME_DUMP");
    if (done || !e || strcmp(e, "1") || !rt_guest_start_ns || kern_now_ns() - rt_guest_start_ns < 30000000000ull) return;
    done = 1;
    uint64_t slot = read_ptr(MOD_MAIN_BASE + 0x4c7e890), mgr = slot ? read_ptr(slot) : 0;
    uint32_t w[160];
    if (!mgr || !read_guest(mgr, w, sizeof w)) return;
    for (int i = 0; i < 160; i += 8) {
        char line[512];
        int o = snprintf(line, sizeof line, "time dump +%03x:", i * 4);
        for (int k = 0; k < 8; k++) {
            float f;
            memcpy(&f, &w[i + k], 4);
            o += snprintf(line + o, sizeof line - o, " %08x(%g)", w[i + k], (double)f);
        }
        rt_log("%s", line);
    }
}

static void speed_report(uint64_t now, double step, int interval) {
    if (!win_t0) {
        win_t0 = now;
        return;
    }
    win_frames++;
    win_game += step;
    win_hist[interval < 1 ? 1 : interval > 3 ? 3 : interval]++;
    if (win_frames < 120) return;
    if (probe < 0) {
        const char *e = cfg_get("NX2NAVI_SPEED_PROBE");
        probe = e && !strcmp(e, "1");
    }
    double wall = (double)(now - win_t0) * 1e-9;
    char pos[240] = "";
    float p[3];
    if (probe && player_pos(p)) {
        double d = last_pos_ns ? hypot(p[0] - last_pos[0], p[2] - last_pos[2]) : 0,
               s = last_pos_ns ? (double)(now - last_pos_ns) * 1e-9 : 0;
        snprintf(pos, sizeof pos, "; player %.1f %.1f %.1f, %.2f m in %.3f s = %.2f m/s", p[0], p[1], p[2], d, s,
                 s > 0 ? d / s : 0);
        memcpy(last_pos, p, sizeof p);
        last_pos_ns = now;
    }
    float global_step;   // Ocarina 1.1.1: the engine's global step (patches-v111.txt, the set ostep's second site)
    if (probe && ostep_page && read_guest(MOD_MAIN_BASE + 0x4d41fc8, &global_step, 4)) {
        size_t o = strlen(pos);
        snprintf(pos + o, sizeof pos - o, "; global step %.3f frames", (double)global_step);
    }
    if (os_step_hi > 0) {   // Ocarina: the shortest and longest step given in these frames, and the time still owed
        size_t o = strlen(pos);
        snprintf(pos + o, sizeof pos - o, "; step %.3f..%.3f frames, %.1f ms owed", os_step_lo * 60, os_step_hi * 60,
                 os_owed * 1000);
        os_step_lo = os_step_hi = 0;
    }
    if (step_word || win_game > 0)
        rt_log("fps: %llu frames in %.3f s (%.1f fps), %.3f s game stepped, speed %.3f; interval 1/2/3+ %llu/%llu/%llu%s",
               (unsigned long long)win_frames, wall, (double)win_frames / wall, win_game, win_game / wall,
               (unsigned long long)win_hist[1], (unsigned long long)win_hist[2], (unsigned long long)win_hist[3], pos);
    else
        rt_log("fps: %llu frames in %.3f s (%.1f fps), whole-vsync steps; interval 1/2/3+ %llu/%llu/%llu%s",
               (unsigned long long)win_frames, wall, (double)win_frames / wall, (unsigned long long)win_hist[1],
               (unsigned long long)win_hist[2], (unsigned long long)win_hist[3], pos);
    win_t0 = now;
    win_frames = 0;
    win_game = 0;
    memset(win_hist, 0, sizeof win_hist);
}

static _Atomic int64_t world_us = -1;
static _Atomic uint64_t game_us;   // game time stepped since the first present (frame_step_ticks)
static uint64_t presents;

int64_t frame_step_world_us(void) { return atomic_load_explicit(&world_us, memory_order_relaxed); }
uint64_t frame_step_ticks(void) { return atomic_load_explicit(&game_us, memory_order_relaxed) * 60 / 1000000; }

static void world_clock(double dt, double step) {
    presents++;
    int64_t w = atomic_load_explicit(&world_us, memory_order_relaxed);
    if (w < 0) {
        float p[3];
        if (!player_pos(p) || fabsf(p[0]) + fabsf(p[2]) < 1.0f) return;   // a few guest reads, until the load ends
        rt_log("frame step: world clock starts at present %llu, %.2f s after the guest started (the player at %.1f %.1f %.1f)",
               (unsigned long long)presents, rt_guest_start_ns ? (kern_now_ns() - rt_guest_start_ns) / 1e9 : 0.0, p[0],
               p[1], p[2]);
        atomic_store_explicit(&world_us, 0, memory_order_relaxed);
        return;
    }
    if (!step_word) step = dt > STEP_MAX_S ? STEP_MAX_S : dt;   // whole-vsync steps: the display time stands in
    atomic_store_explicit(&world_us, w + (int64_t)(step * 1e6 + 0.5), memory_order_relaxed);
}

void frame_step_present(uint64_t display_ns, int interval) {
    atomic_fetch_add_explicit(&guest_presents, 1, memory_order_relaxed);
    game_interval = interval > 0 ? interval : 1;
    frame_limit_live();
    double step = 0, dt = last_ns ? (double)(display_ns - last_ns) * 1e-9 : 0;
    if (step_word && last_ns) {
        if (dt > STEP_RESET_S || dt < 0.001) {
            sample_count = sample_next = 0;
        } else {
            if (dt > STEP_MAX_S) dt = STEP_MAX_S;
            samples[sample_next++ % STEP_SAMPLES] = dt;
            if (sample_count < STEP_SAMPLES) sample_count++;
            double sum = 0;
            for (unsigned i = 0; i < sample_count; i++) sum += samples[i];
            step = sum / sample_count;
            if (step < STEP_MIN_S) step = STEP_MIN_S;
            step_store((float)step);
        }
    }
    if (frame_free_running() && last_ns) {
        // the debug menu's speed factor (debug_tools.c) multiplies the step the game is given
        if (dt > 0 && dt < STEP_RESET_S) ocarina_step_write(ocarina_step(dt) * (double)dbg_speed_factor());
        else ocarina_step_reset();   // a load or a debugger stop: the intervals before it say nothing about the next
    }
    last_ns = display_ns;
    // The game clock of untimed script lines: what the game stepped this frame, the patch page's step with the
    // real-time step (kept through a reset), whole vsyncs without it.
    double game = step_word ? step_now : ostep_last > 0 ? ostep_last / 60.0 : (interval > 0 ? interval : 1) / 60.0;
    atomic_store_explicit(&game_us, atomic_load_explicit(&game_us, memory_order_relaxed) + (uint64_t)(game * 1e6 + 0.5),
                          memory_order_relaxed);
    world_clock(dt, step);
    if (rt_trace & RT_TRACE_PERF) {
        time_dump();
        if (frame_free_running()) {   // the game's own step, as its time manager holds it
            double s = ocarina_step_s();
            step = s > 0 ? s : 0;
        }
        speed_report(display_ns, step, interval);
    }
}
