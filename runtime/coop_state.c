// Co-op progress sync: the merge engine (coop_state.h). Pure: no game, no sockets, no threads, no clock of its own.
//
// Flags live in one array that only grows (an index stays valid) with an open-addressing index over their hashes; pool
// contributors are signed records kept with their pool. A register's section and its share of the section's sum
// depend only on its key and value, so the sums are kept up to date at every change and recomputed when the rules or
// the charter change what counts.
//
// Wire format (little-endian), after the peer message's kind byte (coop_proto.h):
//   DIGEST   u8 version (4), u32 schema, identity, u8 has charter, [charter], u8 nsec, nsec x u32 sum
//   identity u8[32] key, u64 the session before (0 none), u8 name length, name, u8[64] signature
//   charter  u32 epoch, u8 ncat, ncat x u8 mode, u32 pools' epoch, u8 nadmit, nadmit x u64 key id, u8 nremoved,
//            nremoved x u64 key id, u8[32] maker's key, u8[64] signature
//   SECTION  u8 version, u32 schema, u8 n, n x entry, u64 counter, u8[64] signature
//   flag     u8 1, u8 category, u32 hash, u32 value
//   record   u8 5, u8 category, u32 hash, u8[32] key, u32 pools' epoch, u32 seq, u32 base (top bit flipped), u32 gain,
//            u32 spend, u8[64] signature
// Signed messages begin with "nx2navi-coop/4 member", "nx2navi-coop/4 section", "nx2navi-coop/3 rules" or
// "nx2navi-coop/3 pool" and the room's 16-byte context (BLAKE2b keyed with the room's secret), so a signature means
// nothing in another room or in another role:
//   member   context, u8 member id, u64 session, u64 the session before, key, u8 name length, name
//   section  context, u8 sender's member id, u64 its session, the SECTION's bytes up to its signature (its counter last)
//   rules    context, the charter's fields up to the maker's key, the maker's key
//   pool     context, u8 category, u32 hash, u32 pools' epoch, u32 seq, u32 base, u32 gain, u32 spend
// A key id is the first 8 bytes of BLAKE2b-256 of the key, little-endian.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop_proto.h"
#include "coop_state.h"

enum {
    MAX_REGS = 1 << 16,          // a hostile peer cannot grow the state past this; the game has about 12000 flags
    MAX_POOLS = 32,              // the game has 22 counters; an entry for a counter it does not have is refused
    MAX_ADMIT = 24,              // identities a charter admits to the pools, and so records per pool
    MAX_REMOVED = 16,            // identities a charter carries as removed (each game keeps every removal it saw)
    MAX_ACCEPTED = 64,           // identities this player accepted in the room
    ENDED_MAX = 1 << 16,         // (key, session) pairs that ended in this room: an identity of one is never taken again
    KEPT_PER_KEY = 1024,         // and the latest pairs of each key this player accepted, which no other key's churn removes
    POOL_JOINS = 8,              // joins of a pool in one pools' epoch whose claims are made again; older ones stay as they came to
    SIG_RATE = 128,              // signature checks per second on behalf of one peer, and at most SIG_BURST at once
    SIG_BURST = 256,
    POOL_REG_MAX = 1 << 20,      // a record's gains or spends at most, and its base either way
    PEER_RATE = 49152,           // message bytes per second taken from one peer (it sends at most 32 KB/s)
    PEER_BURST = 65536,
    BAD_MAX = 32,                // flags a member sent that this game refuses: past this many it is ignored
    PUSH_SLOTS = 4096,           // local changes waiting for their three sends
    DIGEST_MS = 2000,            // every digest_ms plus up to 400 ms
    ANSWER_MS = 500,             // a section goes to the same peer at most this often
    RATE_BYTES = 32768,          // section and push bytes per second, under the relay's 64 KB/s per member
    BURST_BYTES = 8192,
    IDENT_WAIT_MS = 5000,        // a member that appeared this recently and is not identified holds the offer back
    IDENT_HOLD_MS = 10000,       // another key's identity for a member's session takes its place only after this long
                                 // without the one held (and the two are disputed while both are heard within it)
    CATCHUP_MS = 20000,          // after this long in the room, the state of one member is enough to join the pools
    ORIGINATE_MS = 1000,         // the maker signs a new charter at most this often (a removal at once)
    MSG_CAP = COOP_MAX_MESSAGE,
    SYNC_HDR = 6,
    SEC_TAIL = 8 + COOP_SIG_LEN, // a section's counter and signature
    SEC_WINDOW = 64,             // a section's counter is taken once, and at most this far below the highest taken
    SEC_PRE = 22 + 16 + 1 + 8,   // what a section's signature covers before its bytes: role, context, sender, session
    FLAG_SIZE = 10,
    REC_SIZE = 2 + 4 + COOP_PK_LEN + 4 * 5 + COOP_SIG_LEN,
    NAME_CAP = COOP_NAME_MAX + 1,
    CTX_LEN = 16,
};

static const char *const cat_names[COOP_CAT_N] = {"progress", "items", "songs", "quest", "keys", "rupees", "ammo"};
static const char SIG_MEMBER[] = "nx2navi-coop/4 member", SIG_SECTION[] = "nx2navi-coop/4 section", SIG_RULES[] = "nx2navi-coop/3 rules",
                  SIG_POOL[] = "nx2navi-coop/3 pool";

typedef struct {
    uint32_t hash, s, l, applied;   // s merged, l local, applied: the value last handed to the game
    uint8_t cat, queued;
} Reg;

typedef struct {
    uint64_t kid;
    uint8_t pk[COOP_PK_LEN], sig[COOP_SIG_LEN];
    uint32_t epoch, seq, gain, spend;
    int32_t base;
} Rec;

typedef struct { uint64_t kid; int32_t base; uint32_t gain, spend; } Seen;   // a record's counts as a join saw them

// A join of a pool by this game (in memory only): what it joined with and every record of the pool just before, so that
// its claim can be made again when the records this game counts change.
typedef struct {
    int fresh;                      // the join made this game's record of the epoch
    int64_t want;                   // the game's count (a reset's carry is the joins' floor instead: Joins.floor)
    Seen seen[MAX_ADMIT];
    int nseen;
} Join;

// Every join of a pool in a pools' epoch (another save loaded joins again): this game's base is the highest of their
// claims, each made as if the game had counted then what it counts now.
typedef struct {
    uint32_t epoch;
    int n;
    Join j[POOL_JOINS];
    int32_t floor;                  // the claims of joins no longer kept, at the most they came to
    uint32_t debt;                  // what the first join added as a gain (the pool was in debt), as the record holds it
} Joins;

typedef struct {
    uint32_t hash;
    uint8_t cat, bound, queued, have_local;
    int32_t local, expected, cap, max;   // max: the highest value the game takes for this counter
    Rec rec[MAX_ADMIT];
    int nrec;
    int has_carry;                  // a pool reset: what the pool held for the identities still admitted (carry)
    int64_t carry;
    Joins joins;
    int recheck;                    // the records this game counts changed: its claim is made again (pool_recheck)
} Pool;

typedef struct {
    uint32_t epoch, pool_epoch;
    uint8_t mode[COOP_CAT_N];
    uint8_t nadmit, nremoved;
    uint64_t admit[MAX_ADMIT], removed[MAX_REMOVED];
    uint8_t pk[COOP_PK_LEN], sig[COOP_SIG_LEN];
    uint64_t kid;
} Charter;

typedef struct {
    int present, have_digest;
    uint64_t session, seen_at;
    int ident;                      // its identity was checked for this session
    uint64_t ident_at;              // when its identity was last presented
    uint64_t rival_at;              // when another key's identity for its session last checked out (a dispute), 0 none
    uint8_t pk[COOP_PK_LEN];
    uint64_t kid;
    char name[NAME_CAP];            // the name its identity signed, cleaned for showing
    char raw[NAME_CAP];             // and as it was signed
    uint64_t ch_kid;                // the signer of the charter its last digest carried, 0 none
    int ch_seen;                    // a digest of its identity was read (ch_kid tells its charter)
    uint32_t sums[COOP_SYNC_SECTIONS];
    uint64_t want;                  // sections to send it, a bit each
    uint64_t answered_at[COOP_SYNC_SECTIONS];
    uint64_t digest_at;             // the last digest sent to it alone
    int64_t tokens;                 // its message bytes' budget (PEER_RATE)
    uint64_t tok_at;
    int64_t sig_tokens;             // its budget of signature checks (SIG_RATE)
    uint64_t sig_at;
    uint64_t bad_sig;               // the last signature that failed its check (a tag of it), not checked again
    unsigned bad;                   // flags it sent that this game refuses, and signatures that failed
    int ignored;                    // past BAD_MAX of them: nothing more is taken from it while it is in the room
    int sec_valid;                  // the counters of its sections taken in this session: the highest and the 64 below it
    uint64_t sec_high, sec_bits;
} Peer;

typedef struct { uint64_t kid, session; } Ended;
typedef struct { uint64_t kid; uint64_t *sess; unsigned n, cap; } Kept;   // a ring of up to KEPT_PER_KEY sessions

// idx >= 0: a flag register; else a record, -1 - (pool * MAX_ADMIT + record)
typedef struct { int idx; uint8_t round; uint64_t due, first; } Push;

struct CoopSync {
    uint64_t self_id, now;
    uint32_t schema;
    CoopSyncLimitFn limit;          // the local game's table: what it shares and up to which value
    void *limit_user;
    uint64_t ik0, ik1;              // the index's key
    Reg *regs;
    int nregs, cap;
    int *index;                     // reg index + 1, 0 empty
    int index_cap;
    uint32_t sums[COOP_SYNC_SECTIONS];
    Pool pools[MAX_POOLS];
    int npools;

    // identity: one key per room and start, from seed and the room's secret, or from the room's own seed (the maker's,
    // kept in its profile: coop_sync_room_seed)
    uint8_t seed[32], sk[COOP_SK_LEN], pk[COOP_PK_LEN], ctx[CTX_LEN], id_sig[COOP_SIG_LEN];
    uint8_t room_seed[32];
    char room_seed_code[COOP_SECRET_TEXT + 1];
    uint64_t kid, offer_k0, offer_k1;
    char name[NAME_CAP];
    int id_valid;                   // id_sig is of self, self_session, prev_session and name
    uint64_t self_session, prev_session;   // prev: this game's session in the room before this one, 0 none
    uint64_t sec_counter;           // the counter of its last section

    char code[COOP_SECRET_TEXT + 1];
    int in_room, self, caught_up;
    uint64_t joined_at;
    Peer peer[COOP_MAX_MEMBERS + 1];
    Ended *ended;                   // kept while the room's code stays (a ring past ENDED_MAX): keys not accepted
    unsigned nended, ended_cap;
    Kept kept[MAX_ACCEPTED];        // the ended sessions of the keys this player accepted, each key apart
    int nkept;

    // the charter in force (and the rules it makes), kept while the room's code stays
    Charter ch;
    CoopRules room, mine;
    int have_room, have_mine, made;
    int maker;                      // the member id the relay names as the room's maker, 0 none in the room
    uint64_t originated_at;
    unsigned rules_ver;
    int want_reset;                 // the maker asked for a pool reset
    uint64_t want_remove;           // the key id the maker removes, 0 none
    uint64_t *gone;                 // every key a charter of this room removed, kept while the room's code stays
    int ngone, gone_cap;

    // acceptance, kept while the room's code stays
    char acc_code[COOP_SECRET_TEXT + 1];
    int accepted, have_pin;
    uint64_t acc_kids[MAX_ACCEPTED];
    int nacc;
    char acc_name[MAX_ACCEPTED][NAME_CAP];   // the name each key signed when this player accepted it (as signed)
    uint8_t acc_id[MAX_ACCEPTED];            // the member number each key was last identified as
    uint8_t acc_mode[COOP_CAT_N];
    uint64_t pin_kid;
    uint32_t save_gen, acc_save_gen, copy_gen;
    int bound_once, copy, copy_wanted;

    Push push[PUSH_SLOTS];
    unsigned push_head, push_tail;
    int *apply;                     // ring of reg indices (>= 0) and pools (-1 - i)
    unsigned apply_head, apply_tail, apply_cap;
    uint64_t next_digest;
    uint64_t digest_to;             // members (bit per id) to send a digest now; bit 0: all
    int turn;                       // the member whose section goes first at the next tick
    int64_t tokens;
    uint64_t tok_at;
    uint64_t rng;

    void (*trace)(void *user, const char *line);
    void *trace_user;
    CoopSyncStats st;
};

// ---- small helpers

static uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
static uint64_t key_of(uint8_t kind, uint32_t hash, uint64_t who) { return mix64(((uint64_t)kind << 56) ^ hash ^ mix64(who)); }

// SipHash-2-4 (Aumasson and Bernstein): a keyed hash for the index and the offer's number.
#define SIP_ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define SIP_ROUND                                                                                                      \
    do {                                                                                                               \
        v0 += v1, v1 = SIP_ROTL(v1, 13), v1 ^= v0, v0 = SIP_ROTL(v0, 32), v2 += v3, v3 = SIP_ROTL(v3, 16), v3 ^= v2;  \
        v0 += v3, v3 = SIP_ROTL(v3, 21), v3 ^= v0, v2 += v1, v1 = SIP_ROTL(v1, 17), v1 ^= v2, v2 = SIP_ROTL(v2, 32);  \
    } while (0)
