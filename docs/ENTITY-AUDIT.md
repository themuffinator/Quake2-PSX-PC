# Map entity spawning and mission behavior audit

This audit checks population selection, placement, lifetime and the named
native mission handlers against the user's PAL SLES-01534 disc. It does not
establish complete parity for every LevelBin callback or creature class.

## Corrected behavior

| Case | Result |
| --- | --- |
| `Zone0Room2`, `Zone1Key`, `Zone2_1` and other suffixed groups | Remain dormant until their authored CREBATCH, unless explicitly selected by LevelBin. A zone-name prefix describes ownership, not automatic activation. |
| Repeated CREBATCH | Cannot revive a freed creature or a record rejected by the resident hull/module/skill filters. Eligible overlapping-zone groups still work when explicitly selected by name. |
| Creature and pickup startup | Share the client's resolved group selection, including groups with empty place lists. |
| BASE3 and MAGDEMO `DoneDish` | Persists the executed mission item; subsequent zone-1 startup selects `Zone1a` rather than selecting `Zone1` again. Existing survivors, casualties and pickup state remain governed by population history. |
| BASE3 and MAGDEMO laser wall | Restores both damaging beams and the contact volume in zone 3. The tagged Gunner's completed death disables the wall, with cold saves and zone/map returns preserving completion. |
| Ordinary map laser damage | Every enabled LASERBEAM runs its actor sweep after visual submission. Damage survives hidden or exhausted rendering; creature reactions are drained immediately. Script gates survive saves and zone returns. |
| LAB's shared lifts reset on zone entry | Lift A retains motion between zones 0/3 and Lift C between 2/4. Their displacement, state, wait timer and collision boxes survive zone/map changes and cold saves, including while a lift is unloaded. |
| JAIL3 return populations | Zone 4 selects `Jail5Return` only with the red pyramid key; zone 5 selects `Jail4Return` only with the purple key. Other zones do not select either group at startup. Authored CREBATCH calls can still activate them. |
| COMMAND white-key return | Zone 4 skips its startup population when the player already holds the white key. Acquiring it after entry does not remove the resident population or change a saved allocation order. |
| JAIL3 `BothKeys` | Updates the authored help text and enables/disables `PyramidOpen` using the retail key predicate. The named script retains its individual key checks. |
| JAIL3 `Bridge` | Creates the lowered bridge's solid floor and rewrites `DrawBridge`'s Scene culling bounds. The authored script still owns its rotation. The solid and bounds survive saves and zone revisits without duplicate boxes. |
| COMMAND `Comp1` / `Comp2` | Persists both computers; completing the pair enables `LasersOff`, disables `TheLasers` and updates the mission popup. |
| WASTE2 / WASTE3 pumps | `Pump1On`, `Pump2On` and `CheckPumps` share persistent pump state across maps, update the appropriate mission text and trigger `WasteFill` when both are on. |
| BOSS1 stairs and lasers | `Stairs0` / `Stairs1` complete `Steps` together. `LaserButton0` / `LaserButton1` independently stop their static and sweeping beam pairs, including damage, particles and lights. |
| BOSS1 entrance lasers | Zone 0 runs its separate five-beam bank. Completing each named laser record turns that beam on; `DoorBlown` stops all five. Script state survives cold saves and zone returns. |
| LAB `Laser0` / `Laser1` | Starts each timed sweep only when idle. Both sweeps advance independently, damage actors once per logic tick and can be rearmed after completion. Loading a save resumes the recorded phase. |
| LAB and BOSS1 sweep sparks | Endpoint bursts use the native cached draw areas, including LAB endpoints outside the collision hull. Both sweeps finish before scripted actor use; signed LAB expiry phases survive saves. |
| SECURITY `OrbActive` / `LOCKLIFTS` | Restores the orb's bobbing Scene transform, two vertical beams, five moving arcs and light; destruction raises the explosion and mission completion once. Locked lifts receive `LIFTA` / `LIFTB` each logic tick. |
| SECURITY `HexLift` and orb frame order | Holds the exit lift at its upper landing after the player crosses the native height threshold. All five arcs update before damage, remain active on their expiry frame, and retain their signed timers through saves. |
| SECURITY boss encounter | Both Tank Commanders must finish dying before `Boss1` activates. Four timed wall explosions follow; the boss's completed death opens the hatch. Timers, queued events and completed stages survive cold saves and zone returns. |
| BOSS2 train exit | The rider's completed death selects `Zone4`, enables `DoorToTrain`, disables `DoorString` and raises the final mission popup once. Killing Jorg alone does not unlock the train. |
| BIGGUN `STOPPLATFORM` | Sets the resident zone-2 platform's wait timer to `0xFFFF`. It finishes travelling and then holds, rather than freezing in midair. |
| JAIL5 `BUTPRESSED` | Explicit retail no-op; the exported function is `jr ra / nop` and is not referenced by the shipped Events records. |
| Pickup floor placement | Includes the initial 30-unit nudge even for no-drop items, adopts the sweep's final Y even when it reaches its full distance, and rejects records that cannot be placed in the resident hull. |
| Static selector output | Returns the number actually written, including zero-capacity calls, so bounded callers cannot read beyond their arrays. |

