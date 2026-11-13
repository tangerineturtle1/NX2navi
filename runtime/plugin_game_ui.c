// plugin_game_ui.h: the table, and its check against the running game.
#include <string.h>

#include "dispatch.h"
#include "host.h"
#include "mods.h"
#include "plugin_game_ui.h"

static const PluginGameUi table[] = {
    {   // Ocarina of Time 1.1.1
        .build_id = "e0f3ccfa",
        .layout_anim = 0x43c7a0, .anim_stop_at = 0x2d2d90, .layout_set_text = 0xa67d30, .layout_pane = 0x43e1e0,
        .pane_set_text = 0x439520, .screen_focus = 0x86ffc0, .find_control = 0xdd5410, .control_set_check = 0x3774810,
        .title_vtable = 0x4bf06e8, .title_setup = 0x34067d0, .title_update = 0x3406d70, .title_name_fn = 0x34070d0,
        .title_menu_update = 0x377af70, .entry_focus = 0xa89150, .entry_unfocus = 0xade540,
        .wait_button_update = 0x32c8b60,
        .ts_size = 0x2f0, .ts_layout = 0x28, .ts_controls_box = 0x38, .ts_cursor = 0x188, .ts_open = 0x1a8,
        .ts_parts = 0x2c8, .ts_controls = 0x2d0, .ts_menu_in = 0x2e8,
        .ctl_size = 0x228, .ctl_item = 0xf0, .ctl_pane = 0x60, .pane_y = 0x50,
        .anim_state = 0x20, .anim_frame = 0x18, .anim_res_frames = 0x28,
        .node_flags = 0x18, .node_queued = 0x30, .node_name = 0x48, .node_control = 0xd8, .node_size = 0x110,
        .tm_state = 0x380, .tm_shown = 0x20000, .tm_entries = 0x310, .tm_entry0 = 0xe8, .tm_count = 0x340, .tm_size = 0x388,
        .layout_height = 2160.0f,
        .opt_vtable = 0x4bef108, .opt_build = 0x34011c0, .opt_open = 0x3402f20, .opt_focus = 0x3403f00,
        .opt_decide = 0x3404190, .opt_caption = 0x34031d0, .opt_dtor = 0x34010e0,
        .opt_table_global = 0x4e7cfa8, .opt_table_field = 0x500,
        .sel_left_right = 0x33cfbd0, .sel_text = 0x33cf7f0, .tog_control = 0x33cff40, .act_refresh = 0x33cf080,
        .save_node_update = 0xf8c8e0, .trig_node_update = 0x7550b0, .save_node_state = 0x20, .trig_node_mask = 0x20,
        .os_size = 0x380, .os_layout = 0x28, .os_controls_box = 0x38, .os_rows_n = 0x338, .os_rows = 0x340, .os_focus = 0x378,
        .os_cursor_mgr = 0x20, .os_mgr = 0x60, .os_list = 0x88, .os_list_off = 0x9c,
        .os_kind_count = {0x2c8, 0x2d8, 0x2b8, 0x2e8},
        .row_screen = 0x18, .row_layout = 0x20, .sel_type = 0x58, .sel_index = 0x5c, .sel_original = 0x68,
        .tog_original = 0x64, .tog_anim = 0x50, .act_anim = 0x40, .ctl_check = 0x89, .ctl_enabled = 0x39,
        .ctl_disable_slot = 0x140,
        .pane_flags = 0xa8, .pane_text_mask = 0x3c00000, .pane_text_kind = 0x800000, .pane_visible = 0xa8,
        .ra_title = 0x3404a08, .ra_guide = 0x3404c1c, .ra_label = {0x33d0028, 0x33cf670}, .ra_select_value = 0x33cf8a0,
        .ra_action_text = {0x33cee4c, 0x33cf274, 0x33cf18c}, .ra_caption = 0x3403318,
        .cam_update = 0x4c48c0, .cam_actor = 0x10, .actor_name = 0x20, .cam_pos = 0xd8, .cam_at = 0x100, .cam_up = 0x128,
        .cam_lens = 0x18, .lens_proj = 0x10, .proj_near = 0x98, .proj_far = 0x9c, .proj_fovy = 0xa0, .proj_aspect = 0xb0,
        .cam_reset_button = 8,   // ZL, the game's "lock on and camera reset"
    },
};

