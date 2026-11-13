// Co-op, the game side (coop_game.h), for Ocarina 1.1.1: where this game's Link is, for the others, and the progress
// the players of a room share. It runs on the game's thread between two frames (coop_present, at the present) and
// talks to the room through coop.h; what the room sends arrives on the co-op thread and waits in an inbox.
//
// Link. The Player actor is [[main+0x4e371d0] + 0x480] while its name, [+0x20], reads "Player": its position in metres
// at +0x31c, its rotation as a 3x3 matrix at +0x328. The area is the game's text flag Sequence_CurrentBanc
// (debug_tools.c dbg_area). A pose goes out only while a save is being played (gate below): on the title screen and
// while an area loads the others see no Link of this game.
//
// The flag database. The game keeps its progress in one manager, *(u64 *)(main+0x4e842a8), as typed containers of 0x60
// bytes from +8: Bool at +0x008, Int at +0x0c8, UInt at +0x788. A container holds its entry count (+0x08), entries
// (+0x10), an open-addressing table of {u32 hash, u32 entry} slots probed from hash % capacity (+0x18, capacity +0x24) and a
// ring of pending sets (+0x28 capacity, +0x30 slots, +0x38 counter, its low 20 bits the count). A Bool entry is 0x18 bytes
// (+0x08 the reset kinds, +0x10 the save file, -1 none, +0x15 default, +0x16 current value), an Int or UInt entry 0x28
// (+0x08, +0x10 the same, +0x18 default, +0x1c current). Structs group flags: the struct table (+0xc10 count, +0xc18
// entries of 0x20, +0xc20 slots, +0xc2c capacity) maps a struct's hash to an entry whose member table (+0x08, capacity +0x14)
// holds {u32 member hash, u32 flag or struct hash} slots. Flags are named by MurmurHash3 of their names; the names stay out
// of this file, the hashes it needs are numbers below.
//
// Writes go through the game's own set routines, SetBool (main+0x682b20) and SetInt (main+0x163b70), with x0 the manager,
// x1 the value and x2 the hash: they queue the change on the ring and the game's per-frame drain writes the current and
// saved values and its own bookkeeping, as for a change the game made. Reads are of the entries' current values.
//
// What is shared (docs/coop.md has the reasons), each value only ever raised:
// - progress: every Bool that goes to progress.sav (save file 0) and is reset only by a new game or a load (reset kinds 8,
//   16 and 32; flags with kind 1 or 256 revert at scene changes, 2 at a new day, 4 are options), except the heart
//   rewards below. A flag counts as 1 once it differs from its default (63 of them start true and count down), and a
//   flag anyone has at 1 becomes 1 for everyone. Story, world and dungeon flags are one kind: the game's flags carry
//   no mark of which is which.
// - items, songs and quest items: the songs' and the items' "got it once" markers, the item counts of the pouch struct
//   (except the lists below), the capacity struct (wallet, bag and quiver sizes) and the magic meter's size; the
//   highest count wins.
// - per player always: small keys, rupees and ammunition (counters the game spends: a highest-count merge would hand
//   back what was spent), the trade sequence, rented masks, the knife that breaks and amiibo items (the game takes
//   them away again), heart pieces, gold skulltula tokens and the 11 heart reward flags (their pickups are flags of
//   single objects this layer does not write, so a shared count would pay a piece twice); life, magic, bottle
//   contents, the equipment worn and the time of day.
//
// The messages (kinds from COOP_GAME_KIND, reliable): FLAGS is u32 schema and up to 140 pairs of u32 hash and u32
// value, sent to everyone when a value rises here and to one player as the parts of this game's whole state; ASK is
// u32 schema, "send me your whole state", sent when this game starts to share (a save is played in a room) and once
// more a few seconds later, since a player who joined a moment ago may not be on every game's list yet. The schema
// is a sum over this file's table (every shared hash with its type and limit): games whose tables differ do not
// merge. A value from the room is checked against the table before it is kept: the flag must be a shared one, a
// Bool's value 1, a count at most 999; anything else is dropped and counted in the log.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop.h"
#include "coop_game.h"
#include "cpu.h"
#include "debug_tools.h"
#include "fs.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#define MGR_PTR (MOD_MAIN_BASE + 0x4e842a8ull)
#define SET_BOOL_FN (MOD_MAIN_BASE + 0x682b20ull)
#define SET_INT_FN (MOD_MAIN_BASE + 0x163b70ull)
#define PLAYER_PTR (MOD_MAIN_BASE + 0x4e371d0ull)

