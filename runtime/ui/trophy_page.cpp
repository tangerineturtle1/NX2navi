// The menu's Trophies tab (ui/trophy_page.h). The set is trophies.c's defs: every trophy is a read-only text row
// (its state changes in the game, never on the row), unlocked ones in the menu's text colour and locked ones greyed
// with "Locked" under the title. The score line counts what the list shows, and the first row is the feature's own
// setting — off, the list still shows what an earlier run earned.
#include <stdio.h>

#include "imgui.h"
#include "ui/option_rows.h"
#include "ui/trophy_page.h"

extern "C" {
#include "trophies.h"
#include "ui_text.h"
}

void trophy_rows(float scale) {
    (void)scale;
    opt_toggle("NX2NAVI_TROPHIES", UI_MENU_TAB_TROPHIES, UI_MENU_TROPHIES_ON_DESC);

    int got = 0, pts = 0, pts_all = 0;
    const int n = trophies_count();
    for (int i = 0; i < n; i++) {
        const TrophyInfo *t = trophies_info(i);
        pts_all += t->points;
        got += t->unlocked;
        pts += t->unlocked ? t->points : 0;
    }
    char score[96];
    snprintf(score, sizeof score, menu_text(UI_MENU_TROPHY_SCORE), got, n, pts, pts_all);
    opt_text_str(score);

    for (int i = 0; i < n; i++) {
        const TrophyInfo *t = trophies_info(i);
        char id[64], pts_s[32];
        snprintf(id, sizeof id, "##trophy_%s", t->id);
        snprintf(pts_s, sizeof pts_s, menu_text(UI_MENU_TROPHY_PTS), t->points);
        if (!t->unlocked) opt_disable_next(UI_MENU_TROPHY_LOCKED);
        opt_text_row(id, t->title, t->unlocked ? pts_s : "", NULL, t->desc, false);
    }
}
