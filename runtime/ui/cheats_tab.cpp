// Built-in policies are grouped by purpose; imported cheat names remain as authored by their mods.
#include <stdio.h>
#include <string.h>
#include "imgui.h"
#include "ui/option_rows.h"
extern "C" {
#include "cheats.h"
#include "ui_text.h"
}

static void builtin_row(int kind) {
    for (int i = 0; i < cheats_count(); i++) {
        if (cheats_builtin_kind(i) != kind) continue;
        const CheatInfo *c = cheats_info(i);
        if (opt_toggle_text(c->key, cheats_display_name(i), cheats_display_note(i), cheats_on(i) != 0)) {
            cheats_set(i, !cheats_on(i));
            opt_save_soon();
        }
        return;
    }
}

static void bottle_rows(void) {
    static const UiTextId ids[] = {UI_MENU_BOTTLE_EMPTY, UI_MENU_BOTTLE_RED, UI_MENU_BOTTLE_GREEN,
        UI_MENU_BOTTLE_BLUE, UI_MENU_BOTTLE_FIRE, UI_MENU_BOTTLE_FISH, UI_MENU_BOTTLE_LETTER,
        UI_MENU_BOTTLE_BIG_POE, UI_MENU_BOTTLE_POE, UI_MENU_BOTTLE_BUG, UI_MENU_BOTTLE_FAIRY,
        UI_MENU_BOTTLE_MILK, UI_MENU_BOTTLE_HALF_MILK};
    const char *names[13];
    for (int i = 0; i < 13; i++) names[i] = menu_text(ids[i]);
    for (int slot = 0; slot < CHEAT_BOTTLE_SLOTS; slot++) {
        int current = cheats_bottle_content(slot);
        if (current < 0) continue;
        char id[24], label[96];
        snprintf(id, sizeof id, "##bottle%d", slot);
        snprintf(label, sizeof label, menu_text(UI_MENU_BOTTLE_LABEL), slot + 1);
        int next = opt_choice_text(id, label, names, 13, current, menu_text(UI_MENU_BOTTLE_DESC));
        if (next >= 0) cheats_bottle_set(slot, next);
    }
}

