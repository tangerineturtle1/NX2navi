// runtime/spv_store.h: a shader's SPIR-V kept zstd-compressed while unused (gfx_shader.c "SPIR-V at rest"). Built and
// run by tests/spv_store_check.py against the platform layer and libzstd.
//
// The single-thread checks walk one store through its states: plain, pinned, packed (the frame made, checked and the
// plain copy freed), decoded back by a pin bit for bit, idle again (only the plain copy goes, the frame stays), and
// its FNV-1a read from a compressed store without a decode. A reader that arrives while the packer encodes keeps the
// plain copy. The stress check runs reader threads that pin random stores and compare every word against the
// original, while a packer thread packs every store as soon as nobody holds it; no reader may ever see other words,
// and every pin must be given back.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spv_store.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

void rt_log(const char *fmt, ...) {   // the platform layer's lines
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) {
    rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
    return (uint32_t)(rng >> 32);
}

// SPIR-V-like words: the header, then instructions with small opcodes, word counts and ids. They are more random than
// the translator's modules (a frame keeps about 65% of them, against 26% of real SPIR-V), which only makes the
// encoder's work larger.
static uint32_t *make_module(uint64_t words) {
    uint32_t *w = malloc(words * 4);
    w[0] = 0x07230203, w[1] = 0x00010300, w[2] = 0, w[3] = 4096, w[4] = 0;
    for (uint64_t i = 5; i < words;) {
        uint32_t len = 2 + rnd() % 5;
        if (i + len > words) len = (uint32_t)(words - i);
        w[i] = len << 16 | (rnd() % 8 ? 61 + rnd() % 8 : rnd() % 400);
        for (uint32_t k = 1; k < len; k++) w[i + k] = rnd() % 4 ? 1000 + rnd() % 3000 : rnd();
        i += len;
    }
    return w;
}

static PlatMutex lk = PLAT_MUTEX_INIT;

static void single_checks(void) {
    uint64_t words = 30000, ns;
    uint32_t *orig = make_module(words), *plain = malloc(words * 4);
    memcpy(plain, orig, words * 4);
    SpvStore s;
    spv_store_fill(&s, plain, words, 100);
    SpvPacker pk = {0};

    const uint32_t *w = spv_pin(&s, &lk, &ns);
    CHECK(w == plain && ns == 0, "a plain store hands out its copy without a decode");
    CHECK(spv_pack_begin(&s, &lk, 100000, 1000) == SPV_PACK_NONE, "a pinned store is not packed");
    spv_unpin(&s, &lk, 500);
    CHECK(s.used_ms == 500 && s.pins == 0, "unpin stamps the use");
    CHECK(spv_pack_begin(&s, &lk, 1400, 1000) == SPV_PACK_NONE, "used 0.9 s ago with a 1 s wait: kept plain");
    CHECK(spv_pack_begin(&s, &lk, 400, 1000) == SPV_PACK_NONE, "a use stamped after the packer's clock counts as recent");

    int step = spv_pack_begin(&s, &lk, 1500, 1000);
    CHECK(step == SPV_PACK_ENCODE && s.pins == 1, "idle for 1 s: the packer pins it to encode");
    uint32_t zn = 0;
    void *z = spv_pack_encode(&s, &pk, 1, &zn);
    CHECK(z && zn > 0 && zn < words * 4, "the frame is made (%u bytes of %llu)", zn, (unsigned long long)words * 4);
    CHECK(s.fnv_ok && s.fnv == spv_fnv_words(orig, words), "the FNV is worked out before the plain copy can go");
    uint64_t freed = spv_pack_end(&s, &lk, step, z, zn, 1500, 1000);
    CHECK(freed == words * 4 && !s.plain && s.z == z && s.zsize == zn && s.pins == 0, "the plain copy is freed, the frame kept");
    printf("round trip: %llu bytes kept as %u (%.1f%%)\n", (unsigned long long)words * 4, zn, 100.0 * zn / (words * 4.0));

    uint64_t h = spv_fnv(&s, &lk, 1600, &ns);
    CHECK(h == spv_fnv_words(orig, words) && ns == 0 && !s.plain, "the FNV of a compressed store needs no decode");

    w = spv_pin(&s, &lk, &ns);
    CHECK(w && s.plain == w && s.pins == 1, "a pin of a compressed store decodes it and installs the copy");
    CHECK(w && !memcmp(w, orig, words * 4), "the decoded SPIR-V is the original bit for bit");
    const uint32_t *w2 = spv_pin(&s, &lk, &ns);
    CHECK(w2 == w && ns == 0 && s.pins == 2, "a second reader shares the decoded copy");
    spv_unpin(&s, &lk, 2000), spv_unpin(&s, &lk, 2000);

    CHECK(spv_pack_begin(&s, &lk, 2500, 1000) == SPV_PACK_NONE, "decoded and used again: plain until idle again");
    step = spv_pack_begin(&s, &lk, 3000, 1000);
    CHECK(step == SPV_PACK_DROP && s.pins == 0, "idle again with its frame: only the plain copy goes");
    freed = spv_pack_end(&s, &lk, step, NULL, 0, 3000, 1000);
    CHECK(freed == words * 4 && !s.plain && s.z == z, "the frame made before stays");

    // a reader that comes while the packer encodes keeps the plain copy
    SpvStore t;
    uint32_t *p2 = malloc(words * 4);
    memcpy(p2, orig, words * 4);
    spv_store_fill(&t, p2, words, 0);
    step = spv_pack_begin(&t, &lk, 5000, 1000);
    CHECK(step == SPV_PACK_ENCODE, "idle: encode");
    w = spv_pin(&t, &lk, &ns);
    CHECK(w == p2, "a reader during the encode gets the plain copy");
    z = spv_pack_encode(&t, &pk, 1, &zn);
    freed = spv_pack_end(&t, &lk, step, z, zn, 5000, 1000);
    CHECK(freed == 0 && t.plain == p2 && t.z == z && t.pins == 1, "the frame is kept and the pinned plain copy stays");
    CHECK(!memcmp(w, orig, words * 4), "the reader's words are untouched");
    spv_unpin(&t, &lk, 5001);
    CHECK(spv_pack_begin(&t, &lk, 5500, 1000) == SPV_PACK_NONE, "used at the encode: not dropped before its wait");

    spv_store_free(&s), spv_store_free(&t), spv_packer_free(&pk);
    free(orig);
}

