// nx2navi_dlssg.dll: DLSS Frame Generation through NVIDIA Streamline, behind the C interface of nx2_sl.h. Built with
// MSVC against the Streamline SDK's headers (tools/build_dlssg.py; the SDK stays outside the repository), so its C++
// types have the ABI of Streamline's own DLLs. sl.interposer.dll and its plugins are loaded from the game's folder at
// run time; this DLL links none of them.
//
// Manual hooking (the SDK's ProgrammingGuideManualHooking): the game creates its instance and device through the
// interposer's vkCreateInstance and vkCreateDevice, which add what DLSS-G needs, and calls the interposer's
// swapchain functions; everything else goes to the Vulkan loader as before. Each frame of the game sleeps in Reflex
// where it starts and gets the PCL latency markers, the camera constants and the depth and motion vector tags (valid
// until the present), then the present goes through the interposer, which inserts the generated frames: one, two or
// three between two rendered frames (2x, 3x or 4x), up to what the GPU and driver allow (numFramesToGenerateMax).
#define NX2SL_BUILD
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <atomic>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_helpers.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

#include "nx2_sl.h"

namespace {

HMODULE g_mod;
Nx2SlLog g_log;
PFun_slInit *p_init;
PFun_slShutdown *p_shutdown;
PFun_slIsFeatureLoaded *p_loaded;
PFun_slSetTagForFrame *p_tag;
PFun_slSetConstants *p_constants;
PFun_slGetNewFrameToken *p_token;
PFun_slGetFeatureFunction *p_feature;
PFun_slDLSSGSetOptions *p_dlssg_options;
PFun_slDLSSGGetState *p_dlssg_state;
PFun_slPCLSetMarker *p_marker;
PFun_slReflexSetOptions *p_reflex;
PFun_slReflexSleep *p_sleep;
PFN_vkGetInstanceProcAddr p_gipa;
PFN_vkGetDeviceProcAddr p_gdpa;
int g_available = -1;
int g_frames = -1;                   // the frames to generate as last set (0 off), -1 before the first nx2sl_frame
std::atomic<uint32_t> g_max{0};      // numFramesToGenerateMax as Streamline last reported it, 0 before it did
uint64_t g_presented, g_shown;

void say(const char *fmt, ...) {
    if (!g_log) return;
    char s[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(s, sizeof s, fmt, a);
    va_end(a);
    g_log(s);
}

void sl_log(sl::LogType type, const char *msg) {
    if (type == sl::LogType::eInfo) return;   // only warnings and errors reach the game's log
    static int n;
    if (n++ < 200) say("dlssg: streamline %s: %s", type == sl::LogType::eError ? "error" : "warning", msg);
}

template <class T> T *proc(const char *name) { return reinterpret_cast<T *>(GetProcAddress(g_mod, name)); }

// The token of frame n. Tokens come from a small ring Streamline reuses, so each call takes its own right before use.
sl::FrameToken *token(uint32_t frame) {
    sl::FrameToken *t = nullptr;
    return frame && p_token(t, &frame) == sl::Result::eOk ? t : nullptr;
}

void matrix(sl::float4x4 &m, const float *v) {
    for (int r = 0; r < 4; r++) m[r] = sl::float4(v[r * 4], v[r * 4 + 1], v[r * 4 + 2], v[r * 4 + 3]);
}

sl::Resource resource(const Nx2SlImage *i) {
    sl::Resource r(sl::ResourceType::eTex2d, (void *)i->image, nullptr, (void *)i->view, (uint32_t)i->layout);
    r.width = i->width;
    r.height = i->height;
    r.nativeFormat = (uint32_t)i->format;
    r.mipLevels = 1;
    r.arrayLayers = 1;
    r.usage = (uint32_t)i->usage;
    return r;
}

}  // namespace

extern "C" NX2SL_API int nx2sl_version(void) { return NX2SL_VERSION; }

extern "C" NX2SL_API int nx2sl_init(const wchar_t *dir, Nx2SlLog log) {
    g_log = log;
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\sl.interposer.dll", dir);
    g_mod = LoadLibraryW(path);
    if (!g_mod) return say("dlssg: sl.interposer.dll not found next to the game: DLSS Frame Generation off"), 0;
    p_init = proc<PFun_slInit>("slInit");
    p_shutdown = proc<PFun_slShutdown>("slShutdown");
    p_loaded = proc<PFun_slIsFeatureLoaded>("slIsFeatureLoaded");
    p_tag = proc<PFun_slSetTagForFrame>("slSetTagForFrame");
    p_constants = proc<PFun_slSetConstants>("slSetConstants");
    p_token = proc<PFun_slGetNewFrameToken>("slGetNewFrameToken");
    p_feature = proc<PFun_slGetFeatureFunction>("slGetFeatureFunction");
    p_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(g_mod, "vkGetInstanceProcAddr"));
    p_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(g_mod, "vkGetDeviceProcAddr"));
    if (!p_init || !p_tag || !p_constants || !p_token || !p_feature || !p_gipa || !p_gdpa)
        return say("dlssg: sl.interposer.dll lacks its entry points"), 0;
    static const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    const wchar_t *paths[] = {dir};
    sl::Preferences p{};
    p.showConsole = false;
    p.logLevel = sl::LogLevel::eDefault;
    p.pathsToPlugins = paths;
    p.numPathsToPlugins = 1;
    p.pathToLogsAndData = dir;
    p.logMessageCallback = sl_log;
    // the game tracks its own command buffer state; no plugin downloads at run time. The depth and motion vectors are
    // tagged per frame (slSetTagForFrame, nx2sl_frame), which Streamline refuses without the frame-based flag.
    p.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
              sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    p.featuresToLoad = features;
    p.numFeaturesToLoad = 3;
    p.applicationId = 231313132;   // the id NVIDIA's samples use for builds without an issued one (as vk_dlaa.c's NGX)
    p.engine = sl::EngineType::eCustom;
    p.renderAPI = sl::RenderAPI::eVulkan;
    sl::Result r = p_init(p, sl::kSDKVersion);
    if (r != sl::Result::eOk) return say("dlssg: slInit failed (%d): DLSS Frame Generation off", (int)r), 0;
    say("dlssg: Streamline up");
    return 1;
}