extern "C" bool tab_cheats(float scale) {
    opt_begin("##cheats", scale);
    int n = cheats_count();
    if (!n) {
        char line[512];
        snprintf(line, sizeof line, menu_text(UI_MENU_CHEATS_NONE), cheats_file()[0] ? cheats_file() : "<build id>.txt");
        opt_text_str(line);
        opt_end();
        return false;
    }
    bool rebuild = cheats_rebuild_pending() && opt_button("##restart", UI_MENU_CHEATS_REBUILD, UI_MENU_CHEATS_REBUILD_DESC);
    bool builtins = false;
    for (int i = 0; i < n; i++) builtins |= cheats_builtin_kind(i) != 0;
    if (builtins) {
        opt_group(UI_MENU_CHEATS_LINK);
        builtin_row(1);
        builtin_row(6);
        opt_toggle("NX2NAVI_AIR_JUMPS", UI_MENU_AIR_JUMPS, UI_MENU_AIR_JUMPS_DESC);
        static const char *const speeds[] = {"normal", "2", "3.5", "5"};
        const char *speed_names[] = {menu_text(UI_MENU_SPRINT_NORMAL), "2x", "3.5x", "5x"};
        opt_choice_list("NX2NAVI_SPRINT_SPEED", UI_MENU_SPRINT_SPEED, speeds, speed_names, 4, UI_MENU_SPRINT_SPEED_DESC);
        builtin_row(2);
        opt_group(UI_MENU_CHEATS_EQUIPMENT);
        opt_toggle("NX2NAVI_BOMB_ARROWS", UI_MENU_BOMB_ARROWS, UI_MENU_BOMB_ARROWS_DESC);
        static const char *const ranges[] = {"normal", "1.5", "2", "2.5"};
        const char *range_names[] = {menu_text(UI_MENU_SPRINT_NORMAL), "1.5x", "2x", "2.5x"};
        opt_choice_list("NX2NAVI_SPIN_RANGE", UI_MENU_SPIN_RANGE, ranges, range_names, 4, UI_MENU_SPIN_RANGE_DESC);
        static const char *const charges[] = {"normal", "instant"};
        const char *charge_names[] = {menu_text(UI_MENU_SPRINT_NORMAL), menu_text(UI_MENU_SPIN_INSTANT)};
        opt_choice_list("NX2NAVI_SPIN_CHARGE", UI_MENU_SPIN_CHARGE, charges, charge_names, 2, UI_MENU_SPIN_CHARGE_DESC);
        opt_group(UI_MENU_CHEATS_WORLD);
        opt_toggle("NX2NAVI_WORLD_SKULLTULAS", UI_MENU_WORLD_SKULLTULAS, UI_MENU_WORLD_SKULLTULAS_DESC);
        opt_toggle("NX2NAVI_WORLD_HEART_PIECES", UI_MENU_WORLD_HEART_PIECES, UI_MENU_WORLD_HEART_PIECES_DESC);
        opt_toggle("NX2NAVI_WORLD_GROTTOS", UI_MENU_WORLD_GROTTOS, UI_MENU_WORLD_GROTTOS_DESC);
        opt_toggle("NX2NAVI_WORLD_CHESTS", UI_MENU_WORLD_CHESTS, UI_MENU_WORLD_CHESTS_DESC);
        opt_toggle("NX2NAVI_WORLD_BEAN_PLOTS", UI_MENU_WORLD_BEAN_PLOTS, UI_MENU_WORLD_BEAN_PLOTS_DESC);
        opt_toggle("NX2NAVI_WORLD_FAIRIES", UI_MENU_WORLD_FAIRIES, UI_MENU_WORLD_FAIRIES_DESC);
        opt_toggle("NX2NAVI_WORLD_UPGRADES", UI_MENU_WORLD_UPGRADES, UI_MENU_WORLD_UPGRADES_DESC);
        opt_group(UI_MENU_CHEATS_INVENTORY);
        builtin_row(13);
        builtin_row(14);
        bottle_rows();
        opt_group(UI_MENU_CHEATS_AMMUNITION);
        static const int ammo[] = {8, 3, 9, 10, 11, 12};
        for (int kind : ammo) builtin_row(kind);
        opt_group(UI_MENU_CHEATS_TIMERS);
        builtin_row(15);
    }
    const char *previous_mod = nullptr;
    static char line[512], desc[768], buttons[192];
    for (int i = 0; i < n; i++) {
        if (cheats_builtin_kind(i)) continue;
        const CheatInfo *c = cheats_info(i);
        if (!previous_mod || strcmp(previous_mod, c->mod)) {
            int listed = 0;
            for (int j = i; j < n; j++)
                listed += !cheats_builtin_kind(j) && !strcmp(cheats_info(j)->mod, c->mod) && cheats_info(j)->kind != CHEAT_HEADING;
            snprintf(line, sizeof line, menu_text(UI_MENU_CHEATS_MOD), c->mod, listed);
            opt_group_text(line);
            previous_mod = c->mod;
        }
        UiTextId text = UI_MENU_CHEAT_CODE_DESC;
        switch (c->kind) {
        case CHEAT_HEADING:
            if (!strstr(c->name, "TID=")) opt_text_str(c->name);
            continue;
        case CHEAT_CODE: break;
        case CHEAT_KEY: text = UI_MENU_CHEAT_KEY_DESC; break;
        case CHEAT_DATA: text = UI_MENU_CHEAT_DATA_DESC; break;
        case CHEAT_MASTER: text = UI_MENU_CHEAT_MASTER_DESC; opt_disable_next(UI_MENU_CHEAT_MASTER); break;
        case CHEAT_RESTORE: text = UI_MENU_CHEAT_RESTORE_DESC; opt_disable_next(UI_MENU_CHEAT_RESTORE); break;
        case CHEAT_REBUILD: text = UI_MENU_CHEAT_REBUILD_DESC; opt_disable_next(UI_MENU_CHEAT_REBUILD); break;
        case CHEAT_UNSUPPORTED: text = UI_MENU_CHEAT_UNSUPPORTED_DESC; opt_disable_next(UI_MENU_CHEAT_UNSUPPORTED); break;
        }
        if (c->kind == CHEAT_KEY)
            snprintf(desc, sizeof desc, menu_text(text), cheats_buttons(c->buttons, buttons, sizeof buttons));
        else if (c->kind == CHEAT_UNSUPPORTED)
            snprintf(desc, sizeof desc, "%s %s", menu_text(text), c->note);
        else snprintf(desc, sizeof desc, "%s", menu_text(text));
        if (opt_toggle_text(c->key, c->name, desc, cheats_on(i) != 0)) {
            cheats_set(i, !cheats_on(i));
            opt_save_soon();
        }
    }
    opt_end();
    return rebuild;
}
