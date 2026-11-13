// NX2NAVI_FLOOR_WATCH (floor_watch in nx2navi.ini, 0 by default; diagnosis, 1.1.1, Windows): which code reads and
// writes the moving floor under Link, at 60 fps and above it. It takes its addresses from frame_rate.c's floor scan
// (which it switches on), once the scan has the values below Link that move with him at 60 fps:
//   - the candidates: up to two survivors inside the platform's rigid body controller unit (the object with vtable
//     main+0x4bf8a88, where the platform's own position is kept), then one survivor of each distinct object (more than
//     0x200 bytes from the ones taken) at the height most survivors share below Link (the platform's pivot, 2 m under
//     him on the Deku Tree's floating platform), FW_CAND in all;
//   - a pass watches them four at a time (the debug registers, platform.h plat_watch_set) for FW_ROUND_NS each, every
//     read or write of the 8 bytes at a survivor (its x and y; 4 bytes, x alone, when it is not 8-byte aligned). Each
//     access counts its call chain (the instruction after it and three return addresses) in a lock-free table, with the
//     candidate and the kind: a write when the value changed since the candidate's last access, else a read;
//   - the first pass runs as soon as the candidates are known (the scan arms at 60 fps); the second at the next stretch
//     of 120 presents at 120 fps or more (frames per update at most 0.5) with Link on the floor (his height over the
//     pivot within 0.2 m of what it was at the first pass, at most 4 m from it sideways). After each pass the log gets
//     the pass's frame rate, each candidate's reads and writes a present, and its FW_SHOW most counted chains, each
//     frame named by the executable's symbol table (lifted functions are f_<guest address>, shown with their main
//     offset), so the code that reads the floor at 60 fps and not at 120 can be compared.
// A round ends early after FW_ROUND_HITS accesses; after that each thread that traps again has its watchpoints cleared.
// Each access costs an exception (several microseconds), so a pass slows the game for its few seconds; symbol lookup
// after a pass holds one present for up to a second. Off it costs one test a present in frame_rate.c.
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "floor_watch.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"
#include "prof_syms.h"

#define FW_CAND 12                       // candidate addresses, four a round
#define FW_ROUND_NS 1500000000ull
#define FW_ROUND_HITS 200000
#define FW_SLOTS 8192                    // call chains, open addressing; a full table drops new ones
#define FW_SHOW 40                       // chains logged a pass
#define FW_PASSES 2
#define FW_HIGH_FRAMES 0.5f              // the second pass: at most this many 60 Hz frames per update (120 fps)
#define FW_HIGH_STRETCH 120

typedef struct {
    _Atomic uint64_t key;   // 0: free
    _Atomic uint64_t n;
    uint64_t frame[PLAT_WATCH_DEPTH];
    uint8_t cand, kind, depth;
} FwChain;

static FwChain tab[FW_SLOTS];
static _Atomic uint64_t fw_hits, fw_dropped, fw_reads[FW_CAND], fw_writes[FW_CAND];
static _Atomic uint64_t last_val[4];
static int slot_cand[4], slot_len[4];
static uint64_t cand[FW_CAND];
static int cand_len[FW_CAND], cand_unit[FW_CAND], ncand, floor_ref = -1;
static float cand_y[FW_CAND], dy0;
static uint64_t round_presents[(FW_CAND + 3) / 4];
static struct {
    int state;   // 0 no candidates, 1 waiting for a pass's frame rate, 2 a pass runs, 3 done
    int pass, round, threads;
    uint64_t pass_t0, round_t0, presents, on_floor, stretch, hits, rounds_armed;
    double frames_sum;
} w;

int floor_watch_wanted(void) {
    static int wanted = -1;
    if (wanted < 0) {
        const char *e = cfg_get("NX2NAVI_FLOOR_WATCH");
        wanted = RT_V111 && e && *e && strcmp(e, "0") != 0;
        if (wanted) rt_log("floor watch: on; it waits for the floor scan's addresses");
    }
    return wanted;
}

