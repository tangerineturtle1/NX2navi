// Uniform blocks as of the submit call. NVN starts the GPU when nvnQueueSubmitCommands returns and the GPU reads the
// constants soon after; here the replay records for several milliseconds, and the GPU reads a pool's bytes when it
// runs the part that uses them (the VRAM mirror's sync copies run then, vk_memory.c "VRAM mirror"). Meanwhile the game,
// having handed the frame over, rewrites per-object uniform blocks on other threads with no fence: late draws read
// another object's matrix (phantom rocks, a flat far field; NX2navi 1.2.1's producer f_70008c6460, 14-24 ms after the
// submit). So at the start of a submission's recording every uniform range it binds is copied into an arena of its own,
// and the uniform bindings of the replay use the copy.
// - Only ranges of imported pools (memory the CPU writes) are copied, aligned out to 256 bytes (the largest
//   minUniformBufferOffsetAlignment) and at most 64 KiB from the bound address, overlapping ones merged.
// - A range the GPU wrote in a submission that has not completed is left out: its bytes reach guest memory only with
//   that submission's write-back, the mirror holds them until then.
// - A GPU write within the submission (UpdateUniformBuffer, a storage buffer a shader writes, a buffer copy or fill,
//   an image-to-buffer copy: vk_usnap_drop) drops the copies it touches from then on, so later draws read the result.
// - The arena goes back to a free list once the submission's fence is done (completion thread).
// The copy is taken at the submit call: where vk_exec_record starts when the replay runs on the submitting thread, and
// by the submitting thread itself with NX2NAVI_REPLAY_ASYNC (vk_usnap_take), where the record starts up to a frame
// later on the replay thread. Taken only there, it held whatever the game had put into those bytes by then: in a
// graveyard the block of the shadow pass came out with another block's values in the frame its allocator moved on to
// the next stretch of memory, about every 364 frames, and that frame's sunlight was all in shadow (a dark frame with
// the interface intact). NX2NAVI_UBO_SNAPSHOT=0 binds live memory as before; =ab alternates (NX2NAVI_AB).
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "platform/platform.h"

#define ALIGN 256u
#define MAX_RANGE (64u << 10)
#define ARENA_MIN (4u << 20)
#define CALL_DEPTH 16

typedef struct Arena {
    struct Arena *next;
    VkBuffer buf;
    VmaAllocation alloc;
    uint8_t *map;
    uint8_t *cpu;    // with DLSS (vk_dlaa_available), the same bytes in host memory for the CPU's reads (vk_usnap_bytes):
                     // map is often device memory through the BAR, written fast but read back at about a microsecond a
                     // cache line. Without DLSS nothing reads them back, and the bytes go into map in one copy.
    uint64_t size;
} Arena;

typedef struct { uint64_t lo, hi; } Rng;
// dropped: 0, or the epoch of its first drop in the stream (vk_exec_epoch); epoch: of the write
typedef struct { uint64_t lo, hi, off, dropped; } Reg;
typedef struct { uint64_t lo, hi, fence, epoch; } Wrote;

static struct {
    int mode;                   // rt_knob: 0 off, 1 on, 2 ab
    int on;                     // this submission has a snapshot
    uint64_t fence, gen;
    Arena *arena;
    Rng *col;                   // collected ranges
    int ncol, ccol;
    Reg *reg;                   // merged, sorted by lo, disjoint
    int nreg, creg;
    Wrote *wr;                  // GPU writes of submissions not known to be complete
    int nwr, cwr;
    // since the last summary
    uint64_t subs, regions, bytes, copy_ns, hits, drops, inflight, refused, off_subs;
} U;

static Arena *free_arenas;
static PlatMutex free_lock = PLAT_MUTEX_INIT;
// U.col, sort_col's buffer and collect's neighbour cache: the submitting thread (vk_usnap_take) and the replay thread
// (vk_usnap_begin, for a submission nothing was taken for) both collect.
static PlatMutex gather_lock = PLAT_MUTEX_INIT;

