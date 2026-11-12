// runtime/action_prompt.c: the action prompt's diamond of face buttons laid out by label. The layout file here is made
// by the test from the format as action_prompt.c's header comment describes it, with the values the game's 1.1.1
// DoCommand_00.buil holds (A right, B bottom, X top, Y left), so the reader is checked against a writer of its own;
// the archives and zstd dictionaries are made here too. No game files. Given romfs.bin and the offsets and sizes of
// the layout archive and Pack/ZsDic.pack.zs in it, it also checks the game's own archive (game_archive). Built and
// run by tests/action_prompt_check.py; prints each failed check and exits 1 on any.
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zdict.h>
#include <zstd.h>

#include "action_prompt.h"
#include "sarc.h"

static int fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL line %d: ", __LINE__);                                                                        \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

// ---- what action_prompt.c reaches in the runtime
static char last_log[512];
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof last_log, fmt, ap);
    va_end(ap);
}
uint64_t plat_now_ns(void) { return (uint64_t)((double)clock() / CLOCKS_PER_SEC * 1e9); }

// ---- a BFFH layout as the game's DoCommand_00.buil has it
enum { SIZE = 0x8000, PANE_REC = 0x44 };
static uint8_t file[SIZE];
static size_t top;   // the next free byte

static uint64_t alloc(size_t n) {
    top = (top + 7) & ~(size_t)7;
    uint64_t at = top;
    top += n;
    if (top > SIZE) {
        printf("the test's layout is too small\n");
        exit(2);
    }
    return at;
}
static void w32(uint64_t at, uint32_t v) { memcpy(file + at, &v, 4); }
static void w64(uint64_t at, uint64_t v) { memcpy(file + at, &v, 8); }
static void wf(uint64_t at, float v) { memcpy(file + at, &v, 4); }
static void set_f(uint8_t *p, uint64_t at, float v) { memcpy(p + at, &v, 4); }
static float rf(const uint8_t *p, uint64_t at) {
    float v;
    memcpy(&v, p + at, 4);
    return v;
}
static uint32_t r32(const uint8_t *p, uint64_t at) {
    uint32_t v;
    memcpy(&v, p + at, 4);
    return v;
}
static uint64_t r64(const uint8_t *p, uint64_t at) {
    uint64_t v;
    memcpy(&v, p + at, 8);
    return v;
}
static uint64_t str(const char *s) {
    uint64_t at = alloc(strlen(s) + 1);
    memcpy(file + at, s, strlen(s) + 1);
    return at;
}

#define Z (-2.842171e-14f)   // the game stores some zeros so
static const struct {
    const char *name;
    float x, y, sx, sy;
    uint32_t flags;
} panes[] = {
    {"RootPane", 0, 0, 1, 1, 0x02555000},
    {"P_BaseA_00", 0, 0, 1, 1, 0x02555089},  {"P_B_00", 0, -80, 1, 1, 0x0255508b}, {"P_X_00", 0, 80, 1, 1, 0x0255508b},
    {"P_Y_00", -80, 0, 1, 1, 0x0255508b},    {"N_BtnA_00", 80, 0, 1, 1, 0x0255500b},
    {"P_BaseB_00", Z, 0, 1, 1, 0x02555089},  {"P_A_00", 80, 0, 1, 1, 0x0255508b}, {"P_X_02", Z, 80, 1, 1, 0x0255508b},
    {"P_Y_02", -80, 0, 1, 1, 0x0255508b},    {"N_BtnB_00", 0, -80, 1, 1, 0x0255500b},
    {"P_BaseX_00", Z, 0, 1, -1, 0x02555089}, {"P_A_02", 80, 0, 1, 1, 0x0255508b}, {"P_B_03", Z, -80, 1, 1, 0x0255508b},
    {"P_Y_04", -80, 0, 1, 1, 0x0255508b},    {"N_BtnX_00", 0, 80, 1, 1, 0x0255500b},
    {"W_BaseX_00", -80, 0, 1, 1, 0x02554889}, {"W_BaseXDecide_00", -80, 0, 1, 1, 0x02554889},
    {"T_GuideX_00", 64, 0, 1, 1, 0x02554909},
    {"P_BaseY_00", Z, 0, -1, 1, 0x02555089}, {"P_A_01", 80, 0, 1, 1, 0x0255508b}, {"P_B_02", Z, -80, 1, 1, 0x0255508b},
    {"P_X_03", Z, 80, 1, 1, 0x0255508b},     {"N_BtnY_00", -80, 0, 1, 1, 0x0255500b},
    {"W_BaseY_00", 80, 0, -1, 1, 0x02554889}, {"W_BaseYDecide_00", 80, 0, -1, 1, 0x02554889},
    {"T_GuideY_00", -68, 0, 1, 1, 0x02555909},
    // left as they are: A's and L's bubbles, A's Decide null
    {"W_BaseA_00", -80, 0, 1, 1, 0x02554889}, {"N_BaseADecide_00", -80, 0, 1, 1, 0x02555002},
    {"T_GuideL_00", 66, 0, 1, 1, 0x02554909},
};
enum { NPANES = sizeof panes / sizeof *panes };
static uint64_t pane_at[NPANES];
static const char *const materials[] = {"P_BaseA_00", "P_BaseB_00", "P_BaseX_00", "P_BaseY_00", "P_B_00"};
static const char *const textures[][2] = {{"DoCommandCrossBase_A_00", "DoCommandCrossBaseGrd_A_00"},
                                          {"DoCommandCrossBase_B_00", "DoCommandCrossBaseGrd_B_00"},
                                          {"DoCommandCrossBase_B_00", "DoCommandCrossBaseGrd_B_00"},
                                          {"DoCommandCrossBase_A_00", "DoCommandCrossBaseGrd_A_00"},
                                          {"DoCommandSubBase_00", "DoCommandSubBaseGrd_00"}};
