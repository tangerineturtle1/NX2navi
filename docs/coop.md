# Co-op

Each player runs their own game and save; the co-op layer connects the games of up to 8 players through a relay. The
game has no multiplayer of its own and its `nn::socket` stays offline: the layer is a host thread of the runtime
(`runtime/coop.c`) with its own UDP socket.

What exists so far: rooms, the relay, a sealed channel for messages between the players, progress sync (below) and the
menu's Co-op tab (below), which makes, joins, accepts and leaves a room. The other players are not shown in your
world, and where your Link is never leaves your PC ("Other players in the world").

**Co-op is a beta in 0.8.0.** The menu shows its Co-op tab only with `coop_beta = 1` under `[coop]` in `nx2navi.ini`;
no menu row sets it. Everything below is as it is with the tab shown.

## What joining a room means for your save

Read this before you join a room.

- **Nothing from a room reaches your game until you accept it.** When you join, the menu's Co-op tab and the log name
  the other players (each by the name and the key fingerprint their game signed), the room's rules and the key that
  signs them; the tab asks you to accept, and the log gives the room's offer a number: `put coop_accept = 1a2b3c4d
  into nx2navi.ini [coop] while the game runs`. Accept on the tab or do that. Until then your game takes nothing from
  the room. You accept players by
  their keys, not their names: a new player, a player whose game restarted, a removed player who comes back, or a
  second player with a name you know is a new offer, and so are a key you accepted that now carries another name (it
  is accepted under the name it had when you accepted it), other rules, rules signed by another key, and another
  save loaded in the room; nothing more is taken until you accept that one too. A key you accepted that comes back
  under its own name as another member number (the relay numbers players as they come) is no new offer; the log says
  so, and it says when a member presents another key than the one it was identified by. A player in the room whose game never
  says who it is keeps the room from being accepted at all. The number is random for each start of the game, so nobody
  can put the right one into a file in advance. Accepting every room without asking (`NX2NAVI_COOP_ACCEPT=always`)
  works only from the environment, never from `nx2navi.ini`.
- **Your game takes values only from players you accepted.** What a player you did not accept sends is not kept, not
  passed on and never applied, also after that player leaves or is removed. Every message that can change your save is
  signed by the player who sent it, so this holds also against someone who knows the room's secret and runs the relay,
  or sends from its address on your network: they can make no message in another player's name that changes your save.
  For story flags and items acceptance is passed along: a player you accepted hands on what they took from the players
  *they* accepted, so a stranger someone else let in reaches you through them. Pools are not passed along: your game
  counts in a pool only what you and the players you accepted brought, picked up and spent, and joins the pools only
  once you accepted the room (another save loaded in it joins only once you accepted that one too). What remains with
  the maker: a player uses the pools at all only once the maker's game admitted them, which it does for every player
  the maker accepted; a player who left before you accepted is not counted in your pools; and while a player you
  accepted counts a stranger in their pools, what they claimed when they joined a pool counts that stranger too, until
  the maker removes the stranger.
- **Your save is copied first.** The moment you accept a room, the game copies your whole save (every slot) to
  `coop-backup/` in the profile folder, and takes nothing from the room until the copy is made. The first copy for a
  room in each start of the game is `first-<date-time>` and is kept until you delete it; later copies (another save
  loaded in the room, say) are `<date-time>`, of which the newest eight are kept. A room the relay lost and that the
  game reaches again is still accepted and copied. "Going back" below says how to restore a copy. A game that has
  not saved yet has no save on the disk to copy: it takes nothing from the room until it has saved once (the log says
  so), and a copy that cannot be written keeps the room closed as well.
- **What the room can change in your save** is what its rules share (progress, items, songs, quest items; small keys,
  rupees and ammunition in a pool): every value is checked against your game's own table of flags before it is
  stored, passed on or applied. A flag can only be set to "done", an item count only up to 999, small keys only up to
  15, a pooled counter only within its capacity. Anything else is dropped, counted in the log, and a player who sends a
  few dozen of those is ignored for as long as they stay in the room.
- **What a player you let in can still do.** A player in the room plays with you: with progress shared, what they do
  in their world (open a chest, beat a boss, set a story flag) happens in yours, and a player who wants to can send
  such flags without playing for them. With small keys in a pool, a player you accepted can spend the keys you share:
  their spends count at most up to what entered the pool (what the players brought and picked up), so they can empty a
  dungeon's pool, and the keys picked up after that, but no more than the dungeon has, and a record that claims less
  than nothing counts as nothing. The maker's removal of that player, also after they left, or a pool reset after they
  left, gives back what they took, in every game, and each game makes its claim to the pool again as if that player
  had never been in it, so keys you brought while their gains were in the pool stay yours, and a claim made while
  their spends were in it does not leave the pool fuller than what the others brought. A pool reset while that player
  is still in the room carries what they took into the new pools, and removing them afterwards does not give it back:
  remove first, then reset (the reset's log line, the maker's hint and the Co-op tab's Reset the pools say so). Keys a
  removed player's gains handed out stay as doors already opened, and in the claim of a player who saved with them and
  joined the pools again with that save before the removal. With rupees or ammunition in a pool (off by default) the
  same holds for them.
  Flags stay set when a player is removed. None of this can be undone inside the game otherwise: go back to the copy
  of your save. Invite only people you trust with your save, as you would hand them your controller.
- **Who can get in.** Only someone who knows the room's secret: 18 characters (85 bits) that the game makes when a room
  is made and that the maker passes on to the others (by voice, a chat message). The relay never learns it. Someone who
  sees your traffic on the network, or who runs the relay, cannot read the messages, change them, make new ones or
  guess the secret from them. Someone who also knows the secret can read the room's messages, but can still make none
  in a player's name: each player signs what they send with their own key.
- **Removing a player.** The room's maker can remove a player by their key's fingerprint, which the log and the Co-op
  tab show beside each member (Remove on the player's row of the tab, or `coop_remove = <fingerprint>` in `nx2navi.ini`
  while the game runs; while they are in the room or after they left): the maker's game signs the removal, every game drops that player's key and keeps it
  dropped (their messages are ignored, their pool records dropped, so what they took from a pool comes back), and the
  relay drops them and refuses them again (the session the maker's game listed for them at that moment, so a player
  given the number since is not the one dropped). A member number in the file is not taken, since numbers pass to other
  players as members come and go, and each value acts once, so a line left in the file or read again after a moment in
  which the file could not be read removes nobody else. Someone who knows the secret can still come back under a new
  key (everyone then gets a new offer to accept or not); to be sure, make a new room, which has a new secret. The maker
  can also reset the pools (`coop_reset_pools = 1`, then 2 the next time, or 0 and then 1 again): every game joins them
  again with what they held for the players in the room, which recovers a pool that a player who left had emptied,
  also when the maker resets twice before the games joined again. What a game held at the reset is its claim from then
  on, also while that player is at the title screen, so a key another player picks up after the reset comes on top of
  it. A player who is not in the room at the reset, or whom the maker has not admitted to the pools yet, is carried
  for nobody: what it brought and picked up leaves the other games' pools until it plays again in the room, when its
  game joins them with what it holds. If the maker resets again before such a player's game joined, that game keeps
  the larger of its two carries, so keys the others used in between can come back (more keys, never fewer). Remove a
  player who emptied a pool and is still in the room first (both lines in one save of `nx2navi.ini` act
  in that order). The key the maker's game signs the room's rules and
  removals with is kept in the profile folder (`coop-keys/`, readable by your account only: the folder 0700, the file
  0600 on Linux; on Windows an access list of its own with your account alone), so a maker whose game restarted is still the room's maker. That file, together with the room's
  secret, lets whoever has both act as the room's maker: never hand out a copy of your profile folder. A game that sees
  another member present its own key says so in the log (that member stays not identified, so nobody can accept the
  room while it is there), and a key file that is damaged or cannot be read is logged too. Deleting the file ends your
  say over rooms you made before.

### Who can see what

| Who | Sees | Cannot |
|---|---|---|
| The relay's operator, and anyone on the network path | that a room exists (a random id and the locator, a value derived from the secret that does not give the secret back), the players' names, their IP addresses, when and how much they send | read, change or make any message between the players; learn the secret; change anyone's save or the room's rules (moving the relay's maker mark changes nothing for a player who accepted). It can drop, delay and send again what a player sent; a player takes one again only as "Replay" below says. |
| Someone who knows the secret and runs the relay, or sends from its address on your network | all of the above, and every message of the room | make a section, identity, pool record or set of rules in another player's name that a game acts on (each is signed by its maker); change your save other than through a player you accepted, or the rules of a player who accepted. It can also: send a member's messages again (see "Replay": within the last 64 they are refused, but four older ones in a row let it send the member's whole present session again); seal a message in a member's name with a counter far ahead, which costs a player that member's next three messages, each time it does so (one after every three of the member's keeps them out while it keeps sending); send rosters, or identities of another key for a member's session, that keep players from being identified or the room from being accepted while it keeps sending and up to 10 s after; send sections and digests in a member's name that use up what a player checks and takes from that member (128 signatures and 48 KB a second), which delays that member's own while it keeps sending; send digests in a member's name with sums of its choosing (see the sync messages); and, to a player who has not accepted the room yet, make every member's digest carry rules of a key of its own (a member does not sign its digest, only the rules in it are signed, by whoever made them): with no maker in the room, or one it controls, that player's game shows those rules and their key in the offer, so compare that key with the maker's before you accept (step 3 below). None of that changes a save, and none of it lasts longer than said once it stops |
| The other players in the room | your name, your progress (what the rules share), your messages | your IP address (the relay forwards everything), your account or profile (the id the relay knows you by is random for each start of the game and never shown to them) |
| Your log and "Copy system info" | a three-character label hashed from the room's secret (no part of the secret) and the offer's number; system info shows that `coop_room`, `coop_server`, `coop_name` and `coop_accept` are set, not their values | the secret, the relay's address (it appears only with `NX2NAVI_TRACE=coop`) |

### Going back to your save from before a session

1. Close the game.
2. In the profile folder, open `coop-backup` and pick the copy to go back to: `first-<date-time>` is the save as it
   was before you first accepted that room, `<date-time>` a later acceptance (the time is UTC).
3. Delete the folder `save-tx` (it may hold a save that was being written; it would otherwise be finished over the copy
   at the next start).
4. Move the folder `save/<numbers>` aside (keep it until you are sure), and copy the contents of the backup folder into
   a new `save/<numbers>` of the same name.
5. Start the game: every slot is as it was when you accepted the room. Progress made after that, in any slot, is in the
   folder you moved aside, not in the copy.

## The menu's Co-op tab

The menu (F1, or the pad's combo) has a Co-op tab, usable with the keyboard, the mouse and a pad.

- **Not in a room:** your name and the relay's address (each row opens a field to type in; with a pad, A on the field
  brings up the system's on-screen keyboard where it has one), Make a room, Join a room (a field for the secret, in any
  case, with or without dashes, and a Paste button that takes the clipboard's text), a line that says what the layer is
  doing or why it stopped (no answer from the relay, no room with that secret, refused), the rules of a room you make,
  and the co-op switch. Make and Join turn co-op on when it is off.
- **In a room:** while the room waits for your answer, who is in it (a player whose game has not said who it is, or
  whose session two keys present at once, is marked "not identified"; a player whose key you accepted under another
  name is marked "a key you accepted as <that name>", and that is a new offer), what its rules share, the fingerprint
  of the key that signs the rules, to compare with the one the maker reads out to you by voice or chat, and Accept or
  not; while any player is not identified the tab says so and why the room cannot be accepted, and Accept is greyed.
  Then the players, with who made the room, how long ago each was last heard and each one's key (and the name you
  accepted it as, when it now signs another); the maker sees its own key to read out, and Remove on each other
  player's row, which removes by the key (a player who is not identified has no key and is removed by member number,
  only while the same session still holds it). The maker also has a Pools group: every key that took more from the
  pools than it brought, in the room or not, with what it took and a Remove of that key, and Reset the pools, which
  says to remove such a player first. What it took can change, up or down, when the maker removes a player or a
  player accepts more keys, also while that player's game is at its title screen. Then the
  room's secret, hidden until you press its row, and a row that copies it to the clipboard; the rules (the room's
  creator changes them, everyone else reads them; a change is a new offer for everyone); Leave the room. The status
  line says when the relay lost the room and the game is getting back into it, and when the relay takes no more joins
  from your network and the game tries again in a minute.
- **Nothing is accepted, removed, reset or left by accident.** Accept or not, Remove, Reset the pools and Leave each
  open a window whose cursor starts on the safe button (Not now, Cancel, Stay), whenever it opens; only Right reaches
  the acting button, and pressing the button that opened the window again, holding it, or a double tap lands on the
  safe one. A new player or other rules while the Accept window is open replace what it shows and put the cursor back
  on Not now: Accept takes only the offer you read. The window says that your whole save is copied to `coop-backup`
  first, and before your first Accept in each start of the game it shows the three warnings of "Before you join a room
  with strangers" below.
- **While the menu is closed** a short note at the top of the screen says when a player joined or left the room and
  when the room waits for your answer.
- The name, the relay, the rules and the switch are settings (written to `nx2navi.ini` like every row); the secret is
  not a setting, is not written to the log, and is shown only when you ask.

## Running a relay

```
python tools/coop_relay.py [--host 0.0.0.0] [--port 24680] [--log-addrs] [--log-joins] [--max-rooms 1000] [--rooms-per-network 8]
```

Python 3, standard library only. A player who can reach it by UDP (forward the port on the router, open it in the
firewall) is all it takes; the others put its address into `coop_server`. The relay forwards the sealed datagrams of a
room and can read none of what the players send each other. It keeps only who is in which room, writes nothing to
disk, and loses its rooms when it stops: the room's maker makes it again under the same secret. Its log has one line
per room made, join, leave and removal, with names, member ids and a random room id (never the secret or the locator);
addresses only with `--log-addrs`; a line when a network's joins ran out (at most every 10 s); and with `--log-joins`,
where each network's joins went.

- A room holds up to 8 players, is closed 10 minutes after the last one left (a minute, if nobody but its maker was ever
  in it), and a player the relay has not heard from for 15 s is dropped (the member id is kept for a minute so that the
  same game, with its rejoin token, joins again as the same member).
- The roster (who is in the room, each one's session, which one made it) goes out at a change, again 0.25 s and 0.75 s
  later, and every 5 s: a datagram is lost now and then, and nothing is acknowledged.
- Limits: a source address answers a cookie challenge before the relay sends it anything larger than it sent; each
  member is limited to 200 datagrams and 64 KB per second, a datagram forwarded to seven counted seven times; each
  network (an IPv4 /24, an IPv6 /48) to 20 challenges a second, a join every 3 s after a burst of 20, and 8 open rooms;
  each room that forwarded to someone in the last 2 s to an equal share of the forwarding (rooms that send nothing, or
  send to nobody, take nothing); the relay as a whole to 5000 challenges a second and 8 MB/s of forwarding. A join
  attempt costs its network one join whatever the answer, and the same attempt sent again (its answer was lost, or it
  asks again for a room that is not there yet) is answered from memory without a charge while the answer is the same
  refusal; the game asks for a room that is not there at most every 10 s and, while it keeps its room, waits a minute
  when the relay refuses it for its network's joins instead of ending the session, so players behind one address who
  wait for a room do not use up each other's joins. The relay keeps the budgets of at most 4096 networks and forgets a
  budget that is full again, and checks its own challenge budget first, so a flood from spoofed addresses leaves no
  state behind. A member that rejoins with its token from its own address and in its own session needs no challenge;
  from another address, or with a new session (every rejoin draws one), it answers one first, so someone who saw its
  CONNECT cannot take its place. The challenges for a CONNECT with a member's valid token come from that member's own
  challenges (2 a second, 8 at once, kept apart for its own network and for the others), never from its packets, and
  from a share set aside for rejoins: 1000 a second split equally among the open rooms for CONNECTs from other networks
  than the member's, and 1000 a second split among the rooms and then among the eight member numbers of each for
  CONNECTs from the member's own network (each 8 at once at least, so a member can rejoin eight times at once even on
  a relay with its full 1000 rooms, and then once every 8 s). So the tokens of rooms someone made to flood with take
  only those rooms' shares, and a flood of CONNECTs with a member's token, someone else's or seen on the way, from
  another network neither drops the member's messages nor keeps its rejoin from its own network out of the room. What
  such a flood can take is the room's share for other networks: a member that comes back from another network (a
  phone that changed networks) may wait for as long as it lasts, and so may one whose own network someone sends from
  with its token. A game whose roster shows
  another session for it joins again, at most once
  every 15 s. A session that was in the room before is refused (the last 4096 of each room are kept). A flood from many
  networks can still use up the relay's 5000 challenges a second for newcomers: a relay has finite capacity, and its
  operator's firewall is the place for more.
- `--host ::` listens for IPv6 as well as IPv4. The game's own socket is IPv4 and IPv6 in one; Windows may show its firewall
  prompt the first time the game opens it.

## Rooms and secrets

A room is its secret: 18 characters of `23456789BCDFGHJKMNPRSTVWXYZ` (no vowels, no 0/1/O/I/L), typed in any case,
with or without `-` and spaces, shown `XXXXXX-XXXXXX-XXXXXX`. The game makes a new random one for each room it makes;
`coop_room` names one for `coop_auto` (tests, LAN parties). A create of a secret whose room is open on the relay is
refused (it is not a join), so nobody can be put into a room they did not mean to join.

## Settings

`nx2navi.ini`, section `[coop]` (`nx2navi.ini.example` has the text); the environment variable is the key with `NX2NAVI_`.

| Key | Default | |
|---|---|---|
| `coop` | `0` | on or off, at once; off: no socket and nothing running |
| `coop_name` | empty | at most 24 bytes; empty: `Player-` and four random hex digits |
| `coop_server` | none | `host`, `host:port` or `[v6]:port`, default port 24680; with none set the layer stays idle and logs why |
| `coop_port` | `0` | local UDP port, 0 = any; read at start |
| `coop_accept` | `ask` | `ask`: nothing from a room is applied until its offer is accepted; a number (the eight hex digits the log shows, random for each start of the game): accepts that offer, set while the game runs (the file's line as it is now counts before the one read at start); `always`, from the environment only (`NX2NAVI_COOP_ACCEPT=always`; an `nx2navi.ini` line is ignored): every room is accepted (the save is still copied first) |
| `coop_auto` | `off` | `create` or `join` starts a session at launch (a join needs `coop_room`); no menu row |
| `coop_room` | empty | the room secret of `coop_auto`; no menu row |
| `coop_remove` | empty | the room's maker removes a player by its key's fingerprint (in the room or after it left; the relay drops it too while it is in the room); each value acts once; a member number is not taken from the file; the Co-op tab's Remove does the same |
| `coop_reset_pools` | empty | the room's maker resets the pools when the value changes to a new one other than `0` (1, then 2; or 0, then 1 again); the Co-op tab's Reset the pools does the same |
| `coop_drop` | `0` | test: percent of received datagrams thrown away; no menu row |
| `coop_test_set` | empty | test, environment only (`NX2NAVI_COOP_TEST_SET`): `hash=value@frame,...` sets those flags (hash in hex) at those frames of play; an `nx2navi.ini` line is ignored |
| `coop_progress` ... `coop_ammo` | see below | the rules of a room this player makes (Progress sync) |

With `coop_auto` a room the relay lost (it restarted) is made again under the same secret by its maker, or waited for,
for as long as the session runs (asked for every 10 s; a refusal for the network's joins waits a minute); a maker who
finds it made again by someone else joins it, and the rules stay the ones its key signed. A player who leaves ends the
session. An `nx2navi.ini` someone else gave you can name a room and join it at launch (`coop_auto`, `coop_room`), but
it cannot accept the room for you: that needs the number of this start of the game, or the environment. The game reads
`coop_accept`, `coop_remove` and `coop_reset_pools` from the file as it is now once a second while it is in a room
(never out of one), as it reads the file at start (a BOM, a repeated key's later line, quotes), and skips a reading
when the file cannot be read or is empty for a moment, as while an editor saves it; values in the file at start, and
at the first reading after the start, act on nothing.

## Progress sync

When one player gains progress, the others' games gain it too, while they play; a player who joins later catches up
with everything the room has, and the room with what the joiner brings. Each player's own save keeps what it got: the
game saves it as its own progress.

The game keeps its progress as flags (a database keyed by 32-bit hashes of flag names). Between two frames the runtime
reads the flags that changed, and sets the ones the room has and this game lacks through the game's own set routines,
as if the player had done it. Nothing is read or set while the game is on its title screen, loading a save or changing
scene, and not while its clock stands (the pause menu): what arrives meanwhile is set afterwards. A chest or a door
another player opened shows open the next time the area loads; an item shows in the menu the next time it opens.

What one room merged belongs to that room and that save: leaving the room, joining another, or loading another save
forgets it. A save loaded while in a room is a new offer: nothing reaches it before you accept again, and the save is
copied again first. Once accepted, it gets the room's progress, and that holds what every save played in the room
brought, the last one's too (the others hold it); no way exists to tell one save's flags from another's in a merged
state, so the new offer and the copy are the answer.

### What is shared

| Category | Rule key | Modes (default first) | What |
|---|---|---|---|
| progress | `coop_progress` | `shared`, `own` | story, world and dungeon flags: events, talks, chests, doors, switches, bosses, map and compass |
| items | `coop_items` | `shared`, `own` | equipment, key items, bottles owned, wallet, bag and quiver sizes, magic meter size |
| songs | `coop_songs` | `shared`, `own` | |
| quest | `coop_quest` | `shared`, `own` | medallions, spiritual stones, the stone of agony, the gerudo card |
| keys | `coop_keys` | `pool`, `own` | small keys, per dungeon |
| rupees | `coop_rupees` | `own`, `pool` | |
| ammo | `coop_ammo` | `own`, `pool` | bombs, bombchus, arrows, deku nuts, seeds and sticks, magic beans |

- `shared`: a flag set by anyone stays set for everyone; of a count (an item, an upgrade) the highest wins.
- `own`: not synced; switching to it stops the merge and keeps what was merged.
- `pool`: one count that every player adds to and spends from. Each player's gains and spends are counted apart (in a
  record their game signs) and add up, so two players spending at once both pay; a pool that two spends at once took
  below zero shows 0 and the next gain pays the debt first. A player's spends count only up to what that player gained
  plus the counter's limit (999 rupees or ammunition), and never beyond what entered the pool (the highest count a
  player brought, at least 0, plus every gain): nobody takes more than the pool really had, so a dungeon's key pool
  loses at most the keys that dungeon has, and in a rupee pool a player who spends more than 999 rupees beyond what they
  gained is not charged for the rest; a count brought below zero counts as zero, so no record turns the others' keys
  into a debt. A joiner whose count is higher raises the pool to it; a game that restarts and rejoins does not add its
  count again; a game that joins a pool in debt (as a drain leaves it) pays the debt with its count, so the count comes
  back with the drainer's removal. A pool above a counter's capacity shows the capacity. Each game counts in its pools
  only the records of the players its player accepted, and its own; it stores and passes on every admitted record, so
  the games hold the same records, and games whose players accepted the same players show the same pools. A game joins
  the pools once its player accepted the room and the maker's game admitted its key, which it does for each player the
  maker accepted (up to 24 keys per pool reset); until then its counts stay its own.
- Small keys in the pool: a key anyone picks up goes into the dungeon's pool, a door anyone opens takes one out, and the
  pickup and the door are progress, so they are gone and open for everyone. Two players who each used the last key
  on a different door at the same moment open one door more than the keys paid for.
- Items, songs and quest items can be `own` only with `coop_progress = own`: in a shared world a chest one player
  opened is empty for the others. With progress `own`, keys are `own` too (each world has its own doors). A room's maker
  whose settings break this gets the nearest valid set, and the log says which.

Never shared, by design or because no reliable way exists yet:

- Where Link is, his health (the game republishes it from the player several times a second, so it cannot be set
  through the flags), the magic in the meter (likewise outside the flags), the time of day (the clock runs in the
  game's world manager, not in a flag), bottle contents and the equipment worn.
- Heart containers and pieces and gold skulltula tokens: most of their pickups are flags of single objects in the world
  that this layer does not write, so a shared count would pay one piece or token twice; the reward flags the game names
  for them stay with each player too, so everyone can collect them.
- Items the game takes away again (the trade sequence, rented masks, the knife that breaks, amiibo items): a shared
  highest count would hand them back.
- Flags that the game itself resets at a scene change, a new day or a load, and counters that are not progress (puzzle
  positions, minigame records, enemies defeated).

### The room's rules

The rules belong to the room: every game in it merges the same way. They are the settings of the player who made the
room, signed with that player's key: a game takes rules only signed by the key its player accepted (pinned at the
first acceptance in the room). Before that, a game that made the room itself takes its own; otherwise it takes the
key of the player the relay marks as the maker, but only while no other player carries rules signed by another key
(the relay's mark cannot outvote the rules the players' digests carry, and a mark the relay puts on your own name does
not make you the maker); with no maker in the room it takes the rules every member's digest carries. A member does
not sign its digest, so before you accept, someone who knows the secret and runs the relay can make every member's
digest to you carry rules of a key of its own, which your game then takes (with no maker in the room, or a maker it
controls): the offer shows the key that signs the rules, and comparing it with the maker's is what tells you (step 3
of "Before you join a room with strangers"). When the members' rules disagree it takes
none, applies nothing, and the log and the offer say so: ask the maker which key fingerprint is theirs. No other
player's settings are used, and a member the relay marks later (after a restart, or a relay that lies) changes
nothing for a player who accepted; the offer then says that the relay names another maker. The signed rules travel
with the sync messages, so a joiner and a game that rejoins after a relay restart get them from anyone. If the maker
has left, the players keep the rules they have. The maker can change a rule during the session: switched to shared or
pool, the category merges from then on (after each player accepts the new offer); switched to own, it stops, and
nothing already merged is taken back. A maker whose game restarted keeps the room's key (from `coop-keys/` in the
profile), takes its own rules back from the others and can change them, remove players and reset the pools again.

### Messages

Sync rides in the room's sealed messages. Each game sends a digest (a sum per section of its state, and the rules)
every 2 s and at once to a player who joins; a section whose sum differs is sent to that player, so lost messages are
repaired and a late joiner catches up within a few seconds. A change goes out at once and again 80 and 250 ms later.
Sending is paced at 32 KB/s, under the relay's limit per member, with the players who asked for sections served in
turn (one that asks for everything all the time gets its share, not all of it), and each game takes at most 48 KB/s
from any one player. Games merge only with the same schema: this layer's version, the game's version, and the whole table of what
is shared and up to which value, so two builds that classify a flag differently do not merge.

## The room's secret and the wire

For a reviewer: what is derived from what, what each datagram carries and what a receiver checks. The code is
`runtime/coop_proto.c` (C) and `tools/coop_relay.py` (the relay's side, Python); `tests/coop_vectors.txt` holds byte
vectors of every message that both are checked against.

**Library.** Monocypher 4.0.2 (`runtime/third_party/monocypher/`, two files, unmodified, BSD-2-Clause, dual-licensed
with CC0) for BLAKE2b and XChaCha20-Poly1305. It is small, has no dependencies, builds the same on Windows and Linux,
and its primitives are standard (RFC 7693, and the XChaCha20-Poly1305 construction of the IETF draft). It is used only
in `coop_proto.c`, behind four functions (`coop_room_keys`, `coop_seal`, `coop_open`, and BLAKE2b inside the first), so
another library can replace it there; the sync layer's signatures (EdDSA over Curve25519 with BLAKE2b, the library's
`crypto_eddsa`) go through `coop_sign_keypair`, `coop_sign`, `coop_sign_check` and `coop_blake2b` there too. The relay
needs no library: it uses Python's own BLAKE2b and HMAC.

**The secret.** 18 symbols, each drawn uniformly from 27 (bytes from the operating system's random source, a byte
below 243 taken modulo 27): log2(27^18) = 85.6 bits. It is made by the client that creates the room and never sent to
the relay or written to a log; players pass it on themselves.

**Derivation.** With `S` the 18 canonical ASCII characters as the key of BLAKE2b:

- locator `L` = BLAKE2b-128(key `S`, "nx2navi-coop/2 room"): the relay's name for the room, and the key of the tags of
  datagrams between a client and the relay. The relay and anyone who sees a CONNECT learn `L`; getting `S` from it is a
  search over 2^85.6 secrets.
- message key `K` = BLAKE2b-256(key `S`, "nx2navi-coop/2 messages"): seals every message between the players. Only
  holders of `S` have it.

**The client and relay datagrams.** A 24-byte header (magic, version 2, type, the relay's room id, src, dst, length,
sequence) and an 8-byte tag, HMAC-SHA256(`L`, header[0:16] ‖ payload)[0:8]. CONNECT carries `L`, a client id (random
for each start of the game), a session (random for each join), a nonce (random for each attempt), the name, the
relay's cookie and the rejoin token. CHALLENGE, JOINED and ERROR echo the nonce and carry `L`'s tag; the client takes
only answers with its own nonce and its own `L`, so a datagram someone else sends from the relay's address neither
ends an attempt nor puts the player into another room (an untagged "protocol version" error is taken only as the
first answer of an attempt). JOINED carries a rejoin token, random per member: the relay gives a member's id (and the
maker's mark) back only to a CONNECT with it. The ROSTER lists each member's id, flags (away, maker), session and
name. KICK (maker to relay) names a member and its session. These datagrams are authenticated against outsiders who
do not know `L` only; the relay and an observer of a CONNECT can make them, which lets them disturb the session
(drop, reorder, invent members that cannot speak), not change a save. So a roster is taken only up to 2^20 ahead of
the last one's number (the relay's counter moves with its own datagrams), and after the silence time and 4 s without
a roster taken, the next one counts whatever its number, so one forged far ahead does not cut a player off; no roster
ends a session for progress sync (a session ends only when its member's identity in a later one names it, below), and
neither does a relay that says the room is gone; a roster that lists another session for this player makes it join
again at most once per 15 s, and its game keeps signing for the session it joined with. A relay or observer that keeps
sending rosters changes the roster for as long as it sends, as it can drop datagrams for as long as it likes.

**The player message envelope** (the payload of DATA, version byte 1):

```
u8 envelope version (1) | u64 sender's session | u32 counter | XChaCha20-Poly1305 ciphertext of (u8 kind, message) | u8[16] tag
nonce (24 bytes) = session (8, LE) | counter (4, LE) | sender's member id (1) | 11 zero bytes
associated data = destination member id, envelope version
```

A session is drawn for every join and the counter starts at 1, so a nonce never repeats under `K` (two sessions
collide with probability about n²/2^65). The kind byte and the message are inside the ciphertext: a new kind of message
(a pose, say) needs only a number, and a later envelope version can change the outer fields without touching the
messages. A message is at most `COOP_MAX_MESSAGE` = 1146 bytes after its kind.

**What a receiver checks, in order:** the sender is in the roster and is not itself, the destination is it or
everyone, the envelope's session is the one the roster lists for that sender, the counter is new within a window of
the last 64 of that session, the tag opens with `K` for that sender and destination; only then is the counter recorded
and the message delivered. A counter older than the window is refused, unless it opens and is the fourth of that
sender's messages in a row below the window, each later than the one before: then the window starts again from it and
it is delivered. `K` is the whole room's, so whoever knows the secret can seal one message in a member's name with a
counter far ahead, which leaves that member's later messages below the window; the member's next four start it again,
and the first three of them are lost. That is per forged message: one sent again after every three of the member's
keeps that member's messages from a player for as long as it keeps sending.

**Replay.** A message recorded before a player joined again carries the old session, which the roster no longer
lists: refused. Within a session the window refuses only what it still remembers: a message sent again within the
last 64 it took is refused, and an older one alone is refused too. But four older ones in a row, each later than the
one before, start the window again (that is how it recovers from a counter far ahead), and from then on the window
has forgotten what it took: anyone who recorded a member's messages of its present session (the relay too, without
the secret) can have every one of them delivered again, in order. Someone who knows the secret can also seal a
recorded message again under a new counter. So the envelope does not make a message arrive once; a layer for which a
repeat matters checks a counter of its own. A sync section carries its sender's own counter inside its signature for
that: a section is taken once, and only within the last 64 of its sender's session; a digest taken again repeats what
it said (until its sender's next one, 2 s later). A relay that lists an old session in a forged roster could replay
that session's messages to a player whose game never saw the session end; they are messages the sender really signed,
and sync messages only raise values, so this brings nothing new into a save.

**The sync messages (version 4).** `runtime/coop_state.c` has the byte layout. Each game makes a signing key for each
room and start of the game (EdDSA, from a random value of the start and the room's secret, so nothing ties a player
across rooms; the maker's from 32 random bytes kept in the profile for that room, mixed with the secret, so that it
outlives a restart) and sends in every DIGEST its identity: the key, its name, the session it held in the room before
this one (0 none) and a signature over the room's context (BLAKE2b-128 keyed with the secret), its member id, the
session it joined with and that session before. A game takes values only from a member whose identity checks out; an
identity belongs to one session, a session a member's later identity names as the one before is never taken again
(the latest 1024 of each key the player accepted are kept apart, where no other member's churn removes them; the
others share the last 65536), and a key another member holds is refused, so an identity cannot be copied to another
member or slot; the relay also refuses a session that was in the room before. An identity of another key for a
session that is identified already (only one of them is the member's) makes the game take that member as neither
while both are presented; once one of them has not been presented for 10 s, the other takes the place. Only the key's
holder ends its sessions
that way: a forged roster, a relay that says the room is gone, or a replayed message or identity ends none. A game
announces its identity at once when its session changes. A name is checked as signed and shown cleaned (control and
reordering characters replaced). Every SECTION (a push of a change or an answer to a digest) ends with its sender's
counter (8 bytes) and its signature (64 bytes) over "nx2navi-coop/4 section", the room's context, the sender's
member id and session and the section's bytes, checked against the identified key before anything of the section is
stored, applied or passed on: 72 bytes and one signature per section whatever it carries, about 35 µs to sign and
100 µs to check one (measured on the runtime's -O2 build of Monocypher on an x64 desktop). Flags are taken only from a
member whose key the player accepted. The DIGEST also carries the
maker's charter: the rules, the keys admitted to the pools (8-byte key ids), the last 16 keys removed and the pools'
epoch, with an epoch the maker raises at each change (saturating at 2^32−1), signed by the maker's key; each game
keeps every removal it saw in the room. A pool record (one per contributor and pool) carries the contributor's key,
the pools' epoch, a sequence number, its base, gains and spends, and its signature; a game keeps the record with the
highest sequence of each key (its own too: a room that comes back hands it its own newest record) and stores only
records of admitted keys of the current pools' epoch, checking the signature before it stores or passes one on; its
pools count only the records of keys its player accepted, and its own. A game remembers (in memory) its last 8 joins
of each pool in the epoch, each with what it joined with and the records it saw then; its base is the highest of
their claims, each made as if the game had counted then the keys it counts now, and made again whenever that changes
(a removal, or its player accepting more). An older join keeps the claim it had when the ninth was made, so after
nine saves loaded in one pools' epoch (a game over and continue counts) a removal leaves that claim as it was until
that save is loaded again. A reset carries this game's own claim as it would be with only the players the reset
admits, and the game signs it at once as its record in the new epoch (a game the reset does not admit claims it when
it joins). A digest's sums are not signed: a forged one can make a game send its sections or join a pool before it
heard every record, as dropping messages can.

**Rules and the maker.** A game takes a charter signed by the key its player accepted; before any acceptance, its own
for a room this session made; else by the key of the member the relay marks as the maker while no identified member
carries a charter of another key; else, with no maker in the room, by the one key every identified member's charter
carries (each member's first digest is waited for, up to 5 s, so the quickest does not win). A charter of the same key
counts only with a later epoch.

**Per-member limits in each game.** Every value is checked against the local table before it is stored, passed on or
applied; at most 48 KB/s is taken from one member; 32 refused flags, or pool records whose contributor's signature
fails, in sections that member signed, and the member is ignored while it stays. Nothing that fails inside a digest
counts against the member it names (an identity, a charter in the maker's name), and neither does a section whose
signature fails: anyone with the secret can send either in its name. A pool record this game cannot count is dropped
without counting against its sender, since an honest member may hold a newer charter for a moment. A signature check
that failed is not made again for the same signature, key and signed bytes (a changed copy of a genuine message fails
on its own and leaves the genuine one to be checked), and each member gets at most 128 signature checks a second (256
at once), sections' included;
the register index is keyed (SipHash-2-4 with a random key of the process); a pool stores at most 24 records (the
admitted keys), a record's base, gains and spends are at most 2^20 either way, and nobody but a record's key holder
can make or change it. Without the system's random source (no client id, session, nonce or key a stranger cannot
guess) co-op stays off.

## Before you join a room with strangers

1. **Keep a save you can go back to.** Join only with a save you are ready to restore from
   `coop-backup/first-<date-time>`. Anyone you accept can set story flags in your save, and so can anyone a player you
   accepted let in. A player in your pools can empty a dungeon's small keys while they play; the maker's removal of
   that player, or a pool reset after they were removed or left, gives back what they took, but no removal takes a
   flag back, and a reset while they are still in the room keeps their drain: remove first, then reset.
2. **Your acceptance is yours, the room is not.** A player you did not accept reaches neither your flags nor, by
   themselves, your pools, also when they know the secret and run the relay. But the players you accept pass on the
   flags of the players they accepted, a player you accepted who counts a stranger in their pools claims against a
   pool with that stranger in it until the maker removes them, and the maker alone decides who may use the pools at
   all. If anyone you do not know is in the room, even one you declined, do not accept: leave, and ask the maker for a
   new room (a new secret) for the people you know.
3. **Check the keys, and keep yours to yourself.** Before you accept, compare the fingerprint of the key that signs
   the room's rules with the maker's, and each player's with that player's, by voice or chat: a name is what a member
   says it is, and someone who knows the secret and runs the relay can hold back a player's own identity and show its
   own key under that player's name and member number until that player's arrives. Never start the game with
   `NX2NAVI_COOP_ACCEPT=always`,
   or with a script or `nx2navi.ini` someone hands you. Never hand out a copy of your profile folder: its `coop-keys`
   folder makes whoever has it, with a room's secret, the maker of that room (your game logs a member that presents its
   key).

What nothing can prevent: everyone who knows the secret can join and read the room; a member or the relay can disturb
it (leave messages out, delay them, send one again as "Replay" says, flood within its limits, keep the rules from
being agreed, change the offer so that nothing is applied until you accept again, keep the room unaccepted by never
saying who it is, send rosters or identities that keep a player unidentified for as long as it keeps sending, or seal
messages with a counter far ahead, each of which costs a player three of a member's messages); and the relay sees who plays
with whom and when, with names and addresses. None of that changes a save.

## Other players in the world

Not in this build. The protocol has a message for it (POS in `runtime/coop_proto.h`: the area, a position, a facing
and what Link's animation slots play), and its codec and its counter against replayed poses are in the build and
checked by `--probe-coop`, but nothing sends one and nothing draws another player: a POS that arrives is dropped. So
no position leaves your PC, whatever the settings.

## In the log

State changes only: `coop: connecting to the relay`, `the relay answered in N ms`, `room XXX… created`, `joined room
XXX… as member N`, `<name> (member N) joined the room` and `left the room`, `relay lost; rejoining`, `left room (<why>)`.
A cause that repeats (a message that does not open, a replay, no server) logs once per 30 s; a counters line follows
every minute in a room. Peer addresses are never logged; the relay's only with `NX2NAVI_TRACE=coop`, which also adds a
line per datagram.

Progress sync: `coop: sync: playing a save: N progress flags, ...` when a save is being played (and `... nothing is read
or written until a save is played` when it stops), `a room to accept; in the room: ...; rules: ...` followed by `put
coop_accept = <number> into nx2navi.ini [coop]` (or, while a member is not identified, `N member(s) in the room are
not identified`; when the members carry rules of different keys, a line that says so), `room accepted`, `the save
as it was before this room is in <folder>`, `the key this game signs the room's rules with is kept in <folder>` (the
maker), `the room's rules: progress shared, ...`, `took N values from the room (progress a, items b)` at most every
5 s, `N values from the room were refused`, `member N sent values this game does not take; nothing more is taken from
it`, for the maker `key K (member N, name | not in the room) took N keys ... from the pools beyond what it brought;
coop_remove = K removes it`, `key K removed`, `coop_remove = N in nx2navi.ini names a member number` (not taken), `the
pools are reset`, `member N presents this game's own key`, `member N presents another key than the one it was identified
by in its session`, `member N (name, key K) presents a key you accepted as member M | under the name ...: under this
name it is a new offer`, and `the room's key file ... is damaged | cannot be read`.
`NX2NAVI_TRACE=coop` adds `sent`,
`received` and `applied` lines by category, hash and value. With `coop = 0` nothing of this is logged and the game's
frames do no more than check the setting.

