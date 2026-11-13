// The debug menu's hands in the game (ui/debug_menu.cpp is its window, F9 with debug_menu = 1). Ocarina of Time 1.1.1.
//
// Reads go straight to the game's memory from the menu's thread, through plat_mem_read, which answers 0 instead of
// faulting. Changes are queued and made on the game's own thread at its pad read (dbg_tick, from ov_input.c), between
// two of the game's frames: a flag through the game's own function, so everything that follows a flag in the game
// follows this change too; a memory value by a plain write into memory the game may read and write.
//
// The game's on/off flags ("game data"): the manager is **[main+0x4c7e180]; +0x10 the number of flags, +0x18 their
// entries (0x18 bytes each, the value's byte at +0x16), +0x20 a table of {hash, index} pairs probed from hash % size
// (+0x2c the size). main+0x20d9c0 (manager, u8 *out, hash) reads a flag and main+0x682b20 (manager, value, hash)
// queues a change, which the manager applies at its next update. A flag is known to the game by the MurmurHash3 of
// its name only; the names shown are those found in the player's own game files (the event flows), hashed here and
// matched (dbg_names), so nothing of them is kept in this code.
//
// The game's speed: the runtime gives the game its step every frame (frame_rate.c ocarina_step_write, the display
// interval); the menu's factor multiplies that step, so 0.5 is half speed for the whole game and its interface.
//
// Link's position is shown but not written: it is the actor's copy ([[main+0x4e371d0]+0x480]+0x31c), which the physics
// writes every frame. Writing it, or every copy of it in the heap, moved Link back within three frames.
//
// Teleporting is the game's own change of area, as a loading zone, a warp song or a cutscene makes it: the game's
// event script reserves the next area and the name of a start position in it (game data texts), and asks the
// sequence that runs during events to change area (the enum Sequence_ControlInEvent = ChangeBancOnEvent). warp_tick
// does the same during a short event of the game that only waits (DefaultParentEvent, 30 frames), so the game unloads
// the area, loads the next one and places Link exactly as it does for its own doors. The places offered are the ones
// in the player's game files (Banc/BancBasedResource/StartPos.byml.zs: each area with its named start positions,
// the warp songs' pads and both sides of every loading zone among them); none of that list is kept in this code.
//
// An event (a cutscene, a conversation, a scripted moment) is started by its file's name through the game's event
// manager, as the game's own actors start them.
//
// The free camera stands in for the game's camera inside the game's own camera update (dbg_freecam_host), so the game
// itself draws from it.
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "bugcap.h"
#include "cfg.h"
#include "cpu.h"
#include "debug_tools.h"
#include "fs.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

int dbg_supported(void) { return RT_V111; }

static int rd(uint64_t a, void *out, size_t n) { return a && plat_mem_read(a, out, n); }
static uint64_t rd_ptr(uint64_t a) {
    uint64_t p = 0;
    return rd(a, &p, 8) ? p : 0;
}
// A write into memory the game may read and write: 0 when the place is not such memory.
static int wr(uint64_t a, const void *from, size_t n) {
    RtMemoryInfo mi;
    if (!a || mem_query(a, &mi) || !mi.state || (mi.perm & 3) != 3 || a + n > mi.base + mi.size) return 0;
    mem_thaw(a, n);
    memcpy((void *)(uintptr_t)a, from, n);
    return 1;
}

// ---- the player and the game's speed
static uint64_t player(void) {
    uint64_t manager = rd_ptr(MOD_MAIN_BASE + 0x4e371d0), actor = manager ? rd_ptr(manager + 0x480) : 0;
    char name[7];
    uint64_t np = actor ? rd_ptr(actor + 0x20) : 0;
    return np && rd(np, name, 7) && !memcmp(name, "Player", 7) ? actor : 0;
}

int dbg_player_pos(float xyz[3]) {
    uint64_t a = dbg_supported() ? player() : 0;
    return a && rd(a + 0x31c, xyz, 12) && isfinite(xyz[0]) && isfinite(xyz[1]) && isfinite(xyz[2]);
}

// The actor's rotation beside its position: nine floats, a 3x3 matrix by rows (bugcap.c writes them as they are).
int dbg_player_rot(float m[9]) {
    uint64_t a = dbg_supported() ? player() : 0;
    if (!a || !rd(a + 0x328, m, 36)) return 0;
    for (int i = 0; i < 9; i++)
        if (!isfinite(m[i])) return 0;
    return 1;
}

static float speed_factor = 1.0f;
float dbg_speed_factor(void) { return speed_factor; }
void dbg_speed_set(float factor) { speed_factor = factor >= 0.05f && factor <= 8.0f ? factor : 1.0f; }

// ---- what the menu asks for, done by dbg_tick
enum { Q_FLAG, Q_WRITE, Q_SKULL, Q_TOKENS, Q_MAX = 256, FREEZE_MAX = 64 };
typedef struct { int kind; uint32_t hash; uint64_t addr, value; int size; } Req;
static PlatMutex q_lock = PLAT_MUTEX_INIT;
static Req q[Q_MAX];
static int q_n;
static struct { uint64_t addr, value; int size; } frozen[FREEZE_MAX];
static int frozen_n;
static uint64_t stat_flags, stat_writes, stat_refused;

static void ask(Req r) {
    plat_mutex_lock(&q_lock);
    if (q_n < Q_MAX) q[q_n++] = r;
    plat_mutex_unlock(&q_lock);
}

void dbg_flag_set(uint32_t hash, int on) { ask((Req){Q_FLAG, hash, 0, (uint64_t)(on != 0), 0}); }
void dbg_mem_write(uint64_t addr, uint64_t value, int size) {
    if (addr) ask((Req){Q_WRITE, 0, addr, value, size});
}

int dbg_freeze(uint64_t addr, uint64_t value, int size, int on) {
    if (!addr) return 0;
    int ok = 1;
    plat_mutex_lock(&q_lock);
    int at = -1;
    for (int i = 0; i < frozen_n; i++)
        if (frozen[i].addr == addr) at = i;
    if (on && at < 0 && frozen_n < FREEZE_MAX) at = frozen_n++;
    if (on && at >= 0) frozen[at].addr = addr, frozen[at].value = value, frozen[at].size = size;
    else if (on) ok = 0;
    else if (at >= 0) frozen[at] = frozen[--frozen_n];
    plat_mutex_unlock(&q_lock);
    return ok;
}

int dbg_frozen(uint64_t addr) {
    int is = 0;
    plat_mutex_lock(&q_lock);
    for (int i = 0; i < frozen_n; i++) is |= frozen[i].addr == addr;
    plat_mutex_unlock(&q_lock);
    return is;
}

int dbg_frozen_count(void) { return frozen_n; }

void dbg_unfreeze_all(void) {
    plat_mutex_lock(&q_lock);
    frozen_n = 0;
    plat_mutex_unlock(&q_lock);
}

// ---- the flags
static uint64_t gmd(void) {
    uint64_t slot = rd_ptr(MOD_MAIN_BASE + 0x4c7e180);
    return slot ? rd_ptr(slot) : 0;
}

typedef struct { uint32_t hash, index; } Pair;
static Pair *flag_pairs;       // the flags by hash, sorted; made once the game's table is there
static const char **flag_name; // parallel: the name found for the hash, or NULL
static uint8_t *flag_value;    // parallel: the value at the last dbg_flags_refresh
static int flag_n;
static volatile int32_t names_state;   // 0 not asked, 1 being read, 2 read
static int names_found;

static int by_hash(const void *a, const void *b) {
    uint32_t x = ((const Pair *)a)->hash, y = ((const Pair *)b)->hash;
    return x < y ? -1 : x > y;
}

// The list of the game's flags from its hash table. 0 until the manager exists (a save is loaded).
static int flags_list(void) {
    if (flag_pairs) return 1;
    uint64_t m = gmd(), table = m ? rd_ptr(m + 0x20) : 0;
    uint32_t count = 0, size = 0;
    if (!table || !rd(m + 0x10, &count, 4) || !rd(m + 0x2c, &size, 4) || !count || count > 200000 || size < count ||
        size > 1000000)
        return 0;
    Pair *slots = malloc(sizeof(Pair) * size), *list = malloc(sizeof(Pair) * count);
    int n = 0;
    if (slots && list && rd(table, slots, sizeof(Pair) * size))
        for (uint32_t i = 0; i < size && n < (int)count; i++)
            if (slots[i].hash && slots[i].index < count) list[n++] = slots[i];
    free(slots);
    if (!list || !n) {
        free(list);
        return 0;
    }
    qsort(list, (size_t)n, sizeof(Pair), by_hash);
    flag_name = calloc((size_t)n, sizeof *flag_name);
    flag_value = calloc((size_t)n, 1);
    if (!flag_name || !flag_value) {
        free(list);
        return 0;
    }
    flag_n = n;
    flag_pairs = list;
    rt_log("debug: %d of the game's %u on/off flags listed", n, count);
    return 1;
}

// Under flags_lock: the menu's thread, the game's thread (test_script, warp_tick) and a bug capture's writer
// (bugcap.c) all ask, and the list is made by whichever asks first.
static PlatMutex flags_lock = PLAT_MUTEX_INIT;
int dbg_flags_refresh(void) {
    if (!dbg_supported()) return 0;
    plat_mutex_lock(&flags_lock);
    if (!flags_list()) {
        plat_mutex_unlock(&flags_lock);
        return 0;
    }
    uint64_t m = gmd(), entries = m ? rd_ptr(m + 0x18) : 0;
    uint32_t count = 0;
    uint8_t *raw = entries && rd(m + 0x10, &count, 4) && count && count <= 200000 ? malloc((size_t)count * 0x18) : NULL;
    if (raw && rd(entries, raw, (size_t)count * 0x18))
        for (int i = 0; i < flag_n; i++)
            if (flag_pairs[i].index < count) flag_value[i] = raw[(size_t)flag_pairs[i].index * 0x18 + 0x16] & 1;
    free(raw);
    plat_mutex_unlock(&flags_lock);
    return flag_n;
}

uint32_t dbg_flag_hash(int i) { return i >= 0 && i < flag_n ? flag_pairs[i].hash : 0; }
int dbg_flag_value(int i) { return i >= 0 && i < flag_n ? flag_value[i] : 0; }
const char *dbg_flag_name(int i) {
    return i >= 0 && i < flag_n && __atomic_load_n(&names_state, __ATOMIC_ACQUIRE) == 2 ? flag_name[i] : NULL;
}

// ---- the flags' names, from the player's game files
static uint32_t murmur3(const uint8_t *p, size_t len) {
    uint32_t h = 0;
    size_t n = len & ~(size_t)3;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t k;
        memcpy(&k, p + i, 4);
        k *= 0xcc9e2d51u, k = (k << 15) | (k >> 17), k *= 0x1b873593u;
        h ^= k, h = (h << 13) | (h >> 19), h = h * 5 + 0xe6546b64u;
    }
    uint32_t k = 0;
    switch (len & 3) {
    case 3: k ^= (uint32_t)p[n + 2] << 16;   // fall through
    case 2: k ^= (uint32_t)p[n + 1] << 8;    // fall through
    case 1:
        k ^= p[n];
        k *= 0xcc9e2d51u, k = (k << 15) | (k >> 17), k *= 0x1b873593u;
        h ^= k;
    }
    h ^= (uint32_t)len;
    h ^= h >> 16, h *= 0x85ebca6bu, h ^= h >> 13, h *= 0xc2b2ae35u, h ^= h >> 16;
    return h;
}

