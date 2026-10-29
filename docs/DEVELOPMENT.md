# Developer reference

Build, inspection and implementation notes for contributors. For installation,
controls and troubleshooting, open the [user guide](../USER-GUIDE.html).

The subsystem audits record verification coverage and remaining limits. The
implementation notes below retain the findings previously kept in the README;
they are not a claim of complete hardware or gameplay parity.

## Building

Requires CMake 3.20+, a C11 compiler and SDL3 for the playable client.
To fetch SDL3 when it is not installed:

```sh
cmake -S . -B build -DQ2PSX_FETCH_SDL3=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Without SDL3, only the core libraries and offline tools are built. Configure
with `-DQ2PSX_BUILD_CLIENT=OFF` when that is intentional.

Windows builds refresh `.install/`, including `USER-GUIDE.html`, the executable,
runtime DLL and launch scripts. The `stage-local` target can refresh it explicitly.

## Faithfulness

Visual fidelity is a hard requirement, not an afterthought. The PS1 look is not a
post-process filter here; it is a consequence of the pipeline being built the same way
the original was:

- **Fixed-point GTE.** Vertex transform runs through an arithmetic
  reimplementation of the PlayStation's Geometry Transformation Engine, including its
  1.3.12 / 1.19.12 fixed-point formats, its saturation flags, and its Newton-Raphson
  reciprocal table. Screen coordinates land on integer pixels with no subpixel
  precision — so the characteristic **vertex wobble** is inherent, not simulated.
- **Affine texture mapping.** UVs interpolate linearly in screen space with no
  perspective divide, producing the authentic **texture swim/warp** on large polygons.
- **Ordering-table sorting, no depth buffer.** Authored SortData orders the world;
  retail's area sorter merges model and effect chains. Depth sorting is a diagnostic fallback.
- **15-bit RGB555 output** with the PSX's ordered dither matrix, its four
  semi-transparency blend modes, texture pages, CLUTs, texture windows, and the mask bit.
- **Near-plane behaviour** matching the original's clipping (or lack of it).

The software backend always uses affine mapping and RGB555. Adaptive sizing,
bloom and CRT presentation are host enhancements; they are separate from retail
framebuffer conformance. Hardware bit-exactness has not yet been established.

See [`docs/FIDELITY.md`](FIDELITY.md) for the rendering conformance target and
[`docs/RENDERER-AUDIT.md`](RENDERER-AUDIT.md) for verified corrections and remaining gaps.

## Status

**The game finishes.** One run walks all five units — Strogg Outpost through Final Showdown, eleven
levels — takes the unit boundaries and their mission screens, reaches *"All missions complete. Return to
Command Ship."*, and ends on `Extro FMV` — which is the level table's own name for the map `QFMV` — playing
`OUTRO1P.STX`.

**And it starts before the menu does.** The boot chain is four screens and a film: `QLOGOS2` cross-fades
`Legal.lbm` into `HamLogo.lbm`, `QLOGOS` does the same with `IdLogo.lbm` and `ActLogo.lbm`, and each names
the next by writing the game-state word — 14 for the second pair, then **12, which is the request for
`Intro FMV`**. So `TAKE1BP.STX` is a PRE-MENU cinematic and the title screen is the fifth thing you see.
Confirming a difficulty then arms the front end's own opening reel, `ROGUEINP.STX`, half a second after the
title goes away, and that one hands over to the first level. All three films run to the stop point their
module passes the player rather than to the end of the file: the outro is cut at frame **1,500 of 1,559**,
so the last 2.4 seconds are on the disc and were never seen.

**And the format is fully read.** [`docs/openquestions.md`](openquestions.md) has **no open
questions**: 158 resolved, 18 partial where a residue is stated, and 3 marked terminal because this disc
cannot answer them — a file with N = 1, a field that is zero in every sample, and a standing security
caution. (The fourth used to be the NTSC build's values, which wanted a disc nobody here owned. There is one
now.) The last two to fall, `AreaConx`'s link payload and the model
force-draw mask, were the same mistake three entries apart: each stood on a premise nobody had tested — *this
address is a function*, *this record is an array of structs* — and every search that followed was sound,
exhaustive, and answering the wrong question.


A level loads from a real disc and renders textured, models included, with the
simulation running on top of it. What is missing is not any one format — it is
the wiring between systems, and the parts of the original's behaviour that have
not been read out of the executable yet.

| Area | State |
|---|---|
| Disc access | cue/bin, iso, bare images; ISO9660; CD-XA Form 1 and 2 |
| Build identification | by executable hash, not by region — both releases catalogued, and the USA executable read through its relocation to PAL's addresses |
| Level data | container, scene graph, geometry, collision, spawns, lights, triggers |
| Collision | the portal-walking hull trace, sliding, stepping and the entity sweep — transcribed from the executable, 47 of 47 maps walkable |
| Rendering | software rasteriser with the PSX's rules; world and models, textured |
| Surfaces | `Scene.flags08` fully decoded — object binding, hide flag, deferred path and the four draw variants; all four semi-transparency modes from the executable's own two tables; near-quad subdivision, which is what controls the affine texture warp |
| Draw order | `SortData` decoded — a self-describing bit stream, 178,801 node references across 715,260 bytes with none out of range |
| Lighting | the world's baked vertex lighting; the per-entity three-light gather through the GTE's own `NCT`; `SpaceLights` decoded — 37,285 index entries, none out of range; and the **lens flares, which are now drawn**. Their four element tables matched the executable's and the pass had no caller: `q2_world_zone` carries `lights` and `light_node` for exactly this and nothing ever assigned them, so the transcribed `0x800759F0` took the null branch every frame of every run. Reading the rest of it down to the last constant found four more things. The four "magic multiplies whose closed form was not recovered" are exact integer divides — a magic `M` with post-shift `p` is a divide by the unique `d` with `M = floor(2^(32+p)/d) + 1`, and all six of the flare's divides solve to 320 or 240 times 4096, 2500 or 3500, so the starburst's arms are `4096/2500` and `4096/3500` of the glow rather than the 1.637 and 1.170 they had been measured at. The disc is not the n-gon generator with n = 6: `0x80074C4C` is its own unrolled hexagon reading a single table entry. The GTE translation at `0x800DDD7C` was recorded as an untraced substitution, and it has no writer in the image, sits in BSS, and is handed to `T_Damage` as both `dir` and `point` by two of its three readers — a static zero vector, so zero was right for a reason. And the ring's mirror index is `out[n+1-k]`, not `out[n-k]`, which had been turning the 12-gon into an 11-gon with a doubled vertex; `tests/test_light.c` counts the distinct rim vertices now, because no screenshot can |
| Models | vertices, faces, texturing and animation — all 4,535 clips decode, and a creature's move now SELECTS its clip rather than being looked up as a position on one continuous timeline: a move's frame count times three is exactly some clip's length for 93 of the disc's 97 moves, and where lengths repeat the k-th move takes the k-th clip. That reproduces the correspondence already known from the other direction, and it is the difference between a body that falls over and one that stands back up halfway through dying. And they are now **backface-rejected**, which they never were: the model linker's own NCLIP pair, whose two comparisons turn out to be the world's exchanged — model quads are wound the opposite way round. With no depth buffer an emitted face paints over whatever the sort put behind it, so a model was drawing its own far side through its near side; a Soldier falls from 233 faces to 129, and a BASE1 frame with 22 creatures in it from 5,120 to 3,589 |
| Audio | sound bank and SPU-ADPCM decode, played through the device — the menu's five, the item table's eleven, the player's own footsteps, landing and four pain grunts, and every creature's from its own module's table, the slot index being the sound number (checked against the Soldier, whose numbering was read from its code). The Tank Commander's voice was recorded here as "cut, not lost in the port", and that was **wrong** — scanning the raw image for VAG headers finds **63 entries whose name begins `tnk_`**, five of the six the module asks for, in twelve or thirteen banks each; only `tnk_idle1` genuinely has no VAG, and `q2psx-inspect export … sounds COMMAND` lists `tnk_atck1`, `tnk_death`, `tnk_pain`, `tnk_sight1` and `tnk_step` in that map's own bank. The original claim rested on `q2psx-inspect audio | grep tnk_`, which prints aggregate statistics and no names at all, so the search could not have found anything either way; and the music, which is now the map's own — a level record carries a seven-track playlist and a track id names one of the twenty XA streams through the executable's own table, whose stated durations 19 of 20 streams match to the tenth of a second |
| Player movement | Retail-derived clamped acceleration, wish rotation, posted jump impulse, ground projection, fall damage, rate-based look, swimming and ladders. Separate corpse and down-body physics now follow the death think handlers, including all-axis braking and stage-dependent solidity. See the [movement audit](MOVEMENT-AUDIT.md) for verified paths, regression coverage and limits. |
| Pad and view | the pad read at `0x80019154`: nine control styles, and the shared tail that turns four configurable pad masks into eleven derived bits — including the one that makes **jump and swim-up the same button**, a tap for one and a hold for the other. Full digital deflection is 127, not 128. And the view is not the aim: `0x80038260` composes three independently decaying kicks — firing over 30 ticks, damage over 150, landing over 90 — plus the strafe lean, and only then is it a camera |
| Environment volumes | crouching, swimming and the no-jump zones are authored per map, not by the pad: they are `UserFuncs` primitives a trigger volume calls. Resolved across the whole disc — 37 `INCROUCH`, 21 `UNDERWATER`, 13 `INWATER`, 52 `DONTJUMP`, and exactly **2** `INLOWCROUCH`, both on `SECURITY`. `DONTJUMP` is what sets the jump gate this project had recorded as never set |
| Level scripting | the trigger graph parses and executes, and a `CALL` now reaches its primitive: rotating brushes turn when a script asks. In the client, playing: walk LAB with the demo pad and a rotator that was at zero is standing turned. Firing every trigger volume on the disc — a player who has walked every map — 26 rotators built, 552 `CALL`s run, 17 rotation steps, 13 turned. What a rotation call's operands mean differs per primitive and is read in one place, beside the builder. The interpreter walks COMMON's script while constructors read object slots from the zone's same-offset `Events` copy (`gp+372`/`gp+376`); that split is what preserves each zone's local Scene-node bindings. Rotating constructors also derive `obj+0x18` from the raw Scene-box midpoint, with ROTHATCH's authored hinge adjustment, so a turning brush stays on its retail pivot instead of orbiting local zero |
| Level flow | **the game plays through**: `LOADMAP` was decoded and nothing acted on it, so a session ended on the map it booted into. All 28 calls on the disc are reachable by a trigger volume and all 28 resolve — and 12 land past zone 0, so the destination zone is the named arrival point's rather than zero. One run now walks Strogg Outpost to Boss2, eleven levels and five units, with the MISSION screen at every boundary, the briefing on arrival, and the inventory carried across — by BOSS1 a weapon picked up two levels earlier is still in hand. The MISSION screen's own counters are the level's: secrets are `INSECRET` calls counted once each, kills come from the creature world, and the unit and the level's display name were sitting unread in each map's `Strings` chunk |
| Level scripting, part two | what a trigger volume ASKS FOR, measured rather than guessed — a histogram of every UserFuncs primitive a player walking the whole disc would run. It found five finished subsystems with no caller. **Doors and lifts**: `q2_movers_build`, `q2_movers_tick` and `q2_mover_trigger` were all dead, so 1,006 `MOVER_A` items stood still and a closed door was a wall — at one LAB frame the before and after differ by 94,068 pixels of 126,976, because before it the player is inside the door looking at its back face. **Scripted ambushes**: 58 of 89 resolvable `CREBATCH` calls name a group claiming no zone — `ShotgunRoom`, `BerserkHide` — and every one of them was standing in the room from level load. **The key gate**: `ONKEYDO` aborts the record it guards and nothing honoured it, so JAIL2 opened five doors without the key. **What the game says**: `STRING`, `SIMPLESOUND` and `HELPCOMPUTER` — *"Locate the Repair Facility and steal the Commander's head you find there."* — none of which had ever appeared |
| Movies | the three `.STX` films, decoded and playing. **5,301 of 5,301 frames** across all three, zero unmatched codes and zero desynchronisations, and every frame satisfies its own header — `bs_num_codes` turns out to be the MDEC's DMA length, `round_up_32(ceil((2*blocks + pairs)/2))`, which is a per-frame check needing no reference and is what settled a Huffman table that four earlier passes had each diagnosed differently. The AC codes are MPEG-1's Table B.14, the macroblocks are column-major, and the escape's 6/10 split is measured rather than assumed: eighteen layouts, two decode everything, and the picture separates them. `--movie NAME` plays one, and the campaign now ends on the outro instead of a placard explaining that it cannot |
| Cinematics, wired | a decoder is not a cinematic. **`QFMV` is the map that IS one**: the level table's records 10 and 11 are `Intro FMV` and `Extro FMV` and both resolve to that one 45 KB directory, so the DISPLAY NAME is what picks the film — which is exactly how the module picks it, comparing the name it was entered under against its own table before calling the player. So the campaign's last `MISCOMPLETE` goes to `Extro FMV` the way `0x80018ED8` goes there instead of to `EndMission 5`, and the intro is what the BOOT CHAIN ends on — `QLOGOS` writes game state 12 through `engine+0x3AC`, which is the request for `Intro FMV`, before the front end has ever been loaded. **And the third film is not the attract reel this project called it for a year**: `ROGUEINP.STX` is named at `module+0xDC4` and played by a countdown that `0x80101E4C` arms *in a delay slot* — from the EASY, MEDIUM and HARD records, half a second after the difficulty is confirmed. What the title screen's own thirty-second idle reaches is `0x80101B08`, which plays no film at all: it puts up `DEMO OF GAME` and hands off. The call site the disassembly notes had recorded, `0x8010D5F8`, has zero references in the whole module. **And the player takes a length**: `play(name, frames, ...)` compares its second argument against the STR header's `frame_number`, so the console plays 1,280 of the intro, 1,499 of the outro and 2,456 of the reel — never the whole file |
| Cinematics, written | **the encoder** (`src/formats/stxenc.[ch]`), because a format is only read when you can write it back. Strict inverse of the decoder against its own tables: forward DCT with the decoder's cosine matrix, `round(F * 8 / (qscale * quant))`, zigzag, B.14, 16-bit LE words MSB first, Cr/Cb/four-luma in column-major macroblocks. The 6,5,5,5 cadence turns out to be the encoder's CONSTRAINT and not its decoration — 21 video sectors per 4 frames is what is left after the audio takes its three — so the quantiser is chosen to fit the budget, which is the rate control the original must have had. XA ADPCM for the soundtrack and real Mode 2 sectors underneath it: **all 7,712 of the disc's own sectors of TAKE1BP.STX come back byte-identical** from the EDC and Reed-Solomon builder. Re-encoding 400 frames of ROGUEINP gives 400 of 400 decoding exactly, `bs_num_codes` agreeing on 400 of 400, no frame over its budget, **45.4 dB mean PSNR**, and audio that round-trips sample-for-sample |
| Hazards | lava, acid and the laser walls — 89 damage volumes across the disc that had no caller, so a player could stand in lava indefinitely. Read out of the five primitives' own handlers (1 point at mod 9, 20 at mod 10) and applied level-triggered, which the per-target throttle forces: firing once on entry would make lava a single point of damage. Standing in BOSS1's lava for three seconds of level clock lands 9 hits and takes 100 health to -80, which is what the 100-tick throttle predicts. Finding that also found that the throttle had never worked: the hurt-actor refresh cleared the deadline it had just armed, along with the invulnerability and protection clocks |
| Breakables | glass breaks when you shoot it. The route from a shot to a pane was the last thing openquestions called "still owed", and it is not the collision node it was guessed to be: it is a 48-entry registry of BOXES that the shot trace sweeps separately from the world hull, whose index the trace reports and whose record pointer is the key the object router matches. Both the hitscan and the projectile paths route, because a bolt is one of the console's five call sites and a blaster is what you have in hand at the first window. LAB's pane throws 11 pieces — 1 from the hit and 10 from the shatter, which is what that item's own two operands say. `SHOOTTHEN` shares the registry and answers differently: shoot the panel and it RUNS THE RECORD it belongs to, which is a shoot-to-activate switch and was previously a primitive with no non-scripted caller |
| Script residue | `DISABLEME` retires the record it runs in — the DISABLED bit the dispatcher tests before running anything. Two calls on the disc: one sits in a record nothing runs, and the other is behind `ONKEYDO`, which is why it had never been seen fire. `--keys` opens the gate for a scripted run and BIGGUN goes from 0 records retired to 1 |
| Saves | and what a save now restores: the level clock, the script's flags, trigger residency, the items collected — and, new, **which doors are open and where in their travel, which windows are broken, and who is dead**. All three were live state nothing carried, and the doors were the worst of them: the script flags ARE saved, so a reload shut every door the player had opened and the record that opened it would not run again. Three additive chunks, no version bump, which is what the chunked format was built for |
| Credits and completion | QFRONT's 177 authored rows use the native scroll, menu font and clipping. VIEW CREDITS returns to Options. Finishing the campaign plays the outro and credits, then awards Bronze/Silver/Gold by difficulty and offers to save a MULTI UNLOCK file. Reload it through MULTIPLAYER → LOAD SETTINGS. Gold's ALL WEAPONS grants the map's available weapons and full-capacity ammo on multiplayer resume/start/respawn. See [functional audit](FUNCTIONAL-AUDIT.md). |
| Monsters | All **22 native modules** have behavior implementations, including Jorg, Rider and Medic. The shared actor path reconstructs physical movement, pending impulses, position/yaw interpolation, animation transitions and activity suspension from the PS1 executable. Native saves and zone rebuilds preserve this state. See [the creature audit](CREATURE-AUDIT.md) for instruction-comparison tests and remaining limits. |
| Simulation | inventory, combat, creature AI, save games |
| Zone persistence | Returning through a gate retains collected pickups, activated ambushes, creature AI/death animation, dropped-item motion, gibs and projectiles with their ownership and remaining lifetimes. Stable identities prevent overlap duplication. Native saves retain visited-zone runtime; older population-only saves remain readable. Failed archive loads preserve the running world, and same-map gates retain simulation and animation clocks. See the [zone handling audit](ZONE-HANDLING.md) for evidence, tests and scope. |
| Multiplayer | One shared arena supports one to four local players, with horizontal or vertical two-player splits and a quad layout for three or four. Each player has independent input, movement, pickups, damage, HUD, view weapon, camera and respawn state. Keyboard/mouse plus three SDL gamepads is the default; `--gamepad-player-one` allows four gamepads. The front end's PROCEED opens independent name, colour/team and controller setup panels and waits for every player to be READY. Settings cards preserve these choices. Deathmatch, Team Deathmatch and Versus use reconstructed QMULTI scoring and limits. Versus reloads rounds while preserving wins; the retail-derived results screen waits for every player's fire press and returns to the MULTIPLAYER menu. The three cut flag/tag modes remain marked cut. See the [multiplayer audit](MULTIPLAYER-AUDIT.md) for evidence and limits. |
| Entities | Shared population selection, deferred CREBATCH activation and checked pickup placement; native bridge, computer, pump, stair, laser, orb and platform mission behavior, SECURITY boss progression, POWER1's shaft hazard, POWER2's reactor sequence and BOSS2's train unlock with save restoration. See the [entity audit](ENTITY-AUDIT.md) for retail evidence, tests and remaining coverage limits. |
| Items | every one of the 64 table records — what it looks like, how it behaves, what collecting it does. 1,013 of 1,013 placed items resolve |
| Weapons | all eleven read out of their own fire functions — damage, spread, kick, refire |
| Damage | armour, power armour, knockback, splash, 21 means of death — and what `T_Damage` (0x800627F8) itself does: the surprise bonus that doubles the first shot on a creature which has not noticed you, FL_NO_KNOCKBACK, FL_GODMODE, and the -9999 floor a corpse cannot be driven below. It does **not** post damage to a creature's module, which this project had recorded that it does; the subtraction is in T_Damage and the call that claim rested on passes a different entity |
| Death | a killed creature used to freeze in the pose the shot caught it in — drawn, but never ticked again. It now falls: the module's own death move is found by name (all seven carry one) and installed, and a corpse runs the frame driver without running the AI |
| Creatures | 651 population records resolve to class/model data. The runtime loads native AI from COMMON and the active zone, preserving local module variants before borrowing COMMON donors. Implemented weapons use posed muzzle positions, traced bullets/rails and simulated bolts, rockets and grenades; friendly fire identifies its shooter. Native roster regressions cover attacks and deaths at all four difficulties, including BASE2 and MAGDEMO Flyers. |
| Menus | every page, its navigation and its settings — 21 of 21 checked against the executable — **and the front end**: the client boots into the title screen, START and OPTIONS over the QFRONT scene, with SINGLE PLAYER / MULTI PLAYER and PLAYER / SOUND / VIDEO OPTIONS / VIEW CREDITS below them. Those pages are not in the executable at all — they are a static record array inside QFRONT's own `LevelBin`, found by asking what points at each string — drawn with the console's own three faces, its gouraud selection bar and its line-drawn sliders |
| Memory card | all nine front-end screens and the release-gated state machine behind them — 31 of 31 items checked against the executable; the card I/O itself is a host interface, not an invention |
| Saved games | four slots reached through that front end, plus quick save. The container is an ordinary host file — a stated divergence, since a save file's container is invisible where the rendering's limits are the point — but what it holds is the original's state: the level clock every powerup deadline is measured against, the script's event flags, which trigger volumes the player is standing in, which items have been collected, the mover's carried cell and frame delta, and the weapon generator. Chunked, checksummed and fixed-width, so a save written by one build loads under another |
| View weapon | the model in the player's hands: its animation bank, four-state machine and view-space transform — 20 of 20 checks against the executable |
| Screen | display envs, double buffering, the sliced ordering table, all five viewport layouts, the frame lock, and the whole per-viewport draw — clip, clear, damage flash, world gate and the performance meter — 174 of 174 checks against the executable |
| Screen effects | the damage flash, raised from the player's own health and armour and drawn as the viewport's own tile; and the **water effect** — a framebuffer warp that displaces columns and rows of the drawn frame by a travelling cosine, under a blue tint that ramps as you go under. It is what writes the screen shake, which had sat in the view record with no writer at all: the inset is the margin the warp copies from |

| HUD | the overlay — notifications, centre line, crosshair, damage flash — its markup language and its own font; the MISSION screen, whose format strings turn out to be markup the game assembles at run time; and the **status bar**, which this project once proved did not exist. It does: health, ammo and armour in 24x24 numerals beside their icons, drawn by the per-viewport hook at `0x800337D0` and anchored to the viewport's own `view+304`. The earlier negative result came of enumerating format strings, and the bar draws sprites. And what you just picked up is on it: the bar's fourth sub-draw (`0x800359C0`) reads the two fields the touch dispatch has always written and turns them into an icon in the upper-left field and the caption beside it — `Super Shotgun`, not `Sshotgun P`, out of the 57-name table at `0x800AC144` that the same effect id indexes. **The effect id and the icon rect index are one number**, which is the join §11.1 asserted and `icontable.h` retracted; both were half right, and this sub-draw is where the mechanism is written down |
| Effects | all five machines, nothing modelled: the fifteen-quad particle groups and their nineteen colour ramps, the one-frame beam pool and its folded hexagonal hull, the debris burst with the hull it slides in and the mover's own gravity and terminal velocity, and the `GlintMod` glint's two draw paths — which the port turns on by READING the level script that raises them, no interpreter needed. Spawned by the simulation, drawn into the same ordering table as the world. The weapon trail is the BFG's: a persistent green beam held on every target the ball can see, refreshed each tick and lingering 45 units after it passes |

Checked against the PAL disc, and every executable check against the USA disc as well: 164 level files, 461,852 vertices, 274,936 quads,
139,240 collision planes, 94,642 collision portals, 1,723 models, 2,036,080
animation keys, 2,475 sounds, zero failures. The remaining gaps are tracked in
[`docs/openquestions.md`](openquestions.md).

Audio findings, corrected defects and the remaining retail parity work are in
[`docs/AUDIO-AUDIT.md`](AUDIO-AUDIT.md).

## Running the client without a player

Everything the frame does — the tick, the viewport build, the world draw, the
ordering table, the rasteriser, the HUD — happens before a single SDL call, so the
client can run with no window at all, on a fixed 1/30 s step, driving the pad from a
script and writing the console's own framebuffer out:

```bash
build-client/bin/q2psx --disc "Quake II (Europe).cue" --headless --demo \
                       --frames 120 --shot run.ppm --shot-every 10
