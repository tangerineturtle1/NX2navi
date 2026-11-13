// Steam for co-op (steam.h): the Steamworks library loaded at run time and used through its C entry points.
//
// The library. steam_api64.dll exports one plain C function per method of each interface ("the flat API"):
// SteamAPI_ISteamMatchmaking_CreateLobby(self, ...), with self from an accessor whose name carries the interface's
// version (SteamAPI_SteamMatchmaking_v009). A library of another age exports other accessor versions, so each accessor
// is looked for under the versions known to have the calls below with these parameters, newest first; a library that
// lacks one of them, the manual dispatch calls or the networking messages interface (before 2020) is refused as too
// old. Nothing of the library's headers is used: the declarations here are the documented parameters of each call, a
// CSteamID passed and returned as the 64-bit number it is, a bool as one byte.
//
// The app id. The library signs in as the app SteamAppId names in the environment, set here to 480 before it loads:
// Steam's test app, which every account owns. A game started from Steam as a non-Steam shortcut has SteamGameId set
// to the shortcut's own number; both are set, so the library does not take the shortcut for the app.
//
// Callbacks. With manual dispatch the library queues what happened and steam_pump takes it out: a callback is a
// number and a block of bytes whose layout is fixed per number (8-byte packing on Windows). The numbers and offsets
// used are in the enum below; an asynchronous call (CreateLobby, JoinLobby) ends in callback 703, which names the
// call and the callback that holds its result.
//
// Messages. ISteamNetworkingMessages sends datagrams to a Steam id; Steam opens the session, finds a direct path or
// relays through its own network, encrypts, and vouches for the sender's id. A session a stranger opens is accepted
// only when that id is in the room, and a message is handed out only when its sender is.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"
#include "steam.h"

#ifdef _WIN32
#define STEAM_LIB "steam_api64.dll"
#else
#define STEAM_LIB "libsteam_api.so"
#endif

enum {
    CB_SERVERS_DISCONNECTED = 103,
    CB_LOBBY_JOIN_REQUESTED = 333,   // u64 lobby, u64 friend
    CB_SHUTDOWN = 704,               // Steam is closing
    CB_CALL_COMPLETED = 703,         // u64 call, i32 callback, u32 size
    CB_LOBBY_INVITE = 503,           // u64 user, u64 lobby, u64 game
    CB_LOBBY_ENTER = 504,            // u64 lobby, u32 permissions, u8 locked, u32 response at 16
    CB_LOBBY_CHAT_UPDATE = 506,      // u64 lobby, u64 user changed, u64 user making the change, u32 change at 24
    CB_LOBBY_CREATED = 513,          // i32 result, u64 lobby at 8
    CB_SESSION_REQUEST = 1251,       // the identity of who asks
    RESULT_OK = 1,
    ENTER_OK = 1, ENTER_NO_ROOM = 2, ENTER_NOT_ALLOWED = 3, ENTER_FULL = 4,
    CHANGE_ENTERED = 1,              // the other bits: left, disconnected, kicked, banned
    LOBBY_FRIENDS_ONLY = 1,          // friends see and may join it; anyone else needs an invite or its number
    FRIENDS_IMMEDIATE = 4,           // the flag for "the player's friends"
    SEND_UNRELIABLE_NO_DELAY = 5, SEND_RELIABLE = 8, SEND_AUTO_RESTART = 32,
    IDENTITY_SIZE = 136,             // SteamNetworkingIdentity
    MSG_DATA = 0, MSG_SIZE = 8, MSG_PEER = 16,   // SteamNetworkingMessage_t: void *data, i32 size, u32 conn, identity
    CHANNEL = 0,
    RECEIVE_BATCH = 32,
    CALL_RESULT_CAP = 64,
};

typedef struct { int32_t user, id; uint8_t *param; int32_t size; } CallbackMsg;

