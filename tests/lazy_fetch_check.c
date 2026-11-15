// runtime/memory.c's lazy write-back (memory.c "Lazy write-back", "Ranges in flight"), built with the Windows platform
// layer and its real fault handler, the rest of the runtime stubbed. The back end's fetch is a stand-in that waits (the
// GPU copy and the wait for it) and then writes a marker over the first 64 bytes of each copy, opening the page for
// it, as the GPU's copy writes past the pages' protection; the back end's clean test may wait too. Built and run by
// tests/lazy_fetch_check.py, one scenario per process (the argument), so an access violation passed to the crash
// handler shows as the exit code. CHECK_CHUNK in the environment sets NX2NAVI_WB_LAZY_CHUNK=1, CHECK_LOCKED
// NX2NAVI_WB_LAZY_UNLOCKED=0. Prints each failed check and exits 1 on any.
#include "memory.c"

#include <stdarg.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- the runtime memory.c and the platform layer reach
unsigned rt_trace;
uint64_t rt_guest_start_ns;
const unsigned long long rt_app_pool_bytes = 0xCD500000ull;
const RtLayoutEntry rt_layout[] = {{"main", MOD_MAIN_BASE, MOD_MAIN_SIZE}};
const int rt_layout_count = 1;
const char *cfg_get(const char *name) { (void)name; return NULL; }
const char *cfg_value(const char *name) { (void)name; return "0"; }
int cfg_bool(const char *name) {
    if (!strcmp(name, "NX2NAVI_WB_LAZY")) return 1;
    if (!strcmp(name, "NX2NAVI_WB_LAZY_CHUNK")) return getenv("CHECK_CHUNK") != NULL;
    if (!strcmp(name, "NX2NAVI_WB_LAZY_UNLOCKED")) return getenv("CHECK_LOCKED") == NULL;
    return 0;
}
void cfg_init(void) {}
int cfg_setenv(const char *name, const char *value) { (void)name, (void)value; return 0; }
int rt_knob(const char *name, const char *def) { (void)name; return strcmp(def, "0") != 0; }
int ldr_count(void) { return 0; }
const RtLoaded *ldr_module(int i) { (void)i; return NULL; }
uint64_t perf_mark_begin(void) { return 0; }
void perf_mark_end(int kind, uint64_t t0, uint64_t n) { (void)kind, (void)t0, (void)n; }
void perf_size(PerfSizes *s, const char *name, uint64_t count, uint64_t limit, uint64_t bytes) {
    (void)s, (void)name, (void)count, (void)limit, (void)bytes;
}
uint64_t res_heap_extra_bytes(void) { return 0; }
static char last_log[4096];
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof last_log, fmt, ap);
    va_end(ap);
    printf("  log: %s\n", last_log);
}
void rt_logf(const char *fmt, ...) { (void)fmt; }
void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("FATAL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
    _Exit(3);
}

