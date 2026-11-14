# Built-in cheats

The settings menu's Cheats tab offers built-in policies on game 1.1.1, identified by the main build ID
`E0F3CCFAF50065AE`. Other builds keep the existing imported-cheat interface without these built-ins. Every
toggle defaults to off; sprint speed, spin range and spin charging default to normal. Settings persist in
`nx2navi.ini`. The current integration uses NX2navi 0.10.1; the supported game version remains 1.1.1.
The earlier spin measurements below used NX2navi 0.9.2.

| Group | Options |
|---|---|
| Link | Infinite health, magic and rupees; unlimited air jumps; sprint speed normal / 2× / 3.5× / 5× |
| Equipment | Bomb arrows; spin attack range normal / 1.5x / 2x / 2.5x; magic spin charging normal / instant. |
| World | Independent map and minimap switches for Gold Skulltulas, heart pieces, hidden grottos, treasure chests, bean plots, Great Fairies and capacity upgrade venues. |
| Inventory | Separate infinite magic beans and business seeds; contents of each unlocked bottle, up to five. |
| Ammunition | Separate infinite arrows, bombs, bombchus, Deku nuts, slingshot seeds and Deku sticks. |
| Timers | Freeze the supported shared countdown at its current remaining time. |

Resource policies preserve existing capacities and unlocks. Health does not revive a dead player. Magic requires
an existing magic meter; ammunition requires its existing capacity, with an additional obtained-item check for
bombchus. Magic beans and business seeds require their obtained flags and use native limits of 10 and 30.

Bottle rows appear only for unlocked slots. Each row selects empty, red/green/blue potion, blue fire, fish,
letter, Big Poe, Poe, bugs, fairy, milk or half milk. An edit updates the current and buffered contents once;
normal consumption continues afterward. Save in the game to retain the contents. Bottle validity and stock
are never changed.

Unlimited Jumps responds to another press of the configured jump action. Holding the button does not repeat
jumps. Sprint Speed scales the native sprint action without changing ordinary movement, gravity or global
game speed. Lower speeds can suit confined locations better. Text refers to actions rather than physical
controller buttons. Labels and help follow the selected menu language: English, Russian, Brazilian Portuguese
or Simplified Chinese.

Bomb Arrows is a live Equipment toggle (`bomb_arrows`, off by default). It converts Link's normal arrows into
explosive arrows while a bow, bomb bag and bomb stock are available. Nocking or cancelling an arrow costs no
bomb; firing consumes one bomb and the game consumes its normal arrow. Existing infinite-resource policies
can refill those stocks independently. Fire, ice and light arrows retain their original behavior.

A small animated black charcoal aura identifies the arrow. It follows the arrow through the scene camera
without requiring DLSS, FSR or frame generation. The marker is a projected overlay and does not sample scene
depth. It disappears on impact. The impact queues a native Item_Bomb actor on the framework thread and uses
the game's chemical detonation path; it does not simulate explosion damage with direct health writes.
An arrow already fired remains explosive if the option is subsequently disabled.

Spin Attack Range multiplies the native collision geometry for the equipped sword's spin attack. Each sword
keeps its own base reach. The game controls the attack window, updates its physics shape and restores ordinary
sword geometry afterward. The option changes hit reach; the sword model and visual effects do not grow with it.
It does not scale ordinary sword attacks or other actors' weapons. A changed range takes effect when the next
native spin shape is calculated. `spin_range` accepts `normal`, `1.5`, `2` or `2.5`.

Magic Spin Charge selects `normal` or `instant` through `spin_charge`. Instant removes the additional wait for
the fully charged magic spin, reaching it on the next gameplay update. Drawing the sword and the normal attack
animation still run. The Great Fairy upgrade and native magic checks remain required. Range and charge speed
are independent options; neither edits the unlock flag or a save directly.

Freeze timers acts only while the shared timer is running in countdown mode with a positive finite value.
Disabling it resumes normal counting. Count-up clocks, bomb fuses and unrelated temporary effects are not
modified. The shared clock was verified live in the Zora diving minigame on a local 0.7.1 Windows build;
references in other event scripts establish reuse of the interface, not testing of every event or its other
failure conditions.

