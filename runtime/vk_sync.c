// Submission, fences and the completion thread of the Vulkan back end.
// One timeline semaphore (VkCtx.timeline) orders everything: each submission records its command handles into a
// primary command buffer (vk_exec_record) and signals the next timeline value, which is the NVN fence value
// the front end hands to syncs. The completion thread waits on the timeline and, in submit order, runs the CPU side
// effects queued for each value (event writes, counter reports, sync state), then frees the memory and texture objects whose users
// completed. A value counts as done only after its side effects ran, so a guest woken by fence_wait sees them.
//
// One timeline value per submission, signalled at ALL_COMMANDS; waits use vkWaitSemaphores, and a failed wait is
// treated as a lost device.
// Contains code Copyright (c) 2026 Peiikai, Copyright (c) 2026 jhaizhou (MIT; see THIRD_PARTY_NOTICES.md).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "platform/platform.h"

#define WEAK __attribute__((weak))

typedef struct { uint64_t fence; void (*fn)(void *); void *arg; } Pending;
typedef struct { VkCommandBuffer cb; uint64_t fence; } CmdSlot;   // reusable once the timeline reached fence

static PlatMutex start_lock = PLAT_MUTEX_INIT;
static int started;
static PlatThread *completion_thread;

// Submissions: one at a time, so timeline values are signalled in the order they are handed out.
static PlatMutex sub_lock = PLAT_MUTEX_INIT;
static VkCommandPool cmd_pool;
static CmdSlot *slots;
static int nslots;
static int cur_slot;          // the slot vk_sync_submit records into now (an index: take_slot may move slots)
static uint64_t cur_fence;    // the value of the submission being recorded
// The command buffers of the part being recorded that come before slots[cur_slot] (vk_sync_append): earlier pieces of
// the replay's own and those the replay workers recorded, in queue order.
#define PART_CBS 254
static VkCommandBuffer part_cbs[PART_CBS];
static int npart_cbs;

// Shared with the completion thread (st_lock).
static PlatMutex st_lock = PLAT_MUTEX_INIT;
static PlatCond cv_work = PLAT_COND_INIT;   // a submission, a callback or stop
static PlatCond cv_done = PLAT_COND_INIT;   // done advanced
static uint64_t issued, done;     // newest submitted value; newest value whose side effects ran
static uint64_t reserved;         // asynchronous replay: newest value handed out before its submission (>= issued)
static int stop;
static Pending *queue;            // side effects in the order they were added
static int qhead, qn, qcap;
// The side effects the game's CPU reads (vk_completion_add_result: occlusion counts, counter reports, event values),
// in a queue of their own: the wait after a frame's first submission runs those that are due itself (results_run,
// "the executed timeline" below). One runner at a time, in the order they were added.
static Pending *rqueue;
static int rqhead, rqn, rqcap;
static PlatMutex res_lock = PLAT_MUTEX_INIT;   // the runner; taken before st_lock
static void results_run(uint64_t upto);

// ---- submit batching
// Every game submission used to be its own vkQueueSubmit2, about 150 a frame, each a trip into the driver on the render
// thread. Now up to hold_max of them are held and go to the queue together, as one vkQueueSubmit2 with one
// VkSubmitInfo2 each, in the order they were made; a part of a long submission (vk_sync_split) goes at once, with what
// is held before it, since it was split off to start the GPU early. Queue order is what orders them with everything
// else, so held work goes to the queue before anything else does or is waited for:
// - every other submitter, and every idle wait, takes the queue lock through vk_queue_lock, which flushes first;
// - a fence wait for a value still held flushes (fence_wait_impl), as do vk_sync_finish and shutdown;
// - the completion thread flushes what has been held for HOLD_NS, so a guest that polls memory a held submission
//   writes, instead of waiting on its fence, still sees it soon; it only tries the lock, never waits for it.
// H is guarded by the queue lock. NX2NAVI_SUBMIT_BATCH=0 (or 1) hands each to the queue at once; =N holds up to N.
#define HOLD_CAP 16
#define HOLD_NS 500000ull
static struct {
    int n;
    VkSubmitInfo2 si[HOLD_CAP];
    VkCommandBufferSubmitInfo cbi[HOLD_CAP][PART_CBS + 2];   // the prologue and the part's command buffers
    VkSemaphoreSubmitInfo sig[HOLD_CAP][2];
    uint64_t last;                    // the newest timeline value a held submission signals, 0 none
    uint64_t batches, subs;           // vkQueueSubmit2 calls and the submissions they carried
} H;
static volatile int32_t held_n;       // H.n, for readers without the lock
static volatile uint64_t held_t0;     // when the oldest held submission was held (kern_now_ns)
static volatile uint64_t queued_v;    // the newest timeline value handed to the queue
static int hold_max = -1;

// ---- the executed timeline
// The wait after a frame's first submission (nvn_api_core.c nvnQueueSubmitCommands) is there so that the GPU has run
// that submission's passes before the game rewrites what they read. With NX2NAVI_WB_SPLIT=0 it is the fence's wait.
// With 1, the default, it is a wait on exec_tl, after which the waiting thread itself runs the results the game
// reads (results_run, the last paragraph but one below). exec_tl is a second timeline that reaches a submission's value when
// the GPU has executed its own commands: the replay ends the part before the write-backs (vk_exec.c,
// vk_sync_split_exec; vk_memory.c vk_mem_mirror_end: what the shaders wrote into VRAM mirrors goes back to guest
// memory), that part signals exec_tl, and the write-backs follow in a part of their own, which signals the timeline
// as before. A submission without that split signals both at its end. Every other wait, the fences the game asks for
// among them, is the fence's either way.
// That was the default for a day, on the thought that the wait covered 3 to 4 ms of write-backs a frame that nothing
// needed before the game went on. It left out more than GPU work. A value counts as done only when the completion
// thread has run its side effects and those of every value before it, and among them is what the game's CPU reads
// next: the occlusion counts, counter reports and event values (vk_exec_xfer.c oq_cb, counter_cb, event_cb), and
// the stamps that tell the texture checks which pages the GPU wrote (vk_gpu_written). A frame's first submission is
// small and runs right behind the last frame's large one, so its signal on exec_tl comes while the completion thread
// is still at that one's callbacks. One frame with the wait on exec_tl:
//   1. the GPU finishes the last frame's main submission; the completion thread wakes and starts on its callbacks,
//      800 to 1,000 occlusion counts among them in Hyrule Field;
//   2. the GPU finishes this frame's first submission and signals exec_tl: the wait returns and the game goes on;
//   3. the game reads its occlusion counts (a 0 culls what the query tested) while some still hold the frame
//      before's; half a millisecond later, more when the CPUs are busy, the callbacks are through.
// Measured in Hyrule Field with the camera turning (RTX 5090, lazy write-back on, 2026-11-11; NX2NAVI_TRACE=perf, "waits
// after a submit on the executed timeline"): 2,267 of 2,320 such waits returned before those side effects had run,
// which were through 0.45 to 0.8 ms later on average and 12.6 ms at most; in that time 770 to 1,015 occlusion counts,
// 21 to 28 counter reports and 17 event values a frame were written into guest memory. The submission the game
// waits for wrote nothing back in any window of those runs, so its own write-backs were never what the wait left
// out. A player saw a patch of grass near the camera change from frame to frame there, and less of it with the
// fence's wait; no scripted run showed such a frame with either wait, so that the late counts are what the player
// saw is not shown. The fence's wait takes 0.65 to 0.7 ms more of a frame there: 85.5 against 86 and 92 frames a
// second in the field, 80.7 against 85.8, and 98 against 105 in a village, in single runs beside other work.
// Later a player's capture showed it plainly: in Hyrule Field, the camera nearly still, trees, ground and walls lost
// their direct light for single frames about every 36 frames (the game's own cycle there), 57 of 1,800 frames, with
// the wait on exec_tl alone and 28,000 to 33,000 occlusion counts per 60 frames written after the game had gone on.
// The fence's wait covers them but waits for everything else the completion thread does for the last frame's
// submissions as well: of its 0.49 ms a frame, 0.11 ms are the occlusion counts and 0.23 to 0.40 ms one loop that
// stamps the pages those submissions' shaders wrote (vk_exec_draw.c cb_done), which the game does not read.
// So the results the game reads have a queue of their own (vk_completion_add_result: occlusion counts, counter
// reports, event values), and the waiter runs those of its submission and of the ones before it as soon as exec_tl is
// reached (results_run; the GPU ran those submissions first, so their queries are complete). The completion thread
// runs them the same way when it reaches a value, before its other callbacks, and one runner at a time keeps them in
// the order they were queued. What stays behind the wait: the write-backs and the page stamps, as with the bare wait.
// A player's hand test of this wait at the place of that capture showed none of those frames (2026-11-11). No
// scripted run has measured it: the one run made for it, with the wait on exec_tl alone, standing in the same field,
// had 8,147 of 8,160 waits back early and 1,170 occlusion counts a frame written after them, and showed no such
// frame either, so the scripted scene does not reproduce what the player saw. What the wait costs against the
// bare one is not measured; the results it runs take 0.11 ms a frame on the completion thread there.
// NX2NAVI_WB_SPLIT=3 is the wait on exec_tl alone, as it was, to measure against.
// With shadow storage (vk_memory.c) NX2NAVI_WB_SPLIT=1 keeps the fence's wait as well: there the write-backs
// themselves reach guest memory in a completion callback. With an import a
// write-back stores into guest memory on the GPU, right behind the passes. With a shadow copy it stores into the copy,
// and the bytes reach guest memory in the fence's completion callback (shadow_wb_cb), after the write-back part, the
// fence and the callbacks queued before it, while the game is already rewriting for its next frame what those passes
// wrote. A write-back that lands after such a rewrite puts the GPU's older bytes over it in guest memory and moves the
// page's base copy along, so the next copy-in finds nothing the CPU changed; and a rewrite with the value the base
// copy holds (a reset to zero) is no difference at any time. The GPU then goes on from its own last output, as the
// game's lighting accumulators do, which the shaders add to and the CPU resets.
// Measured: with the split the whole 3D scene came out brighter for single frames (3.8 of 255, the interface as it
// was), each of them a frame whose main work was held at its CPU event wait and replayed late, together with the next
// frame's: standing outdoors, 695, 624, 443, 323 and 2 of 1,800 frames in five runs, and none in eight runs without
// the split (RTX 5090 with NX2NAVI_SHADOW=1, 2026-11-10). A player on Linux (RX 7800 XT, RADV) saw the open field lit
// in one frame and flat in the next; that it is the same fault is not confirmed on their machine. Which words the
// write-backs overtook was not traced: NX2NAVI_SHADOW_CHECK counts no byte that both sides changed to different
// values, which fits a reset to the base copy's value.
// NX2NAVI_WB_SPLIT=2 keeps the split with shadow storage too, to measure that fault.
static VkSemaphore exec_tl;
static uint64_t exec_sig;             // sub_lock: the newest value a submitted part signals on exec_tl
static int wb_split = -1;
static int wb_results;                // the wait on exec_tl also runs the results the game reads (not with 3)

