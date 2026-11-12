"""nx2navi.ini: set the game language and keep everything else (keys and syntax as runtime/cfg.c reads them).

A new file starts from nx2navi.ini.example, whose lines are all commented defaults. The language goes into `lang`
(a Switch language code, runtime/ov_system.c): an active `lang = ...` line is rewritten in place, else the commented
`# lang = ...` line of the example is activated, else the line is added to [language] (created at the end if
missing). Other lines, comments and line order stay as they are, byte for byte: the file's byte order mark, its line
ends and a last line without one are kept, so a player's own lines come through an install or an update unchanged.
"""
import os
import re
from pathlib import Path

# runtime/ov_system.c: the codes NX2NAVI_LANG offers. Each language appears once, under the code the game accepts.
LANGUAGES = ('ja', 'en-US', 'en-GB', 'fr', 'fr-CA', 'de', 'it', 'es', 'es-419', 'nl', 'pt', 'pt-BR', 'ru', 'ko',
             'zh-Hans', 'zh-Hant')
# Older installer builds wrote these two codes; they are read as the canonical ones and rewritten on the next install.
ALIASES = {'zh-CN': 'zh-Hans', 'zh-TW': 'zh-Hant'}
# Their text falls back to the Switch shared fonts (runtime/ov_system.c), which the fonts step extracts from the
# firmware folder when the player gives one.
NEED_SHARED_FONTS = ('ko', 'zh-Hans', 'zh-Hant')

ACTIVE = re.compile(r'^(\s*)(?:nx2navi_)?lang(\s*=\s*)(.*?)(\s*)$', re.IGNORECASE)
COMMENTED = re.compile(r'^\s*[#;]\s*(?:nx2navi_)?lang\s*=.*$', re.IGNORECASE)
SECTION = re.compile(r'^\s*\[(.*)\]\s*$')
BOM = '﻿'


def canonical_language(lang):
    """The code LANGUAGES lists for lang: an alias of an older installer build is mapped, anything else is kept."""
    return ALIASES.get(lang, lang)


def set_language(text, lang):
    """text with `lang = <lang>` (an alias written as its canonical code); see the module docstring. Only that line
    changes: a byte order mark, each line's own ending (LF or CRLF) and a last line without one stay as they are, and
    a line that is added ends as the file's first line does."""
    lang = canonical_language(lang)
    bom = BOM if text.startswith(BOM) else ''
    lines = text[len(bom):].splitlines(keepends=True)
    nl = '\r\n' if lines and lines[0].endswith('\r\n') else '\n'

    def split(line):
        body = line.rstrip('\r\n')
        return body, line[len(body):]

    for i, line in enumerate(lines):
        body, end = split(line)
        m = ACTIVE.match(body)
        if m:
            lines[i] = f'{m.group(1)}lang{m.group(2)}{lang}{end}'
            return bom + ''.join(lines)
    for i, line in enumerate(lines):
        body, end = split(line)
        if COMMENTED.match(body):
            lines[i] = f'lang = {lang}{end}'
            return bom + ''.join(lines)
    for i, line in enumerate(lines):
        body, end = split(line)
        m = SECTION.match(body)
        if m and m.group(1).strip().lower() == 'language':
            if not end:   # the header is the file's last line, without an ending
                lines[i] = body + nl
            lines.insert(i + 1, f'lang = {lang}{nl}')
            return bom + ''.join(lines)
    if lines and not split(lines[-1])[1]:
        lines[-1] += nl
    if lines and lines[-1].strip():
        lines.append(nl)
    lines += [f'[language]{nl}', f'lang = {lang}{nl}']
    return bom + ''.join(lines)


def get_language(text):
    for line in text.splitlines():
        m = ACTIVE.match(line)
        if m:
            return m.group(3)
    return None


def read(path):
    """The file's text with its byte order mark and its own line ends. Bytes that are not UTF-8 come back as they are
    (surrogate escapes), so a line in another encoding survives a rewrite. A file saved as UTF-16 (an editor's
    "Unicode", which the game reads too: runtime/cfg.c utf16_text) is decoded as that and written back as UTF-8, as
    the game's own menu writes it."""
    raw = Path(path).read_bytes()
    if raw[:2] in (b'\xff\xfe', b'\xfe\xff'):
        return raw.decode('utf-16', errors='replace')
    return raw.decode('utf-8', errors='surrogateescape')


def write(dest, lang, example=None):
    """Write dest/nx2navi.ini with lang; an existing file keeps every other line byte for byte (set_language).
    Returns the path."""
    path = Path(dest) / 'nx2navi.ini'
    old = None
    if path.is_file():
        text = old = read(path)
    elif example and Path(example).is_file():
        text = Path(example).read_text(encoding='utf-8')
    else:
        text = ''
    new = set_language(text, lang)
    if new == old:
        return path
    tmp = path.with_name('nx2navi.ini.tmp')
    with open(tmp, 'w', encoding='utf-8', errors='surrogateescape', newline='') as f:
        f.write(new)
    os.replace(tmp, path)
    return path
