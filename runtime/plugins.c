// The plugin interface (plugin_api.h, docs/plugins.md): a table of functions a mod's native library gets from the
// executable's export nx2navi_plugin_api, the callbacks it registers through it, and the libraries the game loads
// itself at start (NX2NAVI_MOD_DLLS).
//
// Callbacks are kept per kind in a fixed table under one lock. A dispatch copies the registered ones out under the lock
// and calls them without it, so a callback may register and remove callbacks, and a removal on another thread never
// waits for a callback that runs long; the price is that a removed callback can run once more. A library that registers
// a callback is pinned (plat_lib_pin), so that call never lands in an unloaded library.
//
// Where the callbacks run:
//   frame    nvnQueuePresentTexture (nvn_api_obj.c), on the guest thread that presents, with its virtual core held and
//            before the present is queued. It is the frame boundary the runtime itself uses to call into the game
//            (resolution.c res_present, coop_game.c coop_present): the game has finished the frame's command lists and
//            has not started the next frame's work on that thread, and the thread is at a call the runtime may make
//            guest calls from (its registers are saved and restored around them).
//   overlay  ui/menu.cpp overlay_frame, on the present path under its lock, inside the Dear ImGui frame that also
//            holds the settings menu and the performance overlay.
//   key/text the window's thread (ui/menu_win.cpp, ui/menu_sdl.cpp).
//   hook     the entry of a lifted function (cpu.h FN_ENTER, main.c fnprof_hit), on the guest thread that calls it;
//            section "function hooks" below.
//   exit     the thread that ends the process, on its orderly ways out (plugins_exit).
//   pad      the guest thread that reads the controls (input.c input_poll), at each read.
//   title    a title menu button's choice: with the frame callbacks, before them. The title screen's own entry is
//            kept in hooks of the runtime's own, on the game's threads; section "title menu buttons".
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dispatch.h"
#include "fs.h"
#include "host.h"
#include "gfx.h"
#include "input.h"
#include "input_map.h"
#include "input_wsys.h"
#include "mods.h"
#include "ov.h"
#include "platform/platform.h"
#include "plugin_game_ui.h"
#include "plugins.h"

enum { KIND_FRAME, KIND_OVERLAY, KIND_KEY, KIND_TEXT, KIND_EXIT, KIND_PAD, KINDS };
enum { MAX_CB = 32 };   // per kind
static const char *const kind_name[KINDS] = {"frame", "overlay", "key", "text", "exit", "pad"};

typedef struct { void *fn, *user; int handle; } Slot;
static Slot slots[KINDS][MAX_CB];
static int counts[KINDS];        // registered callbacks per kind; read without the lock by the dispatchers
static int next_handle = 1;
static PlatMutex lock = PLAT_MUTEX_INIT;

static uint64_t frames;          // frame callbacks' Nx2Frame.index: presents so far
static Cpu *frame_cpu;           // the guest thread's Cpu while its frame callbacks run, else NULL
static int capture;              // capture_input calls not yet undone
static int refusals;             // guest_call refusals logged so far (the first REFUSALS_LOGGED are)
enum { REFUSALS_LOGGED = 16 };

// ---- who calls

// The file name, without its extension, of the library that holds the code address at; "?" when unknown. The log
// lines name the plugin by it.
static void owner_name(const void *at, char *out, size_t cap) {
    uintptr_t base;
    if (!plat_module_of((uintptr_t)at, out, cap, &base) || !*out) snprintf(out, cap, "?");
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
}

// ---- callbacks

static int add(int kind, void *fn, void *user) {
    if (!fn) return 0;
    char who[64];
    owner_name(fn, who, sizeof who);
    plat_lib_pin(fn);   // the library stays loaded for the life of the process: its callbacks stay callable
    int handle = 0;
    plat_mutex_lock(&lock);
    for (int i = 0; i < MAX_CB && !handle; i++) {
        if (slots[kind][i].fn) continue;
        handle = next_handle++;
        if (next_handle <= 0) next_handle = 1;
        slots[kind][i] = (Slot){fn, user, handle};
        __atomic_add_fetch(&counts[kind], 1, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&lock);
    if (handle) rt_log("plugins: %s registered a callback of the kind %s", who, kind_name[kind]);
    else rt_log("plugins: %s could not register a callback of the kind %s: all %d are taken", who, kind_name[kind], MAX_CB);
    return handle;
}

static void hook_remove_locked(int handle);
static void title_remove_locked(int handle);

static void api_remove(int handle) {
    if (handle <= 0) return;
    plat_mutex_lock(&lock);
    hook_remove_locked(handle);
    title_remove_locked(handle);
    for (int k = 0; k < KINDS; k++)
        for (int i = 0; i < MAX_CB; i++)
            if (slots[k][i].fn && slots[k][i].handle == handle) {
                slots[k][i] = (Slot){0};
                __atomic_sub_fetch(&counts[k], 1, __ATOMIC_RELEASE);
            }
    plat_mutex_unlock(&lock);
}

// The callbacks of one kind as registered now, in table order; their number.
static int snapshot(int kind, Slot out[MAX_CB]) {
    if (!__atomic_load_n(&counts[kind], __ATOMIC_ACQUIRE)) return 0;
    int n = 0;
    plat_mutex_lock(&lock);
    for (int i = 0; i < MAX_CB; i++)
        if (slots[kind][i].fn) out[n++] = slots[kind][i];
    plat_mutex_unlock(&lock);
    return n;
}

static int api_on_frame(Nx2FrameFn fn, void *user) { return add(KIND_FRAME, (void *)fn, user); }
static int api_on_overlay(Nx2OverlayFn fn, void *user) { return add(KIND_OVERLAY, (void *)fn, user); }
static int api_on_key(Nx2KeyFn fn, void *user) { return add(KIND_KEY, (void *)fn, user); }
static int api_on_text(Nx2TextFn fn, void *user) { return add(KIND_TEXT, (void *)fn, user); }
static int api_on_exit(Nx2ExitFn fn, void *user) { return add(KIND_EXIT, (void *)fn, user); }
static int api_on_pad(Nx2PadFn fn, void *user) { return add(KIND_PAD, (void *)fn, user); }

// ---- information and the log

static const Nx2GameInfo *api_game_info(void) {
    static Nx2GameInfo info;
    static char game_dir[PLAT_PATH_MAX * 3], mods[2048], build[80], profile[1024], save[1100], lang[16], account[40];
    static int ready, saved, save_gen;
    // The save's folder has its name once the game's storage is set up (fs_host.c storage_init), which is after the
    // plugins' start: it is filled in by the first call that finds it, its first byte last, so that a reader on
    // another thread sees "" or all of it.
    if (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) && !__atomic_load_n(&saved, __ATOMIC_ACQUIRE) && *fs_save_dir()) {
        plat_mutex_lock(&lock);
        const char *dir = fs_save_dir();
        size_t n = strlen(dir);
        if (!saved && n && n < sizeof save) {
            memcpy(save + 1, dir + 1, n);   // with the terminator
            __atomic_store_n(&save[0], dir[0], __ATOMIC_RELEASE);
        }
        __atomic_store_n(&saved, 1, __ATOMIC_RELEASE);
        plat_mutex_unlock(&lock);
    }
    // After set_save_dir (fs_host.c fs_save_dir_set) the folder has another name: a new text, and the structure's
    // pointer goes to it, so that a reader that still holds the old pointer reads the old name whole. The few old
    // texts stay allocated.
    if (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) && __atomic_load_n(&save_gen, __ATOMIC_ACQUIRE) != fs_save_dir_gen()) {
        plat_mutex_lock(&lock);
        int gen = fs_save_dir_gen();
        const char *dir = fs_save_dir();
        char *copy = *dir && gen != save_gen ? strdup(dir) : NULL;
        if (copy) {
            __atomic_store_n(&info.save_dir, copy, __ATOMIC_RELEASE);
            __atomic_store_n(&save_gen, gen, __ATOMIC_RELEASE);
        }
        plat_mutex_unlock(&lock);
    }
    if (__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) return &info;
    plat_mutex_lock(&lock);
    if (!ready) {
        if (plat_exe_path(game_dir, sizeof game_dir)) {
            char *cut = plat_last_sep(game_dir), *f = strrchr(game_dir, '/');
            if (f > cut) cut = f;
            if (cut) *cut = 0;
        } else game_dir[0] = 0;
        mods_scan();   // sets the folder mods_dir names; the scan runs once per process whoever asks first
        snprintf(mods, sizeof mods, "%s", mods_dir() ? mods_dir() : "");
        snprintf(build, sizeof build, "%s", mods_main_build_id());
        snprintf(profile, sizeof profile, "%s", fs_profile_dir());
        if (*fs_save_dir() && strlen(fs_save_dir()) < sizeof save) strcpy(save, fs_save_dir()), saved = 1;
        snprintf(lang, sizeof lang, "%s", ov_language());
        // the account's folder name as fs_host.c save_roots writes it; a probe without a profile has no account
        uint64_t uid[2] = {0, 0};
        if (*fs_profile_dir()) account_uid(uid);
        if (uid[0] | uid[1]) snprintf(account, sizeof account, "%016llx%016llx", (unsigned long long)uid[1], (unsigned long long)uid[0]);
        info =(Nx2GameInfo){.size = sizeof info, .game_version = (uint32_t)rt_game_version,
                             .runtime_version = rt_port_version, .game_dir = game_dir, .mods_dir = mods,
                             .build_id = build, .profile_dir = profile, .save_dir = save, .language = lang,
                             .account = account};
        for (int i = 0; i < rt_layout_count; i++)
            if (!strcmp(rt_layout[i].name, "main")) info.main_base = rt_layout[i].base, info.main_size = rt_layout[i].size;
        __atomic_store_n(&ready, 1, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&lock);
    return &info;
}

