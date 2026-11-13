// Linux implementation of platform.h: pthreads, CLOCK_MONOTONIC, futex locks and condition variables, POSIX files,
// /proc and sysfs for the load counters, posix_spawn for child processes.
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <malloc.h>
#include <linux/fs.h>
#include <linux/futex.h>
#include <linux/userfaultfd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <unwind.h>

#include <SDL3/SDL_clipboard.h>

#include "platform.h"

// The write-watch ABI of Linux 6.7 (uapi linux/fs.h and linux/userfaultfd.h), for headers older than it: the release
// builds against the Steam Runtime's 5.10 headers and runs on newer kernels. The kernel refuses what it lacks at run time.
#ifndef PAGEMAP_SCAN
struct page_region {
    __u64 start;
    __u64 end;
    __u64 categories;
};
struct pm_scan_arg {
    __u64 size;
    __u64 flags;
    __u64 start;
    __u64 end;
    __u64 walk_end;
    __u64 vec;
    __u64 vec_len;
    __u64 max_pages;
    __u64 category_inverted;
    __u64 category_mask;
    __u64 category_anyof_mask;
    __u64 return_mask;
};
#define PAGEMAP_SCAN _IOWR('f', 16, struct pm_scan_arg)
#define PAGE_IS_WRITTEN (1 << 1)
#define PAGE_IS_PRESENT (1 << 3)
#define PAGE_IS_SWAPPED (1 << 4)
#define PAGE_IS_PFNZERO (1 << 5)
#define PM_SCAN_WP_MATCHING (1 << 0)
#define PM_SCAN_CHECK_WPASYNC (1 << 1)
#endif
#ifndef UFFD_FEATURE_WP_ASYNC
#define UFFD_FEATURE_WP_UNPOPULATED (1 << 13)
#define UFFD_FEATURE_WP_ASYNC (1 << 15)
#endif
#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif

extern char **environ;

// ---- threads

struct PlatThread {
    pthread_t t;
    PlatThreadFn fn;
    void *arg;
    volatile int32_t tid;   // the kernel's id once the thread runs, for plat_thread_set_priority
    char name[16];          // the kernel keeps 15 characters
};

// A signal stack for the calling thread, so the fault and crash handlers still run when its own stack is exhausted.
// Never freed: threads that end are few, and a stack freed while a handler might run on it would be worse.
static void alt_stack(void) {
    stack_t cur;
    if (!sigaltstack(NULL, &cur) && !(cur.ss_flags & SS_DISABLE)) return;
    size_t size = 64 << 10;   // a signal frame with the AVX-512 state takes several KiB
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return;
    stack_t ss = {.ss_sp = p, .ss_size = size};
    sigaltstack(&ss, NULL);
}

// Set in the thread that took a core in plat_cpu_reserve: the processors the threads it starts run on (not that core).
// Threads inherit the mask of the thread that starts them, which for those would be the reserved core alone.
static __thread cpu_set_t cpu_rest;
static __thread int cpu_rest_set;

static void *thread_main(void *p) {
    PlatThread *t = p;
    alt_stack();
    __atomic_store_n(&t->tid, (int32_t)syscall(SYS_gettid), __ATOMIC_RELEASE);
    if (t->name[0]) pthread_setname_np(pthread_self(), t->name);
    t->fn(t->arg);
    plat_thread_end_run();   // what the other files keep per thread (platform.h plat_thread_end_hook)
    return NULL;
}

PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name) {
    return plat_thread_new_ex(fn, arg, name, 0);
}

PlatThread *plat_thread_new_ex(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    PlatThread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    if (name) snprintf(t->name, sizeof t->name, "%s", name);
    pthread_attr_t a;
    pthread_attr_init(&a);
    // The stack is mapped lazily, as Windows reserves it: only the pages a thread touches cost memory.
    pthread_attr_setstacksize(&a, stack ? stack : PLAT_STACK_DEFAULT);
    if (cpu_rest_set) pthread_attr_setaffinity_np(&a, sizeof cpu_rest, &cpu_rest);
    int e = pthread_create(&t->t, &a, thread_main, t);
    pthread_attr_destroy(&a);
    if (e) { free(t); return NULL; }
    return t;
}

static int read_u64_file(const char *path, uint64_t *v);

// The physical core of each logical processor is in /sys/devices/system/cpu/cpu<n>/topology/core_id together with its
// package; the lowest-numbered processor of each (package, core) pair is the core's first.
int plat_cpu_cores(uint64_t *all, uint64_t *first) {
    cpu_set_t set;
    *all = *first = 0;
    if (sched_getaffinity(0, sizeof set, &set)) return 0;
    int cores = 0;
    uint64_t seen[64][2];
    for (int i = 0; i < 64; i++) {
        if (!CPU_ISSET(i, &set)) continue;
        *all |= 1ull << i;
        uint64_t core = 0, pkg = 0;
        char path[96];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/core_id", i);
        if (!read_u64_file(path, &core)) core = (uint64_t)i;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", i);
        read_u64_file(path, &pkg);
        int k = 0;
        while (k < cores && (seen[k][0] != core || seen[k][1] != pkg)) k++;
        if (k == cores) seen[cores][0] = core, seen[cores][1] = pkg, cores++, *first |= 1ull << i;
    }
    return cores;
}

// The L3 cache of each logical processor: the cache/index<k> of /sys/devices/system/cpu/cpu<n> whose level is 3, its
// size in KiB ("98304K").
uint64_t plat_cpu_cache_die(uint32_t *l3_kb) {
    cpu_set_t set;
    *l3_kb = 0;
    if (sched_getaffinity(0, sizeof set, &set)) return 0;
    uint64_t die = 0, in_set = 0;
    uint32_t big = 0, small = ~0u;
    for (int i = 0; i < 64; i++) {
        uint32_t kb = 0;
        for (int k = 0; k < 8 && !kb; k++) {
            char path[96], text[32] = "";
            uint64_t level = 0;
            snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cache/index%d/level", i, k);
            if (!read_u64_file(path, &level)) break;
            if (level != 3) continue;
            snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cache/index%d/size", i, k);
            FILE *f = fopen(path, "r");
            if (f) {
                if (fgets(text, sizeof text, f)) kb = (uint32_t)strtoul(text, NULL, 10) * (strchr(text, 'M') ? 1024 : 1);
                fclose(f);
            }
        }
        if (!kb) continue;
        if (kb > big) big = kb, die = 0;
        if (kb == big) die |= 1ull << i;
        if (kb < small) small = kb;
        if (CPU_ISSET(i, &set)) in_set |= 1ull << i;
    }
    if (!die || big == small) return 0;
    *l3_kb = big;
    return die & in_set;
}

// The core plat_cpu_reserve took: its processors and the thread that holds them (0 before the call), and the
// processors every other thread got then or at the last plat_process_affinity. cpu_lock orders the functions that set
// the masks of every thread.
static PlatMutex cpu_lock = PLAT_MUTEX_INIT;
static cpu_set_t cpu_taken, cpu_others;
static pid_t cpu_taker;

static void cpu_clear(cpu_set_t *set, const cpu_set_t *drop) {
    for (int i = 0; i < CPU_SETSIZE; i++)
        if (CPU_ISSET(i, drop)) CPU_CLR(i, set);
}

// Linux has no process-wide affinity: each thread of /proc/self/task gets the mask, and threads started later inherit
// it from the thread that starts them. The core plat_cpu_reserve took stays out of the mask of every thread but its
// holder, which keeps the mask it has.
int plat_process_affinity(uint64_t mask) {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < 64; i++)
        if (mask >> i & 1) CPU_SET(i, &set);
    plat_mutex_lock(&cpu_lock);
    if (cpu_taker) cpu_clear(&set, &cpu_taken);
    DIR *d = CPU_COUNT(&set) ? opendir("/proc/self/task") : NULL;
    int ok = d != NULL;
    if (ok && cpu_taker) cpu_others = set;
    for (struct dirent *e; d && (e = readdir(d));) {
        pid_t tid = (pid_t)atoi(e->d_name);
        if (tid > 0 && tid != cpu_taker && sched_setaffinity(tid, sizeof set, &set)) ok = 0;
    }
    if (d) closedir(d);
    plat_mutex_unlock(&cpu_lock);
    return ok;
}

// The first field of /proc/self/task/<tid>/schedstat is the time the thread has run, in ns.
uint64_t plat_thread_cpu_ns(uint32_t tid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/self/task/%u/schedstat", tid);
    FILE *f = fopen(path, "r");
    unsigned long long ns = 0;
    if (f) {
        if (fscanf(f, "%llu", &ns) != 1) ns = 0;
        fclose(f);
    }
    return ns;
}

// ---- thread interruption: a signal handshake in place of Windows' SuspendThread / Get/SetThreadContext.
// The caller moves the handle from IDLE to REQ and signals the thread. The thread's handler stores where it was and
// moves REQ to STOPPED, then spins until the caller moves it on to GO (with the redirect in to), changes its own
// return context, and goes back to IDLE. A caller that sees no STOPPED within INTR_WAIT_NS (the thread is blocked in
// the kernel and runs no handler) takes REQ back to IDLE; a handler that then finds IDLE returns at once.
enum { INTR_IDLE, INTR_REQ, INTR_STOPPED, INTR_GO };
#define INTR_WAIT_NS 200000u

struct PlatIntr {
    pthread_t pt;
    volatile int32_t state;
    uint64_t rip, rsp, to;
};

static __thread PlatIntr *intr_me;
static int intr_sig;

