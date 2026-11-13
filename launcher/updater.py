"""Updating the launcher's own release folder (launcher/paths.py, "app/").

An update is staged beside the running version and switched to with two small files, so a failed or interrupted
update never leaves a half-written release in use:

1. The app archive goes to data/downloads, is checked against the manifest's size and sha256 and unpacks into
   app/<version>.part, which is renamed to app/<version> once it is complete and holds the expected VERSION.
2. A new Python (the manifest's runtime id differs from python/runtime-id.txt) unpacks into python.next; the stub
   swaps it in at its next start, while no Python runs.
3. app/previous.txt gets the running version and app/current.txt the new one; the launcher starts the stub again,
   which runs the new version. The stub goes back to previous.txt when the new version fails to start.
4. The new version rebuilds the game (launcher/jobs.py), then prune() removes every version but these two.

The launcher also stages a release in the background while it runs, the game included (stage()), and installs it
later, when the game is not running (install_staged()). Such a release waits in app/<version> with its Python, when it
brings one, in app/<version>/python.staged rather than python.next: the stub swaps python.next in at every start, and
the running version must not start on another Python before current.txt names the new one. app/<version>/staged.json,
written last, says the release is complete; a folder without it is never installed and is staged again.
"""
import json
import os
import shutil
import zipfile
from pathlib import Path

from . import net, versions

STAGED = 'staged.json'             # in app/<version>: the background staging of that release finished
STAGED_PYTHON = 'python.staged'    # in app/<version>: the Python the release brings, until it installs


class UpdateError(Exception):
    pass


def read_text(path, default=''):
    try:
        return Path(path).read_text(encoding='utf-8').strip()
    except OSError:
        return default


def write_text(path, text):
    path = Path(path)
    tmp = path.with_name(path.name + '.tmp')
    tmp.write_text(text + '\n', encoding='utf-8')
    os.replace(tmp, path)


def current(layout):
    return read_text(layout.apps / 'current.txt') or layout.version


def previous(layout):
    return read_text(layout.apps / 'previous.txt')


def runtime_id(layout):
    return read_text(layout.python / 'runtime-id.txt')


def safe_extract(zip_path, dest):
    """Unpack a zip into dest, refusing absolute names and names that climb out of it, and, before anything is
    written, a zip whose files do not fit the disk (net.space_problem)."""
    dest = Path(dest)
    root = dest.resolve()
    with zipfile.ZipFile(zip_path) as z:
        for info in z.infolist():
            name = info.filename.replace('\\', '/')
            if name.startswith('/') or ':' in name.split('/')[0] or '..' in name.split('/'):
                raise UpdateError(f'{Path(zip_path).name}: unsafe path {info.filename}')
            target = (dest / name).resolve()
            if target != root and root not in target.parents:
                raise UpdateError(f'{Path(zip_path).name}: unsafe path {info.filename}')
        full = net.space_problem(dest, sum(info.file_size for info in z.infolist()),
                                 f'the files of {Path(zip_path).name}')
        if full:
            raise UpdateError(full)
        z.extractall(dest)


def _fetch(release, key, layout, progress, cancel, version, log=None):
    url, headers, size, sha = release.asset(key)
    entry = release.manifest[key]
    dest = layout.downloads / entry['name']
    net.download(url, dest, sha256=sha, size=size, headers=headers, progress=progress, cancel=cancel,
                 version=version, log=log)
    return dest


def stage_app(layout, release, progress=None, cancel=None, version='dev', log=None):
    """Download, check and unpack the release's app archive into app/<version>; returns that folder."""
    zip_path = _fetch(release, 'app', layout, progress, cancel, version, log)
    ver = versions.display(release.version)
    final = layout.apps / ver
    part = layout.apps / f'{ver}.part'
    shutil.rmtree(part, ignore_errors=True)
    part.mkdir(parents=True)
    try:
        safe_extract(zip_path, part)
        got = read_text(part / 'VERSION')
        if versions.display(got) != ver or not (part / 'launcher' / 'start.pyw').is_file():
            raise UpdateError(f'{zip_path.name} holds version {got or "?"} without a launcher, not {ver}')
        if final.exists():
            if final.resolve() == layout.app.resolve():
                raise UpdateError(f'{ver} is the version that runs now')
            shutil.rmtree(final)
        os.replace(part, final)
    except BaseException:
        shutil.rmtree(part, ignore_errors=True)
        raise
    zip_path.unlink(missing_ok=True)
    return final


def runtime_needed(layout, release):
    rt = release.manifest.get('runtime')
    return bool(rt) and not layout.dev and rt.get('id') != runtime_id(layout)


def stage_runtime(layout, release, progress=None, cancel=None, version='dev', dest=None, log=None):
    """Unpack a new Python into dest (python.next, for the stub to swap in at the next start); False when the running
    one is current."""
    if not runtime_needed(layout, release):
        return False
    zip_path = _fetch(release, 'runtime', layout, progress, cancel, version, log)
    nxt = Path(dest) if dest else layout.root / 'python.next'
    shutil.rmtree(nxt, ignore_errors=True)
    part = nxt.with_name(nxt.name + '.part')
    shutil.rmtree(part, ignore_errors=True)
    part.mkdir()
    safe_extract(zip_path, part)
    if not (part / 'python.exe').is_file() or read_text(part / 'runtime-id.txt') != release.manifest['runtime']['id']:
        shutil.rmtree(part, ignore_errors=True)
        raise UpdateError(f'{zip_path.name} is not the runtime {release.manifest["runtime"]["id"]}')
    os.replace(part, nxt)
    zip_path.unlink(missing_ok=True)
    return True