// A copy taken at the submit call for the replay thread (vk_usnap_take), in submission order.
typedef struct Taken {
    struct Taken *next;
    Arena *arena;
    Reg *reg;
    int nreg, n;
    uint64_t first, total, refused, ns;   // first, n: the submission's first handle and handle count
} Taken;
static Taken *tq_head, *tq_tail;
static PlatMutex tq_lock = PLAT_MUTEX_INIT;

static void *grow(void *p, int *cap, size_t elem) {
    int n = *cap ? *cap * 2 : 1024;
    void *q = realloc(p, (size_t)n * elem);
    if (!q) rt_fatal("vk_usnap: out of memory");
    *cap = n;
    return q;
}

static Arena *arena_get(uint64_t need) {
    plat_mutex_lock(&free_lock);
    Arena **pp = &free_arenas, *a = NULL;
    for (; *pp; pp = &(*pp)->next)
        if ((*pp)->size >= need) {
            a = *pp, *pp = a->next;
            break;
        }
    plat_mutex_unlock(&free_lock);
    if (a) return a;
    a = calloc(1, sizeof *a);
    if (!a) rt_fatal("vk_usnap: out of memory");
    a->size = need < ARENA_MIN ? ARENA_MIN : (need + (1u << 20) - 1) & ~(uint64_t)((1u << 20) - 1);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = a->size,
                             // transfer source: DLAA copies the camera block out (vk_dlaa.c mv_pass)
                             .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    // Device-local through the BAR when the device has it (the GPU reads constants on every draw), else host memory.
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                           VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    VkResult res = vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &a->buf, &a->alloc, &info);
    if (res != VK_SUCCESS) {
        rt_log("vk_usnap: arena of %llu bytes failed (%d): this submission binds live memory",
               (unsigned long long)a->size, (int)res);
        free(a);
        return NULL;
    }
    a->map = info.pMappedData;
    rt_log("vk_usnap: new arena of %.1f MiB", a->size / 1048576.0);
    return a;
}

static void arena_done(void *arg) {   // completion thread: the GPU has read it
    Arena *a = arg;
    plat_mutex_lock(&free_lock);
    a->next = free_arenas, free_arenas = a;
    plat_mutex_unlock(&free_lock);
}

static void collect(const op_hdr *h, int depth) {
    for (;;) {
        if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_)
            rt_fatal("vk_usnap: corrupt command record at %p: op %u size %u", (const void *)h, h->op, h->size);
        const void *a = nvn_op_payload(h);
        if (h->op == OP_END) return;
        if (h->op == OP_JUMP) {
            h = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target;
            continue;
        }
        if (h->op == OP_CALL && depth < CALL_DEPTH) collect(nvn_op_first(((const p_jump *)a)->target), depth + 1);
        if (h->op == OP_BIND_UBO) {
            const p_bind_buf *b = a;
            if (b->addr && b->size) {
                uint64_t n = b->size < MAX_RANGE ? b->size : MAX_RANGE;
                // the vertex stage's object and bone blocks: also the game's other copy on either side, which still
                // holds last frame's (vk_mvprev.c)
                uint64_t step = vk_mvprev_step(b->stage, b->index, b->size);
                // which neighbours lie in an imported pool, per block and vk_mem_gen: the same blocks are bound by
                // many draws, and two resolves for each binding were a quarter of the snapshot's time
                static struct { uint64_t addr, size, gen; int ok; } nc[1024];
                uint32_t ci = (uint32_t)((b->addr ^ n << 48) * 0x9E3779B97F4A7C15ull >> 54);
                uint64_t gen = vk_mem_gen();
                int known = step && nc[ci].addr == b->addr && nc[ci].size == n && nc[ci].gen == gen, ok = 0;
                for (int k = step ? -1 : 0; k <= (step ? 1 : 0); k++) {
                    uint64_t at = b->addr + (uint64_t)((int64_t)k * (int64_t)step);
                    if (k < 0 && b->addr < step) continue;
                    // a neighbour past its pool would make the whole merged region fail the pool check below, and
                    // the bindings in it read live memory again
                    VkMemSpan sp;
                    uint64_t lo = at & ~(uint64_t)(ALIGN - 1);
                    if (k && known) {
                        if (!(nc[ci].ok >> (k + 1) & 1)) continue;
                    } else if (k) {
                        if (!vk_mem_lookup(lo, at + n - lo, &sp) || sp.alloc || !sp.pool || sp.avail < at + n - lo ||
                            (sp.cls != VK_MCLS_HOST_CACHED && sp.cls != VK_MCLS_HOST_UNC))
                            continue;
                        ok |= 1 << (k + 1);
                    }
                    if (U.ncol == U.ccol) U.col = grow(U.col, &U.ccol, sizeof *U.col);
                    U.col[U.ncol++] = (Rng){at & ~(uint64_t)(ALIGN - 1), at + n};
                }
                if (step && !known) nc[ci].addr = b->addr, nc[ci].size = n, nc[ci].gen = gen, nc[ci].ok = ok;
            }
        }
        h = nvn_op_next(h);
    }
}