static int name_char(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
}

// Every run of name characters in a file is hashed and looked up among the flags.
static void names_in(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n;) {
        if (!name_char(p[i])) {
            i++;
            continue;
        }
        size_t e = i;
        while (e < n && name_char(p[e])) e++;
        if (e - i >= 4 && e - i <= 96) {
            Pair key = {murmur3(p + i, e - i), 0};
            Pair *hit = bsearch(&key, flag_pairs, (size_t)flag_n, sizeof(Pair), by_hash);
            if (hit && !flag_name[hit - flag_pairs]) {
                char *s = malloc(e - i + 1);
                if (s) {
                    memcpy(s, p + i, e - i);
                    s[e - i] = 0;
                    flag_name[hit - flag_pairs] = s;
                    names_found++;
                }
            }
        }
        i = e;
    }
}

// A member of a SARC archive: its place and size, 0 when it is not there.
static int sarc_member(const uint8_t *p, size_t n, const char *name, size_t *at, size_t *size) {
    if (n < 0x20 || memcmp(p, "SARC", 4)) return 0;
    uint16_t head, count;
    uint32_t data;
    memcpy(&head, p + 4, 2);
    memcpy(&data, p + 12, 4);
    if ((size_t)head + 12 > n || memcmp(p + head, "SFAT", 4)) return 0;
    memcpy(&count, p + head + 6, 2);
    size_t names = (size_t)head + 12 + (size_t)count * 16 + 8, len = strlen(name);
    for (unsigned i = 0; i < count; i++) {
        uint32_t attr, lo, hi;
        size_t e = (size_t)head + 12 + (size_t)i * 16;
        if (e + 16 > n) return 0;
        memcpy(&attr, p + e + 4, 4), memcpy(&lo, p + e + 8, 4), memcpy(&hi, p + e + 12, 4);
        size_t s = names + (size_t)(attr & 0xffffff) * 4;
        if (s + len + 1 > n || memcmp(p + s, name, len + 1)) continue;
        if (lo > hi || (size_t)data + hi > n) return 0;
        *at = data + lo, *size = hi - lo;
        return 1;
    }
    return 0;
}

// ---- the places to teleport to and the events, from the player's game files (names_thread)
typedef struct {
    const char *area, *pos;
    float xyz[3];              // where it is in the area, metres, when the file gives it (has_xyz)
    int has_xyz;
} Place;
static Place *places;          // by area, as the game's file lists them; the texts stay in the file's buffer
static const char *place_xyz_key;   // the key the positions' coordinates were found under, for the log
static int place_n;
static char **event_names;     // the event files that hold a script, by name
static int event_n;

static uint32_t u24(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }

// ---- individual Skulltulas
// The collection routine at main+0x57a1a0 counts these 100 keys in the manager's separate, sorted Boolean table.
// They are read from the player's executable data; their areas and positions come from the player's Banc files.
enum { SKULL_COUNT = 100 };
static struct { uint64_t key; char area[112]; float xyz[3]; int placed; } skulls[SKULL_COUNT];
static int skull_keys_ready;

static int skull_index(uint64_t m, uint64_t key) {
    uint32_t count = 0;
    if (!m || !key || !rd(m + 0xc58, &count, 4) || !count || count > 200000) return -1;
    uint64_t entries = rd_ptr(m + 0xc60);
    if (!entries) return -1;
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint64_t candidate = 0;
        if (!rd(entries + (uint64_t)mid * 16, &candidate, 8)) return -1;
        if (candidate == key) return (int)mid;
        if (candidate < key) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

int dbg_skulls(void) {
    return __atomic_load_n(&names_state, __ATOMIC_ACQUIRE) == 2 && skull_keys_ready ? SKULL_COUNT : 0;
}
const char *dbg_skull_area(int i) { return i >= 0 && i < dbg_skulls() ? skulls[i].area : ""; }
int dbg_skull_pos(int i, float xyz[3]) {
    if (i < 0 || i >= dbg_skulls() || !skulls[i].placed) return 0;
    memcpy(xyz, skulls[i].xyz, sizeof skulls[i].xyz);
    return 1;
}
int dbg_skull_collected(int i) {
    if (i < 0 || i >= dbg_skulls()) return -1;
    uint64_t m = gmd();
    int index = skull_index(m, skulls[i].key);
    uint8_t value = 0;
    return index >= 0 && rd(rd_ptr(m + 0xc60) + (uint64_t)index * 16 + 12, &value, 1) ? value != 0 : -1;
}
// A change carries the placement's position in the list (in the request's hash) for the token count below.
void dbg_skull_set(int i, int on) {
    if (i >= 0 && i < dbg_skulls()) ask((Req){Q_SKULL, (uint32_t)i, skulls[i].key, (uint64_t)(on != 0), 0});
}
void dbg_skull_tokens_match(void) { ask((Req){Q_TOKENS, 0, 0, 0, 0}); }

// The token count is an integer of its own beside the 100 flags: the pause screen shows it, and the House of
// Skulltula and its rewards read it. Picking up a token raises it and sets its flag, so after a change here the count
// is set to the number collected. skull_want holds the value last asked for each flag until the game holds it (the
// manager applies queued changes at its next update), so changes not applied yet count already. The game's thread
// alone uses it.
static int8_t skull_want[SKULL_COUNT] = {[0 ... SKULL_COUNT - 1] = -1};

// The number collected once the changes asked for are applied: now[i] is what the game holds (-1 when it cannot be
// read), want[i] the value asked for (-1 none), forgotten once the game holds it. -1 when a flag can be read neither way.
static int skull_tally(const int now[SKULL_COUNT], int8_t want[SKULL_COUNT]) {
    int n = 0;
    for (int i = 0; i < SKULL_COUNT; i++) {
        if (want[i] >= 0 && now[i] == want[i]) want[i] = -1;
        if (want[i] >= 0) n += want[i];
        else if (now[i] < 0) return -1;
        else n += now[i] > 0;
    }
    return n;
}

// A named entrance in the most specific matching area, closest to this placement. An area variant must match a
// complete prefix: Child is not Childish, and an Adult start must never be used for a Child placement.
static int skull_nearest_place(int i) {
    if (i < 0 || i >= dbg_skulls() || !skulls[i].placed) return -1;
    int best = -1;
    size_t specificity = 0;
    double best_distance = 0;
    for (int p = 0; p < place_n; p++) {
        const Place *place = &places[p];
        size_t n = strlen(place->area);
        if (!place->has_xyz || !n || strncmp(skulls[i].area, place->area, n) ||
            (skulls[i].area[n] && skulls[i].area[n] != '_') || n < specificity) continue;
        double distance = 0;
        for (int axis = 0; axis < 3; axis++) {
            double delta = (double)skulls[i].xyz[axis] - place->xyz[axis];
            distance += delta * delta;
        }
        if (!isfinite(distance)) continue;
        if (best < 0 || n > specificity || distance < best_distance) {
            best = p, specificity = n, best_distance = distance;
        }
    }
    return best;
}

const char *dbg_skull_entrance(int i) {
    int p = skull_nearest_place(i);
    return p >= 0 ? places[p].pos : NULL;
}

int dbg_skull_travel(int i) {
    int p = skull_nearest_place(i);
    if (p < 0 || dbg_busy()) return 0;
    dbg_warp(places[p].area, places[p].pos);
    return 1;
}

// The i-th key of a BYML file's key table (a node 0xc2: its count, then the offsets of its texts).
static const char *byml_key(const uint8_t *p, size_t n, uint32_t keys, uint32_t i) {
    uint32_t off;
    if ((size_t)keys + 8 > n || p[keys] != 0xc2 || i >= u24(p + keys + 1) || (size_t)keys + 8 + 4 * (size_t)i > n) return NULL;
    memcpy(&off, p + keys + 4 + 4 * (size_t)i, 4);
    if ((size_t)keys + off >= n || !memchr(p + keys + off, 0, n - keys - off)) return NULL;
    return (const char *)p + keys + off;
}

// A bounded lookup in a little-endian Banc dictionary. The returned value is inline for scalars, an offset otherwise.
static int skull_field(const uint8_t *p, size_t n, uint32_t keys, uint32_t node, const char *name,
                       uint8_t kind, uint32_t *value) {
    if ((uint64_t)node + 4 > n || p[node] != 0xc1) return 0;
    uint32_t count = u24(p + node + 1);
    if ((uint64_t)node + 4 + (uint64_t)count * 8 > n) return 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = p + node + 4 + i * 8;
        const char *key = byml_key(p, n, keys, u24(e));
        if (key && !strcmp(key, name) && e[3] == kind) {
            memcpy(value, e + 4, 4);
            return 1;
        }
    }
    return 0;
}

static int byml_vec3(const uint8_t *p, size_t n, uint32_t keys, uint32_t off, uint8_t kind, float out[3], int depth,
                     const char **key);

static void skulls_in(const uint8_t *p, size_t n, const char *path) {
    if (n < 16 || memcmp(p, "YB", 2)) return;
    uint32_t keys, strings, root, actors;
    memcpy(&keys, p + 4, 4); memcpy(&strings, p + 8, 4); memcpy(&root, p + 12, 4);
    if (!skull_field(p, n, keys, root, "Actors", 0xc0, &actors) || (uint64_t)actors + 4 > n || p[actors] != 0xc0) return;
    uint32_t count = u24(p + actors + 1);
    uint64_t values = (uint64_t)actors + 4 + (((uint64_t)count + 3) & ~3ull);
    if (values + (uint64_t)count * 4 > n) return;
    for (uint32_t i = 0; i < count; i++) {
        if (p[actors + 4 + i] != 0xc1) continue;
        uint32_t actor, gyaml, at;
        memcpy(&actor, p + values + i * 4, 4);
        if (!skull_field(p, n, keys, actor, "Gyaml", 0xa0, &gyaml)) continue;
        const char *name = byml_key(p, n, strings, gyaml);
        if (!name || strcmp(name, "Enemy_Skulltula_Gold") ||
            !skull_field(p, n, keys, actor, "Hash", 0xd5, &at) || (uint64_t)at + 8 > n) continue;
        uint64_t key;
        memcpy(&key, p + at, 8);
        for (int s = 0; s < SKULL_COUNT; s++) {
            if (skulls[s].key != key || skulls[s].placed) continue;
            const char *area = strrchr(path, '/');
            snprintf(skulls[s].area, sizeof skulls[s].area, "%s", area ? area + 1 : path);
            char *dot = strchr(skulls[s].area, '.');
            if (dot) *dot = 0;
            const char *coord_key = NULL;
            skulls[s].placed = byml_vec3(p, n, keys, actor, 0xc1, skulls[s].xyz, 1, &coord_key);
        }
    }
}