enum { NMATS = sizeof materials / sizeof *materials };
static uint64_t binding_at[NMATS][2];   // where each material's first two texture name offsets are
static uint64_t name_a[2], name_b[2];   // the offsets of the A and B pictures' names

static size_t make_layout(void) {
    memset(file, 0, sizeof file);
    top = 0x240;
    memcpy(file, "BFFH", 4);
    // the names first, A's and B's pictures shared as the game shares them
    name_a[0] = str(textures[0][0]), name_a[1] = str(textures[0][1]);
    name_b[0] = str(textures[1][0]), name_b[1] = str(textures[1][1]);
    // panes
    uint64_t names = alloc(8 * NPANES), records = alloc(8 * NPANES);
    for (int i = 0; i < NPANES; i++) {
        uint64_t r = alloc(PANE_REC);
        for (int k = 0; k < PANE_REC; k += 4) w32(r + k, 0xa5a50000u + (uint32_t)(i << 8) + (uint32_t)k);
        wf(r + 0x10, panes[i].sx), wf(r + 0x14, panes[i].sy), wf(r + 0x1c, panes[i].x), wf(r + 0x20, panes[i].y);
        w32(r + 0x38, panes[i].flags);
        w64(names + 8 * i, str(panes[i].name)), w64(records + 8 * i, r);
        pane_at[i] = r;
    }
    w64(0x118, names), w64(0x120, records), w32(0x130, NPANES), w32(0x134, 8);
    // materials and their texture blocks
    uint64_t mnames = alloc(8 * NMATS), mrecords = alloc(8 * NMATS), blocks = alloc(8 * NMATS);
    for (int i = 0; i < NMATS; i++) {
        uint64_t m = alloc(0x40), block = alloc(0x40), bindings = alloc(3 * 8);
        w32(m + 0x14, (uint32_t)i);
        w64(block, bindings), w32(block + 8, 3), w32(block + 12, 8);
        for (int k = 0; k < 3; k++) {
            uint64_t b = alloc(16);
            w32(b, 0x01010202);
            uint64_t s = k == 2 ? str("DecoNoise_00")
                         : i < 4 ? ((i == 0 || i == 3) ? name_a[k] : name_b[k])
                                 : str(textures[i][k]);
            w64(b + 8, s);
            w64(bindings + 8 * k, b);
            if (k < 2) binding_at[i][k] = b + 8;
        }
        w64(mnames + 8 * i, str(materials[i])), w64(mrecords + 8 * i, m), w64(blocks + 8 * i, block);
    }
    w64(0x138, mnames), w64(0x140, mrecords), w32(0x150, NMATS), w32(0x154, 8);
    w64(0x228, blocks), w32(0x230, NMATS), w32(0x234, 8);
    return top;
}

static int pane(const char *name) {
    for (int i = 0; i < NPANES; i++)
        if (!strcmp(panes[i].name, name)) return i;
    printf("no pane %s in the test's table\n", name);
    exit(2);
}
static float px(const uint8_t *p, const char *name, int off) { return rf(p, pane_at[pane(name)] + off); }

// 1 when a differing byte at `at` is one the layout by label may change.
static int may_change(uint64_t at) {
    static const char *const moved[] = {"P_B_00", "P_X_00", "P_Y_00", "N_BtnA_00", "P_A_00", "P_X_02", "P_Y_02",
                                        "N_BtnB_00", "P_A_02", "P_B_03", "P_Y_04", "N_BtnX_00", "P_A_01", "P_B_02",
                                        "P_X_03", "N_BtnY_00", "P_BaseX_00", "P_BaseY_00", "W_BaseX_00",
                                        "W_BaseXDecide_00", "T_GuideX_00", "W_BaseY_00", "W_BaseYDecide_00",
                                        "T_GuideY_00"};
    for (size_t i = 0; i < sizeof moved / sizeof *moved; i++) {
        uint64_t r = pane_at[pane(moved[i])];
        if ((at >= r + 0x10 && at < r + 0x18) || (at >= r + 0x1c && at < r + 0x24) || (at >= r + 0x38 && at < r + 0x3c))
            return 1;
    }
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 2; k++)
            if (at >= binding_at[i][k] && at < binding_at[i][k] + 8) return 1;
    return 0;
}

