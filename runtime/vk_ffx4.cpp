// AMD FSR 4 (FSR Upscaling 4.x, the machine-learned upscaler) through the libraries of AMD's FSR SDK 2.4:
// amd_fidelityfx_loader_vk.dll and amd_fidelityfx_upscaler_vk.dll next to the executable, loaded at run time. The SDK's
// headers are in third_party/ffx_sdk (MIT), at the paths they have in the SDK. FSR 3.1 and frame generation stay with
// vk_ffx.cpp and the library of SDK 1.1.4: the two sets of headers name the same types with other layouts, so nothing
// of the API crosses between the two files, and vk_ffx.cpp's vk_ffx_upscale calls in here first.
//
// The loader answers which upscaler versions it can run on a device (ffxQueryDescGetVersions with the device given):
// 4.1.2, 3.1.6 and 2.3.4 on a Radeon the ML upscaler supports (RX 6000, 7000 and 9000 cards and some Ryzen APUs, with
// AMD's driver 26.10.1 or newer on Windows), 3.1.6 and 2.3.4 on any other. FSR 4 runs only where a 4.x version is
// listed, and its contexts are made with that version asked for by its id, so the loader cannot hand out another.
// Where it is not listed, or a context or a dispatch fails, vk_ffx.cpp's FSR 3.1 serves the frame as before.
//
// The inputs are the ones FSR 3.1 takes (vk_ffx.cpp): the scene's light target in linear HDR, its D32F depth (not
// inverted, the far plane at infinity) and the camera motion vectors in render pixels from this frame to the previous
// one. FSR 4 needs no reactive or transparency mask.
#define VK_FFX_NO_API   // vk_ffx.h: without the API declarations of the other set of headers
#ifndef _WIN32
// AMD publishes the SDK's libraries for Windows only: in any other build FSR 4 is never listed, vk_ffx.cpp's FSR 3.1
// path is all there is, and the SDK's headers are not read.
extern "C" {
#include "vk.h"
#include "vk_ffx.h"
}
extern "C" int vk_ffx4_available(void) { return 0; }
extern "C" const char *vk_ffx4_name(void) { return "AMD FSR 4"; }
extern "C" int vk_ffx4_upscale(VkCommandBuffer, const VkFfxImage *, const VkFfxImage *, const VkFfxImage *, const VkFfxImage *,
                               float, float, int, float, const VkFfxCamera *) {
    return 0;
}
extern "C" void vk_ffx4_release(void) {}
extern "C" int vk_ffx4_failed(void) { return 0; }
extern "C" void vk_ffx4_retry(void) {}
extern "C" void vk_ffx4_trim(uint64_t) {}
#else
#include <windows.h>
#include <atomic>
#include <mutex>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "third_party/ffx_sdk/upscalers/include/ffx_upscale.h"
#include "vk_ffx4_api.h"

extern "C" {
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_ffx.h"
}

