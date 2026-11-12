// VRAM headroom: the next VMA block of a memory type is allocated on a background thread, not on the render thread.
//
// VMA places an allocation in an existing block of its memory type and allocates a new block (vkAllocateMemory, an
// eighth of the default block size up to all of it, vk_vma_block_size) only when none has room. On the render thread
// that took 8-19 ms per 256 MiB block, several times in the first minute of play while VRAM grows (textures from
// create_tex, mirrors). A new VkDeviceMemory made by any other thread
// (VMA's pfnAllocate callback) wakes this thread for its memory type, which it then checks every 500 ms. A check
// allocates half a default block of that type (the largest size VMA still places in blocks, not in dedicated memory).
// When a block has that much room the allocation lands there and is freed at once. When none has, VMA allocates a new
// block here, and the allocation stays as that block's anchor: VMA deletes an empty block at the next free of its
// type (VmaBlockVector::Free, "we have another empty block"), so a spare block left empty lasted until the next
// texture went. The previous anchor is freed then, which gives its half block to the render thread, so at most half
// a block per memory type is held back.
// Only the memory types textures and VRAM mirrors grow in get a spare (spare_type): the device-local types the CPU
// cannot map, or on a GPU that has none of those (integrated, every type mappable), the device-local ones. In one
// session the others, the host heap's two types (system memory, charged to the commit) and the device-local mappable
// one (VRAM behind the PCIe BAR), each got a 256 MiB spare early on and never needed another; to the end the BAR type
// held 320 MiB of blocks for 37 MiB of allocations and one host type 288 MiB for 0.1 MiB, as VMA keeps an empty block
// until the next free of its type, and these types seldom free anything.
// Under VRAM pressure no headroom is kept: the spare block counts against the budget that decides whether a pool gets
// its VRAM mirror (vk_memory.c), so while use plus two blocks passes 85% of the budget the anchor goes and nothing new
// is allocated. Nor while the system's commit is low (vk_texture_commit_low) where device memory counts against it:
// on Windows the driver charges device memory to the commit, and a spare block of each type held up to 256 MiB of it
// empty when an allocation that failed there ended the game; an integrated GPU's memory is the machine's RAM. Linux's
// limits (plat_commit_counts_vram) do not count a discrete card's own memory, so dropping its spare would free nothing
// they count and only put the block allocations back on the render thread.
// NX2NAVI_VRAM_HEADROOM=0 turns it off.

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "platform/platform.h"

#define HR_PERIOD_MS 500
#define HR_SMALL_HEAP (1ull << 30)   // VMA_SMALL_HEAP_MAX_SIZE: smaller heaps use blocks of a heap eighth

static struct {
    int on, stop;
    PlatMutex lock;             // held around each top-up; vk_headroom_stop takes it before the allocator goes
    int started;
    PlatEvent ev;               // auto-reset: another thread allocated, or stop
    volatile uint32_t tid;      // the headroom thread's id
    volatile int64_t types;    // memory types a thread other than this one allocated device memory of
    volatile int64_t made;     // blocks this thread brought in
    volatile int64_t other;    // device memory of 32 MiB or more other threads allocated (VMA blocks, large dedicated)
    VmaAllocation anchor[64]; // per memory type: the half block that keeps the newest spare block from being deleted
    volatile int64_t held;     // bytes of the anchors, for the log's VRAM breakdown (vk_headroom_held)
    volatile uint32_t hold_until;   // plat_now_ms(): no spare block before then (vk_headroom_release)
} H;

