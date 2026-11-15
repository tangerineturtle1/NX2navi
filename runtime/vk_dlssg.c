// DLSS Frame Generation (NX2NAVI_FRAMEGEN = dlss; applies after a restart) through nx2navi_dlssg.dll
// (runtime/dlssg/nx2_sl.cpp), which drives NVIDIA Streamline. Like AMD's frame generation (vk_fg.cpp) it takes the
// depth, motion vectors and camera vk_dlaa.c makes for DLSS or FSR 3.1 upscaling, or without an upscaler for frame
// generation alone (vk_dlaa.c fg_only_frame: the same pass at the scene's size, no jitter); it needs an RTX 40 or 50 GPU.
//
// The DLL and Streamline (sl.interposer.dll and its plugins, nvngx_dlssg.dll) sit next to the game. The instance and
// device are created through Streamline's proxies (vk_device.c), so it can add the extensions, features and queues
// DLSS-G needs; the present path calls Streamline's swapchain functions (gfx_present.c), and every present records the
// frame's constants and tags first (vk_dlssg_frame). Generation pauses while the menu is open.
//
// Multi frame generation: NX2NAVI_FRAMEGEN_MULTIPLIER (2, 3 or 4, live) is how many frames the display gets per
// rendered frame, so one, two or three generated ones. Streamline says how many this GPU and driver can generate
// (numFramesToGenerateMax: one where only 2x runs, three on an RTX 50); a higher setting is clamped to it with a log
// line. The frame limit counts the frames the game draws and the generated ones come on top (frame_rate.c
// limit_game_hz): 60 at 4x is 60 drawn and 240 shown. Only a limit of display is divided by the multiplier in effect
// (vk_dlssg_multiplier), since a frame past the monitor's refresh is never seen.
//
// DLSS-G needs Reflex, whose low-latency mode paces the game (NX2NAVI_REFLEX, default 1; 0 off, 2 on with boost): the
// game's frame sleeps in Reflex where it starts (its acquire of a window image), and each frame carries the latency
// markers from that start to the host present, all with the frame's number, the count of the game's presents up to it.
// The mode is the swapchain's, so it is set again after each one (vk_dlssg_create_swapchain).
//
// One limiter. Reflex's sleep also holds the frame limit (its frame limit, the driver's minimum interval), and the
// game's own pacer then keeps only the game's ceiling (vk_dlssg_pace, nvn_api_obj.c nvnQueuePresentTexture): the pacer's
// wait came after the picture was drawn, so the pad was read up to a period before the picture left, and Reflex's sleep
// at the frame's start already waited for the GPU. The pacer keeps the limit while a movie plays, before the first host
// present, while none went out for DLSSG_HOLD_IDLE_NS, with NX2NAVI_REFLEX_LIMIT=0 (live), a DLL older than
// nx2sl_reflex2 or a limit Streamline refused, and for the rest of the run once the frames show Reflex does not hold it
// (dlssg_pace.h). The guest thread that presents decides and the present thread hands the limit to Streamline before
// its present (vk_dlssg_frame, under the present lock, which keeps Streamline's options calls on one thread at a time);
// the pacer stands aside only once Streamline took that limit.
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "dlssg_pace.h"
#include "host.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_dlssg.h"
#include "vk_dlssg_device.h"
#include "dlssg/nx2_sl.h"
#include "ui/menu.h"