uint64_t coop_siphash24(uint64_t k0, uint64_t k1, const uint8_t *m, size_t n) {
    uint64_t v0 = 0x736f6d6570736575ull ^ k0, v1 = 0x646f72616e646f6dull ^ k1;
    uint64_t v2 = 0x6c7967656e657261ull ^ k0, v3 = 0x7465646279746573ull ^ k1;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w = 0;
        for (int k = 0; k < 8; k++) w |= (uint64_t)m[i + k] << 8 * k;
        v3 ^= w;
        SIP_ROUND;
        SIP_ROUND;
        v0 ^= w;
    }
    uint64_t b = (uint64_t)n << 56;
    for (size_t k = 0; i + k < n; k++) b |= (uint64_t)m[i + k] << 8 * k;
    v3 ^= b;
    SIP_ROUND;
    SIP_ROUND;
    v0 ^= b;
    v2 ^= 0xff;
    for (int r = 0; r < 4; r++) SIP_ROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}

static void put8(uint8_t **p, uint8_t v) { *(*p)++ = v; }
static void put32(uint8_t **p, uint32_t v) { for (int i = 0; i < 4; i++) *(*p)++ = (uint8_t)(v >> 8 * i); }
static void put64(uint8_t **p, uint64_t v) { for (int i = 0; i < 8; i++) *(*p)++ = (uint8_t)(v >> 8 * i); }
static void putn(uint8_t **p, const void *b, size_t n) { memcpy(*p, b, n), *p += n; }
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

static void tracef(CoopSync *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void tracef(CoopSync *s, const char *fmt, ...) {
    if (!s->trace) return;
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    s->trace(s->trace_user, line);
}

static int32_t base_decode(uint32_t v) { return (int32_t)(v ^ 0x80000000u); }
static uint32_t base_encode(int32_t v) { return (uint32_t)v ^ 0x80000000u; }
static int64_t clamp_reg(int64_t v) { return v < -POOL_REG_MAX ? -POOL_REG_MAX : v > POOL_REG_MAX ? POOL_REG_MAX : v; }

static uint64_t kid_of(const uint8_t pk[COOP_PK_LEN]) {
    uint8_t h[32];
    coop_blake2b(h, sizeof h, NULL, 0, pk, COOP_PK_LEN);
    return get64(h);
}

static void fingerprint(uint64_t kid, char out[20]) {
    snprintf(out, 20, "%04x-%04x-%04x-%04x", (unsigned)(kid >> 48 & 0xFFFF), (unsigned)(kid >> 32 & 0xFFFF), (unsigned)(kid >> 16 & 0xFFFF),
             (unsigned)(kid & 0xFFFF));
}

static int in_list(const uint64_t *list, int n, uint64_t v) {
    for (int i = 0; i < n; i++)
        if (list[i] == v) return 1;
    return 0;
}

// ---- rules

const char *coop_cat_name(int cat) { return cat >= 0 && cat < COOP_CAT_N ? cat_names[cat] : "?"; }
const char *coop_mode_name(int mode) { return mode == COOP_MODE_SHARED ? "shared" : mode == COOP_MODE_POOL ? "pool" : "own"; }
int coop_cat_is_pool(int cat) { return cat == COOP_CAT_KEYS || cat == COOP_CAT_RUPEES || cat == COOP_CAT_AMMO; }

void coop_rules_default(CoopRules *r) {
    memset(r, 0, sizeof *r);
    for (int c = 0; c < COOP_CAT_N; c++) r->mode[c] = coop_cat_is_pool(c) ? COOP_MODE_OWN : COOP_MODE_SHARED;
    r->mode[COOP_CAT_KEYS] = COOP_MODE_POOL;
}

unsigned coop_rules_normalize(CoopRules *r) {
    CoopRules d;
    coop_rules_default(&d);
    unsigned changed = 0;
    for (int c = 0; c < COOP_CAT_N; c++) {
        int m = r->mode[c];
        int ok = coop_cat_is_pool(c) ? m == COOP_MODE_OWN || m == COOP_MODE_POOL : m == COOP_MODE_OWN || m == COOP_MODE_SHARED;
        if (!ok) r->mode[c] = d.mode[c], changed |= 1u << c;
    }
    if (r->mode[COOP_CAT_PROGRESS] == COOP_MODE_OWN) {
        if (r->mode[COOP_CAT_KEYS] != COOP_MODE_OWN) r->mode[COOP_CAT_KEYS] = COOP_MODE_OWN, changed |= 1u << COOP_CAT_KEYS;
    } else {
        static const int pickups[] = {COOP_CAT_ITEMS, COOP_CAT_SONGS, COOP_CAT_QUEST};
        for (int i = 0; i < 3; i++)
            if (r->mode[pickups[i]] != COOP_MODE_SHARED) r->mode[pickups[i]] = COOP_MODE_SHARED, changed |= 1u << pickups[i];
    }
    return changed;
}

void coop_rules_text(const CoopRules *r, char *out, size_t cap) {
    size_t at = 0;
    if (cap) out[0] = 0;
    for (int c = 0; c < COOP_CAT_N && at < cap; c++) {
        int k = snprintf(out + at, cap - at, "%s%s %s", c ? ", " : "", cat_names[c], coop_mode_name(r->mode[c]));
        if (k < 0) break;
        at += (size_t)k;
    }
}

// ---- registers and records

static int flag_synced(const CoopSync *s, const Reg *r) { return s->have_room && s->room.mode[r->cat] == COOP_MODE_SHARED; }
static int pool_synced(const CoopSync *s, const Pool *p) { return s->have_room && s->room.mode[p->cat] == COOP_MODE_POOL; }
static int flag_sec(const Reg *r) { return (int)(key_of(COOP_R_FLAG, r->hash, 0) % COOP_SYNC_SECTIONS); }
static uint32_t flag_share(const Reg *r, uint32_t v) { return v ? (uint32_t)mix64(key_of(COOP_R_FLAG, r->hash, 0) ^ v) : 0; }
static int rec_sec(const Pool *p, const Rec *r) { return (int)(key_of(COOP_R_POOL, p->hash, r->kid) % COOP_SYNC_SECTIONS); }
static uint32_t rec_share(const Pool *p, const Rec *r) { return (uint32_t)mix64(key_of(COOP_R_POOL, p->hash, r->kid) ^ get64(r->sig) ^ r->seq) | 1; }

// Removed: by any charter of this room this game saw (the charter carries the last MAX_REMOVED; the game keeps them all).
static int removed(const CoopSync *s, uint64_t kid) { return in_list(s->gone, s->ngone, kid); }
static int admitted(const CoopSync *s, uint64_t kid) { return s->have_room && in_list(s->ch.admit, s->ch.nadmit, kid) && !removed(s, kid); }

static void gone_add(CoopSync *s, uint64_t kid) {
    if (!kid || removed(s, kid)) return;
    if (s->ngone == s->gone_cap) {
        int nc = s->gone_cap ? s->gone_cap * 2 : 64;
        uint64_t *ng = realloc(s->gone, (size_t)nc * sizeof *ng);
        if (!ng) return;
        s->gone = ng, s->gone_cap = nc;
    }
    s->gone[s->ngone++] = kid;
}

// Another key's identity for this member's session checked out recently, as well as the one held: only one of them is
// the member's, and a game cannot tell which, so neither counts until one of them is no longer heard.
static int disputed(const CoopSync *s, const Peer *p) { return p->rival_at && s->now - p->rival_at < IDENT_HOLD_MS; }

// A peer whose messages count: identified and not disputed, not removed, not ignored.
static int peer_counts(const CoopSync *s, const Peer *p) {
    return p->present && p->ident && !disputed(s, p) && !p->ignored && !removed(s, p->kid);
}

// The slot of a flag in the index: keyed with a key of this process, so a peer cannot pile registers onto one run.
static unsigned slot_of(const CoopSync *s, uint32_t hash) {
    uint8_t m[4];
    for (int i = 0; i < 4; i++) m[i] = (uint8_t)(hash >> 8 * i);
    return (unsigned)coop_siphash24(s->ik0, s->ik1, m, sizeof m);
}

static int find_reg(const CoopSync *s, uint32_t hash) {
    if (!s->index_cap) return -1;
    unsigned mask = (unsigned)s->index_cap - 1, i = slot_of(s, hash) & mask;
    for (;;) {
        int k = s->index[i];
        if (!k) return -1;
        if (s->regs[k - 1].hash == hash) return k - 1;
        i = (i + 1) & mask;
    }
}

static void index_insert(CoopSync *s, int idx) {
    unsigned mask = (unsigned)s->index_cap - 1, i = slot_of(s, s->regs[idx].hash) & mask, steps = 1;
    while (s->index[i]) i = (i + 1) & mask, steps++;
    s->index[i] = idx + 1;
    if (steps > s->st.probe_max) s->st.probe_max = steps;
}

// The highest value of (cat, hash) the local game takes (CoopSyncLimitFn); 0: none, or no limits yet.
static uint32_t limit_of(const CoopSync *s, int cat, uint32_t hash) { return s->limit ? s->limit(cat, hash, s->limit_user) : 0; }

static Pool *pool_get(CoopSync *s, uint32_t hash, int cat, int create) {
    for (int i = 0; i < s->npools; i++)
        if (s->pools[i].hash == hash) return &s->pools[i];
    if (!create || s->npools == MAX_POOLS) return NULL;
    Pool *p = &s->pools[s->npools++];
    memset(p, 0, sizeof *p);
    p->hash = hash, p->cat = (uint8_t)cat, p->cap = -1;
    uint32_t max = limit_of(s, cat, hash);
    p->max = max && max < INT32_MAX ? (int32_t)max : INT32_MAX;
    return p;
}

static int reg_get(CoopSync *s, int cat, uint32_t hash) {
    int idx = find_reg(s, hash);
    if (idx >= 0) return idx;
    if (s->nregs >= MAX_REGS) return -1;
    if (s->nregs == s->cap) {
        int nc = s->cap ? s->cap * 2 : 1024;
        Reg *nr = realloc(s->regs, (size_t)nc * sizeof *nr);
        if (!nr) return -1;
        s->regs = nr, s->cap = nc;
    }
    if ((s->nregs + 1) * 10 > s->index_cap * 7) {   // keep the index under 70 % full
        int nc = s->index_cap ? s->index_cap * 2 : 2048;
        int *ni = calloc((size_t)nc, sizeof *ni);
        if (!ni) return -1;
        free(s->index);
        s->index = ni, s->index_cap = nc;
        for (int i = 0; i < s->nregs; i++) index_insert(s, i);
    }
    idx = s->nregs++;
    Reg *r = &s->regs[idx];
    memset(r, 0, sizeof *r);
    r->cat = (uint8_t)cat, r->hash = hash;
    index_insert(s, idx);
    return idx;
}

static void queue_apply(CoopSync *s, int code) {
    if (s->apply_tail - s->apply_head >= s->apply_cap) {
        unsigned nc = s->apply_cap ? s->apply_cap * 2 : 1024;
        int *na = malloc(nc * sizeof *na);
        if (!na) return;
        for (unsigned i = 0; i < s->apply_tail - s->apply_head; i++) na[i] = s->apply[(s->apply_head + i) % s->apply_cap];
        s->apply_tail -= s->apply_head, s->apply_head = 0;
        free(s->apply);
        s->apply = na, s->apply_cap = nc;
    }
    s->apply[s->apply_tail++ % s->apply_cap] = code;
}

// The records of a pool that count in this game: its own, and those of the keys its player accepted in the room. A
// pool follows this player's acceptance, as flags do; the maker's admission decides who may contribute at all.
static int kid_counts(const CoopSync *s, uint64_t kid) { return kid == s->kid || in_list(s->acc_kids, s->nacc, kid); }

// The slot of key kid in the list of keys this player accepted, -1 none.
static int acc_index(const CoopSync *s, uint64_t kid) {
    for (int i = 0; i < s->nacc; i++)
        if (s->acc_kids[i] == kid) return i;
    return -1;
}
static int rec_counts(const CoopSync *s, const Rec *r) { return kid_counts(s, r->kid); }

typedef struct { int64_t value, top, in; int n; } PoolSum;   // n: the records counted; top: the highest base; in: what entered

static int64_t pool_lim(const Pool *p) { return p->max < POOL_REG_MAX ? p->max : POOL_REG_MAX; }

// A pool's value over n records: the highest base + all gains - all spends. The highest base counts as at least 0: an
// honest epoch starts with a claim of at least 0, so a lower base is a record that would only turn what the others
// bring into a debt, which no removal undoes. A record's spends count up to its gains plus the counter's limit (lim),
// and never beyond what entered the pool (the highest base and every gain): no contributor takes more than the pool
// really held, so a dungeon's pool loses at most its real number of keys however much a record claims.
static PoolSum sum_rows(const Seen *v, int n, int64_t lim) {
    PoolSum o = {0, 0, 0, n};
    int64_t gains = 0, spends = 0;
    for (int i = 0; i < n; i++) {
        if (v[i].base > o.top) o.top = v[i].base;
        gains += v[i].gain;
    }
    if (!n) return o;
    o.in = o.top + gains;
    for (int i = 0; i < n; i++) {
        int64_t bound = (int64_t)v[i].gain + lim < o.in ? (int64_t)v[i].gain + lim : o.in;
        spends += v[i].spend < bound ? (int64_t)v[i].spend : bound;
    }
    o.value = o.top + gains - spends;
    return o;
}

// The pool's value over the records that count here (with only: those of these keys in that epoch).
static PoolSum pool_sum(const CoopSync *s, const Pool *p, const uint64_t *only, int nonly, uint32_t epoch) {
    Seen v[MAX_ADMIT];
    int n = 0;
    for (int i = 0; i < p->nrec; i++) {
        const Rec *r = &p->rec[i];
        if (!rec_counts(s, r) || (only && (r->epoch != epoch || !in_list(only, nonly, r->kid)))) continue;
        v[n++] = (Seen){r->kid, r->base, r->gain, r->spend};
    }
    return sum_rows(v, n, pool_lim(p));
}

// What join j would have claimed, had this game counted then the keys it counts now (and only those in keep, when
// set): its claim (-POOL_REG_MAX: none), and what it adds as a gain instead when the pool was in debt (a first join).
static void join_claim(const CoopSync *s, const Pool *p, const Join *j, const uint64_t *keep, int nkeep, int32_t *claim, uint32_t *debt) {
    Seen v[MAX_ADMIT];
    int n = 0;
    for (int i = 0; i < j->nseen; i++)
        if (kid_counts(s, j->seen[i].kid) && !removed(s, j->seen[i].kid) && (!keep || in_list(keep, nkeep, j->seen[i].kid))) v[n++] = j->seen[i];
    PoolSum ps = sum_rows(v, n, pool_lim(p));
    *claim = -POOL_REG_MAX, *debt = 0;
    if (ps.value >= 0 || !ps.n) *claim = (int32_t)clamp_reg(j->want - (ps.n ? ps.value - ps.top : 0));
    else if (j->fresh && j->want > 0) *debt = (uint32_t)(j->want < POOL_REG_MAX ? j->want : POOL_REG_MAX);
}

// This game's base and join gain in pool p as its joins of the epoch claim them now (keep: counting only those keys):
// the highest claim, and the gain the first join added in debt. With nothing changed since, what its record holds.
static void claims_now(const CoopSync *s, const Pool *p, const uint64_t *keep, int nkeep, int32_t *base, uint32_t *debt) {
    *base = p->joins.floor, *debt = 0;
    for (int i = 0; i < p->joins.n; i++) {
        int32_t c;
        uint32_t d;
        join_claim(s, p, &p->joins.j[i], keep, nkeep, &c, &d);
        if (c > *base) *base = c;
        *debt += d;
    }
}

static int64_t pool_value(const CoopSync *s, const Pool *p) { return pool_sum(s, p, NULL, 0, 0).value; }

static int32_t pool_target(const CoopSync *s, const Pool *p) {
    int64_t v = pool_value(s, p);
    if (v < 0) v = 0;
    if (p->cap >= 0 && v > p->cap) v = p->cap;
    if (v > p->max) v = p->max;
    return (int32_t)v;
}

static void pool_check(CoopSync *s, Pool *p) {
    if (!p->bound || p->queued || pool_target(s, p) == p->expected) return;
    p->queued = 1;
    queue_apply(s, -1 - (int)(p - s->pools));
}

static void flag_check(CoopSync *s, int idx) {
    Reg *r = &s->regs[idx];
    if (r->queued || !flag_synced(s, r) || r->s <= r->l || r->s == r->applied) return;
    r->queued = 1;
    queue_apply(s, idx);
}

static void flag_raise(CoopSync *s, int idx, uint32_t v) {
    Reg *r = &s->regs[idx];
    if (flag_synced(s, r)) s->sums[flag_sec(r)] += flag_share(r, v) - flag_share(r, r->s);
    r->s = v;
    flag_check(s, idx);
}

// Record slot i of pool p becomes r (i == p->nrec: a new one).
static void rec_set(CoopSync *s, Pool *p, int i, const Rec *r) {
    int counted = pool_synced(s, p);
    if (i < p->nrec && counted) s->sums[rec_sec(p, &p->rec[i])] -= rec_share(p, &p->rec[i]);
    if (i == p->nrec) p->nrec++;
    p->rec[i] = *r;
    if (counted) s->sums[rec_sec(p, r)] += rec_share(p, r);
    pool_check(s, p);
}

static Rec *rec_of(Pool *p, uint64_t kid) {
    for (int i = 0; i < p->nrec; i++)
        if (p->rec[i].kid == kid) return &p->rec[i];
    return NULL;
}

static void recompute_sums(CoopSync *s) {
    memset(s->sums, 0, sizeof s->sums);
    for (int i = 0; i < s->nregs; i++) {
        const Reg *r = &s->regs[i];
        if (r->s && flag_synced(s, r)) s->sums[flag_sec(r)] += flag_share(r, r->s);
    }
    for (int k = 0; k < s->npools; k++) {
        const Pool *p = &s->pools[k];
        if (pool_synced(s, p))
            for (int i = 0; i < p->nrec; i++) s->sums[rec_sec(p, &p->rec[i])] += rec_share(p, &p->rec[i]);
    }
}

static void push_code(CoopSync *s, int idx, uint64_t now) {
    if (!s->in_room) return;                                    // a digest carries it once the room is known
    if (s->push_tail - s->push_head >= PUSH_SLOTS) return;      // the digests repair what this drops
    s->push[s->push_tail++ % PUSH_SLOTS] = (Push){idx, 0, now, now};
}

static void drop_pushes_of_records(CoopSync *s) {
    unsigned keep = s->push_head;
    for (unsigned i = s->push_head; i != s->push_tail; i++)
        if (s->push[i % PUSH_SLOTS].idx >= 0) s->push[keep++ % PUSH_SLOTS] = s->push[i % PUSH_SLOTS];
    s->push_tail = keep;
}

// The signed bytes of a record of pool p into m; their length.
static size_t rec_message(const CoopSync *s, const Pool *p, const Rec *r, uint8_t *m) {
    uint8_t *at = m;
    putn(&at, SIG_POOL, sizeof SIG_POOL - 1);
    putn(&at, s->ctx, CTX_LEN);
    put8(&at, p->cat);
    put32(&at, p->hash);
    put32(&at, r->epoch);
    put32(&at, r->seq);
    put32(&at, base_encode(r->base));
    put32(&at, r->gain);
    put32(&at, r->spend);
    return (size_t)(at - m);
}

// This game's own record of p changed: signed, stored and sent.
static void own_record(CoopSync *s, Pool *p, Rec *r, uint64_t now) {
    uint8_t m[96];
    r->seq++;
    coop_sign(r->sig, s->sk, m, rec_message(s, p, r, m));
    Rec *old = rec_of(p, s->kid);
    int i = old ? (int)(old - p->rec) : p->nrec;
    if (i == MAX_ADMIT) return;
    rec_set(s, p, i, r);
    push_code(s, -1 - ((int)(p - s->pools) * MAX_ADMIT + i), now);
}

// Forget every flag, pool, queued push and apply (the allocations stay for reuse). keep_own: this game's own pool
// records stay (another save in the same room: they are the room's), with the join that set each and a reset's carry
// it has not joined with yet.
static void drop_state(CoopSync *s, int keep_own) {
    s->nregs = 0;
    if (s->index) memset(s->index, 0, (size_t)s->index_cap * sizeof *s->index);
    int n = 0;
    for (int k = 0; k < s->npools; k++) {
        Pool *p = &s->pools[k];
        Rec *mine = keep_own ? rec_of(p, s->kid) : NULL;
        if (!mine && !(keep_own && p->has_carry)) continue;
        Rec r;
        if (mine) r = *mine;
        Joins joins = p->joins;
        int has_carry = p->has_carry;
        int64_t carry = p->carry;
        Pool *q = &s->pools[n++];
        uint32_t hash = p->hash;
        uint8_t cat = p->cat;
        int32_t max = p->max;
        memset(q, 0, sizeof *q);
        q->hash = hash, q->cat = cat, q->max = max, q->cap = -1;
        if (mine) q->rec[0] = r, q->nrec = 1;
        q->joins = joins, q->has_carry = has_carry, q->carry = carry;
    }
    s->npools = n;
    s->push_head = s->push_tail = 0;
    s->apply_head = s->apply_tail = 0;
    recompute_sums(s);
}

void coop_sync_limits(CoopSync *s, CoopSyncLimitFn fn, void *user) { s->limit = fn, s->limit_user = user; }

void coop_sync_index_key(CoopSync *s, const uint8_t key[16]) {
    s->ik0 = s->ik1 = 0;
    for (int i = 0; i < 8; i++) s->ik0 |= (uint64_t)key[i] << 8 * i, s->ik1 |= (uint64_t)key[8 + i] << 8 * i;
    if (s->index) memset(s->index, 0, (size_t)s->index_cap * sizeof *s->index);
    for (int i = 0; i < s->nregs; i++) index_insert(s, i);
}

void coop_sync_schema(CoopSync *s, uint32_t schema) {
    if (schema == s->schema) return;
    s->schema = schema;
    drop_state(s, 0);   // what was merged under another classification means nothing under this one
    s->caught_up = 0;
    s->digest_to |= 1;
}

unsigned coop_sync_ignored(const CoopSync *s) {
    unsigned mask = 0;
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) mask |= (unsigned)(s->peer[id].present && s->peer[id].ignored) << id;
    return mask;
}

