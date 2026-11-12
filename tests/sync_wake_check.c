// runtime/kernel.c: which WaitSynchronization waits a signalled event or an ended thread wakes (wake_sync_waiters),
// on guest threads whose entries are host functions, with the rest of the runtime stubbed. Built and run by
// tests/sync_wake_check.py; prints each failed check and exits 1 on any. With the argument "all" it runs with
// NX2NAVI_SYNC_WAKE_ALL=1 and checks that every wait is woken and blocks again.
#include "kernel.c"

#include <stdarg.h>

static int fails, wake_all;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- the runtime kernel.c reaches
unsigned rt_trace;
uint64_t rt_guest_start_ns;
volatile uint32_t prof_replay_tid;
const char *cfg_get(const char *name) { return !strcmp(name, "NX2NAVI_SYNC_WAKE_ALL") && wake_all ? "1" : NULL; }
const char *cfg_value(const char *name) { (void)name; return "0"; }   // no asynchronous preemption, no slow-service log
int cfg_bool(const char *name) { (void)name; return 0; }
int rt_ab(void) { return 0; }
int rt_knob(const char *name, const char *def) {
    char key[64];
    snprintf(key, sizeof key, "NX2NAVI_%s", name);
    const char *e = cfg_get(key);
    return strcmp(e && *e ? e : def, "0") != 0;
}
int64_t frame_step_world_us(void) { return -1; }
void ipc_handle_closed(int kind, void *obj) { (void)kind, (void)obj; }
const char *ldr_describe(uint64_t a, char *buf, int n) { snprintf(buf, (size_t)n, "%#llx", (unsigned long long)a); return buf; }
uint64_t ldr_symbol(const char *name) { (void)name; return 0; }
void perf_main_thread(void) {}
void perf_size(PerfSizes *s, const char *name, uint64_t count, uint64_t limit, uint64_t bytes) {
    (void)s, (void)name, (void)count, (void)limit, (void)bytes;   // kern_sizes is not called here
}
int rt_linked_modules(RtModule *out, int max) { (void)out, (void)max; return 0; }
const RtGuestLayout *mem_layout(void) { return NULL; }
void *mem_kernel_alloc(uint64_t size, uint64_t align) {
    void *p = plat_aligned_alloc(size, align < 64 ? 64 : align);
    if (!p) rt_fatal("out of memory");
    memset(p, 0, size);
    return p;
}
void mem_kernel_free(void *p, uint64_t size) { (void)size; plat_aligned_free(p); }
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
void rt_dispatch(Cpu *c) { (void)c; rt_fatal("rt_dispatch in the test"); }
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) { (void)pc; kern_svc(c, imm); }
// A guest entry here is a host function taking the thread's Cpu, with its argument in x0.
uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...) {
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) c->x[i] = va_arg(ap, uint64_t);
    va_end(ap);
    ((void (*)(Cpu *))(uintptr_t)fn)(c);
    return c->x[0];
}

// ---- waiters
typedef struct {
    uint32_t hs[2];
    int n, loop;                // loop: clear the event and wait again after each wake, counting in `wakes`
    int64_t ns;
    volatile uint32_t res, thread;
    volatile int idx, done, wakes;
} Wait;

static void waiter(Cpu *c) {
    Wait *w = (Wait *)(uintptr_t)c->x[0];
    do {
        c->x[1] = (uint64_t)(uintptr_t)w->hs, c->x[2] = (uint64_t)w->n, c->x[3] = (uint64_t)w->ns;
        kern_svc(c, 0x18);
        w->res = (uint32_t)c->x[0], w->idx = (int)c->x[1];
        if (w->loop && !w->res) {
            c->x[0] = w->hs[0];
            kern_svc(c, 0x12);   // ClearEvent on the readable end
            __atomic_add_fetch(&w->wakes, 1, __ATOMIC_SEQ_CST);
        }
    } while (w->loop > 0 && !w->res);
    __atomic_store_n(&w->done, 1, __ATOMIC_SEQ_CST);
    kern_svc(c, 0x0A);
    rt_fatal("ExitThread returned");
}