Saves made after obtaining a key or completing the dish but before leaving a
zone restore their recorded population. They do not prematurely rerun startup
with the newly changed mission state.

## Evidence

The retail selector at `0x80056C60` matches exact twelve-byte group names. The
spawn pass at `0x80056D68` consumes selected groups once. The client now records
an explicit `spawn_pending` latch rather than interpreting every inactive
creature as an unspawned one.

BASE3's zone-1 callback at module `+0x194` branches on mission word `+24`, choosing
the names at `+0x18` (`Zone1a`) and `+0x24` (`Zone1`). `DoneDish`, module `+0x8A0`,
sets that word to one. MAGDEMO ships the same conditional logic.

JAIL3's callback table at `+0x1200` places handlers `+0x290` and `+0x614` in slots
4 and 5. They test bits `0x20` and `0x40` through engine import `+252`, whose
target is the key-flags word at `0x800B29D0`. `BothKeys` at module `+0xC34` passes
`(keys & 0x60) == 0` to import `+160` (`0x80027B04`), which writes the named
record's disabled bit. It does not directly trigger the pyramid movers.

JAIL3 `Bridge`, module `+0xFF0`, allocates a solid through import `+128`
(`0x800555D8`) and copies the box into the named Scene record through import
`+228`. Its bounds are `(-10878,-7039,6639)` to `(-7839,-6839,7919)`.
These are solid and culling bounds, not trigger bounds. Zone-1 restoration
repeats this work at module `+0x1198`.

The other decoded entry points are COMMAND `+0x9D8` / `+0xA60`, BOSS1
`+0x1D64` through `+0x1DB8`, LAB `+0x1628` / `+0x1648`, SECURITY `+0x2548` /
`+0x255C`, and BIGGUN `+0x2F20`. The pumps are engine functions `0x80024134`,
`0x80024170` and `0x800238AC`. Their per-frame consumers were traced as well
as their setters: a stored switch alone does not implement a laser or an orb.
The mission-beam import `0x80048DC8` calls actor sweep `0x800544EC` directly;
unlike ordinary bullets, it does not first shorten its segment against walls.

The 26 October selector review inspected 83 import-`+36` call sites in 21
campaign/demo LevelBin modules. COMMAND's zone-4 callback at `+0x694` tests
key bit `0x800` at `+0x6B4` and branches past the `Zone4` call at `+0x750`.
SECURITY's frame callback at `+0x1DF8` waits for the first kill counter before
selecting `Boss1` at `+0x20AC`, then waits for the second before triggering
`ExplodeHatch`. Its wall sequence at `+0x142C` uses delays 600, 1050, 1500 and
1950 in 1/300-second ticks, advancing only in zone 1.

Those counters are not ordinary live-monster counts. Unit 2 seeds two deaths
for Population flag `0x200` and one for `0x400` at `0x80023280`. `RegisterKill`
(`0x8007CD24`) consumes those tags at corpse detachment (`0x8007F098`) or the
dead-actor departure sweep (`0x8007F698`), rather than when health first crosses
zero. The port derives completed deaths from stable population identities and
their zone history, so revisiting or restoring cannot count one casualty twice.

BOSS2's `+0xD30` branch uses the second counter and the unit completion word
at `+28`; `+0xE04` selects `Zone4` before opening the train route. Jorg's child
constructor writes its own `0x400` flag at creature-module `+0x31E8`, separately
from Jorg's authored `0x200`. The rider's reserved child identity is retained
across saves and zone departure. JAIL5's `QuadInCrate` selector has no matching
Population group on this disc and is an inert authored call.

### POWER2 reactor and laser callbacks

The registered frame function at `+0x11A0` was missing from the client. Both
pumps now start the reactor sequence in zone 0, raise `Unit3Curr5/Unit3Miss2`
and trigger `DrainCool`. The drain's one-half-stroke PISTON lowers two coolant
surfaces by approximately 314 units using the original sine integrator.
`+0x88C` consumes one glass deadline per callback: 4100, 4200, 4700, 4750,
4800 and 5100 ticks. It emits the kind-2 beam, explosion and ten debris pieces,
hides the named glass and stops the core beam at the first successful break.
At 10000 ticks it triggers `ExploDoor` and marks the reactor complete.

The intact core uses the module's own ten red beam faces at `+0x1B1C`, radius
256, and the alternating fifteen-spark emitter at `+0xF20`. The alarm uses the
native strict `>300` threshold. Zone-entry functions `+0x52C`, `+0x604`,
`+0x6DC` and `+0x7B4` finish a sequence left behind; returning to zone 0 hides
all six glass nodes and reconstructs the drain and broken exit without a new
explosion. Cold saves instead preserve their exact timer, consumed deadlines,
pending events and piston phase.

The same callback disables `RockLaser` after the tagged `Zone3/2` Infantry's
completed death. In zone 4, `Laser1` and `Laser2` alternate on a 512-tick cycle,
with both disabled for the first 47 ticks of each half. As established by the
later shared-frame audit below, the frame handler runs after ordinary beams;
its bank changes take effect on the next beam walk.

Image comparison caught a second defect after the callback began running:
the core was queued but emitted no faces. Native `0x80063650` passes a collision
cell to `0x80044C44`; the port's effect renderer takes a resolved draw area.
Passing the cell index directly silently culled the beam. POWER2 and the
shared LAB/BOSS1/SECURITY helper now supply the cell's area byte. The capture
shows the restored red core, and the regression checks emitted faces as well
as queue contents.

