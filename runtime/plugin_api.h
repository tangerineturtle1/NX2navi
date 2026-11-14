// NX2navi plugin interface, version 7: what a mod's native library (a DLL loaded into the game's process) can ask of the
// game. docs/plugins.md is the guide; runtime/plugins.c is the implementation.
//
// Copyright (c) 2026 chrg6.
//
// This one file is an exception to LICENSE-ADDITIONS. Permission is granted, free of charge, to any person to copy
// this file, unchanged or changed, into a mod for NX2navi, to compile it into that mod and to distribute it with that
// mod in source or binary form under the mod's own license, provided this notice stays in every copy of the file.
// The permission covers this file alone and nothing else of the repository it comes from.
// THE FILE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY ARISING FROM THE FILE OR ITS USE.
//
// Getting the interface, either way:
//   - export `void nx2navi_plugin_init(const Nx2PluginApi *api)` from the library. The game calls it once, on the
//     process's first thread, after the game's modules are in memory and before any of the game's code runs, for every
//     library named in the environment variable NX2NAVI_MOD_DLLS (the launcher sets it for the DLLs of the enabled mods);
//   - or ask the executable, at any time and from any thread:
//       Nx2PluginApiFn get = (Nx2PluginApiFn)GetProcAddress(GetModuleHandleW(NULL), NX2_PLUGIN_API_EXPORT);
//       const Nx2PluginApi *api = get ? get(NX2_PLUGIN_API_VERSION) : NULL;    // NULL: a game without the interface
//
// Compatibility: every structure starts with its size in bytes as the game built it. A later game only adds members at
// the end of a structure, so a member is there when `size` covers it (NX2_HAS). nx2navi_plugin_api(v) returns NULL when
// the game cannot serve version v (v is newer than the game, or older than the oldest it still serves). All functions
// use the platform's C calling convention; text is UTF-8.
//
// Version 2 added, all at the end of what version 1 had: function hooks (hook_function, call_original), map_range,
// on_exit, title menu buttons (add_title_button), the game's buttons as events (on_pad), the stack arguments of
// Nx2GuestRegs and the folders of Nx2GameInfo. Version 3 added a menu page in the game's own style (open_menu,
// update_menu, close_menu), and a title menu button's on_select may now run inside a hook of the game's thread instead
// of with the frame callbacks. Version 4 added the scene's camera (camera, world_to_screen) and what the menu page was
// missing (a row enabled again, a page opened from another page, the cursor's row, a text row the player types into),
// the keyboard mark of pad events and restart. Version 5 added the follow camera's reset (camera_reset), the save
// folder changed on the title screen (set_save_dir), the caller's frame pointer in a hook (Nx2HookCall.fp), the game's
// language and the account's name (Nx2GameInfo), a text row typed into with a gamepad, and a menu page whose cursor
// goes where focus_row says. Version 6 added pictures and fonts from files for the overlay (texture_create,
// texture_load, texture_free, font_load, font_free, and Nx2Draw's image, text_font and text_font_size). Version 7 added
// Steam for a mod's own multiplayer (steam: rooms, invites and datagrams between the games of a room). A plugin built
// against an earlier version runs unchanged.
//
// Threads, in short (docs/plugins.md has the rules): frame callbacks run on the game's thread between two frames; hook
// callbacks run on whichever thread of the game calls the hooked function; guest_call works in those two and nowhere
// else; overlay callbacks run on the thread that presents; key and text callbacks run on the window's thread; Steam events
// (version 7) run on the runtime's Steam thread. A library that registered a callback must stay loaded (the game pins
// it).
#ifndef NX2NAVI_PLUGIN_API_H
#define NX2NAVI_PLUGIN_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NX2_PLUGIN_API_VERSION 7u
#define NX2_PLUGIN_API_EXPORT "nx2navi_plugin_api"     // exported by the game's executable
#define NX2_PLUGIN_INIT_EXPORT "nx2navi_plugin_init"   // optionally exported by a plugin

// 1 when the structure *p of the given type, whose first member is its size, holds the member m:
// NX2_HAS(api, Nx2PluginApi, capture_input).
#define NX2_HAS(p, type, m) ((p)->size >= offsetof(type, m) + sizeof(((const type *)0)->m))

// A colour for the drawing calls: red in the low byte, then green, blue and alpha (255 opaque).
#define NX2_RGBA(r, g, b, a) \
    ((uint32_t)(uint8_t)(r) | (uint32_t)(uint8_t)(g) << 8 | (uint32_t)(uint8_t)(b) << 16 | (uint32_t)(uint8_t)(a) << 24)

// What the game is. The pointers stay valid for the life of the process.
typedef struct Nx2GameInfo {
    uint32_t size;
    uint32_t game_version;        // 111 for game 1.1.1
    const char *runtime_version;  // NX2navi's own version, "0.9.0"
    const char *game_dir;         // the folder of the game's executable, without a trailing separator
    const char *mods_dir;         // the mods folder the game reads
    const char *build_id;         // hex of the build id of the game's main module (64 digits); "" when unknown
    uint64_t main_base;           // where main is mapped: a guest address is an address in this process
    uint64_t main_size;
    // ---- version 2
    const char *profile_dir;      // the profile folder the game keeps its saves, cache and account in
    const char *save_dir;         // the folder of the committed save, <profile_dir>/save/<account>; "" until the game has
                                  // opened its storage (some seconds after start): look again later. After
                                  // set_save_dir (version 5) the pointer itself changes: read it from the structure
                                  // each time, do not keep it
    // ---- version 5: check with NX2_HAS(info, Nx2GameInfo, account)
    const char *language;         // the language the game runs in, as the console's language code: "en-US", "en-GB", "fr",
                                  // "fr-CA", "de", "it", "es", "es-419", "ja", "ko", "nl", "pt", "pt-BR", "ru", "pl",
                                  // "zh-Hans", "zh-Hant". It does not change while the game runs.
    const char *account;          // the name of the account's folder under save/ (32 hex digits), known before the game
                                  // has opened its storage: save_dir will be <profile_dir or the save folder>/save/<account>
} Nx2GameInfo;