int vk_sync_wb_split(void) {
    if (wb_split < 0) {
        const char *e = cfg_get("NX2NAVI_WB_SPLIT");
        int forced = e && !strcmp(e, "2"), bare = e && !strcmp(e, "3"), shadow = vk_mem_shadow_storage();
        wb_results = !bare;
        wb_split = cfg_bool("NX2NAVI_WB_SPLIT") && (forced || !shadow);
        rt_log("vk: the wait after a frame's first submission %s (NX2NAVI_WB_SPLIT%s)",
               !wb_split ? "is the fence's: it covers the write-backs and the completion thread's side effects"
               : wb_results ? "ends when the GPU has run the submission's own commands and the results the game reads "
                              "are in its memory: the write-backs and the other side effects are left out"
                            : "ends when the GPU has run the submission's own commands: its write-backs and every side "
                              "effect of the completion thread are left out",
               shadow && !forced ? "; shadow storage keeps the fence's wait unless it is 2" : "");
    }
    return wb_split && exec_tl != VK_NULL_HANDLE;
}

// What a wait on exec_tl leaves behind: the completion thread's side effects for the values up to the waiter's.
// Counted for NX2NAVI_TRACE=perf (vk_exec.c logs them): the waits that returned with such side effects still to
// run, how long after the return they were through, and (vk_sync_effect_late, asked by the callbacks that write
// into guest memory) how many of those ran in that time.
static volatile uint64_t exw_n, exw_early, exw_lag_ns, exw_lag_max;   // since the last vk_sync_exec_stats
static volatile uint64_t exw_value, exw_t0;   // the newest wait that returned early: its value and when; 0 none
static volatile int32_t cb_late;              // the callback being run belongs to a value such a waiter is past

int vk_sync_effect_late(void) { return __atomic_load_n(&cb_late, __ATOMIC_RELAXED); }

// The value whose side effect the calling thread is running, 0 when it is not the completion thread inside one.
static volatile uint64_t cb_value;
static volatile uint32_t cb_tid;

uint64_t vk_sync_cb_value(void) {
    return __atomic_load_n(&cb_tid, __ATOMIC_ACQUIRE) == plat_thread_id() ? __atomic_load_n(&cb_value, __ATOMIC_RELAXED) : 0;
}

void vk_sync_exec_stats(uint64_t *waits, uint64_t *early, uint64_t *lag_ns, uint64_t *lag_max_ns) {
    *waits = __atomic_exchange_n(&exw_n, 0, __ATOMIC_RELAXED), *early = __atomic_exchange_n(&exw_early, 0, __ATOMIC_RELAXED);
    *lag_ns = __atomic_exchange_n(&exw_lag_ns, 0, __ATOMIC_RELAXED);
    *lag_max_ns = __atomic_exchange_n(&exw_lag_max, 0, __ATOMIC_RELAXED);
}

// NX2NAVI_TRACE=perf: the completion thread's time by callback (st_lock), since the last vk_sync_cb_stats. A callback
// is named by its distance from vk_completion_add, which the build's link map turns into its function.
#define CBT_N 32
static struct { void (*fn)(void *); uint64_t ns, calls; } cbt[CBT_N];
static int cbt_n;

void vk_sync_cb_stats(char *out, size_t n) {
    int k = 0;
    *out = 0;
    plat_mutex_lock(&st_lock);
    for (int i = 0; i < cbt_n && k < (int)n - 64; i++) {
        if (!cbt[i].calls) continue;
        k += snprintf(out + k, n - (size_t)k, " %+lld %.3f ms %llu calls;",
                      (long long)((intptr_t)cbt[i].fn - (intptr_t)vk_completion_add), cbt[i].ns / 1e6,
                      (unsigned long long)cbt[i].calls);
        cbt[i].ns = cbt[i].calls = 0;
    }
    plat_mutex_unlock(&st_lock);
}

// NX2NAVI_DEVICE_LOST_AT=n (a test of the lost device's end, vk_device.c vk_device_lost_check): the n-th hand-over to
// the queue is not made and counts as one the driver answered with a lost device. 0 or unset: never.
static int lost_at_due(void) {
    static int at = -1;
    static int32_t calls;
    if (at < 0) {
        const char *e = cfg_get("NX2NAVI_DEVICE_LOST_AT");
        at = e && *e ? atoi(e) : 0;
        if (at < 0) at = 0;
        if (at) rt_log("vk: queue hand-over %d will be taken as a lost device (NX2NAVI_DEVICE_LOST_AT)", at);
    }
    return at && ++calls == at;   // queue lock held
}

static void hold_flush_locked(VkCtx *c) {
    if (!H.n) return;
    if (vk_device_is_lost()) {   // the run is ending: the held submissions are dropped, nothing reaches the queue
        H.n = 0, H.last = 0;
        __atomic_store_n(&held_n, 0, __ATOMIC_RELEASE);
        return;
    }
    VkResult r = lost_at_due() ? VK_ERROR_DEVICE_LOST : vkQueueSubmit2(c->gfx_queue, (uint32_t)H.n, H.si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        vk_device_lost_check(r, "the submit of the replay's command buffers");
        rt_fatal("vk: vkQueueSubmit2 of %d held submissions up to value %llu failed (%d)", H.n,
                 (unsigned long long)H.last, (int)r);
    }
    if (H.last) __atomic_store_n(&queued_v, H.last, __ATOMIC_RELEASE);
    H.batches++, H.subs += (uint64_t)H.n;
    H.n = 0, H.last = 0;
    __atomic_store_n(&held_n, 0, __ATOMIC_RELEASE);
}

void vk_sync_flush_held(void) {
    if (!H.n) return;   // queue lock held: H is stable
    VkCtx *c = vk_ctx();
    if (c) hold_flush_locked(c);
}

// The replay's submission si (signalling value, 0 for a part without a signal), held or handed to the queue.
static void queue_submit(VkCtx *c, const VkSubmitInfo2 *si, uint64_t value) {
    if (hold_max < 0) {
        const char *e = cfg_get("NX2NAVI_SUBMIT_BATCH");
        int v = e && *e ? atoi(e) : 4;
        hold_max = v < 1 ? 1 : v > HOLD_CAP ? HOLD_CAP : v;
        rt_log("vk: up to %d submissions handed to the queue together (NX2NAVI_SUBMIT_BATCH)", hold_max);
    }
    // The present holds this lock for its whole flip. A slow flip (frame generation allocating, a swapchain remade) makes
    // the submitting thread wait here; counted as a present wait, so the slow-frame line no longer shows such a frame
    // with nothing blocked.
    uint64_t p0 = perf_begin();
    vk_queue_lock_raw();
    perf_end(PERF_FENCE, p0);
    if (H.n == HOLD_CAP) hold_flush_locked(c);
    int i = H.n++;
    H.si[i] = *si;
    if (si->commandBufferInfoCount)
        memcpy(H.cbi[i], si->pCommandBufferInfos, sizeof H.cbi[i][0] * si->commandBufferInfoCount);
    H.si[i].pCommandBufferInfos = H.cbi[i];
    if (si->signalSemaphoreInfoCount) {   // at most two: the timeline and the executed timeline (exec_tl)
        memcpy(H.sig[i], si->pSignalSemaphoreInfos, sizeof H.sig[i][0] * si->signalSemaphoreInfoCount);
        H.si[i].pSignalSemaphoreInfos = H.sig[i];
    }
    if (value) H.last = value;
    uint64_t now = kern_now_ns();
    if (i == 0) __atomic_store_n(&held_t0, now, __ATOMIC_RELEASE);
    __atomic_store_n(&held_n, H.n, __ATOMIC_RELEASE);
    // A part of a long submission (no value) goes at once with whatever is held: parts exist to start the GPU early.
    // So do held submissions that have waited HOLD_NS, and a full batch.
    if (H.n >= hold_max || !value || now - __atomic_load_n(&held_t0, __ATOMIC_ACQUIRE) >= HOLD_NS) hold_flush_locked(c);
    vk_queue_unlock();
}

