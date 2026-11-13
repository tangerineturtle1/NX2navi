// runtime/platform: what a thread keeps for itself is given back when its function returns (win32.c thread_main,
// platform.h plat_thread_end_hook). Threads that sleep through plat_wait_ns and read a file are made and ended by the
// hundred, joined and detached; the end hook must run once on each, and on Windows the process must not hold more
// handles afterwards than a few threads' worth. Built and run by tests/thread_end_check.py.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "platform/platform.h"

#ifdef _WIN32
#include <windows.h>
static long handles(void) {
    DWORD n = 0;
    return GetProcessHandleCount(GetCurrentProcess(), &n) ? (long)n : -1;
}
#else
static long handles(void) { return 0; }   // no handle per thread there; the hook is what is checked
#endif

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

void rt_log(const char *fmt, ...) {   // the platform layer's lines
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static const char *path;
static volatile int32_t ended, hook_runs, hook_other;
static __thread int worked;

static void hook(void) {
    if (worked) plat_inc32(&hook_runs);
    else plat_inc32(&hook_other);
    worked = 0;
}

static void worker(void *arg) {
    (void)arg;
    worked = 1;
    plat_wait_ns(1200000);   // over a millisecond: the thread's own timer
    PlatFile *f = plat_file_open(path);
    char b[16];
    if (!f || plat_file_pread(f, b, sizeof b, 0) != (int64_t)sizeof b) plat_inc32(&hook_other);   // counted as a failure
    if (f) plat_file_close(f);
    plat_inc32(&ended);
}

static void wait_ended(int32_t want) {
    for (int i = 0; i < 20000 && __atomic_load_n(&ended, __ATOMIC_ACQUIRE) < want; i++) plat_sleep_ms(1);
    plat_sleep_ms(100);   // the last ones leave their start function and the system lets go of them
}

int main(int argc, char **argv) {
    if (argc != 2) {
        printf("usage: thread_end_check FILE_OF_16_BYTES_OR_MORE\n");
        return 2;
    }
    path = argv[1];
    plat_thread_end_hook(hook);
    enum { WARM = 16, ROUNDS = 25, PER = 16 };
    for (int i = 0; i < WARM; i++) plat_thread_spawn(worker, NULL, "warm", 0);
    wait_ended(WARM);
    long h0 = handles();
    int32_t made = WARM;
    for (int r = 0; r < ROUNDS; r++) {
        PlatThread *t[PER / 2];
        for (int i = 0; i < PER / 2; i++) {
            t[i] = plat_thread_new(worker, NULL, "joined");
            made += t[i] != NULL;
            made += plat_thread_spawn(worker, NULL, "detached", 0);
        }
        for (int i = 0; i < PER / 2; i++)
            if (t[i]) plat_thread_join(t[i]);
        wait_ended(made);
    }
    long h1 = handles();
    CHECK(made == WARM + ROUNDS * PER, "%d threads made of %d", (int)made, WARM + ROUNDS * PER);
    CHECK(__atomic_load_n(&ended, __ATOMIC_ACQUIRE) == made, "%d of %d threads ended", (int)ended, (int)made);
    CHECK(hook_runs == made, "the end hook ran on %d of %d threads", (int)hook_runs, (int)made);
    CHECK(!hook_other, "%d threads could not read the file, or ran the hook without having worked", (int)hook_other);
    // each thread made a timer and a read event: left open, 400 threads would hold 800 more handles
    CHECK(h1 - h0 < 64, "%ld handles more after %d ended threads (%ld before, %ld after)", h1 - h0, ROUNDS * PER, h0, h1);
    printf("thread_end_check: %s (%d threads, the hook ran on %d, handles %ld before and %ld after)\n",
           fails ? "FAILED" : "ok", (int)made, (int)hook_runs, h0, h1);
    return fails ? 1 : 0;
}
