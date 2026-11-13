// Final submitted pose inputs for owned player models. Replay workers change history under
// the motion-list lock; the submitting thread selects and consumes it after the group finishes.
// Model and shape identities come from producer-side ownership records, never from a mesh fingerprint.
#include "motion_owner.h"
#define POSE_ROWS 256
#define POSE_BYTES (128u * 1024u)
#define POSE_BANKS 5  // c3 through c7
#define POSE_VIEWS 8
typedef struct {
    uint64_t frame, cam, indices;
    VkImage depth;
    const GfxShader *shader;
    NVNbufferRange vertex[16];
    uint64_t bindings[POSE_BANKS];
    uint32_t size[POSE_BANKS], capacity;
    unsigned char *bytes;
    uint32_t width, height;
    int seen, selected, ambiguous;
    int visibility_guard, guard_missing_depth;
} ActorPose;
typedef struct {
    ActorPose view[POSE_VIEWS];
    ActorPose *scene;
} PoseFrame;
typedef struct {
    uint32_t count, width, base, actor;
    uint64_t indices, last_frame;
    MotionOwner owner;
    PoseFrame history[2];
} PoseRange;
static PoseRange pose_ranges[POSE_ROWS];
static int pose_count;
static unsigned pose_seen[3], pose_written[3], pose_missing, pose_full, pose_unowned;

static int pose_enabled(void) { return motion_owner_enabled(); }
static int pose_raw(void) {
    static int raw = -1;
    if (raw < 0) raw = rt_knob("SELECTIVE_MOTION_RAW", "0");
    return raw;
}

static int pose_same_owner(const MotionOwner *a, const MotionOwner *b) {
    return a->player == b->player && a->actor == b->actor && a->link == b->link &&
        a->generation == b->generation && a->model == b->model && a->part == b->part && a->lifetime == b->lifetime &&
        a->flags == b->flags;
}

static void pose_clear(PoseRange *r) {
    for (int j = 0; j < 2; ++j)
        for (int k = 0; k < POSE_VIEWS; ++k) free(r->history[j].view[k].bytes);
    memset(r, 0, sizeof *r);
}

static int pose_owner(VkExec *ex, MotionOwner *out) {
    MotionOwner owner = {0};
    // Only vertex resources consumed by this shader can establish identity. Stale unused bindings cannot.
    const GfxShaderRes *res;
    int nres = gfx_shader_resources(ex->dc->B.st.vs, GFX_RES_UBO, &res);
    for (int j = 0; j < nres; ++j) {
        int bank = res[j].slot;
        if (bank < 3 || bank > 7) continue;
        const p_bind_buf *b = &ex->ubo[NVN_STAGE_VS][bank - 3];
        MotionOwner m;
        if (!vk_usnap_motion_owner(b->addr, b->size, &m)) continue;
        if (owner.model && (owner.model != m.model || owner.lifetime != m.lifetime ||
                           (owner.part && m.part && owner.part != m.part))) return 0;
        if (!owner.model || m.part) owner = m;
    }
    // The per-shape record is required even for skinned meshes; one palette can serve several pieces.
    if (!owner.model || !owner.part) return 0;
    *out = owner;
    return 1;
}

// The caller holds the motion-list lock while parallel replay workers record draws.
static PoseRange *pose_match(VkExec *ex, const p_draw_idx *d, const MotionOwner *identity) {
    MotionOwner owner = *identity;
    uint64_t frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    uint32_t width = d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? 2u : 4u;
    PoseRange *free_row = NULL;
    for (int i = 0; i < pose_count; ++i) {
        PoseRange *r = &pose_ranges[i];
        if (pose_same_owner(&r->owner, &owner) && r->indices == d->ibuf &&
            r->count == (uint32_t)d->count && r->width == width && r->base == (uint32_t)d->base_vertex) {
            r->last_frame = frame;
            ++pose_seen[r->actor];
            return r;
        }
        if (!free_row && r->last_frame + 2 < frame) free_row = r;
    }
    if (pose_count < POSE_ROWS) free_row = &pose_ranges[pose_count++];
    if (!free_row) { ++pose_full; return NULL; }
    pose_clear(free_row);
    free_row->owner = owner;
    free_row->indices = d->ibuf;
    free_row->count = (uint32_t)d->count;
    free_row->width = width;
    free_row->base = (uint32_t)d->base_vertex;
    free_row->actor = owner.kind ? 2 : 1;
    free_row->last_frame = frame;
    ++pose_seen[free_row->actor];
    return free_row;
}

