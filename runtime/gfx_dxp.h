// The DXGI presenter (gfx_dxp_win.cpp, Windows): the present's picture goes to the window through a Direct3D 12
// swapchain instead of the Vulkan one. For HDR on a display that another adapter drives (a laptop's or a desktop's
// integrated graphics with the monitor on the mainboard's output): there the rendering GPU's Vulkan surface offers
// no HDR10 format, while DXGI carries an HDR10 picture across adapters.
//
// The present composes into one of a few images this file shares between the two APIs (Vulkan writes, Direct3D reads)
// and a copy on the Direct3D queue puts it into the swapchain's back buffer. Two shared timelines order the two
// queues without a CPU wait: Vulkan signals `rendered` = n with the frame's submission and the copy waits for it; the
// copy signals `copied` = n and the next Vulkan write of that image waits for that.
//
// All calls with gfx_present.c's g_pl held. Elsewhere dxp_hdr_display is 0 and nothing else is called.
#pragma once
#include "vk.h"

#define DXP_IMAGES 3

#ifdef _WIN32
#ifdef __cplusplus
extern "C" {
#endif
// 1 when the device has the two sharing extensions (vk_device.c), Direct3D 12 opens the Vulkan device's adapter and
// the display the window is on shows HDR10 now (Windows: "Use HDR" on).
int dxp_hdr_display(void);
// Makes the swapchain of the window at w x h, or changes the one there is, with DXP_IMAGES shared images of *fmt in
// imgs. hdr: HDR10 (A2B10G10R10, PQ in Rec. 2020) when the swapchain takes that colour space, else 8-bit sRGB; *hdr
// and *fmt say what it got. The device is idle and the views of earlier images are gone. 0 on failure, with nothing
// left of the earlier images (the swapchain stays for the next try).
int dxp_create(uint32_t w, uint32_t h, int *hdr, VkFormat *fmt, VkImage imgs[DXP_IMAGES]);
void dxp_destroy(void);                       // everything; the device is idle
VkSemaphore dxp_rendered(void);               // the timelines (see above)
// and *value: what `copied` reaches when image idx is free to write again (0: nothing read it yet)
VkSemaphore dxp_copied(uint32_t idx, uint64_t *value);
// Queues the copy of shared image idx behind rendered = n and the present. sync: 1 waits for the refresh; tearing
// (with sync 0): shows the frame at once. 0 when the swapchain has to be made again.
int dxp_present(uint32_t idx, uint64_t n, int sync, int tearing);
void dxp_metadata(float peak_nits, float frame_nits);   // HDR10: the display's peak and the picture's average
#ifdef __cplusplus
}
#endif
#else
static inline int dxp_hdr_display(void) { return 0; }
static inline int dxp_create(uint32_t w, uint32_t h, int *hdr, VkFormat *fmt, VkImage *imgs) {
    (void)w, (void)h, (void)hdr, (void)fmt, (void)imgs;
    return 0;
}
static inline void dxp_destroy(void) {}
static inline VkSemaphore dxp_rendered(void) { return VK_NULL_HANDLE; }
static inline VkSemaphore dxp_copied(uint32_t idx, uint64_t *value) { return (void)idx, *value = 0, VK_NULL_HANDLE; }
static inline int dxp_present(uint32_t idx, uint64_t n, int sync, int tearing) {
    (void)idx, (void)n, (void)sync, (void)tearing;
    return 0;
}
static inline void dxp_metadata(float peak_nits, float frame_nits) { (void)peak_nits, (void)frame_nits; }
#endif
