// Level-of-detail settings for Ocarina (the set lod, recomp/patches-v111.txt). The game reads its LoD distances from
// its data: the global game__lod__GameLODParam, the foliage's game__foliage__ResLodSetting (one for the console, one
// for handheld play), two scales in each model's own configuration, and a flag per plant type. The set's sites call the
// host functions here right after each of them is read, and the settings change what was read:
//
//   NX2NAVI_LOD_MAX_DIST             percent of GameLODParam's MaxLevelDistScale
//   NX2NAVI_LOD_START_DIST           percent of its LodStartDistCoefForConsole
//   NX2NAVI_LOD_START_DIST_HANDHELD  percent of its LodStartDistCoefForHandheld
//   NX2NAVI_LOD_STATIC_OFFSET        added to its StaticOffset
//   NX2NAVI_LOD_DYNAMIC_OFFSET       added to its DynamicOffsetForNormal
//   NX2NAVI_FOLIAGE_UNDERGROWTH_DIST percent of the foliage setting's Dist_Undergrowth0 and Dist_Undergrowth1
//   NX2NAVI_FOLIAGE_ORNAMENT_DIST    percent of its Dist_Ornament
//   NX2NAVI_FOLIAGE_TREE_DIST        percent of its Dist_LowTree, Dist_LowTreeShootLeaf, Range_LowTree01 and Range_LowTree12
//   NX2NAVI_FOLIAGE_TREE_SHADOW_DIST percent of the tree distance, into its Dist_LowTreeShadow (below)
//   NX2NAVI_FOLIAGE_TURF_POLY_LEN    percent of its Turf_1PolyLen
//   NX2NAVI_MODEL_LOD_MAX_DIST       percent of each model's GameModelController_LodMaxLevelDistScale
//   NX2NAVI_MODEL_LOD_START_DIST     percent of each model's GameModelController_LodStartDistCoef
//   NX2NAVI_PLANT_DRAW_DIST          extended: every plant type's ExtendDrawDist flag is set
//
// At 100, 0 and game a value stays what the game read. The GameLODParam and foliage settings are live: the objects the
// game read are remembered with the game's own values, and lod_frame writes them again when a setting changes. The
// model and plant settings act on what is loaded after they are set, so they take a restart.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "cpu.h"
#include "host.h"
#include "lod.h"
#include "platform/platform.h"

// What scales or shifts a field
enum {
    K_NONE, K_MAX, K_START, K_START_HH, K_STATIC, K_DYNAMIC,   // GameLODParam
    K_UNDER, K_ORNAMENT, K_TREE, K_TREE_SHADOW, K_TURF,        // the foliage setting
    K_N
};
typedef struct { const char *name; uint32_t off; int k; } Field;

// GameLODParam, as its reader at main+0x31a2560 names and places the fields
static const Field kParam[] = {
    {"DynamicOffsetForNormal", 0x38, K_DYNAMIC},   {"DynamicOffsetForStaticDemo", 0x3c, K_NONE},
    {"FovyScaleForNoraml", 0x40, K_NONE},          {"FovyScaleForTimeline", 0x44, K_NONE},
    {"LodStartDistCoefForConsole", 0x48, K_START}, {"LodStartDistCoefForHandheld", 0x4c, K_START_HH},
    {"MaxLevelDistScale", 0x50, K_MAX},            {"StaticOffset", 0x54, K_STATIC},
};
// game__foliage__ResLodSetting, as its reader at main+0x2bed300 names and places the fields
static const Field kFoliage[] = {
    {"Dist_Undergrowth0", 0x48, K_UNDER},    {"Dist_Undergrowth1", 0x4c, K_UNDER},         {"Dist_Ornament", 0x44, K_ORNAMENT},
    {"Dist_LowTreeShootLeaf", 0x40, K_TREE}, {"Dist_LowTreeShadow", 0x3c, K_TREE_SHADOW}, {"Dist_LowTree", 0x38, K_TREE},
    {"Range_LowTree01", 0x50, K_TREE},       {"Range_LowTree12", 0x54, K_TREE},            {"Turf_1PolyLen", 0x58, K_TURF},
};
#define NF(a) ((int)(sizeof(a) / sizeof(a)[0]))
#define MAX_FIELDS 9
#define PLANT_EXTEND_DRAW_DIST 0x110   // the plant parameter's ExtendDrawDist (a byte), as its reader at main+0x2bf4740 places it

// An object the game read: where it is, its first word (the class's table of functions, by which a later look knows
// the object is still there), the game's own values and the ones written
typedef struct {
    uint64_t p, head;
    const Field *f;
    int n;
    const char *what;
    float game[MAX_FIELDS], ours[MAX_FIELDS];
} Obj;
#define MAX_OBJS 64
static Obj objs[MAX_OBJS];
static int n_objs;
static PlatMutex lock = PLAT_MUTEX_INIT;

