#pragma once
#include <stdint.h>

// NX2NAVI_FLOOR_WATCH (floor_watch.c): which code reads and writes the floor under Link, from hardware watchpoints on
// the addresses frame_rate.c's floor scan found. Diagnosis, 1.1.1, Windows; off by default.
int floor_watch_wanted(void);   // the setting, read once: 1 on
// The scan's survivors once it has them (frame_rate.c floor_scan): n addresses of float triplets, their values at
// that present, whether each lies in the platform's rigid body controller unit, and Link's position. 1 when the watch
// runs from now on.
int floor_watch_found(const uint64_t *addr, const float *val, const int *unit, int n, const float link[3]);
// Each present from then on (frame_rate.c frame_step_present), with the game's frames per update. 0 once the watch is
// done.
int floor_watch_present(const float link[3], int link_ok, float frames);