static void api_log(const char *fmt, ...) {
    char buf[2048], who[64];
    owner_name(__builtin_return_address(0), who, sizeof who);
    int n = snprintf(buf, sizeof buf, "[plugin %s] ", who);
    va_list ap;
    va_start(ap, fmt);
    int k = fmt ? vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap) : 0;
    va_end(ap);
    n = k < 0 ? n : n + k < (int)sizeof buf - 1 ? n + k : (int)sizeof buf - 2;
    buf[n++] = '\n';
    rt_log_raw(buf, (size_t)n);
}

// ---- memory

// Guest memory has pages that fault on purpose and are served by the runtime's own fault handler (memory.c: the lazy
// heap's reserved pages, frozen regions, pages armed for a lazy write-back), so "is it mapped" is asked in the terms
// memory.c keeps. A read: every page committed and readable once the lazy heap's pages in the range are committed
// (mem_host_readable), or guest-readable pages armed for a write-back, which the copy's own fault brings down.
//
// Both checks take locks of memory.c and ask the system about pages, which cost a plugin about a millisecond a call
// while the game ran (the freeze lock is held across page protection changes, and so is the system's own lock of the
// address space). So a range inside memory the guest's own code touches with plain loads and stores is copied without
// either: span_ok keeps, per thread, the last regions memory.c named as such (mem_guest_span) with the mapping
// generation they were named at, and a range inside one is good while the generation stands, which is one load to
// check. A page there that is not accessible right now (left reserved by the lazy heap, frozen, armed) faults into
// memory.c's hooks like a guest access and the copy goes on. Everything else takes the checks below as before.
// NX2NAVI_PLUGIN_FAST_MEM=0 turns the short way off (to measure it, or to rule it out).
typedef struct { uint64_t lo, hi; uint32_t gen; uint8_t write; } Span;
enum { SPANS = 8 };
static _Thread_local Span spans[SPANS];
static _Thread_local unsigned span_next;

static int fast_mem(void) {
    static int on = -1;
    int v = __atomic_load_n(&on, __ATOMIC_RELAXED);
    if (v < 0) {
        char e[8];
        v = !(plat_getenv("NX2NAVI_PLUGIN_FAST_MEM", e, sizeof e) && e[0] == '0');
        __atomic_store_n(&on, v, __ATOMIC_RELAXED);
    }
    return v;
}

// 1 when [addr, addr + n) lies in one region the guest's code reads (write: and writes) directly.
static int span_ok(uint64_t addr, size_t n, int write) {
    if (addr + n < addr) return 0;
    uint32_t gen = mem_map_gen();
    for (int i = 0; i < SPANS; i++) {
        const Span *s = &spans[i];
        if (s->hi && s->gen == gen && addr >= s->lo && addr + n <= s->hi && s->write >= write) return 1;
    }
    if (!fast_mem()) return 0;
    Span s = {.write = (uint8_t)(write != 0)};
    if (!mem_guest_span(addr, write, &s.lo, &s.hi, &s.gen) || addr + n > s.hi) return 0;
    spans[span_next++ % SPANS] = s;
    return 1;
}

static void *api_map_range(uint64_t addr, size_t n, int writable) {
    return n && span_ok(addr, n, writable != 0) ? (void *)(uintptr_t)addr : NULL;
}

static int api_read(uint64_t addr, void *buf, size_t n) {
    if (!n) return 1;
    if (!buf || addr + n < addr) return 0;
    if (span_ok(addr, n, 0)) {
        memcpy(buf, (const void *)(uintptr_t)addr, n);
        return 1;
    }
    if (mem_host_readable(addr, n)) {
        memcpy(buf, (const void *)(uintptr_t)addr, n);
        return 1;
    }
    if (addr + n > RT_ADDRESS_LIMIT || !mem_lazy_armed(addr, n) || !mem_read_hold(addr, n)) return 0;
    memcpy(buf, (const void *)(uintptr_t)addr, n);
    mem_read_release();
    return 1;
}

// A write: only into what the guest itself may write (the heap, mapped alias pages, a module's data, the kernel region's
// thread blocks). mem_thaw makes frozen regions writable and commits the lazy heap's pages, as before a file read into
// guest memory; what is not read-write after it (a page of the kernel region nothing was allocated in) is refused.
static int api_write(uint64_t addr, const void *buf, size_t n) {
    if (!n) return 1;
    if (!buf || addr + n < addr || addr + n > RT_ADDRESS_LIMIT) return 0;
    if (span_ok(addr, n, 1)) {
        memcpy((void *)(uintptr_t)addr, buf, n);
        return 1;
    }
    for (uint64_t a = addr, end = addr + n; a < end;) {
        RtMemoryInfo r;
        mem_query(a, &r);
        // Horizon's memory states 4 (a module's data), 5 (heap, alias), 0xB (a mapped stack), 0xC (thread blocks);
        // permission bit 1 is write
        int state_ok = r.state == 4 || r.state == 5 || r.state == 0xB || r.state == 0xC;
        if (!state_ok || !(r.perm & 2) || r.base + r.size <= a) return 0;
        a = r.base + r.size;
    }
    mem_thaw(addr, n);
    for (uint64_t a = addr, end = addr + n, e; a < end; a = e)
        if (plat_mem_state(a, end, &e) != PLAT_PROT_RW || e <= a) {
            if (!mem_lazy_armed(addr, n)) return 0;
            break;   // armed pages take the write through their fault
        }
    memcpy((void *)(uintptr_t)addr, buf, n);
    return 1;
}

// ---- guest calls

static void refuse(const void *caller, uint64_t addr, const char *why) {
    if (__atomic_fetch_add(&refusals, 1, __ATOMIC_RELAXED) >= REFUSALS_LOGGED) return;
    char who[64], at[64];
    owner_name(caller, who, sizeof who);
    rt_log("plugins: guest call of %s by %s refused: %s", rt_guest_name(addr, at, sizeof at), who, why);
}

// The lifted function that starts at addr, NULL when none does. Inside a module only: the host functions' synthetic
// addresses dispatch too and are not the game's.
static GuestFn game_function(uint64_t addr) {
    int lifted = 0;
    for (int i = 0; i < rt_layout_count; i++) lifted |= addr - rt_layout[i].base < rt_layout[i].size;
    return lifted ? rt_dispatch_lookup(addr) : NULL;
}

// The calling guest thread's Cpu when a guest call of addr may run now, else NULL with the refusal logged.
static Cpu *call_cpu(const void *caller, uint64_t addr) {
    Cpu *c = kern_cpu();
    if (!c || (c != __atomic_load_n(&frame_cpu, __ATOMIC_ACQUIRE) && !c->fn_hooks)) {
        refuse(caller, addr, "guest calls run only inside a frame callback or a hook callback, on its thread");
        return NULL;
    }
    if (!game_function(addr)) {
        refuse(caller, addr, "no function of the game starts at this address");
        return NULL;
    }
    return c;
}

static uint64_t api_guest_call(uint64_t addr, const uint64_t *args, int nargs) {
    const void *caller = __builtin_return_address(0);
    if (nargs < 0 || nargs > 8 || (nargs && !args)) {
        refuse(caller, addr, "0 to 8 arguments are passed");
        return 0;
    }
    Cpu *c = call_cpu(caller, addr);
    if (!c) return 0;
    for (int i = 0; i < nargs; i++) c->x[i] = args[i];
    return rt_call(c, addr, 0);   // rt_call with no arguments leaves x0-x7 as set above
}

static int api_guest_call_regs(uint64_t addr, Nx2GuestRegs *regs) {
    const void *caller = __builtin_return_address(0);
    if (!regs || regs->size < offsetof(Nx2GuestRegs, stack)) {   // the block's size in version 1
        refuse(caller, addr, "the register block is missing or too small");
        return 0;
    }
    uint64_t nstack = NX2_HAS(regs, Nx2GuestRegs, nstack) ? regs->nstack : 0;
    if (nstack > NX2_STACK_ARGS_MAX || (nstack && !regs->stack)) {
        refuse(caller, addr, "more stack arguments than the interface passes, or none given");
        return 0;
    }
    Cpu *c = call_cpu(caller, addr);
    if (!c) return 0;
    for (int i = 0; i < 9; i++) c->x[i] = regs->x[i];
    for (int i = 0; i < 8; i++) c->v[i] = (__uint128_t)regs->v[i][1] << 64 | regs->v[i][0];
    // Stack arguments: below the thread's stack pointer, which is where the function that is running there would put
    // its own callee's (the guest's stack pointer is current in the Cpu at a call, recomp/regs.py); 16-byte aligned as
    // the calling convention has it at a call. The callee leaves the stack pointer as it found it; it is put back
    // regardless.
    uint64_t sp = c->sp;
    if (nstack) {
        c->sp = (sp - 8 * nstack) & ~15ull;
        memcpy((void *)(uintptr_t)c->sp, regs->stack, 8 * nstack);
    }
    rt_call(c, addr, 0);
    c->sp = sp;
    for (int i = 0; i < 9; i++) regs->x[i] = c->x[i];
    for (int i = 0; i < 8; i++) regs->v[i][0] = (uint64_t)c->v[i], regs->v[i][1] = (uint64_t)(c->v[i] >> 64);
    return 1;
}

