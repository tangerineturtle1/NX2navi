// Minimap toggle (minimap.c): hides the game's minimap, switched by a button during gameplay. Ocarina of Time 1.1.1.
#pragma once
#include <stdint.h>

#include "cpu.h"

// Once per pad read on the game thread (ov_input.c npad_states): the button's press during gameplay (kept from the
// game) and the minimap's drawn byte. buttons may be NULL when the controller is not live.
void minimap_pad(Cpu *c, uint64_t *buttons);
// 1 where the button can work: 1.1.1 with the patch set minimap on at the start (the Controls tab's button row).
int minimap_available(void);
