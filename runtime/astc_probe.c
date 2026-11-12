// --probe-astc: the compute ASTC decoder (vk_astc.c, shaders/astc_decode.comp) against astc_decode_ldr, bit for bit,
// for every block footprint, linear and sRGB. Synthetic blocks only (no game files): valid void extents, invalid and
// HDR void extents, random blocks kept by what they exercise (1-4 partitions, dual plane with 1-3 partitions, every
// weight grid and quantization the random modes reach), then plain random blocks (mostly error blocks, HDR endpoint
// modes among them). Each case decodes two regions into two mip levels of an RGBA8 image with cropped edges, and
// compares both the decoder's texels (readback) and what the image holds afterwards. The same regions also go through
// the BC3 encoder (NX2NAVI_ASTC_BC) into a BC3 image, whose blocks are decoded here and held to error bounds (bc_check).
// Then cut-out foliage at every footprint (cutout_case): full mip chains of transparent and opaque patches with sharp
// alpha edges, through the encoder, where alpha 0 and 255 must stay exact and alpha tests keep their outline (cut_check).
// Every block of both is also put to tex_astc.c astc_block_opaque, which must say what a decode says (opaque_check),
// and with NX2NAVI_ASTC_BC1 the regions are encoded to BC1 as well: each BC1 block must be the colour half of the BC3
// block made from the same texels (bc1_check), the four-colour block BC1 decodes alike.
// Also times one 2048x2048 texture on the CPU (astc_decode_ldr_mt) and on the GPU (wall time of the upload batch).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "platform/platform.h"
#include "host.h"
#include "tex.h"
#include "vk.h"

static uint64_t g_rng = 0x2545F4914F6CDD1Dull;
static uint64_t rnd64(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return g_rng;
}

static const uint8_t dims[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                    {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};

// Buckets of valid blocks: partitions 1-4 single plane, then dual plane with 1-3 partitions.
enum { NB = 7, PER_BUCKET = 48 };
static const char *const bucket_name[NB] = {"1p", "2p", "3p", "4p", "dual 1p", "dual 2p", "dual 3p"};
static uint64_t g_bucket[NB], g_void, g_err, g_blocks, g_texels, g_bc_cases;
static double g_bc_db = 99;   // the lowest BC3 PSNR of all cases

static int dual_bit(uint32_t m) {
    if (!(m & 3) && (m >> 7 & 3) == 2) return 0;   // the 6..10 x 6..10 modes have no dual plane
    return (int)(m >> 10 & 1);
}

static void void_block(uint8_t b[16], int kind) {
    // kind 0: valid LDR extent (random coordinates s0 < s1, t0 < t1), 1: all-ones extent, 2: s0 >= s1 (error),
    // 3: HDR (error), 4: reserved bits not 11 (error)
    uint64_t lo = 0x1FC | 3ull << 10, s0 = rnd64() & 0xFFF, s1 = s0 + 1 + (rnd64() & 0xFFF), t0 = rnd64() & 0xFFF,
             t1 = t0 + 1 + (rnd64() & 0xFFF);
    if (s1 > 0x1FFF) s1 = 0x1FFF;
    if (t1 > 0x1FFF) t1 = 0x1FFF;
    if (kind == 1) s0 = s1 = t0 = t1 = 0x1FFF;
    if (kind == 2) { uint64_t x = s0; s0 = s1; s1 = x; }
    lo |= s0 << 12 | s1 << 25 | t0 << 38 | t1 << 51;
    if (kind == 3) lo |= 0x200;
    if (kind == 4) lo &= ~(1ull << 11);
    uint64_t hi = rnd64();
    memcpy(b, &lo, 8), memcpy(b + 8, &hi, 8);
}

// n blocks for footprint bw x bh: the buckets first, void extents, then random.
static void make_blocks(uint8_t *blk, size_t n, uint32_t bw, uint32_t bh) {
    size_t k = 0;
    uint8_t tmp[12 * 12 * 4];
    int have[NB] = {0};
    for (int tries = 0; tries < 400000 && k < n; tries++) {
        uint64_t v[2] = {rnd64(), rnd64()};
        uint8_t *b = (uint8_t *)v;
        uint32_t m = (uint32_t)(v[0] & 0x7FF);
        if ((m & 0x1FF) == 0x1FC || !astc_decode_block(b, bw, bh, tries & 1, tmp)) continue;
        int p = (int)(v[0] >> 11 & 3), bk = dual_bit(m) ? 4 + p : p;
        if (bk >= NB || have[bk] >= PER_BUCKET) continue;
        have[bk]++, g_bucket[bk]++;
        memcpy(blk + 16 * k++, b, 16);
    }
    for (int i = 0; i < 40 && k < n; i++, g_void++) void_block(blk + 16 * k++, i % 5);
    for (; k < n; k++) {
        uint64_t v[2] = {rnd64(), rnd64()};
        memcpy(blk + 16 * k, v, 16);
        if (!astc_decode_block(blk + 16 * k, bw, bh, false, tmp)) g_err++;
    }
}

typedef struct {
    VkImage img;
    VmaAllocation alloc;
} Img;

static int make_image(VkCtx *c, uint32_t w, uint32_t h, uint32_t levels, VkFormat format, Img *o) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = format, .extent = {w, h, 1},
                            .mipLevels = levels, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_SAMPLED_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    return vmaCreateImage(c->vma, &ci, &ai, &o->img, &o->alloc, NULL) == VK_SUCCESS;
}

static void barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst,
                    VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa,
                           .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
#define ALL_RW VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT

static void to_general(VkCommandBuffer cb, VkImage img) {
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
}

static double now_ms(void) { return (double)plat_now_ns() / 1e6; }

