// nx2navi.exe entry: reserves the guest address space, parses the command line and runs one mode.
//
//   nx2navi.exe [--exefs DIR] [--romfs DIR] [--profile DIR] [--audio-device VALUE] [--monitor VALUE] <mode>
//     --audio-device VALUE   play the sound on one output device: sets NX2NAVI_AUDIO_DEVICE before the settings are read, so
//                       it wins over nx2navi.ini's `audio_device =`. VALUE is default (the system default device),
//                       a 1-based index as --list-audio-devices prints it (devices sorted by name), or part of a device name, case-insensitive
//     --list-audio-devices   print each playback device's index, name, channels and frequency, mark the system default and
//                       show which device NX2NAVI_AUDIO_DEVICE picks; needs no game files
//     --monitor VALUE   open the game window on one monitor: sets NX2NAVI_MONITOR before the settings are read, so it wins
//                       over nx2navi.ini's `monitor =`. VALUE is auto (default), primary, a 1-based index as --list-monitors
//                       prints it, or part of a monitor's name (on Windows also of its GDI device name, DISPLAY3),
//                       case-insensitive. Wayland places a windowed window itself: only fullscreen opens on the monitor
//     --list-monitors   print each monitor's index, name (on Windows also its GDI device name), desktop rectangle,
//                       primary flag and DPI; needs no game files
//     --selftest        layout, dispatch table and host-function registry; needs no game files
//     --probe-mods      the mods folder scan, order and switches (mods.c); needs no game files
//     --probe-loader    load and relocate the modules, check them against the measured 1.2.1 values
//     --probe-kernel, --probe-ipc, --probe-services, --probe-fs    host probes
//     --probe-discord  the Rich Presence IPC exchange; skips without a Discord client; no game files
//     --probe-dpi       the process DPI awareness (per-monitor v2) and the primary monitor's size; no game files
//     --probe-gfx-abi   NVN object layouts against nvn.h's reserved sizes; needs no game files
//     --probe-nvn, --probe-nvn-tex, --probe-window, --probe-shader, --probe-vkmem, --probe-vkexec    graphics probes
//     --probe-prewarm   the boot's pipeline stages from the caches next to the executable, the memory each leaves and
//                       what a compute pipeline's first dispatch costs; skips without recorded shaders, runs no game
//     --probe-ffx-cache one FSR 3.1 upscaling context and where its pipelines came from (the pipeline caches kept
//                       across runs); skips without amd_fidelityfx_vk.dll, runs no game
//     --probe-astc      the compute ASTC decoder against astc_decode_ldr; needs no game files
//     --probe-meshfb    the mesh shader fallback: mesh programs drawn through VK_EXT_mesh_shader and through compute
//                       passes, the pictures compared; needs no game files (mesh_fb_probe.c)
//     --probe-fsr3      FSR 3.1 upscaling with the built-in passes (vk_fsr3.c) on a synthetic scene; no game files
//     --probe-zstd      host zstd streams (ov_zstd.c) on frames made here; needs no game files
//     --probe-input     gamepad polls on a virtual gamepad, also while SDL's update takes long; needs no game files
//     --probe-movie     the host movie decoders on one RomFS movie (VP9 movie::Decoder, AV1 movie::VideoDecoder); skips without game data
//     --boot            run the game to the first NVN call
//     --seconds S       run the game (implies --boot's start-up) until the world clock (game time since the loaded
//                       save's first frame) passed S seconds, then exit 0 at that present
//     --shot PNG        with --seconds: save that frame; env NX2NAVI_GPU=null|vulkan selects the back end
//     (no mode)         run the game
//
// Also here: the runtime services the generated code calls besides rt_dispatch and rt_svc (rt_brk, rt_udf,
// rt_ticks, the function profiler hook), logging, SHA-256, and the crash reporter (a vectored exception handler that
// names the lifted function, the guest registers and the guest frame-pointer chain).
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "logq.h"
#include "prof.h"
#include "platform/platform.h"
#include "sha256.h"

#define MAX_MODULES 16

// ---- logging, tracing, hashing
unsigned rt_trace;

// Log file. A game run whose stderr is not a file already (started from Explorer, a shortcut or a console) also
// writes its log to logs\nx2navi.log next to nx2navi.exe, so a player has one to attach to a report; run_game.py and
// `2> file` keep their own file. Each start moves the earlier logs to nx2navi.1.log .. nx2navi.4.log. A file stops at 64 MiB
// (NX2NAVI_TRACE can write that much). Lines from before log_open wait in a static buffer: mem_init runs first so that
// nothing else lands in the guest address space, and an earlier open would put the CRT's heap there.
#define LOG_KEEP 5
#define LOG_CAP (64ull << 20)
enum { LOG_EARLY, LOG_FILE, LOG_OFF };
static int g_log_state;
static PlatAppend *g_log_file;
static char g_log_path[PLAT_PATH_MAX * 3];
static char g_log_early[32768];
static size_t g_log_early_n;
static uint64_t g_log_bytes;

// stderr's writes (log_sink). g_log_final: rt_log_sync has run, so the process is ending: an exit, a fatal error or a
// crash report.
static PlatMutex g_err_lock = PLAT_MUTEX_INIT;
static volatile int32_t g_log_final;

