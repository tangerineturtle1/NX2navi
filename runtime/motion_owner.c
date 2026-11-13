#include "motion_owner.h"
#include "host.h"
#include "layout.h"
#include "nvn_api.h"
#include "platform/platform.h"
#include <string.h>
#include <limits.h>
#include <stdlib.h>

// The supported game build exposes a player equipment component with generation-checked actor links.
// Selection follows those links and the equipment's model-binding parent; proximity is never an identity.
#define OWNER_MODELS 128
#define OWNER_ACTORS 22
typedef struct {
    uint64_t actor, link;
    uint32_t generation, kind;
} OwnerActor;
static struct {
    MotionOwner models[OWNER_MODELS];
    unsigned count;
    uint64_t frame, producer_frame, serial;
    int valid;
    MotionOwnerBinding bindings[MOTION_OWNER_BINDINGS];
    MotionOwnerBinding retired[MOTION_OWNER_BINDINGS];
    unsigned nbindings, nretired;
    uint64_t captures, owned_queries, unknown_queries, conflicts, capacity_misses;
    unsigned model_bind_failures;
} O;
static PlatMutex owner_lock = PLAT_MUTEX_INIT;
static PlatOnce owner_once = PLAT_ONCE_INIT;
static int owner_on;

static void owner_init(void *unused) {
    (void)unused;
    // NX2NAVI_SELECTIVE_MOTION (selective_motion; default 1, 0 off: the estimate from colour and depth). On, every guest
    // submission walks the player's actor, equipment and models in guest memory (motion_owner_capture), each scene
    // draw is looked up in that list (vk_fg_pose.inc pose_owner), and the player's meshes are drawn a second time
    // into the motion vectors DLSS, FSR 3.1 and frame generation read.
    owner_on = RT_V111 && rt_knob("SELECTIVE_MOTION", "1");
}
int motion_owner_enabled(void) {
    plat_once(&owner_once, owner_init, NULL);
    return owner_on;
}
static int read_at(uint64_t a, void *out, size_t n) {
    if (!a || a > UINT64_MAX - n) return 0;
    mem_kernel_access(a, n);
    if (!mem_read_hold(a, n)) return 0;
    memcpy(out, (const void *)(uintptr_t)a, n);
    mem_read_release();
    return 1;
}
static uint64_t ptr(uint64_t a) {
    uint64_t p = 0;
    return read_at(a, &p, sizeof p) ? p : 0;
}
static uint64_t field(uint64_t object, uint32_t offset) { return object ? ptr(object + offset) : 0; }
static uint64_t component(uint64_t actor, uint32_t offset) {
    return field(field(actor, 0x270), offset);
}
static uint64_t player_actor(void) {
    uint64_t p = field(ptr(MOD_MAIN_BASE + 0x4e371d0), 0x480);
    char name[7];
    return p && read_at(field(p, 0x20), name, sizeof name) && !memcmp(name, "Player", 7) ? p : 0;
}
static int actor_link(uint64_t link, OwnerActor *out) {
    struct { uint64_t table; uint32_t generation, padding; } a, b;
    uint32_t generation, state;
    if (!link || !read_at(link + 8, &a, sizeof a) || !a.table || a.generation == UINT32_MAX ||
        !read_at(a.table + 0x48, &generation, 4) || generation != a.generation) return 0;
    uint64_t actor = field(a.table, 0x40);
    if (!actor || !read_at(actor + 0x2c, &state, 4) || (uint32_t)(state - 7) < 5) return 0;
    // Reject a slot replaced during the read. This is not a replacement for the game's actor locking:
    // unknown layouts and any observed disagreement fail closed, and runtime lifetime checks remain required.
    if (!read_at(link + 8, &b, sizeof b) || a.table != b.table || a.generation != b.generation ||
        !read_at(a.table + 0x48, &generation, 4) || generation != a.generation || field(a.table, 0x40) != actor) return 0;
    *out = (OwnerActor){actor, a.table, generation, 0};
    return 1;
}
static int parent_is_player(uint64_t actor, uint64_t player) {
    uint64_t seen[4] = {actor};
    for (unsigned depth = 0; depth < 4; ++depth) {
        uint64_t bind = component(actor, 0x18);
        OwnerActor parent;
        if (!bind || !actor_link(bind + 0x70, &parent)) return 0;
        if (parent.actor == player) return 1;
        for (unsigned i = 0; i <= depth; ++i) if (seen[i] == parent.actor) return 0;
        if (depth + 1 < 4) seen[depth + 1] = parent.actor;
        actor = parent.actor;
    }
    return 0;
}
static uint32_t equipment_flags(uint64_t actor) {
    // This classification is reached only for a generation-checked actor attached to the player.
    // A matching resource elsewhere in the world therefore cannot acquire this policy.
    // Head armor is deliberately excluded: its cap can deform independently of the attachment.
    char name[64];
    if (!read_at(field(actor, 0x20), name, sizeof name)) return 0;
    name[sizeof name - 1] = 0;
    return !strncmp(name, "Weapon_Sword_", 13) || !strncmp(name, "Weapon_Shield_", 14)
               ? MOTION_OWNER_RIGID_EQUIPMENT : 0;
}
static int collect_models(uint64_t player, OwnerActor actor) {
    uint64_t model_component = component(actor.actor, 8);
    if (!model_component) return 1;
    // Only decode the implementation whose getter directly returns its model collection.
    if (field(ptr(model_component), 0xf8) != MOD_MAIN_BASE + 0x9d7160) return 1;
    uint64_t collection = field(model_component, 0x18), list = field(collection, 0x28);
    int32_t count;
    if (!collection || !read_at(collection + 0x20, &count, 4) || count < 0 || count > 16 || (count && !list)) return 0;
    for (int i = 0; i < count; ++i) {
        uint64_t model = ptr(ptr(list + (uint64_t)i * 8));
        if (!model) continue;
        // Match the model implementation used by the traced skeleton and shape binding path.
        if (field(ptr(model), 0x40) != MOD_MAIN_BASE + 0x735430) continue;
        if (O.count == OWNER_MODELS) return 0;
        O.models[O.count++] = (MotionOwner){.player = player, .actor = actor.actor, .link = actor.link,
            .model = model, .generation = actor.generation, .kind = actor.kind,
            .flags = actor.kind ? equipment_flags(actor.actor) : 0};
    }
    return field(model_component, 0x18) == collection && field(collection, 0x28) == list;
}
static void refresh(uint64_t frame, int force) {
    if (!force && O.valid && O.frame == frame) return;
    MotionOwner previous[OWNER_MODELS];
    unsigned nprevious = O.count;
    memcpy(previous, O.models, nprevious * sizeof *previous);
    O.frame = frame;
    O.valid = 1;
    O.count = 0;
    uint64_t player = player_actor();
    if (!player) return;
    OwnerActor actors[OWNER_ACTORS] = {{player, 0, 0, 0}};
    unsigned count = 1;
    uint64_t equipment = component(player, 0x200);
    if (equipment) {
        // Eight static and thirteen dynamic slots, each a 24-byte actor link.
        for (unsigned slot = 0; slot < 21; ++slot) {
            uint64_t link = equipment + (slot < 8 ? 0x20 + slot * 24 : 0xe0 + (slot - 8) * 24);
            OwnerActor a;
            if (!actor_link(link, &a)) continue;
            if (!parent_is_player(a.actor, player)) { ++O.model_bind_failures; continue; }
            unsigned i;
            for (i = 0; i < count && actors[i].actor != a.actor; ++i) {}
            if (i != count) continue;
            a.kind = slot + 1;
            actors[count++] = a;
        }
    }
    for (unsigned i = 0; i < count; ++i)
        if (!collect_models(player, actors[i])) { O.count = 0; break; }
    if (player_actor() != player || component(player, 0x200) != equipment) O.count = 0;
    for (unsigned i = 0; i < O.count; ++i) {
        MotionOwner *m = &O.models[i];
        for (unsigned j = 0; j < nprevious; ++j) {
            const MotionOwner *p = &previous[j];
            if (m->player == p->player && m->actor == p->actor && m->link == p->link &&
                m->generation == p->generation && m->model == p->model) {
                m->lifetime = p->lifetime;
                break;
            }
        }
        if (!m->lifetime) m->lifetime = ++O.serial;
    }
}
static int same_owner(const MotionOwner *a, const MotionOwner *b) {
    return a->player == b->player && a->actor == b->actor && a->link == b->link &&
           a->generation == b->generation && a->model == b->model && a->lifetime == b->lifetime;
}
void motion_owner_observe(Cpu *c, uint64_t address, uint64_t size) {
    if (!motion_owner_enabled() || !address || !size || address > UINT64_MAX - size) return;
    uint64_t lr = c->x[30] - MOD_MAIN_BASE;
    uint64_t draw = 0, model;
    int shape_query = 0;
    // Both inline binding paths retain the shape object in their stack frame.
    // Read that saved pointer rather than temporary registers reused during binding.
    switch (lr) {
    case 0x35e4950: case 0x35e4b34: case 0x35e4cec:
        if (c->x[29] < 16) return;
        draw = ptr(c->x[29] - 16); shape_query = lr == 0x35e4cec; break;
    case 0x5e088c: case 0x5e0c38:
        draw = ptr(c->sp + 0xb8); shape_query = lr == 0x5e0c38; break;
    case 0x187b40: case 0x188184:
        if (c->x[29] < 24) return;
        draw = ptr(c->x[29] - 24); shape_query = lr == 0x188184; break;
    default: return;
    }
    unsigned char header[0x20];
    if (!read_at(draw, header, sizeof header)) return;
    memcpy(&model, header + 8, sizeof model);
    if (!model) return;
    uint16_t shape = 0;
    memcpy(&shape, header + 0x18, sizeof shape);
    if (shape_query && size < 256) return;
    plat_mutex_lock(&owner_lock);
    uint64_t frame = O.producer_frame;
    refresh(frame, !O.count);
    const MotionOwner *owner = NULL;
    for (unsigned i = 0; i < O.count; ++i) if (O.models[i].model == model) { owner = &O.models[i]; break; }
    if (owner) ++O.owned_queries;
    else ++O.unknown_queries;
    uint64_t part = shape_query ? (uint64_t)shape + 1 : 0;
    MotionOwnerBinding *existing = NULL;
    for (unsigned i = 0; i < O.nbindings; ++i) {
        MotionOwnerBinding *b = &O.bindings[i];
        if (address >= b->address + b->size || b->address >= address + size) continue;
        if (b->address == address && b->size == size) existing = b;
        if (!owner || !same_owner(&b->owner, owner) || b->owner.part != part || b->buffer != c->x[0]) {
            O.conflicts += !b->ambiguous;
            b->ambiguous = 1;
        }
    }
    // Never evict an entry during this producer frame: a collision must not erase an ambiguity.
    if (owner && !existing && O.nbindings < MOTION_OWNER_BINDINGS) {
        MotionOwnerBinding *b = &O.bindings[O.nbindings++];
        *b = (MotionOwnerBinding){.address = address, .size = size, .observed = frame,
                                 .buffer = c->x[0], .owner = *owner};
        b->owner.part = part;
        for (unsigned i = 0; i + 1 < O.nbindings; ++i)
            if (O.bindings[i].address < address + size && address < O.bindings[i].address + O.bindings[i].size)
                b->ambiguous = 1;
    } else if (owner && !existing) ++O.capacity_misses;
    plat_mutex_unlock(&owner_lock);
}
static int binding_address_order(const void *a, const void *b) {
    uint64_t x = ((const MotionOwnerBinding *)a)->address, y = ((const MotionOwnerBinding *)b)->address;
    return x < y ? -1 : x > y;
}

