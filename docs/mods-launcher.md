# Mods: the launcher's part

For mod authors. The game reads a mod's game files, code patches and cheats by itself (`runtime/mods.h`), and the
launcher keeps the mods folder in order for it (`launcher/mods.py`: archives unpacked, the load order, the switches,
code patches built into the game). On top of that the launcher (`launcher/modext.py`) adds three things a mod may
bring: a **launcher add-on**
(Python code that adds rows and menus to the launcher), **environment variables** for the game and **game code**
(DLLs loaded into the game's process before it starts running). The player's view is
[launcher.md](launcher.md), "Mods". What a DLL can do once it is in the game, the plugin interface, is in
[plugins.md](plugins.md).

## Folder layout

```
<game>\mods\                     the mods folder (nx2navi.ini mods_dir; a relative one is under the game folder)
  My Mod\                        one folder per mod; its name is the mod's name in nx2navi.ini
    mod.json                     optional: what the launcher adds
    romfs\  exefs\  cheats\      what the game reads (runtime/mods.h)
    launcher_plugin.py           the launcher add-on, named by mod.json
    bin\my_mod.dll               game code, named by mod.json
    settings.json                the add-on's settings (api.save_settings); kept when the mod is replaced
  My Mod.nx2mod                  the single file the mod came as, when it was put here; it stays
```

A mod that holds only launcher files is, to the game, a folder without files: it ignores it, and the Mods screen
gives it its switch all the same. Keep `mod.json` and the `romfs`, `exefs` and `cheats` folders at the top of the mod's
folder: before Play the launcher moves a mod's files up when the folder holds nothing of a mod at its top and exactly
one folder inside that leads to a `romfs`, `exefs` or `cheats` folder (a download wrapped in its own folder).

The mod is on unless nx2navi.ini's `mods_disabled` names it (names separated by `|`). That is the key the game reads,
so one switch in the launcher's Mods screen turns off everything the mod brings. `mods_order` (top first) sorts the
mods; the others follow by name, and the Mods screen's Load Order sets it. The launcher starts the add-ons, loads the
DLLs and merges the variables in that order.

## mod.json

```json
{
  "name": "My Mod",
  "version": "1.2",
  "author": "Someone",
  "description": "One or two sentences for the Mods screen.",
  "launcher": "launcher_plugin.py",
  "dlls": ["bin/my_mod.dll"],
  "env": {"MY_MOD_LEVEL": "3"}
}
```

Every key is optional. `name` is shown instead of the folder name. Paths are relative to the mod's folder and must
stay inside it: an absolute path, or one that leaves the folder through `..` or a link, is refused and shown as the
mod's problem, as is a missing file. A `mod.json` that is not a valid JSON object is a problem too: the game still
takes the mod's game files, the launcher takes nothing of it.

`env` values are text. When two mods set the same variable, the one higher in the order wins; the launcher also sets
`NX2NAVI_MODS_LOADED` to the names of the enabled mods joined by `|` and, on Windows, `NX2NAVI_MOD_DLLS` to the full
paths of the DLLs it loads, joined by `|` (empty when there are none).

## Single-file mods (.nx2mod)

A mod can be handed out as one file: a zip named `<Mod Name>.nx2mod` that holds the mod's files at its top
(`mod.json`, `romfs\`, ...) or in one top folder. The file's name, without its extension, becomes the folder name. A
`.zip` that holds a `mod.json` at its top, or at the top of its one folder, is taken the same way.

The player has two ways to put it in:

- **Add a Mod** on the Mods screen: the file is unpacked into `mods\<Mod Name>`. A mod of that name is replaced (its
  files go, its `settings.json` stays): this is how a player updates your mod. Everything else your mod keeps in its
  folder is gone after an update, so keep what must last in `settings.json`. It is refused while the game runs.
- **Dropping it into the mods folder**: the launcher unpacks it when it starts, at Reload Mods and before Play, when
  there is no folder of its name yet. The file stays there, like every archive in the mods folder (removing the mod
  means removing the folder and the file). A newer file put in its place is unpacked over the mod, `settings.json`
  kept, as Add a Mod does; a folder the launcher did not unpack from such a file (one you copied in by hand) is never
  replaced. The folder holds a small file `.nx2mod-source` saying which file it came from.

The launcher refuses an archive with an absolute name, a name that leaves the folder, a link, a name Windows cannot
hold, more than 20,000 files or more than 8 GB unpacked; the mods folder stays as it was.

`launcher.modext.pack(mod_dir, out_file)` makes such a file from a mod's folder, for a build script:

```python
import sys
sys.path.insert(0, r'path\to\NX2navi')        # the repository, or the installed app\<version>
from launcher import modext
modext.pack('build/My Mod', 'dist/My Mod.nx2mod')
```

It leaves out `settings.json`, `__pycache__` and every file or folder whose name starts with a dot.

For build scripts and checks, `launcher.modext` also has `install_archive(file, mods_folder)` (what Add a Mod does
with a single-file mod), `scan(game_dir)` (the mods with their `mod.json` read: `title`, `plugin`, `dlls`, `env`,
`problems`), `Host` and `load_plugins(mods, host)` (the add-ons run without a window) and
`start_game(game_dir, exe, env, dlls)` (the game started as Play starts it). The same names are found in
`launcher.mods`, where they were before; `launcher.mods.scan(game_dir)` returns the same mods, in the game's load
order, with what the game takes of each as well.

## Launcher add-on

The file `launcher` names is imported when the mod is on: when the launcher starts, after Reload Mods or Add a
Mod, and when the mod is switched on. It runs in the launcher's process (Python 3.14 with tkinter, numpy and pillow)
as a module of its own; no bytecode cache is written into the mod. To load a helper file of the mod, use
`api.mod_dir` (for example with `importlib.util.spec_from_file_location`): the mod's folder is not on `sys.path`.

The module must have `register(api)`. It may have `unregister(api)`, called before the add-on is unloaded (switched
off, Reload Mods). An exception in `register`, in the import or in any function the launcher calls later is caught,
written to the launcher's log with its traceback and shown (as the mod's problem on the Mods screen, or as the
message on screen); it never stops the launcher.

Call the API from the launcher's thread (the functions the launcher calls are called there). `refresh` and
`message` may be called from another thread: they pass the work to the launcher's thread.

### Minimal example

```python
def register(api):
    s = api.load_settings()

    def menu():
        return [
            api.toggle('Show Splits', lambda: s.get('splits', True), lambda v: save(splits=v),
                       desc='Show the split times over the game.'),
            api.text('Runner Name', lambda: s.get('name', ''), lambda v: save(name=v), prompt='Your name:'),
            api.divider(),
            api.action('Play with Timer', lambda: api.play(env={'TIMER_SPLITS': '1' if s.get('splits', True) else '0'}),
                       desc='Start the game with the timer.'),
        ]

    def save(**kw):
        s.update(kw)
        api.save_settings(s)

    api.add_home_row('Speedrun Timer', value=lambda: s.get('name', 'No runner'),
                     desc='The timer and its splits.', on=lambda: api.show_menu('Speedrun Timer', menu))
    api.on_game_exit(lambda code: api.message(f'The run ended (code {code}).'))
```

### The API

One `ModApi` per mod.

| Member | What it does |
| --- | --- |
| `api.mod_dir: Path`, `api.game_dir: Path`, `api.name: str` | The mod's folder, the game folder, the mod's folder name. |
| `api.log(text)` | A line in the launcher's log (`data\logs\launcher.log`), prefixed with `mod <name>:`. |
| `api.add_home_row(label, value='', desc='', on=None)` | A row on the home screen, in a group of the add-ons' rows under Updates. `value` and `desc` are text or a function without arguments returning text (read each time the screen is drawn). `on()` runs when the row is selected. |
| `api.action(label, on, desc='')` | A button row for a menu. |
| `api.value(label, value, on=None, desc='')` | A label and a value (text or function); selectable when `on` is given. |
| `api.toggle(label, get, set, desc='')` | An On/Off switch: `get()` returns its state, `set(bool)` changes it. |
| `api.text(label, get, set, desc='', secret=False, prompt='')` | Shows `get()` (as dots when `secret`); selecting it asks for text in a small box (`prompt` above the field) and calls `set(new)` unless the player cancels. |
| `api.divider()` | A divider between groups of rows. |
| `api.show_menu(title, rows)` | Opens a menu screen. `rows` is a function without arguments returning the list of rows; it is called each time the menu is drawn. A Back row is added at the end. Back (and Esc, B on a pad) goes to the menu before, the last one to home. `title` heads the description box. |
| `api.refresh()` | Draws the current screen again (values from functions are read again). The screen is drawn again after each of the add-on's functions the launcher calls anyway. |
| `api.back()`, `api.home()` | Back one menu; back to home (every menu closed). |
| `api.message(text)` | Text shown at the top of the description box: on home (Play's description and the add-ons' rows) and on the mods and add-on screens. `''` clears it. |
| `api.ask_text(title, prompt, initial='', secret=False) -> str or None` | Asks for text in a small box; None when cancelled. |
| `api.play(env=None, dlls=None) -> bool` | Starts the game as Play does (the mods folder put in order, the mods' variables and DLLs, the launcher hidden while it runs when so set) with `env` and `dlls` on top. A relative DLL path is under `mod_dir`. False, with a message on screen, when the game is not built or already running. When an enabled mod's code patches or code cheats are not built into the game yet, the launcher first builds the game again (its build screen replaces your menu, a few minutes) and then starts it with the same `env` and `dlls`: `play` returns True at once in that case too. |
| `api.game_running() -> bool` | Whether the game runs (also a copy the launcher did not start). |
| `api.start_process(cmd, cwd=None, console=True, env=None) -> subprocess.Popen` | Starts a program; `cmd` as for `subprocess.Popen`. `console`: in a console window of its own (CREATE_NEW_CONSOLE); else with no window. `cwd` defaults to `mod_dir`; `env` is added to the launcher's environment. |
| `api.on_game_exit(callback)` | `callback(code)` runs in the launcher's thread each time a game the launcher started ends (after a Restart of the game's own, when that copy ends). `code` is None when a game an add-on asked for with `play` never started, because the build before it failed or was cancelled. |
| `api.load_settings() -> dict`, `api.save_settings(d)` | The add-on's settings as JSON in `<mod_dir>\settings.json`; `{}` when there is none or it is not valid. |
| `api.open(path)` | Opens a folder or file in Explorer (relative paths under `mod_dir`). |

Rows are returned as opaque objects: build them with the functions above only.

## Game code (DLLs)

On Windows the launcher starts the game suspended (`CREATE_SUSPENDED`, the same hidden console and folder as Play),
loads each DLL of every enabled mod in order, then the ones `api.play` adds, and only then lets the game's main thread
run (`launcher/winsys.py` `start_game`, `inject_dlls`). Each DLL is loaded by a thread the launcher makes in the
game's process, which calls `LoadLibraryW` on the DLL's full path; the launcher waits for it (30 s at most) and checks
that the library loaded. So a DLL's `DllMain` runs before any of the game's code, with the game's own imports already
loaded; it must not wait for the game's main thread there (that thread is still suspended): start a thread of your
own for work that needs the game running, or better, export `nx2navi_plugin_init`.

The game itself then opens every library `NX2NAVI_MOD_DLLS` names (the same ones, so nothing is loaded twice) and
calls each one's `nx2navi_plugin_init` with its plugin interface, once its modules are in memory and before its code
runs: a callback every frame on the game's thread, calls into the game's functions, safe memory access, drawing
over the picture, keys and text. [plugins.md](plugins.md) describes it.

Limits:

- 64-bit DLLs only (the game is x86-64). A DLL that is missing, of the wrong architecture, lacks a dependency or
  returns FALSE from `DllMain` is logged and named on home when the launcher comes back; the game runs without it.
- The game's own Restart (its menu, a mod rebuild at start) ends the process and starts a new one itself. The
  launcher does not start that one, but the environment variables carry over (the new process inherits them), and
  with them `NX2NAVI_MOD_DLLS`: the game loads the DLLs itself there, later than the launcher would (after its own
  start-up, when it calls `nx2navi_plugin_init`). A DLL that only works when loaded before the game's start-up does
  not survive a restart.
- A game started another way (from `ocarina.exe` directly, a shortcut of the player's own) gets no DLLs and no
  variables; the game's own mod parts still apply. The Steam shortcut goes through the launcher and gets both.
- On Linux the launcher loads no DLLs and does not set `NX2NAVI_MOD_DLLS`; the other variables apply.
- A game built before the plugin interface existed ignores `NX2NAVI_MOD_DLLS` and exports no `nx2navi_plugin_api`:
  a DLL is then on its own there, as before.

Game code and launcher add-ons run with the player's rights. Say what your mod does; players are told to install only
mods they trust.
