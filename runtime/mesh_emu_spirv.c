// Mesh shader emulation: SPIR-V rewriting (mesh_emu_spirv.h). The input is what tools/xlate/MeshSpirv.cs makes: the
// translator's compute main, called from an entry function that copies its shared-memory stage buffer into the mesh
// outputs after SetMeshOutputsEXT (mesh), or into the task payload before EmitMeshTasksEXT (task).
//
// Mesh -> GLCompute: the output arrays and the task payload move to Workgroup memory; SetMeshOutputsEXT is dropped (its
// counts are kept); at the entry function's end thread 0 reserves vertex slots and indices in the arena with atomics,
// and every thread copies vertices and triangles out (indices made absolute vertex slots, relative to the vertex area).
// With a task stage mesh workgroup (x, y) is mesh group x of task record (task base + y): a prologue returns when x is
// past the record's count, loads the payload, and the module's WorkgroupId reads become (x, 0, 0).
// Task -> GLCompute: the payload moves to Workgroup memory; EmitMeshTasksEXT becomes a store of the payload and the
// mesh group count into the task record, and workgroup 0 writes the mesh dispatch {max_mesh, task workgroups, 1}
// into the draw record.
#include "mesh_emu_spirv.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

enum {
    OpSourceContinued = 2, OpSource = 3, OpSourceExtension = 4, OpName = 5, OpMemberName = 6, OpString = 7, OpLine = 8,
    OpExtension = 10, OpExtInstImport = 11, OpExtInst = 12, OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16,
    OpCapability = 17, OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23,
    OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32, OpTypeFunction = 33,
    OpConstant = 43, OpConstantComposite = 44, OpFunction = 54, OpFunctionEnd = 56, OpVariable = 59, OpLoad = 61,
    OpStore = 62, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71, OpMemberDecorate = 72,
    OpCompositeConstruct = 80, OpCompositeExtract = 81, OpUConvert = 113, OpConvertUToPtr = 120, OpBitcast = 124,
    OpIAdd = 128, OpIMul = 132, OpLogicalAnd = 167, OpSelect = 169, OpIEqual = 170, OpINotEqual = 171,
    OpULessThan = 176, OpULessThanEqual = 178, OpControlBarrier = 224, OpAtomicIAdd = 234, OpAtomicUMax = 239,
    OpSelectionMerge = 247, OpLabel = 248, OpBranch = 249, OpBranchConditional = 250, OpReturn = 253,
    OpNoLine = 317, OpModuleProcessed = 330, OpExecutionModeId = 331, OpConstantNull = 46, OpCopyObject = 83,
    OpEmitMeshTasksEXT = 5294, OpSetMeshOutputsEXT = 5295,
};
enum { ScInput = 1, ScOutput = 3, ScWorkgroup = 4, ScPrivate = 6, ScFunction = 7, ScPushConstant = 9,
       ScPhysicalStorageBuffer = 5349, ScTaskPayload = 5402 };
enum { DecBlock = 2, DecArrayStride = 6, DecBuiltIn = 11, DecLocation = 30, DecOffset = 35, DecPerPrimitive = 5271 };
enum { BiPosition = 0, BiPointSize = 1, BiPrimPoint = 5294, BiVertexIndex = 42, BiNumWorkgroups = 24, BiWorkgroupId = 26, BiLocalInvocationIndex = 29,
       BiPrimTri = 5296 };
enum { CapShader = 1, CapInt64 = 11, CapMeshShading = 5283, CapPSBAddresses = 5347 };
enum { ModelVertex = 0, ModelGLCompute = 5, ModelTask = 5364, ModelMesh = 5365 };
enum { ModeLocalSize = 17, ModeOutputVertices = 26, ModeOutputPoints = 27, ModeOutputLines = 5269,
       ModeOutputPrimitives = 5270, ModeOutputTriangles = 5298 };
enum { GlslUMin = 38 };
enum { MaxIns = 4096 };

typedef struct { uint32_t *w; size_t n, cap; int bad; } Buf;

static void put(Buf *b, uint32_t x) {
    if (b->n == b->cap) {
        size_t c = b->cap ? b->cap * 2 : 1024;
        uint32_t *w = realloc(b->w, c * sizeof *w);
        if (!w) { b->bad = 1; return; }
        b->w = w, b->cap = c;
    }
    b->w[b->n++] = x;
}
static void putn(Buf *b, const uint32_t *w, size_t n) { for (size_t i = 0; i < n; i++) put(b, w[i]); }
static void op(Buf *b, uint32_t opc, int n, ...) {
    va_list ap;
    va_start(ap, n);
    put(b, (uint32_t)(n + 1) << 16 | opc);
    for (int i = 0; i < n; i++) put(b, va_arg(ap, uint32_t));
    va_end(ap);
}
static void op_str(Buf *b, uint32_t opc, const uint32_t *pre, int npre, const char *s, const uint32_t *post, int npost) {
    size_t sl = strlen(s) + 1, sw = (sl + 3) / 4;
    put(b, (uint32_t)(1 + npre + sw + npost) << 16 | opc);
    for (int i = 0; i < npre; i++) put(b, pre[i]);
    for (size_t i = 0; i < sw; i++) {
        uint32_t x = 0;
        for (size_t k = 0; k < 4 && i * 4 + k < sl; k++) x |= (uint32_t)(uint8_t)s[i * 4 + k] << (8 * k);
        put(b, x);
    }
    for (int i = 0; i < npost; i++) put(b, post[i]);
}

// ---- the module
typedef struct {
    const uint32_t *w;
    size_t n;
    uint32_t bound;
    uint32_t t_void, t_bool, t_u32, t_u64, t_f32, t_v3u, t_v4u, t_v4f;
    uint32_t glsl, ep_fn, model, wgid, lii, nwg;
    int has_int64, has_psb, has_psb_ext;
    struct { uint32_t id, sc, to; } ptr[4096];
    int nptr;
    struct { uint32_t id, type, sc; } var[2048];
    int nvar;
    struct { uint32_t id, type, val; } cst[8192];
    int ncst;
} Mod;

static uint32_t find_ptr(const Mod *m, uint32_t sc, uint32_t to) {
    for (int i = 0; i < m->nptr; i++)
        if (m->ptr[i].sc == sc && m->ptr[i].to == to) return m->ptr[i].id;
    return 0;
}
static uint32_t ptr_to(const Mod *m, uint32_t id) {
    for (int i = 0; i < m->nptr; i++)
        if (m->ptr[i].id == id) return m->ptr[i].to;
    return 0;
}
static const uint32_t *find_type(const Mod *m, uint32_t id) {
    for (size_t i = 5; i < m->n;) {
        uint32_t o = m->w[i] & 0xffff, len = m->w[i] >> 16;
        if (!len || o == OpFunction) break;
        if ((o == OpTypeArray || o == OpTypeRuntimeArray || o == OpTypeVector || o == OpTypeStruct) && m->w[i + 1] == id)
            return m->w + i;
        i += len;
    }
    return NULL;
}
static int const_u32(const Mod *m, uint32_t id, uint32_t *v) {
    for (int i = 0; i < m->ncst; i++)
        if (m->cst[i].id == id && m->cst[i].type == m->t_u32) return *v = m->cst[i].val, 1;
    return 0;
}
static int is_debug(uint32_t o) {
    return o == OpSourceContinued || o == OpSource || o == OpSourceExtension || o == OpName || o == OpMemberName ||
           o == OpString || o == OpModuleProcessed;
}
static int is_annot(uint32_t o) { return o == OpDecorate || o == OpMemberDecorate || o == 73 || o == 74 || o == 75 || o == 332 || o == 5632 || o == 5633; }

static int parse(Mod *m, const uint32_t *w, size_t n, const char **why) {
    m->w = w, m->n = n;
    if (n < 5 || w[0] != 0x07230203) return *why = "not SPIR-V", 0;
    m->bound = w[3];
    for (size_t i = 5; i < n;) {
        uint32_t o = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return *why = "malformed", 0;
        if (len > MaxIns) return *why = "an instruction too long", 0;
        const uint32_t *a = w + i + 1;
        if (o == OpFunction) break;
        switch (o) {
        case OpCapability:
            if (a[0] == CapInt64) m->has_int64 = 1;
            if (a[0] == CapPSBAddresses) m->has_psb = 1;
            break;
        case OpExtension: if (!strcmp((const char *)a, "SPV_KHR_physical_storage_buffer")) m->has_psb_ext = 1; break;
        case OpExtInstImport: if (!strcmp((const char *)(a + 1), "GLSL.std.450")) m->glsl = a[0]; break;
        case OpEntryPoint:
            if (m->ep_fn) return *why = "several entry points", 0;
            m->model = a[0], m->ep_fn = a[1];
            break;
        case OpTypeVoid: if (!m->t_void) m->t_void = a[0]; break;
        case OpTypeBool: if (!m->t_bool) m->t_bool = a[0]; break;
        case OpTypeInt:
            if (a[1] == 32 && !a[2] && !m->t_u32) m->t_u32 = a[0];
            if (a[1] == 64 && !a[2] && !m->t_u64) m->t_u64 = a[0];
            break;
        case OpTypeFloat: if (a[1] == 32 && !m->t_f32) m->t_f32 = a[0]; break;
        case OpTypeVector:
            if (a[1] == m->t_u32 && a[2] == 3 && !m->t_v3u) m->t_v3u = a[0];
            if (a[1] == m->t_u32 && a[2] == 4 && !m->t_v4u) m->t_v4u = a[0];
            if (a[1] == m->t_f32 && m->t_f32 && a[2] == 4 && !m->t_v4f) m->t_v4f = a[0];
            break;
        case OpTypePointer:
            if (m->nptr == 4096) return *why = "too many pointer types", 0;
            m->ptr[m->nptr].id = a[0], m->ptr[m->nptr].sc = a[1], m->ptr[m->nptr++].to = a[2];
            break;
        case OpVariable:
            if (m->nvar == 2048) return *why = "too many variables", 0;
            m->var[m->nvar].id = a[1], m->var[m->nvar].type = a[0], m->var[m->nvar++].sc = a[2];
            break;
        case OpConstant:
            if (len == 4 && m->ncst < 8192) m->cst[m->ncst].id = a[1], m->cst[m->ncst].type = a[0], m->cst[m->ncst++].val = a[2];
            break;
        case OpDecorate:
            if (len >= 4 && a[1] == DecBuiltIn) {
                if (a[2] == BiWorkgroupId) m->wgid = a[0];
                if (a[2] == BiLocalInvocationIndex) m->lii = a[0];
                if (a[2] == BiNumWorkgroups) m->nwg = a[0];
            }
            break;
        }
        i += len;
    }
    if (!m->ep_fn || (m->model != ModelMesh && m->model != ModelTask)) return *why = "not a task or mesh module", 0;
    if (!m->t_u32 || !m->t_void || !m->glsl || !m->lii || !m->wgid) return *why = "missing basic types or built-ins", 0;
    return 1;
}

