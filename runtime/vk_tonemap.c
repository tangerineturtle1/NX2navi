// Decode-and-scale pass standing in for the scene upscaler (vk.h vk_tonemap_record; used by vk_exec_draw.c
// untranslated_dispatch_fallback).
#include "platform/platform.h"
#include "vk.h"

#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "fallback_tonemap_spv.h"
#include "fsr_easu_spv.h"
#include "host.h"

// The game's upscaler (tensor-core, untranslatable) reads the post pass's picture, whose channels hold the square root of
// their linear value packed as RGB9E5 (prog 102), and writes the linear picture the HUD is drawn over; the frame's last
// pass encodes to sRGB. This pass (shaders/fallback_tonemap.spvasm) scales with linear filtering in the stored values
// and squares them into an RGBA16F image of the target's size, which the caller blits in 1:1. The game composites
// and tonemaps before the post pass, so nothing here changes exposure. (It was a tonemap once, when the input was
// taken for the raw HDR scene; the blit's sRGB decode that stood in since darkened mid-tones and lifted the darkest
// values.)
//
// The filter: the console's upscaler is a network that rebuilds edges, and a linear filter over a scene drawn at about
// three quarters of the picture is visibly softer than that. With NX2NAVI_STANDIN_FILTER = easu (the default) the
// pass that enlarges is shaders/fsr_easu.comp instead, the edge-adaptive filter the present scales to the window with
// (vk_fsr.c), built here with its SQUARE constant set: it filters the same stored values, which are the perceptual
// encoding that filter is made for, and squares the result as the linear pass does, so only how the scaled sample is
// taken differs. bilinear keeps the linear pass. The linear pass also stays where nothing is enlarged (the filter only
// enlarges) and where the caller says another upscaler's picture replaces this one (vk_exec_draw.c
// untranslated_dispatch_fallback: DLSS or FSR 3.1, vk_dlaa.c). A scene as large as the picture (scene_share = 1, the
// menu's Native: widescreen.c plain_share) is not scaled at all: the linear pass samples it with a nearest-texel
// sampler, so each output texel is its source texel squared, whatever the filtering hardware's sub-texel precision.
// The movie's frames do not come through here: the caller blits those (RGBA8) itself.
static struct {
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;               // scale with linear filtering, then square (shaders/fallback_tonemap.spvasm)
    VkPipeline easu;               // scale with the edge-adaptive filter, then square (shaders/fsr_easu.comp)
    int easu_tried;
    int filter_logs;               // lines tm_filter_log has written
    int filter_last;               // what its last line named: 1 the linear filter, 2 the edge-adaptive one, 3 a copy 1:1
    VkSampler sampler;
    VkSampler nearest;             // the same with nearest-texel filtering: a source of the output's size (copy 1:1)
    struct { VkImage img; VkFormat f; VkImageView view; } views[16];
    int nviews;
    VkImage out;                   // the pass's own output (RGBA16F, the target's size); the caller blits it in
    VmaAllocation out_alloc;
    VkImageView out_view;
    uint32_t ow, oh;
} TM;

// The output image at w x h, made again when the size changes (the old one is left to the deferred release).
static int tm_out(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    if (TM.out && TM.ow == w && TM.oh == h) return 1;
    VkCtx *c = vk_ctx();
    if (TM.out) vk_defer_view(TM.out_view), vk_defer_image(TM.out, TM.out_alloc), TM.out = VK_NULL_HANDLE;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, &TM.out, &TM.out_alloc, NULL) != VK_SUCCESS) return TM.out = VK_NULL_HANDLE, 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = TM.out,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &TM.out_view) != VK_SUCCESS) {
        vmaDestroyImage(c->vma, TM.out, TM.out_alloc);
        return TM.out = VK_NULL_HANDLE, 0;
    }
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = TM.out, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
    TM.ow = w, TM.oh = h;
    return 1;
}