// ---- a SARC: one other member, then the given layouts in order, each named `name`
enum { MAX_COPIES = 17 };
static size_t member_at[MAX_COPIES];   // where each layout copy starts in the last archive made

static size_t make_sarc(uint8_t *out, size_t cap, const uint8_t *const *layouts, int copies, size_t n,
                        const char *name) {
    static const char other[] = "Other_00.buil";
    static const uint8_t other_data[64] = {1, 2, 3, 4, 5, 6, 7, 8};
    int count = 1 + copies;
    size_t name_len = (strlen(name) + 1 + 3) & ~(size_t)3, other_len = (sizeof other + 3) & ~(size_t)3;
    size_t sfat = 0x14, sfnt = sfat + 0xc + 16 * (size_t)count;
    size_t data = (sfnt + 8 + other_len + name_len * copies + 0xff) & ~(size_t)0xff;
    size_t total = (data + sizeof other_data + 0xff) & ~(size_t)0xff;
    for (int c = 0; c < copies; c++) member_at[c] = total, total = (total + n + 0xff) & ~(size_t)0xff;
    if (total > cap) return 0;
    memset(out, 0, total);
    memcpy(out, "SARC", 4);
    uint16_t hl = 0x14, bom = 0xfeff, v = 0x100, fl = 0xc, cnt = (uint16_t)count, eight = 8;
    uint32_t t32 = (uint32_t)total, d32 = (uint32_t)data, key = 0x65;
    memcpy(out + 4, &hl, 2), memcpy(out + 6, &bom, 2);
    memcpy(out + 8, &t32, 4), memcpy(out + 12, &d32, 4), memcpy(out + 16, &v, 2);
    memcpy(out + sfat, "SFAT", 4), memcpy(out + sfat + 4, &fl, 2), memcpy(out + sfat + 6, &cnt, 2);
    memcpy(out + sfat + 8, &key, 4);
    memcpy(out + sfnt, "SFNT", 4), memcpy(out + sfnt + 4, &eight, 2);
    size_t names = sfnt + 8;
    memcpy(out + names, other, sizeof other);
    uint32_t node[4] = {1, 0x01000000u, 0, (uint32_t)sizeof other_data};
    memcpy(out + sfat + 0xc, node, sizeof node);
    memcpy(out + data, other_data, sizeof other_data);
    for (int c = 0; c < copies; c++) {
        size_t s = names + other_len + name_len * c;
        memcpy(out + s, name, strlen(name) + 1);
        uint32_t e[4] = {2, 0x01000000u | (uint32_t)((s - names) / 4), (uint32_t)(member_at[c] - data),
                         (uint32_t)(member_at[c] - data + n)};
        memcpy(out + sfat + 0xc + 16 * (size_t)(c + 1), e, sizeof e);
        memcpy(out + member_at[c], layouts[c], n);
    }
    return total;
}

// Points copy c of an archive make_sarc made (its SFAT entry c + 1) at s[at..at + size).
static void set_range(uint8_t *s, int c, size_t at, size_t size) {
    uint32_t data, lo, hi;
    memcpy(&data, s + 12, 4);
    lo = (uint32_t)(at - data), hi = (uint32_t)(at + size - data);
    memcpy(s + 0x20 + 16 * (size_t)(c + 1) + 8, &lo, 4);
    memcpy(s + 0x20 + 16 * (size_t)(c + 1) + 12, &hi, 4);
}

// A zstd dictionary with the id `id`, its content and samples taken from src[0..n); its size, 0 when it cannot be made.
static size_t make_dict(uint8_t *out, size_t cap, unsigned id, const uint8_t *src, size_t n) {
    enum { SAMPLES = 32 };
    size_t sizes[SAMPLES];
    for (int i = 0; i < SAMPLES; i++) sizes[i] = n / SAMPLES;
    ZDICT_params_t params = {3, 0, id};
    size_t r = ZDICT_finalizeDictionary(out, cap, src, n < 2048 ? n : 2048, src, sizes, SAMPLES, params);
    return ZDICT_isError(r) ? 0 : r;
}

// ---- the game's own archive, read from romfs.bin
static uint8_t *read_part(const char *path, const char *off, const char *size, size_t *n) {
    long long at = strtoll(off, NULL, 10);
    *n = (size_t)strtoull(size, NULL, 10);
    FILE *f = fopen(path, "rb");
    uint8_t *p = f && *n ? malloc(*n) : NULL;
#ifdef _WIN32
    int ok = p && !_fseeki64(f, at, SEEK_SET) && fread(p, 1, *n, f) == *n;
#else
    int ok = p && !fseeko(f, (off_t)at, SEEK_SET) && fread(p, 1, *n, f) == *n;
#endif
    if (f) fclose(f);
    if (!ok) free(p);
    return ok ? p : NULL;
}