// Decodes the two regions ar (w0 x h0 at level 0, w1 x h1 at level 1) of the src_bytes at src on the GPU; out_rb: the
// decoder's texels, out_img: the image's two levels read back. 0 on a Vulkan failure.
static int gpu_decode_ar(VkCtx *c, const uint8_t *src, size_t src_bytes, const VkAstcRegion ar[2], uint32_t bw,
                         uint32_t bh, int srgb, uint8_t *out_rb, uint8_t *out_img, double *ms) {
    uint32_t w0 = ar[0].w, h0 = ar[0].h, w1 = ar[1].w, h1 = ar[1].h;
    Img im;
    if (!make_image(c, w0, h0, 2, srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, &im)) return 0;
    size_t t0 = (size_t)w0 * h0 * 4, t1 = (size_t)w1 * h1 * 4;
    VkCommandBuffer cb = vk_upload_begin();
    VkBuffer sb, rb, ib;
    VmaAllocation ra, ia;
    uint8_t *stg = vk_staging(src_bytes, &sb, NULL);
    uint8_t *rmap = vk_staging(t0 + t1, &rb, &ra), *imap = vk_staging(t0 + t1, &ib, &ia);
    memcpy(stg, src, src_bytes);
    to_general(cb, im.img);
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    int ok = vk_astc_record(cb, sb, src_bytes, ar, 2, bw, bh, srgb, 0, im.img, VK_IMAGE_ASPECT_COLOR_BIT, rb);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    VkBufferImageCopy cp[2] = {{.bufferOffset = 0, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w0, h0, 1}},
                               {.bufferOffset = t0, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1}, .imageExtent = {w1, h1, 1}}};
    vkCmdCopyImageToBuffer(cb, im.img, VK_IMAGE_LAYOUT_GENERAL, ib, 2, cp);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    vk_upload_end();
    double a = now_ms();
    vk_upload_flush(1);
    if (ms) *ms = now_ms() - a;
    vmaInvalidateAllocation(c->vma, ra, 0, VK_WHOLE_SIZE);
    vmaInvalidateAllocation(c->vma, ia, 0, VK_WHOLE_SIZE);
    if (out_rb) memcpy(out_rb, rmap, t0 + t1);
    if (out_img) memcpy(out_img, imap, t0 + t1);
    vmaDestroyBuffer(c->vma, rb, ra), vmaDestroyBuffer(c->vma, ib, ia);
    vk_defer_image(im.img, im.alloc);
    vk_upload_flush(1);
    return ok > 0;
}

// The two regions as tight blocks: n0 blocks of level 0, then n1 of level 1.
static int gpu_decode(VkCtx *c, const uint8_t *blk, size_t n0, size_t n1, uint32_t w0, uint32_t h0, uint32_t w1,
                      uint32_t h1, uint32_t bw, uint32_t bh, int srgb, uint8_t *out_rb, uint8_t *out_img, double *ms) {
    VkAstcRegion ar[2] = {{.src = 0, .w = w0, .h = h0, .slices = 1, .level = 0},
                          {.src = n0 * 16, .w = w1, .h = h1, .slices = 1, .level = 1}};
    return gpu_decode_ar(c, blk, (n0 + n1) * 16, ar, bw, bh, srgb, out_rb, out_img, ms);
}

static int first_diff(const uint8_t *a, const uint8_t *b, size_t texels, size_t *at);

// The same blocks tiled as the game stores them (tex_bl_copy, the CPU deswizzle's own copy), level 0 in blocks of 4 GOBs
// and level 1 in blocks of 2, decoded in place (VkAstcRegion.bl, astc_decode.comp bl_block): the decoder's texels must be
// want's. Checks the GPU's block-linear addressing against the CPU's.
static int test_bl(VkCtx *c, const uint8_t *blk, size_t n0, uint32_t w0, uint32_t h0, uint32_t w1, uint32_t h1,
                   uint32_t bw, uint32_t bh, int srgb, const uint8_t *want) {
    const unsigned th[2] = {2, 1};
    uint32_t bx[2] = {(w0 + bw - 1) / bw, (w1 + bw - 1) / bw}, by[2] = {(h0 + bh - 1) / bh, (h1 + bh - 1) / bh};
    uint64_t sz[2];
    for (int lv = 0; lv < 2; lv++) {
        uint64_t gh = 8ull << th[lv];
        sz[lv] = ((uint64_t)bx[lv] * 16 + 63) / 64 * ((by[lv] + gh - 1) / gh) * (512ull << th[lv]);
    }
    uint8_t *bl = calloc(1, sz[0] + sz[1]);
    tex_bl_copy(bl, (uint8_t *)blk, (size_t)bx[0] * 16, bx[0] * 16, by[0], 1, th[0], 0, 0);
    tex_bl_copy(bl + sz[0], (uint8_t *)blk + n0 * 16, (size_t)bx[1] * 16, bx[1] * 16, by[1], 1, th[1], 0, 0);
    VkAstcRegion ar[2] = {{.src = 0, .w = w0, .h = h0, .slices = 1, .level = 0, .bl = 1, .th = (uint8_t)th[0]},
                          {.src = sz[0], .w = w1, .h = h1, .slices = 1, .level = 1, .bl = 1, .th = (uint8_t)th[1]}};
    size_t t0 = (size_t)w0 * h0, t1 = (size_t)w1 * h1, at;
    uint8_t *rb = malloc((t0 + t1) * 4);
    memset(rb, 0x5A, (t0 + t1) * 4);
    int bad = 0;
    if (!gpu_decode_ar(c, bl, sz[0] + sz[1], ar, bw, bh, srgb, rb, NULL, NULL)) {
        printf("probe-astc: FAIL %ux%u %s block-linear: the GPU decode was not recorded\n", bw, bh, srgb ? "sRGB" : "linear");
        bad = 1;
    }
    for (int lv = 0; lv < 2 && !bad; lv++) {
        size_t off = lv ? t0 * 4 : 0;
        uint32_t w = lv ? w1 : w0;
        if (first_diff(want + off, rb + off, lv ? t1 : t0, &at)) {
            printf("probe-astc: FAIL %ux%u %s block-linear level %d: texel (%zu,%zu) differs from the tight decode\n", bw, bh,
                   srgb ? "sRGB" : "linear", lv, at % w, at / w);
            bad = 1;
        }
    }
    free(bl), free(rb);
    return bad;
}

