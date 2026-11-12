"""Build and run tests/save_share_check.c: the game's side of a shared save without the game. runtime/fs_host.c,
save_share.c and save_zip.c with the rest of the runtime stubbed, each run of the program one start of the game on a
profile folder made here:

- export: the zip holds the save the game would load next (a finished commit waiting in save-tx before the committed
  save), its manifest as the launcher writes it (launcher/saves.py check_manifest takes it), no key file; with a bug
  report the log and nx2navi.ini; nothing saved yet is refused
- import: zips the launcher exports wait in profile/save-import; at the next start the save the game would load goes
  to saves/backup <time> beside the program, the import becomes the save (the finished commit it replaces too), and
  the start says where the old save went; a second start changes nothing
- refusals (no manifest, another game, a newer format, a path outside the save, a damaged member, no save files):
  nothing waits and the save stays; Cancel drops a waiting import

Usage: save_share_check.py   (Python 3.12 or later; no game files)
"""
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
ACCOUNT = '5555666677778888' + '1111222233334444'   # tests/save_share_check.c account_uid, as fs_host.c names it

from launcher import saves  # noqa: E402


def compiler():
    """$CC, else MSYS2 UCRT64's clang, else clang on PATH (as tests/diff/common.py picks it, without its Unicorn)."""
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def tree(folder):
    folder = Path(folder)
    return {p.relative_to(folder).as_posix(): p.read_bytes() for p in folder.rglob('*') if p.is_file()} \
        if folder.is_dir() else None


