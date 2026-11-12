// NX2NAVI_DUMP=<frame>: a frame dump for the Vulkan back end, which RenderDoc cannot capture (it hides
// VK_EXT_external_memory_host). Between presents frame-1 and frame, the replay's command buffers also copy, after each
// render pass, its colour and depth targets and, after each dispatch, the storage images it writes into host buffers;
// the completion thread turns them into 8-bit PNGs once the submission is done. 3D textures (LUTs, volumes) sampled or
// written in the frame are copied once each, slices side by side. Files in NX2NAVI_DUMP_DIR (default "dump"):
//   pass_<seq>_t<i>_<w>x<h>_<vkformat>_<name>.png   colour target i (td: depth; img<slot>: storage image; lut<id>: 3D;
//                                                    a BC target drawn through a block view: its blocks as that view)
//   ..._tm.png                                       float formats: x/(1+x) of the same image (the plain one clamps);
//                                                    in both, a pixel with a NaN channel is magenta
//   passes.txt   per pass or dispatch: targets, draws, each distinct pipeline + texture set with its shaders' SPIR-V
//                hashes (sh_<hash>.spv written once) and the bound textures with their samplers
//   images.txt   per image: channel min/max, distinct texel values over the image and over its top quarter
// Sampled 8-bit 2D textures up to TEX2D_CAP texels are copied once each as pass_<seq>_tex<id>_... at the end of the pass
// that samples them; texture lines carry the storage address, mirror state, upload count and the image's and slot's
// swizzle. Copies and texture clears are logged as "copy/clear after pass N". NX2NAVI_DUMP_LOG=<from> also logs the
// frames from..frame-1 to passes.txt, with PNGs only of targets up to 64K texels. NX2NAVI_DUMP_BUF=<addr>:<size>[:...]
// snapshots one GPU buffer range after every logged pass (buf_<pass>.bin and bufs.txt, see buf_snap).
// PNGs are opaque (alpha dropped). Integer formats are scaled by the image maximum, depth by its min..max range, SNORM
// as x/2+1/2, UNORM and sRGB bytes written as stored. Caps: MAX_PASSES passes copied, PNG_CAP bytes of PNGs.
// NX2NAVI_DUMP_TEX=<id>|<w>x<h>[,...]: those texture slots, or 2D textures of that size, are copied where first sampled,
// whatever their format.
// NX2NAVI_DUMP_UBO=<n>: each listed pipeline also prints the first n vec4 of its user uniform blocks (c3 and up; 1 means 16,
// the cap is 4096), read from guest memory when the entry is listed. A block the submission rewrites with UpdateUniformBuffer shows its value from
// before the submission (vk_exec_draw.c upd_note).
// NX2NAVI_DUMP_SKIP_RUNS=1: a render pass with the same targets as the render pass before it is logged but not copied
// (the terrain decals draw one pass each into the G-buffer and would use up both caps).
// NX2NAVI_DUMP_SHADERS=<hash>,...: copy images only for passes using those shader hashes; metadata covers the whole frame.
// NX2NAVI_DUMP_RAW=1: also save each copied image's native texel bytes as .bin, for numerical analysis.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "vk.h"

#include "vk_exec.h"
#include "flight.h"
#include "input.h"
uint64_t vk_exec_cs_storage_addr(const VkExec *ex, const GfxShaderRes *r);   // vk_exec_draw.c
uint64_t vk_exec_storage_addr(const VkExec *ex, int st, const GfxShaderRes *r);   // vk_exec_draw.c
int vk_exec_raw_9e5(VkImage img, VkFormat f);                               // vk_exec_draw.c
#include "platform/platform.h"

#define MAX_PASSES 400
#define PNG_CAP (2ull << 30)
#define INFLIGHT_CAP (3ull << 30)   // host buffers waiting for their submission
#define MAX_ENTS 1024               // distinct pipeline + texture sets listed per pass
#define MAX_LUTS 256                // 3D textures and small sampled 8-bit 2D textures dumped per frame
#define TEX2D_CAP (1u << 20)        // texels of a sampled 2D texture that is dumped

int vk_dump_armed;
static uint64_t dump_frame, log_from;   // log_from: NX2NAVI_DUMP_LOG, first frame logged to passes.txt without images
static char dir[400];
static char base_dir[400];             // NX2NAVI_DUMP_DIR; with NX2NAVI_DUMP_WHITE each attempt goes to base_dir/f<frame>
static uint64_t white_last;            // NX2NAVI_DUMP_WHITE: the frame dumped last (0: none), checked 3 presents on
static int white_tries, white_caught;
static uint64_t dump_after;             // NX2NAVI_DUMP_AFTER
static FILE *passes_f, *images_f;
static int seq, finished, copied;      // copied: passes and dispatches of the dump frame whose images were copied
static int skip_runs, dump_ubo;        // NX2NAVI_DUMP_SKIP_RUNS, NX2NAVI_DUMP_UBO (vec4 per block, 0 for none)
static int dump_raw, ncopy_shaders;
static uint64_t copy_shaders[16];
static uint32_t want_tex[16][2];       // NX2NAVI_DUMP_TEX: slot id and 0, or width and height
static int nwant_tex;
static uint64_t last_targets;          // targets_sig of the last render pass; 0 after a dispatch
static const char *pass_skip;          // why the current pass copies nothing, or NULL
static volatile int64_t png_bytes, inflight;
static PlatMutex rec_lock = PLAT_MUTEX_INIT, img_lock = PLAT_MUTEX_INIT;

// ---- formats

enum { K_UNORM, K_SNORM, K_UINT, K_SINT, K_FLOAT, K_RGB10A2, K_BGR10A2, K_RG11B10, K_E5, K_R5G6B5, K_D16, K_D24, K_D32F };
enum { M_UNORM, M_SNORM, M_INT, M_FLOAT, M_DEPTH };
typedef struct { VkFormat vk; const char *name; uint8_t bytes, nc, bits, kind, bgr; } Fmt;

#define F(v, n, by, nc, bi, k) {VK_FORMAT_##v, n, by, nc, bi, k, 0}
static const Fmt fmts[] = {
    F(R8_UNORM, "r8", 1, 1, 8, K_UNORM), F(R8_SNORM, "r8sn", 1, 1, 8, K_SNORM), F(R8_UINT, "r8ui", 1, 1, 8, K_UINT),
    F(R8_SINT, "r8i", 1, 1, 8, K_SINT), F(R8_SRGB, "r8srgb", 1, 1, 8, K_UNORM),
    F(R8G8_UNORM, "rg8", 2, 2, 8, K_UNORM), F(R8G8_SNORM, "rg8sn", 2, 2, 8, K_SNORM),
    F(R8G8_UINT, "rg8ui", 2, 2, 8, K_UINT), F(R8G8_SINT, "rg8i", 2, 2, 8, K_SINT),
    F(R8G8B8A8_UNORM, "rgba8", 4, 4, 8, K_UNORM), F(R8G8B8A8_SNORM, "rgba8sn", 4, 4, 8, K_SNORM),
    F(R8G8B8A8_UINT, "rgba8ui", 4, 4, 8, K_UINT), F(R8G8B8A8_SINT, "rgba8i", 4, 4, 8, K_SINT),
    F(R8G8B8A8_SRGB, "rgba8srgb", 4, 4, 8, K_UNORM),
    {VK_FORMAT_B8G8R8A8_UNORM, "bgra8", 4, 4, 8, K_UNORM, 1}, {VK_FORMAT_B8G8R8A8_SRGB, "bgra8srgb", 4, 4, 8, K_UNORM, 1},
    F(A2B10G10R10_UNORM_PACK32, "rgb10a2", 4, 4, 10, K_RGB10A2), F(A2R10G10B10_UNORM_PACK32, "bgr10a2", 4, 4, 10, K_BGR10A2),
    F(R5G6B5_UNORM_PACK16, "r5g6b5", 2, 3, 5, K_R5G6B5),
    F(R16_UNORM, "r16", 2, 1, 16, K_UNORM), F(R16_SNORM, "r16sn", 2, 1, 16, K_SNORM), F(R16_UINT, "r16ui", 2, 1, 16, K_UINT),
    F(R16_SINT, "r16i", 2, 1, 16, K_SINT), F(R16_SFLOAT, "r16f", 2, 1, 16, K_FLOAT),
    F(R16G16_UNORM, "rg16", 4, 2, 16, K_UNORM), F(R16G16_SNORM, "rg16sn", 4, 2, 16, K_SNORM),
    F(R16G16_UINT, "rg16ui", 4, 2, 16, K_UINT), F(R16G16_SINT, "rg16i", 4, 2, 16, K_SINT),
    F(R16G16_SFLOAT, "rg16f", 4, 2, 16, K_FLOAT),
    F(R16G16B16A16_UNORM, "rgba16", 8, 4, 16, K_UNORM), F(R16G16B16A16_SNORM, "rgba16sn", 8, 4, 16, K_SNORM),
    F(R16G16B16A16_UINT, "rgba16ui", 8, 4, 16, K_UINT), F(R16G16B16A16_SINT, "rgba16i", 8, 4, 16, K_SINT),
    F(R16G16B16A16_SFLOAT, "rgba16f", 8, 4, 16, K_FLOAT),
    F(R32_UINT, "r32ui", 4, 1, 32, K_UINT), F(R32_SINT, "r32i", 4, 1, 32, K_SINT), F(R32_SFLOAT, "r32f", 4, 1, 32, K_FLOAT),
    F(R32G32_UINT, "rg32ui", 8, 2, 32, K_UINT), F(R32G32_SINT, "rg32i", 8, 2, 32, K_SINT),
    F(R32G32_SFLOAT, "rg32f", 8, 2, 32, K_FLOAT),
    F(R32G32B32A32_UINT, "rgba32ui", 16, 4, 32, K_UINT), F(R32G32B32A32_SINT, "rgba32i", 16, 4, 32, K_SINT),
    F(R32G32B32A32_SFLOAT, "rgba32f", 16, 4, 32, K_FLOAT),
    F(B10G11R11_UFLOAT_PACK32, "r11g11b10f", 4, 3, 11, K_RG11B10), F(E5B9G9R9_UFLOAT_PACK32, "rgb9e5", 4, 3, 9, K_E5),
    // depth aspect of depth formats, as vkCmdCopyImageToBuffer writes it
    F(D16_UNORM, "d16", 2, 1, 16, K_D16), F(D16_UNORM_S8_UINT, "d16s8", 2, 1, 16, K_D16),
    F(X8_D24_UNORM_PACK32, "d24", 4, 1, 24, K_D24), F(D24_UNORM_S8_UINT, "d24s8", 4, 1, 24, K_D24),
    F(D32_SFLOAT, "d32f", 4, 1, 32, K_D32F), F(D32_SFLOAT_S8_UINT, "d32fs8", 4, 1, 32, K_D32F),
};
#undef F

