// HDR output (NX2NAVI_HDR=1): an HDR10 swapchain (Rec. 2020 primaries, PQ) and a game picture tone mapped to the
// display's range, one fixed look (shaders/hdr_tonemap.frag says which). Settings: NX2NAVI_HDR, on and off following
// the menu during a run (vk_hdr_wanted), the game's own HDR (native, the menu's other HDR) from the start only; and
// the nits NX2NAVI_HDR_PEAK_NITS (the display's peak), NX2NAVI_HDR_GAME_NITS (the picture's white, "paper white") and
// NX2NAVI_HDR_UI_NITS (the HUD's and this menu's white), read when the settings change (vk_hdr_support). With
// NX2NAVI_HDR=0 the swapchain is SDR, the spliced tonemap gives the game's own SDR picture (vk_hdr_support, p0.w = -1)
// and the present is what it was.
//
// The game's picture (docs/architecture.md, "HDR"). The game tonemaps its scene into an r11g11b10f target, upscales it
// and draws the HUD into a second one (float, nothing in these passes clamps), and one last draw copies that into the
// 8-bit sRGB window texture it presents. So:
// 1. vk_hdr_shader_swap, on each fragment shader as it loads (in memory only; the cache keeps the translator's): 1.1.1's
//    tonemap ends in the fitted ACES curve and a clamp to 0..1 per channel. tonemap_splice links the function
//    nx_tonemap of shaders/hdr_tonemap.frag into that shader and stores its result in place of the three clamped
//    channels: the scene mapped onto the display (that file says how), from the values the curve takes and the scene
//    before the game's exposure gain, with the settings in a vec4 the support buffer gains at 1440. Recognised by the curve's constants, not by a hash, so
//    another translator still finds it. The game's FSR 1 upscale after it would clamp the picture to 1 again;
//    fsr_unclamp drops that clamp in its SPIR-V.
// 2. vk_hdr_draw / vk_hdr_present_source: a draw into a presented window texture that samples one float texture of
//    its size is that copy; the present then reads the float texture instead of the window texture. Any other last
//    draw into it leaves the window texture, decoded from sRGB: menus and movies stay SDR at the HUD's white.
//    The present is queued before the next frame's work (vk_backend.c), so the float target is read before the game
//    draws into it again.
// 3. vk_hdr_encode: on the present, the crop rectangle goes to PQ code values in an RGBA16F image (shaders/hdr_encode.comp:
//    gamma correction, gamut compression, the HUD's white and the peak) that FSR 1 or the blit scale into the
//    swapchain. vk_hdr_ui_*: the in-game menu draws into an RGBA8 image and shaders/hdr_ui.frag blends it onto the
//    swapchain at the HUD's white.
#include "vk.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "hdr_encode_spv.h"
#include "hdr_fullscreen_spv.h"
#include "hdr_tonemap_spv.h"
#include "hdr_ui_spv.h"
#include "host.h"
#include "nvn_api.h"
#include "vk_exec.h"
#include "vk_fg.h"
#include "platform/platform.h"

#define SLOTS 8

// ---- settings

// What the start decides for the whole run (hdr_start, first asked when the Vulkan instance is made):
// - armed: the HDR path is set up, so the menu can switch HDR on and off without a restart: the instance and device
//   get the HDR extensions (vk_device.c), the game's tonemap is spliced as its shaders load (vk_hdr_shader_swap) and
//   every draw's support buffer carries the settings (vk_exec_draw.c). While the swapchain is SDR the spliced tonemap
//   returns the game's own SDR result. Always, unless NX2NAVI_HDR_LIVE = 0 with NX2NAVI_HDR = 0 (the path left out as it
//   was before the setting followed the menu: HDR then needs a restart) or a probe turned it off (vk_hdr_off).
// - native: NX2NAVI_HDR = native, the game's own HDR. The display is reported as HDR10 (ov_gfx.c
//   QueryDisplayHdrStatus, with the peak and paper white of the settings), and the game then renders its HDR picture
//   itself and presents it as PQ-encoded Rec. 2020 in A2B10G10R10 (format 64), which goes to the HDR10 swapchain as it
//   is: no tonemap splice, no float picture, no encode (pictures still presented in 8 bits, before the switch, are
//   encoded as for 1). The game asks once, so a change to or from native waits for a restart (vk_hdr_needs_restart).
static int g_armed = -1, g_native = -1;

static void hdr_start(void) {
    if (g_armed >= 0) return;
    g_native = !strcmp(cfg_value("NX2NAVI_HDR"), "native");
    g_armed = cfg_bool("NX2NAVI_HDR") || cfg_bool("NX2NAVI_HDR_LIVE");   // last: the mark that both are read
}

int vk_hdr_armed(void) {
    hdr_start();
    return g_armed;
}

int vk_hdr_native(void) { return vk_hdr_armed() && g_native; }

// HDR is on now: the setting as the menu has it, read again when the settings change. The swapchain follows at the
// next present (gfx_present.c window_settings_live makes it again) and vk_hdr_active then says what it got. A run that
// started with the game's own HDR keeps it: the game goes on presenting its PQ picture whatever the menu says.
int vk_hdr_wanted(void) {
    static unsigned seen = ~0u;
    static int on;
    if (!vk_hdr_armed()) return 0;
    if (g_native) return 1;
    unsigned v = cfg_version();
    if (__atomic_load_n(&seen, __ATOMIC_ACQUIRE) != v) {
        __atomic_store_n(&on, cfg_bool("NX2NAVI_HDR") != 0, __ATOMIC_RELEASE);
        __atomic_store_n(&seen, v, __ATOMIC_RELEASE);
    }
    return __atomic_load_n(&on, __ATOMIC_ACQUIRE);
}

// The menu's HDR row: what is set takes a restart. To or from the game's own HDR, and switching HDR on in a run that
// started without the HDR path (NX2NAVI_HDR_LIVE = 0).
int vk_hdr_needs_restart(void) {
    hdr_start();
    return (!strcmp(cfg_value("NX2NAVI_HDR"), "native")) != g_native || (!g_armed && cfg_bool("NX2NAVI_HDR"));
}

void vk_hdr_off(void) { g_native = 0, g_armed = 0; }

// The settings, read again when they change. The replay (every draw's support buffer) and the present thread read
// them: an update fills the snapshot not in use and then publishes it.
// The game's picture is graded for a display of gamma about 2.2, and its sRGB encode is linear near black: shown as
// that encode decodes, its darkest tones sit up to a hundred times above where such a display puts them (a scene
// value of 0.001, 0.08 nits at a white of 350 nits against 0.001), the floor of a dark scene lifted to grey. So the
// present shows the picture as a 2.2 display would (hdr_encode.comp, hdr_ui.frag: the power 2.2 of the sRGB code),
// and the tonemap aims its peak below the display's by that correction's inverse (hdr_tonemap.frag nx_tonemap).
#define HDR_GAMMA 1.0f   // the present's correction: 0 none, 1: 2.2, 2: 2.4

typedef struct {
    float peak, game, ui;   // nits
    float gamma;            // HDR_GAMMA (hdr_encode.comp, hdr_ui.frag)
    float tm[4];            // the support buffer's vec4 (shaders/hdr_tonemap.frag p0)
} Nits;
static struct { unsigned seen; int cur; Nits snap[2]; PlatMutex lock; } N = {.seen = ~0u, .lock = PLAT_MUTEX_INIT};

static float clampf(float v, float lo, float hi) { return fminf(fmaxf(v, lo), hi); }

static Nits nits(void) {
    unsigned v = cfg_version();
    if (__atomic_load_n(&N.seen, __ATOMIC_ACQUIRE) != v) {
        plat_mutex_lock(&N.lock);
        if (N.seen != v) {
            Nits *n = &N.snap[!N.cur];
            n->peak = clampf((float)cfg_double("NX2NAVI_HDR_PEAK_NITS"), 80.0f, 10000.0f);
            n->game = clampf((float)cfg_double("NX2NAVI_HDR_GAME_NITS"), 48.0f, 500.0f);
            n->ui = clampf((float)cfg_double("NX2NAVI_HDR_UI_NITS"), 48.0f, 500.0f);
            n->gamma = HDR_GAMMA;
            n->tm[0] = n->peak, n->tm[1] = n->game, n->tm[2] = n->ui, n->tm[3] = 1.0f;
            __atomic_store_n(&N.cur, !N.cur, __ATOMIC_RELEASE);
            __atomic_store_n(&N.seen, v, __ATOMIC_RELEASE);
        }
        plat_mutex_unlock(&N.lock);
    }
    return N.snap[__atomic_load_n(&N.cur, __ATOMIC_ACQUIRE)];
}

void vk_hdr_support(float out[4]) {
    Nits n = nits();
    memcpy(out, n.tm, sizeof n.tm);
    if (!vk_hdr_active()) out[3] = -1.0f;   // no HDR10 swapchain (yet): the game's own SDR picture
}

