// Contains code Copyright (c) 2026 MacNativeGames contributors, Copyright (c) 2026 Peiikai (MIT; see
// THIRD_PARTY_NOTICES.md).
// Guest address space and the memory SVCs: heap, module data permissions, stack moves,
// QueryMemory, physical memory in the alias region, GetInfo memory ids, and the kernel-region allocator.
//
// Every region is a plain reservation; pages are committed and decommitted at 4 KiB granularity. Decommitted pages
// come back zeroed, so no stale-page bookkeeping is needed. The stack and heap block and the alias region are
// reserved write-watched (platform.h plat_mem_reserve): mem_written_pages reports the pages the CPU wrote, which
// the Vulkan back end uploads again.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "layout.h"
#include "mem_need.h"
#include "platform/platform.h"

#define PAGE 0x1000ull
#define GiB (1ull << 30)
#define LOW_SEARCH_FROM 0x100000000ull
#define LOW_SIZE (14 * GiB)        // stack + heap block: room for a Switch 2 heap (HEAP_CAP)
#define STACK_OFF 0x10000000ull
#define STACK_SIZE 0x10000000ull
#define HEAP_OFF (STACK_OFF + STACK_SIZE + 0x10000000ull)
#define HEAP_CAP (12 * GiB)        // heap region: a Switch 2 application pool is 9809 MiB (the 12 GB console arrangement)
#define ALIAS_SIZE (64 * GiB)
#define ALIAS_ALIGN (4 * GiB)
#define ALIAS_PAGES (ALIAS_SIZE / PAGE)
#define DEFAULT_POOL rt_app_pool_bytes  // retail application pool of the title (generated: NX2NAVI 0xCD500000, Switch 2 9809 MiB)
#define SYSTEM_RESOURCE_BUDGET (32ull << 20)
#define MAX_STACK_MAPS 4096
#define REEXEC_LIMIT 20

// Horizon memory states, permissions, attributes and results used here.
enum { ST_FREE = 0, ST_CODE = 3, ST_CODEDATA = 4, ST_NORMAL = 5, ST_STACK = 0xB, ST_THREADLOCAL = 0xC,
       ST_INACCESSIBLE = 0x10 };
enum { PERM_NONE = 0, PERM_R = 1, PERM_RW = 3, PERM_RX = 5 };
enum { ATTR_PERMISSION_LOCKED = 16 };
#define R_OK 0u
#define R_INVALID_ADDRESS 0xcc01u   // misaligned address
#define R_INVALID_SIZE 0xca01u
#define R_INVALID_NEW_PERM 0xd801u
#define R_INVALID_STATE 0xd401u     // InvalidCurrentMemory
#define R_INVALID_REGION 0xdc01u
#define R_LIMIT_REACHED 0x10801u

static RtGuestLayout L;
static uint64_t mods_lo, mods_hi;   // fixed module window
static PlatMutex lock = PLAT_MUTEX_INIT;
static uint64_t heap_size, alias_bytes;
static int heap_lazy;               // NX2NAVI_HEAP_LAZY, Windows: the heap is committed page run by page run on first use
static uint64_t heap_lazy_faults, heap_lazy_bytes;   // accesses that committed heap pages, and the bytes they committed
static uint64_t heap_lazy_ns, heap_lazy_max_ns;      // time in those faults, lock wait included, since the last report
static uint8_t *alias_pages;        // 1 per mapped 4 KiB page of the alias region
typedef struct { uint64_t dst, src, size; } StackMap;
static StackMap stack_maps[MAX_STACK_MAPS];
static unsigned nstack_maps;
static uint8_t *module_perm[MOD_COUNT];   // per data page of each module: perm | ATTR_PERMISSION_LOCKED

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static int reserve_at(uint64_t at, uint64_t size, int watch) { return plat_mem_reserve(at, size, watch); }

// ---- frozen regions
// A 2 MiB region of the watched reservations that nothing wrote for a while can be made read-only (mem_freeze), so the
// back end's write-watch queries skip it: no page of a frozen region changes before a thaw. The first write faults and
// thaw_handler makes the region writable again before the write retries. A kernel write into guest memory (a file
// read) fails on a read-only page instead of faulting, so file reads thaw their buffer first (mem_thaw), and so does
// every commit, decommit and protection change here. A region stays thawed for FRZ_HOLD_MS after a mem_thaw: a file
// read still in flight writes its buffer later. frz_lock is a leaf lock: nothing writes guest memory under it.
#define FRZ_SHIFT 21
#define FRZ_SIZE (1ull << FRZ_SHIFT)
#define FRZ_HOLD_MS 5000u
static volatile uint8_t frz[RT_ADDRESS_LIMIT >> FRZ_SHIFT];
static uint32_t thaw_ms[RT_ADDRESS_LIMIT >> FRZ_SHIFT];   // frz_lock: GetTickCount of the last thaw request, 0 never
// Hot regions. A region a write thaws soon after its freeze is one the game keeps writing (the buffers its loaders
// stream an area into): frozen again as soon as it was idle a moment, it was thawed again moments later. Running
// through Hyrule Field froze and thawed about 8,000 regions in 6,000 frames, and while the loaders wrote and the
// driver locked new memory each thaw's VirtualQuery took 15 to 20 ms: a loader thread stalled 78 to 111 ms in thaws in
// each of the hitches of 125 to 140 ms of that walk, with the game's main thread waiting for it. So a write that thaws a
// region within FRZ_QUICK_MS of its freeze holds the region thawed for its hot time, FRZ_HOT_MIN_MS at first and
// doubled at each such thaw up to FRZ_HOT_MAX_MS; a region that stayed frozen longer starts again from nothing. A
// freeze only saves later write-watch queries over the region, which a region written that often needs anyway.
#define FRZ_QUICK_MS 60000u
#define FRZ_HOT_MIN_MS 4000u
#define FRZ_HOT_MAX_MS 600000u
static uint32_t frz_ms[RT_ADDRESS_LIMIT >> FRZ_SHIFT];        // frz_lock: GetTickCount | 1 of its last freeze, 0 never
static uint32_t frz_hot_ms[RT_ADDRESS_LIMIT >> FRZ_SHIFT];    // frz_lock: its hot time now, 0 not hot
static uint32_t frz_hot_until[RT_ADDRESS_LIMIT >> FRZ_SHIFT]; // frz_lock: no freeze before this GetTickCount, 0 none
static volatile int64_t frz_hot_holds;                        // quick thaws that held a region, since the last stats
static volatile int32_t nfrz;
static PlatMutex frz_lock = PLAT_MUTEX_INIT;
static volatile int64_t frz_count[4];   // freezes; thaws by a write fault, by mem_thaw, by a mapping change
// Thaws outside frz_lock (NX2NAVI_FREEZE_ASYNC, default 1; 0: every thaw under the lock, as before). While the game
// streams an area it creates and finalizes up to 1,500 memory pools a second, the driver pins and unpins the imported
// ones, and the VirtualQuery and VirtualProtect calls of a thaw then take 10-20 ms each. Under frz_lock that held the
// replay (mem_lazy_skip, the freeze pass) and every other thread that faulted or thawed, a 25-45 ms frame. So a thaw by
// a write fault or by mem_thaw marks its regions FRZ_THAWING under the lock, makes their pages read-write without it and
// ends them under it again. A region being thawed is no longer frozen for anyone who asks (a freeze, an arm, a probe
// leave it alone); a fault in it waits for the thaw to end and runs again; a mapping change waits for it first
// (thaw_wait_locked), so a late VirtualProtect never undoes the change; mem_thaw makes its pages read-write itself.
// freeze_locked keeps the runs it made read-only (frz_runs), so a thaw needs no VirtualQuery.
#define FRZ_THAWING 2
static int frz_async = 1;               // the setting, read in mem_init
typedef struct { uint8_t n; uint16_t run[16][2]; } FrzRuns;   // pages of the region, [first, end)
static FrzRuns *frz_runs[RT_ADDRESS_LIMIT >> FRZ_SHIFT];      // frz_lock (written at a freeze): NULL or n 0 = not kept
// vm_lock, a leaf lock held around every write-watch query (written_pages) and around the page calls of a freeze and a
// thaw: on Windows each kind takes the process's address-space lock, and with both running at once a game thread's
// thaw waited about 16 ms per page call behind the replay's queries, 100-300 ms for a thaw of several runs (a frame of
// that length about every second standing in Hyrule Field on an RX 580). Taken in turns here, a thaw waits for one
// query at most.
static PlatMutex vm_lock = PLAT_MUTEX_INIT;
static volatile int64_t frz_async_n;    // thaws done outside the lock, since the start
static volatile int64_t frz_protect_fails;   // VirtualProtect failures of those thaws, since the start

// Probed regions (NX2NAVI_WB_PROBE=1, a diagnosis). The back end copies what the GPU wrote into a mirrored pool back
// into guest memory after every submission, in case the CPU or a host path reads it; whether anything does is what
// the probe finds out. A 2 MiB region that lies wholly inside a range just written back is made inaccessible
// (mem_probe). Its first read or write faults: probe_fault gives the region back the protection it had (read-only
// when frozen, else read-write), notes the address, the instruction and the thread, and the access runs again. The
// bytes are in guest memory all along, so the probe only watches. A probed region is not frozen anew (freeze_locked
// finds pages that are not read-write), and every thaw, which precedes file reads into guest memory and mapping
// changes, ends the probe of its region first.
#define PRB_HITS 64
static volatile uint8_t prb[RT_ADDRESS_LIMIT >> FRZ_SHIFT];
static volatile int32_t nprb, prb_on;
static struct { uint64_t addr, ip; uint32_t tid; uint8_t write; } prb_hit[PRB_HITS];   // frz_lock: the first of each report
static int prb_nhit;                    // frz_lock
static uint64_t prb_arms, prb_ends[2];  // frz_lock: regions armed; probes ended by a read, by a write

static int watched(uint64_t addr, uint64_t size) {
    const RtGuestLayout *g = &L;
    return (addr >= g->aslr.base && size <= LOW_SIZE && addr - g->aslr.base <= LOW_SIZE - size) ||
           (addr >= g->alias.base && size <= ALIAS_SIZE && addr - g->alias.base <= ALIAS_SIZE - size);
}

// frz_lock held exclusively: the pages of a probed region get back what they had, read-only in a frozen region.
static void unprobe_locked(uint64_t r) {
    if (!prb[r]) return;
    int prot = frz[r] == 1 ? PLAT_PROT_READ : PLAT_PROT_RW;
    uint64_t a = r << FRZ_SHIFT, end = a + FRZ_SIZE, b;
    for (; a < end; a = b)   // every inaccessible run is the probe's: mem_probe took only regions without one
        if (plat_mem_state(a, end, &b) == PLAT_PROT_NONE && !plat_mem_protect(a, b - a, prot))
            rt_fatal("memory: ending the probe of %#llx+%#llx failed", (unsigned long long)a, (unsigned long long)(b - a));
    __atomic_store_n(&prb[r], 0, __ATOMIC_RELEASE);
    plat_dec32(&nprb);
}

