// Trophies (trophies.h): NX2navi's own achievements, in the manner of the consoles' trophy lists. Each trophy is a
// small set of conditions over the game's on/off flags — the named progress markers ("IsGet.Weapon_Sword_Kokiri",
// "Gohma_DefeatBoss") the game keeps in its data manager's flag table — evaluated once per presented frame by
// trophies_frame, next to the cheat VM. A trophy unlocks the frame all of its conditions hold and stays unlocked
// in trophies.dat beside nx2navi.exe.
//
// The flags live under the game-data manager: [[main+GDM] + 0x00] is the manager, whose first 0x60-byte section is
// the flag kind: +0x10 the entries' count, +0x18 the entries (0x18 bytes each, the flag's bit at +0x16 & 1), +0x20
// the {hash, index} lookup table and +0x2c its size. A flag's name hashes to a murmur3 of its characters; the set
// below carries the hashes with the name in a comment. runtime/debug_tools.c reads the same table for its flag menu.
//
// Where a save already holds a trophy's state (played before the feature existed), the trophy grants silently: an
// unlock that lands before the player actor exists — the flag table is read from the save during the load, while
// Link is not yet in the world — marks the trophy without queueing a toast. A trophy marked live_only never grants
// that way — "beat Gohma without taking damage" cannot be told from a save that merely says Gohma is dead.
//
// The managers' addresses are found per game build (the way recomp/patches-v111.txt names the globals it patches)
// in addr_off below; a 0 there leaves every trigger unarmed so the file builds and runs on every version while the
// addresses are pending. "A save is loaded" needs no field of its own: the flag table only exists while a save is
// live. "In play" is the player actor, [[main+ACT]+0x480] with "Player" at +0x20 — the same handle the debug tools
// use to know Link is in the world.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "trophies.h"

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

// ---- condition model
// A condition asks whether one game-data flag compares against a value; today every trigger is "flag is 1".

enum { OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE };

typedef struct {
    uint32_t flag;   // murmur3 of the flag's name
    uint8_t op;
    uint8_t reset;   // kills the trigger while it holds
    uint64_t val;
} TrCond;

typedef struct {
    const char *id, *title, *desc;
    uint8_t points, live_only;
    const TrCond *cond;
    uint8_t ncond;
} TrDef;

// ---- the game-data and actor managers, per game build
// Offsets into main's image: GDM is the slot whose pointer is the game-data manager ([[slot]]), ACT the actor
// manager whose +0x480 holds the player ([[slot]+0x480]). A 0 leaves its feature unarmed for that build. The row is
// in rt_game_version order (110, 111, 121, 143).
static const struct { uint32_t gdm, act; } addr_off[] = {
    /* 1.1.0 */ {0, 0},
    /* 1.1.1 */ {0x4c7e180, 0x4e371d0},
    /* 1.2.1 */ {0, 0},
    /* 1.4.3 */ {0, 0},
};

static int ver_row(void) {
    switch (rt_game_version) {
    case 110: return 0;
    case 111: return 1;
    case 121: return 2;
    default: return 3;
    }
}

// ---- the set
// The flag hashes: murmur3 of the name, as debug_tools.c hashes them; the comment keeps the name. Verified in a
// live 1.1.1 run: each exists in the flag table, off on a save before the event and on after (IsGet.* are the
// possession flags; the ocarina's is the Fairy Ocarina, which is Saria's gift).

#define C(f) {f, OP_EQ, 0, 1}
#define CR(f) {f, OP_EQ, 1, 1}

static const TrCond c_kokiri_sword[] = {C(0x405d8a0f)};     // IsGet.Weapon_Sword_Kokiri
static const TrCond c_kokiri_shield[] = {C(0x5217da1e)};    // IsGet.Weapon_Shield_Deku
static const TrCond c_gohma[] = {C(0x6b093b51)};            // Gohma_DefeatBoss
static const TrCond c_emerald[] = {C(0x110b83ec)};          // IsGet.Item_Stone_Forest
static const TrCond c_saria_ocarina[] = {C(0x42bd296f)};    // IsGet.Item_Ocarina_Fairy

