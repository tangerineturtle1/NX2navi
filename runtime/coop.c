// Co-op, the room (coop.h): one host thread that owns Steam (steam.c) and the room and talks to the other games.
//
// A room is a Steam lobby that carries the note nx2navi = 1; a lobby without it is somebody else's (the app id is
// shared with every program that uses Steam's test app) and is left at once. The lobby's owner is the host. Being in
// the lobby is not yet being in the room: a game that came says HELLO to the host with its protocol number and game
// build, the host answers WELCOME or REFUSED and lists the players it let in (ROSTER, at each change and every two
// seconds), and every game takes poses, chat and the game layer's messages only from the players on the host's list.
// So a player the host removed, or a game of another version, is in nobody's room though Steam still shows it in the
// lobby for a moment. When the host leaves, Steam makes another member the owner; that game carries on with the list
// it had.
//
// Messages are one kind byte and a body, in the byte order of the PCs this runs on (little-endian):
//   HELLO    u16 protocol, u32 game build            to the host, every second until it answers
//   WELCOME  u16 protocol                            the host's yes
//   REFUSED  u8 why (REFUSE_*)                       the host's no; the game leaves the lobby
//   ROSTER   u8 count, count x u64 id                the host's list, the host first
//   POSE     u32 number, the pose (pose_put)         unreliable, 20 a second while Link is in the world, 2 a second else
//   CHAT     text, UTF-8, at most COOP_CHAT_CAP      to everyone on the list
//   from COOP_GAME_KIND on: the game layer's (coop_game.c), passed through
// Steam vouches for the sender of every message (its id is the Steam account's) and encrypts them; nothing here adds
// to that. Every length is checked before a body is read, a float that is not finite ends the message, and a name or
// a chat line is shown with its control characters replaced.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop.h"
#include "coop_net.h"
#include "host.h"
#include "platform/platform.h"

enum {
    K_HELLO = 1, K_WELCOME, K_REFUSED, K_ROSTER, K_POSE, K_CHAT,
    REFUSE_VERSION = 1, REFUSE_REMOVED, REFUSE_FULL,
    GAME_BUILD = 0xe0f3ccfa,          // the game build this runtime's co-op knows (1.1.1)
    STEP_MS = 5,
    HELLO_EVERY_MS = 1000, HOST_WAIT_MS = 15000, ROSTER_EVERY_MS = 2000,
    POSE_EVERY_MS = 50, POSE_IDLE_MS = 500, POSE_STALE_MS = 3000,
    STEAM_RETRY_MS = 5000,
    COMMANDS = 16, OUTBOX = 256, REMOVED_MAX = 32,
    POSE_BYTES = 4 + 12 + 12 + 4 + 1 + 1,   // number, pos, vel, yaw, flags, scene length; then the scene
};

typedef enum { C_CREATE = 1, C_JOIN, C_JOIN_INVITE, C_DECLINE, C_LEAVE, C_RETRY, C_INVITE, C_INVITE_WINDOW, C_REMOVE, C_CHAT, C_FRIENDS } CmdKind;
typedef struct { CmdKind kind; uint64_t arg; char text[COOP_CHAT_CAP + 1]; } Cmd;
typedef struct { uint64_t to; uint16_t n; uint8_t reliable; uint8_t bytes[1 + COOP_MESSAGE_MAX]; } Out;

typedef struct {
    uint64_t id;
    char name[COOP_NAME_CAP];
    int has_pose;
    uint32_t pose_number;
    uint64_t pose_at;
    CoopPose pose;
} Player;

// The co-op thread's own state.
static struct {
    const CoopNet *net;
    int up;
    uint64_t retry_at;
    CoopState state;
    CoopNote note;
    char steam_says[160];
    uint64_t room, owner;
    int made;                     // this game made the room
    int welcomed;                 // the host let this game in (always, for the host itself)
    uint64_t entered_at, hello_at, roster_at, pose_sent_at;
    uint32_t pose_number;
    int n;
    Player players[COOP_PLAYERS];   // the host's list as this game knows it; this game is among them once welcomed
    int nremoved;
    uint64_t removed[REMOVED_MAX];
    uint64_t invite_room, invite_from;
    int nchat;
    CoopChatLine chat[COOP_CHAT_LINES];
    uint32_t friends_serial;
    int own_has;                  // this game's own pose, as last sent
    CoopPose own_pose;
} G;

