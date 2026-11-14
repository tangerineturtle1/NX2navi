// Amiibo dump loader, Horizon OS NFP state emulation and the figure's own data: owner, nickname, write counts and the
// game's application area (amiibo.h, "The figure's own data").
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "amiibo.h"
#include "amiibo_crypto.h"
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

#define AMIIBO_PROGRAM_ID 0x0400486024dec000ull   // the game's title id, as svc.c PROGRAM_ID tells it

// Two locks, always taken in this order: g_amiibo_io_lock, then g_amiibo_lock, never the other way round.
// g_amiibo_lock guards the state below and is held for memory work only, so the menu thread's per-frame queries
// (amiibo_has_tag, amiibo_get_reader_state) never wait for a disk. g_amiibo_io_lock serialises the keys, decryption and
// encryption, loading a dump and the file writes of a commit, so two commits reach the disk in the order they were
// made. g_dump, g_has_dump and g_info change only while both are held, so holding g_amiibo_io_lock alone reads them.
static PlatMutex g_amiibo_io_lock = PLAT_MUTEX_INIT;
static PlatMutex g_amiibo_lock = PLAT_MUTEX_INIT;

// 540-byte raw NTAG215 binary dump
static uint8_t g_dump[AMIIBO_DUMP_SIZE];
static bool g_has_dump = false;
static AmiiboInfo g_info;

static bool g_device_present = true;
static bool g_detecting = false;
static bool g_tag_present = false;
static bool g_mounted = false;
static bool g_auto_remove = true;
static float g_auto_remove_delay_s = 2.5f;

static uint64_t g_tap_timestamp_ns = 0;
static bool g_timer_active = false;

// The loaded figure's data (amiibo_get_figure_data) and what AmiiboFigureData does not carry.
static AmiiboFigureData g_fig;
static uint64_t g_title_id;            // program that created the application area
static uint16_t g_app_write_counter;   // writes of the application area
// While g_fig.decrypted: the decrypted dump and its fields, which each commit brings up to date with g_fig so that a
// write-back (amiibo_flush_figure) stores the game's changes and keeps the rest of the tag as it was.
static AmiiboPlain g_plain;
static AmiiboFields g_fields;
static bool g_keys_tried;              // under g_amiibo_io_lock: the keys file was read once (keys_load_io)

static bool amiibo_validate_dump(const uint8_t *data, uint64_t size) {
    if (size != AMIIBO_DUMP_SIZE) {
        return false;
    }

    // NTAG215 Capability Container (CC) at page 3 (offset 0x0C). An amiibo's is F1 10 FF EE, not the E1 of a plain
    // NDEF tag; the CC is not encrypted, so encrypted dumps carry it too.
    static const uint8_t amiibo_cc[4] = {0xF1, 0x10, 0xFF, 0xEE};
    if (memcmp(data + 0x0C, amiibo_cc, sizeof amiibo_cc)) {
        return false;
    }

    // UID sanity: UID (bytes 0..2 and 4..7) cannot be all zeroes or all 0xFF.
    if ((data[0] == 0 && data[1] == 0 && data[2] == 0 && data[4] == 0 &&
         data[5] == 0 && data[6] == 0 && data[7] == 0) ||
        (data[0] == 0xFF && data[1] == 0xFF && data[2] == 0xFF && data[4] == 0xFF &&
         data[5] == 0xFF && data[6] == 0xFF && data[7] == 0xFF)) {
        return false;
    }

    // ISO 14443-3 / NTAG215 Bit Check Character (BCC) verification.
    // Page 0 byte 3 (BCC0) = 0x88 ^ UID[0] ^ UID[1] ^ UID[2]
    // Page 2 byte 0 (BCC1) = UID[3] ^ UID[4] ^ UID[5] ^ UID[6]
    uint8_t bcc0 = (uint8_t)(0x88 ^ data[0] ^ data[1] ^ data[2]);
    uint8_t bcc1 = (uint8_t)(data[4] ^ data[5] ^ data[6] ^ data[7]);
    if (data[3] != bcc0 || data[8] != bcc1) {
        return false;
    }

    return true;
}

// The 7-byte UID of an NTAG215 dump: bytes 0..2, then 4..7. Byte 3 is the check byte BCC0.
static void copy_uid(uint8_t out[7], const uint8_t *dump) {
    memcpy(out, dump, 3);
    memcpy(out + 3, dump + 4, 4);
}

static void parse_dump_info(const char *path) {
    memset(&g_info, 0, sizeof(g_info));
    if (path) {
        snprintf(g_info.path, sizeof(g_info.path), "%s", path);
        const char *slash = strrchr(path, '\\');
        const char *fslash = strrchr(path, '/');
        const char *name = slash ? slash + 1 : (fslash ? fslash + 1 : path);
        if (slash && fslash) {
            name = (slash > fslash ? slash : fslash) + 1;
        }
        snprintf(g_info.filename, sizeof(g_info.filename), "%s", name);
    }

    copy_uid(g_info.uuid, g_dump);
    g_info.uuid_len = 7;

    // Amiibo ID at offset 0x54 (8 bytes, not encrypted): character ID and variant, figure type, model number
    // (big-endian), series, format version 02
    memcpy(g_info.character_id, g_dump + 0x54, 3);
    g_info.nfp_type = g_dump[0x57];
    g_info.numbering_id = ((uint16_t)g_dump[0x58] << 8) | g_dump[0x59];
    g_info.series_id = g_dump[0x5A];
    g_info.version = g_dump[0x5B];
}

