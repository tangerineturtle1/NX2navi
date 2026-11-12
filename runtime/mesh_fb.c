// The mesh shader fallback's modules, for a GPU without VK_EXT_mesh_shader (vk_exec_draw.c "mesh fallback" draws with
// them; docs/architecture.md, "Mesh fallback").
//
// The translator turns a task or mesh program into a compute module whose shared memory is the program's stage buffer
// and wraps it in a TaskEXT or MeshEXT entry function (tools/xlate/MeshSpirv.cs). The wrapping is undone here and
// replaced: the translator's main function stays as it is, the entry function that hands the stage buffer to the mesh
// pipeline goes, and a compute entry function copies the stage buffer into a storage buffer instead.
//   task:  main; the payload area into the task buffer at the workgroup's place.
//   mesh:  [the payload from the task buffer into the payload area] main; the vertex lines and the primitive area into
//          the workgroup's slot of the mesh buffer.
//   draw:  a vertex module made here reads a slot per instance: vertex index / indices per primitive is the
//          primitive, its index byte names the vertex, and the vertex's lines are the outputs.
// The module is made from the translation the shader cache already holds, so the translator and with it every cache
// key stay as they are: a card with mesh shaders never sees these modules, and one without needs no other cache.
//
// Mesh workgroups after a task stage. A task workgroup starts up to `threads` mesh workgroups, and how many is known
// only on the GPU. A pass between the two dispatches (shaders/meshfb_scan.comp) lists them in task order: entry g of
// the list names the task workgroup and the index the mesh program sees as its workgroup id. That id comes from the
// WorkgroupId built-in in the translated code; here the built-in counts list entries, so main reads a private
// variable the entry function fills from the list.
//
// Opcode and enum numbers are the SPIR-V 1.6 specification's and SPV_EXT_mesh_shader's.
#include "mesh_fb.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    OpName = 5, OpExtension = 10, OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16, OpCapability = 17,
    OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeArray = 28,
    OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32, OpTypeFunction = 33, OpConstant = 43,
    OpConstantComposite = 44, OpConstantNull = 46, OpFunction = 54, OpFunctionEnd = 56, OpFunctionCall = 57,
    OpVariable = 59, OpLoad = 61, OpStore = 62, OpCopyMemory = 63, OpAccessChain = 65, OpInBoundsAccessChain = 66,
    OpPtrAccessChain = 67, OpArrayLength = 68, OpDecorate = 71, OpMemberDecorate = 72, OpCompositeConstruct = 80,
    OpCompositeExtract = 81, OpCopyObject = 83, OpBitcast = 124, OpIAdd = 128, OpISub = 130, OpIMul = 132, OpUDiv = 134,
    OpSelect = 169, OpINotEqual = 171, OpULessThan = 176, OpULessThanEqual = 178, OpShiftRightLogical = 194,
    OpBitwiseAnd = 199, OpControlBarrier = 224, OpPhi = 245, OpLoopMerge = 246, OpSelectionMerge = 247, OpLabel = 248,
    OpBranch = 249, OpBranchConditional = 250, OpReturn = 253, OpDecorateId = 332, OpDecorateString = 5632,
};
enum { ScInput = 1, ScOutput = 3, ScWorkgroup = 4, ScPrivate = 6, ScFunction = 7, ScStorageBuffer = 12, ScTaskPayload = 5402 };
enum { DecBlock = 2, DecArrayStride = 6, DecBuiltIn = 11, DecInvariant = 18, DecNonWritable = 24, DecLocation = 30,
       DecBinding = 33, DecDescriptorSet = 34, DecOffset = 35 };
enum { BiPosition = 0, BiPointSize = 1, BiWorkgroupId = 26, BiLocalInvocationIndex = 29, BiVertexIndex = 42,
       BiInstanceIndex = 43 };
enum { CapShader = 1, CapMeshShading = 5283, ModelVertex = 0, ModelGLCompute = 5, ModelTask = 5364, ModelMesh = 5365,
       ModeLocalSize = 17 };

// ---- the program's layout