// ---- function hooks
//
// How a hook reaches a function. Lifted functions call each other directly, as C calls (recomp/gen.py call), and only
// indirect branches go through the dispatch table, so a table entry cannot stand for a function. What every lifted
// function does have is FN_ENTER as its first statement (cpu.h): a test of g_fnprof, which resolution.c already uses
// to see one function called at start. A hook is that test taken further: with a hook registered, g_fnprof holds
// RT_FN_HOOKS, a function whose byte in rt_fn_filter is not 0 calls fnprof_hit (main.c), and that calls plugins_fn_enter
// here, which looks the address up among the hooks. So any function start can be hooked while the game runs, with no
// list at build time and no rebuild, and a function nobody hooks pays the one predicted branch it always paid, plus a
// second one and a load from the 4 KiB filter while any hook exists.
//
// What a hooked call does (plugins_hook_enter). The guest's registers at the call are kept. Each hook's callback gets
// the argument registers in an Nx2HookCall; the next hook of the same function, or after the last one the function
// itself, runs when the callback asks for it (call_original) or, unless it asked to leave the function out, when it
// returns. The function itself is the lifted function called again with Cpu.fn_pass naming it, which makes its
// FN_ENTER pass once. Before it runs, the registers are put back as they were at the call, with the arguments as the
// callbacks left them: guest calls a callback made leave no trace in what the function sees. When all is done the lifted
// function returns to its caller (FN_ENTER) with the results in x0-x8 and v0-v7 and with the registers a callee
// preserves (x19-x30, sp, v8-v15) as at the call, which is all a lifted caller reads after a call (recomp/regs.py).
//
// The table is written under `lock`; an address is also read without it, by the scan that tells a hooked function from
// its neighbours in the filter.
enum { CHAIN_MAX = 8 };              // hooks of one function that run; more are registered but not called
#define CALL_MAGIC 0x4b4f4f48u       // marks an Nx2HookCall of ours while its callback runs

typedef struct { uint64_t addr; Nx2HookFn fn; void *user; int handle; } Hook;
static Hook hooks[NX2_HOOKS_MAX];
static int hook_count;

typedef struct {
    Cpu *c;
    uint64_t addr;
    GuestFn body;                    // the lifted function
    Hook h[CHAIN_MAX];               // its hooks, in the order they were registered
    int n;
    uint32_t tid;                    // the thread the call is on
    Cpu at;                          // the Cpu at the call
} HookRun;
typedef struct { Nx2HookCall pub; uint32_t magic; int level, ran; HookRun *run; } HookLevel;

static void regs_in(Cpu *c, const uint64_t x[9], uint64_t v[8][2]) {
    for (int i = 0; i < 9; i++) c->x[i] = x[i];
    for (int i = 0; i < 8; i++) c->v[i] = (__uint128_t)v[i][1] << 64 | v[i][0];
}

static void regs_out(const Cpu *c, uint64_t x[9], uint64_t v[8][2]) {
    for (int i = 0; i < 9; i++) x[i] = c->x[i];
    for (int i = 0; i < 8; i++) v[i][0] = (uint64_t)c->v[i], v[i][1] = (uint64_t)(c->v[i] >> 64);
}

// Runs the hooks of r from `level` on and then the function, with the arguments x and v; what was returned is in x and
// v afterwards.
static void hook_level(HookRun *r, int level, uint64_t x[9], uint64_t v[8][2]) {
    Cpu *c = r->c;
    if (level == r->n) {
        memcpy(c->x, r->at.x, sizeof c->x);
        memcpy(c->v, r->at.v, sizeof c->v);
        c->sp = r->at.sp, c->nzcv = r->at.nzcv, c->fpsr = r->at.fpsr;
        regs_in(c, x, v);
        rt_fpcr_sync(c);   // a plugin's own code may have changed the host's rounding and denormal modes
        c->fn_pass = r->addr;
        r->body(c);
        c->fn_pass = 0;    // a stand-in body (the probe's) has no FN_ENTER that takes it
        regs_out(c, x, v);
        return;
    }
    HookLevel l = {.pub = {.size = sizeof l.pub, .addr = r->addr, .sp = r->at.sp, .lr = r->at.x[30], .fp = r->at.x[29]},
                   .magic = CALL_MAGIC, .level = level, .run = r};
    memcpy(l.pub.x, x, sizeof l.pub.x);
    memcpy(l.pub.v, v, sizeof l.pub.v);
    c->fn_hooks++;
    r->h[level].fn(&l.pub, r->h[level].user);
    c->fn_hooks--;
    l.magic = 0;
    if (!l.ran && !l.pub.skip) hook_level(r, level + 1, l.pub.x, l.pub.v);
    memcpy(x, l.pub.x, sizeof l.pub.x);
    memcpy(v, l.pub.v, sizeof l.pub.v);
}

int plugins_hook_enter(Cpu *c, uint64_t addr, GuestFn body) {
    HookRun r;
    r.c = c, r.addr = addr, r.body = body, r.n = 0, r.tid = (uint32_t)plat_thread_id();
    plat_mutex_lock(&lock);
    for (int i = 0; i < NX2_HOOKS_MAX; i++) {
        if (hooks[i].addr != addr || !hooks[i].fn) continue;
        int k = r.n < CHAIN_MAX ? r.n++ : -1;   // by handle, ascending: the order of registration
        for (; k > 0 && r.h[k - 1].handle > hooks[i].handle; k--) r.h[k] = r.h[k - 1];
        if (k >= 0) r.h[k] = hooks[i];
    }
    plat_mutex_unlock(&lock);
    if (!r.n) return 0;   // removed since the scan: the function runs as it is
    r.at = *c;
    uint64_t x[9], v[8][2];
    regs_out(c, x, v);
    hook_level(&r, 0, x, v);
    for (int i = 19; i <= 30; i++) c->x[i] = r.at.x[i];
    for (int i = 8; i < 16; i++) c->v[i] = r.at.v[i];
    c->sp = r.at.sp;
    regs_in(c, x, v);
    rt_fpcr_sync(c);
    return 1;
}

// main.c fnprof_hit, for a function whose filter byte is set: 1 when hooks ran and the call is over.
int plugins_fn_enter(Cpu *c, uint64_t addr) {
    if (c->fn_pass == addr) {   // a hook's own call of the function it hooks
        c->fn_pass = 0;
        return 0;
    }
    int hit = 0;
    for (int i = 0; i < NX2_HOOKS_MAX && !hit; i++) hit = __atomic_load_n(&hooks[i].addr, __ATOMIC_RELAXED) == addr;
    if (!hit) return 0;         // another function of the filter's slot
    GuestFn body = rt_dispatch_lookup(addr);
    return body ? plugins_hook_enter(c, addr, body) : 0;
}

static int api_hook_function(uint64_t addr, Nx2HookFn fn, void *user) {
    if (!fn) return 0;
    char who[64], at[64];
    owner_name((const void *)fn, who, sizeof who);
    rt_guest_name(addr, at, sizeof at);
    if (!game_function(addr)) {
        rt_log("plugins: %s could not hook %s: no function of the game starts at this address", who, at);
        return 0;
    }
    plat_lib_pin((const void *)fn);
    int handle = 0;
    plat_mutex_lock(&lock);
    for (int i = 0; i < NX2_HOOKS_MAX && !handle; i++) {
        if (hooks[i].fn) continue;
        handle = next_handle++;
        if (next_handle <= 0) next_handle = 1;
        hooks[i].fn = fn, hooks[i].user = user, hooks[i].handle = handle;
        __atomic_store_n(&hooks[i].addr, addr, __ATOMIC_RELEASE);
        __atomic_add_fetch(&rt_fn_filter[RT_FN_SLOT(addr)], 1, __ATOMIC_RELEASE);
        if (!hook_count++) rt_fn_want(RT_FN_HOOKS, 1);
    }
    plat_mutex_unlock(&lock);
    if (handle) rt_log("plugins: %s hooked the function at %s", who, at);
    else rt_log("plugins: %s could not hook %s: all %d hooks are taken", who, at, NX2_HOOKS_MAX);
    return handle;
}