// ---- 1. the tonemap shader

enum { SPV_OP_NAME = 5, SPV_OP_EXT_INST_IMPORT = 11, SPV_OP_EXT_INST = 12, SPV_OP_MEMORY_MODEL = 14,
       SPV_OP_ENTRY_POINT = 15, SPV_OP_CAPABILITY = 17, SPV_OP_TYPE_BOOL = 20, SPV_OP_TYPE_INT = 21,
       SPV_OP_TYPE_FLOAT = 22, SPV_OP_TYPE_VECTOR = 23, SPV_OP_TYPE_MATRIX = 24, SPV_OP_TYPE_STRUCT = 30,
       SPV_OP_TYPE_POINTER = 32, SPV_OP_TYPE_FUNCTION = 33, SPV_OP_CONSTANT_TRUE = 41, SPV_OP_CONSTANT_FALSE = 42,
       SPV_OP_CONSTANT = 43, SPV_OP_CONSTANT_COMPOSITE = 44, SPV_OP_CONSTANT_NULL = 46, SPV_OP_FUNCTION = 54,
       SPV_OP_FUNCTION_PARAMETER = 55, SPV_OP_FUNCTION_END = 56, SPV_OP_FUNCTION_CALL = 57, SPV_OP_VARIABLE = 59,
       SPV_OP_LOAD = 61, SPV_OP_STORE = 62, SPV_OP_ACCESS_CHAIN = 65, SPV_OP_DECORATE = 71, SPV_OP_MEMBER_DECORATE = 72,
       SPV_OP_VECTOR_SHUFFLE = 79, SPV_OP_COMPOSITE_CONSTRUCT = 80, SPV_OP_COMPOSITE_EXTRACT = 81,
       SPV_OP_COMPOSITE_INSERT = 82, SPV_OP_FADD = 129, SPV_OP_FMUL = 133, SPV_OP_FDIV = 136, SPV_OP_LOOP_MERGE = 246,
       SPV_OP_SELECTION_MERGE = 247, SPV_OP_LABEL = 248, SPV_OP_BRANCH_CONDITIONAL = 250, SPV_OP_SWITCH = 251,
       SPV_OP_LINE = 8, SPV_OP_NO_LINE = 317, SPV_DEC_OFFSET = 35, SPV_SC_UNIFORM = 2, SPV_SC_OUTPUT = 3,
       SPV_SC_FUNCTION = 7, SPV_CAP_SHADER = 1, GLSL_FMA = 50, GLSL_FMAX = 40, GLSL_FCLAMP = 43, GLSL_NMAX = 80,
       GLSL_NCLAMP = 81 };

static int name_is(const uint32_t *a, uint32_t len, const char *s) {   // OpName operands after the id
    size_t n = strlen(s);
    return (len - 2) * 4 >= n + 1 && !memcmp(a + 1, s, n + 1);
}

// A 0..1 clamp: translators write FClamp, or NClamp (NaN gives the low bound, as the GPU's saturate does).
static int is_clamp(uint32_t inst) { return inst == GLSL_FCLAMP || inst == GLSL_NCLAMP; }

// The game's FSR 1 pass after the tonemap (some frames take it in place of the plain pass, at 1:1 as well as when it
// scales up; what picks it is not known yet) ends with clamp(colour, 0.0, 1.0) per channel, which cuts the HDR
// highlights back to the HUD's white for as long as the game takes it. Its own 0..1 clamps of the filter weights each
// feed a float bit trick (OpBitcast), so only the clamps whose result is stored to the shader's output change, to
// max(x, 0.0): one word shorter, so the words after it move down and the module shrinks. Recognised by FSR's two
// reciprocal constants (0x7ef07ebb, 0x7ef19fff) and its one texture fp_t_tcb_8. Returns the clamps changed, -1 for
// that shader with no output clamp found (a translator that stores the colour another way), 0 for any other shader.
static int fsr_unclamp(uint32_t *w, uint64_t *words) {
    uint64_t n = *words;
    uint32_t bound = w[3], glsl = 0, float_type = 0, zero = 0, one = 0;
    int magic = 0, tex = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return 0;
        const uint32_t *a = w + i + 1;
        if (op == SPV_OP_EXT_INST_IMPORT && len >= 3 && name_is(a, len, "GLSL.std.450")) glsl = a[0];
        else if (op == SPV_OP_TYPE_FLOAT && len == 3 && a[1] == 32) float_type = a[0];
        else if (op == SPV_OP_CONSTANT && len == 4) {
            magic |= (a[2] == 0x7ef07ebbu) | (a[2] == 0x7ef19fffu) << 1;
            if (a[0] == float_type && float_type && a[2] == 0) zero = a[1];
            if (a[0] == float_type && float_type && a[2] == 0x3f800000u) one = a[1];
        } else if (op == SPV_OP_NAME && len >= 3 && name_is(a, len, "fp_t_tcb_8")) tex = 1;
        i += len;
    }
    if (magic != 3 || !tex) return 0;
    if (!glsl || !zero || !one || !bound) return -1;
    uint8_t *out = calloc(bound, 1);   // id -> 1: a pointer into an Output variable, 2: a value stored through one
    if (!out) return 0;
    for (uint64_t i = 5; i < n; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        if (op == SPV_OP_VARIABLE && len >= 4 && a[2] == SPV_SC_OUTPUT && a[1] < bound) out[a[1]] = 1;
        else if (op == SPV_OP_ACCESS_CHAIN && len >= 4 && a[2] < bound && out[a[2]] == 1 && a[1] < bound) out[a[1]] = 1;
        else if (op == SPV_OP_STORE && len >= 3 && a[0] < bound && out[a[0]] == 1 && a[1] < bound) out[a[1]] = 2;
    }
    int changed = 0;
    uint64_t k = 5;   // the write position: never past the read position, as instructions only shrink
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        const uint32_t *a = w + i + 1;   // result type, result, set, instruction, x, min, max
        if (op == SPV_OP_EXT_INST && len == 8 && a[2] == glsl && is_clamp(a[3]) && a[1] < bound && out[a[1]] == 2 &&
            a[5] == zero && a[6] == one) {
            uint32_t ins[7] = {7u << 16 | SPV_OP_EXT_INST, a[0], a[1], a[2], a[3] == GLSL_NCLAMP ? GLSL_NMAX : GLSL_FMAX,
                               a[4], zero};
            memmove(w + k, ins, sizeof ins);
            k += 7, changed++;
        } else {
            memmove(w + k, w + i, len * sizeof *w);
            k += len;
        }
        i += len;
    }
    free(out);
    *words = k;
    return changed ? changed : -1;
}

// Which words of an instruction are literals rather than ids, for the opcodes the tonemap library uses (word j, 1 is
// the first after the opcode word). An opcode it does not know makes the splice give up (-1).
static int spv_literal(uint32_t op, uint32_t j) {
    switch (op) {
    case SPV_OP_FUNCTION: case SPV_OP_VARIABLE: return j == 3;
    case SPV_OP_LOAD: return j >= 4;
    case SPV_OP_STORE: return j >= 3;
    case SPV_OP_COMPOSITE_EXTRACT: return j >= 4;
    case SPV_OP_COMPOSITE_INSERT: case SPV_OP_VECTOR_SHUFFLE: return j >= 5;
    case SPV_OP_EXT_INST: return j == 4;
    case SPV_OP_SELECTION_MERGE: return j == 2;
    case SPV_OP_LOOP_MERGE: return j >= 3;
    case SPV_OP_BRANCH_CONDITIONAL: return j >= 4;
    case SPV_OP_CONSTANT: return j >= 3;
    case SPV_OP_TYPE_INT: case SPV_OP_TYPE_FLOAT: return j >= 2;
    case SPV_OP_TYPE_VECTOR: case SPV_OP_TYPE_MATRIX: return j == 3;
    case SPV_OP_TYPE_POINTER: return j == 2;
    case SPV_OP_SWITCH: case SPV_OP_LINE: case SPV_OP_NO_LINE: case 63 /* OpCopyMemory */: return -1;
    default: return 0;
    }
}

typedef struct { uint32_t *w; uint64_t n, cap; int bad; } SpvOut;

static void put(SpvOut *o, const uint32_t *ins, uint32_t len) {
    if (o->n + len > o->cap) {
        uint64_t cap = (o->n + len) * 2;
        uint32_t *w = realloc(o->w, cap * sizeof *w);
        if (!w) { o->bad = 1; return; }
        o->w = w, o->cap = cap;
    }
    memcpy(o->w + o->n, ins, len * sizeof *ins);
    o->n += len;
}
#define PUT(o, ...) do { const uint32_t ins_[] = {__VA_ARGS__}; const uint32_t n_ = sizeof ins_ / sizeof *ins_; \
        uint32_t h_[1] = {n_ << 16 | ins_[0]}; put(o, h_, 1); put(o, ins_ + 1, n_ - 1); } while (0)

