# Zone handling audit

The client used to rebuild each zone's Population from `COMMON.DAT` on every
visit. Event latches survived, but items and creatures did not: collected
pickups returned, dead enemies revived, injured enemies healed and activated
ambushes could become dormant again. The JAIL4 zone 3 return regression failed
19 checks before population history was added.

## Retail evidence

These addresses were read from the user's PAL executable, SLES-01534, using
`q2psx-inspect disasm`; they are not inferred from the desktop implementation.

| Routine or call | Behavior |
| --- | --- |
| `OnLeavingZone`, `0x80078D1C` | Saves outgoing state before changing the zone. |
| `0x80078E5C` -> `0x800569C8` | Saves zone objects. |
| `0x80078E68` -> `0x80056F8C` | Saves Population group flags. |
| `0x80029078` | Saves the map's event latches; this was already implemented. |
| `OnEnteringZone`, `0x80078E98`, call at `0x80078F9C` | Restores objects through `0x800567FC`, before the zone callback. |
| Living-creature serializer, `0x8007D318` | Retains position, health, quantized yaw, target and spawn flags. |
| Item/corpse serializer, `0x80059B08` | Supplies the separate saved-object path for items and bodies. |

The retail format serializes objects; it does not keep native pointers or resume
an arbitrary C AI call stack. The desktop history follows that distinction while
keeping full coordinate and health precision.

## Implemented behavior

`src/game/zonestate.c` stores each Population object's state by map, kind, group
and authored slot. A hash of the Population bytes and difficulty prevents a
different layout or skill from inheriting unrelated records. Runtime array
indices are not stable identities: an item's slot can be reused after collection,
and the creature roster can change with the resident zone's modules.

On departure the client records selected items, including removed items whose
runtime slots have been reused, and creatures that actually existed in the
resident zone. Dormant and out-of-zone placeholders do not overwrite a saved
resident. On return it rebuilds the world, restores selected item groups and
object state, then wakes surviving creatures against the new world and restores
their live AI move after wake-up has installed the class callbacks.
Creature residency follows the saved position in the destination PrimaryColl.
Collision nodes, model bindings, actor caches and AI links are rebuilt.

Dynamic drops, explosion models, gibs and projectiles have a monotonically
allocated map-local identity. It survives an overlapping doorway without
duplicating the object; collection, expiry and slot reuse retire the old
identity. Airborne drops retain their velocity side table, initial draw origin,
bounds and remaining life. Landed drops retain their pickup think and countdown.
Gibs retain their velocity, rest flags and spin, including the retail case of
a body with no resolved model. A missing model or full physics/projectile pool
defers the record without deleting its history.

All projectile kinds retain velocity, damage, flags, fuse, bounce state,
attitude, rocket phase and BFG animation/beam timing. Creature shooters are
rebound by authored identity, even when the shooter is no longer resident.
Held grenades follow the player through same-map gates. Ordinary objects join
the destination when their saved position is inside its PrimaryColl; otherwise
they wait in their last zone. Collision-node caches are recomputed.

Creature snapshots retain the module-relative move, AI frame, next think,
attack/pain deadlines, pursuit targets and the fractional display cursor.
Death moves finish normally instead of freezing or restarting; detached bodies
keep their dissolve state. The map's AI clock, fractional tick, alert references
and player trail also survive. Unloaded objects are suspended: their remaining
deadlines are rebased on re-entry, while objects transferred through overlapping
geometry continue on the active clock. Timers stored as durations stay durations.

Collected items stay removed. Surviving creatures retain wounds, position, yaw,
target and spawn flags; casualties remain dead or removed. Activated ambushes
stay selected even when their original CREBATCH came from a repeatable event
which event-history replay does not rerun. A repeated CREBATCH cannot summon
their saved members again. Multiplayer continues to use its existing arena
spawn and respawn rules.

History is carried across both zone gates and map changes. Starting a new game
clears it without recapturing the abandoned game during the first load. Native
saves contain an additive `ZPOP` chunk with a versioned, bounded, explicitly
little-endian payload. Version 2 adds full object runtime and map clocks;
version 3 also retains direct-think creature callback identities, render scale
and the projection's consumed pain response. The resident `CAIS` chunk likewise
uses version 3 for these fields. Versions 1 and 2, and files with no ZPOP, remain
readable. Older saves
cannot recover motion or populations which were never recorded. The resident
save reconstructs dynamic entity slots before applying its existing ENTS/ITEM
state, then rebinds the full runtime. No model, actor, move or callback address
is written to the file. Resident projectile slots retain their processing order;
collision nodes are rebuilt. History copies own their runtime records independently.
An empty creature roster no longer discards the zone model bank or bypasses
runtime restoration (BOSS2 zone 0 exercises this path).

The loader now opens both the destination zone and COMMON archive before
changing the live mission row, loading-screen counters, transition serial,
shocks or outgoing history. A missing or unreadable archive leaves the current
world intact and clears the failed carry request. A local copy also protects
the frequent `client_load_zone(c, c->map, ...)` call from overlapping string
copying. Same-map player simulation carries its tick count, fractional dt phase
and item-world time along with the existing level clock.

## Validation

`test_client_zones` always runs a synthetic history test. Its disc mode also
uses the real loader, fixed headless rendering and the user's assets:

```powershell
.\build-msvc\bin\Release\test_client_zones.exe --headless .\.install\disc\game.cue .\.agents\tmp\zone-runtime\after
```

The tests cover reused entity slots, roster reordering, out-of-zone placeholders,
Population identity mismatch, JAIL4 collection and casualties, a cold save/load
in another zone, a full map round trip, a new game, failed loads, fractional
simulation timing and BASE1's LiftRoom ambush. The two JAIL4 captures use the
same camera: the health pickup visible before collection remains absent after
leaving, saving, reopening the save and returning.

The runtime regression adds airborne/landed drops, an explosion, an unbound gib,
all five projectile kinds and a held grenade, a firing creature and a creature
partway through death. It saves in both the unloaded and resident zones, then
checks exact state and the next physics/animation step against the uninterrupted
state. A corpse-dissolve round trip, removal followed by slot reuse, repeated
BASE1 overlap crossings and a full toss-table deferral test guard against lost
objects and duplication. The runtime image pair shows drops and projectiles
before departure and after a cold save/load and return.

`test_save` additionally checks multi-map history encoding, removal/corpse/item
fields, v1 migration, independent runtime copies, older files without ZPOP,
duplicate chunks, unsupported versions, malformed runtime flags, truncation
and bounded object counts. The disc harness checks all 21 multi-zone
maps' gate destinations and the 19 maps with walkable gates. PODCITY and THEVAT
have no inter-zone gate and are explicitly skipped by the crossing test.

## Scope

This is native continuation state, not bit-for-bit compatibility with the
console's packed save representation. Transient particle and audio tails are
not archived; restored objects emit their ongoing effects on subsequent ticks.
Creature-module and LevelBin handler coverage and limits
are tracked in [CREATURE-AUDIT.md](CREATURE-AUDIT.md) and
[ENTITY-AUDIT.md](ENTITY-AUDIT.md).