// Holding `lock`.
static void hook_remove_locked(int handle) {
    for (int i = 0; i < NX2_HOOKS_MAX; i++) {
        if (!hooks[i].fn || hooks[i].handle != handle) continue;
        uint64_t addr = hooks[i].addr;
        __atomic_store_n(&hooks[i].addr, 0, __ATOMIC_RELEASE);
        hooks[i].fn = NULL, hooks[i].user = NULL, hooks[i].handle = 0;
        __atomic_sub_fetch(&rt_fn_filter[RT_FN_SLOT(addr)], 1, __ATOMIC_RELEASE);
        if (!--hook_count) rt_fn_want(RT_FN_HOOKS, 0);
    }
}

static uint64_t api_call_original(Nx2HookCall *call) {
    HookLevel *l = (HookLevel *)call;   // pub is its first member
    if (!call || call->size != sizeof *call || l->magic != CALL_MAGIC || l->run->tid != (uint32_t)plat_thread_id()) {
        // not one of ours, or not on the thread and in the callback it belongs to; the magic of a call whose callback
        // has returned is cleared, as long as its stack is not written over
        refuse(__builtin_return_address(0), call && call->size == sizeof *call ? call->addr : 0,
               "call_original works only inside the hook callback the call belongs to");
        return 0;
    }
    hook_level(l->run, l->level + 1, call->x, call->v);
    l->ran = 1;
    return call->x[0];
}

// ---- title menu buttons
//
// A plugin's entry on the game's title menu (plugin_api.h add_title_button). Two ways, the first whenever the game's
// own menu allows it:
//
// The game's own entry (game 1.1.1). The title screen's layout holds four button parts, Pa_Button_00 to _03, and the
// screen makes a button control for each (its set-up, main+0x34067d0, counts the parts it finds). How many of them
// show is the frame the layout's animation "BtnNum" is stopped at: the game's sequence that opens the title stops it at
// its own number (3 with a save: continue, new game, settings) and sets the text of each used part's text id
// "Pa_Button_NN/T_Btn_00" from its message file. The fourth part is never used by the game. So with a button
// registered the runtime, in a hook of the screen's per-frame update (title_screen_hook, on the game's thread), stops
// "BtnNum" one frame further and sets the text of the first unused part to the plugin's label, both with the game's own
// functions. From there the entry is the game's: its font, its ornament and glow, its cursor, its sounds; the game's
// cursor code finds it by its place on the screen like the other three.
// Choosing it: the sequence waits for a button of the screen with a node (its update is main+0x32c8b60) that gets the
// chosen control and hands the control's part name to the graph, which knows the game's three names. A hook there
// (title_decide_hook) sees our control chosen, calls the plugin's callback and hands the graph an empty name, on which
// it goes back to waiting as it does for any name it does not know (or the settings entry's name, when the callback
// opened a menu page: plugin_menu.c).
// The layout has one spare part, so there is one such entry. With several buttons registered it stands for one of them
// at a time, written "< label >", and left and right on it go through them (plugins_title_poll).
//
// Drawn by the runtime (the way back). When the screen does not read as expected (another layout, no spare part, a
// function that does not answer), or with NX2NAVI_PLUGIN_TITLE=drawn in the environment, the buttons are drawn over the
// picture in the overlay pass (ui/plugin_draw.cpp title_buttons) under the game's entries, and the runtime takes the
// game's input while one of them is selected. title_route says which way holds; the log names it once per change.
//
// When the menu shows, for the drawn way. The title screen calls main+0x377af70 from its update while it is open, and
// the 32-bit word 0x380 bytes from the screen's start (in the block that follows it on the heap) says how far its start
// has come: 0x10000 while the logo fades in, 0x20000 once the entries show. The cursor: +0x188 of the screen points
// 0xf0 into the selected control, the controls are 0x228 bytes apart, and "down" on the game's last entry is free: the
// menu does not wrap. While a drawn button is selected the game reads no input (plugins_title_poll zeroes what input.c
// input_poll hands it), and the game's own highlight is taken off its last entry with the game's function for a cursor
// leaving a control (main+0xade540) and put back with the one for a cursor coming (main+0xa89150).
//
// The pointer, both ways: with keyboard and mouse as the device used last the pointer is free on the title menu
// (plugins_title_pointer, input.c input_cursor_allowed); over a plugin's entry it selects it, a left click chooses it
// and, on the game's own entry with several buttons, a right click goes to the next one.
//
// A drawn button's choice is queued and its callback runs at the next frame callbacks (plugins_frame); the game's own
// entry's callback runs in the hook that sees the choice. Both are on a thread of the game with guest calls allowed.
// Where all of that is in the running game: plugin_game_ui.c, the one table of the game's addresses and offsets.
static const PluginGameUi *G;     // set when the first button is added; the hooks run only after that

typedef struct { char label[96], desc[160]; int order, handle; Nx2TitleFn fn; void *user; } TitleBtn;
static TitleBtn title_btn[NX2_TITLE_BUTTONS_MAX];   // under `lock`, sorted by order, then handle
static int title_n;                                 // also read without the lock
static int title_hook;                              // the handle of title_menu_hook, 0 before the first button
static uint64_t title_seen;      // `frames` + 1 at the menu's last update with its entries shown, 0 never
static int title_last;           // 1 when the game's cursor was on its last entry at that update
static uint64_t title_entry;     // ... and that last entry, 0 when the menu's fields did not read as expected
static int title_dark;           // the game's last entry had its highlight taken away (the selection is ours)
static int title_sel = -1;       // the selected drawn button, -1 while the game's own list has the selection
static volatile int32_t title_ptr;   // the pointer is free for the buttons
static int title_chosen[8], title_nchosen;          // under `lock`: handles of the buttons chosen and not yet called

// Which way holds for the title screen that exists now: 0 not known yet (nothing shows), 1 the game's own entry, -1
// drawn. title_forced: NX2NAVI_PLUGIN_TITLE=drawn, or the probe.
static int title_route, title_forced;
// The game's own entry. The screen's fields are touched in the hooks, on the game's threads; what the input thread
// reads of them is the atomics below.
static struct {
    uint64_t screen, layout, anim, control;   // the screen this is about, its layout, "BtnNum" and our part's control
    int max;                     // frames "BtnNum" has: the most parts it shows
    int game;                    // the frame the game stopped it at: its own entries
    int on;                      // our entry shows: "BtnNum" is at game + 1 and the part has our text
    int gen;                     // title_gen the part's text was made from
    int said;                    // the log line for this screen is written
} tr;
static int title_cur;            // under `lock`: the button the game's own entry stands for
static int title_gen = 1;        // counts the changes of what the entry's text is made from
static uint64_t title_real_seen; // `frames` + 1 at the screen's last update with our entry on and the menu open
static int title_real_sel;       // ... the game's cursor was on our entry then
static uint32_t title_real_y;    // ... our entry's centre line as a fraction of the picture (a float's bits), 0 unknown
static int title_focus_req;      // the pointer came over our entry: the cursor is to go there
static char title_text[2][512];  // the entry's text as the game reads it; the game keeps the pointer, so a new text
static int title_text_at;        // goes to the other one

static void title_route_set(int route, const char *why) {
    if (__atomic_exchange_n(&title_route, route, __ATOMIC_RELEASE) == route && tr.said) return;
    tr.said = 1;
    if (route > 0) rt_log("plugins: title menu button: the game's own entry Pa_Button_%02d (%s)", tr.game, why);
    else if (route < 0) rt_log("plugins: title menu button: drawn by the runtime (%s)", why);
}

static int title_drawn(void) { return __atomic_load_n(&title_route, __ATOMIC_ACQUIRE) < 0; }

static void title_menu_hook(Nx2HookCall *call, void *user) {
    (void)user;
    uint64_t m = call->x[0];
    if (!title_drawn() || !span_ok(m, G->tm_size, 0)) return;
    uint32_t state, count;
    uint64_t cursor, entries;
    memcpy(&state, (const void *)(uintptr_t)(m + G->tm_state), 4);
    memcpy(&count, (const void *)(uintptr_t)(m + G->tm_count), 4);
    memcpy(&cursor, (const void *)(uintptr_t)(m + G->ts_cursor), 8);
    memcpy(&entries, (const void *)(uintptr_t)(m + G->tm_entries), 8);
    if (state != G->tm_shown) return;
    uint64_t off = cursor - (entries + G->tm_entry0);
    int last = count >= 1 && count <= 16 && off % G->ctl_size == 0 && off / G->ctl_size == count - 1;
    __atomic_store_n(&title_last, last, __ATOMIC_RELAXED);
    __atomic_store_n(&title_entry, last ? cursor - G->tm_entry0 - 8 : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&title_seen, __atomic_load_n(&frames, __ATOMIC_RELAXED) + 1, __ATOMIC_RELEASE);
}

// The game's title menu shows its entries now (the drawn way): its update ran with them shown within the last few
// presented frames.
static int title_shown(void) {
    uint64_t seen = __atomic_load_n(&title_seen, __ATOMIC_ACQUIRE);
    return title_drawn() && seen && __atomic_load_n(&frames, __ATOMIC_RELAXED) + 1 - seen <= 4;
}