// The log's destinations: the file first, then stderr. logq.c calls it: from the log thread in a game run, else from
// the thread that logs. A write to stderr can block for as long as its reader likes: a console window holds every write
// back while a selection is made in it, and the log thread then stays in that write. The file has the lines by then, and
// the lines of the process's end (rt_log_sync) skip stderr while another thread is stuck in a write to it, so the exit
// and FATAL lines still reach the file. stderr goes through plat_stderr_write, which holds no lock of the C runtime's
// stderr stream, so fflush(stderr) and plat_exit's fflush(NULL) do not wait for a stuck write either.
static void log_sink(const char *p, size_t n) {
    int file = __atomic_load_n(&g_log_state, __ATOMIC_ACQUIRE) == LOG_FILE;
    if (file) {
        uint64_t at = __atomic_fetch_add(&g_log_bytes, n, __ATOMIC_RELAXED);
        if (at + n <= LOG_CAP) {
            plat_append_write(g_log_file, p, n);
        } else if (at <= LOG_CAP) {   // the first write past the cap says so, once
            static const char cut[] = "[host] log: the file reached 64 MiB, the rest of this run goes to stderr only\n";
            plat_append_write(g_log_file, cut, sizeof cut - 1);
        }
    }
    // without the file stderr is the log (run_game.py, `2> file`), and it is waited for
    if (file && __atomic_load_n(&g_log_final, __ATOMIC_ACQUIRE)) {
        if (!plat_mutex_trylock(&g_err_lock)) return;
    } else {
        plat_mutex_lock(&g_err_lock);
    }
    plat_stderr_write(p, n);
    plat_mutex_unlock(&g_err_lock);
}

void rt_log_sync(void) {
    __atomic_store_n(&g_log_final, 1, __ATOMIC_RELEASE);
    logq_sync();
}

// The console window was closed, or Ctrl+C or Ctrl+Break pressed in it (platform.h plat_console_setup): the end of a
// closed game window, which writes the settings, the caches and the saves and logs the exit (gfx_present.c
// gfx_window_closed). Left to Windows, a closed console ended the process with nothing saved.
static void console_closed(const char *what) {
    rt_log("console: %s; the game ends as when its window is closed", what);
    gfx_window_closed();
}

void rt_log_raw(const char *p, size_t n) {
    if (__atomic_load_n(&g_log_state, __ATOMIC_ACQUIRE) == LOG_EARLY) {
        fwrite(p, 1, n, stderr);
        size_t at = __atomic_fetch_add(&g_log_early_n, n, __ATOMIC_RELAXED);
        if (at + n <= sizeof g_log_early) memcpy(g_log_early + at, p, n);
        return;
    }
    logq_write(p, n);
}

void rt_vlogf(const char *fmt, va_list ap) {
    char buf[2048];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > 0) rt_log_raw(buf, n < (int)sizeof buf ? (size_t)n : sizeof buf - 1);
}

void rt_logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rt_vlogf(fmt, ap);
    va_end(ap);
}

void rt_log(const char *fmt, ...) {
    char buf[2048];
    int n = snprintf(buf, sizeof buf, "[host] ");
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap);
    va_end(ap);
    n = k < 0 ? n : n + k < (int)sizeof buf - 1 ? n + k : (int)sizeof buf - 2;
    buf[n++] = '\n';
    rt_log_raw(buf, n);
}

const char *rt_log_path(void) {
    return __atomic_load_n(&g_log_state, __ATOMIC_ACQUIRE) == LOG_FILE ? g_log_path : NULL;
}

// Opens the log file for a game run (game = 1) and writes the buffered lines to it; anything else drops them. Runs on
// the main thread before the boot starts other threads.
static void log_open(int game) {
    logq_init(log_sink);
    char dir[PLAT_PATH_MAX * 3], from[PLAT_PATH_MAX * 3 + 16], to[PLAT_PATH_MAX * 3 + 16];
    char *cut = NULL;
    if (game && !plat_stderr_path(dir, sizeof dir) && plat_exe_path(dir, sizeof dir)) {
        cut = plat_last_sep(dir);
        char *f = strrchr(dir, '/');
        if (f > cut) cut = f;
    }
    if (cut && (size_t)(cut - dir) + sizeof "/logs" <= sizeof dir) {
        strcpy(cut, "/logs");
        plat_file_mkdir_p(dir);
        for (int i = LOG_KEEP - 1; i > 0; i--) {   // nx2navi.3.log -> nx2navi.4.log, ..., nx2navi.log -> nx2navi.1.log
            if (i > 1) snprintf(from, sizeof from, "%s/nx2navi.%d.log", dir, i - 1);
            else snprintf(from, sizeof from, "%s/nx2navi.log", dir);
            snprintf(to, sizeof to, "%s/nx2navi.%d.log", dir, i);
            if (plat_file_exists(from)) plat_file_replace(from, to, 1);
        }
        snprintf(g_log_path, sizeof g_log_path, "%s/nx2navi.log", dir);
        g_log_file = plat_append_open(g_log_path);
    }
    if (!g_log_file) {
        __atomic_store_n(&g_log_state, LOG_OFF, __ATOMIC_RELEASE);
    } else {
        size_t n = __atomic_load_n(&g_log_early_n, __ATOMIC_RELAXED);
        if (n > sizeof g_log_early) n = sizeof g_log_early;
        plat_append_write(g_log_file, g_log_early, n);
        g_log_bytes = n;
        __atomic_store_n(&g_log_state, LOG_FILE, __ATOMIC_RELEASE);
        rt_log("log: %s", g_log_path);
    }
    // A game run writes its log from the log thread (logq.c), whether the lines go to a log file or to stderr alone.
    const char *e = cfg_get("NX2NAVI_LOG_ASYNC");
    if (!game || (e && !strcmp(e, "0"))) return;
    plat_exit_hook = rt_log_sync;
    if (logq_async()) rt_log("log: lines are written by a thread of their own (NX2NAVI_LOG_ASYNC)");
    else rt_log("log: no log thread; every line is written by the thread that logs it");
}

static void parse_trace(void) {
    const char *e = cfg_get("NX2NAVI_TRACE");   // no default: nothing is traced when unset
    if (!e) return;
    static const struct { const char *name; unsigned bit; } names[] = {
        {"svc", RT_TRACE_SVC}, {"host", RT_TRACE_HOSTCALL}, {"ipc", RT_TRACE_IPC}, {"fs", RT_TRACE_FS},
        {"gfx", RT_TRACE_GFX}, {"perf", RT_TRACE_PERF}, {"sched", RT_TRACE_SCHED}, {"audio", RT_TRACE_AUDIO},
        {"present", RT_TRACE_PRESENT}, {"all", ~0u}};
    for (const char *p = e; *p;) {
        size_t n = strcspn(p, ",");
        int known = 0;
        for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
            if (strlen(names[i].name) == n && !strncmp(p, names[i].name, n)) rt_trace |= names[i].bit, known = 1;
        if (!known && n) rt_log("NX2NAVI_TRACE: unknown item '%.*s' (svc, host, ipc, fs, gfx, perf, sched, audio, present, all)", (int)n, p);
        p += n + (p[n] == ',');
    }
}