enum { MAX_LEVELS = 13 };   // a mip chain of up to 4096 texels a side
static size_t bc_bytes(uint32_t w, uint32_t h) { return (size_t)((w + 3) / 4) * ((h + 3) / 4) * 16; }

// Mip levels 0 .. levels-1 (w[i] x h[i] texels from the n[i] blocks that follow the previous level's in blk) decoded and
// encoded to BC3 (bc 1, NX2NAVI_ASTC_BC) or BC1 (bc 2) by one vk_astc_record call into an image of that format; out:
// its blocks read back level after level, rows of 4x4 blocks. 0 on a Vulkan failure or when nothing was recorded.
static int gpu_bc(VkCtx *c, const uint8_t *blk, const size_t *n, const uint32_t *w, const uint32_t *h, int levels,
                  uint32_t bw, uint32_t bh, int srgb, int bc, uint8_t *out) {
    Img im;
    VkFormat f = bc == 2 ? (srgb ? VK_FORMAT_BC1_RGB_SRGB_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK)
                         : (srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK);
    if (levels > MAX_LEVELS || !make_image(c, w[0], h[0], (uint32_t)levels, f, &im)) return 0;
    VkAstcRegion ar[MAX_LEVELS];
    VkBufferImageCopy cp[MAX_LEVELS];
    size_t nb = 0, ob = 0;
    for (int i = 0; i < levels; i++) {
        ar[i] = (VkAstcRegion){.src = nb * 16, .w = w[i], .h = h[i], .slices = 1, .level = (uint32_t)i};
        cp[i] = (VkBufferImageCopy){.bufferOffset = ob, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)i, 0, 1},
                                    .imageExtent = {w[i], h[i], 1}};
        nb += n[i], ob += bc_bytes(w[i], h[i]) / (bc == 2 ? 2 : 1);
    }
    VkCommandBuffer cb = vk_upload_begin();
    VkBuffer sb, ib;
    VmaAllocation ia;
    uint8_t *stg = vk_staging(nb * 16, &sb, NULL), *imap = vk_staging(ob, &ib, &ia);
    memcpy(stg, blk, nb * 16);
    to_general(cb, im.img);
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    int ok = vk_astc_record(cb, sb, nb * 16, ar, levels, bw, bh, srgb, bc, im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_NULL_HANDLE);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    vkCmdCopyImageToBuffer(cb, im.img, VK_IMAGE_LAYOUT_GENERAL, ib, (uint32_t)levels, cp);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    vk_upload_end();
    vk_upload_flush(1);
    vmaInvalidateAllocation(c->vma, ia, 0, VK_WHOLE_SIZE);
    memcpy(out, imap, ob);
    vmaDestroyBuffer(c->vma, ib, ia);
    vk_defer_image(im.img, im.alloc);
    vk_upload_flush(1);
    return ok > 0;
}

// One BC3 block to 16 RGBA8 texels, as the format defines it (565 endpoints widened by bit replication; the colour
// block always in four-colour mode). pos: each texel's place on the line from endpoint 0 (0) to endpoint 1 (3).
static void bc3_decode(const uint8_t b[16], uint8_t t[16][4], uint8_t ep[2][3], int pos[16]) {
    uint8_t a[8] = {b[0], b[1]}, p[4][3];
    if (a[0] > a[1])
        for (int i = 1; i < 7; i++) a[i + 1] = (uint8_t)(((7 - i) * a[0] + i * a[1] + 3) / 7);
    else {
        for (int i = 1; i < 5; i++) a[i + 1] = (uint8_t)(((5 - i) * a[0] + i * a[1] + 2) / 5);
        a[6] = 0, a[7] = 255;
    }
    uint64_t ai = 0;
    for (int i = 0; i < 6; i++) ai |= (uint64_t)b[2 + i] << 8 * i;
    for (int k = 0; k < 2; k++) {
        unsigned v = b[8 + 2 * k] | b[9 + 2 * k] << 8, r = v >> 11, g = v >> 5 & 63, bl = v & 31;
        p[k][0] = (uint8_t)(r << 3 | r >> 2), p[k][1] = (uint8_t)(g << 2 | g >> 4), p[k][2] = (uint8_t)(bl << 3 | bl >> 2);
    }
    for (int ch = 0; ch < 3; ch++)
        p[2][ch] = (uint8_t)((2 * p[0][ch] + p[1][ch] + 1) / 3), p[3][ch] = (uint8_t)((p[0][ch] + 2 * p[1][ch] + 1) / 3);
    uint32_t ci = b[12] | b[13] << 8 | b[14] << 16 | (uint32_t)b[15] << 24;
    static const int code_pos[4] = {0, 3, 1, 2};
    for (int i = 0; i < 16; i++) {
        memcpy(t[i], p[ci >> 2 * i & 3], 3);
        t[i][3] = a[ai >> 3 * i & 7];
        pos[i] = code_pos[ci >> 2 * i & 3];
    }
    memcpy(ep, p, 6);
}

