#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef uintptr_t VkPhysicalDevice;
typedef uintptr_t VkSurfaceKHR;
typedef int VkResult;
typedef struct { int format, colorSpace; } VkSurfaceFormatKHR;
enum { VK_SUCCESS = 0, VK_INCOMPLETE = 5 };
enum { STABLE, GROWING, COUNT_ERROR, DATA_ERROR, ALWAYS_INCOMPLETE, EMPTY, DATA_EMPTY, NO_MEMORY };
static int mode, calls;

static VkResult vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice phys, VkSurfaceKHR surface,
                                                     uint32_t *count, VkSurfaceFormatKHR *formats) {
    (void)phys;
    (void)surface;
    calls++;
    if (mode == COUNT_ERROR) return -1;
    if (!formats) {
        *count = mode == EMPTY ? 0 : mode == GROWING && calls == 1 ? 2 : 72;
        return VK_SUCCESS;
    }
    if (mode == DATA_ERROR) return -1;
    if (mode == DATA_EMPTY) { *count = 0; return VK_SUCCESS; }
    if (mode == ALWAYS_INCOMPLETE || *count < 72) return VK_INCOMPLETE;
    for (uint32_t i = 0; i < 72; i++) formats[i] = (VkSurfaceFormatKHR){97, 0};
    // A usable HDR10 pair may be beyond the former 64-entry cutoff.
    formats[65] = (VkSurfaceFormatKHR){58, 1000104008};
    formats[66] = (VkSurfaceFormatKHR){64, 1000104008};
    *count = 72;
    return VK_SUCCESS;
}

static void *test_malloc(size_t bytes) { return mode == NO_MEMORY ? NULL : malloc(bytes); }
#define malloc test_malloc
#include "surface_formats.inc"
#undef malloc

int main(void) {
    for (mode = STABLE; mode <= NO_MEMORY; mode++) {
        calls = 0;
        uint32_t count = 999;
        VkSurfaceFormatKHR *formats = surface_formats(0, 0, &count);
        if (mode == STABLE || mode == GROWING) {
            assert(formats && count == 72);
            assert(formats[65].format == 58 && formats[65].colorSpace == 1000104008);
            assert(formats[66].format == 64 && formats[66].colorSpace == 1000104008);
            assert(calls == (mode == STABLE ? 2 : 4));
            free(formats);
        } else {
            assert(!formats && count == 0);
            int expected = mode == COUNT_ERROR || mode == EMPTY || mode == NO_MEMORY ? 1
                         : mode == ALWAYS_INCOMPLETE ? 8 : 2;
            assert(calls == expected);
        }
    }
    puts("surface formats: 8 scenarios passed");
    return 0;
}