// ---- lifetime and identity

static void derive(CoopSync *s) {
    uint8_t k[32];
    coop_blake2b(k, sizeof k, s->seed, sizeof s->seed, "nx2navi-coop/3 offer", 20);
    s->offer_k0 = get64(k), s->offer_k1 = get64(k + 8);
    if (!s->code[0]) return;
    // one key for each room and start of the game, so nothing ties a player's identity in one room to another; the
    // room's own seed instead when the game keeps one for it (the maker's: its key outlives a restart)
    int own = s->room_seed_code[0] && !strcmp(s->room_seed_code, s->code);
    coop_blake2b(k, sizeof k, own ? s->room_seed : s->seed, 32, s->code, strlen(s->code));
    coop_sign_keypair(k, s->sk, s->pk);
    s->kid = kid_of(s->pk);
    coop_blake2b(s->ctx, CTX_LEN, s->code, strlen(s->code), "nx2navi-coop/3 sign", 19);
    s->id_valid = 0;
}

CoopSync *coop_sync_new(uint64_t self_id, uint32_t schema) {
    CoopSync *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->self_id = self_id, s->schema = schema;
    s->rng = mix64(self_id ^ 0x5EED);
    for (int i = 0; i < 4; i++) {   // until coop_sync_seed: from the id, which is random for each start too
        uint64_t v = mix64(self_id + (uint64_t)i * 0x1234567);
        memcpy(s->seed + 8 * i, &v, 8);
    }
    s->copy_gen = UINT32_MAX;
    derive(s);
    return s;
}

void coop_sync_seed(CoopSync *s, const uint8_t seed[32]) {
    memcpy(s->seed, seed, sizeof s->seed);
    derive(s);
}

void coop_sync_room_seed(CoopSync *s, const char *code, const uint8_t seed[32]) {
    if (!strcmp(code, s->room_seed_code) && !memcmp(seed, s->room_seed, sizeof s->room_seed)) return;
    snprintf(s->room_seed_code, sizeof s->room_seed_code, "%s", code);
    memcpy(s->room_seed, seed, sizeof s->room_seed);
    if (s->code[0] && !strcmp(code, s->code)) derive(s);
}

void coop_sync_name(CoopSync *s, const char *name) {
    char clean[NAME_CAP];
    coop_name_clean(clean, sizeof clean, (const uint8_t *)name, strlen(name));
    if (strcmp(clean, s->name)) memcpy(s->name, clean, sizeof clean), s->id_valid = 0;
}

void coop_sync_free(CoopSync *s) {
    if (!s) return;
    free(s->regs);
    free(s->index);
    free(s->ended);
    for (int i = 0; i < MAX_ACCEPTED; i++) free(s->kept[i].sess);
    free(s->gone);
    free(s);
}

void coop_sync_trace(CoopSync *s, void (*fn)(void *user, const char *line), void *user) { s->trace = fn, s->trace_user = user; }

// The signed bytes of the identity of member id in session (prev: its session in the room before, 0 none), with key pk
// and name.
static size_t identity_message(const CoopSync *s, int id, uint64_t session, uint64_t prev, const uint8_t *pk, const char *name, uint8_t *m) {
    uint8_t *at = m;
    size_t nl = strlen(name);
    putn(&at, SIG_MEMBER, sizeof SIG_MEMBER - 1);
    putn(&at, s->ctx, CTX_LEN);
    put8(&at, (uint8_t)id);
    put64(&at, session);
    put64(&at, prev);
    putn(&at, pk, COOP_PK_LEN);
    put8(&at, (uint8_t)nl);
    putn(&at, name, nl);
    return (size_t)(at - m);
}

// ---- the charter

static size_t charter_fields(const Charter *c, uint8_t *m) {
    uint8_t *at = m;
    put32(&at, c->epoch);
    put8(&at, COOP_CAT_N);
    putn(&at, c->mode, COOP_CAT_N);
    put32(&at, c->pool_epoch);
    put8(&at, c->nadmit);
    for (int i = 0; i < c->nadmit; i++) put64(&at, c->admit[i]);
    put8(&at, c->nremoved);
    for (int i = 0; i < c->nremoved; i++) put64(&at, c->removed[i]);
    putn(&at, c->pk, COOP_PK_LEN);
    return (size_t)(at - m);
}