static struct {
    int wanted, loaded, active;
    int fail;            // DLSSG_NO_FILES or DLSSG_UNAVAILABLE when it was wanted and does not run (vk_dlssg_status)
    int mode, pending;   // generation as the driver has it, and the frames since the host asked for the other state
    int shown_mult;      // vk_dlssg_multiplier: the frames per game frame as last handed to the DLL (1 while off)
    PFN_nx2sl_frames_max frames_max;   // NULL with an older DLL, which generates one frame
    PFN_nx2sl_init init;
    PFN_nx2sl_instance_proc gipa;
    PFN_nx2sl_device_proc gdpa;
    PFN_nx2sl_available available;
    PFN_nx2sl_log_unavailable log_unavailable;
    PFN_nx2sl_frame frame;
    PFN_nx2sl_present_marker marker;
    PFN_nx2sl_sleep sleep;
    PFN_nx2sl_submit_end submit_end;
    PFN_nx2sl_shown shown;
    PFN_nx2sl_reflex reflex;
    PFN_nx2sl_reflex2 reflex2;   // NULL with an older DLL: Reflex holds no frame limit, the pacer keeps it
    uint32_t game;   // the game's presents (vk_dlssg_game_present)
    uint32_t cur;    // the frame the present thread blits and presents next (vk_dlssg_present_frame)
    uint32_t started;      // the last frame vk_dlssg_frame_start started
    uint64_t hosted_ns;    // kern_now_ns after the last host present
    // Reflex's frame limit, in microseconds (0 none). want: what vk_dlssg_pace asks for (the presenting guest thread
    // writes it); set: what Streamline last took (written under the present lock, reflex_apply); refused: Streamline
    // refused a limit, so the pacer keeps it for the run. Atomics.
    uint32_t limit_want, limit_set;
    int limit_refused;
    DlssgPace pace;              // the presenting guest thread's (vk_dlssg_pace)
    int pace_said;               // who held the limit as last logged: 1 Reflex, 0 the pacer, -1 not yet
    // the frame starts Reflex's sleep held, and their time, since the last statistics line (stats); atomics
    uint64_t n_hold, hold_ns, n_unheld;
    PFN_nx2sl_shutdown shutdown;
    PFN_vkCreateSwapchainKHR create_sc;
    PFN_vkDestroySwapchainKHR destroy_sc;
    PFN_vkGetSwapchainImagesKHR images;
    PFN_vkAcquireNextImageKHR acquire;
    PFN_vkQueuePresentKHR present;
} S = {.wanted = -1, .fail = DLSSG_NO_FILES, .shown_mult = 1, .pace_said = -1};

static void sl_line(const char *line) { rt_log("%s", line); }

static int reflex_mode(void) {
    static int m = -1;
    if (m < 0) {
        const char *s = cfg_get("NX2NAVI_REFLEX");
        m = s && *s ? atoi(s) : 1;
        if (m < 0 || m > 2) m = 1;
    }
    return m;
}

// Reflex's mode and frame limit to Streamline: under the present lock (vk_dlssg_frame, vk_dlssg_create_swapchain), or
// once at the device's creation before any present. Streamline's slReflexSetOptions is not thread safe, and every
// caller is one of these. A limit Streamline refuses leaves the limit to the pacer for the run.
static void reflex_apply(uint32_t us) {
    if (!S.reflex2) {
        S.reflex(reflex_mode());
        return;
    }
    if (__atomic_load_n(&S.limit_refused, __ATOMIC_ACQUIRE)) us = 0;
    int ok = S.reflex2(reflex_mode(), us);
    if (!ok && us) {
        __atomic_store_n(&S.limit_refused, 1, __ATOMIC_RELEASE);
        rt_log("dlssg: Streamline did not take Reflex's frame limit of %.3f ms: the game's own limiter keeps the limit "
               "for the rest of the run", us / 1000.0);
        ok = S.reflex2(reflex_mode(), 0), us = 0;
    }
    __atomic_store_n(&S.limit_set, ok ? us : 0, __ATOMIC_RELEASE);
}

// Reflex may hold the frame limit: NX2NAVI_REFLEX_LIMIT (default 1, live; 0 leaves the limit to the game's own limiter,
// as before Reflex's frame limit was used), and a DLL with nx2sl_reflex2 whose limit Streamline did not refuse. The
// setting is parsed again when a setting changed (cfg_version). Only the presenting guest thread calls it.
static int limit_allowed(void) {
    static unsigned seen;
    static int read, on;
    unsigned v = cfg_version();
    if (!read || v != seen) on = cfg_bool("NX2NAVI_REFLEX_LIMIT"), seen = v, read = 1;
    return on && S.reflex2 && !__atomic_load_n(&S.limit_refused, __ATOMIC_ACQUIRE);
}

