// Co-op progress sync, the game side (coop_game.h), for Ocarina 1.1.1.
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
// x1 the value and x2 the hash: they queue the change on the ring and the game's per-frame drain (main+0xb99980) writes the
// current and saved values and its own bookkeeping, as for a change the game made. Reads are of the entries' current values.
//
// What is shared (docs/coop.md has the reasons):
// - progress: every Bool that goes to progress.sav (save file 0) and is reset only by a new game or a load (reset kinds 8,
//   16 and 32; flags with kind 1 or 256 revert at scene changes, 2 at a new day, 4 are options), except the songs, the
//   first-get markers and the heart rewards below. Its register is 1 once the flag differs from its default (63 of them
//   start true and count down). Story, world and dungeon flags are one category: the game's flags carry no mark of which
//   is which, and their names do not reach the runtime.
// - items: the item counts of the pouch struct (except the lists below), the capacity struct (wallet, bag and quiver
//   sizes), the magic meter's size, and the first-get struct's markers; the highest count wins.
// - songs: 13 first-get markers. quest: 11 pouch counts (medallions, stones, the stone of agony, the gerudo card).
// - keys: the small-key count of each dungeon (the dungeon struct's member); rupees: the wallet's count; ammunition: 7
//   pouch counts. These three are counters the game spends: shared, they are pools (coop_state.h), else per player.
// - per player always: the trade sequence, rented masks, the knife that breaks, amiibo items and one unknown seed item
//   (a highest-count merge would hand back what the game took away); heart pieces and gold skulltula tokens, and the 11
//   heart reward flags (their pickups are mostly per-object flags this layer does not write, so a shared count would pay a
//   piece twice); life, the magic in the meter, bottle contents, equipment worn, the time of day, Link's place.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop.h"
#include "coop_game.h"
#include "coop_state.h"
#include "fs.h"
#include "platform/platform.h"

#define MGR_PTR 0x7004e842a8ull
#define SET_BOOL_FN 0x7000682b20ull
#define SET_INT_FN 0x7000163b70ull