// Three floats at node off of the given kind: an array of three floats (node 0xc0: its count, the kinds of its items
// padded to four bytes, then the items; a float is kind 0xd2, its bits in place), or found in a dictionary under the
// first key that holds one, a key whose name says where (Translate, Trans, Pos) taken before the others. depth bounds
// the search. *key: the key it was found under.
static int byml_vec3(const uint8_t *p, size_t n, uint32_t keys, uint32_t off, uint8_t kind, float out[3], int depth,
                     const char **key) {
    if ((size_t)off + 4 > n) return 0;
    uint32_t count = u24(p + off + 1);
    if (kind == 0xc0 && p[off] == 0xc0 && count == 3) {
        uint32_t vals = off + 4 + ((count + 3) & ~3u);
        if ((size_t)vals + 12 > n || p[off + 4] != 0xd2 || p[off + 5] != 0xd2 || p[off + 6] != 0xd2) return 0;
        memcpy(out, p + vals, 12);
        return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
    }
    if (kind != 0xc1 || p[off] != 0xc1 || depth <= 0) return 0;
    int found = 0;
    for (int pass = 0; pass < 2 && !found; pass++)
        for (uint32_t i = 0; i < count && !found && (size_t)off + 12 + 8 * (size_t)i <= n; i++) {
            const uint8_t *e = p + off + 4 + 8 * (size_t)i;
            const char *name = byml_key(p, n, keys, u24(e));
            int named = name && (strstr(name, "Trans") || strstr(name, "Pos"));
            if (pass == 0 && !named) continue;
            uint32_t at;
            memcpy(&at, e + 4, 4);
            if (byml_vec3(p, n, keys, at, e[3], out, depth - 1, key)) {
                if (key && name && depth == 2) *key = name;
                found = 1;
            }
        }
    return found;
}

// The start positions file: a dictionary (node 0xc1: its count, then entries of a key's number in 24 bits, the
// value's kind and the value's offset) of areas, each a dictionary of its start positions.
static void places_in(const uint8_t *p, size_t n) {
    uint32_t keys, root;
    if (n < 16 || memcmp(p, "YB", 2)) return;
    memcpy(&keys, p + 4, 4), memcpy(&root, p + 12, 4);
    if ((size_t)root + 4 > n || p[root] != 0xc1) return;
    uint32_t areas = u24(p + root + 1);
    Place *list = malloc(sizeof(Place) * 4096);
    int count = 0;
    for (uint32_t i = 0; list && i < areas && (size_t)root + 12 + 8 * (size_t)i <= n; i++) {
        const uint8_t *e = p + root + 4 + 8 * (size_t)i;
        const char *area = byml_key(p, n, keys, u24(e));
        uint32_t at;
        memcpy(&at, e + 4, 4);
        if (!area || e[3] != 0xc1 || (size_t)at + 4 > n || p[at] != 0xc1 || strlen(area) > 63) continue;
        uint32_t spots = u24(p + at + 1);
        for (uint32_t k = 0; k < spots && count < 4096 && (size_t)at + 12 + 8 * (size_t)k <= n; k++) {
            const uint8_t *pe = p + at + 4 + 8 * (size_t)k;
            const char *pos = byml_key(p, n, keys, u24(pe));
            if (!pos || !pos[0] || strlen(pos) > 63) continue;
            Place pl = {area, pos, {0, 0, 0}, 0};
            uint32_t val;
            memcpy(&val, pe + 4, 4);
            pl.has_xyz = byml_vec3(p, n, keys, val, pe[3], pl.xyz, 2, &place_xyz_key);
            list[count++] = pl;
        }
    }
    places = list, place_n = count;
}

static void names_thread(void *arg) {
    (void)arg;
    uint64_t t0 = plat_now_ns(), dn = 0;
    uint8_t *dz = romfs_load("Pack/ZsDic.pack.zs", &dn), *dics = NULL;
    ZSTD_DDict *dd = NULL, *banc_dd = NULL;
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    unsigned long long dsize = dz ? ZSTD_getFrameContentSize(dz, (size_t)dn) : 0;
    size_t at = 0, size = 0;
    int files = 0;
    if (dz && dsize && dsize < (64u << 20) && (dics = malloc((size_t)dsize)) &&
        ZSTD_decompress(dics, (size_t)dsize, dz, (size_t)dn) == dsize && sarc_member(dics, (size_t)dsize, "zs.zsdic", &at, &size))
        dd = ZSTD_createDDict(dics + at, size);
    if (dics && sarc_member(dics, (size_t)dsize, "bcett.byml.zsdic", &at, &size))
        banc_dd = ZSTD_createDDict(dics + at, size);
    uint64_t keys[SKULL_COUNT];
    if (rd(MOD_MAIN_BASE + 0x3f0b230, keys, sizeof keys)) {
        skull_keys_ready = 1;
        for (int i = 0; i < SKULL_COUNT; i++) {
            if (!keys[i]) skull_keys_ready = 0;
            for (int j = 0; j < i; j++) if (keys[j] == keys[i]) skull_keys_ready = 0;
            skulls[i].key = keys[i];
            snprintf(skulls[i].area, sizeof skulls[i].area, "Unknown area");
        }
    }
    if (dd && dctx) {
        uint64_t count = romfs_file_count();
        for (uint64_t i = 0; i < count; i++) {
            uint64_t off, fsize;
            const char *path = romfs_entry(i, &off, &fsize);
            size_t len = path ? strlen(path) : 0;
            if (!path || strncmp(path, "Event/EventFlow/", 16) || len < 4 || strcmp(path + len - 3, ".zs")) continue;
            uint8_t *z = romfs_load(path, &fsize);
            unsigned long long rn = z ? ZSTD_getFrameContentSize(z, (size_t)fsize) : 0;
            uint8_t *raw = z && rn && rn < (64u << 20) ? malloc((size_t)rn) : NULL;
            if (raw && ZSTD_decompress_usingDDict(dctx, raw, (size_t)rn, z, (size_t)fsize, dd) == rn) {
                names_in(raw, (size_t)rn);
                files++;
                // an event file that holds a script (the count at 0x20) can be started by its name
                uint16_t scripts = 0;
                if (rn > 0x22 && !memcmp(raw, "BFEVFL", 6)) memcpy(&scripts, raw + 0x20, 2);
                const char *dot = strchr(path + 16, '.');
                if (scripts && dot && dot - (path + 16) < 80 && (event_names || (event_names = calloc((size_t)count, sizeof *event_names)))) {
                    char *name = calloc(1, (size_t)(dot - (path + 16)) + 1);
                    if (name) memcpy(name, path + 16, (size_t)(dot - (path + 16))), event_names[event_n++] = name;
                }
            }
            free(raw);
            free(z);
        }
        uint64_t zn = 0;
        uint8_t *z = romfs_load("Banc/BancBasedResource/StartPos.byml.zs", &zn);
        unsigned long long rn = z ? ZSTD_getFrameContentSize(z, (size_t)zn) : 0;
        uint8_t *raw = z && rn && rn < (16u << 20) ? malloc((size_t)rn) : NULL;   // kept: the places' texts are in it
        if (raw && ZSTD_decompress_usingDDict(dctx, raw, (size_t)rn, z, (size_t)zn, dd) == rn) places_in(raw, (size_t)rn);
        free(z);
    }
    if (banc_dd && dctx && skull_keys_ready) {
        uint64_t count = romfs_file_count();
        for (uint64_t i = 0; i < count; i++) {
            uint64_t off, zn;
            const char *path = romfs_entry(i, &off, &zn);
            size_t len = path ? strlen(path) : 0;
            if (!path || strncmp(path, "Banc/", 5) || len < 15 || strcmp(path + len - 14, ".bcett.byml.zs")) continue;
            uint8_t *z = romfs_load(path, &zn);
            unsigned long long rn = z ? ZSTD_getFrameContentSize(z, (size_t)zn) : 0;
            uint8_t *raw = z && rn && rn < (64u << 20) ? malloc((size_t)rn) : NULL;
            if (raw && ZSTD_decompress_usingDDict(dctx, raw, (size_t)rn, z, (size_t)zn, banc_dd) == rn)
                skulls_in(raw, (size_t)rn, path);
            free(raw);
            free(z);
        }
    }
    ZSTD_freeDCtx(dctx);
    ZSTD_freeDDict(dd);
    ZSTD_freeDDict(banc_dd);
    free(dics);
    free(dz);
    int skull_placed = 0;
    for (int i = 0; i < SKULL_COUNT; i++) skull_placed += skulls[i].placed;
    rt_log("debug: Skulltula collection keys %s; %d of %d mapped to placements", skull_keys_ready ? "loaded" : "unavailable",
           skull_placed, SKULL_COUNT);
    int with_xyz = 0;
    for (int i = 0; i < place_n; i++) with_xyz += places[i].has_xyz;
    rt_log("debug: names for %d of %d flags, from %d of the game's event files, in %.0f ms; %d events, %d places to teleport to "
           "(%d with their coordinates, under \"%s\")", names_found, flag_n, files, (double)(plat_now_ns() - t0) / 1e6,
           event_n, place_n, with_xyz, place_xyz_key ? place_xyz_key : "");
    if (cfg_get("NX2NAVI_DBG_TEST"))   // test runs: every place with its coordinates, to plan routes (dbg_route)
        for (int i = 0; i < place_n; i++)
            rt_log("debug: place %s > %s at %.1f %.1f %.1f%s", places[i].area, places[i].pos, places[i].xyz[0],
                   places[i].xyz[1], places[i].xyz[2], places[i].has_xyz ? "" : " (no coordinates)");
    __atomic_store_n(&names_state, 2, __ATOMIC_RELEASE);
}

// 0 not started (no flags yet), 1 being read, 2 there. Starts the reading once the flags are listed.
int dbg_names(int *found) {
    int s = __atomic_load_n(&names_state, __ATOMIC_ACQUIRE);
    if (!s && flag_pairs) {
        int32_t zero = 0;
        if (__atomic_compare_exchange_n(&names_state, &zero, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (!plat_thread_spawn(names_thread, NULL, "debug names", 0)) __atomic_store_n(&names_state, 2, __ATOMIC_RELEASE);
            s = 1;
        }
    }
    if (found) *found = s == 2 ? names_found : 0;
    return s;
}

int dbg_places(void) { return __atomic_load_n(&names_state, __ATOMIC_ACQUIRE) == 2 ? place_n : 0; }
const char *dbg_place_area(int i) { return i >= 0 && i < dbg_places() ? places[i].area : ""; }
const char *dbg_place_pos(int i) { return i >= 0 && i < dbg_places() ? places[i].pos : ""; }

// The start position of area that comes after pos in the game's list, the first one for a pos the area does not list
// (or none given), NULL for an area without any. A change of area to a start position the area does not have puts
// Link where nothing holds him: he falls below the map for good, and the game refuses events while he falls.
static const char *place_after(const char *area, const char *pos) {
    const char *first = NULL;
    int seen = 0, n = dbg_places();
    for (int i = 0; i < n; i++) {
        if (strcmp(places[i].area, area)) continue;
        if (seen) return places[i].pos;
        if (!first) first = places[i].pos;
        if (pos && pos[0] && !strcmp(places[i].pos, pos)) seen = 1;
    }
    return first;
}

// The start position of area nearest to xyz among those the game's file gives coordinates for (a bug capture names
// it, and a replay of the capture arrives there, bugcap.c). 0 while the list is not read or the area has none.
int dbg_place_nearest(const char *area, const float xyz[3], char name[64], float *dist) {
    int n = dbg_places(), best = -1;
    float best_d = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(places[i].area, area) || !places[i].has_xyz) continue;
        float dx = places[i].xyz[0] - xyz[0], dy = places[i].xyz[1] - xyz[1], dz = places[i].xyz[2] - xyz[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (best < 0 || d < best_d) best = i, best_d = d;
    }
    if (best < 0) return 0;
    snprintf(name, 64, "%s", places[best].pos);
    if (dist) *dist = best_d;
    return 1;
}

