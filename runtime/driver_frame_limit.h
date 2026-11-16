#pragma once
// The frame rate limit the graphics driver holds this game to, read once from the driver's settings: NVIDIA's Max
// Frame Rate (the game's own profile, else the global one) on Windows (driver_frame_limit_win.c). 0 when none is set,
// the driver is not NVIDIA's, or on other hosts (driver_frame_limit_sdl.c).
int driver_frame_limit(void);
