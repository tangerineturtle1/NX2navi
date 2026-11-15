// tests/submit_clone_check.py builds this: runtime/nvn_submit_guard.h, a synchronous submission's deferred binds when
// another guest thread may be replaying the same recording copy. Four threads submit two hand-built copies while a
// "game" thread rewrites the handle arrays their deferred binds read; a quarter of the calls give their copy twice. A
// replay must see its copy's handles unchanged from its resolve to its end, the in-flight count must be back to 0,
// both the in-place and the clone path must run, and a copy given twice in one call must stay in place when its first
// occurrence does. Then nvn_copy_again and nvn_resolve_binds step by step. "old" resolves in place without the guard
// (0.10.1) and only prints what it sees, to show that the harness catches the race.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"
#include "nvn_submit_guard.h"

#define COPIES 2
#define BINDS 6
#define THREADS 4
#define ROUNDS 200000

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("submit_clone_check: FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static int old_mode;
static uint64_t tables[COPIES][BINDS][32];
static uint8_t *copy[COPIES];
static size_t copy_bytes;
static uint32_t busy[COPIES];
static volatile int32_t stop;
static uint64_t torn, clones, in_place, reads, changed, again;

static uint8_t *bind_rec(uint8_t *q, uint32_t op, int first, int n, const uint64_t *src) {
    *(op_hdr *)q = (op_hdr){op, (uint32_t)(8 + sizeof(p_bind_handles))};
    p_bind_handles *h = (p_bind_handles *)(q + 8);
    memset(h, 0, sizeof *h);
    h->stage = 4, h->first = first, h->n = n, h->src = (uint64_t)(uintptr_t)src;
    return q + 8 + sizeof(p_bind_handles);
}

static uint8_t *make_copy(int c) {
    size_t sz = 8 + BINDS * (8 + sizeof(p_bind_handles)) + 8;
    uint8_t *p = calloc(1, sz), *q = p;
    *(op_hdr *)q = (op_hdr){OP_BEGIN, 8}, q += 8;
    for (int b = 0; b < BINDS; b++) q = bind_rec(q, OP_BIND_TEX, b * 4, 32, tables[c][b]);
    *(op_hdr *)q = (op_hdr){OP_END, 8};
    copy_bytes = sz;
    return p;
}

static void game(void *arg) {
    (void)arg;
    for (uint64_t gen = 1; !__atomic_load_n(&stop, __ATOMIC_ACQUIRE); gen++)
        for (int c = 0; c < COPIES; c++)
            for (int b = 0; b < BINDS; b++)
                for (int i = 0; i < 32; i++) __atomic_store_n(&tables[c][b][i], gen, __ATOMIC_RELAXED);
}

static void snap(const uint8_t *p, uint64_t out[BINDS][32]) {
    const uint8_t *q = p + 8;
    for (int b = 0; b < BINDS; b++, q += 8 + sizeof(p_bind_handles))
        for (int i = 0; i < 32; i++)
            out[b][i] = __atomic_load_n(&((const p_bind_handles *)(q + 8))->h[i], __ATOMIC_RELAXED);
}

// nvn_api_cmd.c nvn_submit_prepare, the replay, nvn_submit_done; handles are copy numbers + 1, a clone's out is 0
static void submitter(void *arg) {
    uint64_t r = (uint64_t)(uintptr_t)arg * 0x2545F4914F6CDD1Dull + 7;
    static uint64_t a[THREADS][BINDS][32], b2[THREADS][BINDS][32];
    int me = (int)(uintptr_t)arg - 1;
    for (int k = 0; k < ROUNDS; k++) {
        r ^= r << 13, r ^= r >> 7, r ^= r << 17;
        int c = (int)(r % COPIES), n = (r >> 8) % 4 == 0 ? 2 : 1;
        uint64_t h[2] = {(uint64_t)c + 1, (uint64_t)c + 1}, out[2];
        uint8_t *use[2], *clone[2] = {NULL, NULL};
        for (int i = 0; i < n; i++) {
            out[i] = h[i], use[i] = copy[c];
            if (old_mode || nvn_copy_enter(&busy[c])) {
                __atomic_add_fetch(&in_place, 1, __ATOMIC_RELAXED);
            } else if (nvn_copy_again(h, out, i)) {
                __atomic_add_fetch(&again, 1, __ATOMIC_RELAXED);
            } else {
                clone[i] = malloc(copy_bytes);
                memcpy(clone[i], copy[c], copy_bytes);
                nvn_copy_leave(&busy[c]);
                use[i] = clone[i], out[i] = 0;
                __atomic_add_fetch(&clones, 1, __ATOMIC_RELAXED);
            }
        }
        const uint8_t *bad = NULL;
        for (int i = 0; i < n; i++) nvn_resolve_binds(use[i], 0, &reads, &changed, NULL, &bad);
        for (int i = 0; i < n; i++) {
            snap(use[i], a[me]);   // the replay reads the binds at its start ...
            for (volatile int s = 0; s < 300; s++) {}
            snap(use[i], b2[me]);   // ... and at its end (or a held continuation copies them)
            if (memcmp(a[me], b2[me], sizeof a[me])) __atomic_add_fetch(&torn, 1, __ATOMIC_RELAXED);
        }
        for (int i = 0; i < n; i++) {
            if (clone[i]) free(clone[i]);
            else if (!old_mode) nvn_copy_leave(&busy[c]);
        }
    }
}

static void stress(void) {
    for (int c = 0; c < COPIES; c++) copy[c] = make_copy(c);
    PlatThread *g = plat_thread_new(game, NULL, "game"), *t[THREADS];
    for (int i = 0; i < THREADS; i++) t[i] = plat_thread_new(submitter, (void *)(uintptr_t)(i + 1), "submit");
    for (int i = 0; i < THREADS; i++) plat_thread_join(t[i]);
    __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
    plat_thread_join(g);
    printf("submit_clone_check: %s: %d submit calls, %llu copies in place, %llu of them given again in their call, "
           "%llu clones; %llu deferred binds read, %llu changed; %llu replays saw their handles change under them\n",
           old_mode ? "old (prints only)" : "new", THREADS * ROUNDS, (unsigned long long)(in_place + again),
           (unsigned long long)again, (unsigned long long)clones, (unsigned long long)reads, (unsigned long long)changed,
           (unsigned long long)torn);
    if (old_mode) return;
    for (int c = 0; c < COPIES; c++) CHECK(!busy[c], "copy %d left with %u submissions in flight", c, busy[c]);
    CHECK(!torn, "%llu replays saw their handles change under them", (unsigned long long)torn);
    CHECK(in_place && clones && again, "a path never ran (%llu in place, %llu again in place, %llu clones)",
          (unsigned long long)in_place, (unsigned long long)again, (unsigned long long)clones);
}

// ---- nvn_resolve_binds, one thread
static uint64_t noted, noted_nth[8];
static void note(const p_bind_handles *b, const uint64_t *now, uint64_t nth) {
    if (noted < 8) noted_nth[noted] = nth;
    noted++;
    if (b->h[0] == now[0]) printf("submit_clone_check: FAIL: a note came after the handles were written\n"), fails++;
}

// nvn_copy_again with nvn_copy_enter as nvn_submit_prepare uses them, one thread: occurrence i of handles h stays in
// place (1) or is cloned (0); a clone's out is 0 and leaves its count at once.
static int prepare_one(uint32_t *b, const uint64_t *h, uint64_t *out, int i) {
    out[i] = h[i];
    if (nvn_copy_enter(b) || nvn_copy_again(h, out, i)) return 1;
    nvn_copy_leave(b);
    out[i] = 0;
    return 0;
}

static void steps(void) {
    // a copy given twice in one call with nothing else in flight: both in place, two counts, both left after the replay
    uint32_t b = 0;
    uint64_t h[3] = {11, 22, 11}, out[3];
    int p0 = prepare_one(&b, h, out, 0), p1 = prepare_one(&(uint32_t){0}, h, out, 1), p2 = prepare_one(&b, h, out, 2);
    CHECK(p0 && p1 && p2 && b == 2, "a copy given twice in one call was cloned (%d %d %d, %u in flight)", p0, p1, p2, b);
    nvn_copy_leave(&b), nvn_copy_leave(&b);
    // another thread's submission in flight: both occurrences cloned, the count back to that one's
    b = 1;
    p0 = prepare_one(&b, h, out, 0), p2 = prepare_one(&b, h, out, 2);
    CHECK(!p0 && !p2 && b == 1, "a copy in flight elsewhere was resolved in place (%d %d, %u in flight)", p0, p2, b);
    CHECK(!nvn_copy_again(h, out, 0), "the first occurrence was taken for a repeat");

    static uint64_t arr[4][32], called_arr[32];
    static uint8_t buf[2048], callee[512], chain[18][16];
    for (int i = 0; i < 32; i++) arr[0][i] = 100 + i, arr[1][i] = 200 + i, called_arr[i] = 300 + i;
    uint8_t *q = buf;
    *(op_hdr *)q = (op_hdr){OP_BEGIN, 8}, q += 8;
    uint8_t *changed_rec = q;
    q = bind_rec(q, OP_BIND_TEX, 0, 4, arr[0]);    // its handles differ from the game's: read and written
    uint8_t *same_rec = q;
    q = bind_rec(q, OP_BIND_SAMP, 0, 4, arr[1]);   // made equal below: read, not counted as changed
    uint8_t *nosrc = q;
    q = bind_rec(q, OP_BIND_IMG, 0, 4, NULL);      // immediate: untouched
    uint8_t *zero = q;
    q = bind_rec(q, OP_BIND_TEX, 0, 0, arr[0]);    // n 0: untouched
    uint8_t *big = q;
    q = bind_rec(q, OP_BIND_TEX, 0, 33, arr[0]);   // n over 32: untouched
    *(op_hdr *)q = (op_hdr){OP_CALL, 16}, memcpy(q + 8, &(uint64_t){(uint64_t)(uintptr_t)callee}, 8), q += 16;
    *(op_hdr *)q = (op_hdr){OP_END, 8};
    uint8_t *c = callee;
    *(op_hdr *)c = (op_hdr){OP_BEGIN, 8}, c += 8;
    uint8_t *called_rec = c;
    c = bind_rec(c, OP_BIND_TEX, 8, 2, called_arr);   // reached through the call
    *(op_hdr *)c = (op_hdr){OP_END, 8};
    memcpy(((p_bind_handles *)(same_rec + 8))->h, arr[1], 4 * 8);
    for (int i = 0; i < 32; i++) ((p_bind_handles *)(nosrc + 8))->h[i] = 7, ((p_bind_handles *)(zero + 8))->h[i] = 7;
    ((p_bind_handles *)(big + 8))->h[0] = 7;
    uint64_t rd = 0, ch = 5;
    const uint8_t *bad = NULL;
    int nd = nvn_resolve_binds(buf, 0, &rd, &ch, note, &bad);
    const p_bind_handles *bc = (const p_bind_handles *)(changed_rec + 8), *bs = (const p_bind_handles *)(same_rec + 8);
    const p_bind_handles *bn = (const p_bind_handles *)(nosrc + 8), *bz = (const p_bind_handles *)(zero + 8);
    const p_bind_handles *bb = (const p_bind_handles *)(big + 8), *bk = (const p_bind_handles *)(called_rec + 8);
    CHECK(nd == 3 && rd == 3, "%d deferred binds, %llu read; 3 and 3 wanted", nd, (unsigned long long)rd);
    CHECK(!memcmp(bc->h, arr[0], 4 * 8) && bc->h[4] == 0, "a changed array was not copied in, or more than n");
    CHECK(!memcmp(bs->h, arr[1], 4 * 8), "an unchanged array was changed");
    CHECK(bn->h[0] == 7 && bz->h[0] == 7 && bb->h[0] == 7, "a bind without src, with n 0 or n over 32 was written");
    CHECK(bk->h[0] == 300 && bk->h[1] == 301 && bk->h[2] == 0, "the bind behind OP_CALL was not resolved");
    CHECK(ch == 7 && noted == 2 && noted_nth[0] == 5 && noted_nth[1] == 6, "changed %llu, noted %llu (%llu, %llu): two "
          "changes counted from 5 wanted", (unsigned long long)ch, (unsigned long long)noted,
          (unsigned long long)noted_nth[0], (unsigned long long)noted_nth[1]);
    rd = 0, noted = 0;
    nd = nvn_resolve_binds(buf, 0, &rd, NULL, note, &bad);
    CHECK(nd == 3 && rd == 3 && noted == 0, "a second resolve with nothing changed noted %llu", (unsigned long long)noted);
    // 17 calls deep: the 17th call stops it
    for (int i = 0; i < 17; i++) {
        *(op_hdr *)chain[i] = (op_hdr){OP_CALL, 16};
        memcpy(chain[i] + 8, &(uint64_t){(uint64_t)(uintptr_t)chain[i + 1]}, 8);
    }
    *(op_hdr *)chain[17] = (op_hdr){OP_END, 8};
    bad = NULL;
    CHECK(nvn_resolve_binds(chain[0], 0, NULL, NULL, NULL, &bad) == -1 && bad == chain[16],
          "a call 17 deep was followed (bad %p, chain[16] %p)", (const void *)bad, (const void *)chain[16]);
    *(op_hdr *)chain[16] = (op_hdr){OP_END, 8};
    CHECK(nvn_resolve_binds(chain[0], 0, NULL, NULL, NULL, &bad) == 0, "calls 16 deep were refused");
    uint8_t corrupt[32] = {0};
    *(op_hdr *)corrupt = (op_hdr){OP_BEGIN, 8};
    *(op_hdr *)(corrupt + 8) = (op_hdr){OP_BIND_TEX, 12};   // a size not a multiple of 8
    bad = NULL;
    CHECK(nvn_resolve_binds(corrupt, 0, NULL, NULL, NULL, &bad) == -1 && bad == corrupt + 8,
          "a corrupt record was not reported where it is");
    *(op_hdr *)(corrupt + 8) = (op_hdr){OP_COUNT_, 8};
    CHECK(nvn_resolve_binds(corrupt, 0, NULL, NULL, NULL, &bad) == -1 && bad == corrupt + 8, "an unknown op passed");
}

int main(int argc, char **argv) {
    old_mode = argc > 1 && !strcmp(argv[1], "old");
    if (!old_mode) steps();
    stress();
    if (old_mode) return 0;
    printf("submit_clone_check: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
