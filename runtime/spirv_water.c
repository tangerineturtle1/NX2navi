// The water corrections of docs/water-projective.md, made on a SPIR-V module in memory. Each edit follows
// tools/projective_recipe.py step for step, new ids and declarations in the same order, so the module it returns is the
// one that tool writes for the same input (tests/water_projective_check.py compares them on a made-up module).
//
// A module is taken apart into its instructions, edited as a list, and put together again. New types and constants
// are collected and placed before the module's one function; an existing type or constant of the same definition is
// used instead of a new one. Every edit refuses a module that is not of the shape it was written for: one entry
// point, one function, one return, and the interface or the store it looks for found exactly once.
#include <stdlib.h>
#include <string.h>

#include "spirv_water.h"

enum { OP_EXT_INST_IMPORT = 11, OP_EXT_INST = 12, OP_ENTRY_POINT = 15, OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22,
       OP_TYPE_VECTOR = 23, OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_CONSTANT = 43, OP_FUNCTION = 54,
       OP_VARIABLE = 59, OP_LOAD = 61, OP_STORE = 62, OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66,
       OP_DECORATE = 71, OP_MEMBER_DECORATE = 72, OP_COMPOSITE_CONSTRUCT = 80, OP_COMPOSITE_EXTRACT = 81,
       OP_FMUL = 133, OP_FDIV = 136, OP_LABEL = 248, OP_RETURN = 253 };
enum { DEC_BUILTIN = 11, DEC_NOPERSPECTIVE = 13, DEC_LOCATION = 30, SC_INPUT = 1, SC_OUTPUT = 3, SC_FUNCTION = 7,
       GLSL_FMA = 50, GLSL_NMAX = 80 };

typedef struct { uint32_t op, n; uint32_t *a; } Ins;   // n: words after the first; a: those words
typedef struct {
    uint32_t hdr[5];
    Ins *ops;
    size_t nops, cap;
    Ins extra[24];          // new types and constants, in the order made
    size_t nextra;
    uint32_t *src;          // the module's words: the instructions' arguments point into this copy
    uint32_t pool[256];     // the arguments of the instructions added
    size_t np;
    uint32_t model;
    int bad;                // an edit ran out of room: the module is refused
} Mod;

static void mod_free(Mod *m) { free(m->ops), free(m->src); }

static int mod_parse(Mod *m, const uint32_t *w, uint64_t n) {
    memset(m, 0, sizeof *m);
    if (!w || n < 5 || w[0] != 0x07230203u) return 0;
    if (!(m->src = malloc((size_t)n * 4))) return 0;
    memcpy(m->src, w, (size_t)n * 4);
    memcpy(m->hdr, w, sizeof m->hdr);
    m->cap = 1024;
    if (!(m->ops = malloc(m->cap * sizeof *m->ops))) return mod_free(m), 0;
    unsigned entries = 0, functions = 0, returns = 0;
    for (uint64_t at = 5; at < n;) {
        uint32_t len = m->src[at] >> 16, op = m->src[at] & 65535;
        if (!len || at + len > n) return mod_free(m), 0;
        if (m->nops + 64 > m->cap) {   // 64: room for what the edits insert
            Ins *t = realloc(m->ops, (m->cap *= 2) * sizeof *t);
            if (!t) return mod_free(m), 0;
            m->ops = t;
        }
        m->ops[m->nops++] = (Ins){op, len - 1, m->src + at + 1};
        if (op == OP_ENTRY_POINT && len >= 2) entries++, m->model = m->src[at + 1];
        functions += op == OP_FUNCTION, returns += op == OP_RETURN;
        at += len;
    }
    if (entries != 1 || functions != 1 || returns != 1) return mod_free(m), 0;
    return 1;
}

static uint32_t new_id(Mod *m) { return m->hdr[3]++; }

static uint32_t *words(Mod *m, uint32_t n, const uint32_t *v) {
    if (m->np + n > sizeof m->pool / sizeof *m->pool) { m->bad = 1; return m->pool; }
    uint32_t *p = m->pool + m->np;
    memcpy(p, v, n * 4);
    m->np += n;
    return p;
}

#define INS(m, op, ...) ((Ins){op, sizeof((uint32_t[]){__VA_ARGS__}) / 4, \
                               words(m, sizeof((uint32_t[]){__VA_ARGS__}) / 4, (uint32_t[]){__VA_ARGS__})})

static void add_extra(Mod *m, Ins i) {
    if (m->nextra == sizeof m->extra / sizeof *m->extra) { m->bad = 1; return; }
    m->extra[m->nextra++] = i;
}

static int is_type(uint32_t op) { return op >= 19 && op <= 33; }

