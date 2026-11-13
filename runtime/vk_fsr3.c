// AMD FSR 3.1 temporal upscaling recorded by the runtime: the upscaler's compute passes (runtime/shaders/fsr3_*.comp,
// over the headers in runtime/shaders/ffx3, built into runtime/fsr3_*_spv.h by tools/build_shaders.py). vk_ffx.cpp
// sends vk_ffx_upscale here in the builds that load no amd_fidelityfx_vk.dll (all but Windows). Frame generation is not
// here: it runs through that library's own API calls (vk_fg.cpp), so not in those builds.
//
// The inputs are vk_dlaa.c's, the ones vk_ffx.cpp hands the library: the scene light (linear HDR), its D32F depth
// (OpenGL-style, 0 near and 1 far, with the frame's near and far planes; not inverted), motion vectors at the render
// size in render pixels from the current frame to the previous one, without the jitter, and the jitter in render
// pixels. The passes are built with the options of runtime/shaders/fsr3_options.h for those inputs (HDR, low-resolution
// motion vectors, depth not inverted) and run with auto exposure. Every image is in GENERAL and stays there.
//
// One call (vk_fsr3_upscale) records, in this order: the clears (history on a reset, the reconstructed depth and the
// SPD mips every time), then prepare inputs (dilated depth and motion, the previous frame's nearest depth
// reconstructed, luma, farthest depth), luma pyramid (SPD: auto exposure and the farthest depth at half size), shading
// change pyramid (SPD again over the luma change), shading change, prepare reactivity (disocclusion, locks,
// accumulation), luma instability, accumulate (the upscale itself) and, with sharpening, RCAS on the accumulated colour
// into the output. A global compute barrier separates the passes. fill_constants computes the constants the passes
// read; the history images alternate between calls.
//
// Contexts (ctx_for): one per render size and output size, or with the game's dynamic resolution one per output size
// for every render size up to it, at most CTX_MAX; one no upscale used for 90 seconds goes, and all of them go while
// the upscaler is off (vk_fsr3_release_idle). The constants live in a host-visible ring of RING slots; a slot is
// reused RING calls later, after the GPU passed the submission that last read it.
#include "vk.h"

#include <float.h>
#include <math.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "vk_fsr3.h"

#include "fsr3_accumulate_sharpen_spv.h"
#include "fsr3_accumulate_spv.h"
#include "fsr3_luma_instability_spv.h"
#include "fsr3_luma_pyramid_spv.h"
#include "fsr3_prepare_inputs_spv.h"
#include "fsr3_prepare_reactivity_spv.h"
#include "fsr3_rcas_spv.h"
#include "fsr3_shading_change_pyramid_spv.h"
#include "fsr3_shading_change_spv.h"

// ---- the images of a context

enum { SZ_RENDER, SZ_HALF, SZ_OUT, SZ_ONE };
enum {
    I_ACC0, I_ACC1,     // accumulation (locks' age), this call's and the last one's, alternating
    I_LUMA0, I_LUMA1,   // current and previous luma, alternating
    I_LH0, I_LH1,       // luma history (four frames), alternating
    I_UP0, I_UP1,       // the upscaled colour, alternating
    I_INTER,            // farthest depth, then luma instability: one image, which the first has left by the second
    I_SHADE, I_LOCKS, I_SPD, I_FDMIP1, I_ATOMIC, I_DREACT, I_FINFO, I_DDEPTH, I_DMV, I_RECON, I_N
};
static const struct {
    VkFormat fmt;
    uint8_t size;
} IMG[I_N] = {
    [I_ACC0] = {VK_FORMAT_R8_UNORM, SZ_RENDER},
    [I_ACC1] = {VK_FORMAT_R8_UNORM, SZ_RENDER},
    [I_LUMA0] = {VK_FORMAT_R16_SFLOAT, SZ_RENDER},
    [I_LUMA1] = {VK_FORMAT_R16_SFLOAT, SZ_RENDER},
    [I_LH0] = {VK_FORMAT_R16G16B16A16_SFLOAT, SZ_RENDER},
    [I_LH1] = {VK_FORMAT_R16G16B16A16_SFLOAT, SZ_RENDER},
    [I_UP0] = {VK_FORMAT_R16G16B16A16_SFLOAT, SZ_OUT},
    [I_UP1] = {VK_FORMAT_R16G16B16A16_SFLOAT, SZ_OUT},
    [I_INTER] = {VK_FORMAT_R16_SFLOAT, SZ_RENDER},
    [I_SHADE] = {VK_FORMAT_R8_UNORM, SZ_HALF},
    [I_LOCKS] = {VK_FORMAT_R8_UNORM, SZ_OUT},
    [I_SPD] = {VK_FORMAT_R16G16_SFLOAT, SZ_HALF},   // with every mip
    [I_FDMIP1] = {VK_FORMAT_R16_SFLOAT, SZ_HALF},
    [I_ATOMIC] = {VK_FORMAT_R32_UINT, SZ_ONE},
    [I_DREACT] = {VK_FORMAT_R8G8B8A8_UNORM, SZ_RENDER},
    [I_FINFO] = {VK_FORMAT_R32G32B32A32_SFLOAT, SZ_ONE},
    [I_DDEPTH] = {VK_FORMAT_R32_SFLOAT, SZ_RENDER},
    [I_DMV] = {VK_FORMAT_R16G16_SFLOAT, SZ_RENDER},
    [I_RECON] = {VK_FORMAT_R32_UINT, SZ_RENDER},
};
#define SPD_VIEWS 6   // the SPD passes write mips 0-5 through views of their own

// ---- what the passes bind (the binding numbers of runtime/shaders/fsr3_*.comp)