// Hands the held submissions to the queue when value is among them (a waiter needs it) or they have waited HOLD_NS
// (aged). The completion thread passes try_only: it never waits for the queue lock, whose holders may wait for it.
static void hold_release(uint64_t value, int aged, int try_only) {
    if (!__atomic_load_n(&held_n, __ATOMIC_ACQUIRE)) return;
    int due = value > __atomic_load_n(&queued_v, __ATOMIC_ACQUIRE) ||
              (aged && kern_now_ns() - __atomic_load_n(&held_t0, __ATOMIC_ACQUIRE) >= HOLD_NS);
    if (!due) return;
    if (try_only) {
        if (!vk_queue_trylock_raw()) return;   // the holder's thread is submitting: it flushes or holds on
    } else {
        vk_queue_lock_raw();
    }
    vk_sync_flush_held();
    vk_queue_unlock();
}

// ---- GPU time (NX2NAVI_TRACE=perf): each submission part (vk_sync_split) writes a timestamp when it starts and when it
// ends, into a ring of query pairs. The completion thread reads a pair once the part's value is reached and adds the
// part's interval to the busy time, counting overlap with the previous part once. Off (one log line) when the queue
// family has no timestamp bits or the period is 0. Upload batches (vk_memory.c) are not timed.
#define TS_PAIRS 1024
static VkQueryPool ts_pool;
static int ts_state;                       // 0 not set up, 1 on, -1 off
static double ts_period;                   // ns per tick
static uint64_t ts_mask;                   // valid bits of a timestamp
static uint32_t ts_next;                   // submitting thread (sub_lock): next pair
static volatile uint8_t ts_busy[TS_PAIRS]; // written until the completion thread read the pair
static int ts_cur = -1;                    // the pair of the part being recorded, -1 untimed
static uint64_t ts_last_end;               // completion thread: end of the latest part read (ticks)
static volatile int64_t ts_busy_ns, ts_parts, ts_missed;
// The same durations, never reset: dynamic resolution differences it across presented frames (the perf window
// consumes ts_busy_ns, so a reader that sampled it would steal from the perf line and see a reset every window).
static volatile int64_t ts_busy_all;
// GPU idle (with calibrated timestamps): the gap before each timed part, split by whether the part was submitted after
// the previous one had ended (the GPU waited for the CPU: starved) or before (it waited for untimed work or a wait).
static uint64_t ts_sub_ns[TS_PAIRS];       // CPU time the part was handed to vkQueueSubmit2
// A pair to read at its value. mirror: the copies of a mirror prologue (vk_sync.c prologue), counted apart as well.
typedef struct { uint32_t pair; int mirror; uint64_t fence; } TsRead;
static TsRead *ts_unsent[4];               // closed and not yet submitted (sub_lock)
static int ts_n_unsent;
static volatile int64_t ts_mirror_ns;     // of ts_busy_ns, the mirror prologues' copies
static uint64_t ts_last_end_ns;            // completion thread: ts_last_end on the CPU clock
static volatile int64_t ts_idle_ns, ts_starved_ns, ts_gaps, ts_max_gap_ns, ts_queued_ns;
static int64_t cal_tick, cal_ns, cal_at;   // completion thread: a calibrated (GPU tick, CPU ns) pair and when it was taken
static PFN_vkGetCalibratedTimestampsKHR cal_fn;

// On the first submission that wants timing (perf_gpu_timing: NX2NAVI_TRACE=perf or the full performance overlay).
static void ts_setup(VkCtx *c) {
    ts_state = -1;
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq ? nq : 1, sizeof *qp);
    if (!qp) return;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, qp);
    uint32_t bits = c->gfx_family < nq ? qp[c->gfx_family].timestampValidBits : 0;
    free(qp);
    float period = c->props.limits.timestampPeriod;
    if (!bits || period <= 0) {
        rt_log("vk: no GPU timestamps on this queue (valid bits %u, period %.3f ns): the perf line shows no GPU time", bits, period);
        return;
    }
    VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                .queryCount = 2 * TS_PAIRS};
    VkResult r = vkCreateQueryPool(c->device, &qi, NULL, &ts_pool);
    if (r != VK_SUCCESS) {
        rt_log("vk: timestamp query pool failed (%d): the perf line shows no GPU time", (int)r);
        return;
    }
    ts_period = period;
    ts_mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
    ts_state = 1;
}

// Completion thread: refresh the GPU tick / CPU clock pair every 250 ms; 0 when there is no calibration. The host
// domain is the clock plat_now_ns reads (QPC on Windows, CLOCK_MONOTONIC elsewhere), plat_counter_hz ticks a second.
static int cal_refresh(VkCtx *c) {
    if (!c->has_calibrated) return 0;
    if (!cal_fn) {
        cal_fn = (PFN_vkGetCalibratedTimestampsKHR)vk_dpa(c->device, c->has_calibrated == 2 ? "vkGetCalibratedTimestampsKHR"
                                                                                                        : "vkGetCalibratedTimestampsEXT");
        if (!cal_fn) { c->has_calibrated = 0; return 0; }
    }
    int64_t now = (int64_t)plat_now_ns();
    if (cal_at && now - cal_at < 250000000) return 1;
    VkCalibratedTimestampInfoKHR ci[2] = {{.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, .timeDomain = VK_TIME_DOMAIN_DEVICE_KHR},
#ifdef _WIN32
                                          {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, .timeDomain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR}};
#else
                                          {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, .timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR}};
#endif
    uint64_t v[2], dev;
    if (cal_fn(c->device, 2, ci, v, &dev) != VK_SUCCESS) {
        if (!cal_at) {   // the domain pair is not supported: no split
            rt_log("vk: calibrated timestamps failed: the perf line shows no GPU idle split");
            c->has_calibrated = 0;
        }
        return cal_at != 0;
    }
    cal_tick = (int64_t)(v[0] & ts_mask), cal_ns = (int64_t)((double)v[1] * 1e9 / (double)plat_counter_hz()), cal_at = now;
    return 1;
}

static int64_t tick_ns(uint64_t t) { return cal_ns + (int64_t)((double)((int64_t)t - cal_tick) * ts_period); }