// One library instruction with its ids mapped (map[id], new ones already assigned); 0 when an id has no mapping.
static int put_mapped(SpvOut *o, const uint32_t *ins, const uint32_t *map, uint32_t lbound) {
    uint32_t op = ins[0] & 0xffff, len = ins[0] >> 16, tmp[64];
    if (len > 64) return 0;
    tmp[0] = ins[0];
    for (uint32_t j = 1; j < len; j++) {
        int lit = spv_literal(op, j);
        if (lit < 0) return 0;
        if (lit) { tmp[j] = ins[j]; continue; }
        if (ins[j] >= lbound || !map[ins[j]]) return 0;
        tmp[j] = map[ins[j]];
    }
    put(o, tmp, len);
    return 1;
}

static int is_type_op(uint32_t op) { return op >= 19 && op <= 39; }
static int is_const_op(uint32_t op) { return op >= SPV_OP_CONSTANT_TRUE && op <= SPV_OP_CONSTANT_NULL && op != 45; }

// What tonemap_splice knows of the game's module while it looks for the exposure: where each result is defined
// (def), the class of each variable (cls: 1 output, 2 uniform, 3 function), and for a function variable the value
// its stores gave it and how many there were (sval, scnt).
typedef struct { const uint32_t *w, *def, *sval; const uint8_t *cls, *scnt; uint32_t bound, glsl; } SpvView;

// The value behind id when id loads a function variable that is stored once (the translator keeps every value in
// one); id itself otherwise.
static uint32_t spv_stored(const SpvView *v, uint32_t id) {
    if (id >= v->bound || !v->def[id]) return id;
    const uint32_t *l = v->w + v->def[id];
    if ((l[0] & 0xffff) != SPV_OP_LOAD || l[3] >= v->bound || v->cls[l[3]] != 3 || v->scnt[l[3]] != 1) return id;
    return v->sval[l[3]];
}

static int spv_uniform_load(const SpvView *v, uint32_t id) {   // id loads a member of a uniform block
    if (id >= v->bound || !v->def[id]) return 0;
    const uint32_t *l = v->w + v->def[id];
    if ((l[0] & 0xffff) != SPV_OP_LOAD || l[3] >= v->bound || !v->def[l[3]]) return 0;
    const uint32_t *c = v->w + v->def[l[3]];
    return (c[0] & 0xffff) == SPV_OP_ACCESS_CHAIN && c[3] < v->bound && v->cls[c[3]] == 2;
}

// a and b are the same value read twice: one id, two loads of one variable, or two loads through access chains of
// one base and the same indices (the translator loads a uniform anew at each use).
static int spv_same_source(const SpvView *v, uint32_t a, uint32_t b) {
    if (a == b) return 1;
    if (a >= v->bound || b >= v->bound || !v->def[a] || !v->def[b]) return 0;
    const uint32_t *la = v->w + v->def[a], *lb = v->w + v->def[b];
    if ((la[0] & 0xffff) != SPV_OP_LOAD || (lb[0] & 0xffff) != SPV_OP_LOAD) return 0;
    if (la[3] == lb[3]) return 1;
    if (la[3] >= v->bound || lb[3] >= v->bound || !v->def[la[3]] || !v->def[lb[3]]) return 0;
    const uint32_t *ca = v->w + v->def[la[3]], *cb = v->w + v->def[lb[3]];
    return (ca[0] & 0xffff) == SPV_OP_ACCESS_CHAIN && ca[0] == cb[0] && !memcmp(ca + 3, cb + 3, ((ca[0] >> 16) - 3) * 4);
}

// The three values are products with one factor in common: 1 with the factor (the first value's operand) and what
// each is multiplied with.
static int spv_common_factor(const SpvView *v, const uint32_t val[3], uint32_t rest[3], uint32_t *factor) {
    const uint32_t *m[3];
    for (int c = 0; c < 3; c++) {
        if (val[c] >= v->bound || !v->def[val[c]]) return 0;
        m[c] = v->w + v->def[val[c]];
        if ((m[c][0] & 0xffff) != SPV_OP_FMUL) return 0;
    }
    for (int pos = 4; pos >= 3; pos--) {
        uint32_t f = m[0][pos];
        int ok = 1;
        rest[0] = m[0][7 - pos];
        for (int c = 1; c < 3 && ok; c++) {
            if (spv_same_source(v, f, m[c][4])) rest[c] = m[c][3];
            else if (spv_same_source(v, f, m[c][3])) rest[c] = m[c][4];
            else ok = 0;
        }
        if (ok) { *factor = f; return 1; }
    }
    return 0;
}

// The three values are fma(a, f, t) with one f for the three: 1 with f, and each value's a and t.
static int spv_common_fma(const SpvView *v, const uint32_t val[3], uint32_t a[3], uint32_t t[3], uint32_t *factor) {
    const uint32_t *m[3];
    for (int c = 0; c < 3; c++) {
        if (val[c] >= v->bound || !v->def[val[c]]) return 0;
        m[c] = v->w + v->def[val[c]];
        if ((m[c][0] & 0xffff) != SPV_OP_EXT_INST || (m[c][0] >> 16) != 8 || m[c][3] != v->glsl || m[c][4] != GLSL_FMA) return 0;
        t[c] = m[c][7];
    }
    for (int pos = 6; pos >= 5; pos--) {
        uint32_t f = m[0][pos];
        int ok = 1;
        a[0] = m[0][11 - pos];
        for (int c = 1; c < 3 && ok; c++) {
            if (spv_same_source(v, f, m[c][6])) a[c] = m[c][5];
            else if (spv_same_source(v, f, m[c][5])) a[c] = m[c][6];
            else ok = 0;
        }
        if (ok) { *factor = f; return 1; }
    }
    return 0;
}

// The game's exposure in the three x it feeds its curve, and those values without local exposure ("exposed"; each
// plus add where add is set). The shapes:
// - x = scene * gain, one gain for the three: a uniform in the scene's tonemap (its constants, element 3 .z), a value
//   the vertex shader passes in the shaders that draw into the tonemapped picture with the curve of their own. The
//   exposed values are x.
// - x = (scene * gain) * local exposure, the gameplay shader: the local exposure computed from a grid, the gain a
//   uniform under it. The exposed values are the inner products.
// - x = fma(scene, gain, colour), a colour added per channel after the exposure, the gain a uniform: x again.
// - x = fma(scene * gain, local exposure, colour): the inner products, plus the colours.
// 1 when one of them is found.
static int spv_exposure(const SpvView *v, const uint32_t x[3], uint32_t exposed[3], uint32_t add[3], uint32_t *gain) {
    uint32_t val[3], inner[3], rest[3], a[3], t[3], f;
    for (int c = 0; c < 3; c++) val[c] = spv_stored(v, x[c]), exposed[c] = x[c], add[c] = 0;
    if (spv_common_factor(v, val, rest, gain)) {
        if (spv_uniform_load(v, *gain)) return 1;
        for (int c = 0; c < 3; c++) inner[c] = spv_stored(v, rest[c]);
        if (spv_common_factor(v, inner, rest, &f) && spv_uniform_load(v, f)) {
            memcpy(exposed, inner, sizeof inner);
            *gain = f;
        }
        return 1;
    }
    if (!spv_common_fma(v, val, a, t, gain)) return 0;
    if (spv_uniform_load(v, *gain)) return 1;
    for (int c = 0; c < 3; c++) inner[c] = spv_stored(v, a[c]);
    if (!spv_common_factor(v, inner, rest, &f) || !spv_uniform_load(v, f)) return 0;
    memcpy(exposed, inner, sizeof inner);
    memcpy(add, t, sizeof t);
    *gain = f;
    return 1;
}

// The instruction at pos, between the function's first label and end, is in a block outside every selection and
// loop: such a block runs on every path and before the blocks after it. Constructs nest between the instruction that
// names their merge block and that block's label.
static int spv_top_level(const uint32_t *w, uint64_t first_label, uint64_t end, uint64_t pos) {
    enum { DEPTH = 64 };
    uint32_t merge[DEPTH];
    int depth = 0;
    for (uint64_t i = first_label; i < end; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff;
        if (op == SPV_OP_LABEL)
            while (depth && merge[depth - 1] == w[i + 1]) depth--;
        if (i == pos) return !depth;
        if (op == SPV_OP_SELECTION_MERGE || op == SPV_OP_LOOP_MERGE) {
            if (depth == DEPTH) return 0;
            merge[depth++] = w[i + 1];
        }
    }
    return 0;
}