static int by_lo(const void *a, const void *b) {
    uint64_t x = ((const Rng *)a)->lo, y = ((const Rng *)b)->lo;
    return x < y ? -1 : x > y;
}
static int wr_by_lo(const void *a, const void *b) {
    uint64_t x = ((const Wrote *)a)->lo, y = ((const Wrote *)b)->lo;
    return x < y ? -1 : x > y;
}

// Sorts the collected ranges by lo. A town frame binds about 10,000 uniform ranges and qsort took half a millisecond
// of the copy's 0.96 ms on the game thread, so they go through an LSD radix sort on (lo - lowest) / ALIGN, three
// passes of 11 bits; qsort is left for a span that does not fit in 33 bits (2 TiB).
#define RADIX_BITS 11
static void sort_col(void) {
    static Rng *tmp;
    static int ctmp;
    int n = U.ncol;
    if (n < 2) return;
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int i = 0; i < n; i++) {
        if (U.col[i].lo < lo) lo = U.col[i].lo;
        if (U.col[i].lo > hi) hi = U.col[i].lo;
    }
    if ((hi - lo) / ALIGN >> (3 * RADIX_BITS)) {
        qsort(U.col, (size_t)n, sizeof *U.col, by_lo);
        return;
    }
    if (ctmp < n) {
        free(tmp);
        ctmp = U.ccol;
        if (!(tmp = malloc((size_t)ctmp * sizeof *tmp))) rt_fatal("vk_usnap: out of memory");
    }
    Rng *src = U.col, *dst = tmp;
    for (int pass = 0; pass < 3; pass++) {
        int shift = pass * RADIX_BITS, count[1 << RADIX_BITS] = {0};
        for (int i = 0; i < n; i++) count[(src[i].lo - lo) / ALIGN >> shift & ((1 << RADIX_BITS) - 1)]++;
        for (int k = 0, sum = 0; k < 1 << RADIX_BITS; k++) {
            int c = count[k];
            count[k] = sum, sum += c;
        }
        for (int i = 0; i < n; i++) dst[count[(src[i].lo - lo) / ALIGN >> shift & ((1 << RADIX_BITS) - 1)]++] = src[i];
        Rng *t = src;
        src = dst, dst = t;
    }
    if (src != U.col) memcpy(U.col, src, (size_t)n * sizeof *src);   // three passes end in tmp
}

// Keeps the GPU writes of submissions that are not done, merged into disjoint ranges sorted by lo (the terrain rewrites
// one block before every draw: hundreds of equal entries), each with the newest fence among its parts.
static void wr_prune(void) {
    uint64_t done = vk_sync_fence_done();
    int n = 0;
    for (int i = 0; i < U.nwr; i++)
        if (U.wr[i].fence > done) U.wr[n++] = U.wr[i];
    qsort(U.wr, (size_t)n, sizeof *U.wr, wr_by_lo);
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (m && U.wr[i].lo <= U.wr[m - 1].hi) {
            if (U.wr[i].hi > U.wr[m - 1].hi) U.wr[m - 1].hi = U.wr[i].hi;
            if (U.wr[i].fence > U.wr[m - 1].fence) U.wr[m - 1].fence = U.wr[i].fence;
        } else {
            U.wr[m++] = U.wr[i];
        }
    }
    U.nwr = m;
}
static int wr_hits(uint64_t lo, uint64_t hi) {   // wr_prune ran: disjoint, sorted
    int a = 0, b = U.nwr;
    while (a < b) {
        int k = (a + b) / 2;
        if (U.wr[k].hi <= lo) a = k + 1;
        else b = k;
    }
    return a < U.nwr && U.wr[a].lo < hi;
}

