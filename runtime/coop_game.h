// Co-op, the game side (coop_game.c): where this game's Link is and the progress a room shares. coop.h declares the
// part the co-op thread calls (coop_game_message, coop_game_joined, coop_game_room).
#pragma once

#include "cpu.h"

void coop_game_init(void);
// At each present, on the game's thread, between two frames: reads Link and the game's flags and sets the flags the
// room has and this game lacks, through the game's own routines. Does nothing while the COOP setting is off.
void coop_present(Cpu *c);
// Each RomFS file the game opens (fs_host.c): the title screen's start is told by its event flow being opened.
void coop_game_rom_open(const char *path);
