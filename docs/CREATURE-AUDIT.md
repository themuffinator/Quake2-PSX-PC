# Creature behavior audit

All **22 distinct creature modules have native behavior implementations**. The
PAL disc (`SLES-01534`) contains 166 instances across `COMMON.DAT` and `ZONE*.DAT`.
Coverage here means transcribed callbacks, frame methods or direct-think handlers,
not just decoded animation/fire imports. Shared actor movement, impulse consumption
and interpolation are reconstructed from the PAL executable as described below.
The checks establish those tested paths, rather than whole-game frame-exact parity.

## Coverage

| State | Modules |
| --- | --- |
| Native callbacks and frame methods | Soldier (blaster, shotgun, machinegun), Infantry, Berserk, Gunner, Tankcomm, Arachner, Insane, Flyer, Gladiator, Hover, Parasite, Ironmaiden, Strider, Boss1, Boss2, Jorg, Rider, Medic |
| Native direct-think scripted modules | RiderStand, Rider2, Rider3, DeadComm |

The runtime loads native modules from COMMON and the active zone before borrowing
COMMON donors. Unknown modules remain unbound; generic decoded actions are not
treated as complete AI. The original COMMON-only medium-difficulty wake census
counted 651 population records, including 139 rejected for an unavailable module.
That is a baseline, not a current zone-aware rejection count. BASE2 zone 1 now
places 32 records instead of 29, adding its three scripted Flyers. Records include
other zones, difficulties and scripted batches, not simultaneous enemies.

All 166 module instances relocate successfully. Same-name relocated bodies match
except for the Soldier and Flyer in MAGDEMO. The native Soldier is additionally
tested against MAGDEMO's own module. Both Flyer copies are now tested too: their
AI instructions are identical, with only two unused bytes after a terminated
blur vertex list differing.

## Corrections made by this audit

- Gunner bullets start at posed muzzle vertices 227/231 and use the module's
  negative velocity lead toward the enemy's eye.
- Gunner grenades start at vertex 273 and enter the projectile pool, with the
  retail upward/sideways toss, ownership, bounce/collision and 600-tick fuse.
  Launching a grenade no longer applies instant player damage.
- Arachner rail shots initialize every field, use the two posed barrel
  attachments, and aim at the saved target. The right barrel refreshes the target
  after firing; the left uses that snapshot. Rails pierce actors and stop at walls.
- The player's AI stand-in receives current velocity and crouched eye height.
- Damage carries its source actor into the AI reaction, so friendly fire can
  trigger retaliation against the shooter. Melee can damage another creature.
- The inspector scans zone files and reports unsupported behavior explicitly.
  `moddisasm` accepts `MAP/ZONE0.DAT` as well as a map name.
- The Flyer implements every registered callback and frame method, including
  the distinct stand/walk/run records sharing frames 13..57, eight alternating
  posed blaster shots, two slashes, the range/80% melee repeat decision, all three
  pain moves, nightmare/debounce ordering, shared transition state and immediate
  damageable TOSS corpse state. Its two blade trails use the native vertex lists.
- Flyer shots retain the module's unnormalized eye delta. The engine divides
  it by 64; the nominal speed 1000 is not used. Frames 82/85/88 select hyper
  bolts (flags 14/MOD 5); the other five use flags 11/MOD 6.
- Gladiator implements its saved-target rail attack, strict minimum range,
  two melee strikes, airborne pain transition, and native death/corpse rules.
- Hover implements distinct walk/run moves, alternating blaster barrels,
  visibility/health-gated 60% burst repetition, three pain moves, and death
  completion that sends the body through the shared gib/explosion dispatcher.
- Parasite implements the posed drain tether, native range/pitch/trace tests,
  eleven damage pulses, launch/reel sounds, idle loop, pain and corpse behavior.
- Ironmaiden implements rocket bursts, reload/refire branches, blade slash and
  trail, dodge, pain and both death moves. Its overwritten sound slot is resolved
  separately from conditional fallback registrations.
- Strider implements its clamped big-turn callbacks, saved-target blaster/laser,
  melee/stomp, protected attack frames, pain and shared-phase death explosions.
  The laser damages one body and continues drawing to the wall. Its sound
  fallbacks retain distinct address identities even when primary names match.