// One frame callback's view of the frame.
typedef struct Nx2Frame {
    uint32_t size;
    uint32_t reserved;
    uint64_t index;               // frames the game presented before this one
} Nx2Frame;

// Registers for guest_call_regs, in and out: x0-x7 are the integer arguments and x0, x1 the results; x8 is the address
// of a result returned in memory; v0-v7 are the floating-point and vector arguments and results, each as two 64-bit
// halves, low first (a float is the low 32 bits of the low half, a double the low half).
// Version 2: stack points at nstack 64-bit words (at most NX2_STACK_ARGS_MAX), the arguments the callee takes on the
// stack, the first at the lowest address; they are copied below the thread's stack pointer for the call. A block whose
// size stops before them (a version 1 plugin's) passes none.
#define NX2_STACK_ARGS_MAX 64
typedef struct Nx2GuestRegs {
    uint32_t size;
    uint32_t reserved;
    uint64_t x[9];
    uint64_t v[8][2];
    // ---- version 2
    const uint64_t *stack;
    uint64_t nstack;
} Nx2GuestRegs;

// One call of a hooked function of the game, as its hook callback gets it: on the thread that makes the call, before
// the function runs. x and v are the argument registers as the caller set them (as in Nx2GuestRegs); the callback may
// change them. What becomes of the function:
//   - nothing else done: it runs when the callback returns, with x and v as the callback left them;
//   - call_original(call): it runs now, with x and v as they are, and x and v then hold what it returned (x[0], x[1],
//     v[0..]); the callback goes on, may change those results, and the caller gets them when the callback returns;
//   - skip = 1 and no call_original: it does not run, and its caller gets x[0], x[1] and v[0..] as the callback left
//     them.
// The structure is valid only inside the callback that got it.
typedef struct Nx2HookCall {
    uint32_t size;
    uint32_t skip;                // set to 1 to leave the function out
    uint64_t addr;                // the hooked function's guest address
    uint64_t x[9];                // x0-x7, and x8 (the address of a result returned in memory)
    uint64_t v[8][2];             // v0-v7, two 64-bit halves each, low first
    uint64_t sp;                  // the stack pointer at the call: arguments past the registers are the 64-bit words at
                                  // sp, sp + 8, ... (guest memory: read or change them there). Read only.
    uint64_t lr;                  // the guest address the function returns to: who called. Read only.
    // ---- version 5: check with NX2_HAS(call, Nx2HookCall, fp)
    uint64_t fp;                  // the caller's frame pointer (x29) at the call, for a backtrace: the 64-bit word at fp is
                                  // the frame pointer of the caller's caller and the word at fp + 8 the address the caller
                                  // returns to; follow the chain with read() until fp is 0 or does not read. Read only.
} Nx2HookCall;

// The drawing calls of one overlay callback. Coordinates are pixels of the window's picture, the origin at its top
// left corner. The structure and its functions are valid only inside the callback that got them, on that thread.
typedef struct Nx2Draw {
    uint32_t size;
    uint32_t menu_open;           // 1 while the game's own settings menu is open (the drawing then shows under it)
    float width, height;          // the picture the overlay covers
    float scale;                  // 1 at a height of 1080 pixels; multiply sizes by it to keep their share of the picture
    float text_height;            // a readable text size in pixels at this scale (the size of the game's own overlay text)
    uint64_t frame;               // as Nx2Frame.index, for the frame under this overlay
    void (*text)(float x, float y, float size, uint32_t rgba, const char *utf8);    // top left corner; '\n' starts a line
    void (*text_size)(float size, const char *utf8, float out_wh[2]);
    void (*rect)(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness);   // an outline
    void (*rect_filled)(float x0, float y0, float x1, float y1, uint32_t rgba, float rounding);
    void (*line)(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness);
    // ---- version 3: check with NX2_HAS(draw, Nx2Draw, picture_height)
    // Where the game's own picture lies in the overlay's, in the same pixels: its top left corner and its size. It is
    // the whole overlay unless the window's shape differs from the picture's (bars at the sides or above and below).
    // A point of the game's picture at the fractions (u, v) is at (picture_x + u * picture_width, picture_y + v *
    // picture_height): what a marker on the game's map or over a character is placed with.
    float picture_x, picture_y, picture_width, picture_height;
    // ---- version 6: check with NX2_HAS(draw, Nx2Draw, text_font_size)
    // A picture (texture_create, texture_load) over the rectangle (x0, y0)-(x1, y1), in the same pixels as rect_filled.
    // (u0, v0) and (u1, v1) are the corners of the part of the picture to show, as fractions of it: 0, 0 is its top
    // left corner and 1, 1 its bottom right one, so 0, 0, 1, 1 shows all of it and a part given in the picture's own
    // pixels is px / width, py / height. Swapped corners mirror it. Every pixel is multiplied by rgba: NX2_RGBA(255,
    // 255, 255, 255) leaves the picture as it is, a lower alpha lets the game show through, a colour tints it.
    // Nothing is drawn for a handle that is not a picture's.
    void (*image)(int texture, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                  uint32_t rgba);
    // text and text_size with a font of the plugin's own (font_load). size is the height in pixels; 0 is the size
    // the font's letters were made at (its pixel_size * scale, or its pixel_size alone with NX2_FONT_EXACT), the one
    // size at which they are drawn as they were made and not stretched. shadow_rgba other than 0 draws the text once more under it in that
    // colour, a little down and to the right (NX2_RGBA(0, 0, 0, 160) is a soft dark one). A handle that is not a
    // font's, or a font loaded since the last picture, draws and measures with the overlay's own font.
    void (*text_font)(int font, float x, float y, float size, uint32_t rgba, uint32_t shadow_rgba, const char *utf8);
    void (*text_font_size)(int font, float size, const char *utf8, float out_wh[2]);
} Nx2Draw;

