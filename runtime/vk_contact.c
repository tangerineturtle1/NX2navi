// Screen-space contact shadows (NX2NAVI_CONTACT_SHADOWS, percent strength, 0 = off). NX2navi fills a full-resolution
// G-buffer whose targets include a B10G11R11 scene colour, an R32F linear depth (view distance / far) and a D32F depth
// (is_gbuffer), lights it in half resolution, and then composites the lighting into that scene colour in one pass
// whose only target is the scene colour, with a D16 depth that sorts pixels into material classes (is_composite).
// When the pass after the composite begins, or a dispatch or copy comes first, runtime/shaders/contact_shadow.comp
// marches each pixel a short way toward the sun through the linear depth and darkens the scene colour where the ray
// passes behind nearby geometry: the small shadows at feet, under stones and in grass that the shadow maps are too
// coarse for. Transparent surfaces, the sky and post-processing come after, untouched.
//
// Inputs from the game, read from the fragment stage's uniform block c4 bound in the composite pass (guest memory,
// so it may be one frame newer than the replay): the projection scales c4[7].x and c4[8].y, the far plane c4[14].y,
// and the first sun cascade's depth row c4[44].xyz, whose negated direction points from a view-space position toward
// the sun (the G-buffer shaders compare dot(c4[44].xyz, pos) + c4[44].w with the cascade map). The scene colour images
// get STORAGE usage when the setting is on (vk_texture.c create_tex).
//
// Ocarina draws forward and has no such G-buffer. Its frame: a depth prepass of the scene, the linear depth made from
// it (an R32F target of the scene's size, alone in its pass; view distance / far), the shadow maps, the lit scene into
// the B10G11R11 scene colour with a depth image of its own, then grass and transparent surfaces with the prepass's
// depth again. There the march runs when the lit scene's passes are over, at the start of the first pass that is not
// one of them (forward_pass_begin): a dispatch or a copy in the middle of the lit scene only suspends its pass, and
// what is drawn after it would cover the march. Its inputs come from two blocks seen at any pass's start: the camera
// block (vertex stage, block 0, 0x3500 bytes: the view matrix in rows at +0, the projection at +0x70, the far distance
// at +0xe4) and the sun block (fragment stage, block 4, 0x200 bytes: at +0 the direction the sun's light travels, in
// world space).
#include "vk.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "contact_shadow_spv.h"
#include "host.h"
#include "vk_dlaa.h"
#include "vk_exec.h"

static struct {
    int tried, ok, strength, debug, logged;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkSampler sampler;
    const VkTexImage *scene, *depth;   // the last G-buffer's scene colour and linear depth
    uint32_t w, h;
    int pending;                       // the composite into scene ran; the march waits for the next pass
    uint64_t c4;
    // Ocarina (forward): the linear depth target seen last and whether the lit scene has followed it; the prepass's
    // depth and colour images and the lit scene's depth; the camera and sun blocks
    const VkTexImage *lin, *pre_depth, *pre_color, *lit_depth;
    uint32_t lin_w, lin_h;
    int lin_new, forward;
    uint64_t cam, sun;
} C;

int vk_contact_strength(void) {
    if (!C.tried) {
        int s = cfg_int("NX2NAVI_CONTACT_SHADOWS");
        C.strength = s < 0 ? 0 : s > 100 ? 100 : s;
    }
    return C.strength;
}

static int init(VkCtx *c) {
    C.tried = 1;
    vk_contact_strength();
    if (!C.strength) return 0;
    const char *e = cfg_get("NX2NAVI_CONTACT_DEBUG");   // 1: the frame shows the contact shadow factor in grey
    C.debug = e && *e ? atoi(e) : 0;   // 2: the linear depth, 50 m to white
    C.push = (PFN_vkCmdPushDescriptorSetKHR)vk_dpa(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 2, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 64};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &C.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = sizeof contact_shadow_spv, .pCode = contact_shadow_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (!C.push || vkCreateDescriptorSetLayout(c->device, &dci, NULL, &C.dsl) != VK_SUCCESS ||
        vkCreatePipelineLayout(c->device, &li, NULL, &C.layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &C.sampler) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) {
        rt_log("vk: contact shadows off: their pipeline could not be created");
        return 0;
    }
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = C.layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &C.pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) {
        rt_log("vk: contact shadows off: their pipeline could not be created");
        return 0;
    }
    rt_log("vk: contact shadows on, strength %d%%%s", C.strength, C.debug ? " (debug view)" : "");
    return C.ok = 1;
}