// ---- stress: readers and one packer on shared stores
#define STORES 64
#define READERS 4
static SpvStore st[STORES];
static uint32_t *orig_of[STORES];
static volatile int32_t stop;
static volatile int64_t reads, decodes, mismatches;
static int64_t packed, dropped;

static void reader(void *arg) {
    uint64_t r = (uintptr_t)arg * 0x2545f4914f6cdd1dull + 1;
    while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
        r ^= r << 13, r ^= r >> 7, r ^= r << 17;
        int i = (int)(r % STORES);
        uint64_t ns, words = st[i].words;
        const uint32_t *w = spv_pin(&st[i], &lk, &ns);
        if (!w || memcmp(w, orig_of[i], words * 4)) plat_inc64(&mismatches);
        if (ns) plat_inc64(&decodes);
        if (r & 1) plat_yield();   // hold some pins across a time slice
        if (!w || w[0] != 0x07230203 || w[words - 1] != orig_of[i][words - 1]) plat_inc64(&mismatches);
        spv_unpin(&st[i], &lk, plat_now_ms());
        plat_inc64(&reads);
    }
}

static void packer(void *arg) {
    (void)arg;
    SpvPacker pk = {0};
    while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
        for (int i = 0; i < STORES; i++) {
            uint64_t now = plat_now_ms();
            int step = spv_pack_begin(&st[i], &lk, now, 0);
            if (step == SPV_PACK_NONE) continue;
            uint32_t zn = 0;
            void *z = step == SPV_PACK_ENCODE ? spv_pack_encode(&st[i], &pk, 1, &zn) : NULL;
            packed += z != NULL;
            dropped += spv_pack_end(&st[i], &lk, step, z, zn, now, 0) != 0;
        }
    }
    spv_packer_free(&pk);
}

static void stress(void) {
    for (int i = 0; i < STORES; i++) {
        uint64_t words = 64 + rnd() % 40000;
        orig_of[i] = make_module(words);
        uint32_t *p = malloc(words * 4);
        memcpy(p, orig_of[i], words * 4);
        spv_store_fill(&st[i], p, words, 0);
    }
    PlatThread *th[READERS + 1];
    for (int i = 0; i < READERS; i++) th[i] = plat_thread_new(reader, (void *)(uintptr_t)(i + 1), "spv reader");
    th[READERS] = plat_thread_new(packer, NULL, "spv packer");
    plat_sleep_ms(3000);
    __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
    for (int i = 0; i <= READERS; i++) plat_thread_join(th[i]);
    int pins = 0, frames = 0;
    for (int i = 0; i < STORES; i++) pins += st[i].pins != 0, frames += st[i].z != NULL;
    printf("stress: %lld reads, %lld decodes, %lld frames made, %lld plain copies dropped, %d of %d stores with a frame\n",
           (long long)reads, (long long)decodes, (long long)packed, (long long)dropped, frames, STORES);
    CHECK(!mismatches, "%lld reads saw other words than the original", (long long)mismatches);
    CHECK(!pins, "%d stores still pinned after every reader ended", pins);
    CHECK(packed == STORES && frames == STORES, "every store got exactly one frame (%lld made)", (long long)packed);
    CHECK(decodes > 100 && dropped > 100, "the readers and the packer met (%lld decodes, %lld drops)", (long long)decodes,
          (long long)dropped);
    for (int i = 0; i < STORES; i++) {   // the final state still gives back the original
        uint64_t ns;
        const uint32_t *w = spv_pin(&st[i], &lk, &ns);
        CHECK(w && !memcmp(w, orig_of[i], st[i].words * 4), "store %d after the stress", i);
        spv_unpin(&st[i], &lk, 0);
        spv_store_free(&st[i]), free(orig_of[i]);
    }
}

int main(void) {
    single_checks();
    stress();
    if (fails) printf("spv_store_check: %d checks failed\n", fails);
    else printf("spv_store_check: all checks passed\n");
    return fails != 0;
}
