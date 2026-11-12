// setjmp / longjmp for lifted code.
//
// The guest's longjmp restores x19-x30, sp and d8-d15 and returns to the setjmp call's return address, which in lifted
// code is the middle of a host function: rt_dispatch has no function there and the run ends ("not a function start").
// Ocarina's font rasterizer reaches it whenever a large glyph's cells overflow its pool: main+0x166215c longjmps back
// to main+0x1661670, which reports the overflow, and the caller renders the glyph again in smaller bands. Text at a
// 2160-high picture does it in normal play.
//
// The overrides keep both halves on the host:
//   setjmp(env): the guest registers longjmp restores, kept for env, and the host state of the lifted function that
//     made the call as it will be when that call returns (platform.h plat_frame_capture: the host stack unwound up to
//     the frame running the guest function that contains the call), so a jump can make the call return again.
//     Returns 0.
//   longjmp(env, val): the guest registers back, x0 = val (1 for 0), and that host state resumed: the lifted caller
//     continues after its setjmp call, reads x0 from the Cpu and takes the "returned again" path.
// Host locals the caller keeps in callee-saved registers come back as they were at setjmp, the ones in its stack frame
// as last written; C allows only the unmodified ones to be relied on after a longjmp, and the guest code was compiled
// under the same rule. A longjmp to a buffer this file did not fill (or after the setjmp's function returned) is fatal.
#include <stdlib.h>
#include <string.h>

#include "dispatch.h"
#include "host.h"
#include "platform/platform.h"

#define MAX_MODULES 16   // as dispatch.c

typedef struct {
    uint64_t env;                    // guest jmp_buf address, 0 free
    void *thread;                    // Cpu.thread of the setjmp
    uint64_t x[12], sp;              // x19-x30, sp
    __uint128_t d[8];                // d8-d15 (the low halves are what the ABI keeps)
    PlatJmp frame;
    uint64_t seq;                    // jt_seq at its last setjmp: the smallest is the entry filled longest ago
} JmpEnt;

#define JMP_N 1024
static JmpEnt jt[JMP_N];
static PlatMutex jt_lock = PLAT_MUTEX_INIT;
static uint64_t jt_seq;              // jt_lock

// Host function starts of the lifted code, sorted, with their guest starts: the guest function a host address runs.
typedef struct { uintptr_t host; uint64_t guest; } Rev;
static Rev *rev;
static uint64_t nrev;
static volatile int32_t rev_ready;

static int by_host(const void *a, const void *b) {
    uintptr_t x = ((const Rev *)a)->host, y = ((const Rev *)b)->host;
    return x < y ? -1 : x > y;
}

