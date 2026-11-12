// Sprint button (sprint.c): the patch set sprint's host functions and the button's input.
#pragma once
#include <stdint.h>

// Called with the buttons of every controller poll the game gets (ov_input.c npad_states): notes the sprint button's
// presses and keeps it from the game during gameplay.
void sprint_pad(uint64_t *buttons);
// One update of Link's sprint: kind is how his state treats it (sprint.c), button and hold the settings. 1 while on.
int sprint_step(int kind, int button, int hold);
// 1 when the game version has the patch set sprint and it is on: the Controls tab shows the sprint rows then.
int sprint_available(void);
