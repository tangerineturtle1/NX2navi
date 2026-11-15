"""Check runtime/gfx_shader.c's carry refusal: a previous translator's translation whose texture pool array has the
earlier length (32768, vk.h VK_POOL_TEX_CARRIED) is not served while the current translator may translate the shader,
because its clamp draws every texture numbered from 32768 up with slot 32767. The check cuts pool_short, carry_usable,
carry_load, carry_find, carry_find_iid and carry_last out of gfx_shader.c, compiles them against stand-ins for the disk
and memory caches, and walks the lookups: a refused entry is freed and the lookup goes on to the next previous identity,
the owned branch (gfx_shader_load_private) frees it as well, and entries with the current length, the sampler pool or no
pool are served as before. carry_last, which runs once the current translator has failed the shader, serves the refused
translation after all and puts it in the memory cache, where the memory lookup finds it. pool_short_note, which counts
the shaders of the earlier length that enter the memory cache for vk_texture.c's warning, counts those only, and logs
only one that is not carried. gfx_shader.c's POOL_TEX_SHORT must equal vk.h's VK_POOL_TEX_CARRIED.

No GPU, no game files, any Python.

Usage: carry_pool_check.py
"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():
    """$CC, else MSYS2 UCRT64's clang, else clang on PATH."""
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "gfx.h"

typedef struct GfxShader {
    GfxShaderRes *res[GFX_RES_KINDS];
    int nres[GFX_RES_KINDS];
    int stage;
    uint8_t carried;
    int pool_len;                 /* the stand-in disk entry's texture pool length, -1 none */
    int keyj;                     /* the previous identity whose key it was read under */
} GfxShader;
typedef struct { uint32_t slots, attrs, fouts; } UseMask;


static struct {
    int n_prev;
    uint8_t prev[8][32], prev_eff[8][32];
    volatile int64_t refused;
} C;
static int carry_on(void) { return C.n_prev > 0; }
static int64_t plat_inc64(volatile int64_t *p) { return ++*p; }
static int32_t plat_inc32(volatile int32_t *p) { return ++*p; }
static int logs;
static char last_log[512];
void rt_log(const char *fmt, ...) {
    logs++;
    snprintf(last_log, sizeof last_log, "%s", fmt);
}

/* The disk cache: entry j of the previous identities (key byte 0 = j) holds a shader whose texture pool array has
   disk_len[j] entries (0: no entry), of stage disk_stage[j]. */
static int disk_len[8], disk_stage[8], loads, frees, live;
static GfxShader *mem;            /* the memory cache: at most one shader here */
static void key_of_input_id(const uint8_t id[32], const uint8_t iid[32], uint8_t key[32]) {
    memset(key, 0, 32);
    key[0] = id[0];
}
static void iid_of(const uint8_t h[32], const GfxShaderAnswers *a, const UseMask *m, uint8_t iid[32]) { memset(iid, 0, 32); }
static int use_lookup(const uint8_t id[32], const uint8_t h[32], UseMask *m) { return 1; }
static GfxShader *mem_find(const uint8_t key[32]) { return mem && mem->keyj == key[0] ? mem : NULL; }
static GfxShader *mem_insert(GfxShader *s) { mem = s; return s; }
static void key_hex(const uint8_t key[32], char out[65]) {
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", key[i]);
}
static void shader_free(GfxShader *s) { frees++, live--; free(s->res[GFX_RES_TEX]); free(s); }
static GfxShader *disk_load(const uint8_t key[32], const uint8_t *iid) {
    int j = key[0];
    if (!disk_len[j]) return NULL;
    loads++, live++;
    GfxShader *s = calloc(1, sizeof *s);
    s->stage = disk_stage[j];
    s->keyj = j;
    s->pool_len = disk_len[j];
    s->res[GFX_RES_TEX] = calloc(3, sizeof(GfxShaderRes));
    s->nres[GFX_RES_TEX] = 3;
    s->res[GFX_RES_TEX][0] = (GfxShaderRes){.slot = 2, .count = 1};                  /* a bound texture */
    s->res[GFX_RES_TEX][1] = (GfxShaderRes){.slot = -1, .count = 4096, .pool = 2};   /* the sampler pool */
    if (disk_len[j] > 0) s->res[GFX_RES_TEX][2] = (GfxShaderRes){.slot = -1, .count = (uint16_t)disk_len[j], .pool = 1};
    else s->nres[GFX_RES_TEX] = 2;
    return s;
}
'''

MAIN = r'''
static void reset(void) {
    if (mem) shader_free(mem), mem = NULL;
    for (int j = 0; j < 8; j++) disk_len[j] = 0, disk_stage[j] = 1;
    C.refused = 0, logs = 0, loads = 0, frees = 0, live = 0;
}