static void summary(void) {
    if (U.subs + U.off_subs < 3600) return;
    double s = U.subs ? (double)U.subs : 1.0;
    rt_log("vk_usnap: %llu submissions with a snapshot, %llu without; per submission %.0f ranges, %.1f KiB copied in "
           "%.3f ms, %.0f uniform bindings from the copy, %.1f dropped by GPU writes, %.1f left out (GPU writes in "
           "flight), %.1f refused (not an imported pool)",
           (unsigned long long)U.subs, (unsigned long long)U.off_subs, U.regions / s, U.bytes / s / 1024.0,
           U.copy_ns / s / 1e6, U.hits / s, U.drops / s, U.inflight / s, U.refused / s);
    U.subs = U.off_subs = U.regions = U.bytes = U.copy_ns = U.hits = U.drops = U.inflight = U.refused = 0;
}

// copy_regions's work, sliced for the replay workers (vk_exec_par): regions of the game's memory into the arena, then
// the host copy into the device copy in COPY_SLICE pieces.
#define COPY_SLICE (64u << 10)
typedef struct { uint8_t *dst, *map; const Reg *reg; uint64_t total; } CopyJob;
static void copy_some(void *arg, int lo, int hi) {
    const CopyJob *j = arg;
    for (int i = lo; i < hi; i++)
        memcpy(j->dst + j->reg[i].off, (const void *)(uintptr_t)j->reg[i].lo, j->reg[i].hi - j->reg[i].lo);
}
static void copy_down(void *arg, int lo, int hi) {
    const CopyJob *j = arg;
    uint64_t a = (uint64_t)lo * COPY_SLICE, b = (uint64_t)hi * COPY_SLICE;
    if (b > j->total) b = j->total;
    memcpy(j->map + a, j->dst + a, b - a);
}

// The bytes of regions reg[0..nreg) (total bytes in all) into a's device copy, through its host copy with DLSS. With
// par (the replay thread at a submission's start), split over the replay workers: about 700 regions and 0.7 MiB in
// Hyrule Field, half a millisecond of the submitting thread's time on one thread.
static void copy_regions(Arena *a, const Reg *reg, int nreg, uint64_t total, int par) {
    uint8_t *dst = a->map;
    if (vk_dlaa_available()) {
        if (!a->cpu && !(a->cpu = malloc(a->size))) rt_fatal("vk_usnap: out of memory");
        dst = a->cpu;
    }
    CopyJob j = {dst, a->map, reg, total};
    if (par) vk_exec_par(copy_some, &j, nreg, 48);
    else copy_some(&j, 0, nreg);
    if (dst != a->map) {   // sequential writes into the device copy
        int slices = (int)((total + COPY_SLICE - 1) / COPY_SLICE);
        if (par) vk_exec_par(copy_down, &j, slices, 2);
        else copy_down(&j, 0, slices);
    }
    vmaFlushAllocation(vk_ctx()->vma, a->alloc, 0, total);   // a no-op on coherent memory
}

