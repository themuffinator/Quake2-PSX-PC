# Functional parity follow-up

Updated 28 October 2026 against the current checkout and the user's PAL
SLES-01534 disc. This pass checks integration and continuation behavior. It
does not claim whole-game equivalence to a hardware capture.

## Corrected behavior

| Gap | Result |
| --- | --- |
| Creature/player ambient used the same guessed value | Creature bodies now initialize and recover toward native RGB 64; gameplay player bodies use RGB 32. Refresh preserves energy-hit colour, while a fresh player body resets it. |
| Extra players always returned to RIGHT STICK | Each player now owns the controller settings edited through their pause menu. Style selection survives later input frames, respawns and map loads. |
| Inversion, vibration and controller sliders borrowed player one's values | Menu bindings, displayed values, input decoding and motor output use the pausing/playing owner's configuration. Video, sound, crosshair and autocentre remain shared settings. |
| Player one's host mouse suppressed every player's autocentre | Continuous mouse look clears only player one's recenter state. Other players retain the shared retail pad-autocentre setting when their movement runs. |
| Old settings cards could retain another session's new settings | Appended controller fields preserve all existing indices. Missing fields load their defaults; truncated settings payloads are refused before mutation. Saved games and multiplayer settings cards include the new profiles. |
| Returning from multiplayer could retain the previous pauser | The front end selects player one's controller profile on entry. |
| PROCEED skipped retail's player setup | Each player now has an independently controlled NAME / COLOR or TEAM / CONTROLLER panel and a READY state. All active players must be ready, and Team Deathmatch requires both teams. Disconnect/reconnect preserves the panel and cursor. |
| Match startup discarded chosen identities | Names, teams and personal colours survive startup and return to the menu. Bodies, end banners and results use those choices; settings cards save them with the controller profiles. |
| `--movie` returned to QFRONT after completion | Standalone playback exits after the film's authored stop and fade. Campaign and boot films retain their successors. The last film render target remains available for `--shot`. |
| Credits used a guessed HUD-font scroll and looped forever | Read the actual 177-row tables, retain blank spacing, use the menu font and fixed title, clip the roll, and return to Options on Back or completion. Rendering no longer advances time. |
| The campaign outro returned straight to the title | The outro enters credits, then the authored WELL DONE question. Easy/Medium/Hard immediately grant Bronze/Silver/Gold, including when Back skips credits or No declines a save. |
| Unlock tiers never reached persistent settings or saved games | MULTI UNLOCK files share the settings slots and load through MULTIPLAYER → LOAD SETTINGS. Ordinary settings and game saves also retain the tier. Older files remain readable and imply tier NONE. |
| Gold's ALL WEAPONS flag had no grant consumer | Multiplayer start, resume and respawn grant weapons present in the current CastList, fill each player's ammo capacity, retain the held weapon and refresh the carousel. Ordinary frames do not refill ammo. |
| COMMAND's white-key return spawned a fresh zone-4 population | The native key predicate suppresses startup selection. Saves made after collecting the key preserve their already resident creatures and pickups. |
| SECURITY's boss arrival and escape depended on unimplemented frame callbacks | Two completed Tank Commander deaths summon Boss1 and start the four wall timers. The boss's completed death opens the hatch. Pending events and timers survive cold saves. |
| BOSS2 never opened the train after the final fight | The rider's completed death selects Zone4, enables the train door and updates mission text once, including after leaving the arena during its death animation. Jorg's first stage alone cannot open it. |
| POWER2's reactor and laser frame callback never ran | Both pumps start coolant drainage, timed glass destruction and the exit explosion. Early departure completes the reactor; saves preserve an active sequence. The tagged guard shuts off RockLaser, and the two zone-4 banks alternate on their authored clock. |
| POWER1's native explosions and shaft damage never ran | The entrance repeats its two explosions; the shaft follows its sixteen-phase cycle, player-height gate and strict damage box. Cold saves retain its deadline and phase. |
| Acid/lava damage omitted its burn cries | Shared damage admission now queues the victim's sound, with acid protection/throttling and lava's first/fifth/ninth cadence. Retail's `pla_burn2` fallback restores sound in all nineteen PAL banks that carry a burn sample. |
| Zero player damage entered knockback, while negative damage was rejected | Zero skips the impulse block but retains downstream feedback and cheats. Signed damage now follows retail's armour arithmetic, health stores and creature reactions. |
| The event opcode census omitted COMMON scripts | The inspect tool counts both COMMON and ZONE items and lists every FX damage record, so caller claims include both script sources. |
| Every damaging hit assigned an invented weapon flinch | Damage and its feedback now preserve the stored view amplitudes, as retail does. The native reader, pain deadline and separate impact shock remain active. |
| BOSS1's zone-0 laser callback was skipped | The five red entrance lasers follow their authored event states, deal damage once per logic tick and shut down at `DoorBlown`. Cold saves and zone returns retain them; the zone-1 sweep and projection paths stay confined to their branch. |
| BASE3/MAGDEMO's laser wall and shutdown callback were absent | Both zone-3 beams render and damage actors; the contact volume reads the resident zone's operands. The tagged Gunner's completed death disables the wall permanently, including after saves and zone/map returns. |
| Ordinary LASERBEAMs rendered without hurting actors | The shared walk now applies each kind's authored damage and MOD to its first actor, drains creature damage immediately, and respects script shutdown across saves and zone returns. |
| Shared effects ran after the native mission callback | Teleports, laser walls and ordinary lasers now precede that callback, matching retail's random-number and shutdown order. |
| LAB's shared lifts reset during zone changes | Lift A and Lift C retain their position, motion, waiting timer and collision across both seams, cold saves and map returns. A new game clears their history. |
| Loading a mover's saved pose left collision at its starting position | Saved-game apply now rebuilds mover collision before movement and prevents the whole restored offset from becoming a false one-tick push. |
| Five more shared lifts reset at zone seams | SECURITY's two lifts, BOSS1's entrance lift and BOSS2's paired stems now preserve motion and collision through zone changes, cold saves and map returns. |
| SECURITY and BOSS2 omitted their post-boss earthquakes | Both callbacks now apply the native physical impulses, rumble and random explosion sound with their original timing and saved cadence. |
| SECURITY's laser course stayed fully enabled and lacked its moving beams | Its 24 bank/ring/grid gates now follow the native clock and alternating sides; both moving kind-1 beams render and hurt actors on their authored paths. |
| SECURITY's exit lift never latched, and orb arcs vanished on their expiry frame | The lift now holds its upper landing and collision pose through saves and revisits. The orb submits all five arcs after updating every trajectory, preserving expiry damage and native random-number ordering. |
| LAB sweep sparks were absent and BOSS1 sparks used the wrong render area | Both callbacks now use their cached emitter areas. LAB's final signed sweep phase survives saves, and scripted actor use follows both sweeps rather than preceding them or running during a render-only refresh. |
| Restart retained native hazard clocks, while zone gates rearmed actor-use attempts | Full loads now reset module state even within the same map. Ordinary gates retain LAB/BOSS1 use latches and module clocks, subject to each zone callback's explicit resets. Saved games retain their recorded state. |
| Sound effects bypassed native Gaussian filtering and envelopes | Voices now run the SPU predictor, Gaussian filter, pitch accumulator and ADSR at 44,100 Hz, with release tails and immediate logical-slot reuse. Twenty-eight published hardware capture cases match after documented phase alignment. The host output conversion and unmeasured SPU transitions remain open in the audio audit. |
| Music and movies used the wrong CD gain scale | The CD matrix now uses 128 as unity and the mixer applies the native CD-input and main-volume gains. All 512 stereo/volume register combinations are checked against executed PAL SDK code. Independent final-output amplitude and clipping measurements remain open. |
| The mixer downsampled native voices and omitted CD interpolation | Mixing now remains at 44,100 Hz, with native voice consumption and the CD's seven-phase, 25-tap XA interpolation. Movie scheduling/fades use the same output clock. The software reference, full suite and disc sweep pass; independent XA waveform and device-output measurements remain open. |

The controller ownership follows the per-player 34-byte records used by
`0x8001C81C`, the pauser selection at `0x8001C9B4`, and the vibration reader at
`0x8003179C`. Reset at `0x8001BDA8` iterates all four records. Existing
`Q2_SET_*` indices are unchanged; the three additional seven-field profiles
fit the current saved-game and settings-card payloads without a format break.

The host has one keyboard/mouse owner. Extra gamepads expose their own analog
styles and inversion; their unavailable USE MOUSE row is disabled and grey.
They can change their controller profile by pausing and opening PLAYER CONTROL.
These changes do not add multiple host mice or assign physical devices differently.
Before starting a match, PROCEED now opens the shared player-setup screen. The
keyboard and host pointer control player one's panel; each gamepad controls its
assigned panel. The name editor uses directions to select/cycle characters, or
the pointer and wheel on the host. Back leaves a subpanel or cancels READY.

### Player-setup reconstruction

The previous sweep missed this entire reachable screen because its multiplayer
scenarios launched arenas directly. QFRONT `+0x59CC` opens module page 11;
`+0x5D94` dispatches seven per-player states and `+0x75C8` constructs their rows.
`+0x6654` supplies the eight-character alphabet (A–Z, 0–9 and space), while
`+0x6B94` selects four personal colours or two teams. `+0x6F34` allows a ready
player to back out. The all-ready path calls `+0x1F60` to start the match.

QFRONT's two-player builder calls engine `+0x168`, bound at `0x80079C94` to
the **stacked** viewport builder `0x80077900`, with projection 210 and far
distance 4000. Player two's rows move to local x=384 at `+0x9BB8`. Three and
four players use quadrants with projection 160. `+0x9C24` reveals only each
viewport's chosen body. Text, selection bars and models are rendered from the
actual disc assets. These are disassembly-derived layouts; paired hardware
captures have not yet established pixel equivalence.

Settings-card schema 2 appends 24 halfwords for four names, teams and colours.
Its bounded payload capacity is 96 halfwords. Schema-1 cards remain readable
and restore default identities. Truncated records and invalid identity bytes
are rejected before changing live settings. Older executables cannot read the
new schema; settings indices are unchanged. The completion work below adds
reward metadata to container version 2, independently of the opaque schema.

## Rechecked reports

### Credits reconstruction

QFRONT `+0x35C8` installs DEVELOPED BY from `+0xFAF4`; `+0x36A0` streams
the remaining rows from the table at `+0xFB24`, advancing before reading.
The previous string-pool scan missed the opening line, repeated text and
74 blank rows. The generic menu scanner's rejection of empty labels and
single-row tables was mistakenly interpreted as evidence that no tables existed.
The reader now validates raw or relocated records and retains their order.

The native callback uses a sixteen-object ring, inserts every sixteen ticks,
and moves one pixel per tick. The port advances this on the disc's logical
video clock, independent of host rendering. It uses the 16-pixel menu face,
a fixed 32-pixel CREDITS title, the native y=60..203 clip, and QFRONT's subpage
logo state. The normal Back/completion callback is `+0xCCA4`, which opens
Options. The last nonempty row clears at tick 2965; completion follows at 2966.
Esc, Backspace, right-click and controller Back use that return. Confirm and
movement do not dismiss the page.