void rt_sha256(const void *p, size_t n, uint8_t out[32]) { sha256(p, n, out); }

// ---- configuration
static RtConfig cfg;
const RtConfig *rt_config(void) { return &cfg; }

// ---- runtime services the generated code calls (cpu.h)
volatile int g_fnprof = 0;   // resolution.c switches it on until the game sets its render size (res_fn_hit)
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) { (void)lr, res_fn_hit(addr, x0); }

void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) {
    char at[128], lr[128];
    rt_fatal("brk #%#x at %s (lr %s)", imm, ldr_describe(pc, at, sizeof at), ldr_describe(c->x[30], lr, sizeof lr));
}

void rt_udf(Cpu *c, uint64_t pc) {
    char at[128], lr[128], d[128];
    rt_logf("udf at %s (lr %s)\n", ldr_describe(pc, at, sizeof at), ldr_describe(c->x[30], lr, sizeof lr));
    for (int i = 0; i < 31; i++) rt_logf("  x%-2d %016llx%s", i, (unsigned long long)c->x[i], i % 4 == 3 ? "\n" : "");
    rt_logf("  sp  %016llx\n  pc  %016llx\n", (unsigned long long)c->sp, (unsigned long long)c->pc);
    uint64_t fp = c->x[29];
    for (int n = 0; n < 32 && fp && !(fp & 7) && plat_mem_readable(fp, 16); n++) {
        uint64_t next = LD64(fp), link = LD64(fp + 8);
        rt_logf("  guest frame %2d: %s\n", n, ldr_describe(link, d, sizeof d));
        if (next <= fp || next - fp > (64u << 20)) break;
        fp = next;
    }
    rt_fatal("undefined instruction at %s (lr %s)", ldr_describe(pc, at, sizeof at),
             ldr_describe(c->x[30], lr, sizeof lr));
}

// CNTPCT_EL0: the Switch counter runs at 19.2 MHz. The scale is set before main, so no thread reads it half written.
static PlatScale tick_scale;

__attribute__((constructor)) static void tick_scale_init(void) { tick_scale = plat_scale(19200000u, plat_counter_hz()); }

uint64_t rt_ticks(void) { return plat_scaled(tick_scale, plat_counter()); }

// ---- crash reporter
typedef struct { uintptr_t host; uint64_t guest; } FnRev;
static FnRev *fn_rev;
static uint64_t fn_rev_n;

static int by_host(const void *a, const void *b) {
    uintptr_t x = ((const FnRev *)a)->host, y = ((const FnRev *)b)->host;
    return x < y ? -1 : x > y;
}

// The lifted function that contains a host code address, by the nearest function start below it; 0 outside the lifted
// code.
static uint64_t lifted_function(uintptr_t rip) {
    if (!fn_rev) {
        static RtModule mods[MAX_MODULES];
        int n = rt_linked_modules(mods, MAX_MODULES);
        uint64_t total = 0;
        for (int i = 0; i < n; i++) total += mods[i].count;
        fn_rev = malloc((total ? total : 1) * sizeof *fn_rev);
        if (!fn_rev) return 0;
        for (int i = 0; i < n; i++)
            for (uint64_t k = 0; k < mods[i].count; k++)
                fn_rev[fn_rev_n++] = (FnRev){(uintptr_t)mods[i].fns[k], mods[i].addrs[k]};
        qsort(fn_rev, fn_rev_n, sizeof *fn_rev, by_host);
    }
    // The lifted code is one range with the host code outside it (kernel.c ap_init checks that); an address above the
    // last function start is taken for host code, which costs only the last function's body.
    if (!fn_rev_n || rip > fn_rev[fn_rev_n - 1].host) return 0;
    uint64_t lo = 0, hi = fn_rev_n;
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (fn_rev[mid].host <= rip) lo = mid + 1;
        else hi = mid;
    }
    return lo ? fn_rev[lo - 1].guest : 0;
}

static int readable(uint64_t a, uint64_t n) { return plat_mem_readable(a, n); }

// A module another program put into the game's process: the Vulkan layer or the hook of a recorder, an overlay, a
// capture tool or a post-processing injector, by the words such files carry in their names. A crash inside one is that
// program's: one report was an access violation in a recorder's layer the moment the present mode changed and the
// swapchain was made again, another an injector's layer reading a null pointer at the end of every shader
// preparation.
static int foreign_hook(const char *module) {
    char low[160];
    size_t n = 0;
    for (; module && module[n] && n < sizeof low - 1; n++) low[n] = (char)tolower((unsigned char)module[n]);
    low[n] = 0;
    return strstr(low, "vk_layer") || strstr(low, "hook") || strstr(low, "overlay") || strstr(low, "reshade");
}

// Set when main_run has returned: the mode's work is done and saved, and all that still runs is the C runtime's exit
// and, inside ExitProcess, the detach call of every library in the process, the graphics driver's and those other
// programs put there. A fault in one of them is not a failure of the run. On one PC an injector's Vulkan layer faulted
// in its detach at the end of every `--warm-shaders` and `--prepare-shaders` (four runs over three versions): each
// wrote a FATAL after "variants built into the driver cache" and ended with code 71, so the launcher took a finished
// preparation for one that failed. crash_handler ends such a process with the code its mode returned. A fault in this
// executable's own code on the way out (an atexit handler) is still reported.
static volatile int32_t mode_done;
static int mode_rc;

