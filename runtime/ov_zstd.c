// The game's zstd decoder on the host's libzstd. Most RomFS files of NX2navi 1.4.3 are zstd frames, many with a
// dictionary from Pack/ZsDic.pack.zs; the decoder is BSD zstd linked into main. The patch set "zstd"
// (recomp/patches-v143.txt) sends every call of ZSTD_decompressMultiFrame (main+0x3a1f0) and of ZSTD_decompress_usingDDict
// (main+0x3a1e0) to the two host functions below, on the patch page. Lifted, the decoder ran its literal and match
// copies as chains of small functions with the registers in Cpu; here the same frames decode in native code.
//
// - Results match the guest's: libzstd decodes the same format, and every result that is an error goes to the guest
//   decoder again (it writes its own error code; a frame libzstd rejects may still be one the game's version accepts).
// - A guest ZSTD_DDict (zstd's struct: dictBuffer, dictContent, dictSize first) becomes a host ZSTD_DDict made from a
//   copy of its content, cached by guest address, content address, size and a fingerprint of the content.
// - Inputs of at least AWAY_BYTES give the virtual core up while they decode (other guest threads run on it).
// NX2NAVI_ZSTD_NATIVE=0 keeps the guest decoder (the patch set switch); NX2NAVI_ZSTD_CHECK=1 decodes each call both ways,
// keeps the guest's result and logs every call where the two differ.
//
// Ocarina of Time 1.1.1 has the same decoder at other addresses (recomp/patches-v111.txt: three calls of
// ZSTD_decompressMultiFrame, main+0xeb380). Its loading threads decode most data through the game's stream feed
// (main+0xf5c80): host_stream, further down. The host decodes there by default since a check run (zstd_check = 1) over a
// village and a field: 3,072 streams, 31.7 GiB of output, compared byte for byte with the game decoder's, none
// different. Until then an unset key sent every call on to the game's decoder, which took about a fifth of the
// running time of the game's threads in a village and a second of each area load. zstd_native = 0 keeps the game's.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <zstd_errors.h>

#include "cfg.h"
#include "host.h"

#if __has_include("patches.h")
#include "patches.h"
#include "platform/platform.h"
#endif

#ifdef RT_PATCH_PAGE

#define GUEST_MULTIFRAME (rt_game_version == 111 ? 0x70000eb380ull : 0x700003a1f0ull)   // ZSTD_decompressMultiFrame: 1.1.1, 1.4.3
#define GUEST_USING_DDICT 0x700003a1e0ull  // 1.4.3 main: ZSTD_decompress_usingDDict (1.1.1 has no site for it)
#define HOST_MULTIFRAME (RT_PATCH_PAGE + 0xff0)
#define HOST_USING_DDICT (RT_PATCH_PAGE + 0xfe0)
#define AWAY_BYTES (64u << 10)
#define MAX_DICT (64u << 20)
#define NDICT 32

static void *gp(uint64_t a) { return (void *)(uintptr_t)a; }

typedef struct {
    uint64_t guest, content, size, print;
    ZSTD_DDict *dd;
} Dict;
static Dict dicts[NDICT];
static int ndicts;
static PlatMutex dict_lock = PLAT_MUTEX_INIT;

static struct {
    uint64_t calls, in, out, ns, guest_ns, fallbacks, away, inplace;
} S;

static int check_mode(void) {
    static int v = -1;
    if (v < 0) v = cfg_bool("NX2NAVI_ZSTD_CHECK");
    return v;
}

// Size, both ends and a sample of the middle: what tells a new dictionary at a reused address from the old one.
static uint64_t fingerprint(const uint8_t *p, uint64_t n) {
    uint64_t h = n * 0x9E3779B97F4A7C15ull;
    uint64_t at[3] = {0, n / 2, n > 64 ? n - 64 : 0};
    for (int k = 0; k < 3; k++)
        for (uint64_t i = at[k]; i < at[k] + 64 && i < n; i += 8) {
            uint64_t v = 0;
            memcpy(&v, p + i, n - i < 8 ? n - i : 8);
            h = (h ^ v) * 0xC2B2AE3D27D4EB4Full;
            h ^= h >> 29;
        }
    return h;
}

// The host dictionary for a guest ZSTD_DDict or raw dictionary bytes (guest == 0); NULL when its fields look wrong.
static ZSTD_DDict *dict_for(uint64_t guest, uint64_t content, uint64_t size) {
    if (!content || !size || size > MAX_DICT || content >= RT_ADDRESS_LIMIT || content + size > RT_ADDRESS_LIMIT)
        return NULL;
    uint64_t print = fingerprint(gp(content), size);
    ZSTD_DDict *dd = NULL;
    plat_mutex_lock_shared(&dict_lock);
    for (int i = 0; i < ndicts && !dd; i++)
        if (dicts[i].guest == guest && dicts[i].content == content && dicts[i].size == size && dicts[i].print == print)
            dd = dicts[i].dd;
    plat_mutex_unlock_shared(&dict_lock);
    if (dd) return dd;
    // A copy: the guest may free or reuse its buffer. Entries are never freed (another thread may be decoding with
    // one); a full table sends further dictionaries to the guest decoder.
    ZSTD_DDict *made = ZSTD_createDDict(gp(content), (size_t)size);
    if (!made) return NULL;
    int slot = -1;
    plat_mutex_lock(&dict_lock);
    for (int i = 0; i < ndicts && !dd; i++)   // another thread may have made it meanwhile
        if (dicts[i].guest == guest && dicts[i].content == content && dicts[i].size == size && dicts[i].print == print)
            dd = dicts[i].dd;
    if (!dd && ndicts < NDICT) slot = ndicts, dicts[ndicts++] = (Dict){guest, content, size, print, made}, dd = made;
    plat_mutex_unlock(&dict_lock);
    if (dd != made) {
        ZSTD_freeDDict(made);
        if (!dd) rt_log("zstd: more than %d dictionaries: further ones decode in the guest", NDICT);
        return dd;
    }
    rt_log("zstd: host dictionary %d for %s %#llx: %llu bytes at %#llx, id %u", slot, guest ? "DDict" : "raw dict",
           (unsigned long long)(guest ? guest : content), (unsigned long long)size, (unsigned long long)content,
           ZSTD_getDictID_fromDDict(dd));
    return dd;
}

