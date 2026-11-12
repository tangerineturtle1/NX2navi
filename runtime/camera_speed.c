// Camera speed: NX2NAVI_STICK_SENS (0.1 to 3, default 1) multiplies the game's own right-stick camera speeds.
// The game's Options menu offers five camera sensitivities, Very Low to Very High; their turn rates are the array
// StickSensitivity {0.8, 1.2, 1.6, 2.0, 2.4} in Component/PlayerCameraParam/PlayerCamera.game__component__
// PlayerCameraParam.bgyml inside Pack/Actor/PlayerCamera.pack.zs. romfs.c serves a copy of that pack with every entry
// multiplied (cam_pack_build), so the setting raises the camera's top speed as well as its speed at a partial tilt, at
// any of the five sensitivities. The mouse camera reaches the game as a right-stick deflection (input.c, mouse_camera),
// divided by the same factor (cam_speed_applied) before it is limited to full tilt (input_mouse_stick), so
// NX2NAVI_MOUSE_SENS alone sets the mouse's turn per movement, and its top speed is the game's with the factor in it.
//
// The pack is a SARC archive compressed with the dictionary pack.zsdic of Pack/ZsDic.pack.zs. A float in a BYML array
// is stored in the array itself, so the multiplied values take the bytes of the old ones and no offset moves.
//
// During a run (cam_speed_tick): the pack served stays the one made at start, and the game has read it by the time the
// player changes the setting. So a change is written into the game's memory instead: the array as the game holds it
// (the five values in a row, times the factor in effect) is looked for in the guest's heap and every copy found is
// overwritten with the values times the new factor. The game reads the array each time it steps the camera, so the
// camera's speed follows at once. When the game opens the pack again (cam_pack_opened: romfs.c), what it reads holds
// the start's factor again, and the search runs once more a moment later.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "sarc.h"

#define MEMBER "Component/PlayerCameraParam/PlayerCamera.game__component__PlayerCameraParam.bgyml"
#define MAX_RAW (64u << 20)

static float applied = 1.0f;       // the factor the game's values in memory carry: what cam_speed_applied reports
static float base[8];              // the game's own StickSensitivity values, from the pack as it is before multiplying
static int base_n;                 // how many; 0 until cam_pack_build has read the pack (or when it could not)
static float served_k = 1.0f;      // the factor of the pack the game reads from the RomFS: the start's

float cam_speed_wanted(void) {
    double v = cfg_double("NX2NAVI_STICK_SENS");
    return (float)(v < 0.1 ? 0.1 : v > 3 ? 3 : v);
}

float cam_speed_applied(void) { return applied; }

static int u32_at(const uint8_t *p, size_t n, size_t off, uint32_t *v) {
    if (off > n || n - off < 4) return 0;
    memcpy(v, p + off, 4);
    return 1;
}

// The StickSensitivity floats of the BYML p[0..n), multiplied by k; their count, 0 when the file has no such array.
static int scale_sensitivity(uint8_t *p, size_t n, float k, float was[8]) {
    uint32_t keys, root, head;
    if (n < 16 || memcmp(p, "YB", 2) || !u32_at(p, n, 4, &keys) || !u32_at(p, n, 12, &root) ||
        !u32_at(p, n, root, &head) || (head & 0xff) != 0xc1 || !u32_at(p, n, keys, &head) || (head & 0xff) != 0xc2)
        return 0;
    uint32_t nkeys = head >> 8;
    if (!u32_at(p, n, root, &head)) return 0;
    for (uint32_t i = 0; i < head >> 8; i++) {
        uint32_t e, at, arr, ah;
        if (!u32_at(p, n, root + 4 + 8 * (size_t)i, &e) || (e & 0xffffff) >= nkeys ||
            !u32_at(p, n, keys + 4 + 4 * (size_t)(e & 0xffffff), &at))
            return 0;
        // in size_t: a mod's key offset near 2^32 would wrap the 32-bit sum below n
        if ((size_t)keys + at + sizeof "StickSensitivity" > n) continue;
        const char *name = (const char *)p + (size_t)keys + at;
        if (strcmp(name, "StickSensitivity")) continue;
        if (e >> 24 != 0xc0 || !u32_at(p, n, root + 8 + 8 * (size_t)i, &arr) || !u32_at(p, n, arr, &ah) ||
            (ah & 0xff) != 0xc0)
            return 0;
        uint32_t count = ah >> 8;
        size_t vals = arr + 4 + ((count + 3) & ~3u);
        if (!count || count > 8 || vals + 4 * (size_t)count > n) return 0;
        for (uint32_t j = 0; j < count; j++)
            if (p[arr + 4 + j] != 0xd2) return 0;
        for (uint32_t j = 0; j < count; j++) {
            float v;
            memcpy(&v, p + vals + 4 * j, 4);
            was[j] = v;
            v *= k;
            memcpy(p + vals + 4 * j, &v, 4);
        }
        return (int)count;
    }
    return 0;
}

