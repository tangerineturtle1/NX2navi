// Texture memory compaction, the part without a GPU: from a table of what lies in the allocator's blocks, how much of
// them is holes that could go back to the driver, and which blocks to empty for it. vk_texture.c ("compaction")
// builds the table and does what the plan says; tests/tex_compact_check.py runs this file alone.
//
// The allocator hands out device memory in blocks (vk_headroom.c vk_vma_block_size) and gives a block back to the
// driver only when its last allocation went. Textures stream in and out all through a session, so after a while most
// blocks hold a few live allocations between holes, and the driver counts every such block whole. A block comes back
// when what is left in it goes elsewhere: a texture that only holds what the game's memory holds too can be copied
// into a hole of a fuller block (move) or dropped and made again when it is next used (drop). Anything else stays,
// and a block with one such allocation cannot be emptied.
//
// The table lists the allocations the caller knows: every texture with memory of its own, and whatever else it can
// name (the VRAM mirrors). A block may hold more than that, so the listed bytes of a block are a lower bound of
// what it holds, and "size less listed" an upper bound of its holes. Both errors are on the safe side for the room
// check: the free bytes outside the chosen blocks are taken as the allocator's total less everything the chosen blocks
// could hold free. What the plan cannot see is an unlisted allocation in a chosen block; such a block is emptied of
// its textures and stays, and the caller marks it held for the next plans.
#pragma once
#include <stdint.h>
#include <string.h>

enum { TCP_STAY, TCP_MOVE, TCP_DROP };

// One listed allocation: the index of its block, its size, and what may be done with it.
typedef struct {
    uint32_t block;
    uint8_t what;
    uint64_t bytes;
} TcpAlloc;

typedef struct {
    uint64_t id;     // the caller's name for the block (its device memory handle)
    uint64_t size;
    uint8_t held;    // not to be chosen: emptied before and not given back
    // filled by tcp_tally
    uint64_t move, drop, stay;          // listed bytes by what may be done with them
    uint32_t n_move, n_drop, n_stay;
    uint8_t victim;                     // tcp_plan chose it
} TcpBlock;

typedef struct {
    uint64_t free_bytes;    // free bytes inside all blocks of the memory type, as the allocator counts them
    uint32_t sparse_pct;    // a block is chosen only when its listed allocations fill at most this share of it
    uint32_t room_pct;      // the moves may take at most this share of the free bytes outside the chosen blocks
    uint64_t min_gain;      // no plan unless the chosen blocks are at least this large together
    uint64_t max_move;      // bytes one plan moves at most
    uint32_t max_blocks;    // blocks one plan chooses at most
} TcpParams;

// What the table holds, for the log: every listed block, those at most sparse_pct full, and those of them a plan could
// choose (nothing listed in them stays, and they are not held).
typedef struct {
    uint32_t blocks, sparse, emptiable;
    uint64_t block_bytes, listed_bytes;             // all listed blocks: their sizes, the allocations listed in them
    uint64_t move_bytes, drop_bytes, stay_bytes;    // the listed allocations by kind
    uint64_t sparse_bytes, sparse_listed;           // the sparse blocks: sizes, listed allocations
    uint64_t emptiable_bytes, emptiable_move, emptiable_drop;
} TcpStats;

typedef struct {
    uint32_t n;                 // blocks chosen
    uint64_t gain;              // their sizes: what goes back when each empties
    uint64_t move, drop;        // bytes to move and to drop for it
    uint32_t n_move, n_drop;
} TcpPlan;

// Sums the allocations of a[] into their blocks. An allocation naming no block of b[] is left out.
static inline void tcp_tally(TcpBlock *b, int nb, const TcpAlloc *a, int na) {
    for (int i = 0; i < nb; i++) {
        b[i].move = b[i].drop = b[i].stay = 0;
        b[i].n_move = b[i].n_drop = b[i].n_stay = 0;
        b[i].victim = 0;
    }
    for (int i = 0; i < na; i++) {
        if (a[i].block >= (uint32_t)nb) continue;
        TcpBlock *k = &b[a[i].block];
        if (a[i].what == TCP_MOVE) k->move += a[i].bytes, k->n_move++;
        else if (a[i].what == TCP_DROP) k->drop += a[i].bytes, k->n_drop++;
        else k->stay += a[i].bytes, k->n_stay++;
    }
}