## The interface for the layers above

`runtime/coop.h`: `coop_send(kind, bytes, n)` and `coop_send_to(member, ...)` send a sealed message to the others (it is
a datagram: it may be lost, arrive out of order or, recorded, arrive again: see "Replay"), `coop_on_message(kind, fn, user)` receives
them on the coop thread, `coop_on_view(fn, user)` reports the state and the roster (with each member's maker mark and
whether its messages opened), `coop_view_get(&view)` copies them, `coop_command(COOP_CMD_CREATE | COOP_CMD_JOIN |
COOP_CMD_LEAVE, secret)` drives the room, `coop_remove_member(id)` is the maker's removal (progress sync signs it
through `coop_on_remove`). `runtime/coop_offer.h` holds the menu's calls, none of which waits for the sync engine (what
they read is a copy the coop thread publishes at each step, what they ask it carries out at its next step):
`coop_game_offer(&offer)` and `coop_game_accept(number)` show a room and accept it (`CoopOffer` in
`runtime/coop_state.h`: each member's signed name, key fingerprint, whether it is identified or removed, how many
members never identified (the room cannot be accepted while there are any), whether the members' rules disagree, the
rules and the key that signs them, whether another save is played and whether the copy is made),
`coop_game_reset_pools()` is the maker's pool reset, `coop_game_remove_key(fingerprint)` the maker's removal of a key
(also of a player who left; a member in the room with that key is removed at the relay too), and
`coop_game_contributors()` lists the pools' contributors with what each took beyond what it brought, per pool and in
the room or not (a player who used keys others found is among them). `CoopView.made` is this session's own creation of
the room, `CoopView.name` the player's own name. The wire format is `runtime/coop_proto.h`. Progress sync uses
`coop_on_tick(fn, user)` for its timers.

