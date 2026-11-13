// Players (runtime/players.c): several people play on one PC, each with saves of their own. A player is a folder that
// holds the save folders of fs_host.c (save, save-tx, save-backup, save-import): player 1 is the profile itself, so the
// saves of a profile made before players existed are player 1's, and player n (2 and up) is <profile>/players/<n>.
// Each folder may hold player-name.txt, the name the menu shows (UTF-8, one line); without it the menu names the
// player by number.
//
// nx2navi.ini's player key (NX2NAVI_PLAYER, a restart-only setting) chooses who plays: the game holds a save in memory
// and writes it back, so the player changes only at a start. The menu's Players tab lists, adds, renames and removes
// players (ui/menu.cpp tab_players).
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLAYER_NAME_MAX 32   // bytes of UTF-8, as nn::account's nickname
#define PLAYERS_MAX 64

typedef struct {
    int id;                              // 1 and up
    char name[PLAYER_NAME_MAX + 1];      // "" when the player has no name of its own
    int has_save;                        // the game saved for this player
} PlayerInfo;

// The player this run plays as: NX2NAVI_PLAYER at the first call (1 when unset or not a player number).
int players_active(void);
// The active player's folder under profile into out (made when missing); 0 when the path does not fit.
int players_save_dir(const char *profile, char *out, size_t cap);
// The players of rt_config()->profile in number order, player 1 first and always there, and the active one even when
// its folder is not made yet. Returns how many were written (at most cap).
int players_list(PlayerInfo *out, int cap);
// A new player with the next free number, named name ("" none). Its number, 0 on failure (logged).
int players_create(const char *name);
// Gives player id the name ("" drops it, back to the number). 1 on success.
int players_rename(int id, const char *name);
// Moves player id's folder to <profile>/players-removed/<id> <UTC time>, where its saves stay until deleted by hand.
// Refused for player 1 and the active player. 1 on success.
int players_remove(int id);
// name cut to PLAYER_NAME_MAX bytes on a character boundary, control characters dropped, spaces trimmed.
void players_clean_name(const char *name, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
