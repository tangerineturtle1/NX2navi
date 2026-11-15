// How the game's task and mesh programs are drawn (vk_device.c create_device). Header only, so that
// tests/mesh_path_check.c checks the decision without a device.
//
//   native     VK_EXT_mesh_shader pipelines
//   fallback   the mesh fallback: the programs run as compute passes and an ordinary draw reads what they wrote
//              (vk_exec_draw.c "mesh fallback", vk_mesh_fb.c)
//   emulation  the mesh emulation (mesh_emulation = 1 on a device without mesh shaders, vk_exec_draw.c); never on an
//              NVIDIA card (below)
//   none       a device without mesh shaders with both off: mesh draws are skipped
//
// mesh_fallback = auto (the default) takes the fallback on a device without mesh shaders, and on AMD's Windows driver
// for RDNA 1 and 2 (vk_caps.amd_sc), which has them on RDNA 2. That driver lost the device ("No fault detected", a
// timeout) minutes into play on RX 6600 to RX 6950 XT cards in every version from 0.6.0 to 0.9.2 (0.10.x built its mesh
// pipelines without the driver's optimiser, NX2NAVI_VK_NOOPT, which no reporter has confirmed), with the pipeline
// library, the replay workers, the asynchronous replay, the lazy write-back and the low-end GPU mode each on and off.
// Where the pass marks named the pass the GPU was in, it was one that starts with a native mesh draw. The same driver
// on RDNA 1 has no mesh shaders and draws through this fallback, and AMD's driver for RDNA 3 and 4 (LLPC) draws them
// natively; neither has such a report. That the native mesh path is the cause is not shown: the fallback is what the
// marks leave to try, at the cost of a compute pass and a pass break per mesh draw, and of mesh programs that are
// left out of the picture until their compute passes are built, at their first sight in a run. mesh_fallback = 0
// draws them natively again; 1 takes the fallback on every device.
//
// The mesh emulation is the older of the two ways for a card without mesh shaders, and the low-end GPU mode turned it
// on up to 0.10.1 (cfg.c lowend_set). On NVIDIA's cards without mesh shaders (GTX 10 and older) it ended the game
// inside the driver at the first mesh draws of an area: an access violation at nvoglv64.dll+0x10cdffe on a GTX 1050 Ti
// and on a GTX 1070, both on 0.10.1, and a Titan X (Pascal) report of the same mode. The GTX 1050 Ti ran on once
// mesh_emulation was set to 0, through the fallback, with the rest of that mode still on. So an NVIDIA card never
// takes the emulation: with it asked for, the card is drawn for as if it were not.
#pragma once
#include <string.h>

enum { MESH_PATH_NATIVE, MESH_PATH_FALLBACK, MESH_PATH_EMULATION, MESH_PATH_NONE };

// has_mesh: the device has VK_EXT_mesh_shader with task and mesh shaders; amd_sc: AMD's Windows driver for RDNA 1 and
// 2; nvidia: an NVIDIA card; fallback: the mesh_fallback setting ("auto", "0", "1"; anything else reads as auto);
// emulation: mesh_emulation ("0" off). *why: the reason, for the boot line.
static inline int mesh_path_decide(int has_mesh, int amd_sc, int nvidia, const char *fallback, const char *emulation,
                                   const char **why) {
    const char *w;
    int path;
    if (!has_mesh && strcmp(emulation, "0") && !nvidia)
        path = MESH_PATH_EMULATION, w = "mesh_emulation = 1 on a device without mesh shaders";
    else if (!strcmp(fallback, "1"))
        path = MESH_PATH_FALLBACK, w = "mesh_fallback = 1";
    else if (!strcmp(fallback, "0"))
        path = has_mesh ? MESH_PATH_NATIVE : MESH_PATH_NONE, w = "mesh_fallback = 0";
    else if (!has_mesh)
        path = MESH_PATH_FALLBACK, w = strcmp(emulation, "0") ? "this device has no mesh shaders; mesh_emulation is "
                                                                "not used on an NVIDIA card, where it ended the game "
                                                                "inside the driver"
                                                              : "this device has no mesh shaders";
    else if (amd_sc)
        path = MESH_PATH_FALLBACK, w = "the default on AMD's driver for RDNA 1 and 2, where native mesh pipelines lost "
                                       "the device in play; mesh_fallback = 0 draws them natively";
    else
        path = MESH_PATH_NATIVE, w = "VK_EXT_mesh_shader";
    if (why) *why = w;
    return path;
}
