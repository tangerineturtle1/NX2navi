// runtime/vk_up_defer.h: what the upscalers let go of is released only once the GPU timeline has passed its last
// submission and, for what a present can read, once enough presents went by. Built and run as C and as C++ by
// tests/up_defer_check.py; prints each failed check and exits 1 on any.
#include <stdio.h>
#include <string.h>

#include "vk_up_defer.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int got[64], ngot;
static UpdList *reenter;   // a callback that puts another entry (a release that lets go of something else)
static void rel(const UpdEntry *e, void *arg) {
    (void)arg;
    got[ngot++] = (int)(intptr_t)e->handle;
    if (reenter && e->kind == 9) upd_put(reenter, 1, (void *)(intptr_t)99, NULL, 0, 0);
}
static void *h(int i) { return (void *)(intptr_t)i; }

int main(void) {
    UpdList l;
    memset(&l, 0, sizeof l);
    // a DLSS feature last evaluated in submission 10, a context last dispatched in 12, a never-used one (fence 0)
    CHECK(upd_put(&l, 1, h(1), NULL, 10, 0) && upd_put(&l, 2, h(2), NULL, 12, 0) && upd_put(&l, 2, h(3), NULL, 0, 0),
          "puts into an empty list");
    CHECK(upd_put(&l, 1, NULL, NULL, 5, 0) && l.n == 3, "a null handle is nothing to queue, and no failure");
    ngot = 0;
    CHECK(upd_take(&l, 9, 0, rel, NULL) == 1 && ngot == 1 && got[0] == 3, "at 9 only the never-used one goes");
    ngot = 0;
    CHECK(upd_take(&l, 10, 0, rel, NULL) == 1 && got[0] == 1, "at 10 the feature of 10 goes");
    CHECK(l.n == 1 && l.e[0].handle == h(2), "the context of 12 stays, in place");
    ngot = 0;
    CHECK(upd_take(&l, 12, 0, rel, NULL) == 1 && got[0] == 2 && l.n == 0, "at 12 it goes");
    // an image a present may read: the fence alone does not free it, the present count must pass too
    upd_put(&l, 3, h(4), NULL, 0, 108);
    ngot = 0;
    CHECK(upd_take(&l, 1000, 107, rel, NULL) == 0, "the present gate holds at 107");
    CHECK(upd_take(&l, 1000, 108, rel, NULL) == 1 && got[0] == 4, "and opens at 108");
    // both gates: the later one decides
    upd_put(&l, 3, h(5), NULL, 50, 200);
    CHECK(upd_take(&l, 49, 300, rel, NULL) == 0 && upd_take(&l, 60, 199, rel, NULL) == 0, "both gates must pass");
    ngot = 0;
    CHECK(upd_take(&l, 50, 200, rel, NULL) == 1 && got[0] == 5, "both passed");
    // order: the ready ones in the order put, the rest kept in order
    for (int i = 0; i < 6; i++) upd_put(&l, 1, h(10 + i), NULL, i % 2 ? 100 : 20, 0);
    ngot = 0;
    upd_take(&l, 20, 0, rel, NULL);
    CHECK(ngot == 3 && got[0] == 10 && got[1] == 12 && got[2] == 14, "ready ones in order (%d)", ngot);
    CHECK(l.n == 3 && l.e[0].handle == h(11) && l.e[1].handle == h(13) && l.e[2].handle == h(15), "the rest in order");
    CHECK(upd_max_fence(&l) == 100 && upd_count(&l, 1) == 3 && upd_count(&l, 2) == 0 && upd_count(&l, -1) == 3,
          "max fence and counts");
    // take_all: the device is idle, everything goes whatever its gates
    upd_put(&l, 3, h(20), NULL, 0, UINT64_MAX - 1);
    ngot = 0;
    CHECK(upd_take_all(&l, rel, NULL) == 4 && l.n == 0 && got[3] == 20, "take_all empties the list");
    CHECK(upd_max_fence(&l) == 0 && upd_count(&l, -1) == 0, "an empty list has no fence and no entry");
    // full: the caller is told, and nothing is overwritten
    int ok = 1;
    for (int i = 0; i < UPD_SLOTS; i++) ok &= upd_put(&l, 1, h(100 + i), NULL, 7, 0);
    CHECK(ok && !upd_put(&l, 1, h(999), NULL, 7, 0) && l.n == UPD_SLOTS, "a full list refuses");
    CHECK(l.e[UPD_SLOTS - 1].handle == h(100 + UPD_SLOTS - 1), "and keeps its last entry");
    ngot = 0;
    upd_take(&l, 7, 0, rel, NULL);
    CHECK(ngot == UPD_SLOTS && l.n == 0, "and empties at its fence");
    // a callback may put: the new entry waits for the next take
    reenter = &l;
    upd_put(&l, 9, h(30), NULL, 1, 0);
    ngot = 0;
    upd_take(&l, 5, 0, rel, NULL);
    CHECK(ngot == 1 && l.n == 1 && l.e[0].handle == h(99), "an entry put by a callback stays for the next take");
    reenter = NULL;
    ngot = 0;
    upd_take(&l, 5, 0, rel, NULL);
    CHECK(ngot == 1 && got[0] == 99, "and goes then");
    CHECK(l.put == l.released, "every entry put was released (%llu, %llu)", (unsigned long long)l.put,
          (unsigned long long)l.released);
    if (!fails) printf("up_defer_check: passed\n");
    return fails ? 1 : 0;
}
