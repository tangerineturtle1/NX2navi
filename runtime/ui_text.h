// Text the host shows the player (runtime/ui_text.c): one catalog, keyed by stable ids. The software keyboard reads it
// in the game's language (ui_text); the menu and the performance overlay read it in the menu language (menu_text).
#pragma once

typedef enum {
    // Software keyboard
    UI_SWKBD_TITLE,          // caption of the text-entry window
    UI_SWKBD_PROMPT,         // prompt when the game gives no header, guide or sub text (NX2navi names a horse)
    UI_SWKBD_DEFAULT_NAME,   // text the entry starts with when the game gives none, so a controller can confirm it
    UI_SWKBD_OK,
    UI_SWKBD_CANCEL,
    UI_SWKBD_PAD_HINT,       // how a controller answers

    // Menu: window, tabs, bottom row. UI_MENU_FIRST..UI_MENU_LAST_ID carry English, Russian, Brazilian Portuguese and
    // Simplified Chinese texts.
    UI_MENU_FIRST,
    UI_MENU_TITLE = UI_MENU_FIRST,
    UI_MENU_TAB_SCREEN, UI_MENU_TAB_GAME, UI_MENU_TAB_MODS, UI_MENU_TAB_SOUND, UI_MENU_TAB_CONTROLS, UI_MENU_TAB_AMIIBO,
    UI_MENU_TAB_MENU, UI_MENU_TAB_ADVANCED,
    UI_MENU_CONTINUE, UI_MENU_QUIT,
    UI_MENU_ON, UI_MENU_OFF, UI_MENU_AUTO, UI_MENU_DEFAULT_MARK,      // "default" beside the default value
    UI_MENU_AFTER_RESTART,    // note on a setting that is read at start
    UI_MENU_SET_BY_ENV,       // note on a setting an environment variable fixes
    UI_MENU_SAVE_FAILED,
    UI_MENU_INI_UNKNOWN,      // "...: %s": the keys of nx2navi.ini that are no settings of this version (cfg_unknown_keys)
    UI_MENU_VERSION,          // "NX2navi %s, game %s": the port version and the game update, beside the title

    // Screen
    UI_MENU_FULLSCREEN, UI_MENU_FULLSCREEN_DESC,
    UI_MENU_WINDOW_SIZE, UI_MENU_WINDOW_SIZE_DESC, UI_MENU_WINDOW_SIZE_CUSTOM,
    UI_MENU_PRESENT_DESC, UI_MENU_PRESENT_MAILBOX, UI_MENU_PRESENT_VSYNC, UI_MENU_PRESENT_IMMEDIATE,
    UI_MENU_PRESENT_IN_USE,   // "in use: %s" under the value when the swapchain got another mode than the setting
    UI_MENU_GPU, UI_MENU_GPU_DESC, UI_MENU_GPU_AUTO,
    UI_MENU_PERF_OVERLAY, UI_MENU_PERF_OVERLAY_DESC, UI_MENU_PERF_OFF, UI_MENU_PERF_FPS, UI_MENU_PERF_FULL,
    UI_PERF_LINE_FPS,
    UI_MENU_GRAPHICS,         // group heading over the resolution rows
    UI_MENU_OUTPUT_RES, UI_MENU_OUTPUT_RES_DESC,
    UI_MENU_RENDER_SCALE, UI_MENU_RENDER_SCALE_DESC, UI_MENU_RENDER_SCALE_GAME,
    UI_MENU_UPSCALER, UI_MENU_UPSCALER_DESC, UI_MENU_UPSCALER_FSR, UI_MENU_UPSCALER_BILINEAR,
    UI_MENU_GAME_ASPECT, UI_MENU_GAME_ASPECT_DESC, UI_MENU_GAME_ASPECT_16_9, UI_MENU_GAME_ASPECT_WINDOW,
    UI_MENU_GAME_ASPECT_21_9, UI_MENU_GAME_ASPECT_32_9,
    UI_MENU_FOV, UI_MENU_FOV_DESC,
    UI_MENU_SHADOW_RES, UI_MENU_SHADOW_RES_DESC, UI_MENU_SHADOW_RES_GAME,
    UI_MENU_ANISO, UI_MENU_ANISO_DESC, UI_MENU_ANISO_GAME,
    UI_MENU_TEX_DETAIL, UI_MENU_TEX_DETAIL_DESC,
    UI_MENU_TEX_DETAIL_AUTO,   // "Automatic: %s", %s the level it picks on this card
    UI_MENU_TEX_DETAIL_FULL, UI_MENU_TEX_DETAIL_REDUCED, UI_MENU_TEX_DETAIL_LOW,
    UI_MENU_SHARPNESS, UI_MENU_SHARPNESS_DESC, UI_MENU_SHARPNESS_NEEDS_FSR,
    UI_MENU_COLOR,            // group heading over the colour rows
    UI_MENU_COLOR_PRESET, UI_MENU_COLOR_PRESET_DESC, UI_MENU_COLOR_DEFAULT, UI_MENU_COLOR_VIVID, UI_MENU_COLOR_CLARITY,
    UI_MENU_COLOR_CUSTOM,
    UI_MENU_COLOR_BRIGHTNESS, UI_MENU_COLOR_BRIGHTNESS_DESC, UI_MENU_COLOR_CONTRAST, UI_MENU_COLOR_CONTRAST_DESC,
    UI_MENU_COLOR_SATURATION, UI_MENU_COLOR_SATURATION_DESC, UI_MENU_COLOR_GAMMA, UI_MENU_COLOR_GAMMA_DESC,
    UI_MENU_HDR_GROUP,        // group heading over the HDR rows
    UI_MENU_HDR, UI_MENU_HDR_DESC, UI_MENU_HDR_PEAK, UI_MENU_HDR_PEAK_DESC, UI_MENU_HDR_GAME, UI_MENU_HDR_GAME_DESC,
    UI_MENU_HDR_UI, UI_MENU_HDR_UI_DESC, UI_MENU_SHADOW_SCALE, UI_MENU_SHADOW_SCALE_DESC,
    UI_MENU_SHADOW_RANGE, UI_MENU_SHADOW_RANGE_DESC,
    UI_MENU_CONTACT_SHADOWS, UI_MENU_CONTACT_SHADOWS_DESC,
    UI_MENU_PCSS, UI_MENU_PCSS_DESC,
    UI_MENU_DLAA, UI_MENU_DLAA_DESC, UI_MENU_DLSS, UI_MENU_DLSS_DESC, UI_MENU_DLSS_DLAA, UI_MENU_DLSS_QUALITY,
    UI_MENU_DLSS_BALANCED, UI_MENU_DLSS_PERFORMANCE, UI_MENU_DLSS_ULTRA, UI_MENU_FSR3_NATIVE, UI_MENU_DLSS_PRESET, UI_MENU_DLSS_PRESET_DESC,
    UI_MENU_DLSS_PRESET_DEFAULT, UI_MENU_DLSS_PRESET_K, UI_MENU_DLSS_PRESET_J, UI_MENU_DLSS_CHARS, UI_MENU_DLSS_CHARS_DESC,
    UI_MENU_DLSS_CHARS_OFF, UI_MENU_DLSS_CHARS_LIGHT, UI_MENU_DLSS_CHARS_MEDIUM, UI_MENU_DLSS_CHARS_FULL, UI_MENU_FXAA, UI_MENU_FXAA_DESC,

    // Game
    UI_MENU_FPS_LIMIT, UI_MENU_FPS_LIMIT_DESC, UI_MENU_FPS_30, UI_MENU_FPS_40, UI_MENU_FPS_45, UI_MENU_FPS_60, UI_MENU_FPS_90,
    UI_MENU_FPS_120, UI_MENU_FPS_144,
    UI_MENU_FPS_DISPLAY, UI_MENU_FPS_DISPLAY_HZ, UI_MENU_FPS_UNLIMITED, UI_MENU_FPS_RTSS, UI_MENU_FPS_RTSS_READONLY, UI_MENU_FPS_RTSS_IGNORED,
    UI_MENU_REALTIME, UI_MENU_REALTIME_DESC,
    UI_MENU_CLOTH_REALTIME, UI_MENU_CLOTH_REALTIME_DESC,
    UI_MENU_HIGHFPS_FIX, UI_MENU_HIGHFPS_FIX_DESC,
    UI_MENU_GAME_LANG, UI_MENU_GAME_LANG_DESC, UI_MENU_GAME_LANG_AUTO,
    UI_MENU_OPERATION_MODE, UI_MENU_OPERATION_MODE_DESC, UI_MENU_MODE_DOCKED, UI_MENU_MODE_HANDHELD,
    UI_MENU_ASSUME_FOCUS, UI_MENU_ASSUME_FOCUS_DESC,

    // Mods: the port's own patches and the player's mods folder (mods.c)
    UI_MENU_MODS_BUILTIN, UI_MENU_MODS_INSTALLED,
    UI_MENU_MODS_NONE,           // no mod folder yet: where to unpack one
    UI_MENU_MODS_OPEN, UI_MENU_MODS_OPEN_DESC,
    UI_MENU_MODS_ROW_DESC,       // printf: files it replaces, files it adds (as of this start)
    UI_MENU_MODS_OVERRIDDEN,     // printf: files a higher mod serves instead, that mod's name
    UI_MENU_MODS_MOVE_HINT,
    UI_MENU_MODS_EMPTY, UI_MENU_MODS_SUBSDK, UI_MENU_MODS_EXEFS_LATER, UI_MENU_MODS_BAD_NAME,
    UI_MENU_MODS_EXEFS_UNSUPPORTED,  // printf: the file and why
    UI_MENU_MODS_EXEFS_OTHER,
    UI_MENU_MODS_EXEFS_ON,           // printf: its ExeFS patch files in use
    UI_MENU_MODS_REPLACES_BUILTIN,   // printf: its ExeFS patch files in use, the port's own patch sets they turn off
    UI_MENU_MODS_OTHER_VERSION,  // printf: files made for another game version, left out
    UI_MENU_MODS_UNUSED,         // under the switch of a mod the port cannot use
    UI_MENU_MODS_CHEATS,         // printf: its cheats as read at start
    UI_MENU_MODS_CHEATS_FILE,    // printf: <build id>.txt, when none were read
    UI_MENU_MODS_FPS60, UI_MENU_MODS_FPS60_DESC, UI_MENU_MODS_RES, UI_MENU_MODS_RES_DESC,
    UI_MENU_MODS_QUICKMENU, UI_MENU_MODS_QUICKMENU_DESC,

    // Sound
    UI_MENU_VOLUME, UI_MENU_VOLUME_DESC, UI_MENU_MENU_SOUNDS, UI_MENU_MENU_SOUNDS_DESC,
    UI_MENU_AUDIO_CHANNELS, UI_MENU_AUDIO_CHANNELS_DESC, UI_MENU_CHANNELS_AUTO, UI_MENU_CHANNELS_STEREO, UI_MENU_CHANNELS_51,

    // Controls
    UI_MENU_MOTION, UI_MENU_MOTION_DESC,
    UI_MENU_RUMBLE_STRENGTH, UI_MENU_RUMBLE_STRENGTH_DESC,
    UI_MENU_RUMBLE_CURVE, UI_MENU_RUMBLE_CURVE_DESC,
    UI_MENU_RUMBLE_TRIGGERS, UI_MENU_RUMBLE_TRIGGERS_DESC,
    UI_MENU_PAD_LAYOUT, UI_MENU_PAD_LAYOUT_DESC, UI_MENU_PAD_LAYOUT_AUTO, UI_MENU_PAD_LAYOUT_NINTENDO,
    UI_MENU_PAD_LAYOUT_XBOX, UI_MENU_PAD_LAYOUT_PLAYSTATION, UI_MENU_PAD_LAYOUT_IN_USE,
    UI_MENU_PROMPTS, UI_MENU_PROMPTS_DESC, UI_MENU_PROMPTS_NINTENDO, UI_MENU_PROMPTS_XBOX, UI_MENU_PROMPTS_PLAYSTATION,
    UI_MENU_PROMPTS_STEAMDECK, UI_MENU_PROMPTS_PC,
    // the notes under the prompts: their files are missing or damaged, they change after restart, or why they are off
    UI_MENU_PROMPTS_MISSING, UI_MENU_PROMPTS_FAILED, UI_MENU_PROMPTS_RESTART, UI_MENU_PROMPTS_OFF_SHAPE,
    UI_MENU_PROMPTS_OFF_MODS, UI_MENU_PROMPTS_OFF_PATCHES,
    // "Restart now" under the prompts while they change after restart, and its confirmation (Cancel: UI_MENU_RESET_NO)
    UI_MENU_RESTART, UI_MENU_RESTART_DESC, UI_MENU_RESTART_CONFIRM, UI_MENU_QUIT_CONFIRM, UI_MENU_RESTART_YES, UI_MENU_RESTART_SAVING, UI_MENU_QUIT_SAVING,
    UI_MENU_RESTART_COUNT,   // the bar's Restart with the count of settings waiting for it (printf: %d)
    UI_MENU_GROUP_STICKS, UI_MENU_LSTICK_DEADZONE, UI_MENU_RSTICK_DEADZONE, UI_MENU_STICK_DEADZONE_DESC,
    UI_MENU_STICK_DEADZONE_NOW, UI_MENU_STICK_SENS, UI_MENU_STICK_SENS_DESC,
    UI_MENU_MOUSE_MODE, UI_MENU_MOUSE_MODE_DESC, UI_MENU_MOUSE_STICK, UI_MENU_MOUSE_DIRECT,
    UI_MENU_MENU_CURSOR, UI_MENU_MENU_CURSOR_DESC,
    UI_MENU_MOUSE_SENS, UI_MENU_MOUSE_SENS_DESC,
    UI_MENU_WALK_KEY, UI_MENU_WALK_KEY_DESC, UI_MENU_WALK_KEY_TAKEN, UI_MENU_WALK_SCALE, UI_MENU_WALK_SCALE_DESC,

    // Controls tab: the remapping rows (ui/binds_page.cpp)
    UI_BINDS_HELP, UI_BINDS_COL_KEY1, UI_BINDS_COL_KEY2, UI_BINDS_COL_PAD,
    UI_BINDS_GROUP_BUTTONS, UI_BINDS_GROUP_MOVE, UI_BINDS_GROUP_CAMERA,
    UI_BINDS_PRESS_KEY,       // "Press a key or mouse button for %s"
    UI_BINDS_PRESS_PAD,       // "Press a controller button for %s"
    UI_BINDS_CANCEL_HINT,     // "Esc cancels, or wait %d s"
    UI_BINDS_TAKEN,           // "%s was taken from %s": the key, the action that lost it
    UI_BINDS_SWAPPED,         // "%s swapped with %s": the two actions
    UI_BINDS_RESET, UI_BINDS_RESET_DESC,
    // the actions, in action order (input_map.h B_A .. B_SHAKE)
    UI_BIND_A, UI_BIND_B, UI_BIND_X, UI_BIND_Y, UI_BIND_LCLICK, UI_BIND_RCLICK, UI_BIND_L, UI_BIND_R, UI_BIND_ZL,
    UI_BIND_ZR, UI_BIND_PLUS, UI_BIND_MINUS, UI_BIND_LEFT, UI_BIND_UP, UI_BIND_RIGHT, UI_BIND_DOWN,
    UI_BIND_LSTICK_LEFT, UI_BIND_LSTICK_UP, UI_BIND_LSTICK_RIGHT, UI_BIND_LSTICK_DOWN,
    UI_BIND_RSTICK_LEFT, UI_BIND_RSTICK_UP, UI_BIND_RSTICK_RIGHT, UI_BIND_RSTICK_DOWN, UI_BIND_SHAKE,
    // what an action does in the world, in action order up to Minus
    UI_BIND_DESC_A, UI_BIND_DESC_B, UI_BIND_DESC_X, UI_BIND_DESC_Y, UI_BIND_DESC_LCLICK, UI_BIND_DESC_RCLICK,
    UI_BIND_DESC_L, UI_BIND_DESC_R, UI_BIND_DESC_ZL, UI_BIND_DESC_ZR, UI_BIND_DESC_PLUS, UI_BIND_DESC_MINUS,
    UI_BIND_DESC_LEFT, UI_BIND_DESC_UP, UI_BIND_DESC_RIGHT, UI_BIND_DESC_DOWN, UI_BIND_DESC_MOVE, UI_BIND_DESC_CAMERA,
    UI_BIND_DESC_SHAKE,
    // key names, in key id order KEY_SPACE .. KEY_PAGEDOWN (letters, digits, F keys and punctuation show as themselves)
    UI_KEY_SPACE, UI_KEY_TAB, UI_KEY_LSHIFT, UI_KEY_LCTRL, UI_KEY_UP, UI_KEY_DOWN, UI_KEY_LEFT, UI_KEY_RIGHT,
    UI_KEY_MOUSE_L, UI_KEY_MOUSE_R, UI_KEY_MOUSE_M, UI_KEY_MOUSE_X1, UI_KEY_MOUSE_X2,
    UI_KEY_ENTER, UI_KEY_BACKSPACE, UI_KEY_RSHIFT, UI_KEY_RCTRL, UI_KEY_LALT, UI_KEY_CAPSLOCK, UI_KEY_INSERT,
    UI_KEY_DELETE, UI_KEY_HOME, UI_KEY_END, UI_KEY_PAGEUP, UI_KEY_PAGEDOWN,
    UI_KEY_NUM_FMT,           // "Num %s": a numpad key
    // gamepad buttons, in pad id order PAD_SOUTH .. PAD_RPADDLE2, then the PlayStation face labels
    UI_PAD_SOUTH, UI_PAD_EAST, UI_PAD_WEST, UI_PAD_NORTH, UI_PAD_LSTICK, UI_PAD_RSTICK, UI_PAD_L, UI_PAD_R,
    UI_PAD_START, UI_PAD_BACK, UI_PAD_DPAD_LEFT, UI_PAD_DPAD_UP, UI_PAD_DPAD_RIGHT, UI_PAD_DPAD_DOWN, UI_PAD_ZL,
    UI_PAD_ZR, UI_PAD_LPADDLE1, UI_PAD_RPADDLE1, UI_PAD_LPADDLE2, UI_PAD_RPADDLE2,
    UI_PAD_CROSS, UI_PAD_CIRCLE, UI_PAD_SQUARE, UI_PAD_TRIANGLE,

    // Amiibo tab (ui/amiibo_page.cpp): the dump folder and its files, the emulated NFC reader and its state, the
    // amiibo on it and the last action
    UI_MENU_AMIIBO_DIR, UI_MENU_AMIIBO_OPEN, UI_MENU_AMIIBO_DEFAULT_DIR, UI_MENU_AMIIBO_REFRESH, UI_MENU_AMIIBO_PICK,
    UI_MENU_AMIIBO_NO_DIR, UI_MENU_AMIIBO_READER, UI_MENU_AMIIBO_STATE, UI_MENU_AMIIBO_STATE_IDLE,
    UI_MENU_AMIIBO_STATE_SEARCHING, UI_MENU_AMIIBO_STATE_FOUND, UI_MENU_AMIIBO_STATE_MOUNTED,
    UI_MENU_AMIIBO_STATE_OFF, UI_MENU_AMIIBO_FILE, UI_MENU_AMIIBO_NO_FILE, UI_MENU_AMIIBO_TAP, UI_MENU_AMIIBO_REMOVE,
    UI_MENU_AMIIBO_AUTO_REMOVE, UI_MENU_AMIIBO_HOLD, UI_MENU_AMIIBO_SECONDS, UI_MENU_AMIIBO_TAG,
    UI_MENU_AMIIBO_NO_TAG, UI_MENU_AMIIBO_TAG_FILE, UI_MENU_AMIIBO_TAG_ID, UI_MENU_AMIIBO_TAG_UID,
    UI_MENU_AMIIBO_READY, UI_MENU_AMIIBO_TAPPED, UI_MENU_AMIIBO_LOAD_FAILED, UI_MENU_AMIIBO_REMOVED,
    UI_MENU_AMIIBO_STATUS,

    // Menu tab
    UI_MENU_MENU_LANG, UI_MENU_MENU_LANG_DESC, UI_MENU_MENU_LANG_AUTO,
    UI_MENU_DISCORD_RPC, UI_MENU_DISCORD_RPC_DESC,

    // Language names, each in its own language and the same in every row (game language and menu language lists)
    UI_MENU_LANGNAME_EN, UI_MENU_LANGNAME_JA, UI_MENU_LANGNAME_FR, UI_MENU_LANGNAME_DE, UI_MENU_LANGNAME_IT,
    UI_MENU_LANGNAME_ES, UI_MENU_LANGNAME_NL, UI_MENU_LANGNAME_RU, UI_MENU_LANGNAME_ZH_HANS,
    UI_MENU_LANGNAME_ZH_HANT, UI_MENU_LANGNAME_KO, UI_MENU_LANGNAME_PT_BR, UI_MENU_LANGNAME_EN_GB,
    UI_MENU_LANGNAME_FR_CA, UI_MENU_LANGNAME_ES_419, UI_MENU_LANGNAME_PL,

    // Advanced
    UI_MENU_ADV_WARNING,
    UI_MENU_GROUP_SCHED, UI_MENU_GROUP_SHADERS, UI_MENU_GROUP_TEXMEM, UI_MENU_GROUP_FIXES, UI_MENU_GROUP_VULKAN,
    UI_MENU_GROUP_DIAG,
    UI_MENU_ASYNC_PREEMPT, UI_MENU_ASYNC_PREEMPT_DESC, UI_MENU_PREEMPT_OFF, UI_MENU_PREEMPT_ON,
    UI_MENU_PREEMPT_INHERIT,
    UI_MENU_REPLAY_ASYNC, UI_MENU_REPLAY_ASYNC_DESC,
    UI_MENU_TIMER_1MS, UI_MENU_TIMER_1MS_DESC,
    UI_MENU_PIPE_WAIT, UI_MENU_PIPE_WAIT_DESC, UI_MENU_PIPE_WAIT_DEFAULT, UI_MENU_PIPE_WAIT_DRAW,
    UI_MENU_PIPE_SKIP, UI_MENU_PIPE_SKIP_DESC, UI_MENU_PIPE_SKIP_STRICT, UI_MENU_PIPE_SKIP_ANY,
    UI_MENU_PIPE_SKIP_FULLSCREEN,
    UI_MENU_LIB_PREBUILD, UI_MENU_LIB_PREBUILD_DESC,
    UI_MENU_LIB_PREBUILD_THREADS, UI_MENU_LIB_PREBUILD_THREADS_DESC,
    UI_MENU_PIPE_PREBUILD, UI_MENU_PIPE_PREBUILD_DESC,
    UI_MENU_PREWARM_XLATE, UI_MENU_PREWARM_XLATE_DESC,
    UI_MENU_GPL, UI_MENU_GPL_DESC,
    UI_MENU_ASTC, UI_MENU_ASTC_DESC, UI_MENU_ASTC_AUTO, UI_MENU_ASTC_GPU, UI_MENU_ASTC_NATIVE, UI_MENU_ASTC_CPU,
    UI_MENU_MEMORY, UI_MENU_MEMORY_DESC, UI_MENU_MEMORY_AUTO, UI_MENU_MEMORY_IMPORT, UI_MENU_MEMORY_MIRROR,
    UI_MENU_MEMORY_VERIFY,
    UI_MENU_TEX_ALIAS, UI_MENU_TEX_ALIAS_DESC,
    UI_MENU_TEX_VRAM_PRESSURE, UI_MENU_TEX_VRAM_PRESSURE_DESC,
    UI_MENU_ASTC_BC, UI_MENU_ASTC_BC_DESC,
    UI_MENU_LOD_GUARD, UI_MENU_LOD_GUARD_DESC,
    UI_MENU_FIX_C0, UI_MENU_FIX_C0_DESC,
    UI_MENU_FIX_ZERO, UI_MENU_FIX_ZERO_DESC,
    UI_MENU_BIND_SKIP, UI_MENU_BIND_SKIP_DESC,
    UI_MENU_SHADER_MEMO, UI_MENU_SHADER_MEMO_DESC,
    UI_MENU_PIPE_MEMO, UI_MENU_PIPE_MEMO_DESC,
    UI_MENU_TEX_MEMO, UI_MENU_TEX_MEMO_DESC,
    UI_MENU_DESC_TEMPLATE, UI_MENU_DESC_TEMPLATE_DESC,
    UI_MENU_PUSH_SKIP, UI_MENU_PUSH_SKIP_DESC,
    UI_MENU_SPLIT_DRAWS, UI_MENU_SPLIT_DRAWS_DESC,
    UI_MENU_VK_VALIDATION, UI_MENU_VK_VALIDATION_DESC, UI_MENU_VK_VALIDATION_MISSING,
    UI_MENU_VK_DYN_VERTEX, UI_MENU_VK_DYN_VERTEX_DESC, UI_MENU_VK_DYN_A2C, UI_MENU_VK_DYN_A2C_DESC,
    UI_MENU_VK_SUBGROUP_PIN, UI_MENU_VK_SUBGROUP_PIN_DESC,
    UI_MENU_TRACE, UI_MENU_TRACE_DESC,
    UI_MENU_FLIGHT, UI_MENU_FLIGHT_DESC,
    UI_MENU_FLIGHT_SPIKE_MS, UI_MENU_FLIGHT_SPIKE_MS_DESC,
    UI_MENU_PASS_CRUMBS, UI_MENU_PASS_CRUMBS_DESC,
    UI_MENU_INPUT_RECORD, UI_MENU_INPUT_RECORD_DESC,
    UI_MENU_OPEN_LOG, UI_MENU_OPEN_LOG_DESC,
    UI_MENU_COPY_INFO, UI_MENU_COPY_INFO_DESC, UI_MENU_COPY_INFO_DONE,
    UI_MENU_ACTION_FAILED,    // beside a button whose action did not work (no clipboard, no folder)
    UI_MENU_RESET, UI_MENU_RESET_DESC, UI_MENU_RESET_CONFIRM, UI_MENU_RESET_YES, UI_MENU_RESET_NO,

    // Performance overlay
    UI_MENU_OV_FPS, UI_MENU_OV_FRAME_MS, UI_MENU_OV_GAME_PCT, UI_MENU_OV_MAX_MS,
    UI_MENU_OV_FPS_LABEL, UI_MENU_OV_FG_LABEL, UI_PERF_LINE_FPS_FG, UI_MENU_OV_FRAME_LABEL, UI_MENU_OV_FMT_FPS,   // the frame rows: their labels, the rate
    UI_MENU_OV_LOW1, UI_MENU_OV_LOW01,   // printf: the fps of the slowest 1% / 0.1% of the frames, no unit (the overlay column stays narrow)
    UI_MENU_OV_GPU, UI_MENU_OV_CPU, UI_MENU_OV_RAM, UI_MENU_OV_VRAM, UI_MENU_OV_TEMP, UI_MENU_OV_POWER,
    UI_MENU_OV_CLOCK, UI_MENU_OV_NONE,
    UI_MENU_OV_REPLAY, UI_MENU_OV_GPU_TIME, UI_MENU_OV_PIPE_WAIT, UI_MENU_OV_SHADER_WAIT, UI_MENU_OV_TEX_UPLOADS,
    // number formats of the overlay's cells (printf, one number each)
    UI_MENU_OV_FMT_PCT, UI_MENU_OV_FMT_TEMP, UI_MENU_OV_FMT_POWER, UI_MENU_OV_FMT_MHZ, UI_MENU_OV_FMT_GB,
    UI_MENU_OV_FMT_GB_OF, UI_MENU_OV_GAME_GB, UI_MENU_OV_FMT_MS, UI_MENU_OV_PER_FRAME,
    // the picture lines: window, game image and 3D scene sizes, the scaling to the window, frame limit and presentation
    UI_MENU_OV_WINDOW, UI_MENU_OV_OUTPUT, UI_MENU_OV_SCENE, UI_MENU_OV_SCALING, UI_MENU_OV_LIMIT, UI_MENU_OV_PRESENT,
    UI_MENU_OV_FMT_SIZE,      // "%ux%u"
    UI_MENU_OV_FMT_SCENE,     // "%ux%u, %.0f%%": the scene and its share of the game image's width
    UI_MENU_OV_FMT_FSR,       // "AMD FSR 1, sharpness %d%%"
    UI_MENU_OV_FMT_SHRINK,    // the image is larger than the window: "bilinear + FSR sharpening %d%%"
    UI_MENU_OV_SCALE_NONE, UI_MENU_OV_SCALE_BILINEAR,
    UI_MENU_OV_SCALE_DOWN,    // FSR is selected but the image is larger than the window: FSR only enlarges
    // the Cheats tab (cheats.c): the file, the kinds of cheat and why one cannot be switched
    UI_MENU_TAB_CHEATS,
    UI_MENU_CHEAT_RUPEES, UI_MENU_CHEAT_RUPEES_DESC, UI_MENU_CHEAT_RUPEES_UNAVAILABLE,
    UI_MENU_CHEATS_NONE,      // "%s": <build id>.txt
    UI_MENU_CHEATS_MOD,       // "%s": the mod, "%d": its cheats
    UI_MENU_CHEATS_REBUILD, UI_MENU_CHEATS_REBUILD_DESC,   // restart into the rebuild for unbuilt code cheats
    UI_MENU_CHEAT_CODE_DESC, UI_MENU_CHEAT_KEY_DESC /* "%s": the buttons */, UI_MENU_CHEAT_DATA_DESC,
    UI_MENU_CHEAT_MASTER, UI_MENU_CHEAT_MASTER_DESC, UI_MENU_CHEAT_RESTORE, UI_MENU_CHEAT_RESTORE_DESC,
    UI_MENU_CHEAT_REBUILD, UI_MENU_CHEAT_REBUILD_DESC, UI_MENU_CHEAT_UNSUPPORTED, UI_MENU_CHEAT_UNSUPPORTED_DESC,
    // the Game tab's Saves group (ui/menu.cpp rows_saves, save_share.c): export the save into a zip, import a shared
    // one at the next start. UI_MENU_SAVE_EXPORTED to UI_MENU_SAVE_E_UNSAFE are printf formats with at most one "%s"
    // (a path, or the zip's NX2navi version and date)
    UI_MENU_SAVES,
    UI_MENU_SAVE_EXPORT, UI_MENU_SAVE_EXPORT_DESC, UI_MENU_SAVE_EXPORT_BUG, UI_MENU_SAVE_EXPORT_BUG_DESC,
    UI_MENU_SAVE_IMPORT, UI_MENU_SAVE_IMPORT_DESC,
    UI_MENU_SAVE_IMPORT_WAITING,     // the import row's value while an import waits for the next start
    UI_MENU_SAVE_IMPORT_CANCEL, UI_MENU_SAVE_IMPORT_CANCEL_DESC,
    UI_MENU_SAVE_BUSY,               // note on the rows while an export or import runs
    UI_MENU_SAVE_EXPORTED,           // "%s": the zip's path
    UI_MENU_SAVE_IMPORT_READY,       // "%s": "<NX2navi version>, <export date>" of the zip
    UI_MENU_SAVE_IMPORT_CANCELLED,
    UI_MENU_SAVE_IMPORTED,           // "%s": the folder the replaced save was copied to, at this start
    UI_MENU_SAVE_IMPORTED_NEW,       // the same with no save to replace
    UI_MENU_SAVE_E_NO_SAVE, UI_MENU_SAVE_E_READ_SAVE,
    UI_MENU_SAVE_E_WRITE,            // "%s": the folder the zip could not be written into
    UI_MENU_SAVE_E_READ,             // "%s" here and below: the zip
    UI_MENU_SAVE_E_STAGE,
    UI_MENU_SAVE_E_NOT_SAVE, UI_MENU_SAVE_E_OTHER_GAME, UI_MENU_SAVE_E_NEWER, UI_MENU_SAVE_E_DAMAGED,
    UI_MENU_SAVE_E_UNSAFE,
    // the Co-op tab (ui/coop_page.cpp, docs/coop.md)
    UI_MENU_TAB_COOP,
    UI_MENU_COOP_ON, UI_MENU_COOP_ON_DESC,
    UI_MENU_COOP_NAME, UI_MENU_COOP_NAME_DESC, UI_MENU_COOP_AUTO,
    UI_MENU_COOP_SERVER, UI_MENU_COOP_SERVER_DESC, UI_MENU_COOP_NONE,
    UI_MENU_COOP_MAKE, UI_MENU_COOP_MAKE_DESC, UI_MENU_COOP_JOIN, UI_MENU_COOP_JOIN_DESC,
    UI_MENU_COOP_NEED_SERVER,     // under Make and Join while no relay is set
    UI_MENU_COOP_SECRET, UI_MENU_COOP_SECRET_HINT, UI_MENU_COOP_SECRET_BAD, UI_MENU_COOP_PASTE, UI_MENU_COOP_PASTE_EMPTY,
    UI_MENU_COOP_OK, UI_MENU_COOP_STOP, UI_MENU_COOP_STOP_DESC,
    UI_MENU_COOP_CONNECTING, UI_MENU_COOP_RECONNECTING, UI_MENU_COOP_LEFT,   // "%s": the co-op layer's reason (English)
    UI_MENU_COOP_NO_ROOM, UI_MENU_COOP_NOT_STARTED,
    UI_MENU_COOP_PLAYERS, UI_MENU_COOP_YOU, UI_MENU_COOP_MAKER, UI_MENU_COOP_HEARD /* "%u": seconds */,
    UI_MENU_COOP_NOT_HEARD, UI_MENU_COOP_AWAY, UI_MENU_COOP_MEMBER_DESC,
    UI_MENU_COOP_REMOVE, UI_MENU_COOP_REMOVE_DESC, UI_MENU_COOP_REMOVE_CONFIRM /* "%s": the player's name */,
    UI_MENU_COOP_SECRET_DESC, UI_MENU_COOP_SHOW, UI_MENU_COOP_HIDE, UI_MENU_COOP_COPY, UI_MENU_COOP_COPY_DESC,
    UI_MENU_COOP_OFFER, UI_MENU_COOP_OFFER_WHO /* "%s": names */, UI_MENU_COOP_OFFER_NOBODY,
    UI_MENU_COOP_OFFER_SHARED, UI_MENU_COOP_OFFER_POOLED /* "%s": rule names */, UI_MENU_COOP_OFFER_NOTHING,
    UI_MENU_COOP_OFFER_COPY, UI_MENU_COOP_OFFER_TRUST,
    UI_MENU_COOP_ANSWER, UI_MENU_COOP_ANSWER_DESC, UI_MENU_COOP_ACCEPT_CONFIRM, UI_MENU_COOP_ACCEPT, UI_MENU_COOP_NOT_NOW,
    UI_MENU_COOP_ACCEPTED, UI_MENU_COOP_COPY_FAILED,
    UI_MENU_COOP_RULES_ROOM, UI_MENU_COOP_RULES_MINE, UI_MENU_COOP_RULES_UNKNOWN, UI_MENU_COOP_RULES_CHANGE,
    UI_MENU_COOP_RULES_SET_BY, UI_MENU_COOP_RULE_FOLLOWS,
    UI_MENU_COOP_R_PROGRESS, UI_MENU_COOP_R_ITEMS, UI_MENU_COOP_R_SONGS, UI_MENU_COOP_R_QUEST, UI_MENU_COOP_R_KEYS,
    UI_MENU_COOP_R_RUPEES, UI_MENU_COOP_R_AMMO,   // in coop_state.h's category order, as are the descriptions
    UI_MENU_COOP_R_PROGRESS_DESC, UI_MENU_COOP_R_ITEMS_DESC, UI_MENU_COOP_R_SONGS_DESC, UI_MENU_COOP_R_QUEST_DESC,
    UI_MENU_COOP_R_KEYS_DESC, UI_MENU_COOP_R_RUPEES_DESC, UI_MENU_COOP_R_AMMO_DESC,
    UI_MENU_COOP_SHARED, UI_MENU_COOP_OWN, UI_MENU_COOP_POOL,
    UI_MENU_COOP_LEAVE, UI_MENU_COOP_LEAVE_DESC, UI_MENU_COOP_LEAVE_CONFIRM, UI_MENU_COOP_LEAVE_MAKER, UI_MENU_COOP_STAY,
    UI_MENU_COOP_TOAST_JOINED, UI_MENU_COOP_TOAST_LEFT /* "%s": a name */, UI_MENU_COOP_TOAST_OFFER,
    // what the hardened layer adds to the Co-op tab: the relay's states, keys and identification, the maker's key, the
    // three warnings before the first Accept, the pools' contributors and the maker's reset
    UI_MENU_COOP_LOST, UI_MENU_COOP_JOIN_LIMIT,
    UI_MENU_COOP_NOT_IDENTIFIED, UI_MENU_COOP_REMOVED, UI_MENU_COOP_KEY /* "%s": a key's fingerprint */,
    UI_MENU_COOP_OFFER_KEY /* "%s": a key's fingerprint */, UI_MENU_COOP_OFFER_COMPARE,
    UI_MENU_COOP_MY_KEY /* "%s": a key's fingerprint */,
    UI_MENU_COOP_UNIDENTIFIED /* "%d": players */, UI_MENU_COOP_DISAGREE, UI_MENU_COOP_NEW_SAVE,
    UI_MENU_COOP_WARNINGS, UI_MENU_COOP_WARN_SAVE, UI_MENU_COOP_WARN_ROOM, UI_MENU_COOP_WARN_KEY,
    UI_MENU_COOP_POOLS, UI_MENU_COOP_SPENT /* "%lld": keys, rupees, ammunition */, UI_MENU_COOP_NOT_IN_ROOM,
    UI_MENU_COOP_SPENT_NONE, UI_MENU_COOP_SPENT_DESC,
    UI_MENU_COOP_RESET, UI_MENU_COOP_RESET_DESC, UI_MENU_COOP_RESET_CONFIRM,
    UI_MENU_COOP_REMOVE_KEY_CONFIRM /* "%s": the player's name, "%s": the key's fingerprint */, UI_MENU_COOP_NOT_ACCEPTED,
    // a key the player accepted, now signed under another name: a new offer (CoopOfferMember.accepted_as)
    UI_MENU_COOP_ACCEPTED_AS /* "%s": the name it was accepted under */,
    UI_MENU_LAST_ID = UI_MENU_COOP_ACCEPTED_AS,
    // the boot screen while the boot prewarm prepares shaders and pipelines (boot.c, ui/menu.cpp menu_boot_frame)
    UI_BOOT_TITLE, UI_BOOT_STARTING,
    UI_BOOT_SHADERS, UI_BOOT_PIPELINES,   // printf: "%lld" done, "%lld" total
    // the settings menu's Picture rows for this game (ui/menu.cpp tab_screen): one upscaling choice (bilinear, FSR 1
    // or a DLSS mode) and the picture's resolution
    UI_MENU_UPSCALING, UI_MENU_UPSCALING_DESC, UI_MENU_FRAMEGEN, UI_MENU_FRAMEGEN_DESC,
    UI_MENU_FRAMEGEN_OFF, UI_MENU_FRAMEGEN_NOTE, UI_MENU_FRAMEGEN_NEEDS, UI_MENU_HDR_TONEMAPPED, UI_MENU_HDR_NATIVE,
    UI_MENU_GAME_RES, UI_MENU_GAME_RES_DESC, UI_MENU_GAME_RES_NATIVE,
    // the same rows' texts on a GPU DLSS does not run on (not NVIDIA): the menu leaves DLSS out there
    UI_MENU_UPSCALING_DESC_FSR, UI_MENU_FRAMEGEN_DESC_FSR, UI_MENU_FRAMEGEN_NOTE_FSR,
    UI_MENU_FRAMEGEN_NEEDS_FSR,
    // and on a build where FSR 3.1 does not load (vk_ffx_available): the menu leaves it out there, and these texts say so
    UI_MENU_UPSCALING_DESC_NO_FSR,
    UI_MENU_FSR3_OFF,   // FSR 3.1 is still set but not loadable: "in use: off" and why
    UI_MENU_FRAMEGEN_DESC_DLSS, UI_MENU_FRAMEGEN_NOTE_DLSS, UI_MENU_FRAMEGEN_NEEDS_DLSS,   // DLSS only
    UI_MENU_FRAMEGEN_DESC_NONE, UI_MENU_FRAMEGEN_NEEDS_NONE,                               // neither runs
    // DLSS frame generation (ui/menu.cpp row_framegen, row_framegen_mult): the multiplier row, its notes when this GPU
    // and driver allow less than 4x, and the frame generation row's notes when DLSS's does not run
    UI_MENU_FRAMEGEN_MULT, UI_MENU_FRAMEGEN_MULT_DESC,
    // the multiplier row where DLSS's frame generation runs through NGX itself (ui/menu.cpp row_framegen_mult_ngx)
    UI_MENU_FG_MULT_DESC, UI_MENU_FG_MULT_DYNAMIC,
    UI_MENU_FRAMEGEN_MULT_RTX50,     // this GPU and driver do 2x only
    UI_MENU_FRAMEGEN_MULT_UPTO,      // printf: "%d", the highest multiplier this GPU and driver allow
    UI_MENU_FRAMEGEN_MULT_OLD_DLL,   // nx2navi_dlssg.dll is older than multi frame generation
    UI_MENU_FRAMEGEN_UNAVAILABLE, UI_MENU_FRAMEGEN_OFF_FILES,
    UI_TEXT_COUNT
} UiTextId;