// A thread's one-shot decoder context (host_decode), freed when the thread ends (plat_thread_end_hook): the game
// makes and ends threads all through play, and each that had decoded left its context behind, about 100 KiB.
static __thread ZSTD_DCtx *dctx;
static volatile int32_t dctx_hooked;

static void dctx_thread_end(void) {
    if (dctx) ZSTD_freeDCtx(dctx);
    dctx = NULL;
}

// Decodes on this thread's host context. Returns libzstd's result; *ok 0 when the inputs cannot be taken.
static size_t host_decode(uint64_t dst, uint64_t cap, uint64_t src, uint64_t size, uint64_t dict, uint64_t dict_size,
                          uint64_t ddict, int *ok) {
    *ok = 0;
    if ((cap && !dst) || !src || src + size > RT_ADDRESS_LIMIT) return 0;   // dst: decode() checks a guest one
    ZSTD_DDict *dd = NULL;
    if (ddict) {
        if (ddict >= RT_ADDRESS_LIMIT - 24) return 0;
        uint64_t f[3];   // ZSTD_DDict: dictBuffer, dictContent, dictSize
        memcpy(f, gp(ddict), sizeof f);
        if (!(dd = dict_for(ddict, f[1], f[2]))) {
            static int logged;
            if (logged < 4) {
                uint64_t w[8];
                memcpy(w, gp(ddict), sizeof w);
                logged++;
                rt_log("zstd: DDict %#llx not taken: %llx %llx %llx %llx %llx %llx %llx %llx", (unsigned long long)ddict,
                       (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3],
                       (unsigned long long)w[4], (unsigned long long)w[5], (unsigned long long)w[6], (unsigned long long)w[7]);
            }
            return 0;
        }
    } else if (dict && dict_size) {
        if (!(dd = dict_for(0, dict, dict_size))) return 0;
    }
    if (!dctx) {
        if (!plat_xchg32(&dctx_hooked, 1)) plat_thread_end_hook(dctx_thread_end);
        if (!(dctx = ZSTD_createDCtx())) return 0;
    }
    *ok = 1;
    return dd ? ZSTD_decompress_usingDDict(dctx, gp(dst), (size_t)cap, gp(src), (size_t)size, dd)
              : ZSTD_decompressDCtx(dctx, gp(dst), (size_t)cap, gp(src), (size_t)size);
}

// The host call inside the thread's host depth, as hostfn.c counts it for asynchronous preemption.
static size_t tracked_decode(Cpu *c, uint64_t dst, uint64_t cap, uint64_t src, uint64_t size, uint64_t dict,
                             uint64_t dict_size, uint64_t ddict, int *ok) {
    int *d = kern_host_track && c->thread ? kern_host_depth(c->thread) : NULL;
    if (d) __atomic_store_n(d, *d + 1, __ATOMIC_RELAXED);
    int away = size >= AWAY_BYTES ? kern_core_away() : 0;
    if (away) __atomic_add_fetch(&S.away, 1, __ATOMIC_RELAXED);
    uint64_t t0 = perf_mark_begin();
    size_t r = host_decode(dst, cap, src, size, dict, dict_size, ddict, ok);
    perf_mark_end(PERF_M_ZSTD, t0, *ok && !ZSTD_isError(r) ? r : 0);
    kern_core_back(&away);
    if (d) {
        int n = *d - 1;
        __atomic_store_n(d, n, __ATOMIC_RELAXED);
        if (!n && kern_preempt_pending(c->thread)) kern_host_left(c->thread);
    }
    return r;
}

static void count(uint64_t in, uint64_t out, uint64_t ns) {
    uint64_t n = __atomic_add_fetch(&S.calls, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&S.in, in, __ATOMIC_RELAXED);
    __atomic_add_fetch(&S.out, out, __ATOMIC_RELAXED);
    __atomic_add_fetch(&S.ns, ns, __ATOMIC_RELAXED);
    if (n == 1 || !(n & 0x3fff))
        rt_log("zstd: %llu calls on the host, %.1f MiB in, %.1f MiB out, %.0f ms (check mode: the guest decoder %.0f ms); "
               "%llu to the guest decoder, %llu in place in the guest, %llu with the core given up", (unsigned long long)n,
               S.in / 1048576.0, S.out / 1048576.0, S.ns / 1e6, S.guest_ns / 1e6, (unsigned long long)S.fallbacks,
               (unsigned long long)S.inplace, (unsigned long long)S.away);
}

// Whether this build's sites may decode on the host: unless zstd_native = 0 says the game's own decoder (the patch set's
// switch already leaves the sites alone then on the other builds; 1.1.1 reads the key here as well), or in the check.
static int trusted(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = cfg_get("NX2NAVI_ZSTD_NATIVE");
        v = !(e && !strcmp(e, "0")) || check_mode();
        if (!v) rt_log("zstd: the game's own decoder (zstd_native = 0; zstd_check = 1 compares the two)");
    }
    return v;
}

