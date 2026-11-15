// tests/frame_owed_check.py builds this: runtime/vk_frame_owed.h, a frame's bookkeeping in replay order, with one
// presenting thread, three replaying threads that hold the lock for their replays, a few of them for HOLD_MS, and a
// present thread that takes each presented frame from the presenting thread (one in waiting at most, as vk_backend.c
// present_image hands it over), blits it and then submits under the lock (vk_backend.c present_main). Every
// frame's bookkeeping must run once and in frame order, never during a replay, before the body of any replay that took
// the lock after the frame was presented, and before the present thread's blit of the frame; the presenting thread
// must never wait for a replay. "frame_owed_check old" leaves out the present thread's fo_close, as the branch first
// had it, to show that the blit check sees a blit before its frame's bookkeeping (it then fails).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"
#include "vk_frame_owed.h"

#define FRAMES 20000
#define REPLAYERS 3
#define HOLD_MS 40
#define PRESENT_MAX_NS 20000000ull   // a present that waited for a long hold would take up to HOLD_MS

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("frame_owed_check: FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static PlatMutex sub_lock = PLAT_MUTEX_INIT;
static FrameOwed FO;
static volatile int32_t in_replay, in_long, stop;
static volatile uint64_t presented, ends, bad_order, during_replay, late, ran_here, long_holds, during_long;
static uint64_t last_end, present_max_ns;
static uint64_t ended;   // the newest frame whose bookkeeping returned, kept apart from FO.done (atomic)
static uint64_t slot;    // the frame handed to the present thread << 1 | left owed (PresentJob.owed); 0: none (atomic)
static uint64_t blits, blit_early, closed_here;   // the present thread's
static int old_mode;

// vk_backend.c frame_end's stand-in, run with the lock held.
static void frame_end(uint64_t frame, void *arg) {
    (void)arg;
    if (__atomic_load_n(&in_replay, __ATOMIC_ACQUIRE)) __atomic_add_fetch(&during_replay, 1, __ATOMIC_RELAXED);
    if (frame != last_end + 1) __atomic_add_fetch(&bad_order, 1, __ATOMIC_RELAXED);
    last_end = frame;
    __atomic_add_fetch(&ends, 1, __ATOMIC_RELAXED);
    for (volatile int i = 0; i < 300; i++) {}
    __atomic_store_n(&ended, frame, __ATOMIC_RELEASE);
}

// vk_sync.c vk_sync_try_locked with vk_backend.c fo_here
static int try_run(void *arg) {
    (void)arg;
    if (!plat_mutex_trylock(&sub_lock)) return 0;
    fo_run(&FO, frame_end, NULL);
    plat_mutex_unlock(&sub_lock);
    return 1;
}

// vk_sync.c submit: the lock, the owed frames (vk_present_frames_owed), then the replay's body
static void replayer(void *arg) {
    uint64_t r = (uint64_t)(uintptr_t)arg * 0x2545F4914F6CDD1Dull + 3;
    while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
        r ^= r << 13, r ^= r >> 7, r ^= r << 17;
        uint64_t p0 = __atomic_load_n(&presented, __ATOMIC_ACQUIRE);
        plat_mutex_lock(&sub_lock);
        if (fo_waiting(&FO)) fo_run(&FO, frame_end, NULL);
        if (__atomic_load_n(&FO.done, __ATOMIC_RELAXED) < p0) __atomic_add_fetch(&late, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&in_replay, 1, __ATOMIC_RELEASE);
        if (r % 997 == 0) {
            __atomic_store_n(&in_long, 1, __ATOMIC_RELEASE);
            plat_sleep_ms(HOLD_MS);
            __atomic_store_n(&in_long, 0, __ATOMIC_RELEASE);
            __atomic_add_fetch(&long_holds, 1, __ATOMIC_RELAXED);
        } else {
            for (volatile uint32_t i = 0; i < r % 20000; i++) {}
        }
        __atomic_store_n(&in_replay, 0, __ATOMIC_RELEASE);
        plat_mutex_unlock(&sub_lock);
        for (volatile uint32_t i = 0; i < r % 5000; i++) {}
    }
}

// vk_backend.c fo_blit_locked: the lock, waiting for it, and what is owed
static void run_locked(void *arg) {
    (void)arg;
    plat_mutex_lock(&sub_lock);
    fo_run(&FO, frame_end, NULL);
    plat_mutex_unlock(&sub_lock);
}

// vk_backend.c present_main: each frame handed over, its bookkeeping made sure of when it was left owed
// (present_frame_closed), the blit, which must see that frame's bookkeeping done, then the blit's submission (vk_sync.c
// submit).
static void present_thread(void *arg) {
    (void)arg;
    for (;;) {
        int stopping = __atomic_load_n(&stop, __ATOMIC_ACQUIRE);
        uint64_t j = __atomic_exchange_n(&slot, 0, __ATOMIC_ACQ_REL);
        if (!j) {
            if (stopping) break;
            plat_yield();
            continue;
        }
        uint64_t frame = j >> 1;
        if ((j & 1) && !old_mode && fo_close(&FO, frame, run_locked, NULL)) closed_here++;
        blits++;
        if (__atomic_load_n(&ended, __ATOMIC_ACQUIRE) < frame) blit_early++;
        for (volatile int i = 0; i < 2000; i++) {}   // the blit's recording
        plat_mutex_lock(&sub_lock);
        if (fo_waiting(&FO)) fo_run(&FO, frame_end, NULL);
        __atomic_store_n(&in_replay, 1, __ATOMIC_RELEASE);
        for (volatile int i = 0; i < 500; i++) {}
        __atomic_store_n(&in_replay, 0, __ATOMIC_RELEASE);
        plat_mutex_unlock(&sub_lock);
    }
}

