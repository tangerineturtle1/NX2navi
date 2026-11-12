// plat_restart on Linux (runtime/platform/linux.c): the game it starts may run on every processor this one could
// before plat_cpu_reserve took a core for the recording thread, whichever thread asks for the restart. No game files.
// Build and run from the repository root:
//   clang -O2 -ffunction-sections -fdata-sections -Iruntime -c runtime/platform/linux.c -o /tmp/plat.o
//   clang -O2 -Wall -Iruntime tests/platform_restart_mask_test.c /tmp/plat.o -Wl,--gc-sections -lpthread -ldl -o /tmp/restart_mask
//   /tmp/restart_mask      (exit 77: this machine has no core plat_cpu_reserve would take)
// The test starts itself again through plat_restart; that copy only writes how many processors it may run on.
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "platform/platform.h"

#define OUT "RESTART_MASK_TEST_OUT"

static int allowed(void) {
    cpu_set_t set;
    return sched_getaffinity(0, sizeof set, &set) ? -1 : CPU_COUNT(&set);
}

// What the copy plat_restart started wrote, waiting up to 5 s for it; -1 when it never came.
static int restarted_count(const char *path) {
    for (int i = 0; i < 500; i++) {
        FILE *f = fopen(path, "r");
        int n = -1;
        if (f) {
            int got = fscanf(f, "%d", &n);
            fclose(f);
            if (got == 1) {
                remove(path);
                return n;
            }
        }
        usleep(10000);
    }
    return -1;
}

static atomic_int go, done;

// A thread that existed before the core was taken, as the game's main thread does: it asks for the restart.
static void *other_thread(void *arg) {
    (void)arg;
    while (!atomic_load(&go)) usleep(1000);
    plat_restart();
    atomic_store(&done, 1);
    return NULL;
}

int main(void) {
    const char *out = getenv(OUT);
    if (getenv("NX2NAVI_RESTART_WAIT") && out) {   // the copy plat_restart started
        FILE *f = fopen(out, "w");
        if (f) fprintf(f, "%d\n", allowed()), fclose(f);
        return 0;
    }
    char path[64];
    snprintf(path, sizeof path, "/tmp/restart-mask-%d.txt", (int)getpid());
    setenv(OUT, path, 1);
    remove(path);
    int before = allowed(), fails = 0;
    pthread_t th;
    pthread_create(&th, NULL, other_thread, NULL);
    char which[128];
    int core = plat_cpu_reserve(which, sizeof which);
    if (!core) { puts("SKIP: no core to reserve on this machine"); return 77; }
    atomic_store(&go, 1);
    while (!atomic_load(&done)) usleep(1000);
    pthread_join(th, NULL);
    int from_other = restarted_count(path);
    plat_restart();   // and from the thread that holds the core
    int from_holder = restarted_count(path);
    printf("%d processors before; the core taken: %s; a restart from another thread starts with %d, from the holder with %d\n",
           before, which, from_other, from_holder);
    if (from_other != before) puts("FAIL: the restarted process lacks processors (asked by another thread)"), fails++;
    if (from_holder != before) puts("FAIL: the restarted process lacks processors (asked by the holder)"), fails++;
    if (!fails) puts("restart mask: the restarted process has every processor");
    return fails != 0;
}