static void rev_init(void) {
    plat_mutex_lock(&jt_lock);
    if (!rev_ready) {
        static RtModule mods[MAX_MODULES];
        int n = rt_linked_modules(mods, MAX_MODULES);
        uint64_t total = 0;
        for (int i = 0; i < n; i++) total += mods[i].count;
        rev = malloc((total ? total : 1) * sizeof *rev);
        if (!rev) rt_fatal("setjmp: out of memory");
        for (int i = 0; i < n; i++)
            for (uint64_t k = 0; k < mods[i].count; k++) rev[nrev++] = (Rev){(uintptr_t)mods[i].fns[k], mods[i].addrs[k]};
        qsort(rev, nrev, sizeof *rev, by_host);
        __atomic_store_n(&rev_ready, 1, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&jt_lock);
}

// The guest start of the lifted function whose host code contains rip; 0 outside (or above the last start).
static uint64_t guest_of_host(uintptr_t rip) {
    if (!nrev || rip < rev[0].host || rip > rev[nrev - 1].host) return 0;
    uint64_t lo = 0, hi = nrev;
    while (hi - lo > 1) {
        uint64_t m = (lo + hi) / 2;
        if (rev[m].host <= rip) lo = m;
        else hi = m;
    }
    return rev[lo].guest;
}

// The guest start of the lifted function containing guest address a, 0 for none.
static uint64_t guest_function_of(uint64_t a) {
    static RtModule mods[MAX_MODULES];
    static int n = -1;
    if (n < 0) n = rt_linked_modules(mods, MAX_MODULES);
    for (int i = 0; i < n; i++) {
        const RtModule *m = &mods[i];
        if (a < m->base || a >= m->base + m->size || !m->count || a < m->addrs[0]) continue;
        uint64_t lo = 0, hi = m->count;
        while (hi - lo > 1) {
            uint64_t k = (lo + hi) / 2;
            if (m->addrs[k] <= a) lo = k;
            else hi = k;
        }
        return m->addrs[lo];
    }
    return 0;
}

static int is_caller(uintptr_t rip, void *arg) { return guest_of_host(rip) == *(const uint64_t *)arg; }

static JmpEnt *slot_of(uint64_t env, int make) {
    uint64_t h = (env * 0x9E3779B97F4A7C15ull) >> 54;   // 10 bits
    for (int k = 0; k < JMP_N; k++) {
        JmpEnt *e = &jt[(h + k) % JMP_N];
        if (e->env == env) return e;
        if (!e->env) {
            if (!make) return NULL;
            e->env = env;
            return e;
        }
    }
    return NULL;
}

static void ov_setjmp(Cpu *c) {
    if (!__atomic_load_n(&rev_ready, __ATOMIC_ACQUIRE)) rev_init();
    uint64_t env = c->x[0];
    uint64_t caller = guest_function_of(c->x[30] - 4);   // the BL to setjmp sits just before the return address
    JmpEnt e = {env, c->thread, {0}, c->sp, {0}, {0}};
    for (int i = 0; i < 12; i++) e.x[i] = c->x[19 + i];
    for (int i = 0; i < 8; i++) e.d[i] = c->v[8 + i];
    if (!caller || !plat_frame_capture(&e.frame, is_caller, &caller))
        rt_fatal("setjmp: no host frame of the guest function %#llx that called it (lr %#llx)",
                 (unsigned long long)caller, (unsigned long long)c->x[30]);
    plat_mutex_lock(&jt_lock);
    JmpEnt *s = slot_of(env, 1);
    int replaced = 0;
    if (!s) {
        // Every slot holds a buffer (entries are never removed, and a long session keeps meeting new buffer
        // addresses): the one filled longest ago gives its slot. With no free slot left a lookup walks the whole
        // table, so the new buffer is found wherever it sits. A longjmp to the replaced buffer ends the process as
        // one to an unknown buffer does (ov_longjmp); before, the 1025th buffer itself ended it.
        s = &jt[0];
        for (int k = 1; k < JMP_N; k++)
            if (jt[k].seq < s->seq) s = &jt[k];
        replaced = 1;
    }
    e.seq = ++jt_seq;
    *s = e;
    plat_mutex_unlock(&jt_lock);
    static volatile int32_t n_replaced;
    if (replaced && plat_inc32(&n_replaced) == 1)
        rt_log("setjmp: all %d buffer slots are in use; the buffer filled longest ago gives its slot from now on "
               "(logged once)", JMP_N);
    c->x[0] = 0;
}

static void ov_longjmp(Cpu *c) {
    uint64_t env = c->x[0], val = (uint32_t)c->x[1];
    static _Thread_local JmpEnt e;   // the frame is resumed from here: not on the stack being abandoned
    plat_mutex_lock(&jt_lock);
    JmpEnt *s = slot_of(env, 0);
    if (s) e = *s;
    plat_mutex_unlock(&jt_lock);
    if (!s || e.thread != c->thread)
        rt_fatal("longjmp to buffer %#llx, which no setjmp of this thread filled (lr %#llx)", (unsigned long long)env,
                 (unsigned long long)c->x[30]);
    static uint32_t n_logged;
    if (n_logged < 4) {
        n_logged++;
        char at[160];
        rt_log("longjmp to buffer %#llx from %s: back into the guest function that called setjmp",
               (unsigned long long)env, ldr_describe(c->x[30], at, sizeof at));
        // the guest frames that led here (the frame-pointer chain), to name what failed
        // (from the setjmp's caller: its return address, then the frame-pointer chain of x29 as setjmp saw it)
        rt_log("  setjmp called from %s", ldr_describe(e.x[11], at, sizeof at));
        uint64_t fp = e.x[10];
        for (int k = 0; k < 10 && fp && mem_readable(fp, 16); k++) {
            const uint64_t *f = (const uint64_t *)(uintptr_t)fp;
            rt_log("  caller %d: %s", k, ldr_describe(f[1], at, sizeof at));
            if (f[0] <= fp) break;
            fp = f[0];
        }
    }
    for (int i = 0; i < 12; i++) c->x[19 + i] = e.x[i];
    for (int i = 0; i < 8; i++) c->v[8 + i] = e.d[i];
    c->sp = e.sp;
    c->x[0] = val ? val : 1;
    plat_jmp_go(&e.frame);
}

HOST_OVERRIDE("setjmp", ov_setjmp);
HOST_OVERRIDE("longjmp", ov_longjmp);

// NX2NAVI_TRACE=perf "mem: sizes" (perf.c): the jmp_buf table's entries (entries are never removed; fatal when full).
void setjmp_sizes(PerfSizes *s) {
    uint64_t n = 0;
    plat_mutex_lock(&jt_lock);
    for (int k = 0; k < JMP_N; k++) n += jt[k].env != 0;
    plat_mutex_unlock(&jt_lock);
    perf_size(s, "setjmp buffers", n, JMP_N, PERF_SIZE_NONE);
}
