// The anti-aliasing setting (NX2NAVI_AA, nx2navi.ini aa): what runs in place of the game's own SMAA.
//
// Ocarina anti-aliases every frame with SMAA in three compute passes right after the bloom composite: edge detection
// into 4x4-pixel tiles, blending weights from the tiles (an indirect dispatch, with the game's area lookup texture),
// and the composition, which reads the composite and the weights and writes the result into the scene target that the
// game's upscaler, the HUD and the output passes then take. The composition stores each channel's square root packed
// as RGB9E5 words through an integer view of that R11G11B10F target (vk_exec_draw.c raw_note).
//
// aa = game (the default) leaves those passes alone. off, fxaa and smaa leave out the edge and weight passes and
// replace the composition (vk_exec_draw.c aa_dispatch) with one of the passes here, which read the same composite and
// write the same words: off stores it as it is, fxaa smooths it first (shaders/aa_post.comp), smaa runs SMAA at its
// Ultra preset (shaders/smaa_*.comp, the reference implementation in shaders/smaa.glsl with the area and search
// textures of third_party/smaa). taa is DLSS or FSR at the scene's own size: the menu sets the upscaler for it
// (ui/menu.cpp row_aa), and the game's SMAA runs with it. While DLSS or FSR anti-aliases or scales the scene (dlaa = 1),
// game, off, fxaa and smaa still choose the pass above: in their native modes the upscalers run before the bloom (vk_dlaa.c,
// "where DLAA runs") and the pass smooths their picture after it, in the scaling modes the pass runs on the scene first
// and the upscaler scales its result. off then leaves the upscaler's TAA alone on the edges.
//
// The game's passes are known by a hash of their guest code (vk_aa_role), which does not change with the translator or
// its settings. Everything here runs on the replay thread.
#include "vk.h"

#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"   // gfx_counters
#include "vk_dlaa.h"
#include "vk_exec.h"
#include "aa_post_spv.h"
#include "smaa_blend_spv.h"
#include "smaa_edge_spv.h"
#include "smaa_encode_spv.h"
#include "smaa_weight_spv.h"

static const unsigned char area_bytes[] = {
#include "smaa_area_tex.h"
};
static const unsigned char search_bytes[] = {
#include "smaa_search_tex.h"
};
#define AREA_W 160
#define AREA_H 560
#define SEARCH_W 64
#define SEARCH_H 16

// FNV-1a over the guest code of the game's three SMAA compute programs (vk_exec_draw.c cs_guest_hash), game 1.1.1,
// found with NX2NAVI_AA_LOG=1: the edges (a 4x4-pixel tile per invocation), the weights (an indirect dispatch over the
// tiles with edges) and the composition (16x16 pixels per group)
static const struct { uint64_t hash; int role; } GAME_SMAA[] = {
    {0xfcb21f2afa88378cull, VK_AA_ROLE_EDGES},
    {0xe44377cbfafc1101ull, VK_AA_ROLE_WEIGHTS},
    {0x470d661afb1cb73aull, VK_AA_ROLE_COMPOSE},
};

int vk_aa_role(uint64_t guest_hash) {
    if (!guest_hash) return VK_AA_ROLE_NONE;
    for (size_t i = 0; i < sizeof GAME_SMAA / sizeof GAME_SMAA[0]; i++)
        if (GAME_SMAA[i].hash == guest_hash) return GAME_SMAA[i].role;
    return VK_AA_ROLE_NONE;
}

static const char *const NAMES[] = {"game", "off", "fxaa", "smaa"};

// DLSS or FSR anti-aliases this frame's scene: dlaa = 1 and one of them runs (vk_dlaa.c)
static int temporal(void) { return cfg_bool("NX2NAVI_DLAA") && (vk_dlaa_available() || vk_dlaa_fsr3()); }

