// Motion vectors for every skinned character (NX2NAVI_SKIN_MOTION, skin_motion in nx2navi.ini; game 1.1.1). The
// player's meshes get their own motion through ownership records (motion_owner.c, vk_fg_pose.inc): a draw is the
// player's when a block it binds belongs to a model of the player's actor. People, animals and enemies have no such
// record, so the camera's motion was all they got, and DLSS and FSR 3.1 smeared them as they moved. This file gives
// them the same second draw, with last frame's pose and camera into the motion vector target, without knowing whose
// they are. A draw the player's path takes (pose_owner) never comes here.
//
// A skinned scene draw (skin_pick): a draw that passes the scene filter of mvobj_pick (solid, one instance, into a
// scene-size depth target with the scene context at c3), whose vertex shader reads a matrix palette
// (gfx_shader.c reflect_palette states that rule and what it matched) from one of the uniform banks c3 to c7, and
// whose vertex layout feeds each joint input of that shader an unsigned integer attribute. Weights are not asked
// for: a mesh that binds each vertex to one joint carries none and moves with its skeleton all the same.
//
// Its second draw uses the vertex shader's last-frame variant (translator patch 0015), which computes the position
// again from the blocks bound at bank + 15. The variant's position may read c3 to c7 only, among them the scene
// context c3 and the palette, and no storage buffer or texture: as for the player. Each block such a draw binds is
// kept once a frame, by address (many pieces share a palette, all share the scene context): its bytes go into the
// frame's arena for the next frame, and a stretch of the submission's scratch memory is set aside where last frame's
// bytes of the same object are put once that is known (SkinBlock).
//
// Which draw of last frame a draw continues is decided when the motion vectors are made (skin_resolve, called from
// vk_fg_pose_prepare), not at the draw: the replay's threads reach the draws of one pass in any order, and the
// number that tells two characters of one model apart counts in the order of the command stream (skin_pair.h, which
// lists the cases that leave a draw to the camera). There the frame's draws are first cut down to the scene's own
// view (the camera block and size the motion vectors are made for) and to one draw for each mesh and palette: the game
// draws a mesh into a depth pre-pass and again, against a copy of that depth, into the light target, and the draw into
// the depth image the vectors are tested against is kept, as for the player; the pre-pass draw stands in where the
// other could not be listed (its vertex shader reads storage buffers), and from the next frame on a mesh with a
// light-target draw has its pre-pass draw left out at the draw already. Then:
//   - a key drawn more than once must also have each palette near last frame's (skin_near, SKIN_NEAR_UNITS);
//   - every bank of the paired draws must have the same size;
//   - the draws that share a block must agree on which block of last frame it continues, or all of them are left out.
// What passes has its blocks' scratch filled with last frame's bytes and stays in the list; the rest is taken out of
// it (MoDraw.skin), so those pixels keep the camera's motion.
//
// The guard for parts that turn sharply (motion_guard_degrees) is chosen at the draw, as it picks the fragment shader:
// a draw is checked against last frame's depth (shaders/link_motion.frag) when a bone of its palette turned past the
// threshold against last frame's palette of any draw with its key. For a key drawn once that is the player's rule;
// for more it errs towards the check.
//
// Cost on the replay thread, for a skinned draw: the palette lookup in the shader's reflection, a hash lookup of the
// listed meshes, for each block its position reads a lookup in the uniform snapshot and one in the frame's blocks, a
// binary search in last frame's list, and for the first draw that binds a block a copy of its bytes; then what any
// motion draw costs (mvobj_draw_). Nothing runs while no upscaler is active (mvobj_draw, skin_pick).
#include "skin_pair.h"

#define SKIN_BANKS 5              // c3 to c7, as POSE_BANKS
#define SKIN_SLOTS 4096           // hash slots of a frame's blocks, listed draws and lit meshes
#define SKIN_FILL 2048            // entries each holds at most
#define SKIN_NEAR_UNITS 1.0f      // a palette of the same key more than this from last frame's is another character's

// Why a skinned draw kept the camera's motion: skin_pair.h's, then this file's.
enum { SKIN_NOT_READY = SKIN_PAIR_WHYS,   // its variant, fragment shader or pipeline is still being made
       SKIN_BANKS_OTHER,                  // the position reads a bank outside c3 to c7, or not the palette, or the
                                          // palette is not in a uniform bank
       SKIN_STORAGE,                      // the variant reads a storage buffer, a texture or an image
       SKIN_SNAPSHOT,                     // a block is not in the uniform snapshot
       SKIN_TWIN,                         // another draw of the same mesh and palette carries the vector
       SKIN_VIEW,                         // drawn with another camera block or size than the scene's
       SKIN_SHARED,                       // the draws sharing one of its blocks disagree on last frame's
       SKIN_FAR,                          // skin_near
       SKIN_FULL,                         // a table is full
       SKIN_UNUSED,                       // the frame's list was not recorded (another submission made the vectors)
       SKIN_WHYS };

