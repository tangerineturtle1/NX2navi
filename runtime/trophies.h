// The port's own trophies (runtime/trophies.c): conditions watched in the game's memory, unlocked locally and kept in
// trophies.dat beside nx2navi.exe. Present path reads trophies_info / trophies_unlocked_next for the menu page and the
// unlock toasts.
#pragma once
#include <stdint.h>

typedef struct {
    const char *id, *title, *desc;
    int points;
    int unlocked;             // granted in this run or in an earlier one
    uint64_t at_ms;           // unlock time (plat_now_ms), 0 for a grant written by an earlier version
} TrophyInfo;

// boot.c, beside cheats_init: needs ldr_load done (main's base resolves the triggers' addresses). Reads trophies.dat.
void trophies_init(void);
// menu.cpp's overlay frame, beside cheats_frame: evaluates the armed triggers once per presented frame.
void trophies_frame(uint64_t frame);
int trophies_on(void);                  // NX2NAVI_TROPHIES
int trophies_count(void);
const TrophyInfo *trophies_info(int i);
// Index of the next unlocked trophy the toast has not shown, -1 when the queue is empty. Silent grants (a save
// already past the event) never queue.
int trophies_unlocked_next(void);
