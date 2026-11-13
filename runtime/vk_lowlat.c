// Low latency without frame generation (NX2NAVI_LOW_LATENCY: 0 off, 1 on, 2 on with boost; default 0). In a run that
// starts with it on, a change takes effect at once; a run that starts with it off makes its device without the
// drivers' extensions (vk_device.c create_device), so turning it on then takes a restart.
//
// A frame's delay from the pad to the screen has two parts the runtime can shorten. The first is the time the frame
// waits after it is drawn: the frame limit used to hold a finished picture back until its slot (vk_backend.c
// present_main), so the pad was read up to a whole period before the picture left. The second is the queue in front of
// the GPU: when the GPU is the slower side, the game draws frames faster than they are shown and each waits its turn.
// Both go when the wait moves to the frame's start, before the pad is read: the driver knows how long the GPU took and
// when the last picture left, and holds the next frame's start until its picture can go out as soon as it is drawn.
//
// Two drivers offer that, and the device enables the one it lists (vk_device.c create_device):
// - NVIDIA, VK_NV_low_latency2. The window's swapchain is made with VkSwapchainLatencyCreateInfoNV (gfx_present.c
//   create_swapchain) and the sleep mode is set on it: low latency, boost for the value 2, and the frame limit as the
//   minimum interval. Each frame asks the driver for its sleep (vkLatencySleepNV, which signals a timeline semaphore
//   when the frame may start) and marks its stages with the frame's number: simulation start, input sample,
//   simulation end, render submit start and end, present start and end. The frame's queue submissions and its
//   present carry the same number (VkLatencySubmissionPresentIdNV, VkPresentIdKHR).
// - AMD, VK_AMD_anti_lag. vkAntiLagUpdateAMD is called with the input stage where the frame starts (the driver waits
//   inside the call) and with the present stage before the frame's present, both with the frame's number and the frame
//   limit in frames a second.
// Any other driver has neither: the setting does nothing there and the menu's row says so.
//
// Threads. The game's frame is made on guest threads and shown by the present thread, so the calls are split:
// - guest thread, where the frame starts (vk_lowlat_frame_start): the first pad read (ov_input.c npad_states) or
//   window image acquire (nvn_api_obj.c nvnWindowAcquireTexture) after a present, whichever the game makes first. The
//   sleep, then the simulation-start marker; the pad read sets the input marker. The thread gives its virtual core
//   up for the sleep, as around every host wait.
// - guest thread, the frame's first nvnQueueSubmitCommands (vk_lowlat_submit_start): simulation end, render submit
//   start. The game's own threads cannot be told apart, so the simulation ends where the first commands are handed over.
// - the thread that replays (the guest's, or the replay thread with NX2NAVI_REPLAY_ASYNC), at the frame's present
//   (vk_lowlat_submitted): render submit end. Its queue submissions are tagged in vk_sync.c queue_submit.
// - present thread (gfx_present.c flip_locked): the present stage or the present markers around the host present.
// The frame's number is the count of the game's presents, as vk_backend.c numbers the frame it hands to the present
// thread (PresentJob.frame): a guest thread starts frame n while n - 1 presents were made, and the replay and the
// present thread get n with the frame's work. A frame the present leaves out (vk_backend.c frame_hold) simply has no
// present stage.
//
// One limiter. With the mode on, the driver's sleep holds the frames to the frame limit and the runtime's own pacer
// holds nothing: nvnQueuePresentTexture asks vk_lowlat_pace, and on 1 gives its present the time now and frees the
// window image at once. Both together would wait twice, and the pacer's wait is the one after the picture is drawn,
// which is the delay the mode is there to remove. The pacer keeps the limit where the driver cannot: while a movie
// plays (nvn_api_obj.c pace keeps the movie's slots through a late frame, which a driver's limiter does not), while no
// host present goes out (a minimized window: the driver would have nothing to time its sleep by, so the sleep is left
// out too), and before the first host present. RivaTuner Statistics Server is kept out as with frame generation
// (frame_rate.c frame_step_cap_ns): it would hold the finished picture as the pacer did. NX2NAVI_PACE_SMOOTH and
// NX2NAVI_FRAME_HOLD_MS act on the present thread as before.
//
// Frame generation. DLSS Frame Generation through the vendor's interposer brings its own low latency
// (vk_dlssg.c, NX2NAVI_REFLEX): a device its proxies make does not get these extensions and this path stays off for
// the run. AMD's frame generation puts its own swapchain object in the window swapchain's place, which NVIDIA's calls
// cannot be given, so NVIDIA's path is off while it is there and on again with the plain swapchain; AMD's path has no
// swapchain in its calls and runs beside it. NVIDIA's frame generation with the runtime's own presenter (gfx_present.c
// ng_run) presents several times a rendered frame, which the driver would take for frames: off while it presents.
// Each is decided where the state changes: at device creation, at each swapchain creation, and at the present that
// starts or stops the presenter.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_dlssg.h"
#include "vk_fg.h"
#include "vk_lowlat.h"

