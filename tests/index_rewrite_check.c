// runtime/vk_index_rewrite.h: the index lists an indexed quad list or line loop is drawn with (vk_exec_draw.c
// op_draw_idx, NX2NAVI_QUAD_INDEX). Built and run by tests/index_rewrite_check.py; prints each failed check and exits 1
// on any.
#include <stdio.h>
#include <string.h>

#include "vk_index_rewrite.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    uint32_t out[64];
    // ---- 16-bit quads: two triangles per quad, from the quad's own indices, the same split as the non-indexed rewrite
    uint16_t q16[8] = {10, 11, 12, 13, 40, 41, 42, 43};
    memset(out, 0xff, sizeof out);
    uint32_t n = idx_quads((const uint8_t *)q16, 2, 8, out);
    static const uint32_t want16[12] = {10, 11, 12, 10, 12, 13, 40, 41, 42, 40, 42, 43};
    CHECK(n == 12 && !memcmp(out, want16, sizeof want16), "two 16-bit quads made %u indices, or the wrong ones", n);
    CHECK(out[12] == 0xffffffffu, "the quads wrote past their 12 indices");
    // ---- a trailing partial quad is not drawn
    n = idx_quads((const uint8_t *)q16, 2, 7, out);
    CHECK(n == 6, "seven indices made %u, not one quad's 6", n);
    n = idx_quads((const uint8_t *)q16, 2, 3, out);
    CHECK(n == 0, "three indices made %u, not none", n);
    // ---- byte and 32-bit indices are read at their own size
    uint8_t q8[4] = {200, 3, 255, 7};
    n = idx_quads(q8, 1, 4, out);
    static const uint32_t want8[6] = {200, 3, 255, 200, 255, 7};
    CHECK(n == 6 && !memcmp(out, want8, sizeof want8), "byte quad read wrong");
    uint32_t q32[4] = {70000, 70001, 0x01000002u, 70003};
    n = idx_quads((const uint8_t *)q32, 4, 4, out);
    static const uint32_t want32[6] = {70000, 70001, 0x01000002u, 70000, 0x01000002u, 70003};
    CHECK(n == 6 && !memcmp(out, want32, sizeof want32), "32-bit quad read wrong");
    // ---- unaligned source (a byte offset into a pool): read without alignment assumptions
    uint8_t raw[1 + 8];
    uint16_t v[4] = {5, 6, 7, 8};
    memcpy(raw + 1, v, 8);
    n = idx_quads(raw + 1, 2, 4, out);
    static const uint32_t wantu[6] = {5, 6, 7, 5, 7, 8};
    CHECK(n == 6 && !memcmp(out, wantu, sizeof wantu), "unaligned 16-bit quad read wrong");
    // ---- a line loop comes back to its first index
    uint16_t l16[3] = {9, 4, 2};
    n = idx_loop((const uint8_t *)l16, 2, 3, out);
    static const uint32_t wantl[4] = {9, 4, 2, 9};
    CHECK(n == 4 && !memcmp(out, wantl, sizeof wantl), "the line loop was not closed");
    uint8_t l8[2] = {1, 250};
    n = idx_loop(l8, 1, 2, out);
    CHECK(n == 3 && out[0] == 1 && out[1] == 250 && out[2] == 1, "the two-index loop was not closed");
    if (!fails) printf("index_rewrite_check: all checks passed\n");
    return fails ? 1 : 0;
}
