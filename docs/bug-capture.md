# Bug captures

A bug capture records where and how the game was when a player saw something wrong, so that the bug can be looked for
again at that place instead of somewhere else. The player presses **F8**; a test run then starts from what the press
wrote. Code: `runtime/bugcap.c`, `tools/capture_run.py`. Ocarina of Time 1.1.1 (the place and the camera come from the
debug tools, `runtime/debug_tools.c`); on other game versions a capture still has the picture, the timing, the
settings and the save.

## For the player

When you see a bug, press **F8** while the game window is in front. About a second later a note at the top of the
screen says *Bug capture 3 saved* with the folder's name. Tell whoever looks into the bug the number ("capture 3") and
what you saw; nothing else is needed. Press it as often as you like: each press is its own capture, and a press while
the one before is still being written is skipped.

The captures are in the game folder under `logs\captures`, one folder each, named by number and time
(`003-20261113-141502`). *Menu > Open log folder* shows `logs`. A capture is a few megabytes, most of it the picture (about
9 MB at 3440x1440 with HDR); delete old ones whenever you like. Nothing leaves your PC: a capture is files on your disk.

## What a capture holds

| File | What |
|---|---|
| `capture.txt` | Written last: a folder that has it is complete. `key = value` lines under `[sections]`, see below |
| `frame.png` | The frame on screen as the window got it, the overlay included, without the capture's own note (an HDR picture converted to sRGB) |
| `frames.tsv` | The last 600 presents before the press and the 30 after: frame, interval and replay time in ms |
| `log-tail.txt` | The last 256 KB of the run's log |
| `settings.ini` | Every setting that had a value (from `nx2navi.ini`, the menu or the environment) as `nx2navi.ini` lines, without the private ones (`runtime/cfg_private.h`) |
| `NX2navi-save-<time>.zip` | The save the game would load next, as *Menu > Export save* writes it |
| `flags.tsv`, `texts.tsv` | The game's on/off flags and its 64-character texts, by the hash of their names |
| `flight/` | With `flight = 1`: the flight recorder's dump of the same moment (`tools/flight_view.py` reads it) |

`capture.txt`:

- `[capture]`: `number`, `name`, `time`, `time_utc`, `trigger` (`F8`, `NX2NAVI_CAPTURE_AT`, `replay`), the NX2navi and
  game versions, the presented `frame`, `run_seconds`, `menu_open`.