extern "C" NX2SL_API PFN_vkGetInstanceProcAddr nx2sl_instance_proc(void) { return p_gipa; }
extern "C" NX2SL_API PFN_vkGetDeviceProcAddr nx2sl_device_proc(void) { return p_gdpa; }

extern "C" NX2SL_API int nx2sl_available(void) {
    if (g_available >= 0) return g_available;
    bool loaded = false;
    g_available = p_loaded && p_loaded(sl::kFeatureDLSS_G, loaded) == sl::Result::eOk && loaded;
    void *f = nullptr;
    if (g_available && p_feature(sl::kFeatureDLSS_G, "slDLSSGSetOptions", f) == sl::Result::eOk) p_dlssg_options = (PFun_slDLSSGSetOptions *)f;
    if (g_available && p_feature(sl::kFeatureDLSS_G, "slDLSSGGetState", f) == sl::Result::eOk) p_dlssg_state = (PFun_slDLSSGGetState *)f;
    if (p_feature(sl::kFeaturePCL, "slPCLSetMarker", f) == sl::Result::eOk) p_marker = (PFun_slPCLSetMarker *)f;
    if (p_feature(sl::kFeatureReflex, "slReflexSetOptions", f) == sl::Result::eOk) p_reflex = (PFun_slReflexSetOptions *)f;
    if (p_feature(sl::kFeatureReflex, "slReflexSleep", f) == sl::Result::eOk) p_sleep = (PFun_slReflexSleep *)f;
    if (!p_dlssg_options || !p_marker) g_available = 0;
    // How many frames this GPU and driver generate per rendered frame: known once the device exists (else after the
    // first presents, nx2sl_present_marker)
    sl::DLSSGState s{};
    if (g_available && p_dlssg_state && p_dlssg_state(sl::ViewportHandle(0), s, nullptr) == sl::Result::eOk)
        g_max.store(s.numFramesToGenerateMax);
    if (!g_available) say("dlssg: frame generation unavailable (DLSS-G plugin loaded: %d, DLSS-G options: %d, PCL marker: %d)",
                          loaded, p_dlssg_options != nullptr, p_marker != nullptr);
    else if (g_max.load()) say("dlssg: DLSS Frame Generation available, up to %ux", g_max.load() + 1);
    else say("dlssg: DLSS Frame Generation available (its most frames per rendered frame not reported yet)");
    return g_available;
}

