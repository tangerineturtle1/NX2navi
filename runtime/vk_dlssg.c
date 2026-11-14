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
// line. With adaptive selected, frame_rate.c treats the limit as the displayed rate and divides the game rate by the
// multiplier in effect; fixed multipliers retain the usual frame-limit behavior.
//
// DLSS-G needs Reflex, whose low-latency mode paces the game (NX2NAVI_REFLEX, default 1; 0 off, 2 on with boost): the
// game's frame sleeps in Reflex where it starts (its acquire of a window image), and each frame carries the latency
// markers from that start to the host present, all with the frame's number, the count of the game's presents up to it.
// The mode is the swapchain's, so it is set again after each one (vk_dlssg_create_swapchain).
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_dlssg.h"
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
    uint32_t game;   // the game's presents (vk_dlssg_game_present)
    uint32_t cur;    // the frame the present thread blits and presents next (vk_dlssg_present_frame)
    uint32_t started;      // the last frame vk_dlssg_frame_start started
    uint64_t hosted_ns;    // kern_now_ns after the last host present
    PFN_nx2sl_shutdown shutdown;
    PFN_vkCreateSwapchainKHR create_sc;
    PFN_vkDestroySwapchainKHR destroy_sc;
    PFN_vkGetSwapchainImagesKHR images;
    PFN_vkAcquireNextImageKHR acquire;
    PFN_vkQueuePresentKHR present;
} S = {.wanted = -1, .fail = DLSSG_NO_FILES, .shown_mult = 1};

// ---------- Adaptive frame generation
// The game's thread measures each frame's work (nvn_api_obj.c nvnWindowAcquireTexture) and vk_dlssg_adaptive_sample
// keeps the last ADAPT_WIN of them. With the display's goal G frames a second and multiplier m the game has m / G
// seconds a frame.
//   more generation (m + 1): a quarter of the last ADAPT_LATE_WIN frames took longer than they have.
//   less generation (m - 1): all but ADAPT_FAST_MISS of the last ADAPT_WIN frames fit in ADAPT_FAST of what they would
//                            have then, so the game keeps a fifth of the frame spare after the change.
// One long frame (a shader or a load) is one sample and changes nothing. Each change is an slDLSSGSetOptions call and
// can be seen, so a change waits ADAPT_HOLD_NS after the last one, and a change to less generation that had to be
// taken back within ADAPT_FAIL_NS doubles the wait before the next try (10 s, 20 s, ... 160 s): a place that sits at
// the edge settles on generation instead of switching back and forth. A minute without taking one back clears that.
#define ADAPT_WIN 120
#define ADAPT_LATE_WIN 60
#define ADAPT_LATE_PART 4            // one in this many of the last ADAPT_LATE_WIN frames late: more generation
#define ADAPT_LATE 1.03              // a frame is late past this share of its time
#define ADAPT_FAST 0.80              // less generation needs frames within this share of the shorter time
#define ADAPT_FAST_MISS 3            // frames of ADAPT_WIN that may be slower than that
#define ADAPT_HOLD_NS 3000000000ull
#define ADAPT_FAIL_NS 20000000000ull
#define ADAPT_CALM_NS 60000000000ull
static struct {
    float work[ADAPT_WIN];   // seconds of work of the newest frames, a ring
    int n, at;               // samples held, and where the next one goes
    int mult;                // the multiplier adaptive wants: 1 (no generated frames) to the GPU's limit
    int fails;               // changes to less generation taken back soon after: each doubles the next wait
    uint64_t changed_ns, down_ns;   // the last change, and the last change to less generation
    int logs;
} A = {.mult = 2};

static int adaptive_set(void) { return !strcmp(cfg_value("NX2NAVI_FRAMEGEN_MULTIPLIER"), "adaptive"); }

