// runtime/vk_gpa.c: the device address table against a model of what a shader reads through it, with the allocator,
// the device and the completion queue replaced by the stand-ins below. Built and run by tests/gpa_check.py; prints each
// failed check and exits 1 on any.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_gpa.h"
#include "platform/platform.h"

static int fails;
#define CHECK(c, ...) do { if (!(c) && fails++ < 40) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- stand-ins for what vk_gpa.c links against
static VkCtx ctx = {.has_buffer_device_address = 1};
VkCtx *vk_ctx(void) { return &ctx; }
const char *cfg_get(const char *name) { (void)name; return NULL; }
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }

static int out_of_blocks_lines;
void perf_size(PerfSizes *s, const char *name, uint64_t count, uint64_t limit, uint64_t bytes) {
    (void)s, (void)name, (void)count, (void)limit, (void)bytes;   // vk_gpa_sizes is not called here
}

void rt_log(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (strstr(line, "out of L2 blocks")) out_of_blocks_lines++;
}

static const uint32_t *table;   // the first buffer vk_gpa.c creates
static uintptr_t next_handle = 0x1000;
// vk.h sends vmaCreateBuffer and vmaDestroyBuffer to these wrappers (vk_vma_budget.c)
VkResult vk_vma_create_buffer(VmaAllocator a, const VkBufferCreateInfo *bi, const VmaAllocationCreateInfo *ai, VkBuffer *buf,
                              VmaAllocation *alloc, VmaAllocationInfo *info, const char *file) {
    (void)a, (void)ai, (void)file;
    void *mem = calloc(1, (size_t)bi->size);
    if (!table) table = mem;
    *buf = (VkBuffer)(next_handle += 0x1000);
    *alloc = (VmaAllocation)mem;
    if (info) info->pMappedData = mem;
    return VK_SUCCESS;
}
void vmaDestroyBuffer(VmaAllocator a, VkBuffer buf, VmaAllocation alloc) { (void)a, (void)buf; free(alloc); }
VkResult vmaInvalidateAllocation(VmaAllocator a, VmaAllocation alloc, VkDeviceSize off, VkDeviceSize n) {
    (void)a, (void)alloc, (void)off, (void)n;
    return VK_SUCCESS;
}

// A buffer's device address: its handle far above the GPU addresses the tests map.
static uint64_t dev_of(VkBuffer b) { return (uint64_t)(uintptr_t)b << 28; }
VkDeviceAddress vkGetBufferDeviceAddress(VkDevice d, const VkBufferDeviceAddressInfo *i) { (void)d; return dev_of(i->buffer); }

// The completion queue: a callback runs once the GPU is past its submission (gpu_done).
static struct { uint64_t fence; void (*fn)(void *); void *arg; } queue[1 << 16];
static int nqueue;
static uint64_t issued;
uint64_t vk_sync_issued(void) { return issued; }
void vk_completion_add(uint64_t fence, void (*fn)(void *), void *arg) {
    if (nqueue == (int)(sizeof queue / sizeof *queue)) { printf("FAIL: completion queue full\n"); exit(1); }
    queue[nqueue].fence = fence, queue[nqueue].fn = fn, queue[nqueue].arg = arg;
    nqueue++;
}
static void gpu_done(void) {   // the submission being recorded is issued and finishes
    issued++;
    int k = 0;
    for (int i = 0; i < nqueue; i++)
        if (queue[i].fence <= issued) queue[i].fn(queue[i].arg);
        else queue[k++] = queue[i];
    nqueue = k;
}

// A store the GPU runs in queue order (vk_gpa_flip_cmd): kept until gpu_runs_stores, as a command buffer keeps it
// until the GPU reaches it.
static struct { VkDeviceSize off, n; uint8_t data[32]; } stores[8];
static int nstores;
void vkCmdUpdateBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkDeviceSize n, const void *data) {
    (void)cb, (void)b;
    if (nstores == 8 || n > sizeof stores[0].data || (off | n) & 3) { printf("FAIL: a store the stand-in cannot hold\n"); exit(1); }
    stores[nstores].off = off, stores[nstores].n = n;
    memcpy(stores[nstores].data, data, (size_t)n);
    nstores++;
}
static void gpu_runs_stores(void) {
    for (int i = 0; i < nstores; i++) memcpy((uint8_t *)(uintptr_t)table + stores[i].off, stores[i].data, (size_t)stores[i].n);
    nstores = 0;
}

