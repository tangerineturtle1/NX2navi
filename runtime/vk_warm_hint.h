// What the library warm's log adds when the driver's cache of stage libraries is cold (vk_pipeline.c lw_main), by GPU
// vendor, by platform and by why the warm found it cold. Own header so tests/warm_hint_check.c checks it without
// Vulkan.
//
// On NVIDIA under Windows the cache has a size limit, Shader Cache Size in the NVIDIA app. A limit smaller than the
// prepared libraries empties the cache as the warm fills it: the driver drops the oldest libraries to make room for
// the newest, and the next start's sample finds them gone. Measured on an RTX 5070 Ti (driver 617.42): the game's cache
// file stood at exactly 2048 MB, a full warm of 42,122 variants was cold again at the next start (a sample's median
// 241 ms), and with Shader Cache Size set to Unlimited the warm finished, the file grew to 2711 MB and later starts
// found the libraries in the cache (median 1.1 ms). Elsewhere there is no NVIDIA app and the driver's cache is set
// otherwise, so the hints that name the setting are for Windows only.
#pragma once

#include <stdint.h>

enum {
    LW_COLD_FIRST,        // cold with no finished warm before it on this driver: a driver update, a change of what
                          // the libraries are built with, or the build's preparation, which writes no shader-warm.txt
    LW_COLD_AFTER_WARM,   // cold again right after a finished warm on the same driver
    LW_COLD_GAVE_UP,      // cold after two finished warms in a row: warming is no longer tried on this driver
    LW_WARM_FORCED,       // lib_warm = force on a cache the sample found warm: nothing lost, no cause to name
};

#define LW_VENDOR_NVIDIA 0x10de
#ifdef _WIN32
#define LW_ON_WINDOWS 1
#else
#define LW_ON_WINDOWS 0
#endif

// A sentence or two for the end of the log line ("" when there is nothing to add), each starting with a space.
// windows: the game runs on Windows (LW_ON_WINDOWS), where NVIDIA's limit is the NVIDIA app's Shader Cache Size.
static inline const char *lw_cold_hint(uint32_t vendor, int why, int windows) {
    int nvidia = vendor == LW_VENDOR_NVIDIA && windows;
    switch (why) {
    case LW_COLD_FIRST:
        return nvidia ? " On NVIDIA the driver's Shader Cache Size limit empties it too, when it is smaller than the "
                        "prepared libraries (a 2048 MB cache dropped the oldest as the warm built the newest): "
                        "Shader Cache Size Unlimited in the NVIDIA app keeps them."
                      : "";
    case LW_COLD_AFTER_WARM:
        return nvidia ? " A finished warm did not stay in the driver's cache: on NVIDIA that is the Shader Cache Size "
                        "limit, smaller than the prepared libraries; set Shader Cache Size to Unlimited in the NVIDIA "
                        "app, or this warm is lost again."
                      : "";
    case LW_COLD_GAVE_UP:
        return nvidia ? " On NVIDIA that is the Shader Cache Size limit: after setting it to Unlimited in the NVIDIA "
                        "app, start once with lib_warm = force (or delete shader-warm.txt) to warm again."
                      : " Once the driver's cache can hold them, start once with lib_warm = force (or delete "
                        "shader-warm.txt) to warm again.";
    case LW_WARM_FORCED:
        return "";
    }
    return "";
}