static const Fmt *fmt_of(VkFormat f) {
    for (size_t i = 0; i < sizeof fmts / sizeof fmts[0]; i++)
        if (fmts[i].vk == f) return &fmts[i];
    return NULL;
}

static const char *fmt_name(VkFormat f, char tmp[16]) {
    const Fmt *d = fmt_of(f);
    if (d) return d->name;
    snprintf(tmp, 16, "vk%d", (int)f);
    return tmp;
}

static int mode_of(const Fmt *f) {
    switch (f->kind) {
    case K_SNORM: return M_SNORM;
    case K_UINT: case K_SINT: return M_INT;
    case K_FLOAT: case K_RG11B10: case K_E5: return M_FLOAT;
    case K_D16: case K_D24: case K_D32F: return M_DEPTH;
    default: return M_UNORM;
    }
}

// Unsigned small float with a 5-bit exponent and mbits of mantissa (half: 10, R11G11B10: 6 and 5).
static float ufloat(uint32_t v, int mbits) {
    uint32_t e = v >> mbits, m = v & ((1u << mbits) - 1);
    if (e == 0) return ldexpf((float)m, -14 - mbits);
    if (e == 31) return m ? NAN : INFINITY;
    return ldexpf((float)(m | 1u << mbits), (int)e - 15 - mbits);
}

static int32_t sext(uint32_t v, int bits) { return bits >= 32 ? (int32_t)v : (int32_t)(v << (32 - bits)) >> (32 - bits); }

static void decode(const Fmt *f, const uint8_t *p, float o[4]) {
    uint32_t u = 0;
    o[0] = o[1] = o[2] = 0, o[3] = 1;
    switch (f->kind) {
    case K_RGB10A2: case K_BGR10A2:
        memcpy(&u, p, 4);
        o[0] = (u & 1023) / 1023.f, o[1] = (u >> 10 & 1023) / 1023.f, o[2] = (u >> 20 & 1023) / 1023.f, o[3] = (u >> 30) / 3.f;
        if (f->kind == K_BGR10A2) { float t = o[0]; o[0] = o[2], o[2] = t; }
        return;
    case K_RG11B10:
        memcpy(&u, p, 4);
        o[0] = ufloat(u & 2047, 6), o[1] = ufloat(u >> 11 & 2047, 6), o[2] = ufloat(u >> 22 & 1023, 5);
        return;
    case K_E5: {
        memcpy(&u, p, 4);
        float s = ldexpf(1.f, (int)(u >> 27) - 24);
        o[0] = (u & 511) * s, o[1] = (u >> 9 & 511) * s, o[2] = (u >> 18 & 511) * s;
        return;
    }
    case K_R5G6B5:
        memcpy(&u, p, 2);
        o[0] = (u >> 11 & 31) / 31.f, o[1] = (u >> 5 & 63) / 63.f, o[2] = (u & 31) / 31.f;
        return;
    case K_D16: memcpy(&u, p, 2); o[0] = u / 65535.f; return;
    case K_D24: memcpy(&u, p, 4); o[0] = (u & 0xffffff) / 16777215.f; return;
    case K_D32F: memcpy(&o[0], p, 4); return;
    }
    int cb = f->bits / 8;
    for (int c = 0; c < f->nc; c++) {
        u = 0;
        memcpy(&u, p + c * cb, (size_t)cb);
        float x;
        switch (f->kind) {
        case K_UNORM: x = (float)u / (float)((1ull << f->bits) - 1); break;
        case K_SNORM: x = fmaxf((float)sext(u, f->bits) / (float)((1u << (f->bits - 1)) - 1), -1.f); break;
        case K_UINT: x = (float)u; break;
        case K_SINT: x = (float)sext(u, f->bits); break;
        default:
            if (f->bits == 16) x = (u & 0x8000 ? -1.f : 1.f) * ufloat(u & 0x7fff, 10);
            else memcpy(&x, &u, 4);
        }
        o[c] = x;
    }
    if (f->bgr) { float t = o[0]; o[0] = o[2], o[2] = t; }
}

// ---- conversion on the completion thread

typedef struct {
    VkBuffer buf;
    VmaAllocation alloc;
    const uint8_t *map;
    uint64_t bytes;
    const Fmt *f;
    uint32_t w, h, d;   // d: 3D slices, laid side by side in the PNG
    char name[160];
} Job;

// Distinct raw texel values among rows [y0, y1) of every slice.
static uint64_t distinct(const Job *j, uint32_t y0, uint32_t y1) {
    uint64_t n = (uint64_t)j->w * (y1 - y0) * j->d, cap = 1024;
    while (cap < 2 * n) cap *= 2;
    uint64_t *set = calloc(cap, sizeof *set), count = 0;
    if (!set) return 0;
    for (uint32_t z = 0; z < j->d; z++)
        for (uint32_t y = y0; y < y1; y++)
            for (uint32_t x = 0; x < j->w; x++) {
                const uint8_t *p = j->map + (((uint64_t)z * j->h + y) * j->w + x) * j->f->bytes;
                uint64_t a = 0, b = 0;
                memcpy(&a, p, j->f->bytes < 8 ? j->f->bytes : 8);
                if (j->f->bytes > 8) memcpy(&b, p + 8, j->f->bytes - 8u);
                uint64_t k = (a ^ (b * 0x9E3779B97F4A7C15ull)) + 1;
                if (!k) k = 1;
                uint64_t i = (k * 0xff51afd7ed558ccdull) >> 17;
                for (;; i++) {
                    uint64_t *s = &set[i & (cap - 1)];
                    if (*s == k) break;
                    if (!*s) { *s = k, count++; break; }
                }
            }
    free(set);
    return count;
}

static uint8_t u8(float x) { return x > 0 ? x < 1 ? (uint8_t)(x * 255.f + .5f) : 255 : 0; }

