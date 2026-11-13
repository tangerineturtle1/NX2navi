// Log writes on a thread of their own.
//
// A log line was written by the thread that logged it: to the log file and to stderr (main.c log_sink). A write can
// stall: a console that scrolls, a pipe whose reader is busy, a virus scanner, a disk that flushes; hundreds of
// milliseconds at worst. The render thread logs too, and a frame it spends in such a write is a hitch with nothing
// else to show for it.
//
// With the queue on (logq_async) a line is copied into a buffer under a lock that is held for the copy alone, and the
// "log" thread writes the buffer out, so lines keep the order they were queued in. A line that finds no room is written
// by its own thread, after everything queued before it (put). logq_sync writes what is queued on the calling thread
// and turns the queue off for the rest of the run: a fatal error, the crash report and the end of the process call it
// (rt_log_sync), so no line is lost to an exit and their own lines are written as they are made.
//
// The log thread gathers before it writes (log_main): from the first line it finds it waits LOGQ_GATHER_NS, or until
// the queue is half full (LOGQ_GATHER_BYTES), and then writes everything queued by then in one write. It used to
// write as soon as it woke, which outside a burst is once for every line: the thread that logged woke the log thread
// for each, and each was a write of its own to every destination. The price is that the log file and the console are
// up to LOGQ_GATHER_NS behind the game. The lines being gathered are in the queue like any others, so logq_sync
// writes them too, and a fatal error, a crash report and an exit still lose none. A process that is ended from
// outside runs no code of ours and loses what was gathered at that moment; it used to lose only the lines the log
// thread had not reached yet.
//
// Every write is timed. One that took LOGQ_SLOW_NS or longer is counted, and a line says so at most every ten
// seconds: with the queue that time was the log thread's, without it (NX2NAVI_LOG_ASYNC=0) the logging thread's.
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "logq.h"
#include "platform/platform.h"

#define LOGQ_CAP (1u << 20)
#define LOGQ_GATHER_NS 250000000ull        // log_main: a batch is written this long after its first line was found ...
#define LOGQ_GATHER_BYTES (LOGQ_CAP / 2)   // ... or as soon as this much is queued
#define LOGQ_SLOW_NS 2000000ll
#define LOGQ_SAY_NS 10000000000ull

static struct {
    PlatMutex q, io;                 // q: buf and n; io: the queue's writes and out. io is taken first
    PlatCond more;
    char *buf, *out;                 // LOGQ_CAP bytes each: the queue, and what the log thread is writing
    size_t n;
    LogqSink sink;
    volatile int32_t on, saying;
    volatile int64_t slow, said, worst_ns, full;   // slow writes, those reported, the longest; lines that found no room
    volatile uint64_t said_at;
} Q = {.q = PLAT_MUTEX_INIT, .io = PLAT_MUTEX_INIT, .more = PLAT_COND_INIT};

void logq_init(LogqSink sink) { Q.sink = sink; }

// The queue's two buffers, made once when the log thread starts (logq_async) and kept: the memory report's figure.
size_t logq_bytes(void) { return __atomic_load_n(&Q.buf, __ATOMIC_RELAXED) ? 2 * (size_t)LOGQ_CAP : 0; }

