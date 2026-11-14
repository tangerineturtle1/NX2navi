// Steam for plugins (plugin_api.h Nx2Steam, version 7): rooms, invites and datagrams for a mod's own multiplayer,
// through the Steam code the game's own co-op uses (steam.c).
//
// One thread, the Steam thread, owns steam.c while a plugin has Steam: it signs in, runs Steam's callbacks, sends what
// the plugin queued and calls the plugin's event callback. The plugin's calls come from any thread; they only queue a
// command or a datagram under a lock and wake the thread, and read copies of the state (status, members, friends) the
// thread publishes under the same lock. The callback is called with no lock held, so it may call back in.
//
// One user of Steam at a time (steam.c steam_start): a plugin's start is refused while the game's own co-op is on
// (coop_steam = 1), and co-op turned on later finds Steam in use until the plugin stops. A second plugin is refused
// while the first has it. The plugin's datagrams travel on channel CHANNEL of Steam's networking messages, co-op's on
// 0, so neither ever reads the other's.
//
// A plugin's rooms. A room is a friends-only Steam lobby that carries the note nx2navi_plugin = <the plugin's tag>.
// The app id is shared with every program that uses Steam's test app, and with the game's own co-op (whose lobbies
// carry nx2navi = 1 and not this note): a lobby entered that lacks the note or carries another tag is left at once.
// Only members of the room can open a session with this game or have a datagram handed out (steam.c).
//
// The stand-in (tests of two games on one PC, which one Steam account cannot do). NX2NAVI_PLUGIN_STEAM_LOCAL=<port>
// in the environment (never from nx2navi.ini) makes this game the player whose id is that UDP port on 127.0.0.1,
// named NX2NAVI_PLUGIN_STEAM_LOCAL_NAME or "Player <port>". A room's number is built from its maker's port in the
// shape of a lobby's number, so it has a room id as a real room has, and another game joins it by that id. The maker
// keeps the member list: a member says JOIN (with the tag and its name) once a second, the maker answers everyone
// with MEMBERS and drops a member it has not heard for five seconds; a member that has not heard the maker for five
// seconds has lost the room. NX2NAVI_PLUGIN_STEAM_LOCAL_FRIENDS=<port>,<port> are this player's friends; an invite is
// a datagram to the friend's port. Loopback loses and reorders nothing in practice, so "reliable" is a plain datagram.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "plugins.h"
#include "steam.h"

enum {
    CHANNEL = 1,
    COMMANDS = 16, OUTBOX = 512, FRIENDS_MAX = 512,
    RETRY_MS = 5000, FRIENDS_EVERY_MS = 3000, PUBLISH_EVERY_MS = 500, OWNER_EVERY_MS = 250,
    WAIT_IN_ROOM_NS = 2000000, WAIT_IDLE_NS = 20000000, WAIT_DOWN_NS = 200000000,
};
#define TAG_KEY "nx2navi_plugin"

typedef void (*EventFn)(const SteamEvent *e, void *user);
typedef struct {
    int (*start)(char *why, size_t cap, int *which);
    void (*stop)(void);
    uint64_t (*self)(void);
    void (*name_of)(uint64_t id, char *out, size_t cap);
    int (*room_create)(int max_players);
    int (*room_join)(uint64_t room);
    void (*room_leave)(void);
    uint64_t (*room_owner)(void);
    int (*room_members)(uint64_t *out, int max);
    int (*room_set)(const char *key, const char *value);
    void (*room_get)(const char *key, char *out, size_t cap);
    int (*friends)(SteamFriend *out, int max);
    int (*invite)(uint64_t friend_id);
    int (*send)(uint64_t to, const void *data, size_t n, int reliable);
    void (*pump)(EventFn on, void *user);
} Net;

typedef enum { C_CREATE = 1, C_JOIN, C_LEAVE, C_INVITE } CmdKind;
typedef struct { CmdKind kind; uint64_t arg; } Cmd;
typedef struct { uint64_t to; uint16_t n; uint8_t reliable; uint8_t bytes[NX2_STEAM_MESSAGE_MAX]; } Out;

// What the plugin's threads and the Steam thread share, under `lock`.
static struct {
    PlatOnce once;
    PlatMutex lock;
    PlatEvent wake;
    PlatThread *thread;           // the Steam thread, also after it ended, until it is joined
    volatile uint32_t thread_id;  // its id while it runs
    volatile int32_t stop;
    int running;                  // a plugin has Steam (from start until the thread ended)
    Nx2SteamFn fn;
    void *user;
    char tag[32];
    char who[64];                 // the plugin as the log names it: by its tag
    Nx2SteamStatus st;
    int nmembers;
    Nx2SteamMember members[NX2_STEAM_ROOM_MAX];
    int nfriends;
    Nx2SteamFriend *friends;
    int ncmd;
    Cmd cmd[COMMANDS];
    int nout;
    Out *out;
} P;

// The Steam thread's own.
static struct {
    const Net *net;
    int up, local;
    uint64_t retry_at, friends_at, publish_at, owner_at;
    uint64_t room, owner;
    int made, entering;
    int why;
    char says[200];
} T;

// ---- the stand-in