The init-registration census covers all 21 campaign/demo modules. COMMAND
installs no frame function; unit 4 initializes its `0x400` counter to zero at
`0x80024524` and the level never consumes it. The earlier suggested COMMAND
boss-counter gap was a false lead.

### POWER1 blasts and the BIGGUN secondary hook

POWER1's missing `+0x5F0` frame callback now repeats the two zone-0 explosions
every 900 ticks and runs the zone-1 shaft hazard at `+0x414`. The shaft advances
one phase every 100 ticks through a 16-step cycle. Phases 1 through 7 emit blasts
at Y 4600 through 1000; phase 0 and phases 8 through 15 are silent. If player
zero's entity-origin Y is at least 5200, only phases 1 and 2 emit. Both clocks
use strict unsigned `next < now`, with at most one advance per callback.

The shaft calls ordinary `T_Damage(player0, player0, 30, 10, blast)` inside a
strict 1000 by 3000 by 1000 box, preserving armour and the lava damage throttle.
The port converts player feet to entity-origin Y for both the height gate and
damage bounds. Zone entry resets the deadline but retains the phase; a map
change clears both. Cold saves retain both rather than restarting the sequence.

The shared damage path now also queues its acid/lava burn sound. Retail's lava
counter starts at 3 and increments before testing, so the first, fifth and ninth
admitted hits sound. The count is shared across players and retained across
loads; the request targets the damaged player. POWER1 carries `pla_burn2`,
selected by retail's explicit fallback when `pla_burn1` is absent. A final
client event drain delivers the shaft callback's request before the next tick
clears it. BIGGUN's acid damage uses the same hook without a duplicate cue.
See [the audio audit](AUDIO-AUDIT.md) for protection order, bank coverage and
actual mixer evidence.

Both entrance origins are outside the shipped PrimaryColl hull. Retail still
looks up byte +32 of node -1: `(chunk + 4) - 36 + 32` is the low byte of the
chunk's node count. The native handler computes that byte safely and spawns the
explosion, including its sound and shock, instead of suppressing it or moving
its authored origin into the map.

`MakeBlitzes`, the frame callback's unit-counter target, is absent from COMMON
and all three zone Events directories. The call is inert regardless of the
unexplained second unit-3 kill counter. No new progression dependency is needed.
BIGGUN's `+0x34FC` simply calls its lightning draw at `+0x138`; its eight noise
accumulators, textured bands and shared frame UV clock already run through
`q2_fx_glint_lightning_tick/draw`. The existing generator/evacuation fixture was
rerun and passes 117 disc-backed checks, including emitted lightning primitives.

### BOSS1 entrance callback

The frame function at `+0xCF8` reads byte 4 of the resident `ZONE0` name
through import `+240` (`0x800E465C`). Its `'0'` branch calls `+0x1708`, skipping
the zone-1 sweep velocities, phases and projection-use callback. The previous
client always took the zone-1 path, leaving the entrance's red lasers absent.

`DoorBlown` must be enabled for this bank to run. Each of `LaserNW`, `LaserNE`,
`LaserSW`, `LaserSE` and `LaserMid` must instead be **disabled** to emit its
beam, the opposite of ordinary LASERBEAM raisers. The module passes kind 4,
radius 64, damage 512 / MOD 11, and no endpoint particles. Names and endpoints
are at `+0x1E6C`, `+0x1EA8` and `+0x1EE4`. Its cell argument is -1;
`0x80044C44` resolves each origin before drawing. The client supplies that
cell's draw area and retains the actor-only damage sweep.

The regression reads the names and endpoints from the actual LevelBin, checks
all 32 beam combinations, verifies one hit per logic tick and no extra RNG or
projection activation, and executes the authored one-shot records. Disk saves
and a zone-1 departure/return preserve the bank's event flags. Disabling laser
rendering still allows damage. The independent zone-1 switch test now names
zone 1 explicitly rather than searching for any zone that emits beams.

The focused test passes 815 checks; removing the new dispatch branch fails
112 assertions. Identical headless cameras produce zero beam faces before
and 260 afterward. Captures and logs are retained in
`.agents/tmp/functional-parity/boss1-entrance/`. This establishes the omitted
callback branch, not whole-frame equivalence to PlayStation hardware.

### BASE3 / MAGDEMO laser wall

BASE3's frame callback at `+0x3F4` reads unit 1's first kill counter through
import `+148`. Initialization at `0x80022A38` seeds one kill; the tagged actor
is `Zone3/1`, Population class index 10 (Gunner), flags `0x0205`. RegisterKill
consumes `flags & 0x600 == 0x200` at completed death, including departure during
the death animation. Counter zero or saved mission word `+32` disables
`LASERWALL1` every frame and latches completion. The port now reconstructs this
flag before the first draw on load. It occupies bit 23 of NMIS's existing flags
word; prior saves remain readable without changing the 160-byte layout.