static size_t charter_message(const CoopSync *s, const Charter *c, uint8_t *m) {
    uint8_t *at = m;
    putn(&at, SIG_RULES, sizeof SIG_RULES - 1);
    putn(&at, s->ctx, CTX_LEN);
    return (size_t)(at - m) + charter_fields(c, at);
}

// A charter from b (n bytes left); its length, 0 when malformed. Not checked against its signature.
static size_t charter_parse(const uint8_t *b, size_t n, Charter *c) {
    memset(c, 0, sizeof *c);
    size_t at = 0;
    if (n < 5) return 0;
    c->epoch = get32(b), at = 4;
    size_t ncat = b[at++];
    if (ncat != COOP_CAT_N || at + ncat + 5 > n) return 0;
    memcpy(c->mode, b + at, ncat), at += ncat;
    c->pool_epoch = get32(b + at), at += 4;
    c->nadmit = b[at++];
    if (c->nadmit > MAX_ADMIT || at + 8u * c->nadmit + 1 > n) return 0;
    for (int i = 0; i < c->nadmit; i++) c->admit[i] = get64(b + at), at += 8;
    c->nremoved = b[at++];
    if (c->nremoved > MAX_REMOVED || at + 8u * c->nremoved + COOP_PK_LEN + COOP_SIG_LEN > n) return 0;
    for (int i = 0; i < c->nremoved; i++) c->removed[i] = get64(b + at), at += 8;
    memcpy(c->pk, b + at, COOP_PK_LEN), at += COOP_PK_LEN;
    memcpy(c->sig, b + at, COOP_SIG_LEN), at += COOP_SIG_LEN;
    c->kid = kid_of(c->pk);
    return at;
}

static void update_caught_up(CoopSync *s) {
    if (s->caught_up || !s->in_room || !s->have_room) return;
    int any = 0, all = 1;
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
        const Peer *p = &s->peer[id];
        if (!p->present) continue;
        if (!p->ident) {
            if (s->now - p->seen_at < IDENT_WAIT_MS) return;   // perhaps a member that holds the pools: wait for it
            continue;
        }
        if (!peer_counts(s, p)) continue;
        if (p->have_digest && !memcmp(p->sums, s->sums, sizeof s->sums)) any = 1;
        else all = 0;
    }
    // every member's state, or, after a while, one member's: one that never matches cannot keep the pools unbound
    if (!all && !(any && s->now - s->joined_at >= CATCHUP_MS)) return;
    s->caught_up = 1;
    tracef(s, "caught up with the room");
}

// The member id holding key kid, 0 none (this player included).
static int member_of_kid(const CoopSync *s, uint64_t kid) {
    if (!kid) return 0;
    if (kid == s->kid && s->self) return s->self;
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++)
        if (s->peer[id].present && s->peer[id].ident && s->peer[id].kid == kid) return id;
    return 0;
}

static void adopt_charter(CoopSync *s, const Charter *c) {
    int had = s->have_room;
    uint32_t old_pool_epoch = s->ch.pool_epoch;
    CoopRules old = s->room;
    s->ch = *c;
    s->have_room = 1;
    memcpy(s->room.mode, c->mode, COOP_CAT_N);
    coop_rules_normalize(&s->room);
    s->room.epoch = c->epoch;
    s->rules_ver++;
    for (int i = 0; i < c->nremoved; i++) gone_add(s, c->removed[i]);
    // a pool reset: each pool carries what it held for the identities the new charter admits, so the games join the
    // new epoch with that, not with what a drainer who is gone left in their counters. This game's own record counts
    // with the claim its join would have made counting only those identities, so keys it brought while a player who
    // is not admitted again had gains in the pool are not lost to them. A game the reset admits signs its carry at once
    // (below); one it does not admit, or that carries nothing, keeps it unsigned for its first join, and a second reset
    // before that keeps the larger of the two carries: keys the others used in between may come back, never fewer
    if (had && c->pool_epoch != old_pool_epoch) {
        uint64_t keep[MAX_ADMIT];
        int nk = 0;
        for (int i = 0; i < c->nadmit; i++)
            if (!removed(s, c->admit[i])) keep[nk++] = c->admit[i];
        for (int k = 0; k < s->npools; k++) {
            Pool *p = &s->pools[k];
            Seen v[MAX_ADMIT];
            int n = 0;
            for (int i = 0; i < p->nrec; i++) {
                const Rec *r = &p->rec[i];
                if (!rec_counts(s, r) || r->epoch != old_pool_epoch || !in_list(keep, nk, r->kid)) continue;
                v[n] = (Seen){r->kid, r->base, r->gain, r->spend};
                if (r->kid == s->kid && p->joins.n && p->joins.epoch == old_pool_epoch) {
                    uint32_t debt, joined = p->joins.debt < r->gain ? p->joins.debt : r->gain;
                    claims_now(s, p, keep, nk, &v[n].base, &debt);
                    v[n].gain = r->gain - joined + debt;
                }
                n++;
            }
            int64_t carry = sum_rows(v, n, pool_lim(p)).value;
            if (!p->has_carry || carry > p->carry) p->carry = carry;
            p->has_carry = 1;
            p->joins.n = 0;
        }
    } else if (had) {
        // the same pools' epoch: a removal may drop records this game counts, so each claim is made again
        for (int k = 0; k < s->npools; k++) s->pools[k].recheck = 1;
    }
    // records of identities no longer admitted, removed, or of another pools' epoch are dropped; a pool this game no
    // longer contributes to, or of a new epoch, joins again
    int dropped = 0;
    for (int k = 0; k < s->npools; k++) {
        Pool *p = &s->pools[k];
        for (int i = 0; i < p->nrec;) {
            Rec *r = &p->rec[i];
            if (admitted(s, r->kid) && r->epoch == c->pool_epoch) {
                i++;
                continue;
            }
            p->rec[i] = p->rec[--p->nrec];
            dropped = 1;
        }
        if (p->bound && (s->room.mode[p->cat] != COOP_MODE_POOL || !admitted(s, s->kid) || c->pool_epoch != old_pool_epoch)) p->bound = 0;
    }
    if (dropped) drop_pushes_of_records(s);
    recompute_sums(s);
    // a pool reset: what this game carries is its claim in the new epoch from now on, signed at once as its record, so
    // that every game counts it from the reset on and adds what anyone picks up after it, also while this game is at its
    // title screen or never plays again. A game the reset does not admit claims its carry when it joins
    // (coop_sync_pool_local)
    if (had && c->pool_epoch != old_pool_epoch && admitted(s, s->kid))
        for (int k = 0; k < s->npools; k++) {
            Pool *p = &s->pools[k];
            if (!p->has_carry || p->carry <= 0 || s->room.mode[p->cat] != COOP_MODE_POOL || rec_of(p, s->kid)) continue;
            Rec r;
            memset(&r, 0, sizeof r);
            r.kid = s->kid, r.epoch = c->pool_epoch, r.base = (int32_t)clamp_reg(p->carry);
            memcpy(r.pk, s->pk, COOP_PK_LEN);
            p->joins = (Joins){.epoch = c->pool_epoch, .floor = r.base};
            p->has_carry = 0;
            own_record(s, p, &r, s->now);
        }
    if (!had || memcmp(old.mode, s->room.mode, COOP_CAT_N) || c->pool_epoch != old_pool_epoch) s->caught_up = 0;
    for (int i = 0; i < s->nregs; i++) flag_check(s, i);
    for (int k = 0; k < s->npools; k++) pool_check(s, &s->pools[k]);
    s->digest_to |= 1;
    update_caught_up(s);
    tracef(s, "the room's charter: epoch %u by member %d, %u admitted, %u removed, pools' epoch %u", c->epoch, member_of_kid(s, c->kid),
           c->nadmit, c->nremoved, c->pool_epoch);
}

// The one signer the charters of the identified members that count carry (0 none); *differ: two of them differ, or one
// differs from want (when set). Members that appeared recently and did not have their say yet: *waiting.
static uint64_t members_signer(const CoopSync *s, uint64_t want, int *differ, int *waiting) {
    uint64_t one = want;
    *differ = *waiting = 0;
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
        const Peer *p = &s->peer[id];
        if (!p->present) continue;
        if (!p->ident || !p->ch_seen) {
            if (s->now - p->seen_at < IDENT_WAIT_MS) *waiting = 1;
            continue;
        }
        if (!p->ch_kid || p->ignored || removed(s, p->kid)) continue;
        if (one && p->ch_kid != one) *differ = 1;
        if (!one) one = p->ch_kid;
    }
    return one;
}

// The key whose charter this game takes, 0 none yet: the one pinned when the player accepted; else the room this game
// made in this session is its own; else the key of the member the relay names as the maker, but only while no member
// carries another signer's charter (a relay's mark cannot outvote the members); else, with no maker in the room, the
// signer of the charter in force, or the one signer every identified member's charter carries. Every member that
// appeared recently has its say first, not the quickest.
static uint64_t trusted_signer(const CoopSync *s) {
    if (s->have_pin) return s->pin_kid;
    if (s->maker && s->maker == s->self && s->made) return s->kid;
    int differ, waiting;
    uint64_t mark = 0;
    if (s->maker && s->maker != s->self) {   // a mark on this player for a room it did not make is no mark
        const Peer *m = &s->peer[s->maker];
        if (!m->present || !m->ident) return 0;
        mark = m->kid;
    }
    uint64_t one = members_signer(s, mark, &differ, &waiting);
    if (waiting || differ) return 0;
    if (mark) return mark;
    return s->have_room ? s->ch.kid : one;
}

// A signature check on behalf of peer p: 1 valid, 0 not, -1 not checked (p's budget of checks is spent: the message
// is dropped, a digest brings it again). The check that failed last is not made again for the same signature, key and
// bytes; the tag covers all three (keyed with this process's own key), so that a changed copy of a genuine message,
// which anyone with the room's secret can send first, does not make the genuine one fail.
static int sig_check(CoopSync *s, Peer *p, const uint8_t sig[COOP_SIG_LEN], const uint8_t pk[COOP_PK_LEN], const uint8_t *m, size_t n) {
    uint8_t sp[COOP_SIG_LEN + COOP_PK_LEN];
    memcpy(sp, sig, COOP_SIG_LEN), memcpy(sp + COOP_SIG_LEN, pk, COOP_PK_LEN);
    uint64_t tag = coop_siphash24(s->ik0 ^ coop_siphash24(s->ik0, s->ik1, sp, sizeof sp), s->ik1, m, n) | 1;
    if (p->bad_sig == tag) return 0;
    if (!p->sig_at) p->sig_at = s->now, p->sig_tokens = SIG_BURST;
    if (s->now > p->sig_at) p->sig_tokens += (int64_t)(s->now - p->sig_at) * SIG_RATE / 1000, p->sig_at = s->now;
    if (p->sig_tokens > SIG_BURST) p->sig_tokens = SIG_BURST;
    if (p->sig_tokens < 1) {
        s->st.sig_skipped++;
        return -1;
    }
    p->sig_tokens--;
    s->st.sig_checks++;
    if (coop_sign_check(sig, pk, m, n)) return 1;
    p->bad_sig = tag;
    return 0;
}

// A charter a digest carries. A digest is not signed by its sender (anyone with the room's secret can send one in a
// member's name), so a charter that does not check out is dropped and counted, never held against that member.
static void consider_charter(CoopSync *s, int from, const Charter *c) {
    if (s->have_room && c->kid == s->ch.kid && c->epoch <= s->ch.epoch) return;   // nothing newer
    if (c->kid != trusted_signer(s)) return;
    uint8_t m[640];
    int ok = sig_check(s, &s->peer[from], c->sig, c->pk, m, charter_message(s, c, m));
    if (!ok) s->st.forged++;
    if (ok == 1) adopt_charter(s, c);
}

// This game holds the key the room's rules are signed with, or makes the first ones (a room it made in this session).
static int is_maker(const CoopSync *s) {
    if (!s->in_room || !s->have_mine) return 0;
    if (s->have_room) return s->ch.kid == s->kid && (!s->have_pin || s->pin_kid == s->kid);
    return s->have_pin ? s->pin_kid == s->kid : s->made;
}

int coop_sync_is_maker(const CoopSync *s) { return is_maker(s); }

// The members this player accepted that charter c does not admit (bit per id).
static unsigned admit_wanted(const CoopSync *s, const Charter *c) {
    unsigned m = 0;
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
        const Peer *p = &s->peer[id];
        if (p->present && p->ident && in_list(s->acc_kids, s->nacc, p->kid) && !in_list(c->admit, c->nadmit, p->kid) && !removed(s, p->kid))
            m |= 1u << id;
    }
    return m;
}

