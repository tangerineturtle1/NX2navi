// The settings menu's look from the game's own files in the user's RomFS, read at the first use (menu_assets.h): its
// interface fonts (Font2/Scalable) and the shared interface textures (UI/Layout2Archive/Common...bntx). Nothing is
// shipped; a RomFS without them leaves the menu as it was.
//
// - .zs files are zstd frames, most with one of the dictionaries of Pack/ZsDic.pack.zs (a SARC compressed without one;
//   the frame names its dictionary by id).
// - A .bfotf / .bfttf font is an OpenType / TrueType file behind an 8-byte header, every big-endian 32-bit word XORed
//   with one key: the first word is 0x7f9a0218 under it, the second the payload size.
// - BNTX: the platform block at 0x20 ("NX  ", or "Ounc" on Switch 2) counts the textures (+4) and points at their BRTI records (+8); a BRTI holds the
//   format (+0x1c), width and height (+0x24, +0x28), the block height of the block-linear layout as log2 of GOBs
//   (+0x34), the level size (+0x50), the name (+0x60, a u16-length string) and the level offsets (+0x70). The levels
//   are block linear in the Switch 2 GOB layout (tex_deswizzle.c tex_bl_copy). BC4 masks become white with the mask as
//   alpha (the menu tints them), ASTC 4x4 goes through the runtime's decoder.
#include <stdlib.h>
#include <string.h>

#include <zstd.h>

#include "fs.h"
#include "host.h"
#include "menu_assets.h"
#include "sarc.h"
#include "tex.h"
#include "platform/platform.h"

#define MAX_RAW (512u << 20)

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static ZSTD_DDict *dicts[8];
static unsigned dict_ids[8];
static int ndicts = -1;

static void dicts_read(void *unused) {
    (void)unused;
    ndicts = 0;
    size_t zn = 0;
    uint64_t zn64 = 0;
    uint8_t *z = romfs_load("Pack/ZsDic.pack.zs", &zn64);
    zn = (size_t)zn64;
    if (!z) return;
    size_t rn = 0, at, size;
    unsigned next = 0;
    uint8_t *raw = zsdic_unpack(z, zn, MAX_RAW, &rn);
    if (raw) {
        while (ndicts < 8 && sarc_next(raw, rn, NULL, &next, &at, &size) == 1) {
            ZSTD_DDict *d = ZSTD_createDDict(raw + at, size);
            if (!d) continue;
            dict_ids[ndicts] = ZSTD_getDictID_fromDDict(d);
            dicts[ndicts++] = d;
        }
    }
    free(raw);
    free(z);
}

// Once, whichever thread asks first (the "menu-art" thread and the present path both do in the first frames); the
// others wait for it. Checking ndicts < 0 instead let a second caller see the 0 set at the start of the read, take it
// for "no dictionaries" and fail its asset: the menu then kept its fallback fonts for the whole run.
static void dicts_load(void) {
    static PlatOnce once = PLAT_ONCE_INIT;
    plat_once(&once, dicts_read, NULL);
}

uint8_t *menu_asset_load(const char *rel, size_t *n_out) {
    uint64_t n = 0;
    uint8_t *b = romfs_load(rel, &n);
    size_t len = strlen(rel);
    if (!b || len < 3 || strcmp(rel + len - 3, ".zs")) {
        if (b && n_out) *n_out = (size_t)n;
        return b;
    }
    unsigned long long rn = ZSTD_getFrameContentSize(b, (size_t)n);
    unsigned id = ZSTD_getDictID_fromFrame(b, (size_t)n);
    if (rn == ZSTD_CONTENTSIZE_UNKNOWN) {   // a frame that does not say its size: streamed into a growing buffer
        ZSTD_DDict *d = NULL;
        if (id) {
            dicts_load();
            for (int i = 0; i < ndicts; i++)
                if (dict_ids[i] == id) d = dicts[i];
            if (!d) { free(b); return NULL; }
        }
        ZSTD_DCtx *dc = ZSTD_createDCtx();
        if (d && dc) ZSTD_DCtx_refDDict(dc, d);
        size_t cap = (size_t)n * 4 + (1u << 20), len = 0;
        uint8_t *out = dc ? malloc(cap) : NULL;
        ZSTD_inBuffer in = {b, (size_t)n, 0};
        size_t r = 1;
        while (out && r != 0) {
            if (len == cap) {
                if (cap >= MAX_RAW) { free(out); out = NULL; break; }
                cap = cap * 2 > MAX_RAW ? MAX_RAW : cap * 2;
                uint8_t *grown = realloc(out, cap);
                if (!grown) { free(out); out = NULL; break; }
                out = grown;
            }
            ZSTD_outBuffer o = {out, cap, len};
            r = ZSTD_decompressStream(dc, &o, &in);
            len = o.pos;
            if (ZSTD_isError(r) || (r && in.pos == in.size && o.pos < o.size)) { free(out); out = NULL; break; }
        }
        ZSTD_freeDCtx(dc);
        free(b);
        if (out && n_out) *n_out = len;
        return out;
    }
    uint8_t *raw = rn && rn != ZSTD_CONTENTSIZE_ERROR && rn <= MAX_RAW ? malloc((size_t)rn) : NULL;
    size_t got = 0;
    if (raw) {
        if (id) {
            dicts_load();
            ZSTD_DDict *d = NULL;
            for (int i = 0; i < ndicts; i++)
                if (dict_ids[i] == id) d = dicts[i];
            ZSTD_DCtx *dc = d ? ZSTD_createDCtx() : NULL;
            got = dc ? ZSTD_decompress_usingDDict(dc, raw, (size_t)rn, b, (size_t)n, d) : 0;
            ZSTD_freeDCtx(dc);
        } else {
            got = ZSTD_decompress(raw, (size_t)rn, b, (size_t)n);
        }
    }
    free(b);
    if (!raw || ZSTD_isError(got) || got != rn) {
        free(raw);
        return NULL;
    }
    if (n_out) *n_out = (size_t)rn;
    return raw;
}

