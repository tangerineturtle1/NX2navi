// runtime/skin_pair.h: which draw of last frame a skinned draw continues, for the motion vectors of skinned
// characters. Built and run by tests/skin_pair_check.py; prints each failed check and exits 1 on any.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "skin_pair.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// A character of the scene: its mesh and program (key), its place in the frame's command stream, and the two copies
// of its palette block, which the game writes in turn.
typedef struct { uint64_t key, order, copy[2]; uint32_t bytes; } Actor;

static SkinEntry lists[2][16];
static int counts[2], cur_list;

// Draws the actors as frame number f (reached by the replay's threads in the order of `reached`, NULL: as given), pairs
// them with the frame before when `consecutive`, and returns the frame's entries; the entry of actor i is found by tag.
static SkinEntry *frame(const Actor *a, int n, int f, const int *reached, int consecutive) {
    cur_list ^= 1;
    SkinEntry *e = lists[cur_list];
    for (int k = 0; k < n; k++) {
        int i = reached ? reached[k] : k;
        e[k] = (SkinEntry){.key = a[i].key, .order = a[i].order, .palette = a[i].copy[f & 1], .bytes = a[i].bytes,
                           .tag = (uint32_t)i};
    }
    counts[cur_list] = n;
    skin_number(e, n);
    skin_pair(e, n, lists[cur_list ^ 1], counts[cur_list ^ 1], consecutive);
    return e;
}

static const SkinEntry *of_actor(const SkinEntry *e, int n, int i) {
    for (int k = 0; k < n; k++)
        if (e[k].tag == (uint32_t)i) return &e[k];
    return NULL;
}

// The actor the paired entry of last frame belongs to, -1 when the entry is not paired.
static int paired_actor(const SkinEntry *e) {
    return e->why == SKIN_PAIRED ? (int)lists[cur_list ^ 1][e->prev].tag : -1;
}