build-client/bin/q2psx --disc "Quake II (Europe).cue" --movie OUTRO1P.STX
```

Split-screen captures use `--dm --dm-players 2` with `--dm-split horizontal` or
`--dm-split vertical`. Three and four players use the quad layout. For a repeatable
HUD and view-weapon check, including all eleven weapons:

```bash
python tests/check_split_screen.py --client build-client/bin/q2psx \
    --disc "Quake II (Europe).cue" --output .agents/tmp/split-screen/PAL --weapon-sweep
```

The check writes engine framebuffer captures and logs under the chosen output
directory. It checks crosshair pixels at each viewport centre and damage flashes
on the injured player. `--baseline /path/to/previous/q2psx` also compares
single-player pixels. The client accepts `--crosshair` and `--no-crosshair` to
override that menu setting during a capture.

Multiplayer parity checks cover all thirteen arenas and complete Deathmatch,
Team Deathmatch and Versus matches. The disc-backed client test also checks
the actual results transition, font/art residency and rendered pixels:

```powershell
python tests/check_split_support.py --client .install/q2psx.exe --disc .install/disc/game.cue --output .agents/tmp/multiplayer-parity/arenas-and-matches
build-msvc/bin/Release/test_client_split.exe .install/disc/game.cue .agents/tmp/multiplayer-parity/results.ppm
```

Run the Python check first to create the output directory. See
[the multiplayer audit](MULTIPLAYER-AUDIT.md) for the retail evidence and
remaining controller-interface limitations.

Save restoration also has a disc-backed regression:

```powershell
build-msvc/bin/Release/test_client_save.exe --headless .install/disc/game.cue .agents/tmp/save-restore
```

Create the output directory first. The check saves before and during a crate
explosion, restores in the same session and a fresh client, then saves on another
map and revisits the destroyed crate. It writes the engine's framebuffer captures
and checks restored geometry, hitboxes and visited-map script history. New saves
carry that history and shared explosive-group health; loading an older save never
borrows progress from the session being replaced. Transient explosion and gib
models are omitted so a save made during their animation remains loadable.

The same regression restores a hatch halfway through its rotation and a hunting
creature from a save made at a different difficulty. Saves now retain rotator
timers, creature animation and target links, corpse state, the AI clock and
pursuit trail. Difficulty is restored before spawning the saved map's roster.
The shared gameplay random stream uses the BIOS generator and is saved in full,
so a loaded creature makes the same subsequent random attack choices. The added
chunks remain optional when reading older files.

Live weapon state also survives a save: recoil, chaingun spin, a held grenade,
and a pending weapon switch resume at the same animation position. The regression
compares the saved and restored framebuffer, then checks the next 45 frames of
animation, ammunition, projectiles, shot reports and random choices. Bolt effect
flags and the BFG beam-damage accumulator are retained too. Active script timers
retain their remaining delay, repeat count and item window; WAIT counters and
queued triggers are retained too. Timer periods reroll on every rearm, including
range-zero timers, while duplicate requests and full tables consume no random
draw. The disc test also resumes BOSS2's repeating light timer after a cold load.
Deadlines fire only after the countdown passes zero, as in the original sweep.
Older files without these chunks retain the previous reconstruction behavior.

Scene collision also covers shoot switches, script-hidden objects, all intact
explosive parts (including groups destroyed only by scripts), and rotating
hatches. Hatches lift their collision slab by 10,000 units while open, exactly
as the original handler does; closing restores it. Destruction frees the box
for movement, shots, creature sight and ground checks. The disc regression
captures movement into an intact crate and checks restored hatch collision.
It also checks all seven script-hidden objects across four maps, including
SECURITY's repeatable script: hidden geometry and freed collision boxes survive
both a cold load and saving elsewhere before returning.

Single-player accepts the first connected gamepad automatically, including its
analog controller settings; without one it uses keyboard and mouse. In multiplayer,
the keyboard and mouse control player one; connected SDL gamepads
fill players two through four in connection order. Disconnecting a controller
releases its input without moving the other controllers to different players.
Use `--gamepad-player-one` to assign up to four gamepads starting with player one.
Additional players use the retail RIGHT STICK scheme: D-pad (or left stick) moves,
right stick looks, left shoulder fires, left trigger jumps, and right shoulder /
right trigger cycle weapons. South selects in menus, East goes back (North remains
a retail-compatible alias), and Start opens or closes the pause menu. The controller page selects
player one's analog scheme when a gamepad is assigned there.

Each player respawns with their own fresh fire press. Versus disables respawning,
preserves wins across arena reloads and ends at the round limit (`--dm-rounds N`
for captures). Results freeze the arena and wait for every player to press fire.
The short automatic respawn used by `--demo --dm-stage` is a capture script.

On Windows, client builds also refresh `.install/` with `q2psx.exe`, SDL3 and
`Play.cmd`. To refresh it explicitly from the existing MSVC build:

```powershell
cmake -S . -B build-msvc -DCMAKE_BUILD_TYPE=Release
cmake --build build-msvc --config Release --target stage-local --parallel
.\.install\q2psx.exe --version
```

Run `.\.install\q2psx.exe` with no arguments and it opens the disc selector: the
disc you played last is selected, any image in `.install/disc/` or beside the
executable is listed, every optical drive is listed with what is in it, Browse opens
an in-window folder browser, and an image dropped on the window joins the list. The
choice is remembered in `%APPDATA%\Q2PSX-PC\launcher.cfg`.

The selector uses the game's original menu letterforms, a blue selection band
and dark metal colours. The disc font's non-square pixel aspect is corrected for
the host display. It hands the **same window and OpenGL context** to the
game, preserving its size and position. The font is read from a detected disc
and cached beside `launcher.cfg` after a successful selection; a built-in font
keeps first-run selection usable before any disc has been found.

Both front ends use bordered keycaps, with PlayStation, Xbox, Nintendo or generic
button labels whenever a controller is connected, and keyboard labels otherwise.
D-pad and left stick navigate; short taps survive between rendered frames.
The selector supports held navigation, shoulder-button paging, South to open/start,
West to browse or go up a folder, East to cancel/quit and North to refresh. Browsing
includes the computer's drives and needs no desktop dialog or keyboard. In the
main menu, West opens Rules; the keyboard equivalent is R.

Double-click `q2psx.exe` or `.install/Play.vbs` for a launch without a command
window. `--developer` attaches to an existing console or opens one and enables
debug logging. Normal desktop launches write diagnostics to
`%APPDATA%\Q2PSX-PC\q2psx.log`; command-line output and redirection still work for
`--version`, `--help`, `--headless` and `--report`.

`--launcher` opens the selector even when `--disc` is supplied. Its screenshot
path is `--launcher --headless --shot selector.bmp`, with optional `--size WxH`
and `--disc`. This renders directly to an engine-owned surface without a window
or input devices. `Q2PSX_LAUNCHER_CFG` selects an isolated test configuration.

Launch with `.\.install\q2psx.exe --disc "path\to\your\disc.cue"`, or put a copy
of your CUE at `.install/disc/game.cue` with its referenced BIN tracks alongside
it and use the legacy `.install/Play.cmd`. This compatibility script starts the
game and exits; use the executable or `Play.vbs` to avoid the shell entirely.
Both scripts forward additional command-line options; `Play.cmd` accepts
`--disc` to override its default. Builds preserve disc
data, saves and configuration in `.install/`; the directory is ignored by Git.

`tests/check_split_support.py` accepts the same `--client`, `--disc` and `--output`
arguments. It checks four live spawns on all thirteen arenas and complete
Deathmatch, Team Deathmatch and Versus matches. CTest's `split_client` additionally
tests actual client glue, simulated SDL controllers, brief input taps at
60/144/240 Hz, shared pickups, damage, respawning, teleports, lighting ownership,
body retirement and results readiness without
opening a window or reading the keyboard/mouse.

The cinematics are reachable the way the game reaches them. A windowed run walks the
BOOT CHAIN — the two logo screens and then `TAKE1BP.STX` — before the menu, and any key
or click ends the screen that is up. A headless one goes straight to the title, because
ten seconds of logos and fifty-one of film in front of every scripted front-end capture
is 1,850 frames nobody asked for; `--boot` and `--no-boot` force it either way. `--new-game`
confirms a difficulty, which is what the opening reel hangs off: half a second of blank
front end, then `ROGUEINP.STX`, then the first level. `--map` takes a level table DISPLAY
name as well as a directory, which is the only way to ask for one of the two FMV screens,
since `Intro FMV` and `Extro FMV` are both the directory `QFMV` and the name is what
chooses the film. `--movie` plays one on its own.

```bash
build-client/bin/q2psx --disc "Quake II (Europe).cue" --boot --headless --frames 560
build-client/bin/q2psx --disc "Quake II (Europe).cue" --new-game
build-client/bin/q2psx --disc "Quake II (Europe).cue" --map "Extro FMV"
build-client/bin/q2psx --disc "Quake II (Europe).cue" --movie ROGUEINP.STX
```

`--watch` adds a camera that stands in front of the nearest live creature and looks
at it — the inspector's `mob` framing, but of a creature that has thought, turned and
is playing whatever move its AI put it in. It exists because a face count says nothing
about what you can see: with an ordering table and no depth buffer, a monster behind a
wall is emitted and then painted over.

`--watch-hold N` also enables that camera and holds a killed creature in view
for N more frames, so a headless capture can show its drops and gibs before
the camera moves to the next live creature.

That is not a convenience. `q2psx-inspect` composes its own frames, so it cannot
catch anything that goes wrong *between* the client's systems — a table loaded after
the thing that reads it, a model never bound, a screen never fed. The first run of
this found two: the overlay was initialised from a flag that had not been set yet, so
the client had never drawn a notification or a crosshair; and a session booted into
the free-fly debug camera, so none of the player's frame ran until you pressed a key
nothing told you about.

## Tools

`q2psx-inspect` is the reverse-engineering harness — it opens a disc image and dumps
the filesystem, build fingerprint, and asset structure without needing a game window.
Every format claim in [`docs/FORMATS.md`](FORMATS.md) has a corresponding check
here, so "we understand this format" is something the build can evaluate rather than
an assertion in a document.

```bash
build/bin/q2psx-inspect ident  "Quake II (Europe).cue"
build/bin/q2psx-inspect verify "Quake II (Europe).cue"
build/bin/q2psx-inspect coll   "Quake II (Europe).cue"             # every hull, checked
build/bin/q2psx-inspect walk   "Quake II (Europe).cue" BASE1 0 150 # drop a player in
build/bin/q2psx-inspect pmove  "Quake II (Europe).cue"             # styles, jump, view, volumes
build/bin/q2psx-inspect pmove  "Quake II (Europe).cue" SECURITY 0  # and one map, frame by frame
build/bin/q2psx-inspect audio  "Quake II (Europe).cue"
build/bin/q2psx-inspect render "Quake II (Europe).cue" BASE0 0 out.ppm 0 1024
build/bin/q2psx-inspect model  "Quake II (Europe).cue" BASE1 Soldier 0 0 out.ppm