// Lazy write-back (NX2NAVI_WB_LAZY; unset: on under Windows except on AMD's own driver, off elsewhere, as
// wb_lazy_choice.h decides and gives the reasons for; vk_memory.c "lazy write-back"). It was off
// by default in 0.7.0: with it on, single frames came out with the whole scene's shadowing missing, 10 to 180 of 1,800
// presents standing in Hyrule Field with other programs keeping the CPU busy and none found with it off. That was
// not this code's doing: frames were drawn from copies taken before the game's signal that their data was written
// (vk_exec_xfer.c "events the CPU signals"), and with the write-backs gone the frames came faster and the signal fell
// into the replay more often. With that fixed, two such runs under eight busy threads showed none.
// What a shader wrote into a mirrored pool is
// copied back into guest memory at the end of every submission, in case the CPU reads it. Ocarina's CPU does not read
// most of it: standing in Hyrule Field, 66 MiB a frame went back over PCIe, 7 to 9 ms of each frame's 13 to 15 ms of
// GPU time, and the write-back probe found no access to it in 40 s. So a range that went back once is armed here: its
// whole pages are made inaccessible (mem_lazy_arm), and while it stays armed the back end leaves its later writes in
// the mirror (mem_lazy_skip marks it stale). The first read or write of an armed page faults: lazy_fault has the
// back end copy the range's bytes down now (lz_fetch, a GPU copy after everything submitted so far), gives the pages
// back and runs the access again, and the range stays out for a while (lz_hot: 1 s, doubling to 32 s while it keeps
// being touched), written back every submission as before; by default only the piece around the address goes that
// way and the rest of the range stays armed (NX2NAVI_WB_LAZY_CHUNK, below). Every thaw (before the kernel writes or reads guest
// memory, and before mapping changes) releases the armed ranges of its regions the same way.
// A region with armed pages is not frozen (freeze_locked wants every page read-write), and a frozen one is not armed.
// A fetch brings what the GPU work handed over so far wrote. The part of a submission that is being recorded has left
// its writes out as well, and makes up for it when it finds a range it skipped no longer armed (vk_memory.c
// lazy_part_check). Between that check and the part's hand-over a release would be seen by neither side, so the
// replay opens a window there (mem_lazy_window) and a fault on an armed page waits for it to close: the fetch then
// has the part's writes. Only a fault of the replay thread itself, a wait of 20 ms, or a release by a thaw goes ahead
// inside the window (lz_gaps counts the faults): that part's writes to the range then reach guest memory with the
// range's next write-back.
// The list of armed segments has a lock of its own, lz_lock, held only for a few steps over the list and never across a
// system call or a fetch: the replay asks it at every draw that writes a mirrored pool (mem_lazy_skip) and at the end of
// every part (mem_lazy_armed, mem_lazy_window). Under frz_lock it waited for freezes, thaws and arms on other threads,
// whose page queries and protection changes take that lock for tens to hundreds of microseconds: 8% of the replay
// thread's samples standing in Hyrule Field on an RX 580 (0.7.1, lazy write-back on by default). A change of the list
// holds both locks (frz_lock first); a read holds either.
typedef struct { uint64_t lo, hi, h0, h1; int64_t delta; uint8_t stale; } LzSeg;   // pages [lo, hi); buffer offset = address + delta
static PlatMutex lz_lock = PLAT_MUTEX_INIT;   // inside frz_lock, never the other way round
static LzSeg *lzs;                      // changed under frz_lock and lz_lock, read under either
static int lz_n, lz_cap;                // the same
static volatile uint8_t lzr[RT_ADDRESS_LIMIT >> FRZ_SHIFT];   // a segment reaches into the region
static uint32_t lz_hot[RT_ADDRESS_LIMIT >> FRZ_SHIFT];        // frz_lock: plat_now_ms() | 1 until which nothing is armed here
static uint8_t lz_back[RT_ADDRESS_LIMIT >> FRZ_SHIFT];        // frz_lock: seconds of the region's last hold
static volatile int32_t lz_on;
static int lz_want;                     // the setting, read in mem_init
static void (*lz_fetch)(uint64_t h0, uint64_t h1, uint64_t off, uint64_t size);
// The back end's test that the mirror holds every CPU write to [lo, hi) (vk_memory.c lazy_clean): a fetch copies the
// mirror's bytes over the whole range, so a CPU write the mirror does not have yet would be lost to it.
static int (*lz_clean)(uint64_t lo, uint64_t hi, void *ctx);
static uint64_t lz_unclean;             // frz_lock: arms refused since the last report because the mirror lacked a CPU write
// frz_lock: bytes mem_lazy_arm left unarmed since the last report, by reason: the region is frozen, held after a touch
// (lz_hot), held after a thaw, has pages that are not read-write (or lacks a CPU write in the mirror), or the bytes lie
// in the partial pages at a range's ends. They are written back at every submission that writes them.
enum { LZ_NO_FROZEN, LZ_NO_HOT, LZ_NO_THAW, LZ_NO_PAGES, LZ_NO_EDGE, LZ_NO_N };
static uint64_t lz_no[LZ_NO_N], lz_no_small, lz_arm_calls;   // ranges without a whole page; ranges written back
// NX2NAVI_WB_LAZY_CHUNK (default 0 = a touch releases its whole range and holds its regions; off because with it on
// the graphics device was lost about 2 s after a save load in Hyrule Field at night, in 7 of about 22 loads on an
// RTX 5090, a GPU read fault at the start of a large buffer, and in 0 of 15 with it off; why is not known yet): the
// first touch of an armed range gives back only the LZ_CHUNK bytes around the address, fetched alone, and only that
// piece is held (lz_hots, 1 s doubling to 32 s like a region's hold); the rest of the range stays armed. More than
// LZ_HEAVY_N touches in one region within LZ_HEAVY_MS are a reader going through the range: its range goes whole
// and its regions are held, as before, and while the region is touched again within LZ_HEAVY_KEEP_MS its touches go
// that way at once, so such a reader pays LZ_HEAVY_N faults more, once.
// Ocarina's CPU reads a few words at the start of buffers its shaders fill (12 bytes into ranges of 6.3 and 15.2 MiB,
// among others), about twice a second in a village and in Hyrule Field. Each read gave back the whole range and held
// every 2 MiB region it reached for up to 32 s, in which nothing was armed: 22 to 26 MiB a frame went back to guest
// memory at the end of every submission although nothing read them, inside the GPU work the game's fence waits for.
// With the pieces 1.7 MiB a frame go back, on 17 to 18 faults in 1,000 frames where there were 14 to 16 and with the
// same time spent fetching (4 to 5 ms in 1,000 frames), and the village ran at 135 to 138 frames a second where it
// had run at 122 to 124.
#define LZ_CHUNK_SHIFT 16
#define LZ_CHUNK (1ull << LZ_CHUNK_SHIFT)
#define LZ_HOTS 1024                    // a power of two
#define LZ_HEAVY_N 4
#define LZ_HEAVY_MS 1000
#define LZ_HEAVY_KEEP_MS 60000
typedef struct { uint64_t key; uint32_t until; uint8_t back; } LzHot;   // key: chunk number + 1, 0 free; until as lz_hot
static LzHot lz_hots[LZ_HOTS];          // frz_lock: open addressing, a short linear probe
static int lz_nhots;                    // frz_lock: entries ever used (0: no lookup needed)
static uint32_t lz_touch_ms[RT_ADDRESS_LIMIT >> FRZ_SHIFT];   // frz_lock: plat_now_ms() | 1 of the first piece of the region's run
static uint8_t lz_touch_n[RT_ADDRESS_LIMIT >> FRZ_SHIFT];     // frz_lock: pieces given back in that run
static uint32_t lz_heavy_ms[RT_ADDRESS_LIMIT >> FRZ_SHIFT];   // frz_lock: ... of its last touch taken as a reader's
static int lz_chunk;                    // the setting, read in mem_init
static uint64_t lz_pieces, lz_piece_bytes, lz_heavy;   // frz_lock, since the last report
static uint64_t lz_foreign;             // frz_lock, since the start: segments of another buffer mem_lazy_skip gave back
// frz_lock: the first faults on armed pages since the last report, for its lines (trace perf)
#define LZ_NOTES 8
static struct { uint64_t addr, lo, hi, out; uint32_t tid; uint8_t write, stale, hold; } lz_note[LZ_NOTES];
static int lz_nnote;
// frz_lock: since the last report: faults by a read, by a write, other releases, fetches, their bytes and time
static uint64_t lz_faults[2], lz_other, lz_fetches, lz_fetch_bytes, lz_fetch_ns, lz_gaps;
static volatile uint32_t lz_win_tid;    // lz_lock: the replay thread's id while its part is between check and hand-over
// For a fatal error's summary line (mem_lazy_note): written under frz_lock and read without it, since the thread that
// fails may be the one inside a fetch.
static volatile uint64_t lz_fetch_total;   // fetches since the start
static volatile uint32_t lz_fetch_ms;      // plat_now_ms() | 1 at the end of the last one; 0: none yet
static volatile uint32_t lz_fetch_tid;     // the thread inside a fetch now; 0: none
static volatile uint64_t lz_fetch_now;     // that fetch's bytes

static void lazy_flags_locked(uint64_t lo, uint64_t hi) {
    for (uint64_t r = lo >> FRZ_SHIFT; r <= (hi - 1) >> FRZ_SHIFT; r++) {
        int any = 0;
        for (int j = 0; j < lz_n && !any; j++)
            any = lzs[j].lo < ((r + 1) << FRZ_SHIFT) && lzs[j].hi > (r << FRZ_SHIFT);
        __atomic_store_n(&lzr[r], (uint8_t)any, __ATOMIC_RELEASE);
    }
}

// frz_lock and lz_lock held: segment i leaves the list, with the stale mark mem_lazy_skip may have set until now. A
// later mem_lazy_skip no longer finds it and writes its range back as ever.
static LzSeg lazy_take_locked(int i) {
    LzSeg s = lzs[i];
    lzs[i] = lzs[--lz_n];
    return s;
}

static void lazy_gone_locked(LzSeg s, int why);

// frz_lock held exclusively. Segment i goes: its bytes come down first when the mirror's are newer, then its pages
// are read-write again. why: 0 a read fault, 1 a write fault (both hold the regions back), else a thaw or the mirror
// going away.
static void lazy_drop_locked(int i, int why) {
    plat_mutex_lock(&lz_lock);
    LzSeg s = lazy_take_locked(i);
    plat_mutex_unlock(&lz_lock);
    lazy_gone_locked(s, why);
}

// frz_lock held exclusively: the pages of s, out of the list already, get their bytes and their access back.
static void lazy_gone_locked(LzSeg s, int why) {
    if (s.stale && lz_fetch) {
        uint64_t t0 = plat_now_ns();
        __atomic_store_n(&lz_fetch_now, s.hi - s.lo, __ATOMIC_RELAXED);
        __atomic_store_n(&lz_fetch_tid, plat_thread_id(), __ATOMIC_RELEASE);
        lz_fetch(s.h0, s.h1, (uint64_t)((int64_t)s.lo + s.delta), s.hi - s.lo);
        __atomic_store_n(&lz_fetch_ms, (uint32_t)plat_now_ms() | 1, __ATOMIC_RELAXED);
        __atomic_store_n(&lz_fetch_total, lz_fetch_total + 1, __ATOMIC_RELAXED);
        __atomic_store_n(&lz_fetch_tid, 0, __ATOMIC_RELEASE);
        lz_fetches++, lz_fetch_bytes += s.hi - s.lo, lz_fetch_ns += plat_now_ns() - t0;
    }
    for (uint64_t a = s.lo, b; a < s.hi; a = b)
        if (plat_mem_state(a, s.hi, &b) == PLAT_PROT_NONE && !plat_mem_protect(a, b - a, PLAT_PROT_RW))
            rt_fatal("memory: giving back the lazily written pages %#llx+%#llx failed", (unsigned long long)a,
                     (unsigned long long)(b - a));
    lazy_flags_locked(s.lo, s.hi);
    if (why > 1) { lz_other++; return; }
    lz_faults[why]++;
    uint32_t now = (uint32_t)plat_now_ms();
    for (uint64_t r = s.lo >> FRZ_SHIFT; r <= (s.hi - 1) >> FRZ_SHIFT; r++) {
        int again = lz_hot[r] && (int32_t)(now - lz_hot[r]) < 60000;   // touched again within a minute of its last hold
        uint8_t b = again ? (uint8_t)(lz_back[r] < 32 ? lz_back[r] * 2 : 32) : 1;
        lz_back[r] = b ? b : 1;
        lz_hot[r] = (now + (uint32_t)lz_back[r] * 1000u) | 1;
    }
}

// frz_lock held. The hold entry of chunk c; make: 1 takes a free slot of the probe for it, or the one whose hold
// ended longest ago.
static LzHot *lazy_hot_locked(uint64_t c, int make) {
    uint32_t h = (uint32_t)((c * 0x9e3779b97f4a7c15ull) >> 40);
    LzHot *take = NULL;
    for (int k = 0; k < 8; k++) {
        LzHot *e = &lz_hots[(h + (uint32_t)k) & (LZ_HOTS - 1)];
        if (e->key == c + 1) return e;
        if (!take || (take->key && (!e->key || (int32_t)(e->until - take->until) < 0))) take = e;
    }
    if (!make) return NULL;
    *take = (LzHot){c + 1, 0, 0};
    lz_nhots++;
    return take;
}

static int lazy_chunk_held_locked(uint64_t c, uint32_t now) {
    if (!lz_nhots) return 0;
    const LzHot *e = lazy_hot_locked(c, 0);
    return e && e->until && (int32_t)(e->until - now) > 0;
}

static void lazy_push_locked(LzSeg s) {
    if (lz_n == lz_cap) {
        lz_cap = lz_cap ? lz_cap * 2 : 256;
        lzs = realloc(lzs, sizeof *lzs * (size_t)lz_cap);
        if (!lzs) rt_fatal("memory: out of memory for the lazily written ranges");
    }
    lzs[lz_n++] = s;
}

// frz_lock held exclusively. A fault at a in segment i: the LZ_CHUNK piece around a goes (its bytes come down first
// when the mirror's are newer, then its pages are read-write again) and is held; what is left of the segment on
// either side stays armed. Returns the piece's size.
static uint64_t lazy_piece_locked(int i, uint64_t a, int write) {
    LzSeg s = lzs[i];
    uint64_t c0 = a & ~(LZ_CHUNK - 1), c1 = c0 + LZ_CHUNK;
    if (c0 < s.lo) c0 = s.lo;
    if (c1 > s.hi) c1 = s.hi;
    if (s.stale && lz_fetch) {
        uint64_t t0 = plat_now_ns();
        __atomic_store_n(&lz_fetch_now, c1 - c0, __ATOMIC_RELAXED);
        __atomic_store_n(&lz_fetch_tid, plat_thread_id(), __ATOMIC_RELEASE);
        lz_fetch(s.h0, s.h1, (uint64_t)((int64_t)c0 + s.delta), c1 - c0);
        __atomic_store_n(&lz_fetch_ms, (uint32_t)plat_now_ms() | 1, __ATOMIC_RELAXED);
        __atomic_store_n(&lz_fetch_total, lz_fetch_total + 1, __ATOMIC_RELAXED);
        __atomic_store_n(&lz_fetch_tid, 0, __ATOMIC_RELEASE);
        lz_fetches++, lz_fetch_bytes += c1 - c0, lz_fetch_ns += plat_now_ns() - t0;
    }
    for (uint64_t x = c0, y; x < c1; x = y)
        if (plat_mem_state(x, c1, &y) == PLAT_PROT_NONE && !plat_mem_protect(x, y - x, PLAT_PROT_RW))
            rt_fatal("memory: giving back the lazily written pages %#llx+%#llx failed", (unsigned long long)x,
                     (unsigned long long)(y - x));
    lzs[i] = lzs[--lz_n];
    if (c0 > s.lo) lazy_push_locked((LzSeg){s.lo, c0, s.h0, s.h1, s.delta, s.stale});
    if (c1 < s.hi) lazy_push_locked((LzSeg){c1, s.hi, s.h0, s.h1, s.delta, s.stale});
    lazy_flags_locked(s.lo, s.hi);
    lz_faults[write != 0]++, lz_pieces++, lz_piece_bytes += c1 - c0;
    uint32_t now = (uint32_t)plat_now_ms();
    LzHot *e = lazy_hot_locked(a >> LZ_CHUNK_SHIFT, 1);
    int again = e->until && (int32_t)(now - e->until) < 60000;   // touched again within a minute of its last hold
    e->back = again ? (uint8_t)(e->back < 32 ? e->back * 2 : 32) : 1;
    if (!e->back) e->back = 1;
    e->until = (now + (uint32_t)e->back * 1000u) | 1;
    return c1 - c0;
}

