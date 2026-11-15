"""The vsync event list reuses the slots of destroyed events (runtime/gfx_present.c gfx_vsync_bind): builds
gfx_vsync_bind, cut from gfx_present.c by its text, with stand-ins for the lock, the log, the vsync thread start and
kern_system_event_live, and checks that

  - binding an address already listed changes nothing;
  - with 16 live events listed, a 17th takes the slot of the first destroyed one and keeps the others;
  - with all 16 live, a 17th still ends the process with the "more than 16 live vsync events" fatal.

No game files. Usage: python tests/vsync_bind_check.py [--cc <clang>]
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
#include <string.h>
typedef int PlatMutex;
#define PLAT_MUTEX_INIT 0
static void plat_mutex_lock(PlatMutex *m) { (void)m; }
static void plat_mutex_unlock(PlatMutex *m) { (void)m; }
static void rt_log(const char *fmt, ...) { (void)fmt; }
static void rt_fatal(const char *fmt, ...) { printf("FATAL: %%s\n", fmt); fflush(stdout); exit(3); }
static int dead[64];   // events the game destroyed, by address (1..63)
static int kern_system_event_live(uint64_t ev) { return ev < 64 && !dead[ev]; }
static int ensured;
static void vsync_ensure(void) { ensured++; }
static PlatMutex g_vs_lock = PLAT_MUTEX_INIT;
static uint64_t g_vs_ev[16];
static int g_vs_nev;
%s
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); putchar('\n'); fails++; } } while (0)
static int listed(uint64_t ev) {
    for (int i = 0; i < g_vs_nev; i++)
        if (g_vs_ev[i] == ev) return 1;
    return 0;
}
int main(int argc, char **argv) {
    for (uint64_t ev = 1; ev <= 16; ev++) gfx_vsync_bind(ev);
    gfx_vsync_bind(5);
    CHECK(g_vs_nev == 16, "16 events listed, an address bound again is not listed twice (%%d)", g_vs_nev);
    CHECK(ensured == 17, "every bind makes sure the vsync thread runs (%%d)", ensured);
    if (argc > 1 && !strcmp(argv[1], "full")) {
        gfx_vsync_bind(17);   // every listed event is live
        printf("no fatal\n");
        return 0;
    }
    dead[7] = dead[9] = 1;
    gfx_vsync_bind(17);
    CHECK(g_vs_nev == 16 && listed(17) && !listed(7) && listed(9), "a 17th takes the first destroyed slot only");
    for (uint64_t ev = 1; ev <= 16; ev++)
        if (ev != 7) CHECK(listed(ev), "event %%llu stays listed", (unsigned long long)ev);
    gfx_vsync_bind(18);
    CHECK(listed(18) && !listed(9), "an 18th takes the next destroyed slot");
    printf(fails ? "vsync bind: %%d failures\n" : "vsync bind: ok\n", fails);
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
    src = (ROOT / 'runtime' / 'gfx_present.c').read_text(encoding='utf-8')
    body = cut(src, 'void gfx_vsync_bind(uint64_t system_event) {', '\nvoid gfx_vsync_bind_counter(')
    with tempfile.TemporaryDirectory(prefix='nx2navi-vsync-bind-') as tmp:
        c = Path(tmp) / 'check.c'
        c.write_text(HARNESS % body, encoding='utf-8')
        exe = Path(tmp) / ('check.exe' if win else 'check')
        subprocess.check_call([cc, '-O2', '-Wall', '-Wno-unused-function', str(c), '-o', str(exe)], env=env)
        rc = subprocess.call([str(exe)])
        p = subprocess.run([str(exe), 'full'], capture_output=True, text=True)
        if p.returncode != 3 or 'more than 16 live vsync events' not in p.stdout:
            print(f'FAIL: a 17th live vsync event must end with the fatal (exit {p.returncode}: {p.stdout.strip()})')
            return 1
        print('vsync bind full: ok (fatal with 16 live events)')
        return rc


if __name__ == '__main__':
    sys.exit(main())