extern "C" NX2SL_API void nx2sl_log_unavailable(VkPhysicalDevice physical) {
    auto supported = proc<PFun_slIsFeatureSupported>("slIsFeatureSupported");
    if (!supported) return say("dlssg: Streamline has no adapter support query");
    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = physical;
    sl::Result r = supported(sl::kFeatureDLSS_G, adapter);
    say("dlssg: Streamline's support check for frame generation: %s (%d)", sl::getResultAsStr(r), (int)r);
}

// Streamline sets Reflex's mode on the swapchain it was told of last (VK_NV_low_latency2) and drops it while there is
// none, so it is set again after each swapchain.
extern "C" NX2SL_API void nx2sl_reflex(int mode) {
    if (g_available != 1 || !p_reflex) return;
    sl::ReflexOptions o{};
    o.mode = mode == 2 ? sl::ReflexMode::eLowLatencyWithBoost : mode ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eOff;
    sl::Result r = p_reflex(o);
    static int n;
    if (n++ < 8) say("dlssg: Reflex low-latency mode %s (%d)", mode == 2 ? "on with boost" : mode ? "on" : "off", (int)r);
}

// The game's simulation and its recording of the frame's commands are one span here: the guest's threads cannot be
// told apart, so the simulation markers sit at the frame's start and the render submission runs until the frame's
// submissions went out (nx2sl_submit_end).
extern "C" NX2SL_API void nx2sl_sleep(uint32_t frame, int hold) {
    if (g_available != 1) return;
    sl::FrameToken *t = token(frame);
    if (hold && t && p_sleep) p_sleep(*t);
    if ((t = token(frame))) {
        p_marker(sl::PCLMarker::eSimulationStart, *t);
        p_marker(sl::PCLMarker::eSimulationEnd, *t);
        p_marker(sl::PCLMarker::eRenderSubmitStart, *t);
    }
}

extern "C" NX2SL_API void nx2sl_submit_end(uint32_t frame) {
    sl::FrameToken *t = g_available == 1 ? token(frame) : nullptr;
    if (t) p_marker(sl::PCLMarker::eRenderSubmitEnd, *t);
}

