"""HTTPS for the launcher: JSON requests and file downloads that resume, report progress, can be cancelled and are
checked against a sha256. The standard library (urllib), and on Windows the truststore package of the launcher's
Python for Windows' own certificate check.

Two checks can accept a server's certificate, tried in turn by request():

- Python's: OpenSSL with the certificates of Windows' ROOT and CA stores (ssl.create_default_context). It knows only
  the certificates already in those stores, while Windows holds few roots and downloads the others the first time a
  Windows program needs them. And it takes a stored certificate before the server's copy of the same name and key: a
  store that still holds an old, expired cross-signature of the root a server's chain leads to makes it report
  "certificate has expired" for a valid chain (repo.msys2.org's chain on a PC whose CA store still holds the
  "ISRG Root X2" cross-signature that expired in September 2025).
- Windows': Windows' own chain engine, which Windows programs check servers with (CertGetCertificateChain and
  CertVerifyCertificateChainPolicy, through truststore.SSLContext). It weighs every path the stores and the server
  allow, prefers a valid one, and downloads a missing root from Windows Update.

A certificate both refuse ends the request with CertificateError: what each check said, whether the PC's clock agrees
with the server's, and what to do."""
import email.utils
import hashlib
import json
import os
import ssl
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import timezone
from http.client import IncompleteRead
from pathlib import Path

from . import NAME

CHUNK = 1 << 20
CLOCK_TOLERANCE = 3600   # seconds between this PC's clock and a server's beyond which this PC's is wrong


class Cancelled(Exception):
    pass


class DownloadError(Exception):
    pass


class CertificateError(DownloadError):
    """No certificate check accepts a server's certificate. Asking again gets the same answer, so download() does
    not retry it."""


def user_agent(version='dev'):
    return f'{NAME}-Launcher/{version}'


def _under_wine():
    try:
        import ctypes
        return hasattr(ctypes.WinDLL('ntdll'), 'wine_get_version')
    except Exception:
        return False


def _windows_check():
    """An SSLContext that leaves the certificate to Windows' own verifier, or None off Windows, in a Python without
    truststore (a checkout's own Python) and under Wine. Wine's crypt32 accepts a chain that stops at an issuer it
    does not know (CERT_TRUST_IS_PARTIAL_CHAIN passes its SSL policy), and its roots are the host's, which Python's
    check reads already."""
    if os.name != 'nt' or _under_wine():
        return None
    try:
        import truststore
        return truststore.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    except Exception:   # not installed, or crypt32 not reachable through ctypes
        return None


_CHECKS = None      # [(name, SSLContext)] in the order request() tries them
_ACCEPTED = {}      # host -> index into _CHECKS of the check that accepted its certificate last


def _checks():
    global _CHECKS
    if _CHECKS is None:
        checks = [("Python's check", ssl.create_default_context())]
        windows = _windows_check()
        if windows is not None:
            checks.append(("Windows' check", windows))
        _CHECKS = checks
    return _CHECKS


LOOPBACK = ('127.0.0.1', 'localhost', '::1')


def _plain_ok(url):
    """Plain HTTP only to this PC (the tests' local servers): anything fetched from elsewhere comes over TLS."""
    return urllib.parse.urlsplit(url).hostname in LOOPBACK


class _Redirect(urllib.request.HTTPRedirectHandler):
    """Follows a redirect only to https (or to this PC), and drops the Authorization header when the redirect leaves
    the host it was meant for: urllib copies every header given to the Request onto the redirected one, and GitHub
    sends asset downloads on to its file servers."""
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if urllib.parse.urlsplit(newurl).scheme.lower() != 'https' and not _plain_ok(newurl):
            raise urllib.error.HTTPError(newurl, code, f'redirect to {newurl} refused: not https', headers, fp)
        new = super().redirect_request(req, fp, code, msg, headers, newurl)
        if new is not None and urllib.parse.urlsplit(newurl).hostname != urllib.parse.urlsplit(req.full_url).hostname:
            new.remove_header('Authorization')
        return new