## World

Show Gold Skulltulas and Show heart pieces are independent live switches, both off by default. They reveal
the current area's native collectible locations on the map and minimap. A collected token or heart piece
keeps the game's check-mark icon; an uncollected one uses its matching icon without a check mark.
Skulltula locations remain visible during the day, including those whose actors only appear at night.
A crescent marks night-only Skulltulas, on both unchecked and collected icons. The condition is read from the
actor placements' native nocturnal property, independently of collection status and the current clock.
Locations remain filtered by the game's current age, so a childhood-only Skulltula does not appear on the adult map.
Dungeon floor selection and the game's map coordinates, zoom and clipping stay native. Turning a switch off restores
the game's ordinary visibility for that category. These options do not collect anything or edit progression.

The launcher builds the extended map layout locally from the player's game. Its textures are already present
in that game's shared UI archive; crescent variants are generated locally from these icons. No extracted assets are distributed. A resource mod replacing the same
layout archive takes precedence, and the reveal hooks remain inactive if the extended layout is unavailable.
Night textures compose with generated controller prompts. Their unchanged allocation sizes and original texture
properties are checked before composition; a resource override without the validated variants keeps ordinary icons.
Collection status comes from native per-instance Skulltula records and the native heart-piece query, including
direct actors, chest rewards and event rewards, rather than inventory totals.

Hidden grottos, Treasure chests, Bean planting spots, Great Fairies and Capacity upgrades each have their own
live switch, off by default. Grotto markers identify entrances; they do not claim the grotto has been cleared.
Chests retain the native opened icon and dungeon floor filtering, and can be revealed without owning a compass.
Outdoor chests absent from the original map table are added from that area's actual placements.
These added records stay hidden when their applicable reveal switches are off, even where native map permissions
would otherwise show a chest. Existing native chest visibility is preserved.
Bean markers identify soil locations for the current age. They currently have no planted check: the soil's
insect-release flag is unrelated to planting and is deliberately excluded.

Great Fairy checks use the corresponding native reward event. Upgrade venues receive a check only where a
specific persistent reward flag is verified; venues without such a flag remain location markers. The Skulltula
House check requires both wallet rewards. Native upgrade chests may also be revealed by Capacity upgrades
without enabling all chests. No option grants a reward or writes a collection flag.

Additional icons and map records are generated locally from the player's resources. If another mod overrides
the required map table or shared UI textures, the additional facility hooks remain inactive. The initial generated
dataset contains 73 age-specific grotto entrances, 20 bean plots, 10 Great Fairy locations and 11 upgrade venues;
the same physical location can have separate child and adult records. This is not a claim that every reward
venue has already been catalogued.

The compile step passes its selected compiler and process environment through both icon generators to the
local ASTC decoder build. Launcher installations use their private toolchain without requiring Clang on the
user's PATH or a separate system MSYS2 installation. `tests/astc_toolchain_test.py` covers this propagation and
cold-cache compiler invocation.

## Implementation and checks

`runtime/cheats.c` validates the build, table shapes, key hashes, indices, readable memory and relevant object
identities before writing. Health and magic capture their live resource bindings through independent bits of
the existing function-entry observer; the resolution observer keeps its own bit. The sprint patch is declared
in `recomp/patches-v111.txt`: the generator validates the original instruction and emits the optional multiplier
path. Spin range uses three native spin-only result sites and a host callback registered in `runtime/patches.c`.
The callback validates Link's current sword and its reference generation, then multiplies the native length and
thickness scales in floating-point return registers. There is no cached collision shape to restore across a
weapon change. Instant changes only the native second-phase charge threshold to 0.125 ticks, a positive value
that avoids division by zero. Original words at all patch sites are validated by the generator. Generated C is
not edited by hand or checked in.