// The type or variable instruction that defines id, among the module's own and the new ones.
static const Ins *type_of(const Mod *m, uint32_t id) {
    for (size_t i = 0; i < m->nops; i++)
        if (is_type(m->ops[i].op) && m->ops[i].n >= 1 && m->ops[i].a[0] == id) return &m->ops[i];
    for (size_t i = 0; i < m->nextra; i++)
        if (is_type(m->extra[i].op) && m->extra[i].a[0] == id) return &m->extra[i];
    return NULL;
}

static const Ins *variable_of(const Mod *m, uint32_t id) {
    for (size_t i = 0; i < m->nops; i++)
        if (m->ops[i].op == OP_VARIABLE && m->ops[i].n >= 3 && m->ops[i].a[1] == id) return &m->ops[i];
    return NULL;
}

// The id of the type (op, v[0..k)), an existing one or a new one.
static uint32_t type_id(Mod *m, uint32_t op, uint32_t k, const uint32_t *v) {
    for (size_t i = 0; i < m->nops; i++)
        if (m->ops[i].op == op && m->ops[i].n == k + 1 && !memcmp(m->ops[i].a + 1, v, k * 4)) return m->ops[i].a[0];
    for (size_t i = 0; i < m->nextra; i++)
        if (m->extra[i].op == op && m->extra[i].n == k + 1 && !memcmp(m->extra[i].a + 1, v, k * 4)) return m->extra[i].a[0];
    uint32_t id = new_id(m), all[4] = {id};
    memcpy(all + 1, v, k * 4);
    add_extra(m, (Ins){op, k + 1, words(m, k + 1, all)});
    return id;
}
#define TYPE(m, op, ...) type_id(m, op, sizeof((uint32_t[]){__VA_ARGS__}) / 4, (uint32_t[]){__VA_ARGS__})

// The id of the 32-bit constant `bits` of type ty, an existing one or a new one.
static uint32_t constant_id(Mod *m, uint32_t ty, uint32_t bits) {
    for (size_t i = 0; i < m->nops; i++)
        if (m->ops[i].op == OP_CONSTANT && m->ops[i].n == 3 && m->ops[i].a[0] == ty && m->ops[i].a[2] == bits)
            return m->ops[i].a[1];
    for (size_t i = 0; i < m->nextra; i++)
        if (m->extra[i].op == OP_CONSTANT && m->extra[i].a[0] == ty && m->extra[i].a[2] == bits) return m->extra[i].a[1];
    uint32_t id = new_id(m);
    add_extra(m, INS(m, OP_CONSTANT, ty, id, bits));
    return id;
}

// The one interface variable of the storage class at location 0, a four-component 32-bit float vector.
static int location0(const Mod *m, uint32_t storage, uint32_t *var, uint32_t *vec, uint32_t *scalar) {
    unsigned found = 0;
    for (size_t i = 0; i < m->nops; i++) {
        const Ins *d = &m->ops[i];
        if (d->op != OP_DECORATE || d->n != 3 || d->a[1] != DEC_LOCATION || d->a[2] != 0) continue;
        const Ins *v = variable_of(m, d->a[0]);
        if (v && v->a[2] == storage) *var = d->a[0], found++;
    }
    if (found != 1) return 0;
    const Ins *p = type_of(m, variable_of(m, *var)->a[0]);
    if (!p || p->op != OP_TYPE_POINTER || p->n != 3 || p->a[1] != storage) return 0;
    const Ins *v = type_of(m, p->a[2]);
    if (!v || v->op != OP_TYPE_VECTOR || v->n != 3 || v->a[2] != 4) return 0;
    const Ins *s = type_of(m, v->a[1]);
    if (!s || s->op != OP_TYPE_FLOAT || s->n != 2 || s->a[1] != 32) return 0;
    *vec = p->a[2], *scalar = v->a[1];
    return 1;
}

// The id of the GLSL.std.450 import when the module has exactly one.
static int glsl_import(const Mod *m, uint32_t *id) {
    unsigned found = 0;
    for (size_t i = 0; i < m->nops; i++) {
        const Ins *e = &m->ops[i];
        static const char name[] = "GLSL.std.450";
        if (e->op != OP_EXT_INST_IMPORT || (e->n - 1) * 4 < sizeof name) continue;
        if (!memcmp(e->a + 1, name, sizeof name)) *id = e->a[0], found++;
    }
    return found == 1;
}

static void insert(Mod *m, size_t at, const Ins *list, size_t k) {   // mod_parse left room for the edits' instructions
    if (m->nops + k > m->cap) { m->bad = 1; return; }
    memmove(m->ops + at + k, m->ops + at, (m->nops - at) * sizeof *m->ops);
    memcpy(m->ops + at, list, k * sizeof *list);
    m->nops += k;
}