static void write_images(const Job *j) {
    uint32_t pw = j->w * j->d, ph = j->h;
    uint64_t n = (uint64_t)pw * ph;
    float *px = malloc(n * 16);
    uint8_t *out = malloc(n * 4), *isnan_ = calloc(n, 1);   // isnan_: a pixel with a NaN channel, drawn magenta
    if (!px || !out || !isnan_) rt_fatal("vk_dump: out of memory for %s", j->name);
    float mn[4] = {INFINITY, INFINITY, INFINITY, INFINITY}, mx[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
    uint64_t nans = 0;
    for (uint32_t z = 0; z < j->d; z++)
        for (uint32_t y = 0; y < j->h; y++)
            for (uint32_t x = 0; x < j->w; x++) {
                float *o = px + 4 * ((uint64_t)y * pw + (uint64_t)z * j->w + x);
                decode(j->f, j->map + (((uint64_t)z * j->h + y) * j->w + x) * j->f->bytes, o);
                for (int c = 0; c < 4; c++) {
                    if (isnan(o[c])) { nans++, o[c] = 0, isnan_[(uint64_t)y * pw + (uint64_t)z * j->w + x] = 1; continue; }
                    if (o[c] < mn[c]) mn[c] = o[c];
                    if (o[c] > mx[c]) mx[c] = o[c];
                }
            }
    int mode = mode_of(j->f), grey = j->f->nc == 1;
    float scale = 1, lo = mn[0], range = mx[0] - mn[0];
    if (mode == M_INT) {
        float m = 1;
        for (int c = 0; c < j->f->nc && c < 3; c++) m = fmaxf(m, fmaxf(fabsf(mx[c]), fabsf(mn[c])));
        scale = 1.f / m;
    }
    for (int pass = 0; pass < (mode == M_FLOAT ? 2 : 1); pass++) {
        for (uint64_t i = 0; i < n; i++) {
            const float *o = px + 4 * i;
            for (int c = 0; c < 3; c++) {
                float x = o[grey ? 0 : c];
                if (mode == M_SNORM) x = x * .5f + .5f;
                else if (mode == M_INT) x *= scale;
                else if (mode == M_DEPTH) x = range > 0 ? (x - lo) / range : 0;
                else if (mode == M_FLOAT && pass) x = x > 0 ? x / (1 + x) : 0;
                out[4 * i + c] = u8(x);
            }
            out[4 * i + 3] = 255;
            if (isnan_[i]) out[4 * i] = 255, out[4 * i + 1] = 0, out[4 * i + 2] = 255;
        }
        char path[600];
        snprintf(path, sizeof path, "%s/%s%s.png", dir, j->name, pass ? "_tm" : "");
        rt_write_png(path, out, pw, ph);
    }
    uint64_t all = distinct(j, 0, j->h), top = distinct(j, 0, j->h / 4 ? j->h / 4 : 1);
    plat_mutex_lock(&img_lock);
    if (images_f) {
        fprintf(images_f, "%s %ux%ux%u min %g %g %g %g max %g %g %g %g distinct %llu top-quarter %llu nan %llu\n", j->name,
                j->w, j->h, j->d, mn[0], mn[1], mn[2], mn[3], mx[0], mx[1], mx[2], mx[3], (unsigned long long)all,
                (unsigned long long)top, (unsigned long long)nans);
        fflush(images_f);
    }
    plat_mutex_unlock(&img_lock);
    free(out);
    free(isnan_);
    free(px);
}

static void job_done(void *arg) {
    Job *j = arg;
    VmaAllocator vma = vk_ctx()->vma;
    vmaInvalidateAllocation(vma, j->alloc, 0, VK_WHOLE_SIZE);
    write_images(j);
    // Raw captures retain the exact texel bytes. BC blocks always get a .bin because the PNG's integer mapping
    // loses their bits; other formats get one when DUMP_RAW is enabled.
    size_t nl = strlen(j->name);
    if (dump_raw || (nl > 7 && !strcmp(j->name + nl - 7, "_blocks"))) {
        char path[600];
        snprintf(path, sizeof path, "%s/%s.bin", dir, j->name);
        FILE *f = fopen(path, "wb");
        if (f) fwrite(j->map, 1, j->bytes, f), fclose(f);
    }
    vmaDestroyBuffer(vma, j->buf, j->alloc);
    plat_add64(&inflight, -(int64_t)j->bytes);
    free(j);
}

// ---- recording

// Records the copy of one subresource into a host buffer and queues its PNGs. Returns NULL, or why it was skipped.
// The buffer is w x h x d texels of vf; the image region is `ext` image texels at depth plane z (differs for the
// blocks of a BC image read as an uncompressed format of the block size, and for a 3D slice).
static const char *copy_region(VkExec *ex, VkImage image, VkFormat vf, VkImageAspectFlags aspect, uint32_t level,
                               uint32_t layer, uint32_t z, VkExtent3D ext, uint32_t w, uint32_t h, uint32_t d,
                               const char *name) {
    const Fmt *f = fmt_of(vf);
    if (!f) return "format not decoded";
    if (__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1 != dump_frame && (uint64_t)w * h * d > 65536)
        return "logged frame, large image";
    if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    else if (aspect != VK_IMAGE_ASPECT_COLOR_BIT) return "stencil only";
    uint64_t bytes = (uint64_t)w * h * d * f->bytes;
    uint64_t est = ((uint64_t)h * (1 + 4ull * w * d) + 128) * (mode_of(f) == M_FLOAT ? 2 : 1);
    if ((uint64_t)png_bytes + est > PNG_CAP) return "PNG cap reached";
    if ((uint64_t)inflight + bytes > INFLIGHT_CAP) return "too many copies in flight";
    Job *j = calloc(1, sizeof *j);
    if (!j) rt_fatal("vk_dump: out of memory");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &j->buf, &j->alloc, &info) != VK_SUCCESS || !info.pMappedData) {
        free(j);
        return "host buffer allocation failed";
    }
    j->map = info.pMappedData, j->bytes = bytes, j->f = f, j->w = w, j->h = h, j->d = d;
    snprintf(j->name, sizeof j->name, "%s", name);
    VkMemoryBarrier2 pre = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                            .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                            .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
    VkMemoryBarrier2 post = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                             .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                             .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                             .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                             .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &pre};
    vkCmdPipelineBarrier2(ex->cb, &di);
    VkBufferImageCopy r = {.imageSubresource = {aspect, level, layer, 1}, .imageOffset = {0, 0, (int32_t)z},
                           .imageExtent = ext};   // packed rows: a BC row of ext.width texels is w blocks
    vkCmdCopyImageToBuffer(ex->cb, image, VK_IMAGE_LAYOUT_GENERAL, j->buf, 1, &r);
    di.pMemoryBarriers = &post;
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded += 3;
    plat_add64(&png_bytes, (int64_t)est);
    plat_add64(&inflight, (int64_t)bytes);
    vk_completion_add(ex->fence, job_done, j);
    return NULL;
}

static const char *copy_out(VkExec *ex, VkImage image, VkFormat vf, VkImageAspectFlags aspect, uint32_t level,
                            uint32_t layer, uint32_t w, uint32_t h, uint32_t d, const char *name) {
    return copy_region(ex, image, vf, aspect, level, layer, 0, (VkExtent3D){w, h, d}, w, h, d, name);
}

// ---- NX2NAVI_DUMP_BUF=<addr>:<size>[:<off>:<stride>:<w>:<h>]: after every pass and dispatch that is logged, the GPU
// buffer range is copied to a host buffer; when it differs from the previous copy it goes to buf_<pass>.bin, and
// bufs.txt gets the changed bytes' extent plus, with the grid arguments, one map per dword of the w x h records of
// `stride` bytes at `off` (each record a letter: its value's index in the table under the map).

static uint64_t bd_addr, bd_size, bd_off, bd_stride;
static uint32_t bd_w, bd_h;
static uint8_t *bd_prev;
static FILE *bufs_f;

typedef struct { VkBuffer buf; VmaAllocation alloc; const uint8_t *map; int pass; } BufJob;

static void buf_grid(const uint8_t *m) {
    for (uint32_t dw = 0; dw < bd_stride / 4; dw++) {
        uint32_t vals[52];
        int nv = 0, over = 0;
        fprintf(bufs_f, "  dword %u:\n", dw);
        for (uint32_t y = 0; y < bd_h; y++) {
            char line[260];
            uint32_t x;
            for (x = 0; x < bd_w && x < 256; x++) {
                uint64_t o = bd_off + ((uint64_t)y * bd_w + x) * bd_stride + dw * 4;
                if (o + 4 > bd_size) { line[x] = ' '; continue; }
                uint32_t v;
                memcpy(&v, m + o, 4);
                int k = 0;
                while (k < nv && vals[k] != v) k++;
                if (k == nv) {
                    if (nv < 52) vals[nv++] = v;
                    else { over = 1; line[x] = '?'; continue; }
                }
                line[x] = (char)(k < 26 ? 'a' + k : 'A' + k - 26);
            }
            line[x] = 0;
            fprintf(bufs_f, "    %2u %s\n", y, line);
        }
        for (int k = 0; k < nv; k++) fprintf(bufs_f, "%s%c=%08x", k % 8 ? " " : "    ", k < 26 ? 'a' + k : 'A' + k - 26, vals[k]);
        fprintf(bufs_f, "%s\n", over ? " (more: ?)" : "");
    }
}

static void buf_done(void *arg) {
    BufJob *j = arg;
    vmaInvalidateAllocation(vk_ctx()->vma, j->alloc, 0, VK_WHOLE_SIZE);
    plat_mutex_lock(&img_lock);
    if (bufs_f) {
        uint64_t lo = bd_size, hi = 0;
        for (uint64_t i = 0; i < bd_size; i++)
            if (!bd_prev || j->map[i] != bd_prev[i]) { if (i < lo) lo = i; hi = i + 1; }
        if (hi) {
            char path[600];
            snprintf(path, sizeof path, "%s/buf_%04d.bin", dir, j->pass);
            FILE *f = fopen(path, "wb");
            if (f) fwrite(j->map, 1, bd_size, f), fclose(f);
            fprintf(bufs_f, "after pass %04d: bytes %#llx..%#llx changed\n", j->pass, (unsigned long long)lo,
                    (unsigned long long)hi);
            if (bd_stride) buf_grid(j->map);
            fflush(bufs_f);
            if (!bd_prev) bd_prev = malloc(bd_size);
            if (bd_prev) memcpy(bd_prev, j->map, bd_size);
        }
    }
    plat_mutex_unlock(&img_lock);
    vmaDestroyBuffer(vk_ctx()->vma, j->buf, j->alloc);
    free(j);
}

static void buf_snap(VkExec *ex, int pass) {
    if (!bd_size) return;
    VkMemSpan s;
    if (!vk_mem_resolve(bd_addr, bd_size, &s) || s.avail < bd_size) return;
    VK_MEM_PICK(s);   // the GPU's view of the buffer
    BufJob *j = calloc(1, sizeof *j);
    if (!j) rt_fatal("vk_dump: out of memory");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bd_size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &j->buf, &j->alloc, &info) != VK_SUCCESS || !info.pMappedData) {
        free(j);
        return;
    }
    j->map = info.pMappedData, j->pass = pass;
    VkMemoryBarrier2 mb[2] = {
        {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
         .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT},
        {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
         .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb[0]};
    vkCmdPipelineBarrier2(ex->cb, &di);
    VkBufferCopy c = {s.offset, 0, bd_size};
    vkCmdCopyBuffer(ex->cb, s.buffer, j->buf, 1, &c);
    di.pMemoryBarriers = &mb[1];
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded += 3;
    vk_completion_add(ex->fence, buf_done, j);
}

static int active(void) {   // passes_f is NULL before a dump that opens its files when it starts (open_files)
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    return !finished && passes_f && (f == dump_frame || (log_from && f >= log_from && f < dump_frame));
}
static uint64_t frame_now(void) { return __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1; }

typedef struct { char *s; size_t n, cap; } Str;

__attribute__((format(printf, 2, 3))) static void sp(Str *b, const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    int k = vsnprintf(NULL, 0, fmt, a);
    va_end(a);
    if (k < 0) return;
    if (b->n + (size_t)k + 1 > b->cap) {
        b->cap = (b->n + (size_t)k + 1) * 2;
        b->s = realloc(b->s, b->cap);
        if (!b->s) rt_fatal("vk_dump: out of memory");
    }
    va_start(a, fmt);
    vsnprintf(b->s + b->n, (size_t)k + 1, fmt, a);
    va_end(a);
    b->n += (size_t)k;
}

