// A translated shader's SPIR-V that is kept zstd-compressed while nobody reads it (gfx_shader.c "SPIR-V at rest").
//
// A store holds the SPIR-V as a plain copy, as a zstd frame, or as both. Readers pin it (spv_pin), which hands out the
// plain copy and decodes the frame first when the plain copy is gone, and unpin it when done (spv_unpin). One thread,
// the packer, makes the frame of a store that has gone unused for a while and then frees the plain copy (spv_pack_*).
//
// Lock rules, which make a reader never see a buffer swapped or freed under it:
//   - every store of one owner shares one lock (PlatMutex), held only for a few loads and stores: never across a
//     decode, an encode or a free, so a reader on the drawing thread waits for no codec;
//   - plain changes under the lock only: a reader sets it from NULL to a buffer it decoded, the packer sets it back to
//     NULL, and only while pins is 0. A reader gets the pointer and counts its pin in the same locked section, so
//     the copy it holds cannot be freed before its unpin;
//   - z is set once, by the packer, before any plain copy is dropped, and stays until spv_store_free: a reader that
//     found plain NULL under the lock decodes z outside it;
//   - one packer per owner at a time (the owner's own lock around its passes): spv_pack_begin and spv_pack_end of one
//     store pair up, and nothing else drops plain copies.
// Two readers that find the frame alone both decode it; the first one to come back installs its copy and the other
// frees its own. The frame is checked by decoding it before the plain copy may go (spv_pack_encode), so a store never
// holds a frame that does not give back its SPIR-V bit for bit.
//
// Header-only so that tests/spv_store_check.c runs it against the platform layer and libzstd alone.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "platform/platform.h"

typedef struct {
    uint32_t *plain;            // the SPIR-V readers get; NULL while the frame alone holds it (changes under the lock)
    uint64_t words;             // its length in 32-bit words; fixed once the store is filled
    void *z;                    // its zstd frame, exactly zsize bytes; set once, freed by spv_store_free only
    uint32_t zsize;
    uint32_t pins;              // readers between spv_pin and spv_unpin (under the lock)
    uint64_t used_ms;           // when the last reader let go, or when the store was filled (under the lock)
    uint64_t fnv;               // FNV-1a (64-bit) over the SPIR-V bytes once fnv_ok is 1
    volatile int32_t fnv_ok;
} SpvStore;

// The packer's buffers, kept from one store to the next: the encoder's output before it is copied out at its size,
// and the decode that checks it.
typedef struct {
    ZSTD_CCtx *cc;
    void *out;
    size_t out_cap;
    uint32_t *back;
    size_t back_cap;
} SpvPacker;

