// tests/tex_copy_mt_check.py builds this with runtime/tex_deswizzle.c, runtime/par_jobs.c and the platform layer:
// tex_bl_copy_mt and par_memcpy write exactly the bytes tex_bl_copy and memcpy do, in both directions, for levels
// under and over the split size, with partial GOBs, block heights and depths, and slices.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "par_jobs.h"
#include "tex.h"

// what tex_deswizzle.c and par_jobs.c need from the rest of the runtime
const int rt_nvn_api_version[2] = {200, 0};
int cfg_bool(const char *name) { (void)name; return 1; }
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
void nvn_tex_layout(const nvn_texdesc *d, nvn_texlayout *o) { (void)d, (void)o; }
uint64_t nvn_tex_level_offset(const nvn_texdesc *d, unsigned level) { (void)d, (void)level; return 0; }
int nvn_fmt_bpb(int format, int *bw, int *bh) { (void)format, *bw = *bh = 1; return 4; }
void nvn_tex_level_tiles(const nvn_texdesc *d, unsigned level, unsigned *tileH, unsigned *tileD) {
    (void)d, (void)level, *tileH = *tileD = 0;
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint8_t rnd8(void) {
    rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
    return (uint8_t)(rng >> 56);
}

static int fails;

static void one(uint32_t row_bytes, uint32_t rows, uint32_t slices, unsigned th, unsigned td) {
    uint32_t bh = 1u << th, bd = 1u << td;
    uint64_t gobs_x = (row_bytes + 63) / 64, blocks_y = (rows + 8 * bh - 1) / (8 * bh), blocks_z = (slices + bd - 1) / bd;
    size_t bl_size = (size_t)(gobs_x * blocks_y * blocks_z * 512ull * bh * bd), pitch = row_bytes + 32;
    size_t lin_size = pitch * rows * slices;
    uint8_t *bl = malloc(bl_size), *a = malloc(lin_size), *b = malloc(lin_size), *bl2 = malloc(bl_size), *bl3 = malloc(bl_size);
    for (size_t i = 0; i < bl_size; i++) bl[i] = rnd8();
    memset(a, 0xa5, lin_size), memset(b, 0xa5, lin_size);
    tex_bl_copy(bl, a, pitch, row_bytes, rows, slices, th, td, 1);
    tex_bl_copy_mt(bl, b, pitch, row_bytes, rows, slices, th, td, 1);
    if (memcmp(a, b, lin_size)) printf("FAIL to linear %ux%ux%u th %u td %u\n", row_bytes, rows, slices, th, td), fails++;
    memset(bl2, 0x5a, bl_size), memset(bl3, 0x5a, bl_size);
    tex_bl_copy(bl2, a, pitch, row_bytes, rows, slices, th, td, 0);
    tex_bl_copy_mt(bl3, a, pitch, row_bytes, rows, slices, th, td, 0);
    if (memcmp(bl2, bl3, bl_size)) printf("FAIL to block-linear %ux%ux%u th %u td %u\n", row_bytes, rows, slices, th, td), fails++;
    free(bl), free(a), free(b), free(bl2), free(bl3);
}

int main(void) {
    printf("pool: %d threads\n", par_threads());
    one(4096, 1024, 1, 4, 0);   // 4 MiB, split
    one(1000, 1300, 1, 3, 0);   // partial GOBs, split (1.2 MiB)
    one(1000, 700, 1, 3, 0);    // partial GOBs, under the split size
    one(16384, 512, 1, 5, 0);   // 8 MiB, wide
    one(256, 64, 8, 1, 2);      // a 3D level under the split size
    one(2048, 128, 16, 2, 1);   // 3D, split
    one(64, 8, 1, 0, 0);        // one GOB
    one(48, 3, 1, 0, 0);        // less than a GOB
    for (size_t n = 0; n < (8u << 20); n = n * 3 + 1001) {
        uint8_t *s = malloc(n + 1), *d1 = malloc(n + 1), *d2 = malloc(n + 1);
        for (size_t i = 0; i < n; i++) s[i] = rnd8();
        memcpy(d1, s, n);
        par_memcpy(d2, s, n);
        if (memcmp(d1, d2, n)) printf("FAIL par_memcpy of %zu bytes\n", n), fails++;
        free(s), free(d1), free(d2);
    }
    if (fails) return 1;
    printf("tex_copy_mt_check: all checks passed\n");
    return 0;
}
