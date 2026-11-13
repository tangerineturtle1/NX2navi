#pragma once

#include <stdint.h>

// Returns an allocated optimized module, or NULL to keep the input. The caller frees the returned module.
uint32_t *vk_spirv_optimize(const uint32_t *input, uint64_t words, uint64_t *output_words);