// frz_lock held exclusively: every segment that reaches into region r goes.
static void lazy_drop_region_locked(uint64_t r, int why) {
    for (int i = lz_n; i-- > 0;)
        if (lzs[i].lo < ((r + 1) << FRZ_SHIFT) && lzs[i].hi > (r << FRZ_SHIFT)) lazy_drop_locked(i, why);
}

static int lazy_covered_locked(uint64_t lo, uint64_t hi) {
    for (int i = 0; i < lz_n; i++)
        if (lzs[i].lo <= lo && lzs[i].hi >= hi) return 1;
    return 0;
}

// frz_lock held exclusively: [x, e), inside one region, becomes inaccessible when every committed page of it is
// read-write, or inaccessible as part of a segment already; anything else (a read-only page, a hole, a page the game
// made inaccessible itself) leaves it as it is. Then the back end checks that the mirror ctx belongs to holds every CPU
// write to [x, e) (lz_clean): after the protection no write can come in, so a write that came before it is in the
// write-watch or the page generations. When the mirror lacks one, the pages are given back and nothing is armed: the
// CPU wrote into the range after its write-back reached guest memory and before this arm, and the next sync of the
// mirror carries that write up; a fetch before it would have copied the mirror's older bytes over it (single frames
// with another frame's lighting constants, or a black scene).
static int lazy_protect_locked(uint64_t x, uint64_t e, void *ctx) {
    uint64_t run[16][2];
    int k = 0;
    for (uint64_t a = x, b; a < e; a = b) {
        int st = plat_mem_state(a, e, &b);
        if (st == PLAT_PROT_RW) {
            if (k == 16) return 0;
            run[k][0] = a, run[k][1] = b, k++;
        } else if (st != PLAT_PROT_NONE || !lazy_covered_locked(a, b)) return 0;
    }
    for (int i = 0; i < k; i++)
        if (!plat_mem_protect(run[i][0], run[i][1] - run[i][0], PLAT_PROT_NONE)) {
            while (i--) plat_mem_protect(run[i][0], run[i][1] - run[i][0], PLAT_PROT_RW);
            return 0;
        }
    if (lz_clean && !lz_clean(x, e, ctx)) {
        for (int i = 0; i < k; i++) plat_mem_protect(run[i][0], run[i][1] - run[i][0], PLAT_PROT_RW);
        lz_unclean++;
        return 0;
    }
    return 1;
}

// Arms the whole pages of [a, a + n), which the back end has just written back from the buffer h0 into h1 at offset
// off: they fault on their next access. Returns the bytes armed now. Not in a frozen, probed, held or thaw-held region.
uint64_t mem_lazy_arm(uint64_t a, uint64_t n, uint64_t h0, uint64_t h1, uint64_t off, void *ctx) {
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED) || a >= RT_ADDRESS_LIMIT || !watched(a, n)) return 0;
    uint64_t lo = (a + PAGE - 1) & ~(PAGE - 1), hi = (a + n) & ~(PAGE - 1), armed = 0;
    int64_t delta = (int64_t)off - (int64_t)a;
    plat_mutex_lock(&frz_lock);
    lz_arm_calls++;
    if (hi <= lo) {
        lz_no[LZ_NO_EDGE] += n, lz_no_small++;
        plat_mutex_unlock(&frz_lock);
        return 0;
    }
    lz_no[LZ_NO_EDGE] += n - (hi - lo);
    if (lazy_covered_locked(lo, hi)) {
        plat_mutex_unlock(&frz_lock);
        return 0;
    }
    uint32_t now = (uint32_t)plat_now_ms();
    for (uint64_t x = lo, e; x < hi; x = e) {
        uint64_t r = x >> FRZ_SHIFT;
        e = (r + 1) << FRZ_SHIFT;
        if (e > hi) e = hi;
        if (lz_nhots) {   // a held piece stays out: this turn arms what lies before it, the next ones what lies behind
            uint64_t c = x >> LZ_CHUNK_SHIFT, y = x;
            while (y < e && lazy_chunk_held_locked(c, now)) y = ++c << LZ_CHUNK_SHIFT;
            if (y > x) {
                if (y > e) y = e;
                lz_no[LZ_NO_HOT] += y - x;
                e = y;
                continue;
            }
            for (c++; c << LZ_CHUNK_SHIFT < e; c++)
                if (lazy_chunk_held_locked(c, now)) {
                    e = c << LZ_CHUNK_SHIFT;
                    break;
                }
        }
        int no = frz[r] ? LZ_NO_FROZEN : prb[r] ? LZ_NO_PAGES : lz_hot[r] && (int32_t)(lz_hot[r] - now) > 0 ? LZ_NO_HOT :
                 thaw_ms[r] && (int32_t)(now - thaw_ms[r]) < (int32_t)FRZ_HOLD_MS ? LZ_NO_THAW :
                 !lazy_protect_locked(x, e, ctx) ? LZ_NO_PAGES : -1;
        if (no >= 0) {
            lz_no[no] += e - x;
            continue;
        }
        uint64_t slo = x, shi = e;
        uint8_t stale = 0;
        plat_mutex_lock(&lz_lock);
        for (int j = 0; j < lz_n; j++)   // one segment for what touches in the same buffer
            if (lzs[j].h0 == h0 && lzs[j].delta == delta && lzs[j].lo <= shi && lzs[j].hi >= slo) {
                if (lzs[j].lo < slo) slo = lzs[j].lo;
                if (lzs[j].hi > shi) shi = lzs[j].hi;
                stale |= lzs[j].stale;
                lzs[j] = lzs[--lz_n];
                j = -1;
            }
        lazy_push_locked((LzSeg){slo, shi, h0, h1, delta, stale});
        plat_mutex_unlock(&lz_lock);
        __atomic_store_n(&lzr[r], 1, __ATOMIC_RELEASE);
        armed += e - x;
    }
    plat_mutex_unlock(&frz_lock);
    return armed;
}

// The armed parts of [a, a + n), sorted, into out (at most max); they count as stale from here on: the caller leaves
// the bytes it is about to write there in the mirror h0, at buffer offset address + delta. Returns how many.
// A segment there that was armed for another buffer (a mirror evicted since, whose range a write-back armed while the
// eviction ran) is given back instead, without a fetch: its buffer may be gone, and since its mirror went every GPU
// write went to guest memory, so guest memory holds the bytes. Marked stale, it would have its next touch copy from the
// freed buffer, a GPU read fault that loses the device.
int mem_lazy_skip(uint64_t a, uint64_t n, uint64_t h0, int64_t delta, uint64_t out[][2], int max) {
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED) || !n || a >= RT_ADDRESS_LIMIT || a + n > RT_ADDRESS_LIMIT) return 0;
    int any = 0, k = 0;
    for (uint64_t r = a >> FRZ_SHIFT; r <= (a + n - 1) >> FRZ_SHIFT && !any; r++) any = lzr[r];
    if (!any) return 0;
    // A segment armed for another buffer goes back under frz_lock (lazy_gone_locked), with arms held off until the
    // rest is marked, as before lz_lock; that is rare, and the replay's per-draw check holds lz_lock only.
    int foreign = 0;
    plat_mutex_lock(&lz_lock);
    for (int i = 0; i < lz_n && !foreign; i++)
        foreign = lzs[i].lo < a + n && lzs[i].hi > a && (lzs[i].h0 != h0 || lzs[i].delta != delta);
    if (foreign) {
        plat_mutex_unlock(&lz_lock);
        plat_mutex_lock(&frz_lock);
        for (;;) {
            plat_mutex_lock(&lz_lock);
            int i = lz_n;
            while (i-- > 0 && !(lzs[i].lo < a + n && lzs[i].hi > a && (lzs[i].h0 != h0 || lzs[i].delta != delta))) {}
            LzSeg s;
            if (i >= 0) {
                lzs[i].stale = 0;
                lz_foreign++;
                s = lazy_take_locked(i);
            }
            plat_mutex_unlock(&lz_lock);
            if (i < 0) break;
            lazy_gone_locked(s, 4);
        }
        plat_mutex_lock(&lz_lock);
    }
    for (int i = 0; i < lz_n && k < max; i++) {
        uint64_t lo = lzs[i].lo > a ? lzs[i].lo : a, hi = lzs[i].hi < a + n ? lzs[i].hi : a + n;
        if (lo >= hi) continue;
        lzs[i].stale = 1;
        int at = k++;
        for (; at > 0 && out[at - 1][0] > lo; at--) out[at][0] = out[at - 1][0], out[at][1] = out[at - 1][1];
        out[at][0] = lo, out[at][1] = hi;
    }
    plat_mutex_unlock(&lz_lock);
    if (foreign) plat_mutex_unlock(&frz_lock);
    return k;
}

// 0 when no page of [a, a + n) can be inaccessible right now (armed by the lazy write-back or probed), so the CPU can
// read it without a fault; 1 when one may be.
int mem_lazy_touches(uint64_t a, uint64_t n) {
    if (!n) return 0;
    if (a >= RT_ADDRESS_LIMIT || a + n > RT_ADDRESS_LIMIT || __atomic_load_n(&prb_on, __ATOMIC_RELAXED)) return 1;
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED)) return 0;
    int any = 0;
    for (uint64_t r = a >> FRZ_SHIFT; r <= (a + n - 1) >> FRZ_SHIFT && !any; r++) any = __atomic_load_n(&lzr[r], __ATOMIC_ACQUIRE);
    if (!any) return 0;
    int hit = 0;
    plat_mutex_lock(&lz_lock);
    for (int i = 0; i < lz_n && !hit; i++) hit = lzs[i].lo < a + n && lzs[i].hi > a;
    plat_mutex_unlock(&lz_lock);
    return hit;
}

// [a, a + n) is armed as a whole still.
int mem_lazy_armed(uint64_t a, uint64_t n) {
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED)) return 0;
    plat_mutex_lock(&lz_lock);
    int ok = lazy_covered_locked(a, a + n);
    plat_mutex_unlock(&lz_lock);
    return ok;
}

// Copies each of n ranges of guest memory, src[i] = {address, bytes}, to dst[i] when every page of it can be read
// without a fault; ok[i] says which were. An armed range, a probed region or a page the game made inaccessible is left
// out instead of faulting on the caller's thread (vk_memory.c mirror_capture runs inside a submission, where a fetch
// could not be sent). frz_lock is held over all of them, so nothing is armed or probed meanwhile; a range in regions
// with neither (lzr, prb: both change under that lock, with the pages' protection) is copied without asking the system
// for its pages' state. Returns how many were copied.
int mem_copy_readable(uint8_t *const *dst, const uint64_t (*src)[2], int n, uint8_t *ok) {
    int got = 0;
    plat_mutex_lock(&frz_lock);
    for (int i = 0; i < n; i++) {
        uint64_t a = src[i][0], len = src[i][1];
        ok[i] = 0;
        if (!len || a >= RT_ADDRESS_LIMIT || a + len > RT_ADDRESS_LIMIT) continue;
        int quick = 1, can = 1;
        for (uint64_t r = a >> FRZ_SHIFT; quick && r <= (a + len - 1) >> FRZ_SHIFT; r++)
            quick = !__atomic_load_n(&lzr[r], __ATOMIC_RELAXED) && !__atomic_load_n(&prb[r], __ATOMIC_RELAXED);
        for (uint64_t x = a, e; !quick && can && x < a + len; x = e) {
            int st = plat_mem_state(x, a + len, &e);
            can = (st == PLAT_PROT_RW || st == PLAT_PROT_READ) && e > x;
        }
        if (!can) continue;
        memcpy(dst[i], (const void *)(uintptr_t)a, len);
        ok[i] = 1, got++;
    }
    plat_mutex_unlock(&frz_lock);
    return got;
}

// Releases what is armed in [a, a + n): before the buffers a fetch would read go away.
void mem_lazy_release(uint64_t a, uint64_t n) {
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED) || !n) return;
    plat_mutex_lock(&frz_lock);
    for (int i = lz_n; i-- > 0;)
        if (lzs[i].lo < a + n && lzs[i].hi > a) lazy_drop_locked(i, 4);
    plat_mutex_unlock(&frz_lock);
}

// The back end is ready to fetch: arming starts. 0 when the setting is off (or the probe is on, which watches the
// same pages).
int mem_lazy_enable(void (*fetch)(uint64_t h0, uint64_t h1, uint64_t off, uint64_t size),
                    int (*clean)(uint64_t lo, uint64_t hi, void *ctx)) {
    if (!lz_want) return 0;
    lz_fetch = fetch, lz_clean = clean;
    __atomic_store_n(&lz_on, 1, __ATOMIC_RELEASE);
    return 1;
}

// Opens (under lz_lock: a fault's segment is out of the list before it, or the fault sees it) or closes the window of
// the replay's part.
void mem_lazy_window(int open) {
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED)) return;
    if (!open) {
        __atomic_store_n(&lz_win_tid, 0, __ATOMIC_RELEASE);
        return;
    }
    plat_mutex_lock(&lz_lock);   // a fault takes its segment out before this or sees the window
    __atomic_store_n(&lz_win_tid, plat_thread_id(), __ATOMIC_RELEASE);
    plat_mutex_unlock(&lz_lock);
}

