"""The player's mods folder as the game reads it (runtime/mods.c): where each mod keeps its parts, and the files of the
mods that change the game's code.

One folder per mod (a symbolic link or junction to a folder counts). A part (romfs, romfslite, exefs, cheats) is the
first of <mod>/<part>, <mod>/atmosphere/contents/<title>/<part>, <mod>/contents/<title>/<part> and
<mod>/<title>/<part> that is a folder (runtime/mods.c find_root).

The files that change the game's code are its Atmosphere cheat file for main's build (<cheats>/<build id>.txt, the
name in any case) and the ExeFS patch files (.ips, .pchtxt) of its exefs part and of
<mod>/atmosphere/exefs_patches/<name>/. The lifted game cannot be patched while it runs, so the build turns them into
patch sets (tools/cheats.py, tools/exefs_patches.py) and the runtime switches those on with their mod; a file the
build did not see waits for the next one. tools/make_game.py builds them in and records what it built
(build-cache/mod-code.json, code_record); the launcher compares that record with the folder (launcher/mods.py) to
rebuild the game before it starts when a mod brought new ones.

Standard library only: the launcher imports it from the kit's tools folder.
"""
import hashlib
from pathlib import Path

TITLE = '0400486024DEC000'   # the game's title id: a mod's Atmosphere folders are named after it
PLACES = ('{}', f'atmosphere/contents/{TITLE}/{{}}', f'contents/{TITLE}/{{}}', f'{TITLE}/{{}}')
PATCH_SUFFIXES = ('.ips', '.pchtxt')


def mod_folders(mods):
    """The mod folders of the mods folder, by name (as the setup's tools list them: Python's order of the names)."""
    mods = Path(mods)
    if not mods.is_dir():
        return []
    return sorted((d for d in mods.iterdir() if d.is_dir()), key=lambda d: d.name)


def part(mod, name):
    """The folder of a mod's part (romfs, romfslite, exefs, cheats): the first of its places that is a folder, or
    None."""
    return next((Path(mod) / w.format(name) for w in PLACES if (Path(mod) / w.format(name)).is_dir()), None)


def cheat_files(mods, bid):
    """[(mod folder name, cheat file)] of the folders of mods, by name: <cheats>/<bid>.txt, the name in any case. bid is
    the first 8 bytes of main's build id in hex, as cheat files name themselves (tools/cheats.py build_id)."""
    out = []
    for mod in mod_folders(mods):
        folder = part(mod, 'cheats')
        if folder:
            f = next((p for p in sorted(folder.iterdir()) if p.is_file() and p.name.lower() == f'{bid}.txt'.lower()),
                     None)
            if f:
                out.append((mod.name, f))
    return out


def patch_files(mods):
    """[(mod folder name, path in the mod, file)] of the folders of mods, by name: the .ips and .pchtxt files of the
    exefs part and of atmosphere/exefs_patches/<name>/."""
    out = []
    for mod in mod_folders(mods):
        folders = []
        exefs = part(mod, 'exefs')
        if exefs:
            folders.append(exefs)
        patches = mod / 'atmosphere' / 'exefs_patches'
        if patches.is_dir():
            folders += sorted(d for d in patches.iterdir() if d.is_dir())
        for folder in folders:
            for f in sorted(folder.iterdir()):
                if f.is_file() and f.suffix.lower() in PATCH_SUFFIXES:
                    out.append((mod.name, f.relative_to(mod).as_posix(), f))
    return out


def main_build_id(main):
    """The first 8 bytes of main's build id in upper-case hex (the cheat files' name), '' when main cannot be read."""
    try:
        with open(main, 'rb') as f:
            head = f.read(0x48)
    except OSError:
        return ''
    return head[0x40:0x48].hex().upper() if len(head) == 0x48 and head[:4] == b'NSO0' else ''


def code_files(mods, bid):
    """[(mod folder name, path in the mod, file)] of every file of the mods folder that changes the game's code: the
    cheat files for bid, then the ExeFS patch files."""
    return ([(m, f.relative_to(Path(mods) / m).as_posix(), f) for m, f in cheat_files(mods, bid)]
            + patch_files(mods))


def code_record(mods, bid):
    """{"<mod>/<path in the mod>": sha256 of its bytes} of code_files: what a build that lifts them in takes. A file
    that cannot be read is left out (the tools would fail on it too, and the next build tries again)."""
    out = {}
    for mod, rel, f in code_files(mods, bid):
        try:
            out[f'{mod}/{rel}'] = hashlib.sha256(f.read_bytes()).hexdigest()
        except OSError:
            pass
    return out