enum {
    C_BOOL = 0x008, C_INT = 0x0c8, C_UINT = 0x788,
    CN_COUNT = 0x08, CN_ENTRIES = 0x10, CN_TABLE = 0x18, CN_CAP = 0x24, CN_RING_CAP = 0x28, CN_RING = 0x30, CN_RING_CTR = 0x38,
    BOOL_SIZE = 0x18, INT_SIZE = 0x28,
    E_RESET = 0x08, E_FILE = 0x10, B_DEFAULT = 0x15, B_CURRENT = 0x16, I_CURRENT = 0x1c,
    ST_COUNT = 0xc10, ST_ENTRIES = 0xc18, ST_TABLE = 0xc20, ST_CAP = 0xc2c, ST_SIZE = 0x20, STE_MEMBERS = 0x08, STE_MCAP = 0x14,
    MGR_SIZE = 0xd80,
    RESET_NEW_GAME = 8 | 16 | 32, RESET_TEMPORARY = 1 | 2 | 4 | 256,
    SLICES = 4,             // a quarter of the flags is read each frame: every flag every 4 frames
    APPLY_PER_FRAME = 16,
    INT_MAX_ITEM = 999,     // the highest item count or counter the room may set: the game's counters show three digits
    KEY_MAX = 15,           // the most small keys of one dungeon the room may set (the most any dungeon has is 9)
    CLOCK_MS = 2500,        // the game clock advanced this recently: the game is being played
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
#define PLAYER_PTR 0x7004e371d0ull   // [main+0x4e371d0] + 0x480: the Player actor, none before the first load

// Scene changes in progress (Bool, not saved): nothing is read or written while one is set.
static const uint32_t transition_flags[] = {0x9a9506e7, 0xf8633c7f, 0xf96989b6, 0xe19dfdd6};
static const uint32_t song_flags[] = {0x01c20002, 0xb492ac33, 0xd3f8bfcc, 0x165dd1ad, 0x03b3450e, 0x2b39dd4f, 0xdc0a0b54,
                                      0x49041ab9, 0x3fa63986, 0xc2fb95f4, 0x802a36a8, 0xd6320ba4, 0x5e86fb4d};
static const uint32_t heart_reward_flags[] = {0x6b8fc751, 0x1d1793e9, 0xb5127e75, 0xe41e1597, 0x98ca7aab, 0xf1891f6e,
                                              0x3f43245b, 0x1657d7d8, 0xa666baa8, 0x5ff7aa30, 0xf2fab510};
static const uint32_t quest_counts[] = {0x692d57cb, 0x5ce254a6, 0x0151a68f, 0x0c7a5fd0, 0x204d2063, 0x235c7d9a,
                                        0xcbd1e58c, 0x7d69ad47, 0xb8e3bf45, 0xf3b14ce2, 0xfeb4d56b};
static const uint32_t own_counts[] = {0x07f10aaf, 0x0165e7ca, 0x51d89ef0, 0x07de3e6f, 0x58c9bc78, 0xa862d276, 0x2815c82a,
                                      0x897cf7eb, 0x919ea324, 0x5c41a6b1, 0xded8e8d3, 0x24596e8e, 0xb6ca6093, 0x6be5c3b0,
                                      0x7e607033, 0x0e94a8df, 0xfc508da7, 0xb5c10899, 0x47b2d775, 0xf046629e, 0x96459cd3,
                                      0x924e9a46, 0xf5e647da, 0x486c59a4, 0x49d1c374, 0x20820479, 0x396a5c02, 0x8d02f5ea,
                                      0x600e29eb, 0x51ec97b3, 0x04735978};
// Ammunition counts and the capacity member that caps each (0: none known).
static const struct { uint32_t count, cap_member; } ammo[] = {
    {0xdb1bcd4f, 0xc0f40041}, {0xdf7c517a, 0}, {0x518ec74b, 0x59663556}, {0xf30d9f54, 0x83d2e406},
    {0x5fadf7fc, 0x483d0f84}, {0xb9494411, 0xf9e54315}, {0xb003124d, 0},
};
#define CAP_RUPEES 0x3b58ca14u   // the capacity member of the wallet

static int in_list(const uint32_t *list, size_t n, uint32_t h) {
    for (size_t i = 0; i < n; i++)
        if (list[i] == h) return 1;
    return 0;
}
#define IN(list, h) in_list(list, sizeof list / sizeof list[0], h)

typedef struct {
    uint32_t hash;
    uint8_t is_int, cat, inv;
    uint32_t max;           // the highest register value the room may set: 1 for a Bool, KEY_MAX or INT_MAX_ITEM for an Int
    int32_t last;           // the raw value last reported
    uint64_t addr;          // the entry
    uint64_t cap_addr;      // a pool counter's capacity entry (Int), 0 none
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

typedef struct { uint32_t hash; int32_t value; uint32_t frame; int done; } TestSet;
enum { PUB_CONTRIBUTORS = 24 };   // the pools' contributors the menu is shown (the maker's log hint takes as many)

static struct {
    // settings, re-read when cfg_version changes (the game thread's copy and the coop thread's)
    unsigned cfg_seen, cfg_seen_net;
    int on, said_version;
    TestSet test[8];        // NX2NAVI_COOP_TEST_SET
    int ntest;

    PlatMutex lock;         // around eng
    CoopSync *eng;
    uint64_t self_id;

    // the game thread's
    uint64_t mgr, bool_entries, int_entries;
    GFlag *flags;           // flags first, then the pool counters
    int *by_hash;           // indices of flags in hash order
    int nflags, npools, cap;
    unsigned count[COOP_CAT_N];
    int bound, gate, slice;
    int loaded;             // a save was loaded since the title screen last started (its clock runs too)
    int titles;             // title_starts when last seen
    int player;             // the Player actor stands in the world (player_in_world)
    uint32_t clock;
    uint64_t clock_at, clock_entry, loading_entry, transition_entry[4];
    uint32_t frames;        // frames played since the save was bound
    unsigned applied[COOP_CAT_N], applied_total;
    uint64_t applied_log_at;
    unsigned rules_seen;
    uint32_t schema;        // of the table: games merge only with the same
    unsigned rejected_seen, ignored_seen;
    uint64_t checked_at;

    // the gate (G.lock; open is read by the game thread without it): nothing from a room is applied before the player
    // accepted what the room is (the engine's offer: identities, rules and their key, the save) and a copy of the save
    // exists to go back to; coop_sync_take checks it again under the lock
    CoopView view;                  // the last view (the relay's names of members not identified yet)
    char first_rooms[8][COOP_SECRET_TEXT + 1];   // rooms whose first copy of the save was made in this start
    unsigned nfirst;
    uint32_t logged;                // the offer last logged
    int logged_unid, said_disagree; // and its unidentified members; the members' rules differed (logged)
    int said_always;
    int open;                       // the current offer is accepted and the copy exists: the game thread applies

    // the maker's key for a room (the coop thread's): the room it was looked up for, and whether one is kept
    char key_code[COOP_SECRET_TEXT + 1];
    uint8_t room_key[32];
    int has_room_key, said_random;
    // the maker's remedies by setting (coop_remove = a key's fingerprint, coop_reset_pools): the values last acted on
    // (the first ones only noted: a value left from before acts on nothing)
    char remove_seen[32], reset_seen[32];
    int cfg_primed;
    uint64_t spent_seen, spent_at;  // the contributors' spends last logged for the maker, and when
    unsigned own_key_seen;          // digests carrying this game's own key, as last logged
    uint64_t own_key_said;
    unsigned key_changes_seen, key_moves_seen;   // a member's key that changed within its session, an accepted key
                                                 // under another number or name: as last logged
    // nx2navi.ini as it is now (cfg reads it at start only), once a second in a room: coop_accept, and the maker's
    // coop_remove and coop_reset_pools as last acted on (the first reading is only noted)
    uint64_t ini_at;
    int ini_primed;
    char ini_accept[32], ini_remove[32], ini_reset[32];

    // what the menu reads and asks (coop_offer.h) without waiting for G.lock: copies the coop thread publishes at each
    // step under pub_lock, which is held only for a copy, and requests the coop thread carries out at its next step
    PlatMutex pub_lock;
    CoopOffer pub;                  // the offer as offer_of gave it last (and its return value: pub_has)
    int pub_has, pub_maker;         // pub_maker: this game holds the key that signs the rules (coop_sync_is_maker)
    CoopContributor pub_contrib[PUB_CONTRIBUTORS];
    int pub_ncontrib;
    uint64_t pub_contrib_at;        // when the contributors were published (at most every 500 ms; 0: at the next step)
    uint32_t want_accept;           // an offer number the menu accepted (atomic)
    uint32_t pub_shown;             // the members whose puppets are shown (coop_offer_shown; atomic)
    // the maker's remedies the menu asked for, each with when (ms; 0 none): a request the coop thread did not take
    // within 2 s (co-op went off meanwhile) is dropped, so that it never acts in a later room
    uint64_t want_reset;            // atomic
    char want_remove[24];           // a key fingerprint, "" none (pub_lock, as want_remove_at)
    uint64_t want_remove_at;
} G;

#define TRACE_ON (rt_trace & RT_TRACE_COOP)

// ---- reading the database

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

typedef struct { uint64_t mgr; uint32_t member; uint64_t flag; } FindMember;
static void find_member(uint32_t m, uint32_t v, void *u) {
    FindMember *f = u;
    if (m == f->member) f->flag = entry_of(f->mgr, C_INT, INT_SIZE, v);
}
// The Int entry a struct member names, 0 when absent.
static uint64_t member_entry(uint64_t mgr, uint32_t s, uint32_t member) {
    uint64_t e = struct_of(mgr, s);
    FindMember f = {mgr, member, 0};
    if (e) members(e, find_member, &f);
    return f.flag;
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

static int cmp_pool_last(const void *a, const void *b) {
    return coop_cat_is_pool(((const GFlag *)a)->cat) - coop_cat_is_pool(((const GFlag *)b)->cat);
}

static int cmp_by_hash(const void *a, const void *b) {
    uint32_t x = G.flags[*(const int *)a].hash, y = G.flags[*(const int *)b].hash;
    return x < y ? -1 : x > y;
}

// The classified table of the manager's flags (the header comment's rules). 0 when the database is not readable.
static int build_table(uint64_t mgr) {
    G.nflags = G.npools = 0;
    memset(G.count, 0, sizeof G.count);
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
            int cat = -1;
            uint8_t inv = 0;
            uint64_t cap_addr = 0;
            if (!type) {
                if (file != 0 || reset & RESET_TEMPORARY || !(reset & RESET_NEW_GAME) || IN(heart_reward_flags, h)) continue;
                cat = IN(song_flags, h) ? COOP_CAT_SONGS : hset_has(&first_get, h) ? COOP_CAT_ITEMS : COOP_CAT_PROGRESS;
                inv = LD8(a + B_DEFAULT) != 0;
            } else if (h == F_RUPEES) {
                cat = COOP_CAT_RUPEES;
                cap_addr = member_entry(mgr, S_CAPACITY, CAP_RUPEES);
            } else if (hset_has(&keys, h)) {
                cat = COOP_CAT_KEYS;
            } else if (hset_has(&pouch, h)) {
                if (IN(own_counts, h)) continue;
                cat = IN(quest_counts, h) ? COOP_CAT_QUEST : COOP_CAT_ITEMS;
                for (size_t k = 0; k < sizeof ammo / sizeof ammo[0]; k++)
                    if (ammo[k].count == h) {
                        cat = COOP_CAT_AMMO;
                        if (ammo[k].cap_member) cap_addr = member_entry(mgr, S_CAPACITY, ammo[k].cap_member);
                    }
            } else if (hset_has(&capacity, h) || h == F_MAGIC_SIZE) {
                cat = COOP_CAT_ITEMS;
            }
            if (cat < 0) continue;
            GFlag *f = add_flag();
            if (!f) break;
            f->hash = h, f->is_int = (uint8_t)type, f->cat = (uint8_t)cat, f->inv = inv, f->addr = a, f->cap_addr = cap_addr;
            f->max = !type ? 1 : cat == COOP_CAT_KEYS ? KEY_MAX : INT_MAX_ITEM;
            G.count[cat]++;
        }
    }
    for (int i = 0; i < 4; i++) free(sets[i]->h);
    qsort(G.flags, (size_t)G.nflags, sizeof *G.flags, cmp_pool_last);   // the scan reads flags in slices, the pools every frame
    for (int i = 0; i < G.nflags; i++) G.npools += coop_cat_is_pool(G.flags[i].cat);
    free(G.by_hash);
    G.by_hash = malloc((size_t)G.nflags * sizeof *G.by_hash);
    if (!G.by_hash) return 0;
    for (int i = 0; i < G.nflags; i++) G.by_hash[i] = i;
    qsort(G.by_hash, (size_t)G.nflags, sizeof *G.by_hash, cmp_by_hash);
    // the schema: this layer's version, the game's, and the table (every hash with its category, type and limit), so
    // that two builds that classify or limit differently do not merge
    uint64_t h = 0xcbf29ce484222325ull;
    for (const char *p = "nx2navi-coop-sync/2"; *p; p++) h = (h ^ (uint8_t)*p) * 0x100000001b3ull;
    h = (h ^ (uint64_t)rt_game_version) * 0x100000001b3ull;
    for (int i = 0; i < G.nflags; i++) {
        const GFlag *f = &G.flags[G.by_hash[i]];
        uint64_t row[2] = {f->hash | (uint64_t)f->cat << 32 | (uint64_t)f->is_int << 40, f->max};
        for (int k = 0; k < 16; k++) h = (h ^ (uint8_t)(row[k / 8] >> 8 * (k % 8))) * 0x100000001b3ull;
    }
    G.schema = (uint32_t)(h ^ h >> 32);
    return G.nflags > 0;
}

static int32_t raw_of(const GFlag *f) { return f->is_int ? (int32_t)LD32(f->addr + I_CURRENT) : LD8(f->addr + B_CURRENT); }
static uint32_t reg_of(const GFlag *f, int32_t raw) {
    if (!f->is_int) return (raw != 0) != f->inv;   // 1: differs from its default
    return raw > 0 ? (uint32_t)raw : 0;
}

static GFlag *flag_by_hash(uint32_t hash) {
    int lo = 0, hi = G.nflags - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        GFlag *f = &G.flags[G.by_hash[mid]];
        if (f->hash == hash) return f;
        if (f->hash < hash) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

// A set of this hash waits on the Int ring (the game changed it this frame): a pool write now would overwrite it.
static int int_pending(uint64_t mgr, uint32_t hash) {
    uint64_t c = mgr + C_INT, ring = LD64(c + CN_RING);
    uint32_t n = LD32(c + CN_RING_CTR) & 0xFFFFF, cap = LD32(c + CN_RING_CAP);
    if (!ring || n > cap || !rd_ok(ring, 12ull * n)) return 1;
    for (uint32_t i = 0; i < n; i++)
        if (LD32(ring + 12ull * i + 4) == hash) return 1;
    return 0;
}

// ---- settings

static int mode_of(const char *key, int pool) {
    const char *v = cfg_value(key);
    if (!strcmp(v, "own")) return COOP_MODE_OWN;
    return pool ? (!strcmp(v, "pool") ? COOP_MODE_POOL : COOP_MODE_OWN) : (!strcmp(v, "shared") ? COOP_MODE_SHARED : COOP_MODE_OWN);
}

static void read_rules(CoopRules *r) {
    static const char *const keys[COOP_CAT_N] = {"NX2NAVI_COOP_PROGRESS", "NX2NAVI_COOP_ITEMS", "NX2NAVI_COOP_SONGS", "NX2NAVI_COOP_QUEST",
                                                 "NX2NAVI_COOP_KEYS", "NX2NAVI_COOP_RUPEES", "NX2NAVI_COOP_AMMO"};
    coop_rules_default(r);
    for (int c = 0; c < COOP_CAT_N; c++) r->mode[c] = (uint8_t)mode_of(keys[c], coop_cat_is_pool(c));
    CoopRules as_set = *r;
    if (coop_rules_normalize(r)) {
        char a[200], b[200];
        coop_rules_text(&as_set, a, sizeof a);
        coop_rules_text(r, b, sizeof b);
        rt_log("coop: sync: the settings ask for %s; a room made with them uses %s", a, b);
    }
}

// "hash=value@frame" items separated by ',' or ';' (hash in hex): set that flag at that frame of play, for tests. Read
// from the environment only, never from nx2navi.ini: an ini a player was handed cannot set flags in their save.
static void read_test_aid(void) {
    G.ntest = 0;
    const char *t = cfg_get("NX2NAVI_COOP_TEST_SET");
    if (t && *t && !cfg_from_env("NX2NAVI_COOP_TEST_SET")) {
        rt_log("coop: test: COOP_TEST_SET is taken from the environment only; the nx2navi.ini line is ignored");
        return;
    }
    while (t && *t && G.ntest < (int)(sizeof G.test / sizeof G.test[0])) {
        char *end;
        unsigned long h = strtoul(t, &end, 16);
        if (*end != '=') break;
        long v = strtol(end + 1, &end, 0);
        if (*end != '@') break;
        unsigned long fr = strtoul(end + 1, &end, 0);
        G.test[G.ntest++] = (TestSet){(uint32_t)h, (int32_t)v, (uint32_t)fr, 0};
        while (*end == ',' || *end == ';' || *end == ' ') end++;
        t = end;
    }
    if (t && *t) rt_log("coop: test: COOP_TEST_SET is \"hash=value@frame,...\" (hex hash); stopped at \"%s\"", t);
}

// ---- the engine's side of the coop thread

static void trace_line(void *user, const char *line) { (void)user, rt_log("coop: sync: %s", line); }

// The engine's limits (coop_sync_limits), with G.lock held: a flag or counter of the table in that category.
static uint32_t limit_fn(int cat, uint32_t hash, void *user) {
    (void)user;
    GFlag *f = flag_by_hash(hash);
    return f && f->cat == cat ? f->max : 0;
}

static int ensure_engine(void) {
    if (G.eng) return 1;
    G.self_id = coop_client_id();
    CoopSync *e = coop_sync_new(G.self_id, 0);   // the schema is the table's, once a save is played (bind)
    if (!e) return 0;
    uint8_t key[16 + 32];   // the index's key, and the seed of this start's identities and offer numbers
    if (!plat_random(key, sizeof key)) {   // a seed others could guess would let them sign as this player: no co-op
        coop_sync_free(e);
        if (!G.said_random) rt_log("coop: sync: the system's random source failed; progress sync stays off");
        G.said_random = 1;
        return 0;
    }
    coop_sync_index_key(e, key);
    coop_sync_seed(e, key + 16);
    coop_sync_limits(e, limit_fn, NULL);
    if (TRACE_ON) coop_sync_trace(e, trace_line, NULL);
    plat_mutex_lock(&G.lock);
    G.eng = e;
    plat_mutex_unlock(&G.lock);
    return 1;
}

static int send_fn(int member, uint8_t kind, const uint8_t *bytes, size_t n, void *user) {
    (void)user;
    return member ? coop_send_to(member, kind, bytes, n) : coop_send(kind, bytes, n);
}

static void on_message(int from, const uint8_t *bytes, size_t n, void *user) {
    uint8_t kind = (uint8_t)(uintptr_t)user;
    plat_mutex_lock(&G.lock);
    if (G.eng) coop_sync_receive(G.eng, from, kind, bytes, n, plat_now_ms());
    plat_mutex_unlock(&G.lock);
}

// The maker's key for the room of code (32 random bytes) in <profile>/coop-keys, so that a maker whose game started
// again signs the room's rules and removals with the same key. The file is named by a hash of the room's secret, and
// the key is mixed with the secret before use (coop_state.c), so the file alone signs nothing; with the secret, whoever
// can read it acts as the room's maker, so the folder is this account's alone (0700, the file 0600). make: a room this
// session made gets one when it has none. 1 and the key in key when there is one.
static int room_key(const char *code, int make, uint8_t key[32]) {
    uint8_t h[8];
    coop_blake2b(h, sizeof h, NULL, 0, code, strlen(code));
    char rel[64], path[1200], dir[1200];
    snprintf(rel, sizeof rel, "coop-keys/%02x%02x%02x%02x%02x%02x%02x%02x.key", h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
    if (!fs_profile_path(rel, path, sizeof path) || !fs_profile_path("coop-keys", dir, sizeof dir)) return 0;
    uint64_t n = 0;
    uint8_t *b = plat_file_read(path, &n);
    int ok = b && n == 32, read = b != NULL;
    if (ok) memcpy(key, b, 32);
    free(b);
    if (ok) {
        plat_file_private(dir), plat_file_private(path);   // a folder an earlier release made readable to others
        return 1;
    }
    if (plat_file_exists(path))
        rt_log("coop: sync: the room's key file in %s %s; %s", dir, read ? "is damaged (not 32 bytes)" : "cannot be read",
               make ? "a new key replaces it, so this game no longer signs the rules the room accepted from it" : "this game does not sign the room's rules");
    if (!make || !plat_random(key, 32)) return 0;
    plat_file_mkdir_p(dir);
    plat_file_private(dir);
    if (!plat_file_write_atomic(path, key, 32)) {
        rt_log("coop: sync: the room's key could not be kept in %s; a restart of this game ends its say over the room's rules", dir);
        return 1;
    }
    plat_file_private(path);
    rt_log("coop: sync: the key this game signs the room's rules with is kept in %s (with the room's secret it acts as the room's maker: "
           "never hand out a copy of this folder or of the profile)", dir);
    return 1;
}

static void on_view(const CoopView *v, void *user) {
    (void)user;
    int ids[COOP_MAX_MEMBERS], n = 0;
    uint64_t sessions[COOP_MAX_MEMBERS];
    int in_room = v->state == COOP_JOINED || v->state == COOP_RECONNECTING;
    for (int i = 0; in_room && i < v->n; i++) {
        // this player's own session is the one it joined with (the engine signs for it), whatever a roster lists
        sessions[n] = v->member[i].id == v->self && v->session ? v->session : v->member[i].session;
        ids[n++] = v->member[i].id;
    }
    if (in_room && (strcmp(G.key_code, v->code) || (v->made && !G.has_room_key))) {   // the file once per room, outside the lock
        snprintf(G.key_code, sizeof G.key_code, "%s", v->code);
        G.has_room_key = room_key(v->code, v->made, G.room_key);
    }
    plat_mutex_lock(&G.lock);
    G.view = *v;
    if (G.eng) {
        coop_sync_name(G.eng, v->name);   // the player's own name, signed: the relay's roster may say another
        if (in_room && G.has_room_key) coop_sync_room_seed(G.eng, v->code, G.room_key);
        coop_sync_room(G.eng, in_room ? v->code : "", v->self, ids, sessions, n, in_room ? v->maker : 0, plat_now_ms());
        CoopRules mine;
        read_rules(&mine);
        coop_sync_settings(G.eng, &mine, in_room && v->made);
    }
    plat_mutex_unlock(&G.lock);
}

// ---- the gate

// coop_accept: "always" accepts every offer, but only from the environment, so an nx2navi.ini someone hands a player
// cannot make their game take a room without asking. A number (hex) accepts that offer from any source: it is keyed
// with a random value of this start of the game, so no file can hold it in advance. nx2navi.ini's line counts as the
// file is now (poll_ini), before the line read at start (which may say always, and that counts for nothing), so the
// player puts the number the log shows there while the game runs. 1 for always, else 0 and the number (0 none) in
// *number.
static int accept_setting(uint32_t *number) {
    const char *v = cfg_value("NX2NAVI_COOP_ACCEPT");
    int env = cfg_from_env("NX2NAVI_COOP_ACCEPT");
    *number = 0;
    if (env && !strcmp(v, "always")) return 1;
    if (!env && G.ini_accept[0]) v = G.ini_accept;
    if (!strcmp(v, "always")) {
        if (!G.said_always)
            rt_log("coop: sync: coop_accept = always is taken from the environment only (NX2NAVI_COOP_ACCEPT); the nx2navi.ini line is ignored");
        G.said_always = 1;
        return 0;
    }
    char *end;
    unsigned long n = strtoul(v, &end, 16);
    if (*v && !*end) *number = (uint32_t)n;
    return 0;
}

// The engine's offer, with the relay's names for members not identified yet (G.lock held).
static int offer_of(CoopOffer *o, uint64_t now) {
    int has = coop_sync_offer(G.eng, o, now);
    for (int i = 0; i < o->n; i++)
        for (int k = 0; !o->member[i].name[0] && k < G.view.n; k++)
            if (G.view.member[k].id == o->member[i].id) snprintf(o->member[i].name, sizeof o->member[i].name, "%s", G.view.member[k].name);
    return has;
}

static void say_accepted(const CoopOffer *o, const char *how) {
    rt_log("coop: sync: room accepted (%s); %s", how, coop_sync_copy_wanted(G.eng) ? "the save is copied first" : "values from it are applied");
    (void)o;
}

// A key this player accepted (coop_offer_shown's question), G.lock held.
static int key_accepted(const char *key, void *eng) {
    uint64_t kid;
    return coop_sync_key_parse(key, &kid) && coop_sync_accepted_key(eng, kid);
}

// What the menu and the puppets read (G.lock held): the offer offer_update settled on, whether this game is the maker,
// the members whose puppets are shown, and the contributors at most every 500 ms.
static void publish(const CoopOffer *o, int has, uint64_t now) {
    int maker = coop_sync_is_maker(G.eng), fresh = !G.pub_contrib_at || now - G.pub_contrib_at >= 500;
    CoopContributor c[PUB_CONTRIBUTORS];
    int n = fresh ? coop_sync_contributors(G.eng, c, PUB_CONTRIBUTORS) : 0;
    __atomic_store_n(&G.pub_shown, coop_offer_shown(o, key_accepted, G.eng), __ATOMIC_RELEASE);
    plat_mutex_lock(&G.pub_lock);
    G.pub = *o, G.pub_has = has, G.pub_maker = maker;
    if (fresh) memcpy(G.pub_contrib, c, sizeof *c * (size_t)n), G.pub_ncontrib = n, G.pub_contrib_at = now;
    plat_mutex_unlock(&G.pub_lock);
}

uint32_t coop_game_shown(void) { return __atomic_load_n(&G.pub_shown, __ATOMIC_ACQUIRE); }

// No engine (co-op off, or between rooms): nothing to read.
static void publish_none(void) {
    __atomic_store_n(&G.pub_shown, 0, __ATOMIC_RELEASE);
    plat_mutex_lock(&G.pub_lock);
    if (G.pub_has || G.pub.n || G.pub_ncontrib || G.pub_maker)
        memset(&G.pub, 0, sizeof G.pub), G.pub_has = G.pub_maker = G.pub_ncontrib = 0, G.pub_contrib_at = 0;
    plat_mutex_unlock(&G.pub_lock);
}

// The offer of the room (the coop thread, G.lock held): accepted by the menu or the setting, logged when it changed and
// is not accepted, and published for the menu.
static void offer_update(uint64_t now) {
    CoopOffer o;
    uint32_t number;
    int always = accept_setting(&number);
    // the menu's accept, of the offer it showed: taken only while that offer is still the room's
    uint32_t menu = __atomic_exchange_n(&G.want_accept, 0, __ATOMIC_ACQ_REL);
    int has = offer_of(&o, now);
    const char *how = !has || o.accepted ? NULL
                      : menu == o.token  ? "in the menu"
                      : always           ? "coop_accept = always"
                      : number == o.token ? "coop_accept names its number"
                                          : NULL;
    if (how && coop_sync_accept(G.eng, o.token)) {
        say_accepted(&o, how);
        has = offer_of(&o, now);
    }
    if (o.token && !o.accepted && (o.token != G.logged || o.unidentified != G.logged_unid)) {
        int again = G.logged != 0;
        G.logged = o.token, G.logged_unid = o.unidentified;
        char who[COOP_MAX_MEMBERS * 160] = "", rules[200];
        size_t at = 0;
        for (int i = 0; i < o.n && at < sizeof who; i++) {
            const CoopOfferMember *m = &o.member[i];
            at += (size_t)snprintf(who + at, sizeof who - at, "%s%s (member %d, %s%s%s%s%s%s)", i ? ", " : "", m->name, m->id, m->heard ? "key " : "not identified",
                                   m->key, m->accepted_as[0] ? ", a key you accepted as " : "", m->accepted_as,
                                   m->maker ? ", signs the rules" : "", m->removed ? ", removed by the maker: ignored" : "");
        }
        coop_rules_text(&o.rules, rules, sizeof rules);
        rt_log("coop: sync: %s; in the room: %s; rules: %s, signed by key %s",
               o.new_save ? "another save is played in the room: it gets the room's progress, which holds what the last save brought"
                          : again ? "the room changed (a member, the rules or the key that signs them)" : "a room to accept",
               o.n ? who : "nobody else", rules, o.maker_key);
        if (o.maker_moved) rt_log("coop: sync: the relay names another member as the maker than the one whose key signs the rules; the signed rules stay");
        if (o.disagree)
            rt_log("coop: sync: members carry rules signed by different keys: before accepting, compare the key that signs these rules with the "
                   "maker's, by voice or chat");
        if (o.unidentified)
            rt_log("coop: sync: %d member(s) in the room are not identified (never said who they are, or two keys present themselves for "
                   "one): the room cannot be accepted while they are in it (the maker can remove them, or everyone moves to a new room)",
                   o.unidentified);
        else
            rt_log("coop: sync: nothing from the room is applied to this game until you accept it: put coop_accept = %08x into nx2navi.ini "
                   "[coop] while the game runs (the number is new at each start of the game and changes with the room); the save is copied "
                   "to coop-backup in the profile folder first. Your key pools count only the players you accept, among those the maker lets "
                   "use the pools at all; your story flags take what the players you accept took from the players they accepted", o.token);
    }
    if (!o.token && o.disagree && !G.said_disagree)
        rt_log("coop: sync: the members carry rules signed by different keys, so this game takes no rules and applies nothing; ask the "
               "room's maker which key is theirs");
    G.said_disagree = !o.token && o.disagree;
    __atomic_store_n(&G.open, coop_sync_open(G.eng), __ATOMIC_RELEASE);
    publish(&o, has, now);
}

int coop_game_offer(CoopOffer *out) {
    plat_mutex_lock(&G.pub_lock);
    *out = G.pub;
    int has = G.pub_has;
    plat_mutex_unlock(&G.pub_lock);
    return has;
}

int coop_game_accept(uint32_t token) {
    plat_mutex_lock(&G.pub_lock);
    int same = token && G.pub_has && token == G.pub.token, done = same && G.pub.accepted;
    int ok = same && !G.pub.accepted && !G.pub.unidentified;
    plat_mutex_unlock(&G.pub_lock);
    if (ok) __atomic_store_n(&G.want_accept, token, __ATOMIC_RELEASE);
    return ok || done;
}

// The maker's pool reset (G.lock held), with the order that gives a drain back: a player still in the room when the
// pools are reset is admitted to the new ones with what it took already taken, so it is removed first.
static int reset_pools(void) {
    int ok = G.eng && coop_sync_reset_pools(G.eng);
    G.pub_contrib_at = 0;
    if (ok)
        rt_log("coop: sync: the pools are reset: every game joins them again with what they held for the players in the room; a player "
               "who drained a pool and is still in the room keeps what it took, so remove it first (coop_remove), then reset");
    else rt_log("coop: sync: coop_reset_pools: only the room's maker resets the pools");
    return ok;
}

// The same on the coop thread without G.lock: nx2navi.ini's value, and a reset the menu queued.
static int lock_and_reset_pools(void) {
    plat_mutex_lock(&G.lock);
    int ok = reset_pools();
    plat_mutex_unlock(&G.lock);
    return ok;
}

int coop_game_reset_pools(void) {
    plat_mutex_lock(&G.pub_lock);
    int maker = G.pub_maker;
    plat_mutex_unlock(&G.pub_lock);
    if (maker) __atomic_store_n(&G.want_reset, plat_now_ms(), __ATOMIC_RELEASE);
    else rt_log("coop: sync: coop_reset_pools: only the room's maker resets the pools");
    return maker;
}

// The maker removes a key (G.lock held): a member that left, or one in the room.
static int remove_key(const char *fingerprint) {
    uint64_t kid;
    int ok = G.eng && coop_sync_key_parse(fingerprint, &kid) && coop_sync_remove_key(G.eng, kid);
    if (ok) rt_log("coop: sync: key %s removed: every game drops it and its pool records, and ignores it from now on", fingerprint);
    else rt_log("coop: sync: key %s was not removed: only the room's maker removes, and only a key the room knows", fingerprint);
    G.pub_contrib_at = 0;
    return ok;
}

int coop_game_remove_key(const char *fingerprint) {
    uint64_t kid;
    if (!fingerprint || !coop_sync_key_parse(fingerprint, &kid)) return 0;
    plat_mutex_lock(&G.pub_lock);
    int maker = G.pub_maker;
    if (maker) snprintf(G.want_remove, sizeof G.want_remove, "%s", fingerprint), G.want_remove_at = plat_now_ms();
    plat_mutex_unlock(&G.pub_lock);
    if (!maker) rt_log("coop: sync: key %s was not removed: only the room's maker removes", fingerprint);
    return maker;
}

int coop_game_contributors(CoopContributor *out, int max) {
    plat_mutex_lock(&G.pub_lock);
    int n = G.pub_ncontrib < max ? G.pub_ncontrib : max;
    if (n > 0) memcpy(out, G.pub_contrib, sizeof *out * (size_t)n);
    plat_mutex_unlock(&G.pub_lock);
    return n > 0 ? n : 0;
}

int coop_game_is_maker(void) {
    plat_mutex_lock(&G.pub_lock);
    int maker = G.pub_maker;
    plat_mutex_unlock(&G.pub_lock);
    return maker;
}

// The maker's removal of a key by its fingerprint (the coop thread, without G.lock: a removal of a member in the room
// goes through coop_remove_member's hook), from nx2navi.ini's coop_remove or the menu. A member in the room that holds
// the key is removed at the relay too.
static void remove_by_fingerprint(const char *fp) {
    uint64_t kid = 0, k;
    int member = 0;
    CoopOffer o;
    plat_mutex_lock(&G.lock);
    if (G.eng && coop_sync_key_parse(fp, &kid)) coop_sync_offer(G.eng, &o, plat_now_ms());
    else o.n = 0;
    for (int i = 0; i < o.n; i++)
        if (o.member[i].heard && coop_sync_key_parse(o.member[i].key, &k) && k == kid) member = o.member[i].id;
    plat_mutex_unlock(&G.lock);
    if (member && coop_remove_member(member)) return;   // in the room: the relay removes it too
    plat_mutex_lock(&G.lock);
    remove_key(fp);
    plat_mutex_unlock(&G.lock);
}

// The maker's coop_remove (a key's fingerprint, which the log shows beside each member) and coop_reset_pools, and
// coop_accept, from nx2navi.ini as it is now, once a second while in a room (on the coop thread, without G.lock). A
// reading that fails (an editor saving the file, a lock) is skipped, not read as empty. A value acts once: when it comes
// back after a missed reading, or stays in the file, it acts on nothing again (coop_reset_pools = 0 lets the next value
// act). A member number is not taken from the file: numbers pass to other players as members come and go.

static void poll_ini(uint64_t now) {
    if (G.view.state != COOP_JOINED && G.view.state != COOP_RECONNECTING) return;   // no room: nothing to act on, nothing read
    if (G.ini_at && now - G.ini_at < 1000) return;
    G.ini_at = now;
    char acc[32], rm[32], rp[32];
    if (cfg_ini_now("coop_accept", acc, sizeof acc) < 0 || cfg_ini_now("coop_remove", rm, sizeof rm) < 0 ||
        cfg_ini_now("coop_reset_pools", rp, sizeof rp) < 0)
        return;
    snprintf(G.ini_accept, sizeof G.ini_accept, "%s", acc);
    int primed = G.ini_primed;
    G.ini_primed = 1;
    if (rm[0] && strcmp(rm, G.ini_remove)) {
        snprintf(G.ini_remove, sizeof G.ini_remove, "%s", rm);
        uint64_t kid;
        if (!primed) {
        } else if (!coop_sync_key_parse(rm, &kid) && strspn(rm, "0123456789") == strlen(rm)) {
            // a fingerprint is 16 digits (dashes aside), so one that is all decimal digits is still a fingerprint
            rt_log("coop: sync: coop_remove = %s in nx2navi.ini names a member number, which another player may have by now: put the "
                   "member's key fingerprint there (the log shows it beside each member)", rm);
        } else {
            remove_by_fingerprint(rm);
        }
    }
    if (!strcmp(rp, "0")) snprintf(G.ini_reset, sizeof G.ini_reset, "0");
    else if (rp[0] && strcmp(rp, G.ini_reset)) {
        snprintf(G.ini_reset, sizeof G.ini_reset, "%s", rp);
        if (primed) lock_and_reset_pools();
    }
}

// coop_remove_member's hook (coop.h): the maker's removal, signed, so that every game drops that identity.
static int on_remove(int member, void *user) {
    (void)user;
    plat_mutex_lock(&G.lock);
    int ok = G.eng && coop_sync_remove(G.eng, member);
    plat_mutex_unlock(&G.lock);
    if (ok) rt_log("coop: sync: member %d removed: every game drops its identity and its pool records", member);
    return ok;
}

// For the maker: the pool contributors that took more from the pools than they brought (per pool:
// coop_sync_contributors), in the room or not, when that changed (at most every 10 s), with the setting that removes
// each (G.lock held). A player who used keys others found is among them too; a drainer is the one nobody expected.
static void say_spenders(uint64_t now) {
    if (now - G.spent_at < 10000 || !coop_sync_is_maker(G.eng)) return;
    CoopContributor c[24];
    int n = coop_sync_contributors(G.eng, c, 24);
    uint64_t h = 0;
    for (int i = 0; i < n; i++)
        if (!c[i].self && !c[i].removed && (c[i].spent[0] > 0 || c[i].spent[1] > 0 || c[i].spent[2] > 0))
            for (int k = 0; k < 3; k++) h = (h ^ (uint64_t)c[i].spent[k] ^ (uint64_t)c[i].key[k * 5] ^ (uint64_t)c[i].member << 40) * 0x100000001b3ull;
    if (h == G.spent_seen) return;
    G.spent_seen = h, G.spent_at = now;
    for (int i = 0; h && i < n; i++) {
        if (c[i].self || c[i].removed || !(c[i].spent[0] > 0 || c[i].spent[1] > 0 || c[i].spent[2] > 0)) continue;
        char who[COOP_NAME_MAX + 40];
        if (c[i].member) snprintf(who, sizeof who, "member %d, %s", c[i].member, c[i].name);
        else snprintf(who, sizeof who, "not in the room");
        rt_log("coop: sync: key %s (%s) took %lld keys, %lld rupees and %lld ammunition from the pools beyond what it brought; "
               "coop_remove = %s removes it and gives back what it took (remove it before a pool reset: a reset while it is in the "
               "room keeps what it took)", c[i].key, who, (long long)c[i].spent[0], (long long)c[i].spent[1], (long long)c[i].spent[2], c[i].key);
    }
}

// The copy of the save an accepted room waits for, on the coop thread without G.lock (it takes a while). The first
// copy for a room in this start of the game is kept out of the rotation ("first-<time>"), so the save from before the
// room outlasts the copies of later acceptances.
static void make_snapshot(void) {
    plat_mutex_lock(&G.lock);
    int want = G.eng && coop_sync_copy_wanted(G.eng);
    char room[sizeof G.view.code];
    snprintf(room, sizeof room, "%s", G.view.code);
    int first = 1;
    for (unsigned i = 0; i < G.nfirst && i < 8; i++) first &= strcmp(G.first_rooms[i], room) != 0;
    plat_mutex_unlock(&G.lock);
    if (!want) return;
    // No committed save on the disk yet (a new game that has not saved, or save storage not set up this early): there
    // is nothing to copy, and the room stays closed until there is. Counting that as a copy made would let the room
    // change a save that was never copied. Looked at again every few seconds, said once.
    static uint64_t unsaved_next;
    static int unsaved_said;
    uint64_t now = plat_now_ms();
    if (now < unsaved_next) return;
    char path[1200];
    int ok = fs_save_snapshot("coop-backup", first ? "first-" : NULL, 8, path, sizeof path);
    if (ok && !path[0]) {
        if (!unsaved_said)
            rt_log("coop: sync: the game has not saved yet, so there is no save to copy: nothing from the room is "
                   "applied until it has saved once");
        unsaved_said = 1, unsaved_next = now + 3000;
        return;
    }
    unsaved_said = 0, unsaved_next = 0;
    if (ok)
        rt_log("coop: sync: the save as it was before this room%s is in %s (docs/coop.md: going back)%s", first ? "" : " was accepted again", path,
               first ? "; it is kept until you delete it" : "");
    else rt_log("coop: sync: the save could not be copied; nothing from the room is applied");
    plat_mutex_lock(&G.lock);
    coop_sync_copied(G.eng, ok);
    if (ok && first) snprintf(G.first_rooms[G.nfirst++ % 8], sizeof G.first_rooms[0], "%s", room);
    __atomic_store_n(&G.open, coop_sync_open(G.eng), __ATOMIC_RELEASE);
    plat_mutex_unlock(&G.lock);
}

static void on_tick(uint64_t now, void *user) {
    (void)user;
    unsigned v = cfg_version();
    make_snapshot();
    poll_ini(now);
    // the maker's remedies the menu asked for since the last step
    char asked[sizeof G.want_remove];
    plat_mutex_lock(&G.pub_lock);
    snprintf(asked, sizeof asked, "%s", now - G.want_remove_at < 2000 ? G.want_remove : "");
    G.want_remove[0] = 0;
    plat_mutex_unlock(&G.pub_lock);
    if (asked[0]) remove_by_fingerprint(asked);
    uint64_t reset_at = __atomic_exchange_n(&G.want_reset, 0, __ATOMIC_ACQ_REL);
    if (reset_at && now - reset_at < 2000) lock_and_reset_pools();
    plat_mutex_lock(&G.lock);
    if (!G.eng) {
        plat_mutex_unlock(&G.lock);
        publish_none();
        return;
    }
    if (v != G.cfg_seen_net) {   // a setting changed: the room's maker passes the rules on, and acts on its remedies
        G.cfg_seen_net = v;
        CoopRules mine;
        read_rules(&mine);
        coop_sync_settings(G.eng, &mine, (G.view.state == COOP_JOINED || G.view.state == COOP_RECONNECTING) && G.view.made);
        // coop_remove = a key's fingerprint (a member id is coop.c's), coop_reset_pools = any new value, set from the
        // environment or by the menu's test hook (the Co-op tab's rows ask coop_offer.h instead). The values at start
        // are only noted: one left from an earlier session acts on nothing
        // read with cfg_get: neither has a row in the settings table, and cfg_value stops the game for such a key
        const char *rm = cfg_get("NX2NAVI_COOP_REMOVE"), *rp = cfg_get("NX2NAVI_COOP_RESET_POOLS");
        rm = rm ? rm : "", rp = rp ? rp : "";
        uint64_t kid;
        int primed = G.cfg_primed;
        G.cfg_primed = 1;
        if (primed && strcmp(rm, G.remove_seen) && coop_sync_key_parse(rm, &kid)) remove_key(rm);
        snprintf(G.remove_seen, sizeof G.remove_seen, "%s", rm);
        if (primed && strcmp(rp, G.reset_seen) && *rp && strcmp(rp, "0")) reset_pools();
        snprintf(G.reset_seen, sizeof G.reset_seen, "%s", rp);
    }
    coop_sync_tick(G.eng, now, send_fn, NULL);
    CoopRules r;
    unsigned ver = coop_sync_rules(G.eng, &r);
    if (ver != G.rules_seen) {
        G.rules_seen = ver;
        if (r.epoch) {
            char text[200];
            coop_rules_text(&r, text, sizeof text);
            rt_log("coop: sync: the room's rules: %s (from member %d)", text, r.maker);
        }
    }
    offer_update(now);
    if (now - G.checked_at >= 1000) {   // what the engine refused, once a second
        G.checked_at = now;
        CoopSyncStats st;
        coop_sync_stats(G.eng, &st);
        unsigned ign = coop_sync_ignored(G.eng);
        if (st.rejected != G.rejected_seen)
            rt_log("coop: sync: %u values from the room were refused (flags this game does not share or values it does not "
                   "take); %u in this session", st.rejected - G.rejected_seen, st.rejected);
        for (int id = 1; id <= COOP_MAX_MEMBERS; id++)
            if (ign >> id & 1 && !(G.ignored_seen >> id & 1))
                rt_log("coop: sync: member %d sent values this game does not take; nothing more is taken from it while it stays in the room", id);
        G.rejected_seen = st.rejected, G.ignored_seen = ign;
        if (st.own_key != G.own_key_seen && (!G.own_key_said || now - G.own_key_said >= 60000)) {
            G.own_key_said = now, G.own_key_seen = st.own_key;
            rt_log("coop: sync: member %d presents this game's own key for the room: another game holds a copy of this profile's coop-keys "
                   "folder (or of the whole profile) and can act as the room's maker; it stays not identified, so nobody can accept the "
                   "room while it is in it", st.own_key_member);
        }
        if (st.key_changes != G.key_changes_seen) {
            G.key_changes_seen = st.key_changes;
            rt_log("coop: sync: member %d presents another key than the one it was identified by in its session (%u times in this room): "
                   "it counts as nobody while both are presented, and the other takes its place 10 s after the first stops; compare the "
                   "fingerprints the room's offer shows", st.key_change_member, st.key_changes);
        }
        if (st.key_moves != G.key_moves_seen) {
            G.key_moves_seen = st.key_moves;
            CoopOffer o;
            const CoopOfferMember *m = NULL;
            coop_sync_offer(G.eng, &o, now);
            for (int i = 0; i < o.n; i++)
                if (o.member[i].id == st.key_move_member) m = &o.member[i];
            char was[48] = "";
            if (st.key_move_from && st.key_move_from != st.key_move_member) snprintf(was, sizeof was, " as member %d", st.key_move_from);
            rt_log("coop: sync: member %d (%s, key %s) presents a key you accepted%s%s%s%s", st.key_move_member, m ? m->name : "?", m ? m->key : "?",
                   was, m && m->accepted_as[0] ? " under the name " : "", m ? m->accepted_as : "",
                   m && m->accepted_as[0] ? ": under this name it is a new offer" : "");
        }
        say_spenders(now);
    }
    plat_mutex_unlock(&G.lock);
}

void coop_game_init(void) {
    plat_mutex_init(&G.lock);
    plat_mutex_init(&G.pub_lock);
    G.cfg_seen = ~0u;
    coop_on_message(COOP_K_DIGEST, on_message, (void *)(uintptr_t)COOP_K_DIGEST);
    coop_on_message(COOP_K_SECTION, on_message, (void *)(uintptr_t)COOP_K_SECTION);
    coop_on_view(on_view, NULL);
    coop_on_tick(on_tick, NULL);
    coop_on_remove(on_remove, NULL);
}

// ---- the game thread

static void unbind(const char *why) {
    if (!G.bound) return;
    G.bound = 0;
    rt_log("coop: sync: %s; nothing is read or written until a save is played", why);
}

static void bind(uint64_t mgr) {
    uint64_t be = LD64(mgr + C_BOOL + CN_ENTRIES), ie = LD64(mgr + C_INT + CN_ENTRIES);
    if (!plat_mutex_trylock(&G.lock)) return;   // the coop thread has the engine: the next frame
    if (mgr != G.mgr || be != G.bool_entries || ie != G.int_entries || !G.nflags) {
        if (!build_table(mgr)) {   // under the lock: the engine's limits read the table on the coop thread
            plat_mutex_unlock(&G.lock);
            return;
        }
        G.mgr = mgr, G.bool_entries = be, G.int_entries = ie;
    }
    uint64_t now = plat_now_ms();
    coop_sync_schema(G.eng, G.schema);
    coop_sync_rebind(G.eng);
    for (int i = 0; i < G.nflags; i++) {
        GFlag *f = &G.flags[i];
        f->last = raw_of(f);
        if (!coop_cat_is_pool(f->cat) && reg_of(f, f->last)) coop_sync_local(G.eng, f->cat, f->hash, reg_of(f, f->last), now);
    }
    plat_mutex_unlock(&G.lock);
    G.bound = 1, G.frames = 0, G.slice = 0;
    rt_log("coop: sync: playing a save: %u progress flags, %u items, %u songs, %u quest items, %u key counters, rupees, %u "
           "ammunition counters; the room's rules decide which are shared",
           G.count[COOP_CAT_PROGRESS], G.count[COOP_CAT_ITEMS], G.count[COOP_CAT_SONGS], G.count[COOP_CAT_QUEST],
           G.count[COOP_CAT_KEYS], G.count[COOP_CAT_AMMO]);
}

// Link stands in the world: the Player actor, [[main+0x4e371d0] + 0x480], exists (its name, [+0x20], reads "Player")
// away from the origin (its position in metres at +0x31c). There is none on the title screen at the start, so this holds
// sync back while the first load builds the world; after a return to the title screen the actor stays (title_starts
// marks that).
static int player_in_world(void) {
    uint64_t m = rd_ok(PLAYER_PTR, 8) ? LD64(PLAYER_PTR) : 0, a = rd_ok(m + 0x480, 8) ? LD64(m + 0x480) : 0;
    uint64_t name = rd_ok(a + 0x20, 8) ? LD64(a + 0x20) : 0;
    if (!rd_ok(name, 7) || memcmp((const void *)(uintptr_t)name, "Player", 7) || !rd_ok(a + 0x31c, 12)) return 0;
    float p[3];
    memcpy(p, (const void *)(uintptr_t)(a + 0x31c), sizeof p);
    return isfinite(p[0]) && isfinite(p[2]) && fabsf(p[0]) + fabsf(p[2]) >= 1.0f;
}

// The title screen's event flow is opened each time the title screen starts: at boot and after the pause menu's Title
// Screen. From then until a save is loaded the flags are the title's, and the play clock runs on from the save's value.
static volatile int32_t title_starts;
void coop_game_rom_open(const char *path) {
    if (__atomic_load_n(&G.on, __ATOMIC_RELAXED) && strstr(path, "/Event/EventFlow/TitleEvent.bfevfl"))
        plat_inc32(&title_starts);
}

// The game is being played: a save was loaded since the title screen, Link stands in the world, the clock runs, no save
// is loading, no scene is changing. Notes a save change (unbind).
static int gate(uint64_t mgr) {
    if (mgr != G.mgr || !G.clock_entry) {   // the addresses of the few flags read every frame
        G.clock_entry = entry_of(mgr, C_UINT, INT_SIZE, F_PLAY_CLOCK);
        G.loading_entry = entry_of(mgr, C_BOOL, BOOL_SIZE, F_LOADING);
        for (int i = 0; i < 4; i++) G.transition_entry[i] = entry_of(mgr, C_BOOL, BOOL_SIZE, transition_flags[i]);
        if (!G.clock_entry || !rd_ok(G.clock_entry, INT_SIZE)) return 0;
    }
    uint64_t now = plat_now_ms();
    int titles = plat_cas32(&title_starts, 0, 0);
    if (titles != G.titles) {
        G.titles = titles;
        unbind("the title screen came back");
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
        if (!tick) unbind("another save was loaded"), G.loaded = 1;
        G.clock = clock;
        G.clock_at = tick ? now : 0;
    }
    if (loading) unbind("a save is loading"), G.loaded = 1;
    int player = player_in_world();
    if (player != G.player && TRACE_ON) rt_log("coop: sync: Link %s the world", player ? "stands in" : "is not in");
    G.player = player;
    int open = G.loaded && player && !loading && !moving && G.clock_at && now - G.clock_at < CLOCK_MS;
    if (open != G.gate && TRACE_ON)
        rt_log("coop: sync: game %s (play clock %u, loading %d, scene change %#x)", open ? "running" : "paused", clock, loading, moving);
    G.gate = open;
    return open;
}

// Calls a set routine of the game with the guest's registers kept as they were at the present (as res_present does).
static void set_flag(Cpu *c, uint64_t mgr, int is_int, uint32_t hash, int32_t value) {
    rt_call(c, is_int ? SET_INT_FN : SET_BOOL_FN, 3, mgr, (uint64_t)(uint32_t)value, (uint64_t)hash);
}

static void run_test_aid(Cpu *c, uint64_t mgr) {
    for (int i = 0; i < G.ntest; i++) {
        if (G.test[i].done || G.frames < G.test[i].frame) continue;
        G.test[i].done = 1;
        uint32_t h = G.test[i].hash;
        int is_int = entry_of(mgr, C_BOOL, BOOL_SIZE, h) ? 0 : entry_of(mgr, C_INT, INT_SIZE, h) ? 1 : -1;
        if (is_int < 0) {
            rt_log("coop: test: no Bool or Int flag %08x", h);
            continue;
        }
        set_flag(c, mgr, is_int, h, G.test[i].value);
        rt_log("coop: test: set %s %08x = %d at frame %u", is_int ? "Int" : "Bool", h, G.test[i].value, G.frames);
    }
}

static void apply(Cpu *c, uint64_t mgr) {
    CoopApply a[APPLY_PER_FRAME];
    if (!__atomic_load_n(&G.open, __ATOMIC_ACQUIRE)) return;   // not accepted, or no copy of the save yet: values wait
    if (!plat_mutex_trylock(&G.lock)) return;   // the coop thread has the engine: nothing is applied this frame
    int n = coop_sync_take(G.eng, a, APPLY_PER_FRAME);
    plat_mutex_unlock(&G.lock);
    for (int i = 0; i < n; i++) {
        GFlag *f = flag_by_hash(a[i].hash);
        if (!f || f->cat != a[i].cat) continue;   // classified differently here: not taken
        int32_t v = a[i].value;
        if (a[i].pool) {
            if (int_pending(mgr, f->hash)) {
                plat_mutex_lock(&G.lock);
                coop_sync_untake(G.eng, &a[i]);
                plat_mutex_unlock(&G.lock);
                continue;
            }
        } else if (!f->is_int) {
            if (v != 1) continue;
            v = !f->inv;
        } else if (v > (int32_t)f->max) {   // the engine took none above it: a check of last resort
            continue;
        }
        set_flag(c, mgr, f->is_int, f->hash, v);
        if (a[i].pool) {
            plat_mutex_lock(&G.lock);
            coop_sync_pool_wrote(G.eng, f->hash, v);
            plat_mutex_unlock(&G.lock);
        }
        G.applied[f->cat]++, G.applied_total++;
        if (TRACE_ON) rt_log("coop: sync: applied %s %08x = %d", coop_cat_name(f->cat), f->hash, v);
    }
    uint64_t now = plat_now_ms();
    if (G.applied_total && now - G.applied_log_at >= 5000) {
        char text[200];
        size_t at = 0;
        for (int k = 0; k < COOP_CAT_N; k++)
            if (G.applied[k] && at < sizeof text)
                at += (size_t)snprintf(text + at, sizeof text - at, "%s%s %u", at ? ", " : "", coop_cat_name(k), G.applied[k]);
        rt_log("coop: sync: took %u values from the room (%s)", G.applied_total, text);
        memset(G.applied, 0, sizeof G.applied);
        G.applied_total = 0;
        G.applied_log_at = now;
    }
}

static void scan(uint64_t now) {
    int nflat = G.nflags - G.npools;
    int from = nflat * G.slice / SLICES, to = nflat * (G.slice + 1) / SLICES;
    if (!plat_mutex_trylock(&G.lock)) return;   // the coop thread has the engine: this frame reads nothing
    G.slice = (G.slice + 1) % SLICES;
    for (int i = from; i < to; i++) {
        GFlag *f = &G.flags[i];
        int32_t raw = raw_of(f);
        if (raw == f->last) continue;
        f->last = raw;
        coop_sync_local(G.eng, f->cat, f->hash, reg_of(f, raw), now);
    }
    for (int i = nflat; i < G.nflags; i++) {
        GFlag *f = &G.flags[i];
        f->last = raw_of(f);
        int32_t cap = f->cap_addr ? (int32_t)LD32(f->cap_addr + I_CURRENT) : -1;
        coop_sync_pool_local(G.eng, f->cat, f->hash, f->last, cap > 0 ? cap : -1, now);
    }
    plat_mutex_unlock(&G.lock);
}

void coop_present(Cpu *c) {
    unsigned v = cfg_version();
    if (v != G.cfg_seen) {
        G.cfg_seen = v;
        G.on = cfg_bool("NX2NAVI_COOP");
        if (G.on) read_test_aid();
    }
    if (!G.on) return;
    if (!RT_V111) {
        if (!G.said_version) rt_log("coop: sync: progress sync knows game 1.1.1 only; this is %d", rt_game_version);
        G.said_version = 1;
        return;
    }
    if (!G.eng && !ensure_engine()) return;
    uint64_t mgr = LD64(MGR_PTR);
    if (!rd_ok(mgr, MGR_SIZE)) {
        unbind("the game's flags are gone");
        return;
    }
    if (G.bound && (mgr != G.mgr || LD64(mgr + C_BOOL + CN_ENTRIES) != G.bool_entries || LD64(mgr + C_INT + CN_ENTRIES) != G.int_entries))
        unbind("the game made its flags anew");
    if (!gate(mgr)) return;
    if (!G.bound) bind(mgr);
    if (!G.bound) return;
    Cpu keep = *c;
    G.frames++;
    run_test_aid(c, mgr);
    scan(plat_now_ms());
    apply(c, mgr);
    memcpy(c->x, keep.x, sizeof c->x);
    memcpy(c->v, keep.v, sizeof c->v);
    c->nzcv = keep.nzcv, c->fpsr = keep.fpsr;
}
