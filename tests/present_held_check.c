// tests/present_held_check.py builds this: runtime/vk_present_held.h, the present that waits for held work, with one
// presenting thread and three submitting threads. Every frame must reach the present callback exactly once and in
// frame order, never two callbacks at once, and a submitting thread must never wait for the lock. "old" runs the
// unlocked check-then-clear of 0.10.1 through the same harness and only prints what it sees, to show that the harness
// catches the race.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk_present_held.h"

#define FRAMES 200000
#define SUBMITTERS 3
#define PH_MAX 8

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("present_held_check: FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static int old_mode;
static volatile int32_t inside, stop;
static volatile uint64_t last_frame, presents, dup_or_order, overlap, held_presents, seq;
static volatile int32_t held;
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint32_t rnd(uint64_t *s) {
    *s ^= *s << 13, *s ^= *s >> 7, *s ^= *s << 17;
    return (uint32_t)*s;
}

static void spin(uint32_t n) {
    for (volatile uint32_t i = 0; i < n; i++) {}
}

// vk_backend.c ph_out's stand-in: present_image's work is the spin, the window a second caller would fall into.
static void present_cb(const PhJob *j, int why, void *arg) {
    (void)arg;
    if (__atomic_exchange_n(&inside, 1, __ATOMIC_ACQ_REL)) __atomic_add_fetch(&overlap, 1, __ATOMIC_RELAXED);
    uint64_t f = j->frame;
    uint64_t was = __atomic_load_n(&last_frame, __ATOMIC_ACQUIRE);
    if (f != was + 1) __atomic_add_fetch(&dup_or_order, 1, __ATOMIC_RELAXED);
    spin(200);
    __atomic_store_n(&last_frame, f, __ATOMIC_RELEASE);
    __atomic_add_fetch(&presents, 1, __ATOMIC_RELAXED);
    if (why != PH_NONE) __atomic_add_fetch(&held_presents, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&inside, 0, __ATOMIC_RELEASE);
}

// vk_backend.c ph_why_after: out when nothing is held any more, or PH_MAX submissions after it began to wait.
static int why_after(const PhJob *j, void *arg) {
    (void)arg;
    if (!__atomic_load_n(&held, __ATOMIC_ACQUIRE)) return PH_AFTER;
    return __atomic_load_n(&seq, __ATOMIC_RELAXED) - j->since >= PH_MAX ? PH_CAPPED : PH_NONE;
}

static int hold_now(void *arg) {
    (void)arg;
    return __atomic_load_n(&held, __ATOMIC_ACQUIRE) > 0;
}

static PhState S = {.lock = PLAT_MUTEX_INIT};

// ---- the 0.10.1 shape (vk_backend.c ph_flush before vk_present_held.h), for the harness's own check
static struct { int on; PhJob job; } OLD;
static void old_flush(int why) {
    if (!OLD.on) return;
    OLD.on = 0;
    present_cb(&OLD.job, why, NULL);
}

static void submitter(void *arg) {
    uint64_t r = (uint64_t)(uintptr_t)arg * 0x2545F4914F6CDD1Dull + 1;
    while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
        uint64_t q = __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED);
        uint32_t x = rnd(&r);
        if (x % 5 == 0) __atomic_add_fetch(&held, 1, __ATOMIC_RELEASE);
        if (x % 3 == 0) __atomic_store_n(&held, 0, __ATOMIC_RELEASE);
        if (old_mode) {
            if (OLD.on) {
                if (!__atomic_load_n(&held, __ATOMIC_ACQUIRE)) old_flush(PH_AFTER);
                else if (q - OLD.job.since >= PH_MAX) old_flush(PH_CAPPED);
            }
        } else {
            ph_flush_try(&S, why_after, present_cb, NULL);
        }
        spin(rnd(&r) % 400);
    }
}