// SPIR-V hash of a shader; the first time, its SPIR-V goes to sh_<hash>.spv and its guest program to sh_<hash>.code.
static uint64_t shader_hash(const GfxShader *s) {
    static struct { const GfxShader *s; uint64_t h; } tab[1024];
    static int n;
    if (!s) return 0;
    for (int i = 0; i < n; i++)
        if (tab[i].s == s) return tab[i].h;
    uint64_t words = 0, h = 0xcbf29ce484222325ull;
    const uint32_t *w = gfx_shader_spirv(s, &words);
    const uint8_t *b = (const uint8_t *)w;
    for (uint64_t i = 0; i < words * 4; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    char path[600];
    snprintf(path, sizeof path, "%s/sh_%016llx.spv", dir, (unsigned long long)h);
    FILE *f = fopen(path, "wb");
    if (f) fwrite(w, 4, (size_t)words, f), fclose(f);
    snprintf(path, sizeof path, "%s/sh_%016llx.code", dir, (unsigned long long)h);   // the guest program it came from
    gfx_shader_write_code(s, path);
    if (n < 1024) tab[n].s = s, tab[n].h = h, n++;
    return h;
}

// Per pass: distinct pipeline + texture sets, draws, 3D textures to copy at its end.
typedef struct { VkPipeline p; uint64_t th; int draws; char *text; } Ent;
static Ent ents[MAX_ENTS];
static int nents, ents_over, pass_draws;
typedef struct { VkImage image; VkFormat format; uint32_t w, h, d; int id, is2d; uint32_t layer, layers; } Lut;
static Lut luts[MAX_LUTS];          // queued for the next pass end or dispatch
static int nluts;
static VkImage luts_done[MAX_LUTS];
static int nluts_done;

static void queue_lut(const VkTexImage *t, int id) {
    if (frame_now() != dump_frame) return;
    for (int i = 0; i < nluts_done; i++)
        if (luts_done[i] == t->image) return;
    for (int i = 0; i < nluts; i++)
        if (luts[i].image == t->image) return;
    if (nluts_done == MAX_LUTS || nluts == MAX_LUTS) return;
    uint32_t nl = t->type != VK_IMAGE_TYPE_3D && t->layers > 1 ? t->layers : 1;
    for (uint32_t k = 0; k < nl && nluts < MAX_LUTS; k++)
        luts[nluts++] = (Lut){t->image, t->format, t->width, t->height, t->type == VK_IMAGE_TYPE_3D ? t->depth : 1, id,
                              t->type != VK_IMAGE_TYPE_3D, k, nl};
}

// The uncompressed format of a BC format's block size, UNDEFINED for other formats: a BC texture is dumped as its raw
// blocks (one texel per block), which shows at least whether it was ever written.
static VkFormat block_format(VkFormat f) {
    if (f == VK_FORMAT_BC4_UNORM_BLOCK || f == VK_FORMAT_BC4_SNORM_BLOCK ||
        (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK))
        return VK_FORMAT_R32G32_UINT;
    if (f >= VK_FORMAT_BC2_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK) return VK_FORMAT_R32G32B32A32_UINT;
    return VK_FORMAT_UNDEFINED;
}

static void flush_luts(VkExec *ex, int n) {
    char fn[16];
    for (int i = 0; i < nluts; i++) {
        const Lut *l = &luts[i];
        char name[160], lay[16] = "";
        VkFormat bf = block_format(l->format);
        if (l->layers > 1) snprintf(lay, sizeof lay, "L%u", l->layer);
        snprintf(name, sizeof name, "pass_%04d_%s%d%s_%ux%ux%u_%d_%s%s", n, l->is2d ? "tex" : "lut", l->id, lay, l->w, l->h,
                 l->d, (int)l->format, fmt_name(l->format, fn), bf ? "_blocks" : "");
        // a sampled depth texture copies its depth aspect (the colour aspect of a depth image read as zeros)
        VkImageAspectFlags asp = l->format >= VK_FORMAT_D16_UNORM && l->format <= VK_FORMAT_D32_SFLOAT_S8_UINT
                                     ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        const char *why = bf ? copy_region(ex, l->image, bf, VK_IMAGE_ASPECT_COLOR_BIT, 0, l->layer, 0,
                                           (VkExtent3D){l->w, l->h, l->d}, (l->w + 3) / 4, (l->h + 3) / 4, l->d, name)
                             : copy_out(ex, l->image, l->format, asp, 0, l->layer, l->w, l->h, l->d, name);
        fprintf(passes_f, "  %s tex %d -> %s.png%s%s\n", l->is2d ? "2D" : "3D", l->id, name, why ? " skipped: " : "",
                why ? why : "");
        if (!why && nluts_done < MAX_LUTS) {
            int known = 0;
            for (int k = 0; k < nluts_done; k++) known |= luts_done[k] == l->image;
            if (!known) luts_done[nluts_done++] = l->image;
        }
    }
    nluts = 0;
}

static int nvn_stage(int gfx_stage) {   // the NVN stage numbers VkExec's slot arrays use: 6 task, 7 mesh
    return gfx_stage == GFX_STAGE_VS ? 0 : gfx_stage == GFX_STAGE_FS ? 1 : gfx_stage == GFX_STAGE_TS ? 6
         : gfx_stage == GFX_STAGE_MS ? 7 : 5;
}

static const char *decl_type(uint16_t t, char tmp[24]) {
    static const char *base[] = {"?", "1D", "buffer", "2D", "3D", "cube"};
    snprintf(tmp, 24, "%s%s%s%s", base[(t & 0xff) < 6 ? t & 0xff : 0], t & 0x100 ? "Array" : "", t & 0x200 ? "MS" : "",
             t & 0x400 ? "Shadow" : "");
    return tmp;
}

static const char *wrap_name(int w) {
    static const char *n[] = {"clamp", "repeat", "mirror_clamp", "mirror_clamp_edge", "mirror_clamp_border",
                              "clamp_border", "mirrored_repeat", "clamp_edge"};
    return w >= 0 && w < 8 ? n[w] : "?";
}

static uint64_t handle_of(const VkExec *ex, int st, int slot, int img) {
    if (img) return slot >= 0 && slot < 32 ? ex->img[st][slot] : 0;
    return slot >= 0 && slot < 128 ? ex->tex[st][slot] : 0;
}

// The textures, images and samplers of info's bindings; 3D textures are queued for a copy.
static void describe_bindings(Str *b, const VkExec *ex, const VkPipeInfo *info) {
    static const char *stn[8] = {"vs", "fs", "?", "?", "?", "cs", "ts", "ms"};
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *bd = &info->bindings[i];
        const GfxShaderRes *r = bd->res;
        if (r && bd->kind == GFX_RES_SSBO) {   // where the address lives (c0: the stage's NVN slot table) and the stage's slots
            int st = nvn_stage(bd->stage);
            sp(b, "    %s ssbo at c%d+%#x%s, bound:", stn[st], r->cbuf_slot, r->handle * 4, r->flags & 1 ? " written" : "");
            if (r->ptr) sp(b, " (address at +%#x of that buffer)", r->ptr - 1u);
            if (st == 5 && r->cbuf_slot == 0 && r->handle * 4 >= 0x10 && r->handle * 4 + 12 <= 0x10 + (int)sizeof ex->cs_push) {
                uint64_t pa; uint32_t ps;   // an NVN2 compute push-constant pointer {address, size}
                memcpy(&pa, ex->cs_push + r->handle * 4 - 0x10, 8), memcpy(&ps, ex->cs_push + r->handle * 4 - 0x10 + 8, 4);
                sp(b, " (push pointer %#llx+%#x)", (unsigned long long)pa, ps);
            }
            if (st == 5 || st == 6 || st == 7) {   // the address and how far its backing span reaches (what a size-0 binding covers)
                uint64_t ca = vk_exec_storage_addr(ex, st, r);
                VkMemSpan ms;
                sp(b, " -> %#llx", (unsigned long long)ca);
                if (ca && vk_mem_resolve(ca, 1, &ms)) sp(b, " (span %#llx)", (unsigned long long)ms.avail);
            }
            for (int k = 0; k < 16; k++)
                if (ex->ssbo[st][k].addr) {
                    VkDescriptorBufferInfo cb;   // "(copy)": a binding that only reads it uses the submit-time copy (vk_usnap.c)
                    int copy = vk_usnap_find(ex->ssbo[st][k].addr, ex->ssbo[st][k].size, 1, &cb);
                    sp(b, " %d=%#llx+%#llx%s", k, (unsigned long long)ex->ssbo[st][k].addr, (unsigned long long)ex->ssbo[st][k].size,
                       copy ? "(copy)" : "");
                }
            sp(b, "\n");
            // NX2NAVI_DUMP_UBO: also the small storage buffers' guest bytes (GPU results as of the last write-back); per-view
            // constants (exposure and such) live in such buffers.
            for (int k = 0; dump_ubo && k < 16; k++) {
                const p_bind_buf *u = &ex->ssbo[st][k];
                VkMemSpan ms;
                if (!u->addr || !u->size || u->size > 0x100 || !vk_mem_resolve(u->addr, 1, &ms)) continue;
                const float *f = (const float *)(uintptr_t)u->addr;
                sp(b, "      %s ssbo %d %#llx:", stn[st], k, (unsigned long long)u->addr);
                for (uint64_t q = 0; q * 16 < u->size && q < (uint64_t)dump_ubo; q++)
                    sp(b, " [%llu] %g %g %g %g", (unsigned long long)q, f[4 * q], f[4 * q + 1], f[4 * q + 2], f[4 * q + 3]);
                sp(b, "\n");
            }
            continue;
        }
        if (r && bd->kind == GFX_RES_UBO && dump_ubo && r->slot >= 3 && r->slot - 3 < 18) {
            int st = nvn_stage(bd->stage);
            const p_bind_buf *u = &ex->ubo[st][r->slot - 3];
            VkMemSpan ms;
            sp(b, "    %s ubo c%d addr %#llx size %#llx", stn[st], r->slot, (unsigned long long)u->addr, (unsigned long long)u->size);
            if (u->addr && vk_mem_resolve(u->addr, 1, &ms)) {
                uint64_t n = u->size && u->size < ms.avail ? u->size : ms.avail;
                const float *f = (const float *)(uintptr_t)u->addr;
                for (uint64_t k = 0; k < (uint64_t)dump_ubo && (k + 1) * 16 <= n; k++)
                    sp(b, "%s[%llu] %g %g %g %g", k % 4 ? " " : "\n      ", (unsigned long long)k, f[4 * k], f[4 * k + 1],
                       f[4 * k + 2], f[4 * k + 3]);
            } else {
                sp(b, " (not mapped)");
            }
            sp(b, "\n");
            continue;
        }
        if (!r || (bd->kind != GFX_RES_TEX && bd->kind != GFX_RES_IMG)) continue;
        int st = nvn_stage(bd->stage), img = bd->kind == GFX_RES_IMG;
        char dt[24], fn[16];
        if (bd->type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || bd->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
            sp(b, "    %s %s slot %d: buffer texture (zero view bound)\n", stn[st], img ? "img" : "tex", r->slot);
            continue;
        }
        for (uint32_t k = 0; k < (bd->count ? bd->count : 1u); k++) {
            int slot = r->slot + (int)k;
            uint64_t h = handle_of(ex, st, slot, img);
            sp(b, "    %s %s slot %d decl %s%s: ", stn[st], img ? "img" : "tex", slot, decl_type(r->type, dt),
               img && (r->flags & 4) ? " store" : "");
            if (!h) { sp(b, "no handle (dummy)\n"); continue; }
            view_t v;
            int hv = 0;
            const VkTexImage *t = vk_texture_by_id(NVN_HANDLE_TEX(h), &v, &hv);
            if (!t) { sp(b, "tex %d unresolved (dummy)\n", NVN_HANDLE_TEX(h)); continue; }
            static const char *vt[] = {"1D", "2D", "3D", "cube", "1DArray", "2DArray", "cubeArray"};
            sp(b, "tex %d %s(%d) %ux%ux%u levels %u layers %u image %s vkimage %p", NVN_HANDLE_TEX(h), fmt_name(t->format, fn),
               (int)t->format, t->width, t->height, t->depth, t->levels, t->layers,
               t->view_type <= 6 ? vt[t->view_type] : "?", (void *)t->image);
            if (hv && v.set_format) sp(b, " view-nvn-fmt %#x", v.format);
            if (hv && v.set_target) sp(b, " view-target %u", v.target);
            if (hv && v.set_levels) sp(b, " view-levels %u+%u", v.base_level, v.nlevels);
            if (hv && v.set_layers) sp(b, " view-layers %u+%u", v.min_layer, v.nlayers);
            samp_par s;
            if (img) {
                // storage image: no sampler
            } else if (r->separate) {
                sp(b, " | separate sampler (not replayed: default sampler)");
            } else if (vk_sampler_params(NVN_HANDLE_SAMP(h), &s)) {
                static const char *minn[] = {"N", "L", "N_mipN", "L_mipN", "N_mipL", "L_mipL"};
                int mf = s.minf;
                sp(b, " | samp %d: mag %s min %s (vk min %s mip %s) wrap %s/%s/%s aniso %g lod %g..%g bias %g%s",
                   NVN_HANDLE_SAMP(h), s.magf == 1 ? "L" : "N", mf < 6 ? minn[mf] : "?",
                   mf == 1 || mf == 3 || mf == 5 ? "L" : "N", mf <= 1 ? "none" : mf >= 4 ? "L" : "N",
                   wrap_name(s.ws), wrap_name(s.wt), wrap_name(s.wr), s.aniso, s.lod_min, s.lod_max, s.lod_bias,
                   s.cmp_mode ? " compare" : "");
                int bw = 0;   // a border wrap: its colour, as the sampler builder stored it
                for (int k = 0; k < 3; k++) {
                    uint8_t wm = k == 0 ? s.ws : k == 1 ? s.wt : s.wr;
                    bw |= wm == NVN_WRAP_MODE_CLAMP_TO_BORDER || wm == NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER;
                }
                if (bw) sp(b, " border %g,%g,%g,%g", s.border[0], s.border[1], s.border[2], s.border[3]);
            } else {
                sp(b, " | samp %d empty (default sampler)", NVN_HANDLE_SAMP(h));
            }
            uint32_t ups;
            int last, wr;
            vk_texture_debug(t, &ups, &last, &wr);
            sp(b, " | addr %#llx mirrored %d gpu_owned %d bound %d uploads %u last-check %d written-since %d",
               (unsigned long long)t->addr, t->mirrored, t->gpu_owned, t->bound, ups, last, wr);
            uint8_t ssw[4] = {0};
            vk_texture_slot_swz(NVN_HANDLE_TEX(h), ssw);
            sp(b, " | swz image %u%u%u%u slot %u%u%u%u", t->swz[0], t->swz[1], t->swz[2], t->swz[3], ssw[0], ssw[1], ssw[2],
               ssw[3]);
            if (hv && v.set_swizzle) sp(b, " view %u%u%u%u", v.swz[0], v.swz[1], v.swz[2], v.swz[3]);
            sp(b, "\n");
            int wanted = 0;
            for (int k = 0; k < nwant_tex; k++)
                wanted |= want_tex[k][1] ? want_tex[k][0] == t->width && want_tex[k][1] == t->height
                                         : want_tex[k][0] == (uint32_t)NVN_HANDLE_TEX(h);
            if (t->type == VK_IMAGE_TYPE_3D) queue_lut(t, NVN_HANDLE_TEX(h));
            else if (wanted && t->type == VK_IMAGE_TYPE_2D && t->samples == 1) queue_lut(t, NVN_HANDLE_TEX(h));
            else if (!img && t->type == VK_IMAGE_TYPE_2D && t->samples == 1 && (uint64_t)t->width * t->height <= TEX2D_CAP &&
                     (t->format == VK_FORMAT_R8_UNORM || t->format == VK_FORMAT_R8G8_UNORM ||
                      t->format == VK_FORMAT_R8G8B8A8_UNORM || t->format == VK_FORMAT_R8G8B8A8_SRGB ||
                      t->format == VK_FORMAT_B8G8R8A8_UNORM || t->format == VK_FORMAT_B8G8R8A8_SRGB))
                queue_lut(t, NVN_HANDLE_TEX(h));
        }
    }
}

