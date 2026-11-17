// VR through OpenXR (vk_xr.h), NX2NAVI_VR=1 at start. Stage one, the theater: the picture the window shows, the menu
// over it, on a screen floating in front of the headset, 2.5 m away: NX2NAVI_VR_SCREEN_SIZE degrees of the view wide,
// curved around the head (NX2NAVI_VR_SCREEN_CURVE, a cylinder layer where the runtime has one) or flat.
//
// The loader is opened at run time (libopenxr_loader.so.1, openxr_loader.dll), so a PC without it starts as before.
// XR_KHR_vulkan_enable: the runtime names the Vulkan instance and device extensions it needs and the GPU it draws with;
// vk_device.c adds the extensions, prefers that GPU and makes the device with one more queue of the graphics family for
// VR's copies, so the session's thread never waits on the game's queue lock.
//
// Each present copies the window's image into one of three images of VR's own (the boxes, vk_xr_capture), and the
// present's submit signals a timeline semaphore with the copy's number. The session's thread copies the newest box into
// the OpenXR swapchain on VR's queue, its submit waiting for that number, and shows it as a quad in the LOCAL space. A
// box the thread reads is never written, and neither is the newest, so three are enough; the headset's frames go on at
// its own rate, the last picture shown again until a newer one is there.
#define XR_NO_PROTOTYPES
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>

#include "third_party/openxr/include/openxr/openxr.h"
#include "third_party/openxr/include/openxr/openxr_platform.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "ui/menu.h"
#include "vk_xr.h"
#include "vr_game.h"
#include "vr_wheel.h"
#include "vr_model.h"
#include "xr_aim_spv.h"
#include "xr_depth_keep_spv.h"
#include "xr_depth_spv.h"
#include "xr_hand_blit_fs_spv.h"
#include "xr_hand_blit_vs_spv.h"
#include "xr_hand_fs_spv.h"
#include "xr_hand_mix_spv.h"
#include "xr_hand_vs_spv.h"
#include "xr_hud_spv.h"
int vk_dlaa_vr_planes(float planes[2]);                     // vk_dlaa.h
void rec_drain(void);                                        // vk_rec.h
void gfx_fit_rect(int cw, int ch, int dw, int dh, int out[4]);   // vk.h
}

namespace {

#define XR_FUNCS(X)                                                                                                    \
    X(xrDestroyInstance) X(xrGetInstanceProperties) X(xrGetSystem) X(xrGetSystemProperties) X(xrPollEvent)            \
    X(xrCreateSession) X(xrBeginSession) X(xrEndSession) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame)                 \
    X(xrCreateReferenceSpace) X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrEnumerateSwapchainImages)       \
    X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) X(xrGetVulkanInstanceExtensionsKHR) \
    X(xrGetVulkanDeviceExtensionsKHR) X(xrGetVulkanGraphicsDeviceKHR) X(xrGetVulkanGraphicsRequirementsKHR)          \
    X(xrLocateViews) X(xrLocateSpace) X(xrStringToPath) X(xrPathToString) X(xrCreateActionSet) X(xrCreateAction)   \
    X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets) X(xrSyncActions) X(xrGetActionStateBoolean)  \
    X(xrGetActionStateFloat) X(xrGetActionStateVector2f) X(xrCreateActionSpace) X(xrGetCurrentInteractionProfile)

// Tangents of a lens's four edges (left and down negative), as OpenXR's angles are
struct Lens {
    float l = -1, r = 1, u = 1, d = -1;
};
struct Pose {
    float q[4] = {0, 0, 0, 1};   // x, y, z, w
    float p[3] = {0, 0, 0};
};
// What a picture was drawn from: the head's pose in the LOCAL space and the lens; tracked false: the game's own view,
// shown on the screen
struct Drawn {
    bool tracked = false;
    bool hud = false;      // the HUD apart: the box holds the scene without it, the HUD box its panel
    bool stereo = false;   // depth stereo: the box holds the left eye's picture, the right box the right eye's
    bool menu = false;     // the game paused (its menus): the boxes hold the world as it last was, the HUD box the
                           // game's picture, its menu, shown on the HUD's panel
    Pose pose;             // the head's, between the eyes
    Lens lens;
    float scale = 1.0f;    // the lens the game drew with over the one shown (NX2NAVI_VR_VIEW_SCALE)
    float ipd = 0.064f;    // the eyes' distance, metres
    int eye = -1;          // Stereo Balanced (eyes in turn): the eye the picture was drawn from (0 left, 1 right)
    Pose eye_pose[2];      // with stereo then: each eye's picture's own pose (stereo's boxes hold the two latest)
    // where the LOCAL space sat in the game's world for this picture: world = base + right * x + up * y + back * z,
    // right (bz, 0, -bx), back (bx, 0, bz) (Balanced: the older eye's picture is put in the newer one's LOCAL space)
    double base[3]{}, bx = 0, bz = 1;
    bool aim = false;      // Link aims: the game's camera's ray (aim_o, aim_d) in the head's view (x right, y up, z back)
    float aim_o[3]{}, aim_d[3]{};
};

// after one of VR's wheels closes, the game's HUD stays off the HUD's panel this long: the game's own quick select, held
// while a wheel is open, is going away
constexpr uint64_t wheel_hide_ns = 400000000ull;

struct Xr {
    bool tried = false, ok = false;
    PFN_xrGetInstanceProcAddr gipa = nullptr;
#define XR_DECLARE(f) PFN_##f f = nullptr;
    XR_FUNCS(XR_DECLARE)
#undef XR_DECLARE
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    std::string instance_exts, device_exts;   // the runtime's lists, split in place: the pointers below point into them
    std::vector<const char *> instance_list, device_list;
    const float screen_distance = 2.5f;
    bool cylinder = false;   // the runtime has XR_KHR_composition_layer_cylinder (enabled)
    // the device (vk_xr_device_ready)
    VkInstance vk_instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t family = 0, queue_index = UINT32_MAX;
    VkQueue queue = VK_NULL_HANDLE;
    VkSemaphore timeline = VK_NULL_HANDLE;
    // the swapchain's size (the window's, from the first capture) and order; ready once the session's thread made it
    std::atomic<uint32_t> want_w{0}, want_h{0};
    std::atomic<bool> ready{false}, rgba{true};
    uint32_t width = 0, height = 0;
    // the boxes (mu): the present's thread writes one, the session's thread reads the newest
    std::mutex mu;
    VkImage boxes[3]{}, boxes_r[3]{};   // (the right eye's, depth stereo)
    uint64_t box_value[3]{};
    int latest = -1, reading = -1, next_slot = 0, pending = -1;
    bool boxes_made = false, boxes_failed = false;
    uint64_t next_value = 0;   // the present's thread alone
    Drawn box_drawn[3];
    // the head (the session's thread writes, the game's threads read)
    std::mutex head_mu;
    bool have_head = false;
    Pose head;
    Lens eyes;   // both eyes' lenses together
    float ipd = 0.064f;
    std::atomic<bool> running{false};
    // the view now (each present): the head's on the game's camera, or the screen (NX2NAVI_VR_VIEW, the menu open)
    std::atomic<bool> head_view{false};
    std::atomic<float> picture_ratio{16.0f / 9.0f};
    // this frame's (frame_mu): taken at the gameplay camera's update (vk_xr_camera), its lens given at the angle's
    // load (vk_xr_lens), kept for the next picture (vk_xr_capture)
    std::mutex frame_mu;
    bool frame_on = false;
    Drawn drawn;
    float render_scale = 1.0f;   // the lens the game draws with over the one shown (NX2NAVI_VR_VIEW_SCALE)
    bool aim_on = false;         // vr_game.c vk_xr_aim: Link aims, along the game's own camera's ray (world)
    // pointed aim (vr_game.c vk_xr_aim_freeze): the view's base yaw held where it was while the controller steers the
    // game's aim (else the view would turn with the aim it chases); the yaw now (cur) for the controller's ray
    bool aim_freeze = false, frozen_ok = false;
    int turn = 0;   // Stereo Balanced: the eye the next camera update draws from (frame_mu)
    // the last camera updates' pictures to be (frame_mu), with the camera's place in the world each gave, and the place
    // the last scene was drawn from (vk_xr_scene_camera): a picture is matched to its own update by the nearest
    struct Given {
        Drawn d;
        double at[3];
        bool used;
    } given[6];
    int given_next = 0;
    double scene_at[3]{};
    bool scene_new = false;
    double frozen_bx = 0, frozen_bz = 1, cur_bx = 0, cur_bz = 1;
    double aim_o[3]{}, aim_d[3]{};
    std::atomic<uint32_t> camera_given{0}, lens_given{0}, tracked_pictures{0};
    // the gameplay camera's updates since the last picture, and the pictures in a row with none: a paused game (the
    // inventory) draws flat pictures while the camera stands still, and those go on the screen
    std::atomic<uint32_t> updates{0};
    int stale = 0;   // the present's thread alone
    // the HUD's panel (NX2NAVI_VR_HUD=panel). The game draws its HUD over its upscaled scene in one float target; the
    // scene as the upscaler left it is kept (vk_xr_scene_image, the replay's stream), and at the present, when the
    // target holds the HUD too, shaders/xr_hud.comp makes the scene without the HUD and the HUD's panel from the two
    std::mutex hud_mu;
    VkImage hud_target = VK_NULL_HANDLE, scene = VK_NULL_HANDLE, scene8 = VK_NULL_HANDLE, panel8 = VK_NULL_HANDLE;
    VkImageView target_view = VK_NULL_HANDLE, scene_view = VK_NULL_HANDLE, scene8_view = VK_NULL_HANDLE, panel8_view = VK_NULL_HANDLE;
    VkImage target_viewed = VK_NULL_HANDLE;   // the target target_view is of
    uint32_t hud_w = 0, hud_h = 0;
    VkFormat hud_format = VK_FORMAT_UNDEFINED;
    uint64_t kept = 0, used = 0;   // scenes kept, and the last one a panel was made from
    VkDescriptorSetLayout hud_set = VK_NULL_HANDLE;
    VkPipelineLayout hud_layout = VK_NULL_HANDLE;
    VkPipeline hud_pipe = VK_NULL_HANDLE;
    VkSampler nearest = VK_NULL_HANDLE;
    PFN_vkCmdPushDescriptorSetKHR push_set = nullptr;
    bool hud_tried = false;
    VkImage hud_boxes[3]{};
    std::atomic<uint32_t> panels{0};
    std::atomic<uint32_t> pic_w{0}, pic_h{0};   // the game's presented picture's size: the HUD's target is the scene upscaled to it
    // depth stereo (NX2NAVI_VR_DEPTH, hud_mu): shaders/xr_depth.comp makes each eye's picture from the scene the HUD's
    // shader leaves and the scene's depth kept by vk_xr_keep_depth
    VkImage eye8[2]{};
    VkImageView eye8_view[2]{};
    uint32_t eye8_w = 0, eye8_h = 0;
    // the scene's depth as it was when the scene was drawn (vk_xr_keep_depth): the game reuses its target for smaller
    // passes before the present
    VkImage depth = VK_NULL_HANDLE;   // R32F
    VkImageView depth_view = VK_NULL_HANDLE;
    uint32_t depth_w = 0, depth_h = 0;
    uint64_t depth_kept = 0;
    VkBuffer rows = VK_NULL_HANDLE;   // each depth row's least then greatest depth (shaders/xr_depth_keep.comp)
    VkDescriptorSetLayout keep_set = VK_NULL_HANDLE;
    VkPipelineLayout keep_layout = VK_NULL_HANDLE;
    VkPipeline keep_pipe = VK_NULL_HANDLE;
    VkSampler keep_sampler = VK_NULL_HANDLE;
    PFN_vkCmdPushDescriptorSetKHR keep_push = nullptr;
    bool keep_tried = false;
    VkDescriptorSetLayout depth_set = VK_NULL_HANDLE;
    VkPipelineLayout depth_layout = VK_NULL_HANDLE;
    VkPipeline depth_pipe = VK_NULL_HANDLE;
    VkSampler linear = VK_NULL_HANDLE;
    bool depth_tried = false;
    // VR's hands (NX2NAVI_VR_PLAY=motion, hud_mu): the game's models of the sword and the shield drawn at the hands
    // (shaders/xr_hand_vs.vert, xr_hand_fs.frag) and put into scene8 and the kept depth (xr_hand_mix.comp)
    // each model read once, by its actor's name: state 1 reading (cpu the model worker's until then), 2 read, 3 none
    struct HandItem {
        char name[96] = "";
        std::atomic<int> state{0};
        VrModel cpu{};
        VkBuffer vb = VK_NULL_HANDLE, ib = VK_NULL_HANDLE, staging[VR_MODEL_PARTS]{};
        VkImage tex[VR_MODEL_PARTS]{};
        VkImageView tex_view[VR_MODEL_PARTS]{};
        bool uploaded = false;
    } items[32];
    std::mutex gear_mu;
    char gear[3][96]{};   // vr_game.c: the sword and the shield equipped (vk_xr_gear), the item used (vk_xr_hand_item)
    VkDescriptorSetLayout hand_set = VK_NULL_HANDLE, mix_set = VK_NULL_HANDLE;
    VkPipelineLayout hand_layout = VK_NULL_HANDLE, mix_layout = VK_NULL_HANDLE;
    VkPipeline hand_pipe = VK_NULL_HANDLE, mix_pipe = VK_NULL_HANDLE;
    VkImage hand_color = VK_NULL_HANDLE, hand_dist = VK_NULL_HANDLE, hand_depth = VK_NULL_HANDLE;
    VkImageView hand_color_view = VK_NULL_HANDLE, hand_dist_view = VK_NULL_HANDLE, hand_depth_view = VK_NULL_HANDLE;
    uint32_t hand_w = 0, hand_h = 0;
    VkSampler hand_sampler = VK_NULL_HANDLE;
    bool hand_tried = false;
    // the hands on the desktop: an overlay of them from the head (hud_mu) blended onto the window (hands_onto_window)
    VkImage win_overlay = VK_NULL_HANDLE;
    VkImageView win_overlay_view = VK_NULL_HANDLE;
    uint32_t win_overlay_w = 0, win_overlay_h = 0;
    bool win_hands = false;   // the overlay holds hands this present
    // Stereo Balanced (hud_mu): each eye's latest picture and its pose, and the desktop's left eye kept for the right's
    // presents (the window's picture region, in the window's format)
    // ([2]: Accurate's left eye's picture, waiting for its right one, both shown together)
    VkImage acc8[3]{};
    VkImageView acc8_view[3]{};
    uint32_t acc_w = 0, acc_h = 0;
    Pose acc_pose[3];
    double acc_base[3][3]{}, acc_bx[3]{}, acc_bz[3]{1, 1, 1};
    bool acc_have[3]{};
    Pose pair_pose[2];   // Accurate: the eyes' poses of the pair shown now
    bool pair_ok = false;
    VkImage win_left = VK_NULL_HANDLE;
    uint32_t win_left_w = 0, win_left_h = 0;
    VkFormat win_left_format = VK_FORMAT_UNDEFINED;
    bool win_left_have = false;
    VkDescriptorSetLayout blit_set = VK_NULL_HANDLE;
    VkPipelineLayout blit_layout = VK_NULL_HANDLE;
    VkPipeline blit_pipe = VK_NULL_HANDLE;
    VkFormat blit_format = VK_FORMAT_UNDEFINED;
    VkImage win_image[8]{};
    VkImageView win_view[8]{};
    int win_next = 0;
    // the aiming reticle (shaders/xr_aim.comp, hud_mu) drawn into scene8 where the aim lands
    VkDescriptorSetLayout aim_set = VK_NULL_HANDLE;
    VkPipelineLayout aim_layout = VK_NULL_HANDLE;
    VkPipeline aim_pipe = VK_NULL_HANDLE;
    bool aim_tried = false;
    std::atomic<uint32_t> stereo_pictures{0}, stereo_missed{0}, menu_pictures{0};
    // the hand controllers (hands_mu; the session's thread writes, the game's input reads): their buttons as the pad's
    // (input_map.h bits) and sticks, and the grips' poses in the LOCAL space
    std::mutex hands_mu;
    uint64_t hand_buttons = 0;
    float hand_sticks[2][2]{};   // left, right; x right, y up
    Pose grip[2];
    Pose aim_pose[2];   // the controllers' aim poses (their pointing rays, -z), as their own pointers use
    bool aim_ok[2]{};
    bool grip_ok[2]{};
    uint64_t hands_at = 0;   // when they were last read (plat_now_ns), 0 never
    int wheel_open = 0;    // a trigger held: 1 the left's D-pad wheel, 2 the right's item wheel (ui/menu.cpp draws them)
    int wheel_slot = -1;   // the slot its hand's stick points at (0 up, clockwise), -1 none
    int used_kind = 0, used_slot = -1;   // a wheel's slot used at its trigger's release, until vk_xr_wheel_used takes it
    std::atomic<bool> wheel_allowed{true};   // vr_game.c: the game is not paused by its own menus
    std::atomic<uint64_t> wheel_closed_at{0};   // plat_now_ns at a wheel's close
    std::atomic<bool> ocarina{false};   // vr_game.c: Link plays the ocarina (the controllers' Ocarina mode)
    std::atomic<bool> weapon_out{false};   // vr_game.c: Link holds his sword and shield (or another weapon) out
    std::atomic<bool> child{true};         // vr_game.c: Link is a child (his hands' model)
    std::atomic<bool> in_play{false};      // vr_game.c: play (not a menu, a pause, an event): the grips' and swing's play roles
    // the last picture drawn from the head whose world scene8 and eye8 still hold (the present's thread alone): a paused
    // game's menus go on the HUD's panel in front of that world, frozen as it was
    Drawn frozen;
};

Xr &xr() {
    static Xr x;
    return x;
}

// "a b c" into its names, the string's spaces made ends
void split(std::string &s, std::vector<const char *> &out) {
    out.clear();
    for (char &ch : s)
        if (ch == ' ') ch = 0;
    for (size_t i = 0; i < s.size();) {
        if (s[i]) out.push_back(s.c_str() + i), i += std::strlen(s.c_str() + i);
        else i++;
    }
}