static inline uint64_t spv_fnv_words(const uint32_t *w, uint64_t words) {
    const uint8_t *b = (const uint8_t *)w;
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i < words * 4; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

// The store takes plain (malloc'd, words long) as its plain copy, last used at now_ms.
static inline void spv_store_fill(SpvStore *s, uint32_t *plain, uint64_t words, uint64_t now_ms) {
    memset(s, 0, sizeof *s);
    s->plain = plain, s->words = words, s->used_ms = now_ms;
}

static inline void spv_store_free(SpvStore *s) {
    free(s->plain), free(s->z);
    s->plain = NULL, s->z = NULL, s->zsize = 0;
}

// The plain SPIR-V, valid until the matching spv_unpin. *decode_ns: the time spent decoding the frame here, 0 when
// the plain copy was there. NULL only when the decode could not allocate its buffer or the frame did not decode to
// the store's length (spv_pack_encode checked it, so neither should happen).
static inline const uint32_t *spv_pin(SpvStore *s, PlatMutex *lk, uint64_t *decode_ns) {
    *decode_ns = 0;
    plat_mutex_lock(lk);
    uint32_t *p = s->plain;
    if (p) s->pins++;
    plat_mutex_unlock(lk);
    if (p) return p;
    uint64_t t0 = plat_now_ns();
    size_t bytes = (size_t)s->words * 4;
    uint32_t *buf = malloc(bytes ? bytes : 4);
    if (!buf) return NULL;
    size_t got = ZSTD_decompress(buf, bytes, s->z, s->zsize);
    if (ZSTD_isError(got) || got != bytes) {
        free(buf);
        return NULL;
    }
    plat_mutex_lock(lk);
    if (s->plain) p = s->plain;           // another reader decoded it meanwhile: its copy, and this one goes
    else __atomic_store_n(&s->plain, buf, __ATOMIC_RELAXED), p = buf, buf = NULL;
    s->pins++;
    plat_mutex_unlock(lk);
    free(buf);
    *decode_ns = plat_now_ns() - t0;
    return p;
}

static inline void spv_unpin(SpvStore *s, PlatMutex *lk, uint64_t now_ms) {
    plat_mutex_lock(lk);
    s->pins--;
    if (now_ms > s->used_ms) s->used_ms = now_ms;
    plat_mutex_unlock(lk);
}

// The FNV-1a of the SPIR-V: worked out at the first call (pinning the store, so decoding it if it is compressed) and
// kept. spv_pack_encode works it out before it compresses, so asking a compressed store decodes nothing.
static inline uint64_t spv_fnv(SpvStore *s, PlatMutex *lk, uint64_t now_ms, uint64_t *decode_ns) {
    *decode_ns = 0;
    if (__atomic_load_n(&s->fnv_ok, __ATOMIC_ACQUIRE)) return __atomic_load_n(&s->fnv, __ATOMIC_RELAXED);
    const uint32_t *w = spv_pin(s, lk, decode_ns);
    if (!w) return 0;
    uint64_t h = spv_fnv_words(w, s->words);
    spv_unpin(s, lk, now_ms);
    __atomic_store_n(&s->fnv, h, __ATOMIC_RELAXED);   // racing callers store the same value
    __atomic_store_n(&s->fnv_ok, 1, __ATOMIC_RELEASE);
    return h;
}

// Whether the store has gone unused for idle_ms at now_ms: no pin, and its last use at least that long ago (a use
// stamped after the packer read the clock counts as recent). Caller holds the lock.
static inline int spv_idle_locked(const SpvStore *s, uint64_t now_ms, uint64_t idle_ms) {
    return !s->pins && now_ms >= s->used_ms && now_ms - s->used_ms >= idle_ms;
}

enum { SPV_PACK_NONE, SPV_PACK_ENCODE, SPV_PACK_DROP };

// The packer's first step on a store: SPV_PACK_NONE when there is nothing to do (no plain copy, in use or used within
// idle_ms); SPV_PACK_DROP when the frame exists and the plain copy may go (spv_pack_end); SPV_PACK_ENCODE when the
// frame is to be made first: the plain copy is pinned for the packer, which encodes it (spv_pack_encode) and then
// calls spv_pack_end in any case.
static inline int spv_pack_begin(SpvStore *s, PlatMutex *lk, uint64_t now_ms, uint64_t idle_ms) {
    plat_mutex_lock(lk);
    int r = SPV_PACK_NONE;
    if (s->plain && spv_idle_locked(s, now_ms, idle_ms)) {
        r = s->z ? SPV_PACK_DROP : SPV_PACK_ENCODE;
        if (r == SPV_PACK_ENCODE) s->pins++;
    }
    plat_mutex_unlock(lk);
    return r;
}

// Encodes the plain copy the packer pinned (spv_pack_begin gave SPV_PACK_ENCODE) at zstd level `level` and checks the
// frame by decoding it. No lock is held: readers go on using the plain copy meanwhile. Returns the frame (malloc'd,
// *zsize bytes) or NULL when the encoder failed or the check found a difference; the FNV is worked out here too.
static inline void *spv_pack_encode(SpvStore *s, SpvPacker *pk, int level, uint32_t *zsize) {
    const uint32_t *w = s->plain;   // pinned by spv_pack_begin: neither swapped nor freed until spv_pack_end
    size_t bytes = (size_t)s->words * 4;
    if (!__atomic_load_n(&s->fnv_ok, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&s->fnv, spv_fnv_words(w, s->words), __ATOMIC_RELAXED);
        __atomic_store_n(&s->fnv_ok, 1, __ATOMIC_RELEASE);
    }
    if (!pk->cc && !(pk->cc = ZSTD_createCCtx())) return NULL;
    size_t cap = ZSTD_compressBound(bytes);
    if (pk->out_cap < cap) {
        void *o = realloc(pk->out, cap);
        if (!o) return NULL;
        pk->out = o, pk->out_cap = cap;
    }
    if (pk->back_cap < bytes + 4) {
        void *b = realloc(pk->back, bytes + 4);
        if (!b) return NULL;
        pk->back = b, pk->back_cap = bytes + 4;
    }
    size_t n = ZSTD_compressCCtx(pk->cc, pk->out, cap, w, bytes, level);
    if (ZSTD_isError(n) || n > UINT32_MAX) return NULL;
    size_t got = ZSTD_decompress(pk->back, bytes + 4, pk->out, n);
    if (ZSTD_isError(got) || got != bytes || memcmp(pk->back, w, bytes)) return NULL;
    void *z = malloc(n ? n : 1);
    if (!z) return NULL;
    memcpy(z, pk->out, n);
    *zsize = (uint32_t)n;
    return z;
}

// The packer's last step on a store. After SPV_PACK_ENCODE, z is what spv_pack_encode gave (NULL: the store stays
// plain) and the packer's pin goes; then the plain copy is freed if the store still has the frame and is still idle
// (a reader may have come meanwhile). Returns the bytes freed.
static inline uint64_t spv_pack_end(SpvStore *s, PlatMutex *lk, int step, void *z, uint32_t zsize, uint64_t now_ms,
                                    uint64_t idle_ms) {
    uint32_t *drop = NULL;
    plat_mutex_lock(lk);
    if (step == SPV_PACK_ENCODE) {
        s->pins--;   // the packer's: no use stamp, it is not a reader
        if (z && !s->z) __atomic_store_n(&s->zsize, zsize, __ATOMIC_RELAXED), __atomic_store_n(&s->z, z, __ATOMIC_RELAXED), z = NULL;
    }
    if (s->plain && s->z && spv_idle_locked(s, now_ms, idle_ms)) drop = s->plain, __atomic_store_n(&s->plain, NULL, __ATOMIC_RELAXED);
    plat_mutex_unlock(lk);
    free(z);   // a frame another packer step installed first (not with one packer per owner)
    free(drop);
    return drop ? (uint64_t)s->words * 4 : 0;
}

static inline void spv_packer_free(SpvPacker *pk) {
    if (pk->cc) ZSTD_freeCCtx(pk->cc);
    free(pk->out), free(pk->back);
    memset(pk, 0, sizeof *pk);
}