static void adaptive_change(int to, uint64_t now, double goal_hz, double worst) {
    if (A.logs++ < 400)
        rt_log("dlssg: adaptive: %dx -> %dx (the %g fps goal gives the game %.1f ms a frame at %dx; newest frames up to %.1f ms)",
               A.mult, to, goal_hz, 1e3 * to / goal_hz, to, 1e3 * worst);
    if (to < A.mult) A.down_ns = now;
    A.changed_ns = now;
    A.n = A.at = 0;   // the frames before the change say nothing about the pace after it
    __atomic_store_n(&A.mult, to, __ATOMIC_RELAXED);
}

void vk_dlssg_adaptive_sample(double work_s, double goal_hz) {
    if (!S.active || !adaptive_set()) return;
    int max = 2;   // until Streamline says: one generated frame
    uint32_t gen_max = S.frames_max ? S.frames_max() : 0;
    if (gen_max) max = gen_max >= 3 ? 4 : (int)gen_max + 1;
    if (A.mult > max) __atomic_store_n(&A.mult, max, __ATOMIC_RELAXED);
    // A movie, an unlimited rate, the menu, or a pending multiplier change supplies no useful sample.
    if (goal_hz <= 0 || menu_is_open() || __atomic_load_n(&S.shown_mult, __ATOMIC_RELAXED) != A.mult) {
        A.n = A.at = 0;
        return;
    }
    A.work[A.at] = (float)work_s;
    A.at = (A.at + 1) % ADAPT_WIN;
    if (A.n < ADAPT_WIN) A.n++;
    uint64_t now = kern_now_ns();
    if (now - A.changed_ns < ADAPT_HOLD_NS) return;
    if (A.fails && A.down_ns && now - A.down_ns > ADAPT_CALM_NS && A.mult < max) A.fails = 0;
    double have = A.mult / goal_hz, worst = 0;
    // more generation: the newest frames do not fit
    if (A.mult < max && A.n >= ADAPT_LATE_WIN) {
        int late = 0;
        for (int i = 1; i <= ADAPT_LATE_WIN; i++) {
            double w = A.work[(A.at + ADAPT_WIN - i) % ADAPT_WIN];
            if (w > have * ADAPT_LATE) late++;
            if (w > worst) worst = w;
        }
        if (late * ADAPT_LATE_PART >= ADAPT_LATE_WIN) {
            if (A.down_ns && now - A.down_ns < ADAPT_FAIL_NS && A.fails < 5) A.fails++;
            adaptive_change(A.mult + 1, now, goal_hz, worst);
            return;
        }
    }
    // less generation: the window's frames would fit with room to spare, and the wait after a failed try is over
    if (A.mult > 1 && A.n >= ADAPT_WIN) {
        uint64_t wait = A.fails ? 10000000000ull << (A.fails - 1) : ADAPT_HOLD_NS;
        if (now - A.changed_ns < wait) return;
        double fit = (A.mult - 1) / goal_hz * ADAPT_FAST;
        int miss = 0;
        worst = 0;
        for (int i = 0; i < ADAPT_WIN; i++) {
            if (A.work[i] > fit) miss++;
            else if (A.work[i] > worst) worst = A.work[i];
        }
        if (miss <= ADAPT_FAST_MISS) adaptive_change(A.mult - 1, now, goal_hz, worst);
    }
}


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