`python tests/cheats_check.py` compiles the real policies against synthetic guest memory, without game files.
`python tests/world_map_check.py` checks independent category toggles, collected versus uncollected icon frames,
the native query contract, CPU preservation, and the absence of progression writes.
`python tests/world_map_art_check.py` checks duplicate archive members and preservation of unrelated layout variants.
`python tests/world_map_night_check.py` checks texture composition, fixed allocation sizes, idempotence, original
resource guards and preservation of unrelated bytes. The map policy test includes checked/unchecked nocturnal frames.
`python tests/world_extra_check.py` checks all five independent switches, native call preservation and verified
completion queries against synthetic memory. `python tests/world_byml_check.py` checks preservation of native
scalar types, 64-bit instance IDs and nested containers when generating map tables.
Set `CC` to the UCRT64 clang executable when it is not on PATH. The test covers the build gate, health/death
and capacity guards, magic capacity, wallet limits, item ownership, legacy no-op policies, all 13 contents in
each of five bottles, locked-slot and invalid-enum rejection, countdown freezing, count-up exclusion and expired
or non-finite timers, all four spin multipliers, preservation of other CPU registers, and stale/non-sword guards.
`--probe-ui-text` checks the language catalogs and live menu-language selection. Changes
under `recomp/` also require the differential checks in [building.md](building.md).

On a 0.9.2 Windows build, isolated save-copy runs measured the Kokiri sword's fully charged spin capsule length
as 1.224 normally and 3.06 at 2.5×, with radius 1.5 and 3.75. The partial charge measured 0.304 / 1.35 normally
and 0.76 / 3.375 at 2.5×. Both returned to ordinary geometry 0.8 / 0.15 after each attack. The second charge phase
took about 1.5 seconds normally and no more than one gameplay update with Instant (sampling about every 53 ms).
All 70 original save, backup and transaction files were unchanged by hash comparison. These are geometry and
timing checks, not an enemy-hit or every-sword combat test. Other swords share the native calculation but were
not individually exercised; a separate locked-upgrade save was not tested live. Other events' timer branches,
Linux and other game builds remain unvalidated.

On the 0.10.1 Windows integration, isolated save-copy sessions verified normal-arrow wall impacts with real native
bomb explosions. With refill policies disabled, three fired arrows reduced the quiver from 30 to 27 and bombs
from 20 to 17. The black marker was exercised with and without DLSS. All 70 original save, backup and transaction
files remained unchanged by hash comparison. Enemy-hit behavior and every scene have not been individually
exercised. `python tests/bomb_arrows_check.py` additionally checks shooter ownership and reference generations,
elemental-arrow exclusion, cancellation without a bomb cost, one-shot consumption, asynchronous request timing,
native actor/detonation arguments, and CPU/stack preservation.

On the 0.10.1 Windows integration, isolated save-copy sessions showed uncollected Gold Skulltulas on the Water Temple
map and both checked and unchecked heart pieces in Kakariko. The Kakariko minimap showed the same collectible icons.
The initial map integration preserved all 624 unrelated layouts and extended all four collectible layout variants.
The crescent follow-up additionally redirects four facility-layout shadow references to the main icon alpha;
the remaining 620 layouts stay byte-identical. Night textures retain the original archive's size and named slots.
Runtime selftest, settings, input and all 3,483 UI-text checks passed. All 70 original save, backup and transaction
files remained unchanged. Every location, age, floor and reward type has not been individually tested live.

The nocturnal follow-up showed crescents on the Kakariko map and minimap with PlayStation prompts retained.
A temporary copy of the test process's collection registry exercised a checked night icon, then restored the
original registry. The original contained 11,657 records: collection queries therefore allow a larger bounded
registry instead of incorrectly rejecting saves above 10,000 entries. Policy tests also exercise 12,000 records.
No primary process or save file was written by that fixture; all 70 original save/backup/transaction files stayed unchanged.

The additional world integration was exercised in isolated Lost Woods, Death Mountain Crater and Kakariko sessions.
Grotto, bean, checked Great Fairy, native chest and upgrade icons appeared on the map/minimap. A separate Kakariko
run kept upgrade icons visible with grotto markers disabled. Disabled markers detach only after the native position
calculation, which needs their parent pane. All 70 primary save/backup/transaction files remained unchanged.