static uint32_t spawn(Wait *w) {
    Cpu c = {0};
    c.x[1] = (uint64_t)(uintptr_t)waiter, c.x[2] = (uint64_t)(uintptr_t)w;
    c.x[3] = (uint64_t)(uintptr_t)mem_kernel_alloc(0x4000, 16) + 0x4000;
    c.x[4] = 44, c.x[5] = (uint64_t)(int64_t)-2;
    kern_svc(&c, 0x08);
    if (c.x[0]) rt_fatal("CreateThread failed %#llx", (unsigned long long)c.x[0]);
    w->thread = (uint32_t)c.x[1];
    c.x[0] = w->thread;
    kern_svc(&c, 0x09);
    return w->thread;
}

static int until(volatile int *flag, int want, int ms) {
    for (int i = 0; i < ms * 10; i++) {
        if (__atomic_load_n(flag, __ATOMIC_SEQ_CST) == want) return 1;
        plat_wait_ns(100000);
    }
    return 0;
}

static int in_sync(uint32_t thread, int ms) {
    for (int i = 0; i < ms * 10; i++) {
        if (kern_thread_state(thread) == KERN_WAIT_SYNC) return 1;
        plat_wait_ns(100000);
    }
    return 0;
}

static void counts(uint64_t *woken, uint64_t *spared, uint64_t *blocks) {
    lock();
    *woken = g_sync_woken, *spared = g_sync_spared, *blocks = g_sync_blocks;
    unlock();
}

// Waits blocked now: those woken for nothing are back in their wait.
static void settle(Wait **ws, int n) {
    plat_wait_ns(20000000);
    for (int i = 0; i < n; i++) CHECK(in_sync(ws[i]->thread, 2000), "waiter %d is not back in its wait", i);
}

