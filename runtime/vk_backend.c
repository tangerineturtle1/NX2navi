// The Vulkan graphics back end: gfx_vulkan_backend wires the memory and
// texture layer (vk_memory.c, vk_texture.c), submission and fences (vk_sync.c) and the present. Every entry starts the
// device on first use.
//
// Present: the game texture is resolved on the calling thread and its blit is handed to a present thread, so
// nvnQueuePresentTexture returns at once (section 5). Queue order does the rest: the submissions that wrote the texture
// are already on gfx_queue, and every call that may write or free GPU state the blit reads (submit, finish, pool and
// texture teardown, texel writes) first waits until the queued blit was submitted (vk_present_drain).
//
// Back-edge pacing: the present thread submits the blit at once, then holds the host present (vkQueuePresentKHR) until
// the frame's display time from the pacer (nvn_api_obj.c), so present-to-present intervals stay even whatever the
// replay took. The blit is already on the queue, so the wait blocks no submission. While a driver's low latency holds
// the frames where they start (vk_lowlat.c), the pacer's display time is the time of the game's present, and the host
// present goes out as soon as the blit is submitted.
//
// Asynchronous replay (NX2NAVI_REPLAY_ASYNC, default 0; 1 turns it on): nvnQueueSubmitCommands
// and nvnQueuePresentTexture only queue. Each item gets its timeline value at enqueue (vk_sync_reserve), in queue
// order; the replay thread runs the synchronous submit and present bodies in that order, so every value is signalled
// by the submission it was handed out for. The game runs at most one frame ahead: queueing present N waits until the
// replay thread handled present N-1. Calls that read or free GPU state that queued work may use drain the queue first
// (replay_drain), each cause counted.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "flight.h"
#include "gfx.h"
#include "motion_owner.h"
#include "host.h"
#include "nvn_api.h"
#include "perfmon.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"
#include "vk_dlaa.h"
#include "vk_dlssg.h"
#include "vk_fg.h"
#include "vk_lowlat.h"
#include "vk_frame_owed.h"
#include "vk_present_held.h"

#define WEAK __attribute__((weak))

// handoff_ns: how long the presenting thread waited to hand this job over (present_image), for the present lines.
// owed: the presenting thread left the frame's bookkeeping to the next holder of the submission lock (present_image,
// vk_frame_owed.h); the present thread makes sure it ran before the blit (present_frame_closed).
typedef struct {
    VkImage image;
    VkFormat format;
    int w, h, has_crop, crop[4], owed;
    uint64_t frame, at, queued, value, handoff_ns;
} PresentJob;

static PlatMutex pl = PLAT_MUTEX_INIT;
static PlatCond pcv = PLAT_COND_INIT;   // job queued, job taken, job submitted
static PresentJob job;
static void present_frame_closed(uint64_t frame);   // "a frame's bookkeeping in replay order"
static int pending, busy, present_thread;

// Sleeps on a high-resolution timer to 1 ms before at, then yields until at: the timer alone wakes up to ~0.5 ms late.
static void wait_until(uint64_t at) {
    uint64_t now = kern_now_ns();
    if (at > now + 1500000) plat_wait_ns(at - now - 1000000);
    while (kern_now_ns() < at) plat_yield();
}

// NX2NAVI_PACE_SMOOTH (default 1; 0 = off): a picture goes out no sooner than seven eighths of the recent interval
// after the one before it. The game's frames reach the present thread in uneven steps although each costs about the
// same: with the replay on the submitting thread, work held for an event is replayed with the next frame, so pictures
// came 13 to 31 ms apart around a mean of 22 ms in a village (5th to 95th percentile), which shows as judder at a
// frame rate that would look even. Only the early ones wait, a few milliseconds each, so the rate stays what it was;
// the recent interval is a moving average over about four pictures of intervals under 100 ms, so the floor follows
// a scene that gets lighter within a second. Not with frame generation, which paces its own presents.
static uint64_t pace_avg_ns;   // the present thread's

static uint64_t pace_smooth(uint64_t at, uint64_t last, uint64_t now) {
    static int on = -1;
    if (on < 0) on = cfg_bool("NX2NAVI_PACE_SMOOTH");
    if (!on || !last || !pace_avg_ns || vk_fg_active() || vk_dlssg_active()) return at;
    uint64_t floor = last + pace_avg_ns / 8 * 7;
    return floor > at && floor < now + 50000000ull ? floor : at;
}

static void pace_shown(uint64_t interval_ns) {
    if (interval_ns >= 100000000ull) return;   // a load or a hitch is not the rate
    pace_avg_ns = pace_avg_ns ? pace_avg_ns + (uint64_t)(((int64_t)interval_ns - (int64_t)pace_avg_ns) / 4) : interval_ns;
}

// NX2NAVI_TRACE=perf: the present thread's own window of PW_PRESENTS presents. The render thread's perf lines time the
// game's side and the hand-off; these say where the present thread's time went: the blit and its submission, the
// wait for the picture's time (pace_smooth), and the present call, which takes the queue lock and in which an overlay
// with a frame limiter sleeps. With the presenting thread's wait to hand each picture over, which that time causes.
#define PW_PRESENTS 60
typedef struct { uint64_t n, blit, wait, call, call_max, past, hand, hand_max; } PresentWin;

static void present_window(PresentWin *w, uint64_t frame, uint64_t blit, uint64_t wait, uint64_t call, uint64_t past,
                           uint64_t hand) {
    w->n++, w->blit += blit, w->wait += wait, w->call += call, w->past += past, w->hand += hand;
    if (call > w->call_max) w->call_max = call;
    if (hand > w->hand_max) w->hand_max = hand;
    if (w->n < PW_PRESENTS) return;
    double n = (double)w->n;
    rt_log("perf: present thread frame %llu: %llu presents; blit and submit avg %.2f ms, paced wait avg %.2f ms, present "
           "call avg %.2f ms max %.2f ms, past target avg %.2f ms; the presenting thread waited to hand over avg %.2f ms "
           "max %.2f ms", (unsigned long long)frame, (unsigned long long)w->n, w->blit / 1e6 / n, w->wait / 1e6 / n,
           w->call / 1e6 / n, w->call_max / 1e6, w->past / 1e6 / n, w->hand / 1e6 / n, w->hand_max / 1e6);
    memset(w, 0, sizeof *w);
}

static void present_main(void *arg) {
    (void)arg;
    uint64_t last = 0, last_queued = 0;
    PresentWin win = {0};
    plat_mutex_lock(&pl);
    for (;;) {
        while (!pending) plat_cond_wait(&pcv, &pl);
        PresentJob j = job;
        pending = 0;
        busy = 1;
        plat_cond_broadcast(&pcv);
        plat_mutex_unlock(&pl);
        unsigned tr = rt_trace;   // read once: the stamps and both lines below follow the same flags
        int timed = (tr & (RT_TRACE_PRESENT | RT_TRACE_PERF)) != 0;
        uint64_t b0 = timed ? kern_now_ns() : 0;
        if (j.owed) present_frame_closed(j.frame);   // vk_dlaa_frame's state of this frame, before anything reads it
        if (flight_on || vk_dump_white) vk_thumbs_frame(j.frame);   // NX2NAVI_DUMP_WHITE reads them too
        vk_dlssg_present_frame(j.frame);
        vk_lowlat_present_frame(j.frame);   // the frame's number for the blit's submit and the present's markers
        vk_present_blit(j.image, VK_IMAGE_LAYOUT_GENERAL, j.format, j.w, j.h, j.has_crop ? j.crop : NULL);
        uint64_t f = vk_sync_submit(NULL, 0);   // a timeline value after the blit: deferred destruction waits for the blit too
        if (j.value && f != j.value)
            rt_fatal("vk: the present of frame %llu signalled %llu, queued as %llu", (unsigned long long)j.frame,
                     (unsigned long long)f, (unsigned long long)j.value);
        plat_mutex_lock(&pl);
        busy = 0;
        plat_cond_broadcast(&pcv);
        plat_mutex_unlock(&pl);
        uint64_t t0 = kern_now_ns();
        wait_until(pace_smooth(j.at, last, t0));
        uint64_t t1 = kern_now_ns();
        if (last) pace_shown(t1 - last);
        vk_present_flip();
        uint64_t t2 = timed ? kern_now_ns() : 0;
        perfmon_shown(t1);   // the overlay's frame times: the pictures as shown
        // NX2NAVI_TRACE=present: one line per present: the host present's interval, the interval of the game's present
        // calls (when the host present went out before back-edge pacing), the wait and how far past its target it went;
        // then how long the present call took and how long the presenting thread waited to hand this picture over
        if (tr & RT_TRACE_PRESENT)
            rt_log("present: frame %llu interval %.3f ms, game %.3f ms, waited %.3f ms, %.3f ms past target; present call "
                   "%.3f ms, hand-off %.3f ms", (unsigned long long)j.frame, last ? (t1 - last) / 1e6 : 0.0,
                   last_queued ? (j.queued - last_queued) / 1e6 : 0.0, (t1 - t0) / 1e6,
                   j.at && t1 > j.at ? (t1 - j.at) / 1e6 : 0.0, (t2 - t1) / 1e6, j.handoff_ns / 1e6);
        if (tr & RT_TRACE_PERF)
            present_window(&win, j.frame, t0 - b0, t1 - t0, t2 - t1, j.at && t1 > j.at ? t1 - j.at : 0, j.handoff_ns);
        last = t1;
        last_queued = j.queued;
        vk_pipeline_autosave();   // starts the autosave thread once
        plat_mutex_lock(&pl);
    }
    return;
}

