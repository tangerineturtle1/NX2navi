// Frame timing (NX2NAVI_TRACE=perf, the frame-time summary, the performance overlay's counts), the A/B switches (rt_ab,
// rt_knob) and NX2NAVI_ONE_PER_CORE. The sampling profiler (NX2NAVI_PROFILE) is the OS's (prof_win.c); without one the weak
// prof_sampler_* below do nothing.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "flight.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "perfmon.h"
#include "prof.h"
#include "platform/platform.h"

volatile uint32_t prof_render_tid, prof_main_tid, prof_replay_tid;

__attribute__((weak)) int prof_sampler_start(void) {
    if (cfg_get("NX2NAVI_PROFILE")) rt_log("prof: NX2NAVI_PROFILE: no sampling profiler on this OS");
    return 0;
}
__attribute__((weak)) void prof_sampler_write(void) {}
__attribute__((weak)) void prof_sampler_frame(uint64_t frame, uint64_t now, uint64_t d) { (void)frame, (void)now, (void)d; }
__attribute__((weak)) void prof_commit_census(uint64_t frame) { (void)frame; }
__attribute__((weak)) void mem_guest_census(uint64_t frame) { (void)frame; }
__attribute__((weak)) void mem_lazy_heap_report(uint64_t frame) { (void)frame; }

// NX2NAVI_TRACE=perf, NX2NAVI_FLIGHT=1 or the full performance overlay: event counts are taken. Wait times are always taken
// (frame-time summary). perf_base: the part fixed at start.
static int perf_on, perf_base;
static int overlay;   // performance overlay mode (perf_set_overlay): 0 off, 1 fps, 2 full

void perf_set_overlay(int mode) {
    __atomic_store_n(&overlay, mode, __ATOMIC_RELAXED);
    __atomic_store_n(&perf_on, perf_base || mode == 2, __ATOMIC_RELAXED);
}

static int gpu_want;   // perf_gpu_timing_want: a feature reads GPU time (dynamic resolution)

void perf_gpu_timing_want(int on) { __atomic_store_n(&gpu_want, on != 0, __ATOMIC_RELAXED); }

int perf_gpu_timing(void) {
    return (rt_trace & RT_TRACE_PERF) || __atomic_load_n(&overlay, __ATOMIC_RELAXED) == 2 ||
           __atomic_load_n(&gpu_want, __ATOMIC_RELAXED);
}

static volatile int64_t gpu_busy_total;

void perf_gpu_busy(uint64_t ns) { plat_add64(&gpu_busy_total, (int64_t)ns); }

uint64_t perf_gpu_busy_total(void) { return (uint64_t)__atomic_load_n(&gpu_busy_total, __ATOMIC_RELAXED); }

void prof_start(void) {
    flight_init();
    atexit(perf_summary_flush);   // boot.c's _exit path goes through prof_write instead
    perf_base = (rt_trace & RT_TRACE_PERF) || flight_on;
    perf_on = perf_base || overlay == 2;
    if (prof_sampler_start()) atexit(prof_write);   // a --seconds run ends in _exit: boot.c calls prof_write itself first
}

void prof_write(void) {
    perf_summary_flush();
    prof_sampler_write();
}

// ---- NX2NAVI_TRACE=perf: frame interval and the time the render thread spends waiting, logged every 60 presents. The
// flight recorder (NX2NAVI_FLIGHT=1, flight.c) takes the same numbers per frame.
static uint64_t wait_ns[PERF_N], frame_ns[PERF_N], worst_ns[PERF_N];   // window totals, this frame, the slowest frame

uint64_t perf_begin(void) { return kern_now_ns(); }

void perf_replay_thread(void) { prof_replay_tid = plat_thread_id(); }
void perf_main_thread(void) { prof_main_tid = plat_thread_id(); }

void perf_end(int kind, uint64_t t0) {
    if (!t0) return;
    uint32_t tid = plat_thread_id();
    if (tid != prof_render_tid && tid != prof_replay_tid) return;
    uint64_t d = kern_now_ns() - t0;
    if (tid == prof_render_tid && !prof_replay_tid) {   // one thread adds: no atomics on the synchronous path
        wait_ns[kind] += d;
        frame_ns[kind] += d;
        return;
    }
    __atomic_fetch_add(&wait_ns[kind], d, __ATOMIC_RELAXED);
    __atomic_fetch_add(&frame_ns[kind], d, __ATOMIC_RELAXED);
}

static volatile int64_t other_wait_ns;     // perf_gpu_wait on threads other than the render thread, over the window
static volatile int64_t render_wait_ns;    // the same on the render thread
void (*perf_gpu_stats)(PerfGpu *out);

void perf_gpu_wait(uint64_t t0) {
    if (!t0 || !__atomic_load_n(&perf_on, __ATOMIC_RELAXED)) return;
    int64_t d = (int64_t)(kern_now_ns() - t0);
    plat_add64(plat_thread_id() == prof_render_tid ? &render_wait_ns : &other_wait_ns, d);
}

static volatile int64_t replay_ns;         // perf_replay: summed over the window, any thread
static volatile int64_t frame_replay_ns;   // the same since the previous present (frame-time summary)
// The replaying thread's CPU time over the same spans (perf_replay_cpu): the rest of the replay's wall time it was
// waiting (a lock, the recorder) or not running (preempted). Since the previous present, and over the period.
static volatile int64_t frame_replay_cpu_ns;
static uint64_t per_replay_ns, per_replay_cpu_ns;

void perf_replay_cpu(uint64_t ns) { plat_add64(&frame_replay_cpu_ns, (int64_t)ns); }

void perf_replay(uint64_t t0) {
    if (!t0) return;
    int64_t d = (int64_t)(kern_now_ns() - t0);
    plat_add64(&frame_replay_ns, d);
    if (__atomic_load_n(&perf_on, __ATOMIC_RELAXED)) plat_add64(&replay_ns, d);
}

static volatile int64_t counts[PERF_CNT_N];   // running totals: the perf log and the flight recorder take differences

void perf_count(int kind) {
    if (__atomic_load_n(&perf_on, __ATOMIC_RELAXED)) plat_inc64(&counts[kind]);
}

// ---- NX2NAVI_TRACE=perf work marks (host.h PERF_M_*): any thread adds; the present takes the frame's sums (mark_frame),
// names them on a slow frame's line and adds them to the 60-frame window's line.
static const char *const mark_name[PERF_M_N] = {"file reads", "zstd decodes", "pipeline creates", "guest memory maps",
                                                "pool init/finalize", "device memory allocations",
                                                "DLSS feature creates"};
static volatile int64_t mk_ns[PERF_M_N], mk_n[PERF_M_N], mk_bytes[PERF_M_N], mk_max[PERF_M_N];   // since the last present
typedef struct { uint64_t ns[PERF_M_N], n[PERF_M_N], bytes[PERF_M_N], max[PERF_M_N]; } MarkSums;
static MarkSums mk_win;   // the 60-frame window's, present thread only

uint64_t perf_mark_begin(void) { return (rt_trace & RT_TRACE_PERF) ? kern_now_ns() : 0; }

