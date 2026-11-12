// runtime/trophies.c against a made-up game: a fake guest arena stands for main's image and holds the game-data
// manager's flag table and the actor manager's player the way the 1.1.1 build lays them out (recomp/patches-v111.txt
// names both slots). The check drives the cases trophies_frame has: no save, a save load's silent grants, a live
// unlock with its toast queue, the flag table growing as the play creates flags, and the trophies.dat round trip.
// Built and run by tests/trophies_check.py; prints each failed check and exits 1 on any.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trophies.h"
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

// ---- the runtime trophies.c calls, stubbed against the arena
// The arena is main's whole image so rd() sees every structure inside its module bounds.

enum { ARENA_SIZE = 0x5000000 };   // covers main+0x4e371d0
static uint8_t *arena;
#define AB(off) ((uint64_t)(uintptr_t)arena + (off))

const int rt_game_version = 111;

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

static RtLoaded mod;
int ldr_count(void) { return 1; }
const RtLoaded *ldr_module(int i) { return i == 0 ? &mod : NULL; }

uint32_t mem_query(uint64_t a, RtMemoryInfo *out) { (void)a; (void)out; return 1; }   // nothing outside the arena

int cfg_bool(const char *name) { (void)name; return 1; }
const char *cfg_get(const char *name) { (void)name; return NULL; }

uint64_t plat_now_ns(void) {
    static uint64_t t = 1000000;
    return (t += 16666666u);   // a frame each call: the unlock times differ
}

// The exe's folder is the check's work dir: trophies.dat lands beside it, so two inits exercise the real file.
static char exe_dir[1024];
int plat_exe_path(char *out, size_t cap) {
    if (strlen(exe_dir) + 8 >= cap) return 0;
    snprintf(out, cap, "%s/trophies_check", exe_dir);
    return 1;
}

uint8_t *plat_file_read(const char *path, uint64_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 1) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    buf[n] = 0;
    *size = (uint64_t)n;
    return buf;
}

int plat_file_write_atomic(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(data, 1, n, f) == n;
    if (f) fclose(f);
    return ok;
}

#include "../runtime/trophies.c"

// ---- the fake game: the manager, its flag table and the actor manager's player, all inside the arena

enum {
    OFF_GDM_SLOT = 0x5000,     // *g_main[0x4c7e180] = this; *this = the manager
    OFF_MGR = 0x6000,
    OFF_ENTRIES = 0x10000,     // the flags' 0x18-byte entries
    OFF_TABLE = 0x300000,      // the {hash, index} slots
    OFF_ACT_SLOT = 0x8000,     // *g_main[0x4e371d0] = this; this+0x480 = the player actor
    OFF_NAME = 0x8400,         // the actor's +0x20 points here ("Player" while Link is in the world)
};

enum { FLAG_MAX = 64 };
static uint32_t flag_hash[FLAG_MAX];
static int flag_n;

static void *arenap(uint64_t off) { return arena + off; }

static void put64(uint64_t off, uint64_t v) { memcpy(arenap(off), &v, 8); }
static void put32(uint64_t off, uint32_t v) { memcpy(arenap(off), &v, 4); }

// A flag in the table: an entry with its bit at +0x16, and a {hash, index} slot so flag_get finds it.
static void flag_add(uint32_t hash, int on) {
    put32(OFF_TABLE + (uint64_t)flag_n * 8, hash);
    put32(OFF_TABLE + (uint64_t)flag_n * 8 + 4, (uint32_t)flag_n);
    arena[OFF_ENTRIES + (uint64_t)flag_n * 0x18 + 0x16] = on ? 1 : 0;
    flag_hash[flag_n] = hash;
    flag_n++;
    put32(OFF_MGR + 0x10, (uint32_t)flag_n);   // the count change makes trophies.c rebuild its map
    put32(OFF_MGR + 0x2c, FLAG_MAX);
}

static void flag_set(uint32_t hash, int on) {
    for (int i = 0; i < flag_n; i++)
        if (flag_hash[i] == hash) {
            arena[OFF_ENTRIES + (uint64_t)i * 0x18 + 0x16] = on ? 1 : 0;
            return;
        }
}

// The manager live (a save loaded) or not (the title screen).
static void save_live(int live) { put64(OFF_GDM_SLOT, live ? AB(OFF_MGR) : 0); }
static void player_in(int in) { put64(OFF_ACT_SLOT + 0x480, in ? AB(OFF_ACT_SLOT + 0x500) : 0); }