void vk_present_drain(void) {
    uint64_t p0 = perf_begin();
    plat_mutex_lock(&pl);
    while (pending || busy) plat_cond_wait(&pcv, &pl);
    plat_mutex_unlock(&pl);
    perf_end(PERF_DRAIN, p0);
}

// NX2NAVI_FRAME_HOLD_MS (default 120; 0 = off): a frame whose draws were skipped because a shader or pipeline was
// still building (vk_exec_draw.c pending_draws) is not shown: the previous picture stays on screen until a frame is
// complete again. Such a frame lacks what was skipped, which showed for a frame or two as a black or unlit picture with
// the interface intact (a lighting pass), a light going out, or ground and walls missing and then back. The game runs
// on meanwhile, so nothing waits. The hold comes from an allowance of this many milliseconds, used up by the time
// held and filled again at an eighth of the clock's rate: a pipeline that takes longer, or a run of them on a first
// visit with nothing recorded, shows its incomplete frames as before and does not freeze the picture. Not with DLSS
// Frame Generation, which is told about every frame.
// The same hold covers a change of the scene's size (a DLSS or FSR 3.1 mode picked in the menu, vk_dlaa.c
// vk_dlaa_resized_take): the first frames drawn into the game's new, empty scene targets come out dark or black, so the
// previous picture stays for RESIZE_HOLD presents from the first frame of the new size. That is once per change of a
// setting and does not use the allowance. It holds with DLSS Frame Generation too: a present left out is a longer gap
// between two rendered frames to it (its constants and tags are set at the host present, gfx_present.c), and a dark
// frame it is given comes out as several generated ones.
// And it covers an area load (vk_exec_draw.c vk_exec_pool_loading): while the texture pool still makes the area's
// textures, the frames show the ground and the walls as black stand-ins, so the picture before the load stays for up
// to LOAD_HOLD_NS, also beside the allowance.
#define RESIZE_HOLD 6
#define LOAD_HOLD_NS 2000000000ull
// frame_hold, at the present, is the only writer of FH. The replay asks what is left of the allowance as it records
// (vk_frame_hold_left, vk_exec_draw.c), on whatever thread holds the submission lock, which need not be the one that
// presents: so that reader works out the allowance from limit_ns, left_ns, last_ns and holding (atomics) and writes
// nothing. When it refilled FH itself, a refill on the replaying thread beside one at the present on another thread
// counted the same interval twice or not at all. A refill writes left_ns and last_ns as a pair under a sequence word
// (seq, odd while it writes), so the reader does not pair a new left_ns with an old last_ns either.
static struct {
    int64_t limit_ns, left_ns;
    uint64_t last_ns, held, runs, shown, load_t0, load_held;
    int holding, resize_left;
    uint32_t seq;
} FH = {.limit_ns = -1};
#define FH_READ_TRIES 3

// NX2NAVI_FRAME_HOLD_MS, read once by whichever thread asks first; that one logs it.
static int64_t fh_limit(void) {
    int64_t lim = __atomic_load_n(&FH.limit_ns, __ATOMIC_ACQUIRE);
    if (lim >= 0) return lim;
    long long ms = cfg_int("NX2NAVI_FRAME_HOLD_MS");
    int64_t want = (int64_t)(ms < 0 ? 0 : ms > 500 ? 500 : ms) * 1000000, was = -1;
    if (__atomic_compare_exchange_n(&FH.limit_ns, &was, want, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && want)
        rt_log("vk: a frame with draws skipped for a pipeline is not shown, the previous picture stays up to %lld ms "
               "(NX2NAVI_FRAME_HOLD_MS)", (long long)(want / 1000000));
    return want;
}

// The allowance at now from the last refill (left at last): the time since used up while frames were held, filled
// again at an eighth of the clock's rate while they were not, never above the limit. Full before the first refill.
static int64_t fh_left_at(int64_t lim, int64_t left, uint64_t last, int holding, uint64_t now) {
    if (!last) return lim;
    left = holding ? left - (int64_t)(now - last) : left + (int64_t)(now - last) / 8;
    return left > lim ? lim : left;
}

static int64_t frame_hold_refill(uint64_t now) {   // frame_hold only; returns the limit
    int64_t lim = fh_limit(), left = fh_left_at(lim, FH.left_ns, FH.last_ns, FH.holding, now);
    uint32_t s = FH.seq;
    __atomic_store_n(&FH.seq, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);   // a reader that sees either new field also sees the odd sequence
    __atomic_store_n(&FH.left_ns, left, __ATOMIC_RELAXED);
    __atomic_store_n(&FH.last_ns, now, __ATOMIC_RELAXED);
    __atomic_store_n(&FH.seq, s + 2, __ATOMIC_RELEASE);
    return lim;
}

// Whether a frame that skipped draws now would be held (vk_exec_draw.c: a full-screen draw is skipped only then). Any
// thread; reads FH only. left_ns and last_ns come from one refill: a read that overlaps a refill is made again, and
// after FH_READ_TRIES such reads the last one is used (it can count one interval twice or not at all, for this one
// decision), so the reader never waits for the present. holding is stored at the present just after its refill; a
// read between the two applies the previous frame's holding to the few microseconds since that refill. The fields are
// loaded before the clock, so now is not before last.
int vk_frame_hold_left(void) {
    int64_t lim = fh_limit(), left = 0;
    uint64_t last = 0;
    for (int k = 0; k < FH_READ_TRIES; k++) {
        uint32_t s0 = __atomic_load_n(&FH.seq, __ATOMIC_ACQUIRE);
        left = __atomic_load_n(&FH.left_ns, __ATOMIC_RELAXED);
        last = __atomic_load_n(&FH.last_ns, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);   // the fields are read before the sequence is read again
        if (!(s0 & 1) && __atomic_load_n(&FH.seq, __ATOMIC_RELAXED) == s0) break;
    }
    int holding = __atomic_load_n(&FH.holding, __ATOMIC_RELAXED);
    return fh_left_at(lim, left, last, holding, kern_now_ns()) > 0 && !vk_dlssg_active();
}

// 1: this frame is not shown. skipped: the draws it lacks; resized: its scene is the first of a new size.
static int frame_hold(uint32_t skipped, int resized, uint64_t frame) {
    int64_t lim = frame_hold_refill(kern_now_ns());
    if (resized && lim > 0) {
        FH.resize_left = RESIZE_HOLD;
        rt_log("vk: frame %llu: the scene has a new size after a setting changed; the previous picture stays for %d "
               "frames", (unsigned long long)frame, RESIZE_HOLD);
    }
    if (FH.resize_left) {   // beside the allowance: nothing below counts these frames
        FH.resize_left--;
        return 1;
    }
    if (lim > 0 && vk_exec_pool_loading()) {
        uint64_t now = kern_now_ns();
        if (!FH.load_t0) FH.load_t0 = now, FH.load_held = 0;
        if (now - FH.load_t0 < LOAD_HOLD_NS) {
            FH.load_held++;
            return 1;
        }
    } else if (FH.load_t0) {
        rt_log("vk: frame %llu: the previous picture stayed for %llu frames, %.0f ms, while an area's textures were made",
               (unsigned long long)frame, (unsigned long long)FH.load_held, (double)(kern_now_ns() - FH.load_t0) / 1e6);
        FH.load_t0 = 0;
    }
    int hold = skipped && FH.left_ns > 0 && !vk_dlssg_active();
    if (hold && !FH.holding) FH.runs++;
    if (skipped && !hold) FH.shown++;
    __atomic_store_n(&FH.holding, hold, __ATOMIC_RELAXED);
    if (hold) FH.held++;
    static uint64_t logged_at, logged_held, logged_shown;
    if (!hold && FH.held + FH.shown != logged_held + logged_shown && frame - logged_at >= 600) {
        rt_log("vk: frame %llu: %llu frames with skipped draws held back so far in %llu runs, %llu shown incomplete "
               "(allowance used up)", (unsigned long long)frame, (unsigned long long)FH.held,
               (unsigned long long)FH.runs, (unsigned long long)FH.shown);
        logged_at = frame, logged_held = FH.held, logged_shown = FH.shown;
    }
    if (hold && (rt_trace & RT_TRACE_PRESENT))
        rt_log("present: frame %llu held back, %u draws skipped", (unsigned long long)frame, skipped);
    return hold;
}

// ---- a frame's bookkeeping in replay order (vk_frame_owed.h)
// A present closes the frame for the replay (frame_end): the trace and conditional-render frame counts
// (vk_exec_frame_log, whose vk_exec_cond_frame_end moves the frame count the conditional draws go by), dynamic
// resolution's step, the DLSS state of the frame (vk_dlaa_frame: the mode and its LOD bias, the frame's points and
// the swap image, the idle releases), the motion tables (vk_mvsurvey_frame, vk_mvprev_frame swaps its two) and the GPU
// pass timing. The replay reads and writes all of it as it records, under the submission lock. With the replay on the
// submitting threads the game presents on one thread while another may be replaying, and running it at the present
// then reset it in the middle of that replay: a DLSS frame reset halfway (a shake, a frame without the upscaler), the
// motion tables swapped halfway (a ghost; with NX2NAVI_MVPREV_CHECK=1 a write out of bounds), the conditional draws
// counted against the next frame. So it runs under the submission lock: at the present when the lock is free, else
// first thing in the next submission, on whichever thread makes it (vk_sync.c submit, vk_present_frames_owed), or on
// the present thread before the frame's blit (present_frame_closed), whichever takes the lock first. The blit reads what
// vk_dlaa_frame closed (the inputs frame generation must not get across an upscaler or frame generation switch), so a
// frame's bookkeeping always comes before its blit, as in 0.10.1. The presenting thread never waits for another
// thread's replay for it; the present thread may, for a frame still owed, as it waits for the lock for the blit's
// submission a moment later anyway. Nothing in frame_end may wait for the present thread, submit or flush with a wait:
// the hand-off, vk_texture_get with its upload flush and vk_present_drain stay in present_image, outside the lock.
// NX2NAVI_PRESENT_ORDER=0: at the present, whatever another thread replays, as in 0.10.1.
static void frame_end(uint64_t frame, const tex_t *t) {   // t: the presented texture, for the trace = gfx line
    vk_exec_frame_log(t, frame);
    dynres_frame();   // before vk_dlaa_frame reads the DLSS mode it may override
    vk_exec_barrier_log(frame);
    vk_dlaa_frame();
    vk_mvsurvey_frame();
    vk_mvprev_frame();
    vk_gpu_passes_frame(frame);
}

#define FO_TEX 4
static FrameOwed FO;
static const tex_t *fo_tex[FO_TEX];   // the newest frames' presented textures, by frame % FO_TEX (atomics)
static uint64_t fo_owed_n, fo_late_n;   // presents that left it to a submission; frames a submission caught up (atomics)
static uint64_t fo_blit_n, fo_blit_ns;  // frames the present thread caught up before their blits, and the time it took
                                        // with the wait for the lock (the present thread's; read by order_log: atomics)

static int present_order_on(void) {   // read once; present_image runs one at a time
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2NAVI_PRESENT_ORDER");
        if (!on) rt_log("vk: NX2NAVI_PRESENT_ORDER=0: a frame's bookkeeping runs at the present, whatever another thread "
                        "replays");
    }
    return on;
}

