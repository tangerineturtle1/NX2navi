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
#include "floor_watch.h"
#include "gfx.h"
#include "host.h"
#include "input.h"
#include "layout.h"
#include "platform/platform.h"
#include "vk_dlssg.h"
#include "vk_fg.h"
#include "vk_lowlat.h"

#define STEP_SAMPLES 5
#define STEP_MAX_S (1.0 / 15.0)    // longest step; a longer frame slows the game down
#define STEP_RESET_S 0.25          // a gap this long (loading, a debugger) restarts the average
#define STEP_FIRST_S (1.0 / 60.0)
#define STEP_MIN_S (1.0 / 240.0)   // shortest step; a shorter one makes the game's physics run out of memory

static _Atomic uint64_t *step_word;   // patch page +0: ratio (low float), seconds (high float)
static double step_now = STEP_FIRST_S;   // the step the patch page holds: what the game steps each frame
static _Atomic uint32_t *step_interval;   // patch page +8: seconds * 60 (patches-v143.txt realtime)
static _Atomic uint32_t *ostep_page;   // patch page +256 with the set ostep: frames per update (patches-v111.txt)
static _Atomic uint32_t *carry_page;   // patch page +260 with the set ofollow: 100 / frames per update

// The set ofollow (patches-v111.txt): the three follow-surface modules of the character controller keep only a tenth
// of the supporting floor's velocity when the physics' inverse step is over the number at page +260. The game compares
// with 100, which the inverse step passed at every update above 100 fps; under one frame per update 100 / frames per
// update makes the cut come where it came at 60 fps. At one frame or more it stays 100, so 60 and 30 fps keep the
// game's own cut. Written with frames per update (ocarina_step_write).
static void floor_carry_store(float frames) {
    if (!carry_page || !(frames > 0)) return;
    float cut = frames < 1.0f ? 100.0f / frames : 100.0f;
    uint32_t bits;
    memcpy(&bits, &cut, 4);
    atomic_store_explicit(carry_page, bits, memory_order_release);
}
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
    int follow = rt_patch_set("ofollow");
    if (follow >= 0) {
        carry_page = (_Atomic uint32_t *)((uint8_t *)rt_patch_page() + 260);
        floor_carry_store(1.0f);
        rt_log("frame rate: a moving floor carries a character as at 60 fps: %s",
               rt_patch_enabled(follow) ? "on (set ofollow)" : "off, the game's cut above 100 fps (floor_carry = 0)");
    }
    // physics_steps (patches-v111.txt osolver and osolverfull): 0 the game's solver step count, trunc(frames per update
    // * 4 + 0.001); 1 (the default) that count rounded up; 2 rounded up and at least the 4 of a 60 Hz frame, every
    // update. The two sets patch the same words; this start keeps one of them on.
    int solver = rt_patch_set("osolver"), solver_full = rt_patch_set("osolverfull");
    if (solver_full >= 0) {
        const char *e = cfg_get("NX2NAVI_PHYSICS_STEPS");
        int full = (e && !strcmp(e, "2")) || rt_patch_enabled(solver_full);
        rt_patch_switch(solver_full, full);
        if (full && solver >= 0) rt_patch_disable(solver);
        rt_log("frame rate: physics solver steps an update: %s",
               full ? "rounded up and at least a 60 fps frame's 4 (physics_steps = 2)"
               : solver >= 0 && rt_patch_enabled(solver) ? "rounded up (physics_steps = 1)"
                                                          : "the game's own count (physics_steps = 0)");
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
// With set omovie of recomp/patches-v111.txt (NX2NAVI_MOVIE_PTS, on by default) the event count follows the shown
// picture's timestamp instead of the redraws, so with the cap off the cues no longer run ahead; the cap stays for the
// pacing in both directions and for builds without those sites.
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
// frames, so the two never stack. NX2NAVI_RTSS=0 and frame generation (RTSS would count the generated frames) keep it
// out, and so does a driver's low latency while it is in effect (vk_lowlat.c): the driver holds the limit where the
// frame starts, and RTSS would hold the finished picture, the wait that mode is there to remove.
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
    int rtss = rtss_allowed && !vk_fg_wanted() && !vk_dlssg_active() && !vk_lowlat_on();
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
// The water's ripples keep their remainders per water object in a table of their own (frame_rate_water_host), and so do
// the fixed steps of the water and floor surfaces (frame_rate_sim_host).
#define WHOLE_SLOTS 8
#define WATER_SLOTS 64
typedef struct { uint64_t at; float rem; uint64_t used; } WholeSlot;   // used: whole_calls at the slot's last use
static WholeSlot whole_slot[WHOLE_SLOTS], water_slot[WATER_SLOTS];
static uint64_t whole_calls;
static PlatMutex whole_lock = PLAT_MUTEX_INIT;

static uint32_t cut_fcvtzs(float f) {   // fcvtzs w, s: toward zero, saturating, NaN 0
    if (f != f) return 0;
    if (f >= 2147483648.0f) return 0x7fffffffu;
    if (f <= -2147483648.0f) return 0x80000000u;
    return (uint32_t)(int32_t)f;
}

// The whole part of amount plus the remainder the table slot[n] keeps for `at`; a new `at` takes the slot unused longest.
static uint32_t whole_take(WholeSlot *slot, int n, uint64_t at, float amount) {
    if (!(amount > 0.0f) || amount >= 2147483648.0f) return cut_fcvtzs(amount);
    plat_mutex_lock(&whole_lock);
    int k = -1, oldest = 0;
    for (int i = 0; i < n; i++) {
        if (slot[i].at == at) { k = i; break; }
        if (slot[i].used < slot[oldest].used) oldest = i;
    }
    if (k < 0) k = oldest, slot[k].at = at, slot[k].rem = 0.0f;
    slot[k].used = ++whole_calls;
    float sum = slot[k].rem + amount;
    uint32_t whole = cut_fcvtzs(sum);
    slot[k].rem = sum - (float)whole;
    plat_mutex_unlock(&whole_lock);
    return whole;
}

static uint32_t whole_frames(uint64_t at, float amount) { return whole_take(whole_slot, WHOLE_SLOTS, at, amount); }

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

// The water's ripples (main+0x472e0, patches-v111.txt): s0 holds the frame's step and w9 gets the whole steps in it,
// which the renderer multiplies by the water's substeps. The remainder is kept for the water object in x21, in a table
// of the waters' own: every water surface of a place asks once a frame, and with fewer slots than surfaces each would
// take another's slot in turn, start without a remainder every frame and stand still as it did with the cut.
// NX2NAVI_TRACE=perf: every 10 s, the steps the waters were given beside those the cut alone would have given.
void frame_rate_water_host(Cpu *c) {
    uint32_t bits = (uint32_t)c->v[0];
    float step;
    memcpy(&step, &bits, 4);
    uint32_t whole = whole_take(water_slot, WATER_SLOTS, c->x[21], step);
    c->x[9] = whole;
    if (rt_trace & RT_TRACE_PERF) {
        static uint64_t t0, asked, given, cut;
        uint64_t now = plat_now_ns(), n = 0, g = 0, k = 0, t = 0;
        plat_mutex_lock(&whole_lock);   // the counts: nothing says one thread alone draws the waters
        asked++, given += whole, cut += cut_fcvtzs(step);
        if (!t0) t0 = now;
        if (now - t0 >= 10000000000ull) n = asked, g = given, k = cut, t = now - t0, t0 = now, asked = given = cut = 0;
        plat_mutex_unlock(&whole_lock);
        if (n)
            rt_log("frame rate: the water's ripples got %llu steps in %.1f s, asked %llu times (cut to whole steps "
                   "each time, as the game cuts them, %llu)", (unsigned long long)g, t / 1e9, (unsigned long long)n,
                   (unsigned long long)k);
    }
}

// The fixed steps of the physics' height-field surfaces (patches-v111.txt owater): the water managers (main+0x3875f20,
// main+0x3882660) and the interactive floors' simulation (main+0x632cb0, the spider webs' sinking floor). Each update
// divides its seconds by 1/60 s into s0 and cuts that to the number of steps of exactly 1/60 s it runs (at most 3),
// dropping the fraction, so every frame shorter than 1/60 s ran none. The site calls this between the division and
// the game's own cut: s0 becomes the whole steps in the quotient plus the remainder kept for the object in x19, as a
// float the cut keeps exactly, and only s0 changes (its upper lanes stay zero, as the division left them). A quotient
// that is not positive, or too large for a count, stays for the cut. The remainders are in a table of their own: an
// object that shares slots with the counters above would lose its fraction to them.
// NX2NAVI_TRACE=perf: every 10 s, the steps the objects were given beside those the cut alone would have given.
// Every interactive floor of a place (the Deku Tree's ground webs are such floors) and both water managers ask once an
// update, each for its own object, so the table holds as many as the waters' does: with fewer slots than objects each
// would take another's slot in turn, start without a remainder every update and stand still again above 60 fps.
#define SIM_SLOTS 64
static WholeSlot sim_slot[SIM_SLOTS];

void frame_rate_sim_host(Cpu *c) {
    uint32_t bits = (uint32_t)c->v[0];
    float steps;
    memcpy(&steps, &bits, 4);
    uint32_t whole = whole_take(sim_slot, SIM_SLOTS, c->x[19], steps);
    if (steps > 0.0f && steps < 2147483648.0f) {   // whole_take's own range: outside it the game's cut stands
        float f = (float)whole;
        memcpy(&bits, &f, 4);
        c->v[0] = bits;
    }
    if (rt_trace & RT_TRACE_PERF) {
        static uint64_t t0, asked, given, cut;
        uint64_t now = plat_now_ns(), n = 0, g = 0, k = 0, t = 0;
        plat_mutex_lock(&whole_lock);
        asked++, given += whole, cut += cut_fcvtzs(steps);
        if (!t0) t0 = now;
        if (now - t0 >= 10000000000ull) n = asked, g = given, k = cut, t = now - t0, t0 = now, asked = given = cut = 0;
        plat_mutex_unlock(&whole_lock);
        if (n)
            rt_log("frame rate: the water and floor surfaces got %llu fixed steps in %.1f s, asked %llu times (cut to "
                   "whole steps each time, as the game cuts them, %llu)", (unsigned long long)g, t / 1e9,
                   (unsigned long long)n, (unsigned long long)k);
    }
}

// ---- the sound manager's job at whole steps (patches-v111.txt osound)
// The job (main+0x415300) runs in three phases a frame, the phase in w2: 0 loads the time manager's step, frames per
// update at [[main+0x4c7e890]] +0x10, and hands it to the sound system ([[main+0x4c80c38]] +0xa8, and with its inverse
// +0x18 of the object at +0x10 of [[main+0x4c806d0]]); 1 runs the sound objects' updates, 2 the system's own
// (main+0x3798410). Any other phase returns at once (main+0x4155fc); the task wrappers main+0x4151f0 and main+0x4152d0
// skip a 3 themselves. The sound system counts its fades, delays and parameter ramps in updates of at least one step.
// With the set ostep the step is the display interval in 60 Hz frames, a fraction of one above 60 fps. Fed fractions,
// this game has been reported to keep sounds held after an area unloaded until no voice was left free, so that new
// sounds, then the music, went silent until a restart (not reproduced on this build: the fault takes a few area
// changes above 60 fps). With the set osound the job runs at 60 Hz while the step is a fraction:
// - frame_rate_sound_gate_host, after the job's prologue: phase 0 adds the step to what the job is owed and runs once
//   a whole step has built up, keeping the rest; phases 1 and 2 follow that frame's choice. A job that does not run
//   gets phase 3 in w2, so it returns through its own epilogue.
// - frame_rate_sound_step_host, after phase 0's load of the step: a fraction becomes one whole step.
// A step of one or more (60 fps and below) runs as the game has it and leaves what is owed for the next fractions; a
// step that is not positive (or NaN) runs as the game has it and clears it. So the sound clock keeps the game's on
// average at any frame rate, and at 60 fps the job runs every frame as before.
typedef struct {
    float owed;   // steps given since the job last ran, under one
    int run;      // phase 0's choice, which the frame's other phases follow
} SoundGate;
static SoundGate sound_gate = {0.0f, 1};   // every phase runs until a phase 0 has chosen

static int sound_gate_take(SoundGate *g, uint32_t phase, float step) {
    if (phase != 0) return g->run;
    if (!(step > 0.0f)) {
        g->owed = 0.0f;
        return g->run = 1;
    }
    if (step >= 1.0f) return g->run = 1;
    g->owed += step;
    g->run = g->owed >= 1.0f;
    if (g->run) g->owed -= 1.0f;
    return g->run;
}

// The step the job's phase 0 loads (1 when the time manager is not there: the gate then lets the phase run).
static float sound_job_step(void) {
    uint64_t slot = read_ptr(MOD_MAIN_BASE + 0x4c7e890), mgr = slot ? read_ptr(slot) : 0;
    float step = 1.0f;
    if (mgr && !read_guest(mgr + 0x10, &step, 4)) step = 1.0f;
    return step;
}

// NX2NAVI_TRACE=perf: every 10 s, how often phase 0 ran of the times it was asked.
void frame_rate_sound_gate_host(Cpu *c) {
    uint32_t phase = (uint32_t)c->x[2];
    float step = phase == 0 ? sound_job_step() : 1.0f;
    uint64_t asked = 0, ran = 0, t = 0;
    plat_mutex_lock(&whole_lock);   // the phases may come from different threads
    int run = sound_gate_take(&sound_gate, phase, step);
    if (phase == 0 && (rt_trace & RT_TRACE_PERF)) {
        static uint64_t t0, n, r;
        uint64_t now = plat_now_ns();
        n++, r += (uint64_t)run;
        if (!t0) t0 = now;
        if (now - t0 >= 10000000000ull) asked = n, ran = r, t = now - t0, t0 = now, n = r = 0;
    }
    plat_mutex_unlock(&whole_lock);
    if (!run) c->x[2] = 3;
    if (asked)
        rt_log("frame rate: the sound job ran %llu of %llu frames in %.1f s (step %.3f)", (unsigned long long)ran,
               (unsigned long long)asked, t / 1e9, step);
}

void frame_rate_sound_step_host(Cpu *c) {
    uint32_t bits = (uint32_t)c->v[0];
    float step;
    memcpy(&step, &bits, 4);
    if (step > 0.0f && step < 1.0f) c->v[0] = 0x3f800000u;   // 1.0f, the lanes above it zero as the load left them
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
        floor_carry_store(f);
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

// ---- NX2NAVI_FLOOR_PROBE (floor_probe in nx2navi.ini, 0 by default): Link on a moving floor, one line per update
// (diagnosis, 1.1.1). Above 60 fps Link stands still on the Deku Tree's floating platform while it moves under him, and
// the code that carries him has not been found; these lines give what a run needs to find it. The moving floors are
// the engine's ground groups: their manager is [[[main+0x4c7df00]] +0xc0] +0x130 (the ground control moves write it,
// main+0x3000a90, and its transform setter main+0x386a740 takes it so), the group count at +0x28 and the array of
// 32-byte entries at +0x30, each starting with the group; a group's transform is three rows of four floats at +0x88,
// +0x98 and +0xa8 (main+0x386a7e8..0x386a800), its translation in their fourth lanes. Each update the probe reads every
// group's translation and Link's position (Player +0x31c, player_pos), and logs the step, Link's position and how far
// he moved since the last update, and FLOOR_PROBE_NEAR groups within FLOOR_PROBE_RADIUS of him horizontally (those
// that moved since the last present first, nearest first, then the nearest still ones), each with its translation and
// how far it moved; with no group that near, one line in 30 presents still gives Link alone. At 60 fps the moves of
// Link and of the group under him match while he stands on it. Each line also gives the time manager's base step, game speed and first two time
// categories. The value is the number of lines to write (1 means 4000); off it costs one test per present.
#define FLOOR_PROBE_RADIUS 12.0f
#define FLOOR_PROBE_NEAR 6   // the groups a line names: the moving ones first, then the nearest still ones
// The manager holds a few thousand groups (5345 in the Deku Tree; most entries are empty), and a readability check per
// read costs a system call: at one per group a present the probe made every frame about 1.9 s. So the entries and the
// groups' rows are read in two batched copies a present (memory.c mem_copy_readable: one lock, no system call for
// memory that is neither armed nor probed), and only while there is a player (on the title screen the manager is not
// one yet). A count above FLOOR_PROBE_GROUPS is taken for a wrong manager and skipped.
#define FLOOR_PROBE_GROUPS 16384
static int floor_probe_left = -1;   // lines still to write; -1 before the setting was read
static float *floor_last;            // the groups' translations at the last update (x, y, z each)
static uint32_t floor_last_n;
static float floor_link[3];
static int floor_link_ok;

static int direct_read(uint64_t a, void *out, size_t n) {
    if (!a || !mem_host_readable(a, n)) return 0;
    memcpy(out, (const void *)(uintptr_t)a, n);
    return 1;
}
static uint64_t direct_ptr(uint64_t a) {
    uint64_t v = 0;
    return direct_read(a, &v, 8) ? v : 0;
}

static void floor_probe(void) {
    if (floor_probe_left < 0) {
        const char *e = cfg_get("NX2NAVI_FLOOR_PROBE");
        int n = e && *e ? atoi(e) : 0;
        floor_probe_left = !RT_V111 || n <= 0 ? 0 : n == 1 ? 4000 : n;
        if (floor_probe_left) rt_log("floor probe: on for %d lines", floor_probe_left);
    }
    if (!floor_probe_left) return;
    float link[3];
    int link_ok = player_pos(link);
    if (!link_ok) {   // no player (the title screen, a load): nothing to log, and the manager may not be one yet
        floor_link_ok = 0;
        return;
    }
    uint64_t g = direct_ptr(MOD_MAIN_BASE + 0x4c7df00), p = direct_ptr(g), q = p ? direct_ptr(p + 0xc0) : 0,
             mgr = q ? direct_ptr(q + 0x130) : 0;
    uint32_t count = 0;
    uint64_t array = 0;
    if (!mgr || !direct_read(mgr + 0x28, &count, 4) || !(array = direct_ptr(mgr + 0x30))) return;
    if (count > FLOOR_PROBE_GROUPS) {
        static int told;
        if (!told++) rt_log("floor probe: a ground group count of %u is not plausible: skipped until it is", count);
        return;
    }
    static uint64_t *entry;              // the manager's entries: 32 bytes each, the group first
    static float *rows;                  // each group's three rows of four floats (+0x88, +0x98, +0xa8)
    static uint64_t (*src)[2];
    static uint8_t **dst, *ok;
    if (!floor_last) {
        floor_last = calloc(FLOOR_PROBE_GROUPS * 3, sizeof(float));
        entry = calloc(FLOOR_PROBE_GROUPS * 4, sizeof(uint64_t));
        rows = calloc(FLOOR_PROBE_GROUPS * 12, sizeof(float));
        src = calloc(FLOOR_PROBE_GROUPS, sizeof *src);
        dst = calloc(FLOOR_PROBE_GROUPS, sizeof *dst);
        ok = calloc(FLOOR_PROBE_GROUPS, 1);
    }
    if (!floor_last || !entry || !rows || !src || !dst || !ok || !count) return;
    // the entries' range is checked once a present (one system call); the groups they name are the game's own objects
    // and must lie in the guest heap, which mem_copy_readable then copies from without asking the system
    uint64_t heap = 0, heap_size = 0;
    if (!mem_info(4, &heap) || !mem_info(5, &heap_size) || !mem_host_readable(array, 32ull * count)) return;
    uint8_t *edst = (uint8_t *)entry, eok = 0;
    uint64_t esrc[1][2] = {{array, 32ull * count}};
    if (mem_copy_readable(&edst, (const uint64_t (*)[2])esrc, 1, &eok) != 1) return;
    int n = 0;
    static uint32_t *which;   // the group index of each batched read
    if (!which) which = calloc(FLOOR_PROBE_GROUPS, sizeof *which);
    if (!which) return;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t grp = entry[4 * i];
        if (grp < heap || grp + 0x88 + 48 > heap + heap_size) continue;
        src[n][0] = grp + 0x88, src[n][1] = 48;
        dst[n] = (uint8_t *)(rows + 12 * n);
        which[n++] = i;
    }
    mem_copy_readable(dst, (const uint64_t (*)[2])src, n, ok);
    int near[FLOOR_PROBE_NEAR], nn = 0;
    float near_d2[FLOOR_PROBE_NEAR], near_t[FLOOR_PROBE_NEAR][3], near_m[FLOOR_PROBE_NEAR][3];
    for (int k = 0; k < n; k++) {
        if (!ok[k]) continue;
        uint32_t i = which[k];
        const float *r = rows + 12 * k;
        float t[3] = {r[3], r[7], r[11]}, *last = floor_last + 3 * i;
        if (!isfinite(t[0]) || !isfinite(t[1]) || !isfinite(t[2])) continue;
        float m[3] = {0, 0, 0};
        if (i < floor_last_n) m[0] = t[0] - last[0], m[1] = t[1] - last[1], m[2] = t[2] - last[2];
        memcpy(last, t, sizeof t);
        float d2 = (t[0] - link[0]) * (t[0] - link[0]) + (t[2] - link[2]) * (t[2] - link[2]);
        if (d2 >= FLOOR_PROBE_RADIUS * FLOOR_PROBE_RADIUS) continue;
        // a moving group sorts before every still one, so a moving floor is never crowded out by the still map pieces
        // nearer to Link (a group's translation is its pivot, which for a moved floor can be metres from its surface)
        if (m[0] == 0 && m[1] == 0 && m[2] == 0) d2 += FLOOR_PROBE_RADIUS * FLOOR_PROBE_RADIUS;
        if (nn == FLOOR_PROBE_NEAR && d2 >= near_d2[nn - 1]) continue;   // insertion by distance, the farthest drops
        int at = nn < FLOOR_PROBE_NEAR ? nn++ : nn - 1;
        while (at > 0 && near_d2[at - 1] > d2) {
            near[at] = near[at - 1], near_d2[at] = near_d2[at - 1];
            memcpy(near_t[at], near_t[at - 1], sizeof near_t[at]);
            memcpy(near_m[at], near_m[at - 1], sizeof near_m[at]);
            at--;
        }
        near[at] = (int)i, near_d2[at] = d2, memcpy(near_t[at], t, sizeof t), memcpy(near_m[at], m, sizeof m);
    }
    floor_last_n = count;
    static unsigned quiet;   // presents since the last line without a group near Link
    if (floor_link_ok && (nn > 0 || ++quiet >= 30)) {
        quiet = 0;
        char line[320 + FLOOR_PROBE_NEAR * 120];
        int o = snprintf(line, sizeof line, "floor probe: step %.4f frames; link %.4f %.4f %.4f moved %.5f %.5f %.5f; "
                         "%d of %u groups within %.0f m", (double)frame_step_frames(), link[0], link[1], link[2],
                         link[0] - floor_link[0], link[1] - floor_link[1], link[2] - floor_link[2], nn, count,
                         (double)FLOOR_PROBE_RADIUS);
        // the time manager **[main+0x4c7e890] the game steps by: +0x1c0 the base step and +0x10 the game speed in 60 Hz
        // frames, and the first two time categories (+0x20 and +0x50: frames, 1 / frames, seconds, 1 / seconds, as
        // main+0x2c84c makes them), so a line shows whether what the physics reads matches the step
        uint64_t tslot = direct_ptr(MOD_MAIN_BASE + 0x4c7e890), tm = tslot ? direct_ptr(tslot) : 0;
        float tb[2] = {0}, tc[8] = {0};
        if (tm && direct_read(tm + 0x1c0, &tb[0], 4) && direct_read(tm + 0x10, &tb[1], 4) &&
            direct_read(tm + 0x20, tc, 16) && direct_read(tm + 0x50, tc + 4, 16) && o > 0 && o < (int)sizeof line)
            o += snprintf(line + o, sizeof line - o,
                          "; time base %.4f speed %.4f cat0 %.4f %.4f %.6f %.3f cat1 %.4f %.4f %.6f %.3f", tb[0], tb[1],
                          tc[0], tc[1], tc[2], tc[3], tc[4], tc[5], tc[6], tc[7]);
        for (int k = 0; k < nn && o > 0 && o < (int)sizeof line; k++)
            o += snprintf(line + o, sizeof line - o, "; group %d at %.4f %.4f %.4f moved %.5f %.5f %.5f", near[k],
                          near_t[k][0], near_t[k][1], near_t[k][2], near_m[k][0], near_m[k][1], near_m[k][2]);
        rt_log("%s", line);
        floor_probe_left--;
    }
    if (link_ok) memcpy(floor_link, link, sizeof link);
    floor_link_ok = link_ok;
}

// ---- NX2NAVI_FLOOR_SCAN (floor_scan in nx2navi.ini, 0 by default): finds the moving floor under Link in guest memory
// (diagnosis, 1.1.1). The Deku Tree's floating platform is not one of the engine's ground groups (floor_probe), and
// where its position lives is not known. The scan arms while Link drifts at the 60 fps rate: a step of one frame, the
// gamepad's left stick under 20%, and for FLOOR_SCAN_ARM presents in a row a horizontal move of 2 to 30 mm a present,
// his height within 5 mm of the present before, and the whole stretch straight (its end within 80% of the sum of its
// moves), so a floor that only sinks does not arm it. It then takes one snapshot of every 4-byte-aligned float triplet
// in the game's heap and alias regions whose x and z lie within 6 m of Link and whose y lies from 4 m below him to
// 0.5 m above (walked as the debug menu's memory search walks them, debug_tools.c dbg_search_new), and logs how long
// that took. FLOOR_SCAN_ARM presents later it keeps the triplets that moved horizontally as far as Link (within a
// fifth, 2 cm at least) and lie more than 0.3 m below him or more than 0.5 m from him horizontally: the floor's
// matrix, body and drive, but not Link's own copies (his actor, body, feet) nor what rides above him. A snapshot that
// keeps none arms the scan again, three times at most. From then on, once a second, it logs Link's position and move
// over that second and one line per survivor (FLOOR_SCAN_SHOW at most) with its address, position and move over the
// same second, so a stretch at a higher frame rate shows each one's own speed. The value is the number of lines to
// write (1 means 20000); off it costs one test per present. NX2NAVI_FLOOR_WATCH switches the scan on too and gets its
// survivors when the logging starts, with those that lie in the platform's rigid body controller unit marked
// (floor_watch.c).
#define FLOOR_SCAN_ARM 30
#define FLOOR_SCAN_MAX 2000000   // candidate triplets a snapshot keeps
#define FLOOR_SCAN_SHOW 64       // survivors logged each second
static int floor_scan_left = -1;   // lines still to write; -1 before the setting was read
static int floor_watching;         // floor_watch.c has the survivors and is not done (NX2NAVI_FLOOR_WATCH)
static struct {
    int state, run, tries;         // state 0: waiting for a drift, 1: filtering, 2: logging
    float link0[3], last[3], start[3], path, sec_link[3];
    int have_last;
    uint64_t *addr, sec_ns;
    float *val;                    // the triplets at the snapshot, then at the last line
    int n;
} fs;

static void floor_scan_snapshot(const float link[3]) {
    enum { PIECE = 16 << 20 };
    if (!fs.addr) fs.addr = malloc(sizeof(uint64_t) * FLOOR_SCAN_MAX);
    if (!fs.val) fs.val = malloc(sizeof(float) * 3 * FLOOR_SCAN_MAX);
    if (!fs.addr || !fs.val) return;
    uint64_t t0 = plat_now_ns(), bytes = 0;
    int over = 0;
    fs.n = 0;
    const RtGuestLayout *l = mem_layout();
    const RtRange ranges[2] = {l->heap, l->alias};
    for (int r = 0; r < 2; r++) {
        uint64_t a = ranges[r].base, end = ranges[r].base + ranges[r].size;
        while (a < end) {
            RtMemoryInfo mi;
            if (mem_query(a, &mi) || !mi.size) break;
            uint64_t stop = mi.base + mi.size < end ? mi.base + mi.size : end;
            if (!mi.state || (mi.perm & 3) != 3) {
                a = stop;
                continue;
            }
            while (a < stop) {
                uint64_t n = stop - a < PIECE ? stop - a : PIECE;
                if (n >= 12 && mem_read_hold(a, n)) {
                    const float *f = (const float *)(uintptr_t)a;
                    uint64_t words = n / 4;
                    bytes += n;
                    for (uint64_t i = 0; i + 2 < words; i++) {
                        float x = f[i];
                        if (!(fabsf(x - link[0]) < 6.0f)) continue;   // the cheap test first: most words fail it
                        float y = f[i + 1] - link[1], z = f[i + 2];
                        if (!(fabsf(z - link[2]) < 6.0f) || !(y > -4.0f && y < 0.5f)) continue;
                        if (fs.n < FLOOR_SCAN_MAX) {
                            fs.addr[fs.n] = a + 4 * i;
                            memcpy(fs.val + 3 * fs.n, f + i, 12);
                            fs.n++;
                        } else
                            over = 1;
                    }
                    mem_read_release();
                }
                a += n;
            }
        }
    }
    memcpy(fs.link0, link, 12);
    rt_log("floor scan: snapshot of %.0f MB in %.0f ms: %d float triplets around and below Link at %.3f %.3f %.3f%s",
           bytes / 1e6, (plat_now_ns() - t0) / 1e6, fs.n, link[0], link[1], link[2],
           over ? " (more were left out)" : "");
}

// Reads the n triplets at fs.addr into out (NAN where the memory could not be read), in one batched copy.
static void floor_scan_read(int n, float *out) {
    static uint64_t (*src)[2];
    static uint8_t **dst, *ok;
    if (!src) src = malloc(sizeof *src * FLOOR_SCAN_MAX);
    if (!dst) dst = malloc(sizeof *dst * FLOOR_SCAN_MAX);
    if (!ok) ok = malloc(FLOOR_SCAN_MAX);
    if (!src || !dst || !ok) return;
    for (int i = 0; i < n; i++) src[i][0] = fs.addr[i], src[i][1] = 12, dst[i] = (uint8_t *)(out + 3 * i);
    mem_copy_readable(dst, (const uint64_t (*)[2])src, n, ok);
    for (int i = 0; i < n; i++)
        if (!ok[i]) out[3 * i] = out[3 * i + 1] = out[3 * i + 2] = NAN;
}

static float hdist(const float a[3], const float b[3]) { return hypotf(a[0] - b[0], a[2] - b[2]); }

// The owner of a floor point (floor_scan, once, when its survivors are known): the 0x500 bytes from 0x400 before the
// point to 0x100 after it as 8-byte words, the words that point into main's data (where its vtables are) named as
// main offsets, and the objects that hold a pointer to it. Those are found by one more walk of the heap and alias
// regions, for the start of each of the FLOOR_SCAN_BASES objects nearest below the point (a word that points into
// main's data is taken as the vtable that starts one), and each holder is logged with the vtable nearest below it. So
// one run gives the class whose object the platform's position is in, and the classes that refer to it (the character
// controller's ground reference among them), for the code that reads it to be found by its vtable.
#define FLOOR_SCAN_BASES 4
#define FLOOR_SCAN_HOLDERS 24
static int main_data_word(uint64_t w) {
    return w >= MOD_MAIN_BASE + 0x4800000 && w < MOD_MAIN_BASE + MOD_MAIN_SIZE && !(w & 7);
}

// The nearest word at or below a (within 0x400) that points into main's data: the vtable of the object a is in.
static uint64_t vtable_below(uint64_t a, uint64_t *where) {
    uint64_t buf[0x80];
    uint8_t ok = 0, *dst = (uint8_t *)buf;
    uint64_t lo = (a & ~7ull) - 0x3f8, src[1][2] = {{lo, sizeof buf}};
    *where = 0;
    if (mem_copy_readable(&dst, (const uint64_t (*)[2])src, 1, &ok) != 1) return 0;
    for (int i = 0x7f; i >= 0; i--)
        if (main_data_word(buf[i])) {
            *where = lo + 8ull * i;
            return buf[i];
        }
    return 0;
}

static void floor_scan_dump(uint64_t a) {
    enum { PIECE = 16 << 20 };
    uint64_t base = (a & ~7ull) - 0x400, words[0xa0];
    uint8_t ok = 0, *dst = (uint8_t *)words;
    uint64_t src[1][2] = {{base, sizeof words}};
    if (mem_copy_readable(&dst, (const uint64_t (*)[2])src, 1, &ok) != 1) {
        rt_log("floor scan: the words around %#llx cannot be read", (unsigned long long)a);
        return;
    }
    rt_log("floor scan: the words from %#llx to %#llx around the floor point %#llx:", (unsigned long long)base,
           (unsigned long long)(base + sizeof words), (unsigned long long)a);
    for (int i = 0; i < 0xa0; i += 4)
        rt_log("floor scan:   %#llx: %016llx %016llx %016llx %016llx", (unsigned long long)(base + 8ull * i),
               (unsigned long long)words[i], (unsigned long long)words[i + 1], (unsigned long long)words[i + 2],
               (unsigned long long)words[i + 3]);
    uint64_t starts[FLOOR_SCAN_BASES];
    int nb = 0;
    for (int i = 0x9f; i >= 0; i--) {
        if (!main_data_word(words[i])) continue;
        uint64_t at = base + 8ull * i;
        rt_log("floor scan:   %#llx (point %+lld) holds main+%#llx", (unsigned long long)at, (long long)(at - a),
               (unsigned long long)(words[i] - MOD_MAIN_BASE));
        if (at <= a && nb < FLOOR_SCAN_BASES) starts[nb++] = at;
    }
    if (!nb) return;
    // the holders: every 8-byte word in the heap and alias regions that holds one of the starts
    uint64_t t0 = plat_now_ns(), holder[FLOOR_SCAN_HOLDERS], held[FLOOR_SCAN_HOLDERS];
    int nh = 0;
    const RtGuestLayout *l = mem_layout();
    const RtRange ranges[2] = {l->heap, l->alias};
    for (int r = 0; r < 2 && nh < FLOOR_SCAN_HOLDERS; r++) {
        uint64_t x = ranges[r].base, end = ranges[r].base + ranges[r].size;
        while (x < end && nh < FLOOR_SCAN_HOLDERS) {
            RtMemoryInfo mi;
            if (mem_query(x, &mi) || !mi.size) break;
            uint64_t stop = mi.base + mi.size < end ? mi.base + mi.size : end;
            if (!mi.state || (mi.perm & 3) != 3) {
                x = stop;
                continue;
            }
            while (x < stop && nh < FLOOR_SCAN_HOLDERS) {
                uint64_t n = stop - x < PIECE ? stop - x : PIECE;
                if (n >= 8 && mem_read_hold(x, n)) {
                    const uint64_t *w = (const uint64_t *)(uintptr_t)x;
                    for (uint64_t i = 0; i < n / 8 && nh < FLOOR_SCAN_HOLDERS; i++)
                        for (int b = 0; b < nb; b++)
                            if (w[i] == starts[b]) holder[nh] = x + 8 * i, held[nh++] = starts[b];
                    mem_read_release();
                }
                x += n;
            }
        }
    }
    rt_log("floor scan: %d holders of the %d object starts below the point found in %.0f ms", nh, nb,
           (plat_now_ns() - t0) / 1e6);
    for (int h = 0; h < nh; h++) {
        uint64_t at = 0, vt = vtable_below(holder[h], &at);
        rt_log("floor scan:   %#llx holds %#llx; its object's vtable main+%#llx at %#llx (holder %+lld)",
               (unsigned long long)holder[h], (unsigned long long)held[h],
               (unsigned long long)(vt ? vt - MOD_MAIN_BASE : 0), (unsigned long long)at,
               (long long)(at ? holder[h] - at : 0));
    }
}

static void floor_scan(const float link[3], int link_ok) {
    if (floor_scan_left < 0) {
        const char *e = cfg_get("NX2NAVI_FLOOR_SCAN");
        int n = e && *e ? atoi(e) : 0;
        if (n <= 0 && floor_watch_wanted()) n = 1;   // the watch takes its addresses from the scan
        floor_scan_left = !RT_V111 || n <= 0 ? 0 : n == 1 ? 20000 : n;
        if (floor_scan_left) rt_log("floor scan: on for %d lines", floor_scan_left);
    }
    if (!floor_scan_left) return;
    if (!link_ok) {
        fs.have_last = 0, fs.run = 0;
        return;
    }
    float move = fs.have_last ? hdist(link, fs.last) : 0, rise = fs.have_last ? fabsf(link[1] - fs.last[1]) : 0;
    int had = fs.have_last;
    memcpy(fs.last, link, 12);
    fs.have_last = 1;
    if (fs.state == 0) {
        double pct[2] = {0, 0};
        int still = !input_pad_stick_percent(pct) || pct[0] < 20.0;
        int drift = had && still && fabsf(frame_step_frames() - 1.0f) < 0.05f && move > 0.002f && move < 0.03f &&
                    rise < 0.005f;
        if (!drift) {
            fs.run = 0;
            return;
        }
        if (!fs.run) memcpy(fs.start, link, 12), fs.path = 0;
        fs.run++, fs.path += move;
        if (fs.run >= FLOOR_SCAN_ARM) {
            int straight = hdist(link, fs.start) >= 0.8f * fs.path;
            fs.run = 0;
            if (straight && fs.tries < 3) {
                fs.tries++;
                floor_scan_snapshot(link);
                if (fs.n) fs.state = 1;
            }
        }
        return;
    }
    if (fs.state == 1) {
        if (++fs.run < FLOOR_SCAN_ARM) return;
        fs.run = 0;
        float d_link = hdist(link, fs.link0);
        static float *now;
        if (!now) now = malloc(sizeof(float) * 3 * FLOOR_SCAN_MAX);
        if (!now) return;
        floor_scan_read(fs.n, now);
        int kept = 0, moved = 0;
        float tol = d_link * 0.2f > 0.02f ? d_link * 0.2f : 0.02f;
        for (int i = 0; i < fs.n; i++) {
            const float *v = now + 3 * i, *w = fs.val + 3 * i;
            if (!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2])) continue;
            float d = hypotf(v[0] - w[0], v[2] - w[2]);
            if (!(fabsf(d - d_link) <= tol) || d < 0.01f) continue;
            moved++;
            if (!(v[1] < link[1] - 0.3f || hdist(v, link) > 0.5f)) continue;   // Link's own copies stay at his feet
            if (kept < FLOOR_SCAN_SHOW) fs.addr[kept] = fs.addr[i], memcpy(fs.val + 3 * kept, v, 12);
            kept++;
        }
        rt_log("floor scan: %d of %d triplets moved with Link (%.3f m in %d presents); %d of those lie below him or "
               "away from his feet%s", moved, fs.n, d_link, FLOOR_SCAN_ARM, kept,
               kept > FLOOR_SCAN_SHOW ? " (the first of them are logged)" : "");
        if (!kept) {   // nothing below him: the next drift arms the scan again
            fs.state = 0, fs.n = 0;
            return;
        }
        fs.n = kept < FLOOR_SCAN_SHOW ? kept : FLOOR_SCAN_SHOW;
        int deep = 0;   // the owner of the survivor deepest below Link: the floor's own body or matrix
        for (int k = 1; k < fs.n; k++)
            if (fs.val[3 * k + 1] < fs.val[3 * deep + 1]) deep = k;
        floor_scan_dump(fs.addr[deep]);
        if (floor_watch_wanted()) {   // the survivors in the platform's rigid body controller unit (floor_watch.c)
            int unit[FLOOR_SCAN_SHOW];
            for (int k = 0; k < fs.n; k++) {
                uint64_t at = 0;
                unit[k] = vtable_below(fs.addr[k], &at) == MOD_MAIN_BASE + 0x4bf8a88;
            }
            floor_watching = floor_watch_found(fs.addr, fs.val, unit, fs.n, link);
        }
        memcpy(fs.sec_link, link, 12);
        fs.sec_ns = plat_now_ns();
        fs.state = 2;
        return;
    }
    // state 2: once a second, Link and each survivor with its move over that second
    uint64_t t = plat_now_ns();
    if (t - fs.sec_ns < 1000000000ull) return;
    double secs = (t - fs.sec_ns) / 1e9;
    fs.sec_ns = t;
    float v[FLOOR_SCAN_SHOW * 3];
    floor_scan_read(fs.n, v);
    rt_log("floor scan: step %.4f frames; link %.4f %.4f %.4f moved %.4f %.4f %.4f in %.3f s (%.3f m/s)",
           (double)frame_step_frames(), link[0], link[1], link[2], link[0] - fs.sec_link[0], link[1] - fs.sec_link[1],
           link[2] - fs.sec_link[2], secs, hdist(link, fs.sec_link) / secs);
    memcpy(fs.sec_link, link, 12);
    floor_scan_left--;
    for (int k = 0; k < fs.n && floor_scan_left > 0; k++) {
        float *w = fs.val + 3 * k, *u = v + 3 * k;
        rt_log("floor scan:   %#llx at %.4f %.4f %.4f moved %.4f %.4f %.4f (%.3f m/s)", (unsigned long long)fs.addr[k],
               u[0], u[1], u[2], u[0] - w[0], u[1] - w[1], u[2] - w[2], hdist(u, w) / secs);
        memcpy(w, u, 12);
        floor_scan_left--;
    }
    if (floor_scan_left < 0) floor_scan_left = 0;
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
    floor_probe();
    if (floor_scan_left || floor_watching) {   // after its first read the scan costs this test alone while off
        float link[3];
        int ok = floor_scan_left > 0 || floor_watching ? player_pos(link) : 0;
        if (floor_scan_left) floor_scan(link, ok);
        if (floor_watching) floor_watching = floor_watch_present(link, ok, frame_step_frames());
    }
    if (rt_trace & RT_TRACE_PERF) {
        time_dump();
        if (frame_free_running()) {   // the game's own step, as its time manager holds it
            double s = ocarina_step_s();
            step = s > 0 ? s : 0;
        }
        speed_report(display_ns, step, interval);
    }
}