// ---- emission over new ids
typedef struct {
    Mod *m;
    Buf *types, *annot, *code;
    uint32_t p_u32_psb, p_v4u_psb, p_words, p_slots, p_pc_u64, p_pc_u32, pc_var, c64_16;
    struct { uint32_t id, val; } cc[256];
    int ncc;
} Em;

static uint32_t nid(Mod *m) { return m->bound++; }
static uint32_t cu(Em *e, uint32_t v) {
    for (int i = 0; i < e->m->ncst; i++)
        if (e->m->cst[i].type == e->m->t_u32 && e->m->cst[i].val == v) return e->m->cst[i].id;
    for (int i = 0; i < e->ncc; i++)
        if (e->cc[i].val == v) return e->cc[i].id;
    uint32_t id = nid(e->m);
    op(e->types, OpConstant, 3, e->m->t_u32, id, v);
    if (e->ncc < 256) e->cc[e->ncc].id = id, e->cc[e->ncc++].val = v;
    return id;
}
static uint32_t E1(Em *e, uint32_t o, uint32_t t, uint32_t a) { uint32_t r = nid(e->m); op(e->code, o, 3, t, r, a); return r; }
static uint32_t E2(Em *e, uint32_t o, uint32_t t, uint32_t a, uint32_t b) { uint32_t r = nid(e->m); op(e->code, o, 4, t, r, a, b); return r; }
static uint32_t E3(Em *e, uint32_t o, uint32_t t, uint32_t a, uint32_t b, uint32_t c) { uint32_t r = nid(e->m); op(e->code, o, 5, t, r, a, b, c); return r; }
static uint32_t add(Em *e, uint32_t a, uint32_t b) { return E2(e, OpIAdd, e->m->t_u32, a, b); }
static uint32_t mul(Em *e, uint32_t a, uint32_t b) { return E2(e, OpIMul, e->m->t_u32, a, b); }
static uint32_t ult(Em *e, uint32_t a, uint32_t b) { return E2(e, OpULessThan, e->m->t_bool, a, b); }
static uint32_t ld(Em *e, uint32_t t, uint32_t p) { return E1(e, OpLoad, t, p); }
static uint32_t pc_u64(Em *e) { return ld(e, e->m->t_u64, E2(e, OpAccessChain, e->p_pc_u64, e->pc_var, cu(e, 0))); }
static uint32_t pc_u32(Em *e, uint32_t k) { return ld(e, e->m->t_u32, E2(e, OpAccessChain, e->p_pc_u32, e->pc_var, cu(e, k))); }
static uint32_t words_base(Em *e) { return E1(e, OpConvertUToPtr, e->p_words, pc_u64(e)); }
static uint32_t word_ptr(Em *e, uint32_t base, uint32_t idx) { return E3(e, OpAccessChain, e->p_u32_psb, base, cu(e, 0), idx); }
static uint32_t word_load(Em *e, uint32_t base, uint32_t idx) {
    uint32_t p = word_ptr(e, base, idx), r = nid(e->m);
    op(e->code, OpLoad, 5, e->m->t_u32, r, p, 2u, 4u);   // Aligned 4
    return r;
}
static void word_store(Em *e, uint32_t base, uint32_t idx, uint32_t v) { op(e->code, OpStore, 4, word_ptr(e, base, idx), v, 2u, 4u); }
static uint32_t atomic(Em *e, uint32_t o, uint32_t base, uint32_t idx, uint32_t v) {
    uint32_t p = word_ptr(e, base, idx), r = nid(e->m);
    op(e->code, o, 6, e->m->t_u32, r, p, cu(e, 1), cu(e, 0), v);   // Device scope, relaxed
    return r;
}
static void barrier(Em *e) { op(e->code, OpControlBarrier, 3, cu(e, 2), cu(e, 2), cu(e, 264)); }   // Workgroup, AcqRel|WorkgroupMemory
static void if_begin(Em *e, uint32_t cond, uint32_t *merge) {
    uint32_t t = nid(e->m);
    *merge = nid(e->m);
    op(e->code, OpSelectionMerge, 2, *merge, 0u);
    op(e->code, OpBranchConditional, 3, cond, t, *merge);
    op(e->code, OpLabel, 1, t);
}
static void if_end(Em *e, uint32_t merge) {
    op(e->code, OpBranch, 1, merge);
    op(e->code, OpLabel, 1, merge);
}

typedef struct { uint32_t var, len, role, loc, slot; } OutVar;   // role 0 position, 1 generic, 2 indices, 3 point size

// The mesh epilogue (replaces the entry function's return)
static void mesh_epilogue(Em *e, const MeshEmuInfo *info, const OutVar *ov, int nov, uint32_t vcount, uint32_t pcount,
                          uint32_t sh_vb, uint32_t sh_ib, uint32_t p_wg_v4f, uint32_t p_wg_v3u, uint32_t p_wg_f32,
                          uint32_t p_wg_u32) {
    Mod *m = e->m;
    uint32_t S = info->slots, T = info->threads, K = info->prim;
    barrier(e);
    uint32_t lid = ld(e, m->t_u32, m->lii), base = words_base(e);
    uint32_t isz = mul(e, pcount, cu(e, K)), vsz = mul(e, vcount, cu(e, S));
    // header word 6: mesh workgroups that reached the output copy; word 7: the primitives they declared (diagnosis)
    {
        uint32_t l0 = E2(e, OpIEqual, m->t_bool, lid, cu(e, 0));
        atomic(e, OpAtomicIAdd, base, cu(e, 6), E3(e, OpSelect, m->t_u32, l0, cu(e, 1), cu(e, 0)));
        atomic(e, OpAtomicIAdd, base, cu(e, 7), E3(e, OpSelect, m->t_u32, l0, pcount, cu(e, 0)));
    }
    uint32_t mz;
    if_begin(e, E2(e, OpIEqual, m->t_bool, lid, cu(e, 0)), &mz);
    {
        // indices from the draw's own range (reserved by the allocation shader), vertex slots from the arena's
        uint32_t drw = pc_u32(e, 1), vcap = pc_u32(e, 5);
        uint32_t icap = word_load(e, base, add(e, drw, cu(e, MESH_EMU_R_ICAP)));
        uint32_t ib = atomic(e, OpAtomicIAdd, base, add(e, drw, cu(e, MESH_EMU_R_ICUR)), isz);
        uint32_t vb = atomic(e, OpAtomicIAdd, base, cu(e, 0), vsz);
        uint32_t iok = E2(e, OpULessThanEqual, m->t_bool, add(e, ib, isz), icap);
        ib = add(e, word_load(e, base, add(e, drw, cu(e, 2))), ib);   // relative to the area, as firstIndex
        uint32_t vok = E2(e, OpULessThanEqual, m->t_bool, add(e, vb, vsz), vcap);
        uint32_t mi;
        if_begin(e, iok, &mi);
        atomic(e, OpAtomicIAdd, base, drw, isz);   // the draw record's index count
        if_end(e, mi);
        uint32_t all = E2(e, OpLogicalAnd, m->t_bool, iok, vok);
        atomic(e, OpAtomicIAdd, base, cu(e, 3), E3(e, OpSelect, m->t_u32, all, cu(e, 0), cu(e, 1)));
        op(e->code, OpStore, 2, sh_vb, E3(e, OpSelect, m->t_u32, vok, vb, cu(e, 0xffffffffu)));
        op(e->code, OpStore, 2, sh_ib, E3(e, OpSelect, m->t_u32, iok, ib, cu(e, 0xffffffffu)));
    }
    if_end(e, mz);
    barrier(e);
    uint32_t VB = ld(e, m->t_u32, sh_vb), IB = ld(e, m->t_u32, sh_ib);
    uint32_t m3;
    if_begin(e, E2(e, OpINotEqual, m->t_bool, IB, cu(e, 0xffffffffu)), &m3);
    {
        uint32_t vbok = E2(e, OpINotEqual, m->t_bool, VB, cu(e, 0xffffffffu));
        uint32_t m4;
        if_begin(e, vbok, &m4);
        {
            uint32_t vw = pc_u32(e, 4);
            uint32_t a64 = E2(e, OpIAdd, m->t_u64, pc_u64(e), E2(e, OpIMul, m->t_u64, E1(e, OpUConvert, m->t_u64, vw), e->c64_16));
            uint32_t sbase = E1(e, OpConvertUToPtr, e->p_slots, a64);
            for (uint32_t r = 0; r * T < info->max_v; r++) {
                uint32_t v = add(e, lid, cu(e, r * T)), mv;
                if_begin(e, ult(e, v, vcount), &mv);
                uint32_t s0 = add(e, VB, mul(e, v, cu(e, S)));
                for (int k = 0; k < nov; k++) {
                    if (ov[k].role == 2) continue;
                    uint32_t bits;
                    if (ov[k].role == 3) {   // the point size, in x
                        uint32_t ps = ld(e, m->t_f32, E2(e, OpAccessChain, p_wg_f32, ov[k].var, v));
                        uint32_t pb = E1(e, OpBitcast, m->t_u32, ps);
                        bits = nid(m);
                        op(e->code, OpCompositeConstruct, 6, m->t_v4u, bits, pb, cu(e, 0), cu(e, 0), cu(e, 0));
                    } else {
                        uint32_t src = ld(e, m->t_v4f, E2(e, OpAccessChain, p_wg_v4f, ov[k].var, v));
                        bits = E1(e, OpBitcast, m->t_v4u, src);
                    }
                    uint32_t dst = E3(e, OpAccessChain, e->p_v4u_psb, sbase, cu(e, 0), add(e, s0, cu(e, ov[k].slot)));
                    op(e->code, OpStore, 4, dst, bits, 2u, 16u);
                }
                if_end(e, mv);
            }
        }
        if_end(e, m4);
        uint32_t iw = pc_u32(e, 2), idxv = 0;
        for (int k = 0; k < nov; k++)
            if (ov[k].role == 2) idxv = ov[k].var;
        for (uint32_t r = 0; r * T < info->max_p; r++) {
            uint32_t p = add(e, lid, cu(e, r * T)), mp;
            if_begin(e, ult(e, p, pcount), &mp);
            uint32_t tri = K == 3 ? ld(e, m->t_v3u, E2(e, OpAccessChain, p_wg_v3u, idxv, p))
                                  : ld(e, m->t_u32, E2(e, OpAccessChain, p_wg_u32, idxv, p));
            uint32_t at = add(e, add(e, iw, IB), mul(e, p, cu(e, K)));
            for (uint32_t c = 0; c < K; c++) {
                uint32_t li = K == 3 ? E2(e, OpCompositeExtract, m->t_u32, tri, c) : tri;
                uint32_t val = E3(e, OpSelect, m->t_u32, vbok, add(e, VB, mul(e, li, cu(e, S))), cu(e, 0));
                word_store(e, base, add(e, at, cu(e, c)), val);
            }
            if_end(e, mp);
        }
    }
    if_end(e, m3);
    op(e->code, OpReturn, 0);
}

