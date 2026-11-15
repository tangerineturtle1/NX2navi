// runtime/kernel.c: the system-event table (kern_bind_system_event, kern_signal_system_event, kern_clear_system_event)
// with the platform layer linked and the rest of the runtime stubbed. Built and run by tests/system_event_check.py;
// prints each failed check and exits 1 on any.
//   (no argument)  a kept handle the guest closed (DestroySystemEvent) is neither signalled nor cleared, also when its
//                  number went to an unrelated event or the state byte reads nonzero again; a host thread signalling
//                  while a guest thread destroys and binds the event again never ends the process; with the table full,
//                  a new bind takes a destroyed guest-memory entry and keeps the entries in host memory, which a host
//                  thread then still signals; kern_system_event_live tells live guest events from destroyed ones;
//                  kern_unbind_system_event destroys a host-memory event and frees its entry, so a full table takes a
//                  new guest bind once the host entries are unbound
//   full           every entry live: the next bind ends with "table full" (the script checks the exit code and text)
#include "kernel.c"

#include <stdarg.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- the runtime kernel.c reaches
unsigned rt_trace;
uint64_t rt_guest_start_ns;
volatile uint32_t prof_replay_tid, prof_main_tid;
// Guest memory is this block; anything else (the "host-memory" events below) is not, as memory.c answers.
static uint8_t guest[600][0x40] __attribute__((aligned(16)));
int mem_readable(uint64_t a, uint64_t n) {
    uint64_t lo = (uint64_t)(uintptr_t)guest, hi = lo + sizeof guest;
    return a >= lo && a + n <= hi;
}
const char *cfg_get(const char *name) { (void)name; return NULL; }
const char *cfg_value(const char *name) { (void)name; return "0"; }
int cfg_bool(const char *name) { (void)name; return 0; }
int rt_ab(void) { return 0; }
int rt_knob(const char *name, const char *def) { (void)name; return strcmp(def, "0") != 0; }
int64_t frame_step_world_us(void) { return -1; }
void ipc_handle_closed(int kind, void *obj) { (void)kind, (void)obj; }
const char *ldr_describe(uint64_t a, char *buf, int n) { snprintf(buf, (size_t)n, "%#llx", (unsigned long long)a); return buf; }
// The SDK's functions the table calls: 1 CreateSystemEvent, 2 GetWritableHandleOfSystemEvent (kern_bind_system_event),
// 3 DestroySystemEvent (kern_unbind_system_event).
uint64_t ldr_symbol(const char *name) {
    return strstr(name, "CreateSystemEvent") ? 1 : strstr(name, "DestroySystemEvent") ? 3 : 2;
}
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
static void destroy(void *p);
// CreateSystemEvent(ev, mode, ipc) makes a kernel event, keeps its writable handle at +0 of the SystemEventType and its
// readable one at +4, and sets the state byte; GetWritableHandleOfSystemEvent(ev) reads +0; DestroySystemEvent(ev) as
// destroy below.
uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...) {
    (void)c;
    va_list ap;
    va_start(ap, n);
    uint64_t ev = va_arg(ap, uint64_t);
    va_end(ap);
    uint8_t *p = (uint8_t *)(uintptr_t)ev;
    if (fn == 1) {
        uint32_t wh = kern_event_new(), rh = kern_event_readable(wh);
        memcpy(p, &wh, 4);
        memcpy(p + 4, &rh, 4);
        __atomic_store_n(p + SE_STATE, 1, __ATOMIC_RELEASE);
        return 0;
    }
    if (fn == 3) {
        destroy(p);
        return 0;
    }
    uint32_t wh;
    memcpy(&wh, p, 4);
    return wh;
}