// The same for the game's own entry: the screen's update ran with our entry on and the menu open.
static int title_real_shown(void) {
    uint64_t seen = __atomic_load_n(&title_real_seen, __ATOMIC_ACQUIRE);
    return __atomic_load_n(&title_route, __ATOMIC_ACQUIRE) > 0 && seen && __atomic_load_n(&frames, __ATOMIC_RELAXED) + 1 - seen <= 4;
}

static uint64_t g64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }
static uint32_t g32(uint64_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static float gf32(uint64_t a) { float v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }

// The entry's text as the game's message format has it: UTF-8, with a tag for the size of what follows (the byte 0x0E,
// then 16-bit words: group 0, type 2, 2 bytes of parameters, the size in percent), which is how the game writes the
// remark after its own "settings" entry. One button: its label, and its description smaller after it. Several: the
// current one between "<" and ">". Bytes under 0x20 in a plugin's text become spaces: they would read as tags.
// Returns the bytes written, without the terminator.
int plugins_title_text(char *out, size_t cap, const char *label, const char *desc, int several) {
    static const char small[] = {0x0e, 0, 0, 2, 0, 2, 0, 80, 0}, full[] = {0x0e, 0, 0, 2, 0, 2, 0, 100, 0};
    size_t n = 0;
#define PUT(p_, k_) do { size_t k = (k_); if (n + k < cap) memcpy(out + n, (p_), k), n += k; } while (0)
#define PUTS(s_) do { for (const char *q = (s_); *q; q++) { char ch = (unsigned char)*q < 0x20 ? ' ' : *q; PUT(&ch, 1); } } while (0)
    if (several) PUT("<  ", 3);
    PUTS(label);
    if (desc && *desc) {
        PUT(small, sizeof small);
        PUT(" ", 1);
        PUTS(desc);
        if (several) PUT(full, sizeof full);
    }
    if (several) PUT("  >", 3);
#undef PUTS
#undef PUT
    out[n] = 0;
    return (int)n;
}

// The screen's hooks' guest calls: on the calling guest thread, inside the hook (plugins_hook_enter puts the registers
// back before the hooked function runs).
static uint64_t title_call(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    return rt_call(c, api_game_info()->main_base + fn, 5, a0, a1, a2, a3, (uint64_t)0);
}

static void title_anim_stop(Cpu *c, uint64_t anim, int frame) {
    float f = (float)frame;
    uint32_t bits;
    memcpy(&bits, &f, 4);
    c->x[0] = anim;
    c->v[0] = bits;
    rt_call(c, api_game_info()->main_base + G->anim_stop_at, 0);   // no arguments given: x0 and v0 as set here
}

// The frame "BtnNum" is at; -1 when the animation does not read as one.
static int title_anim_frame(uint64_t anim, int *frames_max) {
    if (!span_ok(anim, 0x28, 0)) return -1;
    uint64_t state = g64(anim + G->anim_state);
    if (!span_ok(state, 0x20, 0)) return -1;
    uint64_t res = g64(state);
    if (!span_ok(res, 0x30, 0)) return -1;
    float f = gf32(state + G->anim_frame);
    *frames_max = (int)g32(res + G->anim_res_frames);
    return f >= 0 && f < 64 ? (int)f : -1;
}

// Sets our part's text from the buttons as they are. 0 when the layout has no text pane for it.
static int title_set_text(Cpu *c, int part) {
    static char id[40];          // the game compares it with the layout's text ids: guest code reads it
    static struct { const char *text; int32_t bytes, pad; int64_t message; } ref;
    char label[96], desc[160];
    plat_mutex_lock(&lock);
    int n = title_n, cur = title_cur < n ? title_cur : 0, gen = title_gen;
    if (n) memcpy(label, title_btn[cur].label, sizeof label), memcpy(desc, title_btn[cur].desc, sizeof desc);
    plat_mutex_unlock(&lock);
    if (!n) return 1;
    char *text = title_text[title_text_at ^= 1];
    ref.text = text, ref.bytes = plugins_title_text(text, sizeof title_text[0], label, desc, n > 1), ref.message = -1;
    snprintf(id, sizeof id, "Pa_Button_%02d/T_Btn_00", part);
    tr.gen = gen;
    return title_call(c, G->layout_set_text, tr.layout, (uint64_t)(uintptr_t)id, (uint64_t)(uintptr_t)&ref, 1) != 0;
}

// The title screen is set up anew (the game starts, or comes back to the title): what was known of it is gone.
static void title_setup_hook(Nx2HookCall *call, void *user) {
    (void)call, (void)user;
    memset(&tr, 0, sizeof tr);
    __atomic_store_n(&title_real_seen, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&title_real_sel, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&title_route, __atomic_load_n(&title_forced, __ATOMIC_RELAXED) ? -1 : 0, __ATOMIC_RELEASE);
}

// The title screen's update, before it runs: keeps our entry as the registered buttons ask, and notes for the input
// thread whether the menu is open, whether the cursor is on our entry and where the entry lies.
static void title_screen_hook(Nx2HookCall *call, void *user) {
    (void)user;
    static const char btn_num[] = "BtnNum";
    uint64_t s = call->x[0], base = api_game_info()->main_base;
    int n = __atomic_load_n(&title_n, __ATOMIC_ACQUIRE);
    if ((!n && !tr.on) || (title_drawn() && tr.screen == s)) return;
    Cpu *c = kern_cpu();
    if (__atomic_load_n(&title_forced, __ATOMIC_RELAXED) || !c) {
        tr.screen = s;
        title_route_set(-1, c ? "asked for with NX2NAVI_PLUGIN_TITLE" : "not on a thread of the game");
        return;
    }
    if (!span_ok(s, G->ts_size, 0) || g64(s) != base + G->title_vtable) {
        tr.screen = s;
        title_route_set(-1, "the title screen is not the class the runtime knows");
        return;
    }
    uint64_t layout = g64(s + G->ts_layout), controls = g64(s + G->ts_controls);
    uint32_t parts = g32(s + G->ts_parts);
    if (tr.screen != s || tr.layout != layout) {
        memset(&tr, 0, sizeof tr);
        tr.screen = s, tr.layout = layout;
        __atomic_store_n(&title_route, 0, __ATOMIC_RELEASE);
        if (parts < 2 || parts > 16 || !span_ok(controls, 8 * parts, 0) || !span_ok(layout, 0x98, 0)) {
            title_route_set(-1, "the title screen's button parts do not read as expected");
            return;
        }
        tr.anim = title_call(c, G->layout_anim, layout, (uint64_t)(uintptr_t)btn_num, 0, 0);
        if (title_anim_frame(tr.anim, &tr.max) < 0) {
            title_route_set(-1, "the title layout has no animation BtnNum");
            return;
        }
    }
    int frame = title_anim_frame(tr.anim, &tr.max);
    if (tr.on && frame != tr.game + 1) tr.on = 0;   // the game stopped it at its own number again
    if (!tr.on) {
        __atomic_store_n(&title_real_sel, 0, __ATOMIC_RELAXED);
        if (!n || frame < 1) return;                // no button, or the game has not laid its menu out yet
        if ((uint32_t)frame >= parts || frame + 1 > tr.max || !span_ok(g64(controls + 8 * (uint64_t)frame), G->ctl_size, 0)) {
            title_route_set(-1, "the title menu has no unused entry");
            return;
        }
        tr.game = frame;
        if (!title_set_text(c, frame)) {
            title_route_set(-1, "the unused entry has no text pane");
            return;
        }
        title_anim_stop(c, tr.anim, frame + 1);
        tr.control = g64(controls + 8 * (uint64_t)frame);
        tr.on = 1;
        title_route_set(1, "the layout's first unused button part");
    } else if (!n) {                                // the last button was removed: the menu as the game made it
        if (g64(s + G->ts_cursor) == tr.control + G->ctl_item)
            title_call(c, G->screen_focus, s, g64(controls + 8 * (uint64_t)(tr.game - 1)), 0, 0);
        title_anim_stop(c, tr.anim, tr.game);
        tr.on = 0;
        __atomic_store_n(&title_real_seen, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&title_real_sel, 0, __ATOMIC_RELAXED);
        return;
    } else if (tr.gen != __atomic_load_n(&title_gen, __ATOMIC_RELAXED)) {
        title_set_text(c, tr.game);
    }
    const uint8_t *m = (const uint8_t *)(uintptr_t)s;
    int open = m[G->ts_open] == 1 && m[G->ts_menu_in], sel = g64(s + G->ts_cursor) == tr.control + G->ctl_item;
    if (open && !sel && __atomic_exchange_n(&title_focus_req, 0, __ATOMIC_RELAXED))
        title_call(c, G->screen_focus, s, tr.control, 0, 0);
    uint64_t pane = g64(tr.control + G->ctl_pane);
    float y = span_ok(pane, 0x60, 0) ? gf32(pane + G->pane_y) : 0, cy = y != 0 ? 0.5f - y / G->layout_height : 0;
    uint32_t bits;
    memcpy(&bits, &cy, 4);
    __atomic_store_n(&title_real_y, cy > 0 && cy < 1 ? bits : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&title_real_sel, open && sel, __ATOMIC_RELAXED);
    if (open) __atomic_store_n(&title_real_seen, __atomic_load_n(&frames, __ATOMIC_RELAXED) + 1, __ATOMIC_RELEASE);
}

static void title_choose(int i);
static int title_choosing;        // a title button's callback runs now, from the game's own entry

// The update of the sequence's node that waits for a button of a screen, before it runs. Once the screen has answered
// (the node is no longer queued there) the node holds the chosen control and its part's name, and its update hands the
// name on and ends the node. Our control: the plugin's callback is queued and the name becomes empty.
static void title_decide_hook(Nx2HookCall *call, void *user) {
    (void)user;
    uint64_t node = call->x[0];
    if (!tr.on || !tr.control || !span_ok(node, G->node_size, 0)) return;
    if (g64(node + G->node_queued) || g64(node + G->node_control) != tr.control || g32(node + G->node_flags) >> 30 == 2) return;
    // The plugin's callback, here and now: on the game's thread, inside the hook, where guest calls work. When it asked
    // for a menu page (open_menu, plugin_menu.c) the sequence gets the name of the game's own settings entry, the last
    // of the game's entries, and opens its settings screen, which plugin_menu.c fills; else an empty name.
    Nx2TitleFn fn = NULL;
    void *fn_user = NULL;
    plat_mutex_lock(&lock);
    int cur = title_cur < title_n ? title_cur : 0;
    if (title_n) fn = title_btn[cur].fn, fn_user = title_btn[cur].user;
    plat_mutex_unlock(&lock);
    // (not when plugin_menu.c itself pressed the entry, to open the page that replaces the one it just closed)
    __atomic_store_n(&title_choosing, 1, __ATOMIC_RELEASE);
    if (fn && !plugin_menu_reopening()) fn(fn_user);
    __atomic_store_n(&title_choosing, 0, __ATOMIC_RELEASE);
    uint64_t name = g64(node + G->node_name);
    if (!span_ok(name, 16, 1)) return;
    if (plugin_menu_armed()) snprintf((char *)(uintptr_t)name, 16, "Pa_Button_%02d", tr.game - 1);
    else *(char *)(uintptr_t)name = 0;
}

int plugins_title_choosing(void) { return __atomic_load_n(&title_choosing, __ATOMIC_ACQUIRE); }

static int api_add_title_button(const Nx2TitleButton *b) {
    if (!b || !NX2_HAS(b, Nx2TitleButton, user) || !b->label || !*b->label || !b->on_select) return 0;
    char who[64];
    owner_name((const void *)b->on_select, who, sizeof who);
    if (!G) G = plugin_game_ui();
    uint64_t base = api_game_info()->main_base, at = G ? base + G->title_menu_update : 0;
    if (!G || !game_function(at)) {
        rt_log("plugins: %s could not add the title menu button \"%s\": the title menu of this game version is not known "
               "to the runtime", who, b->label);
        return 0;
    }
    plat_lib_pin((const void *)b->on_select);
    int hook = __atomic_load_n(&title_hook, __ATOMIC_ACQUIRE);
    if (!hook) {   // the first button: watch the menu from now on (two threads at once register two hooks, harmlessly)
        hook = api_hook_function(at, title_menu_hook, NULL);
        if (!hook) return 0;
        char e[16];
        int drawn = plat_getenv("NX2NAVI_PLUGIN_TITLE", e, sizeof e) && !strcmp(e, "drawn");
        // the game's own entry needs every one of its hooks; without one the buttons are drawn
        if (!drawn)
            drawn = !api_hook_function(base + G->title_setup, title_setup_hook, NULL) ||
                    !api_hook_function(base + G->title_update, title_screen_hook, NULL) ||
                    !api_hook_function(base + G->wait_button_update, title_decide_hook, NULL);
        if (drawn) {
            __atomic_store_n(&title_forced, 1, __ATOMIC_RELEASE);
            title_route_set(-1, "NX2NAVI_PLUGIN_TITLE=drawn, or a function of the title screen could not be hooked");
        }
        __atomic_store_n(&title_hook, hook, __ATOMIC_RELEASE);
    }
    int handle = 0;
    plat_mutex_lock(&lock);
    if (title_n < NX2_TITLE_BUTTONS_MAX) {
        handle = next_handle++;
        if (next_handle <= 0) next_handle = 1;
        int k = title_n;
        for (; k > 0 && title_btn[k - 1].order > b->order; k--) title_btn[k] = title_btn[k - 1];
        TitleBtn *t = &title_btn[k];
        *t = (TitleBtn){.order = b->order, .handle = handle, .fn = b->on_select, .user = b->user};
        snprintf(t->label, sizeof t->label, "%s", b->label);
        snprintf(t->desc, sizeof t->desc, "%s", b->description ? b->description : "");
        __atomic_store_n(&title_n, title_n + 1, __ATOMIC_RELEASE);
        __atomic_store_n(&title_sel, -1, __ATOMIC_RELAXED);
        title_cur = 0;
        __atomic_add_fetch(&title_gen, 1, __ATOMIC_RELAXED);
    }
    plat_mutex_unlock(&lock);
    if (handle) rt_log("plugins: %s added the title menu button \"%s\"", who, b->label);
    else rt_log("plugins: %s could not add the title menu button \"%s\": all %d are taken", who, b->label, NX2_TITLE_BUTTONS_MAX);
    return handle;
}

// Holding `lock`.
static void title_remove_locked(int handle) {
    for (int i = 0; i < title_n; i++) {
        if (title_btn[i].handle != handle) continue;
        for (int k = i; k + 1 < title_n; k++) title_btn[k] = title_btn[k + 1];
        __atomic_store_n(&title_n, title_n - 1, __ATOMIC_RELEASE);
        __atomic_store_n(&title_sel, -1, __ATOMIC_RELAXED);
        title_cur = 0;
        __atomic_add_fetch(&title_gen, 1, __ATOMIC_RELAXED);
        return;
    }
}

// Where drawn button i of n lies on the game's picture, as fractions of it: its centre line, half its height and the
// height of its text. The game's three entries are 0.0556 of the picture's height apart, the last at 0.869; ours go on
// under it at that distance when there is one, closer and smaller when there are more, so that the last one's band ends
// on the picture.
static void title_place(int i, int n, float *cy, float *half, float *text) {
    float fit = 0.127f / ((float)n + 0.5f), pitch = fit < 0.0556f ? fit : 0.0556f;
    *cy = 0.869f + pitch * (float)(i + 1);
    *half = pitch * 0.5f;
    *text = pitch * 0.52f < 0.0285f ? pitch * 0.52f : 0.0285f;
}
#define TITLE_HALF_WIDTH 0.13f   // of the picture's width, around its middle: what the pointer hits
#define TITLE_HALF_HEIGHT 0.0278f   // half the distance between two of the game's entries, of the picture's height

static void title_choose(int i) {
    plat_mutex_lock(&lock);
    if (i >= 0 && i < title_n && title_nchosen < (int)(sizeof title_chosen / sizeof *title_chosen))
        title_chosen[title_nchosen++] = title_btn[i].handle;
    plat_mutex_unlock(&lock);
}

int plugins_title_pointer(void) { return __atomic_load_n(&title_ptr, __ATOMIC_RELAXED); }

static void title_pointer(int on) {
    if (plat_xchg32(&title_ptr, on) == on) return;
    ikb_cursor_update();
    if (on) ikb_pointer_set(0.5f, 0.6f);   // the pointer appears over the picture, clear of the entries
}

// The entry stands for the next (step 1) or the previous (step -1) button.
static void title_cycle(int step) {
    plat_mutex_lock(&lock);
    if (title_n > 1) {
        title_cur = (title_cur + step + title_n) % title_n;
        __atomic_add_fetch(&title_gen, 1, __ATOMIC_RELAXED);
    }
    plat_mutex_unlock(&lock);
}

// The game's own entry: the game moves its cursor and takes the confirm press itself. Left here are left and right on
// the entry, which go through the buttons when there are several, and the pointer.
static void title_poll_real(uint64_t *buttons, int32_t lx, int n) {
    static uint64_t prev;
    static int held, held_r, click;
    static float last_x = -1, last_y = -1;
    uint64_t now = *buttons | (lx < -16384 ? 1ull << B_LEFT : 0) | (lx > 16384 ? 1ull << B_RIGHT : 0), edge = now & ~prev;
    prev = now;
    int shown = n && title_real_shown(), sel = shown && __atomic_load_n(&title_real_sel, __ATOMIC_RELAXED);
    int pointer = shown && ikb_foreground() && input_kbm_last();
    title_pointer(pointer);
    if (!shown) {
        click = 0;
        return;
    }
    if (sel && (edge & (1ull << B_LEFT | 1ull << B_RIGHT))) title_cycle(edge & 1ull << B_RIGHT ? 1 : -1);
    if (pointer) {
        float nx, ny, r[4], cy;
        uint32_t bits = __atomic_load_n(&title_real_y, __ATOMIC_RELAXED);
        memcpy(&cy, &bits, 4);
        int left = ikb_down(KEY_MOUSE_L), right = ikb_down(KEY_MOUSE_R);
        if (bits && ikb_pointer(&nx, &ny) && gfx_picture_rect(r) && r[2] > 0 && r[3] > 0) {
            float px = (nx - r[0]) / r[2], py = (ny - r[1]) / r[3];
            int hit = px > 0.5f - TITLE_HALF_WIDTH && px < 0.5f + TITLE_HALF_WIDTH && py >= cy - TITLE_HALF_HEIGHT &&
                      py < cy + TITLE_HALF_HEIGHT;
            int moved = nx != last_x || ny != last_y;
            last_x = nx, last_y = ny;
            if (hit && !sel && (moved || (left && !held))) __atomic_store_n(&title_focus_req, 1, __ATOMIC_RELAXED);
            if (hit && left && !held) click = 36;          // chosen once the cursor is there, within a second
            if (hit && right && !held_r) title_cycle(1);
        }
        held = left, held_r = right;
    }
    // a click is the game's own confirm press on the entry, so that the game answers it as it answers the button
    if (click > 6 && sel) click = 6;
    if (click > 0 && click-- <= 6 && sel) *buttons |= 1ull << B_A;
}

void plugins_title_poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    static uint64_t prev;
    static int release, by_pointer, held;
    static float last_x = -1, last_y = -1;
    int n = __atomic_load_n(&title_n, __ATOMIC_ACQUIRE);
    plugin_menu_poll(buttons);   // plugin_menu.c: close_menu is the game's "back" on a plugin's menu page
    plugin_camera_poll(buttons); // plugin_camera.c: camera_reset is the game's camera button, pressed for a moment
    if (!n && !plugins_title_pointer()) return;
    if (!title_drawn()) {
        title_poll_real(buttons, *lx, n);
        return;
    }
    int shown = n && title_shown();
    int sel = __atomic_load_n(&title_sel, __ATOMIC_RELAXED);
    // the left stick past half way moves the selection as the direction buttons do
    uint64_t now = *buttons | (*ly < -16384 ? 1ull << B_DOWN : 0) | (*ly > 16384 ? 1ull << B_UP : 0);
    uint64_t edge = now & ~prev;
    prev = now;
    int pointer = shown && ikb_foreground() && input_kbm_last();
    title_pointer(pointer);
    if (!shown) {
        release = by_pointer = 0;
        if (sel >= 0) __atomic_store_n(&title_sel, -1, __ATOMIC_RELAXED);
        return;
    }
    if (sel >= n) sel = n - 1;
    if (pointer) {
        float nx, ny, r[4];
        int click = ikb_down(KEY_MOUSE_L), hit = -1;
        if (ikb_pointer(&nx, &ny) && gfx_picture_rect(r) && r[2] > 0 && r[3] > 0) {
            float px = (nx - r[0]) / r[2], py = (ny - r[1]) / r[3];
            for (int i = 0; i < n; i++) {
                float cy, half, text;
                title_place(i, n, &cy, &half, &text);
                if (px > 0.5f - TITLE_HALF_WIDTH && px < 0.5f + TITLE_HALF_WIDTH && py >= cy - half && py < cy + half) hit = i;
            }
            int moved = nx != last_x || ny != last_y;
            last_x = nx, last_y = ny;
            if (hit >= 0 && (moved || (click && !held))) sel = hit, by_pointer = 1;
            else if (hit < 0 && moved && by_pointer) sel = -1, by_pointer = 0;
            if (click && !held && hit >= 0) title_choose(hit);
        }
        held = click;
    }
    if (release) {   // the selection went back to the game's list: its input comes back once everything is let go
        if (!*buttons && !*lx && !*ly && !*rx && !*ry) release = 0;
    } else if (sel < 0) {
        if ((edge & 1ull << B_DOWN) && __atomic_load_n(&title_last, __ATOMIC_RELAXED)) sel = 0, by_pointer = 0;
    } else if (edge & 1ull << B_DOWN) {
        sel = sel + 1 < n ? sel + 1 : sel, by_pointer = 0;
    } else if (edge & 1ull << B_UP) {
        if (--sel < 0) release = 1;
        by_pointer = 0;
    } else if (edge & 1ull << B_A) {
        title_choose(sel);
    }
    __atomic_store_n(&title_sel, sel, __ATOMIC_RELAXED);
    if (sel >= 0 || release) *buttons = 0, *lx = *ly = *rx = *ry = 0;
}

