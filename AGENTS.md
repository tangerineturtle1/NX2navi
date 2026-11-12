# NX2navi

Static recompilation of *Ocarina of Time* (Switch 2, ARM64, game version 1.1.1) into a native Windows x86-64 exe with a Vulkan renderer. Design: [docs/architecture.md](docs/architecture.md). Toolchain, build commands and tests: [docs/building.md](docs/building.md). The players' launcher, setup and updates: [docs/launcher.md](docs/launcher.md).

## Game content boundary

The repository's additions are all rights reserved (`LICENSE-ADDITIONS`). It never holds game data: game data lives outside it and every derived file stays in ignored folders.

- The user's game and key paths are in `config.local.toml` (ignored). Pass the keys file path to tools; the tools read it themselves. Key values stay out of your output, logs, commands and files.
- Everything extracted or generated goes under `build/` or `generated/`.
- Every file the code generator emits carries the marker defined as `GENERATED_MARKER` in `tools/check_content.py`.
- Run `python tools/check_content.py --staged` before proposing a commit. It is the same check CI runs.

## Reusing code

`refs/` holds the pinned checkout the build reads (`tools/fetch_refs.sh` recreates it). Read-only.

- Copy only MIT, BSD, ISC, zlib or Apache-2.0 code, keep its header, and add a row to `THIRD_PARTY_NOTICES.md`.
- GPL code is never copied: write our own.
- `tools/vendor/` stays byte-identical to its source; Windows changes go in `tools/extract.py`.

## Code

- Build C with MSYS2 UCRT64 Clang (`C:\msys64\ucrt64\bin`). Generated C uses GNU extensions, so `cl.exe` is out. Call Ninja from that directory by full path or put it first on PATH.
- Two Python environments: `.venv` (3.12) for the lifter and the build, `.venv-py314` for the extraction and shader scripts, which need `compression.zstd`.
- Generated C is never edited by hand: change the generator (`recomp/`).
- The runtime's own shaders are checked in as source only (`runtime/shaders/`, `tools/make_dlaa_mask.py`,
  `tools/make_mvobj_fs.py`). `tools/build_shaders.py` builds `runtime/<name>_spv.h` from them, `build_game.py` runs it,
  git ignores the headers and `check_content.py` refuses them in a commit.
- Every lifter change passes the differential tests against Unicorn ([docs/building.md](docs/building.md), "Checks"). After changing `recomp/jumptables.py`, `tests\jumptables_check.py` reports 0 disagreements on 1.2.1 and 1.4.3.
- `recomp/layout.py` alone decides module bases.
- Graphics bugs: [docs/maxwell.md](docs/maxwell.md) lists the Maxwell behaviour the Vulkan path must match, what goes wrong when it does not, and what our runtime does.
- Comments say what the code does and why, in words that stand on their own. Cite our code by file plus function (a line number may follow as a hint). Comments and docs do not name other projects or say where code came from: the license header of copied code and its `THIRD_PARTY_NOTICES.md` row carry that.

## Writing

Code, comments, docs and commit messages are in English. Commit messages are plain sentences without type prefixes.