enum {
    // sampled images
    S_COLOR, S_DEPTH, S_MV, S_DMV, S_DDEPTH, S_RECON, S_REACTIVE, S_TC, S_ACC, S_SHADE, S_CLUMA, S_PLUMA, S_EXPOSURE,
    S_FINFO, S_LH, S_FDMIP1, S_FD, S_UP, S_LUT, S_DREACT, S_LINST, S_SPD, S_RCAS_IN,
    // storage images
    U_FIRST, U_DMV = U_FIRST, U_DDEPTH, U_RECON, U_FD, U_CLUMA, U_ATOMIC, U_FINFO, U_SPD0, U_SPD1, U_SPD2, U_SPD3,
    U_SPD4, U_SPD5, U_FDMIP1, U_SHADE, U_DREACT, U_LOCKS, U_ACC, U_LH, U_LINST, U_UP, U_OUT,
    // uniform buffers
    C_FIRST, C_MAIN = C_FIRST, C_SPD, C_RCAS,
};
typedef struct {
    uint8_t binding, res;
} Bind;
#define B(...) ((const Bind[]){__VA_ARGS__}), (sizeof((const Bind[]){__VA_ARGS__}) / sizeof(Bind))

enum { P_PREP_IN, P_LUMA_PYR, P_SHADE_PYR, P_SHADE, P_PREP_REACT, P_LUMA_INST, P_ACCUM, P_ACCUM_SHARP, P_RCAS, P_N };
static const struct {
    const char *name;
    const uint32_t *code;
    size_t size;
    const Bind *b;
    uint32_t n;
} PASS[P_N] = {
    [P_PREP_IN] = {"prepare inputs", fsr3_prepare_inputs_spv, sizeof fsr3_prepare_inputs_spv,
                   B({0, S_MV}, {1, S_DEPTH}, {2, S_COLOR}, {3, U_DMV}, {4, U_DDEPTH}, {5, U_RECON}, {6, U_FD},
                     {7, U_CLUMA}, {8, C_MAIN})},
    [P_LUMA_PYR] = {"luma pyramid", fsr3_luma_pyramid_spv, sizeof fsr3_luma_pyramid_spv,
                    B({0, S_CLUMA}, {1, S_FD}, {2, U_ATOMIC}, {3, U_FINFO}, {4, U_SPD0}, {5, U_SPD1}, {6, U_SPD2},
                      {7, U_SPD3}, {8, U_SPD4}, {9, U_SPD5}, {10, U_FDMIP1}, {11, C_MAIN}, {12, C_SPD})},
    [P_SHADE_PYR] = {"shading change pyramid", fsr3_shading_change_pyramid_spv, sizeof fsr3_shading_change_pyramid_spv,
                     B({0, S_CLUMA}, {1, S_PLUMA}, {2, S_DMV}, {3, S_EXPOSURE}, {4, U_ATOMIC}, {5, U_SPD0}, {6, U_SPD1},
                       {7, U_SPD2}, {8, U_SPD3}, {9, U_SPD4}, {10, U_SPD5}, {11, C_MAIN}, {12, C_SPD})},
    [P_SHADE] = {"shading change", fsr3_shading_change_spv, sizeof fsr3_shading_change_spv,
                 B({0, S_SPD}, {1, U_SHADE}, {2, C_MAIN})},
    [P_PREP_REACT] = {"prepare reactivity", fsr3_prepare_reactivity_spv, sizeof fsr3_prepare_reactivity_spv,
                      B({0, S_RECON}, {1, S_DMV}, {2, S_DDEPTH}, {3, S_REACTIVE}, {4, S_TC}, {5, S_ACC}, {6, S_SHADE},
                        {7, S_CLUMA}, {8, S_EXPOSURE}, {9, U_DREACT}, {10, U_LOCKS}, {11, U_ACC}, {12, C_MAIN})},
    [P_LUMA_INST] = {"luma instability", fsr3_luma_instability_spv, sizeof fsr3_luma_instability_spv,
                     B({0, S_EXPOSURE}, {1, S_DREACT}, {2, S_DMV}, {3, S_FINFO}, {4, S_LH}, {5, S_FDMIP1}, {6, S_CLUMA},
                       {7, U_LH}, {8, U_LINST}, {9, C_MAIN})},
    [P_ACCUM] = {"accumulate", fsr3_accumulate_spv, sizeof fsr3_accumulate_spv,
                 B({0, S_EXPOSURE}, {1, S_DREACT}, {2, S_DMV}, {3, S_UP}, {4, S_LUT}, {5, S_FDMIP1}, {6, S_CLUMA},
                   {7, S_LINST}, {8, S_COLOR}, {9, U_UP}, {10, U_OUT}, {11, U_LOCKS}, {12, C_MAIN})},
    [P_ACCUM_SHARP] = {"accumulate (sharpened)", fsr3_accumulate_sharpen_spv, sizeof fsr3_accumulate_sharpen_spv,
                       B({0, S_EXPOSURE}, {1, S_DREACT}, {2, S_DMV}, {3, S_UP}, {4, S_LUT}, {5, S_FDMIP1}, {6, S_CLUMA},
                         {7, S_LINST}, {8, S_COLOR}, {9, U_UP}, {10, U_OUT}, {11, U_LOCKS}, {12, C_MAIN})},
    [P_RCAS] = {"rcas", fsr3_rcas_spv, sizeof fsr3_rcas_spv,
                B({0, S_EXPOSURE}, {1, S_RCAS_IN}, {2, U_OUT}, {3, C_MAIN}, {4, C_RCAS})},
};
#undef B
#define MAX_BINDS 13

