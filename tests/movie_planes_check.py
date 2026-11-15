"""Movie conversion keeps its deband planes per decoder and drops a frame it has no memory for
(runtime/ov_movie2.c planes_for, make_picture): builds the deband, the conversions, run_stripes, planes_for and
make_picture, cut from ov_movie2.c by their text, with stand-ins for dav1d's picture, the platform calls and a malloc
that fails on request, and checks that

  - two pictures of one size allocate the planes once, a larger picture makes them again;
  - the NV12 frame (and with the overlay on, the RGBA frame) equals what fresh planes give for the same input, also
    when the kept planes still hold an earlier picture;
  - a plane allocation that fails drops the frame (0) without a fatal and leaves nothing allocated, and the next
    picture gets its planes.

No game files, no dav1d. Usage: python tests/movie_planes_check.py [--cc <clang>]
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
enum { DAV1D_PIXEL_LAYOUT_I420 = 1 };
typedef struct { int w, h, bpc, layout; } Dav1dPictureParameters;
typedef struct { int color_range, mtrx, pri, trc; } Dav1dSequenceHeader;
typedef struct { int64_t timestamp; } Dav1dDataProps;
typedef struct {
    Dav1dPictureParameters p;
    const Dav1dSequenceHeader *seq_hdr;
    void *data[3];
    ptrdiff_t stride[2];
    Dav1dDataProps m;
} Dav1dPicture;
typedef struct { uint8_t *nv12, *rgba; int w, h; int64_t ts; int eos; } Picture;
typedef struct { uint16_t *y16, *u16, *v16; size_t plane_room; } Video;
typedef int PlatMutex;
typedef int PlatCond;
#define PLAT_MUTEX_INIT 0
#define PLAT_COND_INIT 0
static void plat_mutex_lock(PlatMutex *m) { (void)m; }
static void plat_mutex_unlock(PlatMutex *m) { (void)m; }
static void plat_cond_wait(PlatCond *c, PlatMutex *m) { (void)c, (void)m; }
static void plat_cond_broadcast(PlatCond *c) { (void)c; }
static int plat_thread_spawn(void (*fn)(void *), void *arg, const char *name, int prio) {
    (void)fn, (void)arg, (void)name, (void)prio;
    return 0;   // no helper threads: run_stripes runs every stripe itself
}
static int32_t plat_xchg32(volatile int32_t *p, int32_t v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static void rt_log(const char *fmt, ...) { (void)fmt; }
static void rt_fatal(const char *fmt, ...) { printf("FATAL: %%s\n", fmt); fflush(stdout); exit(3); }
static int overlay_on;
static const char *cfg_get(const char *name) { return !strcmp(name, "NX2NAVI_MOVIE_OVERLAY") && overlay_on ? "1" : NULL; }
// Counted allocations; fail_at: the call number that returns NULL (0: none).
static int calls, live, fail_at;
static void *t_malloc(size_t n) {
    if (++calls == fail_at) return NULL;
    void *p = malloc(n);
    if (p) live++;
    return p;
}
static void t_free(void *p) {
    if (p) live--;
    free(p);
}
#define malloc t_malloc
#define free t_free
%s
#undef malloc
#undef free
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); putchar('\n'); fails++; } } while (0)

// An 8-bit 4:2:0 picture: smooth gradients with a little texture, in planes wider than the picture (dav1d's strides).
static void picture(Dav1dPicture *pic, int w, int h, int seed, uint8_t *planes) {
    memset(pic, 0, sizeof *pic);
    pic->p = (Dav1dPictureParameters){w, h, 8, DAV1D_PIXEL_LAYOUT_I420};
    pic->stride[0] = w + 32, pic->stride[1] = w / 2 + 16;
    uint8_t *y = planes, *u = y + pic->stride[0] * h, *v = u + pic->stride[1] * (h / 2);
    for (int r = 0; r < h; r++)
        for (int x = 0; x < w; x++) y[r * pic->stride[0] + x] = (uint8_t)(16 + (x + r + seed) / 3 + ((x * r + seed) %% 5 == 0));
    for (int r = 0; r < h / 2; r++)
        for (int x = 0; x < w / 2; x++) {
            u[r * pic->stride[1] + x] = (uint8_t)(100 + (x + seed) / 4);
            v[r * pic->stride[1] + x] = (uint8_t)(140 - (r + seed) / 4);
        }
    pic->data[0] = y, pic->data[1] = u, pic->data[2] = v;
    pic->m.timestamp = seed;
}

// What fresh planes give: the NV12 frame (and the RGBA one with the overlay on) from one stripe over every row.
static void reference(const Dav1dPicture *pic, uint8_t *nv12, uint8_t *rgba) {
    int w = pic->p.w, h = pic->p.h;
    uint16_t *Y = malloc((size_t)w * h * 2), *U = malloc((size_t)w * h / 2), *V = malloc((size_t)w * h / 2);
    if (!overlay_on) {
        Stripe s = {2, 0, h, w, h, nv12, Y, U, V, NULL, pic->data[0], pic->data[1], pic->data[2], pic->stride[0],
                    pic->stride[1]};
        stripe_run(&s);
    } else {
        for (int r = 0; r < h; r++) memcpy(nv12 + (size_t)r * w, (uint8_t *)pic->data[0] + r * pic->stride[0], (size_t)w);
        uint8_t *uv = nv12 + (size_t)w * h;
        for (int r = 0; r < h / 2; r++)
            for (int x = 0; x < w / 2; x++) {
                uv[(size_t)r * w + 2 * x] = ((uint8_t *)pic->data[1])[r * pic->stride[1] + x];
                uv[(size_t)r * w + 2 * x + 1] = ((uint8_t *)pic->data[2])[r * pic->stride[1] + x];
            }
        Stripe s = {0, 0, h, w, h, nv12, Y, U, V, rgba};
        stripe_run(&s);
        s.phase = 1;
        stripe_run(&s);
    }
    free(Y), free(U), free(V);
}

static void same(const Picture *p, const Dav1dPicture *pic, const char *what) {
    int w = pic->p.w, h = pic->p.h;
    uint8_t *nv12 = calloc((size_t)w * h * 3 / 2, 1), *rgba = calloc((size_t)w * h * 4, 1);
    reference(pic, nv12, rgba);
    if (!overlay_on) CHECK(!memcmp(p->nv12, nv12, (size_t)w * h * 3 / 2), "%%s: the NV12 frame equals fresh planes'", what);
    else CHECK(p->rgba && !memcmp(p->rgba, rgba, (size_t)w * h * 4), "%%s: the RGBA frame equals fresh planes'", what);
    free(nv12), free(rgba);
}

static void release(Picture *p) {
    t_free(p->nv12), t_free(p->rgba);
    p->nv12 = p->rgba = NULL;
}

int main(int argc, char **argv) {
    overlay_on = argc > 1 && !strcmp(argv[1], "overlay");
    const char *mode = overlay_on ? "overlay" : "nv12";
    uint8_t *planes = malloc(1 << 20);
    Dav1dPicture pic;
    Picture p;
    Video v = {0};
    int per_pic = overlay_on ? 2 : 1;   // the frame allocations of a picture besides the planes
    picture(&pic, 64, 48, 1, planes);
    CHECK(make_picture(&v, &pic, &p) == 1, "%%s: the first picture is made", mode);
    CHECK(calls == per_pic + 3 && v.plane_room == 64 * 48, "%%s: the first picture allocates the planes (%%d calls)", mode, calls);
    same(&p, &pic, "first picture");
    release(&p);
    picture(&pic, 64, 48, 7, planes);   // other content, same size: the planes hold the first picture's values
    CHECK(make_picture(&v, &pic, &p) == 1, "%%s: the second picture is made", mode);
    CHECK(calls == 2 * per_pic + 3, "%%s: the second picture of that size reuses the planes (%%d calls)", mode, calls);
    same(&p, &pic, "second picture");
    release(&p);
    CHECK(live == 3, "%%s: only the three planes stay allocated between pictures (%%d)", mode, live);
    picture(&pic, 96, 64, 3, planes);
    CHECK(make_picture(&v, &pic, &p) == 1 && v.plane_room == 96 * 64, "%%s: a larger picture is made", mode);
    CHECK(calls == 3 * per_pic + 6 && live == 3 + per_pic, "%%s: it makes the planes again (%%d calls, %%d live)", mode, calls, live);
    same(&p, &pic, "larger picture");
    release(&p);
    picture(&pic, 64, 48, 5, planes);
    CHECK(make_picture(&v, &pic, &p) == 1 && v.plane_room == 96 * 64, "%%s: a smaller picture uses the larger planes", mode);
    same(&p, &pic, "smaller picture");
    release(&p);
    t_free(v.y16), t_free(v.u16), t_free(v.v16);   // finalize
    // A second decoder whose U plane allocation fails: the frame is dropped, nothing stays allocated.
    Video w2 = {0};
    int before = live;
    fail_at = calls + per_pic + 2;
    CHECK(make_picture(&w2, &pic, &p) == 0, "%%s: a failed plane allocation drops the frame", mode);
    CHECK(live == before && !w2.y16 && !w2.u16 && !w2.v16 && !w2.plane_room,
          "%%s: nothing stays allocated after the failure (%%d live, %%d before)", mode, live, before);
    fail_at = 0;
    CHECK(make_picture(&w2, &pic, &p) == 1 && w2.plane_room == 64 * 48, "%%s: the next picture gets its planes", mode);
    same(&p, &pic, "picture after a failure");
    release(&p);
    t_free(w2.y16), t_free(w2.u16), t_free(w2.v16);
    CHECK(live == 0, "%%s: everything freed (%%d live)", mode, live);
    printf(fails ? "movie planes (%%s): %%d failures\n" : "movie planes (%%s): ok\n", mode, fails);
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
    src = (ROOT / 'runtime' / 'ov_movie2.c').read_text(encoding='utf-8')
    body = cut(src, '#define DEBAND_THRESH', '\n// Queues a decoded picture, waiting while the queue is full')
    with tempfile.TemporaryDirectory(prefix='nx2navi-movie-planes-') as tmp:
        c = Path(tmp) / 'check.c'
        c.write_text(HARNESS % body, encoding='utf-8')
        exe = Path(tmp) / ('check.exe' if win else 'check')
        subprocess.check_call([cc, '-O2', '-Wall', '-Wno-unused-function', str(c), '-o', str(exe), '-lm'], env=env)
        rc = 0
        for mode in ([], ['overlay']):
            rc = subprocess.call([str(exe), *mode]) or rc
        return rc


if __name__ == '__main__':
    sys.exit(main())