static int lazy_fault(uint64_t a, int write) {
    if (a >= RT_ADDRESS_LIMIT || !__atomic_load_n(&lzr[a >> FRZ_SHIFT], __ATOMIC_ACQUIRE)) return 0;
    uint32_t me = plat_thread_id();
    for (int tries = 0;; tries++) {
        plat_mutex_lock(&frz_lock);
        plat_mutex_lock(&lz_lock);   // the window's test and the segment's leaving the list in one step
        int i = 0;
        while (i < lz_n && !(a >= lzs[i].lo && a < lzs[i].hi)) i++;
        if (i == lz_n) {
            plat_mutex_unlock(&lz_lock);
            plat_mutex_unlock(&frz_lock);
            return 0;
        }
        uint32_t w = __atomic_load_n(&lz_win_tid, __ATOMIC_ACQUIRE);
        if (w && w != me && tries < 400) {   // up to 20 ms: after the hand-over the fetch has that part's writes
            plat_mutex_unlock(&lz_lock);
            plat_mutex_unlock(&frz_lock);
            plat_wait_ns(50000);
            continue;
        }
        if (w) lz_gaps++;
        int k = lz_nnote < LZ_NOTES ? lz_nnote++ : -1;
        if (k >= 0)
            lz_note[k].addr = a, lz_note[k].lo = lzs[i].lo, lz_note[k].hi = lzs[i].hi, lz_note[k].tid = me,
            lz_note[k].write = write != 0, lz_note[k].stale = lzs[i].stale;
        uint64_t r = a >> FRZ_SHIFT, out = lzs[i].hi - lzs[i].lo;
        uint32_t now = (uint32_t)plat_now_ms();
        int reader = lz_heavy_ms[r] && (int32_t)(now - lz_heavy_ms[r]) < LZ_HEAVY_KEEP_MS;
        if (lz_chunk && !reader) {
            if (lz_touch_ms[r] && (int32_t)(now - lz_touch_ms[r]) < LZ_HEAVY_MS) {
                if (++lz_touch_n[r] > LZ_HEAVY_N) reader = 1, lz_heavy++;
            } else {
                lz_touch_ms[r] = now | 1, lz_touch_n[r] = 1;
            }
        }
        if (reader) lz_heavy_ms[r] = now | 1;
        if (lz_chunk && !reader) {   // the piece leaves the list and the rest goes back into it under lz_lock
            out = lazy_piece_locked(i, a, write != 0);
            plat_mutex_unlock(&lz_lock);
            if (k >= 0) lz_note[k].hold = lazy_hot_locked(a >> LZ_CHUNK_SHIFT, 1)->back;
        } else {   // the segment leaves the list under lz_lock; its pages come back without it
            LzSeg s = lazy_take_locked(i);
            plat_mutex_unlock(&lz_lock);
            lazy_gone_locked(s, write != 0);
            if (k >= 0) lz_note[k].hold = lz_back[r];
        }
        if (k >= 0) lz_note[k].out = out;
        plat_mutex_unlock(&frz_lock);
        return 1;
    }
}

// Test (NX2NAVI_WB_LAZY_TOUCH): the first addresses of up to max armed segments, another set each call, for a caller
// that reads them to make the faults a game's access would.
int mem_lazy_pick(uint64_t *addr, int max) {
    static int next;
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED)) return 0;
    int k = 0;
    plat_mutex_lock(&frz_lock);
    for (; k < max && k < lz_n; k++) addr[k] = lzs[(next + k) % lz_n].lo;
    next += k;
    plat_mutex_unlock(&frz_lock);
    return k;
}

// The lazy write-back's state as a phrase for a fatal error's summary line, "" while nothing is armed by it. No lock:
// the count of armed ranges may be a moment old, and a fetch in progress is named with its thread, which tells a game
// that ended inside a fetch (the device lost under it, or its wait run out) from one that ended elsewhere.
void mem_lazy_note(char *out, size_t n) {
    *out = 0;
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED)) return;
    uint32_t tid = __atomic_load_n(&lz_fetch_tid, __ATOMIC_ACQUIRE), last = __atomic_load_n(&lz_fetch_ms, __ATOMIC_RELAXED);
    int k = snprintf(out, n, "%d ranges armed, %llu fetches so far", lz_n,
                     (unsigned long long)__atomic_load_n(&lz_fetch_total, __ATOMIC_RELAXED));
    if (k < 0 || (size_t)k >= n) return;
    if (tid)
        snprintf(out + k, n - (size_t)k, ", one of %llu bytes in progress on thread %u%s",
                 (unsigned long long)__atomic_load_n(&lz_fetch_now, __ATOMIC_RELAXED), tid,
                 tid == plat_thread_id() ? " (this one)" : "");
    else if (last)
        snprintf(out + k, n - (size_t)k, ", the last ended %u ms ago", (uint32_t)plat_now_ms() - last);
}

// One log line since the last one; skipped: the bytes the back end left in the mirror since then.
void mem_lazy_report(uint64_t frame, uint64_t skipped) {
    static int64_t async_said = -1;
    int64_t an = __atomic_load_n(&frz_async_n, __ATOMIC_RELAXED), af = __atomic_load_n(&frz_protect_fails, __ATOMIC_RELAXED);
    if ((rt_trace & RT_TRACE_PERF) && an != async_said) {
        async_said = an;
        rt_log("mem: thaws outside the freeze lock so far %lld (NX2NAVI_FREEZE_ASYNC), page protections that failed there "
               "%lld", (long long)an, (long long)af);
    }
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED)) return;
    plat_mutex_lock(&frz_lock);
    uint64_t bytes = 0, fr = lz_faults[0], fw = lz_faults[1], other = lz_other, nf = lz_fetches, fb = lz_fetch_bytes,
             fns = lz_fetch_ns, gaps = lz_gaps, unclean = lz_unclean, no[LZ_NO_N];
    int n = lz_n;
    for (int i = 0; i < lz_n; i++) bytes += lzs[i].hi - lzs[i].lo;
    lz_faults[0] = lz_faults[1] = lz_other = lz_fetches = lz_fetch_bytes = lz_fetch_ns = lz_gaps = lz_unclean = 0;
    uint64_t small = lz_no_small, calls = lz_arm_calls;
    int nnote = lz_nnote;
    __typeof__(lz_note) note;
    memcpy(note, lz_note, sizeof note);
    lz_nnote = 0;
    memcpy(no, lz_no, sizeof no);
    memset(lz_no, 0, sizeof lz_no);
    lz_no_small = lz_arm_calls = 0;
    uint64_t pieces = lz_pieces, piece_bytes = lz_piece_bytes, heavy = lz_heavy, foreign = lz_foreign;
    lz_pieces = lz_piece_bytes = lz_heavy = 0;
    plat_mutex_unlock(&frz_lock);
    static uint64_t foreign_said;
    if (foreign != foreign_said) {
        foreign_said = foreign;
        rt_log("mem: lazy write-back: %llu armed ranges named a mirror that is no longer the pool's, given back without "
               "a fetch so far", (unsigned long long)foreign);
    }
    rt_log("mem: lazy write-back at frame %llu: %d ranges armed, %.1f MiB; since the last line %.1f MiB left in the "
           "mirror, %llu read and %llu write faults (%llu inside a part's window), %llu other releases, %llu fetches of "
           "%.1f MiB in %.2f ms, %llu arms refused (a CPU write not in the mirror yet)", (unsigned long long)frame, n,
           bytes / 1048576.0, skipped / 1048576.0, (unsigned long long)fr, (unsigned long long)fw,
           (unsigned long long)gaps, (unsigned long long)other, (unsigned long long)nf, fb / 1048576.0, fns / 1e6,
           (unsigned long long)unclean);
    for (int i = 0; i < nnote && (rt_trace & RT_TRACE_PERF); i++)
        rt_log("mem: lazy write-back: thread %u %s %#llx, %llu KiB into an armed range of %.2f MiB at %#llx; %llu KiB of "
               "it given back%s and held for %u s", note[i].tid, note[i].write ? "wrote" : "read",
               (unsigned long long)note[i].addr, (unsigned long long)((note[i].addr - note[i].lo) >> 10),
               (note[i].hi - note[i].lo) / 1048576.0, (unsigned long long)note[i].lo, (unsigned long long)(note[i].out >> 10),
               note[i].stale ? " (fetched)" : " (its bytes were current)", note[i].hold);
    if (pieces || heavy)
        rt_log("mem: lazy write-back at frame %llu: %llu touches gave back a piece only (%.2f MiB in all), %llu regions "
               "were touched too often in a second and give back whole ranges for now",
               (unsigned long long)frame, (unsigned long long)pieces, piece_bytes / 1048576.0, (unsigned long long)heavy);
    if (no[LZ_NO_FROZEN] + no[LZ_NO_HOT] + no[LZ_NO_THAW] + no[LZ_NO_PAGES] + no[LZ_NO_EDGE])
        rt_log("mem: lazy write-back at frame %llu: written back and not armed since the last line: %.1f MiB in frozen "
               "regions, %.1f MiB in regions the CPU touched lately, %.1f MiB in regions held after a thaw, %.1f MiB in "
               "regions with pages that are not read-write or with a CPU write not in the mirror, %.2f MiB in the "
               "partial pages at the ends of ranges; %llu ranges written back, %llu of them without a whole page",
               (unsigned long long)frame, no[LZ_NO_FROZEN] / 1048576.0, no[LZ_NO_HOT] / 1048576.0,
               no[LZ_NO_THAW] / 1048576.0, no[LZ_NO_PAGES] / 1048576.0, no[LZ_NO_EDGE] / 1048576.0,
               (unsigned long long)calls, (unsigned long long)small);
}

// Makes the pages region r's freeze made read-only read-write again: the runs freeze_locked kept, else every read-only
// run (freeze_locked left reserved holes, heap pages given back, as they were; every committed page of a frozen region
// was read-write at the freeze, and mapping changes thaw or wait before they run, so these are all ours). strict: under
// frz_lock, a failure ends the process; outside it (a thaw in flight, FRZ_THAWING) the pages are what the freeze left,
// and a failure is only counted.
static void thaw_pages_vm(uint64_t r, int strict);

static void thaw_pages(uint64_t r, int strict) {
    plat_mutex_lock(&vm_lock);
    thaw_pages_vm(r, strict);
    plat_mutex_unlock(&vm_lock);
}

static void thaw_pages_vm(uint64_t r, int strict) {   // vm_lock held
    uint64_t base = r << FRZ_SHIFT;
    const FrzRuns *k = frz_runs[r];
    if (k && k->n) {
        for (int i = 0; i < k->n; i++) {
            uint64_t a = base + ((uint64_t)k->run[i][0] << 12), n = (uint64_t)(k->run[i][1] - k->run[i][0]) << 12;
            if (!plat_mem_protect(a, n, PLAT_PROT_RW)) {
                if (strict)
                    rt_fatal("memory: thawing %#llx+%#llx failed", (unsigned long long)a, (unsigned long long)n);
                plat_inc64(&frz_protect_fails);
            }
        }
        return;
    }
    uint64_t a = base, end = a + FRZ_SIZE, b;
    for (; a < end; a = b)
        if (plat_mem_state(a, end, &b) == PLAT_PROT_READ && !plat_mem_protect(a, b - a, PLAT_PROT_RW)) {
            if (strict) rt_fatal("memory: thawing %#llx+%#llx failed", (unsigned long long)a, (unsigned long long)(b - a));
            plat_inc64(&frz_protect_fails);
        }
}

// frz_lock held exclusively: what a thaw does before the pages (the hold, the lazy write-back's ranges, a probe).
// Returns 1 when the region is frozen.
static int thaw_begin_locked(uint64_t r, int why) {
    if (why == 2) thaw_ms[r] = (uint32_t)plat_now_ms() | 1;
    if (lzr[r]) lazy_drop_region_locked(r, why + 1);   // not a fault on an armed page: no hold
    if (prb[r]) unprobe_locked(r);
    return frz[r] == 1;
}

static void thaw_end_locked(uint64_t r, int why);

// frz_lock held exclusively. why: 1 a write fault, 2 mem_thaw (holds the region thawed), 3 a mapping change here.
// A region another thread is thawing (FRZ_THAWING) gets its pages made read-write here too, and is ended here.
static void thaw_locked(uint64_t r, int why) {
    if (!thaw_begin_locked(r, why) && frz[r] != FRZ_THAWING) return;
    thaw_pages(r, 1);
    __atomic_store_n(&frz[r], FRZ_THAWING, __ATOMIC_RELEASE);
    thaw_end_locked(r, why);
}

// frz_lock held exclusively: the end of a thaw. Nothing when another thread ended it meanwhile.
static void thaw_end_locked(uint64_t r, int why) {
    if (frz[r] != FRZ_THAWING) return;
    __atomic_store_n(&frz[r], 0, __ATOMIC_RELEASE);
    plat_dec32(&nfrz);
    plat_inc64(&frz_count[why]);
    if (why == 1) {   // a write: a quick one makes the region hot, a late one ends its hot time
        uint32_t now = (uint32_t)plat_now_ms();
        if (frz_ms[r] && now - frz_ms[r] < FRZ_QUICK_MS) {
            uint32_t h = frz_hot_ms[r] ? frz_hot_ms[r] * 2 : FRZ_HOT_MIN_MS;
            frz_hot_ms[r] = h > FRZ_HOT_MAX_MS ? FRZ_HOT_MAX_MS : h;
            frz_hot_until[r] = (now + frz_hot_ms[r]) | 1;
            plat_inc64(&frz_hot_holds);
        } else {
            frz_hot_ms[r] = 0;
        }
    }
}

// frz_lock held exclusively.
static void thaw_range_locked(uint64_t a, uint64_t n, int why) {
    if (!n || a >= RT_ADDRESS_LIMIT) return;
    uint64_t r0 = a >> FRZ_SHIFT, r1 = (a + n - 1 < RT_ADDRESS_LIMIT ? a + n - 1 : RT_ADDRESS_LIMIT - 1) >> FRZ_SHIFT;
    for (uint64_t r = r0; r <= r1; r++) thaw_locked(r, why);
}