static void fo_end(uint64_t frame, void *arg) {
    (void)arg;
    frame_end(frame, __atomic_load_n(&fo_tex[frame % FO_TEX], __ATOMIC_RELAXED));
}
static void fo_here(void) { fo_run(&FO, fo_end, NULL); }
static int fo_try(void *arg) {
    (void)arg;
    return vk_sync_try_locked(fo_here);
}

// vk_sync.c submit, sub_lock held, before its replay: the frames presented while another thread held the lock. Their
// time counts as this thread's replay time (perf_replay), as submit's own timing starts after this call: a DLSS release
// in vk_dlaa_frame (tens of milliseconds) then shows as replay in the frame-time and slow-frame lines, not unattributed.
void vk_present_frames_owed(void) {
    if (!fo_waiting(&FO)) return;
    uint64_t t0 = perf_begin();
    __atomic_add_fetch(&fo_late_n, fo_run(&FO, fo_end, NULL), __ATOMIC_RELAXED);
    perf_replay(t0);
}

// The present thread, before it uses frame (present_main): frame_end of a frame the presenting thread left owed has run
// by then. When no submission came between, the present thread runs it itself, under sub_lock, waiting for a replay in
// progress. Not one of the game's threads, it waits for the same lock for the blit's submission a moment later anyway;
// a frame whose bookkeeping ran (the presenting thread got the lock, or a submission came first) takes no lock here.
static void fo_blit_here(void) {
    __atomic_add_fetch(&fo_blit_n, fo_run(&FO, fo_end, NULL), __ATOMIC_RELAXED);
}
static void fo_blit_locked(void *arg) {
    (void)arg;
    vk_sync_locked(fo_blit_here);
}
static void present_frame_closed(uint64_t frame) {
    uint64_t t0 = kern_now_ns();
    if (fo_close(&FO, frame, fo_blit_locked, NULL))
        __atomic_add_fetch(&fo_blit_ns, kern_now_ns() - t0, __ATOMIC_RELAXED);
}

static void order_log(uint64_t frame, int total);   // after the held present's state (PH)