enum {
    C_BOOL = 0x008, C_INT = 0x0c8, C_UINT = 0x788,
    CN_COUNT = 0x08, CN_ENTRIES = 0x10, CN_TABLE = 0x18, CN_CAP = 0x24,
    BOOL_SIZE = 0x18, INT_SIZE = 0x28,
    E_RESET = 0x08, E_FILE = 0x10, B_DEFAULT = 0x15, B_CURRENT = 0x16, I_CURRENT = 0x1c,
    ST_COUNT = 0xc10, ST_ENTRIES = 0xc18, ST_TABLE = 0xc20, ST_CAP = 0xc2c, ST_SIZE = 0x20, STE_MEMBERS = 0x08, STE_MCAP = 0x14,
    MGR_SIZE = 0xd80,
    RESET_NEW_GAME = 8 | 16 | 32, RESET_TEMPORARY = 1 | 2 | 4 | 256,
    SLICES = 4,             // a quarter of the flags is read each frame: every flag every 4 frames
    APPLY_PER_FRAME = 16,
    INT_MAX_ITEM = 999,     // the highest item count the room may set: the game's counters show three digits
    CLOCK_MS = 2500,        // the game clock advanced this recently: the game is being played
    K_FLAGS = COOP_GAME_KIND, K_ASK,
    PAIRS_PER_MESSAGE = 140,
    INBOX = 16384,          // pairs waiting for the game's thread
    JOBS = 8,
    PARTS_PER_FRAME = 4,    // of a whole state on its way to one player
    ASK_AGAIN_MS = 6000,
    SAVE_COPIES = 12,
};

// Struct and member hashes (MurmurHash3 of the names the game uses).
#define S_POUCH 0x3da094e3u      // item counts, one struct per item
#define M_STOCK 0x22344481u      // an item's count
#define S_FIRST_GET 0xade0ef78u  // "got it once" markers, one per item
#define S_DUNGEON 0xf5a32cdcu    // one struct per dungeon: map, compass, boss key, small keys
#define M_SMALL_KEYS 0x1af87831u
#define S_CAPACITY 0xf80f8077u   // wallet, bomb bag, quiver, seed bag, stick and nut capacities
#define F_RUPEES 0xa77921d7u
#define F_MAGIC_SIZE 0x12d63a97u
#define F_PLAY_CLOCK 0xe53eb6d1u // UInt: seconds played, advanced while the game runs (on the title screen too)
#define F_LOADING 0xf5deef0au    // Bool: a save is being loaded

// Scene changes in progress (Bool, not saved): nothing is read or written while one is set.
static const uint32_t transition_flags[] = {0x9a9506e7, 0xf8633c7f, 0xf96989b6, 0xe19dfdd6};
static const uint32_t heart_reward_flags[] = {0x6b8fc751, 0x1d1793e9, 0xb5127e75, 0xe41e1597, 0x98ca7aab, 0xf1891f6e,
                                              0x3f43245b, 0x1657d7d8, 0xa666baa8, 0x5ff7aa30, 0xf2fab510};
// Item counts that stay with each player: what the game takes away again, heart pieces and skulltula tokens.
static const uint32_t own_counts[] = {0x07f10aaf, 0x0165e7ca, 0x51d89ef0, 0x07de3e6f, 0x58c9bc78, 0xa862d276, 0x2815c82a,
                                      0x897cf7eb, 0x919ea324, 0x5c41a6b1, 0xded8e8d3, 0x24596e8e, 0xb6ca6093, 0x6be5c3b0,
                                      0x7e607033, 0x0e94a8df, 0xfc508da7, 0xb5c10899, 0x47b2d775, 0xf046629e, 0x96459cd3,
                                      0x924e9a46, 0xf5e647da, 0x486c59a4, 0x49d1c374, 0x20820479, 0x396a5c02, 0x8d02f5ea,
                                      0x600e29eb, 0x51ec97b3, 0x04735978};
// Ammunition counts: spent by the game, so per player.
static const uint32_t ammo_counts[] = {0xdb1bcd4f, 0xdf7c517a, 0x518ec74b, 0xf30d9f54, 0x5fadf7fc, 0xb9494411, 0xb003124d};

static int in_list(const uint32_t *list, size_t n, uint32_t h) {
    for (size_t i = 0; i < n; i++)
        if (list[i] == h) return 1;
    return 0;
}
#define IN(list, h) in_list(list, sizeof list / sizeof list[0], h)

typedef struct {
    uint32_t hash;
    uint8_t is_int, inv;
    uint8_t pending;        // want is above the game's value: on the list of flags to set
    uint32_t max;           // the highest value the room may set: 1 for a Bool, INT_MAX_ITEM for a count
    int32_t last;           // the raw value last read
    uint32_t sent;          // the value the room last heard from this game
    uint32_t want;          // the highest value the room has sent
    uint64_t addr;          // the entry
} GFlag;