static uint64_t bindings_hash(const VkExec *ex, const VkPipeInfo *info) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *bd = &info->bindings[i];
        if (bd->res && bd->kind == GFX_RES_SSBO) {
            int st = nvn_stage(bd->stage), slot = bd->res->slot;
            if (slot >= 0 && slot < 16) h = (h ^ ex->ssbo[st][slot].addr) * 0x100000001b3ull;
            continue;
        }
        if (!bd->res || (bd->kind != GFX_RES_TEX && bd->kind != GFX_RES_IMG)) continue;
        for (uint32_t k = 0; k < (bd->count ? bd->count : 1u); k++)
            h = (h ^ handle_of(ex, nvn_stage(bd->stage), bd->res->slot + (int)k, bd->kind == GFX_RES_IMG)) * 0x100000001b3ull;
    }
    return h;
}

// ---- mesh primitive counts: with VkCtx.has_mesh_queries, each mesh draw of the dumped frame runs inside a
// VK_QUERY_TYPE_MESH_PRIMITIVES_GENERATED_EXT query, and mesh_prims.txt (written by vk_dump_present) lists what each
// draw's mesh shader emitted, with its pass, program and depth state.
// A pipeline statistics query runs alongside: task and mesh shader invocations, primitives the clipper got and gave
// out, and fragment shader invocations (a triangle with a NaN or degenerate vertex is emitted but never clipped).
enum { MQ_MAX = 1024, MQ_STATS = 5 };
static const VkQueryPipelineStatisticFlags mq_stat_bits =
    VK_QUERY_PIPELINE_STATISTIC_TASK_SHADER_INVOCATIONS_BIT_EXT | VK_QUERY_PIPELINE_STATISTIC_MESH_SHADER_INVOCATIONS_BIT_EXT |
    VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
    VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
typedef struct { int pass; unsigned long long prog, ts, ms; unsigned dt, dw, df; } MqDraw;
static VkQueryPool mq_pool, mq_stats;
static int mq_failed;
static uint32_t mq_n;                  // queries begun in the dumped frame
static MqDraw mq_draw[MQ_MAX], mq_next;   // mq_next: the mesh draw vk_dump_draw saw last

int vk_dump_mesh_begin(VkExec *ex) {
    VkCtx *c = vk_ctx();
    if (!c->has_mesh_queries) return -1;
    int q = -1;
    plat_mutex_lock(&rec_lock);
    if (!active() || frame_now() != dump_frame || mq_n >= MQ_MAX || mq_failed) goto out;
    if (!mq_pool) {
        VkQueryPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType = VK_QUERY_TYPE_MESH_PRIMITIVES_GENERATED_EXT, .queryCount = MQ_MAX};
        if (vkCreateQueryPool(c->device, &ci, NULL, &mq_pool) != VK_SUCCESS) {
            mq_pool = VK_NULL_HANDLE, mq_failed = 1;
            rt_log("vk_dump: the mesh primitive query pool could not be created");
            goto out;
        }
        VkQueryPoolCreateInfo si = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS, .queryCount = MQ_MAX,
                                    .pipelineStatistics = mq_stat_bits};
        if (vkCreateQueryPool(c->device, &si, NULL, &mq_stats) != VK_SUCCESS) mq_stats = VK_NULL_HANDLE;
    }
    if (!mq_n) {   // the last dump's queries finished at its present
        vkResetQueryPool(c->device, mq_pool, 0, MQ_MAX);
        if (mq_stats) vkResetQueryPool(c->device, mq_stats, 0, MQ_MAX);
    }
    q = (int)mq_n++;
    mq_draw[q] = mq_next;
    mq_draw[q].pass = seq;   // the number this pass gets when it ends
    vkCmdBeginQuery(ex->cb, mq_pool, (uint32_t)q, 0);
    if (mq_stats) vkCmdBeginQuery(ex->cb, mq_stats, (uint32_t)q, 0);
    ex->recorded += 2;
out:
    plat_mutex_unlock(&rec_lock);
    return q;
}

void vk_dump_mesh_end(VkExec *ex, int q) {
    if (q < 0) return;
    if (mq_stats) vkCmdEndQuery(ex->cb, mq_stats, (uint32_t)q);
    vkCmdEndQuery(ex->cb, mq_pool, (uint32_t)q);
    ex->recorded += 2;
}