// With any upscaler or none: vk_dlaa.c makes the inputs either way (vk_dlaa_frame says which, at the start and at each
// change).
int vk_dlssg_wanted(void) {
    if (S.wanted < 0) S.wanted = !strcmp(cfg_value("NX2NAVI_FRAMEGEN"), "dlss");
    return S.wanted;
}

int vk_dlssg_early(void) {
    if (!vk_dlssg_wanted()) return 0;
    if (S.loaded) return S.loaded > 0;
    S.loaded = -1;
#ifdef _WIN32
    wchar_t dir[MAX_PATH], path[MAX_PATH + 32];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    while (n && dir[n - 1] != L'\\') n--;
    dir[n ? n - 1 : 0] = 0;
    swprintf(path, MAX_PATH + 32, L"%ls\\nx2navi_dlssg.dll", dir);
    HMODULE m = LoadLibraryW(path);
    if (!m) {
        rt_log("dlssg: nx2navi_dlssg.dll is not next to the game: DLSS Frame Generation off");
        return 0;
    }
    PFN_nx2sl_version ver = (PFN_nx2sl_version)(void *)GetProcAddress(m, "nx2sl_version");
    S.init = (PFN_nx2sl_init)(void *)GetProcAddress(m, "nx2sl_init");
    S.gipa = (PFN_nx2sl_instance_proc)(void *)GetProcAddress(m, "nx2sl_instance_proc");
    S.gdpa = (PFN_nx2sl_device_proc)(void *)GetProcAddress(m, "nx2sl_device_proc");
    S.available = (PFN_nx2sl_available)(void *)GetProcAddress(m, "nx2sl_available");
    S.log_unavailable = (PFN_nx2sl_log_unavailable)(void *)GetProcAddress(m, "nx2sl_log_unavailable");
    S.frame = (PFN_nx2sl_frame)(void *)GetProcAddress(m, "nx2sl_frame");
    S.marker = (PFN_nx2sl_present_marker)(void *)GetProcAddress(m, "nx2sl_present_marker");
    S.sleep = (PFN_nx2sl_sleep)(void *)GetProcAddress(m, "nx2sl_sleep");
    S.submit_end = (PFN_nx2sl_submit_end)(void *)GetProcAddress(m, "nx2sl_submit_end");
    S.shown = (PFN_nx2sl_shown)(void *)GetProcAddress(m, "nx2sl_shown");
    S.shutdown = (PFN_nx2sl_shutdown)(void *)GetProcAddress(m, "nx2sl_shutdown");
    S.reflex = (PFN_nx2sl_reflex)(void *)GetProcAddress(m, "nx2sl_reflex");
    S.frames_max = (PFN_nx2sl_frames_max)(void *)GetProcAddress(m, "nx2sl_frames_max");   // optional: 2x only without
    if (!S.frames_max) rt_log("dlssg: nx2navi_dlssg.dll is older than multi frame generation: 2x at most");
    S.reflex2 = (PFN_nx2sl_reflex2)(void *)GetProcAddress(m, "nx2sl_reflex2");   // optional: the pacer keeps the limit
    if (!S.reflex2) rt_log("dlssg: nx2navi_dlssg.dll is older than Reflex's frame limit: the game's own limiter keeps it");
    if (!ver || ver() != NX2SL_VERSION || !S.init || !S.gipa || !S.gdpa || !S.available || !S.frame || !S.marker ||
        !S.sleep || !S.submit_end || !S.shown || !S.reflex) {
        rt_log("dlssg: nx2navi_dlssg.dll is from another build: DLSS Frame Generation off");
        return 0;
    }
    if (!S.init(dir, sl_line)) return 0;
    S.loaded = 1;
    return 1;
#else
    return 0;
#endif
}

VkResult vk_dlssg_create_instance(const VkInstanceCreateInfo *ci, VkInstance *out) {
    PFN_vkCreateInstance f = S.loaded > 0 ? (PFN_vkCreateInstance)S.gipa()(VK_NULL_HANDLE, "vkCreateInstance") : NULL;
    return f ? f(ci, NULL, out) : vkCreateInstance(ci, NULL, out);
}