// ---- the figure's data: helpers

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v), put16(p + 2, (uint16_t)(v >> 16)); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v), put32(p + 4, (uint32_t)(v >> 32)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }
static uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

// Today's date on the host clock, as the figure stores it.
static void today(uint16_t *year, uint8_t *month, uint8_t *day) {
    struct tm t;
    memset(&t, 0, sizeof t);
    plat_local_time(&t);
    int y = t.tm_year + 1900, m = t.tm_mon + 1, d = t.tm_mday;
    *year = (uint16_t)(y < 2000 ? 2000 : y > 2127 ? 2127 : y);   // the years a figure's date field holds
    *month = (uint8_t)(m < 1 || m > 12 ? 1 : m);
    *day = (uint8_t)(d < 1 || d > 31 ? 1 : d);
}

// The nickname of a figure without one: the dump's file name without its extension, at most 10 characters (UTF-8, cut
// on a character boundary) and 40 bytes.
static void default_nickname(char out[41], const char *filename) {
    const char *dot = strrchr(filename, '.');
    size_t len = dot && dot != filename ? (size_t)(dot - filename) : strlen(filename);
    size_t n = 0;
    int chars = 0;
    while (n < len && chars < 10) {
        size_t k = 1;   // the character's lead byte, then its continuation bytes
        while (n + k < len && ((uint8_t)filename[n + k] & 0xC0) == 0x80) k++;
        if (n + k > 40) break;
        n += k;
        chars++;
    }
    memcpy(out, filename, n);
    out[n] = 0;
    if (!n) snprintf(out, 41, "amiibo");
}

// <profile>/amiibo and <profile>/amiibo/<UID as 14 lower-case hex digits>.dat. False without a profile folder or when a
// path does not fit.
static bool dat_paths(char *dir, size_t dir_cap, char *path, size_t path_cap, const uint8_t uid[7]) {
    const RtConfig *rc = rt_config();
    const char *profile = rc ? rc->profile : NULL;
    if (!profile || !*profile) return false;
    if (snprintf(dir, dir_cap, "%s/amiibo", profile) >= (int)dir_cap) return false;
    return snprintf(path, path_cap, "%s/%02x%02x%02x%02x%02x%02x%02x.dat", dir, uid[0], uid[1], uid[2], uid[3], uid[4],
                    uid[5], uid[6]) < (int)path_cap;
}

// The .dat file: one figure's data, little-endian.
//   0x00 "NXAD"            0x04 u32 version (1)    0x08 UID (7 bytes)       0x0F flags: 1 registered, 2 app. area
//   0x10 u16 write counter 0x12 setup date: u16 year, u8 month, u8 day      0x16 last write date, the same layout
//   0x1A u16 application area writes                0x1C u32 access id       0x20 u32 application area bytes written
//   0x24 u32 0             0x28 u64 title id of the program that created the application area
//   0x30 nickname, UTF-8, NUL-padded (48 bytes)     0x60 application area (AMIIBO_APP_AREA_SIZE bytes)
#define DAT_VERSION 1u
#define DAT_NICK 0x30
#define DAT_AREA 0x60
#define DAT_SIZE (DAT_AREA + AMIIBO_APP_AREA_SIZE)

// Holding g_amiibo_lock.
static void dat_build(uint8_t out[DAT_SIZE]) {
    memset(out, 0, DAT_SIZE);
    memcpy(out, "NXAD", 4);
    put32(out + 0x04, DAT_VERSION);
    memcpy(out + 0x08, g_info.uuid, 7);
    out[0x0F] = (uint8_t)((g_fig.registered ? 1 : 0) | (g_fig.has_app_area ? 2 : 0));
    put16(out + 0x10, g_fig.write_counter);
    put16(out + 0x12, g_fig.setup_year), out[0x14] = g_fig.setup_month, out[0x15] = g_fig.setup_day;
    put16(out + 0x16, g_fig.write_year), out[0x18] = g_fig.write_month, out[0x19] = g_fig.write_day;
    put16(out + 0x1A, g_app_write_counter);
    put32(out + 0x1C, g_fig.access_id);
    put32(out + 0x20, g_fig.app_area_size);
    put64(out + 0x28, g_title_id);
    memcpy(out + DAT_NICK, g_fig.nickname, strnlen(g_fig.nickname, sizeof g_fig.nickname - 1));
    memcpy(out + DAT_AREA, g_fig.app_area, AMIIBO_APP_AREA_SIZE);
}

