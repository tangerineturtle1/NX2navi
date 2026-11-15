// runtime/memory.c mem_copy_readable with the lazy heap (NX2NAVI_HEAP_LAZY=1): a heap page nothing touched yet is
// still reserved, and the copy holds frz_lock, which the fault on such a page (heap_lazy_fault) waits for on the same
// thread. Built and run by tests/heap_lazy_copy_check.py with the Windows platform layer and the real fault handler;
// the rest of the runtime is stubbed. Prints each failed check and exits 1 on any, 4 when a copy never returns.
// Each copy runs on a worker the main thread gives 3 s: before the fix the first one hung there.
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
int cfg_bool(const char *name) { return !strcmp(name, "NX2NAVI_HEAP_LAZY"); }
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
void rt_log(const char *fmt, ...) { (void)fmt; }
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

// ---- one copy on a worker, as the replay (vk_memory.c mirror_capture) or a present (frame_rate.c) makes it
static struct { uint64_t a, len; uint8_t *dst, ok; int got; volatile int done; } J;
static void copier(void *arg) {
    (void)arg;
    uint64_t src[1][2] = {{J.a, J.len}};
    J.got = mem_copy_readable(&J.dst, (const uint64_t (*)[2])src, 1, &J.ok);
    __atomic_store_n(&J.done, 1, __ATOMIC_SEQ_CST);
}
static void copy(uint64_t a, uint64_t len, uint8_t *dst, const char *what) {
    J.a = a, J.len = len, J.dst = dst, J.ok = 0xEE, J.got = -1, J.done = 0;
    void *t = plat_thread_new(copier, NULL, "copier");
    for (int i = 0; i < 300 && !__atomic_load_n(&J.done, __ATOMIC_SEQ_CST); i++) plat_wait_ns(10000000);
    if (!J.done) {
        printf("FAIL: %s: the copy has not returned after 3 s (frz_lock held by the copying thread)\n", what);
        fflush(stdout);
        _Exit(4);
    }
    plat_thread_join(t);
}
static int all(const uint8_t *p, uint64_t n, uint8_t v) {
    for (uint64_t i = 0; i < n; i++)
        if (p[i] != v) return 0;
    return 1;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    mem_init();
    uint64_t heap = 0, size = 64ull << 20, y;
    CHECK(mem_set_heap_size(size, &heap) == R_OK && heap, "SetHeapSize failed");
    CHECK(heap_lazy, "the lazy heap is off");
    static uint8_t buf[0x30000];

    // ---- a page nothing touched: copied as zeros, and left reserved
    uint64_t a = heap + 0x30000;
    CHECK(plat_mem_state(a, a + PAGE, &y) == PLAT_MEM_RESERVED, "the untouched page is not reserved");
    memset(buf, 0xAB, 64);
    copy(a, 64, buf, "an untouched heap page");
    CHECK(J.got == 1 && J.ok == 1 && all(buf, 64, 0), "an untouched heap page: returned %d, ok %d, bytes %#x",
          J.got, J.ok, buf[0]);
    CHECK(plat_mem_state(a, a + PAGE, &y) == PLAT_MEM_RESERVED, "the copy committed the untouched page");

    // ---- a range from touched pages into untouched ones: the written bytes, then zeros
    uint64_t t = heap + 0x100000;   // a 64 KiB run committed by a host access, then written
    CHECK(mem_host_readable(t, 0x10000), "the heap page could not be made readable");
    memset((void *)(uintptr_t)t, 0x5A, 0x10000);
    CHECK(plat_mem_state(t + 0x10000, t + 0x10000 + PAGE, &y) == PLAT_MEM_RESERVED, "the next run is not reserved");
    memset(buf, 0xAB, sizeof buf);
    copy(t + 0x8000, 0x20000, buf, "touched then untouched heap pages");
    CHECK(J.got == 1 && J.ok == 1 && all(buf, 0x8000, 0x5A) && all(buf + 0x8000, 0x18000, 0),
          "touched then untouched heap pages: returned %d, ok %d, bytes %#x %#x", J.got, J.ok, buf[0], buf[0x8000]);

    // ---- past the heap's end the pages are not the heap's: left out, without a fault
    memset(buf, 0xAB, 64);
    copy(heap + size - 32, 64, buf, "a range past the heap's end");
    CHECK(J.got == 0 && J.ok == 0 && all(buf, 64, 0xAB), "a range past the heap's end: returned %d, ok %d", J.got, J.ok);

    // ---- several ranges in one call, as frame_rate.c batches them
    uint8_t d0[16], d1[16], d2[16], oks[3];
    uint8_t *dst[3] = {d0, d1, d2};
    uint64_t src[3][2] = {{t, 16}, {a + 0x100, 16}, {heap + size - 8, 16}};
    memset(d0, 0, 16), memset(d1, 0xAB, 16), memset(d2, 0xAB, 16);
    int got = mem_copy_readable(dst, (const uint64_t (*)[2])src, 3, oks);
    CHECK(got == 2 && oks[0] && oks[1] && !oks[2] && all(d0, 16, 0x5A) && all(d1, 16, 0),
          "three ranges: returned %d, ok %d %d %d", got, oks[0], oks[1], oks[2]);

    if (fails) return printf("heap_lazy_copy_check: %d checks failed\n", fails), 1;
    printf("heap_lazy_copy_check: all checks passed\n");
    return 0;
}