int main(void) {
    // one character whose palette copies take turns: nothing in its first frame, its place's address is not known
    // in the second, paired from the third on
    Actor one[1] = {{.key = 7, .order = 10, .copy = {0x1000, 0x1400}, .bytes = 960}};
    const SkinEntry *e = frame(one, 1, 1, NULL, 1);
    CHECK(e[0].why == SKIN_NEW && e[0].prev == -1, "a character's first frame is not paired (%d)", e[0].why);
    e = frame(one, 1, 2, NULL, 1);
    CHECK(e[0].why == SKIN_ADDRESS, "its second frame has no address to go by (%d)", e[0].why);
    e = frame(one, 1, 3, NULL, 1);
    CHECK(e[0].why == SKIN_PAIRED && paired_actor(&e[0]) == 0, "its third frame is paired (%d)", e[0].why);
    e = frame(one, 1, 4, NULL, 1);
    CHECK(e[0].why == SKIN_PAIRED, "and stays paired (%d)", e[0].why);
    // a frame without a list in between: last frame's list is not of the frame before
    e = frame(one, 1, 6, NULL, 0);
    CHECK(e[0].why == SKIN_NEW && e[0].prev == -1, "nothing is paired across a gap of a frame (%d)", e[0].why);
    // its two addresses are still known, so the frame after the gap is paired
    e = frame(one, 1, 7, NULL, 1);
    CHECK(e[0].why == SKIN_PAIRED, "the frame after a gap is paired (%d)", e[0].why);
    // written every second frame only: the copy it stays on and the one it moves to are both its own
    Actor slow[1] = {one[0]};
    int ok = 1;
    for (int f = 8; f <= 15; f++) {
        slow[0].copy[f & 1] = one[0].copy[(f >> 1) & 1];
        e = frame(slow, 1, f, NULL, 1);
        ok &= e[0].why == SKIN_PAIRED;
    }
    CHECK(ok, "a block written every second frame stays paired");

    // a block the game does not write again keeps its address: paired in the second frame
    Actor still[1] = {{.key = 9, .order = 1, .copy = {0x9000, 0x9000}, .bytes = 480}};
    memset(lists, 0, sizeof lists), counts[0] = counts[1] = 0;
    frame(still, 1, 1, NULL, 1);
    e = frame(still, 1, 2, NULL, 1);
    CHECK(e[0].why == SKIN_PAIRED, "an unmoved block is paired in its second frame (%d)", e[0].why);

    // two characters of one model: the same key twice in a frame. The numbers follow the command stream, whatever
    // order the replay's threads reached the draws in
    Actor two[2] = {{.key = 5, .order = 20, .copy = {0x2000, 0x2400}, .bytes = 960},
                    {.key = 5, .order = 30, .copy = {0x3000, 0x3400}, .bytes = 960}};
    const int straight[2] = {0, 1}, turned[2] = {1, 0};
    memset(lists, 0, sizeof lists), counts[0] = counts[1] = 0;
    for (int f = 1; f <= 6; f++) {
        e = frame(two, 2, f, f & 1 ? turned : straight, 1);
        const SkinEntry *a = of_actor(e, 2, 0), *b = of_actor(e, 2, 1);
        CHECK(a->nth == 0 && b->nth == 1 && a->of == 2 && b->of == 2, "frame %d: numbered by place in the stream", f);
        if (f >= 3)
            CHECK(paired_actor(a) == 0 && paired_actor(b) == 1, "frame %d: each paired with itself (%d, %d)", f,
                  paired_actor(a), paired_actor(b));
        else
            CHECK(a->why != SKIN_PAIRED && b->why != SKIN_PAIRED, "frame %d: not paired yet", f);
    }
    // they change places in the draw order: each place now holds the other's palette, at an address the place never
    // had, for two frames; then both are paired with themselves again
    two[0].order = 40;
    for (int f = 7; f <= 10; f++) {
        e = frame(two, 2, f, NULL, 1);
        const SkinEntry *a = of_actor(e, 2, 0), *b = of_actor(e, 2, 1);
        if (f <= 8)
            CHECK(a->why == SKIN_ADDRESS && b->why == SKIN_ADDRESS, "frame %d: changed places, not paired (%d, %d)", f,
                  a->why, b->why);
        else
            CHECK(paired_actor(a) == 0 && paired_actor(b) == 1, "frame %d: paired with themselves again", f);
    }

    // a third one of the model appears: every draw of the key is in doubt that frame, and the old two are paired
    // again once the count has held for a frame
    Actor three[3] = {two[0], two[1], {.key = 5, .order = 25, .copy = {0x5000, 0x5400}, .bytes = 960}};
    e = frame(three, 3, 11, NULL, 1);
    for (int i = 0; i < 3; i++) CHECK(of_actor(e, 3, i)->why == SKIN_COUNT, "one appeared: draw %d is not paired", i);
    e = frame(three, 3, 12, NULL, 1);
    for (int i = 0; i < 3; i++) CHECK(of_actor(e, 3, i)->why == SKIN_ADDRESS, "the frame after: draw %d has no address", i);
    e = frame(three, 3, 13, NULL, 1);
    e = frame(three, 3, 14, NULL, 1);
    for (int i = 0; i < 3; i++) CHECK(paired_actor(of_actor(e, 3, i)) == i, "then draw %d is paired with itself", i);
    // one of them leaves
    e = frame(three, 2, 15, NULL, 1);
    CHECK(e[0].why == SKIN_COUNT && e[1].why == SKIN_COUNT, "one left: the others are not paired (%d, %d)", e[0].why, e[1].why);
    // the last one leaves in the frame another one of the model comes: one draw as before, but not the same character
    Actor other[1] = {{.key = 5, .order = 20, .copy = {0x7000, 0x7400}, .bytes = 960}};
    memset(lists, 0, sizeof lists), counts[0] = counts[1] = 0;
    for (int f = 1; f <= 4; f++) frame(one, 1, f, NULL, 1);
    other[0].key = one[0].key;
    e = frame(other, 1, 5, NULL, 1);
    CHECK(e[0].why == SKIN_ADDRESS, "another character in a character's place is not paired (%d)", e[0].why);

    // the palette changes size
    memset(lists, 0, sizeof lists), counts[0] = counts[1] = 0;
    for (int f = 1; f <= 4; f++) frame(one, 1, f, NULL, 1);
    Actor grown[1] = {one[0]};
    grown[0].bytes = 1920;
    e = frame(grown, 1, 5, NULL, 1);
    CHECK(e[0].why == SKIN_SIZE && e[0].prev == -1, "a palette of another size is not paired (%d)", e[0].why);

    // a key that was not drawn, and one in an empty list
    CHECK(skin_find(e, 1, 1234) == -1 && skin_find(e, 0, 7) == -1, "a key not in the list is not found");

    // skin_near: every bone's translation within the limit
    float now[2][12] = {{1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30}, {1, 0, 0, 11, 0, 1, 0, 21, 0, 0, 1, 31}}, then[2][12];
    memcpy(then, now, sizeof now);
    then[1][7] += 0.5f;
    CHECK(skin_near(now, then, sizeof now, 48, 1.0f), "half a unit is near");
    then[1][7] += 1.0f;
    CHECK(!skin_near(now, then, sizeof now, 48, 1.0f), "a unit and a half is not");
    then[1][7] = NAN;
    CHECK(!skin_near(now, then, sizeof now, 48, 1.0f), "a NaN is not near");
    then[1][7] = now[1][7], then[0][0] = -1;   // a turned bone at the same place
    CHECK(skin_near(now, then, sizeof now, 48, 1.0f), "a rotation alone does not count");
    float m4[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1}, n4[16];
    memcpy(n4, m4, sizeof m4);
    n4[13] += 3;
    CHECK(skin_near(m4, m4, sizeof m4, 64, 1.0f) && !skin_near(m4, n4, sizeof m4, 64, 1.0f), "a 4x4 with its translation in the fourth row");
    CHECK(!skin_near(now, now, sizeof now, 16, 1.0f), "a size that is no matrix is not near");

    printf(fails ? "skin_pair_check: %d failed\n" : "skin_pair_check: ok\n", fails);
    return fails != 0;
}