extern "C" NX2SL_API int nx2sl_frame(VkCommandBuffer cb, const Nx2SlImage *depth, const Nx2SlImage *mv,
                                     const Nx2SlImage *hudless, const Nx2SlCamera *cam, uint32_t out_w, uint32_t out_h,
                                     int enable, uint32_t frame) {
    if (g_available != 1) return 0;
    sl::FrameToken *t = token(frame);
    if (!t) return 0;
    sl::ViewportHandle vp(0);
    // never more than Streamline allows: one while it has not said
    uint32_t max = g_max.load();
    int n = enable > 0 ? enable : 0;
    if (n > (int)(max ? max : 1)) n = (int)(max ? max : 1);
    if (n && depth && mv && cam) {
        sl::Constants c{};
        matrix(c.cameraViewToClip, cam->view_to_clip);
        matrix(c.clipToCameraView, cam->clip_to_view);
        matrix(c.clipToPrevClip, cam->clip_to_prev_clip);
        matrix(c.prevClipToClip, cam->prev_clip_to_clip);
        c.jitterOffset = sl::float2(cam->jitter[0], cam->jitter[1]);
        c.mvecScale = sl::float2(cam->mv_scale[0], cam->mv_scale[1]);
        c.cameraPinholeOffset = sl::float2(0, 0);
        c.cameraPos = sl::float3(cam->pos[0], cam->pos[1], cam->pos[2]);
        c.cameraUp = sl::float3(cam->up[0], cam->up[1], cam->up[2]);
        c.cameraRight = sl::float3(cam->right[0], cam->right[1], cam->right[2]);
        c.cameraFwd = sl::float3(cam->fwd[0], cam->fwd[1], cam->fwd[2]);
        c.cameraNear = cam->znear;
        c.cameraFar = cam->zfar;
        c.cameraFOV = cam->fov_y;
        c.cameraAspectRatio = cam->aspect;
        c.depthInverted = sl::Boolean::eFalse;
        c.cameraMotionIncluded = sl::Boolean::eTrue;
        c.motionVectors3D = sl::Boolean::eFalse;
        c.reset = cam->reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        p_constants(c, *t, vp);
        sl::Resource rd = resource(depth), rm = resource(mv), rh = hudless ? resource(hudless) : sl::Resource{};
        sl::Extent ed{0, 0, depth->width, depth->height}, em{0, 0, mv->width, mv->height};
        sl::Extent eh{0, 0, hudless ? hudless->width : 0, hudless ? hudless->height : 0};
        sl::ResourceTag tags[3] = {sl::ResourceTag(&rd, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &ed),
                                   sl::ResourceTag(&rm, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &em),
                                   sl::ResourceTag(&rh, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &eh)};
        if ((t = token(frame))) p_tag(*t, vp, tags, hudless ? 3 : 2, (sl::CommandBuffer *)cb);
    }
    if (n != g_frames) {
        sl::DLSSGOptions o{};
        o.mode = n ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
        o.numFramesToGenerate = n ? (uint32_t)n : 1;
        // Generation goes off for every menu, cutscene and frame without depth and motion, and on again after it. Off
        // frees the generator's resources unless this flag is set, and on allocates them again inside the present, with
        // the queue lock held: a frame of 100 to 150 ms each time, the submitting thread waiting for that lock. With
        // the flag, off only suspends. Whether frame generation runs at all is fixed for the run (vk_dlssg.c), so
        // nothing is held for long that would otherwise be given back; Streamline frees it at shutdown.
        o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
        sl::Result r = p_dlssg_options(vp, o);
        if (n) say("dlssg: frame generation on, %dx (%d generated per rendered frame) (%d), output %ux%u", n + 1, n, (int)r, out_w, out_h);
        else say("dlssg: frame generation off (%d), output %ux%u", (int)r, out_w, out_h);
        g_frames = n;
    }
    return 1;
}

extern "C" NX2SL_API void nx2sl_present_marker(int after, uint32_t frame) {
    sl::FrameToken *t = g_available == 1 ? token(frame) : nullptr;
    if (!t) return;
    p_marker(after ? sl::PCLMarker::ePresentEnd : sl::PCLMarker::ePresentStart, *t);
    if (!after) return;
    g_presented++;
    // Asked after each present, numFramesActuallyPresented is what that present showed: 2 with one generated frame, 4
    // with three.
    sl::DLSSGState s{};
    if (!p_dlssg_state || p_dlssg_state(sl::ViewportHandle(0), s, nullptr) != sl::Result::eOk) return;
    g_shown += s.numFramesActuallyPresented;
    if (s.numFramesToGenerateMax && s.numFramesToGenerateMax != g_max.load()) {   // a 0 now and then is no news
        say("dlssg: up to %ux on this GPU and driver", s.numFramesToGenerateMax + 1);
        g_max.store(s.numFramesToGenerateMax);
    }
    if (g_presented == 600 || g_presented % 3600 == 0)
        say("dlssg: %llu frames, %u presented per frame, status %u", (unsigned long long)g_presented,
            s.numFramesActuallyPresented, (unsigned)s.status);
}

extern "C" NX2SL_API uint32_t nx2sl_frames_max(void) { return g_available == 1 ? g_max.load() : 0; }

extern "C" NX2SL_API uint64_t nx2sl_shown(void) { return g_shown; }

extern "C" NX2SL_API void nx2sl_shutdown(void) {
    if (p_shutdown && g_available >= 0) p_shutdown();
}