enum { L_JOIN = 1, L_MEMBERS, L_DATA, L_LEAVE, L_INVITE, L_NAME = 32, L_SILENT_MS = 5000, L_JOIN_EVERY_MS = 1000,
       L_JOIN_WAIT_MS = 4000, L_KNOWN = 16 };
#define LOCAL_ROOM(port) (0x01860000ull << 32 | (uint64_t)(port))   // the shape of a lobby's number (steam.c steam_code_make)

static struct {
    PlatUdp *udp;
    uint64_t self, room;
    uint64_t joining;             // the room asked for, until its maker answers
    uint64_t join_since, join_sent, members_sent, maker_heard;
    int n;
    uint64_t ids[NX2_STEAM_ROOM_MAX];
    char names[NX2_STEAM_ROOM_MAX][L_NAME];
    uint64_t heard[NX2_STEAM_ROOM_MAX];   // the maker's: when each member last said JOIN
    int pending_entered;          // the maker's own room: entered at the next pump
    char name[L_NAME];
    int nknown;                   // names of players outside the room (who invited)
    uint64_t known_id[L_KNOWN];
    char known_name[L_KNOWN][L_NAME];
} L;

static uint64_t env_number(const char *name) {
    char v[32];
    return plat_getenv(name, v, sizeof v) ? strtoull(v, NULL, 10) : 0;
}

static int local_wanted(void) { return env_number("NX2NAVI_PLUGIN_STEAM_LOCAL") != 0; }

static const uint8_t loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 127, 0, 0, 1};

static void to_port(uint64_t port, const void *p, size_t n) {
    PlatAddr a;
    plat_addr_set(&a, loopback, (uint16_t)port, 0);
    plat_udp_send(L.udp, &a, p, n);
}

static int l_start(char *why, size_t cap, int *which) {
    if (L.udp) return 1;
    uint64_t port = env_number("NX2NAVI_PLUGIN_STEAM_LOCAL");
    L.udp = port && port < 65536 ? plat_udp_open((uint16_t)port, 0) : NULL;
    if (!L.udp) {
        snprintf(why, cap, "the test port %llu cannot be opened", (unsigned long long)port);
        if (which) *which = STEAM_WHY_NO_CLIENT;
        return 0;
    }
    L.self = port;
    if (!plat_getenv("NX2NAVI_PLUGIN_STEAM_LOCAL_NAME", L.name, sizeof L.name) || !L.name[0])
        snprintf(L.name, sizeof L.name, "Player %llu", (unsigned long long)port);
    L.room = L.joining = 0, L.n = 0, L.nknown = 0;
    if (which) *which = STEAM_WHY_OK;
    return 1;
}

static void l_room_leave(void) {
    if (L.room && L.room != LOCAL_ROOM(L.self)) to_port((uint32_t)L.room, (uint8_t[]){L_LEAVE}, 1);
    if (L.room == LOCAL_ROOM(L.self))
        for (int i = 1; i < L.n; i++) to_port(L.ids[i], (uint8_t[]){L_LEAVE}, 1);
    L.room = L.joining = 0, L.n = 0, L.pending_entered = 0;
}

static void l_stop(void) {
    if (!L.udp) return;
    l_room_leave();
    plat_udp_close(L.udp);
    L.udp = NULL;
}

static uint64_t l_self(void) { return L.self; }

static int l_member(uint64_t id) {
    for (int i = 0; i < L.n; i++)
        if (L.ids[i] == id) return i;
    return -1;
}

static void l_name_of(uint64_t id, char *out, size_t cap) {
    int i = l_member(id);
    if (id == L.self) {
        snprintf(out, cap, "%s", L.name);
    } else if (i >= 0 && L.names[i][0]) {
        snprintf(out, cap, "%s", L.names[i]);
    } else {
        for (int k = 0; k < L.nknown; k++)
            if (L.known_id[k] == id) {
                snprintf(out, cap, "%s", L.known_name[k]);
                return;
            }
        snprintf(out, cap, "Player %llu", (unsigned long long)id);
    }
}

static int l_room_create(int max_players) {
    (void)max_players;
    if (!L.udp || L.room || L.joining) return 0;
    L.room = LOCAL_ROOM(L.self), L.n = 1, L.ids[0] = L.self, L.pending_entered = 1;
    snprintf(L.names[0], L_NAME, "%s", L.name);
    return 1;
}

static int l_room_join(uint64_t room) {
    if (!L.udp || L.room || L.joining || room >> 32 != LOCAL_ROOM(0) >> 32 || !(uint32_t)room || (uint32_t)room > 65535 ||
        room == LOCAL_ROOM(L.self))
        return 0;
    L.joining = room, L.join_since = plat_now_ms(), L.join_sent = 0;
    return 1;
}

static uint64_t l_room_owner(void) { return L.room ? (uint32_t)L.room : 0; }
static int l_room_members(uint64_t *out, int max) {
    int n = L.room ? L.n : 0;
    if (n > max) n = max;
    memcpy(out, L.ids, (size_t)n * sizeof *out);
    return n;
}
static int l_room_set(const char *key, const char *value) { (void)key, (void)value; return 1; }
static void l_room_get(const char *key, char *out, size_t cap) {
    // the maker let this game in because its JOIN carried the maker's tag
    snprintf(out, cap, "%s", !strcmp(key, TAG_KEY) ? P.tag : "");
}