// BC3 is lossy, so these are bounds, not bit-exactness. In every block: alpha within a fourteenth of the block's alpha
// range (the eight-value mode's half step); each texel on the palette entry nearest its projection onto the endpoint
// line (within rounding, when the endpoints are apart); a block of one colour within the 565 rounding, alpha exact.
// Per level, a PSNR floor against astc_decode_ldr for gross damage: the random blocks are noise, so it is low (19 dB
// at worst for 5x5 level 1 on an RX 9070). *db: the level's PSNR.
enum { BC_MIN_DB = 15 };
static int bc_check(const uint8_t *want, const uint8_t *blocks, uint32_t w, uint32_t h, const char *what, double *db) {
    uint32_t bx = (w + 3) / 4, by = (h + 3) / 4;
    double se = 0;
    for (uint32_t y4 = 0; y4 < by; y4++)
        for (uint32_t x4 = 0; x4 < bx; x4++) {
            uint8_t t[16][4], ep[2][3], lo[4] = {255, 255, 255, 255}, hi[4] = {0};
            int pos[16];
            bc3_decode(blocks + 16 * ((size_t)y4 * bx + x4), t, ep, pos);
            double axis[3], len2 = 0;
            for (int ch = 0; ch < 3; ch++) axis[ch] = (double)ep[1][ch] - ep[0][ch], len2 += axis[ch] * axis[ch];
            for (int pass = 0; pass < 2; pass++)
                for (int i = 0; i < 16; i++) {
                    uint32_t x = x4 * 4 + (i & 3), y = y4 * 4 + (i >> 2);
                    if (x >= w || y >= h) continue;
                    const uint8_t *s = want + 4 * ((size_t)y * w + x);
                    if (!pass) {
                        for (int ch = 0; ch < 4; ch++) lo[ch] = s[ch] < lo[ch] ? s[ch] : lo[ch], hi[ch] = s[ch] > hi[ch] ? s[ch] : hi[ch];
                        continue;
                    }
                    int flat = !memcmp(lo, hi, 4), lim[4] = {5, 3, 5, 0};
                    if (len2 >= 64) {   // shorter lines: rounding alone can move a texel to the next entry
                        double proj = 0;
                        for (int ch = 0; ch < 3; ch++) proj += ((double)s[ch] - ep[0][ch]) * axis[ch];
                        proj = proj / len2 * 3;
                        proj = proj < 0 ? 0 : proj > 3 ? 3 : proj;
                        if (fabs(pos[i] - proj) > 0.75) {   // a wrong code mapping is a whole place off
                            printf("probe-astc: FAIL BC3 %s: texel (%u,%u) on palette place %d, its projection is %.2f\n",
                                   what, x, y, pos[i], proj);
                            return 1;
                        }
                    }
                    for (int ch = 0; ch < 4; ch++) {
                        int d = abs((int)t[i][ch] - s[ch]);
                        se += (double)d * d;
                        if ((ch == 3 && d > (hi[3] - lo[3]) / 14 + 1) || (flat && d > lim[ch])) {
                            printf("probe-astc: FAIL BC3 %s: texel (%u,%u) channel %d: astc %02x%02x%02x%02x bc3 %02x%02x%02x%02x\n",
                                   what, x, y, ch, s[0], s[1], s[2], s[3], t[i][0], t[i][1], t[i][2], t[i][3]);
                            return 1;
                        }
                    }
                }
        }
    double mse = se / ((double)w * h * 4);
    *db = mse > 0 ? 10 * log10(255.0 * 255.0 / mse) : 99;
    if (*db < BC_MIN_DB) {
        printf("probe-astc: FAIL BC3 %s: PSNR %.1f dB, below %d\n", what, *db, BC_MIN_DB);
        return 1;
    }
    return 0;
}

static int first_diff(const uint8_t *a, const uint8_t *b, size_t texels, size_t *at) {
    for (size_t i = 0; i < texels; i++)
        if (memcmp(a + 4 * i, b + 4 * i, 4)) { *at = i; return 1; }
    return 0;
}

// astc_block_opaque against a decode of each of the n blocks: opaque exactly when every texel has alpha 255. Then the
// loop over them (astc_blocks_opaque, also copying) and the worker-pool scan (astc_blocks_opaque_mt) must give the
// first block that is not opaque, the same bytes, and the same answer. Opaque 4x4 blocks are kept for opaque_mt_case.
enum { OP_POOL = 4096 };
static uint64_t g_op_blocks, g_op_opaque, g_op_decoded, g_bc1_blocks;
static uint8_t g_op_pool[OP_POOL * 16], g_alpha_block[16];
static size_t g_op_pool_n;
static int g_have_alpha;
static int opaque_check(const uint8_t *blk, size_t n, uint32_t bw, uint32_t bh, int srgb, const char *what) {
    uint8_t t[12 * 12 * 4];
    size_t first = n;
    for (size_t k = 0; k < n; k++) {
        const uint8_t *b = blk + 16 * k;
        astc_decode_block(b, bw, bh, srgb, t);
        int want = 1;
        for (uint32_t i = 0; i < bw * bh && want; i++) want = t[4 * i + 3] == 255;
        uint32_t decoded = 0;
        if ((int)astc_block_opaque(b, bw, bh, srgb, &decoded) != want) {
            printf("probe-astc: FAIL opaque check %s block %zu: the decode says %s\n", what, k, want ? "opaque" : "alpha");
            return 1;
        }
        if (!want && first == n) first = k;
        if (bw == 4 && bh == 4 && !srgb && want && g_op_pool_n < OP_POOL) memcpy(g_op_pool + 16 * g_op_pool_n++, b, 16);
        if (bw == 4 && bh == 4 && !srgb && !want && !g_have_alpha) memcpy(g_alpha_block, b, 16), g_have_alpha = 1;
        g_op_blocks++, g_op_opaque += want, g_op_decoded += decoded;
    }
    uint32_t d1 = 0, d2 = 0;
    uint8_t *cp = malloc(16 * n);
    uint64_t got = astc_blocks_opaque(blk, n, bw, bh, srgb, UINT32_MAX, &d1, cp);
    uint8_t *cp2 = malloc(16 * n);
    int bad = got != first || memcmp(cp, blk, 16 * n) ||
              astc_blocks_opaque_mt(blk, n, bw, bh, srgb, UINT32_MAX, &d2, cp2) != (first == n) || memcmp(cp2, blk, 16 * n);
    free(cp2);
    if (bad)
        printf("probe-astc: FAIL opaque check %s: the loop gives block %llu (want %zu), or its copy or the worker pool "
               "differs\n", what, (unsigned long long)got, first);
    free(cp);
    return bad;
}