static void capture_model_buffers(MotionOwnerSnapshot *out) {
    for (unsigned i=0; i<O.count; ++i) {
        MotionOwner owner=O.models[i];
        uint32_t count=0;
        if(!read_at(owner.model+0x20,&count,4)||count>128) continue;
        uint64_t shapes=field(owner.model,0x100);
        for(unsigned shape=0;shape<count;++shape) {
            uint64_t blocks=field(shapes+(uint64_t)shape*0x70,0x20);
            for(unsigned slot=0;slot<2;++slot) {
                uint64_t buffer=field(blocks+(uint64_t)slot*0x48,8);
                buf_t b; pool_t pool;
                if(!read_at(buffer,&b,sizeof b)||b.magic!=MAGIC('B','U','F',0)||
                   !read_at((uint64_t)(uintptr_t)b.pool,&pool,sizeof pool)||pool.magic!=NVN_POOL_MAGIC||
                   b.off<0||b.size<256||b.size>65536||!pool.mem||(uint64_t)b.off>pool.size||b.size>pool.size-(uint64_t)b.off) continue;
                uint64_t address=(uint64_t)(uintptr_t)pool.mem+(uint64_t)b.off;
                if(out->count==MOTION_OWNER_BINDINGS){++O.capacity_misses;return;}
                owner.part=(uint64_t)shape+1;
                out->bindings[out->count++]=(MotionOwnerBinding){.address=address,.size=b.size,.observed=O.producer_frame,.buffer=buffer,.owner=owner};
            }
        }
    }
}