static void intr_handler(int sig, siginfo_t *si, void *uc) {
    (void)sig, (void)si;
    PlatIntr *t = intr_me;
    ucontext_t *u = uc;
    if (!t) return;
    t->rip = (uint64_t)u->uc_mcontext.gregs[REG_RIP];
    t->rsp = (uint64_t)u->uc_mcontext.gregs[REG_RSP];
    int32_t req = INTR_REQ;
    if (!__atomic_compare_exchange_n(&t->state, &req, INTR_STOPPED, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    while (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != INTR_GO) plat_pause();
    if (t->to) u->uc_mcontext.gregs[REG_RIP] = (greg_t)t->to;
    __atomic_store_n(&t->state, INTR_IDLE, __ATOMIC_RELEASE);
}

PlatIntr *plat_intr_self(void) {
    static PlatMutex install = PLAT_MUTEX_INIT;
    if (!__atomic_load_n(&intr_sig, __ATOMIC_ACQUIRE)) {
        plat_mutex_lock(&install);
        if (!intr_sig) {
            int sig = SIGRTMIN + 4;   // glibc keeps the first real-time signals for itself; SIGRTMIN skips them
            struct sigaction sa = {.sa_sigaction = intr_handler, .sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK};
            sigemptyset(&sa.sa_mask);
            if (!sigaction(sig, &sa, NULL)) __atomic_store_n(&intr_sig, sig, __ATOMIC_RELEASE);
        }
        plat_mutex_unlock(&install);
    }
    if (!intr_sig) return NULL;
    PlatIntr *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->pt = pthread_self();
    intr_me = t;
    return t;
}

// Called by the thread itself as it ends, or by another once the thread no longer runs.
void plat_intr_free(PlatIntr *t) {
    if (!t) return;
    if (intr_me == t) intr_me = NULL;
    free(t);
}

int plat_intr_stop(PlatIntr *t, uint64_t *rip, uint64_t *rsp) {
    int32_t idle = INTR_IDLE;
    if (!__atomic_compare_exchange_n(&t->state, &idle, INTR_REQ, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
    if (pthread_kill(t->pt, intr_sig)) {
        __atomic_store_n(&t->state, INTR_IDLE, __ATOMIC_RELEASE);
        return 0;
    }
    uint64_t end = plat_now_ns() + INTR_WAIT_NS;
    while (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != INTR_STOPPED) {
        if (plat_now_ns() < end) {
            plat_pause();
            continue;
        }
        int32_t req = INTR_REQ;
        if (__atomic_compare_exchange_n(&t->state, &req, INTR_IDLE, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
    }
    *rip = t->rip;
    *rsp = t->rsp;
    return 1;
}

int plat_intr_resume(PlatIntr *t, uint64_t to) {
    t->to = to;
    __atomic_store_n(&t->state, INTR_GO, __ATOMIC_RELEASE);
    return 1;
}

// Hardware data watchpoints are Windows' only (platform.h plat_watch_set): the diagnosis that uses them is off here.
int plat_watch_set(const uint64_t *addr, const int *len, int n, PlatWatchHit hit) {
    (void)addr, (void)len, (void)n, (void)hit;
    return -1;
}

// ---- fibers: a stack switch of our own. glibc's swapcontext saves and restores the signal mask with a system call
// on every switch, and the game switches fibers often. fiber_swap saves the callee-saved registers, the MXCSR and the
// x87 control word on the current stack, stores the stack pointer in *save and loads the other one. A new fiber's
// stack is laid out as fiber_swap leaves one, returning into fiber_entry, which calls fn(arg) (r12 and r13).
struct PlatFiber {
    void *sp;
    void *stack;   // mapped stack (NULL for a thread's own context)
    size_t size;
};

void plat_fiber_swap(void **save, void *load);
void plat_fiber_entry(void);
__asm__(".text\n"
        ".globl plat_fiber_swap\n"
        ".type plat_fiber_swap, @function\n"
        ".p2align 4\n"
        "plat_fiber_swap:\n"
        "    .cfi_startproc\n"
        "    pushq %rbp\n"
        "    pushq %rbx\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    subq $16, %rsp\n"
        "    stmxcsr 8(%rsp)\n"
        "    fnstcw (%rsp)\n"
        "    movq %rsp, (%rdi)\n"
        "    movq %rsi, %rsp\n"
        "    fldcw (%rsp)\n"
        "    ldmxcsr 8(%rsp)\n"
        "    addq $16, %rsp\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rbx\n"
        "    popq %rbp\n"
        "    ret\n"
        "    .cfi_endproc\n"
        ".size plat_fiber_swap, .-plat_fiber_swap\n"
        ".globl plat_fiber_entry\n"
        ".type plat_fiber_entry, @function\n"
        ".p2align 4\n"
        "plat_fiber_entry:\n"
        "    andq $-16, %rsp\n"   // fiber_swap's ret leaves rsp 8 off: the SysV ABI wants it aligned at the call
        "    movq %r13, %rdi\n"
        "    callq *%r12\n"
        "    ud2\n"
        ".size plat_fiber_entry, .-plat_fiber_entry\n");

static __thread PlatFiber *fiber_cur;
static __thread PlatFiber fiber_own;

PlatFiber *plat_fiber_self(void) {
    fiber_own = (PlatFiber){0};
    fiber_cur = &fiber_own;
    return &fiber_own;
}

void plat_fiber_unself(void) { fiber_cur = NULL; }

PlatFiber *plat_fiber_new(size_t stack, void (*fn)(void *arg), void *arg) {
    PlatFiber *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    // Reserved, not committed; the lowest page stays inaccessible, so an overflow faults instead of corrupting.
    void *m = mmap(NULL, stack, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if (m == MAP_FAILED) {
        free(f);
        return NULL;
    }
    mprotect(m, 4096, PROT_NONE);
    f->stack = m, f->size = stack;
    uint64_t *top = (uint64_t *)(((uintptr_t)m + stack) & ~(uintptr_t)15);
    uint64_t *sp = top - 10;   // fpu state (2 words), r15, r14, r13, r12, rbx, rbp, return address, padding
    sp[0] = 0x037f;            // x87 control word: the default
    sp[1] = 0x1f80;            // MXCSR: the default
    sp[2] = sp[3] = 0;         // r15, r14
    sp[4] = (uint64_t)(uintptr_t)arg;   // r13
    sp[5] = (uint64_t)(uintptr_t)fn;    // r12
    sp[6] = sp[7] = 0;         // rbx, rbp
    sp[8] = (uint64_t)(uintptr_t)plat_fiber_entry;
    sp[9] = 0;
    f->sp = sp;
    return f;
}

void plat_fiber_switch(PlatFiber *to) {
    PlatFiber *from = fiber_cur;
    fiber_cur = to;   // on this thread; the switch back into from sets it on whichever thread that is
    plat_fiber_swap(&from->sp, to->sp);
}

void plat_fiber_free(PlatFiber *f) {
    if (!f) return;
    if (f->stack) munmap(f->stack, f->size);
    free(f);
}

int plat_thread_spawn(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    PlatThread *t = plat_thread_new_ex(fn, arg, name, stack);
    if (t) plat_thread_detach(t);
    return t != NULL;
}

void plat_thread_join(PlatThread *t) {
    pthread_join(t->t, NULL);
    free(t);
}

int plat_thread_join_ns(PlatThread *t, uint64_t ns) {
    struct timespec at;
    clock_gettime(CLOCK_REALTIME, &at);   // pthread_timedjoin_np takes a CLOCK_REALTIME deadline
    uint64_t end = (uint64_t)at.tv_sec * 1000000000u + (uint64_t)at.tv_nsec + ns;
    at.tv_sec = (time_t)(end / 1000000000u), at.tv_nsec = (long)(end % 1000000000u);
    if (pthread_timedjoin_np(t->t, NULL, &at)) return 0;
    free(t);
    return 1;
}

// The thread keeps using t->fn and t->arg, so the struct stays allocated (a few bytes per detached thread).
void plat_thread_detach(PlatThread *t) { pthread_detach(t->t); }

// ---- a core for the recording thread (plat_cpu_reserve)

// The processors a sysfs CPU list ("0-15", "0,2,4-7") names, added to *set; 0 when s is not such a list.
static int cpu_list_parse(const char *s, cpu_set_t *set) {
    for (;;) {
        char *e;
        long a = strtol(s, &e, 10), b = a;
        if (e == s || a < 0 || a >= CPU_SETSIZE) return 0;
        if (*e == '-') {
            s = e + 1;
            b = strtol(s, &e, 10);
            if (e == s || b < a || b >= CPU_SETSIZE) return 0;
        }
        for (long i = a; i <= b; i++) CPU_SET((int)i, set);
        if (*e != ',') return *e == '\n' || !*e;
        s = e + 1;
    }
}

static int cpu_list_file(const char *path, cpu_set_t *set) {
    char text[512];
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    CPU_ZERO(set);
    return cpu_list_parse(text, set);
}

// cpu<n>/<file> under root/system/cpu holds one number per processor (cpu_capacity, cpufreq/cpuinfo_max_freq). When
// the processors of `in` differ in it by more than a tenth, *out gets the ones within a tenth of the highest and the
// result is 1; otherwise 0 and *out stays as it was. A processor without the file counts as the lowest.
static int cpus_highest_by(const char *root, const char *file, const cpu_set_t *in, cpu_set_t *out) {
    uint64_t v[CPU_SETSIZE], top = 0, low = UINT64_MAX;
    for (int i = 0; i < CPU_SETSIZE; i++) {
        char path[160];
        v[i] = 0;
        if (!CPU_ISSET(i, in)) continue;
        snprintf(path, sizeof path, "%s/system/cpu/cpu%d/%s", root, i, file);
        if (!read_u64_file(path, &v[i])) v[i] = 0;
        if (v[i] > top) top = v[i];
        if (v[i] < low) low = v[i];
    }
    if (low * 10 >= top * 9) return 0;
    CPU_ZERO(out);
    for (int i = 0; i < CPU_SETSIZE; i++)
        if (CPU_ISSET(i, in) && v[i] * 10 >= top * 9) CPU_SET(i, out);
    return 1;
}

// The processors of `all` on the fastest kind of core, and what makes them so. A hybrid Intel CPU lists its performance
// cores in root/cpu_core/cpus; for the others, cores of different capacity or top clock (a tenth apart or less counts
// as the same) keep the highest; a CPU with one kind of core keeps all of `all`.
static const char *cpus_fast(const char *root, const cpu_set_t *all, cpu_set_t *fast) {
    cpu_set_t perf;
    char path[160];
    snprintf(path, sizeof path, "%s/cpu_core/cpus", root);
    if (cpu_list_file(path, &perf)) {
        CPU_AND(fast, &perf, all);
        if (CPU_COUNT(fast)) return "a performance core";
    }
    if (cpus_highest_by(root, "cpu_capacity", all, fast)) return "a core of the highest capacity";
    if (cpus_highest_by(root, "cpufreq/cpuinfo_max_freq", all, fast)) return "a core of the highest clock";
    *fast = *all;
    return "a core";
}

// The processors plat_cpu_reserve takes into *mine: every processor of `all` on the fast core (cpus_fast) that clocks
// highest, the highest-numbered one when several do. A core with a processor outside the fast ones is no fast core.
// Returns how many processors, 0 when fewer than four cores are fast: one of them is then too much to lose. *kind
// names what makes the core fast.
static int cpu_pick_core(const char *root, const cpu_set_t *all, cpu_set_t *mine, const char **kind) {
    cpu_set_t fast;
    *kind = cpus_fast(root, all, &fast);
    int cores = 0;
    uint64_t best = 0;
    for (int i = 0; i < CPU_SETSIZE; i++) {
        char path[160];
        cpu_set_t sib, in;
        uint64_t freq = 0;
        if (!CPU_ISSET(i, &fast)) continue;
        snprintf(path, sizeof path, "%s/system/cpu/cpu%d/topology/thread_siblings_list", root, i);
        if (!cpu_list_file(path, &sib)) CPU_ZERO(&sib);
        CPU_SET(i, &sib);
        CPU_AND(&sib, &sib, all);
        int first = 0;
        while (!CPU_ISSET(first, &sib)) first++;
        CPU_AND(&in, &sib, &fast);
        if (first != i || !CPU_EQUAL(&in, &sib)) continue;   // counted at its first processor, or not fast
        snprintf(path, sizeof path, "%s/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", root, i);
        read_u64_file(path, &freq);
        if (++cores == 1 || freq >= best) best = freq, *mine = sib;
    }
    return cores < 4 ? 0 : CPU_COUNT(mine);
}

// The calling thread gets a fast core to itself (cpu_pick_core): every other thread of the process now running loses
// the core's processors, and the caller keeps only them. Threads the caller starts later through plat_thread_new_ex
// get the others; a thread started by another one inherits that one's mask, which no longer has the core.
// plat_process_affinity leaves the core with the caller. Returns the processors reserved, 0 when no core is fast
// enough or the caller's mask cannot be set; *which (when not NULL) receives them as text, such as "10,11 of a
// performance core".
int plat_cpu_reserve(char *which, size_t size) {
    cpu_set_t all, mine, rest;
    const char *kind;
    pid_t me = (pid_t)syscall(SYS_gettid);
    if (which && size) which[0] = 0;
    if (sched_getaffinity(0, sizeof all, &all) || !cpu_pick_core("/sys/devices", &all, &mine, &kind)) return 0;
    CPU_XOR(&rest, &all, &mine);
    plat_mutex_lock(&cpu_lock);
    if (sched_setaffinity(0, sizeof mine, &mine)) {
        plat_mutex_unlock(&cpu_lock);
        return 0;
    }
    cpu_rest = rest, cpu_rest_set = 1;
    cpu_taken = mine, cpu_taker = me, cpu_others = rest;
    // A thread started during a pass by one it has not reached yet is found by the next.
    for (int pass = 0, moved = 1; moved && pass < 3; pass++) {
        DIR *d = opendir("/proc/self/task");
        moved = 0;
        for (struct dirent *e; d && (e = readdir(d));) {
            pid_t tid = (pid_t)atoi(e->d_name);
            cpu_set_t cur, now;
            if (tid <= 0 || tid == me || sched_getaffinity(tid, sizeof cur, &cur)) continue;
            now = cur;
            cpu_clear(&now, &mine);
            if (CPU_EQUAL(&now, &cur)) continue;
            if (!CPU_COUNT(&now)) now = rest;   // it was pinned to the core alone
            if (!sched_setaffinity(tid, sizeof now, &now)) moved = 1;
        }
        if (d) closedir(d);
    }
    plat_mutex_unlock(&cpu_lock);
    if (which && size) {
        size_t n = 0;
        for (int i = 0; i < CPU_SETSIZE && n < size; i++)
            if (CPU_ISSET(i, &mine)) n += (size_t)snprintf(which + n, size - n, "%s%d", n ? "," : "", i);
        if (n < size) snprintf(which + n, size - n, " of %s", kind);
    }
    return CPU_COUNT(&mine);
}

// A thread the holder starts by other means than plat_thread_new_ex (a library's audio thread, a driver's worker)
// inherits its mask, the core alone, after plat_cpu_reserve's sweep ran, and plat_process_affinity seldom runs again:
// each thread whose mask lies within the core, the holder aside, gets the others' processors here. A thread with
// anything beside the core in its mask keeps it. Once the holder has ended, nothing would use the core, so it goes back:
// the threads with the others' mask get its processors again and the reservation ends. Returns the threads moved, -1
// when the core went back.
int plat_cpu_reserve_sweep(void) {
    plat_mutex_lock(&cpu_lock);
    if (!cpu_taker) {
        plat_mutex_unlock(&cpu_lock);
        return 0;
    }
    char path[48];
    snprintf(path, sizeof path, "/proc/self/task/%d", (int)cpu_taker);
    int ended = access(path, F_OK) != 0, moved = 0;
    cpu_set_t back;
    CPU_OR(&back, &cpu_others, &cpu_taken);
    DIR *d = opendir("/proc/self/task");
    for (struct dirent *e; d && (e = readdir(d));) {
        pid_t tid = (pid_t)atoi(e->d_name);
        cpu_set_t cur, in;
        if (tid <= 0 || tid == cpu_taker || sched_getaffinity(tid, sizeof cur, &cur)) continue;
        if (ended) {
            if (CPU_EQUAL(&cur, &cpu_others) && !sched_setaffinity(tid, sizeof back, &back)) moved++;
            continue;
        }
        CPU_AND(&in, &cur, &cpu_taken);
        if (CPU_COUNT(&cur) && CPU_EQUAL(&in, &cur) && !sched_setaffinity(tid, sizeof cpu_others, &cpu_others)) moved++;
    }
    if (d) closedir(d);
    if (ended) {
        cpu_taker = 0;
        CPU_ZERO(&cpu_taken);
    }
    plat_mutex_unlock(&cpu_lock);
    return ended ? -1 : moved;
}

uint64_t plat_thread_affinity(PlatThread *t, uint64_t mask) {
    pthread_t th = t ? t->t : pthread_self();
    cpu_set_t old, set;
    if (pthread_getaffinity_np(th, sizeof old, &old)) return 0;
    CPU_ZERO(&set);
    for (int i = 0; i < 64; i++)
        if (mask >> i & 1) CPU_SET(i, &set);
    if (pthread_setaffinity_np(th, sizeof set, &set)) return 0;
    uint64_t prev = 0;
    for (int i = 0; i < 64; i++)
        if (CPU_ISSET(i, &old)) prev |= 1ull << i;
    return prev;
}

uint32_t plat_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (uint32_t)n : 1;
}

// The processors of the process's affinity mask (taskset, a launcher's CPU set), every online one when it is unknown.
uint32_t plat_cpu_usable(void) {
    cpu_set_t set;
    if (!sched_getaffinity(0, sizeof set, &set) && CPU_COUNT(&set) > 0) return (uint32_t)CPU_COUNT(&set);
    return plat_cpu_count();
}

// Windows' levels map to nice values: IDLE 19, LOWEST 4, BELOW_NORMAL 2, NORMAL 0, ABOVE_NORMAL -2, HIGHEST -5,
// TIME_CRITICAL -10. Applied only when the process may lower a nice value again (platform.h says why).
static int nice_of(int prio) {
    return prio <= PLAT_PRIO_IDLE ? 19 : prio == PLAT_PRIO_LOWEST ? 4 : prio == PLAT_PRIO_BELOW_NORMAL ? 2
         : prio == PLAT_PRIO_NORMAL ? 0 : prio == PLAT_PRIO_ABOVE_NORMAL ? -2 : prio == PLAT_PRIO_HIGHEST ? -5 : -10;
}

static int may_renice(int nice) {
    static int lowest = 100;   // the lowest nice value this process may set; 100 until probed
    if (lowest == 100) {
        struct rlimit r;
        int l = !getrlimit(RLIMIT_NICE, &r) && r.rlim_cur != RLIM_INFINITY ? 20 - (int)r.rlim_cur : -20;
        // CAP_SYS_NICE shows as being allowed to set a nice value below the limit's
        pid_t tid = (pid_t)syscall(SYS_gettid);
        errno = 0;
        int cur = getpriority(PRIO_PROCESS, (id_t)tid);
        if (!errno && l > cur - 1 && !setpriority(PRIO_PROCESS, (id_t)tid, cur - 1)) {
            setpriority(PRIO_PROCESS, (id_t)tid, cur);
            l = -20;
        }
        __atomic_store_n(&lowest, l, __ATOMIC_RELAXED);
    }
    return lowest <= 0 && nice >= lowest;
}

static void renice(pid_t tid, int prio) {
    int nice = nice_of(prio);
    if (tid > 0 && may_renice(nice)) setpriority(PRIO_PROCESS, (id_t)tid, nice);
}

void plat_thread_priority(int prio) { renice((pid_t)syscall(SYS_gettid), prio); }

void plat_thread_set_priority(PlatThread *t, int prio) { renice(__atomic_load_n(&t->tid, __ATOMIC_ACQUIRE), prio); }

int plat_thread_priority_get(void) {
    errno = 0;
    int nice = getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid));
    if (errno) return PLAT_PRIO_NORMAL;
    return nice >= 19 ? PLAT_PRIO_IDLE : nice >= 4 ? PLAT_PRIO_LOWEST : nice >= 2 ? PLAT_PRIO_BELOW_NORMAL
         : nice >= 0 ? PLAT_PRIO_NORMAL : nice >= -2 ? PLAT_PRIO_ABOVE_NORMAL : nice >= -5 ? PLAT_PRIO_HIGHEST
         : PLAT_PRIO_TIME_CRITICAL;
}

void plat_yield(void) { sched_yield(); }

// ---- time

uint64_t plat_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

uint64_t plat_counter(void) { return plat_now_ns(); }
uint64_t plat_counter_hz(void) { return 1000000000u; }

// The timer slack (50 us by default) is how late the kernel may wake a sleeping thread to batch wakeups. New threads
// inherit it from the thread that creates them.
void plat_timer_1ms(void) { prctl(PR_SET_TIMERSLACK, 1000ul, 0, 0, 0); }
int plat_power_full(void) { return 0; }   // no per-process power throttling to opt out of

// clock_nanosleep wakes within the timer slack (50 us by default) of its deadline; the last PLAT_SPIN_NS are spent
// yielding so the deadline is met closely, as on Windows.
#define PLAT_SPIN_NS 100000u

void plat_sleep_ns(uint64_t ns) {
    uint64_t end = plat_now_ns() + ns;
    if (ns > PLAT_SPIN_NS) {
        uint64_t at = end - PLAT_SPIN_NS;
        struct timespec ts = {(time_t)(at / 1000000000u), (long)(at % 1000000000u)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
    }
    while (plat_now_ns() < end) sched_yield();
}

void plat_wait_ns(uint64_t ns) {
    struct timespec ts = {(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

void plat_sleep_ms(uint32_t ms) {
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

// ---- locks: a reader-writer lock in one 32-bit word, zero when free.
// bit 31: a writer holds it; bit 30: some thread sleeps on the word; bits 0-29: readers holding it.
// A thread that cannot take the lock sets the sleeper bit and waits on the futex; every release that finds the bit
// clears it and wakes all sleepers, which retry (and set it again if they still cannot get in). Readers do not enter
// while the sleeper bit is set, so a waiting writer is not starved by a stream of readers.

#define LK_W 0x80000000u
#define LK_S 0x40000000u
#define LK_R 0x3fffffffu
#define LK_SPINS 64

static long futex(volatile uint32_t *w, int op, uint32_t val, const struct timespec *ts) {
    return syscall(SYS_futex, w, op | FUTEX_PRIVATE_FLAG, val, ts, NULL, 0);
}

static inline volatile uint32_t *lk(PlatMutex *m) { return (volatile uint32_t *)&m->p; }

// Sets the sleeper bit on s and sleeps while the word still equals the result.
static void lk_sleep(volatile uint32_t *w, uint32_t s) {
    if (!(s & LK_S) && !__atomic_compare_exchange_n(w, &s, s | LK_S, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    futex(w, FUTEX_WAIT, s | LK_S, NULL);
}

static void lk_wake(volatile uint32_t *w) {
    __atomic_fetch_and(w, ~LK_S, __ATOMIC_RELAXED);
    futex(w, FUTEX_WAKE, INT_MAX, NULL);
}

void plat_mutex_init(PlatMutex *m) { m->p = 0; }

int plat_mutex_trylock(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    uint32_t s = __atomic_load_n(w, __ATOMIC_RELAXED);
    while (!(s & (LK_W | LK_R)))
        if (__atomic_compare_exchange_n(w, &s, s | LK_W, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
    return 0;
}

// Lock-wait timing is Windows only (platform.h): here the note is kept and never called.
void plat_lock_timing(PlatLockNote fn) { (void)fn; }
void plat_mutex_lock_at(PlatMutex *m, void *site) { (void)site; plat_mutex_lock(m); }

void plat_mutex_lock(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    for (int spin = 0;; spin++) {
        uint32_t s = __atomic_load_n(w, __ATOMIC_RELAXED);
        if (!(s & (LK_W | LK_R))) {
            if (__atomic_compare_exchange_n(w, &s, s | LK_W, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
            continue;
        }
        if (spin < LK_SPINS) plat_pause();
        else lk_sleep(w, s);
    }
}

void plat_mutex_unlock(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    uint32_t s = __atomic_fetch_and(w, ~LK_W, __ATOMIC_RELEASE);
    if (s & LK_S) lk_wake(w);
}

int plat_mutex_trylock_shared(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    uint32_t s = __atomic_load_n(w, __ATOMIC_RELAXED);
    while (!(s & (LK_W | LK_S)))
        if (__atomic_compare_exchange_n(w, &s, s + 1, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
    return 0;
}

void plat_mutex_lock_shared(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    for (int spin = 0;; spin++) {
        uint32_t s = __atomic_load_n(w, __ATOMIC_RELAXED);
        if (!(s & (LK_W | LK_S))) {
            if (__atomic_compare_exchange_n(w, &s, s + 1, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
            continue;
        }
        if (spin < LK_SPINS) plat_pause();
        else lk_sleep(w, s);
    }
}

void plat_mutex_unlock_shared(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    uint32_t s = __atomic_sub_fetch(w, 1, __ATOMIC_RELEASE);
    if (!(s & LK_R) && (s & LK_S)) lk_wake(w);
}

// ---- condition variables: a sequence number in the first 32 bits. A waiter reads it before releasing the lock and
// sleeps only while it is unchanged, so a signal between the release and the sleep is not lost.

static inline volatile uint32_t *cv(PlatCond *c) { return (volatile uint32_t *)&c->p; }

void plat_cond_init(PlatCond *c) { c->p = 0; }

void plat_cond_wait(PlatCond *c, PlatMutex *m) {
    uint32_t seq = __atomic_load_n(cv(c), __ATOMIC_RELAXED);
    plat_mutex_unlock(m);
    futex(cv(c), FUTEX_WAIT, seq, NULL);
    plat_mutex_lock(m);
}

int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns) {
    uint32_t seq = __atomic_load_n(cv(c), __ATOMIC_RELAXED);
    struct timespec ts = {(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)};
    plat_mutex_unlock(m);
    long r = futex(cv(c), FUTEX_WAIT, seq, &ts);
    int timed_out = r == -1 && errno == ETIMEDOUT;
    plat_mutex_lock(m);
    return !timed_out;
}

void plat_cond_signal(PlatCond *c) {
    __atomic_add_fetch(cv(c), 1, __ATOMIC_RELEASE);
    futex(cv(c), FUTEX_WAKE, 1, NULL);
}

void plat_cond_broadcast(PlatCond *c) {
    __atomic_add_fetch(cv(c), 1, __ATOMIC_RELEASE);
    futex(cv(c), FUTEX_WAKE, INT_MAX, NULL);
}

// ---- files

int plat_file_exists(const char *path) {
    struct stat st;
    return !stat(path, &st) && !S_ISDIR(st.st_mode);
}

int plat_dir_exists(const char *path) {
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

int plat_full_path(const char *path, char *out, size_t cap) {
    if (path[0] == '/') return snprintf(out, cap, "%s", path) < (int)cap;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) return 0;
    return snprintf(out, cap, "%s/%s", cwd, path) < (int)cap;
}

void plat_file_mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s", path) >= (int)sizeof tmp) return;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = 0;
            mkdir(tmp, 0755);
            *p = c;
        }
    }
    mkdir(tmp, 0755);
}

static int write_all(int fd, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        p += k, n -= (size_t)k;
    }
    return 1;
}

uint8_t *plat_file_read(const char *path, uint64_t *size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    uint8_t *buf = NULL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < (1ll << 30)) {
        buf = malloc((size_t)st.st_size + 1);
        size_t got = 0;
        while (buf && got < (size_t)st.st_size) {
            ssize_t k = read(fd, buf + got, (size_t)st.st_size - got);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) break;
            got += (size_t)k;
        }
        if (buf && got != (size_t)st.st_size) {
            free(buf);
            buf = NULL;
        }
        if (buf) {
            buf[st.st_size] = 0;
            *size = (uint64_t)st.st_size;
        }
    }
    close(fd);
    return buf;
}

int plat_file_write(const char *path, const void *data, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return 0;
    int ok = write_all(fd, data, n);
    return close(fd) == 0 && ok;
}

// O_APPEND makes each write land whole at the end of the file, from any thread.
PlatAppend *plat_append_open(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
    return fd < 0 ? NULL : (PlatAppend *)(intptr_t)(fd + 1);
}

int plat_append_write(PlatAppend *f, const void *data, size_t n) {
    int fd = (int)(intptr_t)f - 1;
    ssize_t k;
    do k = write(fd, data, n);
    while (k < 0 && errno == EINTR);
    return k == (ssize_t)n;
}

// Flushes the directory holding path, so a rename into it survives a crash.
static void sync_dir_of(const char *path) {
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s", path) >= (int)sizeof dir) return;
    char *slash = strrchr(dir, '/');
    if (slash == dir) slash[1] = 0;
    else if (slash) *slash = 0;
    else strcpy(dir, ".");
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

int plat_file_replace(const char *from, const char *to, int durable) {
    if (rename(from, to)) return 0;
    if (durable) sync_dir_of(to);
    return 1;
}

static char lock_held[4][1100];
static int lock_fd[4];
static int lock_n;
static PlatMutex lock_lock = PLAT_MUTEX_INIT;

int plat_lock_file(const char *path) {
    plat_mutex_lock(&lock_lock);
    int r = 1, nheld = lock_n;
    for (int i = 0; i < nheld; i++)
        if (!strcmp(lock_held[i], path)) goto done;
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);   // open until plat_unlock_file or the process ends
    if (fd < 0) {
        r = -1;
        goto done;
    }
    if (flock(fd, LOCK_EX | LOCK_NB)) {
        r = errno == EWOULDBLOCK ? 0 : -1;
        close(fd);
        goto done;
    }
    if (nheld < 4) {
        snprintf(lock_held[nheld], sizeof lock_held[0], "%s", path);
        lock_fd[nheld] = fd;
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
            close(lock_fd[i]);
            lock_n--;
            memmove(&lock_held[i], &lock_held[i + 1], sizeof lock_held[0] * (size_t)(lock_n - i));
            memmove(&lock_fd[i], &lock_fd[i + 1], sizeof lock_fd[0] * (size_t)(lock_n - i));
            break;
        }
    plat_mutex_unlock(&lock_lock);
}

// path.<pid>.<tid>.tmp renamed over path; durable: the file and then its folder are synced to the disk.
static int write_swap(const char *path, const void *data, size_t n, int durable) {
    // A rename replaces a read-only file here (only the folder's permission counts) but not on Windows: a file the
    // player made read-only stays as it is on both.
    if (access(path, W_OK) && errno != ENOENT) return 0;
    char tmp[PATH_MAX + 48];
    int len = snprintf(tmp, sizeof tmp, "%s.%u.%u.tmp", path, plat_process_id(), plat_thread_id());
    if (len < 0 || len >= (int)sizeof tmp) return 0;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return 0;
    int ok = write_all(fd, data, n) && (!durable || fsync(fd) == 0);
    ok = close(fd) == 0 && ok;
    if (ok && !rename(tmp, path)) {
        if (durable) sync_dir_of(path);
        return 1;
    }
    unlink(tmp);
    return 0;
}

int plat_file_write_atomic(const char *path, const void *data, size_t n) { return write_swap(path, data, n, 1); }

int plat_file_write_cached(const char *path, const void *data, size_t n) { return write_swap(path, data, n, 0); }

int plat_file_flush(const char *path) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

int plat_file_sync(FILE *f) { return !fflush(f) && !fsync(fileno(f)); }

void plat_file_delete(const char *path) { unlink(path); }

int plat_file_truncate(const char *path, uint64_t size) { return truncate(path, (off_t)size) == 0; }

int64_t plat_file_size(const char *path) {
    struct stat st;
    return stat(path, &st) ? -1 : (int64_t)st.st_size;
}

// The owner's write permission stands for Windows' read-only attribute.
int plat_file_set_readonly(const char *path, int on) {
    struct stat st;
    if (stat(path, &st)) return 0;
    mode_t m = on ? st.st_mode & ~(mode_t)0222 : st.st_mode | S_IWUSR;
    return chmod(path, m & 07777) == 0;
}

int plat_file_private(const char *path) {
    struct stat st;
    if (stat(path, &st)) return 0;
    return chmod(path, S_ISDIR(st.st_mode) ? 0700 : 0600) == 0;
}

int plat_file_list(const char *dir, const char *ext, int (*cb)(const char *name, void *ctx), void *ctx) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t el = strlen(ext);
    for (struct dirent *e; (e = readdir(d));) {
        size_t n = strlen(e->d_name);
        if (n < el || strcmp(e->d_name + n - el, ext) || e->d_name[0] == '.') continue;
        if (!cb(e->d_name, ctx)) break;
    }
    closedir(d);
    return 1;
}

// ---- virtual memory. Reservations are PROT_NONE MAP_NORESERVE mappings; commit is mprotect read-write, decommit
// MADV_DONTNEED (private anonymous pages read zero afterwards) and PROT_NONE. Linux keeps no committed/reserved
// distinction, so a byte per guest page records it (pstate, mapped lazily: only the pages touched cost memory).
//
// Write watch: a userfaultfd in asynchronous write-protect mode (Linux 6.7) over the watched reservations. The kernel
// clears a page's protection on the first write without a round trip to us, and PAGEMAP_SCAN lists the pages whose
// protection was cleared and protects them again. Without it (an older kernel), plat_mem_written reports the ranges as
// untracked and the back end uploads them in full.

#define PG 4096ull
#define PS_LIMIT (1ull << 39)
enum { PS_FREE, PS_RESERVED, PS_NONE, PS_READ, PS_RW };   // PS_NONE.. are committed: PLAT_PROT_* + PS_NONE
static uint8_t *pstate;
static int ww_fd = -1, pm_fd = -1;   // userfaultfd and /proc/self/pagemap; -1 when write watch is unavailable
static PlatMutex vm_lock = PLAT_MUTEX_INIT;

static void ps_set(uint64_t addr, uint64_t n, uint8_t v) {
    if (addr < PS_LIMIT) memset(pstate + addr / PG, v, (size_t)((n + PG - 1) / PG));
}

// Guest memory as a memfd's (NX2NAVI_DMABUF, read from the environment: the reservations come before nx2navi.ini; on
// by default where /dev/udmabuf opens, udmabuf's size limit is 1 GiB or more and NVIDIA's kernel module is not loaded;
// 0 off, 1 on wherever it can be).
// The watched reservations are MAP_SHARED mappings of one memfd, each at its own offset, so that /dev/udmabuf can make
// a dma-buf of the game's own pages and the GPU driver import it (vk_memory.c "dma-buf imports"). A host-pointer
// import is a userptr object to amdgpu, which the kernel looks up page by page again at every submission and
// invalidates whenever the write watch or a freeze changes the pages' protection; a udmabuf pins the memfd's pages
// once and never looks at the process's page tables, so protection changes leave the GPU's mapping alone. Everything
// else stays as it was: the write watch, the freezes and the access faults work the same on a shared mapping.
// Two things differ from private memory, and both are handled here under vm_lock:
// - Decommitted pages must read zero when committed again. MADV_DONTNEED leaves a shared mapping's pages in the
//   memfd, so a decommit punches them out of it (fallocate) instead. A page a dma-buf pins cannot go that way: the
//   dma-buf would keep the old page while the CPU got a new one, and the GPU and the CPU would no longer see the same
//   memory. Those pages are zeroed in place (gm_decommit).
// - A dma-buf pins every page of its range, allocating the ones nothing has touched. When it is gone
//   (plat_mem_dmabuf_done) the pages of its range that are not committed and that no other dma-buf pins are punched
//   out, so they cost no memory.
#if __has_include(<linux/udmabuf.h>)
#include <linux/udmabuf.h>
#else   // the kernel's ABI (include/uapi/linux/udmabuf.h), for headers older than it
struct udmabuf_create { __u32 memfd; __u32 flags; __u64 offset; __u64 size; };
#define UDMABUF_FLAGS_CLOEXEC 0x01
#define UDMABUF_CREATE _IOW('u', 0x42, struct udmabuf_create)
#endif
#define GM_RES_MAX 4
typedef struct { uint64_t addr, n, off; } GmRes;
typedef struct { uint64_t lo, hi; } GmPin;
static int gm_state;                   // vm_lock: 0 not decided, 1 the watched reservations are the memfd's, -1 not
static int gm_fd = -1, gm_udev = -1;   // the memfd and /dev/udmabuf
static uint64_t gm_size, gm_limit;     // bytes of the memfd handed out; udmabuf's size limit
static GmRes gm_res[GM_RES_MAX];       // the shared reservations and their offsets in the memfd
static int gm_nres;
static GmPin *gm_pins;                 // vm_lock: the ranges live dma-bufs pin (one entry per dma-buf)
static int gm_npins, gm_cpins;
static char gm_note[200];              // why the reservations are not a memfd's ("" when NX2NAVI_DMABUF=0 or they are)

// The largest dma-buf the udmabuf ioctl accepts, in bytes: the module's size_limit_mb parameter, or the module's
// default of 64 MiB where the parameter cannot be read.
static uint64_t udmabuf_limit(void) {
    uint64_t mib;
    if (!read_u64_file("/sys/module/udmabuf/parameters/size_limit_mb", &mib)) mib = 64;
    return (mib < (1ull << 40) ? mib : 1ull << 40) << 20;
}

static void gm_open(void) {   // vm_lock held: the first watched reservation decides
    const char *e = getenv("NX2NAVI_DMABUF");
    int asked = e && !strcmp(e, "1");
    gm_state = -1;
    if (e && !strcmp(e, "0")) return;
    // Unset: on where it can work and pays. NVIDIA's driver imports host pointers without amdgpu's costs (vk_memory.c
    // mirror_mode), and its import of these dma-bufs has not been tried, so where its kernel module is loaded the memory
    // stays private.
    if (!asked && !access("/sys/module/nvidia", F_OK)) {
        snprintf(gm_note, sizeof gm_note, "NVIDIA's kernel module is loaded (NX2NAVI_DMABUF=1 makes it a memfd's anyway)");
        return;
    }
    gm_udev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (gm_udev < 0) {   // many systems give the device to root and group kvm only
        snprintf(gm_note, sizeof gm_note, "/dev/udmabuf cannot be opened (%s)", strerror(errno));
        fprintf(stderr, "platform: guest memory as dma-bufs (NX2NAVI_DMABUF): %s: guest memory stays private\n", gm_note);
        return;
    }
    gm_limit = udmabuf_limit();
    // The game's largest pools need about 300 MiB in one dma-buf (vk_memory.c mirror_mode): below 1 GiB the back end
    // would keep the shadow copies anyway, so unasked the memory stays private.
    if (!asked && gm_limit < (1ull << 30)) {
        snprintf(gm_note, sizeof gm_note, "udmabuf's size_limit_mb is %llu, below the 1024 a pool of the game may need "
                 "(/sys/module/udmabuf/parameters/size_limit_mb)", (unsigned long long)(gm_limit >> 20));
        close(gm_udev), gm_udev = -1;
        return;
    }
    gm_fd = (int)syscall(SYS_memfd_create, "nx2navi-guest", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (gm_fd < 0) {
        snprintf(gm_note, sizeof gm_note, "no memfd (%s)", strerror(errno));
        fprintf(stderr, "platform: guest memory as dma-bufs (NX2NAVI_DMABUF): %s: guest memory stays private\n", gm_note);
        close(gm_udev), gm_udev = -1;
        return;
    }
    gm_state = 1;
}

static int gm_find(uint64_t addr, uint64_t n) {   // the shared reservation holding [addr, addr + n), -1 none
    for (int i = 0; i < gm_nres; i++)
        if (addr >= gm_res[i].addr && n <= gm_res[i].n && addr - gm_res[i].addr <= gm_res[i].n - n) return i;
    return -1;
}

// vm_lock held. Whether x is pinned by a live dma-buf; *end gets the end of the run of [x, limit) that is the same.
static int gm_pinned_run(uint64_t x, uint64_t limit, uint64_t *end) {
    uint64_t cur = x;
    for (int grew = 1; grew && cur < limit;) {
        grew = 0;
        for (int i = 0; i < gm_npins; i++)
            if (gm_pins[i].lo <= cur && cur < gm_pins[i].hi) cur = gm_pins[i].hi, grew = 1;
    }
    if (cur > x) {
        *end = cur < limit ? cur : limit;
        return 1;
    }
    uint64_t next = limit;
    for (int i = 0; i < gm_npins; i++)
        if (gm_pins[i].lo > x && gm_pins[i].lo < next) next = gm_pins[i].lo;
    *end = next;
    return 0;
}

static int gm_punch(int gi, uint64_t addr, uint64_t n) {
    return !fallocate(gm_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)(gm_res[gi].off + addr - gm_res[gi].addr),
                      (off_t)n);
}

// vm_lock held: a decommit of [addr, addr + n) in shared reservation gi.
static int gm_decommit(int gi, uint64_t addr, uint64_t n) {
    uint64_t end = addr + n;
    for (uint64_t x = addr, y; x < end; x = y) {
        if (!gm_pinned_run(x, end, &y)) {
            if (!gm_punch(gi, x, y - x)) return 0;
            continue;
        }
        for (uint64_t a = x, b; a < y; a = b) {   // the committed runs; a reserved page is zero already
            uint8_t v = pstate[a / PG];
            for (b = a + PG; b < y && pstate[b / PG] == v; b += PG) {}
            if (v < PS_NONE) continue;
            if (v != PS_RW && mprotect((void *)(uintptr_t)a, b - a, PROT_READ | PROT_WRITE)) return 0;
            memset((void *)(uintptr_t)a, 0, b - a);
        }
    }
    if (mprotect((void *)(uintptr_t)addr, n, PROT_NONE)) return 0;
    ps_set(addr, n, PS_RESERVED);
    return 1;
}

int plat_mem_dmabuf_ok(uint64_t *limit, uint64_t *any, const char **why) {
    plat_mutex_lock(&vm_lock);
    int ok = gm_state > 0 && gm_nres > 0;
    *limit = gm_limit;
    *any = gm_nres ? gm_res[0].addr : 0;
    *why = ok ? "" : gm_state > 0 ? "the memfd could not back a reservation" : gm_note;
    plat_mutex_unlock(&vm_lock);
    return ok;
}

int plat_mem_dmabuf(uint64_t addr, uint64_t n) {
    if (!n || ((addr | n) & (PG - 1))) return -1;
    plat_mutex_lock(&vm_lock);
    int gi = gm_state > 0 && n <= gm_limit ? gm_find(addr, n) : -1, fd = -1, err = EINVAL;
    if (gi >= 0 && gm_npins == gm_cpins) {
        int c = gm_cpins ? gm_cpins * 2 : 64;
        GmPin *p = realloc(gm_pins, (size_t)c * sizeof *p);
        if (p) gm_pins = p, gm_cpins = c;
    }
    if (gi >= 0 && gm_npins < gm_cpins) {
        struct udmabuf_create c = {.memfd = (__u32)gm_fd, .flags = UDMABUF_FLAGS_CLOEXEC,
                                   .offset = gm_res[gi].off + addr - gm_res[gi].addr, .size = n};
        fd = ioctl(gm_udev, UDMABUF_CREATE, &c);
        err = errno;
        if (fd >= 0) gm_pins[gm_npins++] = (GmPin){addr, addr + n};
    }
    plat_mutex_unlock(&vm_lock);
    if (fd < 0) errno = err;
    return fd;
}

void plat_mem_dmabuf_done(uint64_t addr, uint64_t n) {
    plat_mutex_lock(&vm_lock);
    for (int i = 0; i < gm_npins; i++)
        if (gm_pins[i].lo == addr && gm_pins[i].hi == addr + n) {
            gm_pins[i] = gm_pins[--gm_npins];
            break;
        }
    int gi = gm_find(addr, n);
    uint64_t end = addr + n;
    for (uint64_t x = addr, y; gi >= 0 && x < end; x = y) {
        if (gm_pinned_run(x, end, &y)) continue;
        for (uint64_t a = x, b; a < y; a = b) {
            uint8_t v = pstate[a / PG];
            for (b = a + PG; b < y && pstate[b / PG] == v; b += PG) {}
            if (v < PS_NONE) gm_punch(gi, a, b - a);
        }
    }
    plat_mutex_unlock(&vm_lock);
}

static void ww_open(void) {
    int fd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    struct uffdio_api api = {.api = UFFD_API,
                             .features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED};
    if (fd < 0 || ioctl(fd, UFFDIO_API, &api)) {
        fprintf(stderr, "platform: no asynchronous userfaultfd write protection (%s): written memory is not tracked\n",
                strerror(errno));
        if (fd >= 0) close(fd);
        return;
    }
    pm_fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (pm_fd < 0) {
        close(fd);
        return;
    }
    ww_fd = fd;
}

int plat_mem_reserve(uint64_t addr, uint64_t n, int watch) {
    plat_mutex_lock(&vm_lock);
    if (!pstate) {
        void *p = mmap(NULL, PS_LIMIT / PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        pstate = p == MAP_FAILED ? NULL : p;
    }
    if (watch && ww_fd < 0 && pm_fd < 0) ww_open();
    if (watch && !gm_state) gm_open();
    int shared = watch && gm_state > 0 && gm_nres < GM_RES_MAX;
    void *p = pstate && addr + n <= PS_LIMIT
                  ? mmap((void *)(uintptr_t)addr, n, PROT_NONE,
                         (shared ? MAP_SHARED : MAP_PRIVATE | MAP_ANONYMOUS) | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                         shared ? gm_fd : -1, shared ? (off_t)gm_size : 0)
                  : MAP_FAILED;
    int ok = p == (void *)(uintptr_t)addr;
    if (p != MAP_FAILED && !ok) munmap(p, n);   // a kernel without MAP_FIXED_NOREPLACE put it elsewhere
    if (ok && shared) {   // the memfd grows by the reservation; shrinking is sealed off, as udmabuf requires
        if (ftruncate(gm_fd, (off_t)(gm_size + n)) || (!gm_nres && fcntl(gm_fd, F_ADD_SEALS, F_SEAL_SHRINK))) {
            fprintf(stderr, "platform: the memfd for %#llx+%#llx failed (%s): private memory there\n",
                    (unsigned long long)addr, (unsigned long long)n, strerror(errno));
            munmap(p, n);
            p = mmap((void *)(uintptr_t)addr, n, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
            ok = p == (void *)(uintptr_t)addr;
            if (p != MAP_FAILED && !ok) munmap(p, n);
        } else {
            gm_res[gm_nres++] = (GmRes){addr, n, gm_size};
            gm_size += n;
        }
    }
    if (ok) {
        ps_set(addr, n, PS_RESERVED);
        // Transparent huge pages would populate (and count as written) 2 MiB at the first write of one page; writes are
        // tracked per 4 KiB page, as on Windows.
        if (watch) madvise(p, n, MADV_NOHUGEPAGE);
        struct uffdio_register reg = {.range = {addr, n}, .mode = UFFDIO_REGISTER_MODE_WP};
        if (watch && ww_fd >= 0 && ioctl(ww_fd, UFFDIO_REGISTER, &reg))
            fprintf(stderr, "platform: write watch over %#llx+%#llx failed (%s)\n", (unsigned long long)addr,
                    (unsigned long long)n, strerror(errno));
    }
    plat_mutex_unlock(&vm_lock);
    return ok;
}

void *plat_mem_alloc(size_t n) {
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

int plat_mem_commit(uint64_t addr, size_t n) {
    // In a shared reservation under vm_lock: plat_mem_dmabuf_done punches out the pages it finds not committed.
    int locked = gm_nres > 0 && gm_find(addr, n) >= 0;
    if (locked) plat_mutex_lock(&vm_lock);
    int ok = !mprotect((void *)(uintptr_t)addr, n, PROT_READ | PROT_WRITE);
    if (ok) ps_set(addr, n, PS_RW);
    if (locked) plat_mutex_unlock(&vm_lock);
    return ok;
}

int plat_mem_decommit(uint64_t addr, uint64_t n) {
    if (gm_nres > 0) {
        plat_mutex_lock(&vm_lock);
        int gi = gm_find(addr, n), ok = gi >= 0 && gm_decommit(gi, addr, n);
        plat_mutex_unlock(&vm_lock);
        if (gi >= 0) return ok;
    }
    if (madvise((void *)(uintptr_t)addr, n, MADV_DONTNEED) || mprotect((void *)(uintptr_t)addr, n, PROT_NONE)) return 0;
    ps_set(addr, n, PS_RESERVED);
    return 1;
}

int plat_mem_protect(uint64_t addr, uint64_t n, int prot) {
    int p = prot == PLAT_PROT_RW ? PROT_READ | PROT_WRITE : prot == PLAT_PROT_READ ? PROT_READ : PROT_NONE;
    if (mprotect((void *)(uintptr_t)addr, n, p)) return 0;
    ps_set(addr, n, (uint8_t)(PS_NONE + prot));
    return 1;
}

int plat_mem_state(uint64_t addr, uint64_t limit, uint64_t *end) {
    if (!pstate || addr >= PS_LIMIT) {
        *end = limit;
        return PLAT_MEM_FREE;
    }
    uint64_t lim = limit < PS_LIMIT ? limit : PS_LIMIT, i = addr / PG;
    uint8_t v = pstate[i];
    while ((i + 1) * PG < lim && pstate[i + 1] == v) i++;
    *end = (i + 1) * PG < lim ? (i + 1) * PG : lim;
    return v == PS_FREE ? PLAT_MEM_FREE : v == PS_RESERVED ? PLAT_MEM_RESERVED : v - PS_NONE;
}

int plat_mem_census(uint64_t *priv, uint64_t *wc, uint64_t *wc_allocs) {
    *priv = *wc = *wc_allocs = 0;
    return 0;
}

int plat_mem_prepare_write(uint64_t addr, uint64_t n) {
    if (ww_fd < 0 || !n || addr >= PS_LIMIT || n > PS_LIMIT - addr) return 0;
    uint64_t lo = addr & ~(PG - 1), hi = (addr + n + PG - 1) & ~(PG - 1);
    struct uffdio_writeprotect wp = {.range = {lo, hi - lo}, .mode = 0};
    // PAGEMAP_SCAN observes cleared protection as written, just as after an asynchronous write fault.
    return ioctl(ww_fd, UFFDIO_WRITEPROTECT, &wp) == 0;
}

uint64_t plat_mem_written(uint64_t addr, uint64_t n, uint64_t *pages, uint64_t max, int reset) {
    if (!max || !n) return 0;
    if (ww_fd < 0) return UINT64_MAX;
    struct page_region r[256];
    uint64_t got = 0;
    for (uint64_t at = addr, end = addr + n; at < end && got < max;) {
        // Written means: write-protection cleared, on a populated page (present or swapped out) that is not the shared
        // zero page. A page never touched since its commit has no protection marker either and counts as written to
        // the kernel; a read maps the zero page there. Neither is a write, as on Windows.
        struct pm_scan_arg arg = {.size = sizeof arg,
                                  .flags = reset ? PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC : 0,
                                  .start = at, .end = end, .vec = (uint64_t)(uintptr_t)r, .vec_len = 256,
                                  .max_pages = max - got,
                                  .category_inverted = PAGE_IS_PFNZERO,
                                  .category_mask = PAGE_IS_WRITTEN | PAGE_IS_PFNZERO,
                                  .category_anyof_mask = PAGE_IS_PRESENT | PAGE_IS_SWAPPED,
                                  .return_mask = PAGE_IS_WRITTEN};
        long k = ioctl(pm_fd, PAGEMAP_SCAN, &arg);
        if (k < 0) return UINT64_MAX;
        for (long i = 0; i < k; i++)
            for (uint64_t p = r[i].start; p < r[i].end && got < max; p += PG) pages[got++] = p;
        if (arg.walk_end <= at) break;
        at = arg.walk_end;
    }
    return got;
}

// One handler for the synchronous faults. A SIGSEGV whose page-fault error code has the write bit set goes to the
// write-fault hook first (memory.c thaws a frozen region); any other fault, in this executable or in a library (Mesa,
// SDL), then goes to the crash hook (main.c), which reports it and ends the process. Without a crash hook the fault
// gets the default action. Runs on the thread's signal stack.
static int (*fault_hook)(uint64_t addr);
static int (*access_hook)(uint64_t addr, int write, uint64_t ip);   // every read or write fault, before fault_hook
static void (*crash_fn)(const PlatCrash *c);

static void fault_handler(int sig, siginfo_t *si, void *uc) {
    ucontext_t *u = uc;
    long err = u->uc_mcontext.gregs[REG_ERR];
    if (sig == SIGSEGV && access_hook && !(err & 16) &&
        access_hook((uint64_t)(uintptr_t)si->si_addr, (err & 2) != 0, (uint64_t)u->uc_mcontext.gregs[REG_RIP]))
        return;
    if (sig == SIGSEGV && fault_hook && (err & 2) && fault_hook((uint64_t)(uintptr_t)si->si_addr)) return;
    uintptr_t rip = (uintptr_t)u->uc_mcontext.gregs[REG_RIP];
    Dl_info at, self;
    if (crash_fn) {
        int found = dladdr((void *)rip, &at) && at.dli_fname;
        PlatCrash c = {sig == SIGSEGV ? "access violation" : sig == SIGBUS ? "bus error"
                       : sig == SIGILL ? "illegal instruction" : "arithmetic exception",
                       (uint32_t)sig, rip, found ? rip - (uintptr_t)at.dli_fbase : rip, -1, 0};
        if (!found) {
            c.module = "";
        } else if (!dladdr((void *)plat_crash_hook, &self) || at.dli_fbase != self.dli_fbase) {
            const char *n = strrchr(at.dli_fname, '/');
            c.module = n ? n + 1 : at.dli_fname;
        }
        if (sig == SIGSEGV || sig == SIGBUS)
            c.access = (err & 16) ? 8 : (err & 2) ? 1 : 0, c.addr = (uint64_t)(uintptr_t)si->si_addr;
        crash_fn(&c);
    }
    signal(sig, SIG_DFL);   // returns into the faulting instruction, which faults again with the default action
}

static void fault_install(void) {
    static int done;
    if (done) return;
    done = 1;
    alt_stack();
    struct sigaction sa = {.sa_sigaction = fault_handler, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

void plat_mem_fault_hook(int (*fn)(uint64_t addr)) {
    fault_hook = fn;
    fault_install();
}

void plat_mem_access_hook(int (*fn)(uint64_t addr, int write, uint64_t ip)) {
    access_hook = fn;
    fault_install();
}

void plat_crash_context(const char *s) { (void)s; }   // the Windows crash notes only
int plat_crash_filter_reassert(char *who, size_t n) { (void)who, (void)n; return 0; }   // Windows only
const char *plat_crash_context_get(void) { return NULL; }

// plat_jmp_save / plat_jmp_go (platform.h): leaf code that does not move rsp before it leaves, as on Windows.
_Static_assert(sizeof(PlatJmp) == 72 && __builtin_offsetof(PlatJmp, mxcsr) == 64, "plat_jmp_save layout");
__asm__(".text\n"
        ".globl plat_jmp_save\n"
        ".type plat_jmp_save, @function\n"
        ".p2align 4\n"
        "plat_jmp_save:\n"
        "    movq %rbx, 0(%rdi)\n"
        "    movq %rbp, 8(%rdi)\n"
        "    movq %r12, 16(%rdi)\n"
        "    movq %r13, 24(%rdi)\n"
        "    movq %r14, 32(%rdi)\n"
        "    movq %r15, 40(%rdi)\n"
        "    leaq 8(%rsp), %rdx\n"   // the caller's rsp after this call returns
        "    movq %rdx, 48(%rdi)\n"
        "    movq (%rsp), %rdx\n"
        "    movq %rdx, 56(%rdi)\n"
        "    stmxcsr 64(%rdi)\n"
        "    fnstcw 68(%rdi)\n"
        "    xorl %eax, %eax\n"
        "    ret\n"
        ".size plat_jmp_save, .-plat_jmp_save\n"
        ".globl plat_jmp_go\n"
        ".type plat_jmp_go, @function\n"
        ".p2align 4\n"
        "plat_jmp_go:\n"
        "    movq 0(%rdi), %rbx\n"
        "    movq 8(%rdi), %rbp\n"
        "    movq 16(%rdi), %r12\n"
        "    movq 24(%rdi), %r13\n"
        "    movq 32(%rdi), %r14\n"
        "    movq 40(%rdi), %r15\n"
        "    ldmxcsr 64(%rdi)\n"
        "    fldcw 68(%rdi)\n"
        "    movq 48(%rdi), %rsp\n"
        "    movl $1, %eax\n"
        "    jmpq *56(%rdi)\n"
        ".size plat_jmp_go, .-plat_jmp_go\n");

// The DWARF unwinder of libgcc (already linked for C++), frame by frame from plat_frame_capture's caller. In each
// frame's callback the IP is its return address, the callee-saved registers are as that frame sees them, and
// _Unwind_GetCFA is its rsp once the call returns (libgcc's context holds the CFA of the frame just unwound).
typedef struct {
    PlatJmp *out;
    int (*want)(uintptr_t rip, void *arg);
    void *arg;
    int n, found;
} FrameWalk;

static _Unwind_Reason_Code frame_step(struct _Unwind_Context *ctx, void *p) {
    FrameWalk *w = p;
    uintptr_t ip = _Unwind_GetIP(ctx), sp = _Unwind_GetCFA(ctx);
    if (w->n++ == 0) return _URC_NO_REASON;   // plat_frame_capture itself
    if (!ip || w->n > 256) return _URC_END_OF_STACK;
    if (!w->want(ip, w->arg)) return _URC_NO_REASON;
    PlatJmp *o = w->out;
    o->gpr[0] = _Unwind_GetGR(ctx, 3), o->gpr[1] = _Unwind_GetGR(ctx, 6);   // DWARF rbx, rbp
    for (int r = 12; r <= 15; r++) o->gpr[2 + r - 12] = _Unwind_GetGR(ctx, r);
    o->gpr[6] = sp, o->gpr[7] = ip;
    __asm__ volatile("stmxcsr %0\n\tfnstcw %1" : "=m"(o->mxcsr), "=m"(o->fpcw));
    w->found = 1;
    return _URC_END_OF_STACK;
}

__attribute__((noinline)) int plat_frame_capture(PlatJmp *out, int (*want)(uintptr_t rip, void *arg), void *arg) {
    FrameWalk w = {out, want, arg, 0, 0};
    _Unwind_Backtrace(frame_step, &w);
    return w.found;
}

void plat_crash_hook(void (*fn)(const PlatCrash *c)) {
    crash_fn = fn;
    fault_install();
}

void plat_reexec(void) {
    static char args[16384];
    char *argv[256];
    int argc = 0;
    FILE *f = fopen("/proc/self/cmdline", "rb");
    size_t n = f ? fread(args, 1, sizeof args - 1, f) : 0;
    if (f) fclose(f);
    args[n] = 0;
    for (size_t i = 0; i < n && argc < 255; i += strlen(args + i) + 1) argv[argc++] = args + i;
    argv[argc] = NULL;
    fflush(stdout);
    fflush(stderr);
    execv("/proc/self/exe", argv);
    fprintf(stderr, "FATAL: re-running the process failed (%s)\n", strerror(errno));
    _exit(70);
}

int plat_mem_read(uint64_t addr, void *out, size_t n) {
    struct iovec local = {out, n}, remote = {(void *)(uintptr_t)addr, n};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)n;
}

int plat_mem_readable(uint64_t addr, uint64_t n) {
    uint8_t b;
    uint64_t end = addr + (n ? n : 1);
    for (uint64_t a = addr; a < end; a = (a | 4095) + 1)
        if (!plat_mem_read(a, &b, 1)) return 0;
    return 1;
}

void *plat_aligned_alloc(size_t size, size_t align) {
    void *p = NULL;
    return posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, size) ? NULL : p;
}

void plat_aligned_free(void *p) { free(p); }

// A PlatFile is the descriptor + 1, so descriptor 0 is not NULL.
PlatFile *plat_file_open(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    return fd < 0 ? NULL : (PlatFile *)(intptr_t)(fd + 1);
}

PlatFile *plat_file_open_unbuffered(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_DIRECT);
    return fd < 0 ? NULL : (PlatFile *)(intptr_t)(fd + 1);
}

static int fd_of(PlatFile *f) { return (int)(intptr_t)f - 1; }

void plat_file_close(PlatFile *f) {
    if (f) close(fd_of(f));
}

int64_t plat_file_pread(PlatFile *f, void *buf, uint32_t n, uint64_t off) {
    ssize_t k;
    do k = pread(fd_of(f), buf, n, (off_t)off);
    while (k < 0 && errno == EINTR);
    return k >= 0 ? k : errno == EFAULT ? -2 : -1;
}

int plat_file_id(PlatFile *f, PlatFileId *out) {
    struct stat st;
    if (fstat(fd_of(f), &st)) return 0;
    *out = (PlatFileId){(uint64_t)st.st_dev, (uint64_t)st.st_ino, (uint64_t)st.st_size,
                        (uint64_t)st.st_mtim.tv_sec * 1000000000u + (uint64_t)st.st_mtim.tv_nsec,
                        (uint64_t)st.st_ctim.tv_sec * 1000000000u + (uint64_t)st.st_ctim.tv_nsec};
    return 1;
}

const void *plat_file_map(PlatFile *f) {
    struct stat st;
    if (fstat(fd_of(f), &st) || !st.st_size) return NULL;
    void *v = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd_of(f), 0);
    return v == MAP_FAILED ? NULL : v;
}

int plat_fs_os_error(void) { return errno; }

static int fs_err(int e) {
    switch (e) {
    case ENOENT: case ENOTDIR: return PLAT_FS_NOT_FOUND;
    case EEXIST: return PLAT_FS_EXISTS;
    case ENOTEMPTY: return PLAT_FS_NOT_EMPTY;
    case EBUSY: case ETXTBSY: return PLAT_FS_BUSY;
    default: return PLAT_FS_ERROR;
    }
}

int plat_fs_kind(const char *path) {
    struct stat st;
    if (lstat(path, &st)) return PLAT_FS_MISSING;
    return S_ISLNK(st.st_mode) ? PLAT_FS_LINK : S_ISDIR(st.st_mode) ? PLAT_FS_DIR : PLAT_FS_FILE;
}

int plat_fs_mkdir(const char *path) { return mkdir(path, 0755) ? fs_err(errno) : PLAT_FS_OK; }

int plat_fs_rmdir(const char *path) {
    struct stat st;
    if (!lstat(path, &st) && S_ISLNK(st.st_mode)) return unlink(path) ? fs_err(errno) : PLAT_FS_OK;
    return rmdir(path) ? fs_err(errno) : PLAT_FS_OK;
}

// Deleting needs write access to the directory, not to the file: a read-only file goes as any other.
int plat_fs_unlink(const char *path) { return unlink(path) ? fs_err(errno) : PLAT_FS_OK; }

// RENAME_NOREPLACE keeps Windows' rule that a rename never replaces; a file system without it (EINVAL) gets a check
// first, which another process could race but the save directories are ours.
int plat_fs_rename(const char *from, const char *to, int durable) {
    if (syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE)) {
        if (errno != EINVAL && errno != ENOSYS) return fs_err(errno);
        if (plat_fs_kind(to) != PLAT_FS_MISSING) return PLAT_FS_EXISTS;
        if (rename(from, to)) return fs_err(errno);
    }
    if (durable) {
        sync_dir_of(from);
        sync_dir_of(to);
    }
    return PLAT_FS_OK;
}

int plat_fs_link(const char *existing, const char *path) { return link(existing, path) ? fs_err(errno) : PLAT_FS_OK; }

int plat_fs_links(const char *path) {
    struct stat st;
    return stat(path, &st) ? 0 : (int)st.st_nlink;
}

void plat_fs_sync_dir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

// Open files never stop a rename here.
void plat_fs_busy_reason(const char *dir, char *out, size_t cap) {
    (void)dir;
    if (cap) *out = 0;
}

int plat_fs_list(const char *dir, int (*cb)(const char *name, int kind, uint64_t size, void *ctx), void *ctx) {
    DIR *d = opendir(dir);
    if (!d) return fs_err(errno);
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        struct stat st;
        if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW)) continue;
        int kind = S_ISLNK(st.st_mode) ? PLAT_FS_LINK : S_ISDIR(st.st_mode) ? PLAT_FS_DIR : PLAT_FS_FILE;
        if (!cb(e->d_name, kind, kind == PLAT_FS_FILE ? (uint64_t)st.st_size : 0, ctx)) break;
    }
    closedir(d);
    return PLAT_FS_OK;
}

PlatFile *plat_fs_open(const char *path, int write, int *err) {
    struct stat st;
    int fd = open(path, (write ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd >= 0 && (fstat(fd, &st) || !S_ISREG(st.st_mode))) {
        close(fd);
        fd = -1, errno = ENOENT;
    }
    if (fd < 0 && err) *err = fs_err(errno);
    return fd < 0 ? NULL : (PlatFile *)(intptr_t)(fd + 1);
}

PlatFile *plat_fs_create(const char *path, int *err) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0 && err) *err = fs_err(errno);
    return fd < 0 ? NULL : (PlatFile *)(intptr_t)(fd + 1);
}

int64_t plat_file_pwrite(PlatFile *f, const void *buf, uint32_t n, uint64_t off) {
    ssize_t k;
    do k = pwrite(fd_of(f), buf, n, (off_t)off);
    while (k < 0 && errno == EINTR);
    return k;
}

int plat_file_set_size(PlatFile *f, uint64_t size) { return ftruncate(fd_of(f), (off_t)size) ? fs_err(errno) : 0; }

int plat_file_fsync(PlatFile *f) { return fsync(fd_of(f)) == 0; }

int plat_temp_dir(char *out, size_t cap) {
    const char *t = getenv("TMPDIR");
    if (!t || !*t) t = "/tmp";
    size_t n = strlen(t);
    while (n > 1 && t[n - 1] == '/') n--;
    return snprintf(out, cap, "%.*s", (int)n, t) < (int)cap;
}

uintptr_t plat_exe_base(void) {
    Dl_info d;
    return dladdr((void *)plat_exe_base, &d) ? (uintptr_t)d.dli_fbase : 0;
}

int plat_module_of(uintptr_t addr, char *name, size_t cap, uintptr_t *base) {
    Dl_info d;
    if (!dladdr((void *)addr, &d) || !d.dli_fname) return 0;
    const char *n = strrchr(d.dli_fname, '/');
    *base = (uintptr_t)d.dli_fbase;
    return snprintf(name, cap, "%s", n ? n + 1 : d.dli_fname) < (int)cap;
}

int plat_exe_path(char *out, size_t cap) {
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0 || (size_t)n >= cap - 1) return 0;
    out[n] = 0;
    return 1;
}

size_t plat_getenv(const char *name, char *out, size_t cap) {
    const char *v = getenv(name);
    size_t n = v ? strlen(v) : 0;
    if (!n || n >= cap) return 0;
    memcpy(out, v, n + 1);
    return n;
}

// Cached per thread: plat_rmutex_lock asks on every acquire, hundreds of thousands of times a second in play, and
// gettid is a full syscall on Linux (Windows reads GetCurrentThreadId from the TEB). A thread's id never changes, and
// the runtime starts child processes with posix_spawn only, so no forked copy inherits a stale id. noinline: a fiber
// may resume on another thread (platform.h), and a caller that inlined this could keep the address of tid it computed
// before a plat_fiber_switch; as a call it reads the slot of the thread it runs on.
__attribute__((noinline)) uint32_t plat_thread_id(void) {
    static __thread uint32_t tid;
    if (!tid) tid = (uint32_t)syscall(SYS_gettid);
    return tid;
}

// The first set variable of names ("ru_RU.UTF-8") as a BCP-47 tag ("ru-RU") into tag; "" for C and POSIX.
static void locale_tag(const char *const *names, int n, char *tag, size_t cap) {
    const char *v = NULL;
    for (int i = 0; i < n && !(v && *v); i++) v = getenv(names[i]);
    size_t k = 0;
    if (v && strcmp(v, "C") && strncmp(v, "C.", 2) && strcmp(v, "POSIX"))
        for (; v[k] && v[k] != '.' && v[k] != '@' && k < cap - 1; k++) tag[k] = v[k] == '_' ? '-' : v[k];
    tag[k] = 0;
}

const char *plat_ui_language(void) {
    static char tag[32];
    static const char *const names[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
    if (!tag[0]) locale_tag(names, 3, tag, sizeof tag);
    return tag;
}

const char *plat_user_locale(void) {
    static char tag[32];
    static const char *const names[] = {"LC_ALL", "LC_TIME", "LANG"};
    if (!tag[0]) locale_tag(names, 3, tag, sizeof tag);
    return tag;
}

// Noto CJK as the common distributions and SteamOS install it; one collection covers every CJK language.
int plat_font_path(const char *lang, char *out, size_t cap) {
    static const char *files[] = {
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    };
    if (strcmp(lang, "ja") && strcmp(lang, "ko") && strcmp(lang, "zh-Hans") && strcmp(lang, "zh-Hant")) return 0;
    for (size_t i = 0; i < sizeof files / sizeof *files; i++)
        if (plat_file_exists(files[i])) return snprintf(out, cap, "%s", files[i]) < (int)cap;
    return 0;
}

// PRETTY_NAME of /etc/os-release and the kernel release: "SteamOS (Linux 6.16.12-valve)".
int plat_os_version(char *out, size_t cap) {
    char name[128] = "Linux";
    FILE *f = fopen("/etc/os-release", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "PRETTY_NAME=", 12)) {
                char *v = line + 12, *e;
                if (*v == '"') v++;
                if ((e = strpbrk(v, "\"\n"))) *e = 0;
                snprintf(name, sizeof name, "%s", v);
                break;
            }
        fclose(f);
    }
    struct utsname u;
    if (uname(&u)) return snprintf(out, cap, "%s", name) < (int)cap;
    return snprintf(out, cap, "%s (Linux %s)", name, u.release) < (int)cap;
}

int plat_is_wine(void) { return 0; }

int plat_stderr_path(char *out, size_t cap) {
    struct stat st;
    if (fstat(2, &st) || !S_ISREG(st.st_mode)) return 0;
    ssize_t n = readlink("/proc/self/fd/2", out, cap - 1);
    if (n <= 0 || (size_t)n >= cap - 1) return 0;
    out[n] = 0;
    return 1;
}

int plat_stderr_write(const void *p, size_t n) {
    for (const char *c = p; n;) {
        ssize_t k = write(2, c, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        c += k, n -= (size_t)k;
    }
    return 1;
}

int plat_console_setup(void (*on_close)(const char *what)) {   // a Windows console's settings only
    (void)on_close;
    return 0;
}

// ---- process

#ifdef NX2NAVI_PGO_GEN
// compiler-rt's profile runtime (an instrumented build: --pgo gen of tools/make_game.py or tools/build_game.py). It
// writes its counters from an atexit handler, which _exit skips; __llvm_profile_dump writes them once and makes later
// writes no-ops.
int __llvm_profile_dump(void);
#endif

void (*plat_exit_hook)(void);

void plat_exit(int code) {
    if (plat_exit_hook) plat_exit_hook();
#ifdef NX2NAVI_PGO_GEN
    __llvm_profile_dump();
#endif
    fflush(stdout);
    fflush(stderr);
    _exit(code);
}

int plat_setenv(const char *name, const char *value) {
    return (value ? setenv(name, value, 1) : unsetenv(name)) == 0;
}

uint32_t plat_process_id(void) { return (uint32_t)getpid(); }

uint64_t plat_process_cpu_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

// Field 22 of /proc/self/stat is the start time in clock ticks after boot; CLOCK_BOOTTIME is now after boot.
uint64_t plat_process_uptime_ns(void) {
    FILE *f = fopen("/proc/self/stat", "r");
    if (!f) return 0;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *p = strrchr(buf, ')');   // the command name may hold spaces
    unsigned long long start = 0;
    if (!p || sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu",
                     &start) != 1)
        return 0;
    struct timespec t;
    clock_gettime(CLOCK_BOOTTIME, &t);
    uint64_t now = (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
    uint64_t at = start * (1000000000u / (uint64_t)sysconf(_SC_CLK_TCK));
    return now > at ? now - at : 0;
}

uint64_t plat_unix_time_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

void plat_local_time(struct tm *out) {
    time_t t = time(NULL);
    localtime_r(&t, out);
}

// The range of the Windows CRT's localtime_s and gmtime_s (1970 to 3000), which glibc exceeds: the game's time
// service reports times outside it as invalid on both hosts.
#define PLAT_TIME_MAX 32535215999ll

int plat_localtime(int64_t unix_s, struct tm *out) {
    time_t t = (time_t)unix_s;
    return unix_s >= 0 && unix_s <= PLAT_TIME_MAX && localtime_r(&t, out) != NULL;
}

int plat_gmtime(int64_t unix_s, struct tm *out) {
    time_t t = (time_t)unix_s;
    return unix_s >= 0 && unix_s <= PLAT_TIME_MAX && gmtime_r(&t, out) != NULL;
}

long plat_tz_west(void) {
    tzset();
    return timezone;
}

int plat_run_self(const char *env_name, const char *env_value, char *out, size_t cap, uint32_t timeout_ms) {
    // the arguments, NUL-separated, from /proc/self/cmdline
    static char args[16384];
    char *argv[256];
    int argc = 0;
    FILE *f = fopen("/proc/self/cmdline", "rb");
    size_t n = f ? fread(args, 1, sizeof args - 1, f) : 0;
    if (f) fclose(f);
    args[n] = 0;
    for (size_t i = 0; i < n && argc < 255; i += strlen(args + i) + 1) argv[argc++] = args + i;
    argv[argc] = NULL;
    char exe[PATH_MAX];
    if (!argc || !plat_exe_path(exe, sizeof exe)) return -1;
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawn_file_actions_adddup2(&fa, p[1], 2);
    setenv(env_name, env_value, 1);
    pid_t pid;
    int e = posix_spawn(&pid, exe, &fa, NULL, argv, environ);
    unsetenv(env_name);
    posix_spawn_file_actions_destroy(&fa);
    close(p[1]);
    if (e) {
        close(p[0]);
        return -1;
    }
    size_t len = 0;
    char sink[512];
    ssize_t k;
    while (len + 1 < cap && (k = read(p[0], out + len, cap - 1 - len)) != 0)
        if (k > 0) len += (size_t)k;
        else if (errno != EINTR) break;
    if (cap) out[len] = 0;
    while ((k = read(p[0], sink, sizeof sink)) > 0 || (k < 0 && errno == EINTR)) {}
    close(p[0]);
    int status = 0;
    uint64_t end = plat_now_ns() + (uint64_t)timeout_ms * 1000000u;
    while (waitpid(pid, &status, timeout_ms ? WNOHANG : 0) == 0) {
        if (plat_now_ns() >= end) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        plat_sleep_ms(10);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

int plat_random(void *out, size_t n) {
    uint8_t *p = out;
    while (n) {
        ssize_t k = getrandom(p, n, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        p += k, n -= (size_t)k;
    }
    return 1;
}

// ---- desktop

int plat_clipboard_set(const char *utf8) { return SDL_SetClipboardText(utf8); }

int plat_clipboard_get(char *out, size_t cap) {
    if (!cap) return 0;
    out[0] = 0;
    char *t = SDL_GetClipboardText();   // "" when there is none, never NULL but on failure
    if (!t) return 0;
    size_t n = strlen(t);
    if (n >= cap) {   // cut where a character starts, not inside one
        n = cap - 1;
        while (n && ((unsigned char)t[n] & 0xc0) == 0x80) n--;
    }
    memcpy(out, t, n);
    out[n] = 0;
    SDL_free(t);
    return n != 0;
}

static void reap_main(void *arg) {
    waitpid((pid_t)(intptr_t)arg, NULL, 0);
}

// xdg-open picks the desktop's file manager; it runs detached, so a slow answer does not hold up the caller, and a
// thread of its own reaps it so it does not stay a zombie.
int plat_open_folder(const char *path) {
    struct stat st;
    if (stat(path, &st) || !S_ISDIR(st.st_mode)) return 0;
    posix_spawnattr_t a;
    posix_spawnattr_init(&a);
    posix_spawnattr_setflags(&a, POSIX_SPAWN_SETSID);
    char *argv[] = {"xdg-open", (char *)path, NULL};
    pid_t pid;
    int e = posix_spawnp(&pid, "xdg-open", NULL, &a, argv, environ);
    posix_spawnattr_destroy(&a);
    if (e) return 0;
    plat_thread_spawn(reap_main, (void *)(intptr_t)pid, "reap", 64 << 10);
    return 1;
}

// ---- dynamic libraries

void *plat_lib_open(const char *name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
void *plat_lib_sym(void *lib, const char *name) { return lib ? dlsym(lib, name) : NULL; }

// ---- machine load: /proc/stat and /proc/self/stat for the CPU, hwmon for the temperature, the amdgpu sysfs files of
// the render device for the GPU.

struct PlatLoad {
    uint64_t busy, total, proc;   // the previous call's jiffies; total 0 before the first
    uint64_t proc_ns;
    char gpu[256];                // /sys/class/drm/cardN/device of the GPU in use; empty when unknown
    char temp[256];               // the CPU's hwmon temp1_input; empty when unknown
};

static int read_u64_file(const char *path, uint64_t *v) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned long long x;
    int ok = fscanf(f, "%llu", &x) == 1;
    fclose(f);
    if (ok) *v = x;
    return ok;
}

// The hwmon device of the CPU package: k10temp (AMD), coretemp (Intel), or the first that has a temp1_input.
static void find_cpu_temp(char *out, size_t cap) {
    out[0] = 0;
    DIR *d = opendir("/sys/class/hwmon");
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        char path[256], name[64] = "";
        snprintf(path, sizeof path, "/sys/class/hwmon/%s/name", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(name, sizeof name, f)) name[0] = 0;
        fclose(f);
        name[strcspn(name, "\n")] = 0;
        int cpu = !strcmp(name, "k10temp") || !strcmp(name, "coretemp") || !strcmp(name, "zenpower");
        snprintf(path, sizeof path, "/sys/class/hwmon/%s/temp1_input", e->d_name);
        if ((cpu || !out[0]) && plat_file_exists(path)) snprintf(out, cap, "%s", path);
        if (cpu && out[0]) break;
    }
    closedir(d);
}

// The DRM card with gpu_busy_percent (amdgpu) on PCI bus bus, the game's device. With bus -1 (no VK_EXT_pci_bus_info):
// of several, the one with the most VRAM in use. None when the game's GPU has no such file (NVIDIA): the overlay then
// takes its load and VRAM from NVML, not from an idle integrated GPU.
static void find_gpu(char *out, size_t cap, int bus) {
    out[0] = 0;
    uint64_t best = 0;
    DIR *d = opendir("/sys/class/drm");
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));) {
        if (strncmp(e->d_name, "card", 4) || strchr(e->d_name, '-')) continue;
        char dev[256], path[300];
        snprintf(dev, sizeof dev, "/sys/class/drm/%s/device", e->d_name);
        snprintf(path, sizeof path, "%s/gpu_busy_percent", dev);
        uint64_t v, used = 0;
        if (!read_u64_file(path, &v)) continue;
        if (bus >= 0) {   // device links to the PCI function, "0000:03:00.0": domain, bus, device, function
            char real[PATH_MAX];
            unsigned dom, b;
            const char *base = realpath(dev, real) ? strrchr(real, '/') : NULL;
            if (!base || sscanf(base + 1, "%x:%x:", &dom, &b) != 2 || (int)b != bus) continue;
        }
        snprintf(path, sizeof path, "%s/mem_info_vram_used", dev);
        read_u64_file(path, &used);
        if (!out[0] || used > best) snprintf(out, cap, "%s", dev), best = used;
    }
    closedir(d);
}

PlatLoad *plat_load_open(const uint8_t *gpu_luid, int pci_bus) {
    (void)gpu_luid;   // Linux has no adapter LUIDs; the PCI bus names the card
    PlatLoad *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    find_cpu_temp(l->temp, sizeof l->temp);
    find_gpu(l->gpu, sizeof l->gpu, pci_bus);
    return l;
}

void plat_load_close(PlatLoad *l) { free(l); }

void plat_cpu_load(PlatLoad *l, float *total_pct, float *proc_pct, float *temp_c) {
    *total_pct = *proc_pct = *temp_c = -1;
    if (!l) return;
    FILE *f = fopen("/proc/stat", "r");
    unsigned long long v[10] = {0};
    int n = f ? fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4],
                       &v[5], &v[6], &v[7], &v[8], &v[9])
              : 0;
    if (f) fclose(f);
    uint64_t pns = plat_process_cpu_ns();
    if (n >= 8) {
        uint64_t idle = v[3] + v[4], total = 0;
        for (int i = 0; i < 8; i++) total += v[i];   // guest time is already inside user time
        uint64_t busy = total - idle;
        if (l->total && total > l->total) {
            uint64_t dt = total - l->total;
            *total_pct = 100.0f * (float)(busy - l->busy) / (float)dt;
            // the process time is in ns, the machine's in jiffies summed over every CPU
            double dt_ns = (double)dt * 1e9 / (double)sysconf(_SC_CLK_TCK);
            *proc_pct = (float)(100.0 * (double)(pns - l->proc_ns) / dt_ns);
        }
        l->busy = busy, l->total = total, l->proc_ns = pns;
    }
    uint64_t mc;
    if (l->temp[0] && read_u64_file(l->temp, &mc)) *temp_c = (float)mc / 1000.0f;
}

void plat_gpu_load(PlatLoad *l, float *load_pct, float *vram_used_mb) {
    *load_pct = *vram_used_mb = -1;
    if (!l || !l->gpu[0]) return;
    char path[300];
    uint64_t v;
    snprintf(path, sizeof path, "%s/gpu_busy_percent", l->gpu);
    if (read_u64_file(path, &v)) *load_pct = (float)v;
    snprintf(path, sizeof path, "%s/mem_info_vram_used", l->gpu);
    if (read_u64_file(path, &v)) *vram_used_mb = (float)(v >> 20);
}

void plat_mem_info(float *used_mb, float *total_mb, float *proc_mb) {
    *used_mb = *total_mb = *proc_mb = -1;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[128];
        unsigned long long total = 0, avail = 0, k;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "MemTotal: %llu kB", &k) == 1) total = k;
            else if (sscanf(line, "MemAvailable: %llu kB", &k) == 1) avail = k;
        }
        fclose(f);
        if (total) *total_mb = (float)(total >> 10), *used_mb = (float)((total - avail) >> 10);
    }
    f = fopen("/proc/self/statm", "r");
    unsigned long long size, resident;
    if (f && fscanf(f, "%llu %llu", &size, &resident) == 2) *proc_mb = (float)(resident * (uint64_t)sysconf(_SC_PAGESIZE) >> 20);
    if (f) fclose(f);
}