void perf_mark_add(int kind, uint64_t ns, uint64_t bytes) {
    if (!(rt_trace & RT_TRACE_PERF) || (unsigned)kind >= PERF_M_N) return;
    plat_add64(&mk_ns[kind], (int64_t)ns);
    plat_inc64(&mk_n[kind]);
    plat_add64(&mk_bytes[kind], (int64_t)bytes);
    int64_t m = __atomic_load_n(&mk_max[kind], __ATOMIC_RELAXED);
    while ((int64_t)ns > m && !__atomic_compare_exchange_n(&mk_max[kind], &m, (int64_t)ns, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

void perf_mark_end(int kind, uint64_t t0, uint64_t bytes) {
    if (t0) perf_mark_add(kind, kern_now_ns() - t0, bytes);
}

// Takes the sums since the last present into *f and adds them to the window.
static void mark_frame(MarkSums *f) {
    for (int k = 0; k < PERF_M_N; k++) {
        f->ns[k] = (uint64_t)plat_xchg64(&mk_ns[k], 0), f->n[k] = (uint64_t)plat_xchg64(&mk_n[k], 0);
        f->bytes[k] = (uint64_t)plat_xchg64(&mk_bytes[k], 0), f->max[k] = (uint64_t)plat_xchg64(&mk_max[k], 0);
        mk_win.ns[k] += f->ns[k], mk_win.n[k] += f->n[k], mk_win.bytes[k] += f->bytes[k];
        if (f->max[k] > mk_win.max[k]) mk_win.max[k] = f->max[k];
    }
}

// "; on any thread: file reads 12.3 ms (40, 5.1 MiB, longest 2.0 ms), ..." for the kinds with work, appended at line + n.
// Device memory allocations carry no time.
static int mark_text(const MarkSums *s, char *line, int n, size_t cap) {
    int any = 0;
    for (int k = 0; k < PERF_M_N && n >= 0 && n < (int)cap - 100; k++) {
        if (!s->n[k]) continue;
        n += snprintf(line + n, cap - (size_t)n, "%s %s", any ? "," : "; on any thread:", mark_name[k]);
        if (k == PERF_M_DEVMEM)
            n += snprintf(line + n, cap - (size_t)n, " %llu (%.1f MiB)", (unsigned long long)s->n[k], s->bytes[k] / 1048576.0);
        else
            n += snprintf(line + n, cap - (size_t)n, " %.1f ms (%llu, %.1f MiB, longest %.1f ms)", s->ns[k] / 1e6,
                          (unsigned long long)s->n[k], s->bytes[k] / 1048576.0, s->max[k] / 1e6);
        any = 1;
    }
    return n;
}

// ---- NX2NAVI_TRACE=perf segments (host.h perf_segment) and scenes (perf_scene). The present thread keeps the running
// segment's frame times in 50 us bins (SEG_BINS, up to 204.8 ms; slower frames land in the last bin, the worst is kept
// apart), so a segment's 1% low is exact to 50 us at any frame rate. perf_segment and perf_scene come from any thread
// (the input script's guest thread, the game's loading threads) and leave a request under seg_lock for the present.
#define SEG_BINS 4096
#define SEG_BIN_NS 50000ull
#define SCENE_SETTLE_NS 10000000000ull
static PlatMutex seg_lock = PLAT_MUTEX_INIT;
static char seg_next[96], scene_name[256];   // seg_lock: the requested segment's name; the areas loaded since the last snapshot
static int seg_asked;                          // seg_lock: a segment was asked for
static uint64_t scene_at;                      // seg_lock: the last area load (plat_now_ns), 0 none since the last snapshot
static struct {
    char name[96];
    uint64_t f0, t0, frames, sum, max, max_frame, over25, over50, over100;
    int64_t w0;
    uint32_t hist[SEG_BINS];
    MarkSums marks;
    int on;
} seg;

__attribute__((weak)) void vk_mem_snapshot(uint64_t frame) { (void)frame; }

// ---- "mem: sizes" lines (host.h PerfSizes): one line per group; a line that grows past the log's line size goes out
// and the group goes on in the next.
#define SIZES_WEAK(f) __attribute__((weak)) void f(PerfSizes *s) { (void)s; }
SIZES_WEAK(mem_sizes) SIZES_WEAK(kern_sizes) SIZES_WEAK(setjmp_sizes) SIZES_WEAK(fs_host_sizes) SIZES_WEAK(romfs_sizes)
SIZES_WEAK(zstd_sizes) SIZES_WEAK(cfg_sizes) SIZES_WEAK(vk_mem_sizes) SIZES_WEAK(vk_gpa_sizes) SIZES_WEAK(vk_texture_sizes)
SIZES_WEAK(vk_exec_sizes) SIZES_WEAK(vk_sync_sizes) SIZES_WEAK(vk_usnap_sizes) SIZES_WEAK(nvn_cmd_sizes)
SIZES_WEAK(vk_pipeline_sizes) SIZES_WEAK(gfx_shader_sizes)

static void sizes_flush(PerfSizes *s) {
    if (s->n > 0) rt_log("mem: sizes frame %llu, %s:%s", (unsigned long long)s->frame, s->group, s->line);
    s->n = 0, s->line[0] = 0;
}

void perf_size(PerfSizes *s, const char *name, uint64_t count, uint64_t limit, uint64_t bytes) {
    if (s->n > (int)sizeof s->line - 120) sizes_flush(s);
    int n = snprintf(s->line + s->n, sizeof s->line - (size_t)s->n, " %s", name);
    if (count != PERF_SIZE_NONE) n += snprintf(s->line + s->n + n, sizeof s->line - (size_t)(s->n + n), " %llu", (unsigned long long)count);
    if (limit) n += snprintf(s->line + s->n + n, sizeof s->line - (size_t)(s->n + n), "/%llu", (unsigned long long)limit);
    if (bytes != PERF_SIZE_NONE)
        n += snprintf(s->line + s->n + n, sizeof s->line - (size_t)(s->n + n), "%s%.1f MiB", count != PERF_SIZE_NONE ? ", " : " ",
                      bytes / 1048576.0);
    n += snprintf(s->line + s->n + n, sizeof s->line - (size_t)(s->n + n), ";");
    if (n > 0 && s->n + n < (int)sizeof s->line) s->n += n;
    else s->line[s->n] = 0;
}

static void sizes_log(uint64_t frame) {
    static const struct { const char *group; void (*fn[7])(PerfSizes *); } groups[] = {
        {"guest memory, kernel, files and settings", {mem_sizes, kern_sizes, setjmp_sizes, fs_host_sizes, romfs_sizes, zstd_sizes, cfg_sizes}},
        {"GPU memory and textures", {vk_mem_sizes, vk_gpa_sizes, vk_texture_sizes}},
        {"replay and submission", {vk_exec_sizes, vk_sync_sizes, vk_usnap_sizes, nvn_cmd_sizes}},
        {"pipelines and shaders", {vk_pipeline_sizes, gfx_shader_sizes}},
    };
    PerfSizes s;
    for (size_t g = 0; g < sizeof groups / sizeof *groups; g++) {
        s.n = 0, s.line[0] = 0, s.frame = frame, s.group = groups[g].group;
        for (int i = 0; i < 7 && groups[g].fn[i]; i++) groups[g].fn[i](&s);
        sizes_flush(&s);
    }
}

// The memory lines of a segment's end or a settled scene: the back end's (vk_mem_snapshot), then the sizes.
static void mem_snapshot(uint64_t frame) {
    vk_mem_snapshot(frame);
    sizes_log(frame);
}
__attribute__((weak)) int vk_mem_vram(uint64_t *used, uint64_t *budget, int *exact) {
    (void)used, (void)budget, (void)exact;
    return 0;
}

// The world clock as the segment and scene lines give it: seconds, or "-" before the loaded save's first frame.
static const char *wtxt(int64_t w, char *b) {
    if (w < 0) return "-";
    snprintf(b, 24, "%.1f", w / 1e6);
    return b;
}

void perf_segment(const char *name) {
    if (!(rt_trace & RT_TRACE_PERF)) return;
    plat_mutex_lock(&seg_lock);
    snprintf(seg_next, sizeof seg_next, "%s", name && *name ? name : "(unnamed)");
    seg_asked = 1;
    plat_mutex_unlock(&seg_lock);
}

void perf_scene(const char *name) {
    if (!(rt_trace & RT_TRACE_PERF)) return;
    char wb[24];
    rt_log("scene: %s loads (frame %llu, world %s s)", name,
           (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), wtxt(frame_step_world_us(), wb));
    plat_mutex_lock(&seg_lock);
    size_t n = strlen(scene_name);
    if (n + strlen(name) + 2 < sizeof scene_name) snprintf(scene_name + n, sizeof scene_name - n, "%s%s", n ? ", " : "", name);
    scene_at = plat_now_ns();
    plat_mutex_unlock(&seg_lock);
}

// The process's memory and the VRAM in use, for the segment and scene lines.
static void mem_text(char *out, size_t cap, int vram) {
    uint64_t res, prv, used, budget;
    int exact;
    plat_process_memory(&res, &prv);
    int n = snprintf(out, cap, "ram working set %.0f MiB, private %.0f MiB", res / 1048576.0, prv / 1048576.0);
    if (vram && vk_mem_vram(&used, &budget, &exact) && n > 0 && (size_t)n < cap)
        snprintf(out + n, cap - (size_t)n, "; vram %.0f of %.0f MiB", used / 1048576.0, budget / 1048576.0);
}

static void seg_close(uint64_t frame, uint64_t now, int at_exit) {
    if (!seg.on) return;
    seg.on = 0;
    char mem[160], marks[700];
    mem_text(mem, sizeof mem, !at_exit);
    marks[0] = 0;
    mark_text(&seg.marks, marks, 0, sizeof marks);
    if (!seg.frames || !seg.sum) {
        rt_log("segment: '%s' ended at frame %llu with no frames timed; %s", seg.name, (unsigned long long)frame, mem);
        return;
    }
    uint64_t cut = (seg.frames + 99) / 100, acc = 0, low = 0;   // the slowest 1% of frames: the upper edge of its bin
    for (int b = SEG_BINS - 1; b >= 0; b--)
        if ((acc += seg.hist[b]) >= cut) { low = (uint64_t)(b + 1) * SEG_BIN_NS; break; }
    if (low > seg.max) low = seg.max;
    char w0[24], w1[24];
    rt_log("segment: '%s' frames %llu-%llu, world %s to %s s: %llu frames in %.1f s, mean %.2f ms (%.1f fps), 1%% low %.1f "
           "fps (%.2f ms), worst %.1f ms at frame %llu; over 25 ms %llu, 50 ms %llu, 100 ms %llu; %s%s",
           seg.name, (unsigned long long)seg.f0, (unsigned long long)frame, wtxt(seg.w0, w0), wtxt(frame_step_world_us(), w1),
           (unsigned long long)seg.frames, (now - seg.t0) / 1e9, seg.sum / 1e6 / seg.frames, seg.frames * 1e9 / seg.sum,
           low ? 1e9 / low : 0.0, low / 1e6, seg.max / 1e6, (unsigned long long)seg.max_frame,
           (unsigned long long)seg.over25, (unsigned long long)seg.over50, (unsigned long long)seg.over100, mem, marks);
    if (!at_exit) mem_snapshot(frame);
}

static void seg_open(const char *name, uint64_t frame, uint64_t now) {
    memset(&seg, 0, sizeof seg);
    snprintf(seg.name, sizeof seg.name, "%s", name);
    seg.f0 = frame, seg.t0 = now, seg.w0 = frame_step_world_us(), seg.on = 1;
}

// Each present while the trace is on: the frame into the running segment (d 0: the first present), then a requested
// segment change and a settled scene's snapshot.
static void seg_frame(uint64_t frame, uint64_t now, uint64_t d, const MarkSums *fm) {
    if (!seg.on && seg.f0 == 0) seg_open("boot", frame, now);   // the first present opens the boot segment
    if (seg.on && d) {
        seg.frames++, seg.sum += d;
        uint64_t b = d / SEG_BIN_NS;
        seg.hist[b < SEG_BINS ? b : SEG_BINS - 1]++;
        if (d > seg.max) seg.max = d, seg.max_frame = frame;
        seg.over25 += d > 25000000ull, seg.over50 += d > 50000000ull, seg.over100 += d > 100000000ull;
        for (int k = 0; k < PERF_M_N; k++) {
            seg.marks.ns[k] += fm->ns[k], seg.marks.n[k] += fm->n[k], seg.marks.bytes[k] += fm->bytes[k];
            if (fm->max[k] > seg.marks.max[k]) seg.marks.max[k] = fm->max[k];
        }
    }
    char name[96], scenes[256];
    int asked = 0, settled = 0;
    plat_mutex_lock(&seg_lock);
    if (seg_asked) asked = 1, seg_asked = 0, memcpy(name, seg_next, sizeof name);
    if (scene_at && now - scene_at >= SCENE_SETTLE_NS)
        settled = 1, scene_at = 0, memcpy(scenes, scene_name, sizeof scenes), scene_name[0] = 0;
    plat_mutex_unlock(&seg_lock);
    if (asked) {
        seg_close(frame, now, 0);
        seg_open(name, frame, now);
        char wb[24];
        rt_log("segment: '%s' starts at frame %llu, world %s s", name, (unsigned long long)frame, wtxt(seg.w0, wb));
    }
    if (settled) {
        char mem[160];
        mem_text(mem, sizeof mem, 1);
        char wb[24];
        rt_log("scene: %s settled (no area loaded for 10 s) at frame %llu, world %s s; %s", scenes,
               (unsigned long long)frame, wtxt(frame_step_world_us(), wb), mem);
        mem_snapshot(frame);
    }
}

// ---- Frame-time summary, always on: every 60 s of play and at exit one line of frame count, mean fps, 1% low and the
// frames over 25/50/100 ms, then the worst frames of the period with the render thread's waits in them. Per frame: a
// histogram increment, the compare against the worst list, a memset of the wait array. No allocation.
#define FT_BINS 1024              // 0.25 ms bins up to 256 ms; slower frames land in the last bin
#define FT_WORST 5
#define FT_PERIOD_NS 60000000000ull
typedef struct {
    uint64_t frame, ns, wait[PERF_N], replay;
    uint64_t op_ns[PERF_RP_N], op_n[PERF_RP_N], max_ns, hold_ns;   // NX2NAVI_REPLAY_TIME: the frame's replay ops
    uint64_t replay_cpu;   // the replaying thread's CPU time in the frame's replay
    uint32_t max_op;
} FtWorst;
// What a frame over 25 ms spent its time on, so a count of slow frames says where to look instead of only that they
// happened. A frame is charged to the largest part when that part is at least a quarter of it, and to "unattributed"
// otherwise - the part of a slow frame none of the recorded waits explains (the GPU, or a stall outside them).
// The replay's part is its time without the pipeline, translation and texture waits, which all happen inside it: with
// them in, every frame that waited 900 ms for a pipeline was charged to "replay", the larger number, and a log
// could not tell a pipeline stall from a slow replay.
enum { FT_C_PIPE, FT_C_XLATE, FT_C_TEX, FT_C_FENCE, FT_C_PACE, FT_C_REPLAY, FT_C_OTHER, FT_C_N };
static const char *const ft_cause_name[FT_C_N] = { "pipeline", "translation", "texture", "fence", "pacing", "replay",
                                                   "unattributed" };
static struct {
    uint64_t t0, frames, sum, over25, over50, over100;
    uint32_t hist[FT_BINS];
    uint64_t slow_n[FT_C_N], slow_ns[FT_C_N];   // frames over 25 ms by cause
    FtWorst worst[FT_WORST];   // slowest first
    int nworst;
} ft;
static uint64_t summary_t0;   // start of the current period (0: not started)

// NX2NAVI_REPLAY_TIME=1: how the replay splits between the kinds of op it walks, reported with the frame-time summary.
// The replay is recorded on one thread, so these need no atomics. The kinds are PERF_RP_* in host.h.
// NX2NAVI_LOCK_TIME=1 (default 0): the replay threads time every lock they have to wait for (platform.h
// plat_lock_timing), kept per asking site, and the frame-time summary lists the sites that waited longest since the last
// summary, with the recorder's full-ring and drain waits. Sites are code addresses; "ref" is perf_lock_timing_here's,
// which the link map names, so the others resolve as offsets from it.
#define LT_N 512
static struct { void *site; uint64_t ns, n, max, ns0, n0; } lt[LT_N];
static struct { void *site; uint64_t ns, n, max, ns0, n0; } lh[LT_N];   // texture lock holds over 1 ms (perf_lock_hold)
static uint64_t lt_lost_ns, lt_rec0[2];
static volatile int64_t fr_hold_ns;   // the texture lock's holds over 1 ms since the last present (ft_frame)
void (*perf_rec_waits)(uint64_t *full_ns, uint64_t *drain_ns);

static void lt_add(typeof(lt[0]) *tab, void *site, uint64_t ns) {
    uint32_t h = (uint32_t)(((uintptr_t)site * 0x9E3779B97F4A7C15ull) >> 55);
    for (uint32_t k = 0; k < 32; k++) {
        typeof(lt[0]) *e = &tab[(h + k) % LT_N];
        void *cur = __atomic_load_n(&e->site, __ATOMIC_ACQUIRE);
        if (!cur && __atomic_compare_exchange_n(&e->site, &cur, site, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) cur = site;
        if (cur != site) continue;
        __atomic_fetch_add(&e->ns, ns, __ATOMIC_RELAXED);
        __atomic_fetch_add(&e->n, 1, __ATOMIC_RELAXED);
        uint64_t m = __atomic_load_n(&e->max, __ATOMIC_RELAXED);
        while (ns > m && !__atomic_compare_exchange_n(&e->max, &m, ns, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
        return;
    }
    __atomic_fetch_add(&lt_lost_ns, ns, __ATOMIC_RELAXED);
}

static void lt_note(void *site, uint64_t ns) { lt_add(lt, site, ns); }
void perf_lock_hold(void *site, uint64_t ns) {
    lt_add((typeof(lt[0]) *)lh, site, ns);
    plat_add64(&fr_hold_ns, (int64_t)ns);
}

static int lt_on = -1;
int perf_lock_holds_on(void) {
    if (lt_on < 0) lt_on = cfg_bool("NX2NAVI_LOCK_TIME");
    return lt_on;
}
void perf_lock_timing_here(void) {
    if (lt_on < 0) lt_on = cfg_bool("NX2NAVI_LOCK_TIME");
    if (lt_on) plat_lock_timing(lt_note);
}

// The eight sites of tab with the most time since the last summary, appended to line; returns the period's total and
// sets *count, then starts a new period.
static uint64_t lt_top(typeof(lt[0]) *tab, char *line, int *n, size_t cap, uint64_t *count) {
    uint64_t all = 0, cnt = 0;
    int top[8], ntop = 0;
    for (int i = 0; i < LT_N; i++) {
        if (!__atomic_load_n(&tab[i].site, __ATOMIC_ACQUIRE)) continue;
        uint64_t d = __atomic_load_n(&tab[i].ns, __ATOMIC_RELAXED) - tab[i].ns0;
        all += d, cnt += __atomic_load_n(&tab[i].n, __ATOMIC_RELAXED) - tab[i].n0;
        int at;
        if (ntop < 8) at = ntop++;
        else if (d > tab[top[7]].ns - tab[top[7]].ns0) at = 7;
        else continue;
        top[at] = i;
        for (int j = at; j > 0; j--) {   // keep top sorted by this period's time
            uint64_t x = tab[top[j]].ns - tab[top[j]].ns0, y = tab[top[j - 1]].ns - tab[top[j - 1]].ns0;
            if (x <= y) break;
            int t = top[j]; top[j] = top[j - 1], top[j - 1] = t;
        }
    }
    for (int j = 0; j < ntop && *n >= 0 && *n < (int)cap - 80; j++) {
        typeof(lt[0]) *e = &tab[top[j]];
        *n += snprintf(line + *n, cap - *n, " %p %.1f ms (%llu, max %.2f ms);", e->site, (e->ns - e->ns0) / 1e6,
                       (unsigned long long)(e->n - e->n0), e->max / 1e6);
    }
    for (int i = 0; i < LT_N; i++) tab[i].ns0 = tab[i].ns, tab[i].n0 = tab[i].n, tab[i].max = 0;
    *count = cnt;
    return all;
}

// The lock lines of a summary: the replay threads' waits with the recorder's, then the texture lock's long holds.
static void lt_report(void) {
    uint64_t rec[2] = {0, 0}, waits, holds;
    if (perf_rec_waits) perf_rec_waits(&rec[0], &rec[1]);
    char line[1400], top[1200];
    int n = 0;
    top[0] = 0;
    uint64_t all = lt_top(lt, top, &n, sizeof top, &waits);
    rt_log("frametime:   replay lock waits: %.0f ms (%llu waits, %.0f ms unplaced); recorder ring full %.0f ms, drains "
           "%.0f ms; ref %p:%s", all / 1e6, (unsigned long long)waits, lt_lost_ns / 1e6, (rec[0] - lt_rec0[0]) / 1e6,
           (rec[1] - lt_rec0[1]) / 1e6, (void *)perf_lock_timing_here, top);
    n = 0, top[0] = 0;
    all = lt_top((typeof(lt[0]) *)lh, top, &n, sizeof top, &holds);
    snprintf(line, sizeof line, "frametime:   texture lock held over 1 ms: %.0f ms (%llu holds):%s", all / 1e6,
             (unsigned long long)holds, top);
    rt_log("%s", line);
    lt_rec0[0] = rec[0], lt_rec0[1] = rec[1], lt_lost_ns = 0;
}

static uint64_t rp_ns[PERF_RP_N], rp_n[PERF_RP_N];
void perf_replay_op(int cat, uint64_t ns) {
    if ((unsigned)cat < PERF_RP_N) rp_ns[cat] += ns, rp_n[cat]++;
}

// The replay ops since the last present (NX2NAVI_REPLAY_TIME), so a frame over 25 ms charged to the replay can say
// what its replay was: how many ops of each kind, their time, the slowest op and how long the texture lock was held
// over 1 ms (NX2NAVI_LOCK_TIME). The present thread takes and clears them in ft_frame; the replay adds.
static volatile int64_t fr_ns[PERF_RP_N], fr_n[PERF_RP_N], fr_max_ns, fr_max_op;
// Over the period: the frames over 25 ms charged to the replay, their sums, and how often each op was a frame's slowest.
static uint64_t sr_n, sr_ns[PERF_RP_N], sr_cnt[PERF_RP_N], sr_max_ns, sr_hold_ns, sr_max_op[256], sr_wall, sr_cpu;

void perf_replay_op_at(int cat, uint32_t op, uint64_t ns) {
    if ((unsigned)cat >= PERF_RP_N) return;
    perf_replay_op(cat, ns);
    plat_add64(&fr_ns[cat], (int64_t)ns);
    plat_add64(&fr_n[cat], 1);
    // one replay thread at a time records ops; a race with the present only misnames the frame's slowest op
    if ((int64_t)ns > fr_max_ns) fr_max_ns = (int64_t)ns, fr_max_op = op;
}

static void ft_report(uint64_t now, const char *tag) {
    if (ft.frames && ft.sum) {
        uint64_t cut = (ft.frames + 99) / 100, acc = 0, low = 0;   // the slowest 1% of frames
        for (int b = FT_BINS - 1; b >= 0; b--) {
            acc += ft.hist[b];
            if (acc >= cut) { low = (uint64_t)(b + 1) * 250000ull; break; }   // upper edge of the bin
        }
        rt_log("frametime: %s: %llu frames in %.1f s, mean %.1f fps, 1%% low %.1f fps, over 25 ms %llu, 50 ms %llu, 100 ms %llu",
               tag, (unsigned long long)ft.frames, (now - ft.t0) / 1e9, ft.frames * 1e9 / ft.sum, low ? 1e9 / low : 0.0,
               (unsigned long long)ft.over25, (unsigned long long)ft.over50, (unsigned long long)ft.over100);
        if (ft.over25) {
            char line[512];
            int n = snprintf(line, sizeof line, "frametime:   over 25 ms by cause:");
            for (int c = 0; c < FT_C_N && n > 0 && n < (int)sizeof line; c++)
                if (ft.slow_n[c])
                    n += snprintf(line + n, sizeof line - n, " %s %llu (%.1f ms avg)", ft_cause_name[c],
                                  (unsigned long long)ft.slow_n[c], (double)ft.slow_ns[c] / ft.slow_n[c] / 1e6);
            if (n > 0) rt_log("%s", line);
        }
        if (rp_n[PERF_RP_DRAW] || rp_n[PERF_RP_BIND] || rp_n[PERF_RP_XFER] || rp_n[PERF_RP_OTHER]) {
            uint64_t all = 0;
            for (int c = 0; c < PERF_RP_N; c++) all += rp_ns[c];
            rt_log("frametime:   replay ops: %.0f ms in all (%.0f%% of the period); draw %.0f ms (%llu), bind %.0f ms (%llu), "
                   "xfer %.0f ms (%llu), other %.0f ms (%llu)", all / 1e6, 100.0 * all / (now - ft.t0),
                   rp_ns[PERF_RP_DRAW] / 1e6, (unsigned long long)rp_n[PERF_RP_DRAW], rp_ns[PERF_RP_BIND] / 1e6,
                   (unsigned long long)rp_n[PERF_RP_BIND], rp_ns[PERF_RP_XFER] / 1e6, (unsigned long long)rp_n[PERF_RP_XFER],
                   rp_ns[PERF_RP_OTHER] / 1e6, (unsigned long long)rp_n[PERF_RP_OTHER]);
        }
        if (lt_on > 0) lt_report();
        if (per_replay_ns && per_replay_cpu_ns)
            rt_log("frametime:   replay: %.0f ms, the replaying thread on the CPU for %.0f ms of it (%.0f%%); the frames over "
                   "25 ms charged to the replay: %.0f ms, on the CPU %.0f ms (%.0f%%)", per_replay_ns / 1e6,
                   per_replay_cpu_ns / 1e6, 100.0 * per_replay_cpu_ns / per_replay_ns, sr_wall / 1e6, sr_cpu / 1e6,
                   sr_wall ? 100.0 * sr_cpu / sr_wall : 0.0);
        if (sr_n && (sr_cnt[PERF_RP_DRAW] || sr_cnt[PERF_RP_BIND])) {   // the replay of the frames over 25 ms charged to it, per frame
            int top[3] = {-1, -1, -1};
            for (int o = 0; o < 256; o++)
                for (int j = 0; j < 3; j++)
                    if (sr_max_op[o] && (top[j] < 0 || sr_max_op[o] > sr_max_op[top[j]])) {
                        for (int k = 2; k > j; k--) top[k] = top[k - 1];
                        top[j] = o;
                        break;
                    }
            char ops[160];
            int n = 0;
            ops[0] = 0;
            for (int j = 0; j < 3 && top[j] >= 0; j++)
                n += snprintf(ops + n, sizeof ops - n, "%s%s %llu", j ? ", " : "", nvn_op_name((uint32_t)top[j]),
                              (unsigned long long)sr_max_op[top[j]]);
            rt_log("frametime:   slow replay frames %llu, per frame: draws %.1f ms (%.0f), binds %.1f ms (%.0f), transfers "
                   "%.1f ms (%.0f); its slowest op %.1f ms (%s); texture lock held over 1 ms %.1f ms",
                   (unsigned long long)sr_n, sr_ns[PERF_RP_DRAW] / 1e6 / sr_n, (double)sr_cnt[PERF_RP_DRAW] / sr_n,
                   sr_ns[PERF_RP_BIND] / 1e6 / sr_n, (double)sr_cnt[PERF_RP_BIND] / sr_n, sr_ns[PERF_RP_XFER] / 1e6 / sr_n,
                   (double)sr_cnt[PERF_RP_XFER] / sr_n, sr_max_ns / 1e6 / sr_n, ops, sr_hold_ns / 1e6 / sr_n);
        }
        for (int i = 0; i < ft.nworst; i++) {
            const FtWorst *w = &ft.worst[i];
            char ops[200] = "";
            if (w->op_n[PERF_RP_DRAW] || w->op_n[PERF_RP_BIND] || w->op_n[PERF_RP_XFER])
                snprintf(ops, sizeof ops, "; ops: draws %.1f ms (%llu), binds %.1f ms (%llu), transfers %.1f ms (%llu), slowest "
                         "%s %.1f ms, texture lock held %.1f ms", w->op_ns[PERF_RP_DRAW] / 1e6,
                         (unsigned long long)w->op_n[PERF_RP_DRAW], w->op_ns[PERF_RP_BIND] / 1e6,
                         (unsigned long long)w->op_n[PERF_RP_BIND], w->op_ns[PERF_RP_XFER] / 1e6,
                         (unsigned long long)w->op_n[PERF_RP_XFER], nvn_op_name(w->max_op), w->max_ns / 1e6, w->hold_ns / 1e6);
            rt_log("frametime:   worst frame %llu: %.1f ms; shader translation %.1f, pipeline build %.1f, texture upload %.1f (astc %.1f), replay %.1f (on the CPU %.1f), fence/present wait %.1f, pacing %.1f%s",
                   (unsigned long long)w->frame, w->ns / 1e6, w->wait[PERF_XLATE] / 1e6, w->wait[PERF_PIPE] / 1e6,
                   w->wait[PERF_TEX] / 1e6, w->wait[PERF_ASTC] / 1e6, w->replay / 1e6, w->replay_cpu / 1e6,
                   (w->wait[PERF_FENCE] + w->wait[PERF_DRAIN]) / 1e6, w->wait[PERF_PACE] / 1e6, ops);
        }
    }
    memset(&ft, 0, sizeof ft);
    memset(rp_ns, 0, sizeof rp_ns);
    memset(rp_n, 0, sizeof rp_n);
    sr_n = sr_max_ns = sr_hold_ns = sr_wall = sr_cpu = 0;
    per_replay_ns = per_replay_cpu_ns = 0;
    memset(sr_ns, 0, sizeof sr_ns), memset(sr_cnt, 0, sizeof sr_cnt), memset(sr_max_op, 0, sizeof sr_max_op);
    ft.t0 = now;
}

__attribute__((weak)) void gfx_exit_log(void) {}
// The renderer's per-frame texture work (vk_texture.c): closes the frame's sums; d is the frame's interval.
__attribute__((weak)) void vk_texstat_frame(uint64_t frame, uint64_t d) { (void)frame, (void)d; }

void perf_summary_flush(void) {   // at exit (prof_write)
    if (rt_trace & RT_TRACE_PERF) seg_close(__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), kern_now_ns(), 1);
    if (summary_t0) {
        ft_report(kern_now_ns(), "exit");
        gfx_exit_log();
    }
    summary_t0 = 0;
}

// Returns the replay time since the previous present.
static uint64_t ft_frame(uint64_t frame, uint64_t now, uint64_t d) {
    int64_t rep = plat_xchg64(&frame_replay_ns, 0);
    vk_texstat_frame(frame, d);
    uint64_t f_cpu = (uint64_t)plat_xchg64(&frame_replay_cpu_ns, 0);
    per_replay_ns += rep > 0 ? (uint64_t)rep : 0, per_replay_cpu_ns += f_cpu;
    uint64_t f_ns[PERF_RP_N], f_n[PERF_RP_N], f_max = (uint64_t)plat_xchg64(&fr_max_ns, 0);
    uint64_t f_hold = (uint64_t)plat_xchg64(&fr_hold_ns, 0);
    uint32_t f_op = (uint32_t)fr_max_op;
    for (int c = 0; c < PERF_RP_N; c++) f_ns[c] = (uint64_t)plat_xchg64(&fr_ns[c], 0), f_n[c] = (uint64_t)plat_xchg64(&fr_n[c], 0);
    if (!summary_t0) {   // the first present starts the period; there is no interval yet
        summary_t0 = now;
        ft.t0 = now;
        return (uint64_t)rep;
    }
    ft.frames++;
    ft.sum += d;
    uint64_t b = d / 250000ull;
    ft.hist[b < FT_BINS ? b : FT_BINS - 1]++;
    if (d > 25000000ull) ft.over25++;
    if (d > 50000000ull) ft.over50++;
    if (d > 100000000ull) ft.over100++;
    if (d > 25000000ull) {   // which part it went to
        uint64_t part[FT_C_N];
        part[FT_C_PIPE] = frame_ns[PERF_PIPE];
        part[FT_C_XLATE] = frame_ns[PERF_XLATE];
        part[FT_C_TEX] = frame_ns[PERF_TEX];
        part[FT_C_FENCE] = frame_ns[PERF_FENCE] + frame_ns[PERF_DRAIN];
        part[FT_C_PACE] = frame_ns[PERF_PACE];
        part[FT_C_REPLAY] = rep > 0 ? (uint64_t)rep : 0;
        uint64_t inside = part[FT_C_PIPE] + part[FT_C_XLATE] + part[FT_C_TEX];   // waits on the replay thread
        part[FT_C_REPLAY] = part[FT_C_REPLAY] > inside ? part[FT_C_REPLAY] - inside : 0;
        uint64_t known = 0;
        for (int c = 0; c < FT_C_OTHER; c++) known += part[c];
        part[FT_C_OTHER] = d > known ? d - known : 0;
        int best = FT_C_OTHER;
        for (int c = 0; c < FT_C_N; c++)
            if (part[c] > part[best]) best = c;
        if (part[best] * 4 < d) best = FT_C_OTHER;   // no recorded part explains a quarter of the frame
        ft.slow_n[best]++;
        ft.slow_ns[best] += d;
        if (best == FT_C_REPLAY) {
            sr_n++, sr_max_ns += f_max, sr_hold_ns += f_hold, sr_max_op[f_op & 255]++;
            sr_wall += rep > 0 ? (uint64_t)rep : 0, sr_cpu += f_cpu;
            for (int c = 0; c < PERF_RP_N; c++) sr_ns[c] += f_ns[c], sr_cnt[c] += f_n[c];
        }
    }
    if (d > 25000000ull && (ft.nworst < FT_WORST || d > ft.worst[FT_WORST - 1].ns)) {
        int i = ft.nworst < FT_WORST ? ft.nworst++ : FT_WORST - 1;
        while (i > 0 && ft.worst[i - 1].ns < d) ft.worst[i] = ft.worst[i - 1], i--;
        ft.worst[i].frame = frame;
        ft.worst[i].ns = d;
        memcpy(ft.worst[i].wait, frame_ns, sizeof frame_ns);
        ft.worst[i].replay = (uint64_t)rep;
        memcpy(ft.worst[i].op_ns, f_ns, sizeof f_ns), memcpy(ft.worst[i].op_n, f_n, sizeof f_n);
        ft.worst[i].max_ns = f_max, ft.worst[i].max_op = f_op, ft.worst[i].hold_ns = f_hold, ft.worst[i].replay_cpu = f_cpu;
    }
    if (now - summary_t0 >= FT_PERIOD_NS) {
        ft_report(now, "60 s");
        summary_t0 = now;
    }
    return (uint64_t)rep;
}

// The second line of the 60-frame window: GPU busy time (timestamps), the time threads waited for GPU fences, VRAM and
// process memory, and which side holds the frame back. pace: the frame waited for pacing (vsync, the frame limiter) at
// least 15% of the window; gpu: the GPU was busy for 80% of the frame interval or more (without timestamps: threads
// waited for fences for half the interval or more); cpu otherwise.
// With calibrated timestamps a third line splits the GPU's idle time between timed parts: the part was submitted only
// after the previous one had ended (the GPU waited for the CPU's replay or the game), or it was already queued (untimed
// work such as uploads and presents ran there, or a wait held it).
static void perf_gpu_line(uint64_t frame, double interval_ms, double pace_share, uint64_t n) {
    PerfGpu g;
    memset(&g, 0, sizeof g);
    if (perf_gpu_stats) perf_gpu_stats(&g);
    double rw = plat_xchg64(&render_wait_ns, 0) / 1e6 / n, ow = plat_xchg64(&other_wait_ns, 0) / 1e6 / n;
    double gpu_ms = g.have_gpu ? g.gpu_ns / 1e6 / n : 0;
    char gs[160], vs[96];
    if (g.have_gpu) snprintf(gs, sizeof gs, "gpu avg %.2f ms (%llu parts timed, %llu not; copies into VRAM mirrors %.2f ms)",
                             gpu_ms, (unsigned long long)g.gpu_parts, (unsigned long long)g.gpu_missed, g.mirror_ns / 1e6 / n);
    else snprintf(gs, sizeof gs, "gpu avg n/a");
    if (g.have_vram) snprintf(vs, sizeof vs, "vram %.0f of %.0f MiB%s", g.vram_used / 1048576.0, g.vram_budget / 1048576.0,
                              g.vram_exact ? "" : " (own allocations of the heap size)");
    else snprintf(vs, sizeof vs, "vram n/a");
    uint64_t res, prv;
    plat_process_memory(&res, &prv);
    double ws = res / 1048576.0, priv = prv / 1048576.0;
    const char *bound = pace_share >= 0.15 ? "pace"
                      : g.have_gpu ? (gpu_ms >= 0.8 * interval_ms ? "gpu" : "cpu")
                      : (rw + ow >= 0.5 * interval_ms ? "gpu" : "cpu");
    rt_log("perf: frame %llu: %s; fence wait per frame: render thread %.2f ms, other threads %.2f ms; %s; ram working set "
           "%.0f MiB, private %.0f MiB; bound: %s",
           (unsigned long long)frame, gs, rw, ow, vs, ws, priv, bound);
    // Where the committed memory lies, every 3600 frames and whenever it grew by 1 GiB since the last census: the process
    // has run out of commit in long sessions while the game's own pools stayed flat. Only with NX2NAVI_MEM_CENSUS=1, as
    // vk_memory.c's census: the walk holds the address-space lock the game's threads take at every page fault, and here
    // it ran on the presenting thread, so each one delayed a frame by 65-240 ms.
    static int census_on = -1;
    static uint64_t census_prv;
    if (census_on < 0) census_on = cfg_bool("NX2NAVI_MEM_CENSUS");
    if (census_on && (frame % 3600 == 0 || prv >= census_prv + (1ull << 30)))
        prof_commit_census(frame), mem_guest_census(frame), census_prv = prv;
    mem_lazy_heap_report(frame);
    if (g.have_gpu && g.have_idle)
        rt_log("perf: frame %llu: gpu idle per frame %.2f ms between timed parts: %.2f ms waiting for the next submission, "
               "%.2f ms with it queued (untimed work or waits); %.1f gaps over 0.5 ms, longest %.2f ms; submit to start avg %.2f ms",
               (unsigned long long)frame, g.idle_ns / 1e6 / n, g.starved_ns / 1e6 / n, (g.idle_ns - g.starved_ns) / 1e6 / n,
               (double)g.idle_gaps / n, g.max_gap_ns / 1e6, g.gpu_parts ? g.queued_ns / 1e6 / g.gpu_parts : 0.0);
}

int rt_ab(void) {
    static int64_t period = -1;   // us of world clock; 0: off
    if (period < 0) {
        const char *e = cfg_get("NX2NAVI_AB");
        period = e && *e ? (int64_t)(atof(e) * 1e6) : 0;
    }
    int64_t w = period ? frame_step_world_us() : -1;
    if (w < 0) return 0;   // before the world clock: A
    int64_t k = w / period % 4;
    return k == 1 || k == 2;
}

int rt_knob(const char *name, const char *def) {
    char key[64];
    snprintf(key, sizeof key, "NX2NAVI_%s", name);
    const char *e = cfg_get(key);
    if (!e || !*e) e = def;
    return !strcmp(e, "ab") ? 2 : strcmp(e, "0") != 0;
}

// NX2NAVI_ONE_PER_CORE=1: the process keeps the first logical processor of each physical core, so busy threads (the
// render thread, the guest cores, the job workers) never share a core's execution units with another; "ab" switches
// it with the A/B blocks. Off by default until measured here; the same rule
// is reported to give +4-10% on an 8-core SMT CPU.
//
// NX2NAVI_CACHE_DIE=1, the default: on a processor whose dies have L3 caches of different sizes (AMD's two-die
// X3D processors, 3D V-Cache on one die) the process runs on the die with the larger cache, so the guest cores, the
// render thread and the recorder share one L3 and never cross dies. Windows' own placement of games on that die
// (game mode with AMD's driver) depends on recognizing the exe as a game. 0 leaves the placement to the OS; "ab"
// switches it with the A/B blocks. No change on one-die and Intel processors.
//
// The placement is set at start (perf_cpu_start, before the thread pools count the usable processors) and checked at
// every present for the A/B switches. The rules narrow the processors the process was given (start /affinity, a
// launcher's or a benchmark's pin), never widen them; with both off nothing is set.
static int one_mode = -1, die_mode;
static uint64_t cpu_all, cpu_one, cpu_die, cpu_applied;   // cpu_applied: the mask set last, 0 none

static void cpu_placement(void) {
    if (one_mode < 0) {
        one_mode = rt_knob("ONE_PER_CORE", "0");
        die_mode = rt_knob("CACHE_DIE", "1");
        int cores = plat_cpu_cores(&cpu_all, &cpu_one);
        uint32_t kb = 0;
        cpu_die = plat_cpu_cache_die(&kb);
        if (one_mode && cpu_one && cpu_one != cpu_all)
            rt_log("cpu: %d cores, %d logical processors: one per core%s", cores, __builtin_popcountll(cpu_all),
                   one_mode == 2 ? " in the B blocks" : "");
        else
            one_mode = 0;
        // The largest L3 is a reason to stay on its processors only where it serves enough cores to run the game: two
        // dies of one design, one of them with stacked cache (a two-die X3D desktop part, 8 cores a die). A die of
        // fewer cores holds the game to them: the 4 large cores of a mobile part whose 8 small cores share a smaller
        // L3, or the 6 of a 12-core X3D part, where the recording thread's own core (vk_exec.c rec_claim) left 5.
        int die_cores = __builtin_popcountll(cpu_die & cpu_one);
        if (cpu_die && cpu_die != cpu_all && die_cores < 8) {
            rt_log("cpu: logical processors %#llx of %#llx share the largest L3 cache (%u MiB), but only %d cores: not "
                   "used", (unsigned long long)cpu_die, (unsigned long long)cpu_all, kb >> 10, die_cores);
            cpu_die = 0;
        }
        if (cpu_die && cpu_die != cpu_all)
            rt_log("cpu: logical processors %#llx of %#llx share the largest L3 cache (%u MiB)%s",
                   (unsigned long long)cpu_die, (unsigned long long)cpu_all, kb >> 10,
                   !die_mode ? "; cache_die = 0: not used" : die_mode == 2 ? ": the process runs there in the B blocks"
                                                                          : ": the process runs there");
        if (!cpu_die || cpu_die == cpu_all) die_mode = 0;
    }
    uint64_t want = cpu_all;
    if (RT_KNOB_ON(die_mode)) want &= cpu_die;
    if (RT_KNOB_ON(one_mode)) want &= cpu_one;
    if (!want) want = cpu_all;
    if (want == (cpu_applied ? cpu_applied : cpu_all)) return;   // nothing new; never restricted: as it was given
    cpu_applied = want;
    if (!plat_process_affinity(want)) rt_log("cpu: could not move the process to logical processors %#llx", (unsigned long long)want);
}

void perf_cpu_start(void) { cpu_placement(); }

int perf_cpu_on_cache_die(void) { return cpu_applied && cpu_die && (cpu_applied & ~cpu_die) == 0; }

// NX2NAVI_CRASH_FILTER_GUARD (default 1; 0 off): once a second, the crash report's last-chance exception filter is put
// back if a DLL loaded after the start (a GPU driver, an overlay) set its own in its place. Without it, a fault in such
// a DLL ends the process with no FATAL line in the log. The first eight replacements are logged with the module.
static void crash_filter_guard(uint64_t frame) {
    static int on = -1, logged;
    if (on < 0) on = rt_knob("CRASH_FILTER_GUARD", "1");
    if (!on || frame % 60) return;
    char who[160];
    if (plat_crash_filter_reassert(who, sizeof who) && logged < 8) {
        logged++;
        rt_log("diag: frame %llu: an exception filter from %s had replaced the crash report's; the crash report's is "
               "back (NX2NAVI_CRASH_FILTER_GUARD)", (unsigned long long)frame, who);
    }
}

// A thread the recording thread starts through a library inherits its reserved core, and the core would stay reserved
// after the recording thread ended (plat_cpu_reserve_sweep): every RESERVE_SWEEP presents both are put right.
#define RESERVE_SWEEP 300
static void reserve_sweep(uint64_t frame) {
    static int logged;
    if (frame % RESERVE_SWEEP) return;
    int n = plat_cpu_reserve_sweep();
    if (!n || logged >= 8) return;
    logged++;
    if (n < 0) rt_log("cpu: frame %llu: the recording thread has ended; its core is open to every thread again",
                      (unsigned long long)frame);
    else rt_log("cpu: frame %llu: %d thread%s started from the recording thread had its core alone; now on the other "
                "processors", (unsigned long long)frame, n, n > 1 ? "s" : "");
}

void perf_present(uint64_t frame) {
    static uint64_t last, sum, max, n, win_start, ab_b;
    crash_filter_guard(frame);
    static int64_t win_prev[PERF_CNT_N];
    prof_render_tid = plat_thread_id();
    if (rt_trace & RT_TRACE_SCHED) kern_sched_report(frame);
    uint64_t now = kern_now_ns(), d = last ? now - last : 0;
    prof_sampler_frame(frame, now, last ? d : 0);
    uint64_t rep = ft_frame(frame, now, d);   // d is 0 on the first present: it only starts the period
    ab_b += rt_ab();
    cpu_placement();
    reserve_sweep(frame);
    if (__atomic_load_n(&overlay, __ATOMIC_RELAXED))   // the overlay's only cost while it is off: this branch
        perfmon_frame(now, d, frame_ns, rep, __atomic_load_n(&counts[PERF_CNT_TEX_UP], __ATOMIC_RELAXED));
    kern_core_diag_frame(frame, d);
    if (flight_on) flight_frame(frame, now, d, frame_ns, counts);
    if (!(rt_trace & RT_TRACE_PERF)) {
        memset(frame_ns, 0, sizeof frame_ns);
        last = now;
        return;
    }
    static uint64_t slow_ns;   // NX2NAVI_PERF_SLOW_MS (default 50): frames at least this long get a line of their own
    if (!slow_ns) {
        const char *e = cfg_get("NX2NAVI_PERF_SLOW_MS");
        slow_ns = (uint64_t)(e && atoi(e) > 0 ? atoi(e) : 50) * 1000000ull;
    }
    MarkSums fm;
    mark_frame(&fm);
    seg_frame(frame, now, last ? d : 0, &fm);
    if (last) {
        sum += d;
        n++;
        if (d > max) max = d, memcpy(worst_ns, frame_ns, sizeof worst_ns);
        if (d >= slow_ns) {
            char marks[700];
            marks[0] = 0;
            mark_text(&fm, marks, 0, sizeof marks);
            // drain: the wait for the present thread's previous job; replay: the replays since the last present, the
            // pipeline, translation and texture waits above included (the rest of a slow frame is the game's own work)
            rt_log("perf: slow frame %llu: %.1f ms; blocked pipeline %.1f xlate %.1f texture %.1f astc %.1f fence %.1f pacing %.1f "
                   "drain %.1f; replay %.1f%s",
                   (unsigned long long)frame, d / 1e6, frame_ns[PERF_PIPE] / 1e6, frame_ns[PERF_XLATE] / 1e6,
                   frame_ns[PERF_TEX] / 1e6, frame_ns[PERF_ASTC] / 1e6, frame_ns[PERF_FENCE] / 1e6,
                   frame_ns[PERF_PACE] / 1e6, frame_ns[PERF_DRAIN] / 1e6, rep / 1e6, marks);
        }
    }
    memset(frame_ns, 0, sizeof frame_ns);
    last = now;
    if (!win_start) win_start = now;
    if (frame % 60) return;
    uint64_t span = now - win_start;
    int64_t win[PERF_CNT_N];
    for (int i = 0; i < PERF_CNT_N; i++) {
        int64_t v = __atomic_load_n(&counts[i], __ATOMIC_RELAXED);
        win[i] = v - win_prev[i];
        win_prev[i] = v;
    }
    int64_t async = win[PERF_CNT_PIPE_ASYNC], skips = win[PERF_CNT_PIPE_SKIP], up = win[PERF_CNT_TEX_UP];
    int64_t reup = win[PERF_CNT_TEX_REUP], sskip = win[PERF_CNT_SHADER_SKIP], waited = win[PERF_CNT_PIPE_WAIT];
    int64_t oneshot = win[PERF_CNT_ONESHOT_WAIT];
    uint64_t shader_keys, shader_xlated, disk[3];
    static uint64_t disk_prev[3];
    gfx_shader_counts(&shader_keys, &shader_xlated);
    gfx_shader_disk_counts(&disk[0], &disk[1], &disk[2]);
    uint64_t dwin[3];
    for (int i = 0; i < 3; i++) dwin[i] = disk[i] - disk_prev[i], disk_prev[i] = disk[i];
    if (n && span)
        rt_log("perf: frame %llu: interval avg %.2f ms max %.2f ms (%.1f fps); render thread blocked: pacing %.0f%% fence %.0f%% drain %.0f%% pipeline build %.0f%% shader xlate %.0f%% texture %.0f%% (astc %.0f%%); pipelines built in background %lld, draws skipped pending %lld, waited %lld, waiting for a shader %lld, one-shot waited %lld; textures uploaded %lld re-uploaded %lld; slowest frame: xlate %.1f ms texture %.1f ms (astc %.1f) pipeline %.1f ms fence %.1f ms; shaders: %llu keys, %llu translated; render busy avg %.2f ms; replay avg %.2f ms; ab %llu/%llu; world %.1f s; shader disk reads: %llu on the render thread (%.1f ms), %llu by the loaders",
               (unsigned long long)frame, sum / 1e6 / n, max / 1e6, n * 1e9 / sum, 100.0 * wait_ns[PERF_PACE] / span,
               100.0 * wait_ns[PERF_FENCE] / span, 100.0 * wait_ns[PERF_DRAIN] / span,
               100.0 * wait_ns[PERF_PIPE] / span, 100.0 * wait_ns[PERF_XLATE] / span,
               100.0 * wait_ns[PERF_TEX] / span, 100.0 * wait_ns[PERF_ASTC] / span, (long long)async, (long long)skips, (long long)waited, (long long)sskip, (long long)oneshot,
               (long long)up, (long long)reup, worst_ns[PERF_XLATE] / 1e6, worst_ns[PERF_TEX] / 1e6,
               worst_ns[PERF_ASTC] / 1e6, worst_ns[PERF_PIPE] / 1e6, worst_ns[PERF_FENCE] / 1e6,
               (unsigned long long)shader_keys, (unsigned long long)shader_xlated,
               (span > wait_ns[PERF_PACE] ? span - wait_ns[PERF_PACE] : 0) / 1e6 / n,
               plat_xchg64(&replay_ns, 0) / 1e6 / n, (unsigned long long)ab_b, (unsigned long long)n, frame_step_world_us() / 1e6,
               (unsigned long long)dwin[0], dwin[1] / 1e6, (unsigned long long)dwin[2]);
    if (n && span) perf_gpu_line(frame, sum / 1e6 / n, (double)wait_ns[PERF_PACE] / span, n);
    char marks[700];
    if (mark_text(&mk_win, marks, 0, sizeof marks) > 0) rt_log("perf: frame %llu: work in the window%s", (unsigned long long)frame, marks);
    memset(&mk_win, 0, sizeof mk_win);
    sum = max = n = ab_b = 0;
    memset(wait_ns, 0, sizeof wait_ns);
    win_start = now;
    if (frame % 600 == 0) sizes_log(frame);   // beside vk_memory.c's 600-frame memory lines
}
