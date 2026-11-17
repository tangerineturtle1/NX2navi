// VR's wheels (vr_wheel.c): the D-pad's actions and the items, each in a fixed slot (0 up, clockwise), shown by the
// game's own icons for what the save owns. Any thread (game data reads).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum { VR_WHEEL_DPAD = 1, VR_WHEEL_ITEMS = 2, VR_WHEEL_ITEM_SLOTS = 16 };

// A texture of the game's files: its BNTX archive in the RomFS and its name in it
typedef struct {
    char file[96], tex[64];
} VrIcon;

// What a slot shows: its icon and name, and small icons under it: the variants a tap swaps to, or the adult's arrows
// (then mini_selected frames the one in use; -1 none). Counts: the ammunition or stock left, -1 for an item without;
// maxes: how many can be carried now, -1 unknown
typedef struct {
    VrIcon main;
    VrIcon now;            // what is equipped from it: the variant in use (an arrow's own icon where main is the bow)
    const char *label;
    int count, max;
    VrIcon mini[4];
    int mini_count[4], mini_max[4];
    int minis, mini_selected;
} VrSlot;

int vr_wheel_slots(int kind);                        // the wheel's slots
int vr_wheel_slot(int kind, int slot, VrSlot *out);  // 1 when the slot holds something owned, and what it shows
int vr_wheel_usable(int kind, int slot);             // the same without what it shows
void vr_wheel_tap(int kind, int slot);               // a tap on the slot: its next owned variant (kept)
const char *vr_wheel_equip(int slot);                // the item slot's AssignableItem value now, NULL when empty
int vr_wheel_child(void);                            // Link is a child now (Sequence_IsChildhood)

#ifdef __cplusplus
}
#endif
