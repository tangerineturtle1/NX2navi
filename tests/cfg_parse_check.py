"""Build and run tests/cfg_parse_check.c: runtime/cfg.c's reading of nx2navi.ini and the environment, against stand-ins
for what it links with. What the readers get and what the log says about the values:

- a comment after a value (cfg.c cut_comment): cut at a blank followed by ';' or '#' for a key whose row has a set of
  values, the line logged once with its line number, "key = ; note" left unset; kept whole in free text (a row
  without a set, a key without a row), in a value in double quotes and in the environment, where the log still says
  it is part of the value; at most 16 lines, then a count;
- switches written as words (cfg.c switch_word, cfg_bool): false, off and no in any case read as 0 and true, on and
  yes as 1 through cfg_get, cfg_bool and cfg_int, from the file and from the environment, with no line about them;
  the switch of the low-end GPU mode, which is gone (cfg.c lowend_set), takes the same words for its one log line
  and turns nothing on: the mode's keys keep their own defaults and are read by themselves;
- values outside their rows' sets (cfg.c check_value): a 0|1 switch with another value, a number outside its range, a
  word that is none of the menu's choices, each named with its line; none for a file whose values fit, nor for a list
  with numbers in it (the menu's presets); at most 16 lines, then a count;
- a full settings table (cfg.c CFG_MAX_KEYS): one line at load naming the first key not read, one at the first menu
  change not kept;
- a number read of a key without a row whose set value does not parse gives 0 (cfg.c default_number), not a crash.

No game files.

Usage: cfg_parse_check.py
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


CLEAN = '''[frame rate]
pace_smooth = 1
fps_limit = 144
; a comment line
# another
[display]
sharpness = 80
color_gamma = 1.1
vk_present = FIFO
render_scale = 300
dlss_preset = k
pad_layout = xbox_prompts
monitor = 2
'''

BAD = '''[frame rate]
pace_smooth = 0 ; off
fullscreen = false
sharpness = 250
color_gamma = 2
vk_present = vsync
fps_limit = 144 # my monitor
sharpness_typo = 3
cloth_realtime = disabled
window_size = 1280x720 # small
aspect = ; stretch some day
dlss_preset = "k ; quoted"
'''

READ = '''[display]
fullscreen = false
rtss = OFF
hdr_live = Yes
cloth_realtime = on ; the default
pace_smooth = No
vk_present = fifo\t; tear-free
fps_limit = 144 # my monitor
window_size = 1280x720 # small
aspect = ; nothing yet
mods_dir = D:\\My Mods #2
dlss_preset = "k ; quoted"
'''


def run(exe, env, case, text, work, extra_env=None):
    ini = work / f'{case}_{len(text)}.ini'
    ini.write_text(text, encoding='utf-8')
    e = dict(env)
    for k in list(e):
        if k.upper().startswith('NX2NAVI_'):
            del e[k]
    e.update(extra_env or {})
    p = subprocess.run([str(exe), case, str(ini)], env=e, capture_output=True, text=True, errors='replace', timeout=60)
    if p.returncode:
        print(p.stdout, p.stderr)
    return p.returncode, p.stdout


def main():
    work = ROOT / 'build' / 'test' / 'cfg_parse'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('cfg_parse_check.exe' if os.name == 'nt' else 'cfg_parse_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-g', '-Wall', '-Wno-unused-function', '-I', str(rt),
                           str(ROOT / 'tests' / 'cfg_parse_check.c'), str(rt / 'cfg.c'), '-o', str(exe)], env=env)
    bad = 0

    def check(ok, what, out=''):
        nonlocal bad
        print(f'{what}: {"ok" if ok else "FAIL"}', flush=True)
        if not ok:
            bad += 1
            print(out)

    # values that fit: no line about them
    rc, out = run(exe, env, 'load', CLEAN, work)
    lines = [l for l in out.splitlines() if l.startswith('log: cfg:') and '=' in l and 'line' in l]
    check(rc == 0 and not lines, 'a file whose values fit logs nothing about them', out)

    # values that do not: one line each, with the line number; a comment after a value of a row with a set: cut, and
    # one line
    rc, out = run(exe, env, 'load', BAD, work, {'NX2NAVI_SHARPNESS': '500', 'NX2NAVI_VK_PRESENT': 'fifo'})
    want = [
        "line 2: pace_smooth = '0', and '; off' after it is a comment",
        "line 4: sharpness = '250' is not a number in 0..100",
        "line 5: color_gamma = '2' is not a number in 0.8..1.2",
        "line 6: vk_present = 'vsync' is none of the menu's choices mailbox|fifo|immediate",
        "line 7: fps_limit = '144', and '# my monitor' after it is a comment",
        "line 9: cloth_realtime = 'disabled' is not 0, 1, on, off, true, false, yes or no",
        "line 10: window_size = '1280x720 # small' a ';' or '#' after the value is part of it",
        "line 11: aspect = '', and '; stretch some day' after it is a comment",
        "line 12: dlss_preset = 'k ; quoted' is none of the menu's choices default|k|j; a ';' or '#' after the value "
        "is part of it",
        "the environment's NX2NAVI_SHARPNESS = '500' is not a number in 0..100",
    ]
    for w in want:
        check(rc == 0 and w in out, f'logged: {w}', out if w not in out else '')
    cfg_lines = [l for l in out.splitlines() if l.startswith('log: cfg: nx2navi.ini line') and 'unknown key' not in l]
    check(len(cfg_lines) == 9, f'one line each for the nine lines, none for "fullscreen = false" ({len(cfg_lines)})',
          out)
    check(not any(' line 2: ' in l and 'is not 0' in l for l in cfg_lines), 'a cut value is checked as cut', out)
    check('NX2NAVI_VK_PRESENT' not in out, 'an environment value that fits is not logged', out)
    check("got: pace_smooth '0'" in out, 'the value is read without its comment', out)

    # what the readers get: switches written as words, cut comments, free text kept whole, the environment as written
    rc, out = run(exe, env, 'read', READ, work, {'NX2NAVI_DYN_RES': 'TRUE', 'NX2NAVI_SHADOW_PCF': '0 ; env note'})
    got = {l.split()[1]: l.split(' ', 2)[2] for l in out.splitlines() if l.startswith('got: ')}
    want = {'PACE_SMOOTH': "'0' bool 0 int 0", 'FULLSCREEN': "'0' bool 0 int 0", 'RTSS': "'0' bool 0 int 0",
            'HDR_LIVE': "'1' bool 1 int 1", 'CLOTH_REALTIME': "'1' bool 1 int 1", 'FPS_LIMIT': "'144'",
            'VK_PRESENT': "'fifo'", 'ASPECT': "'(unset)'", 'WINDOW_SIZE': "'1280x720 # small'",
            'DLSS_PRESET': "'k ; quoted'", 'MODS_DIR': "'D:\\My Mods #2'", 'DYN_RES': "'1' bool 1 int 1",
            'SHADOW_PCF': "'0 ; env note' bool 1 int 0"}
    for k, w in want.items():
        check(rc == 0 and got.get(k) == w, f'read: {k} {w}', f'got {got.get(k)!r}\n{out}' if got.get(k) != w else '')
    cut = [l for l in out.splitlines() if 'after it is a comment' in l]
    check(len(cut) == 4 and any("line 7: vk_present = 'fifo', and '; tear-free'" in l for l in cut),
          f'four cut lines, a tab before the comment too ({len(cut)})', out)
    check(not any(w in out for w in ('fullscreen =', 'rtss =', 'hdr_live =', 'pace_smooth =', 'NX2NAVI_DYN_RES =')),
          'no line about switches written as words', out)
    check("the environment's NX2NAVI_SHADOW_PCF = '0 ; env note' is not 0, 1, on, off, true, false, yes or no; a ';' "
          "or '#' after the value is part of it" in out, "the environment's comment stays in its value", out)

    # at most 16 lines, then a count
    many = '\n'.join(f'sharpness = {200 + i}' if i % 2 else f'fullscreen = no{i}' for i in range(2)) + '\n'
    many = ''.join(f'[s{i}]\nsharpness = {200 + i}\n' for i in range(1)) + ''.join(
        f'{k} = 7\n' for k in ('fullscreen', 'pace_smooth', 'vk_present', 'sharpness', 'color_gamma', 'perf_overlay',
                               'upscaler', 'standin_filter', 'dlss', 'dlss_preset', 'framegen', 'aa', 'aspect',
                               'texture_detail', 'color_preset', 'device_profile', 'mods_rebuild', 'operation_mode',
                               'text_speed', 'sprint_mode'))
    rc, out = run(exe, env, 'load', many, work)
    listed = sum(1 for l in out.splitlines() if l.startswith('log: cfg: nx2navi.ini line') and "= '7'" in l)
    check(rc == 0 and listed == 16 and 'more values outside their settings' in out,
          f'20 values outside their sets: {listed} lines and a count', out)
    rc, out = run(exe, env, 'load', ''.join(f'vk_present = fifo ; note {i}\n' for i in range(20)), work)
    listed = sum(1 for l in out.splitlines() if 'after it is a comment' in l)
    check(rc == 0 and listed == 16 and '4 more comments after a value are not listed' in out,
          f'20 comments after values: {listed} lines and a count', out)
    # more keys than the table holds: one line at load, one for the menu's changes
    full = ''.join(f'cheat_{0x1000 + i:08x} = 1\n' for i in range(600)) + 'low_latency = 1\n'
    rc, out = run(exe, env, 'full', full, work)
    load_lines = [l for l in out.splitlines() if 'has more than 512 keys' in l]
    set_lines = [l for l in out.splitlines() if 'settings table is full' in l]
    check(rc == 0 and len(load_lines) == 1 and 'line 513 (CHEAT_' in load_lines[0] and 'got: low_latency (unset)' in out,
          'a file with 601 keys: one line at load from line 513 on', out)
    check(rc == 0 and len(set_lines) == 1 and 'VK_PRESENT is not kept' in set_lines[0],
          'two menu changes past a full table: one line', out)
    # the switch of the low-end GPU mode, which is gone (cfg.c lowend_set): on, it turns none of the mode's keys on and
    # gives none another default, and the log says once that the line does nothing; the same words as cfg_bool, from
    # the file and from the environment. The mode's keys are read by themselves, with or without the switch.
    gone = 'the low-end GPU mode was removed'
    unset = "mesh_emulation '(unset)' vram_reserve_mb '(unset)' default '0'"
    for text, extra, want, said in (
            ('low_end_gpu = Off\n', None, f'bool 0 {unset}', 0),
            ('low_end_gpu = 0 ; note\n', None, f'bool 0 {unset}', 0),
            ('low_end_gpu = yes\n', None, f'bool 1 {unset}', 1),
            ('low_end_gpu = 1\n', None, f'bool 1 {unset}', 1),
            ('; nothing\n', {'NX2NAVI_LOW_END_GPU': 'FALSE'}, f'bool 0 {unset}', 0),
            ('; nothing\n', {'NX2NAVI_LOW_END_GPU': '0 ; x'}, f'bool 1 {unset}', 1),
            ('mesh_emulation = 1\nvram_reserve_mb = 512\n', None,
             "bool 0 mesh_emulation '1' vram_reserve_mb '512' default '0'", 0),
            ('low_end_gpu = 1\nmesh_emulation = 1\n', None,
             "bool 1 mesh_emulation '1' vram_reserve_mb '(unset)' default '0'", 1)):
        rc, out = run(exe, env, 'lowend', text, work, extra)
        what = text.strip().replace('\n', ', ') if extra is None else f"the environment's {extra['NX2NAVI_LOW_END_GPU']!r}"
        check(rc == 0 and f'got: {want}' in out and out.count(gone) == said,
              f'low end: {what}: {want}, the removal said {said} time(s)', out)
    rc, out = run(exe, env, 'norow', CLEAN, work, {'NX2NAVI_NO_ROW_CHECK': 'abc'})
    check(rc == 0 and 'got: cfg_int 0 cfg_double 0' in out, 'cfg_int of a set, unparsable key without a row: 0', out)
    print(f'cfg_parse_check: {"FAILED" if bad else "ok"}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