// x0 dctx, x1 dst, x2 capacity, x3 src, x4 size, then (dict, dict size, ddict) as the entry has them. guest: the
// guest function with the same arguments, n of them.
static void decode(Cpu *c, uint64_t dict, uint64_t dict_size, uint64_t ddict, uint64_t guest, int n) {
    uint64_t a[8];
    memcpy(a, c->x, sizeof a);
    uint64_t dst = a[1], cap = a[2], src = a[3], size = a[4];
    if (!trusted()) {
        rt_call(c, guest, n, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        return;
    }
    // In place (the input inside the output buffer): the result depends on how the decoder's writes overtake its
    // reads, which libzstd does not promise to do as the game's version does, and a host decode that failed half way
    // would leave the guest decoder a damaged input. Such calls stay in the guest.
    if (cap && src < dst + cap && dst < src + size) {
        __atomic_add_fetch(&S.inplace, 1, __ATOMIC_RELAXED);
        rt_call(c, guest, n, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        return;
    }
    if (check_mode()) {
        uint8_t *mine = cap ? malloc((size_t)cap) : NULL;
        int ok = 0;
        uint64_t t0 = kern_now_ns();
        size_t r = cap && !mine ? 0 : tracked_decode(c, (uint64_t)(uintptr_t)mine, cap, src, size, dict, dict_size, ddict, &ok);
        uint64_t t1 = kern_now_ns();
        uint64_t g = rt_call(c, guest, n, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        __atomic_add_fetch(&S.guest_ns, kern_now_ns() - t1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&S.ns, t1 - t0, __ATOMIC_RELAXED);
        static int logged;
        if (logged < 6 && (!ok || ZSTD_isError(r))) {
            logged++;
            rt_log("zstd check: host %s, guest %llu; dctx %#llx dst %#llx cap %llu src %#llx size %llu dict %#llx/%llu "
                   "ddict %#llx", !ok ? "inputs not taken" : ZSTD_getErrorName(r), (unsigned long long)g,
                   (unsigned long long)a[0], (unsigned long long)dst, (unsigned long long)cap, (unsigned long long)src,
                   (unsigned long long)size, (unsigned long long)dict, (unsigned long long)dict_size,
                   (unsigned long long)ddict);
        }
        if (ok && !ZSTD_isError(r) && (g != r || (r && memcmp(mine, gp(dst), r)))) {
            const uint8_t *p = gp(src);
            size_t fs = ZSTD_findFrameCompressedSize(p, (size_t)size);
            unsigned long long cs = ZSTD_getFrameContentSize(p, (size_t)size);
            ZSTD_DCtx *fresh = ZSTD_createDCtx();
            size_t r2 = ZSTD_decompressDCtx(fresh, mine, (size_t)cap, p, (size_t)size);
            ZSTD_freeDCtx(fresh);
            static int nlog;
            if (nlog++ < 8)
                rt_log("zstd check: MISMATCH %llu bytes from %#llx (cap %llu, first frame %lld bytes, content %lld, magic "
                       "%02x%02x%02x%02x, header %02x, dict %#llx/%llu, ddict %#llx, entry %s, lr %#llx): the host decoded "
                       "%llu bytes, the guest %lld%s; a fresh context without a dictionary: %s", (unsigned long long)size,
                       (unsigned long long)src, (unsigned long long)cap, ZSTD_isError(fs) ? -1ll : (long long)fs,
                       (long long)cs, p[3], p[2], p[1], p[0], p[4], (unsigned long long)dict,
                       (unsigned long long)dict_size, (unsigned long long)ddict, n == 8 ? "MultiFrame" : "usingDDict",
                       (unsigned long long)c->x[30], (unsigned long long)r, (long long)g, g == r ? " (other contents)" : "",
                       ZSTD_isError(r2) ? ZSTD_getErrorName(r2) : "decodes");
        }
        free(mine);
        count(size, ok && !ZSTD_isError(r) ? r : 0, 0);
        return;
    }
    uint64_t t0 = kern_now_ns();
    int ok = 0;
    size_t r = dst + cap > RT_ADDRESS_LIMIT ? 0 : tracked_decode(c, dst, cap, src, size, dict, dict_size, ddict, &ok);
    if (!ok || ZSTD_isError(r)) {   // the guest decoder decides (and writes its own error code)
        __atomic_add_fetch(&S.fallbacks, 1, __ATOMIC_RELAXED);
        static int logged;
        if (logged < 8) {
            logged++;
            rt_log("zstd: %llu bytes at %#llx go to the guest decoder: %s", (unsigned long long)size,
                   (unsigned long long)src, !ok ? "inputs not taken" : ZSTD_getErrorName(r));
        }
        rt_call(c, guest, n, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        return;
    }
    c->x[0] = r;
    count(size, r, kern_now_ns() - t0);
}

// ---- the stream feed of Ocarina of Time 1.1.1 (main+0xf5c80), which decodes most of what the loading threads read
// The game's decode job (main+0xf5bf0) hands each chunk it read to feed(S, src, int size, ddict) and looks only at the
// result: 0 the stream is done, bit 31 set an error, anything else go on. The feed drives zstd's bufferless decoder:
//   S+0x00 u8   phase: 1 begin, 2 running, 3 failed, 4 done
//   S+0x08 u64  where the next output goes        S+0x10 i32  output room left      S+0x14 u32  content left
//   S+0x18 u64  the DCtx workspace                S+0x20 u64, +0x28 i32, +0x2c i32  carry buffer, its size, its fill
//   S+0x30 u64  the DCtx (set at begin)
// Begin reads the frame header from the first chunk: a header that is not complete in it, a content size that is not
// in it, or a begin that fails gives phase 3 and 0xffffffff. Then each feed decodes what its input completes, moves
// +0x08, +0x10 and +0x14 by the bytes it wrote and returns +0x14; when the frame ends it sets phase 4. Input after the
// frame's end gives phase 3 and 0xfffffffd, a decode error phase 3 and 0xfffffffe (the output is too small) or
// 0xffffffff. A feed in phase 3 returns 0xffffffff and in phase 4 0xfffffffd.
// host_stream owns a stream from its begin or not at all: the guest DCtx never sees the blocks a host stream decoded,
// so there is no falling back half way. It takes a stream whose first chunk holds a whole header of a plain frame with
// its content size, whose output has room for all of that content and does not overlap the chunk, whose dictionary
// (if any) the host can take, and for which a slot is free; every other stream, and every feed of a stream it did not
// begin, goes to the game's feed. For its own streams it keeps the guest's fields and results as above, decoding with
// ZSTD_decompressStream straight into the game's output. That output is one buffer for the whole frame
// (ZSTD_d_stableOutBuffer): the decoder keeps no window of its own and reads earlier output where the game has it,
// and a feed that finds the output anywhere but where the last one left it fails the stream.
// The game's job stops feeding at a result of 0, which a frame gives once its content is out even when its last bytes
// (the checksum) are still to come: such a stream keeps its slot for those bytes but is marked spent, and a new stream
// takes a spent slot when no free one is left.
// With NX2NAVI_ZSTD_CHECK=1 the game's feed decodes every stream as before, the host decodes the same chunks into a
// buffer of its own, and the two outputs are compared when the game ends the stream or has all its content out
// ("zstd check: stream ..."). The check also reports a game output that did not advance through one buffer.
// --probe-zstd (probe_zstd, at the end) runs host streams on frames it makes itself.
#define GUEST_STREAM 0x70000f5c80ull
#define HOST_STREAM (RT_PATCH_PAGE + 0xf80)
#define NSTREAM 16

typedef struct {
    uint64_t s;                    // the guest stream state's address; 0: free
    ZSTD_DCtx *dctx;               // stays with the slot from stream to stream
    uint8_t *shadow;               // check mode: the host's output
    uint64_t content, pos;         // the frame's size; bytes the host wrote
    uint64_t dst0, cap0;           // the game's output at the begin: every feed writes on into this one buffer
    uint64_t in;                   // compressed bytes fed
    int spent;                     // all the content is out: the game feeds no more, and the slot may be taken
    int done, bad, moved;          // check mode: the host reached the frame's end / failed; the game's output moved
} Stream;
static Stream streams[NSTREAM];
static PlatMutex stream_lock = PLAT_MUTEX_INIT;
static struct { uint64_t streams, feeds, in, out, ns, guest, checked, differ; } SS;

static uint32_t ld32(uint64_t a) { uint32_t v; memcpy(&v, gp(a), 4); return v; }
static uint64_t ld64(uint64_t a) { uint64_t v; memcpy(&v, gp(a), 8); return v; }
static void st32(uint64_t a, uint32_t v) { memcpy(gp(a), &v, 4); }
static void st64(uint64_t a, uint64_t v) { memcpy(gp(a), &v, 8); }

static Stream *stream_find(uint64_t s) {
    Stream *st = NULL;
    plat_mutex_lock(&stream_lock);
    for (int i = 0; i < NSTREAM && !st; i++)
        if (streams[i].s == s) st = &streams[i];
    plat_mutex_unlock(&stream_lock);
    return st;
}

// The slot of s, or a free one for it, or one whose stream is spent; NULL when there is none (that stream decodes in
// the game).
static Stream *stream_take(uint64_t s) {
    Stream *st = NULL;
    plat_mutex_lock(&stream_lock);
    for (int i = 0; i < NSTREAM && !st; i++)
        if (streams[i].s == s) st = &streams[i];
    for (int i = 0; i < NSTREAM && !st; i++)
        if (!streams[i].s) st = &streams[i];
    for (int i = 0; i < NSTREAM && !st; i++)
        if (streams[i].spent) st = &streams[i];
    if (st) st->s = s, st->spent = 0;
    plat_mutex_unlock(&stream_lock);
    return st;
}

static void stream_drop(Stream *st) {
    if (!st) return;
    free(st->shadow);
    st->shadow = NULL;
    plat_mutex_lock(&stream_lock);
    st->s = 0, st->spent = 0;
    plat_mutex_unlock(&stream_lock);
}

// Drops the slot of stream state s, if it has one. Found and cleared under one hold of the lock: between a
// stream_find and a stream_drop another thread can take a spent slot for its own stream (stream_take), and the drop
// then cleared that thread's slot, which a third thread took as free and reset the decoder context in use.
static void stream_drop_of(uint64_t s) {
    plat_mutex_lock(&stream_lock);
    for (int i = 0; i < NSTREAM; i++)
        if (streams[i].s == s) {
            free(streams[i].shadow);
            streams[i].shadow = NULL;
            streams[i].s = 0, streams[i].spent = 0;
        }
    plat_mutex_unlock(&stream_lock);
}

static void stream_spent(Stream *st) {
    plat_mutex_lock(&stream_lock);
    st->spent = 1;
    plat_mutex_unlock(&stream_lock);
}

// A context for streams: the output is one buffer for the whole frame, so the decoder allocates no window (a window
// of its own would be as large as the frame's, and a frame may name one as large as its content), and it takes any
// window size, as the game's decoder does.
static ZSTD_DCtx *stream_dctx(void) {
    ZSTD_DCtx *d = ZSTD_createDCtx();
    if (d && (ZSTD_isError(ZSTD_DCtx_setParameter(d, ZSTD_d_stableOutBuffer, 1)) ||
              ZSTD_isError(ZSTD_DCtx_setParameter(d, ZSTD_d_windowLogMax, ZSTD_WINDOWLOG_MAX)))) {
        ZSTD_freeDCtx(d);
        d = NULL;
    }
    return d;
}

// Feeds in to the stream's context until the input is used up, the frame ends (0), an error, or nothing moves. Inside
// the thread's host depth and with the core given up for large chunks, as tracked_decode.
static size_t stream_run(Cpu *c, Stream *st, ZSTD_outBuffer *out, ZSTD_inBuffer *in) {
    int *d = kern_host_track && c->thread ? kern_host_depth(c->thread) : NULL;
    if (d) __atomic_store_n(d, *d + 1, __ATOMIC_RELAXED);
    int away = in->size >= AWAY_BYTES ? kern_core_away() : 0;
    if (away) __atomic_add_fetch(&S.away, 1, __ATOMIC_RELAXED);
    size_t r = 1, o_start = out->pos;
    uint64_t t0 = perf_mark_begin();
    while (in->pos < in->size) {
        size_t i0 = in->pos, o0 = out->pos;
        r = ZSTD_decompressStream(st->dctx, out, in);
        if (ZSTD_isError(r) || !r || (in->pos == i0 && out->pos == o0)) break;
    }
    perf_mark_end(PERF_M_ZSTD, t0, out->pos - o_start);
    kern_core_back(&away);
    if (d) {
        int n = *d - 1;
        __atomic_store_n(d, n, __ATOMIC_RELAXED);
        if (!n && kern_preempt_pending(c->thread)) kern_host_left(c->thread);
    }
    return r;
}

static void stream_count(const Stream *st, uint64_t out) {
    uint64_t n = __atomic_add_fetch(&SS.streams, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&SS.in, st->in, __ATOMIC_RELAXED);
    __atomic_add_fetch(&SS.out, out, __ATOMIC_RELAXED);
    if (n == 1 || !(n & 0x3ff))
        rt_log("zstd: %llu streams on the host in %llu feeds, %.1f MiB in, %.1f MiB out, %.0f ms; %llu streams left to "
               "the game's decoder; check: %llu compared, %llu differ", (unsigned long long)n,
               (unsigned long long)SS.feeds, SS.in / 1048576.0, SS.out / 1048576.0, SS.ns / 1e6,
               (unsigned long long)SS.guest, (unsigned long long)SS.checked, (unsigned long long)SS.differ);
}

// A stream at its begin (phase 1): its slot, the host context ready for the frame, or NULL when the game's feed is to
// decode it (see above). In check mode the guest's state is left alone.
static Stream *stream_begin(uint64_t s, uint64_t src, int32_t size, uint64_t ddict) {
    if (size <= 0 || !src || src + (uint64_t)size > RT_ADDRESS_LIMIT) return NULL;
    ZSTD_frameHeader h;
    if (ZSTD_getFrameHeader(&h, gp(src), (size_t)size) != 0 || h.frameType != ZSTD_frame || !h.frameContentSize ||
        h.frameContentSize == ZSTD_CONTENTSIZE_UNKNOWN || h.frameContentSize > 0x7fffffffull)
        return NULL;
    uint64_t dst = ld64(s + 8);
    int32_t cap = (int32_t)ld32(s + 0x10);
    // an output without room for the content fails part way in the game, with what fits written: left to the game
    if (cap < 0 || (uint64_t)cap < h.frameContentSize || !dst || dst + (uint64_t)cap > RT_ADDRESS_LIMIT) return NULL;
    if (src < dst + (uint64_t)cap && dst < src + (uint64_t)size) return NULL;   // in place: the game's decoder
    ZSTD_DDict *dd = NULL;
    if (ddict) {
        if (ddict >= RT_ADDRESS_LIMIT - 24) return NULL;
        uint64_t f[3];   // ZSTD_DDict: dictBuffer, dictContent, dictSize
        memcpy(f, gp(ddict), sizeof f);
        if (!(dd = dict_for(ddict, f[1], f[2]))) return NULL;
    }
    Stream *st = stream_take(s);
    if (!st) return NULL;
    free(st->shadow);
    st->shadow = NULL, st->content = h.frameContentSize, st->pos = 0, st->dst0 = dst, st->cap0 = (uint64_t)cap;
    st->in = 0, st->done = st->bad = st->moved = 0;
    if ((!st->dctx && !(st->dctx = stream_dctx())) ||
        ZSTD_isError(ZSTD_DCtx_reset(st->dctx, ZSTD_reset_session_only)) ||
        ZSTD_isError(ZSTD_DCtx_refDDict(st->dctx, dd)) ||
        (check_mode() && !(st->shadow = malloc((size_t)st->content)))) {
        stream_drop(st);
        return NULL;
    }
    if (!check_mode()) {   // what the game's begin leaves in its state
        st32(s + 0x14, (uint32_t)st->content);
        st64(s + 0x30, ld64(s + 0x18));
        *(uint8_t *)gp(s) = 2;
    }
    return st;
}

// A feed of a host stream: the guest's fields and result as the game's feed leaves them.
static void stream_feed(Cpu *c, Stream *st, uint64_t s, uint64_t src, int32_t size) {
    if (size <= 0) {   // nothing to feed: the content left, as the game
        c->x[0] = ld32(s + 0x14);
        return;
    }
    uint64_t t0 = kern_now_ns(), dst = ld64(s + 8);
    uint32_t cap = ld32(s + 0x10), ret;
    if (!src || src + (uint64_t)size > RT_ADDRESS_LIMIT || dst != st->dst0 + st->pos || cap != st->cap0 - st->pos) {
        static int logged;
        if (logged++ < 8)
            rt_log("zstd: a host stream cannot go on after %llu bytes out: input %#llx + %d, output at %#llx with room "
                   "for %u where %#llx with %llu was left", (unsigned long long)st->pos, (unsigned long long)src, size,
                   (unsigned long long)dst, cap, (unsigned long long)(st->dst0 + st->pos),
                   (unsigned long long)(st->cap0 - st->pos));
        *(uint8_t *)gp(s) = 3;
        c->x[0] = 0xffffffffu;
        stream_drop(st);
        return;
    }
    ZSTD_inBuffer in = {gp(src), (size_t)size, 0};
    ZSTD_outBuffer out = {gp(st->dst0), (size_t)st->cap0, (size_t)st->pos};
    size_t r = stream_run(c, st, &out, &in);
    uint32_t wrote = (uint32_t)(out.pos - st->pos);
    st->pos = out.pos, st->in += in.pos;
    st64(s + 8, dst + wrote);
    st32(s + 0x10, cap - wrote);
    st32(s + 0x14, ld32(s + 0x14) - wrote);
    __atomic_add_fetch(&SS.feeds, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&SS.ns, kern_now_ns() - t0, __ATOMIC_RELAXED);
    int end = 1;
    if (ZSTD_isError(r)) {
        *(uint8_t *)gp(s) = 3;
        ret = ZSTD_getErrorCode(r) == ZSTD_error_dstSize_tooSmall ? 0xfffffffeu : 0xffffffffu;
        static int logged;
        if (logged++ < 8)
            rt_log("zstd: a host stream failed after %llu bytes in: %s", (unsigned long long)st->in, ZSTD_getErrorName(r));
    } else if (!r && in.pos < in.size) {   // input after the frame's end
        *(uint8_t *)gp(s) = 3;
        ret = 0xfffffffdu;
    } else if (!r) {
        *(uint8_t *)gp(s) = 4;
        ret = ld32(s + 0x14);
    } else if (in.pos < in.size) {   // nothing moved with input left
        *(uint8_t *)gp(s) = 3;
        ret = 0xfffffffeu;
    } else {
        ret = ld32(s + 0x14);
        end = 0;
        if (!ret) stream_spent(st);
    }
    c->x[0] = ret;
    if (end) {
        stream_count(st, st->pos);
        stream_drop(st);
    }
}

// Check mode: the host decodes the chunk into its own buffer, the game's feed runs as always and its result stands;
// when the game ends the stream, or has all its content out, the two outputs are compared.
static void stream_check(Cpu *c, Stream *st, uint64_t s, uint64_t src, int32_t size, uint64_t ddict) {
    if (st->spent) {   // compared already: the game was not expected to feed on after a result of 0
        static int logged;
        if (logged++ < 4)
            rt_log("zstd check: a feed of %d bytes after the content of a stream was out (content %llu, lr %#llx)", size,
                   (unsigned long long)st->content, (unsigned long long)c->x[30]);
        rt_call(c, GUEST_STREAM, 4, s, src, (uint64_t)(uint32_t)size, ddict);
        uint8_t ph = *(const uint8_t *)gp(s);
        if (ph == 3 || ph == 4) stream_drop(st);
        return;
    }
    if (!st->bad && !st->done && size > 0 && src && src + (uint64_t)size <= RT_ADDRESS_LIMIT) {
        ZSTD_inBuffer in = {gp(src), (size_t)size, 0};
        ZSTD_outBuffer out = {st->shadow, (size_t)st->content, (size_t)st->pos};
        size_t r = stream_run(c, st, &out, &in);
        st->pos = out.pos, st->in += in.pos;
        if (ZSTD_isError(r)) st->bad = 1;
        else if (!r) st->done = in.pos == in.size ? 1 : 2;   // 2: input after the frame's end
        else if (in.pos < in.size) st->bad = 1;
    }
    uint32_t g = (uint32_t)rt_call(c, GUEST_STREAM, 4, s, src, (uint64_t)(uint32_t)size, ddict);
    uint8_t ph = *(const uint8_t *)gp(s);
    // a host stream writes into the buffer the stream began with: the game's output has to advance through it alone
    if ((ph == 2 || ph == 4) && ld64(s + 8) - st->dst0 != st->content - ld32(s + 0x14)) st->moved = 1;
    int out_all = ph == 2 && !g;   // the job feeds no more, with or without the frame's last bytes
    if (ph != 3 && ph != 4 && !out_all) return;
    __atomic_add_fetch(&SS.checked, 1, __ATOMIC_RELAXED);
    int same = ph == 3 ? st->bad || st->done != 1   // the game failed: the host must not have a clean frame
                       : !st->bad && !st->moved && st->done != 2 && (ph != 4 || st->done == 1) &&
                             st->pos == st->content && !memcmp(st->shadow, gp(st->dst0), (size_t)st->content);
    if (!same) {
        __atomic_add_fetch(&SS.differ, 1, __ATOMIC_RELAXED);
        static int logged;
        if (logged++ < 12)
            rt_log("zstd check: stream MISMATCH: the game ended in phase %d with %#x after %llu bytes in (content %llu, "
                   "ddict %#llx, lr %#llx)%s; the host wrote %llu bytes, %s", (int)ph, g, (unsigned long long)st->in,
                   (unsigned long long)st->content, (unsigned long long)ddict, (unsigned long long)c->x[30],
                   st->moved ? ", its output not advancing through one buffer" : "", (unsigned long long)st->pos,
                   st->bad ? "failed" : st->done == 1 ? "reached the frame's end"
                   : st->done ? "found input after the frame's end" : "had not reached the frame's end");
    }
    stream_count(st, st->pos);
    if (out_all) {   // keeps the slot in case the game does feed on; nothing more is compared
        free(st->shadow);
        st->shadow = NULL;
        stream_spent(st);
    } else {
        stream_drop(st);
    }
}

static void host_stream(Cpu *c) {
    uint64_t s = c->x[0], src = c->x[1], ddict = c->x[3];
    int32_t size = (int32_t)c->x[2];
    Stream *st = NULL;
    if (trusted() && s && s < RT_ADDRESS_LIMIT - 0x38) {
        uint8_t ph = *(const uint8_t *)gp(s);
        if (ph == 1) {
            stream_drop_of(s);   // a stream the game gave up without ending it
            st = stream_begin(s, src, size, ddict);
            if (!st) __atomic_add_fetch(&SS.guest, 1, __ATOMIC_RELAXED);
        } else if (ph == 2) {
            st = stream_find(s);
        }
    }
    if (!st) rt_call(c, GUEST_STREAM, 4, s, src, c->x[2], ddict);
    else if (check_mode()) stream_check(c, st, s, src, size, ddict);
    else stream_feed(c, st, s, src, size);
}

static void host_multiframe(Cpu *c) { decode(c, c->x[5], c->x[6], c->x[7], GUEST_MULTIFRAME, 8); }
static void host_using_ddict(Cpu *c) { decode(c, 0, 0, c->x[5], GUEST_USING_DDICT, 6); }

__attribute__((constructor)) static void zstd_register(void) {
    rt_register_host(HOST_MULTIFRAME, host_multiframe);
    rt_register_host(HOST_USING_DDICT, host_using_ddict);
    rt_register_host(HOST_STREAM, host_stream);
}

// ---- --probe-zstd: host streams on frames made here, fed through stream_begin and stream_feed with a stream state
// laid out as the game's. Checks the output and the state's fields after every feed, the frame's end, the streams that
// are left to the game, input after the end, a damaged frame, a moved output and the slots. Needs no game files.
static int zchecks, zfails;
#define ZCHECK(cond, ...) do { zchecks++; if (!(cond)) { zfails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void probe_state(uint64_t s, uint64_t dst, uint32_t cap, uint64_t ws) {
    memset(gp(s), 0, 0x38);
    *(uint8_t *)gp(s) = 1;
    st64(s + 8, dst), st32(s + 0x10, cap), st64(s + 0x18, ws);
}

static int probe_phase(uint64_t s) { return *(const uint8_t *)gp(s); }

// One feed as host_stream runs it outside check mode: the result, or 1 << 32 when the stream is the game's.
static uint64_t probe_feed(uint64_t s, uint64_t src, int32_t size) {
    static Cpu c;
    Stream *st = NULL;
    if (probe_phase(s) == 1) stream_drop(stream_find(s)), st = stream_begin(s, src, size, 0);
    else if (probe_phase(s) == 2) st = stream_find(s);
    if (!st) return 1ull << 32;
    stream_feed(&c, st, s, src, size);
    return c.x[0];
}

// Feeds frame[0, n) in chunks of `chunk` bytes after a first one of `first`.
static void probe_stream(const char *what, uint64_t s, uint64_t ws, uint64_t frame, size_t n, uint64_t out,
                         const uint8_t *raw, size_t rawn, size_t first, size_t chunk) {
    uint32_t cap = (uint32_t)rawn + 4096;
    probe_state(s, out, cap, ws);
    memset(gp(out), 0xA5, rawn);
    size_t off = 0, feeds = 0;
    uint64_t r = 1;
    while (off < n) {
        size_t k = off ? chunk : first;
        if (k > n - off) k = n - off;
        r = probe_feed(s, frame + off, (int32_t)k);
        off += k, feeds++;
        uint64_t wrote = ld64(s + 8) - out;
        int ph = probe_phase(s);
        if (r >> 32 || wrote > rawn || ld32(s + 0x10) != cap - wrote || ld32(s + 0x14) != rawn - wrote ||
            r != rawn - wrote || (ph != 2 && ph != 4) || (ph == 4) != (off == n)) {
            ZCHECK(0, "%s: the feed ending at %zu of %zu gave %#llx in phase %d with %llu bytes out, room %u, left %u",
                   what, off, n, (unsigned long long)r, ph, (unsigned long long)wrote, ld32(s + 0x10), ld32(s + 0x14));
            stream_drop(stream_find(s));
            return;
        }
    }
    ZCHECK(r == 0 && probe_phase(s) == 4, "%s: ended with %#llx in phase %d", what, (unsigned long long)r, probe_phase(s));
    ZCHECK(ld64(s + 0x30) == ws, "%s: the state's DCtx field is not its workspace", what);
    ZCHECK(!memcmp(gp(out), raw, rawn), "%s: the output differs from the source", what);
    ZCHECK(!stream_find(s), "%s: the slot is still taken after the frame's end", what);
    printf("zstd stream: %s: %zu bytes in %zu feeds gave %zu\n", what, n, feeds, rawn);
}

int probe_zstd(void) {
    enum { RAWN = 3 << 20, SMALLN = 100000, ARENA = 12 << 20 };
    const RtGuestLayout *g = mem_layout();
    uint64_t base = g->alias.base + 0x10000;
    if (mem_map_physical(base, ARENA)) {
        printf("probe-zstd: FAILED (no guest memory)\n");
        return 1;
    }
    uint64_t compa = base + (3 << 20), outa = base + (7 << 20), s = base + (11 << 20), ws = s + 0x10000;
    uint8_t *raw = gp(base);
    // random runs and repeats of earlier bytes from up to 2 MiB back: matches that reach far into earlier output
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < RAWN; i += 64) {
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        if (i >= 4096 && (x & 3)) {
            size_t far = i < (2u << 20) ? i : 2u << 20;
            memcpy(raw + i, raw + i - 64 - (size_t)((x >> 8) % (far - 63)), 64);
        } else {
            for (int k = 0; k < 64; k += 8) {
                x ^= x << 13, x ^= x >> 7, x ^= x << 17;
                memcpy(raw + i + k, &x, 8);
            }
        }
    }
    size_t cn = ZSTD_compress(gp(compa), (size_t)3 << 20, raw, RAWN, 3);
    ZSTD_CCtx *cc = ZSTD_createCCtx();
    uint64_t smalla = compa + (3 << 20), bada = smalla + (256 << 10);
    size_t sn = cc ? ZSTD_CCtx_setParameter(cc, ZSTD_c_checksumFlag, 1), ZSTD_compress2(cc, gp(smalla), 256 << 10, raw, SMALLN)
                   : (size_t)-1;
    ZSTD_freeCCtx(cc);
    if (ZSTD_isError(cn) || ZSTD_isError(sn) || sn < 2048) {
        printf("probe-zstd: FAILED (the frames could not be made)\n");
        return 1;
    }

    probe_stream("a 3 MiB frame in chunks of 64 KiB", s, ws, compa, cn, outa, raw, RAWN, 65536, 65536);
    probe_stream("the same in chunks of 4099 bytes", s, ws, compa, cn, outa, raw, RAWN, 64, 4099);
    probe_stream("the same in one feed", s, ws, compa, cn, outa, raw, RAWN, cn, cn);
    probe_stream("a frame with a checksum in chunks of 977 bytes", s, ws, smalla, sn, outa, raw, SMALLN, 32, 977);
    probe_stream("the same with the checksum in a feed of its own", s, ws, smalla, sn, outa, raw, SMALLN, sn - 4, 4);

    probe_state(s, outa, RAWN + 4096, ws);
    ZCHECK(probe_feed(s, compa, 3) >> 32 && probe_phase(s) == 1, "a first chunk without the whole header was taken");
    probe_state(s, outa, RAWN - 1, ws);
    ZCHECK(probe_feed(s, compa, 65536) >> 32 && probe_phase(s) == 1, "an output smaller than the content was taken");
    probe_state(s, compa + 4096, RAWN + 4096, ws);
    ZCHECK(probe_feed(s, compa, 65536) >> 32 && probe_phase(s) == 1, "an output that overlaps its input was taken");
    printf("zstd stream: a short first chunk, a small output and an overlapping one are left to the game\n");

    probe_state(s, outa, SMALLN + 4096, ws);
    uint64_t r = probe_feed(s, smalla, (int32_t)sn + 8);
    ZCHECK(r == 0xfffffffdu && probe_phase(s) == 3 && !stream_find(s), "input after the frame's end gave %#llx in phase %d",
           (unsigned long long)r, probe_phase(s));

    memcpy(gp(bada), gp(smalla), sn);
    ((uint8_t *)gp(bada))[sn / 2] ^= 0x55;
    probe_state(s, outa, SMALLN + 4096, ws);
    r = 0;
    for (size_t off = 0; off < sn && r < 0x80000000u; off += 977)
        r = probe_feed(s, bada + off, (int32_t)(sn - off < 977 ? sn - off : 977));
    ZCHECK((r == 0xffffffffu || r == 0xfffffffeu) && probe_phase(s) == 3 && !stream_find(s),
           "a damaged frame gave %#llx in phase %d", (unsigned long long)r, probe_phase(s));

    probe_state(s, outa, SMALLN + 4096, ws);
    r = probe_feed(s, smalla, 977);
    st64(s + 8, ld64(s + 8) + 16);
    r = probe_feed(s, smalla + 977, 977);
    ZCHECK(r == 0xffffffffu && probe_phase(s) == 3 && !stream_find(s), "a feed with the output moved gave %#llx in phase %d",
           (unsigned long long)r, probe_phase(s));
    printf("zstd stream: input after the end, a damaged frame and a moved output fail the stream\n");

    // a stream with its content out and its checksum unread is spent: the next stream takes its slot when none is free
    probe_state(s, outa, SMALLN + 4096, ws);
    r = probe_feed(s, smalla, (int32_t)sn - 4);
    ZCHECK(r == 0 && probe_phase(s) == 2 && stream_find(s), "content out before the checksum gave %#llx in phase %d",
           (unsigned long long)r, probe_phase(s));
    int took = 0;
    for (int i = 1; i <= NSTREAM; i++) {
        probe_state(s + 0x40 * i, outa, SMALLN + 4096, ws);
        took += !(probe_feed(s + 0x40 * i, smalla, 64) >> 32);
    }
    ZCHECK(took == NSTREAM && !stream_find(s), "%d of %d streams begun beside a spent one", took, NSTREAM);
    probe_state(s + 0x40 * (NSTREAM + 1), outa, SMALLN + 4096, ws);
    ZCHECK(probe_feed(s + 0x40 * (NSTREAM + 1), smalla, 64) >> 32, "a stream was taken with every slot in use");
    for (int i = 1; i <= NSTREAM; i++) stream_drop(stream_find(s + 0x40 * i));
    printf("zstd stream: %d streams at once, one of them in a spent slot; one more is left to the game\n", NSTREAM);

    if (zfails) {
        printf("probe-zstd: FAILED (%d of %d checks)\n", zfails, zchecks);
        return 1;
    }
    printf("probe-zstd: passed (%d checks)\n", zchecks);
    return 0;
}

#else

int probe_zstd(void) {
    printf("probe-zstd: this build has no patch page, so no host decoder\n");
    return 0;
}

#endif

// NX2NAVI_TRACE=perf "mem: sizes" (perf.c): host dictionaries (never freed) and stream slots taken.
void zstd_sizes(PerfSizes *s) {
    uint64_t d, st = 0;
    plat_mutex_lock(&dict_lock);
    d = (uint64_t)ndicts;
    plat_mutex_unlock(&dict_lock);
    plat_mutex_lock(&stream_lock);
    for (int i = 0; i < NSTREAM; i++) st += streams[i].s != 0;
    plat_mutex_unlock(&stream_lock);
    perf_size(s, "zstd dictionaries", d, NDICT, PERF_SIZE_NONE);
    perf_size(s, "zstd stream slots", st, NSTREAM, PERF_SIZE_NONE);
}
