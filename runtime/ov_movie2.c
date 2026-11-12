// Switch 2 movie playback: movie::VideoDecoder (subsdk0) answered by the host with dav1d.
//
// The title's movies are WebM with AV1 video only (Event/Movie/*.webm, 1920x1080 60 fps, 8-bit 4:2:0, limited
// range BT.709); their sound is a separate stream the game plays alongside (Sound/Resource/Stream/<movie>.bwav),
// so a movie that runs for its real length is also what makes the title sequence audible. The SDK's
// nn::movie::MediaExtractor demuxes as it is; subsdk0's VideoDecoder sits on nn::codec::video (a video server this
// host does not have), so the public VideoDecoder methods the game imports are replaced here. VideoFrame stays
// subsdk0's own object; its layout (subsdk0 VideoFrame::SetBuffer / SetPixelPlaneData / SetTimeStamp /
// SetEndOfStreamState, sizeof 0x90):
//   +0x08 + 8 i  plane i data (plane 0 = the buffer of SetBuffer)   +0x28 + 8 i  plane i filled size
//   +0x48        buffer capacity                                     +0x78        time stamp (TimeSpan, ns)
//   +0x80        pixel format          +0x84 pixel storage type      +0x88        end of stream (byte)
// nn::movie::Buffer (sdk): +0x00 data, +0x08 capacity, +0x10 filled size, +0x18 time stamp, +0x20 flags (8 = end
// of stream, as the game sets it at main+0x3b60014).
//
// Decoding is asynchronous, as on the hardware: DecodeAsync queues a copy of the sample and returns; one host thread
// per decoder runs dav1d and converts each picture to NV12 (the game's frame format: it sizes each frame buffer
// width * height * 3 / 2 and reads two planes) and to RGBA for the window. Decoding on the caller's thread stalled
// the game's movie loop (it also acquires frames) by up to 90 ms on heavy frames: visible stutter.
// Back-pressure: while QREADY samples or pictures wait, DecodeAsync answers R_BUSY and the input-ready event is
// clear; the game's decode step (main+0x3b5e600) treats any non-zero result as "try again" on that event.
// AcquireVideoFrame answers 0x7b when nothing is ready (subsdk0 VideoDecoder::AcquireVideoFrame's own result).
// The game draws the frame with its own shaders (a YUV compute pass). Each frame is debanded first (flat areas
// averaged with sub-level precision), so the encoder's flattened gradients (skies, clouds) do not show as hard bands,
// and written back to NV12 with a 4x4 ordered dither. With NX2NAVI_MOVIE_OVERLAY=1 the host instead shows the frame
// over the game image (gfx_present.c gfx_movie_frame_own), converted here: BT.709 limited range, bilinear chroma,
// the same dither. NX2NAVI_MOVIE_DEBAND=0 turns the deband off.
// NX2NAVI_MOVIES=off skips movies instead (romfs.c movie_skipped); this title then quits, so for testing use
// NX2NAVI_MOVIE_SKIP=<seconds>: a movie ends as if it had played to its end once its samples reach that time (the next
// sample becomes the end-of-stream marker, later ones are dropped), and the game goes on as after a whole movie.
#include <dav1d/dav1d.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "platform/platform.h"

#define R_NO_FRAME 0x7bu
#define R_BUSY 0x7du       // DecodeAsync while the queues are full: the game retries on its input-ready event
#define BUFFER_FLAG_EOS 0x8u
#define QMAX 8       // decoded pictures held at most
#define INMAX 8      // samples waiting for the decoder thread at most
#define QREADY 4     // input-ready while fewer than this many samples and pictures wait
#define NFRAMES 16   // registered VideoFrames
#define CQMAX 2      // decoded pictures waiting for the convert thread at most

typedef struct { uint8_t *nv12, *rgba; int w, h; int64_t ts; int eos; } Picture;
typedef struct { uint8_t *data; size_t size; int64_t ts; int eos; } Sample;
typedef struct { Dav1dPicture pic; uint64_t gen; int64_t ts; int eos; } Decoded;   // for the convert thread
typedef struct {
    PlatMutex lock;          // queues, frames, flags
    PlatMutex codec;         // dav1d: the decoder thread while it decodes, Flush / Finalize
    PlatCond work;           // a sample arrived, or quit
    PlatThread *thread;
    int run_index;           // which movie of the run this is, counted from 1 as decoders come up (for the log)
    // The convert thread debands and converts what dav1d decoded while the decoder thread decodes the next picture:
    // one after the other on the decoder thread the two took longer than the 16.7 ms a 60 fps movie frame has
    PlatThread *convert;
    PlatCond conv;           // a decoded picture arrived, a slot freed, or quit
    Decoded cq[CQMAX];
    int cqh, cqn;
    Dav1dContext *dav1d;
    uint8_t in_ev[0x40], out_ev[0x40];   // nn::os::SystemEventType each
    int in_bound, out_bound;
    uint64_t frame[NFRAMES];
    int busy[NFRAMES], nframe;
    Picture q[QMAX];
    int qh, qn;
    Sample in[INMAX];
    int ih, in_n;
    int quit, eos_queued;
    uint64_t generation;     // bumped by Flush: pictures of an older one are dropped
    uint64_t decoded, shown;
    int live;                // counted in movies_live: from Initialize, or a sample after a Flush, to the end-of-stream
                             // picture, Finalize or the destructor
    // The first shown picture's time stamp and when it was shown, for the pacing report's distance between the
    // picture and real time; clock_on: they hold (cleared by Flush and Initialize)
    int64_t ts0;
    uint64_t shown0_ns;
    int clock_on;
    // The game fetches pictures ahead into a list of its own (main+0x3b61df0) and lets go of each when the next one
    // takes its place on screen (ReleaseVideoFrame), so the distance to real time is measured there too: the newest
    // released picture's, and how far it moved since the first released one (the drift), in ns
    int64_t rel_lag, rel_lag0;
    int rel_on;
} Video;

static Video *video_of(void *obj) { return obj ? *(Video **)obj : NULL; }
static int movies_live;   // decoders playing (Video.live); frame_rate.c movie_cap_hz holds the frame rate meanwhile