namespace {

struct Api {
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
} A;

std::mutex g_lock;                // the probe: the menu asks from its thread, the replay from its own
std::atomic<int> g_state{-1};     // -1 not probed; 0 FSR 4 does not run here; 1 it does
std::atomic<int> g_off{0};        // FAIL_MAX failures in a row: FSR 3.1 serves until the menu asks again (vk_ffx4_retry)
uint64_t g_version;               // the id of the 4.x version the loader listed for the device
char g_name[48] = "AMD FSR 4";    // "AMD FSR 4.1.2", for the menu and the log
int g_failures;                   // contexts or dispatches that failed in a row
const int FAIL_MAX = 4;
// After a context creation failed, FSR 3.1 serves the frames until the next try this much later: a save loading fills
// the video memory with the area's textures for a few frames, and three tries in three frames there turned FSR 4 off.
uint64_t g_retry_ns;
const uint64_t RETRY_NS = 2ull * 1000000000ull;

void message(uint32_t type, const wchar_t *msg) {
    char s[512];
    size_t n = 0;
    for (; msg && msg[n] && n < sizeof s - 1; n++) s[n] = msg[n] < 128 ? (char)msg[n] : '?';
    s[n] = 0;
    static int told;
    if (told++ < 40) rt_log("ffx4: %s %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error:" : "warning:", s);
}

// a library of the SDK next to the executable; 0 when the path does not fit
int beside_exe(const char *name, char *path, size_t size) {
    DWORD n = GetModuleFileNameA(NULL, path, (DWORD)size);
    while (n && path[n - 1] != '\\') n--;
    return snprintf(path + n, size - n, "%s", name) < (int)(size - n);
}

int file_there(const char *path) {
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

int g_loader = -1;   // under g_lock: -1 not tried, 0 the loader is not there or does not load, 1 its entry points are in A

// Loads the SDK's loader from beside the executable. Under g_lock, once the device exists (-1 before).
int load(void) {
    if (g_loader >= 0) return g_loader;
    const VkCtx *c = vk_ctx();
    if (!c || !c->device) return -1;   // asked before the device: again later
    char loader[MAX_PATH];
    if (!beside_exe("amd_fidelityfx_loader_vk.dll", loader, sizeof loader) || !file_there(loader))
        return g_loader = 0;   // not shipped beside this build: nothing is said, FSR 3.1 is what the menu offers
    HMODULE m = LoadLibraryA(loader);
    if (!m) {
        rt_log("ffx4: %s does not load (error %lu): FSR 4 off", loader, (unsigned long)GetLastError());
        return g_loader = 0;
    }
    A.create = (PfnFfxCreateContext)(void *)GetProcAddress(m, "ffxCreateContext");
    A.destroy = (PfnFfxDestroyContext)(void *)GetProcAddress(m, "ffxDestroyContext");
    A.configure = (PfnFfxConfigure)(void *)GetProcAddress(m, "ffxConfigure");
    A.query = (PfnFfxQuery)(void *)GetProcAddress(m, "ffxQuery");
    A.dispatch = (PfnFfxDispatch)(void *)GetProcAddress(m, "ffxDispatch");
    if (!A.create || !A.destroy || !A.configure || !A.query || !A.dispatch) {
        rt_log("ffx4: amd_fidelityfx_loader_vk.dll lacks the API entry points: FSR 4 off");
        return g_loader = 0;
    }
    return g_loader = 1;
}

// The versions the loader lists for one kind of context on this device, in its order: their count, at most cap.
// Under g_lock, the loader loaded.
int versions(uint64_t type, uint64_t *ids, const char **names, int cap) {
    const VkCtx *c = vk_ctx();
    VkDeviceContext dc = {c->device, c->phys, vkGetDeviceProcAddr};
    uint64_t n = 0;
    ffxQueryDescGetVersions q = {};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = type;
    q.device = &dc;
    q.outputCount = &n;
    if (A.query(nullptr, &q.header) != FFX_API_RETURN_OK || !n) return 0;   // the count
    if (n > (uint64_t)cap) n = (uint64_t)cap;
    q.versionIds = ids;
    q.versionNames = names;
    if (A.query(nullptr, &q.header) != FFX_API_RETURN_OK) return 0;
    return (int)n;
}

// Asks the loader for the device's upscaler versions. Under g_lock; -1 before the device exists.
int probe(void) {
    int loaded = load();
    if (loaded <= 0) return loaded;
    char upscaler[MAX_PATH];
    if (!beside_exe("amd_fidelityfx_upscaler_vk.dll", upscaler, sizeof upscaler) || !file_there(upscaler)) return 0;
    uint64_t ids[16] = {};
    const char *names[16] = {};
    int n = versions(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, ids, names, 16);
    if (!n) {
        rt_log("ffx4: the FSR SDK's loader lists no upscaler for this device: FSR 4 off");
        return 0;
    }
    char list[200] = "";
    size_t len = 0;
    int best = -1, best_major = 0;
    for (int i = 0; i < n; i++) {
        const char *nm = names[i] ? names[i] : "?";
        len += (size_t)snprintf(list + len, sizeof list - len, "%s%s", i ? ", " : "", nm);
        if (len >= sizeof list) len = sizeof list - 1;
        int major = atoi(nm);   // "4.1.2", and "4.1.2 *" for a version the driver brings
        if (major >= 4 && major > best_major) best = i, best_major = major;
    }
    if (best < 0) {
        rt_log("ffx4: upscaler versions for this device: %s; none is FSR 4 (it needs a Radeon RX 6000, 7000 or 9000 card "
               "or a supported Ryzen APU, and AMD's driver 26.10.1 or newer): FSR 3.1 stays", list);
        return 0;
    }
    g_version = ids[best];
    snprintf(g_name, sizeof g_name, "AMD FSR %.24s", names[best]);   // the name is the library's memory: copied
    for (char *p = g_name; *p; p++)
        if (*p == '*') *p = 0;   // the driver's mark
    for (size_t k = strlen(g_name); k && g_name[k - 1] == ' '; k--) g_name[k - 1] = 0;
    rt_log("ffx4: upscaler versions for this device: %s; FSR 4 is %s (id %#llx)", list, g_name, (unsigned long long)g_version);
    return 1;
}

// One context per render size and output size, kept as vk_ffx.cpp keeps FSR 3.1's (see there): a new render size
// makes a context of its own (50 to 70 ms and up to about 217 MiB of video memory) and the one before goes a few frames
// later, so a mode change costs a creation; with the game's dynamic resolution on one context with
// FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION takes every step. Contexts kept for every mode tried, as once, piled up to six
// beside FSR 3.1's and three creations that failed (the card out of memory) turned FSR 4 off for the run; one context
// for every render size, as next, preceded an access violation in AMD's driver inside the dispatch after a switch from
// 67 % to 100 %. The descriptors a context was made from stay with it, as the API asks ("must remain live until
// ffxDestroyContext").
struct Up {
    ffxContext ctx = nullptr;
    uint32_t mw = 0, mh = 0, ow = 0, oh = 0;   // the render size it was made for (the largest it takes with drs); the output size
    bool drs = false;                          // made for dynamic resolution (FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION)
    uint64_t used = 0, used_ns = 0;            // up_tick and the time when context_for last returned it
    ffxCreateContextDescUpscale desc = {};
    ffxCreateBackendVKDesc backend = {};
    ffxOverrideVersion version = {};
    ffxCreateContextDescUpscaleVersion built = {};
} U[4];
uint64_t up_tick;
const uint64_t UP_RECENT = 8;   // contexts used this recently may sit in a command buffer not submitted yet
// A context no upscale used for this long goes (vk_ffx4_trim): FSR 4's after a switch to FSR 3.1, and a second scene's
// (the pause screen's) a while after it closed. Unused that long, no command buffer names it.
const uint64_t UP_IDLE_NS = 90ull * 1000000000ull;

void destroy_up(Up *u, const char *why) {
    rt_log("ffx4: the %ux%u context (render sizes up to %ux%u) destroyed: %s", u->ow, u->oh, u->mw, u->mh, why);
    A.destroy(&u->ctx, nullptr);
    *u = Up();
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

// A context in slot for render sizes up to mw x mh and the output size ow x oh; NULL when it is not made.
Up *create(Up *slot, uint32_t mw, uint32_t mh, uint32_t ow, uint32_t oh, bool drs) {
    const VkCtx *c = vk_ctx();
    *slot = Up();
    slot->built.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    slot->built.version = FFX_UPSCALER_VERSION;
    slot->version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    slot->version.header.pNext = &slot->built.header;
    slot->version.versionId = g_version;
    slot->backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    slot->backend.header.pNext = &slot->version.header;
    slot->backend.deviceContext = {c->device, c->phys, vkGetDeviceProcAddr};
    slot->desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    slot->desc.header.pNext = &slot->backend.header;
    slot->desc.flags =
        FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;   // a finite far plane, as FSR 3.1's
    // NX2NAVI_FFX_DEBUG=1 (test): the library checks the values of each dispatch and says what it finds (message)
    if (rt_knob("FFX_DEBUG", "0")) slot->desc.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    if (drs) slot->desc.flags |= FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
    slot->desc.maxRenderSize = {mw, mh};
    slot->desc.maxUpscaleSize = {ow, oh};
    slot->desc.fpMessage = message;
    uint64_t t0 = plat_now_ns();
    ffxReturnCode_t r = A.create(&slot->ctx, &slot->desc.header, nullptr);
    if (r != FFX_API_RETURN_OK || !slot->ctx) {
        rt_log("ffx4: %s context creation for %ux%u (render sizes up to %ux%u) failed (%u)", g_name, ow, oh, mw, mh,
               (unsigned)r);
        *slot = Up();
        return nullptr;
    }
    slot->mw = mw, slot->mh = mh, slot->ow = ow, slot->oh = oh, slot->drs = drs;
    ffxQueryGetProviderVersion v = {};
    v.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    A.query(&slot->ctx, &v.header);
    FfxApiEffectMemoryUsage mem = {};
    ffxQueryDescUpscaleGetGPUMemoryUsage mq = {};
    mq.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE;
    mq.gpuMemoryUsageUpscaler = &mem;
    A.query(&slot->ctx, &mq.header);
    rt_log("ffx4: FSR %s upscaling to %ux%u from render sizes up to %ux%u, made in %.0f ms, %.0f MiB of video memory",
           v.versionName ? v.versionName : "4", ow, oh, mw, mh, (double)(plat_now_ns() - t0) / 1e6,
           (double)mem.totalUsageInBytes / 1048576.0);
    // The provider is asked for the version it runs: anything but a 4.x here means the loader gave another upscaler
    if (v.versionName && atoi(v.versionName) < 4) {
        rt_log("ffx4: the context runs FSR %s, not FSR 4: FSR 4 off", v.versionName);
        A.destroy(&slot->ctx, nullptr);
        *slot = Up();
        return nullptr;
    }
    return slot;
}

// The context for a render size and an output size: the one made for them, else a new one, in a free slot or in place
// of the least recently used one, with the device idle first; the others of that output size go once no command
// buffer can hold them. A creation that fails is tried once more after every context not in use has gone, FSR 3.1's
// idle ones too (vk_ffx_trim_idle): what it lacked was most often video memory.
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
    Up *u = create(slot, mw, mh, ow, oh, drs);
    if (!u) {
        int freed = 0;
        vk_queue_lock(), vkDeviceWaitIdle(c->device), vk_queue_unlock();
        for (Up &o : U)
            if (o.ctx && up_tick - o.used >= UP_RECENT) destroy_up(&o, "room for a context whose creation failed"), freed++;
        freed += vk_ffx_trim_idle(UP_RECENT);
        if (freed) u = create(slot, mw, mh, ow, oh, drs);
    }
    if (u) u->used = up_tick, u->used_ns = now;
    return u;
}

// Failures in a row: FAIL_MAX of them turn FSR 4 off until the menu asks for it again (vk_ffx4_retry); a success
// starts the count again, so a creation that failed once in a while does not add up over a session.
void failed(const char *what, unsigned code) {
    if (++g_failures == FAIL_MAX) {
        rt_log("ffx4: %s failed %d times in a row (last %u): FSR 4 is off, FSR 3.1 takes its place until FSR 4 is "
               "chosen again in the menu", what, FAIL_MAX, code);
        g_off.store(1, std::memory_order_release);
    }
}

}  // namespace

extern "C" int vk_ffx4_available(void) {
    int s = g_state.load(std::memory_order_acquire);
    if (s >= 0) return s;
    std::lock_guard<std::mutex> hold(g_lock);
    s = g_state.load(std::memory_order_acquire);
    if (s >= 0) return s;
    s = probe();
    if (s >= 0) g_state.store(s, std::memory_order_release);
    return s > 0;
}

extern "C" const char *vk_ffx4_name(void) { return g_name; }

extern "C" int vk_ffx4_failed(void) { return g_off.load(std::memory_order_acquire); }

// The menu chose FSR 4 again: after failures that turned it off, it is tried anew.
extern "C" void vk_ffx4_retry(void) {
    if (!g_off.exchange(0, std::memory_order_acq_rel)) return;
    g_failures = 0, g_retry_ns = 0;
    rt_log("ffx4: FSR 4 chosen again in the menu: tried anew");
}

// Called at every upscale, with FSR 4 or FSR 3.1 (vk_ffx.cpp vk_ffx_upscale), on the thread that records them.
extern "C" void vk_ffx4_trim(uint64_t now_ns) {
    for (Up &u : U)
        if (u.ctx && now_ns - u.used_ns > UP_IDLE_NS) destroy_up(&u, "no upscale used it for 90 s");
}

extern "C" int vk_ffx4_upscale(VkCommandBuffer cb, const VkFfxImage *color, const VkFfxImage *depth, const VkFfxImage *mv,
                               const VkFfxImage *out, float jx, float jy, int reset, float dt_ms, const VkFfxCamera *cam) {
    if (!vk_ffx4_available() || g_off.load(std::memory_order_acquire)) return 0;
    uint64_t now = plat_now_ns();
    if (g_failures && now < g_retry_ns) return 0;   // FSR 3.1 serves until the next try
    Up *u = context_for(color->width, color->height, out->width, out->height);
    if (!u) {
        g_retry_ns = now + RETRY_NS;
        failed("making a context", 0);
        return 0;
    }
    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cb;
    // Every image is in GENERAL, which the back end reads as its unordered-access state, and is left there
    d.color = image(color, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, 0);
    d.depth = image(depth, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, 0);
    d.motionVectors = image(mv, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, 0);
    d.output = image(out, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
    d.jitterOffset = {jx, jy};
    d.motionVectorScale = {1.0f, 1.0f};
    d.renderSize = {color->width, color->height};
    d.upscaleSize = {out->width, out->height};
    int sharp = cfg_int("NX2NAVI_SHARPNESS");   // the slider drives the upscaler's own sharpening, as for FSR 3.1
    d.enableSharpening = sharp > 0;
    d.sharpness = sharp > 100 ? 1.0f : sharp / 100.0f;
    d.frameTimeDelta = dt_ms;
    d.preExposure = 1.0f;
    d.reset = reset != 0;
    // the frame's camera (vk_dlaa.c proj_planes), as FSR 3.1 gets it (vk_ffx.cpp)
    d.cameraNear = cam && cam->znear > 0 ? cam->znear : 0.1f;
    d.cameraFar = cam && cam->zfar > 0 ? cam->zfar : 1e8f;
    d.cameraFovAngleVertical = cam && cam->fov_y > 0 ? cam->fov_y : 1.0f;
    d.viewSpaceToMetersFactor = cam && cam->meters > 0 ? cam->meters : 1.0f;
    ffxReturnCode_t r = A.dispatch(&u->ctx, &d.header);
    if (r != FFX_API_RETURN_OK) {
        static int told;
        if (told++ < 8) rt_log("ffx4: %s dispatch failed (%u)", g_name, (unsigned)r);
        failed("a dispatch", (unsigned)r);
        return 0;
    }
    g_failures = 0;
    return 1;
}

// The loader's calls for vk_fg4.cpp (vk_ffx4_api.h).
extern "C" int vk_ffx4_api_file(const char *name) {
    char path[MAX_PATH];
    return beside_exe(name, path, sizeof path) && file_there(path);
}
extern "C" int vk_ffx4_api_versions(uint64_t create_type, uint64_t *ids, const char **names, int cap) {
    std::lock_guard<std::mutex> hold(g_lock);
    return load() > 0 ? versions(create_type, ids, names, cap) : 0;
}
extern "C" ffxReturnCode_t vk_ffx4_api_create(ffxContext *ctx, ffxCreateContextDescHeader *desc) {
    return A.create ? A.create(ctx, desc, nullptr) : FFX_API_RETURN_ERROR;
}
extern "C" ffxReturnCode_t vk_ffx4_api_destroy(ffxContext *ctx) {
    return A.destroy ? A.destroy(ctx, nullptr) : FFX_API_RETURN_ERROR;
}
extern "C" ffxReturnCode_t vk_ffx4_api_configure(ffxContext *ctx, const ffxConfigureDescHeader *desc) {
    return A.configure ? A.configure(ctx, desc) : FFX_API_RETURN_ERROR;
}
extern "C" ffxReturnCode_t vk_ffx4_api_query(ffxContext *ctx, ffxQueryDescHeader *desc) {
    return A.query ? A.query(ctx, desc) : FFX_API_RETURN_ERROR;
}
extern "C" ffxReturnCode_t vk_ffx4_api_dispatch(ffxContext *ctx, const ffxDispatchDescHeader *desc) {
    return A.dispatch ? A.dispatch(ctx, desc) : FFX_API_RETURN_ERROR;
}

extern "C" void vk_ffx4_release(void) {
    if (!A.destroy) return;
    for (Up &u : U)
        if (u.ctx) A.destroy(&u.ctx, nullptr), u = Up();
}
#endif   // _WIN32
