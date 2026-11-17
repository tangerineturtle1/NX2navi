// VR's wheels (vr_wheel.h): what each slot holds, read from the game data's names (debug_tools.c), the slots fixed so
// an item is always in the same place. The item wheel's values are the game's AssignableItem enum's, owned by their
// IsGet.<name> flags, the trade items while their stock is above 0, the bottles by the game data's
// PlayerStatus.AssignableItem.BottleInfo (IsValid, and what each holds for its icon and name). A slot may hold variants a tap on it steps through (the bomb
// kinds; the adult's arrows; the seed and the beans), the last one kept.
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "debug_tools.h"
#include "vr_names.h"
#include "vr_wheel.h"

#define ICON_DIR "UI/Tex/Icon/"
#define COMMON_BNTX "UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.bntx.zs"

typedef enum { OWN_GET, OWN_STOCK, OWN_BOTTLE, OWN_ARROW } Own;
typedef struct {
    const char *value;   // the AssignableItem value
    const char *name;    // its item's name: IsGet.<name>, PouchData.<name>.StockNum, the icon <name>
    Own own;             // OWN_ARROW: IsGet.<name> and the bow
    const char *label;   // shown under the wheel when the game's own name is missing (vr_names.c)
    const char *ammo;    // what counts it (PouchData.<ammo>.StockNum), NULL none
} Item;

static const Item kItems[] = {
    {"SlingShot", "Weapon_Range_SlingShot", OWN_GET, "Fairy Slingshot", "Item_DekuSeed"},
    {"Bow", "Weapon_Range_Bow", OWN_GET, "Fairy Bow", "Item_Arrow_Normal"},
    {"FireArrow", "Item_Arrow_Fire", OWN_ARROW, "Fire Arrow", "Item_Arrow_Normal"},
    {"IceArrow", "Item_Arrow_Ice", OWN_ARROW, "Ice Arrow", "Item_Arrow_Normal"},
    {"LightArrow", "Item_Arrow_Light", OWN_ARROW, "Light Arrow", "Item_Arrow_Normal"},
    {"Boomerang", "Item_Boomerang", OWN_GET, "Boomerang"},
    {"Hookshot", "Item_Hookshot", OWN_GET, "Hookshot"},
    {"Longshot", "Item_Longshot", OWN_GET, "Longshot"},
    {"DekuStick", "Item_DekuStick", OWN_GET, "Deku Stick", "Item_DekuStick"},
    {"Hammer", "Item_Hammer", OWN_GET, "Megaton Hammer"},
    {"DekuNut", "Item_DekuNut", OWN_GET, "Deku Nut", "Item_DekuNut"},
    {"Bomb", "Item_Bomb", OWN_GET, "Bombs", "Item_Bomb"},
    {"Bombchu", "Item_Bombchu", OWN_GET, "Bombchus", "Item_Bombchu"},
    {"GlassTruth", "Armor_GlassTruth", OWN_GET, "Lens of Truth"},
    {"DinFire", "Item_DinFire", OWN_GET, "Din's Fire"},
    {"FaroreWind", "Item_FaroreWind", OWN_GET, "Farore's Wind"},
    {"NayruLove", "Item_NayruLove", OWN_GET, "Nayru's Love"},
    {"MagicBean", "Item_MagicBean", OWN_GET, "Magic Beans", "Item_MagicBean"},
    {"ScrubBusinessSeed", "Item_ScrubBusinessSeed", OWN_STOCK, "Deku Seed", "Item_ScrubBusinessSeed"},
    {"BottleA", "A", OWN_BOTTLE, NULL}, {"BottleB", "B", OWN_BOTTLE, NULL}, {"BottleC", "C", OWN_BOTTLE, NULL},
    {"BottleD", "D", OWN_BOTTLE, NULL}, {"BottleE", "E", OWN_BOTTLE, NULL},
    {"WeirdEgg", "Item_WeirdEgg", OWN_STOCK, "Weird Egg"},
    {"AlarmCucco", "Item_AlarmCucco", OWN_STOCK, "Cucco"},
    {"ZeldaLetter", "Item_ZeldaLetter", OWN_STOCK, "Zelda's Letter"},
    {"PocketEgg", "Item_PocketEgg", OWN_STOCK, "Pocket Egg"},
    {"PocketCucco", "Item_PocketCucco", OWN_STOCK, "Pocket Cucco"},
    {"CojiroCucco", "Item_CojiroCucco", OWN_STOCK, "Cojiro"},
    {"OddMushroom", "Item_OddMushroom", OWN_STOCK, "Odd Mushroom"},
    {"OddPowder", "Item_OddPowder", OWN_STOCK, "Odd Potion"},
    {"PoacherSaw", "Item_PoacherSaw", OWN_STOCK, "Poacher's Saw"},
    {"BrokenSword", "Item_BrokenSword", OWN_STOCK, "Broken Goron's Sword"},
    {"Prescription", "Item_Prescription", OWN_STOCK, "Prescription"},
    {"EyeballFrog", "Item_EyeballFrog", OWN_STOCK, "Eyeball Frog"},
    {"EyeDrop", "Item_EyeDrop", OWN_STOCK, "Eye Drops"},
    {"ClaimCheck", "Item_ClaimCheck", OWN_STOCK, "Claim Check"},
};