// NX2NAVI_REPLAY_ASYNC, the submitting thread before the submission is queued for the replay: the uniform ranges it
// binds are copied now, while they hold what the game submitted. vk_usnap_begin takes the copy over when the replay
// reaches the submission, and leaves out there what GPU writes in flight touch (the replay knows them only then).
void vk_usnap_take(const uint64_t *handles, int n) {
    if (!U.mode) U.mode = rt_knob("UBO_SNAPSHOT", "1") + 1;
    if (n <= 0 || !RT_KNOB_ON(U.mode - 1)) return;
    uint64_t t0 = kern_now_ns();
    Taken *t = calloc(1, sizeof *t);
    if (!t) rt_fatal("vk_usnap: out of memory");
    t->n = n, t->first = handles[0];
    int creg = 0;
    plat_mutex_lock(&gather_lock);
    U.ncol = 0;
    for (int i = 0; i < n; i++)
        if (handles[i]) collect(nvn_op_first(handles[i]), 0);
    sort_col();
    for (int i = 0; i < U.ncol;) {   // merged, keeping what lies wholly in one imported pool
        uint64_t lo = U.col[i].lo, hi = U.col[i].hi;
        for (i++; i < U.ncol && U.col[i].lo <= hi; i++)
            if (U.col[i].hi > hi) hi = U.col[i].hi;
        VkMemSpan sp;
        if (!vk_mem_resolve(lo, hi - lo, &sp) || sp.alloc || !sp.pool || sp.avail < hi - lo ||
            (sp.cls != VK_MCLS_HOST_CACHED && sp.cls != VK_MCLS_HOST_UNC)) {
            t->refused++;
            continue;
        }
        if (t->nreg == creg) t->reg = grow(t->reg, &creg, sizeof *t->reg);
        t->reg[t->nreg++] = (Reg){lo, hi, t->total, 0};
        t->total += (hi - lo + ALIGN - 1) & ~(uint64_t)(ALIGN - 1);
    }
    plat_mutex_unlock(&gather_lock);
    if (t->nreg && (t->arena = arena_get(t->total))) copy_regions(t->arena, t->reg, t->nreg, t->total, 0);
    t->ns = kern_now_ns() - t0;
    plat_mutex_lock(&tq_lock);
    if (tq_tail) tq_tail->next = t;
    else tq_head = t;
    tq_tail = t;
    plat_mutex_unlock(&tq_lock);
}

static void taken_free(Taken *t) {
    if (t->arena) arena_done(t->arena);
    free(t->reg);
    free(t);
}

// The copy vk_usnap_take made for this submission becomes the replay's. 0: none was taken.
static int adopt(const uint64_t *handles, int n) {
    plat_mutex_lock(&tq_lock);
    Taken *t = tq_head;
    // in submission order: an entry ahead of this submission's belongs to one that never reached the replay
    while (t && (t->n != n || t->first != (n > 0 ? handles[0] : 0))) {
        tq_head = t->next;
        taken_free(t);
        t = tq_head;
    }
    if (t) tq_head = t->next;
    if (!tq_head) tq_tail = NULL;
    plat_mutex_unlock(&tq_lock);
    if (!t) return 0;
    U.refused += t->refused;
    if (t->arena) {
        wr_prune();
        while (U.creg < t->nreg) U.reg = grow(U.reg, &U.creg, sizeof *U.reg);
        memcpy(U.reg, t->reg, (size_t)t->nreg * sizeof *U.reg);
        U.nreg = t->nreg;
        for (int i = 0; i < U.nreg; i++)   // a GPU write in flight: the mirror holds those bytes, not guest memory
            if (wr_hits(U.reg[i].lo, U.reg[i].hi)) U.reg[i].dropped = 1, U.inflight++;
        U.arena = t->arena, t->arena = NULL;
        U.on = 1;
        U.subs++, U.regions += (uint64_t)U.nreg, U.bytes += t->total, U.copy_ns += t->ns;
    }
    taken_free(t);
    return 1;
}