// frz_lock held exclusively: returns once no region of [a, a + n) is being thawed outside the lock (FRZ_THAWING),
// releasing the lock while it waits. A mapping change runs after this: the thaw's VirtualProtect then cannot land on
// the pages after the change and undo it.
static void thaw_wait_locked(uint64_t a, uint64_t n) {
    if (!n || a >= RT_ADDRESS_LIMIT) return;
    uint64_t r0 = a >> FRZ_SHIFT, r1 = (a + n - 1 < RT_ADDRESS_LIMIT ? a + n - 1 : RT_ADDRESS_LIMIT - 1) >> FRZ_SHIFT;
    for (uint64_t r = r0; r <= r1; r++)
        while (__atomic_load_n(&frz[r], __ATOMIC_ACQUIRE) == FRZ_THAWING) {
            plat_mutex_unlock(&frz_lock);
            plat_wait_ns(20000);
            plat_mutex_lock(&frz_lock);
        }
}

// Lazy heap (NX2NAVI_HEAP_LAZY, Windows only; off by default). Windows charges every committed page to the system's
// commit at once, and SetHeapSize asks for the whole application heap up front: in Castle Town after Hyrule Field the
// heap held 6.8 GiB of commit of which 3.4 GiB had ever been resident, with 0.45 GB in the page file. With it on,
// SetHeapSize only moves the heap's end, and a page of the heap is committed when something first reaches it: an access
// of the game's or the runtime's code faults into access_fault, which commits the reserved runs of the 64 KiB around
// it; a kernel read or write of a buffer (mem_thaw, mem_kernel_access) and a pointer check (mem_host_readable)
// commit their range first, as the kernel and VirtualQuery do not fault. On Linux pages are committed on first touch
// already.
// frz_lock held exclusively. Commits the reserved pages of [a, a + n) inside the heap; returns the bytes committed. A
// page another path committed is left alone: committing it again would make a read-only or inaccessible page (a
// freeze, a probe, an armed lazy write-back) read-write behind their backs. The regions it commits into are thawed
// first, as for any mapping change, so that no frozen region holds a read-write page.
static uint64_t heap_lazy_commit_locked(uint64_t a, uint64_t n) {
    uint64_t lo = a > L.heap.base ? a : L.heap.base, top = L.heap.base + __atomic_load_n(&heap_size, __ATOMIC_ACQUIRE);
    uint64_t end = a + n < a ? UINT64_MAX : a + n, hi = end < top ? end : top, got = 0;
    if (lo >= hi) return 0;
    lo &= ~(PAGE - 1), hi = align_up(hi, PAGE);
    int thawed = 0;
    for (uint64_t x = lo, y; x < hi; x = y) {
        if (plat_mem_state(x, hi, &y) != PLAT_MEM_RESERVED) continue;
        if (!thawed) thaw_range_locked(lo, hi - lo, 3), thawed = 1;
        if (!plat_mem_commit(x, y - x))
            rt_fatal("memory: committing the heap's pages %#llx+%#llx on first use failed: the system's commit is "
                     "used up", (unsigned long long)x, (unsigned long long)(y - x));
        got += y - x;
    }
    heap_lazy_bytes += got;
    return got;
}

// Commits the heap pages of [a, a + n) that the lazy heap left reserved; nothing when it is off.
void mem_kernel_access(uint64_t a, uint64_t n) {
    if (!heap_lazy || !n) return;
    plat_mutex_lock(&frz_lock);
    heap_lazy_commit_locked(a, n);
    plat_mutex_unlock(&frz_lock);
}

// For a host scan of guest memory (camera_speed.c rewrite): the end of the run of pages at a that the lazy heap left
// reserved, at most end, or 0 when the page at a is not such a page. Nothing was ever written there, so a scan steps
// over the run; reading it would commit every page of the heap and undo what the setting saves.
uint64_t mem_untouched_run(uint64_t a, uint64_t end) {
    if (!heap_lazy || a < L.heap.base || a >= L.heap.base + L.heap.size) return 0;
    uint64_t y, x = a & ~(PAGE - 1);
    return plat_mem_state(x, end, &y) == PLAT_MEM_RESERVED && y > a ? y : 0;
}

// plat_mem_readable for a pointer the host is about to read: untouched heap pages of a lazy heap count as readable
// (they are committed here, and read zero as they would have).
int mem_host_readable(uint64_t a, uint64_t n) {
    mem_kernel_access(a, n);
    return plat_mem_readable(a, n);
}

// An access fault on a reserved page of the lazy heap: the 64 KiB around it is committed and the access runs again.
// 0 when nothing was reserved there, so that a frozen, probed or armed page goes on to its own hook (a page another
// thread committed meanwhile is retried by thaw_fault or the end of access_fault, as the heap is watched).
static int heap_lazy_fault(uint64_t a) {
    uint64_t heap = L.heap.base;
    if (a < heap || a - heap >= __atomic_load_n(&heap_size, __ATOMIC_ACQUIRE)) return 0;
    uint64_t t0 = plat_now_ns();
    plat_mutex_lock(&frz_lock);
    uint64_t got = heap_lazy_commit_locked(a & ~0xFFFFull, 0x10000);
    if (got) {
        uint64_t ns = plat_now_ns() - t0;
        heap_lazy_faults++, heap_lazy_ns += ns;
        if (ns > heap_lazy_max_ns) heap_lazy_max_ns = ns;
    }
    plat_mutex_unlock(&frz_lock);
    return got != 0;
}

void mem_thaw(uint64_t a, uint64_t n) {
    plat_mutex_lock(&frz_lock);   // also when nothing is frozen: the hold keeps a read's buffer writable
    if (heap_lazy || !frz_async || !n || a >= RT_ADDRESS_LIMIT) {
        if (heap_lazy) {
            thaw_wait_locked(a, n);   // it commits pages: a mapping change
            heap_lazy_commit_locked(a, n);   // the kernel's write would fail on a reserved page as well
        }
        thaw_range_locked(a, n, 2);
        plat_mutex_unlock(&frz_lock);
        return;
    }
    // The regions frozen now are marked FRZ_THAWING and made read-write without the lock (see FRZ_THAWING), up to
    // MANY at a time; those another thread is thawing are made read-write here too, as the caller's kernel access needs
    // every page of them now, and that thread ends them.
    enum { MANY = 64 };
    uint64_t r0 = a >> FRZ_SHIFT, r1 = (a + n - 1 < RT_ADDRESS_LIMIT ? a + n - 1 : RT_ADDRESS_LIMIT - 1) >> FRZ_SHIFT;
    for (uint64_t r = r0; r <= r1;) {
        uint64_t mine[MANY], theirs[MANY];
        int nm = 0, nt = 0;
        for (; r <= r1 && nm < MANY && nt < MANY; r++) {
            if (thaw_begin_locked(r, 2)) {
                __atomic_store_n(&frz[r], FRZ_THAWING, __ATOMIC_RELEASE);
                mine[nm++] = r;
            } else if (frz[r] == FRZ_THAWING) {
                theirs[nt++] = r;
            }
        }
        if (!nm && !nt) continue;
        plat_mutex_unlock(&frz_lock);
        for (int i = 0; i < nm; i++) thaw_pages(mine[i], 0);
        for (int i = 0; i < nt; i++) thaw_pages(theirs[i], 0);
        plat_add64(&frz_async_n, nm);
        plat_mutex_lock(&frz_lock);
        for (int i = 0; i < nm; i++) thaw_end_locked(mine[i], 2);
    }
    plat_mutex_unlock(&frz_lock);
}

// frz_lock held exclusively. Makes the committed pages of region r read-only when every one of them is read-write;
// reserved holes (heap pages given back) stay as they are, and thaw_locked finds the read-only runs again.
static int freeze_locked(uint64_t r) {
    uint64_t a = r << FRZ_SHIFT, end = a + FRZ_SIZE, run[16][2];
    int n = 0, ok = 1;
    plat_mutex_lock(&vm_lock);
    for (uint64_t x = a, y; x < end && ok; x = y) {
        int st = plat_mem_state(x, end, &y);
        if (st >= PLAT_PROT_NONE) {
            if (st != PLAT_PROT_RW || n == 16) ok = 0;
            else run[n][0] = x, run[n][1] = y, n++;
        } else if (st != PLAT_MEM_RESERVED) ok = 0;
    }
    for (int i = 0; ok && i < n; i++)
        if (!plat_mem_protect(run[i][0], run[i][1] - run[i][0], PLAT_PROT_READ)) {
            while (i--) plat_mem_protect(run[i][0], run[i][1] - run[i][0], PLAT_PROT_RW);
            ok = 0;
        }
    plat_mutex_unlock(&vm_lock);
    if (!ok) return 0;
    // the runs, for a thaw without VirtualQuery (thaw_pages); kept per region from its first freeze on
    if (n && !frz_runs[r]) frz_runs[r] = calloc(1, sizeof *frz_runs[r]);
    if (frz_runs[r]) {
        frz_runs[r]->n = (uint8_t)n;
        for (int i = 0; i < n; i++)
            frz_runs[r]->run[i][0] = (uint16_t)((run[i][0] - a) >> 12), frz_runs[r]->run[i][1] = (uint16_t)((run[i][1] - a) >> 12);
    }
    return n > 0;
}

int mem_freeze(uint64_t a) {
    if ((a & (FRZ_SIZE - 1)) || !watched(a, FRZ_SIZE)) return 0;
    uint64_t r = a >> FRZ_SHIFT;
    int ok = 0;
    plat_mutex_lock(&frz_lock);
    uint32_t now = (uint32_t)plat_now_ms();
    if (frz[r]) ok = 1;
    else if (thaw_ms[r] && (int32_t)(now - thaw_ms[r]) < (int32_t)FRZ_HOLD_MS) ok = 0;   // |1 may be ahead
    else if (frz_hot_until[r] && (int32_t)(frz_hot_until[r] - now) > 0) ok = 0;         // hot: held thawed
    else if (freeze_locked(r)) {
        frz_ms[r] = now | 1;
        __atomic_store_n(&frz[r], 1, __ATOMIC_RELEASE);
        plat_inc32(&nfrz);
        plat_inc64(&frz_count[0]);
        ok = 1;
    }
    plat_mutex_unlock(&frz_lock);
    return ok;
}

int mem_frozen(uint64_t a) { return a < RT_ADDRESS_LIMIT && __atomic_load_n(&frz[a >> FRZ_SHIFT], __ATOMIC_ACQUIRE); }

// Probes the 2 MiB regions that lie wholly inside [a, a + n): makes them inaccessible until their first access.
// Returns how many it armed. Not a region a file read may still be writing into (the thaw hold), nor one with a page
// that is not plainly read-write (read-only when frozen): those are left alone.
int mem_probe(uint64_t a, uint64_t n) {
    if (!__atomic_load_n(&prb_on, __ATOMIC_RELAXED) || n < FRZ_SIZE || a >= RT_ADDRESS_LIMIT || !watched(a, n)) return 0;
    uint64_t r0 = (a + FRZ_SIZE - 1) >> FRZ_SHIFT, r1 = (a + n) >> FRZ_SHIFT;   // [r0, r1)
    int armed = 0;
    plat_mutex_lock(&frz_lock);
    for (uint64_t r = r0; r < r1; r++) {
        if (prb[r]) continue;
        if (thaw_ms[r] && (int32_t)((uint32_t)plat_now_ms() - thaw_ms[r]) < (int32_t)FRZ_HOLD_MS) continue;
        int want = frz[r] ? PLAT_PROT_READ : PLAT_PROT_RW, k = 0, ok = 1;
        uint64_t run[16][2];
        for (uint64_t x = r << FRZ_SHIFT, end = x + FRZ_SIZE, y; x < end && ok; x = y) {
            int st = plat_mem_state(x, end, &y);
            if (st >= PLAT_PROT_NONE) {
                if (st != want || k == 16) ok = 0;
                else run[k][0] = x, run[k][1] = y, k++;
            } else if (st != PLAT_MEM_RESERVED) ok = 0;
        }
        if (!ok || !k) continue;
        for (int i = 0; i < k && ok; i++)
            if (!plat_mem_protect(run[i][0], run[i][1] - run[i][0], PLAT_PROT_NONE)) {
                while (i--) plat_mem_protect(run[i][0], run[i][1] - run[i][0], want);
                ok = 0;
            }
        if (!ok) continue;
        __atomic_store_n(&prb[r], 1, __ATOMIC_RELEASE);
        plat_inc32(&nprb);
        prb_arms++, armed++;
    }
    plat_mutex_unlock(&frz_lock);
    return armed;
}

// Pages of guest memory may be inaccessible (the probe, or the lazy write-back): a kernel read of them has to thaw first.
int mem_probing(void) { return __atomic_load_n(&prb_on, __ATOMIC_RELAXED) || __atomic_load_n(&lz_on, __ATOMIC_RELAXED); }

// The platform's access-fault hook while the probe is on: the first access of a probed region ends its probe and is
// noted; 1 runs the access again. A read that faulted in a region another thread gave back meanwhile retries too; a
// write goes on to thaw_fault, and any other fault to the crash handler.
static int probe_fault(uint64_t a, int write, uint64_t ip) {
    if (a >= RT_ADDRESS_LIMIT || !watched(a, 1)) return 0;
    uint64_t r = a >> FRZ_SHIFT;
    plat_mutex_lock(&frz_lock);
    int was = prb[r];
    if (was) {
        unprobe_locked(r);
        prb_ends[write != 0]++;
        if (prb_nhit < PRB_HITS) prb_hit[prb_nhit++] = (typeof(prb_hit[0])){a, ip, plat_thread_id(), (uint8_t)(write != 0)};
    }
    plat_mutex_unlock(&frz_lock);
    if (was) return 1;
    if (write) return 0;
    uint64_t end;
    return plat_mem_state(a & ~(PAGE - 1), (a & ~(PAGE - 1)) + PAGE, &end) >= PLAT_PROT_READ;
}