static void stress(void) {
    PlatThread *t[SUBMITTERS];
    for (int i = 0; i < SUBMITTERS; i++) t[i] = plat_thread_new(submitter, (void *)(uintptr_t)(i + 1), "submit");
    for (uint64_t f = 1; f <= FRAMES; f++) {
        uint32_t x = rnd(&rng_state);
        PhJob j = {.frame = f};
        if (old_mode) {
            if (OLD.on) old_flush(PH_NEXT);
            if (x % 2) __atomic_store_n(&held, 0, __ATOMIC_RELEASE);   // held_resume ran what it could
            if (__atomic_load_n(&held, __ATOMIC_ACQUIRE)) {
                OLD.job = j, OLD.job.since = __atomic_load_n(&seq, __ATOMIC_RELAXED);
                OLD.on = 1;
            } else {
                present_cb(&j, PH_NONE, NULL);
            }
        } else {   // vk_backend.c vk_be_present's synchronous branch
            ph_flush_next(&S, present_cb, NULL);
            if (x % 2) __atomic_store_n(&held, 0, __ATOMIC_RELEASE);
            j.since = __atomic_load_n(&seq, __ATOMIC_RELAXED);
            ph_present(&S, &j, hold_now, present_cb, NULL);
        }
        spin(x % 600);
    }
    __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < SUBMITTERS; i++) plat_thread_join(t[i]);
    if (old_mode) {
        old_flush(PH_NEXT);
        printf("present_held_check: old (prints only): %llu presents for %d frames, %llu of them held first; %llu out of "
               "order or twice, %llu while another one ran\n", (unsigned long long)presents, FRAMES,
               (unsigned long long)held_presents, (unsigned long long)dup_or_order, (unsigned long long)overlap);
        return;
    }
    ph_flush_next(&S, present_cb, NULL);
    printf("present_held_check: new: %llu presents for %d frames, %llu of them held first (waited %llu; out after the "
           "work %llu, at PH_MAX %llu, at the next present %llu); %llu out of order or twice, %llu while another one "
           "ran; the presenting thread waited for the lock %llu times\n", (unsigned long long)presents, FRAMES,
           (unsigned long long)held_presents, (unsigned long long)S.waited, (unsigned long long)S.out[PH_AFTER],
           (unsigned long long)S.out[PH_CAPPED], (unsigned long long)S.out[PH_NEXT], (unsigned long long)dup_or_order,
           (unsigned long long)overlap, (unsigned long long)S.lock_waits);
    CHECK(presents == FRAMES, "%llu presents for %d frames", (unsigned long long)presents, FRAMES);
    CHECK(last_frame == FRAMES, "the last frame out is %llu", (unsigned long long)last_frame);
    CHECK(!dup_or_order, "%llu presents out of order or twice", (unsigned long long)dup_or_order);
    CHECK(!overlap, "%llu presents ran while another one did", (unsigned long long)overlap);
    CHECK(S.out[PH_AFTER] && S.out[PH_CAPPED] && S.out[PH_NEXT], "a reason never occurred (after %llu, PH_MAX %llu, "
          "next %llu)", (unsigned long long)S.out[PH_AFTER], (unsigned long long)S.out[PH_CAPPED],
          (unsigned long long)S.out[PH_NEXT]);
}

// ---- one thread, step by step
static uint64_t seen[16], nseen;
static int seen_why[16];
static void record_cb(const PhJob *j, int why, void *arg) {
    (void)arg;
    if (nseen < 16) seen[nseen] = j->frame, seen_why[nseen] = why;
    nseen++;
}
static int hold_yes(void *arg) { (void)arg; return 1; }
static int hold_no(void *arg) { (void)arg; return 0; }
static int why_none(const PhJob *j, void *arg) { (void)j, (void)arg; return PH_NONE; }
static int why_out(const PhJob *j, void *arg) { (void)j, (void)arg; return PH_AFTER; }