// The constants of every pass but RCAS and the SPD ones (std140 cbFSR3Upscaler: all 4-byte members, in this order).
typedef struct {
    int32_t render[2], prev_render[2], upscale[2], prev_upscale[2], max_render[2], max_upscale[2];
    float device_to_view[4];
    float jitter[2], prev_jitter[2];
    float mv_scale[2], downscale[2];
    float mv_jitter_cancel[2];
    float tan_half_fov, jitter_phases;
    float delta_time, delta_pre_exposure, view_to_meters, frame_index;
    float velocity_factor, reactiveness_scale, shading_change_scale, accumulation_per_frame, min_disocclusion;
} Consts;
typedef struct {
    uint32_t mips, groups, offset[2], render[2];
} SpdConsts;
_Static_assert(sizeof(Consts) == 148, "cbFSR3Upscaler is 148 bytes");
_Static_assert(__builtin_offsetof(Consts, device_to_view) == 48, "a vec4 at a 16-byte boundary in std140");
_Static_assert(sizeof(SpdConsts) == 24, "cbSPD is 24 bytes");

// ---- state

#define CTX_MAX 4
#define CTX_RECENT 8   // calls: a context used this recently may sit in a command buffer not yet submitted
// A context no upscale used for this long goes (vk_fsr3_upscale): a second scene's (the pause screen's) a while after it
// closed, or one of an output size the window no longer has. Unused that long, no command buffer names it.
#define CTX_IDLE_NS (90ull * 1000000000ull)
#define RING 256       // constant slots (calls), each one Consts, SpdConsts and the RCAS words
typedef struct {
    uint32_t mw, mh, ow, oh;   // the render size it was made for (the largest it takes with drs); the output size
    int drs;                   // made for dynamic resolution: every render size up to mw x mh
    uint64_t used, used_ns;    // G.tick and the time when ctx_for last returned it
    VkImage img[I_N];
    VmaAllocation mem[I_N];
    VkImageView view[I_N];
    VkImageView spd_mip[SPD_VIEWS];
    uint32_t spd_levels;
    int fresh;           // the images were never recorded into: transition and clear them first
    int odd;             // which of the alternating images this call writes
    Consts k;            // carried from call to call (previous sizes, jitter, frame index)
    float pre_exposure;  // the previous call's
} Ctx;

static struct {
    volatile int32_t state;   // -1: not tried; 0: off; 1: ready
    PFN_vkCmdPushDescriptorSetKHR push;
    VkSampler point, linear;
    VkDescriptorSetLayout dsl[P_N];
    VkPipelineLayout layout[P_N];
    VkPipeline pipe[P_N];
    // shared by every context: the Lanczos table and the 1x1 masks that stand for the reactive and transparency ones
    VkImage lut, zero;
    VmaAllocation lut_mem, zero_mem;
    VkImageView lut_view, zero_view;
    VkBuffer lut_src;   // the table's upload source, kept (256 bytes)
    VmaAllocation lut_src_mem;
    int shared_fresh;
    VkBuffer ring;
    VmaAllocation ring_mem;
    uint8_t *ring_map;
    uint32_t slot_bytes, block;   // one ring slot, and one block of it (the uniform buffer offset alignment)
    uint32_t ring_next;
    uint64_t ring_fence[RING];    // the submission that last read each slot (0: none)
    Ctx ctx[CTX_MAX];
    uint64_t tick;
    int made;   // contexts made so far (vk_fsr3_contexts_made)
} G = {.state = -1};

// ---- setup

static VkImageView make_view(VkImage img, VkFormat fmt, uint32_t base, uint32_t levels) {
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base, levels, 0, 1}};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(vk_ctx()->device, &vi, NULL, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    return v;
}

static int make_image(uint32_t w, uint32_t h, uint32_t levels, VkFormat fmt, VkImageUsageFlags usage, VkImage *img,
                      VmaAllocation *mem) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = levels, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(vk_ctx()->vma, &ci, &ai, img, mem, NULL) != VK_SUCCESS) {
        *img = VK_NULL_HANDLE, *mem = NULL;
        return 0;
    }
    return 1;
}

static const char *unsupported(const VkCtx *c) {
    VkPhysicalDeviceFeatures f;
    vkGetPhysicalDeviceFeatures(c->phys, &f);
    if (!f.shaderStorageImageExtendedFormats) return "no shaderStorageImageExtendedFormats";
    VkPhysicalDeviceSubgroupProperties sg = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sg};
    vkGetPhysicalDeviceProperties2(c->phys, &p2);
    if (!(sg.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) || !(sg.supportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT))
        return "no quad subgroup operations in compute shaders";
    static const VkFormat storage[] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT,
                                       VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_UINT,
                                       VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT};
    for (size_t i = 0; i < sizeof storage / sizeof storage[0]; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(c->phys, storage[i], &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((fp.optimalTilingFeatures & need) != need) return "a storage image format it needs is missing";
    }
    return NULL;
}

static int make_pass(const VkCtx *c, int p) {
    VkDescriptorSetLayoutBinding b[MAX_BINDS + 2];
    uint32_t n = 0;
    for (uint32_t i = 0; i < PASS[p].n; i++) {
        int r = PASS[p].b[i].res;
        b[n++] = (VkDescriptorSetLayoutBinding){
            .binding = PASS[p].b[i].binding, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .descriptorType = r >= C_FIRST ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                            : r >= U_FIRST ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                           : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE};
    }
    // s_PointClamp and s_LinearClamp of the passes' headers, fixed in the layout: nothing to push for them
    b[n++] = (VkDescriptorSetLayoutBinding){.binding = 1000, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                                            .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                            .pImmutableSamplers = &G.point};
    b[n++] = (VkDescriptorSetLayoutBinding){.binding = 1001, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                                            .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                            .pImmutableSamplers = &G.linear};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = n, .pBindings = b};
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &G.dsl[p]) != VK_SUCCESS) return 0;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &G.dsl[p]};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &G.layout[p]) != VK_SUCCESS) return 0;
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = PASS[p].size,
                                   .pCode = PASS[p].code};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) return 0;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = G.layout[p]};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &G.pipe[p]);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) G.pipe[p] = VK_NULL_HANDLE;
    return r == VK_SUCCESS;
}

