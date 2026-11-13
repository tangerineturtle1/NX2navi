"""HD rumble for controllers with linear actuators (runtime/rumble_hd.c): builds it with tests/rumble_hd_check.c and
checks

  - known values: the neutral value, full amplitude, single bands, an odd amplitude step;
  - the clamps: frequencies to each band's ends, amplitudes to 0..1 with the last step at 100, no amplitude as the
    neutral value;
  - a report's data from the two sides, for two actuators and for one, and the output report's layout;
  - from a batch to the reports written: the newest value at once, replaced values never, a held value again every
    30 ms, the neutral value three times once the game stops, then nothing.

Then it decodes a sweep of encoded values with formulae of its own (this script's, not the C file's): each frequency
comes back within half a step of the clamped input, each amplitude as the step nearest to it, and no amplitude step is
above 100.

No game files, no SDL, no controller. Usage: python tests/rumble_hd_check.py [--cc <clang>]
"""
import argparse
import math
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')

LOW = (40.875885, 626.286133)     # Hz, the low band's ends
HIGH = (81.75177, 1252.572266)    # ... and the high band's


def step_amp(k):
    """The amplitude of step k, from the published table's rule: step 1 alone, then quarter, sixteenth and
    thirty-second octaves."""
    if k == 0:
        return 0.0
    if k == 1:
        return 0.007843
    if k < 16:
        return 0.011823 * 2 ** ((k - 2) / 4)
    if k < 32:
        return 2 ** (k / 16) / 17
    return 2 ** (k / 32) / 8.7


def decode(b):
    """Four report bytes to (amp_low step, freq_low Hz, amp_high step, freq_high Hz)."""
    hf = b[0] | (b[1] & 1) << 8
    ha = b[1] >> 1
    lf = b[2] & 0x7f
    la = (b[3] - 0x40) * 2 + (b[2] >> 7)
    return la, 10 * 2 ** ((lf + 0x40) / 32), ha, 10 * 2 ** ((hf / 4 + 0x60) / 32)


def nearest_step(a):
    if not a >= 0.007843 / 2:
        return 0
    a = min(a, 1.0)
    return min(range(1, 101), key=lambda k: abs(math.log(a / step_amp(k))))


def check_sweep(lines):
    fails = 0
    half = 2 ** (1 / 64) * 1.0005   # half a frequency step, and the rounding of a float
    for line in lines:
        f = line.split()
        al, fl, ah, fh = (float(x) for x in f[:4])
        b = [int(x) for x in f[4:]]
        la, dfl, ha, dfh = decode(b)
        what = f'{al}@{fl} Hz {ah}@{fh} Hz -> {" ".join("%02x" % x for x in b)}'
        if not (0 <= la <= 100 and 0 <= ha <= 100 and 0x40 <= b[3] <= 0x72):
            print(f'FAIL {what}: amplitude steps {la}, {ha} outside 0..100')
            fails += 1
            continue
        for name, a, got in (('low', al, la), ('high', ah, ha)):
            want = nearest_step(a)
            # at a boundary between two steps either neighbour is the nearest
            if got != want and (got == 0 or want == 0 or
                                abs(abs(math.log(min(a, 1.0) / step_amp(got))) -
                                    abs(math.log(min(a, 1.0) / step_amp(want)))) > 2e-3):
                print(f'FAIL {what}: {name} amplitude is step {got}, the nearest is {want}')
                fails += 1
        if la == 0 and ha == 0:
            if b != [0x00, 0x01, 0x40, 0x40]:
                print(f'FAIL {what}: no amplitude is not the neutral value')
                fails += 1
            continue
        for name, hz, got, (lo, hi) in (('low', fl, dfl, LOW), ('high', fh, dfh, HIGH)):
            want = min(max(hz, lo), hi)
            if not want / half <= got <= want * half:
                print(f'FAIL {what}: {name} frequency decodes to {got:.2f} Hz, expected {want:.2f}')
                fails += 1
    return fails


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cc', help='the C compiler (default: MSYS2 UCRT64 clang on Windows, clang elsewhere)')
    args = ap.parse_args()
    win = os.name == 'nt'
    cc = args.cc or os.environ.get('CC') or (str(MSYS_BIN / 'clang.exe') if win and MSYS_BIN.exists() else 'clang')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    work = ROOT / 'build' / 'test' / 'rumble_hd'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('rumble_hd_check.exe' if win else 'rumble_hd_check')
    subprocess.check_call([cc, '-O1', '-Wall', '-Wextra', '-fno-strict-aliasing', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'rumble_hd_check.c'), str(ROOT / 'runtime' / 'rumble_hd.c'),
                           '-o', str(exe)] + ([] if win else ['-lm']), env=env)
    rc = subprocess.run([str(exe)], env=env).returncode
    out = subprocess.run([str(exe), '--sweep'], env=env, capture_output=True, text=True, check=True).stdout.splitlines()
    fails = check_sweep(out)
    print(f'rumble hd sweep: {len(out)} values decoded, ' + (f'{fails} failures' if fails else 'ok'))
    return 1 if rc or fails or not out else 0


if __name__ == '__main__':
    sys.exit(main())
