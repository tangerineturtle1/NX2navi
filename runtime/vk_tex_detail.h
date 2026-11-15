// The level texture_detail = auto picks (vk_texture.c "texture detail", vk_texture_detail_auto). Header only, so that
// tests/tex_detail_check.c checks the choice without a device.
#pragma once
#include <stdint.h>

// Full from DETAIL_FULL_MB of device-local budget on, low under DETAIL_LOW_MB, reduced between; under DETAIL_RGBA8_MB
// one step lower where the ASTC textures stay uncompressed. vk_texture.c says where each number comes from.
#define DETAIL_FULL_MB 10240
#define DETAIL_LOW_MB 6144
#define DETAIL_RGBA8_MB 20480

// mb: the device-local budget in MiB, 0 when the device reports none. shared: the video memory is the system's own
// (an integrated GPU), whose budget is a share of the PC's memory: never full. rgba8: the ASTC textures stay RGBA8
// here (astc = cpu, a card without the decoder, astc_bc = 0). Returns 0 full, 1 reduced, 2 low.
static inline int tex_detail_auto(int64_t mb, int shared, int rgba8) {
    int most = !mb || mb >= DETAIL_FULL_MB ? 0 : mb >= DETAIL_LOW_MB ? 1 : 2;
    if (mb && !most && shared) most = 1;
    if (mb && mb < DETAIL_RGBA8_MB && most < 2 && rgba8) most++;
    return most;
}