static struct {
    void *lib;
    int up;
    int32_t pipe;
    void *user, *friends, *mm, *utils, *msgs;
    uint64_t self, room, call;   // call: the CreateLobby or JoinLobby that has not answered yet
    int creating;
    // the library
    void (*shutdown)(void);
    int32_t (*get_pipe)(void);
    void (*md_frame)(int32_t);
    uint8_t (*md_next)(int32_t, CallbackMsg *);
    void (*md_free)(int32_t);
    uint8_t (*md_result)(int32_t, uint64_t, void *, int, int, uint8_t *);
    uint64_t (*get_id)(void *);
    uint8_t (*overlay)(void *);
    const char *(*persona_of)(void *, uint64_t);
    const char *(*persona)(void *);
    int (*friend_count)(void *, int);
    uint64_t (*friend_at)(void *, int, int);
    int (*friend_state)(void *, uint64_t);
    uint8_t (*friend_game)(void *, uint64_t, void *);
    uint8_t (*has_friend)(void *, uint64_t, int);
    void (*invite_dialog)(void *, uint64_t);
    uint64_t (*create)(void *, int, int);
    uint64_t (*join)(void *, uint64_t);
    void (*leave)(void *, uint64_t);
    uint8_t (*invite)(void *, uint64_t, uint64_t);
    uint8_t (*set_data)(void *, uint64_t, const char *, const char *);
    const char *(*get_data)(void *, uint64_t, const char *);
    int (*member_count)(void *, uint64_t);
    uint64_t (*member_at)(void *, uint64_t, int);
    uint64_t (*owner)(void *, uint64_t);
    int (*send)(void *, const void *, const void *, uint32_t, int, int);
    int (*receive)(void *, int, void **, int);
    uint8_t (*accept)(void *, const void *);
    uint8_t (*close_session)(void *, const void *);
    void (*msg_release)(void *);
    void (*ident_clear)(void *);
    void (*ident_set)(void *, uint64_t);
    uint64_t (*ident_get)(void *);
} S;

static void *sym(const char *name) { return plat_lib_sym(S.lib, name); }

// The accessor of an interface under the first of the version names the library has; NULL: none.
static void *interface_of(const char *const *names) {
    for (; *names; names++) {
        void *(*get)(void) = sym(*names);
        if (get) return get();
    }
    return NULL;
}

static int fail(char *why, size_t cap, int *which, int code, const char *text) {
    snprintf(why, cap, "%s", text);
    if (which) *which = code;
    return 0;
}

int steam_up(void) { return S.up; }
uint64_t steam_self(void) { return S.self; }
uint64_t steam_room(void) { return S.room; }

