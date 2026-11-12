# Launcher

NX2navi's launcher installs everything the build needs, builds the game from the player's own dump, starts it, and
keeps the port up to date. Players download one file, `NX2navi-Setup-<version>.exe`; nothing of the game ships with
it. The code is `launcher/`, the release is made by `tools/make_launcher.py`, and the build itself is
`tools/make_game.py` (the build kit's script, [README-BuildKit.md](README-BuildKit.md)).

## What the player does

1. Starts `NX2navi-Setup.exe` (from the folder that holds their `.nup` dump, ideally). It installs the launcher into
   `%LOCALAPPDATA%\Programs\NX2navi` without administrator rights, adds Start menu and Desktop shortcuts and an entry
   in Settings > Apps, and opens the launcher. Windows SmartScreen warns once: the programs are not signed.
2. The launcher finds the dump by itself in the setup's folder, Downloads, Desktop, Documents or the top of each
   drive, and proposes `<launcher>\game` as the install folder (or `X:\Games\NX2navi` on the drive with the most
   free space when that drive has less than 40 GB). Both rows can be changed.
3. Selects **Begin Installation**. The launcher downloads the build tools (about 170 MB, once), checks the dump,
   unpacks it, translates and compiles the game: 20 to 45 minutes. It keeps the PC awake meanwhile.
4. Selects **Play**. The launcher hides while the game runs and comes back when it exits.

From then on the launcher looks for a new version when it opens and every hour while it runs, also while it is
hidden during play (Settings can turn that off). A new version downloads in the background, the game playing on, and
installs when the game is not running: right after the game exits, or the next time the launcher opens. The launcher
then restarts in the new version and rebuilds the game. With Install Updates Automatically off (Settings) it waits
until the player selects Updates. Saves and settings are never touched.

## Look

The window follows the game's options menu, with colours sampled from it: a wooden band across the whole top with
Navi and the name, which is also the window's title bar (no system title bar: drag the band to move the window,
minimise and close sit at its right end, Alt+F4 closes), bronze label cells, dark value cells, orange On/Off choices, a white glowing cursor around the selected
row, the menu's filigree dividers and a description box for the selected row. All text is in the game's own Cetra
Display: the launcher decodes it from the player's dump or built game into `data/cache/fonts` (as
`runtime/menu_assets.c` does for the in-game menu) and uses the system's interface font until then. Every picture is
drawn in code (`launcher/ui/art.py`); no texture, font or sound of the game is in the repository or the release.

Customize Launcher (on the home screen) makes the look the player's own:

- Color Scheme: Forest (the colours above), Volcano, Lake, Desert, Temple and Shadow. A scheme is the first one with
  its woods, accents, backdrop and glow turned to other hues (`art.THEMES`, `art.recolor`), so each keeps the same
  light and dark steps and the same readable text. The window is drawn again in the new colours at once. The names
  are plain words, not the game's. Custom, the last one, takes three colours of the player's: Background Color for
  the backdrop, Panel Color for the list panel, the description box and the title band, and Button Color for the
  rows' cells and the orange choices in them. Each opens a picker in the description
  box's place: a square of the hue's colours (saturation across, brightness up and down) and a hue bar under it, to
  click or drag on, Hue, Saturation and Brightness rows for the arrows and the gamepad, and Hex Code, where a code
  such as `#3A7BD5` is typed. The picked colour stands in for one colour of the first scheme (`art.CUSTOM_SLOTS`) and
  the rest of its part of the window follows it, lighter and darker as in the first scheme (`art.follow`); text on
  a light button or panel turns dark. The window takes the new colour as the mouse moves; Reset brings back the first scheme's colour.
- Background: a picture of the player's own behind the panels instead of the scheme's backdrop (`launcher/ui/picture.py`:
  PNG, JPEG, WebP, BMP, GIF or TIFF, read with Pillow). It fills the window below the title band, so none of it is
  hidden under the band: cut to that shape around its middle and scaled evenly, never stretched, and kept as
  `data/background.npy`, so the launcher needs the file no more (a copy an older launcher kept for the whole window is
  made again from the file while it is there); Background Dimming darkens it (0 to 80 %) for the
  text over it, the list panel and the description box turn a little see-through, and Remove Background brings the
  scheme's backdrop back.
- Animations (on by default): the cursor glides from row to row, a new screen slides into place, sparks drift up
  over the backdrop and Navi bobs in the title band with a pulsing halo (`App.animate`). They move by the time that
  passed, stand still while the window is hidden or minimised or another window is in front (the game, when Hide
  Launcher While Playing is off), and Off stops them all.
- Sounds (Low by default; Off, Medium or High): two of the game's own interface sounds, the decide and cancel sounds
  of its menus, when a row is chosen or a choice changes and when the player goes back. The launcher decodes them
  from the player's built game or dump into `data/cache/sounds` (`gamefiles.extract_sounds`:
  `Sound/Resource/Stream/SE_Sys_Do00.bwav` and `SE_Sys_Cancel00.bwav`, the files `runtime/menu_sound.c` plays in the
  in-game menu). No sound is in the repository, the release or the setup, and the launcher makes none of its own:
  it is silent until the player's game has been read. Moving the cursor makes no sound. They are played from memory
  (PlaySound on Windows; `pw-play`, `paplay` or `aplay` elsewhere).