// An instance function through the interposer while Streamline is loaded, else NULL (the caller takes the loader's).
// The window surface goes through it: Streamline learns the window a swapchain shows from vkCreateWin32SurfaceKHR, and
// without that its swapchain hook found "no window corresponding to this application" and generated no frames.
PFN_vkVoidFunction vk_dlssg_instance_proc(VkInstance inst, const char *name) {
    return S.loaded > 0 ? S.gipa()(inst, name) : NULL;
}

// The interposer's vkCreateDevice finds the device's instance in a map its own vkEnumeratePhysicalDevices fills; a
// physical device enumerated through the loader alone left it a null instance, so the plugins got an empty instance
// table and sl.common called a null GetPhysicalDeviceMemoryProperties while it set up. Enumerate through it as well.
VkResult vk_dlssg_enumerate(VkInstance inst, uint32_t *n, VkPhysicalDevice *pd) {
    PFN_vkEnumeratePhysicalDevices f =
        S.loaded > 0 ? (PFN_vkEnumeratePhysicalDevices)S.gipa()(inst, "vkEnumeratePhysicalDevices") : NULL;
    return f ? f(inst, n, pd) : vkEnumeratePhysicalDevices(inst, n, pd);
}

int vk_dlssg_interposed(void) { return S.loaded > 0; }

// Frame generation is off for the run from here on: every later call goes to the loader alone, as when Streamline's
// files are missing, and the settings menu says it is unavailable (vk_dlssg_status).
static void give_up(const char *why) {
    S.loaded = -1, S.fail = DLSSG_UNAVAILABLE;
    rt_log("dlssg: DLSS Frame Generation is off in this run: %s", why);
}

// The interposer's vkCreateDevice asks the device for NVIDIA's extensions (VK_NV_low_latency2 among them) and fails
// without them (VK_ERROR_EXTENSION_NOT_PRESENT). On a laptop with an NVIDIA card and Intel graphics, frame generation
// set to DLSS and the Graphics card setting on the Intel one, that ended every start with "no usable Vulkan device"
// (exit code 70) until nx2navi.ini was edited by hand: four reports on 0.10.1. A device of another vendor is now made
// by the loader, without frame generation, and so is one the interposer refuses for any other reason.
// The order of the attempts is vk_dlssg_device.h's dlssg_device_create (tests/dlssg_device_check.c).
typedef struct { PFN_vkCreateDevice f; VkPhysicalDevice pd; const VkDeviceCreateInfo *ci; VkDevice *out; } DevCreate;
static int create_interposed(void *u) {
    const DevCreate *d = u;
    return (int)d->f(d->pd, d->ci, NULL, d->out);
}
static int create_loader(void *u) {
    const DevCreate *d = u;
    return (int)vkCreateDevice(d->pd, d->ci, NULL, d->out);
}

VkResult vk_dlssg_create_device(VkInstance inst, VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, VkDevice *out) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(pd, &p);
    DevCreate d = {S.loaded > 0 ? (PFN_vkCreateDevice)S.gipa()(inst, "vkCreateDevice") : NULL, pd, ci, out};
    int how, refused;
    VkResult r = (VkResult)dlssg_device_create(d.f != NULL, p.vendorID, create_interposed, create_loader, &d, &how, &refused);
    if (how == DLSSG_DEV_NOT_NVIDIA) {
        give_up("the graphics card in use is not an NVIDIA card (the Graphics card setting, gpu_index)");
    } else if (how == DLSSG_DEV_REFUSED) {
        char why[96];
        snprintf(why, sizeof why, "Streamline could not make the device (%d)", refused);
        give_up(why);
    }
    return r;
}

