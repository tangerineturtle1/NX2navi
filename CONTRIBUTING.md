# Contributing

## The one hard rule

Nothing that came out of the game goes into the repository: no game files, keys, extracted data, recompiled C, compiled binaries, shaders, shader caches, fonts, audio or movies. Not in commits, not in issues, not in pull requests. Only information about the game goes in: addresses, hashes, patch code we wrote, shader and bind-state identifiers.

Our own shaders go in as source only (`runtime/shaders/`, `tools/make_dlaa_mask.py`, `tools/make_mvobj_fs.py`); the build compiles them (`tools/build_shaders.py`), and the same checks refuse a compiled one.

Two checks enforce this:

- CI runs `tools/check_content.py` on every push and pull request.
- The same check runs before each local commit once you enable the hook:

  ```sh
  git config core.hooksPath .githooks
  ```

A failing check is never bypassed with `--no-verify`. If it flags a file by mistake, change the check in a separate pull request and say why.

Links to game files, keys or firmware in issues or pull requests get the issue closed and the content removed.

## Code from other projects

Code may be copied only from projects under MIT, BSD, ISC, zlib or Apache-2.0. Each copy keeps its original header and gets a row in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) with the source commit.

GPL code is never copied, and none goes into the game binary. Unicorn (GPL) is fine as a separate test tool.

## Reporting bugs

Use the issue form. Include the port version and game update from the title line of the settings menu (F1), your GPU and driver, the log (`logs
x2navi.log` in the game folder; after a crash, copy it before you start the game again), and the steps to reproduce. Screenshots are fine; recordings of long gameplay are not needed.

## Pull requests

- One topic per pull request.
- Describe the problem, then what changed.
- Say how you tested it: which version, which scene, how long.
- A change to the lifter (`recomp/`) or the CPU headers passes the differential tests in [docs/building.md](docs/building.md), "Checks".
- Commit messages are plain sentences, without `feat:` or other prefixes.
- AI-assisted code is welcome. You are responsible for having read and run it.

## Code and docs

- Code, comments, commit messages and documentation are in English.
- Generated code is never edited by hand: change the generator.