// A .dat file into g_fig (all but source and decrypted), g_title_id and g_app_write_counter. False, with nothing
// changed, when it is not a .dat of this version or belongs to another UID. Holding g_amiibo_lock.
static bool dat_parse(const uint8_t *p, uint64_t n, const uint8_t uid[7]) {
    if (!p || n != DAT_SIZE || memcmp(p, "NXAD", 4) || get32(p + 0x04) != DAT_VERSION || memcmp(p + 0x08, uid, 7))
        return false;
    uint32_t size = get32(p + 0x20);
    if (size > AMIIBO_APP_AREA_SIZE) return false;
    g_fig.registered = (p[0x0F] & 1) != 0;
    g_fig.has_app_area = (p[0x0F] & 2) != 0;
    g_fig.write_counter = get16(p + 0x10);
    g_fig.setup_year = get16(p + 0x12), g_fig.setup_month = p[0x14], g_fig.setup_day = p[0x15];
    g_fig.write_year = get16(p + 0x16), g_fig.write_month = p[0x18], g_fig.write_day = p[0x19];
    g_app_write_counter = get16(p + 0x1A);
    g_fig.access_id = get32(p + 0x1C);
    g_fig.app_area_size = size;
    g_title_id = get64(p + 0x28);
    memset(g_fig.nickname, 0, sizeof g_fig.nickname);
    memcpy(g_fig.nickname, p + DAT_NICK, sizeof g_fig.nickname - 1);
    memcpy(g_fig.app_area, p + DAT_AREA, AMIIBO_APP_AREA_SIZE);
    return true;
}

// g_fig from the decrypted fields in g_fields. Holding g_amiibo_lock.
static void seed_from_fields(void) {
    const AmiiboFields *f = &g_fields;
    g_fig.registered = (f->flags & AMIIBO_FLAG_REGISTERED) != 0;
    memcpy(g_fig.nickname, f->nickname, sizeof g_fig.nickname - 1);
    g_fig.nickname[sizeof g_fig.nickname - 1] = 0;
    g_fig.write_counter = f->write_counter;
    g_fig.setup_year = f->setup_year, g_fig.setup_month = f->setup_month, g_fig.setup_day = f->setup_day;
    g_fig.write_year = f->write_year, g_fig.write_month = f->write_month, g_fig.write_day = f->write_day;
    g_fig.has_app_area = (f->flags & AMIIBO_FLAG_APP_AREA) != 0;
    g_fig.access_id = g_fig.has_app_area ? f->access_id : 0;
    g_fig.app_area_size = g_fig.has_app_area ? AMIIBO_APP_AREA_SIZE : 0;
    memcpy(g_fig.app_area, f->app_area, AMIIBO_APP_AREA_SIZE);
    g_title_id = f->title_id;
    g_app_write_counter = f->app_write_counter;
    g_fig.source = AMIIBO_SOURCE_DUMP;
}

// The loaded figure's data, in the order of amiibo.h: the .dat in the profile, else the decrypted dump, else defaults
// (registered, nickname from the file name, today's date, no application area). g_dump, g_info and, when dec,
// g_plain and g_fields are set. Holding g_amiibo_lock.
static void figure_load_locked(const uint8_t *saved, uint64_t saved_size, bool dec) {
    memset(&g_fig, 0, sizeof g_fig);
    g_title_id = 0;
    g_app_write_counter = 0;
    if (dat_parse(saved, saved_size, g_info.uuid)) {
        g_fig.source = AMIIBO_SOURCE_SAVED;
    } else if (dec) {
        seed_from_fields();
    } else {
        g_fig.source = AMIIBO_SOURCE_NEW;
        g_fig.registered = true;
        default_nickname(g_fig.nickname, g_info.filename);
        today(&g_fig.setup_year, &g_fig.setup_month, &g_fig.setup_day);
        g_fig.write_year = g_fig.setup_year, g_fig.write_month = g_fig.setup_month, g_fig.write_day = g_fig.setup_day;
    }
    g_fig.decrypted = dec;
}

// NX2NAVI_AMIIBO_WRITEBACK: "1" writes the game's changes back into the dump file. Read through cfg_get, which needs no
// settings-table row and takes no lock.
static bool writeback_on(void) {
    const char *v = cfg_get("NX2NAVI_AMIIBO_WRITEBACK");
    return v && *v && strcmp(v, "0") != 0;
}

// The keys file: NX2NAVI_AMIIBO_KEYS, else <NX2NAVI_AMIIBO_DIR, or ./amiibo>/key_retail.bin. The keys loaded before are
// dropped either way (amiibo_keys_load). Holding g_amiibo_io_lock, not g_amiibo_lock.
static bool keys_load_io(void) {
    char path[1100];
    const char *k = cfg_get("NX2NAVI_AMIIBO_KEYS");
    if (k && *k) {
        snprintf(path, sizeof path, "%s", k);
    } else {
        const char *dir = cfg_get("NX2NAVI_AMIIBO_DIR");
        snprintf(path, sizeof path, "%s/key_retail.bin", dir && *dir ? dir : "./amiibo");
    }
    g_keys_tried = true;
    bool ok = amiibo_keys_load(path);   // logs what it found
    if (!ok) rt_log("amiibo: without keys a figure's own data starts empty and is kept in the profile only");
    return ok;
}

// What a commit writes, gathered under g_amiibo_lock and written after it is released.
typedef struct {
    bool dat;                          // the .dat below
    char dat_dir[1100], dat_path[1200];
    uint8_t dat_bytes[DAT_SIZE];
    bool dump;                         // the encrypted dump below, to dump_path
    char dump_path[512];
    uint8_t dump_bytes[AMIIBO_DUMP_SIZE];
} AmiiboCommit;

