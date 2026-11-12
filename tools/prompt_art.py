"""The button art tools/pad_prompts.py draws the button prompts from: the pictures of each pad style (STYLES), the
keyboard and mouse folders (KM), and art_files, the files a release carries so the build can draw the prompts on the
player's PC (tools/make_launcher.py and tools/make_kit.py --prompt-art, tools/make_game.py's prompts step).
Standard library only, so the release tools need no imaging packages.

The art is 128x128 PNGs in the folders of button prompt art packs: XGamepad/Default and /Light, P5Gamepad/Default and
/Light, Keyboard_Mouse/Dark, /White and /Blanks. It never enters the repository: a release copies it from the folders
--prompt-art names into its prompt-art folder, which make_game.py passes to pad_prompts.py as --art.
"""
import shutil
import sys
from pathlib import Path

# The art folders, each named as in the art packs; --art holds all three.
ART_SETS = ('XGamepad', 'P5Gamepad', 'Keyboard_Mouse')
# The folder of a release that holds the art (app/<version>/prompt-art, the build kit's prompt-art).
FOLDER = 'prompt-art'


def _sticks(folder, stick, press, rotate):
    """The stick pictures of one style: <stick>_<direction> for L and R."""
    out = {}
    for side, name in (('L', stick[0]), ('R', stick[1])):
        base = f'{folder}{name}'
        out.update({f'{side}STICK': f'{base}.png', f'{side}STICK_UP': f'{base}_UP.png',
                    f'{side}STICK_DOWN': f'{base}_Down.png', f'{side}STICK_LEFT': f'{base}_Left.png',
                    f'{side}STICK_RIGHT': f'{base}_Right.png', f'{side}STICK_UPDOWN': f'{base}_Y.png',
                    f'{side}STICK_LEFTRIGHT': f'{base}_X.png', f'{side}STICK_PRESS': f'{folder}{press[side == "R"]}'})
    out['RSTICK_ROTATE'] = f'{folder}{rotate}'
    return out


def _dpad(folder, name, up):
    return {'DPAD': f'{folder}{name}.png', 'DPAD_UP': f'{folder}{name}_{up}.png', 'DPAD_DOWN': f'{folder}{name}_Down.png',
            'DPAD_LEFT': f'{folder}{name}_Left.png', 'DPAD_RIGHT': f'{folder}{name}_Right.png',
            'DPAD_UPDOWN': f'{folder}{name}_Y.png', 'DPAD_LEFTRIGHT': f'{folder}{name}_X.png'}


# Per style: glyph, the dark pictures the button font and the action prompt's letter are drawn from (by Switch button,
# the face buttons by their place on the pad); note, the light ones the ocarina's note buttons are drawn from. A
# value of OPTIONS is drawn by pad_prompts.py (options_art) from SHARE: the art shows PlayStation's Options button as
# its word over a small pill, which is unreadable at text size, so the prompt is the Create button's pill with three
# bars, as on the pad.
X, XL, P, PL = 'XGamepad/Default/T_X_', 'XGamepad/Light/T_X_', 'P5Gamepad/Default/T_P5_', 'P5Gamepad/Light/T_P5_'
OPTIONS = 'options'
SHARE = 'XGamepad/Default/T_X_Share-1.png'
STYLES = {
    'xbox': {
        'glyph': {'south': f'{X}A_White.png', 'east': f'{X}B_White.png', 'west': f'{X}X_White.png',
                  'north': f'{X}Y_White.png', 'L': f'{X}LB.png', 'R': f'{X}RB.png', 'ZL': f'{X}LT.png',
                  'ZR': f'{X}RT.png', 'PLUS': f'{X}X.png', 'MINUS': f'{X}Share.png',   # Menu (three bars), View
                  **_dpad(X, 'Dpad', 'Up'), **_sticks(X, ('L', 'R'), ('Left_Stick_Click.png', 'Right_Stick_Click.png'),
                                                      'R_2D.png')},
        'note': {'south': f'{XL}A_Color_Light.png', 'east': f'{XL}B_Color_Light.png', 'west': f'{XL}X_Color_Light.png',
                 'north': f'{XL}Y_Color_Light.png', 'L': f'{XL}LB_Light.png', 'R': f'{XL}RB_Light.png'},
    },
    'playstation': {
        'glyph': {'south': f'{P}Cross.png', 'east': f'{P}Circle.png', 'west': f'{P}Square.png',
                  'north': f'{P}Triangle.png', 'L': f'{P}L1.png', 'R': f'{P}R1.png', 'ZL': f'{P}L2.png',
                  'ZR': f'{P}R2.png', 'PLUS': OPTIONS, 'MINUS': SHARE,   # a share pill
                  **_dpad(P, 'Dpad', 'UP'), **_sticks(P, ('L', 'R'), ('L3.png', 'R3.png'), 'R_2D.png')},
        'note': {'south': f'{PL}Cross_Color_Light.png', 'east': f'{PL}Circle_Color_Light.png',
                 'west': f'{PL}Square_Color_Light.png', 'north': f'{PL}Triangle_Color_Light.png',
                 'L': f'{PL}L1_Light.png', 'R': f'{PL}R1_Light.png'},
    },
    # The Steam Deck's own labels, from the two pads' pictures (the packs have no Deck style): A B X Y where an Xbox pad
    # has them, in white as the Deck prints them; L1 R1 L2 R2 and L3 R3 as a PlayStation pad names them; Menu (three
    # bars) and View.
    'steamdeck': {
        'glyph': {'south': f'{X}A_White.png', 'east': f'{X}B_White.png', 'west': f'{X}X_White.png',
                  'north': f'{X}Y_White.png', 'L': f'{P}L1.png', 'R': f'{P}R1.png', 'ZL': f'{P}L2.png',
                  'ZR': f'{P}R2.png', 'PLUS': f'{X}X.png', 'MINUS': f'{X}Share.png',
                  **_dpad(X, 'Dpad', 'Up'), **_sticks(X, ('L', 'R'), ('Left_Stick_Click.png', 'Right_Stick_Click.png'),
                                                      'R_2D.png'),
                  'LSTICK_PRESS': f'{P}L3.png', 'RSTICK_PRESS': f'{P}R3.png'},
        'note': {'south': f'{XL}A_White_Light.png', 'east': f'{XL}B_White_Light.png', 'west': f'{XL}X_White_Light.png',
                 'north': f'{XL}Y_White_Light.png', 'L': f'{PL}L1_Light.png', 'R': f'{PL}R1_Light.png'},
    },
}

