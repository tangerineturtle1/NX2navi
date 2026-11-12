// SARC archives and the zstd dictionaries of Pack/ZsDic.pack.zs. A SARC is a little-endian header ("SARC", its
// length at +4, the byte-order mark 0xfeff at +6, the start of the members' data at +12), then SFAT: its member count
// at +6 and from +12 one 16-byte entry per member (name hash, attributes, start and end relative to the data). An
// entry's attributes hold a nonzero top byte when the member has a name, and in the low 24 bits the name's offset in
// SFNT's string table divided by 4; SFNT's 8-byte header follows the entries. The game's archives may hold several
// members under one name (UI/Layout2Archive/Common...blarc.zs holds each layout four times), so members are walked
// one by one.
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "sarc.h"

int sarc_next(const uint8_t *p, size_t n, const char *name, unsigned *next, size_t *at, size_t *size) {
    uint32_t data;
    uint16_t head, count, bom;
    if (n < 0x14 || memcmp(p, "SARC", 4)) return -1;
    memcpy(&head, p + 4, 2);
    memcpy(&bom, p + 6, 2);
    memcpy(&data, p + 12, 4);
    if (bom != 0xfeff || (size_t)head + 12 > n || memcmp(p + head, "SFAT", 4)) return -1;
    memcpy(&count, p + head + 6, 2);
    size_t names = (size_t)head + 12 + (size_t)count * 16;   // the entries end where SFNT starts
    if (names + 8 > n || memcmp(p + names, "SFNT", 4)) return -1;
    names += 8;
    size_t len = name ? strlen(name) : 0;
    for (unsigned i = *next; i < count; i++) {
        uint32_t attr, lo, hi;
        const uint8_t *e = p + head + 12 + (size_t)i * 16;
        memcpy(&attr, e + 4, 4);
        memcpy(&lo, e + 8, 4);
        memcpy(&hi, e + 12, 4);
        if (name) {
            size_t s = names + (size_t)(attr & 0xffffff) * 4;
            if (!(attr >> 24) || s > n || n - s < len + 1 || memcmp(p + s, name, len + 1)) continue;
        }
        if (lo > hi || (size_t)data + hi > n) return -1;
        *at = (size_t)data + lo;
        *size = hi - lo;
        *next = i + 1;
        return 1;
    }
    *next = count;
    return 0;
}

int sarc_member(const uint8_t *p, size_t n, const char *name, size_t *at, size_t *size) {
    unsigned next = 0;
    return sarc_next(p, n, name, &next, at, size) == 1;
}

uint8_t *zsdic_unpack(const void *z, size_t zn, size_t max, size_t *n) {
    unsigned long long rn = ZSTD_getFrameContentSize(z, zn);
    if (rn == ZSTD_CONTENTSIZE_UNKNOWN || rn == ZSTD_CONTENTSIZE_ERROR || !rn || rn > max) return NULL;
    uint8_t *raw = malloc((size_t)rn);
    size_t got = raw ? ZSTD_decompress(raw, (size_t)rn, z, zn) : 0;
    if (!raw || ZSTD_isError(got) || got != rn) {
        free(raw);
        return NULL;
    }
    *n = (size_t)rn;
    return raw;
}

int zsdic_find(const uint8_t *dics, size_t n, unsigned id, size_t *at, size_t *size) {
    unsigned next = 0;
    if (!id) return 0;
    while (sarc_next(dics, n, NULL, &next, at, size) == 1)
        if (ZSTD_getDictID_fromDict(dics + *at, *size) == id) return 1;
    return 0;
}
