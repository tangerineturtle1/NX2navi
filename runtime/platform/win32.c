// Windows implementation of platform.h: CreateThread, QueryPerformanceCounter, high-resolution waitable timers,
// SRW locks and condition variables.
#include <io.h>
#include <limits.h>
#include <malloc.h>     // _get_heap_handle (plat_heap_info)
#include <stdio.h>
#include <winsock2.h>   // before windows.h, which would otherwise pull in the old winsock.h
#include <ws2tcpip.h>
#include <windows.h>
#include <aclapi.h>     // SetNamedSecurityInfoW (plat_file_private)
#include <bcrypt.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <restartmanager.h>
#include <shellapi.h>
#include <timeapi.h>
#include <tlhelp32.h>   // CreateToolhelp32Snapshot (plat_watch_set)
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "platform.h"

_Static_assert(sizeof(PlatMutex) == sizeof(SRWLOCK), "PlatMutex holds an SRWLOCK");
_Static_assert(sizeof(PlatCond) == sizeof(CONDITION_VARIABLE), "PlatCond holds a CONDITION_VARIABLE");

// ---- threads

struct PlatThread {
    HANDLE h;
    PlatThreadFn fn;
    void *arg;
    volatile int32_t state;   // 0 running and owned by its maker, 1 detached, 2 its function has returned
};

// What a thread made here keeps for itself is given back when its function returns: the handles below (the sleep
// timer of plat_wait_ns, the events of plat_file_pread and plat_file_pwrite), what the other files keep per thread
// (plat_thread_end_hook) and, for a detached thread, its PlatThread. The game makes and ends threads all through
// play, each a host thread: nothing closed them, so every ended thread that had slept, read or written left up
// to three kernel handles behind for the rest of the run. A guest thread ends by returning here too (kernel.c:
// ExitThread jumps back to the thread's start).
static void thread_handles_close(void);

static DWORD WINAPI thread_main(LPVOID p) {
    PlatThread *t = p;
    t->fn(t->arg);
    plat_thread_end_run();
    thread_handles_close();
    if (plat_xchg32(&t->state, 2) == 1) free(t);   // detached while it ran: nobody else frees it
    return 0;
}

PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name) {
    return plat_thread_new_ex(fn, arg, name, 0);
}

PlatThread *plat_thread_new_ex(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    PlatThread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    t->h = CreateThread(NULL, stack ? stack : PLAT_STACK_DEFAULT, thread_main, t,
                        CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t->h) { free(t); return NULL; }
    if (name) {
        wchar_t w[64];
        if (MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 64)) SetThreadDescription(t->h, w);
    }
    ResumeThread(t->h);
    return t;
}

void plat_thread_join(PlatThread *t) {
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}

int plat_thread_join_ns(PlatThread *t, uint64_t ns) {
    uint64_t ms = (ns + 999999) / 1000000;
    if (WaitForSingleObject(t->h, ms >= INFINITE ? INFINITE - 1 : (DWORD)ms) != WAIT_OBJECT_0) return 0;
    CloseHandle(t->h);
    free(t);
    return 1;
}

// The thread still reads t->fn and t->arg when it starts, so the struct goes when the thread's function has
// returned (thread_main), or here when it already has.
void plat_thread_detach(PlatThread *t) {
    CloseHandle(t->h);
    t->h = NULL;
    if (plat_xchg32(&t->state, 1) == 2) free(t);
}

int plat_cpu_cores(uint64_t *all, uint64_t *first) {
    DWORD_PTR proc, sys;
    *all = *first = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys)) return 0;
    *all = proc;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *buf = len ? malloc(len) : NULL;
    int cores = 0;
    if (buf && GetLogicalProcessorInformationEx(RelationProcessorCore, buf, &len))
        for (DWORD off = 0; off < len;) {
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e = (void *)((char *)buf + off);
            KAFFINITY m = e->Processor.GroupMask[0].Mask & proc;
            if (e->Processor.GroupMask[0].Group == 0 && m) *first |= m & -m, cores++;   // the lowest of the core's
            off += e->Size;
        }
    free(buf);
    return cores;
}

uint64_t plat_cpu_cache_die(uint32_t *l3_kb) {
    DWORD_PTR proc, sys;
    *l3_kb = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys)) return 0;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationCache, NULL, &len);
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *buf = len ? malloc(len) : NULL;
    uint64_t die = 0;
    DWORD big = 0, small = ~0u;
    if (buf && GetLogicalProcessorInformationEx(RelationCache, buf, &len))
        for (DWORD off = 0; off < len;) {
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e = (void *)((char *)buf + off);
            const CACHE_RELATIONSHIP *c = &e->Cache;
            if (c->Level == 3 && c->GroupMask.Group == 0) {   // every L3, also those outside the process's set
                if (c->CacheSize > big) big = c->CacheSize, die = 0;
                if (c->CacheSize == big) die |= c->GroupMask.Mask;
                if (c->CacheSize < small) small = c->CacheSize;
            }
            off += e->Size;
        }
    free(buf);
    if (!die || big == small) return 0;
    *l3_kb = big >> 10;
    return die & proc;
}

int plat_process_affinity(uint64_t mask) { return SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR)mask) != 0; }

uint64_t plat_thread_cpu_ns(uint32_t tid) {
    HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    FILETIME a, b, k, u;
    uint64_t ns = 0;
    if (th && GetThreadTimes(th, &a, &b, &k, &u))
        ns = (((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime)) *
             100;
    if (th) CloseHandle(th);
    return ns;
}

// ---- thread interruption: SuspendThread, Get/SetThreadContext, ResumeThread.
struct PlatIntr {
    CONTEXT ctx;   // first: CONTEXT wants 16-byte alignment
    HANDLE h;
};

PlatIntr *plat_intr_self(void) {
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                          FALSE, GetCurrentThreadId());
    PlatIntr *t = h ? _aligned_malloc(sizeof *t, 16) : NULL;
    if (!t) {
        if (h) CloseHandle(h);
        return NULL;
    }
    t->h = h;
    return t;
}

void plat_intr_free(PlatIntr *t) {
    if (!t) return;
    CloseHandle(t->h);
    _aligned_free(t);
}

int plat_intr_stop(PlatIntr *t, uint64_t *rip, uint64_t *rsp) {
    if (SuspendThread(t->h) == (DWORD)-1) return 0;
    memset(&t->ctx, 0, sizeof t->ctx);
    t->ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;   // set back whole, so the kernel restores every GPR
    if (!GetThreadContext(t->h, &t->ctx)) {
        ResumeThread(t->h);
        return 0;
    }
    *rip = t->ctx.Rip;
    *rsp = t->ctx.Rsp;
    return 1;
}

int plat_intr_resume(PlatIntr *t, uint64_t to) {
    int ok = 1;
    if (to) {
        t->ctx.Rip = to;
        ok = SetThreadContext(t->h, &t->ctx) != 0;
    }
    ResumeThread(t->h);
    return ok;
}

// ---- hardware data watchpoints (platform.h plat_watch_set): the debug registers DR0-DR3 hold the addresses and DR7
// enables them, each for reads and writes (R/W 11) of its length. A thread's debug registers are part of its context:
// a helper thread lists the process's threads, then stops each, sets them with SetThreadContext and lets it go on; it
// opens every handle before it stops anything and takes no lock while a thread is stopped, so a thread stopped while
// holding the heap or loader lock cannot block it. The caller waits for the helper, which also sets the caller's own
// registers. An access traps after the instruction that made it, as EXCEPTION_SINGLE_STEP with the slot's bit in DR6;
// watch_veh hands it on and writes the current set into the context it continues with. So a thread that traps after
// its registers were cleared, or whose continue would restore registers from before a change, ends up with the
// current set too.
static PlatWatchHit watch_fn;
static volatile LONG watch_on;
static DWORD64 watch_addr[4], watch_dr7;
static int watch_added;

// frames[0] the instruction after the access, then the return addresses the unwind tables give
static int watch_unwind(const CONTEXT *at, uint64_t *frames) {
    CONTEXT c = *at;
    int d = 0;
    frames[d++] = c.Rip;
    while (d < PLAT_WATCH_DEPTH) {
        DWORD64 base;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, NULL);
        if (f) {
            void *hd;
            DWORD64 est;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &hd, &est, NULL);
        } else {   // a leaf function: the return address is on top
            c.Rip = *(DWORD64 *)c.Rsp;
            c.Rsp += 8;
        }
        if (!c.Rip) break;
        frames[d++] = c.Rip;
    }
    return d;
}

static LONG CALLBACK watch_veh(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord;
    unsigned bits = (unsigned)(c->Dr6 & 0xf);
    if (!bits) return EXCEPTION_CONTINUE_SEARCH;   // a trap-flag step, not a watchpoint
    int on = watch_on != 0, stop = !on;
    if (on && watch_fn) {
        uint64_t frames[PLAT_WATCH_DEPTH];
        int d = watch_unwind(c, frames);
        for (int s = 0; s < 4; s++)
            if (bits >> s & 1 && watch_dr7 >> (2 * s) & 1) stop |= watch_fn(s, frames, d);
    }
    c->Dr0 = watch_addr[0], c->Dr1 = watch_addr[1], c->Dr2 = watch_addr[2], c->Dr3 = watch_addr[3];
    c->Dr7 = stop ? 0 : watch_dr7;
    c->Dr6 = 0;
    c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
    return EXCEPTION_CONTINUE_EXECUTION;
}

#define WATCH_THREADS 4096
static DWORD WINAPI watch_apply(void *arg) {
    (void)arg;
    static DWORD tids[WATCH_THREADS];
    static HANDLE hs[WATCH_THREADS];
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return (DWORD)-1;
    int nt = 0;
    THREADENTRY32 te = {.dwSize = sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok && nt < WATCH_THREADS; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) tids[nt++] = te.th32ThreadID;
    CloseHandle(snap);
    int nh = 0;
    for (int i = 0; i < nt; i++) {
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_SET_CONTEXT, FALSE, tids[i]);
        if (h) hs[nh++] = h;
    }
    static CONTEXT ctx __attribute__((aligned(16)));
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    ctx.Dr0 = watch_addr[0], ctx.Dr1 = watch_addr[1], ctx.Dr2 = watch_addr[2], ctx.Dr3 = watch_addr[3];
    ctx.Dr7 = watch_on ? watch_dr7 : 0;
    DWORD done = 0;
    for (int i = 0; i < nh; i++) {   // nothing between the stop and the go-on but the context call
        if (SuspendThread(hs[i]) != (DWORD)-1) {
            if (SetThreadContext(hs[i], &ctx)) done++;
            ResumeThread(hs[i]);
        }
    }
    for (int i = 0; i < nh; i++) CloseHandle(hs[i]);
    return done;
}

int plat_watch_set(const uint64_t *addr, const int *len, int n, PlatWatchHit hit) {
    if (n < 0) n = 0;
    if (n > 4) n = 4;
    if (!watch_added) {
        if (!n) return 0;
        if (!AddVectoredExceptionHandler(1, watch_veh)) return -1;
        watch_added = 1;
    }
    DWORD64 dr7 = 0;
    for (int i = 0; i < 4; i++) watch_addr[i] = i < n ? addr[i] : 0;
    for (int i = 0; i < n; i++) {
        DWORD64 size = len[i] == 8 ? 2 : len[i] == 4 ? 3 : len[i] == 2 ? 1 : 0;   // DR7's LEN encoding
        dr7 |= 1ull << (2 * i) | 3ull << (16 + 4 * i) | size << (18 + 4 * i);
    }
    // the hook and the registers are in place before watch_on says so, and a clear takes watch_on away first
    if (n) watch_fn = hit, watch_dr7 = dr7;
    InterlockedExchange(&watch_on, n > 0);
    HANDLE t = CreateThread(NULL, 64 << 10, watch_apply, NULL, 0, NULL);
    DWORD code = (DWORD)-1;
    if (!t || WaitForSingleObject(t, 10000) != WAIT_OBJECT_0 || !GetExitCodeThread(t, &code)) code = (DWORD)-1;
    if (t) CloseHandle(t);
    if (code == (DWORD)-1 && n) InterlockedExchange(&watch_on, 0);
    return code == (DWORD)-1 ? -1 : (int)code;
}

// ---- fibers: Windows fibers with FIBER_FLAG_FLOAT_SWITCH (the MXCSR and x87 control word switch with them).
struct PlatFiber {
    void *h;
    void (*fn)(void *arg);
    void *arg;
};

static __thread PlatFiber self_fiber;

PlatFiber *plat_fiber_self(void) {
    self_fiber.h = ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
    return self_fiber.h ? &self_fiber : NULL;
}

void plat_fiber_unself(void) {
    ConvertFiberToThread();
    self_fiber.h = NULL;
}

static void WINAPI fiber_start(void *p) {
    PlatFiber *f = p;
    f->fn(f->arg);
}

PlatFiber *plat_fiber_new(size_t stack, void (*fn)(void *arg), void *arg) {
    PlatFiber *f = malloc(sizeof *f);
    if (!f) return NULL;
    f->fn = fn, f->arg = arg;
    f->h = CreateFiberEx(0x10000, stack, FIBER_FLAG_FLOAT_SWITCH, fiber_start, f);
    if (!f->h) {
        free(f);
        return NULL;
    }
    return f;
}

void plat_fiber_switch(PlatFiber *to) { SwitchToFiber(to->h); }

void plat_fiber_free(PlatFiber *f) {
    if (!f) return;
    DeleteFiber(f->h);
    free(f);
}