// ---- helpers
static RtThread me;   // the main thread plays a guest thread for the binds
static Cpu cpu;
static uint64_t ev_of(void *p) { return (uint64_t)(uintptr_t)p; }
static uint32_t wh_of(const void *p) { uint32_t h; memcpy(&h, p, 4); return h; }
static uint32_t rh_of(const void *p) { uint32_t h; memcpy(&h, (const uint8_t *)p + 4, 4); return h; }
static void bind(void *p) { kern_bind_system_event(&cpu, ev_of(p), 0); }
// nn::os::DestroySystemEvent: the state byte first, then both handles (SVC 0x16).
static void destroy(void *p) {
    __atomic_store_n((uint8_t *)p + SE_STATE, 0, __ATOMIC_RELEASE);
    kern_handle_close(rh_of(p));
    kern_handle_close(wh_of(p));
}
static int signaled(uint32_t wh) {
    lock();
    Event *e = h_peek(wh, O_EVENT_W);
    int s = e ? e->signaled : -1;
    unlock();
    return s;
}

static void stale_and_reuse(void) {
    uint8_t *ev = guest[0];
    bind(ev);
    uint32_t wh = wh_of(ev);
    kern_signal_system_event(ev_of(ev));
    CHECK(signaled(wh) == 1, "a live bound event is signalled");
    kern_clear_system_event(ev_of(ev));
    CHECK(signaled(wh) == 0, "a live bound event is cleared");
    CHECK(kern_system_event_live(ev_of(ev)), "kern_system_event_live: a bound event in guest memory is live");
    destroy(ev);
    CHECK(!kern_system_event_live(ev_of(ev)), "kern_system_event_live: a destroyed event is not");
    kern_signal_system_event(ev_of(ev));   // state byte 0: skipped before the kernel lock
    // The byte reads nonzero again with the handles closed (the memory reused, or the SDK made the event again
    // without the binder): the signal must neither end the process nor reach anything.
    ev[SE_STATE] = 1;
    kern_signal_system_event(ev_of(ev));
    kern_clear_system_event(ev_of(ev));
    // The closed number goes to the next new event at once: the stale entry must not set or clear it.
    uint32_t other = kern_event_new();
    CHECK(other == wh, "the next event takes the closed number (%#x, closed %#x)", other, wh);
    kern_signal_system_event(ev_of(ev));
    CHECK(signaled(other) == 0, "an unrelated event that took the closed number is not signalled by the stale entry");
    lock();
    ((Event *)h_peek(other, O_EVENT_W))->signaled = 1;
    unlock();
    kern_clear_system_event(ev_of(ev));
    CHECK(signaled(other) == 1, "an unrelated event that took the closed number is not cleared by the stale entry");
    kern_handle_close(other);
    // Bound again after the destroy (state byte 0 first, as the guest leaves it): signals again.
    ev[SE_STATE] = 0;
    bind(ev);
    kern_signal_system_event(ev_of(ev));
    CHECK(signaled(wh_of(ev)) == 1, "the event bound again is signalled");
    destroy(ev);
}

// A host thread signals in a loop while the guest thread destroys the event and binds it again (the override called
// again after a DestroySystemEvent), as gfx_present.c vsync_main does with a vsync event.
static volatile int stop;
static volatile long signals;
static uint8_t *race_ev;
static void signaller(void *arg) {
    (void)arg;
    while (!stop) {
        kern_signal_system_event(ev_of(race_ev));
        kern_clear_system_event(ev_of(race_ev));
        __atomic_add_fetch(&signals, 1, __ATOMIC_RELAXED);
    }
}

static void race(void) {
    race_ev = guest[1];
    bind(race_ev);
    void *t = plat_thread_new(signaller, NULL, "signaller");
    uint64_t t0 = plat_now_ns();
    long rounds = 0;
    while (plat_now_ns() - t0 < 2000000000ull || rounds < 1000) {
        destroy(race_ev);
        bind(race_ev);
        rounds++;
    }
    stop = 1;
    plat_thread_join(t);
    printf("race: %ld destroy and bind rounds against %ld signals, no fatal\n", rounds, signals);
    destroy(race_ev);
}

// ov_movie2.c update_events on a movie decoder thread (a host thread).
static uint8_t *movie_in, *movie_out;
static void decoder(void *arg) {
    (void)arg;
    kern_signal_system_event(ev_of(movie_in));
    kern_signal_system_event(ev_of(movie_out));
}