// ---- version 6: flags of texture_create and texture_load, and of font_load
enum {
    NX2_TEXTURE_PIXELS = 1,       // pixel art: shown larger, the picture's pixels stay squares with hard edges, and no
                                  // smaller copies are made of it. Without it a picture is smoothed when stretched and
                                  // keeps smaller copies of itself, so that it stays clean when drawn much smaller.
};
enum {
    NX2_FONT_EXACT = 1,           // pixel_size is in the window's own pixels: the letters are made at that size whatever
                                  // the window's size (see font_load)
};

// A key going down or up. key is an NX2_KEY_* value.
typedef struct Nx2KeyEvent {
    uint32_t size;
    uint32_t key;
    uint8_t down;                 // 1 pressed, 0 released
    uint8_t repeat;               // 1 for the repeats of a held key
    uint8_t reserved[6];
} Nx2KeyEvent;

// Key codes: letters and digits are their upper-case ASCII ('A', '7'); the rest are the values below.
enum {
    NX2_KEY_BACKSPACE = 0x08, NX2_KEY_TAB = 0x09, NX2_KEY_ENTER = 0x0D, NX2_KEY_SHIFT = 0x10, NX2_KEY_CTRL = 0x11,
    NX2_KEY_ALT = 0x12, NX2_KEY_ESCAPE = 0x1B, NX2_KEY_SPACE = 0x20, NX2_KEY_PAGE_UP = 0x21, NX2_KEY_PAGE_DOWN = 0x22,
    NX2_KEY_END = 0x23, NX2_KEY_HOME = 0x24, NX2_KEY_LEFT = 0x25, NX2_KEY_UP = 0x26, NX2_KEY_RIGHT = 0x27,
    NX2_KEY_DOWN = 0x28, NX2_KEY_INSERT = 0x2D, NX2_KEY_DELETE = 0x2E,
    NX2_KEY_F1 = 0x70,            // F2 to F12 follow: NX2_KEY_F1 + n - 1
    NX2_KEY_F12 = 0x7B,
};

typedef void (*Nx2FrameFn)(const Nx2Frame *frame, void *user);
typedef void (*Nx2OverlayFn)(const Nx2Draw *draw, void *user);
typedef int (*Nx2KeyFn)(const Nx2KeyEvent *event, void *user);   // return 1 to keep the key from the game's own menu keys
typedef void (*Nx2TextFn)(uint32_t codepoint, void *user);       // one typed character, a Unicode code point
typedef void (*Nx2HookFn)(Nx2HookCall *call, void *user);        // version 2: one call of a hooked function
typedef void (*Nx2ExitFn)(void *user);                           // version 2: the game is about to end
typedef void (*Nx2TitleFn)(void *user);                          // version 2: a title menu button was chosen

// A button for add_title_button: one more entry under the game's own on its title menu. The texts are copied.
typedef struct Nx2TitleButton {
    uint32_t size;
    int32_t order;                // buttons are listed by ascending order, equal ones in the order they were added
    const char *label;            // UTF-8, at most 95 bytes; in the player's language if the plugin has it
    const char *description;      // NULL, or a few words shown smaller after the label (at most 159 bytes)
    Nx2TitleFn on_select;         // called when the player chooses the button
    void *user;
} Nx2TitleButton;

// A button of the game's controls going down or up, for on_pad. button is an NX2_PAD_* value. It is what the game
// would read: the gamepad, and the keys the player bound to its buttons. The left stick pushed past half way counts
// as the direction button of that side, so a menu needs the four directions only.
typedef struct Nx2PadEvent {
    uint32_t size;
    uint32_t button;
    uint8_t down;                 // 1 pressed, 0 released
    uint8_t keyboard;             // version 4: 1 when no gamepad holds the button: the press comes from a key or mouse
                                  // button the player bound to it (the same key also arrives in on_key). 0 in older games.
    uint8_t reserved[6];
} Nx2PadEvent;
enum {                            // the buttons by the names the game's own prompts use
    NX2_PAD_A, NX2_PAD_B, NX2_PAD_X, NX2_PAD_Y, NX2_PAD_STICK_L, NX2_PAD_STICK_R, NX2_PAD_L, NX2_PAD_R, NX2_PAD_ZL,
    NX2_PAD_ZR, NX2_PAD_PLUS, NX2_PAD_MINUS, NX2_PAD_LEFT, NX2_PAD_UP, NX2_PAD_RIGHT, NX2_PAD_DOWN,
};
typedef void (*Nx2PadFn)(const Nx2PadEvent *event, void *user);  // version 2