int ov_movie_live(void) { return __atomic_load_n(&movies_live, __ATOMIC_RELAXED) > 0; }

static void set_live(Video *v, int live) {   // the guest's decode and acquire threads may race here
    if (__atomic_exchange_n(&v->live, live, __ATOMIC_RELAXED) != live)
        __atomic_add_fetch(&movies_live, live ? 1 : -1, __ATOMIC_RELAXED);
}
// Time summed for the pacing report (one movie plays at a time): on the decoder thread waiting for the game's next
// sample, dav1d_send_data and dav1d_get_picture; on the convert thread make_picture (deband and conversion)
static uint64_t t_wait_ns, t_send_ns, t_get_ns, t_make_ns, n_made;   // n_made: pictures converted

// under v->lock
static void update_events(Video *v) {
    int ready = v->qn < QREADY && v->in_n < QREADY && !v->eos_queued;
    if (v->in_bound) {
        if (ready) kern_signal_system_event((uint64_t)(uintptr_t)v->in_ev);
        else kern_clear_system_event((uint64_t)(uintptr_t)v->in_ev);
    }
    if (v->out_bound) {
        if (v->qn) kern_signal_system_event((uint64_t)(uintptr_t)v->out_ev);
        else kern_clear_system_event((uint64_t)(uintptr_t)v->out_ev);
    }
}

static void picture_free(Picture *p) {
    free(p->nv12);
    free(p->rgba);
    p->nv12 = p->rgba = NULL;
}

// under v->lock
static void clear_queues(Video *v) {
    while (v->qn) {
        picture_free(&v->q[v->qh]);
        v->qh = (v->qh + 1) % QMAX;
        v->qn--;
    }
    while (v->in_n) {
        free(v->in[v->ih].data);
        v->ih = (v->ih + 1) % INMAX;
        v->in_n--;
    }
    v->eos_queued = 0;
}

// Deband (the approach of mpv's deband filter, our own code): the movies' flat gradients (skies, clouds) are encoded
// as blocks of one level with hard contours between them. For each sample, four neighbours at a pseudo-random offset
// of 4..16 samples (rotated per pixel, so no pattern repeats) are compared with it; when all lie within THRESH levels
// the area is flat and the sample becomes their mean with 4 bits of fraction, else it keeps its value. Edges and
// texture differ by more than that and stay as they are. Output: plane * 16.
#define DEBAND_THRESH 3
// The deband taps: radii 4..16 at golden-angle rotations, so every tap of a sample at least DEBAND_REACH away from
// each edge lies inside the plane.
#define DEBAND_REACH 16
static int16_t deband_off[64][2];
static volatile int32_t deband_ready;
static void deband_table(void) {
    if (__atomic_load_n(&deband_ready, __ATOMIC_ACQUIRE)) return;
    for (int i = 0; i < 64; i++) {
        double r = 4 + (i * 12) / 63.0, t = i * 2.39996323;
        deband_off[i][0] = (int16_t)(r * __builtin_cos(t)), deband_off[i][1] = (int16_t)(r * __builtin_sin(t));
    }
    __atomic_store_n(&deband_ready, 1, __ATOMIC_RELEASE);
}
// One sample near an edge: a tap outside the plane keeps the sample as it is
static inline uint16_t deband_edge(const uint8_t *src, int w, int h, int stride, int step, int x, int y) {
    int c = src[(size_t)y * stride + x * step];
    const int16_t *d = deband_off[(x * 7 + y * 13 + ((x ^ y) >> 2)) & 63];
    int dx = d[0], dy = d[1];
    int xa = x + dx, xb = x - dx, ya = y + dy, yb = y - dy;
    int xc = x - dy, xd = x + dy, yc = y + dx, yd = y - dx;
    if (xa < 0 || xb < 0 || xc < 0 || xd < 0 || xa >= w || xb >= w || xc >= w || xd >= w || ya < 0 || yb < 0 ||
        yc < 0 || yd < 0 || ya >= h || yb >= h || yc >= h || yd >= h)
        return (uint16_t)(c * 16);
    int s0 = src[(size_t)ya * stride + xa * step], s1 = src[(size_t)yb * stride + xb * step];
    int s2 = src[(size_t)yc * stride + xc * step], s3 = src[(size_t)yd * stride + xd * step];
    int m0 = abs(s0 - c), m1 = abs(s1 - c), m2 = abs(s2 - c), m3 = abs(s3 - c);
    if (m0 > DEBAND_THRESH || m1 > DEBAND_THRESH || m2 > DEBAND_THRESH || m3 > DEBAND_THRESH) return (uint16_t)(c * 16);
    return (uint16_t)((s0 + s1 + s2 + s3) * 4);   // the mean, * 16
}
// Each sample becomes the mean of four taps around it (a rotated pair of mirrored offsets) when all four lie within
// DEBAND_THRESH of it, else stays as it is; the result is * 16, keeping the mean's fraction. Samples away from the
// edges, nearly all of them, skip the bounds tests and choose without a branch: 28 ms on one thread for a 1080p frame
// became 16 ms, and in play the stripes no longer overran the 16.7 ms a 60 fps movie frame has.
static void deband(const uint8_t *src, uint16_t *dst, int w, int h, int stride, int step, int y0, int y1) {
    deband_table();
    const int R = DEBAND_REACH;
    ptrdiff_t ta[64], tb[64];   // each offset as a distance in the plane: (+dx, +dy) and (-dy, +dx); their mirrors negate
    for (int i = 0; i < 64; i++) {
        ta[i] = (ptrdiff_t)deband_off[i][1] * stride + (ptrdiff_t)deband_off[i][0] * step;
        tb[i] = (ptrdiff_t)deband_off[i][0] * stride - (ptrdiff_t)deband_off[i][1] * step;
    }
    for (int y = y0; y < y1; y++) {
        uint16_t *o = dst + (size_t)y * w;
        if (y < R || y >= h - R || w <= 2 * R) {
            for (int x = 0; x < w; x++) o[x] = deband_edge(src, w, h, stride, step, x, y);
            continue;
        }
        for (int x = 0; x < R; x++) o[x] = deband_edge(src, w, h, stride, step, x, y);
        const uint8_t *row = src + (size_t)y * stride;
        const int y13 = y * 13;
        for (int x = R; x < w - R; x++) {
            const uint8_t *p = row + x * step;
            int c = *p, k = (x * 7 + y13 + ((x ^ y) >> 2)) & 63;
            const ptrdiff_t a = ta[k], b = tb[k];
            int s0 = p[a], s1 = p[-a], s2 = p[b], s3 = p[-b];
            int edge = (abs(s0 - c) > DEBAND_THRESH) | (abs(s1 - c) > DEBAND_THRESH) | (abs(s2 - c) > DEBAND_THRESH) |
                       (abs(s3 - c) > DEBAND_THRESH);
            o[x] = (uint16_t)(edge ? c * 16 : (s0 + s1 + s2 + s3) * 4);
        }
        for (int x = w - R; x < w; x++) o[x] = deband_edge(src, w, h, stride, step, x, y);
    }
}

