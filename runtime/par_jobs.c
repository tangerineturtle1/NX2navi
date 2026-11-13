// A pool of persistent threads that runs one job at a time (par_jobs.h). The threads sleep on a condition variable
// between jobs and cost nothing while the game does not use them. A job is n pieces claimed one by one from an atomic
// counter by the caller and the workers alike, so a slow piece or a busy core evens out. One caller at a time: the
// pool's busy lock is only tried, never waited for, so a second caller (or a piece that calls par_run) runs alone
// rather than waiting, and no caller can wait for a pool that waits for it.
//
// Size: the usable hardware threads less two (the drawing thread and the game's main thread), at most PAR_MAX_THREADS
// in all, the caller counted. NX2NAVI_PAR_JOBS=0 runs every job on its caller (cfg.c).
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "par_jobs.h"
#include "platform/platform.h"

#define PAR_MAX_THREADS 8
#define PAR_MEMCPY_MIN (1u << 20)   // below this, waking the pool costs about what the copy does
#define PAR_MEMCPY_PIECE (256u << 10)

static struct {
    PlatOnce once;
    PlatMutex busy;                   // held by the caller for its whole job
    PlatMutex lock;
    PlatCond work, done;
    int threads;                      // workers, the caller not counted
    int off;
    uint64_t gen;                     // bumped per job; a worker takes part in each job once
    int active;                       // workers that have not finished the current job
    void (*fn)(void *arg, int i);     // the current job (written under lock before gen is bumped)
    void *arg;
    int n;
    volatile int32_t next;            // the first piece not yet claimed
} J = {PLAT_ONCE_INIT, .lock = PLAT_MUTEX_INIT, .work = PLAT_COND_INIT, .done = PLAT_COND_INIT};

static void claim(void) {
    for (;;) {
        int i = __atomic_fetch_add(&J.next, 1, __ATOMIC_RELAXED);
        if (i >= J.n) return;
        J.fn(J.arg, i);
    }
}

static void worker(void *arg) {
    (void)arg;
    uint64_t seen = 0;
    plat_mutex_lock(&J.lock);
    for (;;) {
        while (J.gen == seen) plat_cond_wait(&J.work, &J.lock);
        seen = J.gen;
        plat_mutex_unlock(&J.lock);
        claim();
        plat_mutex_lock(&J.lock);
        if (!--J.active) plat_cond_broadcast(&J.done);
    }
}

static void pool_init(void *param) {
    (void)param;
    if (!cfg_bool("NX2NAVI_PAR_JOBS")) {
        J.off = 1;
        rt_log("par_jobs: off (NX2NAVI_PAR_JOBS=0): large texture copies run on the thread that makes them");
        return;
    }
    int cpus = (int)plat_cpu_usable() - 2;
    int n = (cpus < 1 ? 1 : cpus > PAR_MAX_THREADS ? PAR_MAX_THREADS : cpus) - 1;
    for (int i = 0; i < n; i++) {
        if (!plat_thread_spawn(worker, NULL, "par jobs", 0)) break;
        J.threads++;
    }
    rt_log("par_jobs: %d workers for large texture copies (and the thread that asks)", J.threads);
}

int par_threads(void) {
    plat_once(&J.once, pool_init, NULL);
    return J.off ? 1 : J.threads + 1;
}

void par_run(int n, void (*fn)(void *arg, int i), void *arg) {
    if (n <= 0) return;
    if (n == 1 || par_threads() < 2 || !plat_mutex_trylock(&J.busy)) {
        for (int i = 0; i < n; i++) fn(arg, i);
        return;
    }
    plat_mutex_lock(&J.lock);
    J.fn = fn, J.arg = arg, J.n = n;
    __atomic_store_n(&J.next, 0, __ATOMIC_RELAXED);
    J.active = J.threads;
    J.gen++;
    plat_cond_broadcast(&J.work);
    plat_mutex_unlock(&J.lock);
    claim();
    plat_mutex_lock(&J.lock);
    while (J.active) plat_cond_wait(&J.done, &J.lock);
    plat_mutex_unlock(&J.lock);
    plat_mutex_unlock(&J.busy);
}

typedef struct {
    char *dst;
    const char *src;
    size_t n, piece;
} Copy;

static void copy_piece(void *arg, int i) {
    const Copy *c = arg;
    size_t at = (size_t)i * c->piece, len = c->n - at < c->piece ? c->n - at : c->piece;
    memcpy(c->dst + at, c->src + at, len);
}

void par_memcpy(void *dst, const void *src, size_t n) {
    if (n < PAR_MEMCPY_MIN || par_threads() < 2) {
        memcpy(dst, src, n);
        return;
    }
    Copy c = {dst, src, n, PAR_MEMCPY_PIECE};
    par_run((int)((n + c.piece - 1) / c.piece), copy_piece, &c);
}