void vk_dlssg_device_ready(VkPhysicalDevice physical, VkDevice dev) {
    if (S.loaded <= 0) return;
    if (!S.available()) {
        S.fail = DLSSG_UNAVAILABLE;
        if (S.log_unavailable) S.log_unavailable(physical);
        return;
    }
    PFN_vkGetDeviceProcAddr g = S.gdpa();
    S.create_sc = (PFN_vkCreateSwapchainKHR)g(dev, "vkCreateSwapchainKHR");
    S.destroy_sc = (PFN_vkDestroySwapchainKHR)g(dev, "vkDestroySwapchainKHR");
    S.images = (PFN_vkGetSwapchainImagesKHR)g(dev, "vkGetSwapchainImagesKHR");
    S.acquire = (PFN_vkAcquireNextImageKHR)g(dev, "vkAcquireNextImageKHR");
    S.present = (PFN_vkQueuePresentKHR)g(dev, "vkQueuePresentKHR");
    S.active = S.create_sc && S.destroy_sc && S.images && S.acquire && S.present;
    rt_log("dlssg: %s", S.active ? "DLSS Frame Generation on the swapchain" : "the swapchain hooks are missing: off");
    if (S.active) reflex_apply(0);   // no swapchain and no present yet: the mode alone
}

int vk_dlssg_active(void) { return S.active; }

int vk_dlssg_status(void) {
    if (S.active) return DLSSG_RUNS;
    return vk_dlssg_wanted() ? S.fail : DLSSG_NOT_WANTED;
}

int vk_dlssg_multiplier_of(const char *value, int max, const char **why) {
    const char *w = NULL;
    long n = 2;
    if (value && *value) {
        char *end;
        n = strtol(value, &end, 10);
        if (*end == 'x' || *end == 'X') end++;
        if (*end || end == value || n < 2) w = "is not 2, 3 or 4", n = 2;
        else if (n > 4) w = "is above 4, the most there is", n = 4;
    }
    if (max >= 2 && n > max) w = "is above what this GPU and driver allow", n = max;
    if (why) *why = w;
    return (int)n;
}

// Streamline's limit as a multiplier: 2 to 4, or 0 while it has reported none (the first frames). An older DLL
// generates one frame: 2.
static int gpu_multiplier_max(int *old_dll) {
    if (old_dll) *old_dll = !S.frames_max;
    if (!S.frames_max) return 2;
    uint32_t f = S.frames_max();
    return !f ? 0 : f >= 3 ? 4 : (int)f + 1;
}

int vk_dlssg_multiplier_max(int *old_dll) {
    if (old_dll) *old_dll = 0;
    return S.active ? gpu_multiplier_max(old_dll) : 0;
}

int vk_dlssg_multiplier(void) { return S.active ? __atomic_load_n(&S.shown_mult, __ATOMIC_RELAXED) : 1; }

// The multiplier the setting gives here (present thread), clamped to the GPU's limit; one log line each time the value
// or the limit changes and the value is not taken as it is. Until Streamline reports its limit, one generated frame,
// and no log line about the GPU yet.
static int multiplier_wanted(void) {
    static char last[24];
    static int last_max = -1, m = 2;
    const char *value = cfg_value("NX2NAVI_FRAMEGEN_MULTIPLIER");
    int max = gpu_multiplier_max(NULL);
    if (max == last_max && !strncmp(value, last, sizeof last - 1)) return m;
    snprintf(last, sizeof last, "%s", value);
    last_max = max;
    const char *why;
    int n = vk_dlssg_multiplier_of(value, max, &why);
    if (why) rt_log("dlssg: framegen_multiplier = %s %s (up to %dx here): %dx", value, why, max ? max : 4, n);
    m = max ? n : 2;
    return m;
}

// vk_device.c: while Streamline is loaded, every device entry point the runtime fetches goes through its interposer, so
// its plugins see the calls they hook (Reflex's frame timing and DLSS-G's presents). NULL keeps the loader's.
PFN_vkGetDeviceProcAddr vk_dlssg_dpa(void) { return S.loaded > 0 && S.gdpa ? S.gdpa() : NULL; }

