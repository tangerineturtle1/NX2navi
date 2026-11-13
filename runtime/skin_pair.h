// Which draw of last frame a skinned draw of this frame continues (vk_skin_motion.inc), for the motion vectors of
// skinned characters. The player's meshes are known by ownership records (motion_owner.c); every other character's
// draws have no such record, so a draw is paired with last frame's by what it binds:
//   key      the mesh and the program: the vertex buffers its shader reads, the index buffer, the index count, width
//            and base vertex, and the vertex and fragment shader (the caller hashes them, skin_mix);
//   number   the draw's place among this frame's draws with the same key, counted in the order of the command stream
//            (order), not the order the replay's threads reached them in: two characters of one model share the key.
// A draw is paired with last frame's draw of the same key and number, and with none when anything is in doubt, as a
// vector from another character's pose is worse than the camera's:
//   SKIN_NEW      there was no last frame's list, or the key was not in it (the character appeared);
//   SKIN_COUNT    the key was drawn a different number of times last frame (one appeared or left: the numbers shifted);
//   SKIN_SIZE     the palette had another size;
//   SKIN_ADDRESS  the palette is at neither of the two addresses this key and number last had. The game keeps two
//                 copies of an object's block that take turns, so one object's palette is at last frame's address
//                 (not written again) or at the other copy's, where it was the frame before that. A third address is
//                 another object in this place: two characters of one model changed places in the draw order, or one
//                 left as another came. A new character therefore gets its first vector in its third frame. The two
//                 addresses are remembered across frames without a list, so the frame after such a gap pairs again.
// Own header so tests/skin_pair_check.c checks it without a GPU.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { SKIN_PAIRED, SKIN_NEW, SKIN_COUNT, SKIN_SIZE, SKIN_ADDRESS, SKIN_PAIR_WHYS };

typedef struct {
    uint64_t key;       // the mesh and program
    uint64_t order;     // place in the frame's command stream; unique within a frame
    uint64_t palette;   // address of the palette block
    uint32_t bytes;     // its size
    uint32_t tag;       // the caller's (its index of the draw); the entries are sorted
    // set by skin_number
    uint32_t nth, of;   // number among the frame's draws with this key, and how many there are
    // set by skin_pair
    uint64_t before;    // the last palette address of this key and number other than palette; 0 not known
    int32_t prev;       // index of the paired entry of last frame; -1 none
    uint8_t why;        // SKIN_PAIRED or why not
} SkinEntry;

static inline uint64_t skin_mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

static int skin_entry_cmp(const void *a, const void *b) {
    const SkinEntry *x = a, *y = b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

// Sorts a frame's entries by key and stream order and numbers the draws of each key.
static inline void skin_number(SkinEntry *e, int n) {
    qsort(e, (size_t)n, sizeof *e, skin_entry_cmp);
    for (int i = 0; i < n;) {
        int j = i;
        while (j < n && e[j].key == e[i].key) j++;
        for (int k = i; k < j; k++) e[k].nth = (uint32_t)(k - i), e[k].of = (uint32_t)(j - i);
        i = j;
    }
}

// The first entry with key in a numbered list, -1 when there is none; its .of entries follow each other.
static inline int skin_find(const SkinEntry *e, int n, uint64_t key) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (e[mid].key < key) lo = mid + 1;
        else hi = mid;
    }
    return lo < n && e[lo].key == key ? lo : -1;
}

// Pairs this frame's numbered entries with those of the last list made (numbered and paired in its own frame).
// consecutive: that list is of the frame right before this one; of an older one nothing is paired, and only the
// addresses are carried on.
static inline void skin_pair(SkinEntry *cur, int ncur, const SkinEntry *last, int nlast, int consecutive) {
    for (int i = 0; i < ncur; i++) {
        SkinEntry *c = &cur[i];
        c->prev = -1, c->before = 0, c->why = SKIN_NEW;
        int p = skin_find(last, nlast, c->key);
        if (p < 0) continue;
        if (last[p].of != c->of) {
            if (consecutive) c->why = SKIN_COUNT;
            continue;
        }
        const SkinEntry *l = &last[p + (int)c->nth];
        if (l->bytes != c->bytes) {
            if (consecutive) c->why = SKIN_SIZE;
            continue;
        }
        c->before = c->palette != l->palette ? l->palette : l->before;
        if (!consecutive) continue;
        if (c->palette != l->palette && c->palette != l->before) {
            c->why = SKIN_ADDRESS;
            continue;
        }
        c->prev = p + (int)c->nth, c->why = SKIN_PAIRED;
    }
}

// Whether a palette is last frame's a little moved: every bone's translation within limit units of its last one. The
// matrices are rows of four floats with the translation last in each row (floats 3, 7 and 11); a 4x4 may hold it in
// its fourth row instead (12 to 14), so both places are compared, and the one that is not the translation holds
// constants. For keys drawn more than once a frame: two characters of one model whose draw order alternates frame by
// frame pass the address rule of skin_pair, and stand apart.
static inline int skin_near(const void *now, const void *then, uint32_t bytes, uint32_t stride, float limit) {
    static const uint8_t at[6] = {3, 7, 11, 12, 13, 14};
    int places = stride >= 64 ? 6 : 3;
    if (stride < 48) return 0;
    for (uint32_t o = 0; o + stride <= bytes; o += stride) {
        float a[16], b[16];
        memcpy(a, (const uint8_t *)now + o, stride < 64 ? 48 : 64);
        memcpy(b, (const uint8_t *)then + o, stride < 64 ? 48 : 64);
        for (int k = 0; k < places; k++) {
            float d = a[at[k]] - b[at[k]];
            if (!(d <= limit && d >= -limit)) return 0;   // a NaN is not near
        }
    }
    return 1;
}