// A slot's variant: the values it may be, the first owned taken (Longshot before Hookshot, the latest trade item)
#define ALTS 12
typedef struct {
    const char *alt[ALTS];
} Var;
// A slot: its variants for each era (up to 4; a tap steps through the owned ones), the memory of the one chosen (-1
// none: the first owned), and arrows: the variants drawn as a row of small icons under the first's
enum { MEM_ARROWS, MEM_BOMBS, MEM_SEEDS, MEM_COUNT };
typedef struct {
    Var child[4], adult[4];
    int mem, arrows;
} SlotDef;
#define ONE(v) {{{v}}}
static const SlotDef kSlots[VR_WHEEL_ITEM_SLOTS] = {
    {ONE("GlassTruth"), ONE("GlassTruth"), -1, 0},
    {ONE("DekuNut"), ONE("DekuNut"), -1, 0},
    {ONE("DekuStick"), ONE("Hammer"), -1, 0},
    {ONE("SlingShot"), {{{"Bow"}}, {{"FireArrow"}}, {{"IceArrow"}}, {{"LightArrow"}}}, MEM_ARROWS, 1},
    {ONE("Boomerang"), {{{"Longshot", "Hookshot"}}}, -1, 0},
    {{{{"Bomb"}}, {{"Bombchu"}}}, {{{"Bomb"}}, {{"Bombchu"}}}, MEM_BOMBS, 0},
    {ONE("BottleE"), ONE("BottleE"), -1, 0},
    {ONE("BottleD"), ONE("BottleD"), -1, 0},
    {ONE("BottleC"), ONE("BottleC"), -1, 0},
    {ONE("BottleB"), ONE("BottleB"), -1, 0},
    {ONE("BottleA"), ONE("BottleA"), -1, 0},
    {ONE("NayruLove"), ONE("NayruLove"), -1, 0},
    {ONE("FaroreWind"), ONE("FaroreWind"), -1, 0},
    {ONE("DinFire"), ONE("DinFire"), -1, 0},
    {{{{"ZeldaLetter", "AlarmCucco", "WeirdEgg"}}},
     {{{"ClaimCheck", "EyeDrop", "EyeballFrog", "Prescription", "BrokenSword", "PoacherSaw", "OddPowder", "OddMushroom",
         "CojiroCucco", "PocketCucco", "PocketEgg"}}},
     -1, 0},
    {{{{"ScrubBusinessSeed"}}, {{"MagicBean"}}}, {{{"ScrubBusinessSeed"}}, {{"MagicBean"}}}, MEM_SEEDS, 0},
};
static _Atomic int g_mem[MEM_COUNT];   // the variant chosen last (its index in the slot's list)