// What the other threads share with it.
static struct {
    PlatMutex lock;
    PlatEvent stopped;
    volatile int32_t stop;
    volatile uint32_t serial;
    CoopView view;
    int ncmd;
    Cmd cmd[COMMANDS];
    int nout;
    Out *out;
    int has_pose;
    CoopPose pose;
    int nfriends;
    SteamFriend friends[COOP_FRIENDS];
    int in_room;   // for coop_post: only then is a message queued
} P;

// ---- text from outside: control characters and the characters that reorder a line are no part of a name or a chat line

static void clean_text(char *s) {
    for (unsigned char *p = (unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) *p = ' ';
        // U+202A..U+202E and U+2066..U+2069 (E2 80 AA..AE, E2 81 A6..A9): direction overrides and isolates
        if (p[0] == 0xe2 && ((p[1] == 0x80 && p[2] >= 0xaa && p[2] <= 0xae) || (p[1] == 0x81 && p[2] >= 0xa6 && p[2] <= 0xa9)))
            p[0] = p[1] = p[2] = ' ';
    }
}

// ---- the players

static Player *player_of(uint64_t id) {
    for (int i = 0; i < G.n; i++)
        if (G.players[i].id == id) return &G.players[i];
    return NULL;
}

static void name_player(Player *p) {
    if (p->name[0]) return;
    G.net->name_of(p->id, p->name, sizeof p->name);
    clean_text(p->name);
}

static Player *player_add(uint64_t id) {
    Player *p = player_of(id);
    if (p || G.n >= COOP_PLAYERS) return p;
    p = &G.players[G.n++];
    memset(p, 0, sizeof *p);
    p->id = id;
    name_player(p);
    return p;
}

static void chat_line(const char *name, const char *text, int notice, int joined) {
    if (G.nchat == COOP_CHAT_LINES) memmove(G.chat, G.chat + 1, sizeof G.chat[0] * (size_t)--G.nchat);
    CoopChatLine *l = &G.chat[G.nchat++];
    memset(l, 0, sizeof *l);
    snprintf(l->name, sizeof l->name, "%s", name);
    snprintf(l->text, sizeof l->text, "%s", text);
    l->at_ms = plat_now_ms(), l->notice = (uint8_t)notice, l->joined = (uint8_t)joined;
}

static void player_remove(uint64_t id) {
    for (int i = 0; i < G.n; i++) {
        if (G.players[i].id != id) continue;
        if (G.welcomed && id != G.net->self()) {
            rt_log("coop: %s left the room", G.players[i].name[0] ? G.players[i].name : "a player");
            chat_line(G.players[i].name, "", 1, 0);
        }
        memmove(G.players + i, G.players + i + 1, sizeof G.players[0] * (size_t)(G.n - i - 1));
        G.n--;
        return;
    }
}

static int is_removed(uint64_t id) {
    for (int i = 0; i < G.nremoved; i++)
        if (G.removed[i] == id) return 1;
    return 0;
}

static int hosting(void) { return G.room && G.owner == G.net->self(); }

// ---- sending

static void send_kind(uint64_t to, uint8_t kind, const void *body, size_t n, int reliable) {
    uint8_t d[1 + COOP_MESSAGE_MAX];
    if (n > COOP_MESSAGE_MAX) return;
    d[0] = kind;
    if (n) memcpy(d + 1, body, n);
    G.net->send(to, d, n + 1, reliable);
}

static void send_listed(uint8_t kind, const void *body, size_t n, int reliable) {
    for (int i = 0; i < G.n; i++)
        if (G.players[i].id != G.net->self()) send_kind(G.players[i].id, kind, body, n, reliable);
}

static void send_roster(void) {
    uint8_t d[1 + 8 * COOP_PLAYERS];
    d[0] = (uint8_t)G.n;
    for (int i = 0; i < G.n; i++) memcpy(d + 1 + 8 * i, &G.players[i].id, 8);
    send_listed(K_ROSTER, d, 1 + 8 * (size_t)G.n, 1);
    G.roster_at = plat_now_ms();
}