static void max64(volatile int64_t *m, int64_t v) {
    int64_t o = __atomic_load_n(m, __ATOMIC_RELAXED);
    while (v > o && !__atomic_compare_exchange_n(m, &o, v, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

static void ts_read_cb(void *arg);

// Just before vkQueueSubmit2 (sub_lock held): the parts closed since the last submit leave the CPU now. Their reads
// are queued in the order the GPU runs them: a mirror prologue is closed after the part it was made for but runs
// first. Read after its part, its copies counted neither as busy time nor as a gap of their own: they showed up as
// idle time with work queued, 7-8 ms a frame over PCIe 3.0 in Kokiri Forest, and the frame as CPU-bound.
static void ts_submitted(void) {
    uint64_t now = plat_now_ns();
    gfx_tl_note('S', now, cur_fence, 0);
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < ts_n_unsent; i++) {
            TsRead *t = ts_unsent[i];
            if (!t->mirror != pass) continue;   // the prologues first
            ts_sub_ns[t->pair] = now;
            vk_completion_add(t->fence, ts_read_cb, t);
        }
    ts_n_unsent = 0;
}

static void ts_read_cb(void *arg) {   // completion thread, in submission order
    TsRead *t = arg;
    VkCtx *c = vk_ctx();
    uint64_t v[2];
    if (vkGetQueryPoolResults(c->device, ts_pool, 2 * t->pair, 2, sizeof v, v, sizeof v[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        uint64_t s = v[0] & ts_mask, e = v[1] & ts_mask;
        if (e >= s) {
            if (cal_refresh(c)) {
                int64_t sn = tick_ns(s), en = tick_ns(e), sub = (int64_t)ts_sub_ns[t->pair];
                if (ts_last_end_ns && sn > ts_last_end_ns) {
                    int64_t gap = sn - ts_last_end_ns, late = sub - ts_last_end_ns;
                    plat_add64(&ts_idle_ns, gap);
                    if (late > 0) plat_add64(&ts_starved_ns, late < gap ? late : gap);
                    if (gap > 500000) plat_inc64(&ts_gaps);
                    max64(&ts_max_gap_ns, gap);
                }
                if (sub && sn > sub) plat_add64(&ts_queued_ns, sn - sub);
                gfx_tl_note('G', (uint64_t)sn, (uint64_t)en, (uint64_t)sub);
                if (en > ts_last_end_ns) ts_last_end_ns = en;
            }
            uint64_t from = s > ts_last_end ? s : ts_last_end;
            if (e > from) {
                int64_t ns = (int64_t)((double)(e - from) * ts_period);
                plat_add64(&ts_busy_ns, ns);
                if (t->mirror) plat_add64(&ts_mirror_ns, ns);
                plat_add64(&ts_busy_all, ns);
                perf_gpu_busy((uint64_t)ns);
            }
            if (e > ts_last_end) ts_last_end = e;
            plat_inc64(&ts_parts);
        } else plat_inc64(&ts_missed);   // the counter wrapped
    } else plat_inc64(&ts_missed);
    __atomic_store_n(&ts_busy[t->pair], 0, __ATOMIC_RELEASE);
    free(t);
}

// ---- frame timeline
// NX2NAVI_GPU_TIMELINE=<frame>: from that presented frame on, TL_FRAMES frames of what the CPU and the GPU did go to the
// log once they are complete, in milliseconds from the first of those presents: each replay's begin and end and its
// thread, each timed part's submit, GPU start and end (calibrated timestamps, so with NX2NAVI_TRACE=perf), the waits for
// the GPU after a frame's first submission, the replay workers' groups, chunks and the submitting thread's waits for
// them, and the presents. It tells whether the GPU waits for the replay, for a wait, or for the CPU behind one. Off
// (and one load per note) without the setting.
#define TL_CAP 16384
#define TL_FRAMES 4
typedef struct { uint64_t t, a, b; uint32_t tid; int kind; } TlEv;
static TlEv *tl_ev;
static volatile int32_t tl_n, tl_rec;
static int tl_on = -1;
static uint64_t tl_from, tl_frames;

void gfx_tl_note(int kind, uint64_t t, uint64_t a, uint64_t b) {
    if (!__atomic_load_n(&tl_rec, __ATOMIC_ACQUIRE)) return;
    int32_t i = plat_inc32(&tl_n) - 1;
    if (i < TL_CAP) tl_ev[i] = (TlEv){t ? t : plat_now_ns(), a, b, plat_thread_id(), kind};
}

static int tl_cmp(const void *x, const void *y) {
    uint64_t a = ((const TlEv *)x)->t, b = ((const TlEv *)y)->t;
    return a < b ? -1 : a > b;
}

static void tl_dump(void) {
    int n = tl_n < TL_CAP ? tl_n : TL_CAP;
    qsort(tl_ev, (size_t)n, sizeof *tl_ev, tl_cmp);
    uint64_t t0 = 0, t1 = UINT64_MAX;
    for (int i = 0; i < n; i++)
        if (tl_ev[i].kind == 'P') {
            if (tl_ev[i].a == tl_from) t0 = tl_ev[i].t;
            if (tl_ev[i].a == tl_from + TL_FRAMES) t1 = tl_ev[i].t;
        }
    rt_log("vk: timeline of frames %llu-%llu (%d events), ms from the first present:", (unsigned long long)tl_from,
           (unsigned long long)(tl_from + TL_FRAMES), n);
    for (int i = 0; i < n; i++) {
        const TlEv *e = &tl_ev[i];
        if (e->t < t0 || e->t > t1) continue;
        double at = (double)(int64_t)(e->t - t0) / 1e6;
        switch (e->kind) {
        case 'P': rt_log("vk: tl %8.3f t%-5u present, frame %llu", at, e->tid, (unsigned long long)e->a); break;
        case 'R': rt_log("vk: tl %8.3f t%-5u replay begins: %llu handles, value %llu", at, e->tid,
                         (unsigned long long)e->a, (unsigned long long)e->b); break;
        case 'r': rt_log("vk: tl %8.3f t%-5u replay ends: %llu commands in the last part", at, e->tid,
                         (unsigned long long)e->a); break;
        case 'W': rt_log("vk: tl %8.3f t%-5u waits for the GPU to run value %llu", at, e->tid, (unsigned long long)e->a); break;
        case 'w': rt_log("vk: tl %8.3f t%-5u wait ends", at, e->tid); break;
        case 'g': rt_log("vk: tl %8.3f t%-5u group of %llu draws in %llu chunks", at, e->tid, (unsigned long long)e->a,
                         (unsigned long long)e->b); break;
        case 'h': rt_log("vk: tl %8.3f t%-5u group recorded and in the part", at, e->tid); break;
        case 'q': rt_log("vk: tl %8.3f t%-5u waits for its group: %s", at, e->tid, (const char *)(uintptr_t)e->a); break;
        case 'u': rt_log("vk: tl %8.3f t%-5u group done, goes on", at, e->tid); break;
        case 'c': rt_log("vk: tl %8.3f t%-5u records chunk %llu (%llu ops)", at, e->tid, (unsigned long long)e->a,
                         (unsigned long long)e->b); break;
        case 'd': rt_log("vk: tl %8.3f t%-5u chunk %llu done", at, e->tid, (unsigned long long)e->a); break;
        case 'S': rt_log("vk: tl %8.3f t%-5u part submitted, value %llu", at, e->tid, (unsigned long long)e->a); break;
        case 'G': rt_log("vk: tl %8.3f gpu    part runs %.3f ms (submitted %.3f ms before)", at,
                         (double)(e->a - e->t) / 1e6, (double)(int64_t)(e->t - e->b) / 1e6); break;
        default: break;
        }
    }
}

void gfx_tl_present(void) {
    if (tl_on < 0) {
        const char *e = cfg_get("NX2NAVI_GPU_TIMELINE");
        tl_from = e && *e ? strtoull(e, NULL, 0) : 0;
        tl_on = tl_from && (tl_ev = calloc(TL_CAP, sizeof *tl_ev)) != NULL;
    }
    if (!tl_on) return;
    uint64_t f = ++tl_frames;
    if (f == tl_from) __atomic_store_n(&tl_rec, 1, __ATOMIC_RELEASE);
    gfx_tl_note('P', 0, f, 0);
    if (f == tl_from + TL_FRAMES + 3) {   // the parts of the last frame have been read back by now
        __atomic_store_n(&tl_rec, 0, __ATOMIC_SEQ_CST);
        tl_dump();
        tl_on = 0;
    }
}

// At the start of a command buffer (sub_lock held): the pair it times, -1 untimed.
static int ts_open(VkCommandBuffer cb) {
    if (ts_state != 1 || !perf_gpu_timing()) {
        if (ts_state || !perf_gpu_timing()) return -1;
        ts_setup(vk_ctx());   // the overlay turned full: timestamps from now on
        if (ts_state != 1) return -1;
    }
    uint32_t p = ts_next % TS_PAIRS;
    if (__atomic_load_n(&ts_busy[p], __ATOMIC_ACQUIRE)) { plat_inc64(&ts_missed); return -1; }
    ts_next++;
    ts_busy[p] = 1;
    vkCmdResetQueryPool(cb, ts_pool, 2 * p, 2);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, ts_pool, 2 * p);
    return (int)p;
}

// At its end, before vkEndCommandBuffer; fence: the value whose completion covers it. The read is queued at the
// submit (ts_submitted).
static void ts_close(VkCommandBuffer cb, int pair, uint64_t fence, int mirror) {
    if (pair < 0) return;
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ts_pool, 2 * (uint32_t)pair + 1);
    ts_sub_ns[pair] = 0;
    TsRead *t = malloc(sizeof *t);
    if (!t) rt_fatal("vk: out of memory");
    *t = (TsRead){(uint32_t)pair, mirror, fence};
    if (ts_n_unsent < 4) ts_unsent[ts_n_unsent++] = t;
    else vk_completion_add(fence, ts_read_cb, t);   // more than a part and its prologue between two submits
}

// The pair of the part being recorded.
static void ts_begin(VkCommandBuffer cb) { ts_cur = ts_open(cb); }

static void ts_end(VkCommandBuffer cb, uint64_t fence) {
    ts_close(cb, ts_cur, fence, 0);
    ts_cur = -1;
}

// An empty submission records no command buffer: its pair is never written.
static void ts_drop(void) {
    if (ts_cur < 0) return;
    __atomic_store_n(&ts_busy[ts_cur], 0, __ATOMIC_RELEASE);
    ts_cur = -1;
}

uint64_t vk_gpu_busy_all(void) { return (uint64_t)__atomic_load_n(&ts_busy_all, __ATOMIC_RELAXED); }

static void gpu_stats(PerfGpu *g) {
    g->have_gpu = ts_state == 1;
    g->gpu_ns = (uint64_t)plat_xchg64(&ts_busy_ns, 0);
    g->gpu_parts = (uint64_t)plat_xchg64(&ts_parts, 0);
    g->gpu_missed = (uint64_t)plat_xchg64(&ts_missed, 0);
    g->have_vram = vk_mem_vram(&g->vram_used, &g->vram_budget, &g->vram_exact);
    g->have_idle = vk_ctx()->has_calibrated != 0 && cal_at != 0;
    g->idle_ns = (uint64_t)plat_xchg64(&ts_idle_ns, 0);
    g->starved_ns = (uint64_t)plat_xchg64(&ts_starved_ns, 0);
    g->queued_ns = (uint64_t)plat_xchg64(&ts_queued_ns, 0);
    g->idle_gaps = (uint64_t)plat_xchg64(&ts_gaps, 0);
    g->max_gap_ns = (uint64_t)plat_xchg64(&ts_max_gap_ns, 0);
    g->mirror_ns = (uint64_t)plat_xchg64(&ts_mirror_ns, 0);
}

// Weak stand-in for vk_exec.c's replay: only empty recordings ({OP_BEGIN, OP_END}) can be submitted.
WEAK int vk_exec_record(VkCommandBuffer cb, const uint64_t *handles, int n, uint64_t fence) {
    (void)cb, (void)fence;
    for (int i = 0; i < n; i++) {
        const op_hdr *h = handles[i] ? nvn_op_first(handles[i]) : NULL;
        if (!h || h->op != OP_BEGIN || h->size < sizeof *h || nvn_op_next(h)->op != OP_END)
            rt_fatal("vk: op replay is not linked");
    }
    return 0;
}

static void completion_main(void *arg) {
    VkCtx *c = arg;
    __atomic_store_n(&cb_tid, plat_thread_id(), __ATOMIC_RELEASE);
    plat_mutex_lock(&st_lock);
    for (;;) {
        while (!stop && done == issued && !(qn && queue[qhead].fence <= done) && !(rqn && rqueue[rqhead].fence <= done))
            plat_cond_wait(&cv_work, &st_lock);
        if (stop) break;
        uint64_t reached = done;
        if (issued > done) {
            uint64_t want = done + 1;
            plat_mutex_unlock(&st_lock);
            // Held submissions that have waited long enough go to the queue; while some are held, the wait is short so
            // their age is checked again (submit batching).
            hold_release(0, 1, 1);
            uint64_t tmo = __atomic_load_n(&held_n, __ATOMIC_ACQUIRE) ? HOLD_NS : 100000000ull;
            VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                                      .pSemaphores = &c->timeline, .pValues = &want};
            VkResult r = vkWaitSemaphores(c->device, &wi, tmo);   // 100 ms: stop is checked between waits
            if (r != VK_SUCCESS && r != VK_TIMEOUT) {
                vk_device_lost_check(r, "the completion thread's wait for a submission");
                rt_fatal("vk: vkWaitSemaphores failed (%d; device lost?)", (int)r);
            }
            // A lost device is reported here, where it is first seen. Unchecked, the value was taken as it came: the
            // callbacks of submissions that never ran were called as if their work were done (one report's first sign
            // was an occlusion query "not available after its fence"), and the game went on recording until the next
            // checked queue call failed, an upload's, which then read as the cause.
            r = vkGetSemaphoreCounterValue(c->device, c->timeline, &reached);
            if (r != VK_SUCCESS) {
                vk_device_lost_check(r, "the completion thread's read of the submissions' timeline");
                rt_fatal("vk: the submissions' timeline could not be read (%d; device lost?)", (int)r);
            }
            // Both calls may succeed on a lost device and give a value its submissions never reached. So before a
            // new value counts, the device is asked once more with a call of its own (vk_device.c
            // vk_device_lost_poll, which ends the run when it is lost).
            if (reached > __atomic_load_n(&done, __ATOMIC_ACQUIRE)) vk_device_lost_poll();
            plat_mutex_lock(&st_lock);
            if (reached > issued) reached = issued;
        }
        // Another thread saw the device lost (vk_lost): no value counts from here on and no callback runs. Theirs is
        // work that never finished: frees of what the GPU may still hold, records handed out again, counter reports.
        // done stays where it is, the waiters stay asleep, and the thread that saw the loss ends the process.
        if (vk_device_is_lost()) break;
        plat_mutex_unlock(&st_lock);
        results_run(reached);   // what the game reads first; a waiter after its submit may have run them already
        plat_mutex_lock(&st_lock);
        if (vk_device_is_lost()) break;
        while (qn && queue[qhead].fence <= reached) {
            Pending p = queue[qhead++];
            qn--;
            plat_mutex_unlock(&st_lock);
            __atomic_store_n(&cb_late, p.fence <= __atomic_load_n(&exw_value, __ATOMIC_ACQUIRE), __ATOMIC_RELAXED);
            uint64_t c0 = (rt_trace & RT_TRACE_PERF) ? kern_now_ns() : 0;
            __atomic_store_n(&cb_value, p.fence, __ATOMIC_RELAXED);
            p.fn(p.arg);
            __atomic_store_n(&cb_value, 0, __ATOMIC_RELAXED);
            uint64_t c1 = c0 ? kern_now_ns() : 0;
            __atomic_store_n(&cb_late, 0, __ATOMIC_RELAXED);
            plat_mutex_lock(&st_lock);
            if (c0) {
                int k = 0;
                while (k < cbt_n && cbt[k].fn != p.fn) k++;
                if (k == cbt_n && cbt_n < CBT_N) cbt[cbt_n++].fn = p.fn;
                if (k < cbt_n) cbt[k].ns += c1 - c0, cbt[k].calls++;
            }
            if (vk_device_is_lost()) break;
        }
        if (vk_device_is_lost()) break;
        if (reached > done) {
            uint64_t ev = __atomic_load_n(&exw_value, __ATOMIC_ACQUIRE);
            if (ev && reached >= ev) {   // the side effects a waiter on exec_tl went ahead of are through
                uint64_t lag = kern_now_ns() - __atomic_load_n(&exw_t0, __ATOMIC_RELAXED);
                __atomic_fetch_add(&exw_lag_ns, lag, __ATOMIC_RELAXED);
                if (lag > __atomic_load_n(&exw_lag_max, __ATOMIC_RELAXED)) __atomic_store_n(&exw_lag_max, lag, __ATOMIC_RELAXED);
                __atomic_compare_exchange_n(&exw_value, &ev, 0, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
            }
            // The waiters go first (a fence wait, the frame's first submission, the acquire): freeing what the
            // finished submissions held (vk_mem_collect: upload batches, destroyed images, views and buffers, each
            // checked against the timeline itself) is nothing they read, and it takes cmd_lock, which the upload
            // path holds, so done waited behind uploads.
            __atomic_store_n(&done, reached, __ATOMIC_RELEASE);
            plat_cond_broadcast(&cv_done);
            plat_mutex_unlock(&st_lock);
            vk_mem_collect();
            plat_mutex_lock(&st_lock);
        }
    }
    plat_mutex_unlock(&st_lock);
    return;
}

void vk_sync_start(void) {
    if (__atomic_load_n(&started, __ATOMIC_ACQUIRE)) return;
    plat_mutex_lock(&start_lock);
    if (!started) {
        if (!vk_init()) rt_fatal("vk: no usable Vulkan device (see the vk: lines above; NX2NAVI_GPU=null runs without a GPU)");
        VkCtx *c = vk_ctx();
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                      .queueFamilyIndex = c->gfx_family};
        if (vkCreateCommandPool(c->device, &pi, NULL, &cmd_pool) != VK_SUCCESS) rt_fatal("vk: submission command pool");
        if (!ts_state && perf_gpu_timing()) ts_setup(c);
        perf_gpu_stats = gpu_stats;
        uint64_t now = 0;
        vkGetSemaphoreCounterValue(c->device, c->timeline, &now);
        if (issued > now) {   // a new device after vk_shutdown: values keep increasing for the front end
            VkSemaphoreSignalInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = c->timeline,
                                        .value = issued};
            if (vkSignalSemaphore(c->device, &si) != VK_SUCCESS) rt_fatal("vk: timeline restart at %llu", (unsigned long long)issued);
            now = issued;
        }
        plat_mutex_lock(&st_lock);
        issued = done = now;
        if (reserved < now) reserved = now;
        stop = 0;
        plat_mutex_unlock(&st_lock);
        // the executed timeline starts where the timeline is; without it every exec wait is a fence wait
        VkSemaphoreTypeCreateInfo et = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = now};
        VkSemaphoreCreateInfo ei = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &et};
        if (vkCreateSemaphore(c->device, &ei, NULL, &exec_tl) != VK_SUCCESS) exec_tl = VK_NULL_HANDLE;
        exec_sig = now;
        vk_mem_note_submit(now);
        completion_thread = plat_thread_new(completion_main, c, "vk completion");
        if (!completion_thread) rt_fatal("vk: cannot start the completion thread");
        __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
        vk_astc_mode();   // decides NX2NAVI_ASTC and builds the ASTC decoder now, not at the first ASTC upload
    }
    plat_mutex_unlock(&start_lock);
}