int main(void) {
    uint8_t h[32] = {0}, iid[32] = {0};
    GfxShaderAnswers a = {0};
    C.n_prev = 2;
    C.prev[0][0] = 0x4d, C.prev_eff[0][0] = 0;   /* key byte 0 = identity index */
    C.prev[1][0] = 0x9c, C.prev_eff[1][0] = 1;

    /* The earlier length: refused, freed, nothing served; counted once, logged once. */
    reset();
    disk_len[0] = 32768;
    assert(!carry_find(h, &a, 1, 1, 0));
    assert(loads == 1 && frees == 1 && live == 0 && !mem);
    assert(C.refused == 1 && logs == 1 && strstr(last_log, "texture pool"));
    assert(!carry_find_iid(iid, 1, NULL));
    assert(C.refused == 2 && logs == 1 && live == 0);

    /* The lookup goes on to the next previous identity, whose translation has the current length. */
    reset();
    disk_len[0] = 32768, disk_len[1] = 40960;
    GfxShader *s = carry_find(h, &a, 1, 1, 0);
    assert(s && s->pool_len == 40960 && s->carried == 2 && mem == s);
    assert(C.refused == 1 && frees == 1 && live == 1);

    /* The current length, a shader with only the sampler pool and bound textures: served as before. */
    reset();
    disk_len[0] = 40960;
    s = carry_find(h, &a, 1, 1, 0);
    assert(s && s->carried == 1 && C.refused == 0 && logs == 0);
    reset();
    disk_len[0] = -1;   /* no texture pool array */
    s = carry_find_iid(iid, 1, NULL);
    assert(s && s->carried == 1 && C.refused == 0);

    /* A stage mismatch is still refused for its own reason, before the pool is looked at. */
    reset();
    disk_len[0] = 32768, disk_stage[0] = 2;
    assert(!carry_find(h, &a, 1, 1, 0) && C.refused == 0 && frees == 1);

    /* The owned branch (gfx_shader_load_private): refused and freed, or handed over with *owned 1. */
    reset();
    disk_len[0] = 32768;
    int owned = 0;
    assert(!carry_find_iid(iid, 1, &owned) && !owned && C.refused == 1 && frees == 1 && live == 0);
    reset();
    disk_len[0] = 32768, disk_len[1] = 40960;
    owned = 0;
    s = carry_find_iid(iid, 1, &owned);
    assert(s && owned && s->carried == 2 && !mem && C.refused == 1 && live == 1);
    shader_free(s);

    /* The last resort (carry_last, after the current translator failed the shader): the refused translation serves
       and enters the memory cache, from which the memory lookup (disk 0) serves it from then on; logged. */
    reset();
    disk_len[0] = 32768;
    uint8_t key[32] = {0x5a};
    assert(!carry_find(h, &a, 1, 1, 0) && C.refused == 1 && !mem && live == 0);
    int logs0 = logs;
    s = carry_last(key, h, &a, 1);
    assert(s && s->pool_len == 32768 && s->carried == 1 && mem == s && live == 1 && C.refused == 1);
    assert(logs == logs0 + 1 && strstr(last_log, "current translator failed"));
    assert(carry_find(h, &a, 1, 0, 0) == s && loads == 2);
    /* A usable translation found there is served as well, unlogged; none: NULL; another stage: still refused. */
    reset();
    disk_len[0] = 40960;
    s = carry_last(key, h, &a, 1);
    assert(s && s->pool_len == 40960 && logs == 0);
    reset();
    assert(!carry_last(key, h, &a, 1) && loads == 0);
    reset();
    disk_len[0] = 32768, disk_stage[0] = 2;
    assert(!carry_last(key, h, &a, 1) && frees == 1 && live == 0 && !mem);

    /* Memory only (disk 0) never reads the disk. */
    reset();
    disk_len[0] = 32768;
    assert(!carry_find(h, &a, 1, 0, 0) && !carry_find_iid(iid, 0, NULL) && loads == 0);

    /* pool_short_note (what mem_insert calls for every shader entering the memory cache): only shaders with a pool
       array of the earlier length count, the first is logged, gfx_shader_pool_short_seen gives the count. */
    reset();
    uint8_t k[32] = {0};
    disk_len[0] = 40960, disk_len[1] = 32768, disk_len[2] = -1;
    GfxShader *cur = disk_load(k, NULL);
    k[0] = 1;
    GfxShader *old = disk_load(k, NULL);
    k[0] = 2;
    GfxShader *none = disk_load(k, NULL);
    pool_short_note(cur), pool_short_note(none);
    assert(gfx_shader_pool_short_seen() == 0 && logs == 0);
    old->carried = 1;   /* served by carry_last, which logs it: counted, not logged as the translator in use */
    pool_short_note(old);
    assert(gfx_shader_pool_short_seen() == 1 && logs == 0);
    old->carried = 0;
    pool_short_note(old), pool_short_note(old);
    assert(gfx_shader_pool_short_seen() == 3 && logs == 1 && strstr(last_log, "older than this runtime"));
    shader_free(cur), shader_free(old), shader_free(none);

    reset();
    printf("carry pool check: earlier-length pool translations refused and freed, the lookup goes on, others served; "
           "a refused one serves once the current translator failed; shaders of the earlier length in use counted\n");
    return 0;
}
'''


def cut(source, start, end):
    i = source.index(start)
    return source[i:source.index(end, i)]


def define(source, name):
    """The decimal value of `#define name <n>` in source."""
    m = re.search(r'^#define ' + name + r' (\d+)\b', source, re.M)
    if not m:
        raise SystemExit(f'carry pool check: no #define {name}')
    return int(m.group(1))


def main():
    src = (ROOT / 'runtime' / 'gfx_shader.c').read_text(encoding='utf-8')
    # gfx_shader.c does not include vk.h, so it states the earlier length itself: the two must stay equal.
    short = define(src, 'POOL_TEX_SHORT')
    carried = define((ROOT / 'runtime' / 'vk.h').read_text(encoding='utf-8'), 'VK_POOL_TEX_CARRIED')
    if short != carried:
        print(f'carry pool check: gfx_shader.c POOL_TEX_SHORT is {short}, vk.h VK_POOL_TEX_CARRIED is {carried}')
        return 1
    production = cut(src, '#define POOL_TEX_SHORT', '// Whether the cache holds a previous translator\'s entry of input iid.')
    work = ROOT / 'build' / 'test' / 'carry_pool'
    work.mkdir(parents=True, exist_ok=True)
    c = work / 'check.c'
    c.write_text(HARNESS + production + MAIN, encoding='utf-8')
    exe = work / ('check.exe' if os.name == 'nt' else 'check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wno-unused-function', '-I', str(ROOT / 'runtime'),
                           str(c), '-o', str(exe)], env=env)
    return subprocess.run([str(exe)], env=env, timeout=60).returncode


if __name__ == '__main__':
    sys.exit(main())
