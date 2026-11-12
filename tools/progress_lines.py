"""Turns the "@@" event lines of a build (make_game.py --events, and AppRun's own steps) into progress lines, for a
program that runs the build without a window:

    progress 0.42 Compile the game

The number is the build's overall share, from 0 to 1, and never goes back. Lines that are not events pass through
unchanged; an error event is also printed as "error: <message>" on stderr.

usage: AppRun --events <game.nup> <game folder> | progress_lines.py
"""
import json
import sys

# What each step adds to the whole bar. The RomFS image is packed beside the lift and the compile, so their shares
# together are a little more than the time the three take.
WEIGHTS = {'deps': 0.01, 'check': 0.01, 'exefs': 0.02, 'romfs': 0.15, 'lift': 0.12, 'compile': 0.38,
           'shaders': 0.22, 'prompts': 0.03, 'scan': 0.05, 'dlss': 0.01}
TEXT = {'deps': 'Checking the packages', 'check': 'Checking the dump', 'exefs': 'Unpacking the program files',
        'romfs': 'Building the RomFS image', 'lift': 'Turning the code into C', 'compile': 'Compiling the game',
        'shaders': 'Preparing the shaders', 'prompts': 'Drawing the button prompts', 'scan': 'Checking the game files',
        'dlss': 'Getting NVIDIA DLSS'}


def main():
    share = {step: 0.0 for step in WEIGHTS}   # 0 to 1 of each step
    total = sum(WEIGHTS.values())
    shown = -1
    for line in sys.stdin:
        line = line.rstrip('\r\n')
        if not line.startswith('@@'):
            print(line, flush=True)
            continue
        try:
            ev = json.loads(line[2:])
        except ValueError:
            continue
        step, kind = ev.get('step'), ev.get('ev')
        if kind == 'error':
            print(f'error: {ev.get("message", "")}', file=sys.stderr, flush=True)
            continue
        if step not in WEIGHTS:
            continue
        if kind == 'step' and ev.get('state') in ('done', 'skip'):
            share[step] = 1.0
        elif kind == 'progress':
            if 'fraction' in ev:
                f = float(ev['fraction'])
            elif ev.get('total'):
                f = ev.get('done', 0) / ev['total']
            else:
                f = 0.0
            share[step] = max(share[step], min(f, 0.99))
        elif kind != 'step':
            continue
        whole = sum(WEIGHTS[s] * share[s] for s in WEIGHTS) / total
        if int(whole * 100) > shown:
            shown = int(whole * 100)
            print(f'progress {whole:.2f} {ev.get("detail") or TEXT[step]}', flush=True)


if __name__ == '__main__':
    main()