static void steps(void) {
    PhState s = {.lock = PLAT_MUTEX_INIT};
    PhJob a = {.frame = 1}, b = {.frame = 2};
    ph_present(&s, &a, hold_no, record_cb, NULL);
    CHECK(nseen == 1 && seen[0] == 1 && seen_why[0] == PH_NONE && !ph_armed(&s), "a present with nothing held goes out");
    nseen = 0;
    ph_present(&s, &a, hold_yes, record_cb, NULL);
    CHECK(nseen == 0 && ph_armed(&s) && s.waited == 1, "a present with work held waits");
    CHECK(!ph_flush_try(&s, why_none, record_cb, NULL) && nseen == 0 && ph_armed(&s),
          "a submission after which work is still held keeps the present");
    CHECK(ph_flush_try(&s, why_out, record_cb, NULL) && nseen == 1 && seen[0] == 1 && seen_why[0] == PH_AFTER &&
          !ph_armed(&s) && s.out[PH_AFTER] == 1, "a submission that replayed the held work puts the present out");
    CHECK(!ph_flush_try(&s, why_out, record_cb, NULL) && nseen == 1, "a held present goes out once");
    nseen = 0;
    ph_present(&s, &a, hold_yes, record_cb, NULL);
    CHECK(ph_flush_next(&s, record_cb, NULL) && nseen == 1 && seen_why[0] == PH_NEXT && !ph_armed(&s),
          "the next present puts the held one out");
    CHECK(!ph_flush_next(&s, record_cb, NULL) && nseen == 1, "nothing goes out twice at the next present");
    nseen = 0;
    ph_present(&s, &a, hold_yes, record_cb, NULL);
    ph_present(&s, &b, hold_no, record_cb, NULL);
    CHECK(nseen == 2 && seen[0] == 1 && seen_why[0] == PH_NEXT && seen[1] == 2 && seen_why[1] == PH_NONE,
          "a present behind a held one puts the earlier one out first");
}

// ---- the lock held elsewhere: a submitting thread does not wait, the presenting thread waits and is counted
static PhState L = {.lock = PLAT_MUTEX_INIT};
static volatile uint64_t try_ns;
static volatile int32_t try_out = -1, waits_told;
static void on_wait(uint64_t t0) { (void)t0; __atomic_add_fetch(&waits_told, 1, __ATOMIC_RELAXED); }
static void try_thread(void *arg) {
    (void)arg;
    uint64_t t0 = plat_now_ns();
    try_out = ph_flush_try(&L, why_out, record_cb, NULL);
    try_ns = plat_now_ns() - t0;
}
static void present_thread(void *arg) {
    (void)arg;
    PhJob c = {.frame = 3};
    ph_present(&L, &c, hold_no, record_cb, NULL);
}

static void contended(void) {
    PhJob a = {.frame = 1};
    L.on_wait = on_wait;
    ph_present(&L, &a, hold_yes, record_cb, NULL);   // armed
    nseen = 0;
    plat_mutex_lock(&L.lock);   // another thread puts a held present out
    PlatThread *t = plat_thread_new(try_thread, NULL, "try");
    plat_thread_join(t);
    CHECK(try_out == 0 && nseen == 0 && ph_armed(&L), "a submitting thread put the present out past a held lock");
    CHECK(try_ns < 50000000ull, "a submitting thread took %.1f ms to give up on a held lock", try_ns / 1e6);
    PlatThread *p = plat_thread_new(present_thread, NULL, "present");
    plat_sleep_ms(30);
    CHECK(nseen == 0, "the presenting thread went past a held lock");
    __atomic_store_n(&L.on, 0, __ATOMIC_RELAXED);   // what the lock's holder did: the held present went out
    plat_mutex_unlock(&L.lock);
    plat_thread_join(p);
    CHECK(nseen == 1 && seen[0] == 3, "the presenting thread's present after the lock was let go");
    CHECK(L.lock_waits == 1 && waits_told == 1 && L.lock_wait_ns >= 20000000ull && L.lock_wait_max == L.lock_wait_ns,
          "the presenting thread's wait was not counted: %llu waits, %.1f ms, longest %.1f ms, told %d",
          (unsigned long long)L.lock_waits, L.lock_wait_ns / 1e6, L.lock_wait_max / 1e6, waits_told);
}

int main(int argc, char **argv) {
    old_mode = argc > 1 && !strcmp(argv[1], "old");
    if (!old_mode) steps(), contended();
    stress();
    if (old_mode) return 0;
    printf("present_held_check: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
