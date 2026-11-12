// Vulkan textures of the graphics layer: a VkImage cache keyed by storage
// GPU address plus descriptor, uploads through staging buffers with block-linear deswizzle and ASTC decode (vk_astc.c),
// re-uploads when page write generations (vk_memory.c) show the storage written, ReadTexels / WriteTexels, the texture
// and sampler descriptor slots, and VkSamplers.
//
// A texture that mirrors its pool memory (tex_mirrored) stops re-uploading once the GPU rendered into it (the
// "GPU-owned" rule); a finalized slot follows live GPU content or samples its own image as kept, but never revives
// freed CPU-backed storage.
// Contains code Copyright (c) 2026 jhaizhou, Copyright (c) 2026 Peiikai (MIT; see THIRD_PARTY_NOTICES.md).
//
// Every image lives in VK_IMAGE_LAYOUT_GENERAL. Uploads are recorded into the upload batch of vk_memory.c; the replay
// submits it (vk_upload_flush) before a submission that samples the textures.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "flight.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "nvn_tex.h"
#include "tex.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_exec.h"
#include "prof.h"
#include "platform/platform.h"

typedef struct {
    uint64_t addr;
    nvn_texdesc d;
    uint8_t has_layout, layout[8], pad[7];
} TexKey;

// What makes two images interchangeable: the parameters create_tex makes them with (img_key_of). Image reuse and the
// memory requirement cache match on it.
typedef struct { uint32_t flags, type, format, w, h, d, levels, layers, samples, usage; } ImgKey;

typedef struct Tex {
    VkTexImage pub;               // first: vk_texture_get hands out &pub
    TexKey key;
    tex_t t;                      // the newest texture object seen with this key
    TexVkFormat f;
    VmaAllocation alloc;          // own memory; NULL when bound into pool memory
    uint64_t bytes;               // the size of alloc (0 without own memory), for the released entries' total
    VmaAllocation bound_to;       // the pool memory it is bound into
    uint32_t wgen;                // page generation of the last upload check
    uint32_t checked;             // submit_epoch of the last check from a draw binding
    uint32_t use_round;           // look_round when a lookup last reached it (touch)
    uint64_t use_frame;           // gfx_counters.frames then
    uint64_t samp_frame;          // gfx_counters.frames when it was made, a marked shader last sampled it, or a lookup
                                  // other than the texture pool refresh reached it ("disuse")
    uint8_t retiring;             // evicted by disuse, in retiring[], not destroyed yet: the pool refresh leaves it out
    uint64_t retire_takes;        // vk_pool_takes when it was retired
    uint64_t retire_frame;        // gfx_counters.frames when it was retired
    uint64_t retire_due;          // vk_sync_issued once every pool set was taken since (0: not yet); destroyed after it
    int uploaded, released;
    uint64_t up_retry;            // gfx_counters.frames from which an upload that had no memory is tried again (upload)
    uint64_t uid;                 // names the entry in its bucket's released set (tex_gen_flip); 0 until its first release
    uint64_t rel_frame;           // gfx_counters.frames when it was released (drop_stale)
    uint32_t back_gap;            // most presents between a release and its taking back so far (stale_after)
    uint32_t n_up;                // uploads of this entry (vk_texture_debug)
    int last_ch;                  // last vk_mem_changed result of make_fresh: 1 written, 0 clean, -1 not tracked
    struct Tex *next_at;          // other entries at the same storage address
    uint8_t gpu_dirty;            // mirrored, and the GPU wrote the image after the last write-back to the storage
    uint32_t gpu_gen;             // page generation when the GPU write that set gpu_dirty was recorded
    uint64_t gpu_seq;             // order of that GPU write among all of them (gpu_writes)
    uint64_t gpu_fence;           // fence of the submission that made that write (0: not recorded in one)
    uint64_t run_frame;           // mirrored: the frame of the newest GPU write
    uint8_t run;                  // frames in a row up to run_frame with a GPU write, counted up to WB_RUN
    uint64_t wr_fence;            // any entry: newest submission recorded to write its image (render, copy, clear,
                                  // storage binding); ReadTexels waits for it instead of draining the replay queue
    uint8_t listed;               // in owned[]
    uint8_t *snap;                // a texture that fills no page: its storage bytes when the GPU write was recorded
    uint32_t wt_n;                // WriteTexels into the storage since the last upload
    uint64_t chash;               // content_hash of the storage at its last upload (small textures, CHASH_MAX)
    uint8_t chash_ok;             // chash holds it
    struct Tex *rel_prev, *rel_next;   // while released: the list of released entries, oldest first
    ImgKey ik;                    // the image's creation parameters
    uint8_t reusable;             // its image may go to a new texture once it is released (own memory, GPU-only content)
    uint8_t reused;               // its image may still be in use from before (a released entry's, or its own restarted by
                                  // start_at): the first barrier waits for all earlier work
    uint8_t ru_on;                // in the reuse list of ik
    uint8_t superseded;           // released, and another descriptor's entry was made at its address since (start_at)
    uint32_t rel_epoch;           // submit_epoch when it was released
    struct Tex *ru_prev, *ru_next;   // the reuse list of ik, newest release first
    uint64_t gw_frame;            // gfx_counters.frames at its creation or newest GPU write (overwrite_others)
    uint64_t scan_frame;          // one more than the frame vk_texture_gpu_written last looked for overwritten entries
    uint64_t ow_frame;            // gfx_counters.frames when overwritten was set
    uint8_t overwritten;          // released, and a texture at another address was drawn over its storage (overwrite_others)
    uint8_t in_gwin;              // in gwin[]
    int pidx;                     // 1 + its index in pent[] (vk_texture_pool_note_locked); 0: not listed
    int ph;                       // 1 + its index in pref[] (pool_ref); 0: none
    // Where the entry and its content came from ("origins", below drop_note), for the lines of NX2NAVI_TRACE=perf
    uint8_t had;                  // HAD_*: what put content into the image of an entry the CPU does not write
    uint8_t og_start;             // START_*: how its content started (start_at)
    uint8_t og_drop;              // DROP_*: the drop of this descriptor's last entry, when the ring still knew it
    uint8_t og_reimg;             // made with the image of a released entry (create_tex, reuse)
    uint8_t bc1_try;              // ASTC as BC3 that its first upload makes BC1 when every block is opaque (upload)
    uint8_t said;                 // marks_check wrote its line for this entry
    uint8_t sfill;                // split fill: the CPU does not write it, and its image is the storage's bytes: 1 as a
                                  // CPU-written one's until the GPU writes it, 2 once (from_storage)
    uint32_t og_backs;            // times it was taken back after a release (entry_of)
    uint64_t og_frame;            // gfx_counters.frames when it was made
    uint64_t og_drop_frame;       // gfx_counters.frames of the drop og_drop names
    uint64_t og_back_frame;       // gfx_counters.frames when it was last taken back
    VkTexImage *flat3d;           // sampled as a 3D texture: its one-slice 3D copy (vk_texture_flat3d_locked), else NULL
    VmaAllocation flat3d_alloc;
    uint32_t flat3d_up, flat3d_layer;   // n_up and the layer of the copy's last refresh
    uint64_t flat3d_wr;           // wr_fence then
} Tex;

// A slot keeps what vk_texture_by_id resolved for it, so a draw binding skips the key lookup and the address chain: the
// entry (ce), valid while the generation of its address (agen) is still cgen, the view and has_view handed out with it,
// and the submit_epoch of the last freshness check through the slot (cepoch). Hot fields first: a lookup that hits
// touches one or two cache lines.
typedef struct {
    struct Tex *ce;               // NULL: nothing cached (registered since, or never resolved)
    uint32_t cgen;                // agen of the slot's address when ce was resolved
    uint32_t cepoch;
    view_t cview;
    uint8_t chas_view, used, has_view, finalized;   // finalized survives eviction of the released Tex entry
    uint8_t cheld;                // ce is NULL as the texture was evicted by disuse, valid while cgen is (held_out)
    uint8_t cwant;                // a shader sampled its stand-in (vk_pool_marks_read): slot_resolve makes it at once
    uint64_t khash;               // hash of the descriptor key and view, 0 for CPU-written textures (vk_texture_slot_khash_locked)
    view_t v;
    tex_t t;
} TexSlot;
typedef struct { samp_par p; VkSampler s, sp, sc, sd[2]; uint8_t used; } SampSlot;   // s: vk_sampler_for(&p) once looked up; sp:
                                                                               // its point-sampled form (vk_sampler_point_by_id_locked),
                                                                               // sc its comparing form (vk_sampler_compare_by_id_locked),
                                                                               // sd[n - 1] its form for a texture made without n
                                                                               // levels (vk_sampler_detail_by_id_locked)

static PlatRMutex tex_lock;
static PlatOnce tex_once = PLAT_ONCE_INIT;
static nvn_hmap by_key = {.klen = sizeof(TexKey)};
static nvn_hmap by_addr = {.klen = 8};
static TexSlot *tslots;
static int ntslots;
static SampSlot *sslots;
static int nsslots;
static nvn_hmap samplers = {.klen = sizeof(samp_par)};
static int nreleased;
static Tex *rel_head, *rel_tail;   // released entries in release order (sweep_released drops from the head)
static uint64_t n_uploads, n_reuploads;
// Re-uploads since the last perf line (tex_lock): all, and those of textures a WriteTexels wrote since their last upload.
static uint64_t ru_n, ru_bytes, ru_ns, ru_wt_n, ru_wt_bytes, ru_wt_ns;
static uint64_t ru_same_n, ru_same_bytes;   // re-uploads skipped: the pages were written, the texture's bytes were not
// NX2NAVI_TEX_ALIAS (default on; =0: a new image over live storage starts empty): a new descriptor over the storage of a
// live image that the GPU wrote gets that image's content (alias_copy). Counts since the last log line.
static int tex_alias = -1;
static uint64_t n_alias_copied, n_alias_upload, n_alias_uninit, n_alias_kept, n_alias_released;
// New depth and stencil images (not reused) init_new started at the replay's position, those of them for which an open
// pass was ended, and those that went in the upload batch (no replay recording), since the last perf line.
static uint64_t n_dinit_inline, n_dinit_inpass, n_dinit_upload;
static uint32_t submit_epoch = 1;   // bumped per queue submit: draw bindings check a texture's storage once per submit
// The thresholds further down that count presents (STALE_FRAMES, GPU_STALE_FRAMES, OVERWRITTEN_KEEP, DISUSE_FRAMES)
// were set at 60 presents a second. Without a limit the game presents 85 to 240 a second (a menu), and "120 presents"
// was then half a second and "30 s without a sample" 7 to 20 s: under memory pressure textures went three times as
// readily as meant, and the ones still on screen showed their dark stand-ins until they were made again.
// tex_rate is the presents per second as a multiple of 60 in 1/256 (256 at 60 or fewer), the highest of the last RATE_N
// seconds so that one slow second does not shorten the thresholds; PRESENTS(n) is n presents at 60 in today's presents.
#define RATE_N 8
static uint32_t tex_rate = 256;
#define PRESENTS(n) ((uint64_t)(n) * tex_rate >> 8)

static void rate_tick(void) {   // each submission, tex_lock held
    static uint64_t t0, f0;
    static uint32_t ring[RATE_N], at;
    uint64_t t = kern_now_ns(), f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (!t0) t0 = t, f0 = f;
    if (t - t0 < 1000000000ull) return;
    uint64_t r = (f - f0) * 256ull * 1000000000ull / ((t - t0) * 60);
    ring[at++ % RATE_N] = r < 256 ? 256 : r > 2048 ? 2048 : (uint32_t)r;
    t0 = t, f0 = f;
    uint32_t m = 256;
    for (int i = 0; i < RATE_N; i++)
        if (ring[i] > m) m = ring[i];
    tex_rate = m;
}
// Mirrored textures the GPU wrote into, sorted by address (vk_texture_cpu_stale, vk_texture_write_back). Changed under
// tex_lock and owned_lock; read under either.
static PlatMutex owned_lock = PLAT_MUTEX_INIT;
static Tex **owned;
static int nowned, cowned;
static uint64_t max_owned;   // largest storage size in owned[]
static uint64_t gpu_writes;  // GPU writes into mirrored textures so far: Tex.gpu_seq
// Bumped whenever the entry a slot resolves to can change: an entry created or destroyed, released or taken back into
// use, or a registered slot finalized (the follow reads by_key, by_addr and the entry and slot lifetime flags).
static uint32_t tex_gen = 1;
// Bumped with tex_gen and whenever a slot is registered: what vk_texture_by_id returns for any slot may have changed.
static uint32_t slots_gen = 1;
static uint32_t samp_gen = 1;   // bumped whenever a sampler slot is registered: vk_sampler_by_id may answer otherwise
static uint64_t n_checks;    // draw-binding storage checks (make_fresh_snap) since the last perf line
// tex_gen per storage address (hashed): a slot resolves through the entries at its own address only (slot_resolve), so
// a slot's resolution stays valid while its address's generation does. NX2navi finalizes and initializes transient
// textures every frame: about 90 tex_gen and 160 slots_gen bumps per frame in the monsters fight, one every 16 draws,
// which emptied every cache keyed on the global generations. 65536 buckets, the most the 16-bit index of sinfo,
// slot_bucket and the dirty ring's bucket entries holds: with 4096, about 8 slots shared each bucket, and every
// transient texture made or released at one address made the texture pool refresh (vk_exec_draw.c pool_sync_entries)
// refill its neighbours too, most of its 3-3.7 ms per frame in Kokiri Forest.
#define AGEN_N 65536
static uint32_t agen[AGEN_N];
// Per slot id, for vk_texture_slot_token without the lock: the registration serial (bits 16-47; 0: never registered)
// and the address generation index (bits 0-15), one word so a reader gets a consistent pair. Fixed-size (slot ids are
// 20 bits), unlike tslots, which moves when it grows.
static uint64_t *sinfo;
static uint32_t reg_serial;
static uint64_t n_same_regs;   // vk_texture_register calls that changed nothing (vk_texture_same_regs)

static uint32_t agen_idx(uint64_t addr) { return (uint32_t)(addr * 0x9E3779B97F4A7C15ull >> 48); }

// Slots that may resolve differently since a position, for the texture pool descriptors (vk_exec_draw.c
// pool_sync_entries), which otherwise looked at every registered slot after each change: Ocarina registers slots all
// through a submission, and the refresh walked the whole table about twenty times per submission (3.5 ms). A ring of
// changes, each a slot id (registered) or an address bucket (an entry at that agen index changed: every slot registered
// over it, kept in per-bucket lists). Lock held for all of it.
#define DIRTY_N (1u << 16)
#define DIRTY_BUCKET 0x80000000u
static uint32_t dirty[DIRTY_N];
static uint64_t ndirty;
static int32_t bucket_head[AGEN_N];
static int32_t *slot_next, *slot_prev;   // per slot id: its bucket list links (slot_prev -2: in no list)
static uint16_t *slot_bucket;

static void dirty_push(uint32_t x) { dirty[ndirty++ % DIRTY_N] = x; }

// tex_gen_bump calls, by site: destroy, create, take-back, release; and the releases and take-backs that went back to a
// generation their bucket's state had before (tex_gen_flip), for vk_texture_bucket_stats
static uint64_t n_bumps, n_site[4], n_undone;
static uint32_t bump_count[AGEN_N];
// The highest generation each bucket ever had. A new state takes the next one after it, not the one after the current
// value, so a value a bucket went back from (tex_gen_flip) is never given to another state.
static uint32_t agen_top[AGEN_N];
// What a slot at an address resolves to depends on the entries there and on which of them are released
// (slot_resolve). Per bucket: epoch counts every change other than a release or a take-back (an entry made, destroyed,
// retired, held or taken back with more than its flag changing), sig is the released entries' uids XORed, which a
// release and a take-back each flip. (bucket, epoch, sig) names the state; gs remembers the generation each state
// got, so a bucket that comes back to a state gets that state's generation back.
static uint32_t bepoch[AGEN_N];
static uint64_t bsig[AGEN_N];
#define GS_N 8192
static struct { uint64_t sig; uint32_t b, epoch, gen, valid; } gs[GS_N];
// A slot lookup is resolving (vk_texture_by_id_locked around slot_resolve). The lookup's callers stamp what they keep
// with the generation they read before it: the slot's own cache, the pool's tokens, the descriptor, type and feedback
// memos (vk_exec_draw.c). A lookup that itself changes the state (it takes a released entry back for a slot registered
// since the release) leaves such a stamp naming the state it left, with a result that state would not give: the next
// release of that entry is later than the slot's registration, the slot is stale then (slot_resolve), and the kept
// result would hand out the released entry, whose storage the game may be unmapping. So the state such a lookup
// leaves loses its remembered generation (tex_gen_flip): it gets a new one when the bucket is next in it, and the
// stamp never matches again.
static int in_lookup;

static uint32_t gs_at(uint32_t b) {
    return (uint32_t)(((bsig[b] ^ ((uint64_t)b << 32 | bepoch[b])) * 0x9E3779B97F4A7C15ull) >> 51);
}

static void tex_gen_bump(uint64_t addr) {
    n_bumps++;
    bump_count[agen_idx(addr)]++;
    if (!++tex_gen) tex_gen = 1;
    slots_gen++;
    uint32_t b = agen_idx(addr);
    bepoch[b]++;
    __atomic_store_n(&agen[b], ++agen_top[b], __ATOMIC_RELEASE);
    gs[gs_at(b)] = (typeof(gs[0])){bsig[b], b, bepoch[b], agen[b], 1};
    dirty_push(DIRTY_BUCKET | b);
}

// NX2NAVI_TEX_GEN_UNDO (default 1; 0: every release and take-back is a new generation). Ocarina finalizes and
// initializes transient textures all through a frame: in a large town about 40,000 releases and as many take-backs
// per 3,600 submissions, each a bump of its address bucket, and the buckets with the most slots (1,300 registered over
// one address) were the ones bumped most. The texture pool refresh refilled about 6,000 slots per submission for it.
// A release or a take-back changes one entry's released flag and nothing else, so it moves the bucket between states
// it has usually been in before: the textures the game keeps released and takes back for a moment, and those it
// releases and takes straight back. The bucket then gets the generation that state had, and whatever was resolved in
// that state counts as current again: the pool's tokens, the descriptor memo and the slots' own cached entries. A
// state seen for the first time gets a new generation (agen_top), and any other change starts a new epoch, so a
// generation never stands for two states. The global generations still move, so their readers look, and find the
// tokens equal. plain: the take-back of an entry that was superseded or overwritten while released changes more than
// the flag, and is a change like any other.
// One input of slot_resolve is not in the state: the stale rule reads the slot's finalized flag, which a release sets
// on the slots registered with its descriptor and a registration clears. A slot is stale at every later release once
// it is at one, so what it resolves to in a released state stays right; the one that is not stale takes the entry
// back, which is the lookup-made change in_lookup deals with.
static void tex_gen_flip(Tex *e, int plain) {
    static int on = -1;
    static uint64_t uids;
    if (on < 0) on = rt_knob("TEX_GEN_UNDO", "1") == 1;
    if (!e->uid) {
        uint64_t z = ++uids * 0x9E3779B97F4A7C15ull;
        z = (z ^ z >> 30) * 0xBF58476D1CE4E5B9ull, z = (z ^ z >> 27) * 0x94D049BB133111EBull;
        e->uid = (z ^ z >> 31) | 1;
    }
    uint32_t b = agen_idx(e->key.addr);
    if (in_lookup) {   // see in_lookup: the state this lookup leaves is not to get its generation again
        typeof(gs[0]) *m0 = &gs[gs_at(b)];
        if (m0->valid && m0->b == b && m0->epoch == bepoch[b] && m0->sig == bsig[b]) m0->valid = 0;
    }
    bsig[b] ^= e->uid;
    if (!on || plain) {
        tex_gen_bump(e->key.addr);
        return;
    }
    if (!++tex_gen) tex_gen = 1;
    slots_gen++;
    typeof(gs[0]) *m = &gs[gs_at(b)];
    if (m->valid && m->b == b && m->epoch == bepoch[b] && m->sig == bsig[b]) {
        n_undone++;
        __atomic_store_n(&agen[b], m->gen, __ATOMIC_RELEASE);
    } else {
        n_bumps++;
        bump_count[b]++;
        __atomic_store_n(&agen[b], ++agen_top[b], __ATOMIC_RELEASE);
        *m = (typeof(*m)){bsig[b], b, bepoch[b], agen[b], 1};
    }
    dirty_push(DIRTY_BUCKET | b);
}

// Slot groups: the GPU-written slots of one address bucket that share a descriptor hash (TexSlot.khash) resolve alike
// under the same address and entry generations, as the texture pool's memo has it (vk_exec_draw.c pool_fill). Ocarina
// keeps up to ~400 descriptors of each transient render target registered over its storage, and the fullest bucket,
// 1,281 slots, moves 8 times a submission in Kokiri Forest: 12,800 of the 17,600 slots a submission's dirty walks
// refilled held their views already. The dirty walk can hand each group to the pool whole (vk_texture_dirty_slots_locked's
// gfn), which leaves its members alone when their resolution did not change (NX2NAVI_POOL_GROUP). A slot is in its
// bucket's own list (khash 0: a CPU-written texture) or in its group's member list, through the same links. A slot that
// joins a group goes to its tail and leaves the group's stamp as it was: its registration put it on the dirty ring, so
// the walk that follows refills it on its own, and the head, whose state the pool tests, is a member from before. (The
// game's 118 registrations a submission cleared the stamps of the groups they joined, and the walk at a submission's
// start went member by member as before: 11,300 of 11,550 slots.)
typedef struct { VkSlotGroup pub; int32_t gnext, gprev, tail; uint32_t bucket; } SlotGroup;
static SlotGroup *grp;
static int ngrp, cgrp, grp_free = -1, ngrp_live;
static uint64_t n_grouped;               // slots in groups
static int32_t bucket_ghead[AGEN_N];     // per bucket: its first group (-1: none; set with the lists)
static int32_t *slot_grp;                // per slot id: its group, -1 none
typedef struct { uint64_t kh, b; } GrpKey;
static nvn_hmap grp_by = {.klen = sizeof(GrpKey)};   // -> group + 1

static int grp_get(uint64_t kh, uint32_t b) {
    GrpKey k = {kh, b};
    void *v = nvn_hm_get(&grp_by, &k);
    if (v) return (int)((intptr_t)v - 1);
    int g;
    if (grp_free >= 0) {
        g = grp_free, grp_free = grp[g].gnext;
    } else {
        if (ngrp == cgrp) {
            int n = cgrp ? cgrp * 2 : 1024;
            SlotGroup *p = realloc(grp, sizeof *p * (size_t)n);
            if (!p) rt_fatal("vk: out of memory for texture slot groups");
            grp = p, cgrp = n;
        }
        g = ngrp++;
    }
    SlotGroup *G = &grp[g];
    memset(G, 0, sizeof *G);
    G->pub.kh = kh, G->pub.head = G->tail = -1, G->bucket = b;
    G->gprev = -1, G->gnext = bucket_ghead[b];
    if (G->gnext >= 0) grp[G->gnext].gprev = g;
    bucket_ghead[b] = g;
    nvn_hm_put(&grp_by, &k, (void *)(intptr_t)(g + 1));
    ngrp_live++;
    return g;
}

static void grp_drop(int g) {
    SlotGroup *G = &grp[g];
    GrpKey k = {G->pub.kh, G->bucket};
    nvn_hm_del(&grp_by, &k);
    if (G->gprev >= 0) grp[G->gprev].gnext = G->gnext;
    else bucket_ghead[G->bucket] = G->gnext;
    if (G->gnext >= 0) grp[G->gnext].gprev = G->gprev;
    G->pub.kh = 0, G->pub.head = G->tail = -1, G->pub.n = 0, G->pub.stamp = 0;
    G->gnext = grp_free, grp_free = g;
    ngrp_live--;
}

// Places slot id in bucket b: at the tail of its group when it has a descriptor hash, else at the head of the bucket's
// own list (the lists exist once a slot was registered: vk_texture_register).
static void slot_place(int id, uint32_t b, uint64_t kh) {
    if (slot_prev[id] != -2) {   // out of its old list
        int32_t g = slot_grp[id];
        int32_t *head = g >= 0 ? &grp[g].pub.head : &bucket_head[slot_bucket[id]];
        if (slot_prev[id] >= 0) slot_next[slot_prev[id]] = slot_next[id];
        else *head = slot_next[id];
        if (slot_next[id] >= 0) slot_prev[slot_next[id]] = slot_prev[id];
        else if (g >= 0) grp[g].tail = slot_prev[id];
        slot_grp[id] = -1;
        if (g >= 0) {
            n_grouped--;
            if (!--grp[g].pub.n) grp_drop(g);
        }
    }
    slot_bucket[id] = (uint16_t)b;
    if (kh) {
        int g = grp_get(kh, b);   // may move grp
        SlotGroup *G = &grp[g];
        slot_grp[id] = g;
        G->pub.n++, n_grouped++;
        slot_next[id] = -1;
        slot_prev[id] = G->tail;
        if (G->tail >= 0) slot_next[G->tail] = id;
        else G->pub.head = id;
        G->tail = id;
        return;
    }
    slot_prev[id] = -1;
    slot_next[id] = bucket_head[b];
    if (bucket_head[b] >= 0) slot_prev[bucket_head[b]] = id;
    bucket_head[b] = id;
}

VkSlotGroup *vk_texture_group_locked(int g) { return &grp[g].pub; }
int vk_texture_group_next_locked(int id) { return slot_next[id]; }
void vk_texture_groups_unstamp_locked(void) {
    for (int g = 0; g < ngrp; g++) grp[g].pub.stamp = 0;
}

uint64_t vk_texture_dirty_pos_locked(void) { return ndirty; }

// A bucket listed several times since *pos is walked once a round: the walk reads its slots' tokens after every change
// listed before the round began (the lock is held). Changes the walk itself makes (an entry a lookup created) are the
// next round's. A bucket's slot groups go to gfn when given, else their members to fn.
int vk_texture_dirty_slots_locked(uint64_t *pos, void (*fn)(int id, void *arg), void (*gfn)(int g, void *arg), void *arg) {
    if (!slot_next || ndirty - *pos > DIRTY_N) {
        *pos = ndirty;
        return 0;
    }
    static uint32_t seen[AGEN_N], round;
    for (uint64_t q = *pos; q < ndirty;) {
        uint64_t end = ndirty;
        if (!++round) memset(seen, 0, sizeof seen), round = 1;
        for (; q < end; q++) {
            uint32_t x = dirty[q % DIRTY_N];
            if (x & DIRTY_BUCKET) {
                uint32_t b = x & ~DIRTY_BUCKET;
                if (seen[b] == round) continue;
                seen[b] = round;
                for (int32_t id = bucket_head[b]; id >= 0; id = slot_next[id]) fn(id, arg);
                for (int32_t g = bucket_ghead[b]; g >= 0; g = grp[g].gnext) {   // the walk registers nothing
                    if (gfn) gfn(g, arg);
                    else for (int32_t id = grp[g].pub.head; id >= 0; id = slot_next[id]) fn(id, arg);
                }
            } else {
                fn((int)x, arg);
            }
        }
    }
    *pos = ndirty;
    return 1;
}

static volatile int32_t tex_ready;
static void tex_init(void *p) {
    (void)p;
    __atomic_store_n(&tex_ready, 1, __ATOMIC_RELEASE);
    return;
}
// NX2NAVI_LOCK_TIME: the outermost hold of the lock is timed from here, and one over 1 ms is reported with the code
// that took it (perf_lock_hold); lock is not inlined so that its return address names that code.
static uint64_t hold_t0;
static void *hold_site;
__attribute__((noinline)) static void lock(void) {
    if (!__atomic_load_n(&tex_ready, __ATOMIC_ACQUIRE)) plat_once(&tex_once, tex_init, NULL);
    plat_rmutex_lock_at(&tex_lock, __builtin_return_address(0));
    if (tex_lock.depth == 1 && perf_lock_holds_on()) hold_t0 = plat_now_ns(), hold_site = __builtin_return_address(0);
}
static void unlock(void) {
    if (tex_lock.depth == 1 && hold_t0) {
        uint64_t d = plat_now_ns() - hold_t0;
        hold_t0 = 0;
        if (d > 1000000) perf_lock_hold(hold_site, d);
    }
    plat_rmutex_unlock(&tex_lock);
}

static uint32_t umax(uint32_t a, uint32_t b) { return a > b ? a : b; }
static uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t mip(uint32_t v, unsigned lv) { return umax(v >> lv, 1); }

static void make_key(const tex_t *t, TexKey *k) {
    memset(k, 0, sizeof *k);
    k->addr = (uintptr_t)t->mem;
    k->d = t->d;
    k->has_layout = t->has_layout;
    memcpy(k->layout, t->layout, 8);
}

// Keys equal apart from the descriptor's swizzle selectors: the same storage, format and geometry.
static int same_but_swizzle(const TexKey *a, const TexKey *b) {
    TexKey x = *a, y = *b;
    x.d.swizzle_r = x.d.swizzle_g = x.d.swizzle_b = x.d.swizzle_a = 0;
    y.d.swizzle_r = y.d.swizzle_g = y.d.swizzle_b = y.d.swizzle_a = 0;
    return !memcmp(&x, &y, sizeof x);
}

// Whether a slot (key k) may follow entry p, another descriptor at the same storage, when the slot's own entry is
// released (slot_resolve). Ocarina's per-frame render targets share storage, so the live entry at an address is often
// a target of another size that the frame laid out there: an effect's 1130x86 strip over the 480x270 half-size
// lighting term at a 1280x720 scene. Followed, the compute pass that writes the term and the full-screen pass that
// reads it both got the strip's image, the term read back as zero over most of the screen and a jump attack's hit
// effect drew the right 44% of the scene black. A slot follows only an entry of its own size; block-compressed
// formats are left out of the test, as a GPU encoder renders their blocks through a smaller uncompressed view of the
// same storage. Counted in n_follow_refused.
static uint64_t n_follow_refused;
static int follow_size_ok(const Tex *p, const TexKey *k) {
    int bw1 = 1, bh1 = 1, bw2 = 1, bh2 = 1;
    nvn_fmt_bpb(p->key.d.format, &bw1, &bh1);
    nvn_fmt_bpb(k->d.format, &bw2, &bh2);
    if (bw1 > 1 || bh1 > 1 || bw2 > 1 || bh2 > 1) return 1;
    if (p->key.d.width == k->d.width && p->key.d.height == k->d.height && p->key.d.depth == k->d.depth) return 1;
    if (n_follow_refused++ < 8)
        rt_log("vk: a texture slot at %#llx (%ux%u format %#x) did not follow a %ux%u texture at the same storage: "
               "its own image instead", (unsigned long long)k->addr, k->d.width, k->d.height, k->d.format,
               p->key.d.width, p->key.d.height);
    return 0;
}

// Textures whose content the CPU provides in pool memory. Display, compressible, zcull,
// depth, multisample and GPU-only storage stays GPU-owned.
// mirror_refused: NULL for such a texture, else the rule that leaves it out, in words for a log line.
static const char *mirror_refused(const tex_t *t) {
    if (!t->mem) return "no storage";
    if (nvn_gpu_only((uintptr_t)t->mem)) return "storage in the GPU-only address window";
    if (t->has_layout) return NULL;
    if (!(t->pool_flags & (NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_CPU_CACHED)))
        return "a pool the CPU has no access to";
    if (t->d.flags & (NVN_TEXTURE_FLAGS_DISPLAY | NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_ADAPTIVE_ZCULL |
                      NVN_TEXTURE_FLAGS_DEPTH16_PREFER_FAST_CLEAR | NVN_TEXTURE_FLAGS_ZCULL_SUPPORT_STENCIL))
        return "texture flags (one of 0x1, 0x8, 0x80, 0x100, 0x400)";
    if (t->d.samples > 1) return "multisample";
    if (t->d.target == NVN_TEXTURE_TARGET_BUFFER) return "a buffer texture";
    if (t->d.format >= 0x31 && t->d.format <= 0x36) return "a depth or stencil format";
    return NULL;
}

static int tex_mirrored(const tex_t *t) { return !mirror_refused(t); }

static void log_limited(volatile int32_t *n, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void log_limited(volatile int32_t *n, const char *fmt, ...) {
    if (plat_inc32(n) > 16) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_log("vk: %s", buf);
}

static void barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst,
                    VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa,
                           .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
#define ALL_RW VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT

// ---------------------------------------------------------------- images
static VkFormatFeatureFlags features_of(VkFormat f) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(vk_ctx()->phys, f, &p);
    return p.optimalTilingFeatures;
}

static VkImageAspectFlags aspect_of(const TexVkFormat *f) {
    VkImageAspectFlags a = 0;
    if (f->depth) a |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (f->stencil) a |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return a ? a : VK_IMAGE_ASPECT_COLOR_BIT;
}

// Binds img into the pool memory behind its GPU address (physical pools, virtual pools through the page table) when
// the memory type, alignment and contiguous size allow; 0 otherwise.
// A depth / stencil image that needs more bytes than its NVN storage (guest_size) is not bound: AMD's RDNA2 driver
// (26.10.1) puts the depth compression metadata after the depth data, and above 1080p that tail lies over the next
// texture of the game's heap; each depth write then scrambled it (garbage band at the top, pink glow, blocks in the
// grass, a blue checkered scene in the menu). Colour images keep their place: the RX 9070 also sizes some of them
// above their storage, and nothing there showed that the GPU writes the excess.
static int format_compressed(VkFormat f) {   // vkCmdClearColorImage takes no block-compressed format
    return (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) ||
           (f >= VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK && f <= VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK);
}

// Image reuse (NX2NAVI_TEX_REUSE, default on): a GPU-only texture that needs an image of its own takes the image of a
// released entry made with the same parameters (ImgKey) instead of a new one, which then starts over as a new image
// would (init_image: undefined content, cleared). Ocarina places its per-frame render targets with a linear allocator
// behind small targets whose size follows an effect on screen (77x77 to 97x97), so every few frames the targets after
// them move to new addresses, new descriptors whose images were made again: vkCreateImage, the memory requirements and
// the binding of a dedicated allocation cost 8% of the replay in Kokiri Forest. The released entries of each parameter
// set are kept in a list, newest release first; create_tex takes the one nearest in address of those released at least
// two frames earlier, never during the current submission's replay, whose later parts may still use it, and an
// overwritten one (below) before any other.
static nvn_hmap reuse = {.klen = sizeof(ImgKey)};   // ImgKey -> the newest released reusable entry
static int tex_reuse = -1;
static uint64_t n_reused, n_reuse_new;   // since the last perf line: images taken from a released entry, made anew
static uint64_t n_restarts;              // superseded entries taken back (start_at)

// Moved render targets. Ocarina keeps about a thousand texture objects for its per-frame render targets and lays the
// targets out anew every frame with a linear allocator in one GPU-only pool. A target early in that layout changes its
// size with an effect on screen (811x811, then 778x778 a frame later in Kakariko Village), and every target after it
// then lies about 100 KB further on: eleven targets, 55 MiB of images, at new addresses in each frame the size
// changes, each a new entry with an image of its own, as few of them can lie in the pool memory itself (pool_fits).
// The game finalizes an old texture object only when it needs the object again, 90 to 300 frames later, and a
// descriptor in the texture pool goes on naming it, so the entries were neither released nor idle: after 6,000 frames
// of walking through the village 1,043 of them held 6.1 GiB of device memory, against 233 MiB in the targets still
// drawn, and once released they waited GPU_STALE_FRAMES more. A driver that charges device memory to the system's
// commit kept the commit low from then on, and the idle textures of the area went and came back (commit_evict).
//
// On the console those targets are views of one pool's memory: when another target is drawn over a target's bytes, its
// content is gone. overwrite_others says so here. When the GPU writes a texture in the GPU-only address window
// (nvn_gpu_only), each entry at another address whose storage overlaps it is marked overwritten and released as if
// the game had finalized it. Its image then goes to the next texture made with the same parameters (create_tex), or
// away OVERWRITTEN_KEEP presents later (drop_overwritten), or as early as an intact entry's while memory is short
// (drop_stale). Left alone are an entry at the same address (start_at decides about those), an entry the written
// texture lies inside (a level or a layer of it, which a game draws through a texture of its own) and an entry
// written in this frame or the one before: two layouts that take turns within a frame keep their images. The mark
// goes when the game takes the entry back (entry_of).
// gwin: the entries this applies to (reusable, in the window), sorted by address; gwin_span: the largest storage size
// one of them had, which bounds how far below an address an overlapping entry can start.
#define OVERWRITE_FRAMES 2
#define OVERWRITTEN_KEEP PRESENTS(600)
static struct Tex **gwin;
static int ngwin, cgwin;
static uint64_t gwin_span;
static uint64_t n_overwritten, n_over_drops;   // since the last perf line: entries marked, dropped by drop_overwritten

static int gwin_lower(uint64_t a) {
    int lo = 0, hi = ngwin;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (gwin[m]->key.addr < a) lo = m + 1;
        else hi = m;
    }
    return lo;
}

static void gwin_add(Tex *e) {
    if (ngwin == cgwin) {
        cgwin = cgwin ? 2 * cgwin : 256;
        if (!(gwin = realloc(gwin, sizeof *gwin * (size_t)cgwin))) rt_fatal("vk: out of memory for the render target list");
    }
    int i = gwin_lower(e->key.addr);
    memmove(gwin + i + 1, gwin + i, sizeof *gwin * (size_t)(ngwin - i));
    gwin[i] = e, ngwin++;
    if (e->t.L.size > gwin_span) gwin_span = e->t.L.size;
    e->in_gwin = 1;
}

static void gwin_remove(Tex *e) {
    if (!e->in_gwin) return;
    for (int i = gwin_lower(e->key.addr); i < ngwin && gwin[i]->key.addr == e->key.addr; i++)
        if (gwin[i] == e) {
            memmove(gwin + i, gwin + i + 1, sizeof *gwin * (size_t)(ngwin - i - 1));
            ngwin--;
            break;
        }
    e->in_gwin = 0;
}

static int tex_reuse_on(void) {
    if (tex_reuse < 0) {
        tex_reuse = cfg_bool("NX2NAVI_TEX_REUSE");
        rt_log("vk: texture image reuse %s (NX2NAVI_TEX_REUSE)", tex_reuse ? "on" : "off");
    }
    return tex_reuse;
}

static void destroy_tex(Tex *e);

// Why an entry was destroyed (drop_tex), in the order of drop_names.
enum { DROP_NONE, DROP_MAX, DROP_CAP, DROP_COMMIT, DROP_VRAM, DROP_IDLE_VRAM, DROP_IDLE_COMMIT, DROP_DISUSE,
       DROP_OVERWRITTEN, DROP_REPLACED, DROP_REUSED, DROP_RANGE, DROP_EXACT, DROP_SHUTDOWN, DROP_N };
static void drop_tex(Tex *e, int why);
// How an entry's content started (start_at; Tex.og_start), in the order of start_names.
enum { START_NEW, START_UPLOAD_LIVE, START_COPY_LIVE, START_COPY_RELEASED, START_EMPTY_LIVE, START_EMPTY_RELEASED,
       START_COPY_GONE, START_FILL, START_N };
// What put content into the image of an entry the CPU does not write (Tex.had); a GPU write is pub.gpu_owned.
enum { HAD_COPY = 1, HAD_TEXELS = 2, HAD_FILL = 4 };

// ---- split fill (NX2NAVI_TEX_SPLIT_FILL, tex_split_fill in nx2navi.ini: 1 the default, 0 off, 2 filled once)
// Two descriptors can name one storage the CPU writes: a texture whose content the cache takes from the CPU
// (tex_mirrored), and a second one with a texture flag for which tex_mirrored refuses. The second has no upload of its
// own: its slot shows the first one's image for as long as that entry is live (slot_resolve). Once the first is
// released, a lookup of the second's slot makes the second an entry of its own (entry_of), start_at destroys the
// released first one, and the new image has nothing to start from: cleared, never uploaded, and from then on what
// the slot shows, also after the first descriptor is registered again and gets a new entry beside it. On the console
// both descriptors read the same bytes. tex_probe.c test_split makes the sequence and reads the image back: zeros.
//
// With split fill, a new entry the CPU does not write, at storage the CPU can see, starts with those bytes:
//  - by a copy on the GPU from the released CPU-written entry start_at is about to destroy, when that image holds the
//    storage as it is now (uploaded, and no write since its last check) or content the GPU wrote, and
//    alias_compatible allows;
//  - else from the storage itself, uploaded as a CPU-written texture's is (upload).
// With 1 the image then follows the storage as a CPU-written one's does (from_storage: make_fresh, the pool's list)
// until the GPU or WriteTexels writes it. The texture pool refresh makes such an entry also for a slot whose descriptor
// the game has let go of, long before the next texture's bytes arrive in that memory, and when both descriptors are
// registered again the slot finds that entry: filled once (2), it shows the texture that was there before.
// Left out: depth, stencil and multisample images, buffer textures, pools the CPU has no access to, storage over
// SPLIT_FILL_MAX, and an entry made with a released entry's image, whose first transition is recorded at the replay's
// position, after the upload batch (init_new); so create_tex gives an entry that is to be filled a new image. Nothing
// is filled where a live entry at the address holds content the GPU wrote (that is what the new one should show, and
// start_at copies it when it can), nor with NX2NAVI_TEX_ALIAS off.
//
// What follows the storage also takes what a stale descriptor's entry does not need: an entry made for a slot the game
// let go of is uploaded again whenever its pages are written, by the next textures loaded into that memory, read in
// its own format. The perf line counts those ("filled again after a write"); 2 does without them.
//
// What it costs: an upload (a copy out of the storage, a decode for ASTC) for each such entry that finds no image to
// copy from, where the image was only cleared before. A render target in a CPU pool that the game makes anew often
// pays that every time and no longer takes a released entry's image; the perf line counts the fills, their bytes and
// their time. Until the GPU first writes it, its storage is also checked for writes once a submission.
// What it can show: the bytes in the storage, whatever they are. For a render target that is the last picture the
// game or a write-back left there, where the image was zero before: a pass that reads a new target before anything
// drew into it sees that older picture instead of nothing.
#define SPLIT_FILL_MAX ((uint64_t)32 << 20)
static int split_mode = -1;
// since the last perf line: entries started by a copy from the released entry they replace, filled from storage (with
// the bytes and the time), filled again after a write; left empty for a reused image, for their size
static uint64_t n_split_copy, n_split_fill, n_split_bytes, n_split_ns, n_split_refill, n_split_reused, n_split_big;
static uint64_t split_total[2];   // copies and fills so far (vk_texture_split_counts)

static int split_fill_mode(void) {
    if (split_mode < 0) {
        int v = cfg_int("NX2NAVI_TEX_SPLIT_FILL");
        split_mode = v == 0 ? 0 : v == 2 ? 2 : 1;
        rt_log("vk: a texture the CPU does not write, made at storage the CPU writes, %s (NX2NAVI_TEX_SPLIT_FILL=%d)",
               !split_mode ? "starts empty" : split_mode == 2 ? "starts with the storage's bytes, once"
                                                             : "starts with the storage's bytes and follows them", split_mode);
    }
    return split_mode;
}

// A test's hook (tex_probe.c): the mode from here on, and the copies and fills made so far.
void vk_texture_split_fill_set(int mode) { split_mode = mode; }
void vk_texture_split_counts(uint64_t *copies, uint64_t *fills) { *copies = split_total[0], *fills = split_total[1]; }

// Whether split fill gives a new entry for t its storage's bytes, apart from the size (SPLIT_FILL_MAX).
static int split_wants(const tex_t *t, const TexVkFormat *f) {
    if (!split_fill_mode() || !t->mem || nvn_gpu_only((uintptr_t)t->mem) || tex_mirrored(t)) return 0;
    if (!(t->pool_flags & (NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_CPU_CACHED))) return 0;
    if (f->depth || f->stencil || t->d.samples > 1 || t->d.target == NVN_TEXTURE_TARGET_BUFFER) return 0;
    int bw, bh;
    return nvn_fmt_bpb(t->d.format, &bw, &bh) != 0;
}

// Whether e's image takes its content from the storage: a texture the CPU writes, or one split fill fills.
static int from_storage(const Tex *e) { return e->pub.mirrored || e->sfill == 1 || (e->sfill == 2 && !e->uploaded); }

// Whether an image with these requirements can lie in the pool memory at e's address (*sp: that memory).
static int pool_fits(VkCtx *c, const Tex *e, const VkMemoryRequirements *req, uint64_t guest_size, VkMemSpan *sp) {
    if ((e->f.depth || e->f.stencil) && req->size > guest_size) return 0;
    if (!req->size) return 0;   // an image the device could not lay out (image_size)
    if (!vk_mem_lookup(e->pub.addr, req->size, sp) || !sp->alloc || sp->alloc_offset % req->alignment) return 0;
    VmaAllocationInfo info;
    vmaGetAllocationInfo(c->vma, sp->alloc, &info);
    return req->memoryTypeBits >> info.memoryType & 1;
}

// The memory requirements of each parameter set (ImgKey) an image was made with; images made with the same parameters
// have the same requirements. With them create_tex makes no image that pool_fits would turn down: Ocarina's moving
// render targets mostly miss the device's alignment, and making the image only to read its requirements
// (vkGetImageMemoryRequirements does most of the driver's layout work) cost 5% of the replay in Kokiri Forest.
static nvn_hmap img_reqs = {.klen = sizeof(ImgKey)};   // ImgKey -> VkMemoryRequirements (malloc'd)
static uint64_t n_bind_skips;   // since the last perf line
#define DEDICATED_MIN ((VkDeviceSize)32 << 20)   // create_tex: images this large get memory of their own

static int bind_pool_memory(VkCtx *c, Tex *e, VkImage img, uint64_t guest_size) {
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(c->device, img, &req);
    if (!nvn_hm_get(&img_reqs, &e->ik)) {
        VkMemoryRequirements *q = malloc(sizeof *q);
        if (!q) rt_fatal("vk: out of memory");
        *q = req;
        nvn_hm_put(&img_reqs, &e->ik, q);
    }
    VkMemSpan sp;
    if (!pool_fits(c, e, &req, guest_size, &sp)) return 0;
    if (vmaBindImageMemory2(c->vma, sp.alloc, sp.alloc_offset, img, NULL) != VK_SUCCESS) return 0;
    e->bound_to = sp.alloc;
    return 1;
}

static void img_key_of(const VkImageCreateInfo *ci, ImgKey *k) {
    memset(k, 0, sizeof *k);
    k->flags = ci->flags, k->type = ci->imageType, k->format = ci->format, k->w = ci->extent.width;
    k->h = ci->extent.height, k->d = ci->extent.depth, k->levels = ci->mipLevels, k->layers = ci->arrayLayers;
    k->samples = ci->samples, k->usage = ci->usage;
}

// agl's BC4 encoder draws a BC texture's blocks through an RGBA16UI view, one texel per 8-byte block.
// Vulkan does it with an uncompressed
// view of a BLOCK_TEXEL_VIEW_COMPATIBLE image; EXTENDED_USAGE lets the BC image carry the COLOR_ATTACHMENT usage that
// only such views support. Whether the device takes that combination for this format and shape:
static int block_views_ok(const VkImageCreateInfo *ci) {
    VkPhysicalDeviceImageFormatInfo2 fi = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                                           .format = ci->format, .type = ci->imageType, .tiling = ci->tiling,
                                           .usage = ci->usage, .flags = ci->flags};
    VkImageFormatProperties2 p = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    if (vkGetPhysicalDeviceImageFormatProperties2(vk_ctx()->phys, &fi, &p) != VK_SUCCESS) return 0;
    const VkImageFormatProperties *q = &p.imageFormatProperties;
    return ci->extent.width <= q->maxExtent.width && ci->extent.height <= q->maxExtent.height &&
           ci->extent.depth <= q->maxExtent.depth && ci->mipLevels <= q->maxMipLevels &&
           ci->arrayLayers <= q->maxArrayLayers;
}

// The memory the device wants for an image made from ci: 0 when it cannot lay it out. RADV on GFX9 (Vega, Renoir)
// hands back a 0-byte image for a mipmapped block-compressed 3D image at least as deep as it is wide in blocks, with
// COLOR_ATTACHMENT usage and no STORAGE: that usage asks for a thin 3D layout (Mesa radv_image.c), for which the GFX9
// address library has no swizzle mode left (gfx9addrlib.cpp), and the error is dropped; VMA then refuses the 0-byte
// allocation. The format properties call it supported, so only the size shows it.
static VkDeviceSize image_size(const VkImageCreateInfo *ci) {
    VkDeviceImageMemoryRequirements q = {.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, .pCreateInfo = ci};
    VkMemoryRequirements2 r = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    vkGetDeviceImageMemoryRequirements(vk_ctx()->device, &q, &r);
    return r.memoryRequirements.size;
}

// UNDEFINED -> GENERAL; new depth / stencil images start cleared (LAS textures.c: the game copies depth
// without stencil, and the stencil of those images must not be whatever the memory held).
// A reused image (create_tex) was another texture's: the transition waits for every earlier command, which may still
// read or write it.
static void init_image(Tex *e, VkCommandBuffer cb) {
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                .srcStageMask = e->reused ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_NONE,
                                .srcAccessMask = e->reused ? VK_ACCESS_2_MEMORY_WRITE_BIT : 0,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = e->pub.image, .subresourceRange = {e->pub.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
    if (e->f.depth || e->f.stencil) {
        VkClearDepthStencilValue zero = {0.0f, 0};
        VkImageSubresourceRange rg = {e->pub.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearDepthStencilImage(cb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
        barrier(cb, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    } else if (vk_fix_zero() && !e->bound_to && !e->pub.mirrored && !format_compressed(e->f.vk)) {
        // GPU-only storage that got memory of its own starts at zero, as the pool it stands for (vk_memory.c
        // alloc_physical). Images aliasing pool memory are not cleared: that would erase what other textures rendered
        // there; mirrored images are uploaded from their storage on first use.
        VkClearColorValue zero = {0};
        VkImageSubresourceRange rg = {e->pub.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearColorImage(cb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
        barrier(cb, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    }
}

// NX2NAVI_DEPTH_INIT_INLINE (default 1; 0: in the upload batch, as before): a new depth or stencil image goes through
// init_image at the replay's position too. Its clear writes the image's memory, and an image bound into pool memory
// (bind_pool_memory) shares that with the live texture at its address; from the upload batch, which runs before the
// whole part being recorded (vk_sync.c split_part), the clear can zero what earlier commands of the part still read as
// that texture. Ocarina makes a new depth descriptor over a live one about 40 times a second at the castle town gate
// (a 1280x720 DEPTH32F over a 640x360 one, its sun shadow maps as they resize), and for one frame a few times a minute
// the shadowing went fully dark. At the replay's position vk_exec_inline_begin's barrier orders the clear after them.
static int depth_init_inline(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("DEPTH_INIT_INLINE", "1") != 0;
    return on;
}

// A new entry's first transition. A reused image goes through it at the replay's current position: earlier commands
// of the part being recorded may still use the image as the texture it was, and the upload batch runs before the part.
// So does a new depth or stencil image (depth_init_inline).
static void init_new(Tex *e) {
    int ds = !e->reused && (e->f.depth || e->f.stencil) && depth_init_inline();
    if (e->reused || ds) {
        int reopen;
        VkCommandBuffer cb = vk_exec_inline_begin(&reopen);
        if (cb) {
            init_image(e, cb);
            vk_exec_inline_end(reopen);
            if (ds) n_dinit_inline++, n_dinit_inpass += reopen != 0;
            return;
        }
    }
    if (ds) n_dinit_upload++;
    init_image(e, vk_upload_begin());
    vk_upload_end();
}

// NX2NAVI_SHADOW_SCALE (1 to 4): the game's shadow maps are made this many times larger on each side. NX2navi renders its
// sun cascades into a 960x960 DEPTH16 2D array of 4 layers and its static shadows into 1440x1440 and 720x720 arrays of 2,
// whatever the output resolution, and shrinks the cascades under load (872, 848, 800, 480 and so on). Those are made
// larger here: render passes into them scale the game's viewports and scissors (vk_exec.c resolve_rt, pass_scale),
// copies scale their regions (vk_exec_xfer.c sub_resolve), and shaders sample them with normalised coordinates, so the
// game sees nothing of it. A map whose storage the CPU writes (mirrored) or that aliases pool memory keeps its size,
// as its texels must match the guest layout.
static int shadow_scale(void) {
    static int scale = -1;
    if (scale < 0) {
        int s = cfg_int("NX2NAVI_SHADOW_SCALE");
        s = s < 1 ? 1 : s > 4 ? 4 : s;
        rt_log("vk: shadow map scale %d", s);
        scale = s;
    }
    return scale;
}

static int shadow_scale_of(const nvn_texdesc *d, const TexVkFormat *f, uint32_t samples, int mirrored) {
    int scale = shadow_scale();
    if (scale == 1 || mirrored || samples > 1 || d->format != NVN_FORMAT_DEPTH16 || !f->depth) return 1;
    if (d->target != NVN_TEXTURE_TARGET_2D_ARRAY || d->width != d->height || d->width < 256 || d->depth < 2 || d->depth > 4)
        return 1;
    uint32_t most = vk_ctx()->props.limits.maxImageDimension2D;
    return d->width * (uint32_t)scale <= most ? scale : 1;
}

// ---- ASTC as BC1 (NX2NAVI_ASTC_BC1, vk_astc.c)
// A texture the CPU provides whose ASTC blocks all decode opaque is held as BC1, not BC3: the encoder then writes only
// the colour half of the BC3 blocks it would have written (bc3_encode.comp), which BC1 decodes to the same texels, in
// half the memory. Every ASTC texture is made BC3 (create_tex); its first upload checks its blocks inside the copy into
// staging it makes anyway (astc_copy_opaque, tex_astc.c astc_blocks_opaque), and when all are opaque the entry gets a
// BC1 image in place of the BC3 one before the decode (astc_reformat). A check of its own where the texture is made
// cost 1.7 s of the render thread over a seven-minute teleport tour; the image made again costs a fraction of that.
// The check covers every 16-byte block of the storage that is copied, the padding between levels and layers included
// (zeros there decode as error blocks, opaque magenta; other bytes can only make it stricter). Each later upload into a
// BC1 image checks again: the game may load other content into the same storage, and content with alpha gets a BC3
// image back before it is decoded. Blocks of the endpoint modes with alpha are decoded to tell; past
// ASTC_OPAQUE_DECODES of them in one texture it counts as not opaque, as the check would cost more than it saves.
// Blocks the CPU untiles first (not astc_bl_regions) are checked in the storage before the copy (astc_opaque).
#define ASTC_OPAQUE_DECODES 4096
static uint64_t n_bc1_made, n_bc1_made_bytes, n_bc1_widened, n_bc1_checks, n_bc1_check_ns, n_bc1_check_bytes;

// 1 when every block of t's storage at addr decodes opaque; 0 when one does not or the storage is not mapped.
static int astc_opaque(const tex_t *t, uint64_t addr, const TexVkFormat *f) {
    uint64_t t0 = kern_now_ns(), n = t->L.size / 16;
    if (!t->mem || !n || !mem_read_hold(addr, t->L.size)) return 0;
    uint32_t decoded = 0;
    int ok = astc_blocks_opaque_mt((const uint8_t *)t->mem, n, f->astc_bw, f->astc_bh, f->astc_srgb != 0,
                                   ASTC_OPAQUE_DECODES, &decoded, NULL);
    mem_read_release();
    n_bc1_checks++, n_bc1_check_ns += kern_now_ns() - t0, n_bc1_check_bytes += t->L.size;
    return ok;
}

// ---- texture detail (NX2NAVI_TEXTURE_DETAIL = auto | full | reduced | low, texture_detail in nx2navi.ini; read at
// the first texture, so a change waits for a restart)
// The material textures of a loaded area are what fills a small card, and three quarters of a mipmapped texture is its
// largest level. Over the budget Windows moves device memory into system memory and the frame rate falls to a
// fraction (a tester's RTX 3060 Laptop, 6 GiB with a budget of 4,730 MiB, ran the world at 5 to 15 fps).
//
// Measured on an RTX 5090 with the budget taken as those 4,730 MiB (NX2NAVI_VRAM_BUDGET_MB), a walk through Castle
// Town at 1440p with DLSS Balanced, device memory in use at its peak and the textures in it:
//   full     6,907 MiB, textures 4,302 MiB (2,852 MiB of them ASTC as BC3), 2,589 M ASTC texels decoded
//   reduced  5,472 MiB, textures 2,357 MiB (908 MiB), 826 M texels
//   low      4,987 MiB, textures 1,997 MiB (548 MiB), 503 M texels
// About 1.1 GiB of each is outside the textures and buffers (DLSS alone reports 0.7 GiB). What a level does not touch
// stays: about 0.8 GiB of BC4, BC5 and BC7 textures the game ships in those formats, 0.65 GiB of render targets and
// 0.9 GiB of buffer mirrors.
//
// reduced makes such a texture without its largest level, low without its two largest when it is 1024x1024 or more
// (detail_skip decides). The image then holds the game's levels from VkTexImage.skip on: image level i is the game's
// level i + skip, at that level's own size, so a level and a texel rectangle inside it mean the same in both and only
// the level index moves. Every path from a game level to the image subtracts skip: the upload and its ASTC regions
// (upload, astc_bl_regions), WriteTexels and the write-back (image_texels, write_back_n), the views (vk_exec_draw.c
// get_view) and the sampler's LOD clamp (vk_sampler_detail_by_id_locked). Shaders sample with normalised coordinates
// and derivatives, which pick the same game level as before wherever it is kept.
//
// Only a texture for which that holds is reduced: an ASTC texture this runtime decodes (not one the device samples as
// ASTC, whose copies run on the image, vk_exec_xfer.c sub_resolve), a 2D texture or 2D array, block-linear, one
// sample, mipmapped, DETAIL_MIN texels or more each way, whose content the CPU provides (tex_mirrored). That leaves
// out render targets and storage images (no ASTC format can be either), 3D and cube textures, the small and unmipped
// ones (interface art, glyph atlases, lookup tables, ramps) and whatever the GPU copies, as texture copies of a
// decoded ASTC texture are not replayed at all. Two more are left whole:
//  - a texture made over a live one that holds GPU-written content, which start_at copies in texel for texel;
//  - a texture a shader reads by texel position or whose size it asks for (exact, below): a fetch at a level the image
//    lacks, or coordinates worked out from the size of another texture, would read the wrong texels. The translated
//    shaders do that only through fixed bindings (gfx_shader.c reflect_exact; none of 44,081 prepared shaders of game
//    1.1.1 does it through the texture pool), so the replay sees the texture at the binding and asks for it whole
//    (vk_texture_exact_by_id_locked). The key then stays whole for the session.
// The size of a 2D array is also asked for by the layer clamp the translator puts before array samples; that reads
// the layer count alone, which no level changes, and does not count (reflect_exact).
//
// auto: full from DETAIL_FULL_MB of device-local budget on, low under DETAIL_LOW_MB, reduced between. Textures give
// way from 80% of the budget on (sweep_pressure), so a level wants its use at or under that: full, 6.9 to 7.5 GiB in
// the town and in Hyrule Field, on cards of 10 GiB and more (their budgets start near 9 GiB; an 8 GiB card's is about
// 7.2 GiB); reduced, 5.5 GiB, on 8 GiB cards; and every 6 GiB card, whose budget stays under 5.5 GiB, gets low.
#define DETAIL_MIN 256
#define DETAIL_LOW_MIN 1024
#define DETAIL_FULL_MB 8192
#define DETAIL_LOW_MB 6144
// The levels above assume the ASTC textures end as BC3 (or stay ASTC), a byte a texel. Where they stay RGBA8 (astc =
// cpu, a card without the decoder, astc_bc = 0) they take four times that, which is what one level left out gives
// back: under this budget auto then goes one step lower. A card with 8 GiB that decoded on the CPU held two texture
// arrays of 852 MiB each, was 560 MiB over its budget and lost its device.
#define DETAIL_RGBA8_MB 20480
#define EXACT_MAX 65536
static nvn_hmap exact = {.klen = sizeof(TexKey)};   // keys made whole whatever the setting; value 1
static uint64_t n_exact;                             // textures made whole again so far

static int alias_has_gpu_content(const struct Tex *src);

static int64_t detail_budget_mb = -1;   // the device-local budget auto went by, in MiB; 0: the device reports none

int vk_texture_detail_auto(void) {
    int64_t mb = __atomic_load_n(&detail_budget_mb, __ATOMIC_RELAXED);
    if (mb < 0 && !vk_ctx()) return 0;   // asked before there is a device: nothing is kept
    if (mb < 0) {   // read once: the budget moves with what else uses the card, the choice must not
        uint64_t used = 0, budget = 0;
        int exact_budget = 0;
        mb = vk_mem_vram(&used, &budget, &exact_budget) ? (int64_t)(budget >> 20) : 0;
        __atomic_store_n(&detail_budget_mb, mb, __ATOMIC_RELAXED);
    }
    int most = !mb || mb >= DETAIL_FULL_MB ? 0 : mb >= DETAIL_LOW_MB ? 1 : 2;
    if (mb && mb < DETAIL_RGBA8_MB && most < 2 && vk_astc_mode() != VK_ASTC_NATIVE && !vk_astc_bc()) most++;
    return most;
}

static const char *const detail_names[3] = {"full", "reduced", "low"};
static int detail_chosen = -1;   // detail_most's answer once it is made

// The texture detail in effect, for a fatal error's summary line. No lock, any thread.
const char *vk_texture_detail_name(void) {
    int most = __atomic_load_n(&detail_chosen, __ATOMIC_RELAXED);
    return most < 0 ? "not chosen yet (no texture made)" : detail_names[most];
}

static int detail_most(void) {   // the most levels a texture is made without: 0 full, 1 reduced, 2 low
    static int most = -1;
    if (most >= 0) return most;
    const char *v = cfg_value("NX2NAVI_TEXTURE_DETAIL");
    most = !strcmp(v, "full") ? 0 : !strcmp(v, "reduced") ? 1 : !strcmp(v, "low") ? 2 : -1;
    if (most >= 0) {
        rt_log("vk: texture detail: %s (texture_detail)", v);
    } else {
        most = vk_texture_detail_auto();
        if (!detail_budget_mb)
            rt_log("vk: texture detail: full (texture_detail = %s; the device reports no video memory budget)", v);
        else
            rt_log("vk: texture detail: %s, chosen for a video memory budget of %lld MiB (texture_detail = %s; full from "
                   "%d MiB, low under %d MiB%s)", detail_names[most],(long long)detail_budget_mb, v, DETAIL_FULL_MB, DETAIL_LOW_MB,
                   vk_astc_mode() != VK_ASTC_NATIVE && !vk_astc_bc() && detail_budget_mb < DETAIL_RGBA8_MB
                       ? "; one step lower because ASTC textures stay uncompressed here" : "");
    }
    __atomic_store_n(&detail_chosen, most, __ATOMIC_RELAXED);
    return most;
}

// The top mip levels the image of t is made without (the rule above). w, h, levels: the image as it would be whole.
static uint32_t detail_skip(const tex_t *t, const TexKey *k, const TexVkFormat *f, uint32_t w, uint32_t h, uint32_t levels,
                            uint32_t samples) {
    int most = detail_most();
    const nvn_texdesc *d = &t->d;
    if (!most || !f->astc_bw || samples > 1 || levels < 2 || w < DETAIL_MIN || h < DETAIL_MIN) return 0;
    if ((d->target != NVN_TEXTURE_TARGET_2D && d->target != NVN_TEXTURE_TARGET_2D_ARRAY) || nvn_tex_is_linear(d)) return 0;
    if (!tex_mirrored(t) || (exact.n && nvn_hm_get(&exact, k))) return 0;
    for (const struct Tex *p = nvn_hm_get(&by_addr, &k->addr); p; p = p->next_at)
        if (!p->released && alias_has_gpu_content(p)) return 0;
    return most >= 2 && levels >= 4 && w >= DETAIL_LOW_MIN && h >= DETAIL_LOW_MIN ? 2 : 1;
}

// A texture whose content the CPU provides (tex_mirrored), up to SUBALLOC_MAX, takes memory from VMA's device blocks:
// the driver prefers a dedicated allocation for most images, and vmaCreateImage follows that, so a scene load
// (about 2700 new textures in one frame entering Kokiri Forest) made as many vkAllocateMemory calls, 43% of that
// 680 ms frame. vmaAllocateMemory with the plain requirements sub-allocates instead, from the blocks the headroom
// thread keeps ahead (vk_headroom.c). Render targets keep vmaCreateImage and the driver's preference (vma_image).
// VK_NULL_HANDLE when the image does not take that memory.
enum { SUBALLOC_MAX = 32 << 20 };
static VkImage suballoc_image(VkCtx *c, const VkImageCreateInfo *ci, VmaAllocation *alloc) {
    VkImage img;
    if (vkCreateImage(c->device, ci, NULL, &img) != VK_SUCCESS) return VK_NULL_HANDLE;
    // an image the driver requires (not only prefers) dedicated memory for keeps it: vma_image
    VkMemoryDedicatedRequirements dr = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 mr2 = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dr};
    VkImageMemoryRequirementsInfo2 ri = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = img};
    vkGetImageMemoryRequirements2(c->device, &ri, &mr2);
    VkMemoryRequirements mr = mr2.memoryRequirements;
    VmaAllocationCreateInfo ai = {.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    int ok = !dr.requiresDedicatedAllocation && mr.size <= SUBALLOC_MAX &&
             vmaAllocateMemory(c->vma, &mr, &ai, alloc, NULL) == VK_SUCCESS;
    if (ok && vmaBindImageMemory(c->vma, *alloc, img) != VK_SUCCESS) {
        vmaFreeMemory(c->vma, *alloc);
        ok = 0;
    }
    if (!ok) {
        vkDestroyImage(c->device, img, NULL);
        img = VK_NULL_HANDLE, *alloc = NULL;
    }
    return img;
}

// An image made with vmaCreateImage. One of DEDICATED_MIN or more gets memory of its own: freed inside one of VMA's
// 256 MiB blocks it left a hole the driver went on counting, with the rest of the block, against the budget and the
// system's commit (0.5 to 1.8 GiB of such free space in a session at 5120x2160), until the block emptied; its own
// memory goes back when it does.
static VkResult vma_image(VkCtx *c, const VkImageCreateInfo *ci, const ImgKey *ik, VkImage *img, VmaAllocation *alloc) {
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    const VkMemoryRequirements *known = nvn_hm_get(&img_reqs, ik);
    if ((known ? known->size : image_size(ci)) >= DEDICATED_MIN) ai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    return vmaCreateImage(c->vma, ci, &ai, img, alloc, NULL);
}

// defer_init: the caller records the image's first transition (init_image or alias_copy).
static Tex *create_tex(const tex_t *t, const TexKey *k, int defer_init) {
    static volatile int32_t n_unmapped, n_unsupported, n_failed, n_unbound, n_thick;
    VkCtx *c = vk_ctx();
    const nvn_texdesc *d = &t->d;
    if (!c || d->target == NVN_TEXTURE_TARGET_BUFFER) return NULL;
    TexVkFormat f;
    if (!tex_vk_format(d->format, &f)) {
        log_limited(&n_unmapped, "texture format %#x has no Vulkan format: %s", d->format, f.reason);
        return NULL;
    }
    if (!tex_vk_format_supported(&f)) {
        log_limited(&n_unsupported, "texture format %#x: VkFormat %d lacks features %#x", d->format, (int)f.vk, (unsigned)f.need);
        return NULL;
    }
    // ASTC as BC1: a texture the CPU provides, not one that lies in pool memory, may turn BC1 at its first upload
    int bc1_try = f.astc_bc == 1 && tex_mirrored(t) && !nvn_gpu_only(k->addr) && vk_astc_bc1();
    nvn_texlayout L;
    nvn_tex_layout(d, &L);
    uint32_t w = umax(d->width, 1), h = umax(d->height, 1), depth = 1, layers = 1;
    VkImageType type = VK_IMAGE_TYPE_2D;
    switch (d->target) {
    case NVN_TEXTURE_TARGET_1D: type = VK_IMAGE_TYPE_1D; h = 1; break;
    case NVN_TEXTURE_TARGET_1D_ARRAY: type = VK_IMAGE_TYPE_1D; h = 1; layers = L.layers; break;
    case NVN_TEXTURE_TARGET_3D: type = VK_IMAGE_TYPE_3D; depth = umax(d->depth, 1); break;
    case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY:
    case NVN_TEXTURE_TARGET_CUBEMAP: case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: layers = L.layers; break;
    default: break;
    }
    int scale = shadow_scale_of(d, &f, d->samples > 1 ? (uint32_t)d->samples : 1, tex_mirrored(t));
    w *= (uint32_t)scale, h *= (uint32_t)scale;
    uint32_t levels = umax(d->levels, 1), most = 1;
    for (uint32_t s = umax(umax(w, h), depth); s > 1; s >>= 1) most++;
    if (levels > most) levels = most;
    uint32_t samples = d->samples > 1 ? (uint32_t)d->samples : 1;
    if (samples > 1) levels = 1;
    uint32_t skip = detail_skip(t, k, &f, w, h, levels, samples);   // texture detail: the image starts at this level
    if (skip) w = mip(w, skip), h = mip(h, skip), levels -= skip;
    VkFormatFeatureFlags ff = features_of(f.vk);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    int ds = f.depth || f.stencil;
    if (ds && (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (!ds && (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if ((d->flags & NVN_TEXTURE_FLAGS_IMAGE) && (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && samples == 1)
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    // NVN2 titles (API 200+) number the texture flags differently: Ocarina's compute targets (the title movie's RGBA8
    // frames) carry 0x1, NVN1's DISPLAY. Every single-sample colour texture whose format allows it can be a storage
    // image there, so a compute write into one is never dropped.
    if (rt_nvn_api_version[0] >= 200 && !ds && (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && samples == 1)
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    // contact shadows (vk_contact.c) darken the game's B10G11R11 scene colour in place from a compute shader
    if (f.vk == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && samples == 1 &&
        (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && w >= 640 && vk_contact_strength())
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageCreateFlags flags = ds ? 0 : VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;   // format-reinterpreting views
    // Cube compatible only for cube targets: the replay makes cube views of nothing else (vk_exec_draw.c get_view checks
    // the image's own view type). The flag on every square array of 6+ layers changed how AMD's Windows driver lays such
    // an image out, and terrain material arrays sampled through it came out in rainbow blocks that moved with the mip
    // level (seen on an RX 580 and a 7800 XT).
    if (type == VK_IMAGE_TYPE_2D && layers >= 6 && w == h && samples == 1 &&
        (d->target == NVN_TEXTURE_TARGET_CUBEMAP || d->target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY))
        flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    // NVN renders into a 3D texture slice by slice (a view's min layer is the depth plane, nvn_mtl.m), e.g. the
    // grading LUT of the menu background blur. Vulkan needs a 2D view of the slice for that.
    if (type == VK_IMAGE_TYPE_3D && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags, .imageType = type, .format = f.vk,
                            .extent = {w, h, depth}, .mipLevels = levels, .arrayLayers = layers,
                            .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    int bw, bh, block_views = 0;
    // NX2NAVI_BLOCK_VIEWS_3D (default 0): a 3D BC image also gets block views. On RDNA2 cards (driver 26.10.1, issue #5)
    // the 32x32x32 BC4 volume the game creates at the logo, made BLOCK_TEXEL_VIEW_COMPATIBLE and 2D_ARRAY_COMPATIBLE,
    // took a GPU write just past the end of its memory: device lost at the first submit after it when the image ended
    // its memory block. Draws into a 3D BC image go through a stand-in instead (vk_exec.c bc_target).
    static int bv3d = -1;
    if (bv3d < 0) {
        bv3d = rt_knob("BLOCK_VIEWS_3D", "0");
        rt_log("vk: 3D BC textures %s (NX2NAVI_BLOCK_VIEWS_3D=%d)", bv3d ? "get block views (renderable)" : "have no block views",
               bv3d);
    }
    if (!ds && samples == 1 && f.vk == f.ideal && nvn_fmt_bpb(d->format, &bw, &bh) && bw == 4 && bh == 4 &&
        (type != VK_IMAGE_TYPE_3D || bv3d)) {
        VkImageCreateInfo bc = ci;   // BC formats: renderable through views of their blocks (block_views_ok)
        bc.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        bc.flags |= VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        if (type == VK_IMAGE_TYPE_3D) bc.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        VkImageUsageFlags hidden = 0;   // creation usage the replay never uses (views restrict theirs, vk_exec.c)
        int ok = block_views_ok(&bc);
        if (ok && type == VK_IMAGE_TYPE_3D && !image_size(&bc)) {
            // STORAGE usage keeps RADV on GFX9 from the thin layout (image_size), so agl's BC4 encoder still renders
            // into the volume; else the image is made without block views and its encoder passes are left out
            bc.usage |= VK_IMAGE_USAGE_STORAGE_BIT, hidden = VK_IMAGE_USAGE_STORAGE_BIT;
            ok = block_views_ok(&bc) && image_size(&bc);
            log_limited(&n_thick, "texture fmt %#x %ux%ux%u: the device cannot lay out a 3D image with block views; %s",
                        d->format, w, h, depth, ok ? "made with storage usage (a thick layout)" : "made without them");
        }
        if (ok) ci = bc, usage = bc.usage & ~hidden, flags = bc.flags, block_views = 1;
    }
    // Feedback loops (vk.h VkCtx.feedback): every attachable image may be sampled by a draw of a pass that renders it,
    // so it gets the usage now; adding it when a loop shows up would mean recreating the image (that lost the
    // grass tile maps once).
    int fb_usage = c->feedback == VK_FEEDBACK_LAYOUT && !block_views &&
                   (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));
    if (fb_usage) ci.usage = usage |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    Tex *e = calloc(1, sizeof *e);
    if (!e) rt_fatal("vk: out of memory for a texture");
    e->key = *k;
    e->t = *t;
    e->f = f;
    e->pub.addr = k->addr;
    img_key_of(&ci, &e->ik);
    VkImage img = VK_NULL_HANDLE;
    if (scale == 1 && nvn_gpu_only(k->addr)) {   // storage in a physical or virtual pool: alias the pool memory (section 3.2)
        const VkMemoryRequirements *q = nvn_hm_get(&img_reqs, &e->ik);
        VkMemSpan sp;
        if (q && !pool_fits(c, e, q, L.size, &sp)) {   // it would not bind there (img_reqs)
            n_bind_skips++;
            log_limited(&n_unbound, "texture %#llx (%ux%u fmt %#x) cannot alias its pool memory; own memory instead",
                        (unsigned long long)k->addr, w, h, d->format);
        } else if (vkCreateImage(c->device, &ci, NULL, &img) == VK_SUCCESS && !bind_pool_memory(c, e, img, L.size)) {
            vkDestroyImage(c->device, img, NULL);
            img = VK_NULL_HANDLE;
            log_limited(&n_unbound, "texture %#llx (%ux%u fmt %#x) cannot alias its pool memory; own memory instead",
                        (unsigned long long)k->addr, w, h, d->format);
        }
    }
    if (!img && tex_mirrored(t) && samples == 1) img = suballoc_image(c, &ci, &e->alloc);
    int gpu_only = !tex_mirrored(t);
    // an entry split fill is to fill gets a new image: start_at uploads into it in the upload batch (see "split fill")
    int to_fill = gpu_only && split_wants(t, &f) && L.size <= SPLIT_FILL_MAX;
    for (const Tex *p = to_fill ? nvn_hm_get(&by_addr, &k->addr) : NULL; p; p = p->next_at)
        if (alias_has_gpu_content(p)) { to_fill = 0; break; }   // start_at copies that in instead
    if (!img && gpu_only && !to_fill && tex_reuse_on()) {   // the image of a released entry with these parameters (reuse, above)
        // Not one released in this frame or the one before: Ocarina releases its render targets and registers them
        // again every frame, and DLSS's scene target (2560x1440 at a fixed address in Kokiri Forest) lost its image
        // to a moving target of the same size in between, and DLSS its history (vk_dlaa.c evaluate), several times a
        // second. Not one at this address either: its own descriptor comes back for it (start_at). Of the others the
        // one nearest in address, so a target the allocator moves keeps the image it had; an overwritten one first
        // (overwrite_others): nothing can ask for its content again.
        uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        Tex *a = NULL, *kept = NULL;
        uint64_t best = UINT64_MAX, best_kept = UINT64_MAX;
        int n = 0;
        for (Tex *q = nvn_hm_get(&reuse, &e->ik); q && n < 128; q = q->ru_next, n++) {
            if (q->rel_epoch == submit_epoch || now - q->rel_frame < 2 || q->key.addr == k->addr) continue;
            uint64_t dist = q->key.addr > k->addr ? q->key.addr - k->addr : k->addr - q->key.addr;
            if (q->overwritten) {
                if (dist < best) best = dist, a = q;
            } else if (dist < best_kept) best_kept = dist, kept = q;
        }
        if (!a) a = kept;
        if (a) {
            img = a->pub.image, e->alloc = a->alloc;
            vk_exec_image_gone(img);   // the views and records of the texture it was
            a->pub.image = VK_NULL_HANDLE, a->alloc = NULL;
            drop_tex(a, DROP_REUSED);
            e->reused = 1, e->og_reimg = 1;
            n_reused++;
        }
    }
    if (!img) {
        if (gpu_only) n_reuse_new++;
        VkResult r = vma_image(c, &ci, &e->ik, &img, &e->alloc);
        if (r != VK_SUCCESS) {
            log_limited(&n_failed, "image creation failed (%d): nvn fmt %#x %ux%ux%u target %d levels %u layers %u", (int)r,
                        d->format, w, h, depth, d->target, levels, layers);
            free(e);
            return NULL;
        }
    }
    if (e->alloc) {
        VmaAllocationInfo own;
        vmaGetAllocationInfo(c->vma, e->alloc, &own);
        e->bytes = own.size;
    }
    e->bc1_try = (uint8_t)bc1_try;
    e->reusable = e->alloc && gpu_only;
    e->gw_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (e->reusable && nvn_gpu_only(k->addr)) gwin_add(e);
    // A new image's handle can be one a destroyed image had. Views are purged with the image (destroy_tex), so none
    // should be cached under it: one that is was made from a destroyed texture's record after its purge, and would
    // reach this image's memory through the dead one's. Dropped here, and counted in the log.
    if (!e->reused) {
        static volatile int32_t n_stale;
        int stale = vk_exec_image_gone(img);
        if (stale)
            log_limited(&n_stale, "new image %#llx had %d cached views of a destroyed image with its handle; dropped",
                        (unsigned long long)(uintptr_t)img, stale);
    }
    VkTexImage *p = &e->pub;
    p->image = img;
    p->format = f.vk;
    p->type = type;
    p->usage = usage;
    p->aspect = aspect_of(&f);
    p->width = w, p->height = h, p->depth = depth, p->levels = levels, p->layers = layers, p->samples = samples;
    p->view_type = type == VK_IMAGE_TYPE_1D ? (d->target == NVN_TEXTURE_TARGET_1D_ARRAY ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D)
                 : type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                 : d->target == NVN_TEXTURE_TARGET_CUBEMAP && (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ? VK_IMAGE_VIEW_TYPE_CUBE
                 : d->target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY && (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
                 : layers > 1 || d->target == NVN_TEXTURE_TARGET_2D_ARRAY || d->target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY
                     ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    p->alpha_one = f.alpha_one;
    memcpy(p->chan, f.chan, sizeof p->chan);
    p->swz[0] = (uint8_t)d->swizzle_r, p->swz[1] = (uint8_t)d->swizzle_g;
    p->swz[2] = (uint8_t)d->swizzle_b, p->swz[3] = (uint8_t)d->swizzle_a;
    if (!(p->swz[0] | p->swz[1] | p->swz[2] | p->swz[3]))   // descriptors built without nvnTextureBuilderSetDefaults
        p->swz[0] = NVN_TEXTURE_SWIZZLE_R, p->swz[1] = NVN_TEXTURE_SWIZZLE_G, p->swz[2] = NVN_TEXTURE_SWIZZLE_B,
        p->swz[3] = NVN_TEXTURE_SWIZZLE_A;
    p->mirrored = (uint8_t)tex_mirrored(t);
    p->bound = e->bound_to != NULL;
    p->block_views = (uint8_t)block_views;
    p->fb_usage = (uint8_t)fb_usage;
    p->scale = (uint8_t)scale;
    p->skip = (uint8_t)skip;

    if (!defer_init) init_new(e);
    return e;
}

static int owned_lower(uint64_t a) {
    int lo = 0, hi = nowned;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (owned[m]->pub.addr < a) lo = m + 1;
        else hi = m;
    }
    return lo;
}

static void own_add(Tex *e) {
    if (e->listed) return;
    plat_mutex_lock(&owned_lock);
    if (nowned == cowned) {
        cowned = cowned ? 2 * cowned : 256;
        if (!(owned = realloc(owned, sizeof *owned * (size_t)cowned))) rt_fatal("vk: out of memory for the owned-texture list");
    }
    int i = owned_lower(e->pub.addr);
    memmove(owned + i + 1, owned + i, sizeof *owned * (size_t)(nowned - i));
    owned[i] = e;
    __atomic_store_n(&nowned, nowned + 1, __ATOMIC_RELEASE);
    if (e->t.L.size > max_owned) max_owned = e->t.L.size;
    e->listed = 1;
    plat_mutex_unlock(&owned_lock);
}

static void own_remove(Tex *e) {
    if (!e->listed) return;
    plat_mutex_lock(&owned_lock);
    for (int i = owned_lower(e->pub.addr); i < nowned && owned[i]->pub.addr == e->pub.addr; i++)
        if (owned[i] == e) {
            memmove(owned + i, owned + i + 1, sizeof *owned * (size_t)(nowned - i - 1));
            __atomic_store_n(&nowned, nowned - 1, __ATOMIC_RELEASE);
            break;
        }
    e->listed = 0;
    plat_mutex_unlock(&owned_lock);
}

static uint64_t rel_bytes;   // own memory of the released entries (Tex.bytes)

static void ru_link(Tex *e) {
    Tex *head = nvn_hm_get(&reuse, &e->ik);
    e->ru_prev = NULL, e->ru_next = head;
    if (head) head->ru_prev = e;
    nvn_hm_put(&reuse, &e->ik, e);
    e->ru_on = 1;
}

static void ru_unlink(Tex *e) {
    if (!e->ru_on) return;
    if (e->ru_prev) e->ru_prev->ru_next = e->ru_next;
    else if (e->ru_next) nvn_hm_put(&reuse, &e->ik, e->ru_next);
    else nvn_hm_del(&reuse, &e->ik);
    if (e->ru_next) e->ru_next->ru_prev = e->ru_prev;
    e->ru_prev = e->ru_next = NULL;
    e->ru_on = 0;
}

static void rel_append(Tex *e) {
    e->rel_next = NULL;
    e->rel_prev = rel_tail;
    if (rel_tail) rel_tail->rel_next = e;
    else rel_head = e;
    rel_tail = e;
    rel_bytes += e->bytes;
    if (e->reusable && tex_reuse_on()) ru_link(e);
}

static Tex *sweep_at;   // where sweep_released goes on
static void rel_unlink(Tex *e) {
    if (sweep_at == e) sweep_at = e->rel_next;
    ru_unlink(e);
    rel_bytes -= e->bytes;
    if (e->rel_prev) e->rel_prev->rel_next = e->rel_next;
    else rel_head = e->rel_next;
    if (e->rel_next) e->rel_next->rel_prev = e->rel_prev;
    else rel_tail = e->rel_prev;
    e->rel_prev = e->rel_next = NULL;
}

// The entries the bindless texture pool's slots resolved to, for a submission's freshness check (make_fresh_snap once
// per entry), which looked at every slot instead: 33,000 slots, 1.5 ms of the replay thread a submission in Kokiri
// Forest. vk_exec_draw.c pool_fill notes a slot's entry after it resolved the slot; only entries whose content the CPU
// provides are listed (the check does nothing for the others; one the GPU took over, gpu_owned, never uploads again
// and leaves), and an entry stays listed until it is destroyed: the list holds every entry a slot resolves to, and
// some that no slot does any more. Each keeps its storage range and the page generation of its last check here, so
// that the check of an entry whose 2 MiB regions took no write since (vk_mem_unchanged_locked, one shared lock for the
// whole list) touches neither the entry nor the lock: 5,600 entries took 0.77 ms a submission one by one. Texture lock
// held.
typedef struct { Tex *e; uint64_t addr, size; uint32_t since; int ok; } PEnt;   // ok: uploaded as of since
static PEnt *pent;
static int npent, cpent, *pq, cpq;
static uint64_t n_pool_checks, n_pool_skips;
// The region check's misses (vk_mem_unchanged_why_locked): not uploaded, a region without this epoch's snapshot yet, one
// untracked, one written since, nothing to check; and the snapshot queries its own pass made (NX2NAVI_POOL_PRESNAP).
static uint64_t n_pool_miss[5], n_presnap_q;
static void make_fresh_snap(Tex *e);

static void pool_unlist(Tex *e) {
    if (!e->pidx) return;
    int i = e->pidx - 1;
    pent[i] = pent[--npent];
    pent[i].e->pidx = i + 1;
    e->pidx = 0;
}

// The entries the pool's slots hold: for each, how many slots' last fill resolved to it (vk_exec_draw.c pool_hold; a fill
// that shared another slot's lookup counts for the entry that lookup found). The walk over every slot looked each slot
// up in each submission, which left its entry's use stamp (touch): the idle clock reads it, and start_at's alias rule
// reads the frame of an entry with GPU content. The check by list is no lookup, so it leaves the stamps from these
// counts (pool_touch): in each submission for the GPU-owned entries, in turn for the others, each once in
// POOL_TOUCH_DIV submissions (IDLE_ROUNDS is 240 of them). An entry keeps its place until it is destroyed and no slot
// holds it any more: a destroyed one is refilled out of its slots by the bump of its bucket.
#define POOL_TOUCH_DIV 64
typedef struct { Tex *e; uint32_t refs; int next; uint8_t gown; } PRef;   // e NULL: destroyed, or free (refs 0, next)
static PRef *pref;
static int npref, cpref, pref_free;   // pref_free: 1 + the first free place
static void touch(Tex *e);

static uint32_t pool_ref(Tex *e) {
    if (e->ph) return (uint32_t)e->ph;
    int i;
    if (pref_free) {
        i = pref_free - 1, pref_free = pref[i].next;
    } else {
        if (npref == cpref) {
            int n = cpref ? cpref * 2 : 1024;
            PRef *q = realloc(pref, sizeof *q * (size_t)n);
            if (!q) rt_fatal("vk: out of memory for the texture pool's entry counts");
            pref = q, cpref = n;
        }
        i = npref++;
    }
    pref[i] = (PRef){e, 0, 0, e->pub.gpu_owned};
    return (uint32_t)(e->ph = i + 1);
}

void vk_texture_pool_hold_locked(uint32_t old, uint32_t ent) {
    if (ent) pref[ent - 1].refs++;
    if (old && !--pref[old - 1].refs && !pref[old - 1].e) pref[old - 1].next = pref_free, pref_free = (int)old;
}

static void pool_forget(Tex *e) {   // destroy_tex
    if (!e->ph) return;
    PRef *q = &pref[e->ph - 1];
    q->e = NULL;
    if (!q->refs) q->next = pref_free, pref_free = e->ph;
    e->ph = 0;
}

static void pool_touch(void) {
    static int at;
    for (int i = 0; i < npref; i++)
        if (pref[i].gown && pref[i].refs && pref[i].e) touch(pref[i].e);
    for (int k = (npref + POOL_TOUCH_DIV - 1) / POOL_TOUCH_DIV; k-- > 0;) {
        if (at >= npref) at = 0;
        const PRef *q = &pref[at++];
        if (!q->gown && q->refs && q->e) touch(q->e);
    }
}

// e becomes GPU-owned: the list's check does nothing for it any more, and its use stamp is left in each submission.
static void pool_gpu_owned(Tex *e) {
    pool_unlist(e);
    if (e->ph) pref[e->ph - 1].gown = 1;
}

int vk_texture_pool_note_locked(int id, uint32_t *ent) {
    *ent = 0;
    if (id < 0 || id >= ntslots || !tslots[id].used) return 0;
    // The caller's lookups left the slot resolved under its bucket's generation, unless it made none (a fill without a
    // token or a kind): an entry cached under another generation may be destroyed, so the slot is looked up first.
    if (tslots[id].ce && tslots[id].cgen != agen[sinfo[id] & 0xffff]) vk_texture_by_id_locked(id, NULL, NULL);
    Tex *e = tslots[id].ce;
    if (!e) return 0;
    if (!e->pidx && from_storage(e) && !e->pub.gpu_owned) {
        if (npent == cpent) {
            int n = cpent ? cpent * 2 : 1024;
            PEnt *p = realloc(pent, sizeof *p * (size_t)n);
            int *q = realloc(pq, sizeof *q * (size_t)n);
            if (!p || !q) rt_fatal("vk: out of memory for the texture pool's entry list");
            pent = p, pq = q, cpent = cpq = n;
        }
        pent[npent++] = (PEnt){e, e->pub.addr, e->t.L.size, 0, 0};
        e->pidx = npent;
    }
    *ent = pool_ref(e);
    return 1;
}

// Released entries are left out: a slot resolves to none of those whose content the CPU provides (slot_resolve's stale
// rule), and the game may be unmapping their storage.
// NX2NAVI_POOL_PRESNAP=1 (default), 0, ab: the check takes the write-watch snapshot of an entry's regions that have
// none in this submission yet before it looks at them (one exclusive hold of the generation table's lock for the list),
// so an unchanged entry in such a region passes there. Without it 97 % of the 5,600 entries missed the region check for
// want of a snapshot and went the long way, one entry and one lock at a time. With it 97 % pass, 3.5 snapshot queries
// a submission: the check 0.50 to 0.19 ms a submission, the replay thread -0.37 ms against the control.
static int pool_presnap(void) {
    static int mode = -1;
    if (mode < 0) mode = rt_knob("POOL_PRESNAP", "1");
    return RT_KNOB_ON(mode);
}

// The submission being recorded checked the pool's entries by this list: for the idle clock it walked the pool
// (vk_texture_new_submit).
static int pool_by_list;

int vk_texture_pool_fresh_locked(void) {
    int nq = 0, ps = pool_presnap();
    pool_by_list = 1;
    if (ps) vk_mem_gen_lock();
    else vk_mem_gen_lock_shared();
    for (int i = 0; i < npent; i++) {
        const PEnt *p = &pent[i];
        if (p->ok) {
            if (ps) n_presnap_q += vk_mem_snap_range_locked(p->addr, p->size, submit_epoch);
            int why = vk_mem_unchanged_why_locked(p->addr, p->size, p->since, submit_epoch);
            if (!why) continue;
            n_pool_miss[why]++;
        } else {
            n_pool_miss[0]++;
        }
        pq[nq++] = i;
    }
    if (ps) vk_mem_gen_unlock();
    else vk_mem_gen_unlock_shared();
    n_pool_skips += (uint64_t)(npent - nq);
    for (int k = nq; k-- > 0;) {   // from the end: an entry leaving swaps the last one into its place
        int i = pq[k];
        PEnt *p = &pent[i];
        Tex *e = p->e;
        if (e->pub.gpu_owned) {
            pool_unlist(e);
            continue;
        }
        if (e->released) continue;
        if (!e->uploaded || e->checked != submit_epoch) {
            make_fresh_snap(e);
            e->checked = submit_epoch;
            n_pool_checks++;
        }
        p->since = e->wgen, p->ok = e->uploaded;
    }
    pool_touch();
    return npent;
}

// slot_resolve's last answer was none because of its stale rule (vk_texture_slot_retry_locked)
static int resolve_stale;

// NX2NAVI_TRACE=perf, since the last perf line: slots a finalize marked (its own texture object's), slots at the storage
// with the same parameters registered with another texture object, which a finalize leaves current
// (vk_texture_release), and lookups the stale rule answered with the stand-in (slot_resolve). _hm: of them, the
// terrain's streamed heightmaps (R32F 260x260), whose stand-in draws a chunk of ground at height zero.
static uint64_t n_fin_own, n_fin_other, n_fin_other_hm, n_stale, n_stale_hm, n_stale_kept;
static int terrain_heightmap(const tex_t *t) {
    return t->d.format == NVN_FORMAT_R32F && t->d.width == 260 && t->d.height == 260;
}

int vk_texture_slot_retry_locked(int id) {
    if (id < 0 || id >= ntslots || !tslots[id].used || !tslots[id].t.mem) return -1;
    resolve_stale = 0;
    if (vk_texture_by_id_locked(id, NULL, NULL)) return 1;
    return resolve_stale ? -1 : 0;
}

static void retire_remove(Tex *e);

static void destroy_tex(Tex *e) {
    if (e->retiring) retire_remove(e);
    pool_unlist(e);
    pool_forget(e);
    own_remove(e);
    gwin_remove(e);
    n_site[0]++;
    tex_gen_bump(e->key.addr);
    nvn_hm_del(&by_key, &e->key);
    Tex *head = nvn_hm_get(&by_addr, &e->key.addr);
    if (head == e) {
        if (e->next_at) nvn_hm_put(&by_addr, &e->key.addr, e->next_at);
        else nvn_hm_del(&by_addr, &e->key.addr);
    } else {
        for (Tex *p = head; p; p = p->next_at)
            if (p->next_at == e) { p->next_at = e->next_at; break; }
    }
    if (e->released) nreleased--, rel_unlink(e);
    if (e->pub.image) vk_exec_image_gone(e->pub.image);   // none when another texture took it (create_tex, reuse)
    vk_defer_image(e->pub.image, e->alloc);
    if (e->flat3d) {   // its 3D copy goes the same way
        vk_exec_image_gone(e->flat3d->image);
        vk_defer_image(e->flat3d->image, e->flat3d_alloc);
        e->flat3d->gone = 1;
        vk_defer_free(e->flat3d);
    }
    free(e->snap);
    // The record goes once the GPU is past the submission being recorded, as its image: the replay keeps &e->pub as a
    // render target for the rest of a command buffer (vk_exec.c resolve_target) and DLSS for the frame, and this runs on
    // whichever thread released the texture. Freed here, a pass that began again read the freed record, cached a view of
    // the dead image (vk_exec_target_view), and a later image given the same handle drew through it into freed memory.
    e->pub.gone = 1;
    vk_defer_free(e);
}

// ---- a flat texture sampled as a 3D one
// Maxwell samples a texture by its own descriptor, whatever the shader's instruction says: a 2D texture under a 3D
// sample gives its texel at (u, v), and the third coordinate does nothing. Vulkan has no 3D view of a 2D image, so
// get_view (vk_exec_draw.c) found no view for such a binding and the draw sampled a stand-in. Such a texture now gets a
// 3D image of one slice with its texels, all of its levels, made in the upload batch and copied again when the texture
// was uploaded or drawn into since (n_up, wr_fence) or another layer is asked for; a 3D view of that gives the 2D
// texel for any w. The copy is recorded in the upload batch, which runs before the part that samples it, so a texture
// the GPU draws every frame is sampled as it was a part or a frame before. Small plain colour textures only:
// FLAT3D_MAX texels, one sample, not block-compressed (BC rejects 3D on some drivers). NX2NAVI_TEX_FLAT3D=0: the
// stand-in, as before. Texture lock held; NULL when the texture does not qualify or the image cannot be made.
#define FLAT3D_MAX (1024u * 1024u)
static volatile int32_t n_flat3d;

const VkTexImage *vk_texture_flat3d_locked(const VkTexImage *img, uint32_t layer) {
    static int on = -1;
    if (on < 0) on = rt_knob("TEX_FLAT3D", "1") == 1;
    Tex *e = (Tex *)img;   // pub is the first member
    if (!on || img->gone || !img->image || img->type != VK_IMAGE_TYPE_2D || img->samples > 1 ||
        img->aspect != VK_IMAGE_ASPECT_COLOR_BIT || img->block_views || format_compressed(img->format) ||
        (uint64_t)img->width * img->height > FLAT3D_MAX || layer >= img->layers || img->levels > 16)
        return NULL;
    VkCtx *c = vk_ctx();
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (!e->flat3d) {
        VkTexImage *f = malloc(sizeof *f);
        if (!f) return NULL;
        *f = *img;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_3D,
                                .format = img->format, .extent = {img->width, img->height, 1}, .mipLevels = img->levels,
                                .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ci, &ai, &f->image, &e->flat3d_alloc, NULL) != VK_SUCCESS) {
            free(f);
            return NULL;
        }
        f->type = VK_IMAGE_TYPE_3D, f->view_type = VK_IMAGE_VIEW_TYPE_3D, f->depth = 1, f->layers = 1;
        f->usage = ci.usage, f->bound = 0, f->block_views = 0, f->fb_usage = 0, f->fb_learned = 0, f->gone = 0;
        e->flat3d = f;
        e->flat3d_up = ~0u;
        cb = vk_upload_begin();
        VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                    .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                    .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = f->image,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1}};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &ib};
        vkCmdPipelineBarrier2(cb, &di);
        log_limited(&n_flat3d, "texture %#llx (%ux%u, %u levels, format %d) is sampled as a 3D texture: a one-slice 3D "
                    "copy of it is made", (unsigned long long)img->addr, img->width, img->height, img->levels,
                    (int)img->format);
    }
    if (e->flat3d_up != e->n_up || e->flat3d_wr != e->wr_fence || e->flat3d_layer != layer) {
        if (!cb) cb = vk_upload_begin();
        VkImageCopy rg[16];
        for (uint32_t l = 0; l < img->levels; l++) {
            uint32_t w = img->width >> l, h = img->height >> l;
            rg[l] = (VkImageCopy){.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, layer, 1},
                                  .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1},
                                  .extent = {w ? w : 1, h ? h : 1, 1}};
        }
        barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdCopyImage(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, e->flat3d->image, VK_IMAGE_LAYOUT_GENERAL, img->levels, rg);
        barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
        e->flat3d_up = e->n_up, e->flat3d_wr = e->wr_fence, e->flat3d_layer = layer;
    }
    if (cb) vk_upload_end();
    return e->flat3d;
}

// ---- origins (NX2NAVI_TRACE=perf; a run without it tests the flag and counts the drops, nothing more)
// A texture drawn without its content leaves nothing in the log: no call fails and the image exists. What can still be
// told afterwards is where the entry came from, so each entry keeps how it was made and how its content started
// (og_*, had), and the last DROPS_N destroyed entries keep their descriptor and the reason they went. An entry made
// for a descriptor that is still in that ring says which drop it follows (origin_made), and the lines about one
// address list what was dropped there (others_at). The users are marks_check, which names an entry a shader sampled
// while nothing had ever put content into its image, and the lines of start_at.
#define DROPS_N 4096
typedef struct { uint64_t kh, addr, frame; uint32_t format, flags, w, h; uint8_t why, mirrored, released; } DropRec;
static DropRec drops[DROPS_N];
static uint64_t ndrops;
static uint64_t n_drop_by[DROP_N];   // entries destroyed since the last perf line, by reason
static const char *const drop_names[DROP_N] = {
    "", "past RELEASED_MAX released entries", "past the released entries' size cap", "under commit pressure",
    "under VRAM pressure", "idle, under VRAM pressure", "idle, under commit pressure", "unsampled (disuse)",
    "overwritten at another address", "replaced by another descriptor at its address", "its image went to another texture",
    "its storage went", "made whole again (texture detail)", "shutdown"};
static const char *const start_names[START_N] = {
    "started on its own", "filled from storage over a live entry", "content copied from a live entry",
    "content copied from a released entry", "started empty over a live entry", "started empty over a released entry",
    "content copied from the released CPU-written entry it replaced (split fill)", "filled from its storage (split fill)"};
const char *vk_texture_start_name(int start) { return start >= 0 && start < START_N ? start_names[start] : "?"; }
const char *vk_texture_drop_name(int why) { return why >= 0 && why < DROP_N ? drop_names[why] : "?"; }

// An entry's event for the flight recorder (flight.h flight_tex, FLIGHT_TF_*): a no-op while the recorder is off.
static void flight_ev(const Tex *e, int kind, uint32_t detail, uint32_t us) {
    if (!flight_on) return;
    uint32_t fl = (e->f.depth ? FLIGHT_TF_DEPTH : 0) | (e->f.stencil ? FLIGHT_TF_STENCIL : 0) |
                  (e->pub.mirrored ? FLIGHT_TF_MIRRORED : 0) | (e->bound_to ? FLIGHT_TF_BOUND : 0) |
                  (e->f.astc_bw ? FLIGHT_TF_ASTC : 0) | (e->pub.gpu_owned ? FLIGHT_TF_GPU : 0);
    flight_tex(kind, e->key.addr, (uint32_t)e->t.d.format, e->pub.width, e->pub.height, e->pub.layers, fl, detail, us);
}

static uint64_t frames_now(void) { return __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED); }

static uint64_t key_hash(const TexKey *k) {   // FNV-1a; make_key zeroes the key's padding
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *b = (const uint8_t *)k;
    for (size_t i = 0; i < sizeof *k; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

// Drops for a low commit that come back (tex_lock held throughout). A texture dropped because the system's commit ran
// low frees its memory only for as long as the game does not ask for it again; when it does, it is made and uploaded
// again and holds the same memory as before. With other programs holding 49 of an 84 GB limit, the commit sat at the
// low line for whole sessions: 340 to 590 released textures a second went, most were made again within seconds (178
// re-uploads a second against 21 without the drops), and the uploads inside the draws held the texture lock and the
// game's threads behind it, a 35-45 ms frame several times a second, while the commit stayed where it was.
// So each drop under the commit rule notes its texture (by its key, CB_SLOTS slots, the newest wins), and a texture
// made again within CB_WITHIN presents counts as come back. Over each CB_PERIOD, when at least CB_MIN_DROPS went and
// half or more came back, the commit drops (released entries and idle live ones) pause for CB_HOLD_MIN presents,
// doubled at each such period up to CB_HOLD_MAX; a period in which fewer came back ends the doubling. Below half the
// low line (commit_critical) nothing pauses: there the commit running out ends the game.
#define CB_SLOTS 8192
#define CB_WITHIN PRESENTS(600)
#define CB_PERIOD PRESENTS(240)
#define CB_MIN_DROPS 64
#define CB_HOLD_MIN PRESENTS(600)
#define CB_HOLD_MAX PRESENTS(7200)
static struct { uint64_t kh, frame; } cb_slot[CB_SLOTS];
static uint64_t cb_drops, cb_backs, cb_t0, cb_hold, cb_hold_until;
static uint64_t n_commit_back, n_commit_held;   // since the last perf line
static int commit_critical, cb_logs;

static uint64_t cb_key(const Tex *e) { return key_hash(&e->key) ^ (e->key.addr * 0x9e3779b97f4a7c15ull); }

static void commit_drop_note(const Tex *e) {
    uint64_t kh = cb_key(e);
    cb_slot[kh & (CB_SLOTS - 1)].kh = kh, cb_slot[kh & (CB_SLOTS - 1)].frame = frames_now() | 1;
    cb_drops++;
}

static void commit_back_note(const Tex *e) {   // a new entry
    uint64_t kh = cb_key(e), now = frames_now();
    typeof(cb_slot[0]) *s = &cb_slot[kh & (CB_SLOTS - 1)];
    if (s->kh != kh || !s->frame || now - s->frame > CB_WITHIN) return;
    s->frame = 0;
    cb_backs++, n_commit_back++;
}

// 1 while the commit drops pause (above); judges the period that ended.
static int commit_drops_held(void) {
    uint64_t now = frames_now();
    if (now - cb_t0 >= CB_PERIOD) {
        if (cb_drops >= CB_MIN_DROPS && cb_backs * 2 >= cb_drops) {
            cb_hold = cb_hold ? (cb_hold * 2 < CB_HOLD_MAX ? cb_hold * 2 : CB_HOLD_MAX) : CB_HOLD_MIN;
            cb_hold_until = now + cb_hold;
            if (cb_logs < 12) {
                cb_logs++;
                rt_log("vk: commit low, but %llu of the %llu textures dropped for it were made again within %llu presents: "
                       "the commit drops pause for %llu presents", (unsigned long long)cb_backs,
                       (unsigned long long)cb_drops, (unsigned long long)CB_WITHIN, (unsigned long long)cb_hold);
            }
        } else if (cb_drops >= CB_MIN_DROPS) {
            cb_hold = 0;
        }
        cb_t0 = now, cb_drops = cb_backs = 0;
    }
    if (commit_critical || now >= cb_hold_until) return 0;
    n_commit_held++;
    return 1;
}

// destroy_tex with the reason, which every caller gives.
// An entry the GPU drew into since its last write-back holds texels its storage lacks. Dropped for a reason that leaves
// the storage to the game (a budget, a commit or video memory squeeze, its release, a replacement at its address), a
// texture the game makes again from that storage (the drawn-once maps the fence write-back is for, moved or remade)
// would start from what the storage held before the GPU drew. Such an entry goes back first when the fence write-back
// would take it (wb_wanted) and the CPU has not rewritten its storage since; the commit drop pause above counts it the
// same either way. Not an entry another target drew over (DROP_OVERWRITTEN: the storage is the newer one's), nor at
// shutdown.
static int wb_wanted(const Tex *e);
static void wb_cfg_init(void);
static int storage_rewritten(const Tex *e, uint32_t since, uint32_t *now);
static void write_back_n(Tex **v, int n);
static uint64_t n_drop_wb;   // entries written back before their drop since the last perf line

static void drop_tex(Tex *e, int why) {
    flight_ev(e, FLIGHT_TEX_DROP, (uint32_t)why, 0);
    if (why == DROP_COMMIT || why == DROP_IDLE_COMMIT) commit_drop_note(e);
    if (e->gpu_dirty && why != DROP_OVERWRITTEN && why != DROP_SHUTDOWN) {
        uint32_t now;
        wb_cfg_init();
        if (storage_rewritten(e, e->gpu_gen, &now) == 1) e->gpu_dirty = 0;
        else if (wb_wanted(e)) write_back_n(&e, 1), n_drop_wb++;
    }
    n_drop_by[why]++;
    if (rt_trace & RT_TRACE_PERF)
        drops[ndrops++ % DROPS_N] = (DropRec){key_hash(&e->key), e->key.addr, frames_now(), (uint32_t)e->t.d.format,
                                              (uint32_t)e->t.d.flags, e->t.d.width, e->t.d.height, (uint8_t)why,
                                              e->pub.mirrored, e->released != 0};
    destroy_tex(e);
}

// A new entry: the frame, and the drop of its descriptor's last entry when the ring still holds it.
static void origin_made(Tex *e) {
    e->og_frame = frames_now();
    commit_back_note(e);
    if (!(rt_trace & RT_TRACE_PERF)) return;
    uint64_t kh = key_hash(&e->key), n = ndrops < DROPS_N ? ndrops : DROPS_N;
    for (uint64_t i = 1; i <= n; i++) {   // newest first
        const DropRec *r = &drops[(ndrops - i) % DROPS_N];
        if (r->kh != kh || r->addr != e->key.addr) continue;
        e->og_drop = r->why, e->og_drop_frame = r->frame;
        return;
    }
}

// "fmt 0x25 (vk 37) 256x256x1, 9 levels, 1 layers, target 1, samples 0, flags 0x1, pool flags 0x22"
static int desc_text(char *out, size_t n, const Tex *e) {
    const nvn_texdesc *d = &e->t.d;
    return snprintf(out, n, "fmt %#x (vk %d) %ux%ux%u, %u levels, %u layers, target %d, samples %d, flags %#x, pool flags %#x",
                    d->format, (int)e->f.vk, d->width, d->height, d->depth, e->pub.levels + e->pub.skip, e->pub.layers,
                    d->target, d->samples, (unsigned)d->flags, (unsigned)e->t.pool_flags);
}

// "made at frame 100 again after a drop at frame 90 (under commit pressure); started empty over a released entry; ..."
static int origin_text(char *out, size_t n, const Tex *e) {
    int k = snprintf(out, n, "made at frame %llu", (unsigned long long)e->og_frame);
    if (e->og_drop && k < (int)n)
        k += snprintf(out + k, n - (size_t)k, " again after a drop at frame %llu (%s)", (unsigned long long)e->og_drop_frame,
                      drop_names[e->og_drop]);
    if (k < (int)n) k += snprintf(out + k, n - (size_t)k, "; %s", start_names[e->og_start]);
    if (e->og_reimg && k < (int)n) k += snprintf(out + k, n - (size_t)k, "; with a released entry's image");
    if (e->og_backs && k < (int)n)
        k += snprintf(out + k, n - (size_t)k, "; taken back %u times, last at frame %llu", e->og_backs,
                      (unsigned long long)e->og_back_frame);
    return k < (int)n ? k : (int)n - 1;
}

// Whether nothing ever put content into e's image: an entry the CPU writes that was not uploaded, or one it does not
// write, at an address the CPU can see, that the GPU never wrote and that got no copy, no texels and no split fill.
static int no_content(const Tex *e) {
    if (e->pub.gpu_owned) return 0;
    if (e->pub.mirrored) return !e->uploaded;
    return !e->had && !(e->sfill && e->uploaded) && e->key.addr && !nvn_gpu_only(e->key.addr);
}

// Lines per address for one kind of event: up to `most` each, in a table of the first SAID_N addresses that had one
// (a later address always gets its line), so a session's limit is by address, not a count of lines.
#define SAID_N 4096
typedef struct { uint64_t addr[SAID_N]; uint16_t n[SAID_N]; } SaidAt;
static int said_at(SaidAt *s, uint64_t addr, unsigned most) {
    uint32_t i = (uint32_t)(addr * 0x9E3779B97F4A7C15ull >> 52);
    for (int probe = 0; probe < 64; probe++, i = (i + 1) % SAID_N) {
        if (s->addr[i] && s->addr[i] != addr) continue;
        s->addr[i] = addr;
        return s->n[i] < most ? (s->n[i]++, 1) : 0;
    }
    return 1;
}

// The other descriptors at e's address: every entry in the cache with its state, and what the ring knows was dropped
// there, newest first. skip: an entry the caller describes itself (NULL: none).
static int others_at(char *out, size_t n, const Tex *e, const Tex *skip) {
    int k = 0, any = 0;
    out[0] = 0;
    for (const Tex *p = nvn_hm_get(&by_addr, &e->key.addr); p && k < (int)n - 1; p = p->next_at) {
        if (p == e || p == skip) continue;
        k += snprintf(out + k, n - (size_t)k, "%s[%s%s%s%s: fmt %#x flags %#x %ux%u, %s, %s]", any++ ? " " : "",
                      p->released ? "released" : "live", p->superseded ? ", superseded" : "", p->retiring ? ", retiring" : "",
                      p->pub.gpu_owned ? ", GPU-written" : "", p->t.d.format, (unsigned)p->t.d.flags, p->t.d.width,
                      p->t.d.height, p->pub.mirrored ? (p->uploaded ? "CPU-written, uploaded" : "CPU-written, not uploaded")
                                                     : (no_content(p) ? "not CPU-written, no content" : "not CPU-written"),
                      start_names[p->og_start]);
    }
    uint64_t have = ndrops < DROPS_N ? ndrops : DROPS_N;
    int listed = 0;
    for (uint64_t i = 1; i <= have && listed < 4 && k < (int)n - 1; i++) {
        const DropRec *r = &drops[(ndrops - i) % DROPS_N];
        if (r->addr != e->key.addr) continue;
        k += snprintf(out + k, n - (size_t)k, "%s[dropped at frame %llu (%s): fmt %#x flags %#x %ux%u, %s%s]", any++ ? " " : "",
                      (unsigned long long)r->frame, drop_names[r->why], r->format, r->flags, r->w, r->h,
                      r->mirrored ? "CPU-written" : "not CPU-written", r->released ? ", released then" : ", live then");
        listed++;
    }
    if (!any) k = snprintf(out, n, "none");
    return k < (int)n ? k : (int)n - 1;
}

// Released entries with another descriptor at the same address are replaced; past RELEASED_MAX released entries one
// goes at each release (it comes back on demand, a mirrored one re-uploaded from its storage). NX2navi
// finalizes and initializes transient textures every frame, so a released entry is mostly taken back within frames.
// Dropping all of them at once when the count passed 4096 destroyed thousands of images in one frame: 333-422 ms
// frames every 40-50 s on a GTX 1050 Ti, then the textures still in use were created and uploaded again.
//
// The one that goes is the oldest release that nothing sampled or drew into lately (samp_frame) or whose content is
// gone (overwritten), not the oldest release as such. A released entry is not dead to the game (drop_stale, below, has the
// reasons): a descriptor registered for it still resolves to it. Ocarina reads its scene picture in every frame through
// a second descriptor at the picture's address (RGB9E5, 1920x1080, the input of its own upscaler) whose entry was
// released once, a few seconds into the area, and was looked up from then on: it headed the list for the rest of
// the session. One frame at the limit, as it was:
//   1. the count of released entries passes RELEASED_MAX at a release, and the head of the list, that entry, is
//      destroyed although the frame before sampled it;
//   2. later in the frame the game's descriptor is looked up, finds no entry, and one is made again; its content is
//      copied from the released scene target at its address (start_at);
//   3. the frame comes out with the 3D scene black and the interface intact; the next one is whole again. Why the
//      frame with the remade entry is black was not traced.
// The count reaches the limit where released entries pile up: with the game's own upscaler in use (upscaler = fsr)
// its render targets lie at new addresses in every frame, 2 to 13 new entries a frame, and the count went from 540
// to 2,040 within 2,400 frames in Hyrule Field. Measured there (RTX 5090, 2026-11-11, Link running, 13,630 frames
// kept of each run): 8 black frames in 5 of 7 runs. Where the remake was logged (3 runs, 4 black frames) each is the
// frame after it, and where the drop was logged (2 runs) it is this sweep taking that entry as the count first
// reaches the limit; in one of the two runs without a black frame that came after the frames kept. With the rule
// below a run of 18,730 frames dropped 834 released entries past the limit from frame 11,600 on and showed no black
// frame. A session with DLSS as the upscaler had the same black frame at Lon Lon Ranch while its log showed 2,032 to
// 2,047 released entries, kept there by a low system commit; that it is the same drop is read from those counts, not
// logged.
// "Lately" is by sampling, not by lookups. The first form of this rule spared every entry a lookup still reached
// (idle), and the texture pool's refresh looks up every registered slot in each submission that draws through the
// pool: an entry whose descriptor the game merely leaves registered counts as looked up for good. With the game's own
// upscaler in use its render targets move to new addresses in every frame and each leaves such an entry behind, 3 to
// 7 a frame: over a town all 2,049 released entries were "looked up", the sweep fell back to the oldest lookup, which
// among entries all stamped in the same submission is no order at all, and took entries still drawn: 200 to 450 a
// window of 60 frames, with buildings black or wrongly lit for single frames. samp_frame is stamped when a marked
// shader samples the entry's slot (vk_pool_marks_read) and by every lookup that is not the pool refresh (touch: a
// draw's fixed binding, a render target, a copy), so a target the game draws into or reads is recent by it and one
// it only keeps registered is not.
// With this rule a player's session of 30,000 frames through eight areas kept the list at its limit, dropped
// 50,182 entries past it, and showed none of those frames (2026-11-11, a hand test; no scripted run was made).
// When every released entry was sampled within SWEEP_UNSAMPLED presents, the one sampled longest ago goes.
//
// That rule holds from RELEASED_HARD entries on. Between RELEASED_MAX and RELEASED_HARD only an entry that loses
// nothing goes: one that comes back intact or an overwritten one, released STALE_FRAMES presents ago or earlier (a
// younger one is mostly taken back within frames). An entry whose content exists only in its image can be used again
// long after anything sampled it. In Kokiri Forest the count stays at RELEASED_MAX, and two render targets at one
// address (a 40x24x64 volume and a 2240x1260 picture), released at the area's load and last sampled within seconds
// of it, went as unsampled about two minutes later; the game used them again after that, they were made again empty,
// and one frame came out with the scene black. On an 85 s route there (Link running, six runs a batch): a black
// frame in 10 of 24 runs with the oldest release going; in 1 of 12 with the unsampled one going (the two targets went
// in 7 of those runs and were made again empty in 4; the black frame is one in which the sweep took a row of render
// targets); in 0 of 18 with such entries kept. Kept, the count reached 6,200 in the runs that pass the busiest part
// of the route early and 2,300 to 2,500 in the others; frame times and the peak of the device memory in use were the
// same in all three forms, the device memory at the end of a run 0.3 to 0.5 GiB higher with 6,200 kept.
// A kept entry bound into pool memory owns no memory and goes with its pool (vk_texture_range_gone); the memory of
// the others is bounded by NX2NAVI_TEX_RELEASED_MB (drop_stale).
// Each call looks at up to SWEEP_LOOK entries from where the last one stopped (sweep_at; rel_unlink moves it off an
// entry that leaves): with thousands of entries that stay, a walk from the head at every release would cross them all.
// RELEASED_MAX is the floor of that limit; released_max raises it by the card (NX2NAVI_TEX_RELEASED_MAX sets it).
#define RELEASED_MAX 2048
#define RELEASED_MAX_TOP 8192
#define RELEASED_HARD 16384
#define SWEEP_LOOK 64
#define SWEEP_UNSAMPLED PRESENTS(120)

static int comes_back_intact(const Tex *e);

static uint64_t stale_after(const Tex *e);

// The limit of released entries past which the sweep drops those that lose nothing: one per 2 MiB of the video memory
// budget, from RELEASED_MAX to RELEASED_MAX_TOP. In Castle Town market on a 16 GB card (RTX 5060 Ti) with 2048 the
// sweep dropped 412 entries in a session and a few textures blinked black for a moment; with 8192 (and
// NX2NAVI_TEX_RELEASED_MB = 4096) it dropped none, 6,863 were kept and none blinked. Their memory stays bounded by
// NX2NAVI_TEX_RELEASED_MB and the VRAM pressure. Read once the budget is known.
static int released_max(void) {
    static int max;
    if (max) return max;
    const char *s = cfg_get("NX2NAVI_TEX_RELEASED_MAX");
    if (s && *s) {
        long long n = atoll(s);
        max = n < 1 ? 1 : n > RELEASED_HARD ? RELEASED_HARD : (int)n;
    } else {
        uint64_t vused = 0, vbudget = 0;
        int exact = 0;
        if (!vk_mem_vram(&vused, &vbudget, &exact) || !vbudget) return RELEASED_MAX;
        long long n = (long long)(vbudget >> 21);
        max = n < RELEASED_MAX ? RELEASED_MAX : n > RELEASED_MAX_TOP ? RELEASED_MAX_TOP : (int)n;
    }
    rt_log("vk: released textures: the sweep keeps up to %d entries (NX2NAVI_TEX_RELEASED_MAX)", max);
    return max;
}

static void sweep_released(void) {
    int lim = released_max();
    if (nreleased <= lim) return;
    {
        uint64_t now = frames_now();
        const uint64_t stale = PRESENTS(120);   // STALE_FRAMES, defined below
        Tex *e = sweep_at ? sweep_at : rel_head;
        for (int n = 0; n < SWEEP_LOOK && nreleased > lim; n++) {
            if (!e || now - e->rel_frame < stale) { e = rel_head; break; }   // the rest is younger: from the oldest again
            Tex *next = e->rel_next;
            // an entry the game took back before waits out GAP_TIMES the longest of those releases (stale_after)
            if ((comes_back_intact(e) || e->overwritten) && now - e->rel_frame >= stale_after(e)) drop_tex(e, DROP_MAX);
            e = next;
        }
        sweep_at = e;
    }
    while (nreleased > RELEASED_HARD && rel_head) {
        uint64_t now = frames_now();
        Tex *gone = NULL, *lru = rel_head;
        for (Tex *e = rel_head; e && !gone; e = e->rel_next) {   // in release order
            if (e->overwritten || now - e->samp_frame >= SWEEP_UNSAMPLED) gone = e;
            else if (e->samp_frame < lru->samp_frame) lru = e;
        }
        if (!gone) {
            static volatile int32_t n_all_used;
            log_limited(&n_all_used, "all %d released textures were sampled or drawn into lately: the one longest ago goes "
                        "(%#llx fmt %#x %ux%u, last in frame %llu)", nreleased, (unsigned long long)lru->key.addr,
                        lru->t.d.format, lru->t.d.width, lru->t.d.height, (unsigned long long)lru->samp_frame);
        }
        drop_tex(gone ? gone : lru, DROP_MAX);
    }
}

// NX2NAVI_TEX_VRAM_PRESSURE (percent of the device-local budget, default VRAM_PRESSURE_DEFAULT; 0 = off): above it, each
// submit drops up to PRESSURE_STEP of the oldest released entries, so a 4 GiB card does not keep RELEASED_MAX released
// images (each ASTC one decoded to RGBA8) while the heap spills into system memory. A few per submit, for the reason
// above. Only entries that come back intact are dropped: mirrored ones the GPU never wrote, re-uploaded from their
// storage. A render target lives only in its image; dropping it brought the texture back empty (black flashes in a
// Depths boss fight, ~170 "left uninitialised" aliases a minute).
//
// The default lies under the seven eighths of the budget past which a pool gets no VRAM mirror (vk_memory.c
// mirror_create), and the evictions aim 2% under it, so textures give way before mirrors are refused: on an 8 GiB card
// that leaves the mirrors about 0.7 GiB, on a 24 GiB one 2 GiB. With the default at 90 the order was the other way
// round: at 88% of a 22 GiB budget, full of textures of areas left long before, 0.4 MiB mirrors were refused and the
// draws that read their pools over PCIe brought the game to 11-20 fps, while no texture had been asked to go.
#define VRAM_PRESSURE_DEFAULT 80
#define PRESSURE_STEP 32
static int vram_pressure = -1;
static uint64_t n_pressure_drops;

static int comes_back_intact(const Tex *e) { return e->pub.mirrored && !e->pub.gpu_owned && !e->gpu_dirty; }

// When a lookup last reached an entry: entry_of (every texture get and slot resolution) and the per-submit check of a
// slot lookup (vk_texture_by_id_locked). The texture pool refresh looks every registered slot up once in each
// submission that draws through the pool (vk_exec_draw.c pool_sync_entries), so an entry no lookup reached for
// IDLE_ROUNDS such submissions and STALE_FRAMES presents (below) has no live descriptor naming it: the game keeps the
// texture object, nothing samples it. The clock is look_round, which only a submission whose slot checks reached half
// of the registered pool slots moves on (vk_texture_new_submit): counted in submissions, a pause screen or a fade that
// draws nothing through the pool made the whole world's textures idle within seconds.
// With NX2NAVI_POOL_FAST the refresh checks the entries by list instead (vk_texture_pool_fresh_locked), which is no
// lookup. Such a submission counts for the clock all the same, and the entries the pool's slots hold get their stamp
// from the count of those slots (pool_touch), each at least once in POOL_TOUCH_DIV submissions: an entry a slot names
// never comes near IDLE_ROUNDS.
#define IDLE_ROUNDS 240
static uint32_t look_round = 1;     // submissions whose lookups walked the texture pool
static uint32_t round_checks;       // slot checks of the submission being recorded (vk_texture_by_id_locked)
static uint32_t pool_slots;         // registered slots the pool covers (id < VK_POOL_TEX)
static int pool_ctx;                // the texture pool refresh is looking slots up (vk_texture_pool_ctx)
static void touch(Tex *e) {
    e->use_round = look_round;
    e->use_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (!pool_ctx) e->samp_frame = e->use_frame;   // a draw binds it, or the game or the replay reads it otherwise
}

void vk_texture_pool_ctx(int on) { pool_ctx = on; }

static int idle(const Tex *e, uint64_t now);

// NX2NAVI_TEX_RELEASED_MB (default a thirty-second of the installed memory, 512 MiB to 2 GiB, or more by the card's
// budget, released_cap_bytes; 0 = no limit) bounds the
// own memory of the released entries: RELEASED_MAX alone kept 5-7 GiB of released render targets and decoded textures
// after an hour of play at a 2160p game resolution, device memory the driver also charges to the system's commit (the
// process's private bytes reached 31 GiB). A sixteenth (1 to 4 GiB) still let them hold 1.3 GiB on a 32 GB PC whose
// commit then ran out at an area load (device lost); only entries released STALE_FRAMES presents ago go, so the
// transient ones the game takes back within frames stay. NX2NAVI_TEX_COMMIT_FREE_MB (default the larger of 4 GiB and a tenth of the commit
// limit; 0 = off) lowers that bound to COMMIT_LOW_KEEP while less of the system's commit is free: an allocation that
// fails there ends the game. On Linux nothing fails at the limit: the OOM killer ends the game when its cgroup or the
// machine runs out of memory, and plat_commit_info reports the tighter of the two. Both drop only stale entries
// (drop_stale), a few per check like the VRAM pressure.
//
// NX2NAVI_TEX_PROC_COMMIT_MB (default the installed memory, at least PROC_COMMIT_MIN_MB; 0 = off) is a bound on this
// process's own commit that is logged when passed; it counted the commit as low too until it was found that no texture
// dropped could bring the process back under it (commit_is_low, at its end). The system's figure depends on the page file and on what else
// runs: with a 64 GB limit it turned low at 59 GB in use, when this process alone held 42 GB on a PC with 32 GB of
// memory, about half of it device memory, with the textures of five areas left behind. A process is not meant to need
// more commit than the PC has memory: beyond that it lives on the page file, whatever its size. The bound holds until
// the process is a thirty-second under it, so that it does not flip with every texture freed. The floor keeps it out
// of the way on PCs with less memory, where a bound at the installed memory could hold all through play: the system's
// figure decides there, as before. On Linux the process's figure is its resident anonymous memory, which cannot pass
// the installed memory, so only a bound set below that holds there; a cgroup's limit and the machine's RAM act through
// the system's figure.
// Where a discrete GPU's memory counts against the commit (plat_commit_counts_vram: Windows), the bound compares the
// commit less the device-local memory the driver reports in use (vk_mem_vram): that memory is on the card, not in RAM
// or the page file, and the VRAM pressure and tex_disuse already bound it. Counted in, it held the bound all through
// Hyrule Field on a 16 GB PC with a 16 GB card (RTX 5060 Ti): the process at 28.9 to 30.7 GB, 9 to 10 GB of it video
// memory, while the system had 8 to 10 GB of its 49 GB limit free, and the ground flickered white as textures gave way.
// So on such a PC the 42 GB process above, half of it device memory, no longer reaches this bound: the system's
// figure and the VRAM pressure are what act there. The part of a card's memory the driver has moved into shared
// system memory is subtracted too, which it should not be; the driver does not report it apart.
#define COMMIT_LOW_KEEP ((uint64_t)256 << 20)
#define PROC_COMMIT_MIN_MB 24576
static uint64_t released_cap;   // bytes; 0 = not read yet, UINT64_MAX = no limit
#define RELEASED_CAP_MAX_MB 8192
static long long commit_free_mb = -2;   // -2: not read yet, -1: the default
static long long proc_commit_mb;        // this process's bound in MB; 0 = none
static int commit_low, commit_logs;     // commit_low: either of the two below at the last check
static int sys_low, proc_high;          // the system's commit is low; this process's is above its bound
static uint64_t n_cap_drops, n_commit_drops, n_commit_past;

static uint64_t released_cap_bytes(void) {
    if (!released_cap) {
        const char *s = cfg_get("NX2NAVI_TEX_RELEASED_MB");
        long long mb;
        int by_vram = 0;
        if (s && *s) mb = atoll(s);
        else {
            float used, total, proc;
            plat_mem_info(&used, &total, &proc);
            mb = total > 0 ? (long long)(total / 32) : 1024;
            mb = mb < 512 ? 512 : mb > 2048 ? 2048 : mb;
            // By the card too: a quarter of its budget where the PC has an eighth of its memory to match, up to
            // RELEASED_CAP_MAX_MB. The game lets go of the textures of what leaves the view and takes them back when
            // it returns; walking through a village the released ones passed 1 GiB within two or three minutes, and
            // from then on each release dropped an older one that was decoded and encoded again on its way back,
            // hundreds a second, on a card with 30 GiB free. The two pressure rules still come first: released
            // entries go before anything else above NX2NAVI_TEX_VRAM_PRESSURE of the budget, and the bound falls to
            // COMMIT_LOW_KEEP while the commit is low.
            uint64_t vused = 0, vbudget = 0;
            int exact = 0;
            if (vk_mem_vram(&vused, &vbudget, &exact) && total > 0) {
                long long by_card = (long long)(vbudget >> 22), by_ram = (long long)(total / 8);   // a quarter, in MiB
                long long more = by_card < by_ram ? by_card : by_ram;
                if (more > RELEASED_CAP_MAX_MB) more = RELEASED_CAP_MAX_MB;
                if (more > mb) mb = more, by_vram = 1;
            }
        }
        released_cap = mb > 0 ? (uint64_t)mb << 20 : UINT64_MAX;
        if (mb > 0) rt_log("vk: released textures keep at most %lld MiB of their own memory%s (NX2NAVI_TEX_RELEASED_MB)", mb,
                           by_vram ? ", a quarter of the video memory budget" : "");
        else rt_log("vk: released textures: no size limit");
    }
    return released_cap;
}

static int commit_is_low(void) {
    if (commit_free_mb == -2) {
        const char *s = cfg_get("NX2NAVI_TEX_COMMIT_FREE_MB");
        commit_free_mb = s && *s ? atoll(s) : -1;
        if (commit_free_mb < -1) commit_free_mb = -1;
        s = cfg_get("NX2NAVI_TEX_PROC_COMMIT_MB");
        proc_commit_mb = s && *s ? atoll(s) : -1;
        if (proc_commit_mb < 0) {
            float ram_used, ram_total, ram_proc;
            plat_mem_info(&ram_used, &ram_total, &ram_proc);
            proc_commit_mb = ram_total > PROC_COMMIT_MIN_MB ? (long long)ram_total : PROC_COMMIT_MIN_MB;
        }
        if (!plat_commit_enforced()) commit_free_mb = proc_commit_mb = 0;   // no limit known
        char note[2048];   // the platform layer's own lines go to stderr only, so the log file gets this through here
        if (plat_commit_note(note, sizeof note)) rt_log("platform: %s", note);
        if (proc_commit_mb)
            rt_log("vk: this process's commit%s is logged when it passes %lld MB (NX2NAVI_TEX_PROC_COMMIT_MB; 0 = off); "
                   "textures give way only when the system's commit is low",
                   plat_commit_counts_vram() ? ", less the video memory in use on a discrete card," : "", proc_commit_mb);
    }
    if (!commit_free_mb && !proc_commit_mb) return 0;
    double proc, used, limit;
    plat_commit_info(&proc, &used, &limit);
    int low = 0;
    commit_critical = 0;
    if (commit_free_mb && used >= 0 && limit > 0) {
        double want = commit_free_mb > 0 ? (double)commit_free_mb : limit / 10 > 4096 ? limit / 10 : 4096;
        low = limit - used < want;
        commit_critical = limit - used < want / 2;   // the pause for drops that come back ends here
    }
    double dev = 0;   // device-local memory in use, MB: in the commit (plat_commit_counts_vram), not in RAM (see above)
    VkCtx *c = vk_ctx();
    uint64_t dev_used, dev_budget;
    int exact;
    if (proc_commit_mb && plat_commit_counts_vram() && c &&
        c->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && vk_mem_vram(&dev_used, &dev_budget, &exact))
        dev = dev_used / 1048576.0 < proc ? dev_used / 1048576.0 : proc;
    int high = proc_commit_mb &&
               proc - dev > (double)(proc_high ? proc_commit_mb - proc_commit_mb / 32 : proc_commit_mb);
    if (high != proc_high && commit_logs < 40) {
        commit_logs++;
        rt_log("vk: this process's commit %s: %.0f MB less %.0f MB of video memory, the bound is %lld MB (system: %.0f "
               "of %.0f MB in use); released textures kept %d, %.0f MiB of own memory%s", high ? "high" : "no longer high",
               proc, dev, proc_commit_mb, used, limit, nreleased, rel_bytes / 1048576.0,
               high ? ": the host's own memory, which dropping textures does not lower; textures give way only when the "
                      "system's commit is low" : "");
    }
    proc_high = high;
    if (low != sys_low && commit_logs < 40) {   // a few lines, however often it flips
        commit_logs++;
        rt_log("vk: system commit %s: %.0f of %.0f MB in use (this process %.0f MB); released textures kept %d, %.0f MiB "
               "of own memory%s", low ? "low" : "no longer low", used, limit, proc, nreleased, rel_bytes / 1048576.0,
               low ? ": dropping the stale ones" : "");
        // A page file Windows sizes grows only slowly and by little, so the limit stays near the installed memory plus
        // what the page file held at the boot: on a 16 GB PC with a 16 GB card it ran low in Hyrule Field at 32 GB,
        // and textures gave way (white or black ground). A fixed page file of 32 GB or more raised the limit to 49 GB.
        static int hinted;
        if (low && !hinted && plat_page_file_managed() == 1) {
            hinted = 1;
            rt_log("vk: the page file is sized by Windows, which grows it only slowly: a fixed page file whose initial "
                   "size is at least 32 GB keeps the system's commit from running low (System > About > Advanced system "
                   "settings > Performance Settings > Advanced > Virtual memory > Change: untick the automatic "
                   "management, Custom size with an initial size of 32768 MB or more, Set, then restart Windows)");
        }
    }
    sys_low = low;
    // Only the system's figure makes textures give way. The process's bound compares its commit less the video memory
    // in use, and a texture dropped lowers both by the same amount: past the bound nothing a drop does could bring it
    // back under, and textures went one after the other for as long as the rest of the process stayed large, the ground
    // flickering white with the system's commit to spare (#434). What is above it is the host's own memory (the
    // game's heap, pipelines, caches); the bound is logged, and the system's figure still acts when the commit runs low.
    return commit_low = low;
}

int vk_texture_commit_low(void) { return __atomic_load_n(&commit_low, __ATOMIC_RELAXED); }

// Drops up to PRESSURE_STEP released entries that own memory and were released STALE_FRAMES presents ago or earlier,
// oldest first and the intact ones before the others, while the released entries own more than keep. A transient
// texture is taken back within frames, so dropping it frees nothing: the game initializes it again and it is created
// again, empty if it is a render target. Under commit pressure that a system-managed page file never relieves, dropping
// young ones destroyed 2160p render targets about every frame for whole sessions. Frame generation also reads a frame's
// depth target at the host present (vk_dlssg.c vk_dlssg_frame tags it), after the blit deferred destruction waits for.
//
// A released entry is not dead to the game. A descriptor registered for a render target still resolves to its released
// entry (slot_resolve: a finalized slot with no live texture at its storage gets its own), and the game can sample it
// or take it back long after the release. In Kakariko Village, with the system's commit low, render targets dropped at
// STALE_FRAMES came back empty and the scene was drawn black for one frame every two to three seconds (interface
// intact), for as long as the commit stayed low. So no entry goes while lookups still reach it (idle), and one whose
// content exists only in its image waits GPU_STALE_FRAMES, or GAP_TIMES the longest it stayed released before the game
// took it back when that is longer. An overwritten entry has no content left to wait for (overwrite_others): it goes
// with the intact ones, whether or not a descriptor still names it.
#define STALE_FRAMES PRESENTS(120)
#define GPU_STALE_FRAMES PRESENTS(1800)
#define GAP_TIMES 8
static int idle(const Tex *e, uint64_t now) {
    return look_round - e->use_round >= IDLE_ROUNDS && now - e->use_frame >= STALE_FRAMES;
}

// NX2NAVI_TEX_KEEP_TAKEN (default 1; 0: as before): an entry that comes back intact waits out GAP_TIMES the longest it
// stayed released before the game took it back, as one whose content lives only in its image does, and not
// STALE_FRAMES alone. Coming back intact is not free: the descriptor finds no entry, the frame samples its stand-in
// while the entry is made and uploaded again, and the next frame has it. Kakariko's hazy air is drawn with ASTC
// textures the game releases and takes back every few hundred frames; with the released entries at RELEASED_MAX the
// sweep dropped about 30 of them as soon as their 120 presents were up, and the frame that needed them again came out
// without the haze, darker and sharper, once every few seconds.
static int keep_taken(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("TEX_KEEP_TAKEN", "1") == 1;
    return on;
}

static uint64_t stale_after(const Tex *e) {
    uint64_t gap = (uint64_t)e->back_gap * GAP_TIMES;
    if (e->overwritten) return STALE_FRAMES;
    if (comes_back_intact(e)) return keep_taken() && gap > STALE_FRAMES ? gap : STALE_FRAMES;
    return gap > GPU_STALE_FRAMES ? gap : GPU_STALE_FRAMES;
}

static int drop_stale(uint64_t keep, uint64_t *count, int why) {   // returns the entries dropped: PRESSURE_STEP, more may follow
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    int dropped = 0;
    const uint64_t stale = STALE_FRAMES;
    for (int any = 0; any < 2; any++)
        for (Tex *e = rel_head, *next; e && now - e->rel_frame >= stale; e = next) {   // in release order
            if (dropped == PRESSURE_STEP || rel_bytes <= keep) return dropped;
            next = e->rel_next;
            if (!e->bytes || (!any && !comes_back_intact(e) && !e->overwritten)) continue;
            if (now - e->rel_frame < stale_after(e) || (!e->overwritten && !idle(e, now))) continue;
            drop_tex(e, why);
            dropped++, (*count)++;
        }
    return dropped;
}

// Drops up to PRESSURE_STEP entries that were marked overwritten OVERWRITTEN_KEEP presents ago or earlier
// (overwrite_others): the image reuse found no texture to give their images to in that time.
static void drop_overwritten(void) {
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    int dropped = 0;
    for (Tex *e = rel_head, *next; e && now - e->rel_frame >= OVERWRITTEN_KEEP && dropped < PRESSURE_STEP; e = next) {
        next = e->rel_next;
        if (!e->overwritten || now - e->ow_frame < OVERWRITTEN_KEEP) continue;
        drop_tex(e, DROP_OVERWRITTEN);
        dropped++, n_over_drops++;
    }
}

// NX2NAVI_TEX_EVICT (default on; 0 = only released entries go under VRAM pressure, as before): above the VRAM pressure
// threshold, once the released entries that come back intact are gone, live entries go too, least recently looked up
// first: those that come back intact and that no lookup reached for IDLE_ROUNDS pool walks and STALE_FRAMES presents
// (touch: no live descriptor names them). A later lookup creates the image again and uploads it from its storage, as
// for a texture seen the first time. Up to EVICT_STEP entries per check, until the device-local use is 2% of the budget
// under the threshold. The use counts the free space inside VMA's blocks as free (vk_mem_vram_live): a freed texture
// leaves a hole the driver still counts, and measured that way the evictions went on until nearly every idle texture
// was gone. The next check waits until the GPU is past the submissions that may still name the images, as their
// memory goes back to VMA only then (vk_defer_image). A VRAM mirror that did not fit asks for room the same way, below
// the threshold too (vk_texture_want_room): a pool without its mirror costs far more than a texture uploaded again.
// Its request stands as the use the mirror fits at (room_level) for ROOM_CHECKS checks or until the use is down there
// (sweep_pressure): taken as bytes to free at the one check that read it, a request for 205 MiB got the EVICT_STEP
// textures of that check, 26 MiB, or nothing when the check fell before evict_wait, and was gone.
#define EVICT_STEP 64
#define ROOM_CHECKS 64
static int tex_evict = -1;
static uint64_t evict_wait;                       // vk_sync_fence_done value the next eviction waits for
static uint64_t n_evict, n_evict_bytes;           // since the last perf line
static int evict_logs;
static volatile int64_t room_wanted;               // bytes refused VRAM mirrors asked for (vk_texture_want_room)
static uint64_t room_level;                        // tex_lock: the device-local use at which those mirrors fit
static uint32_t room_checks;                       // tex_lock: checks the request still stands for; 0 = none stands

void vk_texture_want_room(uint64_t bytes) {
    int64_t cur = __atomic_load_n(&room_wanted, __ATOMIC_RELAXED);   // the largest request since the last check
    while ((int64_t)bytes > cur && !__atomic_compare_exchange_n(&room_wanted, &cur, (int64_t)bytes, 0, __ATOMIC_RELAXED,
                                                                 __ATOMIC_RELAXED)) {}
}

static uint64_t evict_idle(uint64_t want, uint64_t used, uint64_t budget, int why) {   // tex_lock held; returns the bytes freed
    if (vk_sync_fence_done() < evict_wait) return 0;
    Tex *v[EVICT_STEP];
    int nv = 0;
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < by_key.cap; i++) {   // the EVICT_STEP oldest candidates, oldest first
        Tex *e = by_key.vals[i];
        if (!e || e->released || e->retiring || !e->bytes || !comes_back_intact(e) || !idle(e, now)) continue;
        uint32_t age = look_round - e->use_round;
        int at = nv;
        while (at > 0 && look_round - v[at - 1]->use_round < age) at--;
        if (at == EVICT_STEP) continue;
        if (nv < EVICT_STEP) nv++;
        memmove(v + at + 1, v + at, sizeof *v * (size_t)(nv - 1 - at));
        v[at] = e;
    }
    uint64_t freed = 0;
    int n = 0;
    for (; n < nv && freed < want; n++) freed += v[n]->bytes, drop_tex(v[n], why);
    if (!n) return 0;
    n_evict += (uint64_t)n, n_evict_bytes += freed;
    evict_wait = vk_sync_issued() + 2;
    if (evict_logs < 8 || ((rt_trace & RT_TRACE_PERF) && evict_logs < 64)) {
        evict_logs++;
        rt_log("vk: VRAM %.0f of %.0f MiB in use: %d idle textures evicted (%.1f MiB), least recently used first; they "
               "are uploaded again when the game uses them", used / 1048576.0, budget / 1048576.0, n, freed / 1048576.0);
    }
    return freed;
}

// ---- disuse (NX2NAVI_TEX_DISUSE, default on; vk_pipeline.c "texture pool marks", vk_exec_draw.c "pool marks")
// Ocarina keeps the textures of the areas it has loaded registered in the pool, and the pool refresh looks every
// registered slot up, so idle (above) never holds for them, and their memory, which the driver charges to the system's
// commit, only grew. Marked shaders tell which slots they sample: vk_pool_marks_read sets samp_frame of the texture of
// each marked slot. Where evict_idle frees too little, evict_disused takes live textures that come back intact and that
// nothing sampled for DISUSE_FRAMES presents, least recently sampled first, in two steps:
//  - retired (retire): the entry stays whole, but the pool refresh no longer resolves a slot to it (slot_resolve), so
//    every slot that names it gets the kind's stand-in. Any use in the meantime takes it back as it is (retire_cancel):
//    a lookup other than the pool refresh, or a mark on one of its slots (the stand-in was sampled).
//  - destroyed (retire_sweep) once the texture pool has taken every set of its ring since, each brought up to date
//    when taken (vk_exec_draw.c pool_take_set, pool_catch_up), after a sync that left no slot behind wrote its stand-in
//    (vk_pool_takes counts only such takes), and the GPU has finished the submissions recorded until
//    then: no set can be bound with its view any more, and no submission that bound one is still running. Its key is
//    held then: a held key makes no entry in the pool refresh (held_out), so the slot keeps its stand-in; a mark on the
//    slot drops the hold and the next refresh makes the texture again, as does any other lookup.
// An image freed at the eviction itself, as before, faulted the GPU once (a read of freed memory in a pass, 0.6.1.14)
// without a path to it found in the code; the delay leaves no window in the pool sets, and the cancels count uses the
// marks missed. NX2NAVI_TEX_DISUSE=2 retires and never destroys (stand-ins only, nothing freed): it tells the marks and
// stand-ins from the freeing as the cause of a fault. Only while every module that indexes the pool was marked and two
// reads arrived: an unmarked shader could sample what the marks call unused.
// Two minutes, not the thirty seconds it was: a texture out of view that long and then seen again shows its stand-in
// for the frames until its mark is read, which is a flash of missing ground or a dark object each time the camera
// turns back, and a batch of such textures gave back 0.4 to 31 MiB in a recorded session. The textures of an area
// left behind, which are what the memory grows by, still go two minutes after it.
#define DISUSE_SECONDS 120
#define DISUSE_FRAMES PRESENTS(DISUSE_SECONDS * 60)
#define HOLDS_MAX 65536
static nvn_hmap holds = {.klen = sizeof(TexKey)};   // value: 1
static uint32_t marks_reads;
// Whether the scene is being drawn, from the marks: marks_peak is the most slots one read marked lately (in 1/256,
// halving in about a minute of reads), marks_live the reads in a row that marked at least a quarter of it. A pause
// screen, a menu or a fade draws next to nothing through the pool for longer than DISUSE_SECONDS: every texture of the
// scene behind it then counted as unsampled, went under memory pressure, and the scene came back as stand-ins.
// evict_disused waits for two such reads in a row: the first one after a pause is what notes the scene's textures as
// sampled again.
static uint64_t marks_peak;
static uint32_t marks_live;
static int resolve_held;            // slot_resolve left the slot out for a held key or a retired entry
static uint64_t n_disuse, n_disuse_bytes, n_disuse_back;   // since the last perf line
// since the last perf line: eviction candidates kept because another descriptor is registered at their address
// (shared_address), and holds dropped by a mark that another entry at the address had taken (vk_pool_marks_read)
static uint64_t n_disuse_shared, n_disuse_diverted;
static uint64_t n_retire_cancel, n_retire_freed;           // since the last perf line
static uint64_t n_retire_stalled;                          // retired textures taken back because they never came due
#define RETIRE_PATIENCE PRESENTS(5 * 60)
static int disuse_logs;
static Tex **retiring;              // retired entries not destroyed yet
static int nretiring, cretiring;
static uint64_t retiring_bytes;

// Takes a retired entry back as it is: something uses it. Its slots resolve to it again (the bump).
static void retire_cancel(Tex *e) {
    e->retiring = 0;   // it leaves retiring[] at the next retire_sweep
    retiring_bytes -= e->bytes;
    if (holds.n) nvn_hm_del(&holds, &e->key);
    n_retire_cancel++, n_disuse_back++;
    // In use as of now: while retired the pool refresh left it out, so it carries no use stamp of that time and
    // counted as idle the moment it came back. A sweep over the budget (sweep_pressure calls retire_sweep and then
    // evict_idle) could evict it in the same call that took it back.
    e->use_round = look_round;
    e->use_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    tex_gen_bump(e->key.addr);
}

static void retire_remove(Tex *e) {   // destroy_tex: the entry goes another way (released and dropped, shutdown)
    for (int i = 0; i < nretiring; i++)
        if (retiring[i] == e) { retiring[i] = retiring[--nretiring]; break; }
    if (e->retiring) e->retiring = 0, retiring_bytes -= e->bytes;
}

// Each submission: destroys the retired entries the pool sets and the GPU are done with (above). TEX_DISUSE=2 keeps them.
static void retire_sweep(void) {
    if (!nretiring) return;
    static int keep = -1;
    if (keep < 0) keep = rt_knob("TEX_DISUSE", "1") == 2;
    uint64_t takes = vk_pool_takes(), done = vk_sync_fence_done();
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    int k = 0;
    for (int i = 0; i < nretiring; i++) {
        Tex *e = retiring[i];
        if (!e->retiring) continue;   // taken back
        if (!e->retire_due && takes - e->retire_takes > VK_POOL_SETS) e->retire_due = vk_sync_issued();
        // The takes that count are those of syncs that left no slot behind, and a PC that has no creation time over
        // in most frames leaves some behind in nearly every sync: there a retired texture was never destroyed and its
        // slots showed the stand-in for as long as that lasted, minutes in one report. One that is still not due
        // after RETIRE_PATIENCE presents is taken back and counts as sampled now.
        if (!e->retire_due && now - e->retire_frame > RETIRE_PATIENCE) {
            retire_cancel(e);
            e->samp_frame = now;
            n_retire_stalled++;
            continue;
        }
        if (!keep && e->retire_due && done >= e->retire_due) {
            e->retiring = 0, retiring_bytes -= e->bytes;
            nvn_hm_put(&holds, &e->key, (void *)(uintptr_t)1);
            n_retire_freed++;
            drop_tex(e, DROP_DISUSE);
            continue;
        }
        retiring[k++] = e;
    }
    nretiring = k;
    static uint64_t stalled_logged;
    static int stalled_logs;
    if (n_retire_stalled != stalled_logged && stalled_logs < 8) {
        stalled_logs++;
        rt_log("vk: %llu textures evicted as unsampled were taken back: the texture pool had not been brought up to "
               "date completely in %d s, so they could not be destroyed and their slots showed stand-ins",
               (unsigned long long)(n_retire_stalled - stalled_logged), 5);
        stalled_logged = n_retire_stalled;
    }
}

// A lookup of key k that found no entry: 1 when the pool refresh asks for a held key (it gets the stand-in). Any other
// lookup ends the hold: the game uses the texture.
static int held_out(const TexKey *k) {
    if (!holds.n || !nvn_hm_get(&holds, k)) return 0;
    if (pool_ctx) return 1;
    nvn_hm_del(&holds, k);
    n_disuse_back++;
    return 0;
}

// tex_lock held. Whether a registered slot at e's address names another descriptor than e's: the same bytes through
// another swizzle or format, or a view the game draws through. Such a texture is not retired by disuse. Its slots
// do not all come back by the marks: a slot of the other descriptor that followed e gets an image of its own once
// e is gone, an empty one when the descriptor is not a CPU-written one, and from then on that image takes the
// marks of every slot at the address, so the held key of e was never dropped and the slots that named e showed
// the stand-in for good: the ground of Hyrule Field and of Lon Lon Ranch drawn flat and see-through after 6,663
// textures went under low commit and 928 came back.
static int slot_names_other(int32_t id, const Tex *e) {
    const TexSlot *s = &tslots[id];
    if (!s->used || s->finalized || (uintptr_t)s->t.mem != e->key.addr) return 0;
    TexKey sk;
    make_key(&s->t, &sk);
    return memcmp(&sk, &e->key, sizeof sk) != 0;
}

static int shared_address(const Tex *e) {
    if (!sinfo || !slot_next) return 0;
    uint32_t b = agen_idx(e->key.addr);
    for (int32_t id = bucket_head[b]; id >= 0; id = slot_next[id])
        if (slot_names_other(id, e)) return 1;
    for (int32_t g = bucket_ghead[b]; g >= 0; g = grp[g].gnext)
        for (int32_t id = grp[g].pub.head; id >= 0; id = slot_next[id])
            if (slot_names_other(id, e)) return 1;
    return 0;
}

// NX2NAVI_TEX_DISUSE_EVICT (default 0; 1 = textures no shader sampled for a while are retired under pressure, as
// before). Off since players' ground went flat and see-through after such evictions and stayed so (Hyrule Field, Lon
// Lon Ranch; 1,441 retired on arriving in a village, 82 back): with the eviction off the same play kept its ground,
// and neither of the two holes closed in the way back (shared_address, the hold drop in vk_pool_marks_read) acted
// in the session that still lost it. What takes the retired texture's place for good is not known yet; a draw that
// samples the stand-in once into a picture the game keeps would do it, and no mark can undo that. The shaders keep
// their marks (NX2NAVI_TEX_DISUSE), so turning this on changes no pipeline. Released textures and the idle ones the
// game no longer names still give way under pressure.
static int disuse_evict_on(void) {
    static int on = -1;
    if (on < 0) {
        on = rt_knob("TEX_DISUSE_EVICT", "0") == 1;
        rt_log("vk: textures no shader sampled for a while %s under pressure (NX2NAVI_TEX_DISUSE_EVICT)",
               on ? "are retired" : "stay");
    }
    return on;
}

// tex_lock held; returns the bytes retired (freed a few submissions later), counting those still retiring as freed
static uint64_t evict_disused(uint64_t want) {
    if (!disuse_evict_on()) return 0;
    if (!want || !vk_pool_marks_enabled() || __atomic_load_n(&vk_pool_marks_missed, __ATOMIC_RELAXED) || marks_reads < 2 ||
        marks_live < 2 || vk_sync_fence_done() < evict_wait)
        return 0;
    if (retiring_bytes >= want) return retiring_bytes;   // what is on its way out covers it
    want -= retiring_bytes;
    Tex *v[EVICT_STEP];
    int nv = 0;
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), disuse = DISUSE_FRAMES;
    for (uint32_t i = 0; i < by_key.cap; i++) {   // the EVICT_STEP least recently sampled candidates, oldest first
        Tex *e = by_key.vals[i];
        if (!e || e->released || e->retiring || !e->bytes || !comes_back_intact(e) || now - e->samp_frame < disuse)
            continue;
        int at = nv;
        while (at > 0 && v[at - 1]->samp_frame > e->samp_frame) at--;
        if (at == EVICT_STEP) continue;
        if (nv < EVICT_STEP) nv++;
        memmove(v + at + 1, v + at, sizeof *v * (size_t)(nv - 1 - at));
        v[at] = e;
    }
    // A candidate whose address another descriptor shares stays (shared_address), and counts as sampled now, so the
    // walk over its bucket is made once per disuse time and the next candidates get their turn.
    int kept = 0;
    for (int i = 0; i < nv; i++) {
        if (shared_address(v[i])) v[i]->samp_frame = now, n_disuse_shared++;
        else v[kept++] = v[i];
    }
    nv = kept;
    if (!nv) return 0;
    if (holds.n + (uint32_t)nv > HOLDS_MAX) {
        // keys of textures the game has let go of pile up: keep those a slot still holds out (cheld). Dropping them all
        // would make the pool refresh create every evicted texture again while memory is short.
        nvn_hm_clear(&holds, NULL);
        for (int id = 0; id < ntslots; id++) {
            if (!tslots[id].used || !tslots[id].cheld) continue;
            TexKey k;
            make_key(&tslots[id].t, &k);
            nvn_hm_put(&holds, &k, (void *)(uintptr_t)1);
        }
        rt_log("vk: holds of textures evicted by disuse past %d: kept the %u that registered slots still name", HOLDS_MAX,
               holds.n);
    }
    uint64_t freed = 0;
    int n = 0;
    uint64_t takes = vk_pool_takes();
    for (; n < nv && freed < want; n++) {   // retire: the pool refresh gives their slots the stand-in from now on
        Tex *e = v[n];
        if (nretiring == cretiring) {
            int c = cretiring ? cretiring * 2 : 256;
            Tex **r = realloc(retiring, sizeof *r * (size_t)c);
            if (!r) break;
            retiring = r, cretiring = c;
        }
        retiring[nretiring++] = e;
        e->retiring = 1, e->retire_takes = takes, e->retire_due = 0, e->retire_frame = now;
        pool_unlist(e);   // no slot resolves to it in the pool refresh; taken back, the refill of its slots lists it again
        retiring_bytes += e->bytes;
        freed += e->bytes;
        tex_gen_bump(e->key.addr);
    }
    n_disuse += (uint64_t)n, n_disuse_bytes += freed;
    evict_wait = vk_sync_issued() + 2;
    if (disuse_logs < 8 || ((rt_trace & RT_TRACE_PERF) && disuse_logs < 64)) {
        disuse_logs++;
        rt_log("vk: %d textures no shader sampled for %d s or more evicted (%.1f MiB), least recently sampled first; their "
               "images go once the texture pool sets no longer name them, and one sampled again is made again within a "
               "few frames", n, DISUSE_SECONDS, freed / 1048576.0);
    }
    return retiring_bytes;
}

// The registered slots at e's address, for marks_check's line: how many, those of e's own descriptor ("f": finalized),
// those whose last lookup gave e (a slot of another descriptor among them follows e), and the other descriptors' with
// format and flags; up to 8 ids of each.
static void slot_note(int32_t id, const Tex *e, int *n, int *n_own, int *n_show, int *n_other, char *own, char *show,
                      char *other, size_t own_cap, size_t other_cap) {
    const TexSlot *s = &tslots[id];
    if (!s->used || (uintptr_t)s->t.mem != e->key.addr) return;
    ++*n;
    TexKey sk;
    make_key(&s->t, &sk);
    size_t at;
    if (!memcmp(&sk, &e->key, sizeof sk)) {
        if (++*n_own <= 8 && (at = strlen(own)) < own_cap - 1)
            snprintf(own + at, own_cap - at, " %d%s", (int)id, s->finalized ? "f" : "");
    } else if (++*n_other <= 8 && (at = strlen(other)) < other_cap - 1) {
        snprintf(other + at, other_cap - at, " %d (fmt %#x flags %#x%s)", (int)id, s->t.d.format, (unsigned)s->t.d.flags,
                 s->finalized ? ", finalized" : "");
    }
    if (s->ce == e && s->cgen == agen[sinfo[id] & 0xffff] && ++*n_show <= 8 && (at = strlen(show)) < own_cap - 1)
        snprintf(show + at, own_cap - at, " %d", (int)id);
}

static void slots_text(char *out, size_t n, const Tex *e) {
    char own[112] = "", show[112] = "", other[360] = "";
    int total = 0, n_own = 0, n_show = 0, n_other = 0;
    uint32_t b = agen_idx(e->key.addr);
    for (int32_t id = bucket_head[b]; id >= 0; id = slot_next[id])
        slot_note(id, e, &total, &n_own, &n_show, &n_other, own, show, other, sizeof own, sizeof other);
    for (int32_t g = bucket_ghead[b]; g >= 0; g = grp[g].gnext)
        for (int32_t id = grp[g].pub.head; id >= 0; id = slot_next[id])
            slot_note(id, e, &total, &n_own, &n_show, &n_other, own, show, other, sizeof own, sizeof other);
    snprintf(out, n, "%d registered; %d of this descriptor:%s; %d showing this entry:%s; %d of other descriptors:%s", total,
             n_own, n_own ? own : " none", n_show, n_show ? show : " none", n_other, n_other ? other : " none");
}

// NX2NAVI_TRACE=perf, from vk_pool_marks_read: a shader sampled slot id, and nothing has ever put content into e, the
// entry the slot shows or one credited with the mark (no_content): the picture drawn through it is an empty or
// undefined image, which nothing else reports. One line per entry and up to MARK_LINES per address (said_at); the
// marked slots that met such an entry are counted in the texture origins line whether or not a line was written.
static uint64_t n_mark_empty, n_mark_none;   // since the last perf line: marked slots that met such an entry; that show none
#define MARK_LINES 6
#define MARK_LINES_ALL 20000
static void marks_check(Tex *e, uint32_t id, uint64_t now, int followed) {
    if (!no_content(e)) return;
    // an entry the CPU writes is uploaded by its first check, which one just made (again) has not had yet
    if (e->pub.mirrored && now - e->og_frame < 4) return;
    n_mark_empty++;
    if (e->said) return;
    e->said = 1;
    static SaidAt said;
    static uint32_t total;
    if (total >= MARK_LINES_ALL || !said_at(&said, e->key.addr, MARK_LINES)) return;
    total++;
    char d[224], og[320], state[160], slots[640], others[640];
    desc_text(d, sizeof d, e);
    origin_text(og, sizeof og, e);
    slots_text(slots, sizeof slots, e);
    others_at(others, sizeof others, e, NULL);
    if (e->pub.mirrored)
        snprintf(state, sizeof state, "CPU-written, not uploaded%s",
                 e->up_retry ? " (an upload that had no memory waits for its next try)" : "");
    else
        snprintf(state, sizeof state, "not CPU-written (%s), never drawn into, copied into or filled", mirror_refused(&e->t));
    rt_log("perf: frame %llu: sampled without content: texture %#llx (%llu bytes) [%s]: %s; %s; marked slot %u%s; slots "
           "at the address: %s; other descriptors there: %s", (unsigned long long)now, (unsigned long long)e->key.addr,
           (unsigned long long)e->t.L.size, d, state, og, id, followed ? " (of another descriptor, credited to this entry)" : "",
           slots, others);
}

void vk_pool_marks_read(const uint32_t *marks, uint32_t n) {
    lock();
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    int trace = (rt_trace & RT_TRACE_PERF) != 0;
    if (n > (uint32_t)ntslots) n = (uint32_t)ntslots;
    uint32_t marked = 0;
    for (uint32_t id = 0; id < n; id++) {
        if (!marks[id] || !tslots[id].used) continue;
        marked++;
        TexSlot *s = &tslots[id];
        if (s->ce && s->cgen == agen[sinfo[id] & 0xffff]) {   // what the slot resolves to now
            if (s->ce->retiring) retire_cancel(s->ce);   // not expected: retiring bumps the slot's generation
            s->ce->samp_frame = now;
            if (trace) marks_check(s->ce, id, now, 0);
            continue;
        }
        if (trace && !s->ce && s->cgen == agen[sinfo[id] & 0xffff]) n_mark_none++;   // its last lookup gave no entry
        TexKey k;
        make_key(&s->t, &k);
        Tex *e = nvn_hm_get(&by_key, &k);
        if (e && !e->released) {
            if (e->retiring) retire_cancel(e);   // retired, and its stand-in was sampled: back as it is
            e->samp_frame = now;
            if (trace) marks_check(e, id, now, 0);
            continue;
        }
        // The slot has no entry of its own: it samples another descriptor's image at its address (slot_resolve follows
        // one of the same target, as a second descriptor with another swizzle does). That image is what was sampled.
        // With the mark given to the slot's own key alone, a texture sampled only through such slots was never noted
        // as sampled while the slots' cached resolution was out of date, which it is at most reads in a bucket the
        // game keeps changing: it was retired as unused after two minutes, and a mark on its stand-in could not take
        // it back. While the pool's syncs kept leaving slots behind, a retired texture was never destroyed either
        // (retire_sweep waits for complete takes), so its slots kept the stand-in: the ground of the field drawn
        // without its textures and its height.
        int followed = 0;
        for (Tex *p = nvn_hm_get(&by_addr, &k.addr); p; p = p->next_at) {
            if (p->released || p->key.d.target != k.d.target) continue;
            if (p->retiring) retire_cancel(p);
            p->samp_frame = now;
            followed = 1;
            if (trace) marks_check(p, id, now, 1);
        }
        // The slot's own key may be held although an entry at its address took the mark: the slot shows the stand-in
        // then (slot_resolve follows fewer entries than the loop above credits), and only this drop makes its texture
        // again. Left to the case where the loop found nothing, such a slot kept the stand-in for good.
        if (holds.n && nvn_hm_del(&holds, &k)) {   // the stand-in was sampled: the next pool refresh makes it
            n_disuse_back++;
            n_disuse_diverted += followed != 0;
            s->cheld = 0;
            s->cwant = 1;   // on screen as a stand-in: made at the next refresh, whatever the frame's creation time
            tex_gen_bump(k.addr);
        }
    }
    marks_reads++;
    marks_peak -= marks_peak >> 14;
    if ((uint64_t)marked << 8 > marks_peak) marks_peak = (uint64_t)marked << 8;
    if (marked >= 16 && ((uint64_t)marked << 10) >= marks_peak) marks_live += marks_live < 1000;
    else marks_live = 0;
    unlock();
}

// NX2NAVI_TEX_COMMIT_EVICT (default on; 0 = off): while the commit is low (commit_is_low), idle live textures go as
// under VRAM pressure (evict_idle), COMMIT_EVICT_STEP at a time, once the released entries gave what they could: they
// are down to COMMIT_LOW_KEEP, or drop_stale found less than a full step to drop. The driver charges device memory to
// the commit too, and the eviction above starts only at NX2NAVI_TEX_VRAM_PRESSURE of the device's budget: on a 16-32 GB
// card the textures of every area visited stayed until the commit ran out (27-37 GB processes, then a failed
// allocation or a device lost, #177 #203 #215), as nothing else gave them back while they stayed below that share of
// the video memory. Waiting for the released entries to be down to COMMIT_LOW_KEEP, as before, never ended where the
// game releases and takes back 200-300 textures a frame: 2,048 released entries held 1.4-2.0 GiB, none of them stale,
// and with the commit low for minutes no live texture went (a 42 GB process). The entries the game takes back stay
// either way: drop_stale leaves them, and the evictions below take live textures only.
#define COMMIT_EVICT_STEP ((uint64_t)256 << 20)
static int commit_evict_on = -1;
static void commit_evict(void) {   // tex_lock held, the commit is low and the released entries gave what they could
    if (commit_evict_on < 0) commit_evict_on = rt_knob("TEX_COMMIT_EVICT", "1") == 1;
    if (!commit_evict_on || !tex_evict) return;
    if (rel_bytes > COMMIT_LOW_KEEP) {
        static int logs;
        if (!n_commit_past++ && logs < 4) {
            logs++;
            rt_log("vk: commit low: the %d released textures keep %.0f MiB the game still takes back or looks up; "
                   "live textures nothing sampled go without waiting for them", nreleased, rel_bytes / 1048576.0);
        }
    }
    uint64_t used = 0, budget = 0;
    vk_mem_vram_live(&used, &budget);
    uint64_t freed = evict_idle(COMMIT_EVICT_STEP, used, budget, DROP_IDLE_COMMIT);
    if (freed < COMMIT_EVICT_STEP) evict_disused(COMMIT_EVICT_STEP - freed);
}

static void sweep_pressure(void) {   // tex_lock held
    if (vram_pressure < 0) {
        const char *s = cfg_get("NX2NAVI_TEX_VRAM_PRESSURE");
        vram_pressure = s && *s ? atoi(s) : VRAM_PRESSURE_DEFAULT;
        if (vram_pressure < 0 || vram_pressure > 100) vram_pressure = VRAM_PRESSURE_DEFAULT;
        tex_evict = rt_knob("TEX_EVICT", "1");
        rt_log("vk: textures under VRAM pressure: %s%d%%%s", vram_pressure ? "dropped above " : "off", vram_pressure,
               !vram_pressure ? "" : tex_evict ? ", released ones first, then idle live ones (NX2NAVI_TEX_EVICT=0: released only)"
                                               : ", released ones only (NX2NAVI_TEX_EVICT=0)");
        if (vram_pressure > 87)
            rt_log("vk: NX2NAVI_TEX_VRAM_PRESSURE=%d is above the seven eighths of the budget past which a new VRAM "
                   "mirror is refused: textures make room only once a mirror was refused", vram_pressure);
    }
    retire_sweep();
    if (submit_epoch & 7) return;   // the budget and commit queries every 8 submits
    uint64_t used, budget;
    int exact, dropped = 0;
    uint64_t wanted = tex_evict ? (uint64_t)plat_xchg64(&room_wanted, 0) : 0;
    int known = vram_pressure && (rel_head || tex_evict || wanted) &&
                (tex_evict ? vk_mem_vram_live(&used, &budget) : vk_mem_vram(&used, &budget, &exact));
    uint64_t over = 0;
    if (known) {
        uint64_t limit = budget / 100 * (uint64_t)vram_pressure;
        if (used > limit) over = used - limit + budget / 50;
        if (wanted) {   // the use at which the refused mirror fits; the lowest of the requests that stand
            uint64_t level = used > wanted ? used - wanted : 0;
            if (!room_checks || level < room_level) room_level = level;
            room_checks = ROOM_CHECKS;
        }
        if (room_checks && used <= room_level) room_checks = 0;   // made: the pool gets its mirror when it asks again
        if (room_checks) {
            room_checks--;
            if (used - room_level > over) over = used - room_level;
        }
    }
    if (over) {
        // Released entries that come back intact, by the rule drop_stale keeps: released STALE_FRAMES ago and reached by
        // no lookup since (idle). The game releases and takes back hundreds of textures a frame in towns, and the
        // release list is mostly those: dropped, each was made and uploaded again within frames and freed nothing.
        // With the budget taken as 7.5 GiB (NX2NAVI_VRAM_BUDGET_MB), teleports from Hyrule Field through Kakariko
        // Village, the Graveyard, Death Mountain Trail and Kokiri Forest dropped 82,288 under the pressure and the card
        // stayed over its budget throughout; with this rule 34,699 went, a fifth fewer textures were made again, the
        // render thread waited a third less on textures, and 4 draws met a texture gone where 11 had.
        uint64_t freed = 0, now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        for (Tex *e = rel_head, *next; e && dropped < PRESSURE_STEP && now - e->rel_frame >= STALE_FRAMES; e = next) {
            next = e->rel_next;
            if (!comes_back_intact(e) || !idle(e, now)) continue;
            freed += e->bytes;
            drop_tex(e, DROP_VRAM);
            dropped++, n_pressure_drops++;
        }
        if (tex_evict && freed < over) freed += evict_idle(over - freed, used, budget, DROP_IDLE_VRAM);
        if (tex_evict && freed < over) evict_disused(over - freed);
    }
    int low = commit_is_low(), more = 0;   // more: the released entries have stale ones left for the next check
    int held = low && commit_drops_held();   // the commit drops came back (commit_back_note)
    if (rel_head) {
        drop_overwritten();
        uint64_t cap = released_cap_bytes();
        if (rel_bytes > cap) drop_stale(cap, &n_cap_drops, DROP_CAP);
        if (rel_bytes > COMMIT_LOW_KEEP && low && !held)
            more = drop_stale(COMMIT_LOW_KEEP, &n_commit_drops, DROP_COMMIT) == PRESSURE_STEP;
    }
    if (low && !more && !held) commit_evict();
}

// ---------------------------------------------------------------- per-frame texture work (vk.h VK_TS_*)
// Sums for the frame being made, by the render and replay threads (atomics: both may add at once). vk_texstat_frame,
// called at each present (perf.c ft_frame), takes them; a frame whose texture work (creation, upload and upload flush)
// reaches NX2NAVI_TEXUP_LOG_MS gets a "texup:" line, so an area change's hitch shows where its time went (default 0:
// no lines; 16 is a useful value).
static volatile int64_t ts_ns[VK_TS_N], ts_n[VK_TS_N], ts_bytes[VK_TS_N];
static int ts_log_ms = -1;

static int ts_thread(void) {
    uint32_t tid = plat_thread_id();
    return tid == prof_render_tid || tid == prof_replay_tid;
}

void vk_texstat_time(int kind, uint64_t t0) {
    if (!t0 || !ts_thread()) return;
    plat_add64(&ts_ns[kind], (int64_t)(kern_now_ns() - t0));
    plat_inc64(&ts_n[kind]);
}

void vk_texstat_add(int kind, uint64_t n, uint64_t bytes) {
    if (!ts_thread()) return;
    plat_add64(&ts_n[kind], (int64_t)n);
    plat_add64(&ts_bytes[kind], (int64_t)bytes);
}

void vk_texstat_frame(uint64_t frame, uint64_t d) {
    if (ts_log_ms < 0) {
        ts_log_ms = cfg_int("NX2NAVI_TEXUP_LOG_MS");
        if (ts_log_ms < 0) ts_log_ms = 0;
    }
    int64_t ns[VK_TS_N], n[VK_TS_N], b[VK_TS_N];
    for (int k = 0; k < VK_TS_N; k++)
        ns[k] = plat_xchg64(&ts_ns[k], 0), n[k] = plat_xchg64(&ts_n[k], 0), b[k] = plat_xchg64(&ts_bytes[k], 0);
    double work = (ns[VK_TS_CREATE] + ns[VK_TS_UPLOAD] + ns[VK_TS_FLUSH]) / 1e6;
    static int32_t n_lines;
    if (!ts_log_ms || work < ts_log_ms || ++n_lines > 400) return;
    rt_log("texup: frame %llu: %.1f ms frame, texture work %.1f ms; created %lld in %.1f ms; uploads %lld (%.1f MiB) in "
           "%.1f ms: staging %.1f ms (%lld new buffers, %.1f MiB), storage copy %.1f ms, ASTC record %.1f ms (%lld "
           "scratch buffers made in %.1f ms); upload flush %lld in %.1f ms; new device memory of 32 MiB or more %lld "
           "(%.0f MiB)",
           (unsigned long long)frame, d / 1e6, work, (long long)n[VK_TS_CREATE], ns[VK_TS_CREATE] / 1e6,
           (long long)n[VK_TS_UPLOAD], b[VK_TS_UPLOAD] / 1048576.0, ns[VK_TS_UPLOAD] / 1e6, ns[VK_TS_STAGE] / 1e6,
           (long long)n[VK_TS_STG_NEW], b[VK_TS_STG_NEW] / 1048576.0, ns[VK_TS_COPY] / 1e6, ns[VK_TS_ASTC] / 1e6,
           (long long)n[VK_TS_SCRATCH], ns[VK_TS_SCRATCH] / 1e6, (long long)n[VK_TS_FLUSH], ns[VK_TS_FLUSH] / 1e6,
           (long long)n[VK_TS_DEVMEM], b[VK_TS_DEVMEM] / 1048576.0);
}

// ---------------------------------------------------------------- uploads
static uint32_t texel_alpha_one(int format, uint32_t *elem) {
    switch (format) {
    case 0x19: *elem = 1; return 0xFF;
    case 0x1A: *elem = 1; return 0x7F;
    case 0x1B: case 0x1C: *elem = 1; return 1;
    case 0x1D: *elem = 2; return 0x3C00;
    case 0x1E: *elem = 2; return 0xFFFF;
    case 0x1F: *elem = 2; return 0x7FFF;
    case 0x20: case 0x21: *elem = 2; return 1;
    case 0x22: *elem = 4; return 0x3F800000;
    default: *elem = 4; return 1;   // 0x23, 0x24
    }
}

// 3-component texels to 4 (n texels).
static void expand_rgb(int format, const uint8_t *src, uint8_t *dst, size_t n) {
    uint32_t e, one = texel_alpha_one(format, &e);
    for (size_t i = 0; i < n; i++, src += 3 * e, dst += 4 * e) {
        memcpy(dst, src, 3 * e);
        memcpy(dst + 3 * e, &one, e);
    }
}

static void shrink_rgb(int format, const uint8_t *src, uint8_t *dst, size_t n) {
    uint32_t e;
    texel_alpha_one(format, &e);
    for (size_t i = 0; i < n; i++, src += 4 * e, dst += 3 * e) memcpy(dst, src, 3 * e);
}

// NX2NAVI_ASTC=check: the compute decoder's texels of n regions (gpu: the regions one after the other) against
// astc_decode_ldr of the same raw blocks (raw: the staging bytes the regions point into).
static void astc_check(const Tex *e, const uint8_t *raw, const VkAstcRegion *ar, int n, const uint8_t *gpu) {
    static volatile int32_t n_bad;
    static uint64_t n_tex, n_texels, n_diff_tex;
    uint32_t bw = e->f.astc_bw, bh = e->f.astc_bh;
    uint64_t diff = 0, first = UINT64_MAX;
    uint32_t want = 0, got = 0;
    int first_k = 0;
    for (int k = 0; k < n; k++) {
        const VkAstcRegion *r = &ar[k];
        size_t sb = (size_t)((r->w + bw - 1) / bw) * ((r->h + bh - 1) / bh) * 16, st = (size_t)r->w * r->h * 4;
        uint8_t *cpu = malloc(st);
        for (uint32_t z = 0; z < r->slices; z++, gpu += st) {
            astc_decode_ldr(raw + r->src + z * sb, r->w, r->h, bw, bh, e->f.astc_srgb, cpu);
            if (!memcmp(cpu, gpu, st)) continue;
            for (size_t i = 0; i < st / 4; i++)
                if (memcmp(cpu + 4 * i, gpu + 4 * i, 4)) {
                    if (first == UINT64_MAX) {
                        first = i, first_k = k;
                        memcpy(&want, cpu + 4 * i, 4), memcpy(&got, gpu + 4 * i, 4);
                    }
                    diff++;
                }
        }
        free(cpu);
        n_texels += (uint64_t)r->w * r->h * r->slices;
    }
    n_tex++;
    if (diff) {
        n_diff_tex++;
        log_limited(&n_bad, "ASTC check: texture %#llx fmt %#x %ux%u: %llu texels differ, first region %d texel %llu "
                    "cpu %08x gpu %08x", (unsigned long long)e->pub.addr, e->t.d.format, bw, bh,
                    (unsigned long long)diff, first_k, (unsigned long long)first, want, got);
    }
    if (n_tex == 1 || !(n_tex & 63) || diff)
        rt_log("vk: ASTC check: %llu textures, %llu texels compared, %llu textures differ", (unsigned long long)n_tex,
               (unsigned long long)n_texels, (unsigned long long)n_diff_tex);
}

// Staging bytes of one part of a texture upload decoded or copied on the CPU (see upload). With NX2NAVI_ASTC=cpu a
// large ASTC array decodes to RGBA8 in one go: a staging buffer of 676682336 bytes failed (-2) on an Intel UHD with
// 8 GB of shared memory and the game closed before the title screen.
#define UPLOAD_PART (32ull << 20)   // fits the staging pool (vk_staging takes up to half of its 64 MiB block)

// A texture of at most CHASH_MAX bytes keeps a hash of its storage as it was uploaded. A write to its pages then
// re-uploads it only when its own bytes changed: Ocarina's frames rewrite hundreds of small textures' pages (shared
// with data the CPU writes, or written with the same bytes), about 420 re-uploads a frame of 1.5 KiB each, and one
// costs a staging buffer, a copy and a barrier, several microseconds, where hashing 1.5 KiB takes a fraction of one.
// A CPU write during the hash sets the pages' write bits again, so the next check sees it.
enum { CHASH_MAX = 64 << 10 };
static uint64_t content_hash(const void *p, uint64_t n) {
    const uint8_t *b = p;
    uint64_t h = 0x9e3779b97f4a7c15ull ^ n, i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, b + i, 8);
        h = (h ^ w) * 0xbf58476d1ce4e5b9ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ b[i]) * 0x94d049bb133111ebull;
    return h ^ (h >> 31);
}

// ASTC for the GPU decoder without the CPU deswizzle: the storage goes into staging as the game laid it out, in one
// copy, and the decoder reads each block from its block-linear place (VkAstcRegion.bl, astc_decode.comp bl_block).
// The CPU deswizzle moved every 16 bytes through the GOB arithmetic and was most of an ASTC upload's CPU time when an
// area loads. Fills ar (n = levels * layers, level-major as upload's regions) and returns 1 when every level and layer
// qualifies: block-linear, starting 16-byte aligned, its layout inside the storage. NX2NAVI_ASTC_GPU_DESWIZZLE=0 keeps
// the CPU deswizzle. levels counts the image's levels: level lv of the image is the game's level lv + skip (texture
// detail), which is where its blocks lie and what its size is.
static int astc_bl_regions(const Tex *e, uint32_t levels, uint32_t layers, int bw, int bh, VkAstcRegion *ar) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2NAVI_ASTC_GPU_DESWIZZLE");
        if (!on) rt_log("vk: ASTC blocks deswizzled on the CPU (NX2NAVI_ASTC_GPU_DESWIZZLE=0)");
    }
    const nvn_texdesc *d = &e->t.d;
    if (!on || nvn_tex_is_linear(d)) return 0;
    uint32_t k = 0, sk = e->pub.skip;
    for (uint32_t lv = 0; lv < levels; lv++)
        for (uint32_t ly = 0; ly < layers; ly++) {
            uint32_t row_bytes, rows, slices, bpb;
            uint64_t base;
            unsigned th, td;
            if (nvn_deswizzle_geom(d, lv + sk, ly, &row_bytes, &rows, &slices, &base, &th, &td, &bpb) || (base & 15) ||
                th > 5 || td > 5)
                return 0;
            ar[k] = (VkAstcRegion){.src = base, .w = mip(d->width, lv + sk), .h = mip(d->height, lv + sk), .slices = slices,
                                   .level = lv, .layer = ly, .bl = 1, .th = (uint8_t)th, .td = (uint8_t)td};
            if (base + vk_astc_region_bytes(&ar[k], (uint32_t)bw, (uint32_t)bh) > e->t.L.size) return 0;
            k++;
        }
    return 1;
}

// NX2NAVI_ASTC_ARENA=1 (the default): the raw ASTC blocks for the GPU decoder stage in a
// range of the staging arena (vk_staging_at) like the other formats, not in a VkBuffer of their own. vk_astc_record
// reads its source only through a buffer copy at each region's src, so the range's offset is added to the regions. A
// buffer per texture cost a vmaCreateBuffer and, after the batch, a vmaDestroyBuffer each; an area load uploads
// thousands. The check mode keeps its own buffer (astc_check reads the blocks at the regions' src offsets). 0: a buffer
// per texture. With the kept scratch, a Kakariko walk at 1440p (2026-11-11) went from 342 device memory allocations
// and 6 frames over 50 ms to 233 and 3.
static int astc_arena(void) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2NAVI_ASTC_ARENA");
        rt_log("vk: ASTC blocks for the GPU decoder stage in %s (NX2NAVI_ASTC_ARENA=%d)",
               on ? "the staging arena" : "a buffer per texture", on);
    }
    return on;
}

// Copies the whole storage into the image (every level and layer the image has). An image made without its top levels
// (texture detail, VkTexImage.skip) has the game's level lv + skip at its level lv: the loops below run over the image's
// levels and read the storage at the game's.
// How long an upload whose decode had no memory waits for its next try (upload, up_wait): half a second of frames. A
// try costs the copy of the texture's blocks into staging memory, so not every frame.
#define UP_RETRY_FRAMES 30
static uint64_t n_up_retry;
uint64_t vk_texture_upload_retries(void) { return __atomic_load_n(&n_up_retry, __ATOMIC_RELAXED); }

// 1 while a texture that is not uploaded waits for its next try (e->up_retry; 0 for one never tried).
static int up_wait(const Tex *e) {
    return e->up_retry && __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) < e->up_retry;
}

// ASTC as BC1: e gets an image of the other format, made with the same parameters, in place of its own: BC1 (bc 2) for
// the opaque content of its first upload, BC3 (bc 1) for content with alpha about to go into its BC1 image. The old
// image and its views go once the GPU is past the submission being recorded, as a destroyed texture's do (destroy_tex),
// so commands recorded with it keep it; the new one starts as a new image (init_image) in the upload batch, which runs
// before the part being recorded (in cb, the batch upload has open, or its own), and the slots that resolve to e look
// it up again (tex_gen_bump). 0 when no image could be made; e keeps its own.
static int astc_reformat(Tex *e, VkCommandBuffer cb, int bc) {
    static volatile int32_t n_failed;
    VkCtx *c = vk_ctx();
    const ImgKey *k = &e->ik;
    int s = e->f.astc_srgb;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = k->flags,
                            .imageType = (VkImageType)k->type,
                            .format = bc == 2 ? (s ? VK_FORMAT_BC1_RGB_SRGB_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK)
                                              : (s ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK),
                            .extent = {k->w, k->h, k->d}, .mipLevels = k->levels, .arrayLayers = k->layers,
                            .samples = (VkSampleCountFlagBits)k->samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = k->usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    ImgKey nk;
    img_key_of(&ci, &nk);
    VmaAllocation a = NULL;
    VkImage img = suballoc_image(c, &ci, &a);
    if (!img && vma_image(c, &ci, &nk, &img, &a) != VK_SUCCESS) {
        log_limited(&n_failed, "ASTC texture %#llx (%ux%u): no %s image could be made", (unsigned long long)e->pub.addr,
                    e->t.d.width, e->t.d.height, bc == 2 ? "BC1" : "BC3");
        return 0;
    }
    vk_exec_image_gone(e->pub.image);
    vk_defer_image(e->pub.image, e->alloc);
    vk_exec_image_gone(img);   // views cached under the handle of a destroyed image (create_tex)
    int relink = e->ru_on;
    if (relink) ru_unlink(e);
    if (e->released) rel_bytes -= e->bytes;
    if (e->retiring) retiring_bytes -= e->bytes;
    VmaAllocationInfo own;
    vmaGetAllocationInfo(c->vma, a, &own);
    e->bytes = own.size;
    if (e->released) rel_bytes += e->bytes;
    if (e->retiring) retiring_bytes += e->bytes;
    e->pub.image = img, e->alloc = a, e->reused = 0;
    e->pub.format = e->f.vk = ci.format;
    e->f.astc_bc = (uint8_t)bc;
    e->ik = nk;
    if (relink) ru_link(e);
    if (cb) {
        init_image(e, cb);
    } else {
        init_image(e, vk_upload_begin());
        vk_upload_end();
    }
    tex_gen_bump(e->key.addr);
    if (bc == 2) n_bc1_made++, n_bc1_made_bytes += e->bytes;
    else n_bc1_widened++;
    return 1;
}

// The upload's copy of size bytes of ASTC storage into staging, with the opaque check of its blocks: 1 when every block
// is opaque and *decoded stays within ASTC_OPAQUE_DECODES. A large texture's copy and check run on the ASTC decode
// workers (tex_astc_mt.c astc_blocks_opaque_mt), a small one's here.
static uint64_t n_bc1_up_checks, n_bc1_up_bytes;
static int astc_copy_opaque(const Tex *e, uint8_t *dst, const uint8_t *src, uint64_t size, uint32_t *decoded) {
    uint64_t nb = size / 16;
    int ok = astc_blocks_opaque_mt(src, nb, e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb != 0, ASTC_OPAQUE_DECODES,
                                   decoded, dst);
    if (size % 16) memcpy(dst + 16 * nb, src + 16 * nb, size % 16);
    n_bc1_up_checks++, n_bc1_up_bytes += size;
    return ok;
}

static void upload(Tex *e) {
    static volatile int32_t n_bad, n_undecoded, n_split;
    int no_mem = 0;   // a part's ASTC decode had no memory: the texture is uploaded again later
    const tex_t *t = &e->t;
    const nvn_texdesc *d = &t->d;
    int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    if (!bpb || !t->mem) return;
    // A stale slot (the bindless pools look every slot up): no upload; a later write to mapped storage uploads it
    // (make_fresh_snap).
    if (!mem_readable(e->pub.addr, e->t.L.size)) {
        static volatile int32_t n_unmapped;
        log_limited(&n_unmapped, "texture %#llx (%llu bytes): storage not mapped; not uploaded",
                    (unsigned long long)e->pub.addr, (unsigned long long)e->t.L.size);
        e->uploaded = 1;
        return;
    }
    e->chash_ok = e->t.L.size <= CHASH_MAX;   // the hash of what this upload copies (make_fresh_snap compares it)
    if (e->chash_ok) {
        // read with the storage held mapped, as the copies below: the game unmaps a texture's memory on its own thread
        if (mem_read_hold(e->pub.addr, e->t.L.size)) {
            e->chash = content_hash((const void *)(uintptr_t)e->pub.addr, e->t.L.size);
            mem_read_release();
        } else {
            e->chash_ok = 0;
        }
    }
    static __thread char ctx[160];   // what this thread copies, for a crash report
    snprintf(ctx, sizeof ctx, "texture upload %#llx (%llu bytes, fmt %#x %ux%u)", (unsigned long long)e->pub.addr,
             (unsigned long long)e->t.L.size, d->format, d->width, d->height);
    uint64_t p0 = perf_begin(), u0 = kern_now_ns();
    int linear = nvn_tex_is_linear(d), is3d = d->target == NVN_TEXTURE_TARGET_3D, one_d = e->pub.type == VK_IMAGE_TYPE_1D;
    uint32_t levels = linear ? 1 : e->pub.levels, layers = linear ? 1 : e->pub.layers;
    int astc = e->f.astc_bw != 0, rgb = e->f.expand_rgb;
    int agpu = astc && vk_astc_gpu(), acheck = agpu && vk_astc_mode() == VK_ASTC_CHECK;   // raw blocks to the GPU
    uint32_t n = levels * layers, k = 0, sk = linear ? 0 : e->pub.skip;
    VkBufferImageCopy *rg = calloc(n, sizeof *rg);
    VkAstcRegion *ar = agpu ? calloc(n, sizeof *ar) : NULL;
    VkDeviceSize total = 0, decoded = 0;
    uint64_t src_bytes = 0;   // the storage bytes the image's levels take (all of the storage unless levels are left out)
    for (uint32_t lv = 0; lv < levels; lv++)
        for (uint32_t ly = 0; ly < layers; ly++) {
            uint32_t w = mip(d->width, lv + sk), h = one_d ? 1 : mip(d->height, lv + sk), sl = is3d ? mip(d->depth, lv + sk) : 1;
            uint64_t texels = (uint64_t)w * h * sl, raw = (uint64_t)(w + bw - 1) / bw * bpb * ((h + bh - 1) / bh) * sl;
            uint64_t out = agpu ? raw : astc ? texels * 4 : rgb ? texels * (uint64_t)bpb / 3 * 4 : raw;
            rg[k++] = (VkBufferImageCopy){.bufferOffset = total, .imageSubresource = {e->pub.aspect, lv, ly, 1},
                                          .imageExtent = {w, h, sl}};
            total += (out + 15) & ~15ull;
            decoded += texels * 4;
            src_bytes += raw;
        }
    if (!sk) src_bytes = t->L.size;
    VkBuffer sb, rbuf = VK_NULL_HANDLE;
    VmaAllocation ralloc = NULL;
    uint8_t *rmap = NULL, *raw_copy = NULL;
    int checked = 0, parts = 1;
    // The GPU decoder reads the blocks in place (astc_bl_regions); the check mode compares against tight blocks.
    int gdsw = agpu && !acheck && !one_d && astc_bl_regions(e, levels, layers, bw, bh, ar);
    // ASTC as BC1: a BC1 image takes opaque content only, and a BC3 one that may turn BC1 (bc1_try) does at its
    // first upload when the content is opaque (astc_reformat). The blocks the GPU decoder takes as they lie (gdsw) are
    // checked as they are copied; the others are checked here first. Without memory for a BC3 image the upload waits as
    // one without memory for its decode does (no_mem); without memory for a BC1 one the texture stays BC3.
    int bc1 = e->f.astc_bc == 2, try1 = e->bc1_try && e->f.astc_bc == 1 && !e->n_up, opq = 1;
    uint32_t bc1_dec = 0;
    e->bc1_try = 0;
    if ((bc1 || try1) && !gdsw) {
        opq = astc_opaque(t, e->pub.addr, &e->f);
        if (bc1 && !opq && !astc_reformat(e, VK_NULL_HANDLE, 1)) {
            e->uploaded = 0;
            e->up_retry = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + UP_RETRY_FRAMES;
            __atomic_add_fetch(&n_up_retry, 1, __ATOMIC_RELAXED);
            free(ar), free(rg);
            return;
        }
        if (try1 && opq) astc_reformat(e, VK_NULL_HANDLE, 2);
        bc1 = try1 = 0;
    }
    int check = bc1 || try1;
    // A CPU decode or copy stages at most UPLOAD_PART bytes per staging buffer (a region larger than that alone) and
    // records one copy per buffer; every part stays in the same upload batch, submitted as before. The GPU decoder
    // takes the raw blocks of every region from one buffer.
    for (uint32_t k0 = 0, k1; k0 < n; k0 = k1) {
        VkDeviceSize base = rg[k0].bufferOffset, size;
        for (k1 = k0 + 1; k1 < n && (agpu || (k1 + 1 < n ? rg[k1 + 1].bufferOffset : total) - base <= UPLOAD_PART);) k1++;
        size = (k1 < n ? rg[k1].bufferOffset : total) - base;
        if (gdsw) size = t->L.size;   // the storage as it is (one part: the GPU decoder takes every region at once)
        // Without its top levels, only the levels the image has: each region's blocks as they lie in the storage, one
        // after the other (the top level is three quarters of the bytes, and an array keeps each layer's levels
        // together, so the kept levels are no single range).
        if (gdsw && sk) {
            size = 0;
            for (k = 0; k < n; k++) size += vk_astc_region_bytes(&ar[k], (uint32_t)bw, (uint32_t)bh);
            src_bytes = size;
        }
        if (k0) parts++;
        VkCommandBuffer cb = vk_upload_begin();
        // The part stages in a range of the staging arena (vk_staging_at, its offset added to the regions below), or in
        // a buffer of its own at offset 0 when larger than an arena range; the GPU decoder takes one with
        // NX2NAVI_ASTC_ARENA=0 or in the check mode (astc_arena).
        VkDeviceSize soff = 0;
        uint64_t s0 = kern_now_ns();
        uint8_t *stg = agpu && (acheck || !astc_arena()) ? vk_staging(size, &sb, NULL) : vk_staging_at(size, &sb, &soff);
        vk_texstat_time(VK_TS_STAGE, s0);
        uint64_t c0 = kern_now_ns();   // the storage copies and decodes of this part (VK_TS_COPY)
        if (acheck) rmap = vk_staging(decoded, &rbuf, &ralloc);
        // The part's copies out of the storage, with it held mapped (mem_read_hold): the game releases a texture's
        // memory on its own thread, and a decommit in the middle of a copy faulted (tex_bl_copy). Only the copies run
        // held, so a waiting unmap and the guest's memory calls behind it never wait for the staging block or a decode.
        // Storage released since the check above uploads as zeros, as the game no longer has that texture.
        uint8_t **tmps = calloc(k1 - k0, sizeof *tmps);
        int held = mem_read_hold(e->pub.addr, e->t.L.size);
        if (held) plat_crash_context(ctx);
        else {
            static volatile int32_t n_gone;
            log_limited(&n_gone, "texture %#llx (%llu bytes): storage released during the upload; uploaded as zeros",
                        (unsigned long long)e->pub.addr, (unsigned long long)e->t.L.size);
            // the image holds zeros, not the bytes hashed above: the same bytes mapped back there upload again
            e->chash_ok = 0;
        }
        if (gdsw && !sk) {
            if (held && check) opq = astc_copy_opaque(e, stg, t->mem, size, &bc1_dec);
            else if (held) memcpy(stg, t->mem, size);
            else memset(stg, 0, size);
        } else if (gdsw) {
            VkDeviceSize at = 0;
            for (k = 0; k < n; k++) {   // astc_bl_regions checked that each lies inside the storage
                VkDeviceSize nb = vk_astc_region_bytes(&ar[k], (uint32_t)bw, (uint32_t)bh);
                if (held && check) opq &= astc_copy_opaque(e, stg + at, t->mem + ar[k].src, nb, &bc1_dec);
                else if (held) memcpy(stg + at, t->mem + ar[k].src, nb);
                else memset(stg + at, 0, nb);
                ar[k].src = at, at += nb;
            }
        }
        for (k = k0; k < k1 && !gdsw; k++) {
            uint32_t lv = rg[k].imageSubresource.mipLevel, ly = rg[k].imageSubresource.baseArrayLayer;
            uint32_t w = rg[k].imageExtent.width, h = rg[k].imageExtent.height, sl = rg[k].imageExtent.depth;
            uint32_t rb = (w + bw - 1) / bw * (uint32_t)bpb, rows = (h + bh - 1) / bh;
            size_t raw = (size_t)rb * rows * sl;
            rg[k].bufferOffset -= base;
            uint8_t *dst = stg + rg[k].bufferOffset, *tmp = (astc && !agpu) || rgb ? malloc(raw) : NULL;
            tmps[k - k0] = tmp;
            if (!held) {
                memset(tmp ? tmp : dst, 0, raw);
            } else if (nvn_deswizzle(d, lv + sk, ly, t->mem, tmp ? tmp : dst, rb)) {
                log_limited(&n_bad, "texture %#llx fmt %#x target %d: level %u layer %u has no CPU layout; uploaded as zeros",
                            (unsigned long long)e->pub.addr, d->format, d->target, lv + sk, ly);
                memset(tmp ? tmp : dst, 0, raw);
            }
        }
        if (held) {
            plat_crash_context(NULL);
            mem_read_release();   // the part's reads of the storage are done
        }
        for (k = k0; k < k1 && !gdsw; k++) {   // the decodes, from the copies (gdsw: ar already names the blocks)
            uint32_t lv = rg[k].imageSubresource.mipLevel, ly = rg[k].imageSubresource.baseArrayLayer;
            uint32_t w = rg[k].imageExtent.width, h = rg[k].imageExtent.height, sl = rg[k].imageExtent.depth;
            uint32_t rb = (w + bw - 1) / bw * (uint32_t)bpb, rows = (h + bh - 1) / bh;
            uint8_t *dst = stg + rg[k].bufferOffset, *tmp = tmps[k - k0];
            if (agpu) {
                ar[k] = (VkAstcRegion){.src = rg[k].bufferOffset, .w = w, .h = h, .slices = sl, .level = lv, .layer = ly};
            } else if (astc) {
                uint64_t a0 = perf_begin();
                for (uint32_t z = 0; z < sl; z++)
                    astc_decode_ldr_mt(tmp + (size_t)z * rb * rows, w, h, e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb,
                                       dst + (size_t)z * w * h * 4);
                perf_end(PERF_ASTC, a0);
                rg[k].bufferRowLength = w, rg[k].bufferImageHeight = h;
            } else if (rgb) {
                expand_rgb(d->format, tmp, dst, (size_t)w * h * sl);
                rg[k].bufferRowLength = w, rg[k].bufferImageHeight = h;
            } else {
                rg[k].bufferRowLength = rb / (uint32_t)bpb * (uint32_t)bw, rg[k].bufferImageHeight = rows * (uint32_t)bh;
            }
            free(tmp);
        }
        free(tmps);
        vk_texstat_time(VK_TS_COPY, c0);
        if (acheck) {
            raw_copy = malloc(size);
            memcpy(raw_copy, stg, size);
        }
        barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (agpu) {
            for (k = 0; k < n; k++) ar[k].src += soff;   // a range of the arena (astc_arena); 0 in a buffer of its own
            uint64_t a0 = perf_begin();
            // content with alpha for a BC1 image: a BC3 one first, or no decode for now (-1); opaque content at a BC3
            // image's first upload: a BC1 one (it stays BC3 when none can be made)
            int widen_failed = 0;
            if (bc1 && !opq) widen_failed = !astc_reformat(e, cb, 1);
            else if (try1 && opq) astc_reformat(e, cb, 2);
            checked = widen_failed ? -1
                    : vk_astc_record(cb, sb, size, ar, (int)n, e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb, e->f.astc_bc,
                                     e->pub.image, e->pub.aspect, rbuf);
            perf_end(PERF_ASTC, a0);
            vk_texstat_time(VK_TS_ASTC, a0);
            if (checked <= 0) {
                log_limited(&n_undecoded, "ASTC texture %#llx (fmt %#x %ux%u) not decoded on the GPU: left as it was%s",
                            (unsigned long long)e->pub.addr, d->format, d->width, d->height,
                            checked < 0 ? " and tried again later (no memory for the decode)" : "");
                e->chash_ok = 0;   // the image does not hold the bytes hashed above: the next write uploads, equal or not
                no_mem |= checked < 0;
            }
        } else {
            for (k = k0; k < k1; k++) rg[k].bufferOffset += soff;
            vkCmdCopyBufferToImage(cb, sb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, k1 - k0, rg + k0);
        }
        barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
        vk_upload_end();
    }
    if (parts > 1)
        log_limited(&n_split, "texture %#llx fmt %#x %ux%u, %u levels, %u layers: %.1f MiB staged in %d parts",
                    (unsigned long long)e->pub.addr, d->format, d->width, d->height, levels, layers, total / 1048576.0,
                    parts);
    if (acheck) {   // waits for the GPU
        vk_upload_flush(1);
        if (checked > 0) {
            vmaInvalidateAllocation(vk_ctx()->vma, ralloc, 0, VK_WHOLE_SIZE);
            astc_check(e, raw_copy, ar, (int)n, rmap);
        }
        vmaDestroyBuffer(vk_ctx()->vma, rbuf, ralloc);
    }
    free(raw_copy);
    free(ar);
    free(rg);
    uint64_t dt = kern_now_ns() - u0;
    if (e->uploaded) {
        n_reuploads++, perf_count(PERF_CNT_TEX_REUP);
        n_split_refill += e->sfill != 0;
        ru_n++, ru_bytes += src_bytes, ru_ns += dt;
        if (e->wt_n) {
            ru_wt_n++, ru_wt_bytes += src_bytes, ru_wt_ns += dt;
            if (rt_trace & RT_TRACE_PERF)
                rt_log("perf: frame %llu: re-upload after %u WriteTexels: texture %#llx fmt %#x %ux%u, %.1f KiB, %.3f ms",
                       (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), e->wt_n,
                       (unsigned long long)e->pub.addr, d->format, d->width, d->height, src_bytes / 1024.0, dt / 1e6);
        }
    } else {
        n_uploads++, perf_count(PERF_CNT_TEX_UP);
    }
    flight_ev(e, e->uploaded ? FLIGHT_TEX_REUP : FLIGHT_TEX_UP, e->wt_n, (uint32_t)(dt / 1000));
    e->wt_n = 0;
    e->uploaded = 1;
    e->n_up++;

    // The decode had no memory (vk_astc_record: its scratch buffer failed while the video memory was used up), so the
    // image does not hold this content: a new image holds nothing, an older one what it held before. Counted as
    // uploaded, it stayed that way until the game next wrote the storage, which for most textures is never. It counts
    // as not uploaded instead, and make_fresh and make_fresh_snap upload it again UP_RETRY_FRAMES frames on, when
    // the evictions under memory pressure have had time to make room; again and again until one succeeds.
    if (no_mem) {
        e->uploaded = 0;
        e->up_retry = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + UP_RETRY_FRAMES;
        __atomic_add_fetch(&n_up_retry, 1, __ATOMIC_RELAXED);
    } else {
        e->up_retry = 0;
    }
    perf_end(PERF_TEX, p0);
    vk_texstat_time(VK_TS_UPLOAD, u0);
    vk_texstat_add(VK_TS_UPLOAD, 0, src_bytes);
}

// Whether e's storage fills no 4 KiB page of its own, so the write tracking can only see writes to pages it shares.
static int fills_no_page(const Tex *e) {
    return ((e->pub.addr + e->t.L.size) & ~0xfffull) <= ((e->pub.addr + 0xfff) & ~0xfffull);
}

// A texture that fills no page keeps a copy of its storage bytes from when a GPU write to it was recorded
// (vk_texture_gpu_written), up to SNAP_MAX bytes; wb_exact (NX2NAVI_WB_EXACT, default on) enables it.
enum { SNAP_MAX = 4096 };
static int wb_exact = -1;
static volatile int32_t n_kept;

// vk_mem_changed over the pages wholly inside e's storage (all of its pages when it fills none): a neighbour that shares
// the first or last page does not count as a write to e. A texture that fills no page shares all of its pages, so
// there a page write counts only when e's own bytes differ from the copy taken at the GPU write. NX2navi's Purah Pad
// camera renders the ID, screen rect and depth of what is under the reticle into 4x4 targets of 512 bytes, two
// slots used in turn, and the CPU reads them a frame later. One slot's rect target shares its page with data the CPU
// writes every frame: without the copy its renders were never written back, the game read a zero rect every other
// frame and the target's name label blinked. NX2NAVI_WB_EXACT=0 restores the page-only decision.
static int storage_rewritten(const Tex *e, uint32_t since, uint32_t *now) {
    uint64_t lo = (e->pub.addr + 0xfff) & ~0xfffull, hi = (e->pub.addr + e->t.L.size) & ~0xfffull;
    if (hi <= lo) lo = e->pub.addr, hi = e->pub.addr + e->t.L.size;
    int ch = vk_mem_changed(lo, hi - lo, since, now);
    if (ch != 1 || !since || !e->snap || memcmp(e->snap, (const void *)(uintptr_t)e->pub.addr, e->t.L.size)) return ch == 1;
    log_limited(&n_kept, "write-back of %#llx kept: fmt %#x %ux%u, %llu bytes; a neighbour wrote its page",
                (unsigned long long)e->pub.addr, e->t.d.format, e->t.d.width, e->t.d.height,
                (unsigned long long)e->t.L.size);
    return 0;
}

// The pages of an uploaded texture show a write (or are not tracked): whether its own bytes are still those it was
// uploaded with (upload, content_hash). WriteTexels since the upload always counts as a change.
static int same_content(const Tex *e) {
    static int on = -1;   // NX2NAVI_TEX_SAME=0: every write to the pages uploads again (diagnosis)
    if (on < 0) { const char *v = cfg_get("NX2NAVI_TEX_SAME"); on = !(v && !strcmp(v, "0")); }
    if (!on || !e->chash_ok || e->wt_n || e->t.L.size > CHASH_MAX) return 0;
    if (!mem_read_hold(e->pub.addr, e->t.L.size)) return 0;   // unmapped since: not the same; the upload deals with it
    uint64_t h = content_hash((const void *)(uintptr_t)e->pub.addr, e->t.L.size);
    mem_read_release();
    if (h != e->chash) return 0;
    ru_same_n++, ru_same_bytes += e->t.L.size;
    return 1;
}

// A mirrored texture is uploaded on first use and again when its storage pages carry a newer write generation. So is
// one split fill fills (from_storage), while it is not released: the game may be unmapping a released one's storage,
// and nothing keeps a slot from resolving to it as the stale rule does for a CPU-written one (slot_resolve).
static void make_fresh(Tex *e) {
    if (!from_storage(e) || e->pub.gpu_owned || (e->sfill && e->released)) return;
    uint64_t size = e->t.L.size;
    if (!e->uploaded) {
        if (up_wait(e)) return;
        vk_mem_changed(e->pub.addr, size, 0, &e->wgen);   // resets the write bits before the storage is read
        upload(e);
        return;
    }
    uint32_t now;
    int ch = vk_mem_changed(e->pub.addr, size, e->wgen, &now);
    e->wgen = now;
    e->last_ch = ch;
    if (ch && !same_content(e)) upload(e);   // 1 written, -1 not tracked: both upload again, unless the bytes match
}

// make_fresh for draw bindings: the page check answers from this submission's write-watch snapshot (vk_memory.c
// vk_mem_changed_snap) instead of a system call per texture. A write the snapshot of the texture's region missed (made
// after it, during this replay) keeps its write-watch bit and is caught by the next submission's snapshot.
static void make_fresh_snap(Tex *e) {
    if (!from_storage(e) || e->pub.gpu_owned || (e->sfill && e->released)) return;
    if (!e->uploaded && up_wait(e)) return;
    uint32_t now;
    n_checks++;
    int ch = vk_mem_changed_snap(e->pub.addr, e->t.L.size, e->uploaded ? e->wgen : 0, submit_epoch, &now);
    e->wgen = now;
    if (!e->uploaded) { upload(e); return; }
    e->last_ch = ch;
    if (ch && !same_content(e)) upload(e);
}

// ---------------------------------------------------------------- aliases (NX2NAVI_TEX_ALIAS)
// Bytes per block and block size of the image's VkFormat (ASTC decodes to RGBA8, RGB expands to RGBA); 0 when the
// VkFormat substitutes another one of unknown size.
static int img_block(const Tex *e, int *bw, int *bh) {
    if (e->f.astc_bc) { *bw = *bh = 4; return e->f.astc_bc == 2 ? 8 : 16; }   // BC1 or BC3
    if (e->f.astc_bw) { *bw = *bh = 1; return 4; }
    int b = nvn_fmt_bpb(e->t.d.format, bw, bh);
    if (e->f.expand_rgb) { *bw = *bh = 1; return b / 3 * 4; }
    return e->f.vk == e->f.ideal ? b : 0;
}

// The new entry e can take src's content texel for texel: colour, one sample, the same image type and storage layout,
// and image formats of one block size (0 with a reason in *why when not). Block-compressed images need the same shape;
// uncompressed ones take the region both cover (alias_copy). Ocarina moves render targets whose size follows an effect
// on screen, so a descriptor made over a live target's memory is often a few texels larger or smaller than it. The game
// reads the bytes there as its content (the same memory on the console); started empty instead, the scene went black
// for a frame about once a second while the camera turned, from 0.6.1 on (issue #253).
static int alias_compatible(const Tex *e, const Tex *src, const char **why) {
    const VkTexImage *p = &e->pub, *q = &src->pub;
    if (e->f.depth || e->f.stencil || src->f.depth || src->f.stencil || p->samples > 1 || q->samples > 1)
        return *why = "depth, stencil or multisample", 0;
    if (p->type != q->type) return *why = "another image type", 0;
    if (p->skip || q->skip) return *why = "made without its top levels (texture detail)", 0;   // level i is not level i
    if (nvn_tex_is_linear(&e->t.d) != nvn_tex_is_linear(&src->t.d)) return *why = "linear and block-linear", 0;
    int ew, eh, sw, sh, eb = img_block(e, &ew, &eh), sb = img_block(src, &sw, &sh);
    if (!eb || eb != sb || ew != sw || eh != sh) return *why = "another block size", 0;
    if (ew == 1 && eh == 1) return 1;
    if (p->width != q->width || p->height != q->height || p->depth != q->depth || e->t.L.size != src->t.L.size)
        return *why = "compressed, another shape", 0;
    return 1;
}

// src holds content the storage bytes do not: the GPU wrote it and, for a mirrored texture, it was not written back
// and the CPU has not rewritten the storage since (the new entry's upload then reads the newer bytes).
static int alias_has_gpu_content(const Tex *src) {
    if (!src->pub.gpu_owned) return 0;
    if (!src->pub.mirrored) return 1;
    uint32_t now;
    return src->gpu_dirty && !storage_rewritten(src, src->gpu_gen, &now);
}

// Maxwell reads the same bytes through both descriptors: src's content goes into the new image e at the replay's
// current position, before any later command of the submission uses e. Through a buffer, since bound images share
// their memory and image copies may not overlap; e leaves UNDEFINED between the two halves, so its first transition
// cannot clobber src's bytes before they are read. 0 when no replay is recording (e is then not initialised yet).
static int alias_copy(Tex *e, const Tex *src) {
    int reopen;
    VkCommandBuffer cb = vk_exec_inline_begin(&reopen);
    if (!cb) return 0;
    int bw, bh, bpb = img_block(e, &bw, &bh);
    uint32_t levels = e->pub.levels < src->pub.levels ? e->pub.levels : src->pub.levels;
    uint32_t layers = e->pub.layers < src->pub.layers ? e->pub.layers : src->pub.layers, n = levels * layers, k = 0;
    // the region both images cover (alias_compatible: the same shape for block-compressed ones); the rest of a larger
    // new image starts as init_image would leave it
    int partial = src->pub.width < e->pub.width || src->pub.height < e->pub.height || src->pub.depth < e->pub.depth ||
                  levels < e->pub.levels || layers < e->pub.layers;
    VkBufferImageCopy *rg = calloc(n, sizeof *rg);
    if (!rg) rt_fatal("vk: out of memory for an alias copy");
    VkDeviceSize total = 0;
    for (uint32_t lv = 0; lv < levels; lv++)
        for (uint32_t ly = 0; ly < layers; ly++, k++) {
            uint32_t w = umin(mip(e->pub.width, lv), mip(src->pub.width, lv));
            uint32_t h = umin(mip(e->pub.height, lv), mip(src->pub.height, lv));
            uint32_t z = umin(mip(e->pub.depth, lv), mip(src->pub.depth, lv));
            rg[k] = (VkBufferImageCopy){.bufferOffset = total, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, ly, 1},
                                        .imageExtent = {w, h, z}};
            total += ((VkDeviceSize)(w + bw - 1) / bw * ((h + bh - 1) / bh) * z * bpb + 15) & ~15ull;
        }
    VkBuffer buf = vk_exec_inline_buffer(total);
    if (!buf) {
        free(rg);
        vk_exec_inline_end(reopen);
        return 0;
    }
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    vkCmdCopyImageToBuffer(cb, src->pub.image, VK_IMAGE_LAYOUT_GENERAL, buf, n, rg);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = e->pub.image,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb,
                           .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
    if (partial && vk_fix_zero() && !e->bound_to && !e->pub.mirrored && !format_compressed(e->f.vk)) {
        VkClearColorValue zero = {0};
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearColorImage(cb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
        barrier(cb, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    vkCmdCopyBufferToImage(cb, buf, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, n, rg);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    vk_exec_inline_end(reopen);
    free(rg);
    return 1;
}

// Descriptors whose image creation failed (VRAM exhausted, an unsupported format): not tried again for 2 s, so a
// draw that names one every frame does not repeat a failing allocation on every use. Texture lock held.
#define FAILED_HOLD 32
static struct {
    TexKey k;
    uint64_t until_ns;
} failed_hold[FAILED_HOLD];
static unsigned failed_next;

static int create_held(const TexKey *k) {
    uint64_t now = plat_now_ns();
    for (int i = 0; i < FAILED_HOLD; i++)
        if (failed_hold[i].until_ns > now && !memcmp(&failed_hold[i].k, k, sizeof *k)) return 1;
    return 0;
}

static void create_failed(const TexKey *k) {
    unsigned i = failed_next++ % FAILED_HOLD;
    failed_hold[i].k = *k, failed_hold[i].until_ns = plat_now_ns() + 2000000000ull;
}

// NX2NAVI_TRACE=perf: a line for each event of the two kinds that can leave a descriptor showing an image without
// content, up to START_LINES per address and kind (said_at).
//  - over: e, which the CPU writes, was made over the live entry `over` and is filled from its storage. `over` stays
//    as it is, and a slot that names its descriptor goes on showing it, whatever it holds.
//  - gone: e, which the CPU does not write, was made at an address the CPU can see, and the released CPU-written entry
//    there was destroyed (gone: its description, gone_rel: the frame of its release). Nothing uploads e; a slot of e's
//    descriptor that showed the destroyed entry's image (slot_resolve follows it while it is live) shows e's from now on.
// flag: e was taken back (over); how e started (gone): 0 empty, 1 with a copy of another entry's content, 2 with a
// copy of the destroyed entry's, 3 filled from its storage (the last two: split fill).
static uint64_t n_cpu_empty[3];   // since the last perf line: entries the CPU does not write, at an address it can see,
                                  // that started without content: on their own, over a released entry, over a live one
#define START_LINES 8
#define START_LINES_ALL 20000
static void start_line(const Tex *e, const Tex *over, const char *gone, uint64_t gone_rel, int flag) {
    static SaidAt said[2];
    static uint32_t total[2];
    int kind = over ? 0 : 1;
    if (total[kind] >= START_LINES_ALL || !said_at(&said[kind], e->key.addr, START_LINES)) return;
    total[kind]++;
    char d[224], o[224], og[320];
    desc_text(d, sizeof d, e);
    if (over) {
        const char *why = mirror_refused(&over->t);
        desc_text(o, sizeof o, over);
        origin_text(og, sizeof og, over);
        rt_log("perf: frame %llu: texture %#llx filled from storage over a live entry%s: new [%s]; live [%s], %s%s%s, %s",
               (unsigned long long)frames_now(), (unsigned long long)e->key.addr, flag ? " (taken back)" : "", d, o,
               why ? "not CPU-written: " : "CPU-written", why ? why : "", no_content(over) ? ", no content" : "", og);
    } else {
        static const char *const how[4] = {"started empty", "got another entry's content", "got the destroyed entry's content",
                                           "was filled from its storage"};
        rt_log("perf: frame %llu: texture %#llx, not CPU-written (%s), %s after the released CPU-written entry at its "
               "address was destroyed: new [%s]; destroyed [%s], released at frame %llu",
               (unsigned long long)frames_now(), (unsigned long long)e->key.addr, mirror_refused(&e->t), how[flag & 3], d,
               gone, (unsigned long long)gone_rel);
    }
}

// Split fill from the storage: e, new and through its first transition, gets its storage's bytes in the upload batch
// (make_fresh, as a CPU-written texture's first use) and is marked to follow them or not (Tex.sfill). 0 when e is left
// as it is: storage over SPLIT_FILL_MAX, an image taken from a released entry (see "split fill"), storage not mapped,
// or an upload that had no memory, which make_fresh tries again later.
static int split_fill(Tex *e) {
    if (e->t.L.size > SPLIT_FILL_MAX) return n_split_big++, 0;
    if (e->reused) return n_split_reused++, 0;
    if (!mem_readable(e->pub.addr, e->t.L.size)) return 0;
    uint64_t t0 = kern_now_ns();
    e->sfill = (uint8_t)split_mode;
    make_fresh(e);
    if (!e->uploaded) return 0;
    n_split_fill++, split_total[1]++, n_split_bytes += e->t.L.size, n_split_ns += kern_now_ns() - t0;
    e->had |= HAD_FILL;
    return 1;
}

static int depth_alias_log(void) {   // NX2NAVI_DEPTH_ALIAS_LOG=1: start_at logs each new depth entry over a live texture
    static int on = -1;
    if (on < 0) on = rt_knob("DEPTH_ALIAS_LOG", "0");
    return on;
}

// The start of e's content at its address, for a new entry and for a superseded one taken back (taken_back): another
// descriptor's released entry there is superseded, and a live one holding GPU-written content is the alias source,
// preferring the one that differs only in swizzle (same_but_swizzle). A superseded entry is kept for its own
// descriptor, which takes it back here: Ocarina alternates descriptors at some addresses every frame (a 2560x1440
// target and a 480x270 one in Kokiri Forest), and destroying the released one made both images again each frame. It
// starts over as a new entry would (alias copy or init_image) when a texture at its address holds GPU content written
// since its release (newer), and else keeps its own: the memory still holds what it held then. Ocarina gives its scene
// picture (r11g11b10f) a second descriptor at the same address that nothing draws into (rgb9e5, read raw by the post
// pass), and the picture, taken back about once a second and restarted empty, went black for a frame (issue #253).
// Released mirrored entries are still destroyed: their content is the storage's, uploaded when an entry is made.
// A released entry that holds GPU content gives it to e when no live texture there holds any (rsrc): its memory on
// the console still holds what it drew, and a new descriptor made for that picture after the old one was let go read
// it there (a black frame with FSR 3.1 and its frame generation, two presents long).
static void start_at(Tex *e, const TexKey *k, int taken_back) {
    int alias = 0;
    int trace = (rt_trace & RT_TRACE_PERF) != 0;
    int gone = 0, rel_other = 0;   // released entries at the address: CPU-written ones destroyed here, others superseded
    char gone_desc[224] = "";      // trace: the last destroyed one, described while it existed
    uint64_t gone_rel = 0;
    Tex *src = NULL, *rsrc = NULL;
    // split fill: e is to start with its storage's bytes; msrc: the released CPU-written entry whose image holds them
    // (destroyed after the copy), msrc_gpu: it holds what the GPU wrote instead, msrc_gen: the storage's generation
    int fill = !taken_back && tex_alias && split_wants(&e->t, &e->f), msrc_gpu = 0;
    uint32_t msrc_gen = 0;
    Tex *msrc = NULL;
    const char *why = "no live texture with GPU content";   // for the log of an alias left empty
    const Tex *other = NULL;
    int newer = 0;          // taken back: a texture at this address holds GPU content written since e's release
    int live_content = 0;   // a live texture at this address holds GPU content
    for (Tex *p = nvn_hm_get(&by_addr, &k->addr), *nx; p; p = nx) {
        nx = p->next_at;
        if (p == e) continue;
        if (taken_back && p->use_frame >= e->rel_frame && alias_has_gpu_content(p)) newer = 1;
        if (p->released) {
            if (p->pub.mirrored) {
                if (trace) {
                    int n = desc_text(gone_desc, sizeof gone_desc, p);
                    if (n > 0 && n < (int)sizeof gone_desc)
                        snprintf(gone_desc + n, sizeof gone_desc - (size_t)n, ", %s", p->pub.gpu_owned ? "GPU-written"
                                 : p->uploaded ? "uploaded" : "not uploaded");
                    gone_rel = p->rel_frame;
                }
                gone++;
                const char *no;
                uint32_t gen = 0;
                int gpu = alias_has_gpu_content(p);
                if (fill && !msrc && (gpu || (p->uploaded && !p->pub.gpu_owned &&
                                              !vk_mem_changed(p->pub.addr, p->t.L.size, p->wgen, &gen))) &&
                    alias_compatible(e, p, &no)) {
                    msrc = p, msrc_gpu = gpu, msrc_gen = gen;
                    continue;
                }
                drop_tex(p, DROP_REPLACED);
                continue;
            }
            p->superseded = 1, rel_other = 1;
            const char *unused;
            if (tex_alias && alias_has_gpu_content(p) && alias_compatible(e, p, &unused) &&
                (!rsrc || p->use_frame > rsrc->use_frame))
                rsrc = p;
            continue;
        }
        alias = 1;
        other = p;
        if (alias_has_gpu_content(p)) live_content = 1;
        if (!tex_alias || (src && !(same_but_swizzle(&p->key, k) && !same_but_swizzle(&src->key, k)))) continue;
        if (!alias_compatible(e, p, &why)) continue;
        if (!alias_has_gpu_content(p)) { why = "the live texture holds no GPU content"; continue; }
        src = p;
    }
    int from_released = !src && rsrc && !live_content;
    if (from_released) src = rsrc;
    if (alias) perf_count(PERF_CNT_TEX_ALIAS);
    if (alias && (e->f.depth || e->f.stencil) && depth_alias_log()) {
        // each new depth or stencil entry over a live texture, with the frame and whether the two are bound into pool
        // memory (the same allocation: they share bytes), to line up with a dark frame
        static int n_dlog;
        if (n_dlog++ < 20000)
            rt_log("vk: depth alias: frame %llu %#llx %ux%ux%u fmt %#x%s%s over %ux%ux%u fmt %#x%s%s%s: %s",
                   (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), (unsigned long long)k->addr,
                   e->pub.width, e->pub.height, e->pub.layers, e->t.d.format, e->bound_to ? " bound" : " own memory",
                   taken_back ? " (taken back)" : "", other->pub.width, other->pub.height, other->pub.layers,
                   other->t.d.format, other->f.depth ? " depth" : "", other->bound_to ? " bound" : " own memory",
                   e->bound_to && e->bound_to == other->bound_to ? ", same allocation" : "",
                   taken_back && !newer && !e->pub.mirrored ? "kept its own"
                   : e->reused || taken_back ? "cleared at the replay's position (reused)"
                   : depth_init_inline() ? "cleared at the replay's position" : "cleared in the upload batch");
    }
    if (taken_back && !newer && !e->pub.mirrored) {   // its image still holds its content
        n_alias_kept++;
        return;
    }
    if (!tex_alias) {   // create_tex initialised a new entry
        if (taken_back) init_new(e);
        return;
    }
    // split fill only where nothing live holds content the storage lacks: that content is what e should show
    if (src || live_content) fill = 0;
    int copied = src && alias_copy(e, src), filled = 0;
    if (fill && msrc && alias_copy(e, msrc)) {   // from the released entry this one replaces
        copied = filled = 1;
        n_split_copy++, split_total[0]++;
        e->og_start = START_COPY_GONE;
        e->had |= HAD_COPY;
        if (!msrc_gpu && e->t.L.size <= SPLIT_FILL_MAX) {   // the storage's bytes as of msrc_gen: followed from there
            e->sfill = (uint8_t)split_mode, e->uploaded = 1, e->wgen = msrc_gen;
            e->chash_ok = 0;
        }
    }
    if (msrc) drop_tex(msrc, DROP_REPLACED);
    if (filled) {   // nothing more: e is not CPU-written
    } else if (copied) {
        n_alias_copied++;
        if (from_released) n_alias_released++;
        e->og_start = from_released ? START_COPY_RELEASED : START_COPY_LIVE;
        e->had |= HAD_COPY;
        if (e->pub.mirrored) {   // the content is src's GPU render, newer than the storage bytes
            vk_mem_changed(e->pub.addr, e->t.L.size, 0, &e->wgen);
            e->uploaded = 1;
            pool_gpu_owned(e);
            e->pub.gpu_owned = 1;
        }
        e->chash_ok = 0;
    } else {
        init_new(e);
        if (fill) filled = split_fill(e);
        e->og_start = filled ? START_FILL : alias ? (e->pub.mirrored ? START_UPLOAD_LIVE : START_EMPTY_LIVE)
                    : (gone || rel_other) && !e->pub.mirrored ? START_EMPTY_RELEASED : START_NEW;
        // an entry the CPU does not write, at an address the CPU can see, that starts without content
        if (!filled && !e->pub.mirrored && !nvn_gpu_only(k->addr)) n_cpu_empty[alias ? 2 : gone || rel_other ? 1 : 0]++;
        if (alias) {
            if (e->pub.mirrored) {
                n_alias_upload++;
                if (trace) start_line(e, other, NULL, 0, taken_back);
            } else if (!filled) {
                n_alias_uninit++;
                static int logged;
                // what an empty start over live memory was, to tell a black frame's cause; depth buffers start empty
                // over another target's memory at every area load, harmlessly, and are left out
                int depth = e->f.depth || e->f.stencil || other->f.depth || other->f.stencil;
                if (logged < 24 && other && !depth) {
                    logged++;
                    rt_log("vk: texture %#llx (%ux%u fmt %#x%s) starts empty over a live one (%ux%u fmt %#x): %s; frame %llu",
                           (unsigned long long)k->addr, e->pub.width, e->pub.height, e->t.d.format,
                           taken_back ? ", taken back" : "", other->pub.width, other->pub.height, other->t.d.format,
                           src ? "no replay recording to copy in" : why,
                           (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED));
                }
            }
        }
    }
    if (trace && gone && !e->pub.mirrored && !nvn_gpu_only(k->addr))
        start_line(e, NULL, gone_desc, gone_rel, e->og_start == START_COPY_GONE ? 2 : filled ? 3 : copied);
}

// A replay worker's chunk before its turn (vk_exec.c "replay workers"): a lookup that would make or take back an entry
// returns nothing and sets this; the chunk replays the op again once every earlier chunk is done, so entries are made,
// initialised and taken back in the order of the stream, as the replay would.
static _Thread_local int tex_deferred;
static volatile uint64_t n_deferred[3];   // by what the lookup found: no entry, a retired one, a released one
void vk_texture_deferred_stats(uint64_t out[3]) {
    for (int i = 0; i < 3; i++) out[i] = __atomic_exchange_n(&n_deferred[i], 0, __ATOMIC_RELAXED);
}
int vk_texture_deferred(void) { return tex_deferred; }
int vk_texture_deferred_take(void) {
    int d = tex_deferred;
    tex_deferred = 0;
    return d;
}

// A lookup of k that finds e (NULL: no entry) and needs no turn (vk_exec_turn_due): it returns nothing without changing
// anything (the pool refresh asking for a held key or a retired entry, a creation that failed lately: the texture pool
// refresh looks its unresolved slots up at every sync), or it is the submitting thread's past a group (vk_exec_behind)
// and takes back a released entry whose image stayed its own. That keeps its content and is ordered after the
// group's chunks anyway: one of them that looks the entry up later finds it in use, as it would have after taking it
// back itself. A transient render target the frame releases and takes back at every pass that draws it did that after
// nearly every group (Hyrule Field, 2026-11-12).
static int lookup_quiet(const Tex *e, const TexKey *k) {
    if (e && e->retiring) return pool_ctx;
    if (e) return !e->superseded && vk_exec_behind();
    if (holds.n && nvn_hm_get(&holds, k)) return pool_ctx;
    return create_held(k);
}

static Tex *entry_of(const tex_t *t) {
    TexKey k;
    make_key(t, &k);
    Tex *e = nvn_hm_get(&by_key, &k);
    if ((!e || e->retiring || e->released) && vk_exec_turn_due() && !lookup_quiet(e, &k)) {
        __atomic_add_fetch(&n_deferred[!e ? 0 : e->retiring ? 1 : 2], 1, __ATOMIC_RELAXED);
        tex_deferred = 1;
        return NULL;
    }
    if (e && e->retiring) {   // retired by disuse: the pool refresh leaves it out, any other lookup takes it back
        if (pool_ctx) return NULL;
        retire_cancel(e);
    }
    if (!e) {
        if (held_out(&k) || create_held(&k)) return NULL;
        uint64_t c0 = kern_now_ns();   // the entry's creation (VK_TS_CREATE): image, memory, first transition
        if (tex_alias < 0) {
            tex_alias = cfg_bool("NX2NAVI_TEX_ALIAS");
            rt_log("vk: texture aliases: %s", tex_alias ? "a new image takes the live image's content" : "a new image starts empty");
        }
        if (!(e = create_tex(t, &k, tex_alias))) return create_failed(&k), NULL;
        origin_made(e);
        perf_count(PERF_CNT_TEX_NEW);
        start_at(e, &k, 0);
        flight_ev(e, FLIGHT_TEX_NEW, e->og_start, 0);
        e->next_at = nvn_hm_get(&by_addr, &k.addr);
        nvn_hm_put(&by_addr, &k.addr, e);
        nvn_hm_put(&by_key, &k, e);
        n_site[1]++;
        tex_gen_bump(k.addr);
        vk_texstat_time(VK_TS_CREATE, c0);
        e->samp_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);   // new: not disused before it is drawn
    }
    if (e->released) {
        flight_ev(e, FLIGHT_TEX_TAKEBACK, 0, 0);
        e->samp_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);   // in use again, as a new one
        uint64_t gap = e->samp_frame - e->rel_frame;
        if (gap > e->back_gap) e->back_gap = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
        e->released = 0, nreleased--, rel_unlink(e), n_site[2]++, tex_gen_flip(e, e->superseded || e->overwritten);
        e->overwritten = 0;
        e->og_backs++, e->og_back_frame = e->samp_frame;
        if (e->superseded) {   // its image may still be in use: the first barrier waits for all earlier work
            e->superseded = 0, e->reused = 1;
            n_restarts++;
            start_at(e, &k, 1);
        }
    }
    e->t = *t;
    touch(e);
    return e;
}

// vk_dump: upload bookkeeping of an image vk_texture_get / vk_texture_by_id returned. pages_written: whether its
// storage pages were written since the last check (1, 0, -1 not tracked); the query leaves that check's state alone.
void vk_texture_debug(const VkTexImage *img, uint32_t *uploads, int *last_change, int *pages_written) {
    lock();
    const Tex *e = (const Tex *)img;   // pub is the first member
    uint32_t now;
    *uploads = e->n_up;
    *last_change = e->last_ch;
    *pages_written = e->pub.mirrored && e->uploaded ? vk_mem_changed(e->pub.addr, e->t.L.size, e->wgen, &now) : 0;
    unlock();
}

const VkTexImage *vk_texture_get(const tex_t *t) {
    if (!t || !t->mem) return NULL;
    lock();
    Tex *e = entry_of(t);
    if (e) make_fresh(e);
    unlock();
    return e ? &e->pub : NULL;
}

// Render targets the CPU reads without NVN's invalidate (vk_texture_write_back_small); small_pending: one may be dirty.
// wb_limit: targets up to this size (bytes) always go back when the CPU waits for a fence, NX2NAVI_WB_FENCE_LIMIT, read
// once. A larger one goes back unless the GPU wrote it in each of the last WB_RUN frames (wb_wanted). wb_trace:
// NX2NAVI_TRACE=gfx, read together with it.
enum { WB_SMALL_DEFAULT = 64 << 10, WB_RUN = 16 };
static int small_pending;
static uint64_t wb_limit = WB_SMALL_DEFAULT;
static int wb_trace;
static int wb_cfg_read;

static void wb_cfg_init(void) {
    if (wb_cfg_read) return;
    int v = cfg_int("NX2NAVI_WB_FENCE_LIMIT");
    wb_limit = v < 0 ? 0 : (uint64_t)v;
    wb_trace = (rt_trace & RT_TRACE_GFX) != 0;
    wb_cfg_read = 1;
    if (wb_limit != WB_SMALL_DEFAULT) rt_log("vk: fence write-back limit %llu bytes", (unsigned long long)wb_limit);
}

// Whether the fence write-back takes a dirty target. A target the GPU draws every frame is scratch the CPU does not
// read: in Hyrule Field three 256x256 RGBA8 targets and a 256x256 RG8 one, which cost 2.7 ms a frame to write back.
// NX2navi's 260x260 RG8 grass maps, which its fire spread reads after a fence wait (#132), are drawn once per change, or
// up to 9 frames in a row while a tile loads.
static int wb_wanted(const Tex *e) { return e->t.L.size <= wb_limit || e->run < WB_RUN; }

static void mark_released(Tex *e) {   // tex_lock held; e is not released
    flight_ev(e, FLIGHT_TEX_RELEASE, 0, 0);
    e->released = 1;
    e->rel_epoch = submit_epoch;
    e->rel_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    nreleased++;
    rel_append(e);
    n_site[3]++;
    tex_gen_flip(e, 0);
}

// e, in the GPU-only address window, is written by the GPU in frame fr: the entries its storage runs over are
// overwritten (section "Moved render targets", above gwin).
static void overwrite_others(const Tex *e, uint64_t fr) {
    uint64_t lo = e->key.addr, hi = lo + e->t.L.size;
    int released = 0;
    for (int i = gwin_lower(lo > gwin_span ? lo - gwin_span : 0); i < ngwin && gwin[i]->key.addr < hi; i++) {
        Tex *q = gwin[i];
        uint64_t qlo = q->key.addr, qhi = qlo + q->t.L.size;
        if (q->overwritten || qlo == lo || qhi <= lo || (lo >= qlo && hi <= qhi)) continue;
        if (fr - q->gw_frame < OVERWRITE_FRAMES) continue;
        q->overwritten = 1, q->ow_frame = fr;
        n_overwritten++;
        if (!q->released) mark_released(q), released = 1;
    }
    if (released) sweep_released();
}

void vk_texture_gpu_written(const tex_t *t) {
    if (!t || !t->mem) return;
    vk_exec_frame_written(t);
    lock();
    Tex *e = entry_of(t);
    if (e) {
        make_fresh(e);   // a partial render keeps what the storage held before
        uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        e->gw_frame = now;
        if (e->scan_frame != now + 1 && nvn_gpu_only(e->key.addr)) {   // once per frame and entry
            e->scan_frame = now + 1;
            overwrite_others(e, now);
        }
        pool_gpu_owned(e);
        e->pub.gpu_owned = 1;
        e->chash_ok = 0;   // the image now holds the GPU's content: matching storage bytes no longer mean "unchanged"
        if (e->pub.skip) {   // texture detail leaves out what the GPU can write (detail_skip); said if it ever does
            static volatile int32_t n_skip_written;
            log_limited(&n_skip_written, "texture %#llx (fmt %#x %ux%u), made without its top %u levels, was written by "
                        "the GPU: its levels are not the game's", (unsigned long long)e->pub.addr, e->t.d.format,
                        e->t.d.width, e->t.d.height, e->pub.skip);
        }
        uint64_t f = vk_exec_recording_fence();
        if (f > e->wr_fence) e->wr_fence = f;
        if (e->pub.mirrored) {
            e->gpu_dirty = 1;
            storage_rewritten(e, 0, &e->gpu_gen);   // CPU writes up to here are older than this render
            if (wb_exact < 0) wb_exact = cfg_bool("NX2NAVI_WB_EXACT");
            if (wb_exact && !e->snap && e->t.L.size <= SNAP_MAX && fills_no_page(e) && !(e->snap = malloc(SNAP_MAX)))
                rt_fatal("vk: out of memory for a texture's storage copy");
            if (e->snap) memcpy(e->snap, (const void *)(uintptr_t)e->pub.addr, e->t.L.size);
            e->gpu_seq = ++gpu_writes;
            e->gpu_fence = vk_exec_recording_fence();
            uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
            if (fr != e->run_frame || !e->run) {
                e->run = fr == e->run_frame + 1 && e->run ? (uint8_t)(e->run < WB_RUN ? e->run + 1 : WB_RUN) : 1;
                e->run_frame = fr;
            }
            wb_cfg_init();
            if (wb_wanted(e)) __atomic_store_n(&small_pending, 1, __ATOMIC_RELEASE);
            own_add(e);
        }
    }
    unlock();
}

void vk_texture_release(const tex_t *t) {
    if (!t || !t->mem) return;
    TexKey k;
    make_key(t, &k);
    lock();
    Tex *e = nvn_hm_get(&by_key, &k);
    // Staleness belongs to the registered descriptor, not the cached image: a released Tex can be destroyed before the
    // slot looks again (eviction, the released cap, start_at for a mirrored one). A registered texture may also be
    // finalized before its image is created.
    // Only the slots this texture object registered (its initialization serial): a slot registered with another
    // texture initialized at the same storage with the same parameters names that one, which is still live. Ocarina
    // streams terrain heightmaps (R32F 260x260) through a fixed set of storage and initializes and registers the next
    // chunk's texture there before it finalizes the previous one; finalizing the new registration left the slot stale,
    // the draws sampled the zero stand-in, and the chunk was drawn at height zero, tens of metres below the ground.
    int changed = 0;
    if (sinfo)
        for (int id = bucket_head[agen_idx(k.addr)]; id >= 0; id = slot_next[id]) {
            TexSlot *s = &tslots[id];
            if (s->finalized || (uintptr_t)s->t.mem != k.addr) continue;
            TexKey sk;
            make_key(&s->t, &sk);
            if (memcmp(&sk, &k, sizeof k)) continue;
            if (s->t.serial != t->serial) {   // another texture object's registration: it stays current
                n_fin_other++, n_fin_other_hm += terrain_heightmap(&s->t);
                continue;
            }
            s->finalized = 1, s->ce = NULL, changed = 1;
            n_fin_own++;
        }
    if (e && !e->released) {
        mark_released(e);
        changed = 0;   // the image's release already invalidated the address bucket
        sweep_released();
    }
    if (changed) tex_gen_bump(k.addr);
    unlock();
}

void vk_texture_range_gone(uint64_t lo, uint64_t size, VmaAllocation alloc) {
    lock();
    Tex **list = malloc(sizeof *list * (size_t)(by_key.n + 1));
    int n = 0;
    for (uint32_t i = 0; i < by_key.cap; i++) {
        Tex *e = by_key.vals[i];
        if (e && ((e->key.addr >= lo && e->key.addr - lo < size) || (alloc && e->bound_to == alloc))) list[n++] = e;
    }
    for (int i = 0; i < n; i++) drop_tex(list[i], DROP_RANGE);
    free(list);
    unlock();
}

// ---------------------------------------------------------------- descriptor slots
void vk_texture_register(int id, const tex_t *t, const view_t *v) {
    if (id < 0 || id >= (1 << 20)) rt_fatal("vk: texture slot %d out of range", id);
    lock();
    if (id >= ntslots) {
        int n = ntslots ? ntslots : 1024;
        while (n <= id) n *= 2;
        TexSlot *s = realloc(tslots, sizeof *s * (size_t)n);
        if (!s) rt_fatal("vk: out of memory for texture slots");
        memset(s + ntslots, 0, sizeof *s * (size_t)(n - ntslots));
        tslots = s, ntslots = n;
    }
    if (!sinfo) {
        uint64_t *p = calloc(1 << 20, sizeof *p);
        slot_next = malloc(sizeof *slot_next << 20), slot_prev = malloc(sizeof *slot_prev << 20);
        slot_grp = malloc(sizeof *slot_grp << 20);
        slot_bucket = calloc(1 << 20, sizeof *slot_bucket);
        if (!p || !slot_next || !slot_prev || !slot_grp || !slot_bucket) rt_fatal("vk: out of memory for texture slot tokens");
        for (int i = 0; i < (1 << 20); i++) slot_prev[i] = -2, slot_next[i] = -1, slot_grp[i] = -1;
        for (int i = 0; i < AGEN_N; i++) bucket_head[i] = -1, bucket_ghead[i] = -1;
        __atomic_store_n(&sinfo, p, __ATOMIC_RELEASE);
    }
    TexSlot *s = &tslots[id];
    // The same texture and view registered again (Ocarina rewrites much of its descriptor pool every frame): the slot
    // keeps its serial and resolved entry, so the texture pool refresh (vk_exec_draw.c pool_sync_entries) does not
    // resolve it again. Only while that entry is live: after a release, a registration is what makes a slot current
    // again (slot_resolve's stale rule), so it goes the full way. A slot held out of the pool (cheld) keeps that too:
    // the same descriptor names the same held texture. Both only while the resolution is current (cgen): a destroyed
    // entry bumped the generation and its record may be freed, so ce is not read past that.
    int current = s->used && !s->finalized && s->cgen == agen[sinfo[id] & 0xffff];
    if (current && ((s->ce && !s->ce->released) || s->cheld) && !memcmp(&s->t, t, sizeof *t) &&
        s->has_view == (v != NULL) && (!v || !memcmp(&s->v, v, sizeof *v))) {
        n_same_regs++;
        unlock();
        return;
    }
    s->t = *t;
    if (!s->used && id < VK_POOL_TEX) pool_slots++;
    s->used = 1;
    s->finalized = 0;
    s->ce = NULL;
    s->cheld = 0;
    slots_gen++;
    if (!++reg_serial) reg_serial = 1;
    __atomic_store_n(&sinfo[id], (uint64_t)reg_serial << 16 | agen_idx((uintptr_t)t->mem), __ATOMIC_RELEASE);
    dirty_push((uint32_t)id);
    s->has_view = v != NULL;
    if (v) s->v = *v;
    else memset(&s->v, 0, sizeof s->v);
    s->khash = 0;
    if (!tex_mirrored(t)) {   // FNV-1a over the key (make_key zeroes its padding) and the view
        TexKey k;
        make_key(t, &k);
        uint64_t h = 0xcbf29ce484222325ull;
        const uint8_t *b = (const uint8_t *)&k;
        for (size_t i = 0; i < sizeof k; i++) h = (h ^ b[i]) * 0x100000001b3ull;
        b = (const uint8_t *)&s->v;
        for (size_t i = 0; i < sizeof s->v; i++) h = (h ^ b[i]) * 0x100000001b3ull;
        s->khash = (h ^ s->has_view) | 1;
    }
    slot_place(id, agen_idx((uintptr_t)t->mem), s->khash);
    unlock();
}

// The descriptor hash of a GPU-written slot: two slots with equal hashes, the same address generation and no entry
// created, destroyed, released or taken back between their lookups (vk_texture_entry_gen) resolve alike. 0 for an
// unregistered or CPU-written slot (its resolution depends on when it was registered: slot_resolve's stale rule).
uint64_t vk_texture_slot_khash_locked(int id) { return id >= 0 && id < ntslots && tslots[id].used ? tslots[id].khash : 0; }
uint32_t vk_texture_entry_gen(void) { return tex_gen; }

// The entry slot s resolves to, and the view and has_view a caller gets with it (the slot's, with the descriptor's
// swizzle when the entry is another descriptor at the same storage). NULL when the slot names no texture.
// Entry creation budget of slot lookups (vk_texture_create_budget_locked): -1 none. The bindless pools look up every
// registered slot, and most of a pool is stale descriptors (storage freed or reused); with a budget a lookup creates at
// most that many entries, only for mapped storage, and notes the slots it left for later (create_deferred).
static int create_budget = -1, create_deferred;
// A slot whose stand-in a shader sampled is on screen as that stand-in: its texture is made by the next lookup even
// with the budget at 0 (the frame's creation time spent, vk_exec_draw.c pool_create_ns), WANT_PER_FRAME of them a
// present. Left to the budget, it stayed dark for as long as the refresh had no time over, about a second in a town.
#define WANT_PER_FRAME 32
static int want_take(void) {
    static uint64_t frame = ~0ull;
    static int left;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (f != frame) frame = f, left = WANT_PER_FRAME;
    return left > 0 ? left-- : 0;
}

void vk_texture_create_budget_locked(int n) { create_budget = n, create_deferred = 0; }
int vk_texture_create_deferred_locked(void) { int d = create_deferred; create_deferred = 0; return d; }

// NX2NAVI_TEX_STALE_KEPT (default 1; 0: the stand-in, as before): a finalized slot of a CPU-written texture resolves to
// its own entry while that entry holds the texture's content: uploaded or drawn by the GPU, and neither superseded by
// another descriptor at its address nor overwritten. The game finalizes texture objects on its own thread ahead of the
// replay, which still draws the frame the game recorded before that; on the console the memory still holds the texels
// those draws read. Kakariko moves about 30 of its textures to new addresses every four or five frames: each move
// finalized the old descriptors, the replay's draws of the frame before then met finalized slots, and their stand-ins
// took the village's haze out of a frame a few times a minute (thousands of stand-in lookups in 60 frames). The image
// is sampled as it is: the lookup neither takes the entry back nor reads its storage (vk_texture_by_id_locked).
static int stale_kept(const Tex *own) {
    static int on = -1;
    if (on < 0) on = rt_knob("TEX_STALE_KEPT", "1") == 1;
    return on && own && !own->superseded && !own->overwritten && (own->uploaded || own->pub.gpu_owned);
}

static Tex *slot_resolve(TexSlot *s, view_t *view, int *has_view) {
    *view = s->v;
    *has_view = s->has_view;
    TexKey k;
    make_key(&s->t, &k);
    Tex *e = nvn_hm_get(&by_key, &k);
    int stale = 0;
    if (!e || e->released || s->finalized) {   // a finalized slot follows live GPU content, not newly reused CPU bytes
        // Prefer the live entry that differs from the slot descriptor only in its swizzle: transient render targets
        // share storage, and the newest one at the address is usually another size. The map screen samples its
        // 2250x1080 map target through an (R,G,B,1) descriptor and followed a 960x60 target at the same address.
        Tex *own = e, *any = NULL;
        e = NULL;
        // A slot whose content the CPU writes (tex_mirrored) follows another image only for content its own bytes do
        // not hold, the GPU's: Ocarina rasterizes each UI caption on the CPU into a small r8 glyph atlas and reuses the
        // memory of a freed one for the next, of another width. Following the older 256x39 atlas at the address, the
        // replay uploaded the new 128-wide bytes with a 256-texel row, and the Basic Controls screen's ZL caption
        // ("Target") sampled a mix of its glyphs and stale ones. Without such an image the slot gets its own below
        // (entry_of), uploaded from its storage with its own geometry.
        int cpu_slot = tex_mirrored(&s->t);
        for (Tex *p = nvn_hm_get(&by_addr, &k.addr); p; p = p->next_at) {
            if (p->released) continue;
            // Only an entry of the slot's target: Ocarina's volumetric fog registers a 3D two-channel volume (32x24x64)
            // over the storage of the bloom pass's 480x270 2D target; followed, the fog computes and the fog pass got
            // that 2D image, no 3D view can be made of it (vk_exec_draw.c get_view) and they read and wrote stand-ins.
            // With the target kept they get the volume itself.
            if (p->key.d.target != k.d.target) continue;
            if (same_but_swizzle(&p->key, &k) && (!cpu_slot || !s->finalized || alias_has_gpu_content(p))) { e = p; break; }
            if (!any && (!cpu_slot || alias_has_gpu_content(p)) && follow_size_ok(p, &k)) any = p;
        }
        // A finalized CPU descriptor must not recreate its image from freed storage. The slot keeps this state even
        // after its released image was evicted; registering the descriptor again makes it current. While its own
        // entry still holds the texture (stale_kept), the slot samples that image as it is.
        if (!e && any) e = any;
        else if (!e && !cpu_slot) e = own && own->superseded ? NULL : own;   // a superseded one: entry_of takes it back
        else if (!e && cpu_slot && s->finalized && stale_kept(own)) e = own, n_stale_kept++;
        else if (!e && cpu_slot && s->finalized) stale = 1, n_stale++, n_stale_hm += terrain_heightmap(&s->t);
    }
    resolve_stale = stale;
    if (!e && s->t.mem && !stale) {
        // evicted by disuse: the pool refresh keeps the stand-in, without spending the creation budget on it
        if (pool_ctx && holds.n && nvn_hm_get(&holds, &k)) resolve_held = 1;
        else if (create_budget < 0) e = entry_of(&s->t);
        // Unmapped storage first: such a slot has nothing to make, now or later. Asked after the budget, every stale
        // slot a refresh met with the budget at 0 was noted as left: in a busy frame (its creation time spent) that
        // was about a thousand slots at once, which vk_exec_draw.c pool_sync_entries took for an area load, several
        // times a second in some places, and each time lifted the count budget and the frame's creation time.
        else if (!mem_readable((uint64_t)(uintptr_t)s->t.mem, s->t.L.size)) e = NULL;
        else if (!create_budget && !(s->cwant && want_take())) create_deferred = 1;
        else {
            e = entry_of(&s->t);
            if (create_budget > 0) create_budget--;
        }
        if (e) s->cwant = 0;
    }
    if (e && e->retiring) {   // retired by disuse: the pool refresh gets the stand-in, any other lookup takes it back
        if (pool_ctx) e = NULL, resolve_held = 1;
        else retire_cancel(e);
    }
    // A followed texture is another descriptor at the same storage: its image holds the content, but sampling takes the
    // slot descriptor's swizzle. The UI renders text into RGBA8 and samples it through a (A,A,A,A) descriptor; with the
    // render target's RGBA swizzle the text blur saw R = 1 everywhere and drew solid boxes behind tips and labels.
    if (e && !(s->has_view && s->v.set_swizzle)) {
        const nvn_texdesc *d = &s->t.d;
        uint8_t sw[4] = {(uint8_t)d->swizzle_r, (uint8_t)d->swizzle_g, (uint8_t)d->swizzle_b, (uint8_t)d->swizzle_a};
        if (!(sw[0] | sw[1] | sw[2] | sw[3]))   // descriptors built without nvnTextureBuilderSetDefaults
            sw[0] = NVN_TEXTURE_SWIZZLE_R, sw[1] = NVN_TEXTURE_SWIZZLE_G, sw[2] = NVN_TEXTURE_SWIZZLE_B,
            sw[3] = NVN_TEXTURE_SWIZZLE_A;
        if (memcmp(sw, e->pub.swz, 4)) {
            if (!s->has_view) memset(view, 0, sizeof *view);
            memcpy(view->swz, sw, 4);
            view->set_swizzle = 1;
            *has_view = 1;
        }
    }
    return e;
}

void vk_texture_lock(void) { lock(); }
void vk_texture_unlock(void) { unlock(); }
uint32_t vk_texture_slots_gen(void) { return slots_gen; }
uint64_t vk_texture_same_regs(void) { return __atomic_load_n(&n_same_regs, __ATOMIC_RELAXED); }

// Diagnosis for the texture pool log: address-generation bumps since the last call, the slots of the fullest buckets
// and the bumps those buckets took. Texture lock held by none: takes it.
void vk_texture_bucket_stats(char *out, size_t n) {
    lock();
    static uint32_t occ[AGEN_N];
    memset(occ, 0, sizeof occ);
    for (int i = 0; i < ntslots && sinfo; i++)
        if (tslots[i].used) occ[sinfo[i] & 0xffff]++;
    int top[4] = {-1, -1, -1, -1};
    for (int b = 0; b < AGEN_N; b++)
        for (int k = 0; k < 4; k++)
            if (top[k] < 0 || occ[b] > occ[top[k]]) {
                for (int j = 3; j > k; j--) top[j] = top[j - 1];
                top[k] = b;
                break;
            }
    uint64_t hot = 0;   // bumps weighted by the slots they invalidate
    for (int b = 0; b < AGEN_N; b++) hot += (uint64_t)bump_count[b] * occ[b];
    snprintf(out, n, "%llu new generations (destroy %llu, create %llu, take-back %llu, release %llu; %llu releases and "
             "take-backs went back to a generation their state had), %llu slot invalidations; fullest buckets %u/%u/%u/%u slots, bumped %u/%u/%u/%u",
             (unsigned long long)n_bumps, (unsigned long long)n_site[0], (unsigned long long)n_site[1],
             (unsigned long long)n_site[2], (unsigned long long)n_site[3], (unsigned long long)n_undone,
             (unsigned long long)hot, occ[top[0]], occ[top[1]], occ[top[2]], occ[top[3]],
             bump_count[top[0]], bump_count[top[1]], bump_count[top[2]], bump_count[top[3]]);
    size_t used = strlen(out);
    if (used < n)
        snprintf(out + used, n - used, "; pool freshness check: %llu entries checked, %llu passed by their regions (missed: "
                 "%llu not uploaded, %llu a region without a snapshot yet, %llu untracked, %llu written since, %llu "
                 "nothing to check; %llu snapshot queries of NX2NAVI_POOL_PRESNAP)",
                 (unsigned long long)n_pool_checks, (unsigned long long)n_pool_skips,
                 (unsigned long long)n_pool_miss[0], (unsigned long long)n_pool_miss[1],
                 (unsigned long long)n_pool_miss[2], (unsigned long long)n_pool_miss[3],
                 (unsigned long long)n_pool_miss[4], (unsigned long long)n_presnap_q);
    used = strlen(out);
    if (used < n)
        snprintf(out + used, n - used, "; %d slot groups holding %llu slots", ngrp_live, (unsigned long long)n_grouped);
    int held = 0, gown = 0;
    for (int i = 0; i < npref; i++)
        if (pref[i].refs && pref[i].e) held++, gown += pref[i].gown;
    used = strlen(out);
    if (used < n)
        snprintf(out + used, n - used, "; pool slots hold %d entries, %d of them GPU-owned (stamped in each submission "
                 "checked by list)", held, gown);
    n_bumps = n_undone = n_pool_checks = n_pool_skips = n_presnap_q = 0;
    memset(n_pool_miss, 0, sizeof n_pool_miss);
    memset(n_site, 0, sizeof n_site);
    memset(bump_count, 0, sizeof bump_count);
    unlock();
}

uint64_t vk_texture_slot_token(int id) {
    const uint64_t *si = __atomic_load_n(&sinfo, __ATOMIC_ACQUIRE);
    if (!si || id < 0 || id >= (1 << 20)) return 0;
    uint64_t v = __atomic_load_n(&si[id], __ATOMIC_ACQUIRE);
    return v ? (v >> 16) << 32 | __atomic_load_n(&agen[v & 0xffff], __ATOMIC_ACQUIRE) : 0;
}
uint32_t vk_sampler_gen(void) { return __atomic_load_n(&samp_gen, __ATOMIC_RELAXED); }
int vk_texture_slot_count_locked(void) { return ntslots; }
int vk_sampler_slot_count_locked(void) { return nsslots; }

const VkTexImage *vk_texture_by_id(int id, view_t *view, int *has_view) {
    lock();
    const VkTexImage *r = vk_texture_by_id_locked(id, view, has_view);
    unlock();
    return r;
}

const VkTexImage *vk_texture_by_id_locked(int id, view_t *view, int *has_view) {
    if (id < 0 || id >= ntslots || !tslots[id].used) return NULL;
    TexSlot *s = &tslots[id];
    uint32_t g = agen[sinfo[id] & 0xffff];
    // resolved again: an entry at its address was created, dropped or released, or the slot registered (s->ce
    // cleared); an empty result is not kept, as image creation may succeed later, except a held one for the pool
    // refresh (cheld): the pool walks every slot each submission, and the hold ends with a generation bump
    if ((!s->ce || s->cgen != g) && !(s->cheld && s->cgen == g && pool_ctx)) {
        view_t v;
        int hv;
        resolve_held = 0;
        in_lookup = 1;
        s->ce = slot_resolve(s, &v, &hv);   // entry_of may create an entry and bump g: g read before it
        in_lookup = 0;
        s->cview = v, s->chas_view = (uint8_t)hv;
        s->cgen = g;
        s->cepoch = 0;
        s->cheld = (uint8_t)(!s->ce && resolve_held);
    }
    Tex *e = s->ce;
    // a draw binding: the texture is in use, whether or not the pool's refresh checked it first in this submission
    // (by list that check is no lookup, vk_texture_pool_fresh_locked, so the stamp of the use is left here too)
    if (e && !pool_ctx) touch(e);
    // Bindings of one submit share one check, answered from the submit's write-watch snapshot (guest writes during a
    // replay race the GPU read on hardware too). After one check an entry is either uploaded or never uploads (not
    // mirrored, GPU-owned or no CPU layout), so a slot that checked its entry in this submit has nothing left to do. A
    // held slot counts as checked for look_round: the textures evicted by disuse are registered slots all the same.
    if ((e || s->cheld) && s->cepoch != submit_epoch) {
        round_checks++;
        // a released entry here is a finalized slot's own (stale_kept): the storage may be the game's to free or reuse
        if (e && !e->released && (!e->uploaded || e->checked != submit_epoch)) {
            make_fresh_snap(e);
            e->checked = submit_epoch;
            touch(e);
        }
        s->cepoch = submit_epoch;
    }
    if (view) *view = has_view ? s->cview : s->v;
    if (has_view) *has_view = view ? s->chas_view : s->has_view;
    return e ? &e->pub : NULL;
}

// Texture detail: the texture slot id resolves to was made without its top levels, and a shader binding reads its
// texels by position or asks for its size (vk_exec_draw.c tex_info_full, GfxShaderRes.exact). Its key joins exact, the
// entry goes and the lookup makes it again whole, uploaded from its storage as a texture seen for the first time. A
// lookup that is out of its creation budget leaves the stand-in for now, as for any new texture. Texture lock held.
const VkTexImage *vk_texture_exact_by_id_locked(int id, view_t *view, int *has_view) {
    if (id < 0 || id >= ntslots || !tslots[id].used) return NULL;
    TexSlot *s = &tslots[id];
    Tex *e = s->ce && s->cgen == agen[sinfo[id] & 0xffff] ? s->ce : NULL;   // what the caller's lookup just resolved
    if (e && e->pub.skip) {
        if (exact.n >= EXACT_MAX) nvn_hm_clear(&exact, NULL);   // never seen; they would be found again one by one
        nvn_hm_put(&exact, &e->key, (void *)(uintptr_t)1);
        n_exact++;
        static volatile int32_t n_logged;
        log_limited(&n_logged, "texture detail: texture %#llx (fmt %#x %ux%u, %u levels) is read by texel position or its "
                    "size is asked for: made whole again (%llu so far)", (unsigned long long)e->pub.addr, e->t.d.format,
                    e->t.d.width, e->t.d.height, e->pub.levels + e->pub.skip, (unsigned long long)n_exact);
        drop_tex(e, DROP_EXACT);
    }
    return vk_texture_by_id_locked(id, view, has_view);
}

// vk_dump: the swizzle selectors of the descriptor registered in slot id; 0 when the slot is empty.
int vk_texture_slot_swz(int id, uint8_t swz[4]) {
    lock();
    int ok = id >= 0 && id < ntslots && tslots[id].used;
    if (ok) {
        const nvn_texdesc *d = &tslots[id].t.d;
        swz[0] = (uint8_t)d->swizzle_r, swz[1] = (uint8_t)d->swizzle_g, swz[2] = (uint8_t)d->swizzle_b;
        swz[3] = (uint8_t)d->swizzle_a;
    }
    unlock();
    return ok;
}

uint32_t vk_texture_epoch(void) { return __atomic_load_n(&submit_epoch, __ATOMIC_RELAXED); }

void vk_texture_new_submit(void) {
    lock();
    // the last one walked the pool, or checked its entries by list
    if (pool_by_list || (round_checks && (uint64_t)round_checks * 2 >= pool_slots)) look_round++;
    round_checks = 0, pool_by_list = 0;
    if (!++submit_epoch) submit_epoch = 1;
    rate_tick();
    sweep_pressure();
    static uint64_t log_at;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) / 60;
    if ((rt_trace & RT_TRACE_PERF) && f != log_at && tex_alias > 0) {
        log_at = f;
        rt_log("perf: frame %llu: texture aliases: content copied %llu (%llu from a released texture), filled from storage "
               "%llu, left uninitialised %llu; taken back with their own content %llu",
               (unsigned long long)f * 60, (unsigned long long)n_alias_copied, (unsigned long long)n_alias_released,
               (unsigned long long)n_alias_upload, (unsigned long long)n_alias_uninit, (unsigned long long)n_alias_kept);
        n_alias_copied = n_alias_upload = n_alias_uninit = n_alias_kept = n_alias_released = 0;
        if (n_dinit_inline || n_dinit_upload)
            rt_log("perf: frame %llu: new depth images started at the replay's position %llu (%llu ended an open pass), "
                   "in the upload batch %llu", (unsigned long long)f * 60, (unsigned long long)n_dinit_inline,
                   (unsigned long long)n_dinit_inpass, (unsigned long long)n_dinit_upload);
        n_dinit_inline = n_dinit_inpass = n_dinit_upload = 0;
        if (n_disuse_shared || n_disuse_diverted)
            rt_log("perf: frame %llu: unsampled textures kept because another descriptor shares their address %llu; "
                   "holds dropped by a mark that another entry at the address had taken %llu",
                   (unsigned long long)f * 60, (unsigned long long)n_disuse_shared, (unsigned long long)n_disuse_diverted);
        n_disuse_shared = n_disuse_diverted = 0;
        rt_log("perf: frame %llu: texture re-uploads %llu (%.1f MiB, %.2f ms), after WriteTexels %llu (%.1f MiB, %.2f ms); "
               "skipped with unchanged bytes %llu (%.1f MiB)",
               (unsigned long long)f * 60, (unsigned long long)ru_n, ru_bytes / 1048576.0, ru_ns / 1e6,
               (unsigned long long)ru_wt_n, ru_wt_bytes / 1048576.0, ru_wt_ns / 1e6, (unsigned long long)ru_same_n,
               ru_same_bytes / 1048576.0);
        ru_n = ru_bytes = ru_ns = ru_wt_n = ru_wt_bytes = ru_wt_ns = ru_same_n = ru_same_bytes = 0;
        uint64_t ck, calls, untr;
        vk_mem_snap_stats(&ck, &calls, &untr);
        rt_log("perf: frame %llu: binding storage checks %llu (texture %llu), write-watch snapshot queries %llu, untracked %llu",
               (unsigned long long)f * 60, (unsigned long long)ck, (unsigned long long)n_checks, (unsigned long long)calls,
               (unsigned long long)untr);
        n_checks = 0;
        rt_log("perf: frame %llu: released textures kept %d (%.0f MiB of own memory), dropped past the size limit %llu, "
               "under VRAM pressure %llu, under commit pressure %llu (made again soon after %llu, checks pausing those drops %llu; "
               "%llu checks went on to live textures with the released ones still above their bound); idle live textures evicted %llu (%.1f MiB); "
               "unsampled ones evicted %llu (%.1f MiB), retiring %d (%.1f MiB), freed %llu, taken back while retiring "
               "%llu, back %llu, held %u (marks read %u); images reused %llu, made %llu, pool bindings not tried %llu, "
               "superseded taken back %llu; render targets overwritten at another address %llu, dropped unused %llu",
               (unsigned long long)f * 60, nreleased, rel_bytes / 1048576.0, (unsigned long long)n_cap_drops,
               (unsigned long long)n_pressure_drops, (unsigned long long)n_commit_drops,
               (unsigned long long)n_commit_back, (unsigned long long)n_commit_held,
               (unsigned long long)n_commit_past, (unsigned long long)n_evict, n_evict_bytes / 1048576.0,
               (unsigned long long)n_disuse, n_disuse_bytes / 1048576.0, nretiring, retiring_bytes / 1048576.0,
               (unsigned long long)n_retire_freed, (unsigned long long)n_retire_cancel,
               (unsigned long long)n_disuse_back, holds.n, marks_reads, (unsigned long long)n_reused,
               (unsigned long long)n_reuse_new, (unsigned long long)n_bind_skips, (unsigned long long)n_restarts,
               (unsigned long long)n_overwritten, (unsigned long long)n_over_drops);
        n_cap_drops = n_pressure_drops = n_commit_drops = n_commit_past = n_evict = n_evict_bytes = 0;
        n_commit_back = n_commit_held = 0;
        n_disuse = n_disuse_bytes = n_disuse_back = n_retire_freed = n_retire_cancel = 0;
        n_reused = n_reuse_new = n_bind_skips = n_restarts = n_overwritten = n_over_drops = 0;
        rt_log("perf: frame %llu: texture origins: released entries dropped past RELEASED_MAX %llu, replaced by another "
               "descriptor at their address %llu; entries the CPU does not write made without content at an address it "
               "can see: on their own %llu, over a released entry %llu, over a live one %llu; marked slots that met an "
               "entry without content %llu, that show no entry %llu; GPU-drawn entries written back before their drop %llu",
               (unsigned long long)f * 60, (unsigned long long)n_drop_by[DROP_MAX], (unsigned long long)n_drop_by[DROP_REPLACED],
               (unsigned long long)n_cpu_empty[0], (unsigned long long)n_cpu_empty[1], (unsigned long long)n_cpu_empty[2],
               (unsigned long long)n_mark_empty, (unsigned long long)n_mark_none, (unsigned long long)n_drop_wb);
        memset(n_drop_by, 0, sizeof n_drop_by), memset(n_cpu_empty, 0, sizeof n_cpu_empty);
        n_mark_empty = n_mark_none = n_drop_wb = 0;
        rt_log("perf: frame %llu: texture finalizes: slots marked %llu; slots of another live texture with the same "
               "parameters at the storage, left current %llu (terrain heightmaps %llu); lookups the stale rule gave the "
               "stand-in %llu (terrain heightmaps %llu), their own kept image %llu (NX2NAVI_TEX_STALE_KEPT)",
               (unsigned long long)f * 60, (unsigned long long)n_fin_own, (unsigned long long)n_fin_other, (unsigned long long)n_fin_other_hm,
               (unsigned long long)n_stale, (unsigned long long)n_stale_hm, (unsigned long long)n_stale_kept);
        n_fin_own = n_fin_other = n_fin_other_hm = n_stale = n_stale_hm = n_stale_kept = 0;
        if (split_mode > 0)
            rt_log("perf: frame %llu: split fill: entries the CPU does not write started with their storage's bytes: "
                   "copied from the released entry they replaced %llu, filled from storage %llu (%.1f MiB, %.2f ms), "
                   "filled again after a write %llu; left empty: image taken from a released entry %llu, storage over "
                   "%llu MiB %llu", (unsigned long long)f * 60, (unsigned long long)n_split_copy,
                   (unsigned long long)n_split_fill, n_split_bytes / 1048576.0, n_split_ns / 1e6,
                   (unsigned long long)n_split_refill, (unsigned long long)n_split_reused,
                   (unsigned long long)(SPLIT_FILL_MAX >> 20), (unsigned long long)n_split_big);
        n_split_copy = n_split_fill = n_split_bytes = n_split_ns = n_split_refill = n_split_reused = n_split_big = 0;
    }
    unlock();
}

// ---------------------------------------------------------------- texel reads and writes
typedef struct { unsigned level, layer0; int nz, is3d, bw, bh, bpb; size_t wb, hb, row, img, x0, y0; } Region;

static int region_of(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint64_t row, uint64_t img, Region *g) {
    g->bpb = nvn_fmt_bpb(t->d.format, &g->bw, &g->bh);
    if (!g->bpb || r->width <= 0 || r->height <= 0 || r->depth < 0 || r->xoffset < 0 || r->yoffset < 0 || r->zoffset < 0 ||
        r->xoffset % g->bw || r->yoffset % g->bh)
        return 0;
    g->level = v && v->set_levels ? v->base_level : 0;
    g->layer0 = v && v->set_layers ? v->min_layer : 0;
    g->nz = r->depth ? r->depth : 1;
    g->is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    g->wb = (size_t)((r->width + g->bw - 1) / g->bw) * (size_t)g->bpb;
    g->hb = (size_t)(r->height + g->bh - 1) / (size_t)g->bh;
    g->row = row ? row : g->wb;
    g->img = img ? img : g->row * g->hb;
    g->x0 = (size_t)(r->xoffset / g->bw) * (size_t)g->bpb;
    g->y0 = (size_t)(r->yoffset / g->bh);
    return 1;
}

// Pool bytes <-> caller rows of the region alone (nvn_texels_box), not through a
// linear copy of the whole level: for a small region of a large texture that cost tens of milliseconds while the guest
// thread held its virtual core and the caller the texture lock (monsters2 replay, frames 765-790).
static int storage_texels(const tex_t *t, const Region *g, const NVNcopyRegion *r, uint8_t *mem, int write) {
    for (int z = 0; z < g->nz; z++) {
        unsigned layer = g->is3d ? 0 : g->layer0 + (unsigned)r->zoffset + (unsigned)z;
        uint32_t slice = g->is3d ? (uint32_t)(r->zoffset + z) : 0;
        if (nvn_texels_box(&t->d, g->level, layer, (uint32_t)g->x0, (uint32_t)g->y0, slice, (uint32_t)g->wb,
                           (uint32_t)g->hb, t->mem, mem + (size_t)z * g->img, g->row, !write))
            return 0;
    }
    return 1;
}

static int read_aspect(const Tex *e, VkImageAspectFlags *a) {
    if (!e->f.depth && !e->f.stencil) { *a = VK_IMAGE_ASPECT_COLOR_BIT; return 1; }
    switch (e->t.d.format) {   // packed depth-stencil storage has no single-aspect copy
    case 0x31: *a = VK_IMAGE_ASPECT_STENCIL_BIT; return 1;
    case 0x32: case 0x34: *a = VK_IMAGE_ASPECT_DEPTH_BIT; return 1;
    default: return 0;
    }
}

// An image region read through a staging buffer. texels_read_record records the copy into the open upload batch;
// texels_read_take copies the rows out once that batch is done (vk_upload_flush(1)) and frees the buffer. Several
// records before one flush cost one GPU round trip instead of one each.
typedef struct {
    VkBuffer sb;
    VmaAllocation sa;
    const uint8_t *stg;
    size_t slice, ib;   // ib: bytes per texel of an expanded RGB image, 0 when the image holds the storage layout
    uint32_t w;
} TexRead;

static int texels_read_record(Tex *e, const Region *g, const NVNcopyRegion *r, TexRead *t) {
    static volatile int32_t n_astc, n_ds;
    VkImageAspectFlags aspect;
    if (e->f.astc_bw) {
        log_limited(&n_astc, "ReadTexels of ASTC texture %#llx: the image holds decoded RGBA8; zeros returned",
                    (unsigned long long)e->pub.addr);
        return 0;
    }
    if (!read_aspect(e, &aspect)) {
        log_limited(&n_ds, "texels of packed depth-stencil format %#x are not copied", e->t.d.format);
        return 0;
    }
    if (g->level < e->pub.skip || g->level - e->pub.skip >= e->pub.levels) return 0;   // a level the image lacks
    uint32_t w = (uint32_t)r->width, h = (uint32_t)r->height, il = g->level - e->pub.skip;
    t->w = w;
    t->ib = e->f.expand_rgb ? (size_t)g->bpb / 3 * 4 : 0;
    t->slice = t->ib ? (size_t)w * h * t->ib : g->wb * g->hb;
    t->stg = vk_staging(t->slice * (size_t)g->nz, &t->sb, &t->sa);
    VkBufferImageCopy *rg = calloc((size_t)g->nz, sizeof *rg);
    if (!rg) rt_fatal("vk: out of memory for a texture read");
    for (int z = 0; z < g->nz; z++)
        rg[z] = (VkBufferImageCopy){.bufferOffset = t->slice * (size_t)z,
                                    .imageSubresource = {aspect, il, g->is3d ? 0 : g->layer0 + (uint32_t)r->zoffset + (uint32_t)z, 1},
                                    .imageOffset = {r->xoffset, r->yoffset, g->is3d ? r->zoffset + z : 0},
                                    .imageExtent = {w, h, 1}};
    VkCommandBuffer cb = vk_upload_begin();
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    vkCmdCopyImageToBuffer(cb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, t->sb, (uint32_t)g->nz, rg);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_HOST_READ_BIT);
    vk_upload_end();
    free(rg);
    return 1;
}

static void texels_read_take(const Tex *e, const Region *g, const TexRead *t, uint8_t *mem) {
    VkCtx *c = vk_ctx();
    vmaInvalidateAllocation(c->vma, t->sa, 0, VK_WHOLE_SIZE);
    for (int z = 0; z < g->nz; z++) {
        const uint8_t *s = t->stg + t->slice * (size_t)z;
        uint8_t *m = mem + (size_t)z * g->img;
        for (size_t y = 0; y < g->hb; y++) {
            if (t->ib) shrink_rgb(e->t.d.format, s + y * t->w * t->ib, m + y * g->row, t->w);
            else memcpy(m + y * g->row, s + y * g->wb, g->wb);
        }
    }
    vmaDestroyBuffer(c->vma, t->sb, t->sa);
}

// Image <-> caller rows through a staging buffer; the GPU copy is waited for.
static int image_texels(Tex *e, const Region *g, const NVNcopyRegion *r, uint8_t *mem, int write) {
    static volatile int32_t n_astc, n_ds;
    VkCtx *c = vk_ctx();
    VkImageAspectFlags aspect;
    if (!write) {
        TexRead t;
        if (!texels_read_record(e, g, r, &t)) return 0;
        vk_upload_flush(1);
        texels_read_take(e, g, &t, mem);
        return 1;
    }
    if (!read_aspect(e, &aspect)) {
        log_limited(&n_ds, "texels of packed depth-stencil format %#x are not copied", e->t.d.format);
        return 0;
    }
    // Texture detail: a level the image was made without takes no texels (the storage holds them, vk_write_texels); the
    // others are the image's level g->level - skip, the rectangle unchanged.
    if (g->level < e->pub.skip) return 1;
    uint32_t il = g->level - e->pub.skip;
    if (il >= e->pub.levels) return 0;
    e->had |= HAD_TEXELS, e->sfill = 0;   // its content is no longer the storage's (split fill)
    uint32_t w = (uint32_t)r->width, h = (uint32_t)r->height;
    int rgb = e->f.expand_rgb, astc = e->f.astc_bw != 0, agpu = astc && vk_astc_gpu();
    size_t texels = (size_t)w * h, ib = agpu ? 0 : astc ? 4 : rgb ? (size_t)g->bpb / 3 * 4 : 0;
    size_t slice = ib ? texels * ib : g->wb * g->hb;   // agpu: the raw blocks, decoded by vk_astc_record
    VkBuffer sb;
    VmaAllocation sa;
    uint8_t *stg = vk_staging(slice * (size_t)g->nz, &sb, &sa);
    VkBufferImageCopy *rg = calloc((size_t)g->nz, sizeof *rg);
    for (int z = 0; z < g->nz; z++) {
        uint8_t *s = stg + slice * (size_t)z;
        const uint8_t *m = mem + (size_t)z * g->img;
        if (agpu) {
            for (size_t y = 0; y < g->hb; y++) memcpy(s + y * g->wb, m + y * g->row, g->wb);
        } else if (astc) {
            uint8_t *tight = malloc(g->wb * g->hb);
            for (size_t y = 0; y < g->hb; y++) memcpy(tight + y * g->wb, m + y * g->row, g->wb);
            uint64_t a0 = perf_begin();
            astc_decode_ldr_mt(tight, w, h, e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb, s);
            perf_end(PERF_ASTC, a0);
            free(tight);
        } else if (rgb) {
            for (size_t y = 0; y < g->hb; y++) expand_rgb(e->t.d.format, m + y * g->row, s + y * w * ib, w);
        } else {
            for (size_t y = 0; y < g->hb; y++) memcpy(s + y * g->wb, m + y * g->row, g->wb);
        }
        rg[z] = (VkBufferImageCopy){.bufferOffset = slice * (size_t)z,
                                    .imageSubresource = {aspect, il, g->is3d ? 0 : g->layer0 + (uint32_t)r->zoffset + (uint32_t)z, 1},
                                    .imageOffset = {r->xoffset, r->yoffset, g->is3d ? r->zoffset + z : 0},
                                    .imageExtent = {w, h, 1}};
    }
    VkCommandBuffer cb = vk_upload_begin();
    vmaFlushAllocation(c->vma, sa, 0, VK_WHOLE_SIZE);
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    int done = 0;
    // A BC3 image takes whole 4x4 blocks, or a part that ends on the mip's edge; a region of a 5x5 or 6x6 footprint
    // may end inside a block (vk_astc_record checks the start corner).
    uint32_t mw = e->t.d.width >> g->level, mh = e->t.d.height >> g->level;
    int bc_cut = e->f.astc_bc && (((w & 3) && (uint32_t)r->xoffset + w != (mw ? mw : 1)) ||
                                  ((h & 3) && (uint32_t)r->yoffset + h != (mh ? mh : 1)));
    if (agpu && !bc_cut) {   // bc_cut: nothing recorded, logged below as not decoded
        VkAstcRegion *ar = calloc((size_t)g->nz, sizeof *ar);
        for (int z = 0; z < g->nz; z++)
            ar[z] = (VkAstcRegion){.src = slice * (size_t)z, .w = w, .h = h, .slices = 1, .level = rg[z].imageSubresource.mipLevel,
                                   .layer = rg[z].imageSubresource.baseArrayLayer, .offset = rg[z].imageOffset};
        uint64_t a0 = perf_begin();
        done = vk_astc_record(cb, sb, slice * (size_t)g->nz, ar, g->nz, e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb, e->f.astc_bc,
                              e->pub.image, aspect, VK_NULL_HANDLE);
        perf_end(PERF_ASTC, a0);
        free(ar);
    }
    if (!agpu) vkCmdCopyBufferToImage(cb, sb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)g->nz, rg);
    else if (done <= 0) log_limited(&n_astc, "WriteTexels of ASTC texture %#llx not decoded on the GPU: left as it was",
                                (unsigned long long)e->pub.addr);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    vk_upload_end();
    vk_upload_flush(1);
    free(rg);
    vmaDestroyBuffer(c->vma, sb, sa);
    return 1;
}

static void zero_rows(const Region *g, uint8_t *dst) {
    for (int z = 0; z < g->nz; z++)
        for (size_t y = 0; y < g->hb; y++) memset(dst + (size_t)z * g->img + y * g->row, 0, g->wb);
}

int vk_texture_read_image(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row, uint64_t img) {
    Region g;
    if (!t || !region_of(t, v, r, row, img, &g)) return 0;
    lock();
    Tex *e = entry_of(t);
    int ok = 0;
    if (e) {
        make_fresh(e);
        ok = image_texels(e, &g, r, dst, 0);
    }
    unlock();
    if (!ok) zero_rows(&g, dst);
    return ok;
}

// ReadTexels: the pool bytes are the content of a mirrored texture the GPU has not rendered into; every other texture
// is read back from its image.
void vk_read_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row, uint64_t img) {
    static volatile int32_t n_bad;
    Region g;
    if (!region_of(t, v, r, row, img, &g)) {
        log_limited(&n_bad, "ReadTexels: invalid region %d,%d,%d %dx%dx%d (format %#x)", r->xoffset, r->yoffset,
                    r->zoffset, r->width, r->height, r->depth, t->d.format);
        return;
    }
    lock();
    Tex *e = entry_of(t);
    int ok = 0;
    if (e && e->pub.mirrored && !e->pub.gpu_owned) ok = storage_texels(t, &g, r, dst, 0);
    else if (e) {
        make_fresh(e);
        ok = image_texels(e, &g, r, dst, 0);
    }
    unlock();
    if (!ok) zero_rows(&g, dst);
}

// WriteTexels: a mirrored texture gets the texels in its pool bytes (and in its image once the GPU owns it); every
// other texture in its image.
void vk_write_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, uint64_t row, uint64_t img) {
    static volatile int32_t n_bad;
    Region g;
    if (!region_of(t, v, r, row, img, &g) || (v && v->set_format && v->format != t->d.format)) {
        log_limited(&n_bad, "WriteTexels: invalid region %d,%d,%d %dx%dx%d or view format (format %#x)", r->xoffset,
                    r->yoffset, r->zoffset, r->width, r->height, r->depth, t->d.format);
        return;
    }
    lock();
    Tex *e = entry_of(t);
    if (e && e->pub.mirrored) {
        if (!storage_texels(t, &g, r, (uint8_t *)src, 1))
            log_limited(&n_bad, "WriteTexels: region outside level %u of texture %#llx", g.level, (unsigned long long)e->pub.addr);
        vk_guest_written((uintptr_t)t->mem, t->L.size);
        if (e->pub.gpu_owned) image_texels(e, &g, r, (uint8_t *)src, 1);
        else if (e->uploaded) e->wt_n++;
    } else if (e) {
        image_texels(e, &g, r, (uint8_t *)src, 1);
    }
    unlock();
}

// ---------------------------------------------------------------- CPU reads of GPU-written storage
// A mirrored texture the GPU rendered into holds its texels in its image only; its storage keeps what the CPU last put
// there. The game may still read that storage with the CPU: NX2navi moves its generated grass maps between pools with
// memcpy once the GPU is done, and the copy then carried zeros. Such a read writes the image back first.
//
// Every level and layer of every texture in v is copied to staging in one upload batch with one wait: one GPU round
// trip per level and layer took 0.1 ms each, and a 260x260 grass map has 9 levels. The storage is then written in the
// order of v, so where two textures share storage the later one wins.
typedef struct {
    Tex *e;
    Region g;
    NVNcopyRegion r;
    TexRead t;
} WbPart;

static void write_back_n(Tex **v, int n) {
    int np = 0, cap = 0;
    WbPart *p = NULL;
    for (int i = 0; i < n; i++) {
        Tex *e = v[i];
        const nvn_texdesc *d = &e->t.d;
        int linear = nvn_tex_is_linear(d), is3d = d->target == NVN_TEXTURE_TARGET_3D;
        uint32_t levels = linear ? 1 : e->pub.levels + e->pub.skip, layers = linear || is3d ? 1 : e->pub.layers;
        for (uint32_t lv = linear ? 0 : e->pub.skip; lv < levels; lv++)   // the game's levels the image holds
            for (uint32_t ly = 0; ly < layers; ly++) {
                if (np == cap) {
                    cap = cap ? cap * 2 : 16;
                    if (!(p = realloc(p, sizeof *p * (size_t)cap))) rt_fatal("vk: out of memory for a texture write-back");
                }
                WbPart *q = &p[np];
                view_t vw = {.set_levels = 1, .set_layers = 1, .base_level = (uint8_t)lv, .min_layer = (uint16_t)ly};
                q->e = e;
                q->r = (NVNcopyRegion){.width = (int)mip(d->width, lv),
                                       .height = d->target == 0 || d->target == 3 ? 1 : (int)mip(d->height, lv),
                                       .depth = is3d ? (int)mip(d->depth, lv) : 1};
                if (region_of(&e->t, &vw, &q->r, 0, 0, &q->g) && texels_read_record(e, &q->g, &q->r, &q->t)) np++;
            }
    }
    if (np) vk_upload_flush(1);
    for (int i = 0; i < np; i++) {
        WbPart *q = &p[i];
        uint8_t *buf = malloc(q->g.img * (size_t)q->g.nz);
        if (!buf) rt_fatal("vk: out of memory for a texture write-back");
        texels_read_take(q->e, &q->g, &q->t, buf);
        storage_texels(&q->e->t, &q->g, &q->r, buf, 1);
        free(buf);
    }
    free(p);
    for (int i = 0; i < n; i++) v[i]->gpu_dirty = 0;
}

// Mirrored textures overlapping [addr, addr+size) that the GPU wrote since their last write-back.
static int owned_dirty(uint64_t addr, uint64_t size, Tex **hit) {
    int n = 0;
    for (int i = owned_lower(addr > max_owned ? addr - max_owned : 0); i < nowned && owned[i]->pub.addr < addr + size; i++) {
        Tex *e = owned[i];
        if (__atomic_load_n(&e->gpu_dirty, __ATOMIC_RELAXED) && e->pub.addr + e->t.L.size > addr) {
            if (!hit) return 1;
            hit[n++] = e;
        }
    }
    return n;
}

const VkTexImage *vk_texture_at_locked(const VkTexImage *img, VkFormat format) {
    for (Tex *p = nvn_hm_get(&by_addr, &((Tex *)img)->key.addr); p; p = p->next_at)
        if (!p->released && p->pub.format == format && p->pub.width == img->width && p->pub.height == img->height)
            return &p->pub;
    return NULL;
}

// A storage image may be written by the shaders of the submission being recorded (texture lock held, vk_exec_draw.c
// tex_info). Its storage is not marked GPU-owned: only the write fence for ReadTexels.
void vk_texture_storage_bound_locked(const VkTexImage *img) {
    Tex *e = (Tex *)img;   // pub is the first member
    uint64_t f = vk_exec_recording_fence();
    if (f > e->wr_fence) e->wr_fence = f;
}

uint64_t vk_texture_last_write(const tex_t *t) {
    lock();
    Tex *e = entry_of(t);
    uint64_t f = e ? e->wr_fence : 0;
    unlock();
    return f;
}

size_t vk_texture_texel_span(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint64_t row, uint64_t img) {
    Region g;
    if (!region_of(t, v, r, row, img, &g)) return 0;
    return (size_t)(g.nz - 1) * g.img + (g.hb - 1) * g.row + g.wb;
}

int vk_texture_cpu_stale(uint64_t addr, uint64_t size) {
    if (!size || !__atomic_load_n(&nowned, __ATOMIC_ACQUIRE)) return 0;
    plat_mutex_lock_shared(&owned_lock);
    int dirty = owned_dirty(addr, size, NULL);
    plat_mutex_unlock_shared(&owned_lock);
    return dirty;
}

// The newest submission recorded to write a GPU-written mirrored texture overlapping [addr, addr+size); 0: none.
uint64_t vk_texture_dirty_fence(uint64_t addr, uint64_t size) {
    if (!size || !__atomic_load_n(&nowned, __ATOMIC_ACQUIRE)) return 0;
    lock();
    uint64_t f = 0;
    for (int i = owned_lower(addr > max_owned ? addr - max_owned : 0); i < nowned && owned[i]->pub.addr < addr + size; i++) {
        const Tex *e = owned[i];
        if (!e->gpu_dirty || e->pub.addr + e->t.L.size <= addr) continue;
        if (e->gpu_fence > f) f = e->gpu_fence;
        if (e->wr_fence > f) f = e->wr_fence;
    }
    unlock();
    return f;
}

// The image is the newest content only while nothing wrote the storage since the GPU render: NX2navi finalizes grass maps,
// reuses their storage for new data and memcpys that data on, and writing the old image back first handed the copy
// a stale map or stale resource bytes (wrong textures in the distance). Every hit is checked before any is written
// back, since a write-back is itself a CPU write to storage another hit may share; the hits then go back oldest render
// first, so where two descriptors share storage the newer render wins.
void vk_texture_write_back(uint64_t addr, uint64_t size) {
    lock();
    Tex **hit = malloc(sizeof *hit * (size_t)(nowned + 1));
    if (!hit) rt_fatal("vk: out of memory for a texture write-back");
    int n = owned_dirty(addr, size, hit), k = 0;   // owned[] cannot change while tex_lock is held
    for (int i = 0; i < n; i++) {
        uint32_t now;
        if (storage_rewritten(hit[i], hit[i]->gpu_gen, &now) == 1) hit[i]->gpu_dirty = 0;
        else hit[k++] = hit[i];
    }
    for (int i = 1; i < k; i++)
        for (int j = i; j > 0 && hit[j - 1]->gpu_seq > hit[j]->gpu_seq; j--) {
            Tex *x = hit[j];
            hit[j] = hit[j - 1], hit[j - 1] = x;
        }
    write_back_n(hit, k);
    for (int i = 0; i < k; i++) perf_count(PERF_CNT_WB);
    for (int i = k; i < n; i++) perf_count(PERF_CNT_WB_SKIP);
    free(hit);
    unlock();
}

// NX2navi's grass system renders a 128x128 RGBA8 map of cut and trampled grass into a CPU-cached pool every frame and its
// CPU code reads the pool bytes without nvnMemoryPoolInvalidateMappedRange (cut reactions: blades, insects, the cut
// itself). On the Switch the bytes are there once the frame's fence has passed. So when the
// CPU waits for a fence, mirrored targets rendered by work up to that fence go back to their storage (wb_wanted:
// targets over wb_limit only when not drawn every frame), unless the CPU rewrote it since. Targets drawn every frame
// still go back on an invalidate, memcpy or ReadTexels.
int vk_texture_small_dirty(void) { return __atomic_load_n(&small_pending, __ATOMIC_ACQUIRE); }

// NX2NAVI_TRACE=gfx: a dirty target the fence write-back leaves alone because it is over the limit and drawn every frame
// (what a CPU read after the wait would miss), or sends back although it is over the default limit. One line per
// target address and outcome; the table is small and a full one stops logging.
static void wb_note(const Tex *e, int skipped) {
    static uint64_t seen[2][64];
    static int n_seen[2];
    for (int i = 0; i < n_seen[skipped]; i++)
        if (seen[skipped][i] == e->pub.addr) return;
    if (n_seen[skipped] == 64) return;
    seen[skipped][n_seen[skipped]++] = e->pub.addr;
    const pool_t *p = e->t.pool;
    unsigned fl = p ? p->flags : 0;
    rt_log("vk: fence write-back %s %#llx %ux%u fmt %#x size %llu pool %s (limit %llu)",
           skipped ? "skipped-drawn-every-frame" : "over-default", (unsigned long long)e->pub.addr, e->t.d.width, e->t.d.height,
           e->t.d.format, (unsigned long long)e->t.L.size,
           !p ? "none" : (fl & NVN_MEMORY_POOL_FLAGS_CPU_CACHED) ? "cpu-cached" : (fl & NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED) ? "cpu-uncached" : "other",
           (unsigned long long)wb_limit);
}

void vk_texture_write_back_small(uint64_t fence) {
    lock();
    wb_cfg_init();
    __atomic_store_n(&small_pending, 0, __ATOMIC_RELAXED);
    plat_mutex_lock_shared(&owned_lock);
    int n = 0, later = 0;
    for (int i = 0; i < nowned; i++) n += owned[i]->gpu_dirty && wb_wanted(owned[i]);
    Tex **hit = n ? malloc(sizeof *hit * (size_t)n) : NULL;
    if (n && !hit) rt_fatal("vk: out of memory for a texture write-back");
    n = 0;
    for (int i = 0; i < nowned; i++) {
        Tex *e = owned[i];
        if (!e->gpu_dirty) continue;
        if (!wb_wanted(e)) {
            if (wb_trace && e->gpu_fence <= fence) {
                wb_note(e, 1);
                perf_count(PERF_CNT_WB_SKIP);
            }
            continue;
        }
        if (e->gpu_fence > fence) later = 1;
        else {
            if (wb_trace && e->t.L.size > WB_SMALL_DEFAULT) wb_note(e, 0);
            hit[n++] = e;
        }
    }
    plat_mutex_unlock_shared(&owned_lock);
    if (later) __atomic_store_n(&small_pending, 1, __ATOMIC_RELEASE);
    int k = 0;
    for (int i = 0; i < n; i++) {
        uint32_t now;
        if (storage_rewritten(hit[i], hit[i]->gpu_gen, &now) == 1) hit[i]->gpu_dirty = 0;
        else hit[k++] = hit[i];
    }
    for (int i = 1; i < k; i++)
        for (int j = i; j > 0 && hit[j - 1]->gpu_seq > hit[j]->gpu_seq; j--) {
            Tex *x = hit[j];
            hit[j] = hit[j - 1], hit[j - 1] = x;
        }
    write_back_n(hit, k);
    for (int i = 0; i < k; i++) perf_count(PERF_CNT_WB);
    for (int i = k; i < n; i++) perf_count(PERF_CNT_WB_SKIP);
    free(hit);
    unlock();
}

// ---------------------------------------------------------------- samplers (LAS textures.c)
static VkCompareOp compare_op(uint32_t f) {
    switch (f) {
    case NVN_COMPARE_FUNC_NEVER: return VK_COMPARE_OP_NEVER;
    case NVN_COMPARE_FUNC_LESS: return VK_COMPARE_OP_LESS;
    case NVN_COMPARE_FUNC_EQUAL: return VK_COMPARE_OP_EQUAL;
    case NVN_COMPARE_FUNC_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case NVN_COMPARE_FUNC_GREATER: return VK_COMPARE_OP_GREATER;
    case NVN_COMPARE_FUNC_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case NVN_COMPARE_FUNC_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static struct { int init, mirror_clamp_edge, minmax, border_no_format; float aniso; } sf;   // aniso: NX2NAVI_ANISO, 0 = the game's

static float aniso_setting(void) {
    const char *an = cfg_value("NX2NAVI_ANISO");
    return strcmp(an, "game") ? (float)atoi(an) : 0;
}

static void aniso_log(void) {
    if (sf.aniso > 1) rt_log("vk: anisotropic filtering: %gx", sf.aniso);
    else rt_log("vk: anisotropic filtering: the game's");
}

// Caller holds the lock.
static void sampler_features(VkCtx *c) {
    if (sf.init) return;
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .pNext = c->has_custom_border_color ? &cbf : NULL};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(c->phys, &f2);
    sf.mirror_clamp_edge = f12.samplerMirrorClampToEdge;
    sf.minmax = f12.samplerFilterMinmax;
    sf.border_no_format = c->has_custom_border_color && cbf.customBorderColors && cbf.customBorderColorWithoutFormat;
    sf.aniso = aniso_setting();
    aniso_log();
    sf.init = 1;
}

// NX2NAVI_ANISO raises the level for filtered, mipmapped colour samplers that repeat on U and V: the surface textures,
// which tile, so ground and walls seen at a shallow angle stay sharp into the distance. Depth-compare (shadow)
// samplers, nearest or unmipmapped ones (data maps, LUTs) and clamped ones keep the game's level: clamped samplers
// read screen-sized buffers, whose coordinates can stretch steeply at an object's edge, where anisotropic taps reach
// across it and leave a speckled fringe (the inventory's character preview). The game's own wrap modes decide: a
// mirror-clamp one maps to MIRRORED_REPEAT where the device lacks mirror-clamp-to-edge (make_sampler). Caller holds the
// lock, after sampler_features.
static int aniso_raised(const samp_par *d) {
    int tiled = (d->ws == NVN_WRAP_MODE_REPEAT || d->ws == NVN_WRAP_MODE_MIRRORED_REPEAT) &&
                (d->wt == NVN_WRAP_MODE_REPEAT || d->wt == NVN_WRAP_MODE_MIRRORED_REPEAT);
    return sf.aniso > d->aniso && tiled && !d->cmp_mode && d->magf == NVN_MAG_FILTER_LINEAR &&
           (d->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST || d->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR);
}

static VkSampler make_sampler(VkCtx *c, const samp_par *d) {
    sampler_features(c);
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = d->magf == NVN_MAG_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    uint32_t mf = d->minf;
    si.minFilter = mf == 1 || mf == 3 || mf == 5 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = mf == 4 || mf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode modes[3];
    const uint8_t wrap[3] = {d->ws, d->wt, d->wr};
    int border = 0;
    for (int i = 0; i < 3; i++) {
        switch (wrap[i]) {
        case NVN_WRAP_MODE_CLAMP: case NVN_WRAP_MODE_CLAMP_TO_EDGE: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
        case NVN_WRAP_MODE_MIRROR_CLAMP: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE:
            modes[i] = sf.mirror_clamp_edge ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            break;
        case NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER: case NVN_WRAP_MODE_CLAMP_TO_BORDER:
            modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            border = 1;
            break;
        case NVN_WRAP_MODE_MIRRORED_REPEAT: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        default: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    }
    si.addressModeU = modes[0], si.addressModeV = modes[1], si.addressModeW = modes[2];
    si.minLod = d->lod_min;
    si.maxLod = d->lod_max > 1000 ? VK_LOD_CLAMP_NONE : d->lod_max;
    if (mf <= NVN_MIN_FILTER_LINEAR) si.maxLod = si.minLod + 0.25f;   // not mipmapped: stay on the base level
    if (si.maxLod < si.minLod) si.maxLod = si.minLod;
    float lim = c->props.limits.maxSamplerLodBias;
    si.mipLodBias = fmaxf(-lim, fminf(lim, d->lod_bias));
    // d->aniso holds NX2NAVI_ANISO's level where it applies (sampler_for, aniso_raised)
    if (d->aniso > 1) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = fminf(d->aniso, c->props.limits.maxSamplerAnisotropy);
    }
    if (d->cmp_mode) {
        si.compareEnable = VK_TRUE;
        si.compareOp = compare_op(d->cmp_func);
        // A depth-compare sampler set to nearest returns 0 or 1 per tap, and the game's shaders smooth shadow edges by
        // a few taps spaced for its own shadow map texels. With NX2NAVI_SHADOW_SCALE those taps cover a fraction of the
        // area and the edges turn hard and stepped; linear filtering makes every tap a 2x2 PCF instead.
        if (shadow_scale() > 1) si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    }
    VkSamplerCustomBorderColorCreateInfoEXT cb = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    VkSamplerReductionModeCreateInfo rm = {.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
    const void **tail = &si.pNext;
    if (border) {
        const float *b = d->border;
        if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0) si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        else if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        else if (b[0] == 1 && b[1] == 1 && b[2] == 1 && b[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else if (sf.border_no_format) {
            memcpy(cb.customBorderColor.float32, b, 16);
            si.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            *tail = &cb;
            tail = &cb.pNext;
        } else si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    if (d->reduction && sf.minmax && !si.compareEnable) {
        rm.reductionMode = d->reduction == NVN_SAMPLER_REDUCTION_MIN ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX;
        *tail = &rm;
    }
    VkSampler s;
    VkResult r = vkCreateSampler(c->device, &si, NULL, &s);
    if (r != VK_SUCCESS) rt_fatal("vk: vkCreateSampler failed (%d)", (int)r);
    return s;
}

void vk_sampler_register(int id, const samp_par *s) {
    if (id < 0 || id >= (1 << 12)) rt_fatal("vk: sampler slot %d out of range", id);
    lock();
    if (id >= nsslots) {
        int n = nsslots ? nsslots : 256;
        while (n <= id) n *= 2;
        SampSlot *p = realloc(sslots, sizeof *p * (size_t)n);
        if (!p) rt_fatal("vk: out of memory for sampler slots");
        memset(p + nsslots, 0, sizeof *p * (size_t)(n - nsslots));
        sslots = p, nsslots = n;
    }
    sslots[id].p = *s;
    sslots[id].s = sslots[id].sp = sslots[id].sc = sslots[id].sd[0] = sslots[id].sd[1] = VK_NULL_HANDLE;
    sslots[id].used = 1;
    samp_gen++;
    unlock();
}

// DLSS upscaling (vk_sampler_dlss_bias): the scene's material textures, the filtered mipmapped ones that repeat, are
// sampled for the output's resolution; clamped ones (post effects, LUTs, the UI) keep the game's bias.
static float dlss_bias;

static int dlss_biased(const samp_par *k) {
    int rep_s = k->ws == NVN_WRAP_MODE_REPEAT || k->ws == NVN_WRAP_MODE_MIRRORED_REPEAT;
    int rep_t = k->wt == NVN_WRAP_MODE_REPEAT || k->wt == NVN_WRAP_MODE_MIRRORED_REPEAT;
    return !k->cmp_mode && k->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR && (rep_s || rep_t);
}

void vk_sampler_dlss_bias(float b) {
    lock();
    if (b != dlss_bias) {
        rt_log("vk: texture LOD bias %.2f on the scene's tiled textures (the upscaler's render scale; was %.2f)", b,
               dlss_bias);
        dlss_bias = b;
        for (int i = 0; i < nsslots; i++)   // looked up again (the key moves)
            sslots[i].s = sslots[i].sp = sslots[i].sc = sslots[i].sd[0] = sslots[i].sd[1] = VK_NULL_HANDLE;
        samp_gen++;
    }
    unlock();
}

// NX2NAVI_ANISO, read again at each present after a settings change (vk_backend.c present_image, beside the DLSS bias of
// vk_dlaa_frame): the raised level is part of the sampler key (sampler_for), so on a change the slots look their
// samplers up again and the descriptor caches follow samp_gen, as for vk_sampler_dlss_bias. The old level's samplers
// stay until shutdown like every sampler: one set per level at most. Before the first sampler there is nothing to do:
// sampler_features reads the setting then.
void vk_sampler_aniso_live(void) {
    static unsigned seen = ~0u;
    unsigned v = cfg_version();
    if (v == seen) return;
    seen = v;
    lock();
    float a = aniso_setting();
    if (sf.init && a != sf.aniso) {
        sf.aniso = a;
        aniso_log();
        for (int i = 0; i < nsslots; i++)   // looked up again (the key moves)
            sslots[i].s = sslots[i].sp = sslots[i].sc = sslots[i].sd[0] = sslots[i].sd[1] = VK_NULL_HANDLE;
        samp_gen++;
    }
    unlock();
}

// Caller holds the lock.
static VkSampler sampler_for(VkCtx *c, const samp_par *s) {
    sampler_features(c);
    samp_par k;
    memset(&k, 0, sizeof k);
    k = *s;
    if (dlss_bias != 0 && dlss_biased(&k)) k.lod_bias += dlss_bias;   // part of the key: samplers live until shutdown
    if (aniso_raised(&k)) k.aniso = sf.aniso;                          // likewise
    VkSampler v = (VkSampler)nvn_hm_get(&samplers, &k);
    if (!v) {
        v = make_sampler(c, &k);
        nvn_hm_put(&samplers, &k, (void *)v);
    }
    return v;
}

VkSampler vk_sampler_for(const samp_par *s) {
    VkCtx *c = vk_ctx();
    if (!c || !s) return VK_NULL_HANDLE;
    lock();
    VkSampler v = sampler_for(c, s);
    unlock();
    return v;
}

// Samplers live until shutdown, so a slot keeps the one its parameters map to until it is registered again.
VkSampler vk_sampler_by_id_locked(int id) {
    VkCtx *c = vk_ctx();
    if (!c) return VK_NULL_HANDLE;
    VkSampler v = VK_NULL_HANDLE;
    if (id >= 0 && id < nsslots && sslots[id].used) {
        SampSlot *s = &sslots[id];
        if (!s->s) s->s = sampler_for(c, &s->p);
        v = s->s;
    }
    return v;
}

VkSampler vk_sampler_point_by_id_locked(int id) {
    VkCtx *c = vk_ctx();
    if (!c || id < 0 || id >= nsslots || !sslots[id].used) return VK_NULL_HANDLE;
    SampSlot *s = &sslots[id];
    if (!s->sp) {
        samp_par k = s->p;
        k.magf = NVN_MAG_FILTER_NEAREST;
        k.minf = k.minf == NVN_MIN_FILTER_LINEAR ? NVN_MIN_FILTER_NEAREST :
                 k.minf > NVN_MIN_FILTER_LINEAR ? NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST : k.minf;
        k.aniso = 1;
        s->sp = sampler_for(c, &k);
    }
    return s->sp;
}

// A depth-compare read through a sampler without a compare is undefined in Vulkan. NVIDIA's driver compares anyway; AMD's
// returns the raw depth, so the game's shadow tests (which sample this way) come out wrong there. Such a read gets the
// slot's sampler with the compare on: the game's function when it set a valid one, LEQUAL otherwise, as NVIDIA's driver
// compares. NX2NAVI_SHADOW_COMPARE=0 binds the game's sampler as it is.
VkSampler vk_sampler_compare_by_id_locked(int id) {
    VkCtx *c = vk_ctx();
    if (!c || id < 0 || id >= nsslots || !sslots[id].used) return VK_NULL_HANDLE;
    SampSlot *s = &sslots[id];
    static int on = -1;
    if (on < 0) on = cfg_int("NX2NAVI_SHADOW_COMPARE") != 0;
    if (s->p.cmp_mode || !on) return vk_sampler_by_id_locked(id);
    if (!s->sc) {
        samp_par k = s->p;
        k.cmp_mode = 1;
        if (k.cmp_func < NVN_COMPARE_FUNC_NEVER || k.cmp_func > NVN_COMPARE_FUNC_ALWAYS) k.cmp_func = NVN_COMPARE_FUNC_LEQUAL;
        static int told;
        if (told++ < 8)
            rt_log("vk: a depth-compare read through sampler slot %d without a compare (function %u): compared with %u", id,
                   (unsigned)s->p.cmp_func, (unsigned)k.cmp_func);
        s->sc = sampler_for(c, &k);
    }
    return s->sc;
}

// Texture detail: a texture made without its top `skip` levels has the game's level i + skip at its level i, and the
// LOD the device works out for it is the game's less skip, so the level a sample lands on is the game's wherever that
// level is kept. A sampler's LOD clamp counts the game's levels and has to move down with them: a clamp to levels 0-2
// left alone would let the image's level 2 through, the game's level 4. The bias is relative and stays. A clamp that is
// open (from 0, up to more levels than a texture has) is the same sampler. Only draws that bind texture and sampler
// as one resource come here (vk_exec_draw.c tex_info_full); the sampler pool's entries serve every texture and keep
// the game's clamp.
VkSampler vk_sampler_detail_by_id_locked(int id, uint32_t skip) {
    VkCtx *c = vk_ctx();
    if (!skip || skip > 2) return vk_sampler_by_id_locked(id);
    if (!c || id < 0 || id >= nsslots || !sslots[id].used) return VK_NULL_HANDLE;
    SampSlot *s = &sslots[id];
    if (!s->sd[skip - 1]) {
        samp_par k = s->p;
        if (k.lod_min > 0) k.lod_min = fmaxf(k.lod_min - (float)skip, 0);
        if (k.lod_max < 32) k.lod_max = fmaxf(k.lod_max - (float)skip, 0);   // 32 and more: above any texture's last level
        if (k.lod_min == s->p.lod_min && k.lod_max == s->p.lod_max) {
            s->sd[skip - 1] = vk_sampler_by_id_locked(id);
        } else {
            static int told;   // a clamp that leaves levels out; one up to a texture's last level changes nothing
            if ((s->p.lod_min > 0 || s->p.lod_max < 8) && told++ < 8)
                rt_log("vk: texture detail: sampler slot %d clamps the LOD to %g..%g; %g..%g for a texture made without %u "
                       "levels", id, s->p.lod_min, s->p.lod_max, k.lod_min, k.lod_max, skip);
            s->sd[skip - 1] = sampler_for(c, &k);
        }
    }
    return s->sd[skip - 1];
}

VkSampler vk_sampler_by_id(int id) {
    lock();
    VkSampler v = vk_sampler_by_id_locked(id);
    unlock();
    return v;
}

int vk_sampler_params(int id, samp_par *out) {
    lock();
    int ok = id >= 0 && id < nsslots && sslots[id].used;
    if (ok) *out = sslots[id].p;
    unlock();
    return ok;
}

// ---------------------------------------------------------------- statistics and shutdown
void vk_texture_stats(uint64_t *images, uint64_t *uploads, uint64_t *reuploads) {
    lock();
    if (images) *images = by_key.n;
    if (uploads) *uploads = n_uploads;
    if (reuploads) *reuploads = n_reuploads;
    unlock();
}

void vk_texture_census(VkTexCensus *s) {
    VkCtx *c = vk_ctx();
    memset(s, 0, sizeof *s);
    lock();
    for (uint32_t i = 0; i < by_key.cap; i++) {
        const Tex *e = by_key.vals[i];
        if (!e) continue;
        uint64_t bytes = 0;
        if (e->alloc && c) {
            VmaAllocationInfo ai;
            vmaGetAllocationInfo(c->vma, e->alloc, &ai);
            bytes = ai.size;
        }
        if (e->released) s->released++, s->own_released += e->alloc != NULL, s->bytes_released += bytes;
        else s->live++, s->own_live += e->alloc != NULL, s->bytes_live += bytes;
    }
    unlock();
}

// With NX2NAVI_TRACE=perf every 600 frames, after vk_memory.c's vram line: what the live entries' own memory holds. By
// kind: decoded ASTC (as BC3 or RGBA8) and other textures whose content the CPU provides, all of which a re-upload
// brings back (comes_back_intact), and the GPU-written ones (render targets, textures the GPU drew or copied into),
// whose content lives only in the image. "idle" is the part no lookup reached for IDLE_ROUNDS pool walks and
// STALE_FRAMES presents (touch). Then the entries that share their storage address with another live entry (each
// descriptor of the same storage gets an image of its own), the formats holding the most, and the largest entries.
void vk_texture_census_log(uint64_t frame) {
    enum { K_BC1, K_BC3, K_RGBA8, K_CPU, K_GPU, K_N };
    static const char *const kn[K_N] = {"ASTC as BC1", "ASTC as BC3", "ASTC as RGBA8", "other CPU-written", "GPU-written"};
    uint64_t n[K_N] = {0}, b[K_N] = {0}, in[K_N] = {0}, ib[K_N] = {0}, bound = 0, dup_n = 0, dup_b = 0;
    uint64_t sk_n[3] = {0}, sk_b[3] = {0};   // texture detail: entries made without 1 and 2 levels, and their memory
    struct { int fmt; uint64_t n, b; } fm[48];
    int nfm = 0;
    struct { uint64_t b; uint32_t w, h, d, levels; int fmt, gpu; } big[4] = {{0}};   // copied: read after unlock
    // the addresses whose extra entries hold the most, with their largest few entries (copied: read after unlock)
    struct { uint64_t addr, cnt, extra; struct { uint64_t b; uint32_t w, h, d; int fmt, gpu; } e[4]; } dup[3] = {{0}};
    const double M = 1048576.0;
    lock();
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < by_key.cap; i++) {
        const Tex *e = by_key.vals[i];
        if (!e || e->released) continue;
        if (!e->bytes) { bound += e->bound_to != NULL; continue; }
        int k = !comes_back_intact(e) ? K_GPU
              : e->f.astc_bw ? (e->f.astc_bc == 2 ? K_BC1 : e->f.astc_bc ? K_BC3 : K_RGBA8) : K_CPU;
        n[k]++, b[k] += e->bytes;
        if (e->pub.skip < 3) sk_n[e->pub.skip]++, sk_b[e->pub.skip] += e->bytes;
        if (idle(e, now)) in[k]++, ib[k] += e->bytes;
        int j = 0;
        while (j < nfm && fm[j].fmt != (int)e->f.vk) j++;
        if (j == nfm && nfm < 48) fm[nfm++] = (typeof(fm[0])){(int)e->f.vk, 0, 0};
        if (j < nfm) fm[j].n++, fm[j].b += e->bytes;
        for (int q = 0; q < 4; q++)
            if (e->bytes > big[q].b) {
                memmove(big + q + 1, big + q, sizeof *big * (size_t)(3 - q));
                big[q] = (typeof(big[0])){e->bytes, e->pub.width, e->pub.height, e->pub.layers * e->pub.depth,
                                          e->pub.levels, (int)e->f.vk, k == K_GPU};
                break;
            }
    }
    for (uint32_t i = 0; i < by_addr.cap; i++) {   // live entries with own memory at one address, beyond the largest
        uint64_t cnt = 0, sum = 0, most = 0;
        for (const Tex *p = by_addr.vals[i]; p; p = p->next_at)
            if (!p->released && p->bytes) cnt++, sum += p->bytes, most = p->bytes > most ? p->bytes : most;
        if (cnt > 1) dup_n += cnt - 1, dup_b += sum - most;
        for (int q = 0; cnt > 1 && q < 3; q++) {
            if (sum - most <= dup[q].extra) continue;
            memmove(dup + q + 1, dup + q, sizeof *dup * (size_t)(2 - q));
            memset(&dup[q], 0, sizeof dup[q]);
            dup[q].cnt = cnt, dup[q].extra = sum - most;
            for (const Tex *p = by_addr.vals[i]; p; p = p->next_at) {
                if (p->released || !p->bytes) continue;
                dup[q].addr = p->pub.addr;
                for (int x = 0; x < 4; x++)
                    if (p->bytes > dup[q].e[x].b) {
                        memmove(dup[q].e + x + 1, dup[q].e + x, sizeof dup[q].e[0] * (size_t)(3 - x));
                        dup[q].e[x].b = p->bytes, dup[q].e[x].w = p->pub.width, dup[q].e[x].h = p->pub.height;
                        dup[q].e[x].d = p->pub.layers * p->pub.depth, dup[q].e[x].fmt = (int)p->f.vk;
                        dup[q].e[x].gpu = !comes_back_intact(p);
                        break;
                    }
            }
            break;
        }
    }
    unlock();
    char line[1024];
    int o = snprintf(line, sizeof line, "mem: textures frame %llu: live with own memory by kind, idle part in brackets:",
                     (unsigned long long)frame);
    for (int k = 0; k < K_N; k++)
        o += snprintf(line + o, sizeof line - (size_t)o, " %s %llu %.0f MiB (%llu %.0f MiB);", kn[k], (unsigned long long)n[k],
                      b[k] / M, (unsigned long long)in[k], ib[k] / M);
    o += snprintf(line + o, sizeof line - (size_t)o, " bound into pool memory %llu; sharing an address with another %llu %.0f MiB",
                  (unsigned long long)bound, (unsigned long long)dup_n, dup_b / M);
    if (detail_most())   // nothing more in the line at full detail
        snprintf(line + o, sizeof line - (size_t)o, "; texture detail: made without their top level %llu %.0f MiB, without "
                 "two %llu %.0f MiB, made whole again for a shader %llu (%u shaders loaded read a texture by texel "
                 "position or size)", (unsigned long long)sk_n[1], sk_b[1] / M, (unsigned long long)sk_n[2], sk_b[2] / M,
                 (unsigned long long)n_exact, gfx_shader_exact_seen());
    rt_log("%s", line);
    if (vk_astc_bc1())
        rt_log("mem: ASTC as BC1 so far: %llu textures made BC1 at their first upload (%.0f MiB, half of what BC3 takes), "
               "%llu made BC3 again for content with alpha; checks in the uploads' copies %llu (%.0f MiB), of the storage "
               "before an upload that untiles on the CPU %llu (%.0f MiB) in %.1f ms", (unsigned long long)n_bc1_made,
               n_bc1_made_bytes / M, (unsigned long long)n_bc1_widened, (unsigned long long)n_bc1_up_checks,
               n_bc1_up_bytes / M, (unsigned long long)n_bc1_checks, n_bc1_check_bytes / M, n_bc1_check_ns / 1e6);
    for (int x = 0; x < nfm; x++)   // by bytes, largest first
        for (int y = x + 1; y < nfm; y++)
            if (fm[y].b > fm[x].b) { typeof(fm[0]) t = fm[x]; fm[x] = fm[y], fm[y] = t; }
    o = snprintf(line, sizeof line, "mem: textures frame %llu: VkFormats holding the most:", (unsigned long long)frame);
    for (int x = 0; x < nfm && x < 8; x++)
        o += snprintf(line + o, sizeof line - (size_t)o, " %d: %llu %.0f MiB;", fm[x].fmt, (unsigned long long)fm[x].n,
                      fm[x].b / M);
    o += snprintf(line + o, sizeof line - (size_t)o, " largest:");
    for (int q = 0; q < 4 && big[q].b; q++)
        o += snprintf(line + o, sizeof line - (size_t)o, " %ux%ux%u fmt %d %u levels %.0f MiB%s;", big[q].w, big[q].h,
                      big[q].d, big[q].fmt, big[q].levels, big[q].b / M, big[q].gpu ? " (GPU-written)" : "");
    rt_log("%s", line);
    if (!dup[0].cnt) return;
    // Which entries share an address: each descriptor of the same storage (another size, format or level count) gets
    // an image of its own, and its memory beside the others'.
    o = snprintf(line, sizeof line, "mem: textures frame %llu: addresses with the most extra entries:", (unsigned long long)frame);
    for (int q = 0; q < 3 && dup[q].cnt && o < (int)sizeof line - 200; q++) {
        o += snprintf(line + o, sizeof line - (size_t)o, " %#llx: %llu entries, %.0f MiB beyond the largest (",
                      (unsigned long long)dup[q].addr, (unsigned long long)dup[q].cnt, dup[q].extra / M);
        for (int x = 0; x < 4 && dup[q].e[x].b && o < (int)sizeof line - 80; x++)
            o += snprintf(line + o, sizeof line - (size_t)o, "%s%ux%ux%u fmt %d %.1f MiB%s", x ? ", " : "", dup[q].e[x].w,
                          dup[q].e[x].h, dup[q].e[x].d, dup[q].e[x].fmt, dup[q].e[x].b / M, dup[q].e[x].gpu ? " GPU-written" : "");
        o += snprintf(line + o, sizeof line - (size_t)o, ");");
    }
    rt_log("%s", line);
}

void vk_texture_shutdown(void) {
    VkCtx *c = vk_ctx();
    lock();
    Tex **list = malloc(sizeof *list * (size_t)(by_key.n + 1));
    int n = 0;
    for (uint32_t i = 0; i < by_key.cap; i++)
        if (by_key.vals[i]) list[n++] = by_key.vals[i];
    for (int i = 0; i < n; i++) drop_tex(list[i], DROP_SHUTDOWN);
    free(list);
    for (uint32_t i = 0; i < samplers.cap; i++)
        if (samplers.vals[i] && c) vkDestroySampler(c->device, (VkSampler)samplers.vals[i], NULL);
    free(by_key.keys), free(by_key.vals), free(by_addr.keys), free(by_addr.vals), free(samplers.keys), free(samplers.vals);
    by_key = (nvn_hmap){.klen = sizeof(TexKey)};
    by_addr = (nvn_hmap){.klen = 8};
    for (uint32_t i = 0; i < img_reqs.cap; i++) free(img_reqs.vals[i]);
    free(img_reqs.keys), free(img_reqs.vals), free(reuse.keys), free(reuse.vals);   // reuse: emptied by destroy_tex
    img_reqs = (nvn_hmap){.klen = sizeof(ImgKey)};
    reuse = (nvn_hmap){.klen = sizeof(ImgKey)};
    samplers = (nvn_hmap){.klen = sizeof(samp_par)};
    free(tslots), free(sslots);
    tslots = NULL, sslots = NULL, ntslots = nsslots = 0, nreleased = 0, pool_slots = 0;
    rel_head = rel_tail = NULL, rel_bytes = 0;
    if (sinfo) memset(sinfo, 0, sizeof *sinfo << 20);   // no slot is registered: tokens 0
    unlock();
}

// NX2NAVI_TRACE=perf "mem: sizes" (perf.c): the texture cache's entries (the bytes are in the "mem: vram" line), the
// released ones, those retired by disuse, the sampler cache, the image requirements cache, the disuse holds, the
// reuse keys, and the pool's texture and sampler slots. Counters, and a walk of at most 4096 sampler slots.
void vk_texture_sizes(PerfSizes *s) {
    lock();
    uint64_t imgs = by_key.n, rel = (uint64_t)nreleased, relb = rel_bytes, ret = (uint64_t)nretiring, retb = retiring_bytes,
             smp = samplers.n, reqs = img_reqs.n, hold = holds.n, reuse_keys = reuse.n, tslot = pool_slots, sslot = 0;
    for (int i = 0; i < nsslots; i++) sslot += sslots[i].used != 0;
    unlock();
    perf_size(s, "textures", imgs, 0, PERF_SIZE_NONE);
    perf_size(s, "released textures", rel, 0, relb);
    perf_size(s, "retiring textures", ret, 0, retb);
    perf_size(s, "samplers", smp, 0, PERF_SIZE_NONE);
    perf_size(s, "image requirements", reqs, 0, PERF_SIZE_NONE);
    perf_size(s, "disuse holds", hold, HOLDS_MAX, PERF_SIZE_NONE);
    perf_size(s, "reuse keys", reuse_keys, 0, PERF_SIZE_NONE);
    perf_size(s, "texture pool slots", tslot, VK_POOL_TEX, PERF_SIZE_NONE);
    perf_size(s, "sampler slots", sslot, VK_POOL_SAMP, PERF_SIZE_NONE);
}