// NX2NAVI_VMA_BLOCK_MB (16 to 256, default 64): the size of VMA's blocks on heaps over 1 GiB (its
// preferredLargeHeapBlockSize; VMA's own default is 256 MiB). A freed texture or mirror leaves a hole its block keeps,
// and the driver counts the whole block against the VRAM budget and the system's commit until its last allocation
// goes. With 256 MiB blocks one session held 2.1 GiB free inside them beside 8.2 GiB of allocations after a few area
// changes, every block pinned by a texture or mirror that outlived its neighbours; a smaller block empties, and goes
// back to the driver, once fewer allocations have gone. Allocations over half a block get memory of their own (VMA),
// which matches the 32 MiB from which create_tex gives images their own memory.
VkDeviceSize vk_vma_block_size(void) {
    static VkDeviceSize size;
    if (!size) {
        int mb = cfg_int("NX2NAVI_VMA_BLOCK_MB");
        mb = mb < 16 ? 16 : mb > 256 ? 256 : mb;
        size = (VkDeviceSize)mb << 20;
        rt_log("vk: VMA blocks of %d MiB on heaps over 1 GiB (NX2NAVI_VMA_BLOCK_MB)", mb);
    }
    return size;
}

// The memory types a spare block is kept for (see the top): device-local without host access, or device-local at all
// where the device has no such type.
static int spare_type(const VkCtx *c, uint32_t type) {
    const VkPhysicalDeviceMemoryProperties *m = &c->mem_props;
    VkMemoryPropertyFlags f = m->memoryTypes[type].propertyFlags;
    if (!(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return 0;
    if (!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) return 1;
    for (uint32_t t = 0; t < m->memoryTypeCount; t++)
        if ((m->memoryTypes[t].propertyFlags & (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) ==
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            return 0;
    return 1;
}

uint64_t vk_headroom_held(void) { return (uint64_t)__atomic_load_n(&H.held, __ATOMIC_RELAXED); }

static void VKAPI_PTR on_allocate(VmaAllocator a, uint32_t type, VkDeviceMemory m, VkDeviceSize size, void *user) {
    (void)a, (void)m, (void)size, (void)user;
    perf_mark_add(PERF_M_DEVMEM, 0, size);   // NX2NAVI_TRACE=perf: every block and dedicated allocation VMA makes
    if (!H.on || type >= 64) return;
    if (plat_thread_id() == H.tid) {
        plat_inc64(&H.made);
        return;
    }
    if (size >= 32ull << 20) {
        plat_inc64(&H.other);
        vk_texstat_add(VK_TS_DEVMEM, 1, size);   // counted when the render or replay thread made it
    }
    plat_or64(&H.types, (int64_t)(1ull << type));
    plat_event_set(&H.ev);
}

const VmaDeviceMemoryCallbacks *vk_headroom_callbacks(void) {
    static const VmaDeviceMemoryCallbacks cb = {.pfnAllocate = on_allocate};
    const char *v = cfg_get("NX2NAVI_VRAM_HEADROOM");   // empty is unset: the key has no settings row for cfg_bool
    H.on = !v || !*v || cfg_bool("NX2NAVI_VRAM_HEADROOM");
    return H.on ? &cb : NULL;
}

static VkDeviceSize block_size(const VkCtx *c, uint32_t type) {
    VkDeviceSize heap = c->mem_props.memoryHeaps[c->mem_props.memoryTypes[type].heapIndex].size;
    return heap <= HR_SMALL_HEAP ? heap / 8 : vk_vma_block_size();   // VMA's preferredLargeHeapBlockSize (vk_device.c)
}

static int room(const VkCtx *c, uint32_t type, VkDeviceSize block) {
    if (vk_texture_commit_low() &&
        (plat_commit_counts_vram() || c->props.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU))
        return 0;
    VmaBudget b[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(c->vma, b);
    const VmaBudget *h = &b[c->mem_props.memoryTypes[type].heapIndex];
    return h->budget && h->usage + 2 * block <= h->budget / 100 * 85;
}

static void drop_anchor(const VkCtx *c, uint32_t type) {
    if (H.anchor[type]) {
        VmaAllocationInfo ai;
        vmaGetAllocationInfo(c->vma, H.anchor[type], &ai);
        plat_add64(&H.held, -(int64_t)ai.size);
        vmaFreeMemory(c->vma, H.anchor[type]);
    }
    H.anchor[type] = NULL;
}

static void top_up(const VkCtx *c, uint32_t type) {
    VkDeviceSize block = block_size(c, type);
    if (!room(c, type, block) || (int32_t)(__atomic_load_n(&H.hold_until, __ATOMIC_RELAXED) - (uint32_t)plat_now_ms()) > 0) {
        drop_anchor(c, type);
        return;
    }
    VkMemoryRequirements req = {.size = block / 2, .alignment = 256, .memoryTypeBits = 1u << type};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_UNKNOWN};
    VmaAllocation a;
    int64_t before = H.made;
    if (vmaAllocateMemory(c->vma, &req, &ai, &a, NULL) != VK_SUCCESS) return;
    if (H.made == before) {   // there was room
        vmaFreeMemory(c->vma, a);
        return;
    }
    drop_anchor(c, type);
    H.anchor[type] = a;
    VmaAllocationInfo info;
    vmaGetAllocationInfo(c->vma, a, &info);
    plat_add64(&H.held, (int64_t)info.size);
    static volatile int32_t logged;
    if (plat_inc32(&logged) <= 32)
        rt_log("vk: headroom: a %.0f MiB block of memory type %u allocated ahead (%lld so far; %lld allocations of 32 MiB "
               "or more on other threads)", (double)block / 1048576.0, type, (long long)H.made, (long long)H.other);
}

static void headroom_main(void *arg) {
    (void)arg;
    __atomic_store_n(&H.tid, plat_thread_id(), __ATOMIC_RELEASE);
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    uint64_t watch = 0;   // memory types seen so far
    for (;;) {
        if (watch) plat_event_wait_ns(&H.ev, (uint64_t)HR_PERIOD_MS * 1000000u);
        else plat_event_wait(&H.ev);
        watch |= (uint64_t)plat_xchg64(&H.types, 0);
        plat_mutex_lock(&H.lock);
        const VkCtx *c = H.stop ? NULL : vk_ctx();
        for (uint32_t t = 0; c && t < c->mem_props.memoryTypeCount && t < 64; t++)
            if ((watch >> t & 1) && spare_type(c, t)) top_up(c, t);
        int stop = H.stop;
        if (stop && (c = vk_ctx()))
            for (uint32_t t = 0; t < 64; t++) drop_anchor(c, t);
        plat_mutex_unlock(&H.lock);
        if (stop) return;
    }
}

// An allocation the driver refused (vk_memory.c give_back): the anchors go, so VMA can use or delete their blocks, and
// no spare block is made for HR_RELEASE_HOLD_MS. Returns the bytes the anchors held.
#define HR_RELEASE_HOLD_MS 30000
uint64_t vk_headroom_release(void) {
    if (!H.started) return 0;
    __atomic_store_n(&H.hold_until, (uint32_t)plat_now_ms() + HR_RELEASE_HOLD_MS, __ATOMIC_RELAXED);
    uint64_t was = (uint64_t)__atomic_load_n(&H.held, __ATOMIC_RELAXED);
    plat_mutex_lock(&H.lock);
    const VkCtx *c = vk_ctx();
    for (uint32_t t = 0; c && t < 64; t++) drop_anchor(c, t);
    plat_mutex_unlock(&H.lock);
    return was;
}

// Before the allocator is destroyed: waits for a running top-up, frees the anchors, and the thread allocates no more.
void vk_headroom_stop(void) {
    if (!H.started) return;
    plat_mutex_lock(&H.lock);
    H.stop = 1;
    const VkCtx *c = vk_ctx();
    for (uint32_t t = 0; c && t < 64; t++) drop_anchor(c, t);
    plat_mutex_unlock(&H.lock);
    plat_event_set(&H.ev);
}

void vk_headroom_start(void) {
    if (!H.on || H.started) return;
    plat_mutex_init(&H.lock);
    plat_event_init(&H.ev, 0, 0);
    if (!plat_thread_spawn(headroom_main, NULL, "vk headroom", 0)) {
        H.on = 0;
        rt_log("vk: headroom: no thread, new VMA blocks stay on the allocating thread");
        return;
    }
    H.started = 1;
    rt_log("vk: headroom: the next VMA block of each memory type in use is allocated on a background thread "
           "(NX2NAVI_VRAM_HEADROOM=0 turns it off)");
}
