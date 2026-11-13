"""Exercise the runtime's 64-bit collectible lookup without game files."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'runtime/debug_tools.c').read_text(encoding='utf-8')
match = re.search(r'static int skull_index\(uint64_t m, uint64_t key\) \{', source)
assert match, 'individual collectible lookup has not been implemented'
start = match.start()
end = source.index('\n}', match.end()) + 2
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
static unsigned char manager[0xd00];
static struct { uint64_t key; unsigned char rest[8]; } records[4];
static int rd(uint64_t a, void *v, size_t n) {
    if (!a) return 0;
    memcpy(v, (void *)(uintptr_t)a, n); return 1;
}
static uint64_t rd_ptr(uint64_t a) { uint64_t p; memcpy(&p,(void *)(uintptr_t)a,8);return p; }
''' + source[start:end] + r'''
int main(void) {
    uint64_t m=(uintptr_t)manager, p=(uintptr_t)records;
    uint32_t n=4;
    memcpy(manager+0xc58,&n,4);memcpy(manager+0xc60,&p,8);
    records[0].key=1;records[1].key=0x100000001ull;
    records[2].key=0x8000000000000001ull;records[3].key=UINT64_MAX;
    for (int i=0;i<4;i++) assert(skull_index(m,records[i].key)==i);
    assert(skull_index(m,0)==-1);assert(skull_index(m,2)==-1);
    assert(skull_index(m,0x8000000000000000ull)==-1);
    n=0;memcpy(manager+0xc58,&n,4);assert(skull_index(m,1)==-1);
    n=200001;memcpy(manager+0xc58,&n,4);assert(skull_index(m,1)==-1);
    assert(skull_index(0,1)==-1);
    return 0;
}
'''
match = re.search(r'static int skull_tally\(const int now\[SKULL_COUNT\], int8_t want\[SKULL_COUNT\]\) \{', source)
assert match, 'the token count is not derived from the collection flags'
tally = source[match.start():source.index('\n}', match.end()) + 2]
tally_harness = r'''
#include <assert.h>
#include <stdint.h>
enum { SKULL_COUNT = 100 };
''' + tally + r'''
int main(void) {
    int now[SKULL_COUNT] = {0};
    int8_t want[SKULL_COUNT];
    for (int i = 0; i < SKULL_COUNT; i++) want[i] = -1;
    assert(skull_tally(now, want) == 0);
    for (int i = 0; i < 11; i++) now[i] = 1;
    assert(skull_tally(now, want) == 11);
    // Eighteen changes asked for and not applied by the game yet count already.
    for (int i = 11; i < 29; i++) want[i] = 1;
    assert(skull_tally(now, want) == 29);
    assert(want[11] == 1);
    // Applied ones are forgotten and still count once.
    for (int i = 11; i < 29; i++) now[i] = 1;
    assert(skull_tally(now, want) == 29);
    for (int i = 11; i < 29; i++) assert(want[i] == -1);
    // Taking one back lowers the count before the game has applied it.
    want[0] = 0;
    assert(skull_tally(now, want) == 28);
    assert(want[0] == 0);
    // A flag that cannot be read leaves the count alone, unless a change for it is pending.
    now[50] = -1;
    assert(skull_tally(now, want) == -1);
    want[50] = 1;
    assert(skull_tally(now, want) == 29);
    return 0;
}
'''
match = re.search(r'static int32_t skull_token_delta\(int target, int now\) \{', source)
assert match, 'the token count is not changed by a difference'
start = source.rindex('\nstatic int32_t token_base', 0, match.start()) + 1
delta = source[start:source.index('\n}', match.end()) + 2]
delta_harness = r'''
#include <assert.h>
#include <stdint.h>
''' + delta + r'''
int main(void) {
    // The game adds what is queued; 11 and a target of 29 add 18 once.
    assert(skull_token_delta(29, 11) == 18);
    assert(skull_token_delta(29, 11) == 0);    // not applied yet: nothing more
    assert(skull_token_delta(29, 29) == 0);    // applied
    // A count raised too far comes back down.
    assert(skull_token_delta(29, 243) == -214);
    assert(skull_token_delta(29, 243) == 0);
    assert(skull_token_delta(29, 29) == 0);
    // A second change before the first is applied adds only the rest.
    assert(skull_token_delta(30, 29) == 1);
    assert(skull_token_delta(28, 29) == -2);
    assert(skull_token_delta(28, 28) == 0);
    // A token picked up in the game raises the count and its flag together: nothing to add.
    assert(skull_token_delta(29, 29) == 0);
    assert(skull_token_delta(30, 30) == 0);
    return 0;
}
'''

def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


requests = source[source.index('enum { Q_FLAG,'):source.index('static void ask(Req r)')]
pending = (function('static void skull_pending_refresh(void)')
           if 'static void skull_pending_refresh(void)' in source else '')
lifecycle_harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
enum { SKULL_COUNT = 100, MOD_MAIN_BASE = 0 };
typedef struct { int unused; } Cpu;
typedef int PlatMutex;
#define PLAT_MUTEX_INIT 0
static void plat_mutex_lock(PlatMutex *m) { (void)m; }
static void plat_mutex_unlock(PlatMutex *m) { (void)m; }
''' + requests + r'''
static int values[SKULL_COUNT], additions, held;
static int8_t queued[SKULL_COUNT];
static int8_t skull_want[SKULL_COUNT] = {[0 ... SKULL_COUNT - 1] = -1};
static const char TOKEN_COUNT[] = "token";
static int dbg_supported(void) { return 1; }
static uint64_t player(void) { return 1; }
static uint64_t gmd(void) { return 0x1000; }
static void test_script(uint64_t t) { (void)t; }
static void bugcap_replay_tick(uint64_t t) { (void)t; }
static void warp_tick(Cpu *c, uint64_t t) { (void)c; (void)t; }
static int dbg_skull_collected(int i) { return values[i]; }
static int dbg_skull_tokens(void) { return held; }
static uint32_t hash_of(const char *s) { (void)s; return 1; }
static int skull_index(uint64_t m, uint64_t key) { (void)m; return key < SKULL_COUNT ? (int)key : -1; }
static int rd(uint64_t a, void *out, size_t n) {
    uint32_t v = a == gmd() + 0xf0 || a == gmd() + 0xc68 ? 256 : 0;
    assert(n == sizeof v); memcpy(out, &v, n); return 1;
}
static int wr(uint64_t a, const void *p, size_t n) { (void)a; (void)p; (void)n; return 1; }
static void rt_log(const char *fmt, ...) { (void)fmt; }
static void rt_call(Cpu *c, uint64_t fn, int n, ...) {
    (void)c; (void)n;
    va_list ap; va_start(ap, n); (void)va_arg(ap, uint64_t);
    uint64_t value = va_arg(ap, uint64_t);
    if (fn == 0xf071c0) {
        (void)va_arg(ap, uint64_t); uint64_t i = va_arg(ap, uint64_t);
        assert(i < SKULL_COUNT); queued[i] = (int8_t)value;
    } else if (fn == 0xfc42d0) additions += (int32_t)value;
    else assert(0);
    va_end(ap);
}
''' + tally + '\n' + delta + '\n' + pending + '\n' + function('static void skull_tokens_sync(') + '\n' + function('void dbg_tick(') + r'''
static void apply(void) {
    for (int i = 0; i < SKULL_COUNT; i++) {
        if (queued[i] >= 0) values[i] = queued[i];
        queued[i] = -1;
    }
    held += additions; additions = 0;
}
static void request(Cpu *c, int i, int on) {
    q[q_n++] = (Req){Q_SKULL, (uint32_t)i, (uint64_t)i, (uint64_t)on, 0};
    dbg_tick(c);
}
int main(void) {
    Cpu c = {0}; memset(queued, -1, sizeof queued);
    for (int i = 0; i < 11; i++) values[i] = 1;
    held = 11;
    request(&c, 11, 1); assert(additions == 1);
    dbg_tick(&c); assert(additions == 1); // pending work is not queued twice
    apply(); assert(held == 12);
    dbg_tick(&c); // no menu request: applied intent must still be retired
    assert(skull_want[11] == -1 && token_pending == 0);
    memset(values, 0, sizeof values);
    for (int i = 0; i < 10; i++) values[i] = 1;
    held = 10; // reload an older save
    request(&c, 12, 1); assert(additions == 1);
    apply(); assert(held == 11);
    dbg_tick(&c); assert(skull_want[12] == -1 && token_pending == 0);
    request(&c, 12, 0); assert(additions == -1);
    dbg_tick(&c); assert(additions == -1);
    apply(); dbg_tick(&c); assert(held == 10 && skull_want[12] == -1 && token_pending == 0);
    assert(stat_flags == 3 && stat_writes == 0 && stat_refused == 0);
    return 0;
}
'''
clang = Path(os.environ.get('NX2NAVI_TEST_CLANG', str(Path(os.environ.get('LOCALAPPDATA', '')) /
             'Programs/NX2navi/toolchain/msys64/ucrt64/bin/clang.exe')))
with tempfile.TemporaryDirectory() as tmp:
    for name, text in (('lookup', harness), ('tally', tally_harness), ('delta', delta_harness), ('lifecycle', lifecycle_harness)):
        c = Path(tmp) / f'{name}.c'
        exe = Path(tmp) / f'{name}.exe'
        c.write_text(text, encoding='utf-8')
        subprocess.run([str(clang), '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
print('Individual collectible lookup and token count checks passed')
