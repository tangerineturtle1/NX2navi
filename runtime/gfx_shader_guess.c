// Bind-state answers for a prepared default variant, read from its first translation (gfx_shader.c prep_worker, for
// seed records tools/prepare_shaders.py flags as default variants). A default variant guesses the state play binds:
// every texture a 2D one, float vertex inputs and colour outputs. Against the 3,093 (program, answers) pairs a play
// session of Ocarina recorded, that guess matched 89.5%: vertex 77.3%, compute 88.1%, fragment 99.6%. With two readings
// of the first translation, one more translation for the programs they change, --prepare-shaders matched 98.3% of them
// (vertex 97.1%, compute 96.0%, fragment 99.6%; last-frame vertex variants, vk_mvprev.c, are not prepared):
//
// - Vertex inputs. A vertex attribute without an answer is declared float, and a shader that reads it as an integer
//   then bitcasts the loaded value to an integer type. Such inputs are answered uint: of the 313 integer inputs met
//   in play, all were uint and all were bitcast, against 36 of 3,339 float ones.
// - Compute textures and images. Translated without texture answers (the seed gives a compute default variant none),
//   the translator declares each texture and image with the type its instructions name; those types become the
//   answers, image formats included, and a slot it queries without declaring one keeps the 2D default. Compute misses
//   dropped from 43 to 13. A compute shader play meets untranslated is translated and its pipeline built on the replay
//   thread (vk_exec_draw.c, the dispatch's shader_for call), so each miss is a stall; the prepare step's driver builds
//   of compute pipelines averaged 0.96 s each, 16 at a time, on an RTX 5090.
#include <stdlib.h>
#include <string.h>

#include "gfx.h"

enum { OP_TYPE_INT = 21, OP_TYPE_VECTOR = 23, OP_VARIABLE = 59, OP_LOAD = 61, OP_ACCESS_CHAIN = 65,
       OP_IN_BOUNDS_ACCESS_CHAIN = 66, OP_DECORATE = 71, OP_BITCAST = 124 };
enum { DEC_LOCATION = 30, SC_INPUT = 1 };
#define ID_CAP (1u << 20)

// Input Locations (bit mask) whose loaded value some OpBitcast turns into an integer scalar or vector.
static uint32_t int_read_inputs(const uint32_t *w, uint64_t n) {
    if (n < 5) return 0;
    uint32_t bound = w[3] < ID_CAP ? w[3] : ID_CAP, found = 0;
    int8_t *loc = malloc(bound);                           // per id: the input Location it reads, -1 none
    uint8_t *is_int = calloc(bound, 1), *located = calloc(bound, 1);
    int8_t *dec = malloc(bound);                           // Location decorations
    if (!loc || !is_int || !located || !dec) goto out;
    memset(loc, -1, bound), memset(dec, -1, bound);
    for (uint64_t i = 5; i < n;) {   // declarations come before the code that uses them
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) break;
        const uint32_t *a = w + i + 1;
        if (op == OP_DECORATE && len >= 4 && a[0] < bound && a[1] == DEC_LOCATION && a[2] < 32) dec[a[0]] = (int8_t)a[2];
        else if (op == OP_TYPE_INT && len >= 2 && a[0] < bound) is_int[a[0]] = 1;
        else if (op == OP_TYPE_VECTOR && len >= 3 && a[0] < bound && a[1] < bound) is_int[a[0]] = is_int[a[1]];
        else if (op == OP_VARIABLE && len >= 4 && a[1] < bound && a[2] == SC_INPUT && dec[a[1]] >= 0)
            loc[a[1]] = dec[a[1]], located[a[1]] = 1;
        else if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && len >= 4 && a[1] < bound && a[2] < bound &&
                 located[a[2]])
            loc[a[1]] = loc[a[2]], located[a[1]] = 1;
        else if (op == OP_LOAD && len >= 4 && a[1] < bound && a[2] < bound && located[a[2]])
            loc[a[1]] = loc[a[2]];   // a loaded value: not a pointer, so chains do not continue from it
        else if (op == OP_BITCAST && len >= 4 && a[0] < bound && a[2] < bound && is_int[a[0]] && !located[a[2]] &&
                 loc[a[2]] >= 0)
            found |= 1u << loc[a[2]];
        i += len;
    }
out:
    free(loc), free(is_int), free(located), free(dec);
    return found;
}

// The translator's SamplerType (GfxShaderRes.type) as a GfxShaderAnswers tex_type / img_type code; 0 unknown.
static uint8_t answer_type(uint16_t t) {
    int arr = t & 0x100, ms = t & 0x200;
    switch (t & 0xff) {
    case 1: return arr ? GFX_TEX_1D_ARRAY : GFX_TEX_1D;
    case 2: return GFX_TEX_BUFFER;
    case 3: return ms ? (arr ? GFX_TEX_2D_MS_ARRAY : GFX_TEX_2D_MS) : arr ? GFX_TEX_2D_ARRAY : GFX_TEX_2D;
    case 4: return GFX_TEX_3D;
    case 5: return arr ? GFX_TEX_CUBE_ARRAY : GFX_TEX_CUBE;
    default: return 0;
    }
}

int gfx_shader_guess_refine(const GfxShader *s, int stage, const GfxShaderAnswers *a, GfxShaderAnswers *out) {
    *out = *a;
    if (!s) return 0;
    if (stage == GFX_STAGE_VS) {
        uint64_t words = 0;
        const uint32_t *w = gfx_shader_spirv_pin(s, &words);
        GfxShaderIo io[32];
        uint32_t ints = w ? int_read_inputs(w, words) & gfx_shader_inputs(s, io) : 0;
        gfx_shader_spirv_unpin(s);
        for (int l = 0; l < 32; l++)
            if (ints >> l & 1 && out->attr_type[l] == GFX_TYPE_FLOAT) out->attr_type[l] = GFX_TYPE_UINT;
    } else if (stage == GFX_STAGE_CS) {
        // a slot the translation queries without declaring a texture there (a bindless one) keeps the 2D default
        for (int l = 0; l < 32; l++)
            if (!out->tex_type[l]) out->tex_type[l] = GFX_TEX_2D;
        const GfxShaderRes *r;
        int n = gfx_shader_resources(s, GFX_RES_TEX, &r);
        for (int i = 0; i < n; i++)
            if (r[i].slot >= 0 && r[i].slot < 32 && answer_type(r[i].type)) out->tex_type[r[i].slot] = answer_type(r[i].type);
        n = gfx_shader_resources(s, GFX_RES_IMG, &r);
        for (int i = 0; i < n; i++)
            if (r[i].slot >= 0 && r[i].slot < 32 && answer_type(r[i].type)) {
                out->img_type[r[i].slot] = answer_type(r[i].type);
                if (r[i].format) out->img_format[r[i].slot] = r[i].format;
            }
    }
    return memcmp(out, a, sizeof *a) != 0;
}
