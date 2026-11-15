"""tools/bench.py reads the lines runtime/perf.c writes: lines made with the runtime's format strings (checked below to
still be in perf.c) parse into frames, segments and windows, and the summary's numbers (1% low, p99, CPU- or GPU-bound
shares, the run spread, MangoHud's matching stretch) come out as worked by hand. No game files.

    python tests/bench_check.py
"""
import math
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools'))
import bench  # noqa: E402

PERF_C = Path(__file__).resolve().parent.parent / 'runtime' / 'perf.c'

SLOW = ('perf: slow frame %d: %.1f ms; blocked pipeline %.1f xlate %.1f texture %.1f astc %.1f fence %.1f pacing %.1f '
        'drain %.1f handoff 0.0; replay %.1f')
SEG_START = "segment: '%s' starts at frame %d, world %s s"
SEG_END = ("segment: '%s' frames %d-%d, world %s to %s s: %d frames in %.1f s, mean %.2f ms (%.1f fps), 1%% low %.1f "
           "fps (%.2f ms), worst %.1f ms at frame %d; over 25 ms %d, 50 ms %d, 100 ms %d; %s")
WIN = ('perf: frame %d: gpu avg %.2f ms (%d parts timed, %d not; copies into VRAM mirrors %.2f ms); fence wait per '
       'frame: render thread %.2f ms, other threads %.2f ms; %s; ram working set %.0f MiB, private %.0f MiB; bound: %s')

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print('FAIL', what)


def close(a, b, eps=1e-6):
    return abs(a - b) <= eps


