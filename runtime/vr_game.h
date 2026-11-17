// VR's hooks in the game's camera update (Ocarina of Time 1.1.1): vr_game.c, for vk_xr.cpp
#pragma once

#include "cpu.h"

// The set vrlens's site (recomp/patches-v111.txt), the patch page's +0xf90: the camera's angle load
void vr_lens_host(Cpu *c);
// debug_tools.c dbg_freecam_host, a scene camera's update (1 the gameplay camera, 2 an event's), its position, target
// and up in the site's registers: the head's view put on the gameplay camera
void vr_camera_update(Cpu *c, int scene);
// vk_xr.cpp, the session made with the head's view wanted: the camera hooks switched on
void vr_game_start(void);
// ov_input.c npad_states, the pad's buttons and left stick: ZL held for Link's strafe in first person, the stick
// turned to the camera's way while he runs forward with the head about at rest
void vr_pad(uint64_t *buttons, int32_t *lx, int32_t *ly);
// ov_input.c npad_states, the game's thread: VR's wheels' pause and the item they equip
void vr_tick(Cpu *c);