// A write to the figure (Flush, Create, Recreate, the erase): counts it, dates it, brings g_fields up to date and
// gathers the files: always the .dat; with writeback, when the keys read the dump, the dump encrypted again (g_dump
// takes it too). Holding both locks.
static void commit_prepare_locked(AmiiboCommit *cm, bool writeback) {
    if (g_fig.write_counter < 0xFFFF) g_fig.write_counter++;
    if (g_app_write_counter < 0xFFFF) g_app_write_counter++;
    today(&g_fig.write_year, &g_fig.write_month, &g_fig.write_day);

    cm->dat = dat_paths(cm->dat_dir, sizeof cm->dat_dir, cm->dat_path, sizeof cm->dat_path, g_info.uuid);
    if (cm->dat) dat_build(cm->dat_bytes);

    cm->dump = false;
    if (!g_fig.decrypted) return;
    AmiiboFields *f = &g_fields;
    f->write_counter = g_fig.write_counter;
    f->write_year = g_fig.write_year, f->write_month = g_fig.write_month, f->write_day = g_fig.write_day;
    f->app_write_counter = g_app_write_counter;
    f->access_id = g_fig.access_id;
    f->title_id = g_title_id;
    memcpy(f->app_area, g_fig.app_area, AMIIBO_APP_AREA_SIZE);
    if (g_fig.has_app_area) f->flags |= AMIIBO_FLAG_APP_AREA;
    else f->flags &= (uint8_t)~AMIIBO_FLAG_APP_AREA;
    if (!writeback || !g_info.path[0]) return;
    amiibo_plain_write(&g_plain, f);
    if (!amiibo_encrypt(&g_plain, cm->dump_bytes)) {
        rt_log("amiibo: cannot encrypt '%s' again: the dump file stays as it was", g_info.filename);
        return;
    }
    memcpy(g_dump, cm->dump_bytes, AMIIBO_DUMP_SIZE);
    snprintf(cm->dump_path, sizeof cm->dump_path, "%s", g_info.path);
    cm->dump = true;
}

// The files of a commit. Holding g_amiibo_io_lock, not g_amiibo_lock.
static void commit_write_io(const AmiiboCommit *cm) {
    if (cm->dat) {
        plat_file_mkdir_p(cm->dat_dir);
        if (plat_file_write_atomic(cm->dat_path, cm->dat_bytes, DAT_SIZE)) {
            plat_mutex_lock(&g_amiibo_lock);
            g_fig.source = AMIIBO_SOURCE_SAVED;
            plat_mutex_unlock(&g_amiibo_lock);
        } else {
            rt_log("amiibo: cannot save the figure's data to %s", cm->dat_path);
        }
    }
    if (!cm->dump) return;
    // The dump as it is on disk goes to <dump>.bak once, before the first write-back changes it.
    char bak[600];
    if (snprintf(bak, sizeof bak, "%s.bak", cm->dump_path) >= (int)sizeof bak) {
        rt_log("amiibo: dump path too long for a backup, not writing back: %s", cm->dump_path);
        return;
    }
    if (!plat_file_exists(bak)) {
        uint64_t n = 0;
        uint8_t *old = plat_file_read(cm->dump_path, &n);
        int ok = old && n == AMIIBO_DUMP_SIZE && plat_file_write_atomic(bak, old, (size_t)n);
        free(old);
        if (!ok) {
            rt_log("amiibo: cannot back up %s to %s, not writing back", cm->dump_path, bak);
            return;
        }
        rt_log("amiibo: backed up %s to %s before the first write-back", cm->dump_path, bak);
    }
    if (!plat_file_write_atomic(cm->dump_path, cm->dump_bytes, AMIIBO_DUMP_SIZE))
        rt_log("amiibo: cannot write the game's changes back into %s", cm->dump_path);
}

void amiibo_init(void) {
    plat_mutex_lock(&g_amiibo_io_lock);
    plat_mutex_lock(&g_amiibo_lock);
    g_device_present = true;
    g_detecting = false;
    g_tag_present = false;
    g_mounted = false;
    g_has_dump = false;
    g_timer_active = false;
    memset(g_dump, 0, sizeof(g_dump));
    memset(&g_info, 0, sizeof(g_info));
    memset(&g_fig, 0, sizeof g_fig);
    g_title_id = 0;
    g_app_write_counter = 0;
    plat_mutex_unlock(&g_amiibo_lock);
    plat_mutex_unlock(&g_amiibo_io_lock);
}

void amiibo_set_device_present(bool present) {
    plat_mutex_lock(&g_amiibo_lock);
    g_device_present = present;
    plat_mutex_unlock(&g_amiibo_lock);
}

bool amiibo_is_device_present(void) {
    plat_mutex_lock(&g_amiibo_lock);
    bool res = g_device_present;
    plat_mutex_unlock(&g_amiibo_lock);
    return res;
}

// While the game waits for a tag it stops and starts detection again several times a second (tens with the menu open),
// so the log names the first few and then a count every 200th start.
static uint64_t g_detect_starts;
static uint64_t g_detect_stop_ns;   // the last stop: the menu holds "searching" across the game's restarts

void amiibo_start_detection(void) {
    plat_mutex_lock(&g_amiibo_lock);
    g_detecting = true;
    uint64_t n = ++g_detect_starts;
    if (n <= 3) rt_log("amiibo: start detection");
    else if (n % 200 == 0) rt_log("amiibo: detection started %llu times so far", (unsigned long long)n);
    plat_mutex_unlock(&g_amiibo_lock);
}