static float lanczos2(float x) {
    const float pi = 3.14159265358979f;
    if (fabsf(x) < FLT_EPSILON) return 1.0f;
    return sinf(pi * x) / (pi * x) * (sinf(0.5f * pi * x) / (0.5f * pi * x));
}

static int make_shared(const VkCtx *c) {
    VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!make_image(128, 1, 1, VK_FORMAT_R16_SNORM, u, &G.lut, &G.lut_mem) ||
        !make_image(1, 1, 1, VK_FORMAT_R8_UNORM, u, &G.zero, &G.zero_mem) ||
        !(G.lut_view = make_view(G.lut, VK_FORMAT_R16_SNORM, 0, 1)) ||
        !(G.zero_view = make_view(G.zero, VK_FORMAT_R8_UNORM, 0, 1)))
        return 0;
    // The reprojection's Lanczos-2 weights over [0, 2] in 128 steps, as 16-bit signed normalised values
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                           VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &ai, &G.lut_src, &G.lut_src_mem, &info) != VK_SUCCESS) {
        G.lut_src = VK_NULL_HANDLE;
        return 0;
    }
    int16_t *wts = info.pMappedData;
    for (int i = 0; i < 128; i++) wts[i] = (int16_t)roundf(lanczos2(2.0f * (float)i / 127.0f) * 32767.0f);
    vmaFlushAllocation(c->vma, G.lut_src_mem, 0, VK_WHOLE_SIZE);
    // The constant ring
    uint32_t align = (uint32_t)c->props.limits.minUniformBufferOffsetAlignment;
    if (align < 16) align = 16;
    G.block = (uint32_t)((sizeof(Consts) + align - 1) / align * align);
    G.slot_bytes = G.block * 3;
    bi.size = (VkDeviceSize)G.slot_bytes * RING;
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (vmaCreateBuffer(c->vma, &bi, &ai, &G.ring, &G.ring_mem, &info) != VK_SUCCESS) {
        G.ring = VK_NULL_HANDLE;
        return 0;
    }
    G.ring_map = info.pMappedData;
    G.shared_fresh = 1;
    return 1;
}

static void destroy_global(void) {
    const VkCtx *c = vk_ctx();
    if (!c) return;
    for (int p = 0; p < P_N; p++) {
        if (G.pipe[p]) vkDestroyPipeline(c->device, G.pipe[p], NULL);
        if (G.layout[p]) vkDestroyPipelineLayout(c->device, G.layout[p], NULL);
        if (G.dsl[p]) vkDestroyDescriptorSetLayout(c->device, G.dsl[p], NULL);
        G.pipe[p] = VK_NULL_HANDLE, G.layout[p] = VK_NULL_HANDLE, G.dsl[p] = VK_NULL_HANDLE;
    }
    if (G.point) vkDestroySampler(c->device, G.point, NULL);
    if (G.linear) vkDestroySampler(c->device, G.linear, NULL);
    if (G.lut_view) vkDestroyImageView(c->device, G.lut_view, NULL);
    if (G.zero_view) vkDestroyImageView(c->device, G.zero_view, NULL);
    if (G.lut) vmaDestroyImage(c->vma, G.lut, G.lut_mem);
    if (G.zero) vmaDestroyImage(c->vma, G.zero, G.zero_mem);
    if (G.lut_src) vmaDestroyBuffer(c->vma, G.lut_src, G.lut_src_mem);
    if (G.ring) vmaDestroyBuffer(c->vma, G.ring, G.ring_mem);
    G.point = G.linear = VK_NULL_HANDLE;
    G.lut_view = G.zero_view = VK_NULL_HANDLE;
    G.lut = G.zero = VK_NULL_HANDLE;
    G.lut_src = G.ring = VK_NULL_HANDLE;
    G.ring_map = NULL;
}

int vk_fsr3_init(void) {
    int s = __atomic_load_n(&G.state, __ATOMIC_ACQUIRE);
    if (s >= 0) return s;
    const VkCtx *c = vk_ctx();
    if (!c) return 0;   // asked again once the device exists
    // The present's thread (vk_dlaa_frame) and the replay's (evaluate) may both ask first: one makes the passes, the
    // other finds them not ready yet, as before the device existed
    static volatile int32_t making;
    if (__atomic_exchange_n(&making, 1, __ATOMIC_ACQ_REL)) return 0;
    s = __atomic_load_n(&G.state, __ATOMIC_ACQUIRE);
    if (s >= 0) {
        __atomic_store_n(&making, 0, __ATOMIC_RELEASE);
        return s;
    }
    const char *why = unsupported(c);
    G.push = (PFN_vkCmdPushDescriptorSetKHR)vk_dpa(c->device, "vkCmdPushDescriptorSetKHR");
    if (!why && !G.push) why = "no vkCmdPushDescriptorSetKHR";
    if (!why) {
        VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                                  .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                  .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = VK_LOD_CLAMP_NONE};
        if (vkCreateSampler(c->device, &si, NULL, &G.point) != VK_SUCCESS) why = "sampler creation failed";
        si.magFilter = si.minFilter = VK_FILTER_LINEAR, si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        if (!why && vkCreateSampler(c->device, &si, NULL, &G.linear) != VK_SUCCESS) why = "sampler creation failed";
    }
    uint64_t t0 = plat_now_ns();
    for (int p = 0; !why && p < P_N; p++)
        if (!make_pass(c, p)) why = PASS[p].name;
    if (!why && !make_shared(c)) why = "its shared images could not be made";
    if (why) {
        rt_log("fsr3: AMD FSR 3.1 (the runtime's passes) off: %s", why);
        destroy_global();
        __atomic_store_n(&G.state, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&making, 0, __ATOMIC_RELEASE);
        return 0;
    }
    rt_log("fsr3: AMD FSR 3.1 upscaling with the runtime's passes (%d pipelines in %.0f ms)", P_N,
           (plat_now_ns() - t0) / 1e6);
    __atomic_store_n(&G.state, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&making, 0, __ATOMIC_RELEASE);
    return 1;
}