// The index of a command buffer the GPU no longer uses, claimed for value `fence`. Called with sub_lock held; may move
// slots[].
static int claim_slot(VkCtx *c, uint64_t fence) {
    uint64_t gpu = 0;
    vkGetSemaphoreCounterValue(c->device, c->timeline, &gpu);
    for (int i = 0; i < nslots; i++)
        if (slots[i].fence <= gpu) {
            slots[i].fence = fence;
            return i;
        }
    CmdSlot *n = realloc(slots, sizeof *n * (size_t)(nslots + 1));
    if (!n) rt_fatal("vk: out of memory (command buffers)");
    slots = n;
    CmdSlot *s = &slots[nslots];
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cmd_pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (vkAllocateCommandBuffers(c->device, &ai, &s->cb) != VK_SUCCESS) rt_fatal("vk: command buffer allocation");
    s->fence = fence;
    return nslots++;
}

static CmdSlot *take_slot(VkCtx *c, uint64_t fence) {
    cur_slot = claim_slot(c, fence);
    return &slots[cur_slot];
}

// The copies the replay's first accesses to VRAM mirrors queued (vk_memory.c), in a command buffer of their own that is
// submitted right before the part that needs them and timed like a part; VK_NULL_HANDLE when there are none. After the
// part was recorded, sub_lock held; may move slots[].
// First brings the shadow storage up to date with the CPU's writes, which needs no command buffer.
static VkCommandBuffer prologue(VkCtx *c, uint64_t fence) {
    vk_mem_shadow_sync(fence);   // shadow storage: the CPU's writes go in before the part is submitted
    vk_mem_lazy_part_end();   // what the part left in the mirror and has to write back after all joins the snapshots
    if (!vk_mem_mirror_pending()) return VK_NULL_HANDLE;
    int i = claim_slot(c, fence);   // before reading slots: the claim may move it
    VkCommandBuffer cb = slots[i].cb;
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    int pair = ts_open(cb);
    vk_mem_mirror_prologue(cb);
    ts_close(cb, pair, fence, 1);
    vkEndCommandBuffer(cb);
    return cb;
}