// plat_jmp_save / plat_jmp_go: PlatJmp holds rbx, rbp, rdi, rsi, r12-r15, the caller's rsp and the return address at
// 0..72, MXCSR at 80, the x87 control word at 84 and xmm6-xmm15 from 96, the Win64 callee-saved state. Both are leaf
// code that does not move rsp before it leaves, so they need no unwind data.
_Static_assert(sizeof(PlatJmp) == 256 && __builtin_offsetof(PlatJmp, xmm) == 96, "plat_jmp_save layout");
__asm__(".text\n"
        ".globl plat_jmp_save\n"
        ".def plat_jmp_save; .scl 2; .type 32; .endef\n"
        ".p2align 4\n"
        "plat_jmp_save:\n"
        "    movq %rbx, 0(%rcx)\n"
        "    movq %rbp, 8(%rcx)\n"
        "    movq %rdi, 16(%rcx)\n"
        "    movq %rsi, 24(%rcx)\n"
        "    movq %r12, 32(%rcx)\n"
        "    movq %r13, 40(%rcx)\n"
        "    movq %r14, 48(%rcx)\n"
        "    movq %r15, 56(%rcx)\n"
        "    leaq 8(%rsp), %rdx\n"   // the caller's rsp after this call returns
        "    movq %rdx, 64(%rcx)\n"
        "    movq (%rsp), %rdx\n"
        "    movq %rdx, 72(%rcx)\n"
        "    stmxcsr 80(%rcx)\n"
        "    fnstcw 84(%rcx)\n"
        "    movdqu %xmm6, 96(%rcx)\n"
        "    movdqu %xmm7, 112(%rcx)\n"
        "    movdqu %xmm8, 128(%rcx)\n"
        "    movdqu %xmm9, 144(%rcx)\n"
        "    movdqu %xmm10, 160(%rcx)\n"
        "    movdqu %xmm11, 176(%rcx)\n"
        "    movdqu %xmm12, 192(%rcx)\n"
        "    movdqu %xmm13, 208(%rcx)\n"
        "    movdqu %xmm14, 224(%rcx)\n"
        "    movdqu %xmm15, 240(%rcx)\n"
        "    xorl %eax, %eax\n"
        "    ret\n"
        ".globl plat_jmp_go\n"
        ".def plat_jmp_go; .scl 2; .type 32; .endef\n"
        ".p2align 4\n"
        "plat_jmp_go:\n"
        "    movq 0(%rcx), %rbx\n"
        "    movq 8(%rcx), %rbp\n"
        "    movq 16(%rcx), %rdi\n"
        "    movq 24(%rcx), %rsi\n"
        "    movq 32(%rcx), %r12\n"
        "    movq 40(%rcx), %r13\n"
        "    movq 48(%rcx), %r14\n"
        "    movq 56(%rcx), %r15\n"
        "    ldmxcsr 80(%rcx)\n"
        "    fldcw 84(%rcx)\n"
        "    movdqu 96(%rcx), %xmm6\n"
        "    movdqu 112(%rcx), %xmm7\n"
        "    movdqu 128(%rcx), %xmm8\n"
        "    movdqu 144(%rcx), %xmm9\n"
        "    movdqu 160(%rcx), %xmm10\n"
        "    movdqu 176(%rcx), %xmm11\n"
        "    movdqu 192(%rcx), %xmm12\n"
        "    movdqu 208(%rcx), %xmm13\n"
        "    movdqu 224(%rcx), %xmm14\n"
        "    movdqu 240(%rcx), %xmm15\n"
        "    movq 64(%rcx), %rsp\n"
        "    movl $1, %eax\n"
        "    jmpq *72(%rcx)\n");

int plat_frame_capture(PlatJmp *out, int (*want)(uintptr_t rip, void *arg), void *arg) {
    CONTEXT c;
    RtlCaptureContext(&c);
    ULONG_PTR low, high;
    GetCurrentThreadStackLimits(&low, &high);
    for (int i = 0; i < 256 && c.Rsp >= low && c.Rsp + 8 <= high; i++) {
        DWORD64 img;
        RUNTIME_FUNCTION *fe = RtlLookupFunctionEntry(c.Rip, &img, NULL);
        if (fe) {
            void *data;
            DWORD64 frame;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, img, c.Rip, fe, &c, &data, &frame, NULL);
        } else {   // a leaf: the return address is on top of the stack
            c.Rip = *(const DWORD64 *)c.Rsp;
            c.Rsp += 8;
        }
        if (!c.Rip) return 0;
        if (!want((uintptr_t)c.Rip, arg)) continue;
        out->gpr[0] = c.Rbx, out->gpr[1] = c.Rbp, out->gpr[2] = c.Rdi, out->gpr[3] = c.Rsi;
        out->gpr[4] = c.R12, out->gpr[5] = c.R13, out->gpr[6] = c.R14, out->gpr[7] = c.R15;
        out->gpr[8] = c.Rsp, out->gpr[9] = c.Rip;
        __asm__ volatile("stmxcsr %0\n\tfnstcw %1" : "=m"(out->mxcsr), "=m"(out->fpcw));
        memcpy(out->xmm, &c.Xmm6, 160);   // xmm6-xmm15, consecutive in CONTEXT
        return 1;
    }
    return 0;
}

int plat_thread_spawn(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    PlatThread *t = plat_thread_new_ex(fn, arg, name, stack);
    if (t) plat_thread_detach(t);
    return t != NULL;
}

uint64_t plat_thread_affinity(PlatThread *t, uint64_t mask) {
    return (uint64_t)SetThreadAffinityMask(t ? t->h : GetCurrentThread(), (DWORD_PTR)mask);
}

// The calling thread gets a core of its own through CPU sets: the process's default sets, which every thread without
// sets of its own runs on (those started later too), leave the core out, and the caller's selected sets are that
// core. The core is the highest-numbered one of the fastest efficiency class (a performance core on a hybrid CPU),
// with both its logical processors when it has two; only processors in group 0 and in the process's affinity mask
// count. CPU sets are soft: the scheduler may still use the core when nothing else wants it. Returns the logical
// processors reserved, 0 when Windows has no CPU sets (before Windows 10) or fewer than two cores qualify; *which
// (when not NULL, size bytes) receives their numbers, such as "10,11".
int plat_cpu_reserve(char *which, size_t size) {
    typedef BOOL(WINAPI * GetInfoFn)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
    typedef BOOL(WINAPI * SetProcFn)(HANDLE, const ULONG *, ULONG);
    typedef BOOL(WINAPI * SetThreadFn)(HANDLE, const ULONG *, ULONG);
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    GetInfoFn get = (GetInfoFn)(void *)GetProcAddress(k, "GetSystemCpuSetInformation");
    SetProcFn set_proc = (SetProcFn)(void *)GetProcAddress(k, "SetProcessDefaultCpuSets");
    SetThreadFn set_thread = (SetThreadFn)(void *)GetProcAddress(k, "SetThreadSelectedCpuSets");
    if (!get || !set_proc || !set_thread) return 0;
    ULONG len = 0;
    get(NULL, 0, &len, GetCurrentProcess(), 0);
    uint8_t *buf = len ? malloc(len) : NULL;
    if (!buf || !get((PSYSTEM_CPU_SET_INFORMATION)buf, len, &len, GetCurrentProcess(), 0)) {
        free(buf);
        return 0;
    }
    DWORD_PTR proc = 0, sys = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) || !proc) proc = ~(DWORD_PTR)0;
    int best_class = -1, best_core = -1;
    for (ULONG o = 0; o + sizeof(DWORD) * 2 <= len;) {   // the core: fastest class, then highest index
        const SYSTEM_CPU_SET_INFORMATION *s = (const void *)(buf + o);
        if (!s->Size) break;
        if (s->Type == CpuSetInformation && s->CpuSet.Group == 0 && s->CpuSet.LogicalProcessorIndex < 64 &&
            (proc >> s->CpuSet.LogicalProcessorIndex & 1)) {
            int cls = s->CpuSet.EfficiencyClass, core = s->CpuSet.CoreIndex;
            if (cls > best_class || (cls == best_class && core > best_core)) best_class = cls, best_core = core;
        }
        o += s->Size;
    }
    ULONG rest[64], mine[8];
    unsigned mine_cpu[8];   // the logical processor numbers of mine
    int nrest = 0, nmine = 0;
    for (ULONG o = 0; o + sizeof(DWORD) * 2 <= len;) {
        const SYSTEM_CPU_SET_INFORMATION *s = (const void *)(buf + o);
        if (!s->Size) break;
        if (s->Type == CpuSetInformation && s->CpuSet.Group == 0 && s->CpuSet.LogicalProcessorIndex < 64 &&
            (proc >> s->CpuSet.LogicalProcessorIndex & 1)) {
            if (s->CpuSet.EfficiencyClass == best_class && s->CpuSet.CoreIndex == best_core) {
                if (nmine < 8) mine_cpu[nmine] = s->CpuSet.LogicalProcessorIndex, mine[nmine++] = s->CpuSet.Id;
            } else if (nrest < 64) {
                rest[nrest++] = s->CpuSet.Id;
            }
        }
        o += s->Size;
    }
    free(buf);
    if (!nmine || nrest < 2) return 0;
    if (!set_proc(GetCurrentProcess(), rest, (ULONG)nrest)) return 0;
    if (!set_thread(GetCurrentThread(), mine, (ULONG)nmine)) {
        set_proc(GetCurrentProcess(), NULL, 0);   // back to every processor
        return 0;
    }
    if (which && size) {
        size_t n = 0;
        which[0] = 0;
        for (int i = 0; i < nmine && n < size; i++)
            n += (size_t)snprintf(which + n, size - n, "%s%u", n ? "," : "", mine_cpu[i]);
    }
    return nmine;
}

int plat_cpu_reserve_sweep(void) { return 0; }

uint32_t plat_cpu_count(void) {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n ? (uint32_t)n : 1;
}

// The processors of the process's affinity mask: what `start /affinity`, Task Manager's "Set affinity" or a launcher
// leaves this process. A process that spans processor groups (more than 64 logical processors) gets no mask from
// GetProcessAffinityMask (both masks 0): every active processor counts then.
uint32_t plat_cpu_usable(void) {
    DWORD_PTR proc = 0, sys = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) && proc && sys) {
        uint32_t n = (uint32_t)__builtin_popcountll((unsigned long long)proc);
        if (n) return n;
    }
    return plat_cpu_count();
}

// PLAT_PRIO_* are the THREAD_PRIORITY_* values.
void plat_thread_priority(int prio) { SetThreadPriority(GetCurrentThread(), prio); }
void plat_thread_set_priority(PlatThread *t, int prio) { SetThreadPriority(t->h, prio); }
int plat_thread_priority_get(void) { return GetThreadPriority(GetCurrentThread()); }

void plat_yield(void) { SwitchToThread(); }

// ---- time

static uint64_t qpc_freq(void) {
    static uint64_t freq;
    if (!freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = (uint64_t)f.QuadPart;
    }
    return freq;
}

uint64_t plat_counter(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}

uint64_t plat_counter_hz(void) { return qpc_freq(); }

// The default timer tick is 15.6 ms; a 1 ms wait would cost a full tick.
void plat_timer_1ms(void) { timeBeginPeriod(1); }

// Windows 11 runs a process whose window is not in front (another window focused, a second monitor's overlay) at
// efficiency speed (EcoQoS, the E-cores of a hybrid CPU) and may ignore its timer resolution request, which turns the
// 1 ms waits the pacing and short sleeps rely on into 15.6 ms ones. Opting out keeps full speed and the 1 ms timer.
// The ignore-timer-resolution control needs Windows 11; on Windows 10 the call fails and execution speed alone is asked.
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif
int plat_power_full(void) {
    PROCESS_POWER_THROTTLING_STATE st = {.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                         .ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
                                                        PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION,
                                         .StateMask = 0};
    if (SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st)) return 2;
    st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    return SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st) ? 1 : 0;
}

static PlatScale qpc_ns;   // set before main, so no thread reads it half written

__attribute__((constructor)) static void qpc_ns_init(void) { qpc_ns = plat_scale(1000000000u, qpc_freq()); }

uint64_t plat_now_ns(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return plat_scaled(qpc_ns, (uint64_t)t.QuadPart);
}

// A high-resolution waitable timer per sleeping thread (Windows 10 1803+; one handle per thread, closed when the
// thread's function returns, thread_handles_close)
// wakes within about 0.1-0.5 ms; the last PLAT_SPIN_NS are spent yielding so the deadline is met closely.
// Without that timer kind, Sleep(1) granularity applies.
#define PLAT_SPIN_NS 300000u

static __thread HANDLE sleep_timer;
static __thread int sleep_timer_tried;
static __thread HANDLE pread_ev, pwrite_ev;   // plat_file_pread, plat_file_pwrite: the event of their overlapped call

static void thread_handles_close(void) {
    if (sleep_timer) CloseHandle(sleep_timer);
    if (pread_ev) CloseHandle(pread_ev);
    if (pwrite_ev) CloseHandle(pwrite_ev);
    sleep_timer = pread_ev = pwrite_ev = NULL, sleep_timer_tried = 0;
}

void plat_wait_ns(uint64_t ns) {
    if (!sleep_timer_tried) {
        sleep_timer_tried = 1;
        sleep_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    LARGE_INTEGER due = {.QuadPart = -(LONGLONG)(ns / 100)};   // relative, 100 ns units
    if (!due.QuadPart) due.QuadPart = -1;
    if (sleep_timer && SetWaitableTimerEx(sleep_timer, &due, 0, NULL, NULL, NULL, 0))
        WaitForSingleObject(sleep_timer, INFINITE);
    else Sleep((DWORD)((ns + 999999) / 1000000));
}

void plat_sleep_ns(uint64_t ns) {
    uint64_t end = plat_now_ns() + ns;
    if (ns > PLAT_SPIN_NS) plat_wait_ns(ns - PLAT_SPIN_NS);
    while (plat_now_ns() < end) SwitchToThread();
}

void plat_sleep_ms(uint32_t ms) { Sleep(ms); }

// ---- locks and condition variables

void plat_mutex_init(PlatMutex *m) { InitializeSRWLock((PSRWLOCK)m); }
static _Thread_local PlatLockNote lock_note;   // plat_lock_timing
void plat_lock_timing(PlatLockNote fn) { lock_note = fn; }

void plat_mutex_lock_at(PlatMutex *m, void *site) {
    if (!lock_note || TryAcquireSRWLockExclusive((PSRWLOCK)m)) {
        if (!lock_note) AcquireSRWLockExclusive((PSRWLOCK)m);
        return;
    }
    uint64_t t0 = plat_now_ns();
    AcquireSRWLockExclusive((PSRWLOCK)m);
    lock_note(site, plat_now_ns() - t0);
}

void plat_mutex_lock(PlatMutex *m) {
    if (!lock_note) { AcquireSRWLockExclusive((PSRWLOCK)m); return; }
    plat_mutex_lock_at(m, __builtin_return_address(0));
}
int plat_mutex_trylock(PlatMutex *m) { return TryAcquireSRWLockExclusive((PSRWLOCK)m) != 0; }
void plat_mutex_unlock(PlatMutex *m) { ReleaseSRWLockExclusive((PSRWLOCK)m); }

void plat_mutex_lock_shared(PlatMutex *m) {
    if (!lock_note || TryAcquireSRWLockShared((PSRWLOCK)m)) {
        if (!lock_note) AcquireSRWLockShared((PSRWLOCK)m);
        return;
    }
    uint64_t t0 = plat_now_ns();
    AcquireSRWLockShared((PSRWLOCK)m);
    lock_note(__builtin_return_address(0), plat_now_ns() - t0);
}
int plat_mutex_trylock_shared(PlatMutex *m) { return TryAcquireSRWLockShared((PSRWLOCK)m) != 0; }
void plat_mutex_unlock_shared(PlatMutex *m) { ReleaseSRWLockShared((PSRWLOCK)m); }

void plat_cond_init(PlatCond *c) { InitializeConditionVariable((PCONDITION_VARIABLE)c); }
void plat_cond_wait(PlatCond *c, PlatMutex *m) { SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)m, INFINITE, 0); }

