// Settings whose values stay on the player's PC: a co-op room's secret and the offer number made for it, the relay's
// address, the player's name, the key a maker removes, and the menu's test hook (which can carry a secret). The log's
// line of what nx2navi.ini sets (cfg.c log_file_settings), "Copy system info" (sysinfo.c) and the copy of nx2navi.ini a
// bug report takes along (save_share.c; launcher/saves.py has the same list) show only that such a key is set.
// Header only, so that the tests that build one of those files alone need nothing else.
#ifndef NX2NAVI_CFG_PRIVATE_H
#define NX2NAVI_CFG_PRIVATE_H

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define CFG_PRIVATE_SHOWN "(set, not shown)"

// key: a setting's name without the NX2NAVI_ prefix, in any case, n bytes of it.
static inline int cfg_private_key_n(const char *key, size_t n) {
    static const char *const keys[] = {"COOP_ROOM", "COOP_SERVER", "COOP_NAME", "COOP_ACCEPT", "COOP_REMOVE",
                                       "MENU_TEST_SET"};
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        size_t k = 0;
        while (k < n && keys[i][k] && toupper((unsigned char)key[k]) == keys[i][k]) k++;
        if (k == n && !keys[i][k]) return 1;
    }
    return 0;
}

static inline int cfg_private_key(const char *key) { return cfg_private_key_n(key, strlen(key)); }

// A copy of the text of an nx2navi.ini (n bytes) in which every private key that has a value reads
// "key = (set, not shown)"; everything else, comments and line ends included, byte for byte. malloc'd, *out_n its
// length; NULL when memory runs out.
static inline char *cfg_ini_scrub(const char *text, size_t n, size_t *out_n) {
    size_t lines = 1;
    for (size_t i = 0; i < n; i++) lines += text[i] == '\n';
    char *out = malloc(n + lines * sizeof CFG_PRIVATE_SHOWN + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t at = 0; at < n;) {
        size_t end = at;
        while (end < n && text[end] != '\n') end++;
        size_t stop = end;   // the line's text without its line end
        if (stop > at && text[stop - 1] == '\r') stop--;
        size_t k0 = at;
        if (at == 0 && n >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3)) k0 = 3;   // a byte order mark before the first key
        while (k0 < stop && (text[k0] == ' ' || text[k0] == '\t')) k0++;
        size_t eq = k0;
        while (eq < stop && text[eq] != '=') eq++;
        size_t k1 = eq;
        while (k1 > k0 && (text[k1 - 1] == ' ' || text[k1 - 1] == '\t')) k1--;
        size_t v0 = eq < stop ? eq + 1 : stop;
        while (v0 < stop && (text[v0] == ' ' || text[v0] == '\t')) v0++;
        int hide = eq < stop && v0 < stop && text[k0] != '#' && text[k0] != ';' && text[k0] != '[' &&
                   cfg_private_key_n(text + k0, k1 - k0);
        if (hide) {
            memcpy(out + o, text + at, eq + 1 - at), o += eq + 1 - at;
            out[o++] = ' ';
            memcpy(out + o, CFG_PRIVATE_SHOWN, sizeof CFG_PRIVATE_SHOWN - 1), o += sizeof CFG_PRIVATE_SHOWN - 1;
            memcpy(out + o, text + stop, end - stop), o += end - stop;   // a carriage return, if the line had one
        } else {
            memcpy(out + o, text + at, end - at), o += end - at;
        }
        if (end < n) out[o++] = '\n';
        at = end + 1;
    }
    out[o] = 0;
    *out_n = o;
    return out;
}

#endif