- Boss1 implements its six-shot chaingun burst and repeat gate, three separately
  interpolated rocket tubes, attack protection, pain, death explosions and the
  exported chaingun muzzle flash. Boss2 implements twin guns, four simultaneous
  rockets, its body trace and attack probabilities, one-time activation rise,
  nightmare pain and immediate death destruction.
- RiderStand implements its idle loop, one-shot pain response, eight-tick
  shrink, halfway teleport sound and final corpse state. Its sounds originate
  at the player. Rider2 advances to its final frame and stops; Rider3 loops its
  twenty frames. DeadComm performs its delayed drop and activated animation.
  BOSS1's `E_NearMakron` and LAB's `E_GotHead` disabled-event checks activate
  the corresponding actors once, as their native LevelBin routines do.
- Direct-think actors resolve poses through export 3 without an animation move.
  The scheduler requires a positive deadline and clears it before invoking the
  installed callback. Saves and visited-zone runtime preserve callback identity,
  projection scale and its consumed pain response; classless modules retain their
  owning bind and population identity. These actors do not increase kill totals.
- Jorg implements paired machineguns, burst repetition, BFG attack, protected
  pain branches, stomps, phased death explosions and the delayed Rider spawn.
  Its embedded Rider uses its own method table, move records and sound addresses;
  class byte 83 alone cannot identify which Rider implementation owns an actor.
- Rider implements the 17-shot blaster sweep, saved-target rail, BFG, native
  attack probabilities/cooldowns, pain branches and immediate death destruction.
  Creature BFG shots enter the shared projectile simulator. The engine wrapper
  fires along actor facing and ignores the module's aim/speed arguments, as the
  PAL executable does at `0x800621BC`.
- Medic selects the visible corpse with greatest original maximum health,
  checks cable range/pitch/body trace, applies the revival effect, and reconstructs
  the creature on the next actor frame. The old body's destruction, fresh health,
  live hit cylinder, player target and combat actor all survive this transition.
  Blaster bursts, refire, ducking, pain and damageable corpse behavior are native.
- Dormant records bind callbacks without running export 0. First activation
  consumes the native spawn RNG draws; save restore and zone resume do not repeat
  them. CAIS/ZPOP v4 preserve initialization, module-shared decisions/explosion
  phases and pending Rider/revival handlers. Older CAIS rosters remap their links
  around the newly supported classes.
- The decoder keeps separate method tables for a module's classes, bounds a
  function walk before adjacent code, and follows Strider's chained multiply-back
  sequence correctly (`random % 50`, not `% 18`). Inspector frame ranges use the
  same export-3 record layout as runtime posing.
- Animation names come directly from export 3, with the native record stride
  and negative-frame terminator. Reversed unused ranges are skipped, as the
  engine does; Boss2's reversed Stand2 entry previously discarded every name.
  Unclaimed-name diagnostics and clip-piece discovery use the same table.
- Arachner's cloned pain request plays the original sample at pitch 50; the
  other arm retains pitch 35. The client test verifies actual mixer voice steps.
- Zero-damage and godmode hits reach creature retaliation without inventing
  a pain call. Refused damage does not trigger a reaction.
- Melee uses the native computed contact point for damage and view kick. Its
  separate impulse reaches player movement, including signed halfword wrapping
  and the upward limit. Creature impulses now survive actor refreshes and enter
  the same native physical integrator, independently of AI velocity.

## Verification and limits

### Muzzle effects and SECURITY supertank placement

The muzzle audit checked all 166 module instances. Soldier, Infantry, Gunner
and Boss1 have flash render hooks; their attachment pairs are 228/234, 200/205,
227/231 and 412/419. Boss1 passes a radius of 240 to `0x80031094`; the others
pass 120. Bullet and shotgun wrappers arm these hooks. Blasters, rockets,
grenades and rails keep their own projectile/trail effects. Other creature
modules do not request this flash sprite.