// The present after the frame count: resolves the image and hands the blit to the present thread. queued: when the game
// presented; value: the timeline value queued for this present (asynchronous replay), 0 for the next one (synchronous).
static void present_image(const tex_t *t, const int crop[4], uint64_t at_ns, uint64_t queued, uint64_t frame,
                          uint64_t value) {
    static volatile int32_t n_skipped;
    static int vram_probe;
    if (!vram_probe) vram_probe = 1, perfmon_set_vram_game(vk_mem_device_used_mb);   // the overlay's "game" beside VRAM
    int owed = 0;   // the frame's bookkeeping may still be owed when the present thread takes it (PresentJob.owed)
    if (present_order_on()) {
        __atomic_store_n(&fo_tex[frame % FO_TEX], t, __ATOMIC_RELAXED);
        if (!fo_present(&FO, frame, fo_try, NULL)) owed = 1, __atomic_add_fetch(&fo_owed_n, 1, __ATOMIC_RELAXED);
    } else {
        frame_end(frame, t);
    }
    if ((rt_trace & RT_TRACE_PERF) && frame % 600 == 0 && !gfx_replay_async()) order_log(frame, 0);
    vk_sampler_aniso_live();   // NX2NAVI_ANISO changed in the menu: the surface textures' samplers take the new level
    vk_mem_census(frame);
    if (vk_dump_armed) vk_dump_present(frame);
    __atomic_store_n(&gfx_counters.validation_errors, (uint64_t)vk_validation_errors_total(), __ATOMIC_RELAXED);
    const VkTexImage *img = vk_texture_get(t);
    int held = frame_hold(vk_exec_pending_draws_take(), vk_dlaa_resized_take(), frame);
    if (held || !img || !(img->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) {
        if (!held && plat_inc32(&n_skipped) <= 8)
            rt_log("vk: present of texture %p (format %#x) skipped: no colour image", (const void *)t, t ? t->d.format : 0);
        if (value) {   // the queued value is still signalled, in order
            vk_present_drain();
            uint64_t f = vk_sync_submit(NULL, 0);
            if (f != value) rt_fatal("vk: skipped present signalled %llu, queued as %llu", (unsigned long long)f, (unsigned long long)value);
        }
        return;
    }
    PresentJob j = {.image = img->image, .format = img->format, .w = (int)img->width, .h = (int)img->height, .frame = frame,
                    .at = at_ns, .queued = queued, .value = value, .owed = owed};
    vk_hdr_present_source(img, &j.image, &j.format);   // NX2NAVI_HDR: the float picture the game copied into it, same size
    if (crop) {
        j.has_crop = 1;
        for (int i = 0; i < 4; i++) j.crop[i] = crop[i];
    }
    vk_upload_flush(0);   // an upload vk_texture_get just recorded goes before the blit
    plat_mutex_lock(&pl);
    if (!present_thread) {
        if (!plat_thread_spawn(present_main, NULL, "vk present", 0)) rt_fatal("vk: cannot start the present thread");
        present_thread = 1;
    }
    // One present in waiting at most: the wait for the present thread to take the previous one is the hand-off
    // (PERF_HANDOFF), timed from under pl, where pending is read.
    uint64_t h0 = pending ? perf_begin() : 0;
    while (pending) plat_cond_wait(&pcv, &pl);
    j.handoff_ns = h0 ? kern_now_ns() - h0 : 0;
    job = j;
    pending = 1;
    plat_cond_broadcast(&pcv);
    plat_mutex_unlock(&pl);
    perf_end(PERF_HANDOFF, h0);
    if (rdoc_active()) { vk_present_drain(); rdoc_after_present(frame); }
}

// ---- Registrations in submit order
// The console's GPU reads the texture and sampler pools as it runs a submission, right after the submit, so a draw sees
// the slots as they were at about its submit (docs/maxwell.md 4.9). The replay takes longer than that, and writes the
// submission's bindless set as it goes; the set is read when the submission runs. A registration applied at once while
// a submission is being recorded (made by another of the game's threads, or by the game ahead of a replay on its own
// thread) therefore reaches that submission's draws, the ones recorded before it too: they sample the slot's next
// texture, which the game registered for a later frame. Ocarina registers a round of 30 to 60 streamed textures every
// four or five frames in a town. So a registration takes effect between submissions, in the order the game made it:
//  - replay on the submitting thread: one made while a submission is being recorded (reg_replaying, set by vk_sync.c
//    submit around the replay) waits in regq, and so does any made behind it; the submitting thread applies them once
//    the submission has gone to the queue (vk_reg_replay_end). With no submission being recorded it is applied at once.
//  - replay on its own thread: every one waits in regq and goes into the replay queue ahead of the next item queued
//    (regs_queue_locked), so the replay applies it where the game made it among its submits, as it does a release.
// A texture finalized while its own registration waits is released at once as before, and once more behind that
// registration (RG_RELEASE): vk_texture_release marks the slots the texture object registered, which the first call
// could not see. NX2NAVI_REG_ORDER=0: every registration at once, as before.
enum { RG_TEX, RG_SAMP, RG_RELEASE };
typedef struct {
    int kind, id, has_view;
    tex_t t;      // RG_TEX, RG_RELEASE: a copy (the game may reuse the object's memory)
    view_t v;
    samp_par s;   // RG_SAMP
} RegItem;
static PlatMutex regl = PLAT_MUTEX_INIT;   // regq and the order of what is applied at once; taken before the texture lock
static RegItem *regq;
static int regn, regcap;
static int reg_replaying;   // atomic
static uint64_t reg_waited, reg_rounds;   // under regl: registrations that waited for a submission, and the times some did

static int reg_order_on(void) {
    static int on = -1;
    if (on < 0) {
        on = rt_knob("REG_ORDER", "1") != 0;
        rt_log("vk: texture and sampler registrations take effect %s (NX2NAVI_REG_ORDER=%d)",
               on ? "between submissions, in the order the game made them" : "at once", on);
    }
    return on;
}

static void reg_apply(const RegItem *q, int n) {
    for (int i = 0; i < n; i++) {
        if (q[i].kind == RG_TEX) vk_texture_register(q[i].id, &q[i].t, q[i].has_view ? &q[i].v : NULL);
        else if (q[i].kind == RG_SAMP) vk_sampler_register(q[i].id, &q[i].s);
        else vk_texture_release(&q[i].t);
    }
}

static void reg_push(const RegItem *r) {   // regl held
    if (regn == regcap) {
        int n = regcap ? regcap * 2 : 256;
        RegItem *p = realloc(regq, sizeof *p * (size_t)n);
        if (!p) rt_fatal("vk: out of memory (registrations)");
        regq = p, regcap = n;
    }
    regq[regn++] = *r;
}

// A registration from the game: applied now, or kept for its place among the submissions.
static void reg_add(const RegItem *r) {
    plat_mutex_lock(&regl);
    if (gfx_replay_async() || regn || __atomic_load_n(&reg_replaying, __ATOMIC_ACQUIRE)) reg_push(r);
    else reg_apply(r, 1);   // under regl: in call order with the other threads' registrations
    plat_mutex_unlock(&regl);
}

// vk_sync.c submit, around the replay of a submission of the game's commands, under its submit lock.
void vk_reg_replay_begin(void) {
    if (gfx_replay_async() || !reg_order_on()) return;
    __atomic_store_n(&reg_replaying, 1, __ATOMIC_RELEASE);
}

// The submission has gone to the queue with its set: what waited is applied, before the next submission is recorded.
void vk_reg_replay_end(uint64_t fence) {
    if (gfx_replay_async() || !reg_order_on()) return;
    plat_mutex_lock(&regl);
    __atomic_store_n(&reg_replaying, 0, __ATOMIC_RELEASE);
    int n = regn;
    if (n) {
        reg_apply(regq, n);   // under regl: a registration made meanwhile comes after these
        regn = 0;
        reg_waited += (uint64_t)n, reg_rounds++;
        if (reg_rounds <= 8 || reg_rounds % 1024 == 0)
            rt_log("vk: %d registrations made while submission %llu was being replayed took effect after it (frame %llu; "
                   "%llu so far, %llu times)", n, (unsigned long long)fence,
                   (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED),
                   (unsigned long long)reg_waited, (unsigned long long)reg_rounds);
    }
    plat_mutex_unlock(&regl);
}

// ---- Asynchronous replay
enum { RI_SUBMIT, RI_PRESENT, RI_RELEASE, RI_REGS };
typedef struct {
    int kind, n, has_crop, crop[4];
    uint64_t value, submit_no, at, queued;
    uint64_t *h;
    const tex_t *tex;   // RI_RELEASE: a copy the item owns (the game may reuse the object's memory)
    RegItem *regs;      // RI_REGS: n registrations the item owns, in the order the game made them
} ReplayItem;
#define RQ_CAP 1024   // a frame queues 1 submit and 1 present, and up to a few hundred releases
enum { DR_POOL_FINI, DR_MAP_VIRTUAL, DR_TEX_RELEASE, DR_PROG_RELEASE, DR_FINISH, DR_READ_TEXELS, DR_WRITE_TEXELS,
       DR_GUEST_READ, DR_FRAME, DR_FULL, DR_CHECK, DR_N };
static const char *const dr_name[DR_N] = {
    "pool_fini", "map_virtual", "texture_release (queued)", "program_release", "finish", "read_texels (writer fence)",
    "write_texels", "guest_read (writer fence)", "frame bound", "queue full", "check drain"};

static PlatMutex rl = PLAT_MUTEX_INIT;
static PlatCond rcv = PLAT_COND_INIT;   // item queued, item taken, item done
static ReplayItem rq[RQ_CAP];
static int rq_head, rq_n, r_busy, r_started;
static uint64_t r_presents_queued, r_presents_done, r_submit_done, game_frames;
// Under rl: calls, calls that waited and the time waited per cause, since the last replay_log line and over the run.
static uint64_t dr_calls[DR_N], dr_waits[DR_N], dr_ns[DR_N], dr_tcalls[DR_N], dr_twaits[DR_N], dr_tns[DR_N];
// NX2NAVI_REPLAY_CHECK: ReadTexels compared with a read after a full drain (under rl): checked, differing, differing
// although no submission recorded a write to the texture in between, differing with the queue idle at the first read.
static uint64_t ck_reads, ck_diff, ck_diff_unexplained, ck_diff_idle;

int vk_replay_async(void) {
    static int on = -1;
    if (on < 0) {
        // Off by default. Where the game waits for the GPU it overlaps the replay with the game threads (a GTX 1650 in
        // one scene went from 450-686 frames over 50 ms a minute to 10-13, issue #88), the only place a gain was
        // measured. While it was the default, a GTX 1660 Ti at the 60 fps cap with the GPU at 26 % reported uneven
        // motion that 0.3.4 did not have (#659). It adds up to a frame of latency.
        on = cfg_bool("NX2NAVI_REPLAY_ASYNC");
        rt_log("vk: replay %s (NX2NAVI_REPLAY_ASYNC=%d)",
               on ? "on its own thread, at most one frame behind the game" : "on the submitting thread", on);
    }
    return on;
}

// With the replay on its own thread a submission's value covers its replay too, and the wait stays the fence's.
static int vk_be_gpu_wait(uint64_t value, uint64_t timeout_ns) {
    return vk_replay_async() ? vk_sync_fence_wait(value, timeout_ns) : vk_sync_exec_wait(value, timeout_ns);
}
// The submit wait (nvn_api_core.c NX2NAVI_SUBMIT_FINISH): the GPU's only where the submission reads guest memory on the
// GPU (vk_sync_recorded_wait).
static int vk_be_exec_wait(uint64_t value, uint64_t timeout_ns) {
    return vk_sync_recorded_wait(value, timeout_ns, vk_be_gpu_wait);
}

// The front end's view (nvn_api_core.c, nvn_api_cmd.c): replay is asynchronous only on this back end.
int gfx_replay_async(void) { return gfx_backend() == &gfx_vulkan_backend && vk_replay_async(); }

// The front end's nvnEventSignal, before it writes (vk_exec_xfer.c "events the CPU signals").
void gfx_event_signalling(const void *value, uint32_t to) {
    vk_exec_event_cpu(value);
    vk_exec_evt('c', value, to);
}

// The newest submit number (nvn_submit_count) whose replay is done: recording copies it used may be freed.
uint64_t gfx_replayed_submits(void) {
    if (!gfx_replay_async()) return nvn_submit_count();
    plat_mutex_lock_shared(&rl);
    uint64_t v = r_submit_done;
    plat_mutex_unlock_shared(&rl);
    return v;
}

// ---- a present that waits for held work (event order, vk_exec.c)
// On the console a present is queued behind its queue's work and goes out once the GPU has run the frame, including
// work that waited for an event. The replay holds such work (vk_exec.c, "event order across the game's queues") until
// the wait holds, and the present went out at once: in Kokiri Forest dialogue (Saria), several presents a second came
// with the frame's whole submission held (4 held submissions, nothing drawn since the last present) and showed the
// picture from before, a flash of the whole picture; the frame was then drawn after its present, in the present
// thread's empty submission, next to the following frame. So with work held at a present, what can go on now goes
// first, in a submission of its own (held_resume); if work is still held, the present waits and goes out right after
// the submission that replays the last of it, at the next present, or after PH_MAX submissions at the latest. Only with
// the replay on the submitting thread, as the event order. NX2NAVI_PRESENT_HELD=0: every present goes out at once.
// The game presents from one thread while others submit, so the submission that puts a held present out may be another
// thread's, while the presenting thread goes on to its next present. vk_present_held.h keeps that whole: one lock
// (PH.lock) around arming, putting out and every synchronous present while held presents are on, so a held frame goes
// out once, in one piece and before the next one. A submitting thread only tries it and never waits; the presenting
// thread waits for it only while another thread is putting a held present out (counted, and timed as PERF_DRAIN). No
// path holds it across held_resume, which is a replay; present_image's callees take sub_lock only by trying it.
#define PH_MAX 8
static void ph_lock_waited(uint64_t t0) { perf_end(PERF_DRAIN, t0); }   // perf_begin's clock is plat_now_ns's
static PhState PH = {.lock = PLAT_MUTEX_INIT, .on_wait = ph_lock_waited};
static uint64_t ph_ran;      // held work run at a present (held_resume), on any thread: atomic
static uint64_t ph_logged;   // PH.lock: "went out" lines so far
static uint64_t ph_other;    // held presents another thread's submission put out (written under PH.lock, atomic)

static int present_held_on(void) {
    static int on = -1;
    if (on < 0) on = !gfx_replay_async() && rt_knob("PRESENT_HELD", "1");
    return on;
}

static uint64_t submit_sync(const uint64_t *handles, int n);

// The held work that can go on now, in a submission of its own (vk_exec_record syncs it like any other: held_sync).
// The present thread first finishes what it was handed: its empty submission replays held work and frees what it
// replayed (vk_exec_record), which this walk reads. A present ph_out handed it a moment ago is such a case.
static int held_resume(void) {
    vk_present_drain();
    if (!vk_exec_held() || !vk_exec_held_ready()) return 0;
    submit_sync(NULL, 0);
    __atomic_add_fetch(&ph_ran, 1, __ATOMIC_RELAXED);
    return 1;
}

// vk_present_held.h's present callback, with PH.lock held: a held present (why PH_AFTER, PH_CAPPED, PH_NEXT) or one
// that did not wait (PH_NONE) goes to present_image.
static void ph_out(const PhJob *j, int why, void *arg) {
    (void)arg;
    if ((why == PH_AFTER || why == PH_CAPPED) && plat_thread_id() != j->tid)
        __atomic_add_fetch(&ph_other, 1, __ATOMIC_RELAXED);
    if (why != PH_NONE && ph_logged < 16)
        ph_logged++, rt_log("vk: the present of frame %llu went out %llu submission(s) after the game's present call, %d "
                            "held submissions left (%s)", (unsigned long long)j->frame,
                            (unsigned long long)(vk_exec_submit_seq_now() - j->since), vk_exec_held(),
                            why == PH_AFTER ? "the held work was replayed" : why == PH_CAPPED ? "PH_MAX reached"
                                                                                         : "the next present came");
    present_image(j->t, j->has_crop ? j->crop : NULL, j->at_ns, j->queued, j->frame, 0);
    if (why != PH_NONE && PH.waited % 600 == 0)
        rt_log("vk: presents that waited for held work: %llu (held work run at the present %llu times; out after it %llu, "
               "at %d submissions %llu, at the next present %llu)", (unsigned long long)PH.waited,
               (unsigned long long)__atomic_load_n(&ph_ran, __ATOMIC_RELAXED), (unsigned long long)PH.out[PH_AFTER],
               PH_MAX, (unsigned long long)PH.out[PH_CAPPED], (unsigned long long)PH.out[PH_NEXT]);
}

// Asked with PH.lock held, after a submission that ran held work first (before) or after this thread's submission:
// whether the held present goes out now.
static int ph_why_before(const PhJob *j, void *arg) {
    (void)j, (void)arg;
    return vk_exec_held() ? PH_NONE : PH_AFTER;
}
static int ph_why_after(const PhJob *j, void *arg) {
    (void)arg;
    return !vk_exec_held() ? PH_AFTER : vk_exec_submit_seq_now() - j->since >= PH_MAX ? PH_CAPPED : PH_NONE;
}
static int ph_hold(void *arg) {   // the presenting thread, PH.lock held: work is still held, so its present waits
    (void)arg;
    return vk_exec_held() != 0;
}

static uint64_t submit_sync(const uint64_t *handles, int n) {
    vk_sync_start();
    if (n && ph_armed(&PH)) {   // a present waits: what can go on goes first, then the present, then this submission
        held_resume();
        ph_flush_try(&PH, ph_why_before, ph_out, NULL);
    }
    vk_present_drain();
    vk_exec_gate_wait(handles, n);   // a submission that begins with a wait for the CPU's signal: before its copies
    vk_texture_new_submit();
    uint64_t f = vk_sync_submit(handles, n);
    // The submission stopped at a wait whose signal the CPU gave while it was being replayed (vk_exec_xfer.c "events
    // the CPU signals"): it goes on now, in a submission of its own with copies taken after the signal, and that
    // submission's value is the one the game's fence stands for.
    if (n && vk_exec_event_at_sync() && vk_exec_held() && vk_exec_held_ready()) f = submit_sync(NULL, 0);
    if (n) ph_flush_try(&PH, ph_why_after, ph_out, NULL);   // at once unless a present waits
    return f;
}

// vk_sub_in_frame is the replay's: vk_sync.c submit sets it under the submission lock from the place the submitting
// thread noted for its submission (vk_sub_in_frame_publish), so another thread about to submit cannot change it under
// a replay in progress (vk_memory.c sub_place reads it there and on the replay workers). Submissions without commands
// of their own (held work, the present thread's blit) leave it as it was.
int vk_sub_in_frame;
static _Thread_local int sub_place_next;   // this thread's next submission's place in its frame

void vk_sub_in_frame_publish(void) { vk_sub_in_frame = sub_place_next; }

static void replay_main(void *arg) {
    (void)arg;
    int sub_idx = 0;   // submissions since the last present, in queue order
    perf_replay_thread();
    plat_mutex_lock(&rl);
    for (;;) {
        while (!rq_n) plat_cond_wait(&rcv, &rl);
        ReplayItem it = rq[rq_head];
        rq_head = (rq_head + 1) % RQ_CAP;
        rq_n--;
        r_busy = 1;
        plat_cond_broadcast(&rcv);
        plat_mutex_unlock(&rl);
        if (it.kind == RI_SUBMIT) {
            sub_place_next = sub_idx++;
            uint64_t f = submit_sync(it.h, it.n);
            if (f != it.value)
                rt_fatal("vk: replayed submission signalled %llu, queued as %llu", (unsigned long long)f,
                         (unsigned long long)it.value);
            free(it.h);
        } else if (it.kind == RI_RELEASE) {
            vk_present_drain();
            vk_texture_release(it.tex);
            free((void *)it.tex);
        } else if (it.kind == RI_REGS) {
            reg_apply(it.regs, it.n);
            free(it.regs);
        } else {
            uint64_t frame = __atomic_add_fetch(&gfx_counters.frames, 1, __ATOMIC_RELAXED);
            vk_dlssg_submitted(frame);   // the frame's submissions were replayed before its present
            vk_lowlat_submitted(frame);
            present_image(it.tex, it.has_crop ? it.crop : NULL, it.at, it.queued, frame, it.value);
            sub_idx = 0;
        }
        plat_mutex_lock(&rl);
        if (it.kind == RI_SUBMIT) r_submit_done = it.submit_no;
        else if (it.kind != RI_REGS) r_presents_done++;
        r_busy = 0;
        plat_cond_broadcast(&rcv);
    }
    return;
}

static void wait_counted(int why, uint64_t t0) {   // rl held
    uint64_t d = kern_now_ns() - t0;
    dr_waits[why]++, dr_twaits[why]++;
    dr_ns[why] += d, dr_tns[why] += d;
    perf_end(PERF_DRAIN, t0);
}

// A guest thread waiting for the replay gives its virtual core up, as around any blocking host wait (NVN_BLOCKING):
// guest_read (memcpy, pool invalidate), pool and texture finalize run without it, and a drain lasts up to a replay.
// Called and returning with rl held; rl is not held while the core is taken back.
static int core_away_unlocked(void) {
    plat_mutex_unlock(&rl);
    int away = kern_core_away();
    plat_mutex_lock(&rl);
    return away;
}
static void core_back_unlocked(int *away) {
    if (!*away) return;
    plat_mutex_unlock(&rl);
    kern_core_back(away);
    plat_mutex_lock(&rl);
}

// rl held: the replay thread runs, and the queue has room for `room` items (waits while it has not).
static void rq_room(int room) {
    if (!r_started) {
        if (!plat_thread_spawn(replay_main, NULL, "vk replay", 0)) rt_fatal("vk: cannot start the replay thread");
        r_started = 1;
    }
    // rl is dropped while the core is taken back, and another thread may fill the queue again meanwhile: check again.
    while (rq_n > RQ_CAP - room) {
        uint64_t p0 = perf_begin();
        dr_calls[DR_FULL]++, dr_tcalls[DR_FULL]++;
        int away = core_away_unlocked();
        while (rq_n > RQ_CAP - room) plat_cond_wait(&rcv, &rl);
        wait_counted(DR_FULL, p0);
        core_back_unlocked(&away);
    }
}

// rl held, with room for an item (rq_room): the registrations made since the last queued item go into the queue, to
// be applied before whatever is queued next ("registrations in submit order"). Taken and queued without letting go of
// rl, so two threads' lists keep their order.
static void regs_queue_locked(void) {
    plat_mutex_lock(&regl);
    ReplayItem ri = {.kind = RI_REGS, .n = regn, .regs = regq};
    regq = NULL, regn = regcap = 0;
    plat_mutex_unlock(&regl);
    if (!ri.n) return;
    rq[(rq_head + rq_n) % RQ_CAP] = ri;
    rq_n++;
    plat_cond_broadcast(&rcv);
}

// Queues an item (waits while the queue is full) and returns its value, reserved under rl so values follow queue order.
static uint64_t replay_enqueue(ReplayItem *it) {
    plat_mutex_lock(&rl);
    rq_room(2);   // the item, and the registrations made before it
    regs_queue_locked();
    it->value = it->kind == RI_RELEASE ? 0 : vk_sync_reserve();   // a release signals nothing
    rq[(rq_head + rq_n) % RQ_CAP] = *it;
    rq_n++;
    plat_cond_broadcast(&rcv);
    plat_mutex_unlock(&rl);
    return it->value;
}

// Returns once every queued item was replayed and submitted. Back-end entries only: the replay thread never calls them.
static void replay_drain(int why) {
    if (!gfx_replay_async()) return;
    plat_mutex_lock(&rl);
    dr_calls[why]++, dr_tcalls[why]++;
    if (__atomic_load_n(&regn, __ATOMIC_RELAXED)) {   // this thread's own registrations are in: replayed too
        rq_room(1);
        regs_queue_locked();
    }
    if (rq_n || r_busy) {
        uint64_t p0 = perf_begin();
        int away = core_away_unlocked();
        while (rq_n || r_busy) plat_cond_wait(&rcv, &rl);
        wait_counted(why, p0);
        core_back_unlocked(&away);
    }
    plat_mutex_unlock(&rl);
}

// What made the game wait for the replay: since the last line (every 1800 game frames), or over the run (at exit).
static void replay_log(uint64_t frame, int total) {
    char buf[1024];
    int k = 0;
    plat_mutex_lock(&rl);
    const uint64_t *c = total ? dr_tcalls : dr_calls, *w = total ? dr_twaits : dr_waits, *t = total ? dr_tns : dr_ns;
    for (int i = 0; i < DR_N && k < (int)sizeof buf - 96; i++)
        if (c[i] || w[i])
            k += snprintf(buf + k, sizeof buf - (size_t)k, "%s %s %llu calls, %llu waited %.1f ms", k ? ";" : "", dr_name[i],
                          (unsigned long long)c[i], (unsigned long long)w[i], t[i] / 1e6);
    if (ck_reads && k < (int)sizeof buf - 128)
        k += snprintf(buf + k, sizeof buf - (size_t)k, "; check (run): %llu reads, %llu differ, %llu without a recorded write, %llu with the queue idle",
                      (unsigned long long)ck_reads, (unsigned long long)ck_diff, (unsigned long long)ck_diff_unexplained,
                      (unsigned long long)ck_diff_idle);
    memset(dr_calls, 0, sizeof dr_calls);
    memset(dr_waits, 0, sizeof dr_waits);
    memset(dr_ns, 0, sizeof dr_ns);
    plat_mutex_unlock(&rl);
    rt_log("vk: replay queue %s frame %llu:%s", total ? "over the run to" : "at", (unsigned long long)frame,
           k ? buf : " no waits");
}

// What the game's threads met of each other at the present (synchronous replay): presents whose frame bookkeeping
// went to the next submission because another thread held the submission lock, frames a submission caught up, frames
// the present thread caught up before their blits (present_frame_closed) and how long that took it, the wait for the
// lock included, held presents another thread's submission put out (ph_out), and the presenting thread's waits for
// PH.lock while one went out (vk_present_held.h ph_lock). total: the run, at exit (gfx_exit_log); else
// NX2NAVI_TRACE=perf every 600 presents (present_image), the window since the last line, logged only when a count moved.
static void order_log(uint64_t frame, int total) {
    static uint64_t last[7], last_frame;   // present_image's (one at a time): the counts at the last line
    uint64_t v[7] = {__atomic_load_n(&fo_owed_n, __ATOMIC_RELAXED), __atomic_load_n(&fo_late_n, __ATOMIC_RELAXED),
                     __atomic_load_n(&ph_other, __ATOMIC_RELAXED), __atomic_load_n(&PH.lock_waits, __ATOMIC_RELAXED),
                     __atomic_load_n(&PH.lock_wait_ns, __ATOMIC_RELAXED), __atomic_load_n(&fo_blit_n, __ATOMIC_RELAXED),
                     __atomic_load_n(&fo_blit_ns, __ATOMIC_RELAXED)};
    uint64_t d[7], presents = total ? frame : frame - last_frame;
    for (int i = 0; i < 7; i++) d[i] = total ? v[i] : v[i] - last[i];
    if (!total) {
        if (!d[0] && !d[1] && !d[2] && !d[3] && !d[5]) return;
        memcpy(last, v, sizeof last), last_frame = frame;
    }
    rt_log("%s frame %llu: frame bookkeeping: %llu of %llu presents left it to the next submission (another thread held "
           "the submission lock), %llu frames caught up by a submission, %llu by the present thread before their blits "
           "(%.1f ms in all with its wait for the lock); held presents put out by another thread's submission %llu, the "
           "presenting thread waited for one %llu times, %.1f ms in all, longest %.1f ms so far",
           total ? "vk: over the run to" : "perf: since the last such line, at", (unsigned long long)frame,
           (unsigned long long)d[0], (unsigned long long)presents, (unsigned long long)d[1], (unsigned long long)d[5],
           d[6] / 1e6, (unsigned long long)d[2], (unsigned long long)d[3], d[4] / 1e6,
           __atomic_load_n(&PH.lock_wait_max, __ATOMIC_RELAXED) / 1e6);
}

// The run total, once: the game exits through prof_write (boot.c _exit, the window close), probes through
// vk_backend_shutdown.
void gfx_exit_log(void) {
    static int32_t done, order_done;
    rec_check_log();
    if (gfx_replay_async() && r_started && !plat_xchg32(&done, 1)) replay_log(game_frames, 1);
    if (!gfx_replay_async() && !plat_xchg32(&order_done, 1))
        order_log(__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), 1);
}