### Campaign completion and unlock files

The separate campaign continuation is now wired:
the outro enters dispatcher state 17; `0x80018800` calls `0x8007CCE0(1)`,
setting engine `+0x4A8`. QFRONT `+0x3244` then opens credits with a different
return callback, `+0x4454`. Both Back and natural completion reach that callback.
It assigns engine `+0x378` to difficulty + 1 before asking whether to save an
unlock file: Bronze for Easy, Silver for Medium, Gold for Hard. This is a direct
assignment, including after an earlier higher award. No returns to the title
with the award still active; the question has no Back action. Its four static
rows at `+0xF044` and YES/NO at `+0xF0BC` use the 16-pixel face and subpage logo.

YES (`+0xD2C4`) sets bit 31 while clearing the game-save type bits. The native
backend names that file `Q2_MULTI UNLOCK` (`0x80080968` / `0x800AD7D4`), alongside
ordinary `Q2_MULTI DATA`. `0x80080CF0` saves/restores Game Config's engine
`+0x2D0..+0x38B`, which includes the tier. The directory filter at `0x800821DC`
groups both MULTI kinds for LOAD SETTINGS; it does not automatically load the
award at startup. Successful save acknowledgement returns to MULTIPLAYER
(`+0xA244`); cancel returns to the question. Host I/O uses the existing four
settings positions and overwrite/report screens, separately from game slots.

`Q2CF` version 2 adds checksummed tier/type metadata after its opaque payload.
Version 1 remains readable. Invalid tiers, types and unlock-without-tier files
are refused before applying any state. Saved games use an optional `UNLK`
chunk; older games without it restore tier NONE. No installation saves or
settings were migrated or modified during verification.

Reaching Gold exposed a second missing consumer: `Q2_CHEAT_ALL_WEAPONS` was
set but never granted anything. Native `0x8003B040` filters ids 1..11 by the
map's view-model names, ORs their owned bits, fills all six ammo pools to the
current base/bandolier/pack limits, retains the current gun, and writes the
carousel pair. Its callers are variable installation (`0x8001C7BC`, all present
players) and respawn (`0x8003DE38`). The client now performs those event-time
grants, while its ordinary per-frame scalar/settings refresh remains free of
inventory grants. Removing the toggle leaves weapons already granted intact.

The previously recorded three `raster_quads` failures no longer reproduce in
the starting checkout: all 67 baseline CTests passed.

The recorded chaingun cold-reload RNG failure was in its fixture. It advanced
weapon animation without advancing audio, so one-shots never ended. The first
client filled the 24 voices and stopped drawing pitch RNG; the freshly loaded
client could allocate voices and still drew it. The fixture now advances the
production headless mixer every frame and checks that neither timeline drops
voices. All 334 disc-backed save checks pass, including weapon state, ammunition,
projectiles, shot serials and RNG continuation. No gameplay-RNG or voice-allocation
workaround was introduced. Transient sound tails remain outside saved games.

### Native population progression

Reviewing the remaining selector sites exposed three reachable gaps that an
unconditional static name scan could not establish. The census inspected 83
selector calls in 21 campaign/demo LevelBin modules. COMMAND branches on the
white key; SECURITY and BOSS2 call the selector from gameplay callbacks after
specific tagged deaths. The client now implements those decisions and keeps
the existing static scanner as a fallback, with its limitations explicit.

Kill completion follows retail's corpse-detachment/departure calls to
`RegisterKill`, not the first zero-health frame. Stable creature identities
and zone history distinguish a completed casualty from a dormant spawn or
Jorg's living second stage. The optional NMIS v2 chunk adds four SECURITY wall
timers; v1 and saves without NMIS remain readable. Existing pending-event
serialization also preserves an explosion queued just before saving.

The focused selector fixture passes 259 checks. It verifies the real wall and
hatch geometry changes after restoring their pending events, both final-boss
stages, cold saves, map/zone returns and departure while the rider is dying.
COMMAND's white-key fixture changes from four enemies and three pickups to
zero, with the same deterministic camera. SECURITY captures show the arena
before and after Boss1 appears. Disassembly, census, captures and logs are in
`.agents/tmp/functional-parity/selectors/`; detailed retail addresses are in
[ENTITY-AUDIT.md](ENTITY-AUDIT.md).

### Reactor callback follow-up

Tracing actual init registrations found POWER2's missing `+0x11A0` callback.
The core now uses its original red beam faces and spark emitter. Both pumps
start the objective popup and coolant drain; six timed glass breaks stop the
core and emit the native beam/explosion/debris effects; the exit opens at
10000 ticks. Leaving the reactor early completes it for the next visit.
The tagged zone-3 guard's completed death disables RockLaser. Zone 4 alternates
its two laser banks with the original 47-tick dark windows.

NMIS v3 adds the start clock, alarm accumulator and consumed-glass mask while
retaining v1/v2 compatibility. Cold saves preserve the active sequence and
pending event queue. The focused fixture passes 205 checks with real disc
data, disk saves and deterministic engine captures; its unpatched baseline
failed eleven behaviour checks. Evidence, the callback registration census and
before/after images are in `.agents/tmp/functional-parity/callbacks/`.

The image comparison also found that native beams were being queued with a
collision-cell index instead of the drawing-area byte the port expects. The
core and shared LAB/BOSS1/SECURITY helper now resolve that byte, and the fixture
requires actual emitted faces. The corrected core capture differs visibly
from its baseline; a non-empty beam queue alone had missed the defect.

The census also disproved the suggested COMMAND boss-counter gap: its module
installs no frame callback and never consumes the zero-initialized counter.

### POWER1 callback follow-up

POWER1's `+0x5F0` callback and `+0x414` shaft helper are now implemented. The
entrance repeats two explosions after each strict 900-tick deadline. The shaft
advances every 100 ticks through seven blasts and nine silent phases, limiting
itself to the first two blasts when the player is below the shaft. Its strict
damage box uses entity-origin coordinates and sends 30 points through the
ordinary lava damage path. Zone changes reset the deadline while retaining
the phase; map changes clear both; cold saves preserve the exact active state.

NMIS v4 adds these two words and reads versions 1 through 3. The focused fixture
passes 180 checks, with disk save/restore, all cycle phases, damage boundaries,
replay/render exclusion, and an actual headless frame call. Six behaviour
assertions failed on the unpatched baseline. The engine captures show the
restored blast above the shaft's central pedestal. Evidence is under
`.agents/tmp/functional-parity/power1/`.

The entrance coordinates fall outside the retail collision hull. Retail's
unchecked area lookup consequently reads the node-count byte in the chunk
header. The port reproduces that value safely and retains the original spawn,
sound and shock rather than relocating the effects. `MakeBlitzes` has no named
event on the disc, so its kill-counter call is inert. BIGGUN's secondary
callback is its already implemented lightning draw, confirmed against its
eight accumulator updates and by 117 passing disc-backed client checks.

### Shared acid and lava sound path

The missing POWER1 damage cue exposed a shared integration gap. Acid helper
`0x8003DEE4` sounds on each hit admitted by its protection and 400-tick gate.
Lava helper `0x8003DF0C` increments the process-wide word at `0x800AE898` before
testing its low two bits. The executable seeds that word to **3**: calls 1, 5,
9, and so on sound, rather than waiting for the fourth call. Its only located
read/write sites are that helper. The port retains the call count across all
loads and in-session save restoration, with a fresh client starting at zero
calls. Saved games do not rewind it; a paired retail card test remains unmeasured.

Lava requests sound before general invulnerability; acid's suit/invulnerability
tests precede sound. Armour absorption and zero incoming player damage still
reach this stage. The request belongs to the hurt player, and all players share
the lava cadence. A full event queue still consumes the admitted hit. BIGGUN
uses this common hook in place of its old special-case cue, and the client now
drains events after native frame callbacks so POWER1's late cue is not discarded.

The real-disc fixture found a second defect: `Sound_Burn` at `0x8003B7EC` prefers
`pla_burn1` but explicitly falls back to `pla_burn2` at `0x8003B894`. The client
had only named the first. Across all 49 PAL banks, none carries burn1, nineteen
carry burn2, and thirty carry neither. Tests exercise both choices and missing
pairs; no additional replacement is introduced. The production mixer now emits
the selected sample with the existing pitch variation of 33 or 34.

### Signed damage and complete script census

The outer damage routine tests the original amount for zero at `0x80057EC0`
before knockback. Zero player damage still reaches protection, feedback and
the ONE SHOT KILL rewrite, but must leave pending impulse state untouched.
Negative damage enters the native nonzero paths. Both armour helpers reject
zero rather than nonpositive damage, and a negative saved amount restores
inventory. Power armour compares against the signed halfword of twice the
cells; ordinary armour sign-extends its shifted low halfword before the cap.
The full signed remainder passes between stages. The client now also delivers
the creature pain callback for a negative direct-damage reaction, while ordinary
healing outside damage remains free of that callback.

`tests/fixtures/generate_combat_reference.py` runs the actual PAL instructions
at `0x80057A9C` and `0x80057BE4` in a bounded integer interpreter with branch
and load delays. Its sole external hook records `SetWinFade`; it is not a
general PlayStation emulator. The input segment's SHA-256 is pinned to
`2dbdb53efd709a4cb5930ce91a39b55e8edad162c5fb773a7c6fa164a960fda8`.
The generated header contains 130 numeric vectors, with no retail code. They
cover signed damage, positive controls, zero, both skill biases, all armour
classes, energy damage, fade ordering and halfword inventory boundaries.

The caller search also exposed an incomplete inspect-tool census: it counted
ZONE scripts alone. Including COMMON yields 164 Events chunks, 4,179 records
and 6,646 items. All nine FX damage copies are positive: BIGGUN has five copies
of lava damage 1, and WASTE3 has four copies of damage 65. The previously
reported seven copies omitted the two COMMON records. No normal-play negative
damage source has been established; extreme inventory vectors describe defined
arithmetic, not an observed gameplay encounter. These signed-damage edge cases
have no demonstrated ordinary-play pixel change; the weapon correction below
has its own independent capture.

### Damage preserves stored weapon offsets

Following the guessed amplitude writer found another visible discrepancy.
The outer damage routine `0x80057D54` and PlayerThink's feedback block
`0x8003AE10..0x8003AFA8` preserve `client+0x9C/+0x9E`. Spawn initializes the
client with a 224-byte clear at `0x8003B2BC`. The port nevertheless assigned
`damage * 8`, capped to 455, with roll selected from the impact side. That
created an extra weapon flinch. The current client already gives the world
camera plain aim; these offsets feed the gun through `0x8004F404`.

The assignment has been removed. The measured offset reader and serialized
fields remain, including native scaling of explicitly seeded values by the
210-tick pain deadline over a 150-tick period. Impact shock is independent:
the damage routine calls `0x80040800` for its MOD-dependent GameWindow effect.

The reference generator now also executes native damage, the feedback block
with its established register bindings, and view-offset composition. Its 132
cases cover all 22 MOD values, signed/zero damage, both impact sides, armour,
skill, zero amplitudes and seeded amplitudes. The real fade and impact-shock
routines execute; explicit hooks cover memset, axis-vector normalization and
burn/pain audio dispatch. These bounded cases establish the changed paths,
not arbitrary module execution or a whole-console playthrough.