// The command buffers of the part that ends now, in queue order, into cbi: the prologue (when there is one), the
// part's earlier pieces (vk_sync_append) and, with `with_last`, its last command buffer. Empties the list.
static uint32_t part_list(VkCommandBufferSubmitInfo *cbi, VkCommandBuffer pro, VkCommandBuffer last, int with_last) {
    uint32_t n = 0;
    VkCommandBufferSubmitInfo one = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    if (pro) one.commandBuffer = pro, cbi[n++] = one;
    for (int i = 0; i < npart_cbs; i++) {
        if (!part_cbs[i]) rt_fatal("vk: a replay worker's command buffer is missing from a part");
        one.commandBuffer = part_cbs[i], cbi[n++] = one;
    }
    if (with_last) one.commandBuffer = last, cbi[n++] = one;
    npart_cbs = 0;
    return n;
}

// The replay workers (vk_exec.c "replay workers"): the replay's command buffer being recorded ends and joins the
// part's list, then n places for the command buffers the workers record, in that order, and a new command buffer of
// the same part is begun and returned: the same timeline value, timed and submitted with the rest, so the workers'
// commands run between the replay's commands before and after. The places are filled (vk_sync_append_fill) before the
// part is submitted, which part_list checks. vk_sync_append_room: whether the list has room for n more.
int vk_sync_append_room(int n) { return npart_cbs + n + 1 <= PART_CBS; }

VkCommandBuffer vk_sync_append_later(int n, int *at) {
    if (!vk_sync_append_room(n)) rt_fatal("vk: a submission part of more than %d command buffers", PART_CBS);
    VkCtx *c = vk_ctx();
    VkCommandBuffer cb = slots[cur_slot].cb;
    if (!rec_end_cb(cb, 0, 0)) vkEndCommandBuffer(cb);   // every command of it is in cb, ended by its recorder
    part_cbs[npart_cbs++] = cb;
    *at = npart_cbs;
    for (int i = 0; i < n; i++) part_cbs[npart_cbs++] = VK_NULL_HANDLE;
    CmdSlot *s = take_slot(c, cur_fence);   // the ended one keeps its slot until the value is reached
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(s->cb, &bi);
    rec_begin(s->cb, cur_fence);
    return s->cb;
}

void vk_sync_append_fill(int at, const VkCommandBuffer *cbs, int n) {
    if (at < 0 || at + n > npart_cbs) rt_fatal("vk: replay worker command buffers past the part's list");
    for (int i = 0; i < n; i++) part_cbs[at + i] = cbs[i];
}

// The replay workers' command buffers: each worker has a command pool of its own (a pool belongs to one thread at a
// time) and claims command buffers from it for the submission being recorded, as the replay claims its slots. Called
// on the worker while the submitting thread holds sub_lock and records on (NX2NAVI_REPLAY_PIPE) or waits for it: the
// part with the worker's command buffer is not submitted before it is done, so cur_fence stays put.
#define SYNC_WORKERS 17                // the replay workers (at most 16) and the submitting thread
static struct { VkCommandPool pool; CmdSlot *slots; int n; } WP[SYNC_WORKERS];

VkCommandBuffer vk_sync_worker_begin(int w) {
    VkCtx *c = vk_ctx();
    if (w < 0 || w >= SYNC_WORKERS) rt_fatal("vk: replay worker %d of %d", w, SYNC_WORKERS);
    typeof(WP[0]) *p = &WP[w];
    if (!p->pool) {
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                      .queueFamilyIndex = c->gfx_family};
        if (vkCreateCommandPool(c->device, &pi, NULL, &p->pool) != VK_SUCCESS) rt_fatal("vk: replay worker command pool");
    }
    uint64_t gpu = 0;
    vkGetSemaphoreCounterValue(c->device, c->timeline, &gpu);
    CmdSlot *s = NULL;
    for (int i = 0; i < p->n && !s; i++)
        if (p->slots[i].fence <= gpu) s = &p->slots[i];
    if (!s) {
        CmdSlot *m = realloc(p->slots, sizeof *m * (size_t)(p->n + 1));
        if (!m) rt_fatal("vk: out of memory (command buffers)");
        p->slots = m, s = &m[p->n++];
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = p->pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        if (vkAllocateCommandBuffers(c->device, &ai, &s->cb) != VK_SUCCESS) rt_fatal("vk: command buffer allocation");
    }
    s->fence = cur_fence;
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(s->cb, &bi);
    return s->cb;
}

void vk_sync_worker_end(VkCommandBuffer cb) { vkEndCommandBuffer(cb); }

static VkCommandBuffer split_part(int exec);
VkCommandBuffer vk_sync_split(void) { return split_part(0); }
// The part that ends here is the last with the submission's own commands: it signals the executed timeline.
VkCommandBuffer vk_sync_split_exec(void) { return split_part(1); }

static VkCommandBuffer split_part(int exec) {
    VkCtx *c = vk_ctx();
    VkCommandBuffer cb = slots[cur_slot].cb;
    vk_gpu_passes_part(cb, 1);
    ts_end(cb, cur_fence);
    if (!rec_end_cb(cb, 0, 0)) vkEndCommandBuffer(cb);   // every command of the part is in cb, ended by its recorder
    vk_upload_flush(0);   // the uploads this part asked for precede it in queue order
    VkCommandBuffer pro = prologue(c, cur_fence);
    VkCommandBufferSubmitInfo cbi[PART_CBS + 2];
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = part_list(cbi, pro, cb, 1),
                        .pCommandBufferInfos = cbi};
    VkSemaphoreSubmitInfo esig = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = exec_tl,
                                  .value = cur_fence, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    if (exec && exec_tl && exec_sig < cur_fence)
        si.signalSemaphoreInfoCount = 1, si.pSignalSemaphoreInfos = &esig, exec_sig = cur_fence;
    ts_submitted();
    queue_submit(c, &si, 0);         // held or handed to the queue (submit batching)
    vk_mem_lazy_handed();
    vk_mem_note_submit(cur_fence);   // objects destroyed from here on may be in use by the part just submitted
    CmdSlot *s = take_slot(c, cur_fence);   // the submitted part keeps its slot until the value is reached
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(s->cb, &bi);
    vk_crumb(s->cb, VK_CRUMB_SUBMIT, cur_fence);
    ts_begin(s->cb);
    rec_begin(s->cb, cur_fence);
    vk_gpu_passes_part(s->cb, 0);
    return s->cb;
}

// Asynchronous replay (vk_backend.c): the value the next queued submission or present will signal. Values are handed
// out in queue order and the replay thread submits them in that order, so each vk_sync_submit gets exactly its value.
uint64_t vk_sync_reserve(void) {
    vk_sync_start();
    plat_mutex_lock(&st_lock);
    if (reserved < issued) reserved = issued;
    uint64_t v = ++reserved;
    plat_mutex_unlock(&st_lock);
    return v;
}

// own: NULL for the replay of handles; else the caller's commands in the replay's place (vk_sync_submit_own).
typedef struct {
    void (*record)(VkCommandBuffer, void *);
    void *arg;
} OwnCommands;