int steam_start(char *why, size_t cap, int *which) {
    if (S.up) return 1;
    if (which) *which = STEAM_WHY_OK;
    if (!S.lib) {
        // beside the game's exe, nowhere else: a path from a settings file would load whatever file a stranger's
        // nx2navi.ini names
        char path[PLAT_PATH_MAX], exe[PLAT_PATH_MAX];
        if (!plat_exe_path(exe, sizeof exe)) return fail(why, cap, which, STEAM_WHY_NO_LIBRARY, "the game's folder is not known");
        char *sep = plat_last_sep(exe);
        if (sep) *sep = 0;
        snprintf(path, sizeof path, "%s/%s", exe, STEAM_LIB);
        if (!plat_file_exists(path))
            return fail(why, cap, which, STEAM_WHY_NO_LIBRARY, STEAM_LIB " is not beside the game");
        plat_setenv("SteamAppId", "480");
        plat_setenv("SteamGameId", "480");
        S.lib = plat_lib_open(path);
        if (!S.lib) return fail(why, cap, which, STEAM_WHY_NO_LIBRARY, STEAM_LIB " beside the game does not load");
    }
#define NEED(field, name) \
    if (!(S.field = sym(name))) return fail(why, cap, which, STEAM_WHY_OLD_LIBRARY, STEAM_LIB " is too old: it lacks " name);
    void (*md_init)(void) = sym("SteamAPI_ManualDispatch_Init");
    if (!md_init) return fail(why, cap, which, STEAM_WHY_OLD_LIBRARY, STEAM_LIB " is too old: it lacks SteamAPI_ManualDispatch_Init");
    NEED(shutdown, "SteamAPI_Shutdown") NEED(get_pipe, "SteamAPI_GetHSteamPipe")
    NEED(md_frame, "SteamAPI_ManualDispatch_RunFrame") NEED(md_next, "SteamAPI_ManualDispatch_GetNextCallback")
    NEED(md_free, "SteamAPI_ManualDispatch_FreeLastCallback") NEED(md_result, "SteamAPI_ManualDispatch_GetAPICallResult")
    NEED(get_id, "SteamAPI_ISteamUser_GetSteamID") NEED(overlay, "SteamAPI_ISteamUtils_IsOverlayEnabled")
    NEED(persona_of, "SteamAPI_ISteamFriends_GetFriendPersonaName") NEED(persona, "SteamAPI_ISteamFriends_GetPersonaName")
    NEED(friend_count, "SteamAPI_ISteamFriends_GetFriendCount") NEED(friend_at, "SteamAPI_ISteamFriends_GetFriendByIndex")
    NEED(friend_state, "SteamAPI_ISteamFriends_GetFriendPersonaState") NEED(friend_game, "SteamAPI_ISteamFriends_GetFriendGamePlayed")
    NEED(has_friend, "SteamAPI_ISteamFriends_HasFriend") NEED(invite_dialog, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog")
    NEED(create, "SteamAPI_ISteamMatchmaking_CreateLobby") NEED(join, "SteamAPI_ISteamMatchmaking_JoinLobby")
    NEED(leave, "SteamAPI_ISteamMatchmaking_LeaveLobby") NEED(invite, "SteamAPI_ISteamMatchmaking_InviteUserToLobby")
    NEED(set_data, "SteamAPI_ISteamMatchmaking_SetLobbyData") NEED(get_data, "SteamAPI_ISteamMatchmaking_GetLobbyData")
    NEED(member_count, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers") NEED(member_at, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex")
    NEED(owner, "SteamAPI_ISteamMatchmaking_GetLobbyOwner")
    NEED(send, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser") NEED(receive, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel")
    NEED(accept, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser") NEED(close_session, "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser")
    NEED(msg_release, "SteamAPI_SteamNetworkingMessage_t_Release") NEED(ident_clear, "SteamAPI_SteamNetworkingIdentity_Clear")
    NEED(ident_set, "SteamAPI_SteamNetworkingIdentity_SetSteamID64") NEED(ident_get, "SteamAPI_SteamNetworkingIdentity_GetSteamID64")
#undef NEED
    // signing in: the newest entry point says why it failed, the older ones only that they did
    int (*init_flat)(char *) = sym("SteamAPI_InitFlat");
    uint8_t (*init_safe)(void) = sym("SteamAPI_InitSafe");
    uint8_t (*init_old)(void) = sym("SteamAPI_Init");
    char err[1024] = "";
    int ok = init_flat ? init_flat(err) == 0 : init_safe ? init_safe() : init_old ? init_old() : 0;
    if (!ok) {
        char text[200];
        snprintf(text, sizeof text, "Steam did not answer: is it running and signed in?%s%.100s", *err ? " Steam says: " : "", err);
        return fail(why, cap, which, STEAM_WHY_NO_CLIENT, text);
    }
    md_init();
    S.user = interface_of((const char *const[]){"SteamAPI_SteamUser_v023", "SteamAPI_SteamUser_v022", "SteamAPI_SteamUser_v021",
                                                "SteamAPI_SteamUser_v020", NULL});
    S.friends = interface_of((const char *const[]){"SteamAPI_SteamFriends_v018", "SteamAPI_SteamFriends_v017", NULL});
    S.mm = interface_of((const char *const[]){"SteamAPI_SteamMatchmaking_v009", NULL});
    S.utils = interface_of((const char *const[]){"SteamAPI_SteamUtils_v010", "SteamAPI_SteamUtils_v009", NULL});
    S.msgs = interface_of((const char *const[]){"SteamAPI_SteamNetworkingMessages_SteamAPI_v002", NULL});
    if (!S.user || !S.friends || !S.mm || !S.utils || !S.msgs) {
        S.shutdown();
        return fail(why, cap, which, STEAM_WHY_OLD_LIBRARY, STEAM_LIB " is too old or too new: an interface this version knows is missing");
    }
    // Steam's relay network takes a few seconds to measure its routes: started now, the first message does not wait
    void *nu = interface_of((const char *const[]){"SteamAPI_SteamNetworkingUtils_SteamAPI_v004",
                                                  "SteamAPI_SteamNetworkingUtils_SteamAPI_v003", NULL});
    void (*relay_init)(void *) = sym("SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
    if (nu && relay_init) relay_init(nu);
    S.pipe = S.get_pipe();
    S.self = S.get_id(S.user);
    S.room = S.call = 0;
    S.up = 1;
    return 1;
}

void steam_stop(void) {
    if (!S.up) return;
    steam_room_leave();
    S.shutdown();
    S.up = 0;
}

void steam_name_of(uint64_t id, char *out, size_t cap) {
    const char *name = !S.up ? "" : id == S.self ? S.persona(S.friends) : S.persona_of(S.friends, id);
    // Steam answers "[unknown]" for a player whose name it has not fetched yet
    snprintf(out, cap, "%s", name && strcmp(name, "[unknown]") ? name : "");
}

int steam_is_friend(uint64_t id) { return S.up && S.has_friend(S.friends, id, FRIENDS_IMMEDIATE); }
int steam_overlay(void) { return S.up && S.overlay(S.utils); }

int steam_room_create(int max_players) {
    if (!S.up || S.room || S.call) return 0;
    S.call = S.create(S.mm, LOBBY_FRIENDS_ONLY, max_players);
    S.creating = 1;
    return S.call != 0;
}

int steam_room_join(uint64_t room) {
    if (!S.up || S.room || S.call || !room) return 0;
    S.call = S.join(S.mm, room);
    S.creating = 0;
    return S.call != 0;
}

static void identity_of(uint8_t ident[IDENTITY_SIZE], uint64_t id) {
    S.ident_clear(ident);
    S.ident_set(ident, id);
}

void steam_room_leave(void) {
    if (!S.up || !S.room) return;
    uint64_t ids[STEAM_ROOM_MAX];
    int n = steam_room_members(ids, STEAM_ROOM_MAX);
    for (int i = 0; i < n; i++) {
        if (ids[i] == S.self) continue;
        uint8_t ident[IDENTITY_SIZE];
        identity_of(ident, ids[i]);
        S.close_session(S.msgs, ident);
    }
    S.leave(S.mm, S.room);
    S.room = 0;
}

uint64_t steam_room_owner(void) { return S.up && S.room ? S.owner(S.mm, S.room) : 0; }

int steam_room_members(uint64_t *out, int max) {
    if (!S.up || !S.room) return 0;
    int n = S.member_count(S.mm, S.room);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = S.member_at(S.mm, S.room, i);
    return n;
}

static int in_room(uint64_t id) {
    uint64_t ids[STEAM_ROOM_MAX];
    int n = steam_room_members(ids, STEAM_ROOM_MAX);
    for (int i = 0; i < n; i++)
        if (ids[i] == id) return 1;
    return 0;
}

int steam_room_set(const char *key, const char *value) { return S.up && S.room && S.set_data(S.mm, S.room, key, value); }

void steam_room_get(const char *key, char *out, size_t cap) {
    const char *v = S.up && S.room ? S.get_data(S.mm, S.room, key) : NULL;
    snprintf(out, cap, "%s", v ? v : "");
}

static int friend_rank(const SteamFriend *f) { return f->in_app ? 0 : f->online ? 1 : 2; }
static int friend_cmp(const void *a, const void *b) {
    const SteamFriend *x = a, *y = b;
    int d = friend_rank(x) - friend_rank(y);
    return d ? d : strcmp(x->name, y->name);
}

int steam_friends(SteamFriend *out, int max) {
    if (!S.up) return 0;
    int total = S.friend_count(S.friends, FRIENDS_IMMEDIATE), n = 0;
    for (int i = 0; i < total && n < max; i++) {
        SteamFriend *f = &out[n];
        f->id = S.friend_at(S.friends, i, FRIENDS_IMMEDIATE);
        steam_name_of(f->id, f->name, sizeof f->name);
        if (!f->name[0]) continue;
        f->online = S.friend_state(S.friends, f->id) != 0;
        // FriendGameInfo_t: u64 game id (the app in its low 24 bits), u32 address, u16 and u16 ports, u64 lobby
        uint8_t game[24] = {0};
        uint64_t game_id = 0;
        if (S.friend_game(S.friends, f->id, game)) memcpy(&game_id, game, 8);
        f->in_app = (game_id & 0xffffff) == STEAM_APP_ID;
        n++;
    }
    qsort(out, (size_t)n, sizeof *out, friend_cmp);
    return n;
}

int steam_invite(uint64_t friend_id) { return S.up && S.room && S.invite(S.mm, S.room, friend_id); }
void steam_invite_window(void) {
    if (S.up && S.room) S.invite_dialog(S.friends, S.room);
}

int steam_send(uint64_t to, const void *data, size_t n, int reliable) {
    if (!S.up || !S.room || n > STEAM_MESSAGE_MAX) return 0;
    uint8_t ident[IDENTITY_SIZE];
    identity_of(ident, to);
    // a session that broke (the other player's connection dropped for a moment) is opened again by the next message
    int flags = (reliable ? SEND_RELIABLE : SEND_UNRELIABLE_NO_DELAY) | SEND_AUTO_RESTART;
    return S.send(S.msgs, ident, data, (uint32_t)n, flags, CHANNEL) == RESULT_OK;
}

static uint64_t u64_at(const uint8_t *p, size_t off) {
    uint64_t v;
    memcpy(&v, p + off, 8);
    return v;
}
static uint32_t u32_at(const uint8_t *p, size_t off) {
    uint32_t v;
    memcpy(&v, p + off, 4);
    return v;
}

static void entered(uint64_t room, uint32_t response, void (*on)(const SteamEvent *, void *), void *user) {
    S.call = 0;
    if (response == ENTER_OK) {
        S.room = room;
        on(&(SteamEvent){.kind = STEAM_EV_ROOM_ENTERED, .a = room}, user);
    } else {
        int code = response == ENTER_NO_ROOM ? STEAM_FAIL_NO_ROOM : response == ENTER_FULL ? STEAM_FAIL_FULL
                 : response == ENTER_NOT_ALLOWED ? STEAM_FAIL_REFUSED : STEAM_FAIL_OTHER;
        on(&(SteamEvent){.kind = STEAM_EV_ROOM_FAILED, .code = code}, user);
    }
}

static void callback(int id, const uint8_t *p, int size, void (*on)(const SteamEvent *, void *), void *user) {
    switch (id) {
    case CB_CALL_COMPLETED: {
        if (size < 16) break;
        uint64_t call = u64_at(p, 0);
        int32_t result_id = (int32_t)u32_at(p, 8);
        uint32_t n = u32_at(p, 12);
        if (call != S.call || n > CALL_RESULT_CAP) break;   // a call of the library's own
        uint8_t r[CALL_RESULT_CAP] = {0}, failed = 0;
        if (!S.md_result(S.pipe, call, r, (int)n, result_id, &failed) || failed) {
            S.call = 0;
            on(&(SteamEvent){.kind = STEAM_EV_ROOM_FAILED, .code = S.creating ? STEAM_FAIL_CREATE : STEAM_FAIL_OTHER}, user);
        } else if (result_id == CB_LOBBY_CREATED && n >= 16) {
            if (u32_at(r, 0) == RESULT_OK) {
                entered(u64_at(r, 8), ENTER_OK, on, user);
            } else {
                S.call = 0;
                on(&(SteamEvent){.kind = STEAM_EV_ROOM_FAILED, .code = STEAM_FAIL_CREATE}, user);
            }
        } else if (result_id == CB_LOBBY_ENTER && n >= 20) {
            entered(u64_at(r, 0), u32_at(r, 16), on, user);
        }
        break;
    }
    case CB_LOBBY_CHAT_UPDATE:
        if (size >= 28 && u64_at(p, 0) == S.room && u64_at(p, 8) != S.self) {
            uint64_t who = u64_at(p, 8);
            int came = u32_at(p, 24) == CHANGE_ENTERED;
            if (!came) {
                uint8_t ident[IDENTITY_SIZE];
                identity_of(ident, who);
                S.close_session(S.msgs, ident);
            }
            on(&(SteamEvent){.kind = came ? STEAM_EV_MEMBER_JOINED : STEAM_EV_MEMBER_LEFT, .a = who}, user);
        }
        break;
    case CB_LOBBY_INVITE:
        if (size >= 24 && (u64_at(p, 16) & 0xffffff) == STEAM_APP_ID)
            on(&(SteamEvent){.kind = STEAM_EV_INVITED, .a = u64_at(p, 8), .b = u64_at(p, 0)}, user);
        break;
    case CB_LOBBY_JOIN_REQUESTED:
        if (size >= 16) on(&(SteamEvent){.kind = STEAM_EV_JOIN_ASKED, .a = u64_at(p, 0), .b = u64_at(p, 8)}, user);
        break;
    case CB_SESSION_REQUEST:
        // only a player in the room may open a session: anyone on Steam can ask
        if (size >= IDENTITY_SIZE) {
            uint8_t ident[IDENTITY_SIZE];
            memcpy(ident, p, IDENTITY_SIZE);
            if (in_room(S.ident_get(ident))) S.accept(S.msgs, ident);
        }
        break;
    case CB_SERVERS_DISCONNECTED:
    case CB_SHUTDOWN:
        if (S.room) {
            S.room = S.call = 0;
            on(&(SteamEvent){.kind = STEAM_EV_LOST}, user);
        }
        break;
    }
}

void steam_pump(void (*on)(const SteamEvent *e, void *user), void *user) {
    if (!S.up) return;
    S.md_frame(S.pipe);
    CallbackMsg m;
    while (S.md_next(S.pipe, &m)) {
        // the block is the library's until it is freed, and a handler may call the library: copy first
        uint8_t copy[512];
        int size = m.size < 0 ? 0 : m.size > (int)sizeof copy ? (int)sizeof copy : m.size;
        if (size) memcpy(copy, m.param, (size_t)size);
        int id = m.id;
        S.md_free(S.pipe);
        callback(id, copy, size, on, user);
        if (!S.up) return;
    }
    for (;;) {
        void *batch[RECEIVE_BATCH];
        int n = S.receive(S.msgs, CHANNEL, batch, RECEIVE_BATCH);
        for (int i = 0; i < n; i++) {
            const uint8_t *msg = batch[i];
            const uint8_t *data;
            int32_t size;
            uint8_t ident[IDENTITY_SIZE];
            memcpy(&data, msg + MSG_DATA, sizeof data);
            memcpy(&size, msg + MSG_SIZE, 4);
            memcpy(ident, msg + MSG_PEER, IDENTITY_SIZE);
            uint64_t from = S.ident_get(ident);
            if (size > 0 && size <= STEAM_MESSAGE_MAX && from && from != S.self && in_room(from))
                on(&(SteamEvent){.kind = STEAM_EV_MESSAGE, .a = from, .data = data, .n = (size_t)size}, user);
            S.msg_release(batch[i]);
        }
        if (n < RECEIVE_BATCH) break;
    }
}

// ---- room ids
//
// Every lobby's 64-bit number starts with the same 32 bits (universe 1, the chat type, the lobby flag: 0x01860000); the
// other 32 name the lobby. A room id is those 32 bits and an 8-bit check of them, 40 bits as 8 symbols of 5 bits.

#define LOBBY_HIGH 0x01860000ull
static const char code_symbols[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";   // no 0, 1, I or O

static uint32_t code_check(uint32_t low) {
    uint32_t h = low * 2654435761u;
    return (h >> 24) & 0xff;
}

void steam_code_make(uint64_t room, char out[STEAM_CODE_CAP]) {
    out[0] = 0;
    if (room >> 32 != LOBBY_HIGH) return;   // a lobby of a kind this version does not know: it has no room id
    uint32_t low = (uint32_t)room;
    uint64_t v = (uint64_t)low << 8 | code_check(low);
    int o = 0;
    for (int i = 7; i >= 0; i--) {
        out[o++] = code_symbols[(v >> (5 * i)) & 31];
        if (i == 4) out[o++] = '-';
    }
    out[o] = 0;
}

uint64_t steam_code_parse(const char *text) {
    uint64_t v = 0;
    int n = 0;
    for (; *text; text++) {
        char c = *text;
        if (c == '-' || c == ' ' || c == '\t') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        const char *at = strchr(code_symbols, c);
        if (!at || !c || ++n > 8) return 0;
        v = v << 5 | (uint64_t)(at - code_symbols);
    }
    if (n != 8) return 0;
    uint32_t low = (uint32_t)(v >> 8);
    if ((v & 0xff) != code_check(low)) return 0;
    return LOBBY_HIGH << 32 | low;
}