static int place_listed(const char *area, const char *pos) {
    int n = dbg_places();
    for (int i = 0; i < n; i++)
        if (!strcmp(places[i].area, area) && !strcmp(places[i].pos, pos)) return 1;
    return 0;
}
int dbg_events(void) { return __atomic_load_n(&names_state, __ATOMIC_ACQUIRE) == 2 ? event_n : 0; }
const char *dbg_event_name(int i) { return i >= 0 && i < dbg_events() ? event_names[i] : ""; }

// ---- the game data's other kinds: the manager holds a section of 0x60 bytes for each kind (the kind's number times
// 0x60: +0x10 the entries' count, +0x18 the entries, +0x20 the {hash, index} table, +0x2c its size). An entry of a
// 64-character text is 0x108 bytes, the text's pointer at +0x68 (a second one at +0x18) and its room at +0x70.
// main+0x3e0980 (manager, value, hash) queues the change of an enum, whose value is the hash of the value's name.
enum { GD_TEXT64 = 16 };
static uint32_t hash_of(const char *text) { return murmur3((const uint8_t *)text, strlen(text)); }

static uint64_t gmd_entry(int kind, uint32_t stride, uint32_t hash) {
    uint64_t m = gmd(), sec = m + (uint64_t)kind * 0x60, table = m ? rd_ptr(sec + 0x20) : 0, entries = m ? rd_ptr(sec + 0x18) : 0;
    uint32_t count = 0, size = 0;
    if (!table || !entries || !rd(sec + 0x10, &count, 4) || !rd(sec + 0x2c, &size, 4) || !size || size > 100000) return 0;
    for (uint32_t i = hash % size, n = 0; n < size; i = i + 1 < size ? i + 1 : 0, n++) {
        Pair pr;
        if (!rd(table + (uint64_t)i * 8, &pr, 8) || !pr.hash) return 0;
        if (pr.hash == hash) return pr.index < count ? entries + (uint64_t)pr.index * stride : 0;
    }
    return 0;
}

static int text_get(const char *name, char out[64]) {
    uint64_t e = gmd_entry(GD_TEXT64, 0x108, hash_of(name)), text = e ? rd_ptr(e + 0x68) : 0;
    out[0] = 0;
    if (!text || !rd(text, out, 63)) return 0;
    out[63] = 0;
    return 1;
}

// Every 64-character text of the game data with its name's hash (a bug capture lists them, bugcap.c): the area, the
// reserved next area and start position among them. Returns how many it visited.
int dbg_texts(void (*cb)(uint32_t hash, const char *text, void *ctx), void *ctx) {
    uint64_t m = dbg_supported() ? gmd() : 0, sec = m + (uint64_t)GD_TEXT64 * 0x60;
    uint64_t table = m ? rd_ptr(sec + 0x20) : 0, entries = m ? rd_ptr(sec + 0x18) : 0;
    uint32_t count = 0, size = 0;
    if (!table || !entries || !rd(sec + 0x10, &count, 4) || !rd(sec + 0x2c, &size, 4) || !size || size > 100000) return 0;
    int n = 0;
    for (uint32_t i = 0; i < size; i++) {
        Pair pr;
        char text[64];
        if (!rd(table + (uint64_t)i * 8, &pr, 8) || !pr.hash || pr.index >= count) continue;
        uint64_t tp = rd_ptr(entries + (uint64_t)pr.index * 0x108 + 0x68);
        if (!tp || !rd(tp, text, 63)) continue;
        text[63] = 0;
        cb(pr.hash, text, ctx);
        n++;
    }
    return n;
}
uint32_t dbg_name_hash(const char *name) { return hash_of(name); }

// The area the game is in: its text Sequence_CurrentBanc. 0 while the game has none.
int dbg_area(char out[64]) { return dbg_supported() && text_get("Sequence_CurrentBanc", out) && out[0]; }

// A text written into the entry's own buffers (game thread). 0: the game has no such text.
static int text_set(const char *name, const char *value) {
    uint64_t e = gmd_entry(GD_TEXT64, 0x108, hash_of(name)), text = e ? rd_ptr(e + 0x68) : 0, other = e ? rd_ptr(e + 0x18) : 0;
    int32_t room = 0;
    char buf[64] = {0};
    if (!text || !rd(e + 0x70, &room, 4) || room < 64) return 0;
    snprintf(buf, sizeof buf, "%s", value);
    if (other && other != text) wr(other, buf, 64);
    return wr(text, buf, 64);
}

static void enum_set(Cpu *c, const char *name, const char *value) {
    rt_call(c, MOD_MAIN_BASE + 0x3e0980, 3, gmd(), (uint64_t)hash_of(value), (uint64_t)hash_of(name));
}

// Integers are kind 2: an entry is 0x28 bytes, its value at +0x1c. main+0xfc42d0 (manager, amount, hash) queues an
// addition to one: a record {amount, hash, 1} of 12 bytes at [manager+0xf8], the room for them at +0xf0 and the
// records in use in the low 20 bits of +0x100. The game applies them at its next update and adds the amount to the
// value (eight calls with 29 took a count of 11 to 243), so the count is changed by the difference.
enum { GD_INT = 2 };
static const char TOKEN_COUNT[] = "PouchData.Item_Token_Gold.StockNum";

static int32_t token_base, token_pending;   // the count read when additions were queued, and their sum not applied yet
// What to add for the count to reach target, now being the count the game holds. Additions queued before count until
// the game has applied them, which shows as a count other than the one read when they were queued.
static int32_t skull_token_delta(int target, int now) {
    if (token_pending && now != token_base) token_pending = 0;
    int32_t delta = target - (now + token_pending);
    if (!delta) return 0;
    if (!token_pending) token_base = now;
    token_pending += delta;
    return delta;
}

int dbg_skull_tokens(void) {
    uint64_t e = gmd_entry(GD_INT, 0x28, hash_of(TOKEN_COUNT));
    int32_t value = 0;
    return e && rd(e + 0x1c, &value, 4) ? value : -1;
}

// Retire requests as soon as the game applies them, including ticks with no menu request. Otherwise a later save
// reload could turn an old, already applied request into a pending edit of the restored save.
static void skull_pending_refresh(void) {
    for (int i = 0; i < SKULL_COUNT; i++)
        if (skull_want[i] >= 0 && dbg_skull_collected(i) == skull_want[i]) skull_want[i] = -1;
    if (token_pending) {
        int held = dbg_skull_tokens();
        if (held >= 0 && held != token_base) token_pending = 0;
    }
}

// The game's thread: the token count set to the number collected (skull_tally).
static void skull_tokens_sync(Cpu *c, uint64_t m) {
    int now[SKULL_COUNT];
    for (int i = 0; i < SKULL_COUNT; i++) now[i] = dbg_skull_collected(i);
    int count = skull_tally(now, skull_want), held = dbg_skull_tokens();
    uint32_t hash = hash_of(TOKEN_COUNT), room = 0, used = 0;
    if (count < 0 || held < 0 || !rd(m + 0xf0, &room, 4) || !rd(m + 0x100, &used, 4) || (used & 0xfffff) >= room) {
        stat_refused++;
        rt_log("debug: token count not set: a collection flag, the count or the game's queue unavailable");
        return;
    }
    int32_t delta = skull_token_delta(count, held);
    if (!delta) return;
    rt_call(c, MOD_MAIN_BASE + 0xfc42d0, 3, m, (uint64_t)(uint32_t)delta, (uint64_t)hash);
    rt_log("debug: token count %d changed by %d to %d, the Skulltulas collected", held, delta, count);
}

// ---- events. main+0xd79f10 builds an event request in a block of 0x2000 bytes: the event's name goes into the buffer
// its +0x1a90 points to (the room at +0x1a98). main+0xd7a820 (the event manager **[main+0x4c7e090], request) hands it
// in and answers whether the event starts; the request's two lists at +0x1968 and +0x1950 are then destroyed
// (main+0x20b6f0, their table main+0x4974b20), as the game's own request node does (main+0x32db1a0).
static int event_request(Cpu *c, const char *name) {
    static _Alignas(16) uint8_t arg[0x2000];
    uint64_t a = (uint64_t)(uintptr_t)arg, slot = rd_ptr(MOD_MAIN_BASE + 0x4c7e090), mgr = slot ? rd_ptr(slot) : 0;
    if (!mgr) return 0;
    memset(arg, 0, sizeof arg);
    rt_call(c, MOD_MAIN_BASE + 0xd79f10, 1, a);
    uint64_t buf = rd_ptr(a + 0x1a90), took = 0;
    int32_t room = 0;
    rd(a + 0x1a98, &room, 4);
    if (buf >= a && buf < a + sizeof arg && room > 1 && strlen(name) < (size_t)room) {
        memcpy((void *)(uintptr_t)buf, name, strlen(name) + 1);
        took = rt_call(c, MOD_MAIN_BASE + 0xd7a820, 2, mgr, a) & 0xff;
    }
    uint64_t vt = MOD_MAIN_BASE + 0x4974b20;
    memcpy(arg + 0x1968, &vt, 8);
    rt_call(c, MOD_MAIN_BASE + 0x20b6f0, 1, a + 0x1968);
    memcpy(arg + 0x1950, &vt, 8);
    rt_call(c, MOD_MAIN_BASE + 0x20b6f0, 1, a + 0x1950);
    return took != 0;
}

// ---- the teleport and the event start: asked for by the menu, done by warp_tick on the game's thread
// W_SETTLE: Link arrived and is watched for a while; one who falls FALL_M below where he arrived has no ground under
// him, and the change of area is asked again for the area's next start position (place_after), up to FALL_TRIES times.
// The game refuses events while Link is in the air, so such a request is tried again every RETRY_EVERY ticks until
// RETRY_FOR ticks have passed.
enum { W_IDLE, W_ASKED, W_EVENT, W_FLIGHT, W_SETTLE };
enum { SETTLE_TICKS = 180, FALL_TRIES = 3, RETRY_EVERY = 30, RETRY_FOR = 1200, LIST_WAIT = 600 };
static const float FALL_M = 25.0f;   // metres: an arrival's own drop is a step or two
static struct {
    char area[64], pos[64], event[80];   // q_lock: what the menu asked for
    int state;                           // W_*
    uint64_t at;                         // the tick of the next step, or the one at which the wait for the arrival ends
    uint64_t retry_until, waited;        // W_ASKED: refused requests are tried again until then; ticks spent on the list
    float from[3];                       // where Link stood
    float land;                          // the height he arrived at
    int tries;                           // start positions tried after falls
} warp;
static char status[160];                 // q_lock: the last thing that happened, for the menu