// The task epilogue (replaces EmitMeshTasksEXT x y z)
static void task_epilogue(Em *e, const MeshEmuInfo *info, uint32_t payload, uint32_t p_wg_u32, uint32_t nwg,
                          uint32_t ex, uint32_t ey, uint32_t ez) {
    Mod *m = e->m;
    uint32_t T = info->threads;
    uint32_t cnt = mul(e, ex, mul(e, ey, ez));
    uint32_t nc = E3(e, OpSelect, m->t_u32, ult(e, cnt, cu(e, info->max_mesh)), cnt, cu(e, info->max_mesh));
    barrier(e);
    uint32_t lid = ld(e, m->t_u32, m->lii), wg = ld(e, m->t_v3u, m->wgid), nw = ld(e, m->t_v3u, nwg);
    uint32_t wx = E2(e, OpCompositeExtract, m->t_u32, wg, 0u), wy = E2(e, OpCompositeExtract, m->t_u32, wg, 1u),
             wz = E2(e, OpCompositeExtract, m->t_u32, wg, 2u);
    uint32_t nx = E2(e, OpCompositeExtract, m->t_u32, nw, 0u), ny = E2(e, OpCompositeExtract, m->t_u32, nw, 1u),
             nz = E2(e, OpCompositeExtract, m->t_u32, nw, 2u);
    uint32_t nxy = mul(e, nx, ny);
    uint32_t t = add(e, wx, add(e, mul(e, wy, nx), mul(e, wz, nxy)));
    uint32_t base = words_base(e), drw = pc_u32(e, 1);
    uint32_t tb = word_load(e, base, add(e, drw, cu(e, 8)));
    uint32_t trecs = pc_u32(e, 6), slot = add(e, tb, t), m1;
    uint32_t mz;
    if_begin(e, E2(e, OpIEqual, m->t_bool, lid, cu(e, 0)), &mz);
    atomic(e, OpAtomicUMax, base, cu(e, 2), add(e, slot, cu(e, 1)));   // records asked for, past the capacity too
    if_end(e, mz);
    if_begin(e, ult(e, slot, trecs), &m1);
    {
        uint32_t recw = add(e, cu(e, MESH_EMU_TOFF / 4), mul(e, slot, cu(e, MESH_EMU_TWORDS)));
        for (uint32_t r = 0; r * T < info->payload_words; r++) {   // the last round's threads past the payload idle
            uint32_t k = add(e, lid, cu(e, r * T)), mk = 0;
            if ((r + 1) * T > info->payload_words) if_begin(e, ult(e, k, cu(e, info->payload_words)), &mk);
            uint32_t v = ld(e, m->t_u32, E2(e, OpAccessChain, p_wg_u32, payload, k));
            word_store(e, base, add(e, add(e, recw, cu(e, 1)), k), v);
            if (mk) if_end(e, mk);
        }
        uint32_t m2;
        if_begin(e, E2(e, OpIEqual, m->t_bool, lid, cu(e, 0)), &m2);
        word_store(e, base, recw, nc);
        atomic(e, OpAtomicIAdd, base, add(e, drw, cu(e, MESH_EMU_R_GROUPS)), nc);   // for the index range
        if_end(e, m2);
    }
    if_end(e, m1);
    uint32_t first = E2(e, OpLogicalAnd, m->t_bool, E2(e, OpIEqual, m->t_bool, lid, cu(e, 0)),
                        E2(e, OpIEqual, m->t_bool, t, cu(e, 0)));
    uint32_t m3;
    if_begin(e, first, &m3);
    {
        uint32_t total = mul(e, nxy, nz);
        // task records past the area are not written: their mesh groups are not dispatched
        uint32_t room = E3(e, OpSelect, m->t_u32, ult(e, tb, trecs), E2(e, 130 /* OpISub */, m->t_u32, trecs, tb), cu(e, 0));
        uint32_t ty = E3(e, OpSelect, m->t_u32, ult(e, total, room), total, room);
        word_store(e, base, add(e, drw, cu(e, 5)), cu(e, info->max_mesh));
        word_store(e, base, add(e, drw, cu(e, 6)), ty);
        word_store(e, base, add(e, drw, cu(e, 7)), cu(e, 1));
    }
    if_end(e, m3);
    op(e->code, OpReturn, 0);
}

