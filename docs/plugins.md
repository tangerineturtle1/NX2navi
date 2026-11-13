# Plugins: native code in the game

For mod authors. A mod can bring a native library (a DLL) that runs inside the game's process. The game offers such
a library a small C interface: a callback once per frame on the game's own thread, hooks on the game's own functions,
calls into the game's functions, reads and writes of the game's memory, drawing over the picture, buttons on the
game's title menu, keys, the gamepad's buttons and typed text, a callback when the game closes, and the game's log.
[mods-launcher.md](mods-launcher.md) says how the launcher loads a mod's DLLs; this page is the interface.

Everything is in one header, [`runtime/plugin_api.h`](../runtime/plugin_api.h). Copy it into your mod: its notice
allows that, and it is the only file of this repository a mod may copy. The implementation is `runtime/plugins.c`;
`tests/plugins/hello_plugin.c` is a complete plugin of a hundred and twenty lines.

## Getting the interface

There are two ways, and a plugin may use either.

**Export `nx2navi_plugin_init`.** The game calls it once, on the process's first thread (not the thread the frame
callbacks come on), when the game's modules are in memory and before any of the game's code has run:

```c
#include "plugin_api.h"

static const Nx2PluginApi *api;

__declspec(dllexport) void nx2navi_plugin_init(const Nx2PluginApi *a) {
    if (a->version < NX2_PLUGIN_API_VERSION) return;   // a game older than this plugin was written for; or go on
                                                       // and check each newer member with NX2_HAS (Versions)
    api = a;
    api->log("hello, game %u", api->game_info()->game_version);
    api->on_frame(my_frame, NULL);
}
```