// ---- version 3: a menu page in the game's own style (open_menu). The page is the game's settings screen with the
// plugin's rows: the game's parts, cursor, sounds and transitions. Every text is UTF-8 and is copied.
enum {
    NX2_ROW_ACTION,               // a button: `label` over it, `value` on it (may be NULL); choosing it is an event
    NX2_ROW_TOGGLE,               // on / off: `current` 1 or 0
    NX2_ROW_SELECT,               // one of `choices`, `current` its index; left, right or confirm change it
    NX2_ROW_TEXT,                 // a text the player can change: shown as a button with `value` on it. With text_max 0
                                  // choosing it is an event, on which the plugin takes the text itself (capture_input,
                                  // on_text) and calls update_menu; with text_max above 0 (version 4) the game takes it
                                  // (Nx2MenuRow.text_max) and the event is NX2_MENU_TEXT
    NX2_ROW_SEPARATOR,            // the game's ornament line between two groups of rows; no texts
};
typedef struct Nx2MenuRow {
    uint32_t size;
    uint32_t kind;                // NX2_ROW_*
    const char *label;
    const char *description;      // shown under the list while the cursor is on the row; for a disabled row, say why
    const char *value;            // NX2_ROW_ACTION, NX2_ROW_TEXT
    const char *const *choices;   // NX2_ROW_SELECT: choice_count texts, at most 16
    uint32_t choice_count;
    int32_t current;              // NX2_ROW_TOGGLE, NX2_ROW_SELECT
    uint32_t disabled;            // 1: shown as unavailable; nothing can be done on it, and the cursor passes over it
                                  // (since version 5 over every kind of row; before, only over an action row)
    uint32_t text_max;            // NX2_ROW_TEXT, version 4 (0 in a version 3 plugin: the field was reserved): above 0, the
                                  // player types into the row. Choosing it starts the entry with `value` on the button and
                                  // a cursor mark after it; typed characters and Backspace change it, up to text_max
                                  // bytes of UTF-8 (at most 255); Enter ends it with NX2_MENU_TEXT, Escape puts the old
                                  // text back. The game gets no input meanwhile. Version 5: a gamepad types too. The row
                                  // shows the character being chosen in brackets after the text; up and down go through
                                  // the characters of a group, X goes to the next group (capitals, small letters, digits
                                  // and marks), right takes the character, left or Y takes the last one away, A or +
                                  // ends the entry, B puts the old text back. (In version 4 a gamepad could only end
                                  // the entry with A or leave it with B.)
} Nx2MenuRow;
enum {
    NX2_MENU_ACTIVATE,            // an action or text row was chosen
    NX2_MENU_CHANGE,              // a toggle or select row changed: value is the new state or index
    NX2_MENU_FOCUS,               // the cursor came to a row
    NX2_MENU_CLOSED,              // the page is gone (the player went back, or close_menu)
    NX2_MENU_TEXT,                // version 4: the player ended the entry of a text row (text_max): `text` is the new text,
                                  // value its length in bytes. The row shows it already.
};
typedef struct Nx2MenuEvent {
    uint32_t size;
    uint32_t kind;                // NX2_MENU_*
    uint32_t row;                 // index into the rows given
    int32_t value;
    // ---- version 4: check with NX2_HAS(event, Nx2MenuEvent, text)
    const char *text;             // NX2_MENU_TEXT: the text, valid during the callback; NULL in every other event
} Nx2MenuEvent;
typedef void (*Nx2MenuFn)(const Nx2MenuEvent *event, void *user);
typedef struct Nx2Menu {
    uint32_t size;
    uint32_t row_count;           // 1 to 48, at least one of them an action or text row
    const char *title;
    const Nx2MenuRow *rows;       // row_count rows, each rows[0].size bytes
    Nx2MenuFn on_event;           // called on the game's thread (guest_call works in it); may be NULL
    void *user;
    // ---- version 4 (a version 3 plugin's structure ends above)
    uint32_t focus_row;           // 1 + the row the cursor goes to, when the page opens and at update_menu (0: no wish:
                                  // the first row that can be used when the page opens, the row it is on at update_menu).
                                  // Followed since version 5 (a version 4 game takes the wish and leaves the cursor):
                                  // at the opening the cursor starts there; after update_menu it glides there within
                                  // about half a second. A disabled row is not taken.
    uint32_t reserved;
} Nx2Menu;

// ---- version 4: the camera of the game's main view, as the game's camera update left it for the frame it is working
// on (one frame ahead of the picture in the window). Set size before the call; only that many bytes are written.
// Vectors and matrices are in the game's world: metres, y up. A matrix is 16 floats, row by row, for column vectors:
// clip = projection * view * (x, y, z, 1). view takes a world point into the camera's space (x right, y up, the camera
// looks along -z); projection takes that to clip space, where x / w and y / w run from -1 to 1 across the game's
// picture (y up) and z / w from -1 at the near plane to 1 at the far plane.
typedef struct Nx2Camera {
    uint32_t size;
    uint32_t reserved;
    float position[3];            // where the camera is
    float target[3];              // the point it looks at
    float up[3];                  // its up direction, unit length, at right angles to the viewing direction
    float near_plane, far_plane;
    float fov_y;                  // the vertical angle of view, radians
    float aspect;                 // width / height of the view (the picture's, also when it is not 16:9)
    float view[16];
    float projection[16];
} Nx2Camera;

