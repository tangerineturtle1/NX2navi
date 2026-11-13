"""tools/stutter_map.py reads the lines runtime/stutter_map.c writes: lines made with the runtime's format strings
(copied below; keep them in step with stutter_map_init's neighbours stutter_frame and stutter_report) parse into the
areas, causes and pipelines they hold.

    python tests/stutter_map_check.py
"""
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools'))
import stutter_map  # noqa: E402

C_SOURCE = Path(__file__).resolve().parent.parent / 'runtime' / 'stutter_map.c'


def c_fmt(fmt, *args):
    """A C printf format with %llu, %u and %d as Python's %d."""
    return fmt.replace('%llu', '%d').replace('%u', '%d') % args


def main():
    src = C_SOURCE.read_text(encoding='utf-8')
    # the format strings the parser depends on must still be in the runtime
    for piece in ('stutter map (%s): %.1f min of play in %d areas; %llu hitches in play and %llu ',
                  '"after area loads; %llu long frames were a change of pace (the frames after them as long), not "',
                  '"hitches; in play by cause:"', 'stutter map:   %s: %.1f min at %.0f fps; hitches %llu (%.2f a minute',
                  '"; %s %llu, %.1f ms avg"', '"), worst %.1f ms (frame %llu); over 25 ms %llu; after its "',
                  '"loads %llu; draws that met a pipeline missing %llu, a shader %llu, %.1f ms on the replay thread"',
                  'stutter: frame %llu at %.1f s, %.1f ms (frames about %.1f ms) in %s%s:', '" (charged to %s)"',
                  'stutter map:   %s %s/%s first met in %s: %llu draws met it missing, %llu its shader, %.1f ms on the ',
                  '"replay thread; usable after %.0f ms at most (%llu times); %s%s"'):
        assert piece in src, f'runtime/stutter_map.c no longer has: {piece}'
    lines = [
        '[host] ' + c_fmt('stutter: frame %llu at %.1f s, %.1f ms (frames about %.1f ms) in %s%s:', 1200, 75.2, 31.5, 7.1,
                          'Kakariko Village', '') + ' pipeline 24.0 replay 3.1 (charged to pipeline); draws that met a '
        'pipeline missing: vertex 1a2b3c4d/5e6f7a8b 23.9 ms (skipped)',
        '[host] ' + c_fmt('stutter: frame %llu at %.1f s, %.1f ms (frames about %.1f ms) in %s%s:', 1300, 80.0, 60.0, 7.0,
                          'Kakariko Village', ', after its load') + ' unattributed 55.0 (charged to unattributed)',
        '[host] ' + c_fmt('stutter map (%s): %.1f min of play in %d areas; %llu hitches in play and %llu '
                          'after area loads; %llu long frames were a change of pace (the frames after them as long), not '
                          'hitches; in play by cause:', 'exit', 12.5, 2, 3, 1, 7) + ' pipeline 2 texture 1',
        '[host] ' + c_fmt('stutter map:   %s: %.1f min at %.0f fps; hitches %llu (%.2f a minute', 'Kakariko Village', 10.0,
                          139.6, 3, 0.3)
        + c_fmt('; %s %llu, %.1f ms avg', 'pipeline', 2, 30.2) + c_fmt('; %s %llu, %.1f ms avg', 'texture', 1, 19.0)
        + c_fmt('), worst %.1f ms (frame %llu); over 25 ms %llu; after its '
                'loads %llu; draws that met a pipeline missing %llu, a shader %llu, %.1f ms on the replay thread',
                31.5, 1200, 1, 1, 4, 1, 40.2),
        '[host] ' + c_fmt('stutter map:   %s: %.1f min at %.0f fps; hitches %llu (%.2f a minute', '(before the first area)',
                          2.5, 120.0, 0, 0.0)
        + c_fmt('), worst %.1f ms (frame %llu); over 25 ms %llu; after its '
                'loads %llu; draws that met a pipeline missing %llu, a shader %llu, %.1f ms on the replay thread',
                0.0, 0, 0, 0, 0, 0, 0.0),
        '[host] stutter map: 2 pipelines draws met missing: 1 that this install had recorded before (its prebuild had '
        'not come to them), 1 new to it, 1 with a shader translated in play, 0 still translating when met; 0 not kept '
        '(table full), 0 draws not seen by a present (ring full); the 40 that cost the most:',
        '[host] ' + c_fmt('stutter map:   %s %s/%s first met in %s: %llu draws met it missing, %llu its shader, %.1f ms on '
                          'the replay thread; usable after %.0f ms at most (%llu times); %s%s', 'vertex', '1a2b3c4d',
                          '5e6f7a8b', 'Kakariko Village', 3, 1, 30.0, 160.0, 1, 'new to this install',
                          ', a shader translated in play'),
    ]
    with tempfile.TemporaryDirectory() as d:
        log = Path(d) / 'nx2navi.log'
        log.write_text('\n'.join(lines) + '\n', encoding='utf-8')
        block, hitches = stutter_map.read_log(log)
        assert block and block['head'][0] == 'exit' and block['head'][3] == '3', block and block['head']
        a = block['areas'][0]
        assert a['name'] == 'Kakariko Village' and a['hitch'] == 3 and a['causes'] == {'pipeline': 2, 'texture': 1}, a
        assert a['pmiss'] == 4 and a['smiss'] == 1 and a['load'] == 1 and a['over25'] == 1, a
        assert block['areas'][1]['causes'] == {}, block['areas'][1]
        assert block['sum'] == (2, 1, 1, 1, 0), block['sum']
        p = block['pipes'][0]
        assert p['v'] == '1a2b3c4d' and p['f'] == '5e6f7a8b' and p['ready'] == 160 and p['misses'] == 3, p
        assert p['state'].startswith('new to this install'), p
        assert len(hitches) == 2 and hitches[0]['cause'] == 'pipeline' and hitches[1]['load'], hitches
        out = Path(d) / 'map.md'
        stutter_map.report([log], out)
        text = out.read_text(encoding='utf-8')
        assert block['head'][5] == '7' and block['head'][6] == ' pipeline 2 texture 1', block['head']
        assert '7 long frames left out' in text and '**3 hitches in play**' in text and '| Kakariko Village | 10.0 | 140 |' in text, text
    print('stutter_map_check: ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