// A bottle's contents (its BottleItem enum's values), their icons and names
static const char *const kBottle[][3] = {
    {"Empty", "Item_Bottle", "Empty Bottle"},
    {"RedPotion", "Item_Bottle_RedPotion", "Red Potion"},
    {"GreenPotion", "Item_Bottle_GreenPotion", "Green Potion"},
    {"BluePotion", "Item_Bottle_BluePotion", "Blue Potion"},
    {"BlueFire", "Item_Bottle_BlueFire", "Blue Fire"},
    {"Fish", "Item_Bottle_Fish", "Fish"},
    {"Letter", "Item_Bottle_Letter", "Ruto's Letter"},
    {"BigPoe", "Item_Bottle_BigPoe", "Big Poe"},
    {"Poe", "Item_Bottle_Poe", "Poe"},
    {"Bug", "Item_Bottle_Bug", "Bug"},
    {"Fairy", "Item_Bottle_Fairy", "Fairy"},
    {"MaxMilk", "Item_Bottle_Milk", "Lon Lon Milk"},
    {"HalfMilk", "Item_Bottle_HalfMilk", "Lon Lon Milk (half)"},
};
static const char *const kArrowIcon[4] = {"Item_Arrow_Normal", "Item_Arrow_Fire", "Item_Arrow_Ice", "Item_Arrow_Light"};

static int flag(const char *name) {
    int on = 0;
    return dbg_flag_get(name, &on) && on;
}

static const Item *item_of(const char *value) {
    for (size_t i = 0; i < sizeof kItems / sizeof *kItems; i++)
        if (!strcmp(kItems[i].value, value)) return &kItems[i];
    return NULL;
}

// it owned now; *icon and *label its icon's name and its name (a bottle's by what it holds)
static int owned(const Item *it, const char **icon, const char **label) {
    char key[128];
    *icon = it->name, *label = it->label;
    switch (it->own) {
    case OWN_GET:
        snprintf(key, sizeof key, "IsGet.%s", it->name);
        return flag(key);
    case OWN_ARROW:
        snprintf(key, sizeof key, "IsGet.%s", it->name);
        return flag(key) && flag("IsGet.Weapon_Range_Bow");
    case OWN_STOCK: {
        int32_t n = 0;
        snprintf(key, sizeof key, "PouchData.%s.StockNum", it->name);
        return dbg_int_get(key, &n) && n > 0;
    }
    case OWN_BOTTLE: {
        snprintf(key, sizeof key, "PlayerStatus.AssignableItem.BottleInfo.Bottle%s.IsValid", it->name);
        if (!flag(key)) return 0;
        snprintf(key, sizeof key, "PlayerStatus.AssignableItem.BottleInfo.Bottle%s.BottleItem", it->name);
        uint32_t held = 0;
        *icon = kBottle[0][1], *label = kBottle[0][2];
        if (dbg_enum_get(key, &held))
            for (size_t i = 0; i < sizeof kBottle / sizeof *kBottle; i++)
                if (dbg_name_hash(kBottle[i][0]) == held) *icon = kBottle[i][1], *label = kBottle[i][2];
        return 1;
    }
    }
    return 0;
}

int vr_wheel_child(void) {
    int child = 1;
    dbg_flag_get("Sequence_IsChildhood", &child);
    return child;
}

static void icon_file(VrIcon *out, const char *name) {
    snprintf(out->file, sizeof out->file, ICON_DIR "%s.bntx.zs", name);
    snprintf(out->tex, sizeof out->tex, "%s", name);
}

// A variant's value owned now (the first of its alternatives), its icon and name; NULL when none is owned
static const char *variant(const Var *v, const char **icon, const char **label) {
    for (int i = 0; i < ALTS && v->alt[i]; i++) {
        const Item *it = item_of(v->alt[i]);
        if (it && owned(it, icon, label)) return it->value;
    }
    return NULL;
}

