// --probe-coop, the progress sync part: the merge engine (coop_state.c) between fake games on a fake network, without a
// game, a socket or a clock (the time is simulated, so a minute of play takes milliseconds).
//
// A fake game is a flag store with the categories of the real one (coop_game.c): flags that only go up, item counts, and
// spendable counters with a cap. It reports its flags to its engine as the game side does (every change), takes what the
// engine hands out, and checks that nothing it applies lowers a flag or an item. The fake network delivers each message
// to every other member (or one), losing, repeating and reordering them as asked.
//
// 1. Merge algebra: the same messages in every order, some twice, give the same state; nothing shrinks.
// 2. A room of two, then a late joiner who catches up with everything, and the room catches up with what it brought.
// 3. A room of four with 30 % of the messages lost, some repeated and reordered: every game ends with the union.
// 4. Rules: they come from the room's maker (a charter its key signs), a joiner's own settings are ignored, a room
//    without its maker keeps the rules the members carry, and the modes: items per player, the shared pools (two players
//    spending at once, a cap, a debt paid back, a player who joins with more, a game that restarts), keys per player with
//    progress per player, and a rule switched in the middle of a session (to shared: merges from then on; to per player:
//    stops, keeps what it has).
// 5. Nothing merged survives a change of room or of save: a save loaded after leaving a room, the next room, and another
//    save loaded in a room of one get none of what the last room and the last save had.
// 6. A hostile member: hashes the game does not share, values out of range (a flag set to 2 to freeze it, an item of
//    1000), pool records it did not sign or under keys the maker did not admit, a flood; none of it is stored, passed on
//    or applied, the honest values still merge, and the member is ignored after a few dozen. Its own pool records take
//    at most what the pool can hold, and the maker's removal gives that back. Identities: acceptance is by key, a
//    removed member or a copied identity changes nothing, and the rules' key is pinned. Answers are fair.
// 7. Random bytes into the receiver.
// 8. The third review: flags only from keys the player accepted, a drain the maker's restarted game undoes, identity
//    replays, the relay's maker mark against the members' rules, signed names, failed signatures, own pool records
//    after the room came back, removals that stay.
// 9. The fourth review: records with the lowest base, pools that follow the player's own acceptance, two resets, ended
//    sessions per key, forged rosters, what a contributor took per pool.
// 10. Hostile members and an attacker who hold the secret, at random, against a ledger the harness keeps itself.
// 11. The fifth review: sections in another member's name, sessions that no roster or replay ends, claims made again
//    after a removal, another save that joins the pools only once accepted.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop_proto.h"
#include "coop_state.h"

// From the fifth review on, a section carries its sender's counter and signature, and an identity the session before
// it; the tests build the format of the engine they run against (COOP_BEFORE: the engine before those, through a shim).
#ifdef COOP_BEFORE
#define H5_SIGNED 0
#else
#define H5_SIGNED 1
#endif

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL coop-sync: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static uint64_t rng = 0x243F6A8885A308D3ull;
static uint32_t rnd(void) {
    rng ^= rng >> 12, rng ^= rng << 25, rng ^= rng >> 27;
    return (uint32_t)((rng * 0x2545F4914F6CDD1Dull) >> 32);
}

// ---- a fake game: its flags, in the categories of the real one

enum { MAX_FLAGS = 600, MAX_PLAYERS = 6, RUPEE = 0x52555045, ARROWS = 0x4152524F, KEYS_FOREST = 0x4B455901, KEYS_WATER = 0x4B455902 };

typedef struct {
    uint32_t hash;
    uint8_t cat;
    int32_t value, reported, cap;   // reported: what the engine last heard
    int known;                      // reported at least once since the last rebind
} Flag;

typedef struct {
    int id;                         // member id
    int made;                       // it made the room: the relay names it the maker
    int ask;                        // its player accepts nothing by itself (else every offer, as coop_accept = always)
    uint64_t session;               // its session in the room (the relay's roster lists it)
    uint64_t client;                // its start's random value (the seed of its keys)
    CoopSync *eng;
    Flag f[MAX_FLAGS];
    int n;
    int lowered;                    // an apply lowered a flag or an item: never allowed
    int hold_pools;                 // no save played yet (the title screen): its pool counters are not reported
    unsigned applied;
    unsigned applied_closed;        // values handed out while the room was not open to it: never allowed
    struct Adv *ledger;             // the randomised run's ledger of its pools (section 10), NULL elsewhere
} Game;

// The games by member id (the latest one made with an id): a test that hands an engine a section as a member sends
// it signs it with that member's key, as coop_state.c signs its own (seal_as).
static Game *by_id[COOP_MAX_MEMBERS + 1];

struct Adv;
static void adv_led_report(struct Adv *a, Game *g, const Flag *f, int was_bound, int after);
static void adv_led_applied(struct Adv *a, Game *g, const Flag *f);
static void adv_led_snap(struct Adv *a, const Game *g);

static Flag *flag_of(Game *g, uint32_t hash) {
    for (int i = 0; i < g->n; i++)
        if (g->f[i].hash == hash) return &g->f[i];
    return NULL;
}

static Flag *flag_add(Game *g, int cat, uint32_t hash, int32_t value, int32_t cap) {
    Flag *f = flag_of(g, hash);
    if (!f) {
        f = &g->f[g->n++];
        memset(f, 0, sizeof *f);
        f->hash = hash, f->cat = (uint8_t)cat, f->cap = cap;
    }
    f->value = value;
    return f;
}

// The game's table as the engine sees it (coop_game.c's limits): every flag of the fake game in its own category, a
// Bool, song or quest item up to 1, an item up to 4, small keys up to 15 and the other counters up to 999.
static uint32_t fake_limit(int cat, uint32_t hash, void *user) {
    Flag *f = flag_of(user, hash);
    if (!f || f->cat != cat) return 0;
    return cat == COOP_CAT_ITEMS ? 4 : cat == COOP_CAT_KEYS ? 15 : coop_cat_is_pool(cat) ? 999 : 1;
}

// The flag set of the fake game: hashes 1..P are progress flags, 0x1000+ items, 0x2000+ songs, 0x3000+ quest items.
static void game_init(Game *g, int id, uint64_t client) {
    memset(g, 0, sizeof *g);
    g->id = id, g->client = client;
    if (id >= 1 && id <= COOP_MAX_MEMBERS) by_id[id] = g;
    static const uint8_t index_key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    g->eng = coop_sync_new(client, 0xC0FFEE);
    g->session = client * 0x9E3779B97F4A7C15ull | 1;
    coop_sync_limits(g->eng, fake_limit, g);
    coop_sync_index_key(g->eng, index_key);
    uint8_t seed[32] = {0};
    memcpy(seed, &client, sizeof client);   // a start of the game: its own random value
    coop_sync_seed(g->eng, seed);
    char name[16];
    snprintf(name, sizeof name, "P%d", id);
    coop_sync_name(g->eng, name);
    coop_sync_rebind(g->eng);   // a save is being played
    for (uint32_t h = 1; h <= 300; h++) flag_add(g, COOP_CAT_PROGRESS, h, 0, -1);
    for (uint32_t h = 0x1000; h < 0x1040; h++) flag_add(g, COOP_CAT_ITEMS, h, 0, -1);
    for (uint32_t h = 0x2000; h < 0x200D; h++) flag_add(g, COOP_CAT_SONGS, h, 0, -1);
    for (uint32_t h = 0x3000; h < 0x300B; h++) flag_add(g, COOP_CAT_QUEST, h, 0, -1);
    flag_add(g, COOP_CAT_RUPEES, RUPEE, 0, 200);
    flag_add(g, COOP_CAT_AMMO, ARROWS, 0, 30);
    flag_add(g, COOP_CAT_KEYS, KEYS_FOREST, 0, -1);
}

// The player's acceptance (unless the game asks) and the copy of the save, as coop_game.c does with coop_accept = always.
static void game_accept(Game *g, uint64_t now) {
    CoopOffer o;
    if (!g->ask && coop_sync_offer(g->eng, &o, now) && !o.accepted) coop_sync_accept(g->eng, o.token);
    if (coop_sync_copy_wanted(g->eng)) coop_sync_copied(g->eng, 1);
}

// What the game side does every frame: report what changed (pools every time), then take what the room has.
static void game_frame(Game *g, uint64_t now) {
    game_accept(g, now);
    for (int i = 0; i < g->n; i++) {
        Flag *f = &g->f[i];
        if (coop_cat_is_pool(f->cat)) {
            if (g->hold_pools) continue;
            int was = g->ledger && coop_sync_pool(g->eng, f->hash, NULL);
            if (g->ledger) adv_led_report(g->ledger, g, f, was, 0);
            coop_sync_pool_local(g->eng, f->cat, f->hash, f->value, f->cap, now);
            if (g->ledger) adv_led_report(g->ledger, g, f, was, 1);
        } else if (!f->known || f->value != f->reported) {
            if (f->value || f->known) coop_sync_local(g->eng, f->cat, f->hash, (uint32_t)f->value, now);
            f->reported = f->value, f->known = 1;
        }
    }
    CoopApply a[64];
    int n, open = coop_sync_open(g->eng);
    while ((n = coop_sync_take(g->eng, a, 64)) > 0) {
        if (!open) g->applied_closed += (unsigned)n;
        for (int i = 0; i < n; i++) {
            Flag *f = flag_of(g, a[i].hash);
            if (!f || f->cat != a[i].cat) continue;   // a hash this game does not have: the real game skips it too
            if (a[i].pool) {
                f->value = a[i].value;
                coop_sync_pool_wrote(g->eng, a[i].hash, a[i].value);
                if (g->ledger) adv_led_applied(g->ledger, g, f);
            } else {
                if (a[i].value <= f->value) g->lowered++;
                f->value = a[i].value;
            }
            g->applied++;
        }
    }
}

static void game_free(Game *g) {
    for (int i = 0; i <= COOP_MAX_MEMBERS; i++)
        if (by_id[i] == g) by_id[i] = NULL;
    coop_sync_free(g->eng);
}

// ---- a fake network

typedef struct {
    int from, to;          // to: 0 every other member
    uint8_t kind;
    uint16_t n;
    uint64_t sess;         // the sender's session when it sent it
    uint8_t b[COOP_MAX_PAYLOAD];
} Packet;

typedef struct {
    Packet *q;
    int n, cap;
    unsigned loss, dup, reorder;   // percent
    Game *games[MAX_PLAYERS];
    int present[MAX_PLAYERS];
    int sending;                   // the member a send comes from
    int only[9];                   // by sender id: deliver its messages to this member only (0: as addressed)
    int mute[9];                   // by sender id: deliver none of its messages
    unsigned sent, lost;
    Packet *log;                   // when set: the last log_cap messages of the members up to log_ids, for replays
    unsigned log_cap, log_n;
    int log_ids;
} Net;

static int net_send(int member, uint8_t kind, const uint8_t *bytes, size_t n, void *user) {
    Net *net = user;
    if (n > sizeof net->q[0].b) return 0;
    if (net->n == net->cap) {
        int nc = net->cap ? net->cap * 2 : 256;
        Packet *nq = realloc(net->q, (size_t)nc * sizeof *nq);
        if (!nq) return 0;
        net->q = nq, net->cap = nc;
    }
    Packet *p = &net->q[net->n++];
    p->from = net->sending, p->to = member, p->kind = kind, p->n = (uint16_t)n, p->sess = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (net->games[i] && net->games[i]->id == net->sending) p->sess = net->games[i]->session;
    memcpy(p->b, bytes, n);
    net->sent++;
    if (net->log && net->sending <= net->log_ids) net->log[net->log_n++ % net->log_cap] = *p;
    return 1;
}

static void deliver_one(Net *net, const Packet *p, uint64_t now) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Game *g = net->games[i];
        if (!g || !net->present[i] || g->id == p->from || (p->to && p->to != g->id)) continue;
        if (p->from >= 1 && p->from <= 8 && (net->mute[p->from] || (net->only[p->from] && net->only[p->from] != g->id))) continue;
        if (rnd() % 100 < net->loss) {
            net->lost++;
            continue;
        }
        if (g->ledger && p->kind == COOP_K_DIGEST) adv_led_snap(g->ledger, g);   // a digest may bring a pool reset
        coop_sync_receive(g->eng, p->from, p->kind, p->b, p->n, now);
        if (rnd() % 100 < net->dup) coop_sync_receive(g->eng, p->from, p->kind, p->b, p->n, now);
    }
}

// Deliver what was sent, in a shuffled order when reorder is set.
static void net_flush(Net *net, uint64_t now) {
    while (net->n) {
        int i = net->reorder && rnd() % 100 < net->reorder ? (int)(rnd() % (unsigned)net->n) : 0;
        Packet p = net->q[i];
        memmove(&net->q[i], &net->q[i + 1], (size_t)(net->n - i - 1) * sizeof p);
        net->n--;
        deliver_one(net, &p, now);
    }
}

// Every present game hears the roster: who is there, and the maker the relay names (a present game that made the room).
static void net_roster(Net *net, const char *code, uint64_t now) {
    int ids[MAX_PLAYERS], n = 0, maker = 0;
    uint64_t sessions[MAX_PLAYERS];
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (net->games[i] && net->present[i]) {
            sessions[n] = net->games[i]->session;
            ids[n++] = net->games[i]->id;
            if (net->games[i]->made) maker = net->games[i]->id;
        }
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (net->games[i] && net->present[i]) coop_sync_room(net->games[i]->eng, code, net->games[i]->id, ids, sessions, n, maker, now);
}

// NX2NAVI_COOP_ADVERSARY_TRACE set: the randomised test prints each event and the honest engines' lines about pools,
// charters and identities, with the simulated time, so that a failing seed can be followed by hand.
static int adv_trace_on;
static uint64_t adv_trace_now;
static void adv_trace_line(void *user, const char *line) {
    if (strstr(line, "pool") || strstr(line, "keys") || strstr(line, "rupees") || strstr(line, "charter") || strstr(line, "identified") ||
        strstr(line, "key") || strstr(line, "caught"))
        fprintf(stderr, "[%llu] P%d %s\n", (unsigned long long)adv_trace_now, ((const Game *)user)->id, line);
}

// Run every present game for ms of simulated time in steps of step ms (8: the coop thread's step in a room, a frame on
// the game side; the randomised runs take larger ones).
static uint64_t run_by(Net *net, uint64_t now, unsigned ms, unsigned step) {
    for (unsigned t = 0; t < ms; t += step) {
        now += step;
        adv_trace_now = now;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            Game *g = net->games[i];
            if (!g || !net->present[i]) continue;
            game_frame(g, now);
            net->sending = g->id;
            coop_sync_tick(g->eng, now, net_send, net);
        }
        net_flush(net, now);
    }
    return now;
}

static uint64_t run(Net *net, uint64_t now, unsigned ms) { return run_by(net, now, ms, 8); }

static int sums_equal(Game *a, Game *b) {
    uint32_t x[COOP_SYNC_SECTIONS], y[COOP_SYNC_SECTIONS];
    coop_sync_sums(a->eng, x);
    coop_sync_sums(b->eng, y);
    return !memcmp(x, y, sizeof x);
}

static int32_t value_of(Game *g, uint32_t hash) {
    Flag *f = flag_of(g, hash);
    return f ? f->value : -1;
}

// Every flag of a category in games[0..n) equals the highest value want[] holds for it.
static int union_holds(Game **gs, int n, const int32_t *want, int cat_mask) {
    for (int k = 0; k < n; k++)
        for (int i = 0; i < gs[k]->n; i++) {
            Flag *f = &gs[k]->f[i];
            if (!(cat_mask >> f->cat & 1) || coop_cat_is_pool(f->cat)) continue;
            if (f->value != want[i]) return 0;
        }
    return 1;
}

static void settings(Game *g, int made, int items, int keys, int rupees, int ammo, int progress) {
    CoopRules r;
    coop_rules_default(&r);
    r.mode[COOP_CAT_PROGRESS] = (uint8_t)progress;
    r.mode[COOP_CAT_ITEMS] = (uint8_t)items;
    r.mode[COOP_CAT_KEYS] = (uint8_t)keys;
    r.mode[COOP_CAT_RUPEES] = (uint8_t)rupees;
    r.mode[COOP_CAT_AMMO] = (uint8_t)ammo;
    coop_sync_settings(g->eng, &r, made);
    g->made = made;
}

static void defaults(Game *g, int made) {
    CoopRules r;
    coop_rules_default(&r);
    coop_sync_settings(g->eng, &r, made);
    g->made = made;
}

// ---- 1. merge algebra

// The SECTION messages player i (member 6 + i) sends about its own batch of flags and items: it sits in a room with a
// peer that hears only its digests, so the peer keeps asking and the player keeps answering (some sections more than
// once). digest: its last digest, which carries its identity. Each player is a member of its own, since a section is
// signed by its sender's key for its member id and session.
static int capture(int i, Packet **out, Packet *digest) {
    Game solo, peer;
    int id = 6 + i;
    game_init(&solo, id, 200 + (uint64_t)i);
    game_init(&peer, 1, 300 + (uint64_t)i);
    defaults(&solo, 1);
    defaults(&peer, 0);
    for (int k = 0; k < 50; k++) solo.f[(k * 37 + i * 101) % 300].value = 1;
    for (int k = 0; k < 6; k++) solo.f[300 + (k * 11 + i * 7) % 64].value = 1 + (k + i) % 4;
    int ids[2] = {1, id}, n = 0;
    uint64_t t = 1000;
    coop_sync_room(solo.eng, "ALG002", id, ids, NULL, 2, id, t);
    coop_sync_room(peer.eng, "ALG002", 1, ids, NULL, 2, id, t);
    Net q = {0};
    for (int step = 0; step < 400; step++) {
        t += 8;
        game_frame(&solo, t);
        q.sending = id;
        coop_sync_tick(solo.eng, t, net_send, &q);
        for (int k = 0; k < q.n; k++) {
            if (q.q[k].kind == COOP_K_SECTION) {
                *out = realloc(*out, (size_t)(n + 1) * sizeof **out);
                (*out)[n++] = q.q[k];
            } else {
                if (digest) *digest = q.q[k];
                coop_sync_receive(peer.eng, id, q.q[k].kind, q.q[k].b, q.q[k].n, t);   // the rules reach the peer
            }
        }
        q.n = 0;
        q.sending = 1;
        coop_sync_tick(peer.eng, t, net_send, &q);
        for (int k = 0; k < q.n; k++) coop_sync_receive(solo.eng, 1, q.q[k].kind, q.q[k].b, q.q[k].n, t);
        q.n = 0;
    }
    free(q.q);
    game_free(&solo);
    game_free(&peer);
    return n;
}

static uint32_t le32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

static void probe_algebra(void) {
    // three players' messages, delivered to a fresh game in all six orders, the first player's twice
    static const int orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    Packet *streams[3] = {0}, digest[3];
    int counts[3];
    for (int i = 0; i < 3; i++) counts[i] = capture(i, &streams[i], &digest[i]);
    uint32_t first[COOP_SYNC_SECTIONS];
    int shrank = 0, differ = 0;
    for (int o = 0; o < 6; o++) {
        Game g;
        game_init(&g, 1, 999);
        defaults(&g, 1);
        int ids[4] = {1, 6, 7, 8};
        coop_sync_room(g.eng, "ALG002", 1, ids, NULL, 4, 1, 1000);
        Net none = {0};
        none.sending = 1;
        coop_sync_tick(g.eng, 1000, net_send, &none);   // it made the room: its rules (the defaults) are in force
        free(none.q);
        for (int i = 0; i < 3; i++) coop_sync_receive(g.eng, 6 + i, COOP_K_DIGEST, digest[i].b, digest[i].n, 1050);   // the identities
        CoopOffer off;   // whose keys this player accepts: flags are taken only from accepted keys
        if (coop_sync_offer(g.eng, &off, 1050)) coop_sync_accept(g.eng, off.token);
        for (int k = 0; k < 3; k++) {
            const int si = orders[o][k];
            for (int m = 0; m < counts[si]; m++)
                for (int rep = 0; rep < (k == 0 ? 2 : 1); rep++) {   // one message every 100 ms: under a member's rate
                    static uint64_t t = 1100;
                    coop_sync_receive(g.eng, 6 + si, COOP_K_SECTION, streams[si][m].b, streams[si][m].n, t += 100);
                }
        }
        // nothing shrinks: every flag a message carried is in the state, at least at the value it carried
        for (int si = 0; si < 3; si++)
            for (int m = 0; m < counts[si]; m++) {
                const uint8_t *b = streams[si][m].b;
                size_t at = 6;
                for (int e = 0; e < b[5]; e++) {
                    if (b[at] == COOP_R_FLAG && coop_sync_flag(g.eng, b[at + 1], le32(b + at + 2)) < le32(b + at + 6)) shrank++;
                    at += b[at] == COOP_R_POOL ? 2 + 4 + COOP_PK_LEN + 20 + COOP_SIG_LEN : 10;
                }
            }
        uint32_t sums[COOP_SYNC_SECTIONS];
        coop_sync_sums(g.eng, sums);
        if (!o) memcpy(first, sums, sizeof sums);
        else if (memcmp(first, sums, sizeof sums)) differ++;
        game_free(&g);
    }
    CHECK(counts[0] && counts[1] && counts[2], "every player sent its sections (%d %d %d messages)", counts[0], counts[1], counts[2]);
    CHECK(!differ, "%d of 6 orders give another state", differ);
    CHECK(!shrank, "%d values are lower than a message said", shrank);
    for (int i = 0; i < 3; i++) free(streams[i]);
    printf("coop-probe: sync algebra: %d + %d + %d messages in 6 orders, the first player's twice, give one state\n", counts[0],
           counts[1], counts[2]);
}

// ---- 2 and 3. rooms

static void probe_rooms(unsigned loss, int players, int late) {
    Net net = {.loss = loss, .dup = loss ? 5 : 0, .reorder = loss ? 20 : 0};
    Game g[MAX_PLAYERS];
    int32_t want[MAX_FLAGS] = {0};
    for (int i = 0; i < players; i++) {
        game_init(&g[i], i + 1, 0xA000 + (uint64_t)i);
        defaults(&g[i], i == 0);
        net.games[i] = &g[i];
        net.present[i] = !(late && i == players - 1);
    }
    uint64_t now = 1000;
    net_roster(&net, "ROOM01", now);
    // two minutes of play: each game gains flags and items now and then; the late joiner plays alone meanwhile
    for (int sec = 0; sec < 120; sec++) {
        for (int i = 0; i < players; i++)
            if (rnd() % 3 == 0) {
                int k = (int)(rnd() % 364);
                int32_t v = k < 300 ? 1 : 1 + (int32_t)(rnd() % 3);
                if (v > g[i].f[k].value) g[i].f[k].value = v;
            }
        if (late && sec == 60) {   // the last player joins half way
            net.present[players - 1] = 1;
            net_roster(&net, "ROOM01", now);
        }
        now = run(&net, now, 1000);
    }
    for (int i = 0; i < players; i++)   // the union of everything any game reached
        for (int k = 0; k < g[i].n; k++)
            if (g[i].f[k].value > want[k] && !coop_cat_is_pool(g[i].f[k].cat)) want[k] = g[i].f[k].value;
    now = run(&net, now, 20000);   // quiet: let the digests repair what was lost
    Game *gs[MAX_PLAYERS];
    for (int i = 0; i < players; i++) gs[i] = &g[i];
    int all = 1;
    for (int i = 1; i < players; i++) all &= sums_equal(&g[0], &g[i]);
    unsigned lowered = 0, applied = 0;
    for (int i = 0; i < players; i++) lowered += (unsigned)g[i].lowered, applied += g[i].applied;
    CHECK(all, "%d players at %u %% loss: the states differ", players, loss);
    CHECK(union_holds(gs, players, want, 0xF), "%d players at %u %% loss: a game lacks part of the union", players, loss);
    CHECK(!lowered, "%u applies lowered a value", lowered);
    printf("coop-probe: sync %d players%s, %u %% loss: every game holds the union (%u applies, %u of %u messages lost)\n", players,
           late ? " (one joins late)" : "", loss, applied, net.lost, net.sent);
    for (int i = 0; i < players; i++) game_free(&g[i]);
    free(net.q);
}

// ---- 4. rules and modes