static int l_friends(SteamFriend *out, int max) {
    char list[256];
    int n = 0;
    if (!plat_getenv("NX2NAVI_PLUGIN_STEAM_LOCAL_FRIENDS", list, sizeof list)) return 0;
    for (char *p = list; *p && n < max;) {
        uint64_t port = strtoull(p, &p, 10);
        if (port && port < 65536 && port != L.self) {
            SteamFriend *f = &out[n++];
            memset(f, 0, sizeof *f);
            f->id = port, f->online = 1, f->in_app = 1;
            snprintf(f->name, sizeof f->name, "Player %llu", (unsigned long long)port);
        }
        while (*p && (*p < '0' || *p > '9')) p++;
    }
    return n;
}

static int l_invite(uint64_t friend_id) {
    if (!L.room || !friend_id || friend_id > 65535) return 0;
    uint8_t d[1 + 8 + L_NAME] = {L_INVITE};
    memcpy(d + 1, &L.room, 8);
    memcpy(d + 9, L.name, L_NAME);
    to_port(friend_id, d, sizeof d);
    return 1;
}

static int l_send(uint64_t to, const void *data, size_t n, int reliable) {
    (void)reliable;
    if (!L.room || n > STEAM_MESSAGE_MAX || l_member(to) < 0) return 0;
    uint8_t d[1 + STEAM_MESSAGE_MAX];
    d[0] = L_DATA;
    memcpy(d + 1, data, n);
    to_port(to, d, n + 1);
    return 1;
}

static void l_send_members(void) {
    uint8_t d[2 + (8 + L_NAME) * NX2_STEAM_ROOM_MAX];
    d[0] = L_MEMBERS, d[1] = (uint8_t)L.n;
    for (int i = 0; i < L.n; i++) {
        memcpy(d + 2 + (8 + L_NAME) * i, &L.ids[i], 8);
        memcpy(d + 2 + (8 + L_NAME) * i + 8, L.names[i], L_NAME);
    }
    for (int i = 1; i < L.n; i++) to_port(L.ids[i], d, 2 + (8 + L_NAME) * (size_t)L.n);
    L.members_sent = plat_now_ms();
}

static void l_drop(int i, EventFn on, void *user) {
    uint64_t id = L.ids[i];
    for (int k = i; k + 1 < L.n; k++) {
        L.ids[k] = L.ids[k + 1], L.heard[k] = L.heard[k + 1];
        memcpy(L.names[k], L.names[k + 1], L_NAME);
    }
    L.n--;
    on(&(SteamEvent){.kind = STEAM_EV_MEMBER_LEFT, .a = id}, user);
}