int vk_fsr3_available(void) { return __atomic_load_n(&G.state, __ATOMIC_ACQUIRE) != 0; }

int vk_fsr3_contexts_made(void) { return G.made; }

// ---- contexts

static void destroy_ctx(Ctx *x, int deferred) {
    const VkCtx *c = vk_ctx();
    for (int i = 0; i < SPD_VIEWS; i++)
        if (x->spd_mip[i]) deferred ? vk_defer_view(x->spd_mip[i]) : vkDestroyImageView(c->device, x->spd_mip[i], NULL);
    for (int i = 0; i < I_N; i++) {
        if (x->view[i]) deferred ? vk_defer_view(x->view[i]) : vkDestroyImageView(c->device, x->view[i], NULL);
        if (x->img[i]) deferred ? vk_defer_image(x->img[i], x->mem[i]) : vmaDestroyImage(c->vma, x->img[i], x->mem[i]);
    }
    memset(x, 0, sizeof *x);
}

static uint32_t at_least_1(uint32_t v) { return v ? v : 1; }

// The images for render sizes up to mw x mh (the passes take the size of each call from the constants and read only
// that part of an image) and the output size ow x oh.
static int make_ctx(Ctx *x, uint32_t mw, uint32_t mh, uint32_t ow, uint32_t oh) {
    memset(x, 0, sizeof *x);
    x->mw = mw, x->mh = mh, x->ow = ow, x->oh = oh;
    uint32_t hw = at_least_1(mw / 2), hh = at_least_1(mh / 2), big = hw > hh ? hw : hh;
    x->spd_levels = 1;
    while (big >>= 1) x->spd_levels++;
    VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (int i = 0; i < I_N; i++) {
        uint32_t iw = 1, ih = 1, levels = i == I_SPD ? x->spd_levels : 1;
        switch (IMG[i].size) {
        case SZ_RENDER: iw = mw, ih = mh; break;
        case SZ_HALF: iw = hw, ih = hh; break;
        case SZ_OUT: iw = ow, ih = oh; break;
        }
        if (!make_image(iw, ih, levels, IMG[i].fmt, u, &x->img[i], &x->mem[i]) ||
            !(x->view[i] = make_view(x->img[i], IMG[i].fmt, 0, levels))) {
            destroy_ctx(x, 0);
            return 0;
        }
    }
    for (uint32_t m = 0; m < SPD_VIEWS; m++) {
        uint32_t level = m < x->spd_levels ? m : x->spd_levels - 1;   // a small scene has fewer mips than the passes name
        if (!(x->spd_mip[m] = make_view(x->img[I_SPD], IMG[I_SPD].fmt, level, 1))) {
            destroy_ctx(x, 0);
            return 0;
        }
    }
    x->fresh = 1;
    // The constants that stay for the context's life: the largest sizes and the passes' tuning at its defaults
    x->k.max_render[0] = (int32_t)mw, x->k.max_render[1] = (int32_t)mh;
    x->k.max_upscale[0] = (int32_t)ow, x->k.max_upscale[1] = (int32_t)oh;
    x->k.velocity_factor = 1.0f;
    x->k.reactiveness_scale = 1.0f;
    x->k.shading_change_scale = 1.0f;
    x->k.accumulation_per_frame = 1.0f / 3.0f;
    x->k.min_disocclusion = -1.0f / 3.0f;
    return 1;
}

static void trim(uint64_t now_ns) {
    for (int i = 0; i < CTX_MAX; i++) {
        Ctx *x = &G.ctx[i];
        if (!x->img[0] || now_ns - x->used_ns <= CTX_IDLE_NS) continue;
        rt_log("fsr3: the context for %ux%u (render size %ux%u) destroyed: no upscale used it for 90 s", x->ow, x->oh,
               x->mw, x->mh);
        destroy_ctx(x, 1);
    }
}

int vk_fsr3_holds(void) {
    for (int i = 0; i < CTX_MAX; i++)
        if (G.ctx[i].img[0]) return 1;
    return 0;
}

void vk_fsr3_release_idle(void) {
    for (int i = 0; i < CTX_MAX; i++) {
        Ctx *x = &G.ctx[i];
        if (!x->img[0]) continue;
        rt_log("fsr3: the context for %ux%u (render size %ux%u) destroyed: the upscaler is off", x->ow, x->oh, x->mw,
               x->mh);
        destroy_ctx(x, 0);   // the device is idle (the caller waited for it)
    }
}

