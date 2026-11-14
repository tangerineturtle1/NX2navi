// Amiibo / NFC menu page (ui/amiibo_page.cpp), and the amiibo list on its own over the game (the picker).
#pragma once
#include <stdint.h>

void tab_amiibo(float scale);

// The picker: the amiibo list ui/menu.cpp opens in the menu's place when the game starts waiting for an amiibo
// (NX2NAVI_AMIIBO_POPUP). Present path only.
// Every overlay frame: true on the frame the game starts waiting for an amiibo while the popup setting is on.
bool amiibo_picker_wanted(void);
// The picker opens: the folder is read again and the cursor goes to the list.
void amiibo_picker_begin(void);
// Lays the picker out; false when it should close (a figure was tapped, B or Esc, the game stopped waiting).
bool amiibo_picker_layout(uint32_t w, uint32_t h, float scale);
// After a tap from the picker: "Tapped: <figure>" at the top of the screen for a few seconds.
bool amiibo_note_shows(void);
void amiibo_note_draw(uint32_t w, float scale);