int plugins_title_buttons(int max, const char **label, const char **desc, float *cy, float *half, float *text, int *selected) {
    static char labels[NX2_TITLE_BUTTONS_MAX][96], descs[NX2_TITLE_BUTTONS_MAX][160];   // the presenting thread's copy
    if (!__atomic_load_n(&title_n, __ATOMIC_ACQUIRE) || !title_shown()) return 0;
    plat_mutex_lock(&lock);
    int n = title_n < max ? title_n : max;
    for (int i = 0; i < n; i++) {
        memcpy(labels[i], title_btn[i].label, sizeof labels[i]);
        memcpy(descs[i], title_btn[i].desc, sizeof descs[i]);
        label[i] = labels[i], desc[i] = descs[i];
        title_place(i, n, &cy[i], &half[i], &text[i]);
    }
    plat_mutex_unlock(&lock);
    *selected = __atomic_load_n(&title_sel, __ATOMIC_RELAXED);
    return n;
}

// The probe, and a build that wants the drawn buttons whatever the game's menu holds.
void plugins_title_force_drawn(void) {
    __atomic_store_n(&title_forced, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&title_route, -1, __ATOMIC_RELEASE);
}

// The probe: the node's hook with a stand-in control and no function behind it, as if our entry showed.
static void title_no_body(Cpu *c) { (void)c; }
void plugins_title_probe_decide(Cpu *c, uint64_t node, uint64_t control) {
    tr.on = 1, tr.control = control, tr.game = 3;
    c->x[0] = node;
    plugins_hook_enter(c, api_game_info()->main_base + G->wait_button_update, title_no_body);
    tr.on = 0, tr.control = 0;
}