static void probe_rules(void) {
    Net net = {0};
    Game a, b, c;
    game_init(&a, 1, 0xA1);
    game_init(&b, 2, 0xB2);
    game_init(&c, 3, 0xC3);
    settings(&a, 1, COOP_MODE_OWN, COOP_MODE_POOL, COOP_MODE_POOL, COOP_MODE_POOL, COOP_MODE_OWN);   // progress own: items may be own
    settings(&b, 0, COOP_MODE_SHARED, COOP_MODE_POOL, COOP_MODE_OWN, COOP_MODE_OWN, COOP_MODE_SHARED);  // ignored: A made the room
    net.games[0] = &a, net.games[1] = &b, net.games[2] = &c;
    net.present[0] = net.present[1] = 1;
    a.f[0].value = 1;   // a progress flag
    a.f[300].value = 1; // an item
    flag_of(&a, RUPEE)->value = 50;
    flag_of(&b, RUPEE)->value = 30;
    flag_of(&a, ARROWS)->value = 10;
    flag_of(&b, ARROWS)->value = 20;
    flag_of(&a, KEYS_FOREST)->value = 1;
    uint64_t now = run(&net, (net_roster(&net, "RULE01", 1000), 1000), 3000);
    CoopRules ra, rb;
    coop_sync_rules(a.eng, &ra);
    coop_sync_rules(b.eng, &rb);
    CHECK(ra.epoch && !memcmp(ra.mode, rb.mode, COOP_CAT_N) && rb.maker == 1, "the joiner uses the maker's rules");
    CHECK(rb.mode[COOP_CAT_KEYS] == COOP_MODE_OWN, "with progress per player keys are per player (normalized), not %d", rb.mode[COOP_CAT_KEYS]);
    CHECK(value_of(&b, 1) == 0 && value_of(&b, 0x1000) == 0, "progress and items per player stay with A");
    CHECK(value_of(&b, KEYS_FOREST) == 0, "keys per player stay with A");
    CHECK(value_of(&a, RUPEE) == 50 && value_of(&b, RUPEE) == 50, "a joiner with fewer rupees gets the pool's 50 (%d %d)", value_of(&a, RUPEE),
          value_of(&b, RUPEE));
    CHECK(value_of(&a, ARROWS) == 20 && value_of(&b, ARROWS) == 20, "the arrow pool takes the higher count (%d %d)", value_of(&a, ARROWS),
          value_of(&b, ARROWS));

    // two players spend at once, before either hears of the other's: both spends count
    flag_of(&a, RUPEE)->value -= 30;
    flag_of(&b, RUPEE)->value -= 15;
    now = run(&net, now, 2000);
    CHECK(value_of(&a, RUPEE) == 5 && value_of(&b, RUPEE) == 5, "two spends at once: 50 - 30 - 15 = 5 (%d %d)", value_of(&a, RUPEE),
          value_of(&b, RUPEE));
    // both spend the last units at once: the pool goes below zero, shows 0, and the next gain pays the debt first
    flag_of(&a, RUPEE)->value -= 5;
    flag_of(&b, RUPEE)->value -= 5;
    now = run(&net, now, 2000);
    int64_t v;
    coop_sync_pool(a.eng, RUPEE, &v);
    CHECK(v == -5 && value_of(&a, RUPEE) == 0 && value_of(&b, RUPEE) == 0, "spent twice: the pool is -5 and shows 0 (%lld)", (long long)v);
    flag_of(&b, RUPEE)->value += 20;
    now = run(&net, now, 2000);
    CHECK(value_of(&a, RUPEE) == 15 && value_of(&b, RUPEE) == 15, "a gain of 20 after a debt of 5 shows 15 (%d %d)", value_of(&a, RUPEE),
          value_of(&b, RUPEE));
    // gains at once from both, past the cap: the pool counts both, each game shows its cap
    flag_of(&a, ARROWS)->value = 30;
    flag_of(&b, ARROWS)->value = 30;
    now = run(&net, now, 2000);
    coop_sync_pool(a.eng, ARROWS, &v);
    CHECK(v == 40 && value_of(&a, ARROWS) == 30 && value_of(&b, ARROWS) == 30, "two gains of 10 at once: pool 40, both show the cap 30 (%lld)",
          (long long)v);
    flag_of(&a, ARROWS)->value -= 5;   // a spend from a capped count: the pool falls to 35, still over the cap
    now = run(&net, now, 2000);
    CHECK(value_of(&a, ARROWS) == 30 && value_of(&b, ARROWS) == 30, "a spend of 5 from 40 still shows the cap (%d %d)", value_of(&a, ARROWS),
          value_of(&b, ARROWS));

    // a third player joins with more rupees than the pool: the pool rises to it, nobody loses any
    flag_of(&c, RUPEE)->value = 120;
    settings(&c, 0, COOP_MODE_SHARED, COOP_MODE_POOL, COOP_MODE_OWN, COOP_MODE_OWN, COOP_MODE_SHARED);
    net.present[2] = 1;
    net_roster(&net, "RULE01", now);
    now = run(&net, now, 4000);
    CHECK(value_of(&a, RUPEE) == 120 && value_of(&b, RUPEE) == 120 && value_of(&c, RUPEE) == 120, "a joiner with 120 raises the pool to 120 (%d %d %d)",
          value_of(&a, RUPEE), value_of(&b, RUPEE), value_of(&c, RUPEE));

    // B's game restarts: a new engine and identity, its save had the 120 it showed; the pool does not pay it twice
    int32_t saved = value_of(&b, RUPEE);
    game_free(&b);
    game_init(&b, 2, 0xB2B2);
    settings(&b, 0, COOP_MODE_SHARED, COOP_MODE_POOL, COOP_MODE_OWN, COOP_MODE_OWN, COOP_MODE_SHARED);
    flag_of(&b, RUPEE)->value = saved;
    net_roster(&net, "RULE01", now);
    now = run(&net, now, 8000);
    coop_sync_pool(a.eng, RUPEE, &v);
    CHECK(v == 120 && value_of(&b, RUPEE) == 120, "a restarted game rejoins the pool without adding to it (%lld, %d)", (long long)v, value_of(&b, RUPEE));

    // mid-session: the maker switches items to shared; what A got while they were per player reaches the others, and later items too
    settings(&a, 1, COOP_MODE_SHARED, COOP_MODE_POOL, COOP_MODE_POOL, COOP_MODE_POOL, COOP_MODE_OWN);
    now = run(&net, now, 4000);
    CHECK(value_of(&b, 0x1000) == 1 && value_of(&c, 0x1000) == 1, "items switched to shared: A's earlier item reaches B and C");
    b.f[301].value = 2;
    now = run(&net, now, 3000);
    CHECK(value_of(&a, 0x1001) == 2 && value_of(&c, 0x1001) == 2, "items shared: B's new item reaches A and C");
    // and back to per player: nothing merges any more, nothing merged is taken back
    settings(&a, 1, COOP_MODE_OWN, COOP_MODE_POOL, COOP_MODE_OWN, COOP_MODE_POOL, COOP_MODE_OWN);
    now = run(&net, now, 3000);
    c.f[302].value = 1;
    flag_of(&c, RUPEE)->value = 7;   // rupees per player now: C's change stays with C
    now = run(&net, now, 3000);
    CHECK(value_of(&a, 0x1002) == 0 && value_of(&b, 0x1002) == 0, "items switched to per player: C's new item stays with C");
    CHECK(value_of(&a, 0x1000) == 1 && value_of(&b, 0x1001) == 2, "items switched to per player: what was merged stays");
    CHECK(value_of(&a, RUPEE) == 120 && value_of(&b, RUPEE) == 120 && value_of(&c, RUPEE) == 7, "rupees switched to per player: each keeps its own");
    CHECK(!a.lowered && !b.lowered && !c.lowered, "no apply lowered a flag or an item");

    // the maker leaves; a new member still gets the room's rules from the others
    net.present[0] = 0;
    Game d;
    game_init(&d, 4, 0xD4);
    defaults(&d, 0);
    net.games[3] = &d, net.present[3] = 1;
    net_roster(&net, "RULE01", now);
    now = run(&net, now, 3000);
    CoopRules rd;
    coop_sync_rules(d.eng, &rd);
    CHECK(rd.epoch && rd.mode[COOP_CAT_ITEMS] == COOP_MODE_OWN, "the maker left: a joiner adopts the rules the others hold, signed by the maker");
    game_free(&a), game_free(&b), game_free(&c), game_free(&d);
    free(net.q);

    // a room whose maker is gone before anyone heard the rules: nobody else makes any, nothing merges
    Net n2 = {0};
    Game x, y;
    game_init(&x, 5, 0x55);
    game_init(&y, 3, 0x33);
    settings(&x, 0, COOP_MODE_SHARED, COOP_MODE_POOL, COOP_MODE_POOL, COOP_MODE_OWN, COOP_MODE_SHARED);
    settings(&y, 0, COOP_MODE_SHARED, COOP_MODE_POOL, COOP_MODE_OWN, COOP_MODE_POOL, COOP_MODE_SHARED);
    x.f[0].value = 1;
    n2.games[0] = &x, n2.games[1] = &y, n2.present[0] = n2.present[1] = 1;
    now = run(&n2, (net_roster(&n2, "RULE02", 1000), 1000), 8000);
    CoopRules rx;
    coop_sync_rules(x.eng, &rx);
    CHECK(!rx.epoch && value_of(&y, 1) == 0, "a room without its maker: no member makes rules, nothing merges (epoch %u)", rx.epoch);
    game_free(&x), game_free(&y);
    free(n2.q);

    // normalization: with progress shared, items, songs and quest items are shared whatever the settings say
    CoopRules r;
    coop_rules_default(&r);
    r.mode[COOP_CAT_SONGS] = COOP_MODE_OWN;
    r.mode[COOP_CAT_RUPEES] = COOP_MODE_SHARED;   // not a mode rupees have
    unsigned changed = coop_rules_normalize(&r);
    CHECK(r.mode[COOP_CAT_SONGS] == COOP_MODE_SHARED && r.mode[COOP_CAT_RUPEES] == COOP_MODE_OWN &&
              changed == (1u << COOP_CAT_SONGS | 1u << COOP_CAT_RUPEES), "normalize: songs shared with progress shared, rupees own");
    printf("coop-probe: sync rules: the maker's rules hold, pools spend and gain at once, rules switch mid-session\n");
}

// Keys in a pool with progress shared: a key one player picks up opens a door for the other; the door spends it for both.
static void probe_keys(void) {
    Net net = {.loss = 30, .dup = 5, .reorder = 20};
    Game a, b;
    game_init(&a, 1, 0x1A);
    game_init(&b, 2, 0x2B);
    defaults(&a, 1);
    defaults(&b, 0);
    net.games[0] = &a, net.games[1] = &b, net.present[0] = net.present[1] = 1;
    // at 30 % loss the room is known (and the pools join) after a few digests; each step waits several of them
    uint64_t now = run(&net, (net_roster(&net, "KEYS01", 1000), 1000), 15000);
    flag_of(&a, KEYS_FOREST)->value = 1;   // A picks up a key (its pickup flag is progress)
    a.f[10].value = 1;
    now = run(&net, now, 15000);
    CHECK(value_of(&b, KEYS_FOREST) == 1 && value_of(&b, 11) == 1, "A's key and its pickup reach B");
    flag_of(&b, KEYS_FOREST)->value = 0;   // B opens a door with it
    b.f[20].value = 1;
    now = run(&net, now, 15000);
    CHECK(value_of(&a, KEYS_FOREST) == 0 && value_of(&a, 21) == 1, "B's door spends the key for A too, and opens for A");
    CHECK(!a.lowered && !b.lowered, "no apply lowered a flag");
    game_free(&a), game_free(&b);
    free(net.q);
    printf("coop-probe: sync keys: a key picked up by one opens a door for the other, once, at 30 %% loss\n");
}

// ---- 5. carry-over between rooms and saves

static void reload_save(Game *g) {   // the game loads another save: every flag at its default, reported again
    for (int k = 0; k < g->n; k++) g->f[k].value = 0, g->f[k].reported = 0, g->f[k].known = 0;
    coop_sync_rebind(g->eng);
}

static void probe_carry_over(void) {
    Net net = {0};
    Game x, p, q;
    game_init(&x, 1, 0x51);
    game_init(&p, 2, 0x52);
    game_init(&q, 3, 0x53);
    defaults(&x, 1);
    defaults(&p, 0);
    for (int k = 0; k < 10; k++) x.f[k].value = 1;    // save X: progress flags 1..10
    for (int k = 10; k < 20; k++) p.f[k].value = 1;   // P: 11..20
    net.games[0] = &x, net.games[1] = &p, net.present[0] = net.present[1] = 1;
    uint64_t now = run(&net, (net_roster(&net, "ROOMAA", 1000), 1000), 5000);
    CHECK(value_of(&x, 11) == 1 && value_of(&p, 1) == 1, "carry-over: room A merged X and P");
    // X leaves room A, loads save Y (only flag 31), and joins room B, which Q made (Q has flag 41)
    net.present[1] = 0;
    coop_sync_room(x.eng, "", 0, NULL, NULL, 0, 0, now);
    reload_save(&x);
    x.f[30].value = 1;
    q.f[40].value = 1;
    defaults(&x, 0);
    defaults(&q, 1);
    net.games[2] = &q, net.present[2] = 1;
    now = run(&net, (net_roster(&net, "ROOMBB", now), now), 6000);
    int leaked = 0;
    for (uint32_t h = 1; h <= 20; h++) leaked += value_of(&x, h) != 0 || value_of(&q, h) != 0;
    CHECK(!leaked, "carry-over: %d flags of room A and save X reached save Y or room B", leaked);
    CHECK(value_of(&q, 31) == 1 && value_of(&x, 41) == 1, "carry-over: room B merges Y and Q");
    // in a room of its own, X loads save Z: nothing of Y reaches it
    net.present[2] = 0;
    defaults(&x, 1);
    now = run(&net, (net_roster(&net, "ROOMCC", now), now), 3000);
    reload_save(&x);
    x.f[50].value = 1;
    now = run(&net, now, 3000);
    CHECK(value_of(&x, 31) == 0 && value_of(&x, 41) == 0 && value_of(&x, 51) == 1, "carry-over: save Z got nothing of save Y (%d %d)",
          value_of(&x, 31), value_of(&x, 41));
    game_free(&x), game_free(&p), game_free(&q);
    free(net.q);
    printf("coop-probe: sync carry-over: a new room and a new save start with nothing of the last ones\n");
}

// ---- 6. a hostile member

enum { REC_BYTES = 2 + 4 + COOP_PK_LEN + 20 + COOP_SIG_LEN };   // a pool record's entry

static uint8_t *put_le(uint8_t *p, uint64_t v, int n) {
    for (int i = 0; i < n; i++) *p++ = (uint8_t)(v >> 8 * i);
    return p;
}

static uint8_t *entry_flag(uint8_t *p, int cat, uint32_t hash, uint32_t v) {
    *p++ = COOP_R_FLAG, *p++ = (uint8_t)cat;
    p = put_le(p, hash, 4);
    return put_le(p, v, 4);
}

// A pool record of room code signed with the key of seed, as coop_state.c makes one.
static uint8_t *entry_record(uint8_t *p, const char *code, int cat, uint32_t hash, const uint8_t seed[32], uint32_t epoch, uint32_t seq,
                             int32_t base, uint32_t gain, uint32_t spend) {
    uint8_t sk[COOP_SK_LEN], pk[COOP_PK_LEN], ctx[16], m[96], *at = m;
    coop_sign_keypair(seed, sk, pk);
    coop_blake2b(ctx, sizeof ctx, code, strlen(code), "nx2navi-coop/3 sign", 19);
    memcpy(at, "nx2navi-coop/3 pool", 19), at += 19;
    memcpy(at, ctx, sizeof ctx), at += sizeof ctx;
    *at++ = (uint8_t)cat;
    at = put_le(at, hash, 4), at = put_le(at, epoch, 4), at = put_le(at, seq, 4), at = put_le(at, (uint32_t)base ^ 0x80000000u, 4);
    at = put_le(at, gain, 4), at = put_le(at, spend, 4);
    *p++ = COOP_R_POOL, *p++ = (uint8_t)cat;
    p = put_le(p, hash, 4);
    memcpy(p, pk, sizeof pk), p += sizeof pk;
    p = put_le(p, epoch, 4), p = put_le(p, seq, 4), p = put_le(p, (uint32_t)base ^ 0x80000000u, 4), p = put_le(p, gain, 4), p = put_le(p, spend, 4);
    coop_sign(p, sk, m, (size_t)(at - m));
    return p + COOP_SIG_LEN;
}

// The section in buf (n bytes, its entries' count set) as member signer's game sends it: from the fifth review on with
// that game's next counter and its signature for its member id and session (coop_sync_sign_section), before as it is.
// Its length, 0 when it does not fit cap.
static size_t seal_as(const Game *signer, uint8_t *buf, size_t n, size_t cap) {
#if H5_SIGNED
    return signer ? coop_sync_sign_section(signer->eng, buf, n, cap) : n;
#else
    (void)signer, (void)buf, (void)cap;
    return n;
#endif
}

// A SECTION message of the entries in [b + 6, end) (count of them) from member from (signed by its game) to games a and b.
static void inject(Game *a, Game *b, int from, uint8_t *buf, uint8_t *end, int count, uint64_t now) {
    buf[0] = COOP_K_DIGEST_VER, buf[1] = 0xEE, buf[2] = 0xFF, buf[3] = 0xC0, buf[4] = 0, buf[5] = (uint8_t)count;
    size_t n = seal_as(from >= 1 && from <= COOP_MAX_MEMBERS ? by_id[from] : NULL, buf, (size_t)(end - buf), COOP_MAX_MESSAGE);
    coop_sync_receive(a->eng, from, COOP_K_SECTION, buf, n, now);
    if (b) coop_sync_receive(b->eng, from, COOP_K_SECTION, buf, n, now);
}

// Records under n keys nobody admitted (seeds tag, 1..n), gains and spends of 1 that cancel, from member from to g.
static uint64_t inject_strangers(Game *g, int from, const char *code, int tag, int n, uint64_t now) {
    uint8_t buf[COOP_MAX_PAYLOAD], *p = buf + 6;
    int count = 0;
    for (int k = 1; k <= n; k++) {
        uint8_t seed[32] = {(uint8_t)tag, (uint8_t)k, (uint8_t)(k >> 8)};
        p = entry_record(p, code, COOP_CAT_KEYS, KEYS_FOREST, seed, 0, 1, 0, k & 1, !(k & 1));
        if (++count == 9 || k == n) inject(g, NULL, from, buf, p, count, now += 100), p = buf + 6, count = 0;
    }
    return now;
}

static void probe_hostile(void) {
    uint8_t msg[15];
    for (int i = 0; i < 15; i++) msg[i] = (uint8_t)i;
    uint64_t k0 = 0x0706050403020100ull, k1 = 0x0f0e0d0c0b0a0908ull;   // the paper's key 00..0f and message 00..0e
    CHECK(coop_siphash24(k0, k1, msg, 15) == 0xa129ca6149be45e5ull && coop_siphash24(k0, k1, msg, 0) == 0x726fdb47dd0e0e31ull,
          "SipHash-2-4 reference vectors");

    Net net = {0};
    Game a, b, h;
    game_init(&a, 1, 0x61);
    game_init(&b, 2, 0x62);
    game_init(&h, 3, 0x63);
    defaults(&a, 1), defaults(&b, 0), defaults(&h, 0);
    flag_of(&a, KEYS_FOREST)->value = 2;
    net.games[0] = &a, net.games[1] = &b, net.games[2] = &h, net.present[0] = net.present[1] = net.present[2] = 1;
    uint64_t now = run(&net, (net_roster(&net, "HOST01", 1000), 1000), 4000);
    uint8_t buf[COOP_MAX_PAYLOAD], *p = buf + 6;
    static const uint8_t stranger[32] = {0x99};
    for (uint32_t k = 0; k < 10; k++) p = entry_flag(p, COOP_CAT_PROGRESS, 0xDEAD0000 + k, 1);   // not flags of the game
    p = entry_flag(p, COOP_CAT_PROGRESS, 5, 2);                                                    // a flag at 2: frozen
    p = entry_flag(p, COOP_CAT_ITEMS, 0x1000, 1000);                                               // an item out of range
    p = entry_record(p, "HOST01", COOP_CAT_KEYS, KEYS_FOREST, stranger, 0, 1, 0, 0, 1u << 20);     // keys to 0 under a stranger's key
    inject(&a, &b, 3, buf, p, 13, now);
    now = run(&net, now, 2000);
    a.f[4].value = 1;   // the honest value of flag 5
    now = run(&net, now, 3000);
    int64_t ka = -1, kb = -1;
    coop_sync_pool(a.eng, KEYS_FOREST, &ka);
    coop_sync_pool(b.eng, KEYS_FOREST, &kb);
    CHECK(!coop_sync_flag(a.eng, COOP_CAT_PROGRESS, 0xDEAD0001) && !coop_sync_flag(b.eng, COOP_CAT_PROGRESS, 0xDEAD0009),
          "hostile: hashes the game does not share are not stored");
    CHECK(value_of(&b, 5) == 1 && value_of(&a, 5) == 1, "hostile: a flag sent as 2 does not freeze it: the honest 1 reaches B (%d)", value_of(&b, 5));
    CHECK(value_of(&a, 0x1000) == 0 && value_of(&b, 0x1000) == 0, "hostile: an item of 1000 is not applied (%d)", value_of(&b, 0x1000));
    CHECK(ka == 2 && kb == 2 && value_of(&a, KEYS_FOREST) == 2 && value_of(&b, KEYS_FOREST) == 2,
          "hostile: a spend of 2^20 under a key the maker did not admit leaves the key pool at 2 (%lld %lld, shown %d %d)", (long long)ka,
          (long long)kb, value_of(&a, KEYS_FOREST), value_of(&b, KEYS_FOREST));
    CoopSyncStats st;
    coop_sync_stats(b.eng, &st);
    CHECK(st.rejected >= 13 && !(coop_sync_ignored(b.eng) >> 3 & 1), "hostile: 13 refused values counted (%u), member 3 not yet ignored", st.rejected);
    // more of them: member 3 is ignored, even for a value the game would take
    p = buf + 6;
    for (uint32_t k = 0; k < 20; k++) p = entry_flag(p, COOP_CAT_SONGS, 0xBAD00000 + k, 1);
    inject(&a, &b, 3, buf, p, 20, now);
    p = entry_flag(buf + 6, COOP_CAT_PROGRESS, 7, 1);
    inject(&a, &b, 3, buf, p, 1, now + 10);
    CHECK((coop_sync_ignored(a.eng) >> 3 & 1) && (coop_sync_ignored(b.eng) >> 3 & 1) && !coop_sync_flag(b.eng, COOP_CAT_PROGRESS, 7),
          "hostile: after 32 refused flags member 3 is ignored (%x)", coop_sync_ignored(b.eng));
    // a flood from member 2 at one instant: past its rate the messages are dropped
    coop_sync_stats(a.eng, &st);
    unsigned dropped = st.rate_dropped;
    p = buf + 6;
    for (int k = 0; k < 100; k++) p = entry_flag(p, COOP_CAT_PROGRESS, 1 + (uint32_t)k, 0);
    for (int k = 0; k < 200; k++) inject(&a, NULL, 2, buf, p, 100, now + 20);
    coop_sync_stats(a.eng, &st);
    CHECK(st.rate_dropped > dropped + 100, "hostile: a flood of 200 KB at once from one member is rate limited (%u dropped)", st.rate_dropped - dropped);
    CHECK(!a.lowered && !b.lowered, "hostile: no apply lowered a value");
    game_free(&a), game_free(&b), game_free(&h);
    free(net.q);

    // 600 records under keys nobody admitted: none is stored
    Game c, d;
    Net n2 = {0};
    game_init(&c, 1, 0x71);
    game_init(&d, 2, 0x72);
    defaults(&c, 1), defaults(&d, 0);
    n2.games[0] = &c, n2.games[1] = &d, n2.present[0] = n2.present[1] = 1;
    now = run(&n2, (net_roster(&n2, "HOST02", now), now), 3000);
    CoopSyncStats before;
    coop_sync_stats(c.eng, &before);
    now = inject_strangers(&c, 2, "HOST02", 0x55, 600, now);
    coop_sync_stats(c.eng, &st);
    CHECK(st.regs == before.regs && st.probe_max < 40, "600 records under keys the maker did not admit make %u registers (longest probe %u)",
          st.regs - before.regs, st.probe_max);
    game_free(&c), game_free(&d);
    free(n2.q);
    printf("coop-probe: sync hostile member: refused values are not stored, passed on or applied, a frozen flag still merges, the "
           "member is ignored, a flood is limited, records of strangers are not stored (probe %u)\n", st.probe_max);
}

// One record of member from for pool hash in its next sends: the game runs (frames and ticks) until it sends one,
// and what it sent is delivered. 1 and the entry's bytes when found.
static int catch_record(Net *net, Game *g, uint32_t hash, uint64_t *now, uint8_t out[REC_BYTES]) {
    Net cap = *net;
    cap.q = NULL, cap.n = cap.cap = 0;
    int found = 0;
    for (int t = 0; t < 100 && !found; t++) {
        *now += 8;
        game_frame(g, *now);
        cap.sending = g->id;
        coop_sync_tick(g->eng, *now, net_send, &cap);
        for (int i = 0; i < cap.n && !found; i++) {
            Packet *q = &cap.q[i];
            if (q->kind != COOP_K_SECTION) continue;
            size_t at = 6;
            for (int e = 0; e < q->b[5] && at < q->n && !found; e++) {
                if (q->b[at] == COOP_R_POOL && le32(q->b + at + 2) == hash) memcpy(out, q->b + at, REC_BYTES), found = 1;
                at += q->b[at] == COOP_R_POOL ? REC_BYTES : 10;
            }
        }
        net_flush(&cap, *now);
    }
    free(cap.q);
    return found;
}

// Pools against a member who knows the secret: its own records take at most what the pool can hold, and the maker's
// removal gives that back; a member's keys filling the pool change nothing; nobody changes another's record.
static void probe_pool_hostile(void) {
    Net net = {0};
    Game a, b, h;
    game_init(&a, 1, 0x161), game_init(&b, 2, 0x162), game_init(&h, 3, 0x163);
    defaults(&a, 1), defaults(&b, 0), defaults(&h, 0);
    flag_of(&a, KEYS_FOREST)->value = 2;
    net.games[0] = &a, net.games[1] = &b, net.games[2] = &h, net.present[0] = net.present[1] = net.present[2] = 1;
    uint64_t now = run(&net, (net_roster(&net, "POOL01", 1000), 1000), 5000);
    flag_of(&h, KEYS_FOREST)->value -= 2000000;   // H's game says it spent two million keys: its own record, signed
    now = run(&net, now, 3000);
    int64_t v;
    coop_sync_pool(b.eng, KEYS_FOREST, &v);
    CHECK(v >= 0 && value_of(&b, KEYS_FOREST) == 0, "pool: a member's spend of 2^20 counts at most what entered the pool (pool %lld)",
          (long long)v);
    CHECK(coop_sync_remove(a.eng, 3) && !coop_sync_remove(b.eng, 1), "pool: the maker removes a member, another member cannot");
    net.present[2] = 0;
    now = run(&net, (net_roster(&net, "POOL01", now), now), 3000);
    coop_sync_pool(b.eng, KEYS_FOREST, &v);
    CHECK(v == 2 && value_of(&b, KEYS_FOREST) == 2 && value_of(&a, KEYS_FOREST) == 2, "pool: the maker's removal gives the pool back (pool %lld, shown %d %d)",
          (long long)v, value_of(&a, KEYS_FOREST), value_of(&b, KEYS_FOREST));
    game_free(&a), game_free(&b), game_free(&h);
    free(net.q);

    // a member fills the pool with 256 records under keys of its own; a joiner's door still spends a key for both
    Net n2 = {0};
    Game c, e, hh;
    game_init(&c, 1, 0x171), game_init(&e, 2, 0x172), game_init(&hh, 3, 0x173);
    defaults(&c, 1), defaults(&e, 0), defaults(&hh, 0);
    flag_of(&c, KEYS_FOREST)->value = 2;
    n2.games[0] = &c, n2.games[1] = &e, n2.games[2] = &hh, n2.present[0] = n2.present[2] = 1;
    now = run(&n2, (net_roster(&n2, "POOL02", now), now), 3000);
    now = inject_strangers(&c, 3, "POOL02", 0x66, 256, now);
    n2.present[1] = 1;
    now = run(&n2, (net_roster(&n2, "POOL02", now), now), 6000);
    flag_of(&e, KEYS_FOREST)->value -= 1;
    now = run(&n2, now, 3000);
    CHECK(value_of(&c, KEYS_FOREST) == 1 && value_of(&e, KEYS_FOREST) == 1, "pool: with 256 records of a member's keys sent, a joiner's door spends a key for both (%d %d)",
          value_of(&c, KEYS_FOREST), value_of(&e, KEYS_FOREST));
    game_free(&c), game_free(&e), game_free(&hh);
    free(n2.q);

    // Z sends Y's record with its gain raised, and Y's older record again: neither changes Y's record anywhere
    Net n3 = {0};
    Game x, y, z;
    game_init(&x, 1, 0x181), game_init(&y, 2, 0x182), game_init(&z, 3, 0x183);
    defaults(&x, 1), defaults(&y, 0), defaults(&z, 0);
    flag_of(&x, KEYS_FOREST)->value = 2;
    n3.games[0] = &x, n3.games[1] = &y, n3.games[2] = &z, n3.present[0] = n3.present[1] = n3.present[2] = 1;
    now = run(&n3, (net_roster(&n3, "POOL03", now), now), 5000);
    uint8_t old[REC_BYTES], buf[COOP_MAX_PAYLOAD];
    flag_of(&y, KEYS_FOREST)->value += 1;   // Y picks up a key: its record goes out
    int caught = catch_record(&n3, &y, KEYS_FOREST, &now, old);
    now = run(&n3, now, 3000);
    flag_of(&y, KEYS_FOREST)->value += 1;   // and another
    now = run(&n3, now, 3000);
    memcpy(buf + 6, old, REC_BYTES);
    inject(&x, NULL, 3, buf, buf + 6 + REC_BYTES, 1, now);   // the older one again
    uint32_t raised = 1u << 20, later = 1000;
    memcpy(buf + 6 + 6 + COOP_PK_LEN + 4, &later, 4);        // a later seq and its gain raised: the signature no longer matches
    memcpy(buf + 6 + 6 + COOP_PK_LEN + 12, &raised, 4);
    inject(&x, NULL, 3, buf, buf + 6 + REC_BYTES, 1, now + 10);
    now = run(&n3, now, 3000);
    int64_t vx, vy;
    coop_sync_pool(x.eng, KEYS_FOREST, &vx);
    coop_sync_pool(y.eng, KEYS_FOREST, &vy);
    CHECK(caught && vx == 4 && vy == 4 && value_of(&x, KEYS_FOREST) == 4, "pool: nobody but Y changes Y's record in X (X's pool %lld, Y's %lld)",
          (long long)vx, (long long)vy);
    game_free(&x), game_free(&y), game_free(&z);
    free(n3.q);
    printf("coop-probe: sync pools against a member: its spends take at most what a pool holds, the maker's removal gives them back, "
           "strangers' records and changed or old records change nothing\n");
}