// A G-buffer pass: a B10G11R11 scene colour, an R32F linear depth and a D32F depth, all one size, at least 640 wide.
static int is_gbuffer(const VkExec *ex, const VkTexImage **scene, const VkTexImage **depth) {
    *scene = *depth = NULL;
    if (ex->pass_noatt || ex->pass_w < 640 || ex->depth.format != VK_FORMAT_D32_SFLOAT || ex->depth.level) return 0;
    for (uint32_t i = 0; i < ex->pass_ncolor && i < 8; i++) {
        const VkExecTarget *t = &ex->color[i];
        if (!t->image || t->level || t->width != ex->pass_w || t->height != ex->pass_h) continue;
        if (t->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && !*scene) *scene = t->img;
        if (t->format == VK_FORMAT_R32_SFLOAT && !*depth) *depth = t->img;
    }
    return *scene && *depth && ((*scene)->usage & VK_IMAGE_USAGE_STORAGE_BIT);
}

static void mem_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &d);
}

int vk_contact_pending(void) { return C.pending && !C.forward; }   // Ocarina's waits for a pass's start instead

// The march, recorded where the caller is in the stream.
static void march(VkExec *ex) {
    C.pending = 0;
    if (ex->in_pass) return;
    float lx, ly, lz, px, py, zfar;
    if (C.forward) {
        // the sun's direction turned into view space by the view matrix's rows; the linear depth is a fraction of the
        // camera's far distance
        if (!C.cam || !C.sun) return;
        const float *m = (const float *)(uintptr_t)C.cam, *s = (const float *)(uintptr_t)C.sun;
        lx = -(m[0] * s[0] + m[1] * s[1] + m[2] * s[2]);
        ly = -(m[4] * s[0] + m[5] * s[1] + m[6] * s[2]);
        lz = -(m[8] * s[0] + m[9] * s[1] + m[10] * s[2]);
        px = m[28], py = m[33], zfar = m[57];
    } else {
        const float *u = (const float *)(uintptr_t)C.c4;
        lx = -u[44 * 4 + 0], ly = -u[44 * 4 + 1], lz = -u[44 * 4 + 2];
        px = u[7 * 4 + 0], py = u[8 * 4 + 1], zfar = u[14 * 4 + 1];
        if (!(zfar > 1)) return;
    }
    float ll = sqrtf(lx * lx + ly * ly + lz * lz);
    if (!(ll > 1e-6f) || !(px > 0) || !(py > 0) || !(zfar > 0)) return;
    VkImageView in = vk_exec_target_view(C.depth, VK_FORMAT_R32_SFLOAT, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
                                         0, 0, 1, 0);
    VkImageView out = vk_exec_target_view(C.scene, VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_IMAGE_VIEW_TYPE_2D,
                                          VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, 0);
    if (!in || !out) return;
    VkCommandBuffer cb = ex->cb;
    float pcv[16] = {px, py, zfar, (float)C.strength / 100.0f,
                     lx / ll, ly / ll, lz / ll, 0.6f,
                     (float)C.w, (float)C.h, 1.0f / (float)C.w, 1.0f / (float)C.h,
                     (float)C.debug, vk_dlaa_available() ? (float)(__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) & 63) : 0,
                     0, 0};
    if (!C.logged) {
        C.logged = 1;
        rt_log("vk: contact shadows on a %ux%u scene, sun toward %.3f %.3f %.3f (view space), far %g", C.w, C.h,
               pcv[4], pcv[5], pcv[6], zfar);
    }
    mem_barrier(cb);   // the compose passes' writes, before the march reads and writes them
    VkDescriptorImageInfo ii = {C.sampler, in, VK_IMAGE_LAYOUT_GENERAL}, oi = {VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, C.pipe);
    rec_push(C.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, C.layout, 0, 2, w);
    vkCmdPushConstants(cb, C.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pcv, pcv);
    vkCmdDispatch(cb, (C.w + 7) / 8, (C.h + 7) / 8, 1);
    mem_barrier(cb);   // and what follows sees the darkened colour
    ex->recorded += 6;
    vk_exec_draw_new_cb();   // the game's bound compute pipeline and descriptors were replaced: bind them again
}

void vk_contact_flush(VkExec *ex) {
    if (C.forward) return;   // Ocarina: only another pass's start ends the lit scene (forward_pass_begin)
    // The submitting thread past a group (vk_exec.c NX2NAVI_REPLAY_PIPE): the group's first chunk may have made the
    // pass due and a later one records it where the replay would; only one still due after the group is this one's.
    if (vk_exec_behind()) {
        vk_exec_hooks_wait();
        if (!C.pending) return;
        vk_exec_turn();
    }
    if (!C.pending || C.forward) return;
    march(ex);
}