static void mq_write(void) {   // after vk_sync_finish: every query of the frame was submitted and ran
    if (!mq_n || !mq_pool) return;
    uint64_t *r = calloc(2ull * mq_n, sizeof *r), *st = calloc((MQ_STATS + 1ull) * mq_n, sizeof *st);
    char path[600];
    snprintf(path, sizeof path, "%s/mesh_prims.txt", dir);
    FILE *f = r && st ? fopen(path, "w") : NULL;
    if (f) {
        VkResult vr = vkGetQueryPoolResults(vk_ctx()->device, mq_pool, 0, mq_n, 16ull * mq_n, r, 16,
                                            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        VkResult vs = mq_stats ? vkGetQueryPoolResults(vk_ctx()->device, mq_stats, 0, mq_n, 8ull * (MQ_STATS + 1) * mq_n, st,
                                                       8ull * (MQ_STATS + 1),
                                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)
                               : VK_ERROR_UNKNOWN;
        fprintf(f, "# per mesh draw: primitives its mesh shader emitted (mesh primitives generated query; result %d), then "
                   "pipeline statistics (result %d): task and mesh shader invocations, clipper input and output "
                   "primitives, fragment shader invocations\n", (int)vr, (int)vs);
        for (uint32_t i = 0; i < mq_n; i++) {
            const MqDraw *d = &mq_draw[i];
            const uint64_t *s = st + (MQ_STATS + 1ull) * i;
            fprintf(f, "pass %04d prog %llu ts %016llx ms %016llx depth test %u write %u func %u: ", d->pass, d->prog, d->ts,
                    d->ms, d->dt, d->dw, d->df);
            if (r[2 * i + 1]) fprintf(f, "%llu primitives", (unsigned long long)r[2 * i]);
            else fprintf(f, "primitives not available");
            // the counters come in the order of their flag bits: clipping invocations 0x20, clipping primitives 0x40,
            // fragment invocations 0x80, task invocations 0x800, mesh invocations 0x1000
            if (mq_stats && s[MQ_STATS])
                fprintf(f, "; task %llu mesh %llu clip in %llu clip out %llu fragments %llu", (unsigned long long)s[3],
                        (unsigned long long)s[4], (unsigned long long)s[0], (unsigned long long)s[1], (unsigned long long)s[2]);
            else fprintf(f, "; statistics not available");
            fprintf(f, "\n");
        }
        fclose(f);
    }
    free(r), free(st);
    mq_n = 0;
}

void vk_dump_draw(VkExec *ex, const VkPipeState *s, VkPipeline p, const VkPipeInfo *info, int mesh) {
    plat_mutex_lock(&rec_lock);
    if (!active()) goto out;
    if (mesh)
        mq_next = (MqDraw){0, ex->prog ? ex->prog->serial : 0, shader_hash(s->ts), shader_hash(s->vs), (unsigned)s->ds.dt,
                           (unsigned)s->ds.dw, (unsigned)s->ds.df};
    pass_draws++;
    uint64_t th = bindings_hash(ex, info);
    if (mesh)   // a mesh draw's per-draw parameters often live in its uniform blocks: draws that differ there are told apart
        for (int st = 0; st < 8; st++)   // every NVN stage: 6 is task, 7 mesh
            for (int k = 0; k < 18; k++) th = (th ^ ex->ubo[st][k].addr ^ ex->ubo[st][k].size << 48) * 0x100000001b3ull;
    for (int i = 0; i < nents; i++)
        if (ents[i].p == p && ents[i].th == th) {
            ents[i].draws++;
            goto out;
        }
    if (nents == MAX_ENTS) {
        ents_over++;
        goto out;
    }
    Str b = {0};
    if (mesh)   // depth: test, write and the NVN function (VkCompareOp + 1: LESS 2, EQUAL 3, LEQUAL 4, GREATER 5, GEQUAL 7)
        sp(&b, "mesh: ts %016llx ms %016llx fs %016llx prog %llu depth test %u write %u func %u cull %u front %s\n",
           (unsigned long long)shader_hash(s->ts), (unsigned long long)shader_hash(s->vs),
           (unsigned long long)shader_hash(s->fs), (unsigned long long)(ex->prog ? ex->prog->serial : 0),
           (unsigned)s->ds.dt, (unsigned)s->ds.dw, (unsigned)s->ds.df, (unsigned)s->poly.cull,
           s->poly.front == NVN_FRONT_FACE_CW ? "CW" : "CCW");
    else
        sp(&b, "vs %016llx fs %016llx prog %llu prim %d\n", (unsigned long long)shader_hash(s->vs),
           (unsigned long long)shader_hash(s->fs), (unsigned long long)(ex->prog ? ex->prog->serial : 0), s->prim);
    // target 0's blending as NVN numbers (NVNblendFunc, NVNblendEquation), which decides whether a quad covers or mixes
    const blend_st *bl = &s->blend[0];
    sp(&b, "    blend: enabled targets %#x; target 0 rgb %u*src %u*dst eq %u, alpha %u*src %u*dst eq %u; channel mask %#x\n",
       (unsigned)s->color.blend_en, (unsigned)bl->src_rgb, (unsigned)bl->dst_rgb, (unsigned)bl->eq_rgb,
       (unsigned)bl->src_a, (unsigned)bl->dst_a, (unsigned)bl->eq_a, (unsigned)s->chmask.mask);
    sp(&b, "    depth: test %u write %u func %u; alpha func %u; cull %u; a2c %u; samples %u; mesh task %d\n", (unsigned)s->ds.dt,
       (unsigned)s->ds.dw, (unsigned)s->ds.df, (unsigned)s->color.alpha_func, (unsigned)s->poly.cull, (unsigned)s->ms.a2c,
       (unsigned)s->samples, s->ts != NULL);
    describe_bindings(&b, ex, info);
    ents[nents++] = (Ent){p, th, 1, b.s};
out:
    plat_mutex_unlock(&rec_lock);
}

static void target_line(VkExec *ex, const VkExecTarget *t, const char *tag, int n) {
    char fn[16], name[160];
    if (!t->image) {
        fprintf(passes_f, "  %s: nvn %#x target %d %ux%u: not renderable, left out\n", tag, t->tex->d.format,
                t->tex->d.target, t->tex->d.width, t->tex->d.height);
        return;
    }
    snprintf(name, sizeof name, "pass_%04d_%s_%ux%u_%d_%s", n, tag, t->width, t->height, (int)t->format,
             fmt_name(t->format, fn));
    // a block view: the plane's blocks, read as the view format (one texel per block)
    uint32_t tw = t->tex->d.width >> t->level ? t->tex->d.width >> t->level : 1;
    uint32_t th = t->tex->d.height >> t->level ? t->tex->d.height >> t->level : 1;
    (void)n;
    const char *why = pass_skip ? pass_skip : t->samples > 1 ? "multisampled" :
                      t->blocks ? copy_region(ex, t->image, t->format, t->aspect, t->level, t->is3d ? 0 : t->base_layer,
                                              t->is3d ? t->base_layer : 0, (VkExtent3D){tw, th, 1}, t->width,
                                              t->height, 1, name) :
                      t->is3d ? "3D slice (the volume is dumped as lut<id> where it is sampled)" :
                      copy_out(ex, t->image, t->format, t->aspect, t->level, t->base_layer, t->width, t->height, 1, name);
    fprintf(passes_f, "  %s: nvn %#x %s(%d) %ux%u level %u layers %u+%u samples %u addr %#llx vkimage %p -> %s.png%s%s\n",
            tag, t->nvn_format, fmt_name(t->format, fn), (int)t->format, t->width, t->height, t->level, t->base_layer,
            t->layers, t->samples, (unsigned long long)(uintptr_t)t->tex->mem, (void *)t->image, name, why ? " skipped: " : "",
            why ? why : "");
}

static uint64_t targets_sig(const VkExec *ex) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 9; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        uint64_t v[3] = {(uint64_t)(uintptr_t)t->image, t->level, t->base_layer};
        for (int k = 0; k < 3; k++) h = (h ^ v[k]) * 1099511628211ull;
    }
    return h;
}

// Sets pass_skip for the pass or dispatch being logged and counts it when it copies.
static int render_copy_selected(void) {
    if (!ncopy_shaders) return 1;
    for (int k = 0; k < ncopy_shaders; k++) {
        char vertex[24], fragment[24];
        snprintf(vertex, sizeof vertex, "vs %016llx", (unsigned long long)copy_shaders[k]);
        snprintf(fragment, sizeof fragment, "fs %016llx", (unsigned long long)copy_shaders[k]);
        for (int i = 0; i < nents; i++)
            if (strstr(ents[i].text, vertex) || strstr(ents[i].text, fragment)) return 1;
    }
    return 0;
}

static int compute_copy_selected(const GfxShader *cs) {
    if (!ncopy_shaders) return 1;
    uint64_t h = shader_hash(cs);
    for (int k = 0; k < ncopy_shaders; k++)
        if (copy_shaders[k] == h) return 1;
    return 0;
}

static void pass_decide(int same_targets, int selected) {
    if (frame_now() != dump_frame) copied = 0;
    pass_skip = !selected ? "shader not selected" : copied >= MAX_PASSES ? "pass cap" :
                    same_targets ? "same targets as the previous pass" : NULL;
    if (!pass_skip) copied++;
}

static void open_files(void);