static float factor[K_N];   // the percent settings as factors, the offsets as they are
static float k_model_max = 1, k_model_start = 1;
static int plant_extend, ready;
static unsigned cfg_seen;

static float pct(const char *name) { return (float)cfg_int(name) / 100.0f; }

// The live settings, read again. 1 when one of them changed.
static int read_live(void) {
    float f[K_N];
    f[K_NONE] = 1;
    f[K_MAX] = pct("NX2NAVI_LOD_MAX_DIST");
    f[K_START] = pct("NX2NAVI_LOD_START_DIST");
    f[K_START_HH] = pct("NX2NAVI_LOD_START_DIST_HANDHELD");
    f[K_STATIC] = (float)cfg_int("NX2NAVI_LOD_STATIC_OFFSET");
    f[K_DYNAMIC] = (float)cfg_int("NX2NAVI_LOD_DYNAMIC_OFFSET");
    f[K_UNDER] = pct("NX2NAVI_FOLIAGE_UNDERGROWTH_DIST");
    f[K_ORNAMENT] = pct("NX2NAVI_FOLIAGE_ORNAMENT_DIST");
    f[K_TREE] = pct("NX2NAVI_FOLIAGE_TREE_DIST");
    f[K_TREE_SHADOW] = pct("NX2NAVI_FOLIAGE_TREE_SHADOW_DIST");
    f[K_TURF] = pct("NX2NAVI_FOLIAGE_TURF_POLY_LEN");
    int changed = memcmp(f, factor, sizeof f) != 0;
    memcpy(factor, f, sizeof f);
    return changed;
}

static void settings(void) {
    if (ready) return;
    cfg_seen = cfg_version();
    read_live();
    k_model_max = pct("NX2NAVI_MODEL_LOD_MAX_DIST");
    k_model_start = pct("NX2NAVI_MODEL_LOD_START_DIST");
    plant_extend = !strcmp(cfg_value("NX2NAVI_PLANT_DRAW_DIST"), "extended");
    ready = 1;
}

static float load_f(uint64_t at) {
    float f;
    memcpy(&f, (const void *)(uintptr_t)at, sizeof f);
    return f;
}

static void store_f(uint64_t at, float f) { memcpy((void *)(uintptr_t)at, &f, sizeof f); }

static uint64_t load_q(uint64_t at) {
    uint64_t v;
    memcpy(&v, (const void *)(uintptr_t)at, sizeof v);
    return v;
}

// Dist_LowTreeShadow is how far a low tree outside the camera's view keeps its detailed form, for the shadow it casts
// into the view. The tree gather shader takes it only while a flag of the foliage block is clear, and the game sets
// that flag: every tree keeps its form to Dist_LowTree, in view or not, and the value does nothing. With the shader's
// test of the flag gone (vk_pipeline.c spirv_tree_shadow) the value acts, and it is written as the tree distance in
// effect times the setting. The shader's other path treats the trees outside the view a little differently even at
// the same distance (the near cascade takes about 6 percent more fragments), so a run that starts with the setting at
// 100 keeps the game's shader: the shaders are made once, which makes the setting one for the next start.
enum { F_LOW_TREE = 5 };   // kFoliage's Dist_LowTree

int lod_tree_shadow_split(void) {
    static int on = -1;
    if (on < 0) on = rt_patch_set("lod") >= 0 && cfg_int("NX2NAVI_FOLIAGE_TREE_SHADOW_DIST") != 100;
    return on;
}

static float changed_value(float game, int k) { return k == K_STATIC || k == K_DYNAMIC ? game + factor[k] : game * factor[k]; }

// The settings on the object o, whose own values are in o->game: writes the changed values and logs the object's line.
// Under the lock.
static void write_obj(Obj *o, int at) {
    char line[640];
    size_t len = 0;
    line[0] = 0;
    for (int i = 0; i < o->n; i++) {
        float v = o->game[i], w = changed_value(v, o->f[i].k);
        if (o->f == kFoliage && o->f[i].k == K_TREE_SHADOW && lod_tree_shadow_split())
            w = changed_value(o->game[F_LOW_TREE], K_TREE) * factor[K_TREE_SHADOW];
        if (w != load_f(o->p + o->f[i].off)) store_f(o->p + o->f[i].off, w);
        o->ours[i] = w;
        if (len >= sizeof line) continue;
        if (w == v) len += (size_t)snprintf(line + len, sizeof line - len, "%s%s %g", i ? ", " : "", o->f[i].name, (double)v);
        else
            len += (size_t)snprintf(line + len, sizeof line - len, "%s%s %g -> %g", i ? ", " : "", o->f[i].name, (double)v,
                                    (double)w);
    }
    rt_log("lod: %s %d at %#llx: %s", o->what, at, (unsigned long long)o->p, line);
}