static ActorPose *pose_previous(PoseRange *r, VkExec *ex, const p_draw_idx *d, uint64_t frame, uint32_t banks,
                                ActorPose **candidate) {
    PoseFrame *now = &r->history[frame & 1];
    ActorPose *prev = r->history[(frame & 1) ^ 1].scene;
    uint64_t cam = ex->ubo[NVN_STAGE_VS][0].addr;
    // The same mesh is drawn into several depth targets. Only the resolve knows which view supplies FG.
    ActorPose *cur = NULL;
    for (int i = 0; i < POSE_VIEWS; ++i) {
        ActorPose *p = &now->view[i];
        if (!p->seen || p->frame != frame || (p->cam == cam && p->depth == ex->depth.image)) {
            cur = p;
            break;
        }
    }
    *candidate = cur;
    if (!cur) return NULL;
    int repeated = cur->seen && cur->frame == frame;
    if (repeated && cur->ambiguous) return NULL;
    if (!repeated) {
        cur->seen = 1;
        cur->frame = frame;
        cur->selected = cur->ambiguous = 0;
        cur->visibility_guard = cur->guard_missing_depth = 0;
        cur->cam = cam;
        cur->depth = ex->depth.image;
        cur->width = ex->depth.width;
        cur->height = ex->depth.height;
    }

    // Unused bindings retain the previous draw's values. Only the shader's vertex inputs identify this mesh.
    NVNbufferRange vertex[16] = {0};
    uint32_t inputs = gfx_shader_inputs(ex->dc->B.st.vs, NULL);
    for (int i = 0; i < 16; ++i) {
        va_st a;
        memcpy(&a, &ex->dc->B.st.vattr[i], sizeof a);
        if ((inputs >> i & 1) && a.fmt && a.stream < 16) vertex[a.stream] = ex->vbuf[a.stream];
    }
    uint32_t size[POSE_BANKS] = {0}, n = 0;
    uint64_t bindings[POSE_BANKS] = {0};
    const uint8_t *part[POSE_BANKS] = {0};
    for (int i = 0; i < POSE_BANKS; ++i) {
        if (!(banks & (1u << (i + 3)))) continue;
        const p_bind_buf *u = &ex->ubo[NVN_STAGE_VS][i];
        VkDescriptorBufferInfo snap;
        if (!u->size || u->size > LIM.ubo_max || u->size > POSE_BYTES - n ||
            !vk_usnap_find(u->addr, u->size, LIM.ubo_align, &snap)) {
            cur->ambiguous = 1;
            ++pose_missing;
            return NULL;
        }
        part[i] = vk_usnap_map() + snap.offset;
        size[i] = (uint32_t)u->size;
        bindings[i] = u->addr;
        n += size[i];
    }
    if (repeated) {
        // The same instance can be submitted again in the same pass. A second instance of this mesh
        // can use another palette, or rewrite a palette at the same address between submissions.
        cur->ambiguous = cur->depth != ex->depth.image || cur->indices != d->ibuf ||
                         memcmp(vertex, cur->vertex, sizeof vertex) || memcmp(size, cur->size, sizeof size) ||
                         memcmp(bindings, cur->bindings, sizeof bindings);
        uint32_t offset = 0;
        for (int i = 0; !cur->ambiguous && i < POSE_BANKS; ++i) {
            if (size[i] && memcmp(cur->bytes + offset, part[i], size[i])) cur->ambiguous = 1;
            offset += size[i];
        }
        return NULL;
    }
    cur->shader = ex->dc->B.st.vs;
    cur->indices = d->ibuf;
    memcpy(cur->vertex, vertex, sizeof vertex);
    memcpy(cur->bindings, bindings, sizeof bindings);
    memcpy(cur->size, size, sizeof size);
    if (n > cur->capacity) {
        uint8_t *larger = realloc(cur->bytes, n);
        if (!larger) { cur->ambiguous = 1; return NULL; }
        cur->bytes = larger;
        cur->capacity = n;
    }
    uint8_t *data = cur->bytes;
    for (int i = 0; i < POSE_BANKS; ++i) {
        if (part[i]) memcpy(data, part[i], cur->size[i]);
        data += cur->size[i];
    }
    return prev && frame && prev->frame == frame - 1 && prev->selected && !prev->ambiguous &&
           prev->width == cur->width && prev->height == cur->height &&
           prev->shader == cur->shader && prev->indices == cur->indices && cur->vertex[0].address &&
           !memcmp(prev->vertex, cur->vertex, sizeof cur->vertex) &&
           !memcmp(prev->size, cur->size, sizeof cur->size) ? prev : NULL;
}

