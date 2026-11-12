// The layout of shader-prepared.bin, the list of shader variants a preparation translated and built ahead of play
// (runtime/gfx_shader.c gfx_shader_prepare_seed writes it; gfx_shader_warm_list and runtime/vk_pipeline.c lw_start,
// lb_warm_prepared read it; tools/make_game.py prepared_from reads its head).
//
//   "NX2NSPRP", version (u32), record size (u32, 36)
//   version 2 only: what the list was prepared from, 96 bytes: the sha256 of seed-programs.bin, the sha256 of
//     seed-answers.bin (each over the whole file), and the translator identity (the 32 bytes of nx2navi_xlate.id,
//     before the host options are mixed in)
//   records: stage, priority, 2 zero bytes, input id (32 bytes); in priority order, one per input id
//
// A version 1 file has no such block: it does not say what it was prepared from. The seed decides which variants are in
// the list, and nothing else recorded it: a build that came with another seed (more shaders listed, another pipeline
// list) kept the old list, and the variants only the new seed names were translated when play first drew them. With
// the block the build compares it with the seed it has just written and prepares again when they differ
// (tools/make_game.py step_shaders). The translator identity is recorded for the same comparison; a list prepared by
// another translator is not discarded for that alone, since input ids do not depend on the translator and the game
// carries the translations over (gfx_shader.c, "Carry").
// tests/prp_file_check.py checks this header against the reader in tools/make_game.py.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PRP_MAGIC "NX2NSPRP"
#define PRP_VERSION 2
#define PRP_REC 36
#define PRP_HEAD 16      // magic, version, record size
#define PRP_FROM_SIZE 96

typedef struct {
    uint8_t programs[32];     // sha256 of seed-programs.bin
    uint8_t answers[32];      // sha256 of seed-answers.bin
    uint8_t translator[32];   // the translator identity
} PrpFrom;

// The head of a version 2 file into out (PRP_HEAD + PRP_FROM_SIZE bytes); returns that size.
static inline size_t prp_head_write(uint8_t *out, const PrpFrom *from) {
    uint32_t v = PRP_VERSION, r = PRP_REC;
    memcpy(out, PRP_MAGIC, 8), memcpy(out + 8, &v, 4), memcpy(out + 12, &r, 4);
    memcpy(out + PRP_HEAD, from->programs, 32), memcpy(out + PRP_HEAD + 32, from->answers, 32);
    memcpy(out + PRP_HEAD + 64, from->translator, 32);
    return PRP_HEAD + PRP_FROM_SIZE;
}

// The records of the file f of size bytes: a pointer into f and their count in *n; NULL for a file that is no prepared
// list or has no whole record. *has_from: 1 with *from filled for a version 2 file, 0 for a version 1 file.
static inline const uint8_t *prp_parse(const uint8_t *f, uint64_t size, size_t *n, PrpFrom *from, int *has_from) {
    uint32_t v = 0, r = 0;
    *n = 0, *has_from = 0;
    if (!f || size < PRP_HEAD || memcmp(f, PRP_MAGIC, 8)) return NULL;
    memcpy(&v, f + 8, 4), memcpy(&r, f + 12, 4);
    if (r != PRP_REC || (v != 1 && v != 2)) return NULL;
    uint64_t at = PRP_HEAD;
    if (v == 2) {
        if (size < PRP_HEAD + PRP_FROM_SIZE) return NULL;
        memcpy(from->programs, f + PRP_HEAD, 32), memcpy(from->answers, f + PRP_HEAD + 32, 32);
        memcpy(from->translator, f + PRP_HEAD + 64, 32);
        *has_from = 1, at += PRP_FROM_SIZE;
    }
    *n = (size_t)((size - at) / PRP_REC);
    return *n ? f + at : NULL;
}

// runtime/gfx_shader.c: the records of the prepared list at path (PRP_REC bytes each, malloc'd; their count in *n),
// NULL without any. *from, *has_from (both optional): what a version 2 file says it was prepared from.
uint8_t *gfx_shader_prepared_read(const char *path, size_t *n, PrpFrom *from, int *has_from);