static int tm_init(void) {
    if (TM.tried) return TM.ok;
    TM.tried = 1;
    VkCtx *c = vk_ctx();
    TM.push = (PFN_vkCmdPushDescriptorSetKHR)vk_dpa(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 2, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};   // the linear pass reads 24 bytes, tm_easu's 32
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = sizeof fallback_tonemap_spv, .pCode = fallback_tonemap_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    int ok = TM.push && vkCreateDescriptorSetLayout(c->device, &dci, NULL, &TM.dsl) == VK_SUCCESS;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &TM.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkSamplerCreateInfo ni = si;
    ni.magFilter = ni.minFilter = VK_FILTER_NEAREST;
    ok = ok && vkCreatePipelineLayout(c->device, &li, NULL, &TM.layout) == VK_SUCCESS &&
         vkCreateSampler(c->device, &si, NULL, &TM.sampler) == VK_SUCCESS &&
         vkCreateSampler(c->device, &ni, NULL, &TM.nearest) == VK_SUCCESS &&
         vkCreateShaderModule(c->device, &mi, NULL, &m) == VK_SUCCESS;
    if (ok) {
        VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                    .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                          .layout = TM.layout};
        ok = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &TM.pipe) == VK_SUCCESS;
    }
    if (m) vkDestroyShaderModule(c->device, m, NULL);
    rt_log("vk_exec: upscaler stand-in: %s", ok ? "scale and square-decode pass" : "blit (its pass could not be created)");
    return TM.ok = ok;
}

// The edge-adaptive pipeline, made at the first frame that asks for it (a run that never does, with the setting at
// bilinear or DLSS on, never builds it): the present's EASU shader with specialization constant 0 (SQUARE) set, on
// this file's layout, whose two bindings are that shader's. VK_NULL_HANDLE when it cannot be made; the linear pass
// then stays.
static VkPipeline tm_easu(void) {
    if (TM.easu_tried) return TM.easu;
    TM.easu_tried = 1;
    VkCtx *c = vk_ctx();
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof fsr_easu_spv,
                                   .pCode = fsr_easu_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    static const VkBool32 square = VK_TRUE;
    VkSpecializationMapEntry me = {.constantID = 0, .offset = 0, .size = sizeof square};
    VkSpecializationInfo sp = {.mapEntryCount = 1, .pMapEntries = &me, .dataSize = sizeof square, .pData = &square};
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) == VK_SUCCESS) {
        VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                    .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main",
                                                    .pSpecializationInfo = &sp},
                                          .layout = TM.layout};
        if (vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &TM.easu) != VK_SUCCESS)
            TM.easu = VK_NULL_HANDLE;
        vkDestroyShaderModule(c->device, m, NULL);
    }
    if (!TM.easu) rt_log("vk_exec: upscaler stand-in: its edge-adaptive pass could not be created; it scales bilinearly");
    return TM.easu;
}

// One log line when the filter the pass scales with changes (the setting, DLSS or FSR switched on or off, a scene as
// large as the picture), 8 at most: a scene whose size changes from frame to frame can cross the picture's size often.
static void tm_filter_log(int easu, int edge, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh) {
    int same = sw == dw && sh == dh, now = easu ? 2 : same && edge ? 3 : 1;
    if (TM.filter_last == now || TM.filter_logs >= 8) return;
    TM.filter_last = now, TM.filter_logs++;
    if (easu)
        rt_log("vk_exec: upscaler stand-in scales %ux%u to %ux%u with the edge-adaptive filter "
               "(NX2NAVI_STANDIN_FILTER=bilinear: the linear one)", sw, sh, dw, dh);
    else if (now == 3)
        rt_log("vk_exec: upscaler stand-in copies %ux%u 1:1, decoded and not scaled: the scene is the picture's size", sw,
               sh);
    else
        rt_log("vk_exec: upscaler stand-in scales %ux%u to %ux%u with the linear filter: %s", sw, sh, dw, dh,
               !edge ? "DLSS or FSR scales the scene and its picture goes over this one"
               : dw < sw || dh < sh ? "nothing to enlarge"
               : strcmp(cfg_value("NX2NAVI_STANDIN_FILTER"), "easu") ? "NX2NAVI_STANDIN_FILTER asks for it"
                                                                     : "the edge-adaptive pass is not there");
}

// Views of the images the pass samples, kept as long as the image: vk_tonemap_forget drops those of an image
// fallback_scratch frees, and those of a game image the pass read as it is when the game's texture goes
// (vk_exec_image_gone), which also gives their places back. Without the second, a game image's view stayed naming a
// destroyed image whose handle a new image could get. The texture's destroy runs on any thread, hence the lock.
static PlatMutex tm_view_lock = PLAT_MUTEX_INIT;

