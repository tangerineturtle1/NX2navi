// A copy of the first held submission's wait of each event-ordered queue (vk_exec.c "event order"), for the readers
// that do not hold the submission lock. Header only, so that tests/evo_peek_check.c checks it without a device.
//
// vk_backend.c held_resume and submit_sync ask vk_exec.c vk_exec_held_ready whether held work can go on, without the
// submission lock. Following the queue's first continuation (vk_exec.c Cont) there read memory another submitting
// thread may free meanwhile (vk_exec.c evo_resume, cont_free): a continuation's wait lies in its private copy of the
// recording, and an event's address read from a reused block points anywhere. So the holder of the lock publishes
// what the decision needs (whether work is held, the wait it stopped at, by value, and when it stopped) each time a
// queue's first continuation or its stop changes, and the readers take a copy from one publish (a sequence lock).
// A reader never waits for the writer: after EVO_PEEK_TRIES attempts that overlap a publish it answers "not ready",
// which is what losing the race to the writer answered before.
#pragma once
#include <stdint.h>

#define EVO_PEEK_TRIES 3

// p_wait_event's layout (nvn_api.h; vk_exec.c checks the two agree): passed to vk_exec_event_met as its payload.
typedef struct { volatile uint32_t *val; uint32_t mode, value; } EvoWait;

typedef struct {
    uint32_t has;      // a continuation is held
    uint32_t has_at;   // it stopped at a wait (else it has not started and goes on at once)
    EvoWait w;         // that wait
    uint64_t since;    // vk_exec_submit_seq when it stopped
} EvoPeekCopy;

typedef struct {
    uint32_t seq;   // odd while a publish runs
    uint32_t has, has_at, mode, value;
    uintptr_t val;
    uint64_t since;
} EvoPeek;

// The writer: one at a time (vk_exec.c: under the submission lock).
static inline void evo_peek_publish(EvoPeek *p, const EvoPeekCopy *v) {
    uint32_t s = __atomic_load_n(&p->seq, __ATOMIC_RELAXED);
    __atomic_store_n(&p->seq, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);   // a reader that sees any field below also sees the odd sequence
    __atomic_store_n(&p->has, v->has, __ATOMIC_RELAXED);
    __atomic_store_n(&p->has_at, v->has_at, __ATOMIC_RELAXED);
    __atomic_store_n(&p->mode, v->w.mode, __ATOMIC_RELAXED);
    __atomic_store_n(&p->value, v->w.value, __ATOMIC_RELAXED);
    __atomic_store_n(&p->val, (uintptr_t)v->w.val, __ATOMIC_RELAXED);
    __atomic_store_n(&p->since, v->since, __ATOMIC_RELAXED);
    __atomic_store_n(&p->seq, s + 2, __ATOMIC_RELEASE);
}

// Any thread: 1 with *out a copy of one publish, 0 when every try overlapped a publish.
static inline int evo_peek_read(const EvoPeek *p, EvoPeekCopy *out) {
    for (int k = 0; k < EVO_PEEK_TRIES; k++) {
        uint32_t s0 = __atomic_load_n(&p->seq, __ATOMIC_ACQUIRE);
        if (s0 & 1) continue;
        out->has = __atomic_load_n(&p->has, __ATOMIC_RELAXED);
        out->has_at = __atomic_load_n(&p->has_at, __ATOMIC_RELAXED);
        out->w.mode = __atomic_load_n(&p->mode, __ATOMIC_RELAXED);
        out->w.value = __atomic_load_n(&p->value, __ATOMIC_RELAXED);
        out->w.val = (volatile uint32_t *)__atomic_load_n(&p->val, __ATOMIC_RELAXED);
        out->since = __atomic_load_n(&p->since, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);   // the fields are read before the sequence is read again
        if (__atomic_load_n(&p->seq, __ATOMIC_RELAXED) == s0) return 1;
    }
    return 0;
}

// The decision vk_exec.c cont_ready makes for a continuation, from a copy: it can go on when it has not started, its
// wait holds (met: vk_exec_event_met), or it has waited more than max submissions.
static inline int evo_peek_ready(const EvoPeekCopy *c, int (*met)(const void *wait), uint64_t submit_seq, uint64_t max) {
    return c->has && (!c->has_at || met(&c->w) || submit_seq - c->since > max);
}