// plugins_frame, on the game's thread between two frames: the highlight of the game's last entry follows the selection
// (the drawn way).
static int title_highlight_due(void) {
    if (!title_shown()) {   // the menu went (a save loads, the title starts again): its entries are made anew
        title_dark = 0;
        return 0;
    }
    int dark = __atomic_load_n(&title_sel, __ATOMIC_RELAXED) >= 0;
    return dark != title_dark && __atomic_load_n(&title_entry, __ATOMIC_RELAXED);
}

static void title_game_highlight(Cpu *c) {
    if (!title_highlight_due()) return;
    uint64_t entry = __atomic_load_n(&title_entry, __ATOMIC_RELAXED), base = api_game_info()->main_base;
    title_dark = !title_dark;
    if (kern_cpu() != c) return;   // not a thread of the game (the probe): nothing to call
    rt_call(c, base + (title_dark ? G->entry_unfocus : G->entry_focus), 1, entry);
}

// The buttons chosen since the last frame, called as frame callbacks are: plugins_frame, with guest calls allowed.
static void title_run_chosen(void) {
    int handles[sizeof title_chosen / sizeof *title_chosen], n;
    plat_mutex_lock(&lock);
    n = title_nchosen, title_nchosen = 0;
    memcpy(handles, title_chosen, sizeof handles);
    plat_mutex_unlock(&lock);
    for (int i = 0; i < n; i++) {
        Nx2TitleFn fn = NULL;
        void *user = NULL;
        plat_mutex_lock(&lock);
        for (int k = 0; k < title_n; k++)
            if (title_btn[k].handle == handles[i]) fn = title_btn[k].fn, user = title_btn[k].user;
        plat_mutex_unlock(&lock);
        if (fn) fn(user);
    }
}

// ---- the game's buttons as events (on_pad)

void plugins_pad(uint64_t buttons, int32_t lx, int32_t ly) {
    static uint64_t prev;
    if (!__atomic_load_n(&counts[KIND_PAD], __ATOMIC_ACQUIRE)) { prev = 0; return; }
    uint64_t now = (buttons & 0xffff) | (lx < -16384 ? 1ull << B_LEFT : 0) | (lx > 16384 ? 1ull << B_RIGHT : 0) |
                   (ly < -16384 ? 1ull << B_DOWN : 0) | (ly > 16384 ? 1ull << B_UP : 0);
    uint64_t changed = now ^ prev;
    prev = now;
    if (!changed) return;
    Slot s[MAX_CB];
    int n = snapshot(KIND_PAD, s);
    for (int b = 0; b < 16; b++) {
        if (!(changed >> b & 1)) continue;
        Nx2PadEvent e = {.size = sizeof e, .button = (uint32_t)b, .down = (uint8_t)(now >> b & 1)};
        // no gamepad holds it: a key bound to the button. NX2NAVI_PLUGIN_SCRIPT_PAD=1 (for tests): the presses of an
        // input script, which no gamepad holds either, count as a gamepad's.
        static int script_pad = -1;
        if (script_pad < 0) {
            char v[8];
            script_pad = plat_getenv("NX2NAVI_PLUGIN_SCRIPT_PAD", v, sizeof v) && v[0] == '1';
        }
        e.keyboard = e.down && !script_pad && !(input_pad_bits() >> b & 1);
        for (int i = 0; i < n; i++) ((Nx2PadFn)s[i].fn)(&e, s[i].user);
    }
}

