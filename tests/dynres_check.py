"""Dynamic resolution's target and step hold (runtime/dynres.c): builds dynres.c, cut by its text, with stand-ins for
the settings, the GPU's busy time, the clock and the game's rate under the limit (frame_rate.c frame_step_game_hz),
and checks that

  - the default target is the frame time of the game's rate (1000 / the limit: 60, 144 and 240 fps), 16.7 ms while
    that rate is unknown or unlimited, and a target that is set wins at any rate;
  - a limit changed while it runs moves the target on the next frame, with one log line, also when the menu changed
    it (or dyn_res_target) and dynres_read took the new target first; a refresh rate wandering under 1 % after the
    start line logs nothing, and after dyn_res off and on a move back to the earlier run's last target is logged;
  - each step is held for dyn_res_step 60ths of a second of the clock at 60 and 240 fps (45: 0.75 s; 90: 1.5 s), down
    while the GPU is over the target and back up once it is under three quarters of it.

No game files, no GPU. Usage: python tests/dynres_check.py [--cc <clang>]
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
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned ver = 1;
static const char *dlss = "quality", *dyn = "1";
static double set_target_ms;   // NX2NAVI_DYN_RES_TARGET as cfg_double reads it (0: the default)
static int set_step = 45;      // NX2NAVI_DYN_RES_STEP
static double game_hz = 60;
static uint64_t busy = 1, now_ns = 1000000000ull;
static int timing, logs;
static char last_log[256];
static unsigned cfg_version(void) { return ver; }
static const char *cfg_value(const char *k) {
    return !strcmp(k, "NX2NAVI_DLSS") ? dlss : !strcmp(k, "NX2NAVI_DYN_RES") ? dyn : "0";
}
static int cfg_bool(const char *k) { return strcmp(cfg_value(k), "0") != 0; }
static double cfg_double(const char *k) { return !strcmp(k, "NX2NAVI_DYN_RES_TARGET") ? set_target_ms : 0; }
static int cfg_int(const char *k) { return !strcmp(k, "NX2NAVI_DYN_RES_STEP") ? set_step : 0; }
static double frame_step_game_hz(void) { return game_hz; }
static uint64_t vk_gpu_busy_all(void) { return busy; }
static void perf_gpu_timing_want(int on) { timing = on; }
static uint64_t plat_now_ns(void) { return now_ns; }
static void rt_log(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vsnprintf(last_log, sizeof last_log, fmt, a);
    va_end(a);
    logs++;
}
%s
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); putchar('\n'); fails++; } } while (0)
#define NEAR(a, b) (fabs((a) - (b)) <= (b) * 1e-6)

// One presented frame at hz with the GPU busy gpu_ms; returns the step in use (0 quality .. 3 ultra performance).
static int frame(double hz, double gpu_ms) {
    now_ns += (uint64_t)(1e9 / hz);
    busy += (uint64_t)(gpu_ms * 1e6);
    dynres_frame();
    return D.level;
}

static void restart(void) {
    dyn = "0", ver++, frame(60, 1);
    dyn = "1", ver++, frame(60, 1);
}

// Frames at hz with the GPU busy gpu_ms until the step changes (at most limit frames): their count, -1 for none.
static int frames_to_change(double hz, double gpu_ms, int limit) {
    int from = D.level;
    for (int k = 1; k <= limit; k++)
        if (frame(hz, gpu_ms) != from) return k;
    return -1;
}

int main(void) {
    // The default target follows the game's rate.
    static const double rates[] = {60, 144, 240, 0};
    for (int i = 0; i < 4; i++) {
        game_hz = rates[i];
        restart();
        double want = rates[i] > 0 ? 1e9 / rates[i] : 16.7e6;
        CHECK(NEAR(D.target_ns, want), "at %%.0f fps the target is %%.3f ms, want %%.3f", rates[i], D.target_ns / 1e6,
              want / 1e6);
        CHECK(strstr(last_log, "dynamic resolution on") && strstr(last_log, "the frame limit's"), "start line: %%s",
              last_log);
    }
    CHECK(timing, "GPU timestamps asked for while it runs");
    // A target that is set wins.
    set_target_ms = 10, game_hz = 240;
    restart();
    frame(240, 1);
    CHECK(NEAR(D.target_ns, 10e6) && strstr(last_log, "as set"), "a set target: %%.3f ms (%%s)", D.target_ns / 1e6,
          last_log);
    set_target_ms = 0;
    // A limit changed while it runs: the next frame's target, one line.
    game_hz = 60;
    restart();
    int before = logs;
    game_hz = 144;
    frame(144, 1);
    CHECK(NEAR(D.target_ns, 1e9 / 144) && logs == before + 1 && strstr(last_log, "6.94"), "live limit: %%.3f ms, %%d "
          "lines (%%s)", D.target_ns / 1e6, logs - before, last_log);
    before = logs;
    for (int k = 0; k < 100; k++) frame(144, 1);
    CHECK(logs == before, "a steady limit logged %%d lines", logs - before);
    // A limit or a target changed in the menu (a new cfg_version, so dynres_read takes the new target first): one line.
    before = logs;
    game_hz = 120, ver++;
    frame(120, 1);
    CHECK(NEAR(D.target_ns, 1e9 / 120) && logs == before + 1 && strstr(last_log, "8.33 ms (the frame limit's)"),
          "the menu's limit: %%.3f ms, %%d lines (%%s)", D.target_ns / 1e6, logs - before, last_log);
    before = logs;
    set_target_ms = 20, ver++;
    frame(120, 1);
    CHECK(NEAR(D.target_ns, 20e6) && logs == before + 1 && strstr(last_log, "20.00 ms (as set)"),
          "the menu's target: %%.3f ms, %%d lines (%%s)", D.target_ns / 1e6, logs - before, last_log);
    set_target_ms = 0;
    // A refresh rate wandering under 1 %% after the start line: no line. Off and on again, then back to the target the
    // earlier run logged last: one line, measured from the new start line.
    game_hz = 60;
    restart();
    before = logs;
    game_hz = 59.9;
    frame(59.9, 1);
    CHECK(logs == before, "a wander of 0.17 %%%% after the start logged %%d lines (%%s)", logs - before, last_log);
    game_hz = 144;
    frame(144, 1);
    dyn = "0", ver++, frame(144, 1);
    game_hz = 60;
    dyn = "1", ver++, frame(60, 1);
    before = logs;
    game_hz = 144;
    frame(144, 1);
    CHECK(logs == before + 1 && strstr(last_log, "6.94"), "back to the earlier run's last target: %%d lines (%%s)",
          logs - before, last_log);
    // Each step is held for a time, whatever the frame rate.
    static const struct { double hz; int step; double hold_s; } holds[] = {{60, 45, 0.75}, {240, 45, 0.75},
                                                                           {240, 90, 1.5}, {144, 8, 8 / 60.0}};
    for (int i = 0; i < 4; i++) {
        double hz = holds[i].hz;
        game_hz = hz, set_step = holds[i].step;
        restart();
        double target = 1000 / hz;
        int want = (int)ceil(holds[i].hold_s * hz);
        int n = frames_to_change(hz, target * 2, 100000);
        CHECK(D.level == 1 && n >= want - 1 && n <= want + 1, "%%.0f fps, step %%d: down after %%d frames, want %%d",
              hz, holds[i].step, n, want);
        n = frames_to_change(hz, target * 2, 100000);
        CHECK(D.level == 2 && n >= want - 1 && n <= want + 1, "%%.0f fps: the next step after %%d frames, want %%d", hz,
              n, want);
        n = frames_to_change(hz, target * 2, want * 3);
        CHECK(n < 0 && D.level == 2, "%%.0f fps: below performance (%%d)", hz, D.level);
        n = frames_to_change(hz, target * 0.5, 100000);   // the average needs a few frames to fall under 75 %%
        CHECK(D.level == 1 && n >= 1 && n <= want + 40, "%%.0f fps: up after %%d frames", hz, n);
        n = frames_to_change(hz, target * 0.5, 100000);
        CHECK(D.level == 0 && n >= want - 1 && n <= want + 1, "%%.0f fps: back to quality after %%d frames, want %%d",
              hz, n, want);
        n = frames_to_change(hz, target * 0.9, want * 3);
        CHECK(n < 0, "%%.0f fps: a GPU between three quarters of the target and 8 %%%% over it moved the step", hz);
    }
    set_step = 45;
    printf(fails ? "dynres: %%d failures\n" : "dynres: ok\n", fails);
    return fails != 0;
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cc', help='the C compiler (default: MSYS2 UCRT64 clang on Windows, clang elsewhere)')
    args = ap.parse_args()
    win = os.name == 'nt'
    cc = args.cc or (str(MSYS_BIN / 'clang.exe') if win and MSYS_BIN.exists() else 'clang')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    src = (ROOT / 'runtime' / 'dynres.c').read_text(encoding='utf-8')
    body = src[src.index('#define DYN_LEVELS'):]
    with tempfile.TemporaryDirectory(prefix='nx2navi-dynres-') as tmp:
        c = Path(tmp) / 'check.c'
        c.write_text(HARNESS % body, encoding='utf-8')
        exe = Path(tmp) / ('check.exe' if win else 'check')
        subprocess.check_call([cc, '-O2', '-Wall', '-Wextra', '-Wno-unused-function', str(c), '-o', str(exe),
                               *([] if win else ['-lm'])], env=env)
        return subprocess.call([str(exe)])


if __name__ == '__main__':
    sys.exit(main())
