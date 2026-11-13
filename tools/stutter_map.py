"""The stutter map of a play session, from its log (runtime/stutter_map.c, NX2NAVI_STUTTER_MAP=1), and what a game
folder recorded that a pipeline list lacks.

    python tools/stutter_map.py report LOG [LOG ...] [--out FILE.md]
    python tools/stutter_map.py missing GAME_DIR LIST_FILE

report: each log's last "stutter map" block (the one written at exit, or the last periodic one when the game did not
exit cleanly) gives the areas ranked by their hitches with the causes, and the pipelines draws met missing that cost
the most; the "stutter:" lines add how the hitches spread over the session. Several logs (one session's rotated files,
or several sessions) are reported one after the other, then summed by area.

missing: the pipeline records of GAME_DIR's pipeline-keys.bin and the shader variants of its shader-answers.bin that
LIST_FILE (tools/pipeline_list.py merge) does not hold: what `pipeline_list.py merge NEW LIST_FILE GAME_DIR` would
add, so the next installs build them before play instead of meeting them in it. The files are derived from game data;
the list stays out of the repository.
"""
import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pipeline_list  # noqa: E402

HEAD = re.compile(r'stutter map \(([^)]*)\): ([\d.]+) min of play in (\d+) areas; (\d+) hitches in play and (\d+) after '
                  r'area loads; (?:(\d+) long frames were a change of pace.*?not hitches; )?in play by cause:(.*)$')
AREA = re.compile(r'stutter map:   (.+?): ([\d.]+) min at (\d+) fps; hitches (\d+) \(([\d.]+) a minute(.*?)\), worst '
                  r'([\d.]+) ms \(frame (\d+)\); over 25 ms (\d+); after its loads (\d+); draws that met a pipeline '
                  r'missing (\d+), a shader (\d+), ([\d.]+) ms on the replay thread')
CAUSE = re.compile(r'; (\w+) (\d+), ([\d.]+) ms avg')
PIPES = re.compile(r'stutter map: (\d+) pipelines draws met missing: (\d+) that this install had recorded before .*?, '
                   r'(\d+) new to it, (\d+) with a shader translated in play, (\d+) still translating when met')
PIPE = re.compile(r'stutter map:   (mesh|vertex) (\S+)/(\S+) first met in (.+?): (\d+) draws met it missing, (\d+) its '
                  r'shader, ([\d.]+) ms on the replay thread; usable after (\d+) ms at most \((\d+) times\); (.*)$')
HITCH = re.compile(r'stutter: frame (\d+) at ([\d.]+) s, ([\d.]+) ms \(frames about ([\d.]+) ms\) in (.+?)(, after its '
                   r'load)?: .*?\(charged to (\w+)\)')


def read_log(path):
    """(the last map block as (head, areas, pipes summary, pipes), hitch lines) of one log."""
    block, cur, hitches = None, None, []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = HEAD.search(line)
        if m:
            cur = {'head': m.groups(), 'areas': [], 'sum': None, 'pipes': []}
            block = cur
            continue
        if cur is not None:
            m = AREA.search(line)
            if m:
                g = m.groups()
                cur['areas'].append({'name': g[0], 'min': float(g[1]), 'fps': int(g[2]), 'hitch': int(g[3]),
                                     'causes': {c: int(n) for c, n, _ in CAUSE.findall(g[5])}, 'worst': float(g[6]),
                                     'over25': int(g[8]), 'load': int(g[9]), 'pmiss': int(g[10]), 'smiss': int(g[11]),
                                     'miss_ms': float(g[12])})
                continue
            m = PIPES.search(line)
            if m:
                cur['sum'] = tuple(int(x) for x in m.groups())
                continue
            m = PIPE.search(line)
            if m:
                g = m.groups()
                cur['pipes'].append({'kind': g[0], 'v': g[1], 'f': g[2], 'area': g[3], 'misses': int(g[4]),
                                     'smiss': int(g[5]), 'ms': float(g[6]), 'ready': int(g[7]), 'state': g[9]})
                continue
        m = HITCH.search(line)
        if m:
            g = m.groups()
            hitches.append({'frame': int(g[0]), 't': float(g[1]), 'ms': float(g[2]), 'area': g[4], 'load': bool(g[5]),
                            'cause': g[6]})
    return block, hitches