static const TrDef defs[] = {
    {"kokiri_sword", "Kokiri Sword", "Obtain the Kokiri Sword", 5, 0, c_kokiri_sword, 1},
    {"kokiri_shield", "Kokiri Shield", "Obtain the Kokiri Shield", 5, 0, c_kokiri_shield, 1},
    {"gohma", "Gohma Defeated", "Defeat Gohma inside the Deku Tree", 10, 0, c_gohma, 1},
    {"kokiri_emerald", "Kokiri's Emerald", "Receive the Kokiri's Emerald", 10, 0, c_emerald, 1},
    {"saria_ocarina", "Saria's Ocarina", "Receive Saria's ocarina", 10, 0, c_saria_ocarina, 1},
};

// ---- runtime state

typedef struct {
    TrDef def;                    // copied: info strings stay pointers
    uint64_t prev[4];             // a condition's last-read value, for delta reads (ncond <= 4 in the set)
    int unlocked;
    uint64_t at_ms;
} Tr;

static Tr g_tr[sizeof(defs) / sizeof(defs[0])];
static int g_n, g_armed;
static int g_on = -1;             // cfg, read on the first frame so the ini row exists before trophies_init logs it
static char g_path[PLAT_PATH_MAX];   // trophies.dat beside the exe
static int g_dirty;
static int g_queue[16];           // unlocked indices the toast has not shown
static int g_qh, g_qt;

// ---- guest memory: the cheat VM's rule — a range main's image or mem_query allows is read straight

static uint64_t g_main, g_main_size;

static int rd(uint64_t a, void *v, size_t n) {
    if (!a) return 0;
    if (!(a >= g_main && n <= g_main_size && a - g_main <= g_main_size - n)) {
        RtMemoryInfo mi;
        if (mem_query(a, &mi) || !mi.state || !(mi.perm & 1) || a + n > mi.base + mi.size) return 0;
    }
    memcpy(v, (const void *)(uintptr_t)a, n);
    return 1;
}

static uint64_t rd_ptr(uint64_t a) {
    uint64_t v = 0;
    rd(a, &v, 8);
    return v;
}

static int cmp(int op, uint64_t a, uint64_t b) {
    switch (op) {
    case OP_EQ: return a == b;
    case OP_NE: return a != b;
    case OP_LT: return a < b;
    case OP_LE: return a <= b;
    case OP_GT: return a > b;
    default: return a >= b;
    }
}

// ---- the flag table
// gmd() is the manager, 0 while none is live. flags holds the {hash, index} pairs of the current manager's flag
// kind, sorted for bsearch; it rebuilds when the manager or its table changes (a load, a return to the title).

typedef struct { uint32_t hash, index; } Pair;

static uint64_t g_gdm, g_act;     // the managers' guest addresses this build, 0 on pending rows
static uint64_t g_mgr, g_table;   // the manager and flag table the map was built from
static uint32_t g_count;          // the table's entry count when the map was built
static Pair *g_flags;
static int g_flag_n;

static uint64_t gdm(void) {
    uint64_t slot = rd_ptr(g_gdm);
    return slot ? rd_ptr(slot) : 0;
}

static int by_hash(const void *a, const void *b) {
    uint32_t x = ((const Pair *)a)->hash, y = ((const Pair *)b)->hash;
    return x < y ? -1 : x > y;
}