enum { LL_NONE, LL_NV, LL_AMD };
#define RING 64                      // frames between a frame's start and its present are a handful at most
#define HOLD_IDLE_NS 200000000ull    // no host present for this long: the driver's sleep is left out
#define SLEEP_MAX_NS 500000000ull    // the longest wait for the driver to end a sleep
#define SLEEP_FAILS 3                // that many waits in a row running out: the mode is given up for the run
#define REPORTS 16                   // frames of the driver's report read for a statistics line
#define PACE_WINDOW 60               // frames over which the driver's hold of the frame limit is checked
#define MODE_UNSET 0xffffffffu

#define LOAD(p) __atomic_load_n(p, __ATOMIC_ACQUIRE)
#define STORE(p, v) __atomic_store_n(p, v, __ATOMIC_RELEASE)

static struct {
    int kind;            // LL_*: fixed by vk_lowlat_init
    int why;             // vk_lowlat_status while kind is LL_NONE
    int present_ids;     // the presentId feature is on: the present carries the frame's number
    VkDevice dev;
    PFN_vkSetLatencySleepModeNV set_mode;
    PFN_vkLatencySleepNV sleep;
    PFN_vkSetLatencyMarkerNV marker;
    PFN_vkGetLatencyTimingsNV timings;
    PFN_vkAntiLagUpdateAMD antilag;
    VkSemaphore sem;          // NVIDIA: the timeline the driver signals to end a sleep
    // NVIDIA's calls name the swapchain and nothing says they may run at once on two threads: all of them, and the
    // swapchain's replacement, hold m. None of them waits (the sleep's wait is on the semaphore, outside m). AMD's
    // stage calls hold it shared and its switch-off exclusively (amd_stage).
    PlatMutex m;
    VkSwapchainKHR sc;        // m: the swapchain made with the latency mode, or none
    uint32_t mode;            // m to write, atomic to read: the sleep mode as last set on sc (mode_want), or MODE_UNSET
    uint64_t sleep_value;     // m: the semaphore value of the newest sleep
    // atomics
    int attached;             // sc is set
    int on;                   // the direct path is in effect
    int said;                 // on, as last logged
    int failed;               // the driver refused the mode or did not end its sleeps: off for the run
    int fg_block;             // vk_lowlat_fg_presents
    int pace_bad;             // the driver's sleep did not hold the frame limit: the pacer holds it (vk_lowlat_pace)
    int cfg_read, level;      // the setting as last parsed (level_now)
    unsigned cfg_seen;
    uint32_t interval_us;     // the period the driver holds frames to, 0 while the pacer does (vk_lowlat_pace)
    uint32_t fails;           // sleeps in a row whose wait ran out
    uint64_t game;            // the game's presents so far
    uint64_t started;         // the newest frame that started
    uint64_t input_id;        // the newest frame whose pad read was marked
    uint64_t submit_id;       // the newest frame whose first submission was marked
    uint64_t cur;             // the frame of the present thread's next blit
    uint64_t hosted_ns;       // kern_now_ns after the newest host present of a frame this path followed
    uint64_t ring[RING];      // [n % RING] == n: frame n started with the path on, so its later stages are marked
    uint64_t staged[RING];    // AMD, the same: frame n's input stage was called, so its present stage is too
    // statistics since the last line (vk_lowlat_present_end)
    uint64_t n_sleep, sleep_ns, n_input, n_unheld, n_timeout;
} L = {.why = VK_LOWLAT_NO_DRIVER, .mode = MODE_UNSET};