static void l_pump(EventFn on, void *user) {
    if (!L.udp) return;
    uint64_t now = plat_now_ms();
    int maker = L.room && L.room == LOCAL_ROOM(L.self);
    if (L.pending_entered) {
        L.pending_entered = 0;
        on(&(SteamEvent){.kind = STEAM_EV_ROOM_ENTERED, .a = L.room}, user);
    }
    uint64_t at = L.joining ? L.joining : L.room && !maker ? L.room : 0;
    if (at && now - L.join_sent >= L_JOIN_EVERY_MS) {
        uint8_t d[1 + sizeof P.tag + L_NAME] = {L_JOIN};
        memcpy(d + 1, P.tag, sizeof P.tag);
        memcpy(d + 1 + sizeof P.tag, L.name, L_NAME);
        to_port((uint32_t)at, d, sizeof d);
        L.join_sent = now;
    }
    if (L.joining && now - L.join_since > L_JOIN_WAIT_MS) {
        L.joining = 0;
        on(&(SteamEvent){.kind = STEAM_EV_ROOM_FAILED, .code = STEAM_FAIL_NO_ROOM}, user);
    }
    for (;;) {
        uint8_t d[1 + STEAM_MESSAGE_MAX + 8];
        PlatAddr from;
        uint8_t ip[16];
        int n = plat_udp_recv(L.udp, d, sizeof d, &from, 0);
        if (n <= 0) break;
        plat_addr_get(&from, ip, NULL);
        if (memcmp(ip, loopback, 16)) continue;   // the stand-in is for games on this PC
        uint64_t who = plat_addr_port(&from);
        maker = L.room && L.room == LOCAL_ROOM(L.self);
        if (d[0] == L_JOIN && maker && n == 1 + (int)sizeof P.tag + L_NAME) {
            d[sizeof P.tag] = 0, d[n - 1] = 0;
            if (strcmp((const char *)d + 1, P.tag)) continue;   // a game of another plugin: it finds no room here
            int i = l_member(who);
            if (i < 0 && L.n < NX2_STEAM_ROOM_MAX) {
                i = L.n++;
                L.ids[i] = who, L.heard[i] = now;
                snprintf(L.names[i], L_NAME, "%s", (const char *)d + 1 + sizeof P.tag);
                l_send_members();
                on(&(SteamEvent){.kind = STEAM_EV_MEMBER_JOINED, .a = who}, user);
            } else if (i >= 0) {
                L.heard[i] = now;
            }
        } else if (d[0] == L_LEAVE && maker) {
            int i = l_member(who);
            if (i > 0) l_drop(i, on, user), l_send_members();
        } else if (d[0] == L_LEAVE && L.room && !maker && who == (uint32_t)L.room) {
            L.room = 0, L.n = 0;
            on(&(SteamEvent){.kind = STEAM_EV_LOST}, user);
        } else if (d[0] == L_MEMBERS && n >= 2 && n == 2 + (8 + L_NAME) * d[1] && d[1] <= NX2_STEAM_ROOM_MAX &&
                   ((L.joining && who == (uint32_t)L.joining) || (L.room && !maker && who == (uint32_t)L.room))) {
            int count = d[1];
            uint64_t ids[NX2_STEAM_ROOM_MAX];
            for (int k = 0; k < count; k++) memcpy(&ids[k], d + 2 + (8 + L_NAME) * k, 8);
            int entering = L.joining != 0;
            if (entering) L.room = L.joining, L.joining = 0, L.n = 0;
            L.maker_heard = now;
            // who left, then who came, against the list held
            for (int i = L.n - 1; i >= 0; i--) {
                int still = 0;
                for (int k = 0; k < count; k++) still |= ids[k] == L.ids[i];
                if (!still) l_drop(i, on, user);
            }
            for (int k = 0; k < count; k++) {
                int i = l_member(ids[k]), fresh = i < 0;
                if (fresh) {
                    if (L.n >= NX2_STEAM_ROOM_MAX) continue;
                    i = L.n++, L.ids[i] = ids[k];
                }
                memcpy(L.names[i], d + 2 + (8 + L_NAME) * k + 8, L_NAME);
                L.names[i][L_NAME - 1] = 0;
                if (fresh && !entering && ids[k] != L.self) on(&(SteamEvent){.kind = STEAM_EV_MEMBER_JOINED, .a = ids[k]}, user);
            }
            if (entering) on(&(SteamEvent){.kind = STEAM_EV_ROOM_ENTERED, .a = L.room}, user);
        } else if (d[0] == L_INVITE && n == 1 + 8 + L_NAME) {
            uint64_t room;
            memcpy(&room, d + 1, 8);
            d[n - 1] = 0;
            int k = 0;
            while (k < L.nknown && L.known_id[k] != who) k++;
            if (k == L.nknown && L.nknown < L_KNOWN) L.nknown++;
            if (k < L_KNOWN) L.known_id[k] = who, snprintf(L.known_name[k], L_NAME, "%s", (const char *)d + 9);
            on(&(SteamEvent){.kind = STEAM_EV_INVITED, .a = room, .b = who}, user);
        } else if (d[0] == L_DATA && L.room && l_member(who) >= 0 && n > 1) {
            on(&(SteamEvent){.kind = STEAM_EV_MESSAGE, .a = who, .data = d + 1, .n = (size_t)n - 1}, user);
        }
        if (!L.udp) return;   // a handler stopped the stand-in
    }
    maker = L.room && L.room == LOCAL_ROOM(L.self);
    if (maker) {
        for (int i = L.n - 1; i > 0; i--)
            if (now - L.heard[i] > L_SILENT_MS) l_drop(i, on, user), l_send_members();
        if (now - L.members_sent >= L_JOIN_EVERY_MS) l_send_members();
    } else if (L.room && now - L.maker_heard > L_SILENT_MS) {
        L.room = 0, L.n = 0;
        on(&(SteamEvent){.kind = STEAM_EV_LOST}, user);
    }
}

static const Net net_local = {l_start, l_stop, l_self, l_name_of, l_room_create, l_room_join, l_room_leave, l_room_owner,
                              l_room_members, l_room_set, l_room_get, l_friends, l_invite, l_send, l_pump};

// ---- Steam itself

static int s_start(char *why, size_t cap, int *which) {
    if (!steam_start(why, cap, which)) return 0;
    steam_channel(CHANNEL);
    return 1;
}

static const Net net_steam = {s_start, steam_stop, steam_self, steam_name_of, steam_room_create, steam_room_join,
                              steam_room_leave, steam_room_owner, steam_room_members, steam_room_set, steam_room_get,
                              steam_friends, steam_invite, steam_send, steam_pump};

// ---- the Steam thread

// The game's own co-op is on: it has, or will take, Steam. Read from the settings as they are now (the menu's switch
// works at once); a build without that co-op has no such setting.
static int game_coop_on(void) {
    const char *v = cfg_get("NX2NAVI_COOP_STEAM");
    return v && *v && strcmp(v, "0");
}

static void clean_text(char *s) {
    for (unsigned char *p = (unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) *p = ' ';
        // U+202A..U+202E and U+2066..U+2069: direction overrides and isolates
        if (p[0] == 0xe2 && ((p[1] == 0x80 && p[2] >= 0xaa && p[2] <= 0xae) || (p[1] == 0x81 && p[2] >= 0xa6 && p[2] <= 0xa9)))
            p[0] = p[1] = p[2] = ' ';
    }
}

static void emit(uint32_t kind, uint64_t a, uint64_t b, int code, const void *data, size_t n) {
    if (__atomic_load_n(&P.stop, __ATOMIC_ACQUIRE)) return;
    Nx2SteamEvent e = {.size = sizeof e, .kind = kind, .a = a, .b = b, .code = code, .n = (uint32_t)n, .data = data};
    P.fn(&e, P.user);
}