static uint64_t main_base(void) {
    for (int i = 0; i < rt_layout_count; i++)
        if (!strcmp(rt_layout[i].name, "main")) return rt_layout[i].base;
    return 0;
}

static uint64_t slot(uint64_t base, uint64_t vtable, uint32_t off) {
    uint64_t v;
    if (!mem_host_readable(base + vtable + off, 8)) return 0;   // a probe without the game's modules in memory
    memcpy(&v, (const void *)(uintptr_t)(base + vtable + off), 8);
    return v - base;
}

// The string a two-instruction function returns the address of (adrp x0; add x0, x0, #lo), "" when it is not one.
static const char *named_by(uint64_t base, uint64_t fn) {
    uint32_t w[2];
    if (!mem_host_readable(base + fn, 8)) return "";
    memcpy(w, (const void *)(uintptr_t)(base + fn), 8);
    if ((w[0] & 0x9f00001f) != 0x90000000 || (w[1] & 0xffc003ff) != 0x91000000) return "";
    int64_t page = (int64_t)((uint64_t)(w[0] >> 5 & 0x7ffff) << 2 | (w[0] >> 29 & 3));
    if (page & 0x100000) page -= 0x200000;
    uint64_t at = ((base + fn) & ~0xfffull) + (uint64_t)(page * 4096) + (w[1] >> 10 & 0xfff);
    return mem_host_readable(at, 16) ? (const char *)(uintptr_t)at : "";
}

static const char *check(const PluginGameUi *g, uint64_t base) {
    const uint64_t fns[] = {g->layout_anim, g->anim_stop_at, g->layout_set_text, g->layout_pane, g->pane_set_text,
        g->screen_focus, g->find_control, g->control_set_check, g->title_setup, g->title_update, g->title_menu_update,
        g->entry_focus, g->entry_unfocus, g->wait_button_update, g->opt_build, g->opt_open, g->opt_focus, g->opt_decide,
        g->opt_caption, g->opt_dtor, g->sel_left_right, g->sel_text, g->tog_control, g->act_refresh,
        g->save_node_update, g->trig_node_update, g->cam_update};
    for (size_t i = 0; i < sizeof fns / sizeof *fns; i++)
        if (!rt_dispatch_lookup(base + fns[i])) return "a function does not start where the table says";
    if (slot(base, g->title_vtable, 0x2e8) != g->title_setup || slot(base, g->title_vtable, 0x300) != g->title_update)
        return "the title screen's class does not hold its set-up and update";
    if (strcmp(named_by(base, g->title_name_fn), "UITitleScreen")) return "the title screen's class is not named UITitleScreen";
    if (slot(base, g->opt_vtable, 0x2f0) != g->opt_build || slot(base, g->opt_vtable, 0x310) != g->opt_open ||
        slot(base, g->opt_vtable, 0x170) != g->opt_focus || slot(base, g->opt_vtable, 0x190) != g->opt_decide)
        return "the settings screen's class does not hold its functions";
    return NULL;
}

static const PluginGameUi *found;
static int done;

void plugin_game_ui_for_probe(void) {
    found = &table[0];
    __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
}

const PluginGameUi *plugin_game_ui(void) {
    if (__atomic_load_n(&done, __ATOMIC_ACQUIRE)) return found;
    const char *id = mods_main_build_id();
    uint64_t base = main_base();
    const PluginGameUi *g = NULL;
    for (size_t i = 0; i < sizeof table / sizeof *table; i++)
        if (id && *id && !strncmp(id, table[i].build_id, strlen(table[i].build_id))) g = &table[i];
    const char *why = !g ? "the runtime has no table for this build of the game" : !base ? "main is not loaded" : check(g, base);
    if (why) rt_log("plugins: the game's user interface is not usable (build %.16s): %s", id ? id : "", why);
    else found = g;
    __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
    return found;
}