// On the thread that made the access, inside its exception handling: no lock, no allocation, no log.
static int fw_hit(int slot, const uint64_t *frames, int depth) {
    if (atomic_fetch_add_explicit(&fw_hits, 1, memory_order_relaxed) >= FW_ROUND_HITS) return 1;
    int ci = slot_cand[slot];
    // the value now, read through the system (a read from here would trap again): a write changed it
    uint64_t v = 0;
    plat_mem_read(cand[ci], &v, (size_t)slot_len[slot]);
    int kind = atomic_exchange_explicit(&last_val[slot], v, memory_order_relaxed) != v;
    atomic_fetch_add_explicit(kind ? &fw_writes[ci] : &fw_reads[ci], 1, memory_order_relaxed);
    uint64_t key = 0xcbf29ce484222325ull ^ (uint64_t)(ci * 2 + kind);
    for (int d = 0; d < depth; d++) key = (key ^ frames[d]) * 0x100000001b3ull;
    key = ((key ^ (uint64_t)depth) * 0x100000001b3ull) | 1;
    uint32_t i = (uint32_t)(key >> 40) & (FW_SLOTS - 1);
    for (int k = 0; k < 64; k++, i = (i + 1) & (FW_SLOTS - 1)) {
        uint64_t have = atomic_load_explicit(&tab[i].key, memory_order_acquire);
        if (!have) {
            if (atomic_compare_exchange_strong_explicit(&tab[i].key, &have, key, memory_order_acq_rel,
                                                        memory_order_acquire)) {
                memcpy(tab[i].frame, frames, sizeof(uint64_t) * (size_t)depth);
                tab[i].depth = (uint8_t)depth, tab[i].cand = (uint8_t)ci, tab[i].kind = (uint8_t)kind;
                atomic_fetch_add_explicit(&tab[i].n, 1, memory_order_relaxed);
                return 0;
            }
        }
        if (have == key) {
            atomic_fetch_add_explicit(&tab[i].n, 1, memory_order_relaxed);
            return 0;
        }
    }
    atomic_fetch_add_explicit(&fw_dropped, 1, memory_order_relaxed);
    return 0;
}

int floor_watch_found(const uint64_t *addr, const float *val, const int *unit, int n, const float link[3]) {
    if (!floor_watch_wanted() || w.state) return 0;
    ncand = 0;
    for (int k = 0; k < n && ncand < 2; k++)   // the controller unit's own triplets
        if (unit[k]) cand_y[ncand] = val[3 * k + 1], cand_unit[ncand] = 1, cand[ncand++] = addr[k];
    // the height most survivors share below Link: the pivot that the floor's copies (body, matrices) hold
    int best = -1, best_n = 0;
    for (int k = 0; k < n; k++) {
        if (!(val[3 * k + 1] < link[1] - 0.3f)) continue;
        int m = 0;
        for (int j = 0; j < n; j++) m += fabsf(val[3 * j + 1] - val[3 * k + 1]) < 0.02f;
        if (m > best_n) best = k, best_n = m;
    }
    for (int k = 0; best >= 0 && k < n && ncand < FW_CAND; k++) {
        if (fabsf(val[3 * k + 1] - val[3 * best + 1]) >= 0.02f) continue;
        int near = 0;
        for (int j = 0; j < ncand; j++) near |= (addr[k] > cand[j] ? addr[k] - cand[j] : cand[j] - addr[k]) <= 0x200;
        if (near) continue;
        if (floor_ref < 0) floor_ref = ncand;
        cand_y[ncand] = val[3 * k + 1], cand_unit[ncand] = 0, cand[ncand++] = addr[k];
    }
    for (int j = 0; j < ncand; j++) cand_len[j] = cand[j] % 8 ? 4 : 8;
    if (!ncand || floor_ref < 0) {
        rt_log("floor watch: the scan's %d survivors hold no height below Link to watch; off", n);
        w.state = 3;
        return 0;
    }
    dy0 = link[1] - cand_y[floor_ref];
    rt_log("floor watch: %d addresses to watch, four at a time; Link %.3f m above the floor's pivot at %#llx", ncand,
           dy0, (unsigned long long)cand[floor_ref]);
    for (int j = 0; j < ncand; j++)
        rt_log("floor watch:   address %d: %#llx (%d bytes), y %.3f%s", j, (unsigned long long)cand[j], cand_len[j],
               cand_y[j], cand_unit[j] ? ", in the rigid body controller unit (vtable main+0x4bf8a88)" : "");
    w.state = 1;
    return 1;
}