build/bin/q2psx-inspect hud    "Quake II (Europe).cue" BASE0 hud.ppm
build/bin/q2psx-inspect weapons "Quake II (Europe).cue"            # every weapon, checked
build/bin/q2psx-inspect effects "Quake II (Europe).cue"            # every ramp, beam and laser, checked
build/bin/q2psx-inspect lights  "Quake II (Europe).cue"            # every light, its cell and its flare
build/bin/q2psx-inspect lit     "Quake II (Europe).cue" BASE1      # what lights the player where they start
build/bin/q2psx-inspect items   "Quake II (Europe).cue"            # every item, checked
build/bin/q2psx-inspect menu   "Quake II (Europe).cue"             # every page, checked
build/bin/q2psx-inspect menu   "Quake II (Europe).cue" 26 pause.ppm BASE1  # real font
build/bin/q2psx-inspect text   "Quake II (Europe).cue"             # every map's text, checked
build/bin/q2psx-inspect text   "Quake II (Europe).cue" SECURITY    # and the briefing it would show
build/bin/q2psx-inspect screen "Quake II (Europe).cue"             # every constant, checked
build/bin/q2psx-inspect viewweapon "Quake II (Europe).cue"         # the weapon in hand, checked
build/bin/q2psx-inspect viewweapon "Quake II (Europe).cue" 10 rail.ppm COMMAND 0
build/bin/q2psx-inspect screen "Quake II (Europe).cue" split.ppm quad BASE1 0
build/bin/q2psx-inspect screen "Quake II (Europe).cue" wet.ppm one+water BASE1 0   # underwater
build/bin/q2psx-inspect screen "Quake II (Europe).cue" hit.ppm one+flash BASE1 0   # damage flash
build/bin/q2psx-inspect surfaces "Quake II (Europe).cue"           # flags, blends, draw order
build/bin/q2psx-inspect movie  "Quake II (Europe).cue"            # every film, every frame, checked
build/bin/q2psx-inspect movie  "Quake II (Europe).cue" OUTRO1P.STX out.ppm 200
build/bin/q2psx-inspect movie  "Quake II (Europe).cue" sweep      # score the escape's field widths
build/bin/q2psx-inspect movie  "Quake II (Europe).cue" encode ROGUEINP.STX out.stx 400