// The state as the plugin's calls read it.
static void publish(int state) {
    Nx2SteamStatus st;
    Nx2SteamMember members[NX2_STEAM_ROOM_MAX];
    int nm = 0;
    memset(&st, 0, sizeof st);
    memset(members, 0, sizeof members);
    st.size = sizeof st, st.state = (uint32_t)state, st.local = (uint32_t)T.local;
    st.why = state == NX2_STEAM_UNAVAILABLE ? (uint32_t)T.why : NX2_STEAM_WHY_NONE;
    if (state == NX2_STEAM_UNAVAILABLE) snprintf(st.says, sizeof st.says, "%s", T.says);
    if (T.up) {
        st.self = T.net->self();
        T.net->name_of(st.self, st.name, sizeof st.name);
        clean_text(st.name);
        st.entering = (uint32_t)T.entering;
        if (T.room) {
            uint64_t ids[NX2_STEAM_ROOM_MAX];
            int n = T.net->room_members(ids, NX2_STEAM_ROOM_MAX);
            st.room = T.room, st.owner = T.owner;
            steam_code_make(T.room, st.room_code);
            // the owner first
            for (int pass = 0; pass < 2; pass++)
                for (int i = 0; i < n; i++) {
                    if ((ids[i] == T.owner) != (pass == 0)) continue;
                    Nx2SteamMember *m = &members[nm++];
                    m->id = ids[i], m->self = ids[i] == st.self, m->owner = ids[i] == T.owner;
                    T.net->name_of(ids[i], m->name, sizeof m->name);
                    clean_text(m->name);
                }
            st.members = (uint32_t)nm;
        }
    }
    plat_mutex_lock(&P.lock);
    for (int i = 0; i < P.ncmd; i++)   // asked for and not yet begun
        if (P.cmd[i].kind == C_CREATE || P.cmd[i].kind == C_JOIN) st.entering = 1;
    P.st = st;
    P.nmembers = nm;
    memcpy(P.members, members, sizeof members);
    plat_mutex_unlock(&P.lock);
    T.publish_at = plat_now_ms();
}

static void refresh_friends(void) {
    static SteamFriend found[FRIENDS_MAX];
    static Nx2SteamFriend made[FRIENDS_MAX];
    int n = T.net->friends(found, FRIENDS_MAX);
    for (int i = 0; i < n; i++) {
        memset(&made[i], 0, sizeof made[i]);
        made[i].id = found[i].id, made[i].online = (uint8_t)found[i].online, made[i].in_app = (uint8_t)found[i].in_app;
        snprintf(made[i].name, sizeof made[i].name, "%s", found[i].name);
        clean_text(made[i].name);
    }
    plat_mutex_lock(&P.lock);
    int first = P.nfriends != n;
    memcpy(P.friends, made, sizeof made[0] * (size_t)n);
    P.nfriends = n;
    plat_mutex_unlock(&P.lock);
    // counts only: the log never holds a Steam id or a friend's name
    if (first) rt_log("plugins: Steam: %d friend%s listed", n, n == 1 ? "" : "s");
    T.friends_at = plat_now_ms();
}

static void room_gone(void) {
    T.room = T.owner = 0, T.made = T.entering = 0;
}

static void on_event(const SteamEvent *e, void *user) {
    (void)user;
    switch (e->kind) {
    case STEAM_EV_ROOM_ENTERED:
        if (!T.entering) {   // the plugin gave up meanwhile (room_leave)
            T.net->room_leave();
            break;
        }
        T.entering = 0;
        if (T.made) {
            T.net->room_set(TAG_KEY, P.tag);
        } else {
            char tag[sizeof P.tag + 1];
            T.net->room_get(TAG_KEY, tag, sizeof tag);
            if (strcmp(tag, P.tag)) {
                rt_log("plugins: Steam: that lobby is not a room of %s's; left it", P.who);
                T.net->room_leave();
                room_gone();
                publish(NX2_STEAM_READY);
                emit(NX2_STEAM_EV_ROOM_FAILED, 0, 0, NX2_STEAM_FAIL_NOT_OURS, NULL, 0);
                break;
            }
        }
        T.room = e->a;
        T.owner = T.net->room_owner();
        rt_log("plugins: Steam: %s", T.made ? "room made" : "in the room");
        publish(NX2_STEAM_READY);
        emit(NX2_STEAM_EV_ROOM_ENTERED, e->a, 0, 0, NULL, 0);
        break;
    case STEAM_EV_ROOM_FAILED:
        if (!T.entering) break;
        room_gone();
        rt_log("plugins: Steam: the room was not %s (%d)", e->code == STEAM_FAIL_CREATE ? "made" : "entered", e->code);
        publish(NX2_STEAM_READY);
        emit(NX2_STEAM_EV_ROOM_FAILED, 0, 0, e->code, NULL, 0);   // STEAM_FAIL_* are NX2_STEAM_FAIL_*'s numbers
        break;
    case STEAM_EV_MEMBER_JOINED:
    case STEAM_EV_MEMBER_LEFT:
        if (!T.room) break;
        publish(NX2_STEAM_READY);
        rt_log("plugins: Steam: a player %s the room (%u in it)", e->kind == STEAM_EV_MEMBER_JOINED ? "came into" : "left",
               (unsigned)P.st.members);
        emit(e->kind == STEAM_EV_MEMBER_JOINED ? NX2_STEAM_EV_MEMBER_JOINED : NX2_STEAM_EV_MEMBER_LEFT, e->a, 0, 0, NULL, 0);
        break;
    case STEAM_EV_INVITED:
        emit(NX2_STEAM_EV_INVITED, e->a, e->b, 0, NULL, 0);
        break;
    case STEAM_EV_JOIN_ASKED:
        emit(NX2_STEAM_EV_JOIN_ASKED, e->a, e->b, 0, NULL, 0);
        break;
    case STEAM_EV_MESSAGE:
        if (T.room) emit(NX2_STEAM_EV_MESSAGE, e->a, 0, 0, e->data, e->n);
        break;
    case STEAM_EV_LOST:
        room_gone();
        rt_log("plugins: Steam: the room is lost (Steam closed or lost its connection)");
        publish(NX2_STEAM_READY);
        emit(NX2_STEAM_EV_LOST, 0, 0, 0, NULL, 0);
        break;
    }
}

