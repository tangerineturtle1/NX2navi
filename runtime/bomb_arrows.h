// Live bomb-arrow state shared with the menu renderer.
#pragma once
#include <stdint.h>
typedef struct { float pos[3]; uint64_t seen_ns; int armed; } BombArrowVisual;
int bomb_arrows_visuals(BombArrowVisual *out, int capacity);
void bomb_arrows_draw(float width, float height);
typedef struct { float pos[3], right[3], up[3], fwd[3], fov_y, aspect; } BombArrowCamera;
// Renderer snapshots the dominant scene camera without requiring a temporal upscaler.
void bomb_arrows_camera_observe(uint64_t frame,uint64_t address,const float *rows);
int bomb_arrows_camera(BombArrowCamera *out);
int bomb_arrows_camera_needed(void);