- `[place]`: `area` (the game's own name, which says child or adult for the outdoor areas: `HyruleField_Adult`),
  `player_pos` in metres (`none` at the title screen and while loading), `player_rot` (Link's 3x3 rotation by rows) and
  `player_yaw`, and `nearest_start`, the area's start position nearest to him, when the list of start positions was
  read (the debug menu or a test reads it).
- `[camera]`: `camera_pos`, `camera_dir`, `camera_yaw` and `camera_pitch` (radians; the camera looks along
  `(cos pitch sin yaw, sin pitch, cos pitch cos yaw)`, the free camera's convention) with `camera_source`: the frame on
  screen when DLSS, DLAA or FSR runs (then also `camera_fov_y_degrees`, `camera_aspect`, the near and far planes),
  else the free camera when it is on, else the game's own camera. `game_camera_pos` / `game_camera_target` and
  `event_camera_pos` / `event_camera_target` are the game's two scene cameras as its camera update chose them in the
  frames after the press, whatever was drawn.
- `[picture]`: the window's size, the game image's size, HDR, the scaler, the present mode, the display's rate.
- `[timing]`: over the presents before the press: the average frame rate, the median, 95th and 99th percentile and
  the longest frame time, how many frames took over 33 and 50 ms, the last 30 intervals before and the first after,
  and the performance overlay's numbers when it was on.
- `[system]`: OS, CPU, RAM, GPU, driver.
- `[settings that differ from the defaults]`: the settings whose value is not the default; a private one shows only
  that it is set.

Position, rotation, area and the camera of the frame on screen are read on the render thread at the press, so they
belong to the frame the timing ends with; the picture is the next present.

## Starting a test run from a capture

```
python tools/capture_run.py 3
python tools/capture_run.py "C:\...\game\logs\captures\003-20261113-141502" --set FSR4=0 --hold 30
```

The argument is a capture's folder or its number under the installed game's `logs\captures` (`--game` names another
game folder). The script

1. refuses to start while a game is running (one game at a time);
2. makes a throwaway profile: the game's own profile without its saves, with the capture's save as a waiting import,
   which the game makes the save when it starts;
3. starts the game with `NX2NAVI_CAPTURE_REPLAY=<the capture's folder>`, in a window of the capture's size that takes
   neither the focus nor your input (`--foreground` and `--fullscreen` change that), co-op and Discord off;
4. waits while the game presses A through the title screen and the file menu, teleports to the capture's area,
   walks Link to where he stood, puts the free camera where the camera was and takes a capture of its own;
5. closes the game and prints both captures side by side: area, positions, the distance between them, cameras, and
   the mean difference of the two pictures.

The run's log and its capture are in `<capture>/replays` (`--out` names another folder). `--set KEY=VALUE` puts one
setting on top of the capture's for this run, which is how a suspected setting is tried: run once as captured and
once with `--set`. `--hold <seconds>` keeps the game open after its capture; `--no-camera` leaves the camera to the
game, for a bug that is about the camera itself. Any other test setting can ride along with `--set` (`DUMP`, `SHOT_DIR`
and `SHOT_EVERY`, `DBG_TEST`, `TRACE`).

What the game does in such a run (`NX2NAVI_CAPTURE_REPLAY`, environment only):

- The capture's `settings.ini` stands in for `nx2navi.ini`, which is neither read nor written; the environment still
  wins over it. The log's first lines say so and list the settings.
- Until Link appears the pad presses A (four reads in every hundred).
- Three seconds later the game changes area as its own doors do (`dbg_warp`), to the start position of the capture's
  area that is nearest to the captured place; when the save already starts within 60 m of it, there is no teleport.
  A refused teleport (an event still running after the load) is asked again, up to six times.
- Link runs to the captured `x z` (`dbg_route_to`), steered by the game's own camera and walking the last three
  metres, and counts as there within half a metre. He is walked, not put: his position cannot be written
  (`debug_tools.c`). When something is in the way he sidesteps, and after ten tries the walk is given up where he is.
  When he does not move at all for two seconds, the game holds him (a conversation that began where the teleport
  arrived, on a save that had not been there): A is pressed until he moves, and the walk is asked for again.
- The free camera is turned on standing (the pad still plays the game) and put at `camera_pos`, `camera_yaw`,
  `camera_pitch`. `NX2NAVI_CAPTURE_REPLAY_CAMERA=0` leaves the game's camera.
- Three seconds later the log says `capture: replay of <name> ready: ...` with how far Link is from the captured
  place, and the run takes its own capture (`trigger = replay`), whose `capture.txt` adds `replay_of`,
  `replay_player_off_metres`, `replay_walk` and `replay_camera`.

What a replay does not bring back: the time of day, enemies and other actors, Link's pose and what he holds, an event
or cutscene that was running, the camera's field of view where the game had changed it (the comparison shows both),
and progress made since the last save (the save is the one on disk at the press; the flags that differed are in
`flags.tsv`). A capture taken at the title screen or while loading has no area: its replay applies the settings and
stops.

Measured on the PC the feature was written on (RX 7900 XT, HDR on): in a 3232x1352 window a capture is written in
1.2 s and holds about 9 MB, eight of them the picture; in a 1720x720 window 0.6 to 0.8 s and 3.5 MB. The frame after
the press is some 10 ms longer, for the read-back of the picture. A replay of a capture taken three metres from where
the save starts took 45 s from start to its own capture, with Link 0.4 to 0.5 m from the captured place and the
camera's position and angles equal to the last digit; one that teleports to another area and ends a conversation on
the way took about 100 s.

Without the script: set `NX2NAVI_CAPTURE_REPLAY` to the folder and start the game with `--profile <a copy of a
profile>`; `NX2NAVI_CAPTURE_DIR` says where the run's own capture goes (default `<capture>/replays`).

## Test settings

Environment or `nx2navi.ini`, for runs without a keyboard:

- `NX2NAVI_CAPTURE_AT=<frame>[,<frame>...]`: a capture at each of those presented frames (up to 16).
- `NX2NAVI_CAPTURE_DIR=<folder>`: the folder captures go to, in place of `logs/captures`.

## The flight recorder

`flight = 1` keeps a ring of the last frames with a small picture each (`runtime/flight.c`). Its dump on F8 now
belongs to the capture: it goes to the capture's `flight` folder. Its other dumps (a frame over `flight_spike_ms`,
`NX2NAVI_FLIGHT_AT`, `NX2NAVI_FLIGHT_EVERY`) go to the `flight` folder beside the game folder as before.
