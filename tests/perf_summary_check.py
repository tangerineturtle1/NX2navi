"""tools/perf_summary.py reads the present side of a log: lines made with the runtime's format strings (copied below;
keep them in step with runtime/perf.c perf_present and runtime/vk_backend.c present_main and present_window) give the
present percentiles, the present thread's window averages and the render thread's hand-off share, and the lines of
older logs (no hand-off share, no present call or hand-off on the `present:` lines) still parse.

    python tests/perf_summary_check.py
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / 'tools' / 'perf_summary.py'


def c_fmt(fmt, *args):
    """A C printf format with %llu, %lld and %u as Python's %d and %% kept."""
    return fmt.replace('%llu', '%d').replace('%lld', '%d').replace('%u', '%d') % args


# runtime/vk_backend.c present_main's TRACE=present line and present_window's line, as the C source has them
PRESENT_OLD = 'present: frame %llu interval %.3f ms, game %.3f ms, waited %.3f ms, %.3f ms past target'
PRESENT_TAIL = '; present call %.3f ms, hand-off %.3f ms'
WINDOW = ('perf: present thread frame %llu: %llu presents; blit and submit avg %.2f ms, paced wait avg %.2f ms, present '
          'call avg %.2f ms max %.2f ms, past target avg %.2f ms; the presenting thread waited to hand over avg %.2f ms '
          'max %.2f ms')
# runtime/perf.c perf_present's window line, its head to the shares (the rest is filled in below)
PERF_HEAD = ('perf: frame %llu: interval avg %.2f ms max %.2f ms (%.1f fps); render thread blocked: pacing %.0f%% fence '
             '%.0f%% drain %.0f%% handoff %.0f%% pipeline build %.0f%% shader xlate %.0f%% texture %.0f%% (astc %.0f%%)')
PERF_TAIL = ('; pipelines built in background 0, draws skipped pending 0, waited 0, waiting for a shader 0, one-shot waited '
             '0; textures uploaded 3 re-uploaded 0; slowest frame: xlate 0.0 ms texture 0.0 ms (astc 0.0) pipeline 0.0 ms '
             'fence 0.0 ms; shaders: 10 keys, 10 translated; render busy avg 5.00 ms; replay avg 3.00 ms; ab 0/60; world '
             '12.0 s; shader disk reads: 0 on the render thread (0.0 ms), 0 by the loaders')


def source_has():
    """The format strings above are still the runtime's (the C sources split them over string literals)."""
    def joined(path):
        text = path.read_text(encoding='utf-8')
        return re.sub(r'"\s*\n\s*"', '', text)   # adjacent literals on following lines become one
    be, perf = joined(ROOT / 'runtime' / 'vk_backend.c'), joined(ROOT / 'runtime' / 'perf.c')
    for piece in (PRESENT_OLD + PRESENT_TAIL, WINDOW):
        assert piece in be, f'runtime/vk_backend.c no longer has: {piece}'
    assert PERF_HEAD in perf, f'runtime/perf.c no longer has: {PERF_HEAD}'


def run(lines, lo=0):
    with tempfile.TemporaryDirectory() as d:
        log = Path(d) / 'nx2navi.log'
        log.write_text('\n'.join(lines) + '\n', encoding='utf-8')
        out = subprocess.run([sys.executable, str(TOOL), str(log), str(lo)], capture_output=True, text=True,
                             encoding='utf-8', check=True).stdout
    return out


def summary(out, head):
    found = [ln for ln in out.splitlines() if ln.startswith(head)]
    assert len(found) == 1, (head, out)
    return found[0]


def main():
    source_has()
    lines = []
    # 100 presents of the new form: intervals 16.0..16.99 ms, then one 40 ms; the present call 0.1..1.09 ms; the
    # hand-off 2.0 ms each but one of 30 ms
    for i in range(100):
        lines.append('[host] ' + c_fmt(PRESENT_OLD + PRESENT_TAIL, 1000 + i, 16.0 + i / 100, 16.5, 1.0 + i / 100, 0.05,
                                       0.1 + i / 100, 30.0 if i == 50 else 2.0))
    lines.append('[host] ' + c_fmt(PRESENT_OLD + PRESENT_TAIL, 1100, 40.0, 40.0, 0.0, 0.5, 20.0, 2.0))
    lines.append('[host] ' + c_fmt(WINDOW, 1080, 60, 0.8, 1.5, 0.6, 1.09, 0.05, 2.5, 30.0))
    lines.append('[host] ' + c_fmt(WINDOW, 1140, 60, 1.0, 1.0, 1.0, 20.0, 0.10, 1.5, 3.0))
    lines.append('[host] ' + c_fmt(PERF_HEAD, 1080, 16.7, 40.0, 59.9, 0.0, 2.0, 0.0, 12.0, 0.0, 0.0, 1.0, 0.0) + PERF_TAIL)
    lines.append('[host] ' + c_fmt(PERF_HEAD, 1140, 16.7, 20.0, 59.9, 0.0, 2.0, 0.0, 18.0, 0.0, 0.0, 1.0, 0.0) + PERF_TAIL)
    out = run(lines)
    pres = summary(out, 'SUMMARY present:')
    # 101 intervals: p50 is the 51st smallest (16.50), p99 the 100th (16.99), max 40
    assert '101 presents' in pres and 'interval p50 16.50 p95 16.95 p99 16.99 max 40.00' in pres, pres
    assert 'present call p50 0.60 p95 1.05 p99 1.09 max 20.00' in pres, pres
    assert 'hand-off p50 2.00 p95 2.00 p99 2.00 max 30.00' in pres, pres
    win = summary(out, 'SUMMARY present thread:')
    assert '120 presents in 2 windows' in win and 'present call avg 0.80 ms max 20.00 ms' in win, win
    assert 'hand-off avg 2.00 ms max 30.00 ms' in win, win
    frames = summary(out, 'SUMMARY frames')
    assert 'blocked at the hand-off 15.0% of the time (the highest window 18%)' in frames, frames
    assert 'perf 1080: 59.9 fps max 40.00 pacing 0% handoff 12% pipe 0% tex 1%' in out, out

    # an older log: no hand-off share on the perf line, no present call or hand-off on the present lines
    old_perf = PERF_HEAD.replace(' handoff %.0f%%', '')
    old = ['[host] ' + c_fmt(PRESENT_OLD, 2000 + i, 16.0 + i / 10, 16.5, 1.0, 0.0) for i in range(10)]
    old.append('[host] ' + c_fmt(old_perf, 2040, 16.7, 20.0, 59.9, 0.0, 2.0, 0.0, 0.0, 0.0, 1.0, 0.0) + PERF_TAIL)
    out = run(old)
    pres = summary(out, 'SUMMARY present:')
    assert '10 presents' in pres and 'interval p50 16.50' in pres and 'present call' not in pres, pres
    assert 'hand-off' not in pres, pres
    assert 'SUMMARY present thread' not in out, out
    frames = summary(out, 'SUMMARY frames')
    assert 'hand-off' not in frames and 'perf 2040: 59.9 fps max 20.00 pacing 0% pipe 0% tex 1%' in out, out
    print('perf_summary_check: ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