static inline uint64_t tcp_listed(const TcpBlock *k) { return k->move + k->drop + k->stay; }

// At most pct percent of the block is listed allocations. A block listed fuller than its size (a wrong size from the
// caller) is not sparse.
static inline int tcp_sparse(const TcpBlock *k, uint32_t pct) {
    uint64_t l = tcp_listed(k);
    return k->size && l <= k->size && l * 100 <= k->size * (uint64_t)pct;
}

// Whether a plan may choose the block: sparse, something in it to move or drop, nothing that stays, not held.
static inline int tcp_emptiable(const TcpBlock *k, uint32_t pct) {
    return !k->held && !k->n_stay && (k->n_move || k->n_drop) && tcp_sparse(k, pct);
}

// After tcp_tally.
static inline void tcp_stats(const TcpBlock *b, int nb, uint32_t sparse_pct, TcpStats *s) {
    memset(s, 0, sizeof *s);
    for (int i = 0; i < nb; i++) {
        const TcpBlock *k = &b[i];
        s->blocks++, s->block_bytes += k->size, s->listed_bytes += tcp_listed(k);
        s->move_bytes += k->move, s->drop_bytes += k->drop, s->stay_bytes += k->stay;
        if (!tcp_sparse(k, sparse_pct)) continue;
        s->sparse++, s->sparse_bytes += k->size, s->sparse_listed += tcp_listed(k);
        if (!tcp_emptiable(k, sparse_pct)) continue;
        s->emptiable++, s->emptiable_bytes += k->size, s->emptiable_move += k->move, s->emptiable_drop += k->drop;
    }
}

// After tcp_tally: chooses the blocks to empty, the one that takes the fewest moved bytes first (of two alike, the
// larger), while what must move still fits the room outside the chosen blocks, and marks them (TcpBlock.victim).
// order[0..n) gets their indices in the order chosen, n at most max_order; *out the sums. Returns n, 0 with nothing
// marked when the chosen blocks together are smaller than min_gain.
static inline int tcp_plan(TcpBlock *b, int nb, const TcpParams *p, uint32_t *order, int max_order, TcpPlan *out) {
    memset(out, 0, sizeof *out);
    for (int i = 0; i < nb; i++) b[i].victim = 0;
    uint64_t inside = 0;   // the most the chosen blocks can hold free: their sizes less what is listed in them
    uint32_t most = p->max_blocks < (uint32_t)(max_order > 0 ? max_order : 0) ? p->max_blocks : (uint32_t)(max_order > 0 ? max_order : 0);
    // b[i].victim: 0 not looked at, 1 chosen, 2 looked at and left (no room for it)
    while (out->n < most) {
        int best = -1;
        for (int i = 0; i < nb; i++) {
            if (b[i].victim || !tcp_emptiable(&b[i], p->sparse_pct)) continue;
            if (best < 0 || b[i].move < b[best].move || (b[i].move == b[best].move && b[i].size > b[best].size)) best = i;
        }
        if (best < 0) break;
        TcpBlock *k = &b[best];
        uint64_t in = inside + (k->size - tcp_listed(k)), need = out->move + k->move;
        uint64_t outside = p->free_bytes > in ? p->free_bytes - in : 0;
        if (need > p->max_move || need * 100 > outside * (uint64_t)p->room_pct) {
            k->victim = 2;
            continue;
        }
        k->victim = 1;
        inside = in;
        order[out->n++] = (uint32_t)best;
        out->gain += k->size, out->move += k->move, out->drop += k->drop;
        out->n_move += k->n_move, out->n_drop += k->n_drop;
    }
    for (int i = 0; i < nb; i++)
        if (b[i].victim == 2 || out->gain < p->min_gain) b[i].victim = 0;
    if (out->gain < p->min_gain) memset(out, 0, sizeof *out);
    return (int)out->n;
}
