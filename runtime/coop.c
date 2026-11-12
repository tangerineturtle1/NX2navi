// Co-op session layer (coop.h): the state machine of one player's connection to a relay, and the thread that runs it.
//
// States: OFF (the COOP setting is off), LEFT (on, in no room), CONNECTING (reaching the relay to create or join a room),
// JOINED, RECONNECTING (the relay went silent; joining the same room again as the same member).
//
// A room is its secret (coop_proto.h): the relay knows it by the locator derived from it, and the players seal their
// messages with the message key derived from it, so the relay forwards what it cannot read, change or make.
//
// Reaching the relay is a cookie exchange (tools/coop_relay.py): a CONNECT is answered by a CHALLENGE with a cookie, a
// CONNECT that carries the cookie is answered by JOINED or an ERROR. Every answer carries the nonce of the attempt and
// the locator's tag, so a datagram someone else sends from the relay's address does not end or redirect the attempt.
// A burst of four CONNECTs 250 ms apart carries the exchange over a lossy path; a burst without an answer is followed by
// a wait of 1, 2, 4, 8, then 15 s. A session that keeps its room (retry) asks for a room that is not there at most every
// 10 s, and waits a minute when the relay refuses it for its network's joins, instead of ending. In a room a PING every
// 2 s keeps the NAT mapping open and measures the round trip, and 8 s without any datagram from the relay is
// RECONNECTING. JOINED hands out a rejoin token, which a later CONNECT of the same member carries: the relay gives a
// member's id back only to it.
//
// Every join is a new session: a random number the CONNECT tells the relay, which lists it in the roster. A player's
// messages carry its session and a counter from 1 in the sealed envelope; the others take a message only of the session
// the roster names for its sender and once within a window of 64 counters, so a message recorded before a rejoin, or
// sent again within the window, is refused. Four older ones in a row start the window again (window_below), after which
// a recorded message of that session can come again: a layer that must take a message once checks a counter of its own.
//
// One thread owns the socket, the DNS lookup and the state. Other threads reach it through a mailbox (commands, settings), an
// outbox (messages to send) and a copy of the state they may read; all three sit behind one lock that the thread holds only
// to copy. The thread never holds a virtual core (kernel.c: host work runs without one), as discord_rpc.c's.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop.h"
#include "coop_app.h"
#include "host.h"
#include "platform/platform.h"

enum {
    PING_MS = 2000, SILENCE_MS = 8000, BURST_GAP_MS = 250, BACKOFF_FIRST_MS = 1000, BACKOFF_MAX_MS = 15000,
    BURST_TRIES = 4,                 // CONNECTs per burst
    NO_ROOM_WAIT_MS = 10000,         // a room that is not there is asked for this often at most (each asks the relay for a join)
    RATE_WAIT_MS = 60000,            // the wait after the relay refused a join for the network's budget
    ROSTER_AHEAD = 1 << 20,          // a roster's number is taken at most this far ahead of the last one's
    ROSTER_STALE_MS = 4000,          // after the silence time and this long without a roster taken, any roster is taken
    STATS_MS = 60000,                // the periodic counters line, while in a room
    LOG_EVERY_MS = 30000,            // a repeated cause logs this often
    RENAME_MS = 1000,                // a name change joins the room again after this much quiet
    OUT_SLOTS = 64,                  // messages waiting to be sent
    DEFAULT_PORT = 24680,
    VIEW_HOOKS = 8,
    TICK_HOOKS = 4,
};

typedef struct { uint8_t dst, kind; uint16_t len; uint8_t data[COOP_MAX_MESSAGE]; } OutMsg;
typedef struct { CoopMsgFn fn; void *user; } MsgHandler;
typedef struct { CoopViewFn fn; void *user; } ViewHook;
typedef struct { CoopTickFn fn; void *user; } TickHook;

// A cause that logs at most once per LOG_EVERY_MS.
enum { L_MALFORMED, L_SENDER, L_TAG, L_REPLAY, L_OUTBOX, L_NO_SERVER, L_RESOLVE, L_SOCKET, L_CAUSES };

// The counters of one sender's session seen last: a counter at most 64 below the highest is taken once, an older one
// is not, unless the sender's own messages keep arriving below the window (window_below). A new session (the roster
// names it) starts a new window. low and low_n: the last counter that opened below it, and how many did in a row.
typedef struct { uint64_t session; uint32_t high, low; uint64_t bits; int valid, low_n; } Window;
enum { WINDOW_AGAIN = 4 };

struct CoopSess {
    // ---- settings, set by coop_sess_new and coop_sess_configure
    char tag[16];
    uint32_t ping_ms, silence_ms, burst_gap_ms, backoff_first_ms, backoff_max_ms;
    // what the stepping thread works with now
    char server[160], name[COOP_NAME_MAX + 1], host[128];
    uint16_t port, local_port;
    uint64_t client_id;
    unsigned drop_pct;
    int retry;

    // ---- shared, behind lock
    PlatMutex lock;
    PlatEvent off_done;
    int cmd;                                    // a command waiting (COOP_CMD_*), 0 none; a later one replaces it
    char cmd_secret[COOP_SECRET_LEN + 1];
    int cmd_remove;                             // a member the maker removes, 0 none, and its session in the view then
    uint64_t cmd_remove_session;
    int cfg_pending;                            // pending holds settings the thread has not applied
    struct { char server[160], name[COOP_NAME_MAX + 1]; uint64_t client_id; uint16_t local_port; unsigned drop_pct; int retry, on; } pending;
    OutMsg *out;
    unsigned out_head, out_tail;
    CoopView view;
    uint64_t heard_at[COOP_MAX_MEMBERS + 1];    // last message of each member that opened, by id; 0 never
    MsgHandler msg[256];
    ViewHook hooks[VIEW_HOOKS];
    int n_hooks;
    TickHook ticks[TICK_HOOKS];
    int n_ticks;
    int pub_state;                              // state, readable without the lock
    int pub_joined;

    // ---- the stepping thread's
    CoopState state;
    PlatUdp *sock;
    PlatAddr relay;
    int relay_known;
    char relay_text[64];                        // the relay's resolved address, for the trace
    int intent, made_room;                      // CONNECT's intent; this session's own CREATE made the room
    uint64_t room_session;                      // the session the room lists for this player since its last JOINED
    char want[COOP_SECRET_LEN + 1];             // the secret of the room being joined or made
    uint8_t locator[COOP_KEY_LEN];              // of want: the relay's name of the room and the tags' key
    uint8_t msg_key[COOP_MSG_KEY_LEN];          // of want: seals the players' messages
    uint64_t session, nonce;                    // this join's session (in the roster); this attempt's nonce
    uint8_t cookie[COOP_COOKIE_LEN], token[COOP_COOKIE_LEN];
    int have_cookie, have_token, said_answer, heard_relay;
    uint32_t connect_seq, out_seq, out_counter;
    uint64_t next_tx, last_connect_tx, last_rx, next_ping, next_stats, rename_at;
    uint32_t ping_salt;   // added to the clock in a PING's token, new for every episode: the token alone would tell
                          // the relay how long this PC has been up
    uint64_t roster_at, roster_rejoin_at;       // when a roster was last taken; the last rejoin a roster asked for
    int own_taken;                              // the roster taken last lists another session for this player
    uint32_t backoff;
    int burst_left;
    uint32_t room;
    int self;
    char secret[COOP_SECRET_LEN + 1];           // of the room we are in
    CoopRoster roster;
    uint32_t roster_seq;
    Window rw[COOP_MAX_MEMBERS + 1];
    char status[96];
    int dirty;
    int off_applied;                            // a stop was applied: off_done is set once the view saying so is published
    uint64_t rng;
    uint64_t log_at[L_CAUSES];
    uint32_t n_tx, n_rx, n_lost, n_malformed, n_bad_tag, n_sender, n_dup, n_test_drop, n_outbox_full;
    uint32_t stats_seen;
    uint8_t rx[2048];
};

// ---- logging

