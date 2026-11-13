// AMD FSR 3.1 temporal upscaling recorded by the runtime itself (vk_fsr3.c): the upscaler's compute passes
// (runtime/shaders/fsr3_*.comp, built into runtime/fsr3_*_spv.h). vk_ffx.cpp routes vk_ffx_upscale here in the builds
// that load no amd_fidelityfx_vk.dll (all but Windows).
#pragma once
#include "vk_ffx.h"

// 1 when the device can run the passes (16-bit storage image formats, quad subgroup operations in compute shaders)
// and their pipelines were made; made once, on the first call after the device exists. 0 before it exists.
int vk_fsr3_init(void);
// Before vk_fsr3_init ran: 1 (the build has the passes); after, what it found. Any thread (the settings menu).
int vk_fsr3_available(void);
// vk_ffx_upscale's contract (vk_ffx.h); every image also carries a view (VkFfxImage.view). sharpness 0..1, RCAS after
// the upscale when sharpen is set. Returns 0 when it recorded nothing.
int vk_fsr3_upscale(VkCommandBuffer cb, const VkFfxImage *color, const VkFfxImage *depth, const VkFfxImage *mv,
                    const VkFfxImage *out, float jx, float jy, int reset, float dt_ms, const VkFfxCamera *cam,
                    int sharpen, float sharpness);
void vk_fsr3_release(void);   // before the device goes away (the GPU idle)
// The upscaler is off (vk_ffx_release_idle, the device idle): whether a context is made, and destroy them; the next
// upscale makes one again. The pipelines stay.
int vk_fsr3_holds(void);
void vk_fsr3_release_idle(void);
int vk_fsr3_contexts_made(void);   // contexts made so far (the probe: what a change of render size makes)