// NX2NAVI_LOW_LATENCY as a number, parsed again when a setting changed.
static int level_now(void) {
    unsigned v = cfg_version();
    if (!LOAD(&L.cfg_read) || v != LOAD(&L.cfg_seen)) {
        int n = cfg_int("NX2NAVI_LOW_LATENCY");
        STORE(&L.level, n < 0 ? 0 : n > 2 ? 2 : n);
        STORE(&L.cfg_seen, v);
        STORE(&L.cfg_read, 1);
    }
    return LOAD(&L.level);
}

static int level_eff(void) { return LOAD(&L.failed) ? 0 : level_now(); }

static int frame_on(uint64_t id) { return id && LOAD(&L.ring[id % RING]) == id; }

static void say(int on, const char *why) {
    static int32_t lines;
    if (__atomic_exchange_n(&L.said, on, __ATOMIC_ACQ_REL) == on || plat_inc32(&lines) > 40) return;
    if (on)
        rt_log("lowlat: low latency on%s: the driver holds each frame's start, and the frame limit with it",
               L.kind == LL_NV && level_now() == 2 ? " with boost" : "");
    else rt_log("lowlat: low latency off (%s): the game's own limiter paces the frames", why);
}

static void give_up(const char *what, int r) {
    if (__atomic_exchange_n(&L.failed, 1, __ATOMIC_ACQ_REL)) return;
    rt_log("lowlat: %s (%d): low latency is off for the rest of the run", what, r);
}

// ---- NVIDIA
// The sleep mode in one word: bits 0-1 the level (0 off, 1 low latency, 2 with boost), the rest the minimum interval
// in microseconds. 0 while the setting is off or the runtime's frame generation presenter presents.
static uint32_t mode_want(void) {
    int lv = LOAD(&L.fg_block) ? 0 : level_eff();
    return lv ? (uint32_t)lv | LOAD(&L.interval_us) << 2 : 0;
}

// m held, sc set. vkSetLatencySleepModeNV: sc is a live swapchain of dev (it is replaced only under m), the structure
// has its type; the call returns at once.
static void nv_mode_locked(uint32_t want) {
    if (want == L.mode) return;
    VkLatencySleepModeInfoNV mi = {.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV,
                                   .lowLatencyMode = (want & 3) != 0, .lowLatencyBoost = (want & 3) == 2,
                                   .minimumIntervalUs = want >> 2};
    VkResult r = L.set_mode(L.dev, L.sc, &mi);
    if (r != VK_SUCCESS) {
        give_up("the driver refused the low-latency sleep mode", (int)r);
        want = 0;   // taken as off: nothing sleeps on it
    }
    STORE(&L.mode, want);
    STORE(&L.on, (want & 3) != 0);
    say((want & 3) != 0, LOAD(&L.failed) ? "the driver refused it" : LOAD(&L.fg_block) ? "frame generation presents the frames"
                                                                                       : "the setting");
}

static void nv_marker(VkLatencyMarkerNV marker, uint64_t id) {
    VkSetLatencyMarkerInfoNV mi = {.sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV, .presentID = id, .marker = marker};
    plat_mutex_lock(&L.m);
    if (L.sc) L.marker(L.dev, L.sc, &mi);
    plat_mutex_unlock(&L.m);
}