int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns) {
    uint64_t ms = (ns + 999999) / 1000000;
    DWORD to = ms >= INFINITE ? INFINITE - 1 : (DWORD)ms;
    return SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)m, to, 0) ? 1 : GetLastError() != ERROR_TIMEOUT;
}

void plat_cond_signal(PlatCond *c) { WakeConditionVariable((PCONDITION_VARIABLE)c); }
void plat_cond_broadcast(PlatCond *c) { WakeAllConditionVariable((PCONDITION_VARIABLE)c); }

// ---- files

static int widen(const char *s, wchar_t *w, int cap) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, w, cap);
    if (!n) w[0] = 0;
    return n;
}

#define WPATH 2048

int plat_file_exists(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int plat_dir_exists(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int plat_full_path(const char *path, char *out, size_t cap) {
    wchar_t w[WPATH], full[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    DWORD n = GetFullPathNameW(w, WPATH, full, NULL);
    if (!n || n >= WPATH) return 0;
    return WideCharToMultiByte(CP_UTF8, 0, full, -1, out, (int)cap, NULL, NULL) != 0;
}

void plat_file_mkdir_p(const char *path) {
    wchar_t tmp[WPATH];
    if (!widen(path, tmp, WPATH)) return;
    for (wchar_t *p = tmp + 1; *p; p++) {
        if (*p == L'\\' || *p == L'/') {
            wchar_t c = *p;
            *p = 0;
            CreateDirectoryW(tmp, NULL);
            *p = c;
        }
    }
    CreateDirectoryW(tmp, NULL);
}

uint8_t *plat_file_read(const char *path, uint64_t *size) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return NULL;
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    uint8_t *buf = NULL;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (1ll << 30)) {
        buf = malloc((size_t)sz.QuadPart + 1);
        DWORD got = 0;
        if (buf && (!ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL) || got != sz.QuadPart)) {
            free(buf);
            buf = NULL;
        }
        if (buf) {
            buf[sz.QuadPart] = 0;
            *size = (uint64_t)sz.QuadPart;
        }
    }
    CloseHandle(h);
    return buf;
}

int plat_file_write(const char *path, const void *data, size_t n) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD put = 0;
    int ok = WriteFile(h, data, (DWORD)n, &put, NULL) && put == n;
    CloseHandle(h);
    return ok;
}

PlatAppend *plat_append_open(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return NULL;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    return h == INVALID_HANDLE_VALUE ? NULL : (PlatAppend *)h;
}

int plat_append_write(PlatAppend *f, const void *data, size_t n) {
    // Offset and OffsetHigh all ones: WriteFile appends at the end of the file, atomically against other writers.
    OVERLAPPED o = {.Offset = 0xffffffff, .OffsetHigh = 0xffffffff};
    DWORD put = 0;
    return WriteFile((HANDLE)f, data, (DWORD)n, &put, &o) && put == n;
}

int plat_file_replace(const char *from, const char *to, int durable) {
    wchar_t a[WPATH], b[WPATH];
    if (!widen(from, a, WPATH) || !widen(to, b, WPATH)) return 0;
    return MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | (durable ? MOVEFILE_WRITE_THROUGH : 0)) != 0;
}

static char lock_held[4][1100];
static HANDLE lock_handle[4];
static int lock_n;
static PlatMutex lock_lock = PLAT_MUTEX_INIT;

int plat_lock_file(const char *path) {
    plat_mutex_lock(&lock_lock);
    int r = 1, nheld = lock_n;
    for (int i = 0; i < nheld; i++)
        if (!strcmp(lock_held[i], path)) goto done;
    wchar_t w[WPATH];
    // no sharing at all: a second process's open fails with a sharing violation; the handle stays open until
    // plat_unlock_file or the process ends
    HANDLE h = widen(path, w, WPATH) ? CreateFileW(w, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                                                   FILE_ATTRIBUTE_NORMAL, NULL)
                                     : INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE) {
        r = GetLastError() == ERROR_SHARING_VIOLATION ? 0 : -1;
        goto done;
    }
    if (nheld < 4) {
        snprintf(lock_held[nheld], sizeof lock_held[0], "%s", path);
        lock_handle[nheld] = h;
        lock_n = nheld + 1;
    }
done:
    plat_mutex_unlock(&lock_lock);
    return r;
}

void plat_unlock_file(const char *path) {
    plat_mutex_lock(&lock_lock);
    for (int i = 0; i < lock_n; i++)
        if (!strcmp(lock_held[i], path)) {
            CloseHandle(lock_handle[i]);
            lock_n--;
            memmove(&lock_held[i], &lock_held[i + 1], sizeof lock_held[0] * (size_t)(lock_n - i));
            memmove(&lock_handle[i], &lock_handle[i + 1], sizeof lock_handle[0] * (size_t)(lock_n - i));
            break;
        }
    plat_mutex_unlock(&lock_lock);
}

// path.<pid>.<tid>.tmp renamed over path; durable: flushed to the disk before the rename, which is written through.
static int write_swap(const char *path, const void *data, size_t n, int durable) {
    char tmp[WPATH + 48];
    wchar_t a[WPATH], b[WPATH];
    int len = snprintf(tmp, sizeof tmp, "%s.%u.%u.tmp", path, (unsigned)GetCurrentProcessId(), (unsigned)GetCurrentThreadId());
    if (len < 0 || len >= (int)sizeof tmp || !widen(tmp, a, WPATH) || !widen(path, b, WPATH)) return 0;
    HANDLE h = CreateFileW(a, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    // In pieces: WriteFile takes a 32-bit count, so a block of 4 GB or more (a pipeline cache can grow that far)
    // would otherwise be cut to its low 32 bits and the write refused.
    int ok = 1;
    for (size_t at = 0; ok && at < n;) {
        DWORD piece = n - at > (1u << 30) ? (1u << 30) : (DWORD)(n - at), put = 0;
        ok = WriteFile(h, (const char *)data + at, piece, &put, NULL) && put == piece;
        at += piece;
    }
    ok = ok && (!durable || FlushFileBuffers(h));
    CloseHandle(h);
    if (ok && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | (durable ? MOVEFILE_WRITE_THROUGH : 0))) return 1;
    DeleteFileW(a);
    return 0;
}

int plat_file_write_atomic(const char *path, const void *data, size_t n) { return write_swap(path, data, n, 1); }

int plat_file_write_cached(const char *path, const void *data, size_t n) { return write_swap(path, data, n, 0); }

int plat_file_flush(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = FlushFileBuffers(h) != 0;
    CloseHandle(h);
    return ok;
}

int plat_file_sync(FILE *f) { return !fflush(f) && !_commit(_fileno(f)); }

void plat_file_delete(const char *path) {
    wchar_t w[WPATH];
    if (widen(path, w, WPATH)) DeleteFileW(w);
}

int plat_file_truncate(const char *path, uint64_t size) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER at = {.QuadPart = (LONGLONG)size};
    int ok = SetFilePointerEx(h, at, NULL, FILE_BEGIN) && SetEndOfFile(h);
    CloseHandle(h);
    return ok;
}

int64_t plat_file_size(const char *path) {
    wchar_t w[WPATH];
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!widen(path, w, WPATH) || !GetFileAttributesExW(w, GetFileExInfoStandard, &d)) return -1;
    return (int64_t)(((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow);
}

int plat_file_set_readonly(const char *path, int on) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    return SetFileAttributesW(w, on ? a | FILE_ATTRIBUTE_READONLY : a & ~(DWORD)FILE_ATTRIBUTE_READONLY) != 0;
}

// The file or folder gets an access list of its own with one entry, full control for the account the game runs as,
// and inherits none from its folder: a game folder outside the user's own folders is often readable by every account
// of the PC. Administrators can still take ownership, as on any file.
int plat_file_private(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH) || GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES) return 0;
    HANDLE tok = NULL;
    union { TOKEN_USER u; BYTE raw[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]; } user;
    DWORD got = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return 0;
    BOOL have = GetTokenInformation(tok, TokenUser, &user, sizeof user, &got);
    CloseHandle(tok);
    if (!have) return 0;
    union { ACL acl; BYTE raw[sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + SECURITY_MAX_SID_SIZE]; } list;
    if (!InitializeAcl(&list.acl, sizeof list, ACL_REVISION) ||
        !AddAccessAllowedAceEx(&list.acl, ACL_REVISION, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE, GENERIC_ALL, user.u.User.Sid))
        return 0;
    return SetNamedSecurityInfoW(w, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, NULL,
                                 NULL, &list.acl, NULL) == ERROR_SUCCESS;
}

int plat_file_list(const char *dir, const char *ext, int (*cb)(const char *name, void *ctx), void *ctx) {
    wchar_t wd[WPATH], we[64], pat[WPATH + 64];
    if (!widen(dir, wd, WPATH) || !widen(ext, we, 64)) return 0;
    swprintf(pat, WPATH + 64, L"%ls\\*%ls", wd, we);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char name[1024];
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL)) continue;
        if (!cb(name, ctx)) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 1;
}

// ---- virtual memory: VirtualAlloc reservations, MEM_WRITE_WATCH for the watched ones.

int plat_mem_reserve(uint64_t addr, uint64_t n, int watch) {
    void *p = VirtualAlloc((void *)(uintptr_t)addr, n, MEM_RESERVE | (watch ? MEM_WRITE_WATCH : 0), PAGE_NOACCESS);
    if (p && p != (void *)(uintptr_t)addr) {
        VirtualFree(p, 0, MEM_RELEASE);
        p = NULL;
    }
    return p != NULL;
}

void *plat_mem_alloc(size_t n) { return VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }

int plat_mem_commit(uint64_t addr, size_t n) {
    return VirtualAlloc((void *)(uintptr_t)addr, n, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

// Decommitted pages come back zeroed when committed again.
int plat_mem_decommit(uint64_t addr, uint64_t n) { return VirtualFree((void *)(uintptr_t)addr, n, MEM_DECOMMIT) != 0; }

int plat_mem_protect(uint64_t addr, uint64_t n, int prot) {
    DWORD old;
    DWORD p = prot == PLAT_PROT_RW ? PAGE_READWRITE : prot == PLAT_PROT_READ ? PAGE_READONLY : PAGE_NOACCESS;
    return VirtualProtect((void *)(uintptr_t)addr, n, p, &old) != 0;
}

int plat_mem_state(uint64_t addr, uint64_t limit, uint64_t *end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void *)(uintptr_t)addr, &mbi, sizeof mbi)) {
        *end = limit;
        return PLAT_MEM_FREE;
    }
    uint64_t e = (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    *end = e < limit ? e : limit;
    if (mbi.State == MEM_RESERVE) return PLAT_MEM_RESERVED;
    if (mbi.State != MEM_COMMIT) return PLAT_MEM_FREE;
    DWORD p = mbi.Protect & 0xff;
    return p == PAGE_READWRITE ? PLAT_PROT_RW : p == PAGE_READONLY ? PLAT_PROT_READ : PLAT_PROT_NONE;
}

int plat_mem_census(uint64_t *priv, uint64_t *wc, uint64_t *wc_allocs) {
    *priv = *wc = *wc_allocs = 0;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t a = 0;
    void *last = NULL;
    while (VirtualQuery((void *)a, &mbi, sizeof mbi) == sizeof mbi) {
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE) {
            *priv += mbi.RegionSize;
            if (mbi.Protect & PAGE_WRITECOMBINE) {
                *wc += mbi.RegionSize;
                if (mbi.AllocationBase != last) (*wc_allocs)++, last = mbi.AllocationBase;
            }
        }
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= a) break;
        a = next;
    }
    return 1;
}

int plat_mem_prepare_write(uint64_t addr, uint64_t n) {
    (void)addr; (void)n;
    return 0;
}

// Guest memory as dma-bufs is Linux's (platform.h).
int plat_mem_dmabuf_ok(uint64_t *limit, uint64_t *any, const char **why) { *limit = *any = 0, *why = ""; return 0; }
int plat_mem_dmabuf(uint64_t addr, uint64_t n) { (void)addr; (void)n; return -1; }
void plat_mem_dmabuf_done(uint64_t addr, uint64_t n) { (void)addr; (void)n; }

// Contains code Copyright (c) 2026 Peiikai (MIT; see THIRD_PARTY_NOTICES.md).
// GetWriteWatch with the reset flag.
uint64_t plat_mem_written(uint64_t addr, uint64_t n, uint64_t *pages, uint64_t max, int reset) {
    if (!max || !n) return 0;
    ULONG_PTR got = (ULONG_PTR)max;
    DWORD gran;
    _Static_assert(sizeof(PVOID) == sizeof(uint64_t), "page addresses are written straight into a uint64_t array");
    if (GetWriteWatch(reset ? WRITE_WATCH_FLAG_RESET : 0, (void *)(uintptr_t)addr, (SIZE_T)n, (PVOID *)pages, &got,
                      &gran))
        return UINT64_MAX;
    return got;
}

static int (*fault_hook)(uint64_t addr);
static int (*access_hook)(uint64_t addr, int write, uint64_t ip);

// A read or write access violation (ExceptionInformation[0] 0 or 1) goes to the access hook when there is one, then a
// write to the write-fault hook; registered first, before the crash handler.
static LONG CALLBACK fault_veh(EXCEPTION_POINTERS *ep) {
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 || er->ExceptionInformation[0] > 1)
        return EXCEPTION_CONTINUE_SEARCH;
    int write = er->ExceptionInformation[0] == 1;
    uint64_t addr = (uint64_t)er->ExceptionInformation[1];
    if (access_hook && access_hook(addr, write, (uint64_t)(uintptr_t)er->ExceptionAddress)) return EXCEPTION_CONTINUE_EXECUTION;
    if (!write) return EXCEPTION_CONTINUE_SEARCH;
    return fault_hook(addr) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