// The maker's game signs a new charter: its settings, the pools' admissions (itself, and the members its player
// accepted), a removal or a pool reset it was asked for, one epoch later (saturating: only the maker raises it).
static void originate(CoopSync *s) {
    Charter c;
    if (s->have_room && s->ch.kid == s->kid) c = s->ch;
    else memset(&c, 0, sizeof c), c.epoch = s->have_room ? s->ch.epoch : 0, c.pool_epoch = s->have_room ? s->ch.pool_epoch : 0;
    CoopRules r = s->mine;
    coop_rules_normalize(&r);
    memcpy(c.mode, r.mode, COOP_CAT_N);
    if (s->want_reset) c.pool_epoch++, c.nadmit = 0;
    if (s->want_remove) {
        if (!in_list(c.removed, c.nremoved, s->want_remove)) {   // the charter carries the latest; every game keeps all
            if (c.nremoved == MAX_REMOVED) memmove(c.removed, c.removed + 1, sizeof c.removed[0] * --c.nremoved);
            c.removed[c.nremoved++] = s->want_remove;
        }
        for (int i = 0; i < c.nadmit; i++)
            if (c.admit[i] == s->want_remove) c.admit[i--] = c.admit[--c.nadmit];
    }
    s->want_reset = 0, s->want_remove = 0;
    if (!in_list(c.admit, c.nadmit, s->kid) && c.nadmit < MAX_ADMIT) c.admit[c.nadmit++] = s->kid;
    unsigned want = admit_wanted(s, &c);
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
        if (!(want >> id & 1)) continue;
        if (c.nadmit == MAX_ADMIT) {
            tracef(s, "the pools admit no more players: a pool reset admits the ones in the room");
            break;
        }
        c.admit[c.nadmit++] = s->peer[id].kid;
    }
    c.epoch = c.epoch == UINT32_MAX ? UINT32_MAX : c.epoch + 1;
    memcpy(c.pk, s->pk, COOP_PK_LEN);
    c.kid = s->kid;
    uint8_t m[640];
    coop_sign(c.sig, s->sk, m, charter_message(s, &c, m));
    adopt_charter(s, &c);
}

// What the maker's charter lacks (other settings, an admission, a removal or reset asked for).
static int charter_stale(const CoopSync *s) {
    if (!s->have_room || s->ch.kid != s->kid) return 1;
    CoopRules r = s->mine;
    coop_rules_normalize(&r);
    return memcmp(s->ch.mode, r.mode, COOP_CAT_N) || s->want_reset || s->want_remove || !in_list(s->ch.admit, s->ch.nadmit, s->kid) ||
           (admit_wanted(s, &s->ch) && s->ch.nadmit < MAX_ADMIT);
}

unsigned coop_sync_rules(const CoopSync *s, CoopRules *out) {
    if (out) {
        if (s->have_room) *out = s->room, out->maker = (uint8_t)member_of_kid(s, s->ch.kid);
        else memset(out, 0, sizeof *out);
    }
    return s->rules_ver;
}

int coop_sync_caught_up(const CoopSync *s) { return s->caught_up; }

void coop_sync_settings(CoopSync *s, const CoopRules *mine, int made_room) {
    s->mine = *mine;
    coop_rules_normalize(&s->mine);
    s->have_mine = 1;
    s->made = made_room;
}

int coop_sync_key_parse(const char *text, uint64_t *kid) {
    uint64_t v = 0;
    int digits = 0;
    for (const char *c = text; *c; c++) {
        int d = *c >= '0' && *c <= '9' ? *c - '0' : *c >= 'a' && *c <= 'f' ? *c - 'a' + 10 : *c >= 'A' && *c <= 'F' ? *c - 'A' + 10 : -1;
        if (d < 0 && *c == '-' && digits && digits % 4 == 0 && c[1]) continue;
        if (d < 0 || ++digits > 16) return 0;
        v = v << 4 | (uint64_t)d;
    }
    if (digits != 16 || !v) return 0;
    *kid = v;
    return 1;
}

int coop_sync_remove_key(CoopSync *s, uint64_t kid) {
    // a key the room knows: admitted to the pools, accepted by this player, or a member's
    if (!is_maker(s) || !kid || kid == s->kid) return 0;
    if (!in_list(s->ch.admit, s->ch.nadmit, kid) && !in_list(s->acc_kids, s->nacc, kid) && !member_of_kid(s, kid)) return 0;
    s->want_remove = kid;
    originate(s);   // at once: the removal reaches everyone with the next digests
    s->originated_at = s->now ? s->now : 1;
    return 1;
}

int coop_sync_remove(CoopSync *s, int member) {
    if (member < 1 || member > COOP_MAX_MEMBERS || member == s->self) return 0;
    const Peer *p = &s->peer[member];
    return p->present && p->ident && coop_sync_remove_key(s, p->kid);
}

int coop_sync_reset_pools(CoopSync *s) {
    if (!is_maker(s)) return 0;
    s->want_reset = 1;
    originate(s);
    s->originated_at = s->now ? s->now : 1;
    return 1;
}

int coop_sync_contributors(const CoopSync *s, CoopContributor *out, int max) {
    int n = 0;
    for (int i = 0; s->have_room && i < s->ch.nadmit && n < max; i++) {
        uint64_t kid = s->ch.admit[i];
        CoopContributor *c = &out[n++];
        memset(c, 0, sizeof *c);
        fingerprint(kid, c->key);
        c->member = member_of_kid(s, kid);
        c->self = kid == s->kid;
        c->removed = removed(s, kid);
        if (c->member && !c->self) snprintf(c->name, sizeof c->name, "%s", s->peer[c->member].name);
        for (int k = 0; k < s->npools; k++) {
            const Pool *p = &s->pools[k];
            const Rec *r = NULL;
            for (int j = 0; j < p->nrec; j++)
                if (p->rec[j].kid == kid) r = &p->rec[j];
            if (!r) continue;
            // per pool, what its spends took (up to its gains plus the limit and what entered the pool) beyond its own
            // gains: gains it signs for one pool hide nothing it took from another
            int at = p->cat == COOP_CAT_KEYS ? 0 : p->cat == COOP_CAT_RUPEES ? 1 : 2;
            int64_t lim = p->max < POOL_REG_MAX ? p->max : POOL_REG_MAX, took = r->spend, in = pool_sum(s, p, NULL, 0, 0).in;
            if (took > (int64_t)r->gain + lim) took = (int64_t)r->gain + lim;
            if (took > in) took = in;
            if (took > (int64_t)r->gain) c->spent[at] += took - (int64_t)r->gain;
        }
    }
    return n;
}

// ---- the offer

int coop_sync_offer(CoopSync *s, CoopOffer *o, uint64_t now) {
    memset(o, 0, sizeof *o);
    if (now > s->now) s->now = now;
    if (!s->in_room) return 0;
    int waiting = 0;
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
        const Peer *p = &s->peer[id];
        if (!p->present) continue;
        CoopOfferMember *m = &o->member[o->n++];
        int ident = p->ident && !disputed(s, p);   // two keys for its session: identified as neither
        m->id = id, m->heard = ident, m->removed = ident && removed(s, p->kid);
        m->maker = ident && s->have_room && p->kid == s->ch.kid;
        if (ident) {
            snprintf(m->name, sizeof m->name, "%s", p->name);
            fingerprint(p->kid, m->key);
            int a = acc_index(s, p->kid);
            if (a >= 0 && strcmp(s->acc_name[a], p->raw))
                coop_name_clean(m->accepted_as, sizeof m->accepted_as, (const uint8_t *)s->acc_name[a], strlen(s->acc_name[a]));
        } else if (!p->ident && s->now - p->seen_at < IDENT_WAIT_MS) {
            waiting = 1;
        } else {
            o->unidentified++;   // present and silent about who it is: the room cannot be accepted while it is there
        }
    }
    int differ, recent;
    members_signer(s, s->have_room ? s->ch.kid : 0, &differ, &recent);
    o->disagree = differ;
    if (!s->have_room) return 0;
    o->rules = s->room;
    o->rules.maker = (uint8_t)member_of_kid(s, s->ch.kid);
    fingerprint(s->ch.kid, o->maker_key);
    o->maker_moved = s->maker && s->maker != s->self && s->peer[s->maker].ident && s->peer[s->maker].kid != s->ch.kid;
    o->new_save = s->accepted && s->save_gen != s->acc_save_gen;
    o->copy = s->copy_gen == s->save_gen ? s->copy : 0;
    if (waiting) return 0;
    // the number: keyed with this start's random value, over everything the acceptance covers (each key with the name it
    // signed: a known key under another name is another offer)
    const Peer *who[COOP_MAX_MEMBERS];
    int nw = 0;
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++)
        if (s->peer[id].present && s->peer[id].ident && !disputed(s, &s->peer[id]) && !removed(s, s->peer[id].kid)) who[nw++] = &s->peer[id];
    for (int i = 1; i < nw; i++)
        for (int j = i; j > 0 && who[j]->kid < who[j - 1]->kid; j--) {
            const Peer *t = who[j];
            who[j] = who[j - 1], who[j - 1] = t;
        }
    uint8_t m[COOP_SECRET_TEXT + (9 + COOP_NAME_MAX) * COOP_MAX_MEMBERS + 8 + COOP_CAT_N + 4], *at = m;
    putn(&at, s->code, strlen(s->code));
    for (int i = 0; i < nw; i++) {
        size_t nl = strlen(who[i]->raw);
        put64(&at, who[i]->kid);
        put8(&at, (uint8_t)nl);
        putn(&at, who[i]->raw, nl);
    }
    put64(&at, s->ch.kid);
    putn(&at, s->room.mode, COOP_CAT_N);
    put32(&at, s->save_gen);
    uint64_t h = coop_siphash24(s->offer_k0, s->offer_k1, m, (size_t)(at - m));
    o->token = (uint32_t)(h ^ h >> 32) | 1;
    int ok = s->accepted && s->have_pin && s->pin_kid == s->ch.kid && !memcmp(s->acc_mode, s->room.mode, COOP_CAT_N) &&
             s->acc_save_gen == s->save_gen && !o->unidentified;
    for (int i = 0; i < nw && ok; i++) {
        int a = acc_index(s, who[i]->kid);
        ok = a >= 0 && !strcmp(s->acc_name[a], who[i]->raw);
    }
    o->accepted = ok;
    return 1;
}

static void keep_ended_of(CoopSync *s, uint64_t kid);

int coop_sync_accept(CoopSync *s, uint32_t token) {
    CoopOffer o;
    if (!token || !coop_sync_offer(s, &o, s->now) || o.token != token || o.unidentified) return 0;
    if (!s->accepted) s->nacc = 0;
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
        const Peer *p = &s->peer[id];
        if (!p->present || !p->ident || removed(s, p->kid)) continue;
        int a = acc_index(s, p->kid);
        if (a < 0) {
            if (s->nacc == MAX_ACCEPTED) {   // full: the members of this offer are what matters now
                s->nacc = 0;
                id = 0;
                continue;
            }
            a = s->nacc++;
            s->acc_kids[a] = p->kid;
            keep_ended_of(s, p->kid);
        }
        snprintf(s->acc_name[a], sizeof s->acc_name[a], "%s", p->raw);   // accepted as the name it signed now
        s->acc_id[a] = (uint8_t)id;
    }
    memcpy(s->acc_mode, s->room.mode, COOP_CAT_N);
    s->pin_kid = s->ch.kid, s->have_pin = 1;
    s->acc_save_gen = s->save_gen;
    s->accepted = 1;
    if (s->copy_gen != s->save_gen || s->copy != 1) s->copy = 0, s->copy_wanted = 1;
    for (int k = 0; k < s->npools; k++) s->pools[k].recheck = 1, pool_check(s, &s->pools[k]);   // the pools count the keys accepted now
    tracef(s, "the room is accepted; its rules' key is pinned");
    return 1;
}

int coop_sync_copy_wanted(const CoopSync *s) { return s->copy_wanted; }

void coop_sync_copied(CoopSync *s, int ok) {
    s->copy_wanted = 0;
    s->copy = ok ? 1 : -1;
    s->copy_gen = s->save_gen;
}

int coop_sync_open(CoopSync *s) {
    CoopOffer o;
    return coop_sync_offer(s, &o, s->now) && o.accepted && s->copy == 1 && s->copy_gen == s->save_gen;
}

// ---- the room

// A (key, session) pair whose session ended: an identity of that key signed for that session is never taken again.
// The latest pairs of each key this player accepted are kept apart, where no other key's churn pushes them out; the
// other keys share a ring.
static Kept *kept_of(const CoopSync *s, uint64_t kid) {
    for (int i = 0; i < s->nkept; i++)
        if (s->kept[i].kid == kid) return (Kept *)&s->kept[i];
    return NULL;
}

static void kept_add(CoopSync *s, uint64_t kid, uint64_t session) {
    Kept *k = kept_of(s, kid);
    if (!k) {
        if (s->nkept == MAX_ACCEPTED) return;
        k = &s->kept[s->nkept++];   // its ring of an earlier room is reused
        k->kid = kid, k->n = 0;
    }
    if (k->n == k->cap && k->cap < KEPT_PER_KEY) {
        unsigned nc = k->cap ? k->cap * 2 : 16;
        uint64_t *ns = realloc(k->sess, nc * sizeof *ns);
        if (ns) k->sess = ns, k->cap = nc;
    }
    if (k->cap) k->sess[k->n++ % k->cap] = session;
}

static void end_pair(CoopSync *s, uint64_t kid, uint64_t session) {
    if (in_list(s->acc_kids, s->nacc, kid) && (kept_of(s, kid) || s->nkept < MAX_ACCEPTED)) {
        kept_add(s, kid, session);
        return;
    }
    if (s->nended == s->ended_cap && s->ended_cap < ENDED_MAX) {
        unsigned nc = s->ended_cap ? s->ended_cap * 2 : 64;
        Ended *ne = realloc(s->ended, nc * sizeof *ne);
        if (ne) s->ended = ne, s->ended_cap = nc;
    }
    if (s->ended_cap) s->ended[s->nended++ % s->ended_cap] = (Ended){kid, session};
}