bool init() {
    Xr &x = xr();
    if (x.tried) return x.ok;
    x.tried = true;
    if (!cfg_bool("NX2NAVI_VR")) return false;
#ifdef _WIN32
    void *lib = NULL;
    char beside[PLAT_PATH_MAX];
    char *sep = plat_exe_path(beside, sizeof beside) ? plat_last_sep(beside) : NULL;
    if (sep && (size_t)(sep + 1 - beside) + sizeof "openxr_loader.dll" <= sizeof beside) {
        strcpy(sep + 1, "openxr_loader.dll");
        if (plat_file_exists(beside)) lib = plat_lib_open(beside);
    }
    if (!lib) lib = plat_lib_open("openxr_loader.dll");
#else
    // WiVRn installed from Flathub registers no runtime for programs outside the sandbox: its manifest is used when no
    // runtime is named
    char named[8];
    if (!plat_getenv("XR_RUNTIME_JSON", named, sizeof named)) {
        static const char *wivrn = "/var/lib/flatpak/app/io.github.wivrn.wivrn/current/active/files/share/openxr/1/openxr_wivrn.json";
        if (plat_file_exists(wivrn)) {
            plat_setenv("XR_RUNTIME_JSON", wivrn);
            rt_log("vr: XR_RUNTIME_JSON unset: WiVRn's Flatpak runtime, %s", wivrn);
        }
    }
    void *lib = plat_lib_open("libopenxr_loader.so.1");
#endif
    if (lib) x.gipa = (PFN_xrGetInstanceProcAddr)plat_lib_sym(lib, "xrGetInstanceProcAddr");
    if (!x.gipa) {
        rt_log("vr: no OpenXR loader: VR off");
        return false;
    }
    PFN_xrCreateInstance create = nullptr;
    PFN_xrEnumerateInstanceExtensionProperties enumerate = nullptr;
    x.gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&create);
    x.gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction *)&enumerate);
    if (!create || !enumerate) {
        rt_log("vr: the OpenXR loader lacks xrCreateInstance: VR off");
        return false;
    }
    uint32_t n = 0;
    enumerate(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
    enumerate(nullptr, n, &n, exts.data());
    bool vulkan = false;
    for (const auto &e : exts) {
        vulkan |= !std::strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
        x.cylinder |= !std::strcmp(e.extensionName, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    }
    if (!x.cylinder) rt_log("vr: the runtime has no %s: the screen stays flat", XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    if (!vulkan) {
        rt_log("vr: the OpenXR runtime has no %s (is its server running?): VR off", XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
        return false;
    }
    const char *want[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(ci.applicationInfo.applicationName, "NX2navi");
    std::strcpy(ci.applicationInfo.engineName, "NX2navi");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = x.cylinder ? 2 : 1;
    ci.enabledExtensionNames = want;
    XrResult r = create(&ci, &x.instance);
    if (XR_FAILED(r)) {
        rt_log("vr: xrCreateInstance failed (%d; is the runtime's server running and the headset connected?): VR off", (int)r);
        return false;
    }
#define XR_LOAD(f) x.gipa(x.instance, #f, (PFN_xrVoidFunction *)&x.f);
    XR_FUNCS(XR_LOAD)
#undef XR_LOAD
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    x.xrGetInstanceProperties(x.instance, &ip);
    XrSystemGetInfo sg{XR_TYPE_SYSTEM_GET_INFO};
    sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = x.xrGetSystem(x.instance, &sg, &x.system);
    if (XR_FAILED(r)) {
        rt_log("vr: %s %u.%u.%u: no headset (%d): VR off", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
               XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion), (int)r);
        x.xrDestroyInstance(x.instance), x.instance = XR_NULL_HANDLE;
        return false;
    }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    x.xrGetSystemProperties(x.instance, x.system, &sp);
    XrGraphicsRequirementsVulkanKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};   // asked before a session, as the spec says
    x.xrGetVulkanGraphicsRequirementsKHR(x.instance, x.system, &req);
    for (int pass = 0; pass < 2; pass++) {
        auto get = pass ? x.xrGetVulkanDeviceExtensionsKHR : x.xrGetVulkanInstanceExtensionsKHR;
        std::string &s = pass ? x.device_exts : x.instance_exts;
        uint32_t len = 0;
        get(x.instance, x.system, 0, &len, nullptr);
        s.assign(len, '\0');
        if (len) get(x.instance, x.system, len, &len, s.data());
        while (!s.empty() && !s.back()) s.pop_back();
        split(s, pass ? x.device_list : x.instance_list);
    }
    rt_log("vr: %s %u.%u.%u, %s; Vulkan %u.%u to %u.%u; instance extensions \"%s\", device extensions \"%s\"",
           ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
           XR_VERSION_PATCH(ip.runtimeVersion), sp.systemName, XR_VERSION_MAJOR(req.minApiVersionSupported),
           XR_VERSION_MINOR(req.minApiVersionSupported), XR_VERSION_MAJOR(req.maxApiVersionSupported),
           XR_VERSION_MINOR(req.maxApiVersionSupported), x.instance_exts.c_str(), x.device_exts.c_str());
    x.ok = true;
    return true;
}

bool listed(const char *const *list, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++)
        if (!std::strcmp(list[i], name)) return true;
    return false;
}

void barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
             VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from, b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1, d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &d);
}