void plat_mem_access_hook(int (*fn)(uint64_t addr, int write, uint64_t ip)) { access_hook = fn; }

void plat_mem_fault_hook(int (*fn)(uint64_t addr)) {
    fault_hook = fn;
    AddVectoredExceptionHandler(1, fault_veh);
}

static void (*crash_fn)(const PlatCrash *c);

// A line written straight to the standard error handle, for the crash path: no C runtime buffering or locks.
static const char *image_name(const char *base);
static __thread const char *crash_ctx;
void plat_crash_context(const char *s) { crash_ctx = s; }
const char *plat_crash_context_get(void) { return crash_ctx; }
static void raw_note(const char *tag, DWORD code, uintptr_t rip, uintptr_t addr) {
    char b[256];
    void *base = NULL;
    RtlPcToFileHeader((void *)rip, &base);
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    int n = snprintf(b, sizeof b, "[host] %s: exception %#lx at %p (%s+%#llx), address %p, thread %lu, stack %llu KiB reserved\n", tag,
                     (unsigned long)code, (void *)rip, base ? image_name(base) : "no module",
                     (unsigned long long)(rip - (uintptr_t)base), (void *)addr, (unsigned long)GetCurrentThreadId(),
                     (unsigned long long)(hi - lo) >> 10);
    DWORD w;
    if (n > 0) WriteFile(GetStdHandle(STD_ERROR_HANDLE), b, (DWORD)n, &w, NULL);
    if (crash_ctx) {
        n = snprintf(b, sizeof b, "[host] %s: the thread was %s\n", tag, crash_ctx);
        if (n > 0) WriteFile(GetStdHandle(STD_ERROR_HANDLE), b, (DWORD)n, &w, NULL);
    }
}

static LONG CALLBACK crash_veh(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_ILLEGAL_INSTRUCTION)
        raw_note("crash_veh", code, (uintptr_t)ep->ContextRecord->Rip,
                 ep->ExceptionRecord->NumberParameters >= 2 ? (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1] : 0);
    if (code == EXCEPTION_ACCESS_VIOLATION && !ep->ContextRecord->Rip) {
        // a call through a null pointer: the call's return address is on top of the stack, in whichever module made it
        const uintptr_t *sp = (const uintptr_t *)ep->ContextRecord->Rsp;
        for (int i = 0; i < 4; i++) {
            void *base = NULL;
            if (!RtlPcToFileHeader((void *)sp[i], &base) || !base) continue;
            char b[200];
            int n = snprintf(b, sizeof b, "[host] crash_veh: called from %s+%#llx (stack word %d)\n", image_name(base),
                             (unsigned long long)(sp[i] - (uintptr_t)base), i);
            DWORD w;
            if (n > 0) WriteFile(GetStdHandle(STD_ERROR_HANDLE), b, (DWORD)n, &w, NULL);
        }
        // the caller's registers (the object a call through a table read its pointer from is often one of them)
        const CONTEXT *x = ep->ContextRecord;
        char b[600];
        int n = snprintf(b, sizeof b,
                         "[host] crash_veh: rax %#llx rbx %#llx rcx %#llx rdx %#llx rsi %#llx rdi %#llx rbp %#llx r8 %#llx "
                         "r9 %#llx r12 %#llx r13 %#llx r14 %#llx r15 %#llx\n",
                         (unsigned long long)x->Rax, (unsigned long long)x->Rbx, (unsigned long long)x->Rcx,
                         (unsigned long long)x->Rdx, (unsigned long long)x->Rsi, (unsigned long long)x->Rdi,
                         (unsigned long long)x->Rbp, (unsigned long long)x->R8, (unsigned long long)x->R9,
                         (unsigned long long)x->R12, (unsigned long long)x->R13, (unsigned long long)x->R14,
                         (unsigned long long)x->R15);
        DWORD w;
        if (n > 0) WriteFile(GetStdHandle(STD_ERROR_HANDLE), b, (DWORD)n, &w, NULL);
    }
    const char *what = code == EXCEPTION_ACCESS_VIOLATION ? "access violation"
                     : code == EXCEPTION_ILLEGAL_INSTRUCTION ? "illegal instruction"
                     : code == EXCEPTION_INT_DIVIDE_BY_ZERO ? "integer division by zero"
                     : code == EXCEPTION_PRIV_INSTRUCTION ? "privileged instruction"
                     : code == EXCEPTION_STACK_OVERFLOW ? "stack overflow"
                     : code == EXCEPTION_IN_PAGE_ERROR ? "in-page error" : NULL;
    if (!what) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;
    MODULEINFO mi;
    if (!GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(NULL), &mi, sizeof mi) ||
        rip - (uintptr_t)mi.lpBaseOfDll >= mi.SizeOfImage)
        return EXCEPTION_CONTINUE_SEARCH;   // not our code: leave it to the handler that expects it
    PlatCrash c = {what, (uint32_t)code, rip, rip - (uintptr_t)mi.lpBaseOfDll, -1, 0};
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        c.access = (int)ep->ExceptionRecord->ExceptionInformation[0], c.addr = ep->ExceptionRecord->ExceptionInformation[1];
    {   // the faulting host stack, so the report can name the runtime frame that took it
        void *f[32];
        USHORT n = RtlCaptureStackBackTrace(0, 32, f, NULL);
        for (USHORT i = 0; i < n && c.nframes < (int)(sizeof c.frames / sizeof *c.frames); i++)
            if ((uintptr_t)f[i] - (uintptr_t)mi.lpBaseOfDll < mi.SizeOfImage)
                c.frames[c.nframes++] = (uintptr_t)f[i];
    }
    crash_fn(&c);
    return EXCEPTION_CONTINUE_SEARCH;
}

// abort() and an invalid parameter to a C runtime function raise no exception: the C runtime ends the process with
// 0xc0000409 after calling the SIGABRT or invalid-parameter handler. The report names the host frames in this
// executable; the innermost is the caller of abort (assert) or of the C runtime function. Skipped frames: this function
// and the handler that called it.
__attribute__((noinline)) static void crash_from_runtime(const char *what, uint32_t code) {
    PlatCrash c = {what, code, 0, 0, -1, 0};
    void *f[62];
    USHORT n = RtlCaptureStackBackTrace(2, 62, f, NULL);
    MODULEINFO mi;
    if (GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(NULL), &mi, sizeof mi)) {
        uintptr_t base = (uintptr_t)mi.lpBaseOfDll;
        for (USHORT i = 0; i < n && c.nframes < (int)(sizeof c.frames / sizeof *c.frames); i++)
            if ((uintptr_t)f[i] - base < mi.SizeOfImage) c.frames[c.nframes++] = (uintptr_t)f[i];
        if (c.nframes) c.rip = c.frames[0], c.offset = c.rip - base;
    }
    crash_fn(&c);
}

// The name a DLL records for itself in its export directory ("amdvlk64.dll"). Read from its mapped headers, not through
// the loader, whose lock another thread may hold when the process crashes.
static const char *image_name(const char *base) {
    const IMAGE_NT_HEADERS64 *nt = (const void *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY *d = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!d->VirtualAddress || !d->Size) return "unnamed module";
    return base + ((const IMAGE_EXPORT_DIRECTORY *)(base + d->VirtualAddress))->Name;
}

// The return addresses in [lo, lo + size) on the crashed thread's stack, innermost first: unwinds the faulting context
// with the x64 unwind tables; a function without a table entry (a leaf, or address 0 after a call through a null
// pointer) has its return address on top of the stack. The walk stops at return address 0 (the thread's first frame)
// and when the stack pointer leaves the thread's stack, so a damaged stack ends it instead of faulting.
static int frames_in(const CONTEXT *at, uintptr_t lo, uintptr_t size, uintptr_t *out, int max) {
    CONTEXT c = *at;
    ULONG_PTR low, high;
    GetCurrentThreadStackLimits(&low, &high);
    int n = 0;
    for (int i = 0; i < 256 && n < max && (c.Rip || !i) && c.Rsp >= low && c.Rsp + 8 <= high; i++) {
        DWORD64 img;
        RUNTIME_FUNCTION *fe = RtlLookupFunctionEntry(c.Rip, &img, NULL);
        if (fe) {
            void *data;
            DWORD64 frame;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, img, c.Rip, fe, &c, &data, &frame, NULL);
        } else {
            c.Rip = *(const DWORD64 *)c.Rsp;
            c.Rsp += 8;
        }
        if (c.Rip - lo < size) out[n++] = (uintptr_t)c.Rip;
    }
    return n;
}

// The last-chance filter: an exception no handler took, in any module. crash_veh reports faults in this executable as
// they happen; this one catches the rest, which used to end the process with no line in the log: a fault in a GPU
// driver's own thread or in a driver call (vkQueueSubmit, vkAllocateMemory), a C++ exception a library let escape, a
// call through a null pointer (rip 0, outside every module). Windows calls it only after every handler declined, so
// the exceptions drivers raise and catch themselves never get here; it costs nothing until a crash.
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    DWORD code = er->ExceptionCode;
    uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;
    raw_note("crash_filter", code, rip, er->NumberParameters >= 2 ? (uintptr_t)er->ExceptionInformation[1] : 0);
    PlatCrash c = {code == EXCEPTION_ACCESS_VIOLATION        ? "access violation"
                   : code == EXCEPTION_ILLEGAL_INSTRUCTION   ? "illegal instruction"
                   : code == EXCEPTION_INT_DIVIDE_BY_ZERO    ? "integer division by zero"
                   : code == EXCEPTION_PRIV_INSTRUCTION      ? "privileged instruction"
                   : code == EXCEPTION_STACK_OVERFLOW        ? "stack overflow"
                   : code == EXCEPTION_IN_PAGE_ERROR         ? "in-page error"
                   : code == EXCEPTION_BREAKPOINT            ? "breakpoint"
                   : code == 0xe06d7363u                     ? "C++ exception"
                   : code == 0xc0000374u                     ? "heap corruption"   // STATUS_HEAP_CORRUPTION
                                                             : "unhandled exception",
                   (uint32_t)code, rip, 0, -1, 0};
    if (code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        c.access = (int)er->ExceptionInformation[0], c.addr = er->ExceptionInformation[1];
    void *base = NULL;
    RtlPcToFileHeader((void *)rip, &base);
    MODULEINFO mi;
    uintptr_t exe = (uintptr_t)GetModuleHandleW(NULL);
    c.offset = rip - (uintptr_t)base;
    if (!base) c.module = "";
    else if ((uintptr_t)base != exe) c.module = image_name(base);
    if (c.module && GetModuleInformation(GetCurrentProcess(), (HMODULE)exe, &mi, sizeof mi))
        c.nframes = frames_in(ep->ContextRecord, exe, mi.SizeOfImage, c.frames,
                              (int)(sizeof c.frames / sizeof *c.frames));
    crash_fn(&c);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void abort_signal(int sig) { crash_from_runtime("abort", (uint32_t)sig); }

static void invalid_parameter(const wchar_t *expr, const wchar_t *func, const wchar_t *file, unsigned line,
                              uintptr_t reserved) {
    (void)expr, (void)func, (void)file, (void)line, (void)reserved;   // NULL and 0 in the release C runtime
    crash_from_runtime("invalid parameter to a C runtime function", 0xc0000417u);   // STATUS_INVALID_CRUNTIME_PARAMETER
}

void plat_crash_hook(void (*fn)(const PlatCrash *c)) {
    crash_fn = fn;
    AddVectoredExceptionHandler(0, crash_veh);
    SetUnhandledExceptionFilter(crash_filter);   // replaces the C runtime's filter; crash_fn never returns
    signal(SIGABRT, abort_signal);
    _set_invalid_parameter_handler(invalid_parameter);
}

// The last-chance filter is set once, at the start, before the GPU driver, the window and audio libraries and the
// movie decoder load. A DLL that sets its own filter later takes the place of crash_filter, and a fault outside this
// executable then ends the process with no line in the log. This puts crash_filter back; when another filter had
// taken its place, `who` gets the module and offset of that filter and the call returns 1.
int plat_crash_filter_reassert(char *who, size_t n) {
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(crash_filter);
    if (prev == crash_filter) return 0;
    void *base = NULL;
    if (prev) RtlPcToFileHeader((void *)prev, &base);
    if (!prev) snprintf(who, n, "no filter (removed)");
    else if (!base) snprintf(who, n, "%p, outside every module", (void *)prev);
    else snprintf(who, n, "%s+%#llx", image_name(base), (unsigned long long)((uintptr_t)prev - (uintptr_t)base));
    return 1;
}

// A fresh process gets a fresh layout: Windows places DLLs, heaps and thread stacks at random addresses. The parent
// waits and exits with the child's code.
void plat_reexec(void) {
    fprintf(stderr, "[host] memory: a fixed region is taken; re-running the process\n");
    fflush(stdout);
    fflush(stderr);
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    wchar_t *cmd = _wcsdup(GetCommandLineW());   // CreateProcessW may write its command line
    if (!cmd || !CreateProcessW(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "FATAL: re-running the process failed (error %lu)\n", GetLastError());
        ExitProcess(70);
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 70;
    GetExitCodeProcess(pi.hProcess, &code);
    ExitProcess(code);
}

int plat_mem_read(uint64_t addr, void *out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void *)(uintptr_t)addr, out, n, &got) && got == n;
}