// The driver's sleep for the frame about to start: vkLatencySleepNV returns at once and signals the timeline semaphore
// with the value given when the frame may start. Each sleep takes the next value, so a wait that ran out leaves a value
// behind that a later signal passes. 1 when the driver ended the sleep.
static int nv_sleep(void) {
    VkLatencySleepInfoNV si = {.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV, .signalSemaphore = L.sem};
    VkResult r = VK_NOT_READY;
    plat_mutex_lock(&L.m);
    if (L.sc && L.mode != MODE_UNSET && (L.mode & 3)) {
        si.value = ++L.sleep_value;
        r = L.sleep(L.dev, L.sc, &si);
    }
    plat_mutex_unlock(&L.m);
    if (r == VK_NOT_READY) return 0;   // the swapchain went, or the mode was switched off, since the frame started
    if (r != VK_SUCCESS) {
        vk_device_lost_check(r, "the low-latency sleep");
        give_up("the driver refused the low-latency sleep", (int)r);
        return 0;
    }
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &L.sem,
                              .pValues = &si.value};
    r = vkWaitSemaphores(L.dev, &wi, SLEEP_MAX_NS);
    vk_device_lost_check(r, "the wait for the low-latency sleep");
    if (r == VK_SUCCESS) {
        STORE(&L.fails, 0);
        return 1;
    }
    __atomic_add_fetch(&L.n_timeout, 1, __ATOMIC_RELAXED);
    if (__atomic_add_fetch(&L.fails, 1, __ATOMIC_ACQ_REL) >= SLEEP_FAILS)
        give_up("the driver did not end its low-latency sleep three times in a row", (int)r);
    return 0;
}

// ---- AMD
// vkAntiLagUpdateAMD with a stage of frame id: the antiLag feature is enabled (vk_device.c create_device keeps
// has_anti_lag only with it), both structures have their types, the mode and the stage are values of their enums.
// The stage calls hold m shared, and make none once the path is off: the input stage waits inside the call at a
// frame's start while the present thread makes the present stage of the frame before, which must not wait for it
// (the command has no parameter the caller has to synchronize, so the two may run at once). The switch-off (refresh)
// holds m exclusively: it waits for a stage call in flight, so a stage call with the mode on cannot come after it and
// leave the driver on.
static void amd_stage(VkAntiLagStageAMD stage, uint64_t id) {
    uint32_t us = LOAD(&L.interval_us);
    VkAntiLagPresentationInfoAMD pi = {.sType = VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD, .stage = stage,
                                       .frameIndex = id};
    VkAntiLagDataAMD d = {.sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD, .mode = VK_ANTI_LAG_MODE_ON_AMD,
                          .maxFPS = us ? (1000000u + us / 2) / us : 0, .pPresentationInfo = &pi};
    plat_mutex_lock_shared(&L.m);
    if (LOAD(&L.on)) L.antilag(L.dev, &d);
    plat_mutex_unlock_shared(&L.m);
}

// ---- both
// The guest thread at a frame's start: brings the driver's mode in step with the setting, the frame limit and what
// presents the frames. 1 while the direct path is in effect.
static int refresh(void) {
    if (L.kind == LL_AMD) {
        int want = level_eff() > 0, was = __atomic_exchange_n(&L.on, want, __ATOMIC_ACQ_REL);
        if (was && !want) {   // switched off: told once, without a stage, after any stage call in flight (amd_stage)
            VkAntiLagDataAMD d = {.sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD, .mode = VK_ANTI_LAG_MODE_OFF_AMD};
            plat_mutex_lock(&L.m);
            if (!LOAD(&L.on)) L.antilag(L.dev, &d);   // not when another thread switched it on again meanwhile
            plat_mutex_unlock(&L.m);
        }
        say(want, LOAD(&L.failed) ? "the driver refused it" : "the setting");
        return want;
    }
    if (!LOAD(&L.attached)) return 0;   // no swapchain with the mode: on is 0 (vk_lowlat_swapchain)
    uint32_t want = mode_want();
    if (want != LOAD(&L.mode)) {
        plat_mutex_lock(&L.m);
        if (L.sc) nv_mode_locked(want);
        plat_mutex_unlock(&L.m);
    }
    return LOAD(&L.on);
}