// The platform's access-fault hook: a reserved page of the lazy heap first, then an armed page of the lazy write-back,
// then the probe. A read that faulted on
// a page another thread gave back meanwhile runs again; a write goes on to thaw_fault.
static int access_fault(uint64_t a, int write, uint64_t ip) {
    if (heap_lazy && heap_lazy_fault(a)) return 1;
    if (__atomic_load_n(&lz_on, __ATOMIC_RELAXED) && lazy_fault(a, write)) return 1;
    if (__atomic_load_n(&prb_on, __ATOMIC_RELAXED)) return probe_fault(a, write, ip);
    if (write || a >= RT_ADDRESS_LIMIT || !watched(a, 1)) return 0;
    uint64_t end;
    return plat_mem_state(a & ~(PAGE - 1), (a & ~(PAGE - 1)) + PAGE, &end) >= PLAT_PROT_READ;
}

// One log block for the probe since the last one: how many regions were armed, how many probes a read or a write
// ended, and the first accesses by instruction (an address inside the executable as nx2navi+offset, for the linker
// map or nm). Nothing while the probe is off.
void mem_probe_report(uint64_t frame) {
    if (!__atomic_load_n(&prb_on, __ATOMIC_RELAXED)) return;
    typeof(prb_hit[0]) hit[PRB_HITS];
    plat_mutex_lock(&frz_lock);
    uint64_t arms = prb_arms, reads = prb_ends[0], writes = prb_ends[1];
    int n = prb_nhit;
    memcpy(hit, prb_hit, sizeof hit[0] * (size_t)n);
    prb_arms = prb_ends[0] = prb_ends[1] = 0, prb_nhit = 0;
    plat_mutex_unlock(&frz_lock);
    rt_log("mem: write-back probe at frame %llu: %llu regions of 2 MiB armed, probes ended by %llu reads and %llu writes "
           "since the last line; %d probed now", (unsigned long long)frame, (unsigned long long)arms,
           (unsigned long long)reads, (unsigned long long)writes, (int)__atomic_load_n(&nprb, __ATOMIC_RELAXED));
    uintptr_t exe = plat_exe_base();
    for (int i = 0, lines = 0; i < n && lines < 12; i++) {
        if (!hit[i].ip) continue;   // counted with an earlier access of the same instruction
        int same = 1;
        for (int j = i + 1; j < n; j++)
            if (hit[j].ip == hit[i].ip && hit[j].write == hit[i].write) same++, hit[j].ip = 0;
        rt_log("mem:   %d x a %s at %#llx.. from %s%#llx (thread %u)", same, hit[i].write ? "write" : "read",
               (unsigned long long)hit[i].addr, hit[i].ip >= exe && hit[i].ip - exe < (1ull << 32) ? "nx2navi+" : "",
               (unsigned long long)(hit[i].ip >= exe && hit[i].ip - exe < (1ull << 32) ? hit[i].ip - exe : hit[i].ip),
               hit[i].tid);
        lines++;
    }
}

void mem_freeze_stats(uint64_t *frozen, uint64_t *freezes, uint64_t *fault_thaws, uint64_t *other_thaws,
                      uint64_t *hot_holds) {
    *frozen = (uint64_t)__atomic_load_n(&nfrz, __ATOMIC_RELAXED);
    *hot_holds = (uint64_t)plat_xchg64(&frz_hot_holds, 0);
    *freezes = (uint64_t)plat_xchg64(&frz_count[0], 0);
    *fault_thaws = (uint64_t)plat_xchg64(&frz_count[1], 0);
    *other_thaws = (uint64_t)plat_xchg64(&frz_count[2], 0) + (uint64_t)plat_xchg64(&frz_count[3], 0);
}

// The first write into a frozen region thaws it and retries. A fault in a region another thread thawed meanwhile
// retries too; any other fault goes on to the crash handler.
// The platform's write-fault hook (plat_mem_fault_hook): 1 retries the write.
static int thaw_fault(uint64_t a) {
    if (a >= RT_ADDRESS_LIMIT || !watched(a, 1)) return 0;
    uint64_t r = a >> FRZ_SHIFT;
    plat_mutex_lock(&frz_lock);
    int was = frz[r];
    if (was == FRZ_THAWING) {   // another thread's thaw: the write runs again once it ended
        while (frz[r] == FRZ_THAWING) {
            plat_mutex_unlock(&frz_lock);
            plat_wait_ns(20000);
            plat_mutex_lock(&frz_lock);
        }
        plat_mutex_unlock(&frz_lock);
        return 1;
    }
    if (frz_async && was == 1) {   // its pages outside the lock (FRZ_THAWING)
        thaw_begin_locked(r, 1);
        __atomic_store_n(&frz[r], FRZ_THAWING, __ATOMIC_RELEASE);
        plat_mutex_unlock(&frz_lock);
        thaw_pages(r, 0);
        plat_inc64(&frz_async_n);
        plat_mutex_lock(&frz_lock);
        thaw_end_locked(r, 1);
        plat_mutex_unlock(&frz_lock);
        return 1;
    }
    thaw_locked(r, 1);
    plat_mutex_unlock(&frz_lock);
    if (was) return 1;
    uint64_t end;
    return plat_mem_state(a & ~(PAGE - 1), (a & ~(PAGE - 1)) + PAGE, &end) == PLAT_PROT_RW;
}

// Mapping changes thaw the regions they touch and run under frz_lock, so no freeze sees them half done.
static void commit(uint64_t a, uint64_t n, const char *what) {
    plat_mutex_lock(&frz_lock);
    thaw_wait_locked(a, n);
    thaw_range_locked(a, n, 3);
    int ok = !n || plat_mem_commit(a, n);
    plat_mutex_unlock(&frz_lock);
    if (!ok) rt_fatal("memory: commit %s %#llx+%#llx failed", what, (unsigned long long)a, (unsigned long long)n);
}

static void decommit(uint64_t a, uint64_t n, const char *what) {
    plat_mutex_lock(&frz_lock);
    thaw_wait_locked(a, n);
    thaw_range_locked(a, n, 3);
    int ok = !n || plat_mem_decommit(a, n);
    plat_mutex_unlock(&frz_lock);
    if (!ok) rt_fatal("memory: decommit %s %#llx+%#llx failed", what, (unsigned long long)a, (unsigned long long)n);
}

static void protect(uint64_t a, uint64_t n, int prot, const char *what) {
    plat_mutex_lock(&frz_lock);
    thaw_wait_locked(a, n);
    thaw_range_locked(a, n, 3);
    int ok = !n || plat_mem_protect(a, n, prot);
    plat_mutex_unlock(&frz_lock);
    if (!ok) rt_fatal("memory: protect %s %#llx+%#llx failed", what, (unsigned long long)a, (unsigned long long)n);
}

// The process is re-run when a region is taken: Windows places DLLs, heaps and thread stacks at random addresses,
// and a fresh process gets a fresh layout.
static void reexec_or_fail(const char *what) {
    const char *e = cfg_get("NX2NAVI_REEXEC");
    int n = e ? atoi(e) : 0;
    if (n >= REEXEC_LIMIT) rt_fatal("memory: %s is not free after %d attempts", what, n);
    char buf[16];
    snprintf(buf, sizeof buf, "%d", n + 1);
    cfg_setenv("NX2NAVI_REEXEC", buf);
    plat_reexec();
}

void mem_init(void) {
    if (L.pool) return;
    // Fixed window: modules, kernel region, host-function addresses.
    mods_lo = MOD_MAIN_BASE;
    mods_hi = align_up(rt_layout[rt_layout_count - 1].base + rt_layout[rt_layout_count - 1].size, 0x10000);
    if (mods_hi > RT_KERNEL_BASE) rt_fatal("memory: modules end at %#llx, inside the kernel region",
                                           (unsigned long long)mods_hi);
    if (!reserve_at(mods_lo, mods_hi - mods_lo, 0)) reexec_or_fail("the module window");
    if (!reserve_at(RT_KERNEL_BASE, RT_KERNEL_SIZE, 0)) reexec_or_fail("the kernel region");
    if (!reserve_at(RT_HOSTFN_BASE, RT_HOSTFN_SIZE, 0)) reexec_or_fail("the host-function window");

    // Stack and heap block, then the alias region above it, both searched below the module window.
    uint64_t low = 0, alias = 0;
    for (uint64_t at = LOW_SEARCH_FROM; at + LOW_SIZE <= mods_lo && !low; at += GiB)
        if (reserve_at(at, LOW_SIZE, 1)) low = at;
    if (!low) reexec_or_fail("a 14 GiB stack and heap block below 2^39");
    for (uint64_t at = align_up(low + LOW_SIZE, ALIAS_ALIGN); at + ALIAS_SIZE <= mods_lo && !alias; at += ALIAS_ALIGN)
        if (reserve_at(at, ALIAS_SIZE, 1)) alias = at;
    if (!alias) reexec_or_fail("a 64 GiB alias region below 2^39");

    plat_mem_fault_hook(thaw_fault);   // before the crash handler
    alias_pages = calloc(ALIAS_PAGES, 1);
    if (!alias_pages) rt_fatal("memory: cannot allocate the alias page map");
    // nx2navi.ini is read here, once the address space is claimed and before the first setting: the pool size below
    // depends on NX2NAVI_APP_POOL and on the resolution settings (resolution.c decides them once, on this first call).
    cfg_init();
    if (cfg_bool("NX2NAVI_WB_PROBE")) {   // a diagnosis: off, no access fault reaches probe_fault and nothing is probed
        plat_mem_access_hook(access_fault);
        __atomic_store_n(&prb_on, 1, __ATOMIC_RELEASE);
    } else if (cfg_bool("NX2NAVI_WB_LAZY")) {   // armed once the back end wants it and can fetch (mem_lazy_enable)
        plat_mem_access_hook(access_fault);
        lz_want = 1;
        lz_chunk = cfg_bool("NX2NAVI_WB_LAZY_CHUNK");
    }
    const char *fa = cfg_get("NX2NAVI_FREEZE_ASYNC");   // thaws outside frz_lock (FRZ_THAWING); unset: on
    frz_async = !fa || !*fa || atoi(fa) != 0;
    const char *pool = cfg_get("NX2NAVI_APP_POOL");
    L = (RtGuestLayout){
        .aslr = {low, RT_ADDRESS_LIMIT - low},
        .stack = {low + STACK_OFF, STACK_SIZE},
        .heap = {low + HEAP_OFF, HEAP_CAP},
        .alias = {alias, ALIAS_SIZE},
        .kernel = {RT_KERNEL_BASE, RT_KERNEL_SIZE},
        .hostfn = {RT_HOSTFN_BASE, RT_HOSTFN_SIZE},
        .pool = pool ? strtoull(pool, NULL, 0) : DEFAULT_POOL + res_heap_extra_bytes(),   // resolution.c
    };
    if (!L.pool) rt_fatal("memory: NX2NAVI_APP_POOL is 0");
#ifdef _WIN32
    if (cfg_bool("NX2NAVI_HEAP_LAZY")) {   // set before the game's first SetHeapSize
        plat_mem_access_hook(access_fault);
        heap_lazy = 1;
        rt_log("mem: the heap is committed on first use (NX2NAVI_HEAP_LAZY)");
    }
#endif
}

const RtGuestLayout *mem_layout(void) { return &L; }

// The start of a game (main.c, before the boot): one line when the machine's free commit is below what the game takes
// in play (mem_need.h; NX2NAVI_MEM_NEED_MB, 0 no check), so a log that ends in a failed allocation, dropped textures
// or a lost device says at its top that the machine was short before the game took anything. 1 when it was.
// probe: --probe-memneed, which prints the numbers either way.
int mem_start_note(int probe) {
    double proc, used, limit, need = strtod(cfg_value("NX2NAVI_MEM_NEED_MB"), NULL);
    plat_commit_info(&proc, &used, &limit);
    int is_short = mem_need_short(proc, used, limit, need);
    if (probe)
        printf("probe-memneed: system commit %.0f of %.0f MB in use, this process %.0f MB, the game needs %.0f MB "
               "(NX2NAVI_MEM_NEED_MB): %s\n", used, limit, proc, need,
               need <= 0 ? "not checked" : limit < 0 ? "no limit known" : is_short ? "short" : "enough");
    if (!is_short) return 0;
    char note[400] = "";
    plat_commit_note(note, sizeof note);
    rt_log("mem: WARNING: this machine has %.0f MB of memory left for the game and the game takes about %.0f MB in play "
           "(system commit: %.0f of %.0f MB in use; the limit is RAM plus page file%s%s). Short of memory the game "
           "stutters, loses textures or stops with an out-of-memory or lost-device error. Closing other programs or a "
           "larger page file gives it room (mem_need_mb = 0 in nx2navi.ini turns this warning off)",
           mem_need_room(proc, used, limit), need, used, limit, note[0] ? "; " : "", note);
    return 1;
}

// A range inside one watched reservation (stack and heap block, or alias region) only.
static uint64_t written_pages(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max, int reset) {
    if (!max || !size) return 0;
    if (!watched(addr, size)) return UINT64_MAX;
    plat_mutex_lock(&vm_lock);
    uint64_t n = plat_mem_written(addr, size, pages, max, reset);
    plat_mutex_unlock(&vm_lock);
    return n;
}

uint64_t mem_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max) {
    return written_pages(addr, size, pages, max, 1);
}

uint64_t mem_written_peek(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max) {
    return written_pages(addr, size, pages, max, 0);
}

int mem_watched(uint64_t addr, uint64_t size) { return size && watched(addr, size); }

static uint64_t image_bytes(void) {
    uint64_t n = 0;
    for (int i = 0; i < ldr_count(); i++) n += ldr_module(i)->size;
    return align_up(n, PAGE);
}

static int overlaps(uint64_t a, uint64_t n, uint64_t b, uint64_t m) { return a < b + m && b < a + n; }
static int inside(uint64_t a, uint64_t n, RtRange r) { return a >= r.base && n <= r.size && a - r.base <= r.size - n; }

// Bumped (lock held exclusively) by every change of what is mapped or readable: vk_exec_draw.c's texture pool looks the
// slots over unreadable storage up again only then (and now and then).
static uint32_t map_gen;
uint32_t mem_map_gen(void) { return __atomic_load_n(&map_gen, __ATOMIC_ACQUIRE); }

