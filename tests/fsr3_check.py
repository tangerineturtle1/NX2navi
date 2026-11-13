"""Run the game executable's --probe-fsr3 with Vulkan's validation on (runtime/fsr3_probe.c: FSR 3.1 upscaling with
the runtime's own passes, runtime/vk_fsr3.c, on a synthetic panning scene, without a window or the game).

The probe checks the passes' output against a supersampled reference and against bilinear, the jitter and motion
vector conventions runtime/vk_dlaa.c hands over (reversing either does worse), two render sizes on one output size
served by one context, and that the passes raise no validation error; then it times one upscale at the docked sizes.
This test passes when the probe does, and prints its lines.

The executable needs a Vulkan device that runs the passes; without one the probe says SKIP and so does this test.

Usage: fsr3_check.py PATH_TO_GAME_EXE
"""
import os
import subprocess
import sys


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    env = dict(os.environ)
    env['NX2NAVI_VK_VALIDATION'] = '1'
    r = subprocess.run([sys.argv[1], '--probe-fsr3'], env=env, capture_output=True, text=True, errors='replace')
    lines = [ln for ln in r.stdout.splitlines() if ln.startswith('probe-fsr3:')]
    print('\n'.join(lines))
    last = lines[-1] if lines else ''
    if 'SKIP' in last:
        print('fsr3_check: SKIP')
        return 0
    ok = r.returncode == 0 and last.endswith('passed (0 validation errors)')
    print('fsr3_check: %s' % ('passed' if ok else 'FAILED'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