// The worker-pool scan of a texture of 300,000 opaque 4x4 blocks (opaque blocks of the cases, repeated), alone and
// copying: opaque, every byte copied; then with one block that is not opaque at 250,000: not opaque, every byte still
// copied, and the loop names that block.
static int opaque_mt_case(void) {
    if (g_op_pool_n < 64 || !g_have_alpha) {
        printf("probe-astc: FAIL opaque check: too few opaque (%zu) or no alpha 4x4 blocks to build the large case\n",
               g_op_pool_n);
        return 1;
    }
    size_t n = 300000;
    uint8_t *blk = malloc(16 * n);
    for (size_t k = 0; k < n; k++) memcpy(blk + 16 * k, g_op_pool + 16 * (k % g_op_pool_n), 16);
    uint32_t d = 0;
    uint8_t *cp = malloc(16 * n);
    int bad = !astc_blocks_opaque_mt(blk, n, 4, 4, false, UINT32_MAX, &d, NULL) ||
              !astc_blocks_opaque_mt(blk, n, 4, 4, false, UINT32_MAX, &d, cp) || memcmp(cp, blk, 16 * n);
    memcpy(blk + 16 * 250000, g_alpha_block, 16);
    memset(cp, 0, 16 * n);
    d = 0;
    bad = bad || astc_blocks_opaque_mt(blk, n, 4, 4, false, UINT32_MAX, &d, NULL) ||
          astc_blocks_opaque_mt(blk, n, 4, 4, false, UINT32_MAX, &d, cp) || memcmp(cp, blk, 16 * n) ||
          astc_blocks_opaque(blk, n, 4, 4, false, UINT32_MAX, &d, NULL) != 250000;
    if (bad) printf("probe-astc: FAIL opaque check (and copy) of %zu blocks on the worker pool\n", n);
    free(blk), free(cp);
    return bad;
}

// The BC1 blocks (8 bytes each) of a level against its BC3 blocks: each the second half of its BC3 block.
static int bc1_check(const uint8_t *bc3, const uint8_t *bc1, uint32_t w, uint32_t h, const char *what) {
    size_t n = bc_bytes(w, h) / 16;
    for (size_t k = 0; k < n; k++)
        if (memcmp(bc3 + 16 * k + 8, bc1 + 8 * k, 8)) {
            printf("probe-astc: FAIL BC1 %s: block %zu is not the colour half of its BC3 block\n", what, k);
            return 1;
        }
    g_bc1_blocks += n;
    return 0;
}

// The regions of one case encoded to BC1 too (vk_astc_bc1), held to bc1_check against their BC3 blocks.
static int bc1_case(VkCtx *c, const uint8_t *blk, const size_t *n, const uint32_t *w, const uint32_t *h, int levels,
                    uint32_t bw, uint32_t bh, int srgb, const uint8_t *bc3, const char *what) {
    if (!vk_astc_bc1()) return 0;
    size_t total = 0;
    for (int i = 0; i < levels; i++) total += bc_bytes(w[i], h[i]) / 2;
    uint8_t *bc1 = malloc(total);
    int bad = !gpu_bc(c, blk, n, w, h, levels, bw, bh, srgb, 2, bc1);
    if (bad) printf("probe-astc: FAIL BC1 %s: the encode was not recorded\n", what);
    size_t o3 = 0, o1 = 0;
    for (int i = 0; i < levels && !bad; o3 += bc_bytes(w[i], h[i]), o1 += bc_bytes(w[i], h[i]) / 2, i++) {
        char lv[96];
        snprintf(lv, sizeof lv, "%s level %d", what, i);
        bad = bc1_check(bc3 + o3, bc1 + o1, w[i], h[i], lv);
    }
    free(bc1);
    return bad;
}

static int test_case(VkCtx *c, uint32_t bw, uint32_t bh, int srgb) {
    // level 0: 37 x 23 blocks, cropped by 1-3 texels; level 1 half of that, other blocks
    uint32_t w0 = 37 * bw - 1 - (bw > 5), h0 = 23 * bh - 2, w1 = w0 > 1 ? w0 >> 1 : 1, h1 = h0 > 1 ? h0 >> 1 : 1;
    size_t n0 = (size_t)((w0 + bw - 1) / bw) * ((h0 + bh - 1) / bh), n1 = (size_t)((w1 + bw - 1) / bw) * ((h1 + bh - 1) / bh);
    uint8_t *blk = malloc((n0 + n1) * 16);
    make_blocks(blk, n0, bw, bh);
    make_blocks(blk + n0 * 16, n1, bw, bh);
    size_t t0 = (size_t)w0 * h0, t1 = (size_t)w1 * h1;
    uint8_t *want = malloc((t0 + t1) * 4), *rb = malloc((t0 + t1) * 4), *im = malloc((t0 + t1) * 4);
    memset(rb, 0x5A, (t0 + t1) * 4), memset(im, 0xA5, (t0 + t1) * 4);
    astc_decode_ldr(blk, w0, h0, bw, bh, srgb, want);
    astc_decode_ldr(blk + n0 * 16, w1, h1, bw, bh, srgb, want + t0 * 4);
    char name[48];
    snprintf(name, sizeof name, "%ux%u %s", bw, bh, srgb ? "sRGB" : "linear");
    int bad = opaque_check(blk, n0 + n1, bw, bh, srgb, name);
    if (!bad && !gpu_decode(c, blk, n0, n1, w0, h0, w1, h1, bw, bh, srgb, rb, im, NULL)) {
        printf("probe-astc: FAIL %ux%u %s: the GPU decode was not recorded\n", bw, bh, srgb ? "sRGB" : "linear");
        bad = 1;
    }
    for (int lv = 0; lv < 2 && !bad; lv++) {
        size_t off = lv ? t0 * 4 : 0, n = lv ? t1 : t0, at;
        uint32_t w = lv ? w1 : w0;
        for (int which = 0; which < 2 && !bad; which++) {
            const uint8_t *got = (which ? im : rb) + off;
            if (first_diff(want + off, got, n, &at)) {
                const uint8_t *e = want + off + 4 * at, *g = got + 4 * at;
                printf("probe-astc: FAIL %ux%u %s level %d (%s): texel (%zu,%zu) cpu %02x%02x%02x%02x gpu %02x%02x%02x%02x\n",
                       bw, bh, srgb ? "sRGB" : "linear", lv, which ? "image" : "decoder output", at % w, at / w,
                       e[0], e[1], e[2], e[3], g[0], g[1], g[2], g[3]);
                bad = 1;
            }
        }
    }
    if (!bad) bad = test_bl(c, blk, n0, w0, h0, w1, h1, bw, bh, srgb, want);
    if (!bad && vk_astc_bc()) {
        size_t b0 = bc_bytes(w0, h0), b1 = bc_bytes(w1, h1);
        uint8_t *bc = malloc(b0 + b1);
        char what[2][48];
        double db[2] = {0, 0};
        for (int lv = 0; lv < 2; lv++) snprintf(what[lv], sizeof what[lv], "%ux%u %s level %d", bw, bh, srgb ? "sRGB" : "linear", lv);
        size_t nn[2] = {n0, n1};
        uint32_t ww[2] = {w0, w1}, hh[2] = {h0, h1};
        if (!gpu_bc(c, blk, nn, ww, hh, 2, bw, bh, srgb, 1, bc)) {
            printf("probe-astc: FAIL BC3 %s: the encode was not recorded\n", what[0]);
            bad = 1;
        }
        bad = bad || bc_check(want, bc, w0, h0, what[0], &db[0]) || bc_check(want + t0 * 4, bc + b0, w1, h1, what[1], &db[1]);
        bad = bad || bc1_case(c, blk, nn, ww, hh, 2, bw, bh, srgb, bc, name);
        for (int lv = 0; lv < 2 && !bad; lv++)
            if (db[lv] < g_bc_db) g_bc_db = db[lv];
        g_bc_cases += !bad;
        free(bc);
    }
    g_blocks += n0 + n1, g_texels += t0 + t1;
    free(blk), free(want), free(rb), free(im);
    return bad;
}