void vk_tonemap_forget(VkImage src) {
    plat_mutex_lock(&tm_view_lock);
    for (int i = 0; i < TM.nviews;)
        if (TM.views[i].img == src) vk_defer_view(TM.views[i].view), TM.views[i] = TM.views[--TM.nviews];
        else i++;
    plat_mutex_unlock(&tm_view_lock);
}

static VkImageView tm_view(VkImage img, VkFormat f) {
    plat_mutex_lock(&tm_view_lock);
    VkImageView v = VK_NULL_HANDLE;
    for (int i = 0; i < TM.nviews && !v; i++)
        if (TM.views[i].img == img && TM.views[i].f == f) v = TM.views[i].view;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = f, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!v && TM.nviews < 16 && vkCreateImageView(vk_ctx()->device, &vi, NULL, &v) == VK_SUCCESS)
        TM.views[TM.nviews].img = img, TM.views[TM.nviews].f = f, TM.views[TM.nviews].view = v, TM.nviews++;
    plat_mutex_unlock(&tm_view_lock);
    return v;
}

// Records the pass from src (sampled, sw x sh) into the pass's own RGBA16F image of dw x dh and returns it (the caller
// blits it into the target); VK_NULL_HANDLE when it cannot (the caller blits the source instead). edge: the picture is
// what the screen shows, so the edge-adaptive filter may scale it (0: another upscaler's picture replaces it).
VkImage vk_tonemap_record(VkCommandBuffer cb, VkImage src, VkFormat sf, uint32_t sw, uint32_t sh, uint32_t dw,
                          uint32_t dh, int edge) {
    static int disp = -1;   // NX2NAVI_TM_DISPATCH=0: everything but the dispatch, =2: not at all (diagnosis)
    if (disp < 0) disp = rt_knob("TM_DISPATCH", "1");
    if (disp == 2 || !tm_init() || !tm_out(cb, dw, dh)) return VK_NULL_HANDLE;
    VkImageView sv = tm_view(src, sf), dv = TM.out_view;
    if (!sv) return VK_NULL_HANDLE;
    // NX2NAVI_STANDIN_FILTER, read each frame (the setting is live). The edge-adaptive filter only enlarges: a target
    // smaller than the source in either direction, or of its size, keeps the linear pass.
    int want = edge && dw >= sw && dh >= sh && (dw > sw || dh > sh) && !strcmp(cfg_value("NX2NAVI_STANDIN_FILTER"), "easu");
    VkPipeline easu = want ? tm_easu() : VK_NULL_HANDLE;
    tm_filter_log(easu != VK_NULL_HANDLE, edge, sw, sh, dw, dh);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT};
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &dep);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, easu ? easu : TM.pipe);
    // the same size both ways: each output texel takes its own source texel (see the top of the file)
    VkSampler smp = !easu && dw == sw && dh == sh ? TM.nearest : TM.sampler;
    VkDescriptorImageInfo ii[2] = {{smp, sv, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, dv, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[0]},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ii[1]}};
    rec_push(TM.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, TM.layout, 0, 2, w);   // through the recorder (vk_rec.h), in order
    if (easu) {
        // shaders/fsr_easu.comp's constants: the part of the source that is the picture and the source's size (the
        // whole image both times, as the linear pass maps it), then the output's size and the picture's corner in the
        // source. Each of its groups writes a 16x16 tile.
        const float pc[8] = {(float)sw, (float)sh, (float)sw, (float)sh, (float)dw, (float)dh, 0, 0};
        vkCmdPushConstants(cb, TM.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
        if (disp) vkCmdDispatch(cb, (dw + 15) / 16, (dh + 15) / 16, 1);
    } else {
        struct { float inv[2], unused, pad; int32_t size[2]; } pc = {{1.0f / (float)dw, 1.0f / (float)dh}, 0, 0,
                                                                    {(int32_t)dw, (int32_t)dh}};
        vkCmdPushConstants(cb, TM.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        if (disp) vkCmdDispatch(cb, (dw + 7) / 8, (dh + 7) / 8, 1);
    }
    VkMemoryBarrier2 after = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                              .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
    VkDependencyInfo dep2 = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &after};
    vkCmdPipelineBarrier2(cb, &dep2);
    return TM.out;
}