// c6 holds a part's submitted transform. Translation is deliberately excluded: a fast jump
// does not itself make a vector unreliable. Only a positively identified sharp rotation opts in.
static float pose_rotation_trace_limit(void) {
    static int degrees;
    static float limit;
    if (!degrees) {
        const char *setting = cfg_get("NX2NAVI_MOTION_GUARD_DEGREES");   // motion_guard_degrees in nx2navi.ini too
        degrees = setting && !strcmp(setting, "35") ? 35 : setting && !strcmp(setting, "45") ? 45 : 60;
        limit = degrees == 60 ? 2.0f : degrees == 45 ? 2.41421356f : 2.63830409f;
        rt_log("motion policy: rotation guard threshold %d degrees per rendered frame", degrees);
    }
    return limit;
}
// Two 3x4 matrices (rows of four floats) of one part or bone, this frame's and last frame's: 1 when both are plain
// rotations and the turn between them is past the guard threshold. vk_skin_motion.inc asks it for each bone of a palette.
static int pose_matrix_sharp(const uint8_t *now, const uint8_t *prev) {
    float m[2][12];
    const uint8_t *p[2] = {now, prev};
    for (int k = 0; k < 2; ++k) {
        memcpy(m[k], p[k], sizeof m[k]);
        for (int a = 0; a < 3; ++a) {
            for (int b = a; b < 3; ++b) {
                float dot = 0.0f;
                for (int j = 0; j < 3; ++j) dot += m[k][4 * a + j] * m[k][4 * b + j];
                float wanted = a == b ? 1.0f : 0.0f;
                if (!(dot >= wanted - 0.005f && dot <= wanted + 0.005f)) return 0;
            }
        }
    }
    float trace = 0.0f;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) trace += m[0][4 * i + j] * m[1][4 * i + j];
    // trace(R1 * transpose(R0)) = 1 + 2*cos(angle). The selected threshold controls
    // when this part needs visibility filtering; equipment retains its separate raw policy.
    return trace < pose_rotation_trace_limit();
}
static int pose_sharp_rotation(const ActorPose *now, const ActorPose *prev) {
    if (!now || !prev || prev->frame + 1 != now->frame || now->size[3] < 48 || prev->size[3] < 48) return 0;
    return pose_matrix_sharp(now->bytes + now->size[0] + now->size[1] + now->size[2],
                             prev->bytes + prev->size[0] + prev->size[1] + prev->size[2]);
}

static int pose_guard_needed(const PoseRange *range, const ActorPose *now, const ActorPose *previous) {
    if (pose_raw() || (range->owner.flags & MOTION_OWNER_RIGID_EQUIPMENT)) return 0;
    return pose_sharp_rotation(now, previous);
}

static unsigned pose_guarded_parts, pose_raw_parts, pose_rigid_parts;
static void pose_select(uint64_t frame, uint64_t camera, VkImage depth) {
    unsigned guarded = 0, raw = 0, rigid = 0, missing_depth = 0;
    for (int i = 0; i < pose_count; ++i) {
        PoseFrame *h = &pose_ranges[i].history[frame & 1];
        h->scene = NULL;
        for (int j = 0; j < POSE_VIEWS; ++j) {
            ActorPose *p = &h->view[j];
            p->selected = p->seen && p->frame == frame && !p->ambiguous && p->cam == camera && p->depth == depth;
            if (p->selected) {
                h->scene = p;
                guarded += p->visibility_guard;
                raw += !p->visibility_guard;
                rigid += !!(pose_ranges[i].owner.flags & MOTION_OWNER_RIGID_EQUIPMENT);
                missing_depth += p->guard_missing_depth;
            }
        }
    }
    // Selection preserves each part independently. No six-frame hold and no queue-wide drop:
    // a risky body triangle cannot suppress the shield, sword, or another well-behaved part.
    pose_guarded_parts += guarded;
    pose_raw_parts += raw;
    pose_rigid_parts += rigid;
    static int trace = -1;
    if (trace < 0) trace = rt_knob("MOTION_POLICY_TRACE", "0");
    if (trace && (guarded || raw))
        rt_log("motion policy: world %.3f frame %llu guarded %u raw %u rigid %u depth-unavailable %u",
               frame_step_world_us() / 1e6, (unsigned long long)frame, guarded, raw, rigid, missing_depth);
}

// The motion status line, every 120 frames with motion vectors: the player's parts (ownership records), then the
// other skinned draws (vk_skin_motion.inc skin_status).
static int skin_status(char *out, size_t cap);
static void pose_status(uint64_t frame) {
    if (frame % 120) return;
    char skinned[1200];
    skin_status(skinned, sizeof skinned);
    rt_log("selective motion: body %u/%u, attached equipment %u/%u matched/written; missing blocks %u, "
           "unowned draws %u, history capacity misses %u; selected parts guarded %u, raw %u, rigid equipment %u%s",
           pose_seen[1], pose_written[1], pose_seen[2], pose_written[2], pose_missing, pose_unowned, pose_full,
           pose_guarded_parts, pose_raw_parts, pose_rigid_parts, skinned);
}

static void pose_shutdown(void) {
    for (int i = 0; i < pose_count; ++i) pose_clear(&pose_ranges[i]);
    pose_count = 0;
    pose_guarded_parts = pose_raw_parts = pose_rigid_parts = 0;
}
