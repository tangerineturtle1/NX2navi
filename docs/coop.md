# Co-op

Up to four players, each in their own game with their own save, meet in a room through Steam. In a room you see the
other players' names where their Link stands in your world, you can chat, and progress is shared: what anyone achieves
counts for everyone.

**Co-op is experimental and off by default.** Turn it on in the menu's Co-op tab (*Co-op (experimental)*), or with
`coop_steam = 1` under `[coop]` in `nx2navi.ini`. With it off the game uses no network at all.

Not in this version: the other players are shown as a name with a distance, not as a Link model, and enemies, pots
and the time of day are each game's own.

## What you need

- **Steam, running and signed in.** NX2navi is no Steam game of its own: it signs in to Steam as *Spacewar* (app 480),
  the test app Valve gives every account. While co-op is on, Steam shows you as playing Spacewar. Nobody forwards a
  port on a router and nobody runs a server: the room is a Steam lobby and the games talk through Steam's own network,
  which finds a direct path between them or relays for them.
- **At least one Steam game from 2021 or later installed** (Windows). Co-op uses Steam's library file,
  `steam_api64.dll`, which NX2navi may not ship; every Steam game has one in its folder. The launcher fetches it for
  you: each time it starts the game and the file is not beside `ocarina.exe`, it looks through your Steam libraries
  for the newest one that Valve signed and that has the calls co-op uses, and copies it there (a second or two, once;
  `launcher/steamlib.py`). You can also copy one there yourself. The Co-op tab says when the file is missing or too
  old; the game loads it from that one place only.
- **The same NX2navi version and game version (1.1.1)** as the others. A game of another version is told so and does
  not get into the room.

## Rooms

- **Create a room** makes a room and shows its **room ID**, eight letters and digits as `XXXX-XXXX`. A press on the row
  copies it. Your game is the room's host.