uint8_t *menu_asset_font(const char *rel, size_t *n_out) {
    size_t n = 0;
    uint8_t *b = menu_asset_load(rel, &n);
    if (!b || n < 12) { free(b); return NULL; }
    uint32_t key = be32(b) ^ 0x7f9a0218u, size = be32(b + 4) ^ key;
    if (size > n - 8 || size < 12) { free(b); return NULL; }
    uint8_t *out = malloc(size);
    if (!out) { free(b); return NULL; }
    for (uint32_t i = 0; i < size; i += 4) {
        uint8_t w[4] = {0};
        uint32_t k = size - i < 4 ? size - i : 4;
        memcpy(w, b + 8 + i, k);
        uint32_t v = be32(w) ^ key;
        uint8_t o[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
        memcpy(out + i, o, k);
    }
    free(b);
    // OpenType with CFF ("OTTO") or TrueType outlines (0x00010000)
    if (memcmp(out, "OTTO", 4) && be32(out) != 0x00010000u) { free(out); return NULL; }
    if (n_out) *n_out = size;
    return out;
}

static void bc4_decode(const uint8_t *blocks, uint32_t wb, uint32_t hb, uint32_t w, uint32_t h, uint8_t *rgba) {
    for (uint32_t by = 0; by < hb; by++)
        for (uint32_t bx = 0; bx < wb; bx++) {
            const uint8_t *b = blocks + ((size_t)by * wb + bx) * 8;
            uint8_t pal[8] = {b[0], b[1]};
            if (b[0] > b[1])
                for (int i = 0; i < 6; i++) pal[2 + i] = (uint8_t)(((6 - i) * b[0] + (i + 1) * b[1]) / 7);
            else {
                for (int i = 0; i < 4; i++) pal[2 + i] = (uint8_t)(((4 - i) * b[0] + (i + 1) * b[1]) / 5);
                pal[6] = 0, pal[7] = 255;
            }
            uint64_t bits = 0;
            for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
            for (int i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                if (x >= w || y >= h) continue;
                uint8_t *p = rgba + ((size_t)y * w + x) * 4;
                p[0] = p[1] = p[2] = 255;
                p[3] = pal[bits >> (3 * i) & 7];
            }
        }
}

int menu_asset_texture(const uint8_t *bntx, size_t n, const char *name, uint8_t **rgba_out, uint32_t *w_out, uint32_t *h_out) {
    // the platform block at 0x20 is "NX  " on Switch, "Ounc" in Switch 2 archives; its layout is the same
    if (!bntx || n < 0x40 || memcmp(bntx, "BNTX", 4)) return 0;
    uint32_t count = le32(bntx + 0x24);
    if (!count || count > 65536) return 0;
    uint64_t arr = le64(bntx + 0x28);
    if (arr + 8ull * count > n) return 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t p = le64(bntx + arr + 8ull * i);
        if (p + 0x78 > n || memcmp(bntx + p, "BRTI", 4)) continue;
        uint64_t no = le64(bntx + p + 0x60);
        if (no + 2 > n) continue;
        uint16_t nl = le16(bntx + no);
        if (no + 2 + nl > n || nl != strlen(name) || memcmp(bntx + no + 2, name, nl)) continue;
        uint32_t fmt = le32(bntx + p + 0x1c), w = le32(bntx + p + 0x24), h = le32(bntx + p + 0x28);
        uint32_t th = le32(bntx + p + 0x34), size = le32(bntx + p + 0x50);
        uint64_t mips = le64(bntx + p + 0x70);
        if (mips + 8 > n || !w || !h || w > 8192 || h > 8192 || th > 5) return 0;
        uint64_t data = le64(bntx + mips);
        if (data + size > n) return 0;
        uint32_t kind = fmt >> 8, bpb, bw = 4, bh = 4;
        if (kind == 0x1d) bpb = 8;                       // BC4
        else if (kind == 0x2d) bpb = 16;                 // ASTC 4x4
        else if (kind == 0x0b) bpb = 4, bw = bh = 1;     // R8G8B8A8
        else {
            rt_log("menu: interface texture %s has format %#x, not read", name, fmt);
            return 0;
        }
        uint32_t wb = (w + bw - 1) / bw, hb = (h + bh - 1) / bh, row = wb * bpb;
        // The block-linear source padded out to whole GOB columns and block-height units (tex_bl_copy reads them).
        uint64_t gob_cols = (row + 63) / 64, unit_rows = 8ull << th, units = (hb + unit_rows - 1) / unit_rows;
        uint64_t need = gob_cols * units * (512ull << th);
        uint8_t *bl = calloc(1, need > size ? need : size), *lin = malloc((size_t)row * hb), *rgba = malloc((size_t)w * h * 4);
        if (!bl || !lin || !rgba) { free(bl), free(lin), free(rgba); return 0; }
        memcpy(bl, bntx + data, size);
        tex_bl_copy(bl, lin, row, row, hb, 1, th, 0, 1);
        if (kind == 0x1d) bc4_decode(lin, wb, hb, w, h, rgba);
        else if (kind == 0x2d) astc_decode_ldr(lin, w, h, 4, 4, (fmt & 0xff) == 6, rgba);
        else memcpy(rgba, lin, (size_t)w * h * 4);
        free(bl), free(lin);
        *rgba_out = rgba, *w_out = w, *h_out = h;
        return 1;
    }
    return 0;
}