- Add to Steam (Windows): the game in the player's Steam library as *The Legend of Zelda Ocarina of Time*, started
  from there without the launcher's window (`launcher/steam.py`). Its pictures are made from the player's game when
  the row is selected (`launcher/gameart.py`): Navi as the game's HUD draws her for the icon, the title screen's logo,
  and a frame of the first title movie (the moonlit field) as the background, the cover and the wide tile, the movie
  decoded with the toolchain's `dav1d.exe` and the logo with the runtime's own ASTC decoder built with its clang. They
  go into each signed-in account's `config/grid`, the shortcut into its `shortcuts.vdf` (other shortcuts kept as they
  are, ours updated when it is there; the file before the first change kept as `shortcuts.vdf.nx2navi-backup`).
  Steam reads that file only when it starts and writes it back when it closes, so a running Steam is asked to close
  first and started again after. The shortcut runs the launcher's `pythonw.exe` on `data/steam_play.pyw`, which
  starts the launcher of `app/current.txt` with `--play`: the game starts as Play starts it, and the process waits
  for it, so Steam counts the game as running while it is.

The choices are `theme`, `custom_background`, `custom_panel`, `custom_buttons`, `background`, `background_dim`,
`animations` and `sounds` in `data/launcher.json`.

Navi (`art.NAVI`, 22 x 21 pixels, drawn in whole pixels at every size) is also the icon of `NX2navi.exe`,
`NX2navi-Setup.exe`, the window and its taskbar button (`art.navi_ico`, written into the release by
`tools/make_launcher.py`). The window sets that icon on itself, and the setup's shortcuts carry the window's taskbar
identity (`NX2navi.Launcher`), so the taskbar groups and pins it under NX2navi rather than Python.

Screens (`launcher/ui/app.py`): first run (dump, install folder, Desktop Shortcut, Check for Updates at Start,
Update Channel, Begin Installation); the steps of an install, update or rebuild with their progress, the build log's
last lines and Cancel, and after a failure Try Again and Copy Report; home (Play, Updates, Game Folder, Saves,
Mods, Rebuild the Game, Customize Launcher, Settings); mods (Open Mods Folder, Add a Mod, Add a Mod Folder, one On/Off
row for each mod, More Mods when they fill more than a page, Load Order, Back); load order (each mod with its place,
left and right to move it); customize (Color Scheme, with Custom Background Color, Panel Color and
Button Color, Background, with a picture set Background Dimming and Remove Background, Animations, Sounds, Add to Steam on
Windows, Back); the colour picker (Hue, Saturation, Brightness, Hex Code, Reset, Done); saves (Export Save, Add Log and Settings, Export To, Import a Save, Import a Save Folder,
Open Saves Folder, Back); settings (update check, Install Updates Automatically, Hide Launcher While Playing, Keep
Build Cache, Update Channel, the folders, Open Logs Folder, Uninstall, Back); uninstall (keep or remove the built game).
A screen with more rows than the window holds draws them closer together (`fit_rows`).

Controls: the mouse (hover moves the cursor, a click selects); the keyboard (arrows, Enter or Space, Esc, F5 checks
for updates); an XInput gamepad (d-pad or left stick, A, B, Y) while the window is in front.

`python -m launcher --selftest --shots DIR` draws every screen with made-up states and saves a PNG of each.