static void crash_handler(const PlatCrash *x) {
    static volatile int32_t busy;
    if (plat_xchg32(&busy, 1)) plat_exit(71);
    if (__atomic_load_n(&mode_done, __ATOMIC_ACQUIRE) && x->module) {
        rt_log("exit: %s (%#x) in %s while the process was ending, after its work was done and saved%s: the run ends "
               "with its own code %d", x->what, x->code, *x->module ? x->module : "code outside every module",
               foreign_hook(x->module) ? " (that library is not part of the game: another program put it there)" : "",
               mode_rc);
        plat_exit(mode_rc);
    }
    vk_pipeline_crash_note();   // first: the next start tells a crash in the boot's builds from a stopped start by it
    rt_log_sync();   // the lines before the crash, then the report, each written as it is made
    char d[160];
    uintptr_t rip = x->rip;
    rt_logf("FATAL: %s (%#x) at host %p (", x->what, x->code, (void *)rip);
    if (x->module && !*x->module) rt_logf("outside every module)");
    else rt_logf("%s+%#llx)", x->module ? x->module : "nx2navi", (unsigned long long)x->offset);
    if (x->access >= 0)
        rt_logf(", %s address %#llx", x->access == 1 ? "writing" : x->access == 8 ? "executing" : "reading",
                (unsigned long long)x->addr);
    rt_log_raw("\n", 1);
    if (foreign_hook(x->module))
        rt_logf("  %s is not part of the game: a recording, overlay or capture program put it into the game. Close that "
                "program or turn off its Vulkan capture, then start the game again\n", x->module);
    // what the thread was doing (a pipeline build names its shaders): the stderr note crash_veh writes misses the log
    const char *note = plat_crash_context_get();
    if (note) rt_logf("  the thread was %s\n", note);
    uint64_t f = x->module ? 0 : lifted_function(rip);
    if (f) rt_logf("  in lifted function f_%llx = %s\n", (unsigned long long)f, ldr_describe(f, d, sizeof d));
    // abort and invalid parameter: where the host code called it; a fault in another module: the host calls that led
    // into it
    uintptr_t exe = plat_exe_base();
    for (int i = 0; i < x->nframes; i++) {
        uint64_t g = lifted_function(x->frames[i]);
        rt_logf("  host frame %2d: nx2navi+%#llx", i, (unsigned long long)(x->frames[i] - exe));
        if (g) rt_logf(" in lifted function f_%llx", (unsigned long long)g);
        rt_log_raw("\n", 1);
    }
    Cpu *c = kern_cpu();
    if (c) {
        for (int i = 0; i < 31; i++)
            rt_logf("  x%-2d %016llx%s", i, (unsigned long long)c->x[i], i % 4 == 3 ? "\n" : "");
        rt_logf("  sp  %016llx\n  pc  %016llx  lr %s\n", (unsigned long long)c->sp, (unsigned long long)c->pc,
                ldr_describe(c->x[30], d, sizeof d));
        uint64_t fp = c->x[29];
        for (int n = 0; n < 32 && fp && !(fp & 7) && plat_mem_readable(fp, 16); n++) {
            uint64_t next = LD64(fp), lr = LD64(fp + 8);
            rt_logf("  guest frame %2d: %s\n", n, ldr_describe(lr, d, sizeof d));
            if (next <= fp || next - fp > (64u << 20)) break;
            fp = next;
        }
    } else {
        rt_logf("  not on a guest thread\n");
    }
    rt_fatal_notes();   // the card, its driver and the memory settings in effect (vk_device.c fatal_note)
    kern_free_cores_note();
    fs_save_crash_wait();   // a save the game already reported as done finishes first (bounded; fs_host.c)
    rtss_limit_restore();   // after the report: RivaTuner Statistics Server's profile as it was
    fflush(stderr);
    prof_sampler_write();   // NX2NAVI_PROFILE: what was sampled until the crash (it gives up after 2 s if the sampler hangs)
    plat_exit(71);
}

// ---- self-test: layout, dispatch table, host-function registry, without game files
static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

static double now_s(void) { return (double)plat_now_ns() / 1e9; }

static volatile int host_probe_hits;
static void host_probe(Cpu *c) { host_probe_hits++; c->x[0] = 42; }

// The instruction set build_game.py --cpu compiled for: v3 (AVX2/FMA/BMI2, the default) or v2 (SSE4.2/POPCNT).
#ifndef RT_CPU_LEVEL
#define RT_CPU_LEVEL 3
#endif

static void check_cpu(void) {
    __builtin_cpu_init();
#if RT_CPU_LEVEL >= 3
    int ok = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2");
    printf("cpu: x86-64-v3 (AVX2, FMA, BMI2) %s\n", ok ? "present" : "MISSING");
    CHECK(ok, "the lifted code is built for x86-64-v3");
#else
    int ok = __builtin_cpu_supports("sse4.2") && __builtin_cpu_supports("popcnt");
    printf("cpu: x86-64-v2 (SSE4.2, POPCNT) %s\n", ok ? "present" : "MISSING");
    CHECK(ok, "the lifted code is built for x86-64-v2");
#endif
}