// The fitted ACES curve the game ends its tonemap with: FClamp/NClamp(FMul(FMul(x, Fma(x, 2.51, 0.03)), rcp), 0, 1)
// per channel, each result stored to a function variable that the output store of its channel loads. tonemap_splice
// keeps everything the game computes and only changes what the three colour channels store: nx_tonemap
// (shaders/hdr_tonemap.frag), called just before the first of those stores, of the three x, of those values without
// local exposure with the exposure gain in them (spv_exposure; x and a gain of 1 when it finds none), and of the
// settings from the HDR_SETS vec4 the support buffer gains at 1440 (vk_exec_draw.c support_fill writes them; the
// translator's SupportBuffer ends there). The library's types are matched to the game's where the same (SPIR-V allows
// one declaration of each), its constants and functions appended with new ids. Gives up (0) on any shape it does not
// expect, and the shader stays as it was.
enum { HDR_SETS = 1, HDR_PARAMS = HDR_SETS + 2 };   // nx_tonemap(vec3 x, vec4 scene, vec4 settings[HDR_SETS])
static int tonemap_splice(uint32_t **spirv, uint64_t *words) {
    uint32_t *w = *spirv;
    uint64_t n = *words;
    uint32_t bound = w[3];
    if (bound < 8 || bound > (1u << 22)) return 0;
    const float f251 = 2.51f, f003 = 0.03f, fone = 1.0f;
    uint32_t b251, b003, bone;
    memcpy(&b251, &f251, 4), memcpy(&b003, &f003, 4), memcpy(&bone, &fone, 4);
    uint32_t glsl = 0, ft = 0, c0 = 0, c1 = 0, c251 = 0, c003 = 0, sb = 0, sb_ptr = 0, st = 0, st_len = 0, st_v4 = 0,
             it = 0, ptr_v4u = 0, last_offset = 0;
    uint64_t first_fn = 0, first_label = 0, st_at = 0, dec_at = 0;
    int nfn = 0;
    uint32_t *def = calloc(bound, sizeof *def);   // result id -> word index of its instruction
    int32_t *cval = calloc(bound, sizeof *cval);  // integer constant id -> value + 1
    uint8_t *cls = calloc(bound, 1);              // a variable's class: 1 output, 2 uniform, 3 function
    uint32_t *sval = calloc(bound, sizeof *sval); // variable -> the value last stored to it
    uint8_t *scnt = calloc(bound, 1);             // variable -> its stores, counted to 2
    if (!def || !cval || !cls || !sval || !scnt) goto fail;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) goto fail;
        const uint32_t *a = w + i + 1;
        if (op == SPV_OP_EXT_INST_IMPORT && len >= 3 && name_is(a, len, "GLSL.std.450")) glsl = a[0];
        else if (op == SPV_OP_TYPE_FLOAT && len == 3 && a[1] == 32) ft = a[0];
        else if (op == SPV_OP_TYPE_INT && len == 4 && a[1] == 32 && (!it || a[2] == 1)) it = a[0];
        else if (op == SPV_OP_CONSTANT && len == 4 && a[0] == ft && ft) {
            if (a[2] == 0) c0 = a[1];
            else if (a[2] == bone) c1 = a[1];
            else if (a[2] == b251) c251 = a[1];
            else if (a[2] == b003) c003 = a[1];
        } else if (op == SPV_OP_CONSTANT && len == 4 && a[1] < bound && a[2] < 0x10000) cval[a[1]] = (int32_t)a[2] + 1;
        else if (op == SPV_OP_NAME && len >= 3 && name_is(a, len, "support_buffer")) sb = a[0];
        else if (op == SPV_OP_MEMBER_DECORATE && len == 5 && a[1] == 10 && a[2] == SPV_DEC_OFFSET && a[3] == 1424)
            last_offset = a[0], dec_at = i;   // the struct whose member 10 is at 1424
        else if (op == SPV_OP_FUNCTION) { nfn++; if (!first_fn) first_fn = i; }
        else if (op == SPV_OP_LABEL && !first_label) first_label = i;
        if (op == SPV_OP_VARIABLE && len >= 4 && a[1] < bound) {
            if (a[1] == sb && sb) sb_ptr = a[0];
            cls[a[1]] = a[2] == SPV_SC_OUTPUT ? 1 : a[2] == SPV_SC_UNIFORM ? 2 : a[2] == SPV_SC_FUNCTION ? 3 : 0;
        }
        if (op == SPV_OP_STORE && len >= 3 && a[0] < bound) sval[a[0]] = a[1], scnt[a[0]] += scnt[a[0]] < 2;
        // where each result is defined: the globals the checks below look at, and inside the function every instruction
        // that has a result (its second word; the first definition stands, so a word of an instruction without one,
        // taken for a result here, cannot replace a real one)
        if (op == SPV_OP_VARIABLE && len >= 4 && a[1] < bound) def[a[1]] = (uint32_t)i;
        else if (first_fn && len >= 3 && a[1] < bound && !def[a[1]] && op != SPV_OP_STORE && op != SPV_OP_LABEL &&
                 op != SPV_OP_SELECTION_MERGE && op != SPV_OP_LOOP_MERGE && op != SPV_OP_BRANCH_CONDITIONAL &&
                 op != SPV_OP_SWITCH && op != 63 /* OpCopyMemory */ && op != 99 /* OpImageWrite */ &&
                 op != 228 /* OpAtomicStore */ && op != 254 /* OpReturnValue */)
            def[a[1]] = (uint32_t)i;
        i += len;
    }
    for (uint64_t i = 5; i < n; i += w[i] >> 16) {   // the support buffer's block and a Uniform vec4 pointer
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == SPV_OP_TYPE_POINTER && len == 4 && w[i + 1] == sb_ptr && sb_ptr) st = w[i + 3];
    }
    for (uint64_t i = 5; i < n && st; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == SPV_OP_TYPE_STRUCT && w[i + 1] == st) st_len = len, st_at = i, st_v4 = w[i + len - 1];
    }
    for (uint64_t i = 5; i < n && st_v4; i += w[i] >> 16)
        if ((w[i] & 0xffff) == SPV_OP_TYPE_POINTER && (w[i] >> 16) == 4 && w[i + 2] == SPV_SC_UNIFORM && w[i + 3] == st_v4)
            ptr_v4u = w[i + 1];
    if (!glsl || !ft || !it || !c0 || !c1 || !c251 || !c003 || !sb || st_len != 13 || last_offset != st || nfn != 1 ||
        !first_label || first_label < first_fn)
        goto fail;
    // the clamp sites and their x
    enum { MAX_SITES = 8 };
    uint64_t site[MAX_SITES];
    uint32_t site_x[MAX_SITES], site_var[MAX_SITES];
    int nsite = 0;
    for (uint64_t i = first_fn; i < n && nsite < MAX_SITES; i += w[i] >> 16) {
        const uint32_t *a = w + i + 1;
        if ((w[i] & 0xffff) != SPV_OP_EXT_INST || (w[i] >> 16) != 8 || a[2] != glsl || !is_clamp(a[3]) ||
            a[5] != c0 || a[6] != c1 || a[4] >= bound || !def[a[4]])
            continue;
        const uint32_t *m = w + def[a[4]];   // FMul(num, rcp)
        if ((m[0] & 0xffff) != SPV_OP_FMUL || m[3] >= bound || !def[m[3]]) continue;
        const uint32_t *num = w + def[m[3]];   // FMul(x, Fma(x, 2.51, 0.03))
        if ((num[0] & 0xffff) != SPV_OP_FMUL || num[4] >= bound || !def[num[4]]) continue;
        const uint32_t *f = w + def[num[4]];
        if ((f[0] & 0xffff) != SPV_OP_EXT_INST || (f[0] >> 16) != 8 || f[4] != GLSL_FMA || f[6] != c251 || f[7] != c003)
            continue;
        site[nsite] = i, site_x[nsite] = num[3], site_var[nsite] = 0;
        for (uint64_t j = i; j < n; j += w[j] >> 16)   // OpStore var, result
            if ((w[j] & 0xffff) == SPV_OP_STORE && (w[j] >> 16) >= 3 && w[j + 2] == a[1]) { site_var[nsite] = w[j + 1]; break; }
        if (site_var[nsite]) nsite++;
    }
    if (nsite < 3) goto fail;
    // the output stores of channels 0..2: OpStore (AccessChain out, c), value, with a load of a site's variable between
    uint64_t chain_at[3] = {0}, store_at[3] = {0};
    int chan_site[3] = {-1, -1, -1};
    for (uint64_t i = first_fn; i < n; i += w[i] >> 16) {
        const uint32_t *a = w + i + 1;
        if ((w[i] & 0xffff) != SPV_OP_STORE || (w[i] >> 16) < 3 || a[0] >= bound || !def[a[0]]) continue;
        const uint32_t *ch = w + def[a[0]];
        if ((ch[0] & 0xffff) != SPV_OP_ACCESS_CHAIN || (ch[0] >> 16) != 5 || ch[3] >= bound || cls[ch[3]] != 1 ||
            ch[4] >= bound || cval[ch[4]] < 1 || cval[ch[4]] > 3)
            continue;
        int c = cval[ch[4]] - 1;
        if (store_at[c]) goto fail;   // a channel stored twice
        chain_at[c] = def[a[0]], store_at[c] = i;
        for (uint64_t j = chain_at[c]; j < i; j += w[j] >> 16)
            if ((w[j] & 0xffff) == SPV_OP_LOAD)
                for (int s = 0; s < nsite; s++)
                    if (w[j + 3] == site_var[s]) chan_site[c] = s;
    }
    uint64_t at = ~0ull, lo = ~0ull, hi = 0;   // the insertion point; the span that must be one block
    for (int c = 0; c < 3; c++) {
        if (chan_site[c] < 0 || !store_at[c]) goto fail;
        for (int d = 0; d < c; d++)
            if (chan_site[d] == chan_site[c]) goto fail;
        int s = chan_site[c];
        at = chain_at[c] < at ? chain_at[c] : at;
        uint64_t xd = def[site_x[s]] ? def[site_x[s]] : site[s];
        lo = xd < lo ? xd : lo, lo = site[s] < lo ? site[s] : lo;
        hi = store_at[c] > hi ? store_at[c] : hi;
    }
    for (int c = 0; c < 3; c++)
        if (site[chan_site[c]] >= at || def[site_x[chan_site[c]]] >= at) goto fail;
    for (uint64_t i = lo; i < hi; i += w[i] >> 16)
        if ((w[i] & 0xffff) == SPV_OP_LABEL) goto fail;
    // the exposed values and the gain: usable at the call when defined in a block outside every selection and loop
    // before it, or in the call's own
    const SpvView view = {w, def, sval, cls, scnt, bound, glsl};
    uint32_t exposed[3], add[3], gain = 0;
    uint64_t at_block = first_label;
    for (uint64_t i = first_label + (w[first_label] >> 16); i < at; i += w[i] >> 16)
        if ((w[i] & 0xffff) == SPV_OP_LABEL) at_block = i;
    const uint32_t xs[3] = {site_x[chan_site[0]], site_x[chan_site[1]], site_x[chan_site[2]]};
    int exposure_found = spv_exposure(&view, xs, exposed, add, &gain);
    for (int c = 0; c < 7 && exposure_found; c++) {
        uint32_t idc = c < 3 ? exposed[c] : c < 6 ? add[c - 3] : gain;
        if (c >= 3 && c < 6 && !idc) continue;   // nothing added
        uint64_t d = idc < bound ? def[idc] : 0;
        if (!d || d >= at || !(d > at_block || spv_top_level(w, first_label, at, d))) exposure_found = 0;
    }
    if (!exposure_found) {
        for (int c = 0; c < 3; c++) exposed[c] = site_x[chan_site[c]], add[c] = 0;
        gain = c1;
    }

    // the library: its ids mapped into the game's id space
    const uint32_t *L = hdr_tonemap_spv;
    const uint64_t ln = sizeof hdr_tonemap_spv / 4;
    const uint32_t lbound = L[3];
    uint32_t *map = calloc(lbound, sizeof *map), id = bound;
    SpvOut g = {0}, f = {0}, o = {0};   // the library's globals, its functions, the result
    uint32_t entry = 0, fn = 0, fn_type = 0;
    if (!map) goto fail;
    for (uint64_t i = 5; i < ln; i += L[i] >> 16) {
        uint32_t op = L[i] & 0xffff, len = L[i] >> 16;
        if (op == SPV_OP_ENTRY_POINT) entry = L[i + 2];
        else if (op == SPV_OP_NAME && len >= 3 && !memcmp(L + i + 2, "nx_tonemap(", 11)) fn = L[i + 1];
    }
    for (uint64_t i = 5; i < ln;) {
        uint32_t op = L[i] & 0xffff, len = L[i] >> 16;
        const uint32_t *a = L + i + 1;
        if (op == SPV_OP_FUNCTION) break;
        if (op == SPV_OP_CAPABILITY && a[0] != SPV_CAP_SHADER) goto lib_fail;
        if (op == SPV_OP_EXT_INST_IMPORT) {
            if (!name_is(a, len, "GLSL.std.450")) goto lib_fail;
            map[a[0]] = glsl;
        } else if (is_type_op(op)) {
            uint32_t tmp[64];
            if (len > 64) goto lib_fail;
            tmp[0] = L[i], tmp[1] = 0;
            for (uint32_t j = 2; j < len; j++) {
                int lit = spv_literal(op, j);
                if (!lit && (a[j - 1] >= lbound || !map[a[j - 1]])) goto lib_fail;
                tmp[j] = lit ? a[j - 1] : map[a[j - 1]];
            }
            uint32_t same = 0;   // the game's own declaration of this type (any but a struct)
            for (uint64_t k = 5; k < first_fn && op != SPV_OP_TYPE_STRUCT && !same; k += w[k] >> 16)
                if (w[k] == L[i] && !memcmp(w + k + 2, tmp + 2, (len - 2) * 4)) same = w[k + 1];
            if (same) {
                map[a[0]] = same;
            } else {
                map[a[0]] = tmp[1] = id++;
                put(&g, tmp, len);
            }
        } else if (is_const_op(op)) {
            map[a[1]] = id++;
            if (!put_mapped(&g, L + i, map, lbound)) goto lib_fail;
        } else if (op == SPV_OP_VARIABLE && a[2] != 1 /* Input */ && a[2] != SPV_SC_OUTPUT) {
            goto lib_fail;   // a global the functions could use; main's inputs and output are left behind with main
        }
        // names, decorations (the inputs' locations), the entry point and the memory model stay the game's
        i += len;
    }
    for (uint64_t i = 5; i < ln; i += L[i] >> 16)   // nx_tonemap's function type
        if ((L[i] & 0xffff) == SPV_OP_FUNCTION && L[i + 2] == fn) fn_type = L[i + 4];
    if (!entry || !fn || !fn_type || !map[fn_type]) goto lib_fail;
    // nx_tonemap(vec3, vec4 x (1 + HDR_SETS)) with its parameters by pointer, as the compiler writes them
    const uint32_t *ftype = NULL;
    for (uint64_t i = 5; i < ln && !ftype; i += L[i] >> 16)
        if ((L[i] & 0xffff) == SPV_OP_TYPE_FUNCTION && L[i + 1] == fn_type && (L[i] >> 16) == 3 + HDR_PARAMS) ftype = L + i;
    if (!ftype) goto lib_fail;
    uint32_t param_ptr[HDR_PARAMS], v3 = map[ftype[2]], v4 = 0, ptr_types_ok = 1;
    for (int p = 0; p < HDR_PARAMS; p++) {
        param_ptr[p] = map[ftype[3 + p]];
        uint32_t pointee = 0;
        for (uint64_t i = 5; i < ln; i += L[i] >> 16)
            if ((L[i] & 0xffff) == SPV_OP_TYPE_POINTER && L[i + 1] == ftype[3 + p] && L[i + 2] == SPV_SC_FUNCTION)
                pointee = map[L[i + 3]];
        if (!pointee || (p == 0 && pointee != v3) || (p > 0 && v4 && pointee != v4)) ptr_types_ok = 0;
        if (p > 0) v4 = pointee;
    }
    if (!ptr_types_ok || v4 != st_v4) goto lib_fail;   // the library's vec4 is the support buffer's
    // the library's functions, all but its main. First a new id for every result in them (calls may refer ahead):
    // OpLabel's is its first word, any other instruction's with a result its second.
    uint64_t lib_fn = 0;
    for (uint64_t i = 5; i < ln && !lib_fn; i += L[i] >> 16)
        if ((L[i] & 0xffff) == SPV_OP_FUNCTION) lib_fn = i;
    for (uint64_t i = lib_fn; i < ln && lib_fn; i += L[i] >> 16) {
        uint32_t op = L[i] & 0xffff, len = L[i] >> 16, r;
        switch (op) {
        case SPV_OP_STORE: case SPV_OP_SELECTION_MERGE: case SPV_OP_LOOP_MERGE: case SPV_OP_BRANCH_CONDITIONAL:
        case 249 /* OpBranch */: case 253 /* OpReturn */: case 254 /* OpReturnValue */: case SPV_OP_FUNCTION_END:
        case 252 /* OpKill */: case 255 /* OpUnreachable */:
            continue;
        case SPV_OP_LABEL: r = len >= 2 ? L[i + 1] : 0; break;
        default: r = len >= 3 ? L[i + 2] : 0; break;
        }
        if (!r || r >= lbound) goto lib_fail;
        if (!map[r]) map[r] = id++;
    }
    int in_main = 0;
    for (uint64_t i = lib_fn; i < ln && lib_fn; i += L[i] >> 16) {
        uint32_t op = L[i] & 0xffff;
        if (op == SPV_OP_FUNCTION) in_main = L[i + 2] == entry;
        if (!in_main && !put_mapped(&f, L + i, map, lbound)) goto lib_fail;
        if (op == SPV_OP_FUNCTION_END) in_main = 0;
    }
    if (!lib_fn || !map[fn] || g.bad || f.bad) goto lib_fail;

    // the result: the game's module with the support block grown, the library's globals before the first function,
    // the call in main, the three stores changed, and the library's functions at the end
    uint32_t idx[HDR_SETS], ptr_v4 = ptr_v4u ? ptr_v4u : id++, var[HDR_PARAMS], pv[HDR_SETS], pl[HDR_SETS], xv = id++,
             sv = id++, res = id++, e[3], sum[3];
    for (int m = 0; m < HDR_SETS; m++) idx[m] = id++, pv[m] = id++, pl[m] = id++;
    for (int p = 0; p < HDR_PARAMS; p++) var[p] = id++;
    for (int c = 0; c < 3; c++) e[c] = id++, sum[c] = id++;
    put(&o, w, 5);
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (i == first_fn) {
            put(&o, g.w, (uint32_t)g.n);
            if (!ptr_v4u) PUT(&o, SPV_OP_TYPE_POINTER, ptr_v4, SPV_SC_UNIFORM, st_v4);
            for (int m = 0; m < HDR_SETS; m++) PUT(&o, SPV_OP_CONSTANT, it, idx[m], 11 + m);
        }
        if (i == st_at) {   // the support block gains HDR_SETS vec4 at 1440
            put(&o, w + i, len);
            o.w[o.n - len] = (len + HDR_SETS) << 16 | op;
            for (int m = 0; m < HDR_SETS; m++) put(&o, &st_v4, 1);
        } else if (i == dec_at) {
            put(&o, w + i, len);
            for (int m = 0; m < HDR_SETS; m++) PUT(&o, SPV_OP_MEMBER_DECORATE, st, 11 + m, SPV_DEC_OFFSET, 1440 + 16 * m);
        } else if (i == first_label) {
            put(&o, w + i, len);
            for (int p = 0; p < HDR_PARAMS; p++) PUT(&o, SPV_OP_VARIABLE, param_ptr[p], var[p], SPV_SC_FUNCTION);
        } else if (i == store_at[0] || i == store_at[1] || i == store_at[2]) {
            int c = i == store_at[0] ? 0 : i == store_at[1] ? 1 : 2;
            put(&o, w + i, len);
            o.w[o.n - len + 2] = e[c];
        } else {
            if (i == at) {
                for (int m = 0; m < HDR_SETS; m++) {
                    PUT(&o, SPV_OP_ACCESS_CHAIN, ptr_v4, pv[m], sb, idx[m]);
                    PUT(&o, SPV_OP_LOAD, st_v4, pl[m], pv[m]);
                }
                PUT(&o, SPV_OP_COMPOSITE_CONSTRUCT, v3, xv, site_x[chan_site[0]], site_x[chan_site[1]],
                    site_x[chan_site[2]]);
                for (int c = 0; c < 3; c++)
                    if (add[c]) {
                        PUT(&o, SPV_OP_FADD, ft, sum[c], exposed[c], add[c]);
                        exposed[c] = sum[c];
                    }
                PUT(&o, SPV_OP_COMPOSITE_CONSTRUCT, st_v4, sv, exposed[0], exposed[1], exposed[2], gain);
                PUT(&o, SPV_OP_STORE, var[0], xv);
                PUT(&o, SPV_OP_STORE, var[1], sv);
                for (int m = 0; m < HDR_SETS; m++) PUT(&o, SPV_OP_STORE, var[m + 2], pl[m]);
                uint32_t call[4 + HDR_PARAMS] = {(4u + HDR_PARAMS) << 16 | SPV_OP_FUNCTION_CALL, v3, res, map[fn]};
                memcpy(call + 4, var, sizeof var);
                put(&o, call, 4 + HDR_PARAMS);
                for (int c = 0; c < 3; c++) PUT(&o, SPV_OP_COMPOSITE_EXTRACT, ft, e[c], res, c);
            }
            put(&o, w + i, len);
        }
        i += len;
    }
    put(&o, f.w, (uint32_t)f.n);
    if (o.bad) goto lib_fail;
    o.w[3] = id;
    free(map), free(g.w), free(f.w), free(def), free(cval), free(cls), free(sval), free(scnt);
    free(*spirv);
    *spirv = o.w;
    *words = o.n;
    return exposure_found ? 1 : 2;