static void status_set(const char *fmt, const char *a, const char *b) {
    plat_mutex_lock(&q_lock);
    snprintf(status, sizeof status, fmt, a, b);
    plat_mutex_unlock(&q_lock);
    rt_log("debug: %s", status);
}

void dbg_status(char out[160]) {
    plat_mutex_lock(&q_lock);
    memcpy(out, status, sizeof status);
    plat_mutex_unlock(&q_lock);
}

int dbg_busy(void) { return warp.state != W_IDLE || warp.event[0]; }

void dbg_warp(const char *area, const char *pos) {
    plat_mutex_lock(&q_lock);
    if (warp.state == W_IDLE && !warp.event[0] && area[0]) {
        snprintf(warp.area, sizeof warp.area, "%s", area);
        snprintf(warp.pos, sizeof warp.pos, "%s", pos);
        warp.tries = 0, warp.retry_until = 0, warp.waited = 0, warp.at = 0;
        warp.state = W_ASKED;
    }
    plat_mutex_unlock(&q_lock);
}

void dbg_event_start(const char *name) {
    plat_mutex_lock(&q_lock);
    if (warp.state == W_IDLE && !warp.event[0]) snprintf(warp.event, sizeof warp.event, "%s", name);
    plat_mutex_unlock(&q_lock);
}

static const char REFUSED[] = "The game refused %s: it is in a cutscene, a conversation or a menu, or Link cannot take part "
                              "now (swimming, in the air). Try again once he stands free.";

static void warp_tick(Cpu *c, uint64_t tick) {
    if (warp.event[0]) {
        char name[80];
        snprintf(name, sizeof name, "%s", warp.event);
        status_set(player() && event_request(c, name) ? "Started the event %s." : REFUSED, name, "");
        plat_mutex_lock(&q_lock);
        warp.event[0] = 0;
        plat_mutex_unlock(&q_lock);
    }
    if (warp.state == W_ASKED && tick >= warp.at) {
        // The area's own list of start positions decides the position: read with the flag names, which a test run that
        // never opened the menu has not started yet
        if (dbg_names(NULL) != 2 && warp.waited++ < LIST_WAIT) {
            dbg_flags_refresh(), dbg_names(NULL);
            return;
        }
        if (dbg_places() && !place_listed(warp.area, warp.pos)) {
            const char *first = place_after(warp.area, NULL);
            if (!first) {
                status_set("%s is not an area of the game's list of start positions: no teleport.", warp.area, "");
                warp.state = W_IDLE;
                return;
            }
            rt_log("debug: %s lists no start position \"%s\": %s, the first it lists, instead", warp.area, warp.pos, first);
            plat_mutex_lock(&q_lock);
            snprintf(warp.pos, sizeof warp.pos, "%s", first);
            plat_mutex_unlock(&q_lock);
        }
        // the game's event that only waits: the change of area is asked for while it runs
        if (player() && gmd() && dbg_player_pos(warp.from) && event_request(c, "DefaultParentEvent")) {
            warp.at = tick + 4, warp.state = W_EVENT;
        } else if (tick < warp.retry_until) {
            warp.at = tick + RETRY_EVERY;   // Link still falls: the game takes it once he lands, or the time runs out
        } else {
            status_set(warp.tries ? "The game refuses a teleport while Link falls below %s: load the save, or try again "
                                    "should he land."
                                  : REFUSED,
                       warp.tries ? warp.area : "the teleport", "");
            warp.state = W_IDLE;
        }
    } else if (warp.state == W_EVENT && tick >= warp.at) {
        // what the game's own script reserves (its actor's ReserveBancChange): the next area and the start position's
        // name, no event at the arrival, the event held during the change, a black fade, and the request itself
        int ok = text_set("Sequence_ReservedNextBanc", warp.area) && text_set("Sequence_ReservedNextStartPosName", warp.pos);
        text_set("Sequence_ReservedBootEventName", "");
        text_set("Sequence_ReservedEventStarterName", "");
        if (ok) {
            rt_call(c, MOD_MAIN_BASE + 0x682b20, 3, gmd(), (uint64_t)1, (uint64_t)hash_of("Sequence_IsEventBancChangePause"));
            enum_set(c, "Sequence_BancChangeFadePattern", "BlackFade");
            enum_set(c, "Sequence_ControlInEvent", "ChangeBancOnEvent");
            status_set("Teleporting to %s, %s...", warp.area, warp.pos[0] ? warp.pos : "its default start");
            warp.at = tick + 3600, warp.state = W_FLIGHT;
        } else {
            status_set("The teleport to %s could not be asked for: the game's texts for it are not there.", warp.area, "");
            warp.state = W_IDLE;
        }
    } else if (warp.state == W_FLIGHT) {
        char now[64];
        float p[3] = {0, 0, 0};
        int there = text_get("Sequence_CurrentBanc", now) && !strcmp(now, warp.area) && dbg_player_pos(p) &&
                    fabsf(p[0] - warp.from[0]) + fabsf(p[1] - warp.from[1]) + fabsf(p[2] - warp.from[2]) > 0.5f;
        if (there || tick >= warp.at) {
            status_set(there ? "Arrived in %s, %s." : "The teleport to %s, %s was asked for; no arrival was seen.", warp.area,
                       warp.pos[0] ? warp.pos : "its default start");
            warp.state = there ? W_SETTLE : W_IDLE;
            warp.land = p[1], warp.at = tick + SETTLE_TICKS;
            dbg_freecam_reset();   // a free camera left behind in the old area's place starts again from the game's view
        }
    } else if (warp.state == W_SETTLE) {
        float p[3];
        if (dbg_player_pos(p) && p[1] < warp.land - FALL_M) {
            const char *next = place_after(warp.area, warp.pos);
            if (warp.tries < FALL_TRIES && next && strcmp(next, warp.pos)) {
                status_set("Link fell through the ground at %s: the next start position, %s...", warp.area, next);
                plat_mutex_lock(&q_lock);
                snprintf(warp.pos, sizeof warp.pos, "%s", next);
                plat_mutex_unlock(&q_lock);
                warp.tries++, warp.retry_until = tick + RETRY_FOR, warp.at = tick, warp.state = W_ASKED;
            } else {
                status_set("Link fell through the ground at %s, %s, and the area has no other start position left to try: "
                           "load the save.", warp.area, warp.pos);
                warp.state = W_IDLE;
            }
        } else if (tick >= warp.at) {
            warp.state = W_IDLE;
        }
    }
}

// ---- the memory search: values of 1, 2 or 4 bytes in the game's heap
enum { RESULT_MAX = 2000000 };
static uint64_t *res_addr;
static uint32_t *res_last;
static int res_n, res_size, res_over;

int dbg_mem_read(uint64_t addr, int size, uint32_t *v) {
    *v = 0;
    return (size == 1 || size == 2 || size == 4) && rd(addr, v, (size_t)size);
}

int dbg_search_count(int *over) {
    if (over) *over = res_over;
    return res_n;
}
uint64_t dbg_search_addr(int i) { return i >= 0 && i < res_n ? res_addr[i] : 0; }
int dbg_search_size(void) { return res_size; }
void dbg_search_clear(void) { res_n = res_over = 0; }

// A new search: every place in the heap, aligned to the size, that holds the value. Returns how many (at most
// RESULT_MAX are kept; dbg_search_count's over says there were more).
int dbg_search_new(uint32_t value, int size) {
    enum { PIECE = 16 << 20 };
    if (!res_addr) res_addr = malloc(sizeof(uint64_t) * RESULT_MAX), res_last = malloc(sizeof(uint32_t) * RESULT_MAX);
    res_n = res_over = 0;
    res_size = size;
    if (!res_addr || !res_last || !dbg_supported()) return 0;
    const RtGuestLayout *l = mem_layout();
    const RtRange ranges[2] = {l->heap, l->alias};
    for (int r = 0; r < 2; r++) {
        uint64_t a = ranges[r].base, end = ranges[r].base + ranges[r].size;
        while (a < end) {
            RtMemoryInfo mi;
            if (mem_query(a, &mi) || !mi.size) break;
            uint64_t stop = mi.base + mi.size < end ? mi.base + mi.size : end;
            if (!mi.state || (mi.perm & 3) != 3) {   // free, or not the game's to read and write
                a = stop;
                continue;
            }
            while (a < stop) {
                uint64_t n = stop - a < PIECE ? stop - a : PIECE;
                if (mem_read_hold(a, n)) {
                    const uint8_t *p = (const uint8_t *)(uintptr_t)a;
#define FIND(T)                                                                                                        \
    for (uint64_t i = 0; i + sizeof(T) <= n; i += sizeof(T)) {                                                         \
        T x;                                                                                                           \
        memcpy(&x, p + i, sizeof(T));                                                                                  \
        if (x == (T)value) {                                                                                           \
            if (res_n < RESULT_MAX) res_addr[res_n] = a + i, res_last[res_n++] = value;                                \
            else res_over = 1;                                                                                         \
        }                                                                                                              \
    }
                    if (size == 4) { FIND(uint32_t) } else if (size == 2) { FIND(uint16_t) } else { FIND(uint8_t) }
#undef FIND
                    mem_read_release();
                }
                a += n;
            }
        }
    }
    return res_n;
}

// Keeps the results whose value now passes: mode 0 equals value, 1 changed, 2 unchanged, 3 greater, 4 less than at the
// search before (unsigned; a float is searched by its bits with mode 0).
int dbg_search_next(int mode, uint32_t value) {
    int kept = 0;
    for (int i = 0; i < res_n; i++) {
        uint32_t v, was = res_last[i];
        if (!dbg_mem_read(res_addr[i], res_size, &v)) continue;
        int pass = mode == 0 ? v == value : mode == 1 ? v != was : mode == 2 ? v == was : mode == 3 ? v > was : v < was;
        if (pass) res_addr[kept] = res_addr[i], res_last[kept++] = v;
    }
    res_n = kept;
    res_over = 0;
    return kept;
}

// ---- the free camera. The game's camera update (main+0x4c48c0) chooses the frame's position and target (doubles) and
// up vector between the camera's own and an override, and at main+0x4c4e50 has them in d0-d2, d3-d5 and s7, s16, s6,
// before it stores them into the look-at camera and makes its matrix. The patch set freecam (recomp/patches-v111.txt)
// sends that instruction here while the free camera is on: the registers get the free camera's vectors, so the game
// draws, culls and loads detail for the view it is given, in play and in cutscenes, and Link, the world and the
// game's own camera work go on as they were. The pad's sticks fly it (dbg_pad, from ov_input.c before the game gets the pad's state).
#include "input_map.h"