void plat_process_memory(uint64_t *resident, uint64_t *private_bytes) {
    *resident = *private_bytes = 0;
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return;
    char line[128];
    unsigned long long k;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "VmRSS: %llu kB", &k) == 1) *resident = (uint64_t)k << 10;
        else if (sscanf(line, "RssAnon: %llu kB", &k) == 1) *private_bytes = (uint64_t)k << 10;
    }
    fclose(f);
}

// glibc's mallinfo2 (2.33 and later) sums every arena: arena is what the arenas took with brk and mmap, hblkhd the
// blocks mmapped one by one, uordblks what the arenas handed out. Other C libraries leave the figures unknown.
void plat_heap_info(PlatHeapInfo *h) {
    memset(h, 0, sizeof *h);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    struct mallinfo2 m = mallinfo2();
    h->committed = (uint64_t)m.arena + m.hblkhd;
    h->used = (uint64_t)m.uordblks + m.hblkhd;
#endif
}

// ---- memory limits
//
// Linux overcommits, so memory that runs short does not fail an allocation: the OOM killer ends the process, when it
// outgrew its cgroup's memory.max or when the machine's RAM is gone, and Committed_AS against CommitLimit (which binds
// only with vm.overcommit_memory = 2) says neither. plat_commit_info reports the limit with the least headroom of
//   - each cgroup v2 level from this process's cgroup up to the root that sets memory.max or memory.high (the smaller;
//     the kernel throttles at memory.high), against that level's memory.current less its file cache (memory.stat file
//     without shmem): the kernel drops the cache before it ends anything, so a cache that filled the limit is no
//     shortage;
//   - the machine's RAM: MemTotal against MemTotal less MemAvailable;
//   - with vm.overcommit_memory = 2 only, where an allocation does fail: CommitLimit against Committed_AS.
// The process's share is its resident anonymous memory, the nearest to Windows' private bytes. The sweeps that ask
// run several times a second, so a read is reused for 250 ms. The first read keeps every limit it found as text, which
// the runtime logs (plat_commit_note): stderr alone does not reach the log file when the launcher starts the game.