// The ring's pairs of a key the player accepts now go to its own (it had none: the pairs of an accepted key go there).
static void keep_ended_of(CoopSync *s, uint64_t kid) {
    unsigned n = s->nended < s->ended_cap ? s->nended : s->ended_cap;
    for (unsigned i = 0; i < n; i++)
        if (s->ended[i].kid == kid) kept_add(s, kid, s->ended[i].session);
}

static int session_ended(const CoopSync *s, uint64_t kid, uint64_t session) {
    const Kept *k = kept_of(s, kid);
    for (unsigned i = 0; k && i < k->n && i < k->cap; i++)
        if (k->sess[i] == session) return 1;
    unsigned n = s->nended < s->ended_cap ? s->nended : s->ended_cap;
    for (unsigned i = 0; i < n; i++)
        if (s->ended[i].session == session && s->ended[i].kid == kid) return 1;
    return 0;
}

void coop_sync_room(CoopSync *s, const char *code, int self, const int *ids, const uint64_t *sessions, int n, int maker, uint64_t now) {
    if (!code) code = "";
    if (now > s->now) s->now = now;
    if (strcmp(code, s->code)) {
        int same = code[0] && !strcmp(code, s->acc_code);
        snprintf(s->code, sizeof s->code, "%s", code);
        s->in_room = code[0] != 0;
        s->caught_up = 0;
        memset(s->peer, 0, sizeof s->peer);
        s->joined_at = now, s->originated_at = 0;
        s->rules_ver++;
        // everything merged belongs to the last room: nothing of it (flags, the save it came from) reaches the next one.
        // This game's own pool records stay while the room may come back (out of it, or back in the same one): they are
        // signed with its key for that room, and the room holds them
        drop_state(s, !code[0] || same);
        if (code[0] && !same) {   // another room: its own acceptance, charter, removals and sessions
            snprintf(s->acc_code, sizeof s->acc_code, "%s", code);
            s->have_room = 0, s->accepted = 0, s->have_pin = 0, s->nacc = 0, s->copy = 0, s->copy_wanted = 0, s->copy_gen = UINT32_MAX;
            memset(&s->ch, 0, sizeof s->ch);
            memset(&s->room, 0, sizeof s->room);
            s->nended = 0, s->nkept = 0, s->ngone = 0;
            s->self_session = s->prev_session = 0;   // a session of another room is none of this one's
        }
        if (code[0]) derive(s);
        s->next_digest = now;
    }
    s->self = self;
    s->maker = maker >= 1 && maker <= COOP_MAX_MEMBERS ? maker : 0;
    int seen[COOP_MAX_MEMBERS + 1] = {0};
    uint64_t sess[COOP_MAX_MEMBERS + 1] = {0};
    for (int i = 0; i < n; i++) {
        if (ids[i] < 1 || ids[i] > COOP_MAX_MEMBERS) continue;
        uint64_t v = sessions ? sessions[i] : (uint64_t)ids[i];
        if (ids[i] == self) {
            // this game's own session (the one it joined with); the one before it is named in its next identity, which
            // goes to everyone at once, so that the others take it and end that session
            if (v != s->self_session) s->prev_session = s->self_session, s->self_session = v, s->id_valid = 0, s->digest_to |= 1;
            continue;
        }
        seen[ids[i]] = 1, sess[ids[i]] = v;
    }
    // A session ends at this game only when its member's identity in a later one names it (on_digest): what a roster
    // says, and that the relay lost the room, anyone on the path can say too, so a member that leaves the roster or
    // comes back in another session is only met again, and a session ended by a roster could never be taken again
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
        Peer *p = &s->peer[id];
        if (seen[id] && (!p->present || p->session != sess[id])) {
            memset(p, 0, sizeof *p);
            p->present = 1, p->session = sess[id], p->seen_at = now;
            s->digest_to |= 1ull << id;   // a member who just joined hears this player's state at once
        } else if (!seen[id] && p->present) {
            memset(p, 0, sizeof *p);
        }
    }
    update_caught_up(s);
}

// ---- messages

typedef struct {
    uint8_t buf[MSG_CAP];
    uint8_t *at;
    int n;
} Msg;

static void msg_begin(CoopSync *s, Msg *m) {
    m->at = m->buf;
    put8(&m->at, COOP_K_DIGEST_VER);
    put32(&m->at, s->schema);
    put8(&m->at, 0);
    m->n = 0;
}

static int msg_room(Msg *m, size_t size) { return (size_t)(m->at - m->buf) + size + SEC_TAIL <= sizeof m->buf && m->n < 255; }

static int msg_add_flag(Msg *m, const Reg *r) {
    if (!msg_room(m, FLAG_SIZE)) return 0;
    put8(&m->at, COOP_R_FLAG);
    put8(&m->at, r->cat);
    put32(&m->at, r->hash);
    put32(&m->at, r->s);
    m->n++;
    return 1;
}

static int msg_add_rec(Msg *m, const Pool *p, const Rec *r) {
    if (!msg_room(m, REC_SIZE)) return 0;
    put8(&m->at, COOP_R_POOL);
    put8(&m->at, p->cat);
    put32(&m->at, p->hash);
    putn(&m->at, r->pk, COOP_PK_LEN);
    put32(&m->at, r->epoch);
    put32(&m->at, r->seq);
    put32(&m->at, base_encode(r->base));
    put32(&m->at, r->gain);
    put32(&m->at, r->spend);
    putn(&m->at, r->sig, COOP_SIG_LEN);
    m->n++;
    return 1;
}

// What a section's signature covers before its bytes: the role, the room's context, the sender's member id and session.
static size_t section_prefix(const CoopSync *s, int id, uint64_t session, uint8_t m[SEC_PRE]) {
    uint8_t *at = m;
    putn(&at, SIG_SECTION, sizeof SIG_SECTION - 1);
    putn(&at, s->ctx, CTX_LEN);
    put8(&at, (uint8_t)id);
    put64(&at, session);
    return (size_t)(at - m);
}

// The section in b (n bytes, cap at most) gets this game's next counter and its signature for its member id and session:
// every member checks it before anything of it is stored, applied or passed on, so that a message key (which every
// holder of the room's secret has) makes no section in a member's name. One signature for the whole message, whatever
// it carries. The new length, 0 when it does not fit.
static size_t section_seal(CoopSync *s, uint8_t *b, size_t n, size_t cap) {
    if (n + SEC_TAIL > cap || n > MSG_CAP) return 0;
    uint8_t *at = b + n, m[SEC_PRE + MSG_CAP];
    put64(&at, ++s->sec_counter);
    size_t pl = section_prefix(s, s->self, s->self_session, m);
    memcpy(m + pl, b, n + 8);
    coop_sign(at, s->sk, m, pl + n + 8);
    return n + SEC_TAIL;
}

size_t coop_sync_sign_section(CoopSync *s, uint8_t *msg, size_t n, size_t cap) { return section_seal(s, msg, n, cap); }

static int msg_send(CoopSync *s, Msg *m, int to, CoopSyncSendFn send, void *user) {
    if (!m->n) return 1;
    m->buf[5] = (uint8_t)m->n;
    size_t len = section_seal(s, m->buf, (size_t)(m->at - m->buf), sizeof m->buf);
    int ok = len && send(to, COOP_K_SECTION, m->buf, len, user);
    if (ok) s->st.sent_msgs++, s->st.sent_bytes += (unsigned)len;
    s->tokens -= (int64_t)len;
    return ok;
}

static void trace_flag(CoopSync *s, const char *verb, const Reg *r, uint32_t v, int member) {
    if (s->trace) tracef(s, "%s %s %08x = %u %s %s%.0d", verb, cat_names[r->cat], r->hash, v, *verb == 's' ? "to" : "from", member ? "member " : "all", member);
}

static void trace_rec(CoopSync *s, const char *verb, const Pool *p, const Rec *r, int member) {
    if (s->trace)
        tracef(s, "%s %s %08x of member %d: base %d gain %u spend %u %s %s%.0d", verb, cat_names[p->cat], p->hash, member_of_kid(s, r->kid), r->base,
               r->gain, r->spend, *verb == 's' ? "to" : "from", member ? "member " : "all", member);
}

static void send_digest(CoopSync *s, int to, CoopSyncSendFn send, void *user) {
    if (!s->id_valid) {   // this player's identity, signed for its member id and session in this room and the one before
        uint8_t m[128];
        coop_sign(s->id_sig, s->sk, m, identity_message(s, s->self, s->self_session, s->prev_session, s->pk, s->name, m));
        s->id_valid = 1;
    }
    uint8_t buf[COOP_MAX_MESSAGE], *p = buf;
    size_t nl = strlen(s->name);
    put8(&p, COOP_K_DIGEST_VER);
    put32(&p, s->schema);
    putn(&p, s->pk, COOP_PK_LEN);
    put64(&p, s->prev_session);
    put8(&p, (uint8_t)nl);
    putn(&p, s->name, nl);
    putn(&p, s->id_sig, COOP_SIG_LEN);
    put8(&p, (uint8_t)s->have_room);
    if (s->have_room) {
        p += charter_fields(&s->ch, p);
        putn(&p, s->ch.sig, COOP_SIG_LEN);
    }
    put8(&p, COOP_SYNC_SECTIONS);
    for (int i = 0; i < COOP_SYNC_SECTIONS; i++) put32(&p, s->sums[i]);
    if (send(to, COOP_K_DIGEST, buf, (size_t)(p - buf), user)) s->st.sent_msgs++, s->st.sent_bytes += (unsigned)(p - buf);
}

static void on_digest(CoopSync *s, int from, const uint8_t *b, size_t n, uint64_t now) {
    if (n < 5 + COOP_PK_LEN + 9 || b[0] != COOP_K_DIGEST_VER || get32(b + 1) != s->schema) return;
    Peer *p = &s->peer[from];
    size_t at = 5;
    const uint8_t *pk = b + at;
    uint64_t prev = get64(b + at + COOP_PK_LEN);
    size_t nl = b[at + COOP_PK_LEN + 8];
    at += COOP_PK_LEN + 9;
    if (nl > COOP_NAME_MAX || at + nl + COOP_SIG_LEN + 1 > n) return;
    char name[NAME_CAP];
    coop_name_clean(name, sizeof name, b + at, nl);
    const uint8_t *name_raw = b + at, *sig = b + at + nl;
    at += nl + COOP_SIG_LEN;
    uint64_t kid = kid_of(pk);
    if (p->ident && kid == p->kid && !memcmp(p->raw, name_raw, nl) && !p->raw[nl]) {
        p->ident_at = now;   // the identity held, presented again
    } else {
        // an identity is bound to one session: a session of that key that ended, a key another member holds, or the key
        // held under another name is not taken. Another key for the same session is one of two that cannot both be
        // the member's: it takes the place only once the one held was not presented for IDENT_HOLD_MS, and until then
        // the two are disputed (neither counts) while both keep coming. So whoever holds the room's secret and is first
        // with an identity of its own key for a member's session cuts that member off only while it keeps sending
        int rival = p->ident && kid != p->kid;
        if (p->ident && !rival) return;
        if (session_ended(s, kid, p->session) || (kid != s->kid && member_of_kid(s, kid))) return;
        char raw[NAME_CAP];
        memcpy(raw, name_raw, nl), raw[nl] = 0;
        if (strlen(raw) != nl) return;
        // checked over the bytes as signed; shown cleaned, as the relay's names are. A digest is not signed by its sender:
        // an identity that does not check out is dropped and counted, never held against the member
        uint8_t m[128];
        int ok = sig_check(s, p, sig, pk, m, identity_message(s, from, p->session, prev, pk, raw, m));
        if (!ok) s->st.forged++;
        if (ok != 1) return;
        if (kid == s->kid) {   // signed with this game's own key: another game holds a copy of its key file
            s->st.own_key++, s->st.own_key_member = from;
            return;
        }
        if (rival && now - p->ident_at < IDENT_HOLD_MS) {
            if (!disputed(s, p)) {
                tracef(s, "member %d: another key's identity for its session; neither counts while both are presented", from);
                s->st.key_changes++, s->st.key_change_member = from;
            }
            p->rival_at = now;
            return;
        }
        if (rival) {   // the key held was not presented for a while: what came with it is forgotten, its budgets stay
            Peer q = *p;
            memset(p, 0, sizeof *p);
            p->present = q.present, p->session = q.session, p->seen_at = q.seen_at, p->tokens = q.tokens, p->tok_at = q.tok_at;
            p->sig_tokens = q.sig_tokens, p->sig_at = q.sig_at, p->bad_sig = q.bad_sig;
            tracef(s, "member %d: the key held for its session was not presented for %d s; another key's identity takes its place", from,
                   IDENT_HOLD_MS / 1000);
            s->st.key_changes++, s->st.key_change_member = from;
        }
        p->ident = 1, p->kid = kid, p->ident_at = now;
        memcpy(p->pk, pk, COOP_PK_LEN);
        memcpy(p->raw, raw, nl + 1);
        memcpy(p->name, name, sizeof name);
        tracef(s, "member %d is identified (%s)", from, name);
        // a key this player accepted, now as another member number or under another name than it had: said, and under
        // another name it is a new offer (coop_sync_offer), so that a name the player knows never carries another key
        int a = acc_index(s, kid);
        if (a >= 0 && ((s->acc_id[a] && s->acc_id[a] != from) || strcmp(s->acc_name[a], raw)))
            s->st.key_moves++, s->st.key_move_member = from, s->st.key_move_from = s->acc_id[a];
        if (a >= 0) s->acc_id[a] = (uint8_t)from;
        // the session its identity names as the one before ends here: only the key's holder says so, so neither a
        // roster nor a replayed message or identity ends a session the member still holds
        if (prev && prev != p->session) end_pair(s, kid, prev);
    }
    Charter c;
    int has = b[at++];
    size_t cl = has ? charter_parse(b + at, n - at, &c) : 0;
    if (has && !cl) return;
    at += cl;
    p->ch_kid = has ? c.kid : 0, p->ch_seen = 1;
    if (removed(s, p->kid)) return;
    if (has) consider_charter(s, from, &c);
    if (p->ignored) return;
    if (at + 1 > n) return;
    size_t nsec = b[at++];
    if (nsec != COOP_SYNC_SECTIONS || at + 4 * nsec > n) return;
    p->have_digest = 1;
    int differs = 0;
    for (int i = 0; i < COOP_SYNC_SECTIONS; i++) {
        p->sums[i] = get32(b + at + 4 * (size_t)i);
        if (p->sums[i] != s->sums[i]) {
            differs = 1;
            if (now - p->answered_at[i] >= ANSWER_MS || !p->answered_at[i]) p->want |= 1ull << i;
        }
    }
    // the peer may hold what this player lacks: a digest back lets it answer at once instead of at its next one
    if (differs && now - p->digest_at >= ANSWER_MS) s->digest_to |= 1ull << from, p->digest_at = now;
    update_caught_up(s);
}