static void fail(char *err, size_t cap, const char *fmt, ...) {
    if (!err || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

int meshfb_info(const uint8_t *control, const uint8_t *header, int stage, MeshFbInfo *m, char *err, size_t cap) {
    uint32_t w58, w5c, w60;
    memset(m, 0, sizeof *m);
    memcpy(&w58, control + 0x58, 4), memcpy(&w5c, control + 0x5c, 4), memcpy(&w60, control + 0x60, 4);
    m->stage = stage;
    m->threads = w58 & 0xffff ? w58 & 0xffff : 1;
    m->payload_bytes = (w58 >> 24) * 256;
    m->maxv = w60 & 0xffff, m->maxp = w60 >> 16;
    uint32_t topo = w5c >> 24;
    m->ipp = topo == 1 ? 1 : topo == 2 ? 2 : 3;
    m->has_task = stage == 7 && (w5c >> 16 & 0xff) != 0;
    // The payload as the translator sizes it (tools/xlate/Program.cs Translate): a size of 0 is one 256-byte unit, and
    // the header of 0x20 bytes comes on top.
    if (stage == 6 || m->has_task) m->payload_bytes = (m->payload_bytes ? m->payload_bytes : 256) + 0x20;
    if (stage == 6) {
        m->pay = 0, m->shared_bytes = m->payload_bytes;
        return 1;
    }
    if (stage != 7) return fail(err, cap, "stage %d is neither task nor mesh", stage), 0;
    if (!m->maxv || !m->maxp || m->maxv > 256) return fail(err, cap, "limits of %u vertices and %u primitives", m->maxv, m->maxp), 0;
    // The output map of the program header: system values B at byte 0x35, one bit per component from 0x60; generic
    // vectors from byte 0x36, a nibble each. System values A and C never appear in the game's mesh programs.
    if (header[0x34] || header[0x48] || header[0x49]) return fail(err, cap, "output map with system values A or C"), 0;
    for (int b = 0; b < 8; b++)
        if (header[0x35] >> b & 1) m->lines[m->nlines++] = (uint16_t)(0x60 + 4 * b);
    for (int g = 0; g < 32; g++)
        for (int c = 0; c < 4; c++)
            if (header[0x36 + g / 2] >> (4 * (g & 1) + c) & 1) m->lines[m->nlines++] = (uint16_t)(0x80 + 16 * g + 4 * c);
    uint32_t groups = (m->maxv + 31) / 32;
    m->attr = 0;
    m->map = (m->attr + groups * m->nlines * 128 + 15) & ~15u;
    m->pay = (m->map + 4 + m->ipp * m->maxp + 15) & ~15u;
    m->shared_bytes = m->pay + (m->has_task ? m->payload_bytes : 0);
    return 1;
}

// ---- a growing list of SPIR-V words

typedef struct { uint32_t *w; size_t n, cap; int oom; } Wb;

static void wb_put(Wb *b, const uint32_t *w, size_t n) {
    if (b->n + n > b->cap) {
        size_t cap = (b->n + n) * 2 + 256;
        uint32_t *g = realloc(b->w, cap * 4);
        if (!g) { b->oom = 1; return; }
        b->w = g, b->cap = cap;
    }
    memcpy(b->w + b->n, w, n * 4);
    b->n += n;
}

static void wb_ins(Wb *b, uint32_t op, const uint32_t *operands, size_t n) {
    uint32_t head = (uint32_t)((n + 1) << 16) | op;
    wb_put(b, &head, 1);
    wb_put(b, operands, n);
}
#define INS(b, op, ...) wb_ins(b, op, (const uint32_t[]){__VA_ARGS__}, sizeof((const uint32_t[]){__VA_ARGS__}) / 4)

// What a module under construction adds: decorations, then types, constants and variables, then one function.
typedef struct {
    uint32_t bound;
    Wb deco, glob, fn;
    uint32_t t_void, t_bool, t_uint, t_fnvoid, t_v3uint;   // existing or new
    uint32_t p_fn_uint, p_sb_uint;
    struct { uint32_t v, id; } k[96];
    int nk;
} Cx;

static uint32_t nid(Cx *c) { return c->bound++; }

static uint32_t K(Cx *c, uint32_t v) {   // a 32-bit unsigned constant
    for (int i = 0; i < c->nk; i++)
        if (c->k[i].v == v) return c->k[i].id;
    uint32_t id = nid(c);
    INS(&c->glob, OpConstant, c->t_uint, id, v);
    if (c->nk < 96) c->k[c->nk].v = v, c->k[c->nk].id = id, c->nk++;
    return id;
}

static uint32_t E_(Cx *c, uint32_t op, uint32_t type, const uint32_t *operands, size_t n) {   // an instruction with a result
    uint32_t id = nid(c), head[3] = {(uint32_t)((n + 3) << 16) | op, type, id};
    wb_put(&c->fn, head, 3);
    wb_put(&c->fn, operands, n);
    return id;
}
#define E(c, op, type, ...) E_(c, op, type, (const uint32_t[]){__VA_ARGS__}, sizeof((const uint32_t[]){__VA_ARGS__}) / 4)
#define DO(c, op, ...) INS(&(c)->fn, op, __VA_ARGS__)

static uint32_t add(Cx *c, uint32_t a, uint32_t b) { return E(c, OpIAdd, c->t_uint, a, b); }
static uint32_t mul(Cx *c, uint32_t a, uint32_t b) { return E(c, OpIMul, c->t_uint, a, b); }
static uint32_t umin(Cx *c, uint32_t a, uint32_t b) {
    return E(c, OpSelect, c->t_uint, E(c, OpULessThan, c->t_bool, a, b), a, b);
}

// if (cond) { ...the instructions emitted until if_end }
static uint32_t if_begin(Cx *c, uint32_t cond) {
    uint32_t then = nid(c), merge = nid(c);
    DO(c, OpSelectionMerge, merge, 0);
    DO(c, OpBranchConditional, cond, then, merge);
    DO(c, OpLabel, then);
    return merge;
}
static void if_end(Cx *c, uint32_t merge) {
    DO(c, OpBranch, merge);
    DO(c, OpLabel, merge);
}

// for (i = start; i < limit; i += step) { ...the instructions emitted until loop_end }; counter: a Function variable
typedef struct { uint32_t head, cont, merge, i, counter, step; } Loop;
static Loop loop_begin(Cx *c, uint32_t counter, uint32_t start, uint32_t limit, uint32_t step) {
    Loop l = {nid(c), nid(c), nid(c), 0, counter, step};
    uint32_t cond = nid(c), body = nid(c);
    DO(c, OpStore, counter, start);
    DO(c, OpBranch, l.head);
    DO(c, OpLabel, l.head);
    DO(c, OpLoopMerge, l.merge, l.cont, 0);
    DO(c, OpBranch, cond);
    DO(c, OpLabel, cond);
    l.i = E(c, OpLoad, c->t_uint, counter);
    DO(c, OpBranchConditional, E(c, OpULessThan, c->t_bool, l.i, limit), body, l.merge);
    DO(c, OpLabel, body);
    return l;
}
static void loop_end(Cx *c, const Loop *l) {
    DO(c, OpBranch, l->cont);
    DO(c, OpLabel, l->cont);
    DO(c, OpStore, l->counter, add(c, l->i, l->step));
    DO(c, OpBranch, l->head);
    DO(c, OpLabel, l->merge);
}

// A storage buffer of 32-bit words at (set 0, binding): its variable. readonly: the module only reads it.
static uint32_t word_buffer(Cx *c, uint32_t binding, int readonly) {
    uint32_t arr = nid(c), st = nid(c), ptr = nid(c), var = nid(c);
    INS(&c->glob, OpTypeRuntimeArray, arr, c->t_uint);
    INS(&c->glob, OpTypeStruct, st, arr);
    INS(&c->glob, OpTypePointer, ptr, ScStorageBuffer, st);
    INS(&c->glob, OpVariable, ptr, var, ScStorageBuffer);
    INS(&c->deco, OpDecorate, arr, DecArrayStride, 4);
    INS(&c->deco, OpDecorate, st, DecBlock);
    INS(&c->deco, OpMemberDecorate, st, 0, DecOffset, 0);
    if (readonly) INS(&c->deco, OpMemberDecorate, st, 0, DecNonWritable);
    INS(&c->deco, OpDecorate, var, DecDescriptorSet, 0);
    INS(&c->deco, OpDecorate, var, DecBinding, binding);
    return var;
}
static uint32_t buf_ptr(Cx *c, uint32_t var, uint32_t index) { return E(c, OpAccessChain, c->p_sb_uint, var, K(c, 0), index); }
static uint32_t buf_load(Cx *c, uint32_t var, uint32_t index) { return E(c, OpLoad, c->t_uint, buf_ptr(c, var, index)); }

// ---- a translated task or mesh module to its compute module

typedef struct { uint32_t at, op, len; } In;   // an instruction: its first word's index, opcode, word count

static int id_in(const uint32_t *set, int n, uint32_t id) {
    for (int i = 0; i < n; i++)
        if (set[i] == id) return 1;
    return 0;
}

static int is_decl(uint32_t op) {   // a type, constant or global variable declaration
    return (op >= OpTypeVoid && op <= 39) || (op >= 41 && op <= 52) || op == OpVariable || op == 1;
}

static uint32_t *convert(const uint32_t *w, uint64_t words, const MeshFbInfo *m, uint32_t binding, uint32_t task_words,
                         uint64_t *out_words, char *err, size_t cap) {
    In *in = NULL;
    uint32_t *removed = NULL, *gvars = NULL;
    Wb out = {0};
    Cx c = {0};
#define FAIL(...) do { fail(err, cap, __VA_ARGS__); goto bad; } while (0)
    if (words < 6 || w[0] != 0x07230203) return fail(err, cap, "not a SPIR-V module"), NULL;
    if (w[1] < 0x00010400) return fail(err, cap, "SPIR-V %u.%u, 1.4 expected", w[1] >> 16 & 0xff, w[1] >> 8 & 0xff), NULL;
    size_t n = 0, ncap = 0;
    for (uint64_t p = 5; p < words;) {
        uint32_t len = w[p] >> 16, op = w[p] & 0xffff;
        if (!len || p + len > words) return fail(err, cap, "malformed SPIR-V"), NULL;
        if (n == ncap) {
            ncap = ncap ? ncap * 2 : 4096;
            In *g = realloc(in, ncap * sizeof *in);
            if (!g) FAIL("out of memory");
            in = g;
        }
        in[n++] = (In){(uint32_t)p, op, len};
        p += len;
    }
    c.bound = w[3];
    int mesh = m->stage == 7, n_removed = 0, n_gvars = 0, n_entry = 0, n_shared = 0;
    removed = malloc((n + 1) * 4), gvars = malloc((n + 16) * 4);
    if (!removed || !gvars) FAIL("out of memory");
    uint32_t old_entry = 0, model = 0, shared_var = 0, shared_type = 0, wg_var = 0, lii_var = 0, first_fn = 0, first_decl = 0;
    // pass 1: what the module has
    for (size_t i = 0; i < n; i++) {
        const uint32_t *a = w + in[i].at + 1;
        uint32_t op = in[i].op, len = in[i].len;
        if (op == OpEntryPoint) n_entry++, model = a[0], old_entry = a[1];
        else if (op == OpTypeVoid) c.t_void = a[0];
        else if (op == OpTypeBool) c.t_bool = a[0];
        else if (op == OpTypeInt && a[1] == 32 && a[2] == 0) c.t_uint = a[0];
        else if (op == OpTypeVector && a[1] == c.t_uint && c.t_uint && a[2] == 3) c.t_v3uint = a[0];
        else if (op == OpTypeFunction && len == 3 && a[1] == c.t_void && c.t_void) c.t_fnvoid = a[0];
        else if (op == OpTypePointer && (a[1] == ScOutput || a[1] == ScTaskPayload)) removed[n_removed++] = a[0];
        else if (op == OpVariable && !first_fn) {
            if (a[2] == ScOutput || a[2] == ScTaskPayload) removed[n_removed++] = a[1];
            else gvars[n_gvars++] = a[1];
            if (a[2] == ScWorkgroup) n_shared++, shared_var = a[1], shared_type = a[0];
        } else if (op == OpFunction && !first_fn) first_fn = (uint32_t)i;
        if (!first_decl && !first_fn && is_decl(op)) first_decl = (uint32_t)i;
    }
    if (n_entry != 1 || model != (uint32_t)(mesh ? ModelMesh : ModelTask)) FAIL("the entry point is not a %s one", mesh ? "mesh" : "task");
    if (n_shared != 1) FAIL("%d workgroup variables (one shared memory array expected)", n_shared);
    if (!c.t_void || !c.t_bool || !c.t_uint || !c.t_fnvoid || !first_fn || !first_decl) FAIL("a basic type or the functions are missing");
    {   // the shared memory array holds the layout this module is asked to copy
        uint32_t arr = 0, len_id = 0, len = 0;
        for (size_t i = 0; i < n; i++) {
            const uint32_t *a = w + in[i].at + 1;
            if (in[i].op == OpTypePointer && a[0] == shared_type) arr = a[2];
        }
        for (size_t i = 0; i < n; i++) {
            const uint32_t *a = w + in[i].at + 1;
            if (in[i].op == OpTypeArray && a[0] == arr && a[1] == c.t_uint) len_id = a[2];
        }
        for (size_t i = 0; i < n; i++) {
            const uint32_t *a = w + in[i].at + 1;
            if (in[i].op == OpConstant && a[1] == len_id && len_id) len = a[2];
        }
        if (len * 4 < m->shared_bytes) FAIL("shared memory of %u bytes, the program's layout needs %u", len * 4, m->shared_bytes);
    }
    for (size_t i = 0; i < n; i++) {
        const uint32_t *a = w + in[i].at + 1;
        if (in[i].op != OpDecorate || in[i].len < 4 || a[1] != DecBuiltIn || id_in(removed, n_removed, a[0])) continue;
        if (a[2] == BiWorkgroupId) wg_var = a[0];
        if (a[2] == BiLocalInvocationIndex) lii_var = a[0];
    }
    // the old entry function, and the function it calls: the translator's main
    size_t e0 = 0, e1 = 0;
    uint32_t main_fn = 0, n_calls = 0;
    for (size_t i = first_fn; i < n; i++)
        if (in[i].op == OpFunction && w[in[i].at + 2] == old_entry) {
            e0 = i;
            for (e1 = i; e1 < n && in[e1].op != OpFunctionEnd; e1++)
                if (in[e1].op == OpFunctionCall) n_calls++, main_fn = w[in[e1].at + 3];
            break;
        }
    if (!e0 || e1 >= n || n_calls != 1) FAIL("the entry function calls %u functions (one expected)", n_calls);

    // ---- new types, variables and the entry function
    if (!c.t_v3uint) c.t_v3uint = nid(&c), INS(&c.glob, OpTypeVector, c.t_v3uint, c.t_uint, 3);
    c.p_fn_uint = nid(&c), INS(&c.glob, OpTypePointer, c.p_fn_uint, ScFunction, c.t_uint);
    c.p_sb_uint = nid(&c), INS(&c.glob, OpTypePointer, c.p_sb_uint, ScStorageBuffer, c.t_uint);
    uint32_t p_shared = nid(&c);
    INS(&c.glob, OpTypePointer, p_shared, ScWorkgroup, c.t_uint);
    if (!lii_var) {
        uint32_t pt = nid(&c);
        lii_var = nid(&c);
        INS(&c.glob, OpTypePointer, pt, ScInput, c.t_uint);
        INS(&c.glob, OpVariable, pt, lii_var, ScInput);
        INS(&c.deco, OpDecorate, lii_var, DecBuiltIn, BiLocalInvocationIndex);
        gvars[n_gvars++] = lii_var;
    }
    if (!wg_var) {
        uint32_t pt = nid(&c);
        wg_var = nid(&c);
        INS(&c.glob, OpTypePointer, pt, ScInput, c.t_v3uint);
        INS(&c.glob, OpVariable, pt, wg_var, ScInput);
        INS(&c.deco, OpDecorate, wg_var, DecBuiltIn, BiWorkgroupId);
        gvars[n_gvars++] = wg_var;
    }
    uint32_t fake_wg = 0, p_priv_uint = 0;
    if (mesh) {   // the workgroup id main sees (see the top)
        uint32_t pt = nid(&c);
        fake_wg = nid(&c), p_priv_uint = nid(&c);
        INS(&c.glob, OpTypePointer, pt, ScPrivate, c.t_v3uint);
        INS(&c.glob, OpTypePointer, p_priv_uint, ScPrivate, c.t_uint);
        INS(&c.glob, OpVariable, pt, fake_wg, ScPrivate);
        gvars[n_gvars++] = fake_wg;
    }
    uint32_t task_buf = 0, mesh_buf = 0, list_buf = 0;
    if (!mesh || m->has_task) task_buf = word_buffer(&c, binding, mesh), gvars[n_gvars++] = task_buf;
    if (mesh) mesh_buf = word_buffer(&c, binding + 1, 0), gvars[n_gvars++] = mesh_buf;
    if (mesh && m->has_task) list_buf = word_buffer(&c, binding + 2, 1), gvars[n_gvars++] = list_buf;

    uint32_t threads = K(&c, m->threads), k0 = K(&c, 0), k1 = K(&c, 1);
    uint32_t entry = nid(&c);
    DO(&c, OpFunction, c.t_void, entry, 0, c.t_fnvoid);
    DO(&c, OpLabel, nid(&c));
    uint32_t counter = nid(&c);
    DO(&c, OpVariable, c.p_fn_uint, counter, ScFunction);
    uint32_t tid = E(&c, OpLoad, c.t_uint, lii_var);
    uint32_t g = E(&c, OpCompositeExtract, c.t_uint, E(&c, OpLoad, c.t_v3uint, wg_var), 0);
    uint32_t first = E(&c, OpULessThan, c.t_bool, tid, k1);   // the workgroup's first thread
#define SHARED(index) E(&c, OpAccessChain, p_shared, shared_var, index)
#define BARRIER() DO(&c, OpControlBarrier, K(&c, 2), K(&c, 2), K(&c, 0x108))   /* Workgroup, AcquireRelease | WorkgroupMemory */
    if (!mesh) {
        uint32_t pw = m->payload_bytes / 4;
        uint32_t at = mul(&c, g, K(&c, pw));
        uint32_t room = E(&c, OpULessThanEqual, c.t_bool, add(&c, at, K(&c, pw)), E(&c, OpArrayLength, c.t_uint, task_buf, 0));
        // the mesh workgroup count starts at 0: a task that culls everything never writes it
        uint32_t mg = if_begin(&c, first);
        DO(&c, OpStore, SHARED(K(&c, (m->pay + 4) / 4)), k0);
        if_end(&c, mg);
        BARRIER();
        E(&c, OpFunctionCall, c.t_void, main_fn);
        BARRIER();
        mg = if_begin(&c, room);
        Loop l = loop_begin(&c, counter, tid, K(&c, pw), threads);
        DO(&c, OpStore, buf_ptr(&c, task_buf, add(&c, at, l.i)), E(&c, OpLoad, c.t_uint, SHARED(add(&c, l.i, K(&c, m->pay / 4)))));
        loop_end(&c, &l);
        if_end(&c, mg);
    } else {
        uint32_t slot_words = meshfb_slot_words(m), count_word = K(&c, m->map / 4);
        uint32_t j = g, task_at = 0;
        if (m->has_task) {
            uint32_t e = buf_load(&c, list_buf, g);
            task_at = mul(&c, E(&c, OpBitwiseAnd, c.t_uint, e, K(&c, (1u << MESHFB_LIST_TASK_BITS) - 1)), K(&c, task_words));
            j = E(&c, OpShiftRightLogical, c.t_uint, e, K(&c, MESHFB_LIST_TASK_BITS));
        }
        DO(&c, OpStore, fake_wg, E(&c, OpCompositeConstruct, c.t_v3uint, j, k0, k0));
        uint32_t at = mul(&c, g, K(&c, slot_words));
        uint32_t room = E(&c, OpULessThanEqual, c.t_bool, add(&c, at, K(&c, slot_words)), E(&c, OpArrayLength, c.t_uint, mesh_buf, 0));
        // The whole stage buffer starts at 0: the vertex lines, the primitive count and the vertex indices. Workgroup
        // memory is undefined at the start, a mesh program need not write every vertex it has room for or every index
        // byte (nor its count when it emits nothing), and the slot below is copied whole and drawn: what an earlier
        // workgroup left there became vertices and primitives. The payload area after it is copied in next.
        Loop z = loop_begin(&c, counter, tid, K(&c, slot_words), threads);
        DO(&c, OpStore, SHARED(z.i), k0);
        loop_end(&c, &z);
        uint32_t mg;
        if (m->has_task) {
            uint32_t pw = m->payload_bytes / 4;
            if (pw > task_words) pw = task_words;
            Loop l = loop_begin(&c, counter, tid, K(&c, pw), threads);
            DO(&c, OpStore, SHARED(add(&c, l.i, K(&c, m->pay / 4))), buf_load(&c, task_buf, add(&c, task_at, l.i)));
            loop_end(&c, &l);
        }
        BARRIER();
        E(&c, OpFunctionCall, c.t_void, main_fn);
        BARRIER();
        uint32_t count = E(&c, OpLoad, c.t_uint, SHARED(count_word));
        mg = if_begin(&c, room);
        {   // a slot without primitives gets its count alone
            uint32_t some = E(&c, OpINotEqual, c.t_bool, count, k0), then = nid(&c), other = nid(&c), merge = nid(&c);
            DO(&c, OpSelectionMerge, merge, 0);
            DO(&c, OpBranchConditional, some, then, other);
            DO(&c, OpLabel, then);
            Loop l = loop_begin(&c, counter, tid, K(&c, slot_words), threads);
            DO(&c, OpStore, buf_ptr(&c, mesh_buf, add(&c, at, l.i)), E(&c, OpLoad, c.t_uint, SHARED(l.i)));
            loop_end(&c, &l);
            DO(&c, OpBranch, merge);
            DO(&c, OpLabel, other);
            uint32_t m2 = if_begin(&c, first);
            DO(&c, OpStore, buf_ptr(&c, mesh_buf, add(&c, at, count_word)), k0);
            if_end(&c, m2);
            DO(&c, OpBranch, merge);
            DO(&c, OpLabel, merge);
        }
        if_end(&c, mg);
    }
    wb_ins(&c.fn, OpReturn, NULL, 0);
    wb_ins(&c.fn, OpFunctionEnd, NULL, 0);

    // ---- pass 2: the module again
    uint32_t head[5] = {w[0], w[1], w[2], 0, 0};
    wb_put(&out, head, 5);
    for (size_t i = 0; i < n; i++) {
        const uint32_t *a = w + in[i].at + 1;
        uint32_t op = in[i].op, len = in[i].len;
        if (i == first_decl) wb_put(&out, c.deco.w, c.deco.n);
        if (i == first_fn) wb_put(&out, c.glob.w, c.glob.n);
        if (i >= e0 && i <= e1) continue;   // the old entry function
        if (op == OpCapability && a[0] == CapMeshShading) continue;
        if (op == OpExtension && len >= 6 && !memcmp(a, "SPV_EXT_mesh_shader", 20)) continue;
        if (op == OpExecutionMode && a[1] != ModeLocalSize) continue;
        if ((op == OpName || op == OpDecorate || op == OpDecorateId || op == OpDecorateString) && id_in(removed, n_removed, a[0])) continue;
        if ((op == OpTypePointer && id_in(removed, n_removed, a[0])) || (op == OpVariable && id_in(removed, n_removed, a[1]))) continue;
        if (op == OpEntryPoint) {   // every global variable is its interface (SPIR-V 1.4)
            uint32_t ep[4] = {(uint32_t)((n_gvars + 5) << 16) | OpEntryPoint, ModelGLCompute, entry, 0x6e69616d /* "main" */};
            uint32_t zero = 0;
            wb_put(&out, ep, 4), wb_put(&out, &zero, 1);
            wb_put(&out, gvars, (size_t)n_gvars);
            continue;
        }
        size_t at = out.n;
        wb_put(&out, w + in[i].at, len);
        if (out.oom) break;
        uint32_t *o = out.w + at + 1;
        if (op == OpExecutionMode) o[0] = entry;
        if (mesh && i > first_fn) {   // main and what it calls read the workgroup id from the private variable
            if ((op == OpAccessChain || op == OpInBoundsAccessChain) && a[2] == wg_var) {
                uint32_t pointee = 0;
                for (size_t t = first_decl; t < first_fn; t++)
                    if (in[t].op == OpTypePointer && w[in[t].at + 1] == a[0]) pointee = w[in[t].at + 3];
                if (pointee != c.t_uint || len != 5) FAIL("the workgroup id is read in a way the fallback does not know");
                o[0] = p_priv_uint, o[2] = fake_wg;
            } else if (op == OpLoad && a[2] == wg_var) {
                o[2] = fake_wg;
            } else if ((op == OpStore && a[0] == wg_var) || (op == OpCopyMemory && (a[0] == wg_var || a[1] == wg_var)) ||
                       ((op == OpPtrAccessChain || op == OpCopyObject) && a[2] == wg_var)) {
                FAIL("the workgroup id is read in a way the fallback does not know");
            } else if (op == OpFunctionCall || op == OpSelect || op == OpPhi) {
                for (uint32_t k = 2; k + 1 < len; k++)
                    if (a[k] == wg_var) FAIL("the workgroup id is passed on as a pointer");
            }
        }
    }
    wb_put(&out, c.fn.w, c.fn.n);
    if (out.oom || c.deco.oom || c.glob.oom || c.fn.oom) FAIL("out of memory");
    out.w[3] = c.bound;
    *out_words = out.n;
    free(in), free(removed), free(gvars), free(c.deco.w), free(c.glob.w), free(c.fn.w);
    return out.w;
bad:
    free(in), free(removed), free(gvars), free(c.deco.w), free(c.glob.w), free(c.fn.w), free(out.w);
    return NULL;
#undef FAIL
#undef SHARED
#undef BARRIER
}

uint32_t *meshfb_task_cs(const uint32_t *spirv, uint64_t words, const MeshFbInfo *m, uint32_t binding, uint64_t *out_words,
                         char *err, size_t cap) {
    if (m->stage != 6) return fail(err, cap, "not a task program"), NULL;
    return convert(spirv, words, m, binding, 0, out_words, err, cap);
}

uint32_t *meshfb_mesh_cs(const uint32_t *spirv, uint64_t words, const MeshFbInfo *m, uint32_t binding,
                         uint32_t task_payload_words, uint64_t *out_words, char *err, size_t cap) {
    if (m->stage != 7) return fail(err, cap, "not a mesh program"), NULL;
    if (m->has_task && !task_payload_words) return fail(err, cap, "a mesh program after a task stage without its payload size"), NULL;
    return convert(spirv, words, m, binding, task_payload_words, out_words, err, cap);
}

// ---- the vertex module

uint32_t *meshfb_vs(const MeshFbInfo *m, uint32_t out_mask, const uint8_t out_type[32], uint64_t *out_words) {
    Cx c = {.bound = 1};
    Wb out = {0};
    uint32_t iface[40], n_iface = 0;
    c.t_void = nid(&c), INS(&c.glob, OpTypeVoid, c.t_void);
    c.t_bool = nid(&c), INS(&c.glob, OpTypeBool, c.t_bool);
    c.t_uint = nid(&c), INS(&c.glob, OpTypeInt, c.t_uint, 32, 0);
    uint32_t t_int = nid(&c), t_float = nid(&c), t_v4[3], p_out[3], t_scalar[3];   // by GFX_TYPE_*: float, sint, uint
    INS(&c.glob, OpTypeInt, t_int, 32, 1);
    INS(&c.glob, OpTypeFloat, t_float, 32);
    t_scalar[0] = t_float, t_scalar[1] = t_int, t_scalar[2] = c.t_uint;
    for (int t = 0; t < 3; t++) {
        t_v4[t] = nid(&c), INS(&c.glob, OpTypeVector, t_v4[t], t_scalar[t], 4);
        p_out[t] = nid(&c), INS(&c.glob, OpTypePointer, p_out[t], ScOutput, t_v4[t]);
    }
    c.t_fnvoid = nid(&c), INS(&c.glob, OpTypeFunction, c.t_fnvoid, c.t_void);
    c.p_sb_uint = nid(&c), INS(&c.glob, OpTypePointer, c.p_sb_uint, ScStorageBuffer, c.t_uint);
    uint32_t p_in_int = nid(&c), p_out_float = nid(&c);
    INS(&c.glob, OpTypePointer, p_in_int, ScInput, t_int);
    INS(&c.glob, OpTypePointer, p_out_float, ScOutput, t_float);
    uint32_t vi_var = nid(&c), ii_var = nid(&c), pos_var = nid(&c), size_var = 0, buf, loc_var[32] = {0};
    INS(&c.glob, OpVariable, p_in_int, vi_var, ScInput);
    INS(&c.glob, OpVariable, p_in_int, ii_var, ScInput);
    INS(&c.glob, OpVariable, p_out[0], pos_var, ScOutput);
    INS(&c.deco, OpDecorate, vi_var, DecBuiltIn, BiVertexIndex);
    INS(&c.deco, OpDecorate, ii_var, DecBuiltIn, BiInstanceIndex);
    INS(&c.deco, OpDecorate, pos_var, DecBuiltIn, BiPosition);
    INS(&c.deco, OpDecorate, pos_var, DecInvariant);
    iface[n_iface++] = vi_var, iface[n_iface++] = ii_var, iface[n_iface++] = pos_var;
    if (m->ipp == 1) {   // a point list needs its size written
        size_var = nid(&c);
        INS(&c.glob, OpVariable, p_out_float, size_var, ScOutput);
        INS(&c.deco, OpDecorate, size_var, DecBuiltIn, BiPointSize);
        iface[n_iface++] = size_var;
    }
    for (int g = 0; g < 32; g++) {
        if (!(out_mask >> g & 1)) continue;
        int t = out_type[g] < 3 ? out_type[g] : 0;
        loc_var[g] = nid(&c);
        INS(&c.glob, OpVariable, p_out[t], loc_var[g], ScOutput);
        INS(&c.deco, OpDecorate, loc_var[g], DecLocation, (uint32_t)g);
        iface[n_iface++] = loc_var[g];
    }
    buf = word_buffer(&c, 1, 1);
    iface[n_iface++] = buf;
    uint32_t f0 = nid(&c), f1 = nid(&c), fout = nid(&c), null_v4[3], away = nid(&c);
    INS(&c.glob, OpConstant, t_float, f0, 0);
    INS(&c.glob, OpConstant, t_float, f1, 0x3f800000);
    INS(&c.glob, OpConstant, t_float, fout, 0xc1800000);   // -16: outside the clip volume, for a point of no primitive
    for (int t = 0; t < 3; t++) null_v4[t] = nid(&c), INS(&c.glob, OpConstantNull, t_v4[t], null_v4[t]);
    INS(&c.glob, OpConstantComposite, t_v4[0], away, fout, fout, f0, f1);

    uint32_t slot_words = meshfb_slot_words(m), k0 = K(&c, 0), entry = nid(&c);
    DO(&c, OpFunction, c.t_void, entry, 0, c.t_fnvoid);
    DO(&c, OpLabel, nid(&c));
    uint32_t vi = E(&c, OpBitcast, c.t_uint, E(&c, OpLoad, t_int, vi_var));
    uint32_t slot = E(&c, OpBitcast, c.t_uint, E(&c, OpLoad, t_int, ii_var));
    uint32_t prim = m->ipp == 1 ? vi : E(&c, OpUDiv, c.t_uint, vi, K(&c, m->ipp));
    uint32_t corner = m->ipp == 1 ? k0 : E(&c, OpISub, c.t_uint, vi, mul(&c, prim, K(&c, m->ipp)));
    uint32_t base = mul(&c, slot, K(&c, slot_words));
    // a slot past the buffer (more workgroups than it has room for) draws nothing
    uint32_t valid = E(&c, OpULessThanEqual, c.t_bool, add(&c, base, K(&c, slot_words)), E(&c, OpArrayLength, c.t_uint, buf, 0));
    uint32_t cw = buf_load(&c, buf, E(&c, OpSelect, c.t_uint, valid, add(&c, base, K(&c, m->map / 4)), k0));
    uint32_t count = E(&c, OpSelect, c.t_uint, valid, umin(&c, cw, K(&c, m->maxp)), k0);
    uint32_t live = E(&c, OpULessThan, c.t_bool, prim, count), then = nid(&c), other = nid(&c), merge = nid(&c);
    DO(&c, OpSelectionMerge, merge, 0);
    DO(&c, OpBranchConditional, live, then, other);
    DO(&c, OpLabel, then);
    {
        // byte map + 4 + ipp * prim + corner of the slot is the vertex
        uint32_t b = add(&c, add(&c, mul(&c, prim, K(&c, m->ipp)), corner), K(&c, m->map + 4));
        uint32_t word = buf_load(&c, buf, add(&c, base, E(&c, OpShiftRightLogical, c.t_uint, b, K(&c, 2))));
        uint32_t shift = mul(&c, E(&c, OpBitwiseAnd, c.t_uint, b, K(&c, 3)), K(&c, 8));
        uint32_t v = umin(&c, E(&c, OpBitwiseAnd, c.t_uint, E(&c, OpShiftRightLogical, c.t_uint, word, shift), K(&c, 0xff)), K(&c, m->maxv - 1));
        // word ((v >> 5) * lines + line) * 32 + (v & 31) of the vertex area
        uint32_t at = add(&c, add(&c, mul(&c, E(&c, OpShiftRightLogical, c.t_uint, v, K(&c, 5)), K(&c, m->nlines * 32)),
                                  E(&c, OpBitwiseAnd, c.t_uint, v, K(&c, 31))), add(&c, base, K(&c, m->attr / 4)));
        uint32_t pos[4] = {f0, f0, f0, f1}, size = f1, gen[32][4];
        memset(gen, 0, sizeof gen);
        for (uint32_t l = 0; l < m->nlines; l++) {
            uint32_t a = m->lines[l];
            int g = a >= 0x80 ? (int)(a - 0x80) / 16 : -1;
            if (a < 0x6c || (g >= 0 && !loc_var[g])) continue;   // layer and viewport index are not carried; an output nothing reads
            uint32_t word_l = buf_load(&c, buf, l ? add(&c, at, K(&c, l * 32)) : at);
            if (g >= 0) gen[g][(a - 0x80) % 16 / 4] = word_l;
            else if (a == 0x6c) size = E(&c, OpBitcast, t_float, word_l);
            else pos[(a - 0x70) / 4] = E(&c, OpBitcast, t_float, word_l);
        }
        DO(&c, OpStore, pos_var, E(&c, OpCompositeConstruct, t_v4[0], pos[0], pos[1], pos[2], pos[3]));
        if (size_var) DO(&c, OpStore, size_var, size);
        for (int g = 0; g < 32; g++) {
            if (!loc_var[g]) continue;
            int t = out_type[g] < 3 ? out_type[g] : 0;
            // A component outside the program's output map reads as Maxwell gives an unwritten one: (0, 0, 0, 1), the w
            // as the bits of 1.0f whatever the type, as vk_pipeline.c spirv_add_outputs pads a missing output.
            uint32_t comp[4];
            for (int k = 0; k < 4; k++) {
                uint32_t word_k = gen[g][k] ? gen[g][k] : k == 3 ? K(&c, 0x3f800000) : k0;
                comp[k] = t == 2 ? word_k : E(&c, OpBitcast, t_scalar[t], word_k);
            }
            DO(&c, OpStore, loc_var[g], E(&c, OpCompositeConstruct, t_v4[t], comp[0], comp[1], comp[2], comp[3]));
        }
    }
    DO(&c, OpBranch, merge);
    DO(&c, OpLabel, other);
    DO(&c, OpStore, pos_var, away);
    if (size_var) DO(&c, OpStore, size_var, f1);
    for (int g = 0; g < 32; g++)
        if (loc_var[g]) DO(&c, OpStore, loc_var[g], null_v4[out_type[g] < 3 ? out_type[g] : 0]);
    DO(&c, OpBranch, merge);
    DO(&c, OpLabel, merge);
    wb_ins(&c.fn, OpReturn, NULL, 0);
    wb_ins(&c.fn, OpFunctionEnd, NULL, 0);

    uint32_t head[5] = {0x07230203, 0x00010400, 0, c.bound, 0};
    wb_put(&out, head, 5);
    INS(&out, OpCapability, CapShader);
    INS(&out, OpMemoryModel, 0, 1);   // Logical, GLSL450
    uint32_t ep[5] = {(n_iface + 5) << 16 | OpEntryPoint, ModelVertex, entry, 0x6e69616d /* "main" */, 0};
    wb_put(&out, ep, 5);
    wb_put(&out, iface, n_iface);
    wb_put(&out, c.deco.w, c.deco.n);
    wb_put(&out, c.glob.w, c.glob.n);
    wb_put(&out, c.fn.w, c.fn.n);
    int oom = out.oom || c.deco.oom || c.glob.oom || c.fn.oom;
    free(c.deco.w), free(c.glob.w), free(c.fn.w);
    if (oom) return free(out.w), NULL;
    *out_words = out.n;
    return out.w;
}

// ---- reflection text

char *meshfb_res_json(const char *res, uint32_t binding, const int *cb_slots, int n) {
    size_t len = strlen(res);
    char *out = malloc(len + (size_t)n * 96 + 1), *o = out;
    if (!out) return NULL;
    const char *sb = strstr(res, "\"sbufs\""), *open = sb ? strchr(sb, '[') : NULL;
    if (!open) return free(out), NULL;
    memcpy(o, res, (size_t)(open + 1 - res)), o += open + 1 - res;
    const char *rest = open + 1;
    while (*rest == ' ' || *rest == '\n' || *rest == '\r' || *rest == '\t') rest++;
    for (int i = 0; i < n; i++)
        o += sprintf(o, "{\"binding\": %u, \"slot\": -1, \"cb_slot\": %d, \"cb_offset\": 0, \"flags\": %d}%s", binding + (uint32_t)i,
                     cb_slots[i], cb_slots[i] == MESHFB_CB_LIST ? 0 : 1, i + 1 < n || *rest != ']' ? "," : "");
    strcpy(o, open + 1);
    char *st = strstr(out, "\"stage\"");
    st = st ? strchr(st + 7, '"') : NULL;
    if (!st || (st[1] != 'm' && st[1] != 't') || st[2] != 's') return free(out), NULL;
    st[1] = 'c';
    return out;
}
