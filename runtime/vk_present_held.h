// A present that waits for held work (vk_backend.c "a present that waits for held work"): its state and who may put it
// out. Header only, so that tests/present_held_check.c checks it without a device.
//
// With the replay on the submitting thread the game presents from one guest thread while others submit. A present
// held for event-ordered work goes out after the submission that replays the last of that work, on whichever thread
// made it, at the next present, or after PH_MAX submissions; only the presenting thread arms one. The lock keeps the
// held job whole and lets one thread put it out once. It also keeps the synchronous presents one at a time and in the
// game's order: the present thread takes one picture in waiting (vk_backend.c present_image), so a held present another
// thread puts out has to reach it before the next present does. A submitting thread only tries the lock: when another
// thread holds it, that thread is putting a present out or arming one, and the submitting thread goes on without
// waiting; its next submission, the next present or PH_MAX checks again. The presenting thread waits for the lock
// (ph_lock, counted) only while another thread puts a held present out: at most that one present_image.
#pragma once
#include <stdint.h>

#include "platform/platform.h"

typedef struct {
    const void *t;                          // the game's texture (tex_t)
    int crop[4], has_crop;
    uint64_t at_ns, queued, frame, since;   // since: the replay's submission count when it began to wait
    uint32_t tid;                           // the thread that presented it (for the counts of who put it out)
} PhJob;

enum { PH_AFTER, PH_CAPPED, PH_NEXT, PH_WHY_N };   // why a held present went out: work replayed, PH_MAX, next present
#define PH_NONE (-1)                                // nothing goes out; for the present callback, one that did not wait

typedef struct {
    PlatMutex lock;                    // job, on, waited and out; the order of the synchronous presents
    int32_t on;                        // a present waits: changed under lock, read without it as a hint (atomic)
    PhJob job;
    uint64_t waited, out[PH_WHY_N];    // presents that waited; held presents put out, by why
    // The presenting thread's waits for the lock: how many, their sum and the longest, in ns. Written under the lock,
    // read anywhere (atomics). on_wait (may be NULL) is told when such a wait began (plat_now_ns), once it has ended.
    uint64_t lock_waits, lock_wait_ns, lock_wait_max;
    void (*on_wait)(uint64_t t0);
} PhState;

// Puts a present out (vk_backend.c present_image), with the lock held. why: PH_AFTER, PH_CAPPED or PH_NEXT for a held
// one, PH_NONE for a present that did not wait.
typedef void (*PhPresent)(const PhJob *j, int why, void *arg);
// Whether a held present goes out after a submission, asked with the lock held: PH_AFTER, PH_CAPPED or PH_NONE.
typedef int (*PhWhy)(const PhJob *j, void *arg);

static inline int ph_armed(const PhState *s) { return __atomic_load_n(&s->on, __ATOMIC_ACQUIRE); }

// The presenting thread's lock: at once when it is free, else a counted wait.
static inline void ph_lock(PhState *s) {
    if (plat_mutex_trylock(&s->lock)) return;
    uint64_t t0 = plat_now_ns();
    plat_mutex_lock(&s->lock);
    uint64_t d = plat_now_ns() - t0;
    __atomic_add_fetch(&s->lock_waits, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s->lock_wait_ns, d, __ATOMIC_RELAXED);
    if (d > s->lock_wait_max) __atomic_store_n(&s->lock_wait_max, d, __ATOMIC_RELAXED);
    if (s->on_wait) s->on_wait(t0);
}

// A submitting thread, before or after its submission. Never waits; 1 when it put the held present out.
static inline int ph_flush_try(PhState *s, PhWhy why_now, PhPresent present, void *arg) {
    if (!ph_armed(s) || !plat_mutex_trylock(&s->lock)) return 0;
    int why = s->on ? why_now(&s->job, arg) : PH_NONE;
    if (why != PH_NONE) {
        __atomic_store_n(&s->on, 0, __ATOMIC_RELAXED);
        s->out[why]++;
        present(&s->job, why, arg);
    }
    plat_mutex_unlock(&s->lock);
    return why != PH_NONE;
}

// The presenting thread at its next present: a held present goes out now. Waits while another thread puts it out.
static inline int ph_flush_next(PhState *s, PhPresent present, void *arg) {
    if (!ph_armed(s)) return 0;
    ph_lock(s);
    int out = s->on;
    if (out) {
        __atomic_store_n(&s->on, 0, __ATOMIC_RELAXED);
        s->out[PH_NEXT]++;
        present(&s->job, PH_NEXT, arg);
    }
    plat_mutex_unlock(&s->lock);
    return out;
}

// The presenting thread: j waits for held work (hold_now(arg), asked with the lock held) or goes out now, in either case
// behind a held present another thread is putting out.
static inline void ph_present(PhState *s, const PhJob *j, int (*hold_now)(void *arg), PhPresent present, void *arg) {
    ph_lock(s);
    if (s->on) {   // only a presenting thread arms: a second presenting thread's held present goes first
        __atomic_store_n(&s->on, 0, __ATOMIC_RELAXED);
        s->out[PH_NEXT]++;
        present(&s->job, PH_NEXT, arg);
    }
    if (hold_now && hold_now(arg)) {
        s->job = *j;
        s->waited++;
        __atomic_store_n(&s->on, 1, __ATOMIC_RELEASE);
    } else {
        present(j, PH_NONE, arg);
    }
    plat_mutex_unlock(&s->lock);
}