Boss1's missing flash is now connected with its own size. Sprite staging grows
beyond sixteen entries; the native sixteen-light limit does not limit the
number of visible sprites. Player shotgun, super shotgun, machinegun and
chaingun lights now drain after the viewweapon fires, including display frames
between simulation ticks. Each event is consumed once. Continuous projectile
lights remain visible between ticks while one-frame muzzle lights expire.

SECURITY zone 1's supertank was also placed through its platform: the downward
spawn sweep only queried SecondaryCol, while the platform at Y=-11040 is a
solid scene entity. Creature placement now includes those solid boxes. Its
origin rests at -11326 (feet at -11040), instead of -10471, which was 855 units
too low. This applies to creature placement generally, without a boss offset.

`test_client_muzzle` covers all eleven player weapons, dry fire, event lifetime,
four-player firing and the four disc-backed creature flash attachments. Its
headless disc run also checks the supertank's rendered height and continued
platform support. `test_effect` queues 65 simultaneous flashes in four views;
`test_item` covers thin platforms, inactive boxes, trigger volumes and a nearer
static floor. Run the capture fixture with an existing output directory:

```powershell
.\build-msvc\bin\Release\test_client_muzzle.exe --headless .install/disc/game.cue .agents/tmp/muzzle-flashes/after
```

Evidence and native export disassembly are retained in
`.agents/tmp/muzzle-flashes/`.

This checkpoint passes 59 CTests, 271 disc-backed muzzle/placement checks,
539 creature client checks and 21,354 native creature checks. The staged
headless shotgun smoke run fires five shots with zero warnings or errors.

The placement inspection found that `q2_model_pose_low_y` compared a boolean
with `Q2_OK`, skipping valid parts. The fidelity pass below repairs that helper.
Further native inspection established that creature corpses retain the constant
model bias: a pose-dependent corpse correction would itself diverge from PS1.

### Shared creature placement audit

The floor fix is in the shared entity drop, not in Boss1. A disc-wide audit
enumerates all 49 map directories and 115 zones at each of the four difficulties.
It checks 450, 494, 561 and 651 resident authored placements respectively: 2,156
in total, covering 24 populated class IDs. There are no missed entity supports
or floor overlaps in this scan. The six records that require entity support
are present at every difficulty:

| Map / zone | Batch / slot | Creature | Static-only drop error |
| --- | --- | --- | ---: |
| JAIL4 / 2 | Zone2 / 0 | Insane | 878 units too low |
| LAB / 0 | Mercedes / 0, 1 | Ironmaiden | 58 units too low each |
| LAB / 2 | Zone2 / 2 | DeadComm | 668 units too low |
| LAB / 2 | Zone2 / 6 | Medic | 125 units too low |
| SECURITY / 1 | Boss1 / 0 | Boss1 | 855 units too low |

Two additional shared edge cases are covered: a downward probe beginning
exactly on an entity's top plane must keep that support, and an existing side
or ceiling overlap must not mask a floor below it. The drop now considers each
active solid box's top plane inclusively, keeps the nearest floor within the
original static sweep, and ignores trigger volumes. It uses the existing
286-unit physical body, independently of visual model height or creature class.

Dormant CREBATCH placeholders now repeat placement when actually summoned,
using the original authored feet and the current collision world. A raised,
lowered or removed platform therefore cannot leave a stale load-time height.
The creature world borrows the live world object rather than its replaceable
target array. Already initialized actors retain their runtime position;
revived bodies and boss children keep their existing lifecycle placement.

`test_client_placement` runs without a disc in CTest. It covers all 37 nonzero
class slots, exact contact, side overlap, nearest supports, inactive supports,
moving/removed supports before summon, target-array replacement, preservation
of initialized actors, failed-placement retry, and one-shot/dead actors. Its
optional headless disc run audits every zone and exercises delayed Boss1 and
Mercedes batches against changed support heights. Captures compare a diagnostic
reproduction of static-only placement with current placement through the actual
renderer, using the same camera and fixed frame step:

```powershell
.\build-msvc\bin\Release\test_client_placement.exe --headless .install/disc/game.cue .agents/tmp/creature-placement
```