int plat_mem_readable(uint64_t addr, uint64_t n) {
    for (uint64_t a = addr, end = addr + n; a < end || a == addr;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
        a = (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (a >= end) return 1;
    }
    return 1;
}

void *plat_aligned_alloc(size_t size, size_t align) { return _aligned_malloc(size, align); }
void plat_aligned_free(void *p) { _aligned_free(p); }

// ---- process

#ifdef NX2NAVI_PGO_GEN
// compiler-rt's profile runtime (an instrumented build: --pgo gen of tools/make_game.py or tools/build_game.py). It
// writes its counters from an atexit handler, which ExitProcess skips; __llvm_profile_dump writes them once and makes
// later writes no-ops.
int __llvm_profile_dump(void);
int __llvm_profile_write_file(void);
void __llvm_profile_reset_counters(void);

// A long played session must not lose its counts to a crash or a reboot: every minute the counters are added to the
// .profraw (the file merges, -fprofile-generate's %m) and zeroed. Counts taken between the write and the reset are lost.
static DWORD WINAPI pgo_flush_thread(void *arg) {
    (void)arg;
    for (;;) {
        Sleep(60 * 1000);
        if (__llvm_profile_write_file() == 0) __llvm_profile_reset_counters();
    }
}

__attribute__((constructor)) static void pgo_flush_start(void) {
    HANDLE h = CreateThread(NULL, 0, pgo_flush_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}
#endif

// ---- the console window (plat_console_setup)
static HANDLE con_in = INVALID_HANDLE_VALUE;   // the console's input, while its QuickEdit mode is off; con_mode as it was
static DWORD con_mode;
static void (*con_close)(const char *what);
static volatile LONG con_closing;

// Runs on a thread the system starts for the event. A close returns only when on_close does, which it must not.
static BOOL WINAPI con_ctrl(DWORD ev) {
    const char *what = ev == CTRL_CLOSE_EVENT ? "closed" : ev == CTRL_C_EVENT ? "Ctrl+C"
                     : ev == CTRL_BREAK_EVENT ? "Ctrl+Break" : NULL;
    // log-off and shutdown reach a process with a window as WM_ENDSESSION (gfx_window_win.c), not here; a second event
    // while on_close runs takes the default path, which ends the process at once
    if (!what || !con_close || InterlockedExchange(&con_closing, 1)) return FALSE;
    con_close(what);
    return TRUE;
}

int plat_console_setup(void (*on_close)(const char *what)) {
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    DWORD m;
    if (!err || err == INVALID_HANDLE_VALUE || !GetConsoleMode(err, &m)) return 0;
    // CONIN$: the console's input even when stdin was redirected. ENABLE_EXTENDED_FLAGS makes the call change the
    // QuickEdit bit at all.
    HANDLE in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, 0, NULL);
    if (in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &con_mode) && (con_mode & ENABLE_QUICK_EDIT_MODE) &&
        SetConsoleMode(in, (con_mode & ~(DWORD)ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS))
        con_in = in;
    else if (in != INVALID_HANDLE_VALUE)
        CloseHandle(in);
    con_close = on_close;
    if (on_close) SetConsoleCtrlHandler(con_ctrl, TRUE);
    return 1;
}

int plat_stderr_write(const void *p, size_t n) {
    int fd = _fileno(stderr);   // the descriptor fwrite(stderr) writes to; negative when the process has no stderr
    if (fd < 0) return 0;
    for (const char *c = p; n;) {
        int k = _write(fd, c, n > (1u << 30) ? 1u << 30 : (unsigned)n);
        if (k <= 0) return 0;
        c += k, n -= (size_t)k;
    }
    return 1;
}

void (*plat_exit_hook)(void);

void plat_exit(int code) {
    if (plat_exit_hook) plat_exit_hook();
#ifdef NX2NAVI_PGO_GEN
    __llvm_profile_dump();
#endif
    // QuickEdit back on in a console that outlives the process (the one of the shell that started it)
    if (con_in != INVALID_HANDLE_VALUE) SetConsoleMode(con_in, con_mode | ENABLE_EXTENDED_FLAGS);
    fflush(NULL);   // every open stream: nothing below runs the C runtime's own exit
    // The process ends here, without the libraries' detach calls that ExitProcess makes. The callers have saved what
    // is to be kept (gfx_present.c gfx_window_closed: settings, pipeline cache, profile, saves), and the game's other
    // threads are still running when this is called. ExitProcess stops them wherever they are and then runs every
    // library's detach on this thread: NVIDIA's NGX core, which still held the device and the DLSS features, read a
    // dead pointer in its detach on the window thread after the window was closed (an access violation in
    // _nvngx.dll after the last save, once in 5 scripted runs that had freed and made a DLSS feature again), and the
    // crash handler wrote a FATAL for a run that had ended cleanly.
    TerminateProcess(GetCurrentProcess(), (UINT)code);
    ExitProcess((UINT)code);   // not reached
}

// _putenv_s, not SetEnvironmentVariableA: getenv reads the C runtime's copy of the environment, which only the CRT's
// own calls update. An empty value removes the variable.
int plat_setenv(const char *name, const char *value) { return _putenv_s(name, value ? value : "") == 0; }

uint32_t plat_process_id(void) { return GetCurrentProcessId(); }

static uint64_t ft64(FILETIME f) { return (uint64_t)f.dwHighDateTime << 32 | f.dwLowDateTime; }

uint64_t plat_process_cpu_ns(void) {
    FILETIME cr, ex, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &cr, &ex, &k, &u)) return 0;
    return (ft64(k) + ft64(u)) * 100;
}

uint64_t plat_process_uptime_ns(void) {
    FILETIME cr, ex, k, u, now;
    if (!GetProcessTimes(GetCurrentProcess(), &cr, &ex, &k, &u)) return 0;
    GetSystemTimeAsFileTime(&now);
    return ft64(now) > ft64(cr) ? (ft64(now) - ft64(cr)) * 100 : 0;
}

uint64_t plat_unix_time_ns(void) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return (ft64(now) - 116444736000000000ull) * 100;   // FILETIME counts 100 ns from 1601
}

void plat_local_time(struct tm *out) {
    time_t t = time(NULL);
    localtime_s(out, &t);
}

int plat_localtime(int64_t unix_s, struct tm *out) {
    time_t t = (time_t)unix_s;
    return !localtime_s(out, &t);
}

int plat_gmtime(int64_t unix_s, struct tm *out) {
    time_t t = (time_t)unix_s;
    return !gmtime_s(out, &t);
}

long plat_tz_west(void) {
    long west = 0;
    _tzset();
    _get_timezone(&west);
    return west;
}

int plat_run_self(const char *env_name, const char *env_value, char *out, size_t cap, uint32_t timeout_ms) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES, .hStdInput = GetStdHandle(STD_INPUT_HANDLE),
                       .hStdOutput = wr, .hStdError = wr};
    PROCESS_INFORMATION pi;
    wchar_t *cmdline = _wcsdup(GetCommandLineW());
    SetEnvironmentVariableA(env_name, env_value);
    BOOL ok = cmdline && CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    SetEnvironmentVariableA(env_name, NULL);
    free(cmdline);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }
    size_t len = 0;
    DWORD got;
    char sink[512];
    while (len + 1 < cap && ReadFile(rd, out + len, (DWORD)(cap - 1 - len), &got, NULL) && got) len += got;
    if (cap) out[len] = 0;
    while (ReadFile(rd, sink, sizeof sink, &got, NULL) && got) {}
    CloseHandle(rd);
    if (WaitForSingleObject(pi.hProcess, timeout_ms ? timeout_ms : INFINITE) != WAIT_OBJECT_0)
        TerminateProcess(pi.hProcess, 1), WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

int plat_random(void *out, size_t n) {
    return BCryptGenRandom(NULL, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

// FILE_FLAG_OVERLAPPED handles: a read names its offset, and each thread waits on an event of its own, so concurrent
// reads on one handle do not share the file pointer or the event.
PlatFile *plat_file_open(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return NULL;
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    return h == INVALID_HANDLE_VALUE ? NULL : (PlatFile *)h;
}

PlatFile *plat_file_open_unbuffered(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return NULL;
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING, NULL);
    return h == INVALID_HANDLE_VALUE ? NULL : (PlatFile *)h;
}

void plat_file_close(PlatFile *f) {
    if (f) CloseHandle((HANDLE)f);
}

int64_t plat_file_pread(PlatFile *f, void *buf, uint32_t n, uint64_t off) {
    HANDLE ev = pread_ev;
    if (!ev && !(ev = pread_ev = CreateEventW(NULL, TRUE, FALSE, NULL))) return -1;
    OVERLAPPED ov = {.Offset = (DWORD)off, .OffsetHigh = (DWORD)(off >> 32), .hEvent = ev};
    DWORD got = 0;
    if (ReadFile((HANDLE)f, buf, n, &got, &ov)) return got;
    DWORD e = GetLastError();
    if (e == ERROR_IO_PENDING) {
        if (GetOverlappedResult((HANDLE)f, &ov, &got, TRUE)) return got;
        e = GetLastError();
    }
    return e == ERROR_HANDLE_EOF ? 0 : e == ERROR_NOACCESS ? -2 : -1;
}

int plat_file_id(PlatFile *f, PlatFileId *out) {
    BY_HANDLE_FILE_INFORMATION i;
    if (!GetFileInformationByHandle((HANDLE)f, &i)) return 0;
    *out = (PlatFileId){i.dwVolumeSerialNumber, (uint64_t)i.nFileIndexHigh << 32 | i.nFileIndexLow,
                        (uint64_t)i.nFileSizeHigh << 32 | i.nFileSizeLow,
                        (uint64_t)i.ftLastWriteTime.dwHighDateTime << 32 | i.ftLastWriteTime.dwLowDateTime,
                        (uint64_t)i.ftCreationTime.dwHighDateTime << 32 | i.ftCreationTime.dwLowDateTime};
    return 1;
}

const void *plat_file_map(PlatFile *f) {
    HANDLE map = CreateFileMappingW((HANDLE)f, NULL, PAGE_READONLY, 0, 0, NULL);
    const void *v = map ? MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0) : NULL;
    if (map) CloseHandle(map);   // the view keeps the mapping alive
    return v;
}

static __thread DWORD fs_last_error;
int plat_fs_os_error(void) { return (int)fs_last_error; }

static int fs_err(DWORD e) {
    fs_last_error = e;
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_DIRECTORY: return PLAT_FS_NOT_FOUND;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return PLAT_FS_EXISTS;
    case ERROR_DIR_NOT_EMPTY: return PLAT_FS_NOT_EMPTY;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: return PLAT_FS_BUSY;
    default: return PLAT_FS_ERROR;
    }
}

// A symbolic link or junction is a link: removed as a link, never followed. Any other reparse point (OneDrive's cloud
// placeholders, deduplicated files) is an ordinary file or directory whose content the file system fetches on access.
static int fs_kind_of(const WIN32_FIND_DATAW *fd) {
    DWORD a = fd->dwFileAttributes;
    if (a & FILE_ATTRIBUTE_REPARSE_POINT && (fd->dwReserved0 == IO_REPARSE_TAG_SYMLINK || fd->dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT))
        return PLAT_FS_LINK;
    return a & FILE_ATTRIBUTE_DIRECTORY ? PLAT_FS_DIR : PLAT_FS_FILE;
}

int plat_fs_kind(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return PLAT_FS_MISSING;
    DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES) return PLAT_FS_MISSING;
    if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) return a & FILE_ATTRIBUTE_DIRECTORY ? PLAT_FS_DIR : PLAT_FS_FILE;
    // the reparse tag comes with the find data only; FindFirstFileW is asked only here, since it fails on a path
    // with a trailing separator and reads wildcards
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    if (h == INVALID_HANDLE_VALUE) return PLAT_FS_LINK;
    FindClose(h);
    return fs_kind_of(&fd);
}

int plat_fs_mkdir(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return PLAT_FS_ERROR;
    return CreateDirectoryW(w, NULL) ? PLAT_FS_OK : fs_err(GetLastError());
}

int plat_fs_rmdir(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return PLAT_FS_ERROR;
    if (RemoveDirectoryW(w)) return PLAT_FS_OK;
    DWORD e = GetLastError(), a = GetFileAttributesW(w);
    if (e == ERROR_ACCESS_DENIED && a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
        // a read-only directory (copied over from another emulator's save folder) refuses RemoveDirectoryW
        SetFileAttributesW(w, a & ~(DWORD)FILE_ATTRIBUTE_READONLY);
        if (RemoveDirectoryW(w)) return PLAT_FS_OK;
        e = GetLastError();
    }
    return fs_err(e);
}

int plat_fs_unlink(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return PLAT_FS_ERROR;
    DWORD a = GetFileAttributesW(w);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(w, a & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    return DeleteFileW(w) ? PLAT_FS_OK : fs_err(GetLastError());
}

int plat_fs_rename(const char *from, const char *to, int durable) {
    wchar_t a[WPATH], b[WPATH];
    if (!widen(from, a, WPATH) || !widen(to, b, WPATH)) return PLAT_FS_ERROR;
    return MoveFileExW(a, b, durable ? MOVEFILE_WRITE_THROUGH : 0) ? PLAT_FS_OK : fs_err(GetLastError());
}

int plat_fs_link(const char *existing, const char *path) {
    wchar_t a[WPATH], b[WPATH];
    if (!widen(existing, a, WPATH) || !widen(path, b, WPATH)) return PLAT_FS_ERROR;
    return CreateHardLinkW(b, a, NULL) ? PLAT_FS_OK : fs_err(GetLastError());
}

int plat_fs_links(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    HANDLE f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    BY_HANDLE_FILE_INFORMATION fi;
    int links = f != INVALID_HANDLE_VALUE && GetFileInformationByHandle(f, &fi) ? (int)fi.nNumberOfLinks : 0;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    return links;
}

// NTFS journals directory entries; flushing the directory handle is as far as Windows goes.
void plat_fs_sync_dir(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(h);
        CloseHandle(h);
    }
}

// Restart Manager (rstrtmgr.dll, loaded on first use) names the processes that hold a file open.
typedef DWORD(WINAPI *RmStartSession_t)(DWORD *, DWORD, WCHAR *);
typedef DWORD(WINAPI *RmRegisterResources_t)(DWORD, UINT, LPCWSTR *, UINT, RM_UNIQUE_PROCESS *, UINT, LPCWSTR *);
typedef DWORD(WINAPI *RmGetList_t)(DWORD, UINT *, UINT *, RM_PROCESS_INFO *, LPDWORD);
typedef DWORD(WINAPI *RmEndSession_t)(DWORD);
static struct {
    INIT_ONCE once;
    RmStartSession_t start;
    RmRegisterResources_t add;
    RmGetList_t list;
    RmEndSession_t end;
} rm = {INIT_ONCE_STATIC_INIT};

static BOOL CALLBACK rm_load(INIT_ONCE *o, void *p, void **c) {
    (void)o, (void)p, (void)c;
    HMODULE l = plat_lib_open("rstrtmgr.dll");
    if (!l) return TRUE;
    rm.add = (RmRegisterResources_t)(void *)GetProcAddress(l, "RmRegisterResources");
    rm.list = (RmGetList_t)(void *)GetProcAddress(l, "RmGetList");
    rm.end = (RmEndSession_t)(void *)GetProcAddress(l, "RmEndSession");
    RmStartSession_t start = (RmStartSession_t)(void *)GetProcAddress(l, "RmStartSession");
    if (rm.add && rm.list && rm.end) rm.start = start;   // start set: all of them are there
    return TRUE;
}