// ---- version 7: Steam for a mod's own multiplayer (Nx2PluginApi.steam; docs/plugins.md "Steam"). The game signs in to
// Steam as Steam's public test app (480), as the game's own co-op does, with the Steam library the launcher puts
// beside the game; rooms are friends-only Steam lobbies, and the games of a room send each other datagrams through
// Steam's network: no port to forward, no address to type. Every text is UTF-8.
enum {                            // Nx2SteamStatus.state
    NX2_STEAM_OFF,                // start was not called, or stop was
    NX2_STEAM_STARTING,           // start was called; Steam has not answered yet
    NX2_STEAM_READY,              // signed in: rooms can be made and joined
    NX2_STEAM_UNAVAILABLE,        // Steam cannot be used now: `why` and `says` tell; tried again every few seconds
};
enum {                            // Nx2SteamStatus.why
    NX2_STEAM_WHY_NONE,
    NX2_STEAM_WHY_NO_LIBRARY,     // steam_api64.dll is not beside the game (the launcher copies it there from a Steam
                                  // game the player has installed), or does not load
    NX2_STEAM_WHY_OLD_LIBRARY,    // the library beside the game lacks a call this needs (it is from before 2021)
    NX2_STEAM_WHY_NO_CLIENT,      // Steam is not running, or not signed in
    NX2_STEAM_WHY_GAME_COOP,      // the game's own co-op is on (coop_steam = 1) and has Steam: one of the two at a time
    NX2_STEAM_WHY_IN_USE,         // another plugin has it
    NX2_STEAM_WHY_OTHER,
};
enum {                            // Nx2SteamEvent.kind
    NX2_STEAM_EV_STATE = 1,       // Nx2SteamStatus.state changed (ready, unavailable): ask status
    NX2_STEAM_EV_ROOM_ENTERED,    // a: the room. This game made it or came into it; members and status know it
    NX2_STEAM_EV_ROOM_FAILED,     // code: NX2_STEAM_FAIL_*. The room was not made or not entered; this game is in none
    NX2_STEAM_EV_MEMBER_JOINED,   // a: the player's id
    NX2_STEAM_EV_MEMBER_LEFT,     // a: the player's id
    NX2_STEAM_EV_OWNER_CHANGED,   // a: the new owner's id (the owner left and Steam handed the room to a member)
    NX2_STEAM_EV_INVITED,         // a: the room, b: the friend who invites this player (room_code gives its text)
    NX2_STEAM_EV_JOIN_ASKED,      // a: the room. The player accepted an invite, or chose "Join game", in Steam itself
    NX2_STEAM_EV_MESSAGE,         // a: the sender (a member of the room), data and n: the datagram, valid in the callback
    NX2_STEAM_EV_LOST,            // Steam closed or lost its connection: the room is gone, this game is in none
};
enum {                            // Nx2SteamEvent.code of NX2_STEAM_EV_ROOM_FAILED
    NX2_STEAM_FAIL_CREATE = 1,    // Steam did not make the room
    NX2_STEAM_FAIL_NO_ROOM,       // no room has that id (any more)
    NX2_STEAM_FAIL_FULL,
    NX2_STEAM_FAIL_REFUSED,       // Steam does not let this player in
    NX2_STEAM_FAIL_OTHER,
    NX2_STEAM_FAIL_NOT_OURS,      // the lobby is not one of this plugin's (another tag, or another program's): left at once
};
#define NX2_STEAM_NAME_CAP 64     // a player's name with its terminator
#define NX2_STEAM_CODE_CAP 16     // a room id's text with its terminator
#define NX2_STEAM_MESSAGE_MAX 1200   // the most bytes of one datagram (send)
#define NX2_STEAM_ROOM_MAX 8      // the most players of a room
typedef struct Nx2SteamEvent {
    uint32_t size;
    uint32_t kind;                // NX2_STEAM_EV_*
    uint64_t a, b;
    int32_t code;
    uint32_t n;                   // NX2_STEAM_EV_MESSAGE: the datagram's bytes
    const void *data;
} Nx2SteamEvent;
// Called on the runtime's Steam thread (never the game's: guest_call does not work in it), one event at a time. It may
// call every function of Nx2Steam; it should return soon, since no other event and no datagram is handed out
// meanwhile.
typedef void (*Nx2SteamFn)(const Nx2SteamEvent *event, void *user);
typedef struct Nx2SteamStatus {
    uint32_t size;                // set it before the call; only that many bytes are written
    uint32_t state;               // NX2_STEAM_*
    uint32_t why;                 // NX2_STEAM_WHY_*, while state is NX2_STEAM_UNAVAILABLE (and after a refused start)
    uint32_t local;               // 1: the stand-in for tests is in use, not Steam (docs/plugins.md)
    char says[200];               // the reason as an English sentence, for a log; "" when there is none
    uint64_t self;                // this player's Steam id; 0 until ready
    char name[NX2_STEAM_NAME_CAP];   // this player's Steam name
    uint64_t room;                // the room this game is in, 0 none
    uint64_t owner;               // its owner's id: the player whose game made it, or the one Steam handed it to
    char room_code[NX2_STEAM_CODE_CAP];   // the room's id as players pass it on, "XXXX-XXXX"; "" in no room
    uint32_t entering;            // 1 from room_create or room_join until its ENTERED or FAILED event
    uint32_t members;             // players in the room, this one included
} Nx2SteamStatus;
typedef struct Nx2SteamMember {   // fixed: 80 bytes
    uint64_t id;
    char name[NX2_STEAM_NAME_CAP];   // "" while Steam has not got the name yet
    uint8_t self, owner;
    uint8_t reserved[6];
} Nx2SteamMember;
typedef struct Nx2SteamFriend {   // fixed: 80 bytes
    uint64_t id;
    char name[NX2_STEAM_NAME_CAP];
    uint8_t online;               // signed in to Steam now
    uint8_t in_app;               // shown as playing app 480: probably in this game with Steam on, so that an invite
                                  // reaches the game itself and not only Steam's own window
    uint8_t reserved[6];
} Nx2SteamFriend;
// The functions may be called from any thread. Those that ask something of Steam (start, room_create, room_join,
// room_leave, invite, send) only queue it for the runtime's Steam thread and return at once; what came of it arrives
// as an event.
typedef struct Nx2Steam {
    uint32_t size;
    uint32_t reserved;
    // Signs in to Steam for this plugin and starts the events. tag names the plugin's rooms, 1 to 31 characters of
    // a-z, 0-9, '-', '_' and '.': a room made carries it, and a lobby entered that carries another tag, or none (the
    // game's own co-op's, another program's), is left at once (NX2_STEAM_FAIL_NOT_OURS). fn(event, user) gets what
    // happens. Returns 1 when the sign-in is on its way: NX2_STEAM_EV_STATE follows, with state READY, or UNAVAILABLE
    // and the reason; an unavailable Steam is tried again every few seconds until stop. Returns 0, with status().why
    // saying why, when it cannot start at all: the game's own co-op is on, another plugin has Steam, a tag out of
    // the rule, fn NULL. Steam shows the player as playing "Spacewar" from the sign-in until stop.
    int (*start)(const char *tag, Nx2SteamFn fn, void *user);
    // Leaves the room and signs out; no event comes after it returned, unless it was called inside the callback
    // (the one being handled is then the last). Also done by the runtime when the game ends.
    void (*stop)(void);
    // Where Steam stands, a copy made under a lock. 1 when *out was filled.
    int (*status)(Nx2SteamStatus *out);
    // Makes a room for up to max_players (2 to NX2_STEAM_ROOM_MAX): a friends-only lobby, which the player's friends
    // may see and join from Steam and anyone else joins by its id or an invite. 1 when asked: ROOM_ENTERED or
    // ROOM_FAILED follows. 0: not ready, or already in a room or on the way into one.
    int (*room_create)(int max_players);
    // Joins the room a room id names (any case, with or without the dash and spaces). 1 when asked, as room_create; 0
    // also when the text is no room id (nothing is asked of Steam).
    int (*room_join)(const char *code);
    void (*room_leave)(void);     // leaves the room, or gives up entering one; no event follows
    // A room's number as the text players pass on ("XXXX-XXXX", the game's own co-op's format), for the room of an
    // invite. 1 when out was written; 0 for a number that is no room's.
    int (*room_code)(uint64_t room, char out[NX2_STEAM_CODE_CAP]);
    // The players in the room, the owner first, then in Steam's order; at most max are written. Returns how many.
    int (*members)(Nx2SteamMember *out, int max);
    // The player's friends as of a moment ago (looked up every few seconds while ready): those in the app first, then
    // those online, then the rest, each group by name. Returns how many were written.
    int (*friends)(Nx2SteamFriend *out, int max);
    // Steam shows that friend an invite to the room this game is in. In a game with Steam started through this
    // interface it also arrives as NX2_STEAM_EV_INVITED. 1 when asked.
    int (*invite)(uint64_t friend_id);
    // A datagram of n bytes (1 to NX2_STEAM_MESSAGE_MAX) to a member of the room; to 0: to every other member.
    // reliable 1: it arrives, in order with the other reliable ones; 0: it may be lost and is never late. Steam
    // encrypts it and vouches for the sender's id; only a member's datagrams are handed out (NX2_STEAM_EV_MESSAGE).
    // They travel on a channel of the plugin interface's own, apart from the game's own co-op's. 1 when queued; 0: in
    // no room, n out of range, or 512 datagrams are waiting.
    int (*send)(uint64_t to, const void *data, size_t n, int reliable);
} Nx2Steam;