static size_t first(const Mod *m, uint32_t op) {
    for (size_t i = 0; i < m->nops; i++)
        if (m->ops[i].op == op) return i;
    return m->nops;
}

// The edited module's words, the new declarations placed before the function; frees m.
static uint32_t *mod_finish(Mod *m, uint64_t *out_n) {
    insert(m, first(m, OP_FUNCTION), m->extra, m->nextra);
    uint64_t n = 5;
    for (size_t i = 0; i < m->nops; i++) n += m->ops[i].n + 1;
    uint32_t *out = m->bad ? NULL : malloc((size_t)n * 4);
    if (out) {
        memcpy(out, m->hdr, sizeof m->hdr);
        uint64_t at = 5;
        for (size_t i = 0; i < m->nops; i++) {
            out[at++] = (m->ops[i].n + 1) << 16 | m->ops[i].op;
            memcpy(out + at, m->ops[i].a, m->ops[i].n * 4);
            at += m->ops[i].n;
        }
        *out_n = n;
    }
    mod_free(m);
    return out;
}

uint32_t *spirv_water_vertex(const uint32_t *w, uint64_t n, uint64_t *out_n) {
    Mod m;
    if (!mod_parse(&m, w, n)) return NULL;
    uint32_t varying, vec, scalar, structure = 0, member = 0, block = 0;
    unsigned positions = 0, blocks = 0;
    if (m.model != 0 || !location0(&m, SC_OUTPUT, &varying, &vec, &scalar)) return mod_free(&m), NULL;
    for (size_t i = 0; i < m.nops; i++) {
        const Ins *d = &m.ops[i];
        if (d->op == OP_MEMBER_DECORATE && d->n == 4 && d->a[2] == DEC_BUILTIN && d->a[3] == 0)   // BuiltIn Position
            structure = d->a[0], member = d->a[1], positions++;
    }
    if (positions != 1) return mod_free(&m), NULL;
    for (size_t i = 0; i < m.nops; i++) {
        const Ins *v = &m.ops[i];
        if (v->op != OP_VARIABLE || v->n < 3 || v->a[2] != SC_OUTPUT) continue;
        const Ins *p = type_of(&m, v->a[0]);
        if (p && p->op == OP_TYPE_POINTER && p->n == 3 && p->a[1] == SC_OUTPUT && p->a[2] == structure) block = v->a[1], blocks++;
    }
    const Ins *st = type_of(&m, structure);
    if (blocks != 1 || !st || st->op != OP_TYPE_STRUCT || member + 1 >= st->n || st->a[1 + member] != vec)
        return mod_free(&m), NULL;
    uint32_t pointer = TYPE(&m, OP_TYPE_POINTER, SC_OUTPUT, vec);
    uint32_t integer = TYPE(&m, OP_TYPE_INT, 32, 1);
    uint32_t member_id = constant_id(&m, integer, member);
    uint32_t one = constant_id(&m, scalar, 0x3F800000);
    uint32_t ptr = new_id(&m), pos = new_id(&m), x = new_id(&m), y = new_id(&m), cw = new_id(&m), enc = new_id(&m);
    Ins code[] = {INS(&m, OP_ACCESS_CHAIN, pointer, ptr, block, member_id),
                  INS(&m, OP_LOAD, vec, pos, ptr),
                  INS(&m, OP_COMPOSITE_EXTRACT, scalar, x, pos, 0),
                  INS(&m, OP_COMPOSITE_EXTRACT, scalar, y, pos, 1),
                  INS(&m, OP_COMPOSITE_EXTRACT, scalar, cw, pos, 3),
                  INS(&m, OP_COMPOSITE_CONSTRUCT, vec, enc, x, y, cw, one),
                  INS(&m, OP_STORE, varying, enc)};
    insert(&m, first(&m, OP_RETURN), code, sizeof code / sizeof *code);
    return mod_finish(&m, out_n);
}