void motion_owner_capture(MotionOwnerSnapshot *out) {
    memset(out, 0, sizeof *out);
    if (!motion_owner_enabled()) return;
    plat_mutex_lock(&owner_lock);
    refresh(O.producer_frame, 1);
    ++O.captures;
    capture_model_buffers(out);
    O.nbindings=out->count;
    memcpy(O.bindings,out->bindings,out->count*sizeof *O.bindings);
    if(O.captures%43200==0) rt_log("motion direct: %u shape bindings from %u models",out->count,O.count);
    plat_mutex_unlock(&owner_lock);
    qsort(out->bindings, out->count, sizeof *out->bindings, binding_address_order);
}

void motion_owner_present(void) {
    if (!motion_owner_enabled()) return;
    plat_mutex_lock(&owner_lock);
    ++O.producer_frame;
    if (O.producer_frame % 7200 == 0)   // on by default: a line every minute or two, not every two seconds
        rt_log("motion ownership: %u models, %llu captures, %llu owned/%llu other queries, "
               "%llu conflicts, %llu capacity misses, %u attachment rejections",
               O.count, (unsigned long long)O.captures, (unsigned long long)O.owned_queries,
               (unsigned long long)O.unknown_queries, (unsigned long long)O.conflicts,
               (unsigned long long)O.capacity_misses, O.model_bind_failures);
    memcpy(O.retired, O.bindings, O.nbindings * sizeof *O.bindings);
    O.nretired = O.nbindings;
    O.nbindings = 0;
    O.valid = 0;
    plat_mutex_unlock(&owner_lock);
}