static size_t pose_put(uint8_t *d, uint32_t number, const CoopPose *p) {
    size_t sl = strnlen(p->scene, COOP_SCENE_CAP - 1);
    memcpy(d, &number, 4), memcpy(d + 4, p->pos, 12), memcpy(d + 16, p->vel, 12), memcpy(d + 28, &p->yaw, 4);
    d[32] = p->flags, d[33] = (uint8_t)sl;
    memcpy(d + POSE_BYTES, p->scene, sl);
    return POSE_BYTES + sl;
}

static int pose_get(const uint8_t *d, size_t n, uint32_t *number, CoopPose *p) {
    if (n < POSE_BYTES || d[33] >= COOP_SCENE_CAP || n != POSE_BYTES + (size_t)d[33]) return 0;
    memset(p, 0, sizeof *p);
    memcpy(number, d, 4), memcpy(p->pos, d + 4, 12), memcpy(p->vel, d + 16, 12), memcpy(&p->yaw, d + 28, 4);
    p->flags = d[32];
    memcpy(p->scene, d + POSE_BYTES, d[33]);
    for (int i = 0; i < 3; i++)
        if (!isfinite(p->pos[i]) || !isfinite(p->vel[i]) || fabsf(p->pos[i]) > 1e6f) return 0;
    if (!isfinite(p->yaw)) return 0;
    clean_text(p->scene);
    return 1;
}

// ---- the room

static void room_end(CoopNote note) {
    if (G.room || G.state == COOP_ENTERING) {
        G.net->room_leave();
        if (G.welcomed) coop_game_room(0);
        if (G.room) rt_log("coop: left the room");
    }
    G.room = G.owner = 0, G.made = G.welcomed = 0, G.n = 0, G.nremoved = 0;
    G.state = COOP_READY;
    G.note = note;
}

static void welcomed(void) {
    G.welcomed = 1;
    G.state = COOP_IN_ROOM;
    G.note = COOP_NOTE_NONE;
    player_add(G.net->self());
    coop_game_room(1);
}

static void on_entered(uint64_t room) {
    G.room = room;
    G.owner = G.net->room_owner();
    G.entered_at = plat_now_ms(), G.hello_at = 0;
    G.n = 0, G.nremoved = 0, G.welcomed = 0;
    if (G.made) {
        G.net->room_set("nx2navi", "1");
        welcomed();
        char code[STEAM_CODE_CAP];
        steam_code_make(room, code);
        rt_log("coop: room made%s", code[0] || G.net == &coop_net_local ? "" : " (it has no room id: invite through Steam)");
        return;
    }
    char ours[8];
    G.net->room_get("nx2navi", ours, sizeof ours);
    if (strcmp(ours, "1")) {
        rt_log("coop: that Steam lobby is no NX2navi room; left it");
        room_end(COOP_NOTE_NOT_OURS);
        return;
    }
    rt_log("coop: in the lobby; asking the host");
}