build/bin/q2psx-inspect syms   "Quake II (USA).cue"             # the linker's own symbol table
build/bin/q2psx-inspect syms   "Quake II (USA).cue" T_Damage    # -> 0x800627F8, G_COMBAT.C
```

`movie encode` is the round trip: it decodes a film off the disc, re-encodes it into a
new `.STX` — real Mode 2 sectors, XA audio in slot 7, the 6,5,5,5 cadence — and then
reads what it wrote back with the same decoder, reporting whether every frame decodes
exactly, whether each satisfies its own `bs_num_codes`, whether any frame overran its
sector budget, and how far the picture and the sound moved. It checks the sector builder
against the disc's own sectors first, so "0 sectors failing their EDC or parity" is a
statement about a 1997 master and not about the tool agreeing with itself.

`render` needs no window — it writes a PPM. That is how the geometry pipeline was
brought up before the client existed, and it remains the quickest way to check a
change end to end.

It also reads the *code*. The remaining unknowns are questions about the original
executable, so the tool carries a PS-X EXE loader and an R3000A disassembler and
answers them from the disc, with no external disassembler in the loop:

```bash
build/bin/q2psx-inspect exe    "Quake II (Europe).cue"              # map + landmarks
build/bin/q2psx-inspect disasm "Quake II (Europe).cue" 0x80076378   # to the return
build/bin/q2psx-inspect xrefs  "Quake II (Europe).cue" 0x80068A58   # calls, constants, tables
build/bin/q2psx-inspect modstrings "Quake II (Europe).cue" QFRONT   # a level module's own text
```

`exe` re-checks nine addresses that `docs/FORMATS.md` makes claims about, so a
documentation drift against the real executable fails the command.

For the few functions where control flow matters more than field access,
`tools/ghidra/decompile.py` is an optional headless script that decompiles
addresses out of the same segment. Ghidra is not a dependency of anything here;
`q2psx-inspect exe <disc> text.bin` writes the segment with its header stripped
so a raw import at `0x80018000` lines up with every address in the docs.

## Display aspect ratios

The default `--aspect auto` follows the window's shape, including portrait,
square, widescreen and ultrawide displays. The camera preserves its original
vertical field of view: wide displays reveal more world horizontally, while
portrait displays narrow the horizontal view without extreme vertical distortion.
Each split-screen viewport adapts independently. Menus, dialogs, scores and
mission panels fit a centered area without stretching; HUD elements remain
inside their player's view, and the weapon stays anchored near its bottom.
Movies and boot artwork retain their original proportions.

Resize the window freely, or use `--size 2560x1080` to choose its initial size.
For repeatable captures use `--headless --size 2560x1080 --frames 60 --shot out.ppm`.
The raw render target retains console-shaped pixels; `--shot-display display.ppm`
writes the aspect-correct presentation. The software target stays within the
original projection hardware's coordinate limits, so very extreme shapes may
have borders. `--aspect 4:3`, `tv`, `square` and `stretch` remain available;
press **V** to cycle modes. Only `auto` adjusts the camera to the display.
Menus and other front-end screens override the raw `square` / `stretch` debug
fits with `auto`, so their proportions and pointer targets remain correct.

## CRT display

**Options → Video Options → CRT Emulation** enables a curved CRT display with
source-aligned scanlines, an RGB phosphor grille, linear-light beam filtering
and subtle optical glow. It covers the game, HUD and menus, and is **off by
default**. Video's **Reset to Defaults** switches it off again. The setting is
stored by the existing game/settings save system; older saves load with CRT off.

The display pass runs on the GPU through OpenGL, with the same beam profiles,
phosphor mask and colour conversion as the CPU reference. Only the small native
RGB555 framebuffer is uploaded each frame; CRT filtering and output scaling
stay on the GPU. Machines without the required OpenGL support use the CPU fallback.

`--crt` and `--no-crt` select it at launch; `--crt-cpu` selects the CPU reference
for comparisons or troubleshooting. F11 always preserves display pixel aspect
and includes the effect when enabled;
`--shot` always keeps the untouched console framebuffer for fidelity checks.
For a reproducible picture of the fitted display, use
`--headless --frames 60 --scale 2 --crt --shot-display crt.ppm` with your disc
and map arguments. Display captures omit the window borders and screen-position
offset. GPU captures use a hidden context and read the engine's output target.
CRT works with every aspect mode and split-screen layout. No CRT filtering runs
while the option is off.

The look was inspired by [openQ4's optional CRT display](https://github.com/themuffinator/openQ4/blob/master/content/baseoq4/pak0/glprogs/crt.fs).
This is an original GPLv2 implementation; no GPLv3 shader code was copied.

## Bloom

**Options → Video Options → Bloom** adds a soft glow to bright scene surfaces
and lights. It is **off by default**, saved with the other video settings, and
disabled by **Reset to Defaults**. Older saves also load with bloom off.

Bloom runs on the GPU in linear light, with a soft highlight threshold and two
blur radii. HUD numbers, icons, crosshairs, messages and menu text neither emit
bloom nor receive it. Scene colour is retained underneath the UI during the
ordinary raster pass; transparent glyph texels leave the scene visible. Each
split-screen viewport filters independently. When CRT is also enabled, it
processes the finished scene/UI composite as before.

Use `--bloom` / `--no-bloom` to override the saved setting at launch. F11 and
`--shot-display` include bloom; `--shot` remains the unfiltered RGB555 frame.
For deterministic captures, add `--headless --frames 60 --scale 2`.
Bloom requires the OpenGL display backend; the CPU fallback and `--crt-cpu`
display without bloom. Loading screens, boot images and movies are unaffected.
No scene capture or bloom filtering runs while the option is off.

## Layout

```
src/common/     shared types, fixed-point math, memory, logging
src/disc/       CD image + physical drive access, ISO9660, CD-XA
src/build/      per-build (region/revision) identification and data tables
src/formats/    on-disc asset parsers (.DAT container, zones, sound banks)
src/psx/        GTE and GPU primitive model — the fidelity core
src/render/     rasterizer backends that consume PSX primitives
src/menu/       the menu pages and engine, read out of the executable
src/screen/     display environments, viewports, the ordering table's shape
src/audio/      SPU-ADPCM, XA-ADPCM, CD-DA
src/game/       reimplemented game logic
src/client/     playable client, launcher and SDL3 host integration
tools/          offline inspection and extraction utilities
docs/           format specs and architecture
```