// The write itself, timed.
static void put_out(const char *p, size_t n) {
    uint64_t t0 = plat_now_ns();
    Q.sink(p, n);
    int64_t d = (int64_t)(plat_now_ns() - t0);
    if (d < LOGQ_SLOW_NS) return;
    __atomic_add_fetch(&Q.slow, 1, __ATOMIC_RELAXED);
    int64_t w = __atomic_load_n(&Q.worst_ns, __ATOMIC_RELAXED);
    while (d > w && !__atomic_compare_exchange_n(&Q.worst_ns, &w, d, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

// The line about slow writes, when there are new ones and the last line is ten seconds old. Called with no lock held:
// the line goes through logq_write like any other.
static void say_slow(void) {
    int64_t slow = __atomic_load_n(&Q.slow, __ATOMIC_RELAXED);
    if (slow == __atomic_load_n(&Q.said, __ATOMIC_RELAXED)) return;
    uint64_t now = plat_now_ns(), at = __atomic_load_n(&Q.said_at, __ATOMIC_RELAXED);
    if ((at && now - at < LOGQ_SAY_NS) || plat_xchg32(&Q.saying, 1)) return;
    __atomic_store_n(&Q.said, slow, __ATOMIC_RELAXED);
    __atomic_store_n(&Q.said_at, now ? now : 1, __ATOMIC_RELAXED);
    rt_log("log: %lld writes of the log took over %d ms so far, the longest %.1f ms, %s (NX2NAVI_LOG_ASYNC); %lld lines "
           "found the queue full and waited for its write", (long long)slow, (int)(LOGQ_SLOW_NS / 1000000),
           __atomic_load_n(&Q.worst_ns, __ATOMIC_RELAXED) / 1e6,
           __atomic_load_n(&Q.on, __ATOMIC_ACQUIRE) ? "on the log thread" : "on the threads that logged",
           (long long)__atomic_load_n(&Q.full, __ATOMIC_RELAXED));
    plat_xchg32(&Q.saying, 0);
}

// Writes out what is queued, in order. Any thread.
static void drain(void) {
    plat_mutex_lock(&Q.io);
    plat_mutex_lock(&Q.q);
    char *b = Q.buf;
    size_t n = Q.n;
    Q.buf = Q.out, Q.out = b, Q.n = 0;
    plat_mutex_unlock(&Q.q);
    if (n) put_out(b, n);
    plat_mutex_unlock(&Q.io);
}

// 1 when the bytes were queued, 0 when the caller is to write them: the queue is off, or it had no room, and then
// what it held has been written first (drain). So a thread's lines reach the log in the order it logged them.
//
// The queue's lock is waited for. A thread that wrote its line itself because the lock was taken would put that line
// before the lines it queued earlier, and on a busy machine the lock is taken for a whole time slice whenever its
// holder loses the processor inside the copy. Waiting cannot hang: the lock is held for the copy alone, the runtime
// stops a thread only for moments in which it takes no lock and logs nothing (kernel.c ap_try and dg_sample,
// prof_win.c), and a fatal error or a crash turns the queue off before it logs (logq_sync, which only tries the locks).
static int put(const char *p, size_t n) {
    if (!__atomic_load_n(&Q.on, __ATOMIC_ACQUIRE)) return 0;
    plat_mutex_lock(&Q.q);
    int on = __atomic_load_n(&Q.on, __ATOMIC_ACQUIRE);
    if (!on || Q.n + n > LOGQ_CAP) {   // no room: the log thread is held up in a write, or the line is larger than the queue
        plat_mutex_unlock(&Q.q);
        if (on) {
            __atomic_add_fetch(&Q.full, 1, __ATOMIC_RELAXED);
            drain();
        }
        return 0;
    }
    // the log thread sleeps until the first line of a batch, and then until the batch fills half the queue (log_main)
    int wake = !Q.n || (Q.n < LOGQ_GATHER_BYTES && Q.n + n >= LOGQ_GATHER_BYTES);
    memcpy(Q.buf + Q.n, p, n);
    Q.n += n;
    plat_mutex_unlock(&Q.q);
    if (wake) plat_cond_signal(&Q.more);
    return 1;
}

void logq_write(const char *p, size_t n) {
    if (put(p, n)) return;
    put_out(p, n);
    say_slow();
}

// The log thread: it sleeps until a line is queued, gathers, and writes the batch (drain). While it gathers it holds
// no lock, so neither the threads that log nor logq_sync wait for it. The gathering ends when its time is up, when
// the queue is half full (put signals that), and when the queue is empty again because another thread wrote it: a
// line that found no room (put), or logq_sync, after which nothing is queued any more and this thread sleeps for good.
static void log_main(void *arg) {
    (void)arg;
    for (;;) {
        plat_mutex_lock(&Q.q);
        while (!Q.n) plat_cond_wait(&Q.more, &Q.q);
        for (uint64_t t0 = plat_now_ns(), spent = 0; Q.n && Q.n < LOGQ_GATHER_BYTES && spent < LOGQ_GATHER_NS;
             spent = plat_now_ns() - t0)
            plat_cond_wait_ns(&Q.more, &Q.q, LOGQ_GATHER_NS - spent);
        plat_mutex_unlock(&Q.q);
        drain();
        say_slow();
    }
}

int logq_async(void) {
    if (!Q.sink || __atomic_load_n(&Q.on, __ATOMIC_ACQUIRE)) return 0;
    if (!Q.buf) Q.buf = malloc(LOGQ_CAP), Q.out = malloc(LOGQ_CAP);
    if (!Q.buf || !Q.out || !plat_thread_spawn(log_main, NULL, "log", 0)) return 0;
    __atomic_store_n(&Q.on, 1, __ATOMIC_RELEASE);
    return 1;
}

void logq_sync(void) {
    if (!plat_xchg32(&Q.on, 0)) return;
    // Each lock is tried for half a second: the thread that holds it may be the one that crashed, or be stuck in the
    // write this call is there to get past. Without the lock the queue is written as it stands.
    int io = 0, q = 0;
    for (int i = 0; i < 500 && !(io = plat_mutex_trylock(&Q.io)); i++) plat_sleep_ms(1);
    for (int i = 0; i < 500 && !(q = plat_mutex_trylock(&Q.q)); i++) plat_sleep_ms(1);
    size_t n = Q.n;
    Q.n = 0;
    if (n && n <= LOGQ_CAP) put_out(Q.buf, n);
    if (q) plat_mutex_unlock(&Q.q);
    if (io) plat_mutex_unlock(&Q.io);
}