## Checks

- `python tests/coop_relay_test.py`: the relay (rooms, rejoin with the token, timeouts, limits per member, per network
  and in all, removal by the maker, bad tags, a stolen client id, challenge and room floods, fan-out, garbage from a
  member, the log without secrets, a join attempt asked again without a charge, two players behind one address who
  wait ten minutes for a room, a rejoin during a challenge flood, rooms that send to nobody, a rejoin while the members
  of four other rooms flood with their tokens, and a rejoin on a relay with its full 1000 rooms while someone floods
  with every token of the room and a member of it with its own) on a fake clock and over
  UDP, and the codec against the vectors. With `NX2NAVI_EXE=<game>/ocarina` it also runs the probe against a relay it
  starts (with `--log-joins`, whose log a failure shows), with and without 30 % loss, and restarts that relay in the
  middle.
- `ocarina --probe-coop`: the sockets, the codec and its cryptography (known answers, a sealed message that does not
  open when changed or sent by another), a fuzz of every parser, the POS message's counter (a pose sent again, one
  recorded 35000 poses earlier, another session in the same member number, one forged far ahead), which members count
  as shown (only those accepted and identified), and two sessions in
  one process that make and join a
  room, exchange messages, take garbage and forgeries from a hostile relay (a changed message, one it sealed itself,
  a replay after a rejoin, lies while a player connects), lose the relay and find each other again, and in which the
  maker removes a player. Needs no window, GPU or game files. It runs its own small relay;
  `NX2NAVI_COOP_RELAY=host:port` uses a real one, `NX2NAVI_COOP_DROP=30` drops that share of what the sessions
  receive, `NX2NAVI_COOP_VECTORS=tests/coop_vectors.txt` checks the byte vectors. Its sync part runs the merge engine
  between fake games on a fake network: merging, late joiners, loss, the rules and pools, nothing carried from one room
  or save to the next, a hostile member (refused values, pool records it did not sign or that nobody admitted, its own
  records against a pool, removal), identities (a removed player back, a copied identity, a second name, the accepted
  list), the offer's number, rules that wait for acceptance, another save in the room, the maker's pinned key, an
  honest base above the limit, fair answers, the third review's cases: flags only from accepted keys (through
  another member, after a declined member left or was removed, from a member that never identified), a key drain with
  the maker's game restarted and the absent drainer removed, a pool reset, an identity replayed after 64 sessions or a
  room the relay lost, the relay's maker mark against the members' rules, signed names, failed signatures, a game's own
  pool record after the room came back, and a removal 17 removals ago; and the fourth review's: a record with the
  lowest base, pools that follow the player's own acceptance, two resets in a row, a forged roster's sessions, what a
  contributor took per pool, an accepted key's ended session after 65600 others; and the fifth review's: sections in
  another member's name made with the secret alone (and a flood of them), a member's session after a forged roster
  with a replayed message, a roster without it, a relay that says the room is gone, and a replayed identity of a
  session the game never saw end, claims made again after a removal (keys brought while a removed player's gains
  counted, a claim made while its spends counted, honest games that agree, a game at its title screen), another save
  that joins only once accepted; and the sixth review's: digests in a member's name whose charter does not check out,
  a changed copy of a member's digest and of the maker's charter sent first, another key's identity for a member's
  session sent first, and a reset's carry with a player who comes after it and with a removal in the same charter;
  and the seventh review's: a key the player accepted in another member's place under that member's name, and a relay
  restart that hands the players other numbers;
  and a randomised run (a few seeds by default; `NX2NAVI_COOP_ADVERSARY=first:count` runs those seeds alone,
  `NX2NAVI_COOP_ADVERSARY_TRACE=1` prints its events and the engines' lines) of hostile members who hold the secret,
  admitted or not, and of an attacker with the secret and no accepted key who forges senders and rosters, replays
  messages and identities, sends changed copies of digests and sections and identities of its own key in a member's
  place, against invariants of flags, acceptance, identification, ignored members, the maker's charter and a ledger
  of the honest players' joins, carries, pickups and uses. The session part adds rosters forged
  far and a little ahead, rosters that list another session again and again, a message in a member's name sealed with
  the room's key 2^31 − 1 ahead, a refusal for the network's joins, and a
  room that is not there. `ocarina --probe-fs` checks that the first copy
  of a save outlasts the rotation.
- `python tests/coop_page_check.py`: the menu's Co-op tab without the game, driven with the keyboard's and the pad's
  events: typing a name, a relay and a secret (Q and E are letters there, not tab keys), Make, Join with a wrong secret
  and a pasted one, the offer accepted and declined, the maker's key to compare, the three warnings before the first
  Accept only, a player not identified (Accept greyed, removed by member number only while the same session holds
  it), a key accepted under another name (said in the offer, the row and the Accept window), Remove by key, a pool
  contributor's key removed, Reset the pools, Leave, the relay's join limit and a lost room,
  the maker's rules and the others' read-only ones; Accept, Remove, Reset and Leave never act on a second press, a held
  button or a double tap, and a new offer under an open Accept window puts the cursor back on Not now. It then runs
  again with a deliberate fault (each such window opens with the cursor moved onto its acting button) and passes only
  when that run fails. Needs a C++ compiler only.