The shared constructor `0x8002E1F0` registers at most eight walls. Its 24-byte
item contains axis/count at `+4/+5`, signed rise/length/spread at `+6/+8/+10`,
offsets at `+12/+14/+16`, Scene index at `+18`, and contact damage at `+20`.
The draw walk `0x8002A02C` uses the live Scene bounding-box midpoint, truncating
negative odd sums toward zero, subtracts the Y offset, and shares the first
row's collision cell across the whole wall. It emits kind 0 with no endpoint
particles: radius 16, damage 512, MOD 11. A disabled raiser skips all rows.

Both maps register only in zone 3: Scene node 123 in BASE3 and 126 in MAGDEMO.
Their beams run from `(7048,2320,4000)` to `(12018,2320,4000)`, and the second
row is at Y 2820. Their separate 200-damage trigger was also ineffective:
the old contact-hazard reader used COMMON's `-1` object index. Hazard binding
now reads resident-zone operands and preserves self-attribution and the
entity-origin contact point. Zones 0–2 retain the absent-object gate.

MAGDEMO has matching geometry, tagged population and callback structure, but
targets an older 1260-byte import ABI, rather than retail's 1268-byte ABI.
Its counter and EventDisable offsets are four bytes earlier. Supporting its
authored behavior does not imply its raw module could run against the retail
import block unchanged.

The `--laserwalls` fixture passes 677 checks covering both maps, exact rows,
beam and contact damage, disabled rendering, event gates, cold saves while
alive/dying/completed, zone/map returns, departure during death, and the actual
headless frame caller. Unit checks cover bounded registration, signed geometry,
moving emitters, invalid axes and missing nodes. Matching engine captures show
84 beam faces after the fix and 4,477 changed pixels. Evidence is retained in
`.agents/tmp/functional-parity/laserwall/`.

### Ordinary LASERBEAM damage and shared frame order

`DrawLaserBeams` (`0x8002EE38`) tests the raiser's disabled bit and calls
`0x80048DC8` with the cached cell, authored endpoints/kind and `ends=0`.
That last argument suppresses endpoint bursts only. The routine still calls
the actor-only segment sweep at `0x80048F8C` and damage at `0x80048FB4`, with
a null attacker. Kinds 0..5 use damage `{512,1,512,1,512,512}` and MOD
`{11,16,12,16,11,12}` respectively. Only the nearest actor is struck; world
geometry does not shorten the authored segment. Visual packet exhaustion
does not suppress damage. The client now performs and drains each hit before
advancing to the next beam.

The disc has 72 authored items: 71 register in at least one zone, while one
zeroed JAIL2 item never registers. Checking every zone yields 77 resident
instances across POWER1 (6), POWER2 (14), JAIL2 (19), JAIL3 (4), COMMAND (2)
and SECURITY (32). The `test_client_effects --headless DISC OUTPUT --lasers`
fixture passes 764 checks covering every resident segment, independent
damage/MOD constants, render-only and hidden/full-pool paths, disabled
records, creature pain and health persistence, the real COMMAND `LasersOff`
switch, cold disk saves, zone returns and clearing the set on QFMV entry.
Its actual headless frame caller reproduces JAIL2 health 600 before the fix
and 88 after a single red-laser hit.

The same review established retail's shared stage order at
`0x80038FA4..0x80038FD4`: teleports, laser walls, ordinary beams, then the
native callback loaded from `0x800B2D8C` (import `+100`). The client previously
called the native callback first and teleports last. It now matches the
measured order, including teleports' 45 random draws with no pads. POWER2's
zone-4 fixture checks the one-frame bank transition: ten beams before initial
shutdown, none before the clock enables `Laser1`, then that bank's six beams.
Evidence and before/after engine captures are retained in
`.agents/tmp/functional-parity/static-lasers/`.

### LAB's shared lift state

The LAB frame callback ends with `+0x3D0`, which records two lifts in persistent
unit save memory. `+0xD8`, called by all five zone-entry callbacks, finds
`Lift A` and `Lift C` by Scene name and restores the resident one after ordinary
event replay. The 76-byte native entries hold a rebound runtime pointer, the
object's signed displacement (`+0x14`), wait timer (`+0x4E`) and state (`+0x52`),
plus the pusher's live bounds, broad-phase envelope and last displacement.
Absent lifts leave their saved entries untouched.

Lift A is Scene node 30 in zone 0 and 25 in zone 3, with item offset 216 and
travel -4450. Lift C is node 26 in zone 2 and 23 in zone 4, item offset 120,
travel -4435. Both start at Y 4480..4639 and move at speed 4. The port's event
replay reconstructed them at rest on every entry. The failing control moves
each lift to offset -3600, then crosses to its other zone; both reset to zero
instead of continuing to move.

The client now records both entries, rebinds by Scene name and restores after
the seven replay passes. Collision restores the exact saved envelope and last
vertical step; the next update measures movement from the restored offset,
avoiding a spurious 3600-unit pusher step. Capture also runs at a successful
departure and saved-game capture so the latest committed pose is retained.
Failed loads leave the entries intact, and new games clear them.

Testing older saves exposed a shared restore omission: MOVR applied each
mover's visible displacement but did not update its collision box. Collision
stayed at its reconstructed starting position until the next tick, which then
treated the entire saved displacement as one movement step. Saved-game apply
now rebuilds all mover boxes immediately and clears that false delta. A v5 LAB
snapshot then restores its exact prior step; older files use zero because
MOVR never recorded it. The legacy regression failed four assertions before
this correction, including the next tick's 80-unit motion.