// Debanded planes (* 16) -> RGBA, BT.709 limited range. Chroma is interpolated bilinearly between the 4:2:0 samples
// (centred between luma rows, co-sited with even columns); a 4x4 Bayer threshold is added before the final rounding
// to 8 bits, which turns the remaining sub-level steps into fine noise instead of bands.
static void to_rgba(const uint16_t *Y, const uint16_t *U, const uint16_t *V, uint8_t *out, int w, int h, int y0, int y1) {
    static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    const int cw = w / 2, ch = h / 2;
    for (int y = y0; y < y1; y++) {
        // the two chroma rows around this luma row, weighted 3:1 toward the nearer one
        int cy0 = (y - 1) >> 1, cy1 = cy0 + 1, wy1 = (y & 1) ? 1 : 3, wy0 = 4 - wy1;
        if (cy0 < 0) cy0 = 0;
        if (cy1 >= ch) cy1 = ch - 1;
        const uint16_t *u0 = U + (size_t)cy0 * cw, *u1 = U + (size_t)cy1 * cw;
        const uint16_t *v0 = V + (size_t)cy0 * cw, *v1 = V + (size_t)cy1 * cw, *yr = Y + (size_t)y * w;
        uint8_t *o = out + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            int cx0 = x >> 1, cx1 = (x & 1) && cx0 + 1 < cw ? cx0 + 1 : cx0;   // even: on the sample, odd: halfway
            int wx1 = cx1 != cx0 ? 2 : 0, wx0 = 4 - wx1;
            // chroma * 16 * 16 (plane * 16, weights summing to 16) -> >> 4: * 16
            int u = ((u0[cx0] * wx0 + u0[cx1] * wx1) * wy0 + (u1[cx0] * wx0 + u1[cx1] * wx1) * wy1) >> 4;
            int vv = ((v0[cx0] * wx0 + v0[cx1] * wx1) * wy0 + (v1[cx0] * wx0 + v1[cx1] * wx1) * wy1) >> 4;
            int d = u - 128 * 16, e = vv - 128 * 16;     // * 16
            int cc = (yr[x] - 16 * 16) * 298;            // * 16 * 256
            int dith = bayer[y & 3][x & 3] * 256 + 128;  // < 1 output step in the * 4096 unit
            int r = (cc + 459 * e + dith) >> 12;
            int g = (cc - 55 * d - 136 * e + dith) >> 12;
            int b = (cc + 541 * d + dith) >> 12;
            o[4 * x] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
            o[4 * x + 1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
            o[4 * x + 2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
            o[4 * x + 3] = 255;
        }
    }
}

// The overlay conversion runs in NSTRIPES horizontal stripes on short-lived threads, in two phases (deband every
// plane, then convert), since the convert reads chroma rows of the neighbouring stripe. One thread took 27 ms per
// 1080p frame: too slow for 60 fps.
#define NSTRIPES 6
typedef struct {
    int phase, y0, y1, w, h;
    uint8_t *nv12;
    uint16_t *Y16, *U16, *V16;
    uint8_t *rgba;
    const uint8_t *py, *pu, *pv;   // phase 2: the decoder's 8-bit planes and their strides (luma, chroma)
    ptrdiff_t ys, cs;
} Stripe;
// The debanded planes back into the NV12 frame (the game converts it itself), with a 4x4 ordered dither: the
// 4 fraction bits become texel-sized noise instead of bands.
static void to_nv12(const Stripe *t) {
    static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    int w = t->w;
    for (int y = t->y0; y < t->y1; y++)
        for (int x = 0; x < w; x++) {
            int v = (t->Y16[(size_t)y * w + x] + bayer[y & 3][x & 3]) >> 4;
            t->nv12[(size_t)y * w + x] = (uint8_t)(v > 255 ? 255 : v);
        }
    uint8_t *uv = t->nv12 + (size_t)w * t->h;
    for (int y = t->y0 / 2; y < t->y1 / 2; y++)
        for (int x = 0; x < w / 2; x++) {
            int d = bayer[y & 3][x & 3], i = y * (w / 2) + x;
            int u = (t->U16[i] + d) >> 4, v = (t->V16[i] + (15 - d)) >> 4;
            uv[(size_t)y * w + 2 * x] = (uint8_t)(u > 255 ? 255 : u);
            uv[(size_t)y * w + 2 * x + 1] = (uint8_t)(v > 255 ? 255 : v);
        }
}

static void stripe_run(void *arg) {
    Stripe *t = arg;
    if (t->phase == 0) {
        const uint8_t *uv = t->nv12 + (size_t)t->w * t->h;
        deband(t->nv12, t->Y16, t->w, t->h, t->w, 1, t->y0, t->y1);
        deband(uv, t->U16, t->w / 2, t->h / 2, t->w, 2, t->y0 / 2, t->y1 / 2);
        deband(uv + 1, t->V16, t->w / 2, t->h / 2, t->w, 2, t->y0 / 2, t->y1 / 2);
    } else if (t->phase == 2) {   // deband straight from the decoder's planes, so the NV12 rows can be written at once
        deband(t->py, t->Y16, t->w, t->h, t->ys, 1, t->y0, t->y1);
        deband(t->pu, t->U16, t->w / 2, t->h / 2, t->cs, 1, t->y0 / 2, t->y1 / 2);
        deband(t->pv, t->V16, t->w / 2, t->h / 2, t->cs, 1, t->y0 / 2, t->y1 / 2);
        to_nv12(t);
    } else if (t->rgba) {
        to_rgba(t->Y16, t->U16, t->V16, t->rgba, t->w, t->h, t->y0, t->y1);
    } else {
        to_nv12(t);
    }
}
static void run_stripes(Stripe base) {
    Stripe st[NSTRIPES];
    PlatThread *th[NSTRIPES] = {0};
    int rows = ((base.h + NSTRIPES - 1) / NSTRIPES + 1) & ~1;   // even: chroma rows split at the same places
    for (int i = 0; i < NSTRIPES; i++) {
        st[i] = base;
        st[i].y0 = i * rows < base.h ? i * rows : base.h;
        st[i].y1 = (i + 1) * rows < base.h ? (i + 1) * rows : base.h;
        if (i) th[i] = plat_thread_new(stripe_run, &st[i], "movie convert");
        if (i && !th[i]) stripe_run(&st[i]);
    }
    stripe_run(&st[0]);
    for (int i = 1; i < NSTRIPES; i++)
        if (th[i]) plat_thread_join(th[i]);
}

// Out of memory for a decoded frame: the frame is dropped (the game shows the previous one a little longer) and
// the first drop of a run is logged. The game used to stop here; on a PC whose commit charge reached its limit
// for a moment, that ended a run that the next second would have had memory for.
static void movie_oom_log(int w, int h) {
    static volatile int32_t drops;
    if (plat_xchg32(&drops, 1) == 0) rt_log("movie: out of memory for a %dx%d frame: dropped (later drops not logged)", w, h);
}

// One dav1d picture (8- or 10-bit 4:2:0) -> NV12 with 8-bit samples, and its RGBA; decoder thread
static int make_picture(const Dav1dPicture *pic, Picture *p) {
    int w = pic->p.w, h = pic->p.h, hbd = pic->p.bpc > 8, sh = pic->p.bpc - 8;
    if (pic->p.layout != DAV1D_PIXEL_LAYOUT_I420) {
        static volatile int32_t logged;
        if (!plat_xchg32(&logged, 1)) rt_log("movie: picture layout %d is not 4:2:0: skipped", pic->p.layout);
        return 0;
    }
    {
        static volatile int32_t logged;
        if (!plat_xchg32(&logged, 1) && pic->seq_hdr)
            rt_log("movie: stream %dx%d, %d-bit, colour range %s, matrix %d, primaries %d, transfer %d", w, h,
                   pic->p.bpc, pic->seq_hdr->color_range ? "full" : "limited", pic->seq_hdr->mtrx, pic->seq_hdr->pri,
                   pic->seq_hdr->trc);
    }
    // The host overlay (NX2NAVI_MOVIE_OVERLAY=1) takes an RGBA frame; without it the game converts the NV12 frame itself.
    static int overlay = -1;
    if (overlay < 0) {
        const char *e = cfg_get("NX2NAVI_MOVIE_OVERLAY");
        overlay = e && strcmp(e, "0");
    }
    uint8_t *o = malloc((size_t)w * h * 3 / 2), *rgba = overlay ? malloc((size_t)w * h * 4) : NULL;
    if (!o || (overlay && !rgba)) {   // a frame the PC has no memory for is dropped, not the game
        free(o), free(rgba);
        movie_oom_log(w, h);
        return 0;
    }
    const ptrdiff_t ys = pic->stride[0], cs = pic->stride[1];
    static int deband_on = -1;   // NX2NAVI_MOVIE_DEBAND=0 keeps the decoded planes
    if (deband_on < 0) {
        const char *e = cfg_get("NX2NAVI_MOVIE_DEBAND");
        deband_on = !(e && !strcmp(e, "0"));
    }
    if (deband_on && !hbd && !rgba) {
        // The usual case (8-bit, debanded, the game converts the frame): one pass per stripe debands the decoder's own
        // planes and writes the dithered NV12 rows. Reading the decoder's planes instead of an NV12 copy of them gives
        // the same samples, and nothing reads the frame while it is written, so each stripe finishes its rows alone:
        // one round of threads per frame instead of two, and no single-threaded copy before it.
        uint16_t *Y16 = malloc((size_t)w * h * 2), *U16 = malloc((size_t)w * h / 2), *V16 = malloc((size_t)w * h / 2);
        if (!Y16 || !U16 || !V16) rt_fatal("movie: out of memory for a %dx%d frame", w, h);
        Stripe base = {2, 0, 0, w, h, o, Y16, U16, V16, NULL, pic->data[0], pic->data[1], pic->data[2], ys, cs};
        run_stripes(base);
        free(Y16), free(U16), free(V16);
        *p = (Picture){o, NULL, w, h, pic->m.timestamp, 0};
        return 1;
    }
    for (int y = 0; y < h; y++) {
        const uint8_t *row = (const uint8_t *)pic->data[0] + y * ys;
        if (!hbd) memcpy(o + (size_t)y * w, row, (size_t)w);
        else for (int x = 0; x < w; x++) o[(size_t)y * w + x] = (uint8_t)(((const uint16_t *)row)[x] >> sh);
    }
    uint8_t *uv = o + (size_t)w * h;
    for (int y = 0; y < h / 2; y++) {
        const uint8_t *u = (const uint8_t *)pic->data[1] + y * cs, *vv = (const uint8_t *)pic->data[2] + y * cs;
        for (int x = 0; x < w / 2; x++) {
            uv[(size_t)y * w + 2 * x] = hbd ? (uint8_t)(((const uint16_t *)u)[x] >> sh) : u[x];
            uv[(size_t)y * w + 2 * x + 1] = hbd ? (uint8_t)(((const uint16_t *)vv)[x] >> sh) : vv[x];
        }
    }
    // debanded planes (NX2NAVI_MOVIE_DEBAND=0 keeps the decoded ones), then the overlay's RGBA or the dithered NV12
    {
        uint16_t *Y16 = malloc((size_t)w * h * 2), *U16 = malloc((size_t)w * h / 2), *V16 = malloc((size_t)w * h / 2);
        if (!Y16 || !U16 || !V16) {
            free(Y16), free(U16), free(V16), free(o), free(rgba);
            movie_oom_log(w, h);
            return 0;
        }
        Stripe base = {0, 0, 0, w, h, o, Y16, U16, V16, rgba};
        if (deband_on) {
            run_stripes(base);
        } else if (!rgba) {
            free(Y16), free(U16), free(V16);
            *p = (Picture){o, rgba, w, h, pic->m.timestamp, 0};
            return 1;
        } else {
            for (size_t i = 0; i < (size_t)w * h; i++) Y16[i] = (uint16_t)(o[i] * 16);
            for (size_t i = 0; i < (size_t)w * h / 4; i++) U16[i] = (uint16_t)(uv[2 * i] * 16), V16[i] = (uint16_t)(uv[2 * i + 1] * 16);
        }
        base.phase = 1;
        run_stripes(base);
        free(Y16), free(U16), free(V16);
    }
    *p = (Picture){o, rgba, w, h, pic->m.timestamp, 0};
    return 1;
}

// Queues a decoded picture, waiting while the queue is full (the game acquires at the movie's pace); decoder thread
static void deliver(Video *v, Picture p, uint64_t gen) {
    plat_mutex_lock(&v->lock);
    while (v->qn == QMAX && !v->quit && v->generation == gen) {
        plat_mutex_unlock(&v->lock);
        plat_sleep_ms(2);
        plat_mutex_lock(&v->lock);
    }
    if (v->quit || v->generation != gen) {
        plat_mutex_unlock(&v->lock);
        picture_free(&p);
        return;
    }
    v->q[(v->qh + v->qn) % QMAX] = p;
    v->qn++;
    v->decoded += !p.eos;
    update_events(v);
    plat_mutex_unlock(&v->lock);
}

// Queues a decoded picture (or the end-of-stream marker) for the convert thread, waiting while it has CQMAX;
// decoder thread. On quit the picture is released here.
static void hand_over(Video *v, Decoded d) {
    plat_mutex_lock(&v->lock);
    while (v->cqn == CQMAX && !v->quit) plat_cond_wait(&v->conv, &v->lock);
    if (v->quit) {
        plat_mutex_unlock(&v->lock);
        if (!d.eos) dav1d_picture_unref(&d.pic);
        return;
    }
    v->cq[(v->cqh + v->cqn) % CQMAX] = d;
    v->cqn++;
    plat_cond_broadcast(&v->conv);
    plat_mutex_unlock(&v->lock);
}
// Releases the pictures still waiting for the convert thread; under v->lock
static void clear_converts(Video *v) {
    while (v->cqn) {
        Decoded *d = &v->cq[v->cqh];
        if (!d->eos) dav1d_picture_unref(&d->pic);
        v->cqh = (v->cqh + 1) % CQMAX;
        v->cqn--;
    }
    v->cqh = 0;
    plat_cond_broadcast(&v->conv);
}
static void convert_thread(void *arg) {
    Video *v = arg;
    plat_mutex_lock(&v->lock);
    for (;;) {
        while (!v->cqn && !v->quit) plat_cond_wait(&v->conv, &v->lock);
        if (v->quit) break;
        Decoded d = v->cq[v->cqh];
        v->cqh = (v->cqh + 1) % CQMAX;
        v->cqn--;
        plat_cond_broadcast(&v->conv);
        plat_mutex_unlock(&v->lock);
        if (d.eos) {
            deliver(v, (Picture){NULL, NULL, 0, 0, d.ts, 1}, d.gen);
        } else {
            uint64_t t0 = plat_now_ns();
            Picture p;
            int ok = make_picture(&d.pic, &p);
            t_make_ns += plat_now_ns() - t0;
            n_made++;
            dav1d_picture_unref(&d.pic);
            if (ok) deliver(v, p, d.gen);
        }
        plat_mutex_lock(&v->lock);
    }
    plat_mutex_unlock(&v->lock);
}

static void drain(Video *v, uint64_t gen) {
    Dav1dPicture pic = {0};
    for (;;) {
        uint64_t t0 = plat_now_ns();
        int got = dav1d_get_picture(v->dav1d, &pic) == 0;
        uint64_t t1 = plat_now_ns();
        t_get_ns += t1 - t0;
        if (!got) break;
        hand_over(v, (Decoded){pic, gen, pic.m.timestamp, 0});
        pic = (Dav1dPicture){0};
    }
}

static void decoder_thread(void *arg) {
    Video *v = arg;
    plat_mutex_lock(&v->lock);
    for (;;) {
        uint64_t tw = plat_now_ns();
        while (!v->in_n && !v->quit) plat_cond_wait(&v->work, &v->lock);
        t_wait_ns += plat_now_ns() - tw;
        if (v->quit) break;
        Sample s = v->in[v->ih];
        v->ih = (v->ih + 1) % INMAX;
        v->in_n--;
        uint64_t gen = v->generation;
        update_events(v);
        plat_mutex_unlock(&v->lock);
        plat_mutex_lock(&v->codec);
        // a flush (ov_flush) between taking the sample and the codec: the sample belongs to the stream before it, and fed
        // into the flushed decoder its picture would come out after the seek
        plat_mutex_lock(&v->lock);
        int stale = v->generation != gen;
        plat_mutex_unlock(&v->lock);
        if (stale) s.size = 0, s.eos = 0;
        if (v->dav1d && s.data && s.size) {
            Dav1dData d = {0};
            uint8_t *p = dav1d_data_create(&d, s.size);
            if (p) {
                memcpy(p, s.data, s.size);
                d.m.timestamp = s.ts;
                while (d.sz) {
                    uint64_t ts0 = plat_now_ns();
                    int r = dav1d_send_data(v->dav1d, &d);
                    t_send_ns += plat_now_ns() - ts0;
                    if (r == DAV1D_ERR(EAGAIN)) drain(v, gen);
                    else if (r < 0) {
                        static volatile int32_t logged;
                        if (!plat_xchg32(&logged, 1)) rt_log("movie: dav1d rejected a sample (%d)", r);
                        dav1d_data_unref(&d);
                        break;
                    }
                }
                drain(v, gen);
            }
        }
        if (s.eos && v->dav1d) {
            drain(v, gen);
            hand_over(v, (Decoded){.gen = gen, .ts = s.ts, .eos = 1});   // after the last picture, through the same queue
        }
        plat_mutex_unlock(&v->codec);
        free(s.data);
        plat_mutex_lock(&v->lock);
    }
    plat_mutex_unlock(&v->lock);
}

static void stop_thread(Video *v) {
    if (!v->thread && !v->convert) return;
    plat_mutex_lock(&v->lock);
    v->quit = 1;
    plat_cond_broadcast(&v->work);
    plat_cond_broadcast(&v->conv);
    plat_mutex_unlock(&v->lock);
    if (v->thread) plat_thread_join(v->thread);
    if (v->convert) plat_thread_join(v->convert);
    v->thread = v->convert = NULL;
    plat_mutex_lock(&v->lock);
    clear_converts(v);   // before dav1d_close: the pictures are the decoder's
    v->quit = 0;
    plat_mutex_unlock(&v->lock);
}

static void finalize(Video *v) {
    stop_thread(v);
    plat_mutex_lock(&v->codec);
    if (v->dav1d) dav1d_close(&v->dav1d);
    plat_mutex_unlock(&v->codec);
    plat_mutex_lock(&v->lock);
    clear_queues(v);
    memset(v->busy, 0, sizeof v->busy);
    plat_mutex_unlock(&v->lock);
}

// ---- movie::VideoDecoder
// VideoDecoder() / ~VideoDecoder(): the object's first 8 bytes hold the host record
static void ov_ctor(Cpu *c) { *(Video **)arg_p(c, 0) = NULL; }
static void ov_dtor(Cpu *c) {
    Video **slot = arg_p(c, 0);
    if (!slot || !*slot) return;
    finalize(*slot);
    set_live(*slot, 0);
    free(*slot);   // the bound events' kernel handles stay, as for any guest event never destroyed
    *slot = NULL;
    gfx_movie_stop();
}
void vk_dump_movie_frame(int k, uint64_t shown);   // vk_dump.c (NX2NAVI_DUMP_MOVIE)
static int movies_started;                          // decoders that came up this run (Video.run_index)
// Result Initialize(void *memory, size_t size, MediaMimeType mime, VideoDecoderOutputFormat format, VideoCodecMode)
static void ov_initialize(Cpu *c) {
    Video **slot = arg_p(c, 0);
    if (!*slot) {
        *slot = calloc(1, sizeof **slot);
        if (!*slot) rt_fatal("movie: decoder allocation");
        plat_mutex_init(&(*slot)->lock);
        plat_mutex_init(&(*slot)->codec);
        plat_cond_init(&(*slot)->work);
        plat_cond_init(&(*slot)->conv);
    }
    Video *v = *slot;
    finalize(v);
    set_live(v, 1);
    v->clock_on = v->rel_on = 0;
    Dav1dSettings s;
    dav1d_default_settings(&s);
    // Frame threading on (dav1d picks the delay): heavy stretches of the title movie took longer than a frame to
    // decode one picture at a time (max_frame_delay 1) and the game's loop waited up to 94 ms for them. The delay
    // costs nothing here, as decoding runs ahead on its own thread; end of stream drains the delayed pictures.
    s.n_threads = 8;
    s.max_frame_delay = 0;
    int r = dav1d_open(&v->dav1d, &s);
    if (r < 0) rt_fatal("movie: dav1d_open failed (%d)", r);
    v->thread = plat_thread_new(decoder_thread, v, "movie decoder");
    if (!v->thread) rt_fatal("movie: cannot start the decoder thread");
    v->convert = plat_thread_new(convert_thread, v, "movie convert");
    if (!v->convert) rt_fatal("movie: cannot start the convert thread");
    v->run_index = __atomic_add_fetch(&movies_started, 1, __ATOMIC_RELAXED);
    rt_log("movie: video decoder %d up (AV1 via dav1d %s on its own thread; mime %u, output format %u, mode %u)",
           v->run_index, dav1d_version(), (unsigned)arg_x(c, 3), (unsigned)arg_x(c, 4), (unsigned)arg_x(c, 5));
    ret_x(c, 0);
}
// Result Configure(const MediaProperty *, const void *configuration, size_t): the AV1 sequence header also comes
// in band with the first key frame, which dav1d reads
static void ov_ok(Cpu *c) { ret_x(c, 0); }
// Result Finalize()
static void ov_finalize(Cpu *c) {
    Video *v = video_of(arg_p(c, 0));
    if (v) {
        rt_log("movie: video decoder finalised (%llu frames decoded, %llu shown)", (unsigned long long)v->decoded,
               (unsigned long long)v->shown);
        finalize(v);
        set_live(v, 0);
    }
    gfx_movie_stop();
    ret_x(c, 0);
}
// Result RegisterVideoFrame(VideoFrame **frames, int count)
static void ov_register(Cpu *c) {
    Video *v = video_of(arg_p(c, 0));
    uint64_t *frames = arg_p(c, 1);
    int n = (int)arg_x(c, 2);
    if (!v || n < 0 || n > NFRAMES || (n && !frames)) rt_fatal("movie: RegisterVideoFrame of %d frames", n);
    plat_mutex_lock(&v->lock);
    memcpy(v->frame, frames, (size_t)n * sizeof *frames);
    memset(v->busy, 0, sizeof v->busy);
    v->nframe = n;
    // Each frame buffer starts as NV12 black (Y 16, UV 128 in its last third). The game converts a frame it holds
    // before a picture arrives; zeros there are YUV (0, 0, 0), green, in stripes where rows were partly written.
    for (int i = 0; i < n; i++) {
        uint8_t *fr = (uint8_t *)(uintptr_t)frames[i], *buf;
        uint64_t cap;
        if (!fr) continue;
        memcpy(&buf, fr + 0x08, 8), memcpy(&cap, fr + 0x48, 8);
        if (!buf || cap < 3 || cap > (64u << 20)) continue;
        uint64_t ysz = cap / 3 * 2;
        memset(buf, 16, (size_t)ysz);
        memset(buf + ysz, 128, (size_t)(cap - ysz));
    }
    plat_mutex_unlock(&v->lock);
    ret_x(c, 0);
}
// Result GetInputReadyEvent(SystemEventType **out) / GetOutputReadyEvent(SystemEventType **out)
static void get_event(Cpu *c, int input) {
    Video *v = video_of(arg_p(c, 0));
    uint64_t *out = arg_p(c, 1);
    if (!v || !out) rt_fatal("movie: Get%sReadyEvent before Initialize", input ? "Input" : "Output");
    uint8_t *ev = input ? v->in_ev : v->out_ev;
    int *bound = input ? &v->in_bound : &v->out_bound;
    if (!*bound) {
        kern_bind_system_event(c, (uint64_t)(uintptr_t)ev, 0);
        plat_mutex_lock(&v->lock);
        *bound = 1;
        plat_mutex_unlock(&v->lock);
    }
    plat_mutex_lock(&v->lock);
    update_events(v);
    plat_mutex_unlock(&v->lock);
    *out = (uint64_t)(uintptr_t)ev;
    ret_x(c, 0);
}
static void ov_input_event(Cpu *c) { get_event(c, 1); }
static void ov_output_event(Cpu *c) { get_event(c, 0); }
// Result Flush(): samples and pictures not yet acquired are dropped (a seek or a restart follows)
static void ov_flush(Cpu *c) {
    Video *v = video_of(arg_p(c, 0));
    if (v) {
        plat_mutex_lock(&v->lock);
        v->generation++;   // the decoder and convert threads drop what they are making
        v->clock_on = v->rel_on = 0;   // a seek: the report's real-time reference starts again from the next picture
        clear_queues(v);
        clear_converts(v);
        plat_mutex_unlock(&v->lock);
        plat_mutex_lock(&v->codec);
        if (v->dav1d) dav1d_flush(v->dav1d);
        plat_mutex_unlock(&v->codec);
        plat_mutex_lock(&v->lock);
        update_events(v);
        plat_mutex_unlock(&v->lock);
    }
    ret_x(c, 0);
}
// Result DecodeAsync(const nn::movie::Buffer *sample): queued for the decoder thread
static void ov_decode(Cpu *c) {
    Video *v = video_of(arg_p(c, 0));
    const uint8_t *b = arg_p(c, 1);
    if (!v || !v->dav1d || !b) {
        ret_x(c, 0);
        return;
    }
    const uint8_t *data;
    uint64_t size;
    int64_t ts;
    uint32_t flags;
    memcpy(&data, b, 8), memcpy(&size, b + 0x10, 8), memcpy(&ts, b + 0x18, 8), memcpy(&flags, b + 0x20, 4);
    int eos = (flags & BUFFER_FLAG_EOS) != 0;
    static int64_t skip_ns = -2;   // NX2NAVI_MOVIE_SKIP: -1 off
    if (skip_ns == -2) {
        const char *e = cfg_get("NX2NAVI_MOVIE_SKIP");
        skip_ns = e && *e ? (int64_t)(atof(e) * 1e9) : -1;
        if (skip_ns >= 0) rt_log("movie: movies end after %.1f s (NX2NAVI_MOVIE_SKIP)", skip_ns / 1e9);
    }
    plat_mutex_lock(&v->lock);
    if (skip_ns >= 0 && v->eos_queued) {   // after the early end: what the demuxer still sends is dropped
        plat_mutex_unlock(&v->lock);
        ret_x(c, 0);
        return;
    }
    if (skip_ns >= 0 && !eos && ts >= skip_ns) eos = 1, size = 0;   // the early end
    if (v->in_n >= INMAX || ((v->qn >= QREADY || v->in_n >= QREADY) && !eos)) {
        update_events(v);
        plat_mutex_unlock(&v->lock);
        ret_x(c, R_BUSY);
        return;
    }
    Sample s = {NULL, 0, ts, eos};
    if (data && size) {
        s.data = malloc((size_t)size);
        if (!s.data) {   // no memory for the sample now: the game is told to try again, as with a full queue
            static volatile int32_t logged;
            if (plat_xchg32(&logged, 1) == 0)
                rt_log("movie: out of memory for a %llu-byte sample: retried (later ones not logged)", (unsigned long long)size);
            update_events(v);
            plat_mutex_unlock(&v->lock);
            ret_x(c, R_BUSY);
            return;
        }
        memcpy(s.data, data, (size_t)size);
        s.size = (size_t)size;
    }
    v->in[(v->ih + v->in_n) % INMAX] = s;
    v->in_n++;
    if (!eos && !v->eos_queued) set_live(v, 1);   // playing again after its end (a Flush clears eos_queued)
    if (eos) v->eos_queued = 1;
    update_events(v);
    plat_cond_signal(&v->work);
    plat_mutex_unlock(&v->lock);
    ret_x(c, 0);
}
// Result AcquireVideoFrame(VideoFrame **out): the oldest decoded picture in a free registered frame
static void ov_acquire(Cpu *c) {
    Video *v = video_of(arg_p(c, 0));
    uint64_t *out = arg_p(c, 1);
    if (!v || !out) {
        ret_x(c, R_NO_FRAME);
        return;
    }
    plat_mutex_lock(&v->lock);
    int f = 0;
    while (f < v->nframe && v->busy[f]) f++;
    if (!v->qn || f == v->nframe) {
        plat_mutex_unlock(&v->lock);
        ret_x(c, R_NO_FRAME);
        return;
    }
    Picture p = v->q[v->qh];
    v->qh = (v->qh + 1) % QMAX;
    v->qn--;
    uint8_t *fr = (uint8_t *)(uintptr_t)v->frame[f];
    uint8_t *buf;
    uint64_t cap;
    memcpy(&buf, fr + 0x08, 8), memcpy(&cap, fr + 0x48, 8);
    uint64_t ysz = (uint64_t)p.w * p.h, uvsz = ysz / 2;
    if (p.nv12 && buf && cap >= ysz + uvsz) {
        memcpy(buf, p.nv12, (size_t)(ysz + uvsz));
        // The game's YUV pass reads this buffer on the GPU. A host write into pool storage is one the page write
        // tracking may not see in time (a mirror or texture over it kept its older or still-empty bytes: frames
        // with stripes of zeros, which NV12 shows as green), so the back end is told directly.
        nvn_guest_written((uint64_t)(uintptr_t)buf, ysz + uvsz);
        uint64_t uvp = (uint64_t)(uintptr_t)(buf + ysz);
        memcpy(fr + 0x10, &uvp, 8);
        memcpy(fr + 0x28, &ysz, 8), memcpy(fr + 0x30, &uvsz, 8);
    } else {
        uint64_t zero = 0;
        memcpy(fr + 0x28, &zero, 8), memcpy(fr + 0x30, &zero, 8);
        if (p.nv12) {
            static volatile int32_t logged;
            if (!plat_xchg32(&logged, 1))
                rt_log("movie: frame buffer %#llx bytes is smaller than a %dx%d NV12 frame", (unsigned long long)cap,
                       p.w, p.h);
        }
    }
    memcpy(fr + 0x78, &p.ts, 8);
    fr[0x88] = (uint8_t)p.eos;
    v->busy[f] = 1;
    v->shown += !p.eos;
    if (p.nv12 && v->shown == 1) rt_log("movie: first frame %dx%d shown", p.w, p.h);
    if (p.nv12) vk_dump_movie_frame(v->run_index, v->shown);
    if (p.nv12 && !v->clock_on) v->ts0 = p.ts, v->shown0_ns = plat_now_ns(), v->clock_on = 1;
    if (p.nv12) {   // pacing report every 600 frames (10 s): the gaps between acquisitions and their time stamps
        static uint64_t last_ns, sum_gap, max_gap, n;
        uint64_t now = plat_now_ns(), gap = last_ns ? now - last_ns : 0;
        // how far the picture is behind the time since the first one (positive: the picture is late)
        int64_t lag = (int64_t)(now - v->shown0_ns) - (p.ts - v->ts0);
        sum_gap += gap, n++;
        if (gap > max_gap) max_gap = gap;
        if (n % 600 == 0) {
            rt_log("movie: pacing over 600 frames: acquire gap avg %.2f ms, max %.2f ms; %d pictures waiting; "
                   "per frame shown: decoder thread waiting for samples %.2f ms, send %.2f ms, get %.2f ms; convert thread "
                   "%.2f ms per picture (%llu pictures); "
                   "picture %.0f ms behind real time since the first picture; on screen %.0f ms behind, %+.0f ms since its start",
                   sum_gap / 600 / 1e6, max_gap / 1e6, v->qn, t_wait_ns / 600 / 1e6, t_send_ns / 600 / 1e6,
                   t_get_ns / 600 / 1e6, n_made ? t_make_ns / n_made / 1e6 : 0.0, (unsigned long long)n_made, lag / 1e6, v->rel_lag / 1e6,
                   (v->rel_lag - v->rel_lag0) / 1e6);
            t_wait_ns = t_send_ns = t_get_ns = t_make_ns = n_made = 0;
            sum_gap = max_gap = 0;
        }
        last_ns = now;
    }
    update_events(v);
    plat_mutex_unlock(&v->lock);
    if (p.rgba) gfx_movie_frame_own(p.rgba, p.w, p.h), p.rgba = NULL;   // the overlay takes the buffer
    if (p.eos) {   // how the movie kept time: the picture on screen against the clock, at its start and its end
        if (v->rel_on)
            rt_log("movie: end of stream after %llu pictures: on screen %.0f ms behind real time, %+.0f ms since its start",
                   (unsigned long long)v->shown, v->rel_lag / 1e6, (v->rel_lag - v->rel_lag0) / 1e6);
        gfx_movie_stop(), set_live(v, 0);
    }
    picture_free(&p);
    *out = v->frame[f];
    ret_x(c, 0);
}
// Result ReleaseVideoFrame(VideoFrame *)
static void ov_release(Cpu *c) {
    Video *v = video_of(arg_p(c, 0));
    uint64_t fr = arg_x(c, 1);
    if (v) {
        plat_mutex_lock(&v->lock);
        for (int i = 0; i < v->nframe; i++)
            if (v->frame[i] == fr) {
                v->busy[i] = 0;
                int64_t ts;
                memcpy(&ts, (uint8_t *)(uintptr_t)fr + 0x78, 8);
                if (v->clock_on && !((uint8_t *)(uintptr_t)fr)[0x88]) {
                    v->rel_lag = (int64_t)(plat_now_ns() - v->shown0_ns) - (ts - v->ts0);
                    if (!v->rel_on) v->rel_lag0 = v->rel_lag, v->rel_on = 1;
                }
            }
        update_events(v);
        plat_mutex_unlock(&v->lock);
    }
    ret_x(c, 0);
}

// Skipping movies (NX2NAVI_MOVIES=off) leaves the subsdk0 decoder in place: the movie never opens, so it is not called.
#define S(n) "_ZN5movie12VideoDecoder" n
HOST_OVERRIDE(S("C1Ev"), ov_ctor);
HOST_OVERRIDE(S("D1Ev"), ov_dtor);
HOST_OVERRIDE(S("10InitializeEPvmN2nn5movie13MediaMimeTypeENS_24VideoDecoderOutputFormatENS_14VideoCodecModeE"),
              ov_initialize);
HOST_OVERRIDE(S("9ConfigureEPKN2nn5movie13MediaPropertyEPKvm"), ov_ok);
HOST_OVERRIDE(S("8FinalizeEv"), ov_finalize);
HOST_OVERRIDE(S("18RegisterVideoFrameEPPNS_10VideoFrameEi"), ov_register);
HOST_OVERRIDE(S("18GetInputReadyEventEPPN2nn2os15SystemEventTypeE"), ov_input_event);
HOST_OVERRIDE(S("19GetOutputReadyEventEPPN2nn2os15SystemEventTypeE"), ov_output_event);
HOST_OVERRIDE(S("5StartEv"), ov_ok);
HOST_OVERRIDE(S("4StopEv"), ov_ok);
HOST_OVERRIDE(S("5FlushEv"), ov_flush);
HOST_OVERRIDE(S("11DecodeAsyncEPKN2nn5movie6BufferE"), ov_decode);
HOST_OVERRIDE(S("17AcquireVideoFrameEPPNS_10VideoFrameE"), ov_acquire);
HOST_OVERRIDE(S("17ReleaseVideoFrameEPNS_10VideoFrameE"), ov_release);
#undef S