static struct {
    uint64_t frame_read;
    int mode;              // this frame's (vk_aa_mode)
    int said;              // taa was last logged as waiting for DLSS or FSR
    int composed;          // the last composition seen was replaced (vk_aa_composed): the passes before it may go
    uint64_t frames_on, composes;   // frames with a replacement asked for, compositions replaced
    int none_told;
} A = {.frame_read = ~0ull, .composed = 1};

int vk_aa_mode(void) {
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (f == A.frame_read) return A.mode;
    A.frame_read = f;
    const char *v = cfg_value("NX2NAVI_AA");
    int want = VK_AA_GAME;
    for (int i = 1; i < 4; i++)
        if (!strcmp(v, NAMES[i])) want = i;
    // taa is the game's SMAA with DLSS's or FSR's TAA; without them at work, the game's SMAA alone
    int said = !strcmp(v, "taa") && !temporal();
    if (said != A.said) {
        A.said = said;
        if (said) rt_log("aa: taa needs DLSS or FSR at work (dlaa = 1): the game's SMAA stays");
    }
    A.mode = want;
    if (A.mode != VK_AA_GAME && !A.none_told && ++A.frames_on == 3600 && !A.composes) {
        A.none_told = 1;   // a game version or a frame without the passes this file knows: nothing is replaced
        rt_log("aa: aa = %s found none of the game's SMAA passes in %llu frames: the picture stays the game's",
               NAMES[A.mode], (unsigned long long)A.frames_on);
    }
    return A.mode;
}

int vk_aa_skip_ok(void) { return A.composed; }

// A composition that could not be replaced runs as the game's, and the edge and weight passes run again from the next
// frame on, until a composition is replaced again: the game's composition must never read weights left out.
void vk_aa_composed(int ok) {
    if (ok && !A.composes++) rt_log("aa: the game's SMAA replaced by %s", NAMES[A.mode]);
    A.composed = ok;
}

// ---------------------------------------------------------------- pipelines
enum { P_POST, P_LUMA, P_EDGE, P_WEIGHT, P_BLEND, P_N };
#define S VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
#define I VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
static const struct { const uint32_t *code; size_t size; int n; VkDescriptorType t[4]; const char *name; } PIPES[P_N] = {
    {aa_post_spv, sizeof aa_post_spv, 2, {S, I}, "aa_post"},
    {smaa_encode_spv, sizeof smaa_encode_spv, 2, {S, I}, "smaa_encode"},
    {smaa_edge_spv, sizeof smaa_edge_spv, 2, {S, I}, "smaa_edge"},
    {smaa_weight_spv, sizeof smaa_weight_spv, 4, {S, S, S, I}, "smaa_weight"},
    {smaa_blend_spv, sizeof smaa_blend_spv, 3, {S, S, I}, "smaa_blend"},
};
#undef S
#undef I

typedef struct { VkImage img; VmaAllocation mem; VkImageView view; } Img;

static struct {
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl[P_N];
    VkPipelineLayout layout[P_N];
    VkPipeline pipe[P_N];
    VkSampler linear, nearest;
    int tables;          // the area and search textures: 0 not made, 1 ready, -1 failed
    Img area, search;
    Img luma, edges, weights;   // at the scene's size (w, h)
    uint32_t w, h;
} P;