The `--lablifts` disc fixture checks both directions of both seams, collision
traces, a subsequent motion step, open/waiting and closing states, cold saves
in the lift zone and in zone 1 where neither lift exists, leaving LAB and saving
in JAIL2, and new-game reset. Matching headless engine captures show Lift A
reset at the bottom before the fix and retained at its raised position after.
Evidence is under `.agents/tmp/functional-parity/lab-lifts/`.

### SECURITY and boss-map lift continuity

The same 76-byte native save-memory pattern also appears in three other
LevelBins. All five lifts below span zones 0 and 1; SECURITY and BOSS2 have a
zone 2 in which their lifts are absent. BOSS1 has only two shipped zones.

| Map / Scene name | Restore / record functions | Scene nodes in zones 0 / 1 | Travel / speed / wait |
| --- | --- | --- | --- |
| SECURITY / Lift A | `+0x168` / `+0x460` | 223 / 97 | -6080 / 5 / 600 |
| SECURITY / Lift B | `+0x168` / `+0x460` | 231 / 105 | -6080 / 5 / 600 |
| BOSS1 / Lift A | `+0x130` / `+0x424` | 274 / 6 | -4860 / 4 / 300 |
| BOSS2 / LiftStemA | `+0x11C` / `+0x414` | 79 / 91 | -4096 / 3 / 900 |
| BOSS2 / LiftStemB | `+0x11C` / `+0x414` | 88 / 100 | -4104 / 2 / 900 |

SECURITY and BOSS2 record at callback entry (`+0x1E08` / `+0xAB4`);
BOSS1 records at the tail (`+0x16D0`). Zone-entry helpers rebind the named
Scene and restore displacement, state, wait timer and pusher geometry after
ordinary event replay. The port was missing all three pairs of helpers. The
failing control observed every lift reset to idle at offset zero after moving
3000, 3000, 2400, 1800 and 1200 units upward respectively.

The client now shares the proven LAB mechanism across all seven named lifts,
with distinct persistent entries even when maps reuse the name `Lift A`.
BOSS2 also writes its object's `+0x30` partner displacement. Its port mover
contains both parts, so restoration derives both collision boxes and their
envelopes relative to the saved named part. This avoids a one-frame mismatch
or a false large vertical step in either part.

The `--nativelifts` fixture checks all shipped zones, both directions across
each seam, both BOSS2 parts, collision traces, next-step displacement, open
wait timers, closing motion, cold saves while resident and absent, map
departures, failed loads, multiplayer/draw-only guards and new-game reset.
A combined cold save retains all seven lifts and verifies that their state
does not alias across maps. Evidence and matching SECURITY shaft captures
are under `.agents/tmp/functional-parity/native-lifts/`.

### SECURITY and BOSS2 earthquakes

Both native callbacks were missing physical earthquake impulses. Import
`+260` is `GameWindows` (`0x800D5C30`); its first window's `+0x120` points to
player 0. These routines add to entity `+0x2F8/+0x2FA/+0x2FC` and arm
`+0x10C & 0x4000`. They do not write the camera's kick fields or replace the
player's velocity. The next movement integration consumes the impulses.

| Map | Quake routine / module clocks | Start and stop |
| --- | --- | --- |
| SECURITY | `+0x590`, active `+0x2654`, phase `+0x276C` | Two completed Tank Commander deaths set the flag with wall timers at `+0x2028`; the fourth wall clears it at `+0x1594`. |
| BOSS2 | `+0xADC..+0xD28`, clock `+0x14D0`, phase `+0x14E8` | Clock begins at `-9999`; the rider's completed death writes zero at `+0x1048`. Pulses start above 300 ticks; above 3000 ticks the clock becomes -3000. |

Each active frame adds `dt` to the pulse phase and emits while positive,
subtracting 30 after each pulse. Each pulse draws X/Y/Z offsets from
`rand()-16384`, divides Y by 16 and X/Z by 6 (SECURITY) or 12 (BOSS2), truncates
toward zero, and wraps the accumulated impulses at 16 bits. Its fourth random
draw gives `(rand() & 4095) + 1024` for nonpositional Explosion rumble on all
windows. The fifth draw plays `wep_grenlx1a` at the player's entity origin when
below 8192. A missing player skips the quake; zero health does not.

Quakes run before progression checks, so the trigger frame cannot pulse and
SECURITY's final wall frame can. SECURITY continues shaking in zones 0/2
while its wall timers pause outside zone 1. BOSS2 preserves the pulse residual
during its quiet interval. Ordinary zone gates retain these clocks; full
loads, including Restart/Resupply within the same map, reset them. SECURITY's on-disc timer storage is zero,
so this also fixes pending wall timers incorrectly surviving a module reload.
Unit completion flags still prevent restarting an already completed sequence.

NMIS v7 appends the three signed clocks (592 bytes total). Versions 1–6 remain
readable: missing pulse phases become zero, and an unrecorded BOSS2 clock uses
the inactive sentinel. The `--quakes` fixture checks real boss death gates,
native numeric vectors, halfword wrap, cadence and strict thresholds, guards,
real sample decoding, cold saves, all three zones, module unload and new games.
Matching fixed-step engine frames show actual player displacement. Evidence
is in `.agents/tmp/functional-parity/earthquakes/`.