// A set of hashes (open addressing, 0 empty), for the struct memberships while the table is built.
typedef struct { uint32_t *h; unsigned cap; } HSet;
static void hset_add(HSet *s, uint32_t h) {
    if (!h || !s->h) return;
    unsigned i = h & (s->cap - 1);
    while (s->h[i] && s->h[i] != h) i = (i + 1) & (s->cap - 1);
    s->h[i] = h;
}
static int hset_has(const HSet *s, uint32_t h) {
    if (!h || !s->h) return 0;
    unsigned i = h & (s->cap - 1);
    while (s->h[i]) {
        if (s->h[i] == h) return 1;
        i = (i + 1) & (s->cap - 1);
    }
    return 0;
}

typedef struct { uint32_t hash, value; } Pair;
typedef struct { uint64_t to; uint64_t at; int next; int live; } Job;   // this game's whole state on its way to one player

// The game thread's state.
static struct {
    unsigned cfg_seen;
    int on, share, said_version;
    // the table
    uint64_t mgr, bool_entries, int_entries;
    int bound;
    GFlag *flags;
    int nflags, cap;
    int *by_hash;
    uint32_t schema;
    int slice;
    int *todo;              // indices of flags with pending set
    int ntodo;
    // the gate
    uint64_t gate_mgr, clock_entry, loading_entry, transition_entry[4];
    uint32_t clock;
    uint64_t clock_at;
    int titles, loaded, player, gate;
    // the room
    int sharing;
    uint32_t room_epoch;    // X.room_epoch when sharing started: another room since starts it again
    int asks_left;          // ASK messages still to send, the next at ask_at
    uint64_t ask_at;
    Job jobs[JOBS];
    uint32_t refused, applied;
    uint64_t log_at;
    // the pose
    float last_pos[3];
    uint64_t last_pos_at;
    int posed;
} G;

// What the co-op thread hands over.
static struct {
    PlatMutex lock;
    volatile int32_t room;        // in a room (coop_game_room)
    volatile uint32_t room_epoch; // counts the rooms entered
    volatile int32_t on;          // the COOP setting, for coop_game_rom_open on the game's file threads
    volatile int32_t save_copied; // the save was copied for this room: values may be set
    volatile int32_t titles;      // the title screen's starts (coop_game_rom_open)
    int n;
    Pair *pairs;                  // INBOX of them
    int overflow;                 // pairs were dropped: ask for the whole state again
    int njoined, nasked;
    uint64_t joined[JOBS], asked[JOBS];
    uint32_t schema;              // the table's, for the co-op thread's check of a message; 0 while there is none
    uint32_t other_schema;        // messages with another schema arrived (logged by the game thread)
} X;

static int rd_ok(uint64_t a, uint64_t n) { return a >= 0x1000 && mem_readable(a, n); }

// The entry of hash in the container at base (stride bytes per entry), 0 when absent.
static uint64_t entry_of(uint64_t mgr, unsigned base, unsigned stride, uint32_t hash) {
    uint64_t c = mgr + base, table = LD64(c + CN_TABLE), entries = LD64(c + CN_ENTRIES);
    uint32_t cap = LD32(c + CN_CAP), count = LD32(c + CN_COUNT);
    if (!hash || !cap || !table || !entries) return 0;
    for (uint32_t i = hash % cap, k = 0; k < cap; k++, i = i + 1 == cap ? 0 : i + 1) {
        uint32_t h = LD32(table + 8ull * i);
        if (h == hash) {
            uint32_t idx = LD32(table + 8ull * i + 4);
            return entries + (uint64_t)(idx < count ? idx : 0) * stride;
        }
        if (!h) return 0;
    }
    return 0;
}

static uint64_t struct_of(uint64_t mgr, uint32_t hash) {
    uint64_t table = LD64(mgr + ST_TABLE), entries = LD64(mgr + ST_ENTRIES);
    uint32_t cap = LD32(mgr + ST_CAP), count = LD32(mgr + ST_COUNT);
    if (!cap || !table || !entries || !rd_ok(table, 8ull * cap)) return 0;
    for (uint32_t i = hash % cap, k = 0; k < cap; k++, i = i + 1 == cap ? 0 : i + 1) {
        uint32_t h = LD32(table + 8ull * i);
        if (h == hash) {
            uint32_t idx = LD32(table + 8ull * i + 4);
            uint64_t e = entries + (uint64_t)(idx < count ? idx : 0) * ST_SIZE;
            return rd_ok(e, ST_SIZE) ? e : 0;
        }
        if (!h) return 0;
    }
    return 0;
}

// fn(member hash, flag or struct hash, user) for each member of struct entry e.
static void members(uint64_t e, void (*fn)(uint32_t m, uint32_t v, void *u), void *u) {
    uint64_t t = LD64(e + STE_MEMBERS);
    uint32_t cap = LD32(e + STE_MCAP);
    if (!t || !cap || cap > 65536 || !rd_ok(t, 8ull * cap)) return;
    for (uint32_t i = 0; i < cap; i++) {
        uint32_t m = LD32(t + 8ull * i);
        if (m) fn(m, LD32(t + 8ull * i + 4), u);
    }
}

