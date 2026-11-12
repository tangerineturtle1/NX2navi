// runtime/wb_lazy_choice.h: whether the lazy write-back runs, by the player's setting, the platform and the GPU driver
// (runtime/vk_memory.c vk_mem_lazy_decide). Built and run by tests/wb_lazy_check.py; prints each failed check and
// exits 1 on any.
#include <stdio.h>
#include <string.h>

#include "wb_lazy_choice.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// VkDriverId values (vulkan_core.h)
enum { AMD_PROPRIETARY = 1, AMD_OPEN_SOURCE = 2, MESA_RADV = 3, NVIDIA_PROPRIETARY = 4, INTEL_WINDOWS = 5, MESA_NVK = 24 };
enum { LINUX = 0, WINDOWS = 1 };

static int has(const char *s, const char *part) { return s && strstr(s, part) != NULL; }

int main(void) {
    static const unsigned drivers[] = {AMD_PROPRIETARY, AMD_OPEN_SOURCE, MESA_RADV, NVIDIA_PROPRIETARY, INTEL_WINDOWS,
                                       MESA_NVK, 0};
    enum { ND = sizeof drivers / sizeof *drivers };

    // Nothing set, Windows: on, except on AMD's own driver (both of its branches report this id).
    WblChoice nv = wb_lazy_choice(NULL, WINDOWS, NVIDIA_PROPRIETARY);
    CHECK(nv.on == 1, "Windows, NVIDIA's driver, nothing set: on");
    CHECK(has(nv.why, "default") && has(nv.why, "wb_lazy = 0"), "it says it is the default and how to turn it off: %s", nv.why);
    CHECK(wb_lazy_choice(NULL, WINDOWS, INTEL_WINDOWS).on == 1, "Windows, Intel's driver, nothing set: on");
    CHECK(wb_lazy_choice(NULL, WINDOWS, 0).on == 1, "Windows, a driver that reports no id: on, as 0.7.1 had it");
    WblChoice amd = wb_lazy_choice(NULL, WINDOWS, AMD_PROPRIETARY);
    CHECK(amd.on == 0, "Windows, AMD's driver, nothing set: off");
    CHECK(has(amd.why, "AMD") && has(amd.why, "wb_lazy = 1"), "it names the driver and how to turn it on: %s", amd.why);
    CHECK(has(amd.why, "frame rate"), "it says what off costs: %s", amd.why);
    CHECK(has(amd.why, "default"), "it says it is a default: %s", amd.why);

    // An empty value is no value (cfg.c add: "key =" leaves the setting at its default).
    CHECK(wb_lazy_choice("", WINDOWS, AMD_PROPRIETARY).on == 0, "an empty value on AMD: the default, off");
    CHECK(wb_lazy_choice("", WINDOWS, NVIDIA_PROPRIETARY).on == 1, "an empty value on NVIDIA: the default, on");

    // Nothing set, elsewhere: off on every driver.
    for (int i = 0; i < ND; i++) {
        WblChoice c = wb_lazy_choice(NULL, LINUX, drivers[i]);
        CHECK(c.on == 0 && has(c.why, "wb_lazy = 1"), "Linux, driver %u, nothing set: off, and how to turn it on: %s",
              drivers[i], c.why);
    }

    // What the player sets wins on every platform and driver, and the reason says it was set.
    for (int os = LINUX; os <= WINDOWS; os++)
        for (int i = 0; i < ND; i++) {
            WblChoice on = wb_lazy_choice("1", os, drivers[i]), off = wb_lazy_choice("0", os, drivers[i]);
            CHECK(on.on == 1 && has(on.why, "set"), "os %d driver %u, wb_lazy = 1: on (%s)", os, drivers[i], on.why);
            CHECK(off.on == 0 && has(off.why, "set"), "os %d driver %u, wb_lazy = 0: off (%s)", os, drivers[i], off.why);
            CHECK(!has(on.why, "default") && !has(off.why, "default"), "os %d driver %u: a set value is not a default", os,
                  drivers[i]);
        }
    // Any value but 0 counts as on, as cfg_bool reads it for memory.c mem_init.
    CHECK(wb_lazy_choice("on", WINDOWS, AMD_PROPRIETARY).on == 1, "wb_lazy = on: on");

    // The exception is the driver, not the vendor: another driver for AMD cards follows its platform's default.
    CHECK(wb_lazy_choice(NULL, WINDOWS, AMD_OPEN_SOURCE).on == 1 && wb_lazy_choice(NULL, WINDOWS, MESA_RADV).on == 1,
          "the exception is AMD's own driver");
    CHECK(WBL_DRIVER_AMD == AMD_PROPRIETARY, "WBL_DRIVER_AMD is VK_DRIVER_ID_AMD_PROPRIETARY");

    // The game passes the platform it was built for.
#ifdef _WIN32
    CHECK(WBL_ON_WINDOWS == 1, "a Windows build says so");
#else
    CHECK(WBL_ON_WINDOWS == 0, "a build elsewhere says so");
#endif

    if (!fails) printf("wb lazy choice: all checks passed\n");
    return fails != 0;
}