// Acceptance is by identity: a removed player back with a new key, a copy of another's identity, a second player of
// an accepted name; and the accepted list does not fill up.
static void probe_identities(void) {
    Net net = {0};
    Game a, b, m, m2, t;
    game_init(&a, 1, 0x1D1), game_init(&b, 2, 0x1D2), game_init(&m, 3, 0x1D3);
    defaults(&a, 1), defaults(&b, 0), defaults(&m, 0);
    b.ask = 1;
    net.games[0] = &a, net.games[1] = &b, net.games[2] = &m, net.present[0] = net.present[1] = net.present[2] = 1;
    uint64_t now = run(&net, (net_roster(&net, "IDENT1", 1000), 1000), 3000);
    CoopOffer o;
    coop_sync_offer(b.eng, &o, now);
    CHECK(o.token && o.n == 2 && o.member[0].heard && o.member[1].heard && !strcmp(o.member[1].name, "P3") && o.member[0].maker && o.rules.maker == 1,
          "identity: B's offer names P1 (the maker) and P3, both identified (token %08x, %d members)", o.token, o.n);
    CHECK(coop_sync_accept(b.eng, o.token) && coop_sync_copy_wanted(b.eng), "identity: B accepts; a copy of the save is wanted");
    coop_sync_copied(b.eng, 1);
    now = run(&net, now, 1000);
    coop_sync_offer(b.eng, &o, now);
    CHECK(o.accepted && o.copy == 1 && coop_sync_open(b.eng), "identity: accepted and copied, values are applied");
    // the maker removes M; M starts again and comes back as member 4 under its name, with a new key
    coop_sync_remove(a.eng, 3);
    net.present[2] = 0;
    now = run(&net, (net_roster(&net, "IDENT1", now), now), 2000);
    game_init(&m2, 4, 0x1D4);
    coop_sync_name(m2.eng, "P3");
    defaults(&m2, 0);
    net.games[3] = &m2, net.present[3] = 1;
    now = run(&net, (net_roster(&net, "IDENT1", now), now), 3000);
    coop_sync_offer(b.eng, &o, now);
    CHECK(o.token && !o.accepted && !coop_sync_open(b.eng), "identity: a removed player back under its name with a new key is a new offer (accepted %d)",
          o.accepted);
    // the same key back in a new session (the relay would refuse it; a hostile relay might not): removed, ignored
    m.session ^= 0x10, m.id = 5;
    net.games[2] = &m, net.present[2] = 1;
    coop_sync_name(m.eng, "P3");
    now = run(&net, (net_roster(&net, "IDENT1", now), now), 3000);
    coop_sync_offer(b.eng, &o, now);
    int rm = 0;
    for (int i = 0; i < o.n; i++) rm |= o.member[i].id == 5 && o.member[i].removed;
    CHECK(rm, "identity: the removed key in a new session shows as removed and counts for nothing");
    net.present[2] = 0;
    coop_sync_accept(b.eng, o.token);
    now = run(&net, (net_roster(&net, "IDENT1", now), now), 2000);
    coop_sync_offer(b.eng, &o, now);
    CHECK(o.accepted, "identity: B accepts P1 and the new P3");
    // a copy of A's identity: A's digest from member 6, whose session is A's
    Net cap = {0};
    cap.sending = 1;
    coop_sync_tick(a.eng, now + 3000, net_send, &cap);
    Game fake;
    game_init(&fake, 6, 0x1D6);
    fake.session = a.session;
    net.games[2] = &fake, net.present[2] = 1;
    net_roster(&net, "IDENT1", now);
    for (int i = 0; i < cap.n; i++)
        if (cap.q[i].kind == COOP_K_DIGEST) coop_sync_receive(b.eng, 6, COOP_K_DIGEST, cap.q[i].b, cap.q[i].n, now);
    free(cap.q);
    coop_sync_offer(b.eng, &o, now);
    int copied = 0;
    for (int i = 0; i < o.n; i++) copied |= o.member[i].id == 6 && o.member[i].heard && !strcmp(o.member[i].name, "P1");
    CHECK(!copied, "identity: a member that sends another's identity in that one's session is not taken for it");
    net.present[2] = 0;
    now = run(&net, (net_roster(&net, "IDENT1", now), now), 1000);
    // a second player named P1 (A's name) with its own key
    game_init(&t, 7, 0x1D7);
    coop_sync_name(t.eng, "P1");
    defaults(&t, 0);
    net.games[2] = &t, net.present[2] = 1;
    now = run(&net, (net_roster(&net, "IDENT1", now), now), 3000);
    coop_sync_offer(b.eng, &o, now);
    CHECK(!o.accepted, "identity: a second player named P1 is a new offer");
    game_free(&t);
    // 70 players come and go, each accepted; then one more is accepted too
    for (int k = 0; k < 71; k++) {
        game_init(&t, 7, 0x2000 + (uint64_t)k);
        defaults(&t, 0);
        net.games[2] = &t;
        now = run(&net, (net_roster(&net, "IDENT1", now), now), 1200);
        coop_sync_offer(b.eng, &o, now);
        coop_sync_accept(b.eng, o.token);
        coop_sync_offer(b.eng, &o, now);
        if (k < 70) game_free(&t);
    }
    CHECK(o.token && o.accepted, "identity: after 70 acceptances a new player is still accepted (token %08x)", o.token);
    game_free(&t);
    game_free(&a), game_free(&b), game_free(&m), game_free(&m2), game_free(&fake);
    free(net.q);
    printf("coop-probe: sync identities: acceptance is by key; a removed player back, a copied identity and a second name are new or refused\n");
}

// The offer's number differs at each start of the game: one saved from an earlier start accepts nothing.
static void probe_offer_number(void) {
    Net net = {0};
    Game a, b;
    game_init(&a, 1, 0x1E1), game_init(&b, 2, 0x1E2);
    defaults(&a, 1), defaults(&b, 0);
    b.ask = 1;
    net.games[0] = &a, net.games[1] = &b, net.present[0] = net.present[1] = 1;
    uint64_t now = run(&net, (net_roster(&net, "OFFER1", 1000), 1000), 3000);
    CoopOffer o;
    coop_sync_offer(b.eng, &o, now);
    uint32_t first = o.token;
    game_free(&b);
    game_init(&b, 2, 0x1E2E2);   // B's game starts again: the same player, room and rules
    defaults(&b, 0);
    b.ask = 1;
    now = run(&net, (net_roster(&net, "OFFER1", now), now), 3000);
    coop_sync_offer(b.eng, &o, now);
    CHECK(first && o.token && o.token != first && !coop_sync_accept(b.eng, first),
          "offer: the number of the same room differs at the next start and the old one accepts nothing (%08x, %08x)", first, o.token);
    game_free(&a), game_free(&b);
    free(net.q);
    printf("coop-probe: sync offer number: keyed with a random value of each start\n");
}

// A rules change the player has not accepted hands out nothing, whenever the game takes values.
static void probe_rules_wait(void) {
    Net net = {0};
    Game a, b;
    game_init(&a, 1, 0x1F1), game_init(&b, 2, 0x1F2);
    settings(&a, 1, COOP_MODE_OWN, COOP_MODE_OWN, COOP_MODE_OWN, COOP_MODE_OWN, COOP_MODE_OWN);
    defaults(&b, 0);
    net.games[0] = &a, net.games[1] = &b, net.present[0] = net.present[1] = 1;
    uint64_t now = run(&net, (net_roster(&net, "WAIT01", 1000), 1000), 3000);
    a.f[300].value = 2;
    now = run(&net, now, 2000);
    b.ask = 1;   // B accepted the room with items own; from now on its player accepts nothing more
    settings(&a, 1, COOP_MODE_SHARED, COOP_MODE_OWN, COOP_MODE_OWN, COOP_MODE_OWN, COOP_MODE_OWN);
    now = run(&net, now, 4000);
    CoopRules rb;
    coop_sync_rules(b.eng, &rb);
    CHECK(rb.mode[COOP_CAT_ITEMS] == COOP_MODE_SHARED && coop_sync_flag(b.eng, COOP_CAT_ITEMS, 0x1000) == 2 && value_of(&b, 0x1000) == 0,
          "rules: items switched to shared merge, but nothing is applied before the player accepts (merged %u, applied %d)",
          coop_sync_flag(b.eng, COOP_CAT_ITEMS, 0x1000), value_of(&b, 0x1000));
    b.ask = 0;
    now = run(&net, now, 1000);
    CHECK(value_of(&b, 0x1000) == 2, "rules: and applied once accepted (%d)", value_of(&b, 0x1000));
    game_free(&a), game_free(&b);
    free(net.q);
    printf("coop-probe: sync rules wait: a rules change applies nothing before it is accepted\n");
}

// A save loaded in the room is a new offer: nothing reaches it before the player accepts again (and a copy is made);
// then it gets the room's progress, which holds what the last save brought.
static void probe_save_switch(void) {
    Net net = {0};
    Game x, p;
    game_init(&x, 1, 0x201), game_init(&p, 2, 0x202);
    defaults(&x, 1), defaults(&p, 0);
    for (int k = 0; k < 10; k++) x.f[k].value = 1;
    net.games[0] = &x, net.games[1] = &p, net.present[0] = net.present[1] = 1;
    uint64_t now = run(&net, (net_roster(&net, "SAVE01", 1000), 1000), 4000);
    x.ask = 1;
    reload_save(&x);   // another save, the same room
    now = run(&net, now, 4000);
    int got = 0;
    for (uint32_t h = 1; h <= 10; h++) got += value_of(&x, h) != 0;
    CoopOffer o;
    coop_sync_offer(x.eng, &o, now);
    CHECK(!got && o.token && !o.accepted && o.new_save, "save switch: the save loaded in the room gets nothing before the player accepts again (%d flags, new save %d)",
          got, o.new_save);
    CHECK(coop_sync_accept(x.eng, o.token) && coop_sync_copy_wanted(x.eng), "save switch: accepting it asks for a copy of the save first");
    CoopApply ap[16];
    CHECK(!coop_sync_open(x.eng) && !coop_sync_take(x.eng, ap, 16), "save switch: nothing is handed out before the copy is made");
    x.ask = 0;
    now = run(&net, now, 3000);
    got = 0;
    for (uint32_t h = 1; h <= 10; h++) got += value_of(&x, h) != 0;
    CHECK(got == 10, "save switch: once accepted and copied the save gets the room's progress, the last save's included (%d)", got);
    game_free(&x), game_free(&p);
    free(net.q);
    printf("coop-probe: sync save switch: another save in the room is a new offer and a new copy\n");
}

// The rules' key is pinned when the player accepts: a member the relay marks as the maker later (a restart, a hostile
// relay) changes nothing, the maker's own changes still hold, and a newcomer takes nobody's rules while the members
// disagree about who made the room.
static void probe_maker_pinned(void) {
    Net net = {0}, alone = {0};
    Game a, b, h, d;
    game_init(&a, 1, 0x211), game_init(&b, 2, 0x212), game_init(&h, 3, 0x213), game_init(&d, 4, 0x214);
    settings(&a, 1, COOP_MODE_SHARED, COOP_MODE_POOL, COOP_MODE_OWN, COOP_MODE_OWN, COOP_MODE_SHARED);   // rupees own
    defaults(&b, 0);
    net.games[0] = &a, net.games[1] = &b, net.present[0] = net.present[1] = 1;
    uint64_t now = run(&net, (net_roster(&net, "PIN001", 1000), 1000), 3000);
    // H made the room for itself while the relay had lost it, with rupees pooled
    settings(&h, 1, COOP_MODE_SHARED, COOP_MODE_POOL, COOP_MODE_POOL, COOP_MODE_POOL, COOP_MODE_SHARED);
    alone.games[0] = &h, alone.present[0] = 1;
    now = run(&alone, (net_roster(&alone, "PIN001", now), now), 2000);
    // the relay marks H as the maker of the room A and B are in
    a.made = 0;
    net.games[2] = &h, net.present[2] = 1;
    now = run(&net, (net_roster(&net, "PIN001", now), now), 4000);
    CoopRules rb;
    coop_sync_rules(b.eng, &rb);
    CoopOffer o;
    coop_sync_offer(b.eng, &o, now);
    CHECK(rb.mode[COOP_CAT_RUPEES] == COOP_MODE_OWN && rb.maker == 1 && o.maker_moved,
          "maker: a member the relay marks later cannot change the rules (rupees %s, from member %d, moved %d)", coop_mode_name(rb.mode[COOP_CAT_RUPEES]),
          rb.maker, o.maker_moved);
    settings(&a, 0, COOP_MODE_SHARED, COOP_MODE_POOL, COOP_MODE_OWN, COOP_MODE_POOL, COOP_MODE_SHARED);   // A pools ammunition
    now = run(&net, now, 3000);
    coop_sync_rules(b.eng, &rb);
    CHECK(rb.mode[COOP_CAT_AMMO] == COOP_MODE_POOL && rb.mode[COOP_CAT_RUPEES] == COOP_MODE_OWN,
          "maker: the maker's key still changes the rules without the relay's mark (ammo %s)", coop_mode_name(rb.mode[COOP_CAT_AMMO]));
    // A leaves; D joins while H carries its own rules and B the maker's
    net.present[0] = 0, h.made = 0;
    d.ask = 1;
    defaults(&d, 0);
    net.games[3] = &d, net.present[3] = 1;
    now = run(&net, (net_roster(&net, "PIN001", now), now), 4000);
    CoopRules rd;
    coop_sync_rules(d.eng, &rd);
    CHECK(!rd.epoch && !coop_sync_offer(d.eng, &o, now), "maker: a newcomer takes nobody's rules while the members disagree about the maker (epoch %u)",
          rd.epoch);
    game_free(&a), game_free(&b), game_free(&h), game_free(&d);
    free(net.q), free(alone.q);
    printf("coop-probe: sync maker pinned: rules only from the key the player accepted\n");
}

// Rupees pooled: A binds with 500 and spends them all, then B joins with 500, so B's base is 1000, above the counter's
// limit of 999. That is honest: nobody ignores B, and B's progress still reaches A after a minute and a half.
static void probe_honest_base(void) {
    Net net = {0};
    Game a, b;
    game_init(&a, 1, 0x91), game_init(&b, 2, 0x92);
    settings(&a, 1, COOP_MODE_SHARED, COOP_MODE_POOL, COOP_MODE_POOL, COOP_MODE_OWN, COOP_MODE_SHARED);
    settings(&b, 0, COOP_MODE_SHARED, COOP_MODE_POOL, COOP_MODE_POOL, COOP_MODE_OWN, COOP_MODE_SHARED);
    flag_of(&a, RUPEE)->cap = flag_of(&b, RUPEE)->cap = 999;
    flag_of(&a, RUPEE)->value = 500;
    flag_of(&b, RUPEE)->value = 500;
    net.games[0] = &a, net.games[1] = &b, net.present[0] = 1;
    uint64_t now = run(&net, (net_roster(&net, "BASE01", 1000), 1000), 3000);
    flag_of(&a, RUPEE)->value = 0;
    now = run(&net, now, 3000);
    net.present[1] = 1;
    now = run(&net, (net_roster(&net, "BASE01", now), now), 90000);
    b.f[40].value = 1;
    now = run(&net, now, 3000);
    CHECK(!(coop_sync_ignored(a.eng) >> 2 & 1) && value_of(&a, 41) == 1 && value_of(&a, RUPEE) == 500,
          "an honest joiner whose pool base is above the counter's limit is not ignored (ignored %x, its flag %d, rupees %d)",
          coop_sync_ignored(a.eng), value_of(&a, 41), value_of(&a, RUPEE));
    game_free(&a), game_free(&b);
    free(net.q);
    printf("coop-probe: sync honest base: a pool base above the counter's limit after spends is taken, and its sender is not ignored\n");
}

// A member asks for every section every 100 ms (its digests with random sums) of a game whose state is 60 KB: a member
// who joins meanwhile still gets the whole state, because the game answers its peers in turn.
static uint32_t big_limit(int cat, uint32_t hash, void *user) {
    (void)user;
    return cat == COOP_CAT_PROGRESS && hash >= 0x10000 && hash < 0x20000 ? 1 : 0;
}

static void probe_fair_answers(void) {
    Net net = {0};
    Game h, a, c;
    game_init(&h, 1, 0xB1), game_init(&a, 2, 0xB2), game_init(&c, 3, 0xB3);
    coop_sync_limits(h.eng, big_limit, NULL), coop_sync_limits(a.eng, big_limit, NULL), coop_sync_limits(c.eng, big_limit, NULL);
    defaults(&a, 1), defaults(&h, 0), defaults(&c, 0);
    net.games[0] = &h, net.games[1] = &a, net.games[2] = &c, net.present[0] = net.present[1] = 1;
    uint64_t now = 1000;
    net_roster(&net, "FAIR01", now);
    for (uint32_t k = 0; k < 6000; k++) coop_sync_local(a.eng, COOP_CAT_PROGRESS, 0x10000 + k, 1, now);
    now = run(&net, now, 3000);
    Net cap = {0};
    cap.sending = 1;
    coop_sync_tick(h.eng, now + 3000, net_send, &cap);   // H's own digest: its identity checks out
    Packet d = cap.q[0];
    free(cap.q);
    net.present[2] = 1;
    net_roster(&net, "FAIR01", now);
    for (int t = 0; t < 300; t++) {   // 30 s
        for (size_t k = 0; k < 4 * COOP_SYNC_SECTIONS; k++) d.b[d.n - 4 * COOP_SYNC_SECTIONS + k] = (uint8_t)rnd();
        coop_sync_receive(a.eng, 1, COOP_K_DIGEST, d.b, d.n, now);
        now = run(&net, now, 100);
    }
    unsigned got = 0;
    for (uint32_t k = 0; k < 6000; k++) got += coop_sync_flag(c.eng, COOP_CAT_PROGRESS, 0x10000 + k) == 1;
    CHECK(d.kind == COOP_K_DIGEST && got == 6000, "a late joiner gets the whole state in 30 s while another member asks for everything every 100 ms (%u of 6000)", got);
    game_free(&h), game_free(&a), game_free(&c);
    free(net.q);
    printf("coop-probe: sync fair answers: a member asking for everything all the time does not starve a late joiner\n");
}

// ---- 7. hostile bytes

static uint32_t fuzz_limit(int cat, uint32_t hash, void *user) {
    (void)user;
    return hash & 1 ? 0 : coop_cat_is_pool(cat) ? 999 : 1;
}

static void probe_sync_fuzz(void) {
    CoopSync *s = coop_sync_new(1, 0xC0FFEE);
    coop_sync_limits(s, fuzz_limit, NULL);
    CoopRules r;
    coop_rules_default(&r);
    coop_sync_settings(s, &r, 1);
    int ids[2] = {1, 2};
    coop_sync_room(s, "FUZZ01", 1, ids, NULL, 2, 1, 1000);
    Net none = {0};
    coop_sync_tick(s, 1000, net_send, &none);
    uint8_t b[COOP_MAX_PAYLOAD];
    for (int i = 0; i < 200000; i++) {
        size_t n = rnd() % sizeof b;
        for (size_t k = 0; k < n; k++) b[k] = (uint8_t)rnd();
        if (n > 6 && rnd() % 2) b[0] = COOP_K_DIGEST_VER, b[1] = 0xEE, b[2] = 0xFF, b[3] = 0xC0, b[4] = 0;   // a valid header
        coop_sync_receive(s, 1 + (int)(rnd() % 9), (uint8_t)(rnd() % 2 ? COOP_K_SECTION : COOP_K_DIGEST), b, n, 1000 + (uint64_t)i);
        if (i % 1000 == 0) {
            coop_sync_tick(s, 1000 + (uint64_t)i, net_send, &none);
            none.n = 0;
            CoopApply a[64];
            while (coop_sync_take(s, a, 64) > 0) {}
        }
    }
    CoopSyncStats st;
    coop_sync_stats(s, &st);
    CHECK(st.regs <= 65536, "registers stay bounded (%u)", st.regs);
    coop_sync_free(s);
    free(none.q);
    printf("coop-probe: sync fuzz: 200000 random messages, %u registers made\n", st.regs);
}

// ---- 8. the third review's findings

// The identity of a DIGEST (the key, from the fifth review on the session before, the name, the signature) of member id
// in session (name: nl raw bytes) of room code, signed with (sk, pk), at p; past its end. bad: the signature broken.
static uint8_t *put_identity(uint8_t *p, const char *code, const uint8_t sk[COOP_SK_LEN], const uint8_t pk[COOP_PK_LEN], int id, uint64_t session,
                             uint64_t prev, const char *name, size_t nl, int bad) {
    uint8_t ctx[16], m[200], *at = m;
    coop_blake2b(ctx, sizeof ctx, code, strlen(code), "nx2navi-coop/3 sign", 19);
    memcpy(at, H5_SIGNED ? "nx2navi-coop/4 member" : "nx2navi-coop/3 member", 21), at += 21;
    memcpy(at, ctx, sizeof ctx), at += sizeof ctx;
    *at++ = (uint8_t)id;
    at = put_le(at, session, 8);
    if (H5_SIGNED) at = put_le(at, prev, 8);
    memcpy(at, pk, COOP_PK_LEN), at += COOP_PK_LEN;
    *at++ = (uint8_t)nl;
    memcpy(at, name, nl), at += nl;
    memcpy(p, pk, COOP_PK_LEN), p += COOP_PK_LEN;
    if (H5_SIGNED) p = put_le(p, prev, 8);
    *p++ = (uint8_t)nl;
    memcpy(p, name, nl), p += nl;
    coop_sign(p, sk, m, (size_t)(at - m));
    if (bad) p[bad % COOP_SIG_LEN] ^= (uint8_t)(1 + bad / COOP_SIG_LEN);
    return p + COOP_SIG_LEN;
}

// A DIGEST of an identity signed with the key of seed for member id in session (name: nl raw bytes), schema 0xC0FFEE,
// with a charter (pk, epoch, a signature that is not one) when charter_pk is set. bad: the identity's signature broken.
static size_t make_digest(uint8_t *out, const char *code, const uint8_t seed[32], int id, uint64_t session, const char *name, size_t nl,
                          int bad, const uint8_t *charter_pk, uint32_t epoch) {
    uint8_t sk[COOP_SK_LEN], pk[COOP_PK_LEN], *p = out;
    coop_sign_keypair(seed, sk, pk);
    *p++ = COOP_K_DIGEST_VER;
    p = put_le(p, 0xC0FFEE, 4);
    p = put_identity(p, code, sk, pk, id, session, 0, name, nl, bad);
    *p++ = charter_pk != NULL;
    if (charter_pk) {
        p = put_le(p, epoch, 4);
        *p++ = COOP_CAT_N;
        static const uint8_t modes[COOP_CAT_N] = {1, 1, 1, 1, 2, 0, 0};
        memcpy(p, modes, COOP_CAT_N), p += COOP_CAT_N;
        p = put_le(p, 0, 4);
        *p++ = 0, *p++ = 0;
        memcpy(p, charter_pk, COOP_PK_LEN), p += COOP_PK_LEN;
        for (int i = 0; i < COOP_SIG_LEN; i++) *p++ = (uint8_t)(epoch * 31 + (uint32_t)i);
    }
    *p++ = COOP_SYNC_SECTIONS;
    memset(p, 0, 4 * COOP_SYNC_SECTIONS), p += 4 * COOP_SYNC_SECTIONS;
    return (size_t)(p - out);
}

// The offer's member of that id (NULL when none).
static const CoopOfferMember *offer_member(const CoopOffer *o, int id) {
    for (int i = 0; i < o->n; i++)
        if (o->member[i].id == id) return &o->member[i];
    return NULL;
}

static int flags_held(Game **gs, int n, uint32_t first, uint32_t last) {
    int got = 0;
    for (int k = 0; k < n; k++)
        for (uint32_t h = first; h <= last; h++) got += coop_sync_flag(gs[k]->eng, COOP_CAT_PROGRESS, h) != 0 || value_of(gs[k], h) > 0;
    return got;
}

