// runtime/players.c without the game (tests/players_check.py): one command per run on the profile folder argv[1], the
// player of the run from PLAYERS_CHECK_PLAYER as nx2navi.ini's player key. Names are given in hex (their UTF-8 bytes):
// a Windows command line would turn them into the ANSI code page.
//
//   list                 "<id> <has save> <name>" per player
//   create [name]        "id <n>"
//   rename <id> [name]   "ok <0|1>"
//   remove <id>          "ok <0|1>"
//   dir                  the active player's save folder
//   clean <name>         players_clean_name's result between brackets
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "players.h"

typedef struct { const char *exefs, *romfs, *profile; int boot; double seconds; const char *shot, *gpu; } RtConfig;
static RtConfig config;

const RtConfig *rt_config(void) { return &config; }
const char *cfg_value(const char *name) {
    (void)name;
    const char *p = getenv("PLAYERS_CHECK_PLAYER");
    return p ? p : "1";
}
void rt_log(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    fputs("log: ", stderr);
    vfprintf(stderr, fmt, a);
    fputc('\n', stderr);
    va_end(a);
}
void rt_fatal(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    exit(3);
}

static char *unhex(const char *h) {
    static char buf[4][512];
    static int k;
    char *o = buf[k++ % 4];
    size_t n = 0;
    for (; h[0] && h[1] && n + 1 < sizeof buf[0]; h += 2) {
        char two[3] = {h[0], h[1], 0};
        o[n++] = (char)strtol(two, NULL, 16);
    }
    o[n] = 0;
    return o;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    config.profile = argv[1];
    const char *mode = argv[2];
    if (!strcmp(mode, "list")) {
        PlayerInfo p[PLAYERS_MAX];
        int n = players_list(p, PLAYERS_MAX);
        for (int i = 0; i < n; i++) printf("%d %d %s\n", p[i].id, p[i].has_save, p[i].name);
    } else if (!strcmp(mode, "create")) {
        printf("id %d\n", players_create(argc > 3 ? unhex(argv[3]) : ""));
    } else if (!strcmp(mode, "rename") && argc > 3) {
        printf("ok %d\n", players_rename(atoi(argv[3]), argc > 4 ? unhex(argv[4]) : ""));
    } else if (!strcmp(mode, "remove") && argc > 3) {
        printf("ok %d\n", players_remove(atoi(argv[3])));
    } else if (!strcmp(mode, "dir")) {
        char dir[2048];
        if (!players_save_dir(argv[1], dir, sizeof dir)) return 4;
        printf("%s\n", dir);
    } else if (!strcmp(mode, "clean") && argc > 3) {
        char out[PLAYER_NAME_MAX + 1];
        players_clean_name(unhex(argv[3]), out, sizeof out);
        printf("[%s]\n", out);
    } else {
        return 2;
    }
    return 0;
}