The game calls it for the libraries the environment variable `NX2NAVI_MOD_DLLS` names (full paths separated by `|`;
a relative path is under the game's folder). The launcher sets that variable to the DLLs of the enabled mods and to
those `api.play(dlls=...)` adds, so a mod needs nothing but its `mod.json` entry. Without the launcher, set the
variable yourself and start `ocarina.exe`.

**Ask the executable.** The game's executable exports `nx2navi_plugin_api`:

```c
Nx2PluginApiFn get = (Nx2PluginApiFn)GetProcAddress(GetModuleHandleW(NULL), NX2_PLUGIN_API_EXPORT);
const Nx2PluginApi *api = get ? get(NX2_PLUGIN_API_VERSION) : NULL;
```

`get` is NULL in a game built before the interface existed, and the call returns NULL when the game cannot serve
the version asked for. This works from any thread once the game runs, for a library that came into the process some
other way. Do not call it from `DllMain`: a DLL the launcher loads runs its `DllMain` before the game has set up
its memory and its log.

### Loading order

1. The launcher starts the game suspended and loads each DLL (`DllMain` runs; the game has not started).
2. The game starts: it reserves its memory, reads its settings, opens its log, loads the game's modules and builds
   its function table.
3. The game opens every library of `NX2NAVI_MOD_DLLS` in the order given (for a DLL the launcher loaded this finds
   the loaded one; for the game's own Restart, which the launcher does not start, it loads it) and calls its
   `nx2navi_plugin_init`. A library that does not load is named in the log and left out.
4. The game's code starts. The first frame callback comes with the first picture the game presents, some seconds
   later; overlay callbacks start with it.

On Linux the launcher loads no libraries and sets no variable. A game started with `NX2NAVI_MOD_DLLS` naming shared
objects opens them and calls `nx2navi_plugin_init`; that path is compiled but has not been run.

## Versions

`NX2_PLUGIN_API_VERSION` is 5. Every structure starts with `size`, its size in bytes in the game that filled it. A
later game adds members only at the end of a structure and functions only at the end of the table, and keeps the
meaning of what is there. So:

- a plugin built against an older header keeps working in a newer game;
- a plugin built against a newer header checks that a member exists before using it:
  `if (NX2_HAS(api, Nx2PluginApi, capture_input)) ...`, and the same for `Nx2Draw`, `Nx2GameInfo` and the others;
- `api->version` is the newest version the game serves. A change that cannot keep the old meaning gets a new
  version number, and `nx2navi_plugin_api(old)` then returns NULL once the game no longer serves the old one.

The interface is the same for every game version, but the game's addresses are not: check
`game_info()->build_id` before using an address you found in one build.

| Version | Added |
| --- | --- |
| 1 | `game_info`, `log`, `read`, `write`, `on_frame`, `on_overlay`, `on_key`, `on_text`, `remove`, `guest_call`, `guest_call_regs`, `capture_input` |
| 2 | `hook_function`, `call_original`, `map_range`, `on_exit`, `add_title_button`, `on_pad`; `Nx2GuestRegs.stack` and `nstack`; `Nx2GameInfo.profile_dir` and `save_dir`; `guest_call` inside a hook callback. `read` and `write` are the same calls, faster. |
| 3 | `open_menu`, `update_menu`, `close_menu` (a page in the game's settings screen); the title menu button is the game's own entry and its `on_select` runs inside the game's handling of the choice; `Nx2Draw.picture_x`, `picture_y`, `picture_width`, `picture_height`. |
| 4 | `camera`, `world_to_screen`; for the menu page: `open_menu` from a page (it replaces the page), `Nx2Menu.focus_row`, `update_menu` enables a row again, `Nx2MenuRow.text_max` with `NX2_MENU_TEXT` and `Nx2MenuEvent.text` (a text row the player types into); `Nx2PadEvent.keyboard`; `restart`; the environment variable `NX2NAVI_SAVE_DIR`. |
| 5 | `camera_reset`, `set_save_dir`; `Nx2HookCall.fp` (a backtrace from a hook); `Nx2GameInfo.language` and `account`; for the menu page: the cursor goes where `focus_row` says and passes over every disabled row, `update_menu` sets the caption of the row under the cursor again, and a text row is typed into with a gamepad too. |

A plugin built against an older header runs unchanged in a later game: it asks for its version and gets the same
table, of which it reads the part it knows. A plugin built against a newer header asks for the newest version first
and for an older one when the answer is NULL (an older game), and checks before it uses a later member:
`NX2_HAS(api, Nx2PluginApi, hook_function)`, `NX2_HAS(info, Nx2GameInfo, save_dir)`,
`NX2_HAS(api, Nx2PluginApi, world_to_screen)`; the example plugin does. A structure the plugin fills (`Nx2Menu`,
`Nx2MenuRow`, `Nx2TitleButton`) is read by its `size` in the same way: members the plugin's header did not have
count as 0.

```c
const Nx2PluginApi *api = NULL;
for (uint32_t v = NX2_PLUGIN_API_VERSION; v >= 1 && !api; v--) api = get(v);
```

## The functions

| Member | Thread | What it does |
| --- | --- | --- |
| `game_info()` | any | The game's description: `game_version` (111 for 1.1.1), `runtime_version` (NX2navi's own), `game_dir`, `mods_dir`, `build_id` (hex of main's build id, `""` when unknown), `main_base` and `main_size`; since version 2 `profile_dir` and `save_dir` ([The save](#the-save)); since version 5 `language` (the language the game runs in, as the console's code: `"en-US"`, `"fr"`, `"ja"`, `"pt-BR"`, `"zh-Hans"` ...; it is fixed for the run) and `account` (the name of the account's folder under `save/`, known from the start). The pointers stay valid; `save_dir` is read from the structure each time, since `set_save_dir` gives it a new one. |
| `log(fmt, ...)` | any | A line in `logs\nx2navi.log`, prefixed with `[plugin <your DLL's name>]`. `printf` formatting. |
| `read(addr, buf, n)` | any | Copies `n` bytes out of the game's memory; 1 on success, 0 when part of the range cannot be read. Never faults on unmapped memory. |
| `write(addr, buf, n)` | any | Copies `n` bytes into the game's memory; 1 on success, 0 with nothing written when part of the range is not memory the game itself may write (unmapped, or the game's code and constants). |
| `map_range(addr, n, writable)` | any | Version 2. `addr` as a pointer when the `n` bytes are memory the game's own code reads (`writable`: and writes) directly, else NULL: your code then reads and writes the range itself ([Memory](#memory)). |
| `on_frame(fn, user)` | calls `fn` on the game's thread | `fn(frame, user)` once per frame the game presents. `frame->index` counts the frames. Returns a handle, 0 on failure. |
| `guest_call(addr, args, nargs)` | **frame or hook callback only** | Calls the game's function at `addr` with up to 8 integer or pointer arguments and returns its `x0`. |
| `guest_call_regs(addr, regs)` | **frame or hook callback only** | The same with every argument register given and returned: `x[0..7]`, `x[8]` (the address of a result returned in memory) and `v[0..7]` (floating-point arguments and results; a `float` is the low 32 bits of `v[i][0]`). Since version 2 `regs->stack` points at `regs->nstack` 64-bit words, the arguments the function takes on the stack (those past the eighth integer or the eighth floating-point one), the first at the lowest address, at most 64. Returns 1 when the call ran. |
| `hook_function(addr, fn, user)` | calls `fn` on the thread that calls the function | Version 2. `fn(call, user)` at every call of the game's function that starts at `addr`, before it runs ([Function hooks](#function-hooks)). Returns a handle, 0 on failure. |
| `call_original(call)` | **inside that hook callback** | Version 2. Runs the hooked function now and returns its `x0`. |
| `on_exit(fn, user)` | calls `fn` on the thread that ends the game | Version 2. `fn(user)` once before the process ends in an orderly way: the window was closed, the game restarts itself, or the game exits. Not on a crash or a kill. Keep it short (a goodbye to a server, a file closed): the game's other threads still run, and guest calls do not work in it. |
| `on_overlay(fn, user)` | calls `fn` on the presenting thread | `fn(draw, user)` for every picture the window shows. Draw with the calls in `draw`. |
| `on_key(fn, user)` | calls `fn` on the window's thread | `fn(event, user)` for every key going down or up: `event->key` is an `NX2_KEY_*` value or the upper-case ASCII of a letter or digit. Return 1 to keep the key from the game's own menu keys (F1, Esc); F10 and Alt combinations cannot be kept, and nothing is kept while the game's menu is open. |
| `on_text(fn, user)` | calls `fn` on the window's thread | `fn(codepoint, user)` for every character typed while input is captured. |
| `on_pad(fn, user)` | calls `fn` on the game's thread that reads the controls | Version 2. `fn(event, user)` for every button of the game's controls going down or up: `event->button` is an `NX2_PAD_*` value (`A`, `B`, `X`, `Y`, the shoulder buttons, `PLUS`, `MINUS`, the four directions), named as the game's own prompts name them. It is what the game would read: the gamepad and the keys bound to its buttons. The left stick past half way counts as its direction. The events also come while `capture_input` holds the game's input. Since version 4 `event->keyboard` is 1 when no gamepad holds the button, so the press comes from a key bound to it (the key also arrives in `on_key`): a plugin's text box ignores those. |
| `add_title_button(button)` | calls `button->on_select` on the game's thread | Version 2. One more entry on the game's title menu ([Title menu buttons](#title-menu-buttons)). Returns a handle, 0 on failure. |
| `open_menu(menu)`, `update_menu(menu)`, `close_menu()` | game's thread (`close_menu`: any) | Version 3. A page of the plugin's rows in the game's own settings screen ([A menu in the game's style](#a-menu-in-the-games-style)). |
| `camera(out)` | any | Version 4. The camera of the game's main view: position, target, up, planes, angle, ratio and the two matrices ([The camera](#the-camera)). 1 when filled. |
| `world_to_screen(world, out_xy)` | any; made for the overlay callback | Version 4. Where a point of the game's world shows in the overlay's pixels; 1 when it is in front of the camera ([The camera](#the-camera)). |
| `camera_reset()` | any | Version 5. Puts the follow camera behind the player, as the player's press of the game's camera button does ([The camera](#the-camera)). 1 when the press is on its way. |
| `set_save_dir(folder)` | game's thread | Version 5. Moves the game's saves to another folder under the running game ([The save](#the-save)): for the title screen, and with a limit of the game's own that is described there. 1 when the folder is in use. |
| `restart(env)` | any | Version 4. Ends the game in an orderly way (exit callbacks, saves flushed) and starts it again with the same command line and environment plus the variables in `env`: NULL, or a NULL-ended list of `"NAME=value"` texts (`"NAME="` takes one out). 1 when the restart is on its way. |
| `capture_input(on)` | any | 1: the game gets no keyboard, mouse or gamepad input, the mouse pointer is free, and typed characters go to the `on_text` callbacks (an input method works while it is on). 0: gives the input back once every key and button is released. Counted: each 1 needs its 0. |
| `remove(handle)` | any | Takes a callback or a hook out. It may run once more if it was already being called. |

Up to 32 callbacks of each kind, 64 function hooks and 4 title menu buttons (over all plugins) can be registered at
a time.

### Title menu buttons

A plugin can put an entry of its own on the game's title menu, under the game's entries (continue, new game,
settings), while the game runs and with no rebuild:

```c
static void open_my_menu(void *user) { /* the player chose the entry */ }

Nx2TitleButton b = {.size = sizeof b, .label = "Co-op", .description = "(host or join)", .on_select = open_my_menu};
int handle = api->add_title_button(&b);
```

`label` is the entry's text and `description` an optional remark shown smaller after it, as the game writes its own
("Settings (including ...)"); both are UTF-8, are copied and are yours to translate. `order` sorts the buttons of all
plugins, lower first. `remove(handle)` takes the entry away. The plugin says nothing about where the menu is: the
runtime finds the game's screen, its parts and its functions.

**The entry is the game's own.** The title screen's layout holds four button parts and the game uses three (two
without a save). The runtime makes the first unused part visible with the game's own functions and gives it the
plugin's label, so the game draws it (its font, its ornament and glow), moves its own cursor onto it, plays its own
sounds and its confirm animation. How it behaves for the player:

- The entry shows with the title menu's own entries and goes with them (the settings screen, the save list, a loaded
  save); it comes back when the game returns to the title.
- The directions, the confirm button and the stick work on it as on the game's entries, in every language of the
  game: nothing of it depends on the game's texts.
- Choosing it calls `on_select(user)` on the game's thread, inside the game's own handling of the choice
  (`guest_call` works in it). The title menu stays as it is, the cursor stays on the entry. A plugin opens its page
  there: [a menu in the game's style](#a-menu-in-the-games-style) with `open_menu`, or a menu of its own
  (`capture_input(1)`, `on_pad`, `on_key`, `on_overlay`).
- **One entry, several buttons.** The layout has one unused part. With one button registered the entry is that
  button. With two to four it stands for one of them at a time, written `< label >`, and left and right on it go
  through them; confirm chooses the one shown.
- With keyboard and mouse as the device used last, the mouse pointer is free on the title menu: over the entry it
  moves the game's cursor there, a left click chooses it (as the game's own confirm press) and a right click goes to
  the next of several buttons. The game's own entries of 1.1.1 do not take a pointer. The pointer is read on Windows
  only. (The pointer has not been tried in a running game yet.)

What the runtime does for it (`runtime/plugins.c`, "title menu buttons"):

- A hook of the title screen's per-frame update stops the layout's animation `BtnNum` (the number of entries shown)
  one frame after the game's own number and sets the text of the part's text id `Pa_Button_NN/T_Btn_00`; a hook of
  the sequence node that waits for a button of the screen sees the entry chosen, calls the plugin and hands the
  sequence an empty name, on which the sequence goes on waiting.
- **The drawn way back.** When the game is not as expected (a build the runtime has no table for returns 0 from
  `add_title_button`; a title screen without an unused part, without `BtnNum` or without the text pane), or with
  `NX2NAVI_PLUGIN_TITLE=drawn` in the environment, the buttons are drawn by the runtime under the game's entries as
  before, in the settings menu's font, up to four of them, and `on_select` then comes with the frame callbacks. The
  log says which way holds: `plugins: title menu button: the game's own entry Pa_Button_03 (...)` or
  `... drawn by the runtime (reason)`.
- While a plugin has a title button registered, four function hooks of the runtime's are in place (of the 64), with
  [what hooks cost](#what-hooks-cost).

### A menu in the game's style

`open_menu` (version 3) opens **the game's own settings screen** with a plugin's title and rows. Nothing on it is
drawn by the runtime: the frame, the row parts, the cursor, the scrolling, the caption under the list, the sounds
and the transitions are the game's, in the game's font. The game takes the input (gamepad, or the keys bound to it)
and the game underneath gets none of it, as on its own settings screen.

```c
static const char *const speeds[] = {"Normal", "Fast"};
static void on_event(const Nx2MenuEvent *e, void *user) {
    if (e->kind == NX2_MENU_ACTIVATE && e->row == 0) host_a_game();
    if (e->kind == NX2_MENU_CHANGE && e->row == 2) speed = e->value;
}
static void on_select(void *user) {           // the title menu button's callback
    Nx2MenuRow rows[] = {
        {.size = sizeof *rows, .kind = NX2_ROW_ACTION, .label = "Host over Local Network", .value = "Host",
         .description = "Opens a game on this PC."},
        {.size = sizeof *rows, .kind = NX2_ROW_ACTION, .label = "Host over Steam", .value = "Unavailable",
         .description = "Steam is not running.", .disabled = 1},
        {.size = sizeof *rows, .kind = NX2_ROW_SELECT, .label = "Speed", .choices = speeds, .choice_count = 2},
    };
    Nx2Menu menu = {.size = sizeof menu, .title = "Multiplayer", .rows = rows, .row_count = 3, .on_event = on_event};
    if (!api->open_menu(&menu)) open_my_own_menu();   // 0: not possible here, the log says why
}
```

| Row | Shows as | Events |
|---|---|---|
| `NX2_ROW_ACTION` | the game's button row: `label` in the game's heading font, `value` on the button | `NX2_MENU_ACTIVATE` |
| `NX2_ROW_TOGGLE` | the game's on/off row; `current` 1 or 0 | `NX2_MENU_CHANGE`, `value` the new state |
| `NX2_ROW_SELECT` | the game's choice row with its arrows; `choices`, `current` | `NX2_MENU_CHANGE`, `value` the new index |
| `NX2_ROW_TEXT` | as an action row with the current text as `value` | with `text_max` above 0 (version 4): the player types into the row, then `NX2_MENU_TEXT` with `text`; with `text_max` 0: `NX2_MENU_ACTIVATE`, and the plugin takes the text itself (`capture_input(1)`, `on_text`, `on_key`), then `update_menu` |
| `NX2_ROW_SEPARATOR` | the game's ornament line | none |

`description` is shown in the caption under the list while the cursor is on the row. `disabled` rows are shown as
the game shows a setting it rules out (dimmed) and nothing happens on them; the cursor passes over them (version 5:
over every kind of row; the game itself passes over a disabled action row, and the runtime sends the cursor on from
a disabled toggle or select row, which the player sees it touch on the way), so the description of a disabled row
is not read: put the reason into an action row's `value` ("Unavailable"). The labels of action and text rows are written in the game's
heading font, which fits about 11 characters beside the button: when a page has a longer one, the labels of all its
button rows are written smaller by the same share (down to 45%), so keep them short. `NX2_MENU_FOCUS` comes when the cursor moves to a row, `NX2_MENU_CLOSED` when the page is gone.
`update_menu(&menu)` gives the open page new texts, values and disabled states for the same rows (same number, same
kinds, same numbers of choices); call it in `on_event` or a frame callback. It only keeps the rows: the game puts
them on the screen at its next frame step (on its calc thread, once the frame's jobs are done), so the page shows them one
frame later, and of several updates in one frame the last one shows. `close_menu()` closes the page as the
player's "back" does. Events come on the game's thread, inside the game's own handling: `guest_call` works.

Version 4 added:

- **A row enabled again.** `update_menu` with `disabled = 0` for a row that was disabled gives it back its look and
  its use (version 3 could only disable).
- **The cursor's row.** `Nx2Menu.focus_row` is 1 + the row the cursor goes to, when the page opens and at
  `update_menu` (0: the first row that can be used at the opening, no move at an update). A version 4 game took the
  field and left the cursor where it was; since version 5 the cursor goes there: at the opening it starts on the
  row, and after an `update_menu` it glides there within about half a second (a move asked for inside `on_event` is
  made once the game has finished answering the row's choice). A disabled row is not a place for it.
- **A page from a page.** `open_menu` called while a page is open (in its `on_event`, or a frame callback) replaces
  the page: the game closes it and opens its settings screen again with the new rows, title and `on_event`, in about
  a second; the title menu shows in between. The page that goes sends no `NX2_MENU_CLOSED`. "Back" on any page leads
  to the title menu, so a sub-page gets a row that opens its parent page again. This is also how the rows of a page
  change in number or kind.
- **A text row the player types into.** `NX2_ROW_TEXT` with `text_max` above 0 (bytes of UTF-8, at most 255): choosing
  the row starts the entry, with the current `value` on the button and a `_` after it. Typed characters are added,
  Backspace takes the last one away, Enter ends the entry: the row shows the new text and `on_event` gets
  `NX2_MENU_TEXT` with `text` (valid during the callback) and `value`, its length. Escape puts the old text back, with
  no event. The game gets no input while the entry is open (`capture_input`), and an input method works. With a
  gamepad (version 5; in version 4 it could only confirm with A or leave with B) the row shows the character being
  chosen in brackets after the text, `192.168.1[0]`: up and down go through the characters of a group (held, they
  go on by themselves), X goes to the next group (capitals, small letters, digits with `. : - _ / @` and the space),
  right takes the character, left or Y takes the last one away, A or + ends the entry and B puts the old text back.
  The first character offered is a digit after a text that ends like a number or an address, else a capital.
- **The caption after an update** (version 5). `update_menu` makes the game set the caption again, so a new
  `description` of the row the cursor is on shows at once (version 4 showed it when the cursor next moved).

Limits, as tried in game 1.1.1:

- The first page opens inside a title menu button's `on_select`, with the button shown as the game's own entry;
  `open_menu` returns 0 anywhere else while no page is open (in play, from a drawn button, in a game build without a
  table): keep a menu of your own for that case. In play the game opens its settings from its pause menu, through
  another part of its sequence than the title's; that way in is not built.
- One page at a time, 48 rows at most, at least one action or text row (the game's screen puts its first cursor on a
  button row). The rows of an open page cannot change in number or kind: open the page again (`open_menu` from the
  page, version 4).
- A page needs no row of every kind: the layout's pattern parts for the kinds it does not use (toggle, select,
  separator) are hidden when the page opens. (Version 3 showed them as a stray row.)
- A line break (`\n`) in a text is kept: the caption box under the list has room for three lines. Other control
  characters become spaces.
- There is no row kind of the game's for typed text, so `NX2_ROW_TEXT` is a button with the text on it; a gamepad
  types by choosing one character at a time on the row itself, there is no keyboard picture.
- The game's "reset to defaults" (Y) is kept from the page and its footer label is emptied; its button icon still
  shows. The settings sequence's save on the way out is left out after a plugin's page.
- The pointer does not work on the page (the game's settings rows of 1.1.1 take none).
- While the first page has been opened, ten more function hooks of the runtime's are in place, one of them on the
  function that sets every text of the game's interface (with [what hooks cost](#what-hooks-cost)).

How it is done (`runtime/plugin_menu.c`): the settings screen is made anew at every opening and builds its rows from a
table in the game's memory. `open_menu` arms the page and the title's hook hands the game's sequence the name of the
game's own settings entry, so the game opens its settings screen itself; before the screen builds, the table is
replaced by the plugin's rows (with option types the game does not know and no game data behind them, so nothing
reaches a setting), and put back when the screen is destroyed; the texts the game then sets are replaced by the
plugin's as they are set.

### Another game version

Everything the two features above know about the game is in one table, `runtime/plugin_game_ui.c`, one row per build
of the game (its build id). The numbers belong to the game's build, not to a player's PC: every player of 1.1.1 has
the same ones, whatever the processor and graphics card. Panes, parts, text ids and animations are asked of the game
by name at run time and are not in the table. At the first use the row is checked against the running game (every
function starts where the row says; the two screen classes hold their functions in the slots expected; the title
screen's name function returns "UITitleScreen"); when a check fails, `add_title_button` returns 0 with a log line
(`plugins: the game's user interface is not usable ...`) and nothing of the game is touched.

A new game version needs a new row. The anchors each entry is found from, in the new `main`:

- the screen classes: the strings `UITitleScreen` and `UISystemOptionScreen` are each returned by a two-instruction
  function next to the class's factory, which calls the constructor that stores the vtable. Set-up, update, build,
  open, focus and decide are slots of those vtables (0x2e8, 0x300; 0x2f0, 0x310, 0x170, 0x190 in 1.1.1);
- the layout functions: the title screen's set-up calls "animation by name" with `"In"`, `"MenuIn"`, "pane by name"
  with `"N_BtnPos_00"` and "part by name" with `"Pa_Button_%02d"`; the dialog screen next to it (strings
  `LayoutMsg/SystemWindow_00`, `BtnNum`) calls "stop animation at frame" and "set text by text id";
- the sequence nodes: each node class has a function returning its name (`SeqExecuteWaitScreenButtonInput`,
  `SeqExecuteSaveGameData`, `SeqQueryControllerTrigMaskChecker`); its update is in the same vtable;
- the option table and the row classes: the settings screen's build function reads the table (count, items) and
  makes the three row classes, whose fill functions set the text ids `T_Option_00` and `T_Text_00`; the return
  addresses in the row are the instructions after those calls of "set text by text id";
- the camera: the camera update is the function that copies near, far, angle and ratio from its argument into a
  perspective projection four floats apart and stores a position and a target as doubles and as floats; the cameras'
  actors are named `PlayerCamera` and `EventCamera`; the button `camera_reset` presses is the one the game's control
  table names `LockOnAndCamerraReset` (ZL in 1.1.1);
- structure offsets: read them in the same functions (the fields they load from the screen and the row objects).
  A diagnostic plugin with `hook_function` on the candidates and a log line per call confirms each in a running game.

Resolving all of this by pattern at every start was considered and left out: the anchors are strings and call
patterns that a scan could follow for the two class vtables, but most entries are structure offsets and return
addresses inside functions, which no cheap pattern finds safely; a wrong guess there writes into the game's objects.
A table that is checked before use fails closed instead.

### Function hooks

`hook_function(addr, fn, user)` makes the game call `fn` whenever its function at `addr` is called, whoever calls it
(the game itself, or a `guest_call`), on the calling thread and before the function runs. `addr` is the start of a
function of the game, in any of its modules; anything else is refused with a log line. Hooks can be added and
removed at any time while the game runs, from any thread, and need no rebuild of the game.

```c
static void on_enemy_update(Nx2HookCall *call, void *user) {
    if (held(call->x[0])) call->skip = 1;        // this enemy's update does not run this frame
}

static void on_step(Nx2HookCall *call, void *user) {
    before_the_frame();                          // guest calls work here
    api->call_original(call);                    // the game's frame step runs now
    after_the_frame();                           // and here
}
```

The callback gets the call in an `Nx2HookCall`:

| Member | Meaning |
| --- | --- |
| `x[0..7]`, `x[8]` | The integer and pointer arguments as the caller passed them, and the address of a result returned in memory. Change them to change what the function gets. |
| `v[0..7]` | The floating-point and vector arguments, two 64-bit halves each (a `float` is the low 32 bits of `v[i][0]`). |
| `sp` | The stack pointer at the call. Arguments past the registers are the 64-bit words at `sp`, `sp + 8`, ...: read or change them as memory. Read only. |
| `lr` | The guest address the function will return to, which says who called. Read only. |
| `fp` | Version 5 (check `NX2_HAS(call, Nx2HookCall, fp)`). The caller's frame pointer, for a backtrace: see below. Read only. |
| `addr` | The hooked function. |
| `skip` | Set to 1 to leave the function out. |

What happens to the function is the callback's choice:

- **Nothing more.** When the callback returns, the function runs with `x` and `v` as the callback left them.
- **`api->call_original(call)`.** The function runs at that point with `x` and `v` as they are. When it returns, `x[0]`,
  `x[1]` and `v[0..]` hold its results; the callback goes on, may change them, and the caller gets them. Calling it
  again runs the function again.
- **`call->skip = 1`.** The function does not run. Its caller gets `x[0]`, `x[1]` and `v[0..]` as the callback set
  them, so set the return value the caller expects.

Rules:

- The callback runs on whichever of the game's threads makes the call, and two threads can be in it at once. The
  game's frame step is called on one thread; a function the game's workers call is not.
- `guest_call` and `guest_call_regs` work inside a hook callback. Whatever they leave in the registers is undone
  before the hooked function runs. A guest call of the hooked function itself comes back into the hook: guard against
  that, or use `call_original`.
- The function's caller sees what a caller may see after a call: the results, and every register a function must
  preserve as it was. A hook cannot change those other registers.
- Several hooks on one function, from one plugin or several, run in the order they were registered: the first one's
  `call_original` (or its return) enters the second, and the last one's enters the function. Eight run per function.
- Return quickly and do not wait for things in a hook: the thread is the game's.
- A hook on a function the game calls very often costs the game the callback each time. A function nobody hooks
  costs nothing new while no hook is registered, and next to nothing while some are ([what it costs](#what-hooks-cost)).

**Who called, all the way up** (version 5). `lr` names the caller; `fp` leads to the callers above it. The game's
functions keep a frame record on the stack: the 64-bit word at `fp` is the frame pointer of the caller's caller and
the word at `fp + 8` is the address the caller itself returns to. Follow it with `read`, which stops safely at the
end:

```c
uint64_t fp = NX2_HAS(call, Nx2HookCall, fp) ? call->fp : 0, rec[2];
api->log("called from main+%llx", (unsigned long long)(call->lr - base));
for (int i = 0; i < 16 && fp && api->read(fp, rec, 16) && rec[1]; i++, fp = rec[0])
    api->log("  from main+%llx", (unsigned long long)(rec[1] - base));
```

A function that keeps no frame record (a small leaf, a tail call) does not show in the chain; the rest does. In the
running game the chain of a hook on the settings screen's focus function read the screen's control handler, the
control's change of state, the cursor's update and on up to the frame loop.

**A callback inside the game's frame.** A frame callback runs on the thread that presents, after the picture was
handed over. A hook gives a callback at a point of the game's own choosing instead: in game 1.1.1 the function at
`main + 0x3b4e300` is called once per frame from the engine's frame loop, on another thread than the one that
presents, and a hook on it that calls `call_original` runs code before and after it there (the example plugin counts
its calls). Leaving that function out does not hold the game still, though: tried for 300 calls, the game went on
drawing and its world went on moving, and it ran on normally afterwards. To hold one thing still, hook the function
that updates it and skip it.

#### What hooks cost

Every function of the translated game starts with one test of a flag, which was there before hooks existed (the
runtime uses it itself at start). With no hook registered that test is all there is: nothing was added. While at
least one hook is registered, every function call also reads one byte of a 4 KiB table and tests it; only functions
whose address shares a table slot with a hooked one (1 in 4096) go further, and only the hooked ones run a callback.
Measured in game 1.1.1 at 30 frames a second in Hyrule Field, on a PC with 16 hardware threads: with no hook the game
used the processor time it used before hooks existed (225 million cycles a frame over all its threads, in both
builds); with one hook registered, on a function that is not called, 230 million, 2% more, at the same frame rate.
The change made the executable 7 MB (2%) larger.

### Memory

A guest address is an address in the game's process, so the game's memory can be read in place. Three ways, from
the safest to the fastest:

- `read` and `write` check the range and copy. They never fault, and `write` never writes into the game's code or
  constants. Since version 2 a range inside the game's heap, its stacks or its modules is copied without taking the
  memory manager's locks: in the running game a `read` or a `write` of 8 bytes of the heap took 0.05 microseconds
  where it took 240 before.
- `map_range(addr, n, writable)` makes that check once and hands back `addr` as a pointer. Use it for a structure
  you read or write field by field. The pointer is good for as long as the game does not unmap the memory: inside a
  frame or hook callback, until the callback returns; ask again in the next callback (the answer is remembered, so
  asking is cheap). It is NULL for memory that is not plainly the game's (unmapped addresses, the game's code when
  asked for writing, thread blocks of the kernel region, your own process's memory): fall back to `read` and `write`.
  Use the pointer in your own code only; do not hand it to a system call (`ReadFile`, `recv`) as its buffer, which
  can fail on pages the game's memory manager has set aside. Copy through a buffer of your own instead.
- Without either, a pointer you took from the game's own data may point anywhere: check before you follow it.

### The camera

`world_to_screen` (version 4) says where a point of the game's world shows on the screen, for a name over another
player's head or a marker on a place. Call it in the overlay callback:

```c
static void on_overlay(const Nx2Draw *d, void *user) {
    float head[3] = {other.x, other.y + 1.9f, other.z}, xy[2], wh[2];   // the world is in metres, y up
    if (!NX2_HAS(api, Nx2PluginApi, world_to_screen) || !api->world_to_screen(head, xy)) return;   // behind the camera
    if (xy[0] < d->picture_x || xy[0] > d->picture_x + d->picture_width) return;                   // off the picture
    d->text_size(0, other.name, wh);
    d->text(xy[0] - wh[0] / 2, xy[1] - wh[1], 0, NX2_RGBA(255, 255, 255, 255), other.name);
}
```

The point comes back in the overlay's pixels, already placed inside the game picture's rectangle (`picture_x`,
`picture_y`, `picture_width`, `picture_height`), so bars beside a picture of another shape than the window are
accounted for, and so is a picture wider than 16:9 (the camera's ratio is read from what the renderer uses). It
returns 0, with `out_xy` untouched, for a point behind the camera and while there is no camera (title screen,
loading). A point in front of the camera can still lie outside the picture: compare with the rectangle.

`camera(&c)` gives the numbers themselves (`Nx2Camera`; set `c.size` first): `position`, `target`, `up`,
`near_plane`, `far_plane`, `fov_y` (radians), `aspect`, and the matrices `view` and `projection`, 16 floats each, row
by row, for column vectors: `clip = projection * view * (x, y, z, 1)`. In the camera's space x is right, y up and the
camera looks along -z; `clip.x / clip.w` and `clip.y / clip.w` run from -1 to 1 across the game's picture, y up. The
distance for sizing a label is the length of `point - position`.

`camera_reset()` (version 5) puts the follow camera behind the player. A plugin that moves the player (a teleport
beside another player) calls it afterwards: the camera stays where it was relative to the world otherwise, which can
be inside the character or in front of its face. The reset is the game's own: the runtime presses the game's camera
button (ZL, "lock on and camera reset") for the game for 160 ms, beginning at the game's next read of its controls,
beside whatever the player holds. So it does what that button does in the situation: with nothing to lock on to the
camera swings behind the player in about a quarter of a second (seen in game 1.1.1: from 4 m in front of the
character to 4.5 m behind it in 8 frames); with a target in reach the game locks on to it for those frames, as a
tap of the button would. Not tried with a target in reach, nor during an event. A press asked for while the
runtime's own menu or a plugin's `capture_input` holds the game's input is made when the input comes back. It
returns 0 in a game version without a table row.

What to know:

- The first call of either function starts the tracking (one function hook of the runtime's, on the game's camera
  update, with [what hooks cost](#what-hooks-cost)); the camera is there from the next frame.
- The numbers are those of the frame the game is working on, one frame ahead of the picture in the window: a marker
  trails a fast turn of the camera by a frame.
- It is the camera of the scene: the one that follows Link, and the one the game's events direct while it is in use.
  The cameras of the interface's 3D models and of the sound's listener are not it. During a cutscene and with the
  debug menu's free camera on, the numbers were not checked against the picture.
- Game 1.1.1 only (`runtime/plugin_camera.c`, the table in `runtime/plugin_game_ui.c`); in another game version both
  functions return 0.

### The save

`game_info()->profile_dir` is the folder the game keeps its saves, cache and account in. `save_dir` is the folder of
the committed save, `<profile_dir>/save/<account>`: it is `""` until the game has opened its storage, a few seconds
after start, so read it in a frame callback, not at init. The game writes a save into a working copy and swaps the
finished copy in for the committed save, so a file there is never half written, but it can be replaced at any time
while the game runs. To give the game another save, do not write into `save_dir` under a running game: put the
save's files into `<profile_dir>/save-import/` (write them into `save-import.part` and rename the folder when it is
complete); the next start of the game makes it the save and keeps a backup of the one it replaces. This is what
the settings menu's save import does.

To start the game on another save without touching the player's own, start it with the environment variable
`NX2NAVI_SAVE_DIR=<folder>` (a mod's launcher add-on passes it in `api.play(env=...)`): the committed save, the
working copy and the save backups then live under that folder (`<folder>\save\<account>`, `save-tx`, `save-backup`)
while the account, the cache and everything else stay in the player's profile; `save_dir` names the folder in use.
The game reads the variable once, when it opens its storage; a change of it afterwards does nothing. A plugin
that receives a save while the game runs (a co-op host's) writes it into such a folder and calls
`api->restart((const char *const[]){"NX2NAVI_SAVE_DIR=<folder>", NULL})`: the game closes as from its window and
comes back on that save, with the player's own profile, settings and caches. A plugin that has the save before the
game opens its storage (in `nx2navi_plugin_init`) sets the variable in its own process instead and needs no restart;
`game_info()->account` names the account's folder by then.

**Without a restart, on the title screen** (version 5): `api->set_save_dir(folder)` moves the saves to `folder` under
the running game, exactly as the variable places them: the committed save is `<folder>\save\<account>` from then on,
the game's save mount gets a fresh working copy of what is there, and `game_info()->save_dir` names the new folder
(read the pointer again). `NULL` or `""` goes back to the profile's saves. Write the save's files into
`<folder>\save\<account>\` before the call. It is for the title screen, before the player has loaded a save or
started a new game: afterwards the game holds its save in memory and would write that into the new folder. The
runtime refuses, with 0 and a log line, what it can see is unsafe: save data being written or committed, a file of
the save open, the game closing, a folder another running game uses. Call it on the game's thread.

What game 1.1.1 makes of it, as seen in the running game (the title screen, `set_save_dir` to a folder whose
`slot_00` held another save, then "continue" and "load"): **the game loaded the save of the new folder**, and its
next save went there. But the list of saves is what the game read at its start:

- the load dialog still shows the picture and caption of the player's own newest save, not the received one's;
- "continue" loads the slot that was the newest at the start, so the received save goes into the slot folder of that
  name, with `account.sav` and `option.sav` copied from the player's own save folder (tried with the newest save in
  `slot_00` and the received save written to `slot_00`; another slot, or a folder without the other slots' files
  when the player then opens the full list, was not tried);
- a player without a save has no "continue" on the title menu and gets none: for them, and whenever in doubt, use
  `restart` as above.

### Drawing

Inside an overlay callback, `draw` holds the picture's `width` and `height` in pixels, `scale` (1 at a picture 1080
pixels high: multiply your sizes by it), `text_height` (a readable text size at that scale), `menu_open` (the
game's settings menu is open; your drawing shows under it) and `frame` (the index of the last frame callback).

| Call | Draws |
| --- | --- |
| `text(x, y, size, rgba, utf8)` | Text in the menu's font with its top left corner at `x`, `y`; `size` is the line height in pixels (0: `text_height`); `\n` starts a new line. |
| `text_size(size, utf8, out_wh)` | Nothing: the width and height that text would take. |
| `rect(x0, y0, x1, y1, rgba, thickness)` | An outline. |
| `rect_filled(x0, y0, x1, y1, rgba, rounding)` | A filled rectangle, its corners rounded by `rounding` pixels. |
| `line(x0, y0, x1, y1, rgba, thickness)` | A line. |

Colours are `NX2_RGBA(r, g, b, a)`. Coordinates are pixels of the window's picture, the origin at its top left; the
game's own image may be letterboxed inside it: since version 3 `picture_x`, `picture_y`, `picture_width` and
`picture_height` give its rectangle (check `NX2_HAS(draw, Nx2Draw, picture_height)`), and a point of the game's
picture at the fractions (u, v) is at `(picture_x + u * picture_width, picture_y + v * picture_height)`. The calls and the `draw` pointer are valid only inside the callback
that got them. The font holds the characters the menu's languages need; a character it lacks shows as its
replacement glyph.

## Threads

Three threads call a plugin, and they run at the same time:

- **The game's thread** (frame callbacks). The callback runs between two frames: the game has handed its finished
  picture to the display and has not started the next frame on this thread. This is where the game's state is
  read, changed and where its functions are called. `guest_call` works here and in hook callbacks and nowhere else:
  called from another thread, or outside those callbacks, it does nothing, returns 0 and writes a log line (the
  first 16 times). The game has other threads (sound, file loading, workers) that keep running during the callback;
  what the game itself guards with locks is not safe to change just because the frame is over.
- **Any thread of the game** (hook callbacks): the one that calls the hooked function. A hook on the frame step is
  the place for changes that must happen at a fixed point of the game's own frame.
  **The game's user interface is one of those things.** Its layout animations are advanced by a job on the worker
  threads, and that job also runs while the finished picture is presented, which is when frame callbacks run (measured
  in game 1.1.1: during 253 of 3664 presents, during none of 3664 frame steps). A game function that starts, stops or
  sets an animation of a screen (most of what changes a button, a row or a cursor does) must therefore not be called
  from a frame callback: now and then the animator is left walking a list without an end and the game makes no more
  frames. Call such functions from a hook of the frame step (below; game 1.1.1: `main+0x3b4e300`, the calls that
  return to `main+0x6e70fc` and `main+0x6e6ddc`), as the runtime's own menu page does since 2026-11-13 (until then
  `update_menu` made those calls at once, and a page updated at every typed character could stop the game).
- **The presenting thread** (overlay callbacks). Draw from data your frame callback prepared, guarded by a lock or
  atomics of your own. Do not call game functions here, and return quickly: the picture waits for you.
- **The window's thread** (key and text callbacks). Queue the event for your frame callback and return.
- **The game's thread that reads the controls** (pad callbacks): the same rule, queue and return. A title menu
  button's `on_select` comes with the frame callbacks, on their thread.
- **The thread that ends the game** (exit callbacks): the window's, or one of the game's.

`game_info`, `log`, `read`, `write`, `map_range`, `on_*`, `hook_function`, `remove` and `capture_input` may be called
from any thread, your own threads included.

A callback runs with the registers of the game saved: whatever a guest call leaves in them is undone when the frame
callbacks return, and before a hooked function runs. The callee runs on the game thread's own stack below its stack
pointer; stack arguments are put there.

## Limits and rules

- **Stay loaded.** A library that has registered a callback must not unload. The game pins the library at each
  registration, so `FreeLibrary` no longer unloads it.
- **A guest call is a real call.** A wrong address or wrong arguments crash the game like any bug in it would; the
  crash report in the log names the function. Only the start of a function of the game is accepted as an address.
- **`read` and `write` check, then copy.** They cannot fault on memory that is unmapped when they look; inside a
  frame or hook callback nothing unmaps memory under them on the game's thread. A read from another thread while the
  game gives that memory back to the system can still fault, and so can a pointer from `map_range` used then.
- **A hook is in the game's own call.** A hook that changes arguments or results, or leaves a function out, changes
  what the game does next: the game crashes like any bug in it would when it gets something it cannot handle.
- **The exit callback is for orderly ends only.** A crash, a kill from the task manager or a fatal error of the
  runtime end the process without it.
- **The game's own Restart** (its settings menu, a mod rebuild) starts a new process that inherits the
  environment: the plugins named in `NX2NAVI_MOD_DLLS` are loaded again there, and your state starts from nothing.
- 64-bit libraries only. Plugins run with the player's rights.

## Building and trying one

With the compiler the launcher installs (`%LOCALAPPDATA%\Programs\NX2navi\toolchain\msys64\ucrt64\bin`):

```
clang -O2 -shared -I runtime -o build\plugins\hello_plugin.dll tests\plugins\hello_plugin.c
set NX2NAVI_MOD_DLLS=%CD%\build\plugins\hello_plugin.dll
<game folder>\ocarina.exe
```

The log then holds `plugins: loaded ...hello_plugin.dll` and the plugin's own lines, and the picture shows its text
in the lower left corner. Any C or C++ compiler that makes a 64-bit DLL works; link the C runtime statically or ship
its DLLs in your mod.

`ocarina.exe --probe-plugins` checks the interface without game files: the version answer, the memory calls and
`map_range` on mapped and unmapped memory, every kind of callback and its removal, the input capture's count, the
guest call's refusal outside a frame callback, function hooks on a stand-in function (changed arguments,
`call_original`, the skip, several hooks in order, the caller's registers afterwards, the table's limit), and the
title menu buttons against a stand-in for the menu (when they show, their order and places, the selection moving in
and out, what the game gets of the input meanwhile, the choice's callback) with the pad events.
`python tests/plugins_check.py <game folder>\ocarina.exe` runs it, and checks the header, the recorded layout of
every structure, the example plugin and a plugin built against the version 1 header.

`NX2NAVI_PLUGIN_FAST_MEM=0` in the game's environment makes `read`, `write` and `map_range` take the memory
manager's locks for every call, as version 1 did (`map_range` then always answers NULL): for telling a problem of
the short way from one of your own.
