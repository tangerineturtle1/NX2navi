# NX2navi → Triforce: the launcher handover

NX2navi and Triforce have merged and go on as Triforce. This package is one NX2navi release that turns every
installed NX2navi launcher into the door to Triforce's launcher, through NX2navi's own update channel. Nothing new
has to be installed by hand, and nothing of NX2navi is removed from a player's PC.

It is made for whoever publishes NX2navi's releases (`tangerineturtle1/NX2navi`).

## What to do

1. Look at it first: the pictures in `preview/`, and the code in `bridge/launcher/` (four small Python files).
2. Upload the two files in `release/` to a new GitHub release **`v0.12.0`** of `tangerineturtle1/NX2navi`:
   - `NX2navi-app-0.12.0.zip`
   - `NX2navi-update.json`
3. Publish it as a **full release, not a pre-release**. Launchers on the Stable channel pass pre-releases over.

That is all. Every launcher sees it at its next check (when it opens, and every hour while it runs), downloads the
3.3 MB archive, checks its size and sha256 against the manifest and switches to it, exactly as for any update. With
Install Updates Automatically on (the default) that happens by itself while the game is not running.

`0.12.0` is newer than everything published as of 2026-11-14 (Stable `0.10.1`, Preview `0.11.0-rc4`). For another
number, see "Another version" below. The number must be plain (`0.12.0`, not `0.12.0-rc1`).

## What a player sees

After the update the launcher restarts, and instead of the old window this one opens (`preview/`):

- **Get Triforce** starts Triforce Setup, Triforce's launcher, which installs itself per user as NX2navi's did and
  takes the player through building the game from their own files. When Triforce is already on the PC the button
  reads **Open Triforce** and starts the installed one.
- **Play NX2navi** starts the game that is built on the PC (`ocarina.exe` in the game folder), as the launcher's
  Play did. It is there for as long as the player wants it. It is not shown when no game was built.
- **Your game files** and **Your NX2navi game and saves**: the two paths from the launcher's settings, each with
  Copy path, because Triforce Setup asks for the game files and its Convert saves page takes the saves from the
  game folder.
- Links to triforceproject.com and the Discord.

The NX2navi shortcuts keep working and open this window. A Steam shortcut made by Add to Steam opens it too.

## What it touches, and what it does not