// The flag map for the live manager; 0 flags while there is none. Rebuilds on a change of manager or table.
static int flags_ready(void) {
    uint64_t m = gdm();
    if (!m) {
        g_mgr = g_table = 0;
        return 0;
    }
    // the table can grow as the play creates flags, so the map also rebuilds on a count change
    uint64_t table = rd_ptr(m + 0x20);
    uint32_t count = 0;
    if (!table || !rd(m + 0x10, &count, 4)) {
        g_mgr = g_table = 0;
        return 0;
    }
    if (m == g_mgr && table == g_table && count == g_count) return g_flag_n > 0;
    uint32_t size = 0;
    if (!rd(m + 0x2c, &size, 4) || !count || count > 200000 || size < count || size > 1000000) {
        g_mgr = g_table = 0;
        return 0;
    }
    Pair *slots = malloc(sizeof(Pair) * size), *list = malloc(sizeof(Pair) * count);
    int n = 0;
    if (slots && list && rd(table, slots, sizeof(Pair) * size))
        for (uint32_t i = 0; i < size && n < (int)count; i++)
            if (slots[i].hash && slots[i].index < count) list[n++] = slots[i];
    free(slots);
    if (!list || !n) {
        free(list);
        g_mgr = g_table = 0;
        return 0;
    }
    qsort(list, (size_t)n, sizeof(Pair), by_hash);
    free(g_flags);
    g_flags = list;
    g_flag_n = n;
    g_mgr = m;
    g_table = table;
    g_count = count;
    rt_log("trophies: %d of the game's %u flags listed", n, count);
    return 1;
}

// The player actor, the debug tools' "Link is in the world" handle: [[main+ACT]+0x480], whose actor holds "Player"
// at +0x20. 0 until a play spawns it — and during the title screen, the file select and the load in between, which
// is exactly the window a satisfied trigger grants silently in.
static int in_play(void) {
    if (!g_act) return 1;   // the actor address is pending on this build: never treat a frame as a load
    uint64_t manager = rd_ptr(g_act), actor = manager ? rd_ptr(manager + 0x480) : 0;
    char name[7];
    uint64_t np = actor ? rd_ptr(actor + 0x20) : 0;
    return np && rd(np, name, 7) && !memcmp(name, "Player", 7);
}

// A flag's bit, through the map: -1 while its entry cannot be read.
static int flag_get(uint32_t hash) {
    Pair key = {hash, 0};
    Pair *hit = g_flags ? bsearch(&key, g_flags, (size_t)g_flag_n, sizeof(Pair), by_hash) : NULL;
    if (!hit) return -1;
    uint64_t entries = rd_ptr(g_mgr + 0x18);
    uint8_t raw[0x18];
    if (!entries || !rd(entries + (uint64_t)hit->index * 0x18, raw, sizeof raw)) return -1;
    return raw[0x16] & 1;
}

// ---- trophies.dat: 'NXTR', fmt 1, then per record the id's FNV-1a 64 and the unlock's plat_now_ms

static uint64_t id_hash(const char *s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x100000001b3ull;
    return h;
}

enum { DAT_VER = 1 };

static void dat_save(void) {
    if (!g_path[0]) return;
    uint8_t buf[16 + sizeof(g_tr) / sizeof(g_tr[0]) * 16];
    memcpy(buf, "NXTR", 4);
    uint32_t n = DAT_VER;
    memcpy(buf + 4, &n, 4);
    n = 0;
    for (int i = 0; i < g_n; i++) n += g_tr[i].unlocked;
    memcpy(buf + 8, &n, 4);
    uint32_t k = 12;
    for (int i = 0; i < g_n; i++) {
        if (!g_tr[i].unlocked) continue;
        uint64_t id = id_hash(g_tr[i].def.id);
        memcpy(buf + k, &id, 8);
        memcpy(buf + k + 8, &g_tr[i].at_ms, 8);
        k += 16;
    }
    if (!plat_file_write_atomic(g_path, buf, k)) rt_log("trophies: cannot write %s", g_path);
}

static void dat_load(void) {
    uint64_t size = 0;
    uint8_t *buf = plat_file_read(g_path, &size);
    if (!buf) return;
    if (size < 12 || memcmp(buf, "NXTR", 4)) {
        rt_log("trophies: %s is not a trophies file; ignored", g_path);
        free(buf);
        return;
    }
    uint32_t ver, n;
    memcpy(&ver, buf + 4, 4);
    memcpy(&n, buf + 8, 4);
    if (ver != DAT_VER || (uint64_t)n * 16 > size - 12) {
        rt_log("trophies: %s is fmt %u or cut short; ignored", g_path, ver);
        free(buf);
        return;
    }
    int back = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t id, at;
        memcpy(&id, buf + 12 + i * 16, 8);
        memcpy(&at, buf + 20 + i * 16, 8);
        for (int t = 0; t < g_n; t++) {
            if (id_hash(g_tr[t].def.id) != id) continue;
            g_tr[t].unlocked = 1;
            g_tr[t].at_ms = at;
            back++;
        }
    }
    free(buf);
    if (back) rt_log("trophies: %d already unlocked (%s)", back, g_path);
}