typedef struct { uint8_t stride, bank; } SkinPick;   // skin_pick: the bytes of a bone matrix (0: not a skinned draw)
                                                     // and the palette's bank, counted from c3

typedef struct {
    uint64_t addr;
    uint32_t size, at;              // at: this frame's bytes in the frame's arena
    VkDescriptorBufferInfo was;     // where last frame's bytes of the same object go; the motion draws bind it
    uint8_t *was_map;
    int32_t last;                   // last frame's block it continues: -1 none named, -2 two draws named different ones
    int32_t sharp_with, sharp;      // skin_sharp's answer for last frame's block sharp_with
} SkinBlock;

typedef struct {
    int32_t block[SKIN_BANKS];      // the frame's block per bank, -1 for a bank the position does not read
    uint8_t palette, stride;        // the palette's bank, the bytes of a matrix
    uint32_t zw, zh;                // the depth target it was drawn into
    VkImage depth;
    uint64_t cam;                   // its scene context block
    uint64_t mesh, twin;            // hashes: the mesh; the mesh and its palette's address
    int32_t draw;                   // its MoDraw
} SkinDraw;

typedef struct {
    uint64_t frame;
    VkImage scene_depth;            // once resolved: the depth image the vectors were tested against
    SkinDraw *d;                    // the listed draws
    SkinEntry *e;                   // while listing one per draw (tag: its index); once resolved the kept ones,
    int n, ne, cap_d, cap_e;        // numbered (skin_number), ne of them
    SkinBlock *b;
    int nb, cap_b;
    uint8_t *bytes;                 // the blocks' bytes
    uint32_t used, size;
    uint16_t block_at[SKIN_SLOTS];  // by address and size: index + 1
    uint64_t seen[SKIN_SLOTS];      // listed draws by mesh, palette, camera block and depth image
    uint64_t lit[SKIN_SLOTS];       // once resolved: the meshes with a kept draw into scene_depth
    int nseen;
} SkinFrame;

static struct {
    SkinFrame f[2];
    int cur;                        // f[cur]: of the list being made; f[cur ^ 1]: the last one resolved
    int resolved;                   // the list being made was resolved (its draws carry their verdicts)
    uint32_t pending;               // its draws not counted yet
    uint64_t seen, own, guarded, why[SKIN_WHYS];
} SK;

static int skin_enabled(void) {
    static int on = -1;
    if (on < 0) on = RT_V111 && cfg_bool("NX2NAVI_SKIN_MOTION");
    return on;
}

static void skin_left(int why) { __atomic_add_fetch(&SK.why[why], 1, __ATOMIC_RELAXED); }

// A hash set of SKIN_SLOTS words (0: empty). skin_has: whether v is in it; skin_put: adds it.
static int skin_has(const uint64_t *set, uint64_t v) {
    v |= 1;
    for (uint32_t i = (uint32_t)(v >> 40) & (SKIN_SLOTS - 1); set[i]; i = (i + 1) & (SKIN_SLOTS - 1))
        if (set[i] == v) return 1;
    return 0;
}
static void skin_put(uint64_t *set, uint64_t v) {
    v |= 1;
    uint32_t i = (uint32_t)(v >> 40) & (SKIN_SLOTS - 1);
    while (set[i] && set[i] != v) i = (i + 1) & (SKIN_SLOTS - 1);
    set[i] = v;
}

// An unsigned integer vertex format: what joint indices are stored as.
static int skin_joint_format(uint32_t fmt) {
    switch (fmt) {
    case NVN_FORMAT_R8UI: case NVN_FORMAT_RG8UI: case NVN_FORMAT_RGB8UI: case NVN_FORMAT_RGBA8UI:
    case NVN_FORMAT_R16UI: case NVN_FORMAT_RG16UI: case NVN_FORMAT_RGB16UI: case NVN_FORMAT_RGBA16UI:
    case NVN_FORMAT_R32UI: case NVN_FORMAT_RG32UI: case NVN_FORMAT_RGB32UI: case NVN_FORMAT_RGBA32UI: return 1;
    default: return 0;
    }
}