The new simulation checks failed 206 assertions with the guessed assignment
and pass after its removal. A disc-backed fixture waits for the actual pain
deadline, then captures the render target after a 20-point melee hit. Before
the fix its composed offsets were pitch -224 and roll -112; afterward both
are zero, with health still 80 and the damage flash retained. The comparison
changes 10,321 pixels, all in the weapon rectangle (303,140)..(494,247).
Captures are `damage-view-before.png` and `damage-view-after.png` under
`.agents/tmp/functional-parity/damage-audit/`.

### BOSS1 entrance lasers

The callback census exposed a missing branch inside a registered handler.
BOSS1 `+0xD3C` tests the resident zone name and calls `+0x1708` for zone 0.
That branch emits five kind-4 lasers when their named records become disabled,
unless `DoorBlown` is disabled. It bypasses the random sweep velocities,
phase clocks and projection-use callback that belong to zone 1. The client
previously ran only the latter path.

The new disc-backed regression reads the beam order and endpoints directly
from LevelBin and exercises all 32 combinations, damage, the render-only path,
disabled rendering, authored event execution, cold disk saves and zone returns.
It passes 815 checks. A control run with the new dispatch branch removed fails
112 assertions and draws zero beam faces; the corrected capture draws 260.
The matching headless images and disassembly are in
`.agents/tmp/functional-parity/boss1-entrance/`. JAIL5's revisited
`ExplodingBox` / `QuadInCrate` branch remains inert because this disc has no
matching population group. See [the entity audit](ENTITY-AUDIT.md) for offsets
and gate semantics.

### BASE3 / MAGDEMO laser wall

Reviewing the next frame callback found three connected omissions: the shared
LASERWALL renderer/actor sweep, its resident-zone contact-damage binding, and
the native completed-kill shutdown. The wall now emits its two thin red beams,
applies 512 beam damage and 200 contact damage, and stays off after the tagged
Gunner finishes dying. Leaving during its death animation also completes the
kill. Saves retain the state before, during and after death.