### SECURITY laser course

The zone-0 branch of SECURITY's frame callback (`+0x1F98`) calls
`+0xE90..+0x1428`. That handler was missing: all 24 named gates remained
enabled and its two moving beams were absent. Import `+236` reads unsigned
level time (`0x800AEBAC`); import `+232` reads the world frame counter
(`0x800B2DE4`). The low counter bit selects left on odd frames and right on
even frames, independently of render-frame parity.

| Gates | Native level-clock pattern |
| --- | --- |
| `LaserBank LA..LF`, `RA..RF` | `(time % 600) / 30`; even phases are dark. Odd phases select A, B, C, D, E, F, E, D, C, B. |
| `LaserRing LA..LD`, `RA..RD` | `(time % 200) / 50` selects A through D. |
| `LaserGrid LA..LB`, `RA..RB` | `(time % 300) / 150` selects A or B. |

`LaserRing CA/CB` are independent and retain their authored state. The callback
writes DisableEvent after the shared LASERBEAM walk, so those gate changes
affect the next frame. A repeated render at the same clocks is idempotent and
cannot deal damage. No random draws are needed.

Module data at `+0x25DC` supplies four origins: X is 79 on the right and -3920
on the left, Y is -5921, and the two rows use Z -15040 and -11840. The same
counter bit chooses the side. The target X is 1276 or -5120 respectively.
Let `s = (time % 1800) * 8000 / 1800`, reflected to `8000-s` above 4000.
For `s < 800`, the target Y is `-5121-s` and Z stays at the row origin.
Otherwise Y is -5921 and Z is `s-15840` or `-11040-s`. Each segment uses
kind 1 (radius 16, damage 1, MOD 16), the origin cell's draw area, and no
endpoint sparks. Both hits apply where the segments cross; a creature in
front absorbs its segment's hit and its result is drained immediately.

The `--security-lasers` fixture checks literal masks at 48 time boundaries on
both sides, 13 endpoint phases, unsigned wrap, every gate, damage with hidden
or exhausted visual pools, creature interception, callback order and guards,
cold saves with and without NMIS, both zone seams, and the real headless
simulation path. Existing saved clocks and event flags are sufficient; no
private state or new save format is introduced. The failing control, native
arithmetic checks, matching renderer captures and logs are retained under
`.agents/tmp/functional-parity/security-lasers/`.

### SECURITY orb and exit lift

The zone-2 frame branch calls the orb at `+0x7D4` while unit 2's word `+40`
is set, then calls `+0x15C8` independently of orb activity. The latter callback
was missing. It compares `HexLift`'s current Y offset with its target and sets
unit 2's word `+44` once player 0's entity-origin Y is strictly below -8899.
Once set, it clears the object's tick callback and pins its offset to the
target (-1440 on PAL). The port retains this flag across zone/map changes and
cold saves, then restores the held mover and its translated collision box
before simulation resumes. This ownership differs from BASE0's crates, whose
drawn positions move while their construction collision boxes remain fixed.

The same native callback hides `LiftRing1` at full travel. PAL zone 2 instead
contains `ring` (node 208), with no `LiftRing1` entry. Import `+132`, resolved
to `0x8006CF48`, performs an exact name lookup and returns -1 when absent.
The port preserves that lookup and safely skips an absent node; it does not
invent a `ring` alias or reproduce the native out-of-bounds Scene write. The
fixture supplies a temporary in-memory name binding to test the conditional
visibility without changing disc assets.

The orb updates all five trajectories before submitting their damage beams
(`+0xD0C`). Previously, an early arc hit could consume effect RNG before a
later arc renewed. Each arc now advances once, subtracts the frame duration
from its signed timer, and is submitted even when that subtraction reaches
zero or becomes negative. The minimum reachable residual is -29; renewal
happens on the next logic tick. All seven beams use the fixed orb-origin
cell resolved at zone entry, even while the sphere bobs. Renderer-only calls
preserve trajectories and cannot deal damage. Zone reentry clears only the
five lifetimes (`+0x1A98`); a cold save preserves the recorded trajectories.

The `--security-orb` fixture checks strict height boundaries, lift ownership
and collision after cold loads, absent-zone and other-map saves, legacy saves,
new-game reset, ring visibility and callback guards. It also checks expiry
damage with hidden or exhausted beam pools, signed-timer round trips, damage
RNG ordering, and fixed cell/radius values across seven bob phases. Native
disassembly, the failing control and matching headless before/after captures
are retained in `.agents/tmp/functional-parity/security-orb/`.

### LAB and BOSS1 sweep endpoint effects

LAB's zone-1 callback caches both emitter cells and their area bytes at
`+0x6C0..+0x724`. The frame callback passes those bytes directly to its two
endpoint spark spawns (`+0xDA8` and `+0x10D4`). The port instead looked up the
moving endpoints: both fall outside PrimaryColl on the measured first frame,
so neither burst was created. Their native emitter areas are 34 and 33.