// ---- cut-outs: alpha-tested foliage through the BC3 encoder
// Mip chains whose ASTC blocks are, by a hash of their place, fully transparent (alpha 0 over whatever colour, as an
// encoder leaves the colour under a cut-out), fully opaque leaf green, or a block whose alpha jumps from transparent to
// opaque between two neighbouring texels. The edges between blocks lie on the footprint's grid, so along a side of 5, 6
// or 10 texels they cut through BC3's 4x4 blocks at every offset; the levels shrink to odd sizes down to 1x1.
enum { SHARP_POOL = 96 };
static uint64_t g_cut_cases, g_cut_texels, g_cut_edges, g_cut_sharp;
static double g_cut_db = 99;   // the lowest BC3 PSNR of the cut-out levels

// Valid blocks (not void extents) whose alpha runs from at most 16 to at least 240 with a step of 192 or more between
// two neighbouring texels; the number found, up to SHARP_POOL.
static size_t sharp_blocks(uint8_t *pool, uint32_t bw, uint32_t bh) {
    uint8_t t[12 * 12 * 4];
    size_t k = 0;
    for (int tries = 0; tries < 400000 && k < SHARP_POOL; tries++) {
        uint64_t v[2] = {rnd64(), rnd64()};
        if ((v[0] & 0x1FF) == 0x1FC || !astc_decode_block((uint8_t *)v, bw, bh, false, t)) continue;
        int lo = 255, hi = 0, step = 0;
        for (uint32_t y = 0; y < bh; y++)
            for (uint32_t x = 0; x < bw; x++) {
                int a = t[4 * (y * bw + x) + 3];
                lo = a < lo ? a : lo, hi = a > hi ? a : hi;
                if ((x && abs(a - t[4 * (y * bw + x - 1) + 3]) >= 192) || (y && abs(a - t[4 * ((y - 1) * bw + x) + 3]) >= 192))
                    step = 1;
            }
        if (lo <= 16 && hi >= 240 && step) memcpy(pool + 16 * k++, v, 16);
    }
    return k;
}

// A void-extent block of one colour (UNORM16 channels, the 8-bit value in both bytes).
static void void_rgba(uint8_t b[16], const uint8_t c[4]) {
    uint64_t lo = 0x1FC | 3ull << 10 | 0x1FFFull << 12 | 0x1FFFull << 25 | 0x1FFFull << 38 | 0x1FFFull << 51, hi = 0;
    for (int ch = 0; ch < 4; ch++) hi |= (uint64_t)(c[ch] * 257u) << 16 * ch;
    memcpy(b, &lo, 8), memcpy(b + 8, &hi, 8);
}

// The n blocks of one level, bx to a row: opaque and transparent in patches of 2x2 blocks, one block in five from the
// sharp-edged pool.
static void cut_blocks(uint8_t *blk, size_t n, uint32_t bx, int level, const uint8_t *pool, size_t npool) {
    for (size_t k = 0; k < n; k++) {
        uint32_t x = (uint32_t)(k % bx), y = (uint32_t)(k / bx);
        uint64_t patch = ((x >> 1) * 0x9E3779B1ull ^ (y >> 1) * 0x85EBCA77ull ^ (uint64_t)level * 0xC2B2AE3Dull) * 0x27D4EB2F165667C5ull;
        uint64_t own = (x * 0xC2B2AE3Dull ^ y * 0x9E3779B1ull ^ (uint64_t)level) * 0x165667B19E3779F9ull;
        patch ^= patch >> 29, own ^= own >> 31;
        if (own % 5 == 0) {
            memcpy(blk + 16 * k, pool + 16 * (own / 5 % npool), 16);
            continue;
        }
        uint64_t r = rnd64();
        uint8_t col[4] = {(uint8_t)r, (uint8_t)(r >> 8), (uint8_t)(r >> 16), 0};   // transparent: any colour
        if (patch % 2)
            col[0] = (uint8_t)(40 + r % 60), col[1] = (uint8_t)(120 + (r >> 8) % 80), col[2] = (uint8_t)(20 + (r >> 16) % 40),
            col[3] = 255;
        void_rgba(blk + 16 * k, col);
    }
}