def _open(url, h, timeout, ctx=None):
    handlers = [_Redirect()] + ([urllib.request.HTTPSHandler(context=ctx)] if ctx is not None else [])
    return urllib.request.build_opener(*handlers).open(urllib.request.Request(url, headers=h), timeout=timeout)


def request(url, headers=None, timeout=30, version='dev'):
    """The open response to a GET of url. An https URL is tried with each certificate check in turn, first the one
    that accepted the host last, until one accepts the server; CertificateError when none does. Plain http is
    refused but for this PC's own address."""
    h = {'User-Agent': user_agent(version)}
    h.update(headers or {})
    if urllib.parse.urlsplit(url).scheme.lower() != 'https':
        if not _plain_ok(url):
            raise DownloadError(f'{url}: not an https address')
        return _open(url, h, timeout)
    host = urllib.parse.urlsplit(url).hostname or ''
    checks = _checks()
    first = min(_ACCEPTED.get(host, 0), len(checks) - 1)
    refused = []
    for i in [first] + [i for i in range(len(checks)) if i != first]:
        name, ctx = checks[i]
        try:
            r = _open(url, h, timeout, ctx)
        except urllib.error.URLError as e:
            if not isinstance(e.reason, ssl.SSLCertVerificationError):
                raise
            refused.append((name, e.reason))
            continue
        _ACCEPTED[host] = i
        return r
    raise CertificateError(certificate_problem(url, refused, version)) from refused[0][1]


def server_clock(url, timeout=10, version='dev'):
    """The time (epoch seconds) of the Date header a plain-HTTP HEAD request to url answers with, without following a
    redirect, or None without an answer. A PC whose clock is wrong cannot check any certificate, so its clock is
    compared over HTTP; the answer is not authenticated and only chooses the advice of an error message."""
    class Stay(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, *args, **kwargs):
            return None

    req = urllib.request.Request(url, method='HEAD', headers={'User-Agent': user_agent(version)})
    try:
        with urllib.request.build_opener(Stay).open(req, timeout=timeout) as r:
            date = r.headers.get('Date')
    except urllib.error.HTTPError as e:   # the redirect to https, or any other status: its Date is as good
        date = e.headers.get('Date')
        e.close()
    except Exception:   # no answer, whatever the reason (refused, timed out, not HTTP)
        return None
    if not date:
        return None
    try:
        when = email.utils.parsedate_to_datetime(date)
    except (TypeError, ValueError):
        return None
    return (when if when.tzinfo else when.replace(tzinfo=timezone.utc)).timestamp()


def _said(err):
    said = getattr(err, 'verify_message', None) or (str(err.args[-1]) if err.args else type(err).__name__)
    return said.strip().rstrip('.')


def _local(t):
    return time.strftime('%Y-%m-%d %H:%M', time.localtime(t))


def certificate_problem(url, refused, version='dev'):
    """The message of a CertificateError for url: what each check in refused ([(name, error)]) said, then the advice
    that fits this PC's clock."""
    host = urllib.parse.urlsplit(url).hostname or url
    said = '; '.join(f'{name}: {_said(e)}' for name, e in refused)
    text = f'{url}: the certificate of {host} was not accepted ({said}).'
    where = 'Windows Settings > Time & language > Date & time' if os.name == 'nt' else "the system's settings"
    ours, theirs = time.time(), server_clock(f'http://{host}/', version=version)
    if theirs is not None and abs(ours - theirs) > CLOCK_TOLERANCE:
        return (f"{text} This PC's clock reads {_local(ours)}, but {host} says it is {_local(theirs)}: set the date, "
                f'time and time zone in {where}, then try again.')
    clock = (f"This PC's clock agrees with {host}'s." if theirs is not None else
             f"Check that this PC's date, time and time zone are right ({where}).")
    return (f'{text} {clock} Security software that scans encrypted connections (HTTPS or SSL scanning) or a proxy '
            'may be replacing the certificate: let NX2navi through it or turn that scanning off, then try again.')


def _check_eof(response):
    # HTTPResponse.read(n) accepts an early EOF on a Content-Length body and leaves the missing byte count in length.
    remaining = getattr(response, 'length', None)
    if remaining:
        raise ConnectionError(f'connection closed with {remaining} response bytes still expected')