uint32_t *spirv_water_fragment(const uint32_t *w, uint64_t n, uint64_t *out_n) {
    Mod m;
    if (!mod_parse(&m, w, n)) return NULL;
    uint32_t varying, vec, scalar, glsl;
    if (m.model != 4 || !location0(&m, SC_INPUT, &varying, &vec, &scalar)) return mod_free(&m), NULL;
    size_t flat = m.nops;
    unsigned flats = 0;
    for (size_t i = 0; i < m.nops; i++)
        if (m.ops[i].op == OP_DECORATE && m.ops[i].n == 2 && m.ops[i].a[0] == varying && m.ops[i].a[1] == DEC_NOPERSPECTIVE)
            flat = i, flats++;
    if (flats != 1) return mod_free(&m), NULL;
    memmove(m.ops + flat, m.ops + flat + 1, (m.nops - flat - 1) * sizeof *m.ops);   // the NoPerspective goes
    m.nops--;
    if (!glsl_import(&m, &glsl)) return mod_free(&m), NULL;
    uint32_t local_pointer = TYPE(&m, OP_TYPE_POINTER, SC_FUNCTION, vec), local = new_id(&m);
    unsigned count = 0;   // the stage's own reads of the input go to the local
    for (size_t i = 0; i < m.nops; i++) {
        Ins *o = &m.ops[i];
        if ((o->op == OP_ACCESS_CHAIN || o->op == OP_IN_BOUNDS_ACCESS_CHAIN) && o->n >= 3 && o->a[2] == varying) {
            const Ins *p = type_of(&m, o->a[0]);
            if (!p || p->op != OP_TYPE_POINTER || p->n != 3 || p->a[1] != SC_INPUT) return mod_free(&m), NULL;
            o->a[0] = TYPE(&m, OP_TYPE_POINTER, SC_FUNCTION, p->a[2]);
            o->a[2] = local, count++;
        } else if (o->op == OP_LOAD && o->n >= 3 && o->a[2] == varying) {
            o->a[2] = local, count++;
        }
    }
    if (count != 2) return mod_free(&m), NULL;
    uint32_t zero = constant_id(&m, scalar, 0), one = constant_id(&m, scalar, 0x3F800000);
    uint32_t half = constant_id(&m, scalar, 0x3F000000), nhalf = constant_id(&m, scalar, 0xBF000000);
    uint32_t raw = new_id(&m), x = new_id(&m), y = new_id(&m), cw = new_id(&m), nx = new_id(&m), ny = new_id(&m);
    uint32_t u = new_id(&m), v = new_id(&m), dec = new_id(&m);
    Ins code[] = {INS(&m, OP_VARIABLE, local_pointer, local, SC_FUNCTION),
                  INS(&m, OP_LOAD, vec, raw, varying),
                  INS(&m, OP_COMPOSITE_EXTRACT, scalar, x, raw, 0),
                  INS(&m, OP_COMPOSITE_EXTRACT, scalar, y, raw, 1),
                  INS(&m, OP_COMPOSITE_EXTRACT, scalar, cw, raw, 2),
                  INS(&m, OP_FDIV, scalar, nx, x, cw),
                  INS(&m, OP_FDIV, scalar, ny, y, cw),
                  INS(&m, OP_EXT_INST, scalar, u, glsl, GLSL_FMA, nx, half, half),
                  INS(&m, OP_EXT_INST, scalar, v, glsl, GLSL_FMA, ny, nhalf, half),
                  INS(&m, OP_COMPOSITE_CONSTRUCT, vec, dec, u, v, zero, one),
                  INS(&m, OP_STORE, local, dec)};
    size_t at = first(&m, OP_LABEL) + 1;   // after the function's own variables
    while (at < m.nops && m.ops[at].op == OP_VARIABLE) at++;
    if (at > m.nops) return mod_free(&m), NULL;
    insert(&m, at, code, sizeof code / sizeof *code);
    return mod_finish(&m, out_n);
}

uint32_t *spirv_water_absorption(const uint32_t *w, uint64_t n, uint32_t pointer, uint32_t distance, uint64_t *out_n) {
    Mod m;
    if (!mod_parse(&m, w, n)) return NULL;
    size_t store = 0;
    unsigned stores = 0, products = 0;
    uint32_t scalar = 0, glsl;
    if (m.model != 4) return mod_free(&m), NULL;
    for (size_t i = 0; i < m.nops; i++) {
        const Ins *o = &m.ops[i];
        if (o->op == OP_STORE && o->n == 2 && o->a[0] == pointer && o->a[1] == distance) store = i, stores++;
        if (o->op == OP_FMUL && o->n == 4 && o->a[1] == distance) scalar = o->a[0], products++;
    }
    const Ins *s = products == 1 ? type_of(&m, scalar) : NULL;
    if (stores != 1 || !s || s->op != OP_TYPE_FLOAT || s->n != 2 || s->a[1] != 32 || !glsl_import(&m, &glsl))
        return mod_free(&m), NULL;
    uint32_t zero = constant_id(&m, scalar, 0), guarded = new_id(&m);
    // NMax(distance, 0): a negative distance through the water must not turn the dimming into a gain
    Ins guard = INS(&m, OP_EXT_INST, scalar, guarded, glsl, GLSL_NMAX, distance, zero);
    m.ops[store] = INS(&m, OP_STORE, pointer, guarded);
    insert(&m, store, &guard, 1);
    return mod_finish(&m, out_n);
}