def report(logs, out):
    lines = ['# Stutter map', '']
    total = {}
    for p in logs:
        block, hitches = read_log(p)
        lines += [f'## {p}', '']
        if not block:
            lines += ['No stutter map in this log (stutter_map = 1 in nx2navi.ini turns it on).', '']
            continue
        tag, mins, nareas, hitch, load, paced, causes = block['head']
        lines += [f'{float(mins):.1f} min of play in {nareas} areas ({tag}): **{hitch} hitches in play**, {load} right '
                  f'after area loads' + (f', {paced} long frames left out as a change of pace' if paced else '')
                  + f'. In play by cause:{causes}.', '']
        lines += ['| Area | Minutes | fps | Hitches | a minute | Causes | Worst ms | Pipelines met missing | Shaders met '
                  'missing |', '|---|---|---|---|---|---|---|---|---|']
        for a in block['areas']:
            causes_txt = ', '.join(f'{c} {n}' for c, n in sorted(a['causes'].items(), key=lambda kv: -kv[1]))
            lines.append(f"| {a['name']} | {a['min']:.1f} | {a['fps']} | {a['hitch']} | "
                         f"{a['hitch'] / a['min'] if a['min'] else 0:.2f} | {causes_txt} | {a['worst']:.1f} | "
                         f"{a['pmiss']} | {a['smiss']} |")
            t = total.setdefault(a['name'], {'min': 0.0, 'hitch': 0, 'causes': {}, 'pmiss': 0, 'smiss': 0})
            for k in ('min', 'hitch', 'pmiss', 'smiss'):
                t[k] += a[k]
            for c, n in a['causes'].items():
                t['causes'][c] = t['causes'].get(c, 0) + n
        lines.append('')
        if block['sum']:
            n, rec, new, fresh, unk = block['sum']
            lines += [f'{n} pipelines draws met missing: {new} new to this install (no list or earlier run named them), '
                      f'{rec} recorded but not built yet when met, {fresh} with a shader translated in play, {unk} '
                      'met while their shaders were still translating.', '']
        if block['pipes']:
            lines += ['| Pipeline | First met in | Draws met it missing | Its shader | ms on the replay thread | Usable '
                      'after ms | State |', '|---|---|---|---|---|---|---|']
            for q in block['pipes']:
                lines.append(f"| {q['kind']} {q['v']}/{q['f']} | {q['area']} | {q['misses']} | {q['smiss']} | "
                             f"{q['ms']:.1f} | {q['ready']} | {q['state']} |")
            lines.append('')
        if hitches:
            play = [h for h in hitches if not h['load']]
            by_min = {}
            for h in play:
                by_min[int(h['t'] // 60)] = by_min.get(int(h['t'] // 60), 0) + 1
            busiest = sorted(by_min.items(), key=lambda kv: -kv[1])[:5]
            lines += [f'{len(hitches)} hitch lines ({len(play)} in play). Busiest minutes of the session: '
                      + ', '.join(f'minute {m} ({n})' for m, n in busiest) + '.', '']
    if len(logs) > 1 and total:
        lines += ['## All logs, by area', '', '| Area | Minutes | Hitches | a minute | Causes | Pipelines met missing | '
                  'Shaders met missing |', '|---|---|---|---|---|---|---|']
        for name, t in sorted(total.items(), key=lambda kv: -kv[1]['hitch']):
            causes_txt = ', '.join(f'{c} {n}' for c, n in sorted(t['causes'].items(), key=lambda kv: -kv[1]))
            lines.append(f"| {name} | {t['min']:.1f} | {t['hitch']} | {t['hitch'] / t['min'] if t['min'] else 0:.2f} "
                         f"| {causes_txt} | {t['pmiss']} | {t['smiss']} |")
        lines.append('')
    text = '\n'.join(lines)
    if out:
        Path(out).write_text(text, encoding='utf-8')
        print(f'wrote {out}')
    else:
        print(text)
    return 0


def missing(game_dir, list_file):
    (magic, version, rec, recs), ans = pipeline_list.read_dir(game_dir)
    got = pipeline_list.unpack(Path(list_file).read_bytes())
    if got is None:
        print(f'{list_file}: not a pipeline list')
        return 1
    l_ans, l_keys = got
    lmagic, lversion, lrec, lrecs = pipeline_list.read_keys(l_keys)
    if recs and (magic, version, rec) != (lmagic, lversion, lrec):
        print(f'{game_dir}: its pipeline-keys.bin has another layout than the list ({magic!r} v{version} {rec} bytes; '
              f'the list {lmagic!r} v{lversion} {lrec} bytes): a merge leaves it out')
        return 1
    have = set(lrecs)
    new = [r for r in recs if r not in have]
    stage = {a[36:68]: a[0:4] for a in ans}
    mesh = sum(1 for r in new if stage.get(r[:pipeline_list.ID]) == (pipeline_list.STAGE_MESH).to_bytes(4, 'little'))
    l_ids = {a[36:68] for a in l_ans}
    new_ans = [a for a in ans if a[36:68] not in l_ids]
    by = {}
    for a in new_ans:
        s = pipeline_list.STAGE_NAMES.get(int.from_bytes(a[0:4], 'little'), 'other')
        by[s] = by.get(s, 0) + 1
    print(f'{game_dir}: {len(recs)} pipelines recorded, {len(new)} of them not in {Path(list_file).name} ({mesh} mesh); '
          f'{len(ans)} shader variants recorded, {len(new_ans)} not in the list ('
          + ', '.join(f'{n} {s}' for s, n in sorted(by.items())) + ')')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('report', help='the stutter map of one or more logs, as Markdown')
    r.add_argument('logs', nargs='+', type=Path)
    r.add_argument('--out', type=Path)
    m = sub.add_parser('missing', help='what a game folder recorded that a pipeline list lacks')
    m.add_argument('game_dir', type=Path)
    m.add_argument('list_file', type=Path)
    a = ap.parse_args()
    if a.cmd == 'report':
        return report(a.logs, a.out)
    return missing(a.game_dir, a.list_file)


if __name__ == '__main__':
    sys.exit(main())