// Link on the floor: his height over the floor's pivot as at the first pass, and not far from it sideways
static int on_floor(const float link[3]) {
    float p[3];
    if (!plat_mem_read(cand[floor_ref], p, sizeof p) || !isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2])) return 0;
    return fabsf(link[1] - p[1] - dy0) < 0.2f && hypotf(link[0] - p[0], link[2] - p[2]) < 4.0f;
}

static int round_arm(void) {
    uint64_t a[4];
    int len[4], n = 0;
    for (int k = 0; k < 4; k++) {
        int ci = w.round * 4 + k;
        uint64_t v = 0;
        if (ci >= ncand || !plat_mem_read(cand[ci], &v, (size_t)cand_len[ci])) continue;   // gone with its area
        slot_cand[n] = ci, slot_len[n] = cand_len[ci];
        atomic_store_explicit(&last_val[n], v, memory_order_relaxed);
        a[n] = cand[ci], len[n] = cand_len[ci], n++;
    }
    atomic_store_explicit(&fw_hits, 0, memory_order_relaxed);
    if (!n) return 0;
    int t = plat_watch_set(a, len, n, fw_hit);
    if (t > w.threads) w.threads = t;
    if (t >= 0) w.rounds_armed++;
    return t;
}

// ---- the report of a pass
typedef struct { uint64_t addr; char name[48]; uint32_t off; } FwName;
static FwName *names;
static int nnames;

static void name_one(void *ctx, uint64_t addr, const char *name, uint32_t off) {
    (void)ctx;
    if (!names || nnames >= FW_SHOW * PLAT_WATCH_DEPTH) return;
    FwName *f = &names[nnames++];
    f->addr = addr, f->off = off;
    snprintf(f->name, sizeof f->name, "%s", name);
}

static int frame_text(char *out, size_t cap, uint64_t a) {
    for (int i = 0; i < nnames; i++) {
        if (names[i].addr != a) continue;
        // a lifted function is named after its guest address: f_7002c215e0 is main+0x2c215e0
        const char *nm = names[i].name;
        uint64_t guest = !strncmp(nm, "f_", 2) ? strtoull(nm + 2, NULL, 16) : 0;
        if (guest >= MOD_MAIN_BASE && guest < MOD_MAIN_BASE + MOD_MAIN_SIZE)
            return snprintf(out, cap, "%s+0x%x (main+%#llx)", nm, names[i].off,
                            (unsigned long long)(guest - MOD_MAIN_BASE));
        return snprintf(out, cap, "%s+0x%x", nm, names[i].off);
    }
    char mod[64];
    uintptr_t base = 0;
    if (plat_module_of((uintptr_t)a, mod, sizeof mod, &base))
        return snprintf(out, cap, "%s+%#llx", mod, (unsigned long long)(a - base));
    return snprintf(out, cap, "%#llx", (unsigned long long)a);
}

static int by_count(const void *x, const void *y) {
    uint64_t a = atomic_load_explicit(&(*(FwChain *const *)x)->n, memory_order_relaxed),
             b = atomic_load_explicit(&(*(FwChain *const *)y)->n, memory_order_relaxed);
    return a < b ? 1 : a > b ? -1 : 0;
}