// What an alpha test sees of the cut-outs. A texel the ASTC image holds fully transparent or fully opaque stays
// exactly 0 or 255 (a BC3 alpha block's endpoints are its 4x4 block's least and greatest alpha), and every texel keeps
// its side of an alpha test at a quarter, a half and three quarters unless it lies within the block's alpha step of
// the threshold. *edges: the 4x4 blocks whose alpha runs from at most 16 to at least 240.
static int cut_check(const uint8_t *want, const uint8_t *blocks, uint32_t w, uint32_t h, const char *what, uint64_t *edges) {
    static const int thr[3] = {64, 128, 192};
    uint32_t bx = (w + 3) / 4, by = (h + 3) / 4;
    for (uint32_t y4 = 0; y4 < by; y4++)
        for (uint32_t x4 = 0; x4 < bx; x4++) {
            uint8_t t[16][4], ep[2][3];
            int pos[16], lo = 255, hi = 0;
            bc3_decode(blocks + 16 * ((size_t)y4 * bx + x4), t, ep, pos);
            for (int pass = 0; pass < 2; pass++)
                for (int i = 0; i < 16; i++) {
                    uint32_t x = x4 * 4 + (i & 3), y = y4 * 4 + (i >> 2);
                    if (x >= w || y >= h) continue;
                    int a = want[4 * ((size_t)y * w + x) + 3], b = t[i][3];
                    if (!pass) {
                        lo = a < lo ? a : lo, hi = a > hi ? a : hi;
                        continue;
                    }
                    int margin = (hi - lo) / 14 + 1;   // bc_check's alpha bound
                    if ((a == 0 || a == 255) && b != a) {
                        printf("probe-astc: FAIL BC3 cut-out %s: texel (%u,%u) alpha %d came out %d\n", what, x, y, a, b);
                        return 1;
                    }
                    for (int k = 0; k < 3; k++)
                        if (abs(a - thr[k]) > margin && (a >= thr[k]) != (b >= thr[k])) {
                            printf("probe-astc: FAIL BC3 cut-out %s: texel (%u,%u) alpha %d came out %d, across an alpha "
                                   "test at %d (block alpha %d..%d)\n", what, x, y, a, b, thr[k], lo, hi);
                            return 1;
                        }
                }
            *edges += lo <= 16 && hi >= 240;
        }
    return 0;
}

// One cut-out mip chain of w0 x h0 texels at footprint bw x bh: every level held to bc_check's bounds and cut_check.
static int cutout_case(VkCtx *c, uint32_t bw, uint32_t bh, int srgb, uint32_t w0, uint32_t h0) {
    uint8_t pool[SHARP_POOL * 16];
    size_t npool = sharp_blocks(pool, bw, bh);
    char what[64];
    snprintf(what, sizeof what, "%ux%u %s %ux%u", bw, bh, srgb ? "sRGB" : "linear", w0, h0);
    if (npool < 8) {
        printf("probe-astc: FAIL BC3 cut-out %s: only %zu blocks with a sharp alpha edge were generated\n", what, npool);
        return 1;
    }
    int levels = 1;
    for (uint32_t s = w0 > h0 ? w0 : h0; s > 1; s >>= 1) levels++;
    if (levels > MAX_LEVELS) levels = MAX_LEVELS;
    size_t n[MAX_LEVELS], nb = 0, nt = 0, nbc = 0;
    uint32_t w[MAX_LEVELS], h[MAX_LEVELS];
    for (int i = 0; i < levels; i++) {
        w[i] = w0 >> i ? w0 >> i : 1, h[i] = h0 >> i ? h0 >> i : 1;
        n[i] = (size_t)((w[i] + bw - 1) / bw) * ((h[i] + bh - 1) / bh);
        nb += n[i], nt += (size_t)w[i] * h[i], nbc += bc_bytes(w[i], h[i]);
    }
    uint8_t *blk = malloc(nb * 16), *want = malloc(nt * 4), *bc = malloc(nbc);
    if (!blk || !want || !bc) { printf("probe-astc: FAIL out of memory\n"); return 1; }
    size_t ob = 0, ot = 0;
    for (int i = 0; i < levels; ob += n[i], ot += (size_t)w[i] * h[i], i++) {
        cut_blocks(blk + ob * 16, n[i], (w[i] + bw - 1) / bw, i, pool, npool);
        astc_decode_ldr(blk + ob * 16, w[i], h[i], bw, bh, srgb, want + ot * 4);
    }
    int bad = opaque_check(blk, nb, bw, bh, srgb, what);
    if (!bad && !gpu_bc(c, blk, n, w, h, levels, bw, bh, srgb, 1, bc)) {
        printf("probe-astc: FAIL BC3 cut-out %s: the encode was not recorded\n", what);
        bad = 1;
    }
    if (!bad) bad = bc1_case(c, blk, n, w, h, levels, bw, bh, srgb, bc, what);
    uint64_t edges = 0;
    ot = ob = 0;
    for (int i = 0; i < levels && !bad; ot += (size_t)w[i] * h[i], ob += bc_bytes(w[i], h[i]), i++) {
        char lv[80];
        double db = 99;
        snprintf(lv, sizeof lv, "%s level %d (%ux%u)", what, i, w[i], h[i]);
        bad = bc_check(want + ot * 4, bc + ob, w[i], h[i], lv, &db) || cut_check(want + ot * 4, bc + ob, w[i], h[i], lv, &edges);
        if (!bad && db < g_cut_db) g_cut_db = db;
    }
    if (!bad && !edges) {
        printf("probe-astc: FAIL BC3 cut-out %s: no 4x4 block ran from transparent to opaque\n", what);
        bad = 1;
    }
    if (!bad) g_cut_cases++, g_cut_texels += nt, g_cut_edges += edges, g_cut_sharp += npool;
    free(blk), free(want), free(bc);
    return bad;
}