// Whether the draw being replayed is a skinned scene draw (the rule at the top; the caller has applied the scene
// filter). Reads the draw's own state only: the chunks of a parallel group ask at once.
static int skin_pick(VkExec *ex, SkinPick *out) {
    out->stride = 0;
    if (!skin_enabled() || !(vk_dlaa_active() || vk_dlaa_fg_only())) return 0;   // the upscaler or frame generation alone
    const DrawCtx *cx = ex->dc;
    int slot, storage;
    uint32_t joints;
    int stride = gfx_shader_palette(cx->B.st.vs, &slot, &storage, &joints);
    if (!stride || joints >> 16) return 0;
    for (int l = 0; l < 16; l++) {
        va_st a;
        memcpy(&a, &cx->B.st.vattr[l], sizeof a);
        if ((joints >> l & 1) && !skin_joint_format(a.fmt)) return 0;
    }
    __atomic_add_fetch(&SK.seen, 1, __ATOMIC_RELAXED);
    if (storage || slot < 3 || slot >= 3 + SKIN_BANKS) {
        skin_left(SKIN_BANKS_OTHER);
        return 0;
    }
    out->stride = (uint8_t)stride, out->bank = (uint8_t)(slot - 3);
    return 1;
}

// mvobj_pick left a draw skin_pick took: counted, 0.
static int skin_pick_left(const SkinPick *s, int why) {
    if (s->stride) skin_left(why);
    return 0;
}

// Starts the frame's list (mo_begin): the draws of the one before it that nothing counted were never recorded.
static void skin_begin(uint64_t frame) {
    SK.why[SKIN_UNUSED] += SK.pending;
    SK.pending = 0, SK.resolved = 0;
    if (!skin_enabled()) return;
    SkinFrame *f = &SK.f[SK.cur];
    f->frame = frame, f->scene_depth = VK_NULL_HANDLE;
    f->n = f->ne = f->nb = f->nseen = 0, f->used = 0;
    memset(f->block_at, 0, sizeof f->block_at);
    memset(f->seen, 0, sizeof f->seen);
}

// The frame's block for a uniform binding, made at its first draw: -1 when it is not in the snapshot, -2 when the
// table is full.
static int skin_block(VkExec *ex, SkinFrame *f, const p_bind_buf *u) {
    // asked of the snapshot for every draw: a block the submission itself writes between two draws is dropped from
    // it there, and the bytes kept for the first draw are not the second one's
    VkDescriptorBufferInfo snap;
    if (!u->addr || !u->size || u->size > LIM.ubo_max || u->size > POSE_BYTES ||
        !vk_usnap_find(u->addr, u->size, LIM.ubo_align, &snap))
        return -1;
    uint32_t h = (uint32_t)(skin_mix(u->addr, u->size) >> 40) & (SKIN_SLOTS - 1);
    for (; f->block_at[h]; h = (h + 1) & (SKIN_SLOTS - 1)) {
        const SkinBlock *b = &f->b[f->block_at[h] - 1];
        if (b->addr == u->addr && b->size == u->size) return f->block_at[h] - 1;
    }
    if (f->nb == SKIN_FILL) return -2;
    uint32_t size = (uint32_t)u->size;
    if (f->used + size > f->size) {
        uint32_t want = f->size ? f->size * 2 : 1u << 20;
        while (want < f->used + size) want *= 2;
        uint8_t *bytes = realloc(f->bytes, want);
        if (!bytes) return -2;
        f->bytes = bytes, f->size = want;
    }
    f->b = mo_grow(f->b, &f->cap_b, f->nb + 1, sizeof *f->b);
    SkinBlock *b = &f->b[f->nb];
    *b = (SkinBlock){.addr = u->addr, .size = size, .at = f->used, .last = -1, .sharp_with = -1};
    memcpy(f->bytes + b->at, vk_usnap_map() + snap.offset, size);
    f->used += size;
    b->was.range = size;
    b->was_map = scratch(ex, size, &b->was.buffer, &b->was.offset);
    f->block_at[h] = (uint16_t)(f->nb + 1);
    return f->nb++;
}