// The files under dir (links not followed), at most BUSY_FILES of them, each a malloc'd path.
#define BUSY_FILES 256
typedef struct { LPCWSTR p[BUSY_FILES]; UINT n; } BusyFiles;

static void busy_files(const wchar_t *dir, BusyFiles *f) {
    wchar_t pat[WPATH + 4];
    swprintf(pat, WPATH + 4, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        DWORD a = fd.dwFileAttributes;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || (a & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        size_t n = wcslen(dir) + wcslen(fd.cFileName) + 2;
        wchar_t *p = n <= WPATH ? malloc(n * sizeof *p) : NULL;
        if (!p) continue;
        swprintf(p, n, L"%ls\\%ls", dir, fd.cFileName);
        if (a & FILE_ATTRIBUTE_DIRECTORY) busy_files(p, f), free(p);
        else f->p[f->n++] = p;
    } while (f->n < BUSY_FILES && FindNextFileW(h, &fd));
    FindClose(h);
}

void plat_fs_busy_reason(const char *dir, char *out, size_t cap) {
    if (!cap) return;
    *out = 0;
    wchar_t w[WPATH];
    if (!widen(dir, w, WPATH)) return;
    for (wchar_t *q = w; *q; q++)
        if (*q == L'/') *q = L'\\';
    // a rename needs DELETE access to the directory: a folder made by an elevated run can refuse it
    HANDLE h = CreateFileW(w, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_ACCESS_DENIED) snprintf(out, cap, "no permission to rename it");
        return;
    }
    CloseHandle(h);
    InitOnceExecuteOnce(&rm.once, rm_load, NULL, NULL);
    DWORD session;
    WCHAR key[CCH_RM_SESSION_KEY + 1] = {0};
    if (!rm.start || rm.start(&session, 0, key) != ERROR_SUCCESS) return;
    BusyFiles f = {.n = 0};
    busy_files(w, &f);
    RM_PROCESS_INFO info[8];
    UINT need = 0, n = 8;
    DWORD reasons = 0, e = f.n ? rm.add(session, f.n, f.p, 0, NULL, 0, NULL) : ERROR_SUCCESS;
    if (f.n && e == ERROR_SUCCESS) e = rm.list(session, &need, &n, info, &reasons);
    else n = 0;
    rm.end(session);
    for (UINT i = 0; i < f.n; i++) free((void *)f.p[i]);
    if (e == ERROR_MORE_DATA) snprintf(out, cap, "%u programs hold files in it", need);   // info is not filled then
    if (e != ERROR_SUCCESS) return;
    size_t at = 0;
    for (UINT i = 0; i < n && at < cap; i++) {   // the program's display name ("Windows Explorer") and process id
        char name[CCH_RM_MAX_APP_NAME + 1];
        if (!WideCharToMultiByte(CP_UTF8, 0, info[i].strAppName, -1, name, sizeof name, NULL, NULL)) strcpy(name, "?");
        int k = snprintf(out + at, cap - at, "%s%s %lu", i ? ", " : "", name, info[i].Process.dwProcessId);
        if (k < 0) break;
        at += (size_t)k;
    }
}

int plat_fs_list(const char *dir, int (*cb)(const char *name, int kind, uint64_t size, void *ctx), void *ctx) {
    wchar_t wd[WPATH], pat[WPATH + 4];
    if (!widen(dir, wd, WPATH)) return PLAT_FS_ERROR;
    swprintf(pat, WPATH + 4, L"%ls\\*", wd);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return fs_err(GetLastError());
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        char name[1024];
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL)) continue;
        int kind = fs_kind_of(&fd);
        if (!cb(name, kind, (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow, ctx)) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return PLAT_FS_OK;
}

static PlatFile *fs_open(const char *path, DWORD access, DWORD share, DWORD how, int *err) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) {
        if (err) *err = PLAT_FS_ERROR;
        return NULL;
    }
    HANDLE h = CreateFileW(w, access, share, NULL, how, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = fs_err(GetLastError());
        return NULL;
    }
    return (PlatFile *)h;
}

PlatFile *plat_fs_open(const char *path, int write, int *err) {
    return fs_open(path, GENERIC_READ | (write ? GENERIC_WRITE : 0),
                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, err);
}

PlatFile *plat_fs_create(const char *path, int *err) { return fs_open(path, GENERIC_WRITE, 0, CREATE_NEW, err); }

int64_t plat_file_pwrite(PlatFile *f, const void *buf, uint32_t n, uint64_t off) {
    HANDLE ev = pwrite_ev;
    if (!ev && !(ev = pwrite_ev = CreateEventW(NULL, TRUE, FALSE, NULL))) return -1;
    OVERLAPPED ov = {.Offset = (DWORD)off, .OffsetHigh = (DWORD)(off >> 32), .hEvent = ev};
    DWORD put = 0;
    if (WriteFile((HANDLE)f, buf, n, &put, &ov)) return put;
    if (GetLastError() == ERROR_IO_PENDING && GetOverlappedResult((HANDLE)f, &ov, &put, TRUE)) return put;
    return -1;
}

int plat_file_set_size(PlatFile *f, uint64_t size) {
    FILE_END_OF_FILE_INFO eof = {.EndOfFile.QuadPart = (LONGLONG)size};
    return SetFileInformationByHandle((HANDLE)f, FileEndOfFileInfo, &eof, sizeof eof) ? PLAT_FS_OK
                                                                                      : fs_err(GetLastError());
}

int plat_file_fsync(PlatFile *f) { return FlushFileBuffers((HANDLE)f) != 0; }

int plat_temp_dir(char *out, size_t cap) {
    wchar_t w[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, w);
    if (!n || n > MAX_PATH) return 0;
    while (n && (w[n - 1] == L'\\' || w[n - 1] == L'/')) w[--n] = 0;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) != 0;
}

uintptr_t plat_exe_base(void) { return (uintptr_t)GetModuleHandleW(NULL); }

int plat_module_of(uintptr_t addr, char *name, size_t cap, uintptr_t *base) {
    HMODULE m = NULL;
    wchar_t w[WPATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)addr, &m) ||
        !m || !GetModuleFileNameW(m, w, WPATH))
        return 0;
    const wchar_t *n = wcsrchr(w, L'\\') ? wcsrchr(w, L'\\') + 1 : w;
    *base = (uintptr_t)m;
    return WideCharToMultiByte(CP_UTF8, 0, n, -1, name, (int)cap, NULL, NULL) != 0;
}

int plat_exe_path(char *out, size_t cap) {
    wchar_t w[WPATH];
    DWORD n = GetModuleFileNameW(NULL, w, WPATH);
    if (!n || n >= WPATH) return 0;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) != 0;
}

size_t plat_getenv(const char *name, char *out, size_t cap) {
    wchar_t wn[256], v[WPATH];
    if (!widen(name, wn, 256)) return 0;
    DWORD n = GetEnvironmentVariableW(wn, v, WPATH);
    if (!n || n >= WPATH) return 0;
    int m = WideCharToMultiByte(CP_UTF8, 0, v, -1, out, (int)cap, NULL, NULL);
    return m ? (size_t)m - 1 : 0;
}

uint32_t plat_thread_id(void) { return GetCurrentThreadId(); }

const char *plat_ui_language(void) {
    static char tag[LOCALE_NAME_MAX_LENGTH];
    static int done;
    if (!done) {
        wchar_t w[LOCALE_NAME_MAX_LENGTH];
        LCID lcid = GetUserDefaultUILanguage();
        int n = LCIDToLocaleName(lcid, w, LOCALE_NAME_MAX_LENGTH, 0);
        if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, w, -1, tag, sizeof tag, NULL, NULL)) tag[0] = 0;
        done = 1;
    }
    return tag;
}

const char *plat_user_locale(void) {
    static char tag[LOCALE_NAME_MAX_LENGTH];
    static int done;
    if (!done) {
        wchar_t w[LOCALE_NAME_MAX_LENGTH];
        size_t i = 0;
        if (GetUserDefaultLocaleName(w, LOCALE_NAME_MAX_LENGTH))
            for (; w[i] && w[i] < 0x80 && i + 1 < sizeof tag; i++) tag[i] = (char)w[i];
        tag[i] = 0;
        done = 1;
    }
    return tag;
}

int plat_font_path(const char *lang, char *out, size_t cap) {
    static const struct { const char *lang; const wchar_t *files[3]; } fonts[] = {
        {"ja", {L"YuGothM.ttc", L"meiryo.ttc", L"msgothic.ttc"}},
        {"ko", {L"malgun.ttf", L"gulim.ttc", NULL}},
        {"zh-Hans", {L"msyh.ttc", L"simsun.ttc", NULL}},
        {"zh-Hant", {L"msjh.ttc", L"mingliu.ttc", NULL}},
    };
    wchar_t dir[MAX_PATH], w[WPATH];
    UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    for (size_t i = 0; i < sizeof fonts / sizeof *fonts; i++) {
        if (strcmp(fonts[i].lang, lang)) continue;
        for (int k = 0; k < 3 && fonts[i].files[k]; k++) {
            swprintf(w, WPATH, L"%ls\\Fonts\\%ls", dir, fonts[i].files[k]);
            DWORD a = GetFileAttributesW(w);
            if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY))
                return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) != 0;
        }
    }
    return 0;
}

int plat_os_version(char *out, size_t cap) {
    // GetVersionEx reports the version the manifest asks for; ntdll's RtlGetVersion the real one
    typedef LONG(WINAPI * RtlGetVersion_t)(OSVERSIONINFOW *);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersion_t get = nt ? (RtlGetVersion_t)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
    OSVERSIONINFOW v = {.dwOSVersionInfoSize = sizeof v};
    if (!get || get(&v) != 0) return 0;
    DWORD ubr = 0, size = sizeof ubr;   // the update build revision, the number after the build
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD,
                     NULL, &ubr, &size) != ERROR_SUCCESS)
        ubr = 0;
    snprintf(out, cap, ubr ? "Windows %lu.%lu.%lu.%lu" : "Windows %lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion,
             v.dwBuildNumber, ubr);
    return 1;
}

int plat_is_wine(void) {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    return nt && GetProcAddress(nt, "wine_get_version") != NULL;
}

int plat_stderr_path(char *out, size_t cap) {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    wchar_t w[WPATH];
    if (!h || h == INVALID_HANDLE_VALUE || GetFileType(h) != FILE_TYPE_DISK) return 0;
    DWORD n = GetFinalPathNameByHandleW(h, w, WPATH, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n || n >= WPATH) return 0;
    const wchar_t *p = wcsncmp(w, L"\\\\?\\", 4) ? w : w + 4;   // the \\?\ prefix
    return WideCharToMultiByte(CP_UTF8, 0, p, -1, out, (int)cap, NULL, NULL) != 0;
}

// ---- desktop

int plat_clipboard_set(const char *utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0) return 0;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)n * sizeof(wchar_t));
    if (!mem) return 0;
    wchar_t *w = GlobalLock(mem);
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
    GlobalUnlock(mem);
    // A clipboard opened without a window gets no owner, and SetClipboardData may then fail: a message-only window of
    // the calling thread owns it for the call.
    HWND owner = CreateWindowExW(0, L"STATIC", NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
    int ok = 0;
    if (OpenClipboard(owner)) {
        ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, mem) != NULL;
        CloseClipboard();
    }
    if (owner) DestroyWindow(owner);
    if (!ok) GlobalFree(mem);   // after a successful SetClipboardData the system owns it
    return ok;
}

int plat_clipboard_get(char *out, size_t cap) {
    if (!cap) return 0;
    out[0] = 0;
    if (!OpenClipboard(NULL)) return 0;   // reading needs no owner window
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *w = h ? GlobalLock(h) : NULL;
    int n = w ? WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) : 0;   // 0 when it does not fit
    if (w) GlobalUnlock(h);
    CloseClipboard();
    if (n <= 0) out[0] = 0;
    return out[0] != 0;
}

static DWORD WINAPI open_folder_main(void *arg) {
    ShellExecuteW(NULL, L"open", arg, NULL, NULL, SW_SHOWNORMAL);
    free(arg);
    return 0;
}

int plat_open_folder(const char *path) {
    wchar_t *w = malloc(WPATH * sizeof *w);
    if (!w || !widen(path, w, WPATH)) return free(w), 0;
    DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return free(w), 0;
    // the shell can take a while to answer: not on the caller's thread (the present path)
    HANDLE t = CreateThread(NULL, 0, open_folder_main, w, 0, NULL);
    if (!t) return free(w), 0;
    CloseHandle(t);
    return 1;
}

// ---- dynamic libraries

void *plat_lib_open(const char *name) {
    wchar_t w[WPATH];
    if (!widen(name, w, WPATH)) return NULL;
    int bare = !wcschr(w, L'\\') && !wcschr(w, L'/');
    return (void *)LoadLibraryExW(w, NULL, bare ? LOAD_LIBRARY_SEARCH_SYSTEM32 : 0);
}

void *plat_lib_sym(void *lib, const char *name) { return lib ? (void *)GetProcAddress((HMODULE)lib, name) : NULL; }

// ---- machine load: GetSystemTimes for the CPU, PDH (pdh.dll, loaded on first use) for the thermal zones and the GPU
// engine and adapter memory counters that Task Manager reads.

typedef PDH_STATUS(WINAPI *PdhOpenQueryW_t)(LPCWSTR, DWORD_PTR, PDH_HQUERY *);
typedef PDH_STATUS(WINAPI *PdhAddEnglishCounterW_t)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER *);
typedef PDH_STATUS(WINAPI *PdhCollectQueryData_t)(PDH_HQUERY);
typedef PDH_STATUS(WINAPI *PdhGetFormattedCounterArrayW_t)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD,
                                                           PPDH_FMT_COUNTERVALUE_ITEM_W);
typedef PDH_STATUS(WINAPI *PdhCloseQuery_t)(PDH_HQUERY);
static struct {
    INIT_ONCE once;
    PdhOpenQueryW_t open;
    PdhAddEnglishCounterW_t add;
    PdhCollectQueryData_t collect;
    PdhGetFormattedCounterArrayW_t get;
    PdhCloseQuery_t close;
} pdh = {INIT_ONCE_STATIC_INIT};

