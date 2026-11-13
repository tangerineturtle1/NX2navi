// AMD FidelityFX through its API library (amd_fidelityfx_vk.dll next to the executable, loaded at run time): FSR 3.1
// temporal upscaling here, in the place of DLSS in vk_dlaa.c, and frame generation (vk_ffx_fg_*, below). The API's
// headers are in third_party/ffx_api (MIT); they are C++ in places, hence this file. Nothing of it runs unless the
// upscaler or frame generation asks for FSR 3.1.
//
// FSR 3.1 takes the same inputs vk_dlaa.c makes for DLSS: the scene's light target (linear HDR), its D32F depth (0
// near, 1 far, with the near and far planes of the frame's own projection, vk_dlaa.c proj_planes) and the camera
// motion vectors in render pixels from the current frame to the previous one, the convention both share, so the
// vectors go in unscaled.
#ifdef _WIN32
#include <windows.h>
#endif
#include <atomic>
#include <chrono>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "third_party/ffx_api/ffx_api.h"
#include "third_party/ffx_api/ffx_upscale.h"
#include "third_party/ffx_api/vk/ffx_api_vk.h"

extern "C" {
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_ffx.h"
#include "vk_fg.h"
}

namespace {

struct Api {
    void *lib = nullptr;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    int tried = 0, ok = 0;
} A;

// What the settings menu may ask before anything has used FSR 3.1 (vk_ffx_available), from any thread.
std::atomic<int> g_loaded{-1};    // what vk_ffx_load found: 1 or 0; -1 while it has not run
std::atomic<int> g_present{-1};   // whether the library file is there: 1 or 0; -1 while not asked

#ifdef _WIN32
// amd_fidelityfx_vk.dll next to the executable
void library_path(char *path, size_t size) {
    DWORD n = GetModuleFileNameA(NULL, path, (DWORD)size);
    while (n && path[n - 1] != '\\') n--;
    snprintf(path + n, size - n, "amd_fidelityfx_vk.dll");
}
#endif

void message(uint32_t type, const wchar_t *msg) {
    char s[512];
    size_t n = 0;
    for (; msg && msg[n] && n < sizeof s - 1; n++) s[n] = msg[n] < 128 ? (char)msg[n] : '?';
    s[n] = 0;
    static int told;
    if (told++ < 40) rt_log("ffx: %s %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error:" : "warning:", s);
}

// The library's pipeline creates (vk_ffx_proc_addr). It passes no pipeline cache, so one that names none goes through
// the runtime's (vk_pipeline.c vk_pipeline_create_foreign); one that names a cache is the library's own business.
VKAPI_ATTR VkResult VKAPI_CALL compute_pipelines(VkDevice dev, VkPipelineCache cache, uint32_t n,
                                                 const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a,
                                                 VkPipeline *out) {
    if (cache) return vkCreateComputePipelines(dev, cache, n, ci, a, out);
    return vk_pipeline_create_foreign(dev, 1, n, ci, a, out);
}

VKAPI_ATTR VkResult VKAPI_CALL graphics_pipelines(VkDevice dev, VkPipelineCache cache, uint32_t n,
                                                  const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a,
                                                  VkPipeline *out) {
    if (cache) return vkCreateGraphicsPipelines(dev, cache, n, ci, a, out);
    return vk_pipeline_create_foreign(dev, 0, n, ci, a, out);
}

int pipe_cache_on(void) {   // NX2NAVI_FFX_PIPE_CACHE, read once
    static std::atomic<int> on{-1};
    int v = on.load(std::memory_order_relaxed);
    if (v < 0) on.store(v = cfg_bool("NX2NAVI_FFX_PIPE_CACHE"), std::memory_order_relaxed);
    return v;
}

// ffxCreateContext with what it made in pipelines: the library compiles every pass of a context inside this call, on
// the thread that asked, so the line says what a context cost and where its pipelines came from.
ffxReturnCode_t create_counted(ffxContext *ctx, ffxCreateContextDescHeader *desc) {
    VkForeignPipes p0, p1;
    vk_pipeline_foreign_counts(&p0);
    auto t0 = std::chrono::steady_clock::now();
    ffxReturnCode_t r = A.create(ctx, desc, nullptr);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    vk_pipeline_foreign_counts(&p1);
    unsigned long long made = p1.made - p0.made, hit = p1.from_cache - p0.from_cache, comp = p1.compiled - p0.compiled,
                       none = p1.uncached - p0.uncached;
    if (r != FFX_API_RETURN_OK) return r;
    if (!pipe_cache_on())
        rt_log("ffx: context created in %.0f ms; its pipelines are made without the runtime's pipeline caches "
               "(NX2NAVI_FFX_PIPE_CACHE=0)", ms);
    else if (!made)
        rt_log("ffx: context created in %.0f ms; it made no pipeline through the runtime's creates", ms);
    else
        rt_log("ffx: context created in %.0f ms; its %llu pipelines: %llu from the pipeline caches without a compile, "
               "%llu compiled and kept in the play cache for the next start, %llu through the one pipeline cache (what "
               "it held is not told apart), %llu with no pipeline cache (none is up yet, or pipeline binaries are on)",
               ms, made, hit, comp, made - hit - comp - none, none);
    return r;
}

// One upscaling context per render size and output size, as AMD's own sample keeps them: it waits for the GPU and makes
// the context anew whenever the render resolution changes, and sets FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION only for its
// dynamic resolution mode. One context per output size that every mode of the menu ran on, with another render size at
// each dispatch and without that flag, preceded an access violation in AMD's driver inside FSR 4's dispatch at the
// first frame after a switch from 67 % to 100 % (vk_ffx4.cpp keeps its contexts the same way). With the game's dynamic
// resolution on (NX2NAVI_DYN_RES, dynres.c), whose steps change the render size every few seconds, one context with
// that flag takes them all. A context of another render size goes once no command buffer can still hold it
// (UP_RECENT), so a switch keeps at most two of an output size for a few frames, and the game's second scenes, of
// another output size, keep their own. When all slots are taken, the least recently used one is destroyed for the new
// size once the GPU is idle, as vk_dlaa.c free_size does for DLSS.
struct Up {
    ffxContext ctx = nullptr;
    uint32_t mw = 0, mh = 0, ow = 0, oh = 0;   // the render size it was made for (the largest it takes with drs); the output size
    bool drs = false;                          // made for dynamic resolution (FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION)
    uint64_t used = 0, used_ns = 0;            // up_tick and the time when context_for last returned it
    uint64_t vram = 0;                         // its video memory as the library reported it at its creation, 0 not told
} U[4];
uint64_t up_tick;
// A context used in the last few calls may sit in a command buffer that is not submitted yet, where waiting for the
// GPU does not cover it: a frame makes one or two calls (the scene, and a second scene after it), so this is a few
// frames back.
const uint64_t UP_RECENT = 8;
// A context no upscale used for this long goes (trim): FSR 3.1's after a switch to FSR 4, and a second scene's (the
// pause screen's) a while after it closed. Unused that long, no command buffer names it.
const uint64_t UP_IDLE_NS = 90ull * 1000000000ull;

void destroy_up(Up *u, const char *why) {
    rt_log("ffx: the FSR 3.1 context for %ux%u (render sizes up to %ux%u) destroyed: %s", u->ow, u->oh, u->mw, u->mh, why);
    A.destroy(&u->ctx, nullptr);
    *u = Up();
}

void trim(uint64_t now_ns) {
    for (Up &u : U)
        if (u.ctx && now_ns - u.used_ns > UP_IDLE_NS) destroy_up(&u, "no upscale used it for 90 s");
}

FfxApiResource image(const VkFfxImage *i, uint32_t state, uint32_t usage) {
    VkImageCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = i->format;
    ci.extent = {i->width, i->height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.usage = i->storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0;
    return ffxApiGetResourceVK((void *)i->image, ffxApiGetImageResourceDescriptionVK(i->image, ci, usage), state);
}

// NX2NAVI_FFX_KEYS=<key>:<value>[,...] (test): FSR 3.1's tuning constants for each new context (ffx_upscale.h
// FfxApiConfigureUpscaleKey: 0 velocity factor, 1 reactiveness scale, 2 shading change scale, 3 accumulation added per
// frame, 4 minimum disocclusion accumulation). The library reads the value through the pointer.
void configure_keys(ffxContext *ctx) {
    const char *e = cfg_get("NX2NAVI_FFX_KEYS");
    static float value[8];
    for (int n = 0; e && *e && n < 8; n++) {
        unsigned key = 0;
        float v = 0;
        if (sscanf(e, "%u:%f", &key, &v) != 2) break;
        value[n] = v;
        ffxConfigureDescUpscaleKeyValue d = {};
        d.header.type = FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE;
        d.key = key, d.ptr = &value[n];
        ffxReturnCode_t r = A.configure(ctx, &d.header);
        rt_log("ffx: FSR 3.1 key %u = %g (%s)", key, (double)v, r == FFX_API_RETURN_OK ? "set" : "refused");
        e = strchr(e, ',');
        if (e) e++;
    }
}

Up *context_for(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh) {
    up_tick++;
    uint64_t now = plat_now_ns();
    const bool drs = cfg_bool("NX2NAVI_DYN_RES");
    Up *found = nullptr;
    for (Up &u : U)
        if (u.ctx && u.ow == ow && u.oh == oh && u.drs == drs && (drs ? w <= u.mw && h <= u.mh : w == u.mw && h == u.mh))
            found = &u;
    const VkCtx *c = vk_ctx();
    int waited = 0;
    for (Up &u : U)   // the contexts of this output size for another render size, once no command buffer holds them
        if (u.ctx && &u != found && u.ow == ow && u.oh == oh && up_tick - u.used >= UP_RECENT) {
            if (!waited++) vk_queue_lock(), vkDeviceWaitIdle(c->device), vk_queue_unlock();   // their work may be in flight
            destroy_up(&u, "the render size changed");
        }
    if (found) {
        found->used = up_tick, found->used_ns = now;
        return found;
    }
    Up *slot = nullptr, *lru = nullptr;
    for (Up &u : U) {
        if (!u.ctx) {
            if (!slot) slot = &u;
            continue;
        }
        if (up_tick - u.used >= UP_RECENT && (!lru || u.used < lru->used)) lru = &u;
    }
    if (!slot && lru) {
        vk_queue_lock(), vkDeviceWaitIdle(c->device), vk_queue_unlock();   // its work may still be in flight
        destroy_up(lru, "its slot went to another output size");
        slot = lru;
    }
    if (!slot) return nullptr;
    // the render size itself; for dynamic resolution, every size up to the output's (and a scene at the output size)
    uint32_t mw = !drs ? w : w > ow ? w : ow, mh = !drs ? h : h > oh ? h : oh;
    ffxCreateBackendVKDesc be = {};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    be.vkDevice = c->device;
    be.vkPhysicalDevice = c->phys;
    be.vkDeviceProcAddr = vk_ffx_proc_addr;
    ffxCreateContextDescUpscale d = {};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    d.header.pNext = &be.header;
    // no infinite-depth flag: the game's far plane is finite (vk_dlaa.c proj_planes) and each dispatch gives it
    d.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    // NX2NAVI_FFX_DEBUG=1 (test): the library checks the values of each dispatch and says what it finds (message)
    if (rt_knob("FFX_DEBUG", "0")) d.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    if (drs) d.flags |= FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
    d.maxRenderSize = {mw, mh};
    d.maxUpscaleSize = {ow, oh};
    d.fpMessage = message;
    ffxReturnCode_t r = create_counted(&slot->ctx, &d.header);
    if (r != FFX_API_RETURN_OK) {
        rt_log("ffx: FSR 3.1 context creation for %ux%u (render sizes up to %ux%u) failed (%u)", ow, oh, mw, mh, (unsigned)r);
        slot->ctx = nullptr;
        return nullptr;
    }
    slot->mw = mw, slot->mh = mh, slot->ow = ow, slot->oh = oh, slot->drs = drs, slot->used = up_tick, slot->used_ns = now;
    configure_keys(&slot->ctx);
    ffxQueryGetProviderVersion v = {};
    v.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    A.query(&slot->ctx, &v.header);
    // its video memory, for the memory report (vk_ffx_vram); a provider that does not know the query leaves it 0
    FfxApiEffectMemoryUsage mem = {};
    ffxQueryDescUpscaleGetGPUMemoryUsage mq = {};
    mq.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE;
    mq.gpuMemoryUsageUpscaler = &mem;
    if (A.query(&slot->ctx, &mq.header) == FFX_API_RETURN_OK) slot->vram = mem.totalUsageInBytes;
    rt_log("ffx: FSR 3.1 upscaling to %ux%u from render sizes up to %ux%u (%s)", ow, oh, mw, mh,
           v.versionName ? v.versionName : "provider unknown");
    return slot;
}

}  // namespace

extern "C" int vk_ffx_load(void) {
    if (A.tried) return A.ok;
    A.tried = 1;
#ifdef _WIN32
    char path[MAX_PATH];
    library_path(path, sizeof path);
    HMODULE m = LoadLibraryA(path);
    if (!m) {
        rt_log("ffx: %s not found: FSR 3.1 off", path);
        g_loaded.store(0, std::memory_order_release);
        return 0;
    }
    A.lib = m;
    A.create = (PfnFfxCreateContext)(void *)GetProcAddress(m, "ffxCreateContext");
    A.destroy = (PfnFfxDestroyContext)(void *)GetProcAddress(m, "ffxDestroyContext");
    A.configure = (PfnFfxConfigure)(void *)GetProcAddress(m, "ffxConfigure");
    A.query = (PfnFfxQuery)(void *)GetProcAddress(m, "ffxQuery");
    A.dispatch = (PfnFfxDispatch)(void *)GetProcAddress(m, "ffxDispatch");
    A.ok = A.create && A.destroy && A.configure && A.query && A.dispatch;
    rt_log("ffx: amd_fidelityfx_vk.dll %s", A.ok ? "loaded" : "lacks the API entry points: FSR 3.1 off");
#else
    // Only the Windows build loads AMD's library: there is nothing to load here
    rt_log("ffx: this build has no FSR 3.1 (it needs amd_fidelityfx_vk.dll, which only the Windows build loads): FSR 3.1 off");
#endif
    g_loaded.store(A.ok, std::memory_order_release);
    return A.ok;
}

extern "C" int vk_ffx_available(void) {
    int loaded = g_loaded.load(std::memory_order_acquire);
    if (loaded >= 0) return loaded;
#ifdef _WIN32
    int present = g_present.load(std::memory_order_relaxed);
    if (present < 0) {   // the menu asks every frame: look once, and leave the loading to the first use
        char path[MAX_PATH];
        library_path(path, sizeof path);
        DWORD attr = GetFileAttributesA(path);
        present = attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
        g_present.store(present, std::memory_order_relaxed);
    }
    return present;
#else
    return 0;
#endif
}

// the same library, its API calls; and a queue for the frame generation swapchain's presents (vk_fg_has_queues)
extern "C" int vk_ffx_framegen_available(void) { return vk_ffx_available() && vk_fg_has_queues(); }

// NX2NAVI_FSR4 (default 0, live): FSR 4 where it runs (vk_ffx4.cpp) and has not failed in a row, else FSR 3.1.
extern "C" int vk_ffx_fsr4(void) { return cfg_bool("NX2NAVI_FSR4") && vk_ffx4_available() && !vk_ffx4_failed(); }

extern "C" int vk_ffx_trim_idle(uint64_t recent) {
    int n = 0;
    for (Up &u : U)
        if (u.ctx && up_tick - u.used >= recent) destroy_up(&u, "room for an FSR 4 context"), n++;
    return n;
}

extern "C" int vk_ffx_upscale(VkCommandBuffer cb, const VkFfxImage *color, const VkFfxImage *depth, const VkFfxImage *mv,
                              const VkFfxImage *out, float jx, float jy, int reset, float dt_ms, const VkFfxCamera *cam) {
    if (!vk_ffx_load()) return 0;
    uint64_t now = plat_now_ns();
    trim(now);
    vk_ffx4_trim(now);
    // Each upscaler keeps a history of its own: the frame after a change from one to the other starts it anew, or it
    // would blend this picture with whatever that upscaler last saw.
    static int last;   // 3 or 4: which one made the frame before
    if (vk_ffx_fsr4()) {
        if (vk_ffx4_upscale(cb, color, depth, mv, out, jx, jy, reset || last != 4, dt_ms, cam)) {
            if (last != 4) rt_log("ffx: the upscaler is %s", vk_ffx4_name());
            last = 4;
            return 1;
        }
        // it failed and said so: FSR 3.1 makes this frame
    }
    if (last != 3) {
        if (last) rt_log("ffx: the upscaler is AMD FSR 3.1");
        reset = 1, last = 3;
    }
    Up *u = context_for(color->width, color->height, out->width, out->height);
    if (!u) return 0;
    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cb;
    // Every image is in GENERAL, which the backend reads as its unordered-access state, and is left there
    d.color = image(color, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, 0);
    d.depth = image(depth, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, 0);
    d.motionVectors = image(mv, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, 0);
    d.output = image(out, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
    d.jitterOffset = {jx, jy};
    d.motionVectorScale = {1.0f, 1.0f};
    d.renderSize = {color->width, color->height};
    d.upscaleSize = {out->width, out->height};
    // The sharpness slider (0-100) drives FSR's own RCAS here, as it does for FSR 1. Below a render scale of three
    // quarters it goes towards the maximum, all the way from Performance down, where FSR 3.1 is soft (vk_dlaa.c
    // "FSR 3.1's second sharpening pass", which has the measurements); 0 still turns it off.
    int sharp = cfg_int("NX2NAVI_SHARPNESS");
    d.enableSharpening = sharp > 0;
    d.sharpness = sharp > 100 ? 1.0f : sharp / 100.0f;
    if (sharp > 0) d.sharpness += (1.0f - d.sharpness) * vk_dlaa_fsr3_boost(color->width, out->width);
    d.frameTimeDelta = dt_ms;
    d.preExposure = 1.0f;
    d.reset = reset != 0;
    // the frame's camera (vk_dlaa.c proj_planes)
    d.cameraNear = cam && cam->znear > 0 ? cam->znear : 0.1f;
    d.cameraFar = cam && cam->zfar > 0 ? cam->zfar : 1e8f;
    d.cameraFovAngleVertical = cam && cam->fov_y > 0 ? cam->fov_y : 1.0f;
    d.viewSpaceToMetersFactor = cam && cam->meters > 0 ? cam->meters : 1.0f;
    ffxReturnCode_t r = A.dispatch(&u->ctx, &d.header);
    if (r != FFX_API_RETURN_OK) {
        static int told;
        if (told++ < 8) rt_log("ffx: FSR 3.1 dispatch failed (%u)", (unsigned)r);
        return 0;
    }
    return 1;
}

extern "C" ffxReturnCode_t vk_ffx_create(ffxContext *ctx, ffxCreateContextDescHeader *desc) {
    return vk_ffx_load() ? create_counted(ctx, desc) : FFX_API_RETURN_ERROR;
}

// What the library is handed as ffxCreateBackendVKDesc.vkDeviceProcAddr: the device's functions, with the two
// pipeline creates replaced by ours (compute_pipelines, graphics_pipelines) unless NX2NAVI_FFX_PIPE_CACHE is 0.
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_ffx_proc_addr(VkDevice dev, const char *name) {
    if (name && pipe_cache_on()) {
        if (!strcmp(name, "vkCreateComputePipelines")) return (PFN_vkVoidFunction)compute_pipelines;
        if (!strcmp(name, "vkCreateGraphicsPipelines")) return (PFN_vkVoidFunction)graphics_pipelines;
    }
    return vkGetDeviceProcAddr(dev, name);
}

// --probe-ffx-cache (host.h): one FSR 3.1 upscaling context on the device, without a window or the game, and where
// its pipelines came from. The pipeline caches are those beside the executable, or NX2NAVI_PIPELINE_CACHE's, and are
// saved as after a run, so a second run of the probe shows what the first one's files give
// (tests/ffx_cache_check.py runs it twice on a folder of its own). Fails when the context is not created, or when
// the library made no pipeline through the runtime's creates although they are on.
extern "C" int vk_ffx_probe_cache(void) {
    if (!vk_init()) {
        printf("probe-ffx-cache: SKIP (no Vulkan device)\n");
        return 0;
    }
    if (!vk_ffx_load()) {
        printf("probe-ffx-cache: SKIP (no amd_fidelityfx_vk.dll beside the executable, or a build without FSR 3.1)\n");
        return 0;
    }
    vk_pipeline_push_fn();   // the first vk_pipeline_* call reads the pipeline caches
    VkForeignPipes p0, p1;
    vk_pipeline_foreign_counts(&p0);
    auto t0 = std::chrono::steady_clock::now();
    Up *u = context_for(1280, 720, 2560, 1440);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    vk_pipeline_foreign_counts(&p1);
    unsigned long long made = p1.made - p0.made, hit = p1.from_cache - p0.from_cache, comp = p1.compiled - p0.compiled,
                       none = p1.uncached - p0.uncached;
    int on = pipe_cache_on(), fails = 0;
    printf("probe-ffx-cache: context %s in %.0f ms; pipelines made through the runtime's creates: %llu (from the "
           "pipeline caches %llu, compiled into the play cache %llu, through the one cache %llu, with no cache %llu); "
           "NX2NAVI_FFX_PIPE_CACHE %d\n", u ? "created" : "not created", ms, made, hit, comp, made - hit - comp - none,
           none, on);
    if (!u) fails++, printf("probe-ffx-cache: FAIL: no context\n");
    if (u && on && !made) fails++, printf("probe-ffx-cache: FAIL: the library created its pipelines past the runtime\n");
    if (u && !on && made) fails++, printf("probe-ffx-cache: FAIL: pipelines came through the runtime with the setting 0\n");
    vk_queue_lock(), vkDeviceWaitIdle(vk_ctx()->device), vk_queue_unlock();
    vk_ffx_release();
    vk_pipeline_save();
    printf("probe-ffx-cache: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
extern "C" ffxReturnCode_t vk_ffx_destroy(ffxContext *ctx) { return A.ok ? A.destroy(ctx, nullptr) : FFX_API_RETURN_ERROR; }
extern "C" ffxReturnCode_t vk_ffx_configure(ffxContext *ctx, const ffxConfigureDescHeader *desc) {
    return A.ok ? A.configure(ctx, desc) : FFX_API_RETURN_ERROR;
}
extern "C" ffxReturnCode_t vk_ffx_query(ffxContext *ctx, ffxQueryDescHeader *desc) {
    return A.ok ? A.query(ctx, desc) : FFX_API_RETURN_ERROR;
}
extern "C" ffxReturnCode_t vk_ffx_dispatch(ffxContext *ctx, const ffxDispatchDescHeader *desc) {
    return A.ok ? A.dispatch(ctx, desc) : FFX_API_RETURN_ERROR;
}

// The upscaler is off (vk_dlaa.c retire_idle; the device is idle): FSR 3.1's and FSR 4's upscale contexts go, made again
// at the next upscale. Frame generation's contexts are not these.
extern "C" int vk_ffx_holds(void) {
    for (Up &u : U)
        if (u.ctx) return 1;
    return vk_ffx4_holds();
}
extern "C" void vk_ffx_release_idle(void) {
    vk_ffx4_release();
    if (!A.ok) return;
    for (Up &u : U)
        if (u.ctx) destroy_up(&u, "the upscaler is off");
}

extern "C" void vk_ffx_release(void) {
    vk_ffx4_release();
    if (!A.ok) return;
    for (Up &u : U)
        if (u.ctx) A.destroy(&u.ctx, nullptr), u.ctx = nullptr;
}

// The memory report's FSR figures (vk_memory.c outside_log): the video memory of the upscaling contexts that exist,
// FSR 3.1's here and FSR 4's (vk_ffx4_vram), as the library reported each at its creation, with their count; and
// frame generation's (vk_fg_vram). The library allocates it itself, outside VMA. Read without a lock: a context made or
// destroyed meanwhile shows in the next report.
extern "C" uint64_t vk_ffx_vram(int *contexts, uint64_t *fg) {
    uint64_t bytes = 0;
    int n = 0;
    for (Up &u : U)
        if (__atomic_load_n(&u.ctx, __ATOMIC_RELAXED)) n++, bytes += __atomic_load_n(&u.vram, __ATOMIC_RELAXED);
    int n4 = 0;
    bytes += vk_ffx4_vram(&n4);
    *contexts = n + n4;
    *fg = vk_fg_vram();
    return bytes;
}
