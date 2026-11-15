// runtime/vk_evo_peek.h: the copy of each event-ordered queue's first held wait that vk_exec.c vk_exec_held_ready
// reads without the submission lock, and the decision it makes from that copy. Built and run by
// tests/evo_peek_check.py; prints each failed check and exits 1 on any.
// One writer republishes changing waits while three readers copy them, as the submitting thread (vk_exec.c evo_resume,
// cont_push) and the presenting threads (vk_backend.c held_resume, submit_sync) do: no reader may take a copy mixed
// from two publishes, and none may wait for the writer.
#include <stdint.h>
#include <stdio.h>

#include "vk_evo_peek.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- the decision, with a stand-in for vk_exec_event_met that answers from the wait's value
static int met_calls;
static int met_stub(const void *wait) {
    met_calls++;
    return ((const EvoWait *)wait)->value == 1;
}

// ---- the race: every field of publish i is derived from i, so a mixed copy shows
enum { READERS = 3, PUBLISHES = 1000000 };
static EvoPeek shared;
static volatile int writer_done;
static volatile long long torn, copies, refused;

static EvoPeekCopy make(uint32_t i) {
    EvoPeekCopy v = {.has = 1, .has_at = i & 1, .since = (uint64_t)i * 5 + 3};
    v.w.val = (volatile uint32_t *)(uintptr_t)((uint64_t)i * 3 + 0x10000);
    v.w.mode = i * 7, v.w.value = ~i;
    return v;
}
static int consistent(const EvoPeekCopy *c) {
    uint32_t i = ~c->w.value;
    EvoPeekCopy want = make(i);
    return c->has == want.has && c->has_at == want.has_at && c->since == want.since && c->w.val == want.w.val &&
           c->w.mode == want.w.mode;
}

#ifdef _WIN32
static DWORD WINAPI writer(LPVOID arg) {
#else
static void *writer(void *arg) {
#endif
    (void)arg;
    for (uint32_t i = 1; i <= PUBLISHES; i++) {
        EvoPeekCopy v = make(i);
        evo_peek_publish(&shared, &v);
    }
    __atomic_store_n(&writer_done, 1, __ATOMIC_RELEASE);
    return 0;
}

#ifdef _WIN32
static DWORD WINAPI reader(LPVOID arg) {
#else
static void *reader(void *arg) {
#endif
    (void)arg;
    long long t = 0, n = 0, r = 0;
    while (!__atomic_load_n(&writer_done, __ATOMIC_ACQUIRE)) {
        EvoPeekCopy c;
        if (!evo_peek_read(&shared, &c)) {
            r++;
            continue;
        }
        n++;
        if (c.has) t += !consistent(&c);
        else t += c.has_at || c.since || c.w.val || c.w.mode || c.w.value;   // the all-zero state before the first
    }
    __atomic_add_fetch(&torn, t, __ATOMIC_RELAXED);
    __atomic_add_fetch(&copies, n, __ATOMIC_RELAXED);
    __atomic_add_fetch(&refused, r, __ATOMIC_RELAXED);
    return 0;
}

int main(void) {
    // ---- the decision vk_exec.c cont_ready makes, from a copy
    EvoPeekCopy c = {0};
    CHECK(!evo_peek_ready(&c, met_stub, 100, 36), "a queue with nothing held was ready");
    c.has = 1;
    met_calls = 0;
    CHECK(evo_peek_ready(&c, met_stub, 100, 36) && !met_calls, "held work that has not started was not ready at once");
    c.has_at = 1, c.since = 90, c.w.value = 1;
    CHECK(evo_peek_ready(&c, met_stub, 100, 36), "held work whose wait holds was not ready");
    c.w.value = 0;
    CHECK(!evo_peek_ready(&c, met_stub, 100, 36), "held work whose wait does not hold was ready");
    CHECK(!evo_peek_ready(&c, met_stub, 90 + 36, 36), "held work that waited exactly the limit was ready");
    CHECK(evo_peek_ready(&c, met_stub, 90 + 37, 36), "held work past the limit was not ready");

    // ---- a publish is read back whole; a fresh one reads as nothing held
    EvoPeek p = {0};
    EvoPeekCopy out;
    CHECK(evo_peek_read(&p, &out) && !out.has, "a fresh copy did not read as nothing held");
    EvoPeekCopy v = make(12345);
    evo_peek_publish(&p, &v);
    CHECK(evo_peek_read(&p, &out) && consistent(&out) && ~out.w.value == 12345u, "a publish was not read back whole");
    CHECK(p.seq == 2, "one publish left the sequence at %u, not 2", p.seq);

    // ---- a reader never waits for a publish that does not end: it gives up after EVO_PEEK_TRIES tries
    p.seq = 3;
    CHECK(!evo_peek_read(&p, &out), "a read during a publish returned a copy");
    p.seq = 4;

    // ---- one writer, three readers
#ifdef _WIN32
    HANDLE t[READERS + 1];
    for (int k = 0; k < READERS; k++) t[k] = CreateThread(NULL, 0, reader, NULL, 0, NULL);
    t[READERS] = CreateThread(NULL, 0, writer, NULL, 0, NULL);
    WaitForMultipleObjects(READERS + 1, t, TRUE, INFINITE);
    for (int k = 0; k <= READERS; k++) CloseHandle(t[k]);
#else
    pthread_t t[READERS + 1];
    for (int k = 0; k < READERS; k++) pthread_create(&t[k], NULL, reader, NULL);
    pthread_create(&t[READERS], NULL, writer, NULL);
    for (int k = 0; k <= READERS; k++) pthread_join(t[k], NULL);
#endif
    printf("evo_peek_check: %d publishes; readers took %lld copies, gave up %lld times, %lld mixed\n", PUBLISHES,
           copies, refused, torn);
    CHECK(!torn, "%lld copies mixed two publishes", torn);
    CHECK(copies > 0, "no reader took a copy while the writer ran");
    EvoPeekCopy last;
    CHECK(evo_peek_read(&shared, &last) && ~last.w.value == PUBLISHES && consistent(&last),
          "after the writer ended the copy is not its last publish");

    printf("evo_peek_check: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