// A reader of the object at p ended well. A new object is remembered with the values the game read; one read again
// keeps the values written here where they still stand, and takes a value the game put there anew as its own.
static void read_done(uint64_t p, const Field *f, int n, const char *what) {
    settings();
    plat_mutex_lock(&lock);
    int at = 0;
    while (at < n_objs && objs[at].p != p) at++;
    int fresh = at == n_objs;
    if (fresh && n_objs == MAX_OBJS) {   // full: the oldest goes
        memmove(objs, objs + 1, sizeof objs - sizeof objs[0]);
        at = --n_objs;
    }
    Obj *o = &objs[at];
    int changed = fresh;
    if (fresh) n_objs++, o->p = p, o->f = f, o->n = n, o->what = what;
    o->head = load_q(p);
    for (int i = 0; i < n; i++) {
        float v = load_f(p + f[i].off);
        if (!fresh && v == o->ours[i]) continue;
        o->game[i] = v, changed = 1;
    }
    if (changed) write_obj(o, at);
    plat_mutex_unlock(&lock);
}

// The site at main+0x31a28c4, where every path of GameLODParam's reader ends: its `mov w0, w21`, then the settings on
// the parameter in x20 when the read went well (w21 = 0).
void lod_param_host(Cpu *c) {
    c->x[0] = (uint32_t)c->x[21];
    if (!(uint32_t)c->x[21]) read_done(c->x[20], kParam, NF(kParam), "GameLODParam");
}

// The site at main+0x2bed698, where every path of the foliage LoD setting's reader ends: its `mov w0, w21`, then the
// settings on the distances of the setting in x20 when the read went well.
void lod_foliage_host(Cpu *c) {
    c->x[0] = (uint32_t)c->x[21];
    if (!(uint32_t)c->x[21]) read_done(c->x[20], kFoliage, NF(kFoliage), "foliage LoD setting");
}

// Once per presented frame: a live setting changed in the menu, so every remembered object takes the new values. An
// object whose first word or values are no longer what was left there has been freed or read anew, and is forgotten.
void lod_frame(void) {
    if (!ready || cfg_version() == cfg_seen) return;
    plat_mutex_lock(&lock);
    cfg_seen = cfg_version();
    if (read_live()) {
        int kept = 0;
        for (int i = 0; i < n_objs; i++) {
            Obj *o = &objs[i];
            int there = load_q(o->p) == o->head;
            for (int j = 0; there && j < o->n; j++) there = load_f(o->p + o->f[j].off) == o->ours[j];
            if (!there) continue;
            if (kept != i) objs[kept] = *o;
            write_obj(&objs[kept], kept);
            kept++;
        }
        n_objs = kept;
    }
    plat_mutex_unlock(&lock);
}

// The site at main+0x2bf6010, where every path of the plant parameter's reader ends: its `mov w0, w21`, then the
// plant type's ExtendDrawDist flag in x20 set when the setting asks for it.
void lod_plant_host(Cpu *c) {
    c->x[0] = (uint32_t)c->x[21];
    if ((uint32_t)c->x[21]) return;
    settings();
    static uint32_t seen, had, set;
    uint8_t *flag = (uint8_t *)(uintptr_t)(c->x[20] + PLANT_EXTEND_DRAW_DIST);
    seen++;
    if (*flag) had++;
    else if (plant_extend) *flag = 1, set++;
    if (seen == 1 || seen == 64 || seen == 512)
        rt_log("lod: plant parameters read: %u; ExtendDrawDist the game's own on %u, set here on %u "
               "(NX2NAVI_PLANT_DRAW_DIST=%s)", seen, had, set, plant_extend ? "extended" : "game");
}

// A model's scale in a W register, as float bits, times k. *own counts the values that were not 1.
static uint64_t scaled(uint64_t bits, float k, uint32_t *own) {
    float f;
    uint32_t w = (uint32_t)bits;
    memcpy(&f, &w, sizeof f);
    if (f != 1.0f) (*own)++;
    f *= k;
    memcpy(&w, &f, sizeof w);
    return w;
}

// The sites at main+0x8af20 and main+0x8ab38 in the reader of a model's configuration, where the value of
// GameModelController_LodMaxLevelDistScale (w27) and of GameModelController_LodStartDistCoef (w19) is final whether
// the configuration had the key or not: the displaced `ldr x25, [sp, #0x48]`, then the scale. A model without a
// configuration takes 1.0 at main+0x897a0 and main+0x897c0, which read the scaled 1.0 from the patch page instead.
static uint32_t n_models, n_model_max, n_model_start;