void amiibo_stop_detection(void) {
    plat_mutex_lock(&g_amiibo_lock);
    if (g_detecting) g_detect_stop_ns = plat_now_ns();
    g_detecting = false;
    if (g_detect_starts <= 3) rt_log("amiibo: stop detection");
    plat_mutex_unlock(&g_amiibo_lock);
}

bool amiibo_is_detecting(void) {
    plat_mutex_lock(&g_amiibo_lock);
    bool res = g_detecting;
    plat_mutex_unlock(&g_amiibo_lock);
    return res;
}

bool amiibo_mount(void) {
    plat_mutex_lock(&g_amiibo_lock);
    if (!g_device_present || !g_tag_present) {
        plat_mutex_unlock(&g_amiibo_lock);
        return false;
    }
    g_mounted = true;
    rt_log("amiibo: mounted tag '%s'", g_info.filename[0] ? g_info.filename : "unknown");
    plat_mutex_unlock(&g_amiibo_lock);
    return true;
}

void amiibo_unmount(void) {
    plat_mutex_lock(&g_amiibo_lock);
    g_mounted = false;
    rt_log("amiibo: unmounted tag");
    plat_mutex_unlock(&g_amiibo_lock);
}

bool amiibo_is_mounted(void) {
    plat_mutex_lock(&g_amiibo_lock);
    bool res = g_mounted;
    plat_mutex_unlock(&g_amiibo_lock);
    return res;
}

AmiiboDeviceState amiibo_get_device_state(void) {
    plat_mutex_lock(&g_amiibo_lock);
    if (!g_device_present) {
        plat_mutex_unlock(&g_amiibo_lock);
        return AMIIBO_DEV_STATE_UNAVAILABLE;
    }
    if (g_mounted) {
        plat_mutex_unlock(&g_amiibo_lock);
        return AMIIBO_DEV_STATE_TAG_MOUNTED;
    }
    if (g_tag_present) {
        plat_mutex_unlock(&g_amiibo_lock);
        return AMIIBO_DEV_STATE_TAG_FOUND;
    }
    if (g_detecting) {
        plat_mutex_unlock(&g_amiibo_lock);
        return AMIIBO_DEV_STATE_SEARCHING;
    }
    plat_mutex_unlock(&g_amiibo_lock);
    return AMIIBO_DEV_STATE_INITIALIZED;
}

AmiiboDeviceState amiibo_get_reader_state(void) {
    AmiiboDeviceState state = amiibo_get_device_state();
    if (state != AMIIBO_DEV_STATE_INITIALIZED) return state;
    plat_mutex_lock(&g_amiibo_lock);
    bool recent = g_detect_stop_ns && plat_now_ns() - g_detect_stop_ns < 1000000000ull;
    plat_mutex_unlock(&g_amiibo_lock);
    return recent ? AMIIBO_DEV_STATE_SEARCHING : state;
}

bool amiibo_load_file(const char *path) {
    if (!path || !*path) return false;
    uint64_t size = 0;
    uint8_t *data = plat_file_read(path, &size);
    if (!data) {
        rt_log("amiibo: failed to read file: %s", path);
        return false;
    }
    if (size != AMIIBO_DUMP_SIZE) {
        rt_log("amiibo: invalid dump size (%llu bytes, expected exactly %d): %s",
               (unsigned long long)size, AMIIBO_DUMP_SIZE, path);
        free(data);
        return false;
    }
    if (!amiibo_validate_dump(data, size)) {
        rt_log("amiibo: dump failed NTAG215 structure sanity checks: %s", path);
        free(data);
        return false;
    }

    plat_mutex_lock(&g_amiibo_io_lock);
    // The figure's saved data, read after any commit of it has reached the disk
    uint8_t uid[7];
    copy_uid(uid, data);
    char dat_dir[1100], dat_path[1200];
    uint64_t saved_size = 0;
    uint8_t *saved = dat_paths(dat_dir, sizeof dat_dir, dat_path, sizeof dat_path, uid) ?
                     plat_file_read(dat_path, &saved_size) : NULL;
    if (!g_keys_tried) keys_load_io();   // the first dump loads the keys: the settings are read by then
    static AmiiboPlain plain;            // under g_amiibo_io_lock
    static AmiiboFields fields;
    bool dec = amiibo_keys_loaded() && amiibo_decrypt(data, &plain);
    if (dec) amiibo_plain_read(&plain, &fields);

    plat_mutex_lock(&g_amiibo_lock);
    memcpy(g_dump, data, AMIIBO_DUMP_SIZE);
    g_has_dump = true;
    parse_dump_info(path);
    if (dec) {
        g_plain = plain;
        g_fields = fields;
    }
    figure_load_locked(saved, saved_size, dec);
    AmiiboInfo info = g_info;
    AmiiboDataSource source = g_fig.source;
    bool has_area = g_fig.has_app_area;
    uint32_t access_id = g_fig.access_id;
    plat_mutex_unlock(&g_amiibo_lock);
    plat_mutex_unlock(&g_amiibo_io_lock);

    free(saved);
    free(data);
    static const char *const sources[] = {"new", "saved", "dump"};
    char area[48];
    if (has_area) snprintf(area, sizeof area, "application area of access id %#x", access_id);
    else snprintf(area, sizeof area, "no application area");
    rt_log("amiibo: loaded dump '%s' (char_id: %02x%02x%02x, series: %02x, num: %04x; %s, data: %s, %s)",
           info.filename, info.character_id[0], info.character_id[1], info.character_id[2], info.series_id,
           info.numbering_id, dec ? "decrypted" : "not decrypted", sources[source], area);
    return true;
}