// How many of a counted item can be carried: the largest of its bags owned (the game's RSDB/PouchActorInfo,
// AfterExpansionMaxNum of each bag, the bags the item's MaxNumGameData shares; the smallest without one), or its
// FixedMaxNum; -1 unknown
static int max_of(const char *ammo) {
    static const struct { const char *ammo, *bag; int n[3]; } bags[] = {
        {"Item_Bomb", "BombBag", {20, 30, 40}},        {"Item_Bombchu", "BombBag", {20, 30, 40}},
        {"Item_Arrow_Normal", "Quiver", {30, 40, 50}}, {"Item_DekuSeed", "BulletBag", {30, 40, 50}},
        {"Item_DekuNut", "NutBag", {20, 30, 40}},      {"Item_DekuStick", "StickBag", {10, 20, 30}},
    };
    static const char *const sizes[3] = {"Small", "Medium", "Large"};
    if (!strcmp(ammo, "Item_MagicBean")) return 10;
    for (size_t i = 0; i < sizeof bags / sizeof *bags; i++) {
        if (strcmp(bags[i].ammo, ammo)) continue;
        char key[128];
        for (int s = 2; s > 0; s--) {
            snprintf(key, sizeof key, "IsGet.Item_%s_%s", bags[i].bag, sizes[s]);
            if (flag(key)) return bags[i].n[s];
        }
        return bags[i].n[0];
    }
    return -1;
}

// A value's ammunition or stock now (-1 for an item that has none) and *max how many can be carried
static int count_of(const char *value, int *max) {
    const Item *it = value ? item_of(value) : NULL;
    char key[128];
    int32_t n = 0;
    *max = -1;
    if (!it || !it->ammo) return -1;
    snprintf(key, sizeof key, "PouchData.%s.StockNum", it->ammo);
    if (!dbg_int_get(key, &n)) return -1;
    *max = max_of(it->ammo);
    return n;
}

// The item slot now: its variants owned (their indices in the list, *n of them) and the one in use (an index of those)
static const Var *slot_vars(int slot, int *count) {
    const Var *vars = vr_wheel_child() ? kSlots[slot].child : kSlots[slot].adult;
    *count = 0;
    while (*count < 4 && vars[*count].alt[0]) (*count)++;
    return vars;
}
static int slot_current(int slot, const Var *vars, int count, int owned_idx[4], int *n_owned) {
    const char *icon, *label;
    *n_owned = 0;
    for (int i = 0; i < count; i++)
        if (variant(&vars[i], &icon, &label)) owned_idx[(*n_owned)++] = i;
    if (!*n_owned) return -1;
    const int mem = kSlots[slot].mem;
    const int want = mem >= 0 ? atomic_load(&g_mem[mem]) : 0;
    for (int k = 0; k < *n_owned; k++)
        if (owned_idx[k] == want) return k;
    return 0;
}

int vr_wheel_slots(int kind) { return kind == VR_WHEEL_DPAD ? 4 : VR_WHEEL_ITEM_SLOTS; }

static int dpad_slot(int slot, VrSlot *out) {
    static const char *const masks[][2] = {
        {"Armor_Child_Mask_Keaton", "Keaton Mask"}, {"Armor_Child_Mask_Skull", "Skull Mask"},
        {"Armor_Child_Mask_Spooky", "Spooky Mask"}, {"Armor_Child_Mask_Bunny", "Bunny Hood"},
        {"Armor_Child_Mask_Truth", "Mask of Truth"}, {"Armor_Child_Mask_Goron", "Goron Mask"},
        {"Armor_Child_Mask_Zora", "Zora Mask"},     {"Armor_Child_Mask_Gerudo", "Gerudo Mask"}};
    static const char *const boots[][2] = {{"Armor_Adult_HoverBoot", "Hover Boots"},
                                           {"Armor_Adult_HeavyBoot", "Iron Boots"}};
    static const char *const ocarinas[][2] = {{"Item_Ocarina_Time", "Ocarina of Time"},
                                              {"Item_Ocarina_Fairy", "Fairy Ocarina"}};
    const char *const(*list)[2] = NULL;
    size_t n = 0;
    char key[128];
    switch (slot) {
    case 0: list = masks, n = sizeof masks / sizeof *masks; break;   // the accessory
    case 1:   // Navi, always
        snprintf(out->main.file, sizeof out->main.file, "%s", COMMON_BNTX);
        snprintf(out->main.tex, sizeof out->main.tex, "HUDNaviIcon_Body_00");
        out->now = out->main;
        out->label = "Navi";   // (the game's item names do not name her)
        return 1;
    case 2: list = boots, n = sizeof boots / sizeof *boots; break;
    case 3: list = ocarinas, n = sizeof ocarinas / sizeof *ocarinas; break;
    default: return 0;
    }
    for (size_t i = 0; i < n; i++) {
        snprintf(key, sizeof key, "IsGet.%s", list[i][0]);
        if (flag(key)) {
            icon_file(&out->main, list[i][0]);
            out->now = out->main;
            out->label = vr_item_name(list[i][0], list[i][1]);
            return 1;
        }
    }
    return 0;
}