// Finding 1: values only from keys this player accepted, and no acceptance while a member is not identified.
static void probe_h3_accepted_only(void) {
    Net net = {0};
    Game a, b, c, m, u;
    game_init(&a, 1, 0x3101), game_init(&b, 2, 0x3102), game_init(&c, 3, 0x3103), game_init(&m, 4, 0x3104), game_init(&u, 4, 0x3105);
    defaults(&a, 1), defaults(&b, 0), defaults(&c, 0), defaults(&m, 0), defaults(&u, 0);
    net.games[0] = &a, net.games[1] = &b, net.games[2] = &c;
    net.present[0] = net.present[1] = net.present[2] = 1;
    uint64_t now = run(&net, (net_roster(&net, "H3F1", 1000), 1000), 3000);
    a.ask = b.ask = c.ask = 1;   // the three accepted each other; their players accept nothing more by themselves
    Game *abc[3] = {&a, &b, &c};
    CoopOffer o;
    // M tells only B who it is and sends only B its flags (201..210, earned by nobody)
    for (int k = 200; k < 210; k++) m.f[k].value = 1;
    net.games[3] = &m, net.present[3] = 1, net.only[4] = 2;
    now = run(&net, (net_roster(&net, "H3F1", now), now), 8000);
    int at_b = flags_held(&abc[1], 1, 201, 210), at_c = flags_held(&abc[2], 1, 201, 210);
    coop_sync_offer(c.eng, &o, now);
    CHECK(!at_b, "F1: B, whose player did not accept M, stores none of M's flags (%d of 10)", at_b);
    CHECK(!at_c, "F1: C gets none of M's flags through B (%d of 10)", at_c);
    CHECK(o.token && !o.accepted, "F1: C's room is not accepted while M is in it unidentified (accepted %d)", o.accepted);
    // M tells everyone; nobody accepts it; M leaves: none of its flags arrives anywhere
    for (int k = 210; k < 220; k++) m.f[k].value = 1;
    net.only[4] = 0;
    now = run(&net, now, 4000);
    net.present[3] = 0;
    now = run(&net, (net_roster(&net, "H3F1", now), now), 4000);
    int got = flags_held(abc, 3, 211, 220);
    CHECK(!got, "F1: the flags of a newcomer nobody accepted reach nobody once it left (%d)", got);
    // M back (a new session), declined; the maker removes it, which opens the room again: still nothing of M's
    for (int k = 220; k < 230; k++) m.f[k].value = 1;
    m.session ^= 0x20;
    net.present[3] = 1;
    now = run(&net, (net_roster(&net, "H3F1", now), now), 4000);
    int removed_m = coop_sync_remove(a.eng, 4);
    now = run(&net, now, 4000);
    got = flags_held(abc, 3, 221, 230);
    coop_sync_offer(b.eng, &o, now);
    CHECK(removed_m && o.accepted && !got, "F1: the maker's removal opens the room again and none of M's flags is applied (%d, accepted %d)", got,
          o.accepted);
    // U never identifies; B's player accepts meanwhile; then U tells B who it is, sends its flags and leaves
    net.present[3] = 0;
    now = run(&net, (net_roster(&net, "H3F1", now), now), 1000);
    for (int k = 230; k < 240; k++) u.f[k].value = 1;
    net.games[3] = &u, net.present[3] = 1, net.mute[4] = 1;
    now = run(&net, (net_roster(&net, "H3F1", now), now), 6000);
    coop_sync_offer(b.eng, &o, now);
    int took = coop_sync_accept(b.eng, o.token);
    coop_sync_offer(b.eng, &o, now);
    CHECK(!o.accepted, "F1: a room with a member that never identified is not accepted (accept %d, accepted %d)", took, o.accepted);
    net.mute[4] = 0, net.only[4] = 2;
    now = run(&net, now, 4000);
    net.present[3] = 0;
    now = run(&net, (net_roster(&net, "H3F1", now), now), 4000);
    got = flags_held(abc, 3, 231, 240);
    CHECK(!got, "F1: the flags of a member that identified after the player accepted reach nobody (%d)", got);
    CHECK(!a.lowered && !b.lowered && !c.lowered, "F1: no apply lowered a flag");
    game_free(&a), game_free(&b), game_free(&c), game_free(&m), game_free(&u);
    free(net.q);
    printf("coop-probe: sync accepted keys only: no flag of a member nobody accepted is stored, passed on or applied, "
           "and a member that never identified keeps the room unaccepted\n");
}

// Finding 2: a drain takes at most what entered the pool; the maker's game, started again, removes the drainer who left,
// and the keys come back in every game; a pool reset brings them back too.
static void probe_h3_drain(void) {
    static const uint8_t maker_seed[32] = {0x4D, 0x4B, 0x33};   // the maker's key for the room, kept in its profile
    Net net = {0};
    Game a, b, m, a2;
    game_init(&a, 1, 0x3201), game_init(&b, 2, 0x3202), game_init(&m, 3, 0x3203);
    coop_sync_room_seed(a.eng, "H3F2", maker_seed);
    defaults(&a, 1), defaults(&b, 0), defaults(&m, 0);
    flag_of(&a, KEYS_FOREST)->value = 2;
    net.games[0] = &a, net.games[1] = &b, net.games[2] = &m, net.present[0] = net.present[1] = net.present[2] = 1;
    uint64_t now = run(&net, (net_roster(&net, "H3F2", 1000), 1000), 5000);
    CoopOffer o;
    coop_sync_offer(b.eng, &o, now);
    char fp[20] = "";
    if (offer_member(&o, 3)) snprintf(fp, sizeof fp, "%s", offer_member(&o, 3)->key);
    flag_of(&m, KEYS_FOREST)->value -= 2000000;   // M's own record spends two million keys
    now = run(&net, now, 3000);
    flag_of(&a, KEYS_FOREST)->value += 1;         // A picks up a key meanwhile
    now = run(&net, now, 3000);
    int64_t v;
    coop_sync_pool(b.eng, KEYS_FOREST, &v);
    CHECK(v >= 0 && value_of(&b, KEYS_FOREST) == 0, "F2: a drain takes at most what entered the pool, no debt beyond it (pool %lld)", (long long)v);
    // M leaves, and so does the maker's game
    net.present[2] = 0, net.present[0] = 0;
    now = run(&net, (net_roster(&net, "H3F2", now), now), 3000);
    // the maker's game starts again: a new engine and session, the room's key from the profile; the relay marks nobody
    game_init(&a2, 1, 0x3204);
    coop_sync_room_seed(a2.eng, "H3F2", maker_seed);
    defaults(&a2, 0);
    flag_of(&a2, KEYS_FOREST)->value = value_of(&a, KEYS_FOREST);   // its save as the drain left it
    net.games[0] = &a2, net.present[0] = 1;
    now = run(&net, (net_roster(&net, "H3F2", now), now), 6000);
    CHECK(coop_sync_is_maker(a2.eng), "F2: the maker's game, started again, holds the room's key and makes the rules again");
    uint64_t kid = 0;
    int parsed = coop_sync_key_parse(fp, &kid), removed_m = parsed && coop_sync_remove_key(a2.eng, kid);
    CHECK(removed_m, "F2: the maker removes M, who left, by its key %s (parsed %d)", fp, parsed);
    now = run(&net, now, 5000);
    CHECK(value_of(&b, KEYS_FOREST) == 3 && value_of(&a2, KEYS_FOREST) == 3,
          "F2: the keys come back in every game, the one picked up during the drain too (%d %d)", value_of(&b, KEYS_FOREST), value_of(&a2, KEYS_FOREST));
    game_free(&a), game_free(&b), game_free(&m), game_free(&a2);
    free(net.q);

    // the same drain; M leaves and nobody removes it; the maker resets the pools: they count what the players in the room
    // brought, and the keys come back
    Net n2 = {0};
    Game c, d, mm;
    game_init(&c, 1, 0x3211), game_init(&d, 2, 0x3212), game_init(&mm, 3, 0x3213);
    defaults(&c, 1), defaults(&d, 0), defaults(&mm, 0);
    flag_of(&c, KEYS_FOREST)->value = 2;
    n2.games[0] = &c, n2.games[1] = &d, n2.games[2] = &mm, n2.present[0] = n2.present[1] = n2.present[2] = 1;
    now = run(&n2, (net_roster(&n2, "H3F2R", now), now), 5000);
    flag_of(&mm, KEYS_FOREST)->value -= 2000000;
    now = run(&n2, now, 3000);
    n2.present[2] = 0;
    now = run(&n2, (net_roster(&n2, "H3F2R", now), now), 2000);
    int reset = coop_sync_reset_pools(c.eng);
    now = run(&n2, now, 8000);
    CHECK(reset && value_of(&c, KEYS_FOREST) == 2 && value_of(&d, KEYS_FOREST) == 2, "F2: after a pool reset the keys come back in every game (%d %d)",
          value_of(&c, KEYS_FOREST), value_of(&d, KEYS_FOREST));
#ifndef COOP_BEFORE
    CoopContributor who[8];
    int nw = coop_sync_contributors(c.eng, who, 8), seen = 0;
    for (int i = 0; i < nw; i++) seen |= who[i].member == 2;
    CHECK(nw >= 2 && seen, "F2: the maker's game lists the pools' contributors (%d)", nw);
#endif
    game_free(&c), game_free(&d), game_free(&mm);
    free(n2.q);
    printf("coop-probe: sync drain: a member's spends take at most what entered a pool; the maker's game, started again, removes the "
           "drainer who left, or resets the pools, and the keys come back everywhere\n");
}

// Finding 4: an identity of a session that ended is not taken again, after any number of other sessions or a room lost.
// A session ends when its member's identity in its next session names it (from the fifth review on: a roster that
// leaves the member out, or a relay that says the room is gone, ends nothing by itself, since anyone on the path can
// send those), so B joins again before each replay.
static Packet capture_digest(Game *g, uint64_t at) {   // g's digest as its engine sends it at that time
    Net cap = {0};
    cap.sending = g->id;
    coop_sync_tick(g->eng, at, net_send, &cap);
    Packet d = {0};
    for (int i = 0; i < cap.n; i++)
        if (cap.q[i].kind == COOP_K_DIGEST) d = cap.q[i];
    free(cap.q);
    return d;
}

static void probe_h3_replay(void) {
    Net net = {0};
    Game a, b, c;
    game_init(&a, 1, 0x3401), game_init(&b, 2, 0x3402), game_init(&c, 3, 0x3403);
    defaults(&a, 1), defaults(&b, 0), defaults(&c, 0);
    net.games[0] = &a, net.games[1] = &b, net.games[2] = &c, net.present[0] = net.present[1] = net.present[2] = 1;
    uint64_t now = run(&net, (net_roster(&net, "H3F4", 1000), 1000), 3000);
    Packet dig = capture_digest(&b, now + 3000);
    uint64_t bsess = b.session;
    // B joins again (and is heard in its new session), then leaves; then 64 sessions come and go
    b.session = bsess ^ 0x77;
    now = run(&net, (net_roster(&net, "H3F4", now), now), 4000);
    net.present[1] = 0;
    now = run(&net, (net_roster(&net, "H3F4", now), now), 1000);
    int ids[3] = {1, 3, 5};
    uint64_t sess[3] = {a.session, c.session, 0};
    for (int k = 0; k < 64; k++) sess[2] = 0x5000 + (uint64_t)k, coop_sync_room(c.eng, "H3F4", 3, ids, sess, 3, 1, now += 50);
    coop_sync_room(c.eng, "H3F4", 3, ids, sess, 2, 1, now += 50);
    // M: member 2 in B's first session, with B's identity
    int ids2[3] = {1, 2, 3};
    uint64_t sess2[3] = {a.session, bsess, c.session};
    coop_sync_room(c.eng, "H3F4", 3, ids2, sess2, 3, 1, now += 50);
    coop_sync_receive(c.eng, 2, COOP_K_DIGEST, dig.b, dig.n, now += 10);
    CoopOffer o;
    coop_sync_offer(c.eng, &o, now);
    const CoopOfferMember *x = offer_member(&o, 2);
    CHECK(dig.n && !(x && x->heard), "F4: B's identity in B's old session is not taken after 64 other sessions ended (heard %d)", x ? x->heard : -1);
    // B back in a new session; the room is lost while B is in it; everyone is back in new sessions; M takes B's session
    // from before
    b.session = bsess ^ 0x99;
    net.present[1] = 1;
    now = run(&net, (net_roster(&net, "H3F4", now), now), 4000);
    dig = capture_digest(&b, now + 3000);
    coop_sync_offer(c.eng, &o, now);
    x = offer_member(&o, 2);
    int heard_before = x && x->heard;
#if !H5_SIGNED
    coop_sync_room_lost(c.eng);
#endif
    for (int i = 0; i < 3; i++) coop_sync_room(net.games[i]->eng, "", 0, NULL, NULL, 0, 0, now + 50);
    a.session ^= 0xA0, b.session ^= 0xB0, c.session ^= 0xC0;
    now = run(&net, (net_roster(&net, "H3F4", now + 100), now + 100), 4000);
    uint64_t sess3[3] = {a.session, bsess ^ 0x99, c.session};
    coop_sync_room(c.eng, "H3F4", 3, ids2, sess3, 3, 1, now += 50);
    coop_sync_receive(c.eng, 2, COOP_K_DIGEST, dig.b, dig.n, now += 10);
    coop_sync_offer(c.eng, &o, now);
    x = offer_member(&o, 2);
    CHECK(heard_before && !(x && x->heard), "F4: after the room was lost, B's identity of the session before is not taken again (heard %d, %d)",
          heard_before, x ? x->heard : -1);
    game_free(&a), game_free(&b), game_free(&c);
    free(net.q);
    printf("coop-probe: sync replay: an identity of a session that ended is never taken again\n");
}

// Finding 5: the relay's mark gives no rules that the members' charters contradict, and a game's own mark is not taken
// for its own creation.
static void probe_h3_mark(void) {
    Net net = {0};
    Game a, b, h, n;
    game_init(&a, 1, 0x3501), game_init(&b, 2, 0x3502), game_init(&h, 3, 0x3503), game_init(&n, 4, 0x3504);
    defaults(&a, 1), defaults(&b, 0);
    net.games[0] = &a, net.games[1] = &b, net.present[0] = net.present[1] = 1;
    uint64_t now = run(&net, (net_roster(&net, "H3F5", 1000), 1000), 3000);
    // A is gone; H made the room again (the relay marks it) and signs rules of its own
    net.present[0] = 0;
    defaults(&h, 1), h.ask = 1;
    net.games[2] = &h, net.present[2] = 1;
    now = run(&net, (net_roster(&net, "H3F5", now), now), 4000);
    defaults(&n, 0), n.ask = 1;
    net.games[3] = &n, net.present[3] = 1;
    now = run(&net, (net_roster(&net, "H3F5", now), now), 6000);
    CoopRules rn;
    coop_sync_rules(n.eng, &rn);
    CHECK(!(rn.epoch && rn.maker == 3), "F5: a newcomer does not take the rules of the marked member while another member carries another key's (rules by %d)",
          rn.maker);
#ifndef COOP_BEFORE
    CoopOffer o;
    coop_sync_offer(n.eng, &o, now);
    CHECK(o.disagree, "F5: the newcomer's offer says the members carry rules of different keys");
#endif
    game_free(&a), game_free(&b), game_free(&h), game_free(&n);
    free(net.q);

    // a hostile relay marks the newcomer X as the maker in X's own roster
    Net n2 = {0};
    Game p, q, x;
    game_init(&p, 1, 0x3511), game_init(&q, 2, 0x3512), game_init(&x, 3, 0x3513);
    defaults(&p, 1), defaults(&q, 0), defaults(&x, 0);
    n2.games[0] = &p, n2.games[1] = &q, n2.present[0] = n2.present[1] = 1;
    now = run(&n2, (net_roster(&n2, "H3F5X", now), now), 3000);
    n2.games[2] = &x, n2.present[2] = 1;
    net_roster(&n2, "H3F5X", now);
    int ids[3] = {1, 2, 3};
    uint64_t sess[3] = {p.session, q.session, x.session};
    coop_sync_room(x.eng, "H3F5X", 3, ids, sess, 3, 3, now);
    now = run(&n2, now, 6000);
    CoopRules rx;
    coop_sync_rules(x.eng, &rx);
    CHECK(rx.epoch && rx.maker == 1, "F5: a newcomer the relay marks in its own roster takes the maker's rules, not its own (epoch %u, by %d)", rx.epoch,
          rx.maker);
    game_free(&p), game_free(&q), game_free(&x);
    free(n2.q);
    printf("coop-probe: sync mark: the relay's maker mark gives no rules the members' charters contradict\n");
}

// Findings 6 and 8: a signed name is shown cleaned; identities and charters that fail their check are not checked twice
// for the same bytes and are capped. From the sixth review on they do not count against the member a digest names: a
// digest is not signed by its sender, so anyone with the room's secret can send one in that member's name.
static void probe_h3_signatures(void) {
    Net net = {0};
    Game a, b;
    game_init(&a, 1, 0x3601), game_init(&b, 2, 0x3602);
    defaults(&a, 1), defaults(&b, 0);
    net.games[0] = &a, net.games[1] = &b, net.present[0] = net.present[1] = 1;
    uint64_t now = run(&net, (net_roster(&net, "H3F6", 1000), 1000), 3000);
    CoopSyncStats st0;   // the checks before the attack (A's own signed sections among them) are not the attack's
    coop_sync_stats(b.eng, &st0);
    int ids[4] = {1, 2, 4, 5};
    uint64_t sess[4] = {a.session, b.session, 0x4444, 0x5555};
    coop_sync_room(b.eng, "H3F6", 2, ids, sess, 4, 1, now);
    static const char evil[] = "Ev\x1b[31m\nroom accepted\xe2\x80\xae";
    uint8_t d[COOP_MAX_PAYLOAD], seed4[32] = {4, 4}, seed5[32] = {5, 5};
    size_t dn = make_digest(d, "H3F6", seed4, 4, 0x4444, evil, sizeof evil - 1, 0, NULL, 0);
    coop_sync_receive(b.eng, 4, COOP_K_DIGEST, d, dn, now += 10);
    CoopOffer o;
    coop_sync_offer(b.eng, &o, now + 6000);
    const CoopOfferMember *x = offer_member(&o, 4);
    int raw = 0;
    for (const char *c = x ? x->name : ""; *c; c++) raw |= (uint8_t)*c < 0x20 || *c == 0x7f || !memcmp(c, "\xe2\x80\xae", 3);
    CHECK(x && x->heard && !raw, "F6: a signed name with control and reordering characters is shown cleaned (heard %d)", x ? x->heard : -1);
    // member 5: an identity that never checks out, the same bytes again and again, then new ones
    size_t bn = make_digest(d, "H3F6", seed5, 5, 0x5555, "Mal", 3, 1, NULL, 0);
    uint64_t t0 = now;
    CoopSyncStats st1, st5;
    coop_sync_stats(b.eng, &st5);
    for (int i = 0; i < 300; i++) coop_sync_receive(b.eng, 5, COOP_K_DIGEST, d, bn, now += 3);
    coop_sync_stats(b.eng, &st1);
    for (int i = 0; i < 300; i++) bn = make_digest(d, "H3F6", seed5, 5, 0x5555, "Mal", 3, 2 + i, NULL, 0), coop_sync_receive(b.eng, 5, COOP_K_DIGEST, d, bn, now += 3);
    coop_sync_offer(b.eng, &o, now);
    x = offer_member(&o, 5);
    CHECK(!(coop_sync_ignored(b.eng) >> 5 & 1) && x && !x->heard && st1.sig_checks - st5.sig_checks <= 1,
          "F8: an identity that fails its check again and again is not taken, its same bytes are checked once (%u checks), and it does not "
          "count against the member it names (ignored %u)", st1.sig_checks - st5.sig_checks, coop_sync_ignored(b.eng) >> 5 & 1);
    // member 4, identified: charters claiming A's key with a later epoch and no valid signature
    uint8_t apk[COOP_PK_LEN] = {0};
    Net cap = {0};
    cap.sending = 1;
    coop_sync_tick(a.eng, now + 3000, net_send, &cap);
    for (int i = 0; i < cap.n; i++)
        if (cap.q[i].kind == COOP_K_DIGEST) memcpy(apk, cap.q[i].b + 5, COOP_PK_LEN);
    free(cap.q);
    for (int i = 0; i < 300; i++)
        dn = make_digest(d, "H3F6", seed4, 4, 0x4444, evil, sizeof evil - 1, 0, apk, 1000 + (uint32_t)i), coop_sync_receive(b.eng, 4, COOP_K_DIGEST, d, dn, now += 3);
    CoopSyncStats st;
    coop_sync_stats(b.eng, &st);
    unsigned most = 2 * (256 + 128 * (unsigned)((now - t0) / 1000 + 1));
    CHECK(!(coop_sync_ignored(b.eng) >> 4 & 1) && st.forged - st0.forged >= 2 && st.sig_checks - st0.sig_checks <= most,
          "F8: charters in the maker's name that fail their check count against nobody (ignored %u), are counted as forged (%u), and the "
          "checks stay within the two members' budgets (%u checks in %llu ms, at most %u)", coop_sync_ignored(b.eng) >> 4 & 1,
          st.forged - st0.forged, st.sig_checks - st0.sig_checks, (unsigned long long)(now - t0), most);
    game_free(&a), game_free(&b);
    free(net.q);
    printf("coop-probe: sync signatures: signed names are shown cleaned; failed checks are made once for the same bytes, capped, and held against nobody\n");
}

// Finding 10: a game's own pool record is not rolled back after the room came back.
static void probe_h3_own_records(void) {
    Net net = {0};
    Game x, y, z;
    game_init(&x, 1, 0x3A01), game_init(&y, 2, 0x3A02), game_init(&z, 3, 0x3A03);
    defaults(&x, 1), defaults(&y, 0), defaults(&z, 0);
    flag_of(&x, KEYS_FOREST)->value = 2;
    net.games[0] = &x, net.games[1] = &y, net.games[2] = &z, net.present[0] = net.present[1] = net.present[2] = 1;
    uint64_t now = run(&net, (net_roster(&net, "H3F10", 1000), 1000), 5000);
    uint8_t old[REC_BYTES], buf[COOP_MAX_PAYLOAD];
    flag_of(&y, KEYS_FOREST)->value += 1;
    int caught = catch_record(&net, &y, KEYS_FOREST, &now, old);
    for (int i = 0; i < 3; i++) flag_of(&y, KEYS_FOREST)->value += 1, now = run(&net, now, 2000);
    // the relay lost the room; Y is back in it, and Z (identified to Y first) hands Y its own old record before the
    // room's records arrive
    coop_sync_room(y.eng, "", 0, NULL, NULL, 0, 0, now);
    net_roster(&net, "H3F10", now += 8);
    Net cap = {0};
    cap.sending = 3;
    coop_sync_tick(z.eng, now + 3000, net_send, &cap);
    for (int i = 0; i < cap.n; i++)
        if (cap.q[i].kind == COOP_K_DIGEST) coop_sync_receive(y.eng, 3, COOP_K_DIGEST, cap.q[i].b, cap.q[i].n, now);
    free(cap.q);
    memcpy(buf + 6, old, REC_BYTES);
    inject(&y, NULL, 3, buf, buf + 6 + REC_BYTES, 1, now += 1);
    now = run(&net, now, 5000);
    flag_of(&y, KEYS_FOREST)->value += 1;
    now = run(&net, now, 4000);
    int64_t vx, vy;
    coop_sync_pool(x.eng, KEYS_FOREST, &vx);
    coop_sync_pool(y.eng, KEYS_FOREST, &vy);
    CHECK(caught && vx == 7 && vy == 7, "F10: Y's own record is not rolled back: both pools hold 2 + 5 (X %lld, Y %lld)", (long long)vx, (long long)vy);
    game_free(&x), game_free(&y), game_free(&z);
    free(net.q);
    printf("coop-probe: sync own records: a game's own pool record survives the room coming back, and an old one changes nothing\n");
}

// Finding 12: a key the maker removed stays removed however many removals follow.
static void probe_h3_removals(void) {
    Net net = {0};
    Game a, b, h;
    game_init(&a, 1, 0x3C01), game_init(&b, 2, 0x3C02);
    defaults(&a, 1), defaults(&b, 0);
    net.games[0] = &a, net.games[1] = &b, net.present[0] = net.present[1] = 1;
    uint64_t now = run(&net, (net_roster(&net, "H3F12", 1000), 1000), 3000);
    int done = 0;
    for (int k = 0; k < 17; k++) {
        game_init(&h, 3, 0x3C10 + (uint64_t)k);
        defaults(&h, 0);
        net.games[2] = &h, net.present[2] = 1;
        now = run(&net, (net_roster(&net, "H3F12", now), now), 1500);
        done += coop_sync_remove(a.eng, 3);
        now = run(&net, now, 1500);
        net.present[2] = 0;
        now = run(&net, (net_roster(&net, "H3F12", now), now), 500);
        game_free(&h);
    }
    game_init(&h, 3, 0x3C10);   // the first one again: the same key, a new session
    h.session ^= 0x77;
    defaults(&h, 0);
    net.present[2] = 1;
    now = run(&net, (net_roster(&net, "H3F12", now), now), 2000);
    CoopOffer o;
    coop_sync_offer(b.eng, &o, now);
    const CoopOfferMember *x = offer_member(&o, 3);
    CHECK(done == 17 && x && x->heard && x->removed, "F12: a key removed 17 removals ago is still removed (%d removals, heard %d, removed %d)", done,
          x ? x->heard : -1, x ? x->removed : -1);
    game_free(&a), game_free(&b), game_free(&h);
    free(net.q);
    printf("coop-probe: sync removals: a removed key stays removed after any number of later removals\n");
}

// ---- 9. the fourth review's findings

#ifdef COOP_BEFORE   // the engine before them has neither; its run gets them from a shim over its own internals
int coop_sync_own_record(const CoopSync *s, uint32_t hash, int32_t *base, uint32_t *gain, uint32_t *spend);
int coop_sync_accepted_key(const CoopSync *s, uint64_t kid);
#endif

// The seed of the key game g signs with in room code (coop_state.c: BLAKE2b keyed with its start's random value over
// the room's secret), a key's id (the first 8 bytes of BLAKE2b-256 of the public key) and its fingerprint.
static void game_key_seed(const Game *g, const char *code, uint8_t out[32]) {
    uint8_t seed[32] = {0};
    memcpy(seed, &g->client, sizeof g->client);
    coop_blake2b(out, 32, seed, sizeof seed, code, strlen(code));
}

static uint64_t pk_kid(const uint8_t pk[COOP_PK_LEN]) {
    uint8_t h[32];
    coop_blake2b(h, sizeof h, NULL, 0, pk, COOP_PK_LEN);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)h[i] << 8 * i;
    return v;
}

static uint64_t game_kid(const Game *g, const char *code) {
    uint8_t k[32], sk[COOP_SK_LEN], pk[COOP_PK_LEN];
    game_key_seed(g, code, k);
    coop_sign_keypair(k, sk, pk);
    return pk_kid(pk);
}