# The keyboard and mouse: a key in each shade (Dark: a dark key with a white label, the button font and the action
# prompt; White: the other way round, the ocarina's notes), and the blank key a key the art has no picture of gets its
# name written on (pad_prompts.py label_art).
KM = 'Keyboard_Mouse/'
SHADES = ('Dark', 'White')
BLANKS = {'Dark': f'{KM}Blanks/T_Blank_Black_Normal.png', 'White': f'{KM}Blanks/T_Blank_White_Normal.png'}


def overlay_folder(style, layout=None):
    """The game folder's overlay folder of a style ('pc' for the keyboard and mouse) and pad layout ('position' or
    'label'), as runtime/input_map.c input_prompts_folder names it."""
    return f'romfs-patches-{style}' + ('-label' if layout == 'label' else '')


# Every folder pad_prompts.py writes without --style and --layout.
OVERLAYS = tuple(overlay_folder(s, layout) for s in sorted(STYLES) for layout in ('label', 'position')) + \
    (overlay_folder('pc'),)


def art_roots(folders):
    """ART_SETS name -> the folder that holds it, from folders that each hold some of ART_SETS or are one of them
    (for art spread over several packs). A set named itself wins over one found in a folder that holds it, so a pack's
    Source folder and another pack's Keyboard_Mouse take the latter's keys. Exits naming a set that is missing, or
    found twice at the same rank."""
    ranked = ({}, {})   # sets named themselves, sets found inside a folder
    for f in map(Path, folders):
        if not f.is_dir():
            sys.exit(f'{f}: no such folder')
        rank, found = (0, [(f.name, f.parent)]) if f.name in ART_SETS else \
            (1, [(n, f) for n in ART_SETS if (f / n).is_dir()])
        if not found:
            sys.exit(f'{f} holds none of {", ".join(ART_SETS)}')
        for name, root in found:
            seen = ranked[rank].get(name)
            if seen and seen.resolve() != root.resolve():
                sys.exit(f'{name} is in both {seen} and {root}: name the one to use')
            ranked[rank][name] = root
    roots = {**ranked[1], **ranked[0]}
    missing = [n for n in ART_SETS if n not in roots]
    if missing:
        sys.exit(f'no {", ".join(missing)} folder in {", ".join(map(str, folders))}')
    return roots


def art_files(folders):
    """Relative path (as pad_prompts.py --art reads it) -> source file, for every picture pad_prompts.py can read: the
    pictures of STYLES, SHARE, the blank keys, and every PNG of Keyboard_Mouse/Dark and /White, as a player may bind
    any key. Exits naming the pictures that are missing."""
    roots = art_roots(folders)
    wanted = {rel for style in STYLES.values() for part in style.values() for rel in part.values() if rel != OPTIONS}
    wanted |= {SHARE, *BLANKS.values()}
    files, missing = {}, []
    for rel in sorted(wanted):
        src = roots[rel.split('/')[0]] / rel
        if src.is_file():
            files[rel] = src
        else:
            missing.append(str(src))
    for shade in SHADES:
        keys = sorted((roots['Keyboard_Mouse'] / KM / shade).glob('*.png'))
        if not keys:
            missing.append(str(roots['Keyboard_Mouse'] / KM / shade / '*.png'))
        files.update({f'{KM}{shade}/{p.name}': p for p in keys})
    if missing:
        sys.exit('button art missing:\n  ' + '\n  '.join(missing[:20]))
    return files


def copy_art(folders, dest):
    """Copies art_files(folders) into dest (a release's prompt-art folder, emptied first); returns (files, bytes)."""
    files = art_files(folders)
    shutil.rmtree(dest, ignore_errors=True)
    for rel, src in files.items():
        (dest / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dest / rel)
    return len(files), sum(p.stat().st_size for p in files.values())