static char cg_leaf[PATH_MAX];   // this process's cgroup v2 directory ("" when it has none)
static size_t cg_floor;          // the length of the cgroup2 mount point at the start of cg_leaf

// The process's cgroup v2 directory: its path from /proc/self/cgroup below the root of the cgroup2 mount that
// /proc/self/mountinfo shows (a container's own cgroup is mounted as the root), else the nearest directory above it
// that exists.
static void cg_locate(void) {
    char path[PATH_MAX] = "", mnt[PATH_MAX] = "", root[PATH_MAX] = "", line[2 * PATH_MAX];
    FILE *f = fopen("/proc/self/cgroup", "r");
    if (f) {
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "0::", 3)) {
                line[strcspn(line, "\n")] = 0;
                snprintf(path, sizeof path, "%s", line + 3);
            }
        fclose(f);
    }
    f = path[0] ? fopen("/proc/self/mountinfo", "r") : NULL;
    while (f && fgets(line, sizeof line, f)) {
        char r[PATH_MAX], m[PATH_MAX];
        if (!strstr(line, " - cgroup2 ") || sscanf(line, "%*d %*d %*s %4095s %4095s", r, m) != 2) continue;
        snprintf(root, sizeof root, "%s", r);
        snprintf(mnt, sizeof mnt, "%s", m);
        break;
    }
    if (f) fclose(f);
    if (!mnt[0]) return;
    size_t rl = strlen(root);
    const char *rel = path;
    // A mount of a cgroup below the root of the hierarchy shows the process's path without that prefix.
    if (rl > 1 && !strncmp(path, root, rl) && (!path[rl] || path[rl] == '/')) rel = path + rl;
    else if (rl > 1) rel = "";
    if (!strcmp(rel, "/")) rel = "";
    snprintf(cg_leaf, sizeof cg_leaf, "%s%s", mnt, rel);
    cg_floor = strlen(mnt);
    struct stat st;
    while (stat(cg_leaf, &st) || !S_ISDIR(st.st_mode)) {   // not shown by the mount: the nearest directory above
        char *s = strrchr(cg_leaf, '/');
        if (!s || (size_t)(s - cg_leaf) < cg_floor) {
            cg_leaf[0] = 0;
            return;
        }
        *s = 0;
    }
}