The output directory must already exist. Omit it to run without captures;
append `--capture-only` to skip the disc-wide enumeration while adjusting a
capture fixture. Evidence is retained in `.agents/tmp/creature-placement/`.
This checkpoint passes 60 CTests, 3,706 disc-backed placement checks, 696
population client checks, 408 zone checks, 321 save checks, 271 muzzle checks
and 21,354 native creature checks.
The audit checks resident authored spawns and the tested lifecycle transitions,
not every possible later mover trajectory or combat displacement. Side contact
with an adjacent box is not counted as floor penetration unless the creature's
center lies over that box's actual footprint. Scripted decorative modules can
apply their own post-spawn offsets and are excluded from exact origin equality.

### Earlier verification checkpoints

The disc-backed creature regression runs attack/death sequences at all four
difficulties for the nine implemented population variants and the three MAGDEMO
Soldier variants. It verifies move/method binding, aimed shot parameters, attacks
and final corpse states. Client integration tests exercise actual firing hooks,
rail misses/hits, deferred grenade damage, player aim state and friendly fire.
Simulation tests cover rail penetration/occlusion and grenade ownership/fuses.

Validation on this disc: 45 CTests passed; the optional roster run passed 9,502
checks across 48 population/difficulty cases. Deterministic 300-frame smoke runs
completed on BASE1, COMMAND, WASTE4, POWER1, POWER2 and LAB. A separate 180-frame
POWER1 capture recorded eight traced rounds (three player hits, five misses) and
four launched grenades. The baseline's creature shots bypassed both paths.

The Flyer checkpoint passed 49 CTests and 11,389
disc-backed checks, including both module copies
and all four difficulties. A headless client fixture wakes BASE2's actual scripted
batch and verifies eight launched bolts, two slashes, blade trails and corpse state
(111 checks, with death delivered through the real damage path). BASE2 and
MAGDEMO headless smoke runs report zero warnings/errors.
Evidence is under `.agents/tmp/creature-reconstruction/`.

Gladiator and Hover extend the disc regression to 13,008 checks. The headless
client fixture passes 175 checks, including Gladiator rail/melee/death, Hover
barrel selection and corpse destruction, Arachner pitch, zero/godmode damage
reactions and player melee displacement.

Parasite, Ironmaiden and Strider extend the disc regression to 16,733 passing
checks and the headless client fixture to 253. Shared turn tests pass 315 checks,
simulation tests pass 666, and effects pass 1,012. Attached tethers retain their
last visible expiry frame. At that checkpoint Strider's exact-yaw branch was
treated as inside the attack cone. The fidelity pass below reconstructs the
preserved caller register that actually controls this branch.

Boss1 and Boss2 extend this to 18,585 disc-backed checks and 298 headless client
checks. All 53 CTests pass. The client fixture verifies the bosses' actual shot
routes, model-name posing, muzzle flashes and destruction. Boss1's native
machinegun leaves aim uninitialized after enemy loss; that otherwise undefined
case fires forward in this reconstruction.

The four direct-think modules extend this to 18,640 disc-backed checks and 387
headless client checks. The current checkout passes all 55 CTests. Client checks
include named-event activation, sound delivery, rendered projection scale,
animation posing and save/zone callback restoration. Native disassembly, test
logs and engine captures are retained in `.agents/tmp/creature-reconstruction/`.

Jorg, both Rider implementations and Medic extend the disc-backed suite to
21,354 passing checks and 464 headless creature client checks. The client fixture
exercises their actual weapon routes,
posed attachments, sounds, effects, full cold save reload and zone rebuild while
boss activation or revival is pending. Evidence for this pass is retained under
`.agents/tmp/creature-ai-completion/`.
The final checkout passes 56 CTests, plus 321 disc-backed client save checks,
408 zone checks and 768 population/script checks.

### Physical actor reconstruction

`cremotion.c` reconstructs the live handler at `0x8007EBC4`, the LinkEntity
contract at `0x8005C8C8`, animation selection at `0x8007E9DC` and initial posing
at `0x8007F474`. Physical origin/yaw, linked AI destination, physical velocity,
pending impulse and the per-actor clock are independent records. At each
30-unit AI boundary the mover feeds its actual origin back to the edict before
thinking. Every interpolation fraction uses a denominator of 30, including the
residual frame; it is not normalized by the remaining time. Wrapped 32-bit
products and signed-halfword stores preserve the executable's rounding.