def get_bytes(url, headers=None, timeout=30, version='dev', limit=64 << 20, retries=3):
    """The body of a GET of url, at most limit bytes. A cut connection, a timeout or a server error (5xx, 429) is tried
    again after 1, then 2 seconds, as download's are: one connection reset during Gather Build Tools (the package
    index read by toolchain.ensure_zbic) ended the install. A refused certificate or another HTTP error is not."""
    last_error = None
    for attempt in range(retries):
        try:
            with request(url, headers, timeout, version) as r:
                data = r.read(limit + 1)
                if len(data) > limit:
                    raise DownloadError(f'{url}: more than {limit >> 20} MB')
                _check_eof(r)
            break
        except urllib.error.HTTPError as e:
            if e.code != 429 and e.code < 500:
                raise
            last_error = e
        except (urllib.error.URLError, OSError, IncompleteRead) as e:   # not DownloadError or CertificateError
            last_error = e
        if attempt + 1 < retries:
            time.sleep(min(2 ** attempt, 8))
    else:
        raise last_error
    return data


def get_json(url, headers=None, timeout=30, version='dev'):
    return json.loads(get_bytes(url, headers, timeout, version).decode('utf-8'))


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while b := f.read(CHUNK):
            h.update(b)
    return h.hexdigest()


def download(url, dest, sha256=None, size=None, progress=None, cancel=None, headers=None, retries=4, version='dev'):
    """Fetch url to dest through dest.part, resuming a part file a cut connection left. progress(done, total) after
    every chunk; cancel() returning True raises Cancelled. A wrong size or sha256 deletes the file and raises
    DownloadError. A dest already holding the expected sha256 is kept without a request."""
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    if sha256 and dest.is_file() and sha256_file(dest) == sha256.lower():
        if progress:
            progress(dest.stat().st_size, dest.stat().st_size)
        return dest
    part = dest.with_name(dest.name + '.part')
    last_error = None
    for attempt in range(retries):
        have = part.stat().st_size if part.is_file() else 0
        h = dict(headers or {})
        if have:
            h['Range'] = f'bytes={have}-'
        try:
            with request(url, h, 60, version) as r:
                status = getattr(r, 'status', 200)
                if have and status != 206:   # the server ignores Range: start over
                    have = 0
                total = int(r.headers.get('Content-Length') or 0) + have or size or 0
                with open(part, 'ab' if have else 'wb') as f:
                    done = have
                    while True:
                        if cancel and cancel():
                            raise Cancelled()
                        b = r.read(CHUNK)
                        if not b:
                            _check_eof(r)
                            break
                        f.write(b)
                        done += len(b)
                        if size and done > size:   # stop a server that sends more than the file: it would fill the disk
                            raise DownloadError(f'{url}: more than the expected {size} bytes')
                        if progress:
                            progress(done, total or done)
            break
        except (Cancelled, CertificateError):
            raise
        except DownloadError:
            part.unlink(missing_ok=True)
            raise
        except urllib.error.HTTPError as e:
            if e.code == 416 and have:   # the part file is complete or longer than the file: start over
                part.unlink(missing_ok=True)
                last_error = e
                continue
            if e.code in (401, 403, 404, 410) or 300 <= e.code < 400:   # 3xx: a redirect _Redirect refused
                raise DownloadError(f'{url}: HTTP {e.code}') from e
            last_error = e
        except (urllib.error.URLError, OSError, IncompleteRead) as e:
            last_error = e
        time.sleep(min(2 ** attempt, 16))
    else:
        raise DownloadError(f'{url}: {last_error}')
    got = part.stat().st_size
    if size and got != size:
        part.unlink(missing_ok=True)
        raise DownloadError(f'{url}: {got} bytes, expected {size}')
    if sha256:
        digest = sha256_file(part)
        if digest != sha256.lower():
            part.unlink(missing_ok=True)
            raise DownloadError(f'{url}: sha256 {digest}, expected {sha256.lower()}')
    os.replace(part, dest)
    return dest