void vk_dump_pass_end(VkExec *ex) {
    plat_mutex_lock(&rec_lock);
    if (dump_after && dump_frame == UINT64_MAX && ex->depth.tex && (uint64_t)(uintptr_t)ex->depth.tex->mem == dump_after) {
        if (!passes_f) open_files();   // NX2NAVI_DUMP_WHITE and NX2NAVI_DUMP_MOVIE open them for each of their dumps
        dump_frame = frame_now();   // this pass and the rest of its frame
        rt_log("vk_dump: a pass drew into %#llx in frame %llu: it and the rest of the frame go to %s",
               (unsigned long long)dump_after, (unsigned long long)dump_frame, dir);
    }
    if (!active()) goto out;
    int n = seq++;
    uint64_t sig = targets_sig(ex);
    pass_decide(skip_runs && sig == last_targets, render_copy_selected());
    last_targets = sig;
    fprintf(passes_f, "pass %04d: render pass, draws %d, area %ux%u layers %u, fence %llu, frame %llu\n", n, pass_draws,
            ex->pass_w, ex->pass_h, ex->pass_layers, (unsigned long long)ex->fence, (unsigned long long)frame_now());
    for (int i = 0; i < 8; i++) {
        if (!ex->color[i].tex) continue;
        char tag[8];
        snprintf(tag, sizeof tag, "t%d", i);
        target_line(ex, &ex->color[i], tag, n);
    }
    if (ex->depth.tex) target_line(ex, &ex->depth, "td", n);
    for (int i = 0; i < nents; i++) {
        fprintf(passes_f, "  pipe %d (draws %d): %s", i, ents[i].draws, ents[i].text);
        free(ents[i].text);
    }
    if (ents_over) fprintf(passes_f, "  ... %d more draws with other pipeline/texture sets\n", ents_over);
    if (!pass_skip) flush_luts(ex, n);
    else nluts = 0;
    buf_snap(ex, n);
    fflush(passes_f);
    nents = ents_over = pass_draws = 0;
out:
    plat_mutex_unlock(&rec_lock);
}