lib_fail:
    free(map), free(g.w), free(f.w), free(o.w);
fail:
    free(def), free(cval), free(cls), free(sval), free(scnt);
    return 0;
}
#undef PUT

void vk_hdr_shader_swap(uint32_t **spirv, uint64_t *words) {
    // armed, not wanted: a shader loaded while HDR is off must be ready for the menu switching it on
    if (!vk_hdr_armed() || vk_hdr_native() || !*spirv || *words < 5) return;
    int spliced = tonemap_splice(spirv, words);
    if (spliced) {
        static volatile int32_t count;
        rt_log("hdr: the game's tonemap maps its picture onto the display's range (%d shaders so far)%s",
               plat_inc32(&count), spliced == 2 ? "; its exposure was not found, so the tone mapper takes it as 1" : "");
    }
    int unclamped = fsr_unclamp(*spirv, words);
    if (unclamped > 0) {
        static volatile int32_t fsrs;
        rt_log("hdr: the game's FSR pass keeps values above 1 (%d output clamps; %d so far)", unclamped,
               plat_inc32(&fsrs));
    } else if (unclamped < 0) {
        static volatile int32_t missed;
        if (plat_inc32(&missed) == 1)
            rt_log("hdr: the game's FSR pass was found but no clamp of its output; it still clips HDR highlights");
    }
}