static void vk_be_present(const tex_t *t, const int crop[4], uint64_t at_ns) {
    motion_owner_present();
    vk_sync_start();
    gfx_tl_present();
    vk_dlssg_game_present();
    vk_lowlat_game_present();
    if (!gfx_replay_async()) {
        uint64_t frame = __atomic_add_fetch(&gfx_counters.frames, 1, __ATOMIC_RELAXED);
        vk_dlssg_submitted(frame);
        vk_lowlat_submitted(frame);
        vk_exec_evt_frame(frame);
        perf_present(frame);   // before vk_exec_frame_log, which resets the frame's exec counts the flight recorder reads
        uint64_t queued = kern_now_ns();
        if (present_held_on()) {   // present_held
            PhJob j = {.t = t, .has_crop = crop != NULL, .at_ns = at_ns, .queued = queued, .frame = frame,
                       .tid = plat_thread_id()};
            if (crop) memcpy(j.crop, crop, sizeof j.crop);
            vk_present_drain();    // the present thread's empty submission may run held work: done before it is read
            if (ph_armed(&PH)) held_resume(), ph_flush_next(&PH, ph_out, NULL);   // the last present still waits: out now
            if (vk_exec_held()) held_resume();
            j.since = vk_exec_submit_seq_now();
            ph_present(&PH, &j, ph_hold, ph_out, NULL);   // waits for the held work, or goes out now
            return;
        }
        present_image(t, crop, at_ns, queued, frame, 0);   // only this thread presents: no lock
        return;
    }
    // Frame bound: the replay thread handled the previous present before this one is queued.
    plat_mutex_lock(&rl);
    dr_calls[DR_FRAME]++, dr_tcalls[DR_FRAME]++;
    if (r_presents_done < r_presents_queued) {
        uint64_t p0 = perf_begin();
        while (r_presents_done < r_presents_queued) plat_cond_wait(&rcv, &rl);
        wait_counted(DR_FRAME, p0);
    }
    r_presents_queued++;
    plat_mutex_unlock(&rl);
    uint64_t frame = ++game_frames;
    perf_present(frame);
    if (frame % 1800 == 0) replay_log(frame, 0);
    ReplayItem it = {.kind = RI_PRESENT, .tex = t, .at = at_ns, .queued = kern_now_ns()};
    if (crop) {
        it.has_crop = 1;
        for (int i = 0; i < 4; i++) it.crop[i] = crop[i];
    }
    replay_enqueue(&it);
}