static void run_command(const Cmd *c) {
    switch (c->kind) {
    case C_CREATE:
    case C_JOIN:
        if (!T.up || T.room || T.entering) break;
        T.made = c->kind == C_CREATE, T.entering = 1;
        if (!(T.made ? T.net->room_create((int)c->arg) : T.net->room_join(c->arg))) {
            room_gone();
            publish(NX2_STEAM_READY);
            emit(NX2_STEAM_EV_ROOM_FAILED, 0, 0, c->kind == C_CREATE ? NX2_STEAM_FAIL_CREATE : NX2_STEAM_FAIL_OTHER, NULL, 0);
        }
        break;
    case C_LEAVE:
        if (!T.up) break;
        if (T.room) rt_log("plugins: Steam: left the room");
        T.net->room_leave();
        room_gone();
        publish(NX2_STEAM_READY);
        break;
    case C_INVITE:
        if (T.up && T.room) T.net->invite(c->arg);
        break;
    }
}

static void try_start(uint64_t now) {
    int which = 0, why;
    char says[200] = "";
    T.net = local_wanted() ? &net_local : &net_steam;
    T.local = T.net == &net_local;
    if (game_coop_on() && !T.local) {
        why = NX2_STEAM_WHY_GAME_COOP;
        snprintf(says, sizeof says, "the game's own co-op is on (coop_steam = 1) and has Steam: turn it off to use this mod's");
    } else if (T.net->start(says, sizeof says, &which)) {
        T.up = 1, T.why = 0, T.says[0] = 0;
        room_gone();
        rt_log("plugins: Steam for %s: on%s", P.who, T.local ? " (the test stand-in for Steam)" : ", signed in to Steam as app 480");
        publish(NX2_STEAM_READY);
        refresh_friends();
        emit(NX2_STEAM_EV_STATE, 0, 0, 0, NULL, 0);
        return;
    } else {
        why = which == STEAM_WHY_NO_LIBRARY ? NX2_STEAM_WHY_NO_LIBRARY : which == STEAM_WHY_OLD_LIBRARY ? NX2_STEAM_WHY_OLD_LIBRARY
            : which == STEAM_WHY_NO_CLIENT ? NX2_STEAM_WHY_NO_CLIENT : which == STEAM_WHY_IN_USE ? NX2_STEAM_WHY_GAME_COOP
            : NX2_STEAM_WHY_OTHER;
    }
    T.retry_at = now + RETRY_MS;
    int changed = why != T.why || strcmp(says, T.says);
    T.why = why;
    snprintf(T.says, sizeof T.says, "%s", says);
    if (changed) {   // said once, not at every try
        rt_log("plugins: Steam for %s is not available: %s", P.who, says);
        publish(NX2_STEAM_UNAVAILABLE);
        emit(NX2_STEAM_EV_STATE, 0, 0, 0, NULL, 0);
    }
}