// A DIGEST of the identity of key (sk, pk) for member id in session (prev: the session before it, from the fifth review
// on), schema 0xC0FFEE, signed right, with a charter that key signs when modes is set (epoch, pools' epoch, the admitted
// and removed key ids), then zero sums.
static size_t digest_signed(uint8_t *out, const char *code, const uint8_t sk[COOP_SK_LEN], const uint8_t pk[COOP_PK_LEN], int id, uint64_t session,
                            uint64_t prev, const char *name, const uint8_t *modes, uint32_t epoch, uint32_t pool_epoch, const uint64_t *admit,
                            int nadmit, const uint64_t *gone, int ngone) {
    uint8_t ctx[16], m[700], *at = m, *p = out;
    coop_blake2b(ctx, sizeof ctx, code, strlen(code), "nx2navi-coop/3 sign", 19);
    *p++ = COOP_K_DIGEST_VER;
    p = put_le(p, 0xC0FFEE, 4);
    p = put_identity(p, code, sk, pk, id, session, prev, name, strlen(name), 0);
    *p++ = modes != NULL;
    if (modes) {
        uint8_t *fields = p;
        p = put_le(p, epoch, 4);
        *p++ = COOP_CAT_N;
        memcpy(p, modes, COOP_CAT_N), p += COOP_CAT_N;
        p = put_le(p, pool_epoch, 4);
        *p++ = (uint8_t)nadmit;
        for (int i = 0; i < nadmit; i++) p = put_le(p, admit[i], 8);
        *p++ = (uint8_t)ngone;
        for (int i = 0; i < ngone; i++) p = put_le(p, gone[i], 8);
        memcpy(p, pk, COOP_PK_LEN), p += COOP_PK_LEN;
        at = m;
        memcpy(at, "nx2navi-coop/3 rules", 20), at += 20;
        memcpy(at, ctx, sizeof ctx), at += sizeof ctx;
        memcpy(at, fields, (size_t)(p - fields)), at += p - fields;
        coop_sign(p, sk, m, (size_t)(at - m)), p += COOP_SIG_LEN;
    }
    *p++ = COOP_SYNC_SECTIONS;
    memset(p, 0, 4 * COOP_SYNC_SECTIONS), p += 4 * COOP_SYNC_SECTIONS;
    return (size_t)(p - out);
}

// B's digest as B's engine sends it now (the engine ticks once at a later time to send one).
static Packet digest_of(Game *g, uint64_t at) {
    Net cap = {0};
    cap.sending = g->id;
    coop_sync_tick(g->eng, at, net_send, &cap);
    Packet d = {0};
    for (int i = 0; i < cap.n; i++)
        if (cap.q[i].kind == COOP_K_DIGEST) d = cap.q[i];
    free(cap.q);
    return d;
}

// Finding 1: a record with the lowest base (-2^20), signed before any honest game joined the pool (a room made at the
// title screen), puts no pool in a debt: the maker's removal of its signer gives the keys back, and so does a pool
// reset after it left.
static void probe_h4_negative_base(void) {
    for (int variant = 0; variant < 2; variant++) {
        char code[8];
        snprintf(code, sizeof code, "H4F1%c", 'A' + variant);
        Net net = {0};
        Game a, b, m;
        game_init(&a, 1, 0x4101 + 16 * (uint64_t)variant), game_init(&b, 2, 0x4102 + 16 * (uint64_t)variant), game_init(&m, 3, 0x4103 + 16 * (uint64_t)variant);
        defaults(&a, 1), defaults(&b, 0), defaults(&m, 0);
        flag_of(&a, KEYS_FOREST)->value = 2;
        flag_of(&b, KEYS_FOREST)->value = 1;
        flag_of(&m, KEYS_FOREST)->value = -2000000;   // M's claim: the lowest base a record may carry
        a.hold_pools = b.hold_pools = 1;               // A and B are at the title screen: no save, no pools
        net.games[0] = &a, net.games[1] = &b, net.games[2] = &m;
        net.present[0] = net.present[1] = net.present[2] = 1;
        uint64_t now = run(&net, (net_roster(&net, code, 1000), 1000), 6000);
        a.hold_pools = b.hold_pools = 0;               // they load their saves
        now = run(&net, now, 6000);
        int va = value_of(&a, KEYS_FOREST), vb = value_of(&b, KEYS_FOREST), acted;
        if (!variant) {
            acted = coop_sync_remove(a.eng, 3);
            net.present[2] = 0;
            now = run(&net, (net_roster(&net, code, now), now), 6000);
        } else {
            net.present[2] = 0;
            now = run(&net, (net_roster(&net, code, now), now), 3000);
            acted = coop_sync_reset_pools(a.eng);
            now = run(&net, now, 8000);
        }
        int64_t pa, pb;
        coop_sync_pool(a.eng, KEYS_FOREST, &pa), coop_sync_pool(b.eng, KEYS_FOREST, &pb);
        CHECK(acted && value_of(&a, KEYS_FOREST) == 2 && value_of(&b, KEYS_FOREST) == 2,
              "F1: after M's record with base -2^20, %s gives the keys back (A %d, B %d; after the record %d, %d; pools %lld, %lld)",
              variant ? "a pool reset after M left" : "the maker's removal of M", value_of(&a, KEYS_FOREST), value_of(&b, KEYS_FOREST), va, vb,
              (long long)pa, (long long)pb);
        game_free(&a), game_free(&b), game_free(&m);
        free(net.q);
    }
    printf("coop-probe: sync negative base: a record with the lowest base empties no pool; removal and reset give the keys back\n");
}

// Finding 2: a pool counts only the records of keys this player accepted (and its own): B, whose player accepted the
// room before M came, keeps its keys through M's drain and after M left, though the maker admitted M.
static void probe_h4_own_acceptance(void) {
    Net net = {0};
    Game a, b, m;
    game_init(&a, 1, 0x4201), game_init(&b, 2, 0x4202), game_init(&m, 3, 0x4203);
    defaults(&a, 1), defaults(&b, 0), defaults(&m, 0);
    flag_of(&a, KEYS_FOREST)->value = 2;
    b.ask = 1;
    net.games[0] = &a, net.games[1] = &b, net.games[2] = &m, net.present[0] = net.present[1] = 1;
    uint64_t now = run(&net, (net_roster(&net, "H4F2", 1000), 1000), 3000);
    CoopOffer o;
    coop_sync_offer(b.eng, &o, now);
    int took = coop_sync_accept(b.eng, o.token);   // B's player accepts the room of A
    now = run(&net, now, 4000);
    int before = value_of(&b, KEYS_FOREST);
    net.present[2] = 1;                            // M comes; A's player accepts it, so the maker admits it
    now = run(&net, (net_roster(&net, "H4F2", now), now), 5000);
    flag_of(&m, KEYS_FOREST)->value -= 2000000;    // and M drains
    now = run(&net, now, 3000);
    int drained = value_of(&a, KEYS_FOREST);
    net.present[2] = 0;                            // M leaves: B's room is the one its player accepted again
    now = run(&net, (net_roster(&net, "H4F2", now), now), 5000);
    int64_t vb;
    coop_sync_pool(b.eng, KEYS_FOREST, &vb);
    coop_sync_offer(b.eng, &o, now);
    CHECK(took && before == 2 && o.accepted && value_of(&b, KEYS_FOREST) == 2 && vb == 2,
          "F2: B, whose player never accepted M, keeps its keys after the drain of M, whom the maker admitted (shows %d, pool %lld; before %d, "
          "A's after the drain %d, accepted %d)", value_of(&b, KEYS_FOREST), (long long)vb, before, drained, o.accepted);
    game_free(&a), game_free(&b), game_free(&m);
    free(net.q);
    printf("coop-probe: sync own acceptance: a pool counts the records of the keys its player accepted, not the maker's choice alone\n");
}

// Finding 7: a second pool reset before the games joined the pools again keeps what the first one carried.
static void probe_h4_second_reset(void) {
    Net net = {0};
    Game a, b, m;
    game_init(&a, 1, 0x4301), game_init(&b, 2, 0x4302), game_init(&m, 3, 0x4303);
    defaults(&a, 1), defaults(&b, 0), defaults(&m, 0);
    flag_of(&a, KEYS_FOREST)->value = 2;
    net.games[0] = &a, net.games[1] = &b, net.games[2] = &m, net.present[0] = net.present[1] = net.present[2] = 1;
    uint64_t now = run(&net, (net_roster(&net, "H4F7", 1000), 1000), 5000);
    flag_of(&m, KEYS_FOREST)->value -= 2000000;
    now = run(&net, now, 3000);
    net.present[2] = 0;
    now = run(&net, (net_roster(&net, "H4F7", now), now), 3000);
    int drained = value_of(&b, KEYS_FOREST);
    a.hold_pools = b.hold_pools = 1;   // both in a loading screen: their pools wait
    int r1 = coop_sync_reset_pools(a.eng);
    now = run(&net, now, 1500);
    int r2 = coop_sync_reset_pools(a.eng);
    now = run(&net, now, 1500);
    a.hold_pools = b.hold_pools = 0;
    now = run(&net, now, 8000);
    CHECK(r1 && r2 && value_of(&a, KEYS_FOREST) == 2 && value_of(&b, KEYS_FOREST) == 2,
          "F7: after two pool resets before the games joined again the keys come back (A %d, B %d; drained %d)", value_of(&a, KEYS_FOREST),
          value_of(&b, KEYS_FOREST), drained);
    game_free(&a), game_free(&b), game_free(&m);
    free(net.q);
    printf("coop-probe: sync second reset: a reset before the games joined again keeps what the first one carried\n");
}

// Finding 8: the ended sessions of a key the player accepted outlast any number of other members' sessions.
static void probe_h4_ended_per_key(void) {
    Net net = {0};
    Game a, b, c;
    game_init(&a, 1, 0x4801), game_init(&b, 2, 0x4802), game_init(&c, 3, 0x4803);
    defaults(&a, 1), defaults(&b, 0), defaults(&c, 0);
    net.games[0] = &a, net.games[1] = &b, net.games[2] = &c, net.present[0] = net.present[1] = net.present[2] = 1;
    uint64_t now = run(&net, (net_roster(&net, "H4F8", 1000), 1000), 3000);   // C's player accepts A and B
    Packet dig = digest_of(&b, now + 3000);
    uint64_t bsess = b.session;
    b.session ^= 0x77;    // B joins again: its identity in the new session ends the old one at C
    now = run(&net, (net_roster(&net, "H4F8", now), now), 4000);
    net.present[1] = 0;   // and leaves
    now = run(&net, (net_roster(&net, "H4F8", now), now), 1000);
    // 65600 sessions of a key C never accepted come and go as member 5
    uint8_t ks[32] = {0x48, 0x55}, sk[COOP_SK_LEN], pk[COOP_PK_LEN], d[COOP_MAX_PAYLOAD];
    coop_sign_keypair(ks, sk, pk);
    int ids[3] = {1, 3, 5};
    uint64_t sess[3] = {a.session, c.session, 0};
    for (uint32_t k = 0; k < 65600; k++) {
        sess[2] = 0x900000 + (uint64_t)k;
        coop_sync_room(c.eng, "H4F8", 3, ids, sess, 3, 1, now += 10);
        size_t dn = digest_signed(d, "H4F8", sk, pk, 5, sess[2], k ? sess[2] - 1 : 0, "X", NULL, 0, 0, NULL, 0, NULL, 0);
        coop_sync_receive(c.eng, 5, COOP_K_DIGEST, d, dn, now);
    }
    coop_sync_room(c.eng, "H4F8", 3, ids, sess, 2, 1, now += 50);
    // M: member 2 in B's old session, with B's identity
    int ids2[3] = {1, 2, 3};
    uint64_t sess2[3] = {a.session, bsess, c.session};
    coop_sync_room(c.eng, "H4F8", 3, ids2, sess2, 3, 1, now += 50);
    coop_sync_receive(c.eng, 2, COOP_K_DIGEST, dig.b, dig.n, now += 10);
    CoopOffer o;
    coop_sync_offer(c.eng, &o, now);
    const CoopOfferMember *x = offer_member(&o, 2), *y = offer_member(&o, 5);
    (void)y;
    CHECK(dig.n && !(x && x->heard), "F8: B's identity in B's old session is not taken after 65600 sessions of another key ended (heard %d)",
          x ? x->heard : -1);
    game_free(&a), game_free(&b), game_free(&c);
    free(net.q);
    printf("coop-probe: sync ended per key: the ended sessions of an accepted key outlast any churn of others\n");
}

// Finding 4 (the engine's part): a roster that lists an invented session for a member ends none of its sessions; one
// ends once the member was heard in its next session, or left.
static void probe_h4_forged_roster(void) {
    Net net = {0};
    Game a, b, c;
    game_init(&a, 1, 0x4401), game_init(&b, 2, 0x4402), game_init(&c, 3, 0x4403);
    defaults(&a, 1), defaults(&b, 0), defaults(&c, 0);
    net.games[0] = &a, net.games[1] = &b, net.games[2] = &c, net.present[0] = net.present[1] = net.present[2] = 1;
    uint64_t now = run(&net, (net_roster(&net, "H4F4", 1000), 1000), 3000);
    int ids[3] = {1, 2, 3};
    uint64_t forged[3] = {a.session, b.session ^ 0xF00D, c.session}, real[3] = {a.session, b.session, c.session};
    coop_sync_room(c.eng, "H4F4", 3, ids, forged, 3, 1, now += 8);   // someone on the path: another session for B
    coop_sync_room(c.eng, "H4F4", 3, ids, real, 3, 1, now += 8);     // the relay's roster again
    now = run(&net, now, 3000);
    CoopOffer o;
    coop_sync_offer(c.eng, &o, now);
    const CoopOfferMember *x = offer_member(&o, 2);
    int heard = x && x->heard;
    CHECK(heard, "F4: after a forged roster listed another session for B, B is taken again in its own (heard %d)", x ? x->heard : -1);
    // B rejoins for real (a new session) and is heard in it; a roster listing its old session again, with its old
    // identity, is a replay
    Packet old = digest_of(&b, now + 3000);
    b.session ^= 0x1234;
    now = run(&net, (net_roster(&net, "H4F4", now), now), 7000);   // B's next digest (in its new session) reaches C
    coop_sync_offer(c.eng, &o, now);
    x = offer_member(&o, 2);
    int heard_new = x && x->heard;
    coop_sync_room(c.eng, "H4F4", 3, ids, real, 3, 1, now += 8);
    coop_sync_receive(c.eng, 2, COOP_K_DIGEST, old.b, old.n, now += 8);
    coop_sync_offer(c.eng, &o, now);
    x = offer_member(&o, 2);
    CHECK(old.n && heard_new && !(x && x->heard), "F4: B's old session, ended once B was heard in its new one, is not taken again (heard %d, %d)",
          heard_new, x ? x->heard : -1);
    game_free(&a), game_free(&b), game_free(&c);
    free(net.q);
    printf("coop-probe: sync forged roster: a roster ends no session by itself; a session ends when its member was heard in the next or left\n");
}

// Finding 6: what a contributor spent beyond what it brought counts per pool, so gains it signs for one pool hide no
// drain of another.
static void probe_h4_spent(void) {
    Net net = {0};
    Game a, b, m;
    game_init(&a, 1, 0x4601), game_init(&b, 2, 0x4602), game_init(&m, 3, 0x4603);
    defaults(&a, 1), defaults(&b, 0), defaults(&m, 0);
    Game *all[3] = {&a, &b, &m};
    for (int i = 0; i < 3; i++) flag_add(all[i], COOP_CAT_KEYS, KEYS_WATER, 0, -1);
    flag_of(&a, KEYS_FOREST)->value = 2;
    net.games[0] = &a, net.games[1] = &b, net.games[2] = &m, net.present[0] = net.present[1] = net.present[2] = 1;
    uint64_t now = run(&net, (net_roster(&net, "H4F6", 1000), 1000), 5000);
    flag_of(&m, KEYS_FOREST)->value -= 2000000;   // M drains the forest's pool
    uint8_t ks[32], buf[COOP_MAX_PAYLOAD], *p = buf + 6;
    game_key_seed(&m, "H4F6", ks);                // and signs base -2^20, gain 2^20 for the water's, which nobody visited
    p = entry_record(p, "H4F6", COOP_CAT_KEYS, KEYS_WATER, ks, 0, 100000, -(1 << 20), 1u << 20, 0);
    inject(&a, &b, 3, buf, p, 1, now);
    now = run(&net, now, 3000);
    CoopContributor who[8];
    int n = coop_sync_contributors(a.eng, who, 8);
    long long spent = -1;
    for (int i = 0; i < n; i++)
        if (who[i].member == 3) spent = (long long)who[i].spent[0];
    CHECK(spent >= 1 && value_of(&a, KEYS_FOREST) == 0, "F6: the drainer's keys taken beyond what it brought show (%lld), though it signed 2^20 gains in another pool",
          spent);
    game_free(&a), game_free(&b), game_free(&m);
    free(net.q);
    printf("coop-probe: sync spent: what a contributor took beyond what it brought counts per pool\n");
}

// ---- 10. hostile members and an attacker who hold the secret, at random
//
// One seed is one room: three honest games, two hostile members who hold the secret and are admitted by the maker when
// its player accepts everything (else nobody's accepts them), and an attacker who holds the secret but no key anyone
// accepted: whoever runs the relay knowing the secret, or a sender on a player's network that can send from the
// relay's address. The hostile members send every signed record and flag the wire format allows, with extreme and
// random values, charters signed with their own keys, and their own games' counters at the extremes. The attacker
// sends sections in another member's name; rosters that leave a member out, list another session or an earlier one
// for it, or list a member nobody is; what the honest games sent before (their identities and sections, of their
// session now or an earlier one) after a roster that lists the session it was sent in; and, from the sixth review on,
// copies of an honest game's digest with the maker's charter far later or one mode changed, or with one byte of its
// name changed after two rosters that make the victim meet it again, or of its section with a byte changed, all with
// the genuine signature; and identities of its own key for an honest game's session (often right after that game joined
// again). All of it comes in a random order with the honest players' joins, rejoins, leaving and coming back, saves
// loaded (often, in one seed in five), keys picked up and used, and the maker's removals and resets (a reset after the
// maker removed the hostile keys it admitted, as docs/coop.md says, one time in three in the same charter). At the end
// the hostile members leave and the maker removes every key they used, or resets the pools (once or twice).
//
// The harness keeps a ledger of the pools of its own: what each honest game joined each pool with (its count), the
// carry of a reset (which the ledger works out itself, over the players the reset admits) as a claim made at the reset,
// the honest records it had in view then, and the keys each honest player picked up and used since. From that it works
// out by the rule docs/coop.md gives (the highest claim, every pickup, every use) what an honest pool holds with no
// hostile record in it and no claim as a game wrote it, so a claim that absorbed a removed key's gains, or a carry that
// kept a removed key's drain or absorbed a later pickup, shows as a pool below it. Invariants, printed with the seed
// when broken:
// - after every event: no honest game stores or shows a flag only the hostile members or the attacker sent unless it
//   accepted a hostile key or the attacker's or (transitively, as documented) a player who did; nothing was applied
//   while a game's room was not open to it; nothing lowered a flag;
// - every tenth event, after a quiet 1.5 s and with the hostile members still there: a game whose pools count no
//   hostile key, directly or through a player it accepted, holds at least what the ledger says;
// - at the end: every honest game identifies the others by their own keys, ignores none of them, holds the maker's
//   charter and is open, and its pools are bound, hold at least what the ledger says, agree between the honest games
//   and show what they hold.

enum { ADV_H = 3, ADV_M = 2, ADV_STARTS = 4, ADV_FLAG_LO = 251, ADV_FLAG_HI = 290, ADV_X = 7, ADV_POOLS = 3, ADV_LOG = 48 };
static const uint32_t adv_pool_hash[ADV_POOLS] = {KEYS_FOREST, KEYS_WATER, RUPEE};

#if !H5_SIGNED   // the engine before the fifth review has neither; its run gets them from a shim over its own internals
int coop_sync_record(const CoopSync *s, uint32_t hash, uint64_t kid, int32_t *base, uint32_t *gain, uint32_t *spend);
uint32_t coop_sync_pool_epoch(const CoopSync *s);
#endif

typedef struct {   // an honest game's join of a pool, as the harness saw it
    int fresh;                     // its first in the pools' epoch
    int64_t w;                     // what it joined with: its count (a reset's carry is the epoch's floor instead)
    int has[ADV_H];                // the honest records in its view just before (what their players picked up and used)
    int32_t vb[ADV_H];
    uint32_t vg[ADV_H], vs[ADV_H];
} AdvJoin;

typedef struct {   // an honest game's joins, pickups and uses in one pools' epoch
    uint32_t ep;
    int valid;
    AdvJoin j[8];                  // its joins (as coop_state.c keeps POOL_JOINS of them)
    int nj;
    int64_t floor;                 // the claims of joins no longer kept, at the most they came to, and a reset's carry
    int carried;                   // the reset's carry was signed at once as the game's record: a claim made at the reset
    uint32_t gain, spend;          // its player's pickups and uses since its first join in the epoch, counted here
    int own;                       // its game's own record of the epoch as the game last signed it (ob, og, os): final
    int32_t ob;                    // once the game moved on to a later epoch
    uint32_t og, os;
} AdvEpoch;

typedef struct { int in; int64_t b, g, s; } AdvRow;   // an honest game's claim, gains and spends in one game's view

typedef struct {
    AdvEpoch cur, past[4];         // the pools' epoch its game is in and the ones before, latest first: a game a reset
                                   // has not reached yet counts what the others did in its epoch, also after they moved on
    int32_t last;                  // its count at its last report (after what the pool handed it)
    AdvJoin pre;                   // the view before a report
    int has_carry, snap_ok;        // a reset's carry until the game joins; snap_ok: the game had the room's rules
    int64_t carry;
    AdvRow snap[ADV_H];            // the honest records it held just before a message that may bring a reset
} AdvLed;

typedef struct Adv {
    uint64_t seed, r, now;
    char code[24];
    Net net;
    Game h[ADV_H], m[ADV_M], x;    // x: the attacker's engine, with a key nobody accepts; never in the room
    int m_live[ADV_M], m_starts[ADV_M];
    uint64_t mkid[ADV_M * ADV_STARTS];   // every key a hostile member signed with
    int mgone[ADV_M * ADV_STARTS];       // the maker removed it
    int nmkid;
    uint64_t hkid[ADV_H], xkid;    // the honest games' keys, and the attacker's
    int reloads;                   // the honest players load another save often (the joins a pool remembers fill up)
    int rupees;                    // the maker pools rupees too
    uint32_t pool_epoch;           // the resets so far
    AdvLed led[ADV_H][ADV_POOLS];
    int fails;
} Adv;

static unsigned long long adv_forged, adv_rosters, adv_replays, adv_checks, adv_tampered, adv_impostors, adv_together;   // for the summary