uint32_t *mesh_emu_compute(const uint32_t *w, size_t n, MeshEmuInfo *info, size_t *out_n, const char **why) {
    static const char *dummy;
    if (!why) why = &dummy;
    *why = NULL;
    Mod *m = calloc(1, sizeof *m);
    uint32_t *ins = malloc(MaxIns * 4), *ret = NULL;
    Buf caps = {0}, exts = {0}, imps = {0}, modes = {0}, debug = {0}, annot = {0}, types = {0}, ntypes = {0}, nannot = {0},
        code = {0}, out = {0};
    if (!m || !ins) { *why = "out of memory"; goto done; }
    if (!parse(m, w, n, why)) goto done;
    memset(info, 0, sizeof *info);
    int mesh = m->model == ModelMesh;
    info->task = !mesh;
    for (size_t i = 5; i < n;) {
        uint32_t o = w[i] & 0xffff, len = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        if (o == OpFunction) break;
        if (o == OpExecutionMode && a[0] == m->ep_fn) {
            if (a[1] == ModeLocalSize) info->threads = a[2] * a[3] * a[4];
            if (a[1] == ModeOutputVertices) info->max_v = a[2];
            if (a[1] == ModeOutputPrimitives) info->max_p = a[2];
            if (a[1] == ModeOutputTriangles) info->prim = 3;
            if (a[1] == ModeOutputLines) info->prim = 2;
            if (a[1] == ModeOutputPoints) info->prim = 1;
        }
        i += len;
    }
    if (!info->threads) { *why = "no LocalSize"; goto done; }
    if (mesh && info->prim != 3 && info->prim != 1) { *why = "only triangle and point meshes are emulated"; goto done; }
    if (!m->t_f32 || (mesh && !m->t_v4f)) { *why = "no float types"; goto done; }

    // output variables and the payload
    OutVar ov[40];
    int nov = 0;
    uint32_t payload = 0, payload_len = 0;
    for (int i = 0; i < m->nvar; i++) {
        uint32_t sc = m->var[i].sc;
        if (sc != ScOutput && sc != ScTaskPayload) continue;
        const uint32_t *arr = find_type(m, ptr_to(m, m->var[i].type));
        uint32_t len = 0;
        if (!arr || (arr[0] & 0xffff) != OpTypeArray || !const_u32(m, arr[3], &len)) { *why = "an output or payload is not a sized array"; goto done; }
        if (sc == ScTaskPayload) {
            if (arr[2] != m->t_u32) { *why = "a payload that is not uint[]"; goto done; }
            payload = m->var[i].id, payload_len = len;
            continue;
        }
        if (nov == 40) { *why = "too many outputs"; goto done; }
        ov[nov] = (OutVar){m->var[i].id, len, 9, 0, 0};
        for (size_t k = 5; k < n;) {
            uint32_t o = w[k] & 0xffff, l2 = w[k] >> 16;
            const uint32_t *a = w + k + 1;
            if (o == OpFunction) break;
            if (o == OpDecorate && a[0] == m->var[i].id) {
                if (a[1] == DecBuiltIn && a[2] == BiPosition) ov[nov].role = 0;
                else if (a[1] == DecBuiltIn && (a[2] == BiPrimTri || a[2] == BiPrimPoint)) ov[nov].role = 2;
                else if (a[1] == DecBuiltIn && a[2] == BiPointSize) ov[nov].role = 3;
                else if (a[1] == DecLocation) ov[nov].role = 1, ov[nov].loc = a[2];
                else if (a[1] == DecBuiltIn) { *why = "an unsupported mesh output built-in"; goto done; }
                else if (a[1] == DecPerPrimitive) { *why = "per-primitive outputs are not emulated"; goto done; }
            }
            k += l2;
        }
        if (ov[nov].role == 9) { *why = "an output without a role"; goto done; }
        if (ov[nov].role < 2 && arr[2] != m->t_v4f) { *why = "a vertex output that is not a vec4"; goto done; }
        if (ov[nov].role == 3 && arr[2] != m->t_f32) { *why = "a point size that is not a float"; goto done; }
        if (ov[nov].role == 2 && arr[2] != (info->prim == 3 ? m->t_v3u : m->t_u32)) { *why = "indices of an unexpected type"; goto done; }
        nov++;
    }
    info->payload_words = payload_len;
    if (payload_len > MESH_EMU_TWORDS - 1) { *why = "payload too large"; goto done; }
    if (mesh) {
        uint32_t locs = 0;
        int have_pos = 0, have_idx = 0;
        for (int i = 0; i < nov; i++) {
            if (ov[i].role == 1) {
                if (ov[i].loc >= 32 || locs >> ov[i].loc & 1) { *why = "bad generic location"; goto done; }
                locs |= 1u << ov[i].loc;
            }
            have_pos |= ov[i].role == 0, have_idx |= ov[i].role == 2;
        }
        if (!have_pos || !have_idx) { *why = "no position or no indices"; goto done; }
        for (uint32_t l = 0; l < 32; l++)
            if (locs >> l & 1) info->generic_loc[info->n_generic++] = (uint8_t)l;
        info->slots = 1 + info->n_generic;
        for (int i = 0; i < nov; i++)
            if (ov[i].role == 3) info->psize_slot = info->slots, ov[i].slot = info->slots++;
        for (int i = 0; i < nov; i++) {
            if (ov[i].role == 0) ov[i].slot = 0;
            if (ov[i].role == 1)
                for (uint32_t g = 0; g < info->n_generic; g++)
                    if (info->generic_loc[g] == ov[i].loc) ov[i].slot = 1 + g;
        }
    }
    uint32_t emit_x = 0, emit_y = 0, emit_z = 0;
    if (!mesh) {
        size_t i = 5;
        while (i < n && (w[i] & 0xffff) != OpFunction) i += w[i] >> 16;
        for (; i < n; i += w[i] >> 16)
            if ((w[i] & 0xffff) == OpEmitMeshTasksEXT) emit_x = w[i + 1], emit_y = w[i + 2], emit_z = w[i + 3];
        if (!emit_x || !payload) { *why = "no EmitMeshTasksEXT or no payload"; goto done; }
        info->max_mesh = 32;
        i = 5;
        while (i < n && (w[i] & 0xffff) != OpFunction) i += w[i] >> 16;
        for (; i < n; i += w[i] >> 16) {
            uint32_t v;
            if ((w[i] & 0xffff) == OpExtInst && (w[i] >> 16) == 7 && w[i + 2] == emit_x && w[i + 3] == m->glsl &&
                w[i + 4] == GlslUMin && const_u32(m, w[i + 6], &v) && v && v <= 1024)
                info->max_mesh = v;
        }
    }

    // ---- new types, constants and variables
    Em e = {.m = m, .types = &ntypes, .annot = &nannot, .code = &code};
    if (!m->t_bool) m->t_bool = nid(m), op(&ntypes, OpTypeBool, 1, m->t_bool);
    if (!m->t_u64) m->t_u64 = nid(m), op(&ntypes, OpTypeInt, 3, m->t_u64, 64u, 0u);
    if (!m->t_v3u) m->t_v3u = nid(m), op(&ntypes, OpTypeVector, 3, m->t_v3u, m->t_u32, 3u);
    if (!m->t_v4u) m->t_v4u = nid(m), op(&ntypes, OpTypeVector, 3, m->t_v4u, m->t_u32, 4u);
    uint32_t rt_u = nid(m), s_words = nid(m), rt_v4 = nid(m), s_slots = nid(m), pc_struct = nid(m);
    op(&ntypes, OpTypeRuntimeArray, 2, rt_u, m->t_u32);
    op(&ntypes, OpTypeStruct, 2, s_words, rt_u);
    op(&ntypes, OpTypeRuntimeArray, 2, rt_v4, m->t_v4u);
    op(&ntypes, OpTypeStruct, 2, s_slots, rt_v4);
    op(&ntypes, OpTypeStruct, 8, pc_struct, m->t_u64, m->t_u32, m->t_u32, m->t_u32, m->t_u32, m->t_u32, m->t_u32);
    e.p_words = nid(m), op(&ntypes, OpTypePointer, 3, e.p_words, (uint32_t)ScPhysicalStorageBuffer, s_words);
    e.p_slots = nid(m), op(&ntypes, OpTypePointer, 3, e.p_slots, (uint32_t)ScPhysicalStorageBuffer, s_slots);
#define NEED_PTR(DST, SC, TO) do { DST = find_ptr(m, SC, TO); if (!DST) { DST = nid(m); op(&ntypes, OpTypePointer, 3, DST, (uint32_t)(SC), TO); \
        m->ptr[m->nptr].id = DST, m->ptr[m->nptr].sc = SC, m->ptr[m->nptr++].to = TO; } } while (0)
    NEED_PTR(e.p_u32_psb, ScPhysicalStorageBuffer, m->t_u32);
    NEED_PTR(e.p_v4u_psb, ScPhysicalStorageBuffer, m->t_v4u);
    uint32_t p_pc = nid(m);
    op(&ntypes, OpTypePointer, 3, p_pc, (uint32_t)ScPushConstant, pc_struct);
    NEED_PTR(e.p_pc_u64, ScPushConstant, m->t_u64);
    NEED_PTR(e.p_pc_u32, ScPushConstant, m->t_u32);
    e.pc_var = nid(m), op(&ntypes, OpVariable, 3, p_pc, e.pc_var, (uint32_t)ScPushConstant);
    e.c64_16 = nid(m), op(&ntypes, OpConstant, 4, m->t_u64, e.c64_16, 16u, 0u);
    op(&nannot, OpDecorate, 3, rt_u, (uint32_t)DecArrayStride, 4u);
    op(&nannot, OpDecorate, 2, s_words, (uint32_t)DecBlock);
    op(&nannot, OpMemberDecorate, 4, s_words, 0u, (uint32_t)DecOffset, 0u);
    op(&nannot, OpDecorate, 3, rt_v4, (uint32_t)DecArrayStride, 16u);
    op(&nannot, OpDecorate, 2, s_slots, (uint32_t)DecBlock);
    op(&nannot, OpMemberDecorate, 4, s_slots, 0u, (uint32_t)DecOffset, 0u);
    op(&nannot, OpDecorate, 2, pc_struct, (uint32_t)DecBlock);
    for (uint32_t k = 0; k < 7; k++) op(&nannot, OpMemberDecorate, 4, pc_struct, k, (uint32_t)DecOffset, k ? 4 + 4 * k : 0u);

    // Output and payload pointers become Workgroup pointers: in place when no such pointer exists yet, else mapped
    struct { uint32_t from, to; } pmap[512];
    int npmap = 0;
    for (int i = 0; i < m->nptr; i++) {
        if (m->ptr[i].sc != ScOutput && m->ptr[i].sc != ScTaskPayload) continue;
        uint32_t have = find_ptr(m, ScWorkgroup, m->ptr[i].to);
        if (have) {
            if (npmap == 512) { *why = "too many pointer types"; goto done; }
            pmap[npmap].from = m->ptr[i].id, pmap[npmap++].to = have;
            m->ptr[i].sc = 0xffffffffu;   // dropped
        } else {
            m->ptr[i].sc = ScWorkgroup;   // rewritten in place below
        }
    }
    uint32_t p_wg_u32, p_wg_v4f = 0, p_wg_v3u = 0;
    NEED_PTR(p_wg_u32, ScWorkgroup, m->t_u32);
    uint32_t p_wg_f32 = 0;
    if (mesh) { NEED_PTR(p_wg_v4f, ScWorkgroup, m->t_v4f); NEED_PTR(p_wg_v3u, ScWorkgroup, m->t_v3u); NEED_PTR(p_wg_f32, ScWorkgroup, m->t_f32); }
    uint32_t sh_vb = 0, sh_ib = 0, wgemu = 0, p_priv_v3 = 0, p_priv_u32 = 0, p_in_u32 = find_ptr(m, ScInput, m->t_u32);
    if (mesh) {
        sh_vb = nid(m), op(&ntypes, OpVariable, 3, p_wg_u32, sh_vb, (uint32_t)ScWorkgroup);
        sh_ib = nid(m), op(&ntypes, OpVariable, 3, p_wg_u32, sh_ib, (uint32_t)ScWorkgroup);
    }
    int remap_wg = mesh && payload;
    if (remap_wg) {
        NEED_PTR(p_priv_v3, ScPrivate, m->t_v3u);
        NEED_PTR(p_priv_u32, ScPrivate, m->t_u32);
        wgemu = nid(m), op(&ntypes, OpVariable, 3, p_priv_v3, wgemu, (uint32_t)ScPrivate);
    }
    uint32_t nwg = m->nwg;
    if (!mesh && !nwg) {
        uint32_t p_in_v3;
        NEED_PTR(p_in_v3, ScInput, m->t_v3u);
        nwg = nid(m), op(&ntypes, OpVariable, 3, p_in_v3, nwg, (uint32_t)ScInput);
        op(&nannot, OpDecorate, 3, nwg, (uint32_t)DecBuiltIn, (uint32_t)BiNumWorkgroups);
    }