def prerelease(release):
    """True for a GitHub pre-release or a version with a pre-release tag (0.5.0-rc1): only the Preview channel
    installs it."""
    try:
        return bool(release.prerelease) or versions.parse(release.version)[1] != (1,)
    except ValueError:
        return True


def stage(layout, release, progress=None, cancel=None, version='dev', log=None):
    """Download, check and unpack a release into app/<version>, with the Python it brings in python.staged there,
    without switching to it; staged.json, written last, marks it complete. Earlier staged releases it supersedes are
    removed. Returns the folder."""
    ver = versions.display(release.version)
    folder = stage_app(layout, release, progress=progress, cancel=cancel, version=version, log=log)
    runtime = ''
    if runtime_needed(layout, release):
        stage_runtime(layout, release, progress=progress, cancel=cancel, version=version,
                      dest=folder / STAGED_PYTHON, log=log)
        runtime = release.manifest['runtime']['id']
    write_text(folder / STAGED, json.dumps({'version': ver, 'runtime': runtime, 'prerelease': prerelease(release),
                                            'notes': release.notes}))
    keep = {ver, current(layout), previous(layout)}
    for d in layout.apps.iterdir():
        if (d.is_dir() and d.name not in keep and (d / STAGED).is_file()
                and d.resolve() != Path(layout.app).resolve()):
            shutil.rmtree(d, ignore_errors=True)
    return folder


def staged_info(layout, ver):
    """What staged.json says about app/<ver> ({'version', 'runtime', 'prerelease', 'notes'}) when stage() finished
    it and it is still whole (its launcher, its VERSION, the Python it needs), else None."""
    folder = layout.apps / ver
    try:
        m = json.loads((folder / STAGED).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None
    if not isinstance(m, dict) or m.get('version') != ver:
        return None
    if versions.display(read_text(folder / 'VERSION')) != ver or not (folder / 'launcher' / 'start.pyw').is_file():
        return None
    rt = m.get('runtime') or ''
    if rt and rt != runtime_id(layout) and read_text(folder / STAGED_PYTHON / 'runtime-id.txt') != rt:
        return None
    return m


def staged(layout, channel='stable', skip=''):
    """The newest release stage() left complete that is newer than the running version, as staged_info() gives it,
    or None. A pre-release counts only on the Preview channel; skip is a version that failed to start."""
    best = None
    if not layout.apps.is_dir():
        return None
    for d in layout.apps.iterdir():
        if not d.is_dir() or d.name == skip or not (d / STAGED).is_file():
            continue
        try:
            if not versions.newer(d.name, layout.version) or (best and not versions.newer(d.name, best['version'])):
                continue
        except ValueError:
            continue
        m = staged_info(layout, d.name)
        if m and (channel == 'preview' or not m.get('prerelease')):
            best = m
    return best


def install_staged(layout, ver):
    """Make a release stage() finished the one the stub starts at its next start: its Python goes to python.next for
    the stub to swap in, current.txt names it (previous.txt the running version), and staged.json goes, so a version
    the stub rolls back from is not installed again by itself."""
    m = staged_info(layout, ver)
    if not m:
        raise UpdateError(f'app/{ver} is not a complete staged release')
    py = layout.apps / ver / STAGED_PYTHON
    nxt = layout.root / 'python.next'
    moved = bool(m.get('runtime')) and m['runtime'] != runtime_id(layout)
    if moved:
        shutil.rmtree(nxt, ignore_errors=True)
        os.replace(py, nxt)
    try:
        switch_to(layout, ver)
    except BaseException:
        if moved:   # the running version stays: it must not start on the new Python
            os.replace(nxt, py)
        raise
    shutil.rmtree(py, ignore_errors=True)
    (layout.apps / ver / STAGED).unlink(missing_ok=True)


def switch_to(layout, ver):
    """Make ver the version the stub starts, keeping the running one as previous."""
    if not (layout.apps / ver / 'launcher' / 'start.pyw').is_file():
        raise UpdateError(f'app/{ver} is not a complete release')
    now = current(layout)
    if now != ver:
        write_text(layout.apps / 'previous.txt', now)
    write_text(layout.apps / 'current.txt', ver)


def rollback(layout):
    """Back to previous.txt's version; False when there is none to go back to."""
    prev = previous(layout)
    if not prev or not (layout.apps / prev / 'launcher' / 'start.pyw').is_file():
        return False
    write_text(layout.apps / 'current.txt', prev)
    (layout.apps / 'previous.txt').unlink(missing_ok=True)
    return True


def prune(layout):
    """Remove every app version but the current and previous one and a staged release newer than the current one,
    an old Python the stub set aside, and stale downloads."""
    now = current(layout)
    keep = {now, previous(layout)}
    if layout.apps.is_dir():
        for d in layout.apps.iterdir():
            if d.is_dir() and d.name not in keep and not _waiting(layout, d.name, now):
                shutil.rmtree(d, ignore_errors=True)
    for d in layout.root.glob('python.old*'):
        shutil.rmtree(d, ignore_errors=True)
    if layout.downloads.is_dir():
        for f in layout.downloads.iterdir():
            if f.suffix in ('.part', '.zip', net.PART_OF):
                f.unlink(missing_ok=True)


def _waiting(layout, name, now):
    """True for a staged release newer than version now: it waits to be installed."""
    try:
        return versions.newer(name, now) and staged_info(layout, name) is not None
    except ValueError:
        return False
