// Text speed (text_speed.c): the patch set text_speed's page words and switch.
#pragma once

// Called at every controller poll the game makes (ov_input.c npad_states): puts the text_speed setting on the patch
// page and switches the set, so a change in the menu reaches the text that types next.
void text_speed_poll(void);
// 1 when the game version has the patch set text_speed and nothing keeps it off (NX2NAVI_TEXT_SPEED_PATCH=0, a mod
// patching its sites): the Game tab shows the row then. The set itself runs only while the setting is not normal.
int text_speed_available(void);
// The factor the fast setting multiplies the message window's typing speed by.
#define TEXT_SPEED_FAST_FACTOR 3.0f