The shared `0x8004583C` mover performs separation, ground projection, sliding,
walker step-up/down and final validity checks. Its `0x80045E74` integrator
preserves pre-impulse displacement, the no-gravity branch's retained arm bit,
gravity/liquid behavior, halfword impulse accumulation and the single-player
upward limit. The next interpolation call replaces the integrated delta, just
as the native handler does; adding a separate AI-velocity displacement would
invent motion. Detached corpses do not call this mover. Their dissolve handler
now consumes the actual engine frame delta.

The area flood (`0x800651D4`) and activity gate (`0x800578D8`) control suspension:
an inactive actor refreshes its clock without consuming its phase or impulse.
Connected areas, hidden portal markers, visible area records and the inclusive
8192-unit X/Z camera range determine activity. Native animation follows the
previous AI base and phase, retains held frames, and enables a ten-unit
outgoing/incoming clip blend in the transition's final phase. Queued turns use
the low halfword value at actor+0x2E8. Rendering, hit bodies, posed attachments,
sound origins and gib momentum consume the physical state.

CAIS and ZPOP version 5 preserve this state through native saves and zone
rebuilds. Earlier versions remain readable and initialize a physical record
from their restored AI origin. Zone reconstruction invalidates cached collision
cells and rebases the local actor clock.

The optional `test_cremotion --disc` runner executes the user's original PAL
instructions in a bounded R3000 harness. It compares 6,000 integrator cases,
2,000 live-handler cases, 2,000 animation cases and 1,000 area/activity cases:
**135,028 checks pass**. Collision and presentation calls in the live-handler
comparison are explicit stubs; this is not a full PS1 emulator or proof of
whole-game equivalence. The real headless client separately covers geometry,
combat, mid-phase save/resume, zone rebuild and pending-impulse persistence.
Its disc-backed run passes 542 checks; the roster passes 21,354, save 321,
zone 408, population 768 and movement 74. Deterministic staged-client smoke runs
on BASE1, BASE3 and SECURITY complete with zero warnings/errors; SECURITY
exercises moving, hunting and firing creatures.

### Remaining fidelity audit

The subsequent pass resolves the bounded sound scan, quaternion rounding,
Strider caller context and pose-bounds defect directly against the PAL code:

- `creinit.c` executes export 2 in isolated R3000 memory, including branch and
  load delays, packed sound names, stack spills, aliases, conditional fallbacks
  and overwritten slots. SoundIndex receives the actual map bank's availability;
  the final BSS handles determine playback. Sound registration, cloning and
  parameter imports retain request state; unrelated initializer imports use
  explicit stubs. This is not a general PS1 emulator. Unsupported instructions,
  addresses and instruction-budget
  exhaustion fail without publishing partial bindings. The input module stays
  immutable. The old hand-maintained fallback helpers remain diagnostic APIs;
  the client uses executed initialization. The registration inspector combines
  all-present/all-missing traces, not an assertion of arbitrary branch coverage.
  All 166 module copies execute under 18 bank configurations apiece, including
  their actual map banks. Strider's step slot `+0x2BD0` is `str_step`, falling
  back to `tnk_step`; the old literal scan incorrectly reported `str_deth`.
  MAGDEMO's Soldier has its sound slots eight bytes later than other copies;
  executed initialization handles that layout without a module-name offset table.
  Arachner plays its actual cloned pain handle, retaining its independent pitch
  of 50 while the original request remains at 35; overriding the original
  request would incorrectly change later ordinary pain cries.
- The inverse-cosine function now reproduces all 4,096 native entries, including
  cosine quantization and the two truncation boundaries. Slerp preserves native
  equal-input copies and the original incoming quaternion at the endpoint even
  when the interior takes the opposite sign. Quaternion-to-matrix doubles the
  operand before multiplication and shifting, preserving odd coefficients.
  The pose walker reads a final successor across block C into block D when the
  native code does, bounded by the complete model allocation.