static void on_message(uint64_t from, const uint8_t *d, size_t n) {
    if (!G.room || n < 1) return;
    uint8_t kind = d[0];
    const uint8_t *b = d + 1;
    n--;
    int from_host = from == G.owner;
    if (kind == K_HELLO) {
        if (!hosting() || n != 6) return;
        uint16_t proto;
        uint32_t build;
        memcpy(&proto, b, 2), memcpy(&build, b + 2, 4);
        uint8_t why = proto != COOP_PROTOCOL || build != GAME_BUILD ? REFUSE_VERSION : is_removed(from) ? REFUSE_REMOVED
                    : !player_of(from) && G.n >= COOP_PLAYERS ? REFUSE_FULL : 0;
        if (why) {
            send_kind(from, K_REFUSED, &why, 1, 1);
            return;
        }
        int fresh = !player_of(from);
        Player *p = player_add(from);
        if (!p) return;
        uint16_t mine = COOP_PROTOCOL;
        send_kind(from, K_WELCOME, &mine, 2, 1);
        send_roster();
        if (fresh) {
            rt_log("coop: %s joined the room", p->name[0] ? p->name : "a player");
            chat_line(p->name, "", 1, 1);
            coop_game_joined(from);
        }
    } else if (kind == K_WELCOME) {
        if (from_host && !G.welcomed && n == 2) {
            player_add(from);
            welcomed();
            rt_log("coop: joined the room");
            coop_game_joined(from);
        }
    } else if (kind == K_REFUSED) {
        if (from_host && n == 1 && !hosting())
            room_end(b[0] == REFUSE_VERSION ? COOP_NOTE_OTHER_VERSION : b[0] == REFUSE_REMOVED ? COOP_NOTE_REMOVED
                     : b[0] == REFUSE_FULL ? COOP_NOTE_FULL : COOP_NOTE_REFUSED);
    } else if (kind == K_ROSTER) {
        if (!from_host || hosting() || !G.welcomed || n < 1 || b[0] > COOP_PLAYERS || n != 1 + 8 * (size_t)b[0]) return;
        uint64_t ids[COOP_PLAYERS];
        memcpy(ids, b + 1, 8 * (size_t)b[0]);
        int listed = 0;
        for (int i = 0; i < b[0]; i++) listed |= ids[i] == G.net->self();
        if (!listed) {   // the host took this game off its list
            room_end(COOP_NOTE_REMOVED);
            return;
        }
        for (int i = G.n - 1; i >= 0; i--) {
            int still = 0;
            for (int k = 0; k < b[0]; k++) still |= ids[k] == G.players[i].id;
            if (!still) player_remove(G.players[i].id);
        }
        for (int k = 0; k < b[0]; k++) {
            if (player_of(ids[k])) continue;
            Player *p = player_add(ids[k]);
            if (p && ids[k] != G.owner) {
                rt_log("coop: %s joined the room", p->name[0] ? p->name : "a player");
                chat_line(p->name, "", 1, 1);
            }
            if (p) coop_game_joined(ids[k]);
        }
    } else if (!G.welcomed || !player_of(from)) {
        return;   // everything below: only from a player on the host's list
    } else if (kind == K_POSE) {
        Player *p = player_of(from);
        uint32_t number;
        CoopPose pose;
        if (!pose_get(b, n, &number, &pose)) return;
        if (p->has_pose && (int32_t)(number - p->pose_number) <= 0) return;   // late or twice
        p->pose = pose, p->pose_number = number, p->pose_at = plat_now_ms(), p->has_pose = 1;
    } else if (kind == K_CHAT) {
        if (n < 1 || n > COOP_CHAT_CAP) return;
        char text[COOP_CHAT_CAP + 1];
        memcpy(text, b, n), text[n] = 0;
        clean_text(text);
        Player *p = player_of(from);
        name_player(p);
        chat_line(p->name, text, 0, 0);
    } else if (kind >= COOP_GAME_KIND) {
        coop_game_message(from, kind, b, n);
    }
}

static void on_event(const SteamEvent *e, void *user) {
    (void)user;
    switch (e->kind) {
    case STEAM_EV_ROOM_ENTERED: on_entered(e->a); break;
    case STEAM_EV_ROOM_FAILED:
        G.state = COOP_READY, G.made = 0;
        G.note = e->code == STEAM_FAIL_CREATE ? COOP_NOTE_CREATE_FAILED : e->code == STEAM_FAIL_NO_ROOM ? COOP_NOTE_NO_ROOM
               : e->code == STEAM_FAIL_FULL ? COOP_NOTE_FULL : COOP_NOTE_REFUSED;
        rt_log("coop: the room could not be %s (%d)", e->code == STEAM_FAIL_CREATE ? "made" : "joined", e->code);
        break;
    case STEAM_EV_MEMBER_JOINED: break;   // it says HELLO when it is one of ours
    case STEAM_EV_MEMBER_LEFT:
        if (player_of(e->a)) {
            player_remove(e->a);
            if (hosting()) send_roster();
        }
        break;
    case STEAM_EV_INVITED:
        if (!G.room && G.state != COOP_ENTERING) G.invite_room = e->a, G.invite_from = e->b;
        break;
    case STEAM_EV_JOIN_ASKED:
        // the player said yes in Steam itself: leave the room this game is in and go there
        if (e->a != G.room) {
            if (G.room || G.state == COOP_ENTERING) room_end(COOP_NOTE_NONE);
            G.made = 0, G.invite_room = 0;
            if (G.net->room_join(e->a)) G.state = COOP_ENTERING, G.entered_at = plat_now_ms();
        }
        break;
    case STEAM_EV_MESSAGE: on_message(e->a, e->data, e->n); break;
    case STEAM_EV_LOST:
        rt_log("coop: Steam lost its connection; the room is gone");
        if (G.welcomed) coop_game_room(0);
        G.room = G.owner = 0, G.made = G.welcomed = 0, G.n = 0;
        G.state = COOP_READY, G.note = COOP_NOTE_LOST;
        break;
    }
}

