// What the menu's Co-op tab reads of a room and asks of it (coop_game.c), in a header of its own without host.h so that
// the menu's C++ can include it. Every call may be made from any thread and never waits for the sync engine: what is
// read is a copy the coop thread publishes at each of its steps, and what is asked is carried out by the coop thread at
// its next step (a few ms in a room), which logs the outcome.
#pragma once
#include "coop_proto.h"
#include "coop_state.h"

// What joining the room means, for the player to accept: the engine's offer (coop_state.h: CoopOffer), with the
// relay's names for members not identified yet. Nothing from a room is applied to the game before the player accepted
// its offer and a copy of the save was made (<profile>/coop-backup/<time>/, docs/coop.md; the first copy for a room is
// "first-<time>" and is never pruned). An accepted offer stays accepted while every member is an identity accepted,
// the rules and the key that signs them stay the same and the same save is played; anything else is a new offer, and
// nothing more is applied until that one is accepted. A member in the room that never identified itself keeps the room
// unaccepted. The offer's number is random for each start of the game. The COOP_ACCEPT setting accepts one offer by its
// number (the log shows it) or, from the environment only, every offer with "always"; the menu calls coop_game_accept.
// 1 when there is an offer; without one (token 0) the members and disagree are still filled in while in a room.
int coop_game_offer(CoopOffer *out);
// Accepts the offer of that number, the one the player was shown: 1 when it is the offer published last (or that offer
// is accepted already) and no member in the room is unidentified; 0 otherwise, and nothing changes (ask again once
// unidentified is 0). The coop thread accepts it at its next step only while it is still the room's offer.
int coop_game_accept(uint32_t token);
// The room's maker (its game holds the key that signs the rules, kept in the profile across restarts) resets the pools:
// every game joins them again with what they held for the players in the room. 1 when asked (this game is the maker).
// The setting COOP_RESET_POOLS does it too (when it changes to a value other than 0).
int coop_game_reset_pools(void);
// The room's maker removes a key by its fingerprint (xxxx-xxxx-xxxx-xxxx, as the offer, the contributors and the log
// show it), also of a member that left: every game drops it and its pool records, and a member in the room that holds
// it is removed at the relay too. 1 when asked (a fingerprint, and this game is the maker). The setting COOP_REMOVE
// takes a fingerprint too. A member is never removed by its id from here: ids pass to other players as members come
// and go.
int coop_game_remove_key(const char *fingerprint);
// The identities the pools admit and what each took beyond what it brought, per pool (coop_state.h: CoopContributor):
// whom the maker would remove after a drain. Their count (at most max), as published at most half a second ago.
int coop_game_contributors(CoopContributor *out, int max);
// This game holds the key that signs the room's rules (it made the room, in this start of the game or an earlier one):
// its removals and pool resets act.
int coop_game_is_maker(void);
// The members this game would show in its world (nothing draws them in this build), one bit per member id: identified (heard, so not disputed
// between two keys), not removed, and with a key this player accepted under the name it signs now (no accepted_as).
// A member the player has not accepted, or who is not identified, shows no puppet. accepted(key fingerprint, user): 1
// when this player accepted that key. Kept apart from the game so that --probe-coop checks it (coop_proto.c).
uint32_t coop_offer_shown(const CoopOffer *o, int (*accepted)(const char *key, void *user), void *user);
// The same, as the coop thread last published it (0 outside a room).
uint32_t coop_game_shown(void);
