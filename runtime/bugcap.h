// Bug captures (bugcap.c): F8 writes what is needed to find a bug again where the player saw it, as one folder under
// the log folder, and NX2NAVI_CAPTURE_REPLAY starts a run at a capture's place with its camera and its settings.
// docs/bug-capture.md has the folder's files and the steps.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// perf_present, every present on the render thread: one record into the ring of frame times, and the triggers (F8 while
// the game window is in front, NX2NAVI_CAPTURE_AT=<frame>[,<frame>...], a replay's own capture).
void bugcap_frame(uint64_t frame, uint64_t now_ns, uint64_t interval_ns, uint64_t replay_ns);

// The note on screen after a capture (ui/menu.cpp, the present path): 1 while it shows, with the capture's number,
// whether it was written, and its folder's name ("003-20261113-141502").
int bugcap_note(int *number, int *ok, char name[64]);

// A replay (NX2NAVI_CAPTURE_REPLAY=<a capture's folder>). bugcap_replay_dir: the folder, or NULL without one; cfg.c
// reads the capture's settings.ini in place of nx2navi.ini then and writes neither. The game's thread at its pad read
// (debug_tools.c): bugcap_replay_pad presses A until the save is loaded, bugcap_replay_tick then teleports to the
// capture's area, walks Link to where he stood, puts the free camera where the camera was and takes a capture of its
// own for the comparison.
const char *bugcap_replay_dir(void);
void bugcap_replay_pad(uint64_t *buttons);
void bugcap_replay_tick(uint64_t tick);

#ifdef __cplusplus
}
#endif