void amiibo_tap_tag(void) {
    plat_mutex_lock(&g_amiibo_lock);
    if (!g_has_dump) {
        plat_mutex_unlock(&g_amiibo_lock);
        return;
    }
    g_tag_present = true;
    g_tap_timestamp_ns = plat_now_ns();
    if (g_auto_remove) {
        g_timer_active = true;
    }
    rt_log("amiibo: tag tapped on reader ('%s')", g_info.filename);
    plat_mutex_unlock(&g_amiibo_lock);
}

bool amiibo_scan_and_tap(const char *path) {
    if (!amiibo_load_file(path)) return false;
    amiibo_tap_tag();
    return true;
}

void amiibo_remove_tag(void) {
    plat_mutex_lock(&g_amiibo_lock);
    if (g_tag_present || g_mounted) {
        g_tag_present = false;
        g_mounted = false;
        g_timer_active = false;
        rt_log("amiibo: tag removed from reader");
    }
    plat_mutex_unlock(&g_amiibo_lock);
}

void amiibo_tick(void) {
    plat_mutex_lock(&g_amiibo_lock);
    if (g_timer_active && g_tag_present) {
        uint64_t now = plat_now_ns();
        uint64_t diff = now > g_tap_timestamp_ns ? now - g_tap_timestamp_ns : 0;
        uint64_t limit_ns = (uint64_t)(g_auto_remove_delay_s * 1000000000.0f);
        if (diff >= limit_ns) {
            g_tag_present = false;
            g_mounted = false;
            g_timer_active = false;
            rt_log("amiibo: auto-removed tag after %.1fs timeout", (double)g_auto_remove_delay_s);
        }
    }
    plat_mutex_unlock(&g_amiibo_lock);
}

void amiibo_set_auto_remove(bool enable) {
    plat_mutex_lock(&g_amiibo_lock);
    g_auto_remove = enable;
    if (!enable) g_timer_active = false;
    plat_mutex_unlock(&g_amiibo_lock);
}

bool amiibo_get_auto_remove(void) {
    plat_mutex_lock(&g_amiibo_lock);
    bool res = g_auto_remove;
    plat_mutex_unlock(&g_amiibo_lock);
    return res;
}

void amiibo_set_auto_remove_delay_s(float seconds) {
    plat_mutex_lock(&g_amiibo_lock);
    if (seconds >= 0.5f && seconds <= 10.0f) g_auto_remove_delay_s = seconds;
    plat_mutex_unlock(&g_amiibo_lock);
}

float amiibo_get_auto_remove_delay_s(void) {
    plat_mutex_lock(&g_amiibo_lock);
    float res = g_auto_remove_delay_s;
    plat_mutex_unlock(&g_amiibo_lock);
    return res;
}

bool amiibo_has_tag(void) {
    plat_mutex_lock(&g_amiibo_lock);
    bool res = g_has_dump && g_tag_present;
    plat_mutex_unlock(&g_amiibo_lock);
    return res;
}

const AmiiboInfo *amiibo_get_current_info(void) {
    return &g_info;
}

const uint8_t *amiibo_get_raw_data(void) {
    return g_dump;
}

void amiibo_get_tag_info(void *out_tag_info) {
    plat_mutex_lock(&g_amiibo_lock);
    uint8_t *out = (uint8_t *)out_tag_info;
    memset(out, 0, 0x58);
    // UUID: offset 0x00, 10 bytes max
    copy_uid(out, g_dump);
    out[0x0A] = 7;          // UUID length
    // Protocol at offset 0x20: TypeA = 1
    *(uint32_t *)(out + 0x20) = 1;
    // TagType at offset 0x24: Type2 / NTAG215 = 2
    *(uint32_t *)(out + 0x24) = 2;
    plat_mutex_unlock(&g_amiibo_lock);
}

void amiibo_get_model_info(void *out_model_info) {
    plat_mutex_lock(&g_amiibo_lock);
    uint8_t *out = (uint8_t *)out_model_info;
    memset(out, 0, 0x40);
    // nn::nfp::ModelInfo from the amiibo ID at 0x54: character ID and variant in the tag's byte order, series, model
    // number little-endian (big-endian on the tag), figure type. The game tells figures of one character apart by the
    // model number.
    memcpy(out + 0x00, g_dump + 0x54, 3);   // character ID, variant
    out[0x03] = g_dump[0x5A];               // series
    out[0x04] = g_dump[0x59];               // model number
    out[0x05] = g_dump[0x58];
    out[0x06] = g_dump[0x57];               // figure type
    plat_mutex_unlock(&g_amiibo_lock);
}