typedef struct { uint64_t mgr; HSet *set; uint32_t want; } Walk;
static void add_member(uint32_t m, uint32_t v, void *u) { (void)m, hset_add(((Walk *)u)->set, v); }
static void pick(uint32_t m, uint32_t v, void *u) {
    Walk *w = u;
    if (m == w->want) hset_add(w->set, v);
}
// A struct of structs: the flag of member want in each.
static void add_nested(uint32_t m, uint32_t v, void *u) {
    (void)m;
    Walk *w = u;
    uint64_t e = struct_of(w->mgr, v);
    if (e) members(e, pick, w);
}

static GFlag *add_flag(void) {
    if (G.nflags == G.cap) {
        int nc = G.cap ? G.cap * 2 : 4096;
        GFlag *nf = realloc(G.flags, (size_t)nc * sizeof *nf);
        if (!nf) return NULL;
        G.flags = nf, G.cap = nc;
    }
    GFlag *f = &G.flags[G.nflags++];
    memset(f, 0, sizeof *f);
    return f;
}

static int cmp_by_hash(const void *a, const void *b) {
    uint32_t x = G.flags[*(const int *)a].hash, y = G.flags[*(const int *)b].hash;
    return x < y ? -1 : x > y;
}

// The table of the manager's shared flags (the header comment's rules). 0 when the database is not readable.
static int build_table(uint64_t mgr) {
    G.nflags = 0;
    HSet first_get = {0}, pouch = {0}, keys = {0}, capacity = {0};
    HSet *sets[] = {&first_get, &pouch, &keys, &capacity};
    for (int i = 0; i < 4; i++) {
        sets[i]->cap = 1024;
        sets[i]->h = calloc(sets[i]->cap, sizeof *sets[i]->h);
    }
    uint64_t e;
    Walk w = {mgr, &first_get, 0};
    if ((e = struct_of(mgr, S_FIRST_GET))) members(e, add_member, &w);
    w.set = &pouch, w.want = M_STOCK;
    if ((e = struct_of(mgr, S_POUCH))) members(e, add_nested, &w);
    w.set = &keys, w.want = M_SMALL_KEYS;
    if ((e = struct_of(mgr, S_DUNGEON))) members(e, add_nested, &w);
    w.set = &capacity;
    if ((e = struct_of(mgr, S_CAPACITY))) members(e, add_member, &w);

    for (int type = 0; type < 2; type++) {
        unsigned base = type ? C_INT : C_BOOL, stride = type ? INT_SIZE : BOOL_SIZE;
        uint64_t c = mgr + base, table = LD64(c + CN_TABLE), entries = LD64(c + CN_ENTRIES);
        uint32_t cap = LD32(c + CN_CAP), count = LD32(c + CN_COUNT);
        if (!cap || !count || count > 1000000 || !rd_ok(table, 8ull * cap) || !rd_ok(entries, (uint64_t)count * stride)) continue;
        for (uint32_t i = 0; i < cap; i++) {
            uint32_t h = LD32(table + 8ull * i), idx = LD32(table + 8ull * i + 4);
            if (!h || idx >= count) continue;
            uint64_t a = entries + (uint64_t)idx * stride;
            uint32_t reset = LD32(a + E_RESET);
            int32_t file = (int32_t)LD32(a + E_FILE);
            uint8_t inv = 0;
            if (!type) {
                if (file != 0 || reset & RESET_TEMPORARY || !(reset & RESET_NEW_GAME) || IN(heart_reward_flags, h)) continue;
                inv = LD8(a + B_DEFAULT) != 0;
            } else if (h == F_RUPEES || hset_has(&keys, h)) {
                continue;
            } else if (hset_has(&pouch, h)) {
                if (IN(own_counts, h) || IN(ammo_counts, h)) continue;
            } else if (!hset_has(&capacity, h) && h != F_MAGIC_SIZE) {
                continue;
            }
            GFlag *f = add_flag();
            if (!f) break;
            f->hash = h, f->is_int = (uint8_t)type, f->inv = inv, f->addr = a;
            f->max = type ? INT_MAX_ITEM : 1;
        }
    }
    for (int i = 0; i < 4; i++) free(sets[i]->h);
    free(G.by_hash), free(G.todo);
    G.by_hash = malloc((size_t)G.nflags * sizeof *G.by_hash);
    G.todo = malloc((size_t)G.nflags * sizeof *G.todo);
    G.ntodo = 0;
    if (!G.by_hash || !G.todo) return 0;
    for (int i = 0; i < G.nflags; i++) G.by_hash[i] = i;
    qsort(G.by_hash, (size_t)G.nflags, sizeof *G.by_hash, cmp_by_hash);
    // the schema: this layer's version, the game's, and the table (every hash with its type and limit), so that two
    // builds that share or limit differently do not merge
    uint64_t h = 0xcbf29ce484222325ull;
    for (const char *p = "nx2navi-coop-share/1"; *p; p++) h = (h ^ (uint8_t)*p) * 0x100000001b3ull;
    h = (h ^ (uint64_t)rt_game_version) * 0x100000001b3ull;
    for (int i = 0; i < G.nflags; i++) {
        const GFlag *f = &G.flags[G.by_hash[i]];
        uint64_t row[2] = {f->hash | (uint64_t)f->is_int << 40, f->max};
        for (int k = 0; k < 16; k++) h = (h ^ (uint8_t)(row[k / 8] >> 8 * (k % 8))) * 0x100000001b3ull;
    }
    G.schema = (uint32_t)(h ^ h >> 32);
    if (!G.schema) G.schema = 1;
    return G.nflags > 0;
}

