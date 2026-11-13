// Index lists for the NVN primitives Vulkan has no topology for, made from an indexed draw's own indices
// (vk_exec_draw.c op_draw_idx, NX2NAVI_QUAD_INDEX). Header only, so that tests/index_rewrite_check.c checks it without a
// device.
//
// The replay draws a quad list as a triangle list and a line loop as a line strip (vk_pipeline.c vk_pipeline_topology).
// For a draw without indices it makes the indices itself (op_draw); an indexed one went to the GPU with the game's
// indices as they are: a quad list read three indices at a time, which joins corners of neighbouring quads into
// triangles across the mesh, and a line loop without its closing line. These turn the game's indices into the list the
// Vulkan topology wants: two triangles (0, 1, 2) and (0, 2, 3) per quad, as the non-indexed rewrite makes them, and the
// loop's first index again at its end. Indices are read at their size (1, 2 or 4 bytes, little-endian) and written as
// 32-bit ones; a base vertex stays the draw's.
#pragma once
#include <stdint.h>
#include <string.h>

static inline uint32_t idx_read(const uint8_t *src, uint32_t unit, uint32_t i) {
    if (unit == 1) return src[i];
    if (unit == 2) {
        uint16_t v;
        memcpy(&v, src + 2 * (size_t)i, 2);
        return v;
    }
    uint32_t v;
    memcpy(&v, src + 4 * (size_t)i, 4);
    return v;
}

// Indices of a quad list as triangles: out holds 6 * (count / 4); a trailing partial quad is left out, as NVN draws no
// incomplete primitive. Returns how many were written.
static inline uint32_t idx_quads(const uint8_t *src, uint32_t unit, uint32_t count, uint32_t *out) {
    static const uint8_t k[6] = {0, 1, 2, 0, 2, 3};
    uint32_t q = count / 4;
    for (uint32_t i = 0; i < q; i++)
        for (int j = 0; j < 6; j++) out[6 * i + (uint32_t)j] = idx_read(src, unit, 4 * i + k[j]);
    return 6 * q;
}

// Indices of a line loop as a strip that comes back to its first vertex: out holds count + 1 (count >= 2). Returns how
// many were written.
static inline uint32_t idx_loop(const uint8_t *src, uint32_t unit, uint32_t count, uint32_t *out) {
    for (uint32_t i = 0; i < count; i++) out[i] = idx_read(src, unit, i);
    out[count] = out[0];
    return count + 1;
}