static void frame_begin(uint64_t id, int at_input) {
    uint64_t t0 = kern_now_ns(), hosted = LOAD(&L.hosted_ns);
    STORE(&L.ring[id % RING], id);
    if (hosted && t0 - hosted < HOLD_IDLE_NS) {
        int away = kern_core_away();   // 0 where the caller gave the core up already (the acquire)
        uint64_t p0 = perf_begin();
        if (L.kind == LL_NV) nv_sleep();
        else STORE(&L.staged[id % RING], id), amd_stage(VK_ANTI_LAG_STAGE_INPUT_AMD, id);
        perf_end(PERF_PACE, p0);
        kern_core_back(&away);
        __atomic_add_fetch(&L.sleep_ns, kern_now_ns() - t0, __ATOMIC_RELAXED);
        __atomic_add_fetch(&L.n_sleep, 1, __ATOMIC_RELAXED);
        if (at_input) __atomic_add_fetch(&L.n_input, 1, __ATOMIC_RELAXED);
    } else {
        __atomic_add_fetch(&L.n_unheld, 1, __ATOMIC_RELAXED);
    }
    if (L.kind == LL_NV) nv_marker(VK_LATENCY_MARKER_SIMULATION_START_NV, id);
}

void vk_lowlat_frame_start(int at_input) {
    if (L.kind == LL_NONE) return;
    if (!refresh() || vk_device_is_lost()) return;
    uint64_t id = LOAD(&L.game) + 1, prev = LOAD(&L.started);
    // once a frame, whichever guest thread comes first: the game reads several pads and may acquire again
    if (prev != id && __atomic_compare_exchange_n(&L.started, &prev, id, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        frame_begin(id, at_input);
    if (!at_input || L.kind != LL_NV) return;
    prev = LOAD(&L.input_id);
    if (prev != id && __atomic_compare_exchange_n(&L.input_id, &prev, id, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED) && frame_on(id))
        nv_marker(VK_LATENCY_MARKER_INPUT_SAMPLE_NV, id);
}

void vk_lowlat_submit_start(void) {
    if (L.kind != LL_NV || !LOAD(&L.on)) return;
    uint64_t id = LOAD(&L.game) + 1, prev = LOAD(&L.submit_id);
    if (prev == id || !frame_on(id)) return;
    if (!__atomic_compare_exchange_n(&L.submit_id, &prev, id, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return;
    nv_marker(VK_LATENCY_MARKER_SIMULATION_END_NV, id);
    nv_marker(VK_LATENCY_MARKER_RENDERSUBMIT_START_NV, id);
}

void vk_lowlat_game_present(void) { __atomic_add_fetch(&L.game, 1, __ATOMIC_RELEASE); }

void vk_lowlat_submitted(uint64_t frame) {
    if (L.kind == LL_NV && frame_on(frame)) nv_marker(VK_LATENCY_MARKER_RENDERSUBMIT_END_NV, frame);
}

// The pacer gives the frame limit up only while the driver keeps it, and that is checked: of every PACE_WINDOW frames
// the driver was to hold, at most half may come sooner than three quarters of the period after the frame before. More
// means nothing holds the game (a driver that ignores the interval, or a game whose pad read and image acquire are not
// on the thread that makes the frame, so the sleep held another thread), and a game that runs free can outrun what
// its own code survives (frame_rate.c LIMIT_FREE_MAX_HZ). The pacer then holds the limit for the rest of the run and
// the driver's sleep goes on without an interval.
int vk_lowlat_pace(uint64_t period_ns) {
    static uint64_t last_ns, last_period;   // the presenting guest thread's: the newest present the driver was to hold
    static int window, fast;
    if (!LOAD(&L.on)) return 0;
    uint64_t now = kern_now_ns(), hosted = LOAD(&L.hosted_ns);
    if (last_period && last_period == period_ns && !LOAD(&L.pace_bad)) {
        if (now - last_ns < period_ns / 4 * 3) fast++;
        if (++window >= PACE_WINDOW) {
            if (fast > PACE_WINDOW / 2) {
                STORE(&L.pace_bad, 1);
                rt_log("lowlat: the driver's sleep does not hold the frame limit (%d of %d frames came sooner than three "
                       "quarters of %.2f ms): the game's own limiter holds it for the rest of the run", fast, PACE_WINDOW,
                       (double)period_ns / 1e6);
            }
            window = fast = 0;
        }
    }
    uint64_t us = period_ns && !LOAD(&L.pace_bad) && hosted && now - hosted < HOLD_IDLE_NS ? (period_ns + 500) / 1000 : 0;
    if (us > 0x3fffffffu) us = 0x3fffffffu;
    STORE(&L.interval_us, (uint32_t)us);
    last_ns = now, last_period = us ? period_ns : 0;
    return us != 0;
}

// VkLatencySubmissionPresentIdNV extends VkSubmitInfo2 where the extension is enabled, which kind LL_NV says.
static void tag_fill(VkLatencySubmissionPresentIdNV *tag, uint64_t id) {
    *tag = (VkLatencySubmissionPresentIdNV){.sType = VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV, .presentID = id};
}

// The frame being replayed is the one after the newest present the replaying thread counted (vk_backend.c: the guest
// thread in vk_be_present, the replay thread at its present item).
int vk_lowlat_submit_tag(VkLatencySubmissionPresentIdNV *tag) {
    if (L.kind != LL_NV || !LOAD(&L.on)) return 0;
    uint64_t id = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    if (!frame_on(id)) return 0;
    tag_fill(tag, id);
    return 1;
}

void vk_lowlat_present_frame(uint64_t frame) { STORE(&L.cur, frame); }

uint64_t vk_lowlat_blit_frame(VkLatencySubmissionPresentIdNV *tag, int *tagged) {
    *tagged = 0;
    if (L.kind == LL_NONE) return 0;
    uint64_t id = __atomic_exchange_n(&L.cur, 0, __ATOMIC_ACQ_REL);
    if (!frame_on(id)) return 0;
    if (L.kind == LL_NV) tag_fill(tag, id), *tagged = 1;
    return id;
}

// The caller holds the present lock, under which the swapchain is replaced too: sc, when set, is the swapchain this
// present goes to. VkPresentIdKHR: one swapchain as in the present, the presentId feature on (present_ids), and the
// frame numbers of a swapchain's presents only grow (a frame is presented once, in order; a new swapchain starts over).
const void *vk_lowlat_present_begin(uint64_t frame, VkLowlatPresent *p) {
    p->id = 0;
    if (L.kind == LL_NONE || !frame_on(frame)) return NULL;
    p->id = frame;
    if (L.kind == LL_AMD) {
        if (LOAD(&L.on) && LOAD(&L.staged[frame % RING]) == frame) amd_stage(VK_ANTI_LAG_STAGE_PRESENT_AMD, frame);
        return NULL;
    }
    VkSetLatencyMarkerInfoNV mi = {.sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV, .presentID = frame,
                                   .marker = VK_LATENCY_MARKER_PRESENT_START_NV};
    plat_mutex_lock(&L.m);
    int have = L.sc != VK_NULL_HANDLE;
    if (have) L.marker(L.dev, L.sc, &mi);
    plat_mutex_unlock(&L.m);
    if (!have || !L.present_ids) return NULL;
    p->info = (VkPresentIdKHR){.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .swapchainCount = 1, .pPresentIds = &p->id};
    return &p->info;
}

// The driver's own account of the newest frames (vkGetLatencyTimingsNV; every report has its type, the count says how
// many the array holds and comes back as how many were written), averaged into text for the statistics line.
static void nv_report(char *out, size_t cap) {
    static VkLatencyTimingsFrameReportNV rep[REPORTS];   // the present thread's
    memset(rep, 0, sizeof rep);
    for (int i = 0; i < REPORTS; i++) rep[i].sType = VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV;
    VkGetLatencyMarkerInfoNV gi = {.sType = VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV, .timingCount = REPORTS, .pTimings = rep};
    plat_mutex_lock(&L.m);
    if (L.sc) L.timings(L.dev, L.sc, &gi);
    else gi.timingCount = 0;
    plat_mutex_unlock(&L.m);
    uint32_t n = gi.timingCount > REPORTS ? REPORTS : gi.timingCount, used = 0;
    double total = 0, sim = 0, sub = 0, pres = 0, drv = 0, osq = 0, gpu = 0;
#define SPAN(a, b) ((b) > (a) ? (double)((b) - (a)) / 1000.0 : 0.0)
    for (uint32_t i = 0; i < n; i++) {
        const VkLatencyTimingsFrameReportNV *t = &rep[i];
        uint64_t from = t->inputSampleTimeUs ? t->inputSampleTimeUs : t->simStartTimeUs;
        if (!from || t->gpuRenderEndTimeUs <= from) continue;   // a frame the driver has not finished timing
        used++;
        total += SPAN(from, t->gpuRenderEndTimeUs), sim += SPAN(t->simStartTimeUs, t->simEndTimeUs);
        sub += SPAN(t->renderSubmitStartTimeUs, t->renderSubmitEndTimeUs), pres += SPAN(t->presentStartTimeUs, t->presentEndTimeUs);
        drv += SPAN(t->driverStartTimeUs, t->driverEndTimeUs), osq += SPAN(t->osRenderQueueStartTimeUs, t->osRenderQueueEndTimeUs);
        gpu += SPAN(t->gpuRenderStartTimeUs, t->gpuRenderEndTimeUs);
    }
#undef SPAN
    if (!used) {
        snprintf(out, cap, "; the driver reported no frame timings (%u reports)", n);
        return;
    }
    snprintf(out, cap, "; the driver's report over %u frames: pad read to GPU done %.2f ms (simulation %.2f, render "
             "submit %.2f, present call %.2f, driver %.2f, OS queue %.2f, GPU %.2f)", used, total / used, sim / used,
             sub / used, pres / used, drv / used, osq / used, gpu / used);
}

// One line after the first 600 frames the path followed and every 3600 after that (every 600 with NX2NAVI_TRACE=perf):
// how many frame starts the driver held and for how long, where they were held, and the driver's report.
static void stats(uint64_t frame) {
    static uint64_t shown, next = 600;   // the present thread's
    if (++shown < next) return;
    next = shown + (rt_trace & RT_TRACE_PERF ? 600 : 3600);
    uint64_t n = __atomic_exchange_n(&L.n_sleep, 0, __ATOMIC_RELAXED), ns = __atomic_exchange_n(&L.sleep_ns, 0, __ATOMIC_RELAXED);
    uint64_t in = __atomic_exchange_n(&L.n_input, 0, __ATOMIC_RELAXED), un = __atomic_exchange_n(&L.n_unheld, 0, __ATOMIC_RELAXED);
    uint64_t late = __atomic_exchange_n(&L.n_timeout, 0, __ATOMIC_RELAXED);
    uint32_t us = LOAD(&L.interval_us);
    char limit[96], drv[320] = "";
    if (us) snprintf(limit, sizeof limit, "the driver holds frames to %.3f ms", us / 1000.0);
    else snprintf(limit, sizeof limit, "the game's own limiter holds the frame limit");
    if (L.kind == LL_NV) nv_report(drv, sizeof drv);
    rt_log("lowlat: frame %llu: %s; %llu frame starts held %.2f ms on average, %llu at the pad read and %llu at the "
           "image acquire; %llu not held, %llu sleeps not ended in time%s", (unsigned long long)frame, limit,
           (unsigned long long)n, n ? (double)ns / (double)n / 1e6 : 0.0, (unsigned long long)in,
           (unsigned long long)(n - in), (unsigned long long)un, (unsigned long long)late, drv);
}

void vk_lowlat_present_end(const VkLowlatPresent *p) {
    if (!p->id) return;
    if (L.kind == LL_NV) nv_marker(VK_LATENCY_MARKER_PRESENT_END_NV, p->id);
    STORE(&L.hosted_ns, kern_now_ns());
    stats(p->id);
}

// ---- the swapchain and the state
int vk_lowlat_swapchain_wanted(void) { return L.kind == LL_NV && level_eff() > 0; }

void vk_lowlat_swapchain(VkSwapchainKHR sc, int made) {
    if (L.kind != LL_NV) return;
    plat_mutex_lock(&L.m);
    L.sc = sc;
    STORE(&L.mode, MODE_UNSET);   // a new swapchain has no mode yet; none has none
    STORE(&L.on, 0);
    if (sc) nv_mode_locked(mode_want());
    STORE(&L.attached, sc != VK_NULL_HANDLE);
    plat_mutex_unlock(&L.m);
    // a swapchain was made and it is not one for this path, with the setting on: a frame generation's (a resize in
    // between says nothing: the next swapchain is there a moment later)
    if (made && !sc && level_eff() > 0) say(0, "a frame generation's swapchain is in the window swapchain's place");
}

void vk_lowlat_swapchain_refused(int result) { give_up("no swapchain could be made with the latency mode", result); }

void vk_lowlat_fg_presents(int on) {
    if (L.kind == LL_NV) STORE(&L.fg_block, on != 0);   // taken up at the next frame's start (refresh)
}

int vk_lowlat_on(void) { return LOAD(&L.on); }

int vk_lowlat_status(void) {
    if (L.kind == LL_NONE) return L.why;
    if (LOAD(&L.failed)) return VK_LOWLAT_FAILED;
    if (L.kind == LL_AMD) return VK_LOWLAT_AMD;
    return vk_fg_active() || LOAD(&L.fg_block) ? VK_LOWLAT_FG : VK_LOWLAT_NV;
}

void vk_lowlat_init(VkCtx *c) {
    int level = level_now();
    L.dev = c->device;
    L.kind = LL_NONE, L.why = VK_LOWLAT_NO_DRIVER, L.present_ids = 0;   // a probe makes the device more than once
    STORE(&L.failed, 0);
    STORE(&L.pace_bad, 0);
    if (vk_dlssg_dpa()) {
        L.why = VK_LOWLAT_DLSSG;
        rt_log("lowlat: DLSS Frame Generation's interposer owns low latency in this run (NX2NAVI_REFLEX); the direct "
               "path is off (NX2NAVI_LOW_LATENCY=%d)", level);
        return;
    }
    if (c->has_low_latency2) {
        L.set_mode = (PFN_vkSetLatencySleepModeNV)vk_dpa(c->device, "vkSetLatencySleepModeNV");
        L.sleep = (PFN_vkLatencySleepNV)vk_dpa(c->device, "vkLatencySleepNV");
        L.marker = (PFN_vkSetLatencyMarkerNV)vk_dpa(c->device, "vkSetLatencyMarkerNV");
        L.timings = (PFN_vkGetLatencyTimingsNV)vk_dpa(c->device, "vkGetLatencyTimingsNV");
        // the sleep's semaphore has to be a timeline semaphore (the timelineSemaphore feature is one the device needs)
        VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
        if (L.set_mode && L.sleep && L.marker && L.timings && vkCreateSemaphore(c->device, &si, NULL, &L.sem) == VK_SUCCESS) {
            L.present_ids = c->has_present_id;
            L.kind = LL_NV;
            rt_log("lowlat: NVIDIA's low latency through VK_NV_low_latency2 (presents %s the frame's number): %s "
                   "(NX2NAVI_LOW_LATENCY=%d)", L.present_ids ? "carry" : "do not carry",
                   level == 2 ? "on with boost" : level ? "on" : "off", level);
            return;
        }
        rt_log("lowlat: VK_NV_low_latency2 is enabled but its entry points or the sleep's semaphore are missing");
    } else if (c->has_anti_lag) {
        L.antilag = (PFN_vkAntiLagUpdateAMD)vk_dpa(c->device, "vkAntiLagUpdateAMD");
        if (L.antilag) {
            L.kind = LL_AMD;
            rt_log("lowlat: AMD's Anti-Lag through VK_AMD_anti_lag: %s (NX2NAVI_LOW_LATENCY=%d)", level ? "on" : "off", level);
            return;
        }
        rt_log("lowlat: VK_AMD_anti_lag is enabled but its entry point is missing");
    }
    if (c->lists_low_latency) {
        L.why = VK_LOWLAT_RESTART;
        rt_log("lowlat: the driver has a low-latency mode; this run started with NX2NAVI_LOW_LATENCY off and made its "
               "device without it, so the setting takes effect at the next start");
        return;
    }
    rt_log("lowlat: this driver has neither VK_NV_low_latency2 nor VK_AMD_anti_lag: no low-latency mode%s",
           level ? " (NX2NAVI_LOW_LATENCY does nothing here)" : "");
}

void vk_lowlat_release(void) {
    L.kind = LL_NONE;
    STORE(&L.on, 0);
    STORE(&L.attached, 0);
    L.sc = VK_NULL_HANDLE;
    if (L.sem) vkDestroySemaphore(L.dev, L.sem, NULL);
    L.sem = VK_NULL_HANDLE;
}
