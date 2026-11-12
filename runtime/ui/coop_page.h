// The menu's Co-op tab (ui/coop_page.cpp, docs/coop.md): make, join, accept and leave a room. Present path only (ImGui).
#pragma once

// Inside the page's opt_begin / opt_end. Not in a room: the player's name, the relay's address, Make a room, Join a
// room, a status line, the rules of a room this player makes and the co-op switch. In a room: the room's offer while it
// waits for an answer, the players (each one's Remove for the room's maker), the secret (hidden until asked) and its
// copy, the room's rules (the maker changes them, the others read them) and Leave.
void coop_rows(float scale);
// After opt_end: the window a row opens: a name, an address or a secret to type, and the confirmations of Accept,
// Remove and Leave, each opening with the cursor on its safe button.
void coop_after_page(float scale);
// The window is open: B, Esc, L / R and the tab keys stay with it.
bool coop_modal(void);
// The menu opened or closed (menu.cpp): the window closes and the secret is hidden again.
void coop_cancel(void);
// NX2NAVI_MENU_TEST_SET pseudo-keys (menu.cpp test_apply_sets): COOP_PAGE=name|server|join|accept|leave|remove:<id>
// opens that window as its row does, COOP_JOIN=<secret> joins as the Join window's button does, COOP_ACCEPT_OFFER=1
// accepts the room's offer as the Accept window's button does. 1 when key is one of them.
int coop_test_set(const char *key, const char *value);

// Toasts over the game while the menu is closed: a player joined or left the room, a room waits for an answer.
// coop_toast_tick on every overlay frame (menu open or not; it costs a copy of the co-op view while co-op is on): true
// while a toast is to be drawn. coop_toast_draw draws them into the foreground of a w x h frame.
bool coop_toast_tick(void);
void coop_toast_draw(float w, float h, float scale);