// The lighting composite: the remembered scene colour as the only target, with a D16 depth of its size.
static int is_composite(const VkExec *ex) {
    return C.scene && ex->pass_ncolor >= 1 && ex->color[0].img == C.scene && !ex->color[0].level &&
           ex->depth.format == VK_FORMAT_D16_UNORM && ex->pass_w == C.w && ex->pass_h == C.h;
}

// Ocarina's frame at a pass's start (the header's second part). The pass hooks run one after another in the order of
// the stream (vk_exec.c vk_exec_pass_begin), and what is kept here is read only by them, except C.forward and
// C.pending, which vk_contact_flush reads from any chunk: C.forward is set once, in turn, and while it is set the
// flush does nothing whatever C.pending says.
static void forward_pass_begin(VkExec *ex) {
    const p_bind_buf *cb = &ex->ubo[0][0], *sb = &ex->ubo[1][4];
    if (cb->addr && cb->size == 0x3500) {
        const float *m = (const float *)(uintptr_t)cb->addr;
        if (m[28] > 0 && m[42] == -1.0f && m[43] == 0) C.cam = cb->addr;   // a perspective projection: the camera's
    }
    if (sb->addr && sb->size == 0x200) C.sun = sb->addr;
    if (!ex->pass_noatt && ex->pass_ncolor == 1 && !ex->depth.image && ex->pass_w >= 640 && !ex->color[0].level &&
        ex->color[0].format == VK_FORMAT_R32_SFLOAT && ex->color[0].img) {
        C.lin = ex->color[0].img, C.lin_w = ex->pass_w, C.lin_h = ex->pass_h, C.lin_new = 1, C.pending = 0;
        return;
    }
    int scene_like = !ex->pass_noatt && ex->pass_ncolor == 1 && ex->depth.format == VK_FORMAT_D32_SFLOAT && ex->depth.img &&
                     !ex->color[0].level && ex->color[0].img && ex->color[0].format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 &&
                     (ex->color[0].img->usage & VK_IMAGE_USAGE_STORAGE_BIT);
    if (C.lin_new) {
        // the lit scene: the prepass's colour image with another depth image
        int lit = scene_like && ex->pass_w == C.lin_w && ex->pass_h == C.lin_h && ex->color[0].img == C.pre_color &&
                  ex->depth.img != C.pre_depth;
        if (!C.pending) {
            if (lit) {
                if (!C.forward) vk_exec_hook_turn();
                C.scene = ex->color[0].img, C.lit_depth = ex->depth.img, C.depth = C.lin, C.w = ex->pass_w, C.h = ex->pass_h;
                C.forward = 1, C.pending = 1;
            }
            return;
        }
        if (lit && ex->color[0].img == C.scene && ex->depth.img == C.lit_depth) return;   // the lit scene goes on
        vk_exec_hook_turn();   // the march is recorded after everything drawn before this pass
        march(ex);
        C.lin_new = 0;
    }
    if (scene_like) C.pre_depth = ex->depth.img, C.pre_color = ex->color[0].img;
}

void vk_contact_pass_begin(VkExec *ex) {
    if (!C.tried && !init(vk_ctx())) return;
    if (!C.ok) return;
    const VkTexImage *scene, *depth;
    // The camera block: c4 of the fragment stage, 0x940 bytes. The composite binds it only after its pass begins, so
    // the newest one seen at any pass start serves.
    const p_bind_buf *b = &ex->ubo[1][1];
    if (b->addr && b->size == 0x940 && C.c4 != b->addr) vk_exec_hook_turn(), C.c4 = b->addr;
    if (RT_V111) {   // Ocarina draws forward
        forward_pass_begin(ex);
        return;
    }
    if (is_gbuffer(ex, &scene, &depth)) {
        vk_exec_hook_turn();
        C.pending = 0;
        C.scene = scene, C.depth = depth, C.w = ex->pass_w, C.h = ex->pass_h;
        return;
    }
    if (is_composite(ex)) {
        vk_exec_hook_turn();
        C.pending = C.c4 != 0;
        return;
    }
    vk_contact_flush(ex);
}

void vk_contact_release(void) {
    VkCtx *c = vk_ctx();
    if (!c || !C.tried) return;
    if (C.pipe) vkDestroyPipeline(c->device, C.pipe, NULL);
    if (C.sampler) vkDestroySampler(c->device, C.sampler, NULL);
    if (C.layout) vkDestroyPipelineLayout(c->device, C.layout, NULL);
    if (C.dsl) vkDestroyDescriptorSetLayout(c->device, C.dsl, NULL);
    memset(&C, 0, sizeof C);
}
