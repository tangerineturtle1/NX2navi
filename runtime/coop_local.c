// The stand-in for Steam in tests of two or more games on one PC (coop_net.h): datagrams between UDP ports on
// 127.0.0.1. A player's id is its port, a room's number its maker's port. The maker keeps the member list: a member
// says JOIN once a second, the maker answers everyone with MEMBERS and drops a member it has not heard for five
// seconds. Loopback loses and reorders nothing in practice, so "reliable" is a plain datagram here.
//
// Only the environment turns it on: a settings file cannot.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop_net.h"
#include "platform/platform.h"

enum { L_JOIN = 1, L_MEMBERS, L_DATA, L_LEAVE, L_MAX = 8, L_SILENT_MS = 5000, L_JOIN_EVERY_MS = 1000, L_JOIN_WAIT_MS = 4000 };

static struct {
    PlatUdp *udp;
    uint64_t self, room;
    uint64_t joining;        // the room asked for, until its maker answers
    uint64_t join_since, join_sent, members_sent;
    int n;
    uint64_t ids[L_MAX];
    uint64_t heard[L_MAX];   // the maker's: when each member last said JOIN
    int pending_entered;     // the maker's own room: entered at the next pump
} L;

static uint64_t env_number(const char *name) {
    char v[32];
    return plat_getenv(name, v, sizeof v) ? strtoull(v, NULL, 10) : 0;
}

int coop_local_wanted(void) { return env_number("NX2NAVI_COOP_LOCAL") != 0; }
uint64_t coop_local_join(void) { return env_number("NX2NAVI_COOP_LOCAL_JOIN"); }
int coop_local_makes(void) { return env_number("NX2NAVI_COOP_LOCAL_MAKE") == 1; }

static void to_port(uint64_t port, const void *p, size_t n) {
    static const uint8_t loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 127, 0, 0, 1};
    PlatAddr a;
    plat_addr_set(&a, loopback, (uint16_t)port, 0);
    plat_udp_send(L.udp, &a, p, n);
}

static int l_start(char *why, size_t cap, int *which) {
    if (L.udp) return 1;
    uint64_t port = env_number("NX2NAVI_COOP_LOCAL");
    L.udp = port && port < 65536 ? plat_udp_open((uint16_t)port, 0) : NULL;
    if (!L.udp) {
        snprintf(why, cap, "the test port %llu cannot be opened", (unsigned long long)port);
        if (which) *which = STEAM_WHY_NO_CLIENT;
        return 0;
    }
    L.self = port;
    if (which) *which = STEAM_WHY_OK;
    return 1;
}

static void l_room_leave(void) {
    if (L.room && L.room != L.self) to_port(L.room, (uint8_t[]){L_LEAVE}, 1);
    L.room = L.joining = 0, L.n = 0;
}

static void l_stop(void) {
    l_room_leave();
    if (L.udp) plat_udp_close(L.udp);
    L.udp = NULL;
}

static uint64_t l_self(void) { return L.self; }
static void l_name_of(uint64_t id, char *out, size_t cap) { snprintf(out, cap, "Player %llu", (unsigned long long)id); }

static int l_room_create(int max_players) {
    (void)max_players;
    if (!L.udp || L.room) return 0;
    L.room = L.self, L.n = 1, L.ids[0] = L.self, L.pending_entered = 1;
    return 1;
}

static int l_room_join(uint64_t room) {
    if (!L.udp || L.room || !room || room == L.self) return 0;
    L.joining = room, L.join_since = plat_now_ms(), L.join_sent = 0;
    return 1;
}

static uint64_t l_room(void) { return L.room; }
static uint64_t l_room_owner(void) { return L.room; }
static int l_room_members(uint64_t *out, int max) {
    int n = L.room ? L.n : 0;
    if (n > max) n = max;
    memcpy(out, L.ids, (size_t)n * sizeof *out);
    return n;
}
static int l_room_set(const char *key, const char *value) { (void)key, (void)value; return 1; }
static void l_room_get(const char *key, char *out, size_t cap) {
    snprintf(out, cap, "%s", !strcmp(key, "nx2navi") ? "1" : "");   // every room of the stand-in is one of ours
}

static int member(uint64_t id) {
    for (int i = 0; i < L.n; i++)
        if (L.ids[i] == id) return i;
    return -1;
}

static int l_send(uint64_t to, const void *data, size_t n, int reliable) {
    (void)reliable;
    if (!L.room || n > STEAM_MESSAGE_MAX || member(to) < 0) return 0;
    uint8_t d[1 + STEAM_MESSAGE_MAX];
    d[0] = L_DATA;
    memcpy(d + 1, data, n);
    to_port(to, d, n + 1);
    return 1;
}

