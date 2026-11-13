// The menu's Co-op tab and what co-op draws over the game (ui/coop_page.cpp, docs/coop.md). Present path only (ImGui).
#pragma once

// Inside the page's opt_begin / opt_end. Co-op off: its switch. Steam not there: why, and Look for Steam again. In no
// room: an invite that waits, Create a room, Join a room, the settings. In a room: the room ID (a press copies it),
// Invite a friend, the players (the host's press removes one), the chat and Leave.
void coop_rows(float scale);
// After opt_end: the window a row opens: a room ID or a chat line to type, the friends to invite, and the
// confirmations of Remove and Leave, each opening with the cursor on its safe button.
void coop_after_page(float scale);
// The window is open: B, Esc, L / R and the tab keys stay with it.
bool coop_modal(void);
// The menu opened or closed (menu.cpp): the window closes.
void coop_cancel(void);
// NX2NAVI_MENU_TEST_SET pseudo-keys (menu.cpp test_apply_sets), from the environment only: COOP_DO=create | leave |
// join:<room id> | chat:<text> | page:join | page:invite | page:chat | page:leave does what the row or the window's
// button does. 1 when key is one of them.
int coop_test_set(const char *key, const char *value);

// Over the game, menu open or not: the other players' names where they stand, and while the menu is closed the chat's
// new lines and an invite that waits. coop_overlay_tick on every overlay frame (it costs a copy of the co-op view while
// co-op is on): true while there is something to draw. coop_overlay_draw draws into a w x h frame.
bool coop_overlay_tick(void);
void coop_overlay_draw(float w, float h, float scale, bool menu_open);