// ---- the thread's step

static void run_command(const Cmd *c) {
    if (!G.up) {
        if (c->kind == C_RETRY) G.retry_at = 0;
        return;
    }
    switch (c->kind) {
    case C_CREATE:
        if (G.room || G.state == COOP_ENTERING) break;
        if (G.net->room_create(COOP_PLAYERS)) G.state = COOP_ENTERING, G.made = 1, G.note = COOP_NOTE_NONE, G.entered_at = plat_now_ms();
        else G.note = COOP_NOTE_CREATE_FAILED;
        break;
    case C_JOIN:
    case C_JOIN_INVITE: {
        uint64_t room = c->kind == C_JOIN ? c->arg : G.invite_room;
        G.invite_room = 0;
        if (!room || G.room || G.state == COOP_ENTERING) break;
        if (G.net->room_join(room)) G.state = COOP_ENTERING, G.made = 0, G.note = COOP_NOTE_NONE, G.entered_at = plat_now_ms();
        else G.note = COOP_NOTE_NO_ROOM;
        break;
    }
    case C_DECLINE: G.invite_room = 0; break;
    case C_LEAVE:
        if (G.room || G.state == COOP_ENTERING) room_end(COOP_NOTE_LEFT);
        break;
    case C_RETRY: break;
    case C_INVITE:
        if (G.room && !coop_local_wanted()) steam_invite(c->arg);
        break;
    case C_INVITE_WINDOW:
        if (G.room && !coop_local_wanted()) steam_invite_window();
        break;
    case C_REMOVE:
        if (!hosting() || c->arg == G.net->self() || !player_of(c->arg)) break;
        if (G.nremoved < REMOVED_MAX) G.removed[G.nremoved++] = c->arg;
        send_kind(c->arg, K_REFUSED, (uint8_t[]){REFUSE_REMOVED}, 1, 1);
        player_remove(c->arg);
        send_roster();
        break;
    case C_CHAT: {
        size_t n = strlen(c->text);
        if (!G.welcomed || !n) break;
        send_listed(K_CHAT, c->text, n, 1);
        Player *me = player_of(G.net->self());
        if (me) name_player(me), chat_line(me->name, c->text, 0, 0);
        break;
    }
    case C_FRIENDS:
        if (!coop_local_wanted()) {
            static SteamFriend list[COOP_FRIENDS];
            int n = steam_friends(list, COOP_FRIENDS);
            plat_mutex_lock(&P.lock);
            memcpy(P.friends, list, sizeof list[0] * (size_t)n);
            P.nfriends = n;
            plat_mutex_unlock(&P.lock);
            G.friends_serial++;
        }
        break;
    }
}

