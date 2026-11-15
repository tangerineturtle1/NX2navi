"""The count of recording copies whose control memory the game reused (runtime/nvn_api_cmd.c, NX2NAVI_COPY_RETIRE):
builds ctl_fingerprint, retire_visit, retire_queue and retire_pass, cut from nvn_api_cmd.c by their text, with a stub
table of live copies and stub guest memory, and checks that

  - a copy's control bytes are fingerprinted at the pass after it was made, and nothing is counted while it is in use;
  - once no submission used it for 5 minutes, changed bytes count it as changed and unmapped memory as gone, unchanged
    bytes count nothing, and a copy used again within 5 minutes leaves the counts;
  - a lazily armed range is never read (unknown, not counted), and an untouched page of the lazy heap is taken as zeros
    without a read;
  - a retired copy, or one whose handle a new copy took, leaves its records and the counts;
  - a copy released and recorded again before the next submission, with the same handle, made_at and size, keeps one
    record, fingerprinted as its control bytes are now, so the one copy is not counted twice;
  - the visits go on where the last pass stopped: two passes over 5000 copies look each up at least once and none
    more than twice;
  - the sweep thread does not lower its priority below normal (retire_main), as nearly every step of a pass holds a lock
    the recording and submitting threads take.

No game files. Usage: python tests/copy_sweep_check.py [--cc <clang>]
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
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_hmap.h"
typedef int PlatMutex;
#define PLAT_MUTEX_INIT 0
static void plat_mutex_lock(PlatMutex *m) { (void)m; }
static void plat_mutex_unlock(PlatMutex *m) { (void)m; }
static void plat_mutex_lock_shared(PlatMutex *m) { (void)m; }
static void plat_mutex_unlock_shared(PlatMutex *m) { (void)m; }
static int64_t plat_add64(volatile int64_t *p, int64_t v) { return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST); }
static int cfg_int(const char *name) { (void)name; return 1; }
typedef struct { uint64_t magic, bytes, made_at, used_at, deferred, used_min; uint32_t pins, busy; } copy_prefix;
static PlatMutex copy_lock = PLAT_MUTEX_INIT;
#define COPY_IDLE_MINS 5
#define NCOPY 6000
static copy_prefix copies[NCOPY + 1];   // handle i: copies[i]; magic 0 means not live
static int lookups[NCOPY + 1];
static copy_prefix *copy_find(uint64_t handle) {
    if (handle > NCOPY) return NULL;
    lookups[handle]++;
    return copies[handle].magic ? &copies[handle] : NULL;
}
static uint64_t minute;
static uint64_t copy_minute(void) { return minute; }
// Guest memory: 16 bytes of control memory per copy; per copy, whether it is mapped, lazily armed or an untouched
// lazy-heap page (which must not be read).
static uint8_t ctl[NCOPY + 1][16];
static uint8_t unmapped[NCOPY + 1], armed[NCOPY + 1], untouched[NCOPY + 1];
static int reads_untouched, reads_armed;
static long slot_of(uint64_t a) {
    uint64_t lo = (uint64_t)(uintptr_t)ctl;
    return a >= lo && a < lo + sizeof ctl ? (long)((a - lo) / 16) : -1;
}
static int mem_lazy_touches(uint64_t a, uint64_t n) { (void)n; long i = slot_of(a); return i >= 0 && armed[i]; }
static uint64_t mem_untouched_run(uint64_t a, uint64_t end) { long i = slot_of(a); return i >= 0 && untouched[i] ? end : 0; }
static int mem_read_hold(uint64_t a, uint64_t n) {
    (void)n;
    long i = slot_of(a);
    if (i < 0 || unmapped[i]) return 0;
    reads_untouched += untouched[i], reads_armed += armed[i];
    return 1;
}
static void mem_read_release(void) {}
%s
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); putchar('\n'); fails++; } } while (0)
static void make(uint64_t h, uint64_t bytes) {
    copies[h] = (copy_prefix){.magic = 1, .bytes = bytes, .made_at = h * 3, .used_min = minute};
    retire_queue(h, (uint64_t)(uintptr_t)ctl[h], copies[h].made_at, bytes);
}
static int64_t ld(volatile int64_t *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
int main(void) {
    minute = 100;
    for (int i = 1; i <= 6; i++) memset(ctl[i], i, 16);
    armed[4] = 1;        // copy 4: a lazily armed range
    untouched[5] = 1;    // copy 5: an untouched lazy-heap page
    memset(ctl[5], 0, 16);
    for (uint64_t h = 1; h <= 6; h++) make(h, 1000 * h);
    retire_pass();
    CHECK(retire_nrec == 6, "six copies known (%%u)", retire_nrec);
    CHECK(retire_recs[3].fp == FP_UNKNOWN, "an armed range is not read");
    CHECK(reads_armed == 0 && reads_untouched == 0, "no read of an armed range or an untouched page");
    CHECK(ld(&retire_changed_n) == 0 && ld(&retire_gone_n) == 0, "nothing counted while in use");
    // Six minutes on: copy 2's bytes changed, copy 3's memory unmapped, copy 5 written with other bytes, copy 6 changed
    // but used a minute ago, copy 1 as it was.
    minute = 106;
    copies[6].used_min = 105;
    ctl[2][0] ^= 0xff;
    unmapped[3] = 1;
    untouched[5] = 0, ctl[5][7] = 9;
    ctl[6][3] ^= 1;
    retire_pass();
    CHECK(ld(&retire_changed_n) == 2 && ld(&retire_changed_b) == 2000 + 5000,
          "changed: copies 2 and 5 (%%lld, %%lld bytes)", (long long)ld(&retire_changed_n), (long long)ld(&retire_changed_b));
    CHECK(ld(&retire_gone_n) == 1 && ld(&retire_gone_b) == 3000, "gone: copy 3 (%%lld)", (long long)ld(&retire_gone_n));
    CHECK(reads_armed == 0, "the armed range stays unread");
    // Copy 2 released and recorded again before the next submission, over its changed control bytes: the same handle,
    // made_at and size come back. Its older record goes, and the new copy is fingerprinted as the bytes are now.
    retire_queue(2, (uint64_t)(uintptr_t)ctl[2], copies[2].made_at, copies[2].bytes);
    retire_pass();
    CHECK(retire_nrec == 6, "a copy recorded again with a known handle keeps one record (%%u records)", retire_nrec);
    CHECK(ld(&retire_changed_n) == 1 && ld(&retire_changed_b) == 5000,
          "the older record of copy 2 leaves the changed count (%%lld, %%lld bytes)", (long long)ld(&retire_changed_n),
          (long long)ld(&retire_changed_b));
    CHECK(ld(&retire_gone_n) == 1, "copy 3 stays gone (%%lld)", (long long)ld(&retire_gone_n));
    // Copy 2 used again: it leaves the changed count. Copy 3 retired (AddControlMemory again): it leaves its records.
    // Copy 5's handle taken by a new copy: the old record goes, the new one is fingerprinted.
    copies[2].used_min = 106;
    copies[3].magic = 0;
    copies[5] = (copy_prefix){.magic = 1, .bytes = 777, .made_at = 999, .used_min = minute};
    retire_queue(5, (uint64_t)(uintptr_t)ctl[5], 999, 777);
    retire_pass();
    CHECK(ld(&retire_changed_n) == 0 && ld(&retire_changed_b) == 0, "used or replaced copies leave the changed count (%%lld)",
          (long long)ld(&retire_changed_n));
    CHECK(ld(&retire_gone_n) == 0 && ld(&retire_gone_b) == 0, "a retired copy leaves the gone count");
    CHECK(retire_nrec == 5, "the retired copy and the replaced record are dropped (%%u records)", retire_nrec);
    // 5000 idle, unchanged copies: two passes look each up at least once, none more than twice.
    for (uint64_t h = 1; h <= NCOPY; h++) copies[h].magic = 0;
    retire_pass();   // drops every record
    CHECK(retire_nrec == 0, "every record dropped once nothing is live (%%u)", retire_nrec);
    memset(armed, 0, sizeof armed), memset(unmapped, 0, sizeof unmapped), memset(untouched, 0, sizeof untouched);
    minute = 200;
    for (uint64_t h = 1; h <= 5000; h++) make(h, 8);
    minute = 210;
    memset(lookups, 0, sizeof lookups);
    retire_pass();
    retire_pass();
    int low = 0, high = 0;
    for (uint64_t h = 1; h <= 5000; h++) low += lookups[h] < 1, high += lookups[h] > 2;
    CHECK(!low && !high, "two passes look every copy up once or twice (%%d never, %%d more than twice)", low, high);
    CHECK(ld(&retire_changed_n) == 0 && ld(&retire_gone_n) == 0, "unchanged idle copies count nothing");
    printf(fails ? "copy sweep: %%d failures\n" : "copy sweep: ok\n", fails);
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
    src = (ROOT / 'runtime' / 'nvn_api_cmd.c').read_text(encoding='utf-8')
    body = cut(src, '// ---------- recording copies whose control memory the game reused', '\nstatic void retire_main(')
    sweep = cut(src, '\nstatic void retire_main(', '\n}\n')
    lowered = [p for p in ('PLAT_PRIO_IDLE', 'PLAT_PRIO_LOWEST', 'PLAT_PRIO_BELOW_NORMAL') if p in sweep]
    if lowered:
        print(f'FAIL: retire_main lowers the sweep thread\'s priority ({", ".join(lowered)}); it holds locks the '
              'recording and submitting threads take')
        return 1
    with tempfile.TemporaryDirectory(prefix='nx2navi-copy-sweep-') as tmp:
        c = Path(tmp) / 'check.c'
        c.write_text(HARNESS % body, encoding='utf-8')
        exe = Path(tmp) / ('check.exe' if win else 'check')
        subprocess.check_call([cc, '-O2', '-Wall', '-Wno-unused-function', '-I', str(ROOT / 'runtime'), str(c), '-o',
                               str(exe)], env=env)
        return subprocess.call([str(exe)])


if __name__ == '__main__':
    sys.exit(main())