VkResult vk_dlssg_create_device(VkInstance inst, VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, VkDevice *out) {
    PFN_vkCreateDevice f = S.loaded > 0 ? (PFN_vkCreateDevice)S.gipa()(inst, "vkCreateDevice") : NULL;
    return f ? f(pd, ci, NULL, out) : vkCreateDevice(pd, ci, NULL, out);
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
    if (S.active) S.reflex(reflex_mode());
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
    if (!strcmp(value, "adaptive")) {
        int current = __atomic_load_n(&A.mult, __ATOMIC_RELAXED), top = max ? max : 2;
        return current > top ? top : current;
    }
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

VkResult vk_dlssg_create_swapchain(VkDevice d, const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *sc) {
    VkResult r = S.create_sc(d, ci, NULL, sc);
    if (r == VK_SUCCESS) S.reflex(reflex_mode());
    return r;
}
void vk_dlssg_destroy_swapchain(VkDevice d, VkSwapchainKHR sc) { S.destroy_sc(d, sc, NULL); }
VkResult vk_dlssg_get_images(VkDevice d, VkSwapchainKHR sc, uint32_t *n, VkImage *img) { return S.images(d, sc, n, img); }
VkResult vk_dlssg_acquire(VkDevice d, VkSwapchainKHR sc, uint64_t t, VkSemaphore sem, uint32_t *idx) {
    return S.acquire(d, sc, t, sem, VK_NULL_HANDLE, idx);
}

VkResult vk_dlssg_present(VkQueue q, const VkPresentInfoKHR *pi) {
    S.marker(0, S.cur);
    VkResult r = S.present(q, pi);
    S.marker(1, S.cur);
    __atomic_store_n(&S.hosted_ns, kern_now_ns(), __ATOMIC_RELAXED);
    return r;
}

// Reflex holds the frame's start until the time the driver computes from the measured render and queue times, so the
// game's CPU work ends about when the GPU is free for it; then the frame's first markers. Once a frame: the game may
// acquire again before it presents. No hold while no host present went out for HOLD_IDLE_NS (the window minimized:
// gfx_present.c skips the host presents while the game runs on): the driver then has no present to time the frame by,
// and the sleep would wait out Streamline's half-second timeout each frame.
#define HOLD_IDLE_NS 200000000ull

// Turning generation on or off is an slDLSSGSetOptions call and Streamline's own rebuild, and it can be seen: around a
// menu, a fade or a swapchain change the depth and motion-vector views come and go for a frame or two. Following them
// frame by frame switched generation off and on dozens of times a second (a run logged 32 changes, in pairs, a frame
// apart), so a change waits until the new state has held for FG_SETTLE_FRAMES frames.
#define FG_SETTLE_FRAMES 12

void vk_dlssg_frame_start(void) {
    if (!S.active) return;
    uint32_t f = __atomic_load_n(&S.game, __ATOMIC_RELAXED) + 1;
    if (f == S.started) return;
    S.started = f;
    uint64_t hosted = __atomic_load_n(&S.hosted_ns, __ATOMIC_RELAXED);
    S.sleep(f, hosted && kern_now_ns() - hosted < HOLD_IDLE_NS);
}

void vk_dlssg_game_present(void) { __atomic_add_fetch(&S.game, 1, __ATOMIC_RELAXED); }

void vk_dlssg_submitted(uint64_t frame) {
    if (S.active) S.submit_end((uint32_t)frame);
}

void vk_dlssg_present_frame(uint64_t frame) { S.cur = (uint32_t)frame; }

uint64_t vk_dlssg_shown(void) { return S.active ? S.shown() : 0; }

void vk_dlssg_frame(VkCommandBuffer cb, VkExtent2D ext) {
    if (!S.active) return;
    VkFfxImage d = {0}, m = {0};
    VkImageView dv = VK_NULL_HANDLE, mvv = VK_NULL_HANDLE;
    VkDlaaCamera c;
    float jx = 0, jy = 0;
    int reset = 0;
    int have = vk_dlaa_fg_inputs(&d, &m, &jx, &jy, &reset) && vk_dlaa_fg_views(&dv, &mvv) && vk_dlaa_fg_camera(&c);
    int want = have && !menu_is_open();
    if (want == S.mode) S.pending = 0;
    else if (++S.pending >= FG_SETTLE_FRAMES) { S.mode = want; S.pending = 0; }
    else want = S.mode;   // hold the state it has until the new one stays
    int mult = multiplier_wanted(), on = want && mult > 1;
    __atomic_store_n(&S.shown_mult, on ? mult : 1, __ATOMIC_RELAXED);   // the pacer's divisor (frame_rate.c)
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
}