static void report(uint64_t t) {
    double secs = (t - w.pass_t0) / 1e9, frames = w.presents ? w.frames_sum / w.presents : 0;
    rt_log("floor watch: pass %d at %.3f frames per update on average (%.0f fps): %.2f s in %llu rounds on up to %d "
           "threads, %llu presents (%llu with Link on the floor), %llu accesses, %llu chains not kept",
           w.pass + 1, frames, frames > 0 ? 60.0 / frames : 0.0, secs, (unsigned long long)w.rounds_armed, w.threads,
           (unsigned long long)w.presents, (unsigned long long)w.on_floor, (unsigned long long)w.hits,
           (unsigned long long)atomic_load(&fw_dropped));
    for (int j = 0; j < ncand; j++) {
        uint64_t r = atomic_load(&fw_reads[j]), wr = atomic_load(&fw_writes[j]), p = round_presents[j / 4];
        rt_log("floor watch:   address %d %#llx: %llu reads, %llu writes in %llu presents (%.2f and %.2f a present)", j,
               (unsigned long long)cand[j], (unsigned long long)r, (unsigned long long)wr, (unsigned long long)p,
               p ? (double)r / p : 0.0, p ? (double)wr / p : 0.0);
    }
    static FwChain *list[FW_SLOTS];
    int n = 0;
    for (int i = 0; i < FW_SLOTS; i++)
        if (atomic_load(&tab[i].key) && atomic_load(&tab[i].n)) list[n++] = &tab[i];
    qsort(list, (size_t)n, sizeof *list, by_count);
    if (n > FW_SHOW) n = FW_SHOW;
    // the frames' names from the executable's own symbol table
    uint64_t addrs[FW_SHOW * PLAT_WATCH_DEPTH];
    size_t na = 0;
    for (int i = 0; i < n; i++)
        for (int d = 0; d < list[i]->depth; d++) addrs[na++] = list[i]->frame[d];
    char exe[1024];
    if (!names) names = malloc(sizeof *names * FW_SHOW * PLAT_WATCH_DEPTH);
    nnames = 0;
    if (names && na && plat_exe_path(exe, sizeof exe) &&
        prof_syms_each(exe, (uint64_t)plat_exe_base(), addrs, na, name_one, NULL) < 0)
        rt_log("floor watch: the executable has no symbol table: frames stay unnamed");
    for (int i = 0; i < n; i++) {
        const FwChain *c = list[i];
        uint64_t cnt = atomic_load(&c->n), p = round_presents[c->cand / 4];
        char line[1024];
        int o = snprintf(line, sizeof line, "floor watch:   %llu (%.2f a present) %s of address %d:",
                         (unsigned long long)cnt, p ? (double)cnt / p : 0.0, c->kind ? "writes" : "reads", c->cand);
        for (int d = 0; d < c->depth && o > 0 && o < (int)sizeof line - 2; d++) {
            o += snprintf(line + o, sizeof line - o, "%s", d ? " < " : " ");
            if (o < (int)sizeof line) o += frame_text(line + o, sizeof line - o, c->frame[d]);
        }
        rt_log("%s", line);
    }
}

static void pass_start(uint64_t t) {
    memset(tab, 0, sizeof tab);
    for (int j = 0; j < FW_CAND; j++) atomic_store(&fw_reads[j], 0), atomic_store(&fw_writes[j], 0);
    memset(round_presents, 0, sizeof round_presents);
    atomic_store(&fw_dropped, 0);
    w.round = 0, w.presents = w.on_floor = w.hits = w.rounds_armed = 0, w.frames_sum = 0, w.threads = 0;
    w.pass_t0 = w.round_t0 = t;
    if (round_arm() < 0) {
        rt_log("floor watch: no hardware watchpoints here (or they could not be set); off");
        w.state = 3;
        return;
    }
    rt_log("floor watch: pass %d starts", w.pass + 1);
    w.state = 2;
}

int floor_watch_present(const float link[3], int link_ok, float frames) {
    if (w.state == 0 || w.state == 3) return w.state == 0;
    uint64_t t = plat_now_ns();
    if (w.state == 1) {   // the pass's frame rate, with Link on the floor
        int band = w.pass == 0 ? fabsf(frames - 1.0f) < 0.05f : frames <= FW_HIGH_FRAMES;
        w.stretch = band && link_ok && on_floor(link) ? w.stretch + 1 : 0;
        if (w.stretch >= (w.pass == 0 ? 1u : FW_HIGH_STRETCH)) pass_start(t);
        return w.state != 3;
    }
    w.presents++, round_presents[w.round]++;
    w.frames_sum += frames;
    if (link_ok && on_floor(link)) w.on_floor++;
    uint64_t hits = atomic_load_explicit(&fw_hits, memory_order_relaxed);
    if (t - w.round_t0 < FW_ROUND_NS && hits < FW_ROUND_HITS) return 1;
    plat_watch_set(NULL, NULL, 0, NULL);
    w.hits += hits < FW_ROUND_HITS ? hits : FW_ROUND_HITS;
    if (++w.round * 4 < ncand) {
        w.round_t0 = t;
        round_arm();
        return 1;
    }
    report(t);
    w.stretch = 0;
    w.state = ++w.pass < FW_PASSES ? 1 : 3;
    if (w.state == 1) rt_log("floor watch: pass 2 waits for %d presents at 120 fps or more with Link on the floor",
                             FW_HIGH_STRETCH);
    else rt_log("floor watch: done");
    return w.state != 3;
}