static int32_t raw_of(const GFlag *f) { return f->is_int ? (int32_t)LD32(f->addr + I_CURRENT) : LD8(f->addr + B_CURRENT); }
static uint32_t value_of(const GFlag *f, int32_t raw) {
    if (!f->is_int) return (raw != 0) != f->inv;   // 1: differs from its default
    return raw > 0 ? (uint32_t)raw : 0;
}

static int flag_index(uint32_t hash) {
    int lo = 0, hi = G.nflags - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, i = G.by_hash[mid];
        if (G.flags[i].hash == hash) return i;
        if (G.flags[i].hash < hash) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

// ---- the co-op thread's side

void coop_game_init(void) {
    plat_mutex_init(&X.lock);
    X.pairs = calloc(INBOX, sizeof *X.pairs);
}

void coop_game_rom_open(const char *path) {
    // the title screen's event flow is opened each time the title screen starts: at boot and after the pause menu's
    // Title Screen
    if (__atomic_load_n(&X.on, __ATOMIC_RELAXED) && strstr(path, "/Event/EventFlow/TitleEvent.bfevfl")) plat_inc32(&X.titles);
}

// Before anything of a room reaches the save: a copy of it as it is, every slot. Slow (the whole save), so on a
// thread of its own, started when this game begins to share in a room (a save is played then: the game has its save
// storage open); values wait until the copy is there. arg: the room's count, so a copy that ends after the player
// went on to another room does not open that one.
static void copy_save(void *arg) {
    uint32_t epoch = (uint32_t)(uintptr_t)arg;
    char path[PLAT_PATH_MAX];
    int ok = fs_save_snapshot("coop-backup", NULL, SAVE_COPIES, path, sizeof path);
    if (ok && path[0]) rt_log("coop: the save as it was before this room is in %s", path);
    else if (ok) rt_log("coop: nothing is saved yet, so there is no save to copy before this room");
    else rt_log("coop: the save could not be copied; nothing of the room's progress is taken");
    if (ok && epoch == __atomic_load_n(&X.room_epoch, __ATOMIC_ACQUIRE)) __atomic_store_n(&X.save_copied, 1, __ATOMIC_RELEASE);
}

void coop_game_room(int in_room) {
    __atomic_store_n(&X.save_copied, 0, __ATOMIC_RELEASE);
    plat_mutex_lock(&X.lock);
    X.n = 0, X.overflow = 0, X.njoined = X.nasked = 0;
    plat_mutex_unlock(&X.lock);
    if (in_room) __atomic_add_fetch(&X.room_epoch, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&X.room, in_room, __ATOMIC_RELEASE);
}

void coop_game_joined(uint64_t id) {
    plat_mutex_lock(&X.lock);
    if (X.njoined < JOBS) X.joined[X.njoined++] = id;
    plat_mutex_unlock(&X.lock);
}

void coop_game_message(uint64_t from, uint8_t kind, const uint8_t *data, size_t n) {
    if (n < 4) return;
    uint32_t schema;
    memcpy(&schema, data, 4);
    plat_mutex_lock(&X.lock);
    if (X.schema && schema != X.schema) {
        X.other_schema++;
    } else if (kind == K_ASK && n == 4) {
        if (X.nasked < JOBS) X.asked[X.nasked++] = from;
    } else if (kind == K_FLAGS && (n - 4) % 8 == 0 && (n - 4) / 8 <= PAIRS_PER_MESSAGE) {
        size_t count = (n - 4) / 8;
        if (X.pairs && X.n + (int)count <= INBOX) {
            memcpy(X.pairs + X.n, data + 4, count * 8);
            X.n += (int)count;
        } else {
            X.overflow = 1;
        }
    }
    plat_mutex_unlock(&X.lock);
}

// ---- the game's thread

// The table is the save's: another save, a load or the title screen ends it, and sharing with it.
static void unbind(void) {
    G.bound = 0, G.sharing = 0, G.ntodo = 0;   // the gate's own addresses stay: it reads through them this frame
    memset(G.jobs, 0, sizeof G.jobs);
}

static void bind(uint64_t mgr) {
    if (!build_table(mgr)) return;
    G.mgr = mgr, G.bool_entries = LD64(mgr + C_BOOL + CN_ENTRIES), G.int_entries = LD64(mgr + C_INT + CN_ENTRIES);
    for (int i = 0; i < G.nflags; i++) G.flags[i].last = raw_of(&G.flags[i]);
    G.bound = 1, G.slice = 0;
    plat_mutex_lock(&X.lock);
    X.schema = G.schema;
    plat_mutex_unlock(&X.lock);
    rt_log("coop: a save is played: %d flags and counts can be shared", G.nflags);
}

static uint64_t player_actor(void) {
    uint64_t m = rd_ok(PLAYER_PTR, 8) ? LD64(PLAYER_PTR) : 0, a = rd_ok(m + 0x480, 8) ? LD64(m + 0x480) : 0;
    uint64_t name = rd_ok(a + 0x20, 8) ? LD64(a + 0x20) : 0;
    if (!rd_ok(name, 7) || memcmp((const void *)(uintptr_t)name, "Player", 7) || !rd_ok(a + 0x31c, 12 + 36)) return 0;
    return a;
}

// Link stands in the world: the Player actor exists away from the origin. There is none on the title screen at the
// start, so this holds everything back while the first load builds the world; after a return to the title screen the
// actor stays (the title count marks that).
static int player_in_world(float pos[3]) {
    uint64_t a = player_actor();
    if (!a) return 0;
    memcpy(pos, (const void *)(uintptr_t)(a + 0x31c), 12);
    return isfinite(pos[0]) && isfinite(pos[1]) && isfinite(pos[2]) && fabsf(pos[0]) + fabsf(pos[2]) >= 1.0f;
}

// The game is being played: a save was loaded since the title screen, Link stands in the world, the clock runs, no save
// is loading, no scene is changing. Notes a save change (unbind).
static int gate(uint64_t mgr, float pos[3]) {
    if (mgr != G.gate_mgr || !G.clock_entry) {   // the addresses of the few flags read every frame
        G.gate_mgr = mgr;
        G.clock_entry = entry_of(mgr, C_UINT, INT_SIZE, F_PLAY_CLOCK);
        G.loading_entry = entry_of(mgr, C_BOOL, BOOL_SIZE, F_LOADING);
        for (int i = 0; i < 4; i++) G.transition_entry[i] = entry_of(mgr, C_BOOL, BOOL_SIZE, transition_flags[i]);
    }
    if (!G.clock_entry || !rd_ok(G.clock_entry, INT_SIZE)) return 0;
    uint64_t now = plat_now_ms();
    int titles = __atomic_load_n(&X.titles, __ATOMIC_ACQUIRE);
    if (titles != G.titles) {
        G.titles = titles;
        unbind();
        G.loaded = 0;
    }
    uint32_t clock = LD32(G.clock_entry + I_CURRENT);
    int loading = G.loading_entry && LD8(G.loading_entry + B_CURRENT);
    unsigned moving = 0;
    for (int i = 0; i < 4; i++) moving |= (unsigned)(G.transition_entry[i] && LD8(G.transition_entry[i] + B_CURRENT)) << i;
    if (clock != G.clock) {
        // the clock counts seconds one by one, on the title screen too; any other change is a save being loaded (a load,
        // a new game, the same save again from where it was saved), which also sets the flags: play starts at the next
        // tick after one
        int tick = clock > G.clock && clock - G.clock <= 5;
        if (!tick) unbind(), G.loaded = 1;
        G.clock = clock;
        G.clock_at = tick ? now : 0;
    }
    if (loading) unbind(), G.loaded = 1;
    G.player = player_in_world(pos);
    G.gate = G.loaded && G.player && !loading && !moving && G.clock_at && now - G.clock_at < CLOCK_MS;
    return G.gate;
}

static void post_pose(const float pos[3], uint64_t now) {
    CoopPose p;
    memset(&p, 0, sizeof p);
    memcpy(p.pos, pos, 12);
    if (G.posed && now > G.last_pos_at && now - G.last_pos_at < 500) {
        float dt = (float)(now - G.last_pos_at) / 1000.0f;
        for (int i = 0; i < 3; i++) p.vel[i] = (pos[i] - G.last_pos[i]) / dt;
    }
    memcpy(G.last_pos, pos, 12), G.last_pos_at = now, G.posed = 1;
    // the way Link faces, from his rotation's forward column; nothing draws by it yet
    uint64_t a = player_actor();
    float m[9];
    if (a) {
        memcpy(m, (const void *)(uintptr_t)(a + 0x328), sizeof m);
        if (isfinite(m[2]) && isfinite(m[8])) p.yaw = atan2f(m[2], m[8]);
    }
    char area[64];
    if (dbg_area(area)) snprintf(p.scene, sizeof p.scene, "%s", area);
    coop_pose_set(&p);
}

static void set_flag(Cpu *c, uint64_t mgr, int is_int, uint32_t hash, int32_t value) {
    rt_call(c, is_int ? SET_INT_FN : SET_BOOL_FN, 3, mgr, (uint64_t)(uint32_t)value, (uint64_t)hash);
}

static int post_pairs(uint64_t to, const Pair *pairs, int n) {
    uint8_t d[4 + 8 * PAIRS_PER_MESSAGE];
    memcpy(d, &G.schema, 4);
    memcpy(d + 4, pairs, (size_t)n * 8);
    return coop_post(to, K_FLAGS, d, 4 + (size_t)n * 8, 1);
}

// Reads a quarter of the flags; what changed and is above what the room last heard goes to everyone.
static void scan(void) {
    int from = G.nflags * G.slice / SLICES, to = G.nflags * (G.slice + 1) / SLICES;
    G.slice = (G.slice + 1) % SLICES;
    Pair out[PAIRS_PER_MESSAGE];
    int n = 0;
    for (int i = from; i < to && n < PAIRS_PER_MESSAGE; i++) {
        GFlag *f = &G.flags[i];
        int32_t raw = raw_of(f);
        if (raw == f->last) continue;
        f->last = raw;
        uint32_t v = value_of(f, raw);
        if (v > f->max) v = f->max;
        if (v > f->sent) out[n++] = (Pair){f->hash, v};
    }
    if (!n) return;
    int sent = post_pairs(0, out, n);
    for (int i = 0; i < n; i++) {
        GFlag *f = &G.flags[flag_index(out[i].hash)];
        if (sent) f->sent = out[i].value;
        else f->last = INT32_MIN;   // the room's queue is full: read as changed again at the next pass
    }
}

// The inbox into the flags' wanted values.
static void take_inbox(void) {
    static Pair batch[INBOX];
    uint64_t joined[JOBS], asked[JOBS];
    plat_mutex_lock(&X.lock);
    int n = X.n, overflow = X.overflow, nj = X.njoined, na = X.nasked;
    uint32_t other = X.other_schema;
    memcpy(batch, X.pairs, (size_t)n * sizeof *batch);
    memcpy(joined, X.joined, sizeof joined), memcpy(asked, X.asked, sizeof asked);
    X.n = 0, X.overflow = 0, X.njoined = X.nasked = 0, X.other_schema = 0;
    plat_mutex_unlock(&X.lock);
    uint64_t now = plat_now_ms();
    if (other) rt_log("coop: %u messages came from a game that shares other things than this one (another version); not taken", other);
    for (int i = 0; i < n; i++) {
        int k = flag_index(batch[i].hash);
        GFlag *f = k >= 0 ? &G.flags[k] : NULL;
        if (!f || !batch[i].value || batch[i].value > f->max) {
            G.refused++;
            continue;
        }
        if (batch[i].value > f->want) f->want = batch[i].value;
        if (batch[i].value > f->sent) f->sent = batch[i].value;   // the room has it: no need to say it again
        if (!f->pending && f->want > value_of(f, raw_of(f))) f->pending = 1, G.todo[G.ntodo++] = k;
    }
    // a whole state for each player who asked, and (a little later, when it is on everyone's list) for each who came
    for (int pass = 0; pass < 2; pass++) {
        int count = pass ? nj : na;
        const uint64_t *ids = pass ? joined : asked;
        for (int i = 0; i < count; i++)
            for (int j = 0; j < JOBS; j++)
                if (!G.jobs[j].live) {
                    G.jobs[j] = (Job){.to = ids[i], .at = now + (pass ? 2000 : 0), .next = 0, .live = 1};
                    break;
                }
    }
    if (overflow && !G.asks_left) G.asks_left = 1, G.ask_at = now;   // pairs were dropped: the whole state once more
}

static void apply(Cpu *c, uint64_t mgr) {
    if (!__atomic_load_n(&X.save_copied, __ATOMIC_ACQUIRE)) return;   // no copy of the save yet: values wait
    int done = 0;
    while (G.ntodo && done < APPLY_PER_FRAME) {
        int k = G.todo[--G.ntodo];
        GFlag *f = &G.flags[k];
        f->pending = 0;
        if (f->want <= value_of(f, raw_of(f))) continue;   // the game got there itself meanwhile
        set_flag(c, mgr, f->is_int, f->hash, f->is_int ? (int32_t)f->want : !f->inv);
        G.applied++, done++;
    }
}

// This game's whole state to the players who wait for it, a few messages a frame.
static void run_jobs(uint64_t now) {
    for (int j = 0; j < JOBS; j++) {
        Job *job = &G.jobs[j];
        if (!job->live || now < job->at) continue;
        for (int part = 0; part < PARTS_PER_FRAME && job->live; part++) {
            Pair out[PAIRS_PER_MESSAGE];
            int n = 0, i = job->next;
            for (; i < G.nflags && n < PAIRS_PER_MESSAGE; i++) {
                const GFlag *f = &G.flags[i];
                uint32_t v = value_of(f, raw_of(f));
                if (v > f->max) v = f->max;
                if (v) out[n++] = (Pair){f->hash, v};
            }
            if (n && !post_pairs(job->to, out, n)) return;   // the room's queue is full: go on at the next frame
            if (!job->to)   // everyone has these now: a later rise is what scan sends
                for (int k = 0; k < n; k++) {
                    GFlag *f = &G.flags[flag_index(out[k].hash)];
                    if (out[k].value > f->sent) f->sent = out[k].value;
                }
            job->next = i;
            if (i >= G.nflags) job->live = 0;
        }
    }
}

void coop_present(Cpu *c) {
    unsigned v = cfg_version();
    if (v != G.cfg_seen) {
        G.cfg_seen = v;
        G.on = cfg_bool("NX2NAVI_COOP_STEAM");
        __atomic_store_n(&X.on, G.on, __ATOMIC_RELAXED);
        G.share = cfg_bool("NX2NAVI_COOP_SHARE");
    }
    if (!G.on) {
        if (G.posed) coop_pose_set(NULL), G.posed = 0;
        return;
    }
    if (!RT_V111) {
        if (!G.said_version) rt_log("coop: co-op knows game 1.1.1 only; this is %d", rt_game_version);
        G.said_version = 1;
        return;
    }
    uint64_t mgr = LD64(MGR_PTR);
    if (!rd_ok(mgr, MGR_SIZE)) {
        unbind();
        return;
    }
    if (G.bound && (mgr != G.mgr || LD64(mgr + C_BOOL + CN_ENTRIES) != G.bool_entries || LD64(mgr + C_INT + CN_ENTRIES) != G.int_entries))
        unbind();
    float pos[3];
    uint64_t now = plat_now_ms();
    if (!gate(mgr, pos)) {
        if (G.posed) coop_pose_set(NULL), G.posed = 0;
        return;
    }
    post_pose(pos, now);
    int in_room = __atomic_load_n(&X.room, __ATOMIC_ACQUIRE);
    if (!in_room || !G.share) {
        if (G.sharing) {   // left the room, or sharing was turned off: what the next room hears starts from nothing
            G.sharing = 0, G.ntodo = 0;
            memset(G.jobs, 0, sizeof G.jobs);
            for (int i = 0; i < G.nflags; i++) G.flags[i].sent = G.flags[i].want = 0, G.flags[i].pending = 0;
        }
        return;
    }
    if (!G.bound) bind(mgr);
    if (!G.bound) return;
    Cpu keep = *c;
    uint32_t epoch = __atomic_load_n(&X.room_epoch, __ATOMIC_ACQUIRE);
    if (!G.sharing || epoch != G.room_epoch) {
        // this game starts to share: the others send what they have, and get what this save has
        G.sharing = 1, G.room_epoch = epoch;
        if (!__atomic_load_n(&X.save_copied, __ATOMIC_ACQUIRE) &&
            !plat_thread_spawn(copy_save, (void *)(uintptr_t)epoch, "coop-save", 0))
            rt_log("coop: the save could not be copied; nothing of the room's progress is taken");
        for (int i = 0; i < G.nflags; i++) G.flags[i].sent = G.flags[i].want = 0, G.flags[i].pending = 0;
        G.ntodo = 0;
        G.asks_left = 2, G.ask_at = now;
        memset(G.jobs, 0, sizeof G.jobs);
        G.jobs[0] = (Job){.to = 0, .at = now, .next = 0, .live = 1};
        G.jobs[1] = (Job){.to = 0, .at = now + ASK_AGAIN_MS, .next = 0, .live = 1};
    }
    if (G.asks_left && now >= G.ask_at && coop_post(0, K_ASK, &G.schema, 4, 1)) G.asks_left--, G.ask_at = now + ASK_AGAIN_MS;
    take_inbox();
    scan();
    apply(c, mgr);
    run_jobs(now);
    memcpy(c->x, keep.x, sizeof c->x);
    memcpy(c->v, keep.v, sizeof c->v);
    c->nzcv = keep.nzcv, c->fpsr = keep.fpsr;
    if (now - G.log_at >= 5000 && (G.applied || G.refused)) {
        if (G.applied) rt_log("coop: took %u values of progress from the room", G.applied);
        if (G.refused) rt_log("coop: %u values from the room were not ones this game shares; dropped", G.refused);
        G.applied = G.refused = 0, G.log_at = now;
    }
}