int cam_pack_build(const void *in, size_t n, const void *zsdic, size_t zsdic_n, void **out, size_t *out_n) {
    float k = cam_speed_wanted();
    *out = NULL;
    int ok = 0;
    uint8_t *dics = NULL, *raw = NULL;
    ZSTD_DDict *dd = NULL;
    ZSTD_CDict *cd = NULL;
    void *z = NULL;
    // The frame names its dictionary (pack.zsdic) by id; Pack/ZsDic.pack.zs holds them.
    unsigned id = ZSTD_getDictID_fromFrame(in, n);
    size_t dn, dic_at, dic_n;
    if (!id || !(dics = zsdic_unpack(zsdic, zsdic_n, MAX_RAW, &dn)) || !zsdic_find(dics, dn, id, &dic_at, &dic_n)) {
        rt_log("camera speed: Pack/ZsDic.pack.zs has no dictionary %u for Pack/Actor/PlayerCamera.pack.zs; the camera "
               "keeps its own speed", id);
        goto done;
    }
    unsigned long long rn = ZSTD_getFrameContentSize(in, n);
    dd = ZSTD_createDDict(dics + dic_at, dic_n);
    if (!dd || rn == ZSTD_CONTENTSIZE_UNKNOWN || rn == ZSTD_CONTENTSIZE_ERROR || rn > MAX_RAW ||
        !(raw = malloc((size_t)rn))) {
        rt_log("camera speed: Pack/Actor/PlayerCamera.pack.zs is not a zstd frame with a size; the camera keeps its "
               "own speed");
        goto done;
    }
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    size_t got = dctx ? ZSTD_decompress_usingDDict(dctx, raw, (size_t)rn, in, n, dd) : 0;
    ZSTD_freeDCtx(dctx);
    size_t at, size;
    float was[8];
    int count = !ZSTD_isError(got) && got == rn && sarc_member(raw, (size_t)rn, MEMBER, &at, &size)
                    ? scale_sensitivity(raw + at, size, k, was) : 0;
    if (!count) {
        rt_log("camera speed: no StickSensitivity array in %s; the camera keeps its own speed", MEMBER);
        goto done;
    }
    memcpy(base, was, sizeof(float) * (size_t)count);   // what a change during the run looks for in memory (cam_speed_tick)
    base_n = count;
    if (k == 1.0f) goto done;   // the game's own pack is served as it is
    cd = ZSTD_createCDict(dics + dic_at, dic_n, 9);
    size_t cap = ZSTD_compressBound((size_t)rn), zn = 0;
    ZSTD_CCtx *cctx = cd ? ZSTD_createCCtx() : NULL;
    if (cctx && (z = malloc(cap))) zn = ZSTD_compress_usingCDict(cctx, z, cap, raw, (size_t)rn, cd);
    ZSTD_freeCCtx(cctx);
    if (!z || !zn || ZSTD_isError(zn)) goto done;
    *out = z;
    *out_n = zn;
    z = NULL;
    ok = 1;
    rt_log("camera speed: x%.2f, StickSensitivity %.2f %.2f %.2f %.2f %.2f -> %.2f %.2f %.2f %.2f %.2f", k, was[0],
           count > 1 ? was[1] : 0, count > 2 ? was[2] : 0, count > 3 ? was[3] : 0, count > 4 ? was[4] : 0, was[0] * k,
           count > 1 ? was[1] * k : 0, count > 2 ? was[2] * k : 0, count > 3 ? was[3] * k : 0,
           count > 4 ? was[4] * k : 0);
done:
    free(z);
    free(raw);
    free(dics);
    ZSTD_freeDDict(dd);
    ZSTD_freeCDict(cd);
    return ok;
}

void cam_pack_served(void) { applied = served_k = cam_speed_wanted(); }

// ---- during a run

#define COPIES 16
static uint64_t copies[COPIES];   // where the game's memory held the array at the last search
static int ncopies;

static void factor(float out[8], float k) {   // the array at factor k, as cam_pack_build's multiplication gives it
    for (int i = 0; i < base_n; i++) out[i] = base[i], out[i] *= k;
}