static int pipes_init(void) {
    if (P.tried) return P.ok;
    P.tried = 1;
    VkCtx *c = vk_ctx();
    P.push = (PFN_vkCmdPushDescriptorSetKHR)vk_dpa(c->device, "vkCmdPushDescriptorSetKHR");
    int ok = P.push != NULL;
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    ok = ok && vkCreateSampler(c->device, &si, NULL, &P.linear) == VK_SUCCESS;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    ok = ok && vkCreateSampler(c->device, &si, NULL, &P.nearest) == VK_SUCCESS;
    for (int p = 0; ok && p < P_N; p++) {
        VkDescriptorSetLayoutBinding b[4];
        for (int i = 0; i < PIPES[p].n; i++)
            b[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorType = PIPES[p].t[i],
                                                  .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
        VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                               .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                               .bindingCount = (uint32_t)PIPES[p].n, .pBindings = b};
        VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
        VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &P.dsl[p], .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
        VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = PIPES[p].size,
                                       .pCode = PIPES[p].code};
        VkShaderModule m = VK_NULL_HANDLE;
        ok = vkCreateDescriptorSetLayout(c->device, &dci, NULL, &P.dsl[p]) == VK_SUCCESS &&
             vkCreatePipelineLayout(c->device, &li, NULL, &P.layout[p]) == VK_SUCCESS &&
             vkCreateShaderModule(c->device, &mi, NULL, &m) == VK_SUCCESS;
        if (ok) {
            VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                              .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                        .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                              .layout = P.layout[p]};
            ok = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &P.pipe[p]) == VK_SUCCESS;
        }
        if (m) vkDestroyShaderModule(c->device, m, NULL);
        if (!ok) rt_log("aa: the %s pipeline could not be created", PIPES[p].name);
    }
    rt_log("aa: anti-aliasing passes %s", ok ? "ready" : "unavailable: the game's SMAA stays");
    return P.ok = ok;
}

static void img_free(Img *m) {
    if (m->view) vk_defer_view(m->view);
    if (m->img) vk_defer_image(m->img, m->mem);
    memset(m, 0, sizeof *m);
}

// An image of the passes' own, in the GENERAL layout from cb on (the transition is recorded into cb)
static int img_make(VkCommandBuffer cb, Img *m, VkFormat f, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    VkCtx *c = vk_ctx();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, &m->img, &m->mem, NULL) != VK_SUCCESS) return m->img = VK_NULL_HANDLE, 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = m->img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &m->view) != VK_SUCCESS) {
        vmaDestroyImage(c->vma, m->img, m->mem);
        memset(m, 0, sizeof *m);
        return 0;
    }
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                               .newLayout = (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                                                                       : VK_IMAGE_LAYOUT_GENERAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = m->img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
    return 1;
}