// Whether a bone of the palette block c of this frame turned sharply against the palette block l of last frame
// (pose_matrix_sharp); kept for the pieces that share the palette.
static int skin_sharp(SkinFrame *f, const SkinFrame *last, int c, int l, uint32_t stride) {
    SkinBlock *b = &f->b[c];
    const SkinBlock *o = &last->b[l];
    if (b->sharp_with == l) return b->sharp;
    b->sharp_with = l, b->sharp = 0;
    if (b->size != o->size) return 0;
    const uint8_t *now = f->bytes + b->at, *then = last->bytes + o->at;
    for (uint32_t at = 0; at + stride <= b->size && !b->sharp; at += stride)
        if (memcmp(now + at, then + at, 48)) b->sharp = pose_matrix_sharp(now + at, then + at);
    return b->sharp;
}

// A skinned draw mvobj_pick took, before it goes onto the frame's list (the caller holds the motion-list lock): its
// blocks, key and place in the stream into *sd and *se, and into *guard whether it is checked against last frame's
// depth. 0: left to the camera, counted. vs: the last-frame variant, banks: the banks its position reads.
static int skin_try(VkExec *ex, const p_draw_idx *d, uint64_t frame, const GfxShader *vs, uint32_t banks,
                    const SkinPick *pick, SkinDraw *sd, SkinEntry *se, uint64_t *listed, int *guard) {
    const DrawCtx *cx = ex->dc;
    SkinFrame *f = &SK.f[SK.cur];
    const SkinFrame *last = &SK.f[SK.cur ^ 1];
    int consecutive = last->frame + 1 == frame;
    const GfxShaderRes *res;
    *guard = 0;
    // a draw after the frame's vectors were made (skin_resolve made that list last frame's, and f is not begun)
    if (SK.resolved || f->frame != frame) return skin_pick_left(pick, SKIN_UNUSED);
    if (!(banks >> 3 & 1) || !(banks >> (pick->bank + 3) & 1) || (banks & ~0xf8u)) return skin_pick_left(pick, SKIN_BANKS_OTHER);
    if (gfx_shader_resources(vs, GFX_RES_SSBO, &res) || gfx_shader_resources(vs, GFX_RES_TEX, &res) ||
        gfx_shader_resources(vs, GFX_RES_IMG, &res))
        return skin_pick_left(pick, SKIN_STORAGE);
    // the mesh: the vertex buffers the shader's inputs read (a binding no input reads keeps an earlier draw's) and
    // the indices
    uint32_t inputs = gfx_shader_inputs(cx->B.st.vs, NULL), streams = 0;
    for (int i = 0; i < 16; i++) {
        va_st a;
        memcpy(&a, &cx->B.st.vattr[i], sizeof a);
        if ((inputs >> i & 1) && a.fmt && a.stream < 16) streams |= 1u << a.stream;
    }
    uint64_t mesh = skin_mix(skin_mix(d->ibuf, (uint64_t)(uint32_t)d->count << 32 | (uint32_t)d->base_vertex), (uint64_t)d->itype);
    for (int s = 0; s < 16; s++)
        if (streams >> s & 1) mesh = skin_mix(skin_mix(mesh, ex->vbuf[s].address), ex->vbuf[s].size ^ (uint64_t)s << 56);
    const p_bind_buf *pal = &ex->ubo[NVN_STAGE_VS][pick->bank];
    const VkExecTarget *z = &ex->depth;
    uint64_t twin = skin_mix(mesh, pal->addr), cam = ex->ubo[NVN_STAGE_VS][0].addr;
    uint64_t here = skin_mix(skin_mix(twin, cam), (uint64_t)(uintptr_t)z->image);
    // the same mesh with the same palette in the same view: a second pass over it, or the draw submitted twice
    if (skin_has(f->seen, here)) return skin_pick_left(pick, SKIN_TWIN);
    // the pre-pass draw (it writes depth, into another image than the vectors are tested against) of a mesh whose
    // light-target draw carried the vector last frame
    if (consecutive && cx->B.st.ds.dw && last->scene_depth && z->image != last->scene_depth && skin_has(last->lit, mesh))
        return skin_pick_left(pick, SKIN_TWIN);
    if (f->nseen == SKIN_FILL) return skin_pick_left(pick, SKIN_FULL);
    for (int i = 0; i < SKIN_BANKS; i++) {
        sd->block[i] = -1;
        if (!(banks >> (i + 3) & 1)) continue;
        sd->block[i] = skin_block(ex, f, &ex->ubo[NVN_STAGE_VS][i]);
        if (sd->block[i] < 0) return skin_pick_left(pick, sd->block[i] == -1 ? SKIN_SNAPSHOT : SKIN_FULL);
    }
    sd->palette = pick->bank, sd->stride = pick->stride;
    sd->zw = z->width, sd->zh = z->height, sd->depth = z->image;
    sd->cam = cam, sd->mesh = mesh, sd->twin = twin;
    *se = (SkinEntry){.key = skin_mix(skin_mix(mesh, (uint64_t)(uintptr_t)cx->B.st.vs), (uint64_t)(uintptr_t)cx->B.st.fs),
                      .order = vk_exec_stream_pos() << 24 | (uint64_t)f->n, .palette = pal->addr,
                      .bytes = (uint32_t)pal->size};
    *listed = here;
    int at = consecutive && !pose_raw() ? skin_find(last->e, last->ne, se->key) : -1;
    for (uint32_t k = 0; at >= 0 && k < last->e[at].of && !*guard; k++) {
        const SkinDraw *ld = &last->d[last->e[at + (int)k].tag];
        if (ld->palette == sd->palette) *guard = skin_sharp(f, last, sd->block[sd->palette], ld->block[ld->palette], sd->stride);
    }
    return 1;
}