#undef NEED_PTR

    // ---- one pass over the module
    uint32_t moved[48];
    int nmoved = 0;
    for (int i = 0; i < nov; i++) moved[nmoved++] = ov[i].var;
    if (payload) moved[nmoved++] = payload;
    uint32_t vcount = 0, pcount = 0;
    int in_fn = 0, in_ep = 0, ep_started = 0, ep_done = 0;
    for (size_t i = 5; i < n;) {
        uint32_t o = w[i] & 0xffff, len = w[i] >> 16;
        memcpy(ins, w + i, len * 4);
        const uint32_t *a = w + i + 1;
        if (o == OpFunction) in_fn = 1, in_ep = a[1] == m->ep_fn;
        if (!in_fn) {
            if (o == OpCapability) { if (a[0] != CapMeshShading) putn(&caps, ins, len); }
            else if (o == OpExtension) { if (strcmp((const char *)a, "SPV_EXT_mesh_shader")) putn(&exts, ins, len); }
            else if (o == OpExtInstImport) putn(&imps, ins, len);
            else if (o == OpMemoryModel || o == OpEntryPoint) {}
            else if (o == OpExecutionMode || o == OpExecutionModeId) {
                if (!(o == OpExecutionMode && (a[1] == ModeOutputVertices || a[1] == ModeOutputPrimitives ||
                                               a[1] == ModeOutputTriangles || a[1] == ModeOutputLines || a[1] == ModeOutputPoints)))
                    putn(&modes, ins, len);
            } else if (is_debug(o)) putn(&debug, ins, len);
            else if (is_annot(o)) {
                int drop = 0;
                for (int k = 0; k < nmoved; k++) drop |= o == OpDecorate && a[0] == moved[k];
                if (!drop) putn(&annot, ins, len);
            } else {   // types, constants, global variables
                int drop = 0;
                if (o == OpTypePointer) {
                    for (int k = 0; k < npmap; k++) drop |= pmap[k].from == a[0];
                    if (a[1] == ScOutput || a[1] == ScTaskPayload) ins[2] = ScWorkgroup;
                }
                if (o == OpVariable && (a[2] == ScOutput || a[2] == ScTaskPayload)) ins[3] = ScWorkgroup;
                for (uint32_t k = 1; k < len; k++) {
                    int idpos = o == OpTypeFunction || o == OpTypeStruct || o == OpTypeArray || o == OpTypeRuntimeArray ||
                                (o == OpTypePointer && k == 3) || k == 1;
                    if (o == OpTypePointer && k == 1) idpos = 0;
                    for (int q = 0; idpos && q < npmap; q++)
                        if (ins[k] == pmap[q].from) ins[k] = pmap[q].to;
                }
                if (!drop) putn(&types, ins, len);
            }
            i += len;
            continue;
        }
        // function code
        if (len >= 2)
            for (int q = 0; q < npmap; q++)
                if (ins[1] == pmap[q].from) ins[1] = pmap[q].to;
        if (remap_wg && len >= 4 && (o == OpLoad || o == OpAccessChain || o == OpInBoundsAccessChain) && ins[3] == m->wgid) {
            ins[3] = wgemu;
            if (o != OpLoad) {
                if (!p_in_u32 || ins[1] != p_in_u32) { *why = "an unexpected WorkgroupId access"; goto done; }
                ins[1] = p_priv_u32;
            }
        } else if (remap_wg) {
            for (uint32_t k = 1; k < len; k++) {   // id operands only: literals of these instructions may equal the id
                int lit = (o == OpExtInst && k == 4) || (o == 81 && k >= 4) || (o == 82 && k >= 5) || (o == 79 && k >= 5) ||
                          (o == OpLoad && k >= 4) || (o == OpStore && k >= 3) || o == 251 /* OpSwitch */ || o == OpSelectionMerge ||
                          o == 246 /* OpLoopMerge */;
                if (!lit && ins[k] == m->wgid) { *why = "an unexpected WorkgroupId use"; goto done; }
            }
        }
        if (o == OpFunctionEnd) in_fn = 0, in_ep = 0;
        if (in_ep && o == OpLabel && !ep_started) {
            putn(&code, ins, len);
            size_t j = i + len;   // function variables stay first
            while (j < n && (w[j] & 0xffff) == OpVariable) putn(&code, w + j, w[j] >> 16), j += w[j] >> 16;
            ep_started = 1;
            i = j;
            if (remap_wg) {
                uint32_t lid = ld(&e, m->t_u32, m->lii), wg = ld(&e, m->t_v3u, m->wgid);
                uint32_t x = E2(&e, OpCompositeExtract, m->t_u32, wg, 0u), y = E2(&e, OpCompositeExtract, m->t_u32, wg, 1u);
                uint32_t base = words_base(&e), drw = pc_u32(&e, 1);
                uint32_t tb = word_load(&e, base, add(&e, drw, cu(&e, 8)));
                uint32_t slot = add(&e, tb, y), ok = ult(&e, slot, pc_u32(&e, 6));
                uint32_t slotc = E3(&e, OpSelect, m->t_u32, ok, slot, cu(&e, 0));
                uint32_t recw = add(&e, cu(&e, MESH_EMU_TOFF / 4), mul(&e, slotc, cu(&e, MESH_EMU_TWORDS)));
                uint32_t cnt = word_load(&e, base, recw);
                uint32_t act = E2(&e, OpLogicalAnd, m->t_bool, ok, ult(&e, x, cnt));
                // header words 4 and 5: mesh workgroups started and past this check (diagnosis: vk_dump mesh.txt)
                uint32_t l0 = E2(&e, OpIEqual, m->t_bool, lid, cu(&e, 0));
                atomic(&e, OpAtomicIAdd, base, cu(&e, 4), E3(&e, OpSelect, m->t_u32, l0, cu(&e, 1), cu(&e, 0)));
                atomic(&e, OpAtomicIAdd, base, cu(&e, 5),
                       E3(&e, OpSelect, m->t_u32, E2(&e, OpLogicalAnd, m->t_bool, l0, act), cu(&e, 1), cu(&e, 0)));
                uint32_t cont = nid(m), retl = nid(m);
                op(&code, OpSelectionMerge, 2, cont, 0u);
                op(&code, OpBranchConditional, 3, act, cont, retl);
                op(&code, OpLabel, 1, retl);
                op(&code, OpReturn, 0);
                op(&code, OpLabel, 1, cont);
                uint32_t v3 = nid(m);
                op(&code, OpCompositeConstruct, 5, m->t_v3u, v3, x, cu(&e, 0), cu(&e, 0));
                op(&code, OpStore, 2, wgemu, v3);
                for (uint32_t r = 0; r * info->threads < payload_len; r++) {   // as task_epilogue's copy
                    uint32_t k = add(&e, lid, cu(&e, r * info->threads)), mk = 0;
                    if ((r + 1) * info->threads > payload_len) if_begin(&e, ult(&e, k, cu(&e, payload_len)), &mk);
                    uint32_t v = word_load(&e, base, add(&e, add(&e, recw, cu(&e, 1)), k));
                    op(&code, OpStore, 2, E2(&e, OpAccessChain, p_wg_u32, payload, k), v);
                    if (mk) if_end(&e, mk);
                }
                barrier(&e);
            }
            continue;
        }
        if (in_ep && o == OpSetMeshOutputsEXT) {
            vcount = a[0], pcount = a[1];
            i += len;
            continue;
        }
        if (in_ep && mesh && o == OpReturn) {
            if (!vcount || ep_done) { *why = "no SetMeshOutputsEXT before the return, or several returns"; goto done; }
            mesh_epilogue(&e, info, ov, nov, vcount, pcount, sh_vb, sh_ib, p_wg_v4f, p_wg_v3u, p_wg_f32, p_wg_u32);
            ep_done = 1;
            i += len;
            continue;
        }
        if (in_ep && !mesh && o == OpEmitMeshTasksEXT) {
            task_epilogue(&e, info, payload, p_wg_u32, nwg, emit_x, emit_y, emit_z);
            ep_done = 1;
            i += len;
            continue;
        }
        putn(&code, ins, len);
        i += len;
    }
    if (!ep_done) { *why = "the entry function's end was not found"; goto done; }

    // ---- assemble
    put(&out, 0x07230203);
    put(&out, w[1] < 0x00010500 ? 0x00010500 : w[1]);   // SPIR-V 1.5: physical storage buffers are core
    put(&out, w[2]);
    put(&out, m->bound);
    put(&out, 0);
    putn(&out, caps.w, caps.n);
    if (!m->has_int64) op(&out, OpCapability, 1, (uint32_t)CapInt64);
    if (!m->has_psb) op(&out, OpCapability, 1, (uint32_t)CapPSBAddresses);
    putn(&out, exts.w, exts.n);
    putn(&out, imps.w, imps.n);
    op(&out, OpMemoryModel, 2, 5348u, 1u);   // PhysicalStorageBuffer64, GLSL450
    {   // entry point: every global variable
        Buf ep = {0};
        uint32_t pre[2] = {ModelGLCompute, m->ep_fn};
        uint32_t ifc[2048 + 16];
        int ni = 0;
        for (int i = 0; i < m->nvar && ni < 2048; i++) {
            int dropped = 0;
            if (!dropped && m->var[i].sc != ScFunction) ifc[ni++] = m->var[i].id;
        }
        // the new ones: scan ntypes for OpVariable
        for (size_t i = 0; i < ntypes.n;) {
            uint32_t o = ntypes.w[i] & 0xffff, len = ntypes.w[i] >> 16;
            if (o == OpVariable && ni < 2048 + 16) ifc[ni++] = ntypes.w[i + 2];
            i += len;
        }
        op_str(&ep, OpEntryPoint, pre, 2, "main", ifc, ni);
        putn(&out, ep.w, ep.n);
        free(ep.w);
    }
    putn(&out, modes.w, modes.n);
    putn(&out, debug.w, debug.n);
    putn(&out, annot.w, annot.n);
    putn(&out, nannot.w, nannot.n);
    putn(&out, types.w, types.n);
    putn(&out, ntypes.w, ntypes.n);
    putn(&out, code.w, code.n);
    out.w[3] = m->bound;
    if (caps.bad || exts.bad || imps.bad || modes.bad || debug.bad || annot.bad || types.bad || ntypes.bad ||
        nannot.bad || code.bad || out.bad) { *why = "out of memory"; goto done; }
    ret = out.w, *out_n = out.n, out.w = NULL;
done:
    free(caps.w), free(exts.w), free(imps.w), free(modes.w), free(debug.w), free(annot.w), free(types.w), free(ntypes.w),
        free(nannot.w), free(code.w), free(out.w);
    free(m), free(ins);
    return ret;
}

int mesh_emu_prim(const uint32_t *w, size_t n) {
    uint32_t ep = 0;
    for (size_t i = 5; i + 1 < n;) {
        uint32_t o = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || o == OpFunction) break;
        if (o == OpEntryPoint && len >= 3) ep = w[i + 1] == ModelMesh || w[i + 1] == ModelTask;
        if (o == OpExecutionMode && len >= 3 && ep) {
            if (w[i + 2] == ModeOutputTriangles) return 3;
            if (w[i + 2] == ModeOutputLines) return 2;
            if (w[i + 2] == ModeOutputPoints) return 1;
        }
        i += len;
    }
    return 0;
}

