// A frame's bookkeeping at its present (vk_backend.c frame_end: the replay's per-frame state that a present closes), in
// replay order. Header only, so that tests/frame_owed_check.c checks it without a device.
//
// The replay reads and writes that state as it records, under the submission lock (vk_sync.c sub_lock). With the
// replay on the submitting threads the game presents on one thread while another may be replaying, and running the
// bookkeeping then reset the state in the middle of that replay. The presenting thread does not wait for the replay:
// it marks its frame owed and runs the bookkeeping itself only when it gets the lock at once; otherwise the next thread
// to take the lock (vk_sync.c submit) runs every owed frame's bookkeeping, in frame order, before its own replay. A frame
// caught up behind a later present runs as two presents with no submission between them always did.
//
// The present thread's blit of a frame reads what that frame's bookkeeping closed (vk_dlaa.c vk_dlaa_frame: the
// inputs frame generation must not get across an upscaler or frame generation switch), so the bookkeeping must come
// before it, as it always did when it ran at the present. A frame left owed may still be owed when the present thread
// takes it: fo_close runs what is owed then, under the lock, waiting for a replay in progress as the present thread's
// own blit submission does a moment later. The present thread is not one of the game's threads.
#pragma once
#include <stdint.h>

typedef struct {
    uint64_t owed;   // the newest presented frame (atomic: the presenting thread raises it, the lock's holder reads it)
    uint64_t done;   // the newest frame whose bookkeeping ran (the lock's holder writes it; atomic for fo_ran)
} FrameOwed;

typedef void (*FrameEnd)(uint64_t frame, void *arg);

// The lock's holder: whether a frame waits for its bookkeeping (one atomic load, for every submission).
static inline int fo_waiting(FrameOwed *o) {
    return __atomic_load_n(&o->owed, __ATOMIC_ACQUIRE) != __atomic_load_n(&o->done, __ATOMIC_RELAXED);
}

// The lock's holder: the bookkeeping of every frame presented since the last run, in frame order; returns how many.
// done moves past a frame only once its bookkeeping returned (fo_ran reads it without the lock).
static inline uint64_t fo_run(FrameOwed *o, FrameEnd end, void *arg) {
    uint64_t owed = __atomic_load_n(&o->owed, __ATOMIC_ACQUIRE), d = __atomic_load_n(&o->done, __ATOMIC_RELAXED), n = 0;
    while (d < owed) end(++d, arg), __atomic_store_n(&o->done, d, __ATOMIC_RELEASE), n++;
    return n;
}

// Any thread: whether frame's bookkeeping ran; when it did, what it wrote is visible to the caller (the release of done
// in fo_run).
static inline int fo_ran(FrameOwed *o, uint64_t frame) { return __atomic_load_n(&o->done, __ATOMIC_ACQUIRE) >= frame; }

// The presenting thread, frame numbers rising (a smaller one than owed leaves it: that frame's bookkeeping ran or is
// owed already). try_run(arg) takes the lock without waiting, runs fo_run under it and returns 1, or returns 0 when
// another thread holds it. Returns whether the bookkeeping ran here.
static inline int fo_present(FrameOwed *o, uint64_t frame, int (*try_run)(void *), void *arg) {
    uint64_t was = __atomic_load_n(&o->owed, __ATOMIC_RELAXED);
    while (was < frame && !__atomic_compare_exchange_n(&o->owed, &was, frame, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {}
    return try_run(arg);
}

// The present thread, before it uses a presented frame (its blit): when the frame's bookkeeping has not run, run_locked
// (arg) takes the lock, waiting for it, and runs fo_run under it. Returns whether it had to; afterwards fo_ran(frame).
static inline int fo_close(FrameOwed *o, uint64_t frame, void (*run_locked)(void *), void *arg) {
    if (fo_ran(o, frame)) return 0;
    run_locked(arg);
    return 1;
}