// The .zs file z[0..zn) unpacked with the dictionary it names from the unpacked dictionaries dics[0..dn); NULL when it
// does not unpack.
static uint8_t *unpack(const void *z, size_t zn, const uint8_t *dics, size_t dn, size_t *n) {
    unsigned id = ZSTD_getDictID_fromFrame(z, zn);
    unsigned long long rn = ZSTD_getFrameContentSize(z, zn);
    size_t at = 0, size = 0;
    if (rn == ZSTD_CONTENTSIZE_UNKNOWN || rn == ZSTD_CONTENTSIZE_ERROR || (id && !zsdic_find(dics, dn, id, &at, &size)))
        return NULL;
    uint8_t *raw = malloc((size_t)rn);
    ZSTD_DCtx *d = ZSTD_createDCtx();
    size_t got = !raw || !d ? 0
                 : id       ? ZSTD_decompress_usingDict(d, raw, (size_t)rn, z, zn, dics + at, size)
                            : ZSTD_decompressDCtx(d, raw, (size_t)rn, z, zn);
    ZSTD_freeDCtx(d);
    if (ZSTD_isError(got) || got != rn) {
        free(raw);
        return NULL;
    }
    *n = (size_t)rn;
    return raw;
}

// By label: made once, with the game's dictionary and unpacked size, and byte for byte the archive laid out by label,
// which differs from the game's only inside its four copies of the layout; then nothing more to make by label, nothing
// to make by position on the game's own, and by position again on the one by label.
static void game_archive(char **argv) {
    size_t an, dn, pn = 0, rn = 0, on = 0, bn = 0;
    uint8_t *a = read_part(argv[1], argv[2], argv[3], &an), *d = read_part(argv[1], argv[4], argv[5], &dn);
    uint8_t *dics = d ? zsdic_unpack(d, dn, 64u << 20, &pn) : NULL;
    uint8_t *raw = a && dics ? unpack(a, an, dics, pn, &rn) : NULL;
    CHECK(raw != NULL, "game: cannot read and unpack the layout archive from %s", argv[1]);
    if (!raw) {
        free(dics), free(a), free(d);
        return;
    }
    size_t at[16], size[16];
    unsigned next = 0;
    int copies = 0;
    while (copies < 16 && sarc_next(raw, rn, "DoCommand_00.buil", &next, &at[copies], &size[copies]) == 1) copies++;
    char why[200] = "";
    uint8_t *expect = malloc(rn), *expect_back = malloc(rn);
    memcpy(expect, raw, rn);
    int fields = action_prompt_archive(expect, rn, 1, why, sizeof why);
    CHECK(copies == 4 && fields == 4 * 52, "game: %d copies of the layout, %d fields changed (%s)", copies, fields, why);
    memcpy(expect_back, expect, rn);
    action_prompt_archive(expect_back, rn, 0, why, sizeof why);

    void *out = NULL, *again = NULL;
    size_t out_n = 0, again_n = 0;
    int made = action_prompt_build(a, an, d, dn, 1, &out, &out_n);
    CHECK(made == 1, "game: build by label %d (%s)", made, last_log);
    if (made == 1) {
        printf("game: %s, %zu bytes packed (the game's %zu)\n", last_log, out_n, an);
        CHECK(ZSTD_getDictID_fromFrame(out, out_n) == ZSTD_getDictID_fromFrame(a, an) &&
                  ZSTD_getFrameContentSize(out, out_n) == rn,
              "game: the built frame names another dictionary or size");
        uint8_t *got = unpack(out, out_n, dics, pn, &on);
        CHECK(got && on == rn && !memcmp(got, expect, rn), "game: the built archive is not the archive laid out by label");
        size_t outside = 0;
        for (size_t i = 0; got && on == rn && i < rn; i++) {
            if (got[i] == raw[i]) continue;
            int inside = 0;
            for (int c = 0; c < copies; c++) inside |= i >= at[c] && i < at[c] + size[c];
            outside += !inside;
        }
        CHECK(!outside, "game: %zu bytes changed outside the layout's copies", outside);
        CHECK(action_prompt_build(out, out_n, d, dn, 1, &again, &again_n) == 0 && !again, "game: second build: %s",
              last_log);
        made = action_prompt_build(out, out_n, d, dn, 0, &again, &again_n);
        uint8_t *back = made == 1 ? unpack(again, again_n, dics, pn, &bn) : NULL;
        CHECK(back && bn == rn && !memcmp(back, expect_back, rn), "game: by position again: %d (%s)", made, last_log);
        free(back);
        free(again);
        free(got);
    }
    again = NULL;
    CHECK(action_prompt_build(a, an, d, dn, 0, &again, &again_n) == 0 && !again,
          "game: build by position on the game's archive: %s", last_log);
    free(out);
    free(expect);
    free(expect_back);
    free(raw);
    free(dics);
    free(a);
    free(d);
}

