// Players: the folders the saves of each player live in (players.h).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "players.h"

#define NAME_FILE "player-name.txt"
#define ID_MAX 9999

static PlatMutex lock = PLAT_MUTEX_INIT;   // creating, renaming and removing against each other and the listing

// "2".."9999" -> the number; 0 for anything else (player 1 is never a folder under players/).
static int folder_id(const char *s) {
    if (!s || !*s || strlen(s) > 4 || strspn(s, "0123456789") != strlen(s) || *s == '0') return 0;
    int id = atoi(s);
    return id >= 2 && id <= ID_MAX ? id : 0;
}

static int player_dir(const char *profile, int id, char *out, size_t cap) {
    int n = id <= 1 ? snprintf(out, cap, "%s", profile) : snprintf(out, cap, "%s/players/%d", profile, id);
    return n > 0 && (size_t)n < cap;
}

static PlatOnce active_once = PLAT_ONCE_INIT;
static int active = 1;
static void read_active(void *param) {
    (void)param;
    const char *v = cfg_value("NX2NAVI_PLAYER");
    if (v && *v && strcmp(v, "1")) {
        int id = folder_id(v);
        if (id) active = id;
        else rt_log("players: player = %s is not a player number; playing as player 1", v);
    }
}

int players_active(void) {
    plat_once(&active_once, read_active, NULL);
    return active;
}

int players_save_dir(const char *profile, char *out, size_t cap) {
    if (!player_dir(profile, players_active(), out, cap)) return 0;
    plat_file_mkdir_p(out);
    if (!plat_dir_exists(out)) rt_fatal("players: cannot create %s", out);
    if (active > 1) rt_log("players: playing as player %d (%s)", active, out);
    return 1;
}

void players_clean_name(const char *name, char *out, size_t cap) {
    size_t n = 0, lim = cap ? cap - 1 : 0;
    if (lim > PLAYER_NAME_MAX) lim = PLAYER_NAME_MAX;
    for (const unsigned char *p = (const unsigned char *)(name ? name : ""); *p && n < lim;) {
        if (*p < 0x20 || *p == 0x7f) {   // a line break or tab would end the name file's line
            p++;
            continue;
        }
        size_t len = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : (*p >> 3) == 30 ? 4 : 0;
        if (!len) {   // a stray continuation byte
            p++;
            continue;
        }
        size_t k = 1;
        while (k < len && (p[k] & 0xc0) == 0x80) k++;
        if (k < len) {   // a character cut short
            p += k;
            continue;
        }
        if (n + len > lim) break;
        memcpy(out + n, p, len);
        n += len, p += len;
    }
    size_t start = 0;
    while (start < n && out[start] == ' ') start++;
    while (n > start && out[n - 1] == ' ') n--;
    memmove(out, out + start, n - start);
    if (cap) out[n - start] = 0;
}

static void read_name(const char *dir, char *out, size_t cap) {
    char path[PLAT_PATH_MAX + 32];
    out[0] = 0;
    if (snprintf(path, sizeof path, "%s/" NAME_FILE, dir) >= (int)sizeof path) return;
    uint64_t n = 0;
    uint8_t *b = plat_file_read(path, &n);
    if (!b) return;
    if (n > 3 && b[0] == 0xef && b[1] == 0xbb && b[2] == 0xbf) memmove(b, b + 3, (size_t)(n -= 3) + 1);   // Notepad's BOM
    b[strcspn((char *)b, "\r\n")] = 0;
    players_clean_name((const char *)b, out, cap);
    free(b);
}

static int write_name(const char *dir, const char *name) {
    char path[PLAT_PATH_MAX + 32], clean[PLAYER_NAME_MAX + 1];
    if (snprintf(path, sizeof path, "%s/" NAME_FILE, dir) >= (int)sizeof path) return 0;
    players_clean_name(name, clean, sizeof clean);
    if (!*clean) {
        if (plat_file_exists(path)) plat_file_delete(path);
        return !plat_file_exists(path);
    }
    char line[PLAYER_NAME_MAX + 2];
    int n = snprintf(line, sizeof line, "%s\n", clean);
    return plat_file_write_atomic(path, line, (size_t)n);
}

// The game saved for the player: an account folder under <dir>/save that is not empty, or a finished commit waiting in
// save-tx.
static int any_entry(const char *name, int kind, uint64_t size, void *ctx) {
    (void)name, (void)kind, (void)size;
    *(int *)ctx = 1;
    return 0;
}
typedef struct { const char *dir; int found; } SaveScan;
static int scan_account(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    SaveScan *s = ctx;
    if (kind != PLAT_FS_DIR && kind != PLAT_FS_LINK) return 1;
    char p[PLAT_PATH_MAX + 64];
    if (snprintf(p, sizeof p, "%s/%s", s->dir, name) >= (int)sizeof p) return 1;
    plat_fs_list(p, any_entry, &s->found);
    return !s->found;
}
static int scan_ready(const char *name, int kind, uint64_t size, void *ctx) {
    (void)kind, (void)size;
    if (strncmp(name, "ready-", 6)) return 1;
    *(int *)ctx = 1;
    return 0;
}
static int has_save(const char *dir) {
    char p[PLAT_PATH_MAX + 16];
    SaveScan s = {p, 0};
    if (snprintf(p, sizeof p, "%s/save", dir) < (int)sizeof p && plat_dir_exists(p)) plat_fs_list(p, scan_account, &s);
    if (!s.found && snprintf(p, sizeof p, "%s/save-tx", dir) < (int)sizeof p && plat_dir_exists(p))
        plat_fs_list(p, scan_ready, &s.found);
    return s.found;
}

