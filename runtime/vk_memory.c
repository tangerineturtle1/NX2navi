// Vulkan memory of the graphics layer: NVN memory pools as Vulkan
// buffers, the virtual-pool page table, GPU address resolution, page write generations for re-uploads, and the
// upload batch (command buffer, staging buffers, deferred destruction) that vk_texture.c records into.
//
// Contains code Copyright (c) 2026 jhaizhou, Copyright (c) 2026 Peiikai (MIT; see THIRD_PARTY_NOTICES.md).
// Virtual pools have 64 KiB pages. A texture is stale when a covered page carries a generation newer than its upload;
// every write-watch query resets the OS bits, so each query stamps the pages it saw with a new generation shared by all
// users. Virtual pools are a GPU-side page table
// over device-local memory and dirty pages come from GetWriteWatch (mem_written_pages), so neither host VM aliasing
// nor page protection (a fault handler) is needed.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "flight.h"
#include "gfx.h"
#include "host.h"
#include "logq.h"
#include "mem_budget.h"
#include "mem_growth.h"
#include "nvn_api.h"
#include "nvn_pool_index.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_exec.h"
#include "vk_gpa.h"
#include "wb_lazy_choice.h"
#include "wb_merge_spv.h"
#include "platform/platform.h"

// Timestamps for the tracking cost, in ns; qpc_ms converts.
static uint64_t qpc(void) { return plat_now_ns(); }

static double qpc_ms(uint64_t ticks) { return (double)ticks / 1e6; }

// ---------------------------------------------------------------- pools
enum { POOL_IMPORT = 1, POOL_PHYS, POOL_VIRT };
enum { MS_NONE, MS_ON, MS_REFUSED };

typedef struct { struct PoolRec *phys; uint64_t off; } VPage;   // virtual page -> physical pool and offset

#define FLUSH_RANGES 32                // flush ranges a mirrored pool keeps between two syncs (flush_add)
typedef struct { uint64_t lo, hi; } FlushRange;

typedef struct PoolRec {
    const pool_t *key;
    int kind;
    uint32_t flags;
    uint64_t lo, size;                 // GPU address range of the pool
    uint64_t import_lo, import_hi;     // POOL_IMPORT: the imported host range (aligned outwards)
    VkBuffer buf;
    VkDeviceMemory mem;                // POOL_IMPORT
    struct Span *span;                 // POOL_IMPORT as a dma-buf: the span whose memory buf is bound to ("dma-buf imports")
    VmaAllocation alloc;               // POOL_PHYS
    VPage *pt;                         // POOL_VIRT: size / 64 KiB entries
    uint64_t npages;
    volatile int32_t used;                // resolved at least once: the GPU may still read it at finalize
    // NX2NAVI_TRACE=perf dirty census (vk_mem_dirty_submit), replay thread under the submit lock only:
    uint32_t bound_sub;                // last submit that bound it as a buffer (dirty_sub), 0 never
    uint32_t synced_gen;               // page generation its lazy-mirror count covers; DIRTY_UNSYNCED until scanned
    uint8_t bufbound;                  // ever bound as a vertex, index, uniform, storage or indirect buffer
    uint8_t untracked;                 // the last dirty scan found no write watch over it
    // VRAM mirror (POOL_IMPORT, "VRAM mirror" below). mstate and mbuf change on the replay thread only.
    uint8_t mstate;                    // MS_NONE until the first buffer use, then MS_ON or MS_REFUSED
    uint32_t gpa_rec;                  // POOL_IMPORT: its record in the device address table (vk_gpa.c), 0 none
    uint8_t gpa_flip;                  // as pend: the next prologue records the store that hands its table record to
                                       // the mirror (gpa_flip_cmds); 0 when the table names the mirror, or none exists
    struct GpaMap { uint64_t lo, hi; uint32_t rec; } *vmaps;   // POOL_VIRT: its mapped ranges in the table
    int nvmaps, cvmaps;
    VkBuffer mbuf;                     // MS_ON: device-local copy of [import_lo, import_hi), same offsets as buf
    VmaAllocation malloc_;
    uint32_t acc_sub;                  // replay: the last submission that synced the mirror (mirror_sync)
    uint32_t pm_frame;                 // replay: the frame (pframe) of pm_cur
    uint8_t pm_cur, pm_prev;           // replay: the places in their frame (vk_sub_in_frame, 7 the rest) of the submissions
                                       // that synced it, in frame pm_frame and in the one before (NX2NAVI_PRESNAP_PLACE)
    uint32_t wb_sub;                   // replay: the last submission that wrote mirror bytes back to guest memory
    uint64_t wb_fence;                 // replay: that submission's fence (mirror_capture waits for no write-back)
    uint8_t wb_kept;                   // replay: a write-back left bytes in the mirror alone (wb_skip): never evicted
    uint32_t retry_sub;                // replay: a mirror refused for the VRAM budget or a failed allocation is asked
                                       // again from here, 0 never
    uint8_t refused_why;               // replay: the MR_* reason of its refusal, for the counts when it is asked again
    uint8_t asked_room;                // replay: its refusal already asked the textures for room (once until it gets one)
    uint8_t twin;                      // MS_ON: the table's writes reach guest memory too (twin_wanted); 0: the mirror only
    uint32_t vd_wrote_sub, vd_use_sub; // verify mode: the last submission that bound it written / used its mirror
    uint64_t vd_wrote_n;               // verify mode: written bindings of it so far
    uint32_t mgen;                     // gen_lock: every page write stamped up to this generation is in the mirror
                                       // (or in a copy queued before its next use)
    FlushRange fl[FLUSH_RANGES + 1];   // flush_lock: nvnMemoryPoolFlushMappedRange pages not copied yet (flush_add)
    int fl_n;                          // flush_lock: ranges in fl; read without the lock to skip an empty list
    uint64_t fl_calls, fl_sum;         // flush_lock: the calls and bytes behind fl, for mirror_sync's statistics
    uint64_t hot_bytes;                // replay: uploads, write-backs and merges since the last hot check (mirror_hot)
    uint64_t hot_up;                   // of them the uploads: pages the CPU wrote (mirror_hot leaves such pools mirrored)
    // Shadow storage (POOL_IMPORT in shadow mode, "shadow storage" below): buf is a host-visible copy of the guest pages.
    VmaAllocation salloc;
    uint8_t *smap;                     // its mapping; offsets as in buf (from import_lo)
    uint32_t sgen;                     // gen_lock: every page write stamped up to this generation is in the shadow
    uint64_t sborn;                    // the value of the first submission that can have used the shadow (shadow_pool)
    volatile uint8_t stouched;         // on the touched list: the GPU may use it in the submission being recorded
    uint32_t stab_i;                   // shadow_lock: 1 + its index in stab (the table's shadows), 0 when not there
    // shadow_lock: base copies of the pages a noted GPU write covers (merge_page), open addressing by page address;
    // csb a power of two, page 0 an empty slot
    struct SBase { uint64_t page; uint8_t *base; } *sb;
    uint32_t nsb, csb;
    // shadow_lock ("Pages a shader stores into through the device address table" below): a mark per page of
    // [import_lo, import_hi), the hash of the shadow page as its last whole copy-in left it, 0 not known; the pages
    // found stored into (guest addresses), and whether the pool is on the list of pools that have such pages
    uint32_t *smark;
    uint64_t *tpg;
    uint32_t ntpg, ctpg;
    uint8_t tlisted;
    uint64_t t_over;                   // whole copy-ins that found such a store in the page they overwrote, so far
    uint64_t t_over_log;               // t_over at the last log line
    uint64_t s_in;                     // NX2NAVI_SHADOW_CHECK: bytes copied in whole since the last log line
    // NX2NAVI_TRACE=perf per-pool mirror traffic since the last mirror_pool_log, replay thread (the log reads and clears
    // them without a lock: a count lost to the race only blurs a diagnostic)
    uint64_t st_pages, st_flush, st_wb, st_merge;   // bytes: written pages and flush ranges copied up, GPU writes back
    uint32_t st_syncs;                              // syncs
} PoolRec;

static void mirror_forget(PoolRec *r);   // pool_lock exclusive: off the mirrored list
static int shadow_on;                    // shadow mode: imported pools get shadow storage instead (vk_mem_class_log)
static int shadow_pool(VkCtx *c, PoolRec *r, uint64_t addr, uint64_t size);
static void shadow_touch(PoolRec *r);
static void shadow_forget(PoolRec *r);
static void pool_retiring(PoolRec *r);  // pool_fini: kept until pool_retire (or vk_mem_shutdown)
static void shadow_table_add(PoolRec *r);
static void pool_retire(void *arg);    // completion thread: destroys a finalized pool's record
static int pool_fini_idle(void);
static void pool_fini_wait(void);
static void pool_wait_add(PoolRec *r);
static void grow(void **arr, int *cap, size_t elem);

static PlatMutex pool_lock = PLAT_MUTEX_INIT;
// Bumped under pool_lock (exclusive) by every change to the index or a page table: vk_mem_resolve's per-thread hits
// stay valid while it is unchanged.
static uint64_t pool_gen = 1;
// Bumped when a buffer use may get another buffer for the same span (a mirror created, refused or forgotten) and at
// every submission's start (mirror syncs are per submission). vk_mem_gen: both together.
static uint64_t mirror_gen;
static pidx pool_index;                // POOL_IMPORT, POOL_PHYS and POOL_VIRT records by GPU address
static PoolRec **phys_list;            // physical pools by key, for MapVirtual requests
static int nphys, cap_phys;
static int32_t n_imports, n_import_failures;
static PFN_vkGetMemoryHostPointerPropertiesEXT p_host_props;

static VkBufferUsageFlags pool_usage(const VkCtx *c) {
    VkBufferUsageFlags u = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (c->has_transform_feedback)
        u |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT;
    if (c->has_conditional_rendering) u |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
    if (vk_gpa_wanted()) u |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;   // pages of the device address table
    return u;
}

static int memory_type(const VkCtx *c, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < c->mem_props.memoryTypeCount; i++)
        if ((bits >> i & 1) && (c->mem_props.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    for (uint32_t i = 0; i < c->mem_props.memoryTypeCount; i++)
        if (bits >> i & 1) return (int)i;
    return -1;
}

// ---------------------------------------------------------------- dma-buf imports
// Where guest memory is a memfd's (Linux, NX2NAVI_DMABUF, on by default: platform/linux.c "Guest memory as a memfd's"), a
// CPU-visible pool is imported as a dma-buf of its own pages, not as a host pointer and not as a shadow copy. It keeps
// what an import is to the rest of this file (the GPU's writes are in guest memory once the GPU has made them, the
// device address table's twin is guest memory, mirrors sync from it and write back into it), without amdgpu's costs
// for host pointers: those are userptr objects, which the kernel looks up page by page at every submission and
// invalidates whenever the write watch or a freeze re-protects their pages (about 0.2 frames a second in Kokiri
// Forest), which is why Linux had shadow storage. A dma-buf's pages are pinned once and the process's page tables
// never matter to it again: the write watch, the freezes and the lazy write-back's armed pages leave the GPU alone.
// RADV puts every imported memory object in every submission, at about 0.6 us each (RX 6800 XT, Linux 7.3, Mesa 26.2:
// 2.6 ms a vkQueueSubmit with 4096 of them, 0.04 ms with 64), and the game keeps over 4,000 pools, so a pool does not
// get a dma-buf of its own: guest addresses are cut into aligned spans of NX2NAVI_DMABUF_MB (64 by default), the first
// pool made in a span imports the span's dma-buf, and every pool is a buffer bound into the memory of a span that holds
// it, at its offset. A pool that no span holds gets a new one over the aligned range around it, cut back at the spans
// already there (span_get): spans overlap only where such a pool itself lies in an older span, because every span's
// pages count in amdgpu's GTT use (an overlap twice) and stay pinned. A span whose
// last pool is gone stays SPAN_KEEP frames for the pools the game makes again at the same addresses, then is freed
// (span_gc); its dma-buf goes with the memory.
// A new span pins its pages: about 13 us per MiB the game had touched and 260 us per MiB it had not, which are
// allocated then (the same RX 6800 XT machine), so a 64 MiB span costs 1-17 ms once. A span over udmabuf's size limit
// (/sys/module/udmabuf/parameters/size_limit_mb) is not made; such a pool, or one outside the memfd, falls back to a
// host-pointer import, which a shared mapping's pages refuse on RADV: it gets no GPU buffer (logged).
typedef struct Span { uint64_t lo, hi; VkDeviceMemory mem; int32_t refs; uint64_t idle_frame; } Span;
#define SPAN_KEEP 600
static int dmabuf_on;                  // mirror_mode
static int dmabuf_exts;                // vk_mem_dmabuf_caps: the device has VK_KHR_external_memory_fd and _dma_buf
static uint64_t dmabuf_limit;          // udmabuf's size limit, bytes
static uint64_t span_size = 64ull << 20;
static PlatMutex span_lock = PLAT_MUTEX_INIT;   // after pool_lock
static Span **spans;                   // span_lock
static int nspans, cspans;
static uint32_t dmabuf_type = UINT32_MAX;   // span_lock: the memory type of the imports, asked at the first one
static PFN_vkGetMemoryFdPropertiesKHR p_fd_props;
static int64_t sp_live_bytes;          // span_lock
static uint64_t sp_made, sp_made_bytes, sp_ns, sp_max_ns, sp_freed, sp_freed_bytes, sp_fail, sp_fallback;   // span_lock, since the log
static uint64_t sp_fail_all;           // span_lock
static volatile uint64_t span_frame;   // the frame vk_mem_census last saw
#ifdef _WIN32
static void dmabuf_close(int fd) { (void)fd; }
#else
#include <unistd.h>
static void dmabuf_close(int fd) { close(fd); }
#endif

void vk_mem_dmabuf_caps(int exts) { dmabuf_exts = exts; }
int vk_mem_dmabuf_on(void) { return dmabuf_on; }

// The memory type of the imports, from the types the driver offers for the dma-buf fd: cached where offered, as guest
// memory is to the CPU (RADV: the GPU snoops it); not one of AMD's device-coherent or device-uncached types, which the
// third pass takes only when nothing else is offered. Sets dmabuf_type (UINT32_MAX: none fits); returns the offer.
static uint32_t dmabuf_pick_type(VkDevice dev, const VkPhysicalDeviceMemoryProperties *mp, int fd) {
    if (!p_fd_props) p_fd_props = (PFN_vkGetMemoryFdPropertiesKHR)vk_dpa(dev, "vkGetMemoryFdPropertiesKHR");
    VkMemoryFdPropertiesKHR fp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    uint32_t bits = p_fd_props && p_fd_props(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp) == VK_SUCCESS
                        ? fp.memoryTypeBits : 0;
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    dmabuf_type = UINT32_MAX;
    for (int w = 0; w < 3 && dmabuf_type == UINT32_MAX; w++)
        for (uint32_t i = 0; i < mp->memoryTypeCount && dmabuf_type == UINT32_MAX; i++) {
            VkMemoryPropertyFlags f = mp->memoryTypes[i].propertyFlags;
            if ((bits >> i & 1) && (w == 0 ? f == (hv | VK_MEMORY_PROPERTY_HOST_CACHED_BIT) : w == 1 ? f == hv : (f & hv) == hv))
                dmabuf_type = i;
        }
    rt_log("mem: dma-bufs of guest memory may have memory types %#x; type %d taken", bits,
           dmabuf_type == UINT32_MAX ? -1 : (int)dmabuf_type);
    return bits;
}

static uint64_t dmabuf_any;            // a page of guest memory a dma-buf can be made of (vk_mem_dmabuf_probe)

// After device creation, before the first pool and before anything asks for the storage mode (vk_mem_lazy_decide): one
// page of guest memory is made a dma-buf, imported and bound to a buffer, as every pool will be. Any step refused turns
// the dma-buf imports off for the run: the pools are shadow copies, as with NX2NAVI_DMABUF=0. A refusal later, for
// one span, leaves its pools without a GPU buffer (span_log counts them), so the probe asks once for what all need.
static int s_learn, s_check;
void vk_mem_dmabuf_probe(VkDevice dev, const VkPhysicalDeviceMemoryProperties *mp, int bda) {
    if (!dmabuf_on) return;
    const char *no = NULL;
    long err = 0;
    uint32_t bits = 0;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    int fd = plat_mem_dmabuf(dmabuf_any, 4096);
    if (fd < 0) {
        no = "udmabuf refused a dma-buf of guest memory", err = errno;
    } else if (bits = dmabuf_pick_type(dev, mp, fd), dmabuf_type == UINT32_MAX) {
        no = "the device offers no host-visible coherent memory type for it";
        dmabuf_close(fd);
    } else {
        VkImportMemoryFdInfoKHR im = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = fd};
        VkMemoryAllocateFlagsInfo fi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .pNext = &im,
                                        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = bda ? (void *)&fi : (void *)&im,
                                   .allocationSize = 4096, .memoryTypeIndex = dmabuf_type};
        VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                               .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = 4096,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                          (bda ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0),
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VkMemoryRequirements req;
        VkResult r = vkAllocateMemory(dev, &ai, NULL, &mem);
        if (r != VK_SUCCESS) {
            mem = VK_NULL_HANDLE;
            dmabuf_close(fd);   // a failed import leaves the descriptor with us
            no = "the device refused to import it", err = r;
        } else if ((r = vkCreateBuffer(dev, &bi, NULL, &buf)) != VK_SUCCESS) {
            buf = VK_NULL_HANDLE;
            no = "no buffer for it", err = r;
        } else if (vkGetBufferMemoryRequirements(dev, buf, &req), !(req.memoryTypeBits >> dmabuf_type & 1)) {
            no = "a buffer cannot be bound to its memory type", err = (long)req.memoryTypeBits;
        } else if ((r = vkBindBufferMemory(dev, buf, mem, 0)) != VK_SUCCESS) {
            no = "binding a buffer to it failed", err = r;
        }
    }
    if (buf) vkDestroyBuffer(dev, buf, NULL);
    if (mem) vkFreeMemory(dev, mem, NULL);
    if (fd >= 0) plat_mem_dmabuf_done(dmabuf_any, 4096);
    if (!no) return;
    dmabuf_on = 0, shadow_on = 1;
    s_learn = rt_knob("SHADOW_TABLE_STORES", "0") == 1;
    s_check = cfg_bool("NX2NAVI_SHADOW_CHECK");
    rt_log("mem: dma-buf imports off: %s (%ld; memory types %#x): CPU-visible pools are host-visible copies of guest "
           "memory (shadow storage), as with NX2NAVI_DMABUF=0", no, err, bits);
}

static void span_fail(const char *what, uint64_t lo, uint64_t hi, long err) {   // span_lock held
    sp_fail++;
    if (++sp_fail_all <= 8)
        rt_log("mem: dma-buf of %#llx+%#llx: %s (%ld)%s", (unsigned long long)lo, (unsigned long long)(hi - lo), what, err,
               sp_fail_all == 8 ? "; no more of these lines" : "");
}

// span_lock held: a new span over [lo, hi), NULL when udmabuf or the import refuses it.
static Span *span_create(VkCtx *c, uint64_t lo, uint64_t hi) {
    uint64_t t0 = qpc();
    int fd = plat_mem_dmabuf(lo, hi - lo);
    if (fd < 0) {
        span_fail("udmabuf refused it", lo, hi, errno);
        return NULL;
    }
    if (dmabuf_type == UINT32_MAX) {   // every dma-buf of guest memory has the same types: asked once
        dmabuf_pick_type(c->device, &c->mem_props, fd);
        if (dmabuf_type == UINT32_MAX) {
            dmabuf_close(fd);
            plat_mem_dmabuf_done(lo, hi - lo);
            span_fail("no host-visible coherent memory type", lo, hi, 0);
            dmabuf_type = UINT32_MAX - 1;   // not asked again: every span fails
            return NULL;
        }
    }
    if (dmabuf_type >= c->mem_props.memoryTypeCount) {
        dmabuf_close(fd);
        plat_mem_dmabuf_done(lo, hi - lo);
        return NULL;
    }
    VkImportMemoryFdInfoKHR im = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                  .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = fd};
    // A buffer with device address usage needs memory allocated for device addresses (vk_gpa.c).
    VkMemoryAllocateFlagsInfo fi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .pNext = &im,
                                    .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .pNext = vk_gpa_wanted() ? (void *)&fi : (void *)&im, .allocationSize = hi - lo,
                               .memoryTypeIndex = dmabuf_type};
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkResult res = vkAllocateMemory(c->device, &ai, NULL, &mem);
    Span *s = res == VK_SUCCESS ? calloc(1, sizeof *s) : NULL;
    if (!s) {
        if (res == VK_SUCCESS) vkFreeMemory(c->device, mem, NULL);
        else dmabuf_close(fd);   // a failed import leaves the descriptor with us; a successful one took it
        plat_mem_dmabuf_done(lo, hi - lo);
        span_fail("the import failed", lo, hi, res);
        return NULL;
    }
    s->lo = lo, s->hi = hi, s->mem = mem;
    if (nspans == cspans) grow((void **)&spans, &cspans, sizeof *spans);
    spans[nspans++] = s;
    uint64_t ns = qpc() - t0;
    sp_made++, sp_made_bytes += hi - lo, sp_ns += ns, sp_live_bytes += (int64_t)(hi - lo);
    if (ns > sp_max_ns) sp_max_ns = ns;
    return s;
}

// span_lock held: the span a pool's [lo, hi) is bound into, with a reference for it; NULL when none can be made. al is
// the alignment the pool's buffer needs inside the span's memory.
// A new span covers the span_size-aligned range around the pool, less what other spans hold below and above the pool:
// a pool across an edge of an older span then pins only its own pages twice, not the older span's whole range. With
// the whole aligned range, Hyrule Field's area loads left 11.5-11.7 GiB of spans counted in amdgpu's GTT use; cut back,
// 9.5-9.6 GiB, over the same 9.3 GiB of distinct pages, at the same frame rate (RX 6800 XT, 2 pairs).
static Span *span_get(VkCtx *c, uint64_t lo, uint64_t hi, uint64_t al) {
    Span *best = NULL;
    for (int i = 0; i < nspans; i++)
        if (spans[i]->lo <= lo && hi <= spans[i]->hi && (!best || spans[i]->hi - spans[i]->lo > best->hi - best->lo))
            best = spans[i];
    if (!best) {
        uint64_t a = lo / span_size * span_size, b = (hi + span_size - 1) / span_size * span_size;
        for (int i = 0; i < nspans; i++) {   // no span holds [lo, hi): none reaches below lo and above hi at once
            const Span *s = spans[i];
            if (s->lo < lo && s->hi > a) a = s->hi < lo ? s->hi : lo;
            if (s->hi > hi && s->lo < b) b = s->lo > hi ? s->lo : hi;
        }
        if (al > 1) a = lo - (lo - a) / al * al;   // the pool's offset in the span a multiple of al
        if (b - a <= dmabuf_limit) best = span_create(c, a, b);
        // the aligned range may leave the memfd's reservation: the pool's own pages then
        if (!best && (a != lo || b != hi) && hi - lo <= dmabuf_limit) best = span_create(c, lo, hi);
    }
    if (best) best->refs++;
    return best;
}

static void span_put(Span *s) {
    plat_mutex_lock(&span_lock);
    if (!--s->refs) s->idle_frame = __atomic_load_n(&span_frame, __ATOMIC_RELAXED);
    plat_mutex_unlock(&span_lock);
}

// Frees the spans no pool has used for SPAN_KEEP frames (all: every span without a pool). A span without a reference
// is out of the GPU's work: a pool's reference goes when its record is destroyed, after the GPU is done with it.
static void span_gc(uint64_t frame, int all) {
    VkCtx *c = vk_ctx();
    if (!c || !dmabuf_on) return;
    Span *gone[64];
    int ng;
    do {
        ng = 0;
        plat_mutex_lock(&span_lock);
        for (int i = 0; i < nspans && ng < 64;)
            if (!spans[i]->refs && (all || frame - spans[i]->idle_frame >= SPAN_KEEP)) {
                gone[ng++] = spans[i];
                sp_freed++, sp_freed_bytes += spans[i]->hi - spans[i]->lo;
                sp_live_bytes -= (int64_t)(spans[i]->hi - spans[i]->lo);
                spans[i] = spans[--nspans];
            } else {
                i++;
            }
        plat_mutex_unlock(&span_lock);
        for (int i = 0; i < ng; i++) {   // the dma-buf goes with the memory; then its pages are let go
            vkFreeMemory(c->device, gone[i]->mem, NULL);
            plat_mem_dmabuf_done(gone[i]->lo, gone[i]->hi - gone[i]->lo);
            free(gone[i]);
        }
    } while (ng == 64);
}

static void span_log(uint64_t frame) {
    if (!dmabuf_on) return;
    plat_mutex_lock(&span_lock);
    int64_t refs = 0;
    for (int i = 0; i < nspans; i++) refs += spans[i]->refs;
    rt_log("mem: dma-buf spans at frame %llu: %d live, %.0f MiB, %lld pools in them; since the last line %llu made "
           "(%.0f MiB, %.1f ms, the longest %.1f ms), %llu freed (%.0f MiB), %llu refused, %llu pools left out",
           (unsigned long long)frame, nspans, sp_live_bytes / 1048576.0, (long long)refs, (unsigned long long)sp_made,
           sp_made_bytes / 1048576.0, qpc_ms(sp_ns), qpc_ms(sp_max_ns), (unsigned long long)sp_freed,
           sp_freed_bytes / 1048576.0, (unsigned long long)sp_fail, (unsigned long long)sp_fallback);
    sp_made = sp_made_bytes = sp_ns = sp_max_ns = sp_freed = sp_freed_bytes = sp_fail = sp_fallback = 0;
    plat_mutex_unlock(&span_lock);
}

// A pool's buffer bound into a span's memory; 0 when no span can be made for it.
static int import_dmabuf(VkCtx *c, PoolRec *r, uint64_t addr, uint64_t size) {
    r->import_lo = addr & ~0xfffull;
    r->import_hi = (addr + size + 0xfff) & ~0xfffull;
    uint64_t n = r->import_hi - r->import_lo;
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = n,
                             .usage = pool_usage(c), .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkResult res = vkCreateBuffer(c->device, &bi, NULL, &r->buf);
    if (res != VK_SUCCESS) {
        r->buf = VK_NULL_HANDLE;
        return 0;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c->device, r->buf, &req);
    uint64_t need = ((req.size > n ? req.size : n) + 0xfff) & ~0xfffull;
    plat_mutex_lock(&span_lock);
    Span *s = span_get(c, r->import_lo, r->import_lo + need, req.alignment);
    int ok = s && (r->import_lo - s->lo) % (req.alignment ? req.alignment : 1) == 0 && (req.memoryTypeBits >> dmabuf_type & 1);
    if (!s) sp_fallback++;
    plat_mutex_unlock(&span_lock);
    if (ok && (res = vkBindBufferMemory(c->device, r->buf, s->mem, r->import_lo - s->lo)) != VK_SUCCESS) ok = 0;
    if (!ok) {
        if (s) {
            rt_log("vk: pool %#llx+%#llx: binding into its dma-buf span failed (alignment %llu, types %#x, %d)",
                   (unsigned long long)addr, (unsigned long long)size, (unsigned long long)req.alignment,
                   req.memoryTypeBits, (int)res);
            span_put(s);
        }
        vkDestroyBuffer(c->device, r->buf, NULL);
        r->buf = VK_NULL_HANDLE;
        return 0;
    }
    r->span = s;
    plat_inc32(&n_imports);
    return 1;
}

// CPU-visible storage: the guest pages themselves become the buffer's memory (VK_EXT_external_memory_host, or a
// dma-buf of them: "dma-buf imports" above).
static int import_pool(VkCtx *c, PoolRec *r, uint64_t addr, uint64_t size) {
    if (shadow_on) return shadow_pool(c, r, addr, size);
    if (dmabuf_on && import_dmabuf(c, r, addr, size)) return 1;
    if (!p_host_props)
        p_host_props = (PFN_vkGetMemoryHostPointerPropertiesEXT)vk_dpa(c->device, "vkGetMemoryHostPointerPropertiesEXT");
    uint64_t al = c->min_imported_host_alignment ? c->min_imported_host_alignment : 0x1000;
    r->import_lo = addr & ~(al - 1);
    r->import_hi = (addr + size + al - 1) & ~(al - 1);
    uint64_t n = r->import_hi - r->import_lo;
    VkMemoryHostPointerPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult res = p_host_props ? p_host_props(c->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                               (void *)(uintptr_t)r->import_lo, &hp)
                                : VK_ERROR_EXTENSION_NOT_PRESENT;
    if (res != VK_SUCCESS || !hp.memoryTypeBits) {
        rt_log("vk: pool %#llx+%#llx: host pointer not importable (%d)", (unsigned long long)addr, (unsigned long long)size, (int)res);
        return 0;
    }
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = n,
                             .usage = pool_usage(c), .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if ((res = vkCreateBuffer(c->device, &bi, NULL, &r->buf)) != VK_SUCCESS) {
        rt_log("vk: pool %#llx: vkCreateBuffer for the import failed (%d)", (unsigned long long)addr, (int)res);
        return 0;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c->device, r->buf, &req);
    int type = memory_type(c, req.memoryTypeBits & hp.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0 || req.size > n) {
        rt_log("vk: pool %#llx: no memory type for the import (buffer %#x, host %#x, size %#llx of %#llx)",
               (unsigned long long)addr, req.memoryTypeBits, hp.memoryTypeBits, (unsigned long long)req.size,
               (unsigned long long)n);
        vkDestroyBuffer(c->device, r->buf, NULL);
        r->buf = VK_NULL_HANDLE;
        return 0;
    }
    VkImportMemoryHostPointerInfoEXT im = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                           .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                           .pHostPointer = (void *)(uintptr_t)r->import_lo};
    // A buffer with device address usage needs memory allocated for device addresses (vk_gpa.c).
    VkMemoryAllocateFlagsInfo fi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .pNext = &im,
                                    .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = vk_gpa_wanted() ? (void *)&fi : (void *)&im,
                               .allocationSize = n, .memoryTypeIndex = (uint32_t)type};
    if ((res = vkAllocateMemory(c->device, &ai, NULL, &r->mem)) != VK_SUCCESS ||
        (res = vkBindBufferMemory(c->device, r->buf, r->mem, 0)) != VK_SUCCESS) {
        rt_log("vk: pool %#llx+%#llx: host memory import failed (%d)", (unsigned long long)addr, (unsigned long long)size, (int)res);
        if (r->mem) vkFreeMemory(c->device, r->mem, NULL);
        vkDestroyBuffer(c->device, r->buf, NULL);
        r->buf = VK_NULL_HANDLE;
        r->mem = VK_NULL_HANDLE;
        return 0;
    }
    plat_inc32(&n_imports);
    return 1;
}

// Physical pool: device-local memory of the pool size that both buffers and optimal-tiling images can be bound to.
static int alloc_physical(VkCtx *c, PoolRec *r) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = r->size, .usage = pool_usage(c),
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkResult res = vkCreateBuffer(c->device, &bi, NULL, &r->buf);
    if (res != VK_SUCCESS) { rt_log("vk: physical pool: vkCreateBuffer failed (%d)", (int)res); return 0; }
    VkMemoryRequirements rb;
    vkGetBufferMemoryRequirements(c->device, r->buf, &rb);
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {256, 256, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    VkDeviceImageMemoryRequirements di = {.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, .pCreateInfo = &ii};
    VkMemoryRequirements2 ri = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    vkGetDeviceImageMemoryRequirements(c->device, &di, &ri);
    uint32_t bits = rb.memoryTypeBits & ri.memoryRequirements.memoryTypeBits;
    if (!bits) bits = rb.memoryTypeBits;
    VkDeviceSize align = NVN_VPOOL_PAGE;
    if (rb.alignment > align) align = rb.alignment;
    if (ri.memoryRequirements.alignment > align) align = ri.memoryRequirements.alignment;
    VkMemoryRequirements req = {.size = (rb.size + align - 1) & ~(align - 1), .alignment = align, .memoryTypeBits = bits};
    VmaAllocationCreateInfo aci = {.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
                                   .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    res = vmaAllocateMemory(c->vma, &req, &aci, &r->alloc, NULL);
    if (res != VK_SUCCESS) {   // video memory full (VMA heap limits, vk_device.c): any memory the pool can live in
        VmaAllocationCreateInfo any = {.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
                                       .preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
        r->alloc = NULL;
        res = vmaAllocateMemory(c->vma, &req, &any, &r->alloc, NULL);
        if (res == VK_SUCCESS)
            rt_log("vk: physical pool of %#llx bytes: no video memory left, placed in system memory",
                   (unsigned long long)r->size);
    }
    if (res != VK_SUCCESS || (res = vmaBindBufferMemory(c->vma, r->alloc, r->buf)) != VK_SUCCESS) {
        rt_log("vk: physical pool of %#llx bytes: device-local allocation failed (%d)", (unsigned long long)r->size, (int)res);
        if (r->alloc) vmaFreeMemory(c->vma, r->alloc);
        vkDestroyBuffer(c->device, r->buf, NULL);
        r->buf = VK_NULL_HANDLE;
        r->alloc = NULL;
        return 0;
    }
    if (vk_fix_zero()) {   // Switch pool memory starts zeroed; once per pool, not per image placed in it
        VkCommandBuffer cb = vk_upload_begin();
        vkCmdFillBuffer(cb, r->buf, 0, VK_WHOLE_SIZE, 0);
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(cb, &di);
        vk_upload_end();
    }
    return 1;
}

int vk_fix_zero(void) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2NAVI_FIX_ZERO");
    }
    return on;
}

static void index_put(PoolRec *r) {
    __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
    pidx_ent e = {.lo = r->lo, .hi = r->lo + r->size, .base = r->lo, .buf = r, .gpu = r->lo, .key = r->key};
    pidx_remove(&pool_index, r->key, NULL);   // an object initialized again without Finalize
    if (!pidx_insert(&pool_index, &e)) rt_fatal("vk: out of memory for the pool index");
}

static PoolRec *phys_find(const pool_t *key) {
    for (int i = 0; i < nphys; i++) if (phys_list[i]->key == key) return phys_list[i];
    return NULL;
}

static void destroy_rec(VkCtx *c, PoolRec *r) {
    if (r->mbuf) vmaDestroyBuffer(c->vma, r->mbuf, r->malloc_);
    if (r->salloc) vmaDestroyBuffer(c->vma, r->buf, r->salloc), r->buf = VK_NULL_HANDLE;
    if (r->buf) vkDestroyBuffer(c->device, r->buf, NULL);
    if (r->mem) vkFreeMemory(c->device, r->mem, NULL);
    if (r->span) span_put(r->span);
    if (r->alloc) vmaFreeMemory(c->vma, r->alloc);
    for (uint32_t i = 0; i < r->csb; i++) free(r->sb[i].base);   // shadow base copies shadow_forget left (shutdown)
    free(r->sb);
    free(r->pt);
    free(r);
}

static int pool_init(pool_t *p) {
    VkCtx *c = vk_ctx();
    if (!c) { rt_log("vk: MemoryPoolInitialize before vk_init"); return 0; }
    // The GPU never sees a GPU_NO_ACCESS pool, except a physical one (0x189; 1.4.3 also 0x18c, CPU-visible) through the
    // virtual pools that map it. Either gets device-local memory: 1.4.3's CPU never writes its 0x18c pool's storage
    // (unchanged over 3600 frames of the intro), so the guest storage and the GPU copy need no sync.
    if ((p->flags & NVN_MEMORY_POOL_FLAGS_GPU_NO_ACCESS) && !(p->flags & NVN_MEMORY_POOL_FLAGS_PHYSICAL)) return 1;
    // A pool of zero bytes (NX2navi makes some now and then) holds no address the GPU can use: it gets no record. A record
    // would import 0 bytes (invalid usage; the driver refuses the memory) and put an empty range into the index at the
    // pool's address, where a lookup of the pool that starts there found the empty one first. vk_pool_fini finds no
    // record for it and does nothing.
    if (!p->size) return 1;
    PoolRec *r = calloc(1, sizeof *r);
    if (!r) rt_fatal("vk: out of memory for a pool record");
    r->key = p;
    r->flags = p->flags;
    r->lo = (uintptr_t)p->mem;
    r->size = p->size;
    int ok;
    if (p->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) {
        r->kind = POOL_VIRT;
        r->npages = p->size / NVN_VPOOL_PAGE;
        r->pt = calloc(r->npages ? r->npages : 1, sizeof *r->pt);
        ok = r->pt != NULL;
    } else if (p->flags & NVN_MEMORY_POOL_FLAGS_PHYSICAL) {
        r->kind = POOL_PHYS;
        ok = alloc_physical(c, r);
    } else {
        r->kind = POOL_IMPORT;
        mem_thaw((uintptr_t)p->mem, p->size);   // the import may need write access to the pages
        ok = import_pool(c, r, (uintptr_t)p->mem, p->size);
        if (!ok) {
            // Not fatal for the game: the pool works for the CPU, and vk_mem_resolve reports its addresses as unbacked.
            long n = plat_inc32(&n_import_failures);
            if (n <= 8) rt_log("vk: pool %p (flags %#x, %#llx bytes) has no GPU buffer (%ld so far)", (void *)p->mem,
                               p->flags, (unsigned long long)p->size, n);
            r->buf = VK_NULL_HANDLE;
            ok = 1;
        }
    }
    if (!ok) { free(r->pt); free(r); return 0; }
    plat_mutex_lock(&pool_lock);
    index_put(r);
    if (r->kind == POOL_PHYS) {
        if (nphys == cap_phys) {
            cap_phys = cap_phys ? cap_phys * 2 : 16;
            phys_list = realloc(phys_list, (size_t)cap_phys * sizeof *phys_list);
            if (!phys_list) rt_fatal("vk: out of memory for the physical pool list");
        }
        phys_list[nphys++] = r;
    }
    plat_mutex_unlock(&pool_lock);
    // The imported pages, or the shadow that stands for them, go into the device address table: shaders that read a
    // pointer from memory reach them there. A shadow in the table is kept up to date for the table's readers whether
    // or not the replay resolves it (vk_mem_shadow_sync).
    if (r->kind == POOL_IMPORT && (r->mem || r->span || r->smap) && r->buf && r->import_hi > r->import_lo) {
        r->gpa_rec = vk_gpa_map(r->import_lo, r->import_hi, r->buf, r->import_lo);
        if (r->smap && r->gpa_rec) shadow_table_add(r);
    }
    return 1;
}

// vk_gpa_unmap: the record of another live imported pool that holds the 4 KiB page at a whole (overlapping imports
// of the same guest memory), 0 for none.
static uint32_t gpa_cover(uint64_t a) {
    uint32_t rec = 0;
    plat_mutex_lock_shared(&pool_lock);
    for (int i = pidx_upper(&pool_index, a) - 1; i >= 0 && !rec; i--) {
        const PoolRec *o = pool_index.e[i].buf;
        if (o->kind == POOL_IMPORT && o->gpa_rec && o->import_lo <= a && o->import_hi >= a + 4096) rec = o->gpa_rec;
        if (i && pool_index.maxhi[i - 1] <= a) break;
    }
    plat_mutex_unlock_shared(&pool_lock);
    return rec;
}

static void pool_fini(pool_t *p) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    pidx_ent e;
    plat_mutex_lock(&pool_lock);
    int had = pidx_remove(&pool_index, p, &e);
    __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
    PoolRec *r = had ? e.buf : NULL;
    if (r && r->mstate == MS_ON) mirror_forget(r);
    if (r && r->kind == POOL_PHYS) {
        for (int i = 0; i < nphys; i++)
            if (phys_list[i] == r) { phys_list[i] = phys_list[--nphys]; break; }
        for (int i = 0; i < pool_index.n; i++) {   // virtual pages that still map it become unmapped
            PoolRec *v = pool_index.e[i].buf;
            if (v->kind != POOL_VIRT) continue;
            for (uint64_t k = 0; k < v->npages; k++)
                if (v->pt[k].phys == r) {
                    v->pt[k].phys = NULL;
                    vk_gpa_clear(v->lo + k * NVN_VPOOL_PAGE, v->lo + (k + 1) * NVN_VPOOL_PAGE);   // the buffer goes away
                }
        }
    }
    plat_mutex_unlock(&pool_lock);
    if (!r) return;
    if (r->smap) shadow_forget(r);
    // Off the device address table first; the buffer outlives the submissions that may still read the old pages.
    if (r->gpa_rec) vk_gpa_unmap(r->import_lo, r->import_hi, r->gpa_rec, gpa_cover);
    for (int i = 0; i < r->nvmaps; i++) vk_gpa_unmap(r->vmaps[i].lo, r->vmaps[i].hi, r->vmaps[i].rec, NULL);
    free(r->vmaps), r->vmaps = NULL, r->nvmaps = 0;
    if (r->kind == POOL_IMPORT) mem_thaw(r->lo, r->size);   // the storage goes back to the guest writable
    // Images cut from this range go first; a used buffer or memory waits until the GPU is done with it, since the
    // guest gets the storage back when this returns.
    vk_texture_range_gone(r->lo, r->size, r->alloc);
    int narrow = 0;
    if (r->used && r->kind != POOL_VIRT) {
        if (pool_fini_idle()) {
            vk_upload_flush(1);
            vk_queue_lock();
            vkQueueWaitIdle(c->gfx_queue);
            vk_queue_unlock();
        } else {
            narrow = 1;
            pool_fini_wait();
        }
    }
    vk_mem_collect();
    if (narrow) {   // the Vulkan objects outlive the upload batches and the submission being recorded (pool_wait_add)
        pool_retiring(r);
        pool_wait_add(r);
        return;
    }
    // A record that waits for the next submission is on the retiring list until its callback runs: when no
    // submission follows (a pool freed at the end), vk_mem_shutdown destroys it. Left to the callback alone, a
    // shadow copy's buffer was still allocated when the allocator went, which stops the process there.
    // A mirrored pool's record and buffers outlive the submission being recorded, whose sync copies and write-backs may
    // name them (the game frees a pool only once the GPU is done with it, but its CPU writes may still be queued).
    if (r->mbuf) pool_retiring(r), vk_completion_add(vk_sync_issued() + 1, pool_retire, r);
    // the queue was not drained
    else if (r->gpa_rec && !r->used) pool_retiring(r), vk_completion_add(vk_sync_issued() + 1, pool_retire, r);
    // an unused physical pool may still be read through the virtual pages that mapped it (device addresses, which do not
    // mark it used): its buffer outlives the submission being recorded too
    else if (r->kind == POOL_PHYS && !r->used) pool_retiring(r), vk_completion_add(vk_sync_issued() + 1, pool_retire, r);
    else destroy_rec(c, r);
}

// NX2NAVI_TRACE=perf: a pool's initialize and finalize, with their imports, allocations, uploads and GPU waits, marked
// as work a frame can wait for (perf.c perf_mark_add).
int vk_pool_init(pool_t *p) {
    uint64_t t0 = perf_mark_begin();
    int r = pool_init(p);
    perf_mark_end(PERF_M_POOL, t0, p->size);
    return r;
}

void vk_pool_fini(pool_t *p) {
    uint64_t t0 = perf_mark_begin(), size = p->size;
    pool_fini(p);
    perf_mark_end(PERF_M_POOL, t0, size);
}

// pool_lock held: the device address table follows a MapVirtual request on [lo, hi) of virtual pool v. Ranges it
// covers whole leave the table (their records retire); the pages then name the physical pool's buffer at poff, or none.
static void gpa_map_virtual(PoolRec *v, uint64_t lo, uint64_t hi, PoolRec *ph, uint64_t poff) {
    if (!vk_gpa_wanted()) return;
    for (int i = 0; i < v->nvmaps;)
        if (v->vmaps[i].lo >= lo && v->vmaps[i].hi <= hi) {
            vk_gpa_unmap(v->vmaps[i].lo, v->vmaps[i].hi, v->vmaps[i].rec, NULL);
            v->vmaps[i] = v->vmaps[--v->nvmaps];
        } else {
            i++;
        }
    vk_gpa_clear(lo, hi);
    if (!ph || !ph->buf) return;
    uint32_t rec = vk_gpa_map(lo, hi, ph->buf, lo - poff);
    if (!rec) return;
    if (v->nvmaps == v->cvmaps) grow((void **)&v->vmaps, &v->cvmaps, sizeof *v->vmaps);
    v->vmaps[v->nvmaps++] = (struct GpaMap){lo, hi, rec};
}

// Requests were checked by the front end (nvn_api_core.c); apply them in order.
int vk_pool_map_virtual(pool_t *v, int n, const void *requests) {
    const nvn_mapping_t *rq = requests;
    plat_mutex_lock(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)v->mem, 0);
    PoolRec *r = e && e->key == v ? e->buf : NULL;
    int ok = r && r->kind == POOL_VIRT;
    __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
    for (int i = 0; ok && i < n; i++) {
        PoolRec *ph = rq[i].physical_pool ? phys_find((const pool_t *)(const void *)rq[i].physical_pool) : NULL;
        if (rq[i].physical_pool && !ph) {
            rt_log("vk: MapVirtual request %d names physical pool %p that has no Vulkan memory", i,
                   (const void *)rq[i].physical_pool);
            ok = 0;
            break;
        }
        uint64_t first = (uint64_t)rq[i].virtual_offset / NVN_VPOOL_PAGE, count = rq[i].size / NVN_VPOOL_PAGE;
        if (first + count > r->npages) { ok = 0; break; }
        for (uint64_t k = 0; k < count; k++)
            r->pt[first + k] = (VPage){ph, ph ? (uint64_t)rq[i].physical_offset + k * NVN_VPOOL_PAGE : 0};
        gpa_map_virtual(r, r->lo + first * NVN_VPOOL_PAGE, r->lo + (first + count) * NVN_VPOOL_PAGE, ph,
                        (uint64_t)rq[i].physical_offset);
    }
    plat_mutex_unlock(&pool_lock);
    if (!ok) rt_log("vk: MapVirtual on pool %p failed", (void *)v->mem);
    return ok;
}

// Per-thread copies of resolved import and physical pools that overlap no other index entry (so pidx_find picks them for
// any range inside), valid while pool_gen is unchanged: the replay resolves a few dozen pools thousands of times per
// frame (16 vertex streams and every buffer binding of each draw). Direct-mapped by the 4 KiB page of the address, so
// a hit is one compare however many pools a frame touches. NX2NAVI_RESOLVE_CHECK=1 compares every hit with the locked
// lookup and stops on a difference.
static int rec_class(const PoolRec *r) {
    if (r->kind != POOL_IMPORT) return VK_MCLS_DEVICE;
    return r->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS ? VK_MCLS_HOST_NOACC
         : r->flags & NVN_MEMORY_POOL_FLAGS_CPU_CACHED ? VK_MCLS_HOST_CACHED : VK_MCLS_HOST_UNC;
}

typedef struct { uint64_t gen, lo, hi, base, end; VkBuffer buf; VmaAllocation alloc; int cls; void *pool; } ResolveHit;
#define RHIT_SLOTS 512
static _Thread_local ResolveHit rhit[RHIT_SLOTS];

static ResolveHit *rhit_slot(uint64_t addr) { return &rhit[(addr >> 12 ^ addr >> 21) & (RHIT_SLOTS - 1)]; }

static int resolve_check_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("NX2NAVI_RESOLVE_CHECK");
        on = e && *e && strcmp(e, "0");
    }
    return on;
}

static int resolve_locked(uint64_t addr, uint64_t size, VkMemSpan *out, int touch);

// touch: the GPU may read the pool's buffer in the submission being recorded, so its shadow goes on the touched list
// (shadow storage). A lookup (vk_mem_lookup) leaves it off: the caller reads guest memory itself or only asks what
// backs the address. Shaders that reach a pool through the device address table resolve nothing: the part they run in
// brings every pool of the table up to date (vk_mem_shadow_sync).
static int mem_resolve(uint64_t addr, uint64_t size, VkMemSpan *out, int touch) {
    memset(out, 0, sizeof *out);
    uint64_t g = __atomic_load_n(&pool_gen, __ATOMIC_ACQUIRE), end = addr + size;
    const ResolveHit *h = rhit_slot(addr);
    if (end >= addr && h->gen == g && addr >= h->lo && end <= h->hi) {
        out->buffer = h->buf;
        out->offset = addr - h->base;
        out->avail = h->end - addr;
        out->alloc = h->alloc;
        out->cls = h->cls;
        out->pool = h->pool;
        if (h->alloc) out->alloc_offset = out->offset;
        if (shadow_on && touch && h->pool && !((PoolRec *)h->pool)->stouched) shadow_touch(h->pool);
        if (resolve_check_on()) {
            VkMemSpan s;
            if (!resolve_locked(addr, size, &s, touch) || memcmp(&s, out, sizeof s))
                rt_fatal("vk: resolve cache of 0x%llx differs from the pool index", (unsigned long long)addr);
        }
        return 1;
    }
    return resolve_locked(addr, size, out, touch);
}

int vk_mem_resolve(uint64_t addr, uint64_t size, VkMemSpan *out) { return mem_resolve(addr, size, out, 1); }
int vk_mem_lookup(uint64_t addr, uint64_t size, VkMemSpan *out) { return mem_resolve(addr, size, out, 0); }

// The pool index lookup under the lock; a non-overlapped import or physical pool goes into the address's cache slot.
static int resolve_locked(uint64_t addr, uint64_t size, VkMemSpan *out, int touch) {
    memset(out, 0, sizeof *out);
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)addr, size);
    PoolRec *r = e ? e->buf : NULL;
    int ok = 0;
    if (r && (r->kind == POOL_IMPORT ? r->buf != VK_NULL_HANDLE : r->kind == POOL_PHYS)) {
        int i = (int)(e - pool_index.e);
        if ((i == 0 || pool_index.maxhi[i - 1] <= e->lo) && (i + 1 == pool_index.n || pool_index.e[i + 1].lo >= e->hi)) {
            ResolveHit *h = rhit_slot(addr);
            h->gen = __atomic_load_n(&pool_gen, __ATOMIC_RELAXED);   // stable: changed only under the exclusive lock
            h->lo = e->lo, h->hi = e->hi, h->buf = r->buf, h->cls = rec_class(r);
            if (r->kind == POOL_IMPORT) h->base = r->import_lo, h->end = r->import_hi, h->alloc = NULL, h->pool = r;
            else h->base = r->lo, h->end = r->lo + r->size, h->alloc = r->alloc, h->pool = NULL;
        }
    }
    if (r && r->kind == POOL_IMPORT && r->buf) {
        out->cls = rec_class(r);
        out->pool = r;
        out->buffer = r->buf;
        out->offset = addr - r->import_lo;
        out->avail = r->import_hi - addr;
        if (r->smap && touch && !r->stouched) shadow_touch(r);
        ok = 1;
    } else if (r && r->kind == POOL_PHYS) {
        out->buffer = r->buf;
        out->offset = out->alloc_offset = addr - r->lo;
        out->avail = r->size - out->offset;
        out->alloc = r->alloc;
        out->cls = VK_MCLS_DEVICE;
        ok = 1;
    } else if (r && r->kind == POOL_VIRT) {
        uint64_t rel = addr - r->lo, k = rel / NVN_VPOOL_PAGE;
        if (k < r->npages && r->pt[k].phys) {
            PoolRec *ph = r->pt[k].phys;
            uint64_t off = r->pt[k].off + rel % NVN_VPOOL_PAGE, end = r->pt[k].off + NVN_VPOOL_PAGE;
            for (uint64_t j = k + 1; j < r->npages && r->pt[j].phys == ph && r->pt[j].off == end; j++) end += NVN_VPOOL_PAGE;
            out->buffer = ph->buf;
            out->offset = out->alloc_offset = off;
            out->avail = end - off;
            out->alloc = ph->alloc;
            out->cls = VK_MCLS_DEVICE;
            ok = out->avail >= size;
            if (ok && !ph->used) plat_xchg32(&ph->used, 1);   // read first: the flag is only ever set
        }
    }
    if (ok && !r->used) plat_xchg32(&r->used, 1);
    plat_mutex_unlock_shared(&pool_lock);
    if (!ok) memset(out, 0, sizeof *out);
    return ok;
}

uint64_t vk_mem_gen(void) {
    return __atomic_load_n(&pool_gen, __ATOMIC_ACQUIRE) + __atomic_load_n(&mirror_gen, __ATOMIC_ACQUIRE);
}

int vk_mem_class_of(uint64_t addr) {
    VkMemSpan m;
    return vk_mem_lookup(addr, 1, &m) ? m.cls : VK_MCLS_NONE;
}

// ---------------------------------------------------------------- census
static volatile int64_t traffic[VK_MCLS_N][VK_MUSE_N];

// Every caller is the replay (vk_exec*, under vk_sync_submit's lock), about ten adds per draw: one writer at a time, so
// a plain add, not a locked one. An add racing the census's reset may be lost.
void vk_mem_count(int cls, int use, uint64_t bytes) {
    if ((unsigned)cls < VK_MCLS_N && (unsigned)use < VK_MUSE_N)
        __atomic_store_n(&traffic[cls][use], __atomic_load_n(&traffic[cls][use], __ATOMIC_RELAXED) + (int64_t)bytes,
                         __ATOMIC_RELAXED);
}

static void flag_names(uint32_t f, char *buf, size_t n) {
    static const char *const names[] = {"CPU_NO_ACCESS", "CPU_UNCACHED", "CPU_CACHED", "GPU_NO_ACCESS", "GPU_UNCACHED",
                                        "GPU_CACHED", "SHADER_CODE", "COMPRESSIBLE", "PHYSICAL", "VIRTUAL"};
    size_t k = 0;
    buf[0] = 0;
    for (int i = 0; i < 10; i++)
        if (f >> i & 1) k += (size_t)snprintf(buf + k, k < n ? n - k : 0, "%s%s", k ? "|" : "", names[i]);
    if (!k) snprintf(buf, n, "none");
}

// One line per (kind, flags) group of the live pools, then the bytes the replay bound or copied per frame by pool
// class since the previous census (the first one: since frame 1200). Logged at frame 1800 (in the world from the
// run-game saves), at frame 600, and with NX2NAVI_TRACE=perf every 1800 frames.
static void dirty_frame(uint64_t frame);

static void mirror_log(uint64_t frame);
static void mirror_pool_log(uint64_t frame);
static void shadow_log(uint64_t frame);
static int shadow_check_on(void);
static int dead_pending(void);

static void vram_log(uint64_t frame);
#define HOLES_PERIOD 3600   // frames between two "mem: holes" lines (holes_log)
static void holes_log(uint64_t frame);
static void owner_log(uint64_t frame);   // the memory report, after the staging pool (vk_mem_give_back)
static void compact_look(void);
static void outside_watch(uint64_t frame);
static void up_ts_log(uint64_t frame);

static volatile uint64_t mirror_frame;   // the frame vk_mem_census last saw, for mirror_sync's log lines

// NX2NAVI_TRACE=perf: the 600-frame memory lines now, at a segment's end and once a scene has settled (perf.c).
void vk_mem_snapshot(uint64_t frame) {
    if (rt_trace & RT_TRACE_PERF) vram_log(frame);
}

void vk_mem_census(uint64_t frame) {
    mirror_frame = frame;
    __atomic_store_n(&span_frame, frame, __ATOMIC_RELAXED);
    if (dmabuf_on && frame % 60 == 0) span_gc(frame, 0);
    static uint64_t window_start;
    if ((rt_trace & RT_TRACE_PERF) && frame % 600 == 0) vram_log(frame), mirror_pool_log(frame);
    if (frame && frame % ((rt_trace & RT_TRACE_PERF) ? 600 : HOLES_PERIOD) == 0) holes_log(frame), owner_log(frame);
    if (vk_texture_compact_due()) compact_look();   // NX2NAVI_TEX_COMPACT: 0 at once with the setting off
    if ((rt_trace & RT_TRACE_PERF) && frame % 60 == 0) outside_watch(frame), up_ts_log(frame), vk_gtl_log(frame, 60);
    if ((rt_trace & RT_TRACE_PERF) && !vk_mirror_on) dirty_frame(frame);
    if ((rt_trace & RT_TRACE_PERF) ? frame % 60 == 0 : frame == 600 || frame == 1800)
        mirror_log(frame), shadow_log(frame), span_log(frame);   // mirror_log also logs the write-watch line
    else if (shadow_check_on() && frame % 60 == 0)
        shadow_log(frame);   // NX2NAVI_SHADOW_CHECK: the shadow lines at that rate without the perf trace
    if (frame == 1200) {   // loading is over: the first window starts here
        for (int c = 0; c < VK_MCLS_N; c++)
            for (int u = 0; u < VK_MUSE_N; u++) plat_xchg64(&traffic[c][u], 0);
        window_start = frame;
        return;
    }
    // Frame 600 as well: runs on slow machines end before 1800.
    if (frame != 600 && (frame % 1800 || !(frame == 1800 || (rt_trace & RT_TRACE_PERF)))) return;
    typedef struct { int kind; uint32_t flags; uint64_t n, bytes; } Group;
    Group g[64];
    int ng = 0;
    uint64_t total[4] = {0}, count[4] = {0};
    plat_mutex_lock_shared(&pool_lock);
    for (int i = 0; i < pool_index.n; i++) {
        const PoolRec *r = pool_index.e[i].buf;
        int j = 0;
        while (j < ng && (g[j].kind != r->kind || g[j].flags != r->flags)) j++;
        if (j == ng && ng < 64) g[ng++] = (Group){r->kind, r->flags, 0, 0};
        if (j < ng) g[j].n++, g[j].bytes += r->size;
        total[r->kind] += r->size, count[r->kind]++;
    }
    plat_mutex_unlock_shared(&pool_lock);
    rt_log("mem: census at frame %llu: import %llu pools %.1f MiB (%d without a GPU buffer), physical %llu pools %.1f MiB, "
           "virtual %llu pools %.1f MiB of address space",
           (unsigned long long)frame, (unsigned long long)count[POOL_IMPORT], total[POOL_IMPORT] / 1048576.0,
           n_import_failures, (unsigned long long)count[POOL_PHYS], total[POOL_PHYS] / 1048576.0,
           (unsigned long long)count[POOL_VIRT], total[POOL_VIRT] / 1048576.0);
    static const char *const kinds[] = {"", "import", "physical", "virtual"};
    for (int j = 0; j < ng; j++) {
        char f[160];
        flag_names(g[j].flags, f, sizeof f);
        rt_log("mem:   %s %#x %s: %llu pools, %.1f MiB", kinds[g[j].kind], g[j].flags, f, (unsigned long long)g[j].n,
               g[j].bytes / 1048576.0);
    }
    uint64_t frames = window_start && frame > window_start ? frame - window_start : frame;
    static const char *const cls[] = {"", "host CPU_NO_ACCESS", "host CPU_UNCACHED", "host CPU_CACHED", "device", "scratch"};
    rt_log("mem: traffic per frame over %llu frames, MiB (bound ranges, not bytes read): class: vertex index uniform "
           "storage indirect copy-src gpu-write cpu-read", (unsigned long long)frames);
    for (int c = 1; c < VK_MCLS_N; c++) {
        double v[VK_MUSE_N];
        for (int u = 0; u < VK_MUSE_N; u++) v[u] = plat_xchg64(&traffic[c][u], 0) / 1048576.0 / (double)(frames ? frames : 1);
        rt_log("mem:   %s: %.2f %.2f %.2f %.2f %.3f %.2f %.2f %.3f", cls[c], v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    }
    window_start = frame;
}

// NX2NAVI_VRAM_BUDGET_MB (test; unset or 0: the device's): the device-local budget every decision here and in
// vk_texture.c compares with is at most this many MiB, so a large card behaves as a small one does under pressure
// (mirrors refused or evicted, textures dropped). The heap lines still log what the driver reports.
static uint64_t budget_cap(void) {
    static int64_t mb = -1;
    if (mb < 0) {
        const char *s = cfg_get("NX2NAVI_VRAM_BUDGET_MB");
        int64_t v = s && *s ? atoll(s) : 0;
        if (v > 0) rt_log("mem: the VRAM budget is taken as at most %lld MiB (NX2NAVI_VRAM_BUDGET_MB, a test)", (long long)v);
        mb = v > 0 ? v : 0;
    }
    return (uint64_t)mb << 20;
}

// NX2NAVI_VRAM_BALLAST_MB (test; unset or 0: none): that much device-local memory is taken when the allocator is made
// and held until it goes, in buffers of BALLAST_PIECE of their own memory. NX2NAVI_VRAM_BUDGET_MB above changes only the
// number the decisions compare with; this fills the card for real, so the driver's own behaviour at its limit (slow
// allocations, memory moved to system RAM, refusals) shows on a large card as it does on an 8 GB one or beside another
// program's use.
#define BALLAST_PIECE (256ull << 20)
#define BALLAST_MAX 512
static VkBuffer ballast_buf[BALLAST_MAX];
static VmaAllocation ballast_alloc[BALLAST_MAX];
static int nballast;

void vk_mem_ballast(VmaAllocator vma) {   // vk_device.c, right after the allocator is made (before vk_ctx answers)
    const char *s = cfg_get("NX2NAVI_VRAM_BALLAST_MB");
    uint64_t want = s && *s && atoll(s) > 0 ? (uint64_t)atoll(s) << 20 : 0, got = 0;
    if (!want) return;
    while (got < want && nballast < BALLAST_MAX) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .size = want - got < BALLAST_PIECE ? want - got : BALLAST_PIECE,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                      .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
                                      .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
        if (vmaCreateBuffer(vma, &bi, &ai, &ballast_buf[nballast], &ballast_alloc[nballast], NULL) != VK_SUCCESS) break;
        got += bi.size, nballast++;
    }
    rt_log("mem: %llu of %llu MiB of video memory taken and held for the session (NX2NAVI_VRAM_BALLAST_MB, a test)",
           (unsigned long long)(got >> 20), (unsigned long long)(want >> 20));
}

void vk_mem_ballast_release(VmaAllocator vma) {   // before the allocator goes
    for (int i = 0; i < nballast; i++) vmaDestroyBuffer(vma, ballast_buf[i], ballast_alloc[i]);
    nballast = 0;
}

static int vram_query(uint64_t *used, uint64_t *budget, int *exact) {
    VkCtx *c = vk_ctx();
    *used = *budget = 0, *exact = 0;
    if (!c) return 0;
    const VkPhysicalDeviceMemoryProperties *mp = &c->mem_props;
    if (c->has_memory_budget) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &b};
        vkGetPhysicalDeviceMemoryProperties2(c->phys, &p2);
        for (uint32_t h = 0; h < p2.memoryProperties.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
            if (p2.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                *used += b.heapUsage[h], *budget += b.heapBudget[h];
        *exact = 1;
        return *budget != 0;
    }
    VmaBudget vb[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(c->vma, vb);
    for (uint32_t h = 0; h < mp->memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        if (mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            *used += vb[h].statistics.blockBytes, *budget += mp->memoryHeaps[h].size;
    return *budget != 0;
}

int vk_mem_vram(uint64_t *used, uint64_t *budget, int *exact) {
    int ok = vram_query(used, budget, exact);
    uint64_t cap = budget_cap();
    if (ok && cap && *budget > cap) *budget = cap;
    return ok;
}

// vk_mem_vram's use less the free space inside VMA's device-local blocks: the driver counts a block whole for as long
// as one allocation lives in it, and the next allocation of its type takes a hole first. The evictions measure with
// this: a texture or mirror freed in the middle of a block left the driver's figure where it was, so an eviction that
// waited for that figure to fall went on until nearly every idle texture and mirror was gone (16.2 GiB in use, 9.2 GiB
// of it free inside blocks after the evictions).
int vk_mem_vram_live(uint64_t *used, uint64_t *budget) {
    int exact;
    if (!vk_mem_vram(used, budget, &exact)) return 0;
    VkCtx *c = vk_ctx();
    VmaBudget vb[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(c->vma, vb);
    uint64_t slack = 0;
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        if ((c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            vb[h].statistics.blockBytes > vb[h].statistics.allocationBytes)
            slack += vb[h].statistics.blockBytes - vb[h].statistics.allocationBytes;
    *used = *used > slack ? *used - slack : 0;
    return 1;
}

// ---------------------------------------------------------------- page write generations
// Two-level table over the guest address space (below 2^39): 256 MiB chunks of 65,536 page generations, allocated
// when a page of the chunk is first stamped. 0 = never stamped.
#define GEN_CHUNK_SHIFT 28
#define GEN_PAGE_SHIFT 12
#define GEN_CHUNKS (RT_ADDRESS_LIMIT >> GEN_CHUNK_SHIFT)
#define GEN_PER_CHUNK (1u << (GEN_CHUNK_SHIFT - GEN_PAGE_SHIFT))
static uint32_t *gen_chunk[GEN_CHUNKS];
static uint32_t gen_now;                 // last generation handed out
static PlatMutex gen_lock = PLAT_MUTEX_INIT;
static uint64_t gen_pages[1024];
// The newest generation stamped on any page of each 2 MiB region (the write-watch snapshot regions below): the mirror
// sync skips regions with nothing newer than its pool's mirror.
#define REG_SHIFT 21
static uint32_t reg_max[RT_ADDRESS_LIMIT >> REG_SHIFT];
// The same two tables without the stamps of a mirror's own write-back (vk_gpu_written_back): what the CPU and the host
// paths wrote. The mirror sync reads these, so the bytes a mirror wrote back into guest memory are not copied up into it
// again at its next sync: the storage buffers the game's compute shaders write went down and came back up every frame,
// 20 MiB a frame over PCIe in Kokiri Forest (2026-11-09).
static uint32_t *cgen_chunk[GEN_CHUNKS];
static uint32_t creg_max[RT_ADDRESS_LIMIT >> REG_SHIFT];

// Every write-watch query of the generation table (gen_lock held exclusively): calls, bytes and time since the last
// mirror summary, all consumers together; the queries that found written pages (they also flush TLBs) apart.
static uint64_t ww_calls, ww_bytes, ww_ticks, ww_dirty_calls, ww_dirty_ticks, ww_pages;

// NX2NAVI_TRACE=perf: a write-watch query, freeze or its follow-up query that took over 4 ms gets a line (the first
// 200). A frame some ten frames after an area load took 51.6 ms of replay with 1.2 ms of it in the replay's ops; the
// replaying thread's samples sat in GetWriteWatch under vk_mem_mirror_begin and in the freeze's VirtualProtect, and no
// thread of the process could be sampled for 28 ms in between.
static void slow_mem_call(const char *what, uint64_t addr, uint64_t size, uint64_t pages, uint64_t ns) {
    static uint32_t logged;
    if (ns < 4000000ull || !(rt_trace & RT_TRACE_PERF) || __atomic_add_fetch(&logged, 1, __ATOMIC_RELAXED) > 200) return;
    rt_log("mem: slow %s: %.1f ms over %#llx + %.1f MiB, %lld pages written (frame %llu)", what, ns / 1e6,
           (unsigned long long)addr, size / 1048576.0, pages == UINT64_MAX ? -1ll : (long long)pages,
           (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED));
}

static uint64_t ww_query(uint64_t addr, uint64_t size) {
    uint64_t t0 = qpc();
    uint64_t n = mem_written_pages(addr, size, gen_pages, sizeof gen_pages / sizeof *gen_pages);
    uint64_t dt = qpc() - t0;
    ww_ticks += dt, ww_calls++, ww_bytes += size;
    if (n && n != UINT64_MAX) ww_dirty_calls++, ww_dirty_ticks += dt, ww_pages += n;
    slow_mem_call("write-watch query", addr, size, n, dt);
    return n;
}

// Frozen regions: a 2 MiB region whose pages nothing wrote for freeze_idle submissions is made read-only
// (memory.c mem_freeze), and write-watch queries skip it until a write or a file read thaws it. Most of the pool memory
// the mirror and the texture checks cover is static (meshes, textures), and a query costs about 1 us per MiB (4-5 on an
// 8th-gen i7), so this keeps the queries to the regions the CPU writes. The region need not lie wholly in pools: NVN
// pools are small and not 2 MiB aligned, so most regions also hold other guest memory, and a rule that refused those
// left 340 of 436 queried MiB a frame unfrozen in town although nothing had written them for 300 submissions. The
// other memory is safe to freeze for the same reason pools are: the only kernel writes into guest memory are file
// reads, which thaw their buffer first (fs_host.c, romfs.c), and every user-mode write faults and thaws.
#define FREEZE_PER_SUB 32
static int freeze_idle;                       // 0: off
static uint32_t ww_ep = 1;                    // gen_lock: the submission being recorded (vk_mem_mirror_begin)
static uint32_t reg_last[RT_ADDRESS_LIMIT >> REG_SHIFT];    // gen_lock: ww_ep of the region's last stamp
static uint32_t reg_tried[RT_ADDRESS_LIMIT >> REG_SHIFT];   // gen_lock: ww_ep of its last freeze candidacy
static uint32_t *fcand;                       // gen_lock: regions to try at the next freeze pass
static int nfcand, cfcand;
static uint64_t n_frz_refused;                // gen_lock: candidates mem_freeze refused

static uint32_t *slot_in(uint32_t **tab, uint64_t page_addr, int create) {
    uint64_t ci = page_addr >> GEN_CHUNK_SHIFT;
    if (ci >= GEN_CHUNKS) return NULL;
    if (!tab[ci]) {
        if (!create) return NULL;
        tab[ci] = calloc(GEN_PER_CHUNK, sizeof(uint32_t));
        if (!tab[ci]) rt_fatal("vk: out of memory for page generations");
    }
    return &tab[ci][(page_addr >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)];
}

static uint32_t *gen_slot(uint64_t page_addr, int create) { return slot_in(gen_chunk, page_addr, create); }

// gen_lock held exclusively; g is the newest generation handed out. back: a mirror's write-back, which leaves the
// CPU-side tables alone.
static void stamp_page_as(uint64_t a, uint32_t g, int back) {
    uint32_t *s = gen_slot(a, 1);
    if (!s) return;
    *s = g;
    reg_max[a >> REG_SHIFT] = g;
    reg_last[a >> REG_SHIFT] = ww_ep;
    if (back) return;
    uint32_t *c = slot_in(cgen_chunk, a, 1);
    if (c) *c = g, creg_max[a >> REG_SHIFT] = g;
}

static void stamp_page(uint64_t a, uint32_t g) { stamp_page_as(a, g, 0); }

static void stamp_range(uint64_t lo, uint64_t hi, uint32_t g) {
    for (uint64_t a = lo; a < hi; a += 1ull << GEN_PAGE_SHIFT) stamp_page(a, g);
}

void vk_guest_written(uint64_t addr, uint64_t size) {
    if (!size || nvn_gpu_only(addr)) return;
    uint64_t lo = addr & ~0xfffull, hi = (addr + size + 0xfff) & ~0xfffull;
    plat_mutex_lock(&gen_lock);
    stamp_range(lo, hi, ++gen_now);
    plat_mutex_unlock(&gen_lock);
}

int vk_mem_changed(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now) {
    if (!size) { *now = since; return 0; }
    uint64_t lo = addr & ~0xfffull, hi = (addr + size + 0xfff) & ~0xfffull;
    plat_mutex_lock(&gen_lock);
    uint32_t g = ++gen_now;
    int tracked = 1, frozen = 1;
    for (uint64_t a = lo & ~((1ull << REG_SHIFT) - 1); a < hi && frozen; a += 1ull << REG_SHIFT) frozen = mem_frozen(a);
    while (!frozen) {
        uint64_t n = ww_query(lo, hi - lo);
        if (n == UINT64_MAX) { tracked = 0; break; }
        for (uint64_t k = 0; k < n; k++) stamp_page(gen_pages[k], g);
        if (n < sizeof gen_pages / sizeof *gen_pages) break;   // a full answer may have left pages for the next call
    }
    int changed = 0;
    for (uint64_t a = lo; tracked && a < hi && !changed; a += 1ull << GEN_PAGE_SHIFT) {
        uint32_t *s = gen_slot(a, 0);
        if (s && *s > since) changed = 1;
    }
    plat_mutex_unlock(&gen_lock);
    *now = g;
    return tracked ? changed : -1;
}

// Write-watch snapshots per submission: the guest address space in 2 MiB regions, each queried at most once per epoch
// (a guest submission), at the first check that covers it. A query costs about 1.3 us plus 0.55 us per MiB, so one per
// region instead of one per texture. The snapshot only adds observations to the generation table: a page written
// before it carries a generation newer than any check before it, and a page written after it keeps its OS bit until
// the next query over it (at the latest the next epoch's snapshot).
#define SNAP_SHIFT 21
#define SNAP_REGIONS (RT_ADDRESS_LIMIT >> SNAP_SHIFT)
static uint32_t snap_at[SNAP_REGIONS];      // epoch of the region's last snapshot; 0 never
static uint8_t snap_untracked[SNAP_REGIONS];
static uint64_t n_snap_checks, n_snap_calls, n_snap_untracked;

// gen_lock held exclusively. Queries the regions r0..r1 that were not snapped in epoch, one query per run of them;
// returns the number of queries.
static uint64_t snap_regions(uint64_t r0, uint64_t r1, uint32_t epoch) {
    uint32_t g = 0;
    uint64_t calls = 0;
    for (uint64_t r = r0; r <= r1;) {
        if (snap_at[r] == epoch) { r++; continue; }
        if (mem_frozen(r << SNAP_SHIFT)) {   // nothing in it changed since its freeze
            snap_at[r] = epoch, snap_untracked[r] = 0;
            r++;
            continue;
        }
        uint64_t k = r;   // one query over a run of regions not seen in this epoch
        while (k + 1 <= r1 && snap_at[k + 1] != epoch && !mem_frozen((k + 1) << SNAP_SHIFT)) k++;
        uint64_t a = r << SNAP_SHIFT, b = (k + 1) << SNAP_SHIFT;
        int tracked = 1;
        if (!g) g = ++gen_now;
        for (;;) {
            uint64_t n = ww_query(a, b - a);
            if (n == UINT64_MAX) { tracked = 0; break; }
            for (uint64_t j = 0; j < n; j++) stamp_page(gen_pages[j], g);
            if (n < sizeof gen_pages / sizeof *gen_pages) break;
        }
        calls++;
        for (uint64_t j = r; j <= k; j++) {
            snap_at[j] = epoch, snap_untracked[j] = (uint8_t)!tracked;
            if (tracked && freeze_idle && ww_ep - reg_last[j] >= (uint32_t)freeze_idle &&
                ww_ep - reg_tried[j] >= (uint32_t)freeze_idle) {
                reg_tried[j] = ww_ep;
                if (nfcand == cfcand) grow((void **)&fcand, &cfcand, sizeof *fcand);
                fcand[nfcand++] = (uint32_t)j;
            }
        }
        r = k + 1;
    }
    return calls;
}

// NX2NAVI_SNAP_WIDEN=<regions> (8 with low_end_gpu = 1, else 0, off): a texture check that has to query widens the query to up to this
// many 2 MiB regions on each side, inside the import pool that holds the texture, so the textures of one pool are
// snapped by one query a submission instead of one each. With freezing off (NX2NAVI_FREEZE_IDLE) an RX 580 in Hyrule
// Field made about 1,130 texture queries a frame, about 6 MiB each on average incl. repeats, 9.4 ms with the pools'.
static int snap_widen_n(void) {
    static int n = -1;
    if (n < 0) {
        const char *e = cfg_get("NX2NAVI_SNAP_WIDEN");
        n = e && *e ? atoi(e) : 0;
        if (n < 0) n = 0;
        rt_log("mem: a texture check's write-watch query takes up to %d neighbouring 2 MiB regions of its pool on each "
               "side (NX2NAVI_SNAP_WIDEN)", n);
    }
    return n;
}

// r0..r1 widened inside the import pool that holds [lo, hi); pool_lock taken here (before gen_lock, as everywhere).
static void snap_widen(uint64_t lo, uint64_t hi, uint64_t *r0, uint64_t *r1) {
    int w = snap_widen_n();
    if (!w) return;
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)lo, hi - lo);
    const PoolRec *r = e ? e->buf : NULL;
    if (r && r->kind == POOL_IMPORT && r->buf && r->import_hi > r->import_lo) {
        uint64_t p0 = (r->import_lo + (1ull << SNAP_SHIFT) - 1) >> SNAP_SHIFT;   // whole regions of the pool only
        uint64_t p1 = r->import_hi >> SNAP_SHIFT;                                // [p0, p1)
        if (p1 > p0) {
            uint64_t a = *r0 > (uint64_t)w ? *r0 - (uint64_t)w : 0, b = *r1 + (uint64_t)w;
            if (a < p0) a = p0;
            if (b > p1 - 1) b = p1 - 1;
            if (a < *r0) *r0 = a;
            if (b > *r1 && b < SNAP_REGIONS) *r1 = b;
        }
    }
    plat_mutex_unlock_shared(&pool_lock);
}

int vk_mem_changed_snap(uint64_t addr, uint64_t size, uint32_t since, uint32_t epoch, uint32_t *now) {
    if (!size) { *now = since; return 0; }
    uint64_t lo = addr & ~0xfffull, hi = (addr + size + 0xfff) & ~0xfffull;
    uint64_t r0 = lo >> SNAP_SHIFT, r1 = (hi - 1) >> SNAP_SHIFT;
    if (!epoch || r1 >= SNAP_REGIONS) return vk_mem_changed(addr, size, since, now);
    int excl = 0;
    plat_mutex_lock_shared(&gen_lock);
    for (uint64_t r = r0; r <= r1; r++)
        if (snap_at[r] != epoch) { excl = 1; break; }
    if (excl) {
        plat_mutex_unlock_shared(&gen_lock);
        uint64_t w0 = r0, w1 = r1;
        snap_widen(lo, hi, &w0, &w1);
        plat_mutex_lock(&gen_lock);
        n_snap_calls += snap_regions(w0, w1, epoch);
    }
    int untracked = 0, changed = 0;
    for (uint64_t r = r0; r <= r1; r++) untracked |= snap_untracked[r];
    // Pages only in regions stamped after since (reg_max; REG_SHIFT is SNAP_SHIFT): an unchanged texture of 4 MiB was
    // 1,024 page reads, at about 1,100 texture checks a frame.
    for (uint64_t r = r0; !untracked && r <= r1 && !changed; r++) {
        if (reg_max[r] <= since) continue;
        uint64_t a = r << REG_SHIFT > lo ? r << REG_SHIFT : lo, b = (r + 1) << REG_SHIFT < hi ? (r + 1) << REG_SHIFT : hi;
        for (; a < b && !changed; a += 1ull << GEN_PAGE_SHIFT) {
            uint32_t *s = gen_slot(a, 0);
            if (s && *s > since) changed = 1;
        }
    }
    *now = gen_now;   // every observation so far is in the table
    __atomic_add_fetch(&n_snap_checks, 1, __ATOMIC_RELAXED);
    if (untracked) __atomic_add_fetch(&n_snap_untracked, 1, __ATOMIC_RELAXED);
    if (excl) plat_mutex_unlock(&gen_lock);
    else plat_mutex_unlock_shared(&gen_lock);
    return untracked ? -1 : changed;
}

void vk_mem_gen_lock_shared(void) { plat_mutex_lock_shared(&gen_lock); }
void vk_mem_gen_unlock_shared(void) { plat_mutex_unlock_shared(&gen_lock); }

void vk_mem_gen_lock(void) { plat_mutex_lock(&gen_lock); }
void vk_mem_gen_unlock(void) { plat_mutex_unlock(&gen_lock); }

int vk_mem_unchanged_why_locked(uint64_t addr, uint64_t size, uint32_t since, uint32_t epoch) {
    if (!size || !epoch) return 4;
    uint64_t lo = addr & ~0xfffull, hi = (addr + size + 0xfff) & ~0xfffull;
    uint64_t r0 = lo >> SNAP_SHIFT, r1 = (hi - 1) >> SNAP_SHIFT;
    if (r1 >= SNAP_REGIONS) return 4;
    for (uint64_t r = r0; r <= r1; r++) {   // REG_SHIFT is SNAP_SHIFT: reg_max is the newest stamp in the region
        if (snap_at[r] != epoch) return 1;
        if (snap_untracked[r]) return 2;
        if (reg_max[r] > since) return 3;
    }
    return 0;
}

int vk_mem_unchanged_locked(uint64_t addr, uint64_t size, uint32_t since, uint32_t epoch) {
    return !vk_mem_unchanged_why_locked(addr, size, since, epoch);
}

uint64_t vk_mem_snap_range_locked(uint64_t addr, uint64_t size, uint32_t epoch) {
    if (!size || !epoch) return 0;
    uint64_t lo = addr & ~0xfffull, hi = (addr + size + 0xfff) & ~0xfffull;
    uint64_t r0 = lo >> SNAP_SHIFT, r1 = (hi - 1) >> SNAP_SHIFT;
    if (r1 >= SNAP_REGIONS) return 0;
    for (uint64_t r = r0; r <= r1; r++)
        if (snap_at[r] != epoch) {
            uint64_t q = snap_regions(r0, r1, epoch);
            n_snap_calls += q;
            return q;
        }
    return 0;
}

// The snapshot of many ranges at once, for the texture pool's freshness check (vk_texture.c
// vk_texture_pool_fresh_locked): vk_mem_snap_note_locked notes the regions of a range that have no snapshot in epoch
// yet, and vk_mem_snap_noted_locked queries what was noted, one query per run of adjacent regions.
// Asked range by range (vk_mem_snap_range_locked), every such region got a query of its own, because a texture rarely
// spans two regions and the list is in no address order. While the player runs through a town the game writes about 32
// textures into new storage every four or five frames, a region the CPU wrote is not frozen again for freeze_idle
// submissions, and so 100 to 500 regions of the texture pools were queried one by one at every check, 2.7 checks a
// frame: the check took 0.5 to 1.5 ms against 0.11 ms standing still (5 to 10 queries), about 3 us a query (five
// sessions of one player, RTX 5090, 2026-11-12). About half of a 2 MiB query is the call itself: in a process of its
// own, 400 of them took 727 us and one query over the same 800 MiB 360 us, with 0, 8 or 64 pages written.
// The regions queried and the pages stamped are the same, and every one is stamped before any entry of the list is
// looked at, as before: each entry's regions were queried ahead of its own look. A run is asked in one query only where
// it lies inside one watched reservation (mem_watched): a query across the edge of one fails as a whole, so the regions
// of such a run are left to the queries by range, and *left says that there were some.
static uint32_t snap_noted[SNAP_REGIONS];   // gen_lock: snap_pass when the region is in snap_want
static uint32_t snap_pass = 1;
static uint32_t *snap_want;                 // gen_lock: the regions noted since the last vk_mem_snap_noted_locked
static int nsnap_want, csnap_want;

void vk_mem_snap_note_locked(uint64_t addr, uint64_t size, uint32_t epoch) {
    if (!size || !epoch) return;
    uint64_t lo = addr & ~0xfffull, hi = (addr + size + 0xfff) & ~0xfffull;
    uint64_t r0 = lo >> SNAP_SHIFT, r1 = (hi - 1) >> SNAP_SHIFT;
    if (r1 >= SNAP_REGIONS) return;
    for (uint64_t r = r0; r <= r1; r++) {
        if (snap_at[r] == epoch || snap_noted[r] == snap_pass) continue;
        snap_noted[r] = snap_pass;
        if (nsnap_want == csnap_want) grow((void **)&snap_want, &csnap_want, sizeof *snap_want);
        snap_want[nsnap_want++] = (uint32_t)r;
    }
}

static int snap_want_cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

uint64_t vk_mem_snap_noted_locked(uint32_t epoch, int *left) {
    uint64_t calls = 0;
    *left = 0;
    if (nsnap_want > 1) qsort(snap_want, (size_t)nsnap_want, sizeof *snap_want, snap_want_cmp);
    for (int i = 0, k; i < nsnap_want; i = k + 1) {
        for (k = i; k + 1 < nsnap_want && snap_want[k + 1] == snap_want[k] + 1;) k++;
        uint64_t r0 = snap_want[i], r1 = snap_want[k];
        if (r1 > r0 && !mem_watched(r0 << SNAP_SHIFT, (r1 - r0 + 1) << SNAP_SHIFT)) *left = 1;
        else calls += snap_regions(r0, r1, epoch);
    }
    nsnap_want = 0;
    if (!++snap_pass) memset(snap_noted, 0, sizeof snap_noted), snap_pass = 1;
    n_snap_calls += calls;
    return calls;
}

void vk_mem_snap_stats(uint64_t *checks, uint64_t *calls, uint64_t *untracked) {
    *checks = __atomic_exchange_n(&n_snap_checks, 0, __ATOMIC_RELAXED);
    *calls = __atomic_exchange_n(&n_snap_calls, 0, __ATOMIC_RELAXED);
    *untracked = __atomic_exchange_n(&n_snap_untracked, 0, __ATOMIC_RELAXED);
}

// ---------------------------------------------------------------- dirty census (NX2NAVI_TRACE=perf)
// What a device-local mirror of the buffer-bound pools would upload. The replay notes every
// imported pool it binds as a vertex, index, uniform, storage or indirect buffer (vk_mem_bound_note). At the start of
// each submit one write-watch query per run of such pools stamps the pages written since with a new generation, in the
// shared generation table (no second consumer of the OS bits: texture checks see the stamps), and the pages of those
// pools are counted:
// - eager: pages stamped after the previous submit's scan, in every buffer-bound pool (upload at every submit);
// - lazy: pages stamped after the pool's previous count, in pools the previous submit bound (upload when bound);
// - first use: the size of a pool the first time it is bound (a mirror copies the whole pool then).
// Classes: CPU_CACHED, CPU_UNCACHED, other (CPU_NO_ACCESS). Logged as `mem: dirty` every 60 frames.
#define DIRTY_UNSYNCED UINT32_MAX
#define DIRTY_GAP 0x100000ull   // pools closer than this share one query (the gap's pages are stamped too)
#define DIRTY_WIN 60
enum { DC_CACHED, DC_UNC, DC_OTHER, DC_N };
typedef struct {
    uint64_t eager[DC_N], lazy[DC_N], first[DC_N];
    uint64_t submits, queries, scan_ns, sub_max, frames;
} DirtyAcc;
static uint32_t dirty_sub = 1, dirty_gen;   // submit lock: the current submit number, generation of the last scan
static uint64_t first_pending[DC_N], ever_n[DC_N], ever_bytes[DC_N];   // submit lock
static PoolRec **dlist;                     // submit lock: the buffer-bound pools of a scan
static int dcap;
static PlatMutex dirty_lock = PLAT_MUTEX_INIT;   // everything below
static DirtyAcc dfr, dwin;                  // this frame, this window
static uint32_t dwin_kib[DIRTY_WIN][DC_N];  // eager KiB of each frame of the window
static uint64_t live_n[DC_N], live_bytes[DC_N], live_untracked, ever_snap_n[DC_N], ever_snap_bytes[DC_N];

static int dc_class(const PoolRec *r) {
    int c = rec_class(r);
    return c == VK_MCLS_HOST_CACHED ? DC_CACHED : c == VK_MCLS_HOST_UNC ? DC_UNC : DC_OTHER;
}

void vk_mem_bound_note(void *pool) {
    PoolRec *r = pool;
    if (r->bound_sub == dirty_sub) return;
    r->bound_sub = dirty_sub;
    if (r->bufbound) return;
    r->bufbound = 1, r->synced_gen = DIRTY_UNSYNCED;
    int k = dc_class(r);
    ever_n[k]++, ever_bytes[k] += r->size, first_pending[k] += r->size;
}

static uint64_t page_lo(const PoolRec *r) { return r->lo & ~0xfffull; }
static uint64_t page_hi(const PoolRec *r) { return (r->lo + r->size + 0xfff) & ~0xfffull; }

// gen_lock held exclusively. 0: the range is not write-watched.
static int query_stamp(uint64_t lo, uint64_t hi, uint32_t g) {
    for (;;) {
        uint64_t n = ww_query(lo, hi - lo);
        if (n == UINT64_MAX) return 0;
        for (uint64_t k = 0; k < n; k++) stamp_page(gen_pages[k], g);
        if (n < sizeof gen_pages / sizeof *gen_pages) return 1;
    }
}

// ---------------------------------------------------------------- shadow storage
// Where importing guest memory costs too much (Linux: re-protecting pages that amdgpu imported as a userptr
// invalidates the buffer, about 1.3 ms per 16 MiB at the next submit, and the import's write pinning clears the
// write watch), a CPU-visible pool gets a host-visible buffer of its own instead, at the offsets the import would have:
// - CPU -> shadow: right before each submit (vk_mem_shadow_sync), the pools the replay resolved since the last one are
//   queried for written pages; pages stamped after a pool's sgen are copied in by the CPU.
// - The device address table (vk_gpa.c) names the shadows, and its readers reach pools the replay never resolves: the
//   instanced foliage's and Navi's data, through pointers read from other buffers. Without a sync those shadows held
//   what guest memory held when they were made, and Kokiri Forest's trees were bare trunks, without ferns or grass
//   tufts. So a guest submission whose record binds the table (vk_mem_shadow_table_read) syncs every shadow in the
//   table, once, before the first part recorded after the binding is submitted (vk_mem_shadow_sync), and those that
//   have a VRAM mirror, which the table then names, queue the same pages for upload. Once is enough: the copy-ins read
//   the guest submission's write-watch snapshots (snap_regions), which a second sync in the same guest submission
//   would find unchanged. A sync in every part that used the table ran about three times a frame outdoors. Measured
//   on Linux (RTX 4090, Kokiri Forest, by the change's author): a sync at every guest submission, about six a
//   frame, took 9.8 ms and 888 write-watch queries a frame; one in the guest submissions that read the table 3.9 ms
//   and 92.
// - GPU -> guest: the replay notes each GPU write into CPU-visible memory at record time (vk_mem_gpu_pending) and
//   reports it at the fence (vk_gpu_written), which copies the GPU's bytes back into guest memory.
// - Pages both write: a GPU write is noted with the whole range its binding names, and the CPU writes other fields of the
//   same records meanwhile. Each page such a range covers keeps a base copy from its first note on, the bytes guest
//   memory and the shadow last agreed on. A copy-in takes only the bytes where guest memory differs from the base (the
//   CPU's writes) and the write-back at the fence only those where the shadow differs from it (the GPU's), and either
//   moves the base along (merge_page). A byte both changed takes the CPU's value when a copy-in comes first and the
//   GPU's when the fence does. Whole-range copies both ways lost the CPU's bytes: Ocarina writes a lighting parameter
//   into a buffer whose binding its compute passes write every frame, the GPU only ever read zero there, the lighting
//   coefficients and then the volumetric fog came out NaN, and the fog pass drew the 3D scene black.
// - The game must not get ahead of the write-backs. With an import the GPU's bytes are in guest memory when the GPU
//   has stored them; here they get there in a completion callback, after the fence. A CPU write that should follow
//   them and lands before that callback is overtaken: the write-back puts the GPU's older bytes over it and moves the
//   base copy, and a rewrite with the base copy's value (a reset to zero) is no difference at any time. So the wait
//   after a frame's first submission covers the write-backs with shadow storage (vk_sync.c "the executed timeline",
//   which tells what the game rewrites there and what a frame looked like without it).
// - Pages a shader stores into through the device address table (vk_gpa.c): the store goes to the pool's VRAM mirror
//   and to its twin, which here is the shadow, not guest memory. No binding names the range, so nothing notes it
//   (vk_mem_gpu_pending): the page has no base copy and guest memory never gets the bytes. The game's culling passes
//   store counts and records this way into pages whose other words the CPU rewrites. For one such page P, a word X the
//   shaders store and a word Y the CPU writes:
//     guest submission A (with the pass that stores): the CPU wrote Y, so A's sync copies P in whole (shadow_copy)
//       and queues it for the mirror (mirror_sync). X in the shadow and in the mirror is now guest memory's X, which
//       no store ever reached. A runs and stores X = v into both.
//     guest submission B (a later one that reads X through the table), the CPU has not written P since A's
//       write-watch snapshot:
//       B's sync leaves P alone and its draws read v.
//     B, the CPU has written Y again between the two snapshots: B's sync copies P in whole and uploads it, X
//       is guest memory's stale word again in both copies, and the draws read that.
//   An import does not have the fault: its twin is guest memory, and a page copied up from there holds the store.
//   Measured (NX2NAVI_SHADOW_CHECK, a standing run outdoors, RTX 5090 with NX2NAVI_SHADOW=1, 2026-11-10): the stores
//   go into one page each of ten pools of 20 and 44 KiB, and whole copy-ins overwrite one exactly four times a frame
//   (240 in every 60 frames): A's case, the copy that precedes the frame's stores, which does no harm. B's second
//   case would add to the four and was not seen in 1,800 frames; nothing proves it cannot happen where the frame's
//   timing differs. (Counted when every part that used the table synced; a guest submission syncs once now, from the
//   same write-watch snapshots, and the counts were not taken again.)
//   NX2NAVI_SHADOW_TABLE_STORES=1 keeps the stores. The pages are found by their content: each whole copy-in leaves a
//   hash of the page it wrote (smark), and the next one hashes the shadow page first. A different hash means something
//   stored into the shadow in between, and with no base copy on the page that was a store through the table. That
//   copy-in still overwrites the store (nothing tells its bytes from the CPU's), and from then on the page keeps a
//   base copy: copy-ins take the CPU's bytes only, and the fence of every submission whose record bound the table
//   writes the stored bytes of all such pages back to guest memory (table_wb_cb), as the fence of a noted write does.
//   Guest memory needs them: a word the CPU resets each frame to the value it had before the stores would otherwise
//   never differ from its base, and the reset would never reach the GPU.
//   It is off by default, for three reasons. The fault it removes was not seen. Its hashes cost 1.8 ms a frame of
//   the replay there (copy_marked). And it puts these pages under the rule of the bullet above: a count the CPU
//   resets before that write-back has landed keeps counting from its old value, where the whole copy-in of today
//   resets it every time; whether the game resets any of these words from the CPU is not known.
// NX2NAVI_SHADOW_CHECK=1 counts, with the shadow line and at its rate every 60 frames (shadow_table_log): the whole
// copy-ins that overwrote a store through the table, per pool; the write-backs that moved bytes into a page the CPU
// had written since its copy-in (cpu_wrote_since); the bytes both sides changed since their base copy (taken from the
// CPU at a copy-in, from the GPU at a fence); what the copy-ins cost by kind.
// NX2NAVI_SHADOW=1/0 forces the mode on or off; the default is on for Linux and Wine, except for NVIDIA's own driver on
// Linux, where imports ran about twice as fast on 0.7.1 (mirror_mode).
#define SB_PAGE 4096ull
static PlatMutex shadow_lock = PLAT_MUTEX_INIT;   // after pool_lock; before gen_lock
static PoolRec **stouch;                          // shadow_lock: pools resolved since the last sync
static int nstouch, cstouch;
static uint64_t s_sb_pages;                       // shadow_lock: base copies kept (sbase_make)
static int64_t s_live_n, s_live_bytes;            // shadow_lock
static uint64_t s_syncs, s_copied, s_queries, s_untracked, s_wb_bytes, s_ticks;   // shadow_lock: since the last log
static uint64_t s_wb_seen, s_wb_ticks;            // shadow_lock: bytes the fence write-backs compared, their time
static uint64_t s_q_ticks;                        // shadow_lock: the copy-ins' write-watch queries
static int wb_no_reup(void);
// Pages a shader stores into through the device address table. The two switches are read in mirror_mode.
// s_learn (NX2NAVI_SHADOW_TABLE_STORES: such pages get a base copy and a write-back) and s_check (NX2NAVI_SHADOW_CHECK:
// the counts below marked "check") are defined with vk_mem_dmabuf_probe, which may turn shadow storage on late.
static PoolRec **tpools;                          // shadow_lock: the pools that have such pages
static int ntpools, ctpools;
static uint64_t s_tab_pages;                      // shadow_lock: such pages, all pools
// shadow_lock, since the last log:
static uint64_t s_tab_over;                       // whole copy-ins that overwrote a store through the table
static uint64_t s_tab_wb, s_tab_wb_seen, s_tab_wb_ticks, s_tab_wb_jobs;   // table_wb_cb: bytes moved and compared, time, runs
static uint64_t s_marked, s_mark_ticks;           // pages copied in whole with a mark; check: the time of those copies
static uint64_t s_plain_ticks;                    // check: the time of the whole copies made without marks
static uint64_t s_both_cpu, s_both_gpu;           // check: bytes both sides changed, taken from the CPU / from the GPU
static uint64_t s_in_merge, s_in_nomirror;        // check: bytes copied in by merge_page; whole, into pools without a mirror
static uint64_t s_tab_ticks, s_tab_syncs;         // the sync for the table's readers: its time (part of s_ticks), its runs
static PoolRec **stab;                            // shadow_lock: the shadows in the device address table (PoolRec.stab_i)
static int nstab, cstab;
static uint32_t stab_want;                        // the last guest submission whose record bound the table
static uint64_t s_t_over_all;                     // shadow_lock: s_tab_over since the start
static uint64_t s_late_pages, s_late_bytes;       // shadow_lock, check: write-backs into pages the CPU wrote since their copy-in
static uint64_t s_stale_pages;                    // shadow_lock: write-backs left out, the shadow could not hold them (shadow_write_back)

static int shadow_pool(VkCtx *c, PoolRec *r, uint64_t addr, uint64_t size) {
    r->import_lo = addr & ~0xfffull;
    r->import_hi = (addr + size + 0xfff) & ~0xfffull;
    uint64_t n = r->import_hi - r->import_lo;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = pool_usage(c),
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    // Cached where the device has it: the fence copies read it back on the CPU.
    VmaAllocationCreateInfo ai = {.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  .preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT};
    VmaAllocationInfo info;
    VkResult res = vmaCreateBuffer(c->vma, &bi, &ai, &r->buf, &r->salloc, &info);
    if (res != VK_SUCCESS) {
        rt_log("vk: pool %#llx+%#llx: shadow allocation failed (%d)", (unsigned long long)addr, (unsigned long long)size,
               (int)res);
        r->buf = VK_NULL_HANDLE, r->salloc = NULL;
        return 0;
    }
    r->smap = info.pMappedData;
    r->sborn = vk_sync_issued() + 1;   // the submission being recorded now, or the next one
    plat_mutex_lock(&gen_lock);
    r->sgen = gen_now;   // the copy below holds every write stamped so far; later stamps are copied again
    plat_mutex_unlock(&gen_lock);
    memcpy(r->smap, (const void *)(uintptr_t)r->import_lo, n);
    plat_mutex_lock(&shadow_lock);
    s_live_n++, s_live_bytes += (int64_t)n;
    plat_mutex_unlock(&shadow_lock);
    plat_inc32(&n_imports);
    return 1;
}

static void shadow_touch(PoolRec *r) {
    plat_mutex_lock(&shadow_lock);
    if (!r->stouched) {
        if (nstouch == cstouch) grow((void **)&stouch, &cstouch, sizeof *stouch);
        stouch[nstouch++] = r;
        r->stouched = 1;
    }
    plat_mutex_unlock(&shadow_lock);
}

// ---- base copies of the pages both write ("Pages both write" above). shadow_lock held throughout.
static uint32_t sbase_hash(uint64_t pg) { return (uint32_t)((pg >> 12) * 0x9E3779B97F4A7C15ull >> 32); }

// The base copy of guest page pg of r, NULL when it has none.
static uint8_t *sbase_find(const PoolRec *r, uint64_t pg) {
    if (!r->nsb) return NULL;
    for (uint32_t i = sbase_hash(pg) & (r->csb - 1);; i = (i + 1) & (r->csb - 1)) {
        if (r->sb[i].page == pg) return r->sb[i].base;
        if (!r->sb[i].page) return NULL;
    }
}

static void sbase_insert(struct SBase *t, uint32_t cap, uint64_t pg, uint8_t *base) {
    uint32_t i = sbase_hash(pg) & (cap - 1);
    while (t[i].page) i = (i + 1) & (cap - 1);
    t[i].page = pg, t[i].base = base;
}

// Page pg of r gets a base copy unless it has one. Taken from the shadow: no GPU write was noted on the page before, so
// the shadow holds what guest memory held at its last copy-in, and guest memory differs from it by the CPU's writes since.
static void sbase_make(PoolRec *r, uint64_t pg) {
    if (sbase_find(r, pg)) return;
    if ((r->nsb + 1) * 4 > r->csb * 3) {   // at most three quarters full
        uint32_t cap = r->csb ? r->csb * 2 : 16;
        struct SBase *t = calloc(cap, sizeof *t);
        if (!t) rt_fatal("vk: out of memory for the shadow's base copies");
        for (uint32_t i = 0; i < r->csb; i++)
            if (r->sb[i].page) sbase_insert(t, cap, r->sb[i].page, r->sb[i].base);
        free(r->sb);
        r->sb = t, r->csb = cap;
    }
    uint8_t *b = malloc(SB_PAGE);
    if (!b) rt_fatal("vk: out of memory for the shadow's base copies");
    memcpy(b, r->smap + (pg - r->import_lo), SB_PAGE);
    sbase_insert(r->sb, r->csb, pg, b);
    r->nsb++;
    s_sb_pages++;
}

static void sbase_free(PoolRec *r) {
    for (uint32_t i = 0; i < r->csb; i++) free(r->sb[i].base);
    free(r->sb);
    s_sb_pages -= r->nsb;
    r->sb = NULL, r->nsb = r->csb = 0;
}

// 0xff in each byte of x that is not zero, 0 in the others.
static inline uint64_t byte_mask(uint64_t x) {
    x |= x >> 4, x |= x >> 2, x |= x >> 1;
    return (x & 0x0101010101010101ull) * 0xff;
}

// The bytes of v that m selects into dst (little-endian), the other bytes of dst left as they are: the other side may
// be writing them at the same time.
static inline void store_bytes(uint8_t *dst, uint64_t v, uint64_t m) {
    if (m == ~0ull) {
        memcpy(dst, &v, 8);
        return;
    }
    for (int k = 0; k < 8; k++)
        if (m >> (8 * k) & 0xff) dst[k] = (uint8_t)(v >> (8 * k));
}

// n bytes of one page with base copy b: the bytes where src differs from the base go into dst and into the base. src
// and dst are guest memory and the shadow, one way or the other; each src byte that moves is read once, as its side
// may be writing it. Returns the bytes moved.
static uint64_t merge_page(uint8_t *dst, const uint8_t *src, uint8_t *b, uint64_t n) {
    uint64_t moved = 0, i = 0;
    for (; i < n && ((uintptr_t)(src + i) & 7); i++) {
        uint8_t v = ((const volatile uint8_t *)src)[i];
        if (v != b[i]) dst[i] = b[i] = v, moved++;
    }
    for (; i + 8 <= n; i += 8) {
        // Most of a page is unchanged: a 64-byte block equal to its base is passed over with one vector compare. A
        // byte its side writes after the compare is a later write, which the next copy-in or write-back takes.
        if (!((uintptr_t)(src + i) & 63) && i + 64 <= n) {
            uint64_t x = 0;
            for (int k = 0; k < 64; k += 8) {
                uint64_t sv, ov;
                memcpy(&sv, src + i + k, 8), memcpy(&ov, b + i + k, 8);
                x |= sv ^ ov;
            }
            if (!x) {
                i += 56;
                continue;
            }
        }
        uint64_t s = *(const volatile uint64_t *)(src + i), o;
        memcpy(&o, b + i, 8);
        uint64_t m = byte_mask(s ^ o);
        if (!m) continue;
        store_bytes(dst + i, s, m);
        memcpy(b + i, &s, 8);   // the bytes m leaves out are equal in both
        moved += (uint64_t)__builtin_popcountll(m) / 8;
    }
    for (; i < n; i++) {
        uint8_t v = ((const volatile uint8_t *)src)[i];
        if (v != b[i]) dst[i] = b[i] = v, moved++;
    }
    return moved;
}

// ---- pages a shader stores into through the device address table ("shadow storage" above). shadow_lock held.

// The mark of a 4 KiB page: a hash of its bytes, never 0. Four sums that do not wait for each other.
static uint32_t page_mark(const uint8_t *p) {
    uint64_t h[4] = {0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull};
    for (uint64_t i = 0; i < SB_PAGE; i += 32) {
        uint64_t w[4];
        memcpy(w, p + i, 32);
        for (int k = 0; k < 4; k++) {
            h[k] = (h[k] ^ w[k]) * 0xFF51AFD7ED558CCDull;
            h[k] = h[k] << 29 | h[k] >> 35;
        }
    }
    uint64_t x = (h[0] ^ h[1] << 13 ^ h[2] << 27 ^ h[3] << 41 ^ h[1] >> 51 ^ h[2] >> 37 ^ h[3] >> 23) * 0xC4CEB9FE1A85EC53ull;
    uint32_t m = (uint32_t)(x >> 32);
    return m ? m : 1;
}

// Guest page pg of r was stored into through the table: it keeps a base copy from here on, taken from the shadow as
// the whole copy-in just left it (guest memory's bytes), and table_wb_cb writes its stores back.
static void table_page_add(PoolRec *r, uint64_t pg) {
    sbase_make(r, pg);
    if (r->ntpg == r->ctpg) {
        uint32_t cap = r->ctpg ? r->ctpg * 2 : 16;
        uint64_t *t = realloc(r->tpg, cap * sizeof *t);
        if (!t) rt_fatal("vk: out of memory for the shadow's table pages");
        r->tpg = t, r->ctpg = cap;
    }
    r->tpg[r->ntpg++] = pg;
    s_tab_pages++;
    if (r->tlisted) return;
    if (ntpools == ctpools) grow((void **)&tpools, &ctpools, sizeof *tpools);
    tpools[ntpools++] = r;
    r->tlisted = 1;
}

// NX2NAVI_SHADOW_CHECK: the bytes of [0, n) that both sides changed since the base copy b and that now differ, which a
// merge of src into dst is about to settle in src's favour.
static uint64_t both_changed(const uint8_t *dst, const uint8_t *src, const uint8_t *b, uint64_t n) {
    uint64_t k = 0;
    for (uint64_t i = 0; i < n; i++) k += (src[i] != b[i]) & (dst[i] != b[i]) & (dst[i] != src[i]);
    return k;
}

// Copies the whole guest pages [a, e) of r into its shadow, none of which has a base copy. Each page's mark tells
// whether the shadow page still holds what the last such copy left: if not, a shader stored into it through the table
// meanwhile, and this copy overwrites that store ("Pages a shader stores into" above says why it cannot be kept, and
// what the page gets from here on).
// Only with NX2NAVI_SHADOW_TABLE_STORES or NX2NAVI_SHADOW_CHECK (shadow_copy). A mark is two hashes of the page, one
// of them of bytes the copy would not have read: outdoors, where 2,400 pages a frame are copied in whole, the marks
// cost 1.8 ms a frame of the replay (3.09 against 1.32 ms of copy-ins, 67 against 75 fps, RTX 5090 with
// NX2NAVI_SHADOW=1, 2026-11-10).
static void copy_marked(PoolRec *r, uint64_t a, uint64_t e) {
    if (!r->smark) {
        r->smark = calloc((r->import_hi - r->import_lo) / SB_PAGE, sizeof *r->smark);
        if (!r->smark) rt_fatal("vk: out of memory for the shadow's page marks");
    }
    for (uint64_t pg = a; pg < e; pg += SB_PAGE) {
        uint8_t *s = r->smap + (pg - r->import_lo);
        uint32_t *m = &r->smark[(pg - r->import_lo) / SB_PAGE];
        int stored = *m && page_mark(s) != *m;
        memcpy(s, (const void *)(uintptr_t)pg, SB_PAGE);
        if (stored) {
            s_tab_over++, s_t_over_all++, r->t_over++;
            if (s_learn) {   // merged from now on: its mark is not read again
                table_page_add(r, pg);
                continue;
            }
        }
        *m = page_mark(s);   // of the shadow, not of guest memory, which the CPU may be writing
    }
    s_marked += (e - a) / SB_PAGE;
}

// vk_pool_fini, after the pool left the index: off the touched list before its record goes.
static void shadow_forget(PoolRec *r) {
    plat_mutex_lock(&shadow_lock);
    for (int i = 0; i < nstouch; i++)
        if (stouch[i] == r) { stouch[i] = stouch[--nstouch]; break; }
    for (int i = 0; r->tlisted && i < ntpools; i++)
        if (tpools[i] == r) { tpools[i] = tpools[--ntpools]; break; }
    if (r->stab_i) {   // the last one takes its place
        PoolRec *last = stab[--nstab];
        stab[r->stab_i - 1] = last, last->stab_i = r->stab_i;
        r->stab_i = 0;
    }
    s_live_n--, s_live_bytes -= (int64_t)(r->import_hi - r->import_lo);
    s_tab_pages -= r->ntpg;
    free(r->tpg), free(r->smark);
    r->tpg = NULL, r->smark = NULL, r->ntpg = r->ctpg = 0, r->tlisted = 0;
    sbase_free(r);
    plat_mutex_unlock(&shadow_lock);
}

// shadow_lock held: copies guest [a, b) into the shadow; of a page with a base copy only the CPU's bytes (merge_page).
static void shadow_copy(PoolRec *r, uint64_t a, uint64_t b) {
    while (a < b) {
        uint64_t e = b;
        uint8_t *base = NULL;
        if (r->nsb) {   // the plain run up to the next page with a base copy
            for (e = a; e < b; e = (e & ~(SB_PAGE - 1)) + SB_PAGE)
                if ((base = sbase_find(r, e & ~(SB_PAGE - 1)))) break;
            if (e > b) e = b;
            if (e == a) {   // a starts on such a page: its part of [a, b)
                uint64_t pg = a & ~(SB_PAGE - 1), pe = pg + SB_PAGE < b ? pg + SB_PAGE : b;
                uint8_t *s = r->smap + (a - r->import_lo);
                if (s_check) s_both_cpu += both_changed(s, (const uint8_t *)(uintptr_t)a, base + (a - pg), pe - a);
                uint64_t n = merge_page(s, (const uint8_t *)(uintptr_t)a, base + (a - pg), pe - a);
                s_copied += n, s_in_merge += n;
                a = pe;
                continue;
            }
        }
        // The copies are of whole pages (shadow_sync_pool); the marks need the device address table, whose stores
        // they find, and one of the two switches.
        uint64_t t0 = s_check ? qpc() : 0;
        int marks = (s_learn || s_check) && r->gpa_rec && !((a | e) & (SB_PAGE - 1));
        if (marks) copy_marked(r, a, e);
        else memcpy(r->smap + (a - r->import_lo), (const void *)(uintptr_t)a, e - a);
        if (s_check) {
            *(marks ? &s_mark_ticks : &s_plain_ticks) += qpc() - t0;
            if (r->mstate != MS_ON) s_in_nomirror += e - a;
            r->s_in += e - a;
        }
        s_copied += e - a;
        a = e;
    }
}

// shadow_lock and gen_lock held: stamps the pool's written pages and copies those newer than its shadow. The pool's
// regions are queried through the write-watch snapshots of the guest submission (snap_regions, epoch), which the
// texture checks and the mirror syncs share: each 2 MiB region at most once per guest submission. A query of every
// touched pool at every part's submit was about 380 write-watch calls and 10 ms a frame of the submitting thread in
// Kokiri Forest, while the GPU waited for the next part (RTX 4090, 2026-11-09). A page the CPU writes after its
// region's snapshot goes in at the next guest submission's, as it does into a VRAM mirror.
// cpu (wb_no_reup): the pages are read from the tables without the stamps of a mirror's write-back (stamp_page_as),
// since that write-back went through the shadow, which holds those bytes already; with the other tables every storage
// buffer the game's compute shaders wrote was compared again at the next copy-in.
static void shadow_sync_pool(PoolRec *r, uint32_t epoch, int cpu) {
    uint64_t lo = r->import_lo, hi = r->import_hi;
    uint64_t r0 = lo >> SNAP_SHIFT, r1 = (hi - 1) >> SNAP_SHIFT;
    int tracked = 1;
    uint64_t t0 = qpc();
    if (epoch && r1 < SNAP_REGIONS) {
        s_queries += snap_regions(r0, r1, epoch);
        for (uint64_t k = r0; k <= r1 && tracked; k++) tracked = !snap_untracked[k];
    } else {   // outside the snapshot regions, or before the first guest submission: one query per unfrozen run
        uint32_t g = ++gen_now;
        for (uint64_t a = lo; a < hi;) {
            uint64_t b = (a | ((1ull << REG_SHIFT) - 1)) + 1;
            if (b > hi) b = hi;
            if (mem_frozen(a)) { a = b; continue; }
            while (b < hi && !mem_frozen(b)) b = b + (1ull << REG_SHIFT) < hi ? b + (1ull << REG_SHIFT) : hi;
            s_queries++;
            if (!query_stamp(a, b, g)) tracked = 0;
            a = b;
        }
    }
    s_q_ticks += qpc() - t0;
    uint32_t since = r->sgen;
    r->sgen = gen_now;
    if (!tracked) {   // nothing tells which pages changed: all of them
        s_untracked++;
        shadow_copy(r, lo, hi);
        return;
    }
    uint64_t run = 0;
    for (uint64_t k = lo >> REG_SHIFT; k <= (hi - 1) >> REG_SHIFT; k++) {
        uint64_t a = k << REG_SHIFT, b = a + (1ull << REG_SHIFT);
        if (a < lo) a = lo;
        if (b > hi) b = hi;
        const uint32_t *ch = (cpu ? cgen_chunk : gen_chunk)[a >> GEN_CHUNK_SHIFT];   // a 2 MiB region lies in one chunk
        if ((cpu ? creg_max : reg_max)[k] <= since || !ch) {
            if (run) shadow_copy(r, run, a), run = 0;
            continue;
        }
        for (uint64_t x = a; x < b; x += 1ull << GEN_PAGE_SHIFT) {
            int dirty = ch[(x >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)] > since;
            if (dirty && !run) run = x;
            else if (!dirty && run) shadow_copy(r, run, x), run = 0;
        }
    }
    if (run) shadow_copy(r, run, hi);
}

// Record time: the GPU may write [addr, addr+size) of CPU-visible memory. With shadow storage the pages it covers get
// their base copies before the submission's copy-in (vk_mem_shadow_sync) can reach them.
void vk_mem_gpu_pending(uint64_t addr, uint64_t size) {
    if (!shadow_on || !size) return;
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, addr, size);
    PoolRec *r = e ? e->buf : NULL;
    plat_mutex_lock(&shadow_lock);
    if (r && r->smap && addr >= r->import_lo && addr + size <= r->import_hi)
        for (uint64_t pg = addr & ~(SB_PAGE - 1); pg < addr + size; pg += SB_PAGE) sbase_make(r, pg);
    plat_mutex_unlock(&shadow_lock);
    plat_mutex_unlock_shared(&pool_lock);
}

// NX2NAVI_WB_NO_REUP (default 1; 0 = off): the pages a VRAM mirror wrote back into guest memory are not copied up into
// it again at its next sync. vk_gpu_written stamps the pages the GPU wrote, at the fence, so the texture cache re-reads
// them; the same stamps made mirror_sync copy them back up into the mirror the write-back had read them from: the
// storage buffers the game's compute shaders write went down and came back up every frame, about 57 of the 60 MiB copied
// up a frame outdoors (RTX 4080 SUPER, 2026-11-08) and 20 MiB a frame in Kokiri Forest (RTX 4090, PCIe 3.0, 2026-11-09).
// A write into a mirror (vk_mem_mirrored, noted at record time) stamps only the texture tables (vk_gpu_written_back);
// mirror_sync reads the CPU-side ones (stamp_page_as), which a CPU write or a host path still stamps.
static int wb_reup_off = -1;
static int wb_no_reup(void) {
    if (wb_reup_off < 0) {
        int on = rt_knob("WB_NO_REUP", "1") == 1;
        rt_log("mem: pages a VRAM mirror wrote back %s (NX2NAVI_WB_NO_REUP)",
               on ? "are not copied up into it again" : "are copied up into it again");
        __atomic_store_n(&wb_reup_off, on, __ATOMIC_RELEASE);
    }
    return wb_reup_off;
}

// NX2NAVI_SHADOW_BULK_WRITE=1 (default 0): before a write-back stores into guest memory, the write watch's protection
// of the whole range is cleared in one call (plat_mem_prepare_write). Where the watch protects pages (Linux), the
// first store into each page otherwise takes a fault of its own; the pages count as written either way.
static int shadow_bulk_write(void) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2NAVI_SHADOW_BULK_WRITE");
        if (on) rt_log("mem: shadow write-backs clear the write watch's protection of their range in one call "
                       "(NX2NAVI_SHADOW_BULK_WRITE)");
    }
    return on;
}

// NX2NAVI_SHADOW_CHECK: the CPU wrote guest page pg after the copy-in that sgen stands for: a write-watch snapshot
// stamped it later, or its write-watch bit is set and no snapshot has read it yet. A write-back that moves bytes into
// such a page may have come after a CPU write it should have come before ("The game must not get ahead of the
// write-backs" above); a page of which the CPU wrote other bytes counts too.
// No gen_lock, as lazy_clean: a stamp that lands meanwhile only blurs a count.
static int cpu_wrote_since(uint64_t pg, uint32_t sgen) {
    const uint32_t *ch = __atomic_load_n(&cgen_chunk[pg >> GEN_CHUNK_SHIFT], __ATOMIC_ACQUIRE);
    if (ch && __atomic_load_n(&ch[(pg >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)], __ATOMIC_RELAXED) > sgen) return 1;
    uint64_t p[1], n = mem_written_peek(pg, SB_PAGE, p, 1);
    return n && n != UINT64_MAX;
}

// Completion thread: the GPU's bytes of [addr, addr+size) from the shadow into guest memory (merge_page against the
// base copies; a page without one whole). Returns the bytes moved.
// A page without a base copy is one no write was noted on in this record (vk_mem_gpu_pending notes every write the
// replay records). It gets here when the pool the write was noted on was finalized before the write's fence and
// another was made at its address: shadow_forget dropped the base copies, and the new record's shadow is a copy of
// guest memory from its own start that holds no byte of that write. Copied whole, it put those older bytes over
// everything the CPU had written to the page since, for no gain. So the page is left alone when the record is younger
// than the submission whose fence this is (sborn), or when the CPU has written the page since the shadow's last
// copy-in (cpu_wrote_since: a pool made again while that submission was still being recorded has the same value).
// Any other such page goes whole as before: a write nothing noted, into a shadow that holds it.
static uint64_t shadow_write_back(uint64_t addr, uint64_t size) {
    uint64_t moved = 0;
    int bulk = shadow_bulk_write();   // before the locks: its first call logs
    uint64_t value = vk_sync_cb_value();   // the fence being completed; a call from elsewhere reports submitted work
    if (!value) value = vk_sync_issued();
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, addr, size);
    PoolRec *r = e ? e->buf : NULL;
    plat_mutex_lock(&shadow_lock);
    if (r && r->smap && addr >= r->import_lo && addr + size <= r->import_hi) {
        uint64_t t0 = qpc();
        mem_thaw(addr, size);
        if (bulk) plat_mem_prepare_write(addr, size);
        for (uint64_t a = addr, end = addr + size; a < end;) {
            uint64_t pg = a & ~(SB_PAGE - 1), pe = pg + SB_PAGE < end ? pg + SB_PAGE : end;
            uint8_t *base = sbase_find(r, pg), *s = r->smap + (a - r->import_lo);
            if (base && s_check) s_both_gpu += both_changed((const uint8_t *)(uintptr_t)a, s, base + (a - pg), pe - a);
            int late = base && s_check && cpu_wrote_since(pg, r->sgen);   // before the merge's own stores into the page
            uint64_t n = 0;
            if (base) moved += n = merge_page((uint8_t *)(uintptr_t)a, s, base + (a - pg), pe - a);
            else if (r->sborn > value || cpu_wrote_since(pg, r->sgen)) s_stale_pages++;   // the shadow cannot hold the write
            else memcpy((void *)(uintptr_t)a, s, pe - a), moved += pe - a;   // a write never noted as pending
            if (late && n) s_late_pages++, s_late_bytes += n;
            a = pe;
        }
        s_wb_bytes += moved, s_wb_seen += size, s_wb_ticks += qpc() - t0;
    }
    plat_mutex_unlock(&shadow_lock);
    plat_mutex_unlock_shared(&pool_lock);
    return moved;
}

// Completion thread, at the fence of a submission whose record bound the device address table (vk_mem_shadow_sync):
// what the shaders stored into the pages found so far ("Pages a shader stores into" above) goes from the shadow into
// guest memory, the bytes that differ from the page's base copy. A page whose shadow equals its base is passed over
// before guest memory is touched, so a frozen region stays frozen. The pages that changed are stamped as a mirror's
// write-back is (swb_run): the stores are in the mirror already.
static void table_wb_cb(void *arg) {
    (void)arg;
    static uint64_t *wrote;   // this thread only: the pages that changed
    static int cwrote;
    int nwrote = 0, bulk = shadow_bulk_write(), back = wb_no_reup();   // before the locks: their first calls log
    uint64_t t0 = qpc(), moved = 0, seen = 0;
    plat_mutex_lock(&shadow_lock);
    for (int i = 0; i < ntpools; i++) {
        PoolRec *r = tpools[i];
        for (uint32_t k = 0; k < r->ntpg; k++) {
            uint64_t pg = r->tpg[k];
            uint8_t *base = sbase_find(r, pg), *s = r->smap + (pg - r->import_lo);
            seen += SB_PAGE;
            if (!base || !memcmp(s, base, SB_PAGE)) continue;
            if (mem_frozen(pg)) mem_thaw(pg, SB_PAGE);
            if (bulk) plat_mem_prepare_write(pg, SB_PAGE);
            if (s_check) s_both_gpu += both_changed((const uint8_t *)(uintptr_t)pg, s, base, SB_PAGE);
            uint64_t n = merge_page((uint8_t *)(uintptr_t)pg, s, base, SB_PAGE);
            if (!n) continue;   // the store came after the compare above: the next fence takes it
            moved += n;
            if (nvn_gpu_only(pg)) continue;
            if (nwrote == cwrote) grow((void **)&wrote, &cwrote, sizeof *wrote);
            wrote[nwrote++] = pg;
        }
    }
    s_tab_wb += moved, s_tab_wb_seen += seen, s_tab_wb_jobs++;
    if (nwrote) {
        plat_mutex_lock(&gen_lock);
        uint32_t g = ++gen_now;
        for (int i = 0; i < nwrote; i++) stamp_page_as(wrote[i], g, back);
        plat_mutex_unlock(&gen_lock);
    }
    s_tab_wb_ticks += qpc() - t0;
    plat_mutex_unlock(&shadow_lock);
}

static void gpu_written(uint64_t addr, uint64_t size, int back) {
    // back: the bytes are in a VRAM mirror, and its write-back brings them into the shadow; that submission's
    // shadow_wb_cb, after the replay's own callbacks, then copies them into guest memory.
    if (shadow_on && size && !back) shadow_write_back(addr, size);
    if (!size || nvn_gpu_only(addr)) return;
    uint64_t lo = addr & ~0xfffull, hi = (addr + size + 0xfff) & ~0xfffull;
    back = back && wb_no_reup();   // read before gen_lock: its first call logs
    plat_mutex_lock(&gen_lock);
    uint32_t g = ++gen_now;
    for (uint64_t a = lo; a < hi; a += 1ull << GEN_PAGE_SHIFT) stamp_page_as(a, g, back);
    plat_mutex_unlock(&gen_lock);
}

void vk_gpu_written(uint64_t addr, uint64_t size) { gpu_written(addr, size, 0); }
static int wb_lazy(void);
static int lazy_clean(uint64_t lo, uint64_t hi, void *ctx);

void vk_gpu_written_back(uint64_t addr, uint64_t size) {
    gpu_written(addr, size, 1);
    // NX2NAVI_WB_PROBE: the bytes are in guest memory now; note who reads them next. With shadow storage they get there
    // later, in the submission's shadow_wb_cb, whose thaw would end the probe at once: that job arms it instead.
    if (!shadow_on) mem_probe(addr, size);
    VkMemSpan sp;
    if (size && wb_lazy() && vk_mem_lookup(addr, size, &sp) && sp.pool) {   // or newer ones stay in the mirror from here on
        // The check and the arm under pool_lock: mirror_evict releases what is armed in the pool's range and drops its
        // mirror under the exclusive lock, and pool_fini takes the pool off the index under it. Without the lock an arm
        // could land between the two and name the evicted mirror, which mem_lazy_skip gives back again later.
        PoolRec *r = sp.pool;
        plat_mutex_lock_shared(&pool_lock);
        const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)addr, size);
        if (e && e->buf == r && r->mstate == MS_ON && r->mbuf && r->buf && addr >= r->import_lo &&
            addr + size <= r->import_hi)
            mem_lazy_arm(addr, size, (uint64_t)(uintptr_t)r->mbuf, (uint64_t)(uintptr_t)r->buf, addr - r->import_lo, r);
        plat_mutex_unlock_shared(&pool_lock);
    }
}

int vk_mem_mirrored(uint64_t addr, uint64_t size) {
    VkMemSpan sp;
    if (!vk_mirror_on || !size || !vk_mem_lookup(addr, size, &sp) || !sp.pool) return 0;
    const PoolRec *r = sp.pool;
    return r->mstate == MS_ON && addr >= r->import_lo && addr + size <= r->import_hi;
}

// shadow_lock held, with the shadow line: the pages a shader stores into through the device address table ("shadow
// storage" above), and with NX2NAVI_SHADOW_CHECK (pool_lock held shared too, for the index) what the copy-ins did and
// cost and the pools whose stores were overwritten most since the last line.
#define TABLE_LOG_TOP 6
static void shadow_table_log(uint64_t frame) {
    rt_log("mem: shadow table stores at frame %llu (%s): %llu pages in %d pools keep a base copy; since the last: %llu whole "
           "copy-ins overwrote a store (%llu so far), %llu pages copied in with a mark; written back %.3f MiB of %.1f "
           "compared in %llu fence jobs, %.2f ms",
           (unsigned long long)frame, s_learn ? "NX2NAVI_SHADOW_TABLE_STORES=1: kept from the second copy-in on" : "overwritten",
           (unsigned long long)s_tab_pages, ntpools, (unsigned long long)s_tab_over, (unsigned long long)s_t_over_all,
           (unsigned long long)s_marked, s_tab_wb / 1048576.0, s_tab_wb_seen / 1048576.0, (unsigned long long)s_tab_wb_jobs,
           qpc_ms(s_tab_wb_ticks));
    if (s_check) {
        rt_log("mem: shadow check at frame %llu: copied in whole %.2f ms with marks, %.2f ms without; %.1f MiB by merge, "
               "%.1f MiB whole into pools without a mirror; %llu table syncs %.2f ms; bytes both sides changed: %llu taken "
               "from the CPU at a copy-in, %llu from the GPU at a fence; write-backs into %llu pages the CPU had written "
               "since their copy-in, %llu bytes",
               (unsigned long long)frame, qpc_ms(s_mark_ticks), qpc_ms(s_plain_ticks), s_in_merge / 1048576.0,
               s_in_nomirror / 1048576.0, (unsigned long long)s_tab_syncs, qpc_ms(s_tab_ticks),
               (unsigned long long)s_both_cpu, (unsigned long long)s_both_gpu, (unsigned long long)s_late_pages,
               (unsigned long long)s_late_bytes);
        PoolRec *top[TABLE_LOG_TOP];
        int nt = 0;
        for (int i = 0; i < pool_index.n; i++) {
            PoolRec *r = pool_index.e[i].buf;
            uint64_t d = r->kind == POOL_IMPORT ? r->t_over - r->t_over_log : 0;
            int k = nt;
            while (k > 0 && top[k - 1]->t_over - top[k - 1]->t_over_log < d) k--;
            if (!d || k >= TABLE_LOG_TOP) continue;
            int last = nt < TABLE_LOG_TOP ? nt : TABLE_LOG_TOP - 1;
            memmove(top + k + 1, top + k, (size_t)(last - k) * sizeof *top);
            top[k] = r;
            if (nt < TABLE_LOG_TOP) nt++;
        }
        for (int i = 0; i < nt; i++) {
            char f[160];
            flag_names(top[i]->flags, f, sizeof f);
            rt_log("mem: shadow check:   pool %#llx+%#llx (%s, %s): %llu whole copy-ins overwrote a store (%llu so far), "
                   "%u of its pages keep a base copy for it",
                   (unsigned long long)top[i]->lo, (unsigned long long)top[i]->size, f,
                   top[i]->mstate == MS_ON ? "mirrored" : "no mirror",
                   (unsigned long long)(top[i]->t_over - top[i]->t_over_log), (unsigned long long)top[i]->t_over, top[i]->ntpg);
        }
        // the pools that took the most whole pages, and what all pools of each flag set took
        struct { uint32_t flags; uint64_t bytes; int n; } cls[16];
        int ncls = 0;
        nt = 0;
        for (int i = 0; i < pool_index.n; i++) {
            PoolRec *r = pool_index.e[i].buf;
            if (r->kind != POOL_IMPORT || !r->s_in) continue;
            int c = 0, k = nt;
            while (c < ncls && cls[c].flags != r->flags) c++;
            if (c == ncls && ncls < 16) cls[ncls++] = (__typeof__(cls[0])){r->flags, 0, 0};
            if (c < ncls) cls[c].bytes += r->s_in, cls[c].n++;
            while (k > 0 && top[k - 1]->s_in < r->s_in) k--;
            if (k >= TABLE_LOG_TOP) continue;
            int last = nt < TABLE_LOG_TOP ? nt : TABLE_LOG_TOP - 1;
            memmove(top + k + 1, top + k, (size_t)(last - k) * sizeof *top);
            top[k] = r;
            if (nt < TABLE_LOG_TOP) nt++;
        }
        for (int i = 0; i < ncls; i++) {
            char f[160];
            flag_names(cls[i].flags, f, sizeof f);
            rt_log("mem: shadow check:   copied in whole into %d pools %s: %.2f MiB", cls[i].n, f, cls[i].bytes / 1048576.0);
        }
        for (int i = 0; i < nt; i++) {
            char f[160];
            flag_names(top[i]->flags, f, sizeof f);
            rt_log("mem: shadow check:   pool %#llx+%#llx (%s, %s, %s): %.2f MiB copied in whole",
                   (unsigned long long)top[i]->lo, (unsigned long long)top[i]->size, f,
                   top[i]->mstate == MS_ON ? "mirrored" : "no mirror", top[i]->used ? "resolved by the GPU" : "never resolved",
                   top[i]->s_in / 1048576.0);
        }
        for (int i = 0; i < pool_index.n; i++) {
            PoolRec *r = pool_index.e[i].buf;
            if (r->kind == POOL_IMPORT) r->t_over_log = r->t_over, r->s_in = 0;
        }
    }
    s_tab_over = s_tab_wb = s_tab_wb_seen = s_tab_wb_ticks = s_tab_wb_jobs = s_marked = s_mark_ticks = s_plain_ticks = 0;
    s_both_cpu = s_both_gpu = s_in_merge = s_in_nomirror = s_tab_ticks = s_tab_syncs = s_late_pages = s_late_bytes = 0;
}

static void shadow_log(uint64_t frame) {
    if (!shadow_on) return;
    if (s_check) plat_mutex_lock_shared(&pool_lock);   // before shadow_lock: shadow_table_log walks the index
    plat_mutex_lock(&shadow_lock);
    rt_log("mem: shadow at frame %llu: %lld pools %.1f MiB; since the last: %llu pool syncs, %llu queries, %.1f MiB "
           "copied in (%llu untracked), %.2f ms (queries %.2f); %.1f MiB written back of %.1f MiB the GPU may have written, %.2f ms; %llu "
           "pages the GPU writes keep a base copy; %llu pages not written back, their pool was made again or the CPU "
           "wrote them since",
           (unsigned long long)frame, (long long)s_live_n, s_live_bytes / 1048576.0, (unsigned long long)s_syncs,
           (unsigned long long)s_queries, s_copied / 1048576.0, (unsigned long long)s_untracked, qpc_ms(s_ticks),
           qpc_ms(s_q_ticks), s_wb_bytes / 1048576.0, s_wb_seen / 1048576.0, qpc_ms(s_wb_ticks), (unsigned long long)s_sb_pages,
           (unsigned long long)s_stale_pages);
    s_syncs = s_queries = s_copied = s_untracked = s_wb_bytes = s_ticks = s_wb_seen = s_wb_ticks = s_q_ticks = 0;
    s_stale_pages = 0;
    if (s_learn || s_check) shadow_table_log(frame);
    plat_mutex_unlock(&shadow_lock);
    if (s_check) plat_mutex_unlock_shared(&pool_lock);
}

void vk_mem_dirty_submit(void) {
    if (!(rt_trace & RT_TRACE_PERF) || vk_mirror_on) return;   // the mirror's own line replaces the census
    uint64_t t0 = kern_now_ns();
    uint32_t prev = dirty_sub;   // the submit whose replay noted the bindings
    dirty_sub = prev + 1 ? prev + 1 : 1;
    uint64_t eager[DC_N] = {0}, lazy[DC_N] = {0}, ln[DC_N] = {0}, lb[DC_N] = {0}, untracked = 0, queries = 0;
    plat_mutex_lock_shared(&pool_lock);
    if (dcap < pool_index.n) {
        dcap = pool_index.n * 2;
        dlist = realloc(dlist, (size_t)dcap * sizeof *dlist);
        if (!dlist) rt_fatal("vk: out of memory for the dirty census");
    }
    int n = 0;
    for (int i = 0; i < pool_index.n; i++) {
        PoolRec *r = pool_index.e[i].buf;
        if (r->kind == POOL_IMPORT && r->bufbound) dlist[n++] = r;
    }
    plat_mutex_lock(&gen_lock);
    uint32_t g = ++gen_now, since = dirty_gen;
    dirty_gen = g;
    for (int i = 0; i < n;) {   // one query per run of pools (sorted by address)
        uint64_t lo = page_lo(dlist[i]), hi = page_hi(dlist[i]);
        int j = i + 1;
        for (; j < n && page_lo(dlist[j]) <= hi + DIRTY_GAP; j++)
            if (page_hi(dlist[j]) > hi) hi = page_hi(dlist[j]);
        queries++;
        int ok = query_stamp(lo, hi, g);
        for (int k = i; k < j; k++) {
            dlist[k]->untracked = 0;
            if (!ok) queries++, dlist[k]->untracked = (uint8_t)!query_stamp(page_lo(dlist[k]), page_hi(dlist[k]), g);
        }
        i = j;
    }
    uint64_t covered = 0;
    for (int i = 0; i < n; i++) {
        PoolRec *r = dlist[i];
        int k = dc_class(r);
        ln[k]++, lb[k] += r->size;
        int fresh = r->synced_gen == DIRTY_UNSYNCED, lz = !fresh && r->bound_sub == prev;
        uint32_t ls = r->synced_gen;
        if (fresh || r->bound_sub == prev) r->synced_gen = g;
        if (r->untracked) { untracked += r->size; continue; }
        if (fresh) continue;   // counted as first use
        uint64_t lo = page_lo(r), hi = page_hi(r), ne = 0, nl = 0;
        if (lo < covered) lo = covered;   // overlapping pools: each page once
        if (lo >= hi) continue;
        covered = hi;
        for (uint64_t a = lo; a < hi;) {
            uint64_t ci = a >> GEN_CHUNK_SHIFT, ce = (ci + 1) << GEN_CHUNK_SHIFT;
            if (ce > hi) ce = hi;
            const uint32_t *ch = ci < GEN_CHUNKS ? gen_chunk[ci] : NULL;
            if (ch)
                for (uint64_t x = a; x < ce; x += 1ull << GEN_PAGE_SHIFT) {
                    uint32_t v = ch[(x >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)];
                    ne += v > since;
                    nl += lz && v > ls;
                }
            a = ce;
        }
        eager[k] += ne << GEN_PAGE_SHIFT, lazy[k] += nl << GEN_PAGE_SHIFT;
    }
    plat_mutex_unlock(&gen_lock);
    plat_mutex_unlock_shared(&pool_lock);
    uint64_t tot = 0;
    for (int k = 0; k < DC_N; k++) tot += eager[k];
    plat_mutex_lock(&dirty_lock);
    for (int k = 0; k < DC_N; k++) {
        dfr.eager[k] += eager[k], dfr.lazy[k] += lazy[k], dfr.first[k] += first_pending[k];
        first_pending[k] = 0;
        live_n[k] = ln[k], live_bytes[k] = lb[k], ever_snap_n[k] = ever_n[k], ever_snap_bytes[k] = ever_bytes[k];
    }
    live_untracked = untracked;
    dfr.submits++, dfr.queries += queries;
    if (tot > dfr.sub_max) dfr.sub_max = tot;
    dfr.scan_ns += kern_now_ns() - t0;
    plat_mutex_unlock(&dirty_lock);
}

static void dirty_frame(uint64_t frame) {
    plat_mutex_lock(&dirty_lock);
    if (dwin.frames < DIRTY_WIN)
        for (int k = 0; k < DC_N; k++) dwin_kib[dwin.frames][k] = (uint32_t)(dfr.eager[k] >> 10);
    for (int k = 0; k < DC_N; k++) dwin.eager[k] += dfr.eager[k], dwin.lazy[k] += dfr.lazy[k], dwin.first[k] += dfr.first[k];
    dwin.submits += dfr.submits, dwin.queries += dfr.queries, dwin.scan_ns += dfr.scan_ns, dwin.frames++;
    if (dfr.sub_max > dwin.sub_max) dwin.sub_max = dfr.sub_max;
    memset(&dfr, 0, sizeof dfr);
    if (frame % DIRTY_WIN) { plat_mutex_unlock(&dirty_lock); return; }
    DirtyAcc w = dwin;
    uint32_t kib[DIRTY_WIN][DC_N];
    memcpy(kib, dwin_kib, sizeof kib);
    uint64_t lv[DC_N], lvb[DC_N], ev[DC_N], evb[DC_N], unt = live_untracked;
    for (int k = 0; k < DC_N; k++) lv[k] = live_n[k], lvb[k] = live_bytes[k], ev[k] = ever_snap_n[k], evb[k] = ever_snap_bytes[k];
    memset(&dwin, 0, sizeof dwin);
    plat_mutex_unlock(&dirty_lock);
    if (!w.frames) return;
    uint64_t fl[2][3][2];
    nvn_flush_stats(fl);
    double f = (double)w.frames, M = 1048576.0;
    uint64_t sub_tot = 0;
    for (int k = 0; k < DC_N; k++) sub_tot += w.eager[k];
#define C3(a, d) (a)[0] / (d), (a)[1] / (d), (a)[2] / (d)
    rt_log("mem: dirty frame %llu: frames %llu submits_per_frame %.2f; per frame, cached/uncached/other: eager_mib "
           "%.3f/%.3f/%.3f lazy_mib %.3f/%.3f/%.3f first_mib %.3f/%.3f/%.3f flush_pool_calls %.1f/%.1f/%.1f "
           "flush_pool_mib %.3f/%.3f/%.3f flush_buf_calls %.1f/%.1f/%.1f flush_buf_mib %.3f/%.3f/%.3f; per submit: "
           "avg_mib %.3f max_mib %.3f; mirror: live_pools %llu/%llu/%llu live_mib %.1f/%.1f/%.1f ever_pools %llu/%llu/%llu "
           "ever_mib %.1f/%.1f/%.1f untracked_mib %.1f; cost: queries_per_frame %.1f scan_ms_per_frame %.3f",
           (unsigned long long)frame, (unsigned long long)w.frames, w.submits / f,
           C3(w.eager, f * M), C3(w.lazy, f * M), C3(w.first, f * M),
           fl[0][0][0] / f, fl[0][1][0] / f, fl[0][2][0] / f, fl[0][0][1] / f / M, fl[0][1][1] / f / M, fl[0][2][1] / f / M,
           fl[1][0][0] / f, fl[1][1][0] / f, fl[1][2][0] / f, fl[1][0][1] / f / M, fl[1][1][1] / f / M, fl[1][2][1] / f / M,
           w.submits ? sub_tot / (double)w.submits / M : 0.0, w.sub_max / M,
           (unsigned long long)lv[0], (unsigned long long)lv[1], (unsigned long long)lv[2], C3(lvb, M),
           (unsigned long long)ev[0], (unsigned long long)ev[1], (unsigned long long)ev[2], C3(evb, M), unt / M,
           w.queries / f, w.scan_ns / 1e6 / f);
#undef C3
    char line[DIRTY_WIN * 36 + 64];
    size_t k = (size_t)snprintf(line, sizeof line, "mem: dirty frames KiB cached/uncached/other:");
    for (uint64_t i = 0; i < w.frames && i < DIRTY_WIN; i++)
        k += (size_t)snprintf(line + k, k < sizeof line ? sizeof line - k : 0, " %u/%u/%u", kib[i][0], kib[i][1], kib[i][2]);
    rt_log("%s", line);
}

// ---------------------------------------------------------------- VRAM mirror of buffer-bound pools
// An imported pool keeps vertices, indices, constants
// and storage buffers in system memory, so a discrete GPU reads them over PCIe on every draw (GTX 1050 Ti and RTX
// 3090 Ti: about 110 ms of GPU time per frame in the field). In mirror mode (device class discrete, NX2NAVI_MEMORY) an
// imported pool gets a device-local buffer over its imported range the first time the replay uses it as a vertex,
// index, uniform, storage or indirect buffer (vk_mem_use). From then on those bindings, and the buffer copies, fills
// and uniform updates that touch the pool (vk_mem_pick), use the mirror at the same offsets. Guest memory stays what
// the CPU reads and writes; textures and every host path that reads or writes pool bytes keep using it.
// - CPU -> mirror: the first access of a submission to a mirror syncs it (mirror_sync): the 2 MiB write-watch
//   snapshots of its regions for this epoch (snap_regions, shared with the texture checks: the OS bits keep one
//   consumer, the shared page generations) and every page the CPU-side tables (cgen_chunk) stamped after the mirror's
//   mgen goes into a copy. Where a region of the pool is not write-watched, so do the nvnMemoryPoolFlushMappedRange
//   ranges, each on its own: one span from the lowest to the highest flush of a 252 MiB pool copied up to the whole
//   pool for a few MiB flushed, 30-50 ms of GPU time over PCIe 3.0 x8 in crowded towns (NX2NAVI_FLUSH_SPAN=1 restores
//   it). Where the write watch covers the pool, a flushed page the CPU wrote is a written page already, and the game's
//   flushes of whole pools it had not written cost 47-57 MiB of copies a frame in Kokiri Forest (2026-11-09);
//   NX2NAVI_FLUSH_UPLOAD=1 copies them anyway. The first use copies the whole range. The copies run in a prologue command
//   buffer that vk_sync.c submits right before the part that accessed the mirror first, between full barriers.
//   vk_mem_mirror_begin snaps the regions of the pools the previous submission accessed in a few merged queries.
// - GPU -> guest: what the replay writes into a mirror (storage buffers its shaders write, buffer copies, fills,
//   uniform updates, texture-to-buffer copies: vk_mem_gpu_wrote) goes back into the imported buffer at the end of the
//   submission, exact byte ranges, before the fence signals (vk_mem_mirror_end). At the fence those pages get a new
//   generation for the texture checks (vk_gpu_written_back) but not in the CPU-side tables: the mirror holds the bytes.
// - NX2NAVI_MEMORY=verify: every submission also copies up to VERIFY_BYTES of mirror pages, round robin, into host memory;
//   at its fence the pages the CPU has not written since their sync are compared with guest memory.
// A pool stays imported when its mirror would not fit the VRAM budget, its range is not write-watched, it overlaps
// another pool or the allocation fails.
#define VERIFY_BYTES (4u << 20)
#define VERIFY_SAMPLES 64
#define VERIFY_LOG 200
enum { MR_BUDGET, MR_UNTRACKED, MR_OVERLAP, MR_ALLOC, MR_N };
#define RETRY_SUBS 600                 // a pool refused for the budget asks again this many submissions later (eviction)

int vk_mirror_on;
static int mirror_verify;
static int mirror_gap = 2;             // pre-snap: hot pools at most this many 2 MiB regions apart share one query
static uint32_t pframe;                // replay: frames begun (vk_mem_mirror_begin: a place not after the last one's)
static int pplace = 99;                // replay: the place of the last submission vk_mem_mirror_begin saw

// NX2NAVI_PRESNAP_PLACE=1 (default), 0, ab: a submission's pre-snap (vk_mem_mirror_begin) takes the write-watch
// snapshots of the pools the submission at its place in the last frame synced, not those the submission before it
// synced. In Kokiri Forest the frame's first submission (#0) and its fourth (#3) use no pool, and their pre-snaps
// covered the pools of the submission before them, #5's and #2's: 36.8 queries over 282 MiB a frame, 0.23-0.31 ms; then
// #2 and #5, whose submissions before them use no pool either, queried their pools one at a time at their first use,
// 63.5 queries over 254 MiB.
static int presnap_place(void) {
    static int mode = -1;
    if (mode < 0) mode = rt_knob("PRESNAP_PLACE", "1");
    return RT_KNOB_ON(mode);
}

static int sub_place(void) { return vk_sub_in_frame >= 0 && vk_sub_in_frame < 7 ? vk_sub_in_frame : 7; }

static void pm_note(PoolRec *r) {   // replay: r synced in the submission being recorded
    if (r->pm_frame != pframe) {
        r->pm_prev = r->pm_frame + 1 == pframe ? r->pm_cur : 0;
        r->pm_cur = 0, r->pm_frame = pframe;
    }
    r->pm_cur |= (uint8_t)(1u << sub_place());
}

static uint8_t pm_last(const PoolRec *r) {   // the places that synced r in the last frame
    return r->pm_frame == pframe ? r->pm_prev : r->pm_frame + 1 == pframe ? r->pm_cur : 0;
}
static PoolRec **mlist;                // pool_lock: mirrored pools by import_lo
static int nm, mcap;
static uint32_t msub;                  // replay: number of the submission being recorded
static uint32_t mepoch;                // replay: its write-watch snapshot epoch (vk_texture_epoch)
typedef struct { PoolRec *r; uint64_t off, size; } MRange;
static MRange *pend, *wbl;             // replay: copies for the next prologue; GPU writes into mirrors this submission
static int npend, cpend, nwbl, cwbl;
static VkBufferCopy *mregions;         // replay: vkCmdCopyBuffer regions
static int cregions;
static PlatMutex flush_lock = PLAT_MUTEX_INIT;
static int flush_cap = FLUSH_RANGES;   // ranges a pool keeps; 1 with NX2NAVI_FLUSH_SPAN=1
static int flush_upload;               // NX2NAVI_FLUSH_UPLOAD=1: flush ranges are copied where the write watch sees them too
// NX2NAVI_FLUSH_SKIP_CLEAN (flush_clean_bytes in the 0.5.0 and 0.6.0 fixes: flushed ranges upload only the pages the CPU
// wrote) is this default since 0.6.1: a flush range of a write-watched pool is not copied at all; the key stays known so an
// nx2navi.ini that sets it reads without a warning, and does nothing.

typedef struct {
    uint64_t subs, syncs, upload_bytes, upload_copies, first_n, first_bytes, wb_bytes, wb_copies, wb_ranges;
    uint64_t presnap_queries, sync_queries, presnap_t, sync_t, prologues, presnap_bytes;
    uint64_t snap_bytes, merge_bytes, merge_dispatches, snap_allocs;   // write-back merge
    uint64_t page_bytes, flush_n, flush_bytes, flush_calls, flush_sum;   // what mirror_sync queues, by source
    uint64_t flush_skipped;   // flush ranges not copied: the write watch covers them
    uint64_t cap_bytes, cap_ranges, cap_left, cap_t;   // mirror_capture: copied by the CPU, left to the GPU copy, its time
} MirrorAcc;
static MirrorAcc mrep;                 // replay: this submission
static MirrorAcc macc;                 // mirror_stat_lock: since the last summary
static PlatMutex mirror_stat_lock = PLAT_MUTEX_INIT;
static volatile int64_t m_live_n, m_live_bytes, m_refused_n[MR_N], m_refused_bytes[MR_N];
static volatile int64_t m_flush_calls, m_flush_bytes, v_pages, v_skipped, v_mismatch;

static void mirror_mode(int discrete, int nvidia_own) {
    const char *e = cfg_value("NX2NAVI_MEMORY");
    int mode = discrete ? 1 : 0;   // 0 import, 1 mirror, 2 verify
    const char *why = discrete ? "auto, discrete gpu" : "auto, uma";
    if (strcmp(e, "auto")) {
        if (!strcmp(e, "import")) mode = 0, why = "NX2NAVI_MEMORY=import";
        else if (!strcmp(e, "mirror")) mode = 1, why = "NX2NAVI_MEMORY=mirror";
        else if (!strcmp(e, "verify")) mode = 2, why = "NX2NAVI_MEMORY=verify";
        else rt_log("mem: NX2NAVI_MEMORY=%s is not import, mirror, verify or auto: auto", e);
    }
    vk_mirror_on = mode > 0;
    mirror_verify = mode == 2;
    const char *g = cfg_get("NX2NAVI_MIRROR_GAP");
    mirror_gap = g && *g ? atoi(g) : 2;
    if (mirror_gap < 0) mirror_gap = 0;
    if (rt_knob("FLUSH_SPAN", "0")) {
        flush_cap = 1;
        rt_log("mem: a pool's flushes copy one span from the lowest to the highest (NX2NAVI_FLUSH_SPAN=1)");
    }
    flush_upload = rt_knob("FLUSH_UPLOAD", "0") == 1;
    if (flush_upload) rt_log("mem: flushed ranges are copied into the mirrors as well as written pages (NX2NAVI_FLUSH_UPLOAD=1)");
    // Submissions a region stays unwritten before it is frozen (NX2NAVI_FREEZE_IDLE; 0: no freezing). 60 was chosen
    // when a thaw by a write cost about 60 us: in a fight 100 of the 257 MiB queried a frame lay in regions idle for
    // 30-299 submissions. While the game streams an area, though, it creates and finalizes up to 1,500 memory pools a
    // second, the driver pins and unpins the imported ones, and every VirtualQuery and VirtualProtect in the process
    // waits behind that: a thaw took 10-20 ms with frz_lock held, which the replay needs too. The streaming buffers,
    // idle for a few frames at a time, were frozen and thawed about five times a second, and walking from Kakariko
    // Village through the Bottom of the Well to the Graveyard had a frame of 25-90 ms in 18-24% of its seconds (11%
    // with freezing off, at 3-3.6 ms a frame more of write-watch queries over 3.5 GiB). At 1800 (about 3 s) a walk
    // through Hyrule Field and Ganon's Castle froze 0.1 regions a second, thawed none, queried 14 MiB in 0.03 ms a
    // frame, and had such a frame in 1 of 253 seconds.
    const char *fi = cfg_get("NX2NAVI_FREEZE_IDLE");
    freeze_idle = fi && *fi ? atoi(fi) : 1800;
    if (freeze_idle < 0) freeze_idle = 0;
    const char *sh = cfg_get("NX2NAVI_SHADOW");
#ifdef _WIN32
    shadow_on = sh && *sh ? strcmp(sh, "0") != 0 : plat_is_wine();
#else
    // NVIDIA's own driver imports guest memory without the cost shadow storage avoids on amdgpu (re-protecting imported
    // pages), so there the copies are only the slower path: in Kokiri Forest on 0.7.1 (RTX 4090, driver 615.78) imports
    // with VRAM mirrors ran at 51.9 frames a second and the copies at 27.4, and the copies hold about 5 GB. What a
    // shader stores through the device address table into a pool with a shadow copy also does not come back to guest
    // memory (vk_gpa.c). The pools stay imports unless NX2NAVI_SHADOW asks for the copies. The open driver for the same
    // cards keeps the copies: it is not measured. The AppImage's play.sh has set the same for this driver since 0.6.0.
    shadow_on = sh && *sh ? strcmp(sh, "0") != 0 : !nvidia_own;
    // Guest memory a memfd's (NX2NAVI_DMABUF, platform/linux.c): the pools are dma-buf imports ("dma-buf imports"),
    // whatever the driver, unless NX2NAVI_SHADOW=1 asks for the copies. A device that cannot import dma-bufs keeps the
    // copies: a shared mapping's pages are no host-pointer import on RADV.
    uint64_t lim = 0;
    const char *dmwhy = "";
    static char small[200];
    if (plat_mem_dmabuf_ok(&lim, &dmabuf_any, &dmwhy)) {
        int forced = sh && *sh && strcmp(sh, "0");
        // The game's largest pools take about 300 MiB, each in one dma-buf: below 1 GiB (the module's default is 64 MB)
        // some would get none, so the copies stay.
        if (lim < (1ull << 30))
            snprintf(small, sizeof small, "udmabuf's size_limit_mb is %llu, below the 1024 a pool of the game may need "
                     "(/sys/module/udmabuf/parameters/size_limit_mb)", (unsigned long long)(lim >> 20));
        dmabuf_on = dmabuf_exts && !forced && lim >= (1ull << 30);
        shadow_on = !dmabuf_on;
        dmabuf_limit = lim;
        const char *mbs = cfg_get("NX2NAVI_DMABUF_MB");
        int mb = mbs && *mbs ? atoi(mbs) : 64;
        span_size = (uint64_t)(mb > 0 ? mb : 64) << 20;
        if (span_size > dmabuf_limit) span_size = dmabuf_limit & ~0xfffull;
        if (span_size < (2ull << 20)) span_size = 2ull << 20;
        if (dmabuf_on)
            rt_log("mem: guest memory is a memfd's: CPU-visible pools are imported as dma-bufs of it, in spans of %llu MiB "
                   "(NX2NAVI_DMABUF_MB; udmabuf's limit %llu MiB)", (unsigned long long)(span_size >> 20),
                   (unsigned long long)(dmabuf_limit >> 20));
        else
            rt_log("mem: guest memory is a memfd's, but its pools get shadow copies: %s",
                   forced ? "NX2NAVI_SHADOW asks for them"
                   : !dmabuf_exts ? "the device cannot import dma-bufs (VK_KHR_external_memory_fd, VK_EXT_external_memory_dma_buf)"
                                  : small);
    } else if (*dmwhy) {
        rt_log("mem: guest memory is not a memfd's (NX2NAVI_DMABUF): %s: CPU-visible pools are %s, as with NX2NAVI_DMABUF=0", dmwhy,
               shadow_on ? "host-visible copies of guest memory (shadow storage)" : "host-pointer imports");
    }
#endif
    if (shadow_on) rt_log("mem: shadow storage: CPU-visible pools are host-visible copies of guest memory, not imports%s",
                          sh && *sh ? " (NX2NAVI_SHADOW)" : "");
    s_learn = shadow_on && rt_knob("SHADOW_TABLE_STORES", "0") == 1;
    s_check = shadow_on && cfg_bool("NX2NAVI_SHADOW_CHECK");
    if (shadow_on)
        rt_log("mem: what shaders store through the device address table into a shadow copy %s "
               "(NX2NAVI_SHADOW_TABLE_STORES)%s",
               s_learn ? "is kept through the copy-ins and written back to guest memory, from the second copy-in of its page on"
                       : "is overwritten by the next whole copy-in of its page",
               s_check ? "; NX2NAVI_SHADOW_CHECK counts the overwrites" : "");
    rt_log("mem: mode %s (%s): %s", mode == 2 ? "verify" : mode ? "mirror" : "import", why,
           mode ? "pools used as buffers get a device-local mirror" : "every CPU-visible pool is imported host memory");
    // Import is the integrated-GPU mode. On a discrete card every draw reads the pools over PCIe (the comment above
    // this section has the measurements), which is the difference between playable and 5 fps when it is not intended.
    if (!mode && discrete)
        rt_log("mem: warning: import on a discrete GPU: the GPU reads the game's pools over PCIe on every draw, far "
               "slower than mirror (the auto default on a discrete GPU); use memory = auto unless testing");
}

static void range_add(MRange **a, int *n, int *cap, PoolRec *r, uint64_t off, uint64_t size) {
    if (*n && (*a)[*n - 1].r == r && (*a)[*n - 1].off + (*a)[*n - 1].size == off) {
        (*a)[*n - 1].size += size;
        return;
    }
    if (*n == *cap) grow((void **)a, cap, sizeof **a);
    (*a)[(*n)++] = (MRange){r, off, size};
}

static int range_cmp(const void *x, const void *y) {
    const MRange *a = x, *b = y;
    if (a->r != b->r) return (uintptr_t)a->r < (uintptr_t)b->r ? -1 : 1;
    return a->off < b->off ? -1 : a->off > b->off;
}

// Records the ranges as one vkCmdCopyBuffer per pool, overlapping and adjacent ranges merged; to_mirror: buf -> mbuf,
// else mbuf -> buf. Returns the commands recorded; adds what they move to *bytes and *copies.
static int copy_ranges(VkCommandBuffer cb, MRange *a, int n, int to_mirror, uint64_t *bytes, uint64_t *copies) {
    qsort(a, (size_t)n, sizeof *a, range_cmp);
    int rec = 0;
    for (int i = 0; i < n;) {
        PoolRec *r = a[i].r;
        uint64_t lim = r->import_hi - r->import_lo;
        int k = 0;
        for (; i < n && a[i].r == r; i++) {
            uint64_t lo = a[i].off, hi = a[i].off + a[i].size;
            if (hi > lim) hi = lim;
            if (lo >= hi) continue;
            if (k && lo <= mregions[k - 1].srcOffset + mregions[k - 1].size) {
                uint64_t end = mregions[k - 1].srcOffset + mregions[k - 1].size;
                if (hi > end) mregions[k - 1].size += hi - end;
                continue;
            }
            if (k == cregions) grow((void **)&mregions, &cregions, sizeof *mregions);
            mregions[k++] = (VkBufferCopy){lo, lo, hi - lo};
        }
        if (!k) continue;
        if (to_mirror) vkCmdCopyBuffer(cb, r->buf, r->mbuf, (uint32_t)k, mregions);
        else vkCmdCopyBuffer(cb, r->mbuf, r->buf, (uint32_t)k, mregions);
        rec++;
        for (int j = 0; j < k; j++) *bytes += mregions[j].size;
        *copies += (uint64_t)k;
    }
    return rec;
}

static void mbarrier(VkCommandBuffer cb, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds,
                     VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                           .dstStageMask = ds, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

static void refuse(PoolRec *r, int why, const char *what) {
    r->mstate = MS_REFUSED;
    // A failed device-local allocation is asked again as a budget refusal is: memory that was short at the pool's
    // first use may be there 600 submissions later, and a pool never asked again was read over the bus until the game
    // finalized it. Untracked and overlapping pools stay refused: nothing about them changes.
    r->retry_sub = why == MR_BUDGET || why == MR_ALLOC ? (msub + RETRY_SUBS ? msub + RETRY_SUBS : 1) : 0;
    r->refused_why = (uint8_t)why;
    uint64_t n = r->import_hi - r->import_lo;
    static volatile int32_t logged[MR_N];   // four lines per reason: the refusals asked again would repeat theirs
    plat_inc64(&m_refused_n[why]);
    if (plat_inc32(&logged[why]) <= 4) {
        char f[160];
        flag_names(r->flags, f, sizeof f);
        rt_log("mem: pool %#llx+%#llx (%s) stays imported: %s", (unsigned long long)r->lo, (unsigned long long)r->size,
               f, what);
    }
    plat_add64(&m_refused_bytes[why], (int64_t)n);
}

// ---- eviction (NX2NAVI_MIRROR_EVICT, default on; 0 = a pool over the budget stays imported, as before)
// A pool whose mirror would not fit the VRAM budget stayed imported for good, however hot, while mirrors nothing had
// used for minutes kept their VRAM; draws that read an imported pool over PCIe bring a discrete GPU down to a few
// frames per second. Now a new mirror that does not fit takes the room of mirrors no submission synced for MIRROR_IDLE
// submissions, least recently synced first (mirror_evict), and only when they free enough; when they cannot, idle
// textures are asked to make the rest (vk_texture_want_room) and the pool asks again RETRY_SOON submissions later, once
// per refusal series; after that, and for any other pool refused for the budget, every RETRY_SUBS submissions
// (vk_mem_use). Asked every RETRY_SOON submissions without end, 113 refused pools made 1,795 retries in 6,000 frames. Mirrors never go to make room for textures: evicted
// under VRAM pressure during a menu, 395 of them were refused again when the world came back and the game ran at 4 fps.
// The budget counts the free space inside VMA's blocks as room (vk_mem_vram_live).
// A victim was neither synced, used nor written back in the submission being recorded (it has nothing in the copy,
// write-back or snapshot lists), and every GPU write to it went back to guest memory at the end of the submission that
// made it (vk_mem_mirror_end), so the imported buffer holds everything. The device address table moves its pages back
// to the imported buffer (vk_gpa_remap_plain); a shader of a submission in flight that reads the old record still finds
// the mirror, destroyed only once the submission being recorded is done (vk_defer_buffer). A mirror without a twin
// (twin_wanted) is never a victim: what shaders wrote into it through the table is in the mirror alone. The pool is
// mirrored again, from a whole copy, on its next buffer use. Its freed bytes count as room at once (evict_room), until
// the GPU is past the submission and the destroy shows in VMA's figures.
#define MIRROR_IDLE 120
#define MIRROR_SHARE 2   // the mirrors' share of the budget that the budget check never refuses: one in this many
#define RETRY_SOON 16
static int mirror_evict_on = -1;
static int64_t m_evicted_n, m_evicted_bytes, m_retries;
static volatile int64_t m_over_n, m_over_bytes;   // mirrors made although the budget check failed (mirror_create)
static uint64_t room_bytes, room_fence;   // replay: bytes evicted that the budget may still count, until room_fence

static int evict_enabled(void) {
    if (mirror_evict_on < 0) {
        mirror_evict_on = rt_knob("MIRROR_EVICT", "1");
        rt_log("mem: idle mirrors %s (NX2NAVI_MIRROR_EVICT=%d)", mirror_evict_on ? "make room for new ones and for textures under VRAM pressure"
                                                                               : "stay; a pool over the VRAM budget stays imported", mirror_evict_on);
    }
    return mirror_evict_on;
}

static uint64_t evict_room(void) {
    if (room_bytes && vk_sync_fence_done() >= room_fence) room_bytes = 0;
    return room_bytes;
}

// Replay thread: evicts mirrors idle for at least min_idle submissions, least recently synced first, until need bytes
// are freed; none when the candidates cannot free that much. Returns the bytes freed.
static uint64_t mirror_evict(uint64_t need, uint32_t min_idle, const char *why) {
    enum { MAXV = 64 };
    PoolRec *v[MAXV];
    int nv = 0;
    plat_mutex_lock(&pool_lock);
    for (int i = 0; i < nm; i++) {   // the MAXV least recently synced candidates, oldest first
        PoolRec *r = mlist[i];
        uint32_t age = msub - r->acc_sub;
        if (age < min_idle || r->wb_sub == msub || msub - r->wb_sub < 2) continue;
        if (!r->twin && r->gpa_rec) continue;   // the table's writes are in the mirror alone
        if (r->wb_kept) continue;               // so are the bytes wb_skip kept from guest memory
        int at = nv;
        while (at > 0 && msub - v[at - 1]->acc_sub < age) at--;
        if (at == MAXV) continue;
        if (nv < MAXV) nv++;
        memmove(v + at + 1, v + at, sizeof *v * (size_t)(nv - 1 - at));
        v[at] = r;
    }
    uint64_t can = 0;   // what all the candidates would free: short of need, none goes (no mirror lost for nothing)
    for (int i = 0; i < nv; i++) can += v[i]->import_hi - v[i]->import_lo;
    if (can < need) nv = 0;
    uint64_t freed = 0;
    uint32_t idle_for[MAXV];
    VkBuffer dead[MAXV];
    VmaAllocation dead_a[MAXV];
    int n = 0;
    // All under the exclusive lock: vk_pool_fini, on another thread, takes a pool off the list under it, so a victim is
    // still a live pool here, and a finalize that follows finds it imported, its table pages on the imported buffer.
    for (; n < nv && freed < need; n++) {
        PoolRec *r = v[n];
        idle_for[n] = msub - r->acc_sub;
        mem_lazy_release(r->import_lo, r->import_hi - r->import_lo);   // what only the mirror holds comes down first
        mirror_forget(r);   // off the list, mirror_gen bumped: no buffer use gets the mirror from here on
        r->mstate = MS_NONE, r->acc_sub = 0, r->retry_sub = 0;
        dead[n] = r->mbuf, dead_a[n] = r->malloc_;
        r->mbuf = VK_NULL_HANDLE, r->malloc_ = NULL;
        plat_mutex_lock(&flush_lock);   // flushes not copied yet: the next mirror starts from a whole copy
        __atomic_store_n(&r->fl_n, 0, __ATOMIC_RELEASE);
        r->fl_calls = r->fl_sum = 0;
        plat_mutex_unlock(&flush_lock);
        if (r->gpa_rec) r->gpa_rec = vk_gpa_remap_plain(r->import_lo, r->import_hi, r->gpa_rec, r->buf, r->import_lo);
        freed += r->import_hi - r->import_lo;
        if (m_evicted_n + n < 8)
            rt_log("mem: mirror of pool %#llx+%#llx (%.1f MiB) evicted, idle for %u submissions: %s", (unsigned long long)r->lo,
                   (unsigned long long)r->size, (r->import_hi - r->import_lo) / 1048576.0, idle_for[n], why);
    }
    plat_mutex_unlock(&pool_lock);
    for (int i = 0; i < n; i++) vk_defer_buffer(dead[i], dead_a[i]);   // after the submission being recorded
    if (n) {
        plat_add64(&m_evicted_n, n);
        plat_add64(&m_evicted_bytes, (int64_t)freed);
        room_bytes += freed, room_fence = vk_sync_issued() + 2;
    }
    return freed;
}

// ---- twins (NX2NAVI_GPA_TWIN_MAX_KB, default 0: every mirror has one)
// Shaders write through the device address table into both a mirror and its pool's imported buffer, guest memory
// (vk_gpa.c, "The twin"): such writes are not recorded as GPU writes, so with the mirror alone the CPU never saw
// them and a sync of a page the CPU wrote copied the stale guest page over them. On a discrete GPU the second write
// crosses PCIe: on PCIe 3.0 the twins cost about 3 ms of GPU time a frame in Kokiri Forest (NX2NAVI_GPA_TWIN=0 against
// on). The writes that went wrong without them were the foliage counts and records in pools of 20 and 44 KiB.
// GPA_TWIN_MAX_KB=n gives a twin only to mirrors of pools up to n KiB; a larger mirror keeps the table's writes to
// itself (its record names the mirror twice, which sends the second write to the table's scratch page) and its
// buffer fills stay in the mirror (vk_mem_guest_twin). Such a mirror is never evicted (mirror_evict), since guest
// memory lacks what the shaders wrote into it. For measuring which pools need a twin: a pool that does and is left
// without draws stretched or flickering foliage.
static int64_t twin_max = -1;          // bytes; 0 no limit
static int64_t m_untwinned_n, m_untwinned_bytes;

static int twin_wanted(const PoolRec *r) {
    if (twin_max < 0) {
        int kb = cfg_int("NX2NAVI_GPA_TWIN_MAX_KB");
        twin_max = kb > 0 ? (int64_t)kb << 10 : 0;
        if (twin_max)
            rt_log("mem: only mirrors of pools up to %d KiB send the device address table's writes to guest memory "
                   "too (NX2NAVI_GPA_TWIN_MAX_KB); larger ones keep them and are not evicted", kb);
    }
    return !twin_max || (int64_t)(r->import_hi - r->import_lo) <= twin_max;
}

// ---- when the device address table takes up a new mirror (NX2NAVI_GPA_LATE, default 1; 0 = at once, as before)
// A mirror is made while the replay records a part of a submission, and its first copy of the pool is in that part's
// prologue. The table is memory the shaders read as they run, so a change of it made here, at record time, reaches
// every part already handed to the queue (the submissions before, and the earlier parts of this one), which run
// before that copy: they read the mirror through the table, memory nothing had written yet, and wrote into it, and on
// a card over its budget the block was one the driver had not made resident for them. A GTX 1650 4 GB lost its device
// that way at area loads in every session: six reports of a GPU read fault at byte 0 of a mirror made in the faulting
// submission, each the first byte of a new 2 MiB-aligned block, with mirrors being made over the budget; with the
// table kept empty the same routes ran without a loss.
// So the table is not touched here. The prologue that holds the mirror's first copy also holds, right after it, a
// store into the pool's record (vk_gpa_flip_cmd): the GPU runs it in its place in the queue, so the parts before it
// read and write guest memory through the table as if no mirror existed, and the parts from this one on read the
// mirror and write both. What the earlier parts wrote through the table is in guest memory when the first copy reads
// it there (a twinned pool's first copy is always the GPU's own copy from the imported buffer, mirror_capture), so
// the mirror misses none of it, and no shader reads through the table a state older than the one its place in the
// queue gives it.
static int n_gpa_flip;                 // as pend: mirrors whose record the next prologue hands over (PoolRec.gpa_flip)
static uint64_t n_gpa_late;            // replay: records handed to their mirror in queue order, so far

static int gpa_late(void);
int vk_mem_gpa_late(void) { return gpa_late(); }   // --probe-vkmem

static int gpa_late(void) {
    static int on = -1;
    if (on < 0) {
        on = rt_knob("GPA_LATE", "1") == 1;
        rt_log("mem: the device address table takes up a new VRAM mirror %s (NX2NAVI_GPA_LATE)",
               on ? "in queue order, after the mirror's first copy" : "at once, as before");
    }
    return on;
}

static void mbarrier(VkCommandBuffer cb, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds,
                     VkAccessFlags2 da);

// Prologue, after the copies into the mirrors: the stores that hand the new mirrors' records over. Everything queued
// before is done with the table before the stores run, and everything after sees them.
static void gpa_flip_cmds(VkCommandBuffer cb) {
    if (!n_gpa_flip) return;
    mbarrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
             VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    plat_mutex_lock(&pool_lock);
    for (int i = 0; i < nm && n_gpa_flip; i++) {
        PoolRec *r = mlist[i];
        if (!r->gpa_flip) continue;
        r->gpa_flip = 0, n_gpa_flip--;
        if (r->gpa_rec && r->mbuf && vk_gpa_flip_cmd(cb, r->gpa_rec, r->mbuf, r->import_lo, r->twin)) n_gpa_late++;
    }
    n_gpa_flip = 0;
    plat_mutex_unlock(&pool_lock);
    mbarrier(cb, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
             VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}

// Replay thread: the pool's mirror on its first buffer use.
static void mirror_create(PoolRec *r) {
    VkCtx *c = vk_ctx();
    uint64_t n = r->import_hi - r->import_lo;
    r->mstate = MS_REFUSED;
    __atomic_add_fetch(&mirror_gen, 1, __ATOMIC_RELEASE);
    if (r->kind != POOL_IMPORT || !r->buf || !n) return;
    const uint64_t lo = r->lo;   // the index is searched by it again when the mirror joins the list
    plat_mutex_lock_shared(&pool_lock);
    int i = pidx_upper(&pool_index, lo) - 1;
    while (i >= 0 && pool_index.e[i].buf != r) i--;
    int alone = i >= 0 && (i == 0 || pool_index.maxhi[i - 1] <= pool_index.e[i].lo) &&
                (i + 1 == pool_index.n || pool_index.e[i + 1].lo >= pool_index.e[i].hi);
    plat_mutex_unlock_shared(&pool_lock);
    if (i < 0) return;   // finalized meanwhile
    if (!alone) return refuse(r, MR_OVERLAP, "it overlaps another pool");
    uint64_t r0 = r->import_lo >> SNAP_SHIFT, r1 = (r->import_hi - 1) >> SNAP_SHIFT;
    if (r1 >= SNAP_REGIONS) return refuse(r, MR_UNTRACKED, "outside the write-watched address space");
    uint64_t used, budget;
    int exact;
    int evict = evict_enabled();
    if (evict ? vk_mem_vram_live(&used, &budget) : vk_mem_vram(&used, &budget, &exact)) {
        if (evict) {
            uint64_t room = evict_room();
            used = used > room ? used - room : 0;
        }
        if (used + n + budget / 8 > budget && evict) {   // room from mirrors nothing synced for a while
            char why[96];
            snprintf(why, sizeof why, "room for a %.1f MiB mirror", n / 1048576.0);
            uint64_t f = mirror_evict(used + n + budget / 8 - budget, MIRROR_IDLE, why);
            used = used > f ? used - f : 0;
        }
        // Over the budget still. The mirrors keep their place up to MIRROR_SHARE of the budget whatever else fills the
        // VRAM: on a 6 GiB card a loaded area's textures alone pass the budget before the game's buffers are first
        // used, every pool was refused, and the GPU read vertices, indices and storage over PCIe at each draw: 70 to
        // 250 ms of GPU time a frame, 5 to 13 fps, in two reports from RTX 3060 Laptop cards. A mirror the driver cannot
        // keep in VRAM is paged to system memory, where the imported pool already is, so a mirror never reads slower
        // than its pool, and what is drawn from every frame is not what the system pages out first. The textures are
        // still asked for the room. Past that share a pool is refused as before.
        if (used + n + budget / 8 > budget) {
            uint64_t over = used + n + budget / 8 - budget;
            if (evict && (uint64_t)m_live_bytes + n <= budget / MIRROR_SHARE) {
                vk_texture_want_room(over);
                plat_inc64(&m_over_n);
                plat_add64(&m_over_bytes, (int64_t)n);
                if (m_over_n <= 4 || !(m_over_n & 255))
                    rt_log("mem: mirror of %.1f MiB made over the VRAM budget (%.0f of %.0f MiB in use, mirrors %.0f MiB): "
                           "%lld such so far, %.0f MiB; idle textures are asked for the room",
                           n / 1048576.0, used / 1048576.0, budget / 1048576.0, m_live_bytes / 1048576.0,
                           (long long)m_over_n, m_over_bytes / 1048576.0);
            } else {
                char why[128];
                snprintf(why, sizeof why, "its %.1f MiB mirror does not fit the VRAM budget (%.0f of %.0f MiB in use)",
                         n / 1048576.0, used / 1048576.0, budget / 1048576.0);
                refuse(r, MR_BUDGET, why);
                if (evict && !r->asked_room) {   // idle textures make the rest; asked again once they had the time
                    vk_texture_want_room(over);
                    r->retry_sub = msub + RETRY_SOON ? msub + RETRY_SOON : 1;
                    r->asked_room = 1;   // later refusals wait RETRY_SUBS: the textures had nothing to give
                }
                return;
            }
        }
    }
    plat_mutex_lock(&gen_lock);
    mrep.sync_queries += snap_regions(r0, r1, mepoch);
    int untracked = 0;
    for (uint64_t k = r0; k <= r1; k++) untracked |= snap_untracked[k];
    r->mgen = gen_now;   // the whole-range copy below holds every write observed so far
    plat_mutex_unlock(&gen_lock);
    if (untracked) return refuse(r, MR_UNTRACKED, "its range is not write-watched");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = pool_usage(c),
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                  .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    VkResult res = vmaCreateBuffer(c->vma, &bi, &ai, &r->mbuf, &r->malloc_, NULL);
    if (res != VK_SUCCESS) {
        r->mbuf = VK_NULL_HANDLE, r->malloc_ = NULL;
        char why[96];
        snprintf(why, sizeof why, "device-local allocation of %.1f MiB failed (%d)", n / 1048576.0, (int)res);
        return refuse(r, MR_ALLOC, why);
    }
    plat_mutex_lock(&pool_lock);
    // The pool may have been finalized on another thread since the check above, without a mirror to forget (its state
    // said refused meanwhile): on the list, the mirror would outlive its record and be freed twice, or never. Its
    // buffer goes once the GPU is past the submission being recorded, which nothing of it has reached.
    int k = pidx_upper(&pool_index, lo) - 1;
    while (k >= 0 && pool_index.e[k].buf != r) k--;
    if (k < 0) {
        plat_mutex_unlock(&pool_lock);
        vk_defer_buffer(r->mbuf, r->malloc_);
        r->mbuf = VK_NULL_HANDLE, r->malloc_ = NULL;
        return;
    }
    if (nm == mcap) grow((void **)&mlist, &mcap, sizeof *mlist);
    int at = nm;
    while (at > 0 && mlist[at - 1]->import_lo > r->import_lo) at--;
    memmove(mlist + at + 1, mlist + at, (size_t)(nm - at) * sizeof *mlist);
    mlist[at] = r;
    nm++;
    plat_mutex_unlock(&pool_lock);
    r->acc_sub = msub;
    r->mstate = MS_ON;
    r->asked_room = 0;
    // The device address table names the mirror: the GPU's writes go there, and the sync copies the CPU's. With a
    // twin they also go to the imported buffer; without one (twin_wanted) the record names the mirror twice. The
    // table takes the mirror up in queue order, after its first copy (gpa_late, above).
    r->twin = (uint8_t)twin_wanted(r);
    if (!r->twin) {
        plat_inc64(&m_untwinned_n);
        plat_add64(&m_untwinned_bytes, (int64_t)n);
    }
    if (r->gpa_rec && gpa_late()) {
        plat_mutex_lock(&pool_lock);   // as mirror_forget and gpa_flip_cmds, which count it down
        r->gpa_flip = 1, n_gpa_flip++;   // vk_mem_mirror_prologue, with the copy range_add queues below
        plat_mutex_unlock(&pool_lock);
    } else if (r->gpa_rec) {
        plat_mutex_lock(&pool_lock);
        r->gpa_rec = r->twin ? vk_gpa_remap(r->import_lo, r->import_hi, r->gpa_rec, r->mbuf, r->import_lo)
                             : vk_gpa_remap_plain(r->import_lo, r->import_hi, r->gpa_rec, r->mbuf, r->import_lo);
        plat_mutex_unlock(&pool_lock);
    }
    range_add(&pend, &npend, &cpend, r, 0, n);
    mrep.first_n++, mrep.first_bytes += n;
    plat_inc64(&m_live_n);
    plat_add64(&m_live_bytes, (int64_t)n);
}

static void mirror_forget(PoolRec *r) {
    __atomic_add_fetch(&mirror_gen, 1, __ATOMIC_RELEASE);
    if (r->gpa_flip) r->gpa_flip = 0, n_gpa_flip--;   // gone before its first copy: its record stays as it is
    for (int i = 0; i < nm; i++)
        if (mlist[i] == r) {
            memmove(mlist + i, mlist + i + 1, (size_t)(nm - i - 1) * sizeof *mlist);
            nm--;
            plat_dec64(&m_live_n);
            plat_add64(&m_live_bytes, -(int64_t)(r->import_hi - r->import_lo));
            break;
        }
}

// Finalized pools waiting for pool_retire (pool_fini says which); vk_mem_shutdown destroys those whose callback
// never ran.
static PoolRec **retiring;
static int nretiring, cretiring;
static PlatMutex retire_lock = PLAT_MUTEX_INIT;

static void pool_retiring(PoolRec *r) {
    plat_mutex_lock(&retire_lock);
    if (nretiring == cretiring) grow((void **)&retiring, &cretiring, sizeof *retiring);
    retiring[nretiring++] = r;
    plat_mutex_unlock(&retire_lock);
}

static void pool_retire(void *arg) {
    plat_mutex_lock(&retire_lock);
    for (int i = 0; i < nretiring; i++)
        if (retiring[i] == arg) { retiring[i] = retiring[--nretiring]; break; }
    plat_mutex_unlock(&retire_lock);
    destroy_rec(vk_ctx(), arg);
}

// Replay thread, the first access of this submission: queues the copies of the pages written since the last sync.
static void mirror_sync(PoolRec *r) {
    uint64_t t0 = qpc();
    r->acc_sub = msub;
    pm_note(r);
    uint64_t lo = r->import_lo, hi = r->import_hi;
    uint64_t r0 = lo >> SNAP_SHIFT, r1 = (hi - 1) >> SNAP_SHIFT;
    int excl = 0;
    plat_mutex_lock_shared(&gen_lock);
    for (uint64_t k = r0; k <= r1; k++)
        if (snap_at[k] != mepoch) { excl = 1; break; }
    if (excl) {
        plat_mutex_unlock_shared(&gen_lock);
        plat_mutex_lock(&gen_lock);
        mrep.sync_queries += snap_regions(r0, r1, mepoch);
    }
    uint32_t since = r->mgen;
    int untracked = 0;
    for (uint64_t k = r0; k <= r1; k++) {
        untracked |= snap_untracked[k];
        if (creg_max[k] <= since) continue;
        uint64_t a = k << SNAP_SHIFT, b = a + (1ull << SNAP_SHIFT);
        if (a < lo) a = lo;
        if (b > hi) b = hi;
        const uint32_t *ch = cgen_chunk[a >> GEN_CHUNK_SHIFT];   // a 2 MiB region lies in one chunk
        if (!ch) continue;
        for (uint64_t x = a; x < b; x += 1ull << GEN_PAGE_SHIFT)
            if (ch[(x >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)] > since) {
                range_add(&pend, &npend, &cpend, r, x - lo, 1ull << GEN_PAGE_SHIFT), mrep.page_bytes += 1ull << GEN_PAGE_SHIFT;
                r->st_pages += 1ull << GEN_PAGE_SHIFT, r->hot_bytes += 1ull << GEN_PAGE_SHIFT, r->hot_up += 1ull << GEN_PAGE_SHIFT;
            }
    }
    r->mgen = gen_now;
    if (excl) plat_mutex_unlock(&gen_lock);
    else plat_mutex_unlock_shared(&gen_lock);
    if (__atomic_load_n(&r->fl_n, __ATOMIC_ACQUIRE)) {
        FlushRange fl[FLUSH_RANGES + 1];
        plat_mutex_lock(&flush_lock);
        int n = r->fl_n;
        memcpy(fl, r->fl, (size_t)n * sizeof *fl);
        uint64_t calls = r->fl_calls, sum = r->fl_sum;
        r->fl_n = 0, r->fl_calls = r->fl_sum = 0;
        plat_mutex_unlock(&flush_lock);
        mrep.flush_calls += calls, mrep.flush_sum += sum;
        r->hot_up += sum;   // the CPU wrote them (mirror_hot)
        for (int i = 0; i < n; i++) {
            uint64_t a = fl[i].lo > lo ? fl[i].lo : lo, b = fl[i].hi < hi ? fl[i].hi : hi;
            if (a >= b) continue;
            if (!untracked && !flush_upload) {   // the write watch saw every write in them: the pages above
                mrep.flush_skipped += b - a;
                continue;
            }
            range_add(&pend, &npend, &cpend, r, a - lo, b - a);
            mrep.flush_n++, mrep.flush_bytes += b - a, r->st_flush += b - a;
            if ((rt_trace & RT_TRACE_PERF) && b - a >= 16u << 20)
                rt_log("mem: mirror frame %llu: flush range %.1f MiB at +%.1f MiB of a %.1f MiB pool (%d ranges from %llu "
                       "flushes of %.3f MiB)", (unsigned long long)mirror_frame, (b - a) / 1048576.0, (a - lo) / 1048576.0,
                       (hi - lo) / 1048576.0, n, (unsigned long long)calls, sum / 1048576.0);
        }
    }
    mrep.syncs++, r->st_syncs++;
    mrep.sync_t += qpc() - t0;
}

// ---- shadow storage: the copy-ins before a part is submitted, and the device address table's readers
int vk_mem_shadow_storage(void) { return shadow_on; }
static int shadow_check_on(void) { return s_check; }
int vk_mem_shadow_table_stores(void) { return s_learn; }

static void shadow_table_add(PoolRec *r) {
    plat_mutex_lock(&shadow_lock);
    if (!r->stab_i) {
        if (nstab == cstab) grow((void **)&stab, &cstab, sizeof *stab);
        stab[nstab++] = r;
        r->stab_i = (uint32_t)nstab;
    }
    plat_mutex_unlock(&shadow_lock);
}

void vk_mem_shadow_table_read(void) {
    if (shadow_on) __atomic_store_n(&stab_want, vk_texture_epoch(), __ATOMIC_RELAXED);
}

// gen_lock held: nothing of the pool was stamped since its shadow's last copy-in. Its regions have this epoch's
// write-watch snapshot, none of them is untracked, and none holds a stamp newer than the shadow's sgen, which then
// stays as it is. With cpu the stamps are the CPU-side tables', as shadow_sync_pool reads them. The table's sync asks
// this of every shadow in the table before it runs shadow_sync_pool with its own timer reads, snapshot walk and
// region loop: outdoors about 3,800 pools are in the table and a few hundred were written.
static int shadow_pool_clean(const PoolRec *r, uint32_t epoch, int cpu) {
    uint64_t r0 = r->import_lo >> SNAP_SHIFT, r1 = (r->import_hi - 1) >> SNAP_SHIFT;
    if (!epoch || r1 >= SNAP_REGIONS) return 0;
    const uint32_t *mx = cpu ? creg_max : reg_max;
    for (uint64_t k = r0; k <= r1; k++)
        if (snap_at[k] != epoch || snap_untracked[k] || mx[k] > r->sgen) return 0;
    return 1;
}

void vk_mem_shadow_sync(uint64_t fence) {
    if (!shadow_on) return;
    uint64_t t0 = qpc();
    uint32_t epoch = vk_texture_epoch();
    int cpu = vk_mirror_on && wb_no_reup();   // before the locks: its first call logs
    // The table's readers: the record of this guest submission bound the table, and no part of it has synced yet.
    // The callers are one at a time (vk_sync.c prologue under the submission lock, and the probe).
    static uint32_t tab_epoch;    // the guest submission the table's shadows were last synced for
    static uint64_t wb_fence;     // the last fence that got the write-back job
    int table = epoch && epoch != tab_epoch && __atomic_load_n(&stab_want, __ATOMIC_RELAXED) == epoch;
    if (table) {
        tab_epoch = epoch;
        // What this submission's shaders store through the table goes back to guest memory at its fence, once
        // (table_wb_cb). Asked before the copy-ins below: the sync whose copy-ins find the first such page leaves it
        // to the next guest submission that binds the table.
        if (s_learn && fence && fence != wb_fence && __atomic_load_n(&ntpools, __ATOMIC_RELAXED)) {
            wb_fence = fence;
            vk_completion_add(fence, table_wb_cb, NULL);
        }
    }
    plat_mutex_lock(&shadow_lock);
    if (nstouch || (table && nstab)) {
        plat_mutex_lock(&gen_lock);
        for (int i = 0; i < nstouch; i++) shadow_sync_pool(stouch[i], epoch, cpu);
        uint64_t t1 = qpc();
        for (int i = 0; table && i < nstab; i++) {   // the shadows the replay did not resolve
            PoolRec *r = stab[i];
            if (r->stouched) continue;
            uint64_t r0 = r->import_lo >> SNAP_SHIFT, r1 = (r->import_hi - 1) >> SNAP_SHIFT;
            if (r1 < SNAP_REGIONS) s_queries += snap_regions(r0, r1, epoch);   // so that shadow_pool_clean can tell
            if (!shadow_pool_clean(r, epoch, cpu)) shadow_sync_pool(r, epoch, cpu), s_syncs++;
        }
        for (int i = 0; i < nstouch; i++) stouch[i]->stouched = 0;
        plat_mutex_unlock(&gen_lock);
        s_syncs += (uint64_t)nstouch;
        nstouch = 0;
        // A shadow with a VRAM mirror: the table names the mirror, which takes the same pages unless this guest
        // submission synced it already (a pool bound as a buffer). The uploads read the shadows just brought up to
        // date, and the prologue records them ahead of the part, after the write-backs of the submissions before it.
        // No mirror is made for a reader of the table.
        for (int i = 0; table && i < nstab; i++)
            if (stab[i]->mstate == MS_ON && stab[i]->acc_sub != msub) mirror_sync(stab[i]);
        if (table) s_tab_ticks += qpc() - t1, s_tab_syncs++;
    }
    s_ticks += qpc() - t0;
    plat_mutex_unlock(&shadow_lock);
}

// The replay workers (vk_exec.c) bind and write from several threads at once: while a chunk records, a mirror's first
// use in a submission (creation, sync) and the notes of GPU writes go under use_lock. A use the submission already
// synced takes no lock, nor does the replay outside a parallel group (it alone records then).
static PlatMutex use_lock = PLAT_MUTEX_INIT;

static void mem_use_locked(VkMemSpan *sp);
void vk_mem_use(VkMemSpan *sp) {
    PoolRec *r = sp->pool;
    if (r->mstate == MS_ON && r->acc_sub == msub) {
        r->vd_use_sub = msub;
        sp->buffer = r->mbuf;
        return;
    }
    // a refused pool not due for another try stays imported: nearly every pool in the field (two have mirrors)
    if (r->mstate == MS_REFUSED && !(r->retry_sub && (int32_t)(msub - r->retry_sub) >= 0)) {
        vk_mem_note_live();   // the GPU reads this pool's guest memory as it runs (vk_mem_take_live)
        return;
    }
    int lk = vk_exec_shared();
    if (lk) plat_mutex_lock(&use_lock);
    mem_use_locked(sp);
    if (lk) plat_mutex_unlock(&use_lock);
}

static void mem_use_locked(VkMemSpan *sp) {
    PoolRec *r = sp->pool;
    if (r->mstate == MS_REFUSED && r->retry_sub && (int32_t)(msub - r->retry_sub) >= 0 && evict_enabled()) {
        plat_dec64(&m_refused_n[r->refused_why]);   // counted again if refused again
        plat_add64(&m_refused_bytes[r->refused_why], -(int64_t)(r->import_hi - r->import_lo));
        plat_inc64(&m_retries);
        r->mstate = MS_NONE, r->retry_sub = 0;   // refuse sets it again
    }
    if (r->mstate == MS_NONE) mirror_create(r);
    if (r->mstate != MS_ON) {   // the GPU reads this pool's guest memory as it runs (staged mirror uploads)
        vk_mem_note_live();
        return;
    }
    if (r->acc_sub != msub) mirror_sync(r);
    r->vd_use_sub = msub;
    sp->buffer = r->mbuf;
}

void vk_mem_pick(VkMemSpan *sp) {
    PoolRec *r = sp->pool;
    if (!r) return;
    if (r->mstate != MS_ON) {   // guest memory itself, read by the GPU when it runs (vk_mem_take_live)
        vk_mem_note_live();
        return;
    }
    if (r->acc_sub != msub) {
        int lk = vk_exec_shared();
        if (lk) plat_mutex_lock(&use_lock);
        if (r->acc_sub != msub) mirror_sync(r);
        if (lk) plat_mutex_unlock(&use_lock);
    }
    sp->buffer = r->mbuf;
}

VkBuffer vk_mem_guest_twin(const VkMemSpan *sp) {
    const PoolRec *r = sp->pool;
    return r && r->mstate == MS_ON && r->twin && sp->buffer == r->mbuf ? r->buf : VK_NULL_HANDLE;
}

int vk_mem_mirror_probe(uint64_t addr) {
    VkMemSpan sp;
    if (!vk_mirror_on || !vk_mem_resolve(addr, 1, &sp) || !sp.pool) return 0;
    mepoch = vk_texture_epoch();
    vk_mem_use(&sp);
    return ((PoolRec *)sp.pool)->mstate == MS_ON;
}

// ---- write-back merge
// A storage buffer a shader writes whole records of does not
// go back to guest memory as a plain copy. The CPU may write fields of the same records while the GPU runs: NX2navi's
// particle emitters write a new particle's whole record into a free slot of a buffer that the simulation shaders update
// (the waterfall splashes and mist, 2026-11-02), and a plain copy puts the mirror's stale copy of those words back over
// them. Instead:
// - record time (gpu_wrote from vk_mem_gpu_wrote_shader): each distinct range of the submission gets room in a snapshot
//   chunk (snap_add);
// - each part's prologue, after the CPU -> mirror copies: the mirror's words of the ranges the part added are copied
//   into their snapshots (snap_copies), the state before the part's GPU work. Words of an earlier part's writes that a
//   later range overlaps are covered by the earlier range's own snapshot;
// - vk_mem_mirror_end: shaders/wb_merge.comp writes each 32-bit word that differs between the mirror and the snapshot
//   into the pool's imported buffer (wb_merge_record), one dispatch per pool and group of ranges.
// Words the GPU left unchanged keep the CPU's value; a word both changed takes the GPU's, as a plain copy would.
// NX2NAVI_WB_MERGE=0 restores the plain copies; so does a pipeline or allocation failure.
#define SNAP_CHUNK (1u << 20)
#define MERGE_SPAN (64ull << 20)   // the bytes of one pool a merge dispatch binds at most
#define MERGE_WORDS (65535u * 64u) // the words one merge dispatch covers at most
// Larger written ranges stay plain copies (NX2NAVI_WB_MERGE_MAX, MiB, default 32). A plain copy writes the whole range
// over PCIe at the end of each submission even when the shader changed a few words; a merge copies it within video
// memory and writes only the words that changed. With a 4 MiB limit the field ran about 46 MiB of plain write-back a
// frame in ranges of about 5.5 MiB, just above it; at 16 the same outdoor route went from a median 47.5 to 67.3 fps and
// from 11.3 to 7.2 ms of GPU time a frame (RTX 4080 SUPER, 120 fps limit, 2026-11-08). Kokiri Forest writes seven
// storage buffer ranges of 4-15 MiB a frame; at 32, merged in MERGE_PIECE parts so that no dispatch covers more than
// MERGE_WORDS, the plain copies of about 4 ms of GPU time a frame on PCIe 3.0 went (RTX 4090, 2026-11-09).
#define MERGE_PIECE (4ull << 20)
#define MERGE_MAX_LIMIT (256ull << 20)   // the snapshot pool's largest size (snap_pool_cap)
static uint64_t merge_max(void) {
    static uint64_t m;
    if (!m) {
        const char *e = cfg_get("NX2NAVI_WB_MERGE_MAX");
        uint64_t mib = e && *e ? (uint64_t)strtoull(e, NULL, 10) : 32;
        m = mib << 20;
        if (!m) m = 1;
        if (m > MERGE_MAX_LIMIT) m = MERGE_MAX_LIMIT;
        rt_log("mem: write-back merge covers ranges up to %.1f MiB (NX2NAVI_WB_MERGE_MAX)", m / 1048576.0);
    }
    return m;
}
typedef struct { PoolRec *r; uint64_t off, size, soff; int chunk; } SnapRange;
typedef struct { VkBuffer buf; VmaAllocation alloc; uint64_t size, used; int host; } SnapChunk;
typedef struct { int n; SnapChunk c[]; } SnapFree;   // a submission's chunks and range lists, pooled at its fence
static SnapRange *snl;              // replay: this submission's merged ranges; [0, snl_copied) have their snapshot copy
static int nsnl, csnl, snl_copied;
static SnapChunk *chunks;           // replay: this submission's snapshot chunks and merge range lists
static int nchunks, cchunks;
// Chunks a finished submission used (snap_free) wait here for chunk_new, so a frame with merges creates no buffer once
// the pool holds the frames in flight: a VMA allocation that needs a new memory block costs 8-11 ms on the replay
// thread. Chunk sizes are powers of two so that one fits the next request; what the pool cannot hold is destroyed. The
// pool holds about three frames of Kokiri Forest's snapshots (65 MiB a frame); at 16 MiB it held less than one frame's
// and two buffers a frame were created and destroyed again (2026-11-09).
#define SNAP_LIST_MIN (64u << 10)
#define SNAP_POOL_N 128
// The pool holds up to 1/64 of the VRAM budget, between 64 and 256 MiB: room for merges of multi-MiB ranges across the
// submissions in flight. Its chunks prefer device memory and are not evictable, so a small card keeps less of them.
static uint64_t snap_pool_cap(void) {
    static uint64_t cap;
    if (!cap) {
        uint64_t used = 0, budget = 0;
        int exact = 0;
        uint64_t c = vk_mem_vram(&used, &budget, &exact) && budget ? budget / 64 : (64ull << 20);
        if (c < (64ull << 20)) c = 64ull << 20;
        if (c > (256ull << 20)) c = 256ull << 20;
        cap = c;
        rt_log("mem: write-back snapshot pool keeps up to %llu MiB", (unsigned long long)(cap >> 20));
    }
    return cap;
}
static PlatMutex snap_pool_lock = PLAT_MUTEX_INIT;
static SnapChunk snap_pool[SNAP_POOL_N];
static int nsnap_pool;
static uint64_t snap_pool_bytes;
static int merge_on = -1;
static struct {
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
} WM;

static int merge_init(VkCtx *c) {
    WM.tried = 1;
    WM.push = (PFN_vkCmdPushDescriptorSetKHR)vk_dpa(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b[5];
    for (int i = 0; i < 5; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 5, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof wb_merge_spv,
                                   .pCode = wb_merge_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (!WM.push || vkCreateDescriptorSetLayout(c->device, &dci, NULL, &WM.dsl) != VK_SUCCESS) goto fail;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &WM.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &WM.layout) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS)
        goto fail;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = WM.layout};
    VkResult res = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &WM.pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (res != VK_SUCCESS) goto fail;
    return WM.ok = 1;
fail:
    rt_log("mem: write-back merge off: its pipeline could not be created");
    return 0;
}

static int merging(void) {
    if (merge_on < 0) {
        merge_on = cfg_bool("NX2NAVI_WB_MERGE");
        rt_log("mem: write-back merge %s (NX2NAVI_WB_MERGE)", merge_on ? "on" : "off");
    }
    if (!merge_on) return 0;
    if (!WM.tried) merge_init(vk_ctx());
    return WM.ok;
}

// A chunk of at least size bytes in chunks[]: the smallest fitting one of the pool, else a new buffer; -1 when the
// allocation fails. host: 0 device memory (snapshots), CHUNK_LIST host memory the CPU writes (range lists),
// CHUNK_MARKS host memory the CPU reads back (the merge's page marks, shadow_wb_cb).
enum { CHUNK_LIST = 1, CHUNK_MARKS = 2 };
static int chunk_new(uint64_t size, int host) {
    uint64_t want = host ? SNAP_LIST_MIN : SNAP_CHUNK;
    while (want < size) want <<= 1;
    if (nchunks == cchunks) grow((void **)&chunks, &cchunks, sizeof *chunks);
    SnapChunk *ch = &chunks[nchunks];
    int best = -1;
    plat_mutex_lock(&snap_pool_lock);
    for (int i = 0; i < nsnap_pool; i++)
        if (snap_pool[i].host == host && snap_pool[i].size >= want && (best < 0 || snap_pool[i].size < snap_pool[best].size))
            best = i;
    if (best >= 0) {
        *ch = snap_pool[best];
        snap_pool[best] = snap_pool[--nsnap_pool];
        snap_pool_bytes -= ch->size;
    }
    plat_mutex_unlock(&snap_pool_lock);
    if (best < 0) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = want,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,   // the source of mirror_capture's copies
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo ai = {.usage = host ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                      .flags = host == CHUNK_MARKS ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                                                         VMA_ALLOCATION_CREATE_MAPPED_BIT
                                             : host ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                                          VMA_ALLOCATION_CREATE_MAPPED_BIT : 0};
        if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &ch->buf, &ch->alloc, NULL) != VK_SUCCESS) return -1;
        ch->size = want, ch->host = host;
        mrep.snap_allocs++;
    }
    ch->used = 0;
    return nchunks++;
}

// Record time: room in a snapshot chunk for [off, off+size) of r's mirror, once per submission. 0: plain copy instead.
static int snap_add(PoolRec *r, uint64_t off, uint64_t size) {
    for (int i = nsnl; i-- > 0;)
        if (snl[i].r == r && snl[i].off == off && snl[i].size == size) return 1;
    int k = -1;
    for (int i = nchunks; i-- > 0;)   // the newest snapshot chunk, if the range fits
        if (chunks[i].used != UINT64_MAX && chunks[i].size - chunks[i].used >= size) { k = i; break; }
    if (k < 0 && (k = chunk_new(size > SNAP_CHUNK ? size : SNAP_CHUNK, 0)) < 0) return 0;
    if (nsnl == csnl) grow((void **)&snl, &csnl, sizeof *snl);
    snl[nsnl++] = (SnapRange){r, off, size, chunks[k].used, k};
    chunks[k].used += (size + 15) & ~15ull;
    return 1;
}

// Prologue: the snapshot copies of the ranges added since the last prologue.
static void snap_copies(VkCommandBuffer cb) {
    for (int i = snl_copied; i < nsnl; i++) {
        VkBufferCopy cp = {snl[i].off, snl[i].soff, snl[i].size};
        vkCmdCopyBuffer(cb, snl[i].r->mbuf, chunks[snl[i].chunk].buf, 1, &cp);
        mrep.snap_bytes += snl[i].size;
    }
    snl_copied = nsnl;
}

static int snap_cmp(const void *x, const void *y) {
    const SnapRange *a = x, *b = y;
    if (a->r != b->r) return (uintptr_t)a->r < (uintptr_t)b->r ? -1 : 1;
    if (a->chunk != b->chunk) return a->chunk < b->chunk ? -1 : 1;
    return a->off < b->off ? -1 : a->off > b->off;
}

// One dispatch: ranges s[0..n) share a pool and a chunk and lie in [base, base + span) of the pool. marks: the chunk
// of the page marks, its first at marks_off, or -1 when the dispatch marks none (the range list stands in for the
// binding, which the shader then does not write).
static void merge_dispatch(VkCommandBuffer cb, const SnapRange *s, int n, uint64_t base, uint64_t span, int list,
                           uint64_t list_off, uint32_t total, int marks, uint64_t marks_off) {
    uint64_t npg = ((base & (SB_PAGE - 1)) + span + SB_PAGE - 1) / SB_PAGE;
    VkDescriptorBufferInfo bi[5] = {{s->r->mbuf, base, span}, {chunks[s->chunk].buf, 0, chunks[s->chunk].size},
                                    {s->r->buf, base, span}, {chunks[list].buf, list_off, (uint64_t)n * 16},
                                    marks >= 0 ? (VkDescriptorBufferInfo){chunks[marks].buf, marks_off, npg * 4}
                                               : (VkDescriptorBufferInfo){chunks[list].buf, list_off, (uint64_t)n * 16}};
    VkWriteDescriptorSet w[5];
    for (int i = 0; i < 5; i++)
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .pBufferInfo = &bi[i]};
    rec_push(WM.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, WM.layout, 0, 5, w);   // in call order (vk_rec.h)
    uint32_t pc[4] = {(uint32_t)n, total, (uint32_t)(base & (SB_PAGE - 1)), marks >= 0};
    vkCmdPushConstants(cb, WM.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (total + 63) / 64, 1, 1);
}

// ---- shadow storage: the GPU's writes into VRAM mirrors back into guest memory
// The mirror's write-back (vk_mem_mirror_end) merges or copies them into the shadow, and the submission's job, added to
// its fence after the replay's own callbacks, copies them on into guest memory: of a merge dispatch only the pages its
// shader marked (wb_merge.comp), of a copy every page, compared with its base copy (merge_page). The fence callback of
// each noted range (gpu_written) compared all of it: 65 MiB of storage buffers a frame in Kokiri Forest, 9 ms a frame
// of the completion thread, behind which the game's fence waits sat (RTX 4090, 2026-11-09).
typedef struct { uint64_t addr, size; uint32_t mark0; } SwbRange;   // guest range; its first page's mark, or SWB_ALL
#define SWB_ALL UINT32_MAX
typedef struct { VmaAllocation marks_alloc; const uint32_t *marks; int n; SwbRange g[]; } SwbJob;
static SwbRange *swl;              // replay: this submission's merge dispatches
static int nswl, cswl;
static int swl_marks = -1;         // replay: their page marks' chunk, -1 none

static void swl_add(uint64_t addr, uint64_t size, uint32_t mark0) {
    if (nswl == cswl) grow((void **)&swl, &cswl, sizeof *swl);
    swl[nswl++] = (SwbRange){addr, size, mark0};
}

// Completion thread: [a, b) into guest memory, and the pages that changed stamped as the mirror's write-back.
static void swb_run(uint64_t a, uint64_t b) {
    if (b <= a || !shadow_write_back(a, b - a) || nvn_gpu_only(a)) return;
    int back = wb_no_reup();
    plat_mutex_lock(&gen_lock);
    uint32_t g = ++gen_now;
    for (uint64_t x = a & ~0xfffull; x < b; x += 1ull << GEN_PAGE_SHIFT) stamp_page_as(x, g, back);
    plat_mutex_unlock(&gen_lock);
}

static void shadow_wb_cb(void *arg) {   // completion thread
    SwbJob *j = arg;
    if (j->marks_alloc) vmaInvalidateAllocation(vk_ctx()->vma, j->marks_alloc, 0, VK_WHOLE_SIZE);
    for (int i = 0; i < j->n; i++) {
        const SwbRange *g = &j->g[i];
        uint64_t end = g->addr + g->size;
        if (g->mark0 == SWB_ALL) {
            swb_run(g->addr, end);
            continue;
        }
        uint64_t run = 0;   // start of the run of marked pages; guest address 0 is never a pool's
        uint32_t k = g->mark0;
        for (uint64_t pg = g->addr & ~(SB_PAGE - 1); pg < end; pg += SB_PAGE, k++) {
            uint64_t a = pg > g->addr ? pg : g->addr;
            if (j->marks[k]) {
                if (!run) run = a;
            } else if (run) {
                swb_run(run, a);
                run = 0;
            }
        }
        if (run) swb_run(run, end);
    }
    // NX2NAVI_WB_PROBE (vk_gpu_written_back): every range is in guest memory now; a later range's thaw would end the
    // probe of a region an earlier one armed, so the probes come last
    for (int i = 0; i < j->n; i++) mem_probe(j->g[i].addr, j->g[i].size);
    free(j);
}

// vk_mem_mirror_end, before the snapshot chunks go back at the fence (snap_free reuses the marks' chunk): the job of
// this submission's merge dispatches and copies.
static void swb_job(uint64_t fence) {
    SwbJob *j = malloc(sizeof *j + sizeof(SwbRange) * (size_t)(nswl + nwbl));
    if (!j) rt_fatal("vk: out of memory");
    j->marks_alloc = NULL, j->marks = NULL;
    if (swl_marks >= 0) {
        VmaAllocationInfo info;
        vmaGetAllocationInfo(vk_ctx()->vma, chunks[swl_marks].alloc, &info);
        j->marks_alloc = chunks[swl_marks].alloc, j->marks = info.pMappedData;
    }
    memcpy(j->g, swl, sizeof *swl * (size_t)nswl);
    j->n = nswl;
    for (int i = 0; i < nwbl; i++) j->g[j->n++] = (SwbRange){wbl[i].r->import_lo + wbl[i].off, wbl[i].size, SWB_ALL};
    nswl = 0, swl_marks = -1;
    vk_completion_add(fence, shadow_wb_cb, j);
}

// vk_mem_mirror_end: the merge dispatches of this submission's ranges. Returns the commands recorded; 0 with nsnl
// left as it was when the range list cannot be allocated (the caller copies them plainly). With shadow storage each
// dispatch also marks the pages it changed, for swb_job.
static int wb_merge_record(VkCommandBuffer cb) {
    VkCtx *c = vk_ctx();
    uint64_t align = c->props.limits.minStorageBufferOffsetAlignment;
    if (align < 16) align = 16;
    SnapRange *s = malloc(sizeof *s * (size_t)nsnl);
    struct MGroup { int i, j; uint64_t base, end; uint32_t total; } *gr = malloc(sizeof *gr * (size_t)nsnl);
    if (!s || !gr) rt_fatal("vk: out of memory");
    memcpy(s, snl, sizeof *s * (size_t)nsnl);
    qsort(s, (size_t)nsnl, sizeof *s, snap_cmp);
    // the dispatches: ranges of one pool and snapshot chunk, within MERGE_SPAN bytes and MERGE_WORDS words
    int ng = 0;
    uint64_t nmarks = 0;   // bytes of page marks, each dispatch's at an aligned offset
    for (int i = 0; i < nsnl;) {
        uint64_t base = s[i].off & ~(align - 1), end = s[i].off + s[i].size;
        uint32_t total = 0;
        int j = i;
        for (; j < nsnl && s[j].r == s[i].r && s[j].chunk == s[i].chunk; j++) {
            uint64_t e = s[j].off + s[j].size > end ? s[j].off + s[j].size : end;
            if (j > i && (e - base > MERGE_SPAN || total + s[j].size / 4 > MERGE_WORDS)) break;
            end = e;
            total += (uint32_t)(s[j].size / 4);
        }
        gr[ng++] = (struct MGroup){i, j, base, end, total};
        nmarks += (((base & (SB_PAGE - 1)) + end - base + SB_PAGE - 1) / SB_PAGE * 4 + align - 1) & ~(align - 1);
        i = j;
    }
    // the range lists: at most one 16-byte entry per range, each dispatch's list at an aligned offset
    int list = chunk_new((uint64_t)nsnl * 16 + (uint64_t)nsnl * align, CHUNK_LIST);
    if (list < 0) {
        free(s), free(gr);
        return 0;
    }
    chunks[list].used = UINT64_MAX;   // not a snapshot chunk
    int marks = shadow_on ? chunk_new(nmarks, CHUNK_MARKS) : -1;
    uint32_t *mm = NULL;
    if (marks >= 0) {
        chunks[marks].used = UINT64_MAX;
        VmaAllocationInfo mi;
        vmaGetAllocationInfo(c->vma, chunks[marks].alloc, &mi);
        mm = mi.pMappedData;
        memset(mm, 0, nmarks);
        vmaFlushAllocation(c->vma, chunks[marks].alloc, 0, VK_WHOLE_SIZE);
        swl_marks = marks;
    }
    VmaAllocationInfo li;
    vmaGetAllocationInfo(c->vma, chunks[list].alloc, &li);
    uint32_t *lm = li.pMappedData;
    uint64_t lo = 0, mo = 0;
    int rec = 0;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, WM.pipe);
    for (int k = 0; k < ng; k++) {
        const struct MGroup *d = &gr[k];
        lo = (lo + align - 1) & ~(align - 1);
        uint64_t l0 = lo;
        uint32_t first = 0;   // the range's first invocation
        for (int j = d->i; j < d->j; j++) {
            uint32_t *q = lm + lo / 4;
            q[0] = (uint32_t)((s[j].off - d->base) / 4), q[1] = (uint32_t)(s[j].soff / 4), q[2] = (uint32_t)(s[j].size / 4);
            q[3] = first;
            first += (uint32_t)(s[j].size / 4);
            lo += 16;
            mrep.merge_bytes += s[j].size;
        }
        merge_dispatch(cb, s + d->i, d->j - d->i, d->base, d->end - d->base, list, l0, d->total, marks, mo);
        if (shadow_on) {
            uint64_t ga = s[d->i].r->import_lo + d->base;
            if (marks >= 0) swl_add(ga, d->end - d->base, (uint32_t)(mo / 4));
            else
                for (int j = d->i; j < d->j; j++) swl_add(s[j].r->import_lo + s[j].off, s[j].size, SWB_ALL);
        }
        mo += (((d->base & (SB_PAGE - 1)) + d->end - d->base + SB_PAGE - 1) / SB_PAGE * 4 + align - 1) & ~(align - 1);
        mrep.merge_dispatches++;
        rec += 3;
    }
    vmaFlushAllocation(c->vma, chunks[list].alloc, 0, VK_WHOLE_SIZE);
    for (int i = 0; i < nsnl; i++) snl[i].r->wb_sub = msub;
    free(s), free(gr);
    return rec;
}

static void snap_free(void *arg) {   // completion thread: the submission is done with its chunks
    SnapFree *f = arg;
    plat_mutex_lock(&snap_pool_lock);
    for (int i = 0; i < f->n; i++) {
        SnapChunk *ch = &f->c[i];
        if (nsnap_pool == SNAP_POOL_N || snap_pool_bytes + ch->size > snap_pool_cap()) continue;
        snap_pool[nsnap_pool++] = *ch;
        snap_pool_bytes += ch->size;
        ch->buf = VK_NULL_HANDLE;
    }
    plat_mutex_unlock(&snap_pool_lock);
    for (int i = 0; i < f->n; i++)
        if (f->c[i].buf) vmaDestroyBuffer(vk_ctx()->vma, f->c[i].buf, f->c[i].alloc);
    free(f);
}

// ---- lazy write-back (NX2NAVI_WB_LAZY; the default by platform and driver, wb_lazy_choice.h; memory.c "Lazy write-back")
// A range a shader wrote goes back to guest memory once and is then armed (vk_gpu_written_back, mem_lazy_arm). While it
// stays armed, gpu_wrote leaves the later writes of its pages in the mirror: no snapshot, no merge, no copy. When the
// CPU touches a page, memory.c calls lazy_fetch with the mirror and the imported buffer of the range: a copy of its
// own, sent after every part handed over so far (the held ones first), and waited for. The part that is still being
// recorded when that happens has already left its writes out: at its end (lazy_part_check, from vk_mem_mirror_end or
// from the prologue of a part split off earlier) it writes back, as ever, what it skipped and finds no longer armed;
// the snapshot for that merge is taken in the part's own prologue, before its GPU work. From that check to the part's
// hand-over (vk_mem_lazy_handed) memory.c's window keeps a fault from releasing a range unseen.
static VkCommandPool lz_pool;
static VkCommandBuffer lz_cb;
static VkFence lz_fence;
static volatile int32_t lz_state;   // 0 not asked, 1 on, 2 off
static PlatMutex lz_init_lock = PLAT_MUTEX_INIT;
typedef struct { VkMemSpan sp; uint64_t size; int shader; } LzSkip;
static LzSkip *lz_skip;             // replay: what the part being recorded left in the mirror
static int lz_nskip, lz_cskip;
static int lz_win;                  // replay: the part's window is open (lazy_part_check)
static uint64_t lz_skipped;         // replay: bytes left in the mirror since the last log line

static void mbarrier(VkCommandBuffer cb, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds,
                     VkAccessFlags2 da);

// memory.c's fetch (frz_lock held there, so one at a time): [off, off + size) of the mirror h0 into the imported
// buffer h1, after all the GPU work handed over so far.
static void lazy_fetch(uint64_t h0, uint64_t h1, uint64_t off, uint64_t size) {
    if (vk_device_is_lost()) return;   // the run is ending (vk_device.c vk_device_lost_check): the bytes stay as they are
    VkCtx *c = vk_ctx();
    VkBufferCopy reg = {off, off, size};
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkResetCommandPool(c->device, lz_pool, 0);
    vkBeginCommandBuffer(lz_cb, &bi);
    mbarrier(lz_cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
             VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyBuffer(lz_cb, (VkBuffer)(uintptr_t)h0, (VkBuffer)(uintptr_t)h1, 1, &reg);
    mbarrier(lz_cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
             VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
             VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    vkEndCommandBuffer(lz_cb);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &lz_cb};
    vk_queue_lock();
    vk_sync_flush_held();   // the held submissions wrote first
    VkResult r = vkQueueSubmit(c->gfx_queue, 1, &si, lz_fence);
    vk_queue_unlock();
    if (r == VK_SUCCESS) r = vkWaitForFences(c->device, 1, &lz_fence, VK_TRUE, 10000000000ull);
    if (r != VK_SUCCESS) {
        vk_device_lost_check(r, "the submit or the wait that brings GPU writes down to guest memory");
        rt_fatal("vk: bringing %llu bytes the GPU wrote down to guest memory failed (%d)", (unsigned long long)size, (int)r);
    }
    vkResetFences(c->device, 1, &lz_fence);
}

// memory.c's test before it arms [lo, hi) of the pool ctx (frz_lock held there, the pages already inaccessible): the
// mirror holds every CPU write to them. A fetch copies the mirror over the whole range, not the merge of the bytes the
// GPU changed that a write-back does, so a CPU write that came after the range's write-back reached guest memory and
// before this arm, and that no sync of the mirror has carried up yet, would be overwritten by the mirror's older bytes:
// a page stamped after the mirror's last sync (the CPU-write table, mgen), or a write-watch bit nobody has read yet.
// No lock: the pages are inaccessible, so nothing stamps them meanwhile; the mirror's mgen only grows.
static int lazy_clean(uint64_t lo, uint64_t hi, void *ctx) {
    const PoolRec *r = ctx;
    uint32_t mgen = __atomic_load_n(&r->mgen, __ATOMIC_ACQUIRE);
    for (uint64_t a = lo; a < hi; a += 1ull << GEN_PAGE_SHIFT) {
        const uint32_t *ch = __atomic_load_n(&cgen_chunk[a >> GEN_CHUNK_SHIFT], __ATOMIC_ACQUIRE);
        if (ch && __atomic_load_n(&ch[(a >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)], __ATOMIC_RELAXED) > mgen) return 0;
    }
    uint64_t p[1];
    return mem_written_peek(lo, hi - lo, p, 1) == 0;   // UINT64_MAX (not watched) counts as written
}

// Whether the lazy write-back is wanted on this device, decided and logged once, when the driver is known
// (vk_device.c create_device, after vk_caps_init): the player's setting, else the default of the platform and driver
// (wb_lazy_choice.h). Never in import mode, where no pool has a mirror; with shadow storage, where a fetch would copy
// the mirror into the pool's buffer, which is guest memory only when the pool is imported (a shadow's bytes reach
// guest memory in the submission's shadow_wb_cb); or with the write-back probe, which watches the same pages.
_Static_assert(WBL_DRIVER_AMD == VK_DRIVER_ID_AMD_PROPRIETARY, "wb_lazy_choice.h names AMD's own driver");
static int lz_wanted = -1;          // -1 not decided, 0 off, 1 on
static const char *lz_why = "";     // the reason the log gave

void vk_mem_lazy_decide(void) {
    const char *set = cfg_is_set("NX2NAVI_WB_LAZY") ? cfg_get("NX2NAVI_WB_LAZY") : NULL;
    WblChoice ch = wb_lazy_choice(set, WBL_ON_WINDOWS, vk_caps.driver_id);
    if (!vk_mirror_on) ch = (WblChoice){0, "no pool has a mirror in import mode"};
    else if (shadow_on) ch = (WblChoice){0, "shadow storage keeps it off"};
    else if (ch.on && cfg_bool("NX2NAVI_WB_PROBE")) ch = (WblChoice){0, "the write-back probe watches the same pages"};
    lz_why = ch.why;
    __atomic_store_n(&lz_wanted, ch.on, __ATOMIC_RELEASE);
    rt_log("mem: lazy write-back %s: %s", ch.on ? "on" : "off", ch.why);
}

static int wb_lazy(void) {
    int s = __atomic_load_n(&lz_state, __ATOMIC_ACQUIRE);
    if (s) return s == 1;
    plat_mutex_lock(&lz_init_lock);
    if (!lz_state) {
        VkCtx *c = vk_ctx();
        int on = c && __atomic_load_n(&lz_wanted, __ATOMIC_ACQUIRE) == 1;
        if (on) {
            VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                          .queueFamilyIndex = c->gfx_family};
            VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            on = vkCreateCommandPool(c->device, &pi, NULL, &lz_pool) == VK_SUCCESS;
            VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = lz_pool,
                                              .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
            on = on && vkAllocateCommandBuffers(c->device, &ai, &lz_cb) == VK_SUCCESS &&
                 vkCreateFence(c->device, &fi, NULL, &lz_fence) == VK_SUCCESS;
            on = on && mem_lazy_enable(lazy_fetch, lazy_clean);
        }
        rt_log("mem: what shaders write into mirrored pools %s (lazy write-back: %s)",
               on ? "goes back to guest memory once and then only when the CPU touches it"
                  : "goes back to guest memory at the end of every submission",
               on || lz_wanted != 1 ? lz_why : "its command buffer and fence could not be made");
        __atomic_store_n(&lz_state, on ? 1 : 2, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&lz_init_lock);
    return lz_state == 1;
}

static void gpu_wrote_range(const VkMemSpan *sp, uint64_t size, int shader);

// NX2NAVI_WB_SKIP_MB (default 0, off), a test switch for drivers that run without the lazy write-back (AMD's, and
// outside Windows: wb_lazy_choice.h): there, what a binding written by a shader or a copy holds goes back to guest
// memory at the end of every submission, up to 66 MiB a frame in Hyrule Field. With it, a written range of at least
// this many MiB goes back only for its first WB_SKIP_HEAD bytes and the rest stays in the mirror. The CPU reads a few
// words at the start of such buffers (12 bytes into ranges of 6.3 and 15.2 MiB, see "Lazy write-back" in memory.c),
// which the head still carries; a CPU read further in would see older bytes, and a CPU write further in would have the
// next sync copy that page's older guest bytes over what the GPU wrote there. The pool's mirror is never evicted from
// then on, since guest memory lacks those bytes.
#define WB_SKIP_HEAD (64u << 10)
static uint64_t wb_skip(void) {
    static int64_t at = -1;
    if (at < 0) {
        int mb = cfg_int("NX2NAVI_WB_SKIP_MB");
        at = mb > 0 ? (int64_t)mb << 20 : 0;
        if (at)
            rt_log("mem: without the lazy write-back, written ranges of %d MiB or more go back to guest memory only for "
                   "their first %u KiB (NX2NAVI_WB_SKIP_MB)", mb, WB_SKIP_HEAD >> 10);
    }
    return (uint64_t)at;
}
static int64_t m_wb_skipped;    // bytes wb_skip kept in mirrors since the last summary

static volatile uint64_t n_gpu_wrote;   // vk_mem_gpu_wrote_count
uint64_t vk_mem_gpu_wrote_count(void) { return __atomic_load_n(&n_gpu_wrote, __ATOMIC_RELAXED); }

static void gpu_wrote(const VkMemSpan *sp, uint64_t size, int shader) {
    __atomic_fetch_add(&n_gpu_wrote, 1, __ATOMIC_RELAXED);
    PoolRec *r = sp->pool;
    if (!r || r->mstate != MS_ON || sp->buffer != r->mbuf || !size || !wb_lazy()) {
        uint64_t skip = r && r->mstate == MS_ON && sp->buffer == r->mbuf ? wb_skip() : 0;
        if (skip && size >= skip && !wb_lazy()) {
            gpu_wrote_range(sp, WB_SKIP_HEAD, shader);
            r->wb_kept = 1;
            plat_add64(&m_wb_skipped, (int64_t)(size - WB_SKIP_HEAD));
            return;
        }
        gpu_wrote_range(sp, size, shader);
        return;
    }
    uint64_t g0 = r->import_lo + sp->offset, g1 = g0 + size, run = g0, out[16][2];
    int k = mem_lazy_skip(g0, size, (uint64_t)(uintptr_t)r->mbuf, -(int64_t)r->import_lo, out, 16);
    for (int i = 0; i <= k; i++) {
        uint64_t lo = i < k ? out[i][0] : g1, hi = i < k ? out[i][1] : g1;
        if (lo > run) {   // not armed: written back as ever
            VkMemSpan part = *sp;
            part.offset = sp->offset + (run - g0);
            gpu_wrote_range(&part, lo - run, shader);
        }
        if (i < k) {
            if (lz_nskip == lz_cskip) grow((void **)&lz_skip, &lz_cskip, sizeof *lz_skip);
            LzSkip k = {*sp, hi - lo, shader};
            k.sp.offset = sp->offset + (lo - g0);
            lz_skip[lz_nskip++] = k;
            lz_skipped += hi - lo;
        }
        run = hi;
    }
}

// The end of the part being recorded: what it left in the mirror for a range that is no longer armed (the CPU touched
// it meanwhile) is written back after all. Opens memory.c's window, which vk_mem_lazy_handed closes.
static void lazy_part_check(void) {
    if (lz_win || !lz_nskip) return;
    mem_lazy_window(1);
    lz_win = 1;
    for (int i = 0; i < lz_nskip; i++) {
        const PoolRec *r = lz_skip[i].sp.pool;
        if (!mem_lazy_armed(r->import_lo + lz_skip[i].sp.offset, lz_skip[i].size))
            gpu_wrote_range(&lz_skip[i].sp, lz_skip[i].size, lz_skip[i].shader);
    }
}

// vk_sync.c, before a part's prologue is recorded: the check for a part that was split off (the last part's ran in
// vk_mem_mirror_end already).
void vk_mem_lazy_part_end(void) { lazy_part_check(); }

// vk_sync.c, once the part is held or handed to the queue: a fetch from here on has its writes.
void vk_mem_lazy_handed(void) {
    lz_nskip = 0;
    if (lz_win) mem_lazy_window(0), lz_win = 0;
}

static void gpu_wrote_range(const VkMemSpan *sp, uint64_t size, int shader) {
    PoolRec *r = sp->pool;
    if (!r || r->mstate != MS_ON || sp->buffer != r->mbuf || !size) return;
    r->vd_wrote_sub = msub, r->vd_wrote_n++;
    r->hot_bytes += size;
    // A binding to the end of its pool (no size) can span hundreds of MiB: those stay copies.
    uint64_t o = 0;
    if (shader && !(sp->offset & 3) && !(size & 3) && size <= merge_max() && merging())
        for (uint64_t n; o < size; o += n)
            if (!snap_add(r, sp->offset + o, n = size - o < MERGE_PIECE ? size - o : MERGE_PIECE)) break;
    r->st_merge += o;
    if (o == size) return;
    r->st_wb += size - o;
    range_add(&wbl, &nwbl, &cwbl, r, sp->offset + o, size - o);   // what found no snapshot room
}
void vk_mem_gpu_wrote(const VkMemSpan *sp, uint64_t size) {
    int lk = vk_exec_shared();
    if (lk) plat_mutex_lock(&use_lock);
    gpu_wrote(sp, size, 0);
    if (lk) plat_mutex_unlock(&use_lock);
}
void vk_mem_gpu_wrote_shader(const VkMemSpan *sp, uint64_t size) {
    int lk = vk_exec_shared();
    if (lk) plat_mutex_lock(&use_lock);
    gpu_wrote(sp, size, 1);
    if (lk) plat_mutex_unlock(&use_lock);
}

// Adds [a, b) to l, n sorted ranges that do not touch; returns the new count. Above cap ranges (l holds cap + 1) the two
// nearest merge, so a full list copies the fewest bytes that were not flushed; cap 1 keeps one span over all flushes.
static int flush_add(FlushRange *l, int n, uint64_t a, uint64_t b, int cap) {
    int i = 0;
    while (i < n && l[i].hi < a) i++;
    int j = i;
    for (; j < n && l[j].lo <= b; j++) {
        if (l[j].lo < a) a = l[j].lo;
        if (l[j].hi > b) b = l[j].hi;
    }
    memmove(l + i + 1, l + j, (size_t)(n - j) * sizeof *l);
    l[i] = (FlushRange){a, b};
    n += 1 - (j - i);
    if (n > cap) {
        int k = 0;
        for (int x = 1; x + 1 < n; x++)
            if (l[x + 1].lo - l[x].hi < l[k + 1].lo - l[k].hi) k = x;
        l[k].hi = l[k + 1].hi;
        memmove(l + k + 1, l + k + 2, (size_t)(n - k - 2) * sizeof *l);
        n--;
    }
    return n;
}

int vk_mem_flush_add_probe(void) {
    FlushRange l[5];
    int n = 0;
    n = flush_add(l, n, 0x8000, 0x9000, 4);
    n = flush_add(l, n, 0x1000, 0x2000, 4);
    n = flush_add(l, n, 0x4000, 0x5000, 4);
    if (n != 3 || l[0].lo != 0x1000 || l[1].lo != 0x4000 || l[2].hi != 0x9000) return 1;   // kept apart, sorted
    n = flush_add(l, n, 0x2000, 0x3000, 4);
    if (n != 3 || l[0].hi != 0x3000) return 2;                                             // touching ranges join
    n = flush_add(l, n, 0x800, 0x8800, 4);
    if (n != 1 || l[0].lo != 0x800 || l[0].hi != 0x9000) return 3;                         // one covers several
    n = 0;
    n = flush_add(l, n, 0x10000, 0x11000, 2);
    n = flush_add(l, n, 0x40000, 0x41000, 2);
    n = flush_add(l, n, 0x12000, 0x13000, 2);                                              // over the cap: nearest merge
    if (n != 2 || l[0].lo != 0x10000 || l[0].hi != 0x13000 || l[1].lo != 0x40000) return 4;
    n = 0;
    n = flush_add(l, n, 0x10000, 0x11000, 1);
    n = flush_add(l, n, 0x40000, 0x41000, 1);
    if (n != 1 || l[0].lo != 0x10000 || l[0].hi != 0x41000) return 5;                      // NX2NAVI_FLUSH_SPAN=1
    return 0;
}

uint64_t vk_mem_mirror_uploaded(void) {
    plat_mutex_lock(&mirror_stat_lock);
    uint64_t n = macc.upload_bytes;
    plat_mutex_unlock(&mirror_stat_lock);
    return n;
}

void nvn_pool_flushed(const pool_t *p, uint64_t off, uint64_t size) {
    if (!vk_mirror_on || !size || !p->mem) return;
    plat_inc64(&m_flush_calls);
    plat_add64(&m_flush_bytes, (int64_t)size);
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)p->mem, 0);
    PoolRec *r = e && e->key == p ? e->buf : NULL;
    if (r && r->mstate == MS_ON) {
        uint64_t a = ((uintptr_t)p->mem + off) & ~0xfffull, b = ((uintptr_t)p->mem + off + size + 0xfff) & ~0xfffull;
        plat_mutex_lock(&flush_lock);
        __atomic_store_n(&r->fl_n, flush_add(r->fl, r->fl_n, a, b, flush_cap), __ATOMIC_RELEASE);
        r->fl_calls++, r->fl_sum += size;
        plat_mutex_unlock(&flush_lock);
    }
    plat_mutex_unlock_shared(&pool_lock);
}

// gen_lock exclusive: freezes up to FREEZE_PER_SUB candidate regions still idle. The query after the
// protection collects the writes made before it took effect.
// A freeze is a VirtualQuery and a VirtualProtect of 2 MiB, some microseconds as a rule; in the second after an area
// load, while the loaders write and the driver imports and locks new memory, each took 8 to 17 ms. The newly loaded
// regions all go idle together, so 32 of them came up in one submission: its mirror begin took 70.5 ms and the frame
// 78 ms, and one freeze of 9 ms followed in each of the next 20 frames. A freeze only saves later write-watch queries
// (about a microsecond per MiB per submission), so it can wait: a pass stops once it has spent freeze_budget_ns, and a
// freeze over FREEZE_SLOW_NS holds the passes for FREEZE_BACKOFF_NS, doubled for each slow one in a row up to 2 s.
// NX2NAVI_FREEZE_BUDGET_US (default 1000; 0: no budget and no backoff, as before).
#define FREEZE_SLOW_NS 2000000ull
#define FREEZE_BACKOFF_NS 250000000ull
#define FREEZE_BACKOFF_MAX_NS 2000000000ull
static uint64_t freeze_budget_ns = ~0ull;   // ~0: not read yet
static uint64_t frz_hold_until, frz_backoff, frz_held_passes, frz_slow;   // gen_lock

static void freeze_pass(void) {
    if (freeze_budget_ns == ~0ull) {
        long long us = cfg_int("NX2NAVI_FREEZE_BUDGET_US");
        freeze_budget_ns = us > 0 ? (uint64_t)us * 1000 : 0;
    }
    uint64_t start = qpc();
    if (freeze_budget_ns && start < frz_hold_until) {
        if (nfcand) frz_held_passes++;
        return;
    }
    for (int done = 0; nfcand && done < FREEZE_PER_SUB;) {
        uint64_t j = fcand[--nfcand], a = j << REG_SHIFT;
        if (ww_ep - reg_last[j] < (uint32_t)freeze_idle || mem_frozen(a)) continue;
        done++;
        uint64_t t0 = qpc();
        int ok = mem_freeze(a);
        uint64_t t1 = qpc();
        slow_mem_call("freeze", a, 1ull << REG_SHIFT, 0, t1 - t0);
        if (ok) query_stamp(a, a + (1ull << REG_SHIFT), ++gen_now);
        else n_frz_refused++;
        if (!freeze_budget_ns) continue;
        if (t1 - t0 >= FREEZE_SLOW_NS) {
            frz_slow++;
            frz_backoff = frz_backoff ? frz_backoff * 2 : FREEZE_BACKOFF_NS;
            if (frz_backoff > FREEZE_BACKOFF_MAX_NS) frz_backoff = FREEZE_BACKOFF_MAX_NS;
            frz_hold_until = t1 + frz_backoff;
            static uint32_t logged;
            if (++logged <= 40)
                rt_log("mem: freezing an idle region took %.1f ms: no freezes for %.0f ms (%llu slow freezes so far, %llu "
                       "passes held, %d regions waiting)", (t1 - t0) / 1e6, frz_backoff / 1e6, (unsigned long long)frz_slow,
                       (unsigned long long)frz_held_passes, nfcand);
            break;
        }
        frz_backoff = 0;
        if (qpc() - start >= freeze_budget_ns) break;
    }
}

// NX2NAVI_MIRROR_HOT (percent, 0 off): a mirrored pool whose copies each frame (CPU pages up, GPU writes back) reach this
// share of its size goes back to being read and written by the GPU in system memory. On an RX 580 in Kokiri Forest a
// handful of 6-16 MiB pools the CPU refills every frame and compute shaders write took 66 MiB of uploads and 76 MiB of
// write-backs a frame through the mirrors, the GPU idle 7.8 ms a frame with those copies queued; read in place, only
// the bytes the shaders touch cross the bus. Checked every HOT_FRAMES frames at the start of a submission, when the
// previous one has queued its write-backs and nothing is pending for the next: the pool leaves the mirrored list, the
// device address table points at its imported buffer again, and the mirror is freed once the submissions that may still
// use it are done (vk_defer_buffer, as mirror_evict). Pools under HOT_MIN a frame stay mirrored.
// Only pools the CPU does not write go: a pool read in place gives the GPU the game's bytes as they are when it reads
// them, not as they were at the submit (the mirror's copy). The game refills its per-frame pools for the next frame
// while the replay (asynchronous, up to a frame behind) and the GPU still draw the last one from them: the small grass
// and moss, drawn from instances in such pools, flickered and slid over the ground as the camera turned, and with
// 0.8.0's mesh fallback every mesh flashed (RX 580, Kokiri Forest, 2026-11-11; 18 pools of 2-15 MiB had gone, each
// refilled by the CPU every frame). A pool with more than HOT_UP_MAX of CPU writes a frame stays mirrored.
#define HOT_UP_MAX (64ull << 10)
#define HOT_FRAMES 120
#define HOT_MIN (1ull << 20)
static int mirror_hot = -1;
static uint64_t hot_frame0, n_demoted;

static void hot_check(void) {
    if (mirror_hot < 0) {
        const char *v = cfg_value("NX2NAVI_MIRROR_HOT");   // the settings row's default (0, off) when unset
        int p = v && *v ? atoi(v) : 0;
        mirror_hot = p < 0 ? 0 : p > 1000 ? 1000 : p;
        if (mirror_hot) rt_log("mem: pools copied %d%% of their size a frame stay in system memory (NX2NAVI_MIRROR_HOT)", mirror_hot);
    }
    if (!mirror_hot) return;
    uint64_t fr = mirror_frame;
    if (!hot_frame0 || fr < hot_frame0) {
        hot_frame0 = fr ? fr : 1;
        return;
    }
    if (fr - hot_frame0 < HOT_FRAMES || npend || nwbl) return;   // nsnl: the previous submission's, recorded already
    double frames = (double)(fr - hot_frame0);
    hot_frame0 = fr;
    PoolRec *out[8];
    VkBuffer dead[8];
    VmaAllocation dead_a[8];
    int nout = 0;
    plat_mutex_lock(&pool_lock);
    for (int i = 0; i < nm;) {
        PoolRec *r = mlist[i];
        uint64_t n = r->import_hi - r->import_lo, per = (uint64_t)(r->hot_bytes / frames), up = (uint64_t)(r->hot_up / frames);
        r->hot_bytes = (uint64_t)per;   // kept for the log line below
        r->hot_up = 0;
        if (nout == 8 || per < HOT_MIN || per * 100 < n * (uint64_t)mirror_hot || up > HOT_UP_MAX || !r->mbuf ||
            (!r->twin && r->gpa_rec)) {
            r->hot_bytes = 0, i++;   // !twin with a table record: the table's writes are in the mirror alone (mirror_evict)
            continue;
        }
        mem_lazy_release(r->import_lo, r->import_hi - r->import_lo);   // what only the mirror holds comes down first
        // as mirror_evict, under the exclusive lock (vk_pool_fini takes pools off the list under it), but for good:
        // MS_REFUSED with no retry (retry_sub 0), so neither vk_mem_use nor the budget retries mirror it again
        mirror_forget(r);   // takes r off mlist: i now names the next pool
        r->mstate = MS_REFUSED, r->retry_sub = 0;
        plat_mutex_lock(&flush_lock);   // flushes not copied yet: the pool is read in place from now on
        __atomic_store_n(&r->fl_n, 0, __ATOMIC_RELEASE);
        r->fl_calls = r->fl_sum = 0;
        plat_mutex_unlock(&flush_lock);
        // the pages name the imported buffer alone again (the twin was that buffer)
        if (r->gpa_rec) r->gpa_rec = vk_gpa_remap_plain(r->import_lo, r->import_hi, r->gpa_rec, r->buf, r->import_lo);
        dead[nout] = r->mbuf, dead_a[nout] = r->malloc_;
        r->mbuf = VK_NULL_HANDLE, r->malloc_ = NULL;
        out[nout++] = r;
    }
    plat_mutex_unlock(&pool_lock);
    for (int k = 0; k < nout; k++) {
        PoolRec *r = out[k];
        uint64_t n = r->import_hi - r->import_lo, per = r->hot_bytes;
        r->hot_bytes = 0;
        vk_defer_buffer(dead[k], dead_a[k]);   // after the submission being recorded
        char f[160];
        flag_names(r->flags, f, sizeof f);
        if (++n_demoted <= 32)
            rt_log("mem: pool %#llx (%.1f MiB, %s) copied %.1f MiB a frame through its mirror: system memory from now on",
                   (unsigned long long)r->lo, n / 1048576.0, f, per / 1048576.0);
    }
}

void vk_mem_mirror_begin(void) {
    if (vk_mirror_on) hot_check();
    if (!vk_mirror_on) {   // import mode: the texture checks' snapshots still skip frozen regions
        if (!freeze_idle) return;
        plat_mutex_lock(&gen_lock);
        ww_ep++;
        freeze_pass();
        plat_mutex_unlock(&gen_lock);
        return;
    }
    uint64_t t0 = qpc();
    nsnl = snl_copied = nchunks = 0;   // the previous submission handed its chunks to snap_free
    uint32_t prev = msub;
    msub = msub + 1 ? msub + 1 : 1;
    __atomic_add_fetch(&mirror_gen, 1, __ATOMIC_RELEASE);
    mepoch = vk_texture_epoch();
    uint64_t q = 0, run0 = 0, run1 = 0;
    int have = 0;
    plat_mutex_lock_shared(&pool_lock);
    plat_mutex_lock(&gen_lock);
    ww_ep++;
    uint64_t b0 = ww_bytes;
    int pl = sub_place(), pp = presnap_place();
    if (pl <= pplace) pframe++;   // a new frame, though its first submissions may have had nothing to record
    pplace = pl;
    for (int i = 0; i < nm; i++) {
        const PoolRec *r = mlist[i];
        if (pp ? !(pm_last(r) >> pl & 1) : r->acc_sub != prev) continue;
        uint64_t a = r->import_lo >> SNAP_SHIFT, b = (r->import_hi - 1) >> SNAP_SHIFT;
        if (have && a <= run1 + 1 + (uint64_t)mirror_gap) {
            if (b > run1) run1 = b;
            continue;
        }
        if (have) q += snap_regions(run0, run1, mepoch);
        run0 = a, run1 = b, have = 1;
    }
    if (have) q += snap_regions(run0, run1, mepoch);
    mrep.presnap_bytes += ww_bytes - b0;
    if (freeze_idle) freeze_pass();
    plat_mutex_unlock(&gen_lock);
    plat_mutex_unlock_shared(&pool_lock);
    mrep.subs++;
    mrep.presnap_queries += q;
    mrep.presnap_t += qpc() - t0;
    slow_mem_call("mirror begin (its queries and freezes)", 0, ww_bytes - b0, q, qpc() - t0);
}

int vk_mem_mirror_pending(void) { return npend || snl_copied < nsnl || n_gpa_flip; }

// NX2NAVI_MIRROR_CAPTURE_KB: the pages the CPU wrote since a VRAM mirror's last sync are copied out of guest memory by
// the CPU when the part that needs them is handed to the queue, up to this many KiB a part, into a host buffer the
// submission owns (a chunk of the snapshot pool, given back at its fence); the prologue copies them from there into
// the mirror. Without it the prologue copies them from guest memory when the GPU runs it, a frame or more after the
// game's submit when the GPU is behind: the GPU then reads whatever the game wrote there meanwhile, values meant for a
// later frame. A pool stays with the GPU-time copy while a write-back of its mirror into guest memory is still on the
// GPU (the CPU would read guest bytes that write-back has not reached yet and copy them over the mirror's newer ones),
// and when its device address table writes reach guest memory as well (a twin: what they write is not recorded); so
// does a range with a page that cannot be read without a fault (mem_copy_readable: a lazily armed range). The
// whole-pool copy of a mirror's first use is larger than the limit and stays with the GPU.
// Unset or 0 with NX2NAVI_MIRROR_STAGE=1 (what low_end_gpu = 1 sets): CAPTURE_STAGE_KB. A submission whose prologue copied
// everything this way reads nothing of guest memory late, so its submit wait ends once the replay has submitted it
// (vk_sync_recorded_wait); one with a range left to the GPU-time copy, or a draw that reads a pool without a mirror,
// is noted (vk_mem_note_live) and its submit wait is the GPU's. The first form of this (0.8.0) copied while the replay
// recorded and left out neither the write-backs in flight nor the twins.
#define CAPTURE_STAGE_KB (64 << 10)
static volatile int32_t mirror_live;   // the submission being recorded had the GPU read guest memory

static int stage_on(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("MIRROR_STAGE", "0") != 0;
    return on;
}

static uint64_t capture_cap(void) {
    static int64_t cap = -1;
    if (cap < 0) {
        int kb = cfg_int("NX2NAVI_MIRROR_CAPTURE_KB");
        if (kb <= 0 && stage_on()) kb = CAPTURE_STAGE_KB;
        cap = kb > 0 ? (int64_t)kb << 10 : 0;
        if (cap)
            rt_log("mem: mirror pages the CPU wrote are copied out of guest memory when their part is handed over, up to "
                   "%d KiB a part (NX2NAVI_MIRROR_CAPTURE_KB, NX2NAVI_MIRROR_STAGE)", kb);
    }
    return (uint64_t)cap;
}

void vk_mem_note_live(void) { __atomic_store_n(&mirror_live, 1, __ATOMIC_RELAXED); }
int vk_mem_live_swap(int v) { return __atomic_exchange_n(&mirror_live, v, __ATOMIC_RELAXED); }
static int wb_lazy(void);
int vk_mem_wb_lazy(void) { return wb_lazy(); }   // the lazy write-back runs (vk_exec_draw.c op_update_ub)

int vk_mem_take_live(void) {
    int live = __atomic_exchange_n(&mirror_live, 0, __ATOMIC_RELAXED);
    if (!vk_mirror_on || shadow_on || !capture_cap()) live = 1;
    return live;
}

static MRange *capm;   // replay: mirror_capture's merged ranges
static uint64_t *caps; // replay: the chunk offset of each, UINT64_MAX for one left to the GPU-time copy
static uint8_t **capd, *capok;   // replay: mem_copy_readable's destinations and results
static uint64_t (*capsrc)[2];    // replay: and its sources
static int ccapm, ccaps, ccapd, ccapsrc, ccapok;

// Prologue, before copy_ranges: takes the ranges of pend the CPU copies (capture_cap) out of it and records their
// copies from the chunk into the mirrors; what is left in pend goes the GPU-time way.
static void mirror_capture(VkCommandBuffer cb) {
    uint64_t cap = capture_cap();
    if (!cap || shadow_on || !npend) return;
    uint64_t t0 = qpc(), done = vk_sync_fence_done(), total = 0;
    qsort(pend, (size_t)npend, sizeof *pend, range_cmp);
    int n = 0;
    for (int i = 0; i < npend; i++) {   // merged as copy_ranges merges them
        PoolRec *r = pend[i].r;
        uint64_t lim = r->import_hi - r->import_lo, lo = pend[i].off, hi = pend[i].off + pend[i].size;
        if (hi > lim) hi = lim;
        if (lo >= hi) continue;
        if (n && capm[n - 1].r == r && lo <= capm[n - 1].off + capm[n - 1].size) {
            if (hi > capm[n - 1].off + capm[n - 1].size) capm[n - 1].size = hi - capm[n - 1].off;
            continue;
        }
        if (n == ccapm) grow((void **)&capm, &ccapm, sizeof *capm);
        capm[n++] = (MRange){r, lo, hi - lo};
    }
    while (ccaps < ccapm) grow((void **)&caps, &ccaps, sizeof *caps);
    for (int i = 0; i < n; i++) {
        const PoolRec *r = capm[i].r;
        uint64_t sz = (capm[i].size + 15) & ~15ull;
        caps[i] = UINT64_MAX;
        if (r->twin || r->wb_fence > done || total + sz > cap) continue;
        caps[i] = total;
        total += sz;
    }
    int k = total ? chunk_new(total, CHUNK_LIST) : -1;
    uint8_t *map = NULL;
    if (k >= 0) {
        chunks[k].used = UINT64_MAX;   // not a snapshot chunk
        VmaAllocationInfo info;
        vmaGetAllocationInfo(vk_ctx()->vma, chunks[k].alloc, &info);
        map = info.pMappedData;
    }
    // the copies out of guest memory, all under one hold of memory.c's freeze lock
    while (ccapd < ccapm) grow((void **)&capd, &ccapd, sizeof *capd);
    while (ccapsrc < ccapm) grow((void **)&capsrc, &ccapsrc, sizeof *capsrc);
    while (ccapok < ccapm) grow((void **)&capok, &ccapok, sizeof *capok);
    int nc = 0;
    for (int i = 0; map && i < n; i++)
        if (caps[i] != UINT64_MAX) {
            capd[nc] = map + caps[i];
            capsrc[nc][0] = capm[i].r->import_lo + capm[i].off, capsrc[nc][1] = capm[i].size;
            nc++;
        }
    if (nc) mem_copy_readable(capd, (const uint64_t (*)[2])capsrc, nc, capok);
    npend = 0;
    for (int i = 0, c = 0; i < n;) {
        PoolRec *r = capm[i].r;
        int nr = 0;
        for (; i < n && capm[i].r == r; i++) {
            int got = map && caps[i] != UINT64_MAX && capok[c++];
            if (got) {
                if (nr == cregions) grow((void **)&mregions, &cregions, sizeof *mregions);
                mregions[nr++] = (VkBufferCopy){caps[i], capm[i].off, capm[i].size};
                mrep.cap_bytes += capm[i].size, mrep.cap_ranges++;
                continue;
            }
            range_add(&pend, &npend, &cpend, r, capm[i].off, capm[i].size);
            mrep.cap_left += capm[i].size;
        }
        if (nr) vkCmdCopyBuffer(cb, chunks[k].buf, r->mbuf, (uint32_t)nr, mregions);
    }
    if (map) vmaFlushAllocation(vk_ctx()->vma, chunks[k].alloc, 0, VK_WHOLE_SIZE);
    mrep.cap_t += qpc() - t0;
}

void vk_mem_mirror_prologue(VkCommandBuffer cb) {
    if (!npend && snl_copied == nsnl && !n_gpa_flip) return;
    mbarrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
             VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    mirror_capture(cb);
    if (npend) vk_mem_note_live();   // left to the GPU-time copy: the GPU reads guest memory late
    copy_ranges(cb, pend, npend, 1, &mrep.upload_bytes, &mrep.upload_copies);
    if (snl_copied < nsnl) {   // the snapshots read what the uploads above wrote
        if (npend) mbarrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                            VK_ACCESS_2_TRANSFER_READ_BIT);
        snap_copies(cb);
    }
    mbarrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
             VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    gpa_flip_cmds(cb);   // after the new mirrors' first copies
    npend = 0;
    mrep.prologues++;
}

// ---- verify mode
typedef struct { PoolRec *r; uint64_t off, len, dst; uint32_t mgen; } VSample;
typedef struct { uint32_t sub; int n; VkBuffer buf; VmaAllocation alloc; const uint8_t *map; VSample s[VERIFY_SAMPLES]; } VJob;
static int ver_i;
static uint64_t ver_off;

// The CPU wrote the page since the mirror's sync that mgen records: stamped later, or its write-watch bit is set.
static int page_dirty(uint64_t a, uint32_t mgen) {
    uint32_t *s = gen_slot(a, 0);
    if (s && __atomic_load_n(s, __ATOMIC_RELAXED) > mgen) return 1;
    uint64_t p[1];
    return mem_written_peek(a, 1ull << GEN_PAGE_SHIFT, p, 1) != 0;   // UINT64_MAX (not watched) counts as written
}

static void verify_cb(void *arg) {   // completion thread
    VJob *j = arg;
    static volatile int32_t logged;
    uint64_t pages = 0, skipped = 0, bad = 0;
    for (int i = 0; i < j->n; i++) {
        const VSample *s = &j->s[i];
        PoolRec *r = s->r;
        uint32_t wb = __atomic_load_n(&r->wb_sub, __ATOMIC_RELAXED);
        if (wb != j->sub && wb - j->sub < 0x80000000u) {   // a later submission wrote back into it
            skipped += s->len >> GEN_PAGE_SHIFT;
            continue;
        }
        for (uint64_t o = 0; o < s->len; o += 1ull << GEN_PAGE_SHIFT) {
            uint64_t a = r->import_lo + s->off + o;
            const uint8_t *m = j->map + s->dst + o, *g = (const uint8_t *)(uintptr_t)a;
            pages++;
            if (!memcmp(m, g, 1ull << GEN_PAGE_SHIFT)) continue;
            // gen_lock: a write-watch query clears the page's bit and stamps it in one hold (vk_mem_changed, mirror_sync),
            // so the check sees one or the other; without it a page between the two counted as a mismatch.
            plat_mutex_lock_shared(&gen_lock);
            int dirty = page_dirty(a, s->mgen);
            plat_mutex_unlock_shared(&gen_lock);
            if (dirty) { skipped++; continue; }
            bad++;
            if (plat_inc32(&logged) <= VERIFY_LOG) {
                int k = 0, e = (1 << GEN_PAGE_SHIFT) - 1;
                while (k < e && m[k] == g[k]) k++;
                while (e > k && m[e] == g[e]) e--;
                char f[160];
                flag_names(r->flags, f, sizeof f);
                rt_log("mem: verify mismatch: pool %#llx+%#llx (%s) offset %#llx: bytes %#x..%#x of the page differ, "
                       "mirror %02x guest %02x at the first (submission %u)", (unsigned long long)r->lo,
                       (unsigned long long)r->size, f, (unsigned long long)(a - r->lo), k, e, m[k], g[k], j->sub);
                const uint32_t *gs = gen_slot(a & ~((1ull << GEN_PAGE_SHIFT) - 1), 0);
                char mh[100], gh[100];
                for (int b = 0; b < 16; b++) snprintf(mh + 3 * b, 4, "%02x ", m[b]), snprintf(gh + 3 * b, 4, "%02x ", g[b]);
                rt_log("mem: verify mismatch detail: written binds %llu, last %u; used %u; written back %u; gpa record %u; "
                       "page cpu stamp %u (synced to %u, now %u); mirror %s| guest %s",
                       (unsigned long long)r->vd_wrote_n, r->vd_wrote_sub, r->vd_use_sub, wb, r->gpa_rec, gs ? *gs : 0,
                       s->mgen, gen_now, mh, gh);
            }
        }
    }
    plat_add64(&v_pages, (int64_t)pages);
    plat_add64(&v_skipped, (int64_t)skipped);
    plat_add64(&v_mismatch, (int64_t)bad);
    vmaDestroyBuffer(vk_ctx()->vma, j->buf, j->alloc);
    free(j);
}

static int verify_record(VkCommandBuffer cb, uint64_t fence) {
    VJob *j = calloc(1, sizeof *j);
    if (!j) rt_fatal("vk: out of memory");
    uint64_t left = VERIFY_BYTES, dst = 0;
    plat_mutex_lock_shared(&pool_lock);
    for (int tries = 0; nm && left && j->n < VERIFY_SAMPLES && tries <= nm; tries++) {
        if (ver_i >= nm) ver_i = 0, ver_off = 0;
        PoolRec *r = mlist[ver_i];
        uint64_t size = r->import_hi - r->import_lo;
        if (ver_off >= size) { ver_i++, ver_off = 0; continue; }
        uint64_t len = size - ver_off < left ? size - ver_off : left;
        j->s[j->n++] = (VSample){r, ver_off, len, dst, r->mgen};
        dst += len, left -= len, ver_off += len;
        if (ver_off >= size) ver_i++, ver_off = 0;
    }
    plat_mutex_unlock_shared(&pool_lock);
    if (!j->n) { free(j); return 0; }
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = dst,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &j->buf, &j->alloc, &info) != VK_SUCCESS) { free(j); return 0; }
    j->map = info.pMappedData;
    j->sub = msub;
    mbarrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
             VK_ACCESS_2_TRANSFER_READ_BIT);
    for (int i = 0; i < j->n; i++) {
        VkBufferCopy c = {j->s[i].off, j->s[i].dst, j->s[i].len};
        vkCmdCopyBuffer(cb, j->s[i].r->mbuf, j->buf, 1, &c);
    }
    mbarrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
             VK_ACCESS_2_HOST_READ_BIT);
    vk_completion_add(fence, verify_cb, j);
    return j->n + 2;
}

int vk_mem_mirror_wb_pending(void) { return vk_mirror_on && (nsnl || nwbl); }

int vk_mem_mirror_end(VkCommandBuffer cb, uint64_t fence) {
    if (!vk_mirror_on) return 0;
    int rec = 0;
    lazy_part_check();   // before the merges are recorded: what the last part skipped and has to write back after all
    if (nsnl) {
        // the merge reads the mirror the submission's shaders wrote and writes the imported buffers
        mbarrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        vk_gpu_gap_note(VK_GAP_WB_MERGE, 0);
        int m = wb_merge_record(cb);
        if (!m) {   // no range list: copy them plainly
            for (int i = 0; i < nsnl; i++) range_add(&wbl, &nwbl, &cwbl, snl[i].r, snl[i].off, snl[i].size);
        }
        for (int i = 0; i < nsnl; i++) snl[i].r->wb_fence = fence;
        rec += m + 1;
        mbarrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    }
    if (shadow_on && (nswl || nwbl)) swb_job(fence);   // before snap_free takes back the marks' chunk
    if (nchunks) {   // the last part's prologue still records snapshot copies into them; back to the pool at the fence
        SnapFree *f = malloc(sizeof *f + sizeof(SnapChunk) * (size_t)nchunks);
        if (!f) rt_fatal("vk: out of memory");
        f->n = nchunks;
        memcpy(f->c, chunks, sizeof(SnapChunk) * (size_t)nchunks);
        vk_completion_add(fence, snap_free, f);
    }
    if (nsnl || nwbl) vk_exec_note_results(VK_RES_WRITEBACK);   // write-backs: guest memory gets the GPU's bytes at the fence
    if (nwbl) {
        mbarrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                 VK_ACCESS_2_TRANSFER_READ_BIT);
        for (int i = 0; i < nwbl; i++) wbl[i].r->wb_sub = msub, wbl[i].r->wb_fence = fence;
        vk_gpu_gap_note(VK_GAP_WB_COPY, 0);
        mrep.wb_ranges += (uint64_t)nwbl;
        rec += copy_ranges(cb, wbl, nwbl, 0, &mrep.wb_bytes, &mrep.wb_copies) + 2;
        mbarrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        nwbl = 0;
    }
    if (mirror_verify) rec += verify_record(cb, fence);
    plat_mutex_lock(&mirror_stat_lock);
    uint64_t *d = (uint64_t *)&macc, *s = (uint64_t *)&mrep;
    for (size_t i = 0; i < sizeof macc / sizeof(uint64_t); i++) d[i] += s[i];
    plat_mutex_unlock(&mirror_stat_lock);
    memset(&mrep, 0, sizeof mrep);
    return rec;
}

// NX2NAVI_TRACE=perf, every 600 frames: the mirrored pools that moved the most bytes since the last list, per frame: what
// their CPU writes and flushes copied up, how often a submission synced them, and what the GPU's writes sent back
// (merged or copied). A pool whose upload is close to its size every frame is rewritten whole each frame.
#define POOL_LOG_TOP 12
static uint64_t pool_score(const PoolRec *r) { return r->st_pages + r->st_flush + r->st_wb + r->st_merge; }
static void mirror_pool_log(uint64_t frame) {
    static uint64_t last;
    double fr = frame > last ? (double)(frame - last) : 1.0;
    last = frame;
    PoolRec *top[POOL_LOG_TOP];
    int nt = 0;
    uint64_t all_up = 0;
    plat_mutex_lock_shared(&pool_lock);
    for (int i = 0; i < nm; i++) {
        PoolRec *r = mlist[i];
        all_up += r->st_pages + r->st_flush;
        uint64_t sc = pool_score(r);
        if (!sc || (nt == POOL_LOG_TOP && sc <= pool_score(top[nt - 1]))) continue;
        int k = nt < POOL_LOG_TOP ? nt++ : nt - 1;   // insertion into top[], largest first
        for (; k > 0 && pool_score(top[k - 1]) < sc; k--) top[k] = top[k - 1];
        top[k] = r;
    }
    rt_log("mem: mirror pools frame %llu: %d mirrored, %.1f MiB a frame copied up in all; the top %d (MiB a frame: written pages, "
           "flush ranges (overlap with the pages counted twice), syncs a frame, GPU writes back merged / copied):",
           (unsigned long long)frame, nm, all_up / fr / 1048576.0, nt);
    for (int i = 0; i < nt; i++) {
        const PoolRec *r = top[i];
        char f[160];
        flag_names(r->flags, f, sizeof f);
        rt_log("mem:   pool %#llx %.1f MiB %s: up %.2f + %.2f, %.1f syncs, back %.2f / %.2f", (unsigned long long)r->lo,
               r->size / 1048576.0, f, r->st_pages / fr / 1048576.0, r->st_flush / fr / 1048576.0, r->st_syncs / fr,
               r->st_merge / fr / 1048576.0, r->st_wb / fr / 1048576.0);
    }
    for (int i = 0; i < nm; i++) {
        PoolRec *r = mlist[i];
        r->st_pages = r->st_flush = r->st_wb = r->st_merge = 0, r->st_syncs = 0;
    }
    plat_mutex_unlock_shared(&pool_lock);
}

// One summary line: with NX2NAVI_TRACE=perf every 60 frames, else at frames 600 and 1800.
static void mirror_log(uint64_t frame) {
    static uint64_t last, ww_last;
    plat_mutex_lock(&gen_lock);
    uint64_t wc = ww_calls, wb = ww_bytes, wt = ww_ticks, dc = ww_dirty_calls, dt = ww_dirty_ticks, wp = ww_pages;
    ww_pages = 0;
    uint64_t nr = n_frz_refused;
    ww_calls = ww_bytes = ww_ticks = ww_dirty_calls = ww_dirty_ticks = n_frz_refused = 0;
    plat_mutex_unlock(&gen_lock);
    double wf = frame > ww_last ? (double)(frame - ww_last) : 1.0;
    ww_last = frame;
    uint64_t fz, ff, ft, fo, fh;
    mem_freeze_stats(&fz, &ff, &ft, &fo, &fh);
    mem_probe_report(frame);
    mem_lazy_report(frame, lz_skipped);
    lz_skipped = 0;
    // NX2NAVI_WB_LAZY_TOUCH=<n> (test, 0 off): at each of these lines this thread reads a byte of n armed ranges, as
    // the game's CPU might, so the fault, the fetch and the hold run without waiting for the game to do it.
    static int touch = -1;
    if (touch < 0) touch = cfg_int("NX2NAVI_WB_LAZY_TOUCH");
    if (touch > 0) {
        uint64_t at[16];
        int k = mem_lazy_pick(at, touch > 16 ? 16 : touch);
        for (int i = 0; i < k; i++) {
            volatile uint8_t v = *(volatile const uint8_t *)(uintptr_t)at[i];
            (void)v;
        }
    }
    if (rt_trace & RT_TRACE_PERF)
        rt_log("mem: write-watch frame %llu: per frame, every consumer: %.1f queries over %.1f MiB, %.3f ms (%.1f found "
               "%.0f written pages, %.3f ms); frozen %.0f MiB; since the last line: freezes %llu, thaws by a write %llu, by a file read "
               "or mapping change %llu, refused %llu; quick thaws that held a hot region thawed %llu",
               (unsigned long long)frame, wc / wf, wb / wf / 1048576.0, qpc_ms(wt) / wf, dc / wf, wp / wf, qpc_ms(dt) / wf,
               fz * 2.0, (unsigned long long)ff, (unsigned long long)ft, (unsigned long long)fo, (unsigned long long)nr,
               (unsigned long long)fh);
    if (!vk_mirror_on) return;
    plat_mutex_lock(&mirror_stat_lock);
    MirrorAcc a = macc;
    memset(&macc, 0, sizeof macc);
    plat_mutex_unlock(&mirror_stat_lock);
    double f = frame > last ? (double)(frame - last) : 1.0, M = 1048576.0;
    last = frame;
    uint64_t fc = (uint64_t)plat_xchg64(&m_flush_calls, 0), fb = (uint64_t)plat_xchg64(&m_flush_bytes, 0);
    char ver[160] = "";
    if (mirror_verify)
        snprintf(ver, sizeof ver, "; verify: pages compared %lld, skipped %lld (written since), mismatches %lld",
                 (long long)plat_xchg64(&v_pages, 0), (long long)plat_xchg64(&v_skipped, 0),
                 (long long)plat_xchg64(&v_mismatch, 0));
    vk_gpa_report();
    rt_log("mem: mirror frame %llu: pools %lld, %.1f MiB device-local; stay imported: budget %lld (%.1f MiB), untracked "
           "%lld, overlapping %lld, failed %lld; per frame: submits %.2f, syncs %.1f, upload %.3f MiB in %.1f ranges, "
           "first use %.2f pools %.3f MiB, write-back %.3f MiB in %.1f ranges, merge %.3f MiB in %.1f dispatches "
           "(snapshots %.3f MiB, new buffers %.2f), flush calls %.1f (%.3f MiB), queries "
           "%.1f pre-snap (%.1f MiB) + %.1f sync, prologues %.2f, tracking cpu %.3f ms (pre-snap %.3f, syncs %.3f); so far "
           "evicted %lld (%.1f MiB), budget refusals asked again %lld, made without a twin %lld (%.1f MiB)%s",
           (unsigned long long)frame, (long long)m_live_n, m_live_bytes / M, (long long)m_refused_n[MR_BUDGET],
           m_refused_bytes[MR_BUDGET] / M, (long long)m_refused_n[MR_UNTRACKED], (long long)m_refused_n[MR_OVERLAP],
           (long long)m_refused_n[MR_ALLOC], a.subs / f, a.syncs / f, a.upload_bytes / f / M, a.upload_copies / f,
           a.first_n / f, a.first_bytes / f / M, a.wb_bytes / f / M, a.wb_copies / f, a.merge_bytes / f / M,
           a.merge_dispatches / f, a.snap_bytes / f / M, a.snap_allocs / f, fc / f, fb / f / M,
           a.presnap_queries / f, a.presnap_bytes / f / M, a.sync_queries / f, a.prologues / f, qpc_ms(a.presnap_t + a.sync_t) / f,
           qpc_ms(a.presnap_t) / f, qpc_ms(a.sync_t) / f, (long long)m_evicted_n, m_evicted_bytes / M, (long long)m_retries,
           (long long)m_untwinned_n, m_untwinned_bytes / M, ver);
    char capl[240] = "";
    if (wb_skip()) {
        snprintf(capl, sizeof capl, "; written ranges kept in the mirror alone %.3f MiB",
                 (uint64_t)plat_xchg64(&m_wb_skipped, 0) / f / M);
    }
    if (capture_cap())
        snprintf(capl + strlen(capl), sizeof capl - strlen(capl), "; copied by the CPU at hand-over %.3f MiB in %.1f "
                 "ranges, %.3f ms; left to the GPU copy %.3f MiB", a.cap_bytes / f / M, a.cap_ranges / f,
                 qpc_ms(a.cap_t) / f, a.cap_left / f / M);
    rt_log("mem: mirror frame %llu: upload sources per frame, overlaps counted twice: written pages %.3f MiB; flush ranges "
           "%.2f, %.3f MiB, from %.1f flushes of %.3f MiB; flushed and not copied (write-watched) %.3f MiB%s",
           (unsigned long long)frame, a.page_bytes / f / M, a.flush_n / f,
           a.flush_bytes / f / M, a.flush_calls / f, a.flush_sum / f / M, a.flush_skipped / f / M, capl);
    if (n_gpa_late)
        rt_log("mem: mirror frame %llu: the device address table took up %llu mirrors in queue order, after their first "
               "copy, so far (NX2NAVI_GPA_LATE)", (unsigned long long)frame, (unsigned long long)n_gpa_late);
}

// After vram_log: every heap's usage as the driver reports it (VK_EXT_memory_budget) beside VMA's share of it, VMA by
// memory type, and the process's private commit with its write-combined part (host-visible memory the driver maps
// uncached), to tell which memory a long session grows. When the write-combined total (VMA's blocks where that is not
// known) grew by 2 GiB since the last look, VMA's detailed map goes to vma-frame<N>.json beside the log, 4 at most.
// The process's part walks the whole address space (plat_mem_census), 100-240 ms in a long session, so it runs on a
// thread of its own (census_main) and logs a line of its own; a look while the last walk still runs goes without it.
typedef struct { VkCtx *c; uint64_t frame; } CensusJob;
static volatile int32_t census_busy;

static void census_main(void *arg) {
    CensusJob j = *(CensusJob *)arg;
    free(arg);
    const double M = 1048576.0;
    uint64_t priv = 0, wc = 0, wcn = 0, now;
    int have = plat_mem_census(&priv, &wc, &wcn);
    if (have) {
        rt_log("mem: process frame %llu: private %.0f MiB, write-combined %.0f MiB in %llu allocations",
               (unsigned long long)j.frame, priv / M, wc / M, (unsigned long long)wcn);
        now = wc;
    } else {
        VmaBudget vb[VK_MAX_MEMORY_HEAPS];
        vmaGetHeapBudgets(j.c->vma, vb);
        now = 0;
        for (uint32_t h = 0; h < j.c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++) now += vb[h].statistics.blockBytes;
    }
    static uint64_t base;
    static int dumps = -1;   // -1: no baseline yet
    if (dumps < 0) base = now, dumps = 0;
    else if (dumps < 4 && now >= base + (2ull << 30) && rt_log_path()) {
        base = now, dumps++;
        const char *lp = rt_log_path(), *sl = strrchr(lp, '/'), *bs = strrchr(lp, '\\');
        if (bs && (!sl || bs > sl)) sl = bs;
        char path[1024];
        snprintf(path, sizeof path, "%.*svma-frame%llu.json", sl ? (int)(sl - lp) + 1 : 0, lp, (unsigned long long)j.frame);
        char *json = NULL;
        vmaBuildStatsString(j.c->vma, &json, VK_TRUE);
        // Through a temporary file and a rename: nobody waits for this thread when the game closes, and a map cut short
        // there would read as a whole one
        int ok = json && plat_file_write_cached(path, json, strlen(json));
        rt_log("mem: vma detailed map %s %s", ok ? "written to" : "not written:", path);
        if (json) vmaFreeStatsString(j.c->vma, json);
    }
    plat_xchg32(&census_busy, 0);
}

static void heap_log(VkCtx *c, uint64_t frame, const VmaBudget *vb) {
    const double M = 1048576.0;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    if (c->has_memory_budget) {
        VkPhysicalDeviceMemoryProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &bp};
        vkGetPhysicalDeviceMemoryProperties2(c->phys, &p2);
    }
    char line[1536];
    int o = snprintf(line, sizeof line, "mem: heaps frame %llu:", (unsigned long long)frame);
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS && o < (int)sizeof line - 160; h++)
        o += snprintf(line + o, sizeof line - (size_t)o, " heap %u (%s) used %.0f of %.0f MiB, vma %u blocks %.0f MiB, %u allocations %.0f MiB;",
                      h, c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? "device" : "host",
                      (c->has_memory_budget ? bp.heapUsage[h] : vb[h].usage) / M,
                      (c->has_memory_budget ? bp.heapBudget[h] : vb[h].budget) / M, vb[h].statistics.blockCount,
                      vb[h].statistics.blockBytes / M, vb[h].statistics.allocationCount, vb[h].statistics.allocationBytes / M);
    rt_log("%s", line);
    VmaTotalStatistics ts;
    vmaCalculateStatistics(c->vma, &ts);
    o = snprintf(line, sizeof line, "mem: vma by type frame %llu:", (unsigned long long)frame);
    for (uint32_t t = 0; t < c->mem_props.memoryTypeCount && t < VK_MAX_MEMORY_TYPES && o < (int)sizeof line - 120; t++) {
        const VmaDetailedStatistics *d = &ts.memoryType[t];
        if (!d->statistics.blockCount) continue;
        o += snprintf(line + o, sizeof line - (size_t)o,
                      " type %u: %u blocks %.0f MiB, %u allocations %.0f MiB, largest %.1f MiB, %u free ranges (largest %.1f MiB);",
                      t, d->statistics.blockCount, d->statistics.blockBytes / M, d->statistics.allocationCount,
                      d->statistics.allocationBytes / M, d->allocationSizeMax / M, d->unusedRangeCount,
                      d->unusedRangeCount ? d->unusedRangeSizeMax / M : 0.0);
    }
    rt_log("%s", line);
    // The address-space walk of the census (plat_mem_census, 100-240 ms of VirtualQuery) runs on its own thread, but
    // it holds the process's address-space lock, which the game's threads take at every page fault and every
    // VirtualAlloc: the present after each walk arrived 65-120 ms late. NX2NAVI_MEM_CENSUS=1 asks for it.
    static int census_on = -1;
    if (census_on < 0) census_on = cfg_bool("NX2NAVI_MEM_CENSUS");
    if (!census_on || plat_xchg32(&census_busy, 1)) return;
    CensusJob *j = malloc(sizeof *j);
    if (j) *j = (CensusJob){c, frame};
    if (!j || !plat_thread_spawn(census_main, j, "mem census", 0)) free(j), plat_xchg32(&census_busy, 0);
}

// After the vram line: the imported pools the GPU reached through a resolve (a binding, copy or texture placement) and
// those it never did, which hold host memory the driver counts in the host heap all the same (the 4.3 GiB imported
// at the title screen exceed the 3.9 GiB host heap of an integrated GPU with 16 GB of RAM); then the VRAM mirrors no
// submission synced for MIRROR_IDLE submissions, which a scene that needs the room could take back.
static void pool_census_log(uint64_t frame) {
    uint64_t un = 0, ub = 0, nn = 0, nb = 0, mn = 0, mb = 0, in = 0, ib = 0;
    uint32_t sub = __atomic_load_n(&msub, __ATOMIC_RELAXED);   // the replay's; a log line may read it a submission late
    plat_mutex_lock_shared(&pool_lock);
    for (int i = 0; i < pool_index.n; i++) {
        const PoolRec *r = pool_index.e[i].buf;
        if (r->kind != POOL_IMPORT || !r->buf) continue;
        uint64_t n = r->import_hi - r->import_lo;
        if (__atomic_load_n(&r->used, __ATOMIC_RELAXED)) un++, ub += n;
        else nn++, nb += n;
        if (r->mstate != MS_ON) continue;
        mn++, mb += n;
        if (sub - r->acc_sub >= MIRROR_IDLE) in++, ib += n;
    }
    plat_mutex_unlock_shared(&pool_lock);
    const double M = 1048576.0;
    rt_log("mem: pools frame %llu: imported %llu %.0f MiB resolved by the GPU, %llu %.0f MiB never; mirrors %llu %.0f MiB, "
           "idle for %d submissions %llu %.0f MiB", (unsigned long long)frame, (unsigned long long)un, ub / M,
           (unsigned long long)nn, nb / M, (unsigned long long)mn, mb / M, MIRROR_IDLE, (unsigned long long)in, ib / M);
}

// After the heap lines: the driver's figure for the device-local heaps split by what holds it. VMA's blocks hold the
// textures with their own memory (live and released), the VRAM mirrors, the physical pools, the ASTC decoder's scratch,
// the headroom's spare blocks (their anchors), everything else VMA placed, and the free space inside the blocks, which
// the driver counts as used until a block empties. The rest of the driver's figure was never allocated through VMA:
// the swapchain, DLSS and frame generation (Streamline), compiled pipelines and the driver's own memory; DLSS's share
// as its statistics report it, when known.
static void vram_use_log(VkCtx *c, uint64_t frame, const VmaBudget *vb, const VkTexCensus *t) {
    uint64_t usage = 0, blocks = 0, allocs = 0, phys = 0;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    if (c->has_memory_budget) {
        VkPhysicalDeviceMemoryProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &bp};
        vkGetPhysicalDeviceMemoryProperties2(c->phys, &p2);
    }
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        if (c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            usage += c->has_memory_budget ? bp.heapUsage[h] : vb[h].statistics.blockBytes,
                blocks += vb[h].statistics.blockBytes, allocs += vb[h].statistics.allocationBytes;
    plat_mutex_lock_shared(&pool_lock);
    for (int i = 0; i < nphys; i++)
        if (phys_list[i]->alloc) phys += phys_list[i]->size;
    plat_mutex_unlock_shared(&pool_lock);
    uint64_t mir = (uint64_t)__atomic_load_n(&m_live_bytes, __ATOMIC_RELAXED), held = vk_headroom_held(),
             scratch = vk_astc_scratch_bytes();
    uint64_t known = t->bytes_live + t->bytes_released + mir + phys + held + scratch;
    int64_t dlss = vk_dlaa_vram();
    char ds[48] = "n/a";
    if (dlss >= 0) snprintf(ds, sizeof ds, "%.0f MiB", dlss / 1048576.0);
    const double M = 1048576.0;
    rt_log("mem: vram by use frame %llu: in use %.0f MiB = VMA blocks %.0f MiB (textures %.0f, released textures %.0f, "
           "VRAM mirrors %.0f, physical pools %.0f, ASTC scratch %.0f, headroom spare %.0f, other %.0f, free inside the "
           "blocks %.0f) + outside VMA %.0f MiB (swapchain, DLSS and frame generation, pipelines, driver; DLSS reports %s)",
           (unsigned long long)frame, usage / M, blocks / M, t->bytes_live / M, t->bytes_released / M, mir / M, phys / M,
           scratch / M, held / M, allocs > known ? (allocs - known) / M : 0.0, blocks > allocs ? (blocks - allocs) / M : 0.0,
           usage > blocks ? (usage - blocks) / M : 0.0, ds);
}

// The VRAM mirrors that lie in VMA's blocks, for the texture block table (vk_texture.c "compaction"): a mirror in a
// block keeps the block. malloc'd, *n rows; NULL with none. Read under the shared pool lock: a mirror comes onto the
// list and leaves it, with its allocation, under the exclusive one (mirror_create, mirror_evict, mirror_forget).
static VkBlockRow *mirror_rows(VkCtx *c, int *n) {
    VkBlockRow *rows = NULL;
    *n = 0;
    plat_mutex_lock_shared(&pool_lock);
    if (nm) rows = malloc(sizeof *rows * (size_t)nm);
    for (int i = 0; rows && i < nm; i++) {
        const PoolRec *r = mlist[i];
        if (!r->malloc_) continue;
        VmaAllocationInfo2 ai;
        vmaGetAllocationInfo2(c->vma, r->malloc_, &ai);
        if (ai.dedicatedMemory) continue;   // memory of its own: in no block
        rows[(*n)++] = (VkBlockRow){(uint64_t)(uintptr_t)ai.allocationInfo.deviceMemory, ai.blockSize,
                                    ai.allocationInfo.size, ai.allocationInfo.memoryType};
    }
    plat_mutex_unlock_shared(&pool_lock);
    return rows;
}

// NX2NAVI_TEX_COMPACT, when vk_texture_compact_due says the holes are due a look: the texture cache gets the mirrors'
// rows, taken here because the pool lock is not to be taken under the texture lock, and plans a pass or not
// (vk_texture.c vk_texture_compact_plan).
static void compact_look(void) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    int n;
    VkBlockRow *rows = mirror_rows(c, &n);
    vk_texture_compact_plan(rows, n);
    free(rows);
}

// Every HOLES_PERIOD frames (600 with NX2NAVI_TRACE=perf), in every run: how much of VMA's device-local blocks is
// holes, and how much of that a compaction of the textures could give back. The first two figures are VMA's own
// (blocks and allocations, memory of its own counted in both). The rest is of the memory type the textures use most,
// from the block table (vk_texture.c compact_table): the blocks that hold a texture, with the textures and VRAM
// mirrors in them; what is left of those blocks is holes and whatever else VMA placed there, which the table cannot
// tell apart. A block at most half full by what is listed is "sparse"; when everything listed in it is a texture that
// could be copied elsewhere or dropped (vk_tex_compact.h), emptying it gives the whole block back. That last sum is the
// most a compaction could return now: less when such a block holds memory of another kind too.
// Without the perf trace the vram lines above are not logged, and a player's log of a long session said nothing about
// the holes: four traced sessions of one PC ended with 1.4 to 2.2 GiB free inside 8.3 to 9.6 GiB of blocks.
static void holes_log(uint64_t frame) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    int n;
    VkBlockRow *rows = mirror_rows(c, &n);
    VkTexHoles h;
    vk_texture_holes(rows, n, &h);
    free(rows);
    VmaBudget vb[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(c->vma, vb);
    uint64_t blocks = 0, allocs = 0;
    for (uint32_t i = 0; i < c->mem_props.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; i++)
        if (c->mem_props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            blocks += vb[i].statistics.blockBytes, allocs += vb[i].statistics.allocationBytes;
    const double M = 1048576.0;
    uint64_t listed = h.tex_bytes + h.other_bytes;
    rt_log("mem: holes frame %llu: device-local VMA blocks %.0f MiB hold %.0f MiB of allocations, %.0f MiB free inside "
           "them; memory type %u: %u blocks with textures, %.0f MiB: textures %.0f MiB (%.0f MiB could move, %.0f MiB "
           "be dropped%s), VRAM mirrors %.0f MiB, free or of another kind %.0f MiB; %u of them at most half full, %.0f "
           "MiB holding %.0f MiB; %u of those, %.0f MiB, hold nothing but textures that could move (%.0f MiB) or be "
           "dropped (%.0f MiB)", (unsigned long long)frame, blocks / M, allocs / M,
           blocks > allocs ? (blocks - allocs) / M : 0.0, h.type, h.blocks, h.block_bytes / M, h.tex_bytes / M,
           h.move_bytes / M, h.drop_bytes / M, h.marks ? "" : "; the texture pool's marks are not in use, so none counts "
           "as unsampled", h.other_bytes / M, h.block_bytes > listed ? (h.block_bytes - listed) / M : 0.0, h.sparse,
           h.sparse_bytes / M, h.sparse_listed / M, h.emptiable, h.emptiable_bytes / M, h.emptiable_move / M,
           h.emptiable_drop / M);
}

// With NX2NAVI_TRACE=perf every 60 frames: a line when the device-local memory VMA did not allocate (vram_use_log's
// "outside VMA") moved by OUTSIDE_STEP or more since the last line, so the log places it beside what did it. In one
// session it grew from 0.6 to 2.6 GiB at the title screen, in two steps of about 1 GiB while the boot's compute
// pipelines were being built (vk_pipeline.c, stage library prebuild), and DLSS and frame generation add theirs when
// they start.
#define OUTSIDE_STEP (256ull << 20)
static void outside_watch(uint64_t frame) {
    VkCtx *c = vk_ctx();
    if (!c || !c->has_memory_budget) return;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &bp};
    vkGetPhysicalDeviceMemoryProperties2(c->phys, &p2);
    VmaBudget vb[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(c->vma, vb);
    uint64_t usage = 0, blocks = 0;
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        if (c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            usage += bp.heapUsage[h], blocks += vb[h].statistics.blockBytes;
    uint64_t out = usage > blocks ? usage - blocks : 0;
    static uint64_t last;
    static int seen;
    if (seen && (out > last ? out - last : last - out) < OUTSIDE_STEP) return;
    const double M = 1048576.0;
    rt_log("mem: frame %llu: device memory outside VMA (swapchain, DLSS and frame generation, pipelines, driver) %s%.0f "
           "MiB (%s%.0f MiB)", (unsigned long long)frame, seen ? "now " : "", out / M, out >= last ? "+" : "-",
           (out >= last ? out - last : last - out) / M);
    last = out, seen = 1;
}

// The overlay's "game" figure beside the card's memory in use (perfmon.c, its sampling thread): what the driver
// counts for this process on the device-local heaps, the same figure vram_use_log splits by use.
// With the device lost report (vk_device.c): the video memory in use and the budget the driver gives the game. A lost
// device with the memory at or over its budget points at the memory; one far below it does not, and the report of a
// card with 8 GB that ended this way held neither number.
void vk_mem_lost_report(void) {
    uint64_t used, budget;
    int exact;
    if (!vram_query(&used, &budget, &exact)) return;
    rt_log("vk: video memory when the device was lost: %llu MiB in use of a budget of %llu MiB (%s)",
           (unsigned long long)(used >> 20), (unsigned long long)(budget >> 20),
           exact ? "the driver's figures, every program counted in the budget" : "this game's own allocations only");
}

// The settings that change how the game's memory reaches the GPU, as they are in effect, for a fatal error's summary
// line (vk_device.c fatal_note): the memory mode and the lazy write-back with its reason and state. No lock, any thread.
void vk_mem_fatal_note(char *out, size_t n) {
    char lazy[160];
    mem_lazy_note(lazy, sizeof lazy);
    int st = __atomic_load_n(&lz_state, __ATOMIC_ACQUIRE), want = __atomic_load_n(&lz_wanted, __ATOMIC_ACQUIRE);
    snprintf(out, n, "memory mode %s%s, lazy write-back %s (%s%s%s)", mirror_verify ? "verify" : vk_mirror_on ? "mirror" : "import",
             shadow_on ? " with shadow storage" : "", want < 0 ? "not decided" : st == 2 && want ? "failed to start" : want ? "on" : "off",
             lz_why, *lazy ? "; " : "", lazy);
}

float vk_mem_device_used_mb(void) {
    VkCtx *c = vk_ctx();
    if (!c || !c->has_memory_budget) return -1;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &bp};
    vkGetPhysicalDeviceMemoryProperties2(c->phys, &p2);
    uint64_t usage = 0;
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        if (c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) usage += bp.heapUsage[h];
    return (float)(usage / 1048576.0);
}

// One line per boot stage (vk_pipeline.c calls it after the pipeline cache load, the shader prewarm, the recorded
// pipelines and the compute pipeline prebuild): the device-local memory in use, the part of it VMA did not allocate
// (outside_watch's "outside VMA": pipelines and the driver's own, before the swapchain and the upscaler exist), and the
// process's commit and working set. Subtracting two lines gives what a stage left behind: on an installed copy the
// compute pipeline prebuild alone left 2.1 GiB of device memory on an RTX 5090, held by twenty very large compute
// shaders no run dispatches (vk_pipeline.c, "compute pipelines the boot keeps"), which this line was added to place.
void vk_mem_stage_log(const char *stage) {
    VkCtx *c = vk_ctx();
    double proc, used, limit;
    float ram_used, ram_total, ws;
    plat_commit_info(&proc, &used, &limit);
    plat_mem_info(&ram_used, &ram_total, &ws);
    const double M = 1048576.0;
    uint64_t usage = 0, blocks = 0;
    int budget = c && c->has_memory_budget;
    if (budget) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT bp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &bp};
        vkGetPhysicalDeviceMemoryProperties2(c->phys, &p2);
        VmaBudget vb[VK_MAX_MEMORY_HEAPS] = {0};
        if (c->vma) vmaGetHeapBudgets(c->vma, vb);
        for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
            if (c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                usage += bp.heapUsage[h], blocks += vb[h].statistics.blockBytes;
    }
    char dev[96] = "device memory unknown (no VK_EXT_memory_budget)";
    if (budget)
        snprintf(dev, sizeof dev, "device memory in use %.0f MiB, outside VMA %.0f MiB", usage / M,
                 usage > blocks ? (usage - blocks) / M : 0.0);
    rt_log("mem: boot stage, after %s: %s; process commit %.0f MB, working set %.0f MB; system commit %.0f of %.0f MB",
           stage, dev, proc, (double)ws, used, limit);
}

// With NX2NAVI_TRACE=perf every 600 frames: what holds device-local memory, to see what grows over a long session.
// VMA's blocks and allocations on the device-local heaps, texture entries with their own memory, the VRAM mirrors and
// the objects waiting for their last submission to retire.
static void vram_log(uint64_t frame) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    VmaBudget vb[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(c->vma, vb);
    uint64_t blocks = 0, block_bytes = 0, allocs = 0, alloc_bytes = 0;
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        if (c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            blocks += vb[h].statistics.blockCount, block_bytes += vb[h].statistics.blockBytes,
                allocs += vb[h].statistics.allocationCount, alloc_bytes += vb[h].statistics.allocationBytes;
    VkTexCensus t;
    vk_texture_census(&t);
    const double M = 1048576.0;
    rt_log("mem: vram frame %llu: vma %llu blocks %.0f MiB, %llu allocations %.0f MiB; textures live %llu (own memory %llu, "
           "%.0f MiB), released %llu (own memory %llu, %.0f MiB); mirrors %lld %.0f MiB; releases pending %d",
           (unsigned long long)frame, (unsigned long long)blocks, block_bytes / M, (unsigned long long)allocs,
           alloc_bytes / M, (unsigned long long)t.live, (unsigned long long)t.own_live, t.bytes_live / M,
           (unsigned long long)t.released, (unsigned long long)t.own_released, t.bytes_released / M, (long long)m_live_n,
           m_live_bytes / M, dead_pending());
    vk_texture_census_log(frame);
    pool_census_log(frame);
    heap_log(c, frame, vb);
    vram_use_log(c, frame, vb, &t);
}

// Device class: INTEGRATED -> uma; a discrete GPU with a DEVICE_LOCAL heap of 1 GiB or more beside a
// heap that is not device-local (host memory) -> discrete; otherwise uma (import everywhere). Sets the mirror mode.
void vk_mem_class_log(const VkPhysicalDeviceProperties *p, const VkPhysicalDeviceMemoryProperties *m) {
    int big_local = -1, host_heap = -1;
    for (uint32_t h = 0; h < m->memoryHeapCount; h++) {
        if (m->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            if (m->memoryHeaps[h].size >= (1ull << 30) && big_local < 0) big_local = (int)h;
        } else if (host_heap < 0) host_heap = (int)h;
    }
    static const char *const types[] = {"other", "integrated", "discrete", "virtual", "cpu"};
    int t = (int)p->deviceType;
    const char *tn = t >= 0 && t <= 4 ? types[t] : "unknown";
    const char *cls, *why;
    if (p->deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) cls = "uma", why = "integrated gpu";
    else if (p->deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) cls = "uma", why = "not a discrete gpu";
    else if (big_local < 0) cls = "uma", why = "no device-local heap of 1 GiB or more";
    else if (host_heap < 0) cls = "uma", why = "every heap is device-local";
    else cls = "discrete", why = "device-local heap separate from host memory";
    int discrete = cls[0] == 'd';
    char line[2048];
    size_t k = (size_t)snprintf(line, sizeof line, "mem: class %s (%s; device type %d %s, %s)", cls, why, t, tn, p->deviceName);
    for (uint32_t h = 0; h < m->memoryHeapCount; h++)
        k += (size_t)snprintf(line + k, k < sizeof line ? sizeof line - k : 0, "; heap %u %.0f MiB flags %#x%s%s", h,
                              m->memoryHeaps[h].size / 1048576.0, m->memoryHeaps[h].flags,
                              m->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " DEVICE_LOCAL" : "",
                              m->memoryHeaps[h].flags & VK_MEMORY_HEAP_MULTI_INSTANCE_BIT ? " MULTI_INSTANCE" : "");
    rt_log("%s", line);
    k = (size_t)snprintf(line, sizeof line, "mem: types");
    for (uint32_t i = 0; i < m->memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = m->memoryTypes[i].propertyFlags;
        k += (size_t)snprintf(line + k, k < sizeof line ? sizeof line - k : 0, "; %u heap %u flags %#x%s%s%s%s%s", i,
                              m->memoryTypes[i].heapIndex, f, f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? " DEVICE_LOCAL" : "",
                              f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? " HOST_VISIBLE" : "",
                              f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? " HOST_COHERENT" : "",
                              f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? " HOST_CACHED" : "",
                              f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT ? " LAZY" : "");
    }
    rt_log("%s", line);
    mirror_mode(discrete, vk_caps.driver_id == VK_DRIVER_ID_NVIDIA_PROPRIETARY);
}

// ---------------------------------------------------------------- upload batches and deferred destruction
typedef struct { VkBuffer buf; VmaAllocation alloc; VkDeviceSize size; } Staging;   // size: the VkBuffer's own
typedef struct { VkBuffer buf; VmaAllocation alloc; uint8_t *map; } StgChunk;          // a staging arena chunk
typedef struct {
    VkCommandBuffer cb;
    VkFence fence;
    uint64_t seq;
    Staging *stg;
    int nstg, cap;
    StgChunk *chunk;          // arena chunks this batch's uploads took ranges of (vk_staging_at), the last one open
    int nchunk, cap_chunk;
    VkDeviceSize chunk_used;  // bytes of the last chunk handed out
    uint64_t bytes;   // staging bytes of its buffers (upload_cap)
    int ts;           // its pair of upload timestamps (up_ts_*), -1: not timed
    uint64_t sub_ns;  // when it was handed to the queue (plat_now_ns), for the GPU timeline (vk_gtl_note)
} Batch;

enum { DEAD_IMAGE = 1, DEAD_IMAGE_VMA, DEAD_VIEW, DEAD_BUFFER_VMA, DEAD_HOST };
typedef struct { int kind; uint64_t handle; VmaAllocation alloc; uint64_t seq, tl; } Dead;

static PlatRMutex cmd_lock;
static PlatOnce cmd_once = PLAT_ONCE_INIT;
static VkCommandPool cmd_pool;
static Batch open_b;
static int open_active;
static Batch *inflight;
static int ninflight, cap_inflight;
static Batch *spare;                  // completed batches whose command buffer and fence are reused

// ---- upload GPU time (NX2NAVI_TRACE=perf): a timestamp at the start and the end of each upload batch, read when it
// retires. The replay's GPU time (vk_sync.c) times only its own parts; upload batches go to the queue as submissions of
// their own, with the ASTC decoder's dispatches in them, so their time showed only as GPU idle "with it queued".
// cmd_lock held throughout.
#define UP_TS_PAIRS 256
static VkQueryPool up_ts_pool;
static int up_ts_state;               // 0 not set up, 1 on, -1 off
static uint8_t up_ts_busy[UP_TS_PAIRS];
static uint32_t up_ts_next;
static uint64_t up_ts_ns, up_ts_n, up_ts_max_ns, up_ts_missed, up_n_batches;   // since the last log line

static int up_ts_open(VkCtx *c, VkCommandBuffer cb) {
    if (!(rt_trace & RT_TRACE_PERF) || up_ts_state < 0) return -1;
    if (!up_ts_state) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                    .queryCount = 2 * UP_TS_PAIRS};
        up_ts_state = c->props.limits.timestampComputeAndGraphics &&
                      vkCreateQueryPool(c->device, &qi, NULL, &up_ts_pool) == VK_SUCCESS ? 1 : -1;
        if (up_ts_state < 0) return -1;
    }
    uint32_t p = up_ts_next % UP_TS_PAIRS;
    if (up_ts_busy[p]) { up_ts_missed++; return -1; }
    up_ts_next++, up_ts_busy[p] = 1;
    vkCmdResetQueryPool(cb, up_ts_pool, 2 * p, 2);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, up_ts_pool, 2 * p);
    return (int)p;
}

static void up_ts_read(VkCtx *c, Batch *b) {   // b retired: its fence signalled
    if (b->ts < 0) return;
    uint64_t t[2];
    if (vkGetQueryPoolResults(c->device, up_ts_pool, 2 * (uint32_t)b->ts, 2, sizeof t, t, sizeof t[0],
                              VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] >= t[0]) {
        uint64_t ns = (uint64_t)((double)(t[1] - t[0]) * c->props.limits.timestampPeriod);
        up_ts_ns += ns, up_ts_n++;
        vk_gtl_note(GTL_UPLOAD, b->sub_ns, t[0], t[1]);
        if (ns > up_ts_max_ns) up_ts_max_ns = ns;
    }
    up_ts_busy[b->ts] = 0;
    b->ts = -1;
}

static void cmd_init(void *p);
static void up_ts_log(uint64_t frame) {   // every 60 frames
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    rt_log("perf: frame %llu: upload batches %.1f per frame, on the GPU %.2f ms per frame (%llu timed, %.3f ms each, the "
           "longest %.2f ms; %llu not timed)", (unsigned long long)frame, up_n_batches / 60.0, up_ts_ns / 60e6,
           (unsigned long long)up_ts_n, up_ts_n ? up_ts_ns / 1e6 / (double)up_ts_n : 0.0, up_ts_max_ns / 1e6,
           (unsigned long long)up_ts_missed);
    up_ts_ns = up_ts_n = up_ts_max_ns = up_ts_missed = up_n_batches = 0;
    plat_rmutex_unlock(&cmd_lock);
}
static int nspare, cap_spare;
static uint64_t next_seq, done_seq;   // batches handed out / completed (all batches up to done_seq)
static Dead *dead;
static int ndead, cap_dead;
static uint64_t tl_noted;             // newest timeline value vk_sync.c reported submitted

static void cmd_init(void *p) {
    (void)p;
    return;
}

static void grow(void **arr, int *cap, size_t elem) {
    *cap = *cap ? *cap * 2 : 16;
    void *n = realloc(*arr, (size_t)*cap * elem);
    if (!n) rt_fatal("vk: out of memory (upload bookkeeping)");
    *arr = n;
}

// Staging buffers of a finished batch go back to a cache instead of the allocator: an upload-heavy scene creates and
// retires the same few sizes thousands of times a frame (a texture re-upload, a texel copy, an ASTC decode), and each
// vmaCreateBuffer/vmaDestroyBuffer pair walks the pool's block list under its lock. Bounded in count and bytes; a buffer
// that does not fit is destroyed as before. The batch is complete when this runs, so nothing reads the bytes again.
// Sizes are the VkBuffers' own: an allocation can be larger than its buffer, and a copy reads only the buffer's range.
// cmd_lock held (retire, and vk_staging between vk_upload_begin and vk_upload_end).
#define STG_CACHE 24
#define STG_CACHE_BYTES (192ull << 20)
static Staging stg_cache[STG_CACHE];
static int stg_ncache;
static VkDeviceSize stg_cache_bytes;

// The staging arena: an upload of at most ARENA_MAX bytes takes an ARENA_ALIGN-aligned range of a persistent mapped
// chunk (vk_staging_at) instead of a buffer of its own. The chunks belong to the open batch while it records and go
// back to the free list when its fence signals, so a frame's few hundred small texture uploads cost a pointer bump
// each instead of a vmaCreateBuffer and vmaDestroyBuffer under the allocator's lock. ARENA_KEEP free chunks stay; the
// rest are destroyed. 256-byte alignment covers every bufferOffset rule of a buffer-to-image copy. NX2NAVI_STG_ARENA=0
// gives every upload its own buffer again.
#define ARENA_CHUNK (8ull << 20)
#define ARENA_MAX (ARENA_CHUNK / 4)
#define ARENA_ALIGN 256ull
#define ARENA_KEEP 8
static StgChunk arena_free[ARENA_KEEP];
static int narena_free;
static uint64_t arena_made, arena_ranges;

static void free_staging(VkCtx *c, Batch *b) {
    for (int i = 0; i < b->nstg; i++) {
        if (stg_ncache < STG_CACHE && stg_cache_bytes + b->stg[i].size <= STG_CACHE_BYTES) {
            stg_cache[stg_ncache++] = b->stg[i];
            stg_cache_bytes += b->stg[i].size;
            continue;
        }
        vmaDestroyBuffer(c->vma, b->stg[i].buf, b->stg[i].alloc);
    }
    b->nstg = 0;
    for (int i = 0; i < b->nchunk; i++) {
        if (narena_free < ARENA_KEEP) arena_free[narena_free++] = b->chunk[i];
        else vmaDestroyBuffer(c->vma, b->chunk[i].buf, b->chunk[i].alloc);
    }
    b->nchunk = 0, b->chunk_used = 0;
    b->bytes = 0;
}

// An upload batch holding this much staging (its buffers and arena chunks) is submitted at the outermost vk_upload_end,
// whatever NX2NAVI_UPLOAD_BATCH_MB says (the staging pool's comment, below).
#define STG_FLUSH (64ull << 20)

// cmd_lock held: the staging bytes of batch b (its buffers and arena chunks).
static uint64_t batch_staging(const Batch *b) {
    uint64_t n = (uint64_t)b->nchunk * ARENA_CHUNK;
    for (int i = 0; i < b->nstg; i++) n += b->stg[i].size;
    return n;
}

// The cached staging buffer that fits n with the least waste: at most four times the request, so a small upload does not
// take a block-sized buffer another upload wanted.
static int staging_cache_take(VkCtx *c, VkDeviceSize n, Staging *out, void **map) {
    int best = -1;
    for (int i = 0; i < stg_ncache; i++) {
        if (stg_cache[i].size < n || stg_cache[i].size > n * 4 + 65536) continue;
        if (best < 0 || stg_cache[i].size < stg_cache[best].size) best = i;
    }
    if (best < 0) return 0;
    VmaAllocationInfo ai;
    vmaGetAllocationInfo(c->vma, stg_cache[best].alloc, &ai);
    *out = stg_cache[best], *map = ai.pMappedData;
    stg_cache_bytes -= stg_cache[best].size;
    stg_cache[best] = stg_cache[--stg_ncache];
    return 1;
}

// Batches complete in submission order (one queue): retire from the front while their fences are signalled.
static void retire(VkCtx *c, int wait_all) {
    int k = 0;
    for (; k < ninflight; k++) {
        Batch *b = &inflight[k];
        if (wait_all) vkWaitForFences(c->device, 1, &b->fence, VK_TRUE, UINT64_MAX);
        else if (vkGetFenceStatus(c->device, b->fence) != VK_SUCCESS) break;
        up_ts_read(c, b);
        free_staging(c, b);
        done_seq = b->seq;
        if (nspare == cap_spare) grow((void **)&spare, &cap_spare, sizeof *spare);
        spare[nspare++] = *b;
    }
    memmove(inflight, inflight + k, (size_t)(ninflight - k) * sizeof *inflight);
    ninflight -= k;
}

static void destroy_dead(VkCtx *c, const Dead *d) {
    switch (d->kind) {
    case DEAD_IMAGE: vkDestroyImage(c->device, (VkImage)(uintptr_t)d->handle, NULL); break;
    case DEAD_IMAGE_VMA: vmaDestroyImage(c->vma, (VkImage)(uintptr_t)d->handle, d->alloc); break;
    case DEAD_VIEW: vkDestroyImageView(c->device, (VkImageView)(uintptr_t)d->handle, NULL); break;
    case DEAD_BUFFER_VMA: vmaDestroyBuffer(c->vma, (VkBuffer)(uintptr_t)d->handle, d->alloc); break;
    }
}

// The objects the GPU is done with: destroyed here (out != NULL: moved to *out, for the caller to destroy without
// cmd_lock; *nout of them).
// ---- The finalize of a used pool (pool_fini)
// The finalize returned only once the queue was idle and every upload batch was through (vkQueueWaitIdle), since the
// guest gets the storage back when it returns. During an area load the queue also holds the load's texture uploads,
// which name no pool's storage: entering Kokiri Forest, one finalize waited 418 ms for them, the frame took 462 ms.
// The game finalizes a pool once its own work with it is done, as on the console, so the finalize now waits for the
// submissions issued so far (the game's work, the mirror copies and write-backs in it) and returns. The pool's buffer
// and memory, which images cut from it (vk_texture_range_gone) and the upload batches open or in flight may still
// name, go later: once those batches and the submission being recorded are through (pool_wait_collect, from the
// upload collector). NX2NAVI_POOL_FINI_IDLE=1 waits for the idle queue as before.
typedef struct { PoolRec *r; uint64_t seq, tl; } PoolWait;
static PoolWait *pwait;   // retire_lock
static int npwait, cpwait;
static uint64_t pf_n, pf_ns, pf_max_ns, pf_logged;   // narrowed finalizes, their waits (any thread; statistics only)

static int pool_fini_idle(void) {
    static int on = -1;
    if (on < 0) {
        on = rt_knob("POOL_FINI_IDLE", "0") == 1;
        rt_log("vk: a pool's finalize waits for %s (NX2NAVI_POOL_FINI_IDLE=%d)",
               on ? "the idle queue, the upload batches included" : "the submissions issued so far", on);
    }
    return on;
}

static void pool_fini_wait(void) {
    uint64_t t0 = kern_now_ns(), v = vk_sync_issued();
    vk_sync_fence_wait(v, UINT64_MAX);
    uint64_t d = kern_now_ns() - t0;
    uint64_t n = __atomic_add_fetch(&pf_n, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&pf_ns, d, __ATOMIC_RELAXED);
    if (d > __atomic_load_n(&pf_max_ns, __ATOMIC_RELAXED)) __atomic_store_n(&pf_max_ns, d, __ATOMIC_RELAXED);
    if (d > 20000000ull && __atomic_add_fetch(&pf_logged, 1, __ATOMIC_RELAXED) <= 20)
        rt_log("vk: a pool's finalize waited %.1f ms for the submissions up to %llu (finalizes so far %llu, %.1f ms in all)",
               d / 1e6, (unsigned long long)v, (unsigned long long)n, __atomic_load_n(&pf_ns, __ATOMIC_RELAXED) / 1e6);
    else if (n % 256 == 0)
        rt_log("vk: pool finalizes that waited for the GPU: %llu, %.1f ms in all, the longest %.1f ms",
               (unsigned long long)n, __atomic_load_n(&pf_ns, __ATOMIC_RELAXED) / 1e6,
               __atomic_load_n(&pf_max_ns, __ATOMIC_RELAXED) / 1e6);
}

static void pool_wait_add(PoolRec *r) {
    // every upload batch handed out so far, and the submission another thread may be recording (vk_sync_submit sets
    // issued after it), as defer stamps a deferred object
    PoolWait w = {r, __atomic_load_n(&next_seq, __ATOMIC_ACQUIRE), vk_sync_issued() + 1};
    plat_mutex_lock(&retire_lock);
    if (npwait == cpwait) grow((void **)&pwait, &cpwait, sizeof *pwait);
    pwait[npwait++] = w;
    plat_mutex_unlock(&retire_lock);
}

// cmd_lock held (collect_locked_out): the waiting pools whose batches and submission are through go now.
static void pool_wait_collect(uint64_t tl) {
    PoolRec *go[64];
    int n = 0;
    plat_mutex_lock(&retire_lock);
    for (int i = 0; i < npwait && n < 64;)
        if (pwait[i].seq <= done_seq && pwait[i].tl <= tl) go[n++] = pwait[i].r, pwait[i] = pwait[--npwait];
        else i++;
    plat_mutex_unlock(&retire_lock);
    for (int i = 0; i < n; i++) pool_retire(go[i]);   // off the retiring list, then destroyed
}

static void collect_locked_out(VkCtx *c, Dead **out, int *nout, int *cout) {
    retire(c, 0);
    uint64_t tl = 0;
    if (c->timeline) vkGetSemaphoreCounterValue(c->device, c->timeline, &tl);
    if (npwait) pool_wait_collect(tl);
    int k = 0;
    for (int i = 0; i < ndead; i++) {
        Dead *d = &dead[i];
        if (d->seq > done_seq || d->tl > tl) { dead[k++] = *d; continue; }
        if (d->kind == DEAD_HOST) {   // not a Vulkan object: nothing for the recorder's handle check
            free((void *)(uintptr_t)d->handle);
            continue;
        }
        rec_check_handle(d->kind == DEAD_VIEW ? REC_VIEW : d->kind == DEAD_BUFFER_VMA ? REC_BUFFER : REC_IMAGE, d->handle, 1,
                         d->tl, tl);
        if (!out) { destroy_dead(c, d); continue; }
        if (*nout == *cout) grow((void **)out, cout, sizeof **out);
        (*out)[(*nout)++] = *d;
    }
    ndead = k;
}

static void collect_locked(VkCtx *c) { collect_locked_out(c, NULL, NULL, NULL); }

VkCommandBuffer vk_upload_begin(void) {
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    VkCtx *c = vk_ctx();
    if (!c) rt_fatal("vk: upload before vk_init");
    if (open_active) return open_b.cb;
    if (!cmd_pool) {
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                      .queueFamilyIndex = c->gfx_family};
        if (vkCreateCommandPool(c->device, &pi, NULL, &cmd_pool) != VK_SUCCESS) rt_fatal("vk: upload command pool");
    }
    retire(c, 0);
    memset(&open_b, 0, sizeof open_b);
    if (nspare) {
        Batch s = spare[--nspare];
        open_b.cb = s.cb, open_b.fence = s.fence, open_b.stg = s.stg, open_b.cap = s.cap;
        open_b.chunk = s.chunk, open_b.cap_chunk = s.cap_chunk;
        vkResetCommandBuffer(open_b.cb, 0);
        vkResetFences(c->device, 1, &open_b.fence);
    } else {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cmd_pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkAllocateCommandBuffers(c->device, &ai, &open_b.cb) != VK_SUCCESS ||
            vkCreateFence(c->device, &fi, NULL, &open_b.fence) != VK_SUCCESS)
            rt_fatal("vk: upload command buffer");
    }
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(open_b.cb, &bi);
    open_b.ts = up_ts_open(c, open_b.cb);
    open_b.seq = ++next_seq;
    vk_crumb(open_b.cb, VK_CRUMB_UPLOAD, open_b.seq);
    open_active = 1;
    return open_b.cb;
}

// NX2NAVI_UPLOAD_BATCH_MB (256 with low_end_gpu = 1; 0, the default, = no limit of its own): once the open batch's
// staging buffers hold that much, the outermost vk_upload_end submits it. It does so too once the batch's staging
// reaches STG_FLUSH, so that an area load's staging stays inside the staging pool's blocks (the staging pool's
// comment): 0 therefore means STG_FLUSH (64 MiB), not "never", and a value above STG_FLUSH changes nothing, the
// low-end mode's 256 and the settings menu's 128 to 1024 included. A save load made thousands of textures between two replay submissions, and every one of them went into
// one batch: about 1.5 GB of raw ASTC blocks plus the decoder's work in a single submission,
// which AMD's 23.10 driver on an RX 580 refused with VK_ERROR_OUT_OF_DEVICE_MEMORY. Submitted early, a batch still
// precedes the replay submission that uses its textures (the queue keeps order), and its staging is freed as it retires.
static uint64_t upload_cap = UINT64_MAX - 1;   // UINT64_MAX - 1: not read yet
static uint64_t n_early;

void vk_upload_end(void) {
    if (upload_cap == UINT64_MAX - 1) {
        const char *e = cfg_get("NX2NAVI_UPLOAD_BATCH_MB");
        long mb = e && *e ? atol(e) : 0;
        upload_cap = mb > 0 ? (uint64_t)mb << 20 : UINT64_MAX;
        if (mb > 0) rt_log("vk: upload batches submitted early above %ld MiB of staging (NX2NAVI_UPLOAD_BATCH_MB)", mb);
        else rt_log("vk: upload batches submitted early above %llu MiB of staging only, the staging pool's limit "
                    "(NX2NAVI_UPLOAD_BATCH_MB=0)", (unsigned long long)(STG_FLUSH >> 20));
    }
    if (open_active && cmd_lock.depth == 1 && (open_b.bytes >= upload_cap || batch_staging(&open_b) >= STG_FLUSH)) {
        n_early++;
        if (n_early <= 4 || !(n_early & (n_early - 1)))
            rt_log("vk: upload batch submitted early with %.0f MiB of staging in %d buffers and %d arena chunks (%llu so far)",
                   open_b.bytes / 1048576.0, open_b.nstg, open_b.nchunk, (unsigned long long)n_early);
        vk_upload_flush(0);
    }
    plat_rmutex_unlock(&cmd_lock);
}

// An out-of-memory failure of a submit: every heap's use against its budget (the driver's numbers with
// VK_EXT_memory_budget), VMA's blocks and allocations on it, and the upload batches, so the log names what ran out.
// Takes no lock but the allocator's own: it may run with cmd_lock held.
void vk_mem_oom_report(const char *what, VkResult r) {
    if (r != VK_ERROR_OUT_OF_DEVICE_MEMORY && r != VK_ERROR_OUT_OF_HOST_MEMORY) return;
    VkCtx *c = vk_ctx();
    if (!c) return;
    rt_log("vk: %s failed: out of %s memory (%d)", what, r == VK_ERROR_OUT_OF_DEVICE_MEMORY ? "device" : "host", (int)r);
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
                                            .pNext = c->has_memory_budget ? &b : NULL};
    vkGetPhysicalDeviceMemoryProperties2(c->phys, &p2);
    VmaBudget vb[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(c->vma, vb);
    const double M = 1048576.0;
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        rt_log("vk:   heap %u (%s, %.0f MiB): driver use %.0f of budget %.0f MiB%s; vma %u blocks %.0f MiB, %u allocations "
               "%.0f MiB", h, c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? "device-local" : "host",
               c->mem_props.memoryHeaps[h].size / M, c->has_memory_budget ? b.heapUsage[h] / M : vb[h].usage / M,
               c->has_memory_budget ? b.heapBudget[h] / M : vb[h].budget / M,
               c->has_memory_budget ? "" : " (VMA's estimate)", vb[h].statistics.blockCount,
               vb[h].statistics.blockBytes / M, vb[h].statistics.allocationCount, vb[h].statistics.allocationBytes / M);
    uint64_t ib = 0;
    for (int i = 0; i < ninflight; i++) ib += inflight[i].bytes;
    rt_log("vk:   upload batch open %d (%d staging buffers, %.0f MiB), %d in flight (%.0f MiB of staging), %llu submitted "
           "early; VRAM mirrors %lld (%.0f MiB); objects waiting for release %d", open_active, open_b.nstg,
           open_b.bytes / M, ninflight, ib / M, (unsigned long long)n_early, (long long)m_live_n, m_live_bytes / M, ndead);
}

// Staging memory comes from a pool of its own that keeps STG_MIN_BLOCKS blocks from its first use (at boot). In the
// default pools VMA creates a block under the lock of its memory type (VmaBlockVector::Allocate), so a block made by
// the headroom thread (vk_headroom.c) or a burst of uploads held every staging allocation of the render thread: one
// took 47-50 ms, for a few bytes, in the first frame after a save load. Larger buffers than a block go to the default
// pool.
// The pool's blocks are also all the staging an area load gets (stg_create). Entering Hyrule Field uploads about 3,100
// textures (1.2 GiB) in one frame; each upload kept its staging until its batch completed, so that frame made 221 new
// staging buffers, 1.1 GiB of new host memory. On Linux every new block is a fresh kernel allocation that asks for
// 2-4 MiB pages first, and with the free 2 MiB blocks used up the kernel compacted memory for each: on an RX 6800 XT
// (amdgpu, 32 GB) the load frame then took 2.4 to 4.7 s instead of 0.4, the texture thread in the kernel all along,
// the compaction scanning millions of pages per half second. Now a load stays inside the blocks made at boot: a
// batch whose staging reaches STG_FLUSH is submitted at the end of that upload (vk_upload_end), and a staging buffer that
// does not fit the blocks takes the room of the kept buffers and idle arena chunks, then of the batches in flight,
// which it waits for in order (a wait covers all the queue's work submitted ahead of that batch, tens of ms on a heavy
// frame, with cmd_lock held). Only when none of that makes room (the open batch's own staging, staging buffers their
// callers hold, the blocks' fragmentation) does the pool grow by a block, and the log says so.
// A discrete GPU keeps STG_MIN_BLOCKS blocks, 256 MiB of host memory for the whole run (128 MiB up to 0.9.2); an
// integrated one, whose host memory is the machine's RAM and where low memory ended the game before
// (vk_mem_give_back), keeps STG_MIN_BLOCKS_UMA, 128 MiB, and its loads wait for batches sooner.
#define STG_BLOCK (64ull << 20)
#define STG_MIN_BLOCKS 4
#define STG_MIN_BLOCKS_UMA 2
static VmaPool stg_pool;
static int stg_pool_tried;
static uint64_t stg_room_cache, stg_room_wait, stg_room_new;   // cmd_lock: how stg_create found room (rt_log below)
static uint64_t stg_wait_ticks, stg_wait_max;                  // cmd_lock: qpc ticks stg_create waited for batches

// vmaCreateBuffer of a staging buffer (*ai names stg_pool) inside the pool's blocks: when they are full, the kept
// staging buffers and the arena's idle chunks go, then the oldest batch in flight is waited for and retired, until the
// buffer fits. chunk: the buffer is an arena chunk, and VK_INCOMPLETE says that a retired batch gave chunks back to
// arena_free, to take one of those instead. Without a batch to wait for (or once the device is lost) the pool grows.
// The wait runs with cmd_lock held, and cannot leave it: the caller is inside vk_upload_begin .. vk_upload_end with
// the open batch's command buffer in hand, and the staging buffer it asks for belongs to that batch, so letting go of
// the lock would let another thread submit the batch under it. Meanwhile a replay submission that wants to send its
// uploads (vk_upload_flush) waits for the lock; its own commands would queue behind the same batches, so what it
// loses is recording time, not GPU time. How long that is on a load is not measured anywhere yet: the waits are
// timed here and logged (the first four, then at every doubling of their count), so a played session's log says.
static VkResult stg_create(VkCtx *c, const VkBufferCreateInfo *bi, const VmaAllocationCreateInfo *ai, VkBuffer *b,
                           VmaAllocation *a, VmaAllocationInfo *info, int chunk) {
    VmaAllocationCreateInfo in = *ai;
    in.flags |= VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT;
    plat_rmutex_lock(&cmd_lock);
    VkResult r;
    for (;;) {
        if ((r = vmaCreateBuffer(c->vma, bi, &in, b, a, info)) == VK_SUCCESS) break;
        if (stg_ncache || narena_free) {
            for (int i = 0; i < stg_ncache; i++) vmaDestroyBuffer(c->vma, stg_cache[i].buf, stg_cache[i].alloc);
            for (int i = 0; i < narena_free; i++) vmaDestroyBuffer(c->vma, arena_free[i].buf, arena_free[i].alloc);
            stg_ncache = 0, stg_cache_bytes = 0, narena_free = 0;
            stg_room_cache++;
            continue;
        }
        if (ninflight && !vk_device_is_lost()) {
            int before = ninflight;
            uint64_t w0 = qpc();
            VkResult w = vkWaitForFences(c->device, 1, &inflight[0].fence, VK_TRUE, UINT64_MAX);
            if (w != VK_SUCCESS) {
                vk_device_lost_check(w, "the wait for an upload batch");
                rt_fatal("vk: the wait for an upload failed (%d)", (int)w);
            }
            uint64_t took = qpc() - w0;
            stg_wait_ticks += took;
            if (took > stg_wait_max) stg_wait_max = took;
            retire(c, 0);
            stg_room_wait++;
            if (stg_room_wait <= 4 || !(stg_room_wait & (stg_room_wait - 1)))
                rt_log("vk: a staging buffer of %.1f MiB waited %.1f ms for an upload batch to finish, with the upload "
                       "lock held (%llu such waits so far, %.0f ms in all, the longest %.1f ms; %d batches still in "
                       "flight). A replay submission that sends its uploads waits for that lock",
                       bi->size / 1048576.0, qpc_ms(took), (unsigned long long)stg_room_wait, qpc_ms(stg_wait_ticks),
                       qpc_ms(stg_wait_max), ninflight);
            if (chunk && narena_free) {
                r = VK_INCOMPLETE;
                break;
            }
            if (ninflight < before) continue;
        }
        r = vmaCreateBuffer(c->vma, bi, ai, b, a, info);
        if (r == VK_SUCCESS && stg_room_new++ < 16)
            rt_log("vk: the staging pool grew by a block for a buffer of %.1f MiB (the open upload batch holds %.0f MiB, "
                   "%d batches in flight; room found so far by dropping kept buffers %llu times, by waiting for a batch "
                   "%llu times; buffers callers hold and the blocks' fragmentation also take room)",
                   bi->size / 1048576.0, open_active ? batch_staging(&open_b) / 1048576.0 : 0.0, ninflight,
                   (unsigned long long)stg_room_cache, (unsigned long long)stg_room_wait);
        break;
    }
    plat_rmutex_unlock(&cmd_lock);
    return r;
}

// A staging buffer the driver refused (vk_staging): the buffers kept for reuse go back to the driver before one more
// try, with the headroom's spare blocks (vk_headroom_release): the staging cache, the arena's free chunks and the
// write-back snapshot pool, up to some hundred MiB. On an integrated GPU the device-local heap is the machine's RAM and
// fills before the commit checks see anything; staging buffers of 11 KiB to 16 MiB refused there ended the game in
// Hyrule Field and on the first scene of play (#465, #567). The replay's transient buffers and descriptor pools do the
// same (vk_exec_draw.c scratch, alloc_set). Returns the bytes given back.
uint64_t vk_mem_give_back(void) {
    VkCtx *c = vk_ctx();
    uint64_t freed = 0;
    plat_rmutex_lock(&cmd_lock);
    for (int i = 0; i < stg_ncache; i++) {
        freed += stg_cache[i].size;
        vmaDestroyBuffer(c->vma, stg_cache[i].buf, stg_cache[i].alloc);
    }
    stg_ncache = 0, stg_cache_bytes = 0;
    for (int i = 0; i < narena_free; i++) vmaDestroyBuffer(c->vma, arena_free[i].buf, arena_free[i].alloc);
    freed += (uint64_t)narena_free * ARENA_CHUNK;
    narena_free = 0;
    plat_rmutex_unlock(&cmd_lock);
    plat_mutex_lock(&snap_pool_lock);
    for (int i = 0; i < nsnap_pool; i++) vmaDestroyBuffer(c->vma, snap_pool[i].buf, snap_pool[i].alloc);
    freed += snap_pool_bytes;
    nsnap_pool = 0, snap_pool_bytes = 0;
    plat_mutex_unlock(&snap_pool_lock);
    return freed + vk_headroom_release();
}

// ---- The memory report past VMA's figures: every report (owner_log) adds three lines. "mem: outside VMA" follows the
// driver's usage and budget per heap against VMA's blocks in it, so that what VMA never allocated (the driver's usage
// less VMA's blocks) can be followed over a session, and counts what the runtime itself made outside VMA where no size
// can be known: the driver gives none for a pipeline, a stage library or a descriptor pool (the boot stage lines,
// vk_mem_stage_log, place their memory by difference: 1.5 GiB for the recorded pipelines on an RTX 5090); the
// swapchain's pixels and DLSS's and FSR's figures are sizes. "mem: system" splits the process's commit by owner: the
// game's heap and physical maps, the device-local memory on Windows (the driver charges it to the process: in a session
// of 2026-11-13 the commit rose 4.6 GB at the first area while VMA's device-local allocations rose 3.4 GiB, and fell
// with them later), VMA's host-visible blocks, the C runtime heap with the shares of it the runtime can name, the data
// of the driver's pipeline caches, and the rest. "mem: growth" names what grew by more than GROWTH_STEP since the report
// before and since the first report in play; every RANK_EVERY_NS of play and at exit (vk_mem_exit_log) "mem: growth
// ranking" ranks every category by its growth since play began and marks those that only climbed (mem_growth.h).
// Every figure is read at the report: sums the owners keep, walks of the pipeline, shader and texture tables under
// their locks (a fraction of a millisecond, once a report), the C runtime heap's summary. Nothing is counted per frame.
#define IDLE_MIN 10                              // pipelines no lookup reached for this many minutes count as unused
#define GROWTH_STEP (64ull << 20)                // growth_take names a category that grew by more than this
#define RANK_EVERY_NS (600ull * 1000000000ull)   // the ranking's period in play
#define HEAP_SLOW_NS 2000000ull                  // a heap summary that took longer is not asked for again

typedef struct {
    uint64_t usage, blocks, allocs;   // the device-local heaps: the driver's usage, VMA's blocks and allocations
    uint64_t hblocks;                 // VMA's blocks on the other heaps
    uint64_t tex, pools, vma_other;   // VMA's device-local allocations: textures, pools (mirrors and physical), the rest
    uint64_t heap_c, maps;            // the game's heap as committed, its physical maps
} ReportFig;

// The game's memory imported for the GPU (VK_EXT_external_memory_host): the driver counts it in a host heap's usage.
static uint64_t import_bytes(void) {
    uint64_t b = 0;
    plat_mutex_lock_shared(&pool_lock);
    for (int i = 0; i < pool_index.n; i++) {
        const PoolRec *r = pool_index.e[i].buf;
        if (r->kind == POOL_IMPORT && r->buf) b += r->import_hi - r->import_lo;
    }
    plat_mutex_unlock_shared(&pool_lock);
    return b;
}

static void outside_log(VkCtx *c, uint64_t frame, const VkPhysicalDeviceMemoryBudgetPropertiesEXT *bp,
                        const VmaBudget *vb, const VkPipeMem *pm) {
    const double M = 1048576.0;
    char line[2000];
    size_t k = 0;
    mg_put(line, sizeof line, &k, "mem: outside VMA frame %llu: by heap, the driver's usage of its budget, VMA's blocks "
           "in it and the rest:", (unsigned long long)frame);
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++) {
        uint64_t use = c->has_memory_budget ? bp->heapUsage[h] : vb[h].usage;
        uint64_t budget = c->has_memory_budget ? bp->heapBudget[h] : vb[h].budget, vma = vb[h].statistics.blockBytes;
        mg_put(line, sizeof line, &k, "%s heap %u (%s) %.0f of %.0f MiB, VMA's blocks %.0f, outside VMA %.0f", h ? ";" : "",
               h, c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? "device-local" : "host",
               use / M, budget / M, vma / M, use > vma ? (use - vma) / M : 0.0);
    }
    uint64_t imp = import_bytes();
    if (imp) mg_put(line, sizeof line, &k, " (the game's memory imported for the GPU, %.0f MiB, is in the host heaps' rest)",
                    imp / M);
    if (!c->has_memory_budget)
        mg_put(line, sizeof line, &k, " (VMA's own estimates: without VK_EXT_memory_budget nothing outside VMA shows)");
    mg_put(line, sizeof line, &k, ". Made outside VMA, counted where the driver gives no size: graphics pipelines %llu "
           "(whole %llu, fast-linked %llu, optimised after a fast link %llu; no draw for %u minutes %llu, none in this run "
           "%llu; building or failed %llu), stage libraries %llu (vertex input %llu, pre-rasterization %llu, fragment "
           "shader %llu, fragment output %llu), compute pipelines %llu (no dispatch for %u minutes %llu, none in this run "
           "%llu), pipeline layouts %llu", (unsigned long long)pm->gfx, (unsigned long long)pm->gfx_whole,
           (unsigned long long)pm->gfx_fast, (unsigned long long)pm->gfx_opt, pm->idle_min,
           (unsigned long long)pm->gfx_idle, (unsigned long long)pm->gfx_never, (unsigned long long)pm->gfx_none,
           (unsigned long long)(pm->lib[0] + pm->lib[1] + pm->lib[2] + pm->lib[3]), (unsigned long long)pm->lib[0],
           (unsigned long long)pm->lib[1], (unsigned long long)pm->lib[2], (unsigned long long)pm->lib[3],
           (unsigned long long)pm->comp, pm->idle_min, (unsigned long long)pm->comp_idle,
           (unsigned long long)pm->comp_never, (unsigned long long)pm->layouts);
    VkDescMem dm;
    vk_exec_desc_mem(&dm);
    mg_put(line, sizeof line, &k, "; descriptor pools %llu of %llu sets each (%llu of them spare), the texture pool's %llu "
           "sets", (unsigned long long)dm.pools, (unsigned long long)dm.sets_each, (unsigned long long)dm.spare,
           (unsigned long long)dm.tex_sets);
    uint32_t sn, sw, sh;
    uint64_t sb;
    vk_present_swapchain_mem(&sn, &sw, &sh, &sb);
    mg_put(line, sizeof line, &k, "; the swapchain %u images of %ux%u, %.0f MiB of pixels", sn, sw, sh, sb / M);
    int64_t dlss = vk_dlaa_vram();
    int nfsr = 0;
    uint64_t fg = 0, fsr = vk_ffx_vram(&nfsr, &fg);
    if (dlss >= 0) mg_put(line, sizeof line, &k, "; DLSS %.0f MiB", dlss / M);
    else mg_put(line, sizeof line, &k, "; DLSS reports nothing");
    mg_put(line, sizeof line, &k, "; FSR upscaling %.0f MiB in %d contexts, FSR frame generation %.0f MiB (DLSS frame "
           "generation gives no size)", fsr / M, nfsr, fg / M);
    rt_log("%s", line);
}

// The figures of system_log that the growth bookkeeping follows.
typedef struct { uint64_t commit, crt, shaders, pipes, texhost, pcache, other; } SysFig;

static void system_log(uint64_t frame, const ReportFig *f, const VkPipeMem *pm, SysFig *out) {
    const double M = 1048576.0;
    uint64_t res, priv;
    plat_process_memory(&res, &priv);
    // The C runtime heap's summary holds the heap's lock while it adds up: asked at the report only, and no more once
    // one was slow, so that a heap grown past what the summary handles quickly does not hold every malloc up again.
    static int heap_off;
    PlatHeapInfo hi = {0};
    if (!heap_off) {
        uint64_t t0 = plat_now_ns();
        plat_heap_info(&hi);
        uint64_t took = plat_now_ns() - t0;
        if (took > HEAP_SLOW_NS)
            heap_off = 1, rt_log("mem: the C runtime heap's summary took %.1f ms; it is not asked for again in this run",
                                 took / 1e6);
    }
    GfxShaderRam sr;
    gfx_shader_ram(&sr);
    VkTexHost th;
    vk_texture_host(&th);
    uint64_t frec, fthumb;
    flight_mem(&frec, &fthumb);
    uint64_t lq = logq_bytes();
    uint64_t shaders = sr.cache + sr.use + sr.pack + sr.records;
    // The shaders' SPIR-V as held (gfx_shader.c "SPIR-V at rest"): against what it would be uncompressed, how many
    // shaders hold it compressed alone, and the decodes readers' pins made, those on the drawing threads apart.
    char spv[400];
    if (sr.compress > 0)
        snprintf(spv, sizeof spv, " (their SPIR-V %.0f, uncompressed %.0f; %llu shaders compressed; %llu decompressions "
                 "in %.0f ms, %llu of them on the drawing threads in %.1f ms, the longest %.2f ms)", sr.spv_held / M,
                 sr.spv_full / M, (unsigned long long)sr.spv_packed, (unsigned long long)sr.decodes, sr.decode_ns / 1e6,
                 (unsigned long long)sr.draw_decodes, sr.draw_decode_ns / 1e6, sr.draw_decode_max_ns / 1e6);
    else
        snprintf(spv, sizeof spv, " (their SPIR-V %.0f, uncompressed%s)", sr.spv_held / M,
                 sr.compress < 0 ? ": shader_compress = 0" : "");
    uint64_t pipes = pm->ram_pipes + pm->ram_keys + pm->ram_seed + pm->ram_binaries;
    uint64_t texhost = th.bytes + th.tmp, known = shaders + pipes + texhost + lq + frec;
    uint64_t pcache = pm->cache[0] + pm->cache[1];
    // Windows charges the device-local memory the driver allocates to the process's commit (platform.h
    // plat_commit_counts_vram): the driver's figure, VMA's blocks and what VMA never allocated alike.
    int charged = plat_commit_counts_vram();
    uint64_t dev = charged ? f->usage : 0, crt = hi.committed ? hi.committed : known;
    int64_t other = (int64_t)priv - (int64_t)(f->heap_c + f->maps + dev + f->hblocks + crt + pcache);
    char line[2000];
    size_t k = 0;
    mg_put(line, sizeof line, &k, "mem: system frame %llu: process commit %.0f MiB%s = the game's heap %.0f + its physical "
           "maps %.0f", (unsigned long long)frame, priv / M, charged ? "" : " (the resident anonymous pages)",
           f->heap_c / M, f->maps / M);
    if (charged) mg_put(line, sizeof line, &k, " + device-local memory, which Windows charges to the process, %.0f", dev / M);
    mg_put(line, sizeof line, &k, " + VMA's host-visible blocks %.0f (the flight recorder's thumbnails %.0f of them)",
           f->hblocks / M, fthumb / M);
    if (hi.committed)
        mg_put(line, sizeof line, &k, " + the C runtime heap %.0f (%.0f of it handed out; of it known: shader caches %.0f in "
               "%llu shaders%s, pipeline tables %.0f, the texture cache %.0f with decode temporaries %.0f now and %.0f at most "
               "since the last report, the log queue %.0f, the flight recorder's records %.0f; the rest %.0f)",
               hi.committed / M, hi.used / M, shaders / M, (unsigned long long)sr.shaders, spv, pipes / M, texhost / M,
               th.tmp / M, th.tmp_peak / M, lq / M, frec / M, ((double)hi.committed - (double)known) / M);
    else
        mg_put(line, sizeof line, &k, " + what the runtime knows it allocated %.0f (the C runtime heap's size is not "
               "known: shader caches %.0f in %llu shaders%s, pipeline tables %.0f, the texture cache %.0f with decode "
               "temporaries %.0f now and %.0f at most since the last report, the log queue %.0f, the flight recorder's "
               "records %.0f)", known / M, shaders / M, (unsigned long long)sr.shaders, spv, pipes / M, texhost / M,
               th.tmp / M, th.tmp_peak / M, lq / M, frec / M);
    mg_put(line, sizeof line, &k, " + the driver's pipeline cache data %.0f (pipeline-cache.bin's %.0f, the play cache's "
           "%.0f, %s) + other %.0f (the driver's own memory, thread stacks, the program's data; %u other heaps)",
           pcache / M, pm->cache[0] / M, pm->cache[1] / M, pm->cache_asked ? "as last asked" : "as loaded at the boot",
           other / M, hi.other_heaps);
    char room[160];
    mem_budget_text(room, sizeof room);   // the levels the owners that give memory back go by (mem_budget.h)
    mg_put(line, sizeof line, &k, "; room: %s", room);
    rt_log("%s", line);
    *out = (SysFig){priv, crt, shaders, pipes, texhost, pcache, other > 0 ? (uint64_t)other : 0};
}

// ---- growth bookkeeping (mem_growth.h): the categories, in the order the lines name them
enum { G_DEV, G_TEX, G_POOLS, G_VMA_OTHER, G_VMA_FREE, G_OUTSIDE, G_COMMIT, G_HEAP, G_MAPS, G_HOSTVMA, G_CRT, G_SHADERS,
       G_PIPES, G_TEXHOST, G_PCACHE, G_OTHER, G_N };
static const char *const grow_names[G_N] = {
    "device-local in use", "textures", "pools and VRAM mirrors", "VMA's other allocations", "free inside VMA's blocks",
    "outside VMA", "process commit", "the game's heap", "its physical maps", "VMA's host-visible blocks",
    "the C runtime heap", "shader caches", "pipeline tables", "the texture cache's host memory",
    "the driver's pipeline cache data", "other system memory"};
static PlatMutex grow_lock = PLAT_MUTEX_INIT;   // the book, between the report's thread and the exit's
static MgBook gbook;
static uint64_t grow_t0, grow_ranked;           // plat_now_ns at the first report in play, at the last ranking

static void rank_log_locked(const char *when, uint64_t now) {
    char line[1900];
    mg_rank(&gbook, line, sizeof line);
    rt_log("mem: growth ranking %s %.1f min of play (%u reports; only climbs: rose in a report and fell in none): %s", when,
           (now - grow_t0) / 60e9, gbook.play_lines, line);
}

static void growth_take(uint64_t frame, const uint64_t v[G_N]) {
    plat_mutex_lock(&grow_lock);
    if (!gbook.n)
        for (int i = 0; i < G_N; i++) mg_cat(&gbook, grow_names[i]);
    int was = gbook.play;
    mg_take(&gbook, v, frame_step_world_us() >= 0);
    uint64_t now = plat_now_ns();
    if (gbook.play && !was) grow_t0 = grow_ranked = now;
    char line[1800], when[48];
    if (gbook.play) snprintf(when, sizeof when, "%.1f min of play", (now - grow_t0) / 60e9);
    else snprintf(when, sizeof when, "before play");
    if (mg_grew(&gbook, GROWTH_STEP, line, sizeof line))
        rt_log("mem: growth frame %llu (%s), by more than %llu MiB: %s", (unsigned long long)frame, when,
               (unsigned long long)(GROWTH_STEP >> 20), line);
    if (gbook.play && now - grow_ranked >= RANK_EVERY_NS) grow_ranked = now, rank_log_locked("after", now);
    plat_mutex_unlock(&grow_lock);
}

// At the process's end (the window's close, the game's exit, a timed run's end): the session's ranking, once it reached
// play, from the figures of the last report; nothing is asked of the device here.
void vk_mem_exit_log(void) {
    plat_mutex_lock(&grow_lock);
    if (gbook.play) rank_log_locked("at exit, after", plat_now_ns());
    plat_mutex_unlock(&grow_lock);
}

static void beyond_log(VkCtx *c, uint64_t frame, const VkPhysicalDeviceMemoryBudgetPropertiesEXT *bp, const VmaBudget *vb,
                       const ReportFig *f) {
    VkPipeMem pm;
    vk_pipeline_mem(&pm, IDLE_MIN);
    outside_log(c, frame, bp, vb, &pm);
    SysFig s;
    system_log(frame, f, &pm, &s);
    uint64_t v[G_N] = {
        [G_DEV] = f->usage, [G_TEX] = f->tex, [G_POOLS] = f->pools, [G_VMA_OTHER] = f->vma_other,
        [G_VMA_FREE] = f->blocks > f->allocs ? f->blocks - f->allocs : 0,
        [G_OUTSIDE] = f->usage > f->blocks ? f->usage - f->blocks : 0, [G_COMMIT] = s.commit, [G_HEAP] = f->heap_c,
        [G_MAPS] = f->maps, [G_HOSTVMA] = f->hblocks, [G_CRT] = s.crt, [G_SHADERS] = s.shaders, [G_PIPES] = s.pipes,
        [G_TEXHOST] = s.texhost, [G_PCACHE] = s.pcache, [G_OTHER] = s.other};
    growth_take(frame, v);
}

// ---- The memory report: every HOLES_PERIOD presents (600 with NX2NAVI_TRACE=perf), after the holes line, in every run.
// The device-local memory the driver counts for the game split by what holds it: VMA's allocations by owner
// (vk_vma_budget.c, "owners"), with the textures split further by the texture cache's census (vk_texture_census) and
// the pools into VRAM mirrors and physical pools; the free space inside VMA's blocks; and what VMA never allocated
// (compiled pipelines, descriptor memory, the swapchain, the DLSS and frame generation features, the driver's own),
// with DLSS's share as its statistics report it. Then the host-visible allocations (staging above all) and the
// process's system memory with the game's own heap. Then the released textures' reuse (vk_texture_reuse_log).
// A category that only climbs from line to line over a long session is a leak. The figures are sums the owners keep
// already, one driver query, VMA's budget query and one walk of the texture entries (vk_texture_census): well under a
// millisecond once a minute, at the present that logs.
static void owner_log(uint64_t frame) {
    VkCtx *c = vk_ctx();
    if (!c || !c->vma) return;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    if (c->has_memory_budget) {
        VkPhysicalDeviceMemoryProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &bp};
        vkGetPhysicalDeviceMemoryProperties2(c->phys, &p2);
    }
    VmaBudget vb[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(c->vma, vb);
    uint64_t usage = 0, blocks = 0, allocs = 0, hblocks = 0, hallocs = 0;
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++) {
        if (c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            usage += c->has_memory_budget ? bp.heapUsage[h] : vb[h].statistics.blockBytes,
                blocks += vb[h].statistics.blockBytes, allocs += vb[h].statistics.allocationBytes;
        else hblocks += vb[h].statistics.blockBytes, hallocs += vb[h].statistics.allocationBytes;
    }
    uint64_t phys = 0;
    plat_mutex_lock_shared(&pool_lock);
    for (int i = 0; i < nphys; i++)
        if (phys_list[i]->alloc) phys += phys_list[i]->size;
    plat_mutex_unlock_shared(&pool_lock);
    VkTexCensus t;
    vk_texture_census(&t);
    uint64_t own[VK_OWN_N], hown[VK_OWN_N];
    for (int o = 0; o < VK_OWN_N; o++) own[o] = vk_vma_owner_bytes(o, 1), hown[o] = vk_vma_owner_bytes(o, 0);
    uint64_t stg_blocks = 0, kept;
    if (stg_pool) {
        VmaStatistics st;
        vmaGetPoolStatistics(c->vma, stg_pool, &st);
        stg_blocks = st.blockBytes;
    }
    // Read without their locks: an upload holds cmd_lock while it waits for a batch (stg_create), tens of milliseconds
    // the present would wait too; a figure a log reads a moment late does no harm.
    kept = __atomic_load_n(&stg_cache_bytes, __ATOMIC_RELAXED) +
           (uint64_t)__atomic_load_n(&narena_free, __ATOMIC_RELAXED) * ARENA_CHUNK;
    uint64_t snaps = __atomic_load_n(&snap_pool_bytes, __ATOMIC_RELAXED);
    int64_t dlss = vk_dlaa_vram();
    char ds[48] = "nothing";
    if (dlss >= 0) snprintf(ds, sizeof ds, "%.0f MiB", dlss / 1048576.0);
    const double M = 1048576.0;
    uint64_t mir = (uint64_t)__atomic_load_n(&m_live_bytes, __ATOMIC_RELAXED);
    rt_log("mem: by owner frame %llu: device-local in use %.0f MiB (the driver's figure) = VMA's allocations %.0f + free "
           "inside VMA's blocks %.0f + outside VMA %.0f (compiled pipelines, descriptor memory, the swapchain, DLSS and "
           "frame generation features, the driver; DLSS reports %s). VMA's allocations: textures %.0f (live %.0f, of them "
           "render targets and other GPU-drawn %.0f and idle %.0f; released %.0f, of them too lately to drop %.0f), pools "
           "%.0f (VRAM mirrors %.0f, physical pools %.0f), upscalers %.0f, replay buffers %.0f, uniform snapshots %.0f, "
           "device address table %.0f, ASTC decoder %.0f, present and post-processing %.0f, headroom spares %.0f, other "
           "%.0f", (unsigned long long)frame, usage / M, allocs / M, blocks > allocs ? (blocks - allocs) / M : 0.0,
           usage > blocks ? (usage - blocks) / M : 0.0, ds, own[VK_OWN_TEXTURES] / M, t.bytes_live / M,
           t.bytes_live_gpu / M, t.bytes_live_idle / M, t.bytes_released / M, t.bytes_released_young / M,
           own[VK_OWN_POOLS] / M, mir / M, phys / M, own[VK_OWN_UPSCALER] / M, own[VK_OWN_REPLAY] / M,
           own[VK_OWN_USNAP] / M, own[VK_OWN_GPA] / M, own[VK_OWN_ASTC] / M, own[VK_OWN_PRESENT] / M,
           own[VK_OWN_HEADROOM] / M, own[VK_OWN_OTHER] / M);
    double proc, sys_used, sys_limit;
    float ram_used, ram_total, ws;
    plat_commit_info(&proc, &sys_used, &sys_limit);
    plat_mem_info(&ram_used, &ram_total, &ws);
    uint64_t heap, heap_c, maps;
    mem_guest_usage(&heap, &heap_c, &maps);
    rt_log("mem: host frame %llu: VMA's host-visible blocks %.0f MiB hold %.0f MiB: staging pool blocks %.0f (kept staging "
           "buffers and arena chunks %.0f), write-back snapshots %.0f; by owner: pools and staging %.0f, textures %.0f, "
           "upscalers %.0f, replay buffers %.0f, uniform snapshots %.0f, present and post-processing (the flight "
           "recorder's thumbnails among them) %.0f, other %.0f. Process commit %.0f MB, working set %.0f MB; system commit "
           "%.0f of %.0f MB; the game's heap %.0f MiB (%.0f committed), its physical maps %.0f MiB",
           (unsigned long long)frame, hblocks / M, hallocs / M, stg_blocks / M, kept / M, snaps / M, hown[VK_OWN_POOLS] / M,
           hown[VK_OWN_TEXTURES] / M, hown[VK_OWN_UPSCALER] / M, hown[VK_OWN_REPLAY] / M, hown[VK_OWN_USNAP] / M,
           hown[VK_OWN_PRESENT] / M, (hown[VK_OWN_OTHER] + hown[VK_OWN_GPA] + hown[VK_OWN_ASTC] + hown[VK_OWN_HEADROOM]) / M,
           proc, (double)ws, sys_used, sys_limit, heap / M, heap_c / M, maps / M);
    uint64_t tex = own[VK_OWN_TEXTURES], pools = own[VK_OWN_POOLS];
    ReportFig f = {usage, blocks, allocs, hblocks, tex, pools, allocs > tex + pools ? allocs - tex - pools : 0, heap_c, maps};
    beyond_log(c, frame, &bp, vb, &f);
    vk_texture_reuse_log(frame);
}

// NX2NAVI_STAGING_FAIL_AT=n (test): the n-th staging buffer the driver makes is taken as refused for memory once, so
// the give-back runs (vk_mem_give_back).
static int staging_fail_now(void) {
    static int64_t at = -1, n;
    if (at < 0) at = cfg_int("NX2NAVI_STAGING_FAIL_AT");
    return at > 0 && plat_inc64(&n) == at;
}

void *vk_staging(VkDeviceSize size, VkBuffer *buf, VmaAllocation *alloc) {
    VkCtx *c = vk_ctx();
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size ? size : 16,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                   .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    if (!__atomic_load_n(&stg_pool_tried, __ATOMIC_ACQUIRE)) {
        plat_once(&cmd_once, cmd_init, NULL);
        plat_rmutex_lock(&cmd_lock);
        if (!stg_pool_tried) {
            uint32_t type;
            int keep = c->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? STG_MIN_BLOCKS : STG_MIN_BLOCKS_UMA;
            VmaPoolCreateInfo pi = {.blockSize = STG_BLOCK, .minBlockCount = (size_t)keep};
            if (vmaFindMemoryTypeIndexForBufferInfo(c->vma, &bi, &aci, &type) == VK_SUCCESS) {
                pi.memoryTypeIndex = type;
                if (vmaCreatePool(c->vma, &pi, &stg_pool) != VK_SUCCESS) stg_pool = NULL;
            }
            rt_log("vk: staging pool %s (%d blocks of %llu MiB kept)", stg_pool ? "created" : "unavailable: default pool",
                   keep, (unsigned long long)(STG_BLOCK >> 20));
            __atomic_store_n(&stg_pool_tried, 1, __ATOMIC_RELEASE);
        }
        plat_rmutex_unlock(&cmd_lock);
    }
    if (stg_pool && bi.size <= STG_BLOCK / 2) aci.pool = stg_pool;
    if (!alloc) {   // the batch owns it, so a cached one of a fitting size serves
        Staging cs;
        void *cmap;
        if (staging_cache_take(c, bi.size, &cs, &cmap)) {
            *buf = cs.buf;
            if (open_b.nstg == open_b.cap) grow((void **)&open_b.stg, &open_b.cap, sizeof *open_b.stg);
            open_b.stg[open_b.nstg++] = cs;
            return cmap;
        }
    }
    VmaAllocationInfo info;
    VkBuffer b;
    VmaAllocation a;
    VkResult r = aci.pool ? stg_create(c, &bi, &aci, &b, &a, &info, 0) : vmaCreateBuffer(c->vma, &bi, &aci, &b, &a, &info);
    if (r == VK_ERROR_OUT_OF_DEVICE_MEMORY && aci.pool) {
        // The staging pool can grow only in 64 MiB blocks of its one memory type. A smaller allocation may
        // still fit in a default block or another host-visible type. Keep the same mapping requirements.
        aci.pool = NULL;
        r = vmaCreateBuffer(c->vma, &bi, &aci, &b, &a, &info);
    }
    if (r == VK_SUCCESS && staging_fail_now()) {
        vmaDestroyBuffer(c->vma, b, a);
        r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    if (r == VK_ERROR_OUT_OF_DEVICE_MEMORY || r == VK_ERROR_OUT_OF_HOST_MEMORY) {
        VkResult first = r;
        uint64_t freed = vk_mem_give_back();
        aci.pool = NULL;
        r = vmaCreateBuffer(c->vma, &bi, &aci, &b, &a, &info);
        static volatile int32_t told;
        if (plat_inc32(&told) <= 16)
            rt_log("vk: staging buffer of %llu bytes refused (%d): %.1f MiB of kept buffers and spare blocks given back, "
                   "%s", (unsigned long long)size, (int)first, freed / 1048576.0,
                   r == VK_SUCCESS ? "made at the second try" : "refused again");
    }
    if (r != VK_SUCCESS) rt_fatal("vk: staging buffer of %llu bytes failed (%d)", (unsigned long long)size, (int)r);
    vk_texstat_add(VK_TS_STG_NEW, 1, bi.size);
    *buf = b;
    if (alloc) *alloc = a;   // the caller destroys it (vmaDestroyBuffer) after vk_upload_flush(1)
    else {
        if (open_b.nstg == open_b.cap) grow((void **)&open_b.stg, &open_b.cap, sizeof *open_b.stg);
        open_b.stg[open_b.nstg++] = (Staging){b, a, bi.size};
        open_b.bytes += bi.size;
    }
    return info.pMappedData;
}

// A range of the staging arena for an upload of the open batch (between vk_upload_begin and vk_upload_end): *buf and
// *off name it, and the batch owns it until its fence signals. Larger requests, and every request with
// NX2NAVI_STG_ARENA=0, get a buffer of their own at offset 0 (vk_staging).
void *vk_staging_at(VkDeviceSize size, VkBuffer *buf, VkDeviceSize *off) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2NAVI_STG_ARENA");
        if (!on) rt_log("vk: staging arena off (NX2NAVI_STG_ARENA=0): every upload stages in a buffer of its own");
    }
    *off = 0;
    if (!on || !open_active || size > ARENA_MAX) return vk_staging(size, buf, NULL);
    VkCtx *c = vk_ctx();
    VkDeviceSize at = (open_b.chunk_used + ARENA_ALIGN - 1) & ~(ARENA_ALIGN - 1);
    if (!open_b.nchunk || at + size > ARENA_CHUNK) {
        StgChunk k;
        VkResult r = VK_INCOMPLETE;
        if (!narena_free) {
            VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = ARENA_CHUNK,
                                     .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
            VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                           .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                                    VMA_ALLOCATION_CREATE_MAPPED_BIT};
            if (stg_pool) aci.pool = stg_pool;   // an eighth of a staging block (STG_BLOCK)
            VmaAllocationInfo info;
            r = stg_pool ? stg_create(c, &bi, &aci, &k.buf, &k.alloc, &info, 1)
                         : vmaCreateBuffer(c->vma, &bi, &aci, &k.buf, &k.alloc, &info);
            if (r != VK_SUCCESS && r != VK_INCOMPLETE) return vk_staging(size, buf, NULL);
            if (r == VK_SUCCESS) {
                k.map = info.pMappedData;
                vk_texstat_add(VK_TS_STG_NEW, 1, ARENA_CHUNK);
                if (arena_made++ < 16)
                    rt_log("vk: staging arena chunk %llu made (%llu MiB)", (unsigned long long)arena_made,
                           (unsigned long long)(ARENA_CHUNK >> 20));
            }
        }
        if (r == VK_INCOMPLETE) k = arena_free[--narena_free];   // one already free, or one a waited-for batch gave back
        if (open_b.nchunk == open_b.cap_chunk) grow((void **)&open_b.chunk, &open_b.cap_chunk, sizeof *open_b.chunk);
        open_b.chunk[open_b.nchunk++] = k;
        open_b.bytes += ARENA_CHUNK;   // the staging the batch holds (upload_cap)
        at = 0;
    }
    StgChunk *k = &open_b.chunk[open_b.nchunk - 1];
    open_b.chunk_used = at + (size ? size : 16);
    arena_ranges++;
    *buf = k->buf, *off = at;
    return k->map + at;
}

uint64_t vk_upload_flush(int wait) {
    if (vk_device_is_lost()) return 0;   // the run is ending (vk_device.c vk_device_lost_check): no submit, no wait
    uint64_t f0 = kern_now_ns();
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    VkCtx *c = vk_ctx();
    uint64_t seq = next_seq;
    if (c && open_active) {
        for (int i = 0; i < open_b.nstg; i++) vmaFlushAllocation(c->vma, open_b.stg[i].alloc, 0, VK_WHOLE_SIZE);
        for (int i = 0; i < open_b.nchunk; i++)
            vmaFlushAllocation(c->vma, open_b.chunk[i].alloc, 0, i + 1 < open_b.nchunk ? VK_WHOLE_SIZE : open_b.chunk_used);
        vk_crumb(open_b.cb, VK_CRUMB_UPLOAD_END, open_b.seq);
        if (open_b.ts >= 0) vkCmdWriteTimestamp(open_b.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, up_ts_pool, 2 * (uint32_t)open_b.ts + 1);
        up_n_batches++;
        vkEndCommandBuffer(open_b.cb);
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &open_b.cb};
        vk_queue_lock();
        open_b.sub_ns = plat_now_ns();
        VkResult r = vkQueueSubmit(c->gfx_queue, 1, &si, open_b.fence);
        vk_queue_unlock();
        if (r == VK_ERROR_OUT_OF_DEVICE_MEMORY || r == VK_ERROR_OUT_OF_HOST_MEMORY) {
            // The driver had no memory to take the batch, which ended the game here (five logs of three reports, all on
            // integrated GPUs, one with 9 GB of commit to spare: the device's own heap was full). A refused
            // submit leaves the command buffer and the fence as they were, so it can be made again: first the batches
            // in flight are waited for, which gives their staging back and lets the objects that waited for them be
            // destroyed, then the kept buffers and spare blocks go back to the driver (vk_mem_give_back), as for a
            // refused staging buffer. Only a second refusal ends the run.
            VkResult first = r;
            int flying = ninflight;
            retire(c, 1);
            collect_locked(c);
            uint64_t freed = vk_mem_give_back();
            vk_queue_lock();
            open_b.sub_ns = plat_now_ns();
            r = vkQueueSubmit(c->gfx_queue, 1, &si, open_b.fence);
            vk_queue_unlock();
            static volatile int32_t told;
            if (plat_inc32(&told) <= 16)
                rt_log("vk: upload submit refused (%d): waited for %d batches in flight, %.1f MiB of kept buffers and "
                       "spare blocks given back, %s", (int)first, flying, freed / 1048576.0,
                       r == VK_SUCCESS ? "taken at the second try" : "refused again");
        }
        if (r != VK_SUCCESS) {
            vk_mem_oom_report("upload submit", r);
            vk_device_lost_check(r, "the submit of an upload batch");
            rt_fatal("vk: upload submit failed (%d)", (int)r);
        }
        if (ninflight == cap_inflight) grow((void **)&inflight, &cap_inflight, sizeof *inflight);
        inflight[ninflight++] = open_b;
        open_active = 0;
    }
    if (c && wait) {
        while (ninflight && done_seq < seq) {
            // a lost device ends the wait at once and leaves the fence unsignalled: retire would take nothing and this
            // loop would spin for good, holding the upload lock, instead of the run ending with the device's report
            VkResult w = vkWaitForFences(c->device, 1, &inflight[0].fence, VK_TRUE, UINT64_MAX);
            if (w != VK_SUCCESS) {
                vk_device_lost_check(w, "the wait for an upload batch");
                rt_fatal("vk: the wait for an upload failed (%d)", (int)w);
            }
            retire(c, 0);
        }
    }
    if (c) collect_locked(c);
    plat_rmutex_unlock(&cmd_lock);
    vk_texstat_time(VK_TS_FLUSH, f0);
    return seq;
}

static void defer(int kind, uint64_t handle, VmaAllocation alloc) {
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    if (ndead == cap_dead) grow((void **)&dead, &cap_dead, sizeof *dead);
    // The open batch may record into the object: it counts as the newest user. The GPU must also be past every
    // submission that names it. tl_noted covers the submitted ones; an object the replay lets go of while it records
    // a submission (vk_exec_recording_fence) may be named by that one too, not yet submitted: vk_texture.c
    // slot_resolve hands a draw a released entry, and entry_of destroys released entries at that address later in
    // the same submission. Stamped with the last submitted value, the object went as soon as the GPU reached it,
    // before the submission that names it ran, and with the recorder thread (vk_rec.c) behind, before the recorder
    // made the call that names it. So every deferral is stamped with the submission being
    // recorded. NX2NAVI_DEFER_STAMP=0 brings back the old stamp.
    static int stamp = -1;
    if (stamp < 0) stamp = rt_knob("DEFER_STAMP", "1") != 0;
    uint64_t tl = tl_noted, rf = stamp ? vk_exec_recording_fence() : 0;
    // vk_exec_recording_fence knows only the recording thread's own submission. A texture a guest thread releases (the
    // synchronous replay: vk_texture_release, entry_of, sweep_released on the game's loader threads) can be named by
    // the submission another thread records meanwhile: that one is issued + 1 (vk_sync_submit sets issued after it).
    if (stamp && !rf) rf = vk_sync_issued() + 1;
    if (rf > tl) tl = rf;
    dead[ndead++] = (Dead){kind, handle, alloc, open_active ? open_b.seq : next_seq, tl};
    if (kind != DEAD_HOST)
        rec_check_handle(kind == DEAD_VIEW ? REC_VIEW : kind == DEAD_BUFFER_VMA ? REC_BUFFER : REC_IMAGE, handle, 0, tl,
                         vk_sync_fence_done());
    plat_rmutex_unlock(&cmd_lock);
}

static int dead_pending(void) {
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    int n = ndead;
    plat_rmutex_unlock(&cmd_lock);
    return n;
}

void vk_defer_image(VkImage img, VmaAllocation alloc) {
    if (img) defer(alloc ? DEAD_IMAGE_VMA : DEAD_IMAGE, (uint64_t)(uintptr_t)img, alloc);
}
void vk_defer_view(VkImageView view) { if (view) defer(DEAD_VIEW, (uint64_t)(uintptr_t)view, NULL); }
void vk_defer_free(void *p) { if (p) defer(DEAD_HOST, (uint64_t)(uintptr_t)p, NULL); }
void vk_defer_buffer(VkBuffer buf, VmaAllocation alloc) {
    if (buf) defer(DEAD_BUFFER_VMA, (uint64_t)(uintptr_t)buf, alloc);
}

void vk_mem_note_submit(uint64_t timeline_value) {
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    if (timeline_value > tl_noted) tl_noted = timeline_value;
    plat_rmutex_unlock(&cmd_lock);
}

// The objects go once cmd_lock is released: destroying an image and its memory takes up to milliseconds (the
// collect's 0.3 slow ones a frame took a median 0.26-0.43 ms, up to 1.9 ms), and the replay's uploads
// (vk_upload_begin) wait for cmd_lock.
void vk_mem_collect(void) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    plat_once(&cmd_once, cmd_init, NULL);
    Dead *out = NULL;
    int nout = 0, cout = 0;
    plat_rmutex_lock(&cmd_lock);
    collect_locked_out(c, &out, &nout, &cout);
    plat_rmutex_unlock(&cmd_lock);
    for (int i = 0; i < nout; i++) destroy_dead(c, &out[i]);
    free(out);
}

void vk_mem_stats(VkMemStats *s) {
    memset(s, 0, sizeof *s);
    plat_mutex_lock_shared(&pool_lock);
    for (int i = 0; i < pool_index.n; i++) {
        const PoolRec *r = pool_index.e[i].buf;
        if (r->kind == POOL_IMPORT) s->imported_pools++;
        else if (r->kind == POOL_PHYS) s->physical_pools++;
        else s->virtual_pools++;
    }
    plat_mutex_unlock_shared(&pool_lock);
    s->import_failures = (uint64_t)n_import_failures;
    s->upload_batches = next_seq;
}

void vk_mem_shutdown(void) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    vk_upload_flush(1);
    vk_queue_lock();
    vkDeviceWaitIdle(c->device);
    vk_queue_unlock();
    vk_gpa_report();
    vk_gpa_shutdown();
    vk_texture_shutdown();
    plat_rmutex_lock(&cmd_lock);
    retire(c, 1);
    uint64_t keep = tl_noted;
    tl_noted = 0;
    for (int i = 0; i < ndead; i++) dead[i].seq = 0, dead[i].tl = 0;   // the device is idle
    collect_locked(c);
    tl_noted = keep;
    for (int i = 0; i < nspare; i++) {
        vkDestroyFence(c->device, spare[i].fence, NULL);
        free(spare[i].stg), free(spare[i].chunk);
    }
    nspare = 0;
    // The staging cache and the arena's free chunks live in stg_pool: before it goes
    for (int i = 0; i < stg_ncache; i++) vmaDestroyBuffer(c->vma, stg_cache[i].buf, stg_cache[i].alloc);
    stg_ncache = 0, stg_cache_bytes = 0;
    for (int i = 0; i < narena_free; i++) vmaDestroyBuffer(c->vma, arena_free[i].buf, arena_free[i].alloc);
    narena_free = 0;
    if (cmd_pool) vkDestroyCommandPool(c->device, cmd_pool, NULL);
    cmd_pool = VK_NULL_HANDLE;
    if (stg_pool) vmaDestroyPool(c->vma, stg_pool);   // after retire: no staging buffer is left in it
    stg_pool = NULL;
    stg_pool_tried = 0;
    plat_rmutex_unlock(&cmd_lock);
    plat_mutex_lock(&pool_lock);
    for (int i = 0; i < pool_index.n; i++) destroy_rec(c, pool_index.e[i].buf);
    pool_index.n = 0;
    nm = npend = nwbl = nsnl = snl_copied = nswl = n_gpa_flip = 0;   // mirrors went with their pools
    swl_marks = -1;
    plat_mutex_lock(&snap_pool_lock);
    for (int i = 0; i < nsnap_pool; i++) vmaDestroyBuffer(c->vma, snap_pool[i].buf, snap_pool[i].alloc);
    nsnap_pool = 0, snap_pool_bytes = 0;
    plat_mutex_unlock(&snap_pool_lock);
    plat_mutex_lock(&shadow_lock);
    nstouch = nstab = 0, s_sb_pages = 0;   // shadows too (their base copies went with their records)
    s_live_n = s_live_bytes = 0;
    plat_mutex_unlock(&shadow_lock);
    for (int i = 0; i < nretiring; i++) destroy_rec(c, retiring[i]);   // finalized ones whose callback never ran
    nretiring = 0;
    npwait = 0;   // those waiting for pool_wait_collect were on the retiring list too
    __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
    nphys = 0;
    plat_mutex_unlock(&pool_lock);
    span_gc(0, 1);   // every pool is gone: so are the spans' references
    plat_mutex_lock(&gen_lock);
    for (uint64_t i = 0; i < GEN_CHUNKS; i++) free(gen_chunk[i]), gen_chunk[i] = NULL, free(cgen_chunk[i]), cgen_chunk[i] = NULL;
    plat_mutex_unlock(&gen_lock);
}

// NX2NAVI_TRACE=perf "mem: sizes" (perf.c): pool records, VRAM mirrors, the deferred destruction list, upload batches,
// the staging cache and arena, the write-back merge snapshot pool and the shadow storage's base copies. Counters only:
// cmd_lock is taken by every upload and release, so nothing is walked under it.
void vk_mem_sizes(PerfSizes *s) {
    plat_mutex_lock_shared(&pool_lock);
    uint64_t pools = (uint64_t)pool_index.n, phys = (uint64_t)nphys;
    plat_mutex_unlock_shared(&pool_lock);
    plat_rmutex_lock(&cmd_lock);
    uint64_t dead_n = (uint64_t)ndead, infl = (uint64_t)ninflight, spare_b = (uint64_t)nspare, stg_n = (uint64_t)stg_ncache,
             stg_b = (uint64_t)stg_cache_bytes, arena_n = (uint64_t)narena_free;
    plat_rmutex_unlock(&cmd_lock);
    plat_mutex_lock(&snap_pool_lock);
    uint64_t snap_n = (uint64_t)nsnap_pool, snap_b = snap_pool_bytes;
    plat_mutex_unlock(&snap_pool_lock);
    plat_mutex_lock(&shadow_lock);
    uint64_t sb = s_sb_pages;
    plat_mutex_unlock(&shadow_lock);
    perf_size(s, "pool records", pools, 0, PERF_SIZE_NONE);
    perf_size(s, "of them physical", phys, 0, PERF_SIZE_NONE);
    perf_size(s, "VRAM mirrors", (uint64_t)__atomic_load_n(&m_live_n, __ATOMIC_RELAXED), 0,
              (uint64_t)__atomic_load_n(&m_live_bytes, __ATOMIC_RELAXED));
    perf_size(s, "deferred destructions", dead_n, 0, PERF_SIZE_NONE);
    perf_size(s, "upload batches in flight", infl, 0, PERF_SIZE_NONE);
    perf_size(s, "spare upload batches", spare_b, 0, PERF_SIZE_NONE);
    perf_size(s, "staging cache", stg_n, STG_CACHE, stg_b);
    perf_size(s, "free staging arena chunks", arena_n, ARENA_KEEP, arena_n * ARENA_CHUNK);
    perf_size(s, "write-back snapshot pool", snap_n, SNAP_POOL_N, snap_b);
    perf_size(s, "shadow base copies", sb, 0, sb * SB_PAGE);
    // the page generations handed out so far (gen_now, 32 bits, no wrap handling): how fast a session uses them up
    perf_size(s, "page generations handed out", (uint64_t)__atomic_load_n(&gen_now, __ATOMIC_RELAXED), 0, PERF_SIZE_NONE);
}