static struct {
    volatile int on, capture, sticks;   // the menu's: on; take the game's view at the next update; the sticks fly it
    volatile int avoid;                 // the menu's: ground avoidance (dbg_pad), off unless asked for
    volatile float speed;               // metres a second
    double pos[3], reach;               // the game thread's: where it is, and the game's distance to its target
    float yaw, pitch;
    uint64_t last_ns, updates;
    float ground[9], ground_tan, ground_aspect;   // dbg_freecam_ground's, under ground_lock
    volatile uint64_t ground_ns;        // and when; 0 none
} fc = {.sticks = 1, .speed = 8.0f, .reach = 5.0};
static PlatMutex ground_lock = PLAT_MUTEX_INIT;

// The game's own camera as its update chose it, before the free camera stands in: kept per scene camera (0 the one
// that follows Link, 1 the one events direct) while a bug capture watches (dbg_camera_watch) or the free camera is on,
// which is when the patch site calls dbg_freecam_host at all.
static struct { double pos[3], at[3]; float up[3]; uint64_t n; } cam_seen[2];
static PlatMutex cam_lock = PLAT_MUTEX_INIT;
static volatile int32_t cam_watch;   // how many watch it: a bug capture, and Link's walk to a captured place

int dbg_freecam_flying(void) { return fc.on && fc.sticks && !fc.capture && fc.avoid; }
void dbg_freecam_avoid_set(int on) {
    if ((on != 0) != fc.avoid) rt_log("debug: free camera ground avoidance %s", on ? "on" : "off");
    fc.avoid = on != 0;
}
int dbg_freecam_avoid(void) { return fc.avoid; }

// The replay's thread (vk_dlaa.c fcam_probe): view distances read back from the lower part of the view a few frames
// ago, three rows of three (0.6, 0.78 and 0.96 of the way down; 0.2, 0.5 and 0.8 across), 0 where it saw the sky. Kept
// for dbg_pad (ground_under).
void dbg_freecam_ground(const float *dist, int n, float tan_half_y, float aspect) {
    if (n != 9 || !(tan_half_y > 0) || !(aspect > 0)) return;
    plat_mutex_lock(&ground_lock);
    for (int i = 0; i < 9; i++) fc.ground[i] = dist[i] > 0 && isfinite(dist[i]) ? dist[i] : 0;
    fc.ground_tan = tan_half_y, fc.ground_aspect = aspect;
    plat_mutex_unlock(&ground_lock);
    __atomic_store_n(&fc.ground_ns, plat_now_ns(), __ATOMIC_RELEASE);
}

// How far below the camera the ground is, in metres, from the view distances above, or a negative number when they show
// no ground within reach metres ahead. Each sample is a point of the scene: its height against the camera and its
// distance ahead follow from the view's angles and the camera's pitch. In each of the three columns the lowest sample
// counts as ground only when the surface from it to the one above it rises by less than GROUND_SLOPE: the ground the
// camera flies over, a field or a gentle hill. Whatever stands on it or in front of the camera (a character, a rock, a
// wall, a tree) is steeper and is left alone, so the camera can go right up to it; taken for ground too, as at first,
// anything within three metres in the lower view sent the camera upwards on its way to it.
#define GROUND_SLOPE 0.6   /* rise over run: about 30 degrees */
static double ground_under(double reach) {
    float d[9], ty, as;
    plat_mutex_lock(&ground_lock);
    memcpy(d, fc.ground, sizeof d), ty = fc.ground_tan, as = fc.ground_aspect;
    plat_mutex_unlock(&ground_lock);
    static const double xn[3] = {-0.6, 0.0, 0.6}, yn[3] = {-0.2, -0.56, -0.92};   // the samples in the view, -1..1, up positive
    double sp = sin(fc.pitch), cp = cos(fc.pitch), best = -1;
    for (int col = 0; col < 3; col++) {
        double up[3], run[3];   // per row: the point's height against the camera, and how far ahead it is, level
        for (int row = 0; row < 3; row++) {
            double z = d[row * 3 + col], yv = yn[row] * ty * z, xv = xn[col] * ty * as * z, fwd = z * cp - yv * sp;
            up[row] = yv * cp + z * sp, run[row] = sqrt(fwd * fwd + xv * xv);
        }
        if (!(d[6 + col] > 0) || !(d[3 + col] > 0)) continue;   // the lowest sample and the one above it
        double rise = fabs(up[1] - up[2]), along = fabs(run[1] - run[2]);
        if (rise > GROUND_SLOPE * along || up[2] >= 0 || run[2] > reach) continue;
        if (best < 0 || -up[2] < best) best = -up[2];
    }
    return best;
}

static double reg_d(Cpu *c, int r) {
    uint64_t b = (uint64_t)c->v[r];
    double v;
    memcpy(&v, &b, 8);
    return v;
}
static void set_d(Cpu *c, int r, double v) {
    uint64_t b;
    memcpy(&b, &v, 8);
    c->v[r] = b;
}
static void set_s(Cpu *c, int r, float v) {
    uint32_t b;
    memcpy(&b, &v, 4);
    c->v[r] = b;
}

// The update runs for each of the game's cameras, every frame: the one that follows Link and the one its events
// direct are the scene's, and the free camera stands in for both; the camera of the interface's 3D models and the
// one the sound follows stay the game's. A camera's state (x19) has its actor at +0x10, the actor its name at +0x20.
// 1 the camera that follows Link, 2 the events' camera, 0 another.
static int scene_camera(uint64_t state) {
    char name[16] = "";
    uint64_t actor = rd_ptr(state + 0x10), np = actor ? rd_ptr(actor + 0x20) : 0;
    if (!np || !rd(np, name, 13)) return 0;
    return !memcmp(name, "PlayerCamera", 13) ? 1 : !memcmp(name, "EventCamera", 12) ? 2 : 0;
}

// The patch site's host function, on the patch page at +0xf70. patches.c registers it: that file is compiled with
// the game version's patches.h, which has the page's address.
void dbg_freecam_host(Cpu *c) {
    fc.updates++;
    int scene = scene_camera(c->x[19]);
    if (scene) {   // what the game chose, for a bug capture (dbg_camera_seen)
        static const int up_reg[3] = {7, 16, 6};
        plat_mutex_lock(&cam_lock);
        for (int i = 0; i < 3; i++) cam_seen[scene - 1].pos[i] = reg_d(c, i), cam_seen[scene - 1].at[i] = reg_d(c, 3 + i);
        for (int i = 0; i < 3; i++) {
            uint32_t b = (uint32_t)c->v[up_reg[i]];
            memcpy(&cam_seen[scene - 1].up[i], &b, 4);
        }
        cam_seen[scene - 1].n++;
        plat_mutex_unlock(&cam_lock);
    }
    if (fc.on && scene) {
        if (fc.capture) {   // start where the game's camera is, looking where it looks
            double d[3] = {reg_d(c, 3) - reg_d(c, 0), reg_d(c, 4) - reg_d(c, 1), reg_d(c, 5) - reg_d(c, 2)};
            double len = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (isfinite(len) && len > 1e-3) {
                for (int i = 0; i < 3; i++) fc.pos[i] = reg_d(c, i);
                fc.yaw = (float)atan2(d[0], d[2]), fc.pitch = (float)asin(d[1] / len);
                fc.reach = len < 1 ? 1 : len > 50 ? 50 : len;
                fc.capture = 0;
            }
        }
        if (!fc.capture) {
            double cp = cos(fc.pitch), dir[3] = {cp * sin(fc.yaw), sin(fc.pitch), cp * cos(fc.yaw)};
            for (int i = 0; i < 3; i++) set_d(c, i, fc.pos[i]), set_d(c, 3 + i, fc.pos[i] + dir[i] * fc.reach);
            set_s(c, 7, 0), set_s(c, 16, 1), set_s(c, 6, 0);   // up
        }
    }
    set_s(c, 17, (float)reg_d(c, 0));   // the instruction this call stands in for: fcvt s17, d0
}

int dbg_freecam_available(void) { return dbg_supported() && rt_patch_set("freecam") >= 0; }

// The patch site calls dbg_freecam_host while the free camera is on or a bug capture watches the game's camera.
static void cam_patch_sync(void) { rt_patch_switch(rt_patch_set("freecam"), fc.on || cam_watch); }

void dbg_camera_watch(int on) {
    if (!dbg_freecam_available()) return;
    if (__atomic_add_fetch(&cam_watch, on ? 1 : -1, __ATOMIC_ACQ_REL) < 0) __atomic_store_n(&cam_watch, 0, __ATOMIC_RELEASE);
    cam_patch_sync();
}

uint64_t dbg_camera_seen(int which, double pos[3], double at[3], float up[3]) {
    if (which < 0 || which > 1) return 0;
    plat_mutex_lock(&cam_lock);
    memcpy(pos, cam_seen[which].pos, sizeof cam_seen[which].pos);
    memcpy(at, cam_seen[which].at, sizeof cam_seen[which].at);
    memcpy(up, cam_seen[which].up, sizeof cam_seen[which].up);
    uint64_t n = cam_seen[which].n;
    plat_mutex_unlock(&cam_lock);
    return n;
}

// The game's thread: the free camera, which must be on, at a place and direction given outright (a replay of a bug
// capture, bugcap.c), instead of where the game's camera is.
void dbg_freecam_place(const double pos[3], float yaw, float pitch) {
    memcpy(fc.pos, pos, sizeof fc.pos);
    fc.yaw = yaw, fc.pitch = pitch > 1.5f ? 1.5f : pitch < -1.5f ? -1.5f : pitch;
    fc.reach = 5.0;
    fc.capture = 0;
}

void dbg_freecam_set(int on, int sticks, float speed) {
    if (!dbg_freecam_available()) return;
    fc.sticks = sticks != 0;
    fc.speed = speed >= 0.5f && speed <= 200.0f ? speed : 8.0f;
    if (on && !fc.on) fc.capture = 1;
    if ((on != 0) != fc.on) rt_log("debug: free camera %s", on ? "on" : "off");   // what a tester changed, for the log
    fc.on = on != 0;
    cam_patch_sync();
}

void dbg_freecam_reset(void) { fc.capture = 1; }   // back to the game's view, and free from there

int dbg_freecam(int *sticks, float *speed, double pos[3]) {
    if (sticks) *sticks = fc.sticks;
    if (speed) *speed = fc.speed;
    if (pos) memcpy(pos, fc.pos, sizeof fc.pos);
    return fc.on;
}
void dbg_freecam_angles(float *yaw, float *pitch) { *yaw = fc.yaw, *pitch = fc.pitch; }

// Whether s holds the len characters at part, in any case.
static int contains_nocase(const char *s, const char *part, size_t len) {
    for (; *s; s++) {
        size_t i = 0;
        while (i < len && s[i] && tolower((unsigned char)s[i]) == tolower((unsigned char)part[i])) i++;
        if (i == len) return 1;
    }
    return 0;
}