// vk_pipeline.c drops a released program's pipelines here.
WEAK void vk_program_release(const prog_t *p) { (void)p; }

static int vk_be_pool_init(pool_t *p) { vk_sync_start(); return vk_pool_init(p); }
static void vk_be_pool_fini(pool_t *p) { vk_sync_start(); replay_drain(DR_POOL_FINI); vk_present_drain(); vk_pool_fini(p); }
static int vk_be_pool_map_virtual(pool_t *v, int n, const void *requests) {
    vk_sync_start();
    replay_drain(DR_MAP_VIRTUAL);
    vk_present_drain();
    return vk_pool_map_virtual(v, n, requests);
}
// Both take effect between submissions ("registrations in submit order").
static void vk_be_texture_register(int id, const tex_t *t, const view_t *v) {
    vk_sync_start();
    if (!t || !reg_order_on()) { vk_texture_register(id, t, v); return; }
    RegItem r = {.kind = RG_TEX, .id = id, .has_view = v != NULL, .t = *t};
    if (v) r.v = *v;
    reg_add(&r);
}
static void vk_be_sampler_register(int id, const samp_par *s) {
    vk_sync_start();
    if (!s || !reg_order_on()) { vk_sampler_register(id, s); return; }
    RegItem r = {.kind = RG_SAMP, .id = id, .s = *s};
    reg_add(&r);
}
// Asynchronous: the release is queued behind the submissions before it, so their replay still resolves the texture's
// slots to its image (a released slot follows another live texture at the storage), and the ones after it see it
// released, the order of the synchronous replay. vk_texture_release reads only the object's key fields: a copy.
static void vk_be_texture_release(const tex_t *t) {
    vk_sync_start();
    if (!gfx_replay_async()) {
        vk_present_drain();
        vk_texture_release(t);
        // Its own registration still waits for the submission being replayed: released again behind it, which marks
        // the slot that registration fills ("registrations in submit order").
        if (t && t->mem && __atomic_load_n(&regn, __ATOMIC_RELAXED)) {
            plat_mutex_lock(&regl);
            int mine = 0;
            for (int i = 0; i < regn && !mine; i++)
                mine = regq[i].kind == RG_TEX && regq[i].t.serial == t->serial && regq[i].t.mem == t->mem;
            if (mine) {
                RegItem r = {.kind = RG_RELEASE, .t = *t};
                reg_push(&r);
            }
            plat_mutex_unlock(&regl);
        }
        return;
    }
    if (!t || !t->mem) return;
    tex_t *copy = malloc(sizeof *copy);
    if (!copy) rt_fatal("vk: out of memory (replay queue)");
    *copy = *t;
    ReplayItem it = {.kind = RI_RELEASE, .tex = copy};
    plat_mutex_lock(&rl);
    dr_calls[DR_TEX_RELEASE]++, dr_tcalls[DR_TEX_RELEASE]++;
    plat_mutex_unlock(&rl);
    replay_enqueue(&it);
}
static void vk_be_program_release(const prog_t *p) { replay_drain(DR_PROG_RELEASE); vk_program_release(p); }
// Asynchronous: the handles (nvn_submit_prepare_async ran under the front end's submit lock) are copied and queued;
// the value returned is the one the replay will signal.
static uint64_t vk_be_submit(const uint64_t *handles, int n) {
    vk_lowlat_submit_start();   // a frame's first submission: its simulation is over (nothing while low latency is off)
    if (!gfx_replay_async()) {
        sub_place_next = __atomic_load_n(&nvn_submits_in_frame, __ATOMIC_RELAXED);   // counted up after the submit returns
        return submit_sync(handles, n);
    }
    vk_sync_start();
    ReplayItem it = {.kind = RI_SUBMIT, .n = n, .submit_no = nvn_submit_count()};
    if (n > 0) {
        it.h = malloc(sizeof *it.h * (size_t)n);
        if (!it.h) rt_fatal("vk: out of memory (replay queue)");
        memcpy(it.h, handles, sizeof *it.h * (size_t)n);
    }
    vk_usnap_take(handles, n);   // the uniform blocks as they are now: the game writes on once this returns
    return replay_enqueue(&it);
}
static void vk_be_finish(void) { vk_sync_start(); replay_drain(DR_FINISH); vk_present_drain(); vk_sync_finish(); }
static int replay_check(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("NX2NAVI_REPLAY_CHECK");
        on = e && strcmp(e, "0");
        if (on) rt_log("vk: NX2NAVI_REPLAY_CHECK: every ReadTexels is read again after a full replay drain and compared");
    }
    return on;
}