// ---- the back end's stand-ins
static uint64_t heap;
static volatile uint64_t fetch_ns = 300000000ull, clean_ns;
static volatile int in_fetch, fetch_calls, fetch_copies, fetch_now, fetch_most, fetch_batch_most;
static volatile uint8_t marker = 0xA0;
static struct { uint64_t a, end; } ends[256];   // when the fetch that copied the range at a ended
static volatile int nends;
static void fake_fetch(const MemLazyCopy *c, int n) {
    int now = __atomic_add_fetch(&fetch_now, 1, __ATOMIC_SEQ_CST);
    for (int m = fetch_most; now > m && !__atomic_compare_exchange_n(&fetch_most, &m, now, 0, __ATOMIC_SEQ_CST,
                                                                      __ATOMIC_SEQ_CST);) {}
    for (int m = fetch_batch_most; n > m && !__atomic_compare_exchange_n(&fetch_batch_most, &m, n, 0, __ATOMIC_SEQ_CST,
                                                                         __ATOMIC_SEQ_CST);) {}
    __atomic_add_fetch(&fetch_calls, 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&fetch_copies, n, __ATOMIC_SEQ_CST);
    __atomic_store_n(&in_fetch, 1, __ATOMIC_SEQ_CST);
    plat_wait_ns(fetch_ns);
    uint64_t end = plat_now_ns();
    for (int i = 0; i < n; i++) {   // the GPU's copy writes past the protection; here the page is opened for it
        uint64_t a = c[i].h1 + c[i].off;
        plat_mem_protect(a & ~(PAGE - 1), PAGE, PLAT_PROT_RW);
        for (int j = 0; j < 64; j++) ((volatile uint8_t *)(uintptr_t)a)[j] = marker;
        plat_mem_protect(a & ~(PAGE - 1), PAGE, PLAT_PROT_NONE);
        int k = __atomic_fetch_add(&nends, 1, __ATOMIC_SEQ_CST);
        if (k < 256) ends[k].a = a, ends[k].end = end;
    }
    __atomic_sub_fetch(&fetch_now, 1, __ATOMIC_SEQ_CST);
}
static int fake_clean(uint64_t lo, uint64_t hi, void *ctx) {
    (void)lo, (void)hi, (void)ctx;
    if (clean_ns) plat_wait_ns(clean_ns);   // GetWriteWatch while the driver pins memory
    return 1;
}
// the end of the fetch that copied the range starting at a, 0 when none did
static uint64_t fetch_end(uint64_t a) {
    for (int k = 0; k < nends && k < 256; k++)
        if (ends[k].a == a) return ends[k].end;
    return 0;
}
static void wait_fetch(void) {
    while (!__atomic_load_n(&in_fetch, __ATOMIC_SEQ_CST)) plat_wait_ns(1000000);
}

// the range [a, a + n) of the heap, as written back from buffer h0 into the heap's own (h1 = heap, offset a - heap)
static uint64_t arm_buf(uint64_t a, uint64_t n, uint64_t h0) { return mem_lazy_arm(a, n, h0, heap, a - heap, NULL); }
// the replay leaves its writes to [a, a + n) in the mirror h0: the armed parts there are stale (a touch fetches them)
static int stale(uint64_t a, uint64_t n, uint64_t h0) {
    uint64_t out[16][2];
    return mem_lazy_skip(a, n, h0, -(int64_t)heap, out, 16);
}
static int armed(uint64_t a, uint64_t n) { return mem_lazy_armed(a, n); }
static void release(uint64_t lo, uint64_t hi, uint64_t h0) {
    uint64_t rg[1][3] = {{lo, hi, h0}};
    mem_lazy_release((const uint64_t (*)[3])rg, 1);
}
static int page_rw(uint64_t a) {
    uint64_t end;
    return plat_mem_state(a & ~(PAGE - 1), (a & ~(PAGE - 1)) + PAGE, &end) == PLAT_PROT_RW;
}
static double ms(uint64_t t0) { return (plat_now_ns() - t0) / 1e6; }
// the number after "what" in the last log line, -1 when it is not there
static double logged(const char *what) {
    const char *p = strstr(last_log, what);
    return p ? strtod(p + strlen(what), NULL) : -1;
}