// The application pool is the title's own plus res_heap_extra_bytes (mem_init), and the game asks for a heap of
// nearly all of it: past HEAP_CAP the request below is refused, so this is the most that may be added.
uint64_t mem_heap_extra_max_kib(void) {
    return HEAP_CAP > DEFAULT_POOL ? ((HEAP_CAP - DEFAULT_POOL) & ~0x1fffffull) >> 10 : 0;
}

// ---- 0x01 SetHeapSize
uint32_t mem_set_heap_size(uint64_t size, uint64_t *addr) {
    uint64_t t0 = perf_mark_begin();   // NX2NAVI_TRACE=perf: the commit or decommit, the wait for the lock included
    plat_mutex_lock(&lock);
    if (size > HEAP_CAP || size % 0x200000 || size + image_bytes() > L.pool) {
        plat_mutex_unlock(&lock);
        return R_INVALID_STATE;
    }
    uint64_t was = heap_size;
    if (size > was && !heap_lazy) commit(L.heap.base + was, size - was, "heap");
    __atomic_store_n(&heap_size, size, __ATOMIC_RELEASE);   // before a shrink: no first-use commit past the new end
    if (size < was) decommit(L.heap.base + size, was - size, "heap");   // reserved pages in the range are fine
    uint64_t moved = size > was ? size - was : was - size;
    __atomic_store_n(&map_gen, map_gen + 1, __ATOMIC_RELEASE);
    plat_mutex_unlock(&lock);
    perf_mark_end(PERF_M_GMEM, t0, moved);
    if (addr) *addr = L.heap.base;
    return R_OK;
}

// ---- module data permissions (0x02, 0x03) and module pages for QueryMemory
static int module_index(uint64_t a) {
    for (int i = 0; i < ldr_count(); i++) {
        const RtLoaded *m = ldr_module(i);
        if (a - m->base < m->size) return i;
    }
    return -1;
}

static uint8_t *perm_table(int i) {   // under lock
    if (!module_perm[i]) {
        const RtLoaded *m = ldr_module(i);
        size_t n = (m->size - m->data_off) / PAGE;
        module_perm[i] = malloc(n);
        if (!module_perm[i]) rt_fatal("memory: module permission table");
        memset(module_perm[i], PERM_RW, n);
    }
    return module_perm[i];
}

static unsigned module_page_perm(int i, uint64_t off) {   // under lock
    const RtLoaded *m = ldr_module(i);
    if (off < m->data_off) return off < align_up(m->text_end, PAGE) ? PERM_RX : PERM_R;
    return module_perm[i] ? module_perm[i][(off - m->data_off) / PAGE] : PERM_RW;
}

// The module whose data segment holds [a, a+n), or -1.
static int module_data(uint64_t a, uint64_t n) {
    int i = module_index(a);
    if (i < 0) return -1;
    const RtLoaded *m = ldr_module(i);
    uint64_t off = a - m->base;
    return off >= m->data_off && n <= m->size - off ? i : -1;
}

uint32_t mem_set_permission(uint64_t a, uint64_t n, uint32_t perm) {
    if (a & (PAGE - 1)) return R_INVALID_ADDRESS;
    if (!n || (n & (PAGE - 1))) return R_INVALID_SIZE;
    if (perm != PERM_NONE && perm != PERM_R && perm != PERM_RW) return R_INVALID_NEW_PERM;
    int i = module_data(a, n);
    if (i < 0) return R_INVALID_STATE;
    const RtLoaded *m = ldr_module(i);
    plat_mutex_lock(&lock);
    uint8_t *t = perm_table(i) + (a - m->base - m->data_off) / PAGE;
    for (uint64_t p = 0; p < n / PAGE; p++)
        if (t[p] & ATTR_PERMISSION_LOCKED) {
            plat_mutex_unlock(&lock);
            return R_INVALID_STATE;
        }
    memset(t, (int)perm, n / PAGE);
    protect(a, n, perm == PERM_RW ? PLAT_PROT_RW : perm == PERM_R ? PLAT_PROT_READ : PLAT_PROT_NONE, "module data");
    __atomic_store_n(&map_gen, map_gen + 1, __ATOMIC_RELEASE);
    plat_mutex_unlock(&lock);
    rt_log("%s data permission +%#llx size %#llx perm %u", m->name, (unsigned long long)(a - m->base),
           (unsigned long long)n, perm);
    return R_OK;
}

// The only module attribute the SDK sets is the irreversible RELRO lock.
uint32_t mem_set_attribute(uint64_t a, uint64_t n, uint32_t mask, uint32_t at) {
    if (a & (PAGE - 1)) return R_INVALID_ADDRESS;
    if (!n || (n & (PAGE - 1))) return R_INVALID_SIZE;
    if (mask != ATTR_PERMISSION_LOCKED || at != ATTR_PERMISSION_LOCKED) return R_INVALID_NEW_PERM;
    int i = module_data(a, n);
    if (i < 0) return R_INVALID_STATE;
    const RtLoaded *m = ldr_module(i);
    plat_mutex_lock(&lock);
    uint8_t *t = perm_table(i) + (a - m->base - m->data_off) / PAGE;
    for (uint64_t p = 0; p < n / PAGE; p++) t[p] |= ATTR_PERMISSION_LOCKED;
    plat_mutex_unlock(&lock);
    return R_OK;
}

// ---- 0x04/0x05 MapMemory, UnmapMemory: thread stacks move from heap or alias into the stack region by copy; the
// source is inaccessible while moved and gets the contents back on unmap.
static int alias_all_mapped(uint64_t a, uint64_t n) {   // under lock
    uint64_t first = (a - L.alias.base) / PAGE;
    for (uint64_t i = 0; i < n / PAGE; i++)
        if (!alias_pages[first + i]) return 0;
    return 1;
}

static uint32_t stack_move(int unmap, uint64_t dst, uint64_t src, uint64_t n) {
    if (!n || ((dst | src | n) & (PAGE - 1))) return R_INVALID_ADDRESS;
    if (!inside(dst, n, L.stack)) return R_INVALID_REGION;
    int in_heap = inside(src, n, (RtRange){L.heap.base, heap_size});
    int in_alias = inside(src, n, L.alias);
    if (!in_heap && !in_alias) return R_INVALID_REGION;
    plat_mutex_lock(&lock);
    uint32_t r = R_OK;
    if (in_alias && !alias_all_mapped(src, n)) {
        r = R_INVALID_REGION;
        goto out;
    }
    unsigned found = nstack_maps;
    for (unsigned i = 0; i < nstack_maps; i++)
        if (stack_maps[i].dst == dst && stack_maps[i].src == src && stack_maps[i].size == n) found = i;
    if (unmap) {
        if (found == nstack_maps) {
            r = R_INVALID_STATE;
            goto out;
        }
        protect(src, n, PLAT_PROT_RW, "moved stack source");
        memcpy((void *)(uintptr_t)src, (const void *)(uintptr_t)dst, n);
        decommit(dst, n, "moved stack");
        stack_maps[found] = stack_maps[--nstack_maps];
    } else {
        if (nstack_maps == MAX_STACK_MAPS) {
            r = R_INVALID_STATE;
            goto out;
        }
        for (unsigned i = 0; i < nstack_maps; i++)
            if (overlaps(dst, n, stack_maps[i].dst, stack_maps[i].size) ||
                overlaps(src, n, stack_maps[i].src, stack_maps[i].size)) {
                r = R_INVALID_REGION;
                goto out;
            }
        commit(dst, n, "moved stack");
        memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, n);
        protect(src, n, PLAT_PROT_NONE, "moved stack source");
        stack_maps[nstack_maps++] = (StackMap){dst, src, n};
    }
out:
    __atomic_store_n(&map_gen, map_gen + 1, __ATOMIC_RELEASE);
    plat_mutex_unlock(&lock);
    return r;
}

uint32_t mem_map(uint64_t dst, uint64_t src, uint64_t n) { return stack_move(0, dst, src, n); }
uint32_t mem_unmap(uint64_t dst, uint64_t src, uint64_t n) { return stack_move(1, dst, src, n); }

// ---- 0x2C/0x2D MapPhysicalMemory, UnmapPhysicalMemory. Horizon maps only the still-unmapped pages of the range and
// leaves mapped pages and their contents alone; NX2navi's heaps grow by re-mapping ranges they partly own.
static void alias_runs(uint64_t first, uint64_t count, uint8_t want, int do_commit) {   // under lock
    uint64_t i = 0;
    while (i < count) {
        if (alias_pages[first + i] != want) { i++; continue; }
        uint64_t j = i;
        while (j < count && alias_pages[first + j] == want) j++;
        uint64_t a = L.alias.base + (first + i) * PAGE, n = (j - i) * PAGE;
        if (do_commit) commit(a, n, "physical memory");
        else decommit(a, n, "physical memory");
        memset(alias_pages + first + i, !want, j - i);
        i = j;
    }
}

static uint32_t alias_map(int unmap, uint64_t a, uint64_t n) {
    if (!n) return R_INVALID_ADDRESS;
    n = (n + PAGE - 1) & ~(uint64_t)(PAGE - 1);   // the SDK asks for byte ranges; a mapping is whole pages
    if ((a | n) & (PAGE - 1)) return R_INVALID_ADDRESS;
    if (!inside(a, n, L.alias)) return R_INVALID_REGION;
    uint64_t first = (a - L.alias.base) / PAGE, count = n / PAGE, t0 = perf_mark_begin();
    plat_mutex_lock(&lock);
    uint32_t r = R_OK;
    if (unmap) {
        for (unsigned i = 0; i < nstack_maps; i++)
            if (overlaps(a, n, stack_maps[i].src, stack_maps[i].size)) {
                r = R_INVALID_REGION;
                goto out;
            }
        uint64_t released = 0;
        for (uint64_t i = 0; i < count; i++) released += alias_pages[first + i];
        alias_runs(first, count, 1, 0);
        alias_bytes -= released * PAGE;
    } else {
        uint64_t fresh = 0;
        for (uint64_t i = 0; i < count; i++) fresh += !alias_pages[first + i];
        if (fresh * PAGE + alias_bytes + heap_size + image_bytes() > L.pool) {
            r = R_LIMIT_REACHED;   // the SDK maps LimitReached to its recoverable out-of-memory result
            goto out;
        }
        alias_runs(first, count, 0, 1);
        alias_bytes += fresh * PAGE;
    }
out:
    __atomic_store_n(&map_gen, map_gen + 1, __ATOMIC_RELEASE);
    plat_mutex_unlock(&lock);
    perf_mark_end(PERF_M_GMEM, t0, n);
    if (r) rt_log("%sMapPhysicalMemory %#llx+%#llx failed %#x (alias %#llx used, heap %#llx)", unmap ? "Un" : "",
                  (unsigned long long)a, (unsigned long long)n, r, (unsigned long long)alias_bytes,
                  (unsigned long long)heap_size);
    return r;
}

uint32_t mem_map_physical(uint64_t a, uint64_t n) { return alias_map(0, a, n); }
uint32_t mem_unmap_physical(uint64_t a, uint64_t n) { return alias_map(1, a, n); }

// ---- 0x06 QueryMemory
// The region around a that no known region covers: everything else below 2^39 belongs to the host and is reported
// as taken (Code, RX), so the SDK never picks it.
static void taken_gap(uint64_t a, uint64_t *base, uint64_t *size) {
    uint64_t lo = 0, hi = RT_ADDRESS_LIMIT;
    RtRange known[3 + MOD_COUNT + 2];
    int n = 0;
    known[n++] = (RtRange){L.aslr.base, LOW_SIZE};
    known[n++] = L.alias;
    known[n++] = L.kernel;
    known[n++] = L.hostfn;
    for (int i = 0; i < ldr_count(); i++) known[n++] = (RtRange){ldr_module(i)->base, ldr_module(i)->size};
    for (int i = 0; i < n; i++) {
        uint64_t e = known[i].base + known[i].size;
        if (e <= a && e > lo) lo = e;
        if (known[i].base > a && known[i].base < hi) hi = known[i].base;
    }
    *base = lo;
    *size = hi - lo;
}

static uint64_t run_end(const uint8_t *p, uint64_t i, uint64_t n, uint8_t v) {
    while (i < n && (i & 7)) { if (p[i] != v) return i; i++; }
    uint64_t w = v ? 0x0101010101010101ull : 0;
    while (i + 8 <= n && *(const uint64_t *)(p + i) == w) i += 8;
    while (i < n && p[i] == v) i++;
    return i;
}

static uint64_t run_start(const uint8_t *p, uint64_t i, uint8_t v) {   // first index of the run that ends at i
    while (i > 0 && (i & 7)) { if (p[i - 1] != v) return i; i--; }
    uint64_t w = v ? 0x0101010101010101ull : 0;
    while (i >= 8 && *(const uint64_t *)(p + i - 8) == w) i -= 8;
    while (i > 0 && p[i - 1] == v) i--;
    return i;
}

static void query_locked(uint64_t a, RtMemoryInfo *out);

static int readable_locked(uint64_t a, uint64_t n) {
    for (uint64_t end = a + n; a < end;) {
        RtMemoryInfo r;
        query_locked(a, &r);
        if (r.state == ST_FREE || r.state == ST_INACCESSIBLE || !(r.perm & PERM_R) || r.base + r.size <= a) return 0;
        a = r.base + r.size;
    }
    return 1;
}

// Whether every byte of [a, a + n) can be read: no free, inaccessible or no-access page (mem_query's regions). For
// host reads of guest memory the guest itself may never make, such as a texture upload of a pool slot no shader
// samples (vk_exec_draw.c, texture pools: stale slots point at unmapped memory).
int mem_readable(uint64_t a, uint64_t n) {
    plat_mutex_lock_shared(&lock);
    int ok = readable_locked(a, n);
    plat_mutex_unlock_shared(&lock);
    return ok;
}