The headless fixture uses the actual Scene bounds and scripts on both maps.
Its matching before/after frames change 4,477 pixels and draw 84 beam faces
after the fix. The contact test initially failed six assertions, exposing the
COMMON `-1` object index that had suppressed damage even in resident zone 3.
Details, offsets and MAGDEMO's older import-ABI qualification are recorded in
[the entity audit](ENTITY-AUDIT.md#base3--magdemo-laser-wall).

### Ordinary map lasers and frame order

The shared `DrawLaserBeams` walk at `0x8002EE38` calls the damaging routine
`0x80048DC8`. Passing `ends=0` suppresses endpoint particles, not the actor
sweep at `0x80048F8C` or damage call at `0x80048FB4`. The port had implemented
only the visual half. Crossing a red JAIL2 laser left health at 600; the same
fixed-step capture now shows 88 after one 512-damage hit.

The disc fixture covers all 77 resident beam instances in POWER1, POWER2,
JAIL2, JAIL3, COMMAND and SECURITY. These are 71 authored live beams, six of
which appear in more than one zone; the remaining authored JAIL2 item is
never registered. It checks independent damage/MOD constants, render-only
frames, hidden rendering, exhausted visual pools, disabled records, immediate
creature pain/health persistence, COMMAND's real `LasersOff` switch, a cold
disk save and zone returns. Entering a map without beam records clears the
previous set.

`QuakeLoop` at `0x80038FA4..0x80038FD4` runs teleports, laser walls, ordinary
lasers, then the registered mission callback. The client now uses that order,
including the teleport walk's unconditional 45 random draws. POWER2's zone-4
clock verifies that the bank still draws on its shutdown frame and that the
newly enabled six-beam `Laser1` bank appears on the next frame.

### LAB lift continuity

The next callback review found LAB's `+0x3D0` lift recorder and `+0xD8` entry
restorer had no native implementation. Lift A spans zones 0/3 and Lift C spans
2/4, with different Scene indices on each side. The generic event replay left
both at the bottom on entry. A disc-backed failing control moved each 3600
units upward, crossed the seam, and observed offset zero and idle state.

Both lifts now retain their motion and collision data in unit state. The
entry hook rebinds by name after event replay; neither unloaded zones nor
failed loads erase a previous snapshot. NMIS version 5 adds two bounded
60-byte records to the saved-game payload, retaining readers for versions
1–4. Earlier files cannot recover unloaded lift history they never recorded.
The fixture covers both travel directions, open/wait timers, closing motion,
collision traces and subsequent steps, cold saves with and without a resident
lift, map returns and new-game reset. Native offsets and geometry are recorded
in [the entity audit](ENTITY-AUDIT.md#labs-shared-lift-state).

An older-save case also exposed a shared MOVR restore gap: visible displacement
was applied while collision stayed at its initial pose. Four failing assertions
established the stale bounds and false next-frame push. Saved-game apply now
rebuilds collision immediately, with zero previous motion when an old file did
not record it. V5 LAB records retain their exact previous step.

### SECURITY, BOSS1 and BOSS2 lift continuity

The broader module review found five more lifts with the same omitted
save-memory callbacks: SECURITY's `Lift A`/`Lift B`, BOSS1's `Lift A`, and
BOSS2's `LiftStemA`/`LiftStemB`. All span zones 0 and 1. The failing disc
control moved them partway upward and observed every one reset to idle at
offset zero after crossing the seam. Their state now survives zone and map
departures, saved games and return trips, with collision restored immediately.
BOSS2's paired parts resume together. Separate entries prevent the shared
`Lift A` names from overwriting one another.

NMIS version 6 appends five 60-byte records, keeping versions 1–5 readable.
The combined regression saves all seven native lifts while absent, reopens
the client and checks each map's state. The focused fixture also covers open
wait timers, closing motion, both directions of every seam, collision traces
and next-frame movement, failed loads and new-game reset. Native addresses,
Scene indices and timings are in
[the entity audit](ENTITY-AUDIT.md#security-and-boss-map-lift-continuity).

### SECURITY and BOSS2 earthquakes

The native callback review found two omitted earthquake sequences. The
failing disc control completed both maps' actual boss gates but produced no
player impulses or rumble. SECURITY now shakes from the two Tank Commander
deaths through the fourth wall explosion. BOSS2 begins after the rider's
defeat and its strict 300-tick delay, then follows the native shaking/quiet
cycle. The callbacks restore physical movement, Explosion rumble and the
random positional explosion sound, including the original signed arithmetic.

Three saved clocks preserve pulse cadence through cold loads and zone changes.
NMIS version 7 keeps all earlier readers and leaves an unrecorded legacy BOSS2
clock inactive. Full loads, including Restart/Resupply in the same map, reset
module clocks and SECURITY's pending wall timers, while retaining unit
completion flags. Details and
native addresses are in
[the entity audit](ENTITY-AUDIT.md#security-and-boss2-earthquakes).

### SECURITY laser course

SECURITY's zone-0 callback at `+0xE90` was absent: the failing control found
all 24 patterned lasers enabled and neither moving beam. The restored callback
cycles the six bank positions out and back, the four ring positions and the
two grid positions, alternating sides on the world counter. Its two moving
beams follow a triangular sweep, including their initial vertical section.
Both use the native kind-1 damage and first-hit actor rules.

The static beam walk still precedes the callback, so a changed gate affects
the next frame. Rendering alone does not apply damage or advance either
clock. Existing saved level time, world counter and event flags preserve the
course through cold loads and zone returns without a new save field. Native
addresses, numeric periods and endpoint details are in
[the entity audit](ENTITY-AUDIT.md#security-laser-course).

### SECURITY orb and exit lift

The missing zone-2 callback now holds `HexLift` at its upper landing once
player 0 crosses the native strict height threshold. Its unit-save flag
survives zone changes, map returns and cold saves. Restoring that flag also
restores the raised collision box, while retaining BASE0's different conveyor
collision behavior.

The orb no longer loses its five arcs on their expiry frame. Signed timer
residuals survive saves, and all five trajectories update before any of their
damage calls can consume effect RNG. Both vertical beams and the five arcs
use the fixed orb-origin cell. Rendering alone does not advance trajectories
or deal damage. The existing NMIS layout accommodates both corrections.

PAL's module requests `LiftRing1`, but its zone names contain only `ring`.
The exact lookup is preserved without inventing an asset alias. Native
addresses, the missing-name behavior and compatibility limits are documented
in [the entity audit](ENTITY-AUDIT.md#security-orb-and-exit-lift).

### LAB and BOSS1 endpoint sparks

The port looked up each moving beam endpoint to choose a spark draw area.
LAB's endpoints lie outside PrimaryColl, suppressing both bursts; BOSS1's
endpoints returned area 12 although the native callback submits area 9.
Both now use the areas recorded by their zone-entry callbacks. The original
fifteen-particle velocities, lifetimes, hit rules and dynamic lights remain.

The review also corrected LAB's signed final phase decrement and the order
of both modules' actor-use helpers. Saves retain expiry phases down to -59;
the final beam still damages, later inactive frames stay quiet, and the
normal mission call can rearm the sweep. Actor use follows both sweeps and
is not consumed by rendering alone. Native addresses and the captured failing
controls are in [the entity audit](ENTITY-AUDIT.md#lab-and-boss1-sweep-endpoint-effects).

### Native module lifetimes

The full native loader rereads COMMON and runs LevelBin export 0 even when
Restart or Resupply keeps the current map. Ordinary zone gates retain that
module and run only the indexed zone-entry callback. The port had used the
map name to decide when to reset hazard clocks, but cleared LAB/BOSS1's
one-time actor-use latches at every zone load. Both boundaries are corrected.

The regression exercises six modules, cold saves, failed loads and both real
menu requests. Older hazard fixtures now set the same carry flags as ordinary
zone gates, instead of using full reloads as a substitute. The failing control
left both LAB sweep beams visible after Restart; the corrected frame has none.
Native call paths and module writes are recorded in
[the entity audit](ENTITY-AUDIT.md#module-state-across-gates-and-full-reloads).

## Validation

- The native-lifecycle fixture passes 185 focused checks after its original
  17-failure control. All 5,139 population checks with capture/save output,
  689 save-codec checks, 334 client-save checks and 68 CTests pass. The disc
  sweep passes 215 cases (213 executed, the same two crossing skips).
  Matching LAB Restart frames differ in 32,691 pixels: the stale sweep beams
  and their light are cleared. Native disassembly, before/after PNGs and logs
  are retained in `.agents/tmp/functional-parity/native-lifecycle/`.
  The staged executable and SDL3 match Release, `--version` succeeds, and all
  21 zones across the six affected maps complete 300 headless frames from
  `.install/` without warnings or errors. `stage-verification.json` records
  the hashes and run results.

- The LAB/BOSS1 sweep fixture passes 144 focused checks and the save codec
  passes 689. All 4,652 population checks, 334 client save checks and 68 CTests
  pass. The 215-case disc sweep passes (213 executed, the same two crossing
  skips). Seven-step headless control/corrected captures differ in 859 LAB
  pixels and 10 BOSS1 pixels; the first-frame control reproduces the original
  baseline exactly. Evidence is in `.agents/tmp/functional-parity/native-sweeps/`.
  The staged executable and SDL3 match Release, `--version` succeeds, and all
  five LAB plus both BOSS1 zones complete 300 headless frames from `.install/`
  without warnings or errors. `stage-verification.json` retains the hashes
  and results.

- The SECURITY orb/lift fixture passes 206 focused checks; its failing control
  exposed the missing lift hold and the two-beam expiry frame. The save codec
  passes 687 checks, including the held flag, signed timer bounds and existing
  NMIS compatibility. All 4,530 population checks, 334 client save checks and
  68 CTests pass. The 215-case disc sweep passes (213 executed, two documented
  crossing skips). Matching headless expiry captures differ in 534 pixels:
  the corrected frame keeps all five arcs. Evidence is retained in
  `.agents/tmp/functional-parity/security-orb/`. The staged executable and SDL3
  match Release; `--version` and 300-frame runs in all three SECURITY zones
  pass from `.install/` without warnings or errors. Their results and hashes
  are recorded in `stage-verification.json`.

- The SECURITY laser-course fixture passes 650 focused checks. All 4,350
  population checks, 764 static-laser effect checks and 68 CTests pass. The
  215-case disc sweep passes (213 executed, two documented crossing skips).
  Evidence is in `.agents/tmp/functional-parity/security-lasers/`. Matching
  headless captures at level time 30 and world counter 1 show the changed
  gates and moving beams. The staged executable and SDL3 match Release;
  `--version` and 300-frame runs in all three SECURITY zones pass from
  `.install/` without warnings or errors. `stage-verification.json` retains
  their hashes and results.

- The earthquake fixture passes 184 focused disc checks; the save codec
  passes 685, including NMIS v1–6 compatibility and malformed v7 records.
  All 3,717 population checks, 334 client save checks and 68 CTests pass.
  The 215-case sweep passes (213 executed, the two documented crossing skips).
  Evidence is in `.agents/tmp/functional-parity/earthquakes/`. Matching
  fixed-step SECURITY captures change the player position from
  `(-1849,-11040,-8974)` to `(-2560,-11040,-8642)` with no input; BOSS2 also
  displaces the player. The staged executable and SDL3 match Release, and
  `--version` plus 300-frame smoke runs in all six SECURITY/BOSS2 zones pass
  from `.install/` without warnings or errors. `stage-verification.json`
  records hashes, version and results.

- The SECURITY/BOSS lift fixture passes 771 focused disc checks. The save
  codec passes 661, including v5 compatibility and malformed v6 records.
  The complete population fixture passes 3,557 checks, the client save
  fixture passes 334, and all 68 CTests pass. Baseline failures, native
  disassembly and final logs are in `.agents/tmp/functional-parity/native-lifts/`.
  Matching SECURITY captures differ in 4,908 pixels, confined to the lift at
  the centre of the shaft view. The 215-case disc sweep passes (213 executed;
  PODCITY/THEVAT crossings remain explicit skips). The staged executable and
  SDL3 match the Release build, `--version` starts, and all 13 zones across
  LAB, SECURITY, BOSS1 and BOSS2 complete 300 headless frames from `.install/`
  without warnings or errors. `native-lifts/stage-verification.json` records
  version, hashes and smoke results.

- The LAB lift fixture passes 223 focused disc checks, and the expanded save
  suite passes 609 checks including NMIS v4 compatibility and malformed v5
  records. The full population run passes 2,846 checks and the real-client
  save fixture passes 334. All 68 CTests and the 215-case headless disc sweep
  pass (213 executed, two explicit crossing skips). Matching before/after
  engine captures, disassembly and the failing
  baseline are retained in `.agents/tmp/functional-parity/lab-lifts/`.
  The staged executable and SDL3 match the Release build; `--version` starts
  successfully and all five LAB zones complete 300 headless frames from
  `.install/` without warnings or errors. `lab-lifts/stage-verification.json`
  records version, hashes and results. The matching lift images differ in
  47,251 pixels.

- The ordinary-laser fixture passes 764 checks across six maps. All 2,655
  population checks and 117 generator/evacuation checks pass after the frame
  order change, along with all 68 CTests and the 215-case headless sweep
  (213 executed; PODCITY/THEVAT crossings are explicit skips). Native
  disassembly, the failing baseline, final logs and
  matching JAIL2 before/after captures are under
  `.agents/tmp/functional-parity/static-lasers/`.
  The staged client starts and matches the Release build, including SDL3.
  JAIL2/1, POWER2/4, SECURITY/0, BOSS1/1, LAB/2 and BASE3/3 each complete 300
  fixed-step frames from `.install/` without warnings or errors. Version,
  hashes and smoke results are recorded in `static-lasers/stage-verification.json`.

- The laser-wall follow-up passes 677 focused disc checks, all 2,655 population
  checks and 1,104 effect checks. All 68 CTests and all nine BASE3 headless
  scenarios pass after the Release rebuild. Evidence, exact disassembly and
  before/after/completed engine captures are under
  `.agents/tmp/functional-parity/laserwall/`.
  The staged executable and SDL3 match the Release output; `--version` starts
  successfully. BASE3/3, MAGDEMO/3, BOSS1/0 and LAB/2 each complete 300 fixed-step
  headless frames from `.install/` without warnings/errors. Hashes and results
  are recorded in `laserwall/stage-verification.json`.

- The BOSS1 entrance follow-up passes 815 focused checks and all 2,027
  population checks. All 68 CTests, six BOSS1 headless scenarios and nine LAB
  scenarios pass after the complete Release rebuild. These cover map startup,
  event scripts, gates, crossings and save/load; they are not full playthroughs.
  Matching doorway captures differ in 2,720 pixels within (216,82)..(329,172).
  Evidence is retained under `.agents/tmp/functional-parity/boss1-entrance/`.
  The staged executable and SDL3 hashes match the Release output. BOSS1 zones
  0 and 1, LAB zone 2 and BASE1 zone 0 each complete 300 headless frames from
  `.install/` without warnings/errors; `stage-verification.json` records them.
- The weapon-offset follow-up passes 1,120 simulation checks and all nine
  focused disc-client checks. `view-baseline.log` and `view-before.log` retain
  the failing controls; `sim.log` and `view-after.log` record the corrections.
  After the complete Release rebuild, all 68 CTests and all 215 headless sweep
  cases pass again (213 executed, two explicit no-gate skips).
- The signed-damage follow-up passes 695 combat checks, including the 130 native
  vectors; its initial regression run failed 145 checks. All 45 synthetic
  creature-client checks and 10,536 disc-audio checks pass. All 68 CTests and
  the complete 215-case headless sweep pass again (213 executed, two explicit
  no-gate skips). Evidence, the native segment used to regenerate the numeric
  fixtures, disassembly and the complete script census are retained under
  `.agents/tmp/functional-parity/damage-audit/`.
- The burn-sound follow-up passes 724 simulation checks, 10,536 disc-audio
  checks, 1,240 population checks and 117 BIGGUN client checks. All 68 CTests
  and the full 215-case headless sweep pass again. The focused POWER1 fixture
  also checks mixer output, victim ownership, persistent cadence and the late
  native callback. Evidence and the actual mixed WAV are in
  `.agents/tmp/functional-parity/burn-sounds/`. The current staged executable
  and SDL3 hashes match the Release output. POWER1/1, BIGGUN/1, BOSS1/0 and
  BASE1/0 each completed 300 headless frames from `.install/` with zero
  warnings/errors; `stage-verification.json` records version and hashes.
- All 68 configured CTests pass in a complete Release run after the POWER1
  follow-up. The credits test adds 747 checks for table bounds, malformed pointers,
  blank/repeated rows, ring wrap, host-rate-independent timing, completion and
  native/adaptive draw-area clipping. The prior CRT legacy-card correction and
  full-capacity settings-container round trip also pass. The save unit suite
  now passes 592 checks, including legacy NMIS v1/v2/v3, wall/reactor/shaft clocks, malformed native
  state, legacy containers and malformed rewards.
- After the POWER1 follow-up, the full 215-case headless disc sweep passed:
  213 cases executed and two gate cases were explicitly skipped because
  PODCITY and THEVAT have no inter-zone gate. It covers loading, rendering,
  scripts, combat, saves, campaign transitions and cinematic starts.
- Both credits scenarios and the new 5,000-frame campaign completion pass.
  Standalone intro and outro playback finish without entering credits or
  awarding a tier. A separate 300-frame BASE1 smoke
  run from `.install/` has no warnings or errors. The staged executable and
  SDL3 hashes match the Release build; `--version` reports
  `v0.2.0-20-ga590ce9-dirty`.
- The added standalone-movie completion regression passes. The PAL intro shows
  all 1,280 intended pictures and exits after 1,539 fixed-step frames, before the
  5,000-frame diagnostic cap, with no errors or warnings.
- All 18 split-screen scenarios pass: thirteen arenas and full two/four-player
  Deathmatch, Team Deathmatch and Versus lifecycle checks, including READY and
  return to the multiplayer menu.
- The disc-backed controller capture fixture passes 84 checks. Its images come
  from the engine framebuffer with fixed headless stepping; they show each
  player's own style, inversion, vibration and slider values.
- The disc-backed save fixture passes 334 checks. Menu and split-client tests
  additionally cover profile isolation, actual inverted input, motor ownership,
  current/legacy settings round trips and malformed-card refusal.
- The full disc-backed population fixture passes 1240 checks without capture
  output; the focused selector run passes 259 with on-disk save round trips
  and engine captures. Both run headlessly, and save tests use project-local
  TEMP/TMP/TMPDIR. Current full-suite logs are under `power1/`; the prior
  selector pass's captures and logs remain under `selectors/`.
  The selector pass staged BASE1 zone 0, SECURITY zone 1 and BOSS2 zone 1; each
  complete 300 headless frames from `.install/` without warnings or errors.
  The reactor pass additionally checks POWER2 zones 0/3/4, LAB zone 2 and
  BASE1 zone 0 for 300 headless frames each from the refreshed `.install/`.
  The POWER1 pass checks its zones 0/1/2, POWER2 zone 0, BIGGUN zone 1 and
  BASE1 zone 0 for 300 headless frames each, again without warnings or errors.
  `--version` starts successfully; executable and SDL3 hashes match Release.
- The player-setup fixture passes 1,220 checks. It enters through PROCEED with SDL-local virtual pads,
  edits all four names and teams, changes independent controller styles, checks
  rapid Start/Back aliases, waits for every READY, starts Team Deathmatch and
  reaches its results screen. It also starts two-player Deathmatch and
  three-player Versus. Two-player setup keeps its retail stacked layout even
  when the ensuing match uses the user's vertical split preference.
- Menu checks cover held entry input, name alphabet/length, colour/team wrap,
  disconnected-player restoration, the two-team gate, readiness and mouse hit
  areas. Synthetic controller events and coordinate-level pointer tests do not
  read or control the user's input devices.
- The credits client fixture passes 274 checks against the real PAL disc,
  compares raw and relocated row decoding, checks the exact completion tick,
  rejects Confirm, and handles held/tapped controller Back. Repeated draws and
  window-aspect changes leave the timeline unchanged. Engine captures show the
  opening rows, later groups, a wide display and the returned Options page.
- The unlock client fixture passes 332 checks: actual relocated page records,
  all three awards, natural/Back completion, No, cancel/retry, overwrite
  refusal, failed I/O, fresh-client settings loads, and ordinary settings
  retaining the tier. It starts a four-player match from the restored Gold
  page and checks grants, all ammo capacities, respawn and non-refilling
  frames. BASE1 verifies that missing weapon models are not granted.

Reproduction from the repository root:

```powershell
ctest --test-dir build-msvc -C Release --output-on-failure --parallel 4
python tests/check_headless_disc.py --disc .install/disc/game.cue --output .agents/tmp/functional-parity/disc-sweep --jobs 4
python tests/check_split_support.py --client .install/q2psx.exe --disc .install/disc/game.cue --output .agents/tmp/functional-parity/multiplayer
```

The save/capture fixtures require existing output directories:

```powershell
.\build-msvc\bin\Release\test_client_save.exe --headless .install/disc/game.cue .agents/tmp/functional-parity/save-verified
.\build-msvc\bin\Release\test_client_menu.exe --headless .install/disc/game.cue .agents/tmp/functional-parity/controllers --profiles
.\build-msvc\bin\Release\test_client_menu.exe --headless .install/disc/game.cue .agents/tmp/functional-parity/players --players
.\build-msvc\bin\Release\test_client_menu.exe --headless .install/disc/game.cue .agents/tmp/functional-parity/credits --credits
.\build-msvc\bin\Release\test_client_menu.exe --headless .install/disc/game.cue .agents/tmp/functional-parity/unlocks --unlocks
.\build-msvc\bin\Release\test_client_population.exe --disc .install/disc/game.cue
.\build-msvc\bin\Release\test_client_population.exe --selectors .install/disc/game.cue .agents/tmp/functional-parity/selectors/after
.\build-msvc\bin\Release\test_client_population.exe --callbacks .install/disc/game.cue .agents/tmp/functional-parity/callbacks
.\build-msvc\bin\Release\test_client_population.exe --power1 .install/disc/game.cue .agents/tmp/functional-parity/power1
.\build-msvc\bin\Release\test_client_population.exe --boss1 .install/disc/game.cue .agents/tmp/functional-parity/boss1-entrance/after
.\build-msvc\bin\Release\test_client_population.exe --nativelifts .install/disc/game.cue .agents/tmp/functional-parity/native-lifts
.\build-msvc\bin\Release\test_client_population.exe --quakes .install/disc/game.cue .agents/tmp/functional-parity/earthquakes
.\build-msvc\bin\Release\test_client_population.exe --security-lasers .install/disc/game.cue .agents/tmp/functional-parity/security-lasers
.\build-msvc\bin\Release\test_client_population.exe --security-orb .install/disc/game.cue .agents/tmp/functional-parity/security-orb
.\build-msvc\bin\Release\test_client_population.exe --native-sweeps .install/disc/game.cue .agents/tmp/functional-parity/native-sweeps
.\build-msvc\bin\Release\test_client_effects.exe --headless .install/disc/game.cue .agents/tmp/functional-parity/damage-audit/view-after --hurt
```

Evidence is retained under `.agents/tmp/functional-parity/`. No disc data or
external implementation code is added to the repository.

## Actor ambient lighting

Native allocation at PAL `0x8006C098` seeds current ambient (`+0x2AC`) and
target ambient (`+0x2B0`) to RGB 64. Gameplay player spawn at `0x8003B250`
explicitly replaces both with RGB 32 at `0x8003B444..0x8003B470`. The port had
retained a guessed 48 for both actor types; its initially zeroed player record
could also start with zero ambient until the presentation pass raised it.

The independent fixture executes both verified retail executables: 50 player
spawn prefixes, 1,024 complete fade calls, and export-2 initialization followed
by shared spawning/export 0 for all 49 distinct creature module images (24 PAL,
25 US, representing 166 module occurrences per disc). Those creature spawns
retain RGB 64 without writing either colour. Geometry, model lookup and sound
services use explicit recorded hooks and neutral records; the native link and
monster-start functions execute. These are bounded initialization/fade tests,
not full-machine or paired retail-frame evidence.

The actor refresh now distinguishes first player conversion from a later
refresh. Four-player tests retain hit colour through refresh, compare 32
recovery ticks against native results, and reset each respawn to RGB 32.
Items, viewmodels and front-end showcase entities keep their own spawn paths.
No saved-game format changed.

The control failed 346 simulation checks and both disc-client ambient checks.
After correction all 73 CTests pass; the simulation passes 1,977 checks, the
full PAL creature client passes 560, and the focused US client passes 59.
Matched engine captures change 7,536 creature pixels and 4,234 player-body
pixels, confined to the respective bodies. These are before/after port
captures, not retail comparisons. Native traces, controls, logs and images
are in `.agents/tmp/functional-parity/actor-ambient/`.

The staged Release executable and SDL3 match the current build, and PAL/US
BASE1 each pass a 90-frame headless smoke from `.install` with no warnings or
errors. No new unrelated runtime issue was found. Automatic approval review
rejected cleanup with "blocked by policy": 58 converted raw PPMs, seven test
saves and one superseded partial disassembly remain (22,202,850 bytes).
`cleanup.json` records the checked paths; each PPM has a verified lossless PNG.

## Player-body animation progress, 28 October

Body pose sampling no longer chooses moves, advances time, consumes RNG or
completes deaths. The client advances each owner's body on a positive game
tick, including bodies that no viewport draws. RUN uses horizontal wish input;
JUMP uses the successful jump impulse. ATTACK cannot override either, and a
new nonzero damage byte requests PAIN. Death and respawn update the same state.

`q2_player_anim_step` implements the complete native `0x8003CE14`, including
advance-before-install, strict end boundaries, jump holds and contact release,
terminal/loop rest positions, pain/death locks and repeated attack restarts.
Pain consumes the shared RNG before the installation lock; death draws only
on the first request. Native corpse think tests the previous animation flags
before advancing, so a newly completed death settles on the following tick.

Independent fixtures execute 4,080 complete animation calls on each PAL/US
executable, covering boundaries and sequential histories. They also execute
48 bounded request-selection cases per revision. The latter supply jump and
weapon returns explicitly; they do not execute those two producer functions
or a whole player think. Loader and player-binding execution establishes that
block D's three positions are multiplied by five, then Stand/Run's rest is
rewritten to start. Earlier claims in `model.h` and reconstruction note 51f
denying that loader conversion were incorrect and are now corrected.

Regressions cover all four owners, successful jump versus airborne movement,
real railgun damage and pain consumption, offscreen death/respawn, non-ticking
frames, and repeated pose sampling at 30/60/90/120 Hz display deltas. The real
Male2 model fixture checks all ten clips at start/middle/end. Its moving-and-
firing scene retains RUN where the old renderer selected ATTACK. The controlled
before/after framebuffers differ in 5,694 pixels, confined to the body. The
restored old chooser/pose fails the priority and read-only sampling checks.
Evidence is under `.agents/tmp/functional-parity/player-animation/`.

Validation: all 73 configured CTests pass, including 618 split-client and
1,979 simulation checks. The actual-model fixture passes 326 checks on each
disc; all 18 PAL split-support scenarios pass, including match completion,
Versus rounds and return to the multiplayer menu. The staged Release client
and SDL3 match the build hashes, `--version` starts successfully, and PAL/US
each complete 90 BASE1 frames plus 600 four-player MATRIX5 combat frames with
zero errors or warnings. `stage.json` records commands, hashes and reports.
Twenty-one redundant raw PPMs (7,987,515 bytes) were removed only after lossless
PNG pixel verification and path/link checks; `cleanup.json` records them.

The old pain-animation regression was also found to be absent from its test
program's main function. Its replacement and the new animation regressions
are now explicitly invoked. This coverage defect and the stale format claims
were separate findings from the rendering fix.

**Remaining animation/weapon boundary:** the client still uses shot-serial
changes for its ATTACK request. Native `0x8004EE0C` returns the last substep's
fire result at `0x800B2B7E`: the state machine writes zero or the idle-fire
result (`0x8004FC40`), and continuous-fire arms can overwrite it. Dry result 2
also requests ATTACK; a later zero substep can erase an earlier shot. The
view-weapon driver still runs on display updates. These producer/tick ordering
differences require native driver sequences and client regressions before
the overall player-animation item can close. The current fixtures do not
claim to validate that boundary or whole-game RNG ordering.

Two additional implementation gaps were found while reconciling source:

- `client_advance_view_weapon` supplied zero to aim-assist easing. The native
  target search and its firing direction are restored by the auto-aim follow-up.
- The projectile world-hit path passes a NULL surface normal to grenade
  impact. Its bounce arm requires a normal, so geometry contact detonates
  instead of reflecting the grenade. This geometry defect is corrected by
  the grenade follow-up below; additional projectile details remain open.

Other source comments identify candidates whose shipped-game effect still
needs qualification: secret deduplication and the secret chime's source
position; reconstructed creature-hit knockback points and vertical impulse;
the shared AI noise stand-in and path-corner scan order; BFG beam
area retention and actor effect-area refresh; first-matching-unit briefing
selection; and the fifth mission camera's out-of-table fallback. A comment
labelled "DEVIATION" is not sufficient evidence of a reachable retail defect.
These candidates remain explicit below until their callers and state histories
are compared. Legacy host saves that predate secret-history storage cannot
recover that history; this is a host-format compatibility limit.

## Grenade geometry follow-up, 28 October

Launcher and released hand grenades now run the shared native toss behavior.
Runtime entity boxes precede PrimaryColl, supply their contact normals and
withdraw the whole displacement before the hull query. Geometry reflects the
remaining displacement, applies the SDK's quantized square root and vector
normalization with restitution 2304/4096, and rests below the native speed
threshold. Moving tosses use three shared RNG draws for tumble; resting ones
skip movement and tumble. Blur vertices use the pre-tumble attitude.

A rebound no longer applies blast damage or spawns a fireball. It queues
`wep_grenlb1b` at the resulting position. Shootable geometry receives the
native three-point contact damage. The fuse is checked after movement and
contact, including a rebound on the expiry tick. Damageable actor contact
still detonates; geometry contact suppresses that tick's actor sweep. The
creature-projectile/player-hit branch also now creates the explosion model
and sound that were missing there.

The independent fixture executes 2,592 complete toss calls on each PAL/US
executable, with explicit collision-service outputs. Native SDK instructions
and tables execute; the fixture models their GTE SQR/GPF operations. This is
CPU arithmetic and control-flow evidence, not a hardware GTE capture or a
complete native collision-world comparison. All 45,302 weapon checks pass.
The simulation suite passes 2,027 checks, including both grenade types,
entity contacts, rest, actor priority, glass damage and expiry ordering.

The real-disc fixture passes 1,483 checks on each supported disc. MATRIX5
captures show the old first-contact explosion versus the surviving rebound;
BASE1 cold saves preserve a moving rebound and resting grenade, including
their flags, velocities, angles, fuse and subsequent shared-RNG history.
Evidence is under `.agents/tmp/functional-parity/grenade-bounce/`.

All 73 configured CTests and all 18 PAL split-support scenarios pass. The
staged Release executable and SDL3 match the build hashes, `--version`
starts successfully, and PAL/US each complete 90 BASE1 frames plus 600
four-player MATRIX5 combat frames with zero errors or warnings. Commands,
reports and hashes are recorded in `grenade-bounce/stage.json`.

The owner-exclusion and actor-contact gaps found after the bounce fix are
corrected in the follow-up below. Simultaneous sound ordering still needs
native sequence comparisons. Gib toss uses inferred
Euclidean root and normalization instead of the SDK arithmetic measured here.
Extreme SDK inputs outside the signed-positive table domain retain a defined
host fallback and are not qualified by these cases.

## Projectile actor contacts and save continuation, 28 October

The independent fixture executes 768 bounded grenade owner-gate cases, 3,072
complete actor queries and five named integration probes on each verified
PAL/US executable. Both revisions agree. Grenade2 and released Grenade3 now
permanently clear their owner-ignore mask after the pre-move position leaves
the owner's world bounds expanded by 40 in X/Z; the native containment check
ignores Y. The query excludes an ignored owner before scanning, so it no longer
hides another valid target by selecting the owner and discarding the result.

The complete native query does not filter damage immunity. Its integer root,
strict endpoint gate and successive clipping differ from the older query;
the latter disagrees with 860 of the 3,072 fixtures when comparing selected
actors and clipped points. All five live projectile kinds now use the measured
query and retain its clipped point for damage and effects. Damage rejection is not equivalent to
removing a collision body: a body can stop a projectile without taking damage.

`q2_combat_trace_actors` now matches all 3,072 native selected actors and clipped
points, including the native fractional accumulator's overflow. The combat
suite passes 3,767 checks. The owner helper matches all 768 cases and the weapon
suite passes 47,612 checks. A missing `s3` initialization in the launcher fixture
was found when those cases were first checked against the implementation. It
was corrected from the thinker's prologue, then both original executables were
rerun. These are CPU execution comparisons with modeled GTE
operations, not hardware captures. Evidence is retained under
`.agents/tmp/functional-parity/projectile-contact/`.

The simulation now includes the single player in projectile contact/fuse
candidates, including a creature grenade's timed explosion. A grenade touching
its owner applies only half self-splash, without a direct hit. The host capture
god mode still refuses damage while retaining the collision body. Player damage
uses the entity origin and the measured contact without the former extra
feet-origin conversion; splash visibility likewise resolves the canonical
player origin. The simulation suite passes 2,127 checks, covering all five
projectile kinds, immune blockers, all four player owners and a creature owner.

The real-disc return/save fixture passes 1,531 checks per revision. Before the
fix, the grenade passed through its owner with health 100 and no explosion.
The fixed run contacts at a relative Z of 287 and leaves health 46. Engine
before/after frames are retained. Cold saves preserve both armed and cleared
owner masks, including a cleared grenade that re-enters the expanded box, and
match eight subsequent ticks of positions, velocities, attitudes, cells, fuses,
health and RNG. The existing flags payload carries the state without a format
change; older host saves cannot recover a cleared mask they never recorded.

That continuation also exposed a separate restore defect: ZPOP rebuilt a
projectile's cell using SecondaryColl, then moved it through PrimaryColl.
In BASE1 it restored cell 235 instead of 161 and missed a later wall contact.
Restore now uses the projectile hull; the complete continuation matches.
The previous rebound/rest/cold-save fixture still passes 1,483 checks per disc.

All 73 configured CTests and all 18 PAL split-support scenarios pass. The
staged Release executable and SDL3 match the current build; `--version` starts
successfully. PAL/US each complete 90 BASE1 frames and 600 four-player MATRIX5
combat frames with zero errors or warnings. `projectile-contact/stage.json`
records commands, hashes and reports. The matched before/after engine frames
change 110,689 pixels around the returning grenade's explosion; they are port
captures, not matched retail framebuffers.
Twenty-three redundant raw PPMs (8,737,113 bytes) were removed after lossless
PNG pixel verification and path/link checks; `cleanup.json` records the files.

**Still open:** hitscan and AI consumers of the older cylinder helpers need
their own migration and integrated qualification. The bolt trail and impact
sequence identified here is corrected by the follow-up below. Other projectile world/contact ordering,
simultaneous sounds, gib SDK arithmetic and extreme math domains remain in
the inventory. These contact and save fixes do not establish whole-frame parity.

## Bolt flight and impact sequencing, 28 October

The old comment claimed the trail used the committed endpoint, but the call
still passed the old position. The light also ran before movement, and expired
or actor-contact bolts emitted a normal flight trail. Actor hits additionally
emitted the world spark burst before blood. These extra draws changed later
particles and weapon randomness.

`generate_bolt_reference.py` now executes the complete PAL/US bolt sweep at
`0x80047C6C` and its blood spawner. Both revisions agree on 192 cases: lifetime
boundaries, clear flight, actor contacts, type-2 entity contacts shielding an
actor, hull impacts, flag combinations, negative/odd displacements and signed
halfword overflow. The native actor query runs unchanged. Entity contacts,
cached hull results, output allocation, damage and sound are explicit hooks;
zero views skip body packet submission. These are control-flow and effect/RNG
fixtures, not hardware captures or cached-hull equivalence tests.

The bolt path now follows those decisions. Expiry emits nothing. Type-2 solid
contacts precede actor damage, so a door shields the actor behind it. Actor
contacts emit the native blood pair, including immune collision bodies, without
an extra spark pair. The native type-2 impact branch retains the previous bolt
position for its sparks. Clear flight commits before lights and trail; hull
impacts retain that frame's flight effects before their spark pair. Impact-bit
gating and the otherwise-unused larger-light bit are covered independently.
The movement delta now uses the native signed-halfword truncation, and the
trail avoids signed-left-shift overflow while preserving native rounding.

Trail and impact draws now share the bolt's saved combat RNG stream. The first
test draft compared the separate global stream and did not seed the combat
stream; that fixture setup was corrected before accepting results. The wider
port still has separate combat, effect and global gameplay streams. Unifying
their native call order and save continuation is a remaining parity gap, not
something these isolated sweep comparisons establish.

`test_sim` passes 5,903 checks, including every fixture's position, liveness,
actor damage, lights, particle counts, anchors, ramps, velocity/offset hashes
and RNG result. The disc-backed BASE1 test passes 1,587 checks on both PAL and
US, captures the real trail, and compares 32 uninterrupted/cold-loaded steps
through impact and retirement, including cell and both saved RNG states.
The old BASE1 build fails the trail-anchor and light-position checks by 480
world units. Evidence is under `.agents/tmp/functional-parity/bolt-sequence/`.

All 73 configured CTests and all 18 PAL split-support scenarios pass. The
staged Release executable and SDL3 match the build, and `--version` starts.
PAL/US each complete 90 BASE1 frames and 600 four-player MATRIX5 combat frames
with no warnings or errors. Commands, hashes and reports are in `stage.json`.
The matched BASE1 port captures change 384 pixels along the six-particle
trail; they are not paired retail framebuffers.

Twenty-one raw captures were converted to pixel-identical PNGs. The cleanup
manifest also identifies an obsolete root-level `disasm` file whose SHA-256
matches the PAL executable segment, a disposable smoke helper and two
superseded status-build logs. Automatic approval review rejected the removal
command as "blocked by policy"; all 25 files remain, with `removed: false`
recorded in `cleanup.json`. No alternate deletion was attempted.

The cached native hull marcher, posed-model contact refinement, body packet
submission on the final hull-impact frame, non-type-2 entity contacts, noise
and simultaneous sound ordering still need integrated qualification. The
numeric fixture intentionally exposes its hooks instead of claiming those
paths are covered.

## Weapon driver, hitscan and deviation review, 29 October

**View-weapon driver.** 0x8004EE0C's only caller is 0x8003AD98 in the live
player think, which reads the frame's dt global 0x800B2DB4. Each substep calls
the state machine 0x8004F87C, and every one of its exits stores the result
halfword at 0x800B2B7E. 0x8004FA58 and 0x8004FAB0 store zero; 0x8004FC40
stores `s4`, which is zero unless the idle arm called the fire function at
0x8004FB30. The frame driver 0x8004FEE8 receives that address and the
machinegun, chaingun and hyperblaster arms overwrite it with their own fire
result (0x8004FFE8, 0x80050214, 0x800502E0). 0x8004F6D4 returns the last
substep's value, and 0x8003ADA0..0x8003ADB4 requests ATTACK for any nonzero
low halfword. Native fire functions return 1 for a shot (the blaster's
0x8004C1A4), 0 when the spawn failed and 2 when dry. The port's own enum numbers
a shot 0, so `q2_viewweapon.result` keeps the console's encoding.

The client now runs the driver once per logic tick with that tick's dt for
every player. The body chooser reads `q2_vw_result` instead of a shot counter.
The selection fixture was extended to fire result 2: native requests ATTACK
for it in all 48 relevant rows, matching both PAL and US. `test_viewweapon` pins
the result's life: idle shot, the next call's clear, the stale value on a zero
step, dry 2, an arm's round, and an arm's round erased by a later quiet
substep. The old counter also advanced on attempts the refire clock refused.
Holding a fed railgun for 300 headless PAL frames, the old build posed ATTACK
on 168 frames with 7 shots and the new one on 156 with 6. The muzzle test now
requires the shot on the frame that takes a tick. A frame between ticks fires
nothing, where the old build fired and lit the muzzle on it. A new MATRIX5
regression drives a dry click through the real client loop. It passes on the old
build as well, because that build's counter also counted dry attempts, so it is
a regression check rather than a control.

Limit: the port's driver is restructured. Its state machine runs at key
boundaries, plus an end-of-call idle check, and the fire report is computed
before the call. The result is written at the equivalent points, but within a
call that crosses key boundaries the ordering can still differ from native. No
native driver-sequence fixture has been executed.

**Hitscan.** Every native instant weapon calls LineIntersectCreCylinder
0x800544EC, the routine `q2_combat_trace_actors` reproduces, from the world
trace's stop to the shooter's ignore argument. The bullet does this at
0x8004891C, the rail at 0x80049318, the mission laser at 0x80048F8C and the
creature laser at 0x80049AA8. The rail makes one query and one DoDamage
(`a3 = 3`, 0x80049334), so it does not penetrate. The query does not filter
`takedamage`, so an immune body stops a round and shields what is behind it.
`q2_combat_hitscan` implements that shape; the player's bullets and rail, the
creatures' rounds and rail, and both lasers use it. The player's bullet blood
had been placed at the world fraction, on the wall behind the creature, and is
now placed at the entry. `test_combat` and `test_sim` now pin a single rail
victim, immune shielding and entry-ordered selection, replacing assertions of
the old behaviour.

**Knockback.** DoDamage forms `target+0x54 - point` (0x80057EF0..0x80057F28),
and +0x54 is the mid-body origin. `q2_sim_hurt_player` built the player's
actor with the feet there. A reconstructed "blind" point 286 units back toward
the shooter then cancelled the error, so every creature round pushed
horizontally. The actor now carries the origin, the reconstruction is gone,
and rounds pass their real entry point. A level round pushes 533 units back
and none vertically; a round from 1,200 units above pushes 275 back and 456
down. Only mods 3, 13, 15 and 18 read the point as a direction. Still open:
for a client target the bullet arm adjusts the entry with GenMissileHitPlane
(0x80048930..0x80048954) against the previous frame's view plane, a shift of
at most a few dozen units per axis. That adjustment is not reproduced.

**Drowning.** 0x8003D358 `slti v0, a2, 3601` / `bne v0, zero, 0x8003D3E0`
takes the 750-tick ambience arm while air is below 3601. It drowns at
`air >> 10`, capped at 7, only once air reaches 3601. The port had the arms
reversed. A submerged player took 0..3 points per pass for twelve seconds
and then nothing indefinitely. `test_sim` now checks no damage below 3601,
three to seven per pass after it, and continued damage.

**Smaller corrections.** Gib rebounds use SquareRoot0 0x8008A7E8 and
VectorNormal 0x8008A588, as the shared toss mover does (0x800466F4,
0x80046A14). A 3200 rebound becomes 1798, not 1800. The BFG beam's far end is
the target origin plus `(572 - height) / 2`, truncated toward zero
(0x80048D4C..0x80048D74). The secret chime plays from the entity origin
(`s2 + 84`, 0x80028FA0), not the feet.

**Qualified as equivalent or unreachable.** These verdicts come from native
reading and shipped-data censuses; no fixture executes them.

- Secret deduplication: 33 of the 34 INSECRET items are fired by volumes in
  one-shot records (flags 0x48 or 0x50) that nothing re-enables. BASE0's crate
  record fires once.
- Briefing unit: every campaign map's strings carry exactly one unit, which
  equals its level-table id.
- Path corners: native uses linear scans, not an area tree. Every map has at
  most one PathCorner group with at most 32 corners, and no duplicate
  positive targetname.
- Actor effect area, and the BFG beam's area while the beam is refreshed.
- Muzzle roll: RotMatrix is Ry·Rx·Rz with roll innermost, so the native fire
  matrix and the port's camera transpose compose identically. A one-unit
  rounding difference remains: RotateS floors in the Y-negated frame.

The unit-five board camera is reachable through BOSS2 record 0xC0. It reads
the level table's static first entry, and the port keeps the player camera as
host policy.

**Identified, not yet corrected:**

- Player noise. Native PlayerNoise (0x80062B80) spawns two noise entities per
  player: class 201, no client and no object, with their origin frozen at the
  noise and `teleport_time` stamped. The port points both slots at the live
  player stand-in. As a result, hearing propagates sight (FoundTarget
  publishes only for a client, 0x8005D11C), a creature hunting a noise never
  re-runs its sight checks against the player (0x8005D4A4), and it follows the
  player rather than the noise point. Its 50-tick expiry reads the noise's
  health through a null object pointer (0x8005DE48); that read is undefined,
  but in practice the noise counts as dead. Missile and bullet impacts make
  type-2 noise natively (0x8004861C, 0x80048650, 0x800489AC), the grenade
  and BFG fire functions make none, and wading in makes none. This needs
  separate noise entities, save and zone references to them, and AI
  regressions.
- The BFG beam freezes its endpoints for up to about four ticks when a
  refresh is skipped; native recomputes them every frame (ProcessSplurges).
  This is visual only.
- The AI body clip (aiworld.c) still uses the older cylinder arithmetic. Its
  list order against native list membership is unqualified.

All 73 CTests pass. The disc-backed muzzle, weapon (grenade, projectile
contact, bolt, auto-aim), save, player-animation and split-screen suites pass
on both PAL and US.

## Limits

The audit remains open. Passing suites did not demonstrate full functional
parity: the follow-up above found the missing player-setup flow outside their
previous coverage. A script sweep or campaign route is not an exhaustive
playthrough of all possible state combinations. The selector review closes the
identified COMMAND, SECURITY and BOSS2 gaps but does not establish arbitrary
module execution. Static candidates are not proof of register values or branch
coverage. POWER1 and BIGGUN's secondary hook are now accounted for, and the
suspected COMMAND/POWER1 counter consumers proved inert. Shared acid/lava
sound dispatch and its bank fallback are now covered. The BOSS1 entrance branch
and BASE3/MAGDEMO's wall callback are covered as well. The shared ordinary
laser damage path and effect ordering are now checked; other registered frame
branches still need to be assessed
against their effects and persistent state.
Signed damage now follows the measured native arithmetic, but no normal-play
negative-damage caller has been established. Native damage and its feedback
preserve the view-amplitude fields; the port no longer assigns guessed values.
The measured reader is retained without claiming that an exhaustive trace of
every possible writer in every module has been performed.
The creature and entity audits describe their native coverage.

Remaining fidelity/measurement work includes paired retail camera/state captures,
NTSC comparison captures, SPU key-on transitions, host output conversion and measured final audio amplitude,
hardware movie A/V phase, and GPU cycle/scanout timing. See the
[renderer](RENDERER-AUDIT.md) and [audio](AUDIO-AUDIT.md) audits. Physical controller
ergonomics, audible output and physical host-device switching remain hands-on checks.

### Player auto-aim follow-up, 28 October

`FindPlayerTarget` (`0x80039DDC`) now searches eleven elevation rays in native
order: zero, then alternating positive/negative offsets in steps of 512 through
2560, with a forward extent of 8192. Its initial projected-screen filter is
overwritten unconditionally by the full actor list. The implementation therefore
adds no visibility, health or damageability filter. Each ray uses the measured
actor query, followed by a PrimaryColl test to the clipped contact; solid-entity
collision is not part of this native visibility test. The first clear ray wins.
The SDK normalization and ratan2 results feed the firing direction and barrel
target. A miss retains the native half-first-ray direction and targets pitch zero.

Acquisition uses the previous published viewport position and cell, before the
current live player's movement/look input. Easing runs once per logic tick and
the display driver copies its result. All fire substeps retain that tick's aim;
grenade paths that use their separate throw matrix keep that native distinction.
Corpses skip acquisition/easing. The optional versioned PLYR tail preserves the
direction, target, partly eased barrel, viewport origin/cell and validity across
cold saves; older player records remain readable.

The fixture executes complete PAL/US search, rotation, actor-query and SDK math
instructions with synthetic actor/window inputs and explicit hull visibility.
All 256 cases per revision agree, including every elevation, camera index,
occlusion, null viewport, roll and rejected initial screen projections.
`test_combat` checks the exact target, direction, pitch and visibility arguments.
The client regression covers four owners, pre-input acquisition, a viewport
distinct from the current player position, retained fire direction, display-only
updates, target loss, corpse behavior and immediate firing after cold loading.
The disc-backed BASE1 capture changes the barrel from zero to -160 while leaving
the camera level. Both PAL and US pass. Evidence is under
`.agents/tmp/functional-parity/autoaim/`.

These comparisons do not establish whole-session actor-list ordering, posed hit
refinement, full weapon-driver scheduling or matched hardware picture output;
those remain in the corresponding rows below. The original before-run log also
contains five subsequently corrected test assertions that used floor shift for
negative bolt velocity; native bolt division truncates toward zero. The six
barrel-state failures are the actual integration control.

### Consolidated remaining work, 28 October

This is the known remaining scope, not proof that undiscovered defects are
absent. A missing implementation, an unmeasured result and an archival format
question are different kinds of evidence; none is silently counted as complete.

| Area | Current limit | Evidence needed to close it |
| --- | --- | --- |
| Player-body / weapon scheduling | Body transitions, RNG use and draw-independent advancement have native fixtures. The view-weapon driver runs once per logic tick, and ATTACK follows its last-substep return, dry 2 included (29 October section). The port's restructured driver loop can still order writes differently from native within a call that crosses key boundaries. | Execute full native weapon-driver sequences against the port's loop, and compare integrated live/death ordering and RNG histories. |
| Projectile and other collision/toss details | Grenade bounce/owner lifetime, projectile actor queries, owner self-splash, cold-save cells and bolt flight/impact effects are corrected. Hitscan now uses the native query, with a single rail victim, immune shielding and entry-point blood. Knockback uses the entity origin, and gib rebounds use SDK arithmetic. Still open: the AI body clip's arithmetic, GenMissileHitPlane for client targets, the cached bolt hull marcher, posed contact refinement, final-frame body submission, other entity/contact and simultaneous-sound ordering, and extreme math domains. | Migrate the AI body clip and compare it with native list order. Qualify the remaining collision, presentation and sound boundaries and reachable numeric domains. |
| Shared gameplay randomness | The port retains separate combat, effect and global gameplay streams. Bolt trail/impact draws now share their saved combat stream, but native BIOS randomness is shared across subsystems. | Reconstruct reachable caller order, unify stream ownership and qualify integrated gameplay plus cold-save histories without claiming isolated seeded fixtures establish whole-frame parity. |
| Other explicit source deviations | Reviewed 29 October. The chime origin, knockback, BFG beam centring and drowning arms are corrected. Secret dedup, path-corner order, briefing-unit selection, actor effect areas and the muzzle's roll composition are equivalent or unreachable. The unit-five camera stays host policy. Remaining: a one-unit muzzle rounding difference and the BFG beam's endpoint freeze when a refresh is skipped. | Pin the muzzle rounding against a native RotMatrix/RotateS fixture, and recompute BFG beam endpoints per submit while the ball lives. |
| Creature hearing (player noise) | Confirmed defect. Both noise slots point at the live player stand-in instead of PlayerNoise's frozen, client-less class-201 entities. Impact noise and the grenade/BFG/wade noise rules differ (29 October section). | Separate noise entities with save and zone references, FindTarget's NOTARGET owner test, impact DoNoise, and AI regressions for propagation, re-sighting, the walk target and expiry. |
| Native mission and campaign behavior | Named handlers are implemented, but static LevelBin fallback does not execute arbitrary branches or track registers. Registered frame callbacks, trigger combinations and their persistent state are not exhaustively qualified. | Compare reachable native branches and state transitions across difficulties, zone returns, deaths, restarts and saves; extend regressions for each discrepancy. |
| Whole-game behavior | Creature callbacks, movement, weapons, menus and multiplayer have focused native/functional coverage; a scripted campaign route is not a complete retail playthrough. | Matched retail state/input traces for remaining interactions, including boss combat and movement edge cases. |
| PAL rendering | No paired retail scenes with matching camera, animation, lighting and RNG are available for current gameplay output. | Compare matched native frames, including near-eye geometry, translucency, water and split-screen. |
| NTSC and other revisions | Selected US loader states now match independent native execution, and both PAL/US pass all-zone allocation coverage. Matched NTSC picture/audio captures are still missing; these loader comparisons do not qualify every subsystem or revision. | Re-run behavior, picture, movie and audio acceptance against each supported build and qualify additional revisions separately. |
| GPU timing | GPU duration, CPU/DMA contention and scanout racing the draw stream are unmodeled. | Establish shipped-game impact with packet/bus traces, then implement any required timing behavior. |
| GPU/GTE arithmetic coverage | Existing external golden vectors cover a subset of coordinates, windings, gradients and opcode modes. | Broaden independent hardware acceptance data; do not derive expected values from the port. |
| Flat-polygon allocation | The client uses native heap accounting for known layouts. All 82 selected PAL/US load phases match native mission dispatch and loader results, with separate coverage of all 102 direct-level selections. Multiplayer's erroneous Tribute allocation is removed; US four-player TIMS now receives the native 5,664 packets. All 115 zones pass on each disc. Unknown layouts or rejected accounting retain the provisional fallback. | Qualify arbitrary outer-state histories and recording allocations. Intermission retention and view counts are still supplied as fixture context; loader traces and all-zone coverage do not establish whole-session/hardware parity. See the renderer audit. |
| Final audio amplitude | CD/main register values and gain units are fixed; the complete output chain lacks independent hardware measurements. | Measure final balance, clipping order and least-bit rounding with voices and CD mixed. |
| XA and output conversion | The mixer now stays at 44,100 Hz with the CD's 25-tap interpolation; the XA predictor, filter rounding/start state and device conversion remain unqualified against hardware. | Compare the complete CD waveform and final device output against independent hardware measurements. |
| SPU transitions and coverage | Absolute key-on latency/phase/history, remaining fractional filter positions and general ADSR settings are not fully established. Two finite END/repeat captures are excluded from the steady-waveform suite. | Match independent transition captures, including those exclusions. The game-specific ADSR setting is `0000/0000`. |
| Reserved ADPCM shifts | The shift-12-through-15 hardware capture does not match the fallback; all 2,475 PAL samples use valid shifts. | Match an independently verified model, without treating this unused PAL edge case as a current audible defect. |
| Movie synchronization | Software scheduling and fades work; initial hardware A/V phase, downstream device latency and long-run alignment are unmeasured. | Compare independent retail picture/audio captures and host output timing. |
| Host audio devices | Missing/failed streams now retry, with nonfatal initialization and silent movie-clock continuity. Open default-device migration is delegated to SDL; physical switching and real speaker latency remain unqualified. | Exercise actual unplug/replug and default-device switching, and measure audible latency. Automated failures and real SDL dummy-output recovery are covered in the audio audit. |
| Physical controllers | Automated tests cover synthetic events; physical ergonomics and rumble are not qualified. | A specifically authorized hands-on session with the intended controllers. |
| Undefined native memory | Enemy-less Boss1 aim and the stand-on-entity branch can depend on uninitialized PS1 stack history. The port uses documented defined behavior. | Full machine-history evidence if reproducing these invalid/undefined states is required; another actor-state formula cannot establish equivalence. |
| Historical reconstruction notes | `openquestions.md` retains stale open markers and low-impact metadata questions. Scene bytes/bits, resource fields, remap IDs and authored coordinate/bounds provenance are not all classified. | Reconcile each note with current code and native consumers. Count a functional defect only when a reachable behavioral effect is demonstrated. |

The table contains twenty-one known remaining areas. It is an inventory of current
evidence, not an assertion that every undiscovered bug has been enumerated.

CHD/MDS input, perspective-correct rendering, multiple host mice and revival of
cut multiplayer modes are host extensions or intentionally unsupported features,
not missing behavior of the supported retail game. Host file containers, CRT,
bloom and adaptive presentation likewise have explicit divergence policies.
The owner's requested stronger pickup autoswitch is also deliberate; disabling
it restores the native blaster-only pickup switch.

The CD-DA item has been reclassified after executing both native music paths.
All 51 named playlists on each supported disc either select XA ids 2..21 or
stop; 1,170 starts across repeated loops never select negative-file ids 0/1.
Those reserved entries have zero duration, boot TOC setup does not change
them, and the exported `PlayThisXA` function does nothing. CD-DA PCM remains
unimplemented for modified tables or a different revision, but this is not an
identified missing soundtrack in the supported retail game. The PAL audio
track is all zeroes; the US track contains only signed PCM values -1, 0 and 1.
The comparison also fixed an implicit restart at a playlist's zero terminator;
native playback stops there. Both finite control lists failed before that fix,
while shipped nonempty lists already loop explicitly. The client now matches
native selection, channel, timer, cursor and termination results in 3,156 checks
per disc. See the [audio audit](AUDIO-AUDIT.md#cd-da-and-playlist-scope)
for the evidence and its limits.

## Unrelated housekeeping

The current Git inventory, rechecked during the 28 October audio-clock pass,
contains only the main checkout on `codex/functional-parity`. The six entries
listed below are no longer registered. This pass did not remove them;
`audio-clock/worktrees.json` records the empty secondary-worktree inventory.

Historical finding: these worktrees were clean with all commits already in
the current checkout (`a590ce9`). They were reported as removal candidates:

| Path | Branch |
| --- | --- |
| `E:\Repositories\Q2PSX-PC\.claude\worktrees\jovial-easley-421933` | detached at `816e874` |
| `E:\Repositories\Q2PSX-PC\.claude\worktrees\youthful-jennings-369181` | detached at `ac7d59d` |
| `E:\Repositories\Q2PSX-PC-fire` | `claude/creature-fire-secret-b8bfe1` |
| `E:\Repositories\Q2PSX-PC-mg` | `claude/mg-recoil-doors-c4e349` |
| `E:\Repositories\Q2PSX-PC-rail` | `claude/railgun-tank-7c31a2` |
| `C:\Users\djdac\AppData\Local\Temp\claude\E--Repositories-Q2PSX-PC\37c44139-0571-4fd4-a2c6-c44bd7b15f05\scratchpad\headwt` | detached at `d93b1fe` |

All six were rechecked on 27 October during the BOSS1 pass: clean, fully merged,
and with their latest commits dated 22 October. None is checked out on the
default branch. `boss1-entrance/worktrees.json` retains the check.
The LAB lift pass rechecked all six with the same results; its current record
is `lab-lifts/worktrees.json`. The broader native-lift pass confirmed the same
state in `native-lifts/worktrees.json`; the earthquake and laser-course passes
confirmed it again in `earthquakes/worktrees.json` and
`security-lasers/worktrees.json`. The orb/lift pass confirmed all six remain
clean and fully merged, with the same commit dates, in
`security-orb/worktrees.json`. Those removal recommendations are historical;
the current inventory above supersedes them.
The LAB/BOSS1 sweep pass rechecked the same six clean, fully merged checkouts;
`native-sweeps/worktrees.json` records their paths, branches and commit dates.
The lifecycle pass rechecked all six with the same result; its record is
`native-lifecycle/worktrees.json`.
The 28 October audio-gain pass again found all six clean and fully merged,
with no commits newer than 22 October; `audio-gain/worktrees.json` records
their current paths, branches and dates. They remain removal candidates,
and none was removed.

The orb/lift review also found PAL's `LiftRing1` / `ring` asset-name mismatch,
documented above. It remains an explicit source-data discrepancy; there is no
evidence for substituting the differently named node.

The damage pass removed its two converted raw PPM captures and superseded
`view-build.log` / `inspect-build.log` after verifying their absolute paths and
absence of links. The PNGs, failing controls and final build/test logs remain.

The save fixture defaults to Windows TEMP. The focused rerun redirected TEMP,
TMP and TMPDIR into the project task area. Automatic approval review blocked
deleting emptied test directories (reason: "blocked by policy"), including
`C:\Users\djdac\AppData\Local\Temp\q2psx_save_test`. Disposable files were
removed; selected PNGs and final logs were retained.

The BOSS1 pass completed the previously blocked file cleanup with explicit
paths after a manifest-driven request was rejected. It removed 24 converted
raw PPMs, one duplicate PNG, two crosshair-only POWER1 diagnostic PNGs, and
seven superseded logs across `unlocks/`, `selectors/`, `callbacks/`, `power1/`,
`burn-sounds/` and `boss1-entrance/`. Absolute targets and link-free ancestors
were checked before deletion; pixel comparisons verified every retained image.
The cleanup record is `boss1-entrance/cleanup-images.json`. Useful before/after
PNGs, failing controls, final build/test logs and unique sweep captures remain.

The laser-wall pass removed six converted raw PPMs after verifying their PNGs
contain identical pixels, plus one superseded build log. Automatic approval
review rejected removal of the four empty smoke-profile directories and their
parent under `.agents/tmp/functional-parity/laserwall/smoke-saves/` with reason
"blocked by policy". These empty directories remain; PNGs, disassembly, final
test logs and unique headless-sweep evidence are retained.

The ordinary-laser pass removed nine converted raw PPMs after checking their
absolute paths, link-free ancestors and identical retained PNG pixels. The
conversion record, current sweep evidence, disassembly, failing control and
final test/build/stage logs remain in `static-lasers/`.

The LAB lift pass removed 15 converted raw PPMs, 13 disposable regression saves
and its superseded census log after checking exact paths and link-free
ancestors. PNG pixel equality and the duplicate census rows were verified;
`lab-lifts/cleanup.json` lists the removed files. Current logs, both failing
controls, native disassembly and selected PNGs remain.

The broader native-lift pass removed 15 raw PPMs after verifying identical
retained PNG pixels, 13 disposable regression saves and its superseded census
probe. Exact absolute paths and link-free ancestors were checked first;
`native-lifts/cleanup.json` records all 29 removals. The failing control,
corrected census, final logs, disassembly and sweep evidence remain.

The laser-course pass removed 194 converted raw PPMs and 22 disposable disc-sweep
saves after checking the exact task-local paths and every ancestor for reparse
points. Identical PNG pixels, test logs and reports remain;
`security-lasers/cleanup.json` records all 216 removals.

The orb/lift pass removed 207 converted raw PPMs and 35 disposable regression
saves. Exact absolute paths and every ancestor were checked for reparse
points, and retained PNG pixels were compared before deletion.
`security-orb/cleanup.json` records all 242 removals; the before/after PNGs,
native disassembly, failing control and final build/test/stage evidence remain.

The LAB/BOSS1 sweep pass removed 210 converted raw PPMs, 35 disposable test
saves, one duplicate first-frame PNG, its temporary control-source backup and
the superseded control-build log. Every absolute target and ancestor was
checked for reparse points; retained PNG pixels and the restored production
source were verified. `native-sweeps/cleanup.json` records all 248 removals.
The original failing control, native disassembly, matching before/after PNGs
and final build/test/stage evidence remain.

The earthquake pass removed 16 converted raw PPMs, 13 disposable regression
saves and two superseded logs after checking exact paths, link-free ancestors
and identical retained PNG pixels. `earthquakes/cleanup.json` records all 31
removals. Before/after PNGs, native disassembly and final test/stage evidence
remain in that directory.

The lifecycle pass removed 237 converted raw PPMs, 35 disposable regression
saves and one duplicate corrected capture. Exact absolute targets and all
ancestors were checked for reparse points, and retained PNG pixels were
verified. `native-lifecycle/cleanup.json` records all 273 removals. Native
disassembly, the failing control, before/after PNGs and final logs remain.