int main(int argc, char **argv) {
    char why[200];
    size_t n = make_layout();
    static uint8_t before[SIZE], p[SIZE];
    memcpy(before, file, n);

    // By label: every place, cross, flip and bubble the game's diamond needs, and nothing else.
    memcpy(p, before, n);
    int changed = action_prompt_lay_out(p, n, 1, why, sizeof why);
    CHECK(changed == 52, "fields changed by label: %d (%s)", changed, changed < 0 ? why : "");
    static const struct { const char *pane; float x, y; } places[] = {
        {"N_BtnA_00", 0, -80}, {"N_BtnB_00", 80, 0}, {"N_BtnX_00", -80, 0}, {"N_BtnY_00", 0, 80},
        {"P_B_00", 80, 0},     {"P_X_00", -80, 0},   {"P_Y_00", 0, 80},     {"P_A_00", 0, -80},
        {"P_X_02", -80, 0},    {"P_Y_02", 0, 80},    {"P_A_02", 0, -80},    {"P_B_03", 80, 0},
        {"P_Y_04", 0, 80},     {"P_A_01", 0, -80},   {"P_B_02", 80, 0},     {"P_X_03", -80, 0}};
    for (size_t i = 0; i < sizeof places / sizeof *places; i++)
        CHECK(px(p, places[i].pane, 0x1c) == places[i].x && px(p, places[i].pane, 0x20) == places[i].y,
              "%s at %g %g, expected %g %g", places[i].pane, px(p, places[i].pane, 0x1c), px(p, places[i].pane, 0x20),
              places[i].x, places[i].y);
    // The crosses: A lit down (picture B), B right (A), X left (A flipped in x), Y up (B flipped in y).
    CHECK(r64(p, binding_at[0][0]) == name_b[0] && r64(p, binding_at[0][1]) == name_b[1], "A's cross is not picture B");
    CHECK(r64(p, binding_at[1][0]) == name_a[0] && r64(p, binding_at[1][1]) == name_a[1], "B's cross is not picture A");
    CHECK(r64(p, binding_at[2][0]) == name_a[0] && r64(p, binding_at[2][1]) == name_a[1], "X's cross is not picture A");
    CHECK(r64(p, binding_at[3][0]) == name_b[0] && r64(p, binding_at[3][1]) == name_b[1], "Y's cross is not picture B");
    CHECK(px(p, "P_BaseA_00", 0x10) == 1 && px(p, "P_BaseA_00", 0x14) == 1, "A's cross flipped");
    CHECK(px(p, "P_BaseX_00", 0x10) == -1 && px(p, "P_BaseX_00", 0x14) == 1, "X's cross not flipped in x only");
    CHECK(px(p, "P_BaseY_00", 0x10) == 1 && px(p, "P_BaseY_00", 0x14) == -1, "Y's cross not flipped in y only");
    // X's bubble opens to the left, Y's to the right; the text origin follows.
    CHECK(px(p, "W_BaseX_00", 0x10) == -1 && px(p, "W_BaseX_00", 0x1c) == 80, "X's bubble");
    CHECK(px(p, "W_BaseXDecide_00", 0x10) == -1 && px(p, "W_BaseXDecide_00", 0x1c) == 80, "X's Decide bubble");
    CHECK(px(p, "T_GuideX_00", 0x1c) == -68 && r32(p, pane_at[pane("T_GuideX_00")] + 0x38) == 0x02555909, "X's name");
    CHECK(px(p, "W_BaseY_00", 0x10) == 1 && px(p, "W_BaseY_00", 0x1c) == -80, "Y's bubble");
    CHECK(px(p, "W_BaseYDecide_00", 0x10) == 1 && px(p, "W_BaseYDecide_00", 0x1c) == -80, "Y's Decide bubble");
    CHECK(px(p, "T_GuideY_00", 0x1c) == 64 && r32(p, pane_at[pane("T_GuideY_00")] + 0x38) == 0x02554909, "Y's name");
    int stray = 0;
    for (size_t i = 0; i < n; i++)
        if (p[i] != before[i] && !may_change(i)) stray++;
    CHECK(!stray, "%d bytes changed outside the diamond's fields", stray);

    // Laid out by label already: nothing changes, and it says so.
    uint8_t *again = malloc(n);
    memcpy(again, p, n);
    CHECK(action_prompt_lay_out(again, n, 1, why, sizeof why) == 0 && !memcmp(again, p, n), "a second run changed it");
    // And back by position: the game's file again, its stored -2.8e-14 zeros now plain zeros.
    changed = action_prompt_lay_out(again, n, 0, why, sizeof why);
    CHECK(changed == 52, "fields changed back by position: %d (%s)", changed, changed < 0 ? why : "");
    uint8_t *expect = malloc(n);
    memcpy(expect, before, n);
    for (int i = 0; i < NPANES; i++)
        if (panes[i].x == Z && strncmp(panes[i].name, "P_Base", 6)) set_f(expect, pane_at[i] + 0x1c, 0.0f);
    CHECK(!memcmp(again, expect, n), "by label and back is not the game's layout");

    // A file that is not the expected one is refused before anything is written.
    struct { const char *pane; int off; float v; } wrong[] = {
        {"P_Y_04", 0x20, 1.0f}, {"N_BtnA_00", 0x20, -80.0f}, {"P_BaseY_00", 0x10, 1.0f}, {"T_GuideY_00", 0x1c, -60.0f}};
    for (size_t i = 0; i < sizeof wrong / sizeof *wrong; i++) {
        memcpy(p, before, n);
        memcpy(p + pane_at[pane(wrong[i].pane)] + wrong[i].off, &wrong[i].v, 4);
        memcpy(again, p, n);
        int r = action_prompt_lay_out(p, n, 1, why, sizeof why);
        CHECK(r == -1 && !memcmp(p, again, n) && strstr(why, wrong[i].pane), "%s changed: %d, %s", wrong[i].pane, r, why);
    }
    // Half laid out (A moved, the rest not): refused, untouched.
    memcpy(p, before, n);
    set_f(p, pane_at[pane("N_BtnA_00")] + 0x1c, 0), set_f(p, pane_at[pane("N_BtnA_00")] + 0x20, -80);
    memcpy(again, p, n);
    CHECK(action_prompt_lay_out(p, n, 1, why, sizeof why) == -1 && !memcmp(p, again, n), "half laid out: %s", why);
    // A cross with another picture, a missing pane, a missing material: refused.
    memcpy(p, before, n);
    memcpy(p + binding_at[0][0], &name_b[0], 8);
    CHECK(action_prompt_lay_out(p, n, 1, why, sizeof why) == -1 && strstr(why, "P_BaseA_00"), "cross picture: %s", why);
    memcpy(p, before, n);
    uint64_t s = r64(p, r64(p, 0x118) + 8 * (uint64_t)pane("P_X_03"));
    p[s] = 'Q';
    CHECK(action_prompt_lay_out(p, n, 1, why, sizeof why) == -1 && strstr(why, "P_X_03"), "missing pane: %s", why);
    memcpy(p, before, n);
    memset(p + 0x150, 0, 4);   // no materials
    CHECK(action_prompt_lay_out(p, n, 1, why, sizeof why) == -1 && strstr(why, "P_BaseA_00"), "no materials: %s", why);

    // Offsets out of the file, a cut file and a wrong magic: refused, never read past the end.
    memcpy(p, before, n);
    uint64_t far = (uint64_t)n + 100;
    memcpy(p + 0x118, &far, 8);
    CHECK(action_prompt_lay_out(p, n, 1, why, sizeof why) == -1, "pane names past the end");
    memcpy(p, before, n);
    far = UINT64_MAX - 4;
    memcpy(p + 0x120, &far, 8);
    CHECK(action_prompt_lay_out(p, n, 1, why, sizeof why) == -1, "pane records at 2^64");
    memcpy(p, before, n);
    CHECK(action_prompt_lay_out(p, 0x200, 1, why, sizeof why) == -1, "a cut file");
    CHECK(action_prompt_lay_out(p, 3, 1, why, sizeof why) == -1, "a 3-byte file");
    p[0] = 'X';
    CHECK(action_prompt_lay_out(p, n, 1, why, sizeof why) == -1 && strstr(why, "BFFH"), "wrong magic: %s", why);
    // A label other than 1 counts as by label.
    memcpy(p, before, n);
    CHECK(action_prompt_lay_out(p, n, 2, why, sizeof why) == 52 && px(p, "N_BtnA_00", 0x20) == -80, "label 2: %s", why);
    // Both bubbles of X on one record, their values the same in both layouts: laid out, the record moved once.
    memcpy(p, before, n);
    uint64_t shared = pane_at[pane("W_BaseX_00")];
    memcpy(p + r64(p, 0x120) + 8 * (uint64_t)pane("W_BaseXDecide_00"), &shared, 8);
    changed = action_prompt_lay_out(p, n, 1, why, sizeof why);
    CHECK(changed == 52 && px(p, "W_BaseX_00", 0x10) == -1 && px(p, "W_BaseX_00", 0x1c) == 80,
          "bubbles on one record: %d (%s)", changed, why);
    // P_B_00's record laid over P_BaseA_00's material, its x on the material's block index (0 as x is 0.0): the file
    // reads as expected, but moving the dot would lose A's cross. Refused, and every byte as it was.
    memcpy(p, before, n);
    uint64_t mat = r64(p, r64(p, 0x140)), over = mat + 0x14 - 0x1c;
    memcpy(p + r64(p, 0x120) + 8 * (uint64_t)pane("P_B_00"), &over, 8);
    set_f(p, mat + 0x18, -80.0f);
    memcpy(again, p, n);
    changed = action_prompt_lay_out(p, n, 1, why, sizeof why);
    CHECK(changed == -1 && !memcmp(p, again, n) && strstr(why, "overlaps"), "a pane on a material: %d, %s", changed,
          why);
    // Random damage, mostly to the header's tables: a refusal or a layout, never a crash. Each copy is an allocation
    // of its own, so that AddressSanitizer, where the runner has it, sees a read past its end.
    srand(1);
    for (int t = 0; t < 20000; t++) {
        uint8_t *q = malloc(n);
        memcpy(q, before, n);
        for (int k = 0; k < 1 + rand() % 4; k++) {
            size_t at = rand() % 2 ? 0x110 + rand() % 0x130 : rand() % n;
            q[at] ^= (uint8_t)(1 << rand() % 8);
        }
        action_prompt_lay_out(q, n, 1, why, sizeof why);
        free(q);
    }

    // The archive, holding the layout four times as the game's does: every copy changes, nothing else; one copy
    // that is not the expected layout leaves all of them as they were; without the layout, refused.
    static uint8_t sarc[SIZE * 20], sarc2[SIZE * 20];
    const uint8_t *four[MAX_COPIES] = {before, before, before, before};
    size_t sn = make_sarc(sarc, sizeof sarc, four, 4, n, "DoCommand_00.buil");
    memcpy(sarc2, sarc, sn);
    changed = action_prompt_archive(sarc2, sn, 1, why, sizeof why);
    memcpy(p, before, n);
    action_prompt_lay_out(p, n, 1, why, sizeof why);
    CHECK(changed == 4 * 52 && !memcmp(sarc2, sarc, member_at[0]), "archive: %d", changed);
    for (int c = 0; c < 4; c++) CHECK(!memcmp(sarc2 + member_at[c], p, n), "copy %d not laid out by label", c);
    // Two copies laid out already and two not (an archive half laid out by hand): the other two are laid out.
    const uint8_t *mixed[MAX_COPIES] = {p, before, p, before};
    sn = make_sarc(sarc2, sizeof sarc2, mixed, 4, n, "DoCommand_00.buil");
    changed = action_prompt_archive(sarc2, sn, 1, why, sizeof why);
    CHECK(changed == 2 * 52 && !memcmp(sarc2 + member_at[1], p, n) && !memcmp(sarc2 + member_at[3], p, n),
          "archive half laid out: %d (%s)", changed, why);
    memcpy(p, before, n);
    set_f(p, pane_at[pane("P_Y_04")] + 0x20, 1.0f);
    const uint8_t *one_off[MAX_COPIES] = {before, p, before, before};
    sn = make_sarc(sarc2, sizeof sarc2, one_off, 4, n, "DoCommand_00.buil");
    memcpy(sarc, sarc2, sn);
    CHECK(action_prompt_archive(sarc2, sn, 1, why, sizeof why) == -1 && !memcmp(sarc, sarc2, sn) &&
              strstr(why, "P_Y_04"),
          "archive with one copy not as expected: %s", why);
    sn = make_sarc(sarc2, sizeof sarc2, four, 1, n, "DoCommand_01.buil");
    CHECK(action_prompt_archive(sarc2, sn, 1, why, sizeof why) == -1 && strstr(why, "DoCommand_00.buil"),
          "archive without the layout: %s", why);
    // More copies than it lays out: refused, not the first 16 laid out.
    const uint8_t *many[MAX_COPIES];
    for (int c = 0; c < MAX_COPIES; c++) many[c] = before;
    sn = make_sarc(sarc2, sizeof sarc2, many, MAX_COPIES, n, "DoCommand_00.buil");
    memcpy(sarc, sarc2, sn);
    CHECK(sn && action_prompt_archive(sarc2, sn, 1, why, sizeof why) == -1 && !memcmp(sarc, sarc2, sn) &&
              strstr(why, "more than"),
          "archive with %d copies: %s", MAX_COPIES, why);
    // Copies that share their bytes are laid out once; copies that share some of them, refused.
    sn = make_sarc(sarc2, sizeof sarc2, four, 4, n, "DoCommand_00.buil");
    set_range(sarc2, 1, member_at[0], n);
    changed = action_prompt_archive(sarc2, sn, 1, why, sizeof why);
    CHECK(changed == 3 * 52, "archive with two copies on the same bytes: %d (%s)", changed, why);
    sn = make_sarc(sarc2, sizeof sarc2, four, 4, n, "DoCommand_00.buil");
    set_range(sarc2, 1, member_at[0] + 0x100, n);
    memcpy(sarc, sarc2, sn);
    CHECK(action_prompt_archive(sarc2, sn, 1, why, sizeof why) == -1 && !memcmp(sarc, sarc2, sn) &&
              strstr(why, "overlap"),
          "archive with overlapping copies: %s", why);
    // A copy that runs past the archive's end: damaged, refused.
    sn = make_sarc(sarc2, sizeof sarc2, four, 4, n, "DoCommand_00.buil");
    set_range(sarc2, 2, member_at[2], sn);
    memcpy(sarc, sarc2, sn);
    CHECK(action_prompt_archive(sarc2, sn, 1, why, sizeof why) == -1 && !memcmp(sarc, sarc2, sn) &&
              strstr(why, "damaged"),
          "archive with a copy past its end: %s", why);
    sn = make_sarc(sarc, sizeof sarc, four, 4, n, "DoCommand_00.buil");

    // The zstd file without a dictionary: made once, then nothing to make; a damaged frame refused.
    size_t cap = ZSTD_compressBound(sn), zn;
    uint8_t *z = malloc(cap);
    zn = ZSTD_compress(z, cap, sarc, sn, 3);
    uint8_t dic_frame[64];
    size_t dic_n = ZSTD_compress(dic_frame, sizeof dic_frame, "SARC", 4, 1);
    void *out = NULL;
    size_t out_n = 0;
    int made = action_prompt_build(z, zn, dic_frame, dic_n, 1, &out, &out_n);
    uint8_t *back = malloc(sn);
    CHECK(made == 1 && out && ZSTD_decompress(back, sn, out, out_n) == sn, "build: %d (%s)", made, last_log);
    memcpy(sarc2, sarc, sn);
    action_prompt_archive(sarc2, sn, 1, why, sizeof why);
    CHECK(made == 1 && !memcmp(back, sarc2, sn), "the built archive is not the archive laid out by label");
    void *out2 = NULL;
    CHECK(action_prompt_build(out, out_n, dic_frame, dic_n, 1, &out2, &out_n) == 0 && !out2 &&
              strstr(last_log, "already"),
          "build on a laid out archive: %s", last_log);
    CHECK(action_prompt_build(z, zn / 2, dic_frame, dic_n, 1, &out2, &out_n) == -1 && !out2 &&
              strstr(last_log, "Switch places"),
          "build on a cut frame: %s", last_log);
    free(out);

    // The zstd file with a dictionary, as the game's: the pack of dictionaries holds two (ids 1234 and 77) beside a
    // member that is not one; the archive's frame names 1234. Made, with that dictionary named again; a frame that
    // names one the pack does not hold (4321), refused.
    enum { DICT_CAP = 8192 };
    static uint8_t dicts[3][DICT_CAP];
    static const unsigned ids[3] = {1234, 77, 4321};
    for (int i = 0; i < 3; i++)
        CHECK(make_dict(dicts[i], DICT_CAP, ids[i], sarc + member_at[0], n), "dictionary %u not made", ids[i]);
    static uint8_t pack[SIZE * 4];
    const uint8_t *two[MAX_COPIES] = {dicts[0], dicts[1]};
    size_t pack_n = make_sarc(pack, sizeof pack, two, 2, DICT_CAP, "zs.zsdic");
    uint8_t *pack_z = malloc(ZSTD_compressBound(pack_n));
    size_t pack_zn = ZSTD_compress(pack_z, ZSTD_compressBound(pack_n), pack, pack_n, 3);
    for (int i = 0; i < 3; i += 2) {
        ZSTD_CDict *cd = ZSTD_createCDict(dicts[i], DICT_CAP, 3);
        ZSTD_CCtx *cc = ZSTD_createCCtx();
        zn = ZSTD_compress_usingCDict(cc, z, cap, sarc, sn, cd);
        ZSTD_freeCCtx(cc);
        ZSTD_freeCDict(cd);
        out = NULL;
        made = action_prompt_build(z, zn, pack_z, pack_zn, 1, &out, &out_n);
        if (i == 2) {
            CHECK(made == -1 && !out && strstr(last_log, "no dictionary 4321"), "a frame of another dictionary: %s",
                  last_log);
            break;
        }
        CHECK(made == 1 && ZSTD_getDictID_fromFrame(out, out_n) == 1234, "build with a dictionary: %d (%s)", made,
              last_log);
        ZSTD_DCtx *dc = ZSTD_createDCtx();
        size_t got = made == 1 ? ZSTD_decompress_usingDict(dc, back, sn, out, out_n, dicts[0], DICT_CAP) : 0;
        ZSTD_freeDCtx(dc);
        CHECK(got == sn && !memcmp(back, sarc2, sn), "the archive built with a dictionary is not the one laid out");
        free(out);
    }
    free(pack_z);
    free(back);
    free(z);
    free(again);
    free(expect);

    if (argc == 6) game_archive(argv);
    else if (argc != 1) printf("usage: action_prompt_check [romfs.bin archive-offset size zsdic-offset size]\n"), fails++;
    if (fails) printf("%d checks failed\n", fails);
    else printf("action prompt: all checks passed\n");
    return fails != 0;
}