// The area and search textures, uploaded once through the upload batch and waited for (the first SMAA frame only)
static int tables_make(void) {
    if (P.tables) return P.tables > 0;
    P.tables = -1;
    VkCommandBuffer cb = vk_upload_begin();
    int ok = img_make(cb, &P.area, VK_FORMAT_R8G8_UNORM, AREA_W, AREA_H, VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
             img_make(cb, &P.search, VK_FORMAT_R8_UNORM, SEARCH_W, SEARCH_H, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceSize off = 0;
    uint8_t *st = ok ? vk_staging_at(sizeof area_bytes + sizeof search_bytes, &buf, &off) : NULL;
    if (st) {
        memcpy(st, area_bytes, sizeof area_bytes);
        memcpy(st + sizeof area_bytes, search_bytes, sizeof search_bytes);
        VkBufferImageCopy r = {.bufferOffset = off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .imageExtent = {AREA_W, AREA_H, 1}};
        vkCmdCopyBufferToImage(cb, buf, P.area.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        r.bufferOffset = off + sizeof area_bytes, r.imageExtent = (VkExtent3D){SEARCH_W, SEARCH_H, 1};
        vkCmdCopyBufferToImage(cb, buf, P.search.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        VkImageMemoryBarrier2 b[2];
        for (int i = 0; i < 2; i++)
            b[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT,
                                           .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                           .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                           .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                           .image = i ? P.search.img : P.area.img,
                                           .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = b};
        vkCmdPipelineBarrier2(cb, &d);
    }
    vk_upload_end();
    if (!st) {
        img_free(&P.area), img_free(&P.search);
        rt_log("aa: SMAA's lookup textures could not be made: smaa runs as off");
        return 0;
    }
    vk_upload_flush(1);
    P.tables = 1;
    return 1;
}

// SMAA's work images at the scene's size, made again when it changes
static int work_images(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    if (P.luma.img && P.w == w && P.h == h) return 1;
    img_free(&P.luma), img_free(&P.edges), img_free(&P.weights);
    P.w = P.h = 0;
    if (!img_make(cb, &P.luma, VK_FORMAT_R16_SFLOAT, w, h, VK_IMAGE_USAGE_STORAGE_BIT) ||
        !img_make(cb, &P.edges, VK_FORMAT_R8G8B8A8_UNORM, w, h, VK_IMAGE_USAGE_STORAGE_BIT) ||
        !img_make(cb, &P.weights, VK_FORMAT_R8G8B8A8_UNORM, w, h, VK_IMAGE_USAGE_STORAGE_BIT)) {
        img_free(&P.luma), img_free(&P.edges), img_free(&P.weights);
        return 0;
    }
    P.w = w, P.h = h;
    return 1;
}

// One pass: its pipeline, its images (samplers: linear but for the search texture) and the push constants
static void pass(VkCommandBuffer cb, int p, const VkImageView *views, const VkSampler *samp, const void *pc, size_t pcn,
                 uint32_t w, uint32_t h) {
    VkDescriptorImageInfo ii[4];
    VkWriteDescriptorSet wr[4];
    for (int i = 0; i < PIPES[p].n; i++) {
        int sampled = PIPES[p].t[i] == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ii[i] = (VkDescriptorImageInfo){sampled ? samp[i] : VK_NULL_HANDLE, views[i], VK_IMAGE_LAYOUT_GENERAL};
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i,
                                       .descriptorCount = 1, .descriptorType = PIPES[p].t[i], .pImageInfo = &ii[i]};
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, P.pipe[p]);
    rec_push(P.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, P.layout[p], 0, (uint32_t)PIPES[p].n, wr);   // through the recorder
    vkCmdPushConstants(cb, P.layout[p], VK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)pcn, pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &d);
}

int vk_aa_record(VkCommandBuffer cb, int mode, VkImageView src, VkImageView dst, uint32_t w, uint32_t h) {
    if (mode == VK_AA_GAME || !w || !h || !pipes_init()) return 0;
    if (mode == VK_AA_SMAA && (!tables_make() || !work_images(cb, w, h))) {
        static int told;
        if (!told++) rt_log("aa: SMAA's images could not be made (%ux%u): smaa runs as off", w, h);
        mode = VK_AA_OFF;
    }
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT};
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &dep);
    if (mode != VK_AA_SMAA) {
        struct { float inv[2]; int32_t size[2]; int32_t mode; } pc = {{1.0f / (float)w, 1.0f / (float)h},
                                                                      {(int32_t)w, (int32_t)h}, mode == VK_AA_FXAA};
        const VkImageView v[2] = {src, dst};
        const VkSampler s[2] = {P.linear, VK_NULL_HANDLE};
        pass(cb, P_POST, v, s, &pc, sizeof pc, w, h);
        return 1;
    }
    const float metrics[4] = {1.0f / (float)w, 1.0f / (float)h, (float)w, (float)h};
    const VkSampler lin[4] = {P.linear, P.linear, P.linear, P.linear};
    pass(cb, P_LUMA, (const VkImageView[]){src, P.luma.view}, lin, metrics, sizeof metrics, w, h);
    pass(cb, P_EDGE, (const VkImageView[]){P.luma.view, P.edges.view}, lin, metrics, sizeof metrics, w, h);
    pass(cb, P_WEIGHT, (const VkImageView[]){P.edges.view, P.area.view, P.search.view, P.weights.view},
         (const VkSampler[]){P.linear, P.linear, P.nearest, VK_NULL_HANDLE}, metrics, sizeof metrics, w, h);
    pass(cb, P_BLEND, (const VkImageView[]){src, P.weights.view, dst}, lin, metrics, sizeof metrics, w, h);
    return 1;
}
