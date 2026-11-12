"""Build and run tests/coop_page_check.cpp: the menu's Co-op tab (runtime/ui/coop_page.cpp) without the game, driven with
the key and pad events the menu hands Dear ImGui, the co-op layer answered by the test. Typing a name, a relay and a
secret (with the letters that switch tabs elsewhere), Make, Join with a wrong secret and a pasted one, the room's offer
accepted and declined, the maker's key, a player not identified, a key accepted under another name, Remove by key, the
pools' contributors and their reset, Leave; no confirmation acts on a repeated or held press, the maker changes a rule
and the others cannot.

Then the same checks again with a deliberate fault (COOP_PAGE_FAULT=1: each window that acts on the room opens with the
cursor moved onto its acting button), which they must catch: the test passes only when the first run has no failed
check and the second has some.

Usage: coop_page_check.py   (needs clang++ or g++; no game files)
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / 'runtime'
SOURCES = [ROOT / 'tests' / 'coop_page_check.cpp', RT / 'ui' / 'option_rows.cpp', RT / 'ui' / 'menu_skin.cpp',
           RT / 'ui_text.c', RT / 'coop_proto.c', RT / 'coop_state.c', RT / 'sha256.c',
           RT / 'third_party' / 'monocypher' / 'monocypher.c'] + sorted((RT / 'third_party' / 'imgui').glob('imgui*.cpp'))


def main():
    cxx = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
    if not cxx:
        sys.exit('needs clang++ or g++ on PATH')
    work = ROOT / 'build' / 'test' / 'coop_page'
    work.mkdir(parents=True, exist_ok=True)
    objs = []
    for src in SOURCES:
        obj = work / (src.stem + '.o')
        lang = ['-x', 'c', '-std=gnu11'] if src.suffix == '.c' else ['-x', 'c++', '-std=gnu++17']
        # the page's own file builds with the game's warnings; the rest as the game builds them, unchecked here
        warn = ['-Wall', '-Wextra', '-Werror'] if src.name == 'coop_page_check.cpp' else ['-w']
        if subprocess.run([cxx, *lang, '-c', '-O1', '-g', *warn, '-march=x86-64-v2', '-I', str(RT),
                           '-I', str(RT / 'third_party' / 'imgui'), str(src), '-o', str(obj)]).returncode:
            sys.exit(f'{src.name} did not compile')
        objs.append(str(obj))
    exe = work / ('coop_page_check.exe' if os.name == 'nt' else 'coop_page_check')
    if subprocess.run([cxx, *objs, '-o', str(exe), '-lm']).returncode:
        sys.exit('the link failed')
    env = {**os.environ, 'COOP_PAGE_FONT': str(RT / 'ui' / 'fonts' / 'Roboto-Medium.ttf')}   # measures the rows' notes
    r = subprocess.run([str(exe)], env=env)
    if r.returncode:
        sys.exit(r.returncode)
    fault = subprocess.run([str(exe)], env={**env, 'COOP_PAGE_FAULT': '1'}, capture_output=True, text=True)
    fails = [line for line in fault.stdout.splitlines() if line.startswith('FAIL ')]
    print(f'deliberate fault (the cursor moved onto the acting button as a window opens): {len(fails)} checks fail')
    for line in fails[:3]:
        print('  ' + line)
    if not fails:
        sys.exit('the deliberate fault went unnoticed')


if __name__ == '__main__':
    main()