def main():
    src = PERF_C.read_text(encoding='utf-8')
    # the format strings the parser depends on must still be in the runtime, as written
    for piece in ('"perf: slow frame %llu: %.1f ms; blocked pipeline %.1f xlate %.1f texture %.1f astc %.1f fence %.1f '
                  'pacing %.1f "', '"drain %.1f handoff %.1f; replay %.1f%s"', "\"segment: '%s' starts at frame %llu, world %s s\"",
                  "\"segment: '%s' frames %llu-%llu, world %s to %s s: %llu frames in %.1f s, mean %.2f ms (%.1f fps), "
                  "1%% low %.1f \"", '"gpu avg %.2f ms (%llu parts timed, %llu not; copies into VRAM mirrors %.2f ms)"',
                  '"%.0f MiB, private %.0f MiB; bound: %s"'):
        check(piece in src, f'runtime/perf.c still holds {piece}')

    # 200 frames in the measured segment (frames 101-300): 196 cpu frames of 20 ms, 2 gpu frames of 40 ms (fence 30),
    # 2 stall frames of 60 ms (pipeline 30); frames before and after the segment do not count
    lines = [SEG_START % ('measure', 100, '20.0')]
    for k in range(90, 311):
        if k in (150, 250):
            lines.append(SLOW % (k, 40.0, 0, 0, 0, 0, 30.0, 0, 0, 5.0))
        elif k in (200, 210):
            lines.append(SLOW % (k, 60.0, 30.0, 0, 0, 0, 0, 0, 0, 40.0))
        else:
            lines.append(SLOW % (k, 20.0, 0, 0, 0, 0, 1.0, 0, 0, 12.0))
        if k == 300:
            lines.append(SEG_END % ('measure', 100, 300, '20.0', '24.4', 200, 4.4, 20.4, 49.0, 16.7, 60.0, 60.0, 200, 4, 2, 0,
                                    'ram working set 1 MiB'))
            lines.append(SEG_START % ('end', 300, '24.4'))
        if k == 240:
            lines.append(WIN % (240, 15.5, 10, 0, 0.1, 1.0, 0.0, 'vram 1 of 8 MiB', 100, 100, 'cpu'))
    frames, starts, ends, windows = bench.parse_log('\n'.join('[host] ' + l for l in lines))
    sel, wins = bench.measured(frames, starts, windows)
    check(len(sel) == 200 and sel[0]['frame'] == 101 and sel[-1]['frame'] == 300, f'segment frames {len(sel)}')
    check(ends['measure']['frames'] == 200 and close(ends['measure']['low1_fps'], 16.7), 'segment end line')
    check(len(wins) == 1 and close(wins[0]['gpu_ms'], 15.5) and wins[0]['bound'] == 'cpu', 'window line')
    r = bench.run_summary(sel, wins)
    total = 196 * 20 + 2 * 40 + 2 * 60
    check(close(r['avg_fps'], 1000 * 200 / total), f'avg fps {r["avg_fps"]}')
    # the slowest 1% of 200 frames is 2 frames: the two 60 ms frames
    check(close(r['low1_fps'], 1000 / 60), f'1% low {r["low1_fps"]}')
    # p99 of 200 sorted values: position 197.01 between 40 (index 197) and 60 (index 198)
    check(close(r['p99_ms'], 40 + 20 * 0.01, 1e-9), f'p99 {r["p99_ms"]}')
    check(close(r['cpu_pct'], 98) and close(r['gpu_pct'], 1) and close(r['stall_pct'], 1), 'bound shares')
    check(r['over33'] == 4, 'frames over 33.3 ms')

    # the bound rules at their edges
    f = lambda **k: {**dict.fromkeys(bench.FIELDS, 0.0), **k}
    check(bench.bound_of(f(ms=10, fence=5)) == 'gpu', 'fence half the frame is gpu')
    check(bench.bound_of(f(ms=10, fence=4.9)) == 'cpu', 'fence under half is cpu')
    check(bench.bound_of(f(ms=10, texture=2.5)) == 'stall', 'a quarter in texture uploads is a stall')
    check(bench.bound_of(f(ms=10, pacing=1.5)) == 'pace', 'pacing')
    check(bench.bound_of(f(ms=10), gpu_ms=8) == 'gpu', 'the GPU busy for 80% of the frame is gpu')
    check(bench.bound_of(f(ms=10), gpu_ms=7.9) == 'cpu', 'the GPU busy for under 80% is cpu')
    # a window line at frame N gives its GPU time to frames N-59 to N; later frames have none
    fr = {k: f(frame=k, ms=10) for k in (60, 61, 120, 121)}
    bench.mark_bounds(fr, [{'frame': 60, 'gpu_ms': 9.0}, {'frame': 120, 'gpu_ms': 2.0}])
    check([fr[k]['bound'] for k in (60, 61, 120, 121)] == ['gpu', 'cpu', 'cpu', 'cpu'], 'frames take their window')
    fr = {1: f(frame=1, ms=10)}
    bench.mark_bounds(fr, [{'frame': 60, 'gpu_ms': None}])
    check(fr[1]['bound'] == 'cpu', 'a window without timestamps')

    # logs of 0.11.0 and older have no handoff field
    old = bench.SLOW_RE.search('perf: slow frame 7: 20.0 ms; blocked pipeline 0.0 xlate 0.0 texture 0.0 astc 0.0 fence 1.0 '
                               'pacing 0.0 drain 0.0; replay 12.0')
    check(old is not None and old[10] == '12.0', 'slow frame line without handoff')

    # spread over runs
    m, lo, hi, pct = bench.spread([50.0, 55.0, 45.0])
    check(close(m, 50) and lo == 45 and hi == 55 and close(pct, 20), 'spread')
    check(math.isnan(bench.spread([])[0]), 'spread of nothing')

    # MangoHud: a log in ms, its stretch matched to the game's from the end of the run
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / 'ocarina_2026.csv'
        p.write_text('os,cpu,gpu\nLinux,x,y\nfps,frametime,cpu_load\n' + '50,20.0,10\n' * 300 + '25,40.0,10\n' * 10)
        mh = bench.mangohud_frames(p)
        check(len(mh) == 310 and mh[0] == 20.0, 'mangohud rows')
        game = {k: {'ms': 20.0} for k in range(1, 301)}
        game.update({k: {'ms': 40.0} for k in range(301, 311)})
        win = bench.mangohud_window(mh, game, 100, 300)
        check(len(win) == 200 and all(v == 20.0 for v in win), f'mangohud stretch {len(win)}')
        p.write_text('fps,frametime\n' + '50,20000\n' * 10)
        check(bench.mangohud_frames(p)[0] == 20.0, 'mangohud microseconds')

    # the input script: untimed A presses, then the world-clock lines
    s = bench.make_script({'name': 'x', 'warmup_s': 10, 'measure_s': 30,
                           'moves': [[5, 'RSTICK', 20000, 0, 1.5], [6, 'LSTICK', 0, 32767, 2]]})
    check('240 A 4' in s and '@15000 RSTICK 20000 0 1500' in s and '@16000 LSTICK 0 32767 2000' in s and '@10000 MARK measure' in s
          and '@40000 MARK end' in s, 'script')
    check(bench.dbg_test({'dbg': [['@1000', 'warp A B'], [900, 'route 1 2']]}) == '@1000:warp A B;900:route 1 2', 'dbg test')

    print('bench_check:', 'ok' if not failures else f'{failures} failed')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