static uint64_t submit(const uint64_t *handles, int n, const OwnCommands *own) {
    vk_sync_start();
    // The run is ending (vk_device.c vk_device_lost_check): nothing is recorded or submitted and no value is issued.
    // The value returned is the last one issued; a wait for it sleeps until the process ends.
    if (vk_device_is_lost()) return __atomic_load_n(&issued, __ATOMIC_ACQUIRE);
    VkCtx *c = vk_ctx();
    plat_mutex_lock(&sub_lock);
    vk_mem_dirty_submit();   // NX2NAVI_TRACE=perf: what a mirror would upload before this submit
    uint64_t fence = __atomic_load_n(&issued, __ATOMIC_ACQUIRE) + 1;
    cur_fence = fence;
    CmdSlot *s = take_slot(c, fence);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    // NX2NAVI_REPLAY_TIME: the replaying thread's CPU time over the replay too (perf_replay_cpu), against its wall time
    static int cpu_on = -1;
    if (cpu_on < 0) cpu_on = cfg_bool("NX2NAVI_REPLAY_TIME");
    uint32_t me = plat_thread_id();
    uint64_t t0 = perf_begin(), cpu0 = cpu_on ? plat_thread_cpu_ns(me) : 0;
    vkBeginCommandBuffer(s->cb, &bi);
    vk_crumb(s->cb, VK_CRUMB_SUBMIT, fence);
    ts_begin(s->cb);
    rec_begin(s->cb, fence);
    vk_gpu_passes_part(s->cb, 0);
    int recorded = 1;
    if (own) own->record(s->cb, own->arg);
    else recorded = vk_exec_record(s->cb, handles, n, fence);
    s = &slots[cur_slot];   // the last part (vk_sync_split)
    vk_gpu_passes_part(s->cb, 1);
    if (recorded) ts_end(s->cb, fence);
    else ts_drop();
    // The recorder is done with the part: the end-of-submission crumb, then the end, by the recorder when it records.
    if (!rec_end_cb(s->cb, VK_CRUMB_SUBMIT_END, fence)) vkEndCommandBuffer(s->cb);
    perf_replay(t0);
    if (cpu0) perf_replay_cpu(plat_thread_cpu_ns(me) - cpu0);
    // After the recording: the uploads it asked for (and every earlier one) precede it in queue order.
    vk_upload_flush(0);
    VkCommandBuffer pro = prologue(c, fence);
    s = &slots[cur_slot];
    VkCommandBufferSubmitInfo cbi[PART_CBS + 2];
    uint32_t ncbi = part_list(cbi, pro, s->cb, recorded);
    // the timeline, and the executed timeline unless a part of this submission signalled it already (split_part)
    VkSemaphoreSubmitInfo sig[2] = {{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = c->timeline,
                                     .value = fence, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                                    {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = exec_tl,
                                     .value = fence, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    int both = exec_tl && exec_sig < fence;
    if (both) exec_sig = fence;
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = ncbi,
                        .pCommandBufferInfos = cbi, .signalSemaphoreInfoCount = both ? 2u : 1u,
                        .pSignalSemaphoreInfos = sig};
    ts_submitted();
    queue_submit(c, &si, fence);   // held or handed to the queue (submit batching)
    vk_mem_lazy_handed();
    s->fence = recorded ? fence : 0;
    vk_mem_note_submit(fence);
    plat_mutex_lock(&st_lock);
    __atomic_store_n(&issued, fence, __ATOMIC_RELEASE);
    plat_cond_broadcast(&cv_work);
    plat_mutex_unlock(&st_lock);
    static unsigned subs;
    if (++subs % 4096 == 0) {
        rec_stats_log();
        if (subs % (4096 * 8) == 4096)   // statistics only: read without the queue lock
            rt_log("vk: submit batching: %llu submissions in %llu queue calls (%.2f each)", (unsigned long long)H.subs,
                   (unsigned long long)H.batches, H.batches ? (double)H.subs / (double)H.batches : 0.0);
    }
    plat_mutex_unlock(&sub_lock);
    return fence;
}

uint64_t vk_sync_submit(const uint64_t *handles, int n) { return submit(handles, n, NULL); }

// The upload batch is no place for a probe's commands: it is submitted before the memory prologue.
uint64_t vk_sync_submit_own(void (*record)(VkCommandBuffer, void *), void *arg) {
    OwnCommands own = {record, arg};
    return submit(NULL, 0, &own);
}

// st_lock held: p at the end of the queue *q.
static void pending_push(Pending **q, int *head, int *n, int *cap, Pending p) {
    if (*head + *n == *cap) {
        if (*head) {
            memmove(*q, *q + *head, sizeof **q * (size_t)*n);
            *head = 0;
        } else {
            int c = *cap ? *cap * 2 : 256;
            Pending *m = realloc(*q, sizeof *m * (size_t)c);
            if (!m) rt_fatal("vk: out of memory (completion queue)");
            *q = m, *cap = c;
        }
    }
    (*q)[*head + (*n)++] = p;
}

void vk_completion_add(uint64_t fence, void (*fn)(void *), void *arg) {
    plat_mutex_lock(&st_lock);
    pending_push(&queue, &qhead, &qn, &qcap, (Pending){fence, fn, arg});
    // The completion thread sleeps only while nothing is issued beyond done: a callback for a later value (the replay's,
    // for the submission it records: counter reports, freed resources) becomes due through the submit, which wakes it.
    if (fence <= done) plat_cond_broadcast(&cv_work);
    plat_mutex_unlock(&st_lock);
}

// A side effect the game's CPU reads once the GPU has run submission fence's own commands: it runs before the other
// side effects of the values reached, and a wait after a submit runs it itself (vk_sync_exec_wait).
void vk_completion_add_result(uint64_t fence, void (*fn)(void *), void *arg) {
    plat_mutex_lock(&st_lock);
    pending_push(&rqueue, &rqhead, &rqn, &rqcap, (Pending){fence, fn, arg});
    if (fence <= done) plat_cond_broadcast(&cv_work);
    plat_mutex_unlock(&st_lock);
}

// Runs the results queued for the values up to upto, which the GPU has passed: on the completion thread for each
// value it reaches, and on a guest thread whose wait on exec_tl has just returned. st_lock not held.
static void results_run(uint64_t upto) {
    plat_mutex_lock(&res_lock);
    plat_mutex_lock(&st_lock);
    while (rqn && rqueue[rqhead].fence <= upto && !vk_device_is_lost()) {
        Pending p = rqueue[rqhead++];
        rqn--;
        plat_mutex_unlock(&st_lock);
        __atomic_store_n(&cb_late, p.fence <= __atomic_load_n(&exw_value, __ATOMIC_ACQUIRE), __ATOMIC_RELAXED);
        p.fn(p.arg);
        __atomic_store_n(&cb_late, 0, __ATOMIC_RELAXED);
        plat_mutex_lock(&st_lock);
    }
    plat_mutex_unlock(&st_lock);
    plat_mutex_unlock(&res_lock);
}

static int fence_wait_impl(uint64_t value, uint64_t timeout_ns);

int vk_sync_fence_wait(uint64_t value, uint64_t timeout_ns) {
    uint64_t p0 = perf_begin();
    int ok = fence_wait_impl(value, timeout_ns);
    perf_end(PERF_FENCE, p0);
    perf_gpu_wait(p0);
    return ok;
}

static int fence_wait_impl(uint64_t value, uint64_t timeout_ns) {
    vk_sync_start();
    hold_release(value, 0, 0);   // a held submission the waiter needs goes to the queue now
    plat_mutex_lock(&st_lock);
    int ok;
    if (value > issued && value > reserved) {
        ok = 0;   // never submitted nor queued: never reached (as in the null back end)
    } else {
        uint64_t t0 = kern_now_ns();
        while (done < value) {
            if (timeout_ns < (1ull << 62)) {
                uint64_t el = kern_now_ns() - t0;
                if (el >= timeout_ns) break;
                plat_cond_wait_ns(&cv_done, &st_lock, timeout_ns - el);
            } else {
                plat_cond_wait(&cv_done, &st_lock);
            }
        }
        ok = done >= value;
    }
    plat_mutex_unlock(&st_lock);
    return ok;
}

// The wait after a submit: the fence's (vk_sync_fence_wait), or with NX2NAVI_WB_SPLIT until the GPU has executed
// submission value's own commands (the executed timeline) and the results the game reads are in its memory
// (results_run), its write-backs and the completion thread's other side effects for it and for the submissions
// before it still to come. 1 when reached.
int vk_sync_exec_wait(uint64_t value, uint64_t timeout_ns) {
    vk_sync_start();
    // The fence's wait unless the split is asked for ("the executed timeline" above says what the game reads that a
    // wait on exec_tl leaves out). With shadow storage (vk_mem_shadow_storage) what the GPU wrote reaches guest memory
    // in such a callback too: a wait that returned before it let the game rewrite a pool that an older write-back
    // then overwrote (Kokiri Forest: a sky coefficient the game had reset came back, and the world flickered washed
    // out).
    // The run is ending (vk_device.c vk_device_lost_check): no wait on the lost device, and none for a value that
    // will not be reached.
    if (vk_device_is_lost()) return __atomic_load_n(&done, __ATOMIC_ACQUIRE) >= value;
    if (!vk_sync_wb_split()) return vk_sync_fence_wait(value, timeout_ns);
    if (__atomic_load_n(&done, __ATOMIC_ACQUIRE) >= value) return 1;
    uint64_t p0 = perf_begin();
    hold_release(value, 0, 0);   // a held submission the waiter needs goes to the queue now
    VkCtx *c = vk_ctx();
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                              .pSemaphores = &exec_tl, .pValues = &value};
    VkResult r = vkWaitSemaphores(c->device, &wi, timeout_ns);
    if (r != VK_SUCCESS && r != VK_TIMEOUT) {
        vk_device_lost_check(r, "the wait for a submission's commands");
        rt_fatal("vk: the wait for a submission's commands failed (%d; device lost?)", (int)r);
    }
    // The results the game reads, of this submission and of those before it, which the GPU ran first: here, not when
    // the completion thread gets to them behind the other side effects of the last frame's submissions.
    if (r == VK_SUCCESS && wb_results) results_run(value);
    perf_end(PERF_FENCE, p0);
    perf_gpu_wait(p0);
    if (r == VK_SUCCESS) {
        __atomic_fetch_add(&exw_n, 1, __ATOMIC_RELAXED);
        if (__atomic_load_n(&done, __ATOMIC_ACQUIRE) < value) {
            __atomic_fetch_add(&exw_early, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&exw_t0, kern_now_ns(), __ATOMIC_RELAXED);
            __atomic_store_n(&exw_value, value, __ATOMIC_RELEASE);
        }
    }
    return r == VK_SUCCESS;
}

uint64_t vk_sync_fence_done(void) { return __atomic_load_n(&done, __ATOMIC_ACQUIRE); }

uint64_t vk_sync_issued(void) { return __atomic_load_n(&issued, __ATOMIC_ACQUIRE); }

void vk_sync_finish(void) {
    vk_sync_start();
    vk_sync_fence_wait(__atomic_load_n(&issued, __ATOMIC_ACQUIRE), UINT64_MAX);
    vk_upload_flush(1);
}

void vk_sync_shutdown(void) {
    plat_mutex_lock(&start_lock);
    if (started) {
        vk_sync_finish();
        plat_mutex_lock(&st_lock);
        stop = 1;
        plat_cond_broadcast(&cv_work);
        plat_mutex_unlock(&st_lock);
        plat_thread_join(completion_thread);
        completion_thread = NULL;
        plat_mutex_lock(&st_lock);
        qhead = qn = rqhead = rqn = 0;   // callbacks of values never submitted
        plat_mutex_unlock(&st_lock);
        plat_mutex_lock(&sub_lock);
        VkCtx *c = vk_ctx();
        if (c && cmd_pool) vkDestroyCommandPool(c->device, cmd_pool, NULL);
        for (int w = 0; w < SYNC_WORKERS; w++) {
            if (c && WP[w].pool) vkDestroyCommandPool(c->device, WP[w].pool, NULL);
            free(WP[w].slots);
            memset(&WP[w], 0, sizeof WP[w]);
        }
        if (c && exec_tl) vkDestroySemaphore(c->device, exec_tl, NULL);
        exec_tl = VK_NULL_HANDLE;
        perf_gpu_stats = NULL;
        if (c && ts_pool) vkDestroyQueryPool(c->device, ts_pool, NULL);
        ts_pool = VK_NULL_HANDLE;
        ts_state = 0;
        ts_last_end = 0;
        memset((void *)ts_busy, 0, sizeof ts_busy);
        cmd_pool = VK_NULL_HANDLE;
        free(slots);
        slots = NULL;
        nslots = 0;
        plat_mutex_unlock(&sub_lock);
        __atomic_store_n(&started, 0, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&start_lock);
}

// ---- --probe-devlost (host.h): the end of a run whose device is lost, without a lost device and without a window.
// The device itself stays healthy, so what the driver answers after a real loss is not covered here.
// 1. A callback queued for the next value runs when the timeline reaches it (the check that step 2 can fail).
// 2. With vk_lost set, as after another thread saw the loss: a callback queued for the next value does not run although
//    the timeline reaches it, done stays, and a submit, an upload flush and a wait for a submission's commands return
//    at once without issuing a value.
// 3. With vk_lost clear again, four threads report a lost device at the same moment: the process must end with
//    RT_LOST_EXIT, one report and one FATAL line in the output (tests/device_lost_check.py counts them). Reaching the
//    end of this function is the failure.
static volatile int32_t probe_ran[2];
static void probe_lost_cb(void *arg) { plat_xchg32((volatile int32_t *)arg, 1); }
static void probe_lost_record(VkCommandBuffer cb, void *arg) { (void)cb; plat_xchg32((volatile int32_t *)arg, 1); }
static volatile int32_t probe_go;
static void probe_lost_thread(void *arg) {
    static const char *const where[4] = {"probe thread 0", "probe thread 1", "probe thread 2", "probe thread 3"};
    while (!__atomic_load_n(&probe_go, __ATOMIC_ACQUIRE)) {}
    vk_device_lost_check(VK_ERROR_DEVICE_LOST, where[(intptr_t)arg & 3]);
}

// Makes the timeline reach the next value as a finished submission would: issued moves on, the semaphore is signalled
// from the host, and the completion thread is woken. Returns the value.
static uint64_t probe_lost_advance(VkCtx *c, volatile int32_t *ran) {
    plat_mutex_lock(&st_lock);
    uint64_t v = issued + 1;
    plat_mutex_unlock(&st_lock);
    vk_completion_add(v, probe_lost_cb, (void *)ran);
    VkSemaphoreSignalInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = c->timeline, .value = v};
    if (vkSignalSemaphore(c->device, &si) != VK_SUCCESS) return 0;
    plat_mutex_lock(&st_lock);
    __atomic_store_n(&issued, v, __ATOMIC_RELEASE);
    plat_cond_broadcast(&cv_work);
    plat_mutex_unlock(&st_lock);
    return v;
}

int vk_sync_probe_devlost(void) {
    if (!vk_init()) {
        printf("probe-devlost: SKIP (no Vulkan device on this machine)\n");
        return 0;
    }
    vk_sync_start();
    VkCtx *c = vk_ctx();
    uint64_t v1 = probe_lost_advance(c, &probe_ran[0]);
    for (int i = 0; i < 200 && !plat_cas32(&probe_ran[0], 1, 1); i++) plat_sleep_ms(10);
    if (!v1 || !plat_cas32(&probe_ran[0], 1, 1) || !vk_sync_fence_wait(v1, 2000000000ull)) {
        printf("probe-devlost: FAILED: a callback did not run on a healthy device (value %llu)\n", (unsigned long long)v1);
        return 1;
    }
    printf("probe-devlost: healthy device: the callback of value %llu ran\n", (unsigned long long)v1);

    plat_xchg32(&vk_lost, 1);
    uint64_t v2 = probe_lost_advance(c, &probe_ran[1]);
    plat_sleep_ms(500);   // five of the completion thread's 100 ms waits
    int ran = plat_cas32(&probe_ran[1], 1, 1), reached = vk_sync_fence_done() >= v2;
    volatile int32_t recorded = 0;
    uint64_t before = vk_sync_issued(), t0 = plat_now_ms();
    uint64_t got = vk_sync_submit_own(probe_lost_record, (void *)&recorded);
    vk_upload_flush(1);
    int waited = vk_sync_exec_wait(v2, 5000000000ull);
    uint64_t ms = plat_now_ms() - t0;
    int ok = v2 && !ran && !reached && !recorded && got == before && vk_sync_issued() == before && !waited && ms < 1000;
    printf("probe-devlost: device marked lost: callback of value %llu %s, done %s, submit %s (issued %llu -> %llu), "
           "the paths returned in %llu ms\n", (unsigned long long)v2, ran ? "RAN" : "not run", reached ? "ADVANCED" : "stayed",
           recorded ? "RECORDED" : "not recorded", (unsigned long long)before, (unsigned long long)vk_sync_issued(),
           (unsigned long long)ms);
    if (!ok) { printf("probe-devlost: FAILED\n"); return 1; }

    printf("probe-devlost: four threads report the loss now; the process must end with exit code %d\n", RT_LOST_EXIT);
    fflush(stdout);
    plat_xchg32(&vk_lost, 0);
    for (intptr_t i = 0; i < 4; i++) plat_thread_spawn(probe_lost_thread, (void *)i, "probe lost", 0);
    plat_sleep_ms(50);
    __atomic_store_n(&probe_go, 1, __ATOMIC_RELEASE);
    plat_sleep_ms(20000);
    printf("probe-devlost: FAILED: still running 20 s after the loss was reported\n");
    return 1;
}

// NX2NAVI_TRACE=perf "mem: sizes" (perf.c): command buffer slots (read without sub_lock, which a whole replay holds),
// the completion queue and the submissions held back.
void vk_sync_sizes(PerfSizes *s) {
    uint64_t slots = (uint64_t)__atomic_load_n(&nslots, __ATOMIC_RELAXED);
    plat_mutex_lock(&st_lock);
    uint64_t q = (uint64_t)qn;
    plat_mutex_unlock(&st_lock);
    perf_size(s, "command buffer slots", slots, 0, PERF_SIZE_NONE);
    perf_size(s, "completion queue", q, 0, PERF_SIZE_NONE);
    perf_size(s, "held submissions", (uint64_t)__atomic_load_n(&held_n, __ATOMIC_RELAXED), 0, PERF_SIZE_NONE);
}