// A cgroup file into buf as text; 0 when it is missing (the controller is not enabled there).
static int cg_file(const char *dir, const char *name, char *buf, size_t n) {
    char p[PATH_MAX + 32];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r <= 0) return 0;
    buf[r] = 0;
    return 1;
}

// A byte limit file: its value in bytes, -1 for "max" or when missing.
static double cg_bytes(const char *dir, const char *name) {
    char b[64];
    if (!cg_file(dir, name, b, sizeof b) || !strncmp(b, "max", 3)) return -1;
    return (double)strtoull(b, NULL, 10);
}

// A memory.stat counter in bytes, 0 when it is missing.
static double cg_stat(const char *stat, const char *key) {
    size_t kl = strlen(key);
    for (const char *p = stat; p; p = strchr(p, '\n')) {
        if (*p == '\n') p++;
        if (!strncmp(p, key, kl) && p[kl] == ' ') return (double)strtoull(p + kl + 1, NULL, 10);
    }
    return 0;
}

typedef struct {
    double proc, used, limit;   // MB; -1 when unknown
    const char *who;            // the limit that is the tightest, for the first read's log line
    uint64_t at;                // plat_now_ms of the read, 0 before the first
} CommitSnap;

// Makes (limit, used) the snapshot's when its headroom is smaller than the one held.
static void commit_pick(CommitSnap *s, double limit, double used, const char *who) {
    if (s->limit < 0 || limit - used < s->limit - s->used) s->limit = limit, s->used = used, s->who = who;
}