// A flag this game refuses: counted, and against its sender, since an honest member with the same schema refuses it
// too and never passes it on.
static void refuse(CoopSync *s, int from, const char *why, uint32_t hash) {
    Peer *p = &s->peer[from];
    s->st.rejected++;
    p->bad++;
    tracef(s, "refused from member %d: %s %08x (%u so far)", from, why, hash, p->bad);
    if (p->bad >= BAD_MAX && !p->ignored) {
        p->ignored = 1;
        tracef(s, "member %d is ignored while it stays in the room", from);
    }
}

static void on_record(CoopSync *s, int from, const uint8_t *e) {
    uint8_t cat = e[1];
    uint32_t hash = get32(e + 2);
    Rec r;
    memset(&r, 0, sizeof r);
    memcpy(r.pk, e + 6, COOP_PK_LEN);
    const uint8_t *f = e + 6 + COOP_PK_LEN;
    r.epoch = get32(f), r.seq = get32(f + 4), r.base = base_decode(get32(f + 8)), r.gain = get32(f + 12), r.spend = get32(f + 16);
    memcpy(r.sig, f + 20, COOP_SIG_LEN);
    // what this game cannot count is dropped, never held against the sender: an honest member may hold a newer
    // charter (another admission or pools' epoch) than this game for a moment
    if (!s->have_room || !coop_cat_is_pool(cat) || s->room.mode[cat] != COOP_MODE_POOL || !limit_of(s, cat, hash) || r.epoch != s->ch.pool_epoch ||
        r.base < -POOL_REG_MAX || r.base > POOL_REG_MAX || r.gain > POOL_REG_MAX || r.spend > POOL_REG_MAX) {
        s->st.rejected++;
        return;
    }
    r.kid = kid_of(r.pk);
    if (!admitted(s, r.kid)) {
        s->st.rejected++;
        return;
    }
    Pool *p = pool_get(s, hash, cat, 1);
    if (!p || p->cat != cat) return;
    Rec *old = rec_of(p, r.kid);
    // the highest sequence wins, so the newest record of its owner; this game's own record too, which the room may hold
    // newer than this game does (the room came back, or the maker's game started again with the room's key)
    if (old && (r.seq < old->seq || (r.seq == old->seq && (r.kid == s->kid || memcmp(r.sig, old->sig, COOP_SIG_LEN) <= 0)))) return;
    if (!old && p->nrec == MAX_ADMIT) {
        s->st.rejected++;
        return;
    }
    uint8_t m[96];
    int ok = sig_check(s, &s->peer[from], r.sig, r.pk, m, rec_message(s, p, &r, m));
    if (!ok) refuse(s, from, "a pool record not signed by its contributor for", hash);   // an honest member checks before it passes one on
    if (ok != 1) return;
    trace_rec(s, "received", p, &r, from);
    rec_set(s, p, old ? (int)(old - p->rec) : p->nrec, &r);
    s->st.merged++;
}

// A section's counter is taken once in its sender's session, and at most SEC_WINDOW below the highest taken (as coop.c
// takes its envelopes): whoever holds the room's message key can seal a member's old section again under a new
// envelope counter, and this refuses it.
static int sec_fresh(const Peer *p, uint64_t c) {
    if (!p->sec_valid || c > p->sec_high) return 1;
    return p->sec_high - c < SEC_WINDOW && !(p->sec_bits >> (p->sec_high - c) & 1);
}

static void sec_take(Peer *p, uint64_t c) {
    if (!p->sec_valid) {
        p->sec_valid = 1, p->sec_high = c, p->sec_bits = 1;
    } else if (c > p->sec_high) {
        p->sec_bits = c - p->sec_high >= SEC_WINDOW ? 1 : p->sec_bits << (c - p->sec_high) | 1;
        p->sec_high = c;
    } else {
        p->sec_bits |= 1ull << (p->sec_high - c);
    }
}

static void on_section(CoopSync *s, int from, const uint8_t *b, size_t n) {
    if (n < SYNC_HDR + SEC_TAIL || n > MSG_CAP || b[0] != COOP_K_DIGEST_VER || get32(b + 1) != s->schema) return;
    Peer *p = &s->peer[from];
    // the sender's signature over its member id, session, counter and bytes, checked before anything of it counts: a
    // section that does not check out was made in its name by someone else (anyone with the room's secret can seal
    // one), so it is dropped and counted, and it costs that member's budget of checks, not its standing
    size_t body = n - COOP_SIG_LEN;
    uint64_t counter = get64(b + body - 8);
    if (!sec_fresh(p, counter)) {
        s->st.replayed++;
        return;
    }
    uint8_t m[SEC_PRE + MSG_CAP];
    size_t pl = section_prefix(s, from, p->session, m);
    memcpy(m + pl, b, body);
    int ok = sig_check(s, p, b + body, p->pk, m, pl + body);
    if (ok != 1) {
        if (!ok) s->st.forged++;
        return;
    }
    sec_take(p, counter);
    n = body - 8;
    size_t count = b[5], at = SYNC_HDR;
    // flags only from a key this player accepted in this room: nothing of a member it did not accept is stored, so
    // nothing of it is passed on or applied later (when that member leaves or is removed). Pool records are stored as
    // the maker admits them (on_record), and a pool counts those of the keys this player accepted (pool_sum).
    int take = s->accepted && in_list(s->acc_kids, s->nacc, p->kid);
    for (size_t i = 0; i < count && !p->ignored; i++) {
        if (at + 2 > n) return;
        uint8_t kind = b[at], cat = b[at + 1];
        if ((kind != COOP_R_FLAG && kind != COOP_R_POOL) || cat >= COOP_CAT_N) return;
        size_t sz = kind == COOP_R_FLAG ? FLAG_SIZE : REC_SIZE;
        if (at + sz > n) return;
        const uint8_t *e = b + at;
        at += sz;
        if (!s->limit) continue;   // the game's table is not known yet
        if (kind == COOP_R_POOL) {
            on_record(s, from, e);
            continue;
        }
        uint32_t hash = get32(e + 2), v = get32(e + 6);
        if (!s->have_room || s->room.mode[cat] != COOP_MODE_SHARED) continue;
        if (!take) {
            s->st.unaccepted++;
            continue;
        }
        // checked before anything is stored or passed on: a hash this game shares in that category, a value it takes
        uint32_t max = limit_of(s, cat, hash);
        if (coop_cat_is_pool(cat) || !max) {
            refuse(s, from, "not a value this game shares", hash);
            continue;
        }
        if (v > max) {
            refuse(s, from, "a value out of range for", hash);
            continue;
        }
        int idx = find_reg(s, hash);
        if (idx >= 0 && s->regs[idx].cat != cat) continue;   // classified differently here: this game's view wins
        if (idx >= 0 && v <= s->regs[idx].s) continue;
        if (idx < 0 && !v) continue;
        if (idx < 0 && (idx = reg_get(s, cat, hash)) < 0) {
            s->st.rejected++;   // the state is full
            continue;
        }
        trace_flag(s, "received", &s->regs[idx], v, from);
        flag_raise(s, idx, v);
        s->st.merged++;
    }
    update_caught_up(s);
}

void coop_sync_receive(CoopSync *s, int from, uint8_t kind, const uint8_t *bytes, size_t n, uint64_t now) {
    if (from < 1 || from > COOP_MAX_MEMBERS || from == s->self || !s->in_room) return;
    if (now > s->now) s->now = now;
    Peer *p = &s->peer[from];
    if (!p->present || p->ignored) return;
    if (!p->tok_at) p->tok_at = now, p->tokens = PEER_BURST;   // a member's messages are taken at PEER_RATE
    if (now > p->tok_at) p->tokens += (int64_t)(now - p->tok_at) * PEER_RATE / 1000, p->tok_at = now;
    if (p->tokens > PEER_BURST) p->tokens = PEER_BURST;
    if (p->tokens < (int64_t)n) {
        s->st.rate_dropped++;
        return;
    }
    p->tokens -= (int64_t)n;
    s->st.recv_msgs++;
    if (kind == COOP_K_DIGEST) on_digest(s, from, bytes, n, now);
    else if (kind == COOP_K_SECTION && peer_counts(s, p)) on_section(s, from, bytes, n);   // values only from an identity
}

// One section's synced registers to member to; 0 when the sender refused a message (the outbox is full).
static int send_section(CoopSync *s, int sec, int to, CoopSyncSendFn send, void *user) {
    Msg m;
    msg_begin(s, &m);
    for (int i = 0; i < s->nregs; i++) {
        const Reg *r = &s->regs[i];
        if (!r->s || flag_sec(r) != sec || !flag_synced(s, r)) continue;
        if (!msg_add_flag(&m, r)) {
            if (!msg_send(s, &m, to, send, user)) return 0;
            msg_begin(s, &m);
            msg_add_flag(&m, r);
        }
        trace_flag(s, "sent", r, r->s, to);
    }
    for (int k = 0; k < s->npools; k++) {
        const Pool *p = &s->pools[k];
        if (!pool_synced(s, p)) continue;
        for (int i = 0; i < p->nrec; i++) {
            if (rec_sec(p, &p->rec[i]) != sec) continue;
            if (!msg_add_rec(&m, p, &p->rec[i])) {
                if (!msg_send(s, &m, to, send, user)) return 0;
                msg_begin(s, &m);
                msg_add_rec(&m, p, &p->rec[i]);
            }
        }
    }
    return msg_send(s, &m, to, send, user);
}

static void pool_recheck(CoopSync *s, Pool *p, uint64_t now);

void coop_sync_tick(CoopSync *s, uint64_t now, CoopSyncSendFn send, void *user) {
    if (now > s->now) s->now = now;
    if (!s->tok_at) s->tok_at = now, s->tokens = BURST_BYTES;
    s->tokens += (int64_t)(now - s->tok_at) * RATE_BYTES / 1000;
    if (s->tokens > BURST_BYTES) s->tokens = BURST_BYTES;
    s->tok_at = now;
    if (!s->in_room) return;

    // the charter: the maker's game signs a new one when its settings or the admissions changed (once a second)
    if (is_maker(s) && charter_stale(s) && (!s->originated_at || now - s->originated_at >= ORIGINATE_MS)) {
        s->originated_at = now ? now : 1;
        originate(s);
    }
    update_caught_up(s);

    // digests: every DIGEST_MS to all, at once to whoever needs one
    if (now >= s->next_digest) {
        s->digest_to |= 1;
        s->rng = mix64(s->rng);
        s->next_digest = now + DIGEST_MS + s->rng % 400;
    }
    if (s->digest_to & 1) send_digest(s, 0, send, user);
    else
        for (int id = 1; id <= COOP_MAX_MEMBERS; id++)
            if (s->digest_to >> id & 1 && s->peer[id].present) send_digest(s, id, send, user);
    s->digest_to = 0;
    if (!s->have_room) return;

    // a claim waiting to be made again (a removal, or the player accepting more keys) is made now, also while the game
    // reports no pool counter (its title screen, a load): until then the others count this player's keys as the old
    // claim had them, and a pool reset in that time would carry them for nobody but this game
    for (int k = 0; k < s->npools; k++)
        if (s->pools[k].recheck) pool_recheck(s, &s->pools[k], now), pool_check(s, &s->pools[k]);

    // local changes: three sends, 0, 80 and 250 ms after the change
    Msg m;
    msg_begin(s, &m);
    unsigned keep = s->push_tail;
    while (s->push_head != keep && s->tokens > 0) {
        Push *q = &s->push[s->push_head % PUSH_SLOTS];
        if (q->due > now) {   // not yet: to the back of the line (the line stays in order of due time per round)
            s->push[s->push_tail++ % PUSH_SLOTS] = *q;
            s->push_head++;
            continue;
        }
        int ok;
        if (q->idx >= 0) {
            const Reg *r = &s->regs[q->idx];
            if (!(ok = msg_add_flag(&m, r))) msg_send(s, &m, 0, send, user), msg_begin(s, &m), msg_add_flag(&m, r);
            if (!q->round) trace_flag(s, "sent", r, r->s, 0);
        } else {
            int code = -1 - q->idx;
            const Pool *p = &s->pools[code / MAX_ADMIT];
            if (code / MAX_ADMIT < s->npools && code % MAX_ADMIT < p->nrec) {
                const Rec *r = &p->rec[code % MAX_ADMIT];
                if (!(ok = msg_add_rec(&m, p, r))) msg_send(s, &m, 0, send, user), msg_begin(s, &m), msg_add_rec(&m, p, r);
                if (!q->round) trace_rec(s, "sent", p, r, 0);
            }
        }
        if (q->round < 2) s->push[s->push_tail++ % PUSH_SLOTS] = (Push){q->idx, (uint8_t)(q->round + 1), q->first + (q->round ? 250 : 80), q->first};
        s->push_head++;
    }
    msg_send(s, &m, 0, send, user);

    // sections a peer's digest showed to differ, paced: one section to each peer that wants one, in turn, from a first
    // peer that moves on at every tick, so a member that asks for everything all the time gets its share of the budget
    // and the others theirs, and a late joiner catches up beside it
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++)
        if (!peer_counts(s, &s->peer[id])) s->peer[id].want = 0;
    int first = s->turn;
    s->turn = (s->turn + 1) % COOP_MAX_MEMBERS;
    for (int served = 1; served && s->tokens > 0;) {
        served = 0;
        for (int k = 0; k < COOP_MAX_MEMBERS && s->tokens > 0; k++) {
            int id = 1 + (first + k) % COOP_MAX_MEMBERS;
            Peer *p = &s->peer[id];
            if (!p->want) continue;
            int sec = __builtin_ctzll(p->want);
            if (!send_section(s, sec, id, send, user)) return;   // the outbox is full: next tick
            p->want &= p->want - 1;
            p->answered_at[sec] = now ? now : 1;
            served = 1;
        }
    }
}

