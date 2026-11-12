// The water corrections of docs/water-projective.md as edits of a SPIR-V module in memory (spirv_water.c); the same
// edits tools/projective_recipe.py makes on files. vk_pipeline.c applies them to the exact translations it knows.
#pragma once
#include <stdint.h>

// Each returns a new module (free it) with its word count in *out_n, or NULL when the module is not of the shape the
// edit expects; the caller then keeps the original.
// vertex: the location 0 output gets (clip x, clip y, clip w, 1) of the position the stage wrote, before its return.
uint32_t *spirv_water_vertex(const uint32_t *w, uint64_t n, uint64_t *out_n);
// fragment: the location 0 input loses NoPerspective and is read through a local holding (x / w * 0.5 + 0.5,
// y / w * -0.5 + 0.5, 0, 1), which is what the vertex stage wrote there before, now divided per fragment.
uint32_t *spirv_water_fragment(const uint32_t *w, uint64_t n, uint64_t *out_n);
// absorption: the store of id `distance` through id `pointer` stores NMax(distance, 0) instead.
uint32_t *spirv_water_absorption(const uint32_t *w, uint64_t n, uint32_t pointer, uint32_t distance, uint64_t *out_n);