// Appends one limit found to the note of the first read ("; " between them; a full note takes no more).
static void note_add(char *note, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define NOTE_MAX 2048
static void note_add(char *note, const char *fmt, ...) {
    size_t n = strlen(note);
    if (n + 3 >= NOTE_MAX) return;
    if (n) memcpy(note + n, "; ", 3), n += 2;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(note + n, NOTE_MAX - n, fmt, ap);
    va_end(ap);
}

// The cgroups from the process's up to the root that limit memory.
static void commit_cgroups(CommitSnap *s, char *note) {
    char d[PATH_MAX];
    snprintf(d, sizeof d, "%s", cg_leaf);
    while (d[0]) {
        double max = cg_bytes(d, "memory.max"), high = cg_bytes(d, "memory.high");
        int use_high = max < 0 || (high >= 0 && high < max);
        double lim = use_high ? high : max;
        double cur = lim < 0 ? -1 : cg_bytes(d, "memory.current");
        if (cur >= 0) {
            char st[4096] = "";   // memory.stat starts with anon and file; the counters needed lie in its first lines
            cg_file(d, "memory.stat", st, sizeof st);
            double cache = cg_stat(st, "file") - cg_stat(st, "shmem");
            cache = cache < 0 ? 0 : cache > cur ? cur : cache;
            commit_pick(s, lim / 1048576.0, (cur - cache) / 1048576.0,
                        use_high ? "cgroup memory.high" : "cgroup memory.max");
            if (note)
                note_add(note, "cgroup %s %s %.0f MB (%.0f MB in use, %.0f MB of it file cache)",
                         d[cg_floor] ? d + cg_floor : "/", use_high ? "memory.high" : "memory.max", lim / 1048576.0,
                         cur / 1048576.0, cache / 1048576.0);
        }
        char *sl = strrchr(d, '/');
        if (strlen(d) <= cg_floor || !sl || (size_t)(sl - d) < cg_floor) break;
        *sl = 0;
    }
}

// The machine's RAM, and the system's commit when vm.overcommit_memory = 2.
static void commit_machine(CommitSnap *s, char *note) {
    static int overcommit = -1;
    unsigned long long total = 0, avail = 0, cl = 0, as = 0, k;
    char line[128];
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "MemTotal: %llu kB", &k) == 1) total = k;
            else if (sscanf(line, "MemAvailable: %llu kB", &k) == 1) avail = k;
            else if (sscanf(line, "CommitLimit: %llu kB", &k) == 1) cl = k;
            else if (sscanf(line, "Committed_AS: %llu kB", &k) == 1) as = k;
        }
        fclose(f);
    }
    if (total && avail && avail <= total) {
        commit_pick(s, (double)total / 1024.0, (double)(total - avail) / 1024.0, "RAM");
        if (note) note_add(note, "RAM %.0f MB (%.0f MB available)", (double)total / 1024.0, (double)avail / 1024.0);
    }
    if (overcommit < 0) {
        FILE *o = fopen("/proc/sys/vm/overcommit_memory", "r");
        if (!o || fscanf(o, "%d", &overcommit) != 1) overcommit = 0;
        if (o) fclose(o);
    }
    if (overcommit == 2 && cl) {
        commit_pick(s, (double)cl / 1024.0, (double)as / 1024.0, "commit limit");
        if (note)
            note_add(note, "commit limit %.0f MB (%.0f MB committed, vm.overcommit_memory = 2)", (double)cl / 1024.0,
                     (double)as / 1024.0);
    }
}