- **Join a room** takes a room ID (typed in any case, with or without the dash; Paste takes the clipboard's text).
- **Invite a friend** lists your Steam friends, those in co-op first, and sends a Steam invite to the one you choose.
  The invite reaches a friend whose NX2navi is running with co-op on: it shows at the top of their screen and on their
  Co-op tab, where *Join their room* accepts it. A friend whose game is not running gets Steam's own invite, which
  would start Spacewar, not NX2navi: tell them the room ID instead. When the game was started from Steam (the
  launcher's *Add to Steam*), Steam's own invite window is offered as well.
- **Players** lists who is in the room and the area each one's Link is in. The host can press a player to remove
  them; a removed player cannot come back into that room.
- **Leave the room** asks first. When the host leaves, Steam hands the room to another player, whose game is the host
  from then on.

A room ID is the number of the Steam lobby. Anyone who has it can join while the room is open, so pass it on only to
the people you want in your game; the room is also visible to your Steam friends, who may join it from Steam. To be
rid of someone, the host removes them; to be sure, leave and create a new room.

## What joining a room means for your save

- **Your save is copied first.** When your game starts to share in a room (you are in the room and playing a save),
  it copies your whole save (every slot) to `coop-backup/<date-time>` in the profile folder, and takes nothing from
  the room until the copy is made. The newest twelve copies are kept. "Going back" below says how to restore one.
- **Progress is shared while *Share progress* is on** (it is, by default): story, world and dungeon flags (events,
  talks, chests, doors, switches, bosses, maps and compasses), items and equipment, songs, quest items, wallet, bag and
  quiver sizes. A flag anyone has set is set for everyone, and of a count the highest wins. A player who joins later
  gets what the room has, and the room what the joiner brings. Your game saves it as its own progress.
- **Each player keeps their own** small keys, rupees, ammunition, heart pieces and containers, gold skulltula tokens,
  bottle contents, the trade sequence, the equipment worn, life and magic.
- **Nothing is undone.** Leaving a room keeps what was shared. A player in the room plays with you: what they do in
  their world happens in yours, and a game that wants to can send progress it did not play for. Join rooms of people
  you trust with your save, as you would hand them your controller, and go back to the copy if you regret it.
- With *Share progress* off, your game sends and takes no progress: only names, positions and chat.

Every value from the room is checked against your game's own table of what is shared before it is used: the flag must
be one of the shared ones, an on/off flag can only be set to "done", an item count only up to 999. Anything else is
dropped and counted in the log.

### Going back to your save from before a room

1. Close the game.
2. In the profile folder, open `coop-backup` and pick the copy to go back to (the time is UTC).
3. Delete the folder `save-tx` (it may hold a save that was being written; it would otherwise be finished over the
   copy at the next start).
4. Move the folder `save/<numbers>` aside (keep it until you are sure), and copy the contents of the backup folder
   into a new `save/<numbers>` of the same name.
5. Start the game: every slot is as it was before the room's progress reached it.

## Who can see what

| Who | Sees |
|---|---|
| The players in the room | your Steam name, the area your Link is in and where he stands, your chat lines, the progress your game shares |
| Your Steam friends | that you play Spacewar, and the room (they may join it) |
| Valve | what Steam sees of any game that uses its lobbies and network: who is in which lobby, and the traffic's size and timing. Steam encrypts the traffic between the games |
| Your log (`logs/nx2navi.log`) and "Copy system info" | that a room was made or joined, the other players' Steam names, and the lines below; never a room ID or a Steam id. Steam's library prints lines of its own, one with your Steam id, to the console the game was started from; they are not in the log file, but they are in a file you redirect the game's output to yourself |

## Settings

`nx2navi.ini`, section `[coop]`; the menu's Co-op tab has a row for each. The environment variable is the key with
`NX2NAVI_`.

| Key | Default | |
|---|---|---|
| `coop_steam` | `0` | on or off, at once. On: the game signs in to Steam; off: no network. (`coop`, the switch of the co-op of versions up to 0.9.2, does nothing: a line of it left in a file turns nothing on) |
| `coop_share` | `1` | in a room, share progress, items and songs (above) |
| `coop_names` | `1` | show the other players' names where they stand. Needs DLSS, FSR or TAA on: only then does the menu know where the camera looks |
| `coop_chat` | `1` | show new chat lines, and who joined or left, over the game for a few seconds while the menu is closed |

A settings file cannot put a game into a room: a room is made or joined only from the menu.

## In the log

`coop: on, signed in to Steam as app 480`, `coop: Steam is not available: <why>`, `coop: room made`, `coop: in the
lobby; asking the host`, `coop: joined the room`, `<name> joined the room`, `<name> left the room`, `coop: the host
left; this game is the host now`, `coop: left the room`, `coop: the save as it was before this room is in <folder>`,
`coop: a save is played: N flags and counts can be shared`, `coop: took N values of progress from the room`, `coop: N
values from the room were not ones this game shares; dropped`.

## How it works

For a reviewer. The code is `runtime/steam.c` (the Steam library), `runtime/coop.c` (the room), `runtime/coop_game.c`
(the game side) and `runtime/ui/coop_page.cpp` (the menu's tab and the names over the game).

**The Steam library.** `steam.c` loads `steam_api64.dll` from beside the exe at run time and calls the plain C
functions it exports; it declares them itself, and nothing of the Steamworks SDK is in this repository. It sets
`SteamAppId` and `SteamGameId` to 480 in its own environment before the library loads. Callbacks are taken with the
library's manual dispatch on the co-op thread, the only thread that uses the library. A library that lacks one of the
calls is refused as too old.

**The room.** A room is a friends-only Steam lobby that carries the note `nx2navi = 1`; a lobby without it belongs to
some other program that uses the test app and is left at once. The lobby's owner is the host. A game that entered the
lobby says HELLO to the host with its protocol number and game build; the host answers WELCOME or REFUSED and sends
everyone the list of the players it let in. Every game takes positions, chat and progress only from the players on
the host's list, and only a lobby member's session is accepted, so a stranger on Steam, a player the host removed and
a game of another version reach nobody.

**Messages** go through Steam's networking messages (datagrams to a Steam id): positions unreliable, twenty a second
while Link is in the world; everything else reliable. Steam vouches for the sender's id and encrypts. Each message is
one kind byte and a body whose length is checked before it is read; `coop.c`'s header comment lists them.

**Progress.** The game keeps its progress as flags in a database keyed by 32-bit hashes of flag names
(`coop_game.c`'s header comment has the layout). Between two frames the runtime reads the flags that changed and sends
the ones that rose; it sets the ones the room has and this game lacks through the game's own set routines, as if the
player had done it, at most sixteen a frame. Nothing is read or set on the title screen, while a save loads or an area
changes. A game that starts to share asks the others for their whole state and sends its own. Games merge only when
their tables of what is shared are the same (a sum over the table travels with every message).

**The host's part is small**: it decides who is in the room. Progress has no arbiter, because every shared value only
ever rises: two games that set the same flag agree whatever the order.

## Checks

- Two games on one PC cannot both sign in to one Steam account, so the room, the messages and the game side are
  tested with a stand-in for Steam: `NX2NAVI_COOP_LOCAL=<port>` in the environment (never from `nx2navi.ini`) makes a
  game the player whose id is that UDP port on 127.0.0.1; `NX2NAVI_COOP_LOCAL_MAKE=1` makes its room at once and
  `NX2NAVI_COOP_LOCAL_JOIN=<port>` joins another game's. `NX2NAVI_COOP_TRACE=1` logs every two seconds where this
  game's Link is and where it has the others.
- `python tests/coop_peer.py --host <port>` is a second player without a second game (two games need twice the
  memory): it joins the room of a game started with `NX2NAVI_COOP_LOCAL=<port>` and `NX2NAVI_COOP_LOCAL_MAKE=1`,
  stands two metres beside the game's Link so that its name shows there, says a chat line, asks for the game's
  progress and, with `--flags`, sends flags as set. It ends with `peer: ok` when it was let in, saw the game's Link
  in the world and got its progress. Run it on a copy of a profile: the flags it sends reach that save.
- `NX2NAVI_MENU_TEST_SET=<frame>:COOP_DO=create | leave | join:<room id> | chat:<text> | page:join | page:invite |
  page:chat | page:leave` (environment only) does what the Co-op tab's row or window does.
- `ocarina --probe-services` checks, among the rest, that every text of the tab has its four languages.
