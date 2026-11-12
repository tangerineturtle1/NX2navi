// The trophy unlock toast (ui/trophy_toast.cpp): when a trophy unlocks live (runtime/trophies.c queues it), a panel
// in the menu's look slides in from the right edge at the top right corner, under the performance overlay, stays a
// few seconds and slides back out. One toast at a time; the rest wait in line.
#pragma once

// On every overlay frame (menu open or not): true while a toast is waiting or showing, so the overlay keeps drawing
// with the menu closed.
bool trophy_toast_tick(void);
// Into the foreground of a w x h frame, the top right corner starting top pixels down (the menu passes the bottom of
// the performance overlay so the toast lands under it).
void trophy_toast_draw(float w, float h, float top, float scale);
