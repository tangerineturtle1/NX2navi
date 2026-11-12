"""Run the game executable's --probe-devlost and check how a run ends when the GPU device is lost
(runtime/vk_device.c vk_device_lost_check, runtime/vk_sync.c vk_sync_probe_devlost).

The probe needs a Vulkan device and opens no window. It does not lose the device: it marks it lost, and then has four
threads report a loss at the same moment. Checked here:

  - on the healthy device a completion callback runs (so the next check can fail);
  - with the device marked lost no callback runs, no value counts as done, and the submit, upload and wait paths return
    at once;
  - the process ends with exit code 72 (dispatch.h RT_LOST_EXIT), with one device report and one FATAL line, well
    before the watchdog's 10 seconds.

What a driver answers after a real loss is not covered.

Usage: device_lost_check.py PATH_TO_GAME_EXE
"""
import subprocess
import sys
import time

LOST_EXIT = 72


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    p = subprocess.Popen([sys.argv[1], '--probe-devlost'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, errors='replace')
    lines, reported_at = [], None
    for line in p.stdout:
        lines.append(line.rstrip('\n'))
        if 'four threads report the loss now' in line:
            reported_at = time.monotonic()
    code = p.wait()
    ended = time.monotonic()
    text = '\n'.join(lines)
    if 'probe-devlost: SKIP' in text:
        print('device_lost_check: SKIP (no Vulkan device)')
        return 0
    fails = []

    def check(ok, what):
        if not ok:
            fails.append(what)

    check('probe-devlost: healthy device: the callback of value' in text, 'no callback ran on the healthy device')
    marked = [ln for ln in lines if ln.startswith('probe-devlost: device marked lost:')]
    check(len(marked) == 1 and 'not run, done stayed, submit not recorded' in marked[0],
          'a path went on after the device was marked lost: %s' % (marked or ['no line']))
    check('probe-devlost: FAILED' not in text, 'the probe reports a failure')
    check(code == LOST_EXIT, 'exit code %d, expected %d' % (code, LOST_EXIT))
    fatal = [ln for ln in lines if ln.startswith('FATAL: ')]
    check(len(fatal) == 1 and 'the GPU device was lost' in fatal[0], '%d FATAL lines, expected one: %s' % (len(fatal), fatal))
    reports = [ln for ln in lines if 'video memory when the device was lost' in ln]
    check(len(reports) == 1, '%d device reports, expected one' % len(reports))
    took = ended - reported_at if reported_at else -1
    check(0 <= took < 5, 'the process ended %.1f s after the loss was reported (the watchdog, not the reporting thread?)' % took)
    for f in fails:
        print('device_lost_check: FAIL:', f)
    if fails:
        print(text[-3000:])
        return 1
    print('device_lost_check: passed (exit code %d, one report, one FATAL line, %.2f s from the report to the exit)'
          % (code, took))
    return 0


if __name__ == '__main__':
    sys.exit(main())