// ---- input

static void api_capture_input(int on) {
    plat_mutex_lock(&lock);
    int was = capture;
    capture += on ? 1 : capture > 0 ? -1 : 0;
    int now = capture;
    plat_mutex_unlock(&lock);
    if (!was != !now) input_plugin(now != 0);
}

int plugins_capturing(void) { return __atomic_load_n(&capture, __ATOMIC_RELAXED) > 0; }

int plugins_key(unsigned key, int down, int repeat) {
    Slot s[MAX_CB];
    int n = snapshot(KIND_KEY, s), kept = 0;
    Nx2KeyEvent e = {.size = sizeof e, .key = key, .down = down != 0, .repeat = repeat != 0};
    for (int i = 0; i < n; i++) kept |= ((Nx2KeyFn)s[i].fn)(&e, s[i].user) != 0;
    return kept;
}

void plugins_text(unsigned codepoint) {
    if (!plugins_capturing()) return;
    Slot s[MAX_CB];
    int n = snapshot(KIND_TEXT, s);
    for (int i = 0; i < n; i++) ((Nx2TextFn)s[i].fn)(codepoint, s[i].user);
}

// ---- frames and the overlay

void plugins_frame(Cpu *c) {
    uint64_t index = __atomic_fetch_add(&frames, 1, __ATOMIC_RELAXED);
    Slot s[MAX_CB];
    int n = snapshot(KIND_FRAME, s), chosen = __atomic_load_n(&title_nchosen, __ATOMIC_RELAXED);
    int highlight = __atomic_load_n(&title_n, __ATOMIC_RELAXED) && title_highlight_due();
    if (!n && !chosen && !highlight) return;
    Cpu keep = *c;   // the guest's registers as at the present call: the callbacks' guest calls leave no trace in them
    if (highlight) title_game_highlight(c);
    __atomic_store_n(&frame_cpu, c, __ATOMIC_RELEASE);
    if (chosen) title_run_chosen();
    Nx2Frame f = {.size = sizeof f, .index = index};
    for (int i = 0; i < n; i++) ((Nx2FrameFn)s[i].fn)(&f, s[i].user);
    __atomic_store_n(&frame_cpu, NULL, __ATOMIC_RELEASE);
    memcpy(c->x, keep.x, sizeof c->x);
    memcpy(c->v, keep.v, sizeof c->v);
    c->nzcv = keep.nzcv, c->fpsr = keep.fpsr;
    rt_fpcr_sync(c);   // a plugin's own code may have changed the host's rounding and denormal modes
}

// The orderly ways out of the process (gfx_present.c gfx_window_closed, svc.c ExitProcess, boot.c rt_frame_presented),
// before the saves are flushed and the process ends: the exit callbacks, once.
void plugins_exit(void) {
    static int done;
    if (__atomic_exchange_n(&done, 1, __ATOMIC_SEQ_CST)) return;
    Slot s[MAX_CB];
    int n = snapshot(KIND_EXIT, s);
    if (n) rt_log("plugins: the game ends: %d exit callback%s", n, n == 1 ? "" : "s");
    for (int i = 0; i < n; i++) ((Nx2ExitFn)s[i].fn)(s[i].user);
    plugin_steam_shutdown();   // plugin_steam.c: a room a plugin left open is left, Steam signed out of
}

int plugins_overlay_wanted(void) {
    return __atomic_load_n(&counts[KIND_OVERLAY], __ATOMIC_RELAXED) > 0 || (__atomic_load_n(&title_n, __ATOMIC_RELAXED) && title_shown());
}

void plugins_overlay(Nx2Draw *draw) {
    Slot s[MAX_CB];
    int n = snapshot(KIND_OVERLAY, s);
    uint64_t f = __atomic_load_n(&frames, __ATOMIC_RELAXED);
    draw->frame = f ? f - 1 : 0;
    plugin_camera_overlay(draw);
    for (int i = 0; i < n; i++) ((Nx2OverlayFn)s[i].fn)(draw, s[i].user);
}

// ---- the table

// plugin_api.h restart: the variables go into this process's environment, which the new process inherits
// (gfx_present.c gfx_window_closed, plat_restart), then the window is asked to close for a restart, as the settings
// menu's own restart does.
void gfx_window_request_restart(void);   // gfx_present.c
static int api_restart(const char *const *env) {
    enum { MAX_VARS = 64 };
    int n = 0;
    for (; env && env[n]; n++) {
        const char *eq = strchr(env[n], '=');
        if (!eq || eq == env[n] || eq - env[n] >= 128 || n >= MAX_VARS) {
            rt_log("plugins: restart refused: \"%.80s\" is not NAME=value (or more than %d variables)", env[n], MAX_VARS);
            return 0;
        }
    }
    for (int i = 0; i < n; i++) {
        char name[128];
        const char *eq = strchr(env[i], '=');
        memcpy(name, env[i], (size_t)(eq - env[i])), name[eq - env[i]] = 0;
        plat_setenv(name, eq[1] ? eq + 1 : NULL);
    }
    char who[64];
    owner_name(__builtin_return_address(0), who, sizeof who);
    rt_log("plugins: %s asked for a restart of the game with %d variables", who, n);
    gfx_window_request_restart();
    return 1;
}

// plugin_api.h set_save_dir: fs_host.c does it or says why not.
static int api_set_save_dir(const char *folder) {
    char who[64];
    const char *why = "";
    owner_name(__builtin_return_address(0), who, sizeof who);
    int ok = fs_save_dir_set(folder, &why);
    if (ok) api_game_info();   // the description takes the new name
    else rt_log("plugins: set_save_dir of %s refused: %s", who, why);
    return ok;
}

static const Nx2PluginApi api = {
    .size = sizeof(Nx2PluginApi),
    .version = NX2_PLUGIN_API_VERSION,
    .game_info = api_game_info,
    .log = api_log,
    .read = api_read,
    .write = api_write,
    .on_frame = api_on_frame,
    .on_overlay = api_on_overlay,
    .on_key = api_on_key,
    .on_text = api_on_text,
    .remove = api_remove,
    .guest_call = api_guest_call,
    .guest_call_regs = api_guest_call_regs,
    .capture_input = api_capture_input,
    .hook_function = api_hook_function,
    .call_original = api_call_original,
    .map_range = api_map_range,
    .on_exit = api_on_exit,
    .add_title_button = api_add_title_button,
    .on_pad = api_on_pad,
    .open_menu = plugin_menu_open,
    .update_menu = plugin_menu_update,
    .close_menu = plugin_menu_close,
    .camera = plugin_camera_get,
    .world_to_screen = plugin_camera_to_screen,
    .restart = api_restart,
    .camera_reset = plugin_camera_reset,
    .set_save_dir = api_set_save_dir,
    .texture_create = plugin_texture_create,
    .texture_load = plugin_texture_load,
    .texture_free = plugin_texture_free,
    .font_load = plugin_font_load,
    .font_free = plugin_font_free,
    .steam = plugin_steam,
};

// The executable's export (NX2_PLUGIN_API_EXPORT). Every version from 1, the first, up to this runtime's own is served
// with the one table, which only grows at its end: a version 1 plugin reads the part it knows.
__attribute__((used)) const Nx2PluginApi *nx2navi_plugin_api(uint32_t version) {
    return version >= 1 && version <= NX2_PLUGIN_API_VERSION ? &api : NULL;
}

// ---- the libraries the game loads itself

// NX2NAVI_MOD_DLLS: library paths separated by '|', a relative one under the game's folder. The launcher sets it to the
// libraries it also loads into the suspended process (launcher/modext.py game_env); opening one of those again gives the
// same library, and a game the launcher did not start (the game's own Restart, which inherits the environment) loads
// them here. Each library's nx2navi_plugin_init, when it exports one, is called once with the table.
void plugins_start(void) {
    static char list[32768];
    static int started;
    if (started) return;
    started = 1;
    if (!plat_getenv("NX2NAVI_MOD_DLLS", list, sizeof list)) return;
    const char *game_dir = api_game_info()->game_dir;
    for (char *p = list, *next; p && *p; p = next) {
        next = strchr(p, '|');
        if (next) *next++ = 0;
        if (!*p) continue;
        char path[PLAT_PATH_MAX * 3];
        int absolute = p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':');
        if (absolute) snprintf(path, sizeof path, "%s", p);
        else snprintf(path, sizeof path, "%s/%s", game_dir, p);
        void *lib = plat_lib_open(path);
        if (!lib) {
            rt_log("plugins: %s did not load (missing, not a 64-bit library, or a library it needs is missing); the "
                   "game runs without it", path);
            continue;
        }
        Nx2PluginInitFn init = (Nx2PluginInitFn)plat_lib_sym(lib, NX2_PLUGIN_INIT_EXPORT);
        rt_log("plugins: loaded %s%s", path, init ? "" : " (it exports no " NX2_PLUGIN_INIT_EXPORT ")");
        if (init) init(&api);
    }
}
