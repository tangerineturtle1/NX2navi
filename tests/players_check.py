"""Build and run tests/players_check.c: runtime/players.c, the player folders of the in-game menu's Players group,
without the game, on a profile folder made here:

- a profile made before players existed is player 1 alone, with its save; player 1's save folder is the profile
- Add takes the next free number (a stray file named like a player is skipped) and writes the name; Rename changes and
  drops it; names are cut to 32 bytes on a character boundary, control characters dropped, spaces trimmed
- the player nx2navi.ini names is listed, and gets a folder at the start even when none was made
- Remove moves the folder to players-removed, and refuses player 1 and the player in game

Usage: players_check.py   (Python 3.12 or later; no game files)
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
ACCOUNT = '5555666677778888' + '1111222233334444'


def compiler():
    """$CC, else MSYS2 UCRT64's clang, else clang on PATH."""
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def main():
    work = ROOT / 'build' / 'test' / 'players'
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    exe = work / ('players_check.exe' if os.name == 'nt' else 'players_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    plat = ['platform/win32.c', 'platform/common.c'] if os.name == 'nt' else ['platform/linux.c', 'platform/common.c']
    libs = ['-lbcrypt', '-lws2_32', '-lwinmm', '-lpsapi', '-lshell32', '-lole32', '-luser32'] if os.name == 'nt' else \
        ['-lpthread', '-ldl', '-lm']
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-g', '-w', '-I', str(rt), str(ROOT / 'tests' /
                           'players_check.c'), *(str(rt / f) for f in ('players.c', *plat)), '-o', str(exe), *libs],
                          env=env)
    bad = 0

    def check(ok, what):
        nonlocal bad
        if not ok:
            bad += 1
            print('FAIL', what)

    profile = work / 'profile'

    def run(*args, player=None):
        e = dict(env)
        if player is not None:
            e['PLAYERS_CHECK_PLAYER'] = str(player)
        r = subprocess.run([str(exe), str(profile), *map(str, args)], env=e, capture_output=True)
        if r.returncode:
            print(r.stdout, r.stderr)
        check(r.returncode == 0, f'{args[0]} exited with {r.returncode}')
        return r.stdout.decode('utf-8').replace('\r\n', '\n').strip()

    def hexname(s):
        return s.encode('utf-8').hex()

    def listed(player=None):
        return [tuple((line + '  ').split(' ', 2)[:2]) + (line.split(' ', 2)[2] if line.count(' ') >= 2 else '',)
                for line in run('list', player=player).splitlines()]

    # a profile from before players: player 1 with its save
    acct = profile / 'save' / ACCOUNT
    acct.mkdir(parents=True)
    (acct / 'option.sav').write_bytes(b'o')
    check(listed() == [('1', '1', '')], f'player 1 alone: {listed()}')
    check(Path(run('dir')) == profile, 'player 1 saves in the profile')

    # add, with and without a name; a stray file named 3 is skipped
    check(run('create', hexname('Zelda')) == 'id 2', 'the first new player is 2')
    (profile / 'players' / '3').write_bytes(b'not a player')
    check(run('create') == 'id 4', 'the next free number')
    check(listed() == [('1', '1', ''), ('2', '0', 'Zelda'), ('4', '0', '')], f'list: {listed()}')
    check((profile / 'players' / '2' / 'player-name.txt').read_bytes() == b'Zelda\n', 'name file')

    # rename and names
    check(run('rename', 1, hexname('  Link  ')) == 'ok 1' and listed()[0] == ('1', '1', 'Link'), 'rename player 1')
    check(run('rename', 2) == 'ok 1' and listed()[1] == ('2', '0', ''), 'an empty name drops it')
    check(not (profile / 'players' / '2' / 'player-name.txt').exists(), 'and its file')
    check(run('clean', hexname('a\tb\nc')) == '[abc]', 'control characters dropped')
    check(run('clean', hexname('\u00e9' * 20)) == '[' + '\u00e9' * 16 + ']', 'cut on a character boundary')
    check(run('clean', hexname('x' * 31 + '\u00e9')) == '[' + 'x' * 31 + ']',
          'a character that does not fit is left out')

    # the player nx2navi.ini names: listed, and its folder made at the start
    check(('7', '0', '') in listed(player=7), 'the active player is listed before its folder exists')
    check(Path(run('dir', player=7)) == profile / 'players' / '7' and (profile / 'players' / '7').is_dir(),
          "the start makes the active player's folder")
    check(Path(run('dir', player='abc')) == profile, 'a value that is no number plays as player 1')

    # remove: player 1 and the player in game stay; the others move to players-removed
    check(run('remove', 1) == 'ok 0', 'player 1 stays')
    check(run('remove', 4, player=4) == 'ok 0' and (profile / 'players' / '4').is_dir(), 'the player in game stays')
    (profile / 'players' / '4' / 'save').mkdir()
    check(run('remove', 4) == 'ok 1' and not (profile / 'players' / '4').exists(), 'remove player 4')
    moved = list((profile / 'players-removed').iterdir())
    check(len(moved) == 1 and moved[0].name.startswith('4 ') and (moved[0] / 'save').is_dir(),
          f'its saves are kept: {[m.name for m in moved]}')
    check(run('remove', 9) == 'ok 1', 'a player with no folder is nothing to remove')
    check([p[0] for p in listed()] == ['1', '2', '7'], f'after the removal: {listed()}')
    check(run('create') == 'id 8', 'numbers go on after the highest')

    print('players_check:', 'FAILED' if bad else 'ok')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
