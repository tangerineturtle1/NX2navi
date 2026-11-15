"""The shake key swings the right stick by the game's updates, not by the clock (runtime/input.c shake_stick): builds
shake_stick, cut from input.c by its text, with stand-ins for the clock and the present count
(frame_rate.c frame_step_presents), and checks that

  - each side lasts two updates at 20, 30, 60, 120, 144 and 240 fps, starting left, also when the stick is read
    twice an update;
  - a press swings it for 800 ms, a hold keeps it swinging and the release ends it at once;
  - a press inside the swing keeps its side, and a swing without presents (a load) keeps one side.

No game files, no SDL. Usage: python tests/shake_key_check.py [--cc <clang>]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
enum { STICK_MAX = 32767 };
static uint64_t now_ns = 5000000000ull, presents = 1000;
static uint64_t plat_now_ns(void) { return now_ns; }
static uint64_t frame_step_presents(void) { return presents; }
%s
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); putchar('\n'); fails++; } } while (0)

// One update at hz: the clock moves a frame, the game presents once, and the stick is read reads times.
static int32_t update(int hz, int held, int reads) {
    now_ns += 1000000000ull / (uint64_t)hz;
    presents++;
    int32_t x = 0;
    for (int r = 0; r < reads; r++) {
        int32_t y = shake_stick(held);
        CHECK(r == 0 || y == x, "two reads in one update differ: %%d %%d", x, y);
        x = y;
    }
    return x;
}

static void settle(void) {
    for (int i = 0; i < 4; i++) now_ns += 1000000000ull, presents++, shake_stick(0);
}

int main(void) {
    static const int rates[] = {20, 30, 60, 120, 144, 240};
    for (int i = 0; i < 6; i++) {
        for (int reads = 1; reads <= 2; reads++) {
            int hz = rates[i];
            settle();
            int32_t first = shake_stick(1);   // the press, read before the update that shows it
            CHECK(first == -STICK_MAX, "%%d fps: the swing starts left (%%d)", hz, first);
            int n = 0, sides = 0;
            int32_t last = first, run = 1;
            for (int k = 0; k < hz * 2; k++) {
                int32_t x = update(hz, 0, reads);
                if (!x) break;
                n++;
                if (x != last) {
                    CHECK(run == 2, "%%d fps, %%d reads: a side lasted %%d updates", hz, reads, run);
                    sides++, run = 0, last = x;
                }
                run++;
            }
            // the press swings for 800 ms: 0.8 * hz updates after the press, the first counted at the press
            int want = (int)(0.8 * hz + 0.5) - 1;
            CHECK(n >= want - 1 && n <= want + 1, "%%d fps: %%d updates swung, want about %%d", hz, n, want);
            CHECK(sides >= n / 2 - 1, "%%d fps: %%d sides in %%d updates", hz, sides, n);
        }
    }
    // A hold keeps swinging past 800 ms; the release ends it at once.
    settle();
    shake_stick(1);
    int swung = 0;
    for (int k = 0; k < 240 * 2; k++) swung += update(240, 1, 1) != 0;
    CHECK(swung == 480, "a 2 s hold swung %%d of 480 updates", swung);
    CHECK(update(240, 0, 1) == 0, "the release after a long hold ends the swing");
    // A second press inside the swing keeps the side it is on.
    settle();
    shake_stick(1);
    update(60, 0, 1), update(60, 0, 1);
    int32_t on = update(60, 0, 1);
    int32_t again = shake_stick(1);
    CHECK(on == STICK_MAX && again == on, "a press inside the swing moved the side: %%d to %%d", on, again);
    // No presents (a load screen): the stick keeps its side while the swing lasts.
    settle();
    shake_stick(1);
    for (int k = 0; k < 10; k++) {
        now_ns += 50000000ull;
        CHECK(shake_stick(0) == -STICK_MAX, "without presents the side changed at %%d", k);
    }
    printf(fails ? "shake key: %%d failures\n" : "shake key: ok\n", fails);
    return fails != 0;
}
'''


def cut(text, start, end_marker):
    a = text.index(start)
    b = text.index(end_marker, a)
    return text[a:b]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cc', help='the C compiler (default: MSYS2 UCRT64 clang on Windows, clang elsewhere)')
    args = ap.parse_args()
    win = os.name == 'nt'
    cc = args.cc or (str(MSYS_BIN / 'clang.exe') if win and MSYS_BIN.exists() else 'clang')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    src = (ROOT / 'runtime' / 'input.c').read_text(encoding='utf-8')
    body = cut(src, '#define SHAKE_NS', '\n// Under lock and inside sdl_enter')
    with tempfile.TemporaryDirectory(prefix='nx2navi-shake-') as tmp:
        c = Path(tmp) / 'check.c'
        c.write_text(HARNESS % body, encoding='utf-8')
        exe = Path(tmp) / ('check.exe' if win else 'check')
        subprocess.check_call([cc, '-O2', '-Wall', '-Wextra', '-Wno-unused-function', str(c), '-o', str(exe)], env=env)
        return subprocess.call([str(exe)])


if __name__ == '__main__':
    sys.exit(main())