BOSS1's zone-1 callback stores one shared spark area at `+0x1E5C`, taken from
the second sweep emitter (`+0x8C8`). Both spawns (`+0x10A4` and `+0x1500`)
read that same value, area 9 on PAL. Looking up either moving endpoint gave
area 12 instead, changing the render batch and its visibility/order. The
helper now accepts the caller's authored area without another endpoint lookup.

Both modules generate the same fifteen velocity vectors before any beam:
X and Z use `(rand()-16384)>>8`, Y uses `(-rand())>>13`. Groups use ramp 0
twice, size 10000 and zero acceleration. LAB's lifetime is 10 and a beam hit
suppresses its endpoint burst; BOSS1's lifetime is 5 and it emits the burst
even after an actor hit. Both retain their existing two dynamic lights.

LAB stores `phase - 2*dt` without clamping (`+0xC6C`/`+0xF98`) and still emits
the final beam. With a maximum dt of 30, the last phase can be -59. The port
now retains that value through the existing signed NMIS field, skips later
inactive frames, and allows the normal mission call to rearm it to 8192.
The actor-use helpers run after both sweeps (`LAB+0x129C`, `BOSS1+0x16C8`),
before the lift recorders. Renderer-only refreshes do not consume that use.

The focused `--native-sweeps` fixture checks module-sourced endpoints and
emitter areas, literal BIOS-seeded velocities, both ramps and lifetimes,
expiry damage, hidden/full beam pools, cold expiry saves, rearming and actor
callback order. The initial control failed five checks. A second control
temporarily restored only the old endpoint lookup to capture seven matching
fixed steps; it reproduced both original first-frame images pixel for pixel.
The restored source, final tests and real renderer images are recorded under
`.agents/tmp/functional-parity/native-sweeps/`.

### Module state across gates and full reloads

Full level loads pass through `0x80070D84` → `0x80070C94` → `0x8007956C`.
The call at `0x800795D8` reloads COMMON through `0x8007A430`, and `0x80070F64`
calls LevelBin export 0. This includes Restart and Resupply even when the
directory stays the same. The port had reset module clocks only when the map
name changed, leaving live sweep beams, quake timers and reactor/blast phases
behind after a restart.

Ordinary zone gates instead use `0x8007901C` → `0x800793B8`, whose
`0x800794D8` loads only the zone. Entry at `0x80078E98` calls the indexed zone
callback at `0x80079004`; it does not rerun export 0. LAB's used-commander word
`+0x1708` and BOSS1's used-projection word `+0x1F8C` are cleared only by their
module initializers (`+0x1414` and `+0x1B9C`). Clearing them at every zone gate
incorrectly allowed another actor-use attempt on return.

The client now resets module state on successful full loads, retains it on
ordinary gates, and restores the recorded state on saved-game apply. Explicit
zone-callback writes still apply: POWER1 clears its next-blast deadline, and
SECURITY zone 2 clears arc lifetimes. Unit save words and lift snapshots remain
persistent. Refused loads leave the running state intact. The NMIS layout is
unchanged.

The `--native-lifecycle` fixture covers LAB, BOSS1, SECURITY, BOSS2, POWER1 and
POWER2. It distinguishes real carry-flag zone transitions from full loads,
checks cold saves and refused loads, and invokes the actual Restart/Resupply
menu requests without host input. All 185 focused checks now pass. Its
original control failed 17 assertions; LAB still emitted two sweep beams
after Restart. Matching engine captures and
the native loader/module evidence are in
`.agents/tmp/functional-parity/native-lifecycle/`.

### The apparent buried items

The earlier JAIL4 reward capture used a diagnostic camera at
`(-13480,8250,4350)`, outside the room's PrimaryColl hull. With no valid sort
cell, the renderer used fallback depth sorting and drew the floor over the
pickups. Moving the diagnostic camera inside cell 23 restores the authored
draw order. No item-height or model-offset workaround was applied.

The items' entity Y is 8674 and their lower extent is the floor at Y 8960.
The corrected capture and its regression assertions check both placement and
the camera's valid authored sort cell.

Retail does perform placement checks: `0x80050FA0` lifts the authored point
by 286 and another 30, then sweeps down; `0x800511C0` writes the final Y.
An unplaceable item is rejected by the constructor at `0x800588C0`. This is
spawn-time placement, not evidence of a general continuous item-unsticking
system. The port now respects the final sweep position and rejection result.

## Save compatibility

CAIS chunk version 2 adds one boolean `spawn_pending` after each monster's
existing move/thinking fields. The scalar-state layout remains unchanged.
Readers still accept version 1, preserving a fresh resident dormant record only
when the saved creature is inactive, not dead and has never started thinking.
Version 2 rejects non-boolean pending values and pending creatures marked live
or dead. Old saves cannot recover lifecycle distinctions they never recorded.

Mission calls retain their persistent event-object shadow in EOBJ/EVHS.
The optional `NMIS` chunk, version 7, stores native flags, both LAB and BOSS1
phases, the five SECURITY arc records, four wall-explosion timers and POWER2's
start clock, alarm accumulator and consumed-glass mask, plus POWER1's blast
deadline and phase, all seven native lift snapshots, and the three earthquake
clocks. The existing flags word also records SECURITY's held exit lift;
signed orb lifetimes accept the native final-frame range down to -29, and LAB
sweep phases retain their final decrement down to -59. These corrections do
not alter the chunk layout. Its explicitly encoded payload is 592 bytes
including the version.
Versions 1–6 retain their 124-, 140-, 152-, 160-, 280- and 580-byte payloads.
Missing fields become zero except the BOSS2 quake clock, which stays inactive;
malformed, duplicate or out-of-range state is rejected. The orb's Scene
offset is reconstructed from the saved level clock before the first post-load
draw. Population histories are retained by
the separate ZPOP support.