static void steam_main(void *arg) {
    (void)arg;
    static Out batch[OUTBOX];
    __atomic_store_n(&P.thread_id, plat_thread_id(), __ATOMIC_RELEASE);
    memset(&T, 0, sizeof T);
    while (!__atomic_load_n(&P.stop, __ATOMIC_ACQUIRE)) {
        uint64_t now = plat_now_ms();
        Cmd cmds[COMMANDS];
        plat_mutex_lock(&P.lock);
        int nc = P.ncmd, nout = P.nout;
        memcpy(cmds, P.cmd, sizeof cmds[0] * (size_t)nc);
        if (nout) memcpy(batch, P.out, sizeof batch[0] * (size_t)nout);
        P.ncmd = P.nout = 0;
        plat_mutex_unlock(&P.lock);
        if (!T.up) {
            if (now >= T.retry_at) try_start(now);
            if (!T.up) {
                plat_event_wait_ns(&P.wake, WAIT_DOWN_NS);
                continue;
            }
        }
        for (int i = 0; i < nc; i++) run_command(&cmds[i]);
        if (nout && T.room) {
            // only to members: a datagram to anyone else would open a session with a stranger
            uint64_t ids[NX2_STEAM_ROOM_MAX], self = T.net->self();
            int n = T.net->room_members(ids, NX2_STEAM_ROOM_MAX);
            for (int i = 0; i < nout; i++)
                for (int k = 0; k < n; k++)
                    if (ids[k] != self && (!batch[i].to || batch[i].to == ids[k]))
                        T.net->send(ids[k], batch[i].bytes, batch[i].n, batch[i].reliable);
        }
        T.net->pump(on_event, NULL);
        if (T.room && now - T.owner_at >= OWNER_EVERY_MS) {
            // the owner left and Steam picked another member
            uint64_t owner = T.net->room_owner();
            T.owner_at = now;
            if (owner && owner != T.owner) {
                T.owner = owner;
                rt_log("plugins: Steam: the room has another owner now%s", owner == T.net->self() ? " (this game)" : "");
                publish(NX2_STEAM_READY);
                emit(NX2_STEAM_EV_OWNER_CHANGED, owner, 0, 0, NULL, 0);
            }
        }
        if (now - T.publish_at >= PUBLISH_EVERY_MS || nc) publish(NX2_STEAM_READY);   // names Steam fetched meanwhile
        if (now - T.friends_at >= FRIENDS_EVERY_MS) refresh_friends();
        plat_event_wait_ns(&P.wake, T.room ? WAIT_IN_ROOM_NS : WAIT_IDLE_NS);
    }
    if (T.up) {
        if (T.room || T.entering) T.net->room_leave();
        T.net->stop();
        T.up = 0;
        rt_log("plugins: Steam for %s: off", P.who);
    }
    room_gone();
    plat_mutex_lock(&P.lock);
    memset(&P.st, 0, sizeof P.st);
    P.st.size = sizeof P.st;
    P.nmembers = P.nfriends = P.ncmd = P.nout = 0;
    P.running = 0;
    plat_mutex_unlock(&P.lock);
    __atomic_store_n(&P.thread_id, 0, __ATOMIC_RELEASE);
}

// ---- the plugin's calls

static void init_once(void *arg) {
    (void)arg;
    plat_event_init(&P.wake, 0, 0);
    P.st.size = sizeof P.st;
}

static int on_steam_thread(void) {
    uint32_t id = __atomic_load_n(&P.thread_id, __ATOMIC_ACQUIRE);
    return id && id == plat_thread_id();
}

// The thread of an earlier start that has ended, or is ending, is waited for and freed. Not on that thread itself.
static void join_ended(void) {
    plat_mutex_lock(&P.lock);
    PlatThread *t = P.thread;
    P.thread = NULL;
    plat_mutex_unlock(&P.lock);
    if (t) plat_thread_join(t);
}

static void refuse(int why, const char *says) {
    plat_mutex_lock(&P.lock);
    if (!P.running) {
        memset(&P.st, 0, sizeof P.st);
        P.st.size = sizeof P.st, P.st.state = NX2_STEAM_OFF, P.st.why = (uint32_t)why;
        snprintf(P.st.says, sizeof P.st.says, "%s", says);
    }
    plat_mutex_unlock(&P.lock);
    rt_log("plugins: Steam was not started for a plugin: %s", says);
}

static int api_start(const char *tag, Nx2SteamFn fn, void *user) {
    size_t tn = tag ? strlen(tag) : 0;
    int tag_ok = tn >= 1 && tn < sizeof P.tag;
    for (size_t i = 0; tag_ok && i < tn; i++)
        tag_ok = (tag[i] >= 'a' && tag[i] <= 'z') || (tag[i] >= '0' && tag[i] <= '9') || tag[i] == '-' || tag[i] == '_' || tag[i] == '.';
    if (!tag_ok || !fn) {
        refuse(NX2_STEAM_WHY_OTHER, !fn ? "no event callback was given" : "the tag is not 1 to 31 characters of a-z, 0-9, '-', '_' and '.'");
        return 0;
    }
    if (on_steam_thread()) {
        refuse(NX2_STEAM_WHY_IN_USE, "start was called inside a Steam event of a start that has not ended");
        return 0;
    }
    if (game_coop_on() && !local_wanted()) {
        refuse(NX2_STEAM_WHY_GAME_COOP, "the game's own co-op is on (coop_steam = 1) and has Steam: turn it off to use this mod's");
        return 0;
    }
    plat_mutex_lock(&P.lock);
    int busy = P.running && !__atomic_load_n(&P.stop, __ATOMIC_ACQUIRE);
    plat_mutex_unlock(&P.lock);
    if (busy) {
        refuse(NX2_STEAM_WHY_IN_USE, "a plugin has Steam already");
        return 0;
    }
    join_ended();
    plat_mutex_lock(&P.lock);
    if (P.running) {   // another thread's start came first
        plat_mutex_unlock(&P.lock);
        return 0;
    }
    if (!P.out) P.out = malloc(sizeof(Out) * OUTBOX);
    if (!P.friends) P.friends = malloc(sizeof(Nx2SteamFriend) * FRIENDS_MAX);
    int ok = P.out && P.friends;
    if (ok) {
        P.fn = fn, P.user = user;
        snprintf(P.tag, sizeof P.tag, "%s", tag);
        snprintf(P.who, sizeof P.who, "the plugin with the tag \"%s\"", tag);
        memset(&P.st, 0, sizeof P.st);
        P.st.size = sizeof P.st, P.st.state = NX2_STEAM_STARTING;
        P.nmembers = P.nfriends = P.ncmd = P.nout = 0;
        __atomic_store_n(&P.stop, 0, __ATOMIC_RELEASE);
        P.thread = plat_thread_new(steam_main, NULL, "plugin steam");
        ok = P.thread != NULL;
        P.running = ok;
        if (!ok) P.st.state = NX2_STEAM_OFF;
    }
    plat_mutex_unlock(&P.lock);
    if (!ok) rt_log("plugins: Steam was not started for a plugin: no thread or no memory");
    return ok;
}

