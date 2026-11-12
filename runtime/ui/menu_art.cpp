// The settings menu's art from the game's own interface textures (menu_assets.c reads them from the user's RomFS):
// the frame's corner ornaments, the control guide's bar, and the layers of the buttons, cursor, panels and divider that
// ui/menu_skin.cpp puts together. A worker decodes them into RGBA once at start (the archive unpacks to about 240 MB and
// takes a moment); the window thread uploads each to the GPU with its smaller levels the first time the menu asks for
// it, and registers it with Dear ImGui's Vulkan back end. Without the files the menu draws its plain shapes.
#include <atomic>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

extern "C" {
#include "host.h"
#include "vk.h"
#include "platform/platform.h"
}
#include "menu_assets.h"
#include "ui/menu_art.h"
#include "ui/menu_skin.h"

#define COMMON_BNTX "UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.bntx.zs"

// in the order of MenuArt (tools/menu_preview.py reads this list too)
static const char *const kNames[MENU_ART_COUNT] = {
    "LocationDeco_00", "CtrlGuideBG_00", "MessageWindowCommonBase_00",
    "BtnThinBase_01", "BtnThinBase_00", "BtnThinBase_05", "BtnThinBase_04", "BtnThinBase_02",
    "OptionBtnThinCursorGlow_00", "OptionBtnThinCursorLine_00", "SystemCaptionBg_00",
    "OptionSeparateMark_00", "SystemLine_03", "MenuHeaderSelect_00",
};
// Levels uploaded for each art: the buttons draw it at about half size and smaller, where a line one or two texels wide
// would break up if sampled from the full image alone.
enum { MAX_LEVELS = 6 };

static struct Art {
    uint8_t *rgba;
    uint32_t w, h;
    VkImage img;
    VmaAllocation alloc;
    VkImageView view;
    VkDescriptorSet ds;
    int failed;
} g_art[MENU_ART_COUNT];
static std::atomic<int> g_state;   // 0 not started, 1 decoding, 2 decoded, 3 nothing to show
static VkSampler g_sampler;

static void decode_all(void *) {
    size_t n = 0;
    uint8_t *bntx = menu_asset_load(COMMON_BNTX, &n);
    int got = 0;
    if (!bntx) rt_log("menu: %s could not be read from the RomFS", COMMON_BNTX);
    for (int i = 0; bntx && i < MENU_ART_COUNT; i++) {
        Art &a = g_art[i];
        if (menu_asset_texture(bntx, n, kNames[i], &a.rgba, &a.w, &a.h)) got++, skin_art_prepare(i, a.rgba, a.w, a.h);
        else rt_log("menu: interface texture %s not decoded (archive of %llu bytes)", kNames[i], (unsigned long long)n);
    }
    free(bntx);
    rt_log("menu: %d of %d interface textures from the RomFS", got, (int)MENU_ART_COUNT);
    g_state.store(got ? 2 : 3, std::memory_order_release);
}

extern "C" void menu_art_start(void) {
    int expect = 0;
    if (!g_state.compare_exchange_strong(expect, 1)) return;
    if (!plat_thread_spawn(decode_all, nullptr, "menu-art", 0)) decode_all(nullptr);
}

// Frees what upload_rgba made for a (none of it in use: the caller waited for the queue or never drew with it).
static void art_free(Art &a) {
    VkCtx *c = vk_ctx();
    if (a.ds) ImGui_ImplVulkan_RemoveTexture(a.ds);
    if (c && a.view) vkDestroyImageView(c->device, a.view, nullptr);
    if (c && a.img) vmaDestroyImage(c->vma, a.img, a.alloc);
    a.ds = VK_NULL_HANDLE, a.view = VK_NULL_HANDLE, a.img = VK_NULL_HANDLE, a.alloc = nullptr;
}