// ---- the route (test runs: NX2NAVI_DBG_TEST "route" and "routeto"): Link runs through points of the area on his
// own, so a walking test crosses the field instead of pressing into its walls. The left stick is turned from the way
// he moves towards the next point (the game's camera follows behind him, so a stick to the side turns him), with an
// integral that takes out the camera's own angle; a point counts as reached within ROUTE_NEAR metres. When he gains
// less than a metre on it in 2.5 seconds he is stuck (a wall, a fence, a steep slope): he sidesteps for two seconds,
// three times to one side and then to the other, and after ROUTE_STUCK such tries the route ends. A route asked for
// with a smaller distance (dbg_route_to, a replay of a bug capture) walks the last three metres, so that Link stops on
// the point instead of running rings round it, and is steered by the game's own camera (dbg_camera_seen): the stick
// moves Link along the camera's view and across it, so the stick's angle is the angle between the view and the way to
// the point, with nothing to estimate. In a room that matters: the estimate starts with a run straight ahead, which
// in Link's house put him on the furniture beside the door, three metres from the point, for good.
#define ROUTE_MAX 16
#define ROUTE_NEAR 4.0
#define ROUTE_STUCK 10
static struct {
    volatile int on, gave_up;
    int by_camera;            // steered by the game's camera, which is watched meanwhile (dbg_route_to)
    uint64_t cam_n, cam_ns;   // the camera updates seen at the last look, and when their number last grew
    float within;             // metres: a point counts as reached within them
    int n, at, have_last, have_heading, side, stuck;
    float pt[ROUTE_MAX][2];   // x, z in metres
    char name[ROUTE_MAX][48];
    float last[3], best;
    double heading, integ;    // radians: the way he moved last; the integral of the turn
    uint64_t prev_ns, prog_ns, side_until;
} route;

static void route_end(void) {
    route.on = 0;
    if (route.by_camera) dbg_camera_watch(0);
    route.by_camera = 0;
}

static void route_start(float within) {
    route.within = within, route.gave_up = 0;
    route.at = 0, route.have_last = route.have_heading = 0, route.stuck = 0, route.side = 1, route.integ = 0;
    route.best = 1e9f, route.prog_ns = plat_now_ns(), route.side_until = 0, route.prev_ns = 0;
    if (route.by_camera) dbg_camera_watch(0);   // of a walk that this route replaces
    route.by_camera = 0;
    route.on = route.n > 0;
    if (route.on) rt_log("debug: route of %d points, first %s at %.1f %.1f", route.n, route.name[0], route.pt[0][0], route.pt[0][1]);
}

// The game's thread: the left stick while a route runs. Stick right turns Link right, which lowers atan2(x, z) (the
// free camera's own frame: ahead (sin yaw, cos yaw), right (-cos yaw, sin yaw)).
static void route_pad(int32_t *lx, int32_t *ly) {
    float p[3];
    if (!route.on || !dbg_player_pos(p)) return;
    uint64_t now = plat_now_ns();
    double dt = route.prev_ns ? (double)(now - route.prev_ns) / 1e9 : 0;
    route.prev_ns = now;
    if (dt > 0.1) dt = 0.1;
    double dx = route.pt[route.at][0] - p[0], dz = route.pt[route.at][1] - p[2], d = sqrt(dx * dx + dz * dz);
    if (d < route.within) {
        rt_log("debug: route: reached %s (%d of %d) at %.1f %.1f %.1f", route.name[route.at], route.at + 1, route.n, p[0], p[1], p[2]);
        if (++route.at == route.n) {
            route_end();
            rt_log("debug: route done");
            return;
        }
        route.best = 1e9f, route.prog_ns = now, route.integ = 0;
        dx = route.pt[route.at][0] - p[0], dz = route.pt[route.at][1] - p[2], d = sqrt(dx * dx + dz * dz);
    }
    if (route.have_last) {
        double mx = p[0] - route.last[0], mz = p[2] - route.last[2];
        if (mx * mx + mz * mz > 0.0025) route.heading = atan2(mx, mz), route.have_heading = 1;
    }
    memcpy(route.last, p, sizeof route.last), route.have_last = 1;
    double err = route.have_heading ? atan2(dx, dz) - route.heading : 0;
    while (err > M_PI) err -= 2 * M_PI;
    while (err < -M_PI) err += 2 * M_PI;
    route.integ += err * dt * 0.4;
    route.integ = route.integ > 1.0 ? 1.0 : route.integ < -1.0 ? -1.0 : route.integ;
    double a = -(1.2 * err + route.integ);   // the stick's angle from straight ahead, right positive
    if (route.by_camera) {
        double cp[3], ct[3];
        float up[3];
        uint64_t seen = dbg_camera_seen(0, cp, ct, up);
        if (seen != route.cam_n) route.cam_n = seen, route.cam_ns = now;
        double fx = ct[0] - cp[0], fz = ct[2] - cp[2], fl = sqrt(fx * fx + fz * fz);
        // the view, level: ahead (fx, fz), right (-fz, fx), as the free camera's frame below has them
        if (route.cam_ns && now - route.cam_ns < 250000000ull && fl > 1e-6 && d > 1e-6)
            a = atan2((-dx * fz + dz * fx) / fl, (dx * fx + dz * fz) / fl);
    }
    a = a > 1.4 ? 1.4 : a < -1.4 ? -1.4 : a;
    if (d < route.best - (route.within < ROUTE_NEAR ? 0.25f : 1.0f)) route.best = (float)d, route.prog_ns = now;
    else if (now - route.prog_ns > 2500000000ull && now >= route.side_until) {
        if (++route.stuck > ROUTE_STUCK) {
            route_end(), route.gave_up = 1;
            rt_log("debug: route given up before %s: stuck %d times at %.1f %.1f %.1f", route.name[route.at], ROUTE_STUCK, p[0], p[1], p[2]);
            return;
        }
        // the same side three times before the other: a wall is gone round, where turns to alternate sides kept him
        // pressed against a cliff face south of the Kakariko Village gate
        if (route.stuck % 3 == 1) route.side = -route.side;
        route.side_until = now + 2000000000ull, route.prog_ns = now + 2000000000ull, route.best = (float)d;
        rt_log("debug: route: stuck at %.1f %.1f %.1f, %.1f m before %s: a sidestep to the %s", p[0], p[1], p[2], d,
               route.name[route.at], route.side > 0 ? "right" : "left");
    }
    if (now < route.side_until) a = route.side * 1.5;
    double tilt = route.within < ROUTE_NEAR && d < 3.0 && now >= route.side_until ? (d / 3.0 < 0.4 ? 0.4 : d / 3.0) : 1.0;
    *lx = (int32_t)(sin(a) * 30000 * tilt), *ly = (int32_t)(cos(a) * 30000 * tilt);
}

// The game's thread: Link runs to one point of the area, reached within near_m metres. dbg_route_state: 1 on his way,
// 0 there (or no route), 2 given up (stuck).
void dbg_route_to(float x, float z, float near_m) {
    route.n = 1;
    route.pt[0][0] = x, route.pt[0][1] = z;
    snprintf(route.name[0], sizeof route.name[0], "the captured place");
    route_start(near_m > 0.3f && near_m < ROUTE_NEAR ? near_m : (float)ROUTE_NEAR);
    route.by_camera = 1, route.cam_n = 0, route.cam_ns = 0;
    dbg_camera_watch(1);
}
int dbg_route_state(void) { return route.on ? 1 : route.gave_up ? 2 : 0; }
void dbg_route_stop(void) { route_end(); }

// The game's thread, with the pad's state before the game gets it: while the free camera has the sticks, the left one
// moves it where it looks and sideways, the right one turns it, ZR and ZL raise and lower it, R makes it four times as
// fast and L four times as slow, and the game is told that none of those is touched, so Link stays where he is.
// Nothing stops it: it goes through walls, objects and the ground, below which the picture shows the void under the
// map. Ground avoidance (the menu's switch, off unless asked for) keeps it a metre above ground-like surfaces instead.
void dbg_pad(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    bugcap_replay_pad(buttons);   // a replay of a bug capture presses A until the save is loaded
    route_pad(lx, ly);
    if (!fc.on || !fc.sticks || fc.capture) {
        fc.last_ns = 0;
        return;
    }
    uint64_t now = plat_now_ns();
    double dt = fc.last_ns ? (double)(now - fc.last_ns) / 1e9 : 0;
    fc.last_ns = now;
    if (dt > 0.1) dt = 0.1;
    double in[4] = {*lx / 32767.0, *ly / 32767.0, *rx / 32767.0, *ry / 32767.0};
    for (int i = 0; i < 4; i++) in[i] = fabs(in[i]) < 0.15 ? 0 : in[i];
    const uint64_t zl = 1ull << B_ZL, zr = 1ull << B_ZR, l = 1ull << B_L, r = 1ull << B_R;
    double rise = (*buttons & zr ? 1 : 0) - (*buttons & zl ? 1 : 0);
    double step = fc.speed * (*buttons & r ? 4 : 1) * (*buttons & l ? 0.25 : 1) * dt;
    fc.yaw -= (float)(in[2] * 2.4 * dt);
    fc.pitch += (float)(in[3] * 1.8 * dt);
    fc.pitch = fc.pitch > 1.5f ? 1.5f : fc.pitch < -1.5f ? -1.5f : fc.pitch;
    double cp = cos(fc.pitch), look[3] = {cp * sin(fc.yaw), sin(fc.pitch), cp * cos(fc.yaw)};   // where it looks
    double right[3] = {-cos(fc.yaw), 0, sin(fc.yaw)};   // the view's direction crossed with up
    double move[3];
    for (int i = 0; i < 3; i++) move[i] = (look[i] * in[1] + right[i] * in[0]) * step;
    move[1] += rise * step;
    // Ground avoidance, when it is on: where the ground under the view (ground_under, from vk_dlaa.c fcam_probe with
    // DLSS, DLAA or FSR on) is less than a metre below, the camera goes no lower and rises to that metre, instead of
    // going on into a field or a gentle hill and under the map. Objects and walls do not count. The reach ahead grows
    // with the speed: the distances are a few frames old.
    uint64_t seen = __atomic_load_n(&fc.ground_ns, __ATOMIC_ACQUIRE);
    if (fc.avoid && seen && now - seen < 300000000ull && dt > 0) {
        const double clear = 1.0;
        double below = ground_under(3.0 + 0.3 * step / dt);
        if (below >= 0 && below < clear) {
            if (move[1] < 0) move[1] = 0;
            move[1] += (clear - below) * (dt * 8 < 1 ? dt * 8 : 1);
        }
    }
    for (int i = 0; i < 3; i++) fc.pos[i] += move[i];
    *lx = *ly = *rx = *ry = 0;
    *buttons &= ~(zl | zr | l | r);
}

