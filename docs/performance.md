# Performance

How to measure the frame rate of a scene the same way twice, and how to read the result.

## Benchmark

`tools/bench.py` runs fixed scenes (`tools/bench/scenes.toml`) on a game folder and writes every frame's timing:

```sh
.venv/bin/python tools/bench.py run ~/Games/Ocarina --save <save backup> --label base
.venv/bin/python tools/bench.py summary build/bench/<date>-base      # the summary again from the logs
```

- **The save**: one of the game folder's `profile/save-backup/<time>` folders, or any save `tools/run_game.py` takes.
  The scenes teleport Link, so the save only has to be in the right age (child or adult) for them.
- **A run**: the game starts through `tools/run_game.py` on a copy of the profile, presses A until the save loads,
  teleports to the scene's area and flies the debug tools' free camera along a fixed path there
  (`runtime/debug_tools.c` `test_script`, game 1.1.1 only). The frame limit is off (`NX2NAVI_FPS_LIMIT=unlimited`);
  every other setting is the game folder's `nx2navi.ini`, copied into the results. `--set KEY=VALUE` adds a setting
  (`{tag}` in the value becomes the run's scene and number).
- **Why the free camera**: the game draws, culls and loads detail for the view the free camera gives it, and nothing
  in its way changes the path. Link running a route of points (`route`) met villagers who start a conversation,
  water and walls, and ended somewhere else each time. The cost of Link's own running (animation, collision) is
  therefore not in the numbers; he stands at the scene's start position. The path stays within about 8 m of that
  position: wider ones passed through walls into the void outside the map.
- **Steps at a game time**: `NX2NAVI_DBG_TEST` steps written `@<ms>:` wait for the world clock instead of a count of
  the game's pad reads, which come as often as frames: the teleport and the camera then start at the same game time
  on a fast build and a slow one. A build without this (0.10.1 and older) runs every `@` step at once.
- **Pictures along the way**: `--set NX2NAVI_SHOTS=w15-53:5 --set NX2NAVI_SHOTS_DIR=<dir>/shots-{tag}` saves a small
  picture every 5 s of the flight, to check where it went. Leave it off for numbers: each picture costs a readback.
- **Rounds**: `--runs 3` (the default) plays every scene once per round, three rounds, so a drift over time (heat,
  background work) reaches every scene alike. The summary gives the spread between runs.
- **Results** in `build/bench/<date>-<label>/`: per run the game's log and a CSV of every measured frame;
  `summary.md` and `summary.csv`; `meta.json` with the commit, the machine, the settings and the mode.
- **Play mode and `--shot`**: a run plays as a player does. Pipelines are built in the background, and a draw whose
  pipeline is not ready yet is skipped for that frame. `--shot` also saves a PNG of each run's last frame, to compare
  pictures between builds. The runtime then builds every pipeline on the render thread and waits for the boot's
  pipeline warm-up (`runtime/boot.c`), so the picture does not depend on build timing. A scene's first visit then
  stalls far longer than in play: on an RX 570 with empty caches, the Market was blocked on pipeline builds for 123 s
  with `--shot` and 15 s without. Compare runs of the same mode only; `summary.md` says which mode a run used, and
  results from before the mode was recorded were all `--shot` runs.
- **MangoHud**: `--mangohud` runs the game with MangoHud's logging on and puts its average fps beside the game's,
  over the same stretch.
- **One game at a time**: a run waits while another copy of the game or Ninja runs (`run_game.py --quiet`). Leave the
  PC alone while it runs: other programs change the numbers.

## Reading the summary

- **Avg fps**: frames divided by time over the measured stretch.
- **1% low**: the mean of the slowest 1% of frames, as fps. The game's own segment line (`segment: 'measure'`) gives
  the 99th percentile instead, which reads a little higher.
- **p99**: the 99th percentile frame time.
- **CPU / GPU / Stall %**: each frame's length is put down to what the render thread waited for
  (`tools/bench.py` `bound_of`): GPU when the fence wait is half the frame or more, or when the GPU was busy for 80% of
  the frame or more (the GPU time per frame of the frame's 60-frame window, the runtime's own rule), stall when
  pipeline builds, shader translation and texture uploads are a quarter or more, CPU otherwise (the game thread and the
  replay of its draws). The GPU time is needed with `NX2NAVI_REPLAY_ASYNC=1`: the replay threads wait for the GPU
  then, not the render thread, so the fence wait alone marked every frame CPU-bound.
- **GPU ms**: the GPU's busy time per frame from its own timestamps, averaged over 60 frames. A GPU-bound frame has
  GPU ms close to its frame time; a CPU-bound one well under it.
- **Replay ms**: the CPU time of replaying the game's draws as Vulkan commands, per frame.

The numbers come from `NX2NAVI_TRACE=perf` with `NX2NAVI_PERF_SLOW_MS=1`, which logs a line per frame
(`runtime/perf.c` `perf_present`). `tests/bench_check.py` checks the parser against the runtime's format strings.