// One context per render size and output size, as vk_ffx.cpp context_for keeps the library's: a render size of its own
// starts a new history, as the library's new context does. With the game's dynamic resolution on (NX2NAVI_DYN_RES,
// dynres.c), whose steps change the render size every few seconds, one context per output size takes every render
// size up to the output's, each call's own size coming from the constants. A context of another render size goes once
// no command buffer still being recorded can name it (CTX_RECENT), and the game's second scenes, of another output
// size, keep their own. When all are taken, the least recently used one goes for the new size. Its images go through
// vk_defer_image: the GPU may still run a command buffer that names them.
static Ctx *ctx_for(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh) {
    G.tick++;
    uint64_t now = plat_now_ns();
    const int drs = cfg_bool("NX2NAVI_DYN_RES");
    Ctx *found = NULL;
    for (int i = 0; i < CTX_MAX; i++) {
        Ctx *x = &G.ctx[i];
        if (x->img[0] && x->ow == ow && x->oh == oh && x->drs == drs &&
            (drs ? w <= x->mw && h <= x->mh : w == x->mw && h == x->mh))
            found = x;
    }
    for (int i = 0; i < CTX_MAX; i++) {   // the contexts of this output size for another render size, once not recent
        Ctx *x = &G.ctx[i];
        if (x->img[0] && x != found && x->ow == ow && x->oh == oh && G.tick - x->used >= CTX_RECENT) {
            rt_log("fsr3: the context for %ux%u (render size %ux%u) destroyed: the render size changed", x->ow, x->oh,
                   x->mw, x->mh);
            destroy_ctx(x, 1);
        }
    }
    if (found) {
        found->used = G.tick, found->used_ns = now;
        return found;
    }
    Ctx *slot = NULL, *lru = NULL;
    for (int i = 0; i < CTX_MAX; i++) {
        Ctx *x = &G.ctx[i];
        if (!x->img[0]) {
            if (!slot) slot = x;
            continue;
        }
        if (G.tick - x->used >= CTX_RECENT && (!lru || x->used < lru->used)) lru = x;
    }
    if (!slot && lru) {
        static int told;
        if (told++ < 20)
            rt_log("fsr3: the context for %ux%u (render size %ux%u) destroyed: its slot went to another output size",
                   lru->ow, lru->oh, lru->mw, lru->mh);
        destroy_ctx(lru, 1);
        slot = lru;
    }
    if (!slot) return NULL;
    // the render size itself; for dynamic resolution, every size up to the output's (and a scene at the output size)
    uint32_t mw = !drs ? w : w > ow ? w : ow, mh = !drs ? h : h > oh ? h : oh;
    if (!make_ctx(slot, mw, mh, ow, oh)) {
        rt_log("fsr3: images for %ux%u from %ux%u could not be made", ow, oh, mw, mh);
        return NULL;
    }
    slot->drs = drs, slot->used = G.tick, slot->used_ns = now;
    G.made++;
    rt_log("fsr3: FSR 3.1 upscaling to %ux%u from %s%ux%u (the runtime's passes, %u SPD mips)", ow, oh,
           drs ? "render sizes up to " : "", mw, mh, slot->spd_levels);
    return slot;
}

// ---- recording

static void barrier(VkCommandBuffer cb, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds,
                    VkAccessFlags2 da) {
    VkMemoryBarrier2 m = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                          .dstStageMask = ds, .dstAccessMask = da};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m};
    vkCmdPipelineBarrier2(cb, &d);
}
static const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, XF = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
static const VkAccessFlags2 CS_RW = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;

static void clear(VkCommandBuffer cb, VkImage img, float r, float g, float b, float a) {
    VkClearColorValue v = {.float32 = {r, g, b, a}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &all);
}

static void to_general(VkCommandBuffer cb, const VkImage *img, int n) {
    VkImageMemoryBarrier2 b[I_N + 2];
    for (int i = 0; i < n; i++)
        b[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = XF | CS,
                                       .dstStageMask = XF | CS,
                                       .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | CS_RW,
                                       .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img[i],
                                       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = (uint32_t)n,
                          .pImageMemoryBarriers = b};
    vkCmdPipelineBarrier2(cb, &d);
}

// The constants of this call: sizes, jitter, the depth to view-space conversion of the camera's near and far planes
// (depth not inverted, 0 at the near plane; a projection without a far plane has VK_DLAA_FAR_INFINITE there, far
// enough to be the same), the frame index and the jitter phase count, which steps towards its new value one phase per
// call.
static void fill_constants(Ctx *x, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, float jx, float jy, int reset,
                           float dt_ms, float fov_y, float znear, float zfar, float meters) {
    Consts *k = &x->k;
    k->prev_jitter[0] = k->jitter[0], k->prev_jitter[1] = k->jitter[1];
    k->jitter[0] = jx, k->jitter[1] = jy;
    k->prev_render[0] = k->render[0], k->prev_render[1] = k->render[1];
    k->render[0] = (int32_t)w, k->render[1] = (int32_t)h;
    float aspect = (float)w / (float)h;
    k->tan_half_fov = tanf(atanf(tanf(fov_y / 2) * aspect));   // tan of half the horizontal angle
    k->view_to_meters = meters;
    // view depth from device depth d: device_to_view[1] / (d - device_to_view[0]), with q the projection's far / (near
    // - far)
    float lo = fminf(znear, zfar), hi = fmaxf(znear, zfar), q = hi / (lo - hi);
    k->device_to_view[0] = -q;
    k->device_to_view[1] = q * lo;
    float cot = cosf(0.5f * fov_y) / sinf(0.5f * fov_y);
    k->device_to_view[2] = aspect / cot;
    k->device_to_view[3] = 1.0f / cot;
    k->prev_upscale[0] = k->upscale[0], k->prev_upscale[1] = k->upscale[1];
    k->upscale[0] = (int32_t)ow, k->upscale[1] = (int32_t)oh;
    k->downscale[0] = (float)w / (float)ow, k->downscale[1] = (float)h / (float)oh;
    k->delta_pre_exposure = x->pre_exposure > 0 ? 1.0f / x->pre_exposure : 1.0f;   // the pre-exposure is always 1
    x->pre_exposure = 1.0f;
    k->mv_scale[0] = 1.0f / (float)w, k->mv_scale[1] = 1.0f / (float)h;   // render pixels to UV
    k->mv_jitter_cancel[0] = k->mv_jitter_cancel[1] = 0;                  // the vectors carry no jitter
    float r = (float)ow / (float)w;
    int32_t phases = (int32_t)(8.0f * r * r);
    if (reset || k->jitter_phases == 0) k->jitter_phases = (float)phases;
    else if (phases > (int32_t)k->jitter_phases) k->jitter_phases++;
    else if (phases < (int32_t)k->jitter_phases) k->jitter_phases--;
    float dt = dt_ms / 1000.0f;
    k->delta_time = dt < 0 ? 0 : dt > 1 ? 1 : dt;
    k->frame_index = reset ? 0.0f : k->frame_index + 1.0f;
}