// ---- the table as a shader reads it (the layout at the top of vk_gpa.c)
enum { L1_WORD = 16, L1_N = 65536, L2_WORD = L1_WORD + L1_N, L2_N = 4096, BLOCKS = 1024 };
static uint32_t block_of(uint64_t addr) { return table[L1_WORD + (addr >> 24)]; }
static uint64_t read_through(uint64_t addr, int twin) {   // the device address addr resolves to, 0 for the zero buffer
    uint32_t blk = block_of(addr);
    uint32_t rec = blk ? table[L2_WORD + blk * L2_N + ((addr >> 12) & (L2_N - 1))] : 0;
    if (!rec) return 0;
    const uint32_t *r = table + table[2] + rec * 4 + (twin ? 2 : 0);
    return ((uint64_t)r[0] | (uint64_t)r[1] << 32) + addr;
}

static VkBuffer new_buf(void) { return (VkBuffer)(next_handle += 0x1000); }
#define MiB (1ull << 20)

// Every page of [lo, hi) reads buf (whose byte 0 is at buf_lo), or the zero buffer when buf is none.
static void expect(uint64_t lo, uint64_t hi, VkBuffer buf, uint64_t buf_lo, const char *what) {
    for (uint64_t a = lo; a < hi; a += 4096) {
        uint64_t got = read_through(a, 0), want = buf ? dev_of(buf) + (a - buf_lo) : 0;
        if (got != want) {
            CHECK(0, "%s: %#llx reads %#llx, expected %#llx", what, (unsigned long long)a, (unsigned long long)got,
                  (unsigned long long)want);
            return;
        }
    }
}

static uint32_t cover_rec;
static uint64_t cover_lo, cover_hi;
static uint32_t cover(uint64_t a) { return a >= cover_lo && a + 4096 <= cover_hi ? cover_rec : 0; }