// mem_readable for a read that takes a while on another thread than the guest's: with 1 the range stays mapped until
// mem_read_release, as unmapping it (UnmapPhysicalMemory, a smaller heap, a moved stack) waits for the lock. A pool
// slot's storage the game has just released could otherwise be decommitted halfway through the copy (a texture
// upload faulted that way in tex_bl_copy). In between, the caller takes no lock of this file (mem_query, mem_readable
// and the mapping calls would wait for themselves). 0: not readable, and nothing held.
int mem_read_hold(uint64_t a, uint64_t n) {
    plat_mutex_lock_shared(&lock);
    if (readable_locked(a, n)) return 1;
    plat_mutex_unlock_shared(&lock);
    return 0;
}

void mem_read_release(void) { plat_mutex_unlock_shared(&lock); }

uint32_t mem_query(uint64_t a, RtMemoryInfo *out) {
    plat_mutex_lock_shared(&lock);
    query_locked(a, out);
    plat_mutex_unlock_shared(&lock);
    return R_OK;
}

static void query_locked(uint64_t a, RtMemoryInfo *out) {
    RtMemoryInfo r = {.base = a & ~(PAGE - 1), .size = PAGE};
    uint64_t low = L.aslr.base;
    int mi;
    if (a >= RT_ADDRESS_LIMIT) {
        r.base = RT_ADDRESS_LIMIT;
        r.size = 0 - RT_ADDRESS_LIMIT;
        r.state = ST_INACCESSIBLE;
    } else if (a < low) {
        r.base = 0;
        r.size = low;
        r.state = ST_FREE;
    } else if (a < low + LOW_SIZE) {
        uint64_t heap = L.heap.base;
        if (a >= heap && a < heap + heap_size) {
            r = (RtMemoryInfo){.base = heap, .size = heap_size, .state = ST_NORMAL, .perm = PERM_RW};
        } else if (a < heap) {
            r = (RtMemoryInfo){.base = low, .size = heap - low, .state = ST_FREE};
        } else {
            r = (RtMemoryInfo){.base = heap + heap_size, .size = low + LOW_SIZE - heap - heap_size, .state = ST_FREE};
        }
    } else if (a - L.alias.base < L.alias.size) {
        uint64_t p = (a - L.alias.base) / PAGE;
        uint8_t v = alias_pages[p];
        uint64_t s = run_start(alias_pages, p, v), e = run_end(alias_pages, p, ALIAS_PAGES, v);
        r = (RtMemoryInfo){.base = L.alias.base + s * PAGE, .size = (e - s) * PAGE, .state = v ? ST_NORMAL : ST_FREE,
                           .perm = v ? PERM_RW : PERM_NONE};
    } else if ((mi = module_index(a)) >= 0) {
        const RtLoaded *m = ldr_module(mi);
        uint64_t off = (a - m->base) & ~(PAGE - 1), lo = off, hi = off + PAGE;
        unsigned perm = module_page_perm(mi, off);
        int data = off >= m->data_off;
        uint64_t limit = data ? m->data_off : 0, end = data ? m->size : m->data_off;
        while (lo > limit && module_page_perm(mi, lo - PAGE) == perm) lo -= PAGE;
        while (hi < end && module_page_perm(mi, hi) == perm) hi += PAGE;
        r = (RtMemoryInfo){.base = m->base + lo, .size = hi - lo, .state = data ? ST_CODEDATA : ST_CODE,
                           .perm = perm & 7, .attr = perm & ATTR_PERMISSION_LOCKED};
    } else if (a - L.kernel.base < L.kernel.size) {
        r = (RtMemoryInfo){.base = L.kernel.base, .size = L.kernel.size, .state = ST_THREADLOCAL, .perm = PERM_RW};
    } else if (a - L.hostfn.base < L.hostfn.size) {
        r = (RtMemoryInfo){.base = L.hostfn.base, .size = L.hostfn.size, .state = ST_INACCESSIBLE};
    } else {
        taken_gap(a, &r.base, &r.size);
        r.state = ST_CODE;
        r.perm = PERM_RX;
    }
    // Moved stacks: the destination is Stack, the source Normal without access; free and normal regions around them
    // are clipped.
    for (unsigned i = 0; i < nstack_maps; i++) {
        const StackMap *m = &stack_maps[i];
        uint64_t end = m->dst + m->size, send = m->src + m->size;
        if (a >= m->dst && a < end) {
            r = (RtMemoryInfo){.base = m->dst, .size = m->size, .state = ST_STACK, .perm = PERM_RW};
            break;
        }
        if (a >= m->src && a < send) {
            r = (RtMemoryInfo){.base = m->src, .size = m->size, .state = ST_NORMAL, .perm = PERM_NONE, .attr = 4};
            break;
        }
        uint64_t lo = r.base, hi = r.base + r.size;
        uint64_t b = r.state == ST_FREE ? m->dst : r.state == ST_NORMAL ? m->src : 0;
        uint64_t e = b + m->size;
        if (!b) continue;
        if (e <= a && e > lo) lo = e;
        if (b > a && b < hi) hi = b;
        r.base = lo;
        r.size = hi - lo;
    }
    if (out) *out = r;
}

// ---- GetInfo memory ids
int mem_info(uint32_t id, uint64_t *out) {
    uint64_t v;
    switch (id) {
    case 2: v = L.alias.base; break;
    case 3: v = L.alias.size; break;
    case 4: v = L.heap.base; break;
    case 5: v = L.heap.size; break;
    case 6: case 21: v = L.pool; break;
    case 7: case 22:
        plat_mutex_lock_shared(&lock);
        v = image_bytes() + heap_size + alias_bytes;
        plat_mutex_unlock_shared(&lock);
        break;
    case 12: v = L.aslr.base; break;
    case 13: v = L.aslr.size; break;
    case 14: v = L.stack.base; break;
    case 15: v = L.stack.size; break;
    case 16: v = SYSTEM_RESOURCE_BUDGET; break;
    case 17: v = sizeof stack_maps + ALIAS_PAGES; break;   // system resource in use: our bookkeeping
    case 28: v = 0; break;   // AliasRegionExtraSize: no kernel-owned extension
    default: return 0;
    }
    *out = v;
    return 1;
}

// ---- kernel region: power-of-two blocks, a free list per size, bump allocation with lazy commit.
#define KCLASSES 29          // 64 B .. 256 MiB
#define KMIN_SHIFT 6
#define KCOMMIT 0x10000ull
typedef struct KFree { struct KFree *next; } KFree;
static KFree *kfree_list[KCLASSES];
static uint64_t kbump, kcommitted;
static PlatMutex klock = PLAT_MUTEX_INIT;

static int kclass(uint64_t size) {
    int s = KMIN_SHIFT;
    while ((1ull << s) < size) s++;
    return s - KMIN_SHIFT;
}

void *mem_kernel_alloc(uint64_t size, uint64_t align) {
    if (!L.pool) rt_fatal("mem_kernel_alloc before mem_init");
    if (align > KCOMMIT || (align & (align - 1))) rt_fatal("mem_kernel_alloc: alignment %#llx", (unsigned long long)align);
    uint64_t want = size > align ? size : align;
    int k = kclass(want ? want : 1);
    if (k >= KCLASSES) rt_fatal("mem_kernel_alloc: %#llx bytes", (unsigned long long)size);
    uint64_t bytes = 1ull << (k + KMIN_SHIFT), al = bytes < KCOMMIT ? bytes : KCOMMIT;
    plat_mutex_lock(&klock);
    KFree *f = kfree_list[k];
    void *p;
    if (f) {
        kfree_list[k] = f->next;
        p = f;
    } else {
        uint64_t a = align_up(kbump ? kbump : RT_KERNEL_BASE, al);
        if (a + bytes > RT_KERNEL_BASE + RT_KERNEL_SIZE) rt_fatal("kernel region exhausted (%#llx bytes)",
                                                                  (unsigned long long)bytes);
        uint64_t need = align_up(a + bytes, KCOMMIT);
        if (!kcommitted) kcommitted = RT_KERNEL_BASE;
        if (need > kcommitted) {
            commit(kcommitted, need - kcommitted, "kernel region");
            kcommitted = need;
        }
        kbump = a + bytes;
        p = (void *)(uintptr_t)a;
    }
    plat_mutex_unlock(&klock);
    memset(p, 0, size ? size : 1);
    return p;
}

void mem_kernel_free(void *p, uint64_t size) {
    if (!p) return;
    uint64_t a = (uintptr_t)p;
    if (a - RT_KERNEL_BASE >= RT_KERNEL_SIZE) rt_fatal("mem_kernel_free(%p): not in the kernel region", p);
    int k = kclass(size ? size : 1);
    // A block allocated with a larger alignment sits in a larger class; freeing it into the smaller class only
    // wastes the rest.
    plat_mutex_lock(&klock);
    KFree *f = p;
    f->next = kfree_list[k];
    kfree_list[k] = f;
    plat_mutex_unlock(&klock);
}

// NX2NAVI_TRACE=perf: what the lazy heap's first-use faults cost since the last line (perf.c, every perf window that had
// some), so that a hitch can be told from them: the time includes the wait for frz_lock.
void mem_lazy_heap_report(uint64_t frame) {
    static uint64_t faults_prv, bytes_prv;
    if (!heap_lazy) return;
    plat_mutex_lock(&frz_lock);
    uint64_t faults = heap_lazy_faults, bytes = heap_lazy_bytes, ns = heap_lazy_ns, max = heap_lazy_max_ns;
    heap_lazy_ns = heap_lazy_max_ns = 0;
    plat_mutex_unlock(&frz_lock);
    if (faults == faults_prv && bytes == bytes_prv) return;
    rt_log("mem: lazy heap at frame %llu: %llu faults and %.1f MiB committed since the last line, %.2f ms in the faults "
           "(longest %.3f ms); %.0f of %.0f MiB committed", (unsigned long long)frame,
           (unsigned long long)(faults - faults_prv), (bytes - bytes_prv) / 1048576.0, ns / 1e6, max / 1e6,
           bytes / 1048576.0, __atomic_load_n(&heap_size, __ATOMIC_ACQUIRE) / 1048576.0);
    faults_prv = faults, bytes_prv = bytes;
}

// NX2NAVI_TRACE=perf: how many bytes of each guest region are committed, so a process that has grown says which region
// holds the memory. A region is reserved whole at start-up and committed page by page, so the walk reads each run of
// pages of the same state; the alias window is the largest and costs the most steps when it is full.
void mem_guest_census(uint64_t frame) {
    const RtGuestLayout *g = mem_layout();
    const struct { const char *name; RtRange r; } reg[] = {
        {"aslr", g->aslr}, {"stack", g->stack}, {"heap", g->heap},
        {"alias", g->alias}, {"kernel", g->kernel}, {"hostfn", g->hostfn},
    };
    char line[400];
    int o = snprintf(line, sizeof line, "mem: guest regions at frame %llu (MiB committed):",
                     (unsigned long long)frame);
    uint64_t all = 0;
    for (unsigned i = 0; i < sizeof reg / sizeof *reg && o < (int)sizeof line - 40; i++) {
        uint64_t x = reg[i].r.base, end = reg[i].r.base + reg[i].r.size, committed = 0, b = 0;
        while (x < end) {
            int st = plat_mem_state(x, end, &b);
            if (b <= x) break;
            if (st >= 0) committed += b - x;   // a PLAT_PROT_* state: these pages are committed
            x = b;
        }
        all += committed;
        o += snprintf(line + o, sizeof line - o, " %s %.0f", reg[i].name, committed / 1048576.0);
    }
    rt_log("%s; all %.0f", line, all / 1048576.0);
    if (heap_lazy)
        rt_log("mem: lazy heap at frame %llu: %.0f of %.0f MiB committed, by %llu faults and the rest by kernel "
               "accesses and pointer checks", (unsigned long long)frame, heap_lazy_bytes / 1048576.0,
               __atomic_load_n(&heap_size, __ATOMIC_ACQUIRE) / 1048576.0, (unsigned long long)heap_lazy_faults);
}

void mem_guest_usage(uint64_t *heap, uint64_t *heap_committed, uint64_t *phys_maps) {
    *heap = __atomic_load_n(&heap_size, __ATOMIC_RELAXED);
    *heap_committed = heap_lazy ? __atomic_load_n(&heap_lazy_bytes, __ATOMIC_RELAXED) : *heap;
    *phys_maps = __atomic_load_n(&alias_bytes, __ATOMIC_RELAXED);
}

// NX2NAVI_TRACE=perf "mem: sizes" (perf.c): the guest heap and physical maps, the kernel region's bump and commit, the
// moved stacks, and the lazily written ranges with their array's room.
void mem_sizes(PerfSizes *s) {
    plat_mutex_lock_shared(&lock);
    uint64_t heap = heap_size, alias = alias_bytes, stacks = nstack_maps;
    plat_mutex_unlock_shared(&lock);
    plat_mutex_lock_shared(&klock);
    uint64_t kused = kbump ? kbump - RT_KERNEL_BASE : 0, kcom = kcommitted ? kcommitted - RT_KERNEL_BASE : 0;
    plat_mutex_unlock_shared(&klock);
    plat_mutex_lock_shared(&frz_lock);
    uint64_t lz = (uint64_t)lz_n, lzc = (uint64_t)lz_cap;
    plat_mutex_unlock_shared(&frz_lock);
    perf_size(s, "guest heap", PERF_SIZE_NONE, 0, heap);
    perf_size(s, "guest physical maps", PERF_SIZE_NONE, 0, alias);
    perf_size(s, "kernel region used", PERF_SIZE_NONE, 0, kused);
    perf_size(s, "kernel region committed", PERF_SIZE_NONE, 0, kcom);
    perf_size(s, "moved stacks", stacks, sizeof stack_maps / sizeof *stack_maps, PERF_SIZE_NONE);
    perf_size(s, "lazy write-back ranges", lz, 0, PERF_SIZE_NONE);
    perf_size(s, "their array's room", lzc, 0, PERF_SIZE_NONE);
}
