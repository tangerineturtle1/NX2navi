"""The camera-speed search leaves lazily armed memory alone (runtime/camera_speed.c rewrite): builds factor and rewrite,
cut from camera_speed.c by their text, over a 40 MiB stand-in for the guest's heap with stand-ins for the memory calls,
and checks that

  - copies of the array in plain memory are found and rewritten, also one across two 16 MiB pieces and one across two
    64 KiB steps of a piece that holds armed memory;
  - no read touches an armed range (a GPU-written buffer the CPU has not read), and a copy inside one is left as it is;
  - the first search that leaves armed memory out logs it once, and with every range armed (NX2NAVI_WB_PROBE) the search
    reads nothing.

No game files. Usage: python tests/camera_search_check.py [--cc <clang>]
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
typedef struct { uint64_t base, size; } RtRange;
typedef struct { RtRange aslr, stack, heap, alias, kernel, hostfn; uint64_t pool; } RtGuestLayout;
typedef struct { uint64_t base, size; uint32_t state, attr, perm, ipc, device, pad; } RtMemoryInfo;
#define HEAP (40u << 20)
static uint8_t *heap;
static RtGuestLayout layout;
static const RtGuestLayout *mem_layout(void) { return &layout; }
static uint32_t mem_query(uint64_t a, RtMemoryInfo *out) {
    uint64_t lo = (uint64_t)(uintptr_t)heap;
    memset(out, 0, sizeof *out);
    if (a >= lo && a < lo + HEAP) *out = (RtMemoryInfo){lo, HEAP, 1, 0, 3};
    return 0;
}
// armed: [arm_lo, arm_hi) as offsets in the heap; all_armed: the write-back probe
static uint64_t arm_lo, arm_hi;
static int all_armed, armed_reads, holds, logs;
static uint64_t off(uint64_t a) { return a - (uint64_t)(uintptr_t)heap; }
static int mem_lazy_touches(uint64_t a, uint64_t n) { return all_armed || (off(a) < arm_hi && off(a) + n > arm_lo); }
static uint64_t mem_untouched_run(uint64_t a, uint64_t end) { (void)a, (void)end; return 0; }
static int mem_read_hold(uint64_t a, uint64_t n) {
    holds++;
    armed_reads += off(a) < arm_hi && off(a) + n > arm_lo;
    return 1;
}
static void mem_read_release(void) {}
static void mem_thaw(uint64_t a, uint64_t n) { (void)a, (void)n; }
static void rt_log(const char *fmt, ...) { (void)fmt; logs++; }
static float base[8] = {0.8f, 1.2f, 1.6f, 2.0f, 2.4f};
static int base_n = 5;
%s
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); putchar('\n'); fails++; } } while (0)
static void put(uint64_t at, float k) { float v[8]; factor(v, k); memcpy(heap + at, v, sizeof(float) * 5); }
static int holds_at(uint64_t at, float k) { float v[8]; factor(v, k); return !memcmp(heap + at, v, sizeof(float) * 5); }
int main(void) {
    heap = calloc(HEAP, 1);
    layout.heap = (RtRange){(uint64_t)(uintptr_t)heap, HEAP};
    arm_lo = (20u << 20) + (128u << 10), arm_hi = arm_lo + (64u << 10);   // one armed 64 KiB step in the second piece
    uint64_t plain = 1000, across_piece = (16u << 20) - 8, across_step = (20u << 20) + (64u << 10) - 6;
    uint64_t in_armed = arm_lo + 100;
    put(plain, 1.0f), put(across_piece, 1.0f), put(across_step, 1.0f), put(in_armed, 1.0f);
    int n = rewrite(1.0f, 2.0f, 1);
    CHECK(n == 3, "three copies outside armed memory rewritten (%%d)", n);
    CHECK(holds_at(plain, 2.0f) && holds_at(across_piece, 2.0f) && holds_at(across_step, 2.0f),
          "the plain copy, the one across two pieces and the one across two steps hold the new factor");
    CHECK(holds_at(in_armed, 1.0f), "the copy in armed memory is left as it is");
    CHECK(!armed_reads, "no read touches armed memory (%%d)", armed_reads);
    CHECK(logs == 1, "the left-out memory is logged once (%%d)", logs);
    n = rewrite(2.0f, 1.5f, 1);
    CHECK(n == 3 && logs == 1, "a second search finds the same three and logs nothing more (%%d, %%d)", n, logs);
    all_armed = 1, holds = 0;
    n = rewrite(1.5f, 1.0f, 1);
    CHECK(n == 0 && holds == 0, "with every range armed nothing is read (%%d found, %%d reads)", n, holds);
    printf(fails ? "camera search: %%d failures\n" : "camera search: ok\n", fails);
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
    src = (ROOT / 'runtime' / 'camera_speed.c').read_text(encoding='utf-8')
    body = cut(src, '#define COPIES 16', '\n// The copies of the last search, still holding the array at factor k')
    with tempfile.TemporaryDirectory(prefix='nx2navi-camera-search-') as tmp:
        c = Path(tmp) / 'check.c'
        c.write_text(HARNESS % body, encoding='utf-8')
        exe = Path(tmp) / ('check.exe' if win else 'check')
        subprocess.check_call([cc, '-O2', '-Wall', '-Wno-unused-function', str(c), '-o', str(exe)], env=env)
        return subprocess.call([str(exe)])


if __name__ == '__main__':
    sys.exit(main())