// A pass over the guest's heap and alias regions for the array at factor k, byte for byte, in memory the game may
// read and write; each copy found becomes the array at factor to and is remembered (copies, when fresh is set).
// Returns how many were rewritten. Runs on its own thread: the regions are read 16 MiB at a time under mem_read_hold,
// so the game's own mapping calls wait for one piece at most.
static int rewrite(float k, float to, int fresh) {
    enum { PIECE = 16 << 20 };
    float from[8], into[8];
    factor(from, k);
    factor(into, to);
    const size_t len = sizeof(float) * (size_t)base_n;
    const uint8_t first = *(const uint8_t *)from;
    const RtGuestLayout *l = mem_layout();
    const RtRange ranges[2] = {l->heap, l->alias};
    int found = 0;
    if (fresh) ncopies = 0;
    for (int r = 0; r < 2; r++) {
        uint64_t a = ranges[r].base, end = ranges[r].base + ranges[r].size;
        while (a < end) {
            RtMemoryInfo mi;
            if (mem_query(a, &mi) || !mi.size) break;
            uint64_t stop = mi.base + mi.size < end ? mi.base + mi.size : end;
            if (!mi.state || (mi.perm & 3) != 3) {   // free, or not the game's to read and write
                a = stop;
                continue;
            }
            while (a < stop) {
                uint64_t skip = mem_untouched_run(a, stop);   // heap pages the lazy heap never committed hold nothing
                if (skip) {
                    a = skip;
                    continue;
                }
                uint64_t n = stop - a < PIECE ? stop - a : PIECE;
                if (n >= len && mem_read_hold(a, n)) {
                    const uint8_t *p = (const uint8_t *)(uintptr_t)a, *e = p + n;
                    for (const uint8_t *q = p; q + len <= e && (q = memchr(q, first, (size_t)(e - q) - len + 1)); q++) {
                        if (memcmp(q, from, len)) continue;
                        mem_thaw((uint64_t)(uintptr_t)q, len);   // a write-protected page of a tracked region
                        memcpy((void *)(uintptr_t)q, into, len);
                        if (ncopies < COPIES) copies[ncopies++] = (uint64_t)(uintptr_t)q;
                        found++;
                    }
                    mem_read_release();
                }
                // a copy across two pieces: the next piece starts a little before this one's end
                a += n < PIECE ? n : n - (len - 1);
            }
        }
    }
    return found;
}

// The copies of the last search, still holding the array at factor k, rewritten to factor to: 1 when every one of them
// did (a step of the menu's slider then takes no search), 0 when one is gone or changed, with none of them touched.
static int rewrite_known(float k, float to) {
    float from[8], into[8];
    factor(from, k);
    factor(into, to);
    const size_t len = sizeof(float) * (size_t)base_n;
    if (!ncopies) return 0;
    for (int pass = 0; pass < 2; pass++)   // all checked first, then all written
        for (int i = 0; i < ncopies; i++) {
            if (!mem_read_hold(copies[i], len)) return 0;
            int same = !memcmp((const void *)(uintptr_t)copies[i], pass ? into : from, len) ||
                       (pass && !memcmp((const void *)(uintptr_t)copies[i], from, len));
            if (same && pass) {
                mem_thaw(copies[i], len);
                memcpy((void *)(uintptr_t)copies[i], into, len);
            }
            mem_read_release();
            if (!same) return 0;
        }
    return 1;
}

static volatile int32_t searching;   // the search thread runs
static volatile int32_t reopened;    // the game opened the pack since the last search (cam_pack_opened)
static uint64_t due_ns;              // when cam_speed_tick starts the next search; 0: none due
static int tries;                    // searches left after this one that found nothing
static float asked = 1.0f;           // the factor the last search was started for

static void search_main(void *arg) {
    (void)arg;
    float to = cam_speed_wanted(), was = applied;
    uint64_t t0 = plat_now_ns();
    // what the game holds: the array at the factor in effect, and at the start's after it read the pack again
    int n = to != was ? rewrite(was, to, 1) : 0;
    if (served_k != was && served_k != to) n += rewrite(served_k, to, 0);   // new copies beside the ones known
    if (n) {
        applied = to;
        rt_log("camera speed: x%.2f now, written over %d cop%s of the game's StickSensitivity in memory (%.0f ms)", to, n,
               n == 1 ? "y" : "ies", (plat_now_ns() - t0) / 1e6);
    } else if (to != was) {
        rt_log("camera speed: x%.2f asked for, but the game's StickSensitivity (x%.2f) is not in memory%s", to, was,
               tries ? "; looking again in a moment" : ": the camera keeps its speed until the game loads it");
    }
    plat_xchg32(&searching, 0);
}

void cam_pack_opened(void) { plat_xchg32(&reopened, 1); }

// Every present (gfx_present.c). A factor other than the one in effect goes to the copies the last search found at
// once; without them (the first change of a run, or the game moved them) a search starts, and a few more while one
// finds nothing (the game is still loading). A moment after the game opened the pack again a search looks for the
// start's factor, which is what the game has then read.
void cam_speed_tick(void) {
    if (!base_n) return;
    float want = cam_speed_wanted();
    if (plat_xchg32(&reopened, 0) && want != served_k) due_ns = plat_now_ns() + 1500000000ull, tries = 3;
    if (want == applied && !due_ns) return;
    if (plat_cas32(&searching, 0, 0)) return;   // a search writes applied and the copies
    uint64_t now = plat_now_ns();
    if (want != applied) {
        if (rewrite_known(applied, want)) {
            applied = asked = want;
            return;
        }
        if (want != asked) asked = want, due_ns = now, tries = 3;
    }
    if (!due_ns || now < due_ns) return;
    due_ns = 0;
    if (tries > 0 && want != applied) tries--, due_ns = now + 4000000000ull;   // again unless this one finds it
    plat_xchg32(&searching, 1);
    if (!plat_thread_spawn(search_main, NULL, "camera speed", 0)) plat_xchg32(&searching, 0);
}