// nn::nfp::CommonInfo (0x40 bytes): last write date {u16 year, u8 month, u8 day} at 0, write counter u16 at 4, figure
// format version at 6, application area size u32 at 8; little-endian.
void amiibo_get_common_info(void *out_common_info) {
    plat_mutex_lock(&g_amiibo_lock);
    uint8_t *out = (uint8_t *)out_common_info;
    memset(out, 0, 0x40);
    put16(out + 0x00, g_fig.write_year);
    out[0x02] = g_fig.write_month;
    out[0x03] = g_fig.write_day;
    put16(out + 0x04, g_fig.write_counter);
    out[0x06] = 0x02;
    put32(out + 0x08, AMIIBO_APP_AREA_SIZE);
    plat_mutex_unlock(&g_amiibo_lock);
}

// nn::nfp::RegisterInfo (0x100 bytes): the owner's Mii as a CharInfo at 0 (0x58 bytes), the setup date {u16 year, u8
// month, u8 day} at 0x58, the nickname UTF-8 NUL-terminated at 0x5C (up to 0x29 bytes), font region at 0x85. The Mii is
// a minimal default CharInfo: the tag's own Mii is not converted. A figure without an owner reports the nickname a new
// one gets (default_nickname).
void amiibo_get_register_info(void *out_register_info) {
    plat_mutex_lock(&g_amiibo_lock);
    uint8_t *out = (uint8_t *)out_register_info;
    memset(out, 0, 0x100);
    out[0x00] = 0x03;
    uint16_t year = g_fig.setup_year;
    uint8_t month = g_fig.setup_month, day = g_fig.setup_day;
    if (!year || !month || !day) today(&year, &month, &day);   // an unregistered tag stores date 0 (2000-00-00)
    put16(out + 0x58, year);
    out[0x5A] = month;
    out[0x5B] = day;
    char nick[41];
    if (g_fig.nickname[0]) snprintf(nick, sizeof nick, "%s", g_fig.nickname);
    else default_nickname(nick, g_info.filename[0] ? g_info.filename : "Amiibo");
    snprintf((char *)(out + 0x5C), 0x29, "%s", nick);
    plat_mutex_unlock(&g_amiibo_lock);
}

int amiibo_get_application_area(void *out_buf, uint32_t max_size) {
    plat_mutex_lock(&g_amiibo_lock);
    if (!g_has_dump || !out_buf || !g_fig.has_app_area) {
        plat_mutex_unlock(&g_amiibo_lock);
        return 0;
    }
    uint32_t to_copy = max_size < AMIIBO_APP_AREA_SIZE ? max_size : AMIIBO_APP_AREA_SIZE;
    memcpy(out_buf, g_fig.app_area, to_copy);
    plat_mutex_unlock(&g_amiibo_lock);
    return (int)to_copy;
}

// ---- the figure's own data (amiibo.h)

bool amiibo_get_figure_data(AmiiboFigureData *out) {
    plat_mutex_lock(&g_amiibo_lock);
    bool ok = g_has_dump;
    if (ok && out) *out = g_fig;
    plat_mutex_unlock(&g_amiibo_lock);
    return ok;
}

bool amiibo_erase_app_area(void) {
    bool writeback = writeback_on();
    AmiiboCommit *cm = calloc(1, sizeof *cm);
    if (!cm) rt_fatal("amiibo: out of memory");
    plat_mutex_lock(&g_amiibo_io_lock);
    plat_mutex_lock(&g_amiibo_lock);
    bool ok = g_has_dump;
    if (ok) {
        g_fig.has_app_area = false;
        g_fig.access_id = 0;
        g_fig.app_area_size = 0;
        memset(g_fig.app_area, 0, sizeof g_fig.app_area);
        g_title_id = 0;
        commit_prepare_locked(cm, writeback);
        rt_log("amiibo: erased the game data of '%s'", g_info.filename);
    }
    plat_mutex_unlock(&g_amiibo_lock);
    if (ok) commit_write_io(cm);
    plat_mutex_unlock(&g_amiibo_io_lock);
    free(cm);
    return ok;
}

bool amiibo_reload_keys(void) {
    plat_mutex_lock(&g_amiibo_io_lock);
    keys_load_io();
    bool loaded = amiibo_keys_loaded();
    // g_dump and g_has_dump change only under g_amiibo_io_lock, which this holds: read them without g_amiibo_lock
    static AmiiboPlain plain;   // under g_amiibo_io_lock
    static AmiiboFields fields;
    bool have = g_has_dump;
    bool dec = have && loaded && amiibo_decrypt(g_dump, &plain);
    if (dec) amiibo_plain_read(&plain, &fields);
    if (have) {
        plat_mutex_lock(&g_amiibo_lock);
        if (dec) {
            g_plain = plain;
            g_fields = fields;
            if (g_fig.source == AMIIBO_SOURCE_NEW) seed_from_fields();   // nothing of it stored yet: the tag's own data
        }
        g_fig.decrypted = dec;
        plat_mutex_unlock(&g_amiibo_lock);
        rt_log("amiibo: keys %s, the loaded dump is %s", loaded ? "loaded" : "not loaded",
               dec ? "decrypted" : "not decrypted");
    }
    plat_mutex_unlock(&g_amiibo_io_lock);
    return loaded;
}

// ---- nn::nfp application area calls (ov_system.c, nfp.c)