// ---- the vertex shader of an emulated mesh pipeline
// The fragment shader's input variables as declared: Location, Component, scalar type and width, Flat. 0 when one
// is not a plain 32-bit scalar or vector (the caller then goes by the reflection's per-location summary).
typedef struct { uint32_t loc, comp, type, comps, flat; } FsIn;
static int fs_inputs(const uint32_t *w, size_t n, FsIn *o, int max) {
    if (!w || n < 5 || w[0] != 0x07230203) return 0;
    uint32_t bound = w[3];
    if (bound > (1u << 22)) return 0;
    uint32_t *loc = calloc(bound, 4), *comp = calloc(bound, 4), *ty = calloc(bound, 4), *cn = calloc(bound, 4), *pt = calloc(bound, 4);
    uint8_t *flat = calloc(bound, 1), *bi = calloc(bound, 1), *hl = calloc(bound, 1);
    int k = 0, ok = loc && comp && ty && cn && pt && flat && bi && hl;
    for (size_t i = 5; ok && i < n;) {
        uint32_t o2 = w[i] & 0xffff, len = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        if (!len || i + len > n || o2 == OpFunction) break;
        if (o2 == OpDecorate && len >= 3 && a[0] < bound) {
            if (a[1] == DecLocation && len >= 4) loc[a[0]] = a[2], hl[a[0]] = 1;
            if (a[1] == 31 && len >= 4) comp[a[0]] = a[2];   // Component
            if (a[1] == 14) flat[a[0]] = 1;                 // Flat
            if (a[1] == DecBuiltIn) bi[a[0]] = 1;
        }
        if (o2 == OpTypeFloat && len >= 3 && a[0] < bound && a[1] == 32) ty[a[0]] = 1, cn[a[0]] = 1;
        if (o2 == OpTypeInt && len >= 4 && a[0] < bound && a[1] == 32) ty[a[0]] = a[2] ? 2 : 3, cn[a[0]] = 1;
        if (o2 == OpTypeVector && len >= 4 && a[0] < bound && a[1] < bound && ty[a[1]]) ty[a[0]] = ty[a[1]], cn[a[0]] = a[2];
        if (o2 == OpTypePointer && len >= 4 && a[0] < bound && a[1] == ScInput) pt[a[0]] = a[2] + 1;
        if (o2 == OpVariable && len >= 4 && a[2] == ScInput && a[1] < bound && a[0] < bound && !bi[a[1]] && hl[a[1]]) {
            uint32_t p = pt[a[0]] ? pt[a[0]] - 1 : 0;
            if (!p || p >= bound || !ty[p] || cn[p] > 4 || loc[a[1]] >= 32 || k == max) { k = 0, ok = 0; break; }
            o[k++] = (FsIn){loc[a[1]], comp[a[1]], ty[p] - 1, cn[p], flat[a[1]]};
        }
        i += len;
    }
    free(loc), free(comp), free(ty), free(cn), free(pt), free(flat), free(bi), free(hl);
    return ok ? k : 0;
}

uint32_t *mesh_emu_vs(const MeshEmuInfo *mi, uint32_t need, const uint8_t types[32], const uint8_t comps[32],
                      const uint32_t *fsw, size_t fsn, size_t *out_n) {
    Buf b = {0}, t = {0}, an = {0}, c = {0};
    FsIn in[64];
    int nin = fs_inputs(fsw, fsn, in, 64);
    if (!nin)   // the per-location summary: one vector per location
        for (uint32_t l = 0; l < 32; l++)
            if (need >> l & 1)
                in[nin++] = (FsIn){l, 0, types[l] <= 2 ? types[l] : 0, comps[l] >= 1 && comps[l] <= 4 ? comps[l] : 4, types[l] == 1 || types[l] == 2};
    uint32_t id = 1;
#define N() (id++)
    uint32_t glsl = N(), fn = N(), t_void = N(), t_fn = N(), t_u32 = N(), t_i32 = N(), t_f32 = N(), t_u64 = N(),
             t_v4u = N(), t_v4f = N(), t_v4i = N(), rt_v4 = N(), s_slots = N(), p_slots = N(), p_v4u_psb = N(),
             pc_struct = N(), p_pc = N(), pc_var = N(), p_pc_u64 = N(), p_pc_u32 = N(), p_in_i32 = N(),
             vidx = N(), p_out_v4f = N(), pos = N(), c0 = N(), c4 = N(), c16 = N(), p_out_f32 = N(), psz = N();
    (void)glsl;
    uint32_t vecs[3][5] = {{0}};   // [type][comps] vector ids (comps 2..4), [type][1] the scalar
    vecs[0][1] = t_f32, vecs[1][1] = t_i32, vecs[2][1] = t_u32, vecs[0][4] = t_v4f, vecs[1][4] = t_v4i, vecs[2][4] = t_v4u;
    op(&t, OpTypeVoid, 1, t_void);
    op(&t, OpTypeFunction, 2, t_fn, t_void);
    op(&t, OpTypeInt, 3, t_u32, 32u, 0u);
    op(&t, OpTypeInt, 3, t_i32, 32u, 1u);
    op(&t, OpTypeFloat, 2, t_f32, 32u);
    op(&t, OpTypeInt, 3, t_u64, 64u, 0u);
    op(&t, OpTypeVector, 3, t_v4u, t_u32, 4u);
    op(&t, OpTypeVector, 3, t_v4f, t_f32, 4u);
    op(&t, OpTypeVector, 3, t_v4i, t_i32, 4u);
    for (int ty = 0; ty < 3; ty++)
        for (int k = 2; k < 4; k++) {
            vecs[ty][k] = N();
            op(&t, OpTypeVector, 3, vecs[ty][k], vecs[ty][1], (uint32_t)k);
        }
    op(&t, OpTypeRuntimeArray, 2, rt_v4, t_v4u);
    op(&t, OpTypeStruct, 2, s_slots, rt_v4);
    op(&t, OpTypePointer, 3, p_slots, (uint32_t)ScPhysicalStorageBuffer, s_slots);
    op(&t, OpTypePointer, 3, p_v4u_psb, (uint32_t)ScPhysicalStorageBuffer, t_v4u);
    op(&t, OpTypeStruct, 8, pc_struct, t_u64, t_u32, t_u32, t_u32, t_u32, t_u32, t_u32);
    op(&t, OpTypePointer, 3, p_pc, (uint32_t)ScPushConstant, pc_struct);
    op(&t, OpTypePointer, 3, p_pc_u64, (uint32_t)ScPushConstant, t_u64);
    op(&t, OpTypePointer, 3, p_pc_u32, (uint32_t)ScPushConstant, t_u32);
    op(&t, OpTypePointer, 3, p_in_i32, (uint32_t)ScInput, t_i32);
    op(&t, OpTypePointer, 3, p_out_v4f, (uint32_t)ScOutput, t_v4f);
    op(&t, OpConstant, 3, t_u32, c0, 0u);
    op(&t, OpConstant, 3, t_u32, c4, 4u);
    op(&t, OpConstant, 4, t_u64, c16, 16u, 0u);
    op(&t, OpVariable, 3, p_pc, pc_var, (uint32_t)ScPushConstant);
    op(&t, OpVariable, 3, p_in_i32, vidx, (uint32_t)ScInput);
    op(&t, OpVariable, 3, p_out_v4f, pos, (uint32_t)ScOutput);
    if (mi->psize_slot) {
        op(&t, OpTypePointer, 3, p_out_f32, (uint32_t)ScOutput, t_f32);
        op(&t, OpVariable, 3, p_out_f32, psz, (uint32_t)ScOutput);
        op(&an, OpDecorate, 3, psz, (uint32_t)DecBuiltIn, (uint32_t)BiPointSize);
    }
    op(&an, OpDecorate, 3, rt_v4, (uint32_t)DecArrayStride, 16u);
    op(&an, OpDecorate, 2, s_slots, (uint32_t)DecBlock);
    op(&an, OpMemberDecorate, 4, s_slots, 0u, (uint32_t)DecOffset, 0u);
    op(&an, OpDecorate, 2, pc_struct, (uint32_t)DecBlock);
    for (uint32_t k = 0; k < 7; k++) op(&an, OpMemberDecorate, 4, pc_struct, k, (uint32_t)DecOffset, k ? 4 + 4 * k : 0u);
    op(&an, OpDecorate, 3, vidx, (uint32_t)DecBuiltIn, (uint32_t)BiVertexIndex);
    op(&an, OpDecorate, 3, pos, (uint32_t)DecBuiltIn, (uint32_t)BiPosition);
    uint32_t ifc[80], ni = 0;
    ifc[ni++] = pc_var, ifc[ni++] = vidx, ifc[ni++] = pos;
    if (mi->psize_slot) ifc[ni++] = psz;
    // outputs: one per fragment input, same Location, Component and type
    uint32_t outv[64] = {0}, outt[64] = {0};
    for (int q = 0; q < nin; q++) {
        int ty = in[q].type <= 2 ? (int)in[q].type : 0, k = in[q].comps >= 1 && in[q].comps <= 4 ? (int)in[q].comps : 4;
        if (in[q].comp + (uint32_t)k > 4) k = (int)(4 - in[q].comp);
        in[q].type = (uint32_t)ty, in[q].comps = (uint32_t)k;
        outt[q] = vecs[ty][k];
        uint32_t p = N(), v = N();
        op(&t, OpTypePointer, 3, p, (uint32_t)ScOutput, outt[q]);
        op(&t, OpVariable, 3, p, v, (uint32_t)ScOutput);
        op(&an, OpDecorate, 3, v, (uint32_t)DecLocation, in[q].loc);
        if (in[q].comp) op(&an, OpDecorate, 3, v, 31u, in[q].comp);   // Component
        if (in[q].flat || ty) op(&an, OpDecorate, 2, v, 14u);           // Flat
        outv[q] = v;
        ifc[ni++] = v;
    }
    // code: slot = vertex area + VertexIndex; each output from slot + its index
    uint32_t lab = N();
    op(&c, OpFunction, 4, t_void, fn, 0u, t_fn);
    op(&c, OpLabel, 1, lab);
    uint32_t e64 = N(), e64p = N(), vw = N(), vwp = N(), vi = N(), viu = N(), s = N(), s64 = N(), off = N(), a = N(), base = N();
    op(&c, OpAccessChain, 4, p_pc_u64, e64p, pc_var, c0);
    op(&c, OpLoad, 3, t_u64, e64, e64p);
    op(&c, OpAccessChain, 4, p_pc_u32, vwp, pc_var, c4);
    op(&c, OpLoad, 3, t_u32, vw, vwp);
    op(&c, OpLoad, 3, t_i32, vi, vidx);
    op(&c, OpBitcast, 3, t_u32, viu, vi);
    op(&c, OpIAdd, 4, t_u32, s, vw, viu);
    op(&c, OpUConvert, 3, t_u64, s64, s);
    op(&c, OpIMul, 4, t_u64, off, s64, c16);
    op(&c, OpIAdd, 4, t_u64, a, e64, off);
    op(&c, OpConvertUToPtr, 3, p_slots, base, a);
    if (mi->psize_slot) {
        uint32_t ci = N(), p = N(), v = N(), x = N(), f = N();
        op(&t, OpConstant, 3, t_u32, ci, mi->psize_slot);
        op(&c, OpAccessChain, 5, p_v4u_psb, p, base, c0, ci);
        op(&c, OpLoad, 5, t_v4u, v, p, 2u, 16u);
        op(&c, OpCompositeExtract, 4, t_u32, x, v, 0u);
        op(&c, OpBitcast, 3, t_f32, f, x);
        op(&c, OpStore, 2, psz, f);
    }
    uint32_t slot_of[33];
    for (int k = 0; k < 33; k++) slot_of[k] = 0xffffffffu;
    slot_of[32] = 0;   // position
    for (uint32_t g = 0; g < mi->n_generic; g++) slot_of[mi->generic_loc[g]] = 1 + g;
    for (int q = -1; q < nin; q++) {   // -1: the position
        uint32_t l = q < 0 ? 32 : in[q].loc;
        uint32_t val;
        if (slot_of[l] == 0xffffffffu) {
            val = 0;
        } else {
            uint32_t ci = N(), p = N();
            val = N();
            op(&t, OpConstant, 3, t_u32, ci, slot_of[l]);
            op(&c, OpAccessChain, 5, p_v4u_psb, p, base, c0, ci);
            op(&c, OpLoad, 5, t_v4u, val, p, 2u, 16u);
        }
        if (q < 0) {
            uint32_t f = N();
            op(&c, OpBitcast, 3, t_v4f, f, val);
            op(&c, OpStore, 2, pos, f);
            continue;
        }
        int ty = (int)in[q].type, k = (int)in[q].comps;
        uint32_t v4t = vecs[ty][4], r, c0i = in[q].comp;
        if (!val) {   // not written by the mesh stage: zero
            uint32_t z = N();
            r = N();
            op(&t, OpConstantNull, 2, outt[q], z);
            op(&c, OpCopyObject, 3, outt[q], r, z);
        } else {
            uint32_t cv = val;
            if (ty != 2) cv = N(), op(&c, OpBitcast, 3, v4t, cv, val);
            if (k == 4) r = cv;
            else if (k == 1) r = N(), op(&c, OpCompositeExtract, 4, outt[q], r, cv, c0i);
            else {
                r = N();
                put(&c, (uint32_t)(5 + k) << 16 | 79u);   // OpVectorShuffle: components comp..comp+k-1
                put(&c, outt[q]), put(&c, r), put(&c, cv), put(&c, cv);
                for (int z = 0; z < k; z++) put(&c, c0i + (uint32_t)z);
            }
        }
        op(&c, OpStore, 2, outv[q], r);
    }
    op(&c, OpReturn, 0);
    op(&c, OpFunctionEnd, 0);
    // assemble
    put(&b, 0x07230203), put(&b, 0x00010500), put(&b, 0), put(&b, id), put(&b, 0);
    op(&b, OpCapability, 1, (uint32_t)CapShader);
    op(&b, OpCapability, 1, (uint32_t)CapInt64);
    op(&b, OpCapability, 1, (uint32_t)CapPSBAddresses);
    op(&b, OpMemoryModel, 2, 5348u, 1u);
    uint32_t pre[2] = {ModelVertex, fn};
    op_str(&b, OpEntryPoint, pre, 2, "main", ifc, (int)ni);
    putn(&b, an.w, an.n);
    putn(&b, t.w, t.n);
    putn(&b, c.w, c.n);
    b.w[3] = id;
    int bad = b.bad || t.bad || an.bad || c.bad;
    free(t.w), free(an.w), free(c.w);
    if (bad) { free(b.w); return NULL; }
    *out_n = b.n;
    return b.w;
#undef N
}