// ----

static void unlock(Tr *t, int silent) {
    t->unlocked = 1;
    t->at_ms = plat_now_ms();
    g_dirty = 1;
    if (!silent && g_qt - g_qh < (int)(sizeof(g_queue) / sizeof(g_queue[0]))) g_queue[g_qt++ % 16] = (int)(t - g_tr);
    rt_log("trophies: %s unlocked%s", t->def.id, silent ? " (silent grant: the save already held it)" : "");
}

// A condition's read: 1 when it holds, updating the condition's previous value either way.
static int cond_true(const TrCond *c, uint64_t *prev) {
    int v = flag_get(c->flag);
    if (v < 0) return 0;
    *prev = (uint64_t)v;
    return cmp(c->op, (uint64_t)v, c->val);
}

int trophies_on(void) {
    if (g_on < 0) g_on = cfg_bool("NX2NAVI_TROPHIES");
    return g_on;
}

void trophies_init(void) {
    const RtLoaded *m = ldr_count() ? ldr_module(0) : NULL;
    if (!m) return;
    g_main = m->base;
    g_main_size = m->size;
    g_gdm = addr_off[ver_row()].gdm ? g_main + addr_off[ver_row()].gdm : 0;
    g_act = addr_off[ver_row()].act ? g_main + addr_off[ver_row()].act : 0;
    char dir[PLAT_PATH_MAX];
    if (plat_exe_path(dir, sizeof dir)) {
        char *cut = plat_last_sep(dir);
        if (cut) {
            cut[1] = 0;
            snprintf(g_path, sizeof g_path, "%strophies.dat", dir);
        }
    }
    g_n = (int)(sizeof(defs) / sizeof(defs[0]));
    for (int i = 0; i < g_n; i++) {
        Tr *t = &g_tr[i];
        memcpy(&t->def, &defs[i], sizeof t->def);
        g_armed += g_gdm && t->def.ncond <= (int)(sizeof(t->prev) / sizeof(t->prev[0]));
    }
    dat_load();
    rt_log("trophies: %d in the set, %d armed%s", g_n, g_armed,
           g_armed < g_n ? " (this build's game-data manager address is still not found)" : "");
}

void trophies_frame(uint64_t frame) {
    (void)frame;
    if (!trophies_on() || !g_armed) return;
    if (!flags_ready()) return;
    // An unlock before the player actor exists is the save's own state surfacing during the load: it grants
    // silently. live_only trophies never grant that way — a feat is not readable from the save's flags — so they
    // stay locked until their conditions hold with Link in the world.
    int play = in_play();
    for (int i = 0; i < g_n; i++) {
        Tr *t = &g_tr[i];
        if (t->unlocked) continue;
        int ok = 1;
        for (int c = 0; ok && c < t->def.ncond; c++) {
            const TrCond *k = &t->def.cond[c];
            int holds = cond_true(k, &t->prev[c]);
            if (k->reset ? holds : !holds) ok = 0;
        }
        if (ok && (play || !t->def.live_only)) unlock(t, !play);
    }
    if (g_dirty) {
        dat_save();
        g_dirty = 0;
    }
}

int trophies_count(void) { return g_n; }

const TrophyInfo *trophies_info(int i) {
    if (i < 0 || i >= g_n) return NULL;
    static TrophyInfo out;
    Tr *t = &g_tr[i];
    out.id = t->def.id;
    out.title = t->def.title;
    out.desc = t->def.desc;
    out.points = t->def.points;
    out.unlocked = t->unlocked;
    out.at_ms = t->at_ms;
    return &out;
}

int trophies_unlocked_next(void) {
    if (g_qh == g_qt) return -1;
    return g_queue[g_qh++ % 16];
}