static void tick(uint64_t now) {
    if (!G.room) {
        if (G.state == COOP_ENTERING && now - G.entered_at > HOST_WAIT_MS) room_end(COOP_NOTE_NO_ROOM);
        return;
    }
    // the owner changed: the host left and Steam picked another member
    uint64_t owner = G.net->room_owner();
    if (owner && owner != G.owner) {
        G.owner = owner;
        if (hosting() && G.welcomed) {
            rt_log("coop: the host left; this game is the host now");
            send_roster();
        }
    }
    if (!G.welcomed) {
        if (now - G.entered_at > HOST_WAIT_MS) {
            rt_log("coop: the room's host did not answer");
            room_end(COOP_NOTE_HOST_SILENT);
        } else if (now - G.hello_at >= HELLO_EVERY_MS) {
            uint8_t d[6];
            uint16_t proto = COOP_PROTOCOL;
            uint32_t build = GAME_BUILD;
            memcpy(d, &proto, 2), memcpy(d + 2, &build, 4);
            send_kind(G.owner, K_HELLO, d, sizeof d, 1);
            G.hello_at = now;
        }
        return;
    }
    if (hosting() && now - G.roster_at >= ROSTER_EVERY_MS) send_roster();
    // a member Steam no longer lists is gone, whatever message was missed
    uint64_t ids[STEAM_ROOM_MAX];
    int nm = G.net->room_members(ids, STEAM_ROOM_MAX);
    for (int i = G.n - 1; i >= 0; i--) {
        int there = G.players[i].id == G.net->self();
        for (int k = 0; k < nm; k++) there |= ids[k] == G.players[i].id;
        if (!there) {
            player_remove(G.players[i].id);
            if (hosting()) send_roster();
        }
    }
    for (int i = 0; i < G.n; i++) name_player(&G.players[i]);
    // this game's own pose
    plat_mutex_lock(&P.lock);
    int has = P.has_pose;
    CoopPose pose = P.pose;
    plat_mutex_unlock(&P.lock);
    if (!has) memset(&pose, 0, sizeof pose);
    G.own_has = has, G.own_pose = pose;
    // NX2NAVI_COOP_TRACE=1 (tests, environment only): every two seconds where this game's Link is and where it has the others
    static int trace = -1;
    static uint64_t traced_at;
    if (trace < 0) {
        char v[8];
        trace = plat_getenv("NX2NAVI_COOP_TRACE", v, sizeof v) && v[0] == '1';
    }
    if (trace && now - traced_at >= 2000) {
        traced_at = now;
        rt_log("coop: trace: own %s %.1f %.1f %.1f yaw %.2f; %d in the room", has ? pose.scene : "(not in the world)", pose.pos[0],
               pose.pos[1], pose.pos[2], pose.yaw, G.n);
        for (int i = 0; i < G.n; i++) {
            const Player *p = &G.players[i];
            if (p->id == G.net->self()) continue;
            rt_log("coop: trace: %s %s %.1f %.1f %.1f, %llu ms ago", p->name, p->has_pose ? p->pose.scene : "(no pose)", p->pose.pos[0],
                   p->pose.pos[1], p->pose.pos[2], (unsigned long long)(p->has_pose ? now - p->pose_at : 0));
        }
    }
    if (now - G.pose_sent_at >= (uint64_t)(has ? POSE_EVERY_MS : POSE_IDLE_MS)) {
        uint8_t d[POSE_BYTES + COOP_SCENE_CAP];
        size_t n = pose_put(d, ++G.pose_number, &pose);
        send_listed(K_POSE, d, n, 0);
        G.pose_sent_at = now;
    }
}

static void drain_outbox(void) {
    static Out batch[OUTBOX];
    plat_mutex_lock(&P.lock);
    int n = P.nout;
    if (n) memcpy(batch, P.out, sizeof batch[0] * (size_t)n);
    P.nout = 0;
    plat_mutex_unlock(&P.lock);
    for (int i = 0; i < n && G.welcomed; i++) {
        if (!batch[i].to) {
            for (int k = 0; k < G.n; k++)
                if (G.players[k].id != G.net->self()) G.net->send(G.players[k].id, batch[i].bytes, batch[i].n, batch[i].reliable);
        } else if (player_of(batch[i].to)) {
            G.net->send(batch[i].to, batch[i].bytes, batch[i].n, batch[i].reliable);
        }
    }
}