// Layout: ascending, no overlap, below 2^39, and inside the window mem_init reserved.
static void check_layout(void) {
    const RtLayoutEntry *lay = rt_layout;
    int n = rt_layout_count;
    printf("layout: %d modules\n", n);
    for (int i = 0; i < n; i++) {
        printf("  %-8s base %#llx  size %#10llx  end %#llx\n", lay[i].name, (unsigned long long)lay[i].base,
               (unsigned long long)lay[i].size, (unsigned long long)(lay[i].base + lay[i].size));
        CHECK(lay[i].size > 0, "%s has size 0", lay[i].name);
        CHECK(lay[i].base % 0x10000 == 0, "%s base is not 64 KiB aligned", lay[i].name);
        CHECK(lay[i].base + lay[i].size <= RT_KERNEL_BASE, "%s reaches into the kernel region", lay[i].name);
        if (i) {
            CHECK(lay[i].base > lay[i - 1].base, "%s base is not above %s", lay[i].name, lay[i - 1].name);
            CHECK(lay[i - 1].base + lay[i - 1].size <= lay[i].base, "%s overlaps %s", lay[i - 1].name, lay[i].name);
        }
        uint64_t end;
        int ours = plat_mem_state(lay[i].base, lay[i].base + 1, &end) == PLAT_MEM_RESERVED;
        CHECK(ours, "%s is not inside the reserved module window", lay[i].name);
    }
    const RtGuestLayout *g = mem_layout();
    printf("guest memory: aslr %#llx+%#llx stack %#llx heap %#llx alias %#llx+%#llx kernel %#llx host functions %#llx\n",
           (unsigned long long)g->aslr.base, (unsigned long long)g->aslr.size, (unsigned long long)g->stack.base,
           (unsigned long long)g->heap.base, (unsigned long long)g->alias.base, (unsigned long long)g->alias.size,
           (unsigned long long)g->kernel.base, (unsigned long long)g->hostfn.base);
    CHECK(g->alias.base + g->alias.size <= lay[0].base && g->heap.base + g->heap.size <= g->alias.base,
          "guest regions overlap");
}

// Every function start dispatches to its own function. The word after a start (unless that is a start too), a
// misaligned address and the word below the module dispatch to nothing.
static void check_modules(const RtModule *mods, int n) {
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        const RtModule *m = &mods[i];
        const RtLayoutEntry *l = NULL;
        for (int k = 0; k < rt_layout_count; k++)
            if (!strcmp(rt_layout[k].name, m->name)) l = &rt_layout[k];
        CHECK(l && l->base == m->base && l->size == m->size, "%s: linked base/size differ from the layout", m->name);

        uint64_t bad = 0, bad_neg = 0;
        for (uint64_t k = 0; k < m->count; k++) {
            uint64_t a = m->addrs[k];
            if (rt_dispatch_lookup(a) != m->fns[k] && bad++ < 5)
                printf("  FAIL: %s_fn_addrs[%llu] %#llx does not dispatch to its function\n", m->name,
                       (unsigned long long)k, (unsigned long long)a);
            int next_is_start = k + 1 < m->count && m->addrs[k + 1] == a + 4;
            if ((!next_is_start && rt_dispatch_lookup(a + 4)) || rt_dispatch_lookup(a + 2)) bad_neg++;
        }
        if (rt_dispatch_lookup(m->base - 4)) bad_neg++;
        failures += bad > 0;
        CHECK(!bad_neg, "%s: %llu addresses that are not function starts dispatch to a function", m->name,
              (unsigned long long)bad_neg);
        uint64_t lo = m->count ? m->addrs[0] : 0, hi = m->count ? m->addrs[m->count - 1] : 0;
        printf("  %-8s %8llu functions, first %#llx, last %#llx: %s\n", m->name, (unsigned long long)m->count,
               (unsigned long long)lo, (unsigned long long)hi, bad || bad_neg ? "FAIL" : "ok");
        total += m->count;
    }
    printf("  total    %8llu functions\n", (unsigned long long)total);
}

static void bench(const RtModule *mods, int n) {
    uint64_t total = 0, sink = 0;
    double t0 = now_s();
    for (int i = 0; i < n; i++) {
        for (uint64_t k = 0; k < mods[i].count; k++) sink += (uintptr_t)rt_dispatch_lookup(mods[i].addrs[k]);
        total += mods[i].count;
    }
    double dt = now_s() - t0;
    __asm__ volatile("" ::"r"(sink));
    if (total) printf("lookup: %.1f ns per function start (address order)\n", dt * 1e9 / (double)total);
}

// Memory SVCs without modules: heap, physical memory in the alias region, a stack move, QueryMemory, GetInfo.
static void check_memory(void) {
    const RtGuestLayout *g = mem_layout();
    RtMemoryInfo q;
    uint64_t heap = 0, v = 0;
    int before = failures;
    CHECK(mem_set_heap_size(0x400000, &heap) == 0 && heap == g->heap.base, "SetHeapSize 4 MiB");
    memset((void *)(uintptr_t)heap, 0xAB, 0x400000);
    mem_query(heap + 0x1000, &q);
    CHECK(q.base == heap && q.size == 0x400000 && q.state == 5 && q.perm == 3, "QueryMemory on the heap");
    CHECK(mem_set_heap_size(0x300000, &heap) != 0, "SetHeapSize accepts a size that is not a 2 MiB multiple");

    uint64_t a = g->alias.base + 0x100000;
    CHECK(mem_map_physical(a, 0x10000) == 0, "MapPhysicalMemory");
    ST64(a + 0x8000, 0x1234);
    CHECK(mem_map_physical(a - 0x4000, 0x20000) == 0 && LD64(a + 0x8000) == 0x1234,
          "MapPhysicalMemory over a mapped range lost its contents");
    mem_query(a, &q);
    CHECK(q.base == a - 0x4000 && q.size == 0x20000 && q.state == 5, "QueryMemory on mapped alias pages");
    mem_info(7, &v);
    CHECK(v == 0x400000 + 0x20000, "GetInfo 7 (used memory) %#llx", (unsigned long long)v);

    uint64_t stack = g->stack.base + 0x100000;
    ST64(a + 8, 0x99);
    CHECK(mem_map(stack, a, 0x4000) == 0 && LD64(stack + 8) == 0x99, "MapMemory alias -> stack did not copy");
    ST64(stack + 0x10, 0x5678);
    mem_query(stack, &q);
    CHECK(q.base == stack && q.size == 0x4000 && q.state == 0xB, "QueryMemory on a moved stack");
    mem_query(a, &q);
    CHECK(q.base == a && q.state == 5 && q.perm == 0, "QueryMemory on a moved stack's source");
    CHECK(mem_unmap_physical(a, 0x4000) != 0, "UnmapPhysicalMemory under a moved stack");
    CHECK(mem_unmap(stack, a, 0x4000) == 0 && LD64(a + 0x10) == 0x5678, "UnmapMemory did not copy the stack back");

    CHECK(mem_unmap_physical(a - 0x4000, 0x20000) == 0, "UnmapPhysicalMemory");
    mem_query(a, &q);
    CHECK(q.state == 0 && q.base == g->alias.base && q.size == g->alias.size, "QueryMemory on the free alias region");
    CHECK(mem_map_physical(a, 0x1000) == 0 && LD64(a + 0x10) == 0 && mem_unmap_physical(a, 0x1000) == 0,
          "remapped physical memory is not zeroed");
    // One page more than the whole pool (GetInfo 6), so the request is over the limit whatever the title's pool is.
    mem_info(6, &v);
    CHECK(mem_map_physical(a, ((v + 0xFFFull) & ~0xFFFull) + 0x1000) == 0x10801,
          "MapPhysicalMemory beyond the pool is not LimitReached");
    mem_query(RT_HOSTFN_BASE + 0x40, &q);
    CHECK(q.state == 0x10 && q.base == RT_HOSTFN_BASE, "QueryMemory on the host-function window");
    mem_query(g->alias.base + g->alias.size + 0x1000, &q);
    CHECK(q.state == 3 && q.base == g->alias.base + g->alias.size, "QueryMemory on host memory");
    CHECK(mem_set_heap_size(0, &heap) == 0, "SetHeapSize 0");
    printf("memory: heap, physical memory, stack move, QueryMemory, GetInfo: %s\n", failures > before ? "FAIL" : "ok");
}