#define TRACE(s, ...) do { if (rt_trace & RT_TRACE_COOP) say(s, __VA_ARGS__); } while (0)
static void say(const CoopSess *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void say(const CoopSess *s, const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (s->tag[0]) rt_log("coop[%s]: %s", s->tag, msg);
    else rt_log("coop: %s", msg);
}

// 1 when cause may log now: the first time, then once per LOG_EVERY_MS (the periodic counters line has the totals).
static int log_ready(CoopSess *s, int cause, uint64_t now) {
    if (s->log_at[cause] && now - s->log_at[cause] < LOG_EVERY_MS) return 0;
    s->log_at[cause] = now ? now : 1;
    return 1;
}
#define SAY_LIMITED(s, cause, now, ...) do { if (log_ready(s, cause, now)) say(s, __VA_ARGS__); } while (0)

static const char *type_name(unsigned t) {
    switch (t) {
    case COOP_T_CONNECT: return "CONNECT";
    case COOP_T_CHALLENGE: return "CHALLENGE";
    case COOP_T_JOINED: return "JOINED";
    case COOP_T_ERROR: return "ERROR";
    case COOP_T_ROSTER: return "ROSTER";
    case COOP_T_PING: return "PING";
    case COOP_T_PONG: return "PONG";
    case COOP_T_LEAVE: return "LEAVE";
    case COOP_T_KICK: return "KICK";
    case COOP_T_DATA: return "DATA";
    default: return "?";
    }
}

// ---- small things

static uint32_t rnd(CoopSess *s) {   // xorshift64*: for the test drop and nothing else
    s->rng ^= s->rng >> 12, s->rng ^= s->rng << 25, s->rng ^= s->rng >> 27;
    return (uint32_t)((s->rng * 0x2545F4914F6CDD1Dull) >> 32);
}

// 64 random bits from the system's source (sessions, nonces), never 0; 0 when the source failed: there is no fallback,
// since a session or nonce someone could guess would let them answer for the relay or take this player's place.
static uint64_t random64(void) {
    uint64_t v = 0;
    if (!plat_random(&v, sizeof v)) return 0;
    return v ? v : 1;
}

static void copy_str(char *dst, size_t cap, const char *src) {
    snprintf(dst, cap, "%s", src ? src : "");
}

// "XXX…" for a secret in the log (coop_secret_short: a label hashed from it): no part of the secret goes there.
static const char *label_of(const char *secret, char out[8]) {
    if (secret[0]) coop_secret_short(secret, out);
    else copy_str(out, 8, "");
    return out;
}

static void set_status(CoopSess *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void set_status(CoopSess *s, const char *fmt, ...) {
    char text[sizeof s->status];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (strcmp(text, s->status)) memcpy(s->status, text, sizeof text), s->dirty = 1;
}

static void set_state(CoopSess *s, CoopState st) {
    if (s->state != st) s->dirty = 1;
    s->state = st;
    __atomic_store_n(&s->pub_state, (int)st, __ATOMIC_RELEASE);
    __atomic_store_n(&s->pub_joined, st == COOP_JOINED, __ATOMIC_RELEASE);
}

int coop_server_parse(const char *text, char *host, size_t cap, uint16_t *port) {
    while (*text == ' ') text++;
    size_t n = strlen(text);
    while (n && text[n - 1] == ' ') n--;
    if (!n) return 0;
    const char *h = text, *colon = NULL;
    size_t hn = n;
    if (*text == '[') {   // [v6] or [v6]:port
        const char *close = memchr(text, ']', n);
        if (!close) return 0;
        h = text + 1, hn = (size_t)(close - h);
        if (close + 1 < text + n) {
            if (close[1] != ':') return 0;
            colon = close + 1;
        }
    } else {
        size_t colons = 0;
        for (size_t i = 0; i < n; i++)
            if (text[i] == ':') colons++, colon = text + i;
        if (colons > 1) colon = NULL;   // a bare IPv6 address
        else if (colon) hn = (size_t)(colon - text);
    }
    uint32_t p = DEFAULT_PORT;
    if (colon) {
        const char *d = colon + 1;
        size_t dn = (size_t)(text + n - d);
        if (!dn || dn > 5) return 0;
        p = 0;
        for (size_t i = 0; i < dn; i++) {
            if (d[i] < '0' || d[i] > '9') return 0;
            p = p * 10 + (uint32_t)(d[i] - '0');
        }
        if (!p || p > 65535) return 0;
    }
    if (!hn || hn >= cap) return 0;
    memcpy(host, h, hn);
    host[hn] = 0;
    *port = (uint16_t)p;
    return 1;
}

// ---- sending

// One datagram to the relay, tagged with the locator.
static void send_dgram(CoopSess *s, uint8_t type, uint32_t room, uint8_t src, uint8_t dst, uint32_t seq,
                       const void *payload, size_t n) {
    uint8_t d[COOP_MAX_DGRAM];
    CoopHdr h = {.ver = COOP_PROTO, .type = type, .src = src, .dst = dst, .room = room, .seq = seq};
    size_t k = coop_dgram_pack(d, sizeof d, &h, payload, n, s->locator);
    if (!k || !s->sock || !s->relay_known) return;
    plat_udp_send(s->sock, &s->relay, d, k);
    s->n_tx++;
    TRACE(s, "tx %s to %u, %zu bytes", type_name(type), dst, k);
}

// A datagram of the room we are in.
static void send_room(CoopSess *s, uint8_t type, uint8_t dst, const void *payload, size_t n) {
    send_dgram(s, type, s->room, (uint8_t)s->self, dst, s->out_seq++, payload, n);
}

// A player message, sealed with the room's message key under this session and the next counter.
static void send_sealed(CoopSess *s, uint8_t dst, uint8_t kind, const void *bytes, size_t n) {
    uint8_t payload[COOP_MAX_PAYLOAD];
    size_t k = coop_seal(payload, sizeof payload, s->msg_key, (uint8_t)s->self, dst, s->session, ++s->out_counter, kind, bytes, n);
    if (k) send_room(s, COOP_T_DATA, dst, payload, k);
}

static void send_connect(CoopSess *s, uint64_t now) {
    CoopConnect c = {.intent = (uint8_t)s->intent, .client_id = s->client_id, .session = s->session, .nonce = s->nonce};
    uint8_t payload[128];
    memcpy(c.locator, s->locator, COOP_KEY_LEN);
    copy_str(c.name, sizeof c.name, s->name);
    if (s->have_cookie) memcpy(c.cookie, s->cookie, COOP_COOKIE_LEN);
    if (s->have_token) memcpy(c.token, s->token, COOP_COOKIE_LEN);
    size_t n = coop_connect_pack(payload, sizeof payload, &c);
    send_dgram(s, COOP_T_CONNECT, 0, 0, 0, s->connect_seq, payload, n);
    if (!s->have_cookie) s->last_connect_tx = now;
}

// ---- leaving

static void close_socket(CoopSess *s) {
    if (s->sock) plat_udp_close(s->sock);
    s->sock = NULL;
}

// Tells the others and the relay, twice (a datagram may be lost): BYE to every member, then LEAVE.
static void say_goodbye(CoopSess *s) {
    if (!s->sock || !s->relay_known || !s->room) return;
    for (int i = 0; i < 2; i++) {
        if (s->state == COOP_JOINED) send_sealed(s, COOP_DST_ALL, COOP_K_BYE, NULL, 0);
        send_room(s, COOP_T_LEAVE, 0, NULL, 0);
    }
}

// Out of the room (or never in): closes the socket and shows why.
static void go_left(CoopSess *s, const char *why, int log_it) {
    int was_in = s->state == COOP_JOINED || s->state == COOP_RECONNECTING;
    if (log_it) {
        if (was_in) say(s, "left room (%s)", why);
        else say(s, "no room: %s", why);
    }
    close_socket(s);
    s->room = 0, s->self = 0, s->made_room = 0;
    s->roster.n = 0;
    s->have_token = 0;
    memset(s->rw, 0, sizeof s->rw);
    set_state(s, COOP_LEFT);
    set_status(s, "%s", why);
    s->dirty = 1;
}

// ---- reaching the relay

// A socket and the relay's address, when there are none yet. 0 and a status when they cannot be had.
static int ensure_socket(CoopSess *s, uint64_t now) {
    if (!s->sock) {
        s->sock = plat_udp_open(s->local_port, 1);
        if (!s->sock) {
            set_status(s, "cannot open a UDP socket");
            SAY_LIMITED(s, L_SOCKET, now, "cannot open a UDP socket");
            return 0;
        }
    }
    if (!s->relay_known) {
        say(s, "connecting to the relay");
        TRACE(s, "the relay is %s", s->server);
        if (!plat_net_resolve(s->host, s->port, &s->relay)) {
            set_status(s, "cannot find the relay's address");
            SAY_LIMITED(s, L_RESOLVE, now, "cannot resolve the relay's name");
            return 0;
        }
        plat_addr_str(&s->relay, s->relay_text, sizeof s->relay_text);
        s->relay_known = 1;
    }
    return 1;
}

static void schedule_backoff(CoopSess *s, uint64_t now) {
    s->next_tx = now + s->backoff;
    if (!s->heard_relay && s->sock && s->relay_known) set_status(s, "no answer from the relay; trying again in %u s", (s->backoff + 500) / 1000);
    s->backoff = s->backoff * 2 > s->backoff_max_ms ? s->backoff_max_ms : s->backoff * 2;
}

// A new attempt to get into a room: intent, the room's secret, and whether it is a rejoin of the room we were in (which
// keeps the rejoin token). Every attempt is a new session and a new nonce; 0 (and no room) when the system's random
// source cannot give them.
static int begin_episode(CoopSess *s, int intent, const char *secret, int rejoin, uint64_t now) {
    uint64_t session = random64(), nonce = random64();
    if (!session || !nonce) {
        go_left(s, "the system's random source failed: co-op needs it", 1);
        return 0;
    }
    s->intent = intent;
    if (secret != s->want) copy_str(s->want, sizeof s->want, secret);   // a rejoin passes the secret it already holds
    coop_room_keys(s->want, s->locator, s->msg_key);
    s->session = session, s->nonce = nonce, s->out_counter = 0;
    s->ping_salt = (uint32_t)(nonce >> 32) ^ (uint32_t)session;
    s->connect_seq = s->out_seq++;
    s->have_cookie = s->said_answer = s->heard_relay = 0;
    if (!rejoin) s->have_token = 0;
    s->burst_left = 0;
    s->next_tx = now;
    s->backoff = s->backoff_first_ms;
    s->relay_known = 0;   // looked up again: the name may point elsewhere by now
    if (!rejoin) {
        s->room = 0, s->self = 0, s->made_room = 0;
        s->roster.n = 0;
    }
    set_state(s, rejoin ? COOP_RECONNECTING : COOP_CONNECTING);
    if (rejoin) set_status(s, "reconnecting to the relay");
    else set_status(s, "%s", intent == COOP_INTENT_CREATE ? "creating a room" : "joining the room");
    s->dirty = 1;
    return 1;
}

static void connect_timers(CoopSess *s, uint64_t now) {
    if (now < s->next_tx) return;
    if (s->burst_left == 0) {   // a new burst: the socket and the relay's address first
        if (!ensure_socket(s, now)) {
            s->relay_known = 0;
            schedule_backoff(s, now);
            return;
        }
        s->burst_left = BURST_TRIES;
    }
    send_connect(s, now);
    if (--s->burst_left) s->next_tx = now + s->burst_gap_ms;
    else schedule_backoff(s, now);
}

// ---- the roster

// 1 when every id of r is 1..8 and none repeats: what the rest of the code indexes by.
static int roster_valid(const CoopRoster *r) {
    unsigned seen = 0;
    for (int i = 0; i < r->n; i++) {
        unsigned id = r->m[i].id;
        if (id < 1 || id > COOP_MAX_MEMBERS || (seen & 1u << id)) return 0;
        seen |= 1u << id;
    }
    return 1;
}

static const CoopMemberInfo *member_of(const CoopSess *s, int id) {
    for (int i = 0; i < s->roster.n; i++)
        if (s->roster.m[i].id == id) return &s->roster.m[i];
    return NULL;
}

// The roster the relay sent replaces ours; the log tells who came, went, changed name or went away. 1 when this
// player's own line lists another session than its own: someone took its place at the relay (a CONNECT with its
// token, seen on the way), and the session joins again to take it back (roster_rejoin).
static int apply_roster(CoopSess *s, const CoopRoster *r) {
    int taken = 0;
    for (int i = 0; i < r->n; i++) {
        const CoopMemberInfo *m = &r->m[i], *old = member_of(s, m->id);
        if (m->id == s->self) {   // the relay's maker mark is not taken for this session's own creation (on_joined)
            taken = m->session != s->session;
            continue;
        }
        if (!old) say(s, "%s (member %u) joined the room", m->name, m->id);
        else if (strcmp(old->name, m->name)) say(s, "member %u is now %s", m->id, m->name);
        else if ((old->flags ^ m->flags) & COOP_FLAG_AWAY)
            say(s, "%s (member %u) %s", m->name, m->id, m->flags & COOP_FLAG_AWAY ? "is away" : "is back");
        // a new member, or a new session of it: nothing heard from it yet. Its replay window stays: a window belongs to
        // one session (window_check takes another session as new), so the same session, listed again after this player
        // joined again, keeps refusing what it already delivered
        if (!old || old->session != m->session) {
            plat_mutex_lock(&s->lock);
            s->heard_at[m->id] = 0;
            plat_mutex_unlock(&s->lock);
        }
    }
    for (int j = 0; j < s->roster.n; j++) {
        int id = s->roster.m[j].id, found = 0;
        for (int i = 0; i < r->n; i++) found |= r->m[i].id == id;
        if (found) continue;
        say(s, "%s (member %u) left the room", s->roster.m[j].name, id);
        plat_mutex_lock(&s->lock);
        s->heard_at[id] = 0;
        plat_mutex_unlock(&s->lock);
    }
    s->roster = *r;
    s->dirty = 1;
    return taken;
}

// ---- receiving

// 1 when counter of session is new in the window, 0 when it was taken before, -1 when it is too old to tell (64 or more
// below the highest); nothing changes (commit after the message opened).
static int window_check(const Window *w, uint64_t session, uint32_t counter) {
    if (!w->valid || w->session != session) return 1;
    int32_t d = (int32_t)(counter - w->high);
    if (d > 0) return 1;
    if (d <= -64) return -1;
    return !(w->bits >> (unsigned)-d & 1);
}

static void window_commit(Window *w, uint64_t session, uint32_t counter) {
    if (!w->valid || w->session != session) {
        *w = (Window){.session = session, .high = counter, .bits = 1, .valid = 1};
        return;
    }
    int32_t d = (int32_t)(counter - w->high);
    if (d > 0) {
        w->bits = d >= 64 ? 1 : w->bits << d | 1;
        w->high = counter;
        w->low_n = 0;
    } else {
        w->bits |= 1ull << (unsigned)-d;
    }
}

// A message that opened with the room's message key but is too old for the window. That key is the whole room's, so
// whoever holds it can seal one message in a member's name with a counter far ahead and leave every later message of
// that member below the window for the rest of its session. The sender's own next messages then keep arriving below
// it, each later than the one before: at the WINDOW_AGAIN-th in a row the window starts again from it, which takes this
// one. One that is not later than the last starts the row again (a copy of one counts once, and one sealed above the
// sender's own cannot hold the row there). The window forgets what it took: recorded messages of the session above the
// new start, and its last 64, can then be taken again (coop.h says what a handler does about it). 1 when the window
// started again.
static int window_below(Window *w, uint32_t counter) {
    if (w->low_n && (int32_t)(counter - w->low) <= 0) w->low_n = 0;
    w->low = counter;
    if (++w->low_n < WINDOW_AGAIN) return 0;
    *w = (Window){.session = w->session, .high = counter, .bits = 1, .valid = 1};
    return 1;
}

// The answers to a CONNECT: the attempt's nonce and the locator's tag.
static int answer_ok(CoopSess *s, const uint8_t *d, size_t n, uint64_t nonce) {
    if (nonce != s->nonce || !coop_dgram_verify(d, n, s->locator)) {
        s->n_bad_tag++, s->n_lost++;
        return 0;
    }
    return 1;
}

// The on_* handlers return 1 for a datagram that passed its checks and made sense: only those show the relay is there.
static int on_challenge(CoopSess *s, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, uint64_t now) {
    uint64_t nonce;
    uint8_t cookie[COOP_COOKIE_LEN];
    if ((s->state != COOP_CONNECTING && s->state != COOP_RECONNECTING) || !coop_challenge_parse(p, h->len, &nonce, cookie)) return 0;
    if (!answer_ok(s, d, n, nonce)) return 0;
    memcpy(s->cookie, cookie, COOP_COOKIE_LEN);
    s->have_cookie = 1;
    s->heard_relay = 1;
    if (!s->said_answer) {
        s->said_answer = 1;
        say(s, "the relay answered in %u ms", (unsigned)(now - s->last_connect_tx));
        TRACE(s, "the relay is at %s", s->relay_text);
    }
    send_connect(s, now);   // the CONNECT with the cookie, at once
    return 1;
}

static int on_joined(CoopSess *s, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, uint64_t now) {
    if (s->state != COOP_CONNECTING && s->state != COOP_RECONNECTING) return 0;
    CoopJoined j;
    if (!coop_joined_parse(p, h->len, &j) || !h->room || j.member < 1 || j.member > COOP_MAX_MEMBERS || !roster_valid(&j.roster)) {
        s->n_malformed++, s->n_lost++;
        return 0;
    }
    if (!answer_ok(s, d, n, j.nonce)) return 0;   // the room asked for (our locator), this attempt (our nonce)
    int rejoined = s->state == COOP_RECONNECTING;
    s->room = h->room;
    s->self = j.member;
    memcpy(s->secret, s->want, sizeof s->secret);
    memcpy(s->token, j.token, COOP_COOKIE_LEN);
    s->have_token = 1;
    if (!rejoined) s->made_room = s->intent == COOP_INTENT_CREATE;   // this session's own creation, not the relay's mark
    s->room_session = s->session;
    apply_roster(s, &j.roster);   // after a rejoin: against the roster before it, so a member whose session is the same stays as it was
    s->roster_seq = h->seq;
    s->roster_at = now, s->own_taken = 0;
    s->last_rx = now;
    s->next_ping = now + s->ping_ms;
    s->next_stats = now + STATS_MS;
    char label[8];
    label_of(s->secret, label);
    if (rejoined) say(s, "rejoined room %s as member %u", label, s->self);
    else if (s->intent == COOP_INTENT_CREATE) say(s, "room %s created (member %u)", label, s->self);
    else say(s, "joined room %s as member %u", label, s->self);
    set_state(s, COOP_JOINED);
    set_status(s, "%s", "");
    s->dirty = 1;
    return 1;
}

static void refused(CoopSess *s, uint8_t reason, uint64_t now) {
    char label[8];
    label_of(s->want, label);
    switch (reason) {
    case COOP_E_NO_ROOM:
        if (s->state == COOP_RECONNECTING) {   // the relay lost the room: a restart
            say(s, "room %s is gone from the relay", label);
            if (!s->retry) go_left(s, "the room is closed", 0);
            else if (s->made_room) {
                say(s, "creating room %s again", label);
                if (begin_episode(s, COOP_INTENT_CREATE, s->want, 0, now)) set_status(s, "the room was lost; creating it again");
            } else if (begin_episode(s, COOP_INTENT_JOIN, s->want, 0, now)) {
                set_status(s, "the room was lost; waiting for it");
            }
        } else if (s->retry) {   // not made yet, perhaps: wait and ask again
            s->heard_relay = 1;
            set_status(s, "room %s not found; waiting for it", label);
            if (s->burst_left || now >= s->next_tx) {   // the first answer of a burst ends it; a CONNECT sent twice is answered twice
                s->burst_left = 0;
                schedule_backoff(s, now);
                // each question asks the relay for one of the network's joins: at most one every NO_ROOM_WAIT_MS, so
                // that players behind one address who wait for a room leave joins for the others (the same attempt
                // asked again costs nothing at the relay)
                if (s->next_tx < now + NO_ROOM_WAIT_MS) s->next_tx = now + NO_ROOM_WAIT_MS;
            }
        } else {
            go_left(s, "no such room", 1);
        }
        break;
    case COOP_E_ROOM_FULL: go_left(s, "the room is full", 1); break;
    case COOP_E_PROTO: go_left(s, "update required: the relay speaks another protocol version", 1); break;
    case COOP_E_RATE:
        if (s->retry) {   // a session that keeps its room waits until the network's joins are back, and stays
            if (s->next_tx < now + RATE_WAIT_MS) {
                say(s, "the relay refused: too many joins from this network; trying again in a minute");
                s->next_tx = now + RATE_WAIT_MS;
            }
            s->heard_relay = 1, s->burst_left = 0;
            set_status(s, "too many joins from this network; trying again in a minute");
        } else {
            go_left(s, "too many joins from this network; try again in a minute", 1);
        }
        break;
    case COOP_E_ROOM_EXISTS:
        if (s->retry && s->intent == COOP_INTENT_CREATE) {
            // a session that keeps its room (coop_auto): someone made the room again first after the relay lost it, or
            // it is open already. Joining it keeps the player in the room they meant; the rules stay the ones the
            // players accepted, since a game takes them only signed by the maker it pinned (coop_state.h)
            say(s, "room %s is open already; joining it", label);
            if (begin_episode(s, COOP_INTENT_JOIN, s->want, 0, now)) set_status(s, "the room is open already; joining it");
        } else {
            go_left(s, "a room with this secret is already open on the relay: join it", 1);
        }
        break;
    case COOP_E_REMOVED: go_left(s, "the room's maker removed this player", 1); break;
    case COOP_E_SESSION_USED:
        if (s->retry) {   // someone sent this attempt's CONNECT first (seen on the way): a new attempt, a new session
            say(s, "the relay saw this session in the room before (a replay on the way?); trying again with a new one");
            int rejoin = s->state == COOP_RECONNECTING;
            if (begin_episode(s, s->intent, s->want, rejoin, now)) s->next_tx = now + NO_ROOM_WAIT_MS;
        } else {
            go_left(s, "the relay saw this session in the room before (a replay on the way?)", 1);
        }
        break;
    default: go_left(s, "the relay refused", 1); break;
    }
}

static int on_error(CoopSess *s, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, uint64_t now) {
    int connecting = s->state == COOP_CONNECTING || s->state == COOP_RECONNECTING;
    CoopError e;
    if (!coop_error_parse(p, h->len, &e)) {
        s->n_malformed++, s->n_lost++;
        return 0;
    }
    // an answer to this attempt; in a room, only a removal (nonce 0, the room's tag). A removal in the room never
    // arrives (it carries nonce 0, never this attempt's), and must not be taken by accepting nonce 0: anyone who saw a
    // CONNECT can tag a datagram with its locator. The maker's removal shows as the member leaving the roster instead.
    if (connecting ? !answer_ok(s, d, n, e.nonce) : s->state != COOP_JOINED || e.reason != COOP_E_REMOVED || h->room != s->room ||
                                                        !answer_ok(s, d, n, 0))
        return 0;
    refused(s, e.reason, now);
    return 1;
}

// This player's own line lists another session: join again to take the place back, at most once per backoff_max_ms,
// so that rosters from someone on the path cannot use up the network's joins; a later one waits for its turn (timers).
static void roster_rejoin(CoopSess *s, uint64_t now) {
    if (s->roster_rejoin_at && now - s->roster_rejoin_at < s->backoff_max_ms) return;
    s->roster_rejoin_at = now ? now : 1;
    say(s, "the relay lists another session for this player; joining the room again");
    begin_episode(s, COOP_INTENT_JOIN, s->want, 1, now);
}

static int on_roster(CoopSess *s, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, uint64_t now) {
    CoopRoster r;
    if (s->state != COOP_JOINED || h->room != s->room) return 0;
    if (!coop_dgram_verify(d, n, s->locator)) {
        s->n_bad_tag++, s->n_lost++;
        return 0;
    }
    if (!coop_roster_parse(p, h->len, &r, NULL) || !roster_valid(&r)) {
        s->n_malformed++, s->n_lost++;
        return 0;
    }
    // a later roster, up to ROSTER_AHEAD past the last one: anyone who saw a CONNECT can tag one, and a number far
    // ahead would make every roster of the relay after it look old. An older one arriving late is dropped, unless no
    // roster was taken for a while (the relay sends one every few seconds): then the last one's number was not the
    // relay's
    int32_t ahead = (int32_t)(h->seq - s->roster_seq);
    int stale = now - s->roster_at >= (uint64_t)s->silence_ms + ROSTER_STALE_MS;
    if ((ahead > 0 && ahead <= ROSTER_AHEAD) || (stale && ahead)) {
        s->roster_seq = h->seq, s->roster_at = now;
        s->own_taken = apply_roster(s, &r);
        if (s->own_taken) roster_rejoin(s, now);
    }
    return 1;
}

static int on_pong(CoopSess *s, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, uint64_t now) {
    uint32_t token;
    if (s->state != COOP_JOINED || h->room != s->room || !coop_u32_parse(p, h->len, &token)) return 0;
    if (!coop_dgram_verify(d, n, s->locator)) {
        s->n_bad_tag++, s->n_lost++;
        return 0;
    }
    uint32_t rtt = (uint32_t)now + s->ping_salt - token;
    if (rtt <= 60000) {   // a token of ours
        plat_mutex_lock(&s->lock);
        s->view.ping_ms = rtt ? rtt : 1;
        plat_mutex_unlock(&s->lock);
    }
    return 1;
}

static int on_data(CoopSess *s, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, uint64_t now) {
    (void)d, (void)n;
    if (s->state != COOP_JOINED || h->room != s->room) return 0;
    const CoopMemberInfo *m = h->src != s->self ? member_of(s, h->src) : NULL;
    uint64_t session;
    uint32_t counter;
    if (!m || (h->dst != COOP_DST_ALL && h->dst != s->self)) {
        s->n_sender++, s->n_lost++;
        SAY_LIMITED(s, L_SENDER, now, "a message from member %u to %u was thrown away: not a member the roster names", h->src, h->dst);
        return 0;
    }
    if (!coop_envelope_peek(p, h->len, &session, &counter)) {
        s->n_malformed++, s->n_lost++;
        return 0;
    }
    int fresh = session == m->session ? window_check(&s->rw[h->src], session, counter) : 0;
    if (!fresh) {   // an earlier session's, or seen
        s->n_dup++, s->n_lost++;
        SAY_LIMITED(s, L_REPLAY, now, "a message of member %u that is not of its present session, or seen before, was thrown away", h->src);
        return 0;
    }
    uint8_t kind, bytes[COOP_MAX_PAYLOAD];
    size_t nb;
    if (!coop_open(p, h->len, s->msg_key, h->src, h->dst, &kind, bytes, sizeof bytes, &nb)) {
        s->n_bad_tag++, s->n_lost++;
        SAY_LIMITED(s, L_TAG, now, "a message that does not open with the room's key was thrown away");
        return 0;
    }
    if (fresh < 0 && !window_below(&s->rw[h->src], counter)) {
        s->n_dup++, s->n_lost++;
        SAY_LIMITED(s, L_REPLAY, now, "a message of member %u older than its last 64 was thrown away", h->src);
        return 0;
    }
    if (fresh > 0) window_commit(&s->rw[h->src], session, counter);
    plat_mutex_lock(&s->lock);
    s->heard_at[h->src] = now;
    plat_mutex_unlock(&s->lock);
    TRACE(s, "rx message kind %u from member %u, %zu bytes", kind, h->src, nb);
    CoopMsgFn fn = __atomic_load_n(&s->msg[kind].fn, __ATOMIC_ACQUIRE);
    if (fn) fn(h->src, bytes, nb, s->msg[kind].user);
    return 1;
}

static void handle_datagram(CoopSess *s, size_t n, const PlatAddr *from, uint64_t now) {
    if (s->drop_pct && rnd(s) % 100 < s->drop_pct) {
        s->n_test_drop++;
        return;
    }
    CoopHdr h;
    const uint8_t *p;
    if (!coop_dgram_parse(s->rx, n, &h, &p)) {
        s->n_malformed++, s->n_lost++;
        SAY_LIMITED(s, L_MALFORMED, now, "a datagram that is not a co-op datagram was thrown away");
        return;
    }
    if (!s->relay_known || !plat_addr_eq(from, &s->relay)) {   // only the relay talks to us
        s->n_sender++, s->n_lost++;
        SAY_LIMITED(s, L_SENDER, now, "a datagram from an address that is not the relay was thrown away");
        return;
    }
    if (h.ver != COOP_PROTO) {
        // a relay of another version answers a CONNECT with an untagged ERROR it cannot word in our format: taken only
        // as the first answer of an attempt, so a stray one does not end a session
        if (h.type == COOP_T_ERROR && coop_dgram_tag_zero(s->rx) && !s->heard_relay &&
            (s->state == COOP_CONNECTING || s->state == COOP_RECONNECTING)) {
            s->n_rx++;
            refused(s, COOP_E_PROTO, now);
        }
        return;
    }
    TRACE(s, "rx %s, %zu bytes", type_name(h.type), n);
    int ok = 0;
    switch (h.type) {
    case COOP_T_CHALLENGE: ok = on_challenge(s, s->rx, n, &h, p, now); break;
    case COOP_T_JOINED: ok = on_joined(s, s->rx, n, &h, p, now); break;
    case COOP_T_ERROR: ok = on_error(s, s->rx, n, &h, p, now); break;
    case COOP_T_ROSTER: ok = on_roster(s, s->rx, n, &h, p, now); break;
    case COOP_T_PONG: ok = on_pong(s, s->rx, n, &h, p, now); break;
    case COOP_T_DATA: ok = on_data(s, s->rx, n, &h, p, now); break;
    default: break;
    }
    if (ok) {
        s->n_rx++;
        s->last_rx = now;   // a datagram the relay made: it is there
    }
}

// ---- the outbox

static void flush_outbox(CoopSess *s) {
    for (int i = 0; i < OUT_SLOTS; i++) {
        OutMsg m;
        plat_mutex_lock(&s->lock);
        if (s->out_head == s->out_tail) {
            plat_mutex_unlock(&s->lock);
            return;
        }
        OutMsg *slot = &s->out[s->out_tail];
        m.dst = slot->dst, m.kind = slot->kind, m.len = slot->len;
        memcpy(m.data, slot->data, slot->len);
        s->out_tail = (s->out_tail + 1) % OUT_SLOTS;
        plat_mutex_unlock(&s->lock);
        if (s->state == COOP_JOINED) send_sealed(s, m.dst, m.kind, m.data, m.len);   // anywhere else it is dropped
    }
}

int coop_sess_send(CoopSess *s, int member, uint8_t kind, const void *bytes, size_t n) {
    if (!__atomic_load_n(&s->pub_joined, __ATOMIC_ACQUIRE) || n > COOP_MAX_MESSAGE || member < 0 ||
        (member > COOP_MAX_MEMBERS && member != COOP_DST_ALL))
        return 0;
    plat_mutex_lock(&s->lock);
    unsigned next = (s->out_head + 1) % OUT_SLOTS;
    if (next == s->out_tail) {
        plat_mutex_unlock(&s->lock);
        __atomic_add_fetch(&s->n_outbox_full, 1, __ATOMIC_RELAXED);
        return 0;
    }
    OutMsg *slot = &s->out[s->out_head];
    slot->dst = member ? (uint8_t)member : COOP_DST_ALL;
    slot->kind = kind;
    slot->len = (uint16_t)n;
    if (n) memcpy(slot->data, bytes, n);
    s->out_head = next;
    plat_mutex_unlock(&s->lock);
    return 1;
}

// ---- commands and settings

// The settings coop_sess_configure queued, on the stepping thread.
static void apply_settings(CoopSess *s, uint64_t now) {
    plat_mutex_lock(&s->lock);
    if (!s->cfg_pending) {
        plat_mutex_unlock(&s->lock);
        return;
    }
    s->cfg_pending = 0;
    char server[sizeof s->server], name[sizeof s->name];
    copy_str(server, sizeof server, s->pending.server);
    copy_str(name, sizeof name, s->pending.name);
    s->client_id = s->pending.client_id;
    s->local_port = s->pending.local_port;
    s->drop_pct = s->pending.drop_pct;
    s->retry = s->pending.retry;
    int on = s->pending.on;
    plat_mutex_unlock(&s->lock);
    if (strcmp(server, s->server)) {
        copy_str(s->server, sizeof s->server, server);
        s->relay_known = 0;
        s->host[0] = 0;
        if (s->server[0] && !coop_server_parse(s->server, s->host, sizeof s->host, &s->port))
            say(s, "the coop_server setting is not host or host:port");
        if (s->state == COOP_JOINED || s->state == COOP_RECONNECTING) {
            say_goodbye(s);
            go_left(s, "the server changed", 1);
        } else if (s->state == COOP_CONNECTING) {
            go_left(s, "the server changed", 0);
        }
    }
    if (strcmp(name, s->name)) {
        copy_str(s->name, sizeof s->name, name);
        if (s->state == COOP_JOINED) s->rename_at = now + RENAME_MS;   // joins again once the typing has stopped
    }
    if (on && s->state == COOP_OFF) {
        set_state(s, COOP_LEFT);
        set_status(s, "%s", "");
    } else if (!on) {
        if (s->state == COOP_JOINED || s->state == COOP_RECONNECTING) {
            say_goodbye(s);
            say(s, "left room (co-op turned off)");
        }
        close_socket(s);
        s->room = 0, s->self = 0, s->made_room = 0, s->roster.n = 0;
        s->rename_at = 0;
        set_state(s, COOP_OFF);
        set_status(s, "%s", "");
        s->dirty = 1;
        s->off_applied = 1;
    }
}

static void do_command(CoopSess *s, int cmd, const char *secret, uint64_t now) {
    if (s->state == COOP_OFF) return;
    switch (cmd) {
    case COOP_CMD_LEAVE:
        if (s->state == COOP_LEFT) break;
        if (s->state == COOP_JOINED || s->state == COOP_RECONNECTING) {
            say_goodbye(s);
            say(s, "left room (leave requested)");
        } else {
            say(s, "stopped trying to get into a room");
        }
        go_left(s, "you left the room", 0);
        break;
    case COOP_CMD_CREATE:
    case COOP_CMD_JOIN:
        if (!s->host[0]) {
            SAY_LIMITED(s, L_NO_SERVER, now, "no server configured (coop_server / coop_app.h)");
            go_left(s, "no server configured", 0);
            break;
        }
        if (s->state == COOP_JOINED || s->state == COOP_RECONNECTING) say_goodbye(s);
        begin_episode(s, cmd == COOP_CMD_CREATE ? COOP_INTENT_CREATE : COOP_INTENT_JOIN, secret, 0, now);
        break;
    }
}

// The maker's KICK of a member, with the member's session so that a KICK sent again later cannot remove whoever has the
// id then: the session the published view listed when the maker removed it (progress sync removed that member's key at
// that moment), so that a member that rejoined since, or another player given the id, is not the one kicked.
static void do_remove(CoopSess *s, int member, uint64_t session) {
    const CoopMemberInfo *m = member_of(s, member);
    uint8_t payload[16];
    if (s->state != COOP_JOINED || !s->made_room || !m || member == s->self) {
        say(s, "only the room's maker can remove a member, and only one that is in the room");
        return;
    }
    size_t k = coop_kick_pack(payload, sizeof payload, (uint8_t)member, session ? session : m->session);
    for (int i = 0; i < 2; i++) send_room(s, COOP_T_KICK, 0, payload, k);
    say(s, "removing %s (member %d) from the room", m->name, member);
}

// ---- the step

static void lost_relay(CoopSess *s, uint64_t now) {
    say(s, "relay lost; rejoining");
    begin_episode(s, COOP_INTENT_JOIN, s->want, 1, now);
}

static void timers(CoopSess *s, uint64_t now) {
    switch (s->state) {
    case COOP_CONNECTING:
    case COOP_RECONNECTING:
        connect_timers(s, now);
        break;
    case COOP_JOINED:
        if (now - s->last_rx >= s->silence_ms) {
            lost_relay(s, now);
            break;
        }
        if (s->rename_at && now >= s->rename_at) {   // the name changed: the relay learns it with a new CONNECT
            s->rename_at = 0;
            say(s, "joining the room again as %s", s->name);
            begin_episode(s, COOP_INTENT_JOIN, s->want, 1, now);
            break;
        }
        if (s->own_taken && now - s->roster_rejoin_at >= s->backoff_max_ms) {   // a roster's rejoin that waited for its turn
            roster_rejoin(s, now);
            break;
        }
        if (now >= s->next_ping) {
            uint8_t token[4];
            coop_u32_pack(token, sizeof token, (uint32_t)now + s->ping_salt);
            send_room(s, COOP_T_PING, 0, token, sizeof token);
            s->next_ping = now + s->ping_ms;
        }
        if (now >= s->next_stats) {
            s->next_stats = now + STATS_MS;
            uint32_t seen = s->n_rx + s->n_tx + s->n_lost + s->n_test_drop;
            if (seen != s->stats_seen) {
                s->stats_seen = seen;
                say(s, "stats: sent %u, accepted %u, thrown away %u (tag %u, sender %u, not co-op %u, replay %u), dropped by the test %u, outbox full %u",
                    s->n_tx, s->n_rx, s->n_lost, s->n_bad_tag, s->n_sender, s->n_malformed, s->n_dup, s->n_test_drop,
                    __atomic_load_n(&s->n_outbox_full, __ATOMIC_RELAXED));
            }
        }
        break;
    default:
        break;
    }
}

static void view_rebuild(CoopSess *s) {
    plat_mutex_lock(&s->lock);
    CoopView *v = &s->view;
    int in_room = s->state == COOP_JOINED || s->state == COOP_RECONNECTING;
    const char *secret = in_room ? s->secret : s->state == COOP_CONNECTING ? s->want : "";
    v->state = s->state;
    if (secret[0]) coop_secret_format(secret, v->code);
    else v->code[0] = 0;
    label_of(secret, v->label);
    v->self = in_room ? s->self : 0;
    v->made = in_room && s->made_room;
    v->session = in_room ? s->room_session : 0;
    copy_str(v->name, sizeof v->name, s->name);
    v->maker = 0;
    v->n = in_room ? s->roster.n : 0;
    for (int i = 0; i < v->n; i++) {
        v->member[i].id = s->roster.m[i].id;
        v->member[i].away = s->roster.m[i].flags & COOP_FLAG_AWAY ? 1 : 0;
        v->member[i].maker = s->roster.m[i].flags & COOP_FLAG_MAKER ? 1 : 0;
        if (v->member[i].maker) v->maker = v->member[i].id;
        v->member[i].session = s->roster.m[i].session;
        copy_str(v->member[i].name, sizeof v->member[i].name, s->roster.m[i].name);
    }
    copy_str(v->status, sizeof v->status, s->status);
    if (s->state != COOP_JOINED) v->ping_ms = 0;
    v->version++;
    plat_mutex_unlock(&s->lock);
}

static void publish(CoopSess *s) {
    if (!s->dirty) return;
    s->dirty = 0;
    view_rebuild(s);
    CoopView copy;
    ViewHook hooks[VIEW_HOOKS];
    plat_mutex_lock(&s->lock);
    copy = s->view;
    int n = s->n_hooks;
    memcpy(hooks, s->hooks, sizeof hooks);
    plat_mutex_unlock(&s->lock);
    for (int i = 0; i < n; i++) hooks[i].fn(&copy, hooks[i].user);
    // A stop is signalled only now: whoever it wakes (coop_shutdown's caller, a page that switched co-op off) reads the
    // view that says the session is off, and every view hook has been told of it.
    if (s->off_applied) {
        s->off_applied = 0;
        plat_event_set(&s->off_done);
    }
}

void coop_sess_step(CoopSess *s) {
    uint64_t now = plat_now_ms();
    plat_mutex_lock(&s->lock);
    int cmd = s->cmd, remove = s->cmd_remove;
    uint64_t remove_session = s->cmd_remove_session;
    char secret[sizeof s->cmd_secret];
    memcpy(secret, s->cmd_secret, sizeof secret);
    s->cmd = 0, s->cmd_remove = 0;
    plat_mutex_unlock(&s->lock);
    apply_settings(s, now);
    if (cmd) do_command(s, cmd, secret, now);
    if (remove) do_remove(s, remove, remove_session);
    flush_outbox(s);
    publish(s);   // what a command or a setting changed shows before this thread waits for a datagram
    uint32_t wait = s->state == COOP_JOINED ? 8 : s->state == COOP_CONNECTING || s->state == COOP_RECONNECTING ? 50 : 250;
    if (s->sock) {
        PlatAddr from;
        int n = plat_udp_recv(s->sock, s->rx, sizeof s->rx, &from, wait);
        for (int i = 0; n > 0 && i < 64; i++) {
            handle_datagram(s, (size_t)n, &from, plat_now_ms());
            if (!s->sock) break;   // a handler ended it
            n = plat_udp_recv(s->sock, s->rx, sizeof s->rx, &from, 0);
        }
        if (n < 0 && s->sock) {
            SAY_LIMITED(s, L_SOCKET, now, "the socket failed");
            close_socket(s);
            if (s->state == COOP_JOINED) lost_relay(s, plat_now_ms());
        }
    } else {
        plat_sleep_ms(wait);
    }
    timers(s, plat_now_ms());
    publish(s);
    TickHook ticks[TICK_HOOKS];
    plat_mutex_lock(&s->lock);
    int nt = s->n_ticks;
    memcpy(ticks, s->ticks, sizeof ticks);
    plat_mutex_unlock(&s->lock);
    for (int i = 0; i < nt; i++) ticks[i].fn(plat_now_ms(), ticks[i].user);
}

// ---- the session's interface

CoopSess *coop_sess_new(const CoopParams *p) {
    CoopSess *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->out = calloc(OUT_SLOTS, sizeof *s->out);
    if (!s->out) {
        free(s);
        return NULL;
    }
    copy_str(s->tag, sizeof s->tag, p->tag);
    s->ping_ms = p->ping_ms ? p->ping_ms : PING_MS;
    s->silence_ms = p->silence_ms ? p->silence_ms : SILENCE_MS;
    s->burst_gap_ms = p->burst_gap_ms ? p->burst_gap_ms : BURST_GAP_MS;
    s->backoff_first_ms = p->backoff_first_ms ? p->backoff_first_ms : BACKOFF_FIRST_MS;
    s->backoff_max_ms = p->backoff_max_ms ? p->backoff_max_ms : BACKOFF_MAX_MS;
    plat_mutex_init(&s->lock);
    plat_event_init(&s->off_done, 1, 0);
    uint64_t seed = 0;
    if (!plat_random(&seed, sizeof seed) || !seed) seed = plat_now_ns() | 1;
    s->rng = seed;
    s->state = COOP_OFF;
    s->view.state = COOP_OFF;
    coop_sess_configure(s, p);
    apply_settings(s, plat_now_ms());   // nothing else runs yet: the first step need not wait for it
    publish(s);
    return s;
}

void coop_sess_free(CoopSess *s) {
    if (!s) return;
    close_socket(s);
    free(s->out);
    free(s);
}

void coop_sess_configure(CoopSess *s, const CoopParams *p) {
    plat_mutex_lock(&s->lock);
    copy_str(s->pending.server, sizeof s->pending.server, p->server);
    char clean[COOP_NAME_MAX + 1];
    coop_name_clean(clean, sizeof clean, (const uint8_t *)(p->name ? p->name : ""), p->name ? strlen(p->name) : 0);
    copy_str(s->pending.name, sizeof s->pending.name, clean);
    s->pending.client_id = p->client_id;
    s->pending.local_port = p->local_port;
    s->pending.drop_pct = p->drop_pct > 100 ? 100 : p->drop_pct;
    s->pending.retry = p->retry;
    s->pending.on = p->on;
    if (!p->on) plat_event_reset(&s->off_done);
    s->cfg_pending = 1;
    plat_mutex_unlock(&s->lock);
}

void coop_sess_stop(CoopSess *s) {
    plat_mutex_lock(&s->lock);
    s->pending.on = 0;
    plat_event_reset(&s->off_done);
    s->cfg_pending = 1;
    plat_mutex_unlock(&s->lock);
}

int coop_sess_command(CoopSess *s, CoopCmd cmd, const char *secret) {
    char canon[COOP_SECRET_LEN + 1] = "";
    if (cmd != COOP_CMD_CREATE && cmd != COOP_CMD_JOIN && cmd != COOP_CMD_LEAVE) return 0;
    if (cmd == COOP_CMD_JOIN || (cmd == COOP_CMD_CREATE && secret && *secret)) {
        if (!secret || !coop_secret_normalize(secret, canon)) return 0;
    } else if (cmd == COOP_CMD_CREATE) {   // a new room: a new secret, which only this player knows until it passes it on
        uint8_t r[32];
        if (!plat_random(r, sizeof r) || !coop_secret_from_random(r, canon)) return 0;
    }
    plat_mutex_lock(&s->lock);
    s->cmd = (int)cmd;
    copy_str(s->cmd_secret, sizeof s->cmd_secret, canon);
    plat_mutex_unlock(&s->lock);
    return 1;
}

int coop_sess_remove(CoopSess *s, int member) {
    if (member < 1 || member > COOP_MAX_MEMBERS || !__atomic_load_n(&s->pub_joined, __ATOMIC_ACQUIRE)) return 0;
    plat_mutex_lock(&s->lock);
    s->cmd_remove = member, s->cmd_remove_session = 0;
    for (int i = 0; i < s->view.n; i++)
        if (s->view.member[i].id == member) s->cmd_remove_session = s->view.member[i].session;
    plat_mutex_unlock(&s->lock);
    return 1;
}

int coop_sess_wait_off(CoopSess *s, uint32_t ms) { return plat_event_wait_ns(&s->off_done, (uint64_t)ms * 1000000u); }

int coop_sess_on_message(CoopSess *s, uint8_t kind, CoopMsgFn fn, void *user) {
    s->msg[kind].user = user;
    __atomic_store_n(&s->msg[kind].fn, fn, __ATOMIC_RELEASE);
    return 1;
}

int coop_sess_on_view(CoopSess *s, CoopViewFn fn, void *user) {
    plat_mutex_lock(&s->lock);
    int ok = s->n_hooks < VIEW_HOOKS;
    if (ok) s->hooks[s->n_hooks++] = (ViewHook){fn, user};
    plat_mutex_unlock(&s->lock);
    return ok;
}

int coop_sess_on_tick(CoopSess *s, CoopTickFn fn, void *user) {
    plat_mutex_lock(&s->lock);
    int ok = s->n_ticks < TICK_HOOKS;
    if (ok) s->ticks[s->n_ticks++] = (TickHook){fn, user};
    plat_mutex_unlock(&s->lock);
    return ok;
}

void coop_sess_view(CoopSess *s, CoopView *out) {
    uint64_t now = plat_now_ms();
    plat_mutex_lock(&s->lock);
    *out = s->view;
    for (int i = 0; i < out->n; i++) {
        uint64_t at = s->heard_at[out->member[i].id];
        out->member[i].heard_ms = out->member[i].id == out->self ? 0 : at ? (uint32_t)(now - at) : UINT32_MAX;
    }
    out->tx = __atomic_load_n(&s->n_tx, __ATOMIC_RELAXED);   // counted by the stepping thread without the lock
    out->rx = __atomic_load_n(&s->n_rx, __ATOMIC_RELAXED);
    out->lost = __atomic_load_n(&s->n_lost, __ATOMIC_RELAXED);
    plat_mutex_unlock(&s->lock);
}

// ---- the process's session

static CoopSess *g_sess;
static PlatOnce g_once = PLAT_ONCE_INIT;
static int g_started, g_final;

static void make_session(void *arg) {
    (void)arg;
    CoopParams p = {0};
    g_sess = coop_sess_new(&p);
    if (!g_sess) rt_log("coop: out of memory");
}

static CoopSess *process_session(void) {
    plat_once(&g_once, make_session, NULL);
    return g_sess;
}

// The player's id towards the relay: random for each start of the game, so that nothing ties one session to another,
// and never sent to the other players. The default name's digits are random too, not taken from it. 0 when the system's
// random source failed: an id others could guess would let them take this player's place, so co-op stays off.
static uint64_t g_client_id;
static unsigned g_name_digits;
static PlatOnce g_client_once = PLAT_ONCE_INIT;
static void make_client_id(void *arg) {
    (void)arg;
    uint64_t r[2] = {0, 0};
    if (!plat_random(r, sizeof r)) return;
    g_client_id = r[0] ? r[0] : 1;
    g_name_digits = (unsigned)(r[1] & 0xFFFF);
}

uint64_t coop_client_id(void) {
    plat_once(&g_client_once, make_client_id, NULL);
    return g_client_id;
}

static void coop_main(void *arg) {
    (void)arg;
    CoopSess *s = process_session();
    unsigned seen = ~0u;
    int on = 0, said_random = 0;
    uint64_t client_id = 0;
    char name[COOP_NAME_MAX + 1] = "", server[160] = "", removed[24] = "";
    copy_str(removed, sizeof removed, cfg_get("NX2NAVI_COOP_REMOVE"));   // the value at start is only noted: it acts on nothing
    for (;;) {
        unsigned v = cfg_version();
        if (v != seen && !__atomic_load_n(&g_final, __ATOMIC_ACQUIRE)) {   // the settings changed (the menu writes them)
            seen = v;
            int want = cfg_bool("NX2NAVI_COOP");
            const char *auto_mode = cfg_get("NX2NAVI_COOP_AUTO"), *drop = cfg_get("NX2NAVI_COOP_DROP");
            int automatic = auto_mode && (!strcmp(auto_mode, "create") || !strcmp(auto_mode, "join"));
            if (want) {
                if (!client_id) client_id = coop_client_id();
                if (!client_id) {
                    if (!said_random) rt_log("coop: the system's random source failed; co-op stays off");
                    said_random = 1, want = 0;
                }
                const char *n = cfg_value("NX2NAVI_COOP_NAME");
                coop_name_clean(name, sizeof name, (const uint8_t *)n, strlen(n));
                if (!name[0]) snprintf(name, sizeof name, "Player-%04x", g_name_digits);
                copy_str(server, sizeof server, cfg_value("NX2NAVI_COOP_SERVER"));
            }
            CoopParams p = {.server = server, .name = name, .client_id = client_id, .on = want, .retry = automatic,
                            .local_port = (uint16_t)cfg_int("NX2NAVI_COOP_PORT"), .drop_pct = drop ? (unsigned)atoi(drop) : 0};
            coop_sess_configure(s, &p);
            if (want && !on && auto_mode && *auto_mode && strcmp(auto_mode, "off") && !automatic)
                rt_log("coop: COOP_AUTO must be off, create or join");
            if (want && !on && automatic) {   // a session at launch, for tests and LAN parties
                const char *room = cfg_get("NX2NAVI_COOP_ROOM");
                char secret[COOP_SECRET_LEN + 1] = "";
                int has = room && *room && coop_secret_normalize(room, secret);
                int join = !strcmp(auto_mode, "join");
                if (room && *room && !has) rt_log("coop: COOP_ROOM is not a room secret (18 characters, XXXXXX-XXXXXX-XXXXXX)");
                if (join && !has) rt_log("coop: COOP_AUTO=join needs COOP_ROOM");
                else coop_sess_command(s, join ? COOP_CMD_JOIN : COOP_CMD_CREATE, secret);
            }
            // the maker removes a member: COOP_REMOVE = its member id (until the menu has a row for it); a key's
            // fingerprint there is progress sync's (coop_game.c), for a member that left
            const char *rm = cfg_get("NX2NAVI_COOP_REMOVE");
            if (want && rm && *rm && strcmp(rm, removed) && strlen(rm) <= 2 && strspn(rm, "0123456789") == strlen(rm)) coop_remove_member(atoi(rm));
            copy_str(removed, sizeof removed, rm);
            on = want;
        }
        coop_sess_step(s);
    }
}

void coop_init(void) {
    if (!process_session() || g_started) return;
    g_started = 1;
    if (!plat_thread_spawn(coop_main, NULL, "coop", 0)) rt_log("coop: cannot start the co-op thread");
}

void coop_shutdown(void) {
    if (!g_started) return;
    CoopSess *s = process_session();
    __atomic_store_n(&g_final, 1, __ATOMIC_RELEASE);
    int st = __atomic_load_n(&s->pub_state, __ATOMIC_ACQUIRE);
    if (st != COOP_JOINED && st != COOP_RECONNECTING) return;   // no room to say goodbye to
    coop_sess_stop(s);
    coop_sess_wait_off(s, 400);
}

int coop_command(CoopCmd cmd, const char *secret) {
    CoopSess *s = process_session();
    if (!s || __atomic_load_n(&s->pub_state, __ATOMIC_ACQUIRE) == COOP_OFF) return 0;
    return coop_sess_command(s, cmd, secret);
}

static CoopRemoveFn g_remove_fn;
static void *g_remove_user;

int coop_on_remove(CoopRemoveFn fn, void *user) {
    g_remove_user = user;
    __atomic_store_n(&g_remove_fn, fn, __ATOMIC_RELEASE);
    return 1;
}

int coop_remove_member(int member) {
    CoopSess *s = process_session();
    if (!s) return 0;
    CoopRemoveFn fn = __atomic_load_n(&g_remove_fn, __ATOMIC_ACQUIRE);
    int signed_out = fn && fn(member, g_remove_user);
    CoopView v;
    coop_sess_view(s, &v);
    int kicked = v.made && coop_sess_remove(s, member);   // the relay takes a removal only from the member it marks
    return signed_out || kicked;
}

void coop_view_get(CoopView *out) {
    CoopSess *s = process_session();
    if (s) coop_sess_view(s, out);
    else memset(out, 0, sizeof *out);
}

int coop_send(uint8_t kind, const void *bytes, size_t n) {
    CoopSess *s = process_session();
    return s && coop_sess_send(s, 0, kind, bytes, n);
}

int coop_send_to(int member, uint8_t kind, const void *bytes, size_t n) {
    CoopSess *s = process_session();
    return s && member > 0 && coop_sess_send(s, member, kind, bytes, n);
}

int coop_on_message(uint8_t kind, CoopMsgFn fn, void *user) {
    CoopSess *s = process_session();
    return s && coop_sess_on_message(s, kind, fn, user);
}

int coop_on_view(CoopViewFn fn, void *user) {
    CoopSess *s = process_session();
    return s && coop_sess_on_view(s, fn, user);
}

int coop_on_tick(CoopTickFn fn, void *user) {
    CoopSess *s = process_session();
    return s && coop_sess_on_tick(s, fn, user);
}