static char commit_note[NOTE_MAX + 128];   // the first read's limits (plat_commit_note); not written after it

static void commit_read(CommitSnap *s, int first) {
    char note[NOTE_MAX] = "";
    s->proc = s->used = s->limit = -1;
    s->who = NULL;
    if (first) cg_locate();
    commit_cgroups(s, first ? note : NULL);
    commit_machine(s, first ? note : NULL);
    uint64_t resident, private_bytes;
    plat_process_memory(&resident, &private_bytes);
    if (private_bytes) s->proc = (double)(private_bytes >> 20);
    if (first) {
        if (note[0])
            snprintf(commit_note, sizeof commit_note,
                     "memory limits: %s; the one with the least headroom counts (now: %s)", note, s->who);
        else snprintf(commit_note, sizeof commit_note, "no memory limit readable");
    }
}

void plat_commit_info(double *proc_mb, double *used_mb, double *limit_mb) {
    static PlatMutex lock = PLAT_MUTEX_INIT;
    static CommitSnap snap;
    plat_mutex_lock(&lock);
    uint64_t now = plat_now_ms();
    if (!snap.at || now - snap.at >= 250) {
        commit_read(&snap, !snap.at);
        snap.at = now ? now : 1;
    }
    *proc_mb = snap.proc, *used_mb = snap.used, *limit_mb = snap.limit;
    plat_mutex_unlock(&lock);
}

int plat_commit_enforced(void) {
    double proc, used, limit;
    plat_commit_info(&proc, &used, &limit);
    return limit > 0;
}