// ---- 2. the picture the window texture is copied from

int vk_hdr_track;                 // vk_exec_draw.c calls vk_hdr_draw: set at the first present with NX2NAVI_HDR=1
static volatile int32_t g_active;   // the swapchain is HDR10 (gfx_present.c vk_hdr_set_active)
static PlatMutex rec_lock = PLAT_MUTEX_INIT;
// per presented window image: what the last draw copied, and where in the stream (vk_exec_stream_pos)
static struct { VkImage win, src; VkFormat fmt; uint64_t pos; } recs[4];
static int nrecs;

void vk_hdr_set_active(int on) { plat_xchg32(&g_active, on); }
int vk_hdr_active(void) { return plat_cas32(&g_active, 0, 0); }

static int is_float(VkFormat f) {
    return f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_R16G16B16A16_SFLOAT ||
           f == VK_FORMAT_R32G32B32A32_SFLOAT;
}

void vk_hdr_draw(VkExec *ex, const VkPipeInfo *info) {
    const VkExecTarget *t = &ex->color[0];
    if (!t->image) return;
    int known = 0;
    for (int i = 0; i < 4; i++)   // unlocked first look: most draws go elsewhere (a stale slot only costs the lock below)
        known |= __atomic_load_n(&recs[i].win, __ATOMIC_RELAXED) == t->image;
    if (!known) return;
    // the last such draw of the stream decides, as in the replay, also when replay workers record draws at once
    uint64_t pos = vk_exec_stream_pos();
    VkImage src = VK_NULL_HANDLE;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    int ntex = 0, slot = -1;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *bd = &info->bindings[i];
        if (bd->kind != GFX_RES_TEX || bd->stage != GFX_STAGE_FS || !bd->res) continue;
        ntex += bd->count ? bd->count : 1;
        if ((bd->res->type & 0x7ff) == 3 && !bd->res->separate) slot = bd->res->slot;   // a plain 2D texture
    }
    if (ntex == 1 && slot >= 0 && slot < 128 && ex->tex[1][slot]) {
        const VkTexImage *s = vk_texture_by_id(NVN_HANDLE_TEX(ex->tex[1][slot]), NULL, NULL);
        if (s && is_float(s->format) && s->width == t->width && s->height == t->height && s->samples == 1)
            src = s->image, fmt = s->format;
    }
    plat_mutex_lock(&rec_lock);
    for (int i = 0; i < nrecs; i++)
        if (recs[i].win == t->image && pos >= recs[i].pos) recs[i].src = src, recs[i].fmt = fmt, recs[i].pos = pos;
    plat_mutex_unlock(&rec_lock);
}