static void publish(uint64_t now) {
    static CoopView v;
    memset(&v, 0, sizeof v);
    v.state = G.state, v.note = G.note;
    snprintf(v.steam_says, sizeof v.steam_says, "%s", G.steam_says);
    v.local = G.net == &coop_net_local;
    if (G.up && G.room && G.welcomed) {
        if (v.local) snprintf(v.code, sizeof v.code, "%llu", (unsigned long long)G.room);
        else steam_code_make(G.room, v.code);
        v.host = (uint8_t)hosting();
        v.overlay = (uint8_t)(!v.local && steam_overlay());
        for (int i = 0; i < G.n; i++) {
            CoopPlayer *o = &v.players[v.n++];
            const Player *p = &G.players[i];
            o->id = p->id;
            snprintf(o->name, sizeof o->name, "%s", p->name);
            o->self = p->id == G.net->self(), o->host = p->id == G.owner;
            if (o->self) {
                o->has_pose = (uint8_t)G.own_has, o->pose = G.own_pose;
            } else {
                o->has_pose = (uint8_t)(p->has_pose && now - p->pose_at < POSE_STALE_MS);
                o->pose_ms = o->has_pose ? (uint32_t)(now - p->pose_at) : 0;
                o->pose = p->pose;
            }
        }
    }
    if (G.invite_room) {
        v.invite_room = G.invite_room;
        G.net->name_of(G.invite_from, v.invite_from, sizeof v.invite_from);
        clean_text(v.invite_from);
    }
    v.nchat = G.nchat;
    memcpy(v.chat, G.chat, sizeof G.chat[0] * (size_t)G.nchat);
    v.friends_serial = G.friends_serial;
    plat_mutex_lock(&P.lock);
    if (memcmp(&P.view, &v, sizeof v)) {
        P.view = v;
        __atomic_add_fetch(&P.serial, 1, __ATOMIC_RELEASE);
    }
    P.in_room = G.welcomed;
    plat_mutex_unlock(&P.lock);
}

static const CoopNet net_steam = {steam_start, steam_stop, steam_self, steam_name_of, steam_room_create, steam_room_join,
                                  steam_room_leave, steam_room, steam_room_owner, steam_room_members, steam_room_set,
                                  steam_room_get, steam_send, steam_pump};

static void net_down(void) {
    if (!G.up) return;
    room_end(COOP_NOTE_NONE);
    G.net->stop();
    G.up = 0;
    rt_log("coop: off");
}

static void coop_main(void *arg) {
    (void)arg;
    unsigned cfg_seen = ~0u;
    int on = 0;
    while (!__atomic_load_n(&P.stop, __ATOMIC_ACQUIRE)) {
        uint64_t now = plat_now_ms();
        unsigned v = cfg_version();
        if (v != cfg_seen) cfg_seen = v, on = cfg_bool("NX2NAVI_COOP_STEAM") && RT_V111;
        if (!on) {
            net_down();
            if (G.state != COOP_OFF) G.state = COOP_OFF, G.note = COOP_NOTE_NONE, publish(now);
            plat_sleep_ms(200);
            continue;
        }
        // commands, also while Steam is not up: Retry is one
        Cmd cmds[COMMANDS];
        plat_mutex_lock(&P.lock);
        int nc = P.ncmd;
        memcpy(cmds, P.cmd, sizeof cmds[0] * (size_t)nc);
        P.ncmd = 0;
        plat_mutex_unlock(&P.lock);
        for (int i = 0; i < nc; i++) run_command(&cmds[i]);
        if (!G.up) {
            if (now >= G.retry_at) {
                G.net = coop_local_wanted() ? &coop_net_local : &net_steam;
                int which = 0;
                char why[160] = "";
                if (G.net->start(why, sizeof why, &which)) {
                    G.up = 1, G.state = COOP_READY, G.note = COOP_NOTE_NONE, G.steam_says[0] = 0;
                    rt_log("coop: on%s", G.net == &coop_net_local ? " (the test stand-in for Steam)" : ", signed in to Steam as app 480");
                    if (G.net == &coop_net_local) {
                        if (coop_local_join()) run_command(&(Cmd){.kind = C_JOIN, .arg = coop_local_join()});
                        else if (coop_local_makes()) run_command(&(Cmd){.kind = C_CREATE});
                    }
                } else {
                    CoopNote note = which == STEAM_WHY_NO_LIBRARY ? COOP_NOTE_NO_LIBRARY : which == STEAM_WHY_OLD_LIBRARY ? COOP_NOTE_OLD_LIBRARY
                                  : which == STEAM_WHY_OTHER_OS ? COOP_NOTE_OTHER_OS : COOP_NOTE_NO_CLIENT;
                    if (G.state != COOP_NO_STEAM || G.note != note) rt_log("coop: Steam is not available: %s", why);
                    G.state = COOP_NO_STEAM, G.note = note;
                    snprintf(G.steam_says, sizeof G.steam_says, "%s", why);
                    G.retry_at = now + STEAM_RETRY_MS;
                }
            }
            publish(now);
            plat_sleep_ms(G.up ? STEP_MS : 100);
            continue;
        }
        G.net->pump(on_event, NULL);
        tick(now);
        drain_outbox();
        publish(now);
        plat_sleep_ms(STEP_MS);
    }
    net_down();
    plat_event_set(&P.stopped);
}

