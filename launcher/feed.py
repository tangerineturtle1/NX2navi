"""Where updates come from.

Every release carries NX2navi-update.json (tools/make_launcher.py writes it):

    {"format": 1, "version": "0.3.11", "build": "0.3.11+abc1234", "notes": "...",
     "app":     {"name": "NX2navi-app-0.3.11.zip", "size": n, "sha256": "..."},
     "runtime": {"id": "py3.14.9-1", "name": "NX2navi-runtime-py3.14.9-1.zip", "size": n, "sha256": "..."},
     "toolchain": "<lock digest>"}

The feed (app/<version>/feed.json, or the player's override in settings) is either
    {"type": "github", "repo": "owner/name"}   the newest GitHub release (or pre-release, on the preview channel)
                                                  that has the manifest; a token reads a private repository
    {"type": "url", "url": "https://.../NX2navi-update.json"}   one manifest; its files sit beside it

history() lists a GitHub feed's releases for a downgrade (Settings > Downgrade).
"""
import json
import re
import time
import urllib.parse
from pathlib import Path

from . import net, versions

MANIFEST = 'NX2navi-update.json'
DEFAULT = {'type': 'github', 'repo': 'tangerineturtle1/NX2navi'}
GITHUB_API = 'https://api.github.com'


class FeedError(Exception):
    pass


class Release:
    def __init__(self, manifest, notes='', page='', assets=None, headers=None, prerelease=False):
        self.manifest = manifest
        self.version = manifest['version']
        self.build = manifest.get('build', self.version)
        self.notes = (manifest.get('notes') or notes or '').strip()
        self.page = page
        self.assets = assets or {}       # name -> url
        self.headers = headers or {}     # extra request headers for the asset urls (a private repository's token)
        self.prerelease = prerelease

    def asset(self, key):
        """(url, headers, size, sha256) of the manifest's 'app' or 'runtime' file."""
        entry = self.manifest.get(key)
        if not entry:
            raise FeedError(f'the {self.version} manifest has no {key} file')
        name, size, sha = entry.get('name'), entry.get('size'), entry.get('sha256')
        # The name becomes a file in downloads/: a bare file name only. Every file is checked against its size and
        # sha256, so a manifest without them is refused rather than downloaded unchecked.
        if (not isinstance(name, str) or not name or name in ('.', '..') or name != Path(name).name
                or '/' in name or '\\' in name or ':' in name):
            raise FeedError(f'the {self.version} manifest names its {key} file {name!r}, not a plain file name')
        if (not isinstance(size, int) or isinstance(size, bool) or size <= 0 or not isinstance(sha, str)
                or not re.fullmatch(r'[0-9a-fA-F]{64}', sha)):
            raise FeedError(f'the {self.version} manifest gives no size and sha256 for {name}')
        url = self.assets.get(name)
        if not url:
            raise FeedError(f'the {self.version} release lacks {name}')
        return url, self.headers, size, sha


def load(layout, settings):
    feed = dict(DEFAULT)
    try:
        if layout.feed_file.is_file():
            feed.update(json.loads(layout.feed_file.read_text(encoding='utf-8')))
    except (OSError, ValueError):
        pass
    if settings.get('feed_repo'):
        feed = {'type': 'github', 'repo': settings['feed_repo']}
    return feed


def _check_manifest(m, where):
    if not isinstance(m, dict) or m.get('format') != 1 or 'version' not in m or 'app' not in m:
        raise FeedError(f'{where}: not an NX2navi update manifest')
    versions.parse(m['version'])
    return m