// gfx_present.c create_swapchain, under the present and queue locks: the new swapchain has neither the mode nor the
// limit, so both go to Streamline again, the limit as the presenting guest thread last asked for it. One line for each
// of the first 16 swapchains says the limit sent and the one Streamline took: nx2_sl.cpp nx2sl_reflex2 logs only its
// first calls, and the start (the device, the first swapchain, the first hand-over, a movie) uses them up.
VkResult vk_dlssg_create_swapchain(VkDevice d, const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *sc) {
    VkResult r = S.create_sc(d, ci, NULL, sc);
    if (r == VK_SUCCESS) {
        static int made;
        uint32_t want = __atomic_load_n(&S.limit_want, __ATOMIC_ACQUIRE);
        reflex_apply(want);
        if (S.reflex2 && made++ < 16)
            rt_log("dlssg: swapchain %d: Reflex's mode and frame limit sent again (%.3f ms asked, %.3f ms taken; 0 is "
                   "none)", made, want / 1000.0, __atomic_load_n(&S.limit_set, __ATOMIC_ACQUIRE) / 1000.0);
    }
    return r;
}
void vk_dlssg_destroy_swapchain(VkDevice d, VkSwapchainKHR sc) { S.destroy_sc(d, sc, NULL); }
VkResult vk_dlssg_get_images(VkDevice d, VkSwapchainKHR sc, uint32_t *n, VkImage *img) { return S.images(d, sc, n, img); }
VkResult vk_dlssg_acquire(VkDevice d, VkSwapchainKHR sc, uint64_t t, VkSemaphore sem, uint32_t *idx) {
    return S.acquire(d, sc, t, sem, VK_NULL_HANDLE, idx);
}

// One line after the first 600 blits of a game frame and every 3600 after that (600 with NX2NAVI_TRACE=perf), on the
// present thread outside the queue lock (vk_dlssg_frame): who holds the frame limit, and how many frame starts Reflex's
// sleep held and for how long.
static void stats(void) {
    static uint64_t shown, next = 600;
    if (++shown < next) return;
    next = shown + (rt_trace & RT_TRACE_PERF ? 600 : 3600);
    uint64_t n = __atomic_exchange_n(&S.n_hold, 0, __ATOMIC_RELAXED);
    uint64_t ns = __atomic_exchange_n(&S.hold_ns, 0, __ATOMIC_RELAXED);
    uint64_t un = __atomic_exchange_n(&S.n_unheld, 0, __ATOMIC_RELAXED);
    uint32_t us = __atomic_load_n(&S.limit_set, __ATOMIC_ACQUIRE);
    char limit[96];
    if (us) snprintf(limit, sizeof limit, "Reflex holds frame starts to %.3f ms", us / 1000.0);
    else snprintf(limit, sizeof limit, "the game's own limiter holds the frame limit");
    rt_log("dlssg: present %llu: %s; %llu frame starts held by Reflex's sleep, %.2f ms on average; %llu not held",
           (unsigned long long)shown, limit, (unsigned long long)n, n ? (double)ns / (double)n / 1e6 : 0.0,
           (unsigned long long)un);
}

VkResult vk_dlssg_present(VkQueue q, const VkPresentInfoKHR *pi) {
    S.marker(0, S.cur);
    VkResult r = S.present(q, pi);
    S.marker(1, S.cur);
    __atomic_store_n(&S.hosted_ns, kern_now_ns(), __ATOMIC_RELEASE);
    return r;
}

// Turning generation on or off is an slDLSSGSetOptions call and Streamline's own rebuild, and it can be seen: around a
// menu, a fade or a swapchain change the depth and motion-vector views come and go for a frame or two. Following them
// frame by frame switched generation off and on dozens of times a second (a run logged 32 changes, in pairs, a frame
// apart), so a change waits until the new state has held for FG_SETTLE_FRAMES frames.
#define FG_SETTLE_FRAMES 12

