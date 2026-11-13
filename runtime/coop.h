// Co-op (coop.c): each player runs their own game; a room connects up to four of them through Steam (steam.h). The
// room is a Steam lobby, its maker's game is the host, and the games send each other where their Link is, chat lines
// and what the game layer shares (coop_game.c). docs/coop.md is the players' and the reviewers' description.
//
// One host thread (the co-op thread) owns Steam and the room. The menu and the game's thread never wait for it: they
// read the view it publishes at each step and hand it commands it carries out at its next one.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "steam.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    COOP_PROTOCOL = 1,       // games of another number do not play together
    COOP_PLAYERS = 4,
    COOP_NAME_CAP = STEAM_NAME_CAP,
    COOP_SCENE_CAP = 48,
    COOP_CHAT_CAP = 200,     // bytes of a chat line, UTF-8
    COOP_CHAT_LINES = 8,
    COOP_FRIENDS = 64,
    COOP_GAME_KIND = 16,     // message kinds from here on belong to the game layer (coop_game.c)
    COOP_MESSAGE_MAX = STEAM_MESSAGE_MAX - 1,   // bytes of a message after its kind
};

typedef enum {
    COOP_OFF,        // the setting is off: no thread work, Steam untouched
    COOP_NO_STEAM,   // on, but Steam is not there (note says why); tried again every few seconds and at coop_retry
    COOP_READY,      // Steam is up, in no room
    COOP_ENTERING,   // making or joining a room, or waiting for its host to answer
    COOP_IN_ROOM,
} CoopState;

// Why the last attempt ended, for the menu's status line (ui/coop_page.cpp has the texts).
typedef enum {
    COOP_NOTE_NONE,
    COOP_NOTE_NO_LIBRARY, COOP_NOTE_OLD_LIBRARY, COOP_NOTE_NO_CLIENT, COOP_NOTE_OTHER_OS,
    COOP_NOTE_CREATE_FAILED, COOP_NOTE_NO_ROOM, COOP_NOTE_FULL, COOP_NOTE_REFUSED, COOP_NOTE_NOT_OURS,
    COOP_NOTE_OTHER_VERSION, COOP_NOTE_REMOVED, COOP_NOTE_HOST_SILENT, COOP_NOTE_LOST, COOP_NOTE_BAD_CODE,
    COOP_NOTE_LEFT,
} CoopNote;

// Where a player's Link is, as their game last said.
enum { COOP_POSE_ADULT = 1, COOP_POSE_PAUSED = 2 };
typedef struct {
    float pos[3];    // metres, the game's world
    float vel[3];    // metres a second
    float yaw;       // radians about the up axis, the way Link faces
    uint8_t flags;   // COOP_POSE_*
    char scene[COOP_SCENE_CAP];   // the area's name as the game has it; "": not in the world (title screen, loading)
} CoopPose;

typedef struct {
    uint64_t id;
    char name[COOP_NAME_CAP];
    uint8_t self, host;
    uint8_t has_pose;
    uint32_t pose_ms;   // how long ago the pose arrived
    CoopPose pose;
} CoopPlayer;

typedef struct {
    char name[COOP_NAME_CAP];
    char text[COOP_CHAT_CAP + 1];
    uint64_t at_ms;      // plat_now_ms when it arrived
    uint8_t notice;      // a line of the room itself ("... joined"): name holds the player, text is empty
    uint8_t joined;      // for a notice: 1 joined, 0 left
} CoopChatLine;

typedef struct {
    CoopState state;
    CoopNote note;
    char steam_says[160];          // with COOP_NO_STEAM: the Steam layer's own sentence, for the log's reader
    char code[STEAM_CODE_CAP];     // the room's id, "" outside a room
    uint8_t host;                  // this game is the room's host
    uint8_t overlay;               // Steam's own invite window can be shown
    uint8_t local;                 // the test stand-in for Steam is in use (two games on one PC)
    int n;
    CoopPlayer players[COOP_PLAYERS];
    uint64_t invite_room;          // a friend invites this player to a room (0 none) ...
    char invite_from[COOP_NAME_CAP];
    int nchat;
    CoopChatLine chat[COOP_CHAT_LINES];   // oldest first
    uint32_t friends_serial;       // changes when coop_friends has a new list
} CoopView;

void coop_init(void);       // starts the co-op thread; it idles while the COOP setting is off
void coop_shutdown(void);   // leaves the room and signs out of Steam, before the process ends

void coop_view(CoopView *out);
uint32_t coop_serial(void);   // changes whenever the view does: a reader that keeps a copy compares this first

// Commands, from any thread; each is carried out at the co-op thread's next step and none waits.
void coop_create(void);
int coop_join(const char *code);    // 0: the text is no room id (nothing is asked of Steam)
void coop_join_invite(void);        // the room of the invite the view shows
void coop_decline_invite(void);
void coop_leave(void);
void coop_retry(void);              // look for Steam again now
void coop_invite(uint64_t friend_id);
void coop_invite_window(void);
void coop_remove(uint64_t player_id);   // the host's: that player is out of the room and stays out
void coop_chat(const char *text);
void coop_friends_refresh(void);
int coop_friends(SteamFriend *out, int max);   // the list of the last refresh

// ---- for the game layer (coop_game.c)
void coop_pose_set(const CoopPose *pose);   // this game's own Link, each frame he is in the world; NULL when he is not
// A message of the game layer to everyone in the room (to 0) or to one player; kind from COOP_GAME_KIND on, n at most
// COOP_MESSAGE_MAX. From any thread; it leaves at the co-op thread's next step. 0 outside a room.
int coop_post(uint64_t to, uint8_t kind, const void *data, size_t n, int reliable);
// coop_game.c's part, called on the co-op thread: a message of its kinds from a player of the room, and a player who
// came (the game layer sends that player what it shares) or went.
void coop_game_message(uint64_t from, uint8_t kind, const uint8_t *data, size_t n);
void coop_game_joined(uint64_t id);
void coop_game_room(int in_room);

#ifdef __cplusplus
}
#endif
