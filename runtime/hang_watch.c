// The hang watch: a report in the log when nothing moves for NX2NAVI_HANG_WATCH_S seconds (default 20; 0 off). A
// present, a GPU submission or the completion of one that never comes back (a deadlock, a driver wait that does not
// return, a GPU hang without a reset) left a log that just stopped, and nothing in it said where the threads were.
//
// Its own thread reads, once a second and without a lock, what moves while the game runs: the frames shown
// (gfx_counters.frames) and the GPU submissions issued and done (vk_sync.c vk_sync_progress). Two stalls are reported:
// - no frame shown for N s while the guest runs, the window is in front and not minimized and the process is not
//   ending: the game stops presenting when it loses the focus, so a focus change starts the count again, and the boot
//   screen (before the guest starts) is not judged;
// - submissions issued and not done, with "done" unchanged for N s, whatever the focus.
// One report per stall, and a new stall needs progress first; at most HW_REPORTS a run. The report names the stall,
// who holds the present, queue and window-start locks (gfx_present.c gfx_lock_holders), the guest threads' lock and
// thread-end waits (kernel.c kern_wait_report), and then writes the log out with logq_flush, which leaves the log
// thread on: rt_log_sync would make every later line a write on the thread that logs it, drawing threads included,
// in a run that may yet recover. The watch never ends the process, never calls the device, and takes no lock a
// drawing thread waits for, but for the kernel lock it tries during a report (kernel.c kern_wait_report).
#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "logq.h"
#include "nvn_api.h"
#include "platform/platform.h"

#define HW_REPORTS 3

typedef struct {   // what the watch reads each second
    uint64_t now_ns, frames, issued, done;
    int judge_present;   // the guest runs, the window is in front and not minimized, the process is not ending
} HwBeat;

typedef struct {
    uint64_t limit_ns;
    uint64_t frames, frames_at;   // the frame count last seen, and since when nothing moved while judged
    uint64_t done, done_at;       // the same for the submissions done, while some are outstanding
    int present_told, gpu_told;   // this stall is reported
    int reports;
} HangWatch;

enum { HW_NONE, HW_PRESENT, HW_GPU };

// One second's reading: which stall to report now, if any.
static int hw_step(HangWatch *w, const HwBeat *b) {
    if (b->frames != w->frames || !b->judge_present) {   // progress, or not judged: the count starts again
        w->frames = b->frames;
        w->frames_at = b->now_ns;
        w->present_told = 0;
    }
    if (b->done != w->done || b->issued <= b->done) {
        w->done = b->done;
        w->done_at = b->now_ns;
        w->gpu_told = 0;
    }
    if (w->reports >= HW_REPORTS) return HW_NONE;
    int gpu = !w->gpu_told && b->issued > b->done && b->now_ns - w->done_at >= w->limit_ns;
    int present = !w->present_told && b->judge_present && b->now_ns - w->frames_at >= w->limit_ns;
    if (!gpu && !present) return HW_NONE;
    w->reports++;
    w->present_told = 1;   // a GPU stall stops the frames too: one report for both
    if (gpu) w->gpu_told = 1;
    return gpu ? HW_GPU : HW_PRESENT;
}

static void hw_read(HwBeat *b) {
    b->now_ns = plat_now_ns();
    b->frames = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    vk_sync_progress(&b->issued, &b->done);
    b->judge_present = __atomic_load_n(&rt_guest_start_ns, __ATOMIC_RELAXED) && gfx_window_in_front() &&
                       !rt_exit_started();
}

static void hw_report(int kind, const HwBeat *b, const HangWatch *w) {
    int64_t world = frame_step_world_us();
    uint64_t since = b->now_ns - (kind == HW_GPU ? w->done_at : w->frames_at);
    rt_log("hang watch: %s for %.0f s (frames shown %llu; GPU submissions issued up to %llu, done up to %llu; window "
           "%s; world clock %.1f s); report %d of at most %d, the game goes on",
           kind == HW_GPU ? "GPU submissions not done" : "no frame shown with the window in front", since / 1e9,
           (unsigned long long)b->frames, (unsigned long long)b->issued, (unsigned long long)b->done,
           gfx_window_in_front() ? "in front" : "not in front", world >= 0 ? world / 1e6 : -1.0, w->reports, HW_REPORTS);
    GfxLockHolders h;
    gfx_lock_holders(&h);
    rt_log("hang watch: the present lock is held by thread %u (depth %u), the queue lock by thread %u (depth %u), the "
           "window start lock by thread %u (0: free)", h.present, h.present_depth, h.queue, h.queue_depth, h.start);
    kern_wait_report();
    logq_flush();
}

static void hang_watch_main(void *arg) {
    HangWatch w = {.limit_ns = (uint64_t)(uintptr_t)arg * 1000000000ull};
    HwBeat b;
    hw_read(&b);
    w.frames = b.frames, w.frames_at = b.now_ns, w.done = b.done, w.done_at = b.now_ns;
    while (w.reports < HW_REPORTS) {
        plat_sleep_ms(1000);
        hw_read(&b);
        int kind = hw_step(&w, &b);
        if (kind) hw_report(kind, &b, &w);
    }
}

void hang_watch_start(void) {
    int s = cfg_int("NX2NAVI_HANG_WATCH_S");
    if (s <= 0) return;
    if (!plat_thread_spawn(hang_watch_main, (void *)(uintptr_t)s, "hang watch", 0))
        rt_log("hang watch: its thread did not start");
}