// Reflex holds the frame's start until the time the driver computes from the measured render and queue times, and no
// sooner than its frame limit after the frame before (vk_dlssg_pace), so the game's CPU work ends about when the GPU is
// free for it; then the frame's first markers. Once a frame: the game may acquire again before it presents. No hold
// while no host present went out for DLSSG_HOLD_IDLE_NS (the window minimized: gfx_present.c skips the host presents
// while the game runs on): the driver then has no present to time the frame by, and the sleep would wait out
// Streamline's half-second timeout each frame. The guest thread gave its core up for the acquire (NVN_BLOCKING). The
// sleep counts as the pacer's wait (PERF_PACE) only while Reflex holds a frame limit (limit_set), whose place it then
// takes, on whichever guest thread acquires (perf_end_any), as the acquire's own waits do. Without one (reflex_limit 0,
// an older DLL, a movie, a refused limit, frames that showed Reflex does not hold it) the sleep is only the driver's
// wait for the GPU while the pacer waits as well, so it stays out of PERF_PACE, as before Reflex held a frame limit:
// counted there, GPU-bound frames read as paced in the perf windows.
void vk_dlssg_frame_start(void) {
    if (!S.active) return;
    uint32_t f = __atomic_load_n(&S.game, __ATOMIC_RELAXED) + 1;
    if (f == S.started) return;
    S.started = f;
    uint64_t hosted = __atomic_load_n(&S.hosted_ns, __ATOMIC_ACQUIRE), t0 = kern_now_ns();   // in this order: t0 >= hosted
    int hold = hosted && t0 - hosted < DLSSG_HOLD_IDLE_NS;
    int paced = hold && __atomic_load_n(&S.limit_set, __ATOMIC_ACQUIRE) != 0;
    uint64_t p0 = paced ? perf_begin() : 0;
    S.sleep(f, hold);
    if (!hold) {
        __atomic_add_fetch(&S.n_unheld, 1, __ATOMIC_RELAXED);
        return;
    }
    if (paced) perf_end_any(PERF_PACE, p0);
    __atomic_add_fetch(&S.hold_ns, kern_now_ns() - t0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&S.n_hold, 1, __ATOMIC_RELAXED);
}

// Who holds the frame limit, as a log line when it changes (the first 40). why: the reason the pacer holds it.
static void pace_say(int reflex, uint32_t us, const char *why) {
    static int lines;
    if (S.pace_said == reflex || lines >= 40) return;
    S.pace_said = reflex, lines++;
    if (reflex)
        rt_log("frame rate: Reflex holds the frame limit where each frame starts (%.3f ms%s); the game's own limiter "
               "keeps only its ceiling", us / 1000.0, S.pace.shown ? ", the frames on screen" : "");
    else rt_log("frame rate: the game's own limiter (DLSS Frame Generation: %s)", why);
}

int vk_dlssg_pace(uint64_t period_ns) {
    if (!S.active) return 0;
    uint64_t hosted = __atomic_load_n(&S.hosted_ns, __ATOMIC_ACQUIRE), now = kern_now_ns();
    int allowed = limit_allowed(), mult = vk_dlssg_multiplier(), found;
    uint32_t applied = __atomic_load_n(&S.limit_set, __ATOMIC_ACQUIRE), want;
    int held = dlssg_pace_step(&S.pace, now, period_ns, mult, hosted, allowed, applied, &want, &found);
    __atomic_store_n(&S.limit_want, want, __ATOMIC_RELEASE);   // the present thread hands it over (vk_dlssg_frame)
    double ms = (double)period_ns / 1e6;
    if (found == DLSSG_PACE_FAST)
        rt_log("dlssg: Reflex does not hold the frame limit (over half of %d frames came sooner than three quarters of "
               "%.2f ms): the game's own limiter holds it for the rest of the run", DLSSG_PACE_WINDOW, ms);
    else if (found == DLSSG_PACE_SHOWN)
        rt_log("dlssg: Reflex held the frames on screen to the limit, not the frames the game draws (over half of %d "
               "came about %d times %.2f ms apart): it is given %.2f ms over the multiplier from now on",
               DLSSG_PACE_WINDOW, mult, ms, ms);
    else if (found == DLSSG_PACE_SLOW)
        rt_log("dlssg: the frames still came about %d times %.2f ms apart: the game's own limiter holds the limit for the "
               "rest of the run", mult, ms);
    if (held) pace_say(1, want, NULL);
    else if (!want)   // (a limit Streamline has yet to take is no change: the present thread hands it over next)
        pace_say(0, 0, !allowed     ? (!S.reflex2 ? "nx2navi_dlssg.dll is older than Reflex's frame limit"
                                       : __atomic_load_n(&S.limit_refused, __ATOMIC_ACQUIRE) ? "Streamline refused the limit"
                                                                                             : "NX2NAVI_REFLEX_LIMIT=0")
                       : S.pace.bad ? "Reflex did not hold the limit"
                       : !period_ns ? "a movie plays, or no limit"
                       : hosted     ? "no host present for 200 ms"
                                    : "no host present yet");
    return held;
}

