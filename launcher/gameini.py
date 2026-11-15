"""The game's own settings, nx2navi.ini in the game folder, for the few the launcher's Settings switch.

Read the way runtime/cfg.c reads the file: a UTF-8 byte order mark is skipped, lines starting with #, ; or [ are
not settings, a key is matched without regard to case and with or without its NX2NAVI_ prefix, a value in double
quotes loses them, an empty value leaves the setting as it was (its default, or an earlier line's value), and the last
line with a value wins. For a key whose row in runtime/cfg.c has a set of values (choices=True: numbers, ranges,
words), a blank followed by ';' or '#' starts a comment after a value that is not in double quotes, as cfg.c
cut_comment reads it; free text (a path, a name, the list of mods) keeps its whole value. Sections do not matter to
the game; a key the file does not have yet goes under its section, as the game writes it.
A change rewrites only the line it touches, through a temporary file: the other lines keep their bytes (the byte
order mark, line ends, text that is not UTF-8). A file that exists but cannot be read is never rewritten, as
runtime/cfg.c cfg_save_to does not: rebuilding it from nothing would lose its lines.
"""
import os
import re
from pathlib import Path

PREFIX = 'NX2NAVI_'
BOM = '﻿'


def _path(game_dir):
    return Path(game_dir) / 'nx2navi.ini'


def _key(text):
    k = text.strip().upper()
    return k[len(PREFIX):] if k.startswith(PREFIX) else k


def _text(game_dir):
    """The file's text with its own line ends (bytes that are not UTF-8 kept as surrogates), '' when it does not exist.
    Any other error is raised."""
    try:
        with open(_path(game_dir), encoding='utf-8', errors='surrogateescape', newline='') as f:
            return f.read()
    except FileNotFoundError:
        return ''


def _setting(line, choices=False):
    """(key, value) of a settings line, or None. choices: the key has a set of values, so its value ends at a comment
    (the first blank followed by ';' or '#' after the '=', "key = ; note" giving ''), unless it is in double quotes."""
    s = line.lstrip(BOM).strip()
    if not s or s[0] in '#;[' or '=' not in s:
        return None
    k, raw = s.split('=', 1)
    v = raw.strip()
    quoted = len(v) >= 2 and v[0] == v[-1] == '"'
    if choices and not quoted:
        m = re.search(r'[ \t][;#]', raw)
        if m:
            v = raw[:m.start()].strip()
    if len(v) >= 2 and v[0] == v[-1] == '"':
        v = v[1:-1]
    return _key(k), v


def get(game_dir, key, choices=False):
    """The value the file gives key ('' when it gives none: the game's default applies); choices as _setting. Raises
    OSError when the file exists but cannot be read."""
    want, value = _key(key), ''
    for line in _text(game_dir).splitlines():
        kv = _setting(line, choices)
        if kv and kv[0] == want and kv[1]:
            value = kv[1]
    return value


def put(game_dir, section, key, value, choices=False):
    """Write key = value: on the key's last line that has a value as the game reads it (choices as _setting; else its
    last line) when the file has one, else under [section] (made at the end when the file has none). The line is
    written again whole, without a comment it had, as runtime/cfg.c cfg_save_to writes it. Raises OSError, the file
    unchanged, when it exists but cannot be read or the new one cannot be put in place."""
    text = _text(game_dir)
    bom = BOM if text.startswith(BOM) else ''
    lines = text[len(bom):].splitlines(keepends=True)
    want = _key(key)
    entry = f'{key.lower()} = {value}'
    nl = '\r\n' if lines and lines[0].endswith('\r\n') else '\n'
    mine = [i for i, line in enumerate(lines) if (_setting(line) or ('',))[0] == want]
    valued = [i for i in mine if _setting(lines[i], choices)[1]]
    at = (valued or mine or [-1])[-1]
    if at >= 0:
        end = lines[at][len(lines[at].rstrip('\r\n')):] or nl
        lines[at] = entry + end
    else:
        head = next((i for i, line in enumerate(lines) if line.strip().lower() == f'[{section.lower()}]'), -1)
        if head >= 0:
            if not lines[head].endswith(('\n', '\r')):   # the header is the file's last line
                lines[head] += nl
            lines.insert(head + 1, entry + nl)
        else:
            if lines and not lines[-1].endswith(('\n', '\r')):
                lines[-1] += nl
            lines += [nl] if lines else []
            lines += [f'[{section}]{nl}', entry + nl]
    path = _path(game_dir)
    tmp = path.with_name(path.name + '.tmp')
    try:
        with open(tmp, 'w', encoding='utf-8', errors='surrogateescape', newline='') as f:
            f.write(bom + ''.join(lines))
        os.replace(tmp, path)
    finally:
        tmp.unlink(missing_ok=True)
