// Where the game's own user interface is, for the plugin interface's title menu entry and game-style menu
// (plugins.c "title menu buttons", plugin_menu.c): every guest address, structure offset and return address those two
// use, in one table per build of the game. Nothing else in the runtime holds such a number for them.
//
// Addresses and layouts belong to a build of the game (its build id), not to a player's PC: every player of 1.1.1 has
// the same main + offset. What can be asked of the game by name is asked by name at run time and is not in the table:
// panes and parts ("Pa_Button_03", "T_Caption_00"), text ids, animations ("BtnNum"), the settings entry's part name.
// docs/plugins.md, "Another game version", says how each entry is found again.
#pragma once
#include <stdint.h>

typedef struct PluginGameUi {
    const char *build_id;            // the start of the main module's build id this row is for (hex)
    // ---- shared user interface functions (offsets in main)
    uint64_t layout_anim;            // (layout, name) -> animation
    uint64_t anim_stop_at;           // (animation, s0 = frame)
    uint64_t layout_set_text;        // (layout, text id, ref, 1, 0) -> text panes set
    uint64_t layout_pane;            // (layout, pane path) -> pane
    uint64_t pane_set_text;          // (text pane, ref, 0)
    uint64_t screen_focus;           // (screen, control, 0)
    uint64_t find_control;           // (screen's control container, part) -> control
    uint64_t control_set_check;      // (control, on)
    // ---- the title screen
    uint64_t title_vtable, title_setup, title_update, title_name_fn;   // name_fn returns "UITitleScreen"
    uint64_t title_menu_update;      // called from the update while the menu is open (the drawn way's hook)
    uint64_t entry_focus, entry_unfocus;   // (control): the cursor comes, the cursor leaves (the drawn way)
    uint64_t wait_button_update;     // (node): SeqExecuteWaitScreenButtonInput's update
    uint32_t ts_size, ts_layout, ts_controls_box, ts_cursor, ts_open, ts_parts, ts_controls, ts_menu_in;
    uint32_t ctl_size, ctl_item, ctl_pane, pane_y;
    uint32_t anim_state, anim_frame, anim_res_frames;
    uint32_t node_flags, node_queued, node_name, node_control, node_size;
    uint32_t tm_state, tm_shown, tm_entries, tm_entry0, tm_count, tm_size;   // the drawn way: the menu's start
    float layout_height;             // layout units from the picture's top to its bottom
    // ---- the settings screen (UISystemOptionScreen) and its rows
    uint64_t opt_vtable, opt_build, opt_open, opt_focus, opt_decide, opt_caption, opt_dtor;
    uint64_t opt_table_global;       // [[main + this] + opt_table_field] -> {int32 count; item *items; int32 selected}
    uint32_t opt_table_field;
    uint64_t sel_left_right, sel_text, tog_control, act_refresh;
    uint64_t save_node_update;       // (node): SeqExecuteSaveGameData's update; state at node + save_node_state
    uint64_t trig_node_update;       // (node): SeqQueryControllerTrigMaskChecker; mask at [[node + trig_node_mask]]
    uint32_t save_node_state, trig_node_mask;
    uint32_t os_size, os_layout, os_controls_box, os_rows_n, os_rows, os_focus, os_cursor_mgr, os_mgr, os_list, os_list_off;
    uint32_t os_kind_count[4];       // toggles, selects, actions, separators: count at this, array 8 further
    uint32_t row_screen, row_layout, sel_type, sel_index, sel_original, tog_original, tog_anim, act_anim, ctl_check, ctl_enabled;
    uint32_t ctl_disable_slot;       // control vtable slot (control, 1): shown as unavailable
    uint32_t pane_flags, pane_text_mask, pane_text_kind;   // a pane is a text pane when (flags & mask) == kind
    uint32_t pane_visible;           // the byte of a pane whose bit 0 says it is drawn
    // return addresses of the game's own text calls, by which a call is told (layout_set_text's callers)
    uint64_t ra_title, ra_guide, ra_label[2], ra_select_value, ra_action_text[3], ra_caption;
    // ---- the scene's camera (plugin_camera.c)
    uint64_t cam_update;             // (camera state): the camera update, once a frame for each of the game's cameras
    uint32_t cam_actor, actor_name;  // state -> the camera's actor -> its name ("PlayerCamera", "EventCamera")
    uint32_t cam_pos, cam_at, cam_up;   // in the state, three floats each: where the camera is, what it looks at, up
    uint32_t cam_lens, lens_proj;    // state -> the camera's view object -> the perspective projection the renderer reads
    uint32_t proj_near, proj_far, proj_fovy, proj_aspect;   // in it, floats: the planes, the vertical angle, width / height
    uint32_t cam_reset_button;       // the bit (input_map.h B_*) of the button that puts the follow camera behind the player
} PluginGameUi;

// The table row for the running game, checked against the game's memory once (functions start where it says, the
// screens' vtables hold them, the title screen's name function names "UITitleScreen"); NULL, with one log line, when
// there is no row for this build or a check fails.
const PluginGameUi *plugin_game_ui(void);
// plugins_probe.c, which runs with the functions of game 1.1.1 and without its data in memory: that game's row, unchecked.
void plugin_game_ui_for_probe(void);