void vk_hdr_present_source(const VkTexImage *win, VkImage *image, VkFormat *format) {
    if (!vk_hdr_wanted() || vk_hdr_native()) return;
    vk_hdr_track = 1;
    plat_mutex_lock(&rec_lock);
    int r = -1;
    for (int i = 0; i < nrecs; i++)
        if (recs[i].win == win->image) r = i;
    if (r < 0 && nrecs < 4) r = nrecs++, recs[r].win = win->image, recs[r].src = VK_NULL_HANDLE, recs[r].pos = 0;
    // kept until the next draw into the window texture: a present again without a new copy shows the same picture
    if (r >= 0 && recs[r].src && vk_hdr_active()) *image = recs[r].src, *format = recs[r].fmt;
    plat_mutex_unlock(&rec_lock);
    static int logged;
    if (!logged && *image != win->image) logged = 1, rt_log("hdr: the present reads the game's float picture");
}

void vk_hdr_image_gone(VkImage image) {   // a texture image went away: forget it as window or source
    if (!vk_hdr_track) return;   // no records before the first HDR present (vk_hdr_present_source): no lock per image
    plat_mutex_lock(&rec_lock);
    for (int i = 0; i < nrecs; i++) {
        if (recs[i].src == image) recs[i].src = VK_NULL_HANDLE;
        if (recs[i].win == image) recs[i] = recs[--nrecs], recs[nrecs] = (typeof(recs[0])){0}, i--;
    }
    plat_mutex_unlock(&rec_lock);
}

// ---- swapchain

// While NX2NAVI_HDR is on (vk_hdr_wanted), the HDR10 format among the surface's (10-bit UNORM, ST 2084): 1 with *out
// set, else 0 and the reason logged once.
int vk_hdr_surface_format(const VkSurfaceFormatKHR *f, uint32_t n, VkSurfaceFormatKHR *out) {
    if (!vk_hdr_wanted()) return 0;
    static int logged;
    if (!logged) {
        char b[1024];
        int o = 0;
        for (uint32_t i = 0; i < n && o < (int)sizeof b - 32; i++)
            o += snprintf(b + o, sizeof b - (size_t)o, " %d/%d", (int)f[i].format, (int)f[i].colorSpace);
        rt_log("hdr: surface formats (VkFormat/VkColorSpaceKHR):%s", b);
    }
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < n; i++)
            if (f[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
                f[i].format == (pass ? VK_FORMAT_A2R10G10B10_UNORM_PACK32 : VK_FORMAT_A2B10G10R10_UNORM_PACK32)) {
                *out = f[i];
                if (!logged) rt_log("hdr: HDR10 swapchain (format %d)", (int)f[i].format);
                logged = 1;
                return 1;
            }
    if (!logged)
        rt_log("hdr: the display offers no HDR10 format, so the picture stays SDR (Windows: turn on \"Use HDR\" in the "
               "display settings; Linux: a Wayland compositor with colour management and HDR on)");
    logged = 1;
    return 0;
}

// The display's peak and the picture's white for sc; again when the settings change (gfx_present.c calls it on every
// present to an HDR10 swapchain, with new = 1 for a new swapchain).
void vk_hdr_metadata(VkSwapchainKHR sc, int new_sc) {
    static unsigned sent = ~0u;
    VkCtx *c = vk_ctx();
    if (!c->has_hdr_metadata || (!new_sc && cfg_version() == sent)) return;
    sent = cfg_version();
    // AMD's frame generation swapchain takes it through its own call
    PFN_vkSetHdrMetadataEXT set = vk_fg_hdr_metadata_fn();
    if (!set) set = (PFN_vkSetHdrMetadataEXT)vk_dpa(c->device, "vkSetHdrMetadataEXT");
    if (!set) return;
    Nits n = nits();
    VkHdrMetadataEXT m = {.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT,
                          .displayPrimaryRed = {0.708f, 0.292f}, .displayPrimaryGreen = {0.170f, 0.797f},
                          .displayPrimaryBlue = {0.131f, 0.046f}, .whitePoint = {0.3127f, 0.3290f},
                          .maxLuminance = n.peak, .minLuminance = 0.0f, .maxContentLightLevel = n.peak,
                          .maxFrameAverageLightLevel = n.game};
    set(c->device, 1, &sc, &m);
}

// ---- 3. present passes

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 ss,
                    VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                               .dstStageMask = ds, .dstAccessMask = da, .oldLayout = from, .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

typedef struct { VkImage img; VmaAllocation alloc; VkImageView view; uint32_t w, h; } Img;

static void img_free(VkCtx *c, Img *m) {
    if (m->view) vkDestroyImageView(c->device, m->view, NULL);
    if (m->img) vmaDestroyImage(c->vma, m->img, m->alloc);
    *m = (Img){0};
}

// m at w x h in fmt; another size waits for the device first (earlier presents may still read the old image).
static int img_ensure(VkCtx *c, Img *m, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage) {
    if (m->img && m->w == w && m->h == h) return 1;
    if (m->img) {
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        img_free(c, m);
    }
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, &m->img, &m->alloc, NULL) != VK_SUCCESS) {
        m->img = VK_NULL_HANDLE;
        return 0;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = m->img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &m->view) != VK_SUCCESS) {
        m->view = VK_NULL_HANDLE;
        img_free(c, m);
        return 0;
    }
    m->w = w, m->h = h;
    return 1;
}

static struct {
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkSampler sampler;
    VkDescriptorSetLayout dsl_enc, dsl_ui;
    VkPipelineLayout enc_layout, ui_layout;
    VkPipeline enc;
    VkPipeline ui;                  // for ui_fmt
    VkFormat ui_fmt;
    Img out;                        // PQ code values of the crop, RGBA16F
    Img menu;                       // the in-game menu, RGBA8
    VkImageView src_view[SLOTS];
} H;

static VkShaderModule module(VkCtx *c, const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(c->device, &mi, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

static VkDescriptorSetLayout set_layout(VkCtx *c, const VkDescriptorSetLayoutBinding *b, uint32_t n) {
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                          .bindingCount = n, .pBindings = b};
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    return vkCreateDescriptorSetLayout(c->device, &ci, NULL, &l) == VK_SUCCESS ? l : VK_NULL_HANDLE;
}

static VkPipelineLayout pipe_layout(VkCtx *c, VkDescriptorSetLayout *dsl, VkShaderStageFlags st, uint32_t pc) {
    VkPushConstantRange pr = {st, 0, pc};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkPipelineLayout l = VK_NULL_HANDLE;
    return vkCreatePipelineLayout(c->device, &li, NULL, &l) == VK_SUCCESS ? l : VK_NULL_HANDLE;
}

static int init(VkCtx *c) {
    H.tried = 1;
    H.push = (PFN_vkCmdPushDescriptorSetKHR)vk_dpa(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding enc[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutBinding ui = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    if (!H.push || vkCreateSampler(c->device, &si, NULL, &H.sampler) != VK_SUCCESS ||
        !(H.dsl_enc = set_layout(c, enc, 2)) || !(H.dsl_ui = set_layout(c, &ui, 1)) ||
        !(H.enc_layout = pipe_layout(c, &H.dsl_enc, VK_SHADER_STAGE_COMPUTE_BIT, 32)) ||
        !(H.ui_layout = pipe_layout(c, &H.dsl_ui, VK_SHADER_STAGE_FRAGMENT_BIT, 16))) {
        rt_log("hdr: the present passes could not be created; the picture goes out unconverted");
        return 0;
    }
    VkShaderModule m = module(c, hdr_encode_spv, sizeof hdr_encode_spv);
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = H.enc_layout};
    VkResult r = m ? vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &H.enc) : VK_ERROR_UNKNOWN;
    if (m) vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) {
        rt_log("hdr: the present passes could not be created; the picture goes out unconverted");
        return 0;
    }
    return H.ok = 1;
}

// The game's own HDR picture: PQ codes in A2B10G10R10, the format of NVN's RGB10A2 (vk_formats.c); no guest format
// becomes A2R10G10B10.
int vk_hdr_native_picture(VkFormat f) { return vk_hdr_native() && f == VK_FORMAT_A2B10G10R10_UNORM_PACK32; }