void motion_owner_forget(uint64_t buffer) {
    if (!motion_owner_enabled()) return;
    plat_mutex_lock(&owner_lock);
    for (unsigned age = 0; age < 2; ++age) {
        MotionOwnerBinding *list = age ? O.retired : O.bindings;
        unsigned count = age ? O.nretired : O.nbindings;
        for (unsigned i = 0; i < count; ++i) {
            MotionOwnerBinding *b = &list[i];
            if (b->buffer != buffer) continue;
            b->ambiguous = 1;
            for (unsigned j = 0; j < O.count; ++j)
                if (O.models[j].model == b->owner.model) O.models[j].lifetime = ++O.serial;
        }
    }
    plat_mutex_unlock(&owner_lock);
}

void motion_owner_reset(void) {
    if (!motion_owner_enabled()) return;
    plat_mutex_lock(&owner_lock);
    O.nbindings = O.nretired = O.count = 0;
    O.valid = 0;
    ++O.serial;
    plat_mutex_unlock(&owner_lock);
}

void motion_owner_forget_range(uint64_t address, uint64_t size) {
    if (!motion_owner_enabled() || !size || address>UINT64_MAX-size) return;
    plat_mutex_lock(&owner_lock);
    for(unsigned i=0;i<O.count;++i) {
        int hit=0;
        for(unsigned age=0;age<2 && !hit;++age) {
            MotionOwnerBinding *list=age?O.retired:O.bindings;
            unsigned count=age?O.nretired:O.nbindings;
            for(unsigned j=0;j<count;++j) {
                MotionOwnerBinding *b=&list[j];
                if(b->owner.model==O.models[i].model && b->address<address+size && address<b->address+b->size){hit=1;break;}
            }
        }
        if(hit) O.models[i].lifetime=++O.serial;
    }
    plat_mutex_unlock(&owner_lock);
}