int main(void) {
    CHECK(vk_gpa_on() && table && table[2], "the table exists");

    // a buffer over three 16 MiB ranges, not aligned to them
    uint64_t a0 = 0x600000000ull + 5 * MiB, a1 = a0 + 40 * MiB;
    VkBuffer b0 = new_buf();
    uint32_t r0 = vk_gpa_map(a0, a1, b0, a0);
    CHECK(r0, "a first buffer is mapped");
    expect(a0, a1, b0, a0, "first buffer");
    expect(a0 - 16 * MiB, a0, VK_NULL_HANDLE, 0, "below the first buffer");
    expect(a1, a1 + 16 * MiB, VK_NULL_HANDLE, 0, "above the first buffer");

    // a mirror takes the pages with the imported buffer as twin, and gives them back
    VkBuffer m0 = new_buf();
    r0 = vk_gpa_remap(a0, a1, r0, m0, a0);
    expect(a0, a1, m0, a0, "mirrored");
    CHECK(read_through(a0 + 4096, 1) == dev_of(b0) + 4096, "the twin of a mirrored page is the imported buffer");
    r0 = vk_gpa_remap_plain(a0, a1, r0, b0, a0);
    expect(a0, a1, b0, a0, "mirror evicted");
    CHECK(read_through(a0 + 4096, 1) == dev_of(b0) + 4096, "a plain page is its own twin");
    gpu_done();

    // a new mirror takes the pool's record over by a store the GPU runs after the mirror's first copy (vk_memory.c
    // mirror_create, vk_mem_mirror_prologue): what is queued before the store reads and writes the imported buffer
    // alone, what comes after it reads the mirror and writes both
    VkBuffer m1 = new_buf();
    CHECK(vk_gpa_flip_cmd(VK_NULL_HANDLE, r0, m1, a0, 1), "a new mirror's store is recorded");
    expect(a0, a1, b0, a0, "before the GPU reaches the store: reads stay on the imported buffer");
    CHECK(read_through(a0 + 4096, 1) == dev_of(b0) + 4096 && read_through(a1 - 4096, 1) == dev_of(b0) + (a1 - 4096 - a0),
          "before the GPU reaches the store nothing is written into the mirror");
    gpu_runs_stores();
    expect(a0, a1, m1, a0, "after the store: reads are on the mirror");
    CHECK(read_through(a0 + 4096, 1) == dev_of(b0) + 4096 && read_through(a1 - 4096, 1) == dev_of(b0) + (a1 - 4096 - a0),
          "after the store the imported buffer is the twin");
    gpu_done();
    // evicted: the pages go back to the imported buffer alone, on a record of their own
    uint32_t was = r0;
    r0 = vk_gpa_remap_plain(a0, a1, r0, b0, a0);
    CHECK(r0 && r0 != was, "an evicted mirror's pages take a new record");
    expect(a0, a1, b0, a0, "an evicted mirror");
    CHECK(read_through(a0 + 4096, 1) == dev_of(b0) + 4096, "leaves the imported buffer as its own twin");
    gpu_done();
    // a mirror without a twin: the store names it twice
    VkBuffer m2 = new_buf();
    CHECK(vk_gpa_flip_cmd(VK_NULL_HANDLE, r0, m2, a0, 0), "an untwinned mirror's store is recorded");
    gpu_runs_stores();
    expect(a0, a1, m2, a0, "an untwinned mirror");
    CHECK(read_through(a0 + 4096, 1) == dev_of(m2) + 4096, "is its own twin");
    r0 = vk_gpa_remap_plain(a0, a1, r0, b0, a0);
    expect(a0, a1, b0, a0, "and goes back to the imported buffer");
    CHECK(!vk_gpa_flip_cmd(VK_NULL_HANDLE, 0, m2, a0, 1) && !nstores, "a pool without a record gets no store");
    gpu_done();

    // an unmapped range reads the zero buffer at once, and its blocks serve no other range while the GPU may still read
    // the old L1 entries
    uint32_t old_blk[3] = {block_of(a0), block_of(a0 + 16 * MiB), block_of(a0 + 32 * MiB)};
    CHECK(old_blk[0] && old_blk[1] && old_blk[2], "three blocks for three ranges");
    vk_gpa_unmap(a0, a1, r0, NULL);
    expect(a0, a1, VK_NULL_HANDLE, 0, "unmapped");
    CHECK(!block_of(a0) && !block_of(a0 + 16 * MiB) && !block_of(a0 + 32 * MiB), "empty blocks leave L1");
    uint64_t c0 = 0x700000000ull;
    VkBuffer b1 = new_buf();
    uint32_t r1 = vk_gpa_map(c0, c0 + 48 * MiB, b1, c0);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            CHECK(block_of(c0 + (uint64_t)i * 16 * MiB) != old_blk[j], "a block still waiting for the GPU serves another range");
    for (int j = 0; j < 3; j++)
        for (uint32_t k = 0; k < L2_N; k++)
            if (table[L2_WORD + old_blk[j] * L2_N + k]) { CHECK(0, "a waiting block holds an entry"); break; }
    // the same range mapped again before the GPU is done takes its block back
    VkBuffer b2 = new_buf();
    uint32_t r2 = vk_gpa_map(a0, a1, b2, a0);
    CHECK(block_of(a0) == old_blk[0] && block_of(a0 + 16 * MiB) == old_blk[1] && block_of(a0 + 32 * MiB) == old_blk[2],
          "a range mapped again takes its waiting blocks back");
    expect(a0, a1, b2, a0, "mapped again");
    gpu_done();   // the callbacks of the first release find the blocks in use
    expect(a0, a1, b2, a0, "mapped again, after the GPU");
    VkBuffer b3 = new_buf();
    uint32_t r3 = vk_gpa_map(0x780000000ull, 0x780000000ull + 16 * MiB, b3, 0x780000000ull);
    for (int j = 0; j < 3; j++) CHECK(block_of(0x780000000ull) != old_blk[j], "a block taken back was freed by its old callback");
    vk_gpa_unmap(0x780000000ull, 0x780000000ull + 16 * MiB, r3, NULL);
    vk_gpa_unmap(a0, a1, r2, NULL);
    vk_gpa_unmap(c0, c0 + 48 * MiB, r1, NULL);
    gpu_done();

    // released, taken back and released within one submission, many times: one block, whatever the callbacks pending
    for (int i = 0; i < 3000; i++) {
        VkBuffer b = new_buf();
        uint32_t r = vk_gpa_map(a0, a0 + 8 * MiB, b, a0);
        if (i % 500 == 0) expect(a0, a0 + 8 * MiB, b, a0, "freed and made again at its address");
        vk_gpa_unmap(a0, a0 + 8 * MiB, r, NULL);
        if (i % 1000 == 999) gpu_done();
    }
    gpu_done();
    CHECK(!out_of_blocks_lines, "a pool freed and made again at its address ran out of blocks");

    // addresses that climb through 64 GiB, as the game's allocator hands them out: 20 MiB pools 48 MiB apart, eight
    // alive at a time, far more ranges than there are blocks
    enum { ALIVE = 8, POOLS = 1300 };
    struct { uint64_t lo, hi; VkBuffer b; uint32_t rec; } live[ALIVE] = {0};
    for (int i = 0; i < POOLS; i++) {
        int s = i % ALIVE;
        if (live[s].rec) vk_gpa_unmap(live[s].lo, live[s].hi, live[s].rec, NULL);
        live[s].lo = 0x800000000ull + (uint64_t)i * 48 * MiB + 3 * MiB, live[s].hi = live[s].lo + 20 * MiB;
        live[s].b = new_buf();
        live[s].rec = vk_gpa_map(live[s].lo, live[s].hi, live[s].b, live[s].lo);
        CHECK(live[s].rec, "climbing pool %d has a record", i);
        if (i % 3 == 0) gpu_done();
        if (i % 97 == 0 || i == POOLS - 1)
            for (int k = 0; k < ALIVE; k++)
                if (live[k].rec) expect(live[k].lo, live[k].hi, live[k].b, live[k].lo, "climbing pool");
    }
    CHECK(!out_of_blocks_lines, "climbing pool addresses ran out of blocks (%d lines)", out_of_blocks_lines);
    for (int k = 0; k < ALIVE; k++) vk_gpa_unmap(live[k].lo, live[k].hi, live[k].rec, NULL);
    gpu_done();

    // overlapping imports: the pages of the one that goes return to the other, and their block stays
    uint64_t o0 = 0x500000000ull, o1 = o0 + 16 * MiB;
    VkBuffer ba = new_buf(), bb = new_buf();
    uint32_t ra = vk_gpa_map(o0, o1, ba, o0), rb = vk_gpa_map(o0 + 4 * MiB, o0 + 8 * MiB, bb, o0 + 4 * MiB);
    expect(o0 + 4 * MiB, o0 + 8 * MiB, bb, o0 + 4 * MiB, "the later import holds the shared pages");
    cover_rec = ra, cover_lo = o0, cover_hi = o1;
    vk_gpa_unmap(o0 + 4 * MiB, o0 + 8 * MiB, rb, cover);
    expect(o0, o1, ba, o0, "the shared pages return to the other import");
    cover_rec = 0;
    vk_gpa_clear(o0 + 2 * MiB, o0 + 3 * MiB);
    expect(o0 + 2 * MiB, o0 + 3 * MiB, VK_NULL_HANDLE, 0, "cleared pages");
    expect(o0, o0 + 2 * MiB, ba, o0, "pages below the cleared ones");
    CHECK(block_of(o0), "a block that still maps pages stays");
    vk_gpa_clear(o0, o1);
    CHECK(!block_of(o0), "a block cleared whole leaves L1");
    vk_gpa_unmap(o0, o1, ra, NULL);
    gpu_done();

    // every block in use at once: the next range is left out and said so once, the ranges after it and the mapped
    // ones are untouched, and a block given back serves the range that was left out
    static struct { VkBuffer b; uint32_t rec; } all[BLOCKS];
    uint64_t f0 = 0x1000000000ull;
    for (uint32_t i = 0; i < BLOCKS - 1; i++) {
        all[i].b = new_buf();
        all[i].rec = vk_gpa_map(f0 + (uint64_t)i * 32 * MiB, f0 + (uint64_t)i * 32 * MiB + MiB, all[i].b, f0 + (uint64_t)i * 32 * MiB);
    }
    CHECK(!out_of_blocks_lines, "the table holds %u ranges at once", BLOCKS - 1);
    uint64_t x0 = 0x2000000000ull;
    VkBuffer bx = new_buf();
    uint32_t rx = vk_gpa_map(x0, x0 + MiB, bx, x0);
    CHECK(out_of_blocks_lines == 1, "a range past the last block is reported (%d lines)", out_of_blocks_lines);
    expect(x0, x0 + MiB, VK_NULL_HANDLE, 0, "a range past the last block");
    for (uint32_t i = 0; i < BLOCKS - 1; i += 37)
        expect(f0 + (uint64_t)i * 32 * MiB, f0 + (uint64_t)i * 32 * MiB + MiB, all[i].b, f0 + (uint64_t)i * 32 * MiB, "a full table");
    vk_gpa_unmap(x0, x0 + MiB, rx, NULL);
    vk_gpa_unmap(f0, f0 + MiB, all[0].rec, NULL);
    rx = vk_gpa_map(x0, x0 + MiB, bx, x0);
    expect(x0, x0 + MiB, VK_NULL_HANDLE, 0, "a block the GPU may still read");
    vk_gpa_unmap(x0, x0 + MiB, rx, NULL);
    gpu_done();
    rx = vk_gpa_map(x0, x0 + MiB, bx, x0);
    expect(x0, x0 + MiB, bx, x0, "a block given back");

    vk_gpa_report();
    printf("gpa_check: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