// IEEE half of a float in (0, 1] (the RCAS constant), rounded to nearest
static uint16_t half_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    int32_t e = (int32_t)((u >> 23) & 0xff) - 127 + 15;
    uint32_t m = u & 0x7fffff;
    if (e <= 0) return (uint16_t)((u >> 16) & 0x8000);   // below the halves' normal range: 0 (never here)
    uint32_t h = (u >> 16 & 0x8000) | (uint32_t)e << 10 | m >> 13;
    if (m & 0x1000) h++;   // round half up on the dropped bits
    return (uint16_t)h;
}

static uint32_t ring_slot(void) {
    uint32_t s = G.ring_next;
    G.ring_next = (s + 1) % RING;
    uint64_t f = G.ring_fence[s];
    if (f && f > vk_sync_fence_done() && !vk_sync_fence_wait(f, 100000000ull)) {   // RING calls ago: done long since
        static int told;
        if (told++ < 4) rt_log("fsr3: constant slot %u still in use by submission %llu", s, (unsigned long long)f);
    }
    G.ring_fence[s] = vk_sync_issued() + 1;   // the submission being recorded
    return s;
}

int vk_fsr3_upscale(VkCommandBuffer cb, const VkFfxImage *color, const VkFfxImage *depth, const VkFfxImage *mv,
                    const VkFfxImage *out, float jx, float jy, int reset, float dt_ms, const VkFfxCamera *cam,
                    int sharpen, float sharpness) {
    if (vk_fsr3_init() != 1) return 0;
    if (!color->view || !depth->view || !mv->view || !out->view || !out->storage) {
        static int told;
        if (!told++) rt_log("fsr3: an input or the output has no view (or the output is not a storage image)");
        return 0;
    }
    uint32_t w = color->width, h = color->height, ow = out->width, oh = out->height;
    if (!w || !h || !ow || !oh) return 0;
    trim(plat_now_ns());
    Ctx *x = ctx_for(w, h, ow, oh);
    if (!x) return 0;
    // the frame's camera, or where it is not known the values vk_ffx.cpp vk_ffx_upscale gives the library then
    float fov_y = cam && cam->fov_y > 0 && cam->fov_y < 3.14159f ? cam->fov_y : 1.0f;
    float znear = cam && cam->znear > 0 ? cam->znear : 0.1f, zfar = cam && cam->zfar > 0 ? cam->zfar : 1e8f;
    float meters = cam && cam->meters > 0 ? cam->meters : 1.0f;

    // what drew the inputs, and the last call's reads of what this one writes, come first
    barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, XF | CS,
            VK_ACCESS_2_TRANSFER_WRITE_BIT | CS_RW);
    if (G.shared_fresh) {
        VkImage s[2] = {G.lut, G.zero};
        to_general(cb, s, 2);
        VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {128, 1, 1}};
        vkCmdCopyBufferToImage(cb, G.lut_src, G.lut, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        clear(cb, G.zero, 0, 0, 0, 0);
        G.shared_fresh = 0;
    }
    int first = x->fresh;
    if (x->fresh) {   // every image cleared once, so nothing reads what memory held before
        to_general(cb, x->img, I_N);
        for (int i = 0; i < I_N; i++) clear(cb, x->img[i], 0, 0, 0, 0);
        x->fresh = 0;
    }
    reset = reset || first;
    const int odd = x->odd;
    x->odd = !odd;
    fill_constants(x, w, h, ow, oh, jx, jy, reset, dt_ms, fov_y, znear, zfar, meters);

    if (reset) {
        clear(cb, x->img[odd ? I_ACC1 : I_ACC0], 0, 0, 0, 0);   // the accumulation this call reads
        clear(cb, x->img[I_FINFO], -1.0f, 1.0f, 0, 0);           // auto exposure starts again
    }
    // every call: the reconstructed nearest depth (to the far value, as bits of 1.0f), the SPD mips and their counter
    clear(cb, x->img[I_RECON], 1.0f, 1.0f, 1.0f, 1.0f);
    clear(cb, x->img[I_SPD], 0, 0, 0, 0);
    clear(cb, x->img[I_ATOMIC], 0, 0, 0, 0);
    barrier(cb, XF, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, CS_RW);

    // the constants: this call's slot of the ring
    uint32_t slot = ring_slot();
    VkDeviceSize base = (VkDeviceSize)slot * G.slot_bytes;
    uint8_t *at = G.ring_map + base;
    memcpy(at, &x->k, sizeof x->k);
    uint32_t groups[2], offset[2];
    {   // SPD over the render size (the luma and shading change pyramids)
        offset[0] = offset[1] = 0;
        groups[0] = (w - 1) / 64 + 1, groups[1] = (h - 1) / 64 + 1;
        uint32_t big = w > h ? w : h, mips = 0;
        while (big >>= 1) mips++;   // floor(log2(max(w, h)))
        SpdConsts s = {mips > 12 ? 12 : mips, groups[0] * groups[1], {offset[0], offset[1]}, {w, h}};
        memcpy(at + G.block, &s, sizeof s);
    }
    {   // RCAS: the slider's 0..1 as stops of sharpness reduction (0 = most), packed as the pass reads it
        float stops = -2.0f * sharpness + 2.0f, lin = exp2f(-stops);
        uint32_t con[4] = {0, 0, 0, 0}, hb = (uint32_t)half_bits(lin);
        memcpy(&con[0], &lin, 4);
        con[1] = hb | hb << 16;   // the same as a pair of halves, for a 16-bit build of the pass
        memcpy(at + 2 * (size_t)G.block, con, sizeof con);
    }
    vmaFlushAllocation(vk_ctx()->vma, G.ring_mem, base, G.slot_bytes);

    // what each binding names this call
    VkImageView v[C_FIRST] = {0};
    v[S_COLOR] = color->view, v[S_DEPTH] = depth->view, v[S_MV] = mv->view;
    v[S_DMV] = v[U_DMV] = x->view[I_DMV];
    v[S_DDEPTH] = v[U_DDEPTH] = x->view[I_DDEPTH];
    v[S_RECON] = v[U_RECON] = x->view[I_RECON];
    v[S_REACTIVE] = v[S_TC] = G.zero_view;
    v[S_ACC] = x->view[odd ? I_ACC1 : I_ACC0], v[U_ACC] = x->view[odd ? I_ACC0 : I_ACC1];
    v[S_SHADE] = v[U_SHADE] = x->view[I_SHADE];
    v[S_CLUMA] = v[U_CLUMA] = x->view[odd ? I_LUMA1 : I_LUMA0], v[S_PLUMA] = x->view[odd ? I_LUMA0 : I_LUMA1];
    v[S_EXPOSURE] = v[S_FINFO] = v[U_FINFO] = x->view[I_FINFO];   // auto exposure: the frame info is the exposure
    v[S_LH] = x->view[odd ? I_LH1 : I_LH0], v[U_LH] = x->view[odd ? I_LH0 : I_LH1];
    v[S_FDMIP1] = v[U_FDMIP1] = x->view[I_FDMIP1];
    v[S_FD] = v[U_FD] = v[S_LINST] = v[U_LINST] = x->view[I_INTER];
    v[S_UP] = x->view[odd ? I_UP1 : I_UP0], v[U_UP] = v[S_RCAS_IN] = x->view[odd ? I_UP0 : I_UP1];
    v[S_LUT] = G.lut_view;
    v[S_DREACT] = v[U_DREACT] = x->view[I_DREACT];
    v[S_SPD] = x->view[I_SPD];
    for (int m = 0; m < SPD_VIEWS; m++) v[U_SPD0 + m] = x->spd_mip[m];
    v[U_ATOMIC] = x->view[I_ATOMIC];
    v[U_LOCKS] = x->view[I_LOCKS];
    v[U_OUT] = out->view;

    const uint32_t src[2] = {(w + 7) / 8, (h + 7) / 8}, dst[2] = {(ow + 7) / 8, (oh + 7) / 8};
    const uint32_t shade[2] = {((uint32_t)(w * 0.5f) + 7) / 8, ((uint32_t)(h * 0.5f) + 7) / 8};
    const uint32_t rcas[2] = {(ow + 15) / 16, (oh + 15) / 16};
    struct {
        int p;
        const uint32_t *n;
    } order[] = {{P_PREP_IN, src},      {P_LUMA_PYR, groups},   {P_SHADE_PYR, groups},
                 {P_SHADE, shade},      {P_PREP_REACT, src},    {P_LUMA_INST, src},
                 {sharpen ? P_ACCUM_SHARP : P_ACCUM, dst},      {P_RCAS, rcas}};
    int passes = sharpen ? 8 : 7;
    for (int i = 0; i < passes; i++) {
        int p = order[i].p;
        VkWriteDescriptorSet wr[MAX_BINDS];
        VkDescriptorImageInfo ii[MAX_BINDS];
        VkDescriptorBufferInfo bi[MAX_BINDS];
        for (uint32_t j = 0; j < PASS[p].n; j++) {
            int r = PASS[p].b[j].res;
            wr[j] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstBinding = PASS[p].b[j].binding, .descriptorCount = 1};
            if (r >= C_FIRST) {
                bi[j] = (VkDescriptorBufferInfo){G.ring, base + (VkDeviceSize)(r - C_FIRST) * G.block,
                                                 r == C_MAIN ? sizeof(Consts) : r == C_SPD ? sizeof(SpdConsts) : 16};
                wr[j].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, wr[j].pBufferInfo = &bi[j];
            } else {
                ii[j] = (VkDescriptorImageInfo){VK_NULL_HANDLE, v[r], VK_IMAGE_LAYOUT_GENERAL};
                wr[j].descriptorType = r >= U_FIRST ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                wr[j].pImageInfo = &ii[j];
            }
        }
        if (i) barrier(cb, CS, VK_ACCESS_2_SHADER_WRITE_BIT, CS, CS_RW);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, G.pipe[p]);
        rec_push(G.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, G.layout[p], 0, PASS[p].n, wr);
        vkCmdDispatch(cb, order[i].n[0], order[i].n[1], 1);
    }
    // the caller's barrier (vk_dlaa.c evaluate) orders its reads of the output after these writes
    static int told;
    if (!told++)
        rt_log("fsr3: first upscale %ux%u -> %ux%u recorded (%d passes, sharpening %s)", w, h, ow, oh, passes,
               sharpen ? "on" : "off");
    return 1;
}

void vk_fsr3_release(void) {
    if (!vk_ctx()) return;
    for (int i = 0; i < CTX_MAX; i++)
        if (G.ctx[i].img[0]) destroy_ctx(&G.ctx[i], 0);
    destroy_global();
    memset(G.ring_fence, 0, sizeof G.ring_fence);
    G.ring_next = 0, G.tick = 0, G.shared_fresh = 0;
    __atomic_store_n(&G.state, -1, __ATOMIC_RELEASE);   // a new device makes everything again
}