static BOOL CALLBACK pdh_load(INIT_ONCE *o, void *p, void **c) {
    (void)o, (void)p, (void)c;
    HMODULE l = plat_lib_open("pdh.dll");
    if (!l) return TRUE;
    pdh.add = (PdhAddEnglishCounterW_t)(void *)GetProcAddress(l, "PdhAddEnglishCounterW");
    pdh.collect = (PdhCollectQueryData_t)(void *)GetProcAddress(l, "PdhCollectQueryData");
    pdh.get = (PdhGetFormattedCounterArrayW_t)(void *)GetProcAddress(l, "PdhGetFormattedCounterArrayW");
    pdh.close = (PdhCloseQuery_t)(void *)GetProcAddress(l, "PdhCloseQuery");
    PdhOpenQueryW_t open = (PdhOpenQueryW_t)(void *)GetProcAddress(l, "PdhOpenQueryW");
    if (pdh.add && pdh.collect && pdh.get && pdh.close) pdh.open = open;   // open set: all of them are there
    return TRUE;
}

// One PDH query with its wildcard counters ("\Object(*)\Counter"). A query whose open failed stays NULL.
typedef struct { PDH_HQUERY q; PDH_HCOUNTER c[2]; } PdhQuery;

static void pdh_query(PdhQuery *pq, const wchar_t *c0, const wchar_t *c1) {
    InitOnceExecuteOnce(&pdh.once, pdh_load, NULL, NULL);
    pq->q = NULL;
    if (!pdh.open || pdh.open(NULL, 0, &pq->q) != ERROR_SUCCESS) {
        pq->q = NULL;
        return;
    }
    if (pdh.add(pq->q, c0, 0, &pq->c[0]) != ERROR_SUCCESS) pq->c[0] = NULL;
    if (!c1 || pdh.add(pq->q, c1, 0, &pq->c[1]) != ERROR_SUCCESS) pq->c[1] = NULL;
    if (!pq->c[0] && !pq->c[1]) {
        pdh.close(pq->q);
        pq->q = NULL;
    }
}

// The counter's instances as a malloc'd array (free it), *count of them; NULL when there is no valid data yet (a rate
// counter needs two collections).
static PDH_FMT_COUNTERVALUE_ITEM_W *pdh_items(PDH_HCOUNTER c, DWORD *count) {
    DWORD size = 0;
    *count = 0;
    if (!c || pdh.get(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, count, NULL) != PDH_MORE_DATA || !size) return NULL;
    PDH_FMT_COUNTERVALUE_ITEM_W *items = malloc(size);
    if (items && pdh.get(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, count, items) == ERROR_SUCCESS) return items;
    free(items);
    *count = 0;
    return NULL;
}

struct PlatLoad {
    uint64_t idle, kernel, user, proc;   // the previous call's times; kernel 0 before the first
    PdhQuery thermal, gpu;               // gpu: engine utilisation, adapter dedicated memory
    wchar_t luid[40];                    // "luid_0x<high>_0x<low>" of the adapter, lower case; empty for any
};

PlatLoad *plat_load_open(const uint8_t *gpu_luid, int pci_bus) {
    (void)pci_bus;   // the LUID names the adapter
    PlatLoad *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    if (gpu_luid) {   // Vulkan's deviceLUID is the LUID struct's memory: LowPart, then HighPart
        uint32_t lo, hi;
        memcpy(&lo, gpu_luid, 4);
        memcpy(&hi, gpu_luid + 4, 4);
        swprintf(l->luid, sizeof l->luid / sizeof *l->luid, L"luid_0x%08x_0x%08x", hi, lo);
    }
    pdh_query(&l->thermal, L"\\Thermal Zone Information(*)\\Temperature", NULL);
    pdh_query(&l->gpu, L"\\GPU Engine(*)\\Utilization Percentage", L"\\GPU Adapter Memory(*)\\Dedicated Usage");
    return l;
}

void plat_load_close(PlatLoad *l) {
    if (!l) return;
    if (l->thermal.q) pdh.close(l->thermal.q);
    if (l->gpu.q) pdh.close(l->gpu.q);
    free(l);
}

void plat_cpu_load(PlatLoad *l, float *total_pct, float *proc_pct, float *temp_c) {
    *total_pct = *proc_pct = *temp_c = -1;
    if (!l) return;
    FILETIME idle, kern, user, cr, ex, pk, pu;
    if (GetSystemTimes(&idle, &kern, &user) && GetProcessTimes(GetCurrentProcess(), &cr, &ex, &pk, &pu)) {
        uint64_t i = ft64(idle), k = ft64(kern), u = ft64(user), p = ft64(pk) + ft64(pu);
        uint64_t total = (k - l->kernel) + (u - l->user);   // kernel time includes idle time
        if (l->kernel && total) {
            *total_pct = 100.0f * (float)(total - (i - l->idle)) / (float)total;
            *proc_pct = 100.0f * (float)(p - l->proc) / (float)total;
        }
        l->idle = i, l->kernel = k, l->user = u, l->proc = p;
    }
    // the firmware's ACPI zones, near the CPU on most boards; the exact package temperature needs a kernel driver
    DWORD n;
    PDH_FMT_COUNTERVALUE_ITEM_W *it;
    if (l->thermal.q && pdh.collect(l->thermal.q) == ERROR_SUCCESS && (it = pdh_items(l->thermal.c[0], &n))) {
        for (DWORD i = 0; i < n; i++) {   // the hottest zone, in kelvin
            double c = it[i].FmtValue.doubleValue - 273.15;
            if (it[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA && c > *temp_c && c < 150) *temp_c = (float)c;
        }
        free(it);
    }
}

// The adapter part of an instance name, "luid_0x..._0x...", lower case, into out (40 wide characters).
static void inst_luid(const wchar_t *name, wchar_t *out) {
    const wchar_t *p = wcsstr(name, L"luid_");
    size_t n = 0;
    while (p && p[n] && n < 26 && n < 39) out[n] = (wchar_t)towlower(p[n]), n++;   // luid_0x%08x_0x%08x: 26
    out[n] = 0;
}

void plat_gpu_load(PlatLoad *l, float *load_pct, float *vram_used_mb) {
    *load_pct = *vram_used_mb = -1;
    if (!l || !l->gpu.q || pdh.collect(l->gpu.q) != ERROR_SUCCESS) return;
    // Engine instances: "pid_<n>_luid_0x.._0x.._phys_<n>_eng_<n>_engtype_<type>". Per adapter and engine type the
    // processes' shares add up; the load is the busiest engine type.
    enum { TYPES = 32 };
    struct { wchar_t luid[40], type[32]; double sum; } t[TYPES];
    int nt = 0;
    DWORD n;
    PDH_FMT_COUNTERVALUE_ITEM_W *it = pdh_items(l->gpu.c[0], &n);
    for (DWORD i = 0; it && i < n; i++) {
        if (it[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA) continue;
        wchar_t lu[40];
        inst_luid(it[i].szName, lu);
        if (l->luid[0] && wcscmp(lu, l->luid)) continue;
        const wchar_t *ty = wcsstr(it[i].szName, L"engtype_");
        ty = ty ? ty + 8 : L"";
        int k = 0;
        while (k < nt && (wcscmp(t[k].luid, lu) || wcsncmp(t[k].type, ty, 31))) k++;
        if (k == nt) {
            if (nt == TYPES) continue;
            wcscpy(t[k].luid, lu);
            wcsncpy(t[k].type, ty, 31);
            t[k].type[31] = 0;
            t[k].sum = 0;
            nt++;
        }
        t[k].sum += it[i].FmtValue.doubleValue;
    }
    free(it);
    for (int k = 0; k < nt; k++)
        if (t[k].sum > *load_pct) *load_pct = (float)(t[k].sum > 100 ? 100 : t[k].sum);
    // Adapter memory instances: "luid_0x.._0x.._phys_<n>", bytes. Without a LUID: the adapter that uses the most.
    it = pdh_items(l->gpu.c[1], &n);
    double sum = 0, best = -1;
    for (DWORD i = 0; it && i < n; i++) {
        if (it[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA) continue;
        wchar_t lu[40];
        inst_luid(it[i].szName, lu);
        if (l->luid[0] ? !wcscmp(lu, l->luid) : 0) sum += it[i].FmtValue.doubleValue;
        if (it[i].FmtValue.doubleValue > best) best = it[i].FmtValue.doubleValue;
    }
    if (it && (l->luid[0] ? sum > 0 : best >= 0)) *vram_used_mb = (float)((l->luid[0] ? sum : best) / 1048576.0);
    free(it);
}

void plat_mem_info(float *used_mb, float *total_mb, float *proc_mb) {
    *used_mb = *total_mb = *proc_mb = -1;
    MEMORYSTATUSEX ms = {.dwLength = sizeof ms};
    if (GlobalMemoryStatusEx(&ms)) {
        *total_mb = (float)(ms.ullTotalPhys >> 20);
        *used_mb = (float)((ms.ullTotalPhys - ms.ullAvailPhys) >> 20);
    }
    PROCESS_MEMORY_COUNTERS pmc;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) *proc_mb = (float)(pmc.WorkingSetSize >> 20);
}

void plat_commit_info(double *proc_mb, double *used_mb, double *limit_mb) {
    *proc_mb = *used_mb = *limit_mb = -1;
    MEMORYSTATUSEX ms = {.dwLength = sizeof ms};
    if (GlobalMemoryStatusEx(&ms)) {
        *limit_mb = (double)(ms.ullTotalPageFile >> 20);
        *used_mb = (double)((ms.ullTotalPageFile - ms.ullAvailPageFile) >> 20);
    }
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc))
        *proc_mb = (double)(pmc.PrivateUsage >> 20);
}

int plat_commit_enforced(void) { return 1; }

// The numbers name the one limit there is, the system's commit.
int plat_commit_note(char *out, size_t cap) {
    if (cap) out[0] = 0;
    return 0;
}

// The driver charges the device memory it allocates to the system's commit.
int plat_commit_counts_vram(void) { return 1; }

// The PagingFiles value under Memory Management, one "<path> <initial MB> <maximum MB>" per page file: a path of
// "?:\..." (System managed size on every drive) or no sizes or "0 0" (System managed size on that drive) leaves the
// size to Windows. Read once; the setting takes effect only after a restart.
int plat_page_file_managed(void) {
    static int managed = -2;
    if (managed != -2) return managed;
    wchar_t v[2048];
    DWORD size = sizeof v - sizeof(wchar_t) * 2;
    int r = -1;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
                     L"PagingFiles", RRF_RT_REG_MULTI_SZ, NULL, v, &size) == ERROR_SUCCESS) {
        v[size / sizeof(wchar_t)] = v[size / sizeof(wchar_t) + 1] = 0;
        for (wchar_t *e = v; *e; e += wcslen(e) + 1) {
            wchar_t *sp = wcschr(e, L' ');
            unsigned long lo = sp ? wcstoul(sp, &sp, 10) : 0, hi = sp ? wcstoul(sp, NULL, 10) : 0;
            int own = e[0] == L'?' || (!lo && !hi);
            r = own && r != 0 ? 1 : 0;   // a page file with its sizes set is the user's choice: no advice then
        }
    }
    return managed = r;
}

void plat_process_memory(uint64_t *resident, uint64_t *private_bytes) {
    PROCESS_MEMORY_COUNTERS_EX pm = {.cb = sizeof pm};
    *resident = *private_bytes = 0;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm))
        *resident = pm.WorkingSetSize, *private_bytes = pm.PrivateUsage;
}

// The UCRT's malloc takes its memory from the heap _get_heap_handle names (the process heap); HeapSummary sums it. The
// other heaps are only counted: one made with HEAP_NO_SERIALIZE has no lock to take.
void plat_heap_info(PlatHeapInfo *h) {
    memset(h, 0, sizeof *h);
    HANDLE crt = (HANDLE)_get_heap_handle();
    HEAP_SUMMARY s = {.cb = sizeof s};
    if (crt && HeapSummary(crt, 0, &s)) h->committed = s.cbCommitted, h->used = s.cbAllocated;
    HANDLE hs[256];
    DWORD n = GetProcessHeaps(256, hs);
    for (DWORD i = 0; i < n && i < 256; i++) h->other_heaps += hs[i] != crt;
}

// ---- child processes

struct PlatChild {
    HANDLE proc, in, out;                                  // in: our write end, out: our read end
};

PlatChild *plat_child_start(const char *exe, const char *arg, uint32_t *err) {
    wchar_t we[WPATH], wa[256] = L"", cmd[WPATH + 300];
    if (err) *err = 0;
    if (!widen(exe, we, WPATH) || (arg && !widen(arg, wa, 256))) return NULL;
    swprintf(cmd, WPATH + 300, arg ? L"\"%ls\" %ls" : L"\"%ls\"", we, wa);
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE in_r, in_w, out_r, out_w;
    // Pipes are created and handed out one child at a time: a child started meanwhile would inherit the other pipe
    // ends and keep them open after their owner closed them.
    static SRWLOCK spawn = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&spawn);
    if (!CreatePipe(&in_r, &in_w, &sa, 0)) return ReleaseSRWLockExclusive(&spawn), NULL;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
        CloseHandle(in_r), CloseHandle(in_w);
        return ReleaseSRWLockExclusive(&spawn), NULL;
    }
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    STARTUPINFOW si = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES, .hStdInput = in_r, .hStdOutput = out_w,
                       .hStdError = nul != INVALID_HANDLE_VALUE ? nul : out_w};
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    DWORD e = GetLastError();
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    ReleaseSRWLockExclusive(&spawn);
    if (!ok) {
        CloseHandle(in_w), CloseHandle(out_r);
        if (err) *err = e;
        return NULL;
    }
    CloseHandle(pi.hThread);
    // Every child goes into one job that ends its processes when its last handle closes, which is when this process
    // ends: a translator stuck on a program would otherwise outlive a game that exited or crashed before stopping it.
    // The handle is never closed by hand. A failure leaves the child as it was before.
    static HANDLE job;
    static INIT_ONCE job_once = INIT_ONCE_STATIC_INIT;
    BOOL pending;
    if (InitOnceBeginInitialize(&job_once, 0, &pending, NULL) && pending) {
        HANDLE j = CreateJobObjectW(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (j && !SetInformationJobObject(j, JobObjectExtendedLimitInformation, &li, sizeof li)) CloseHandle(j), j = NULL;
        job = j;
        InitOnceComplete(&job_once, 0, NULL);
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    PlatChild *c = malloc(sizeof *c);
    if (!c) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess), CloseHandle(in_w), CloseHandle(out_r);
        return NULL;
    }
    c->proc = pi.hProcess, c->in = in_w, c->out = out_r;
    return c;
}