typedef struct { int ids[PLAYERS_MAX], n; } IdScan;
static int scan_id(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    IdScan *s = ctx;
    int id = folder_id(name);
    if (id && (kind == PLAT_FS_DIR || kind == PLAT_FS_LINK) && s->n < PLAYERS_MAX) s->ids[s->n++] = id;
    return 1;
}
static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

// Holding lock: 1, the numbered folders under players/ and the active player, sorted, each once.
static void list_ids(const char *profile, IdScan *s) {
    char p[PLAT_PATH_MAX + 16];
    s->n = 0;
    s->ids[s->n++] = 1;
    if (snprintf(p, sizeof p, "%s/players", profile) < (int)sizeof p && plat_dir_exists(p))
        plat_fs_list(p, scan_id, s);
    int a = players_active(), seen = 0;
    for (int i = 0; i < s->n; i++) seen |= s->ids[i] == a;
    if (!seen && s->n < PLAYERS_MAX) s->ids[s->n++] = a;
    qsort(s->ids, (size_t)s->n, sizeof s->ids[0], cmp_int);
}

int players_list(PlayerInfo *out, int cap) {
    const char *profile = rt_config()->profile;
    IdScan s;
    plat_mutex_lock(&lock);
    list_ids(profile, &s);
    int n = 0;
    for (int i = 0; i < s.n && n < cap; i++) {
        char dir[PLAT_PATH_MAX];
        if (!player_dir(profile, s.ids[i], dir, sizeof dir)) continue;
        out[n].id = s.ids[i];
        read_name(dir, out[n].name, sizeof out[n].name);
        out[n].has_save = has_save(dir);
        n++;
    }
    plat_mutex_unlock(&lock);
    return n;
}

int players_create(const char *name) {
    const char *profile = rt_config()->profile;
    IdScan s;
    char dir[PLAT_PATH_MAX];
    plat_mutex_lock(&lock);
    list_ids(profile, &s);
    int id = s.n < PLAYERS_MAX ? s.ids[s.n - 1] + 1 : 0;
    // a folder that is there but not listed (a file named 7, a folder past PLAYERS_MAX) is not taken
    while (id && id <= ID_MAX && player_dir(profile, id, dir, sizeof dir) && plat_fs_kind(dir) != PLAT_FS_MISSING) id++;
    int ok = id && id <= ID_MAX && player_dir(profile, id, dir, sizeof dir);
    if (ok) {
        plat_file_mkdir_p(dir);
        ok = plat_dir_exists(dir) && (!name || !*name || write_name(dir, name));
    }
    plat_mutex_unlock(&lock);
    if (!ok) {
        rt_log("players: cannot make a new player folder%s%s", id ? " " : "", id ? dir : "");
        return 0;
    }
    rt_log("players: new player %d in %s", id, dir);
    return id;
}

int players_rename(int id, const char *name) {
    char dir[PLAT_PATH_MAX];
    if (id < 1 || !player_dir(rt_config()->profile, id, dir, sizeof dir)) return 0;
    plat_mutex_lock(&lock);
    plat_file_mkdir_p(dir);   // the active player before the game's first save
    int ok = plat_dir_exists(dir) && write_name(dir, name);
    plat_mutex_unlock(&lock);
    if (!ok) rt_log("players: cannot write the name of player %d in %s", id, dir);
    return ok;
}

int players_remove(int id) {
    const char *profile = rt_config()->profile;
    char dir[PLAT_PATH_MAX], bin[PLAT_PATH_MAX], to[PLAT_PATH_MAX + 64];
    if (id <= 1 || id == players_active()) return 0;
    if (!player_dir(profile, id, dir, sizeof dir) ||
        snprintf(bin, sizeof bin, "%s/players-removed", profile) >= (int)sizeof bin)
        return 0;
    struct tm t;
    plat_gmtime((int64_t)(plat_unix_time_ns() / 1000000000u), &t);
    plat_mutex_lock(&lock);
    snprintf(to, sizeof to, "(nothing: the folder was not made)");
    int ok = plat_fs_kind(dir) == PLAT_FS_MISSING;   // nothing there: nothing to keep
    if (!ok) {
        plat_file_mkdir_p(bin);
        int n = snprintf(to, sizeof to, "%s/%d %04d%02d%02d-%02d%02d%02d", bin, id, t.tm_year + 1900, t.tm_mon + 1,
                         t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
        for (unsigned k = 2; plat_fs_kind(to) != PLAT_FS_MISSING && k < 100; k++)   // two in one second
            snprintf(to + n, sizeof to - (size_t)n, " (%u)", k);
        ok = !plat_fs_rename(dir, to, 1);
    }
    plat_mutex_unlock(&lock);
    if (ok) rt_log("players: player %d removed; the saves are kept in %s", id, to);
    else rt_log("players: cannot move %s to %s (os error %d)", dir, bin, plat_fs_os_error());
    return ok;
}