static void stop_wait(uint64_t ns) {
    plat_mutex_lock(&P.lock);
    int running = P.running;
    plat_mutex_unlock(&P.lock);
    if (!running && !P.thread) return;
    __atomic_store_n(&P.stop, 1, __ATOMIC_RELEASE);
    plat_event_set(&P.wake);
    if (on_steam_thread()) return;   // inside the callback: the thread ends when the callback has returned
    plat_mutex_lock(&P.lock);
    PlatThread *t = P.thread;
    P.thread = NULL;
    plat_mutex_unlock(&P.lock);
    if (!t) return;
    if (!ns) plat_thread_join(t);
    else if (!plat_thread_join_ns(t, ns)) plat_thread_detach(t);
}

static void api_stop(void) { stop_wait(0); }

static int api_status(Nx2SteamStatus *out) {
    if (!out || out->size < 8) return 0;
    uint32_t size = out->size < sizeof P.st ? out->size : (uint32_t)sizeof P.st;
    plat_mutex_lock(&P.lock);
    memcpy(out, &P.st, size);
    plat_mutex_unlock(&P.lock);
    out->size = size;
    return 1;
}

static int command(CmdKind kind, uint64_t arg, int entering) {
    plat_mutex_lock(&P.lock);
    int ok = P.running && P.st.state == NX2_STEAM_READY && P.ncmd < COMMANDS;
    if (ok && entering) ok = !P.st.room && !P.st.entering;
    if (ok) {
        P.cmd[P.ncmd++] = (Cmd){kind, arg};
        if (entering) P.st.entering = 1;
        if (kind == C_LEAVE) P.st.entering = 0, P.nout = 0;
    }
    plat_mutex_unlock(&P.lock);
    if (ok) plat_event_set(&P.wake);
    return ok;
}

static int api_room_create(int max_players) {
    if (max_players < 2 || max_players > NX2_STEAM_ROOM_MAX) return 0;
    return command(C_CREATE, (uint64_t)max_players, 1);
}

static int api_room_join(const char *code) {
    uint64_t room = code ? steam_code_parse(code) : 0;
    return room ? command(C_JOIN, room, 1) : 0;
}

static void api_room_leave(void) { command(C_LEAVE, 0, 0); }

static int api_room_code(uint64_t room, char out[NX2_STEAM_CODE_CAP]) {
    char code[STEAM_CODE_CAP];
    if (!out) return 0;
    steam_code_make(room, code);
    snprintf(out, NX2_STEAM_CODE_CAP, "%s", code);
    return code[0] != 0;
}

static int api_members(Nx2SteamMember *out, int max) {
    if (!out || max <= 0) return 0;
    plat_mutex_lock(&P.lock);
    int n = P.nmembers < max ? P.nmembers : max;
    memcpy(out, P.members, sizeof out[0] * (size_t)n);
    plat_mutex_unlock(&P.lock);
    return n;
}

static int api_friends(Nx2SteamFriend *out, int max) {
    if (!out || max <= 0) return 0;
    plat_mutex_lock(&P.lock);
    int n = P.nfriends < max ? P.nfriends : max;
    if (n) memcpy(out, P.friends, sizeof out[0] * (size_t)n);
    plat_mutex_unlock(&P.lock);
    return n;
}

static int api_invite(uint64_t friend_id) {
    plat_mutex_lock(&P.lock);
    int in_room = P.st.room != 0;
    plat_mutex_unlock(&P.lock);
    return in_room && friend_id ? command(C_INVITE, friend_id, 0) : 0;
}

static int api_send(uint64_t to, const void *data, size_t n, int reliable) {
    if (!data || n < 1 || n > NX2_STEAM_MESSAGE_MAX) return 0;
    plat_mutex_lock(&P.lock);
    int ok = P.running && P.st.room && P.nout < OUTBOX;
    if (ok) {
        Out *o = &P.out[P.nout++];
        o->to = to, o->n = (uint16_t)n, o->reliable = reliable != 0;
        memcpy(o->bytes, data, n);
    }
    plat_mutex_unlock(&P.lock);
    if (ok) plat_event_set(&P.wake);
    return ok;
}

static const Nx2Steam table = {
    .size = sizeof(Nx2Steam),
    .start = api_start,
    .stop = api_stop,
    .status = api_status,
    .room_create = api_room_create,
    .room_join = api_room_join,
    .room_leave = api_room_leave,
    .room_code = api_room_code,
    .members = api_members,
    .friends = api_friends,
    .invite = api_invite,
    .send = api_send,
};

const Nx2Steam *plugin_steam(void) {
    plat_once(&P.once, init_once, NULL);
    return &table;
}

// plugins.c plugins_exit, after the plugins' own exit callbacks: a room a plugin left open is left and Steam signed
// out of, so the other players see this one go at once and not when Steam notices. It does not wait long: the
// process is ending.
void plugin_steam_shutdown(void) {
    plat_once(&P.once, init_once, NULL);
    stop_wait(1500000000ull);
}