void vk_usnap_begin(const uint64_t *handles, int n, uint64_t fence) {
    if (!U.mode) U.mode = rt_knob("UBO_SNAPSHOT", "1") + 1;   // stored +1: 0 is "not read yet"
    U.on = 0, U.fence = fence, U.gen++;
    summary();
    if (!RT_KNOB_ON(U.mode - 1)) {
        U.off_subs++;
        wr_prune();   // vk_usnap_drop goes on listing the GPU's writes with snapshots off: without this the list only grew
        return;
    }
    if (adopt(handles, n)) return;
    uint64_t t0 = kern_now_ns();
    plat_mutex_lock(&gather_lock);
    U.ncol = 0;
    for (int i = 0; i < n; i++)
        if (handles[i]) collect(nvn_op_first(handles[i]), 0);
    sort_col();
    wr_prune();
    // merge, keeping only what lies wholly in one imported pool and no GPU write in flight touches
    U.nreg = 0;
    uint64_t total = 0;
    for (int i = 0; i < U.ncol;) {
        uint64_t lo = U.col[i].lo, hi = U.col[i].hi;
        for (i++; i < U.ncol && U.col[i].lo <= hi; i++)
            if (U.col[i].hi > hi) hi = U.col[i].hi;
        VkMemSpan sp;
        if (!vk_mem_lookup(lo, hi - lo, &sp) || sp.alloc || !sp.pool || sp.avail < hi - lo ||
            (sp.cls != VK_MCLS_HOST_CACHED && sp.cls != VK_MCLS_HOST_UNC)) {
            U.refused++;
            continue;
        }
        if (wr_hits(lo, hi)) {
            U.inflight++;
            continue;
        }
        if (U.nreg == U.creg) U.reg = grow(U.reg, &U.creg, sizeof *U.reg);
        U.reg[U.nreg++] = (Reg){lo, hi, total, 0};
        total += (hi - lo + ALIGN - 1) & ~(uint64_t)(ALIGN - 1);
    }
    plat_mutex_unlock(&gather_lock);
    if (!U.nreg) return;
    if (!(U.arena = arena_get(total))) return;
    copy_regions(U.arena, U.reg, U.nreg, total, 1);   // the replay thread at the submission's start
    U.on = 1;
    U.subs++, U.regions += (uint64_t)U.nreg, U.bytes += total, U.copy_ns += kern_now_ns() - t0;
}

void vk_usnap_end(uint64_t fence) {
    if (U.arena) vk_completion_add(fence, arena_done, U.arena), U.arena = NULL;
    U.on = 0, U.gen++;
}

// First region whose end is above a.
static int first_above(uint64_t a) {
    int lo = 0, hi = U.nreg;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (U.reg[m].hi <= a) lo = m + 1;
        else hi = m;
    }
    return lo;
}

int vk_usnap_find(uint64_t addr, uint64_t n, uint64_t align, VkDescriptorBufferInfo *out) {
    if (!U.on || !n) return 0;
    // Consecutive bindings mostly name the region the last one did: that one is tried before the binary search. The
    // regions are sorted and disjoint, so the region that holds addr is the one first_above finds.
    static _Thread_local int hint;   // per thread: the replay workers look up at once
    int i = hint;
    if (i >= U.nreg || addr < U.reg[i].lo || addr >= U.reg[i].hi) i = first_above(addr);
    if (i >= U.nreg) return 0;
    const Reg *r = &U.reg[i];
    if ((r->dropped && r->dropped <= vk_exec_epoch(0)) || addr < r->lo || addr + n > r->hi) return 0;
    uint64_t off = r->off + (addr - r->lo);
    if (off % align) return 0;
    *out = (VkDescriptorBufferInfo){U.arena->buf, off, n};
    U.hits++;
    hint = i;
    return 1;
}

static PlatMutex drop_lock = PLAT_MUTEX_INIT;   // the replay workers' shaders write storage at once

// The replay workers (vk_exec.c NX2NAVI_REPLAY_PIPE) record a group's draws while the submitting thread goes on past
// it: a drop holds from its place in the stream on (vk_exec_epoch), so a chunk does not see the drops of ops after its
// group, and the submitting thread sees every one.
void vk_usnap_drop(uint64_t addr, uint64_t n) {
    if (!U.mode || !n) return;
    uint64_t ep = vk_exec_epoch(1);
    int lk = vk_exec_shared();
    if (lk) plat_mutex_lock(&drop_lock);
    if (U.nwr == U.cwr) U.wr = grow(U.wr, &U.cwr, sizeof *U.wr);
    U.wr[U.nwr++] = (Wrote){addr, addr + n, U.fence, ep};
    if (U.on)
        for (int i = first_above(addr); i < U.nreg && U.reg[i].lo < addr + n; i++)
            if (!U.reg[i].dropped || ep < U.reg[i].dropped) {
                if (!U.reg[i].dropped) U.drops++;
                U.reg[i].dropped = ep, __atomic_add_fetch(&U.gen, 1, __ATOMIC_RELAXED);
            }
    if (lk) plat_mutex_unlock(&drop_lock);
}