// The first read wrote the note under plat_commit_info's lock; taking that lock here orders the copy after it.
int plat_commit_note(char *out, size_t cap) {
    double proc, used, limit;
    plat_commit_info(&proc, &used, &limit);
    snprintf(out, cap, "%s", commit_note);
    return out[0] != 0;
}

// The cgroup's memory.current and MemAvailable count system memory only: a graphics card's own memory is in neither.
int plat_commit_counts_vram(void) { return 0; }

// Swap is not a page file Windows sizes, and nothing here advises on its size.
int plat_page_file_managed(void) { return -1; }

// ---- child processes

struct PlatChild {
    pid_t pid;
    int in, out;   // in: our write end, out: our read end
};

PlatChild *plat_child_start(const char *exe, const char *arg, uint32_t *err) {
    if (err) *err = 0;
    int in[2], out[2];
    if (pipe2(in, O_CLOEXEC)) return NULL;
    if (pipe2(out, O_CLOEXEC)) {
        close(in[0]), close(in[1]);
        return NULL;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    char *argv[] = {(char *)exe, (char *)arg, NULL};
    pid_t pid;
    int e = posix_spawn(&pid, exe, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]);
    close(out[1]);
    if (e) {
        close(in[1]), close(out[0]);
        if (err) *err = (uint32_t)e;
        return NULL;
    }
    PlatChild *c = malloc(sizeof *c);
    if (!c) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(in[1]), close(out[0]);
        return NULL;
    }
    c->pid = pid, c->in = in[1], c->out = out[0];
    return c;
}

void plat_child_stop(PlatChild *c) {
    if (!c) return;
    close(c->in);
    close(c->out);
    kill(c->pid, SIGKILL);
    waitpid(c->pid, NULL, 0);
    free(c);
}

void plat_child_lower(PlatChild *c) {   // a higher nice value needs no privilege
    if (c) setpriority(PRIO_PROCESS, (id_t)c->pid, 19);
}

// A child that died makes the write fail with EPIPE; SIGPIPE is ignored for the process (main.c), as Windows has none.
int plat_child_write(PlatChild *c, const void *data, size_t n) { return write_all(c->in, data, n); }

// See win32.c plat_child_read: a child that never answers fails the read after PLAT_CHILD_READ_MS.
long plat_child_read(PlatChild *c, void *buf, size_t cap) {
    struct pollfd pf = {.fd = c->out, .events = POLLIN};
    int r;
    do r = poll(&pf, 1, PLAT_CHILD_READ_MS);
    while (r < 0 && errno == EINTR);
    if (r <= 0) return -1;   // timed out or failed
    ssize_t k;
    do k = read(c->out, buf, cap);
    while (k < 0 && errno == EINTR);
    return (long)k;
}

// ---- local IPC (platform.h): Discord's Rich Presence Unix domain socket
struct PlatIpc { int fd; };

PlatIpc *plat_ipc_connect(const char *name) {
    const char *dirs[] = {getenv("DISCORD_IPC_PATH"), getenv("XDG_RUNTIME_DIR"), getenv("TMPDIR"), "/tmp"};
    for (size_t d = 0; d < sizeof dirs / sizeof *dirs; d++) {
        if (!dirs[d] || !*dirs[d]) continue;
        char path[sizeof ((struct sockaddr_un *)0)->sun_path];
        if ((size_t)snprintf(path, sizeof path, "%s/%s", dirs[d], name) >= sizeof path) continue;
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return NULL;
        struct sockaddr_un sa = {0};
        sa.sun_family = AF_UNIX;
        strcpy(sa.sun_path, path);
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
            PlatIpc *p = malloc(sizeof *p);
            if (!p) {
                close(fd);
                return NULL;
            }
            p->fd = fd;
            return p;
        }
        close(fd);   // nothing listens at this path: the next directory may have it
    }
    return NULL;
}

int plat_ipc_read(PlatIpc *p, void *buf, size_t n, uint32_t ms) {
    struct pollfd pf = {.fd = p->fd, .events = POLLIN};
    int r = poll(&pf, 1, (int)ms);
    if (r <= 0) return r < 0 && errno != EINTR ? -1 : 0;
    ssize_t got = recv(p->fd, buf, n, 0);
    return got > 0 ? (int)got : -1;   // 0 bytes: the peer closed the socket
}

int plat_ipc_write(PlatIpc *p, const void *buf, size_t n) {
    const uint8_t *at = buf;
    while (n) {
        ssize_t put = send(p->fd, at, n, MSG_NOSIGNAL);   // a closed peer is an error, not a SIG_PIPE
        if (put <= 0) {
            if (put < 0 && errno == EINTR) continue;
            return 0;
        }
        at += put, n -= put;
    }
    return 1;
}

void plat_ipc_close(PlatIpc *p) {
    if (!p) return;
    close(p->fd);
    free(p);
}

// ---- UDP sockets (platform.h)
struct PlatUdp { int fd, v6; };

static void addr_from_sockaddr(PlatAddr *out, const struct sockaddr_storage *ss) {
    uint8_t ip[16] = {0};
    if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)ss;
        memcpy(ip, &a->sin6_addr, 16);
        plat_addr_set(out, ip, ntohs(a->sin6_port), a->sin6_scope_id);
    } else {
        const struct sockaddr_in *a = (const struct sockaddr_in *)ss;
        ip[10] = ip[11] = 0xff;
        memcpy(ip + 12, &a->sin_addr, 4);
        plat_addr_set(out, ip, ntohs(a->sin_port), 0);
    }
}

// The socket address of a for a socket of family v6 (1: AF_INET6, 0: AF_INET); 0 when an IPv4 socket cannot reach it.
static int addr_to_sockaddr(const PlatAddr *a, int v6, struct sockaddr_storage *ss, socklen_t *len) {
    uint8_t ip[16];
    uint32_t scope;
    plat_addr_get(a, ip, &scope);
    memset(ss, 0, sizeof *ss);
    if (v6) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)ss;
        s->sin6_family = AF_INET6;
        s->sin6_port = htons(plat_addr_port(a));
        s->sin6_scope_id = scope;
        memcpy(&s->sin6_addr, ip, 16);
        *len = sizeof *s;
        return 1;
    }
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (memcmp(ip, mapped, sizeof mapped)) return 0;
    struct sockaddr_in *s = (struct sockaddr_in *)ss;
    s->sin_family = AF_INET;
    s->sin_port = htons(plat_addr_port(a));
    memcpy(&s->sin_addr, ip + 12, 4);
    *len = sizeof *s;
    return 1;
}

int plat_net_resolve(const char *host, uint16_t port, PlatAddr *out) {
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM}, *res = NULL, *pick = NULL;
    if (!host || !*host || getaddrinfo(host, NULL, &hints, &res) != 0) return 0;
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        if (r->ai_addrlen > sizeof(struct sockaddr_storage) || (r->ai_family != AF_INET && r->ai_family != AF_INET6)) continue;
        if (!pick || (r->ai_family == AF_INET && pick->ai_family != AF_INET)) pick = r;   // IPv4 first
    }
    int ok = pick != NULL;
    if (ok) {
        struct sockaddr_storage ss;
        memset(&ss, 0, sizeof ss);
        memcpy(&ss, pick->ai_addr, pick->ai_addrlen);
        addr_from_sockaddr(out, &ss);
        uint8_t ip[16];
        uint32_t scope;
        plat_addr_get(out, ip, &scope);
        plat_addr_set(out, ip, port, scope);
    }
    freeaddrinfo(res);
    return ok;
}

PlatUdp *plat_udp_open(uint16_t port, int dual_stack) {
    int v6 = dual_stack;
    int fd = v6 ? socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0) : -1;
    if (fd >= 0) {
        int off = 0;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
        struct sockaddr_in6 a = {.sin6_family = AF_INET6, .sin6_addr = in6addr_any, .sin6_port = htons(port)};
        if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) close(fd), fd = -1;
    }
    if (fd < 0) {   // IPv4 only, asked for or all the machine has
        v6 = 0;
        fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) return NULL;
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
            close(fd);
            return NULL;
        }
    }
    PlatUdp *u = malloc(sizeof *u);
    if (!u) {
        close(fd);
        return NULL;
    }
    u->fd = fd, u->v6 = v6;
    return u;
}

uint16_t plat_udp_port(const PlatUdp *u) {
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    if (!u || getsockname(u->fd, (struct sockaddr *)&ss, &len) != 0) return 0;
    PlatAddr a;
    addr_from_sockaddr(&a, &ss);
    return plat_addr_port(&a);
}

int plat_udp_send(PlatUdp *u, const PlatAddr *to, const void *p, size_t n) {
    struct sockaddr_storage ss;
    socklen_t len;
    if (!u || !addr_to_sockaddr(to, u->v6, &ss, &len)) return 0;
    for (;;) {
        ssize_t k = sendto(u->fd, p, n, MSG_NOSIGNAL, (struct sockaddr *)&ss, len);
        if (k >= 0) return 1;
        if (errno == EINTR) continue;
        // a full buffer, a missing route or an ICMP error is a lost datagram; a datagram too long or a socket that is not
        // ours to use can never be sent
        return errno != EMSGSIZE && errno != EBADF && errno != EINVAL && errno != EAFNOSUPPORT && errno != ENOTSOCK;
    }
}

int plat_udp_recv(PlatUdp *u, void *buf, size_t cap, PlatAddr *from, uint32_t ms) {
    if (!u) return -1;
    struct pollfd pf = {.fd = u->fd, .events = POLLIN};
    int r = poll(&pf, 1, ms > (uint32_t)INT32_MAX ? INT32_MAX : (int)ms);
    if (r < 0) return errno == EINTR ? 0 : -1;
    if (r == 0) return 0;
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    ssize_t k = recvfrom(u->fd, buf, cap, MSG_DONTWAIT, (struct sockaddr *)&ss, &len);
    if (k < 0) return errno == EAGAIN || errno == EINTR || errno == ECONNREFUSED || errno == EHOSTUNREACH ? 0 : -1;
    if (from) addr_from_sockaddr(from, &ss);
    return (int)k;
}

void plat_udp_close(PlatUdp *u) {
    if (!u) return;
    close(u->fd);
    free(u);
}


// ---- restart
// NX2NAVI_RESTART_WAIT names the process that called plat_restart, as its id and start time (field 22 of /proc/<pid>/stat,
// clock ticks after boot): a process that has only reused the id is not waited for. Waiting keeps the new process off
// the old one's files (the log it moves at start, the saves) until the old one is gone. The new process gets no
// descriptor past stderr (glibc 2.34's closefrom file action), and the default signal mask and dispositions, whatever
// the calling thread had.
#define RESTART_WAIT "NX2NAVI_RESTART_WAIT"

// The start time of process pid (field 22 of its /proc stat line), 0 when it is gone.
static unsigned long long process_start(int pid) {
    char path[64], buf[1024];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *p = strrchr(buf, ')');   // the command name may hold spaces
    unsigned long long start = 0;
    if (!p || sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu",
                     &start) != 1)
        return 0;
    return start;
}

// glibc 2.34 added posix_spawn_file_actions_addclosefrom_np, and the release links against the Steam Runtime's 2.31: a
// weak reference, used when the running glibc has it. Without it the child inherits the open descriptors.
extern int posix_spawn_file_actions_addclosefrom_np(posix_spawn_file_actions_t *fa, int from) __attribute__((weak));

static void spawn_close_from(posix_spawn_file_actions_t *fa, int from) {
    if (posix_spawn_file_actions_addclosefrom_np) posix_spawn_file_actions_addclosefrom_np(fa, from);
}

// The calling thread's mask with every processor this process uses, for the time of a posix_spawn: a child starts
// with the mask of the thread that spawned it, and after plat_cpu_reserve no thread has them all (the recording thread
// has its core alone, every other thread lacks that core). The game plat_restart starts would begin without the core,
// take the next fast one for its own recording thread, and so have one core fewer after every Restart.
// 1 when the mask was widened: *saved is then the mask to put back.
static int spawn_mask_whole(cpu_set_t *saved) {
    cpu_set_t whole;
    plat_mutex_lock(&cpu_lock);
    int taken = cpu_taker != 0;
    CPU_OR(&whole, &cpu_taken, &cpu_others);
    plat_mutex_unlock(&cpu_lock);
    if (!taken || sched_getaffinity(0, sizeof *saved, saved)) return 0;
    CPU_OR(&whole, &whole, saved);
    return !CPU_EQUAL(&whole, saved) && !sched_setaffinity(0, sizeof whole, &whole);
}

int plat_restart(void) {
    // the arguments, NUL-separated, from /proc/self/cmdline
    static char args[16384];
    char *argv[256];
    int argc = 0;
    FILE *f = fopen("/proc/self/cmdline", "rb");
    size_t n = f ? fread(args, 1, sizeof args - 1, f) : 0;
    if (f) fclose(f);
    args[n] = 0;
    for (size_t i = 0; i < n && argc < 255; i += strlen(args + i) + 1) argv[argc++] = args + i;
    argv[argc] = NULL;
    char exe[PATH_MAX], wait[64];
    if (!argc || !plat_exe_path(exe, sizeof exe)) return 0;
    snprintf(wait, sizeof wait, "%d.%llu", (int)getpid(), process_start((int)getpid()));
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    spawn_close_from(&fa, 3);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    setenv(RESTART_WAIT, wait, 1);
    cpu_set_t mask;
    int whole = spawn_mask_whole(&mask);
    pid_t pid;
    int e = posix_spawn(&pid, exe, &fa, &at, argv, environ);
    if (whole) sched_setaffinity(0, sizeof mask, &mask);
    unsetenv(RESTART_WAIT);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return e == 0;
}

int plat_spawn(const char *const *argv, const char *cwd) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    spawn_close_from(&fa, 3);
    posix_spawn_file_actions_addchdir_np(&fa, cwd);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSID);
    pid_t pid;
    int e = posix_spawn(&pid, argv[0], &fa, &at, (char *const *)argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return e == 0;
}

int plat_restart_wait(uint32_t timeout_ms) {
    const char *v = getenv(RESTART_WAIT);
    if (!v) return 0;
    int pid = 0;
    unsigned long long start = 0;
    int ok = sscanf(v, "%d.%llu", &pid, &start) == 2 && pid > 0 && start;
    unsetenv(RESTART_WAIT);   // the processes this one starts do not wait again
    if (!ok) return 0;
    // The descriptor is taken first: a start time that still matches then proves it is that process, not a new one with
    // the id. It reads as ready once the process has exited, reaped or not.
    int fd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (fd < 0) return 0;   // already gone
    int r = 0;
    if (process_start(pid) == start) {
        struct pollfd p = {fd, POLLIN, 0};
        do r = poll(&p, 1, (int)timeout_ms);
        while (r < 0 && errno == EINTR);
        r = r > 0 ? 1 : -1;
    }
    close(fd);
    return r;
}