// One 2048 x 2048 texture of valid blocks: the CPU worker-pool decode against the GPU time of the dispatches alone
// (vk_astc.c timestamps, NX2NAVI_ASTC_TIME) and the wall time of the upload batch.
static void timing(VkCtx *c, uint32_t bw, uint32_t bh) {
    uint32_t w = 2048, h = 2048;
    size_t n = (size_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh);
    uint8_t *blk = malloc(n * 16), *out = malloc((size_t)w * h * 4), tmp[12 * 12 * 4];
    for (size_t k = 0; k < 4096;) {   // valid, non-constant blocks: error blocks would flatter the GPU
        uint64_t v[2] = {rnd64(), rnd64()};
        if ((v[0] & 0x1FF) != 0x1FC && astc_decode_block((uint8_t *)v, bw, bh, false, tmp)) memcpy(blk + 16 * k++, v, 16);
    }
    for (size_t k = 4096; k < n; k++) memcpy(blk + 16 * k, blk + 16 * (k % 4096), 16);
    double a = now_ms();
    astc_decode_ldr_mt(blk, w, h, bw, bh, false, out);
    double cpu = now_ms() - a, wall = 0;
    vk_astc_time_enable(1);
    uint64_t t0, c0, t1, c1;
    double u0, u1;
    vk_astc_time_get(bw, bh, &t0, &c0, &u0);
    for (int i = 0; i < 4; i++) gpu_decode(c, blk, n, 1, w, h, 1, 1, bw, bh, 0, NULL, NULL, i ? &wall : NULL);
    vk_astc_time_get(bw, bh, &t1, &c1, &u1);
    vk_astc_time_enable(0);
    double mtex = (double)(t1 - t0) / 1e6;
    printf("probe-astc: 2048x2048 %ux%u: CPU (worker pool) %.1f ms; GPU dispatches %.0f us per M texel (%llu decodes "
           "timed, %.2f M texels), batch wall %.1f ms\n", bw, bh, cpu, mtex > 0 ? (u1 - u0) / mtex : 0.0,
           (unsigned long long)(c1 - c0), mtex, wall);
    free(blk), free(out);
}

int probe_astc(void) {
    cfg_setenv("NX2NAVI_ASTC_BC", "1");   // also builds the BC3 encoder; the RGBA8 cases pass bc 0 and do not use it
    if (!vk_init()) { printf("probe-astc: FAIL vk_init\n"); return 1; }
    VkCtx *c = vk_ctx();
    printf("probe-astc: GPU %s, textureCompressionASTC_LDR %d, mode %d\n", c->props.deviceName, c->has_astc_ldr,
           vk_astc_mode());
    int bad = 0;
    if (!vk_astc_gpu()) {
        printf("probe-astc: FAIL the compute decoder is not available (NX2NAVI_ASTC=cpu or pipeline creation failed)\n");
        bad = 1;
    }
    for (int i = 0; i < 14 && !bad; i++)
        for (int s = 0; s < 2 && !bad; s++) bad = test_case(c, dims[i][0], dims[i][1], s);
    for (int b = 0; b < NB && !bad; b++)
        if (!g_bucket[b]) { printf("probe-astc: FAIL no valid %s block was generated\n", bucket_name[b]); bad = 1; }
    if (!bad && !vk_astc_bc()) printf("probe-astc: BC3 encoder not available on this device: not checked\n");
    else if (!bad) {
        printf("probe-astc: BC3 encode: %llu cases within the bounds, lowest PSNR %.1f dB (floor %d)\n",
               (unsigned long long)g_bc_cases, g_bc_db, BC_MIN_DB);
        // cut-outs: every footprint, then two chains large enough that level 0 is a scratch chunk of its own
        for (int i = 0; i < 14 && !bad; i++)
            for (int s = 0; s < 2 && !bad; s++)
                bad = cutout_case(c, dims[i][0], dims[i][1], s, 53u * dims[i][0] - 3, 41u * dims[i][1] - 1);
        if (!bad) bad = cutout_case(c, 4, 4, 1, 3001, 2999) || cutout_case(c, 6, 5, 0, 3003, 2997);
        if (!bad)
            printf("probe-astc: BC3 cut-outs: %llu mip chains, %.2f M texels, %llu 4x4 blocks from transparent to opaque "
                   "(%llu sharp-edged ASTC blocks among the void extents): alpha 0 and 255 exact, every alpha test at 1/4, "
                   "1/2 and 3/4 kept outside the block's alpha step; lowest PSNR %.1f dB\n",
                   (unsigned long long)g_cut_cases, (double)g_cut_texels / 1e6, (unsigned long long)g_cut_edges,
                   (unsigned long long)g_cut_sharp, g_cut_db);
        if (!bad && vk_astc_bc1())
            printf("probe-astc: BC1 encode: %llu blocks, each the colour half of its BC3 block\n",
                   (unsigned long long)g_bc1_blocks);
        else if (!bad) printf("probe-astc: BC1 encoder not on (NX2NAVI_ASTC_BC1 or the device's BC1 features): not checked\n");
    }
    if (!bad) bad = opaque_mt_case();
    if (!bad)
        printf("probe-astc: opaque check: %llu blocks agree with their decode, %llu of them opaque, %llu decoded to tell; "
               "the loop, its copy and the worker pool agree, also on 300,000 blocks\n",
               (unsigned long long)g_op_blocks, (unsigned long long)g_op_opaque, (unsigned long long)g_op_decoded);
    if (!bad) {
        timing(c, 4, 4);
        timing(c, 6, 6);
        timing(c, 8, 8);
        timing(c, 12, 12);
    }
    vk_mem_shutdown();
    vk_shutdown();
    long errors = vk_validation_errors_total();
    if (bad || errors) {
        printf("probe-astc: FAILED (%s, %ld validation errors)\n", bad ? "a check failed" : "checks passed", errors);
        return 1;
    }
    printf("probe-astc: passed (14 footprints x linear/sRGB, tight and block-linear, %llu blocks, %llu texels bit-exact "
           "with astc_decode_ldr; "
           "valid blocks by kind: 1p %llu, 2p %llu, 3p %llu, 4p %llu, dual 1p %llu, dual 2p %llu, dual 3p %llu; "
           "%llu void extents, %llu random error blocks; 0 validation errors)\n",
           (unsigned long long)g_blocks, (unsigned long long)g_texels, (unsigned long long)g_bucket[0],
           (unsigned long long)g_bucket[1], (unsigned long long)g_bucket[2], (unsigned long long)g_bucket[3],
           (unsigned long long)g_bucket[4], (unsigned long long)g_bucket[5], (unsigned long long)g_bucket[6],
           (unsigned long long)g_void, (unsigned long long)g_err);
    return 0;
}
