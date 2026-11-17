// VR's wheels' item names in the game's own words (vr_names.h): the <name>_Name texts of ActorMsg/PouchContent.msbt in
// the Mals archive of the game's language (Mals/<region>.Product.<n>.sarc.zs), read again when the language changes.
// The archive's MSBT is UTF-8 (its header's encoding byte 0): the labels' hash table in LBL1, the texts in TXT2, a text's
// inline tags (0x0e group, type, size, data; 0x0f group, type) left out.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <stdio.h>

#include "fs.h"
#include "host.h"
#include "menu_assets.h"
#include "ov.h"
#include "platform/platform.h"
#include "ui_text.h"
#include "vr_names.h"

typedef struct {
    char *key, *text;
} Name;
static PlatMutex lock = PLAT_MUTEX_INIT;
static Name *names;
static int name_n;
static char loaded[8];   // the region read ("" none yet)

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// A SARC archive's member: its place and size, 0 when it is not there
static int sarc_member(const uint8_t *p, size_t n, const char *name, size_t *at, size_t *size) {
    if (n < 0x20 || memcmp(p, "SARC", 4)) return 0;
    const size_t head = u16(p + 4), data = u32(p + 12);
    if (head + 12 > n || memcmp(p + head, "SFAT", 4)) return 0;
    const unsigned count = u16(p + head + 6);
    const size_t table = head + 12 + (size_t)count * 16 + 8, len = strlen(name);
    for (unsigned i = 0; i < count; i++) {
        const size_t e = head + 12 + (size_t)i * 16;
        if (e + 16 > n) return 0;
        const size_t s = table + (size_t)(u32(p + e + 4) & 0xffffff) * 4, lo = u32(p + e + 8), hi = u32(p + e + 12);
        if (s + len + 1 > n || memcmp(p + s, name, len + 1)) continue;
        if (lo > hi || data + hi > n) return 0;
        *at = data + lo, *size = hi - lo;
        return 1;
    }
    return 0;
}

// The MSBT's texts whose labels end in _Name, into names
static void read_msbt(const uint8_t *b, size_t n) {
    if (n < 0x20 || memcmp(b, "MsgStdBn", 8) || b[8] != 0xff || b[0xc] != 0) return;   // little-endian UTF-8 only
    const unsigned sections = u16(b + 0xe);
    size_t off = 0x20, lbl = 0, txt = 0, txt_size = 0;
    for (unsigned s = 0; s < sections && off + 16 <= n; s++) {
        const size_t size = u32(b + off + 4), body = off + 16;
        if (body + size > n) return;
        if (!memcmp(b + off, "LBL1", 4)) lbl = body;
        if (!memcmp(b + off, "TXT2", 4)) txt = body, txt_size = size;
        off = (body + size + 15) & ~(size_t)15;
    }
    if (!lbl || !txt) return;
    const uint32_t texts = u32(b + txt), slots = u32(b + lbl);
    names = calloc(texts, sizeof *names);
    if (!names) return;
    for (uint32_t k = 0; k < slots; k++) {
        size_t p = lbl + u32(b + lbl + 8 + 8 * k);
        for (uint32_t c = u32(b + lbl + 4 + 8 * k); c && p < n; c--) {
            const size_t len = b[p];
            if (p + 1 + len + 4 > n) return;
            const uint32_t idx = u32(b + p + 1 + len);
            if (len > 5 && !memcmp(b + p + 1 + len - 5, "_Name", 5) && idx < texts && name_n < (int)texts) {
                const size_t from = txt + u32(b + txt + 4 + 4 * idx);
                const size_t to = idx + 1 < texts ? txt + u32(b + txt + 4 + 4 * (idx + 1)) : txt + txt_size;
                char *key = malloc(len - 4), *text = to > from && to <= n ? malloc(to - from + 1) : NULL;
                if (key && text) {
                    memcpy(key, b + p + 1, len - 5), key[len - 5] = 0;
                    size_t o = 0;
                    for (size_t i = from; i < to && b[i];) {
                        if (b[i] == 0x0e && i + 7 <= to) i += 7 + u16(b + i + 5);
                        else if (b[i] == 0x0f) i += 5;
                        else text[o++] = (char)b[i++];
                    }
                    text[o] = 0;
                    names[name_n++] = (Name){key, text};
                } else {
                    free(key), free(text);
                }
            }
            p += 1 + len + 4;
        }
    }
}

static void free_names(void) {
    for (int i = 0; i < name_n; i++) free(names[i].key), free(names[i].text);
    free(names), names = NULL, name_n = 0;
}

// The game's language's archive read, when it is not the one read already
static void load(void) {
    const char *region = ui_text_game_mals(ov_language());
    if (!region) region = "USen";
    if (!strcmp(loaded, region)) return;
    free_names();
    snprintf(loaded, sizeof loaded, "%s", region);
    char prefix[16];
    snprintf(prefix, sizeof prefix, "Mals/%s.", region);
    const char *path = NULL;
    for (uint64_t i = 0, count = romfs_file_count(); i < count && !path; i++) {
        uint64_t o, sz;
        const char *e = romfs_entry(i, &o, &sz);
        if (e && !strncmp(e, prefix, strlen(prefix)) && strstr(e, ".sarc.zs")) path = e;
    }
    size_t n = 0, at, size;
    uint8_t *sarc = path ? menu_asset_load(path, &n) : NULL;
    if (sarc && sarc_member(sarc, n, "ActorMsg/PouchContent.msbt", &at, &size)) read_msbt(sarc + at, size);
    free(sarc);
    rt_log("vr: %d item names from %s", name_n, path ? path : prefix);
}

const char *vr_item_name(const char *item, const char *fallback) {
    plat_mutex_lock(&lock);
    load();
    const char *found = NULL;
    for (int i = 0; i < name_n && !found; i++)
        if (!strcmp(names[i].key, item)) found = names[i].text;
    plat_mutex_unlock(&lock);
    return found && *found ? found : fallback;
}