// ---- the local game

void coop_sync_rebind(CoopSync *s) {
    // the merged state holds the last save's progress: forgotten, so that none of it reaches this save; the game
    // reports its flags again and the room sends what it holds within a few digests. Another save than the first one
    // played is a new offer: the room's progress holds what the last save brought
    if (s->bound_once) s->save_gen++;
    s->bound_once = 1;
    drop_state(s, 1);
    s->caught_up = 0;
    s->digest_to |= 1;
}

void coop_sync_local(CoopSync *s, int cat, uint32_t hash, uint32_t value, uint64_t now) {
    if (cat < 0 || cat >= COOP_CAT_N || coop_cat_is_pool(cat)) return;
    if (now > s->now) s->now = now;
    int idx = find_reg(s, hash);
    if (idx < 0) {
        if (!value || (idx = reg_get(s, cat, hash)) < 0) return;
    }
    Reg *r = &s->regs[idx];
    r->l = value;
    if (value > r->s) {
        flag_raise(s, idx, value);
        if (flag_synced(s, r)) push_code(s, idx, now);
    }
}

static void contribute(CoopSync *s, Pool *p, int spend, uint32_t add, uint64_t now) {
    Rec *mine = rec_of(p, s->kid);
    if (!mine) return;
    Rec r = *mine;
    uint32_t *v = spend ? &r.spend : &r.gain;
    uint64_t nv = (uint64_t)*v + add;   // saturates: never wraps to a small value
    if (nv > POOL_REG_MAX) nv = POOL_REG_MAX;
    if (nv == *v) return;
    *v = (uint32_t)nv;
    own_record(s, p, &r, now);
}

// The records this game counts changed (a removal dropped some, or its player accepted more keys): its claim is made
// again as if it had counted that way when it joined, up or down. So keys this player brought while a removed player's
// gains were in the pool are not absorbed into them, and a claim made while a removed player's spends were in it does
// not leave the pool above what the others brought. Only this game's own record changes, and it is a function of the
// join's view and what the game counts, so games that count the same records still agree.
static void pool_recheck(CoopSync *s, Pool *p, uint64_t now) {
    p->recheck = 0;
    Rec *mine = rec_of(p, s->kid);
    if (!p->joins.n || p->joins.epoch != s->ch.pool_epoch || !mine || mine->epoch != s->ch.pool_epoch) return;
    int32_t base;
    uint32_t debt, joined = p->joins.debt < mine->gain ? p->joins.debt : mine->gain;
    claims_now(s, p, NULL, 0, &base, &debt);
    Rec r = *mine;
    uint64_t gain = (uint64_t)r.gain - joined + debt;
    r.base = base, r.gain = (uint32_t)(gain < POOL_REG_MAX ? gain : POOL_REG_MAX);
    if (r.base == mine->base && r.gain == mine->gain) return;
    p->joins.debt = debt;
    tracef(s, "pool %s %08x: this game's claim made again for the records it counts now: base %d, gain %u", cat_names[p->cat], p->hash, r.base, r.gain);
    own_record(s, p, &r, now);
}

void coop_sync_pool_local(CoopSync *s, int cat, uint32_t hash, int32_t value, int32_t cap, uint64_t now) {
    if (cat < 0 || cat >= COOP_CAT_N || !coop_cat_is_pool(cat)) return;
    if (now > s->now) s->now = now;
    Pool *p = pool_get(s, hash, cat, 1);
    if (!p) return;
    p->cap = cap, p->local = value, p->have_local = 1;
    if (!s->have_room || s->room.mode[cat] != COOP_MODE_POOL) {
        p->bound = 0;
        return;
    }
    if (!p->bound) {
        // join the pool once the room's records are known, the maker admitted this identity and the player accepted the
        // room for the save being played (the pool counts the keys it accepted). With the pool in credit, this game's
        // base claim makes it hold at least what this player has (after a pool reset, at least what it held for the
        // players still admitted). In debt (spends beyond what entered it, as a drain leaves it), what a new
        // contributor has pays the debt like a gain: a claim would cover the debt and outlast the drainer's removal
        if (!s->caught_up || !admitted(s, s->kid) || !s->accepted || s->acc_save_gen != s->save_gen) return;
        Rec *mine = rec_of(p, s->kid);
        int fresh = !mine || mine->epoch != s->ch.pool_epoch;
        if (fresh && !mine && p->nrec == MAX_ADMIT) return;
        Joins *js = &p->joins;
        if (fresh || js->epoch != s->ch.pool_epoch || !js->n) {
            // the first join of the epoch, or a record of it this game does not remember joining with (the room kept
            // it while this game started again, or a reset's carry signed at once): what it holds stays the least it
            // claims. A reset's carry that was not signed then (this game was not admitted yet) is a claim made at the
            // reset, so what anyone picked up since comes on top of it
            int32_t carry = p->has_carry && p->carry > 0 ? (int32_t)clamp_reg(p->carry) : -POOL_REG_MAX;
            js->epoch = s->ch.pool_epoch, js->n = 0, js->debt = 0, js->floor = fresh ? carry : mine->base;
        }
        if (js->n == POOL_JOINS) {   // the second oldest keeps what it claims now; the first holds the debt gain
            int32_t c;
            uint32_t d;
            join_claim(s, p, &js->j[1], NULL, 0, &c, &d);
            if (c > js->floor) js->floor = c;
            memmove(&js->j[1], &js->j[2], (POOL_JOINS - 2) * sizeof js->j[0]);
            js->n--;
        }
        // every join is kept, also one that claims nothing now: a removal may leave it the highest
        Join *j = &js->j[js->n++];
        memset(j, 0, sizeof *j);
        j->fresh = fresh, j->want = value;
        for (int i = 0; i < p->nrec; i++)
            if (p->rec[i].epoch == s->ch.pool_epoch) j->seen[j->nseen++] = (Seen){p->rec[i].kid, p->rec[i].base, p->rec[i].gain, p->rec[i].spend};
        int32_t base;
        uint32_t debt;
        claims_now(s, p, NULL, 0, &base, &debt);
        Rec r;
        if (fresh) memset(&r, 0, sizeof r), r.base = -POOL_REG_MAX, r.epoch = s->ch.pool_epoch, r.kid = s->kid, memcpy(r.pk, s->pk, COOP_PK_LEN);
        else r = *mine;
        uint64_t gain = (uint64_t)r.gain - (js->debt < r.gain ? js->debt : r.gain) + debt;
        if (fresh || base != r.base || gain != r.gain) {
            r.base = base, r.gain = (uint32_t)(gain < POOL_REG_MAX ? gain : POOL_REG_MAX), js->debt = debt;
            own_record(s, p, &r, now);
        }
        p->has_carry = 0;
        p->expected = value, p->bound = 1, p->recheck = 0;
        s->st.pools_bound++;
        tracef(s, "pool %s %08x joined at %d, the pool holds %lld", cat_names[cat], hash, value, (long long)pool_value(s, p));
        pool_check(s, p);
        return;
    }
    if (p->recheck) pool_recheck(s, p, now);
    int64_t d = (int64_t)value - p->expected;
    if (d > 0) contribute(s, p, 0, (uint32_t)d, now);
    else if (d < 0) contribute(s, p, 1, (uint32_t)-d, now);
    p->expected = value;
    pool_check(s, p);
}

int coop_sync_take(CoopSync *s, CoopApply *out, int max) {
    if (!coop_sync_open(s)) return 0;   // checked here, under the caller's lock: a change of rules waits for the player
    int n = 0;
    while (n < max && s->apply_head != s->apply_tail) {
        int code = s->apply[s->apply_head++ % s->apply_cap];
        if (code >= 0) {
            Reg *r = &s->regs[code];
            r->queued = 0;
            if (!flag_synced(s, r) || r->s <= r->l || r->s == r->applied) continue;
            r->applied = r->s;
            out[n++] = (CoopApply){0, r->cat, r->hash, (int32_t)(r->s > INT32_MAX ? INT32_MAX : r->s)};
        } else {
            Pool *p = &s->pools[-1 - code];
            p->queued = 0;
            if (!p->bound) continue;
            int32_t t = pool_target(s, p);
            if (t == p->expected) continue;
            out[n++] = (CoopApply){1, p->cat, p->hash, t};
        }
    }
    return n;
}

void coop_sync_pool_wrote(CoopSync *s, uint32_t hash, int32_t value) {
    Pool *p = pool_get(s, hash, 0, 0);
    if (!p) return;
    p->expected = value;
    pool_check(s, p);
}

void coop_sync_untake(CoopSync *s, const CoopApply *a) {
    if (a->pool) {
        Pool *p = pool_get(s, a->hash, 0, 0);
        if (p && !p->queued) p->queued = 1, queue_apply(s, -1 - (int)(p - s->pools));
        return;
    }
    int idx = find_reg(s, a->hash);
    if (idx < 0) return;
    s->regs[idx].applied = 0;
    flag_check(s, idx);
}

// ---- inspection

void coop_sync_stats(const CoopSync *s, CoopSyncStats *out) {
    *out = s->st;
    out->regs = (unsigned)s->nregs;
    out->synced = 0;
    for (int i = 0; i < s->nregs; i++)
        if (s->regs[i].s && flag_synced(s, &s->regs[i])) out->synced++;
    out->pools_bound = 0;
    for (int i = 0; i < s->npools; i++) {
        out->regs += (unsigned)s->pools[i].nrec;
        if (pool_synced(s, &s->pools[i])) out->synced += (unsigned)s->pools[i].nrec;
        out->pools_bound += s->pools[i].bound;
    }
}

int coop_sync_record(const CoopSync *s, uint32_t hash, uint64_t kid, int32_t *base, uint32_t *gain, uint32_t *spend) {
    for (int i = 0; i < s->npools; i++) {
        const Rec *r = s->pools[i].hash == hash ? rec_of((Pool *)&s->pools[i], kid) : NULL;
        if (!r || r->epoch != s->ch.pool_epoch) continue;
        *base = r->base, *gain = r->gain, *spend = r->spend;
        return 1;
    }
    return 0;
}

uint32_t coop_sync_pool_epoch(const CoopSync *s) { return s->ch.pool_epoch; }

uint32_t coop_sync_flag(const CoopSync *s, int cat, uint32_t hash) {
    (void)cat;
    int idx = find_reg(s, hash);
    return idx < 0 ? 0 : s->regs[idx].s;
}

int coop_sync_pool(const CoopSync *s, uint32_t hash, int64_t *value) {
    for (int i = 0; i < s->npools; i++)
        if (s->pools[i].hash == hash) {
            if (value) *value = pool_value(s, &s->pools[i]);
            return s->pools[i].bound;
        }
    if (value) *value = 0;
    return 0;
}

int coop_sync_own_record(const CoopSync *s, uint32_t hash, int32_t *base, uint32_t *gain, uint32_t *spend) {
    for (int i = 0; i < s->npools; i++) {
        const Pool *p = &s->pools[i];
        const Rec *r = p->hash == hash ? rec_of((Pool *)p, s->kid) : NULL;
        if (!r || r->epoch != s->ch.pool_epoch) continue;
        *base = r->base, *gain = r->gain, *spend = r->spend;
        return 1;
    }
    return 0;
}

int coop_sync_accepted_key(const CoopSync *s, uint64_t kid) { return kid == s->kid || in_list(s->acc_kids, s->nacc, kid); }

void coop_sync_sums(const CoopSync *s, uint32_t out[COOP_SYNC_SECTIONS]) { memcpy(out, s->sums, sizeof s->sums); }