void plat_child_stop(PlatChild *c) {
    if (!c) return;
    CloseHandle(c->in);
    CloseHandle(c->out);
    TerminateProcess(c->proc, 1);
    CloseHandle(c->proc);
    free(c);
}

void plat_child_lower(PlatChild *c) {
    if (c) SetPriorityClass(c->proc, IDLE_PRIORITY_CLASS);
}

int plat_child_write(PlatChild *c, const void *data, size_t n) {
    DWORD put = 0;
    return WriteFile(c->in, data, (DWORD)n, &put, NULL) && put == n;
}

// A child that stays alive but never answers (a translator stuck on one program) used to block the reader for good,
// and with it the draw or dispatch waiting for that shader: after PLAT_CHILD_READ_MS without a byte the read fails,
// and the caller stops the child (gfx_shader.c svc_request). The pipe is anonymous, so readiness is polled.
long plat_child_read(PlatChild *c, void *buf, size_t cap) {
    ULONGLONG t0 = GetTickCount64();
    for (unsigned spins = 0;; spins++) {
        DWORD avail = 0;
        if (!PeekNamedPipe(c->out, NULL, 0, NULL, &avail, NULL)) return -1;   // broken: the child exited
        if (avail) break;
        if (GetTickCount64() - t0 > PLAT_CHILD_READ_MS) return -1;
        Sleep(spins < 50 ? 0 : 1);
    }
    DWORD got = 0;
    if (!ReadFile(c->out, buf, (DWORD)cap, &got, NULL)) return -1;
    return (long)got;
}

// ---- local IPC (platform.h): Discord's Rich Presence named pipe
struct PlatIpc { HANDLE h; };

PlatIpc *plat_ipc_connect(const char *name) {
    wchar_t wname[128], path[152];
    if (!widen(name, wname, 128)) return NULL;
    swprintf(path, 152, L"\\\\.\\pipe\\%ls", wname);
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;   // no listener, or the pipe is busy: the caller retries later
    PlatIpc *p = malloc(sizeof *p);
    if (!p) {
        CloseHandle(h);
        return NULL;
    }
    p->h = h;
    return p;
}

int plat_ipc_read(PlatIpc *p, void *buf, size_t n, uint32_t ms) {
    uint64_t deadline = plat_now_ms() + ms;
    for (;;) {
        DWORD have = 0;
        if (!PeekNamedPipe(p->h, NULL, 0, NULL, &have, NULL)) return -1;   // the writer closed the pipe
        if (have) {
            DWORD got = 0;
            if (!ReadFile(p->h, buf, have < n ? have : (DWORD)n, &got, NULL) || !got) return -1;
            return (int)got;
        }
        if (plat_now_ms() >= deadline) return 0;
        plat_sleep_ms(5);
    }
}

int plat_ipc_write(PlatIpc *p, const void *buf, size_t n) {
    const uint8_t *at = buf;
    while (n) {
        DWORD put = 0;
        if (!WriteFile(p->h, at, n > (1u << 30) ? 1u << 30 : (DWORD)n, &put, NULL) || !put) return 0;
        at += put, n -= put;
    }
    return 1;
}

void plat_ipc_close(PlatIpc *p) {
    if (!p) return;
    CloseHandle(p->h);
    free(p);
}

// ---- UDP sockets (platform.h). Winsock starts at the first call that needs it.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#ifndef WSA_FLAG_NO_HANDLE_INHERIT
#define WSA_FLAG_NO_HANDLE_INHERIT 0x80
#endif

struct PlatUdp { SOCKET s; int v6; };

static PlatOnce wsa_once = PLAT_ONCE_INIT;
static int wsa_ok;

static void wsa_start(void *arg) {
    WSADATA d;
    (void)arg;
    wsa_ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
}

static int net_ready(void) {
    plat_once(&wsa_once, wsa_start, NULL);
    return wsa_ok;
}

static void addr_from_sockaddr(PlatAddr *out, const SOCKADDR_STORAGE *ss) {
    uint8_t ip[16] = {0};
    if (ss->ss_family == AF_INET6) {
        const SOCKADDR_IN6 *a = (const SOCKADDR_IN6 *)ss;
        memcpy(ip, &a->sin6_addr, 16);
        plat_addr_set(out, ip, ntohs(a->sin6_port), a->sin6_scope_id);
    } else {
        const SOCKADDR_IN *a = (const SOCKADDR_IN *)ss;
        ip[10] = ip[11] = 0xff;
        memcpy(ip + 12, &a->sin_addr, 4);
        plat_addr_set(out, ip, ntohs(a->sin_port), 0);
    }
}

// The socket address of a for a socket of family v6 (1: AF_INET6, 0: AF_INET); 0 when an IPv4 socket cannot reach it.
static int addr_to_sockaddr(const PlatAddr *a, int v6, SOCKADDR_STORAGE *ss, int *len) {
    uint8_t ip[16];
    uint32_t scope;
    plat_addr_get(a, ip, &scope);
    memset(ss, 0, sizeof *ss);
    if (v6) {
        SOCKADDR_IN6 *s = (SOCKADDR_IN6 *)ss;
        s->sin6_family = AF_INET6;
        s->sin6_port = htons(plat_addr_port(a));
        s->sin6_scope_id = scope;
        memcpy(&s->sin6_addr, ip, 16);
        *len = (int)sizeof *s;
        return 1;
    }
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (memcmp(ip, mapped, sizeof mapped)) return 0;
    SOCKADDR_IN *s = (SOCKADDR_IN *)ss;
    s->sin_family = AF_INET;
    s->sin_port = htons(plat_addr_port(a));
    memcpy(&s->sin_addr, ip + 12, 4);
    *len = (int)sizeof *s;
    return 1;
}

int plat_net_resolve(const char *host, uint16_t port, PlatAddr *out) {
    wchar_t whost[256];
    if (!host || !*host || !net_ready() || !widen(host, whost, 256)) return 0;
    ADDRINFOW hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM}, *res = NULL, *pick = NULL;
    if (GetAddrInfoW(whost, NULL, &hints, &res) != 0) return 0;
    for (ADDRINFOW *r = res; r; r = r->ai_next) {
        if (r->ai_addrlen > sizeof(SOCKADDR_STORAGE) || (r->ai_family != AF_INET && r->ai_family != AF_INET6)) continue;
        if (!pick || (r->ai_family == AF_INET && pick->ai_family != AF_INET)) pick = r;   // IPv4 first
    }
    int ok = pick != NULL;
    if (ok) {
        SOCKADDR_STORAGE ss;
        memset(&ss, 0, sizeof ss);
        memcpy(&ss, pick->ai_addr, pick->ai_addrlen);
        addr_from_sockaddr(out, &ss);
        uint8_t ip[16];
        uint32_t scope;
        plat_addr_get(out, ip, &scope);
        plat_addr_set(out, ip, port, scope);
    }
    FreeAddrInfoW(res);
    return ok;
}

// A non-blocking UDP socket of family af that no child process inherits, with Windows' report of an ICMP port-unreachable
// as a failed receive (WSAECONNRESET on an unconnected socket) switched off: a peer that is gone is not an error here.
static SOCKET udp_socket(int af) {
    SOCKET s = WSASocketW(af, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) return s;
    u_long nonblocking = 1;
    BOOL report = FALSE;
    DWORD got = 0;
    if (ioctlsocket(s, FIONBIO, &nonblocking) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof report, NULL, 0, &got, NULL, NULL);
    return s;
}

PlatUdp *plat_udp_open(uint16_t port, int dual_stack) {
    if (!net_ready()) return NULL;
    int v6 = dual_stack;
    SOCKET s = v6 ? udp_socket(AF_INET6) : INVALID_SOCKET;
    if (s != INVALID_SOCKET) {
        DWORD only_v6 = 0;   // Windows defaults to IPv6 only
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&only_v6, sizeof only_v6);
        SOCKADDR_IN6 a;
        memset(&a, 0, sizeof a);   // in6addr_any
        a.sin6_family = AF_INET6;
        a.sin6_port = htons(port);
        if (bind(s, (SOCKADDR *)&a, sizeof a) != 0) closesocket(s), s = INVALID_SOCKET;
    }
    if (s == INVALID_SOCKET) {   // IPv4 only, asked for or all the machine has
        v6 = 0;
        s = udp_socket(AF_INET);
        if (s == INVALID_SOCKET) return NULL;
        SOCKADDR_IN a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, (SOCKADDR *)&a, sizeof a) != 0) {
            closesocket(s);
            return NULL;
        }
    }
    PlatUdp *u = malloc(sizeof *u);
    if (!u) {
        closesocket(s);
        return NULL;
    }
    u->s = s, u->v6 = v6;
    return u;
}

uint16_t plat_udp_port(const PlatUdp *u) {
    SOCKADDR_STORAGE ss;
    int len = (int)sizeof ss;
    if (!u || getsockname(u->s, (SOCKADDR *)&ss, &len) != 0) return 0;
    PlatAddr a;
    addr_from_sockaddr(&a, &ss);
    return plat_addr_port(&a);
}

int plat_udp_send(PlatUdp *u, const PlatAddr *to, const void *p, size_t n) {
    SOCKADDR_STORAGE ss;
    int len;
    if (!u || n > 65507 || !addr_to_sockaddr(to, u->v6, &ss, &len)) return 0;
    if (sendto(u->s, (const char *)p, (int)n, 0, (SOCKADDR *)&ss, len) != SOCKET_ERROR) return 1;
    // a full buffer, a missing route or an ICMP error is a lost datagram; a datagram too long or a socket that is not
    // ours to use can never be sent
    int e = WSAGetLastError();
    return e != WSAEMSGSIZE && e != WSAENOTSOCK && e != WSAEINVAL && e != WSAEAFNOSUPPORT;
}

int plat_udp_recv(PlatUdp *u, void *buf, size_t cap, PlatAddr *from, uint32_t ms) {
    if (!u) return -1;
    WSAPOLLFD pf = {.fd = u->s, .events = POLLRDNORM};
    int r = WSAPoll(&pf, 1, ms > (uint32_t)INT_MAX ? INT_MAX : (int)ms);
    if (r == SOCKET_ERROR) return WSAGetLastError() == WSAEINTR ? 0 : -1;
    if (r == 0) return 0;
    SOCKADDR_STORAGE ss;
    int len = (int)sizeof ss;
    int k = recvfrom(u->s, (char *)buf, cap > (size_t)INT_MAX ? INT_MAX : (int)cap, 0, (SOCKADDR *)&ss, &len);
    if (k == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e == WSAEMSGSIZE) {   // longer than buf: Windows reports it as an error but has filled buf
            k = cap > (size_t)INT_MAX ? INT_MAX : (int)cap;
        } else {
            return e == WSAEWOULDBLOCK || e == WSAEINTR || e == WSAECONNRESET || e == WSAENETRESET || e == WSAEHOSTUNREACH ||
                           e == WSAENETUNREACH
                       ? 0 : -1;
        }
    }
    if (from) addr_from_sockaddr(from, &ss);
    return k;
}

void plat_udp_close(PlatUdp *u) {
    if (!u) return;
    closesocket(u->s);
    free(u);
}


// ---- restart
// NX2NAVI_RESTART_WAIT names the process that called plat_restart, as its id and creation time: a process that has only
// reused the id is not waited for. Waiting keeps the new process off the old one's files (the log it moves at start,
// the saves) until the old one is gone; no handle is inherited, so none of them stays open in the new one either.
#define RESTART_WAIT "NX2NAVI_RESTART_WAIT"

static uint64_t process_created(HANDLE h) {
    FILETIME c, e, k, u;
    return GetProcessTimes(h, &c, &e, &k, &u) ? ft64(c) : 0;
}

int plat_restart(void) {
    char wait[48];
    snprintf(wait, sizeof wait, "%lu.%llu", GetCurrentProcessId(), (unsigned long long)process_created(GetCurrentProcess()));
    plat_setenv(RESTART_WAIT, wait);
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    wchar_t *cmd = _wcsdup(GetCommandLineW());   // CreateProcessW may write its command line
    BOOL ok = cmd && CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    free(cmd);
    plat_setenv(RESTART_WAIT, NULL);
    if (!ok) return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

// One argument of a command line as CommandLineToArgvW reads it back: in quotes, a backslash run doubled before a quote
// and at the end, a quote escaped.
static size_t quote_arg(const wchar_t *a, wchar_t *out, size_t cap, size_t at) {
#define PUT(c) do { if (at + 1 < cap) out[at] = (c); at++; } while (0)
    PUT(L'"');
    for (const wchar_t *p = a;; p++) {
        size_t slashes = 0;
        while (*p == L'\\') p++, slashes++;
        size_t times = !*p || *p == L'"' ? 2 : 1;
        for (size_t k = 0; k < slashes * times; k++) PUT(L'\\');
        if (!*p) break;
        if (*p == L'"') PUT(L'\\');
        PUT(*p);
    }
    PUT(L'"');
    PUT(L' ');
#undef PUT
    return at;
}

int plat_spawn(const char *const *argv, const char *cwd) {
    enum { CMD = 32768 };
    wchar_t *cmd = calloc(CMD, sizeof *cmd), *arg = malloc(WPATH * sizeof *arg), *dir = malloc(WPATH * sizeof *dir);
    int ok = cmd && arg && dir && argv[0] && widen(cwd, dir, WPATH);
    size_t at = 0;
    for (int i = 0; ok && argv[i]; i++) {
        ok = widen(argv[i], arg, WPATH);
        if (ok) at = quote_arg(arg, cmd, CMD, at);
    }
    ok = ok && at < CMD && at > 0;
    if (ok) cmd[at - 1] = 0;   // the last separator
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    ok = ok && CreateProcessW(NULL, cmd, NULL, NULL, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, NULL, dir, &si,
                              &pi);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    free(cmd);
    free(arg);
    free(dir);
    return ok;
}

int plat_restart_wait(uint32_t timeout_ms) {
    char v[48];
    if (!plat_getenv(RESTART_WAIT, v, sizeof v)) return 0;
    plat_setenv(RESTART_WAIT, NULL);   // the processes this one starts do not wait again
    unsigned long pid = 0;
    unsigned long long created = 0;
    if (sscanf(v, "%lu.%llu", &pid, &created) != 2) return 0;
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;   // already gone
    int r = process_created(h) != created ? 0 : WaitForSingleObject(h, timeout_ms) == WAIT_OBJECT_0 ? 1 : -1;
    CloseHandle(h);
    return r;
}