// ReadTexels with the replay queued: waits for the newest submission recorded to write the texture (replayed and
// completed), not for the whole queue. The readback copy goes through the upload batch on the same queue, after every
// submission the replay has submitted. Submissions still queued or being recorded run after the copy, as on hardware
// when the GPU has not reached them yet: the game only sees their texels after waiting on a sync, and every sync
// value covers replay and GPU, so a read the game synchronised sees them. NX2NAVI_REPLAY_CHECK compares with a full drain.
static void vk_be_read_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row,
                              uint64_t img) {
    vk_sync_start();
    if (!gfx_replay_async()) {
        vk_read_texels(t, v, r, dst, row, img);
        return;
    }
    uint64_t f = vk_texture_last_write(t);
    plat_mutex_lock(&rl);
    dr_calls[DR_READ_TEXELS]++, dr_tcalls[DR_READ_TEXELS]++;
    int idle = !rq_n && !r_busy;
    plat_mutex_unlock(&rl);
    if (f && vk_sync_fence_done() < f) {
        uint64_t t0 = kern_now_ns();
        vk_sync_fence_wait(f, UINT64_MAX);   // counts as a fence wait in perf
        uint64_t d = kern_now_ns() - t0;
        plat_mutex_lock(&rl);
        dr_waits[DR_READ_TEXELS]++, dr_twaits[DR_READ_TEXELS]++;
        dr_ns[DR_READ_TEXELS] += d, dr_tns[DR_READ_TEXELS] += d;
        plat_mutex_unlock(&rl);
    }
    vk_read_texels(t, v, r, dst, row, img);
    if (!replay_check()) return;
    size_t n = vk_texture_texel_span(t, v, r, row, img);
    uint8_t *again = n ? malloc(n) : NULL;
    if (!again) return;
    memcpy(again, dst, n);   // bytes between the rows stay the caller's
    uint64_t f0 = vk_texture_last_write(t);
    replay_drain(DR_CHECK);
    uint64_t f1 = vk_texture_last_write(t);
    vk_read_texels(t, v, r, again, row, img);
    int differ = memcmp(again, dst, n) != 0;
    static volatile int32_t n_logged;
    if (differ && plat_inc32(&n_logged) <= 40) {
        size_t at = 0;
        while (at < n && again[at] == dst[at]) at++;
        rt_log("vk: replay check: ReadTexels of %p (format %#x, %dx%dx%d at %d,%d,%d) differs at byte %zu of %zu; writer "
               "fence %llu at the read, %llu after the drain, done %llu; queue %s at the read",
               (void *)t->mem, t->d.format, r->width, r->height, r->depth, r->xoffset, r->yoffset, r->zoffset, at, n,
               (unsigned long long)f0, (unsigned long long)f1, (unsigned long long)vk_sync_fence_done(), idle ? "idle" : "busy");
    }
    plat_mutex_lock(&rl);
    ck_reads++;
    if (differ) {
        ck_diff++;
        if (f1 == f0) ck_diff_unexplained++;
        if (idle) ck_diff_idle++;
    }
    plat_mutex_unlock(&rl);
    free(again);
}
static void vk_be_write_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, uint64_t row,
                               uint64_t img) {
    vk_sync_start();
    replay_drain(DR_WRITE_TEXELS);
    vk_present_drain();
    vk_write_texels(t, v, r, src, row, img);
}
static void vk_be_guest_written(uint64_t addr, uint64_t size) { vk_sync_start(); vk_guest_written(addr, size); }
// A CPU read of GPU-written mirrored storage (memcpy, pool invalidate). With the replay queued it waits for the newest
// submission recorded to write the textures there, not for the whole queue: the write-back copy then runs after their
// renders, and clears gpu_dirty only for content that is on the queue. Submissions not replayed yet render after the
// copy, as on hardware when the GPU has not reached them; they mark the texture dirty again for a later read.
static void vk_be_guest_read(uint64_t addr, uint64_t size) {
    if (!vk_texture_cpu_stale(addr, size)) return;
    vk_sync_start();
    if (gfx_replay_async()) {
        uint64_t f = vk_texture_dirty_fence(addr, size);
        plat_mutex_lock(&rl);
        dr_calls[DR_GUEST_READ]++, dr_tcalls[DR_GUEST_READ]++;
        plat_mutex_unlock(&rl);
        if (f && vk_sync_fence_done() < f) {
            uint64_t t0 = kern_now_ns();
            NVN_BLOCKING();
            vk_sync_fence_wait(f, UINT64_MAX);
            uint64_t d = kern_now_ns() - t0;
            plat_mutex_lock(&rl);
            dr_waits[DR_GUEST_READ]++, dr_twaits[DR_GUEST_READ]++;
            dr_ns[DR_GUEST_READ] += d, dr_tns[DR_GUEST_READ] += d;
            plat_mutex_unlock(&rl);
        }
    }
    vk_present_drain();
    vk_texture_write_back(addr, size);
}
// After a fence wait: the value is done, so its replay is too; a later submission the replay records meanwhile marks
// its targets with a later fence, which write_back_small leaves alone. No drain.
// The count of such waits that came back while event-ordered work was still held (vk_exec.c): a submission stopped at an
// event wait returns its value with its remainder held for a later replay, so the game may take a wait for that value
// as the whole submission done and reuse what the held part still reads. How often that happens decides whether the
// value should be signalled only after the remainder (audit finding F-2-05). The counts are atomic adds without a lock:
// the game's threads wait at the same time, and each line is logged by the thread whose add reached it.
// A wait for value 0 is left out: the window acquire's sync stands for no submission (nvn_api_obj.c
// nvnWindowAcquireTexture gives it fence 0), and such waits were half of the lines in two sessions of 2026-11-13.
static uint64_t synced_n, synced_held;
static void synced_held_count(uint64_t fence) {
    if (!fence) return;
    uint64_t n = __atomic_add_fetch(&synced_n, 1, __ATOMIC_RELAXED);
    int held = vk_exec_held();
    if (!held) return;
    uint64_t k = __atomic_add_fetch(&synced_held, 1, __ATOMIC_RELAXED);
    if (k <= 10 || k % 600 == 0)
        rt_log("vk: a game's wait for %llu returned with %d event-ordered submission(s) still held (%llu of %llu such "
               "waits so far)", (unsigned long long)fence, held, (unsigned long long)k, (unsigned long long)n);
}