static int selftest(void) {
    static RtModule mods[MAX_MODULES];
    int n = rt_linked_modules(mods, MAX_MODULES);

    check_cpu();
    check_layout();
    uint64_t probe = hostfn_register("<selftest probe>", host_probe);

    double t0 = now_s();
    hostfn_seal();
    double dt = now_s() - t0;
    RtDispatchStats s = rt_dispatch_stats();
    printf("dispatch: %d modules linked, table covers %#llx-%#llx: %llu slots, %.1f MiB reserved, built in %.0f ms\n",
           n, (unsigned long long)s.lo, (unsigned long long)s.hi, (unsigned long long)s.slots,
           (double)s.table_bytes / (1 << 20), dt * 1e3);
    check_modules(mods, n);
    CHECK(s.functions > 0, "no functions linked");
    Cpu *c = plat_aligned_alloc(sizeof(Cpu), 64);
    memset(c, 0, sizeof *c);
    uint64_t r = rt_call(c, probe, 1, (uint64_t)7);
    CHECK(rt_dispatch_lookup(probe) && r == 42 && host_probe_hits == 1 && c->x[30] == 0,
          "a registered host function does not dispatch through rt_call");
    CHECK(!rt_dispatch_lookup(probe + 16), "an unregistered host-function address dispatches");
    CHECK(rt_dispatch_lookup(RT_RET_HOST), "the rt_call return sentinel is not registered");
    plat_aligned_free(c);
    bench(mods, n);

    void *k1 = mem_kernel_alloc(0x200, 16), *k2 = mem_kernel_alloc(0x2000, 0x1000);
    CHECK((uintptr_t)k1 - RT_KERNEL_BASE < RT_KERNEL_SIZE && !((uintptr_t)k2 & 0xFFF) && k1 != k2,
          "kernel-region allocation");
    mem_kernel_free(k1, 0x200);
    CHECK(mem_kernel_alloc(0x200, 16) == k1, "a freed kernel-region block is not reused");
    check_memory();
    failures += vk_pipeline_spirv_selftest();

    float used, total, proc;
    plat_mem_info(&used, &total, &proc);
    if (proc >= 0) printf("process: working set %.0f MiB\n", proc);
    printf("selftest: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}

// --probe-crash-call and --probe-crash-null crash on purpose, to check the report of a fault outside this executable.
// call: strlen in the C runtime library (ucrtbase.dll, libc.so.6) reads address 8, the way a driver faults inside a
// call the host made; the report names the library and the host frames that called it. null: a call through a null
// function pointer, which leaves rip at 0, outside every module. Both end with exit code 71 and a FATAL line in the
// log.
static size_t (*volatile probe_strlen)(const char *) = strlen;
static void (*volatile probe_null)(void);

static int probe_crash(int null) {
    if (null) probe_null();
    else printf("%zu\n", probe_strlen((const char *)(uintptr_t)8));
    printf("probe-crash: still running, the crash did not happen\n");
    return 1;
}

// --probe-crash-exit: the fault of --probe-crash-call, made by an atexit handler after the mode has returned, the way
// a library another program put into the process faults in its detach. The process must end with the mode's own code
// (PROBE_EXIT_CODE) and an "exit:" line, with no FATAL (mode_done, crash_handler). tests/exit_fault_check.py runs it.
#define PROBE_EXIT_CODE 3
static void probe_exit_fault(void) { printf("%zu\n", probe_strlen((const char *)(uintptr_t)8)); }

static int probe_crash_exit(void) {
    atexit(probe_exit_fault);
    printf("probe-crash-exit: returning %d; a library then faults while the process ends\n", PROBE_EXIT_CODE);
    return PROBE_EXIT_CODE;
}

static int usage(const char *argv0) {
    fprintf(stderr, "usage: %s [--exefs DIR] [--romfs DIR] [--profile DIR] [--audio-device VALUE] [--monitor VALUE] [--list-audio-devices | --list-monitors | --selftest | --probe-loader | "
                    "--probe-cfg | --probe-dpi | --probe-mods | --probe-kernel | --probe-ipc | --probe-services | --probe-fs | --probe-gfx-abi | "
                    "--probe-nvn | --probe-nvn-tex | --probe-window | --probe-shader | --probe-vkmem | "
                    "--probe-vkexec | --probe-meshfb | --probe-astc | --probe-fsr3 | --probe-zstd | --probe-input | --probe-movie | --probe-crash-call | --probe-crash-null | --probe-crash-exit | --probe-discord | "
                    "--prepare-shaders | --warm-shaders | --prepare-pipelines | --probe-prewarm | --probe-buildgate | --probe-ffx-cache | --probe-memneed | --probe-devlost | --boot |--seconds S [--shot PNG]]\n", argv0);
    return 2;
}

// Paths for a copied folder (tools/package_build.py). Per path: NX2NAVI_EXEFS, NX2NAVI_ROMFS, NX2NAVI_PROFILE_DIR, else the
// folder next to nx2navi.exe (exefs with main.npdm, romfs with romfs.bin, any profile folder) if it is there, else the
// path baked in at build time. A relative value is relative to nx2navi.exe's folder.
static char pp_exefs[PLAT_PATH_MAX * 2], pp_romfs[PLAT_PATH_MAX * 2], pp_profile[PLAT_PATH_MAX * 2];

static const char *portable_path(const char *dir, const char *var, const char *sub, const char *must, char *out, size_t cap,
                                 const char *fallback) {
    const char *e = cfg_get(var);
    if (e && *e) {
        int abs_path = e[0] == '\\' || e[0] == '/' || e[1] == ':';
        if (abs_path) snprintf(out, cap, "%s", e);
        else snprintf(out, cap, "%s/%s", dir, e);
        return out;
    }
    char probe[PLAT_PATH_MAX * 3];
    snprintf(out, cap, "%s/%s", dir, sub);
    if (!must) return plat_dir_exists(out) ? out : fallback;   // the folder itself is the sign
    snprintf(probe, sizeof probe, "%s/%s", out, must);
    return plat_file_exists(probe) ? out : fallback;
}

static void portable_paths(RtConfig *c) {
    char exe[PLAT_PATH_MAX * 2];
    int n = plat_exe_path(exe, sizeof exe);
    char *cut = n && n < sizeof exe ? plat_last_sep(exe) : NULL;
    if (!cut) return;
    *cut = 0;
    c->exefs = portable_path(exe, "NX2NAVI_EXEFS", "exefs", "main.npdm", pp_exefs, sizeof pp_exefs, c->exefs);
    c->romfs = portable_path(exe, "NX2NAVI_ROMFS", "romfs", "romfs.bin", pp_romfs, sizeof pp_romfs, c->romfs);
    c->profile = portable_path(exe, "NX2NAVI_PROFILE_DIR", "profile", NULL, pp_profile, sizeof pp_profile, c->profile);
}

static int main_run(int argc, char **argv);

// A game run never comes back here: it ends in plat_exit (boot.c, gfx_present.c). Every other mode returns its code.
int main(int argc, char **argv) {
    mode_rc = main_run(argc, argv);
    __atomic_store_n(&mode_done, 1, __ATOMIC_RELEASE);   // from here on only the exit runs (mode_done, above)
    return mode_rc;
}

static int main_run(int argc, char **argv) {
    mem_init();   // first: claim the guest address space before anything else can land there; reads nx2navi.ini
    // The build that wrote the log, at its top: an attached log from another install or version shows at a glance.
    rt_log("version: NX2navi %s, game %d.%d.%d", rt_port_version, rt_game_version / 100, rt_game_version / 10 % 10,
           rt_game_version % 10);
    // A restart from the settings menu (gfx_present.c gfx_window_closed) starts this process while the one it replaces
    // is still exiting: its log, RomFS and saves are free once it is gone.
    if (plat_restart_wait(30000) < 0) rt_log("restart: the previous process still runs after 30 s; starting anyway");
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    plat_crash_hook(crash_handler);
    parse_trace();
    prof_start();
    // Timed guest waits (kernel.c block_inner) sleep in whole milliseconds; at the default 15.6 ms tick a 1 ms wait
    // costs a full tick. NX2NAVI_TIMER_1MS=0 keeps the default for comparison.
    if (cfg_bool("NX2NAVI_TIMER_1MS")) plat_timer_1ms();
    // Before any thread pool counts the usable processors. Not in the shader preparation modes: they translate and
    // build on every processor the process was given, and the placement (perf.c cpu_placement) is for the game's own
    // threads, so narrowing them to one die would only make the preparation take longer.
    int prepare = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--prepare-shaders") || !strcmp(argv[i], "--warm-shaders") ||
            !strcmp(argv[i], "--prepare-pipelines")) prepare = 1;
    if (!prepare) perf_cpu_start();
    {   // NX2NAVI_POWER_FULL=0 leaves Windows' power throttling of an unfocused window in place (A/B runs)
        const char *pf = cfg_get("NX2NAVI_POWER_FULL");
        if (!(pf && !strcmp(pf, "0"))) {
            int got = plat_power_full();
            rt_log("power: %s", got == 2 ? "full speed and the 1 ms timer kept when the window is not in front"
                               : got == 1 ? "full speed kept when the window is not in front (no timer control here)"
                                          : "the OS throttling of an unfocused window stays as it is");
        }
    }
    cfg = (RtConfig){.exefs = rt_default_exefs, .romfs = rt_default_romfs, .profile = rt_default_profile,
                     .gpu = cfg_get("NX2NAVI_GPU")};
    portable_paths(&cfg);   // command-line paths below still win
    if (cfg.gpu && strcmp(cfg.gpu, "null") && strcmp(cfg.gpu, "vulkan")) {
        rt_logf("NX2NAVI_GPU must be null or vulkan, not '%s'\n", cfg.gpu);
        return 2;
    }
    const char *mode = NULL;
    for (int i = 1; i < argc; i++) {
        const char **path = !strcmp(argv[i], "--exefs") ? &cfg.exefs : !strcmp(argv[i], "--romfs") ? &cfg.romfs
                          : !strcmp(argv[i], "--profile") ? &cfg.profile : NULL;
        if (path) {
            if (++i == argc) return usage(argv[0]);
            *path = argv[i];
        } else if (!strcmp(argv[i], "--audio-device")) {
            // Same approach as every setting: NX2NAVI_AUDIO_DEVICE is read from the environment first (cfg.c raw_get).
            // cfg_init has read the settings by now, so the variable goes in through cfg_setenv, which makes the lookups
            // read again. An empty value would remove the variable (plat_setenv) and hand the choice back to
            // nx2navi.ini, so "default" is the spelling for the system default.
            if (++i == argc || !*argv[i]) return usage(argv[0]);
            cfg_setenv("NX2NAVI_AUDIO_DEVICE", argv[i]);
        } else if (!strcmp(argv[i], "--monitor")) {
            // The settings read NX2NAVI_MONITOR from the environment first (cfg.c raw_get); cfg_setenv as above. An empty
            // value would remove the variable (plat_setenv) and hand the choice back to nx2navi.ini, so "auto" is the
            // spelling for "no monitor choice" on the command line.
            if (++i == argc || !*argv[i]) return usage(argv[0]);
            cfg_setenv("NX2NAVI_MONITOR", argv[i]);
        } else if (!strcmp(argv[i], "--seconds")) {
            char *end;
            if (++i == argc) return usage(argv[0]);
            double s = strtod(argv[i], &end);
            if (*end || !(s > 0) || s > 1e6) return usage(argv[0]);
            cfg.seconds = s;
        } else if (!strcmp(argv[i], "--shot")) {
            if (++i == argc) return usage(argv[0]);
            cfg.shot = argv[i];
        } else if (!mode && !strncmp(argv[i], "--", 2)) {
            mode = argv[i];
        } else {
            return usage(argv[0]);
        }
    }
    if (cfg.shot && !cfg.seconds) return usage(argv[0]);
    if (cfg.seconds && mode) return usage(argv[0]);
    log_open(!mode);
    if (!mode) {
        // the console of a game started directly: no QuickEdit to stop the log, and its close saves (console_closed)
        if (plat_console_setup(console_closed))
            rt_log("console: QuickEdit off; closing the console ends the game as its window does");
        mem_start_note(0);   // memory.c: a warning when the machine has less memory free than the game takes
        cfg.boot = cfg.seconds > 0;   // --seconds starts the game like --boot, with its checkpoints
        vk_driver_cache_env();   // before the threads below and rt_boot's, any of which may load the Vulkan driver
        discord_init();   // the Rich Presence thread (discord_rpc.c); it reads nx2navi.ini itself and idles while off
        rt_boot();
        return 0;
    }
    if (!strcmp(mode, "--selftest")) return selftest();
    if (!strcmp(mode, "--list-audio-devices")) return list_audio_devices();
    if (!strcmp(mode, "--list-monitors")) return list_monitors();
    if (!strcmp(mode, "--probe-loader")) return probe_loader();
    if (!strcmp(mode, "--probe-crash-call")) return probe_crash(0);
    if (!strcmp(mode, "--probe-crash-null")) return probe_crash(1);
    if (!strcmp(mode, "--probe-crash-exit")) return probe_crash_exit();
    if (!strcmp(mode, "--probe-cfg")) return probe_cfg();
#ifdef _WIN32
    if (!strcmp(mode, "--probe-dpi")) return probe_dpi();   // the Win32 DPI awareness of nx2navi.manifest
#endif
    if (!strcmp(mode, "--probe-mods")) return probe_mods();
    if (!strcmp(mode, "--probe-discord")) return probe_discord();
    if (!strcmp(mode, "--probe-kernel")) return probe_kernel();
    if (!strcmp(mode, "--probe-ipc")) return probe_ipc();
    if (!strcmp(mode, "--probe-services")) return probe_services();
    if (!strcmp(mode, "--probe-fs")) return probe_fs();
    if (!strcmp(mode, "--probe-gfx-abi")) return probe_gfx_abi();
    if (!strcmp(mode, "--probe-nvn")) return probe_nvn();
    if (!strcmp(mode, "--probe-nvn-tex")) return probe_nvn_tex();
    if (!strcmp(mode, "--probe-window")) return probe_window();
    if (!strcmp(mode, "--probe-shader")) return probe_shader();
    if (!strcmp(mode, "--probe-vkmem")) return probe_vkmem();
    if (!strcmp(mode, "--probe-vkexec")) return probe_vkexec();
    if (!strcmp(mode, "--probe-meshfb")) return probe_meshfb();
    if (!strcmp(mode, "--probe-astc")) return probe_astc();
    if (!strcmp(mode, "--probe-fsr3")) return probe_fsr3();
    if (!strcmp(mode, "--probe-zstd")) return probe_zstd();
    if (!strcmp(mode, "--probe-input")) return probe_gamepad();
    if (!strcmp(mode, "--probe-vkdevice")) return probe_vkdevice();
    if (!strcmp(mode, "--probe-devlost")) return vk_sync_probe_devlost();
    if (!strcmp(mode, "--probe-movie")) {   // the AV1 decoder tells the graphics back end of each frame it writes: none here
        cfg.gpu = "null";
        return probe_movie();
    }
    if (!strcmp(mode, "--prepare-shaders")) return vk_pipeline_prepare_shaders();
    if (!strcmp(mode, "--warm-shaders")) return vk_pipeline_warm_shaders();
    if (!strcmp(mode, "--prepare-pipelines")) return vk_pipeline_prepare_pipelines();
    if (!strcmp(mode, "--probe-prewarm")) return vk_pipeline_probe_prewarm();
    if (!strcmp(mode, "--probe-buildgate")) return vk_pipeline_probe_buildgate();
    if (!strcmp(mode, "--probe-ffx-cache")) return vk_ffx_probe_cache();
    if (!strcmp(mode, "--probe-memneed")) { mem_start_note(1); return 0; }
    if (!strcmp(mode, "--boot")) { mem_start_note(0); cfg.boot = 1; rt_boot(); return 0; }
    return usage(argv[0]);
}