// rgba (w x h) as a sampled image of up to max_levels levels, each halved from the one before, with its view and Dear
// ImGui descriptor set in a; 1 when all of it is there, else 0 with nothing left allocated.
static int upload_rgba(Art &a, const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t max_levels) {
    VkCtx *c = vk_ctx();
    if (!c || !rgba) return 0;
    if (!g_sampler) {
        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = MAX_LEVELS;
        if (vkCreateSampler(c->device, &si, nullptr, &g_sampler) != VK_SUCCESS) return 0;
    }
    uint32_t lw[MAX_LEVELS] = {w}, lh[MAX_LEVELS] = {h}, levels = 1;
    VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    while (levels < max_levels && levels < MAX_LEVELS && (lw[levels - 1] > 1 || lh[levels - 1] > 1)) {
        lw[levels] = lw[levels - 1] > 1 ? lw[levels - 1] / 2 : 1;
        lh[levels] = lh[levels - 1] > 1 ? lh[levels - 1] / 2 : 1;
        bytes += (VkDeviceSize)lw[levels] * lh[levels] * 4;
        levels++;
    }
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {w, h, 1};
    ii.mipLevels = levels;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(c->vma, &ii, &ai, &a.img, &a.alloc, nullptr) != VK_SUCCESS) {
        a.img = VK_NULL_HANDLE, a.alloc = nullptr;
        return 0;
    }
    VkBuffer sb;
    VmaAllocation sa;
    uint8_t *map = (uint8_t *)vk_staging(bytes, &sb, &sa);
    if (!map) return art_free(a), 0;
    // the levels one after the other in a plain allocation, each halved from the one before (the staging memory may be
    // slow to read back)
    uint8_t *chain = (uint8_t *)malloc((size_t)bytes);
    if (!chain) {
        vmaDestroyBuffer(c->vma, sb, sa);
        return art_free(a), 0;
    }
    memcpy(chain, rgba, (size_t)w * h * 4);
    VkBufferImageCopy regions[MAX_LEVELS] = {};
    VkDeviceSize at = 0;
    for (uint32_t l = 0; l < levels; l++) {
        regions[l].bufferOffset = at;
        regions[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
        regions[l].imageExtent = {lw[l], lh[l], 1};
        if (l) skin_art_halve(chain + regions[l - 1].bufferOffset, lw[l - 1], lh[l - 1], chain + at, lw[l], lh[l]);
        at += (VkDeviceSize)lw[l] * lh[l] * 4;
    }
    memcpy(map, chain, (size_t)bytes);
    free(chain);
    // The batch flushes its own staging buffers. This one is caller-owned and can be noncoherent.
    if (vmaFlushAllocation(c->vma, sa, 0, bytes) != VK_SUCCESS) {
        vmaDestroyBuffer(c->vma, sb, sa);
        return art_free(a), 0;
    }
    VkCommandBuffer cb = vk_upload_begin();
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = a.img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
    vkCmdCopyBufferToImage(cb, sb, a.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, regions);
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cb, &di);
    vk_upload_end();
    vk_upload_flush(1);
    vmaDestroyBuffer(c->vma, sb, sa);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = a.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    if (vkCreateImageView(c->device, &vi, nullptr, &a.view) != VK_SUCCESS) {
        a.view = VK_NULL_HANDLE;
        return art_free(a), 0;
    }
    int errs = menu_vk_errors();
    a.ds = ImGui_ImplVulkan_AddTexture(g_sampler, a.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (menu_vk_errors() != errs) a.ds = VK_NULL_HANDLE;   // the descriptor set was not allocated (menu_vk_check logged it)
    if (!a.ds) return art_free(a), 0;
    return 1;
}

static int upload(Art &a) {
    return upload_rgba(a, a.rgba, a.w, a.h, MAX_LEVELS);   // the pixels stay (a few MB): the back end may start again
}

// The menu's font atlas (ui/menu.cpp fonts_up), uploaded as the art is: through the runtime's upload batch, which
// submits under the queue lock, into memory VMA picks (device-local, else what is left), every step checked. Dear
// ImGui's own upload (ImGui_ImplVulkan_CreateFontsTexture) checked no result, so with the video memory full its copy
// went to an image without memory and the GPU wrote to address 0 (issue #165); and run from ImGui_ImplVulkan_NewFrame,
// its submit and queue wait took no lock and could meet the game's submissions on the same queue. The previous atlas
// goes once the new one is on the GPU and the queue has finished the frames that drew with it.
static Art g_font;

extern "C" int menu_font_upload(const unsigned char *rgba, int w, int h, ImTextureID *tex) {
    VkCtx *c = vk_ctx();
    if (!c || !rgba || w <= 0 || h <= 0) return 0;
    Art n = {};
    if (!upload_rgba(n, rgba, (uint32_t)w, (uint32_t)h, 1)) {
        rt_log("menu: the %dx%d font atlas could not be put on the GPU", w, h);
        return 0;
    }
    if (g_font.ds) {
        vk_queue_lock();
        vkQueueWaitIdle(c->gfx_queue);
        vk_queue_unlock();
        art_free(g_font);
    }
    g_font = n;
    *tex = (ImTextureID)n.ds;
    return 1;
}

extern "C" int menu_art(MenuArt id, ImTextureID *tex, float *w, float *h) {
    if ((unsigned)id >= MENU_ART_COUNT || g_state.load(std::memory_order_acquire) != 2) return 0;
    Art &a = g_art[id];
    if (!a.ds && !a.failed && !upload(a)) a.failed = 1;
    if (!a.ds) return 0;
    *tex = (ImTextureID)a.ds;
    if (w) *w = (float)a.w;
    if (h) *h = (float)a.h;
    return 1;
}

extern "C" void menu_art_release(void) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    for (Art &a : g_art) {
        art_free(a);
        a.failed = 0;   // uploaded again at the next use
    }
    art_free(g_font);   // the menu makes its atlas again after the back end starts again (fonts_up)
    ImGui::GetIO().Fonts->SetTexID(0);
    if (g_sampler) vkDestroySampler(c->device, g_sampler, nullptr), g_sampler = VK_NULL_HANDLE;
}