def retry_text(headers, now=None):
    """When GitHub takes requests again, for the message of a refused one: "after 14:32 (in 25 minutes)". From
    X-RateLimit-Reset, the time the hourly allowance is whole again, when the response says the allowance is used
    up (X-RateLimit-Remaining: 0), else from Retry-After, the seconds to wait. '' when the response says neither
    or says something that cannot be right (more than a day)."""
    now = time.time() if now is None else now
    try:
        if headers.get('X-RateLimit-Remaining') == '0' and headers.get('X-RateLimit-Reset'):
            wait = int(headers.get('X-RateLimit-Reset')) - now
        elif headers.get('Retry-After'):
            wait = int(headers.get('Retry-After'))
        else:
            return ''
    except (AttributeError, TypeError, ValueError):
        return ''
    if wait > 86400:
        return ''
    if wait <= 0:   # the allowance is whole again by this PC's clock
        return 'in a minute'
    minutes = int(-(-wait // 60))
    return (f'after {time.strftime("%H:%M", time.localtime(now + wait))} '
            f'(in {minutes} minute{"" if minutes == 1 else "s"})')


def _github_list(feed, token, version):
    """The repository's releases as GitHub's API lists them (newest first) and the headers its files need."""
    api = feed.get('api', GITHUB_API).rstrip('/')
    headers = {'Accept': 'application/vnd.github+json', 'X-GitHub-Api-Version': '2022-11-28'}
    if token:
        headers['Authorization'] = f'Bearer {token}'
    try:
        releases = net.get_json(f'{api}/repos/{feed["repo"]}/releases?per_page=20', headers, version=version)
    except Exception as e:
        code = getattr(e, 'code', None)
        if code == 404:
            raise FeedError(f'no releases found for {feed["repo"]} (a private repository needs a token)') from e
        if code in (403, 429):   # GitHub answers either when the allowance of requests is used up
            when = retry_text(getattr(e, 'headers', None))
            raise FeedError(f'GitHub refused the request (rate limit); try again {when or "in an hour"}') from e
        raise FeedError(f'GitHub could not be reached: {e}') from e
    return releases


def _github_assets(r, token):
    """(file name -> url, the headers those urls need) of a release that carries the manifest, or None."""
    assets = {a['name']: a for a in r.get('assets', [])}
    if MANIFEST not in assets:
        return None
    if token:   # a private repository's files come through the API with the token
        return ({n: a['url'] for n, a in assets.items()},
                {'Accept': 'application/octet-stream', 'Authorization': f'Bearer {token}'})
    return {n: a['browser_download_url'] for n, a in assets.items()}, {}


def _github_release(r, urls, asset_headers, version):
    try:
        m = _check_manifest(json.loads(net.get_bytes(urls[MANIFEST], asset_headers, version=version)),
                            f'{r.get("tag_name")}/{MANIFEST}')
    except (FeedError, ValueError, OSError) as e:
        raise FeedError(f'{r.get("tag_name")}: {e}') from e
    return Release(m, r.get('body') or '', r.get('html_url', ''), urls, asset_headers, bool(r.get('prerelease')))


def _github(feed, channel, token, version):
    # The API lists the newest release first: the first one the channel takes that has a manifest is the answer.
    for r in _github_list(feed, token, version):
        if r.get('draft') or (r.get('prerelease') and channel != 'preview'):
            continue
        found = _github_assets(r, token)
        if found:
            return _github_release(r, *found, version)
    return None


class Listed:
    """A release in the feed's list (history), read without its manifest: release() fetches that."""

    def __init__(self, version, date, notes, prerelease, raw, urls, headers):
        self.version, self.date, self.notes, self.prerelease = version, date, notes, prerelease
        self._raw, self._urls, self._headers = raw, urls, headers

    def release(self, version='dev'):
        """The Release, with its manifest; FeedError when that names another version than the release's tag."""
        rel = _github_release(self._raw, self._urls, self._headers, version)
        if versions.display(rel.version) != self.version:
            raise FeedError(f'the release {self.version} holds the manifest of {rel.version}')
        return rel


def history(feed, channel='stable', token='', version='dev'):
    """The releases the feed offers on the channel that carry a manifest, newest first, as Listed (one request; no
    manifest is read). A URL feed names one manifest and has no list: []."""
    if feed.get('type') == 'url':
        return []
    if feed.get('type') != 'github':
        raise FeedError(f'unknown feed type {feed.get("type")!r}')
    out = []
    for r in _github_list(feed, token, version):
        if r.get('draft') or (r.get('prerelease') and channel != 'preview'):
            continue
        found = _github_assets(r, token)
        ver = versions.display((r.get('tag_name') or '').lstrip('v'))
        try:
            versions.parse(ver)
        except ValueError:
            continue
        if found:
            out.append(Listed(ver, (r.get('published_at') or '')[:10], r.get('body') or '', bool(r.get('prerelease')),
                              r, *found))
    out.sort(key=lambda e: versions.parse(e.version), reverse=True)
    return out


def _url(feed, version):
    url = feed['url']
    try:
        m = _check_manifest(net.get_json(url, version=version), url)
    except FeedError:
        raise
    except Exception as e:
        raise FeedError(f'{url}: {e}') from e
    base = url.rsplit('/', 1)[0] + '/'
    assets = {e['name']: urllib.parse.urljoin(base, e['name']) for e in (m.get('app'), m.get('runtime')) if e}
    return Release(m, assets=assets)


def latest(feed, channel='stable', token='', version='dev'):
    """The newest release the feed offers on the channel, or None when it has none."""
    if feed.get('type') == 'url':
        return _url(feed, version)
    if feed.get('type') == 'github':
        return _github(feed, channel, token, version)
    raise FeedError(f'unknown feed type {feed.get("type")!r}')


def plain_notes(text, limit=12):
    """Release notes as short lines for the window: markdown bullets and headings without their marks."""
    out = []
    for line in (text or '').splitlines():
        s = line.strip()
        if not s or s.startswith(('<!--', '```', '---')):
            continue
        s = s.lstrip('#').strip()
        for mark in ('- ', '* ', '+ '):
            if s.startswith(mark):
                s = s[2:]
        s = s.replace('**', '').replace('`', '')
        if s:
            out.append(s)
        if len(out) >= limit:
            break
    return out
