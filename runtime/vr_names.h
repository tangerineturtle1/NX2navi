// VR's wheels' item names in the game's language (vr_names.c), from its own message files
#pragma once

// item: the item's name in the game's files ("Item_Hookshot"); its name in the game's language, or fallback when the
// files have none. The text stays valid until the game's language changes.
const char *vr_item_name(const char *item, const char *fallback);