static void game_reset(void) {
    memset(arena, 0, ARENA_SIZE);
    flag_n = 0;
    memcpy(arenap(0x4c7e180), &(uint64_t){AB(OFF_GDM_SLOT)}, 8);   // the gdm slot
    put64(OFF_GDM_SLOT, 0);                                        // no manager: no save
    memcpy(arenap(0x4e371d0), &(uint64_t){AB(OFF_ACT_SLOT)}, 8);   // the actor manager
    put64(OFF_ACT_SLOT + 0x480, 0);                                // no player actor
    put64(OFF_ACT_SLOT + 0x500 + 0x20, AB(OFF_NAME));              // the actor's name pointer
    memcpy(arenap(OFF_NAME), "Player", 7);
    put64(OFF_MGR + 0x18, AB(OFF_ENTRIES));
    put64(OFF_MGR + 0x20, AB(OFF_TABLE));
    put32(OFF_MGR + 0x2c, FLAG_MAX);
}

// The five triggers' flag hashes (trophies.c keeps them with their names in comments).
enum { F_SWORD = 0x405d8a0f, F_SHIELD = 0x5217da1e, F_GOHMA = 0x6b093b51, F_EMERALD = 0x110b83ec, F_OCARINA = 0x42bd296f };

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: trophies_check <work dir>\n"); return 2; }
    snprintf(exe_dir, sizeof exe_dir, "%s", argv[1]);
    arena = malloc(ARENA_SIZE);
    if (!arena) return 2;
    mod.base = AB(0);
    mod.size = ARENA_SIZE;
    game_reset();
    char dat[1200];
    snprintf(dat, sizeof dat, "%s/trophies.dat", exe_dir);
    remove(dat);   // a fresh run: nothing carried from a previous check

    trophies_init();
    CHECK(g_armed == 5, "1.1.1: the whole set armed, got %d", g_armed);

    // the title screen: no manager, nothing to read, nothing unlocked
    trophies_frame(0);
    CHECK(!g_tr[0].unlocked && trophies_unlocked_next() < 0, "no save: nothing unlocks");

    // a save loads with the sword and shield flags on: both grant silently, before any player is around
    flag_add(F_SWORD, 1), flag_add(F_SHIELD, 1), flag_add(F_GOHMA, 0), flag_add(F_EMERALD, 0), flag_add(F_OCARINA, 0);
    save_live(1);
    trophies_frame(1);
    CHECK(g_tr[0].unlocked && g_tr[1].unlocked, "the save's items grant on the load");
    CHECK(!g_tr[2].unlocked && !g_tr[3].unlocked && !g_tr[4].unlocked, "flags still off: no grant");
    CHECK(trophies_unlocked_next() < 0, "a load's grants queue no toast");

    // Link in the world now: beating Gohma queues its toast; a repeat frame does not unlock twice
    player_in(1);
    flag_set(F_GOHMA, 1);
    trophies_frame(2);
    CHECK(g_tr[2].unlocked, "Gohma's flag unlocks it live");
    int toast = trophies_unlocked_next();
    CHECK(toast == 2, "the live unlock queues the toast (%d)", toast);
    trophies_frame(3);
    CHECK(trophies_unlocked_next() < 0, "no second toast for a held unlock");

    // the flag table grows during the play: the ocarina's flag appears late and still grants
    flag_add(F_OCARINA, 1);
    trophies_frame(4);
    CHECK(g_tr[4].unlocked, "a flag created mid-play still triggers");
    CHECK(trophies_unlocked_next() == 4, "the ocarina's toast queues behind Gohma's");

    // the dirty dat writes once; a new process's init reads the unlocks back
    for (int i = 0; i < g_n; i++) g_tr[i].unlocked = 0;
    dat_load();
    int back = 0;
    for (int i = 0; i < g_n; i++) back += g_tr[i].unlocked;
    CHECK(back == 4, "trophies.dat brings back the four unlocks, got %d", back);

    // a torn file is ignored, not read
    FILE *f = fopen(dat, "wb");
    fputs("garbage not a trophies file", f);
    fclose(f);
    for (int i = 0; i < g_n; i++) g_tr[i].unlocked = 0;
    dat_load();
    back = 0;
    for (int i = 0; i < g_n; i++) back += g_tr[i].unlocked;
    CHECK(back == 0, "a bad trophies.dat grants nothing");

    // back to the title screen (player actor gone with the save) and into the same save again: all flags already
    // held, all silent — no toasts
    save_live(0);
    player_in(0);
    trophies_frame(5);
    save_live(1);
    trophies_frame(6);
    int again = trophies_unlocked_next();
    CHECK(again < 0, "a second load of the same save queues nothing (got %d)", again);

    if (!fails) printf("trophies: all checks passed\n");
    free(arena);
    return fails != 0;
}