// The draw skin_try prepared is on the frame's list as MQ.d[draw].
static void skin_commit(const SkinDraw *sd, const SkinEntry *se, uint64_t listed, int draw) {
    SkinFrame *f = &SK.f[SK.cur];
    f->d = mo_grow(f->d, &f->cap_d, f->n + 1, sizeof *f->d);
    f->e = mo_grow(f->e, &f->cap_e, f->n + 1, sizeof *f->e);
    f->d[f->n] = *sd, f->d[f->n].draw = draw;
    f->e[f->n] = *se, f->e[f->n].tag = (uint32_t)f->n;
    f->n++;
    skin_put(f->seen, listed), f->nseen++;
    SK.pending++;
}

// Takes a listed draw out of the frame's list, counted.
static void skin_refuse(const SkinDraw *sd, int why) {
    MQ.d[sd->draw].skin = -1;
    SK.why[why]++, SK.pending--;
}

static const SkinFrame *skin_sort_frame;   // skin_twin_cmp's frame: the entries' draws
// By mesh and palette, the draw into the scene depth first, then by place in the stream.
static int skin_twin_cmp(const void *a, const void *b) {
    const SkinEntry *x = a, *y = b;
    const SkinDraw *dx = &skin_sort_frame->d[x->tag], *dy = &skin_sort_frame->d[y->tag];
    if (dx->twin != dy->twin) return dx->twin < dy->twin ? -1 : 1;
    int ox = dx->depth != skin_sort_frame->scene_depth, oy = dy->depth != skin_sort_frame->scene_depth;
    if (ox != oy) return ox - oy;
    return x->order < y->order ? -1 : x->order > y->order;
}