void vk_dlssg_game_present(void) { __atomic_add_fetch(&S.game, 1, __ATOMIC_RELAXED); }

void vk_dlssg_submitted(uint64_t frame) {
    if (S.active) S.submit_end((uint32_t)frame);
}

void vk_dlssg_present_frame(uint64_t frame) { S.cur = (uint32_t)frame; }

uint64_t vk_dlssg_shown(void) { return S.active ? S.shown() : 0; }

void vk_dlssg_frame(VkCommandBuffer cb, VkExtent2D ext) {
    if (!S.active) return;
    VkFfxImage d, m;
    VkImageView dv, mvv;
    VkDlaaCamera c;
    float jx = 0, jy = 0;
    int reset = 0;
    int have = vk_dlaa_fg_inputs(&d, &m, &jx, &jy, &reset) && vk_dlaa_fg_views(&dv, &mvv) && vk_dlaa_fg_camera(&c);
    int want = have && !menu_is_open();
    if (want == S.mode) S.pending = 0;
    else if (++S.pending >= FG_SETTLE_FRAMES) { S.mode = want; S.pending = 0; }
    else want = S.mode;   // hold the state it has until the new one stays
    int on = want, mult = multiplier_wanted();
    __atomic_store_n(&S.shown_mult, on ? mult : 1, __ATOMIC_RELAXED);   // the pacer's divisor (frame_rate.c)
    // Reflex's frame limit as the presenting guest thread last asked for it (vk_dlssg_pace), before this present
    uint32_t limit = __atomic_load_n(&S.limit_want, __ATOMIC_ACQUIRE);
    if (S.reflex2 && limit != __atomic_load_n(&S.limit_set, __ATOMIC_ACQUIRE)) reflex_apply(limit);
    stats();
    Nx2SlImage zd = {d.image, dv, d.format, d.width, d.height, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT};
    Nx2SlImage zm = {m.image, mvv, m.format, m.width, m.height, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT};
    Nx2SlCamera k;
    memset(&k, 0, sizeof k);
    if (have) {
        memcpy(k.view_to_clip, c.view_to_clip, 64), memcpy(k.clip_to_view, c.clip_to_view, 64);
        memcpy(k.clip_to_prev_clip, c.clip_to_prev_clip, 64), memcpy(k.prev_clip_to_clip, c.prev_clip_to_clip, 64);
        memcpy(k.pos, c.pos, 12), memcpy(k.up, c.up, 12), memcpy(k.right, c.right, 12), memcpy(k.fwd, c.fwd, 12);
        // the frame's own near and far planes (vk_dlaa.c proj_planes); nx2navi_dlssg.dll tells Streamline the depth is
        // not inverted, which holds for every projection the game has shown (0 near, 1 far)
        k.znear = c.znear, k.zfar = c.zfar, k.fov_y = c.fov_y, k.aspect = c.aspect;
        static int told;
        if (c.inverted && !told++) rt_log("dlssg: the scene's depth is inverted, which Streamline is not told");
        k.jitter[0] = jx, k.jitter[1] = jy;
        // render pixels: the scene's size, which is the output's or smaller (an upscaling mode, or the game's own scale
        // without an upscaler); Streamline takes the depth and the vectors at their own size and the output at ext
        k.mv_scale[0] = 1.0f / (float)d.width, k.mv_scale[1] = 1.0f / (float)d.height;
        k.reset = reset;
    }
    // the frames to generate between two rendered ones: the multiplier less the rendered one, 0 off
    S.frame(cb, have ? &zd : NULL, have ? &zm : NULL, NULL, have ? &k : NULL, ext.width, ext.height, on ? mult - 1 : 0, S.cur);
}

void vk_dlssg_release(void) {
    if (S.loaded > 0 && S.shutdown) S.shutdown();
    S.active = 0;
    __atomic_store_n(&S.limit_set, 0, __ATOMIC_RELEASE);   // a device made again starts without a limit
}
