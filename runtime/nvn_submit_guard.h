// Contains code Copyright (c) 2026 jhaizhou (MIT; see THIRD_PARTY_NOTICES.md).
// The deferred binds of a recording copy at a synchronous submission, and what keeps a second submission of the same
// copy, made by another guest thread while the first is replayed, from rewriting it (nvn_api_cmd.c
// nvn_submit_prepare, nvn_submit_done). Header only, so that tests/submit_clone_check.c checks it without a device.
//
// A deferred bind takes the handles the game's array holds at the submit call, and they are written into the recording
// copy (nvn_resolve_binds). With the replay on the submitting thread, a second thread that submitted the same copy
// while the first one replayed it, or copied it for a held continuation (vk_exec.c cont_new), rewrote the handles that
// replay was reading: textures and samplers of the next frame halfway through it. So each copy counts the submissions
// between their prepare and the end of their replay (busy): the first one resolves in place, as before; one that finds
// another in flight resolves a clone of its own, freed after its replay, as the asynchronous path does for a copy a
// queued submission still reads. A clone taken while the first thread still resolves in place may hold a mix of old and
// new handles in its binds' arrays only, and its own resolve then writes every deferred array again: it ends whole.
#pragma once
#include <stdint.h>
#include <string.h>

#include "nvn_obj.h"

// 1: no other submission has the copy in flight; resolve it in place and call nvn_copy_leave after the replay. 0:
// another one has, or this call holds it already (nvn_copy_again); else take a clone and call nvn_copy_leave once the
// clone is made.
static inline int nvn_copy_enter(uint32_t *busy) { return __atomic_fetch_add(busy, 1, __ATOMIC_ACQ_REL) == 0; }
static inline void nvn_copy_leave(uint32_t *busy) { __atomic_fetch_sub(busy, 1, __ATOMIC_RELEASE); }

// After nvn_copy_enter returned 0 for handles[i] of one submit call: whether the same handle came earlier in this call
// and resolves in place there (out[j] == handles[j]). Then the count that made it busy is this thread's own: the copy
// is this submission's to resolve in place, and another thread that submits it meanwhile takes a clone. So this
// occurrence stays in place too: its count is left after the replay like the first one's (nvn_submit_done, per
// occurrence), and no clone is made or logged for it. The
// asynchronous path keeps a repeat in one call in place by its own rule (nvn_api_cmd.c nvn_submit_prepare_async:
// used_at != now).
static inline int nvn_copy_again(const uint64_t *handles, const uint64_t *out, int i) {
    for (int j = 0; j < i; j++)
        if (handles[j] == handles[i] && out[j] == handles[j]) return 1;
    return 0;
}

// Told of a bind whose handles changed, before they are written: b holds the handles at record, now the game's, nth
// how many changed binds came before this one (the changed count).
typedef void (*NvnBindNote)(const p_bind_handles *b, const uint64_t *now, uint64_t nth);

// Deferred binds of the recording at p take the handles the game's arrays hold now: OP_BIND_TEX, OP_BIND_IMG and
// OP_BIND_SAMP with src set and 0 < n <= 32, through OP_JUMP and OP_CALL (16 calls deep at most). reads and changed
// count the binds read and those whose handles changed (atomic adds; either may be NULL); note may be NULL. Returns
// how many deferred binds the recording has, or -1 with *bad at the record that stopped it: a corrupt one, or a call
// past the depth limit.
static inline int nvn_resolve_binds(const uint8_t *p, int depth, uint64_t *reads, uint64_t *changed, NvnBindNote note,
                                    const uint8_t **bad) {
    int nd = 0;
    for (;;) {
        op_hdr h;
        memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) return *bad = p, -1;
        if (h.op == OP_END) return nd;
        if (h.op == OP_JUMP) {
            memcpy(&p, p + 8, 8);
            continue;
        }
        if (h.op == OP_CALL) {
            uint64_t t;
            memcpy(&t, p + 8, 8);
            if (depth >= 16) return *bad = p, -1;
            int k = nvn_resolve_binds((const uint8_t *)(uintptr_t)t, depth + 1, reads, changed, note, bad);
            if (k < 0) return k;
            nd += k;
        } else if (h.op == OP_BIND_TEX || h.op == OP_BIND_IMG || h.op == OP_BIND_SAMP) {
            p_bind_handles *b = (p_bind_handles *)(uintptr_t)(p + 8);
            if (b->src && b->n > 0 && b->n <= 32) {
                nd++;
                uint64_t cur[32];
                memcpy(cur, (const void *)(uintptr_t)b->src, 8 * (size_t)b->n);
                if (reads) __atomic_fetch_add(reads, 1, __ATOMIC_RELAXED);
                if (memcmp(cur, b->h, 8 * (size_t)b->n)) {
                    uint64_t nth = changed ? __atomic_fetch_add(changed, 1, __ATOMIC_RELAXED) : 0;
                    if (note) note(b, cur, nth);
                    memcpy(b->h, cur, 8 * (size_t)b->n);
                }
            }
        }
        p += h.size;
    }
}