uint64_t vk_usnap_gen(void) { return U.gen; }

// A GPU write of a submission not known to be complete touches [lo, hi). A linear scan: vk_usnap_drop appends after
// wr_prune sorted the list, so wr_hits's binary search does not hold here; only called on rare paths.
static int wr_overlaps(uint64_t lo, uint64_t hi) {
    uint64_t ep = vk_exec_epoch(0);
    for (int i = 0; i < U.nwr; i++)
        if (U.wr[i].lo < hi && lo < U.wr[i].hi && U.wr[i].epoch <= ep) return 1;
    return 0;
}

static int cpu_written(uint64_t addr, uint64_t n) {   // the same test vk_usnap_begin keeps regions by
    VkMemSpan sp;
    return vk_mem_lookup(addr, n, &sp) && !sp.alloc && sp.pool && sp.avail >= n &&
           (sp.cls == VK_MCLS_HOST_CACHED || sp.cls == VK_MCLS_HOST_UNC);
}

int vk_usnap_read(uint64_t addr, uint64_t n, void *dst) {
    const uint8_t *s = vk_usnap_bytes(addr, n);
    if (s) return memcpy(dst, s, n), 1;
    if (!n || !cpu_written(addr, n)) return 0;
    int lk = vk_exec_shared();   // a chunk's drop may grow the list
    if (lk) plat_mutex_lock(&drop_lock);
    int w = wr_overlaps(addr, addr + n);
    if (lk) plat_mutex_unlock(&drop_lock);
    if (w) return 0;
    memcpy(dst, (const void *)(uintptr_t)addr, n);
    return 2;
}

int vk_usnap_why(uint64_t addr, uint64_t n) {
    if (!U.on) return VK_USNAP_NO_SNAPSHOT;
    int i = first_above(addr);
    if (i < U.nreg && addr >= U.reg[i].lo && addr + n <= U.reg[i].hi)
        return U.reg[i].dropped ? VK_USNAP_DROPPED : VK_USNAP_FOUND;
    if (wr_overlaps(addr, addr + n)) return VK_USNAP_IN_FLIGHT;
    if (!cpu_written(addr, n)) return VK_USNAP_NOT_CPU;
    return VK_USNAP_NOT_BOUND;
}

// the host copy where there is one (DLSS), else the device copy
static uint8_t *host_copy(void) { return U.arena->cpu ? U.arena->cpu : U.arena->map; }

const uint8_t *vk_usnap_map(void) { return U.arena ? host_copy() : NULL; }

const uint8_t *vk_usnap_bytes(uint64_t addr, uint64_t n) {
    VkDescriptorBufferInfo bi;
    return vk_usnap_find(addr, n, 1, &bi) ? host_copy() + bi.offset : NULL;
}

// The arenas back on the free list (vk_exec_draw_shutdown: the device is idle, every completion ran).
void vk_usnap_shutdown(void) {
    plat_mutex_lock(&free_lock);
    for (Arena *a = free_arenas, *next; a; a = next) next = a->next, vmaDestroyBuffer(vk_ctx()->vma, a->buf, a->alloc), free(a->cpu), free(a);
    free_arenas = NULL;
    plat_mutex_unlock(&free_lock);
}

// NX2NAVI_TRACE=perf "mem: sizes" (perf.c): the free uniform snapshot arenas (the free list is never trimmed) and the
// GPU-write list (the replay thread's; read without a lock, a line may be one write behind).
void vk_usnap_sizes(PerfSizes *s) {
    uint64_t n = 0, b = 0;
    plat_mutex_lock(&free_lock);
    for (const Arena *a = free_arenas; a; a = a->next) n++, b += a->size;
    plat_mutex_unlock(&free_lock);
    perf_size(s, "free uniform snapshot arenas", n, 0, b);
    perf_size(s, "uniform snapshot GPU writes", (uint64_t)__atomic_load_n(&U.nwr, __ATOMIC_RELAXED), 0, PERF_SIZE_NONE);
}