void lod_model_max_host(Cpu *c) {
    memcpy(&c->x[25], (const void *)(uintptr_t)(c->sp + 0x48), 8);
    settings();
    c->x[27] = scaled(c->x[27], k_model_max, &n_model_max);
    if (++n_models == 256 || n_models == 4096)
        rt_log("lod: model configurations read: %u; with their own LodMaxLevelDistScale %u, LodStartDistCoef %u", n_models,
               n_model_max, n_model_start);
}

void lod_model_start_host(Cpu *c) {
    memcpy(&c->x[25], (const void *)(uintptr_t)(c->sp + 0x48), 8);
    settings();
    c->x[19] = scaled(c->x[19], k_model_start, &n_model_start);
}

// The patch page's two floats (+80, +84): 1.0 times the two model settings, for the models without a configuration.
// The capacities of the foliage's GPU lists. The game's code sizes each list with a constant and its shaders carry
// the same constant, and the offsets of the arrays behind a list: an entry past the capacity is dropped, and which
// entries those are changes from frame to frame, so with the distances above raised the vegetation flickers.
// NX2NAVI_FOLIAGE_CAPACITY (percent, 100..400) scales both by one factor: the patch page words the set lod's sites
// load in place of the constants (+0x800 and up, in kCaps' order), and the shaders' constants
// (vk_pipeline.c spirv_foliage_lists).
// The lists' buffers come from the foliage's scene heap, which the game creates with a constant size of 248 MiB inside
// its Foliage heap of 250 MiB and fills anew at each area change. Lists of the larger capacities find no room there (the
// buffer comes back unmapped and the game writes through its null pointer), so both grow by the factor too: the scene
// heap's size is the page word +0x814, and the Foliage heap of the HeapDef table grows by lod_foliage_heap_kib
// (resolution.c res_heapdef_build).
static const uint32_t kCaps[] = {20480, 40960, 16384, 2048, 225280};
#define SCENE_HEAP_MIB 248u

// Read from the setting at each call: the HeapDef table is rewritten before lod_init runs. Without the rewritten table
// (lod_init) the Foliage heap cannot hold a larger scene heap, and the lists keep the game's capacities.
static int cap_refused;

uint32_t lod_foliage_capacity(void) {
    if (rt_patch_set("lod") < 0 || cap_refused) return 1;
    int pct = cfg_int("NX2NAVI_FOLIAGE_CAPACITY");
    return pct >= 200 ? (uint32_t)(pct > 400 ? 400 : pct) / 100 : 1;
}

uint32_t lod_foliage_heap_kib(void) { return (lod_foliage_capacity() - 1) * SCENE_HEAP_MIB << 10; }

int lod_available(void) { return rt_patch_set("lod") >= 0; }

void lod_init(void) {
    if (rt_patch_set("lod") < 0) return;
    settings();
    uint8_t *page = rt_patch_page();
    if (lod_foliage_capacity() > 1) {
        fs_init();   // romfs.c serves the grown HeapDef from here on (res_heapdef_served)
        if (!res_heapdef_ok()) {
            cap_refused = 1;
            rt_log("lod: no grown HeapDef table; the foliage lists keep the game's capacities");
        }
    }
    uint32_t cap_factor = lod_foliage_capacity(), scene_heap = (SCENE_HEAP_MIB << 20) * cap_factor;
    for (size_t i = 0; i < sizeof kCaps / sizeof kCaps[0]; i++) {
        uint32_t v = kCaps[i] * cap_factor;
        memcpy(page + 0x800 + 4 * i, &v, 4);
    }
    memcpy(page + 0x814, &scene_heap, 4);
    if (cap_factor > 1) rt_log("lod: foliage list capacities x%u, their scene heap %u MiB", cap_factor, scene_heap >> 20);
    float a = k_model_max, b = k_model_start;
    memcpy(page + 80, &a, 4);
    memcpy(page + 84, &b, 4);
    rt_log("lod: GameLODParam max dist x%g, start dist x%g (handheld x%g), static offset %+g, dynamic offset %+g; foliage: "
           "undergrowth x%g, ornament x%g, tree x%g, tree shadow x%g, turf x%g; models: max dist x%g, start dist x%g; "
           "plants: %s draw distance", (double)factor[K_MAX], (double)factor[K_START], (double)factor[K_START_HH],
           (double)factor[K_STATIC], (double)factor[K_DYNAMIC], (double)factor[K_UNDER], (double)factor[K_ORNAMENT],
           (double)factor[K_TREE], (double)factor[K_TREE_SHADOW], (double)factor[K_TURF], (double)k_model_max,
           (double)k_model_start, plant_extend ? "extended" : "the game's");
}