def main():
    work = ROOT / 'build' / 'test' / 'save_share'
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    exe = work / ('save_share_check.exe' if os.name == 'nt' else 'save_share_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    plat = ['platform/win32.c', 'platform/common.c'] if os.name == 'nt' else ['platform/linux.c', 'platform/common.c']
    libs = ['-lbcrypt', '-lws2_32', '-lwinmm', '-lpsapi', '-lshell32', '-lole32', '-luser32'] if os.name == 'nt' else \
        ['-lpthread', '-ldl', '-lm']
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-g', '-w', '-I', str(rt), str(ROOT / 'tests' /
                           'save_share_check.c'), *(str(rt / f) for f in ('fs_host.c', 'save_share.c', 'save_zip.c',
                                                                           *plat)), '-o', str(exe), *libs], env=env)
    bad = 0

    def check(ok, what):
        nonlocal bad
        if not ok:
            bad += 1
            print('FAIL', what)

    def run(*args, log=None):
        e = dict(env, SAVE_SHARE_CHECK_LOG=str(log)) if log else env
        r = subprocess.run([str(exe), *map(str, args)], env=e, capture_output=True, text=True)
        if r.returncode:
            print(r.stdout, r.stderr)
        check(r.returncode == 0, f'{args[0]} exited with {r.returncode}')
        return r.stdout.strip()

    profile = work / 'profile'
    acct = profile / 'save' / ACCOUNT
    mine = {'account.sav': b'mine a', 'option.sav': b'mine o', 'slot_00/progress.sav': b'mine 0' * 500,
            'slot_00/caption.sav': b'c0'}
    newer = {'account.sav': b'newer a', 'option.sav': b'newer o', 'slot_01/progress.sav': b'newer 1' * 300}

    # export: nothing yet, then the finished commit before the committed save
    out = work / 'out'
    check(run('export', profile, out).startswith('result 1 '), 'export with no save refused')
    for name, data in mine.items():
        (acct / name).parent.mkdir(parents=True, exist_ok=True)
        (acct / name).write_bytes(data)
    ready = profile / 'save-tx' / 'ready-0000000007'
    for name, data in newer.items():
        (ready / name).parent.mkdir(parents=True, exist_ok=True)
        (ready / name).write_bytes(data)
    (ready / 'prod.keys').write_bytes(b'secret')   # a stray key file in a save folder never goes into a zip
    (work / 'nx2navi.ini').write_bytes(b'[graphics]\nfps_limit = 60\n')
    (work / 'game.log').write_bytes(b'version: NX2navi 9.9.9-check\n')
    line = run('export', profile, out, 'bug', log=work / 'game.log')
    m = re.match(r'result 0 path (.+)$', line)
    check(m is not None, f'export: {line}')
    if m:
        path = Path(m.group(1))
        check(re.fullmatch(r'NX2navi-save-\d{4}-\d\d-\d\d_\d\d-\d\d-\d\d-bug-report\.zip', path.name) is not None,
              f'export name {path.name}')
        with zipfile.ZipFile(path) as z:
            check(z.testzip() is None, 'export zip CRCs')
            got = {n[len('save/'):]: z.read(n) for n in z.namelist() if n.startswith('save/')}
            # the start moved the finished commit in (swap_in) before the export read it
            check(got == newer, f'export holds the newest save: {sorted(got)}')
            check(z.read('bug-report/nx2navi.log').startswith(b'version: NX2navi') and
                  z.read('bug-report/nx2navi.ini').startswith(b'[graphics]'), 'bug report files')
            check(not any(b'secret' in z.read(n) for n in z.namelist()), 'no key file')
            man = json.loads(z.read('manifest.json'))
        saves.check_manifest(man)
        check((man['exported_by'], man['game_version'], man['nx2navi'], man['files'], man['bug_report']) ==
              ('game', '1.1.1', '9.9.9-check', len(newer), True), f'manifest {man}')
        check(re.fullmatch(r'\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ', man['exported']) is not None, 'export time')
        plain = re.match(r'result 0 path (.+)$', run('export', profile, out))
        check(plain is not None and Path(plain.group(1)).name.endswith('.zip') and
              'bug-report' not in Path(plain.group(1)).name, 'export without a bug report')
        if plain:
            with zipfile.ZipFile(plain.group(1)) as z:
                check(not [n for n in z.namelist() if n.startswith('bug-report/')], 'no bug report files')
    check(tree(acct) == dict(newer, **{'prod.keys': b'secret'}), 'the start swapped the finished commit in')
    check([tree(d) for d in (profile / 'save-backup').iterdir()] == [mine], 'and kept the save it replaced')

    # import: a zip the launcher exports, from another game folder
    other = work / 'other-game'
    oacct = saves.account_dir(other)
    shared_save = {'account.sav': b'theirs a', 'option.sav': b'theirs o', 'slot_02/progress.sav': b'theirs 2' * 999,
                   'slot_02/caption.sav': b'cap'}
    for name, data in shared_save.items():
        (oacct / name).parent.mkdir(parents=True, exist_ok=True)
        (oacct / name).write_bytes(data)
    shared = saves.export_save(other, work / 'shared', bug_report=True, version='0.6.2', log=lambda m: None)['path']
    before = tree(acct)
    line = run('import', profile, shared)
    check(re.match(r'result 0 detail 0\.6\.2, \d{4}-\d\d-\d\d waiting 1$', line) is not None, f'import: {line}')
    check(tree(acct) == before, 'the save is unchanged until the next start')
    check(tree(profile / 'save-import') == shared_save, 'the import waits complete in profile/save-import')
    # a finished commit made after the import (the game saved before the restart) is backed up and replaced too
    late = profile / 'save-tx' / 'ready-0000000009'
    (late / 'option.sav').parent.mkdir(parents=True, exist_ok=True)
    (late / 'option.sav').write_bytes(b'saved after the import')
    line = run('start', profile)
    m = re.match(r'done (.+) waiting 0$', line)
    check(m is not None and Path(m.group(1)).parent == work / 'saves', f'start: {line}')
    check(tree(acct) == shared_save, f'the import is the save: {sorted(tree(acct) or {})}')
    if m:
        backup = Path(m.group(1))
        check(re.fullmatch(r'backup \d{4}-\d\d-\d\d \d\d\.\d\d\.\d\d', backup.name) is not None, backup.name)
        check(tree(backup) == {'option.sav': b'saved after the import'}, f'the backup is the save the game would '
              f'have loaded: {sorted(tree(backup) or {})}')
    check(not (profile / 'save-import').exists() and not list((profile / 'save-tx').glob('ready-*')),
          'nothing waits after the start')
    check(run('start', profile) == 'done  waiting 0', 'a second start imports nothing')
    check(tree(acct) == shared_save, 'and keeps the imported save')

    # refusals and Cancel
    def zip_of(name, members, manifest=True):
        path = work / name
        with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
            if manifest:
                z.writestr('manifest.json', json.dumps(dict({'format': 'nx2navi-save', 'format_version': 1,
                                                             'title_id': '0400486024dec000'},
                                                            **(manifest if isinstance(manifest, dict) else {}))))
            for n, b in members.items():
                z.writestr(n, b)
        return path
    good = {'save/option.sav': b'x', 'save/slot_00/progress.sav': b'y' * 400}
    cases = {
        4: zip_of('nomanifest.zip', good, manifest=False),
        5: zip_of('othergame.zip', good, {'title_id': '0100000000010000'}),
        6: zip_of('newer.zip', good, {'format_version': 2}),
        8: zip_of('outside.zip', dict(good, **{'save/../escape.sav': b'z'})),
    }
    damaged = bytearray(zip_of('damaged.zip', good).read_bytes())
    at = damaged.index(b'save/slot_00/progress.sav') + len('save/slot_00/progress.sav') + 3
    damaged[at] ^= 0x40
    (work / 'damaged.zip').write_bytes(damaged)
    cases[7] = work / 'damaged.zip'
    cases[4.5] = zip_of('nosave.zip', {'save/save.dat': b'q'})
    for want, path in cases.items():
        line = run('import', profile, path)
        check(line.startswith(f'result {int(want)} ') and line.endswith('waiting 0'), f'{path.name}: {line}')
    check(not (work / 'escape.sav').exists() and tree(acct) == shared_save, 'a refused zip changes nothing')
    check(run('import', profile, shared).endswith('waiting 1'), 'import again')
    check(run('cancel', profile) == 'result 0 waiting 0', 'cancel')
    check(run('start', profile) == 'done  waiting 0' and tree(acct) == shared_save, 'a cancelled import stays out')

    print('save_share_check:', 'FAILED' if bad else 'ok')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