Run from a checkout, the launcher and the build use the Python that started it, which needs the packages of
`tools/requirements-launcher.txt`. At startup the launcher checks that this Python can read zstd files
(`toolchain.zstd_missing`: Python 3.14's `compression.zstd`, or the `zstandard` package). If it cannot, the first screen
and the log say so and give the `pip install` line, and Gather Build Tools stops with the same message before it
unpacks anything.

## The launcher's folder

```
NX2navi.exe              launcher/native/nx2navi.c: swaps in a staged Python, starts app\<current>\launcher\start.pyw
python/                  python.org's embeddable Python 3.14.9 with tkinter, numpy, lz4, zstandard, fonttools, pillow
                         and truststore
app/current.txt          the version the stub starts; app/previous.txt the one before it
app/<version>/           launcher/, runtime/, recomp/, tools/, bin/nx2navi_xlate.exe, VERSION,
                         toolchain.lock.json (the pinned build tools), feed.json (where updates come from), and in a
                         release made with --prompt-art, prompt-art/ (the button art the prompts are drawn from); a
                         release staged in the background also holds staged.json and the Python it brings,
                         python.staged/, until it installs
pydeps/                  zbic, downloaded from PyPI on this PC
toolchain/msys64/ucrt64  MSYS2's clang and libraries, downloaded from the MSYS2 repository on this PC
toolchain/pkgs/          the downloaded package files
data/launcher.json       settings; data/logs/launcher.log (and crash.log); data/cache/fonts and data/cache/sounds
                         (the game's font and interface sounds, decoded from the player's game);
                         data/background.npy (the player's background picture, fitted below the title band)
game/                    the built game, unless another folder was chosen (its layout: README-BuildKit.md), with
                         saves/ for saves to import and the backups imports make (below)
```

A path the Windows ANSI code page cannot hold breaks the MSYS2 tools, so the toolchain then goes to
`C:\NX2navi\toolchain` instead (`launcher/paths.py`).

## Installing

`jobs.InstallJob`, one row per step:

1. **Gather Build Tools** (`launcher/toolchain.py`): the packages `toolchain.lock.json` pins (MSYS2 UCRT64 clang,
   gcc, lld, SDL3, Opus, libvpx, dav1d, zstd, Vulkan headers and loader, VMA and their dependencies: about 32
   packages, 175 MB; and, when the release found the same clang version there, MSYS2 CLANG64's clang with the DLLs
   it loads: 9 packages, about 100 MB),
   each checked against its sha256 and unpacked into `toolchain/msys64.new`, which replaces `toolchain/msys64` once
   complete. No MSYS2 shell or pacman runs. Then zbic: the wheel for the bundled Python from PyPI, checked against
   PyPI's sha256, into `pydeps/`.
2. **Inspect Your Game** to **Compile**: `tools/make_game.py --nup <dump> --out <game> --msys <toolchain> --events`,
   whose `@@` progress lines drive the rows (check, exefs, romfs, lift, compile). Before the first long step it
   looks at what can be known to fail later: the disk space of every step that will run (the RomFS image and 1 GiB,
   2 GiB for the build cache, 4 GiB for the prepared shaders; `check_space` names each part and how much is
   missing), and that the build tools compile and link an empty program (`probe_toolchain`). How it keeps the build
   short:
   - The RomFS image (26 GB, mostly disk work) is packed beside the lift and the compile (processor work), so a first
     build takes about as long as the longer of the two. Only `romfs.c`, which includes the RomFS index's
     `romfs_config.h`, waits for the image.
   - Every logical processor compiles, the largest files first, so no big file starts last and runs on alone; fewer
     compiles run when memory is short, as many as 80 % of the free memory holds at 768 MiB each (`make_game.py`
     `default_jobs`).
   - The lifted code compiles with MSYS2 CLANG64's clang when the toolchain has it: the same LLVM release as
     UCRT64's, built with profile-guided optimisation, about a fifth faster on this code and writing the same objects
     (it reads UCRT64's headers; `make_game.py` `fast_clang` checks that both report the same version and target).
   - The lifted code is compiled for the processor's own level, x86-64-v3 (AVX2, FMA) or v2 (`cpu_level`): the game
     runs on the PC that builds it, and a generic build would do every fused multiply-add of the game in software.
3. **Prepare the Shaders**: make_game.py translates every shader of the game, in its default bind state,
   into the game's shader cache and builds its stage library or compute pipeline into the driver's own cache
   (`tools/prepare_shaders.py`), so play stops for them far less often. Variants met in play with another bind state are
   still translated then, full pipelines are still linked in play (quickly, from the cached parts), and the driver's
   cache is kept per executable and is lost when the driver clears it. Its bar
   follows the game's own counts, printed every 2 s: the translation fills the first half, the build into the
   driver's cache the second. An update every 10 s keeps the step's text current while nothing is printed, and after
   3 minutes without a count moving it says so, and that the step is still running.
4. **Draw the Button Prompts**, only when the version holds button art (`app/<version>/prompt-art`, a release made
   with `--prompt-art`; `build.has_prompt_art`): make_game.py's last step runs `tools/pad_prompts.py` on that art
   and writes the Xbox, PlayStation, Steam Deck and keyboard and mouse prompts into `romfs-patches-*` beside the game (about
   30 s), which the game's Controls > Button prompts loads. The keyboard and mouse prompts show the keys of
   `nx2navi.ini`'s `bind_` lines, so Rebuild the Game after rebinding keys draws them again; the step's key names
   all it reads (the RomFS, the art, those lines) and the version of the tools (`make_game.py` `PROMPT_TOOLS`, changed
   by hand when a change to them changes what they write: an edit of a script alone does not draw the prompts
   again), so it runs again only when one changed or a folder is gone, and runs no other step again. Without art
   the row is not shown and the game keeps its own prompts.
5. **Check the Game Files**: make_game.py's last step runs `tools/romfs_check.py`, once for each copy of the game data
   (about 4 minutes on players' PCs up to 0.7.1, in worker threads, never while the game runs; its key holds the
   check's version, `SCAN_TOOL`, in the same way). It decompresses every file, writes
   `romfs-check.json` beside the game, and for each damaged texture puts a stand-in, another texture of the same kind
   under the damaged one's name, in `mods/Damaged file stand-ins`, so the game can load what needs it. A damaged file
   of any other kind is only reported: only a clean dump restores it. In play the game logs a file it keeps opening
   and cannot load (runtime/romfs.c `open_note`). The launcher shows what the check found (`jobs.Job.note_scan`):
   the count in the step's row ("4 Flagged, 3 Stand-Ins"), and after a job in which the check ran a sentence on the
   home screen and in `launcher.log`. The four files the check flags on every copy of the game (`jobs.SCAN_EXPECTED`)
   are told as that, with nothing for the player to do; other files are counted and `romfs-check.json` named.
6. The game's Cetra Display is decoded for the window.

## Updating

A release carries `NX2navi-update.json` (version, notes, and the size and sha256 of each file). The feed
(`app/<version>/feed.json`) names a GitHub repository: the launcher reads its newest release (or pre-release, on the
Preview channel) that has the manifest. A private repository answers only with a token (`github_token` in
`data/launcher.json`); players need a public one. `feed_repo` in `data/launcher.json` points one launcher at another
repository for testing; `{"type": "url", "url": ".../NX2navi-update.json"}` in feed.json reads one manifest
directly. Every address is https (plain http only to this PC), a redirect to http is refused, and the token is not
sent on when a redirect leaves the host it was meant for (`launcher/net.py`). A manifest that does not give a file's
size and sha256 is refused.

GitHub allows an address 60 such requests an hour. The Updates row and F5 therefore start no check within a minute
of the last one (`FORCED_CHECK_PAUSE`), and a check GitHub refuses says when the allowance is whole again, from the
answer's `X-RateLimit-Reset` (`feed.retry_text`). A failed check is one line in `launcher.log`.

### In the background

The player never waits for a download and play is never interrupted. A check that finds a newer release (the check
at start, then one an hour after the last, also while the window is hidden during play: `jobs.RECHECK`) stages it
at once, without a screen (`jobs.StageJob`, `updater.stage`):

- The app archive is downloaded and checked as in step 1 below and unpacks into `app/<version>` as in step 2. A new
  Python unpacks into `app/<version>/python.staged`, not `python.next`: the stub swaps `python.next` in at every
  start, which would put the running version on the new Python. `app/<version>/staged.json`, written last, marks the
  release complete (`updater.staged_info`); a folder without it, or one that lost its Python, is never installed and
  is staged again. Nothing is switched and the launcher keeps running. The thread runs in Windows' background mode
  (lower processor, disk and memory priority), so it shares only the network with the game.
- A staging that fails is logged in `launcher.log` and tried again after the next check; it never shows an error.
  Selecting Updates then downloads the release with its steps on screen, as below.
- The Updates row reads "Downloading <version>", then "Ready · <version>". A staged release survives a restart of the
  launcher, which finds it without a check (`updater.staged`). A pre-release counts only on the Preview channel; the
  version the stub last rolled back from (`rolled_back` in `data/launcher.json`) is neither staged nor installed by
  itself; a staged release newer than the feed's newest is passed over, as the release was withdrawn. Staging a
  release removes the staged one it supersedes, and `updater.prune` keeps a staged release newer than the current
  version.

`jobs.BackgroundUpdate` installs a staged release only while no copy of the game runs (`build.game_running`, and the
process the launcher started), no job holds the window and no staging is under way:

- by itself, with Install Updates Automatically on (Settings; the default): when the launcher starts, right after the
  game exits (the launcher comes back from hiding), and, checked every minute, once a copy of the game started
  elsewhere that held the install back has closed;
- when the player selects Updates, also with the setting off; selected during the download, it installs as soon as
  the download is done.

Play always starts the game: a release staged meanwhile installs after the game exits. Installing
(`updater.install_staged`) moves `python.staged` to `python.next`, switches `app/current.txt` as in step 3, deletes
`staged.json` (so a version the stub rolls back from is not installed again) and restarts the launcher with
`--continue-update`; the new version finishes in `jobs.ContinueUpdateJob` (steps 4 to 9). A checkout runs from the
repository: it stages and installs nothing.

### Steps

Without a staged release (its staging failed, it is the version the stub rolled back from, or the launcher runs from a
checkout), selecting Updates runs `jobs.UpdateDownloadJob`, before the restart (`launcher/updater.py`):

1. **Download** `NX2navi-app-<version>.zip` (about 3 MB plus the shader translator) to `data/downloads`, resuming a cut
   download, and check its size and sha256. When the manifest names a Python other than `python/runtime-id.txt`, also
   `NX2navi-runtime-<id>.zip`, unpacked into `python.next`.
2. **Verify**: the archive unpacks into `app/<version>.part`, refusing names that leave the folder, holds the expected
   `VERSION` and a launcher, and becomes `app/<version>`.
3. `app/previous.txt` gets the running version, `app/current.txt` the new one, and the launcher starts the stub with
   `--continue-update <old>` and closes.

The stub swaps `python.next` in while no Python runs, then starts the new version, which finishes in
`jobs.ContinueUpdateJob`:

4. **Update Build Tools**: only when the new release pins other packages.
5. **Translate the Code** and **Compile Changed Files**: `make_game.py --rebuild`. Most updates compile almost
   nothing:
   - The exefs and romfs steps depend only on the game files: they are kept, and the dump is not needed again.
   - The lift runs only when the lifter, its patch table or the bring-up fixes changed.
   - The runtime comes prebuilt with the release (`app/<version>/lib/libnx2navirt.a`, compiled by
     `make_game.py --make-runtime-lib` with the release's own compiler). Only the few runtime files that include a
     header written per install (`layout.h`, `patches.h`, `romfs_config.h`) are compiled on the player's PC.
   - Every other file compiles only when its source, a header it includes, the flags or the compiler changed
     (`build-cache/obj/objects.json`, from clang's dependency files). The kit's folder is not part of that key, so the
     objects of the version before are reused although each version lives in its own folder. The compiler counts by
     its version and size, not its file time, and the faster CLANG64 compiler, whose objects are the same, not at all.
     The lifted code includes only `cpu.h` and `simd.h`: a change there compiles it all again, which is rare.
   - An object an earlier version recorded under its own key form is taken over when its compile and its files are
     the same (`ObjectCache.adopt`): 0.6.0's keys, and 0.5.0's, which named the kit's folder. An update keeps the
     x86-64 level the game's code was compiled for (`build-cache/obj/lifted-build.json`; generic for the lifted
     objects of 0.5.0 and earlier); Rebuild the Game compiles it for this processor's (`--retune`), and runs the
     compile step for that even when nothing else changed (`retune_due`).
6. **Swap In the New Build**: lld links `ocarina.exe.new`, which replaces `ocarina.exe` only once it is complete. A
   failed rebuild leaves the old build playable; Rebuild the Game tries again. A rebuild that lifted again and then
   stopped has no finished compile on record (`build.built_version` is empty), but the old exe and the unpacked game
   files are there: the launcher still counts the game as installed (`build.installed`), opens on home, where
   Rebuild the Game reads "Not Finished", and never asks for the dump again. Begin Installation into a folder whose
   game files are already unpacked builds on from them as well (`jobs.InstallJob.args`).
7. **Prepare the Shaders** is skipped once shaders were prepared before (`shader-prepared.bin`): the game keeps using
   them and translates them again with the new translator in the background (`NX2NAVI_SHADER_CARRY`). With
   `shader_carry = 0`, or nothing prepared yet, the step prepares them all as at the install. So it does when the
   list was prepared from another shader seed than the one this kit writes: `shader-prepared.bin` carries the digests
   of the two seed files and the translator identity (`runtime/prp_file.h`), the step writes the seed first
   (`prepare_shaders.py --no-run`, seconds) and compares (`make_game.py` `seed_changed`); a list from before it
   recorded its seed is prepared again once. Another translator alone does not prepare again. The step still builds
   the prepared shaders into the graphics driver's cache again (`make_game.py` `warm_driver_cache`): seconds while
   the driver has them, 20 minutes on a player's PC when the update changed what they are built with. Its bar
   follows that count from 0 to 100 %, and the Updates row says an update takes from a few minutes to half an hour
   or more (`launcher/ui/app.py` `UPDATE_TAKES`).
8. **Draw the Button Prompts**, with button art in the new version: only when the art, the prompt tools or the key
   bindings changed, or a prompt folder is missing (a first version with art draws them all).
9. Every version but the current and the previous one is deleted, except a staged release newer than the current
   one.

When the new version fails to start (exit code 86 from `start.pyw`, or any error within six seconds), the stub sets
`current.txt` back to `previous.txt`'s version and starts it with `--rolled-back`. `start.pyw` writes the failure to
`data/logs/crash.log` and shows it in a message box only when there is no version to go back to, when Steam started
it, or when the six seconds are over (`quiet_exit`): a box holds the exit code back until it is closed, and the
broken version then stayed the current one. The game is never rebuilt while it
runs. Keep Build Cache off deletes the lifted C and the objects (about 1.5 GB) after each build but keeps
`build-cache/state.json`, so updates still rebuild without the dump (compiling everything).

## Sharing a save

A bug report from a place in the game the developer has not reached yet is quickest to reproduce with the reporter's
save. Saves (home, under Game Folder; `launcher/saves.py`) and the Saves rows of the game's settings menu (Game tab;
`runtime/save_share.c`) write and read the same zip.

**Export.** Export Save writes `NX2navi-save-<date>_<time>.zip` into the Downloads folder (Export To chooses another
folder) and opens the folder with the zip selected, ready to drag into a bug report. With Add Log and Settings on (the
default) it is `...-bug-report.zip` and also holds the game's latest log and its `nx2navi.ini`; while the game has no
save yet (it stopped before its first one) the launcher then writes `NX2navi-log-<date>_<time>-bug-report.zip` with
those two alone, which an import refuses as a zip without a save. In the game, Export
save and Export save for a bug report write the same two zips into Downloads; the second holds the log of the run in
progress. The zip:

```
manifest.json   format "nx2navi-save", format_version 1, the game, its title id and version (1.1.1), the NX2navi
                version, the export time (UTC), exported_by (launcher or game), the number of save files, bug_report
save/           the save the game would load next (a finished commit still waiting in profile/save-tx, else
                profile/save/<account>): account.sav, option.sav, slot_00 to slot_05
bug-report/     nx2navi.log and nx2navi.ini, in a bug report
```

Key files (any `*.keys`, also one left in a save folder) and game files never go in.

**Import.** Import a Save takes such a zip: put it into the game folder's `saves` folder (the row then reads "From the
Saves Folder", and the home's Saves row "Import Waiting") or select the row and choose it. Before anything changes it
checks the manifest (the format, a format version it knows, the game's title id: a newer NX2navi's zip or another
game's save is refused), every path (inside the save, no links), the size (4,000 files, 256 MB), that the save is
this game's (`account.sav`, `option.sav` or a `slot_NN/progress.sav`), and each file's CRC as it unpacks. Then the
zip's save replaces the whole save, every slot, as a save folder does (below): the save the game would load now goes
to `saves/backup <date time>` first. It refuses while the game runs. A zip taken from the saves folder moves to
`saves/imported <date time>`.

The game's menu imports at the next start instead of at once: the game keeps its progress in memory and writes it back
at its next save, over files replaced under it, and the save's working copy is made when the game mounts its save.
Import a save checks the zip as the launcher does and puts its save in `profile/save-import`; the row then reads "at
next start", and Cancel the import drops it. The next start (the menu's Restart, or quitting and starting again)
copies the current save to `saves/backup <date time>` and makes the import the save before the game reads it
(`runtime/fs_host.c` `import_pending`), and the menu says where the old save went. An import in the launcher replaces
one still waiting from the menu.

## Mods

Mods (home, under Saves; `launcher/mods.py`) is where the player puts the mods they download. The game reads the game
folder's `mods` folder (`runtime/mods.c`; `mods_dir` in nx2navi.ini names another): one folder per mod, holding a
`romfs` folder with the game files it replaces, an `exefs` folder with code patches (`.ips`, `.pchtxt`) or a `cheats`
folder, either at its top or in the Atmosphere layout (`atmosphere/contents/0400486024DEC000/romfs` and the others
`tools/mod_files.py` lists).

A mod goes in as it came. Play first puts the folder in order (`mods.prepare`): an archive (`.zip`, and `.7z` or
`.rar` through Windows' own `tar`, which reads them on Windows 10 and 11) is unpacked into a folder of its name unless
that folder is there already, and the archive stays; a mod folder whose files sit one or more folders down (an archive
that held the mod's own folder) gets them moved up; a `romfs`, `romfslite`, `exefs` or `cheats` folder put straight into
the mods folder becomes a mod of its own, *Unnamed mod*. An archive that would write outside its folder is refused.
Nothing is deleted. Add a Mod and Add a Mod Folder do the same for an archive or folder the player picks, which is
unpacked or copied into the mods folder under a free name; Open Mods Folder opens it.

Each mod has an On/Off row; its description says what it changes (how many game files, code patches, cheats), which
other mod changes the same files and which of the two wins, and why the game leaves it off when it does: a code module
(`subsdk*`), a download with options to pick from (several folders that each hold a mod), a mod for another game or
another version, or nothing the game reads. Load Order sets which mod wins a file two of them change: the higher one.
The switches and the order are the game's own settings, `mods_disabled` and `mods_order` in nx2navi.ini, written as the
game writes them; they apply at the game's next start, so they wait while it runs.

**Mods that change the game's code.** The game is lifted from the console's code ahead of time, so code cheats and
ExeFS patches cannot be applied while it runs: the build turns them into patch sets of the lifted code
(`tools/cheats.py`, `tools/exefs_patches.py`) that the game switches on with their mod, and the Cheats tab of the
game's menu switches each cheat. The lift step's key names those files, so a build lifts and compiles again when they
change (only the changed parts compile, and the shaders stay prepared), and `build-cache/mod-code.json` records the
files it took up. When an enabled mod brings code no build has seen, Play builds the game again first (a few minutes;
the home's Mods row says "Build Waiting") and starts it after. A Play from Steam then opens the window for that build.
Code mods are built from the game folder's own `mods` folder only.

## Importing a save folder

Import a Save Folder (Saves) makes another save the game's own. A save put into the game folder's `saves` folder (its
files, or the folder that holds them) is taken first by Import a Save; otherwise this row asks for the folder. It
recognises a Switch save backup (the save's files), an emulator's save folder (nested folders named by user and title
ids), and another NX2navi game folder or its profile folder (the account folder inside). A folder with `progress.sav`
at its top is one save slot: it replaces that slot (`slot_00` to `slot_05`, by the folder's name, else `slot_00`) and
the rest of the current save stays. A folder with more than 4,000 files or 1 GB is refused as picked by mistake.

The game keeps its save in `profile/save/<account>`, named after `profile/account.uid` (made as the game would when
it never ran). The import copies the save the game would load now (a finished commit still waiting in
`profile/save-tx`, else the account folder) to `saves/backup <date time>`, puts the new files in place and empties
`profile/save-tx`, so the game loads the imported save. A save taken from the saves folder moves to
`saves/imported <date time>`. It refuses while the game runs.

## Downloads

Every download (`launcher/net.py`) comes over HTTPS and is checked against a sha256. A server's certificate is checked
first by Python's OpenSSL against the certificates in Windows' stores and, when that refuses it, by Windows' own
certificate check (the truststore package of the bundled Python; not under Wine, whose crypt32 passes a chain that
stops at an issuer it does not know). OpenSSL sees only the roots the PC already holds, and it takes an expired
cross-signature still in a store before the server's valid one ("certificate has expired" on a valid chain); Windows
weighs every chain as its own programs do and downloads a missing root from Windows Update. A certificate both refuse
stops the step with what each check said, the PC's clock against the server's (the Date of a plain-HTTP request to
it), and what to check: the date, time and time zone, or security software that scans encrypted connections.

Downloads go through a proxy when the PC names one: the launcher's opener (`launcher/net.py` `_open`) keeps Python's
default proxy handling, which reads `HTTPS_PROXY` and `HTTP_PROXY` (and `NO_PROXY`) from the environment and, without
them, the proxy set in Windows' Internet settings. Only HTTP proxies work; a SOCKS proxy does not. An address that needs
a user name and password takes them in the variable, as in `HTTPS_PROXY=http://user:password@host:port`.

## Uninstalling

Settings > Uninstall NX2navi, or Settings > Apps > NX2navi (which runs `NX2navi.exe --uninstall`): what the launcher
put in its folder (`NX2navi.exe`, `python`, `app`, `pydeps`, `data`, `toolchain`; `launcher/winsys.py` `OWNED`), the
toolchain in `C:\NX2navi` when it went there, the shortcuts and the Apps entry go; the launcher's folder itself goes
only when that leaves it empty. The built game and its saves stay unless "Remove the Built Game" is On, also when the
game folder is the default `<launcher>\game`. The dump is never touched.

## Command line

`launcher/cli.py` runs the same jobs without the window (`python -m launcher.cli`, or in an installed launcher
`python\python.exe app\<version>\launcher\cli.py`): `check`, `update`, `tools`, `install --dump F --game D`,
`rebuild [--game D]`, `fonts`. `NX2navi.exe --desktop-shortcut on|off` and `--shortcuts on|off` are the stub's own.
`NX2navi-Setup.exe` takes `--dest DIR`, `--no-shortcuts`, `--no-launch` and `--quiet`.

## Releases

1. Bump `VERSION`; build the shader translator (`tools/build_xlate.py`).
2. With Python 3.14, numpy (the icon is drawn with it, `launcher/ui/art.py`) and uv, on Windows (the prebuilt runtime
   needs MSYS2's own clang; on Linux add `--wine` to run it through Wine, else the release ships without it and
   players compile `runtime/` themselves):

   ```powershell
   uv run --no-project --python 3.14 --with numpy tools\make_launcher.py --xlate build\xlate --ffx amd_fidelityfx_vk.dll --feed owner/name --notes notes.md
   ```

   `--ffx` puts AMD's signed `amd_fidelityfx_vk.dll` (FidelityFX SDK 1.1.4) in the release for FSR 3.1.
   `--ffx4 <folder>` puts AMD's signed `amd_fidelityfx_loader_vk.dll` and `amd_fidelityfx_upscaler_vk.dll` (FSR SDK
   2.4.0, the files of its `Kits/FidelityFX/signedbin/`) in the release for FSR 4; without it the menu offers no FSR 4.
   `amd_fidelityfx_framegeneration_vk.dll` goes along when it is in that folder (`fsr4_fg`).
   NVIDIA's `nvngx_dlss.dll` goes in by itself: the script downloads the version `tools/dlss_pin.py` pins from
   NVIDIA's DLSS SDK repository (cached in `toolchain/launcher-dl/`), checks its size and sha256, and puts it in
   `bin/` with `nvngx_dlss-NOTICE.txt`; `make_game.py` copies both beside the game. `--dlss FILE` takes a copy on disk
   instead (it must be the pinned file), `--no-dlss` leaves DLSS out. NVIDIA's RTX SDKs License lets an application
   ship the library; it also asks that a commercial release be announced to NVIDIA first
   (https://developer.nvidia.com/sw-notification) and that the use of DLSS be credited.

   `--dlssg nx2navi_dlssg.dll` (built by `tools/build_dlssg.py`, which needs Visual Studio's C++ compiler) adds DLSS
   Frame Generation in `bin/dlssg/`: beside it NVIDIA Streamline's runtime (`sl.interposer.dll`, `sl.common.dll`,
   `sl.dlss_g.dll`, `sl.pcl.dll`, `sl.reflex.dll`) and `nvngx_dlssg.dll`, taken from the Streamline SDK release that
   `tools/streamline_pin.py` pins. The script downloads that release once (cached in `toolchain/launcher-dl/`), checks
   the archive's size and sha256 and each file's sha256, and writes `dlssg-NOTICE.txt` with the two licence texts;
   `make_game.py` copies the whole set beside the game. `--streamline SDK` takes an SDK folder on disk instead (its
   files must be the pinned ones).

   `--prompt-art FOLDER ...` puts the button art in the release (`app/<version>/prompt-art`): folders that hold
   `XGamepad`, `P5Gamepad` and `Keyboard_Mouse` (the Source folders of button prompt art packs), or those three
   folders themselves; a set named itself wins over one found inside a folder named, and the same set twice is
   refused. Only the pictures `tools/pad_prompts.py` reads are copied (`tools/prompt_art.py`
   `art_files`: the pad pictures it names and every key of `Keyboard_Mouse/Dark` and `/White`, about 280 PNGs and
   1 MB), and it stops first naming any that are missing. The art goes into the release only, never into the
   repository (`.gitignore` has `/prompt-art/`). Without it the release has no art and the build draws no prompts.
   `tools/make_kit.py` takes the same option for the build kit.

   On Linux it cross-compiles the two programs with clang, lld and llvm-windres against the same MSYS2 packages and
   unpacks Tcl/Tk with msiextract (msitools). It writes into `build/launcher-release`: `NX2navi-Setup-<version>.exe`
   (the setup program with the launcher folder appended as a zip), `NX2navi-app-<version>.zip` (with the prebuilt
   runtime),
   `NX2navi-runtime-<id>.zip` and `NX2navi-update.json`. `check_content` runs over the folder first.
3. Upload those four files to GitHub release `v<version>` of the feed repository, by hand or with `--publish`
   (`gh release create`; add `--draft` to look before it goes out). Launchers see it at their next start.

The release resolves the MSYS2 packages of the day into `toolchain.lock.json` (`--lock` reuses an older lock). The
repository keeps old package files, so a release keeps building with the compiler it was tested with.

## Tests

`python tests/launcher_test.py` (Python 3.14): versions, settings, the toolchain resolver and installer against a
local fake repository, resumed and checked downloads, the certificate checks against a local HTTPS server (its
certificates made with the `openssl` command), the GitHub and URL feeds, staging, switching, rollback and
pruning, unsafe archives, the background update (a release staged beside the running version without switching, the
`staged.json` marker, a staged release installed after a restart, no install while the game runs, the install once
it exited, nothing by itself with Install Updates Automatically off, a failed staging tried again after the next
check), the font decoding on a system font wrapped like the game's, the look (every colour scheme complete, the
Custom scheme's picked colours, colour square and typed hex code, the background picture cut to the shape below the
title band and kept, never stretched, the sound effects as WAVs and low by default), `make_game.py`
on a synthetic
`.nup` (events, skipped steps, `--rebuild` without the dump, step keys, the shaders key following the translator the
build uses), the RomFS step beside the lift and compile
and its failure stopping the build, the prompts step with a stand-in prompt tool (when it runs again, and that it
makes no other step run), the button art a release copies and the packages and tools the prompts need in a release,
the compile step's object cache with the system's C compiler and its
largest-first order, its windres call under folders with spaces in their names, CLANG64's clang in the lock and the
install, the save import (the account folder, the save folders it recognises, backup, the commit journal, the saves
folder, refusals), the shared save's zip (export of the save the game would load, the bug report files, no key
files, the import of a zip with its backup, a zip in the saves folder, the menu's waiting import dropped, the zips it
refuses: no manifest, another format or game, a newer format, paths outside the save, links, damage), the install
job with a fake
`make_game.py` (with and without the prompts row), and the window's selftest when a display is there.

What goes wrong around a build is tested the same way, on made-up folders and stand-in processes: an update that
stops in its compile (the game stays installed, home says how to finish, the install goes on from the unpacked
files), `start.pyw` on a version that cannot be imported (exit code 86 at once, no message box, with a version to go
back to), a cut RomFS image and the free space, the object cache written while the compile runs and kept when it is
cut, a RomFS step that does not repeat the lift, the update's duration text and the warm-up's bar, a damaged dump
named by its file and an unexpected error by its message, the file check's result in its row and on home, the
game's FATAL line after exit codes 70 and 71, the job count by free memory and the build's priority class, a line
handler that raises and a log that cannot be written, one forced update check a minute and GitHub's reset time, a
game folder with characters outside the Basic Multilingual Plane (its `modules.c` compiled when a C compiler is
there), the file check at both piece sizes against the check as it was, the disk space of every step and the build
tools' test compile, and the prompt tool's unpacking. Two tests record the logic of `tools/romfs_check.py` and of
the three prompt scripts (their code without comments and docstrings) beside `SCAN_TOOL` and `PROMPT_TOOLS`: they
fail when that logic changes, to ask whether the step must run again on every install.

## Requirements and limits

- Windows 10 or 11, x86-64; the build's own requirements (README-BuildKit.md): 16 GB of RAM, about 34 GB free for
  the game and 1.5 GB for the launcher and its tools, a Vulkan 1.3 GPU.
- The programs are not signed: SmartScreen warns on the first start of the setup.
- English only for now.
- The launcher builds the Windows game only; Linux builds use `docs/building.md`.