// Bytes of the menu's description pane (ui/option_rows.cpp, struct Desc): a description plus the note appended to it
// ("after restart", "set by environment variable", a disabled row's note). UTF-8 Cyrillic takes two bytes a letter, so
// the longest Russian description is about 850 bytes; the probe checks every menu text leaves room for a note.
enum { UI_DESC_BYTES = 2048 };

// UTF-8 text for id in the language of ov_language(); English when the catalog has no row for that language.
const char *ui_text(UiTextId id);
// The same for an explicit catalog language ("en", "ja", "fr", "de", "it", "es", "nl", "pt", "ru", "zh-Hans",
// "zh-Hant", "ko", "pt-BR"); an empty entry gives the English text.
const char *ui_text_in(const char *lang, UiTextId id);
// Calls visit(ctx, text) for every string the row of lang has its own text for (an id that falls back to English is not
// visited): the font builder covers the glyphs of the text a language shows (ui/menu.cpp).
void ui_text_visit(const char *lang, void (*visit)(void *ctx, const char *text), void *ctx);
// 1 when the row of lang has its own text for id (no fallback to English), for the probe.
int ui_text_defined(const char *lang, UiTextId id);
// The catalog language for a Switch language code ("en-GB" -> "en", "zh-CN" -> "zh-Hans", ...).
const char *ui_text_lang(const char *switch_code);

// The game languages the F1 menu offers for a game build (rt_game_version): one entry per Mals language file the build
// ships, each with the Switch code NX2NAVI_LANG takes and the id of its name. 1.4.3 adds Brazilian Portuguese (USpt).
// Fills out (room for max entries) and returns the count.
typedef struct { const char *code; UiTextId name; } UiGameLang;
int ui_text_game_langs(int game_version, UiGameLang *out, int max);
// The same list from the Mals files the RomFS ships (Ocarina: Polish and Brazilian Portuguese, no Portugal Portuguese):
// regions holds the four-letter prefix of each Mals/<region>.Product... file ("USen", "EUpl"); the version gates are
// not used. Returns the count.
int ui_text_game_langs_in(const char *const *regions, int nregions, UiGameLang *out, int max);

// Menu text: ui_text_in(menu language, id). The menu language is NX2NAVI_MENU_LANG ("en", "ru", "pt-BR", "zh-Hans");
// unset, the system UI language when the catalog has menu text for it (English, Russian, Brazilian Portuguese,
// Simplified Chinese), else English. Follows a live change of the setting.
const char *menu_text(UiTextId id);