typedef struct Nx2PluginApi {
    uint32_t size;
    uint32_t version;             // the newest interface version this game serves

    // ---- any thread
    const Nx2GameInfo *(*game_info)(void);
    // A line in the game's log (logs/nx2navi.log next to the executable), prefixed with the calling library's file
    // name. printf formatting; a line longer than about 1900 bytes is cut.
    void (*log)(const char *fmt, ...);
    // Copies n bytes from addr; 1 on success, 0 (and buf untouched or partly written) when part of the range is not
    // readable. No fault on unmapped memory.
    int (*read)(uint64_t addr, void *buf, size_t n);
    // Copies n bytes to addr; 1 on success, 0 with nothing written when part of the range is not memory the game may
    // write (unmapped, or the game's code and constants).
    int (*write)(uint64_t addr, const void *buf, size_t n);

    // ---- callbacks. Each on_* returns a handle above 0, or 0 when it could not register (no free slot, fn NULL).
    // fn(frame, user) once per frame the game presents, on the game's thread, between two frames.
    int (*on_frame)(Nx2FrameFn fn, void *user);
    // fn(draw, user) for every picture the window shows, on the presenting thread.
    int (*on_overlay)(Nx2OverlayFn fn, void *user);
    // fn(event, user) for every key event of the game's window, on the window's thread.
    int (*on_key)(Nx2KeyFn fn, void *user);
    // fn(codepoint, user) for every character typed into the game's window while input is captured (capture_input).
    int (*on_text)(Nx2TextFn fn, void *user);
    // Takes a callback out by its handle. It may still run once more when it was already being called on its thread.
    void (*remove)(int handle);

    // ---- only inside a frame callback or (version 2) a hook callback, on its thread
    // Calls the game's function at the guest address addr with up to 8 integer arguments and returns its x0. Returns 0
    // and writes a log line when called outside those callbacks, with nargs outside 0..8 or for an address that is not
    // the start of a function of the game.
    uint64_t (*guest_call)(uint64_t addr, const uint64_t *args, int nargs);
    // The same with every argument register given and every result register returned in regs; 1 when the call ran.
    int (*guest_call_regs)(uint64_t addr, Nx2GuestRegs *regs);

    // ---- any thread
    // on = 1: the game gets no keyboard, mouse or gamepad input and typed characters go to the on_text callbacks, for a
    // text box of the plugin's own; on = 0 gives the input back once everything is released. Counted per call: every
    // 1 needs its 0.
    void (*capture_input)(int on);

    // ---- version 2: in a game that may be older, check with NX2_HAS(api, Nx2PluginApi, hook_function)
    // Hooks the game's function that starts at the guest address addr: fn(call, user) runs at every call of it, on the
    // calling thread, before the function (Nx2HookCall says what the callback can do). Any thread may register, at any
    // time; the hook holds from the function's next call. Several hooks on one function run in the order they were
    // registered, each one's call_original entering the next. Returns a handle for remove, or 0: fn NULL, no free slot
    // (NX2_HOOKS_MAX), or addr is not the start of a function of the game.
    int (*hook_function)(uint64_t addr, Nx2HookFn fn, void *user);
    // Inside a hook callback, with the call it got: runs the hooked function (through the hooks registered after this
    // one) with call->x and call->v as arguments, stores what it returned in them and returns its x0. It may be called
    // more than once. Returns 0 and does nothing outside the callback the call belongs to.
    uint64_t (*call_original)(Nx2HookCall *call);
    // addr as a pointer when the n bytes there are memory the game's own code reads (writable: and writes) with plain
    // loads and stores: its heap, its mapped stacks, its modules' code, constants and data. The plugin's own code may
    // then read (and write) the range directly, with no call per access, for as long as the game does not unmap it:
    // inside a frame or hook callback, until the callback returns. NULL for anything else, read-only memory asked for
    // writing included: read and write still serve what they can of that. Any thread.
    void *(*map_range)(uint64_t addr, size_t n, int writable);
    // fn(user) once before the game's process ends in an orderly way: the window was closed, the game restarts, or the
    // game itself exits. It runs on the thread that ends the process, while the game's other threads still run and
    // after that thread's last frame callback; guest_call does not work in it. Not called when the game crashes or is
    // killed. Returns a handle or 0, as the other on_* calls.
    int (*on_exit)(Nx2ExitFn fn, void *user);
    // Adds an entry to the game's title menu, under the game's own (continue, new game, settings), with no rebuild of
    // the game. The player reaches it by moving down past the game's last entry, or with the mouse; choosing it calls
    // button->on_select(button->user) on the game's thread, as a frame callback is called (guest_call works in it), with
    // the title menu still showing: open your own menu from it (on_overlay, capture_input, on_key, on_pad). The entry
    // shows only while the title menu's own entries do. Returns a handle for remove, or 0: no label or callback, all
    // NX2_TITLE_BUTTONS_MAX taken, or a game version whose title menu the runtime does not know (docs/plugins.md).
    int (*add_title_button)(const Nx2TitleButton *button);
    // fn(event, user) for every button of the game's controls going down or up, on the game's thread that reads the
    // controls (queue the event for your frame or overlay callback). It also comes while capture_input holds the game's
    // input, which is what a plugin's own menu needs it for. A handle or 0, as the other on_* calls.
    int (*on_pad)(Nx2PadFn fn, void *user);

    // ---- version 3: in a game that may be older, check with NX2_HAS(api, Nx2PluginApi, open_menu)
    // Opens a menu page in the game's own settings screen with the plugin's title and rows (docs/plugins.md, "A menu in
    // the game's style"). Call it inside a title menu button's on_select: the game then opens the page with its own
    // transition, and "back" on it returns to the title menu. The game takes the input on the page and nothing on it
    // reaches the game's own settings. Returns 1 when the page will open; 0 when it cannot (not inside on_select, the
    // button is not shown as the game's own entry, a game version the runtime has no table for, rows the page cannot
    // show: the log says which), and the plugin then shows a menu of its own.
    // Version 4: called while a page is open (inside its on_event or a frame callback) it REPLACES the page: the game
    // closes the page and opens the new one, with its own rows, title and on_event; the old page sends no
    // NX2_MENU_CLOSED. "Back" on any page returns to the title menu: a sub-page that should go back to its parent page
    // gets a row for that, on which the plugin opens the parent page again.
    int (*open_menu)(const Nx2Menu *menu);
    // New texts, values and disabled states for the rows of the open page: the same number of rows of the same kinds
    // (and numbers of choices). Call it inside the page's on_event or a frame callback. 1 when taken. Version 4: a row
    // given with disabled 0 is enabled again.
    int (*update_menu)(const Nx2Menu *menu);
    // Closes the page as the player's "back" does. Any thread.
    void (*close_menu)(void);

    // ---- version 4: in a game that may be older, check with NX2_HAS(api, Nx2PluginApi, world_to_screen)
    // The camera of the game's main view (Nx2Camera). 1 when *out was filled; 0 when there is none: no scene is loaded
    // (title screen, loading), or a game version the runtime has no table for. Any thread. The first call starts the
    // tracking, so the camera is there from the frame after it.
    int (*camera)(Nx2Camera *out);
    // Where a point of the game's world shows in the overlay: out_xy in the pixels of Nx2Draw, placed inside the game
    // picture's rectangle (picture_x, ...), so bars at the window's sides are accounted for. Returns 1 when the point is
    // in front of the camera (out_xy may still lie outside the picture: compare it with the rectangle), 0 when it is
    // behind the camera or there is no camera, with out_xy untouched. Made for an overlay callback; elsewhere it uses
    // the rectangle of the overlay drawn last. For a name over a character, give the character's position plus its
    // height. The first call starts the tracking, as camera's does.
    int (*world_to_screen)(const float world[3], float out_xy[2]);
    // Ends the game in an orderly way, as closing its window does (the exit callbacks run, the saves are flushed), and
    // starts it again with the same command line and environment plus the variables given: env is NULL, or a list of
    // "NAME=value" texts ended by NULL ("NAME=" takes a variable out). For a mod that has to start the game over with
    // another setting, such as NX2NAVI_SAVE_DIR for a save it has just received. Any thread. Returns 1 when the restart
    // is on its way (the process ends within moments; do not rely on code after the call), 0 when a text of the list is
    // not of that form, with nothing changed.
    int (*restart)(const char *const *env);

    // ---- version 5: in a game that may be older, check with NX2_HAS(api, Nx2PluginApi, set_save_dir)
    // Puts the follow camera behind the player, as the player's own press of the game's camera button does (ZL in the
    // game's default controls): for a plugin that has just moved the player (a teleport beside another player), after
    // which the camera can sit inside or in front of the character. The runtime presses that button for the game, for
    // a few frames starting at the game's next read of its controls, so the game's own code does the reset, with its
    // own easing; the player's real input goes on beside it. Like the player's press it does what the button does in
    // the situation: with a target in reach the game locks on to it for those frames. Any thread. Returns 1 when the
    // press is on its way, 0 in a game version the runtime has no table for. A press asked for while the game's input
    // is held back (the runtime's menu is open, capture_input) is made when the input comes back.
    int (*camera_reset)(void);
    // Makes `folder` the place of the game's saves from now on, as NX2NAVI_SAVE_DIR does at the start, without a
    // restart: the committed save is then <folder>/save/<account> (with save-tx and save-backup beside it), and the
    // game's save mount is given a fresh working copy of what is there. For a save received while the title screen
    // shows (a co-op host's): write its files into <folder>/save/<account>/ first, call this, and the save the player
    // then loads from the title menu is that one. NULL or "" goes back to the profile's own saves.
    // What the game does with it (game 1.1.1, seen in the running game): it reads a slot's progress from the files
    // when the player loads it, so "continue" then loads the save of the new folder; but the list of saves and their
    // pictures and captions are what the game read at its start, so the load dialog still shows the caption of the
    // player's own save, a title menu that had no "continue" (a player without a save) still has none, and the slot
    // that "continue" loads is the one that was the newest at the start: give the new folder a save in the slot folder
    // of that name (slot_00 ...), with account.sav and option.sav copied from the player's own save folder. A plugin
    // that cannot meet that (no save of the player's own, another slot) restarts the game instead (restart).
    // ONLY while the title screen shows and before the player has loaded a save or started a new game: the game keeps
    // a loaded save in memory and would write it into the new folder at its next save. The runtime refuses (0, with a
    // log line saying why) what it can see is unsafe: a save of the game's is being written or committed, a file of
    // the save is open, or the folder is in use by another running game. Call it on the game's thread (a frame
    // callback, a hook, a menu page's on_event). Returns 1 when the folder is in use; game_info()->save_dir then names
    // it (a new pointer).
    int (*set_save_dir)(const char *folder);

    // ---- version 6: in a game that may be older, check with NX2_HAS(api, Nx2PluginApi, font_free)
    // Pictures and fonts for the overlay's image and text_font. All five may be called from any thread and at any
    // time, before the window exists too (nx2navi_plugin_init): the work on the graphics card is done by the
    // presenting thread when the picture or font is first needed.
    // A picture from width x height pixels in memory, 4 bytes a pixel in the order red, green, blue, alpha (alpha 255
    // opaque; colours not multiplied by alpha), the top row first, rows without padding. The pixels are copied. flags:
    // NX2_TEXTURE_* or 0. Returns a handle above 0, or 0 (the log says why): a side of 0 or above NX2_TEXTURE_SIDE_MAX,
    // rgba NULL, no memory, or NX2_TEXTURES_MAX pictures in use over all plugins.
    int (*texture_create)(uint32_t width, uint32_t height, const void *rgba, uint32_t flags);
    // A picture from a file. path is UTF-8, absolute (build it from game_info()->mods_dir or the plugin's own folder):
    //   - a PNG file: 8 or 16 bits a channel, grey, grey with alpha, colour, colour with alpha or a palette, with or
    //     without a transparent colour; not interlaced (Adam7), which is refused;
    //   - "romfs:<file>#<name>": the texture <name> of a texture archive (.bntx, or .bntx.zs) of the game's own files,
    //     named as in its RomFS, such as "romfs:UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.bntx.zs#<name>".
    //     Unpacking an archive takes from a moment to a few seconds each call: call it from a thread of the plugin's
    //     own, or once at the start. A mask of the game's (one channel) comes out white with the mask as alpha.
    // out_wh, when not NULL, gets the picture's width and height in pixels. Returns a handle or 0 as texture_create.
    int (*texture_load)(const char *path, uint32_t flags, uint32_t out_wh[2]);
    // Gives a picture up. It may be called while an overlay callback draws with it on another thread: the picture
    // drawn this frame still shows, and the handle draws nothing afterwards. Freeing a picture that was drawn makes
    // the presenting thread wait once for the graphics card (a few milliseconds): free rarely, not every frame.
    void (*texture_free)(int texture);
    // A font from a file for text_font: a TrueType or OpenType file (.ttf, .otf; of a collection .ttc the first
    // face), path as for texture_load, or "romfs:<file>" for a font of the game's own (.bfotf, .bfttf, with or
    // without .zs), such as "romfs:Font2/Scalable/ClarimoUDPE-Regular.Nin_Ounce_NVN2.bfotf.zs". pixel_size is the
    // height the letters are made for (4 to 400), flags NX2_FONT_* or 0:
    //   - flags 0: the height in a picture 1080 pixels high. The runtime makes the letters at pixel_size *
    //     Nx2Draw.scale and again when the window's size changes, so text drawn with size 0, or with pixel_size *
    //     scale, is sharp in every window. Nx2Draw.scale does not go under 2 / 3 (more on a display scaled above
    //     100 %), so that text stays readable in a small window: there such text takes a larger share of the picture;
    //   - NX2_FONT_EXACT: the height in the window's pixels as they are. For text that must keep its share of the
    //     game's picture in every window, as the game's own does: load the font at share * Nx2Draw.picture_height,
    //     and when that height has changed (and held for a moment: a window being dragged changes it every frame),
    //     free the font and load it at the new size.
    // Load a font once for each size the plugin draws at; a size other than the one the letters were made at
    // stretches them, which blurs. The letters made are Latin with its accented letters (U+0020 to U+00FF), Cyrillic
    // (U+0400 to U+052F) and the dashes, quotes and dots U+2010 to U+2027; any other character shows as the font's
    // mark for a missing one.
    // Returns a handle above 0, or 0 (the log says why): the file is missing or is not a font the runtime can read,
    // the size is out of range, or NX2_FONTS_MAX fonts are loaded over all plugins. The font can be drawn with from the
    // next picture on (until then text_font uses the overlay's own font at the size asked for); making the letters
    // takes the presenting thread some tens of milliseconds each time a font was loaded, so load fonts at the start
    // or on a change of the window, not while playing. Fonts whose letters would take more than about 24 million
    // pixels together (a font takes about 250 * its size squared) are left out, with a line in the log, and draw
    // with the overlay's own font.
    int (*font_load)(const char *path, float pixel_size, uint32_t flags);
    void (*font_free)(int font);   // gives a font up; the handle then draws with the overlay's own font

    // ---- version 7: in a game that may be older, check with NX2_HAS(api, Nx2PluginApi, steam)
    // Steam for a mod's own multiplayer: rooms (friends-only Steam lobbies with a room id), invites, and datagrams
    // between the games of a room (Nx2Steam above; docs/plugins.md "Steam"). The table is always returned; whether
    // Steam can be used is what its start and status say. Any thread.
    const Nx2Steam *(*steam)(void);
} Nx2PluginApi;

#define NX2_HOOKS_MAX 64          // function hooks registered at a time, over all plugins
#define NX2_TITLE_BUTTONS_MAX 4   // title menu buttons at a time, over all plugins
#define NX2_TEXTURES_MAX 256      // version 6: pictures at a time, over all plugins
#define NX2_TEXTURE_SIDE_MAX 4096 // version 6: the most pixels along a picture's side
#define NX2_FONTS_MAX 16          // version 6: fonts at a time, over all plugins (one per file and size)

// The executable's export NX2_PLUGIN_API_EXPORT.
typedef const Nx2PluginApi *(*Nx2PluginApiFn)(uint32_t version);
// A plugin's optional export NX2_PLUGIN_INIT_EXPORT.
typedef void (*Nx2PluginInitFn)(const Nx2PluginApi *api);

#ifdef __cplusplus
}
#endif

#endif