#define ADV_FAIL(a, ...)                                                                                                \
    do {                                                                                                               \
        if (!(a)->fails++) {                                                                                           \
            fprintf(stderr, "FAIL coop-sync: adversary seed %llu: ", (unsigned long long)(a)->seed);                   \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

static uint64_t adv_rnd(Adv *a) {
    a->r ^= a->r >> 12, a->r ^= a->r << 25, a->r ^= a->r >> 27;
    return a->r * 0x2545F4914F6CDD1Dull;
}
static unsigned adv_below(Adv *a, unsigned n) { return (unsigned)(adv_rnd(a) >> 33) % n; }
static uint64_t mix_seed(uint64_t x) {   // a seed's random stream (splitmix64), never 0
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x ? x : 1;
}

// ---- the ledger

static int adv_game(const Adv *a, const Game *g) { return g >= a->h && g < a->h + ADV_H ? (int)(g - a->h) : -1; }
static int adv_pool(uint32_t hash) {
    for (int p = 0; p < ADV_POOLS; p++)
        if (adv_pool_hash[p] == hash) return p;
    return -1;
}
static int64_t adv_lim(int p) { return adv_pool_hash[p] == RUPEE ? 999 : 15; }
static int adv_counts(const Adv *a, int g, int k) { return k == g || coop_sync_accepted_key(a->h[g].eng, a->hkid[k]); }
static int64_t adv_clamp(int64_t v) { return v < -(1 << 20) ? -(1 << 20) : v > 1 << 20 ? 1 << 20 : v; }

// coop_state.h's rule over n records: the highest base (at least 0) + every gain - each spend up to its gains plus the
// counter's limit and up to what entered the pool; and that highest base.
typedef struct { int64_t value, top; } AdvSum;
static AdvSum adv_sum(const int64_t *base, const int64_t *gain, const int64_t *spend, int n, int64_t lim) {
    AdvSum o = {0, 0};
    int64_t gains = 0, spends = 0;
    for (int i = 0; i < n; i++) o.top = base[i] > o.top ? base[i] : o.top, gains += gain[i];
    for (int i = 0; i < n; i++) {
        int64_t bound = gain[i] + lim < o.top + gains ? gain[i] + lim : o.top + gains;
        spends += spend[i] < bound ? spend[i] : bound;
    }
    o.value = n ? o.top + gains - spends : 0;
    return o;
}

// The claim of one join of honest game h in pool p over the records in its view then that h counts now (a game makes its
// claims again when what it counts changes), and what it adds as a gain instead (a first join that found the pool in
// debt). keep: the honest games whose records count (a bit each; a reset's carry counts only those it admits).
static void adv_join_claim(const Adv *a, int h, int p, const AdvJoin *j, unsigned keep, int64_t *claim, int64_t *debt) {
    int64_t b[ADV_H], g[ADV_H], s[ADV_H];
    int n = 0;
    for (int k = 0; k < ADV_H; k++)
        if (j->has[k] && adv_counts(a, h, k) && keep >> k & 1) b[n] = j->vb[k], g[n] = j->vg[k], s[n] = j->vs[k], n++;
    AdvSum v = adv_sum(b, g, s, n, adv_lim(p));
    *claim = -(1 << 20), *debt = 0;
    if (v.value >= 0 || !n) *claim = adv_clamp(j->w - (n ? v.value - v.top : 0));
    else if (j->fresh && j->w > 0) *debt = j->w < 1 << 20 ? j->w : 1 << 20;
}

// Honest game h's claim in pool p in epoch e (the highest of its joins' and its carry), and its first join's gain in
// debt. 0 when it did not join and signed no carry.
static int adv_claim(const Adv *a, int h, int p, const AdvEpoch *e, unsigned keep, int64_t *claim, int64_t *debt) {
    *claim = e->floor, *debt = 0;
    for (int i = 0; i < e->nj; i++) {
        int64_t c, d;
        adv_join_claim(a, h, p, &e->j[i], keep, &c, &d);
        if (c > *claim) *claim = c;
        *debt += d;
    }
    return e->nj != 0 || e->carried;
}

// What pool p holds in honest game g's view by the ledger: the claims, pickups and uses in g's epoch of the honest games
// it counts, one row each. held: as g holds their records. One that g holds as its owner last signed it in that epoch
// counts by the ledger's own claim, pickups and uses; an older one of the owner's (the newer is on its way, or is not
// taken while the owner's identity is disputed) counts as g holds it, as signed then; none at all, not at all. At the
// end every record has arrived. An owner that moved on to a later epoch signs nothing more in this one, so its last
// record there is final. The rows that count.
static int adv_rows(const Adv *a, int g, int p, int held, AdvRow rows[ADV_H]) {
    int64_t c, debt;
    int n = 0;
    uint32_t ep = a->led[g][p].cur.ep;
    for (int h = 0; h < ADV_H; h++) {
        const AdvLed *L = &a->led[h][p];
        const AdvEpoch *e = L->cur.ep == ep ? &L->cur : NULL;
        rows[h].in = 0;
        for (int i = 0; i < 4 && !e; i++) e = L->past[i].valid && L->past[i].ep == ep ? &L->past[i] : NULL;
        if (!e || !adv_counts(a, g, h) || !adv_claim(a, h, p, e, 7, &c, &debt)) continue;
        int32_t rb;
        uint32_t rg, rs;
        if (held) {
            if (!coop_sync_record(a->h[g].eng, adv_pool_hash[p], a->hkid[h], &rb, &rg, &rs)) continue;
            if (!(e->own && rb == e->ob && rg == e->og && rs == e->os)) {
                rows[h] = (AdvRow){1, rb, rg, rs}, n++;
                continue;
            }
        }
        rows[h] = (AdvRow){1, c, (int64_t)e->gain + debt, e->spend}, n++;
    }
    return n;
}

// The pool's value over the rows that count, of the games in mask (a bit each); 0 when none counts.
static int adv_rows_sum(const AdvRow rows[ADV_H], unsigned mask, int p, int64_t *out) {
    int64_t b[ADV_H], g[ADV_H], s[ADV_H];
    int n = 0;
    for (int h = 0; h < ADV_H; h++)
        if (rows[h].in && mask >> h & 1) b[n] = rows[h].b, g[n] = rows[h].g, s[n] = rows[h].s, n++;
    if (n) *out = adv_sum(b, g, s, n, adv_lim(p)).value;
    return n != 0;
}

static int adv_ledger(const Adv *a, int g, int p, int held, int64_t *out) {
    AdvRow rows[ADV_H];
    return adv_rows(a, g, p, held, rows) && adv_rows_sum(rows, 7, p, out);
}

// The honest games whose keys game k's charter admits to the pools (a bit each).
static unsigned adv_admitted(const Adv *a, int k) {
    CoopContributor c[32];
    unsigned mask = 0;
    int n = coop_sync_contributors(a->h[k].eng, c, 32);
    for (int i = 0; i < n; i++) {
        uint64_t kid = 0;
        for (int h = 0; h < ADV_H; h++)
            if (!c[i].removed && coop_sync_key_parse(c[i].key, &kid) && kid == a->hkid[h]) mask |= 1u << h;
    }
    return mask;
}

// Game k's pools' epoch as its engine has it: a reset that reached it starts a new record, with the carry the ledger
// works out from the rows it held just before the message that brought it (when the game had the room's rules then):
// those of the honest games the new charter admits, its own claim made again counting only them, as coop_state.c makes
// it. One that waits keeps the larger. And the game's own record of its epoch as its engine holds it now: the engine
// signs it again in a report, and a claim made again at its next tick, also while the game reports nothing. The ledger
// looks before every message that may bring a reset, so a record that a reset ends is kept as the game last signed it.
static void adv_led_epoch(Adv *a, int k, int p) {
    AdvLed *L = &a->led[k][p];
    uint32_t ep = coop_sync_pool_epoch(a->h[k].eng);
    if (ep != L->cur.ep) {
        memmove(&L->past[1], &L->past[0], 3 * sizeof L->past[0]);
        L->past[0] = L->cur;
        memset(&L->cur, 0, sizeof L->cur);
        L->cur.ep = ep, L->cur.valid = 1, L->cur.floor = -(1 << 20);
        if (L->snap_ok) {
            unsigned mask = adv_admitted(a, k);
            int64_t c, debt, snap = 0;
            if (L->snap[k].in && adv_claim(a, k, p, &L->past[0], mask, &c, &debt)) L->snap[k].b = c, L->snap[k].g = (int64_t)L->past[0].gain + debt;
            adv_rows_sum(L->snap, mask, p, &snap);
            if (!L->has_carry || snap > L->carry) L->carry = snap;
            L->has_carry = 1;
        }
        // the rule docs/coop.md gives: a reset's carry is a claim made at the reset, and whatever anyone picks up after
        // it comes on top. A game the reset admits signs it at once (its engine holds its own record of the new epoch
        // before any join); one it does not admit claims it at its first join
        int32_t ob;
        uint32_t og, os;
        if (L->has_carry && coop_sync_own_record(a->h[k].eng, adv_pool_hash[p], &ob, &og, &os)) {
            L->cur.carried = 1, L->has_carry = 0;
            if (L->carry > L->cur.floor) L->cur.floor = adv_clamp(L->carry);
        }
    }
    L->cur.own = coop_sync_own_record(a->h[k].eng, adv_pool_hash[p], &L->cur.ob, &L->cur.og, &L->cur.os);
}

// game_frame's report of pool counter f of honest game g, before (after 0) and after the engine took it: was its pool
// bound before. A change of the game's own count while bound is a pickup or a use; a pool that binds is a join.
static void adv_led_report(Adv *a, Game *g, const Flag *f, int was, int after) {
    int k = adv_game(a, g), p = adv_pool(f->hash);
    if (k < 0 || p < 0) return;
    AdvLed *L = &a->led[k][p];
    AdvEpoch *e = &L->cur;
    adv_led_epoch(a, k, p);
    if (!after) {
        if (!was)
            for (int h = 0; h < ADV_H; h++) L->pre.has[h] = coop_sync_record(g->eng, f->hash, a->hkid[h], &L->pre.vb[h], &L->pre.vg[h], &L->pre.vs[h]);
        return;
    }
    int bound = coop_sync_pool(g->eng, f->hash, NULL);
    if (was && bound) {
        int64_t d = (int64_t)f->value - L->last;
        if (d > 0) e->gain += (uint32_t)d;
        else e->spend += (uint32_t)-d;
    } else if (!was && bound) {   // a join: with its count; every one is kept
        if (e->nj == 8) {
            int64_t c, d;
            adv_join_claim(a, k, p, &e->j[1], 7, &c, &d);
            if (c > e->floor) e->floor = c;
            memmove(&e->j[1], &e->j[2], 6 * sizeof e->j[0]);
            e->nj--;
        }
        AdvJoin *j = &e->j[e->nj];
        *j = L->pre;
        j->fresh = !e->nj && !e->carried;
        e->nj++;
        j->w = f->value;
        if (j->fresh && L->has_carry && L->carry > 0 && L->carry > e->floor) e->floor = adv_clamp(L->carry);   // a carry not signed at the reset
        L->has_carry = 0;
    }
    L->last = f->value;
    e->own = coop_sync_own_record(g->eng, f->hash, &e->ob, &e->og, &e->os);
}

static void adv_led_applied(Adv *a, Game *g, const Flag *f) {
    int k = adv_game(a, g), p = adv_pool(f->hash);
    if (k >= 0 && p >= 0) a->led[k][p].last = f->value;
}

// Before a message that may bring a pool reset reaches honest game g (a digest carries the room's charter), and before
// the maker's own reset: what g's pools hold by the ledger over the records g holds, which is g's carry if the reset
// takes effect there now. A game carries what it holds when the reset reaches it, not when the maker made it: a key it
// used in between is gone from its carry.
static void adv_led_snap(Adv *a, const Game *g) {
    int k = adv_game(a, g);
    if (k < 0) return;
    CoopRules r;
    coop_sync_rules(g->eng, &r);
    for (int p = 0; p < ADV_POOLS; p++) {
        for (int h = 0; h < ADV_H; h++) adv_led_epoch(a, h, p);
        AdvLed *L = &a->led[k][p];
        L->snap_ok = r.epoch != 0;
        if (L->snap_ok) adv_rows(a, k, p, 1, L->snap);
    }
}

// The maker resets the pools: for its own game the reset takes effect at once.
static void adv_reset_pools(Adv *a) {
    adv_led_snap(a, &a->h[0]);
    a->pool_epoch += (uint32_t)coop_sync_reset_pools(a->h[0].eng);
}

// ---- the room

static void adv_save(Adv *a, Game *g) {   // the counters of a save
    flag_of(g, KEYS_FOREST)->value = (int32_t)adv_below(a, 4);
    flag_of(g, KEYS_WATER)->value = (int32_t)adv_below(a, 4);
    flag_of(g, RUPEE)->value = (int32_t)adv_below(a, 201);
}

static void adv_roster(Adv *a) { net_roster(&a->net, a->code, a->now); }

// The room as the relay lists it now: ids, sessions, the maker.
static int adv_list(Adv *a, int *ids, uint64_t *sess, int *maker) {
    int n = 0;
    *maker = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (a->net.games[i] && a->net.present[i]) {
            ids[n] = a->net.games[i]->id, sess[n++] = a->net.games[i]->session;
            if (a->net.games[i]->made) *maker = a->net.games[i]->id;
        }
    return n;
}

static int adv_honest_kid(const Adv *a, uint64_t kid) {
    for (int k = 0; k < ADV_H; k++)
        if (a->hkid[k] == kid) return 1;
    return 0;
}

// A player who checks who is in the room: accepts when every member is identified by the key of a player it knows (a
// removed one aside), so neither a hostile member's key nor the attacker's in an honest member's place.
static void adv_ask(Adv *a, Game *g) {
    CoopOffer o;
    if (!g->ask || !coop_sync_offer(g->eng, &o, a->now) || o.accepted || o.unidentified) return;
    for (int i = 0; i < o.n; i++) {
        uint64_t kid = 0;
        if (o.member[i].removed) continue;
        if (!o.member[i].heard || !coop_sync_key_parse(o.member[i].key, &kid) || !adv_honest_kid(a, kid)) return;
    }
    coop_sync_accept(g->eng, o.token);
}

static void adv_ask_all(Adv *a) {
    for (int k = 0; k < ADV_H; k++)
        if (a->net.present[k]) adv_ask(a, &a->h[k]);
}

static void adv_remove(Adv *a, int j) {   // the maker removes hostile key j
    if (!a->mgone[j] && coop_sync_remove_key(a->h[0].eng, a->mkid[j])) a->mgone[j] = 1;
}

// A section of the entries in [buf + 6, end) from member from (signed by its game) to every honest game in the room.
static void adv_inject(Adv *a, int from, uint8_t *buf, uint8_t *end, int count) {
    buf[0] = COOP_K_DIGEST_VER, buf[1] = 0xEE, buf[2] = 0xFF, buf[3] = 0xC0, buf[4] = 0, buf[5] = (uint8_t)count;
    size_t n = seal_as(by_id[from], buf, (size_t)(end - buf), COOP_MAX_MESSAGE);
    for (int k = 0; k < ADV_H; k++)
        if (a->net.present[k]) coop_sync_receive(a->h[k].eng, from, COOP_K_SECTION, buf, n, a->now);
}

static int32_t adv_base(Adv *a) {
    static const int32_t b[] = {-(1 << 20), -(1 << 20) + 1, -1, 0, 1, 15, (1 << 20) - 1, 1 << 20, (1 << 20) + 1, INT32_MIN, INT32_MAX};
    return adv_below(a, 3) ? b[adv_below(a, 11)] : (int32_t)(adv_rnd(a) % (2u << 20)) - (1 << 20);
}

static uint32_t adv_amount(Adv *a) {
    static const uint32_t v[] = {0, 1, 2, 15, 16, 999, 1u << 20, (1u << 20) + 1, 0xFFFFFFFFu};
    return adv_below(a, 3) ? v[adv_below(a, 9)] : (uint32_t)adv_rnd(a) % (1u << 21);
}

static uint32_t adv_hash(Adv *a) {
    static const uint32_t hs[] = {KEYS_FOREST, KEYS_WATER, RUPEE, ARROWS, 7, 0x1001};
    return adv_below(a, 8) ? hs[adv_below(a, 6)] : (uint32_t)adv_rnd(a);
}

// Up to eight pool records signed with hostile member i's key, every field at random or at an extreme.
static void adv_records(Adv *a, int i) {
    uint8_t ks[32], buf[COOP_MAX_PAYLOAD], *p = buf + 6;
    game_key_seed(&a->m[i], a->code, ks);
    int n = 1 + (int)adv_below(a, 8);
    for (int k = 0; k < n; k++) {
        uint32_t hash = adv_hash(a);
        int cat = hash == RUPEE ? COOP_CAT_RUPEES : hash == ARROWS ? COOP_CAT_AMMO : COOP_CAT_KEYS;
        if (!adv_below(a, 8)) cat = (int)adv_below(a, COOP_CAT_N);
        uint32_t ep = adv_below(a, 4) ? a->pool_epoch : adv_below(a, 2) ? a->pool_epoch + 1 : (uint32_t)adv_rnd(a);
        uint32_t seq = adv_below(a, 4) ? 1000000 + (uint32_t)(a->now / 8) : adv_below(a, 2) ? 0xFFFFFFFFu : (uint32_t)adv_rnd(a);
        int32_t base = adv_base(a);
        uint32_t gain = adv_amount(a), spend = adv_amount(a);
        p = entry_record(p, a->code, cat, hash, ks, ep, seq, base, gain, spend);
    }
    adv_inject(a, ADV_H + 1 + i, buf, p, n);
}

// Flags of every category at random, most in the hostile members' own range, some in the honest one, some of hashes
// and values no game has.
static void adv_flags(Adv *a, int i) {
    uint8_t buf[COOP_MAX_PAYLOAD], *p = buf + 6;
    int n = 1 + (int)adv_below(a, 60);
    for (int k = 0; k < n; k++) {
        uint32_t hash = adv_below(a, 4) ? ADV_FLAG_LO + adv_below(a, ADV_FLAG_HI - ADV_FLAG_LO + 1) : adv_below(a, 2) ? 1 + adv_below(a, 300) : (uint32_t)adv_rnd(a);
        int cat = adv_below(a, 4) ? COOP_CAT_PROGRESS : (int)adv_below(a, COOP_CAT_N);
        uint32_t v = adv_below(a, 3) ? 1 : adv_amount(a);
        p = entry_flag(p, cat, hash, v);
    }
    adv_inject(a, ADV_H + 1 + i, buf, p, n);
}

// A digest of hostile member i with a charter its own key signs: other rules, its keys admitted, honest keys removed,
// a later epoch, another pools' epoch.
static void adv_charter(Adv *a, int i) {
    Game *m = &a->m[i];
    uint8_t ks[32], sk[COOP_SK_LEN], pk[COOP_PK_LEN], d[COOP_MAX_PAYLOAD], modes[COOP_CAT_N];
    game_key_seed(m, a->code, ks);
    coop_sign_keypair(ks, sk, pk);
    for (int c = 0; c < COOP_CAT_N; c++) modes[c] = (uint8_t)adv_below(a, 4);
    uint64_t admit[4] = {a->mkid[a->nmkid - 1], adv_rnd(a), a->hkid[0], a->hkid[1]}, gone[2] = {a->hkid[adv_below(a, ADV_H)], adv_rnd(a)};
    char name[8];
    snprintf(name, sizeof name, "P%d", m->id);
    size_t n = digest_signed(d, a->code, sk, pk, m->id, m->session, 0, name, modes, (uint32_t)adv_rnd(a),
                             adv_below(a, 2) ? a->pool_epoch + 1 : (uint32_t)adv_rnd(a), admit, 1 + (int)adv_below(a, 4), gone, (int)adv_below(a, 3));
    for (int k = 0; k < ADV_H; k++)
        if (a->net.present[k]) adv_led_snap(a, &a->h[k]), coop_sync_receive(a->h[k].eng, m->id, COOP_K_DIGEST, d, n, a->now);
}

// Hostile member i comes (a new start of its game: a new key, sometimes), or goes.
static void adv_hostile_move(Adv *a, int i) {
    int slot = ADV_H + i;
    if (a->m_live[i] && a->net.present[slot]) {
        a->net.present[slot] = 0;
    } else {
        if (!a->m_live[i] || (a->m_starts[i] < ADV_STARTS && adv_below(a, 2))) {
            if (a->m_live[i]) game_free(&a->m[i]);
            Game *m = &a->m[i];
            game_init(m, ADV_H + 1 + i, 0x5A000000ull + a->seed * 64 + (uint64_t)i * 8 + (uint64_t)a->m_starts[i]);
            defaults(m, 0);
            flag_add(m, COOP_CAT_KEYS, KEYS_WATER, 0, -1);
            for (int k = 0; k < 6; k++) m->f[ADV_FLAG_LO - 1 + adv_below(a, ADV_FLAG_HI - ADV_FLAG_LO + 1)].value = 1;   // its own flags
            a->m_live[i] = 1, a->m_starts[i]++;
            a->mkid[a->nmkid++] = game_kid(m, a->code);
            a->net.games[slot] = m;
        }
        a->net.present[slot] = 1;
    }
    adv_roster(a);
}

static void adv_hostile_counters(Adv *a, int i) {   // its own game's counters at the extremes: its engine signs them
    static const int32_t v[] = {-2000000, 2000000, -1, 0, 15, 16, INT32_MIN / 2, INT32_MAX / 2};
    static const uint32_t hs[] = {KEYS_FOREST, KEYS_WATER, RUPEE, ARROWS};
    flag_of(&a->m[i], hs[adv_below(a, 4)])->value = v[adv_below(a, 8)];
}

static void adv_honest_event(Adv *a, Game *g) {
    switch (a->reloads && adv_below(a, 2) ? 4 : adv_below(a, 8)) {
    case 0:
    case 1: {   // a key picked up or used
        Flag *f = flag_of(g, adv_below(a, 2) ? KEYS_FOREST : KEYS_WATER);
        if (adv_below(a, 2)) f->value++;
        else if (f->value > 0) f->value--;
        break;
    }
    case 2: flag_of(g, RUPEE)->value = (int32_t)adv_below(a, 201); break;
    case 3: g->f[adv_below(a, 200)].value = 1; break;            // a progress flag of the honest range (hashes 1..200)
    case 4: reload_save(g), adv_save(a, g); break;               // another save
    case 5: g->session = adv_rnd(a) | 1, adv_roster(a); break;   // it joins the room again: a new session
    default: g->hold_pools = !g->hold_pools; break;              // the title screen, or back from it
    }
}

// ---- the attacker: the secret, and no key anyone accepted

// A section in another member's name (an honest one's or a hostile one's), signed (from the fifth review on) with the
// attacker's own key: flags of the hostile range, some of the honest one, to every honest game in the room.
static void adv_forge_section(Adv *a) {
    uint8_t buf[COOP_MAX_PAYLOAD], *p = buf + 6;
    int n = 1 + (int)adv_below(a, 20);
    for (int k = 0; k < n; k++)
        p = entry_flag(p, COOP_CAT_PROGRESS, adv_below(a, 4) ? ADV_FLAG_LO + adv_below(a, ADV_FLAG_HI - ADV_FLAG_LO + 1) : 1 + adv_below(a, 300), 1);
    buf[0] = COOP_K_DIGEST_VER, buf[1] = 0xEE, buf[2] = 0xFF, buf[3] = 0xC0, buf[4] = 0, buf[5] = (uint8_t)n;
    size_t len = seal_as(&a->x, buf, (size_t)(p - buf), COOP_MAX_MESSAGE);
    int as = 1 + (int)adv_below(a, ADV_H + ADV_M);
    for (int k = 0; k < ADV_H; k++)
        if (a->net.present[k]) coop_sync_receive(a->h[k].eng, as, COOP_K_SECTION, buf, len, a->now);
    adv_forged++;
}

// A roster at one honest game: a member left out, a session nobody holds for one, an earlier session of one, or a
// member nobody is; the relay's own roster comes back after a while.
static void adv_forge_roster(Adv *a) {
    int k = (int)adv_below(a, ADV_H), ids[MAX_PLAYERS + 1], maker;
    uint64_t sess[MAX_PLAYERS + 1];
    int n = adv_list(a, ids, sess, &maker), i = n ? (int)adv_below(a, (unsigned)n) : 0, what = (int)adv_below(a, 4);
    if (!a->net.present[k] || n < 2) return;
    if (ids[i] == a->h[k].id) i = (i + 1) % n;
    if (what == 0) {
        ids[i] = ids[n - 1], sess[i] = sess[n - 1], n--;
    } else if (what == 1) {
        sess[i] = adv_rnd(a) | 1;
    } else if (what == 2) {
        for (unsigned t = 0; t < a->net.log_n && t < ADV_LOG; t++)
            if (a->net.log[t].from == ids[i] && a->net.log[t].sess != sess[i]) {
                sess[i] = a->net.log[t].sess;
                break;
            }
    } else {
        for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
            int taken = 0;
            for (int t = 0; t < n; t++) taken |= ids[t] == id;
            if (!taken && !adv_below(a, 2)) {
                ids[n] = id, sess[n++] = adv_rnd(a) | 1;
                break;
            }
        }
    }
    coop_sync_room(a->h[k].eng, a->code, a->h[k].id, ids, sess, n, maker, a->now);
    adv_rosters++;
    a->now = run_by(&a->net, a->now, 100 + adv_below(a, 2000), 16);
    adv_roster(a);
}

// What an honest game sent before (its identity or a section, of its session now or an earlier one, also of one that
// left) to another honest game, after a roster there that lists the session it was sent in.
static void adv_replay(Adv *a) {
    unsigned have = a->net.log_n < ADV_LOG ? a->net.log_n : ADV_LOG;
    int k = (int)adv_below(a, ADV_H), ids[MAX_PLAYERS + 1], maker, found = 0, forged = 0;
    uint64_t sess[MAX_PLAYERS + 1];
    if (!have || !a->net.present[k]) return;
    Packet *q = &a->net.log[adv_below(a, have)];
    if (q->from == a->h[k].id) return;
    int n = adv_list(a, ids, sess, &maker);
    for (int i = 0; i < n; i++)
        if (ids[i] == q->from) found = 1, forged = sess[i] != q->sess, sess[i] = q->sess;
    if (!found) ids[n] = q->from, sess[n++] = q->sess, forged = 1;
    if (forged) coop_sync_room(a->h[k].eng, a->code, a->h[k].id, ids, sess, n, maker, a->now);
    if (q->kind == COOP_K_DIGEST) adv_led_snap(a, &a->h[k]);
    coop_sync_receive(a->h[k].eng, q->from, q->kind, q->b, q->n, a->now);
    adv_replays++;
    if (forged) a->now = run_by(&a->net, a->now, 100 + adv_below(a, 1500), 16), adv_roster(a);
}

// Where the charter of digest d begins (after its identity and the byte that says it carries one); 0 when it has none.
static size_t charter_at(const Packet *d) {
    size_t at = 5 + COOP_PK_LEN + 8, nl = d->b[at];
    at += 1 + nl + COOP_SIG_LEN;
    return at < d->n && d->b[at] ? at + 1 : 0;
}

// The last message of a kind honest game s sent in its session now, from the log; NULL when there is none.
static const Packet *adv_logged(const Adv *a, int s, uint8_t kind) {
    const Packet *q = NULL;
    unsigned have = a->net.log_n < ADV_LOG ? a->net.log_n : ADV_LOG;
    for (unsigned t = 0; t < have; t++) {
        const Packet *p = &a->net.log[(a->net.log_n - have + t) % ADV_LOG];
        if (p->from == a->h[s].id && p->kind == kind && p->sess == a->h[s].session) q = p;
    }
    return q;
}

// What anyone with the room's secret can make of honest game s's genuine messages (the sixth review), to honest game k:
// 40 copies of s's last digest with the maker's charter in it far later, its signature as it was; s's digest with one
// byte of its name changed, after two rosters that make k meet s again; s's digest with one mode of its charter changed,
// its signature as it was; or s's last section with one byte changed. Nothing of it may make k ignore s, keep s
// unidentified at k, or keep the maker's charter from k once the genuine messages come.
static void adv_tamper(Adv *a) {
    int k = (int)adv_below(a, ADV_H), s = (int)adv_below(a, ADV_H), what = (int)adv_below(a, 4), ids[MAX_PLAYERS + 1], maker;
    uint64_t sess[MAX_PLAYERS + 1];
    if (k == s || !a->net.present[k] || !a->net.present[s]) return;
    const Packet *q = adv_logged(a, s, what == 3 ? COOP_K_SECTION : COOP_K_DIGEST);
    if (!q) return;
    Packet d = *q;
    size_t c = what == 3 ? 0 : charter_at(&d);
    if (what == 3) {
        if (d.n <= 6 + 8 + COOP_SIG_LEN) return;
        d.b[6 + adv_below(a, (unsigned)(d.n - 6 - 8 - COOP_SIG_LEN))] ^= (uint8_t)(1 + adv_below(a, 255));
    } else if (what == 1) {
        if (!d.b[5 + COOP_PK_LEN + 8]) return;
        d.b[5 + COOP_PK_LEN + 8 + 1] ^= 1;
        int n = adv_list(a, ids, sess, &maker);
        uint64_t real[MAX_PLAYERS + 1];
        memcpy(real, sess, sizeof real);
        for (int i = 0; i < n; i++)
            if (ids[i] == a->h[s].id) sess[i] = adv_rnd(a) | 1;
        coop_sync_room(a->h[k].eng, a->code, a->h[k].id, ids, sess, n, maker, a->now);
        coop_sync_room(a->h[k].eng, a->code, a->h[k].id, ids, real, n, maker, a->now);
    } else if (!c) {
        return;
    } else if (what == 0) {
        d.b[c + 3] |= 0x40;   // the charter's epoch, 2^30 on
    } else {
        d.b[c + 5] ^= 1;      // one mode
    }
    for (int i = 0, copies = what ? 1 : 40; i < copies; i++) {
        if (d.kind == COOP_K_DIGEST) adv_led_snap(a, &a->h[k]);
        coop_sync_receive(a->h[k].eng, a->h[s].id, d.kind, d.b, d.n, a->now);
    }
    adv_tampered++;
}

// An identity of the attacker's own key for honest game s's session, at honest game k, often right after s joined the
// room again (so before s's own identity reaches k), once or a few times a second or two apart: k takes s as neither
// while both are presented, and as s by its own key once the attacker stops.
static void adv_impostor(Adv *a) {
    int k = (int)adv_below(a, ADV_H), s = (int)adv_below(a, ADV_H);
    if (k == s || !a->net.present[k] || !a->net.present[s]) return;
    if (adv_below(a, 2)) a->h[s].session = adv_rnd(a) | 1, adv_roster(a);
    uint8_t seed[32], d[COOP_MAX_PAYLOAD];
    char name[8];
    game_key_seed(&a->x, a->code, seed);
    snprintf(name, sizeof name, "P%d", a->h[s].id);
    size_t n = make_digest(d, a->code, seed, a->h[s].id, a->h[s].session, name, strlen(name), 0, NULL, 0);
    for (int i = 0, copies = 1 + (int)adv_below(a, 3); i < copies; i++) {
        if (i) a->now = run_by(&a->net, a->now, 1000 + adv_below(a, 1500), 16);
        if (!a->net.present[k]) break;
        adv_led_snap(a, &a->h[k]);
        coop_sync_receive(a->h[k].eng, a->h[s].id, COOP_K_DIGEST, d, n, a->now);
    }
    adv_impostors++;
}

// ---- the run

// A pool reset as docs/coop.md says to make one: the maker removes the hostile keys it admitted first; then, after a
// quiet moment in which the players who ask accept the room as it is now (a carry counts the keys a game counts when
// the reset reaches it), the maker resets (sometimes twice). One time in three the removal and the reset leave in one
// charter, as one save of nx2navi.ini with both lines makes them.
static void adv_reset(Adv *a) {
    int together = adv_below(a, 3) == 0;
    for (int j = 0; j < a->nmkid; j++)
        if (coop_sync_accepted_key(a->h[0].eng, a->mkid[j])) adv_remove(a, j);
    if (!together) {
        a->now = run_by(&a->net, a->now, 1500, 16);
        adv_ask_all(a);
        a->now = run_by(&a->net, a->now, 500, 16);
        adv_ask_all(a);
    }
    adv_together += (unsigned long long)together;
    adv_reset_pools(a);
    if (adv_below(a, 3) == 0) {
        a->now = run_by(&a->net, a->now, 300, 16);
        adv_reset_pools(a);
    }
}

static void adv_event(Adv *a) {
    unsigned e = adv_below(a, 112);
    if (adv_trace_on) fprintf(stderr, "[%llu] event %u\n", (unsigned long long)a->now, e);
    int i = (int)adv_below(a, ADV_M), k = (int)adv_below(a, ADV_H);
    int there = a->m_live[i] && a->net.present[ADV_H + i];
    if (e < 22) {
        if (a->net.present[k]) adv_honest_event(a, &a->h[k]);
    } else if (e < 28) {   // an honest player comes (back) into the room, in a new session
        if (!a->net.present[k]) a->net.present[k] = 1, a->h[k].session = adv_rnd(a) | 1, adv_roster(a);
    } else if (e < 36) {
        adv_hostile_move(a, i);
    } else if (e < 44) {
        if (there) adv_hostile_counters(a, i);
    } else if (e < 58) {
        if (there) adv_records(a, i);
    } else if (e < 66) {
        if (there) adv_flags(a, i);
    } else if (e < 70) {
        if (there) adv_charter(a, i);
    } else if (e < 76) {
        adv_forge_section(a);
    } else if (e < 82) {
        adv_forge_roster(a);
    } else if (e < 88) {
        adv_replay(a);
    } else if (e < 95) {
        if (a->nmkid) adv_remove(a, (int)adv_below(a, (unsigned)a->nmkid));
    } else if (e < 100) {
        adv_reset(a);
    } else if (e < 105) {
        adv_tamper(a);
    } else if (e < 109) {
        adv_impostor(a);
    } else if (k && a->net.present[k]) {   // an honest player other than the maker leaves the room for a while
        a->net.present[k] = 0, adv_roster(a);
    }
}

// legit[g]: game g accepted a hostile key or the attacker's (a player who accepts everything can take the attacker's key
// in another member's place before that member's own identity arrived), or (transitively) accepted a player who did.
static void adv_legit(Adv *a, int legit[ADV_H]) {
    for (int g = 0; g < ADV_H; g++) {
        legit[g] = coop_sync_accepted_key(a->h[g].eng, a->xkid);
        for (int j = 0; j < a->nmkid; j++) legit[g] |= coop_sync_accepted_key(a->h[g].eng, a->mkid[j]);
    }
    for (int changed = 1; changed;) {
        changed = 0;
        for (int g = 0; g < ADV_H; g++)
            for (int k = 0; k < ADV_H && !legit[g]; k++)
                if (k != g && legit[k] && coop_sync_accepted_key(a->h[g].eng, a->hkid[k])) legit[g] = changed = 1;
    }
}

// The flags only the hostile members or the attacker sent: in an honest game only if it accepted a hostile key, or a
// player who may hold them; and nothing applied while a game's room was not open to it.
static void adv_check_flags(Adv *a, const char *when) {
    int legit[ADV_H];
    adv_legit(a, legit);
    for (int g = 0; g < ADV_H; g++) {
        Game *h = &a->h[g];
        for (uint32_t x = ADV_FLAG_LO; x <= ADV_FLAG_HI && !legit[g]; x++)
            if (coop_sync_flag(h->eng, COOP_CAT_PROGRESS, x) || value_of(h, x) > 0)
                ADV_FAIL(a, "%s: game %d holds flag %u, which only hostile members or the attacker sent, and accepted none of them (stored %u, applied %d)",
                         when, h->id, x, coop_sync_flag(h->eng, COOP_CAT_PROGRESS, x), value_of(h, x));
        if (h->applied_closed || h->lowered)
            ADV_FAIL(a, "%s: game %d took %u values while its room was not open to it, lowered %d", when, h->id, h->applied_closed, h->lowered);
    }
}

// Every honest game's bound pools hold at least what the ledger says (clean_only: only the games whose pools count no
// hostile key, directly or through a player they accepted). The pools' values and whether they are bound into v and
// bound when set, for the end's other checks.
static void adv_check_pools(Adv *a, const char *when, int clean_only, int64_t v[ADV_POOLS][ADV_H], int bound[ADV_POOLS][ADV_H]) {
    int legit[ADV_H];
    adv_legit(a, legit);
    for (int p = 0; p < ADV_POOLS; p++) {
        for (int g = 0; g < ADV_H; g++) adv_led_epoch(a, g, p);
        for (int g = 0; g < ADV_H; g++) {
            int64_t pool, want;
            int b = coop_sync_pool(a->h[g].eng, adv_pool_hash[p], &pool);
            if (v) v[p][g] = pool, bound[p][g] = b;
            if (!b || !a->net.present[g] || (clean_only && legit[g]) || !adv_ledger(a, g, p, clean_only, &want)) continue;
            adv_checks++;
            if (pool < want)
                ADV_FAIL(a, "%s: pool %08x of game %d holds %lld, below the %lld the ledger of what the honest players brought, picked up and "
                         "used says", when, adv_pool_hash[p], g + 1, (long long)pool, (long long)want);
        }
    }
}

static int adv_run(uint64_t seed) {
    Adv *a = calloc(1, sizeof *a);
    if (!a) return 1;
    a->seed = seed, a->r = mix_seed(seed);
    for (int k = 0; k < ADV_H; k++)
        for (int p = 0; p < ADV_POOLS; p++) a->led[k][p].cur.floor = -(1 << 20), a->led[k][p].cur.valid = 1;
    rng = a->r | 1;   // the fake network's losses, from the seed too
    snprintf(a->code, sizeof a->code, "ADV%llx", (unsigned long long)seed);
    a->net.log = calloc(ADV_LOG, sizeof *a->net.log), a->net.log_cap = ADV_LOG, a->net.log_ids = ADV_H;
    for (int k = 0; k < ADV_H; k++) {
        Game *g = &a->h[k];
        game_init(g, 1 + k, 0x70000000ull + seed * 8 + (uint64_t)k);
        flag_add(g, COOP_CAT_KEYS, KEYS_WATER, 0, -1);
        a->hkid[k] = game_kid(g, a->code);
        g->ledger = a;
        if (adv_trace_on) coop_sync_trace(g->eng, adv_trace_line, g);
        a->net.games[k] = g;
    }
    int all_ask = seed % 4 == 3;   // nobody accepts the hostile members: the maker admits none of them
    a->rupees = (int)adv_below(a, 2);
    a->reloads = seed % 5 == 4;   // one seed in five: many saves loaded, so a pool's remembered joins fill up
    settings(&a->h[0], 1, COOP_MODE_SHARED, COOP_MODE_POOL, a->rupees ? COOP_MODE_POOL : COOP_MODE_OWN, COOP_MODE_OWN, COOP_MODE_SHARED);
    defaults(&a->h[1], 0), defaults(&a->h[2], 0);
    a->h[0].ask = all_ask, a->h[1].ask = all_ask || adv_below(a, 2), a->h[2].ask = 1;
    for (int k = 0; k < ADV_H; k++) adv_save(a, &a->h[k]), a->h[k].hold_pools = (int)adv_below(a, 2);
    a->net.loss = adv_below(a, 3) ? 0 : 10, a->net.dup = adv_below(a, 3) ? 0 : 5, a->net.reorder = adv_below(a, 3) ? 0 : 20;
    a->net.present[0] = 1;
    a->now = 1000;
    game_init(&a->x, ADV_X, 0x58000000ull + seed);   // the attacker's engine signs with its own key; it is in no roster
    a->xkid = game_kid(&a->x, a->code);
    int xid = ADV_X;
    coop_sync_room(a->x.eng, a->code, ADV_X, &xid, NULL, 1, 0, a->now);
    adv_roster(a);
    for (int ev = 0; ev < 60 && !a->fails; ev++) {
        a->now = run_by(&a->net, a->now, 200 + adv_below(a, 600), 16);
        adv_event(a);
        adv_ask_all(a);
        adv_check_flags(a, "during the run");
        if (ev % 10 == 9) {   // a quiet moment, the hostile members still there
            a->now = run_by(&a->net, a->now, 1500, 16);
            adv_ask_all(a);
            adv_check_pools(a, "during the run", 1, NULL, NULL);
        }
    }
    // the hostile members leave; every honest game is in the room, its save loaded
    for (int i = 0; i < ADV_M; i++) a->net.present[ADV_H + i] = 0;
    for (int k = 0; k < ADV_H; k++) a->net.present[k] = 1, a->h[k].hold_pools = 0;
    adv_roster(a);
    for (int t = 0; t < 3; t++) a->now = run_by(&a->net, a->now, 1500, 16), adv_ask_all(a);
    int reset = (seed >> 2) & 1, twice = (seed >> 3) & 1;
    if (!reset) {
        for (int j = 0; j < a->nmkid; j++) adv_remove(a, j);
    } else {
        for (int k = 0; k < ADV_H && twice; k++) a->h[k].hold_pools = 1;
        adv_reset_pools(a);
        a->now = run_by(&a->net, a->now, 1200, 16);
        if (twice) adv_reset_pools(a), a->now = run_by(&a->net, a->now, 1200, 16);
        for (int k = 0; k < ADV_H; k++) a->h[k].hold_pools = 0;
    }
    for (int t = 0; t < 8; t++) a->now = run_by(&a->net, a->now, 3000, 16), adv_ask_all(a);
    adv_check_flags(a, "at the end");
    for (int g = 0; g < ADV_H; g++) {
        CoopOffer o;
        coop_sync_offer(a->h[g].eng, &o, a->now);
        unsigned ignored = coop_sync_ignored(a->h[g].eng);
        CoopRules rg, r0;
        coop_sync_rules(a->h[g].eng, &rg), coop_sync_rules(a->h[0].eng, &r0);
        for (int k = 0; k < ADV_H; k++) {
            const CoopOfferMember *x = offer_member(&o, a->h[k].id);
            uint64_t kid = 0;
            if (k != g && !(x && x->heard && coop_sync_key_parse(x->key, &kid) && kid == a->hkid[k]))
                ADV_FAIL(a, "at the end: game %d does not identify game %d by its key in the session the relay lists for it (%s)", g + 1, k + 1,
                         !x ? "not in its room" : x->heard ? "another key holds its place" : "a roster, a replay or a forged digest ended or keeps that session");
            if (k != g && ignored >> a->h[k].id & 1) ADV_FAIL(a, "at the end: game %d ignores game %d", g + 1, k + 1);
        }
        uint64_t signer = 0;
        coop_sync_key_parse(o.maker_key, &signer);
        if (rg.epoch != r0.epoch || signer != a->hkid[0])
            ADV_FAIL(a, "at the end: game %d holds the charter of epoch %u signed by %s, not the maker's of epoch %u", g + 1, rg.epoch, o.maker_key, r0.epoch);
        if (!coop_sync_open(a->h[g].eng)) ADV_FAIL(a, "at the end: game %d's room is not open to it (accepted %d, unidentified %d)", g + 1, o.accepted, o.unidentified);
    }
    int64_t v[ADV_POOLS][ADV_H];
    int bound[ADV_POOLS][ADV_H];
    adv_check_pools(a, "at the end", 0, v, bound);
    for (int p = 0; p < (a->rupees ? 3 : 2) && !a->fails; p++)
        for (int g = 0; g < ADV_H; g++) {
            Flag *f = flag_of(&a->h[g], adv_pool_hash[p]);
            int64_t most = f->cap >= 0 ? f->cap : 15, show = v[p][g] < 0 ? 0 : v[p][g] > most ? most : v[p][g];
            if (!bound[p][g]) ADV_FAIL(a, "pool %08x of game %d is not bound at the end", adv_pool_hash[p], g + 1);
            else if (g && v[p][g] != v[p][0]) ADV_FAIL(a, "pool %08x: game %d holds %lld, game 1 %lld", adv_pool_hash[p], g + 1, (long long)v[p][g], (long long)v[p][0]);
            else if (f->value != show) ADV_FAIL(a, "pool %08x: game %d shows %d of a pool of %lld", adv_pool_hash[p], g + 1, f->value, (long long)v[p][g]);
        }
    int failed = a->fails != 0;
    for (int k = 0; k < ADV_H; k++) game_free(&a->h[k]);
    for (int i = 0; i < ADV_M; i++)
        if (a->m_live[i]) game_free(&a->m[i]);
    game_free(&a->x);
    free(a->net.q);
    free(a->net.log);
    free(a);
    return failed;
}

// NX2NAVI_COOP_ADVERSARY=first:count runs those seeds, and only them (the default is a few, so that --probe-coop stays
// quick; a long run takes thousands, in several processes).
static void probe_adversary(void) {
    unsigned long long first = 1, count = 12;
    const char *e = getenv("NX2NAVI_COOP_ADVERSARY");
    adv_trace_on = getenv("NX2NAVI_COOP_ADVERSARY_TRACE") != NULL;
    if (e && *e) sscanf(e, "%llu:%llu", &first, &count);
    unsigned failed = 0;
    adv_forged = adv_rosters = adv_replays = adv_checks = adv_tampered = adv_impostors = adv_together = 0;
    for (unsigned long long s = first; s < first + count; s++) failed += (unsigned)adv_run(s);
    fails += (int)failed;
    printf("coop-probe: sync adversary: %llu seeds from %llu, %u failed (hostile members who hold the secret, and an attacker with the "
           "secret and no accepted key: %llu forged sections, %llu forged rosters, %llu replays, %llu changed copies, %llu identities in "
           "another's place; %llu removals and resets in one charter; %llu pool checks against the ledger)\n",
           count, first, failed, adv_forged, adv_rosters, adv_replays, adv_tampered, adv_impostors, adv_together, adv_checks);
}

// ---- 11. the fifth review's findings

static int heard_at(Game *g, int id, uint64_t now) {   // g's offer shows member id identified
    CoopOffer o;
    coop_sync_offer(g->eng, &o, now);
    const CoopOfferMember *x = offer_member(&o, id);
    return x && x->heard;
}

// Finding 1: an attacker who holds the room's secret (and so the message key) but no key the players accepted, as a
// relay run by someone who knows the secret or a sender on a player's network can, makes sections in B's name. A, who
// accepted B, takes none of their flags, and a flood of them costs A no more than its budget of checks for B, while
// B's own flags still arrive.
static void probe_h5_forged_sender(void) {
    Net net = {0};
    Game a, b, x;
    game_init(&a, 1, 0x5101), game_init(&b, 2, 0x5102), game_init(&x, 7, 0x5107);
    defaults(&a, 1), defaults(&b, 0), defaults(&x, 0);
    net.games[0] = &a, net.games[1] = &b, net.present[0] = net.present[1] = 1;
    uint64_t now = run(&net, (net_roster(&net, "H5F1", 1000), 1000), 3000);
    int xid = 7;
    coop_sync_room(x.eng, "H5F1", 7, &xid, NULL, 1, 0, now);   // the attacker's own engine, with a key nobody accepted
    uint8_t buf[COOP_MAX_PAYLOAD], *p = buf + 6;
    for (uint32_t h = 241; h <= 250; h++) p = entry_flag(p, COOP_CAT_PROGRESS, h, 1);
    buf[0] = COOP_K_DIGEST_VER, buf[1] = 0xEE, buf[2] = 0xFF, buf[3] = 0xC0, buf[4] = 0, buf[5] = 10;
    size_t n = seal_as(&x, buf, (size_t)(p - buf), COOP_MAX_MESSAGE);
    coop_sync_receive(a.eng, 2, COOP_K_SECTION, buf, n, now);   // in B's name
    now = run(&net, now, 2000);
    Game *ab[2] = {&a, &b};
    int held = flags_held(ab, 2, 241, 250);
    CHECK(!held, "F1: a section made in B's name by someone without B's key gives A nothing (A and B hold %d of its 10 flags)", held);
    CoopSyncStats s0, s1;
    coop_sync_stats(a.eng, &s0);
    uint64_t t0 = now;
    for (int i = 0; i < 2000; i++) {   // 2 s of them, each another
        p = entry_flag(buf + 6, COOP_CAT_PROGRESS, 241 + (uint32_t)(i % 10), 1);
        buf[5] = 1;
        n = seal_as(&x, buf, (size_t)(p - buf), COOP_MAX_MESSAGE);
        coop_sync_receive(a.eng, 2, COOP_K_SECTION, buf, n, now += 1);
    }
    coop_sync_stats(a.eng, &s1);
    unsigned checks = s1.sig_checks - s0.sig_checks, most = 256 + 128 * (unsigned)((now - t0) / 1000 + 1);
    b.f[99].value = 1;   // B's own flag 100
    now = run(&net, now, 3000);
    held = flags_held(ab, 2, 241, 250);
    CHECK(!held && checks <= most && value_of(&a, 100) == 1,
          "F1: 2000 sections in B's name in 2 s: A and B hold %d of their flags, A checked %u signatures for them (at most %u), and B's own "
          "flag still reaches A (%d)", held, checks, most, value_of(&a, 100));
    game_free(&a), game_free(&b), game_free(&x);
    free(net.q);
    printf("coop-probe: sync forged sender: sections in another member's name, made with the room's secret alone, give nothing\n");
}

// Finding 2: no roster, and no replayed message or identity, ends a member's session for good. A (or C) identifies B in
// B's own session once the relay's roster is back, after (a) a roster that lists B's earlier session and B's message of
// it, replayed; (b) a roster without B; (c) the relay saying the room is gone; (d) as (a), at a game that joined after
// B's earlier session was over, with B's identity of it replayed.
static void probe_h5_session_end(void) {
    static const char *const what[4] = {"a roster with B's earlier session and B's message of it replayed", "a roster without B",
                                        "the relay saying the room is gone", "a roster with B's earlier session and B's identity of it replayed"};
    for (int v = 0; v < 4; v++) {
        char code[8];
        snprintf(code, sizeof code, "H5F2%c", 'A' + v);
        Net net = {0};
        Game a, b, c;
        game_init(&a, 1, 0x5201 + 16 * (uint64_t)v), game_init(&b, 2, 0x5202 + 16 * (uint64_t)v), game_init(&c, 3, 0x5203 + 16 * (uint64_t)v);
        defaults(&a, 1), defaults(&b, 0), defaults(&c, 0);
        net.games[0] = &a, net.games[1] = &b, net.games[2] = &c;
        net.present[0] = net.present[1] = 1, net.present[2] = v != 3;   // in (d) C comes after B's first session
        uint64_t now = run(&net, (net_roster(&net, code, 1000), 1000), 3000);
        Packet old = digest_of(&b, now + 3000);   // B's message of its first session: its identity in it
        uint64_t s1 = b.session;
        b.session ^= 0x5EED;                      // B joins again: its second session
        now = run(&net, (net_roster(&net, code, now), now), 4000);
        net.present[2] = 1;
        now = run(&net, (net_roster(&net, code, now), now), 4000);
        Game *t = v == 3 ? &c : &a;
        int ids[3] = {1, 2, 3}, two[2] = {1, 3};
        uint64_t first[3] = {a.session, s1, c.session}, ac[2] = {a.session, c.session};
        int before = heard_at(t, 2, now);
        if (v == 0 || v == 3) {
            coop_sync_room(t->eng, code, t->id, ids, first, 3, 1, now += 8);
            coop_sync_receive(t->eng, 2, COOP_K_DIGEST, old.b, old.n, now += 8);
        } else if (v == 1) {
            coop_sync_room(t->eng, code, t->id, two, ac, 2, 1, now += 8);
        } else {
#if !H5_SIGNED
            coop_sync_room_lost(t->eng);   // what the game side did when the relay answered that the room is gone
#endif
            coop_sync_room(t->eng, "", 0, NULL, NULL, 0, 0, now += 8);
        }
        now = run(&net, (net_roster(&net, code, now + 8), now + 8), 5000);   // the relay's roster is back
        int heard = heard_at(t, 2, now);
        CHECK(old.n && before && heard, "F2: after %s, %s identifies B again in B's own session once the relay's roster is back (%d; before %d)", what[v],
              v == 3 ? "C" : "A", heard, before);
        game_free(&a), game_free(&b), game_free(&c);
        free(net.q);
    }
    printf("coop-probe: sync session end: no roster, replayed message or replayed identity ends a member's session for good\n");
}

// Finding 4: a join claims against the records it counts, and makes its claim again as if it had counted that way at
// the join when a removal drops some of them (or its player accepts more): (a) keys a player brought while a removed
// player's gains counted stay; (b) a join made while a removed player's spends counted does not leave the pool above
// what the others brought; (c) honest games agree after an honest player's removal, and a removal gives back no key an
// honest player used.
static void probe_h5_join(void) {
    {
        Net net = {0};
        Game a, m;
        game_init(&a, 1, 0x5401), game_init(&m, 3, 0x5403);
        defaults(&a, 1), defaults(&m, 0);
        a.hold_pools = 1;   // A at the title screen
        net.games[0] = &a, net.games[1] = &m, net.present[0] = net.present[1] = 1;
        uint64_t now = run(&net, (net_roster(&net, "H5F4A", 1000), 1000), 5000);
        flag_of(&m, KEYS_FOREST)->value = 10;   // M joined the pool, and picks up 10
        now = run(&net, now, 3000);
        flag_of(&a, KEYS_FOREST)->value = 2, a.hold_pools = 0;   // A loads a save with 2 keys
        now = run(&net, now, 3000);
        int shown = value_of(&a, KEYS_FOREST), acted = coop_sync_remove(a.eng, 3);
        net.present[1] = 0;
        now = run(&net, (net_roster(&net, "H5F4A", now), now), 4000);
        int64_t v;
        coop_sync_pool(a.eng, KEYS_FOREST, &v);
        CHECK(acted && shown == 10 && v == 2 && value_of(&a, KEYS_FOREST) == 2,
              "F4: A's 2 keys, brought while M's 10 gains counted, stay after M's removal (pool %lld, shown %d; before the removal %d)", (long long)v,
              value_of(&a, KEYS_FOREST), shown);
        game_free(&a), game_free(&m);
        free(net.q);
    }
    {
        Net net = {0};
        Game a, b, m;
        game_init(&a, 1, 0x5411), game_init(&b, 2, 0x5412), game_init(&m, 3, 0x5413);
        defaults(&a, 1), defaults(&b, 0), defaults(&m, 0);
        flag_of(&a, KEYS_FOREST)->value = 3;
        b.hold_pools = 1;
        net.games[0] = &a, net.games[1] = &b, net.games[2] = &m, net.present[0] = net.present[1] = net.present[2] = 1;
        uint64_t now = run(&net, (net_roster(&net, "H5F4B", 1000), 1000), 5000);
        flag_of(&m, KEYS_FOREST)->value = 0;    // M, shown A's 3, uses them
        now = run(&net, now, 3000);
        int drained = value_of(&a, KEYS_FOREST);
        flag_of(&b, KEYS_FOREST)->value = 3, b.hold_pools = 0;   // B loads a save that shows the same 3 keys
        now = run(&net, now, 3000);
        int acted = coop_sync_remove(a.eng, 3);
        net.present[2] = 0;
        now = run(&net, (net_roster(&net, "H5F4B", now), now), 4000);
        int64_t va, vb;
        coop_sync_pool(a.eng, KEYS_FOREST, &va), coop_sync_pool(b.eng, KEYS_FOREST, &vb);
        CHECK(acted && drained == 0 && va == 3 && vb == 3 && value_of(&a, KEYS_FOREST) == 3 && value_of(&b, KEYS_FOREST) == 3,
              "F4: a join made while M's spends counted does not leave the pool above what A and B brought after M's removal (pools %lld %lld, "
              "shown %d %d; drained %d)", (long long)va, (long long)vb, value_of(&a, KEYS_FOREST), value_of(&b, KEYS_FOREST), drained);
        game_free(&a), game_free(&b), game_free(&m);
        free(net.q);
    }
    {
        Net net = {0};
        Game a, b, d, m;
        game_init(&a, 1, 0x5421), game_init(&b, 2, 0x5422), game_init(&d, 4, 0x5424), game_init(&m, 3, 0x5423);
        defaults(&a, 1), defaults(&b, 0), defaults(&d, 0), defaults(&m, 0);
        flag_of(&a, KEYS_FOREST)->value = 2;
        b.hold_pools = 1;
        net.games[0] = &a, net.games[1] = &b, net.games[2] = &d, net.games[3] = &m;
        net.present[0] = net.present[1] = net.present[2] = net.present[3] = 1;
        uint64_t now = run(&net, (net_roster(&net, "H5F4C", 1000), 1000), 5000);
        flag_of(&d, KEYS_FOREST)->value += 3;   // D picks up 3: the pool holds 5
        now = run(&net, now, 3000);
        flag_of(&b, KEYS_FOREST)->value = 1, b.hold_pools = 0;   // B joins with 1
        now = run(&net, now, 3000);
        flag_of(&b, KEYS_FOREST)->value -= 2;   // B uses 2: 3
        now = run(&net, now, 3000);
        int acted = coop_sync_remove(a.eng, 4) + coop_sync_remove(a.eng, 3);   // the maker removes D (an honest player) and M
        net.present[2] = net.present[3] = 0;
        now = run(&net, (net_roster(&net, "H5F4C", now), now), 5000);
        int64_t va, vb;
        coop_sync_pool(a.eng, KEYS_FOREST, &va), coop_sync_pool(b.eng, KEYS_FOREST, &vb);
        // A brought 2, B's 1 was among them, B used 2: 0, whatever D brought
        CHECK(acted == 2 && va == 0 && vb == 0 && value_of(&a, KEYS_FOREST) == 0 && value_of(&b, KEYS_FOREST) == 0,
              "F4: after the removal of D, who brought 3, and of M, A and B agree that A's 2 less B's 2 used are left (pools %lld %lld, shown %d %d)",
              (long long)va, (long long)vb, value_of(&a, KEYS_FOREST), value_of(&b, KEYS_FOREST));
        game_free(&a), game_free(&b), game_free(&d), game_free(&m);
        free(net.q);
    }
    printf("coop-probe: sync join: a claim is made again without the records a removal drops; honest games still agree\n");
}

// Note 3: another save loaded in an accepted room joins the pools only once its player accepted the room again.
static void probe_h5_rebind(void) {
    Net net = {0};
    Game a, b;
    game_init(&a, 1, 0x5501), game_init(&b, 2, 0x5502);
    defaults(&a, 1), defaults(&b, 0);
    flag_of(&a, KEYS_FOREST)->value = 2, flag_of(&b, KEYS_FOREST)->value = 2;
    net.games[0] = &a, net.games[1] = &b, net.present[0] = net.present[1] = 1;
    uint64_t now = run(&net, (net_roster(&net, "H5N3", 1000), 1000), 5000);
    int32_t base0 = 0, base1 = 0;
    uint32_t g, s;
    int had = coop_sync_own_record(b.eng, KEYS_FOREST, &base0, &g, &s);
    b.ask = 1;   // B's player asks from now on, and loads another save, with 6 keys
    reload_save(&b);
    flag_of(&b, KEYS_FOREST)->value = 6;
    now = run(&net, now, 4000);
    coop_sync_own_record(b.eng, KEYS_FOREST, &base1, &g, &s);
    int bound = coop_sync_pool(b.eng, KEYS_FOREST, NULL);
    int64_t va;
    coop_sync_pool(a.eng, KEYS_FOREST, &va);
    CHECK(had && !bound && base1 == base0 && va == 2,
          "note: another save loaded in an accepted room joins no pool before its offer is accepted (bound %d, B's base %d then %d, A's pool %lld)",
          bound, base0, base1, (long long)va);
    CoopOffer o;
    coop_sync_offer(b.eng, &o, now);
    coop_sync_accept(b.eng, o.token);
    now = run(&net, now, 4000);
    coop_sync_pool(a.eng, KEYS_FOREST, &va);
    CHECK(va == 6 && value_of(&a, KEYS_FOREST) == 6, "note: once accepted, the other save joins the pools (A's pool %lld)", (long long)va);
    game_free(&a), game_free(&b);
    free(net.q);
    printf("coop-probe: sync rebind: another save joins the pools once its player accepted the room again\n");
}

// A claim waiting to be made again (after a removal, or more keys accepted) is made also while the game reports no
// pool counter (its title screen): the others count this player's keys at once, and a pool reset made meanwhile
// carries them for the others too, so no key another player picks up after the reset is absorbed when this player
// comes back (seed 46304 of the randomised run).
static void probe_h5_claim_at_title(void) {
    Net net = {0};
    Game a, b, m;
    game_init(&a, 1, 0x5601), game_init(&b, 2, 0x5602), game_init(&m, 3, 0x5603);
    defaults(&a, 1), defaults(&b, 0), defaults(&m, 0);
    a.hold_pools = 1;   // A at the title screen
    net.games[0] = &a, net.games[1] = &m, net.games[2] = &b, net.present[0] = net.present[1] = 1;
    uint64_t now = run(&net, (net_roster(&net, "H5T", 1000), 1000), 5000);
    flag_of(&m, KEYS_FOREST)->value = 10;   // M joined the pool, and picks up 10
    now = run(&net, now, 3000);
    flag_of(&a, KEYS_FOREST)->value = 2, a.hold_pools = 0;   // A loads a save with 2 keys: its claim counts M's 10
    now = run(&net, now, 3000);
    a.hold_pools = 1;   // A back at its title screen, where the maker removes M
    int acted = coop_sync_remove(a.eng, 3);
    net.present[1] = 0, net.present[2] = 1;   // M leaves; B, who never saw M, comes with no key
    now = run(&net, (net_roster(&net, "H5T", now), now), 5000);
    int64_t after_removal;
    coop_sync_pool(b.eng, KEYS_FOREST, &after_removal);
    int reset = coop_sync_reset_pools(a.eng);   // still at the title screen, the maker resets the pools
    now = run(&net, now, 5000);
    flag_of(&b, KEYS_FOREST)->value += 1;   // B picks up one more
    now = run(&net, now, 3000);
    a.hold_pools = 0;   // A plays again
    now = run(&net, now, 5000);
    int64_t va, vb;
    coop_sync_pool(a.eng, KEYS_FOREST, &va), coop_sync_pool(b.eng, KEYS_FOREST, &vb);
    CHECK(acted && reset && after_removal == 2 && va == 3 && vb == 3 && value_of(&a, KEYS_FOREST) == 3 && value_of(&b, KEYS_FOREST) == 3,
          "closing 5: a claim is made again after a removal while its game is at the title screen: B counts A's 2 keys (%lld), and after a "
          "reset meanwhile and B's pickup the pools hold A's 2 and B's 1 (pools %lld %lld, shown %d %d)",
          (long long)after_removal, (long long)va, (long long)vb, value_of(&a, KEYS_FOREST), value_of(&b, KEYS_FOREST));
    game_free(&a), game_free(&b), game_free(&m);
    free(net.q);
    printf("coop-probe: sync claim at the title screen: a removal makes the claim again also while the game plays no save\n");
}

// ---- 12. the sixth review's findings

// Finding 2: a digest is not signed by its sender, so what fails in one is held against nobody, and a changed copy of
// a genuine signature does not make the genuine one fail. (a) 40 digests in B's name, each with B's identity and the
// maker's charter one epoch later, its signature as it was; (b) two rosters that make A meet B again, then B's own
// digest with one byte of its name changed; (c) the maker's new charter (a removal) with one mode changed and its
// signature as it was, in the maker's name before the genuine one arrives. A keeps taking B's values and identifies B,
// and B takes the genuine removal.
static void probe_h6_digests(void) {
    {
        Net net = {0};
        Game a, b;
        game_init(&a, 1, 0x6101), game_init(&b, 2, 0x6102);
        defaults(&a, 1), defaults(&b, 0);
        net.games[0] = &a, net.games[1] = &b, net.present[0] = net.present[1] = 1;
        uint64_t now = run(&net, (net_roster(&net, "H6F2A", 1000), 1000), 3000);
        Packet d = digest_of(&b, now + 3000);
        size_t c = charter_at(&d);
        if (c) d.b[c] ^= 0x80;   // the epoch, 128 later
        for (int i = 0; i < 40 && c; i++) coop_sync_receive(a.eng, 2, COOP_K_DIGEST, d.b, d.n, now += 20);
        b.f[99].value = 1;
        now = run(&net, now, 3000);
        CHECK(c && !(coop_sync_ignored(a.eng) >> 2 & 1) && value_of(&a, 100) == 1,
              "H6 F2a: 40 digests in B's name carrying the maker's charter with a later epoch and a signature that does not check out leave B "
              "counted at A (ignored %u) and B's flag reaches A (%d)", coop_sync_ignored(a.eng) >> 2 & 1, value_of(&a, 100));
        game_free(&a), game_free(&b);
        free(net.q);
    }
    {
        Net net = {0};
        Game a, b;
        game_init(&a, 1, 0x6111), game_init(&b, 2, 0x6112);
        defaults(&a, 1), defaults(&b, 0);
        net.games[0] = &a, net.games[1] = &b, net.present[0] = net.present[1] = 1;
        uint64_t now = run(&net, (net_roster(&net, "H6F2B", 1000), 1000), 3000);
        Packet d = digest_of(&b, now + 3000);
        d.b[5 + COOP_PK_LEN + 8 + 1] ^= 1;   // one byte of B's name; its signature as it was
        int ids[2] = {1, 2};
        uint64_t made_up[2] = {a.session, 0xBAD5E55}, real[2] = {a.session, b.session};
        coop_sync_room(a.eng, "H6F2B", 1, ids, made_up, 2, 1, now += 8);
        coop_sync_room(a.eng, "H6F2B", 1, ids, real, 2, 1, now += 8);
        coop_sync_receive(a.eng, 2, COOP_K_DIGEST, d.b, d.n, now += 8);
        b.f[100].value = 1;
        now = run(&net, now, 6000);
        CHECK(heard_at(&a, 2, now) && value_of(&a, 101) == 1 && !(coop_sync_ignored(a.eng) >> 2 & 1),
              "H6 F2b: after two rosters and B's digest with one byte of its name changed, A identifies B by B's own digests (%d), takes B's "
              "flag (%d) and does not ignore B (%u)", heard_at(&a, 2, now), value_of(&a, 101), coop_sync_ignored(a.eng) >> 2 & 1);
        game_free(&a), game_free(&b);
        free(net.q);
    }
    {
        Net net = {0};
        Game a, b, m;
        game_init(&a, 1, 0x6121), game_init(&b, 2, 0x6122), game_init(&m, 3, 0x6123);
        defaults(&a, 1), defaults(&b, 0), defaults(&m, 0);
        net.games[0] = &a, net.games[1] = &b, net.games[2] = &m, net.present[0] = net.present[1] = net.present[2] = 1;
        uint64_t now = run(&net, (net_roster(&net, "H6F2C", 1000), 1000), 5000);
        flag_of(&m, KEYS_FOREST)->value = 2;   // M picks up 2
        now = run(&net, now, 3000);
        int64_t before;
        coop_sync_pool(b.eng, KEYS_FOREST, &before);
        int acted = coop_sync_remove(a.eng, 3);
        net.present[2] = 0;
        net_roster(&net, "H6F2C", now);
        Packet d = digest_of(&a, now += 8);   // the maker's digest with the removal, caught before it reached B
        size_t c = charter_at(&d);
        if (c) d.b[c + 5] ^= 1;               // one mode of the charter changed, its signature as it was
        coop_sync_receive(b.eng, 1, COOP_K_DIGEST, d.b, d.n, now += 8);
        now = run(&net, now, 6000);
        CoopRules ra, rb;
        coop_sync_rules(a.eng, &ra), coop_sync_rules(b.eng, &rb);
        int64_t vb;
        coop_sync_pool(b.eng, KEYS_FOREST, &vb);
        CHECK(c && acted && before == 2 && rb.epoch == ra.epoch && vb == 0 && !(coop_sync_ignored(b.eng) >> 1 & 1),
              "H6 F2c: a changed copy of the maker's removal, sent first, keeps neither the genuine one from B (epochs %u %u, B's pool %lld, "
              "before %lld) nor counts against the maker (ignored %u)", ra.epoch, rb.epoch, (long long)vb, (long long)before,
              coop_sync_ignored(b.eng) >> 1 & 1);
        game_free(&a), game_free(&b), game_free(&m);
        free(net.q);
    }
    printf("coop-probe: sync digests: what fails in a digest counts against nobody, and a changed copy does not make the genuine one fail\n");
}

// Finding 1's root cause in the sync layer: an identity is bound to a member's session, but not to one key. Someone
// with the room's secret who is first with an identity of its own key for B's new session at A held B's place at A for
// the whole session. Now the two keys are disputed (A takes B as neither) while both are presented, and once the other
// key stops, B's own takes the place.
static void probe_h6_identity_race(void) {
    Net net = {0};
    Game a, b;
    game_init(&a, 1, 0x6201), game_init(&b, 2, 0x6202);
    defaults(&a, 1), defaults(&b, 0);
    net.games[0] = &a, net.games[1] = &b, net.present[0] = net.present[1] = 1;
    uint64_t now = run(&net, (net_roster(&net, "H6R", 1000), 1000), 3000);
    b.session ^= 0x6EED;   // B joins again: a new session
    net_roster(&net, "H6R", now);
    uint8_t seed_x[32] = {0x58, 6}, d[COOP_MAX_PAYLOAD];
    size_t dn = make_digest(d, "H6R", seed_x, 2, b.session, "P2", 2, 0, NULL, 0);
    coop_sync_receive(a.eng, 2, COOP_K_DIGEST, d, dn, now += 8);   // before B's own identity reaches A
    now = run(&net, now, 4000);
    int during = heard_at(&a, 2, now);
    now = run(&net, now, 12000);   // the other key is not presented again
    CoopOffer o;
    coop_sync_offer(a.eng, &o, now);
    const CoopOfferMember *x = offer_member(&o, 2);
    uint64_t kid = 0;
    int own = x && x->heard && coop_sync_key_parse(x->key, &kid) && kid == game_kid(&b, "H6R");
    b.f[101].value = 1;
    now = run(&net, now, 3000);
    CHECK(!during && own && value_of(&a, 102) == 1,
          "H6 F1: another key's identity for B's new session, at A before B's own: A takes B as neither while both are presented (%d), as "
          "B by B's key once the other stops (%d), and takes B's flag (%d)", during, own, value_of(&a, 102));
    game_free(&a), game_free(&b);
    free(net.q);
    printf("coop-probe: sync identity race: another key first for a member's session disputes it only while it keeps coming\n");
}

// Finding 3: a reset's carry is a claim from the reset on. (a) The maker, at its title screen with a claim of 2, resets;
// B arrives after the reset, joins with nothing and picks up a key; the maker loads its save with 2: the pools hold 3.
// (b) P accepted M and joined with 3 while M's 10 gains counted; G declined M; the maker removes M and resets in one
// charter (one save of nx2navi.ini), P at its title screen; G picks up a key; P loads its save with 3: the pools hold 4.
static void probe_h6_carry(void) {
    {
        Net net = {0};
        Game a, b;
        game_init(&a, 1, 0x6301), game_init(&b, 2, 0x6302);
        defaults(&a, 1), defaults(&b, 0);
        flag_of(&a, KEYS_FOREST)->value = 2;
        net.games[0] = &a, net.games[1] = &b, net.present[0] = 1;
        uint64_t now = run(&net, (net_roster(&net, "H6F3A", 1000), 1000), 5000);
        a.hold_pools = 1;   // the maker at its title screen
        int reset = coop_sync_reset_pools(a.eng);
        now = run(&net, now, 2000);
        net.present[1] = 1;   // B arrives, with no key
        now = run(&net, (net_roster(&net, "H6F3A", now), now), 5000);
        flag_of(&b, KEYS_FOREST)->value += 1;   // and picks one up
        now = run(&net, now, 3000);
        a.hold_pools = 0;   // the maker loads its save with 2
        now = run(&net, now, 5000);
        int64_t va, vb;
        coop_sync_pool(a.eng, KEYS_FOREST, &va), coop_sync_pool(b.eng, KEYS_FOREST, &vb);
        CHECK(reset && va == 3 && vb == 3 && value_of(&a, KEYS_FOREST) == 3 && value_of(&b, KEYS_FOREST) == 3,
              "H6 F3a: the maker's carry of 2 and B's pickup after the reset, B arriving after it: the pools hold 3 (pools %lld %lld, shown %d %d)",
              (long long)va, (long long)vb, value_of(&a, KEYS_FOREST), value_of(&b, KEYS_FOREST));
        game_free(&a), game_free(&b);
        free(net.q);
    }
    {
        Net net = {0};
        Game a, p, g, m;
        game_init(&a, 1, 0x6311), game_init(&p, 2, 0x6312), game_init(&g, 3, 0x6313), game_init(&m, 4, 0x6314);
        defaults(&a, 1), defaults(&p, 0), defaults(&g, 0), defaults(&m, 0);
        g.ask = 1, p.hold_pools = 1, a.hold_pools = 1;   // the maker at its title screen throughout: its counter holds no gift of M's
        net.games[0] = &a, net.games[1] = &p, net.games[2] = &g, net.games[3] = &m;
        net.present[0] = net.present[1] = net.present[2] = 1;
        uint64_t now = run(&net, (net_roster(&net, "H6F3B", 1000), 1000), 5000);
        CoopOffer o;
        int g_ok = coop_sync_offer(g.eng, &o, now) && coop_sync_accept(g.eng, o.token);   // G accepts A and P
        net.present[3] = 1;   // M comes; A and P accept it, G does not
        now = run(&net, (net_roster(&net, "H6F3B", now), now), 4000);
        flag_of(&m, KEYS_FOREST)->value = 10;   // M picks up 10
        now = run(&net, now, 3000);
        flag_of(&p, KEYS_FOREST)->value = 3, p.hold_pools = 0;   // P loads a save with 3: its claim counts M's 10
        now = run(&net, now, 3000);
        int32_t pb = 0;
        uint32_t pg, ps;
        int64_t g_saw = -1, g_after = -1;
        coop_sync_own_record(p.eng, KEYS_FOREST, &pb, &pg, &ps);   // P's claim absorbed into M's gains
        coop_sync_pool(g.eng, KEYS_FOREST, &g_saw);                // and G, who does not count M, counts none of P's 3
        p.hold_pools = 1;   // P at its title screen
        int acted = coop_sync_remove(a.eng, 4) && coop_sync_reset_pools(a.eng);   // one save of nx2navi.ini: one charter
        net.present[3] = 0;
        now = run(&net, (net_roster(&net, "H6F3B", now), now), 5000);
        coop_sync_pool(g.eng, KEYS_FOREST, &g_after);
        flag_of(&g, KEYS_FOREST)->value += 1;   // G picks up one
        now = run(&net, now, 3000);
        flag_of(&p, KEYS_FOREST)->value = 3, p.hold_pools = 0, a.hold_pools = 0;   // P plays its save with 3 again; the maker one with none
        now = run(&net, now, 5000);
        int64_t v[3];
        coop_sync_pool(a.eng, KEYS_FOREST, &v[0]), coop_sync_pool(p.eng, KEYS_FOREST, &v[1]), coop_sync_pool(g.eng, KEYS_FOREST, &v[2]);
        CHECK(g_ok && acted && pb < 3 && g_saw == 0 && v[0] == 4 && v[1] == 4 && v[2] == 4 && value_of(&g, KEYS_FOREST) == 4 && value_of(&p, KEYS_FOREST) == 4,
              "H6 F3b: the removal and the reset in one charter while P is at its title screen, then G's pickup: P's 3 and G's 1 stay (pools "
              "%lld %lld %lld, shown P %d G %d; P's base %d and G's pool %lld before, G's pool %lld after the reset)", (long long)v[0], (long long)v[1],
              (long long)v[2], value_of(&p, KEYS_FOREST), value_of(&g, KEYS_FOREST), pb, (long long)g_saw, (long long)g_after);
        game_free(&a), game_free(&p), game_free(&g), game_free(&m);
        free(net.q);
    }
    printf("coop-probe: sync carry: a reset's carry is a claim from the reset on, also when the removal comes in the same charter\n");
}

// ---- 13. the seventh review's findings

// Finding 3: a key the player accepted, in another member's place and under that member's name. A (who asks) accepted
// the room with B and M in it. M leaves; B joins again in a new session and the relay holds B's messages to A back; M,
// who still has the secret, sends A an identity of its own key for member 2 in B's new session under B's name, then a
// section that key signs there. A must be asked again before anything of it counts, and its log must say so; once B's
// own identity arrives the two keys are disputed, and that is logged too. And a relay restart that hands the members
// other numbers, under their own names, is no new offer.
static void probe_h7_moved_key(void) {
    {
        const char *code = "H7F3A";
        Net net = {0};
        Game a, b, m;
        game_init(&a, 1, 0x7101), game_init(&b, 2, 0x7102), game_init(&m, 4, 0x7104);
        defaults(&a, 1), defaults(&b, 0), defaults(&m, 0);
        a.ask = 1;
        net.games[0] = &a, net.games[1] = &b, net.games[2] = &m;
        net.present[0] = net.present[1] = net.present[2] = 1;
        uint64_t now = run(&net, (net_roster(&net, code, 1000), 1000), 6000);
        CoopOffer o;
        coop_sync_offer(a.eng, &o, now);
        int took = coop_sync_accept(a.eng, o.token);
        now = run(&net, now, 2000);
        net.present[2] = 0;   // M leaves
        now = run(&net, (net_roster(&net, code, now), now), 2000);
        b.session ^= 0x5107;   // B joins again; the relay holds its messages back from A
        net.mute[2] = 1;
        now = run(&net, (net_roster(&net, code, now), now), 100);
        CoopSyncStats s0, s1, s2;
        coop_sync_stats(a.eng, &s0);
        uint8_t mseed[32], d[COOP_MAX_PAYLOAD];
        game_key_seed(&m, code, mseed);
        size_t dn = make_digest(d, code, mseed, 2, b.session, "P2", 2, 0, NULL, 0);   // M's key, member 2, B's session, B's name
        coop_sync_receive(a.eng, 2, COOP_K_DIGEST, d, dn, now += 8);
        int ids[2] = {1, 2};
        uint64_t sess[2] = {a.session, b.session};
        coop_sync_room(m.eng, code, 2, ids, sess, 2, 1, now);   // M's engine signs as member 2 in B's session
        uint8_t buf[COOP_MAX_PAYLOAD], *p = buf + 6;
        *p++ = COOP_R_FLAG, *p++ = COOP_CAT_PROGRESS, p = put_le(p, 250, 4), p = put_le(p, 1, 4);
        buf[0] = COOP_K_DIGEST_VER, buf[1] = 0xEE, buf[2] = 0xFF, buf[3] = 0xC0, buf[4] = 0, buf[5] = 1;
        size_t n = coop_sync_sign_section(m.eng, buf, (size_t)(p - buf), COOP_MAX_MESSAGE);
        coop_sync_receive(a.eng, 2, COOP_K_SECTION, buf, n, now += 8);
        now = run(&net, now, 1000);
        coop_sync_offer(a.eng, &o, now);
        coop_sync_stats(a.eng, &s1);
        const CoopOfferMember *x = offer_member(&o, 2);
        uint64_t kid = 0;
        int as_m = x && x->heard && coop_sync_key_parse(x->key, &kid) && kid == game_kid(&m, code);
        CHECK(took && as_m && !o.accepted && !coop_sync_open(a.eng) && value_of(&a, 250) == 0,
              "H7 F3: a key A accepted, in member 2's place under B's name, is a new offer: accepted %d, open %d, its flag applied %d (M's key "
              "there %d)", o.accepted, coop_sync_open(a.eng), value_of(&a, 250), as_m);
#ifndef COOP_BEFORE7
        CHECK(x && !strcmp(x->accepted_as, "P4") && s1.key_moves - s0.key_moves == 1 && s1.key_move_member == 2 && s1.key_move_from == 4,
              "H7 F3: the offer shows it was accepted as P4 (\"%s\"), and it is counted for the log as member 4's key now member 2's (%u, %d, %d)",
              x ? x->accepted_as : "-", s1.key_moves - s0.key_moves, s1.key_move_member, s1.key_move_from);
#endif
        net.mute[2] = 0;   // B's own identity arrives: two keys for its session
        now = run(&net, now, 3000);
        coop_sync_offer(a.eng, &o, now);
        coop_sync_stats(a.eng, &s2);
        x = offer_member(&o, 2);
        CHECK(x && !x->heard && o.unidentified && !coop_sync_open(a.eng), "H7 F3: once B's own identity arrives, member 2 is disputed (heard %d)", x ? x->heard : -1);
#ifndef COOP_BEFORE7
        CHECK(s2.key_changes - s1.key_changes == 1 && s2.key_change_member == 2, "H7 F3: the dispute is counted for the log (%u, member %d)",
              s2.key_changes - s1.key_changes, s2.key_change_member);
#endif
        game_free(&a), game_free(&b), game_free(&m);
        free(net.q);
    }
    {
        const char *code = "H7F3B";
        Net net = {0};
        Game a, b, c;
        game_init(&a, 1, 0x7111), game_init(&b, 2, 0x7112), game_init(&c, 3, 0x7113);
        defaults(&a, 1), defaults(&b, 0), defaults(&c, 0);
        a.ask = 1;
        net.games[0] = &a, net.games[1] = &b, net.games[2] = &c;
        net.present[0] = net.present[1] = net.present[2] = 1;
        uint64_t now = run(&net, (net_roster(&net, code, 1000), 1000), 6000);
        CoopOffer o;
        coop_sync_offer(a.eng, &o, now);
        int took = coop_sync_accept(a.eng, o.token);
        now = run(&net, now, 2000);
        // the relay restarted: everyone joins again in a new session, and B and C get each other's numbers; their names
        // go with them (a game's name is its own, the number the relay's)
        coop_sync_name(b.eng, "P2"), coop_sync_name(c.eng, "P3");
        b.id = 3, c.id = 2, by_id[2] = &c, by_id[3] = &b;
        a.session ^= 0x77, b.session ^= 0x77, c.session ^= 0x77;
        now = run(&net, (net_roster(&net, code, now), now), 6000);
        b.f[110].value = 1;
        now = run(&net, now, 3000);
        coop_sync_offer(a.eng, &o, now);
        CHECK(took && o.accepted && coop_sync_open(a.eng) && value_of(&a, 111) == 1,
              "H7 F3: a relay restart that hands B and C each other's numbers is no new offer (accepted %d, open %d), and B's flag arrives (%d)",
              o.accepted, coop_sync_open(a.eng), value_of(&a, 111));
        game_free(&a), game_free(&b), game_free(&c);
        free(net.q);
    }
    printf("coop-probe: sync moved key: a key the player accepted under another member's name is a new offer, and a key that changes is said\n");
}

int probe_coop_sync(void) {
    fails = 0;
    if (getenv("NX2NAVI_COOP_ADVERSARY")) {
        probe_adversary();
        return fails;
    }
    probe_algebra();
    probe_rooms(0, 2, 1);
    probe_rooms(30, 4, 1);
    probe_rooms(30, 4, 0);
    probe_rules();
    probe_keys();
    probe_carry_over();
    probe_hostile();
    probe_pool_hostile();
    probe_identities();
    probe_offer_number();
    probe_rules_wait();
    probe_save_switch();
    probe_maker_pinned();
    probe_honest_base();
    probe_fair_answers();
    probe_sync_fuzz();
    probe_h3_accepted_only();
    probe_h3_drain();
    probe_h3_replay();
    probe_h3_mark();
    probe_h3_signatures();
    probe_h3_own_records();
    probe_h3_removals();
    probe_h4_negative_base();
    probe_h4_own_acceptance();
    probe_h4_second_reset();
    probe_h4_forged_roster();
    probe_h4_spent();
    probe_h4_ended_per_key();
    probe_h5_forged_sender();
    probe_h5_session_end();
    probe_h5_join();
    probe_h5_rebind();
    probe_h5_claim_at_title();
    probe_h6_digests();
    probe_h6_identity_race();
    probe_h6_carry();
    probe_h7_moved_key();
    probe_adversary();
    return fails;
}
