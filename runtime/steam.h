// Steam for co-op (steam.c): rooms are Steam lobbies, players are invited through Steam, and the games talk through
// Steam's networking, so nobody forwards a port and nobody runs a server. NX2navi is no Steam app of its own: it signs
// in as Steam's public test app (id 480, "Spacewar"), which every account may use; Steam then shows the player as
// playing that app.
//
// The Steamworks library is not part of NX2navi and is never in its repository. steam.c declares the functions it
// calls itself and loads steam_api64.dll from beside the game at run time (the launcher copies one there from a
// Steam game the player has, launcher/steamlib.py); without the file, or without a running Steam client that is
// signed in, steam_start fails and says why.
//
// Every function here is called from one thread (the co-op thread, coop.c): the library is used single-threaded.
#pragma once

#include <stddef.h>
#include <stdint.h>

enum { STEAM_APP_ID = 480, STEAM_NAME_CAP = 64, STEAM_ROOM_MAX = 8, STEAM_MESSAGE_MAX = 1200 };

typedef enum {
    STEAM_EV_ROOM_ENTERED,   // a: the room. This game made or joined it
    STEAM_EV_ROOM_FAILED,    // code: why (STEAM_FAIL_*)
    STEAM_EV_MEMBER_JOINED,  // a: the player's id
    STEAM_EV_MEMBER_LEFT,    // a: the player's id
    STEAM_EV_INVITED,        // a: the room, b: the friend who invites. Shown to the player, who may join
    STEAM_EV_JOIN_ASKED,     // a: the room. The player accepted an invite, or chose Join Game, in Steam itself
    STEAM_EV_MESSAGE,        // a: the sender, data and n: the message. Only from players in the room
    STEAM_EV_LOST,           // Steam lost its connection or closed: the room is gone
} SteamEventKind;

enum { STEAM_FAIL_CREATE = 1, STEAM_FAIL_NO_ROOM, STEAM_FAIL_FULL, STEAM_FAIL_REFUSED, STEAM_FAIL_OTHER };

typedef struct {
    SteamEventKind kind;
    uint64_t a, b;
    int code;
    const uint8_t *data;
    size_t n;
} SteamEvent;

typedef struct {
    uint64_t id;
    char name[STEAM_NAME_CAP];
    int online;    // signed in to Steam now
    int in_app;    // shown as playing app 480: probably in NX2navi with co-op on, so an invite can reach the game
} SteamFriend;

// Loads the library, signs in as app 480 and takes the interfaces. 1 when Steam is up; 0 with the reason in why (a
// sentence for the log and the menu's status line; STEAM_WHY_* says which, for the menu's own text).
enum { STEAM_WHY_OK, STEAM_WHY_NO_LIBRARY, STEAM_WHY_OLD_LIBRARY, STEAM_WHY_NO_CLIENT, STEAM_WHY_OTHER_OS };
int steam_start(char *why, size_t cap, int *which);
void steam_stop(void);   // leaves the room and signs out
int steam_up(void);

uint64_t steam_self(void);
void steam_name_of(uint64_t id, char *out, size_t cap);   // the player's Steam name; "" while Steam has not got it
int steam_is_friend(uint64_t id);
int steam_overlay(void);   // Steam's overlay is in this game (it was started from Steam): its invite window works

// Rooms. Create and join answer later with STEAM_EV_ROOM_ENTERED or STEAM_EV_ROOM_FAILED.
int steam_room_create(int max_players);
int steam_room_join(uint64_t room);
void steam_room_leave(void);
uint64_t steam_room(void);         // 0: in none
uint64_t steam_room_owner(void);   // whose game made it; Steam hands it to another member when that player leaves
int steam_room_members(uint64_t *out, int max);
int steam_room_set(const char *key, const char *value);              // the owner's notes on the room, read by everyone
void steam_room_get(const char *key, char *out, size_t cap);

int steam_friends(SteamFriend *out, int max);   // the player's friends, those in the app first, then online, then the rest
int steam_invite(uint64_t friend_id);           // Steam shows the friend an invite to this room
void steam_invite_window(void);                 // Steam's own invite window (only with steam_overlay)

// A message to a player in the room. reliable: it arrives, in order; else it may be lost and is never late.
int steam_send(uint64_t to, const void *data, size_t n, int reliable);

// Runs Steam's callbacks and hands out what happened. Call it many times a second.
void steam_pump(void (*on)(const SteamEvent *e, void *user), void *user);

// A room's id as players pass it on, "XXXX-XXXX": the part of the lobby's number that differs between lobbies and a
// check of it, in letters and digits that cannot be mixed up (no 0, 1, I or O). steam_code_parse takes it in any case, with or
// without the dash and spaces; 0 when it is no room id.
enum { STEAM_CODE_CAP = 16 };
void steam_code_make(uint64_t room, char out[STEAM_CODE_CAP]);
uint64_t steam_code_parse(const char *text);
