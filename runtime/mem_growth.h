// The memory report's growth bookkeeping, the part without a GPU: which of the report's figures grew and by how much,
// since the previous report and since the first report in play, and which of them only ever climbed. vk_memory.c
// (growth_log, growth_rank_log) feeds it the figures of each report and logs what these functions write;
// tests/mem_growth_check.py runs this file alone.
//
// A figure is a byte count the report gives (device-local memory, textures, the process's commit, the game's heap, the
// shader caches, ...), one category each. The first report taken once the game is in play (the loaded save's world
// clock runs, frame_step_world_us) is the base of the session: what the boot and the title screen took is no growth.
// Before it a report is compared with the one before it only. A category "only climbs" when, from the base on, it rose
// by at least MG_NOISE against the report before at least once and never fell by that much: a figure that comes down
// again after an area change is a cache doing its work, one that never does is a leak candidate.
#pragma once
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MG_MAX 32                 // categories at most
#define MG_NOISE (1ull << 20)     // a change smaller than this between two reports is neither a rise nor a fall

typedef struct {
    const char *name;
    uint64_t first;               // at the base (the first report in play)
    uint64_t prev, last;          // at the report before the last, and at the last
    uint32_t ups, downs;          // reports from the base on that rose or fell by MG_NOISE or more against the one before
} MgCat;

typedef struct {
    MgCat c[MG_MAX];
    int n;
    uint32_t lines;               // reports taken
    uint32_t play_lines;          // of them from the base on, the base included
    int play;                     // the base is taken
} MgBook;

// A new category; its index, -1 when the book is full. Categories are added before the first report.
static inline int mg_cat(MgBook *b, const char *name) {
    if (b->n >= MG_MAX) return -1;
    memset(&b->c[b->n], 0, sizeof b->c[b->n]);
    b->c[b->n].name = name;
    return b->n++;
}

// A report's figures, v[i] for category i. in_play: the game is in play now; the first such report is the base, and
// later ones count as in play whatever in_play says (a return to the title screen does not end the session).
static inline void mg_take(MgBook *b, const uint64_t *v, int in_play) {
    int base = !b->play && in_play;
    for (int i = 0; i < b->n; i++) {
        MgCat *c = &b->c[i];
        c->prev = b->lines ? c->last : v[i];
        c->last = v[i];
        if (base) c->first = v[i];
        else if (b->play && c->last >= c->prev + MG_NOISE) c->ups++;
        else if (b->play && c->prev >= c->last + MG_NOISE) c->downs++;
    }
    if (base) b->play = 1;
    b->lines++;
    if (b->play) b->play_lines++;
}

static inline int64_t mg_since_prev(const MgCat *c) { return (int64_t)(c->last - c->prev); }
static inline int64_t mg_since_first(const MgBook *b, const MgCat *c) { return b->play ? (int64_t)(c->last - c->first) : 0; }
static inline int mg_only_climbs(const MgBook *b, const MgCat *c) { return b->play && c->ups && !c->downs; }

// Appends printf output at out + *k, never past cap; *k counts what would have been written, so a full buffer stays
// cut at its end (and the caller can tell by *k >= cap).
__attribute__((format(printf, 4, 5))) static inline void mg_put(char *out, size_t cap, size_t *k, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*k < cap ? out + *k : NULL, *k < cap ? cap - *k : 0, fmt, ap);
    va_end(ap);
    if (n > 0) *k += (size_t)n;
}

// The growth since the last two reports, for one log line: "since the previous report: textures +312 MiB, process
// commit +420 MiB; since play began: textures +3800 MiB (only climbs), ...". A category is named in the first list when
// it grew by more than step since the report before the last, in the second when it grew by more than step since the
// base; the second list is there from the base on. Writes nothing when no category is named. Returns the number of
// categories named in either list; out holds a string whenever cap > 0.
static inline int mg_grew(const MgBook *b, uint64_t step, char *out, size_t cap) {
    size_t k = 0;
    int named = 0, any;
    if (cap) out[0] = 0;
    any = 0;
    for (int i = 0; i < b->n; i++) {
        const MgCat *c = &b->c[i];
        int64_t d = mg_since_prev(c);
        if (b->lines < 2 || d <= (int64_t)step) continue;
        mg_put(out, cap, &k, "%s%s %+.0f MiB", any ? ", " : "since the previous report: ", c->name, d / 1048576.0);
        any = 1, named++;
    }
    int listed = any;
    any = 0;
    for (int i = 0; i < b->n; i++) {
        const MgCat *c = &b->c[i];
        int64_t d = mg_since_first(b, c);
        if (b->play_lines < 2 || d <= (int64_t)step) continue;
        mg_put(out, cap, &k, "%s%s %+.0f MiB%s", any ? ", " : listed ? "; since play began: " : "since play began: ",
               c->name, d / 1048576.0, mg_only_climbs(b, c) ? " (only climbs)" : "");
        any = 1, named++;
    }
    if (cap && k >= cap) out[cap - 1] = 0;
    return named;
}

// The categories ranked by their growth since the base, the largest first: those that grew by MG_NOISE or more, each
// marked when it only climbs, then those that fell by that much, then how many stayed within MG_NOISE. Ties keep the
// order the categories were added in. Returns the number of categories that grew; 0 with nothing written before the
// base.
static inline int mg_rank(const MgBook *b, char *out, size_t cap) {
    size_t k = 0;
    if (cap) out[0] = 0;
    if (!b->play) return 0;
    int order[MG_MAX], n = 0;
    for (int i = 0; i < b->n; i++) {   // insertion sort by growth, stable
        int64_t d = mg_since_first(b, &b->c[i]);
        int j = n++;
        while (j > 0 && mg_since_first(b, &b->c[order[j - 1]]) < d) order[j] = order[j - 1], j--;
        order[j] = i;
    }
    int grew = 0, fell = 0, flat = 0;
    for (int j = 0; j < n; j++) {
        const MgCat *c = &b->c[order[j]];
        int64_t d = mg_since_first(b, c);
        if (d < (int64_t)MG_NOISE) continue;
        mg_put(out, cap, &k, "%s%s %+.0f MiB%s", grew ? ", " : "", c->name, d / 1048576.0,
               mg_only_climbs(b, c) ? " (only climbs)" : "");
        grew++;
    }
    if (!grew) mg_put(out, cap, &k, "nothing grew");
    for (int j = n - 1; j >= 0; j--) {   // the largest fall first
        const MgCat *c = &b->c[order[j]];
        int64_t d = mg_since_first(b, c);
        if (d > -(int64_t)MG_NOISE) continue;
        mg_put(out, cap, &k, "%s%s %+.0f MiB", fell ? ", " : "; fell: ", c->name, d / 1048576.0);
        fell++;
    }
    for (int j = 0; j < n; j++) {
        int64_t d = mg_since_first(b, &b->c[order[j]]);
        flat += d < (int64_t)MG_NOISE && d > -(int64_t)MG_NOISE;
    }
    if (flat) mg_put(out, cap, &k, "; within 1 MiB of the base: %d more", flat);
    if (cap && k >= cap) out[cap - 1] = 0;
    return grew;
}