int main(int argc, char **argv) {
    wake_all = argc > 1 && !strcmp(argv[1], "all");
    setvbuf(stdout, NULL, _IONBF, 0);
    uint32_t ea = kern_event_new(), eb = kern_event_new(), ec = kern_event_new(), ed = kern_event_new();
    uint32_t ra = kern_event_readable(ea), rb = kern_event_readable(eb), rc = kern_event_readable(ec);
    uint64_t wk, sp, bl;

    // 1. A waits on a, B on b, C on b and c. A signal of a wakes A; B and C stay in their waits.
    Wait A = {.hs = {ra}, .n = 1, .ns = -1}, B = {.hs = {rb}, .n = 1, .ns = -1}, C = {.hs = {rb, rc}, .n = 2, .ns = -1};
    spawn(&A), spawn(&B), spawn(&C);
    CHECK(in_sync(A.thread, 2000) && in_sync(B.thread, 2000) && in_sync(C.thread, 2000), "the waiters did not block");
    kern_event_signal(ea);
    CHECK(until(&A.done, 1, 2000) && A.res == 0 && A.idx == 0, "A: result %#x index %d", A.res, A.idx);
    settle((Wait *[]){&B, &C}, 2);
    CHECK(!B.done && !C.done, "a signal of another event ended a wait");
    // Two wakes pass B and C by: the signal of a, and the end of A's thread.
    counts(&wk, &sp, &bl);
    if (wake_all) CHECK(wk == 5 && sp == 0 && bl == 7, "all: woken %llu left %llu blocks %llu, want 5 0 7",
                        (unsigned long long)wk, (unsigned long long)sp, (unsigned long long)bl);
    else CHECK(wk == 1 && sp == 4 && bl == 3, "woken %llu left %llu blocks %llu, want 1 4 3",
               (unsigned long long)wk, (unsigned long long)sp, (unsigned long long)bl);

    // 2. A wait on several handles is woken by its second one and reports its index.
    kern_event_signal(ec);
    CHECK(until(&C.done, 1, 2000) && C.res == 0 && C.idx == 1, "C: result %#x index %d", C.res, C.idx);
    CHECK(in_sync(B.thread, 2000) && !B.done, "B left its wait at a signal of c");

    // 3. A thread that ends wakes the wait on its handle: E waits for B's thread, and b ends B.
    Wait E = {.hs = {B.thread}, .n = 1, .ns = -1};
    spawn(&E);
    CHECK(in_sync(E.thread, 2000), "E did not block");
    kern_event_signal(eb);
    CHECK(until(&B.done, 1, 2000) && B.res == 0 && B.idx == 0, "B: result %#x index %d", B.res, B.idx);
    CHECK(until(&E.done, 1, 2000) && E.res == 0 && E.idx == 0, "E: the end of B's thread did not end its wait (result %#x)", E.res);

    // 4. A timed wait keeps its time across other events' signals; a wait without handles is a sleep.
    kern_event_clear(ea);
    Wait F = {.hs = {ra}, .n = 1, .ns = 150000000}, Z = {.n = 0, .ns = 150000000};
    uint64_t t0 = kern_now_ns();
    spawn(&F), spawn(&Z);
    CHECK(in_sync(F.thread, 2000) && in_sync(Z.thread, 2000), "F and Z did not block");
    for (int i = 0; i < 20; i++) kern_event_signal(ed), plat_wait_ns(1000000);
    CHECK(until(&F.done, 1, 2000) && F.res == R_TIMEOUT, "F: result %#x, want a timeout", F.res);
    CHECK(until(&Z.done, 1, 2000) && Z.res == R_TIMEOUT, "Z: result %#x, want a timeout", Z.res);
    CHECK(kern_now_ns() - t0 >= 150000000, "a timed wait ended after %.1f ms, before its 150 ms", (kern_now_ns() - t0) / 1e6);

    // 5. CancelSynchronization ends a wait whatever it waits on.
    Wait G = {.hs = {ra}, .n = 1, .ns = -1};
    spawn(&G);
    CHECK(in_sync(G.thread, 2000), "G did not block");
    Cpu c = {0};
    c.x[0] = G.thread;
    kern_svc(&c, 0x19);
    CHECK(until(&G.done, 1, 2000) && G.res == R_CANCELLED, "G: result %#x, want cancelled", G.res);

    // 6. No signal is lost: eight threads each wait on their own event in a loop; every signal is seen by its thread.
    enum { N = 8, ROUNDS = 4000 };
    static Wait L[N];
    uint32_t ew[N];
    for (int i = 0; i < N; i++) {
        ew[i] = kern_event_new();
        L[i] = (Wait){.hs = {kern_event_readable(ew[i])}, .n = 1, .ns = -1, .loop = 1};
        spawn(&L[i]);
    }
    unsigned seed = 12345;
    int lost = 0;
    for (int r = 0; r < ROUNDS && !lost; r++) {
        seed = seed * 1664525u + 1013904223u;
        int i = (int)(seed >> 24) % N;
        int before = __atomic_load_n(&L[i].wakes, __ATOMIC_SEQ_CST);
        kern_event_signal(ew[i]);
        uint64_t end = kern_now_ns() + 5000000000ull;
        while (__atomic_load_n(&L[i].wakes, __ATOMIC_SEQ_CST) == before && kern_now_ns() < end) plat_yield();
        if (__atomic_load_n(&L[i].wakes, __ATOMIC_SEQ_CST) == before) {
            lost = 1;
            CHECK(0, "round %d: thread %d never saw its signal", r, i);
        }
    }
    int total = 0;
    for (int i = 0; i < N; i++) total += L[i].wakes;
    CHECK(lost || total == ROUNDS, "%d wakes for %d signals", total, ROUNDS);
    for (int i = 0; i < N; i++) {   // end them: the last wake is not followed by another wait
        L[i].loop = -1;
        kern_event_signal(ew[i]);
        CHECK(until(&L[i].done, 1, 5000), "looping waiter %d did not end", i);
    }
    counts(&wk, &sp, &bl);
    printf("sync_wake_check%s: %s (signals woke %llu waits and left %llu asleep; %llu blocks)\n", wake_all ? " all" : "",
           fails ? "FAILED" : "ok", (unsigned long long)wk, (unsigned long long)sp, (unsigned long long)bl);
    return fails ? 1 : 0;
}
