// The room the card and the system have, as a level per side (mem_budget.h): sampled every MEM_BUDGET_PERIOD_MS on the
// "mem budget" thread, logged when a level changes, read by the owners of memory that can be given back (gfx_shader.c
// "SPIR-V at rest" compresses sooner when the system is tight) and shown in the memory report (vk_memory.c system_log).
// A sample is one driver query (vk_mem_vram) and two system calls (plat_commit_info, plat_mem_info): well under a
// millisecond, never on a drawing thread.
#include <stdio.h>

#include "cfg.h"
#include "host.h"
#include "mem_budget.h"
#include "platform/platform.h"
#include "vk.h"

static struct {
    PlatOnce once;
    PlatMutex vram_lock;               // held over a video memory sample: mem_budget_vram(0) waits for it
    int vram_on;                       // the device is up (under vram_lock)
    uint64_t test_mb[MEM_SIDES];       // MEM_BUDGET_TEST_VRAM_MB, MEM_BUDGET_TEST_RAM_MB; 0 off
    volatile int32_t level[MEM_SIDES];
    volatile int32_t known[MEM_SIDES];
    volatile uint32_t free_pm[MEM_SIDES];
} B = {PLAT_ONCE_INIT, PLAT_MUTEX_INIT, 0, {0, 0}, {MEM_NORMAL, MEM_NORMAL}, {0, 0}, {0, 0}};

static const char *const side_name[MEM_SIDES] = {"video memory", "system memory"};

// The video memory's room: the driver's budget (at most MEM_BUDGET_TEST_VRAM_MB) less this process's use. 0 while the
// device is not up or the driver gives no figure.
static int sample_vram(MemRoom *r, char *what, size_t cap) {
    uint64_t used = 0, budget = 0;
    int exact = 0, ok = 0;
    plat_mutex_lock(&B.vram_lock);
    if (B.vram_on) ok = vk_mem_vram(&used, &budget, &exact);
    plat_mutex_unlock(&B.vram_lock);
    if (!ok || !budget) return 0;
    uint64_t u = used >> 20, l = budget >> 20, t = B.test_mb[MEM_VRAM];
    if (t && t < l) l = t;
    *r = mem_room(u, l);
    snprintf(what, cap, "%llu of %llu MiB in use%s%s", (unsigned long long)u, (unsigned long long)l,
             exact ? "" : ", the allocator's own estimate", t ? ", MEM_BUDGET_TEST_VRAM_MB" : "");
    return 1;
}

// The system's room: the lower of the commit's headroom and the physical memory free; with MEM_BUDGET_TEST_RAM_MB
// that many MiB less this process's commit.
static int sample_ram(MemRoom *r, char *what, size_t cap) {
    double proc, used, limit;
    float pused, ptotal, pws;
    plat_commit_info(&proc, &used, &limit);
    plat_mem_info(&pused, &ptotal, &pws);
    uint64_t t = B.test_mb[MEM_RAM];
    if (t) {
        if (proc < 0) return 0;
        *r = mem_room((uint64_t)proc, t);
        snprintf(what, cap, "this process's commit %.0f of %llu MiB, MEM_BUDGET_TEST_RAM_MB", proc, (unsigned long long)t);
        return 1;
    }
    int have_c = used >= 0 && limit > 0, have_p = pused >= 0 && ptotal > 0;
    if (!have_c && !have_p) return 0;
    MemRoom none = {1000, UINT64_MAX};
    MemRoom c = have_c ? mem_room((uint64_t)used, (uint64_t)limit) : none;
    MemRoom p = have_p ? mem_room((uint64_t)pused, (uint64_t)ptotal) : none;
    *r = mem_room_min(c, p);
    snprintf(what, cap, "commit %.0f of %.0f MiB, physical memory %.0f of %.0f MiB in use", used, limit, (double)pused,
             (double)ptotal);
    return 1;
}

static void budget_main(void *arg) {
    (void)arg;
    for (;;) {
        for (int side = 0; side < MEM_SIDES; side++) {
            MemRoom r;
            char what[200];
            if (!(side == MEM_VRAM ? sample_vram(&r, what, sizeof what) : sample_ram(&r, what, sizeof what))) continue;
            int known = __atomic_load_n(&B.known[side], __ATOMIC_RELAXED), was = B.level[side];
            int now = mem_level_next(known ? was : MEM_ROOMY, r);   // the first sample: its level as it is
            __atomic_store_n(&B.free_pm[side], r.free_pm, __ATOMIC_RELAXED);
            __atomic_store_n(&B.level[side], now, __ATOMIC_RELAXED);
            __atomic_store_n(&B.known[side], 1, __ATOMIC_RELEASE);
            if (!known || now != was) {
                char from[32] = "";
                if (known) snprintf(from, sizeof from, ", was %s", mem_level_name[was]);
                rt_log("mem_budget: %s %s, %.1f%% free (%s%s)", side_name[side], mem_level_name[now], r.free_pm / 10.0,
                       what, from);
            }
        }
        plat_sleep_ms(MEM_BUDGET_PERIOD_MS);
    }
}

static void budget_start(void *arg) {
    (void)arg;
    int tv = cfg_int("NX2NAVI_MEM_BUDGET_TEST_VRAM_MB"), tr = cfg_int("NX2NAVI_MEM_BUDGET_TEST_RAM_MB");
    B.test_mb[MEM_VRAM] = tv > 0 ? (uint64_t)tv : 0, B.test_mb[MEM_RAM] = tr > 0 ? (uint64_t)tr : 0;
    if (B.test_mb[MEM_VRAM] || B.test_mb[MEM_RAM])
        rt_log("mem_budget: a test pretends a smaller budget (video memory %llu MiB, system memory %llu MiB; 0: as it is): "
               "the levels follow it, the texture cache's own rules do not", (unsigned long long)B.test_mb[MEM_VRAM],
               (unsigned long long)B.test_mb[MEM_RAM]);
    if (!plat_thread_spawn(budget_main, NULL, "mem budget", 0))
        rt_log("mem_budget: cannot start its thread; every level stays normal");
}

void mem_budget_vram(int on) {
    plat_mutex_lock(&B.vram_lock);
    B.vram_on = on;
    plat_mutex_unlock(&B.vram_lock);
    if (on) plat_once(&B.once, budget_start, NULL);
}

int mem_budget_level(int side) {
    if (side < 0 || side >= MEM_SIDES) return MEM_NORMAL;
    return __atomic_load_n(&B.level[side], __ATOMIC_RELAXED);
}

void mem_budget_text(char *out, unsigned cap) {
    size_t k = 0;
    if (cap) out[0] = 0;
    for (int side = 0; side < MEM_SIDES && k < cap; side++) {
        int n;
        if (__atomic_load_n(&B.known[side], __ATOMIC_ACQUIRE))
            n = snprintf(out + k, cap - k, "%s%s %s (%.1f%% free)", side ? ", " : "", side_name[side],
                         mem_level_name[__atomic_load_n(&B.level[side], __ATOMIC_RELAXED)],
                         __atomic_load_n(&B.free_pm[side], __ATOMIC_RELAXED) / 10.0);
        else n = snprintf(out + k, cap - k, "%s%s not sampled yet", side ? ", " : "", side_name[side]);
        if (n < 0) break;
        k += (size_t)n;
    }
}
