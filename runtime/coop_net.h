// What co-op (coop.c) needs from the network under it: Steam (steam.c), or the stand-in for tests of two games on one
// PC (coop_local.c), which one Steam account cannot do. The functions are steam.h's, with its events.
#pragma once

#include "steam.h"

typedef struct {
    int (*start)(char *why, size_t cap, int *which);
    void (*stop)(void);
    uint64_t (*self)(void);
    void (*name_of)(uint64_t id, char *out, size_t cap);
    int (*room_create)(int max_players);
    int (*room_join)(uint64_t room);
    void (*room_leave)(void);
    uint64_t (*room)(void);
    uint64_t (*room_owner)(void);
    int (*room_members)(uint64_t *out, int max);
    int (*room_set)(const char *key, const char *value);
    void (*room_get)(const char *key, char *out, size_t cap);
    int (*send)(uint64_t to, const void *data, size_t n, int reliable);
    void (*pump)(void (*on)(const SteamEvent *e, void *user), void *user);
} CoopNet;

// The stand-in: NX2NAVI_COOP_LOCAL=<port> in the environment (never from nx2navi.ini) makes this game the player
// whose id is that UDP port on 127.0.0.1; the room it makes has that number, and another game joins it with
// NX2NAVI_COOP_LOCAL_JOIN=<that port>. No Steam, no room ids, no invites.
extern const CoopNet coop_net_local;
int coop_local_wanted(void);        // the variable is set
uint64_t coop_local_join(void);     // the room NX2NAVI_COOP_LOCAL_JOIN names, 0 none
int coop_local_makes(void);         // NX2NAVI_COOP_LOCAL_MAKE=1: this game makes its room at once