void vk_dump_dispatch(VkExec *ex, const GfxShader *cs, VkPipeline p, const VkPipeInfo *info, const p_dispatch *d) {
    (void)p;
    plat_mutex_lock(&rec_lock);
    if (!active()) goto out;
    int n = seq++;
    pass_decide(0, compute_copy_selected(cs));
    last_targets = 0;
    Str b = {0};
    describe_bindings(&b, ex, info);
    fprintf(passes_f, "pass %04d: dispatch %dx%dx%d cs %016llx prog %llu caller %#llx, fence %llu\n%s", n, d->x, d->y,
            d->z, (unsigned long long)shader_hash(cs), (unsigned long long)(ex->prog ? ex->prog->serial : 0),
            (unsigned long long)d->caller, (unsigned long long)ex->fence, b.s ? b.s : "");
    free(b.s);
    if (dump_ubo) {   // NX2NAVI_DUMP_UBO: the NVN2 push constants as 64-bit words
        fprintf(passes_f, "    push:");
        for (int q = 0; q < 0x80; q += 8) {
            uint64_t v;
            memcpy(&v, ex->cs_push + q, 8);
            fprintf(passes_f, " %02x:%llx", q, (unsigned long long)v);
        }
        fprintf(passes_f, "\n");
    }
    for (uint32_t i = 0; i < info->n_bindings && !pass_skip; i++) {   // storage images the dispatch writes
        const VkPipeBinding *bd = &info->bindings[i];
        if (!bd->res || bd->kind != GFX_RES_IMG || !(bd->res->flags & 4) || bd->type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            continue;
        uint64_t h = handle_of(ex, 5, bd->res->slot, 1);
        view_t v;
        int hv = 0;
        const VkTexImage *t = h ? vk_texture_by_id(NVN_HANDLE_TEX(h), &v, &hv) : NULL;
        if (!t) continue;
        uint32_t lv = hv && v.set_levels ? v.base_level : 0, layer = hv && v.set_layers ? v.min_layer : 0;
        if (lv >= t->levels || layer >= t->layers) continue;
        uint32_t w = t->width >> lv ? t->width >> lv : 1, hh = t->height >> lv ? t->height >> lv : 1;
        uint32_t dd = t->type == VK_IMAGE_TYPE_3D ? (t->depth >> lv ? t->depth >> lv : 1) : 1;
        char fn[16], name[160];
        snprintf(name, sizeof name, "pass_%04d_img%d_%ux%u%s_%d_%s", n, bd->res->slot, w, hh, dd > 1 ? "x3d" : "",
                 (int)t->format, fmt_name(t->format, fn));
        if (t->type == VK_IMAGE_TYPE_3D) {   // a written volume: this copy replaces the sampled-3D one
            for (int k = 0; k < nluts_done; k++)
                if (luts_done[k] == t->image) luts_done[k] = VK_NULL_HANDLE;
        }
        const char *why = t->samples > 1 ? "multisampled" :
                          copy_out(ex, t->image, vk_exec_raw_9e5(t->image, t->format) ? VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 : t->format,
                                   t->aspect, lv, layer, w, hh, dd, name);
        fprintf(passes_f, "  writes img slot %d level %u layer %u depth %u -> %s.png%s%s\n", bd->res->slot, lv, layer,
                dd, name, why ? " skipped: " : "", why ? why : "");
    }
    if (!pass_skip) flush_luts(ex, n);
    else nluts = 0;
    buf_snap(ex, n);
    fflush(passes_f);
out:
    plat_mutex_unlock(&rec_lock);
}

static const char *tex_desc(const tex_t *t, char *buf, size_t n) {
    if (!t) return "null";
    snprintf(buf, n, "addr %#llx nvn-fmt %#x %ux%ux%u", (unsigned long long)(uintptr_t)t->mem, t->d.format, t->d.width,
             t->d.height, t->d.depth);
    return buf;
}

static void region_desc(const NVNcopyRegion *r, char *buf, size_t n) {
    snprintf(buf, n, "(%d,%d,%d %dx%dx%d)", r->xoffset, r->yoffset, r->zoffset, r->width, r->height, r->depth);
}

void vk_dump_copy(VkExec *ex, uint32_t op, const void *payload) {
    (void)ex;
    plat_mutex_lock(&rec_lock);
    if (!active()) goto out;
    char a[96], b[96], ra[64], rb[64];
    switch (op) {
    case OP_COPY_B2B: {
        const p_copy_b2b *p = payload;
        fprintf(passes_f, "copy after pass %d: buffer %#llx -> buffer %#llx, %llu bytes\n", seq - 1,
                (unsigned long long)p->src, (unsigned long long)p->dst, (unsigned long long)p->size);
        break;
    }
    case OP_COPY_B2T: case OP_COPY_T2B: {
        const p_copy_b2t *p = payload;
        region_desc(&p->r, ra, sizeof ra);
        fprintf(passes_f, "copy after pass %d: %s buffer %#llx %s texture %s region %s flags %#x\n", seq - 1,
                op == OP_COPY_B2T ? "buffer->texture" : "texture->buffer", (unsigned long long)p->src,
                op == OP_COPY_B2T ? "->" : "<-", tex_desc(p->tex, a, sizeof a), ra, p->flags);
        break;
    }
    case OP_COPY_T2T: {
        const p_copy_t2t *p = payload;
        region_desc(&p->sr, ra, sizeof ra);
        region_desc(&p->dr, rb, sizeof rb);
        fprintf(passes_f, "copy after pass %d: texture %s %s -> texture %s %s flags %#x\n", seq - 1,
                tex_desc(p->src, a, sizeof a), ra, tex_desc(p->dst, b, sizeof b), rb, p->flags);
        break;
    }
    case OP_CLEAR_TEX: {
        const p_clear_tex *p = payload;
        region_desc(&p->r, ra, sizeof ra);
        fprintf(passes_f, "clear after pass %d: texture %s region %s mask %#x\n", seq - 1, tex_desc(p->tex, a, sizeof a),
                ra, p->mask);
        break;
    }
    default: break;
    }
out:
    plat_mutex_unlock(&rec_lock);
}

static void make_dirs(char *path) { plat_file_mkdir_p(path); }

// NX2NAVI_DUMP_WHITE=1 (diagnosis): no frame is named; from frame 1800 each present looks at the thumbnail of the frame
// three presents back (gfx_present.c vk_thumbs_*), and the first one whose picture left of the overlay and above the
// dialogue box is blank white makes the dump take the next frame (white flashes come in runs of several frames).
int vk_dump_white;   // vk_backend.c asks for a thumbnail of every present while it is set
#define dump_white vk_dump_white

// NX2NAVI_DUMP_MOVIE=<k>:<frame>[,<frame>...] (diagnosis): the k-th movie of the run (counted from 1 as each video
// decoder comes up, ov_movie2.c ov_initialize) asks for a dump of the next present once it has shown each listed
// frame, so a movie's picture can be followed through the game's passes at chosen moments. Each dump has its own
// folder, base_dir/f<present>.
static int movie_k, movie_n, movie_done;
static int key_mode;   // NX2NAVI_DUMP=key (vk_dump_init)
static uint64_t movie_at[8];
static int movie_req;   // set by the movie's thread, taken by vk_dump_present

void vk_dump_movie_frame(int k, uint64_t shown) {
    if (!movie_k || k != movie_k || movie_done >= movie_n || shown != movie_at[movie_done]) return;
    movie_done++;
    __atomic_store_n(&movie_req, 1, __ATOMIC_RELEASE);
    rt_log("vk_dump: movie %d shows frame %llu: dumping the next present", k, (unsigned long long)shown);
}

// NX2NAVI_DUMP_FILE=<path> (diagnosis): every 8th present looks for that file (a relative path starts in the game
// folder); when it is there, the present deletes it and asks for a dump of the next present, so a shell or a test
// script takes dumps at the moments it chooses, as many as it likes. A number in the file (up to 8) dumps that many
// consecutive frames, for what a frame hands to the next. Each dump has its own folder, base_dir/f<present>.
static char dump_file[400];
static int file_more;   // consecutive frames still to dump after the one named

// passes.txt and images.txt in dir (a new folder per NX2NAVI_DUMP_WHITE attempt)
static void open_files(void) {
    make_dirs(dir);
    char path[600];
    snprintf(path, sizeof path, "%s/passes.txt", dir);
    passes_f = fopen(path, "w");
    snprintf(path, sizeof path, "%s/images.txt", dir);
    images_f = fopen(path, "w");
    if (!passes_f || !images_f) rt_fatal("vk_dump: cannot write to %s", dir);
    // NX2NAVI_DUMP_BUF with dumps named as the run goes (key, white, movie): the log goes with each dump's folder
    if (bd_size && !bufs_f) {
        snprintf(path, sizeof path, "%s/bufs.txt", dir);
        bufs_f = fopen(path, "w");
    }
}

void vk_dump_init(void) {
    const char *e = cfg_get("NX2NAVI_DUMP");
    dump_white = rt_knob("DUMP_WHITE", "0");
    // NX2NAVI_DUMP_AFTER=<address>: the first render pass whose depth target lives at that address and the rest of its frame
    // (a target drawn once, at an area load)
    const char *da = cfg_get("NX2NAVI_DUMP_AFTER");
    dump_after = da && *da ? strtoull(da, NULL, 0) : 0;
    const char *dm = cfg_get("NX2NAVI_DUMP_MOVIE");
    if (dm && *dm) {
        char *q;
        movie_k = (int)strtol(dm, &q, 10);
        while ((*q == ':' || *q == ',') && movie_n < 8) movie_at[movie_n++] = strtoull(q + 1, &q, 10);
        if (movie_k < 1 || !movie_n) movie_k = 0;
    }
    // NX2NAVI_DUMP=key: no frame is named; each press of Insert (input_dump_key: unbound in the game's layout, and read on
    // Windows even while another window has the focus) dumps the frame after it into NX2NAVI_DUMP_DIR/f<frame>, as often
    // as it is pressed: for a glitch that comes and goes, caught by the player while it is on screen.
    key_mode = e && !strcmp(e, "key");
    const char *df = cfg_get("NX2NAVI_DUMP_FILE");
    if (df && *df) snprintf(dump_file, sizeof dump_file, "%s", df);
    if (dump_white || dump_after) {
        dump_frame = UINT64_MAX;
        vk_thumbs_enable(16);
    } else if (movie_k || key_mode || dump_file[0]) {
        dump_frame = UINT64_MAX;
    } else if (!e || !*e || !(dump_frame = strtoull(e, NULL, 10))) return;
    const char *lf = cfg_get("NX2NAVI_DUMP_LOG");
    if (lf && *lf) log_from = strtoull(lf, NULL, 10);
    const char *sr = cfg_get("NX2NAVI_DUMP_SKIP_RUNS");
    skip_runs = sr && *sr && *sr != '0';
    const char *du = cfg_get("NX2NAVI_DUMP_UBO");
    dump_ubo = du && *du ? atoi(du) : 0;
    if (dump_ubo == 1) dump_ubo = 16;
    if (dump_ubo > 4096) dump_ubo = 4096;
    if (dump_ubo < 0) dump_ubo = 0;
    const char *raw = cfg_get("NX2NAVI_DUMP_RAW");
    dump_raw = raw && *raw && *raw != '0';
    const char *shaders = cfg_get("NX2NAVI_DUMP_SHADERS");
    for (const char *p = shaders; p && *p && ncopy_shaders < 16;) {
        char *end;
        uint64_t h = strtoull(p, &end, 16);
        if (end == p) break;
        copy_shaders[ncopy_shaders++] = h;
        p = *end ? end + 1 : end;
    }
    const char *wt = cfg_get("NX2NAVI_DUMP_TEX");
    for (char *q = (char *)wt; q && *q && nwant_tex < 16; q += *q == ',') {
        uint32_t *w = want_tex[nwant_tex++];
        w[0] = (uint32_t)strtoul(q, &q, 10);
        w[1] = *q == 'x' ? (uint32_t)strtoul(q + 1, &q, 10) : 0;
    }
    const char *d = cfg_get("NX2NAVI_DUMP_DIR");
    snprintf(base_dir, sizeof base_dir, "%s", d && *d ? d : "dump");
    snprintf(dir, sizeof dir, "%s", base_dir);
    char path[600];
    if (!dump_white && !movie_k && !key_mode && !dump_file[0]) open_files();
    const char *bf = cfg_get("NX2NAVI_DUMP_BUF");
    if (bf && *bf) {
        char *q;
        bd_addr = strtoull(bf, &q, 0);
        if (*q == ':') bd_size = strtoull(q + 1, &q, 0);
        if (*q == ':') bd_off = strtoull(q + 1, &q, 0);
        if (*q == ':') bd_stride = strtoull(q + 1, &q, 0) & ~3ull;
        if (*q == ':') bd_w = (uint32_t)strtoul(q + 1, &q, 0);
        if (*q == ':') bd_h = (uint32_t)strtoul(q + 1, &q, 0);
        if (!bd_w || !bd_h) bd_stride = 0;
        snprintf(path, sizeof path, "%s/bufs.txt", dir);
        // a dump named as the run goes opens it with its folder (open_files); a named frame's folder exists already
        if (bd_size && !dump_white && !movie_k && !key_mode && !dump_file[0] && !(bufs_f = fopen(path, "w"))) bd_size = 0;
    }
    vk_dump_armed = 1;
    rt_log("vk_dump: frame %llu goes to %s", (unsigned long long)dump_frame, dir);
}

// The thumbnail of frame f is blank white left of the overlay and above the dialogue box: -1 not available, 0 no, 1 yes.
static int thumb_white(uint64_t f) {
    static uint8_t px[FLIGHT_THUMB_W * FLIGHT_THUMB_H * 4];
    if (!vk_thumbs_read(f, px)) return -1;
    int n = 0, bright = 0;
    for (int y = 0; y < 105; y++)
        for (int x = 0; x < 250; x++, n++) {
            const uint8_t *q = px + 4 * (y * FLIGHT_THUMB_W + x);
            bright += (q[0] * 30 + q[1] * 59 + q[2] * 11) / 100 > 245;
        }
    return bright * 100 > n * 97;
}

static void white_check(uint64_t frame) {
    static int run;   // consecutive white thumbnails: white runs are mostly 1-2 frames, too short for a dump 4 frames on
    if (frame < 1800) return;
    int w = thumb_white(frame - 3);
    if (w < 0) return;
    run = w ? run + 1 : 0;
    if (run >= 3) {   // frames frame-5..frame-3 white: the run is likely still on 4 frames later
        run = 0;
        dump_frame = frame + 1;
        snprintf(dir, sizeof dir, "%s/f%llu", base_dir, (unsigned long long)dump_frame);
        open_files();
        rt_log("vk_dump: frames %llu-%llu were white; frame %llu goes to %s", (unsigned long long)(frame - 5),
               (unsigned long long)(frame - 3), (unsigned long long)dump_frame, dir);
    }
}

void vk_dump_present(uint64_t frame) {
    if (dump_white && white_last && frame == white_last + 3) {   // was the dumped frame itself white?
        int w = thumb_white(white_last);
        rt_log("vk_dump: dumped frame %llu was %s", (unsigned long long)white_last, w > 0 ? "white: done" : "not white: trying again");
        if (w > 0 || ++white_tries >= 8) white_caught = 1;
        white_last = 0;
    }
    if (dump_white && !white_caught && !white_last && dump_frame == UINT64_MAX) white_check(frame);
    if (movie_k && dump_frame == UINT64_MAX && __atomic_exchange_n(&movie_req, 0, __ATOMIC_ACQ_REL)) {
        snprintf(dir, sizeof dir, "%s/f%llu", base_dir, (unsigned long long)(frame + 1));
        open_files();   // before the frame is named: its passes write to them
        dump_frame = frame + 1;
        rt_log("vk_dump: frame %llu goes to %s", (unsigned long long)dump_frame, dir);
    }
    if (key_mode && dump_frame == UINT64_MAX) {   // NX2NAVI_DUMP=key: a new press of Insert names the next frame
        static int held;
        int now = input_dump_key();
        if (now && !held) {
            snprintf(dir, sizeof dir, "%s/f%llu", base_dir, (unsigned long long)(frame + 1));
            open_files();   // before the frame is named: its passes write to them
            dump_frame = frame + 1;
            rt_log("vk_dump: Insert: frame %llu goes to %s", (unsigned long long)dump_frame, dir);
        }
        held = now;
    }
    if (dump_file[0] && dump_frame == UINT64_MAX && frame % 8 == 0 && plat_file_exists(dump_file)) {
        uint64_t sz = 0;
        uint8_t *txt = plat_file_read(dump_file, &sz);
        int n = txt ? atoi((const char *)txt) : 0;
        free(txt);
        file_more = n > 8 ? 7 : n > 1 ? n - 1 : 0;
        plat_file_delete(dump_file);
        snprintf(dir, sizeof dir, "%s/f%llu", base_dir, (unsigned long long)(frame + 1));
        open_files();   // before the frame is named: its passes write to them
        dump_frame = frame + 1;
        rt_log("vk_dump: %s found: frame %llu goes to %s", dump_file, (unsigned long long)dump_frame, dir);
    }
    if (frame != dump_frame) return;
    plat_mutex_lock(&rec_lock);
    finished = 1;
    plat_mutex_unlock(&rec_lock);
    vk_sync_finish();   // the frame's copies are converted and written
    mq_write();
    plat_mutex_lock(&img_lock);
    fclose(images_f);
    images_f = NULL;
    if (bufs_f) fclose(bufs_f), bufs_f = NULL;
    plat_mutex_unlock(&img_lock);
    fclose(passes_f);
    passes_f = NULL;
    rt_log("vk_dump: frame %llu: %d passes and dispatches, about %llu MB of PNGs in %s", (unsigned long long)frame, seq,
           (unsigned long long)(png_bytes >> 20), dir);
    if (dump_white || movie_k || key_mode || dump_file[0]) {   // ready for another dump (vk_dump_present checks a white one's thumbnail 3 presents on)
        if (dump_white) white_last = frame;
        plat_mutex_lock(&rec_lock);
        seq = copied = 0, last_targets = 0, pass_skip = NULL, png_bytes = 0;
        nluts = nluts_done = nents = ents_over = pass_draws = 0;
        dump_frame = UINT64_MAX;
        finished = 0;
        plat_mutex_unlock(&rec_lock);
    }
    if (file_more > 0) {   // NX2NAVI_DUMP_FILE with a count: the next frame too
        file_more--;
        snprintf(dir, sizeof dir, "%s/f%llu", base_dir, (unsigned long long)(frame + 1));
        open_files();
        dump_frame = frame + 1;
        rt_log("vk_dump: frame %llu goes to %s", (unsigned long long)dump_frame, dir);
    }
}