static void send_members(void) {
    uint8_t d[1 + 8 * L_MAX];
    d[0] = L_MEMBERS;
    memcpy(d + 1, L.ids, (size_t)L.n * 8);
    for (int i = 1; i < L.n; i++) to_port(L.ids[i], d, 1 + (size_t)L.n * 8);
    L.members_sent = plat_now_ms();
}

static void drop(int i, void (*on)(const SteamEvent *, void *), void *user) {
    uint64_t id = L.ids[i];
    for (int k = i; k + 1 < L.n; k++) L.ids[k] = L.ids[k + 1], L.heard[k] = L.heard[k + 1];
    L.n--;
    on(&(SteamEvent){.kind = STEAM_EV_MEMBER_LEFT, .a = id}, user);
}

static void l_pump(void (*on)(const SteamEvent *e, void *user), void *user) {
    if (!L.udp) return;
    uint64_t now = plat_now_ms();
    if (L.pending_entered) {
        L.pending_entered = 0;
        on(&(SteamEvent){.kind = STEAM_EV_ROOM_ENTERED, .a = L.room}, user);
    }
    uint64_t maker = L.joining ? L.joining : L.room != L.self ? L.room : 0;
    if (maker && now - L.join_sent >= L_JOIN_EVERY_MS) to_port(maker, (uint8_t[]){L_JOIN}, 1), L.join_sent = now;
    if (L.joining && now - L.join_since > L_JOIN_WAIT_MS) {
        L.joining = 0;
        on(&(SteamEvent){.kind = STEAM_EV_ROOM_FAILED, .code = STEAM_FAIL_NO_ROOM}, user);
    }
    for (;;) {
        uint8_t d[1 + STEAM_MESSAGE_MAX + 8];
        PlatAddr from;
        int n = plat_udp_recv(L.udp, d, sizeof d, &from, 0);
        if (n <= 0) break;
        uint64_t who = plat_addr_port(&from);
        if (d[0] == L_JOIN && L.room == L.self) {
            int i = member(who);
            if (i < 0 && L.n < L_MAX) {
                i = L.n++;
                L.ids[i] = who, L.heard[i] = now;
                send_members();
                on(&(SteamEvent){.kind = STEAM_EV_MEMBER_JOINED, .a = who}, user);
            } else if (i >= 0) {
                L.heard[i] = now;
            }
        } else if (d[0] == L_LEAVE && L.room == L.self) {
            int i = member(who);
            if (i > 0) drop(i, on, user), send_members();
        } else if (d[0] == L_MEMBERS && (who == L.joining || (L.room && who == L.room && L.room != L.self))) {
            uint64_t ids[L_MAX];
            int count = (n - 1) / 8;
            if (count > L_MAX) count = L_MAX;
            memcpy(ids, d + 1, (size_t)count * 8);
            int entering = L.joining != 0;
            if (entering) L.room = L.joining, L.joining = 0, L.n = 0;
            // who left, then who came, against the list held
            for (int i = L.n - 1; i >= 0; i--) {
                int still = 0;
                for (int k = 0; k < count; k++) still |= ids[k] == L.ids[i];
                if (!still) drop(i, on, user);
            }
            for (int k = 0; k < count; k++) {
                if (member(ids[k]) >= 0 || L.n >= L_MAX) continue;
                L.ids[L.n++] = ids[k];
                if (!entering && ids[k] != L.self) on(&(SteamEvent){.kind = STEAM_EV_MEMBER_JOINED, .a = ids[k]}, user);
            }
            if (entering) on(&(SteamEvent){.kind = STEAM_EV_ROOM_ENTERED, .a = L.room}, user);
        } else if (d[0] == L_DATA && L.room && member(who) >= 0 && n > 1) {
            on(&(SteamEvent){.kind = STEAM_EV_MESSAGE, .a = who, .data = d + 1, .n = (size_t)n - 1}, user);
        }
        if (!L.udp) return;   // a handler stopped the stand-in
    }
    if (L.room == L.self) {
        for (int i = L.n - 1; i > 0; i--)
            if (now - L.heard[i] > L_SILENT_MS) drop(i, on, user), send_members();
        if (now - L.members_sent >= L_JOIN_EVERY_MS) send_members();
    }
}

const CoopNet coop_net_local = {l_start, l_stop, l_self, l_name_of, l_room_create, l_room_join, l_room_leave, l_room,
                                l_room_owner, l_room_members, l_room_set, l_room_get, l_send, l_pump};