// The frame's list is complete and its motion vectors are about to be made, for the scene camera block cam, tested
// against the depth image depth of w x h (vk_fg_pose_prepare, on the thread that records them; ex: its submission).
// Decides for each listed skinned draw whether it continues a draw of last frame (the steps at the top), fills the
// blocks of those that do with last frame's bytes, takes the others out of the list, and makes this frame's list the
// one the next frame is paired with.
static void skin_resolve(VkExec *ex, uint64_t frame, uint64_t cam, VkImage depth, uint32_t w, uint32_t h) {
    SkinFrame *f = &SK.f[SK.cur];
    const SkinFrame *last = &SK.f[SK.cur ^ 1];
    // only the list of this submission: its blocks' scratch memory lives as long as the submission
    if (!skin_enabled() || SK.resolved || MQ.frame != frame || MQ.fence != ex->fence || f->frame != frame) return;
    SK.resolved = 1;
    f->scene_depth = depth;
    int ne = 0;
    for (int i = 0; i < f->n; i++) {
        const SkinDraw *sd = &f->d[i];
        if (sd->cam != cam || sd->zw != w || sd->zh != h) skin_refuse(sd, SKIN_VIEW);
        else f->e[ne++] = f->e[i];
    }
    skin_sort_frame = f;
    qsort(f->e, (size_t)ne, sizeof *f->e, skin_twin_cmp);
    int kept = 0;
    for (int i = 0; i < ne; i++) {
        if (kept && f->d[f->e[kept - 1].tag].twin == f->d[f->e[i].tag].twin) skin_refuse(&f->d[f->e[i].tag], SKIN_TWIN);
        else f->e[kept++] = f->e[i];
    }
    f->ne = ne = kept;
    skin_number(f->e, ne);
    skin_pair(f->e, ne, last->e, last->ne, last->frame + 1 == frame);
    for (int i = 0; i < ne; i++) {
        SkinEntry *e = &f->e[i];
        const SkinDraw *sd = &f->d[e->tag];
        if (e->why != SKIN_PAIRED) continue;
        const SkinDraw *ld = &last->d[last->e[e->prev].tag];
        if (ld->palette != sd->palette || ld->stride != sd->stride) e->why = SKIN_SIZE;
        for (int k = 0; k < SKIN_BANKS && e->why == SKIN_PAIRED; k++)
            if ((sd->block[k] < 0) != (ld->block[k] < 0) ||
                (sd->block[k] >= 0 && f->b[sd->block[k]].size != last->b[ld->block[k]].size))
                e->why = SKIN_SIZE;
        if (e->why == SKIN_PAIRED && e->of > 1 &&
            !skin_near(f->bytes + f->b[sd->block[sd->palette]].at, last->bytes + last->b[ld->block[ld->palette]].at,
                       e->bytes, sd->stride, SKIN_NEAR_UNITS))
            e->why = SKIN_FAR;
        for (int k = 0; k < SKIN_BANKS && e->why == SKIN_PAIRED; k++) {
            if (sd->block[k] < 0) continue;
            SkinBlock *b = &f->b[sd->block[k]];
            b->last = b->last == -1 || b->last == ld->block[k] ? ld->block[k] : -2;
        }
    }
    memset(f->lit, 0, sizeof f->lit);
    for (int i = 0; i < ne; i++) {
        SkinEntry *e = &f->e[i];
        const SkinDraw *sd = &f->d[e->tag];
        if (sd->depth == depth) skin_put(f->lit, sd->mesh);
        for (int k = 0; k < SKIN_BANKS && e->why == SKIN_PAIRED; k++)
            if (sd->block[k] >= 0 && f->b[sd->block[k]].last == -2) e->why = SKIN_SHARED;
        if (e->why != SKIN_PAIRED) skin_refuse(sd, e->why);
    }
    for (int i = 0; i < f->nb; i++) {
        const SkinBlock *b = &f->b[i];
        if (b->last >= 0) memcpy(b->was_map, last->bytes + last->b[b->last].at, b->size);
    }
    SK.cur ^= 1;
}

// A skinned draw vk_mvobj_execute records (guarded: checked against last frame's depth).
static void skin_recorded(int guarded) {
    SK.own++, SK.guarded += guarded != 0, SK.pending--;
}

// The skinned draws' part of the motion status line (pose_status): since the start, every draw skin_pick took, the
// ones drawn with their own motion, and the others by why they kept the camera's.
static int skin_status(char *out, size_t cap) {
    out[0] = 0;
    if (!skin_enabled()) return 0;
    const uint64_t *y = SK.why;
    return snprintf(out, cap, "; skinned draws seen %llu, drawn with their own motion %llu (checked against last frame's "
                    "depth %llu), left to the camera: carried by another draw of the mesh %llu, not drawn last frame %llu, "
                    "drawn another number of times %llu, palette of another size %llu, palette at another address %llu, "
                    "too far from last frame's %llu, shared block disagrees %llu, another view %llu, shader or pipeline "
                    "not ready %llu, position reads other blocks %llu, storage in the vertex shader %llu, block not in the "
                    "snapshot %llu, tables full %llu, list not recorded %llu",
                    (unsigned long long)__atomic_load_n(&SK.seen, __ATOMIC_RELAXED), (unsigned long long)SK.own,
                    (unsigned long long)SK.guarded, (unsigned long long)y[SKIN_TWIN], (unsigned long long)y[SKIN_NEW],
                    (unsigned long long)y[SKIN_COUNT], (unsigned long long)y[SKIN_SIZE], (unsigned long long)y[SKIN_ADDRESS],
                    (unsigned long long)y[SKIN_FAR], (unsigned long long)y[SKIN_SHARED], (unsigned long long)y[SKIN_VIEW],
                    (unsigned long long)y[SKIN_NOT_READY], (unsigned long long)y[SKIN_BANKS_OTHER],
                    (unsigned long long)y[SKIN_STORAGE], (unsigned long long)y[SKIN_SNAPSHOT], (unsigned long long)y[SKIN_FULL],
                    (unsigned long long)y[SKIN_UNUSED]);
}

static void skin_shutdown(void) {
    for (int i = 0; i < 2; i++) free(SK.f[i].d), free(SK.f[i].e), free(SK.f[i].b), free(SK.f[i].bytes);
    memset(&SK, 0, sizeof SK);
}
