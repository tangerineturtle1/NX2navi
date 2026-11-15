// DLSS Frame Generation and the card the device is made on (vk_device.c pick_device, vk_dlssg.c
// vk_dlssg_create_device). Header only, so that tests/dlssg_device_check.c checks both decisions without a device.
//
// With frame generation on, the instance and the device are made through Streamline's interposer, whose vkCreateDevice
// asks the card for NVIDIA's extensions (VK_NV_low_latency2 among them) and fails without them
// (VK_ERROR_EXTENSION_NOT_PRESENT, -7). On a laptop with an NVIDIA card and Intel graphics, framegen = dlss and
// gpu_index on the Intel one, that was the only attempt, and every start ended with "no usable Vulkan device" (exit
// code 70) until nx2navi.ini was edited by hand.
#pragma once
#include <stdint.h>

#define DLSSG_VENDOR_NVIDIA 0x10deu

// pick_device: a card the player chose (gpu_index, gpu_name) counts as not usable while the interposer is in use and
// the card is of another vendor, so the best usable card is taken instead, as for a chosen card that is gone. Without
// a choice nothing is skipped: the pass takes what there is, and dlssg_device_create makes the device.
static inline int dlssg_card_skipped(int interposed, int chosen, uint32_t vendor) {
    return interposed && chosen && vendor != DLSSG_VENDOR_NVIDIA;
}

// A creation attempt: 0 (VK_SUCCESS) or the VkResult it failed with.
typedef int (*DlssgCreate)(void *user);

// How the device was made, or tried: by the loader with the interposer not in use; through the interposer; by the
// loader because the card is not an NVIDIA one (the interposer is not asked); by the loader after the interposer
// refused (*refused holds its result). In the last two frame generation is off for the run.
enum { DLSSG_DEV_LOADER, DLSSG_DEV_INTERPOSER, DLSSG_DEV_NOT_NVIDIA, DLSSG_DEV_REFUSED };

// Returns the result of the attempt that counts: the interposer's when it made the device, else the loader's.
static inline int dlssg_device_create(int interposed, uint32_t vendor, DlssgCreate via_interposer, DlssgCreate via_loader,
                                      void *user, int *how, int *refused) {
    *refused = 0;
    if (!interposed) return *how = DLSSG_DEV_LOADER, via_loader(user);
    if (vendor != DLSSG_VENDOR_NVIDIA) return *how = DLSSG_DEV_NOT_NVIDIA, via_loader(user);
    int r = via_interposer(user);
    if (!r) return *how = DLSSG_DEV_INTERPOSER, 0;
    *refused = r;
    return *how = DLSSG_DEV_REFUSED, via_loader(user);
}