Older saves without NMIS remain readable. Available event history can rebuild
persistent switches, but it cannot recover transient native clocks or state
that an older build never recorded. In particular, files before NMIS v5 cannot
recover an absent LAB lift's last position, and files before v6 cannot recover
that history for SECURITY, BOSS1 or BOSS2. Older saves without the exit-lift
flag cannot recover a prior latch, but the normal threshold can latch it again.

## Verification

`test_client_population` runs a synthetic CREBATCH test without assets. Its
optional disc run exercises all 17 deferred suffixed groups on this disc,
BASE3/MAGDEMO dish state across saves and map revisits, and JAIL3's six zones
under all four pyramid-key combinations. It also checks JAIL3's native key
handler, the bridge's collision and Scene bounds, all native mission handlers
listed above, beam damage and timing, orb completion, lift triggers, and their
save/zone restoration. The expanded disc run passes 3,557 checks without capture
output.

The selector follow-up adds COMMAND's white-key branch, both SECURITY kill
stages, delayed wall/hatch execution after cold save, and both BOSS2 kill stages.
It also leaves the arena while the rider is dying and restores in zone 0 before
the progression callback runs. The focused `--selectors` run passes 259 checks.
Its evidence and before/after engine captures are under
`.agents/tmp/functional-parity/selectors/`; current full-suite results are in
[FUNCTIONAL-AUDIT.md](FUNCTIONAL-AUDIT.md).

The `--callbacks` fixture passes 205 checks with disk saves and engine captures.
It reproduces eleven missing POWER2 behaviours before the fix, then checks
both pumps, the actual coolant movement and exit geometry, all glass thresholds,
one-break-per-callback catch-up, paused/render-only and multiplayer exclusion,
alarm timing, cold saves, early departure, map returns, failed zone loads,
completed guard death and laser cycle boundaries. Evidence is under
`.agents/tmp/functional-parity/callbacks/`.

The `--power1` fixture passes 180 checks with a cold disk save and an engine
capture. Its original baseline failed six explosion/damage assertions. It
checks disc origins, absent `MakeBlitzes`, entrance sound requests and the
out-of-hull area byte, exact deadlines, large clock jumps, render-only/replay/
multiplayer exclusion, all sixteen phases, the player-height boundary, all six
damage-box faces, the lava throttle, cold save, failed zone load, zone changes,
map reload and the actual headless frame caller. Evidence and the shaft
before/after captures are in `.agents/tmp/functional-parity/power1/`.

The earlier native-mission pass completed 53 CTests. Its staged Release client completed
360 fixed-step headless frames on JAIL3 zone 1 with `DrawBridge` fired, and
120 frames each on LAB zone 2, BOSS1 zone 0 and SECURITY zone 2. All four exit
successfully without warnings or errors; JAIL3 reports its hatch open. The
version command starts successfully and executable/runtime hashes match the
build output. Logs are retained with this pass's evidence.

```powershell
.\build-msvc\bin\Release\test_client_population.exe --disc .install/disc/game.cue E:/Repositories/Q2PSX-PC/.agents/tmp/native-mission-parity
```

Item, creature and save unit tests cover exact/suffixed names, bounded selector
output, consumed batches, hull exclusion, full-distance and no-drop placement,
CAIS v1 compatibility, malformed v2 state and optional NMIS compatibility.
Simulation tests distinguish a mission beam's actor-only sweep from a bullet
stopped by a door, enforce the authored endpoint and check the nearest victim.
The client harness uses headless fixed-step rendering and writes the engine
framebuffer directly. Current evidence is under
`.agents/tmp/native-mission-parity/`: module disassembly, test/build/smoke logs,
corrected JAIL4 pickups, JAIL3 bridge before/after, LAB lasers and SECURITY orb
captures. Earlier population evidence remains under `.agents/tmp/map-entity-parity/`.

## Remaining coverage limits and other findings

The previously identified named MISEVENT handlers above now have native
implementations and regression coverage. This is not a general interpreter
for LevelBin: static scanning remains a fallback and does not track registers
or execute branches. Registered callbacks now have identified native consumers;
their presence alone does not establish coverage of every branch. The shared
acid/lava burn-sound gap found while wiring POWER1 is now covered, including
retail's initial cadence and bank fallback. Creature implementation coverage is
tracked separately in [CREATURE-AUDIT.md](CREATURE-AUDIT.md).

Retail's spawn-placement routine also sweeps runtime entity boxes before
SecondaryColl. This former gap is closed by the shared placement work recorded
in [CREATURE-AUDIT.md](CREATURE-AUDIT.md), including delayed activation against
changed supports and the disc-wide placement regression.

The earlier population pass also exposed a Flyer `nextthink`/`next_think` field typo,
which was corrected. A startup stack overflow observed while other chats were
editing this checkout no longer reproduces after their client-storage change.
Those fixes are separate from population selection.