// ---- NX2NAVI_DBG_TEST="<tick>:flag <hash hex> <0|1>;<tick>:speed <factor>;<tick>:find <value> <size>;
// <tick>:next <mode> <value>;<tick>:setfirst <value>;<tick>:holdfirst <value>;<tick>:show [<hash hex>];
// <tick>:warp <area> [<start position>];<tick>:event <name>;<tick>:text <game data text's name>;
// <tick>:freecam <0|1> <sticks 0|1> <speed>;<tick>:camavoid <0|1>;<tick>:camgo <dx> <dy> <dz> [<yaw> <pitch>];
// <tick>:camreset;<tick>:campos":
// the menu's requests without the menu, for test runs. Ticks count the game's pad reads from the player's first appearance.
static void test_script(uint64_t tick) {
    static char buf[2048];
    static const char *next;
    static int state;
    if (!state) {
        const char *e = cfg_get("NX2NAVI_DBG_TEST");
        state = e && *e ? 1 : -1;
        if (state > 0) snprintf(buf, sizeof buf, "%s", e), next = buf;
        // the places and events, read with the flag names as the menu starts them, for warp, event and routeto
        if (state > 0) dbg_flags_refresh(), dbg_names(NULL);
    }
    while (state > 0 && next && *next && strtoull(next, NULL, 10) <= tick) {
        const char *colon = strchr(next, ':'), *end = strchr(next, ';');
        if (!colon) break;
        unsigned hash = 0, on = 0, size = 4;
        unsigned long long value = 0;
        float f = 0;
        if (sscanf(colon + 1, "flag %x %u", &hash, &on) == 2) {
            dbg_flag_set(hash, (int)on);
        } else if (sscanf(colon + 1, "speed %f", &f) == 1) {
            dbg_speed_set(f);
        } else if (!strncmp(colon + 1, "campos", 6)) {
            rt_log("debug: tick %llu: free camera %s at %.3f %.3f %.3f, yaw %.3f, pitch %.3f; %llu camera updates through it",
                   (unsigned long long)tick, fc.on ? "on" : "off", fc.pos[0], fc.pos[1], fc.pos[2], fc.yaw, fc.pitch,
                   (unsigned long long)fc.updates);
        } else if (!strncmp(colon + 1, "camavoid ", 9)) {   // camavoid <0|1>: the free camera's ground avoidance
            dbg_freecam_avoid_set(atoi(colon + 10));
        } else if (!strncmp(colon + 1, "freecam ", 8)) {   // freecam <on> <sticks> <speed>
            int cam_on = 0, sticks = 1;
            float speed = 8;
            if (sscanf(colon + 9, "%d %d %f", &cam_on, &sticks, &speed) >= 1) dbg_freecam_set(cam_on, sticks, speed);
        } else if (!strncmp(colon + 1, "camreset", 8)) {
            dbg_freecam_reset();
        } else if (!strncmp(colon + 1, "camgo ", 6)) {      // camgo <dx> <dy> <dz> <yaw> <pitch>: the free camera moved by hand
            double d[3] = {0, 0, 0};
            float yaw = 0, pitch = 0;
            if (sscanf(colon + 7, "%lf %lf %lf %f %f", &d[0], &d[1], &d[2], &yaw, &pitch) >= 3) {
                for (int i = 0; i < 3; i++) fc.pos[i] += d[i];
                fc.yaw += yaw, fc.pitch += pitch;
            }
        } else if (!strncmp(colon + 1, "route ", 6)) {   // route <x> <z> [<x> <z> ...]: points in metres
            const char *q = colon + 7;
            double x, z;
            int used = 0;
            route.n = 0;
            while (route.n < ROUTE_MAX && sscanf(q, "%lf %lf%n", &x, &z, &used) == 2 && (!end || q + used <= end)) {
                route.pt[route.n][0] = (float)x, route.pt[route.n][1] = (float)z;
                snprintf(route.name[route.n], sizeof route.name[0], "point %d", route.n + 1);
                route.n++, q += used;
            }
            route_start((float)ROUTE_NEAR);
        } else if (!strncmp(colon + 1, "routeto ", 8)) {
            // routeto <part of a start position's name>[/<another part>] ...: the area's own start positions, in order
            char area[64] = "", item[128];
            text_get("Sequence_CurrentBanc", area);
            const char *q = colon + 9;
            int used = 0;
            route.n = 0;
            while (route.n < ROUTE_MAX && sscanf(q, "%127[^ ;]%n", item, &used) == 1 && (!end || q + used <= end)) {
                q += used;
                while (*q == ' ') q++;
                int found = -1;
                for (int i = 0; found < 0 && i < dbg_places(); i++) {
                    if (strcmp(places[i].area, area) || !places[i].has_xyz) continue;
                    for (const char *tok = item; *tok && found < 0;) {   // the parts between the slashes
                        size_t len = strcspn(tok, "/");
                        if (len && contains_nocase(places[i].pos, tok, len)) found = i;
                        tok += len + (tok[len] == '/');
                    }
                }
                if (found < 0) {
                    rt_log("debug: routeto: no start position of %s with coordinates matches \"%s\"", area, item);
                    continue;
                }
                route.pt[route.n][0] = places[found].xyz[0], route.pt[route.n][1] = places[found].xyz[2];
                snprintf(route.name[route.n], sizeof route.name[0], "%s", places[found].pos);
                route.n++;
            }
            route_start((float)ROUTE_NEAR);
        } else if (!strncmp(colon + 1, "warp ", 5)) {
            char area[64] = "", pos[64] = "";
            if (sscanf(colon + 6, "%63[^ ;] %63[^ ;]", area, pos) >= 1) dbg_warp(area, pos);
        } else if (!strncmp(colon + 1, "event ", 6)) {
            char name[80] = "";
            if (sscanf(colon + 7, "%79[^ ;]", name) == 1) dbg_event_start(name);
        } else if (!strncmp(colon + 1, "text ", 5)) {
            char name[80] = "", now[64];
            float p[3] = {0, 0, 0};
            sscanf(colon + 6, "%79[^ ;]", name);
            int got = text_get(name, now);
            dbg_player_pos(p);
            rt_log("debug: tick %llu: %s = \"%s\"%s; player %.1f %.1f %.1f", (unsigned long long)tick, name, now,
                   got ? "" : " (not found)", p[0], p[1], p[2]);
        } else if (sscanf(colon + 1, "setfirst %llu", &value) == 1) {
            dbg_mem_write(dbg_search_addr(0), value, dbg_search_size());
        } else if (sscanf(colon + 1, "holdfirst %llu", &value) == 1) {
            dbg_freeze(dbg_search_addr(0), value, dbg_search_size(), value != 0);
        } else if (sscanf(colon + 1, "next %u %llu", &on, &value) == 2) {
            int n = dbg_search_next((int)on, (uint32_t)value);
            uint32_t now = 0;
            dbg_mem_read(dbg_search_addr(0), dbg_search_size(), &now);
            rt_log("debug: tick %llu: %d places left (mode %u, value %llu); the first holds %u; %d held", (unsigned long long)tick, n,
                   on, value, now, dbg_frozen_count());
        } else if (sscanf(colon + 1, "find %llu %u", &value, &size) >= 1) {
            uint64_t t0 = plat_now_ns();
            int n = dbg_search_new((uint32_t)value, (int)size), over = 0;
            dbg_search_count(&over);
            rt_log("debug: tick %llu: %d places hold %llu as %u bytes%s, searched in %.0f ms; first %#llx", (unsigned long long)tick,
                   n, value, size, over ? " (and more)" : "", (double)(plat_now_ns() - t0) / 1e6,
                   (unsigned long long)dbg_search_addr(0));
        } else if (!strncmp(colon + 1, "show", 4)) {
            float p[3] = {0, 0, 0}, sp = dbg_speed_factor();
            int n = dbg_flags_refresh(), set = 0, found = 0;
            for (int i = 0; i < n; i++) set += flag_value[i];
            int names = dbg_names(&found);
            dbg_player_pos(p);
            rt_log("debug: tick %llu: %d flags, %d on; names state %d, %d found; player %.2f %.2f %.2f; speed %.3f; done: %llu flag "
                   "changes, %llu writes, %llu refused", (unsigned long long)tick, n, set, names, found, p[0], p[1], p[2], sp,
                   (unsigned long long)stat_flags, (unsigned long long)stat_writes, (unsigned long long)stat_refused);
            if (sscanf(colon + 1, "show %x", &hash) == 1)
                for (int i = 0; i < n; i++)
                    if (flag_pairs[i].hash == hash)
                        rt_log("debug:   flag %08x (%s) is %d", hash, names == 2 && flag_name[i] ? flag_name[i] : "no name", flag_value[i]);
        }
        next = end ? end + 1 : NULL;
    }
}

// The game's thread, at its pad read.
void dbg_tick(Cpu *c) {
    static uint64_t tick;
    if (!dbg_supported() || (!tick && !player())) return;
    tick++;
    skull_pending_refresh();
    test_script(tick);
    bugcap_replay_tick(tick);   // a replay of a bug capture: the teleport, the walk and the camera (bugcap.c)
    warp_tick(c, tick);
    static Req todo[Q_MAX];
    int n;
    plat_mutex_lock(&q_lock);
    n = q_n, q_n = 0;
    memcpy(todo, q, sizeof(Req) * (size_t)n);
    for (int i = 0; i < frozen_n && n < Q_MAX; i++) todo[n++] = (Req){Q_WRITE, 0, frozen[i].addr, frozen[i].value, frozen[i].size};
    plat_mutex_unlock(&q_lock);
    uint64_t m = gmd();
    int tokens = 0;   // the token count follows the collection flags (skull_tokens_sync)
    for (int i = 0; i < n; i++) {
        if (todo[i].kind == Q_TOKENS) {
            tokens = 1;
        } else if (todo[i].kind == Q_FLAG) {
            if (!m) continue;
            rt_call(c, MOD_MAIN_BASE + 0x682b20, 3, m, todo[i].value, (uint64_t)todo[i].hash);
            stat_flags++;
            rt_log("debug: flag %08x set to %s", todo[i].hash, todo[i].value ? "on" : "off");   // what a tester changed, for the log
        } else if (todo[i].kind == Q_SKULL) {
            int index = skull_index(m, todo[i].addr);
            uint32_t capacity = 0, pending = 0;
            if (index < 0 || !rd(m + 0xc68, &capacity, 4) || !rd(m + 0xc78, &pending, 4) ||
                !capacity || (pending & 0xfffff) >= capacity) {
                stat_refused++;
                rt_log("debug: Skulltula change refused: flag or queue unavailable");
                continue;
            }
            // main+0xf071c0 enqueues {Boolean, record index}; main+0xba24b0 applies it and updates save bookkeeping.
            rt_call(c, MOD_MAIN_BASE + 0xf071c0, 4, m + 0xc58, todo[i].value, todo[i].addr, (uint64_t)index);
            stat_flags++;
            if (todo[i].hash < SKULL_COUNT) skull_want[todo[i].hash] = (int8_t)(todo[i].value != 0);
            tokens = 1;
            rt_log("debug: Skulltula collection flag changed to %s", todo[i].value ? "collected" : "uncollected");
        } else if (todo[i].size == 1 || todo[i].size == 2 || todo[i].size == 4) {
            if (wr(todo[i].addr, &todo[i].value, (size_t)todo[i].size)) stat_writes++;
            else stat_refused++;
        }
    }
    if (tokens && m) skull_tokens_sync(c, m);
}
