// runtime/vk_sync_hw.h: the newest value each of the replay's timelines may reach, and the test that takes a value read
// above it as a lost device (vk_sync.c "a lost device in the timelines' values"). Built and run by
// tests/sync_hw_check.py; prints each failed check and exits 1 on any.
#include <stdint.h>
#include <stdio.h>

#include "vk_sync_hw.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Four threads raise one note with interleaved values, as the submitting thread, the present and the probes may.
enum { THREADS = 4, PER = 200000 };
static volatile uint64_t shared_hw;

#ifdef _WIN32
static DWORD WINAPI raiser(LPVOID arg) {
#else
static void *raiser(void *arg) {
#endif
    uint64_t k = (uint64_t)(uintptr_t)arg;
    for (uint64_t i = 0; i < PER; i++) sync_hw_raise(&shared_hw, i * THREADS + k);
    return 0;
}

int main(void) {
    // ---- a note only rises
    volatile uint64_t hw = 0;
    sync_hw_raise(&hw, 10);
    CHECK(hw == 10, "a first value of 10 left %llu", (unsigned long long)hw);
    sync_hw_raise(&hw, 7);
    CHECK(hw == 10, "an older value (7) lowered the note to %llu", (unsigned long long)hw);
    sync_hw_raise(&hw, 11);
    CHECK(hw == 11, "a newer value (11) left %llu", (unsigned long long)hw);

    // ---- what a live device can read, and what it cannot
    // submissions 1..11 noted: the timeline at any of them, or not yet at the first, is a live reading
    for (uint64_t raw = 0; raw <= 11; raw++) CHECK(!sync_hw_beyond(raw, hw), "a reading of %llu with 11 noted counted as lost", (unsigned long long)raw);
    CHECK(sync_hw_beyond(12, hw), "a reading one past the newest noted value did not count as lost");
    // after a timeout reset Windows releases the waiters of a timeline with the largest value
    CHECK(sync_hw_beyond(UINT64_MAX, hw), "the reading a reset leaves (UINT64_MAX) did not count as lost");
    // a note at the largest value (nothing above it can be read) never counts a reading as lost
    volatile uint64_t top = 0;
    sync_hw_raise(&top, UINT64_MAX);
    CHECK(!sync_hw_beyond(UINT64_MAX, top), "a reading equal to a note of UINT64_MAX counted as lost");

    // ---- the restart of vk_sync_start: the note is raised to the value signalled before the signal, so the
    // completion thread's first reading after it is a live one
    volatile uint64_t restart = 0;
    uint64_t issued = 500, now = 3;   // a new device: its timeline at 3, the front end's values at 500
    sync_hw_raise(&restart, now > issued ? now : issued);
    CHECK(!sync_hw_beyond(issued, restart), "the restart's own value (500) counted as lost");
    CHECK(sync_hw_beyond(issued + 1, restart), "a reading past the restart's value did not count as lost");

    // ---- concurrent raises end at the largest value any thread gave
#ifdef _WIN32
    HANDLE t[THREADS];
    for (int k = 0; k < THREADS; k++) t[k] = CreateThread(NULL, 0, raiser, (LPVOID)(uintptr_t)k, 0, NULL);
    WaitForMultipleObjects(THREADS, t, TRUE, INFINITE);
    for (int k = 0; k < THREADS; k++) CloseHandle(t[k]);
#else
    pthread_t t[THREADS];
    for (int k = 0; k < THREADS; k++) pthread_create(&t[k], NULL, raiser, (void *)(uintptr_t)k);
    for (int k = 0; k < THREADS; k++) pthread_join(t[k], NULL);
#endif
    uint64_t want = (uint64_t)(PER - 1) * THREADS + (THREADS - 1);
    CHECK(shared_hw == want, "four threads' raises ended at %llu, not the largest value %llu",
          (unsigned long long)shared_hw, (unsigned long long)want);

    printf("sync_hw_check: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