VkImage vk_hdr_encode(VkCommandBuffer cb, unsigned slot, VkImage src, VkFormat fmt, const int crop[4]) {
    VkCtx *c = vk_ctx();
    // native HDR: the game's picture holds PQ codes already; the present scales it into the swapchain as it is
    if (vk_hdr_native_picture(fmt)) {
        static int told;
        if (!told++) rt_log("hdr: native: the game's own HDR10 picture goes to the swapchain as it is");
        return VK_NULL_HANDLE;
    }
    if (!H.tried) init(c);
    if (!H.ok || !img_ensure(c, &H.out, (uint32_t)crop[2], (uint32_t)crop[3], VK_FORMAT_R16G16B16A16_SFLOAT,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        return VK_NULL_HANDLE;
    slot %= SLOTS;
    if (H.src_view[slot]) vkDestroyImageView(c->device, H.src_view[slot], NULL);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = src,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &H.src_view[slot]) != VK_SUCCESS) {
        H.src_view[slot] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    Nits n = nits();
    // an sRGB format decodes itself through the view; 8-bit UNORM holds sRGB codes the shader decodes
    int decode = fmt == VK_FORMAT_R8G8B8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_UNORM;
    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, CS,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(cb, H.out.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, ALL, 0, CS,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    VkDescriptorImageInfo ii = {H.sampler, H.src_view[slot], VK_IMAGE_LAYOUT_GENERAL},
                          oi = {VK_NULL_HANDLE, H.out.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, H.enc);
    rec_push(H.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, H.enc_layout, 0, 2, w);
    struct { int32_t offset[2], size[2]; float p[4]; } pc = {{crop[0], crop[1]}, {crop[2], crop[3]},
                                                            {n.ui, n.peak, decode ? 1.0f : 0.0f, n.gamma}};
    vkCmdPushConstants(cb, H.enc_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, ((uint32_t)crop[2] + 7) / 8, ((uint32_t)crop[3] + 7) / 8, 1);
    barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, CS, 0, ALL, 0);
    barrier(cb, H.out.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, ALL,
            VK_ACCESS_2_MEMORY_READ_BIT);
    return H.out.img;
}

// The in-game menu's target: an RGBA8 image of the swapchain's size in COLOR_ATTACHMENT_OPTIMAL (its old content
// dropped); the caller renders the menu into the view with a clear to 0.
VkImageView vk_hdr_ui_target(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    VkCtx *c = vk_ctx();
    if (!H.tried) init(c);
    if (!H.ok || !img_ensure(c, &H.menu, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
        return VK_NULL_HANDLE;
    barrier(cb, H.menu.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    return H.menu.view;
}

static int ui_pipeline(VkCtx *c, VkFormat fmt) {
    if (H.ui && H.ui_fmt == fmt) return 1;
    if (H.ui) {   // another swapchain format: earlier presents may still use the old pipeline
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        vkDestroyPipeline(c->device, H.ui, NULL);
        H.ui = VK_NULL_HANDLE;
    }
    VkShaderModule vs = module(c, hdr_fullscreen_spv, sizeof hdr_fullscreen_spv),
                   fs = module(c, hdr_ui_spv, sizeof hdr_ui_spv);
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs,
         .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {
        .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt};
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds,
                                       .layout = H.ui_layout};
    VkResult r = vs && fs ? vkCreateGraphicsPipelines(c->device, VK_NULL_HANDLE, 1, &gi, NULL, &H.ui) : VK_ERROR_UNKNOWN;
    if (vs) vkDestroyShaderModule(c->device, vs, NULL);
    if (fs) vkDestroyShaderModule(c->device, fs, NULL);
    if (r != VK_SUCCESS) {
        H.ui = VK_NULL_HANDLE;
        rt_log("hdr: the menu pass could not be created; the menu is not shown");
        return 0;
    }
    H.ui_fmt = fmt;
    return 1;
}

// The menu image onto dst (a swapchain image in COLOR_ATTACHMENT_OPTIMAL, view dst_view of format fmt).
void vk_hdr_ui_compose(VkCommandBuffer cb, VkImageView dst_view, VkFormat fmt, uint32_t w, uint32_t h) {
    VkCtx *c = vk_ctx();
    if (!H.ok || !H.menu.img || !ui_pipeline(c, fmt)) return;
    barrier(cb, H.menu.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dst_view,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, H.ui);
    VkViewport v = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D s = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &s);
    VkDescriptorImageInfo ii = {H.sampler, H.menu.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    rec_push(H.push, cb, VK_PIPELINE_BIND_POINT_GRAPHICS, H.ui_layout, 0, 1, &wr);
    Nits n = nits();
    float pc[4] = {n.ui, n.gamma, 0, 0};
    vkCmdPushConstants(cb, H.ui_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
}

void vk_hdr_release(void) {
    VkCtx *c = vk_ctx();
    for (int i = 0; i < SLOTS; i++)
        if (H.src_view[i]) vkDestroyImageView(c->device, H.src_view[i], NULL), H.src_view[i] = VK_NULL_HANDLE;
    img_free(c, &H.out);
    img_free(c, &H.menu);
    if (H.enc) vkDestroyPipeline(c->device, H.enc, NULL);
    if (H.ui) vkDestroyPipeline(c->device, H.ui, NULL);
    if (H.enc_layout) vkDestroyPipelineLayout(c->device, H.enc_layout, NULL);
    if (H.ui_layout) vkDestroyPipelineLayout(c->device, H.ui_layout, NULL);
    if (H.dsl_enc) vkDestroyDescriptorSetLayout(c->device, H.dsl_enc, NULL);
    if (H.dsl_ui) vkDestroyDescriptorSetLayout(c->device, H.dsl_ui, NULL);
    if (H.sampler) vkDestroySampler(c->device, H.sampler, NULL);
    memset(&H, 0, sizeof H);
}

// Luminance of a PQ code (0..1023 over 1) in nits.
static float pq_nits(float e) {
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    float p = powf(e, 1.0f / m2);
    return 10000.0f * powf(fmaxf(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

// 10-bit PQ pixels of an HDR10 swapchain to bytes that show on a display of gamma 2.2 what the HDR display shows, the
// HUD's white at 255 (menu test shots and flight thumbnails): a dark tone the HDR display lifts looks lifted here too.
void vk_hdr_to_srgb8(const uint8_t *in, uint8_t *out, size_t px, int a2r) {
    float ui = nits().ui;
    // Rec. 2020 to Rec. 709 (the inverse of ITU-R BT.2087's matrix)
    static const float m[9] = {1.6605f, -0.5876f, -0.0728f, -0.1246f, 1.1329f, -0.0083f, -0.0182f, -0.1006f, 1.1187f};
    for (size_t i = 0; i < px; i++) {
        uint32_t v;
        memcpy(&v, in + 4 * i, 4);
        float c[3] = {(float)(v & 1023), (float)(v >> 10 & 1023), (float)(v >> 20 & 1023)};   // A2B10G10R10: R low
        if (a2r) { float t = c[0]; c[0] = c[2]; c[2] = t; }
        for (int k = 0; k < 3; k++) c[k] = pq_nits(c[k] / 1023.0f) / ui;
        for (int k = 0; k < 3; k++) {
            float l = fminf(fmaxf(m[3 * k] * c[0] + m[3 * k + 1] * c[1] + m[3 * k + 2] * c[2], 0.0f), 1.0f);
            out[4 * i + k] = (uint8_t)(powf(l, 1.0f / 2.2f) * 255.0f + 0.5f);
        }
        out[4 * i + 3] = 255;
    }
}

// The luminance (Rec. 2020 Y, in nits) of the 10-bit PQ pixels inside the rectangle {x, y, w, h} of a stride-wide
// image: out = the darkest pixel's, the 0.1st, 1st, 50th and 99th percentiles, the brightest's, and the share of pixels
// under 0.01 nits. The percentiles come from a histogram of 64 bins a stop, from 2^-24 to 2^14 nits; a value is a bin's
// upper edge.
void vk_hdr_luminance_stats(const uint8_t *in, int stride, const int rect[4], int a2r, float out[7]) {
    enum { LO = -24, HI = 14, PER = 64, BINS = (HI - LO) * PER };
    static uint32_t hist[BINS + 1];
    static float lut[1024];
    for (int i = 0; i < 1024; i++) lut[i] = pq_nits((float)i / 1023.0f);
    memset(hist, 0, sizeof hist);
    float lo = 1e30f, hi = 0.0f;
    uint64_t total = 0, dark = 0;
    for (int y = rect[1]; y < rect[1] + rect[3]; y++)
        for (int x = rect[0]; x < rect[0] + rect[2]; x++) {
            uint32_t v;
            memcpy(&v, in + 4 * ((size_t)y * (size_t)stride + (size_t)x), 4);
            float r = lut[v & 1023], g = lut[v >> 10 & 1023], b = lut[v >> 20 & 1023];
            if (a2r) { float t = r; r = b; b = t; }
            float l = 0.2627002120f * r + 0.6779980715f * g + 0.0593017165f * b;
            lo = fminf(lo, l), hi = fmaxf(hi, l);
            dark += l < 0.01f;
            int bin = l > 0.0f ? (int)floorf((log2f(l) - LO) * PER) + 1 : 0;   // bin 0: exactly black
            hist[bin < 0 ? 0 : bin > BINS ? BINS : bin]++;
            total++;
        }
    static const double q[4] = {0.001, 0.01, 0.5, 0.99};
    for (int k = 0; k < 4; k++) {
        uint64_t want = (uint64_t)(q[k] * (double)total), seen = 0;
        int bin = 0;
        while (bin < BINS && seen + hist[bin] <= want) seen += hist[bin++];
        out[1 + k] = bin == 0 ? 0.0f : exp2f((float)LO + (float)bin / PER);
    }
    out[0] = total ? lo : 0.0f, out[5] = hi, out[6] = total ? (float)((double)dark / (double)total) : 0.0f;
}