uint32_t amiibo_open_app_area(uint32_t access_id) {
    plat_mutex_lock(&g_amiibo_lock);
    uint32_t r = !g_has_dump || !g_mounted ? AMIIBO_R_DEVICE_NOT_FOUND
               : !g_fig.has_app_area       ? AMIIBO_R_AREA_NOT_CREATED
               : access_id != g_fig.access_id ? AMIIBO_R_WRONG_ACCESS_ID : 0;
    uint32_t have = g_fig.access_id;
    plat_mutex_unlock(&g_amiibo_lock);
    if (r == AMIIBO_R_WRONG_ACCESS_ID)
        rt_log("amiibo: application area of another program (access id %#x, asked %#x)", have, access_id);
    return r;
}

uint32_t amiibo_get_app_area(void *out, uint64_t cap, uint64_t *out_size) {
    if (out_size) *out_size = 0;
    plat_mutex_lock(&g_amiibo_lock);
    uint32_t r = !g_has_dump || !g_mounted ? AMIIBO_R_DEVICE_NOT_FOUND
               : !g_fig.has_app_area       ? AMIIBO_R_AREA_NOT_CREATED : 0;
    if (!r) {
        // the whole area, cut to the caller's buffer: the game checks that the size it gets is the size it asked for
        uint64_t n = cap < AMIIBO_APP_AREA_SIZE ? cap : AMIIBO_APP_AREA_SIZE;
        if (!out) n = 0;
        if (n) memcpy(out, g_fig.app_area, (size_t)n);
        if (out_size) *out_size = n;
    }
    plat_mutex_unlock(&g_amiibo_lock);
    return r;
}

uint32_t amiibo_set_app_area(const void *data, uint64_t size) {
    plat_mutex_lock(&g_amiibo_lock);
    uint32_t r = !g_has_dump || !g_mounted ? AMIIBO_R_DEVICE_NOT_FOUND
               : !g_fig.has_app_area       ? AMIIBO_R_AREA_NOT_CREATED : 0;
    uint64_t asked = size;
    if (!r) {
        if (!data) size = 0;
        if (size > AMIIBO_APP_AREA_SIZE) size = AMIIBO_APP_AREA_SIZE;
        memset(g_fig.app_area, 0, sizeof g_fig.app_area);   // the bytes past the new data read back as zero
        if (size) memcpy(g_fig.app_area, data, (size_t)size);
        g_fig.app_area_size = (uint32_t)size;
    }
    plat_mutex_unlock(&g_amiibo_lock);
    if (!r && asked > AMIIBO_APP_AREA_SIZE)
        rt_log("amiibo: SetApplicationArea of %llu bytes cut to %d", (unsigned long long)asked, AMIIBO_APP_AREA_SIZE);
    return r;
}

uint32_t amiibo_create_app_area(uint32_t access_id, const void *data, uint64_t size, bool recreate) {
    bool writeback = writeback_on();
    AmiiboCommit *cm = calloc(1, sizeof *cm);
    if (!cm) rt_fatal("amiibo: out of memory");
    plat_mutex_lock(&g_amiibo_io_lock);
    plat_mutex_lock(&g_amiibo_lock);
    uint32_t r = !g_has_dump || !g_mounted        ? AMIIBO_R_DEVICE_NOT_FOUND
               : !recreate && g_fig.has_app_area ? AMIIBO_R_AREA_EXISTS : 0;
    if (!r) {
        if (!data) size = 0;
        if (size > AMIIBO_APP_AREA_SIZE) size = AMIIBO_APP_AREA_SIZE;
        g_fig.has_app_area = true;
        g_fig.access_id = access_id;
        memset(g_fig.app_area, 0, sizeof g_fig.app_area);
        if (size) memcpy(g_fig.app_area, data, (size_t)size);
        g_fig.app_area_size = (uint32_t)size;
        g_title_id = AMIIBO_PROGRAM_ID;
        commit_prepare_locked(cm, writeback);
        rt_log("amiibo: %s the application area of '%s' (access id %#x, %llu bytes of initial data)",
               recreate ? "recreated" : "created", g_info.filename, access_id, (unsigned long long)size);
    }
    plat_mutex_unlock(&g_amiibo_lock);
    if (!r) commit_write_io(cm);
    plat_mutex_unlock(&g_amiibo_io_lock);
    free(cm);
    return r;
}

uint32_t amiibo_flush_figure(void) {
    bool writeback = writeback_on();
    AmiiboCommit *cm = calloc(1, sizeof *cm);
    if (!cm) rt_fatal("amiibo: out of memory");
    plat_mutex_lock(&g_amiibo_io_lock);
    plat_mutex_lock(&g_amiibo_lock);
    uint32_t r = !g_has_dump || !g_mounted ? AMIIBO_R_DEVICE_NOT_FOUND : 0;
    uint16_t count = 0;
    if (!r) {
        commit_prepare_locked(cm, writeback);
        count = g_fig.write_counter;
    }
    plat_mutex_unlock(&g_amiibo_lock);
    if (!r) {
        commit_write_io(cm);
        rt_log("amiibo: flushed '%s' (write %u%s)", g_info.filename, (unsigned)count,
               cm->dump ? ", written back into the dump" : "");
    }
    plat_mutex_unlock(&g_amiibo_io_lock);
    free(cm);
    return r;
}