// ---- threads that touch guest memory, as the game's CPU code does
typedef struct { uint64_t at; int write; volatile int done; uint8_t got; uint64_t end_ns; double took; } Touch;
static void toucher(void *arg) {
    Touch *t = arg;
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)t->at;
    uint64_t t0 = plat_now_ns();
    if (t->write) *p = 0x77;
    t->got = *p;
    t->end_ns = plat_now_ns();
    t->took = (t->end_ns - t0) / 1e6;
    __atomic_store_n(&t->done, 1, __ATOMIC_SEQ_CST);
}
static void *touch(Touch *t, uint64_t a, int write) {
    t->at = a, t->write = write, t->done = 0;
    return plat_thread_new(toucher, t, "toucher");
}
static volatile int spin_stop;
static volatile uint64_t spin_reads;
static void spinner(void *arg) {
    volatile uint8_t *p = arg;
    while (!__atomic_load_n(&spin_stop, __ATOMIC_SEQ_CST)) (void)*p, spin_reads++;
}
static uint64_t rel_rg[1][3];
static volatile int rel_done;
static void releaser(void *arg) {
    (void)arg;
    mem_lazy_release((const uint64_t (*)[3])rel_rg, 1);
    __atomic_store_n(&rel_done, 1, __ATOMIC_SEQ_CST);
}
// the completion thread's arm (vk_memory.c vk_gpu_written_back) of 64 KiB at arg, written back from buffer 1
static volatile uint64_t arm_n;
static void armer(void *arg) { arm_n = arm_buf((uint64_t)(uintptr_t)arg, 0x10000, 1); }
// a file read's thaw of the page at arg (fs_host.c, romfs.c: before the kernel writes the buffer)
static void thawer(void *arg) { mem_thaw((uint64_t)(uintptr_t)arg, 0x1000); }

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *what = argc > 1 ? argv[1] : "";
    mem_init();
    if (mem_set_heap_size(256ull << 20, &heap) != R_OK || !heap) return printf("SetHeapSize failed\n"), 2;
    if (!mem_lazy_enable(fake_fetch, fake_clean)) return printf("the lazy write-back is off\n"), 2;
    memset((void *)(uintptr_t)heap, 0x11, 256ull << 20);
    printf("%s: chunk %d, unlocked %d\n", what, lz_chunk, lz_unlocked);
    const uint64_t K64 = 0x10000, M = 1ull << 20;

    if (!strcmp(what, "arm_race") || !strcmp(what, "arm_race_slow")) {
        // A thread reads a page over and over while the completion thread's arm (vk_memory.c vk_gpu_written_back)
        // makes its range inaccessible and a release gives it back: no read may reach the crash handler. The slow one
        // has the clean test take 100 ms, as GetWriteWatch can while the driver pins memory, with the pages
        // inaccessible all along.
        uint64_t t = heap + 4 * M, r = t >> FRZ_SHIFT;
        void *th = plat_thread_new(spinner, (void *)(uintptr_t)(t + 0x100), "spinner");
        plat_wait_ns(20000000);
        int slow = !strcmp(what, "arm_race_slow");
        if (slow) clean_ns = 100000000;
        uint64_t n = 0;
        for (int i = 0; i < (slow ? 1 : 20000); i++) {
            plat_mutex_lock(&frz_lock);   // the hold a touch leaves, so that every turn arms again
            lz_hot[r] = 0;
            plat_mutex_unlock(&frz_lock);
            n += arm_buf(t, K64, 1) != 0;
            release(t, t + K64, 0);
        }
        spin_stop = 1;
        plat_thread_join(th);
        CHECK(n, "nothing was armed");
        printf("  %llu arms, %llu reads\n", (unsigned long long)n, (unsigned long long)spin_reads);
    } else if (!strcmp(what, "no_stall")) {
        // During a 300 ms fetch on another thread nothing below waits for it, and the range counts as one a read
        // would fault on.
        uint64_t s = heap + 8 * M, other = heap + 12 * M, frz_at = heap + 16 * M, thaw_at = heap + 20 * M;
        uint64_t rel_at = heap + 24 * M;
        CHECK(arm_buf(s, K64, 1) == K64 && stale(s, K64, 1) == 1, "arm");
        CHECK(arm_buf(rel_at, K64, 2) == K64, "arm of the range to release");
        Touch a;
        void *th = touch(&a, s + 0x10, 0);
        wait_fetch();
        uint64_t t0 = plat_now_ns();
        int f = mem_freeze(frz_at);
        double d = ms(t0);
        CHECK(d < 50 && f == 1, "mem_freeze of another region took %.1f ms (froze %d)", d, f);
        uint8_t buf[64], ok, *dst = buf;
        uint64_t src[1][2] = {{other + 0x40, 64}};
        t0 = plat_now_ns();
        int got = mem_copy_readable(&dst, (const uint64_t (*)[2])src, 1, &ok);
        d = ms(t0);
        CHECK(d < 50 && got == 1, "mem_copy_readable of another range took %.1f ms", d);
        t0 = plat_now_ns();
        uint64_t n = arm_buf(other, K64, 1);
        d = ms(t0);
        CHECK(d < 50 && n == K64, "an arm of another range took %.1f ms (%llu bytes)", d, (unsigned long long)n);
        t0 = plat_now_ns();
        mem_thaw(thaw_at, 0x1000);
        d = ms(t0);
        CHECK(d < 50, "mem_thaw of another region took %.1f ms", d);
        t0 = plat_now_ns();
        release(rel_at, rel_at + K64, 2);
        d = ms(t0);
        CHECK(d < 50 && !armed(rel_at, K64), "a release of another buffer took %.1f ms", d);
        CHECK(mem_lazy_touches(s + 0x20, 8) && !armed(s, K64), "the range in flight does not count as touched");
        CHECK(!a.done, "the fetch had ended before the checks");
        plat_thread_join(th);
        CHECK(a.got == marker, "the toucher read %#x, not the fetched bytes", a.got);
        CHECK(!mem_lazy_touches(s + 0x20, 8) && page_rw(s), "the range given back still counts as touched");
    } else if (!strcmp(what, "join")) {
        // B reads the range A is fetching, and with the fetch outside frz_lock this thread holds that lock all along:
        // both finish after one fetch, B after it, and the report line counts B's wait.
        uint64_t s = heap + 8 * M;
        CHECK(arm_buf(s, K64, 1) == K64 && stale(s, K64, 1) == 1, "arm");
        Touch a, b;
        void *ta = touch(&a, s + 0x10, 0);
        wait_fetch();
        if (lz_unlocked) plat_mutex_lock(&frz_lock);   // with NX2NAVI_WB_LAZY_UNLOCKED=0 the fetch holds it
        void *tb = touch(&b, s + 0x10, 0);
        for (int i = 0; i < 100 && !(a.done && b.done); i++) plat_wait_ns(10000000);
        int both = a.done && b.done;
        if (lz_unlocked) plat_mutex_unlock(&frz_lock);
        CHECK(both, "the fetch (%d) or the access that waited for it (%d) did not end", a.done, b.done);
        plat_thread_join(ta);
        plat_thread_join(tb);
        CHECK(fetch_calls == 1, "%d fetches", fetch_calls);
        CHECK(b.end_ns >= fetch_end(s), "B's read ended before the fetch did");
        CHECK(lz_joins == 1, "%llu joins", (unsigned long long)lz_joins);
        CHECK(a.got == marker && b.got == marker, "read %#x and %#x, not the fetched bytes", a.got, b.got);
        mem_lazy_report(1, 0);
        double w = logged("touches waited for a range another thread was fetching, the longest ");
        CHECK(w >= 150 && w < 400, "the report's longest wait for a range in flight read %.2f ms", w);
    } else if (!strcmp(what, "loser")) {
        // B writes the range A is fetching: one fetch, B waits for it alone, and the region's other range stays armed
        // (B went to thaw_fault before, which released every armed range of the region)
        fetch_ns = 200000000ull;
        uint64_t r = heap + 32 * M, s1 = r, s2 = r + M;
        CHECK(arm_buf(s1, K64, 1) == K64 && arm_buf(s2, K64, 1) == K64, "arms");
        stale(s1, K64, 1), stale(s2, K64, 1);
        Touch a, b;
        void *ta = touch(&a, s1 + 0x10, 0);
        wait_fetch();
        plat_wait_ns(20000000);
        void *tb = touch(&b, s1 + 0x20, 1);
        plat_thread_join(ta);
        plat_thread_join(tb);
        CHECK(fetch_calls == 1, "%d fetches", fetch_calls);
        CHECK(armed(s2, K64), "the region's other range was released");
        CHECK(lz_other == 0, "%llu other releases", (unsigned long long)lz_other);
        CHECK(b.took < 250 && b.got == 0x77, "the write waited %.1f ms and read back %#x", b.took, b.got);
    } else if (!strcmp(what, "late_write")) {
        // B writes a range whose flight starts and ends while B waits for frz_lock in lazy_fault (this thread holds the
        // lock and runs a release's flight under it, as a thaw does): B finds the page given back and writes again, and
        // the region's other range stays armed (B went on to thaw_fault before, whose thaw released it)
        uint64_t R = heap + 128 * M, s1 = R, s2 = R + M;
        CHECK(arm_buf(s1, K64, 1) == K64 && arm_buf(s2, K64, 1) == K64, "arms");
        stale(s1, K64, 1), stale(s2, K64, 1);
        fetch_ns = 20000000ull;
        plat_mutex_lock(&frz_lock);
        Touch b;
        void *tb = touch(&b, s1 + 0x20, 1);
        plat_wait_ns(50000000);   // B waits for frz_lock in lazy_fault, past its look for a range in flight
        int k[LZ_BATCH];
        LzSeg sg[LZ_BATCH];
        uint64_t rg[1][3] = {{s1, s1 + K64, 0}};
        plat_mutex_lock(&lz_lock);
        int nt = lazy_take_batch_locked((const uint64_t (*)[3])rg, 1, k, sg);
        plat_mutex_unlock(&lz_lock);
        if (nt > 0) lazy_fly_run(k, sg, nt);
        plat_mutex_unlock(&frz_lock);
        plat_thread_join(tb);
        CHECK(nt == 1 && b.got == 0x77, "%d taken out, the write read back %#x", nt, b.got);
        CHECK(armed(s2, K64) && lz_other == 0, "the region's other range was released (%llu other releases)",
              (unsigned long long)lz_other);
    } else if (!strcmp(what, "concurrent")) {
        // two threads touch two ranges at once: their fetches run at the same time
        uint64_t s1 = heap + 40 * M, s2 = heap + 48 * M;
        CHECK(arm_buf(s1, K64, 1) == K64 && arm_buf(s2, K64, 1) == K64, "arms");
        stale(s1, K64, 1), stale(s2, K64, 1);
        Touch a, b;
        uint64_t t0 = plat_now_ns();
        void *ta = touch(&a, s1, 0), *tb = touch(&b, s2, 1);
        plat_thread_join(ta);
        plat_thread_join(tb);
        double d = ms(t0);
        CHECK(fetch_most == 2 && d < 450, "at most %d fetches at once, %.1f ms in all", fetch_most, d);
    } else if (!strcmp(what, "thaw")) {
        // a thaw (a file read into the buffer) of a page in a region with a range in flight returns once that range's
        // pages are back, and fetches nothing again
        uint64_t s = heap + 56 * M;
        CHECK(arm_buf(s, K64, 1) == K64 && stale(s, K64, 1) == 1, "arm");
        Touch a;
        void *ta = touch(&a, s + 0x10, 0);
        wait_fetch();
        mem_thaw(s + 0x1000, 0x1000);
        uint64_t now = plat_now_ns(), fe = fetch_end(s);
        CHECK(fe && now >= fe, "mem_thaw returned before the fetch ended");
        CHECK(page_rw(s + 0x1000), "the page is not read-write after the thaw");
        plat_thread_join(ta);
        CHECK(fetch_calls == 1, "%d fetches", fetch_calls);
    } else if (!strcmp(what, "thaw_unlocked")) {
        // A thaw (a file read into a buffer) of a range armed with stale bytes fetches them before it takes frz_lock
        // (memory.c lazy_release_regions): a freeze of another region meanwhile does not wait for the copy. With
        // NX2NAVI_WB_LAZY_UNLOCKED=0 the thaw fetches under the lock, and the freeze waits.
        uint64_t s = heap + 124 * M, frz_at = heap + 16 * M;
        CHECK(arm_buf(s, K64, 1) == K64 && stale(s, K64, 1) == 1, "arm");
        void *th = plat_thread_new(thawer, (void *)(uintptr_t)(s + 0x1000), "thawer");
        wait_fetch();
        uint64_t t0 = plat_now_ns();
        int f = mem_freeze(frz_at);
        double d = ms(t0);
        if (lz_unlocked) CHECK(d < 50 && f == 1, "mem_freeze during the thaw's fetch took %.1f ms", d);
        else CHECK(d > 200, "mem_freeze took %.1f ms, not the fetch's time", d);
        plat_thread_join(th);
        CHECK(fetch_calls == 1 && page_rw(s) && !armed(s, K64), "%d fetches, pages %d", fetch_calls, page_rw(s));
    } else if (!strcmp(what, "mapping")) {
        // a mapping change (a commit, decommit or protect: thaw_range_locked under frz_lock) in a region with a range
        // in flight and another armed: the armed one is fetched under the lock, the flight ends first, both given back
        uint64_t r = heap + 60 * M, s1 = r, s2 = r + M;
        CHECK(arm_buf(s1, K64, 1) == K64 && arm_buf(s2, K64, 1) == K64, "arms");
        stale(s1, K64, 1), stale(s2, K64, 1);
        Touch a;
        void *ta = touch(&a, s1 + 0x10, 0);
        wait_fetch();
        plat_mutex_lock(&frz_lock);
        thaw_wait_locked(r, FRZ_SIZE);
        thaw_range_locked(r, FRZ_SIZE, 3);
        uint64_t now = plat_now_ns();
        int rw = page_rw(s1) && page_rw(s2), mark = lzr[r >> FRZ_SHIFT];
        plat_mutex_unlock(&frz_lock);
        CHECK(fetch_end(s1) && now >= fetch_end(s1), "the mapping change went ahead of the range in flight");
        CHECK(fetch_end(s2) && rw && !mark && !armed(s2, K64), "the region is not settled (pages %d, mark %d)", rw,
              mark);
        plat_thread_join(ta);
        CHECK(fetch_calls == 2, "%d fetches", fetch_calls);
    } else if (!strcmp(what, "release")) {
        // A release of one buffer's ranges (a mirror eviction): one fetch of its two armed stale segments, and it
        // returns after the fetch another thread runs of its third; the other buffer's range stays armed. With
        // NX2NAVI_WB_LAZY_UNLOCKED=0 each segment is fetched on its own, as in 0.10.1.
        uint64_t base = heap + 64 * M;
        uint64_t x1 = base, x2 = base + M, x3 = base + 3 * M, y = base + 2 * M;
        CHECK(arm_buf(x1, K64, 1) == K64 && arm_buf(x2, K64, 1) == K64 && arm_buf(x3, K64, 1) == K64, "arms of X");
        CHECK(arm_buf(y, K64, 7) == K64, "arm of Y");
        stale(x1, K64, 1), stale(x2, K64, 1), stale(x3, K64, 1), stale(y, K64, 7);
        Touch a;
        void *ta = touch(&a, x3 + 0x10, 0);   // in flight on another thread
        wait_fetch();
        release(base, base + 4 * M, 1);
        uint64_t now = plat_now_ns(), fe = fetch_end(x3);
        CHECK(fe && now >= fe, "the release returned before the other thread's fetch of the buffer ended");
        CHECK(!armed(x1, K64) && !armed(x2, K64) && page_rw(x1) && page_rw(x2), "X still armed");
        CHECK(armed(y, K64), "Y was released");
        CHECK(fetch_batch_most == (lz_unlocked ? 2 : 1), "the release's fetch had %d copies", fetch_batch_most);
        plat_thread_join(ta);
        CHECK(fetch_calls == (lz_unlocked ? 2 : 3) && fetch_copies == 3, "%d fetches of %d copies", fetch_calls,
              fetch_copies);
    } else if (!strcmp(what, "release_join")) {
        // a thread writes a range a release (a mirror eviction) is fetching: it waits for that fetch, its write
        // survives, one fetch in all
        uint64_t x = heap + 88 * M;
        CHECK(arm_buf(x, K64, 3) == K64 && stale(x, K64, 3) == 1, "arm");
        rel_rg[0][0] = x, rel_rg[0][1] = x + K64, rel_rg[0][2] = 3;
        void *tr = plat_thread_new(releaser, NULL, "releaser");
        wait_fetch();
        Touch b;
        void *tb = touch(&b, x + 0x20, 1);
        plat_thread_join(tr);
        plat_thread_join(tb);
        CHECK(rel_done && fetch_calls == 1, "%d fetches", fetch_calls);
        CHECK(b.end_ns >= fetch_end(x) && b.got == 0x77, "the write ended before the fetch, or was lost (%#x)", b.got);
        CHECK(lz_joins == 1 && !armed(x, K64), "%llu joins", (unsigned long long)lz_joins);
    } else if (!strcmp(what, "arm_busy")) {
        // an arm of a range in flight (its region's hold cleared, as when the range was released, not touched) leaves
        // it alone: its pages are given back by the flight, and a later arm takes it again
        uint64_t s = heap + 92 * M, r = s >> FRZ_SHIFT;
        CHECK(arm_buf(s, K64, 1) == K64 && stale(s, K64, 1) == 1, "arm");
        rel_rg[0][0] = s, rel_rg[0][1] = s + K64, rel_rg[0][2] = 0;
        void *tr = plat_thread_new(releaser, NULL, "releaser");
        wait_fetch();
        plat_mutex_lock(&frz_lock);
        int held = lz_hot[r] != 0;
        plat_mutex_unlock(&frz_lock);
        uint64_t n = arm_buf(s, K64, 1);
        CHECK(!held && n == 0 && lzr[r], "an arm of the range in flight armed %llu bytes (hold %d, mark %d)",
              (unsigned long long)n, held, lzr[r]);
        plat_thread_join(tr);
        CHECK(page_rw(s) && !lzr[r], "the range in flight was not given back");
        CHECK(arm_buf(s, K64, 1) == K64 && armed(s, K64), "the range was not armed again after its flight");
    } else if (!strcmp(what, "arm_flight")) {
        // A range in flight (a release, so no hold) in region r ends while an arm of another range in r is in its
        // clean test: the region keeps its mark (memory.c lz_arm_r), so a read of the arming range that faults after
        // that end waits for the arm instead of reaching the crash handler.
        uint64_t R = heap + 120 * M, s1 = R, s2 = R + M;
        CHECK(arm_buf(s1, K64, 1) == K64 && stale(s1, K64, 1) == 1, "arm s1");
        fetch_ns = 100000000ull;
        rel_rg[0][0] = s1, rel_rg[0][1] = s1 + K64, rel_rg[0][2] = 0;
        void *tr = plat_thread_new(releaser, NULL, "releaser");
        wait_fetch();
        clean_ns = 400000000ull;
        void *tc = plat_thread_new(armer, (void *)(uintptr_t)s2, "armer");
        while (!__atomic_load_n(&rel_done, __ATOMIC_SEQ_CST)) plat_wait_ns(1000000);
        plat_wait_ns(30000000);   // the flight has ended; the arm is in its clean test
        Touch b;
        void *tb = touch(&b, s2 + 0x10, 0);
        plat_thread_join(tr);
        plat_thread_join(tc);
        plat_thread_join(tb);
        CHECK(arm_n == K64 && b.done, "armed %llu, read done %d", (unsigned long long)arm_n, b.done);
    } else if (!strcmp(what, "foreign")) {
        // a write by the replay to a range armed for another buffer (a mirror gone since) gives it back without a
        // fetch: that buffer may be gone
        uint64_t s = heap + 100 * M;
        CHECK(arm_buf(s, K64, 1) == K64 && stale(s, K64, 1) == 1, "arm");
        CHECK(stale(s, K64, 2) == 0, "the range armed for another buffer was marked");
        CHECK(fetch_calls == 0 && !armed(s, K64) && page_rw(s) && !lzr[s >> FRZ_SHIFT] && lz_foreign == 1,
              "%d fetches, armed %d, read-write %d", fetch_calls, armed(s, K64), page_rw(s));
    } else if (!strcmp(what, "chunk")) {
        // NX2NAVI_WB_LAZY_CHUNK=1: lz_lock is free during a piece's fetch; the rest of the range stays armed
        uint64_t s = heap + 72 * M, other = heap + 76 * M;
        CHECK(arm_buf(s, M, 1) == M && stale(s, M, 1) == 1, "arm");
        Touch a;
        void *ta = touch(&a, s + 0x10, 0);
        wait_fetch();
        uint64_t t0 = plat_now_ns();
        int am = armed(other, 0x1000);
        double d = ms(t0);
        CHECK(d < 50 && !am, "mem_lazy_armed took %.1f ms", d);
        t0 = plat_now_ns();
        int k = stale(s + M / 2, 0x1000, 1);
        d = ms(t0);
        CHECK(d < 50 && k == 1, "mem_lazy_skip in the same range took %.1f ms (%d)", d, k);
        plat_thread_join(ta);
        CHECK(armed(s + M / 2, 0x1000) && !armed(s, 0x1000) && page_rw(s), "the piece and the rest");
    } else if (!strcmp(what, "locked")) {
        // NX2NAVI_WB_LAZY_UNLOCKED=0: the fetch holds frz_lock as before
        uint64_t s = heap + 8 * M, frz_at = heap + 16 * M;
        CHECK(arm_buf(s, K64, 1) == K64 && stale(s, K64, 1) == 1, "arm");
        Touch a;
        void *th = touch(&a, s + 0x10, 0);
        wait_fetch();
        uint64_t t0 = plat_now_ns();
        mem_freeze(frz_at);
        double d = ms(t0);
        CHECK(d > 200, "mem_freeze took %.1f ms, not the fetch's time", d);
        plat_thread_join(th);
    } else if (!strcmp(what, "report")) {
        // The report line gives the longest fetch since the last line, and the longest wait of a fault for frz_lock;
        // the next line starts from nothing.
        uint64_t s1 = heap + 80 * M, s2 = heap + 84 * M, s3 = heap + 104 * M;
        CHECK(arm_buf(s1, K64, 1) == K64 && arm_buf(s2, K64, 1) == K64 && arm_buf(s3, K64, 1) == K64, "arms");
        CHECK(stale(s1, K64, 1) == 1 && stale(s2, K64, 1) == 1 && stale(s3, K64, 1) == 1, "stale marks");
        Touch a;
        fetch_ns = 30000000ull;
        plat_thread_join(touch(&a, s1, 0));
        fetch_ns = 120000000ull;
        plat_thread_join(touch(&a, s2, 0));
        fetch_ns = 10000000ull;
        plat_mutex_lock(&frz_lock);   // another thread's thaw, freeze or arm, 60 ms long
        void *th = touch(&a, s3, 1);
        plat_wait_ns(60000000);
        plat_mutex_unlock(&frz_lock);
        plat_thread_join(th);
        CHECK(fetch_calls == 3 && a.got == 0x77, "%d fetches, the write read back %#x", fetch_calls, a.got);
        mem_lazy_report(1, 0);
        double longest = logged("the longest "), waited = logged("faults waited up to ");
        CHECK(longest >= 119 && longest < 200, "the longest fetch read %.2f ms", longest);
        CHECK(waited >= 40 && waited < 200, "the longest wait for the lock read %.2f ms", waited);
        mem_lazy_report(2, 0);
        CHECK(logged("the longest ") == 0 && logged("faults waited up to ") == 0, "the next line did not start over");
    } else if (!strcmp(what, "flood")) {
        // more threads than entries touch a range each at once: those past LZ_FLY wait for room, and all end
        enum { N = 80 };
        fetch_ns = 200000000ull;
        static Touch t[N];
        void *th[N];
        for (int i = 0; i < N; i++) {
            uint64_t s = heap + 96 * M + (uint64_t)i * M;
            CHECK(arm_buf(s, K64, 1) == K64 && stale(s, K64, 1) == 1, "arm %d", i);
        }
        for (int i = 0; i < N; i++) th[i] = touch(&t[i], heap + 96 * M + (uint64_t)i * M, i & 1);
        for (int i = 0; i < N; i++) plat_thread_join(th[i]);
        int bad = 0;
        for (int i = 0; i < N; i++) bad += t[i].got != (i & 1 ? 0x77 : marker);
        CHECK(fetch_copies == N && lz_fly_most == LZ_FLY && !bad && !lz_nfly,
              "%d copies, %d in flight at most, %d wrong reads, %d left in flight", fetch_copies, lz_fly_most, bad,
              (int)lz_nfly);
    } else if (!strcmp(what, "freeze")) {
        // a region with a range in flight is not frozen
        uint64_t s = heap + 8 * M;
        CHECK(arm_buf(s, K64, 1) == K64 && stale(s, K64, 1) == 1, "arm");
        Touch a;
        void *th = touch(&a, s + 0x10, 0);
        wait_fetch();
        CHECK(mem_freeze(s) == 0, "a region with a range in flight was frozen");
        plat_thread_join(th);
    } else {
        printf("unknown scenario %s\n", what);
        return 2;
    }
    if (fails) return printf("%s: %d checks failed\n", what, fails), 1;
    printf("%s: all checks passed\n", what);
    return 0;
}