- **Not touched:** the built game, the saves in it, `data/launcher.json`, the build tools, the mods, the dump. The
  old launcher version stays on the disk as `app/<old version>` (the stub's rollback needs it).
- **Written:** `app/0.12.0/` (the archive), `app/current.txt` and `app/previous.txt` (by the old launcher, as for any
  update), `data/logs/handover.log`, `data/handover.json` (the time of the last update check), and, when the player
  selects Get Triforce, a copy of `TriforceSetup.exe` in `%TEMP%\TriforceSetup\`.
- **Uninstalling NX2navi** (Windows' Apps list, which runs `NX2navi.exe --uninstall`) still works: the window asks,
  then removes the shortcuts, the Apps entry and the same files and folders the launcher's own uninstall removed
  (`winsys.OWNED`); the built game goes only on a second, separate Yes.

## Safety

- **A handover that cannot start rolls back.** `start.pyw` keeps the stub's contract: a failure before the window is
  up ends the process with exit code 86 inside the stub's six seconds, and the stub goes back to the version before
  and starts it with `--rolled-back`. That launcher then never installs this version by itself again
  (`rolled_back`), as with any failed update.
- **The way back stays open.** Once an hour at most (and only while Check for Updates is on), the window asks the
  same feed the launcher asked (`feed.json` of the version before; `feed_repo` in the settings still overrides it)
  for a version newer than its own. When there is one it stages it the launcher's way: checked download,
  `app/<version>`, a `runtime` the manifest names into `python.next`, then `previous.txt` and `current.txt`. So a
  later NX2navi release, say `0.12.1`, replaces this window on every PC: a fixed handover, or a launcher again.
- **The Triforce Setup it carries is checked.** `launcher/triforce/setup.json` holds its size and sha256
  (Triforce `v0.3.6`, sha256 `59fba57a…e7eb2e`, the file published at triforceproject.com); the window compares
  before it starts it and starts nothing that differs. Setup updates itself from triforceproject.com when it
  starts, so the carried copy does not have to be the newest.
- Only the standard library and tkinter are used, so it runs on the Python each installed launcher already has: the
  manifest names no `runtime`, and no launcher downloads a Python for it.
- No game data and nothing derived from the game is in the archive.

## Looking at it before it is released

Pictures without showing a window or doing anything, from any installed launcher's Python:

```
python\pythonw.exe <unpacked NX2navi-app-0.12.0.zip>\launcher\start.pyw --selftest --shots <folder>
```

A private run through a real launcher: put the two release files in a release of another repository and set
`"feed_repo": "owner/name"` in that launcher's `data\launcher.json`, or serve them on the same PC and put
`{"type": "url", "url": "http://127.0.0.1:8000/NX2navi-update.json"}` in its `app\<version>\feed.json`.

## Another version, another Setup, other notes

Python 3.8 or later, no packages:

```
python make_handover.py --version 0.12.0
python make_handover.py --version 1.0.0 --setup TriforceSetup.exe --setup-version v0.3.7 --notes notes.md
```

It packs `bridge/` and writes `release/NX2navi-app-<version>.zip` and `release/NX2navi-update.json`. The window's
pictures are in `bridge/launcher/art/`; `tools/make_art.py` draws them again (Pillow and numpy) from `tools/art-src/`.

## What was tested, and what was not

`tests/e2e.py` (run 2026-11-14, all passed) copies an installed NX2navi launcher (versions 0.10.1 and 0.9.2 were on
the PC) and lets its own code and its own `NX2navi.exe` do the work against a feed on 127.0.0.1:

- `cli.py check` and `cli.py update` of both versions see, download, check, stage and switch to the handover;
- the path the window takes in the background (`feed.latest`, `updater.stage`, `staged_info`, `install_staged`);
- the real stub starts the handover with `--continue-update`, and nothing is rolled back;
- a handover that cannot start: exit code 86, and the stub goes back to the version before;
- a GitHub-shaped feed: offered on Stable as a full release, passed over as a pre-release;
- the way back: the handover stages a newer release, and refuses a file that does not match its sha256;
- the uninstall's plan is the same command line the launcher's own `winsys` builds for the same folder;
- the window drawn with NX2navi's own Python (3.14.9, Tk 9.0.4) at 125 % display scale.

Not tested: a release on the real GitHub repository; the 0.11.0 previews' launcher as a running program (in the
0.11.0-rc4 source, `feed.py`, `updater.py`, `versions.py`, `net.py`, `settings.py` and `start.pyw` are the same files
as 0.10.1's, byte for byte, and `cli.py` differs by six added lines); a person clicking through the window on a PC that has
an NX2navi game; Windows 10; display scales other than 125 %; the Linux AppImage, which has no launcher updater and
is not reached by this at all.

## For players who are new

This reaches installed launchers. `NX2navi-Setup-<version>.exe` on the old releases still installs the old launcher,
which then updates to the handover at its first check. The download links for new players should point at
https://triforceproject.com instead.

## Before publishing

Triforce Setup's Convert saves page finds NX2navi's saves by itself from the Triforce release after `v0.3.6` (in
`v0.3.6` the save folder is picked by hand there). The window tells players the page brings their saves over, so
publish this once that Triforce release is out. Nothing in this package has to change for it: Setup updates itself.

Triforce is free software under the GPL-3; its source, Triforce Setup's included, is at
https://git.triforceproject.com/triforce/source

## In the repository

This folder is the package's source. Three parts of the package are not kept here, and `make_handover.py` names the
one it misses when it is run without them:

- `bridge/launcher/art/`, the window's pictures: `tools/make_art.py` draws them from `tools/art-src/`.
- `bridge/launcher/triforce/TriforceSetup.exe`: `setup.json` beside it names the file release 0.12.0 carries (its
  Triforce release, its size and its sha256). Put that file there, or give another with `--setup`.
- `release/`, which `make_handover.py` writes. The archive published as release 0.12.0 is `NX2navi-app-0.12.0.zip`,
  3,333,988 bytes, sha256 `45c9c50d98fa4771c6d7330e8fa2353c9db1e8b65e3b37ef14174fb4b57e1793`.

`tests/e2e.py` ran again on 2026-11-14 against the launchers of 0.9.1, 0.9.2, 0.10.0, 0.10.1 and the 0.10.2 and
0.11.0 previews: all of its checks passed. The pictures the self-test takes with the window off the screen came out
empty on that PC (150 % display scale) while the window itself drew as in `preview/`, and the test does not look into
the pictures: look at them yourself. Release 0.12.0 was published on 2026-11-14 with Triforce `v0.3.6` as its newest
release, before the one "Before publishing" waits for: in `v0.3.6` the Convert saves page asks for the save folder.
