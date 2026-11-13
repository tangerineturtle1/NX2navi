"""The launcher without its window, for scripts, tests and anyone who prefers a console:

    python -m launcher.cli check                       is there a newer release?
    python -m launcher.cli update                      download and stage it, then finish in the new version
    python -m launcher.cli tools                       install the build tools and zbic
    python -m launcher.cli install --dump F --game D   build the game (the window's Begin Installation)
    python -m launcher.cli rebuild [--game D]          rebuild after an update (the window's Rebuild the Game)
    python -m launcher.cli fonts [--dump F]            decode the game's font for the window
    python -m launcher.cli continue-update OLD         what the new version runs after `update`

In an installed launcher: python\\python.exe app\\<version>\\launcher\\cli.py <command> (this file puts the release
folder on sys.path itself, as start.pyw does). Exit code 0 on success, 1 on failure, 3 when cancelled.
"""
import argparse
import os
import subprocess
import sys
import time

if __name__ == '__main__' and not __package__:
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    __package__ = 'launcher'   # noqa: A001 — lets the relative imports below work when run as a script

from . import build, gamefiles, jobs, locks, versions  # noqa: E402
from .context import Context  # noqa: E402
from .paths import Layout  # noqa: E402


def follow(job):
    """Run a job here, printing each step's state as it changes."""
    seen = {}
    job.start()
    while job.result is None:
        with job.locked():   # each step as one change of the worker left it
            steps = [(s.id, s.label, s.state, s.value, s.fraction, s.detail) for s in job.steps]
        for sid, label, state, value, fraction, detail in steps:
            key = (state, value if state != jobs.RUNNING else int(fraction * 20))
            if seen.get(sid) != key:
                seen[sid] = key
                print(f'{label}: {value}' + (f' ({detail})' if detail and state == jobs.RUNNING else ''), flush=True)
        time.sleep(0.2)
    for s in job.steps:
        if seen.get(s.id, (None,))[0] != s.state:
            print(f'{s.label}: {s.value}', flush=True)
    if job.result == 'failed':
        print(f'error: {job.error}', file=sys.stderr, flush=True)
    return {'ok': 0, 'failed': 1, 'cancelled': 3}[job.result]


def main(argv=None):
    ap = argparse.ArgumentParser(prog='launcher.cli', description=__doc__.splitlines()[0])
    ap.add_argument('--root', help='the launcher folder (default: this release\'s, or build/launcher-dev)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('check')
    sub.add_parser('update')
    sub.add_parser('tools')
    p = sub.add_parser('install')
    p.add_argument('--dump', required=True)
    p.add_argument('--game', required=True)
    p = sub.add_parser('rebuild')
    p.add_argument('--game')
    p = sub.add_parser('fonts')
    p.add_argument('--dump', default='')
    p.add_argument('--game', default='')
    p = sub.add_parser('continue-update')
    p.add_argument('old')
    a = ap.parse_args(argv)
    ctx = Context(Layout(root=a.root) if a.root else None, echo=False)
    if a.cmd == 'check':   # reads the feed and changes nothing: it runs beside an open launcher
        return run(a, ctx, None)
    # Every other command changes the launcher's folder or the game's, as the window's jobs do: it holds the folder's
    # lock (launcher/locks.py) from before it writes the settings until it ends, and is refused while the window or
    # another command holds it
    try:
        held = locks.FileLock(ctx.layout.instance_lock, f'"{a.cmd}" from the command line', where=ctx.layout.root)
    except locks.Busy as e:
        print(f'error: {e}. Close it, or wait until it is done, and try again.', file=sys.stderr, flush=True)
        return 1
    try:
        return run(a, ctx, held)
    finally:
        held.release()


def run(a, ctx, held):
    """One command. held: the launcher folder's lock, which `update` lets go of before the new version takes over."""
    s = ctx.settings

    if a.cmd == 'check':
        job = jobs.CheckJob(ctx).run_inline()
        if job.result != 'ok':
            print(f'error: {job.error}', file=sys.stderr)
            return 1
        r = job.release
        if not r:
            print('no release found')
        elif jobs.update_available(ctx, r):
            print(f'update available: {versions.display(r.version)} (you have {ctx.version})')
        else:
            print(f'up to date: {ctx.version} (newest release {versions.display(r.version)})')
        return 0

    if a.cmd == 'update':
        check = jobs.CheckJob(ctx).run_inline()
        if check.result != 'ok' or not jobs.update_available(ctx, check.release):
            print(check.error or f'up to date: {ctx.version}')
            return 0 if check.result == 'ok' else 1
        job = jobs.UpdateDownloadJob(ctx, check.release)
        code = follow(job)
        if code or job.after != 'restart':
            return code
        new = ctx.layout.apps / versions.display(check.release.version)
        cli = new / 'launcher' / 'cli.py'
        print(f'finishing in {versions.display(check.release.version)}', flush=True)
        held.release()   # the new version's command takes the lock itself
        return subprocess.call([ctx.layout.python_for_builds(), str(cli), '--root', str(ctx.layout.root),
                                'continue-update', ctx.version])

    if a.cmd == 'continue-update':
        gd = s.get('game_dir')
        return follow(jobs.ContinueUpdateJob(ctx, gd if build.installed(gd) else '', a.old,
                                             s.get('dump')))

    if a.cmd == 'tools':
        job = jobs.RebuildJob(ctx, '')
        job.steps = job.steps[:1]
        job.run = lambda: job.ensure_tools('tools_update')
        return follow(job)

    if a.cmd == 'install':
        s['dump'], s['game_dir'] = os.path.abspath(a.dump), os.path.abspath(a.game)
        s.save()
        return follow(jobs.InstallJob(ctx, s['dump'], s['game_dir']))

    if a.cmd == 'rebuild':
        gd = os.path.abspath(a.game) if a.game else s.get('game_dir')
        if not gd:
            print('error: no game folder (give --game)', file=sys.stderr)
            return 1
        return follow(jobs.RebuildJob(ctx, gd, s.get('dump')))

    if a.cmd == 'fonts':
        got = gamefiles.extract_fonts(ctx.layout, a.game or s.get('game_dir'), a.dump or s.get('dump'))
        for role, (path, fam) in got.items():
            print(f'{role}: {fam} ({path})')
        return 0 if got else 1
    return 2


if __name__ == '__main__':
    sys.exit(main())