// The allocation shader (mesh_emu_spirv.h): glslangValidator -V -g0 --target-env vulkan1.2, spirv-opt -O --strip-debug,
// spirv-val clean. Its source:
//   #version 460
//   #extension GL_EXT_buffer_reference : require
//   #extension GL_EXT_buffer_reference_uvec2 : require
//   layout(local_size_x = 1) in;
//   layout(buffer_reference, std430, buffer_reference_align = 4) buffer Words { uint w[]; };
//   layout(push_constant) uniform PC { uvec2 addr; uint first; uint n; uint mode; uint icap; uint trecs; uint pad; } pc;
//   void main() {
//       Words a = Words(pc.addr);
//       for (uint d = 0u; d < pc.n; d++) {
//           uint r = 64u + ((pc.first + d) % 4096u) * 16u;
//           bool task = a.w[r + 14u] != 0u;
//           if (pc.mode == 0u && task) {
//               uint tb = a.w[2], k = a.w[r + 10u];
//               a.w[r + 8u] = tb;
//               a.w[2] = k > pc.trecs ? pc.trecs + tb : tb + k;
//           } else if ((pc.mode == 0u) != task) {
//               uint g = pc.mode == 0u ? a.w[r + 10u] : a.w[r + 9u], per = a.w[r + 13u], ib = a.w[1];
//               uint ni = per != 0u && g > pc.icap / per ? pc.icap + 1u : g * per;
//               a.w[r + 2u] = ib;
//               a.w[r + 12u] = ib >= pc.icap ? 0u : min(ni, pc.icap - ib);
//               a.w[1] = min(ib + ni, 0x7fffffffu);
//           }
//       }
//   }
static const uint32_t alloc_words[] = {
    0x07230203, 0x00010500, 0x0008000b, 0x000000df, 0x00000000, 0x00020011, 0x00000001, 0x00020011,
    0x000014e3, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e,
    0x000014e4, 0x00000001, 0x0006000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000f,
    0x00060010, 0x00000004, 0x00000011, 0x00000001, 0x00000001, 0x00000001, 0x00040047, 0x00000008,
    0x00000006, 0x00000004, 0x00030047, 0x00000009, 0x00000002, 0x00050048, 0x00000009, 0x00000000,
    0x00000023, 0x00000000, 0x00030047, 0x0000000d, 0x00000002, 0x00050048, 0x0000000d, 0x00000000,
    0x00000023, 0x00000000, 0x00050048, 0x0000000d, 0x00000001, 0x00000023, 0x00000008, 0x00050048,
    0x0000000d, 0x00000002, 0x00000023, 0x0000000c, 0x00050048, 0x0000000d, 0x00000003, 0x00000023,
    0x00000010, 0x00050048, 0x0000000d, 0x00000004, 0x00000023, 0x00000014, 0x00050048, 0x0000000d,
    0x00000005, 0x00000023, 0x00000018, 0x00050048, 0x0000000d, 0x00000006, 0x00000023, 0x0000001c,
    0x00040047, 0x000000cb, 0x0000000b, 0x00000019, 0x00020013, 0x00000002, 0x00030021, 0x00000003,
    0x00000002, 0x00030027, 0x00000006, 0x000014e5, 0x00040015, 0x00000007, 0x00000020, 0x00000000,
    0x0003001d, 0x00000008, 0x00000007, 0x0003001e, 0x00000009, 0x00000008, 0x00040020, 0x00000006,
    0x000014e5, 0x00000009, 0x00040017, 0x0000000c, 0x00000007, 0x00000002, 0x0009001e, 0x0000000d,
    0x0000000c, 0x00000007, 0x00000007, 0x00000007, 0x00000007, 0x00000007, 0x00000007, 0x00040020,
    0x0000000e, 0x00000009, 0x0000000d, 0x0004003b, 0x0000000e, 0x0000000f, 0x00000009, 0x00040015,
    0x00000010, 0x00000020, 0x00000001, 0x0004002b, 0x00000010, 0x00000011, 0x00000000, 0x00040020,
    0x00000012, 0x00000009, 0x0000000c, 0x0004002b, 0x00000007, 0x00000018, 0x00000000, 0x0004002b,
    0x00000010, 0x0000001f, 0x00000002, 0x00040020, 0x00000020, 0x00000009, 0x00000007, 0x00020014,
    0x00000023, 0x0004002b, 0x00000010, 0x00000027, 0x00000001, 0x0004002b, 0x00000007, 0x0000002c,
    0x00001000, 0x0004002b, 0x00000007, 0x0000002e, 0x00000010, 0x00040020, 0x00000037, 0x000014e5,
    0x00000007, 0x0004002b, 0x00000010, 0x0000003b, 0x00000003, 0x0004002b, 0x00000010, 0x00000056,
    0x00000005, 0x0004002b, 0x00000010, 0x00000094, 0x00000004, 0x0004002b, 0x00000007, 0x000000a0,
    0x00000001, 0x0004002b, 0x00000007, 0x000000c5, 0x7fffffff, 0x00040017, 0x000000ca, 0x00000007,
    0x00000003, 0x0006002c, 0x000000ca, 0x000000cb, 0x000000a0, 0x000000a0, 0x000000a0, 0x0004002b,
    0x00000007, 0x000000d8, 0x0000004e, 0x0004002b, 0x00000007, 0x000000d9, 0x00000049, 0x0004002b,
    0x00000007, 0x000000da, 0x0000004a, 0x0004002b, 0x00000007, 0x000000db, 0x0000004d, 0x0004002b,
    0x00000007, 0x000000dc, 0x00000042, 0x0004002b, 0x00000007, 0x000000dd, 0x0000004c, 0x0004002b,
    0x00000007, 0x000000de, 0x00000048, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003,
    0x000200f8, 0x00000005, 0x00050041, 0x00000012, 0x00000013, 0x0000000f, 0x00000011, 0x0004003d,
    0x0000000c, 0x00000014, 0x00000013, 0x0004007c, 0x00000006, 0x00000015, 0x00000014, 0x000200f9,
    0x00000019, 0x000200f8, 0x00000019, 0x000700f5, 0x00000007, 0x000000cc, 0x00000018, 0x00000005,
    0x000000c9, 0x0000001c, 0x00050041, 0x00000020, 0x00000021, 0x0000000f, 0x0000001f, 0x0004003d,
    0x00000007, 0x00000022, 0x00000021, 0x000500b0, 0x00000023, 0x00000024, 0x000000cc, 0x00000022,
    0x000400f6, 0x0000001b, 0x0000001c, 0x00000000, 0x000400fa, 0x00000024, 0x0000001a, 0x0000001b,
    0x000200f8, 0x0000001a, 0x00050041, 0x00000020, 0x00000028, 0x0000000f, 0x00000027, 0x0004003d,
    0x00000007, 0x00000029, 0x00000028, 0x00050080, 0x00000007, 0x0000002b, 0x00000029, 0x000000cc,
    0x00050089, 0x00000007, 0x0000002d, 0x0000002b, 0x0000002c, 0x00050084, 0x00000007, 0x0000002f,
    0x0000002d, 0x0000002e, 0x00050080, 0x00000007, 0x00000036, 0x0000002f, 0x000000d8, 0x00060041,
    0x00000037, 0x00000038, 0x00000015, 0x00000011, 0x00000036, 0x0006003d, 0x00000007, 0x00000039,
    0x00000038, 0x00000002, 0x00000004, 0x000500ab, 0x00000023, 0x0000003a, 0x00000039, 0x00000018,
    0x00050041, 0x00000020, 0x0000003c, 0x0000000f, 0x0000003b, 0x0004003d, 0x00000007, 0x0000003d,
    0x0000003c, 0x000500aa, 0x00000023, 0x0000003e, 0x0000003d, 0x00000018, 0x000500a7, 0x00000023,
    0x00000040, 0x0000003e, 0x0000003a, 0x000300f7, 0x00000042, 0x00000000, 0x000400fa, 0x00000040,
    0x00000041, 0x00000067, 0x000200f8, 0x00000041, 0x00060041, 0x00000037, 0x00000045, 0x00000015,
    0x00000011, 0x0000001f, 0x0006003d, 0x00000007, 0x00000046, 0x00000045, 0x00000002, 0x00000004,
    0x00050080, 0x00000007, 0x0000004b, 0x0000002f, 0x000000da, 0x00060041, 0x00000037, 0x0000004c,
    0x00000015, 0x00000011, 0x0000004b, 0x0006003d, 0x00000007, 0x0000004d, 0x0000004c, 0x00000002,
    0x00000004, 0x00050080, 0x00000007, 0x00000051, 0x0000002f, 0x000000de, 0x00060041, 0x00000037,
    0x00000053, 0x00000015, 0x00000011, 0x00000051, 0x0005003e, 0x00000053, 0x00000046, 0x00000002,
    0x00000004, 0x00050041, 0x00000020, 0x00000057, 0x0000000f, 0x00000056, 0x0004003d, 0x00000007,
    0x00000058, 0x00000057, 0x000500ac, 0x00000023, 0x00000059, 0x0000004d, 0x00000058, 0x000300f7,
    0x0000005c, 0x00000000, 0x000400fa, 0x00000059, 0x0000005b, 0x00000061, 0x000200f8, 0x0000005b,
    0x00050080, 0x00000007, 0x00000060, 0x00000058, 0x00000046, 0x000200f9, 0x0000005c, 0x000200f8,
    0x00000061, 0x00050080, 0x00000007, 0x00000064, 0x00000046, 0x0000004d, 0x000200f9, 0x0000005c,
    0x000200f8, 0x0000005c, 0x000700f5, 0x00000007, 0x000000d0, 0x00000060, 0x0000005b, 0x00000064,
    0x00000061, 0x0005003e, 0x00000045, 0x000000d0, 0x00000002, 0x00000004, 0x000200f9, 0x00000042,
    0x000200f8, 0x00000067, 0x000500a5, 0x00000023, 0x0000006c, 0x0000003e, 0x0000003a, 0x000300f7,
    0x0000006e, 0x00000000, 0x000400fa, 0x0000006c, 0x0000006d, 0x0000006e, 0x000200f8, 0x0000006d,
    0x000300f7, 0x00000075, 0x00000000, 0x000400fa, 0x0000003e, 0x00000074, 0x0000007b, 0x000200f8,
    0x00000074, 0x00050080, 0x00000007, 0x00000078, 0x0000002f, 0x000000da, 0x00060041, 0x00000037,
    0x00000079, 0x00000015, 0x00000011, 0x00000078, 0x0006003d, 0x00000007, 0x0000007a, 0x00000079,
    0x00000002, 0x00000004, 0x000200f9, 0x00000075, 0x000200f8, 0x0000007b, 0x00050080, 0x00000007,
    0x0000007f, 0x0000002f, 0x000000d9, 0x00060041, 0x00000037, 0x00000080, 0x00000015, 0x00000011,
    0x0000007f, 0x0006003d, 0x00000007, 0x00000081, 0x00000080, 0x00000002, 0x00000004, 0x000200f9,
    0x00000075, 0x000200f8, 0x00000075, 0x000700f5, 0x00000007, 0x000000cd, 0x0000007a, 0x00000074,
    0x00000081, 0x0000007b, 0x00050080, 0x00000007, 0x00000087, 0x0000002f, 0x000000db, 0x00060041,
    0x00000037, 0x00000088, 0x00000015, 0x00000011, 0x00000087, 0x0006003d, 0x00000007, 0x00000089,
    0x00000088, 0x00000002, 0x00000004, 0x00060041, 0x00000037, 0x0000008c, 0x00000015, 0x00000011,
    0x00000027, 0x0006003d, 0x00000007, 0x0000008d, 0x0000008c, 0x00000002, 0x00000004, 0x000500ab,
    0x00000023, 0x00000090, 0x00000089, 0x00000018, 0x000300f7, 0x00000092, 0x00000000, 0x000400fa,
    0x00000090, 0x00000091, 0x00000092, 0x000200f8, 0x00000091, 0x00050041, 0x00000020, 0x00000095,
    0x0000000f, 0x00000094, 0x0004003d, 0x00000007, 0x00000096, 0x00000095, 0x00050086, 0x00000007,
    0x00000098, 0x00000096, 0x00000089, 0x000500ac, 0x00000023, 0x00000099, 0x000000cd, 0x00000098,
    0x000200f9, 0x00000092, 0x000200f8, 0x00000092, 0x000700f5, 0x00000023, 0x0000009a, 0x00000090,
    0x00000075, 0x00000099, 0x00000091, 0x000300f7, 0x0000009d, 0x00000000, 0x000400fa, 0x0000009a,
    0x0000009c, 0x000000a2, 0x000200f8, 0x0000009c, 0x00050041, 0x00000020, 0x0000009e, 0x0000000f,
    0x00000094, 0x0004003d, 0x00000007, 0x0000009f, 0x0000009e, 0x00050080, 0x00000007, 0x000000a1,
    0x0000009f, 0x000000a0, 0x000200f9, 0x0000009d, 0x000200f8, 0x000000a2, 0x00050084, 0x00000007,
    0x000000a5, 0x000000cd, 0x00000089, 0x000200f9, 0x0000009d, 0x000200f8, 0x0000009d, 0x000700f5,
    0x00000007, 0x000000ce, 0x000000a1, 0x0000009c, 0x000000a5, 0x000000a2, 0x00050080, 0x00000007,
    0x000000aa, 0x0000002f, 0x000000dc, 0x00060041, 0x00000037, 0x000000ac, 0x00000015, 0x00000011,
    0x000000aa, 0x0005003e, 0x000000ac, 0x0000008d, 0x00000002, 0x00000004, 0x00050080, 0x00000007,
    0x000000b0, 0x0000002f, 0x000000dd, 0x00050041, 0x00000020, 0x000000b2, 0x0000000f, 0x00000094,
    0x0004003d, 0x00000007, 0x000000b3, 0x000000b2, 0x000500ae, 0x00000023, 0x000000b4, 0x0000008d,
    0x000000b3, 0x000300f7, 0x000000b7, 0x00000000, 0x000400fa, 0x000000b4, 0x000000b6, 0x000000b8,
    0x000200f8, 0x000000b6, 0x000200f9, 0x000000b7, 0x000200f8, 0x000000b8, 0x00050082, 0x00000007,
    0x000000bd, 0x000000b3, 0x0000008d, 0x0007000c, 0x00000007, 0x000000be, 0x00000001, 0x00000026,
    0x000000ce, 0x000000bd, 0x000200f9, 0x000000b7, 0x000200f8, 0x000000b7, 0x000700f5, 0x00000007,
    0x000000cf, 0x00000018, 0x000000b6, 0x000000be, 0x000000b8, 0x00060041, 0x00000037, 0x000000c0,
    0x00000015, 0x00000011, 0x000000b0, 0x0005003e, 0x000000c0, 0x000000cf, 0x00000002, 0x00000004,
    0x00050080, 0x00000007, 0x000000c4, 0x0000008d, 0x000000ce, 0x0007000c, 0x00000007, 0x000000c6,
    0x00000001, 0x00000026, 0x000000c4, 0x000000c5, 0x0005003e, 0x0000008c, 0x000000c6, 0x00000002,
    0x00000004, 0x000200f9, 0x0000006e, 0x000200f8, 0x0000006e, 0x000200f9, 0x00000042, 0x000200f8,
    0x00000042, 0x000200f9, 0x0000001c, 0x000200f8, 0x0000001c, 0x00050080, 0x00000007, 0x000000c9,
    0x000000cc, 0x00000027, 0x000200f9, 0x00000019, 0x000200f8, 0x0000001b, 0x000100fd, 0x00010038,
};
const uint32_t *mesh_emu_alloc_spirv(size_t *n) {
    *n = sizeof alloc_words / sizeof *alloc_words;
    return alloc_words;
}
_Static_assert(MESH_EMU_DOFF / 4 == 64 && MESH_EMU_DRECS == 4096 && MESH_EMU_DWORDS == 16 && MESH_EMU_R_GROUPS == 9 &&
                   MESH_EMU_R_FIRST == 10 && MESH_EMU_R_ICAP == 12 && MESH_EMU_R_PER == 13 && MESH_EMU_R_TASK == 14,
               "the allocation shader's constants");