int vr_wheel_slot(int kind, int slot, VrSlot *out) {
    memset(out, 0, sizeof *out);
    out->mini_selected = -1, out->count = out->max = -1;
    for (int i = 0; i < 4; i++) out->mini_count[i] = out->mini_max[i] = -1;
    if (kind == VR_WHEEL_DPAD) return dpad_slot(slot, out);
    if (slot < 0 || slot >= VR_WHEEL_ITEM_SLOTS) return 0;
    int count, owned_idx[4], n_owned;
    const Var *vars = slot_vars(slot, &count);
    const int cur = slot_current(slot, vars, count, owned_idx, &n_owned);
    if (cur < 0) return 0;
    const char *icon, *label;
    const char *value = variant(&vars[owned_idx[cur]], &icon, &label);
    out->label = vr_item_name(icon, label);   // (an item's icon is named as the item is)
    out->count = count_of(value, &out->max);   // (the arrows share one count, the bow's)
    icon_file(&out->now, owned_idx[cur] == 0 || !kSlots[slot].arrows ? icon : kArrowIcon[owned_idx[cur] & 3]);
    if (kSlots[slot].arrows && !vr_wheel_child()) {   // the bow, its arrows under it, the one in use framed
        icon_file(&out->main, "Weapon_Range_Bow");
        for (int k = 0; k < n_owned; k++) {
            icon_file(&out->mini[out->minis], kArrowIcon[owned_idx[k] & 3]);
            if (k == cur) out->mini_selected = out->minis;
            out->minis++;
        }
        return 1;
    }
    icon_file(&out->main, icon);
    for (int k = 0; k < n_owned && out->minis < 3; k++) {   // the others a tap swaps to
        const char *other = k != cur ? variant(&vars[owned_idx[k]], &icon, &label) : NULL;
        if (other) {
            out->mini_count[out->minis] = count_of(other, &out->mini_max[out->minis]);
            icon_file(&out->mini[out->minis++], icon);
        }
    }
    return 1;
}

int vr_wheel_usable(int kind, int slot) {
    VrSlot s;
    return vr_wheel_slot(kind, slot, &s);
}

void vr_wheel_tap(int kind, int slot) {
    if (kind != VR_WHEEL_ITEMS || slot < 0 || slot >= VR_WHEEL_ITEM_SLOTS || kSlots[slot].mem < 0) return;
    int count, owned_idx[4], n_owned;
    const Var *vars = slot_vars(slot, &count);
    const int cur = slot_current(slot, vars, count, owned_idx, &n_owned);
    if (cur < 0 || n_owned < 2) return;
    atomic_store(&g_mem[kSlots[slot].mem], owned_idx[(cur + 1) % n_owned]);
}

const char *vr_wheel_equip(int slot) {
    if (slot < 0 || slot >= VR_WHEEL_ITEM_SLOTS) return NULL;
    int count, owned_idx[4], n_owned;
    const Var *vars = slot_vars(slot, &count);
    const int cur = slot_current(slot, vars, count, owned_idx, &n_owned);
    const char *icon, *label;
    return cur < 0 ? NULL : variant(&vars[owned_idx[cur]], &icon, &label);
}
