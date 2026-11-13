// Low latency without frame generation (vk_lowlat.c): the driver's own mode, NVIDIA's through VK_NV_low_latency2 or
// AMD's through VK_AMD_anti_lag, used directly. NX2NAVI_LOW_LATENCY: 0 off, 1 on, 2 on with boost (NVIDIA), live.
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

struct VkCtx;

// vk_device.c create_device, once the device exists: takes the entry points of the extension the device enabled
// (VkCtx.has_low_latency2, has_anti_lag) and logs which path this run has. vk_lowlat_release: before the device goes.
void vk_lowlat_init(struct VkCtx *c);
void vk_lowlat_release(void);

// What the menu's row shows (ui/menu.cpp row_low_latency). The first two are the paths that can run; the others say
// why none does: the driver has neither extension, DLSS Frame Generation's interposer owns low latency in this run
// (vk_dlssg.c, NX2NAVI_REFLEX), a frame generation presents the frames in the window swapchain's place, or the driver
// refused the mode or never ended its sleep (the log says which), or the driver has a mode but the run started with
// the setting off, so the device was made without it (vk_device.c create_device): the next start with it on has it.
enum { VK_LOWLAT_NV, VK_LOWLAT_AMD, VK_LOWLAT_NO_DRIVER, VK_LOWLAT_DLSSG, VK_LOWLAT_FG, VK_LOWLAT_FAILED,
       VK_LOWLAT_RESTART };
int vk_lowlat_status(void);
// The direct path is in effect now: the setting is on and nothing else owns the presents. Any thread, one atomic load.
int vk_lowlat_on(void);

// ---- the window's swapchain (gfx_present.c, with its present lock held). NVIDIA's mode belongs to a swapchain.
// 1 when the next plain swapchain is to be made with VkSwapchainLatencyCreateInfoNV (NVIDIA's path, the setting on).
int vk_lowlat_swapchain_wanted(void);
// made 1, after a swapchain was created: the plain swapchain made that way, on which the sleep mode is set from here
// on, or VK_NULL_HANDLE for one this path cannot name (a frame generation's, one made without the mode). made 0, with
// VK_NULL_HANDLE: before the window's swapchain is replaced or destroyed.
void vk_lowlat_swapchain(VkSwapchainKHR sc, int made);
// No swapchain could be made with the mode asked for (gfx_present.c create_swapchain, after three tries): the mode is
// given up for the run, so the next swapchain is made without it.
void vk_lowlat_swapchain_refused(int result);
// The frames go out through a frame generation presenter of the runtime's own (gfx_present.c ng_ready), several presents
// a rendered frame: the direct path steps aside while on is 1.
void vk_lowlat_fg_presents(int on);

// ---- the game's frame. Frames are numbered by the game's presents, frame n ending with the n-th, as
// gfx_counters.frames numbers them at the present (vk_backend.c), so the number a guest thread starts a frame with is
// the one the replay and the present thread end it with.
// A frame starts: the first of the game's pad read (at_input 1, ov_input.c) and its acquire of a window image (0,
// nvn_api_obj.c) after a present. The driver's sleep, then the simulation-start marker; the pad read also sets the
// input marker. Returns at once for a frame that already started and while the mode is off.
void vk_lowlat_frame_start(int at_input);
void vk_lowlat_submit_start(void);          // the frame's first submission is handed over (vk_backend.c vk_be_submit)
void vk_lowlat_game_present(void);          // the game presents it (vk_backend.c vk_be_present): counts it
void vk_lowlat_submitted(uint64_t frame);   // its submissions were replayed (where vk_dlssg_submitted is called)
// nvn_api_obj.c nvnQueuePresentTexture: 1 when the driver's sleep holds the next frame to period_ns, so the pacer must
// not wait as well; 0 when the pacer holds it as before (the mode off, period 0, or no host present went out lately).
int vk_lowlat_pace(uint64_t period_ns);
// vk_sync.c queue_submit: 1 with *tag filled when the submission being handed to the queue is to carry the frame's
// number (NVIDIA's path on); the caller chains it into its VkSubmitInfo2.
int vk_lowlat_submit_tag(VkLatencySubmissionPresentIdNV *tag);

// ---- the host present (the present thread)
void vk_lowlat_present_frame(uint64_t frame);   // vk_backend.c present_main: the next blit is of this frame
// gfx_present.c present_image, at the blit's submit: the frame set above when the direct path follows it (0 otherwise),
// taken, and *tag filled when *tagged is set to 1 as for vk_lowlat_submit_tag.
uint64_t vk_lowlat_blit_frame(VkLatencySubmissionPresentIdNV *tag, int *tagged);
// Around the present of that frame (gfx_present.c flip_locked; frame 0: nothing). Begin: AMD's present stage or
// NVIDIA's present-start marker; returns what goes into the VkPresentInfoKHR's pNext (the frame's present id) or NULL.
// End: the present-end marker and the statistics.
typedef struct { VkPresentIdKHR info; uint64_t id; } VkLowlatPresent;
const void *vk_lowlat_present_begin(uint64_t frame, VkLowlatPresent *p);
void vk_lowlat_present_end(const VkLowlatPresent *p);
