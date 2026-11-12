// Whether the lazy write-back runs (memory.c "Lazy write-back", vk_memory.c vk_mem_lazy_decide), from the player's
// setting, the platform and the GPU driver, with the reason the log gives. Own header so tests/wb_lazy_check.c checks
// it without Vulkan.
//
// The lazy write-back makes pages of guest memory inaccessible while a host-pointer import of them is bound to a buffer
// the GPU writes, and copies their bytes down from inside the access-fault handler of whichever thread touched them. A
// driver is free to handle both differently, so the default is on only where it has been played:
//   - Windows, AMD's own driver: off. In the days after 0.7.1 turned it on for every card on Windows, two reports
//     from AMD cards had the game end in play. An RX 6600 lost its device 3.5 minutes in with it on (146 fetches in
//     the 1,200 frames before; the GPU's last mark was inside a draw pass, not a fetch). A player whose game ended in
//     fights, on a card they did not name, then played for hours with a settings file that turns it off. Neither
//     proves it the cause: the player changed several settings at once, cards with 8 GB lost their device the same
//     way on NVIDIA before it existed, and no failure path was found in the code. But fights are where it is busiest
//     (shaders write, the CPU reads back, every read faults and fetches), and nobody has played one on an AMD card
//     under our eyes: builds with the default on were measured on an RX 7900 XT (driver 26.10.1) standing in Hyrule
//     Field and walking through a village (vk_exec_xfer.c oq_take, vk_exec_draw.c), which says nothing about a fight,
//     and none of us has a card of the driver branch for RDNA 1 and 2. Since the second report's card is unknown, the
//     default is off on both of AMD's branches until a card of each has played fights with it on and a log to show.
//     The cost is the frame rate in open areas, back to what it was before 0.7.1 (about 65 fps against 90 standing in
//     Hyrule Field on an RTX 5090; not measured on AMD).
//   - Windows, any other driver: on, as 0.7.1 shipped it. Measured and played on NVIDIA's; no report from Intel's.
//   - elsewhere: off, as before. The fetch inside the fault handler has only run on Windows.
// A value the player sets wins everywhere: wb_lazy = 1 turns it on for an AMD card, wb_lazy = 0 off for any.
#pragma once

#include <stdint.h>
#include <string.h>

#define WBL_DRIVER_AMD 1   // VkDriverId of AMD's own driver, VK_DRIVER_ID_AMD_PROPRIETARY (vk_memory.c asserts the value)
#ifdef _WIN32
#define WBL_ON_WINDOWS 1
#else
#define WBL_ON_WINDOWS 0
#endif

typedef struct {
    int on;
    const char *why;   // the end of the log line "mem: lazy write-back on|off: "
} WblChoice;

// set: the value the player gave NX2NAVI_WB_LAZY (the environment or nx2navi.ini), NULL or "" when they gave none.
// windows: the game runs on Windows (WBL_ON_WINDOWS). driver_id: the device's VkDriverId.
static inline WblChoice wb_lazy_choice(const char *set, int windows, uint32_t driver_id) {
    if (set && *set) {
        if (!strcmp(set, "0")) return (WblChoice){0, "wb_lazy = 0 is set"};
        return (WblChoice){1, "wb_lazy is set to on"};
    }
    if (!windows)
        return (WblChoice){0, "the default outside Windows, where its fetch inside the access-fault handler has not run "
                              "(wb_lazy = 1 turns it on)"};
    if (driver_id == WBL_DRIVER_AMD)
        return (WblChoice){0, "the default on AMD's driver, where reports had the game end in play with it on and the "
                              "cause is not found; what shaders write goes back at the end of every submission, which "
                              "costs frame rate in open areas (wb_lazy = 1 turns it on)"};
    return (WblChoice){1, "the default on Windows (wb_lazy = 0 turns it off)"};
}