static void full_table(int all_live) {
    int n = (int)(sizeof g_se / sizeof *g_se);
    // the guest entries 0 and 1 of the checks above are destroyed; fill the rest of the table with live ones and the
    // movie's two events, which live in host memory
    for (int i = 2; i < n - 2; i++) bind(guest[i]);
    movie_in = calloc(1, 0x40), movie_out = calloc(1, 0x40);
    bind(movie_in);
    bind(movie_out);
    CHECK(g_nse == n, "the table is full (%d of %d)", g_nse, n);
    if (all_live) {
        bind(guest[0]);   // the two destroyed guest entries are made again: every entry is live

        bind(guest[1]);
        printf("full: binding one more with every entry live\n");
        bind(guest[n]);
        printf("full: no fatal\n");
        return;
    }
    bind(guest[n]);
    CHECK(se_find(ev_of(movie_in), NULL) && se_find(ev_of(movie_out), NULL),
          "the movie's host-memory events keep their entries (in %s, out %s)",
          se_find(ev_of(movie_in), NULL) ? "kept" : "taken", se_find(ev_of(movie_out), NULL) ? "kept" : "taken");
    CHECK(se_find(ev_of(guest[n]), NULL) != 0, "the new event is bound");
    CHECK(se_find(ev_of(guest[0]), NULL) == 0, "it took the first destroyed guest entry");
    bind(guest[n + 1]);
    CHECK(se_find(ev_of(guest[1]), NULL) == 0, "the next takes the second destroyed guest entry");
    void *t = plat_thread_new(decoder, NULL, "decoder");
    plat_thread_join(t);
    CHECK(signaled(wh_of(movie_in)) == 1 && signaled(wh_of(movie_out)) == 1,
          "a host thread signals the movie's events with the table full");
}

// ov_movie2.c ov_dtor with the table full of live guest events and the movie's two host-memory events: each unbound
// event is destroyed (its handles closed) and leaves the table, the other keeps its entry, and once the last host
// entry is gone a new guest bind finds room (the full-table scan keeps host entries, so it would end with "table full").
static void unbind_host(void) {
    int n = (int)(sizeof g_se / sizeof *g_se);
    CHECK(g_nse == n, "the table is full before the unbinds (%d of %d)", g_nse, n);
    uint32_t in_w = wh_of(movie_in), in_r = rh_of(movie_in);
    kern_unbind_system_event(&cpu, ev_of(movie_in));
    CHECK(se_find(ev_of(movie_in), NULL) == 0, "an unbound event leaves the table");
    CHECK(g_nse == n - 1, "the table has one entry fewer (%d)", g_nse);
    CHECK(se_find(ev_of(movie_out), NULL) != 0, "the other host-memory event keeps its entry");
    CHECK(movie_in[SE_STATE] == 0 && signaled(in_w) == -1, "the unbound event is destroyed: state 0, handle %#x closed",
          in_w);
    lock();
    int r_open = h_peek(in_r, O_EVENT_R) != NULL;
    unlock();
    CHECK(!r_open, "its readable handle %#x is closed too", in_r);
    kern_unbind_system_event(&cpu, ev_of(movie_in));   // a second unbind finds nothing and leaves the table alone
    CHECK(g_nse == n - 1, "a second unbind changes nothing (%d)", g_nse);
    // Full again with live guest entries and one host entry; the host entry unbound, the next guest bind takes its room.
    int next = n + 2;
    bind(guest[next++]);
    CHECK(g_nse == n, "full again (%d)", g_nse);
    kern_unbind_system_event(&cpu, ev_of(movie_out));
    bind(guest[next]);
    CHECK(se_find(ev_of(guest[next]), NULL) != 0 && g_nse == n,
          "a guest bind succeeds once the host entry is unbound (%d entries)", g_nse);
    free(movie_in), free(movie_out);
    movie_in = movie_out = NULL;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    me.cur = &cpu;
    tcur = &me;
    int full = argc > 1 && !strcmp(argv[1], "full");
    stale_and_reuse();
    race();
    full_table(full);
    if (full) return 0;
    unbind_host();
    printf(fails ? "system_event_check: %d failed\n" : "system_event_check: ok\n", fails);
    return fails ? 1 : 0;
}