// v rotated by the unit quaternion q (x, y, z, w)
void rotate(const float q[4], const float v[3], float out[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

// The frustum the game draws with for the eyes' lens on a picture of ratio `ratio`: centred (the game's projection is
// symmetric, and the projection layer is told this frustum, so the compositor puts each pixel where it belongs), the
// shorter side grown so nothing the eyes see is left out
Lens frustum(const Lens &eyes, float ratio) {
    const float side = std::fmax(-eyes.l, eyes.r), up = std::fmax(eyes.u, -eyes.d);
    const float v = std::fmax(up, side / ratio), h = v * ratio;
    Lens f;
    f.l = -h, f.r = h, f.u = v, f.d = -v;
    return f;
}

bool is_srgb(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

// The three boxes at the swapchain's size, in its channel order and the window's encoding: a blit between two sRGB or
// two UNORM formats keeps the bytes, so the copy into the sRGB swapchain shows them as the window does
bool make_boxes(VkFormat window) {
    Xr &x = xr();
    const bool srgb = is_srgb(window);
    const VkFormat format = x.rgba ? (srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM)
                                   : (srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM);
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(x.phys, format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        rt_log("vr: no blit into format %d: the headset shows nothing", (int)format);
        return false;
    }
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(x.phys, &mem);
    for (int k = 0; k < 9; k++) {   // the pictures' three, the HUD's, the right eye's
        VkImage &box = k < 3 ? x.boxes[k] : k < 6 ? x.hud_boxes[k - 3] : x.boxes_r[k - 6];
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {x.width, x.height, 1};
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (vkCreateImage(x.device, &ci, nullptr, &box) != VK_SUCCESS) return false;
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(x.device, box, &mr);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < mem.memoryTypeCount && type == UINT32_MAX; i++)
            if ((mr.memoryTypeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                type = i;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size, ai.memoryTypeIndex = type;
        VkDeviceMemory dm = VK_NULL_HANDLE;
        if (type == UINT32_MAX || vkAllocateMemory(x.device, &ai, nullptr, &dm) != VK_SUCCESS ||
            vkBindImageMemory(x.device, box, dm, 0) != VK_SUCCESS)
            return false;
    }
    rt_log("vr: VR's pictures: %ux%u, format %d (the window's %d)", x.width, x.height, (int)format, (int)window);
    return true;
}

// A device-local 2D image (one level, one layer) and a view of it; UNDEFINED until the caller's barrier
bool make_image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImage &image, VkImageView &view,
                VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
    Xr &x = xr();
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = format, ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1, ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL, ci.usage = usage;
    if (vkCreateImage(x.device, &ci, nullptr, &image) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(x.device, image, &mr);
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(x.phys, &mem);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mem.memoryTypeCount && type == UINT32_MAX; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size, ai.memoryTypeIndex = type;
    VkDeviceMemory dm = VK_NULL_HANDLE;
    VkResult ar = type == UINT32_MAX ? VK_ERROR_OUT_OF_DEVICE_MEMORY : vkAllocateMemory(x.device, &ai, nullptr, &dm);
    // the card's memory full (the game keeps it so): any other type the image may live in, the system's memory among them
    for (uint32_t i = 0; i < mem.memoryTypeCount && ar != VK_SUCCESS; i++)
        if ((mr.memoryTypeBits & (1u << i)) && i != type) ai.memoryTypeIndex = i, ar = vkAllocateMemory(x.device, &ai, nullptr, &dm);
    if (ar != VK_SUCCESS || vkBindImageMemory(x.device, image, dm, 0) != VK_SUCCESS) {
        rt_log("vr: an image %ux%u (format %d) found no memory (%d)", w, h, (int)format, (int)ar);
        return false;
    }
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vc.image = image, vc.viewType = VK_IMAGE_VIEW_TYPE_2D, vc.format = format, vc.subresourceRange = {aspect, 0, 1, 0, 1};
    return vkCreateImageView(x.device, &vc, nullptr, &view) == VK_SUCCESS;
}

// A device-local buffer of n bytes
bool make_buffer(VkDeviceSize n, VkBufferUsageFlags usage, VkBuffer &buf) {
    Xr &x = xr();
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = n, bi.usage = usage;
    if (vkCreateBuffer(x.device, &bi, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(x.device, buf, &mr);
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(x.phys, &mem);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mem.memoryTypeCount && type == UINT32_MAX; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size, ai.memoryTypeIndex = type;
    VkDeviceMemory dm = VK_NULL_HANDLE;
    return type != UINT32_MAX && vkAllocateMemory(x.device, &ai, nullptr, &dm) == VK_SUCCESS &&
           vkBindBufferMemory(x.device, buf, dm, 0) == VK_SUCCESS;
}

// A barrier on all memory: what came before is done and visible to what comes after
void memory_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = m.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    m.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    m.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1, d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cb, &d);
}

// The HUD panel's shader (shaders/xr_hud.comp): two sampled float images in, two RGBA8 storage images out, push descriptors
bool make_hud_pipeline() {
    Xr &x = xr();
    x.push_set = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(x.device, "vkCmdPushDescriptorSetKHR");
    if (!x.push_set) return false;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(x.device, &si, nullptr, &x.nearest) != VK_SUCCESS) return false;
    const VkDescriptorSetLayoutBinding b[4] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, li.bindingCount = 4, li.pBindings = b;
    if (vkCreateDescriptorSetLayout(x.device, &li, nullptr, &x.hud_set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(int32_t) * 4};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1, pi.pSetLayouts = &x.hud_set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(x.device, &pi, nullptr, &x.hud_layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof xr_hud_spv, mi.pCode = xr_hud_spv;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(x.device, &mi, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
    ci.layout = x.hud_layout;
    const VkResult r = vkCreateComputePipelines(x.device, VK_NULL_HANDLE, 1, &ci, nullptr, &x.hud_pipe);
    vkDestroyShaderModule(x.device, m, nullptr);
    return r == VK_SUCCESS;
}

bool hud_wanted() { return strcmp(cfg_value("NX2NAVI_VR_HUD"), "panel") == 0; }

// NX2NAVI_VR_STEREO=balanced: each game frame drawn from one eye in turn (true geometry, each eye at half the rate),
// rather than performant's depth stereo from the head
bool stereo_balanced() { return strcmp(cfg_value("NX2NAVI_VR_STEREO"), "balanced") == 0; }
// NX2NAVI_VR_STEREO=accurate: Balanced's eyes in turn with the game's time held for the right eye's update
// (frame_rate.c), so both eyes are drawn at one moment and moving does not judder; the world moves at half the rate
bool stereo_accurate() { return strcmp(cfg_value("NX2NAVI_VR_STEREO"), "accurate") == 0; }
bool stereo_turns() { return stereo_balanced() || stereo_accurate(); }
std::atomic<double> update_step{-1};

// (vk_xr_capture, hud_mu held) the scene without the HUD (with it: keep_hud) and the HUD's panel made from the target as
// it is now and the scene kept from before the HUD, the panel clear within `cut` pixels of its middle (the game's
// reticle while Link aims); false when there is nothing new to make them from
bool make_panel(VkCommandBuffer cb, bool keep_hud, int cut) {
    Xr &x = xr();
    if (!x.scene || !x.hud_target || x.kept == x.used) return false;
    if (!x.hud_tried) {
        x.hud_tried = true;
        if (!make_hud_pipeline()) {
            rt_log("vr: the HUD panel's shader could not be made: the HUD stays in the picture");
            x.hud_pipe = VK_NULL_HANDLE;
        }
    }
    if (!x.hud_pipe) return false;
    if (!x.scene8) {
        const VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (!make_image(x.hud_w, x.hud_h, VK_FORMAT_R8G8B8A8_UNORM, u | VK_IMAGE_USAGE_SAMPLED_BIT, x.scene8, x.scene8_view) ||
            !make_image(x.hud_w, x.hud_h, VK_FORMAT_R8G8B8A8_UNORM, u, x.panel8, x.panel8_view)) {
            x.hud_pipe = VK_NULL_HANDLE;
            return false;
        }
        rt_log("vr: the HUD panel: the HUD's target %ux%u (format %d)", x.hud_w, x.hud_h, (int)x.hud_format);
    }
    if (x.target_viewed != x.hud_target) {   // a view of the game's target, made again when the target is another
        if (x.target_view) vkDestroyImageView(x.device, x.target_view, nullptr);
        VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vc.image = x.hud_target, vc.viewType = VK_IMAGE_VIEW_TYPE_2D, vc.format = x.hud_format;
        vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        x.target_view = VK_NULL_HANDLE;
        if (vkCreateImageView(x.device, &vc, nullptr, &x.target_view) != VK_SUCCESS) return false;
        x.target_viewed = x.hud_target;
    }
    x.used = x.kept;
    barrier(cb, x.scene8, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    barrier(cb, x.panel8, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, x.hud_pipe);
    const VkDescriptorImageInfo info[4] = {{x.nearest, x.target_view, VK_IMAGE_LAYOUT_GENERAL},
                                           {x.nearest, x.scene_view, VK_IMAGE_LAYOUT_GENERAL},
                                           {VK_NULL_HANDLE, x.scene8_view, VK_IMAGE_LAYOUT_GENERAL},
                                           {VK_NULL_HANDLE, x.panel8_view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[4]{};
    for (uint32_t i = 0; i < 4; i++) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, w[i].dstBinding = i, w[i].descriptorCount = 1;
        w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[i].pImageInfo = &info[i];
    }
    x.push_set(cb, VK_PIPELINE_BIND_POINT_COMPUTE, x.hud_layout, 0, 4, w);
    const int32_t pc[4] = {(int32_t)x.hud_w, (int32_t)x.hud_h, keep_hud ? 1 : 0, cut};
    vkCmdPushConstants(cb, x.hud_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (x.hud_w + 7) / 8, (x.hud_h + 7) / 8, 1);
    barrier(cb, x.scene8, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    x.panels++;
    return true;
}

// (vk_xr_capture, a picture from the head) the performance overlay (ui/menu.cpp menu_perf_rect), drawn on the window
// over the picture, copied from the window to the top middle of the HUD's panel, its size there as on the window; the
// HUD box made a clear panel first when the picture has none
// The window's rect r ({x, y, w, h}) clipped to it into [x0, y0, x1, y1); false when nothing is left
bool clip_rect(const int r[4], VkExtent2D extent, int out[4]) {
    out[0] = std::max(r[0], 0), out[1] = std::max(r[1], 0);
    out[2] = std::min(r[0] + r[2], (int)extent.width), out[3] = std::min(r[1] + r[3], (int)extent.height);
    return out[2] > out[0] && out[3] > out[1];
}

// The host's own overlays drawn into the window (ui/menu.cpp) onto the HUD's box: the performance overlay at the top
// middle, the item wheel (a disc, copied in rows of its width so the picture around it stays out) in the middle. Without
// a HUD box this frame (d.hud and d.menu false) one is made clear first.
void overlay_onto_hud(VkCommandBuffer cb, int slot, VkImage window, VkExtent2D extent, Drawn &d) {
    Xr &x = xr();
    int r[4], perf[4] = {}, wheel[4] = {};
    const bool has_perf = menu_perf_rect(r) && clip_rect(r, extent, perf);
    const bool has_wheel = menu_wheel_rect(r) && clip_rect(r, extent, wheel);
    int label[4] = {};
    const bool has_label = has_wheel && menu_wheel_label(r) && clip_rect(r, extent, label);
    // a wheel open, or one closed a moment ago: the game's HUD left off (its quick select, held meanwhile, going away)
    const bool hide = has_wheel || plat_now_ns() - x.wheel_closed_at.load() < wheel_hide_ns;
    if (!has_perf && !has_wheel && !hide) return;
    const VkImage hb = x.hud_boxes[slot];
    const bool fresh = (!d.hud && !d.menu) || hide;
    barrier(cb, hb, fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (fresh) {
        const VkClearColorValue clear{};
        const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, hb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &all);
        barrier(cb, hb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    }
    const float sx = (float)x.width / (float)extent.width, sy = (float)x.height / (float)extent.height;
    VkImageBlit b{};
    b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    if (has_perf) {
        const int x0 = perf[0], y0 = perf[1], x1 = perf[2], y1 = perf[3];
        const int w = std::max(1, (int)((x1 - x0) * sx)), h = std::max(1, (int)((y1 - y0) * sy));
        const int dx = ((int)x.width - w) / 2, dy = (int)(0.03f * (float)x.height);
        b.srcOffsets[0] = {x0, y0, 0}, b.srcOffsets[1] = {x1, y1, 1};
        b.dstOffsets[0] = {dx, dy, 0}, b.dstOffsets[1] = {std::min(dx + w, (int)x.width), std::min(dy + h, (int)x.height), 1};
        vkCmdBlitImage(cb, window, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                       VK_FILTER_LINEAR);
    }
    if (has_wheel) {   // the disc's rows, each its chord's width, its middle onto the box's middle
        const float s = std::min(sx, sy);
        const float cx = 0.5f * (float)(wheel[0] + wheel[2]), cy = 0.5f * (float)(wheel[1] + wheel[3]);
        const float rad = 0.5f * (float)std::min(wheel[2] - wheel[0], wheel[3] - wheel[1]);
        const float dcx = 0.5f * (float)x.width, dcy = 0.5f * (float)x.height;
        constexpr int rows = 96;
        VkImageBlit bands[rows];
        int n = 0;
        for (int i = 0; i < rows; i++) {
            const float y0 = cy - rad + 2.0f * rad * (float)i / rows, y1 = cy - rad + 2.0f * rad * (float)(i + 1) / rows;
            const float mid = 0.5f * (y0 + y1) - cy;   // the chord at the band's middle
            const float half = std::sqrt(std::max(0.0f, rad * rad - mid * mid));
            const int sx0 = (int)std::floor(cx - half), sx1 = (int)std::ceil(cx + half);
            const int sy0 = (int)std::floor(y0), sy1 = (int)std::ceil(y1);
            if (sx1 <= sx0 || sy1 <= sy0) continue;
            VkImageBlit &k = bands[n++];
            k = b;
            k.srcOffsets[0] = {sx0, sy0, 0}, k.srcOffsets[1] = {sx1, sy1, 1};
            const auto dst_x = [&](float v) { return std::clamp((int)std::lround(dcx + (v - cx) * s), 0, (int)x.width); };
            const auto dst_y = [&](float v) { return std::clamp((int)std::lround(dcy + (v - cy) * s), 0, (int)x.height); };
            k.dstOffsets[0] = {dst_x((float)sx0), dst_y((float)sy0), 0}, k.dstOffsets[1] = {dst_x((float)sx1), dst_y((float)sy1), 1};
            if (k.dstOffsets[1].x <= k.dstOffsets[0].x || k.dstOffsets[1].y <= k.dstOffsets[0].y) n--;
        }
        if (n) vkCmdBlitImage(cb, window, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              (uint32_t)n, bands, VK_FILTER_LINEAR);
        if (has_label) {   // the hovered item's name under the disc, placed as in the window
            VkImageBlit k = b;
            const auto dst_x = [&](float v) { return std::clamp((int)std::lround(dcx + (v - cx) * s), 0, (int)x.width); };
            const auto dst_y = [&](float v) { return std::clamp((int)std::lround(dcy + (v - cy) * s), 0, (int)x.height); };
            k.srcOffsets[0] = {label[0], label[1], 0}, k.srcOffsets[1] = {label[2], label[3], 1};
            k.dstOffsets[0] = {dst_x((float)label[0]), dst_y((float)label[1]), 0};
            k.dstOffsets[1] = {dst_x((float)label[2]), dst_y((float)label[3]), 1};
            if (k.dstOffsets[1].x > k.dstOffsets[0].x && k.dstOffsets[1].y > k.dstOffsets[0].y)
                vkCmdBlitImage(cb, window, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &k, VK_FILTER_LINEAR);
        }
    }
    barrier(cb, hb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (fresh && !d.menu) d.hud = true;
}

// The depth stereo shader (shaders/xr_depth.comp): the scene and its depth sampled, the eye's RGBA8 image out
bool make_depth_pipeline() {
    Xr &x = xr();
    if (!x.push_set) return false;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(x.device, &si, nullptr, &x.linear) != VK_SUCCESS) return false;
    const VkDescriptorSetLayoutBinding b[4] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, li.bindingCount = 4, li.pBindings = b;
    if (vkCreateDescriptorSetLayout(x.device, &li, nullptr, &x.depth_set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float) * 7};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1, pi.pSetLayouts = &x.depth_set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(x.device, &pi, nullptr, &x.depth_layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof xr_depth_spv, mi.pCode = xr_depth_spv;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(x.device, &mi, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
    ci.layout = x.depth_layout;
    const VkResult r = vkCreateComputePipelines(x.device, VK_NULL_HANDLE, 1, &ci, nullptr, &x.depth_pipe);
    vkDestroyShaderModule(x.device, m, nullptr);
    return r == VK_SUCCESS;
}

// The depth copy's shader (shaders/xr_depth_keep.comp), made on the thread that records the scene: the scene's depth
// sampled, an R32F storage image out
bool make_keep_pipeline() {
    Xr &x = xr();
    x.keep_push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(x.device, "vkCmdPushDescriptorSetKHR");
    if (!x.keep_push) return false;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(x.device, &si, nullptr, &x.keep_sampler) != VK_SUCCESS) return false;
    const VkDescriptorSetLayoutBinding b[3] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, li.bindingCount = 3, li.pBindings = b;
    if (vkCreateDescriptorSetLayout(x.device, &li, nullptr, &x.keep_set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(int32_t) * 2};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1, pi.pSetLayouts = &x.keep_set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(x.device, &pi, nullptr, &x.keep_layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof xr_depth_keep_spv, mi.pCode = xr_depth_keep_spv;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(x.device, &mi, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
    ci.layout = x.keep_layout;
    const VkResult r = vkCreateComputePipelines(x.device, VK_NULL_HANDLE, 1, &ci, nullptr, &x.keep_pipe);
    vkDestroyShaderModule(x.device, m, nullptr);
    return r == VK_SUCCESS;
}

// The aiming reticle's shader (shaders/xr_aim.comp): the scene's depth sampled, scene8 read and written
bool make_aim_pipeline() {
    Xr &x = xr();
    if (!x.push_set || !x.nearest) return false;
    const VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, li.bindingCount = 2, li.pBindings = b;
    if (vkCreateDescriptorSetLayout(x.device, &li, nullptr, &x.aim_set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float) * 12};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1, pi.pSetLayouts = &x.aim_set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(x.device, &pi, nullptr, &x.aim_layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof xr_aim_spv, mi.pCode = xr_aim_spv;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(x.device, &mi, nullptr, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
    ci.layout = x.aim_layout;
    const VkResult r = vkCreateComputePipelines(x.device, VK_NULL_HANDLE, 1, &ci, nullptr, &x.aim_pipe);
    vkDestroyShaderModule(x.device, m, nullptr);
    return r == VK_SUCCESS;
}

// (vk_xr_capture, hud_mu held, after make_panel, before make_eyes) the reticle drawn into scene8 where the aim's ray
// first goes behind the scene's depth, while Link aims
void make_aim(VkCommandBuffer cb, const Drawn &d) {
    Xr &x = xr();
    float planes[2];
    if (!d.aim || !x.depth_kept || !vk_dlaa_vr_planes(planes)) return;
    if (!x.aim_tried) {
        x.aim_tried = true;
        if (!make_aim_pipeline()) {
            rt_log("vr: the aiming reticle's shader could not be made");
            x.aim_pipe = VK_NULL_HANDLE;
        }
    }
    if (!x.aim_pipe) return;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, x.aim_pipe);
    const VkDescriptorImageInfo info[2] = {{x.nearest, x.depth_view, VK_IMAGE_LAYOUT_GENERAL},
                                           {VK_NULL_HANDLE, x.scene8_view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[2]{};
    for (uint32_t i = 0; i < 2; i++) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, w[i].dstBinding = i, w[i].descriptorCount = 1;
        w[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &info[i];
    }
    x.push_set(cb, VK_PIPELINE_BIND_POINT_COMPUTE, x.aim_layout, 0, 2, w);
    const float pc[12] = {(float)x.hud_w, (float)x.hud_h, d.aim_o[0], d.aim_o[1], d.aim_o[2], d.aim_d[0], d.aim_d[1],
                          d.aim_d[2], d.lens.r * d.scale, d.lens.u * d.scale, planes[0], planes[1]};
    vkCmdPushConstants(cb, x.aim_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, 1, 1, 1);
    barrier(cb, x.scene8, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
}


// VR's hands: a game model (vr_model.c), read on a worker
void read_hand_model(void *arg) {
    Xr::HandItem &it = *(Xr::HandItem *)arg;
    if (!std::strncmp(it.name, "@hand", 5))   // Link's own hand: "@hand" then A (adult) or C (child), L or R
        it.state.store(vr_model_hand(it.name[5] == 'A', it.name[6] == 'R', &it.cpu) ? 2 : 3);
    else it.state.store(vr_model_load(it.name, &it.cpu) ? 2 : 3);
}

// The present's thread: the model of this actor, its reading started the first time it is asked for; NULL until read
Xr::HandItem *hand_item(const char *name) {
    Xr &x = xr();
    if (!name[0]) return nullptr;
    for (auto &it : x.items)
        if (it.name[0] && !strcmp(it.name, name)) return it.state.load() == 2 ? &it : nullptr;
    for (auto &it : x.items)
        if (!it.name[0]) {
            std::snprintf(it.name, sizeof it.name, "%s", name);
            it.state.store(1);
            if (!plat_thread_spawn(read_hand_model, &it, "vr-model", 0)) read_hand_model(&it);
            return nullptr;
        }
    return nullptr;
}

// A host-visible buffer of n bytes holding `data`
bool make_host_buffer(VkDeviceSize n, VkBufferUsageFlags usage, const void *data, VkBuffer &buf) {
    Xr &x = xr();
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = n, bi.usage = usage;
    if (vkCreateBuffer(x.device, &bi, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(x.device, buf, &mr);
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(x.phys, &mem);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mem.memoryTypeCount && type == UINT32_MAX; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & want) == want) type = i;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size, ai.memoryTypeIndex = type;
    VkDeviceMemory dm = VK_NULL_HANDLE;
    void *at = nullptr;
    if (type == UINT32_MAX || vkAllocateMemory(x.device, &ai, nullptr, &dm) != VK_SUCCESS ||
        vkBindBufferMemory(x.device, buf, dm, 0) != VK_SUCCESS || vkMapMemory(x.device, dm, 0, n, 0, &at) != VK_SUCCESS)
        return false;
    std::memcpy(at, data, (size_t)n);
    return true;
}

// The model's buffers and its parts' textures on the card; the textures' copies recorded into cb
bool upload_item(VkCommandBuffer cb, Xr::HandItem &it) {
    const VrModel &m = it.cpu;
    if (!m.verts || !m.parts) return false;
    if (!make_host_buffer(sizeof(float) * 8 * m.verts_n, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, m.verts, it.vb) ||
        !make_host_buffer(sizeof(uint32_t) * m.index_n, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, m.index, it.ib))
        return false;
    for (int i = 0; i < m.parts; i++) {
        const VrModelPart &p = m.part[i];
        if (!make_host_buffer((VkDeviceSize)p.tex_w * p.tex_h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, p.rgba, it.staging[i]) ||
            !make_image(p.tex_w, p.tex_h, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        it.tex[i], it.tex_view[i]))
            return false;
        barrier(cb, it.tex[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {p.tex_w, p.tex_h, 1};
        vkCmdCopyBufferToImage(cb, it.staging[i], it.tex[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        barrier(cb, it.tex[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    it.uploaded = true;
    return true;
}

VkShaderModule shader(const uint32_t *code, size_t n) {
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = n, mi.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(xr().device, &mi, nullptr, &m);
    return m;
}

// The hands' drawing pipeline (a model's vertices, its texture by push descriptor, two colour targets and a reversed
// depth) and the compute that puts them into the scene
bool make_hand_pipelines() {
    Xr &x = xr();
    if (!x.push_set || !x.nearest) return false;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (vkCreateSampler(x.device, &si, nullptr, &x.hand_sampler) != VK_SUCCESS) return false;
    {
        const VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, li.bindingCount = 1, li.pBindings = &b;
        if (vkCreateDescriptorSetLayout(x.device, &li, nullptr, &x.hand_set) != VK_SUCCESS) return false;
        const VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 32};
        VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pi.setLayoutCount = 1, pi.pSetLayouts = &x.hand_set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
        if (vkCreatePipelineLayout(x.device, &pi, nullptr, &x.hand_layout) != VK_SUCCESS) return false;
        const VkShaderModule vs = shader(xr_hand_vs_spv, sizeof xr_hand_vs_spv), fs = shader(xr_hand_fs_spv, sizeof xr_hand_fs_spv);
        if (!vs || !fs) return false;
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
        st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};
        const VkVertexInputBindingDescription vb{0, sizeof(float) * 8, VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription va[3] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                         {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
                                                         {2, 0, VK_FORMAT_R32G32_SFLOAT, 24}};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1, vi.pVertexBindingDescriptions = &vb;
        vi.vertexAttributeDescriptionCount = 3, vi.pVertexAttributeDescriptions = va;
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = VK_CULL_MODE_NONE, rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = ds.depthWriteEnable = VK_TRUE, ds.depthCompareOp = VK_COMPARE_OP_GREATER;   // reversed
        VkPipelineColorBlendAttachmentState cba[2]{};
        cba[0].colorWriteMask = cba[1].colorWriteMask = 0xf;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 2, cb.pAttachments = cba;
        const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2, dy.pDynamicStates = dyn;
        const VkFormat colors[2] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R16_SFLOAT};
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = 2, ri.pColorAttachmentFormats = colors, ri.depthAttachmentFormat = VK_FORMAT_D16_UNORM;
        VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gi.pNext = &ri, gi.stageCount = 2, gi.pStages = st, gi.pVertexInputState = &vi, gi.pInputAssemblyState = &ia;
        gi.pViewportState = &vp, gi.pRasterizationState = &rs, gi.pMultisampleState = &ms, gi.pDepthStencilState = &ds;
        gi.pColorBlendState = &cb, gi.pDynamicState = &dy, gi.layout = x.hand_layout;
        const VkResult r = vkCreateGraphicsPipelines(x.device, VK_NULL_HANDLE, 1, &gi, nullptr, &x.hand_pipe);
        vkDestroyShaderModule(x.device, vs, nullptr), vkDestroyShaderModule(x.device, fs, nullptr);
        if (r != VK_SUCCESS) return false;
    }
    const VkDescriptorSetLayoutBinding b[5] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, li.bindingCount = 5, li.pBindings = b;
    if (vkCreateDescriptorSetLayout(x.device, &li, nullptr, &x.mix_set) != VK_SUCCESS) return false;
    const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(int32_t) * 6};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1, pi.pSetLayouts = &x.mix_set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(x.device, &pi, nullptr, &x.mix_layout) != VK_SUCCESS) return false;
    const VkShaderModule m = shader(xr_hand_mix_spv, sizeof xr_hand_mix_spv);
    if (!m) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
    ci.layout = x.mix_layout;
    const VkResult r = vkCreateComputePipelines(x.device, VK_NULL_HANDLE, 1, &ci, nullptr, &x.mix_pipe);
    vkDestroyShaderModule(x.device, m, nullptr);
    return r == VK_SUCCESS;
}

// 4x4 matrices by columns (as GLSL takes them)
struct Mat4 {
    float m[16];
};
Mat4 mul(const Mat4 &a, const Mat4 &b) {
    Mat4 r{};
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 4; k++)
            for (int i = 0; i < 4; i++) r.m[4 * c + i] += a.m[4 * k + i] * b.m[4 * c + k];
    return r;
}
// a pose's rotation and position: what it holds to the space it is in
Mat4 pose_matrix(const Pose &p) {
    static const float ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0}, ez[3] = {0, 0, 1};
    float c[3][3];
    rotate(p.q, ex, c[0]), rotate(p.q, ey, c[1]), rotate(p.q, ez, c[2]);
    Mat4 r{};
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < 3; i++) r.m[4 * k + i] = c[k][i];
    r.m[12] = p.p[0], r.m[13] = p.p[1], r.m[14] = p.p[2], r.m[15] = 1;
    return r;
}
Mat4 inverse_rigid(const Mat4 &a) {
    Mat4 r{};
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < 3; i++) r.m[4 * k + i] = a.m[4 * i + k];
    for (int i = 0; i < 3; i++) r.m[12 + i] = -(r.m[i] * a.m[12] + r.m[4 + i] * a.m[13] + r.m[8 + i] * a.m[14]);
    r.m[15] = 1;
    return r;
}
// The model held in the grip (OpenXR's grip, held pointing forward: -z forward through the fist from the little finger
// to the thumb, x to the right, y up, on either hand): its axes and where its origin sits, in the grip
Mat4 held(int item, int right_hand, const VrModel &m, const char *name) {
    Mat4 r{};
    r.m[15] = 1;
    if (item == 2 && strstr(name, "Hookshot")) {   // the hookshot: pointing out of the fist forward
        r.m[1] = 1;                 // model x -> grip y
        r.m[6] = 1;                 // model y -> grip z
        r.m[8] = 1;                 // model z -> grip x
        return r;
    }
    if (item == 2) {   // the other items as a sword: along the fist (+z), held near the handle's end or at the middle
        r.m[1] = 1, r.m[4] = 1, r.m[10] = -1;
        if (strstr(name, "Hammer")) r.m[14] = 0.5f * (m.lo[2] + m.head_z);   // the handle's middle
        else if (strstr(name, "DekuStick")) r.m[14] = m.lo[2] + 0.11f;
        else if (strstr(name, "SlingShot")) r.m[14] = m.lo[2] + 0.06f;
        return r;
    }
    if (item == 0) {   // the sword: the blade (+z) along the fist, its flat (y) across the palm, the grip held
        r.m[1] = 1;                 // model x -> grip y
        r.m[4] = 1;                 // model y -> grip x
        r.m[10] = -1;               // model z -> grip -z
        r.m[14] = m.grip_z;         // its grip (behind the crossguard, vr_model.c) at the hand
    } else {   // the shield: its straps (x) down the grip, its face (+y) out from the back of the hand (the grip's left on
               // the left hand, its right on the right), so the forearm brought across the body guards with it
        const float s = right_hand ? 1.0f : -1.0f;
        r.m[1] = -1;                // model x -> grip -y
        r.m[4] = s;                 // model y -> grip -x on the left hand, +x on the right
        r.m[10] = s;                // model z -> grip -z on the left hand, +z on the right
        // then turned 10 degrees about the grip's z (through the fist), its face toward the knuckles
        const float a = -s * 10.0f * 3.14159265f / 180.0f, c = std::cos(a), n = std::sin(a);
        for (int k = 0; k < 3; k++) {
            const float gx = r.m[4 * k], gy = r.m[4 * k + 1];
            r.m[4 * k] = c * gx - n * gy, r.m[4 * k + 1] = n * gx + c * gy;
        }
        // the game's shields are made for Link's right arm: on the left hand mirrored along their straps (model x)
        if (!right_hand)
            for (int i = 0; i < 3; i++) r.m[i] = -r.m[i];
    }
    return r;
}

// VR's hands drawn (vk_xr_capture): the sword and shield equipped while Link holds them out, or the item in his hand,
// with Play style Motion and the Touch controllers. Prepared once a present (hands_ready: the models, the pipelines,
// the targets), then drawn from a pose (hands_draw: into hand_color and hand_dist at three quarters of the scene's
// size) and put into a picture where nearer than the scene's kept depth (hands_mix): each eye's from that eye with
// depth stereo, else the scene's from the head; and the desktop window's from the head (hands_onto_window).
struct HandsFrame {
    Xr::HandItem *gear[3] = {};
    Xr::HandItem *bare[2] = {};   // Link's own hand on a hand that holds nothing (left, right)
    Pose grips[2];
    bool ok[2]{};
    float planes[2]{};
    bool two_handed = false;
    int sword = 0;
};

bool hands_ready(VkCommandBuffer cb, const Drawn &d, HandsFrame &f) {
    Xr &x = xr();
    if (!d.tracked || strcmp(cfg_value("NX2NAVI_VR_PLAY"), "motion") || strcmp(cfg_value("NX2NAVI_VR_CONTROLS"), "meta") ||
        !x.depth_kept || !vk_dlaa_vr_planes(f.planes))
        return false;
    // the item in Link's hand in the sword's place, with the shield; else the sword and shield while they are out; and
    // Link's own hand on each hand that holds none of them
    f.sword = strcmp(cfg_value("NX2NAVI_VR_HANDEDNESS"), "right") == 0 ? 1 : 0;
    {
        std::lock_guard<std::mutex> lk(x.gear_mu);
        const char *item = x.gear[2], *sword = x.gear[0];
        const bool out = item[0] || x.weapon_out.load();
        if (item[0]) f.gear[2] = hand_item(item), f.gear[1] = hand_item(x.gear[1]);
        else if (out) f.gear[0] = hand_item(sword), f.gear[1] = hand_item(x.gear[1]);
        // held in both hands (the Biggoron Sword, the bow, the slingshot, the hammer, a Deku Stick): no shield
        f.two_handed = (!item[0] && out && strstr(sword, "Biggoron")) ||
                       (item[0] && (strstr(item, "_Bow") || strstr(item, "SlingShot") || strstr(item, "Hammer") ||
                                    strstr(item, "DekuStick")));
        const bool held[2] = {out && (item[0] || sword[0]), out && x.gear[1][0] && !f.two_handed};   // sword's, shield's
        char name[8] = "@handAL";
        name[5] = x.child.load() ? 'C' : 'A';
        for (int h = 0; h < 2; h++)
            if (!held[h == f.sword ? 0 : 1]) name[6] = h ? 'R' : 'L', f.bare[h] = hand_item(name);
    }
    if (!f.gear[0] && !f.gear[1] && !f.gear[2] && !f.bare[0] && !f.bare[1]) return false;
    if (!x.hand_tried) {
        x.hand_tried = true;
        if (!make_hand_pipelines()) {
            rt_log("vr: hands: the pipelines could not be made");
            x.hand_pipe = x.mix_pipe = VK_NULL_HANDLE;
        }
    }
    if (!x.hand_pipe || !x.mix_pipe) return false;
    {
        std::lock_guard<std::mutex> lk(x.hands_mu);
        for (int h = 0; h < 2; h++) f.grips[h] = x.grip[h], f.ok[h] = x.grip_ok[h];
    }
    // the models drawn at three quarters of the scene's size (the card's memory is scarce), filtered up into it
    const uint32_t hw = std::max(1u, x.hud_w * 3 / 4), hh = std::max(1u, x.hud_h * 3 / 4);
    if (!x.hand_color || x.hand_w != hw || x.hand_h != hh) {   // (another size's are left behind)
        const VkImageUsageFlags u = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!make_image(hw, hh, VK_FORMAT_R8G8B8A8_SRGB, u, x.hand_color, x.hand_color_view) ||
            !make_image(hw, hh, VK_FORMAT_R16_SFLOAT, u, x.hand_dist, x.hand_dist_view) ||
            !make_image(hw, hh, VK_FORMAT_D16_UNORM, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, x.hand_depth,
                        x.hand_depth_view, VK_IMAGE_ASPECT_DEPTH_BIT)) {
            rt_log("vr: hands: targets %ux%u could not be made", hw, hh);
            x.hand_pipe = VK_NULL_HANDLE;
            return false;
        }
        x.hand_w = hw, x.hand_h = hh;
    }
    for (auto *it : f.gear)
        if (it && !it->uploaded && it->cpu.verts && !upload_item(cb, *it)) it->cpu.verts = nullptr;   // (not again)
    for (auto *it : f.bare)
        if (it && !it->uploaded && it->cpu.verts && !upload_item(cb, *it)) it->cpu.verts = nullptr;
    return true;
}

// the models drawn from the head's pose moved `side` metres along its right (an eye), with the picture's lens:
// clip = (x / tan_x, -y / tan_y, near, -z), the depth reversed
void hands_draw(VkCommandBuffer cb, const Drawn &d, const HandsFrame &f, float side) {
    Xr &x = xr();
    Pose eye = d.pose;
    static const float right[3] = {1, 0, 0};
    float r[3];
    rotate(d.pose.q, right, r);
    for (int c = 0; c < 3; c++) eye.p[c] += side * r[c];
    const Mat4 view = inverse_rigid(pose_matrix(eye));
    const float tx = d.lens.r * d.scale, ty = d.lens.u * d.scale, near = 0.01f;
    Mat4 proj{};
    proj.m[0] = 1.0f / tx, proj.m[5] = -1.0f / ty, proj.m[11] = -1.0f, proj.m[14] = near;
    barrier(cb, x.hand_color, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    barrier(cb, x.hand_dist, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    barrier(cb, x.hand_depth, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkRenderingAttachmentInfo ca[2]{};
    for (int i = 0; i < 2; i++) {
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        ca[i].imageView = i ? x.hand_dist_view : x.hand_color_view;
        ca[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView = x.hand_depth_view, da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    da.clearValue.depthStencil = {0.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {x.hand_w, x.hand_h}}, ri.layerCount = 1;
    ri.colorAttachmentCount = 2, ri.pColorAttachments = ca, ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cb, &ri);
    const VkViewport vp{0, 0, (float)x.hand_w, (float)x.hand_h, 0, 1};
    const VkRect2D sc{{0, 0}, {x.hand_w, x.hand_h}};
    vkCmdSetViewport(cb, 0, 1, &vp), vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, x.hand_pipe);
    for (int i = 0; i < 5; i++) {   // the sword, the shield, the item, then Link's own left and right hands
        const Xr::HandItem *at = i < 3 ? f.gear[i] : f.bare[i - 3];
        if (!at || (i == 1 && f.two_handed)) continue;
        const Xr::HandItem &it = *at;
        const int hand = i >= 3 ? i - 3 : i == 1 ? 1 - f.sword : f.sword;
        if (!it.uploaded || !f.ok[hand]) continue;
        Mat4 in_grip{};   // a hand's model is in the grip's space already
        in_grip.m[0] = in_grip.m[5] = in_grip.m[10] = in_grip.m[15] = 1;
        const Mat4 mv = mul(view, mul(pose_matrix(f.grips[hand]), i >= 3 ? in_grip : held(i, hand == 1, it.cpu, it.name)));
        const Mat4 clip = mul(proj, mv);
        float pc[32];
        std::memcpy(pc, clip.m, sizeof clip.m), std::memcpy(pc + 16, mv.m, sizeof mv.m);
        vkCmdPushConstants(cb, x.hand_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, pc);
        const VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cb, 0, 1, &it.vb, &zero);
        vkCmdBindIndexBuffer(cb, it.ib, 0, VK_INDEX_TYPE_UINT32);
        for (int k = 0; k < it.cpu.parts; k++) {   // each part with its own texture
            const VkDescriptorImageInfo ti{x.hand_sampler, it.tex_view[k], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstBinding = 0, w.descriptorCount = 1, w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &ti;
            x.push_set(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, x.hand_layout, 0, 1, &w);
            vkCmdDrawIndexed(cb, it.cpu.part[k].count, 1, it.cpu.part[k].first, 0, 0);
        }
    }
    vkCmdEndRendering(cb);
    barrier(cb, x.hand_color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    barrier(cb, x.hand_dist, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
}

// the drawn models put into `target` (an RGBA8 storage image of the scene's size, in GENERAL) where nearer than the scene
void hands_mix(VkCommandBuffer cb, const HandsFrame &f, VkImage target, VkImageView target_view) {
    Xr &x = xr();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, x.mix_pipe);
    const VkDescriptorImageInfo info[4] = {{x.hand_sampler, x.hand_color_view, VK_IMAGE_LAYOUT_GENERAL},
                                           {x.nearest, x.hand_dist_view, VK_IMAGE_LAYOUT_GENERAL},
                                           {VK_NULL_HANDLE, target_view, VK_IMAGE_LAYOUT_GENERAL},
                                           {VK_NULL_HANDLE, x.depth_view, VK_IMAGE_LAYOUT_GENERAL}};
    const VkDescriptorBufferInfo rows{x.rows, 0, 8ull * x.depth_h};
    VkWriteDescriptorSet w[5]{};
    for (uint32_t i = 0; i < 5; i++) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, w[i].dstBinding = i, w[i].descriptorCount = 1;
        w[i].descriptorType = i == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                              : i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        if (i == 4) w[i].pBufferInfo = &rows;
        else w[i].pImageInfo = &info[i];
    }
    x.push_set(cb, VK_PIPELINE_BIND_POINT_COMPUTE, x.mix_layout, 0, 5, w);
    int32_t pc[6] = {(int32_t)x.hud_w, (int32_t)x.hud_h, (int32_t)x.depth_w, (int32_t)x.depth_h, 0, 0};
    std::memcpy(&pc[4], &f.planes[0], 4), std::memcpy(&pc[5], &f.planes[1], 4);
    vkCmdPushConstants(cb, x.mix_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (x.hud_w + 7) / 8, (x.hud_h + 7) / 8, 1);
    barrier(cb, target, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
}

// (vk_xr_capture, hud_mu held, after make_eyes) the hands into the headset's pictures: each eye's from that eye (true
// stereo for them, whatever depth stereo does to the world: the same eyes' distance it uses), else the scene's; and
// into the desktop's overlay from the head, for hands_onto_window. Whether the overlay holds hands this present.
bool make_hands(VkCommandBuffer cb, const Drawn &d) {
    Xr &x = xr();
    HandsFrame f;
    if (!hands_ready(cb, d, f)) return false;
    if (d.stereo) {
        const float strength = 0.01f * (float)cfg_int("NX2NAVI_VR_DEPTH");
        for (int e = 0; e < 2; e++) {
            hands_draw(cb, d, f, (e ? 0.5f : -0.5f) * d.ipd * strength);
            hands_mix(cb, f, x.eye8[e], x.eye8_view[e]);
        }
    } else {
        hands_draw(cb, d, f, 0.0f);
        hands_mix(cb, f, x.scene8, x.scene8_view);
    }
    // the desktop's overlay: the scene's size, clear but for the hands from the head
    if (!x.win_overlay || x.win_overlay_w != x.hud_w || x.win_overlay_h != x.hud_h) {
        const VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!make_image(x.hud_w, x.hud_h, VK_FORMAT_R8G8B8A8_UNORM, u, x.win_overlay, x.win_overlay_view)) {
            x.win_overlay = VK_NULL_HANDLE;
            return false;
        }
        x.win_overlay_w = x.hud_w, x.win_overlay_h = x.hud_h;
    }
    barrier(cb, x.win_overlay, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    const VkClearColorValue clear{};
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, x.win_overlay, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &all);
    barrier(cb, x.win_overlay, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    if (d.stereo) hands_draw(cb, d, f, 0.0f);
    hands_mix(cb, f, x.win_overlay, x.win_overlay_view);
    return true;
}

// The desktop's blend of the overlay onto the window (shaders/xr_hand_blit_vs.vert, xr_hand_blit_fs.frag), made for
// the window's format
bool make_blit_pipeline(VkFormat format) {
    Xr &x = xr();
    if (!x.blit_set) {
        const VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, li.bindingCount = 1, li.pBindings = &b;
        if (vkCreateDescriptorSetLayout(x.device, &li, nullptr, &x.blit_set) != VK_SUCCESS) return false;
        const VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(int32_t)};
        VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pi.setLayoutCount = 1, pi.pSetLayouts = &x.blit_set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
        if (vkCreatePipelineLayout(x.device, &pi, nullptr, &x.blit_layout) != VK_SUCCESS) return false;
    }
    const VkShaderModule vs = shader(xr_hand_blit_vs_spv, sizeof xr_hand_blit_vs_spv),
                         fs = shader(xr_hand_blit_fs_spv, sizeof xr_hand_blit_fs_spv);
    if (!vs || !fs) return false;
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
    st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = VK_CULL_MODE_NONE, rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_TRUE, cba.colorWriteMask = 0x7;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE, cba.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1, cb.pAttachments = &cba;
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2, dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1, ri.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri, gi.stageCount = 2, gi.pStages = st, gi.pVertexInputState = &vi, gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp, gi.pRasterizationState = &rs, gi.pMultisampleState = &ms, gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb, gi.pDynamicState = &dy, gi.layout = x.blit_layout;
    const VkResult r = vkCreateGraphicsPipelines(x.device, VK_NULL_HANDLE, 1, &gi, nullptr, &x.blit_pipe);
    vkDestroyShaderModule(x.device, vs, nullptr), vkDestroyShaderModule(x.device, fs, nullptr);
    x.blit_format = format;
    return r == VK_SUCCESS;
}

// (vk_xr_capture, last, hud_mu held) the overlay's hands blended onto the window where the game's picture sits in it
// (gfx_fit_rect, as the present placed it), so the desktop shows them too; the window back in TRANSFER_SRC after
void hands_onto_window(VkCommandBuffer cb, VkImage window, VkExtent2D extent, VkFormat format, const int rect[4]) {
    Xr &x = xr();
    if (!x.win_overlay) return;
    if (x.blit_format != format || !x.blit_pipe) {
        x.blit_pipe = VK_NULL_HANDLE;
        if (!make_blit_pipeline(format)) {
            static bool told;
            if (!told) rt_log("vr: hands: the desktop's blend could not be made"), told = true;
            x.blit_format = format;
            return;
        }
    }
    VkImageView view = VK_NULL_HANDLE;   // the window's images' views, kept by image
    for (int i = 0; i < 8 && !view; i++)
        if (x.win_image[i] == window) view = x.win_view[i];
    if (!view) {
        VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vc.image = window, vc.viewType = VK_IMAGE_VIEW_TYPE_2D, vc.format = format, vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(x.device, &vc, nullptr, &view) != VK_SUCCESS) return;
        const int k = x.win_next++ % 8;   // (a view of an image long gone is left behind)
        x.win_image[k] = window, x.win_view[k] = view;
    }
    int to[4];
    gfx_fit_rect(rect[2], rect[3], (int)extent.width, (int)extent.height, to);
    barrier(cb, window, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = view, ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent}, ri.layerCount = 1, ri.colorAttachmentCount = 1, ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cb, &ri);
    const VkViewport vp{(float)to[0], (float)to[1], (float)to[2], (float)to[3], 0, 1};
    const VkRect2D sc{{to[0], to[1]}, {(uint32_t)to[2], (uint32_t)to[3]}};
    vkCmdSetViewport(cb, 0, 1, &vp), vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, x.blit_pipe);
    const VkDescriptorImageInfo ti{x.hand_sampler, x.win_overlay_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstBinding = 0, w.descriptorCount = 1, w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, w.pImageInfo = &ti;
    x.push_set(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, x.blit_layout, 0, 1, &w);
    const int32_t srgb = is_srgb(format) ? 1 : 0;
    vkCmdPushConstants(cb, x.blit_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof srgb, &srgb);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    barrier(cb, window, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
}

// Stereo Balanced (vk_xr_capture, hud_mu held, after make_hands): this eye's picture (scene8, drawn from it) kept with
// its pose; once both eyes have one the picture is shown in stereo, each eye its latest at its own pose
void keep_eye(VkCommandBuffer cb, Drawn &d) {
    Xr &x = xr();
    if (!x.acc8[0] || x.acc_w != x.hud_w || x.acc_h != x.hud_h) {   // (another size's are left behind)
        const VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        for (int e = 0; e < 3; e++)
            if (!make_image(x.hud_w, x.hud_h, VK_FORMAT_R8G8B8A8_UNORM, u, x.acc8[e], x.acc8_view[e])) {
                x.acc8[0] = x.acc8[1] = x.acc8[2] = VK_NULL_HANDLE;
                return;
            }
        x.acc_w = x.hud_w, x.acc_h = x.hud_h, x.acc_have[0] = x.acc_have[1] = x.acc_have[2] = false, x.pair_ok = false;
        for (int e = 0; e < 3; e++) barrier(cb, x.acc8[e], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    }
    // Accurate: the left eye's picture waits in [2] while the pair before stays shown, and goes to [0] with its right
    const bool paired = stereo_accurate();
    if (!paired) x.pair_ok = false;
    const int into = paired && d.eye == 0 ? 2 : d.eye;
    VkImageCopy c{};
    c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.extent = {x.hud_w, x.hud_h, 1};
    vkCmdCopyImage(cb, x.scene8, VK_IMAGE_LAYOUT_GENERAL, x.acc8[into], VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    barrier(cb, x.acc8[into], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    x.acc_pose[into] = d.pose, x.acc_have[into] = true;
    std::memcpy(x.acc_base[into], d.base, sizeof d.base), x.acc_bx[into] = d.bx, x.acc_bz[into] = d.bz;
    if (paired) {
        if (d.eye == 0) {
            if (x.pair_ok) d.stereo = true, d.eye_pose[0] = x.pair_pose[0], d.eye_pose[1] = x.pair_pose[1];
            return;
        }
        if (!x.acc_have[2]) return;
        std::swap(x.acc8[0], x.acc8[2]), std::swap(x.acc8_view[0], x.acc8_view[2]);
        x.acc_pose[0] = x.acc_pose[2], std::memcpy(x.acc_base[0], x.acc_base[2], sizeof x.acc_base[0]);
        x.acc_bx[0] = x.acc_bx[2], x.acc_bz[0] = x.acc_bz[2], x.acc_have[0] = true, x.acc_have[2] = false;
    }
    if (x.acc_have[0] && x.acc_have[1]) {
        d.stereo = true;
        d.eye_pose[d.eye] = d.pose;
        // the other eye's picture, drawn a frame before with Link (and the game's camera) elsewhere in the world: its
        // pose put in this picture's LOCAL space, local = A_now^T (base_then - base_now + A_then local_then), turned by
        // the yaw between the two
        const int o = 1 - d.eye;
        const Pose &then = x.acc_pose[o];
        const double ob = x.acc_bx[o], oz = x.acc_bz[o];
        const double then_axes[3][3] = {{oz, 0, -ob}, {0, 1, 0}, {ob, 0, oz}}, now_axes[3][3] = {{d.bz, 0, -d.bx}, {0, 1, 0}, {d.bx, 0, d.bz}};
        double w[3];
        for (int c = 0; c < 3; c++)
            w[c] = x.acc_base[o][c] - d.base[c] + then_axes[0][c] * then.p[0] + then_axes[1][c] * then.p[1] + then_axes[2][c] * then.p[2];
        Pose moved = then;
        for (int k = 0; k < 3; k++) moved.p[k] = (float)(now_axes[k][0] * w[0] + now_axes[k][1] * w[1] + now_axes[k][2] * w[2]);
        // the yaw from then's axes to now's: then's right in now's axes (cos, and the back component as sin)
        const double c0 = now_axes[0][0] * then_axes[0][0] + now_axes[0][2] * then_axes[0][2];
        const double s0 = now_axes[2][0] * then_axes[0][0] + now_axes[2][2] * then_axes[0][2];
        const double half = 0.5 * std::atan2(-s0, c0);
        const float qy[4] = {0, (float)std::sin(half), 0, (float)std::cos(half)};   // about y, x y z w
        const float *a = qy, *b = then.q;   // moved.q = qy * then.q
        moved.q[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
        moved.q[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
        moved.q[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
        moved.q[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
        d.eye_pose[o] = moved;
        if (paired) x.pair_pose[0] = d.eye_pose[0], x.pair_pose[1] = d.eye_pose[1], x.pair_ok = true;
    }
}

// the image an eye's picture is in with stereo: Balanced's kept pictures, else depth stereo's
VkImage eye_image(const Drawn &d, int e) { return d.eye >= 0 ? xr().acc8[e] : xr().eye8[e]; }

// Stereo Balanced (vk_xr_capture, last, hud_mu held): the desktop shows the left eye. Its presents' window picture
// (with the HUD and the hands) is kept, and put back over the window on the right eye's presents, so the window does
// not shift between the eyes every frame; the window back in TRANSFER_SRC after
void window_left_eye(VkCommandBuffer cb, VkImage window, VkExtent2D extent, VkFormat format, const int rect[4], int eye) {
    Xr &x = xr();
    int to[4];
    gfx_fit_rect(rect[2], rect[3], (int)extent.width, (int)extent.height, to);
    if (to[2] <= 0 || to[3] <= 0) return;
    if (!x.win_left || x.win_left_w != (uint32_t)to[2] || x.win_left_h != (uint32_t)to[3] || x.win_left_format != format) {
        x.win_left_have = false;
        VkImageView unused = VK_NULL_HANDLE;
        if (!make_image((uint32_t)to[2], (uint32_t)to[3], format, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        x.win_left, unused)) {
            x.win_left = VK_NULL_HANDLE;
            return;
        }
        x.win_left_w = (uint32_t)to[2], x.win_left_h = (uint32_t)to[3], x.win_left_format = format;
        barrier(cb, x.win_left, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    }
    VkImageCopy c{};
    c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.extent = {(uint32_t)to[2], (uint32_t)to[3], 1};
    if (eye == 0) {
        c.srcOffset = {to[0], to[1], 0};
        vkCmdCopyImage(cb, window, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, x.win_left, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        barrier(cb, x.win_left, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        x.win_left_have = true;
    } else if (x.win_left_have) {
        c.dstOffset = {to[0], to[1], 0};
        barrier(cb, window, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyImage(cb, x.win_left, VK_IMAGE_LAYOUT_GENERAL, window, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        barrier(cb, window, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
}

// (vk_xr_capture, hud_mu held, after make_panel) each eye's picture from the scene make_panel left in scene8 and this
// present's scene depth: the eye half the eyes' distance (NX2NAVI_VR_DEPTH percent of it) to its side of the head's
// view; false without the depth (the head's view then shows the one picture to both eyes)
bool make_eyes(VkCommandBuffer cb, const Drawn &d) {
    Xr &x = xr();
    float planes[2];
    if (!vk_dlaa_vr_planes(planes) || !x.depth_kept) {
        x.stereo_missed++;
        return false;
    }
    if (!x.depth_tried) {
        x.depth_tried = true;
        if (!make_depth_pipeline()) {
            rt_log("vr: the depth stereo shader could not be made: one picture to both eyes");
            x.depth_pipe = VK_NULL_HANDLE;
        }
    }
    if (!x.depth_pipe) return false;
    if (!x.eye8[0] || x.eye8_w != x.hud_w || x.eye8_h != x.hud_h) {   // (another size's are left behind, as the scene's)
        for (int e = 0; e < 2; e++)
            if (!make_image(x.hud_w, x.hud_h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            x.eye8[e], x.eye8_view[e])) {
                rt_log("vr: the depth stereo eye images %ux%u could not be made: one picture to both eyes", x.hud_w, x.hud_h);
                x.depth_pipe = VK_NULL_HANDLE;
                return false;
            }
        x.eye8_w = x.hud_w, x.eye8_h = x.hud_h;
        rt_log("vr: depth stereo: eye pictures %ux%u", x.hud_w, x.hud_h);
    }
    const float tan_x = d.lens.r * d.scale;   // the game's lens: x_ndc = view x / (z tan_x)
    const float strength = 0.01f * (float)cfg_int("NX2NAVI_VR_DEPTH");
    const float near_inv = 1.0f / 0.01f;   // a guard alone: everything has its own depth down to a centimetre
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, x.depth_pipe);
    for (int e = 0; e < 2; e++) {
        barrier(cb, x.eye8[e], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        const VkDescriptorImageInfo info[3] = {{x.linear, x.scene8_view, VK_IMAGE_LAYOUT_GENERAL},
                                               {x.nearest, x.depth_view, VK_IMAGE_LAYOUT_GENERAL},
                                               {VK_NULL_HANDLE, x.eye8_view[e], VK_IMAGE_LAYOUT_GENERAL}};
        const VkDescriptorBufferInfo rows{x.rows, 0, 8ull * x.depth_h};
        VkWriteDescriptorSet w[4]{};
        for (uint32_t i = 0; i < 4; i++) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, w[i].dstBinding = i, w[i].descriptorCount = 1;
            w[i].descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                  : i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            if (i == 3) w[i].pBufferInfo = &rows;
            else w[i].pImageInfo = &info[i];
        }
        x.push_set(cb, VK_PIPELINE_BIND_POINT_COMPUTE, x.depth_layout, 0, 4, w);
        const float side = (e ? 0.5f : -0.5f) * d.ipd * strength;   // the left eye to the left: the scene moves right in it
        // the largest move, at most a quarter of the picture: the shader scans no further
        const float most = std::min(0.25f, 0.5f * std::fabs(side / tan_x) * near_inv + 0.002f);
        const float pc[7] = {(float)x.hud_w, (float)x.hud_h, side / tan_x, planes[0], planes[1], most, near_inv};
        vkCmdPushConstants(cb, x.depth_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
        vkCmdDispatch(cb, (x.hud_w + 7) / 8, (x.hud_h + 7) / 8, 1);
        barrier(cb, x.eye8[e], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    }
    x.stereo_pictures++;
    return true;
}

// The hand controllers through OpenXR's actions, bound for the Touch controllers (Quest), read each headset frame onto
// the pad's buttons (input_map.h bits): A and B their own, X and Y A and B again; either stick's click X (jump); the left grip ZL (the Z lock), the right grip ZR; the left Menu tapped Plus (the inventory), held a second
// Minus; the sword hand (NX2NAVI_VR_HANDEDNESS, the left by default) swung fast Y tapped, or held while the swing went
// outward and left the hand out to the side (the charge), until the hand comes back in. The left trigger held opens the D-pad wheel, the right one the
// item wheel (vr_wheel.c; ui/menu.cpp draws them): that hand's stick held toward a slot picks it and the trigger's
// release uses it (the D-pad's direction pressed, or the item equipped by vr_game.c), a flick toward a slot swaps what
// it holds. The shield hand's shield turned to face ahead before its own side's eye holds ZL (the Z lock), as Link
// holds his shield. Both arms shaken upward press the D-pad's down (the boots), both hands together before the face
// its left (the ocarina), both hands beside the head its right (Navi), a hand shaken sideways above the head its up
// (the mask). Both hands brought together behind the head draw the sword and shield (a tap of Y) or, while they are out,
// put them away (B). While Link plays the ocarina (vk_xr_ocarina) the notes swap places: the right stick's left, up, right and
// down play Y, X, A and R, the A button L; no sword, jump or wheel meanwhile. NX2NAVI_VR_CONTROLS=meta puts them on the
// pad (vk_xr_pad); gamepad leaves the pad to the gamepad.
struct Controls {
    enum { A = 0, B, X, Y, STICK_L, STICK_R, L, R, ZL, ZR, PLUS, MINUS, LEFT, UP, RIGHT, DOWN };
    // the D-pad wheel's slots, 0 up and clockwise: the accessory, Navi, the boots, the ocarina
    static constexpr int dpad_slots[4] = {UP, RIGHT, DOWN, LEFT};
    int wheel = 0;
    int wheel_slot = -1, dpad_bit = -1;
    // a tap: the stick toward a slot and back to the middle within flick_ns steps the slot's variants (vr_wheel_tap)
    static constexpr uint64_t flick_ns = 300000000ull;
    int tap_slot = -1;
    uint64_t tap_since = 0;
    uint64_t dpad_from = 0, dpad_until = 0;
    // after a wheel closes the D-pad's press waits this long: the game's own quick select, held while a wheel is open, is
    // closing and would take it
    static constexpr uint64_t after_dpad_ns = 200000000ull;
    static constexpr uint64_t press_ns = 120000000ull, hold_ns = 1000000000ull;
    XrActionSet set = XR_NULL_HANDLE;
    XrAction stick = XR_NULL_HANDLE, click = XR_NULL_HANDLE, trigger = XR_NULL_HANDLE, squeeze = XR_NULL_HANDLE,
             grip = XR_NULL_HANDLE, a = XR_NULL_HANDLE, b = XR_NULL_HANDLE, xb = XR_NULL_HANDLE, yb = XR_NULL_HANDLE,
             menu = XR_NULL_HANDLE, aim = XR_NULL_HANDLE;
    XrPath hand[2]{};
    XrSpace grip_space[2]{}, aim_space[2]{};
    bool ok = false;
    bool trigger_on[2]{}, squeeze_on[2]{};
    uint64_t menu_down = 0, plus_until = 0, minus_until = 0;   // plat_now_ns
    bool minus_sent = false;
    // the swing: the sword hand's grip positions over the last frames (plat_now_ns). A swing starts once the speed is past
    // swing_on (NX2NAVI_VR_SWING_SPEED), at least refractory_ns after the last one started (a swing's follow-through is
    // not another), and ends under swing_off. Each is a tap of Y (tap_ns down, at least gap_ns up
    // before the next, one swing more kept for then), held on as the charge when it went outward and the hand is
    // out_on (NX2NAVI_VR_CHARGE_REACH) from the head's upright line at the tap's end, released once the hand is back
    // within out_off, 10 cm nearer
    static constexpr float swing_off = 1.0f;   // metres a second
    static constexpr uint64_t refractory_ns = 300000000ull;
    uint64_t swing_at = 0;
    static constexpr uint64_t tap_ns = 80000000ull, gap_ns = 80000000ull;
    float swing_p[4][3]{};
    uint64_t swing_t[4]{};
    int swing_n = 0, swing_hand = 1;
    bool swinging = false, outward = false, charging = false, queued = false;
    uint64_t y_down = 0, y_up = 0;   // the tap's press and release (0: none now)
    // the draw: both grips behind the head's eyes (behind_on), no lower than shoulder_drop under them and within
    // together_on of each other; once a draw, again only after the hands parted (apart_off) and refractory
    static constexpr float behind_on = 0.03f, shoulder_drop = 0.30f, together_on = 0.15f, apart_off = 0.35f;
    static constexpr uint64_t draw_refractory_ns = 800000000ull;
    // the D-pad's gestures (in play): both arms shaken upward the boots (down), both hands together before the face the
    // ocarina (left), both hands beside the head Navi (right), a hand shaken sideways above the head the mask (up); each
    // once, then not again for dpad_refractory_ns; the poses held dwell_ns first
    static constexpr uint64_t dpad_refractory_ns = 1000000000ull, dwell_ns = 250000000ull;
    float hand_p[2][4][3]{};
    uint64_t hand_t[2][4]{};
    int hand_n[2]{};
    uint64_t pose_since[2]{};   // the ocarina's and Navi's poses: held since (0 not held)
    uint64_t dpad_gesture_at = 0;
    bool draw_armed = true;
    bool grip_zr[2]{};   // in play: the item button held by that grip (pressed behind the head, or with an item out)
    int tap_bit = Y;     // what the swing's tap presses: Y (the sword) or ZR (the hammer, the stick, an empty bottle)
    static constexpr float shield_on = 0.79f, shield_off = 1.05f, shield_reach = 0.70f;   // radians, radians, metres
    bool shielding = false;
    uint64_t draw_at = 0, draw_until = 0;
    int draw_bit = -1;

    XrPath path(const char *s) {
        XrPath p = XR_NULL_PATH;
        xr().xrStringToPath(xr().instance, s, &p);
        return p;
    }
    XrAction action(const char *name, XrActionType type, bool both) {
        XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
        std::strcpy(ai.actionName, name);
        std::strcpy(ai.localizedActionName, name);
        ai.actionType = type;
        ai.countSubactionPaths = both ? 2 : 0;
        ai.subactionPaths = both ? hand : nullptr;
        XrAction out = XR_NULL_HANDLE;
        if (XR_FAILED(xr().xrCreateAction(set, &ai, &out))) rt_log("vr: xrCreateAction %s failed", name);
        return out;
    }
    void make(XrSession session) {
        Xr &x = xr();
        XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strcpy(si.actionSetName, "gameplay");
        std::strcpy(si.localizedActionSetName, "Gameplay");
        XrResult r = x.xrCreateActionSet(x.instance, &si, &set);
        if (XR_FAILED(r)) {
            rt_log("vr: xrCreateActionSet failed (%d): no hand controllers", (int)r);
            return;
        }
        hand[0] = path("/user/hand/left"), hand[1] = path("/user/hand/right");
        stick = action("stick", XR_ACTION_TYPE_VECTOR2F_INPUT, true);
        click = action("stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
        trigger = action("trigger", XR_ACTION_TYPE_FLOAT_INPUT, true);
        squeeze = action("squeeze", XR_ACTION_TYPE_FLOAT_INPUT, true);
        grip = action("grip", XR_ACTION_TYPE_POSE_INPUT, true);
        aim = action("aim", XR_ACTION_TYPE_POSE_INPUT, true);
        a = action("a", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
        b = action("b", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
        xb = action("x", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
        yb = action("y", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
        menu = action("menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
        const struct {
            XrAction act;
            const char *at;
        } binds[] = {
            {stick, "/user/hand/left/input/thumbstick"},         {stick, "/user/hand/right/input/thumbstick"},
            {click, "/user/hand/left/input/thumbstick/click"},   {click, "/user/hand/right/input/thumbstick/click"},
            {trigger, "/user/hand/left/input/trigger/value"},    {trigger, "/user/hand/right/input/trigger/value"},
            {squeeze, "/user/hand/left/input/squeeze/value"},    {squeeze, "/user/hand/right/input/squeeze/value"},
            {grip, "/user/hand/left/input/grip/pose"},           {grip, "/user/hand/right/input/grip/pose"},
            {aim, "/user/hand/left/input/aim/pose"},             {aim, "/user/hand/right/input/aim/pose"},
            {a, "/user/hand/right/input/a/click"},               {b, "/user/hand/right/input/b/click"},
            {xb, "/user/hand/left/input/x/click"},               {yb, "/user/hand/left/input/y/click"},
            {menu, "/user/hand/left/input/menu/click"},
        };
        XrActionSuggestedBinding sb[sizeof binds / sizeof binds[0]];
        for (size_t i = 0; i < sizeof binds / sizeof binds[0]; i++) sb[i] = {binds[i].act, path(binds[i].at)};
        XrInteractionProfileSuggestedBinding pb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        pb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
        pb.countSuggestedBindings = sizeof sb / sizeof sb[0];
        pb.suggestedBindings = sb;
        r = x.xrSuggestInteractionProfileBindings(x.instance, &pb);
        if (XR_FAILED(r)) {
            rt_log("vr: the Touch controllers' bindings were refused (%d): no hand controllers", (int)r);
            return;
        }
        XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        at.countActionSets = 1, at.actionSets = &set;
        r = x.xrAttachSessionActionSets(session, &at);
        if (XR_FAILED(r)) {
            rt_log("vr: xrAttachSessionActionSets failed (%d): no hand controllers", (int)r);
            return;
        }
        for (int h = 0; h < 2; h++) {
            XrActionSpaceCreateInfo sci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            sci.action = grip, sci.subactionPath = hand[h];
            sci.poseInActionSpace.orientation.w = 1.0f;
            x.xrCreateActionSpace(session, &sci, &grip_space[h]);
            sci.action = aim;
            x.xrCreateActionSpace(session, &sci, &aim_space[h]);
        }
        ok = true;
        rt_log("vr: hand controllers bound (Touch)");
    }
    bool on(XrSession session, XrAction act, XrPath sub) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = act, gi.subactionPath = sub;
        XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
        return XR_SUCCEEDED(xr().xrGetActionStateBoolean(session, &gi, &st)) && st.isActive && st.currentState;
    }
    float value(XrSession session, XrAction act, XrPath sub) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = act, gi.subactionPath = sub;
        XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
        return XR_SUCCEEDED(xr().xrGetActionStateFloat(session, &gi, &st)) && st.isActive ? st.currentState : 0.0f;
    }
    // this headset frame's buttons, sticks and grips (time: the frame's predicted display time) into the Xr's
    void read(XrSession session, XrSpace space, XrTime time, const Pose &head_pose) {
        const float *head = head_pose.p;
        if (!ok) return;
        Xr &x = xr();
        XrActiveActionSet active{set, XR_NULL_PATH};
        XrActionsSyncInfo sy{XR_TYPE_ACTIONS_SYNC_INFO};
        sy.countActiveActionSets = 1, sy.activeActionSets = &active;
        if (x.xrSyncActions(session, &sy) != XR_SUCCESS) return;   // (XR_SESSION_NOT_FOCUSED: nothing to read)
        const uint64_t now = plat_now_ns();
        uint64_t bits = 0;
        const auto set_bit = [&](int bit, bool down) { bits |= down ? 1ull << bit : 0; };
        set_bit(A, on(session, a, XR_NULL_PATH) || on(session, xb, XR_NULL_PATH));
        set_bit(B, on(session, b, XR_NULL_PATH) || on(session, yb, XR_NULL_PATH));
        set_bit(X, on(session, click, hand[0]) || on(session, click, hand[1]));
        bool squeezed_before[2] = {squeeze_on[0], squeeze_on[1]};
        for (int h = 0; h < 2; h++) {   // pressed past 0.6, released under 0.4
            const float t = value(session, trigger, hand[h]), s = value(session, squeeze, hand[h]);
            trigger_on[h] = trigger_on[h] ? t > 0.4f : t > 0.6f;
            squeeze_on[h] = squeeze_on[h] ? s > 0.4f : s > 0.6f;
        }
        const bool in_play = x.in_play.load();
        if (!in_play) set_bit(ZL, squeeze_on[0]), set_bit(ZR, squeeze_on[1]);   // menus and events: the grips as ever
        // the Menu: a tap is Plus for a moment at its release, a second's hold Minus for a moment then
        if (on(session, menu, XR_NULL_PATH)) {
            if (!menu_down) menu_down = now, minus_sent = false;
            if (!minus_sent && now - menu_down >= hold_ns) minus_until = now + press_ns, minus_sent = true;
        } else if (menu_down) {
            if (!minus_sent) plus_until = now + press_ns;
            menu_down = 0;
        }
        set_bit(PLUS, now < plus_until), set_bit(MINUS, now < minus_until);
        float sticks[2][2]{};
        for (int h = 0; h < 2; h++) {
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action = stick, gi.subactionPath = hand[h];
            XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
            if (XR_SUCCEEDED(x.xrGetActionStateVector2f(session, &gi, &st)) && st.isActive)
                sticks[h][0] = st.currentState.x, sticks[h][1] = st.currentState.y;
        }
        // the wheel: open while a trigger is held; the right stick pushed past half picks the slot it points at, which
        // stays picked when the stick comes back; the release uses it
        int used_kind = 0, used_slot = -1;
        if (wheel && !trigger_on[wheel - 1]) {   // its trigger let go: the slot held toward used
            const bool usable = wheel_slot >= 0 && vr_wheel_usable(wheel, wheel_slot);   // an empty slot does nothing
            if (wheel == VR_WHEEL_DPAD && usable)
                dpad_bit = dpad_slots[wheel_slot], dpad_from = now + after_dpad_ns, dpad_until = dpad_from + press_ns;
            x.wheel_closed_at.store(now);
            if (usable) used_kind = wheel, used_slot = wheel_slot;
            wheel = 0;
        }
        const bool ocarina = x.ocarina.load();   // playing the ocarina: no wheel (its held L would play a note)
        if (!wheel && !ocarina && (trigger_on[0] || trigger_on[1]) && x.wheel_allowed.load())
            wheel = trigger_on[0] ? 1 : 2, tap_slot = -1;
        const int was_slot = wheel_slot;
        wheel_slot = -1;
        if (wheel) {   // the wheel's own hand's stick picks
            const int n = vr_wheel_slots(wheel);
            const float sx = sticks[wheel - 1][0], sy = sticks[wheel - 1][1];
            if (sx * sx + sy * sy > 0.25f) {
                float a = std::atan2(sx, sy);   // 0 up, clockwise
                if (a < 0) a += 2.0f * 3.14159265f;
                wheel_slot = (int)std::floor(a / (2.0f * 3.14159265f / n) + 0.5f) % n;
            }
            if (wheel_slot >= 0 && wheel_slot != was_slot) tap_slot = wheel_slot, tap_since = now;
            if (wheel_slot < 0 && tap_slot >= 0) {
                if (now - tap_since < flick_ns) vr_wheel_tap(wheel, tap_slot);
                tap_slot = -1;
            }
        }
        if (now >= dpad_from && now < dpad_until) set_bit(dpad_bit, true);
        set_bit(L, wheel != 0);   // the game's own quick select held meanwhile: it pauses the game (its menu not shown)
        Pose poses[2];
        bool pose_ok[2]{};
        for (int h = 0; h < 2; h++) {
            XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
            const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
            if (grip_space[h] && XR_SUCCEEDED(x.xrLocateSpace(grip_space[h], space, time, &loc)) &&
                (loc.locationFlags & need) == need) {
                std::memcpy(poses[h].q, &loc.pose.orientation, sizeof poses[h].q);
                std::memcpy(poses[h].p, &loc.pose.position, sizeof poses[h].p);
                pose_ok[h] = true;
            }
        }
        // the head's level forward (fx, fz) and right (rx, rz), OpenXR's x right and z back, and where each hand is
        // behind the eyes along it (back_any), and so for the draw when no lower than the shoulders (behind)
        float fx = 0, fz = -1;
        {
            static const float ahead[3] = {0, 0, -1};
            float f[3];
            rotate(head_pose.q, ahead, f);
            const float n = std::sqrt(f[0] * f[0] + f[2] * f[2]);
            if (n > 1e-3f) fx = f[0] / n, fz = f[2] / n;
        }
        const float rx = -fz, rz = fx;
        float behind[2] = {-1.0f, -1.0f}, back_any[2] = {-1.0f, -1.0f}, side[2] = {};
        for (int h = 0; h < 2; h++)
            if (pose_ok[h]) {
                const float dx = poses[h].p[0] - head[0], dz = poses[h].p[2] - head[2];
                back_any[h] = -(dx * fx + dz * fz), side[h] = dx * rx + dz * rz;
                if (poses[h].p[1] > head[1] - shoulder_drop) behind[h] = back_any[h];
            }
        if (pose_ok[0] && pose_ok[1]) {
            float d2 = 0;
            for (int c = 0; c < 3; c++) d2 += (poses[0].p[c] - poses[1].p[c]) * (poses[0].p[c] - poses[1].p[c]);
            const bool drawn = behind[0] > behind_on && behind[1] > behind_on && d2 < together_on * together_on;
            if (drawn && draw_armed && now - draw_at >= draw_refractory_ns && !x.ocarina.load()) {
                draw_bit = x.weapon_out.load() ? B : Y;
                draw_at = now, draw_until = now + press_ns, draw_armed = false;
                y_down = 0, charging = false, queued = false;   // a swing on the way there is not one
            }
            if (!draw_armed && (d2 > apart_off * apart_off || behind[0] < 0 || behind[1] < 0)) draw_armed = true;
        }
        // the hands' velocities (over their last 4 frames) and places about the head: up, ahead (fwd) and to the right
        float hv[2][3]{}, hspeed[2]{}, up[2]{}, fwd[2]{};
        for (int h = 0; h < 2; h++) {
            if (!pose_ok[h]) {
                hand_n[h] = 0;
                continue;
            }
            const int i = hand_n[h]++ & 3;
            std::memcpy(hand_p[h][i], poses[h].p, sizeof hand_p[h][i]);
            hand_t[h][i] = now;
            up[h] = poses[h].p[1] - head[1], fwd[h] = -back_any[h];
            if (hand_n[h] < 4) continue;
            const int o = hand_n[h] & 3;
            const float dt = (float)(hand_t[h][i] - hand_t[h][o]) * 1e-9f;
            for (int c = 0; c < 3 && dt > 0; c++) hv[h][c] = (hand_p[h][i][c] - hand_p[h][o][c]) / dt;
            hspeed[h] = std::sqrt(hv[h][0] * hv[h][0] + hv[h][1] * hv[h][1] + hv[h][2] * hv[h][2]);
        }
        const float shake = 0.008f * (float)cfg_int("NX2NAVI_VR_SWING_SPEED");   // 80% of a swing's speed, m/s
        // both arms shaken upward together; a hand shaken (mostly sideways) above the head
        const bool both_up = hspeed[0] > shake && hspeed[1] > shake && hv[0][1] > 0.7f * hspeed[0] && hv[1][1] > 0.7f * hspeed[1];
        bool wave[2];
        for (int h = 0; h < 2; h++)
            wave[h] = pose_ok[h] && up[h] > 0.15f && hspeed[h] > shake &&
                      hv[h][0] * hv[h][0] + hv[h][2] * hv[h][2] > 0.49f * hspeed[h] * hspeed[h];
        // both hands together before the face (the ocarina held to the mouth); both beside the head (Navi called)
        bool at_face = false, beside = false;
        if (pose_ok[0] && pose_ok[1]) {
            float d2 = 0;
            for (int c = 0; c < 3; c++) d2 += (poses[0].p[c] - poses[1].p[c]) * (poses[0].p[c] - poses[1].p[c]);
            at_face = d2 < 0.16f * 0.16f && fwd[0] > 0.06f && fwd[1] > 0.06f && fwd[0] < 0.21f && fwd[1] < 0.21f &&
                      up[0] > -0.30f && up[1] > -0.30f && up[0] < 0.0f && up[1] < 0.0f &&
                      std::fabs(0.5f * (side[0] + side[1])) < 0.12f;
            beside = side[0] < -0.10f && side[0] > -0.35f && side[1] > 0.10f && side[1] < 0.35f && std::fabs(up[0]) < 0.15f &&
                     std::fabs(up[1]) < 0.15f && std::fabs(fwd[0]) < 0.15f && std::fabs(fwd[1]) < 0.15f;
        }
        pose_since[0] = at_face ? (pose_since[0] ? pose_since[0] : now) : 0;
        pose_since[1] = beside ? (pose_since[1] ? pose_since[1] : now) : 0;
        if (in_play && !x.ocarina.load() && !wheel && now - dpad_gesture_at >= dpad_refractory_ns) {
            int bit = -1;
            if (both_up) bit = DOWN;
            else if (wave[0] || wave[1]) bit = UP;
            else if (pose_since[0] && now - pose_since[0] >= dwell_ns) bit = LEFT;
            else if (pose_since[1] && now - pose_since[1] >= dwell_ns) bit = RIGHT;
            if (bit >= 0) {
                dpad_bit = bit, dpad_from = now, dpad_until = now + press_ns, dpad_gesture_at = now;
                y_down = 0, charging = false, queued = false;   // the shake is not a swing
                pose_since[0] = pose_since[1] = 0;
            }
        }
        // in play the grips are the item button alone (no Z lock: the shield's raise holds it): pressed with that hand
        // behind the head it takes the item out, and held while the grip stays held (reach back, draw forward, let go);
        // with an item out either grip uses it. Which item is out: vr_game.c's (vk_xr_hand_item)
        char held_item[96];
        {
            std::lock_guard<std::mutex> lk(x.gear_mu);
            std::memcpy(held_item, x.gear[2], sizeof held_item);
        }
        if (in_play) {
            for (int h = 0; h < 2; h++) {
                if (squeeze_on[h] && !squeezed_before[h]) grip_zr[h] = held_item[0] || behind[h] > behind_on;
                if (!squeeze_on[h]) grip_zr[h] = false;
            }
            set_bit(ZR, grip_zr[0] || grip_zr[1]);
        } else {
            grip_zr[0] = grip_zr[1] = false;
        }
        // the shield: the other hand's shield (drawn on the back of the hand, its face out of the grip's -x on the left
        // hand and +x on the right) turned to face ahead (within shield_on of the head's level forward) and held before
        // its own side's eye (within 12 cm of it to either side, 19 to stay) holds ZL (the Z lock) until it turns away
        // past shield_off, moves off from before the eye or is held out past shield_reach
        {
            const int shield = strcmp(cfg_value("NX2NAVI_VR_HANDEDNESS"), "right") == 0 ? 0 : 1;
            bool keep = false;
            if (pose_ok[shield]) {
                const float out_of[3] = {shield ? 1.0f : -1.0f, 0, 0};
                float f[3];
                rotate(poses[shield].q, out_of, f);
                const float ahead = f[0] * fx + f[2] * fz;   // the cosine of its face's angle from straight ahead
                // across from its own side's eye (about 3 cm out from the head's middle), either way
                const float off_eye = std::fabs((shield ? side[shield] : -side[shield]) - 0.032f);
                const float dx = poses[shield].p[0] - head[0], dz = poses[shield].p[2] - head[2];
                const bool near = dx * dx + dz * dz < shield_reach * shield_reach;
                keep = shielding ? ahead > std::cos(shield_off) && off_eye < 0.19f && near
                                 : ahead > std::cos(shield_on) && off_eye < 0.12f && near && !at_face;
            }
            shielding = keep && !x.ocarina.load();
            if (shielding) set_bit(ZL, true);
        }
        const float swing_on = 0.01f * (float)cfg_int("NX2NAVI_VR_SWING_SPEED");   // metres a second
        const float out_on = 0.01f * (float)cfg_int("NX2NAVI_VR_CHARGE_REACH"), out_off = out_on - 0.1f;   // metres
        bool swung = false;
        float out = 0.0f;   // the hand's distance from the head's upright line
        const int sword = strcmp(cfg_value("NX2NAVI_VR_HANDEDNESS"), "right") == 0 ? 1 : 0;   // the swinging hand
        if (sword != swing_hand) swing_hand = sword, swing_n = 0, swinging = false;
        if (pose_ok[sword]) {   // the velocity over the last 4 frames
            const float hx = poses[sword].p[0] - head[0], hz = poses[sword].p[2] - head[2];
            out = std::sqrt(hx * hx + hz * hz);
            const int i = swing_n++ & 3;
            std::memcpy(swing_p[i], poses[sword].p, sizeof swing_p[i]);
            swing_t[i] = now;
            if (swing_n >= 4) {
                const int o = swing_n & 3;   // the oldest
                const float dt = (float)(swing_t[i] - swing_t[o]) * 1e-9f;
                float v[3];
                for (int c = 0; c < 3; c++) v[c] = dt > 0 ? (swing_p[i][c] - swing_p[o][c]) / dt : 0.0f;
                const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                // none starts with the hand out to the side of the head or behind it, or moving mostly back toward the
                // body: pulling the hands back (to draw, to rest) is not a swing
                // nor while both arms shake upward or the hand shakes above the head (the D-pad's gestures)
                const bool back = back_any[sword] > -0.03f || v[0] * fx + v[2] * fz < -0.5f * speed || both_up || wave[sword];
                if (!swinging && speed > swing_on && now - swing_at >= refractory_ns && !back) {
                    swinging = swung = true;
                    swing_at = now;
                    // outward: most of the motion away from the head's upright line
                    outward = out > 0.05f && (v[0] * hx + v[2] * hz) / out > 0.6f * speed;
                } else if (swinging && speed < swing_off) {
                    swinging = false;
                }
            }
        } else swing_n = 0, swinging = false;
        if (swung) queued = true;
        if (charging) {
            queued = false;   // (the hand brought back in fast is the release, not a swing)
            if (out < out_off || !pose_ok[sword]) charging = false, y_down = 0, y_up = now;
        } else if (y_down) {
            if (now - y_down >= tap_ns) {
                if (outward && out > out_on) charging = true;
                else y_down = 0, y_up = now;
            }
        }
        if (!y_down && queued && now - y_up >= gap_ns) {
            // what the swing does in play: the sword out (no item) its swing, Y; the hammer, the Deku Stick or an empty
            // bottle out their use, ZR (the hammer's or stick's swing, the bottle's scoop); else nothing
            const bool swinger = strstr(held_item, "Hammer") || strstr(held_item, "DekuStick") || !strcmp(held_item, "Item_Bottle");
            tap_bit = !in_play ? Y : swinger ? ZR : !held_item[0] && x.weapon_out.load() ? Y : -1;
            queued = false;
            if (tap_bit >= 0) y_down = now, outward = outward && swinging && tap_bit == Y;   // (the charge: the sword's)
        }
        if (tap_bit >= 0) set_bit(tap_bit, y_down != 0);
        if (now < draw_until) set_bit(draw_bit, true);
        if (ocarina) {   // the notes: the right stick's left, up, right and down Y, X, A and R; the A button L
            const bool a_down = bits & (1ull << A);
            bits &= ~(1ull << A | 1ull << X | 1ull << Y | 1ull << L | 1ull << R);
            y_down = 0, charging = false, queued = false;   // no sword meanwhile
            set_bit(L, a_down);
            const float sx = sticks[1][0], sy = sticks[1][1];
            if (sx * sx + sy * sy > 0.25f)
                set_bit(std::fabs(sx) > std::fabs(sy) ? (sx < 0 ? Y : A) : (sy > 0 ? X : R), true);
        }
        std::lock_guard<std::mutex> lk(x.hands_mu);
        x.hand_buttons = bits;
        x.wheel_open = wheel, x.wheel_slot = wheel_slot;
        if (used_kind) x.used_kind = used_kind, x.used_slot = used_slot;
        std::memcpy(x.hand_sticks, sticks, sizeof sticks);
        for (int h = 0; h < 2; h++) {
            x.grip[h] = poses[h], x.grip_ok[h] = pose_ok[h];
            XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
            const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            x.aim_ok[h] = aim_space[h] && XR_SUCCEEDED(x.xrLocateSpace(aim_space[h], space, time, &loc)) &&
                          (loc.locationFlags & need) == need;
            if (x.aim_ok[h]) std::memcpy(x.aim_pose[h].q, &loc.pose.orientation, sizeof x.aim_pose[h].q);
        }
        x.hands_at = now;
    }
    void log_profile(XrSession session) {
        if (!ok) return;
        Xr &x = xr();
        for (int h = 0; h < 2; h++) {
            XrInteractionProfileState ps{XR_TYPE_INTERACTION_PROFILE_STATE};
            char name[XR_MAX_PATH_LENGTH] = "none";
            uint32_t n = 0;
            if (XR_SUCCEEDED(x.xrGetCurrentInteractionProfile(session, hand[h], &ps)) && ps.interactionProfile != XR_NULL_PATH)
                x.xrPathToString(x.instance, ps.interactionProfile, sizeof name, &n, name);
            rt_log("vr: the %s hand's controller: %s", h ? "right" : "left", name);
        }
    }
};

// The session's thread: its events, the headset's frames, and each newer box into the swapchain
void run(void *) {
    Xr &x = xr();
    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance = x.vk_instance;
    binding.physicalDevice = x.phys;
    binding.device = x.device;
    binding.queueFamilyIndex = x.family;
    binding.queueIndex = x.queue_index;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = x.system;
    XrSession session = XR_NULL_HANDLE;
    XrResult r = x.xrCreateSession(x.instance, &sci, &session);
    if (XR_FAILED(r)) {
        rt_log("vr: xrCreateSession failed (%d): VR off", (int)r);
        return;
    }
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace space = XR_NULL_HANDLE, view_space = XR_NULL_HANDLE;
    x.xrCreateReferenceSpace(session, &rs, &space);
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    x.xrCreateReferenceSpace(session, &rs, &view_space);

    uint32_t n = 0;
    x.xrEnumerateSwapchainFormats(session, 0, &n, nullptr);
    std::vector<int64_t> formats(n);
    x.xrEnumerateSwapchainFormats(session, n, &n, formats.data());
    int64_t format = 0;
    for (int64_t want : {(int64_t)VK_FORMAT_R8G8B8A8_SRGB, (int64_t)VK_FORMAT_B8G8R8A8_SRGB})
        for (int64_t f : formats)
            if (!format && f == want) format = f;
    if (!format) {
        rt_log("vr: the runtime offers no 8-bit sRGB swapchain format: VR off");
        return;
    }
    x.rgba.store(format == VK_FORMAT_R8G8B8A8_SRGB);

    XrSwapchain swapchain = XR_NULL_HANDLE, hud_swapchain = XR_NULL_HANDLE;   // (the HUD's panel's own)
    std::vector<XrSwapchainImageVulkanKHR> images, hud_images;
    bool hud_has = false;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    // the swapchain is the window's size, known at the first capture
    const auto make_swapchain = [&]() -> bool {
        uint32_t w = x.want_w.load(), h = x.want_h.load();
        if (!w || !h) return false;
        const uint32_t most = w > h ? w : h;
        if (most > 3840) w = w * 3840 / most, h = h * 3840 / most;
        x.width = w & ~1u, x.height = h & ~1u;
        XrSwapchainCreateInfo sw{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sw.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sw.format = format;
        sw.sampleCount = 1;
        sw.width = 2 * x.width, sw.height = x.height;   // the left eye's picture on the left, the right eye's beside it
        sw.faceCount = sw.arraySize = sw.mipCount = 1;
        r = x.xrCreateSwapchain(session, &sw, &swapchain);
        if (XR_FAILED(r)) {
            rt_log("vr: xrCreateSwapchain failed (%d): VR off", (int)r);
            return false;
        }
        x.xrEnumerateSwapchainImages(swapchain, 0, &n, nullptr);
        images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        x.xrEnumerateSwapchainImages(swapchain, n, &n, (XrSwapchainImageBaseHeader *)images.data());
        sw.width = x.width;
        if (XR_SUCCEEDED(x.xrCreateSwapchain(session, &sw, &hud_swapchain))) {
            uint32_t hn = 0;
            x.xrEnumerateSwapchainImages(hud_swapchain, 0, &hn, nullptr);
            hud_images.assign(hn, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
            x.xrEnumerateSwapchainImages(hud_swapchain, hn, &hn, (XrSwapchainImageBaseHeader *)hud_images.data());
        } else {
            hud_swapchain = XR_NULL_HANDLE;
            rt_log("vr: no swapchain for the HUD's panel: the HUD stays in the picture");
        }
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = x.family;
        vkCreateCommandPool(x.device, &pi, nullptr, &pool);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool, ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(x.device, &ai, &cb);
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(x.device, &fi, nullptr, &fence);
        x.ready.store(true, std::memory_order_release);
        rt_log("vr: swapchain made: %ux%u, %u images, format %lld, queue %u of family %u; the screen %.2f m away",
               x.width, x.height, n, (long long)format, x.queue_index, x.family, x.screen_distance);
        return true;
    };
    rt_log("vr: session made");
    Controls controls;
    controls.make(session);

    bool running = false, have_image = false;
    uint64_t shown = 0, frames = 0, copies = 0, tracked_frames = 0;
    Drawn shown_drawn;   // what the picture in the swapchain was drawn from
    uint64_t last_log = plat_now_ns();
    for (;;) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        bool quit = false;
        while (x.xrPollEvent(x.instance, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto *s = (const XrEventDataSessionStateChanged *)&ev;
                rt_log("vr: session state %d", (int)s->state);
                if (s->state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    r = x.xrBeginSession(session, &bi);
                    running = XR_SUCCEEDED(r);
                    if (!running) rt_log("vr: xrBeginSession failed (%d)", (int)r);
                } else if (s->state == XR_SESSION_STATE_STOPPING) {
                    x.xrEndSession(session);
                    running = false;
                } else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) {
                    quit = true;
                }
            } else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
                controls.log_profile(session);
            } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                quit = true;
            }
            ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (quit) {
            rt_log("vr: the session ended: VR off until the game starts again");
            x.ready.store(false), x.running.store(false);
            std::lock_guard<std::mutex> lk(x.head_mu);
            x.have_head = false;
            return;
        }
        x.running.store(running);
        if (!running) {
            plat_sleep_ms(50);
            continue;
        }
        if (!swapchain && !make_swapchain() && x.want_w.load()) return;   // known and failed: said above
        XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        if (XR_FAILED(x.xrWaitFrame(session, &wi, &fs))) continue;
        XrFrameBeginInfo bfi{XR_TYPE_FRAME_BEGIN_INFO};
        x.xrBeginFrame(session, &bfi);
        frames++;

        {   // the head and the eyes' lenses at this frame's predicted time: what the game draws its next frame with
            XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
            li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            li.displayTime = fs.predictedDisplayTime;
            li.space = space;
            XrViewState vs{XR_TYPE_VIEW_STATE};
            XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
            uint32_t count = 0;
            XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
            const bool views_ok = XR_SUCCEEDED(x.xrLocateViews(session, &li, &vs, 2, &count, views)) && count == 2;
            const bool head_ok = XR_SUCCEEDED(x.xrLocateSpace(view_space, space, fs.predictedDisplayTime, &head)) &&
                                 (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
            if (views_ok && head_ok) {
                Lens eyes;
                eyes.l = std::fmin(std::tan(views[0].fov.angleLeft), std::tan(views[1].fov.angleLeft));
                eyes.r = std::fmax(std::tan(views[0].fov.angleRight), std::tan(views[1].fov.angleRight));
                eyes.u = std::fmax(std::tan(views[0].fov.angleUp), std::tan(views[1].fov.angleUp));
                eyes.d = std::fmin(std::tan(views[0].fov.angleDown), std::tan(views[1].fov.angleDown));
                const float dx = views[1].pose.position.x - views[0].pose.position.x,
                            dy = views[1].pose.position.y - views[0].pose.position.y,
                            dz = views[1].pose.position.z - views[0].pose.position.z;
                const float ipd = std::sqrt(dx * dx + dy * dy + dz * dz);
                std::lock_guard<std::mutex> lk(x.head_mu);
                x.eyes = eyes;
                if (ipd > 0.04f && ipd < 0.09f) x.ipd = ipd;
                std::memcpy(x.head.q, &head.pose.orientation, sizeof x.head.q);
                std::memcpy(x.head.p, &head.pose.position, sizeof x.head.p);
                x.have_head = true;
            }
        }
        {
            Pose head_now;
            {
                std::lock_guard<std::mutex> lk(x.head_mu);
                head_now = x.head;
            }
            controls.read(session, space, fs.predictedDisplayTime, head_now);
        }

        int slot = -1;
        uint64_t value = 0;
        Drawn drawn;
        if (swapchain) {
            std::lock_guard<std::mutex> lk(x.mu);
            if (x.latest >= 0 && x.box_value[x.latest] > shown)
                slot = x.latest, value = x.box_value[slot], drawn = x.box_drawn[slot], x.reading = slot;
        }
        // a box into the next image of a swapchain on VR's queue, after the present that wrote it (its timeline value)
        const auto copy_into = [&](XrSwapchain target, std::vector<XrSwapchainImageVulkanKHR> &imgs, VkImage from) -> bool {
            uint32_t index = 0;
            XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = XR_INFINITE_DURATION;
            if (XR_FAILED(x.xrAcquireSwapchainImage(target, &acq, &index)) || XR_FAILED(x.xrWaitSwapchainImage(target, &wait)))
                return false;
            const VkImage dst = imgs[index].image;
            vkResetCommandPool(x.device, pool, 0);
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cb, &bi);
            barrier(cb, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy c{};
            c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.extent = {x.width, x.height, 1};
            vkCmdCopyImage(cb, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            vkEndCommandBuffer(cb);
            VkSemaphoreSubmitInfo w{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
            w.semaphore = x.timeline, w.value = value, w.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
            cbi.commandBuffer = cb;
            VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            si.waitSemaphoreInfoCount = 1, si.pWaitSemaphoreInfos = &w;
            si.commandBufferInfoCount = 1, si.pCommandBufferInfos = &cbi;
            vkQueueSubmit2(x.queue, 1, &si, fence);
            XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            x.xrReleaseSwapchainImage(target, &rel);
            vkWaitForFences(x.device, 1, &fence, VK_TRUE, UINT64_MAX);
            vkResetFences(x.device, 1, &fence);
            return true;
        };
        if (slot >= 0 && (drawn.hud || drawn.menu) && hud_swapchain)
            hud_has = copy_into(hud_swapchain, hud_images, x.hud_boxes[slot]);
        if (slot >= 0) {
            uint32_t index = 0;
            XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = XR_INFINITE_DURATION;
            if (XR_SUCCEEDED(x.xrAcquireSwapchainImage(swapchain, &acq, &index)) &&
                XR_SUCCEEDED(x.xrWaitSwapchainImage(swapchain, &wait))) {
                const VkImage dst = images[index].image;
                vkResetCommandPool(x.device, pool, 0);
                VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(cb, &bi);
                barrier(cb, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkImageCopy c{};
                c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                c.extent = {x.width, x.height, 1};
                vkCmdCopyImage(cb, x.boxes[slot], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
                if (drawn.stereo) {
                    c.dstOffset.x = (int32_t)x.width;
                    vkCmdCopyImage(cb, x.boxes_r[slot], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
                }
                barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                vkEndCommandBuffer(cb);
                VkSemaphoreSubmitInfo w{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
                w.semaphore = x.timeline, w.value = value, w.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
                cbi.commandBuffer = cb;
                VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
                si.waitSemaphoreInfoCount = 1, si.pWaitSemaphoreInfos = &w;
                si.commandBufferInfoCount = 1, si.pCommandBufferInfos = &cbi;
                vkQueueSubmit2(x.queue, 1, &si, fence);
                XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                x.xrReleaseSwapchainImage(swapchain, &rel);
                vkWaitForFences(x.device, 1, &fence, VK_TRUE, UINT64_MAX);
                vkResetFences(x.device, 1, &fence);
                have_image = true;
                shown = value;
                shown_drawn = drawn;
                copies++;
            }
            std::lock_guard<std::mutex> lk(x.mu);
            x.reading = -1;
        }

        // the screen: NX2NAVI_VR_SCREEN_SIZE degrees of the view wide, screen_distance away, an arc of a cylinder around
        // the head or a flat quad
        const float wide = 3.14159265f / 180.0f * (float)cfg_int("NX2NAVI_VR_SCREEN_SIZE");
        const float ratio = x.height ? (float)x.width / (float)x.height : 16.0f / 9.0f;
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        quad.space = space;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = swapchain;
        quad.subImage.imageRect = {{0, 0}, {(int32_t)x.width, (int32_t)x.height}};
        quad.pose.orientation.w = 1.0f;
        quad.pose.position.z = -x.screen_distance;
        const float flat_w = 2.0f * x.screen_distance * std::tan(0.5f * std::fmin(wide, 2.6f));
        quad.size = {flat_w, flat_w / ratio};
        XrCompositionLayerCylinderKHR curve{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
        curve.space = space;
        curve.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        curve.subImage = quad.subImage;
        curve.pose.orientation.w = 1.0f;   // the head's start in the middle, the arc ahead (-z)
        curve.radius = x.screen_distance;
        curve.centralAngle = wide;
        curve.aspectRatio = ratio;   // the arc's length over its height
        const bool curved = x.cylinder && cfg_bool("NX2NAVI_VR_SCREEN_CURVE");
        // a picture drawn from the head's pose: a projection layer at that pose, the same picture to both eyes, or with
        // depth stereo each eye's own picture at its own place, half the eyes' distance to its side; the compositor turns
        // it with the head between the game's frames
        XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                  {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        for (int e = 0; e < 2; e++) {
            XrCompositionLayerProjectionView &v = pv[e];
            const Pose &own = shown_drawn.stereo && shown_drawn.eye >= 0 ? shown_drawn.eye_pose[e] : shown_drawn.pose;
            std::memcpy(&v.pose.orientation, own.q, sizeof own.q);
            float at[3] = {own.p[0], own.p[1], own.p[2]};
            if (shown_drawn.stereo && shown_drawn.eye < 0) {   // (Balanced's eyes are at their own poses already)
                const float side[3] = {(e ? 0.5f : -0.5f) * shown_drawn.ipd, 0, 0};
                float off[3];
                rotate(shown_drawn.pose.q, side, off);
                for (int c = 0; c < 3; c++) at[c] += off[c];
            }
            std::memcpy(&v.pose.position, at, sizeof at);
            v.fov.angleLeft = std::atan(shown_drawn.lens.l), v.fov.angleRight = std::atan(shown_drawn.lens.r);
            v.fov.angleUp = std::atan(shown_drawn.lens.u), v.fov.angleDown = std::atan(shown_drawn.lens.d);
            v.subImage.swapchain = swapchain;
            v.subImage.imageRect = {{shown_drawn.stereo && e ? (int32_t)x.width : 0, 0}, {(int32_t)x.width, (int32_t)x.height}};
        }
        XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        proj.space = space;
        proj.viewCount = 2;
        proj.views = pv;
        // the HUD's panel: held in front of the head (NX2NAVI_VR_HUD_SIZE centimetres wide, a metre away), over the view
        XrCompositionLayerQuad hud{XR_TYPE_COMPOSITION_LAYER_QUAD};
        hud.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        hud.space = view_space;
        hud.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        hud.subImage.swapchain = hud_swapchain;
        hud.subImage.imageRect = {{0, 0}, {(int32_t)x.width, (int32_t)x.height}};
        hud.pose.orientation.w = 1.0f;
        hud.pose.position.z = -1.0f;
        {
            const float wide = 0.01f * (float)cfg_int("NX2NAVI_VR_HUD_SIZE");
            hud.size = {wide, x.width ? wide * (float)x.height / (float)x.width : 0};
        }
        // the paused game's menus go on the HUD's panel, over the world as it was
        const bool show_menu = shown_drawn.tracked && shown_drawn.menu && hud_has && hud_swapchain;
        const bool show_hud = shown_drawn.tracked && shown_drawn.hud && hud_has && hud_swapchain;
        const XrCompositionLayerBaseHeader *screen = curved ? (const XrCompositionLayerBaseHeader *)&curve
                                                            : (const XrCompositionLayerBaseHeader *)&quad;
        const XrCompositionLayerBaseHeader *layers[] = {shown_drawn.tracked ? (const XrCompositionLayerBaseHeader *)&proj
                                                                            : screen,
                                                        (const XrCompositionLayerBaseHeader *)&hud};
        if (shown_drawn.tracked) tracked_frames++;
        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
        ei.displayTime = fs.predictedDisplayTime;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        ei.layerCount = have_image && fs.shouldRender ? (show_hud || show_menu ? 2 : 1) : 0;
        ei.layers = layers;
        r = x.xrEndFrame(session, &ei);
        if (XR_FAILED(r)) rt_log("vr: xrEndFrame failed (%d)", (int)r);
        const uint64_t now = plat_now_ns();
        if (now - last_log > 10000000000ull) {
            rt_log("vr: %llu headset frames (%llu with the head's view), %llu pictures copied in the last 10 s (%u drawn "
                   "from the head, %u HUD panels made, %u in depth stereo, %u without the scene's depth, %u paused); the "
                   "game's camera %u, lens %u", (unsigned long long)frames, (unsigned long long)tracked_frames,
                   (unsigned long long)copies, x.tracked_pictures.exchange(0), x.panels.exchange(0), x.stereo_pictures.exchange(0),
                   x.stereo_missed.exchange(0), x.menu_pictures.exchange(0), x.camera_given.exchange(0), x.lens_given.exchange(0));
            {
                std::lock_guard<std::mutex> lk(x.hands_mu);
                rt_log("vr: hands: buttons %#llx, sticks %.2f %.2f / %.2f %.2f, grips %s %s (left at %.2f %.2f %.2f)",
                       (unsigned long long)x.hand_buttons, x.hand_sticks[0][0], x.hand_sticks[0][1], x.hand_sticks[1][0],
                       x.hand_sticks[1][1], x.grip_ok[0] ? "tracked" : "lost", x.grip_ok[1] ? "tracked" : "lost",
                       x.grip[0].p[0], x.grip[0].p[1], x.grip[0].p[2]);
            }
            frames = copies = tracked_frames = 0, last_log = now;
        }
    }
}

} // namespace

extern "C" void vk_xr_instance_exts(const char **ext, uint32_t *n, uint32_t cap) {
    if (!init()) return;
    for (const char *name : xr().instance_list) {
        if (listed(ext, *n, name)) continue;
        if (*n >= cap) {
            rt_log("vr: no room for the instance extension %s: VR off", name);
            xr().ok = false;
            return;
        }
        ext[(*n)++] = name;
    }
}

extern "C" VkPhysicalDevice vk_xr_device(VkInstance instance) {
    Xr &x = xr();
    if (!x.ok) return VK_NULL_HANDLE;
    VkPhysicalDevice p = VK_NULL_HANDLE;
    const XrResult r = x.xrGetVulkanGraphicsDeviceKHR(x.instance, x.system, instance, &p);
    if (XR_FAILED(r) || !p) {
        rt_log("vr: xrGetVulkanGraphicsDeviceKHR failed (%d): VR off", (int)r);
        x.ok = false;
        return VK_NULL_HANDLE;
    }
    return x.phys = p;
}

extern "C" void vk_xr_device_picked(VkPhysicalDevice picked) {
    Xr &x = xr();
    if (!x.ok || picked == x.phys) return;
    VkPhysicalDeviceProperties a{}, b{};
    vkGetPhysicalDeviceProperties(picked, &a);
    if (x.phys) vkGetPhysicalDeviceProperties(x.phys, &b);
    rt_log("vr: the headset's runtime draws with %s, the game with %s (not usable there, or chosen by NX2NAVI_GPU_*): "
           "VR off", x.phys ? b.deviceName : "no GPU", a.deviceName);
    x.ok = false;
}

extern "C" void vk_xr_device_exts(const VkExtensionProperties *have, uint32_t n_have, const char **ext, uint32_t *n,
                                  uint32_t cap) {
    Xr &x = xr();
    if (!x.ok) return;
    for (const char *name : x.device_list) {
        bool there = false;
        for (uint32_t i = 0; i < n_have && !there; i++) there = !std::strcmp(have[i].extensionName, name);
        if (!there) rt_log("vr: the device lacks %s, which the runtime asks for", name);
        else if (!listed(ext, *n, name)) {
            if (*n >= cap) {
                rt_log("vr: no room for the device extension %s: VR off", name);
                x.ok = false;
                return;
            }
            ext[(*n)++] = name;
        }
    }
}

extern "C" uint32_t vk_xr_queue(uint32_t family_queues, uint32_t first_free) {
    Xr &x = xr();
    if (!x.ok) return UINT32_MAX;
    if (first_free >= family_queues || first_free >= 4) {
        rt_log("vr: the graphics family has %u queues, none spare for VR: VR off", family_queues);
        x.ok = false;
        return UINT32_MAX;
    }
    return x.queue_index = first_free;
}

extern "C" void vk_xr_device_ready(VkInstance instance, VkPhysicalDevice phys, VkDevice device, uint32_t family) {
    Xr &x = xr();
    if (!x.ok || x.queue_index == UINT32_MAX) return;
    x.vk_instance = instance, x.phys = phys, x.device = device, x.family = family;
    vkGetDeviceQueue(device, family, x.queue_index, &x.queue);
    VkSemaphoreTypeCreateInfo tc{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    tc.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sc.pNext = &tc;
    if (vkCreateSemaphore(device, &sc, nullptr, &x.timeline) != VK_SUCCESS) {
        rt_log("vr: no timeline semaphore: VR off");
        x.ok = false;
        return;
    }
    if (!plat_thread_spawn(run, nullptr, "vr session", 0)) {
        rt_log("vr: the session's thread could not start: VR off");
        x.ok = false;
    }
}

extern "C" int vk_xr_head_wanted(void) { return xr().ok && strcmp(cfg_value("NX2NAVI_VR_VIEW"), "head") == 0; }

extern "C" int vk_xr_depth_wanted(void) {   // (Balanced keeps the depth too: the hands and the reticle are hidden by it)
    return vk_xr_head_wanted() && (cfg_int("NX2NAVI_VR_DEPTH") > 0 || stereo_turns());
}

extern "C" void vk_xr_keep_depth(VkCommandBuffer cb, VkImageView depth, uint32_t w, uint32_t h) {
    Xr &x = xr();
    if (!x.ok || !x.head_view.load(std::memory_order_relaxed) || !depth || !w || !h) return;
    std::lock_guard<std::mutex> lk(x.hud_mu);
    if (!x.keep_tried) {
        x.keep_tried = true;
        if (!make_keep_pipeline()) {
            rt_log("vr: the depth stereo's depth copy could not be made: one picture to both eyes");
            x.keep_pipe = VK_NULL_HANDLE;
        }
    }
    if (!x.keep_pipe) return;
    // the replay's commands go through the recorder thread (vk_rec.h), which this file's direct calls bypass: they are
    // made once it has made every command queued before them
    rec_drain();
    bool fresh = false;
    if (!x.depth || x.depth_w != w || x.depth_h != h) {
        x.depth = VK_NULL_HANDLE, x.depth_kept = 0;   // (another size's is left behind, as the scene's)
        x.rows = VK_NULL_HANDLE;
        if (!make_image(w, h, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, x.depth, x.depth_view) ||
            !make_buffer(8ull * h, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, x.rows)) {
            rt_log("vr: the depth stereo's depth image %ux%u could not be made: one picture to both eyes", w, h);
            x.keep_pipe = VK_NULL_HANDLE;
            return;
        }
        x.depth_w = w, x.depth_h = h, fresh = true;
        rt_log("vr: depth stereo keeps the scene's depth: %ux%u", w, h);
    }
    barrier(cb, x.depth, fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdFillBuffer(cb, x.rows, 0, 4ull * h, 0x7f800000u);   // each row's least: +inf
    vkCmdFillBuffer(cb, x.rows, 4ull * h, 4ull * h, 0);      // its greatest: 0
    memory_barrier(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, x.keep_pipe);
    const VkDescriptorImageInfo info[2] = {{x.keep_sampler, depth, VK_IMAGE_LAYOUT_GENERAL},
                                           {VK_NULL_HANDLE, x.depth_view, VK_IMAGE_LAYOUT_GENERAL}};
    const VkDescriptorBufferInfo rows{x.rows, 0, 8ull * h};
    VkWriteDescriptorSet wr[3]{};
    for (uint32_t i = 0; i < 3; i++) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, wr[i].dstBinding = i, wr[i].descriptorCount = 1;
        wr[i].descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                               : i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        if (i == 2) wr[i].pBufferInfo = &rows;
        else wr[i].pImageInfo = &info[i];
    }
    x.keep_push(cb, VK_PIPELINE_BIND_POINT_COMPUTE, x.keep_layout, 0, 3, wr);
    const int32_t pc[2] = {(int32_t)w, (int32_t)h};
    vkCmdPushConstants(cb, x.keep_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    memory_barrier(cb);
    x.depth_kept++;
}

extern "C" int vk_xr_head_active(void) {
    Xr &x = xr();
    return x.ok && x.head_view.load(std::memory_order_relaxed) && x.running.load(std::memory_order_relaxed);
}

extern "C" int vk_xr_pad(uint64_t *buttons, int32_t left[2], int32_t right[2]) {
    Xr &x = xr();
    if (!x.ok || !x.running.load() || strcmp(cfg_value("NX2NAVI_VR_CONTROLS"), "meta") != 0) return 0;
    uint64_t bits;
    float sticks[2][2];
    int wheel;
    {
        std::lock_guard<std::mutex> lk(x.hands_mu);
        if (!x.hands_at || plat_now_ns() - x.hands_at > 250000000ull) return 0;   // not read for a while: not focused
        bits = x.hand_buttons;
        std::memcpy(sticks, x.hand_sticks, sizeof sticks);
        wheel = x.wheel_open;
    }
    if (x.ocarina.load()) sticks[1][0] = sticks[1][1] = 0, right[0] = right[1] = 0;   // the right stick plays the notes
    if (wheel) {   // the wheel's hand's stick picks on it, not in the game
        int32_t *s = wheel == 1 ? left : right;
        sticks[wheel - 1][0] = sticks[wheel - 1][1] = 0, s[0] = s[1] = 0;
    }
    int used = bits != 0;
    int32_t *out[2] = {left, right};
    for (int h = 0; h < 2; h++) {   // a small dead zone, then the stick farther out of the two (the gamepad's or this)
        const float m = std::sqrt(sticks[h][0] * sticks[h][0] + sticks[h][1] * sticks[h][1]);
        if (m < 0.12f) continue;
        const float k = std::fmin(1.0f, (m - 0.12f) / 0.88f) / m;
        const int32_t sx = (int32_t)(sticks[h][0] * k * 32767.0f), sy = (int32_t)(sticks[h][1] * k * 32767.0f);
        if ((int64_t)sx * sx + (int64_t)sy * sy > (int64_t)out[h][0] * out[h][0] + (int64_t)out[h][1] * out[h][1])
            out[h][0] = sx, out[h][1] = sy;
        used = 1;
    }
    *buttons |= bits;
    return used;
}

extern "C" int vk_xr_wheel(int *slot) {
    Xr &x = xr();
    if (!x.ok || !x.running.load() || strcmp(cfg_value("NX2NAVI_VR_CONTROLS"), "meta") != 0) return 0;
    std::lock_guard<std::mutex> lk(x.hands_mu);
    if (!x.wheel_open || !x.hands_at || plat_now_ns() - x.hands_at > 250000000ull) return 0;
    *slot = x.wheel_slot;
    return x.wheel_open;
}

extern "C" int vk_xr_wheel_used(int *kind, int *slot) {
    Xr &x = xr();
    std::lock_guard<std::mutex> lk(x.hands_mu);
    if (!x.used_kind) return 0;
    *kind = x.used_kind, *slot = x.used_slot, x.used_kind = 0;
    return 1;
}

extern "C" void vk_xr_wheel_allow(int allow) { xr().wheel_allowed.store(allow != 0); }

extern "C" void vk_xr_ocarina(int on) { xr().ocarina.store(on != 0); }

extern "C" void vk_xr_weapon_out(int out) { xr().weapon_out.store(out != 0); }
extern "C" void vk_xr_child(int child) { xr().child.store(child != 0); }

extern "C" void vk_xr_in_play(int on) { xr().in_play.store(on != 0); }

extern "C" void vk_xr_scene_camera(const float r[12]) {
    Xr &x = xr();
    double at[3];   // the camera's place: -R^T t
    for (int c = 0; c < 3; c++) at[c] = -(r[c] * r[3] + r[4 + c] * r[7] + r[8 + c] * r[11]);
    if (!std::isfinite(at[0]) || !std::isfinite(at[1]) || !std::isfinite(at[2])) return;
    std::lock_guard<std::mutex> lk(x.frame_mu);
    std::memcpy(x.scene_at, at, sizeof at), x.scene_new = true;
}

extern "C" void vk_xr_aim_freeze(int on) {
    Xr &x = xr();
    std::lock_guard<std::mutex> lk(x.frame_mu);
    x.aim_freeze = on != 0;
}

extern "C" int vk_xr_pointer(double dir[3]) {
    Xr &x = xr();
    Pose g;
    {
        std::lock_guard<std::mutex> lk(x.hands_mu);
        const int sword = strcmp(cfg_value("NX2NAVI_VR_HANDEDNESS"), "right") == 0 ? 1 : 0;
        if (!x.hands_at || plat_now_ns() - x.hands_at > 250000000ull) return 0;
        if (x.aim_ok[sword]) g = x.aim_pose[sword];   // its own pointing ray; else the grip's
        else if (x.grip_ok[sword]) g = x.grip[sword];
        else return 0;
    }
    // where the controller points, in the LOCAL space (x right, y up, z back): its aim pose's -z tilted down by
    // NX2NAVI_VR_AIM_TILT degrees (35 by default, found right for the Quest's Touch controllers)
    const float tilt = (float)cfg_int("NX2NAVI_VR_AIM_TILT") * 3.14159265f / 180.0f;
    const float ahead[3] = {0, -std::sin(tilt), -std::cos(tilt)};
    float v[3];
    rotate(g.q, ahead, v);
    double bx, bz;
    {
        std::lock_guard<std::mutex> lk(x.frame_mu);
        bx = x.cur_bx, bz = x.cur_bz;
    }
    dir[0] = bz * v[0] + bx * v[2], dir[1] = v[1], dir[2] = -bx * v[0] + bz * v[2];
    return 1;
}

extern "C" void vk_xr_hand_item(const char *model) {
    Xr &x = xr();
    std::lock_guard<std::mutex> lk(x.gear_mu);
    std::snprintf(x.gear[2], sizeof x.gear[2], "%s", model);
}

extern "C" void vk_xr_gear(const char *sword, const char *shield) {
    Xr &x = xr();
    std::lock_guard<std::mutex> lk(x.gear_mu);
    std::snprintf(x.gear[0], sizeof x.gear[0], "%s", sword);
    std::snprintf(x.gear[1], sizeof x.gear[1], "%s", shield);
}

extern "C" void vk_xr_aim(int on, const double origin[3], const double dir[3]) {
    Xr &x = xr();
    std::lock_guard<std::mutex> lk(x.frame_mu);
    x.aim_on = on != 0;
    if (on) std::memcpy(x.aim_o, origin, sizeof x.aim_o), std::memcpy(x.aim_d, dir, sizeof x.aim_d);
}


extern "C" int vk_xr_holds_time(void) {
    Xr &x = xr();
    return stereo_accurate() && x.ok && x.head_view.load(std::memory_order_relaxed) &&
           x.running.load(std::memory_order_relaxed);
}

extern "C" void vk_xr_update_step(double seconds) { update_step.store(seconds, std::memory_order_relaxed); }

extern "C" int vk_xr_camera(double pos[3], double at[3], float up[3]) {
    Xr &x = xr();
    if (!x.ok || !x.head_view.load(std::memory_order_relaxed) || !x.running.load(std::memory_order_relaxed)) {
        vk_xr_untracked();
        return 0;
    }
    Pose head;
    Lens eyes;
    float ipd;
    {
        std::lock_guard<std::mutex> lk(x.head_mu);
        if (!x.have_head) return 0;
        head = x.head, eyes = x.eyes;
        ipd = x.ipd;
    }
    // the camera's facing made level (its yaw alone, so the horizon stays level whatever the game's camera does): the
    // head's axes are put on it, x right, y up, z back as OpenXR's
    double f[3] = {at[0] - pos[0], at[1] - pos[1], at[2] - pos[2]};
    const double reach = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    const double flat = std::sqrt(f[0] * f[0] + f[2] * f[2]);
    if (!(reach > 1e-6) || !(flat > 1e-6)) return 0;
    double bx = -f[0] / flat, bz = -f[2] / flat;
    {
        std::lock_guard<std::mutex> lk(x.frame_mu);
        if (x.aim_freeze) {
            if (!x.frozen_ok) x.frozen_bx = bx, x.frozen_bz = bz, x.frozen_ok = true;
            bx = x.frozen_bx, bz = x.frozen_bz;
        } else x.frozen_ok = false;
        x.cur_bx = bx, x.cur_bz = bz;
    }
    const double axes[3][3] = {{bz, 0, -bx}, {0, 1, 0}, {bx, 0, bz}};   // right, up, back in the world
    const auto world = [&](const float v[3], double out[3]) {
        for (int c = 0; c < 3; c++) out[c] = axes[0][c] * v[0] + axes[1][c] * v[1] + axes[2][c] * v[2];
    };
    static const float ahead[3] = {0, 0, -1}, upward[3] = {0, 1, 0}, rightward[3] = {1, 0, 0};
    float hf[3], hu[3], hr[3];
    rotate(head.q, ahead, hf), rotate(head.q, upward, hu), rotate(head.q, rightward, hr);
    // Stereo Balanced: this picture from one eye, the other the next (eyes in turn): the head moved half the eyes'
    // distance (NX2NAVI_VR_DEPTH percent of it, as depth stereo's) to that eye's side, the game's camera with it
    int eye = -1;
    if (stereo_turns()) {
        {
            std::lock_guard<std::mutex> lk(x.frame_mu);
            const double step = update_step.load(std::memory_order_relaxed);
            if (stereo_accurate() && step > 0) x.turn = step < 0.006 ? 1 : 0;   // the held update draws the right eye
            eye = x.turn, x.turn ^= 1;
        }
        const float strength = 0.01f * (float)cfg_int("NX2NAVI_VR_DEPTH");
        for (int c = 0; c < 3; c++) head.p[c] += (eye ? 0.5f : -0.5f) * ipd * strength * hr[c];
    }
    double wf[3], wu[3], wp[3], wr[3];
    world(hf, wf), world(hu, wu), world(head.p, wp), world(hr, wr);
    double base[3] = {pos[0], pos[1], pos[2]};
    for (int c = 0; c < 3; c++) {
        pos[c] += wp[c];
        at[c] = pos[c] + wf[c] * reach;
        up[c] = (float)wu[c];
    }
    std::lock_guard<std::mutex> lk(x.frame_mu);
    x.frame_on = true;
    x.drawn.tracked = true, x.drawn.pose = head, x.drawn.lens = frustum(eyes, x.picture_ratio.load());
    x.drawn.eye = eye;
    std::memcpy(x.drawn.base, base, sizeof base), x.drawn.bx = bx, x.drawn.bz = bz;
    x.drawn.aim = x.aim_on;
    if (x.aim_on) {   // the aim's ray in the view: right, up and back (-forward)
        double o[3];
        for (int c = 0; c < 3; c++) o[c] = x.aim_o[c] - pos[c];
        const double *axis[3] = {wr, wu, wf};
        for (int k = 0; k < 3; k++) {
            const double sign = k == 2 ? -1.0 : 1.0;
            x.drawn.aim_o[k] = (float)(sign * (o[0] * axis[k][0] + o[1] * axis[k][1] + o[2] * axis[k][2]));
            x.drawn.aim_d[k] = (float)(sign * (x.aim_d[0] * axis[k][0] + x.aim_d[1] * axis[k][1] + x.aim_d[2] * axis[k][2]));
        }
    }
    // the game draws a lens this many times the one the headset is told (NX2NAVI_VR_VIEW_SCALE, 100 % by default: the
    // headset's own): more of the world in the same view, everything smaller above 100 %
    x.render_scale = std::fmax(0.5f, std::fmin(2.0f, (float)cfg_int("NX2NAVI_VR_VIEW_SCALE") / 100.0f));
    x.drawn.scale = x.render_scale, x.drawn.ipd = ipd;
    x.camera_given++;
    Xr::Given &g = x.given[x.given_next++ % 6];
    g.d = x.drawn, g.used = true;
    for (int c = 0; c < 3; c++) g.at[c] = pos[c];
    x.updates++;
    return 1;
}

extern "C" int vk_xr_lens(float *angle) {
    Xr &x = xr();
    std::lock_guard<std::mutex> lk(x.frame_mu);
    if (!x.frame_on) return 0;
    *angle = 2.0f * std::atan(x.drawn.lens.u * x.render_scale);
    x.lens_given++;
    return 1;
}

extern "C" void vk_xr_untracked(void) {
    Xr &x = xr();
    std::lock_guard<std::mutex> lk(x.frame_mu);
    x.frame_on = false;
    x.drawn.tracked = false;
}

extern "C" VkImage vk_xr_scene_image(VkImage target, uint32_t w, uint32_t h, VkFormat format, int *init) {
    Xr &x = xr();
    *init = 0;
    if (!x.ok || !x.head_view.load(std::memory_order_relaxed) || !(hud_wanted() || vk_xr_depth_wanted()) || !target || !w || !h)
        return VK_NULL_HANDLE;
    // other pictures pass through the upscaler too (a 2560x1440 sRGB one): only the one of the presented picture's size
    // is the scene the HUD is drawn over
    if (w != x.pic_w.load(std::memory_order_relaxed) || h != x.pic_h.load(std::memory_order_relaxed)) return VK_NULL_HANDLE;
    std::lock_guard<std::mutex> lk(x.hud_mu);
    if (x.hud_tried && !x.hud_pipe) return VK_NULL_HANDLE;   // the panel could not be made: the HUD stays in the picture
    if (!x.scene || x.hud_w != w || x.hud_h != h || x.hud_format != format) {
        // (images of another size are left behind: the picture's size changes rarely, at a setting)
        x.scene = x.scene8 = x.panel8 = VK_NULL_HANDLE, x.used = x.kept;
        if (!make_image(w, h, format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, x.scene, x.scene_view)) {
            static int told_fail;
            if (told_fail++ < 4) rt_log("vr: the HUD panel's scene image %ux%u format %d could not be made", w, h, (int)format);
            x.scene = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        rt_log("vr: the HUD panel keeps the scene: %ux%u format %d", w, h, (int)format);
        x.hud_w = w, x.hud_h = h, x.hud_format = format;
        *init = 1;
    }
    x.hud_target = target;
    x.kept++;
    return x.scene;
}

extern "C" int vk_xr_wants_capture(int hdr) {
    Xr &x = xr();
    if (!x.ok || !x.timeline) return 0;
    const bool head = vk_xr_head_wanted();
    static bool hooks_on;   // the camera hooks, switched on the first time the head's view is wanted
    if (head && !hooks_on) hooks_on = true, vr_game_start();
    x.head_view.store(head && !menu_is_open(), std::memory_order_relaxed);
    if (hdr) {
        static bool told;
        if (!told) told = true, rt_log("vr: the headset needs HDR off");
        return 0;
    }
    if (!x.ready.load(std::memory_order_acquire)) return 1;   // the window's size is still to be learned (vk_xr_capture)
    if (x.boxes_failed) return 0;
    std::lock_guard<std::mutex> lk(x.mu);
    for (int i = 0; i < 3; i++)
        if (i != x.reading && i != x.latest) return 1;
    return 0;
}

extern "C" void vk_xr_capture(VkCommandBuffer cb, VkImage window, VkExtent2D extent, VkFormat format, VkImage picture,
                              VkImageLayout picture_layout, const int rect[4], VkSemaphoreSubmitInfo *signal) {
    Xr &x = xr();
    *signal = VkSemaphoreSubmitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    if (!extent.width || !extent.height) return;
    const bool have_picture = picture && rect[2] > 0 && rect[3] > 0;
    if (have_picture) {
        x.picture_ratio.store((float)rect[2] / (float)rect[3], std::memory_order_relaxed);
        x.pic_w.store((uint32_t)rect[2], std::memory_order_relaxed), x.pic_h.store((uint32_t)rect[3], std::memory_order_relaxed);
    }
    // the swapchain at the game's picture's size (the head's view shows it over the eyes' whole view, every pixel
    // counts); the boot screen before it, which has none, is not shown
    if (!x.want_w.load() && have_picture) x.want_h.store((uint32_t)rect[3]), x.want_w.store((uint32_t)rect[2]);
    if (!x.ready.load(std::memory_order_acquire)) return;
    if (!x.boxes_made) {
        x.boxes_made = make_boxes(format);
        x.boxes_failed = !x.boxes_made;
        if (!x.boxes_made) return;
    }
    int slot = -1;
    {
        std::lock_guard<std::mutex> lk(x.mu);
        for (int i = 0; i < 3 && slot < 0; i++) {
            const int c = (x.next_slot + i) % 3;
            if (c != x.reading && c != x.latest) slot = c;
        }
        if (slot < 0) return;
        x.next_slot = (slot + 1) % 3;
    }
    // the head's view: the game's picture (without the window's menu, which the screen shows) with the pose the last
    // gameplay camera update put on it; else the window for the screen
    Drawn d;
    x.stale = x.updates.exchange(0) ? 0 : x.stale + 1;
    const bool head = x.head_view.load(std::memory_order_relaxed) && have_picture;
    if (head && x.stale < 3) {
        std::lock_guard<std::mutex> lk(x.frame_mu);
        d = x.drawn;
        if (x.scene_new) {   // the camera update this picture was drawn from: the one whose place the scene's camera had
            double best = 1e30;
            for (const auto &g : x.given) {
                if (!g.used) continue;
                double e2 = 0;
                for (int c = 0; c < 3; c++) e2 += (g.at[c] - x.scene_at[c]) * (g.at[c] - x.scene_at[c]);
                if (e2 < best) best = e2, d = g.d;
            }
            x.scene_new = false;
        }
    }
    if (!head) x.frozen.tracked = false;
    if (head && x.stale >= 3 && x.frozen.tracked) {   // paused: the frozen world, and the game's picture on the HUD's panel
        d = x.frozen;
        d.hud = false, d.menu = true;
        x.box_drawn[slot] = d;
        x.menu_pictures++;
        VkImageBlit b{};
        b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.dstOffsets[1] = {(int32_t)x.width, (int32_t)x.height, 1};
        {
            std::lock_guard<std::mutex> lk(x.hud_mu);
            b.srcOffsets[1] = {(int32_t)x.hud_w, (int32_t)x.hud_h, 1};
            for (int e = 0; e < (d.stereo ? 2 : 1); e++) {
                const VkImage bx = e ? x.boxes_r[slot] : x.boxes[slot];
                barrier(cb, bx, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                vkCmdBlitImage(cb, d.stereo ? eye_image(d, e) : x.scene8, VK_IMAGE_LAYOUT_GENERAL, bx,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
                barrier(cb, bx, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            }
        }
        const VkImage hb = x.hud_boxes[slot];
        barrier(cb, hb, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        b.srcOffsets[0] = {rect[0], rect[1], 0};
        b.srcOffsets[1] = {rect[0] + rect[2], rect[1] + rect[3], 1};
        vkCmdBlitImage(cb, picture, picture_layout, hb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
        barrier(cb, hb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        overlay_onto_hud(cb, slot, window, extent, d);
        x.pending = slot;
        signal->semaphore = x.timeline;
        signal->value = ++x.next_value;
        signal->stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        return;
    }
    // the HUD apart (NX2NAVI_VR_HUD=panel) and depth stereo (NX2NAVI_VR_DEPTH) start from the scene kept before the HUD:
    // the head's view without the HUD (with it where it stays in the picture), the HUD on its panel, each eye's picture
    const bool panel = hud_wanted(), depth = d.tracked && vk_xr_depth_wanted();
    bool made = false;
    uint32_t hw = 0, hh = 0;
    if (d.tracked && (panel || depth)) {
        std::lock_guard<std::mutex> lk(x.hud_mu);
        // a wheel open or just closed: the game's HUD left out of the picture too (only the wheel is shown, on the panel)
        int wheel_slot;
        const bool wheel = vk_xr_wheel(&wheel_slot) || plat_now_ns() - x.wheel_closed_at.load() < wheel_hide_ns;
        made = make_panel(cb, !panel && !wheel, d.aim && panel ? (int)(0.06f * (float)x.hud_h) : 0);
        hw = x.hud_w, hh = x.hud_h;
        if (made) make_aim(cb, d);
        if (made && depth && d.eye < 0) d.stereo = make_eyes(cb, d);
        x.win_hands = made && make_hands(cb, d);
        if (made && d.eye >= 0) keep_eye(cb, d);   // Balanced: this eye's picture kept, both shown once both are
    }
    d.hud = made && panel;
    if (d.tracked && made) x.frozen = d;   // its world stays in scene8 and eye8 until the next one made
    x.box_drawn[slot] = d;
    if (d.tracked) x.tracked_pictures++;
    const VkImage box = x.boxes[slot];
    barrier(cb, box, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit b{};
    b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.dstOffsets[1] = {(int32_t)x.width, (int32_t)x.height, 1};
    if (made) {
        b.srcOffsets[1] = {(int32_t)hw, (int32_t)hh, 1};
        vkCmdBlitImage(cb, d.stereo ? eye_image(d, 0) : x.scene8, VK_IMAGE_LAYOUT_GENERAL, box,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
        if (d.stereo) {
            const VkImage rb = x.boxes_r[slot];
            barrier(cb, rb, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdBlitImage(cb, eye_image(d, 1), VK_IMAGE_LAYOUT_GENERAL, rb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                           VK_FILTER_LINEAR);
            barrier(cb, rb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        }
        if (d.hud) {
            const VkImage hb = x.hud_boxes[slot];
            barrier(cb, hb, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdBlitImage(cb, x.panel8, VK_IMAGE_LAYOUT_GENERAL, hb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
            barrier(cb, hb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        }
    } else if (d.tracked) {
        b.srcOffsets[0] = {rect[0], rect[1], 0};
        b.srcOffsets[1] = {rect[0] + rect[2], rect[1] + rect[3], 1};
        vkCmdBlitImage(cb, picture, picture_layout, box, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    } else {
        b.srcOffsets[1] = {(int32_t)extent.width, (int32_t)extent.height, 1};
        vkCmdBlitImage(cb, window, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, box, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                       VK_FILTER_LINEAR);
    }
    barrier(cb, box, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (d.tracked) {
        overlay_onto_hud(cb, slot, window, extent, d);
        x.box_drawn[slot] = d;
        if (have_picture) {   // last: the headset's copies are made
            std::lock_guard<std::mutex> lk(x.hud_mu);
            if (x.win_hands && d.eye != 1) hands_onto_window(cb, window, extent, format, rect);
            if (d.eye >= 0) window_left_eye(cb, window, extent, format, rect, d.eye);
        }
    }
    x.pending = slot;
    signal->semaphore = x.timeline;
    signal->value = ++x.next_value;
    signal->stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
}

extern "C" void vk_xr_captured(const VkSemaphoreSubmitInfo *signal) {
    Xr &x = xr();
    if (!signal->semaphore || x.pending < 0) return;
    std::lock_guard<std::mutex> lk(x.mu);
    x.box_value[x.pending] = signal->value;
    x.latest = x.pending;
    x.pending = -1;
}