// ---- for the other threads

static int started;

void coop_init(void) {
    plat_mutex_init(&P.lock);
    plat_event_init(&P.stopped, 1, 0);
    P.out = calloc(OUTBOX, sizeof *P.out);
    if (!P.out || !plat_thread_spawn(coop_main, NULL, "coop", 0)) {
        rt_log("coop: cannot start the co-op thread");
        return;
    }
    started = 1;
}

void coop_shutdown(void) {
    if (!started) return;
    started = 0;
    __atomic_store_n(&P.stop, 1, __ATOMIC_RELEASE);
    plat_event_wait_ns(&P.stopped, 1500ull * 1000000);   // leaving a lobby is one call to Steam; do not hold the exit for it
}

uint32_t coop_serial(void) { return __atomic_load_n(&P.serial, __ATOMIC_ACQUIRE); }

void coop_view(CoopView *out) {
    if (!started) {
        memset(out, 0, sizeof *out);
        return;
    }
    plat_mutex_lock(&P.lock);
    *out = P.view;
    plat_mutex_unlock(&P.lock);
}

static void command(CmdKind kind, uint64_t arg, const char *text) {
    if (!started) return;
    plat_mutex_lock(&P.lock);
    if (P.ncmd < COMMANDS) {
        Cmd *c = &P.cmd[P.ncmd++];
        memset(c, 0, sizeof *c);
        c->kind = kind, c->arg = arg;
        if (text) snprintf(c->text, sizeof c->text, "%s", text);
    }
    plat_mutex_unlock(&P.lock);
}

void coop_create(void) { command(C_CREATE, 0, NULL); }
int coop_join(const char *code) {
    uint64_t room = steam_code_parse(code);
    if (!room) return 0;
    command(C_JOIN, room, NULL);
    return 1;
}
void coop_join_invite(void) { command(C_JOIN_INVITE, 0, NULL); }
void coop_decline_invite(void) { command(C_DECLINE, 0, NULL); }
void coop_leave(void) { command(C_LEAVE, 0, NULL); }
void coop_retry(void) { command(C_RETRY, 0, NULL); }
void coop_invite(uint64_t friend_id) { command(C_INVITE, friend_id, NULL); }
void coop_invite_window(void) { command(C_INVITE_WINDOW, 0, NULL); }
void coop_remove(uint64_t player_id) { command(C_REMOVE, player_id, NULL); }
void coop_chat(const char *text) { command(C_CHAT, 0, text); }
void coop_friends_refresh(void) { command(C_FRIENDS, 0, NULL); }

int coop_friends(SteamFriend *out, int max) {
    if (!started) return 0;
    plat_mutex_lock(&P.lock);
    int n = P.nfriends < max ? P.nfriends : max;
    memcpy(out, P.friends, sizeof out[0] * (size_t)n);
    plat_mutex_unlock(&P.lock);
    return n;
}

void coop_pose_set(const CoopPose *pose) {
    if (!started) return;
    plat_mutex_lock(&P.lock);
    P.has_pose = pose != NULL;
    if (pose) P.pose = *pose;
    plat_mutex_unlock(&P.lock);
}

int coop_post(uint64_t to, uint8_t kind, const void *data, size_t n, int reliable) {
    if (!started || kind < COOP_GAME_KIND || n > COOP_MESSAGE_MAX) return 0;
    int ok = 0;
    plat_mutex_lock(&P.lock);
    if (P.in_room && P.nout < OUTBOX) {
        Out *o = &P.out[P.nout++];
        o->to = to, o->n = (uint16_t)(n + 1), o->reliable = (uint8_t)reliable;
        o->bytes[0] = kind;
        if (n) memcpy(o->bytes + 1, data, n);
        ok = 1;
    }
    plat_mutex_unlock(&P.lock);
    return ok;
}
