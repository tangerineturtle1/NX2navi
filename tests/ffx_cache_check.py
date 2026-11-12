"""Run the game executable's --probe-ffx-cache twice on pipeline cache files of the test's own
(runtime/vk_ffx.cpp vk_ffx_probe_cache: one FSR 3.1 upscaling context, without a window or the game).

FSR 3.1's library creates its pipelines itself and names no pipeline cache; the runtime hands it pipeline creates that
go through the pipeline caches kept across runs (runtime/vk_pipeline.c vk_pipeline_create_foreign). Checked here:

- the library's pipelines come through those creates (the first run counts them);
- a second run, which reads what the first one saved, compiles none of them: each comes from the caches;
- with ffx_pipe_cache = 0 none comes through the runtime.

The executable needs amd_fidelityfx_vk.dll beside it and a Vulkan device; without either the probe says SKIP and so
does this test. The pipeline cache files go to build/test/ffx_cache, never beside the executable.

Usage: ffx_cache_check.py PATH_TO_GAME_EXE
"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LINE = re.compile(r'probe-ffx-cache: context (created|not created) in \d+ ms; pipelines made through the runtime\'s '
                  r'creates: (\d+) \(from the pipeline caches (\d+), compiled into the play cache (\d+), through the '
                  r'one cache (\d+), with no cache (\d+)\)')


def run(game, cache, setting):
    """One probe run -> (exit code, last probe line, the counts or None)."""
    env = dict(os.environ)
    env['NX2NAVI_PIPELINE_CACHE'] = str(cache)
    env.pop('NX2NAVI_PIPE_BINARIES', None)
    env['NX2NAVI_FFX_PIPE_CACHE'] = setting
    r = subprocess.run([game, '--probe-ffx-cache'], env=env, capture_output=True, text=True, errors='replace')
    lines = [ln for ln in r.stdout.splitlines() if ln.startswith('probe-ffx-cache:')]
    counts = None
    for ln in lines:
        m = LINE.match(ln)
        if m:
            counts = dict(zip(('made', 'cached', 'compiled', 'one', 'none'), map(int, m.groups()[1:])))
    print('\n'.join(lines))
    return r.returncode, lines[-1] if lines else '', counts


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    game = sys.argv[1]
    work = ROOT / 'build' / 'test' / 'ffx_cache'
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    cache = work / 'pipeline-cache.bin'
    rc, last, first = run(game, cache, '1')
    if 'SKIP' in last:
        print('ffx_cache_check: SKIP')
        return 0
    fails = []
    if rc or not last.endswith('passed') or not first:
        fails.append('the first run did not pass')
    elif not first['made']:
        fails.append('the first run made no pipeline through the runtime')
    rc, last, second = run(game, cache, '1')
    if rc or not last.endswith('passed') or not second:
        fails.append('the second run did not pass')
    elif first and (second['made'] != first['made'] or second['compiled']):
        fails.append('the second run made %d pipelines and compiled %d of them: with the first run\'s files it must '
                     'make the same %d and compile none' % (second['made'], second['compiled'], first['made']))
    elif first and not first['one'] and not first['none'] and second['cached'] != second['made']:
        fails.append('with a play cache every pipeline of the second run comes from the caches: %d of %d did'
                     % (second['cached'], second['made']))
    rc, last, off = run(game, cache, '0')
    if rc or not last.endswith('passed') or not off or off['made']:
        fails.append('with ffx_pipe_cache = 0 the run must pass with no pipeline made through the runtime')
    for f in fails:
        print('ffx_cache_check: FAIL: ' + f)
    print('ffx_cache_check: %s' % ('FAILED' if fails else 'passed'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