- Vector-to-yaw now returns native signed `ratan2`, with ratio quantization and
  the large-magnitude division path. Numerically generated lookup values match
  all 1,025 PAL entries. The old sine-dot search and wrapped result were not
  equivalent. Strider's equal-yaw branch at module `+0x1278` retains `s2`:
  move-end repeats inherit zero from `0x8007EE08`, ordinary frame AI inherits
  the local frame index from `0x8006188C`, and `ai_run` supplies the player
  pointer from `0x8005E38C`. The port scopes this transient caller context; it
  is not actor state and does not belong in saves.
- `q2_model_pose_low_y` now visits valid parts and measures their posed vertices.
  Creature rendering retains `origin.y + 286 - ext2`, including corpses, as
  `0x80058870` and `0x800588F0..0x80058904` specify. The previously ineffective
  corpse-only pose adjustment has been removed.

Boss1's enemy-less machinegun remains an explicit fidelity limit. At module
`+0xB4C`, a null enemy skips every write to the three aim words at `sp+0x48`;
`+0xBF0` then passes those untouched words to fire_bullet. Native-instruction
tests demonstrate different directions for identical actor state with different
prior stack contents. Actor state cannot reconstruct that memory history.
The port retains its documented forward fallback rather than pretending that
it reproduces a particular PS1 stack. Ordinary native `ai_charge` also requires
an enemy before reaching this callback. Reproducing this invalid-state case
would require the full machine's stack history, not another aiming formula.

The expanded instruction harness checks 5,000 packed quaternion/slerp/matrix
cases, 10,000 angle/vector-to-yaw cases, every trigonometric table entry, and
all 3,241 variable-rate clips at eight positions each. All **25,928 complete
poses (426,080 part poses)** match native translations and quaternions, including
clip transitions. These comparisons complement the existing actor/impulse
tests; they do not assert whole-renderer or whole-game equivalence.
The current native run passes **3,325,005 checks**, initialization passes
**131,400 checks** (including specific fallback and cloned-request fixtures), and the headless
creature client passes **542 checks**. All **63 CTests** pass, along with the
disc-backed zone (408), population (768), movement (74) and audio (10,137) checks.
Staged-client SECURITY, BASE3 and MAGDEMO smoke runs use 300 fixed headless
frames apiece and finish without warnings or errors.
At this checkpoint the broader disc-backed save suite reported **one failure in 321 checks**:
its chaingun continuation diverges in RNG at step 6 after cold reload, while
the shot serial still matches. This surfaced during concurrent audio-request
integration and is outside the creature reconstruction; see `save-test.log`.
The creature fixture's own cold reload, revival, zone rebuild and pending
impulse checks pass. The [functional follow-up](FUNCTIONAL-AUDIT.md) subsequently
traced that save-suite failure to its non-advancing audio fixture; its corrected
production-frame scheduling passes all 333 checks without a gameplay workaround.
Evidence, native disassembly and real before/after render captures are retained
under `.agents/tmp/creature-fidelity/`.

Run the audit with the user's disc:

```powershell
.\build-msvc\bin\Release\test_creature.exe --disc .install/disc/game.cue
.\build-msvc\bin\Release\test_cremotion.exe --disc .install/disc/game.cue
.\build-msvc\bin\Release\test_creinit.exe --disc .install/disc/game.cue
.\build-msvc\bin\Release\test_client_creatures.exe --headless .install/disc/game.cue .agents/tmp/creature-ai-completion
.\build-msvc\bin\Release\q2psx-inspect.exe creatures .install/disc/game.cue
.\build-msvc\bin\Release\q2psx-inspect.exe moddisasm .install/disc/game.cue BOSS1/ZONE0.DAT 0 16 Ironmaiden
ctest --test-dir build-msvc -C Release --output-on-failure
```

Gameplay evidence is captured with `--headless --demo --watch --report`; `--shot`
reads the engine framebuffer. The task's logs and captures are under
`.agents/tmp/creature-audit/`, `.agents/tmp/creature-reconstruction/` and
`.agents/tmp/creature-ai-completion/` and `.agents/tmp/creature-motion/`;
they are not distributed game assets.
