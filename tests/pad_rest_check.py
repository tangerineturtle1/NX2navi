"""A gamepad axis counts only once it has been seen at rest (runtime/input.c pad_axis): builds pad_axis, cut from
input.c by its text, with stand-ins for the SDL calls it makes, and checks that

  - an axis held from the start (a trigger mapped onto a stick, a virtual pad that reports a held axis) reads 0, and
    keeps reading 0 while it stays held;
  - once it has been at rest it reads what the device reports, held or not;
  - each axis and each gamepad has its own state.

No game files, no SDL. Usage: python tests/pad_rest_check.py [--cc <clang>]
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
#include <stdlib.h>
typedef struct SDL_Gamepad { int16_t axis[6]; } SDL_Gamepad;
typedef enum { SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY,
               SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER } SDL_GamepadAxis;
static int16_t SDL_GetGamepadAxis(SDL_Gamepad *p, SDL_GamepadAxis a) { return p->axis[a]; }
enum { MAX_PADS = 8 };
static SDL_Gamepad *pads[MAX_PADS];
static uint8_t pad_rest[MAX_PADS];
static int npads;
%s
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); putchar('\n'); fails++; } } while (0)
int main(void) {
    SDL_Gamepad real = {{0}}, ghost = {{0}};
    pads[0] = &ghost, pads[1] = &real, npads = 2;
    ghost.axis[SDL_GAMEPAD_AXIS_RIGHTX] = 32767;   // held from the start, for good
    real.axis[SDL_GAMEPAD_AXIS_RIGHTX] = 20000;    // held at the start by the player
    for (int i = 0; i < 100; i++) {
        CHECK(pad_axis(&ghost, SDL_GAMEPAD_AXIS_RIGHTX) == 0, "a held axis read %%d at poll %%d", pad_axis(&ghost, SDL_GAMEPAD_AXIS_RIGHTX), i);
        CHECK(pad_axis(&real, SDL_GAMEPAD_AXIS_RIGHTX) == 0, "the player's held stick read before release");
    }
    CHECK(pad_axis(&real, SDL_GAMEPAD_AXIS_LEFTX) == 0 && pad_axis(&real, SDL_GAMEPAD_AXIS_LEFTY) == 0, "resting axes");
    real.axis[SDL_GAMEPAD_AXIS_LEFTX] = -30000;
    CHECK(pad_axis(&real, SDL_GAMEPAD_AXIS_LEFTX) == -30000, "an axis seen at rest reads the device: %%d",
          pad_axis(&real, SDL_GAMEPAD_AXIS_LEFTX));
    real.axis[SDL_GAMEPAD_AXIS_RIGHTX] = 3000;   // let go (drift below the rest band)
    CHECK(pad_axis(&real, SDL_GAMEPAD_AXIS_RIGHTX) == 3000, "a released stick reads the device");
    real.axis[SDL_GAMEPAD_AXIS_RIGHTX] = 32767;
    CHECK(pad_axis(&real, SDL_GAMEPAD_AXIS_RIGHTX) == 32767, "and then reads full tilt");
    CHECK(pad_axis(&ghost, SDL_GAMEPAD_AXIS_RIGHTX) == 0, "another gamepad's axis keeps its own state");
    ghost.axis[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = 32767;
    CHECK(pad_axis(&ghost, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) == 0, "a trigger held from the start");
    SDL_Gamepad stray = {{32767}};
    CHECK(pad_axis(&stray, SDL_GAMEPAD_AXIS_LEFTX) == 32767, "a gamepad not open is read as it is");
    printf(fails ? "pad rest: %%d failures\n" : "pad rest: ok\n", fails);
    return fails != 0;
}
'''


def cut(text, start, end_marker):
    a = text.index(start)
    b = text.index(end_marker, a)
    return text[a:b]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc', help='the C compiler (default: MSYS2 UCRT64 clang on Windows, clang elsewhere)')
    args = ap.parse_args()
    win = os.name == 'nt'
    cc = args.cc or (str(MSYS_BIN / 'clang.exe') if win and MSYS_BIN.exists() else 'clang')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    src = (ROOT / 'runtime' / 'input.c').read_text(encoding='utf-8')
    body = cut(src, 'enum { AXIS_REST', '\nstatic void stick(')
    with tempfile.TemporaryDirectory(prefix='nx2navi-pad-rest-') as tmp:
        c = Path(tmp) / 'check.c'
        c.write_text(HARNESS % body, encoding='utf-8')
        exe = Path(tmp) / ('check.exe' if win else 'check')
        subprocess.check_call([cc, '-O2', '-Wall', '-Wno-unused-function', str(c), '-o', str(exe)], env=env)
        return subprocess.call([str(exe)])


if __name__ == '__main__':
    sys.exit(main())