static void vk_be_cpu_synced(uint64_t fence) {
    synced_held_count(fence);
    if (!vk_texture_small_dirty()) return;
    vk_sync_start();
    vk_present_drain();
    vk_texture_write_back_small(fence);
}

// A frame of the game starts. DLSS Frame Generation's low latency sleeps at the acquire (vk_dlssg.c); the direct path
// (vk_lowlat.c) at the acquire or the pad read, whichever the game makes first in the frame. At most one of the two
// is in effect in a run, and each returns at once while it is not.
static void vk_be_frame_start(void) {
    vk_dlssg_frame_start();
    vk_lowlat_frame_start(0);
}
static void vk_be_input_read(void) { vk_lowlat_frame_start(1); }

// The period nvnQueuePresentTexture's pacer keeps (GfxBackend.frame_paced). At most one of the two holds the limit in a
// run: DLSS Frame Generation's Reflex, which leaves the pacer its ceiling, or a driver's low latency, which leaves it
// nothing. Each lets go of the limit when given 0 (a movie plays).
static uint64_t vk_be_frame_paced(uint64_t period_ns) {
    if (vk_dlssg_pace(period_ns)) return frame_step_ceiling_ns();
    return vk_lowlat_pace(period_ns) ? 0 : period_ns;
}

const GfxBackend gfx_vulkan_backend = {
    .name = "vulkan",
    .pool_init = vk_be_pool_init,
    .pool_fini = vk_be_pool_fini,
    .pool_map_virtual = vk_be_pool_map_virtual,
    .texture_register = vk_be_texture_register,
    .sampler_register = vk_be_sampler_register,
    .texture_release = vk_be_texture_release,
    .program_release = vk_be_program_release,
    .submit = vk_be_submit,
    .fence_wait = vk_sync_fence_wait,
    .exec_wait = vk_be_exec_wait,
    .game_wait = vk_sync_game_wait,
    .fence_done = vk_sync_fence_done,
    .present = vk_be_present,
    .frame_start = vk_be_frame_start,
    .input_read = vk_be_input_read,
    .frame_paced = vk_be_frame_paced,
    .finish = vk_be_finish,
    .read_texels = vk_be_read_texels,
    .write_texels = vk_be_write_texels,
    .guest_written = vk_be_guest_written,
    .guest_read = vk_be_guest_read,
    .cpu_synced = vk_be_cpu_synced,
};

void vk_backend_shutdown(void) {
    replay_drain(DR_FINISH);
    gfx_exit_log();
    vk_present_drain();
    vk_sync_shutdown();
    vk_mem_shutdown();
    vk_shutdown();
}