int main(int argc, char **argv) {
    old_mode = argc > 1 && !strcmp(argv[1], "old");
    // one thread first: the present runs its own frame's bookkeeping when the lock is free, and two presents with the
    // lock held elsewhere leave both frames to the next holder, which runs them in order
    int here = fo_present(&FO, 1, try_run, NULL);
    CHECK(here && ends == 1 && FO.done == 1, "a present with the lock free did not run its own bookkeeping");
    plat_mutex_lock(&sub_lock);
    int a = fo_present(&FO, 2, try_run, NULL), b = fo_present(&FO, 3, try_run, NULL);
    CHECK(!a && !b && ends == 1 && fo_waiting(&FO), "presents past a held lock ran or lost their bookkeeping");
    CHECK(fo_run(&FO, frame_end, NULL) == 2 && ends == 3 && !bad_order && !fo_waiting(&FO),
          "the next holder did not catch up both frames in order");
    CHECK(fo_present(&FO, 2, try_run, NULL) == 0 && ends == 3 && !fo_waiting(&FO), "an older frame number ran again");
    CHECK(fo_ran(&FO, 3) && !fo_ran(&FO, 4), "fo_ran does not follow the bookkeeping that ran");
    CHECK(fo_close(&FO, 3, run_locked, NULL) == 0, "fo_close took the lock for a frame whose bookkeeping ran");
    plat_mutex_unlock(&sub_lock);
    CHECK(fo_present(&FO, 4, try_run, NULL) == 1 && fo_close(&FO, 4, run_locked, NULL) == 0 && ends == 4,
          "a frame whose present ran its bookkeeping was not seen as closed");
    plat_mutex_lock(&sub_lock);
    CHECK(fo_present(&FO, 5, try_run, NULL) == 0 && !fo_ran(&FO, 5), "a frame left owed was seen as closed");
    plat_mutex_unlock(&sub_lock);
    CHECK(fo_close(&FO, 5, run_locked, NULL) == 1 && fo_ran(&FO, 5) && ends == 5 && !bad_order,
          "fo_close did not run the owed frame's bookkeeping");
    FO.owed = FO.done = 3, last_end = 3, ends = 0, __atomic_store_n(&ended, 3, __ATOMIC_RELAXED);

    PlatThread *t[REPLAYERS], *pt = plat_thread_new(present_thread, NULL, "present");
    for (int i = 0; i < REPLAYERS; i++) t[i] = plat_thread_new(replayer, (void *)(uintptr_t)(i + 1), "replay");
    for (uint64_t f = 4; f < 4 + FRAMES; f++) {
        int lng = __atomic_load_n(&in_long, __ATOMIC_ACQUIRE);
        uint64_t t0 = plat_now_ns();
        int here = fo_present(&FO, f, try_run, NULL);
        ran_here += here;
        uint64_t d = plat_now_ns() - t0;
        if (d > present_max_ns) present_max_ns = d;
        if (lng) during_long++;
        __atomic_store_n(&presented, f, __ATOMIC_RELEASE);
        // the hand-off: the present thread took the frame before (vk_backend.c present_image's wait while pending)
        while (__atomic_load_n(&slot, __ATOMIC_ACQUIRE)) plat_yield();
        __atomic_store_n(&slot, f << 1 | (uint64_t)!here, __ATOMIC_RELEASE);
        for (volatile int i = 0; i < 20000; i++) {}
    }
    while (__atomic_load_n(&slot, __ATOMIC_ACQUIRE)) plat_yield();
    __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < REPLAYERS; i++) plat_thread_join(t[i]);
    plat_thread_join(pt);
    plat_mutex_lock(&sub_lock);
    fo_run(&FO, frame_end, NULL);
    plat_mutex_unlock(&sub_lock);
    CHECK(ends == FRAMES, "%llu frame ends for %d frames", (unsigned long long)ends, FRAMES);
    CHECK(!bad_order, "%llu frame ends out of order", (unsigned long long)bad_order);
    CHECK(!during_replay, "%llu frame ends ran during a replay", (unsigned long long)during_replay);
    CHECK(!late, "%llu replays began before the bookkeeping of a frame presented earlier", (unsigned long long)late);
    CHECK(present_max_ns < PRESENT_MAX_NS, "a present took %.2f ms", present_max_ns / 1e6);
    CHECK(ran_here < FRAMES, "no present found the lock held, so nothing was owed");
    CHECK(during_long, "no present came during a long hold");
    CHECK(blits == FRAMES, "the present thread blitted %llu of %d frames", (unsigned long long)blits, FRAMES);
    CHECK(!blit_early, "%llu blits came before their frame's bookkeeping", (unsigned long long)blit_early);
    printf("frame_owed_check%s: %d frames: %llu ends ran at the present, %llu owed to a replay, %llu of those caught up "
           "by the present thread before their blits; %llu replays held the lock %d ms, %llu presents came during one; the "
           "longest present %.3f ms; %llu blits before their frame's bookkeeping; %s\n", old_mode ? " old" : "", FRAMES,
           (unsigned long long)ran_here, (unsigned long long)(FRAMES - ran_here), (unsigned long long)closed_here,
           (unsigned long long)long_holds, HOLD_MS, (unsigned long long)during_long, present_max_ns / 1e6,
           (unsigned long long)blit_early, fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
